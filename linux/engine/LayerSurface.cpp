#include "LayerSurface.h"
#include <algorithm>
#include <cstring>
#include <poll.h>
#include <vector>
#include <wayland-client.h>
#include "Logger.h"
// The generated header names a parameter "namespace", which is a keyword in C++
#define namespace namespace_
#include "wlr-layer-shell-unstable-v1-client-protocol.h"
#undef namespace

namespace Magpie {

struct LayerSurface::State {
	struct Output {
		wl_output* proxy = nullptr;
		std::string name;
	};

	~State() {
		// Closing the connection destroys everything that was created on it
		if (display) {
			wl_display_disconnect(display);
		}
	}

	wl_display* display = nullptr;
	wl_compositor* compositor = nullptr;
	zwlr_layer_shell_v1* layerShell = nullptr;
	wl_seat* seat = nullptr;
	wl_pointer* pointer = nullptr;
	std::vector<std::unique_ptr<Output>> outputs;

	wl_surface* surface = nullptr;
	zwlr_layer_surface_v1* layerSurface = nullptr;

	Size size;
	bool isConfigured = false;
	bool isClosed = false;
	bool isPointerInside = false;
	Point pointerPos;

	static constexpr wl_output_listener OUTPUT_LISTENER{
		.geometry = [](void*, wl_output*, int32_t, int32_t, int32_t, int32_t, int32_t,
			const char*, const char*, int32_t) {},
		.mode = [](void*, wl_output*, uint32_t, int32_t, int32_t, int32_t) {},
		.done = [](void*, wl_output*) {},
		.scale = [](void*, wl_output*, int32_t) {},
		.name = [](void* data, wl_output*, const char* name) { ((Output*)data)->name = name; },
		.description = [](void*, wl_output*, const char*) {}
	};

	static constexpr wl_pointer_listener POINTER_LISTENER{
		.enter = [](void* data, wl_pointer* pointer, uint32_t serial, wl_surface*, wl_fixed_t x, wl_fixed_t y) {
			State& state = *(State*)data;
			state.isPointerInside = true;
			state.pointerPos = { wl_fixed_to_int(x), wl_fixed_to_int(y) };
			// No cursor image on this surface
			wl_pointer_set_cursor(pointer, serial, nullptr, 0, 0);
		},
		.leave = [](void* data, wl_pointer*, uint32_t, wl_surface*) {
			((State*)data)->isPointerInside = false;
		},
		.motion = [](void* data, wl_pointer*, uint32_t, wl_fixed_t x, wl_fixed_t y) {
			((State*)data)->pointerPos = { wl_fixed_to_int(x), wl_fixed_to_int(y) };
		},
		.button = [](void*, wl_pointer*, uint32_t, uint32_t, uint32_t, uint32_t) {},
		.axis = [](void*, wl_pointer*, uint32_t, uint32_t, wl_fixed_t) {},
		.frame = [](void*, wl_pointer*) {},
		.axis_source = [](void*, wl_pointer*, uint32_t) {},
		.axis_stop = [](void*, wl_pointer*, uint32_t, uint32_t) {},
		.axis_discrete = [](void*, wl_pointer*, uint32_t, int32_t) {}
	};

	static constexpr wl_seat_listener SEAT_LISTENER{
		.capabilities = [](void* data, wl_seat* seat, uint32_t capabilities) {
			State& state = *(State*)data;
			if ((capabilities & WL_SEAT_CAPABILITY_POINTER) && !state.pointer) {
				state.pointer = wl_seat_get_pointer(seat);
				wl_pointer_add_listener(state.pointer, &POINTER_LISTENER, &state);
			}
		},
		.name = [](void*, wl_seat*, const char*) {}
	};

	static constexpr wl_registry_listener REGISTRY_LISTENER{
		.global = [](void* data, wl_registry* registry, uint32_t name, const char* interface, uint32_t version) {
			State& state = *(State*)data;
			if (std::strcmp(interface, wl_compositor_interface.name) == 0) {
				state.compositor = (wl_compositor*)wl_registry_bind(registry, name, &wl_compositor_interface, 4);
			} else if (std::strcmp(interface, zwlr_layer_shell_v1_interface.name) == 0) {
				state.layerShell = (zwlr_layer_shell_v1*)wl_registry_bind(
					registry, name, &zwlr_layer_shell_v1_interface, 1);
			} else if (std::strcmp(interface, wl_seat_interface.name) == 0 && !state.seat) {
				// Version 5 has the frame event, which ends the listener above
				state.seat = (wl_seat*)wl_registry_bind(registry, name, &wl_seat_interface, std::min(version, 5u));
				wl_seat_add_listener(state.seat, &SEAT_LISTENER, &state);
			} else if (std::strcmp(interface, wl_output_interface.name) == 0 && version >= 4) {
				auto output = std::make_unique<Output>();
				output->proxy = (wl_output*)wl_registry_bind(registry, name, &wl_output_interface, 4);
				wl_output_add_listener(output->proxy, &OUTPUT_LISTENER, output.get());
				state.outputs.push_back(std::move(output));
			}
		},
		.global_remove = [](void*, wl_registry*, uint32_t) {}
	};

	static constexpr zwlr_layer_surface_v1_listener LAYER_SURFACE_LISTENER{
		.configure = [](void* data, zwlr_layer_surface_v1* layerSurface, uint32_t serial, uint32_t width, uint32_t height) {
			State& state = *(State*)data;
			zwlr_layer_surface_v1_ack_configure(layerSurface, serial);
			state.size = { (int32_t)width, (int32_t)height };
			state.isConfigured = true;
		},
		.closed = [](void* data, zwlr_layer_surface_v1*) {
			((State*)data)->isClosed = true;
		}
	};
};

LayerSurface::LayerSurface() noexcept : _state(std::make_unique<State>()) {}

LayerSurface::~LayerSurface() = default;

bool LayerSurface::Create(const std::string& outputName) noexcept {
	State& state = *_state;

	state.display = wl_display_connect(nullptr);
	if (!state.display) {
		Logger::Get().Error("Cannot connect to a Wayland compositor");
		return false;
	}

	wl_registry* registry = wl_display_get_registry(state.display);
	wl_registry_add_listener(registry, &State::REGISTRY_LISTENER, &state);
	// The globals, then the names of the outputs and the capabilities of the seat
	if (wl_display_roundtrip(state.display) < 0 || wl_display_roundtrip(state.display) < 0) {
		Logger::Get().Error("The Wayland connection failed");
		return false;
	}
	if (!state.compositor || !state.layerShell) {
		Logger::Get().Error("The compositor lacks wl_compositor or the layer shell");
		return false;
	}

	wl_output* output = nullptr;
	for (const std::unique_ptr<State::Output>& candidate : state.outputs) {
		if (candidate->name == outputName) {
			output = candidate->proxy;
		}
	}
	if (!output) {
		Logger::Get().Error("There is no output named " + outputName);
		return false;
	}

	state.surface = wl_compositor_create_surface(state.compositor);
	state.layerSurface = zwlr_layer_shell_v1_get_layer_surface(state.layerShell, state.surface, output,
		ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY, "magpie-scaling");
	zwlr_layer_surface_v1_add_listener(state.layerSurface, &State::LAYER_SURFACE_LISTENER, &state);
	// Anchored to all four edges with no size of its own: the whole output
	zwlr_layer_surface_v1_set_anchor(state.layerSurface,
		ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP | ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM |
		ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT | ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
	// -1: also over the areas that panels have reserved
	zwlr_layer_surface_v1_set_exclusive_zone(state.layerSurface, -1);
	zwlr_layer_surface_v1_set_keyboard_interactivity(state.layerSurface,
		ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE);
	wl_surface_commit(state.surface);

	while (!state.isConfigured && !state.isClosed) {
		if (wl_display_dispatch(state.display) < 0) {
			Logger::Get().Error("The Wayland connection failed");
			return false;
		}
	}
	if (state.isClosed || state.size.width <= 0 || state.size.height <= 0) {
		Logger::Get().Error("The compositor did not accept the surface for the scaled picture");
		return false;
	}

	// Nothing shows through; the compositor need not draw what is below
	wl_region* region = wl_compositor_create_region(state.compositor);
	wl_region_add(region, 0, 0, state.size.width, state.size.height);
	wl_surface_set_opaque_region(state.surface, region);
	wl_region_destroy(region);

	return true;
}

bool LayerSurface::PumpEvents() noexcept {
	wl_display* display = _state->display;
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
	return wl_display_dispatch_pending(display) >= 0;
}

int LayerSurface::EventFd() const noexcept {
	return wl_display_get_fd(_state->display);
}

wl_display* LayerSurface::Display() const noexcept {
	return _state->display;
}

wl_surface* LayerSurface::Surface() const noexcept {
	return _state->surface;
}

Size LayerSurface::GetSize() const noexcept {
	return _state->size;
}

bool LayerSurface::IsClosed() const noexcept {
	return _state->isClosed;
}

bool LayerSurface::IsPointerInside() const noexcept {
	return _state->isPointerInside;
}

Point LayerSurface::PointerPos() const noexcept {
	return _state->pointerPos;
}

}
