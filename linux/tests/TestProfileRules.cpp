#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <unistd.h>
#include "ProcessInfo.h"
#include "ProfileRules.h"

using namespace Magpie;

static Settings MakeSettings() {
	Settings settings;
	settings.defaultProfile.maxFrameRate = 30.0f;

	Profile& kate = settings.profiles.emplace_back();
	kate.name = "Kate";
	kate.classNameRule = "org.kde.kate";
	kate.pathRule = "/usr/bin/kate";

	Profile& game = settings.profiles.emplace_back();
	game.name = "Game";
	game.classNameRule = "game.exe";
	game.pathRule = "C:\\Games\\Game.exe";
	game.autoScale = AutoScale::Windowed;

	// Made on Windows for a packaged application
	Profile& packaged = settings.profiles.emplace_back();
	packaged.name = "Packaged";
	packaged.isPackaged = true;
	packaged.classNameRule = "ApplicationFrameWindow";
	packaged.pathRule = "Vendor.App_abc!App";
	packaged.autoScale = AutoScale::Fullscreen;
	return settings;
}

TEST_CASE("A window gets the profile whose two rules it meets", "[profiles]") {
	const Settings settings = MakeSettings();

	CHECK(FindProfileForWindow(settings, "org.kde.kate", "/usr/bin/kate", false) == &settings.profiles[0]);
	CHECK(FindProfileForWindow(settings, "game.exe", "C:\\Games\\Game.exe", false) == &settings.profiles[1]);

	// One rule is not enough, and letter case counts as on Windows
	CHECK(FindProfileForWindow(settings, "org.kde.kate", "/usr/bin/kwrite", false) == &settings.defaultProfile);
	CHECK(FindProfileForWindow(settings, "org.kde.kwrite", "/usr/bin/kate", false) == &settings.defaultProfile);
	CHECK(FindProfileForWindow(settings, "Game.exe", "C:\\Games\\Game.exe", false) == &settings.defaultProfile);

	// A window of which one of the two is not known has no profile
	CHECK(FindProfileForWindow(settings, "", "/usr/bin/kate", false) == &settings.defaultProfile);
	CHECK(FindProfileForWindow(settings, "org.kde.kate", "", false) == &settings.defaultProfile);

	// Profiles for packaged Windows applications never apply
	CHECK(FindProfileForWindow(settings, "ApplicationFrameWindow", "Vendor.App_abc!App", false) == &settings.defaultProfile);
}

TEST_CASE("Only profiles with auto-scale count for auto-scale", "[profiles]") {
	const Settings settings = MakeSettings();

	CHECK(FindProfileForWindow(settings, "game.exe", "C:\\Games\\Game.exe", true) == &settings.profiles[1]);
	CHECK(FindProfileForWindow(settings, "org.kde.kate", "/usr/bin/kate", true) == nullptr);
	CHECK(FindProfileForWindow(settings, "org.kde.dolphin", "/usr/bin/dolphin", true) == nullptr);
	CHECK(FindProfileForWindow(settings, "ApplicationFrameWindow", "Vendor.App_abc!App", true) == nullptr);
}

TEST_CASE("A new profile needs rules that no profile has", "[profiles]") {
	const Settings settings = MakeSettings();

	CHECK(TestNewProfile(settings, "org.kde.dolphin", "/usr/bin/dolphin"));
	CHECK(TestNewProfile(settings, "org.kde.kate", "/usr/bin/kwrite"));
	CHECK_FALSE(TestNewProfile(settings, "org.kde.kate", "/usr/bin/kate"));
	CHECK_FALSE(TestNewProfile(settings, "", "/usr/bin/dolphin"));
	CHECK_FALSE(TestNewProfile(settings, "org.kde.dolphin", ""));
}

TEST_CASE("The program path of a process is what profiles recognise it by", "[profiles]") {
	CHECK(ProcessInfo::PathFromProcess("/usr/bin/kate", "kate") == "/usr/bin/kate");
	CHECK(ProcessInfo::PathFromProcess("/usr/bin/python3.14", "/usr/bin/python3") == "/usr/bin/python3.14");

	// A Windows program under Wine: the loader says nothing, the command
	// line names the program
	CHECK(ProcessInfo::PathFromProcess("/usr/bin/wine64-preloader", "C:\\Games\\Game.exe") == "C:\\Games\\Game.exe");
	CHECK(ProcessInfo::PathFromProcess("/opt/proton/files/bin/wine-preloader", "Z:\\home\\user\\game\\GAME.EXE") ==
		"Z:\\home\\user\\game\\GAME.EXE");
	CHECK(ProcessInfo::PathFromProcess("/usr/bin/wine", "game.exe") == "game.exe");
	// Wine's own programs that are not Windows programs
	CHECK(ProcessInfo::PathFromProcess("/usr/bin/wineserver", "/usr/bin/wineserver") == "/usr/bin/wineserver");
	// Not Wine
	CHECK(ProcessInfo::PathFromProcess("/usr/bin/mono", "Program.exe") == "/usr/bin/mono");

	std::error_code error;
	CHECK(ProcessInfo::GetPath((uint32_t)getpid()) == std::filesystem::read_symlink("/proc/self/exe", error).string());
	CHECK(ProcessInfo::GetPath(0).empty());
}
