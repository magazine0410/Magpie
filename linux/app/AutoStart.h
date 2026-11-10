// Starting Magpie with the session: a .desktop file in the autostart
// directory of the XDG specification. Linux counterpart of AutoStartHelper
// (src/Magpie), which uses a scheduled task or the registry.
#pragma once
#include <QString>

namespace Magpie {

struct AutoStart {
	// <config directory>/autostart/<application ID>.desktop
	static QString FilePath();

	static bool IsEnabled();

	// The entry starts `executable` with the option that keeps the main
	// window closed if the tray icon is shown
	static bool Enable(const QString& executable);

	static bool Disable();

	// The text of the entry
	static QString Entry(const QString& executable);
};

}
