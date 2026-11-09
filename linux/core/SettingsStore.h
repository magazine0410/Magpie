// Locates, loads and saves config.json.
#pragma once
#include <cstdint>
#include <filesystem>
#include "Settings.h"

namespace Magpie {

struct SettingsStore {
	// Raised when a new version can no longer read the old file. Part of the
	// path, as on Windows.
	static constexpr uint32_t CONFIG_VERSION = 5;

	enum class LoadResult {
		// The file was read.
		Loaded,
		// There is no file, or it is empty: `settings` holds the initial
		// settings. Nothing was written.
		Initial,
		// The file exists but cannot be read or is not valid. `settings` is
		// unchanged; saving now would destroy the user's file.
		Failed
	};

	// <config dir>/v5/config.json
	static std::filesystem::path ConfigPath() noexcept;

	// `settings` must be freshly constructed.
	static LoadResult Load(Settings& settings) noexcept;

	// Writes a new file and renames it over the old one, so that a failure
	// never leaves a damaged config.json.
	static bool Save(const Settings& settings) noexcept;
};

}
