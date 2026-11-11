// The surface that shows the scaled picture. It has one of three forms:
//
// - A layer surface that covers one output above all windows and never takes
//   keyboard focus, so that the source stays the active window. Linux
//   counterpart of the fullscreen scaling window's styles on Windows
//   (topmost, no-activate). It uses wlr-layer-shell, which KWin and the
//   wlroots compositors implement.
// - An ordinary window with the title bar and borders of the desktop, for
//   windowed scaling.
// - An ordinary window that asks to be fullscreen, for the nested session:
//   there the program runs in a compositor of Magpie's own, and this window
//   is what has the keyboard and the pointer on the desktop.
//
// It only creates the surface and reports the pointer and the keyboard on
// it; a Vulkan swap chain provides the content.
#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
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
			// `button` is 0 for the left, 1 for the right, 2 for the middle
			// button and -1 for any other; `code` is the button as the
			// kernel numbers it (BTN_LEFT and so on)
			ButtonDown,
			ButtonUp,
			// `value` is the distance, positive downwards or to the right
			Scroll,
			Leave,
			// While the pointer is locked: how far the device moved, in `dx`
			// and `dy`, without the desktop's acceleration
			RelativeMotion
		};

		Type type = Type::Motion;
		Point pos;
		int button = 0;
		float value = 0.0f;
		uint32_t code = 0;
		bool isHorizontal = false;
		double dx = 0.0;
		double dy = 0.0;
	};

	struct KeyEvent {
		enum class Type {
			// `key` is the key as the kernel numbers it (KEY_A and so on)
			KeyDown,
			KeyUp,
			// Which modifiers are held, latched and locked, and the layout, as
			// wl_keyboard reports them
			Modifiers,
			// The surface got or lost the keyboard
			FocusIn,
			FocusOut
		};

		Type type = Type::KeyDown;
		uint32_t key = 0;
		uint32_t depressed = 0;
		uint32_t latched = 0;
		uint32_t locked = 0;
		uint32_t group = 0;
	};

	ScalingSurface() noexcept;
	ScalingSurface(const ScalingSurface&) = delete;
	ScalingSurface(ScalingSurface&&) = delete;
	~ScalingSurface();

	// Opens the Wayland connection without making a surface yet, for a
	// caller that wants to know the monitors first. CreateLayer and
	// CreateWindow do this themselves otherwise. `displayName` is the name
	// of the compositor's socket, or empty for the compositor of the session.
	bool Connect(const std::string& displayName) noexcept;

	// The size of the largest output of the compositor, in its pixels; zero
	// if none is known
	Size LargestOutputSize() const noexcept;

	// Opens a Wayland connection of its own and covers the output with this
	// name. With `aboveEverything` the surface is on the overlay layer, above
	// notifications and panels as well; otherwise on the top layer.
	// `displayName` is the name of the compositor's socket, or empty for the
	// compositor of the session.
	bool CreateLayer(const std::string& displayName, const std::string& outputName, bool aboveEverything) noexcept;

	// Opens a Wayland connection of its own and prepares a window whose
	// content has this size. The window appears with its first frame. The
	// user can make it smaller down to `minSize`.
	// With `startsFixed` the window says at first that it cannot be resized,
	// until ReleaseFixedSize: a desktop that tiles windows then leaves it
	// its size and lets it float. With `isFullscreen` it asks to cover a
	// monitor, which the desktop chooses, and takes the size of that.
	bool CreateWindow(
		const std::string& displayName,
		const std::string& title,
		const std::string& appId,
		Size size,
		Size minSize,
		SizeRule sizeRule,
		bool startsFixed = false,
		bool isFullscreen = false
	) noexcept;

	// From now on the user can resize the window, down to its minimum size
	void ReleaseFixedSize() noexcept;

	// Gives a window another title
	void SetTitle(const std::string& title) noexcept;

	// Asks the compositor not to blank the screen or start the screen saver
	// while the surface is shown (the protocol idle-inhibit). Returns false
	// if the compositor does not offer that.
	bool InhibitIdle() noexcept;

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

	// What the keyboard did while the surface had it, since the last call.
	// A layer surface never has the keyboard.
	std::vector<KeyEvent> TakeKeyEvents() noexcept;

	bool HasKeyboardFocus() const noexcept;

	// The keymap that the key codes refer to, if the desktop has sent a new
	// one since the last call: a file with it, which the caller closes, and
	// its size
	bool TakeKeymap(int& fd, uint32_t& size) noexcept;

	// Holds the pointer where it is and reports its movement as
	// RelativeMotion instead; what a game does that turns a camera with the
	// mouse. Takes effect while the surface has the pointer. When the lock is
	// taken away again and `hint` is given, the pointer continues from that
	// place of the surface.
	void SetPointerLocked(bool isLocked, std::optional<Point> hint = std::nullopt) noexcept;

	// The desktop has locked the pointer
	bool IsPointerLocked() const noexcept;

	// The refresh rate of the output the surface is on, in thousandths of a
	// hertz; zero if that is not known
	int32_t OutputRefreshRate() const noexcept;

private:
	struct State;
	std::unique_ptr<State> _state;
};

}
