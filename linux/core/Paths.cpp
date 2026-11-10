#include "Paths.h"
#include <cstdlib>
#include <fstream>
#include <string>
#include <string_view>
#include <pwd.h>
#include <unistd.h>

namespace Magpie {

static constexpr const char* APP_DIR = "magpie";

static std::filesystem::path HomeDir() noexcept {
	if (const char* home = std::getenv("HOME"); home && *home) {
		return home;
	}
	if (const passwd* pw = getpwuid(getuid()); pw && pw->pw_dir) {
		return pw->pw_dir;
	}
	return {};
}

// The specification says a value that is not an absolute path must be ignored.
static std::filesystem::path BaseDir(const char* variable, const char* fallbackInHome) noexcept {
	if (const char* value = std::getenv(variable); value && *value) {
		std::filesystem::path path(value);
		if (path.is_absolute()) {
			return path;
		}
	}
	return HomeDir() / fallbackInHome;
}

std::filesystem::path Paths::ConfigDir() noexcept {
	if (const char* value = std::getenv("MAGPIE_CONFIG_DIR"); value && *value) {
		std::error_code ec;
		std::filesystem::path path = std::filesystem::absolute(value, ec);
		return ec ? std::filesystem::path(value) : path;
	}
	return BaseDir("XDG_CONFIG_HOME", ".config") / APP_DIR;
}

std::filesystem::path Paths::CacheDir() noexcept {
	return BaseDir("XDG_CACHE_HOME", ".cache") / APP_DIR;
}

std::filesystem::path Paths::StateDir() noexcept {
	return BaseDir("XDG_STATE_HOME", ".local/state") / APP_DIR;
}

std::filesystem::path Paths::LogsDir() noexcept {
	return StateDir() / "logs";
}

std::filesystem::path Paths::SystemScreenshotsDir() noexcept {
	const std::filesystem::path home = HomeDir();
	std::filesystem::path pictures = home / "Pictures";

	// Lines such as XDG_PICTURES_DIR="$HOME/Bilder"; the value is either
	// absolute or starts with $HOME
	std::ifstream file(BaseDir("XDG_CONFIG_HOME", ".config") / "user-dirs.dirs");
	std::string line;
	while (std::getline(file, line)) {
		static constexpr std::string_view KEY = "XDG_PICTURES_DIR=\"";
		const size_t start = line.find_first_not_of(" \t");
		if (start == std::string::npos || line.compare(start, KEY.size(), KEY) != 0) {
			continue;
		}
		const size_t valueStart = start + KEY.size();
		const size_t valueEnd = line.find('"', valueStart);
		if (valueEnd == std::string::npos) {
			continue;
		}

		const std::string value = line.substr(valueStart, valueEnd - valueStart);
		if (value.starts_with("$HOME/")) {
			pictures = home / value.substr(6);
		} else if (value.starts_with('/')) {
			pictures = value;
		}
	}

	return pictures / "Screenshots";
}

std::filesystem::path Paths::ScreenshotsDir(const std::filesystem::path& configured) noexcept {
	if (configured.empty() || configured.native().find('\\') != std::string::npos) {
		return SystemScreenshotsDir();
	}
	if (configured.is_relative()) {
		std::error_code ec;
		std::filesystem::path result = std::filesystem::absolute(configured, ec);
		return ec ? configured : result.lexically_normal();
	}
	return configured;
}

}
