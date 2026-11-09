// Tests T2, T4 and the capture half of T5: ask KWin for a screencast stream
// through zkde_screencast_unstable_v1 (no portal, no dialog), consume it with
// PipeWire and report what arrives.
//
// Usage:
//   kwin_cast --window UUID   [options]
//   kwin_cast --output NAME   [options]
//   kwin_cast --virtual NAME WxH [options]     creates a virtual output
// Options:
//   --cursor hidden|embedded|metadata   (default metadata)
//   --no-dmabuf      offer shared-memory buffers only
//   --vk N           take the modifier list from Vulkan device N and import
//                    every DMA-BUF frame into it
//   --seconds N      run time (default 10)
//   --dump FILE      write the last received frame as a PPM image on exit
//                    (shared-memory frames only, so combine with --no-dmabuf);
//                    SIGUSR1 writes FILE.<n>.ppm at any time
//   --size WxH       offer exactly this stream size. For a virtual output the
//                    default is the size given to --virtual. Any stream of a
//                    virtual output resizes it to the negotiated size.
//   --inject-circle CX CY R   move the pointer in a circle (global coordinates)
//                    through org_kde_kwin_fake_input while capturing, and
//                    measure how long each position takes to show up in the
//                    stream's cursor metadata
//   --origin X Y     global position of the stream's top-left corner, needed
//                    to compare injected and reported positions
//
// Once a second it prints: frames with new image data, cursor-only updates,
// buffer type, age of the newest frame (read from the timestamp row drawn by
// t8_testsource) and the last cursor position from the stream metadata.
#include <algorithm>
#include <csignal>
#include <cmath>
#include <deque>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <sys/mman.h>
#include <vector>

#include <libdrm/drm_fourcc.h>
#include <pipewire/pipewire.h>
#include <spa/debug/types.h>
#include <spa/param/video/format-utils.h>
#include <spa/param/video/type-info.h>
#include <wayland-client.h>

#include "fake-input-client-protocol.h"
#include "vk_import.h"
#include "zkde-screencast-unstable-v1-client-protocol.h"

// Same layout as t8_testsource.
namespace Stamp {
constexpr int CELL = 8;
constexpr int SYNC_CELLS = 4;
constexpr int BITS = 48;
constexpr int ROWS = 96;	// rows searched for the timestamp row
}

struct Output {
	wl_output* proxy = nullptr;
	std::string name;
};

struct App {
	// options
	std::string windowUuid, outputName, virtualName;
	int virtualW = 0, virtualH = 0;
	uint32_t cursorMode = ZKDE_SCREENCAST_UNSTABLE_V1_POINTER_METADATA;
	bool offerDmabuf = true;
	int vkIndex = -1;
	int seconds = 10;
	std::string dumpPath;
	int dumpCount = 0;
	int sizeW = 0, sizeH = 0;	// --size
	std::vector<uint8_t> lastFrame;	// tightly packed, 4 bytes per pixel
	uint32_t lastFrameW = 0, lastFrameH = 0;
	bool inject = false;
	double circleX = 0, circleY = 0, circleR = 0;
	int originX = 0, originY = 0;

	// pointer injection
	org_kde_kwin_fake_input* fakeInput = nullptr;
	struct Sample {
		int64_t micros;
		int x, y;
	};
	std::deque<Sample> injected;
	double angle = 0;
	std::vector<double> cursorLag;	// milliseconds

	// Wayland
	wl_display* display = nullptr;
	zkde_screencast_unstable_v1* screencast = nullptr;
	uint32_t screencastVersion = 0;
	std::vector<Output*> outputs;
	zkde_screencast_stream_unstable_v1* castStream = nullptr;
	uint32_t nodeId = 0;
	bool failed = false;

	// PipeWire
	pw_main_loop* loop = nullptr;
	pw_stream* stream = nullptr;
	spa_hook streamListener{};
	spa_video_info_raw format{};
	bool hasModifier = false;

	VkImporter* importer = nullptr;

	// statistics for the current second
	int elapsed = 0;
	int frames = 0, cursorOnly = 0, skipped = 0;
	int cursorMetas = 0, cursorMoves = 0, bitmaps = 0;
	int damageRects = 0;
	uint32_t lastDataType = 0;
	spa_point cursorPos{ -1, -1 };
	spa_rectangle bitmapSize{ 0, 0 };
	std::vector<double> ages;	// milliseconds
	std::vector<double> importTimes;
	std::string importError;
	int stampX = -1, stampY = -1;
	bool mmapTried = false, mmapOk = false;
	// totals
	int totalFrames = 0;
};

static int64_t NowMicros() {
	timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return int64_t(ts.tv_sec) * 1000000 + ts.tv_nsec / 1000;
}

////////////////////////////////////////////////////////////////////////////////
// Pointer injection
////////////////////////////////////////////////////////////////////////////////

// Every 4 ms: next point on the circle, about 2.5 px from the previous one, so
// that each reported position identifies one injection time.
static void OnInject(void* data, uint64_t) {
	App& app = *(App*)data;
	app.angle += 2.5 / app.circleR;
	const double x = app.circleX + app.circleR * std::cos(app.angle);
	const double y = app.circleY + app.circleR * std::sin(app.angle);

	org_kde_kwin_fake_input_pointer_motion_absolute(app.fakeInput, wl_fixed_from_double(x), wl_fixed_from_double(y));
	wl_display_flush(app.display);

	app.injected.push_back({ NowMicros(), (int)std::lround(x), (int)std::lround(y) });
	while (app.injected.size() > 200) {	// 0.8 s of history
		app.injected.pop_front();
	}
}

// Age of the newest injected sample that matches a reported cursor position.
static void MatchCursor(App& app, int globalX, int globalY) {
	for (auto it = app.injected.rbegin(); it != app.injected.rend(); ++it) {
		if (std::abs(it->x - globalX) <= 1 && std::abs(it->y - globalY) <= 1) {
			app.cursorLag.push_back((NowMicros() - it->micros) / 1000.0);
			return;
		}
	}
}

////////////////////////////////////////////////////////////////////////////////
// Timestamp row
////////////////////////////////////////////////////////////////////////////////

// `pixels` holds `rows` rows of 4-byte pixels with the given stride.
struct PixelView {
	const uint8_t* pixels;
	uint32_t width, rows, stride;

	int Luma(uint32_t x, uint32_t y) const {
		const uint8_t* p = pixels + (size_t)y * stride + (size_t)x * 4;
		return (p[0] + p[1] + p[2]) / 3;
	}
	bool White(uint32_t x, uint32_t y) const { return Luma(x, y) > 200; }
	bool Black(uint32_t x, uint32_t y) const { return Luma(x, y) < 55; }
};

static bool SyncAt(const PixelView& v, int x, int y) {
	const int cy = y + Stamp::CELL / 2;
	const int total = (Stamp::SYNC_CELLS + Stamp::BITS) * Stamp::CELL;
	if (x < 0 || y < 0 || cy >= (int)v.rows || x + total > (int)v.width) {
		return false;
	}
	for (int i = 0; i < Stamp::SYNC_CELLS; ++i) {
		const int cx = x + i * Stamp::CELL + Stamp::CELL / 2;
		if (i % 2 ? !v.Black(cx, cy) : !v.White(cx, cy)) {
			return false;
		}
	}
	return true;
}

// Finds the row once (the window may have a decoration above the client area).
static bool LocateStamp(App& app, const PixelView& v) {
	for (int y = 0; y + Stamp::CELL <= (int)v.rows; ++y) {
		for (int x = 0; x < 64; ++x) {
			// Left edge: the pixel to the left is not part of the white cell.
			if (SyncAt(v, x, y) && (x == 0 || !v.White(x - 1, y + Stamp::CELL / 2)) &&
				(y == 0 || !v.White(x + Stamp::CELL / 2, y - 1))) {
				app.stampX = x;
				app.stampY = y;
				std::printf("timestamp row found at x=%d y=%d\n", x, y);
				return true;
			}
		}
	}
	return false;
}

static void ReadStamp(App& app, const PixelView& v) {
	if (app.stampX < 0 && !LocateStamp(app, v)) {
		return;
	}
	if (!SyncAt(v, app.stampX, app.stampY)) {
		return;
	}

	uint64_t stamp = 0;
	const int cy = app.stampY + Stamp::CELL / 2;
	for (int i = 0; i < Stamp::BITS; ++i) {
		const int cx = app.stampX + (Stamp::SYNC_CELLS + i) * Stamp::CELL + Stamp::CELL / 2;
		stamp = (stamp << 1) | (v.White(cx, cy) ? 1 : 0);
	}

	const double age = (NowMicros() - (int64_t)stamp) / 1000.0;
	if (age >= 0 && age < 5000) {
		app.ages.push_back(age);
	}
}

////////////////////////////////////////////////////////////////////////////////
// PipeWire
////////////////////////////////////////////////////////////////////////////////

static uint32_t ToDrmFourcc(spa_video_format f) {
	switch (f) {
	case SPA_VIDEO_FORMAT_BGRx: return DRM_FORMAT_XRGB8888;
	case SPA_VIDEO_FORMAT_BGRA: return DRM_FORMAT_ARGB8888;
	case SPA_VIDEO_FORMAT_RGBx: return DRM_FORMAT_XBGR8888;
	case SPA_VIDEO_FORMAT_RGBA: return DRM_FORMAT_ABGR8888;
	default: return 0;
	}
}

// fixedSize: for a virtual output KWin takes the output's size from this
// negotiation (found with the game test: with a 1..16384 range and a default of
// 640x480 the virtual output became 640x480 whatever size had been requested).
static const spa_pod* BuildFormat(spa_pod_builder* b, spa_video_format format,
	const std::vector<uint64_t>& modifiers, const spa_rectangle* fixedSize) {
	spa_pod_frame formatFrame, modifierFrame;
	spa_rectangle defSize{ 640, 480 }, minSize{ 1, 1 }, maxSize{ 16384, 16384 };
	if (fixedSize) {
		defSize = minSize = maxSize = *fixedSize;
	}
	spa_fraction defRate{ 240, 1 }, minRate{ 0, 1 }, maxRate{ 1000, 1 };

	spa_pod_builder_push_object(b, &formatFrame, SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat);
	spa_pod_builder_add(b,
		SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video),
		SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
		SPA_FORMAT_VIDEO_format, SPA_POD_Id(format), 0);

	if (!modifiers.empty()) {
		spa_pod_builder_prop(b, SPA_FORMAT_VIDEO_modifier,
			SPA_POD_PROP_FLAG_MANDATORY | SPA_POD_PROP_FLAG_DONT_FIXATE);
		spa_pod_builder_push_choice(b, &modifierFrame, SPA_CHOICE_Enum, 0);
		spa_pod_builder_long(b, (int64_t)modifiers[0]);	// default value
		for (uint64_t m : modifiers) {
			spa_pod_builder_long(b, (int64_t)m);
		}
		spa_pod_builder_pop(b, &modifierFrame);
	}

	spa_pod_builder_add(b,
		SPA_FORMAT_VIDEO_size, SPA_POD_CHOICE_RANGE_Rectangle(&defSize, &minSize, &maxSize),
		SPA_FORMAT_VIDEO_framerate, SPA_POD_CHOICE_RANGE_Fraction(&defRate, &minRate, &maxRate), 0);
	return (const spa_pod*)spa_pod_builder_pop(b, &formatFrame);
}

static void OnStateChanged(void*, pw_stream_state, pw_stream_state state, const char* error) {
	std::printf("stream state: %s%s%s\n", pw_stream_state_as_string(state), error ? " error=" : "", error ? error : "");
	std::fflush(stdout);
}

static void OnParamChanged(void* data, uint32_t id, const spa_pod* param) {
	App& app = *(App*)data;
	if (!param || id != SPA_PARAM_Format) {
		return;
	}

	spa_format_video_raw_parse(param, &app.format);
	app.hasModifier = spa_pod_find_prop(param, nullptr, SPA_FORMAT_VIDEO_modifier) != nullptr;

	std::printf("format: %s %ux%u framerate=%u/%u max_framerate=%u/%u ",
		spa_debug_type_find_name(spa_type_video_format, app.format.format),
		app.format.size.width, app.format.size.height,
		app.format.framerate.num, app.format.framerate.denom,
		app.format.max_framerate.num, app.format.max_framerate.denom);
	if (app.hasModifier) {
		std::printf("modifier=0x%llx -> DMA-BUF\n", (unsigned long long)app.format.modifier);
	} else {
		std::printf("no modifier -> shared memory\n");
	}
	std::fflush(stdout);

	uint8_t buffer[1024];
	spa_pod_builder b;
	spa_pod_builder_init(&b, buffer, sizeof(buffer));

	const int dataTypes = app.hasModifier
		? (1 << SPA_DATA_DmaBuf)
		: ((1 << SPA_DATA_MemFd) | (1 << SPA_DATA_MemPtr));

#define CURSOR_META_SIZE(w, h) (sizeof(spa_meta_cursor) + sizeof(spa_meta_bitmap) + (w) * (h) * 4)
	const spa_pod* params[4];
	params[0] = (const spa_pod*)spa_pod_builder_add_object(&b,
		SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers,
		SPA_PARAM_BUFFERS_dataType, SPA_POD_Int(dataTypes));
	params[1] = (const spa_pod*)spa_pod_builder_add_object(&b,
		SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta,
		SPA_PARAM_META_type, SPA_POD_Id(SPA_META_Header),
		SPA_PARAM_META_size, SPA_POD_Int(sizeof(spa_meta_header)));
	params[2] = (const spa_pod*)spa_pod_builder_add_object(&b,
		SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta,
		SPA_PARAM_META_type, SPA_POD_Id(SPA_META_Cursor),
		SPA_PARAM_META_size, SPA_POD_CHOICE_RANGE_Int(
			CURSOR_META_SIZE(64, 64), CURSOR_META_SIZE(1, 1), CURSOR_META_SIZE(1024, 1024)));
	params[3] = (const spa_pod*)spa_pod_builder_add_object(&b,
		SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta,
		SPA_PARAM_META_type, SPA_POD_Id(SPA_META_VideoDamage),
		SPA_PARAM_META_size, SPA_POD_CHOICE_RANGE_Int(
			sizeof(spa_meta_region) * 16, sizeof(spa_meta_region) * 1, sizeof(spa_meta_region) * 16));
#undef CURSOR_META_SIZE

	pw_stream_update_params(app.stream, params, 4);
}

static void OnProcess(void* data) {
	App& app = *(App*)data;

	// Use the newest buffer, as Magpie's Graphics Capture source does.
	pw_buffer* pwBuffer = nullptr;
	while (pw_buffer* next = pw_stream_dequeue_buffer(app.stream)) {
		if (pwBuffer) {
			pw_stream_queue_buffer(app.stream, pwBuffer);
			++app.skipped;
		}
		pwBuffer = next;
	}
	if (!pwBuffer) {
		return;
	}

	spa_buffer* buffer = pwBuffer->buffer;
	spa_data& d = buffer->datas[0];

	// Cursor metadata.
	if (auto* cursor = (spa_meta_cursor*)spa_buffer_find_meta_data(buffer, SPA_META_Cursor, sizeof(spa_meta_cursor));
		cursor && spa_meta_cursor_is_valid(cursor)) {
		++app.cursorMetas;
		if (cursor->position.x != app.cursorPos.x || cursor->position.y != app.cursorPos.y) {
			++app.cursorMoves;
			app.cursorPos = cursor->position;
			if (app.inject) {
				MatchCursor(app, cursor->position.x + app.originX, cursor->position.y + app.originY);
			}
		}
		if (cursor->bitmap_offset >= sizeof(spa_meta_cursor)) {
			auto* bitmap = SPA_PTROFF(cursor, cursor->bitmap_offset, spa_meta_bitmap);
			if (bitmap->size.width > 0 && bitmap->size.height > 0) {
				++app.bitmaps;
				app.bitmapSize = bitmap->size;
			}
		}
	}

	if (auto* meta = spa_buffer_find_meta(buffer, SPA_META_VideoDamage)) {
		spa_meta_region* region;
		spa_meta_for_each(region, meta) {
			if (!spa_meta_region_is_valid(region)) {
				break;
			}
			++app.damageRects;
		}
	}

	auto* header = (spa_meta_header*)spa_buffer_find_meta_data(buffer, SPA_META_Header, sizeof(spa_meta_header));
	const bool corrupted = (d.chunk->flags & SPA_CHUNK_FLAG_CORRUPTED) ||
		(header && (header->flags & SPA_META_HEADER_FLAG_CORRUPTED));
	const bool hasImage = !corrupted && (d.type == SPA_DATA_DmaBuf || d.chunk->size > 0);

	if (!hasImage) {
		++app.cursorOnly;
	} else {
		++app.frames;
		++app.totalFrames;
		app.lastDataType = d.type;

		const uint32_t width = app.format.size.width;
		const uint32_t height = app.format.size.height;
		const uint32_t rows = std::min<uint32_t>(Stamp::ROWS, height);
		const uint32_t stride = d.chunk->stride ? (uint32_t)d.chunk->stride : width * 4;

		if (d.type == SPA_DATA_DmaBuf) {
			if (app.importer) {
				std::vector<uint8_t> pixels;
				double ms = 0;
				const std::string error = VkImporterReadback(app.importer, (int)d.fd, width, height,
					ToDrmFourcc(app.format.format), app.format.modifier, d.chunk->offset, stride, rows, pixels, ms);
				if (error.empty()) {
					app.importTimes.push_back(ms);
					ReadStamp(app, PixelView{ pixels.data(), width, rows, width * 4 });
				} else {
					app.importError = error;
				}
			} else if (!app.mmapTried || app.mmapOk) {
				// Without Vulkan: see whether the buffer can simply be mapped.
				const size_t size = (size_t)d.chunk->offset + (size_t)stride * rows;
				void* map = mmap(nullptr, size, PROT_READ, MAP_SHARED, (int)d.fd, 0);
				app.mmapOk = map != MAP_FAILED;
				if (!app.mmapTried) {
					std::printf("mmap of DMA-BUF: %s\n", app.mmapOk ? "works" : std::strerror(errno));
					app.mmapTried = true;
				}
				if (app.mmapOk) {
					ReadStamp(app, PixelView{ (const uint8_t*)map + d.chunk->offset, width, rows, stride });
					munmap(map, size);
				}
			}
		} else if (d.data) {
			const uint8_t* pixels = (const uint8_t*)d.data + d.chunk->offset;
			ReadStamp(app, PixelView{ pixels, width, rows, stride });

			if (!app.dumpPath.empty()) {
				app.lastFrame.resize((size_t)width * height * 4);
				for (uint32_t y = 0; y < height; ++y) {
					std::memcpy(app.lastFrame.data() + (size_t)y * width * 4, pixels + (size_t)y * stride, (size_t)width * 4);
				}
				app.lastFrameW = width;
				app.lastFrameH = height;
			}
		}
	}

	pw_stream_queue_buffer(app.stream, pwBuffer);
}

static void WriteDump(App& app, const std::string& path) {
	if (path.empty() || app.lastFrame.empty()) {
		return;
	}
	// The stream is BGRx or RGBx; PPM wants RGB.
	const bool bgr = app.format.format == SPA_VIDEO_FORMAT_BGRx || app.format.format == SPA_VIDEO_FORMAT_BGRA;
	if (FILE* f = std::fopen(path.c_str(), "wb")) {
		std::fprintf(f, "P6\n%u %u\n255\n", app.lastFrameW, app.lastFrameH);
		for (size_t i = 0; i < app.lastFrame.size(); i += 4) {
			const uint8_t* px = &app.lastFrame[i];
			const uint8_t rgb[3] = { bgr ? px[2] : px[0], px[1], bgr ? px[0] : px[2] };
			std::fwrite(rgb, 1, 3, f);
		}
		std::fclose(f);
		std::printf("wrote %s (%ux%u)\n", path.c_str(), app.lastFrameW, app.lastFrameH);
		std::fflush(stdout);
	}
}

// SIGUSR1: write the newest frame to <dump>.<n>.ppm without ending the run.
static void OnDumpSignal(void* data, int) {
	App& app = *(App*)data;
	WriteDump(app, app.dumpPath + "." + std::to_string(++app.dumpCount) + ".ppm");
}

static void Summary(const char* label, std::vector<double>& values) {
	if (values.empty()) {
		std::printf(" %s=n/a", label);
		return;
	}
	std::sort(values.begin(), values.end());
	std::printf(" %s=%.1f/%.1f/%.1f", label, values.front(), values[values.size() / 2], values.back());
	values.clear();
}

static void OnTimer(void* data, uint64_t) {
	App& app = *(App*)data;
	++app.elapsed;

	const char* type = app.lastDataType == SPA_DATA_DmaBuf ? "dmabuf"
		: app.lastDataType == SPA_DATA_MemFd ? "memfd"
		: app.lastDataType == SPA_DATA_MemPtr ? "memptr" : "-";
	std::printf("t=%d mono=%.2f frames=%d cursor_only=%d skipped=%d type=%s",
		app.elapsed, NowMicros() / 1e6, app.frames, app.cursorOnly, app.skipped, type);
	Summary("age_ms(min/med/max)", app.ages);
	if (app.inject) {
		std::printf(" lag_samples=%zu", app.cursorLag.size());
		Summary("cursor_lag_ms(min/med/max)", app.cursorLag);
	}
	if (app.importer) {
		Summary("vk_import_ms(min/med/max)", app.importTimes);
		if (!app.importError.empty()) {
			std::printf(" vk_error=\"%s\"", app.importError.c_str());
		}
	}
	std::printf(" cursor_meta=%d cursor_moves=%d bitmaps=%d pos=%d,%d bitmap=%ux%u damage_rects=%d\n",
		app.cursorMetas, app.cursorMoves, app.bitmaps, app.cursorPos.x, app.cursorPos.y,
		app.bitmapSize.width, app.bitmapSize.height, app.damageRects);
	std::fflush(stdout);

	app.frames = app.cursorOnly = app.skipped = 0;
	app.cursorMetas = app.cursorMoves = app.bitmaps = app.damageRects = 0;

	if (app.elapsed >= app.seconds) {
		pw_main_loop_quit(app.loop);
	}
}

////////////////////////////////////////////////////////////////////////////////
// Wayland
////////////////////////////////////////////////////////////////////////////////

static const wl_output_listener OUTPUT_LISTENER = {
	.geometry = [](void*, wl_output*, int32_t, int32_t, int32_t, int32_t, int32_t, const char*, const char*, int32_t) {},
	.mode = [](void*, wl_output*, uint32_t, int32_t, int32_t, int32_t) {},
	.done = [](void*, wl_output*) {},
	.scale = [](void*, wl_output*, int32_t) {},
	.name = [](void* d, wl_output*, const char* name) { ((Output*)d)->name = name; },
	.description = [](void*, wl_output*, const char*) {},
};

static const wl_registry_listener REGISTRY_LISTENER = {
	.global = [](void* data, wl_registry* registry, uint32_t name, const char* interface, uint32_t version) {
		App& app = *(App*)data;
		if (std::strcmp(interface, zkde_screencast_unstable_v1_interface.name) == 0) {
			// Version 5 still delivers the PipeWire node id in the "created" event.
			app.screencastVersion = std::min(version, 5u);
			app.screencast = (zkde_screencast_unstable_v1*)wl_registry_bind(
				registry, name, &zkde_screencast_unstable_v1_interface, app.screencastVersion);
		} else if (std::strcmp(interface, org_kde_kwin_fake_input_interface.name) == 0 && version >= 3) {
			app.fakeInput = (org_kde_kwin_fake_input*)wl_registry_bind(
				registry, name, &org_kde_kwin_fake_input_interface, std::min(version, 4u));
		} else if (std::strcmp(interface, wl_output_interface.name) == 0 && version >= 4) {
			auto* o = new Output;
			o->proxy = (wl_output*)wl_registry_bind(registry, name, &wl_output_interface, 4);
			wl_output_add_listener(o->proxy, &OUTPUT_LISTENER, o);
			app.outputs.push_back(o);
		}
	},
	.global_remove = [](void*, wl_registry*, uint32_t) {},
};

static const zkde_screencast_stream_unstable_v1_listener CAST_LISTENER = {
	.closed = [](void* data, zkde_screencast_stream_unstable_v1*) {
		App& app = *(App*)data;
		std::printf("KWin closed the stream\n");
		app.failed = true;
		if (app.loop) {
			pw_main_loop_quit(app.loop);
		}
	},
	.created = [](void* data, zkde_screencast_stream_unstable_v1*, uint32_t node) {
		((App*)data)->nodeId = node;
	},
	.failed = [](void* data, zkde_screencast_stream_unstable_v1*, const char* error) {
		std::printf("KWin refused the stream: %s\n", error);
		((App*)data)->failed = true;
	},
	.serial = [](void*, zkde_screencast_stream_unstable_v1*, uint32_t, uint32_t) {},
};

static void OnWaylandReadable(void* data, int, uint32_t) {
	App& app = *(App*)data;
	if (wl_display_dispatch(app.display) == -1) {
		std::printf("Wayland connection lost\n");
		pw_main_loop_quit(app.loop);
	}
	wl_display_flush(app.display);
}

////////////////////////////////////////////////////////////////////////////////

int main(int argc, char* argv[]) {
	App app;

	for (int i = 1; i < argc; ++i) {
		const std::string arg = argv[i];
		auto next = [&]() -> const char* { return i + 1 < argc ? argv[++i] : ""; };
		if (arg == "--window") {
			app.windowUuid = next();
		} else if (arg == "--output") {
			app.outputName = next();
		} else if (arg == "--virtual") {
			app.virtualName = next();
			std::sscanf(next(), "%dx%d", &app.virtualW, &app.virtualH);
		} else if (arg == "--cursor") {
			const std::string mode = next();
			app.cursorMode = mode == "hidden" ? ZKDE_SCREENCAST_UNSTABLE_V1_POINTER_HIDDEN
				: mode == "embedded" ? ZKDE_SCREENCAST_UNSTABLE_V1_POINTER_EMBEDDED
				: ZKDE_SCREENCAST_UNSTABLE_V1_POINTER_METADATA;
		} else if (arg == "--no-dmabuf") {
			app.offerDmabuf = false;
		} else if (arg == "--vk") {
			app.vkIndex = std::atoi(next());
		} else if (arg == "--seconds") {
			app.seconds = std::atoi(next());
		} else if (arg == "--dump") {
			app.dumpPath = next();
		} else if (arg == "--size") {
			std::sscanf(next(), "%dx%d", &app.sizeW, &app.sizeH);
		} else if (arg == "--inject-circle") {
			app.inject = true;
			app.circleX = std::atof(next());
			app.circleY = std::atof(next());
			app.circleR = std::max(20.0, std::atof(next()));
		} else if (arg == "--origin") {
			app.originX = std::atoi(next());
			app.originY = std::atoi(next());
		} else {
			std::fprintf(stderr, "unknown argument %s\n", arg.c_str());
			return 2;
		}
	}

	// ---- Wayland: ask KWin for the stream ----
	app.display = wl_display_connect(nullptr);
	if (!app.display) {
		std::fprintf(stderr, "cannot connect to the Wayland display\n");
		return 1;
	}
	wl_registry* registry = wl_display_get_registry(app.display);
	wl_registry_add_listener(registry, &REGISTRY_LISTENER, &app);
	wl_display_roundtrip(app.display);
	wl_display_roundtrip(app.display);

	if (!app.screencast) {
		std::printf("RESULT: zkde_screencast_unstable_v1 is not offered to this client\n");
		return 1;
	}
	std::printf("zkde_screencast_unstable_v1 bound at version %u\n", app.screencastVersion);

	if (!app.windowUuid.empty()) {
		app.castStream = zkde_screencast_unstable_v1_stream_window(
			app.screencast, app.windowUuid.c_str(), app.cursorMode);
	} else if (!app.outputName.empty()) {
		wl_output* output = nullptr;
		for (Output* o : app.outputs) {
			if (o->name == app.outputName) {
				output = o->proxy;
			}
		}
		if (!output) {
			std::fprintf(stderr, "no output named %s\n", app.outputName.c_str());
			return 1;
		}
		app.castStream = zkde_screencast_unstable_v1_stream_output(app.screencast, output, app.cursorMode);
	} else if (!app.virtualName.empty() && app.virtualW > 0 && app.virtualH > 0) {
		app.castStream = zkde_screencast_unstable_v1_stream_virtual_output(
			app.screencast, app.virtualName.c_str(), app.virtualW, app.virtualH,
			wl_fixed_from_double(1.0), app.cursorMode);
	} else {
		std::fprintf(stderr, "need --window, --output or --virtual\n");
		return 2;
	}

	zkde_screencast_stream_unstable_v1_add_listener(app.castStream, &CAST_LISTENER, &app);
	while (!app.nodeId && !app.failed) {
		if (wl_display_roundtrip(app.display) == -1) {
			std::printf("Wayland connection lost while waiting for the stream\n");
			return 1;
		}
	}
	if (app.failed) {
		return 1;
	}
	std::printf("RESULT: KWin created the stream without a prompt, PipeWire node %u\n", app.nodeId);
	std::fflush(stdout);

	// ---- Vulkan (optional) ----
	if (app.vkIndex >= 0) {
		app.importer = VkImporterCreate(app.vkIndex);
		if (!app.importer) {
			return 1;
		}
	}

	// ---- PipeWire ----
	// Block SIGUSR1 before PipeWire starts its threads, so that all of them
	// inherit the mask and the signal only arrives through the loop.
	sigset_t mask;
	sigemptyset(&mask);
	sigaddset(&mask, SIGUSR1);
	pthread_sigmask(SIG_BLOCK, &mask, nullptr);

	pw_init(&argc, &argv);
	app.loop = pw_main_loop_new(nullptr);
	pw_loop* loop = pw_main_loop_get_loop(app.loop);
	pw_loop_add_signal(loop, SIGUSR1, OnDumpSignal, &app);
	pw_context* context = pw_context_new(loop, nullptr, 0);
	pw_core* core = pw_context_connect(context, nullptr, 0);
	if (!core) {
		std::fprintf(stderr, "cannot connect to PipeWire\n");
		return 1;
	}

	app.stream = pw_stream_new(core, "magpie-spike-capture", pw_properties_new(
		PW_KEY_MEDIA_TYPE, "Video", PW_KEY_MEDIA_CATEGORY, "Capture", PW_KEY_MEDIA_ROLE, "Screen", nullptr));

	static pw_stream_events streamEvents{};
	streamEvents.version = PW_VERSION_STREAM_EVENTS;
	streamEvents.state_changed = OnStateChanged;
	streamEvents.param_changed = OnParamChanged;
	streamEvents.process = OnProcess;
	pw_stream_add_listener(app.stream, &app.streamListener, &streamEvents, &app);

	uint8_t buffer[8192];
	spa_pod_builder b;
	spa_pod_builder_init(&b, buffer, sizeof(buffer));
	std::vector<const spa_pod*> params;
	static const spa_video_format FORMATS[] = {
		SPA_VIDEO_FORMAT_BGRx, SPA_VIDEO_FORMAT_BGRA, SPA_VIDEO_FORMAT_RGBx, SPA_VIDEO_FORMAT_RGBA,
	};

	if (app.sizeW <= 0 && !app.virtualName.empty()) {
		app.sizeW = app.virtualW;
		app.sizeH = app.virtualH;
	}
	spa_rectangle wantedSize{ (uint32_t)app.sizeW, (uint32_t)app.sizeH };
	const spa_rectangle* fixedSize = app.sizeW > 0 ? &wantedSize : nullptr;

	if (app.offerDmabuf) {
		for (spa_video_format format : FORMATS) {
			std::vector<uint64_t> modifiers;
			if (app.importer) {
				modifiers = VkImporterModifiers(app.importer, ToDrmFourcc(format));
				if (format == SPA_VIDEO_FORMAT_BGRx) {
					std::printf("vk: %zu importable modifiers for BGRx:", modifiers.size());
					for (uint64_t m : modifiers) {
						std::printf(" 0x%llx", (unsigned long long)m);
					}
					std::printf("\n");
				}
			} else {
				modifiers = { DRM_FORMAT_MOD_INVALID, DRM_FORMAT_MOD_LINEAR };
			}
			if (!modifiers.empty()) {
				params.push_back(BuildFormat(&b, format, modifiers, fixedSize));
			}
		}
	}
	for (spa_video_format format : FORMATS) {
		params.push_back(BuildFormat(&b, format, {}, fixedSize));
	}

	if (pw_stream_connect(app.stream, PW_DIRECTION_INPUT, app.nodeId,
		(pw_stream_flags)(PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS),
		params.data(), (uint32_t)params.size()) < 0) {
		std::fprintf(stderr, "pw_stream_connect failed\n");
		return 1;
	}

	wl_display_flush(app.display);
	pw_loop_add_io(loop, wl_display_get_fd(app.display), SPA_IO_IN, false, OnWaylandReadable, &app);

	spa_source* timer = pw_loop_add_timer(loop, OnTimer, &app);
	timespec second{ 1, 0 };
	pw_loop_update_timer(loop, timer, &second, &second, false);

	if (app.inject) {
		if (!app.fakeInput) {
			std::printf("RESULT: org_kde_kwin_fake_input is not offered to this client\n");
			return 1;
		}
		org_kde_kwin_fake_input_authenticate(app.fakeInput, "Magpie feasibility test", "cursor latency test");
		spa_source* injectTimer = pw_loop_add_timer(loop, OnInject, &app);
		timespec step{ 0, 4'000'000 };
		pw_loop_update_timer(loop, injectTimer, &step, &step, false);
	}

	pw_main_loop_run(app.loop);

	std::printf("total frames with image data: %d in %d s\n", app.totalFrames, app.elapsed);

	WriteDump(app, app.dumpPath);

	pw_stream_destroy(app.stream);
	zkde_screencast_stream_unstable_v1_close(app.castStream);
	wl_display_roundtrip(app.display);
	pw_core_disconnect(core);
	pw_context_destroy(context);
	pw_main_loop_destroy(app.loop);
	return 0;
}
