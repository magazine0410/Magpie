#include "SessionInhibitor.h"
#include <cstring>
#include <fmt/format.h>
#include "Logger.h"

namespace Magpie {

SessionInhibitor::~SessionInhibitor() {
	if (!_bus) {
		return;
	}

	if (_hasScreenSaverCookie) {
		sd_bus_call_method(_bus, "org.freedesktop.ScreenSaver", "/ScreenSaver", "org.freedesktop.ScreenSaver",
			"UnInhibit", nullptr, nullptr, "u", _screenSaverCookie);
	}
	if (_hasNotificationsCookie) {
		sd_bus_call_method(_bus, "org.freedesktop.Notifications", "/org/freedesktop/Notifications",
			"org.freedesktop.Notifications", "UnInhibit", nullptr, nullptr, "u", _notificationsCookie);
	}
	sd_bus_flush_close_unref(_bus);
}

bool SessionInhibitor::_Connect() noexcept {
	if (_bus) {
		return true;
	}

	const int result = sd_bus_open_user(&_bus);
	if (result < 0) {
		Logger::Get().Error(fmt::format("Connecting to the session bus failed: {}", std::strerror(-result)));
		_bus = nullptr;
		return false;
	}
	return true;
}

// Calls a method that returns a cookie
static bool CallForCookie(sd_bus* bus, const char* what, uint32_t& cookie, const auto& call) noexcept {
	sd_bus_error error = SD_BUS_ERROR_NULL;
	sd_bus_message* reply = nullptr;
	int result = call(&error, &reply);
	if (result >= 0) {
		result = sd_bus_message_read(reply, "u", &cookie);
	}
	if (result < 0) {
		Logger::Get().Warn(fmt::format("The desktop did not agree to {}: {}", what,
			error.message ? error.message : std::strerror(-result)));
	}
	sd_bus_error_free(&error);
	sd_bus_message_unref(reply);
	(void)bus;
	return result >= 0;
}

bool SessionInhibitor::KeepScreenOn(const std::string& reason) noexcept {
	if (_hasScreenSaverCookie || !_Connect()) {
		return _hasScreenSaverCookie;
	}

	_hasScreenSaverCookie = CallForCookie(_bus, "keep the screen on", _screenSaverCookie,
		[&](sd_bus_error* error, sd_bus_message** reply) {
			return sd_bus_call_method(_bus, "org.freedesktop.ScreenSaver", "/ScreenSaver",
				"org.freedesktop.ScreenSaver", "Inhibit", error, reply, "ss", "Magpie", reason.c_str());
		});
	return _hasScreenSaverCookie;
}

bool SessionInhibitor::SuppressNotifications(const std::string& desktopEntry, const std::string& reason) noexcept {
	if (_hasNotificationsCookie || !_Connect()) {
		return _hasNotificationsCookie;
	}

	_hasNotificationsCookie = CallForCookie(_bus, "hold back notifications", _notificationsCookie,
		[&](sd_bus_error* error, sd_bus_message** reply) {
			return sd_bus_call_method(_bus, "org.freedesktop.Notifications", "/org/freedesktop/Notifications",
				"org.freedesktop.Notifications", "Inhibit", error, reply, "ssa{sv}",
				desktopEntry.c_str(), reason.c_str(), 0);
		});
	return _hasNotificationsCookie;
}

}
