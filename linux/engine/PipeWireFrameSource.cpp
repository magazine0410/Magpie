#include "PipeWireFrameSource.h"
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <fmt/format.h>
#include <pipewire/pipewire.h>
#include <spa/debug/types.h>
#include <spa/param/video/format-utils.h>
#include <spa/param/video/type-info.h>
#include <spa/utils/result.h>
#include "DuplicateFrameDetector.h"
#include "Logger.h"
#include "ScalingGeometry.h"
#include "VulkanDevice.h"

namespace Magpie {

// The pixel formats that are accepted, in order of preference. Screen casts
// use the first; the others cost nothing to accept.
static constexpr spa_video_format STREAM_FORMATS[] = {
	SPA_VIDEO_FORMAT_BGRx, SPA_VIDEO_FORMAT_BGRA, SPA_VIDEO_FORMAT_RGBx, SPA_VIDEO_FORMAT_RGBA
};

static VkFormat ToVkFormat(spa_video_format format) noexcept {
	switch (format) {
	case SPA_VIDEO_FORMAT_BGRx:
	case SPA_VIDEO_FORMAT_BGRA:
		return VK_FORMAT_B8G8R8A8_UNORM;
	case SPA_VIDEO_FORMAT_RGBx:
	case SPA_VIDEO_FORMAT_RGBA:
		return VK_FORMAT_R8G8B8A8_UNORM;
	default:
		return VK_FORMAT_UNDEFINED;
	}
}

// Largest cursor image that is asked for
static constexpr uint32_t MAX_CURSOR_SIZE = 256;

static constexpr uint32_t CursorMetaSize(uint32_t width, uint32_t height) noexcept {
	return uint32_t(sizeof(spa_meta_cursor) + sizeof(spa_meta_bitmap)) + width * height * 4;
}

struct PipeWireFrameSource::StreamFormat {
	spa_video_info_raw info{};
	VkFormat vkFormat = VK_FORMAT_UNDEFINED;
	// The frames are DMA-BUFs; otherwise they are in shared memory
	bool isDmabuf = false;
};

struct PipeWireFrameSource::Listeners {
	spa_hook stream{};
	spa_hook core{};
	pw_stream_events streamEvents{};
	pw_core_events coreEvents{};

	static void OnStateChanged(void* data, pw_stream_state, pw_stream_state state, const char* error) noexcept {
		PipeWireFrameSource& that = *(PipeWireFrameSource*)data;

		Logger::Get().Info(fmt::format("Capture stream: {}", pw_stream_state_as_string(state)));

		if (state == PW_STREAM_STATE_ERROR) {
			// Also the ordinary end: when the source goes away, the session
			// manager reports that the target is not found
			Logger::Get().Warn(fmt::format("The capture stream stopped: {}", error ? error : "unknown error"));
			that._failed = true;
		} else if (state == PW_STREAM_STATE_UNCONNECTED) {
			// Only reached again when the stream is disconnected, for example
			// because the source is gone
			Logger::Get().Info("The capture stream has ended");
			that._failed = true;
		}
	}

	static void OnParamChanged(void* data, uint32_t id, const spa_pod* param) noexcept {
		PipeWireFrameSource& that = *(PipeWireFrameSource*)data;
		if (!param || id != SPA_PARAM_Format) {
			return;
		}

		StreamFormat format;
		if (spa_format_video_raw_parse(param, &format.info) < 0) {
			Logger::Get().Error("The format of the capture stream cannot be read");
			that._failed = true;
			return;
		}
		format.vkFormat = ToVkFormat(format.info.format);
		format.isDmabuf = spa_pod_find_prop(param, nullptr, SPA_FORMAT_VIDEO_modifier) != nullptr;
		if (format.vkFormat == VK_FORMAT_UNDEFINED) {
			Logger::Get().Error("The capture stream has a pixel format that was not offered");
			that._failed = true;
			return;
		}

		*that._format = format;
		Logger::Get().Info(fmt::format("Capture stream format: {}", that.DescribeFormat()));

		// Which kind of buffers and which metadata to deliver
		uint8_t storage[1024];
		spa_pod_builder builder;
		spa_pod_builder_init(&builder, storage, sizeof(storage));

		const int dataTypes = format.isDmabuf
			? (1 << SPA_DATA_DmaBuf)
			: ((1 << SPA_DATA_MemFd) | (1 << SPA_DATA_MemPtr));

		const spa_pod* params[4];
		params[0] = (const spa_pod*)spa_pod_builder_add_object(&builder,
			SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers,
			SPA_PARAM_BUFFERS_dataType, SPA_POD_Int(dataTypes));
		params[1] = (const spa_pod*)spa_pod_builder_add_object(&builder,
			SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta,
			SPA_PARAM_META_type, SPA_POD_Id(SPA_META_Header),
			SPA_PARAM_META_size, SPA_POD_Int(sizeof(spa_meta_header)));
		params[2] = (const spa_pod*)spa_pod_builder_add_object(&builder,
			SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta,
			SPA_PARAM_META_type, SPA_POD_Id(SPA_META_Cursor),
			SPA_PARAM_META_size, SPA_POD_CHOICE_RANGE_Int(
				CursorMetaSize(64, 64), CursorMetaSize(1, 1), CursorMetaSize(MAX_CURSOR_SIZE, MAX_CURSOR_SIZE)));
		params[3] = (const spa_pod*)spa_pod_builder_add_object(&builder,
			SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta,
			SPA_PARAM_META_type, SPA_POD_Id(SPA_META_VideoDamage),
			SPA_PARAM_META_size, SPA_POD_CHOICE_RANGE_Int(
				sizeof(spa_meta_region) * 16, sizeof(spa_meta_region) * 1, sizeof(spa_meta_region) * 16));

		pw_stream_update_params(that._stream, params, 4);
	}

	// A change of format replaces the buffers
	static void OnRemoveBuffer(void* data, pw_buffer* buffer) noexcept {
		PipeWireFrameSource& that = *(PipeWireFrameSource*)data;
		that._importedBuffers.erase(buffer);
		if (that._pendingBuffer == buffer) {
			that._pendingBuffer = nullptr;
		}
	}

	static void OnProcess(void* data) noexcept {
		PipeWireFrameSource& that = *(PipeWireFrameSource*)data;

		// Every buffer is looked at for the cursor, but only the newest
		// picture is kept, as the Graphics Capture source on Windows does
		while (pw_buffer* buffer = pw_stream_dequeue_buffer(that._stream)) {
			that._ReadCursor(buffer);

			const spa_buffer* spaBuffer = buffer->buffer;
			bool hasPicture = false;
			if (spaBuffer->n_datas > 0) {
				const spa_data& data0 = spaBuffer->datas[0];
				const auto* header = (const spa_meta_header*)spa_buffer_find_meta_data(
					spaBuffer, SPA_META_Header, sizeof(spa_meta_header));
				// "Corrupted" is how a producer says that a buffer has no
				// picture, for example when only the cursor moved
				const bool corrupted = (data0.chunk->flags & SPA_CHUNK_FLAG_CORRUPTED) ||
					(header && (header->flags & SPA_META_HEADER_FLAG_CORRUPTED));
				hasPicture = !corrupted && (data0.type == SPA_DATA_DmaBuf || data0.chunk->size > 0);
			}

			if (!hasPicture) {
				++that._statistics.cursorUpdates;
				pw_stream_queue_buffer(that._stream, buffer);
				continue;
			}

			++that._statistics.frames;

			// The damage is a list of regions that ends with an empty one
			bool isUndamaged = false;
			if (const spa_meta* meta = spa_buffer_find_meta(spaBuffer, SPA_META_VideoDamage)) {
				bool hasRegion = false;
				const spa_meta_region* region;
				spa_meta_for_each(region, meta) {
					if (!spa_meta_region_is_valid(region)) {
						break;
					}
					hasRegion = true;
				}

				if (hasRegion) {
					that._hasSeenDamage = true;
				} else {
					isUndamaged = that._hasSeenDamage;
				}
			}

			if (isUndamaged) {
				++that._statistics.undamagedFrames;
				if (that._options.isDamageUsed) {
					// Nothing has changed, so this is not a new frame
					pw_stream_queue_buffer(that._stream, buffer);
					continue;
				}
			}

			if (that._pendingBuffer) {
				++that._statistics.skippedFrames;
				pw_stream_queue_buffer(that._stream, that._pendingBuffer);
			}
			that._pendingBuffer = buffer;
			that._pendingArrivalTime = std::chrono::steady_clock::now();
			that._isPendingUndamaged = isUndamaged;
		}
	}

	static void OnCoreError(void* data, uint32_t id, int, int res, const char* message) noexcept {
		PipeWireFrameSource& that = *(PipeWireFrameSource*)data;

		if (id == PW_ID_CORE && res == -EPIPE) {
			Logger::Get().Error("The connection to PipeWire is lost");
			that._failed = true;
		} else {
			// Errors of the stream also arrive as a change of its state
			Logger::Get().Info(fmt::format("PipeWire error on object {}: {} ({})",
				id, message ? message : "", spa_strerror(res)));
		}
	}
};

PipeWireFrameSource::PipeWireFrameSource() noexcept :
	_listeners(std::make_unique<Listeners>()), _format(std::make_unique<StreamFormat>()) {}

PipeWireFrameSource::~PipeWireFrameSource() {
	_duplicateFrameDetector.reset();

	if (_stream) {
		// Reports every buffer as removed, which empties _importedBuffers
		pw_stream_destroy(_stream);
	}
	if (_core) {
		pw_core_disconnect(_core);
	}
	if (_context) {
		pw_context_destroy(_context);
	}
	if (_loop) {
		if (_isLoopEntered) {
			pw_loop_leave(_loop);
		}
		pw_loop_destroy(_loop);
	}
	if (_isPipeWireInitialized) {
		pw_deinit();
	}
}

// One offer per pixel format and kind of buffer. With `modifiers` the offer
// is for DMA-BUFs with one of them, without for shared memory.
static const spa_pod* BuildFormatOffer(
	spa_pod_builder* builder,
	spa_video_format format,
	const std::vector<uint64_t>& modifiers
) noexcept {
	spa_rectangle defaultSize{ 1920, 1080 }, minSize{ 1, 1 }, maxSize{ 16384, 16384 };
	// The producer decides the rate; these values only have to allow it
	spa_fraction defaultRate{ 240, 1 }, minRate{ 0, 1 }, maxRate{ 1000, 1 };

	spa_pod_frame formatFrame;
	spa_pod_builder_push_object(builder, &formatFrame, SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat);
	spa_pod_builder_add(builder,
		SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video),
		SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
		SPA_FORMAT_VIDEO_format, SPA_POD_Id(format), 0);

	if (!modifiers.empty()) {
		// The producer chooses one of the modifiers (it knows which of them
		// its own graphics card can render to) and announces it
		spa_pod_frame modifierFrame;
		spa_pod_builder_prop(builder, SPA_FORMAT_VIDEO_modifier,
			SPA_POD_PROP_FLAG_MANDATORY | SPA_POD_PROP_FLAG_DONT_FIXATE);
		spa_pod_builder_push_choice(builder, &modifierFrame, SPA_CHOICE_Enum, 0);
		// The first value of an enumeration is its default
		spa_pod_builder_long(builder, (int64_t)modifiers[0]);
		for (uint64_t modifier : modifiers) {
			spa_pod_builder_long(builder, (int64_t)modifier);
		}
		spa_pod_builder_pop(builder, &modifierFrame);
	}

	spa_pod_builder_add(builder,
		SPA_FORMAT_VIDEO_size, SPA_POD_CHOICE_RANGE_Rectangle(&defaultSize, &minSize, &maxSize),
		SPA_FORMAT_VIDEO_framerate, SPA_POD_CHOICE_RANGE_Fraction(&defaultRate, &minRate, &maxRate), 0);
	return (const spa_pod*)spa_pod_builder_pop(builder, &formatFrame);
}

bool PipeWireFrameSource::Initialize(VulkanDevice& device, const Options& options) noexcept {
	_device = &device;
	_options = options;

	pw_init(nullptr, nullptr);
	_isPipeWireInitialized = true;

	_loop = pw_loop_new(nullptr);
	if (!_loop) {
		Logger::Get().Error("pw_loop_new failed");
		return false;
	}
	_context = pw_context_new(_loop, nullptr, 0);
	if (!_context) {
		Logger::Get().Error("pw_context_new failed");
		return false;
	}

	if (options.pipeWireFd >= 0) {
		// PipeWire closes the descriptor it is given
		const int fd = fcntl(options.pipeWireFd, F_DUPFD_CLOEXEC, 0);
		if (fd < 0) {
			Logger::Get().Error("Duplicating the PipeWire descriptor failed");
			return false;
		}
		_core = pw_context_connect_fd(_context, fd, nullptr, 0);
	} else {
		_core = pw_context_connect(_context, nullptr, 0);
	}
	if (!_core) {
		Logger::Get().Error(fmt::format("Connecting to PipeWire failed: {}", std::strerror(errno)));
		return false;
	}

	_listeners->coreEvents.version = PW_VERSION_CORE_EVENTS;
	_listeners->coreEvents.error = Listeners::OnCoreError;
	pw_core_add_listener(_core, &_listeners->core, &_listeners->coreEvents, this);

	_stream = pw_stream_new(_core, "magpie-capture", pw_properties_new(
		PW_KEY_MEDIA_TYPE, "Video",
		PW_KEY_MEDIA_CATEGORY, "Capture",
		PW_KEY_MEDIA_ROLE, "Screen",
		// When the source goes away the stream ends; without this the session
		// manager keeps it waiting for another node
		PW_KEY_NODE_DONT_RECONNECT, "true",
		nullptr));
	if (!_stream) {
		Logger::Get().Error("pw_stream_new failed");
		return false;
	}

	_listeners->streamEvents.version = PW_VERSION_STREAM_EVENTS;
	_listeners->streamEvents.state_changed = Listeners::OnStateChanged;
	_listeners->streamEvents.param_changed = Listeners::OnParamChanged;
	_listeners->streamEvents.remove_buffer = Listeners::OnRemoveBuffer;
	_listeners->streamEvents.process = Listeners::OnProcess;
	pw_stream_add_listener(_stream, &_listeners->stream, &_listeners->streamEvents, this);

	// The offers, DMA-BUF first. The modifier list is that of the graphics
	// card that renders: a compositor on another card finds no common
	// modifier and falls back to the offers for shared memory.
	std::vector<uint8_t> storage(64 * 1024);
	spa_pod_builder builder;
	spa_pod_builder_init(&builder, storage.data(), (uint32_t)storage.size());
	std::vector<const spa_pod*> offers;

	if (options.isDmabufAllowed && device.IsDmabufImportSupported()) {
		for (spa_video_format format : STREAM_FORMATS) {
			const std::vector<uint64_t> modifiers = device.DmabufModifiers(ToVkFormat(format));
			if (!modifiers.empty()) {
				offers.push_back(BuildFormatOffer(&builder, format, modifiers));
			}
		}
	}
	for (spa_video_format format : STREAM_FORMATS) {
		offers.push_back(BuildFormatOffer(&builder, format, {}));
	}
	if (std::find(offers.begin(), offers.end(), nullptr) != offers.end()) {
		Logger::Get().Error("The format offers do not fit their buffer");
		return false;
	}

	const int result = pw_stream_connect(_stream, PW_DIRECTION_INPUT, options.nodeId,
		pw_stream_flags(PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS),
		offers.data(), (uint32_t)offers.size());
	if (result < 0) {
		Logger::Get().Error(fmt::format("pw_stream_connect failed: {}", spa_strerror(result)));
		return false;
	}

	// From here on the loop belongs to the calling thread
	pw_loop_enter(_loop);
	_isLoopEntered = true;
	return true;
}

int PipeWireFrameSource::EventFd() const noexcept {
	return pw_loop_get_fd(_loop);
}

bool PipeWireFrameSource::Dispatch() noexcept {
	if (_failed) {
		return false;
	}

	const int result = pw_loop_iterate(_loop, 0);
	if (result < 0 && result != -EINTR) {
		Logger::Get().Error(fmt::format("pw_loop_iterate failed: {}", spa_strerror(result)));
		_failed = true;
	}

	return !_failed;
}

void PipeWireFrameSource::_ReadCursor(pw_buffer* buffer) noexcept {
	const spa_meta* meta = spa_buffer_find_meta(buffer->buffer, SPA_META_Cursor);
	if (!meta || meta->size < sizeof(spa_meta_cursor)) {
		// The stream delivers no cursor
		return;
	}

	const auto* cursor = (const spa_meta_cursor*)meta->data;
	if (!spa_meta_cursor_is_valid(cursor)) {
		// The pointer is not over the source, or is hidden
		_cursorState.isVisible = false;
		return;
	}

	_cursorState.isVisible = true;
	_cursorState.pos = { cursor->position.x, cursor->position.y };

	// The image only comes along when it has changed
	if (cursor->bitmap_offset < sizeof(spa_meta_cursor) ||
		cursor->bitmap_offset + sizeof(spa_meta_bitmap) > meta->size) {
		return;
	}

	const auto* bitmap = SPA_PTROFF(cursor, cursor->bitmap_offset, const spa_meta_bitmap);
	const uint32_t width = bitmap->size.width;
	const uint32_t height = bitmap->size.height;
	const bool isBgra = bitmap->format == SPA_VIDEO_FORMAT_BGRA;
	if (bitmap->format != SPA_VIDEO_FORMAT_RGBA && !isBgra) {
		// "No new image" is format 0; other formats are not in use for cursors
		return;
	}

	CaptureCursorImage image;
	if (width > 0 && height > 0 && bitmap->offset >= sizeof(spa_meta_bitmap)) {
		const size_t stride = bitmap->stride > 0 ? (size_t)bitmap->stride : (size_t)width * 4;
		const size_t available = meta->size - cursor->bitmap_offset;
		if (width > MAX_CURSOR_SIZE || height > MAX_CURSOR_SIZE || stride < (size_t)width * 4 ||
			bitmap->offset + stride * (height - 1) + (size_t)width * 4 > available) {
			Logger::Get().Warn("Ignoring a cursor image that does not fit its metadata");
			return;
		}

		image.size = { (int32_t)width, (int32_t)height };
		image.hotSpot = { cursor->hotspot.x, cursor->hotspot.y };
		image.pixels.resize((size_t)width * height * 4);

		const uint8_t* src = SPA_PTROFF(bitmap, bitmap->offset, const uint8_t);
		for (uint32_t y = 0; y < height; ++y) {
			uint8_t* dst = &image.pixels[(size_t)y * width * 4];
			std::memcpy(dst, src + y * stride, (size_t)width * 4);
			if (isBgra) {
				for (uint32_t x = 0; x < width; ++x) {
					std::swap(dst[x * 4], dst[x * 4 + 2]);
				}
			}
		}
	}
	// Otherwise the image is empty, which means an invisible cursor

	// PipeWire does not say whether the colours are multiplied by alpha. KWin
	// and Mutter send them multiplied, and the cursor is drawn on that
	// assumption. A colour above its alpha contradicts it.
	bool isPremultiplied = true;
	for (size_t i = 0; i < image.pixels.size() && isPremultiplied; i += 4) {
		const uint8_t alpha = image.pixels[i + 3];
		isPremultiplied = image.pixels[i] <= alpha && image.pixels[i + 1] <= alpha && image.pixels[i + 2] <= alpha;
	}
	if (isPremultiplied) {
		Logger::Get().Info(fmt::format("Cursor image {}x{} with the hot spot at {},{}",
			image.size.width, image.size.height, image.hotSpot.x, image.hotSpot.y));
	} else {
		Logger::Get().Warn(fmt::format("The cursor image ({}x{}) has colours that are not multiplied by alpha; "
			"its edges will look too bright", image.size.width, image.size.height));
	}

	_cursorImage = std::move(image);
	++_cursorState.imageSerial;
}

FrameSourceState PipeWireFrameSource::Update() noexcept {
	if (_failed) {
		return FrameSourceState::Error;
	}

	if (!_pendingBuffer) {
		return FrameSourceState::Waiting;
	}

	pw_buffer* buffer = _pendingBuffer;
	_pendingBuffer = nullptr;

	const bool copied = _PrepareOutput() && _CopyFrame(buffer);
	// The producer may write to the buffer again
	pw_stream_queue_buffer(_stream, buffer);
	if (!copied) {
		_failed = true;
		return FrameSourceState::Error;
	}

	_outputArrivalTime = _pendingArrivalTime;

	if (_duplicateFrameDetector && _duplicateFrameDetector->OnNewFrame(*_output)) {
		++_statistics.duplicateFrames;
		return FrameSourceState::Waiting;
	}

	if (_isPendingUndamaged && _options.duplicateFrameDetectionMode == DuplicateFrameDetectionMode::Always) {
		// The comparison contradicts the stream's damage
		++_statistics.changedUndamagedFrames;
	}

	return FrameSourceState::NewFrame;
}

// Creates the output texture again if the stream's size or format has changed
bool PipeWireFrameSource::_PrepareOutput() noexcept {
	Rect srcRect{ 0, 0, (int32_t)_format->info.size.width, (int32_t)_format->info.size.height };
	if (!ScalingGeometry::CropSourceRect(srcRect, _options.cropping)) {
		Logger::Get().Error(fmt::format("The source is {}x{}; too little of it is left after cropping",
			_format->info.size.width, _format->info.size.height));
		return false;
	}
	_srcRect = srcRect;

	if (_output && _output->format == _format->vkFormat &&
		(int32_t)_output->width == srcRect.Width() && (int32_t)_output->height == srcRect.Height()) {
		return true;
	}

	// The detector holds a copy of the previous frame, which no longer fits
	_duplicateFrameDetector.reset();

	_output = _device->CreateTexture(_format->vkFormat, (uint32_t)srcRect.Width(), (uint32_t)srcRect.Height(),
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

bool PipeWireFrameSource::_CopyFrame(pw_buffer* buffer) noexcept {
	static constexpr VkImageSubresourceLayers COLOR_LAYER{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };

	const spa_data& data = buffer->buffer->datas[0];
	const uint32_t streamWidth = _format->info.size.width;
	const uint32_t streamHeight = _format->info.size.height;
	const uint32_t stride = data.chunk->stride > 0 ? (uint32_t)data.chunk->stride : streamWidth * 4;
	const VkExtent3D extent{ _output->width, _output->height, 1 };

	// Set when a DMA-BUF frame is copied, otherwise the frame is in _stagingBuffer
	Texture* imported = nullptr;

	if (data.type == SPA_DATA_DmaBuf) {
		std::shared_ptr<Texture>& texture = _importedBuffers[buffer];
		if (!texture) {
			texture = _device->ImportDmabuf((int)data.fd, _format->vkFormat, streamWidth, streamHeight,
				_format->info.modifier, data.chunk->offset, stride);
			if (!texture) {
				_importedBuffers.erase(buffer);
				Logger::Get().Error("A frame of the capture stream could not be imported. "
					"Capturing without DMA-BUF avoids the import.");
				return false;
			}
		}
		imported = texture.get();
	} else {
		if (!data.data) {
			Logger::Get().Error("A frame of the capture stream is not mapped");
			return false;
		}

		const size_t rowBytes = (size_t)extent.width * 4;
		const size_t firstByte = (size_t)data.chunk->offset + (size_t)_srcRect.top * stride + (size_t)_srcRect.left * 4;
		if (firstByte + (size_t)(extent.height - 1) * stride + rowBytes > data.maxsize) {
			Logger::Get().Error("A frame of the capture stream is smaller than its format says");
			return false;
		}

		const size_t stagingSize = rowBytes * extent.height;
		if (!_stagingBuffer || _stagingBuffer->size < stagingSize) {
			_stagingBuffer = _device->CreateBuffer(stagingSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
			if (!_stagingBuffer) {
				return false;
			}
		}

		const uint8_t* src = (const uint8_t*)data.data + firstByte;
		uint8_t* dst = (uint8_t*)_stagingBuffer->data;
		for (uint32_t y = 0; y < extent.height; ++y) {
			std::memcpy(dst + y * rowBytes, src + (size_t)y * stride, rowBytes);
		}
	}

	if (!_device->Execute([&](VkCommandBuffer cmd) {
		TransitionImage(cmd, _output->image, _output->layout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

		if (imported) {
			_device->AcquireDmabuf(cmd, *imported);
			const VkImageCopy region{
				.srcSubresource = COLOR_LAYER,
				.srcOffset = { _srcRect.left, _srcRect.top, 0 },
				.dstSubresource = COLOR_LAYER,
				.extent = extent
			};
			vkCmdCopyImage(cmd, imported->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
				_output->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
			_device->ReleaseDmabuf(cmd, *imported);
		} else {
			const VkBufferImageCopy region{
				.imageSubresource = COLOR_LAYER,
				.imageExtent = extent
			};
			vkCmdCopyBufferToImage(cmd, _stagingBuffer->buffer, _output->image,
				VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
		}

		TransitionImage(cmd, _output->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
	})) {
		return false;
	}

	_output->layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	return true;
}

std::string PipeWireFrameSource::DescribeFormat() const noexcept {
	if (_format->vkFormat == VK_FORMAT_UNDEFINED) {
		return "not negotiated yet";
	}

	const char* name = spa_debug_type_find_short_name(spa_type_video_format, _format->info.format);
	std::string result = fmt::format("{} {}x{}, ", name ? name : "?",
		_format->info.size.width, _format->info.size.height);
	if (_format->isDmabuf) {
		result += fmt::format("DMA-BUF with modifier 0x{:x}", _format->info.modifier);
	} else {
		result += "shared memory";
	}
	if (_format->info.max_framerate.denom != 0 && _format->info.max_framerate.num != 0) {
		result += fmt::format(", at most {:.2f} frames per second",
			(double)_format->info.max_framerate.num / _format->info.max_framerate.denom);
	}
	return result;
}

std::pair<uint32_t, uint32_t> PipeWireFrameSource::GetStatisticsForDynamicDetection() const noexcept {
	return _duplicateFrameDetector ? _duplicateFrameDetector->GetStatisticsForDynamicDetection()
		: std::pair<uint32_t, uint32_t>();
}

}
