#include <catch2/catch_test_macros.hpp>
#include "ScalingOptions.h"

using namespace Magpie;

static ScalingOptions MakeOptions() {
	ScalingOptions options;
	EffectOption& effect = options.effects.emplace_back();
	effect.name = "Lanczos";
	effect.scalingType = ScalingType::Fit;
	options.overlayOptions.scaleShortcut = "Alt+Shift+A";
	options.overlayOptions.windowedModeScaleShortcut = "Alt+Shift+Q";
	options.overlayOptions.takeScreenshotShortcut = "Alt+Shift+S";
	return options;
}

TEST_CASE("Prepare switches off options that windowed mode excludes", "[ScalingOptions]") {
	ScalingOptions options = MakeOptions();
	options.IsWindowedMode(true);
	options.IsAllowScalingMaximized(true);
	options.IsSimulateExclusiveFullscreen(true);

	options.Prepare();

	CHECK_FALSE(options.IsAllowScalingMaximized());
	CHECK_FALSE(options.IsSimulateExclusiveFullscreen());
	CHECK(options.IsWindowedMode());
}

TEST_CASE("Prepare disables duplicate frame detection in 3D game mode", "[ScalingOptions]") {
	ScalingOptions options = MakeOptions();
	options.Is3DGameMode(true);

	options.Prepare();

	CHECK(options.duplicateFrameDetectionMode == DuplicateFrameDetectionMode::Never);
}

TEST_CASE("Prepare supplies the default screenshot file name", "[ScalingOptions]") {
	ScalingOptions options = MakeOptions();
	options.Prepare();
	CHECK(options.screenshotFilenameTemplate == "%PN:20%");

	ScalingOptions custom = MakeOptions();
	custom.screenshotFilenameTemplate = "shot";
	custom.Prepare();
	CHECK(custom.screenshotFilenameTemplate == "shot");
}

TEST_CASE("Flag accessors set and clear single bits", "[ScalingOptions]") {
	ScalingOptions options;
	// The default has cursor speed adjustment on
	CHECK(options.IsAdjustCursorSpeed());

	options.IsDebugMode(true);
	CHECK(options.IsDebugMode());
	CHECK(options.IsAdjustCursorSpeed());

	options.IsAdjustCursorSpeed(false);
	CHECK_FALSE(options.IsAdjustCursorSpeed());
	CHECK(options.IsDebugMode());
}

TEST_CASE("An effect has a scale unless it is Normal with factor 1", "[ScalingOptions]") {
	EffectOption effect;
	CHECK_FALSE(effect.HasScale());

	effect.scale = { 2.0f, 2.0f };
	CHECK(effect.HasScale());

	effect.scale = { 1.0f, 1.0f };
	effect.scalingType = ScalingType::Fit;
	CHECK(effect.HasScale());
}
