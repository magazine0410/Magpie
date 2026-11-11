#include "WlrInput.h"
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <ctime>
#include <poll.h>
#include <wayland-client.h>
#include "Logger.h"
#include "virtual-keyboard-unstable-v1-client-protocol.h"
#include "wlr-virtual-pointer-unstable-v1-client-protocol.h"

namespace Magpie {

static uint32_t NowMilliseconds() noexcept {
	timespec now{};
	clock_gettime(CLOCK_MONOTONIC, &now);
	return (uint32_t)(now.tv_sec * 1000 + now.tv_nsec / 1'000'000);
}

struct WlrInputListeners {
	static constexpr wl_registry_listener REGISTRY{
		.global = [](void* data, wl_registry* registry, uint32_t name, const char* interface, uint32_t) {
			WlrInput& that = *(WlrInput*)data;
			that._interfaces.emplace_back(interface);
			if (std::strcmp(interface, wl_seat_interface.name) == 0 && !that._seat) {
				that._seat = (wl_seat*)wl_registry_bind(registry, name, &wl_seat_interface, 1);
			} else if (std::strcmp(interface, zwlr_virtual_pointer_manager_v1_interface.name) == 0) {
				that._pointerManager = (zwlr_virtual_pointer_manager_v1*)wl_registry_bind(
					registry, name, &zwlr_virtual_pointer_manager_v1_interface, 1);
			} else if (std::strcmp(interface, zwp_virtual_keyboard_manager_v1_interface.name) == 0) {
				that._keyboardManager = (zwp_virtual_keyboard_manager_v1*)wl_registry_bind(
					registry, name, &zwp_virtual_keyboard_manager_v1_interface, 1);
			}
		},
		.global_remove = [](void*, wl_registry*, uint32_t) {}
	};
};

WlrInput::~WlrInput() {
	if (_display) {
		// The compositor removes the devices with the connection
		wl_display_flush(_display);
		wl_display_disconnect(_display);
	}
}

bool WlrInput::Connect(const std::string& displayName) noexcept {
	_display = wl_display_connect(displayName.empty() ? nullptr : displayName.c_str());
	if (!_display) {
		Logger::Get().Error("Cannot connect to the Wayland compositor for input");
		return false;
	}

	wl_registry* registry = wl_display_get_registry(_display);
	wl_registry_add_listener(registry, &WlrInputListeners::REGISTRY, this);
	if (wl_display_roundtrip(_display) < 0) {
		Logger::Get().Error("The Wayland connection for input failed");
		return false;
	}
	return true;
}

bool WlrInput::HasInterface(std::string_view name) const noexcept {
	return std::find(_interfaces.begin(), _interfaces.end(), name) != _interfaces.end();
}

bool WlrInput::_EnsurePointer() noexcept {
	if (_pointer) {
		return true;
	}
	if (!_display || !_pointerManager) {
		return false;
	}
	// Not bound to an output: its positions refer to all outputs together
	_pointer = zwlr_virtual_pointer_manager_v1_create_virtual_pointer(_pointerManager, _seat);
	return _pointer != nullptr;
}

bool WlrInput::_EnsureKeyboard() noexcept {
	if (_keyboard) {
		return true;
	}
	if (!_display || !_keyboardManager || !_seat) {
		return false;
	}
	_keyboard = zwp_virtual_keyboard_manager_v1_create_virtual_keyboard(_keyboardManager, _seat);
	return _keyboard != nullptr;
}

void WlrInput::MoveAbsolute(Point pos, Size extent) noexcept {
	if (!_EnsurePointer() || extent.width <= 0 || extent.height <= 0) {
		return;
	}
	// The compositor divides by the extent and multiplies by its own size.
	// Aiming a quarter of a pixel into the wanted pixel keeps the result in
	// that pixel whichever way the compositor rounds.
	zwlr_virtual_pointer_v1_motion_absolute(_pointer, NowMilliseconds(),
		(uint32_t)(pos.x * 4 + 1), (uint32_t)(pos.y * 4 + 1), (uint32_t)(extent.width * 4), (uint32_t)(extent.height * 4));
	zwlr_virtual_pointer_v1_frame(_pointer);
}

void WlrInput::MoveRelative(double dx, double dy) noexcept {
	if (!_EnsurePointer()) {
		return;
	}
	zwlr_virtual_pointer_v1_motion(_pointer, NowMilliseconds(), wl_fixed_from_double(dx), wl_fixed_from_double(dy));
	zwlr_virtual_pointer_v1_frame(_pointer);
}

void WlrInput::Button(uint32_t button, bool isPressed) noexcept {
	if (!_EnsurePointer()) {
		return;
	}
	zwlr_virtual_pointer_v1_button(_pointer, NowMilliseconds(), button,
		isPressed ? WL_POINTER_BUTTON_STATE_PRESSED : WL_POINTER_BUTTON_STATE_RELEASED);
	zwlr_virtual_pointer_v1_frame(_pointer);
}

void WlrInput::Scroll(bool isHorizontal, double value) noexcept {
	if (!_EnsurePointer()) {
		return;
	}
	const uint32_t axis = isHorizontal ? WL_POINTER_AXIS_HORIZONTAL_SCROLL : WL_POINTER_AXIS_VERTICAL_SCROLL;
	zwlr_virtual_pointer_v1_axis_source(_pointer, WL_POINTER_AXIS_SOURCE_WHEEL);
	zwlr_virtual_pointer_v1_axis(_pointer, NowMilliseconds(), axis, wl_fixed_from_double(value));
	zwlr_virtual_pointer_v1_frame(_pointer);
}

void WlrInput::SetKeymap(int fd, uint32_t size) noexcept {
	if (!_EnsureKeyboard()) {
		return;
	}
	zwp_virtual_keyboard_v1_keymap(_keyboard, WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1, fd, size);
	_hasKeymap = true;
}

void WlrInput::Key(uint32_t key, bool isPressed) noexcept {
	// A key without a keymap is a protocol error
	if (!_EnsureKeyboard() || !_hasKeymap) {
		return;
	}
	zwp_virtual_keyboard_v1_key(_keyboard, NowMilliseconds(), key,
		isPressed ? WL_KEYBOARD_KEY_STATE_PRESSED : WL_KEYBOARD_KEY_STATE_RELEASED);
}

void WlrInput::Modifiers(uint32_t depressed, uint32_t latched, uint32_t locked, uint32_t group) noexcept {
	if (!_EnsureKeyboard() || !_hasKeymap) {
		return;
	}
	zwp_virtual_keyboard_v1_modifiers(_keyboard, depressed, latched, locked, group);
}

bool WlrInput::Flush() noexcept {
	if (!_display) {
		return false;
	}

	// The compositor sends little on this connection, but what it sends has
	// to be read, or its buffer fills up
	while (wl_display_prepare_read(_display) != 0) {
		wl_display_dispatch_pending(_display);
	}
	if (wl_display_flush(_display) < 0 && errno != EAGAIN) {
		wl_display_cancel_read(_display);
		return false;
	}
	pollfd fd{ wl_display_get_fd(_display), POLLIN, 0 };
	if (poll(&fd, 1, 0) > 0) {
		wl_display_read_events(_display);
	} else {
		wl_display_cancel_read(_display);
	}
	return wl_display_dispatch_pending(_display) >= 0;
}

}
