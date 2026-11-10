// The surface that shows the scaled picture: it covers one output above all
// windows and never takes keyboard focus, so that the source stays the active
// window. Linux counterpart of the scaling window's styles on Windows
// (topmost, no-activate). It uses wlr-layer-shell, which KWin and the wlroots
// compositors implement.
//
// It only creates the surface; a Vulkan swap chain provides the content.
#pragma once
#include <memory>
#include <string>
#include "Geometry.h"

struct wl_display;
struct wl_surface;

namespace Magpie {

class LayerSurface {
public:
	LayerSurface() noexcept;
	LayerSurface(const LayerSurface&) = delete;
	LayerSurface(LayerSurface&&) = delete;
	~LayerSurface();

	// Opens a Wayland connection of its own and covers the output with this
	// name.
	bool Create(const std::string& outputName) noexcept;

	// Processes the events that have arrived without waiting for more.
	// Returns false when the connection is lost.
	bool PumpEvents() noexcept;

	// Readable when PumpEvents has something to do
	int EventFd() const noexcept;

	wl_display* Display() const noexcept;

	wl_surface* Surface() const noexcept;

	// The size the compositor gave the surface: that of the output
	Size GetSize() const noexcept;

	// The compositor has removed the surface, for example with its output
	bool IsClosed() const noexcept;

	// The pointer is on the surface. Its image is hidden there: either the
	// scaled cursor is drawn, or the pointer is about to be sent to the source.
	bool IsPointerInside() const noexcept;

	// Relative to the surface; valid while the pointer is inside
	Point PointerPos() const noexcept;

private:
	struct State;
	std::unique_ptr<State> _state;
};

}
