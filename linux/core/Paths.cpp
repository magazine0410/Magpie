#include "Paths.h"
#include <cstdlib>
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

}
