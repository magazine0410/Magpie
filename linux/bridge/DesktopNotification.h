// A short message of the desktop, the counterpart of the toasts with which
// the Windows version tells the user that a screenshot was saved or what
// state the toolbar has now.
#pragma once
#include <string>

namespace Magpie {

struct DesktopNotification {
	// Shows a message that goes away by itself and is not kept in the
	// desktop's history. `desktopEntry` is the application ID, from which
	// the desktop takes name and icon. May be called from any thread. A
	// desktop without notifications is not an error.
	static void Show(const std::string& desktopEntry, const std::string& title, const std::string& text) noexcept;
};

}
