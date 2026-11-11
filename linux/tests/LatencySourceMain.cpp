// A window that numbers its frames, for measuring how long a frame takes from
// being drawn to being shown. The square in the top left corner has the
// frame's number as its colour (red, green and blue are the three bytes of
// the number), which a scaler with MAGPIE_LATENCY_LOG reads from what it
// captures and notes with the time it showed the frame. This program notes
// when it drew each frame and when the compositor showed it on the output
// the window is on (the protocol presentation-time). A click changes the
// colour of the square in the middle with the next frame, and the click and
// that frame's number are noted.
//
// All times are nanoseconds of CLOCK_MONOTONIC. The lines of the log:
//   frame=NUMBER drawn=T committed=T presented=T refresh=NS flags=N
//   frame=NUMBER drawn=T committed=T presented=discarded
//   click seen=T frame=NUMBER
//
// Usage: magpie_latency_source [--display NAME] [--size WxH] [--fps N]
//        [--duration SECONDS] [--log FILE] [--title TEXT] [--app-id ID]
//   --fps: draw at this rate; otherwise whenever the compositor asks for a frame
#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <memory>
#include <string>
#include <string_view>
#include <vector>
#include <fcntl.h>
#include <linux/input-event-codes.h>
#include <poll.h>
#include <sys/mman.h>
#include <sys/timerfd.h>
#include <unistd.h>
#include <wayland-client.h>
#include "presentation-time-client-protocol.h"
#include "xdg-decoration-unstable-v1-client-protocol.h"
#include "xdg-shell-client-protocol.h"

namespace {

int64_t Now() {
	timespec time{};
	clock_gettime(CLOCK_MONOTONIC, &time);
	return (int64_t)time.tv_sec * 1'000'000'000 + time.tv_nsec;
}

struct Buffer {
	wl_buffer* buffer = nullptr;
	uint32_t* pixels = nullptr;
	bool isBusy = false;
};

struct Source;

// One frame the compositor was asked about
struct FrameRecord {
	Source* source = nullptr;
	uint32_t number = 0;
	int64_t drawn = 0;
	int64_t committed = 0;
};

struct Source {
	int width = 640;
	int height = 480;
	std::FILE* log = stdout;

	wl_display* display = nullptr;
	wl_compositor* compositor = nullptr;
	wl_shm* shm = nullptr;
	xdg_wm_base* wmBase = nullptr;
	zxdg_decoration_manager_v1* decorationManager = nullptr;
	wp_presentation* presentation = nullptr;
	uint32_t presentationClock = UINT32_MAX;
	wl_seat* seat = nullptr;
	wl_pointer* pointer = nullptr;
	wl_surface* surface = nullptr;
	xdg_toplevel* toplevel = nullptr;

	Buffer buffers[4];
	std::vector<std::unique_ptr<FrameRecord>> records;
	uint32_t frameNumber = 0;
	bool isConfigured = false;
	bool isClosed = false;
	// The compositor has asked for the next frame
	bool isFrameWanted = true;
	bool isCallbackPending = false;
	// A click that the next frame answers
	bool isClickPending = false;
	bool isResponseOn = false;

	bool CreateBuffers() {
		const size_t bytes = (size_t)width * height * 4;
		for (Buffer& buffer : buffers) {
			const int fd = memfd_create("magpie-latency-source", MFD_CLOEXEC);
			if (fd < 0 || ftruncate(fd, (off_t)bytes) != 0) {
				return false;
			}
			buffer.pixels = (uint32_t*)mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
			wl_shm_pool* pool = wl_shm_create_pool(shm, fd, (int32_t)bytes);
			buffer.buffer = wl_shm_pool_create_buffer(pool, 0, width, height, width * 4, WL_SHM_FORMAT_XRGB8888);
			wl_shm_pool_destroy(pool);
			close(fd);
			if (buffer.pixels == MAP_FAILED) {
				return false;
			}
			wl_buffer_add_listener(buffer.buffer, &BUFFER_LISTENER, &buffer);
		}
		return true;
	}

	void Fill(Buffer& buffer, int left, int top, int right, int bottom, uint32_t color) const {
		for (int y = std::max(top, 0); y < std::min(bottom, height); ++y) {
			std::fill(buffer.pixels + (size_t)y * width + std::max(left, 0),
				buffer.pixels + (size_t)y * width + std::min(right, width), color);
		}
	}

	// False if every buffer is still with the compositor
	bool Draw() {
		const auto free = std::find_if(std::begin(buffers), std::end(buffers), [](const Buffer& b) { return !b.isBusy; });
		if (free == std::end(buffers)) {
			return false;
		}
		Buffer& buffer = *free;

		auto record = std::make_unique<FrameRecord>();
		record->source = this;
		record->number = ++frameNumber;
		record->drawn = Now();

		if (isClickPending) {
			isClickPending = false;
			isResponseOn = !isResponseOn;
			std::fprintf(log, "click seen=%lld frame=%06x\n", (long long)clickSeen, record->number);
		}

		// A bar that moves, so that the whole picture is seen to change
		Fill(buffer, 0, 0, width, height, 0xff202428u);
		const int bar = (int)(frameNumber * 4 % (uint32_t)width);
		Fill(buffer, bar, 0, bar + 24, height, 0xff5080c0u);
		Fill(buffer, width / 2 - 32, height / 2 - 32, width / 2 + 32, height / 2 + 32, isResponseOn ? 0xfff0f0f0u : 0xffc03030u);
		Fill(buffer, 0, 0, 16, 16, 0xff000000u | record->number);

		if (!isCallbackPending) {
			isCallbackPending = true;
			wl_callback_add_listener(wl_surface_frame(surface), &FRAME_LISTENER, this);
		}
		const bool hasFeedback = presentation && presentationClock == (uint32_t)CLOCK_MONOTONIC;
		if (hasFeedback) {
			wp_presentation_feedback_add_listener(
				wp_presentation_feedback(presentation, surface), &FEEDBACK_LISTENER, record.get());
		}

		buffer.isBusy = true;
		wl_surface_attach(surface, buffer.buffer, 0, 0);
		wl_surface_damage_buffer(surface, 0, 0, width, height);
		wl_surface_commit(surface);
		wl_display_flush(display);
		record->committed = Now();
		if (hasFeedback) {
			records.push_back(std::move(record));
		} else {
			std::fprintf(log, "frame=%06x drawn=%lld committed=%lld\n",
				record->number, (long long)record->drawn, (long long)record->committed);
		}
		isFrameWanted = false;
		return true;
	}

	void Finish(FrameRecord* record, const char* presented) {
		std::fprintf(log, "frame=%06x drawn=%lld committed=%lld presented=%s\n",
			record->number, (long long)record->drawn, (long long)record->committed, presented);
		std::erase_if(records, [&](const std::unique_ptr<FrameRecord>& candidate) { return candidate.get() == record; });
	}

	int64_t clickSeen = 0;

	static constexpr wl_buffer_listener BUFFER_LISTENER{
		.release = [](void* data, wl_buffer*) { ((Buffer*)data)->isBusy = false; }
	};

	static constexpr wl_callback_listener FRAME_LISTENER{
		.done = [](void* data, wl_callback* callback, uint32_t) {
			Source& source = *(Source*)data;
			wl_callback_destroy(callback);
			source.isCallbackPending = false;
			source.isFrameWanted = true;
		}
	};

	static constexpr wp_presentation_feedback_listener FEEDBACK_LISTENER{
		.sync_output = [](void*, struct wp_presentation_feedback*, wl_output*) {},
		.presented = [](void* data, struct wp_presentation_feedback* feedback, uint32_t secondsHigh, uint32_t secondsLow,
			uint32_t nanoseconds, uint32_t refresh, uint32_t, uint32_t, uint32_t flags) {
			FrameRecord* record = (FrameRecord*)data;
			wp_presentation_feedback_destroy(feedback);
			const int64_t seconds = (int64_t)(((uint64_t)secondsHigh << 32) | secondsLow);
			char text[96];
			std::snprintf(text, sizeof(text), "%lld refresh=%u flags=%u",
				(long long)(seconds * 1'000'000'000 + nanoseconds), refresh, flags);
			record->source->Finish(record, text);
		},
		.discarded = [](void* data, struct wp_presentation_feedback* feedback) {
			FrameRecord* record = (FrameRecord*)data;
			wp_presentation_feedback_destroy(feedback);
			record->source->Finish(record, "discarded");
		}
	};

	static constexpr wp_presentation_listener PRESENTATION_LISTENER{
		.clock_id = [](void* data, wp_presentation*, uint32_t clock) { ((Source*)data)->presentationClock = clock; }
	};

	static constexpr wl_pointer_listener POINTER_LISTENER{
		.enter = [](void*, wl_pointer*, uint32_t, wl_surface*, wl_fixed_t, wl_fixed_t) {},
		.leave = [](void*, wl_pointer*, uint32_t, wl_surface*) {},
		.motion = [](void*, wl_pointer*, uint32_t, wl_fixed_t, wl_fixed_t) {},
		.button = [](void* data, wl_pointer*, uint32_t, uint32_t, uint32_t button, uint32_t state) {
			Source& source = *(Source*)data;
			if (button == BTN_LEFT && state == WL_POINTER_BUTTON_STATE_PRESSED) {
				source.isClickPending = true;
				source.clickSeen = Now();
			}
		},
		.axis = [](void*, wl_pointer*, uint32_t, uint32_t, wl_fixed_t) {},
		.frame = [](void*, wl_pointer*) {},
		.axis_source = [](void*, wl_pointer*, uint32_t) {},
		.axis_stop = [](void*, wl_pointer*, uint32_t, uint32_t) {},
		.axis_discrete = [](void*, wl_pointer*, uint32_t, int32_t) {}
	};

	static constexpr wl_seat_listener SEAT_LISTENER{
		.capabilities = [](void* data, wl_seat* seat, uint32_t capabilities) {
			Source& source = *(Source*)data;
			if ((capabilities & WL_SEAT_CAPABILITY_POINTER) && !source.pointer) {
				source.pointer = wl_seat_get_pointer(seat);
				wl_pointer_add_listener(source.pointer, &POINTER_LISTENER, &source);
			}
		},
		.name = [](void*, wl_seat*, const char*) {}
	};

	static constexpr xdg_wm_base_listener WM_BASE_LISTENER{
		.ping = [](void*, xdg_wm_base* wmBase, uint32_t serial) { xdg_wm_base_pong(wmBase, serial); }
	};

	static constexpr xdg_toplevel_listener TOPLEVEL_LISTENER{
		.configure = [](void*, xdg_toplevel*, int32_t, int32_t, wl_array*) {},
		.close = [](void* data, xdg_toplevel*) { ((Source*)data)->isClosed = true; },
		.configure_bounds = [](void*, xdg_toplevel*, int32_t, int32_t) {}
	};

	static constexpr xdg_surface_listener XDG_SURFACE_LISTENER{
		.configure = [](void* data, xdg_surface* xdgSurface, uint32_t serial) {
			xdg_surface_ack_configure(xdgSurface, serial);
			((Source*)data)->isConfigured = true;
		}
	};

	static constexpr wl_registry_listener REGISTRY_LISTENER{
		.global = [](void* data, wl_registry* registry, uint32_t name, const char* interface, uint32_t version) {
			Source& source = *(Source*)data;
			if (std::strcmp(interface, wl_compositor_interface.name) == 0) {
				source.compositor = (wl_compositor*)wl_registry_bind(registry, name, &wl_compositor_interface, 4);
			} else if (std::strcmp(interface, wl_shm_interface.name) == 0) {
				source.shm = (wl_shm*)wl_registry_bind(registry, name, &wl_shm_interface, 1);
			} else if (std::strcmp(interface, xdg_wm_base_interface.name) == 0) {
				source.wmBase = (xdg_wm_base*)wl_registry_bind(registry, name, &xdg_wm_base_interface, 2);
				xdg_wm_base_add_listener(source.wmBase, &WM_BASE_LISTENER, nullptr);
			} else if (std::strcmp(interface, zxdg_decoration_manager_v1_interface.name) == 0) {
				source.decorationManager = (zxdg_decoration_manager_v1*)wl_registry_bind(
					registry, name, &zxdg_decoration_manager_v1_interface, 1);
			} else if (std::strcmp(interface, wp_presentation_interface.name) == 0) {
				source.presentation = (wp_presentation*)wl_registry_bind(registry, name, &wp_presentation_interface, 1);
				wp_presentation_add_listener(source.presentation, &PRESENTATION_LISTENER, &source);
			} else if (std::strcmp(interface, wl_seat_interface.name) == 0 && !source.seat) {
				source.seat = (wl_seat*)wl_registry_bind(registry, name, &wl_seat_interface, std::min(version, 5u));
				wl_seat_add_listener(source.seat, &SEAT_LISTENER, &source);
			}
		},
		.global_remove = [](void*, wl_registry*, uint32_t) {}
	};
};

}

int main(int argc, char** argv) {
	Source source;
	std::string displayName;
	std::string title = "Magpie latency source";
	std::string appId = "magpie.latency";
	double duration = 0.0;
	double fps = 0.0;

	for (int i = 1; i < argc; ++i) {
		const std::string_view arg = argv[i];
		const auto value = [&]() -> const char* {
			if (i + 1 >= argc) {
				std::fprintf(stderr, "%s needs a value\n", argv[i]);
				std::exit(2);
			}
			return argv[++i];
		};
		if (arg == "--display") {
			displayName = value();
		} else if (arg == "--title") {
			title = value();
		} else if (arg == "--app-id") {
			appId = value();
		} else if (arg == "--size") {
			if (std::sscanf(value(), "%dx%d", &source.width, &source.height) != 2 || source.width < 64 || source.height < 64) {
				std::fprintf(stderr, "--size takes WIDTHxHEIGHT, at least 64x64\n");
				return 2;
			}
		} else if (arg == "--fps") {
			fps = std::atof(value());
		} else if (arg == "--duration") {
			duration = std::atof(value());
		} else if (arg == "--log") {
			source.log = std::fopen(value(), "w");
			if (!source.log) {
				std::fprintf(stderr, "The log cannot be written: %s\n", std::strerror(errno));
				return 1;
			}
		} else {
			std::fprintf(stderr, "Unknown option %s\n", argv[i]);
			return 2;
		}
	}

	// A line at a time, so that nothing is lost when the program is ended
	std::setvbuf(source.log, nullptr, _IOLBF, 0);

	source.display = wl_display_connect(displayName.empty() ? nullptr : displayName.c_str());
	if (!source.display) {
		std::fprintf(stderr, "No connection to the compositor\n");
		return 1;
	}
	wl_registry_add_listener(wl_display_get_registry(source.display), &Source::REGISTRY_LISTENER, &source);
	wl_display_roundtrip(source.display);
	wl_display_roundtrip(source.display);
	if (!source.compositor || !source.shm || !source.wmBase || !source.CreateBuffers()) {
		std::fprintf(stderr, "The compositor lacks an interface, or there is no memory for the pictures\n");
		return 1;
	}
	if (!source.presentation || source.presentationClock != (uint32_t)CLOCK_MONOTONIC) {
		std::fprintf(stderr, "The compositor does not say when it shows a frame; only the drawing is noted\n");
	}

	source.surface = wl_compositor_create_surface(source.compositor);
	xdg_surface* xdgSurface = xdg_wm_base_get_xdg_surface(source.wmBase, source.surface);
	xdg_surface_add_listener(xdgSurface, &Source::XDG_SURFACE_LISTENER, &source);
	source.toplevel = xdg_surface_get_toplevel(xdgSurface);
	xdg_toplevel_add_listener(source.toplevel, &Source::TOPLEVEL_LISTENER, &source);
	xdg_toplevel_set_title(source.toplevel, title.c_str());
	xdg_toplevel_set_app_id(source.toplevel, appId.c_str());
	// A window of a fixed size, which a tiling desktop lets float
	xdg_toplevel_set_min_size(source.toplevel, source.width, source.height);
	xdg_toplevel_set_max_size(source.toplevel, source.width, source.height);
	if (source.decorationManager) {
		zxdg_toplevel_decoration_v1_set_mode(
			zxdg_decoration_manager_v1_get_toplevel_decoration(source.decorationManager, source.toplevel),
			ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
	}
	wl_surface_commit(source.surface);

	// With a frame rate of its own the program draws when this timer fires
	int timer = -1;
	if (fps > 0.0) {
		timer = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
		const int64_t interval = (int64_t)(1e9 / fps);
		const itimerspec spec{
			.it_interval = { interval / 1'000'000'000, interval % 1'000'000'000 },
			.it_value = { interval / 1'000'000'000, interval % 1'000'000'000 }
		};
		timerfd_settime(timer, 0, &spec, nullptr);
	}

	const int64_t end = duration > 0.0 ? Now() + (int64_t)(duration * 1e9) : INT64_MAX;
	bool isTimerDue = false;
	while (!source.isClosed && Now() < end) {
		const bool isDue = timer >= 0 ? isTimerDue : source.isFrameWanted;
		if (source.isConfigured && isDue && source.Draw()) {
			isTimerDue = false;
		}

		while (wl_display_prepare_read(source.display) != 0) {
			wl_display_dispatch_pending(source.display);
		}
		wl_display_flush(source.display);
		pollfd fds[2]{ { wl_display_get_fd(source.display), POLLIN, 0 }, { timer, POLLIN, 0 } };
		// A frame that is due and has no free picture waits for the compositor
		poll(fds, timer >= 0 ? 2 : 1, 100);
		if (fds[0].revents & POLLIN) {
			wl_display_read_events(source.display);
		} else {
			wl_display_cancel_read(source.display);
		}
		if (wl_display_dispatch_pending(source.display) < 0) {
			break;
		}
		if (timer >= 0 && (fds[1].revents & POLLIN)) {
			uint64_t count = 0;
			if (read(timer, &count, sizeof(count)) > 0) {
				isTimerDue = true;
			}
		}
	}

	std::fflush(source.log);
	wl_display_disconnect(source.display);
	return 0;
}
