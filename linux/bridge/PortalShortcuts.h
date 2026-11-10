// Global shortcuts through the desktop portal
// (org.freedesktop.portal.GlobalShortcuts): the program names the actions it
// wants a key for, and the desktop binds them. On Plasma the user confirms
// the keys in a dialog the first time, and changes them in System Settings.
// Linux counterpart of RegisterHotKey and the keyboard hook in
// src/Magpie/ShortcutService.cpp.
#pragma once
#include <chrono>
#include <span>
#include <string>
#include <utility>
#include <vector>
#include <systemd/sd-bus.h>

namespace Magpie {

class PortalShortcuts {
public:
	struct Shortcut {
		// Names the action for the program; stays the same across versions
		std::string id;
		// What the desktop shows the user
		std::string description;
		// The key the program would like, as in "ALT+SHIFT+a"; may be empty
		std::string preferredTrigger;
	};

	PortalShortcuts() = default;
	PortalShortcuts(const PortalShortcuts&) = delete;
	PortalShortcuts(PortalShortcuts&&) = delete;
	~PortalShortcuts();

	// Makes the program known to the portal as `appId`, for which a .desktop
	// file of that name has to be installed, and asks for the shortcuts. The
	// call waits while the desktop shows its dialog. `bus` has to stay open
	// as long as this object; the shortcuts arrive while it is processed.
	bool Initialize(sd_bus* bus, const std::string& appId, std::span<const Shortcut> shortcuts) noexcept;

	// The ids of the shortcuts that were pressed since the last call
	std::vector<std::string> TakeActivated() noexcept;

	// For every shortcut the desktop bound: its id and the key as text
	const std::vector<std::pair<std::string, std::string>>& Bound() const noexcept {
		return _bound;
	}

	// Whether the desktop reported other keys since the last call; Bound
	// has them then
	bool TakeChanged() noexcept {
		return std::exchange(_isChanged, false);
	}

	// Opens the desktop's dialog in which the user changes the keys of this
	// program's shortcuts. Returns false if the portal has no such function
	// (it came with version 2 of the interface) or refuses.
	bool Configure() noexcept;

private:
	static int _OnActivated(sd_bus_message* message, void* userData, sd_bus_error* error) noexcept;
	static int _OnDeactivated(sd_bus_message* message, void* userData, sd_bus_error* error) noexcept;
	static int _OnShortcutsChanged(sd_bus_message* message, void* userData, sd_bus_error* error) noexcept;

	sd_bus* _bus = nullptr;
	sd_bus_slot* _activatedSlot = nullptr;
	sd_bus_slot* _deactivatedSlot = nullptr;
	sd_bus_slot* _changedSlot = nullptr;
	bool _isChanged = false;
	// The shortcuts whose keys are down, and when each was last reported.
	// The desktop may report a shortcut again and again while its keys are
	// held; only the first report counts.
	std::vector<std::pair<std::string, std::chrono::steady_clock::time_point>> _held;
	std::string _sessionHandle;
	std::vector<std::pair<std::string, std::string>> _bound;
	std::vector<std::string> _activated;
};

}
