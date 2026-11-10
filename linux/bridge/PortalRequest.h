// A method call of the desktop portal that answers through a request object:
// the method returns at once, and the result arrives later as a signal, for
// some methods only after the user has answered a dialog. Shared by the
// portal back ends; not part of the bridge's interface.
#pragma once
#include <functional>
#include <string>
#include <systemd/sd-bus.h>

namespace Magpie::PortalRequest {

inline constexpr const char* PORTAL_NAME = "org.freedesktop.portal.Desktop";
inline constexpr const char* PORTAL_PATH = "/org/freedesktop/portal/desktop";

enum class Outcome {
	Success,
	// The user closed the portal's dialog without agreeing
	Cancelled,
	// The call failed or the portal ended the request; the reason is logged
	Failed
};

// Calls `method` of `interface` on the portal and waits for the answer of
// its request object. `appendArguments` adds the method's arguments, with the
// token it is given as "handle_token" of the options. `readResult` is called
// for every entry of the results, with the read position at the entry's
// value, a variant, which it has to read or skip.
Outcome Call(
	sd_bus* bus,
	const char* interface,
	const char* method,
	const std::function<int(sd_bus_message*, const char*)>& appendArguments,
	const std::function<void(const char*, sd_bus_message*)>& readResult
) noexcept;

// The read position is at a variant. Reads it if it holds a string or an
// object path, and steps over it otherwise.
void ReadStringVariant(sd_bus_message* message, std::string& value) noexcept;

}
