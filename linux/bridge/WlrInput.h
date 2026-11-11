// Pointer and keyboard input that a program hands to a wlroots compositor
// (Sway and its relatives), through the protocols wlr-virtual-pointer and
// virtual-keyboard. The compositor treats it like input from real devices.
#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>
#include "Geometry.h"

struct wl_display;
struct wl_seat;
struct zwlr_virtual_pointer_manager_v1;
struct zwlr_virtual_pointer_v1;
struct zwp_virtual_keyboard_manager_v1;
struct zwp_virtual_keyboard_v1;

namespace Magpie {

class WlrInput {
public:
	WlrInput() = default;
	WlrInput(const WlrInput&) = delete;
	WlrInput(WlrInput&&) = delete;
	~WlrInput();

	// Opens a Wayland connection of its own. `displayName` is the name of the
	// compositor's socket; empty for the one of the session.
	bool Connect(const std::string& displayName) noexcept;

	// The compositor lets programs move the pointer, or press keys
	bool HasPointer() const noexcept {
		return _pointerManager != nullptr;
	}
	bool HasKeyboard() const noexcept {
		return _keyboardManager != nullptr && _seat != nullptr;
	}

	// Whether the compositor offers the interface of this name to programs
	bool HasInterface(std::string_view name) const noexcept;

	// Puts the pointer at `pos` of an area of `extent` pixels that stands for
	// everything the compositor's outputs cover together
	void MoveAbsolute(Point pos, Size extent) noexcept;

	// Moves the pointer by this many pixels
	void MoveRelative(double dx, double dy) noexcept;

	// `button` as in linux/input-event-codes.h, for example BTN_LEFT
	void Button(uint32_t button, bool isPressed) noexcept;

	// `value` in the units of wl_pointer.axis: 15 per notch of a wheel,
	// positive downwards or to the right
	void Scroll(bool isHorizontal, double value) noexcept;

	// The keymap that the key codes refer to: a file with a keymap in the
	// text format of XKB and its size. Has to come before the first key.
	void SetKeymap(int fd, uint32_t size) noexcept;

	// `key` as in linux/input-event-codes.h, for example KEY_A
	void Key(uint32_t key, bool isPressed) noexcept;

	// As wl_keyboard.modifiers reports them
	void Modifiers(uint32_t depressed, uint32_t latched, uint32_t locked, uint32_t group) noexcept;

	// Sends what was asked for and takes in what the compositor has sent.
	// Returns false when the connection is lost.
	bool Flush() noexcept;

private:
	bool _EnsurePointer() noexcept;
	bool _EnsureKeyboard() noexcept;

	wl_display* _display = nullptr;
	wl_seat* _seat = nullptr;
	zwlr_virtual_pointer_manager_v1* _pointerManager = nullptr;
	zwlr_virtual_pointer_v1* _pointer = nullptr;
	zwp_virtual_keyboard_manager_v1* _keyboardManager = nullptr;
	zwp_virtual_keyboard_v1* _keyboard = nullptr;
	bool _hasKeymap = false;
	// The names of all interfaces the compositor announced
	std::vector<std::string> _interfaces;

	friend struct WlrInputListeners;
};

}
