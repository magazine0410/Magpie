// The options of a scaling run built from a profile and the settings, and
// the form in which shortcuts go to the desktop portal.
#include <catch2/catch_test_macros.hpp>
#include "ProfileOptions.h"
#include "Shortcut.h"

using namespace Magpie;

TEST_CASE("Scaling options are built from a profile and the settings", "[ProfileOptions]") {
	Settings settings;
	settings.scalingModes.resize(2);
	settings.scalingModes[0].name = "Empty";
	settings.scalingModes[1].name = "Two effects";
	settings.scalingModes[1].effects.resize(2);
	settings.scalingModes[1].effects[0].name = "Lanczos";
	settings.scalingModes[1].effects[0].scalingType = ScalingType::Fit;
	settings.scalingModes[1].effects[1].name = "FSR\\FSR_RCAS";
	settings.scalingModes[1].effects[1].parameters["sharpness"] = 0.6f;

	Profile profile;
	profile.scalingMode = 1;
	ScalingOptions options;

	SECTION("the effects of the profile's scaling mode") {
		REQUIRE(BuildScalingOptions(settings, profile, false, options) == ScalingError::NoError);
		REQUIRE(options.effects.size() == 2);
		CHECK(options.effects[0].name == "Lanczos");
		CHECK(options.effects[0].scalingType == ScalingType::Fit);
		CHECK(options.effects[1].name == "FSR\\FSR_RCAS");
		CHECK(options.effects[1].parameters.at("sharpness") == 0.6f);
		CHECK_FALSE(options.IsWindowedMode());
	}

	SECTION("a missing or empty scaling mode is an error") {
		profile.scalingMode = -1;
		CHECK(BuildScalingOptions(settings, profile, false, options) == ScalingError::InvalidScalingMode);
		profile.scalingMode = 2;
		CHECK(BuildScalingOptions(settings, profile, false, options) == ScalingError::InvalidScalingMode);
		profile.scalingMode = 0;
		CHECK(BuildScalingOptions(settings, profile, false, options) == ScalingError::InvalidScalingMode);
	}

	SECTION("3D game mode cannot be windowed") {
		profile.Is3DGameMode(true);
		CHECK(BuildScalingOptions(settings, profile, true, options) == ScalingError::Windowed3DGameMode);
		CHECK(BuildScalingOptions(settings, profile, false, options) == ScalingError::NoError);
	}

	SECTION("options that only count when they are enabled") {
		profile.cropping = { 1, 2, 3, 4 };
		profile.maxFrameRate = 30.0f;
		profile.autoHideCursorDelay = 2.5f;
		REQUIRE(BuildScalingOptions(settings, profile, false, options) == ScalingError::NoError);
		CHECK(options.cropping == Cropping{});
		CHECK_FALSE(options.maxFrameRate.has_value());
		CHECK_FALSE(options.autoHideCursorDelay.has_value());

		profile.isCroppingEnabled = true;
		profile.isFrameRateLimiterEnabled = true;
		profile.isAutoHideCursorEnabled = true;
		REQUIRE(BuildScalingOptions(settings, profile, false, options) == ScalingError::NoError);
		CHECK(options.cropping == Cropping{ 1, 2, 3, 4 });
		CHECK(options.maxFrameRate == 30.0f);
		CHECK(options.autoHideCursorDelay == 2.5f);
	}

	SECTION("the minimum frame rate is not above the maximum") {
		settings.minFrameRate = 20.0f;
		REQUIRE(BuildScalingOptions(settings, profile, false, options) == ScalingError::NoError);
		CHECK(options.minFrameRate == 20.0f);

		profile.isFrameRateLimiterEnabled = true;
		profile.maxFrameRate = 15.0f;
		REQUIRE(BuildScalingOptions(settings, profile, false, options) == ScalingError::NoError);
		CHECK(options.minFrameRate == 15.0f);
	}

	SECTION("cursor scaling") {
		const std::pair<CursorScaling, float> cases[]{
			{ CursorScaling::x0_5, 0.5f }, { CursorScaling::NoScaling, 1.0f }, { CursorScaling::x2, 2.0f },
			{ CursorScaling::Source, 0.0f }, { CursorScaling::Custom, 1.7f }
		};
		profile.customCursorScaleFactor = 1.7f;
		for (const auto& [setting, factor] : cases) {
			profile.cursorScaling = setting;
			REQUIRE(BuildScalingOptions(settings, profile, false, options) == ScalingError::NoError);
			CHECK(options.cursorScaleFactor == factor);
		}
	}

	SECTION("global settings and the flags of the profile") {
		settings.isInlineParams = true;
		settings.isFP16Disabled = true;
		settings.isAllowScalingMaximized = true;
		settings.duplicateFrameDetectionMode = DuplicateFrameDetectionMode::Always;
		profile.IsAdjustCursorSpeed(false);
		REQUIRE(BuildScalingOptions(settings, profile, false, options) == ScalingError::NoError);
		CHECK(options.IsInlineParams());
		CHECK(options.IsFP16Disabled());
		CHECK(options.IsAllowScalingMaximized());
		CHECK_FALSE(options.IsEffectCacheDisabled());
		CHECK_FALSE(options.IsAdjustCursorSpeed());
		CHECK(options.duplicateFrameDetectionMode == DuplicateFrameDetectionMode::Always);
	}
}

TEST_CASE("Shortcuts are written the way the desktop portal reads them", "[ProfileOptions]") {
	// The default of the Scale action
	CHECK(ShortcutToPortalTrigger({ .code = 'A', .alt = true, .shift = true }) == "ALT+SHIFT+a");
	CHECK(ShortcutToPortalTrigger({ .code = 'Z', .win = true, .ctrl = true }) == "CTRL+LOGO+z");
	CHECK(ShortcutToPortalTrigger({ .code = '5', .ctrl = true }) == "CTRL+5");
	// VK_F1 and VK_F12
	CHECK(ShortcutToPortalTrigger({ .code = 0x70 }) == "F1");
	CHECK(ShortcutToPortalTrigger({ .code = 0x7B, .shift = true }) == "SHIFT+F12");
	// No shortcut, and a key without a known name
	CHECK(ShortcutToPortalTrigger({}) == "");
	CHECK(ShortcutToPortalTrigger({ .code = 0x2D, .ctrl = true }) == "");
}
