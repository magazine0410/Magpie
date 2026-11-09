#include "SettingsStore.h"
#include <cerrno>
#include <fcntl.h>
#include <fstream>
#include <sstream>
#include <unistd.h>
#include <fmt/format.h>
#include "Logger.h"
#include "Paths.h"
#include "SettingsJson.h"

namespace Magpie {

std::filesystem::path SettingsStore::ConfigPath() noexcept {
	return Paths::ConfigDir() / fmt::format("v{}", CONFIG_VERSION) / "config.json";
}

SettingsStore::LoadResult SettingsStore::Load(Settings& settings) noexcept {
	const std::filesystem::path configPath = ConfigPath();

	std::error_code ec;
	if (!std::filesystem::exists(configPath, ec)) {
		if (ec) {
			Logger::Get().ErrnoError("Cannot access config.json", ec.value());
			return LoadResult::Failed;
		}

		Logger::Get().Info("There is no config.json");
		settings.SetInitialSettings();
		return LoadResult::Initial;
	}

	std::string text;
	{
		std::ifstream file(configPath, std::ios::binary);
		if (!file) {
			Logger::Get().ErrnoError("Cannot open config.json");
			return LoadResult::Failed;
		}

		std::ostringstream content;
		content << file.rdbuf();
		if (file.bad()) {
			Logger::Get().ErrnoError("Cannot read config.json");
			return LoadResult::Failed;
		}
		text = std::move(content).str();
	}

	if (text.empty()) {
		Logger::Get().Info("config.json is empty");
		settings.SetInitialSettings();
		return LoadResult::Initial;
	}

	return ReadSettingsJson(text, settings) ? LoadResult::Loaded : LoadResult::Failed;
}

// Writes all of `text` and makes sure it has reached the disk.
static bool WriteFile(const std::filesystem::path& path, std::string_view text) noexcept {
	const int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
	if (fd < 0) {
		Logger::Get().ErrnoError("Cannot create the new config.json");
		return false;
	}

	while (!text.empty()) {
		const ssize_t written = write(fd, text.data(), text.size());
		if (written < 0) {
			if (errno == EINTR) {
				continue;
			}
			Logger::Get().ErrnoError("Cannot write the new config.json");
			close(fd);
			return false;
		}
		text.remove_prefix((size_t)written);
	}

	if (fsync(fd) != 0) {
		Logger::Get().ErrnoError("Cannot flush the new config.json");
		close(fd);
		return false;
	}

	if (close(fd) != 0) {
		Logger::Get().ErrnoError("Cannot close the new config.json");
		return false;
	}

	return true;
}

bool SettingsStore::Save(const Settings& settings) noexcept {
	const std::filesystem::path configPath = ConfigPath();

	std::error_code ec;
	std::filesystem::create_directories(configPath.parent_path(), ec);
	if (ec) {
		Logger::Get().ErrnoError("Cannot create the configuration directory", ec.value());
		return false;
	}

	std::filesystem::path newConfigPath = configPath;
	newConfigPath += ".new";

	if (!WriteFile(newConfigPath, WriteSettingsJson(settings))) {
		return false;
	}

	// rename replaces the old file in one step
	std::filesystem::rename(newConfigPath, configPath, ec);
	if (ec) {
		Logger::Get().ErrnoError("Cannot replace config.json", ec.value());
		return false;
	}

	return true;
}

}
