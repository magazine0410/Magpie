// Which profile applies to a window. Linux counterpart of the rule handling
// in src/Magpie/ProfileService.cpp.
//
// A profile recognises a window by two texts, as on Windows:
// - classNameRule: the window class there; here the app ID of the window,
//   which for an X11 program is its window class
// - pathRule: the path of the program (ProcessInfo::GetPath)
// The special cases for window classes that change with every start (WPF,
// RPG Maker MZ, TeknoParrot) concern Win32 window classes, which a Windows
// program under Wine does not show to the desktop.
#pragma once
#include <string_view>
#include "Settings.h"

namespace Magpie {

// The profile with these rules. Without one: null for auto-scale, the
// default profile otherwise. Profiles for packaged Windows applications
// never apply.
const Profile* FindProfileForWindow(
	const Settings& settings,
	std::string_view className,
	std::string_view path,
	bool forAutoScale
) noexcept;

// Whether a profile with these rules can be added: both are known and no
// profile has them yet
bool TestNewProfile(const Settings& settings, std::string_view className, std::string_view path) noexcept;

}
