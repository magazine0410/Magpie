#include "WaylandWindow.h"
#include <cstdio>
#include <cstring>
#include <poll.h>
#include <wayland-client.h>
#include "xdg-decoration-unstable-v1-client-protocol.h"
#include "xdg-shell-client-protocol.h"

namespace Magpie {

struct WaylandWindow::Listeners {
	static constexpr wl_registry_listener REGISTRY{
		.global = [](void* data, wl_registry* registry, uint32_t name, const char* interface, uint32_t) {
			WaylandWindow& window = *(WaylandWindow*)data;
			if (std::strcmp(interface, wl_compositor_interface.name) == 0) {
				window._compositor = (wl_compositor*)wl_registry_bind(registry, name, &wl_compositor_interface, 4);
			} else if (std::strcmp(interface, xdg_wm_base_interface.name) == 0) {
				window._wmBase = (xdg_wm_base*)wl_registry_bind(registry, name, &xdg_wm_base_interface, 1);
			} else if (std::strcmp(interface, zxdg_decoration_manager_v1_interface.name) == 0) {
				window._decorationManager = (zxdg_decoration_manager_v1*)wl_registry_bind(
					registry, name, &zxdg_decoration_manager_v1_interface, 1);
			}
		},
		.global_remove = [](void*, wl_registry*, uint32_t) {}
	};

	static constexpr xdg_wm_base_listener WM_BASE{
		.ping = [](void*, xdg_wm_base* wmBase, uint32_t serial) { xdg_wm_base_pong(wmBase, serial); }
	};

	static constexpr xdg_surface_listener XDG_SURFACE{
		.configure = [](void* data, xdg_surface* xdgSurface, uint32_t serial) {
			xdg_surface_ack_configure(xdgSurface, serial);
			((WaylandWindow*)data)->_isConfigured = true;
		}
	};

	static constexpr xdg_toplevel_listener TOPLEVEL{
		.configure = [](void* data, xdg_toplevel*, int32_t width, int32_t height, wl_array*) {
			((WaylandWindow*)data)->_configuredSize = { width, height };
		},
		.close = [](void* data, xdg_toplevel*) { ((WaylandWindow*)data)->_isClosed = true; }
	};
};

WaylandWindow::~WaylandWindow() {
	// Closing the connection destroys everything that was created on it
	if (_display) {
		wl_display_disconnect(_display);
	}
}

bool WaylandWindow::Create(const std::string& title, const std::string& appId, bool fullscreen) noexcept {
	_display = wl_display_connect(nullptr);
	if (!_display) {
		std::fprintf(stderr, "Cannot connect to a Wayland compositor.\n");
		return false;
	}

	wl_registry* registry = wl_display_get_registry(_display);
	wl_registry_add_listener(registry, &Listeners::REGISTRY, this);
	wl_display_roundtrip(_display);
	if (!_compositor || !_wmBase) {
		std::fprintf(stderr, "The compositor lacks wl_compositor or xdg_wm_base.\n");
		return false;
	}
	xdg_wm_base_add_listener(_wmBase, &Listeners::WM_BASE, nullptr);

	_surface = wl_compositor_create_surface(_compositor);
	_xdgSurface = xdg_wm_base_get_xdg_surface(_wmBase, _surface);
	xdg_surface_add_listener(_xdgSurface, &Listeners::XDG_SURFACE, this);
	_toplevel = xdg_surface_get_toplevel(_xdgSurface);
	xdg_toplevel_add_listener(_toplevel, &Listeners::TOPLEVEL, this);
	xdg_toplevel_set_title(_toplevel, title.c_str());
	xdg_toplevel_set_app_id(_toplevel, appId.c_str());
	if (fullscreen) {
		xdg_toplevel_set_fullscreen(_toplevel, nullptr);
	}
	if (_decorationManager) {
		// Ask the compositor to draw the title bar
		zxdg_toplevel_decoration_v1* decoration = zxdg_decoration_manager_v1_get_toplevel_decoration(
			_decorationManager, _toplevel);
		zxdg_toplevel_decoration_v1_set_mode(decoration, ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
	}
	wl_surface_commit(_surface);

	while (!_isConfigured) {
		if (wl_display_dispatch(_display) < 0) {
			return false;
		}
	}
	return true;
}

bool WaylandWindow::PumpEvents() noexcept {
	while (wl_display_prepare_read(_display) != 0) {
		wl_display_dispatch_pending(_display);
	}
	wl_display_flush(_display);

	pollfd fd{ wl_display_get_fd(_display), POLLIN, 0 };
	if (poll(&fd, 1, 0) > 0) {
		wl_display_read_events(_display);
	} else {
		wl_display_cancel_read(_display);
	}
	return wl_display_dispatch_pending(_display) >= 0;
}

int WaylandWindow::EventFd() const noexcept {
	return wl_display_get_fd(_display);
}

}
