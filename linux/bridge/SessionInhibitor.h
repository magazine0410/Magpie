// Asks the desktop to hold back things that would disturb a scaled game:
// the screen saver and screen blanking ("keep screen on"), and notification
// pop-ups ("simulate exclusive fullscreen", which on Windows makes the system
// treat the scaled window like a fullscreen game). Both are requests on the
// session bus that the desktop drops by itself when the program's connection
// closes, so nothing stays behind after a crash.
#pragma once
#include <cstdint>
#include <string>
#include <systemd/sd-bus.h>

namespace Magpie {

class SessionInhibitor {
public:
	SessionInhibitor() = default;
	SessionInhibitor(const SessionInhibitor&) = delete;
	SessionInhibitor(SessionInhibitor&&) = delete;

	// Ends what was asked for
	~SessionInhibitor();

	// org.freedesktop.ScreenSaver. `reason` is shown by the desktop's power
	// settings.
	bool KeepScreenOn(const std::string& reason) noexcept;

	// org.freedesktop.Notifications, as Plasma implements "do not disturb".
	// `desktopEntry` is the application ID.
	bool SuppressNotifications(const std::string& desktopEntry, const std::string& reason) noexcept;

private:
	bool _Connect() noexcept;

	sd_bus* _bus = nullptr;
	uint32_t _screenSaverCookie = 0;
	uint32_t _notificationsCookie = 0;
	bool _hasScreenSaverCookie = false;
	bool _hasNotificationsCookie = false;
};

}
