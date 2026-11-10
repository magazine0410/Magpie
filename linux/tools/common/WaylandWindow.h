// A plain window on a Wayland compositor, for the tools that show something:
// an xdg_toplevel with a title bar drawn by the compositor. It only creates
// the surface; a Vulkan swap chain provides the content.
#pragma once
#include <string>
#include "Geometry.h"

struct wl_compositor;
struct wl_display;
struct wl_surface;
struct xdg_surface;
struct xdg_toplevel;
struct xdg_wm_base;
struct zxdg_decoration_manager_v1;

namespace Magpie {

class WaylandWindow {
public:
	WaylandWindow() = default;
	WaylandWindow(const WaylandWindow&) = delete;
	WaylandWindow(WaylandWindow&&) = delete;
	~WaylandWindow();

	// Connects to the compositor and creates the window. `appId` identifies
	// the program to the desktop.
	bool Create(const std::string& title, const std::string& appId, bool fullscreen) noexcept;

	// Processes the events that have arrived without waiting for more.
	// Returns false when the connection is lost.
	bool PumpEvents() noexcept;

	wl_display* Display() const noexcept {
		return _display;
	}

	wl_surface* Surface() const noexcept {
		return _surface;
	}

	// Readable when PumpEvents has something to do
	int EventFd() const noexcept;

	// The size the compositor asks for, or zero in a direction it leaves to
	// the program
	Size ConfiguredSize() const noexcept {
		return _configuredSize;
	}

	// The user has asked to close the window
	bool IsClosed() const noexcept {
		return _isClosed;
	}

private:
	struct Listeners;

	wl_display* _display = nullptr;
	wl_compositor* _compositor = nullptr;
	xdg_wm_base* _wmBase = nullptr;
	zxdg_decoration_manager_v1* _decorationManager = nullptr;
	wl_surface* _surface = nullptr;
	xdg_surface* _xdgSurface = nullptr;
	xdg_toplevel* _toplevel = nullptr;

	Size _configuredSize;
	bool _isConfigured = false;
	bool _isClosed = false;
};

}
