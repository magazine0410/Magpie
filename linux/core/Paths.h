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

	// $XDG_STATE_HOME/magpie, otherwise ~/.local/state/magpie. For what the
	// program remembers between runs and the user does not edit.
	static std::filesystem::path StateDir() noexcept;

	// The directory "logs" in StateDir.
	static std::filesystem::path LogsDir() noexcept;

	// The directory "Screenshots" in the user's pictures directory, which is
	// XDG_PICTURES_DIR of user-dirs.dirs or ~/Pictures. Counterpart of the
	// Screenshots folder of Windows.
	static std::filesystem::path SystemScreenshotsDir() noexcept;

	// Where screenshots go for a profile that names `configured`: the system
	// directory if it is empty, below the working directory if it is
	// relative, as Profile::GetScreenshotsDir does on Windows. A Windows path
	// from a configuration made there means nothing here and also gives the
	// system directory.
	static std::filesystem::path ScreenshotsDir(const std::filesystem::path& configured) noexcept;

	// The files that come with the program. Started from an installation
	// (<prefix>/bin), it uses those below <prefix>/share/magpie: the
	// directory "effects" and the file of the same name as `buildTreeFont`
	// in "fonts". Started from a build tree, where there is no such
	// directory, it uses the paths it is given, which are those of the
	// source and build tree.
	static std::filesystem::path EffectsDir(const std::filesystem::path& sourceTreeDir) noexcept;
	static std::filesystem::path IconFont(const std::filesystem::path& buildTreeFont) noexcept;
};

}
