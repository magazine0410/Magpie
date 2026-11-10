// A PipeWire video stream that the tests fill themselves, in place of a
// compositor's screen cast. It delivers frames in shared memory with the
// metadata a screen cast has: header, cursor and damage. It runs in a thread
// of its own, as a compositor is another process.
#pragma once
#include <chrono>
#include <cstring>
#include <optional>
#include <vector>
#include <pipewire/pipewire.h>
#include <spa/param/video/format-utils.h>

namespace RenderTest {

class TestStreamProducer {
public:
	struct Cursor {
		// False: the metadata says that there is no cursor
		bool isVisible = true;
		int32_t x = 0;
		int32_t y = 0;
		int32_t hotSpotX = 0;
		int32_t hotSpotY = 0;
		// Sent if not empty: four bytes per pixel, in the given format
		std::vector<uint8_t> bitmap;
		uint32_t bitmapWidth = 0;
		uint32_t bitmapHeight = 0;
		spa_video_format bitmapFormat = SPA_VIDEO_FORMAT_RGBA;
	};

	struct Frame {
		// Tightly packed BGRx; empty for a buffer that only carries the cursor
		std::vector<uint8_t> pixels;
		std::optional<Cursor> cursor;
		bool hasDamage = true;
	};

	// Bytes between the rows of a frame, to show a consumer that ignores the stride
	static constexpr uint32_t ROW_PADDING = 12;
	static constexpr uint32_t MAX_CURSOR_SIZE = 64;

	TestStreamProducer() = default;
	TestStreamProducer(const TestStreamProducer&) = delete;
	TestStreamProducer& operator=(const TestStreamProducer&) = delete;

	~TestStreamProducer() {
		Stop();
	}

	// False if there is no PipeWire daemon to connect to
	bool Start(uint32_t width, uint32_t height) {
		_width = width;
		_height = height;

		pw_init(nullptr, nullptr);
		_isPipeWireInitialized = true;

		_loop = pw_thread_loop_new("magpie-test-producer", nullptr);
		if (!_loop) {
			return false;
		}
		_context = pw_context_new(pw_thread_loop_get_loop(_loop), nullptr, 0);
		if (!_context || pw_thread_loop_start(_loop) < 0) {
			return false;
		}

		pw_thread_loop_lock(_loop);
		_core = pw_context_connect(_context, nullptr, 0);
		if (!_core) {
			pw_thread_loop_unlock(_loop);
			return false;
		}

		_stream = pw_stream_new(_core, "magpie-test-source", pw_properties_new(
			PW_KEY_MEDIA_TYPE, "Video", PW_KEY_MEDIA_CATEGORY, "Source", nullptr));

		_events.version = PW_VERSION_STREAM_EVENTS;
		_events.state_changed = [](void* data, pw_stream_state, pw_stream_state state, const char*) {
			TestStreamProducer& that = *(TestStreamProducer*)data;
			that._state = state;
			pw_thread_loop_signal(that._loop, false);
		};
		_events.param_changed = [](void* data, uint32_t id, const spa_pod* param) {
			if (param && id == SPA_PARAM_Format) {
				((TestStreamProducer*)data)->_OnFormat(param);
			}
		};
		// A graph cycle that Push has started: hand over the filled buffer
		_events.process = [](void* data) {
			TestStreamProducer& that = *(TestStreamProducer*)data;
			if (that._filledBuffer) {
				pw_stream_queue_buffer(that._stream, that._filledBuffer);
				that._filledBuffer = nullptr;
			}
		};
		_events.remove_buffer = [](void* data, pw_buffer* buffer) {
			TestStreamProducer& that = *(TestStreamProducer*)data;
			if (that._filledBuffer == buffer) {
				that._filledBuffer = nullptr;
			}
		};
		pw_stream_add_listener(_stream, &_listener, &_events, this);

		uint8_t storage[1024];
		spa_pod_builder builder;
		spa_pod_builder_init(&builder, storage, sizeof(storage));
		const spa_pod* format = _BuildFormat(&builder);
		// Like a compositor, the stream drives itself: a frame goes out when
		// there is one
		const int result = pw_stream_connect(_stream, PW_DIRECTION_OUTPUT, PW_ID_ANY,
			pw_stream_flags(PW_STREAM_FLAG_DRIVER | PW_STREAM_FLAG_MAP_BUFFERS), &format, 1);

		// The node gets its id when the stream has reached the paused state
		bool ok = result >= 0;
		while (ok && _state != PW_STREAM_STATE_PAUSED && _state != PW_STREAM_STATE_STREAMING) {
			ok = _state != PW_STREAM_STATE_ERROR && pw_thread_loop_timed_wait(_loop, 5) == 0;
		}
		if (ok) {
			_nodeId = pw_stream_get_node_id(_stream);
		}
		pw_thread_loop_unlock(_loop);
		return ok && _nodeId != SPA_ID_INVALID;
	}

	void Stop() {
		if (_loop) {
			pw_thread_loop_stop(_loop);
		}
		if (_stream) {
			pw_stream_destroy(_stream);
			_stream = nullptr;
		}
		if (_core) {
			pw_core_disconnect(_core);
			_core = nullptr;
		}
		if (_context) {
			pw_context_destroy(_context);
			_context = nullptr;
		}
		if (_loop) {
			pw_thread_loop_destroy(_loop);
			_loop = nullptr;
		}
		if (_isPipeWireInitialized) {
			pw_deinit();
			_isPipeWireInitialized = false;
		}
	}

	uint32_t NodeId() const {
		return _nodeId;
	}

	uint32_t Width() const {
		return _width;
	}

	uint32_t Height() const {
		return _height;
	}

	// A consumer is connected and the buffers exist
	bool IsStreaming() {
		pw_thread_loop_lock(_loop);
		const bool result = _state == PW_STREAM_STATE_STREAMING;
		pw_thread_loop_unlock(_loop);
		return result;
	}

	// Announces another size, as a compositor does when the captured window
	// is resized. Later frames have this size.
	void Resize(uint32_t width, uint32_t height) {
		pw_thread_loop_lock(_loop);
		_width = width;
		_height = height;

		uint8_t storage[1024];
		spa_pod_builder builder;
		spa_pod_builder_init(&builder, storage, sizeof(storage));
		const spa_pod* format = _BuildFormat(&builder);
		pw_stream_update_params(_stream, &format, 1);
		pw_thread_loop_unlock(_loop);
	}

	// False if the stream is not ready for the frame: no consumer yet, the
	// previous frame not handed over, or the buffers still of another size.
	// The caller tries again.
	bool Push(const Frame& frame) {
		pw_thread_loop_lock(_loop);
		const bool result = _Push(frame);
		pw_thread_loop_unlock(_loop);
		return result;
	}

private:
	static constexpr uint32_t CursorMetaSize(uint32_t width, uint32_t height) {
		return uint32_t(sizeof(spa_meta_cursor) + sizeof(spa_meta_bitmap)) + width * height * 4;
	}

	uint32_t _Stride() const {
		return _width * 4 + ROW_PADDING;
	}

	const spa_pod* _BuildFormat(spa_pod_builder* builder) const {
		spa_rectangle size{ _width, _height };
		spa_fraction rate{ 0, 1 };
		return (const spa_pod*)spa_pod_builder_add_object(builder,
			SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat,
			SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video),
			SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
			SPA_FORMAT_VIDEO_format, SPA_POD_Id(SPA_VIDEO_FORMAT_BGRx),
			SPA_FORMAT_VIDEO_size, SPA_POD_Rectangle(&size),
			SPA_FORMAT_VIDEO_framerate, SPA_POD_Fraction(&rate));
	}

	void _OnFormat(const spa_pod* param) {
		spa_video_info_raw info{};
		spa_format_video_raw_parse(param, &info);
		_negotiatedWidth = info.size.width;
		_negotiatedHeight = info.size.height;

		uint8_t storage[1024];
		spa_pod_builder builder;
		spa_pod_builder_init(&builder, storage, sizeof(storage));

		const int stride = int(_negotiatedWidth * 4 + ROW_PADDING);
		const spa_pod* params[4];
		params[0] = (const spa_pod*)spa_pod_builder_add_object(&builder,
			SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers,
			SPA_PARAM_BUFFERS_buffers, SPA_POD_CHOICE_RANGE_Int(8, 4, 16),
			SPA_PARAM_BUFFERS_blocks, SPA_POD_Int(1),
			SPA_PARAM_BUFFERS_size, SPA_POD_Int(stride * (int)_negotiatedHeight),
			SPA_PARAM_BUFFERS_stride, SPA_POD_Int(stride),
			SPA_PARAM_BUFFERS_dataType, SPA_POD_CHOICE_FLAGS_Int(1 << SPA_DATA_MemFd));
		params[1] = (const spa_pod*)spa_pod_builder_add_object(&builder,
			SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta,
			SPA_PARAM_META_type, SPA_POD_Id(SPA_META_Header),
			SPA_PARAM_META_size, SPA_POD_Int(sizeof(spa_meta_header)));
		params[2] = (const spa_pod*)spa_pod_builder_add_object(&builder,
			SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta,
			SPA_PARAM_META_type, SPA_POD_Id(SPA_META_Cursor),
			SPA_PARAM_META_size, SPA_POD_Int(CursorMetaSize(MAX_CURSOR_SIZE, MAX_CURSOR_SIZE)));
		params[3] = (const spa_pod*)spa_pod_builder_add_object(&builder,
			SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta,
			SPA_PARAM_META_type, SPA_POD_Id(SPA_META_VideoDamage),
			SPA_PARAM_META_size, SPA_POD_Int(sizeof(spa_meta_region) * 2));
		pw_stream_update_params(_stream, params, 4);
	}

	bool _Push(const Frame& frame) {
		if (_state != PW_STREAM_STATE_STREAMING || !pw_stream_is_driving(_stream) ||
			_negotiatedWidth != _width || _negotiatedHeight != _height) {
			return false;
		}

		if (_filledBuffer) {
			// The cycle that should have taken it did not run; start another
			pw_stream_trigger_process(_stream);
			return false;
		}

		pw_buffer* buffer = pw_stream_dequeue_buffer(_stream);
		if (!buffer) {
			return false;
		}

		spa_buffer* spaBuffer = buffer->buffer;
		spa_data& data = spaBuffer->datas[0];
		const uint32_t stride = _Stride();
		const bool hasPicture = !frame.pixels.empty();

		if (hasPicture) {
			if (frame.pixels.size() != (size_t)_width * _height * 4 || !data.data || data.maxsize < stride * _height) {
				pw_stream_queue_buffer(_stream, buffer);
				return false;
			}
			// Marks the padding, so that it shows if it is taken for pixels
			std::memset(data.data, 0xEE, (size_t)stride * _height);
			for (uint32_t y = 0; y < _height; ++y) {
				std::memcpy((uint8_t*)data.data + (size_t)y * stride,
					frame.pixels.data() + (size_t)y * _width * 4, (size_t)_width * 4);
			}
		}
		data.chunk->offset = 0;
		data.chunk->stride = (int32_t)stride;
		data.chunk->size = hasPicture ? stride * _height : 0;
		data.chunk->flags = hasPicture ? 0 : SPA_CHUNK_FLAG_CORRUPTED;

		if (auto* header = (spa_meta_header*)spa_buffer_find_meta_data(spaBuffer, SPA_META_Header, sizeof(spa_meta_header))) {
			header->flags = hasPicture ? 0 : SPA_META_HEADER_FLAG_CORRUPTED;
			header->pts = std::chrono::duration_cast<std::chrono::nanoseconds>(
				std::chrono::steady_clock::now().time_since_epoch()).count();
			header->seq = _sequence++;
			header->dts_offset = 0;
			header->offset = 0;
		}

		if (spa_meta* meta = spa_buffer_find_meta(spaBuffer, SPA_META_Cursor)) {
			auto* cursor = (spa_meta_cursor*)meta->data;
			std::memset(cursor, 0, sizeof(*cursor));
			if (frame.cursor && frame.cursor->isVisible) {
				const Cursor& source = *frame.cursor;
				cursor->id = 1;
				cursor->position = { source.x, source.y };
				cursor->hotspot = { source.hotSpotX, source.hotSpotY };
				if (!source.bitmap.empty() &&
					CursorMetaSize(source.bitmapWidth, source.bitmapHeight) <= meta->size) {
					cursor->bitmap_offset = sizeof(spa_meta_cursor);
					auto* bitmap = SPA_PTROFF(cursor, cursor->bitmap_offset, spa_meta_bitmap);
					bitmap->format = source.bitmapFormat;
					bitmap->size = { source.bitmapWidth, source.bitmapHeight };
					bitmap->stride = int32_t(source.bitmapWidth * 4);
					bitmap->offset = sizeof(spa_meta_bitmap);
					std::memcpy(SPA_PTROFF(bitmap, bitmap->offset, uint8_t), source.bitmap.data(), source.bitmap.size());
				}
			}
		}

		if (spa_meta* meta = spa_buffer_find_meta(spaBuffer, SPA_META_VideoDamage)) {
			auto* region = (spa_meta_region*)spa_meta_first(meta);
			if (hasPicture && frame.hasDamage) {
				region->region = SPA_REGION(0, 0, _width, _height);
				++region;
			}
			// The list ends with an empty region
			region->region = SPA_REGION(0, 0, 0, 0);
		}

		// The buffer is queued in the process event of the cycle this starts
		_filledBuffer = buffer;
		pw_stream_trigger_process(_stream);
		return true;
	}

	pw_thread_loop* _loop = nullptr;
	pw_context* _context = nullptr;
	pw_core* _core = nullptr;
	pw_stream* _stream = nullptr;
	spa_hook _listener{};
	pw_stream_events _events{};
	pw_stream_state _state = PW_STREAM_STATE_UNCONNECTED;
	pw_buffer* _filledBuffer = nullptr;
	uint32_t _nodeId = SPA_ID_INVALID;
	uint32_t _width = 0;
	uint32_t _height = 0;
	uint32_t _negotiatedWidth = 0;
	uint32_t _negotiatedHeight = 0;
	uint64_t _sequence = 0;
	bool _isPipeWireInitialized = false;
};

}
