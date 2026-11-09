// Linux counterpart of src/Magpie/ScalingMode.h.
#pragma once
#include <map>
#include <string>
#include <utility>
#include <vector>
#include "ScalingOptions.h"

namespace Magpie {

struct EffectItem {
	bool operator==(const EffectItem&) const noexcept = default;

	bool HasScale() const noexcept {
		return scalingType != ScalingType::Normal ||
			!IsApprox(scale.first, 1.0f) || !IsApprox(scale.second, 1.0f);
	}

	explicit operator EffectOption() const noexcept {
		return EffectOption{
			.name = name,
			.parameters = parameters,
			.scalingType = scalingType,
			.scale = scale
		};
	}

	// Path below the effects directory without extension, with backslashes as
	// separators as on Windows (for example "FSR\FSR_EASU").
	std::string name;
	std::map<std::string, float> parameters;
	ScalingType scalingType = ScalingType::Normal;
	std::pair<float, float> scale = { 1.0f,1.0f };
};

struct ScalingMode {
	bool operator==(const ScalingMode&) const noexcept = default;

	std::string name;
	std::vector<EffectItem> effects;
};

}
