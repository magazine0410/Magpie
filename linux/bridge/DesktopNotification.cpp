#include "DesktopNotification.h"
#include <systemd/sd-bus.h>
#include "Logger.h"

namespace Magpie {

void DesktopNotification::Show(
	const std::string& desktopEntry,
	const std::string& title,
	const std::string& text
) noexcept {
	// A connection of its own, so that any thread may call
	sd_bus* bus = nullptr;
	if (sd_bus_open_user(&bus) < 0) {
		return;
	}

	// org.freedesktop.Notifications.Notify: application name, id of a
	// notification to replace, icon, summary, body, actions, hints, and the
	// time after which it goes away in milliseconds
	sd_bus_message* call = nullptr;
	int result = sd_bus_message_new_method_call(bus, &call, "org.freedesktop.Notifications",
		"/org/freedesktop/Notifications", "org.freedesktop.Notifications", "Notify");
	if (result >= 0) {
		result = sd_bus_message_append(call, "susssas", "Magpie", 0u, "", title.c_str(), text.c_str(), 0);
	}
	if (result >= 0) {
		result = sd_bus_message_append(call, "a{sv}", 2,
			"desktop-entry", "s", desktopEntry.c_str(),
			// Not kept in the history
			"transient", "b", 1);
	}
	if (result >= 0) {
		result = sd_bus_message_append(call, "i", 3000);
	}
	if (result >= 0) {
		// Sent without waiting for the answer. A desktop whose notification
		// service does not come up would otherwise hold the caller, which
		// is the thread that scales, for 25 seconds.
		sd_bus_message_set_expect_reply(call, 0);
		result = sd_bus_send(bus, call, nullptr);
	}
	if (result < 0) {
		Logger::Get().Info("The notification could not be sent");
	}

	sd_bus_message_unref(call);
	sd_bus_flush_close_unref(bus);
}

}
