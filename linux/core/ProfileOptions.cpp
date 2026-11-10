#include "ProfileOptions.h"
#include <algorithm>

namespace Magpie {

ScalingError BuildScalingOptions(
	const Settings& settings,
	const Profile& profile,
	bool windowedMode,
	ScalingOptions& options
) noexcept {
	if (profile.scalingMode < 0 || (size_t)profile.scalingMode >= settings.scalingModes.size()) {
		return ScalingError::InvalidScalingMode;
	}

	const std::vector<EffectItem>& effects = settings.scalingModes[(size_t)profile.scalingMode].effects;
	if (effects.empty()) {
		return ScalingError::InvalidScalingMode;
	}

	if (profile.Is3DGameMode() && windowedMode) {
		return ScalingError::Windowed3DGameMode;
	}

	options = {};
	options.effects.reserve(effects.size());
	for (const EffectItem& effectItem : effects) {
		options.effects.push_back((EffectOption)effectItem);
	}

	options.graphicsCardId = profile.graphicsCardId;
	options.captureMethod = profile.captureMethod;
	if (profile.isFrameRateLimiterEnabled) {
		options.maxFrameRate = profile.maxFrameRate;
	}
	options.multiMonitorUsage = profile.multiMonitorUsage;
	options.outputAlignment = profile.outputAlignment;
	options.cursorInterpolationMode = profile.cursorInterpolationMode;
	options.flags = profile.scalingFlags;

	options.IsWindowedMode(windowedMode);

	switch (profile.initialWindowedScaleFactor) {
	case InitialWindowedScaleFactor::x1_25:
		options.initialWindowedScaleFactor = 1.25f;
		break;
	case InitialWindowedScaleFactor::x1_5:
		options.initialWindowedScaleFactor = 1.5f;
		break;
	case InitialWindowedScaleFactor::x1_75:
		options.initialWindowedScaleFactor = 1.75f;
		break;
	case InitialWindowedScaleFactor::x2:
		options.initialWindowedScaleFactor = 2.0f;
		break;
	case InitialWindowedScaleFactor::x3:
		options.initialWindowedScaleFactor = 3.0f;
		break;
	case InitialWindowedScaleFactor::Custom:
		options.initialWindowedScaleFactor = profile.customInitialWindowedScaleFactor;
		break;
	default:
		// Auto
		options.initialWindowedScaleFactor = 0.0f;
		break;
	}

	options.fullscreenInitialToolbarState = profile.fullscreenInitialToolbarState;
	options.windowedInitialToolbarState = profile.windowedInitialToolbarState;
	options.screenshotsDir = profile.screenshotsDir;
	options.screenshotFilenameTemplate = profile.screenshotFilenameTemplate;

	if (profile.isCroppingEnabled) {
		options.cropping = profile.cropping;
	}

	switch (profile.cursorScaling) {
	case CursorScaling::x0_5:
		options.cursorScaleFactor = 0.5f;
		break;
	case CursorScaling::x0_75:
		options.cursorScaleFactor = 0.75f;
		break;
	case CursorScaling::x1_25:
		options.cursorScaleFactor = 1.25f;
		break;
	case CursorScaling::x1_5:
		options.cursorScaleFactor = 1.5f;
		break;
	case CursorScaling::x2:
		options.cursorScaleFactor = 2.0f;
		break;
	case CursorScaling::Source:
		// Zero or less: scaled like the source window
		options.cursorScaleFactor = 0.0f;
		break;
	case CursorScaling::Custom:
		options.cursorScaleFactor = profile.customCursorScaleFactor;
		break;
	default:
		// NoScaling
		options.cursorScaleFactor = 1.0f;
		break;
	}

	if (profile.isAutoHideCursorEnabled) {
		options.autoHideCursorDelay = profile.autoHideCursorDelay;
	}

	// The global settings
	options.IsDeveloperMode(settings.isDeveloperMode);
	options.IsDebugMode(settings.isDebugMode);
	options.IsBenchmarkMode(settings.isBenchmarkMode);
	options.IsTopmostDisabled(settings.isTopmostDisabled);
	options.IsEffectCacheDisabled(settings.isEffectCacheDisabled);
	options.IsFontCacheDisabled(settings.isFontCacheDisabled);
	options.IsSaveEffectSources(settings.isSaveEffectSources);
	options.IsWarningsAreErrors(settings.isWarningsAreErrors);
	options.IsAllowScalingMaximized(settings.isAllowScalingMaximized);
	options.IsKeepScreenOn(settings.isKeepScreenOn);
	options.IsSimulateExclusiveFullscreen(settings.isSimulateExclusiveFullscreen);
	options.duplicateFrameDetectionMode = settings.duplicateFrameDetectionMode;
	options.IsStatisticsForDynamicDetectionEnabled(settings.isStatisticsForDynamicDetectionEnabled);
	options.IsInlineParams(settings.isInlineParams);
	options.IsFP16Disabled(settings.isFP16Disabled);

	if (options.maxFrameRate) {
		// The minimum frame rate cannot be above the maximum
		options.minFrameRate = std::min(settings.minFrameRate, *options.maxFrameRate);
	} else {
		options.minFrameRate = settings.minFrameRate;
	}

	return ScalingError::NoError;
}

}
