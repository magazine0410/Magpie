// Where Magpie keeps its files on Linux, following the XDG Base Directory
// specification. On Windows these live under %LOCALAPPDATA%\Magpie or, in
// portable mode, next to the executable.
#pragma once
#include <filesystem>

namespace Magpie {

struct Paths {
	// $MAGPIE_CONFIG_DIR if set (portable mode), otherwise
	// $XDG_CONFIG_HOME/magpie, otherwise ~/.config/magpie.
	static std::filesystem::path ConfigDir() noexcept;

	// $XDG_CACHE_HOME/magpie, otherwise ~/.cache/magpie.
	static std::filesystem::path CacheDir() noexcept;

	// $XDG_STATE_HOME/magpie/logs, otherwise ~/.local/state/magpie/logs.
	static std::filesystem::path LogsDir() noexcept;
};

}
