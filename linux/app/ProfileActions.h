// What can be done with the program of a profile. Linux counterpart of the
// actions of the Profile class of src/Magpie (Launch, OpenProgramLocation).
#pragma once
#include <QString>
#include "Profile.h"

namespace Magpie {

struct ProfileActions {
	// What is started for the profile: its launcher if one is set and
	// exists, otherwise the program of its rule if that is a file on this
	// system. Empty if there is neither; the rule of a Windows program
	// under Wine names no file here, so such a profile needs a launcher.
	static QString ProgramToLaunch(const Profile& profile);

	// Starts the program with the launch parameters of the profile
	static bool Launch(const Profile& profile);

	// The directory of the program of the rule; empty if it is no file on
	// this system
	static QString ProgramLocation(const Profile& profile);

	// The name or the path of the icon that the desktop has for a program
	// with this app ID, from its .desktop file; a general icon if it has
	// none
	static QString IconName(const QString& appId);

	// The name of the application in its .desktop file; empty if it has none
	static QString ApplicationName(const QString& appId);
};

}
