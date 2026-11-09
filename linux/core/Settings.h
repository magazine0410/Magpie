// The application settings as data. Linux counterpart of the data members of
// src/Magpie/AppSettings.h; change notification and saving in the background
// belong to the application.
#pragma once
#include <array>
#include <chrono>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>
#include "Profile.h"
#include "ScalingMode.h"
#include "Shortcut.h"

namespace Magpie {

enum class AppTheme {
	Light,
	Dark,
	System,
	COUNT
};

// Languages with a translation. Tags are lower case, as in config.json.
inline constexpr std::array<std::string_view, 19> SUPPORTED_LANGUAGES = {
	"ar", "de", "en-us", "es", "fi", "fr", "id", "it", "ja", "ko",
	"pl", "pt-br", "ru", "ta", "tr", "uk", "vi", "zh-hans", "zh-hant"
};

struct Settings {
	bool operator==(const Settings&) const noexcept = default;

	// The state of a first start: the built-in scaling modes, with the first
	// one as the global scaling mode.
	void SetInitialSettings() noexcept;

	// Indexed by ShortcutAction
	std::array<Shortcut, (size_t)ShortcutAction::COUNT_OR_NONE> shortcuts{
		Shortcut{.code = 'A', .alt = true, .shift = true },
		Shortcut{.code = 'Q', .alt = true, .shift = true },
		Shortcut{.code = 'D', .alt = true, .shift = true },
		Shortcut{.code = 'S', .alt = true, .shift = true }
	};

	std::vector<ScalingMode> scalingModes;

	Profile defaultProfile;
	std::vector<Profile> profiles;

	// One of SUPPORTED_LANGUAGES, or empty to follow the system.
	std::string language;

	// Centre of the main window, and its size independent of DPI. A negative
	// size means default position and size.
	float mainWindowCenterX = 0.0f;
	float mainWindowCenterY = 0.0f;
	float mainWindowWidth = -1.0f;
	float mainWindowHeight = -1.0f;

	AppTheme theme = AppTheme::System;
	// 1 to 5
	uint32_t countdownSeconds = 3;

	// When updates were last checked for automatically
	std::chrono::system_clock::time_point updateCheckDate;

	DuplicateFrameDetectionMode duplicateFrameDetectionMode = DuplicateFrameDetectionMode::Dynamic;

	float minFrameRate = 10.0f;

	std::map<std::string, OverlayWindowOption> overlayWindowOptions;

	bool isAlwaysRunAsAdmin = false;
	bool isDeveloperMode = false;
	bool isDebugMode = false;
	bool isBenchmarkMode = false;
	bool isTopmostDisabled = false;
	bool isEffectCacheDisabled = false;
	bool isFontCacheDisabled = false;
	bool isSaveEffectSources = false;
	bool isWarningsAreErrors = false;
	bool isAllowScalingMaximized = false;
	bool isKeepScreenOn = false;
	bool isSimulateExclusiveFullscreen = false;
	bool isInlineParams = false;
	bool isShowNotifyIcon = true;
	bool isMainWindowMaximized = false;
	bool isAutoCheckForUpdates = true;
	bool isCheckForPreviewUpdates = false;
	bool isStatisticsForDynamicDetectionEnabled = false;
	bool isFP16Disabled = false;
};

}
