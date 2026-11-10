// Command line parsing that the tools share: numbers, effect specifications,
// alignments, and scaling modes read from the configuration.
#pragma once
#include <algorithm>
#include <charconv>
#include <cstdio>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>
#include "ScalingMode.h"
#include "ScalingOptions.h"
#include "Settings.h"
#include "SettingsJson.h"
#include "SettingsStore.h"
#include "StrHelper.h"

namespace Magpie::ToolOptions {

inline bool ParseFloat(std::string_view text, float& value) noexcept {
	const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
	return ec == std::errc() && end == text.data() + text.size();
}

inline bool ParseInt(std::string_view text, int& value) noexcept {
	const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
	return ec == std::errc() && end == text.data() + text.size();
}

// "AxB" or, with allowSingle, "A" for both
inline bool ParsePair(std::string_view text, float& first, float& second, bool allowSingle) noexcept {
	const size_t delim = text.find('x');
	if (delim == std::string_view::npos) {
		if (!allowSingle || !ParseFloat(text, first)) {
			return false;
		}
		second = first;
		return true;
	}
	return ParseFloat(text.substr(0, delim), first) && ParseFloat(text.substr(delim + 1), second);
}

// "L,T,R,B", none of them negative
inline bool ParseCropping(std::string_view text, Cropping& cropping) noexcept {
	const auto parts = StrHelper::Split(text, ',');
	return parts.size() == 4 && ParseFloat(parts[0], cropping.Left) && ParseFloat(parts[1], cropping.Top) &&
		ParseFloat(parts[2], cropping.Right) && ParseFloat(parts[3], cropping.Bottom) &&
		cropping.Left >= 0 && cropping.Top >= 0 && cropping.Right >= 0 && cropping.Bottom >= 0;
}

// NAME[,KEY=VALUE]... NAME is the path below the effects directory without
// extension. KEY is "type" (normal, fit, absolute or fill), "scale" (X or
// XxY) or a parameter of the effect.
inline bool ParseEffectSpec(std::string_view spec, EffectOption& effect) noexcept {
	const auto parts = StrHelper::Split(spec, ',');
	if (parts.empty() || parts[0].empty()) {
		return false;
	}

	// Effect names use the Windows separator, as in config.json
	effect.name = parts[0];
	std::replace(effect.name.begin(), effect.name.end(), '/', '\\');

	for (size_t i = 1; i < parts.size(); ++i) {
		const size_t delim = parts[i].find('=');
		if (delim == std::string_view::npos) {
			return false;
		}
		const std::string_view key = parts[i].substr(0, delim);
		const std::string_view value = parts[i].substr(delim + 1);

		if (key == "type") {
			if (value == "normal") {
				effect.scalingType = ScalingType::Normal;
			} else if (value == "fit") {
				effect.scalingType = ScalingType::Fit;
			} else if (value == "absolute") {
				effect.scalingType = ScalingType::Absolute;
			} else if (value == "fill") {
				effect.scalingType = ScalingType::Fill;
			} else {
				return false;
			}
		} else if (key == "scale") {
			if (!ParsePair(value, effect.scale.first, effect.scale.second, true)) {
				return false;
			}
		} else {
			float number;
			if (key.empty() || !ParseFloat(value, number)) {
				return false;
			}
			effect.parameters[std::string(key)] = number;
		}
	}

	return true;
}

inline std::optional<OutputAlignment> ParseAlignment(std::string_view text) noexcept {
	static constexpr const char* NAMES[] = {
		"lefttop", "top", "righttop", "left", "center", "right", "leftbottom", "bottom", "rightbottom"
	};
	for (size_t i = 0; i < std::size(NAMES); ++i) {
		if (text == NAMES[i]) {
			return (OutputAlignment)i;
		}
	}
	return std::nullopt;
}

// The effects of scaling mode `modeName`, from `configFile` or, if that is
// null, from Magpie's own configuration. Prints the reason on failure.
inline bool LoadMode(const char* configFile, std::string_view modeName, std::vector<EffectOption>& effects) {
	Settings settings;
	if (configFile) {
		std::ifstream file(configFile, std::ios::binary);
		std::ostringstream content;
		content << file.rdbuf();
		if (!file || !ReadSettingsJson(content.str(), settings)) {
			std::fprintf(stderr, "Cannot read %s\n", configFile);
			return false;
		}
	} else if (SettingsStore::Load(settings) == SettingsStore::LoadResult::Failed) {
		std::fprintf(stderr, "Cannot read %s\n", SettingsStore::ConfigPath().c_str());
		return false;
	}

	for (const ScalingMode& mode : settings.scalingModes) {
		if (mode.name == modeName) {
			for (const EffectItem& item : mode.effects) {
				effects.push_back((EffectOption)item);
			}
			return true;
		}
	}

	std::fprintf(stderr, "There is no scaling mode \"%.*s\". Available:\n", (int)modeName.size(), modeName.data());
	for (const ScalingMode& mode : settings.scalingModes) {
		std::fprintf(stderr, "  %s\n", mode.name.c_str());
	}
	return false;
}

}
