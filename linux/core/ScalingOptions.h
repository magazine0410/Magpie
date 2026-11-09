// Linux counterpart of src/Magpie.Core/include/ScalingOptions.h.
//
// The enumerations keep the Windows values, because they are stored as numbers
// in config.json and a Windows configuration must stay readable. Which capture
// methods exist on Linux is decided when the capture back ends are written.
//
// Not ported yet: the showToast, showError and save callbacks. They take a
// window handle on Windows, and the Linux window identifier is defined together
// with the compositor bridge.
#pragma once
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>
#include "CommonDefines.h"

namespace Magpie {

enum class CaptureMethod {
	GraphicsCapture,
	DesktopDuplication,
	GDI,
	DwmSharedSurface,
	COUNT
};

enum class MultiMonitorUsage {
	Closest,
	Intersected,
	All,
	COUNT
};

enum class CursorInterpolationMode {
	NearestNeighbor,
	Bilinear,
	COUNT
};

struct Cropping {
	bool operator==(const Cropping&) const noexcept = default;

	float Left;
	float Top;
	float Right;
	float Bottom;
};

struct GraphicsCardId {
	bool operator==(const GraphicsCardId&) const noexcept = default;

	// idx is the position in the adapter list; vendorId and deviceId verify
	// it. If they do not match, the list is searched for a match, which
	// handles a changed set of graphics cards. idx also tells two cards of the
	// same model apart.
	// idx -1 means the default card. If vendorId and deviceId are set as well,
	// the chosen card is currently absent and is used again once it returns.
	int idx = -1;
	uint32_t vendorId = 0;
	uint32_t deviceId = 0;
};

enum class OutputAlignment {
	LeftTop,
	Top,
	RightTop,
	Left,
	Center,
	Right,
	LeftBottom,
	Bottom,
	RightBottom,
	COUNT
};

enum class ScalingType {
	Normal,		// scale is a factor
	Fit,		// scale is relative to the largest size that fits the screen with the aspect ratio kept
	Absolute,	// scale is the target size in pixels
	Fill,		// fill the screen; scale is not used
	COUNT
};

struct EffectOption {
	bool operator==(const EffectOption&) const noexcept = default;

	bool HasScale() const noexcept {
		return scalingType != ScalingType::Normal ||
			!IsApprox(scale.first, 1.0f) || !IsApprox(scale.second, 1.0f);
	}

	std::string name;
	std::map<std::string, float> parameters;
	ScalingType scalingType = ScalingType::Normal;
	std::pair<float, float> scale = { 1.0f,1.0f };
};

enum class DuplicateFrameDetectionMode {
	Always,
	Dynamic,
	Never,
	COUNT
};

enum class ToolbarState {
	Off,
	AlwaysShow,
	AutoHide,
	COUNT
};

struct OverlayWindowOption {
	bool operator==(const OverlayWindowOption&) const noexcept = default;

	// 0: left, hPos is the distance between the window's left edge and the
	//    image's left edge (all distances before DPI scaling)
	// 1: centre, hPos is the ratio of the window centre's distance from the
	//    image's left edge to the image width
	// 2: right, hPos is the distance between the window's right edge and the
	//    image's right edge
	uint16_t hArea = 0;
	// The same vertically: 0 top, 1 centre, 2 bottom.
	uint16_t vArea = 0;
	float hPos = 0.0f;
	float vPos = 0.0f;
};

struct OverlayOptions {
	std::map<std::string, OverlayWindowOption> windows;
	std::string scaleShortcut;
	std::string windowedModeScaleShortcut;
	std::string takeScreenshotShortcut;
};

enum class ScalingError {
	NoError,

	// Preconditions

	// No scaling mode is configured, or it is invalid
	InvalidScalingMode,
	// Enabling touch support failed
	TouchSupport,
	// Windowed scaling is not available in 3D game mode
	Windowed3DGameMode,
	// Desktop Duplication does not support windowed scaling
	WindowedDesktopDuplication,
	// The window cannot be scaled
	InvalidSourceWindow,
	// The window is maximized or fullscreen; a setting can force scaling
	Maximized,
	// The window has a higher integrity level
	LowIntegrityLevel,
	// The size left after custom cropping is too small or negative
	InvalidCropping,
	// The window does not qualify for windowed scaling, for example maximized
	BannedInWindowedMode,

	// Initialisation and run time

	ScalingFailedGeneral,
	// The frame source could not be initialised
	CaptureFailed,
	// ID3D11Device5::CreateFence failed
	CreateFenceFailed
};

enum class ScalingFlags {
	None,
	WindowedMode = 1,
	DebugMode = 1 << 1,
	DisableEffectCache = 1 << 2,
	SaveEffectSources = 1 << 3,
	WarningsAreErrors = 1 << 4,
	KeepScreenOn = 1 << 5,
	SimulateExclusiveFullscreen = 1 << 6,
	Is3DGameMode = 1 << 7,
	CaptureTitleBar = 1 << 8,
	AdjustCursorSpeed = 1 << 9,
	DisableDirectFlip = 1 << 10,
	DisableFontCache = 1 << 11,
	AllowScalingMaximized = 1 << 12,
	EnableStatisticsForDynamicDetection = 1 << 13,
	// Only changes scaling behaviour; the core does not start a touch helper
	TouchSupportEnabled = 1 << 14,
	InlineParams = 1 << 15,
	DisableFP16 = 1 << 16,
	BenchmarkMode = 1 << 17,
	DeveloperMode = 1 << 18,
	DisableTopmost = 1 << 19
};
DEFINE_ENUM_FLAG_OPERATORS(ScalingFlags)

struct ScalingOptions {
	DEFINE_FLAG_ACCESSOR(IsWindowedMode, ScalingFlags::WindowedMode, flags)
	DEFINE_FLAG_ACCESSOR(IsDebugMode, ScalingFlags::DebugMode, flags)
	DEFINE_FLAG_ACCESSOR(IsEffectCacheDisabled, ScalingFlags::DisableEffectCache, flags)
	DEFINE_FLAG_ACCESSOR(IsSaveEffectSources, ScalingFlags::SaveEffectSources, flags)
	DEFINE_FLAG_ACCESSOR(IsWarningsAreErrors, ScalingFlags::WarningsAreErrors, flags)
	DEFINE_FLAG_ACCESSOR(IsKeepScreenOn, ScalingFlags::KeepScreenOn, flags)
	DEFINE_FLAG_ACCESSOR(IsSimulateExclusiveFullscreen, ScalingFlags::SimulateExclusiveFullscreen, flags)
	DEFINE_FLAG_ACCESSOR(Is3DGameMode, ScalingFlags::Is3DGameMode, flags)
	DEFINE_FLAG_ACCESSOR(IsCaptureTitleBar, ScalingFlags::CaptureTitleBar, flags)
	DEFINE_FLAG_ACCESSOR(IsAdjustCursorSpeed, ScalingFlags::AdjustCursorSpeed, flags)
	DEFINE_FLAG_ACCESSOR(IsDirectFlipDisabled, ScalingFlags::DisableDirectFlip, flags)
	DEFINE_FLAG_ACCESSOR(IsFontCacheDisabled, ScalingFlags::DisableFontCache, flags)
	DEFINE_FLAG_ACCESSOR(IsAllowScalingMaximized, ScalingFlags::AllowScalingMaximized, flags)
	DEFINE_FLAG_ACCESSOR(IsStatisticsForDynamicDetectionEnabled, ScalingFlags::EnableStatisticsForDynamicDetection, flags)
	DEFINE_FLAG_ACCESSOR(IsTouchSupportEnabled, ScalingFlags::TouchSupportEnabled, flags)
	DEFINE_FLAG_ACCESSOR(IsInlineParams, ScalingFlags::InlineParams, flags)
	DEFINE_FLAG_ACCESSOR(IsFP16Disabled, ScalingFlags::DisableFP16, flags)
	DEFINE_FLAG_ACCESSOR(IsBenchmarkMode, ScalingFlags::BenchmarkMode, flags)
	DEFINE_FLAG_ACCESSOR(IsDeveloperMode, ScalingFlags::DeveloperMode, flags)
	DEFINE_FLAG_ACCESSOR(IsTopmostDisabled, ScalingFlags::DisableTopmost, flags)

	std::vector<EffectOption> effects;
	ScalingFlags flags = ScalingFlags::AdjustCursorSpeed;
	Cropping cropping{};
	GraphicsCardId graphicsCardId;
	float minFrameRate = 0.0f;
	std::optional<float> maxFrameRate;
	float cursorScaleFactor = 1.0f;
	CaptureMethod captureMethod = CaptureMethod::GraphicsCapture;
	MultiMonitorUsage multiMonitorUsage = MultiMonitorUsage::Closest;
	OutputAlignment outputAlignment = OutputAlignment::Center;
	CursorInterpolationMode cursorInterpolationMode = CursorInterpolationMode::NearestNeighbor;
	std::optional<float> autoHideCursorDelay;
	DuplicateFrameDetectionMode duplicateFrameDetectionMode = DuplicateFrameDetectionMode::Dynamic;
	ToolbarState fullscreenInitialToolbarState = ToolbarState::AutoHide;
	ToolbarState windowedInitialToolbarState = ToolbarState::AutoHide;
	float initialWindowedScaleFactor = 0.0f;
	std::filesystem::path screenshotsDir;
	std::string screenshotFilenameTemplate;

	// May be changed while scaling
	OverlayOptions overlayOptions;

	// Checks the options, resolves combinations that exclude each other and
	// writes the result to the log. Call once before scaling starts.
	void Prepare() noexcept;
};

}
