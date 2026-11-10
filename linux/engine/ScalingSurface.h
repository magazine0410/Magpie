// The surface that shows the scaled picture. It has one of two forms:
//
// - A layer surface that covers one output above all windows and never takes
//   keyboard focus, so that the source stays the active window. Linux
//   counterpart of the fullscreen scaling window's styles on Windows
//   (topmost, no-activate). It uses wlr-layer-shell, which KWin and the
//   wlroots compositors implement.
// - An ordinary window with the title bar and borders of the desktop, for
//   windowed scaling.
//
// It only creates the surface and reports the pointer on it; a Vulkan swap
// chain provides the content.
#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include "Geometry.h"

struct wl_display;
struct wl_surface;

namespace Magpie {

class ScalingSurface {
public:
	// Decides the size of a window when the desktop proposes one: it gets
	// that size, and in which directions it differs from what the window
	// was. While the user resizes the window, that is compared with the
	// size at the start, and a direction that has changed once counts as
	// changed until the user lets go: dragging a corner then stays dragging
	// a corner when the mouse comes back to the row or column it began in.
	using SizeRule = std::function<Size(Size proposed, bool isWidthChanged, bool isHeightChanged)>;

	struct PointerEvent {
		enum class Type {
			Motion,
			// `button` is 0 for the left, 1 for the right, 2 for the middle button
			ButtonDown,
			ButtonUp,
			// `value` is the distance, positive downwards
			Scroll,
			Leave
		};

		Type type = Type::Motion;
		Point pos;
		int button = 0;
		float value = 0.0f;
	};

	ScalingSurface() noexcept;
	ScalingSurface(const ScalingSurface&) = delete;
	ScalingSurface(ScalingSurface&&) = delete;
	~ScalingSurface();

	// Opens a Wayland connection of its own and covers the output with this
	// name. With `aboveEverything` the surface is on the overlay layer, above
	// notifications and panels as well; otherwise on the top layer.
	bool CreateLayer(const std::string& outputName, bool aboveEverything) noexcept;

	// Opens a Wayland connection of its own and prepares a window whose
	// content has this size. The window appears with its first frame. The
	// user can make it smaller down to `minSize`.
	bool CreateWindow(
		const std::string& title,
		const std::string& appId,
		Size size,
		Size minSize,
		SizeRule sizeRule
	) noexcept;

	// Processes the events that have arrived without waiting for more.
	// Returns false when the connection is lost.
	bool PumpEvents() noexcept;

	// Readable when PumpEvents has something to do
	int EventFd() const noexcept;

	wl_display* Display() const noexcept;

	wl_surface* Surface() const noexcept;

	// The size of the content: that of the output for a layer surface, and
	// what the window has been given or the size rule has chosen
	Size GetSize() const noexcept;

	// The compositor has removed the surface, for example with its output,
	// or the user has closed the window
	bool IsClosed() const noexcept;

	// A window that is maximized, fullscreen or tiled: the desktop has set
	// its size, and the size rule was not asked
	bool IsSizeFixed() const noexcept;

	// The pointer is on the surface. Its image is hidden there unless
	// SetCursorShown says otherwise: either the scaled cursor is drawn, or
	// the pointer is about to be sent to the source.
	bool IsPointerInside() const noexcept;

	// Relative to the surface; valid while the pointer is inside
	Point PointerPos() const noexcept;

	// Whether the pointer has the arrow of the desktop on this surface. It
	// is hidden by default; the arrow is for the times when the pointer
	// stays on the surface, to operate the toolbar for example.
	void SetCursorShown(bool shown) noexcept;

	// Counts the times the pointer came onto the surface or moved on it
	uint32_t PointerMotionCount() const noexcept;

	// What the pointer did on the surface since the last call
	std::vector<PointerEvent> TakePointerEvents() noexcept;

private:
	struct State;
	std::unique_ptr<State> _state;
};

}
