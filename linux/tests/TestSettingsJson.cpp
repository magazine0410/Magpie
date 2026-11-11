// The fixtures in data/ were written by hand from the reading and writing code
// of the Windows application (src/Magpie/AppSettings.cpp at configuration
// version 5). They are not files produced by a Windows installation.
#include <catch2/catch_test_macros.hpp>
#include <rapidjson/document.h>
#include "SettingsJson.h"
#include "TestHelpers.h"

using namespace Magpie;

static std::string Fixture(const char* name) {
	const std::string text = ReadFile(std::filesystem::path(MAGPIE_TEST_DATA_DIR) / name);
	REQUIRE_FALSE(text.empty());
	return text;
}

// Lists every place where two JSON values differ, with its path.
static void Diff(const rapidjson::Value& expected, const rapidjson::Value& actual,
	const std::string& path, std::vector<std::string>& differences) {
	if (expected.IsObject() && actual.IsObject()) {
		for (const auto& member : expected.GetObject()) {
			const std::string childPath = path + "/" + member.name.GetString();
			auto found = actual.FindMember(member.name);
			if (found == actual.MemberEnd()) {
				differences.push_back(childPath + ": missing in output");
			} else {
				Diff(member.value, found->value, childPath, differences);
			}
		}
		for (const auto& member : actual.GetObject()) {
			if (!expected.HasMember(member.name)) {
				differences.push_back(path + "/" + member.name.GetString() + ": only in output");
			}
		}
	} else if (expected.IsArray() && actual.IsArray()) {
		if (expected.Size() != actual.Size()) {
			differences.push_back(path + ": array length " + std::to_string(expected.Size()) +
				" became " + std::to_string(actual.Size()));
			return;
		}
		for (rapidjson::SizeType i = 0; i < expected.Size(); ++i) {
			Diff(expected[i], actual[i], path + "[" + std::to_string(i) + "]", differences);
		}
	} else if (expected != actual) {
		// rapidjson compares numbers by value, so 960 equals 960.0
		differences.push_back(path + ": value changed");
	}
}

static std::vector<std::string> Diff(const std::string& expectedJson, const std::string& actualJson) {
	rapidjson::Document expected, actual;
	expected.Parse(expectedJson.c_str());
	actual.Parse(actualJson.c_str());
	REQUIRE_FALSE(expected.HasParseError());
	REQUIRE_FALSE(actual.HasParseError());

	std::vector<std::string> differences;
	Diff(expected, actual, "", differences);
	return differences;
}

TEST_CASE("A version 5 config is read into the settings", "[SettingsJson]") {
	Settings settings;
	REQUIRE(ReadSettingsJson(Fixture("windows-config-v5.json"), settings));

	CHECK(settings.language == "ja");
	CHECK(settings.theme == AppTheme::Dark);
	CHECK(settings.mainWindowCenterX == 960.0f);
	CHECK(settings.mainWindowCenterY == 540.5f);
	CHECK(settings.mainWindowWidth == 980.0f);
	CHECK(settings.mainWindowHeight == 700.0f);
	CHECK_FALSE(settings.isMainWindowMaximized);

	// 1658 = Ctrl (0x200) + Alt (0x400) + F11 (0x7A)
	const Shortcut& scale = settings.shortcuts[(size_t)ShortcutAction::Scale];
	CHECK(scale == Shortcut{ .code = 0x7A, .ctrl = true, .alt = true });
	// 2391 = Win (0x100) + Shift (0x800) + W (0x57)
	const Shortcut& windowed = settings.shortcuts[(size_t)ShortcutAction::WindowedModeScale];
	CHECK(windowed == Shortcut{ .code = 0x57, .win = true, .shift = true });
	CHECK(settings.shortcuts[(size_t)ShortcutAction::Toolbar] == Shortcut{ .code = 'D', .alt = true, .shift = true });
	CHECK(settings.shortcuts[(size_t)ShortcutAction::TakeScreenshot] == Shortcut{});

	CHECK(settings.countdownSeconds == 5);
	CHECK(settings.isDeveloperMode);
	CHECK(settings.isTopmostDisabled);
	CHECK(settings.isFontCacheDisabled);
	CHECK(settings.isAllowScalingMaximized);
	CHECK(settings.isKeepScreenOn);
	CHECK(settings.isAlwaysRunAsAdmin);
	CHECK_FALSE(settings.isShowNotifyIcon);
	CHECK(settings.isInlineParams);
	CHECK_FALSE(settings.isAutoCheckForUpdates);
	CHECK(settings.isCheckForPreviewUpdates);
	CHECK(settings.duplicateFrameDetectionMode == DuplicateFrameDetectionMode::Always);
	CHECK(settings.isStatisticsForDynamicDetectionEnabled);
	CHECK(settings.minFrameRate == 12.5f);
	CHECK(settings.isFP16Disabled);

	REQUIRE(settings.scalingModes.size() == 4);
	CHECK(settings.scalingModes[0].name == "Lanczos");
	CHECK(settings.scalingModes[0].effects[0].scalingType == ScalingType::Fit);
	CHECK(settings.scalingModes[1].effects[0].name == "FSR\\FSR_EASU");
	CHECK(settings.scalingModes[1].effects[1].parameters.at("sharpness") == 0.875f);
	CHECK(settings.scalingModes[2].name == "アニメ 2x");
	CHECK(settings.scalingModes[2].effects[1].scalingType == ScalingType::Absolute);
	CHECK(settings.scalingModes[2].effects[1].scale == std::pair(1920.0f, 1080.0f));
	CHECK(settings.scalingModes[2].effects[1].parameters.size() == 2);
	CHECK(settings.scalingModes[3].effects.empty());

	CHECK(settings.defaultProfile.name.empty());
	CHECK(settings.defaultProfile.scalingMode == 0);
	CHECK(settings.defaultProfile.IsAdjustCursorSpeed());

	REQUIRE(settings.profiles.size() == 2);
	const Profile& game = settings.profiles[0];
	CHECK(game.name == "月姫");
	CHECK_FALSE(game.isPackaged);
	CHECK(game.pathRule == "D:\\Games\\Tsukihime\\tsukihime.exe");
	CHECK(game.classNameRule == "SDL_app");
	// Windows paths are kept byte for byte
	CHECK(game.launcherPath.native() == "D:\\Games\\Tsukihime\\launcher.lnk");
	CHECK(game.screenshotsDir.native() == "D:\\Screenshots");
	CHECK(game.autoScale == AutoScale::Fullscreen);
	CHECK(game.launchParameters == "--window");
	CHECK(game.scalingMode == 2);
	CHECK(game.captureMethod == CaptureMethod::DesktopDuplication);
	CHECK(game.multiMonitorUsage == MultiMonitorUsage::All);
	CHECK(game.initialWindowedScaleFactor == InitialWindowedScaleFactor::Custom);
	CHECK(game.customInitialWindowedScaleFactor == 2.5f);
	CHECK(game.fullscreenInitialToolbarState == ToolbarState::Off);
	CHECK(game.windowedInitialToolbarState == ToolbarState::AlwaysShow);
	CHECK(game.screenshotFilenameTemplate == "%PN:20%_%Y%M%D");
	CHECK(game.graphicsCardId == GraphicsCardId{ .idx = 1, .vendorId = 4318, .deviceId = 9988 });
	CHECK(game.isFrameRateLimiterEnabled);
	CHECK(game.maxFrameRate == 144.0f);
	CHECK(game.Is3DGameMode());
	CHECK(game.IsCaptureTitleBar());
	CHECK_FALSE(game.IsAdjustCursorSpeed());
	CHECK(game.IsDirectFlipDisabled());
	CHECK(game.cursorScaling == CursorScaling::Custom);
	CHECK(game.customCursorScaleFactor == 1.5f);
	CHECK(game.cursorInterpolationMode == CursorInterpolationMode::Bilinear);
	CHECK(game.isAutoHideCursorEnabled);
	CHECK(game.autoHideCursorDelay == 2.5f);
	CHECK(game.isCroppingEnabled);
	CHECK(game.cropping == Cropping{ 8.0f, 31.0f, 8.0f, 8.0f });
	CHECK(game.outputAlignment == OutputAlignment::LeftTop);

	const Profile& packaged = settings.profiles[1];
	CHECK(packaged.isPackaged);
	CHECK(packaged.pathRule == "Microsoft.WindowsCalculator_8wekyb3d8bbwe!App");
	CHECK(packaged.autoScale == AutoScale::Windowed);
	CHECK(packaged.scalingMode == -1);
	CHECK(packaged.captureMethod == CaptureMethod::DwmSharedSurface);
	CHECK(packaged.outputAlignment == OutputAlignment::RightBottom);

	REQUIRE(settings.overlayWindowOptions.size() == 2);
	CHECK(settings.overlayWindowOptions.at("Toolbar") == OverlayWindowOption{ 1, 0, 0.5f, 8.0f });
	CHECK(settings.overlayWindowOptions.at("Profiler") == OverlayWindowOption{ 2, 2, 16.0f, 16.0f });
}

// The exit check of Phase 1.
TEST_CASE("A version 5 config is written back without losing or changing a member", "[SettingsJson]") {
	const std::string original = Fixture("windows-config-v5.json");

	Settings settings;
	REQUIRE(ReadSettingsJson(original, settings));
	const std::string written = WriteSettingsJson(settings);

	const std::vector<std::string> differences = Diff(original, written);
	for (const std::string& difference : differences) {
		UNSCOPED_INFO(difference);
	}
	CHECK(differences.empty());
}

TEST_CASE("Reading and writing again gives the same text", "[SettingsJson]") {
	Settings first;
	REQUIRE(ReadSettingsJson(Fixture("windows-config-v5.json"), first));
	const std::string firstText = WriteSettingsJson(first);

	Settings second;
	REQUIRE(ReadSettingsJson(firstText, second));

	CHECK(first == second);
	CHECK(WriteSettingsJson(second) == firstText);
}

TEST_CASE("The update check date keeps the Windows clock unit", "[SettingsJson]") {
	Settings settings;
	REQUIRE(ReadSettingsJson(Fixture("windows-config-v5.json"), settings));

	// 17624736000000000 units of 100 ns = 1762473600 s after the Unix epoch
	using namespace std::chrono;
	CHECK(settings.updateCheckDate == system_clock::time_point(seconds(1762473600)));

	Settings huge;
	REQUIRE(ReadSettingsJson(R"({"updateCheckDate": 9223372036854775807})", huge));
	CHECK(huge.updateCheckDate == system_clock::time_point());
}

TEST_CASE("Member names of older versions are accepted", "[SettingsJson]") {
	Settings settings;
	REQUIRE(ReadSettingsJson(Fixture("windows-config-legacy.json"), settings));

	// Tags are compared in lower case
	CHECK(settings.language == "zh-hans");
	CHECK(settings.theme == AppTheme::Light);

	// windowPos as top-left corner and pixel size
	CHECK(settings.mainWindowCenterX == 600.0f);
	CHECK(settings.mainWindowCenterY == 400.0f);
	CHECK(settings.mainWindowWidth == 1000.0f);
	CHECK(settings.mainWindowHeight == 700.0f);
	CHECK(settings.isMainWindowMaximized);

	// "hotkeys" and, inside it, "overlay" for the toolbar
	CHECK(settings.shortcuts[(size_t)ShortcutAction::Scale] == Shortcut{ .code = 'A', .ctrl = true });
	CHECK(settings.shortcuts[(size_t)ShortcutAction::Toolbar] == Shortcut{ .code = 0x7A, .ctrl = true, .alt = true });
	// Not in the file: the default stays
	CHECK(settings.shortcuts[(size_t)ShortcutAction::TakeScreenshot] == Shortcut{ .code = 'S', .alt = true, .shift = true });

	CHECK(settings.countdownSeconds == 4);		// downCount
	CHECK(settings.isAlwaysRunAsAdmin);			// alwaysRunAsElevated
	CHECK_FALSE(settings.isShowNotifyIcon);		// showTrayIcon

	// scalingProfiles, captureMode, graphicsAdapter (counted from 1), reserveTitleBar
	CHECK(settings.defaultProfile.scalingMode == 0);
	CHECK(settings.defaultProfile.captureMethod == CaptureMethod::GDI);
	CHECK(settings.defaultProfile.graphicsCardId.idx == 1);
	CHECK(settings.defaultProfile.IsCaptureTitleBar());

	REQUIRE(settings.profiles.size() == 1);
	const Profile& profile = settings.profiles[0];
	CHECK(profile.name == "Old game");
	CHECK(profile.autoScale == AutoScale::Fullscreen);	// boolean autoScale
	CHECK(profile.graphicsCardId.idx == 1);				// graphicsCard

	// Toolbar state and screenshots folder were global and go to every profile
	CHECK(settings.defaultProfile.fullscreenInitialToolbarState == ToolbarState::AlwaysShow);
	CHECK(settings.defaultProfile.windowedInitialToolbarState == ToolbarState::AlwaysShow);
	CHECK(settings.defaultProfile.screenshotsDir.native() == "C:\\Shots");
	CHECK(profile.fullscreenInitialToolbarState == ToolbarState::AlwaysShow);
	CHECK(profile.windowedInitialToolbarState == ToolbarState::AlwaysShow);
	CHECK(profile.screenshotsDir.native() == "C:\\Shots");
}

TEST_CASE("Invalid values fall back to defaults", "[SettingsJson]") {
	Settings settings;
	REQUIRE(ReadSettingsJson(R"({
		"language": "tlh",
		"theme": 7,
		"countdownSeconds": 9,
		"minFrameRate": "fast",
		"duplicateFrameDetectionMode": 12,
		"shortcuts": { "scale": 70000, "toolbar": "D" },
		"scalingModes": [
			"not an object",
			{ "name": "One", "effects": [
				17,
				{ "name": "Nearest", "scalingType": 9, "scale": { "x": -2, "y": 2 }, "parameters": { "a": "b", "c": 1.5 } }
			] }
		],
		"profiles": [
			{ "scalingMode": 5, "maxFrameRate": 5, "autoHideCursorDelay": 60, "customCursorScaling": -1,
			  "customInitialWindowedScaleFactor": 0.5, "captureMethod": 99,
			  "cropping": { "left": 1, "top": -1, "right": 1, "bottom": 1 } },
			{ "name": "   ", "packaged": false, "pathRule": "/usr/bin/a", "classNameRule": "a" },
			{ "name": "No rules", "packaged": false },
			{ "name": "No packaged flag", "pathRule": "/usr/bin/b", "classNameRule": "b" },
			42,
			{ "name": "Valid", "packaged": false, "pathRule": "/usr/bin/c", "classNameRule": "c", "autoScale": 8 }
		],
		"overlay": { "windows": { "Toolbar": { "hArea": 70000, "vArea": 2 }, "Broken": 3 } }
	})", settings));

	CHECK(settings.language.empty());
	CHECK(settings.theme == AppTheme::System);
	CHECK(settings.countdownSeconds == 3);
	CHECK(settings.minFrameRate == 10.0f);
	CHECK(settings.duplicateFrameDetectionMode == DuplicateFrameDetectionMode::Dynamic);
	// Out of range and wrong type: both keep the default
	CHECK(settings.shortcuts[(size_t)ShortcutAction::Scale] == Shortcut{ .code = 'A', .alt = true, .shift = true });
	CHECK(settings.shortcuts[(size_t)ShortcutAction::Toolbar] == Shortcut{ .code = 'D', .alt = true, .shift = true });

	REQUIRE(settings.scalingModes.size() == 1);
	REQUIRE(settings.scalingModes[0].effects.size() == 1);
	const EffectItem& effect = settings.scalingModes[0].effects[0];
	CHECK(effect.scalingType == ScalingType::Normal);
	CHECK(effect.scale == std::pair(1.0f, 1.0f));
	CHECK(effect.parameters == std::map<std::string, float>{ { "c", 1.5f } });

	const Profile& defaultProfile = settings.defaultProfile;
	CHECK(defaultProfile.scalingMode == -1);
	CHECK(defaultProfile.maxFrameRate == 60.0f);
	CHECK(defaultProfile.autoHideCursorDelay == 3.0f);
	CHECK(defaultProfile.customCursorScaleFactor == 1.0f);
	CHECK(defaultProfile.customInitialWindowedScaleFactor == 1.0f);
	CHECK(defaultProfile.captureMethod == CaptureMethod::GraphicsCapture);
	CHECK(defaultProfile.cropping == Cropping{});

	// Only the complete profile survives
	REQUIRE(settings.profiles.size() == 1);
	CHECK(settings.profiles[0].name == "Valid");
	CHECK(settings.profiles[0].autoScale == AutoScale::Disabled);

	REQUIRE(settings.overlayWindowOptions.size() == 1);
	CHECK(settings.overlayWindowOptions.at("Toolbar") == OverlayWindowOption{ 0, 2, 0.0f, 0.0f });
}

TEST_CASE("Text that is not a JSON object is rejected", "[SettingsJson]") {
	Settings settings;
	const Settings untouched;

	CHECK_FALSE(ReadSettingsJson("{ \"theme\": ", settings));
	CHECK_FALSE(ReadSettingsJson("[1, 2, 3]", settings));
	CHECK_FALSE(ReadSettingsJson("", settings));
	CHECK(settings == untouched);
}

TEST_CASE("An empty object leaves every default in place", "[SettingsJson]") {
	Settings settings;
	REQUIRE(ReadSettingsJson("{}", settings));
	CHECK(settings == Settings());
}

TEST_CASE("The initial settings survive a round trip", "[SettingsJson]") {
	Settings initial;
	initial.SetInitialSettings();
	REQUIRE(initial.scalingModes.size() == 7);
	CHECK(initial.defaultProfile.scalingMode == 0);

	Settings loaded;
	REQUIRE(ReadSettingsJson(WriteSettingsJson(initial), loaded));
	CHECK(loaded == initial);
}

TEST_CASE("A profile with native paths keeps them", "[SettingsJson]") {
	Settings settings;
	Profile& profile = settings.profiles.emplace_back();
	profile.name = "Native";
	profile.pathRule = "/opt/games/game/bin/game";
	profile.classNameRule = "game";
	profile.launcherPath = "/opt/games/game/start.sh";
	profile.screenshotsDir = "/home/user/Pictures/Screenshots";

	Settings loaded;
	REQUIRE(ReadSettingsJson(WriteSettingsJson(settings), loaded));
	CHECK(loaded == settings);
}

TEST_CASE("A relative launcher path from an old config becomes absolute", "[SettingsJson]") {
	Settings settings;
	REQUIRE(ReadSettingsJson(R"({ "profiles": [ {}, {
		"name": "Relative", "packaged": false,
		"pathRule": "/opt/games/game/bin/game", "classNameRule": "game",
		"launcherPath": "../start.sh"
	} ] })", settings));

	REQUIRE(settings.profiles.size() == 1);
	CHECK(settings.profiles[0].launcherPath == "/opt/games/game/start.sh");
}

TEST_CASE("A profile keeps how its program is started in a nested session", "[settings]") {
	// Only the Linux version writes and reads these fields
	Settings settings;
	REQUIRE(ReadSettingsJson(R"({ "profiles": [
		{},
		{ "name": "Game", "packaged": false, "pathRule": "/usr/bin/game", "classNameRule": "game",
			"launchNested": true, "nestedWidth": 800, "nestedHeight": 600, "allowTearing": true },
		{ "name": "Other", "packaged": false, "pathRule": "/usr/bin/other", "classNameRule": "other" },
		{ "name": "Wrong", "packaged": false, "pathRule": "/usr/bin/wrong", "classNameRule": "wrong",
			"launchNested": true, "nestedWidth": 4, "nestedHeight": 600 }
	] })", settings));
	REQUIRE(settings.profiles.size() == 3);
	CHECK(settings.profiles[0].isLaunchedNested);
	CHECK(settings.profiles[0].nestedWidth == 800);
	CHECK(settings.profiles[0].nestedHeight == 600);
	CHECK(settings.profiles[0].isTearingAllowed);
	// Without them: as on Windows, and a size for when the user turns it on
	CHECK_FALSE(settings.profiles[1].isLaunchedNested);
	CHECK(settings.profiles[1].nestedWidth == 1280);
	CHECK(settings.profiles[1].nestedHeight == 720);
	CHECK_FALSE(settings.profiles[1].isTearingAllowed);
	// A size that no session can have gives way to that default
	CHECK(settings.profiles[2].isLaunchedNested);
	CHECK(settings.profiles[2].nestedWidth == 1280);
	CHECK(settings.profiles[2].nestedHeight == 720);

	Settings again;
	REQUIRE(ReadSettingsJson(WriteSettingsJson(settings), again));
	CHECK(again.profiles == settings.profiles);

	// A profile that copies another takes them along
	Profile copy;
	copy.Copy(settings.profiles[0]);
	CHECK(copy.isLaunchedNested);
	CHECK(copy.nestedWidth == 800);
	CHECK(copy.isTearingAllowed);
}
