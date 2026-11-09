// config.json in the format of the Windows application (configuration
// version 5), so that a file can move between the two systems.
#pragma once
#include <string>
#include <string_view>
#include "Settings.h"

namespace Magpie {

// Reads the text of a config.json into `settings`, which must be freshly
// constructed. Values that are missing or invalid keep their defaults, and
// names used by older versions are accepted, as on Windows. Returns false only
// if the text is not a JSON object.
bool ReadSettingsJson(std::string_view json, Settings& settings) noexcept;

std::string WriteSettingsJson(const Settings& settings) noexcept;

}
