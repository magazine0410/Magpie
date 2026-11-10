// Turns a profile and the global settings into the options of one scaling
// run. Linux counterpart of the option handling in
// ScalingService::_StartScaleImpl (src/Magpie/ScalingService.cpp).
#pragma once
#include "Profile.h"
#include "ScalingOptions.h"
#include "Settings.h"

namespace Magpie {

// Returns ScalingError::InvalidScalingMode if the profile names no scaling
// mode or one without effects, and Windowed3DGameMode for that combination.
// Whether the effects exist is found out when they are compiled.
ScalingError BuildScalingOptions(
	const Settings& settings,
	const Profile& profile,
	bool windowedMode,
	ScalingOptions& options
) noexcept;

}
