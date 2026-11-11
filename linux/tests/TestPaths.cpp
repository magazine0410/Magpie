#include <catch2/catch_test_macros.hpp>
#include "Paths.h"
#include "TestHelpers.h"

using Magpie::Paths;

TEST_CASE("Directories follow the XDG variables", "[Paths]") {
	ScopedEnv portable("MAGPIE_CONFIG_DIR", nullptr);
	ScopedEnv config("XDG_CONFIG_HOME", "/xdg/config");
	ScopedEnv cache("XDG_CACHE_HOME", "/xdg/cache");
	ScopedEnv state("XDG_STATE_HOME", "/xdg/state");

	CHECK(Paths::ConfigDir() == "/xdg/config/magpie");
	CHECK(Paths::CacheDir() == "/xdg/cache/magpie");
	CHECK(Paths::StateDir() == "/xdg/state/magpie");
	CHECK(Paths::LogsDir() == "/xdg/state/magpie/logs");
}

TEST_CASE("Without XDG variables the defaults below HOME apply", "[Paths]") {
	ScopedEnv portable("MAGPIE_CONFIG_DIR", nullptr);
	ScopedEnv home("HOME", "/home/tester");
	ScopedEnv config("XDG_CONFIG_HOME", nullptr);
	ScopedEnv cache("XDG_CACHE_HOME", nullptr);
	ScopedEnv state("XDG_STATE_HOME", nullptr);

	CHECK(Paths::ConfigDir() == "/home/tester/.config/magpie");
	CHECK(Paths::CacheDir() == "/home/tester/.cache/magpie");
	CHECK(Paths::StateDir() == "/home/tester/.local/state/magpie");
	CHECK(Paths::LogsDir() == "/home/tester/.local/state/magpie/logs");
}

TEST_CASE("A relative or empty XDG value is ignored", "[Paths]") {
	ScopedEnv portable("MAGPIE_CONFIG_DIR", nullptr);
	ScopedEnv home("HOME", "/home/tester");

	{
		ScopedEnv config("XDG_CONFIG_HOME", "relative/dir");
		CHECK(Paths::ConfigDir() == "/home/tester/.config/magpie");
	}
	{
		ScopedEnv config("XDG_CONFIG_HOME", "");
		CHECK(Paths::ConfigDir() == "/home/tester/.config/magpie");
	}
}

TEST_CASE("A program in a build tree uses the files of the source tree", "[Paths]") {
	// The test program is not in <prefix>/bin with a <prefix>/share/magpie
	CHECK(Paths::EffectsDir("/source/Effects") == "/source/Effects");
	CHECK(Paths::IconFont("/build/_deps/font.ttf") == "/build/_deps/font.ttf");
	CHECK(Paths::IconFont({}).empty());
}

TEST_CASE("MAGPIE_CONFIG_DIR replaces the configuration directory", "[Paths]") {
	ScopedEnv config("XDG_CONFIG_HOME", "/xdg/config");
	ScopedEnv portable("MAGPIE_CONFIG_DIR", "/media/stick/magpie-config");

	CHECK(Paths::ConfigDir() == "/media/stick/magpie-config");
	// Only the configuration moves
	ScopedEnv cache("XDG_CACHE_HOME", "/xdg/cache");
	CHECK(Paths::CacheDir() == "/xdg/cache/magpie");
}
