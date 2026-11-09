// Linux counterpart of src/Magpie/Profile.h: the data of a profile. The
// actions of the Windows class (launching the program, opening its folder,
// resolving the screenshots folder) belong to the application.
#pragma once
#include <filesystem>
#include <string>
#include "ScalingOptions.h"

namespace Magpie {

enum class InitialWindowedScaleFactor {
	Auto,
	x1_25,
	x1_5,
	x1_75,
	x2,
	x3,
	Custom,
	COUNT
};

enum class CursorScaling {
	x0_5,
	x0_75,
	NoScaling,
	x1_25,
	x1_5,
	x2,
	Source,
	Custom,
	COUNT
};

enum class AutoScale {
	Disabled,
	Fullscreen,
	Windowed,
	COUNT
};

struct Profile {
	bool operator==(const Profile&) const noexcept = default;

	// Copies everything except the identity of the profile (name, rules,
	// launcher).
	void Copy(const Profile& other) noexcept {
		scalingMode = other.scalingMode;
		autoScale = other.autoScale;
		initialWindowedScaleFactor = other.initialWindowedScaleFactor;
		customInitialWindowedScaleFactor = other.customInitialWindowedScaleFactor;
		fullscreenInitialToolbarState = other.fullscreenInitialToolbarState;
		windowedInitialToolbarState = other.windowedInitialToolbarState;
		screenshotsDir = other.screenshotsDir;
		screenshotFilenameTemplate = other.screenshotFilenameTemplate;
		cursorScaling = other.cursorScaling;
		customCursorScaleFactor = other.customCursorScaleFactor;
		autoHideCursorDelay = other.autoHideCursorDelay;
		cropping = other.cropping;
		captureMethod = other.captureMethod;
		graphicsCardId = other.graphicsCardId;
		maxFrameRate = other.maxFrameRate;
		multiMonitorUsage = other.multiMonitorUsage;
		cursorInterpolationMode = other.cursorInterpolationMode;
		launchParameters = other.launchParameters;
		outputAlignment = other.outputAlignment;
		scalingFlags = other.scalingFlags;

		isCroppingEnabled = other.isCroppingEnabled;
		isFrameRateLimiterEnabled = other.isFrameRateLimiterEnabled;
		isAutoHideCursorEnabled = other.isAutoHideCursorEnabled;
	}

	DEFINE_FLAG_ACCESSOR(Is3DGameMode, ScalingFlags::Is3DGameMode, scalingFlags)
	DEFINE_FLAG_ACCESSOR(IsCaptureTitleBar, ScalingFlags::CaptureTitleBar, scalingFlags)
	DEFINE_FLAG_ACCESSOR(IsAdjustCursorSpeed, ScalingFlags::AdjustCursorSpeed, scalingFlags)
	DEFINE_FLAG_ACCESSOR(IsDirectFlipDisabled, ScalingFlags::DisableDirectFlip, scalingFlags)

	// The default profile has an empty name, pathRule and classNameRule.
	std::string name;

	// Path of the program's executable. For packaged Windows applications it
	// holds the AUMID.
	std::string pathRule;
	// Window class on Windows.
	std::string classNameRule;

	std::filesystem::path launcherPath;

	AutoScale autoScale = AutoScale::Disabled;

	InitialWindowedScaleFactor initialWindowedScaleFactor = InitialWindowedScaleFactor::Auto;
	float customInitialWindowedScaleFactor = 1.25f;

	ToolbarState fullscreenInitialToolbarState = ToolbarState::AutoHide;
	ToolbarState windowedInitialToolbarState = ToolbarState::AutoHide;
	// Empty means the system's screenshots folder.
	std::filesystem::path screenshotsDir;
	std::string screenshotFilenameTemplate;

	CursorScaling cursorScaling = CursorScaling::NoScaling;
	float customCursorScaleFactor = 1.0;

	// 0.1 to 5
	float autoHideCursorDelay = 3.0f;

	Cropping cropping{};
	// Index into the scaling modes, -1 for none
	int scalingMode = -1;
	CaptureMethod captureMethod = CaptureMethod::GraphicsCapture;
	GraphicsCardId graphicsCardId;
	MultiMonitorUsage multiMonitorUsage = MultiMonitorUsage::Closest;
	CursorInterpolationMode cursorInterpolationMode = CursorInterpolationMode::NearestNeighbor;

	// 10 to 1000
	float maxFrameRate = 60.0f;

	std::string launchParameters;
	OutputAlignment outputAlignment = OutputAlignment::Center;

	ScalingFlags scalingFlags = ScalingFlags::AdjustCursorSpeed;

	bool isPackaged = false;
	bool isCroppingEnabled = false;
	bool isFrameRateLimiterEnabled = false;
	bool isAutoHideCursorEnabled = false;
};

}
