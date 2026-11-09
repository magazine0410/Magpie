// Tests T7 and T3: a layer-shell surface that covers one output, as Magpie's
// scaling window would in the "overlay in place" strategy.
//
// Usage: overlay --output NAME [--seconds N] [--alpha 0..255] [--input]
//   --alpha 255 (default) makes the surface opaque and declares it opaque to
//               the compositor, so everything beneath is fully occluded.
//   --input     keep the default input region (the overlay receives pointer
//               events). Without it the input region is empty, so input goes
//               to whatever is beneath.
//
// The surface is on the "overlay" layer, anchored to all edges, and never asks
// for keyboard focus. It always exits by itself after --seconds (default 8).
// Prints frames per second and the presentation feedback flags once a second.
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <sys/mman.h>
#include <unistd.h>
#include <vector>
#include <wayland-client.h>
#include "presentation-time-client-protocol.h"
// The generated header names a parameter "namespace", a C++ keyword.
#define namespace namespace_
#include "wlr-layer-shell-unstable-v1-client-protocol.h"
#undef namespace
#include "xdg-shell-client-protocol.h"

struct Output {
	wl_output* proxy = nullptr;
	std::string name;
};

struct Buffer {
	wl_buffer* proxy = nullptr;
	uint32_t* pixels = nullptr;
	bool busy = false;
	int barX = -1;
};

static wl_compositor* g_compositor;
static wl_shm* g_shm;
static zwlr_layer_shell_v1* g_layerShell;
static wp_presentation* g_presentation;
static wl_seat* g_seat;
static std::vector<Output*> g_outputs;

static wl_surface* g_surface;
static zwlr_layer_surface_v1* g_layerSurface;
static int g_width, g_height;
static Buffer g_buffers[3];
static uint32_t g_fill;
static bool g_configured, g_closed;
// Set before the surface is destroyed: late frame callbacks must not draw.
static bool g_closing;
static uint64_t g_frame;
static int g_framesThisSecond, g_presentedThisSecond, g_pointerEvents;
static uint32_t g_presentFlags, g_refreshNs;

static double Mono() {
	timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

static void DrawBar(Buffer& b, int x, uint32_t color) {
	for (int y = 0; y < g_height; ++y) {
		uint32_t* row = b.pixels + (size_t)y * g_width;
		for (int i = 0; i < 8 && x + i < g_width; ++i) {
			row[x + i] = color;
		}
	}
}

static const wl_buffer_listener BUFFER_LISTENER = {
	.release = [](void* data, wl_buffer*) { ((Buffer*)data)->busy = false; },
};

static void CreateBuffers() {
	const int stride = g_width * 4;
	const size_t size = (size_t)stride * g_height;
	const size_t total = size * std::size(g_buffers);

	const int fd = memfd_create("magpie-spike-overlay", MFD_CLOEXEC);
	if (fd < 0 || ftruncate(fd, (off_t)total) < 0) {
		std::perror("memfd");
		std::exit(1);
	}
	auto* map = (uint32_t*)mmap(nullptr, total, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	wl_shm_pool* pool = wl_shm_create_pool(g_shm, fd, (int32_t)total);

	for (size_t i = 0; i < std::size(g_buffers); ++i) {
		Buffer& b = g_buffers[i];
		b.pixels = map + i * (size / 4);
		std::fill_n(b.pixels, size / 4, g_fill);
		b.proxy = wl_shm_pool_create_buffer(pool, (int32_t)(i * size), g_width, g_height, stride,
			WL_SHM_FORMAT_ARGB8888);
		wl_buffer_add_listener(b.proxy, &BUFFER_LISTENER, &b);
	}
	wl_shm_pool_destroy(pool);
	close(fd);
}

static void Draw();

static const wl_callback_listener FRAME_LISTENER = {
	.done = [](void*, wl_callback* callback, uint32_t) {
		wl_callback_destroy(callback);
		++g_framesThisSecond;
		Draw();
	},
};

static const wp_presentation_feedback_listener FEEDBACK_LISTENER = {
	.sync_output = [](void*, struct wp_presentation_feedback*, wl_output*) {},
	.presented = [](void*, struct wp_presentation_feedback* feedback, uint32_t, uint32_t, uint32_t,
		uint32_t refresh, uint32_t, uint32_t, uint32_t flags) {
		++g_presentedThisSecond;
		g_presentFlags = flags;
		g_refreshNs = refresh;
		wp_presentation_feedback_destroy(feedback);
	},
	.discarded = [](void*, struct wp_presentation_feedback* feedback) { wp_presentation_feedback_destroy(feedback); },
};

static void Draw() {
	if (g_closing) {
		return;
	}
	Buffer* buffer = nullptr;
	for (Buffer& b : g_buffers) {
		if (!b.busy) {
			buffer = &b;
			break;
		}
	}
	if (!buffer) {
		return;
	}

	// A bar that moves every frame keeps the compositor repainting.
	const int x = (int)((g_frame * 4) % (uint64_t)std::max(1, g_width - 8));
	if (buffer->barX >= 0) {
		DrawBar(*buffer, buffer->barX, g_fill);
		wl_surface_damage_buffer(g_surface, buffer->barX, 0, 8, g_height);
	} else {
		wl_surface_damage_buffer(g_surface, 0, 0, g_width, g_height);
	}
	DrawBar(*buffer, x, 0xFFFFFF00);
	wl_surface_damage_buffer(g_surface, x, 0, 8, g_height);
	buffer->barX = x;
	buffer->busy = true;
	++g_frame;

	wl_callback_add_listener(wl_surface_frame(g_surface), &FRAME_LISTENER, nullptr);
	if (g_presentation) {
		wp_presentation_feedback_add_listener(
			wp_presentation_feedback(g_presentation, g_surface), &FEEDBACK_LISTENER, nullptr);
	}
	wl_surface_attach(g_surface, buffer->proxy, 0, 0);
	wl_surface_commit(g_surface);
}

static const zwlr_layer_surface_v1_listener LAYER_SURFACE_LISTENER = {
	.configure = [](void*, zwlr_layer_surface_v1* surface, uint32_t serial, uint32_t width, uint32_t height) {
		zwlr_layer_surface_v1_ack_configure(surface, serial);
		if (!g_configured) {
			g_width = (int)width;
			g_height = (int)height;
			g_configured = true;
			std::printf("configured %dx%d\n", g_width, g_height);
		}
	},
	.closed = [](void*, zwlr_layer_surface_v1*) { g_closed = true; },
};

static const wl_pointer_listener POINTER_LISTENER = {
	.enter = [](void*, wl_pointer*, uint32_t, wl_surface*, wl_fixed_t x, wl_fixed_t y) {
		std::printf("overlay pointer-enter x=%d y=%d\n", wl_fixed_to_int(x), wl_fixed_to_int(y));
	},
	.leave = [](void*, wl_pointer*, uint32_t, wl_surface*) { std::printf("overlay pointer-leave\n"); },
	.motion = [](void*, wl_pointer*, uint32_t, wl_fixed_t, wl_fixed_t) { ++g_pointerEvents; },
	.button = [](void*, wl_pointer*, uint32_t, uint32_t, uint32_t button, uint32_t state) {
		std::printf("overlay button=%u state=%u\n", button, state);
	},
	.axis = [](void*, wl_pointer*, uint32_t, uint32_t, wl_fixed_t) {},
	.frame = [](void*, wl_pointer*) {},
	.axis_source = [](void*, wl_pointer*, uint32_t) {},
	.axis_stop = [](void*, wl_pointer*, uint32_t, uint32_t) {},
	.axis_discrete = [](void*, wl_pointer*, uint32_t, int32_t) {},
	.axis_value120 = [](void*, wl_pointer*, uint32_t, int32_t) {},
	.axis_relative_direction = [](void*, wl_pointer*, uint32_t, uint32_t) {},
};

static const wl_output_listener OUTPUT_LISTENER = {
	.geometry = [](void*, wl_output*, int32_t, int32_t, int32_t, int32_t, int32_t, const char*, const char*, int32_t) {},
	.mode = [](void*, wl_output*, uint32_t, int32_t, int32_t, int32_t) {},
	.done = [](void*, wl_output*) {},
	.scale = [](void*, wl_output*, int32_t) {},
	.name = [](void* d, wl_output*, const char* name) { ((Output*)d)->name = name; },
	.description = [](void*, wl_output*, const char*) {},
};

static const wl_registry_listener REGISTRY_LISTENER = {
	.global = [](void*, wl_registry* registry, uint32_t name, const char* interface, uint32_t version) {
		if (std::strcmp(interface, wl_compositor_interface.name) == 0) {
			g_compositor = (wl_compositor*)wl_registry_bind(registry, name, &wl_compositor_interface, 4);
		} else if (std::strcmp(interface, wl_shm_interface.name) == 0) {
			g_shm = (wl_shm*)wl_registry_bind(registry, name, &wl_shm_interface, 1);
		} else if (std::strcmp(interface, zwlr_layer_shell_v1_interface.name) == 0) {
			g_layerShell = (zwlr_layer_shell_v1*)wl_registry_bind(
				registry, name, &zwlr_layer_shell_v1_interface, std::min(version, 4u));
		} else if (std::strcmp(interface, wp_presentation_interface.name) == 0) {
			g_presentation = (wp_presentation*)wl_registry_bind(registry, name, &wp_presentation_interface, 1);
		} else if (std::strcmp(interface, wl_seat_interface.name) == 0) {
			g_seat = (wl_seat*)wl_registry_bind(registry, name, &wl_seat_interface, std::min(version, 9u));
		} else if (std::strcmp(interface, wl_output_interface.name) == 0 && version >= 4) {
			auto* o = new Output;
			o->proxy = (wl_output*)wl_registry_bind(registry, name, &wl_output_interface, 4);
			wl_output_add_listener(o->proxy, &OUTPUT_LISTENER, o);
			g_outputs.push_back(o);
		}
	},
	.global_remove = [](void*, wl_registry*, uint32_t) {},
};

int main(int argc, char* argv[]) {
	std::string outputName;
	int seconds = 8;
	int alpha = 255;
	bool acceptInput = false;

	for (int i = 1; i < argc; ++i) {
		const std::string arg = argv[i];
		auto next = [&]() -> const char* { return i + 1 < argc ? argv[++i] : ""; };
		if (arg == "--output") {
			outputName = next();
		} else if (arg == "--seconds") {
			seconds = std::atoi(next());
		} else if (arg == "--alpha") {
			alpha = std::clamp(std::atoi(next()), 0, 255);
		} else if (arg == "--input") {
			acceptInput = true;
		} else {
			std::fprintf(stderr, "unknown argument %s\n", arg.c_str());
			return 2;
		}
	}
	// A wrong value must not leave a surface covering the screen for long.
	seconds = std::clamp(seconds, 1, 60);

	wl_display* display = wl_display_connect(nullptr);
	if (!display) {
		std::fprintf(stderr, "cannot connect to the Wayland display\n");
		return 1;
	}
	wl_registry_add_listener(wl_display_get_registry(display), &REGISTRY_LISTENER, nullptr);
	wl_display_roundtrip(display);
	wl_display_roundtrip(display);

	if (!g_layerShell) {
		std::printf("RESULT: zwlr_layer_shell_v1 is not offered\n");
		return 1;
	}

	wl_output* output = nullptr;
	for (Output* o : g_outputs) {
		if (o->name == outputName) {
			output = o->proxy;
		}
	}
	if (!output) {
		std::fprintf(stderr, "no output named \"%s\"\n", outputName.c_str());
		return 1;
	}

	// Premultiplied ARGB: a dark blue tint.
	g_fill = ((uint32_t)alpha << 24) | ((uint32_t)(0x10 * alpha / 255) << 16) |
		((uint32_t)(0x20 * alpha / 255) << 8) | (uint32_t)(0x60 * alpha / 255);

	g_surface = wl_compositor_create_surface(g_compositor);
	g_layerSurface = zwlr_layer_shell_v1_get_layer_surface(
		g_layerShell, g_surface, output, ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY, "magpie-spike-overlay");
	zwlr_layer_surface_v1_add_listener(g_layerSurface, &LAYER_SURFACE_LISTENER, nullptr);
	zwlr_layer_surface_v1_set_anchor(g_layerSurface,
		ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP | ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM |
		ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT | ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
	zwlr_layer_surface_v1_set_size(g_layerSurface, 0, 0);
	// -1: do not move for panels and do not reserve space.
	zwlr_layer_surface_v1_set_exclusive_zone(g_layerSurface, -1);
	zwlr_layer_surface_v1_set_keyboard_interactivity(
		g_layerSurface, ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE);

	if (acceptInput) {
		if (g_seat) {
			wl_pointer_add_listener(wl_seat_get_pointer(g_seat), &POINTER_LISTENER, nullptr);
		}
	} else {
		wl_region* empty = wl_compositor_create_region(g_compositor);
		wl_surface_set_input_region(g_surface, empty);
		wl_region_destroy(empty);
	}

	wl_surface_commit(g_surface);
	while (!g_configured && wl_display_dispatch(display) != -1) {
	}

	if (alpha == 255) {
		wl_region* opaque = wl_compositor_create_region(g_compositor);
		wl_region_add(opaque, 0, 0, g_width, g_height);
		wl_surface_set_opaque_region(g_surface, opaque);
		wl_region_destroy(opaque);
	}

	CreateBuffers();
	Draw();

	const double start = Mono();
	double nextReport = start + 1;
	while (!g_closed && wl_display_dispatch(display) != -1) {
		const double now = Mono();
		if (now >= nextReport) {
			std::printf("overlay mono=%.2f fps=%d presented=%d refresh_hz=%.1f flags=0x%x%s pointer_events=%d\n",
				now, g_framesThisSecond, g_presentedThisSecond, g_refreshNs ? 1e9 / g_refreshNs : 0.0,
				g_presentFlags, (g_presentFlags & WP_PRESENTATION_FEEDBACK_KIND_ZERO_COPY) ? "(zero-copy)" : "",
				g_pointerEvents);
			std::fflush(stdout);
			g_framesThisSecond = g_presentedThisSecond = g_pointerEvents = 0;
			nextReport += 1;
		}
		if (now - start >= seconds) {
			break;
		}
	}

	g_closing = true;
	zwlr_layer_surface_v1_destroy(g_layerSurface);
	wl_surface_destroy(g_surface);
	wl_display_roundtrip(display);
	std::printf("overlay closed\n");
	return 0;
}
