#include "ImageCopyFrameSource.h"
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <optional>
#include <poll.h>
#include <sys/mman.h>
#include <unistd.h>
#include <fmt/format.h>
#include <wayland-client.h>
#include "DuplicateFrameDetector.h"
#include "Logger.h"
#include "ScalingGeometry.h"
#include "VulkanDevice.h"
#include "ext-image-capture-source-v1-client-protocol.h"
#include "ext-image-copy-capture-v1-client-protocol.h"

using namespace std::chrono;

namespace Magpie {

// Largest cursor image that is taken
static constexpr int32_t MAX_CURSOR_SIZE = 256;
// A capture of the output that fails this often in a row is given up
static constexpr uint32_t MAX_FAILURES = 50;
// After a failure the next try waits this long, unless the compositor says
// before that what buffers it wants now
static constexpr milliseconds RETRY_DELAY{ 100 };

bool ImageCopyFrameSource::GetPixelLayout(uint32_t shmFormat, PixelLayout& layout) noexcept {
	// The names give the order from the highest byte of a little-endian
	// number, so the bytes lie in memory the other way round
	switch (shmFormat) {
	case WL_SHM_FORMAT_XRGB8888:
		layout = { 4, false, false };
		return true;
	case WL_SHM_FORMAT_ARGB8888:
		layout = { 4, false, true };
		return true;
	case WL_SHM_FORMAT_XBGR8888:
		layout = { 4, true, false };
		return true;
	case WL_SHM_FORMAT_ABGR8888:
		layout = { 4, true, true };
		return true;
	case WL_SHM_FORMAT_RGB888:
		layout = { 3, false, false };
		return true;
	case WL_SHM_FORMAT_BGR888:
		layout = { 3, true, false };
		return true;
	default:
		return false;
	}
}

bool ImageCopyFrameSource::ChooseFormat(const std::vector<uint32_t>& offered, uint32_t& chosen) noexcept {
	static constexpr uint32_t PREFERENCE[]{
		WL_SHM_FORMAT_XRGB8888, WL_SHM_FORMAT_ARGB8888, WL_SHM_FORMAT_XBGR8888, WL_SHM_FORMAT_ABGR8888,
		WL_SHM_FORMAT_RGB888, WL_SHM_FORMAT_BGR888
	};
	for (uint32_t format : PREFERENCE) {
		if (std::find(offered.begin(), offered.end(), format) != offered.end()) {
			chosen = format;
			return true;
		}
	}
	return false;
}

void ImageCopyFrameSource::ConvertRows(
	const uint8_t* src,
	size_t srcStride,
	const PixelLayout& layout,
	uint8_t* dst,
	uint32_t width,
	uint32_t rowCount
) noexcept {
	const size_t rowBytes = (size_t)width * 4;
	for (uint32_t y = 0; y < rowCount; ++y) {
		const uint8_t* from = src + (size_t)y * srcStride;
		uint8_t* to = dst + (size_t)y * rowBytes;
		if (layout.bytesPerPixel == 4) {
			std::memcpy(to, from, rowBytes);
		} else {
			for (uint32_t x = 0; x < width; ++x) {
				to[x * 4] = from[x * 3];
				to[x * 4 + 1] = from[x * 3 + 1];
				to[x * 4 + 2] = from[x * 3 + 2];
				to[x * 4 + 3] = 255;
			}
		}
	}
}

struct ImageCopyFrameSource::State {
	struct Output {
		wl_output* proxy = nullptr;
		std::string name;
	};

	// One capture: of the output's picture, or of the cursor's image
	struct Capture {
		State* state = nullptr;
		bool isCursor = false;
		ext_image_copy_capture_session_v1* session = nullptr;

		// What the compositor says about the buffers it fills: as it comes
		// in, and as it stands since the last "done"
		std::optional<Size> announcedSize;
		std::vector<uint32_t> announcedFormats;
		Size size;
		uint32_t format = 0;
		PixelLayout layout;
		bool hasConstraints = false;

		wl_buffer* buffer = nullptr;
		uint8_t* data = nullptr;
		size_t dataSize = 0;
		Size bufferSize;
		uint32_t bufferFormat = 0;
		size_t stride = 0;
		// The compositor has not filled the buffer yet
		bool isBufferFresh = true;

		ext_image_copy_capture_frame_v1* frame = nullptr;
		// The buffer holds a frame that has not been used
		bool hasNewFrame = false;
		steady_clock::time_point arrivalTime;
		uint32_t failures = 0;
		// Not before this time, after a failure
		steady_clock::time_point retryTime;
		bool isStopped = false;

		void ReleaseBuffer() noexcept {
			if (buffer) {
				wl_buffer_destroy(buffer);
				buffer = nullptr;
			}
			if (data) {
				munmap(data, dataSize);
				data = nullptr;
			}
		}

		bool EnsureBuffer() noexcept {
			if (buffer && bufferSize == size && bufferFormat == format) {
				return true;
			}
			ReleaseBuffer();

			// Rows follow each other without padding: wlroots refuses a
			// row length that is no multiple of the bytes of a pixel
			stride = (size_t)size.width * layout.bytesPerPixel;
			dataSize = stride * (size_t)size.height;
			const int fd = memfd_create("magpie-capture", MFD_CLOEXEC);
			if (fd < 0 || ftruncate(fd, (off_t)dataSize) != 0) {
				if (fd >= 0) {
					close(fd);
				}
				Logger::Get().Error("No memory for a frame of the capture");
				return false;
			}
			void* mapped = mmap(nullptr, dataSize, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
			if (mapped == MAP_FAILED) {
				close(fd);
				Logger::Get().Error("The memory for a frame of the capture cannot be mapped");
				return false;
			}
			data = (uint8_t*)mapped;

			wl_shm_pool* pool = wl_shm_create_pool(state->shm, fd, (int32_t)dataSize);
			buffer = wl_shm_pool_create_buffer(pool, 0, size.width, size.height, (int32_t)stride, format);
			wl_shm_pool_destroy(pool);
			close(fd);

			bufferSize = size;
			bufferFormat = format;
			isBufferFresh = true;
			return buffer != nullptr;
		}

		// Asks for the next frame, unless one is on its way or waits to be used
		void Request() noexcept {
			if (!session || frame || hasNewFrame || !hasConstraints || isStopped ||
				(failures > 0 && steady_clock::now() < retryTime) || !EnsureBuffer()) {
				return;
			}

			frame = ext_image_copy_capture_session_v1_create_frame(session);
			ext_image_copy_capture_frame_v1_add_listener(frame, &FRAME_LISTENER, this);
			ext_image_copy_capture_frame_v1_attach_buffer(frame, buffer);
			if (isBufferFresh) {
				// Nothing in the buffer is current
				ext_image_copy_capture_frame_v1_damage_buffer(frame, 0, 0, size.width, size.height);
			}
			// Otherwise the buffer holds the previous frame, and the
			// compositor only has to copy what changed since
			ext_image_copy_capture_frame_v1_capture(frame);
		}
	};

	~State() {
		frames.ReleaseBuffer();
		cursor.ReleaseBuffer();
		// Closing the connection destroys everything that was created on it
		if (display) {
			wl_display_disconnect(display);
		}
	}

	void CreateCursorSession() noexcept {
		if (cursorSession || !source || !pointer || !copyManager) {
			return;
		}
		cursorSession = ext_image_copy_capture_manager_v1_create_pointer_cursor_session(copyManager, source, pointer);
		ext_image_copy_capture_cursor_session_v1_add_listener(cursorSession, &CURSOR_LISTENER, this);
		cursor.session = ext_image_copy_capture_cursor_session_v1_get_capture_session(cursorSession);
		ext_image_copy_capture_session_v1_add_listener(cursor.session, &SESSION_LISTENER, &cursor);
	}

	wl_display* display = nullptr;
	wl_shm* shm = nullptr;
	wl_seat* seat = nullptr;
	wl_pointer* pointer = nullptr;
	ext_output_image_capture_source_manager_v1* sourceManager = nullptr;
	ext_image_copy_capture_manager_v1* copyManager = nullptr;
	std::vector<std::unique_ptr<Output>> outputs;

	ext_image_capture_source_v1* source = nullptr;
	Capture frames;
	Capture cursor;
	ext_image_copy_capture_cursor_session_v1* cursorSession = nullptr;
	// The pointer is on the output
	bool isCursorEntered = false;
	Point cursorPos;
	Point cursorHotSpot;
	uint64_t frameCount = 0;
	bool failed = false;

	static constexpr ext_image_copy_capture_session_v1_listener SESSION_LISTENER{
		.buffer_size = [](void* data, ext_image_copy_capture_session_v1*, uint32_t width, uint32_t height) {
			((Capture*)data)->announcedSize = Size{ (int32_t)width, (int32_t)height };
		},
		.shm_format = [](void* data, ext_image_copy_capture_session_v1*, uint32_t format) {
			((Capture*)data)->announcedFormats.push_back(format);
		},
		.dmabuf_device = [](void*, ext_image_copy_capture_session_v1*, wl_array*) {},
		.dmabuf_format = [](void*, ext_image_copy_capture_session_v1*, uint32_t, wl_array*) {},
		.done = [](void* data, ext_image_copy_capture_session_v1*) {
			Capture& capture = *(Capture*)data;
			// What a batch does not mention stays as it was
			if (capture.announcedSize) {
				capture.size = *capture.announcedSize;
				capture.announcedSize.reset();
			}
			if (!capture.announcedFormats.empty()) {
				uint32_t format;
				if (!ChooseFormat(capture.announcedFormats, format) || !GetPixelLayout(format, capture.layout)) {
					if (capture.isCursor) {
						Logger::Get().Warn("The compositor offers the cursor in no format that is handled");
					} else {
						Logger::Get().Error("The compositor offers frames in no format that is handled");
						capture.state->failed = true;
					}
					capture.hasConstraints = false;
					capture.announcedFormats.clear();
					return;
				}
				capture.format = format;
				capture.announcedFormats.clear();
			}

			const Size limit = capture.isCursor ? Size{ MAX_CURSOR_SIZE, MAX_CURSOR_SIZE } : Size{ 16384, 16384 };
			capture.hasConstraints = capture.layout.bytesPerPixel != 0 && capture.size.width > 0 &&
				capture.size.height > 0 && capture.size.width <= limit.width && capture.size.height <= limit.height;
			// With new constraints a new try is worth it at once
			capture.retryTime = {};
			if (!capture.isCursor) {
				Logger::Get().Info(fmt::format("Capture: frames of {}x{}", capture.size.width, capture.size.height));
			}
		},
		.stopped = [](void* data, ext_image_copy_capture_session_v1*) {
			Capture& capture = *(Capture*)data;
			capture.isStopped = true;
			if (!capture.isCursor) {
				// The output is gone
				Logger::Get().Info("The capture has ended");
				capture.state->failed = true;
			}
		}
	};

	static constexpr ext_image_copy_capture_frame_v1_listener FRAME_LISTENER{
		.transform = [](void*, ext_image_copy_capture_frame_v1*, uint32_t) {},
		.damage = [](void*, ext_image_copy_capture_frame_v1*, int32_t, int32_t, int32_t, int32_t) {},
		.presentation_time = [](void*, ext_image_copy_capture_frame_v1*, uint32_t, uint32_t, uint32_t) {},
		.ready = [](void* data, ext_image_copy_capture_frame_v1* frame) {
			Capture& capture = *(Capture*)data;
			ext_image_copy_capture_frame_v1_destroy(frame);
			capture.frame = nullptr;
			capture.hasNewFrame = true;
			capture.isBufferFresh = false;
			capture.arrivalTime = steady_clock::now();
			capture.failures = 0;
			if (!capture.isCursor) {
				++capture.state->frameCount;
			}
		},
		.failed = [](void* data, ext_image_copy_capture_frame_v1* frame, uint32_t reason) {
			Capture& capture = *(Capture*)data;
			ext_image_copy_capture_frame_v1_destroy(frame);
			capture.frame = nullptr;
			// The buffer may hold a part of a frame, and after a change of the
			// constraints it has the wrong size
			capture.isBufferFresh = true;
			++capture.failures;
			capture.retryTime = steady_clock::now() + RETRY_DELAY;

			if (reason == EXT_IMAGE_COPY_CAPTURE_FRAME_V1_FAILURE_REASON_STOPPED) {
				capture.isStopped = true;
			} else if (!capture.isCursor && capture.failures > MAX_FAILURES) {
				// The cursor is tried for as long as the capture lasts: Sway
				// refuses its image whenever the output has no cursor
				capture.isStopped = true;
				Logger::Get().Error(fmt::format("The capture of the output failed {} times in a row", capture.failures));
			}
			if (capture.isStopped && !capture.isCursor) {
				capture.state->failed = true;
			}
		}
	};

	static constexpr ext_image_copy_capture_cursor_session_v1_listener CURSOR_LISTENER{
		.enter = [](void* data, ext_image_copy_capture_cursor_session_v1*) {
			((State*)data)->isCursorEntered = true;
		},
		.leave = [](void* data, ext_image_copy_capture_cursor_session_v1*) {
			((State*)data)->isCursorEntered = false;
		},
		.position = [](void* data, ext_image_copy_capture_cursor_session_v1*, int32_t x, int32_t y) {
			((State*)data)->cursorPos = { x, y };
		},
		.hotspot = [](void* data, ext_image_copy_capture_cursor_session_v1*, int32_t x, int32_t y) {
			((State*)data)->cursorHotSpot = { x, y };
		}
	};

	static constexpr wl_output_listener OUTPUT_LISTENER{
		.geometry = [](void*, wl_output*, int32_t, int32_t, int32_t, int32_t, int32_t,
			const char*, const char*, int32_t) {},
		.mode = [](void*, wl_output*, uint32_t, int32_t, int32_t, int32_t) {},
		.done = [](void*, wl_output*) {},
		.scale = [](void*, wl_output*, int32_t) {},
		.name = [](void* data, wl_output*, const char* name) { ((Output*)data)->name = name; },
		.description = [](void*, wl_output*, const char*) {}
	};

	static constexpr wl_seat_listener SEAT_LISTENER{
		.capabilities = [](void* data, wl_seat* seat, uint32_t capabilities) {
			State& state = *(State*)data;
			// A seat without a pointing device gets one when Magpie makes a
			// virtual pointer; the cursor is followed from then on
			if ((capabilities & WL_SEAT_CAPABILITY_POINTER) && !state.pointer) {
				state.pointer = wl_seat_get_pointer(seat);
				state.CreateCursorSession();
			}
		},
		.name = [](void*, wl_seat*, const char*) {}
	};

	static constexpr wl_registry_listener REGISTRY_LISTENER{
		.global = [](void* data, wl_registry* registry, uint32_t name, const char* interface, uint32_t version) {
			State& state = *(State*)data;
			if (std::strcmp(interface, wl_shm_interface.name) == 0) {
				state.shm = (wl_shm*)wl_registry_bind(registry, name, &wl_shm_interface, 1);
			} else if (std::strcmp(interface, wl_seat_interface.name) == 0 && !state.seat) {
				state.seat = (wl_seat*)wl_registry_bind(registry, name, &wl_seat_interface, std::min(version, 5u));
				wl_seat_add_listener(state.seat, &SEAT_LISTENER, &state);
			} else if (std::strcmp(interface, wl_output_interface.name) == 0 && version >= 4) {
				auto output = std::make_unique<Output>();
				output->proxy = (wl_output*)wl_registry_bind(registry, name, &wl_output_interface, 4);
				wl_output_add_listener(output->proxy, &OUTPUT_LISTENER, output.get());
				state.outputs.push_back(std::move(output));
			} else if (std::strcmp(interface, ext_output_image_capture_source_manager_v1_interface.name) == 0) {
				state.sourceManager = (ext_output_image_capture_source_manager_v1*)wl_registry_bind(
					registry, name, &ext_output_image_capture_source_manager_v1_interface, 1);
			} else if (std::strcmp(interface, ext_image_copy_capture_manager_v1_interface.name) == 0) {
				state.copyManager = (ext_image_copy_capture_manager_v1*)wl_registry_bind(
					registry, name, &ext_image_copy_capture_manager_v1_interface, 1);
			}
		},
		.global_remove = [](void*, wl_registry*, uint32_t) {}
	};
};

ImageCopyFrameSource::ImageCopyFrameSource() noexcept : _state(std::make_unique<State>()) {}

ImageCopyFrameSource::~ImageCopyFrameSource() {
	_duplicateFrameDetector.reset();
}

bool ImageCopyFrameSource::Initialize(VulkanDevice& device, const Options& options) noexcept {
	_device = &device;
	_options = options;
	State& state = *_state;
	state.frames.state = &state;
	state.cursor.state = &state;
	state.cursor.isCursor = true;

	state.display = wl_display_connect(options.waylandDisplay.empty() ? nullptr : options.waylandDisplay.c_str());
	if (!state.display) {
		Logger::Get().Error("Cannot connect to the Wayland compositor for the capture");
		return false;
	}

	wl_registry* registry = wl_display_get_registry(state.display);
	wl_registry_add_listener(registry, &State::REGISTRY_LISTENER, &state);
	// The globals, then the names of the outputs and what the seat has
	if (wl_display_roundtrip(state.display) < 0 || wl_display_roundtrip(state.display) < 0) {
		Logger::Get().Error("The Wayland connection for the capture failed");
		return false;
	}
	if (!state.shm || !state.sourceManager || !state.copyManager) {
		Logger::Get().Error("The compositor lacks the protocol ext-image-copy-capture");
		return false;
	}

	wl_output* output = nullptr;
	for (const std::unique_ptr<State::Output>& candidate : state.outputs) {
		if (candidate->name == options.outputName) {
			output = candidate->proxy;
		}
	}
	if (!output) {
		Logger::Get().Error("There is no output named " + options.outputName + " to capture");
		return false;
	}

	state.source = ext_output_image_capture_source_manager_v1_create_source(state.sourceManager, output);
	// No option: the cursor is not painted into the picture
	state.frames.session = ext_image_copy_capture_manager_v1_create_session(state.copyManager, state.source, 0);
	ext_image_copy_capture_session_v1_add_listener(state.frames.session, &State::SESSION_LISTENER, &state.frames);
	state.CreateCursorSession();

	if (wl_display_flush(state.display) < 0) {
		Logger::Get().Error("The Wayland connection for the capture failed");
		return false;
	}
	return true;
}

int ImageCopyFrameSource::EventFd() const noexcept {
	return wl_display_get_fd(_state->display);
}

bool ImageCopyFrameSource::Dispatch() noexcept {
	State& state = *_state;
	if (state.failed) {
		return false;
	}

	wl_display* display = state.display;
	while (wl_display_prepare_read(display) != 0) {
		wl_display_dispatch_pending(display);
	}
	wl_display_flush(display);
	pollfd fd{ wl_display_get_fd(display), POLLIN, 0 };
	if (poll(&fd, 1, 0) > 0) {
		wl_display_read_events(display);
	} else {
		wl_display_cancel_read(display);
	}
	if (wl_display_dispatch_pending(display) < 0) {
		// What the compositor objected to, if it ended the connection itself
		const wl_interface* interface = nullptr;
		uint32_t id = 0;
		const int error = wl_display_get_error(display);
		const uint32_t code = error == EPROTO ? wl_display_get_protocol_error(display, &interface, &id) : 0;
		Logger::Get().Error(error == EPROTO ?
			fmt::format("The compositor ended the capture: error {} of {}", code, interface ? interface->name : "?") :
			fmt::format("The Wayland connection for the capture is lost: {}", std::strerror(error)));
		state.failed = true;
		return false;
	}

	if (state.cursor.hasNewFrame) {
		_ReadCursorImage();
		state.cursor.hasNewFrame = false;
	}
	_cursorState.pos = state.cursorPos;
	if (_cursorImage.hotSpot != state.cursorHotSpot && !_cursorImage.pixels.empty()) {
		// The same image with another hot spot is another cursor for the drawer
		_cursorImage.hotSpot = state.cursorHotSpot;
		++_cursorState.imageSerial;
	}
	_cursorState.isVisible = state.isCursorEntered && !_cursorImage.pixels.empty();

	state.frames.Request();
	if (state.isCursorEntered) {
		// Only while there is a cursor to have an image of
		state.cursor.Request();
	}
	wl_display_flush(display);

	_statistics.frames = state.frameCount;
	return !state.failed;
}

void ImageCopyFrameSource::_ReadCursorImage() noexcept {
	const State::Capture& capture = _state->cursor;
	const uint32_t width = (uint32_t)capture.bufferSize.width;
	const uint32_t height = (uint32_t)capture.bufferSize.height;

	CaptureCursorImage image;
	image.size = capture.bufferSize;
	image.hotSpot = _state->cursorHotSpot;
	image.pixels.resize((size_t)width * height * 4);
	ConvertRows(capture.data, capture.stride, capture.layout, image.pixels.data(), width, height);
	if (!capture.layout.isRedFirst) {
		for (size_t i = 0; i < image.pixels.size(); i += 4) {
			std::swap(image.pixels[i], image.pixels[i + 2]);
		}
	}

	// A program that hides the cursor gives it an image without content
	const bool isEmpty = std::ranges::all_of(image.pixels, [](uint8_t value) { return value == 0; });
	if (isEmpty) {
		image = {};
	} else if (_cursorImage.size != image.size) {
		Logger::Get().Info(fmt::format("Cursor image {}x{} with the hot spot at {},{}",
			image.size.width, image.size.height, image.hotSpot.x, image.hotSpot.y));
	}

	_cursorImage = std::move(image);
	++_cursorState.imageSerial;
	++_statistics.cursorImages;
}

// Creates the output texture again if the size or the format has changed
bool ImageCopyFrameSource::_PrepareOutput(Size frameSize, bool isRedFirst) noexcept {
	Rect srcRect{ 0, 0, frameSize.width, frameSize.height };
	if (!ScalingGeometry::CropSourceRect(srcRect, _options.cropping)) {
		Logger::Get().Error(fmt::format("The source is {}x{}; too little of it is left after cropping",
			frameSize.width, frameSize.height));
		return false;
	}
	_srcRect = srcRect;

	const VkFormat format = isRedFirst ? VK_FORMAT_R8G8B8A8_UNORM : VK_FORMAT_B8G8R8A8_UNORM;
	if (_output && _output->format == format &&
		(int32_t)_output->width == srcRect.Width() && (int32_t)_output->height == srcRect.Height()) {
		return true;
	}

	// The detector holds a copy of the previous frame, which no longer fits
	_duplicateFrameDetector.reset();

	_output = _device->CreateTexture(format, (uint32_t)srcRect.Width(), (uint32_t)srcRect.Height(),
		VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
	if (!_output) {
		return false;
	}

	if (_options.duplicateFrameDetectionMode != DuplicateFrameDetectionMode::Never) {
		_duplicateFrameDetector = std::make_unique<DuplicateFrameDetector>();
		_duplicateFrameDetector->Initialize(*_device, _options.duplicateFrameDetectionMode,
			_options.isStatisticsForDynamicDetectionEnabled);
	}
	return true;
}

FrameSourceState ImageCopyFrameSource::Update() noexcept {
	State& state = *_state;
	if (state.failed) {
		return FrameSourceState::Error;
	}

	State::Capture& capture = state.frames;
	if (!capture.hasNewFrame) {
		return FrameSourceState::Waiting;
	}

	if (!_PrepareOutput(capture.bufferSize, capture.layout.isRedFirst)) {
		state.failed = true;
		return FrameSourceState::Error;
	}

	const VkExtent3D extent{ _output->width, _output->height, 1 };
	const size_t stagingSize = (size_t)extent.width * extent.height * 4;
	if (!_stagingBuffer || _stagingBuffer->size < stagingSize) {
		_stagingBuffer = _device->CreateBuffer(stagingSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
		if (!_stagingBuffer) {
			state.failed = true;
			return FrameSourceState::Error;
		}
	}
	const uint8_t* src = capture.data + (size_t)_srcRect.top * capture.stride +
		(size_t)_srcRect.left * capture.layout.bytesPerPixel;
	ConvertRows(src, capture.stride, capture.layout, (uint8_t*)_stagingBuffer->data, extent.width, extent.height);

	// The compositor may fill its buffer again while the frame is uploaded
	_outputArrivalTime = capture.arrivalTime;
	capture.hasNewFrame = false;
	capture.Request();
	wl_display_flush(state.display);

	static constexpr VkImageSubresourceLayers COLOR_LAYER{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
	if (!_device->Execute([&](VkCommandBuffer cmd) {
		TransitionImage(cmd, _output->image, _output->layout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
		const VkBufferImageCopy region{
			.imageSubresource = COLOR_LAYER,
			.imageExtent = extent
		};
		vkCmdCopyBufferToImage(cmd, _stagingBuffer->buffer, _output->image,
			VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
		TransitionImage(cmd, _output->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
	})) {
		state.failed = true;
		return FrameSourceState::Error;
	}
	_output->layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

	if (_duplicateFrameDetector && _duplicateFrameDetector->OnNewFrame(*_output)) {
		++_statistics.duplicateFrames;
		return FrameSourceState::Waiting;
	}
	return FrameSourceState::NewFrame;
}

std::string ImageCopyFrameSource::DescribeFormat() const noexcept {
	const State::Capture& capture = _state->frames;
	if (!capture.hasConstraints) {
		return "not negotiated yet";
	}
	const uint32_t format = capture.format;
	// The first two formats of wl_shm have numbers of their own; the others
	// are four letters
	const std::string name = format == WL_SHM_FORMAT_ARGB8888 ? "AR24" : format == WL_SHM_FORMAT_XRGB8888 ? "XR24" :
		std::string{ (char)(format & 0xff), (char)((format >> 8) & 0xff), (char)((format >> 16) & 0xff), (char)(format >> 24) };
	return fmt::format("{} {}x{}, shared memory, output {}",
		name, capture.size.width, capture.size.height, _options.outputName);
}

std::pair<uint32_t, uint32_t> ImageCopyFrameSource::GetStatisticsForDynamicDetection() const noexcept {
	return _duplicateFrameDetector ? _duplicateFrameDetector->GetStatisticsForDynamicDetection()
		: std::pair<uint32_t, uint32_t>();
}

}
