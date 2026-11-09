#include <catch2/catch_test_macros.hpp>
#include "SettingsStore.h"
#include "TestHelpers.h"

using namespace Magpie;

TEST_CASE("The config file is below the versioned directory", "[SettingsStore]") {
	ScopedEnv portable("MAGPIE_CONFIG_DIR", nullptr);
	ScopedEnv config("XDG_CONFIG_HOME", "/xdg/config");

	CHECK(SettingsStore::ConfigPath() == "/xdg/config/magpie/v5/config.json");
}

TEST_CASE("Without a file the initial settings are returned and nothing is written", "[SettingsStore]") {
	TempDir dir;
	ScopedEnv portable("MAGPIE_CONFIG_DIR", nullptr);
	ScopedEnv config("XDG_CONFIG_HOME", dir.Path().c_str());

	Settings settings;
	CHECK(SettingsStore::Load(settings) == SettingsStore::LoadResult::Initial);

	Settings initial;
	initial.SetInitialSettings();
	CHECK(settings == initial);
	CHECK_FALSE(std::filesystem::exists(SettingsStore::ConfigPath()));
}

TEST_CASE("Saved settings are loaded again", "[SettingsStore]") {
	TempDir dir;
	ScopedEnv portable("MAGPIE_CONFIG_DIR", nullptr);
	ScopedEnv config("XDG_CONFIG_HOME", dir.Path().c_str());

	Settings saved;
	saved.SetInitialSettings();
	saved.language = "fi";
	saved.countdownSeconds = 1;
	saved.isKeepScreenOn = true;
	REQUIRE(SettingsStore::Save(saved));

	const std::filesystem::path configPath = SettingsStore::ConfigPath();
	CHECK(std::filesystem::exists(configPath));
	// The temporary file has been renamed away
	CHECK_FALSE(std::filesystem::exists(configPath.string() + ".new"));

	Settings loaded;
	REQUIRE(SettingsStore::Load(loaded) == SettingsStore::LoadResult::Loaded);
	CHECK(loaded == saved);
}

TEST_CASE("Saving replaces an existing file", "[SettingsStore]") {
	TempDir dir;
	ScopedEnv portable("MAGPIE_CONFIG_DIR", nullptr);
	ScopedEnv config("XDG_CONFIG_HOME", dir.Path().c_str());

	Settings first;
	first.countdownSeconds = 2;
	REQUIRE(SettingsStore::Save(first));

	Settings second;
	second.countdownSeconds = 4;
	REQUIRE(SettingsStore::Save(second));

	Settings loaded;
	REQUIRE(SettingsStore::Load(loaded) == SettingsStore::LoadResult::Loaded);
	CHECK(loaded.countdownSeconds == 4);
}

TEST_CASE("An empty file counts as no file", "[SettingsStore]") {
	TempDir dir;
	ScopedEnv portable("MAGPIE_CONFIG_DIR", nullptr);
	ScopedEnv config("XDG_CONFIG_HOME", dir.Path().c_str());

	const std::filesystem::path configPath = SettingsStore::ConfigPath();
	std::filesystem::create_directories(configPath.parent_path());
	std::ofstream(configPath).flush();

	Settings settings;
	CHECK(SettingsStore::Load(settings) == SettingsStore::LoadResult::Initial);
	CHECK(settings.scalingModes.size() == 7);
}

TEST_CASE("A damaged file fails to load and is left alone", "[SettingsStore]") {
	TempDir dir;
	ScopedEnv portable("MAGPIE_CONFIG_DIR", nullptr);
	ScopedEnv config("XDG_CONFIG_HOME", dir.Path().c_str());

	const std::filesystem::path configPath = SettingsStore::ConfigPath();
	std::filesystem::create_directories(configPath.parent_path());
	const std::string damaged = "{ \"theme\": 1, \"profiles\": [";
	std::ofstream(configPath) << damaged;

	Settings settings;
	CHECK(SettingsStore::Load(settings) == SettingsStore::LoadResult::Failed);
	CHECK(settings == Settings());
	CHECK(ReadFile(configPath) == damaged);
}

TEST_CASE("A Windows config placed in the directory is loaded", "[SettingsStore]") {
	TempDir dir;
	ScopedEnv portable("MAGPIE_CONFIG_DIR", dir.Path().c_str());

	const std::filesystem::path configPath = SettingsStore::ConfigPath();
	CHECK(configPath == dir.Path() / "v5" / "config.json");
	std::filesystem::create_directories(configPath.parent_path());
	std::filesystem::copy_file(
		std::filesystem::path(MAGPIE_TEST_DATA_DIR) / "windows-config-v5.json", configPath);

	Settings settings;
	REQUIRE(SettingsStore::Load(settings) == SettingsStore::LoadResult::Loaded);
	CHECK(settings.profiles.size() == 2);
	CHECK(settings.scalingModes.size() == 4);
}

TEST_CASE("Saving fails if the directory cannot be created", "[SettingsStore]") {
	TempDir dir;
	// A regular file where the configuration directory should be
	const std::filesystem::path blocker = dir.Path() / "blocker";
	std::ofstream(blocker) << "x";
	ScopedEnv portable("MAGPIE_CONFIG_DIR", blocker.c_str());

	CHECK_FALSE(SettingsStore::Save(Settings()));
}
