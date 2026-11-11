// Port of the reading and writing code in src/Magpie/AppSettings.cpp,
// ScalingModesService.cpp and JsonHelper.cpp. Member names, their order and
// the handling of invalid and legacy values follow the Windows code.
#include "SettingsJson.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <fmt/format.h>
#include <rapidjson/document.h>
#include <rapidjson/prettywriter.h>
#include <rapidjson/stringbuffer.h>
#include "Logger.h"
#include "StrHelper.h"

namespace Magpie {

using JsonObject = rapidjson::GenericObject<true, rapidjson::Value>;
using JsonWriter = rapidjson::PrettyWriter<rapidjson::StringBuffer>;

// Windows stores updateCheckDate as the tick count of its system clock:
// 100 ns units since the Unix epoch.
using WindowsTicks = std::chrono::duration<int64_t, std::ratio<1, 10'000'000>>;

////////////////////////////////////////////////////////////////////////////////
// Reading helpers. Each returns false if the member has the wrong type, or if
// it is missing and `required` is set.
////////////////////////////////////////////////////////////////////////////////

static bool ReadBool(const JsonObject& obj, const char* name, bool& result, bool required = false) noexcept {
	auto node = obj.FindMember(name);
	if (node == obj.MemberEnd()) {
		return !required;
	}

	if (!node->value.IsBool()) {
		return false;
	}

	result = node->value.GetBool();
	return true;
}

static bool ReadBoolFlag(
	const JsonObject& obj,
	const char* name,
	ScalingFlags flagBit,
	ScalingFlags& flags,
	bool required = false
) noexcept {
	bool value = false;
	auto node = obj.FindMember(name);
	if (node == obj.MemberEnd()) {
		return !required;
	}

	if (!ReadBool(obj, name, value, true)) {
		return false;
	}

	if (value) {
		flags |= flagBit;
	} else {
		flags &= ~flagBit;
	}
	return true;
}

static bool ReadUInt(const JsonObject& obj, const char* name, uint32_t& result, bool required = false) noexcept {
	auto node = obj.FindMember(name);
	if (node == obj.MemberEnd()) {
		return !required;
	}

	if (!node->value.IsUint()) {
		return false;
	}

	result = node->value.GetUint();
	return true;
}

// A value outside the enumeration leaves `result` unchanged.
template <typename Enum>
static bool ReadEnum(const JsonObject& obj, const char* name, Enum& result, bool required = false) noexcept {
	auto node = obj.FindMember(name);
	if (node == obj.MemberEnd()) {
		return !required;
	}

	if (!node->value.IsUint()) {
		return false;
	}

	const uint32_t value = node->value.GetUint();
	if (value < (uint32_t)Enum::COUNT) {
		result = (Enum)value;
	}

	return true;
}

static bool ReadUInt16(const JsonObject& obj, const char* name, uint16_t& result, bool required = false) noexcept {
	uint32_t value = result;
	if (!ReadUInt(obj, name, value, required)) {
		return false;
	}

	if (value > std::numeric_limits<uint16_t>::max()) {
		return false;
	}

	result = (uint16_t)value;
	return true;
}

static bool ReadInt(const JsonObject& obj, const char* name, int& result, bool required = false) noexcept {
	auto node = obj.FindMember(name);
	if (node == obj.MemberEnd()) {
		return !required;
	}

	if (!node->value.IsInt()) {
		return false;
	}

	result = node->value.GetInt();
	return true;
}

static bool ReadInt64(const JsonObject& obj, const char* name, int64_t& result, bool required = false) noexcept {
	auto node = obj.FindMember(name);
	if (node == obj.MemberEnd()) {
		return !required;
	}

	if (!node->value.IsInt64()) {
		return false;
	}

	result = node->value.GetInt64();
	return true;
}

static bool ReadFloat(const JsonObject& obj, const char* name, float& result, bool required = false) noexcept {
	auto node = obj.FindMember(name);
	if (node == obj.MemberEnd()) {
		return !required;
	}

	if (!node->value.IsNumber()) {
		return false;
	}

	result = node->value.GetFloat();
	return true;
}

static bool ReadString(const JsonObject& obj, const char* name, std::string& result, bool required = false) noexcept {
	auto node = obj.FindMember(name);
	if (node == obj.MemberEnd()) {
		return !required;
	}

	if (!node->value.IsString()) {
		return false;
	}

	result.assign(node->value.GetString(), node->value.GetStringLength());
	return true;
}

static bool ReadPath(const JsonObject& obj, const char* name, std::filesystem::path& result) noexcept {
	std::string value;
	if (!ReadString(obj, name, value)) {
		return false;
	}

	// Stored byte for byte. A Windows path such as "D:\Games" is one opaque
	// file name to std::filesystem on Linux, so it survives a round trip.
	result = std::move(value);
	return true;
}

////////////////////////////////////////////////////////////////////////////////
// Shortcuts
////////////////////////////////////////////////////////////////////////////////

// A shortcut is stored as a number, not as text, because some keys share
// their character (the full stop and the numeric keypad's decimal point).
static uint32_t EncodeShortcut(const Shortcut& shortcut) noexcept {
	uint32_t value = shortcut.code;
	if (shortcut.win) {
		value |= 0x100;
	}
	if (shortcut.ctrl) {
		value |= 0x200;
	}
	if (shortcut.alt) {
		value |= 0x400;
	}
	if (shortcut.shift) {
		value |= 0x800;
	}
	return value;
}

static void DecodeShortcut(uint32_t value, Shortcut& shortcut) noexcept {
	if (value > 0xfff) {
		return;
	}

	shortcut.code = value & 0xff;
	shortcut.win = value & 0x100;
	shortcut.ctrl = value & 0x200;
	shortcut.alt = value & 0x400;
	shortcut.shift = value & 0x800;
}

static void ReadShortcut(const JsonObject& obj, const char* name, const char* legacyName, Shortcut& shortcut) noexcept {
	auto node = obj.FindMember(name);
	if (node == obj.MemberEnd() && legacyName) {
		node = obj.FindMember(legacyName);
	}

	if (node != obj.MemberEnd() && node->value.IsUint()) {
		DecodeShortcut(node->value.GetUint(), shortcut);
	}
}

////////////////////////////////////////////////////////////////////////////////
// Scaling modes
////////////////////////////////////////////////////////////////////////////////

static void WriteScalingMode(JsonWriter& writer, const ScalingMode& scalingMode) noexcept {
	writer.StartObject();
	writer.Key("name");
	writer.String(scalingMode.name.c_str());
	if (!scalingMode.effects.empty()) {
		writer.Key("effects");
		writer.StartArray();
		for (const EffectItem& effect : scalingMode.effects) {
			writer.StartObject();
			writer.Key("name");
			writer.String(effect.name.c_str());

			if (effect.HasScale()) {
				writer.Key("scalingType");
				writer.Uint((uint32_t)effect.scalingType);
				writer.Key("scale");
				writer.StartObject();
				writer.Key("x");
				writer.Double(effect.scale.first);
				writer.Key("y");
				writer.Double(effect.scale.second);
				writer.EndObject();
			}

			if (!effect.parameters.empty()) {
				writer.Key("parameters");
				writer.StartObject();
				for (const auto& [name, value] : effect.parameters) {
					writer.Key(name.c_str());
					writer.Double(value);
				}
				writer.EndObject();
			}

			writer.EndObject();
		}
		writer.EndArray();
	}
	writer.EndObject();
}

// While loading config.json (not `strict`) invalid parts are skipped, and
// false is returned only if the scaling mode as a whole is unusable. While
// importing (`strict`) any invalid part gives false.
static bool ReadScalingMode(const JsonObject& scalingModeObj, ScalingMode& scalingMode, bool strict = false) noexcept {
	if (!ReadString(scalingModeObj, "name", scalingMode.name)) {
		return false;
	}

	auto effectsNode = scalingModeObj.FindMember("effects");
	if (effectsNode == scalingModeObj.MemberEnd()) {
		return true;
	}
	if (!effectsNode->value.IsArray()) {
		return !strict;
	}

	auto effectsArray = effectsNode->value.GetArray();
	scalingMode.effects.reserve(effectsArray.Size());

	for (const auto& elem : effectsArray) {
		if (!elem.IsObject()) {
			if (strict) {
				return false;
			}
			continue;
		}

		auto elemObj = elem.GetObject();
		EffectItem& effect = scalingMode.effects.emplace_back();

		if (!ReadString(elemObj, "name", effect.name)) {
			if (strict) {
				return false;
			}
			scalingMode.effects.pop_back();
			continue;
		}

		if (!ReadEnum(elemObj, "scalingType", effect.scalingType) && strict) {
			return false;
		}

		auto scaleNode = elemObj.FindMember("scale");
		if (scaleNode != elemObj.MemberEnd()) {
			bool isValid = false;
			if (scaleNode->value.IsObject()) {
				auto scaleObj = scaleNode->value.GetObject();

				float x, y;
				if (ReadFloat(scaleObj, "x", x, true) && ReadFloat(scaleObj, "y", y, true) && x > 0 && y > 0) {
					effect.scale = { x,y };
					isValid = true;
				}
			}
			if (!isValid && strict) {
				return false;
			}
		}

		auto parametersNode = elemObj.FindMember("parameters");
		if (parametersNode != elemObj.MemberEnd()) {
			if (!parametersNode->value.IsObject()) {
				if (strict) {
					return false;
				}
				continue;
			}

			for (const auto& param : parametersNode->value.GetObject()) {
				if (!param.value.IsNumber()) {
					if (strict) {
						return false;
					}
					continue;
				}

				effect.parameters[param.name.GetString()] = param.value.GetFloat();
			}
		}
	}

	return true;
}

std::string WriteScalingModesJson(const std::vector<ScalingMode>& scalingModes) noexcept {
	rapidjson::StringBuffer json;
	JsonWriter writer(json);
	writer.StartObject();
	writer.Key("scalingModes");
	writer.StartArray();
	for (const ScalingMode& scalingMode : scalingModes) {
		WriteScalingMode(writer, scalingMode);
	}
	writer.EndArray();
	writer.EndObject();
	return { json.GetString(), json.GetSize() };
}

// Port of ScalingModesService::Import for a file the user chose
bool ReadScalingModesJson(std::string_view json, std::vector<ScalingMode>& scalingModes) noexcept {
	rapidjson::Document doc;
	doc.Parse<rapidjson::kParseCommentsFlag | rapidjson::kParseTrailingCommasFlag>(json.data(), json.size());
	if (doc.HasParseError() || !doc.IsObject()) {
		return false;
	}

	const JsonObject root = static_cast<const rapidjson::Document&>(doc).GetObject();
	auto scalingModesNode = root.FindMember("scalingModes");
	if (scalingModesNode == root.MemberEnd()) {
		return true;
	}
	if (!scalingModesNode->value.IsArray()) {
		return false;
	}

	std::vector<ScalingMode> imported;
	for (const auto& elem : scalingModesNode->value.GetArray()) {
		if (!elem.IsObject() || !ReadScalingMode(elem.GetObject(), imported.emplace_back(), true)) {
			return false;
		}
	}

	scalingModes.insert(scalingModes.end(),
		std::make_move_iterator(imported.begin()), std::make_move_iterator(imported.end()));
	return true;
}

static void ReadScalingModes(const JsonObject& root, Settings& settings) noexcept {
	auto scalingModesNode = root.FindMember("scalingModes");
	if (scalingModesNode == root.MemberEnd() || !scalingModesNode->value.IsArray()) {
		return;
	}

	auto scalingModesArray = scalingModesNode->value.GetArray();
	settings.scalingModes.reserve(scalingModesArray.Size());

	for (const auto& elem : scalingModesArray) {
		if (!elem.IsObject()) {
			continue;
		}

		if (!ReadScalingMode(elem.GetObject(), settings.scalingModes.emplace_back())) {
			settings.scalingModes.pop_back();
		}
	}
}

////////////////////////////////////////////////////////////////////////////////
// Profiles
////////////////////////////////////////////////////////////////////////////////

static void WriteProfile(JsonWriter& writer, const Profile& profile) noexcept {
	writer.StartObject();
	if (!profile.name.empty()) {
		writer.Key("name");
		writer.String(profile.name.c_str());
		writer.Key("packaged");
		writer.Bool(profile.isPackaged);
		writer.Key("pathRule");
		writer.String(profile.pathRule.c_str());
		writer.Key("classNameRule");
		writer.String(profile.classNameRule.c_str());
		writer.Key("launcherPath");
		writer.String(profile.launcherPath.c_str());
		writer.Key("autoScale");
		writer.Uint((uint32_t)profile.autoScale);
		writer.Key("launchParameters");
		writer.String(profile.launchParameters.c_str());
		// Only the Linux version has and reads these three. They are left
		// out while they have their defaults, so that a configuration from
		// Windows is written back as it was.
		const Profile defaults;
		if (profile.isLaunchedNested) {
			writer.Key("launchNested");
			writer.Bool(true);
		}
		if (profile.nestedWidth != defaults.nestedWidth || profile.nestedHeight != defaults.nestedHeight) {
			writer.Key("nestedWidth");
			writer.Uint(profile.nestedWidth);
			writer.Key("nestedHeight");
			writer.Uint(profile.nestedHeight);
		}
	}

	writer.Key("scalingMode");
	writer.Int(profile.scalingMode);
	writer.Key("captureMethod");
	writer.Uint((uint32_t)profile.captureMethod);
	writer.Key("multiMonitorUsage");
	writer.Uint((uint32_t)profile.multiMonitorUsage);

	writer.Key("initialWindowedScaleFactor");
	writer.Uint((uint32_t)profile.initialWindowedScaleFactor);
	writer.Key("customInitialWindowedScaleFactor");
	writer.Double(profile.customInitialWindowedScaleFactor);

	writer.Key("fullscreenInitialToolbarState");
	writer.Uint((uint32_t)profile.fullscreenInitialToolbarState);
	writer.Key("windowedInitialToolbarState");
	writer.Uint((uint32_t)profile.windowedInitialToolbarState);
	writer.Key("screenshotsDir");
	writer.String(profile.screenshotsDir.c_str());
	writer.Key("screenshotFilenameTemplate");
	writer.String(profile.screenshotFilenameTemplate.c_str());

	writer.Key("graphicsCardId");
	writer.StartObject();
	writer.Key("idx");
	writer.Int(profile.graphicsCardId.idx);
	writer.Key("vendorId");
	writer.Uint(profile.graphicsCardId.vendorId);
	writer.Key("deviceId");
	writer.Uint(profile.graphicsCardId.deviceId);
	writer.EndObject();
	writer.Key("frameRateLimiterEnabled");
	writer.Bool(profile.isFrameRateLimiterEnabled);
	writer.Key("maxFrameRate");
	writer.Double(profile.maxFrameRate);

	writer.Key("3DGameMode");
	writer.Bool(profile.Is3DGameMode());
	writer.Key("captureTitleBar");
	writer.Bool(profile.IsCaptureTitleBar());
	writer.Key("adjustCursorSpeed");
	writer.Bool(profile.IsAdjustCursorSpeed());
	writer.Key("disableDirectFlip");
	writer.Bool(profile.IsDirectFlipDisabled());

	writer.Key("cursorScaling");
	writer.Uint((uint32_t)profile.cursorScaling);
	writer.Key("customCursorScaling");
	writer.Double(profile.customCursorScaleFactor);
	writer.Key("cursorInterpolationMode");
	writer.Uint((uint32_t)profile.cursorInterpolationMode);
	writer.Key("autoHideCursorEnabled");
	writer.Bool(profile.isAutoHideCursorEnabled);
	writer.Key("autoHideCursorDelay");
	writer.Double(profile.autoHideCursorDelay);

	writer.Key("croppingEnabled");
	writer.Bool(profile.isCroppingEnabled);
	writer.Key("cropping");
	writer.StartObject();
	writer.Key("left");
	writer.Double(profile.cropping.Left);
	writer.Key("top");
	writer.Double(profile.cropping.Top);
	writer.Key("right");
	writer.Double(profile.cropping.Right);
	writer.Key("bottom");
	writer.Double(profile.cropping.Bottom);
	writer.EndObject();

	writer.Key("outputAlignment");
	writer.Uint((uint32_t)profile.outputAlignment);

	writer.EndObject();
}

// Reading the default profile never fails. Any other profile is rejected if
// its name or one of its rules is missing.
static bool ReadProfile(
	const JsonObject& profileObj,
	Profile& profile,
	size_t scalingModeCount,
	bool isDefault = false
) noexcept {
	if (!isDefault) {
		if (!ReadString(profileObj, "name", profile.name, true)) {
			return false;
		}

		{
			std::string_view nameView(profile.name);
			StrHelper::Trim(nameView);
			if (nameView.empty()) {
				return false;
			}
		}

		if (!ReadBool(profileObj, "packaged", profile.isPackaged, true)) {
			return false;
		}

		if (!ReadString(profileObj, "pathRule", profile.pathRule, true) || profile.pathRule.empty()) {
			return false;
		}

		if (!ReadString(profileObj, "classNameRule", profile.classNameRule, true) || profile.classNameRule.empty()) {
			return false;
		}

		ReadPath(profileObj, "launcherPath", profile.launcherPath);

		ReadBool(profileObj, "launchNested", profile.isLaunchedNested);
		ReadUInt(profileObj, "nestedWidth", profile.nestedWidth);
		ReadUInt(profileObj, "nestedHeight", profile.nestedHeight);
		if (profile.nestedWidth < 64 || profile.nestedWidth > 16384 ||
			profile.nestedHeight < 64 || profile.nestedHeight > 16384) {
			profile.nestedWidth = 1280;
			profile.nestedHeight = 720;
		}

		// Old versions stored the launcher relative to the program. This only
		// has an effect for native paths: a Windows path contains no separator
		// that std::filesystem recognises on Linux and is left as it is.
		if (!profile.launcherPath.empty() && profile.launcherPath.is_relative()) {
			const std::filesystem::path exePath(profile.pathRule);
			profile.launcherPath = (exePath.parent_path() / profile.launcherPath).lexically_normal();
		}

		{
			auto autoScaleNode = profileObj.FindMember("autoScale");
			if (autoScaleNode != profileObj.MemberEnd()) {
				if (autoScaleNode->value.IsUint()) {
					uint32_t value = autoScaleNode->value.GetUint();
					if (value >= (uint32_t)AutoScale::COUNT) {
						value = (uint32_t)AutoScale::Disabled;
					}
					profile.autoScale = (AutoScale)value;
				} else if (autoScaleNode->value.IsBool()) {
					// A boolean before v0.12
					profile.autoScale = autoScaleNode->value.GetBool() ?
						AutoScale::Fullscreen : AutoScale::Disabled;
				}
			}
		}

		ReadString(profileObj, "launchParameters", profile.launchParameters);
	}

	ReadInt(profileObj, "scalingMode", profile.scalingMode);
	if (profile.scalingMode < -1 || profile.scalingMode >= (int)scalingModeCount) {
		profile.scalingMode = -1;
	}

	if (!ReadEnum(profileObj, "captureMethod", profile.captureMethod, true)) {
		// v0.10.0-preview1 used captureMode
		ReadEnum(profileObj, "captureMode", profile.captureMethod);
	}

	ReadEnum(profileObj, "multiMonitorUsage", profile.multiMonitorUsage);
	ReadEnum(profileObj, "initialWindowedScaleFactor", profile.initialWindowedScaleFactor);

	ReadFloat(profileObj, "customInitialWindowedScaleFactor", profile.customInitialWindowedScaleFactor);
	if (profile.customInitialWindowedScaleFactor < 1.0f) {
		profile.customInitialWindowedScaleFactor = 1.0f;
	}

	ReadEnum(profileObj, "fullscreenInitialToolbarState", profile.fullscreenInitialToolbarState);
	ReadEnum(profileObj, "windowedInitialToolbarState", profile.windowedInitialToolbarState);

	ReadPath(profileObj, "screenshotsDir", profile.screenshotsDir);

	// Not checked for validity
	ReadString(profileObj, "screenshotFilenameTemplate", profile.screenshotFilenameTemplate);

	{
		auto graphicsCardIdNode = profileObj.FindMember("graphicsCardId");
		if (graphicsCardIdNode == profileObj.MemberEnd()) {
			// v0.10 and v0.11 stored only the index
			int graphicsCardIdx = -1;
			if (!ReadInt(profileObj, "graphicsCard", graphicsCardIdx, true)) {
				// v0.10.0-preview1 used graphicsAdapter, counted from 1
				uint32_t graphicsAdapter = 0;
				ReadUInt(profileObj, "graphicsAdapter", graphicsAdapter);
				graphicsCardIdx = (int)graphicsAdapter - 1;
			}

			// vendorId and deviceId are filled in later from the adapter list
			profile.graphicsCardId.idx = graphicsCardIdx;
		} else if (graphicsCardIdNode->value.IsObject()) {
			auto graphicsCardIdObj = graphicsCardIdNode->value.GetObject();

			ReadInt(graphicsCardIdObj, "idx", profile.graphicsCardId.idx);
			ReadUInt(graphicsCardIdObj, "vendorId", profile.graphicsCardId.vendorId);
			ReadUInt(graphicsCardIdObj, "deviceId", profile.graphicsCardId.deviceId);
		}
	}

	ReadBool(profileObj, "frameRateLimiterEnabled", profile.isFrameRateLimiterEnabled);
	ReadFloat(profileObj, "maxFrameRate", profile.maxFrameRate);
	if (profile.maxFrameRate <= 10.0f - FLOAT_EPSILON<float> ||
		profile.maxFrameRate >= 1000.0f + FLOAT_EPSILON<float>)
	{
		profile.maxFrameRate = 60.0f;
	}

	ReadBoolFlag(profileObj, "3DGameMode", ScalingFlags::Is3DGameMode, profile.scalingFlags);
	if (!ReadBoolFlag(profileObj, "captureTitleBar", ScalingFlags::CaptureTitleBar, profile.scalingFlags, true)) {
		// v0.10.0-preview1 used reserveTitleBar
		ReadBoolFlag(profileObj, "reserveTitleBar", ScalingFlags::CaptureTitleBar, profile.scalingFlags);
	}
	ReadBoolFlag(profileObj, "adjustCursorSpeed", ScalingFlags::AdjustCursorSpeed, profile.scalingFlags);
	ReadBoolFlag(profileObj, "disableDirectFlip", ScalingFlags::DisableDirectFlip, profile.scalingFlags);

	ReadEnum(profileObj, "cursorScaling", profile.cursorScaling);

	ReadFloat(profileObj, "customCursorScaling", profile.customCursorScaleFactor);
	if (profile.customCursorScaleFactor < 0) {
		profile.customCursorScaleFactor = 1.0f;
	}

	ReadEnum(profileObj, "cursorInterpolationMode", profile.cursorInterpolationMode);
	ReadBool(profileObj, "autoHideCursorEnabled", profile.isAutoHideCursorEnabled);
	ReadFloat(profileObj, "autoHideCursorDelay", profile.autoHideCursorDelay);
	if (profile.autoHideCursorDelay <= 0.1f - FLOAT_EPSILON<float> ||
		profile.autoHideCursorDelay >= 5.0f + FLOAT_EPSILON<float>)
	{
		profile.autoHideCursorDelay = 3.0f;
	}

	ReadBool(profileObj, "croppingEnabled", profile.isCroppingEnabled);

	auto croppingNode = profileObj.FindMember("cropping");
	if (croppingNode != profileObj.MemberEnd() && croppingNode->value.IsObject()) {
		auto croppingObj = croppingNode->value.GetObject();

		if (!ReadFloat(croppingObj, "left", profile.cropping.Left, true)
			|| profile.cropping.Left < 0
			|| !ReadFloat(croppingObj, "top", profile.cropping.Top, true)
			|| profile.cropping.Top < 0
			|| !ReadFloat(croppingObj, "right", profile.cropping.Right, true)
			|| profile.cropping.Right < 0
			|| !ReadFloat(croppingObj, "bottom", profile.cropping.Bottom, true)
			|| profile.cropping.Bottom < 0
		) {
			profile.cropping = {};
		}
	}

	ReadEnum(profileObj, "outputAlignment", profile.outputAlignment);

	return true;
}

////////////////////////////////////////////////////////////////////////////////
// Whole file
////////////////////////////////////////////////////////////////////////////////

std::string WriteSettingsJson(const Settings& settings) noexcept {
	rapidjson::StringBuffer json;
	JsonWriter writer(json);
	writer.StartObject();

	writer.Key("language");
	writer.String(settings.language.c_str());

	writer.Key("theme");
	writer.Uint((uint32_t)settings.theme);

	writer.Key("windowPos");
	writer.StartObject();
	writer.Key("centerX");
	writer.Double(settings.mainWindowCenterX);
	writer.Key("centerY");
	writer.Double(settings.mainWindowCenterY);
	writer.Key("width");
	writer.Double(settings.mainWindowWidth);
	writer.Key("height");
	writer.Double(settings.mainWindowHeight);
	writer.Key("maximized");
	writer.Bool(settings.isMainWindowMaximized);
	writer.EndObject();

	writer.Key("shortcuts");
	writer.StartObject();
	writer.Key("scale");
	writer.Uint(EncodeShortcut(settings.shortcuts[(size_t)ShortcutAction::Scale]));
	writer.Key("windowedModeScale");
	writer.Uint(EncodeShortcut(settings.shortcuts[(size_t)ShortcutAction::WindowedModeScale]));
	writer.Key("toolbar");
	writer.Uint(EncodeShortcut(settings.shortcuts[(size_t)ShortcutAction::Toolbar]));
	writer.Key("takeScreenshot");
	writer.Uint(EncodeShortcut(settings.shortcuts[(size_t)ShortcutAction::TakeScreenshot]));
	writer.EndObject();

	writer.Key("countdownSeconds");
	writer.Uint(settings.countdownSeconds);
	writer.Key("developerMode");
	writer.Bool(settings.isDeveloperMode);
	writer.Key("debugMode");
	writer.Bool(settings.isDebugMode);
	writer.Key("benchmarkMode");
	writer.Bool(settings.isBenchmarkMode);
	writer.Key("disableTopmost");
	writer.Bool(settings.isTopmostDisabled);
	writer.Key("disableEffectCache");
	writer.Bool(settings.isEffectCacheDisabled);
	writer.Key("disableFontCache");
	writer.Bool(settings.isFontCacheDisabled);
	writer.Key("saveEffectSources");
	writer.Bool(settings.isSaveEffectSources);
	writer.Key("warningsAreErrors");
	writer.Bool(settings.isWarningsAreErrors);
	writer.Key("allowScalingMaximized");
	writer.Bool(settings.isAllowScalingMaximized);
	writer.Key("keepScreenOn");
	writer.Bool(settings.isKeepScreenOn);
	writer.Key("simulateExclusiveFullscreen");
	writer.Bool(settings.isSimulateExclusiveFullscreen);
	writer.Key("alwaysRunAsAdmin");
	writer.Bool(settings.isAlwaysRunAsAdmin);
	writer.Key("showNotifyIcon");
	writer.Bool(settings.isShowNotifyIcon);
	writer.Key("inlineParams");
	writer.Bool(settings.isInlineParams);
	writer.Key("autoCheckForUpdates");
	writer.Bool(settings.isAutoCheckForUpdates);
	writer.Key("checkForPreviewUpdates");
	writer.Bool(settings.isCheckForPreviewUpdates);
	writer.Key("updateCheckDate");
	writer.Int64(std::chrono::duration_cast<WindowsTicks>(settings.updateCheckDate.time_since_epoch()).count());
	writer.Key("duplicateFrameDetectionMode");
	writer.Uint((uint32_t)settings.duplicateFrameDetectionMode);
	writer.Key("enableStatisticsForDynamicDetection");
	writer.Bool(settings.isStatisticsForDynamicDetectionEnabled);
	writer.Key("minFrameRate");
	writer.Double(settings.minFrameRate);
	writer.Key("disableFP16");
	writer.Bool(settings.isFP16Disabled);

	writer.Key("scalingModes");
	writer.StartArray();
	for (const ScalingMode& scalingMode : settings.scalingModes) {
		WriteScalingMode(writer, scalingMode);
	}
	writer.EndArray();

	writer.Key("profiles");
	writer.StartArray();
	WriteProfile(writer, settings.defaultProfile);
	for (const Profile& profile : settings.profiles) {
		WriteProfile(writer, profile);
	}
	writer.EndArray();

	writer.Key("overlay");
	writer.StartObject();
	writer.Key("windows");
	writer.StartObject();
	for (const auto& [name, windowOption] : settings.overlayWindowOptions) {
		writer.Key(name.c_str());
		writer.StartObject();
		writer.Key("hArea");
		writer.Uint(windowOption.hArea);
		writer.Key("vArea");
		writer.Uint(windowOption.vArea);
		writer.Key("hPos");
		writer.Double(windowOption.hPos);
		writer.Key("vPos");
		writer.Double(windowOption.vPos);
		writer.EndObject();
	}
	writer.EndObject();
	writer.EndObject();

	writer.EndObject();

	return std::string(json.GetString(), json.GetLength());
}

// Never fails: an invalid item is ignored without a message.
static void ReadSettings(const JsonObject& root, Settings& settings) noexcept {
	{
		std::string language;
		ReadString(root, "language", language);
		StrHelper::ToLowerCase(language);
		// An unknown language falls back to the system's
		if (std::find(SUPPORTED_LANGUAGES.begin(), SUPPORTED_LANGUAGES.end(), language) != SUPPORTED_LANGUAGES.end()) {
			settings.language = std::move(language);
		} else {
			settings.language.clear();
		}
	}

	settings.theme = AppTheme::System;
	ReadEnum(root, "theme", settings.theme);

	auto windowPosNode = root.FindMember("windowPos");
	if (windowPosNode != root.MemberEnd() && windowPosNode->value.IsObject()) {
		auto windowPosObj = windowPosNode->value.GetObject();

		float centerX = 0, centerY = 0, width = 0, height = 0;
		if (ReadFloat(windowPosObj, "centerX", centerX, true) &&
			ReadFloat(windowPosObj, "centerY", centerY, true) &&
			ReadFloat(windowPosObj, "width", width, true) &&
			ReadFloat(windowPosObj, "height", height, true)) {
			settings.mainWindowCenterX = centerX;
			settings.mainWindowCenterY = centerY;
			settings.mainWindowWidth = width;
			settings.mainWindowHeight = height;
		} else {
			// Older versions stored the top-left corner and a size in pixels.
			// Windows divides the size by the monitor's DPI factor here; the
			// monitor is unknown on Linux, so a factor of 1 is assumed.
			int x = 0;
			int y = 0;
			uint32_t pixelWidth = 0;
			uint32_t pixelHeight = 0;
			if (ReadInt(windowPosObj, "x", x, true) &&
				ReadInt(windowPosObj, "y", y, true) &&
				ReadUInt(windowPosObj, "width", pixelWidth, true) &&
				ReadUInt(windowPosObj, "height", pixelHeight, true)) {
				settings.mainWindowCenterX = x + pixelWidth / 2.0f;
				settings.mainWindowCenterY = y + pixelHeight / 2.0f;
				settings.mainWindowWidth = (float)pixelWidth;
				settings.mainWindowHeight = (float)pixelHeight;
			}
		}

		ReadBool(windowPosObj, "maximized", settings.isMainWindowMaximized);
	}

	auto shortcutsNode = root.FindMember("shortcuts");
	if (shortcutsNode == root.MemberEnd()) {
		// v0.10.0-preview1 used hotkeys
		shortcutsNode = root.FindMember("hotkeys");
	}
	if (shortcutsNode != root.MemberEnd() && shortcutsNode->value.IsObject()) {
		auto shortcutsObj = shortcutsNode->value.GetObject();

		ReadShortcut(shortcutsObj, "scale", nullptr, settings.shortcuts[(size_t)ShortcutAction::Scale]);
		ReadShortcut(shortcutsObj, "windowedModeScale", nullptr, settings.shortcuts[(size_t)ShortcutAction::WindowedModeScale]);
		// Called overlay before v0.12
		ReadShortcut(shortcutsObj, "toolbar", "overlay", settings.shortcuts[(size_t)ShortcutAction::Toolbar]);
		ReadShortcut(shortcutsObj, "takeScreenshot", nullptr, settings.shortcuts[(size_t)ShortcutAction::TakeScreenshot]);
	}

	if (!ReadUInt(root, "countdownSeconds", settings.countdownSeconds, true)) {
		// v0.10.0-preview1 used downCount
		ReadUInt(root, "downCount", settings.countdownSeconds);
	}
	if (settings.countdownSeconds == 0 || settings.countdownSeconds > 5) {
		settings.countdownSeconds = 3;
	}
	ReadBool(root, "developerMode", settings.isDeveloperMode);
	ReadBool(root, "debugMode", settings.isDebugMode);
	ReadBool(root, "benchmarkMode", settings.isBenchmarkMode);
	ReadBool(root, "disableTopmost", settings.isTopmostDisabled);
	ReadBool(root, "disableEffectCache", settings.isEffectCacheDisabled);
	ReadBool(root, "disableFontCache", settings.isFontCacheDisabled);
	ReadBool(root, "saveEffectSources", settings.isSaveEffectSources);
	ReadBool(root, "warningsAreErrors", settings.isWarningsAreErrors);
	ReadBool(root, "allowScalingMaximized", settings.isAllowScalingMaximized);
	ReadBool(root, "keepScreenOn", settings.isKeepScreenOn);
	ReadBool(root, "simulateExclusiveFullscreen", settings.isSimulateExclusiveFullscreen);
	if (!ReadBool(root, "alwaysRunAsAdmin", settings.isAlwaysRunAsAdmin, true)) {
		// v0.10.0-preview1 used alwaysRunAsElevated
		ReadBool(root, "alwaysRunAsElevated", settings.isAlwaysRunAsAdmin);
	}
	if (!ReadBool(root, "showNotifyIcon", settings.isShowNotifyIcon, true)) {
		// v0.10 used showTrayIcon
		ReadBool(root, "showTrayIcon", settings.isShowNotifyIcon);
	}
	ReadBool(root, "inlineParams", settings.isInlineParams);
	ReadBool(root, "autoCheckForUpdates", settings.isAutoCheckForUpdates);
	ReadBool(root, "checkForPreviewUpdates", settings.isCheckForPreviewUpdates);
	{
		int64_t ticks = 0;
		ReadInt64(root, "updateCheckDate", ticks);

		// A value that cannot be represented by this system's clock is dropped.
		using std::chrono::system_clock;
		const int64_t maxTicks = std::chrono::duration_cast<WindowsTicks>(system_clock::duration::max()).count();
		if (ticks > maxTicks || ticks < -maxTicks) {
			ticks = 0;
		}
		settings.updateCheckDate = system_clock::time_point(
			std::chrono::duration_cast<system_clock::duration>(WindowsTicks(ticks)));
	}
	ReadEnum(root, "duplicateFrameDetectionMode", settings.duplicateFrameDetectionMode);
	ReadBool(root, "enableStatisticsForDynamicDetection", settings.isStatisticsForDynamicDetectionEnabled);
	ReadFloat(root, "minFrameRate", settings.minFrameRate);
	ReadBool(root, "disableFP16", settings.isFP16Disabled);

	// Before the profiles, which refer to scaling modes by index
	ReadScalingModes(root, settings);

	auto profilesNode = root.FindMember("profiles");
	if (profilesNode == root.MemberEnd()) {
		// v0.10.0-preview1 used scalingProfiles
		profilesNode = root.FindMember("scalingProfiles");
	}
	if (profilesNode != root.MemberEnd() && profilesNode->value.IsArray()) {
		auto profilesArray = profilesNode->value.GetArray();
		const size_t scalingModeCount = settings.scalingModes.size();

		const rapidjson::SizeType size = profilesArray.Size();
		if (size > 0) {
			// The first entry is the default profile
			if (profilesArray[0].IsObject()) {
				ReadProfile(profilesArray[0].GetObject(), settings.defaultProfile, scalingModeCount, true);
			}

			if (size > 1) {
				settings.profiles.reserve((size_t)size - 1);
				for (rapidjson::SizeType i = 1; i < size; ++i) {
					if (!profilesArray[i].IsObject()) {
						continue;
					}

					Profile& profile = settings.profiles.emplace_back();
					if (!ReadProfile(profilesArray[i].GetObject(), profile, scalingModeCount)) {
						settings.profiles.pop_back();
					}
				}
			}
		}
	}

	auto overlayNode = root.FindMember("overlay");
	if (overlayNode != root.MemberEnd() && overlayNode->value.IsObject()) {
		auto overlayObj = overlayNode->value.GetObject();

		// These options were global before v0.13
		{
			bool isOldConfig = false;
			Profile& defaultProfile = settings.defaultProfile;

			if (ReadEnum(overlayObj, "fullscreenInitialToolbarState", defaultProfile.fullscreenInitialToolbarState, true)) {
				isOldConfig = true;
				ReadEnum(overlayObj, "windowedInitialToolbarState", defaultProfile.windowedInitialToolbarState);
			} else if (ReadEnum(overlayObj, "initialToolbarState", defaultProfile.fullscreenInitialToolbarState, true)) {
				// v0.12.0-preview1 had one initial state for fullscreen and windowed scaling
				isOldConfig = true;
				defaultProfile.windowedInitialToolbarState = defaultProfile.fullscreenInitialToolbarState;
			}

			std::string screenshotsDir;
			if (ReadString(overlayObj, "screenshotsDir", screenshotsDir, true)) {
				isOldConfig = true;
				defaultProfile.screenshotsDir = std::move(screenshotsDir);
			}

			if (isOldConfig) {
				// Apply the former global settings to every profile
				for (Profile& profile : settings.profiles) {
					profile.fullscreenInitialToolbarState = defaultProfile.fullscreenInitialToolbarState;
					profile.windowedInitialToolbarState = defaultProfile.windowedInitialToolbarState;
					profile.screenshotsDir = defaultProfile.screenshotsDir;
				}
			}
		}

		auto windowsNode = overlayObj.FindMember("windows");
		if (windowsNode != overlayObj.MemberEnd() && windowsNode->value.IsObject()) {
			for (const auto& windowOptionPair : windowsNode->value.GetObject()) {
				if (!windowOptionPair.value.IsObject()) {
					continue;
				}

				auto windowOptionObj = windowOptionPair.value.GetObject();

				OverlayWindowOption& windowOption = settings.overlayWindowOptions[windowOptionPair.name.GetString()];
				ReadUInt16(windowOptionObj, "hArea", windowOption.hArea);
				ReadUInt16(windowOptionObj, "vArea", windowOption.vArea);
				ReadFloat(windowOptionObj, "hPos", windowOption.hPos);
				ReadFloat(windowOptionObj, "vPos", windowOption.vPos);
			}
		}
	}
}

bool ReadSettingsJson(std::string_view json, Settings& settings) noexcept {
	rapidjson::Document doc;
	doc.Parse(json.data(), json.size());
	if (doc.HasParseError()) {
		Logger::Get().Error(fmt::format("config.json is not valid JSON\n\terror code: {}\n\toffset: {}",
			(int)doc.GetParseError(), doc.GetErrorOffset()));
		return false;
	}

	if (!doc.IsObject()) {
		Logger::Get().Error("The root element of config.json is not an object");
		return false;
	}

	ReadSettings(static_cast<const rapidjson::Document&>(doc).GetObject(), settings);
	return true;
}

}
