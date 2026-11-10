#include "PortalShortcuts.h"
#include <algorithm>
#include <string_view>
#include <cstring>
#include <unistd.h>
#include <fmt/format.h>
#include "Logger.h"
#include "PortalRequest.h"

namespace Magpie {

using PortalRequest::PORTAL_NAME;
using PortalRequest::PORTAL_PATH;
static constexpr const char* SHORTCUTS_INTERFACE = "org.freedesktop.portal.GlobalShortcuts";

PortalShortcuts::~PortalShortcuts() {
	sd_bus_slot_unref(_activatedSlot);
	sd_bus_slot_unref(_deactivatedSlot);
	if (_bus && !_sessionHandle.empty()) {
		sd_bus_call_method(_bus, PORTAL_NAME, _sessionHandle.c_str(),
			"org.freedesktop.portal.Session", "Close", nullptr, nullptr, "");
	}
}

// Activated: (o session_handle, s shortcut_id, t timestamp, a{sv} options)
int PortalShortcuts::_OnActivated(sd_bus_message* message, void* userData, sd_bus_error*) noexcept {
	PortalShortcuts& that = *(PortalShortcuts*)userData;

	const char* session = nullptr;
	const char* id = nullptr;
	if (sd_bus_message_read(message, "os", &session, &id) < 0 || !session || !id ||
		that._sessionHandle != session) {
		return 0;
	}

	// A report for keys that are held already is a repetition. Should the
	// release of a shortcut ever go unreported, it counts as released when
	// nothing was heard of it for a while.
	static constexpr std::chrono::seconds STALE_AFTER{ 2 };
	const auto now = std::chrono::steady_clock::now();
	const auto held = std::ranges::find(that._held, std::string_view(id),
		&std::pair<std::string, std::chrono::steady_clock::time_point>::first);
	if (held == that._held.end()) {
		that._held.emplace_back(id, now);
		that._activated.emplace_back(id);
	} else {
		if (now - held->second > STALE_AFTER) {
			that._activated.emplace_back(id);
		}
		held->second = now;
	}
	return 0;
}

// Deactivated: (o session_handle, s shortcut_id, t timestamp, a{sv} options)
int PortalShortcuts::_OnDeactivated(sd_bus_message* message, void* userData, sd_bus_error*) noexcept {
	PortalShortcuts& that = *(PortalShortcuts*)userData;

	const char* session = nullptr;
	const char* id = nullptr;
	if (sd_bus_message_read(message, "os", &session, &id) >= 0 && session && id &&
		that._sessionHandle == session) {
		std::erase_if(that._held, [&](const auto& held) { return held.first == id; });
	}
	return 0;
}

// The read position is at the variant of "shortcuts": a(sa{sv}), for every
// bound shortcut its id and properties
static void ReadBoundShortcuts(sd_bus_message* message, std::vector<std::pair<std::string, std::string>>& bound) noexcept {
	if (sd_bus_message_enter_container(message, 'v', "a(sa{sv})") <= 0) {
		sd_bus_message_skip(message, "v");
		return;
	}

	sd_bus_message_enter_container(message, 'a', "(sa{sv})");
	while (sd_bus_message_enter_container(message, 'r', "sa{sv}") > 0) {
		const char* id = nullptr;
		sd_bus_message_read(message, "s", &id);
		std::string trigger;

		sd_bus_message_enter_container(message, 'a', "{sv}");
		while (sd_bus_message_enter_container(message, 'e', "sv") > 0) {
			const char* key = nullptr;
			sd_bus_message_read(message, "s", &key);
			if (key && std::strcmp(key, "trigger_description") == 0) {
				PortalRequest::ReadStringVariant(message, trigger);
			} else {
				sd_bus_message_skip(message, "v");
			}
			sd_bus_message_exit_container(message);
		}
		sd_bus_message_exit_container(message);

		if (id) {
			bound.emplace_back(id, std::move(trigger));
		}
		sd_bus_message_exit_container(message);
	}
	sd_bus_message_exit_container(message);
	sd_bus_message_exit_container(message);
}

bool PortalShortcuts::Initialize(
	sd_bus* bus,
	const std::string& appId,
	std::span<const Shortcut> shortcuts
) noexcept {
	_bus = bus;

	// A program outside a sandbox tells the portal who it is; it has to do
	// so before its first other call. Portals older than this interface take
	// the name from how the program was started.
	{
		sd_bus_error error = SD_BUS_ERROR_NULL;
		if (sd_bus_call_method(bus, PORTAL_NAME, PORTAL_PATH, "org.freedesktop.host.portal.Registry",
			"Register", &error, nullptr, "sa{sv}", appId.c_str(), 0) < 0) {
			Logger::Get().Warn(fmt::format("The portal did not accept the application ID {}: {}",
				appId, error.message ? error.message : "unknown error"));
		}
		sd_bus_error_free(&error);
	}

	const std::string sessionToken = fmt::format("magpie{}", getpid());
	PortalRequest::Outcome outcome = PortalRequest::Call(bus, SHORTCUTS_INTERFACE, "CreateSession",
		[&](sd_bus_message* call, const char* token) {
			return sd_bus_message_append(call, "a{sv}", 2,
				"handle_token", "s", token,
				"session_handle_token", "s", sessionToken.c_str());
		},
		[&](const char* key, sd_bus_message* message) {
			if (std::strcmp(key, "session_handle") == 0) {
				PortalRequest::ReadStringVariant(message, _sessionHandle);
			} else {
				sd_bus_message_skip(message, "v");
			}
		});
	if (outcome != PortalRequest::Outcome::Success || _sessionHandle.empty()) {
		Logger::Get().Error("The portal gave no session for global shortcuts");
		_sessionHandle.clear();
		return false;
	}

	if (sd_bus_match_signal(bus, &_activatedSlot, nullptr, PORTAL_PATH, SHORTCUTS_INTERFACE,
		"Activated", _OnActivated, this) < 0 ||
		sd_bus_match_signal(bus, &_deactivatedSlot, nullptr, PORTAL_PATH, SHORTCUTS_INTERFACE,
		"Deactivated", _OnDeactivated, this) < 0) {
		Logger::Get().Error("Subscribing to the shortcuts failed");
		return false;
	}

	outcome = PortalRequest::Call(bus, SHORTCUTS_INTERFACE, "BindShortcuts",
		[&](sd_bus_message* call, const char* token) {
			int r = sd_bus_message_append(call, "o", _sessionHandle.c_str());
			if (r >= 0) {
				r = sd_bus_message_open_container(call, 'a', "(sa{sv})");
			}
			for (const Shortcut& shortcut : shortcuts) {
				if (r >= 0) {
					r = sd_bus_message_open_container(call, 'r', "sa{sv}");
				}
				if (r >= 0) {
					r = sd_bus_message_append(call, "s", shortcut.id.c_str());
				}
				if (r >= 0) {
					r = sd_bus_message_open_container(call, 'a', "{sv}");
				}
				if (r >= 0) {
					r = sd_bus_message_append(call, "{sv}", "description", "s", shortcut.description.c_str());
				}
				if (r >= 0 && !shortcut.preferredTrigger.empty()) {
					r = sd_bus_message_append(call, "{sv}", "preferred_trigger", "s", shortcut.preferredTrigger.c_str());
				}
				if (r >= 0) {
					r = sd_bus_message_close_container(call);
				}
				if (r >= 0) {
					r = sd_bus_message_close_container(call);
				}
			}
			if (r >= 0) {
				r = sd_bus_message_close_container(call);
			}
			if (r >= 0) {
				// No parent window
				r = sd_bus_message_append(call, "sa{sv}", "", 1, "handle_token", "s", token);
			}
			return r;
		},
		[&](const char* key, sd_bus_message* message) {
			if (std::strcmp(key, "shortcuts") == 0) {
				ReadBoundShortcuts(message, _bound);
			} else {
				sd_bus_message_skip(message, "v");
			}
		});
	if (outcome == PortalRequest::Outcome::Cancelled) {
		Logger::Get().Warn("The shortcuts were declined in the desktop's dialog");
		return false;
	}
	return outcome == PortalRequest::Outcome::Success;
}

std::vector<std::string> PortalShortcuts::TakeActivated() noexcept {
	return std::move(_activated);
}

}
