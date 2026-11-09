#include "ScalingOptions.h"
#include <cassert>
#include <fmt/format.h>
#include "Logger.h"

namespace Magpie {

static std::string LogParameters(const std::map<std::string, float>& params) noexcept {
	std::string result;
	if (params.empty()) {
		result = "none";
	} else {
		for (const auto& pair : params) {
			result.append(fmt::format("\n\t\t\t\t{}: {}", pair.first, pair.second));
		}
	}

	return result;
}

static std::string LogEffects(const std::vector<EffectOption>& effects) noexcept {
	std::string result;
	for (const EffectOption& effect : effects) {
		result.append(fmt::format(R"(
		{}
			scalingType: {}
			scale: {},{}
			parameters: {})",
			effect.name,
			(int)effect.scalingType,
			effect.scale.first, effect.scale.second,
			LogParameters(effect.parameters)
		));
	}
	return result;
}

void ScalingOptions::Prepare() noexcept {
	assert(!effects.empty());
	assert(cropping.Left >= 0 && cropping.Top >= 0 &&
		cropping.Right >= 0 && cropping.Bottom >= 0);
	assert(minFrameRate >= 0);
	assert(!maxFrameRate.has_value() || *maxFrameRate > 0);
	assert(cursorScaleFactor >= 0);
	assert(!autoHideCursorDelay.has_value() || *autoHideCursorDelay > 0);
	assert(initialWindowedScaleFactor >= 0);
	assert(!overlayOptions.scaleShortcut.empty());
	assert(!overlayOptions.windowedModeScaleShortcut.empty());
	assert(!overlayOptions.takeScreenshotShortcut.empty());

	// GDI and DwmSharedSurface cannot capture the title bar
	IsCaptureTitleBar(IsCaptureTitleBar() &&
		captureMethod != CaptureMethod::GDI && captureMethod != CaptureMethod::DwmSharedSurface);

	if (IsWindowedMode()) {
		IsAllowScalingMaximized(false);
		IsSimulateExclusiveFullscreen(false);
	}

	if (Is3DGameMode()) {
		duplicateFrameDetectionMode = DuplicateFrameDetectionMode::Never;
	}

	// Default for screenshotFilenameTemplate. screenshotsDir may stay empty;
	// an error message is shown when a screenshot is taken.
	if (screenshotFilenameTemplate.empty()) {
		screenshotFilenameTemplate = "%PN:20%";
	}

	Logger::Get().Info(fmt::format(R"(Scaling options
	effects: {}
	IsWindowedMode: {}
	IsDebugMode: {}
	IsBenchmarkMode: {}
	IsTopmostDisabled: {}
	IsFP16Disabled: {}
	IsEffectCacheDisabled: {}
	IsFontCacheDisabled: {}
	IsSaveEffectSources: {}
	IsWarningsAreErrors: {}
	IsStatisticsForDynamicDetectionEnabled: {}
	IsInlineParams: {}
	IsTouchSupportEnabled: {}
	IsAllowScalingMaximized: {}
	IsSimulateExclusiveFullscreen: {}
	Is3DGameMode: {}
	IsCaptureTitleBar: {}
	IsAdjustCursorSpeed: {}
	IsDirectFlipDisabled: {}
	cropping: {},{},{},{}
	graphicsCardId:
		idx: {}
		vendorId: {}
		deviceId: {}
	minFrameRate: {}
	maxFrameRate: {}
	cursorScaleFactor: {}
	captureMethod: {}
	multiMonitorUsage: {}
	cursorInterpolationMode: {}
	autoHideCursorDelay: {}
	duplicateFrameDetectionMode: {}
	fullscreenInitialToolbarState: {}
	windowedInitialToolbarState: {}
	initialWindowedScaleFactor: {}
	screenshotsDir: {}
	screenshotFilenameTemplate: {})",
		LogEffects(effects),
		IsWindowedMode(),
		IsDebugMode(),
		IsBenchmarkMode(),
		IsTopmostDisabled(),
		IsFP16Disabled(),
		IsEffectCacheDisabled(),
		IsFontCacheDisabled(),
		IsSaveEffectSources(),
		IsWarningsAreErrors(),
		IsStatisticsForDynamicDetectionEnabled(),
		IsInlineParams(),
		IsTouchSupportEnabled(),
		IsAllowScalingMaximized(),
		IsSimulateExclusiveFullscreen(),
		Is3DGameMode(),
		IsCaptureTitleBar(),
		IsAdjustCursorSpeed(),
		IsDirectFlipDisabled(),
		cropping.Left, cropping.Top, cropping.Right, cropping.Bottom,
		graphicsCardId.idx,
		graphicsCardId.vendorId,
		graphicsCardId.deviceId,
		minFrameRate,
		maxFrameRate.value_or(0.0f),
		cursorScaleFactor,
		(int)captureMethod,
		(int)multiMonitorUsage,
		(int)cursorInterpolationMode,
		autoHideCursorDelay.value_or(0.0f),
		(int)duplicateFrameDetectionMode,
		(int)fullscreenInitialToolbarState,
		(int)windowedInitialToolbarState,
		initialWindowedScaleFactor,
		screenshotsDir.string(),
		screenshotFilenameTemplate
	));
}

}
