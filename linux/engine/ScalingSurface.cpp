#include "ScalingSurface.h"
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <linux/input-event-codes.h>
#include <poll.h>
#include <utility>
#include <vector>
#include <wayland-client.h>
#include <wayland-cursor.h>
#include "Logger.h"
#include "xdg-shell-client-protocol.h"
#include "xdg-decoration-unstable-v1-client-protocol.h"
// The generated header names a parameter "namespace", which is a keyword in C++
#define namespace namespace_
#include "wlr-layer-shell-unstable-v1-client-protocol.h"
#undef namespace

namespace Magpie {

struct ScalingSurface::State {
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
	xdg_wm_base* wmBase = nullptr;
	zxdg_decoration_manager_v1* decorationManager = nullptr;
	wl_seat* seat = nullptr;
	wl_pointer* pointer = nullptr;
	wl_shm* shm = nullptr;
	// The arrow of the desktop's cursor theme, loaded when it is first shown
	wl_cursor_theme* cursorTheme = nullptr;
	wl_surface* cursorSurface = nullptr;
	Point cursorHotSpot;
	bool isCursorShown = false;
	uint32_t pointerEnterSerial = 0;
	std::vector<std::unique_ptr<Output>> outputs;

	wl_surface* surface = nullptr;
	zwlr_layer_surface_v1* layerSurface = nullptr;
	xdg_surface* xdgSurface = nullptr;
	xdg_toplevel* toplevel = nullptr;

	Size size;
	// What the last toplevel configure event proposed; applied when the
	// configure sequence ends
	Size proposedSize;
	bool isProposedSizeFixed = false;
	bool isSizeFixed = false;
	// The user is resizing the window, and the size it had when that began
	bool isProposedResizing = false;
	bool isResizing = false;
	Size resizeStartSize;
	// The directions in which the user has changed the size since then
	bool isWidthResized = false;
	bool isHeightResized = false;
	SizeRule sizeRule;
	bool isConfigured = false;
	bool isClosed = false;
	bool isPointerInside = false;
	Point pointerPos;
	uint32_t pointerMotionCount = 0;
	std::vector<PointerEvent> pointerEvents;

	bool LoadCursor() noexcept {
		if (cursorSurface) {
			return true;
		}
		if (!shm) {
			return false;
		}

		// The library reads XCURSOR_THEME itself when the name is null
		int size = 24;
		if (const char* value = std::getenv("XCURSOR_SIZE")) {
			size = std::max(std::atoi(value), 8);
		}
		cursorTheme = wl_cursor_theme_load(nullptr, size, shm);
		if (!cursorTheme) {
			return false;
		}
		wl_cursor* cursor = wl_cursor_theme_get_cursor(cursorTheme, "default");
		if (!cursor) {
			cursor = wl_cursor_theme_get_cursor(cursorTheme, "left_ptr");
		}
		if (!cursor || cursor->image_count == 0) {
			return false;
		}

		wl_cursor_image* image = cursor->images[0];
		cursorHotSpot = { (int32_t)image->hotspot_x, (int32_t)image->hotspot_y };
		cursorSurface = wl_compositor_create_surface(compositor);
		wl_surface_attach(cursorSurface, wl_cursor_image_get_buffer(image), 0, 0);
		wl_surface_damage(cursorSurface, 0, 0, (int32_t)image->width, (int32_t)image->height);
		wl_surface_commit(cursorSurface);
		return true;
	}

	// Gives the pointer the image it is to have on this surface
	void ApplyCursor() noexcept {
		if (!pointer || !isPointerInside) {
			return;
		}
		if (isCursorShown && LoadCursor()) {
			wl_pointer_set_cursor(pointer, pointerEnterSerial, cursorSurface, cursorHotSpot.x, cursorHotSpot.y);
		} else {
			wl_pointer_set_cursor(pointer, pointerEnterSerial, nullptr, 0, 0);
		}
	}

	void SetOpaqueRegion() noexcept {
		// Nothing shows through; the compositor need not draw what is below
		wl_region* region = wl_compositor_create_region(compositor);
		wl_region_add(region, 0, 0, size.width, size.height);
		wl_surface_set_opaque_region(surface, region);
		wl_region_destroy(region);
	}

	bool Connect(const std::string& displayName) noexcept {
		display = wl_display_connect(displayName.empty() ? nullptr : displayName.c_str());
		if (!display) {
			Logger::Get().Error("Cannot connect to a Wayland compositor");
			return false;
		}

		wl_registry* registry = wl_display_get_registry(display);
		wl_registry_add_listener(registry, &REGISTRY_LISTENER, this);
		// The globals, then the names of the outputs and the capabilities of the seat
		if (wl_display_roundtrip(display) < 0 || wl_display_roundtrip(display) < 0) {
			Logger::Get().Error("The Wayland connection failed");
			return false;
		}
		if (!compositor) {
			Logger::Get().Error("The compositor lacks wl_compositor");
			return false;
		}
		return true;
	}

	bool WaitForConfigure() noexcept {
		while (!isConfigured && !isClosed) {
			if (wl_display_dispatch(display) < 0) {
				Logger::Get().Error("The Wayland connection failed");
				return false;
			}
		}
		if (isClosed || size.width <= 0 || size.height <= 0) {
			Logger::Get().Error("The compositor did not accept the surface for the scaled picture");
			return false;
		}
		SetOpaqueRegion();
		return true;
	}

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
		.enter = [](void* data, wl_pointer*, uint32_t serial, wl_surface*, wl_fixed_t x, wl_fixed_t y) {
			State& state = *(State*)data;
			state.isPointerInside = true;
			state.pointerPos = { wl_fixed_to_int(x), wl_fixed_to_int(y) };
			++state.pointerMotionCount;
			state.pointerEvents.push_back({ PointerEvent::Type::Motion, state.pointerPos });
			state.pointerEnterSerial = serial;
			state.ApplyCursor();
		},
		.leave = [](void* data, wl_pointer*, uint32_t, wl_surface*) {
			State& state = *(State*)data;
			state.isPointerInside = false;
			state.pointerEvents.push_back({ PointerEvent::Type::Leave, state.pointerPos });
		},
		.motion = [](void* data, wl_pointer*, uint32_t, wl_fixed_t x, wl_fixed_t y) {
			State& state = *(State*)data;
			state.pointerPos = { wl_fixed_to_int(x), wl_fixed_to_int(y) };
			++state.pointerMotionCount;
			state.pointerEvents.push_back({ PointerEvent::Type::Motion, state.pointerPos });
		},
		.button = [](void* data, wl_pointer*, uint32_t, uint32_t, uint32_t button, uint32_t buttonState) {
			State& state = *(State*)data;
			int index;
			switch (button) {
			case BTN_LEFT: index = 0; break;
			case BTN_RIGHT: index = 1; break;
			case BTN_MIDDLE: index = 2; break;
			default: return;
			}
			state.pointerEvents.push_back({
				buttonState == WL_POINTER_BUTTON_STATE_PRESSED ?
					PointerEvent::Type::ButtonDown : PointerEvent::Type::ButtonUp,
				state.pointerPos, index
			});
		},
		.axis = [](void* data, wl_pointer*, uint32_t, uint32_t axis, wl_fixed_t value) {
			State& state = *(State*)data;
			if (axis == WL_POINTER_AXIS_VERTICAL_SCROLL) {
				state.pointerEvents.push_back({
					PointerEvent::Type::Scroll, state.pointerPos, 0, (float)wl_fixed_to_double(value)
				});
			}
		},
		.frame = [](void*, wl_pointer*) {},
		.axis_source = [](void*, wl_pointer*, uint32_t) {},
		.axis_stop = [](void*, wl_pointer*, uint32_t, uint32_t) {},
		.axis_discrete = [](void*, wl_pointer*, uint32_t, int32_t) {}
	};

	static constexpr xdg_wm_base_listener WM_BASE_LISTENER{
		.ping = [](void*, xdg_wm_base* wmBase, uint32_t serial) { xdg_wm_base_pong(wmBase, serial); }
	};

	static constexpr xdg_toplevel_listener TOPLEVEL_LISTENER{
		.configure = [](void* data, xdg_toplevel*, int32_t width, int32_t height, wl_array* states) {
			State& state = *(State*)data;
			state.proposedSize = { width, height };
			state.isProposedSizeFixed = false;
			state.isProposedResizing = false;
			const uint32_t* begin = (const uint32_t*)states->data;
			for (const uint32_t* value = begin; value < begin + states->size / sizeof(uint32_t); ++value) {
				switch (*value) {
				case XDG_TOPLEVEL_STATE_MAXIMIZED:
				case XDG_TOPLEVEL_STATE_FULLSCREEN:
				case XDG_TOPLEVEL_STATE_TILED_LEFT:
				case XDG_TOPLEVEL_STATE_TILED_RIGHT:
				case XDG_TOPLEVEL_STATE_TILED_TOP:
				case XDG_TOPLEVEL_STATE_TILED_BOTTOM:
					state.isProposedSizeFixed = true;
					break;
				case XDG_TOPLEVEL_STATE_RESIZING:
					state.isProposedResizing = true;
					break;
				}
			}
		},
		.close = [](void* data, xdg_toplevel*) { ((State*)data)->isClosed = true; },
		.configure_bounds = [](void*, xdg_toplevel*, int32_t, int32_t) {}
	};

	static constexpr xdg_surface_listener XDG_SURFACE_LISTENER{
		.configure = [](void* data, xdg_surface* xdgSurface, uint32_t serial) {
			State& state = *(State*)data;
			const Size proposed = state.proposedSize;
			if (state.isProposedResizing && !state.isResizing) {
				state.resizeStartSize = state.size;
				state.isWidthResized = state.isHeightResized = false;
			}
			state.isResizing = state.isProposedResizing;

			// Zero: the size is ours to choose, so it stays
			if (proposed.width > 0 && proposed.height > 0) {
				state.isSizeFixed = state.isProposedSizeFixed;

				Size newSize = proposed;
				if (!state.isSizeFixed && state.sizeRule) {
					if (state.isResizing) {
						state.isWidthResized = state.isWidthResized || proposed.width != state.resizeStartSize.width;
						state.isHeightResized = state.isHeightResized || proposed.height != state.resizeStartSize.height;
						newSize = state.sizeRule(proposed, state.isWidthResized, state.isHeightResized);
					} else {
						newSize = state.sizeRule(proposed,
							proposed.width != state.size.width, proposed.height != state.size.height);
					}
				}
				if (newSize != state.size) {
					state.size = newSize;
					state.SetOpaqueRegion();
				}
			}
			xdg_surface_ack_configure(xdgSurface, serial);
			state.isConfigured = true;
		}
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
			} else if (std::strcmp(interface, wl_shm_interface.name) == 0) {
				state.shm = (wl_shm*)wl_registry_bind(registry, name, &wl_shm_interface, 1);
			} else if (std::strcmp(interface, xdg_wm_base_interface.name) == 0) {
				state.wmBase = (xdg_wm_base*)wl_registry_bind(registry, name, &xdg_wm_base_interface, 2);
				xdg_wm_base_add_listener(state.wmBase, &WM_BASE_LISTENER, nullptr);
			} else if (std::strcmp(interface, zxdg_decoration_manager_v1_interface.name) == 0) {
				state.decorationManager = (zxdg_decoration_manager_v1*)wl_registry_bind(
					registry, name, &zxdg_decoration_manager_v1_interface, 1);
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

ScalingSurface::ScalingSurface() noexcept : _state(std::make_unique<State>()) {}

ScalingSurface::~ScalingSurface() = default;

bool ScalingSurface::CreateLayer(
	const std::string& displayName,
	const std::string& outputName,
	bool aboveEverything
) noexcept {
	State& state = *_state;
	if (!state.Connect(displayName)) {
		return false;
	}
	if (!state.layerShell) {
		Logger::Get().Error("The compositor lacks the layer shell");
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
		aboveEverything ? ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY : ZWLR_LAYER_SHELL_V1_LAYER_TOP, "magpie-scaling");
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

	return state.WaitForConfigure();
}

bool ScalingSurface::CreateWindow(
	const std::string& displayName,
	const std::string& title,
	const std::string& appId,
	Size size,
	Size minSize,
	SizeRule sizeRule
) noexcept {
	State& state = *_state;
	if (!state.Connect(displayName)) {
		return false;
	}
	if (!state.wmBase) {
		Logger::Get().Error("The compositor lacks xdg_wm_base");
		return false;
	}

	state.size = size;
	state.sizeRule = std::move(sizeRule);

	state.surface = wl_compositor_create_surface(state.compositor);
	state.xdgSurface = xdg_wm_base_get_xdg_surface(state.wmBase, state.surface);
	xdg_surface_add_listener(state.xdgSurface, &State::XDG_SURFACE_LISTENER, &state);
	state.toplevel = xdg_surface_get_toplevel(state.xdgSurface);
	xdg_toplevel_add_listener(state.toplevel, &State::TOPLEVEL_LISTENER, &state);
	xdg_toplevel_set_title(state.toplevel, title.c_str());
	xdg_toplevel_set_app_id(state.toplevel, appId.c_str());
	xdg_toplevel_set_min_size(state.toplevel, minSize.width, minSize.height);

	if (state.decorationManager) {
		// Title bar and borders from the desktop
		zxdg_toplevel_decoration_v1* decoration = zxdg_decoration_manager_v1_get_toplevel_decoration(
			state.decorationManager, state.toplevel);
		zxdg_toplevel_decoration_v1_set_mode(decoration, ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
	} else {
		Logger::Get().Warn("The desktop draws no title bars; the scaled window has none");
	}
	wl_surface_commit(state.surface);

	return state.WaitForConfigure();
}

bool ScalingSurface::PumpEvents() noexcept {
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

int ScalingSurface::EventFd() const noexcept {
	return wl_display_get_fd(_state->display);
}

wl_display* ScalingSurface::Display() const noexcept {
	return _state->display;
}

wl_surface* ScalingSurface::Surface() const noexcept {
	return _state->surface;
}

Size ScalingSurface::GetSize() const noexcept {
	return _state->size;
}

bool ScalingSurface::IsClosed() const noexcept {
	return _state->isClosed;
}

void ScalingSurface::SetCursorShown(bool shown) noexcept {
	if (_state->isCursorShown == shown) {
		return;
	}
	_state->isCursorShown = shown;
	_state->ApplyCursor();
	wl_display_flush(_state->display);
}

bool ScalingSurface::IsSizeFixed() const noexcept {
	return _state->isSizeFixed;
}

uint32_t ScalingSurface::PointerMotionCount() const noexcept {
	return _state->pointerMotionCount;
}

std::vector<ScalingSurface::PointerEvent> ScalingSurface::TakePointerEvents() noexcept {
	return std::exchange(_state->pointerEvents, {});
}

bool ScalingSurface::IsPointerInside() const noexcept {
	return _state->isPointerInside;
}

Point ScalingSurface::PointerPos() const noexcept {
	return _state->pointerPos;
}

}
