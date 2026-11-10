// The file name template of screenshots, the program name it uses and the
// directory the files go to.
#include <catch2/catch_test_macros.hpp>
#include <unistd.h>
#include "Paths.h"
#include "ProcessInfo.h"
#include "ScreenshotFilenameTemplateHelper.h"
#include "TestHelpers.h"

using namespace Magpie;

namespace {

// 2026-03-07 09:05:02.045
std::tm TestTime() {
	std::tm time{};
	time.tm_year = 126;
	time.tm_mon = 2;
	time.tm_mday = 7;
	time.tm_hour = 9;
	time.tm_min = 5;
	time.tm_sec = 2;
	return time;
}

void WriteFile(const std::filesystem::path& path, std::string_view content) {
	std::filesystem::create_directories(path.parent_path());
	std::ofstream(path, std::ios::binary) << content;
}

std::string Apply(std::string_view templateStr, std::string_view title = "Title", std::string_view process = "game") {
	std::string result;
	REQUIRE(ScreenshotFilenameTemplateHelper::Apply(templateStr, title, process, TestTime(), 45, result));
	return result;
}

}

TEST_CASE("The screenshot file name template is filled in", "[screenshot]") {
	CHECK(Apply("%PN:20%") == "game");
	CHECK(Apply("%WT%_%PN%") == "Title_game");
	CHECK(Apply("%Y%-%m%-%D% %H%.%M%.%S%.%MS%") == "2026-03-07 09.05.02.045");
	CHECK(Apply("%y%") == "26");
	CHECK(Apply("shot") == "shot");

	SECTION("a per cent sign") {
		CHECK(Apply("100\\%") == "100%");
		// Not a token: taken as it is
		CHECK(Apply("%Q%") == "%Q%");
		CHECK(Apply("%") == "%");
	}

	SECTION("the number of characters is limited") {
		CHECK(Apply("%WT:3%", "Tsukihime") == "Tsu");
		CHECK(Apply("%WT:30%", "Tsukihime") == "Tsukihime");
		// Counted in characters, not bytes
		CHECK(Apply("%WT:2%", "月姫 -A piece of blue glass moon-") == "月姫");
		CHECK(Apply("%PN:1%", "x", "éa") == "é");
		// More than fits into the counter: the largest limit
		CHECK(Apply("%WT:300%", "abc") == "abc");
	}

	SECTION("title and program name are made fit for a file name") {
		CHECK(Apply("%WT%", "  a/b: c?  ") == "a#b# c#");
		CHECK(Apply("%WT%", "") == "[Empty]");
		CHECK(Apply("%PN%", "t", "") == "[Unknown]");
	}
}

TEST_CASE("A template with characters that file names cannot have is refused", "[screenshot]") {
	CHECK(ScreenshotFilenameTemplateHelper::IsValid("%PN:20% %Y%"));
	CHECK(ScreenshotFilenameTemplateHelper::IsValid("100\\%"));
	CHECK_FALSE(ScreenshotFilenameTemplateHelper::IsValid("a/b"));
	CHECK_FALSE(ScreenshotFilenameTemplateHelper::IsValid("what?"));
	CHECK_FALSE(ScreenshotFilenameTemplateHelper::IsValid("%H%:%M%"));
	// A limit of zero is no token, which leaves a colon
	CHECK_FALSE(ScreenshotFilenameTemplateHelper::IsValid("%PN:0%"));

	std::string result;
	CHECK_FALSE(ScreenshotFilenameTemplateHelper::Apply("a|b", "t", "p", TestTime(), 0, result));
}

TEST_CASE("The program name comes from the command line", "[screenshot]") {
	CHECK(ProcessInfo::NameFromCommand("/usr/bin/kate") == "kate");
	CHECK(ProcessInfo::NameFromCommand("kate") == "kate");
	// Wine runs a Windows program under its Windows path
	CHECK(ProcessInfo::NameFromCommand("C:\\Games\\Tsukihime\\Tsukihime.exe") == "Tsukihime");
	CHECK(ProcessInfo::NameFromCommand("Z:\\home\\user\\game\\GAME.EXE") == "GAME");
	CHECK(ProcessInfo::NameFromCommand(".exe") == ".exe");

	CHECK(ProcessInfo::GetName((uint32_t)getpid()) == "magpie_tests");
	CHECK(ProcessInfo::GetName(0).empty());
}

TEST_CASE("Screenshots go to the pictures directory unless a profile says otherwise", "[screenshot][Paths]") {
	TempDir temp;
	ScopedEnv home("HOME", temp.Path().c_str());
	ScopedEnv config("XDG_CONFIG_HOME", (temp.Path() / "config").c_str());

	SECTION("without user-dirs.dirs") {
		CHECK(Paths::SystemScreenshotsDir() == temp.Path() / "Pictures/Screenshots");
	}

	SECTION("with a pictures directory below HOME") {
		WriteFile(temp.Path() / "config/user-dirs.dirs",
			"# comment\nXDG_MUSIC_DIR=\"$HOME/Musik\"\nXDG_PICTURES_DIR=\"$HOME/Bilder\"\n");
		CHECK(Paths::SystemScreenshotsDir() == temp.Path() / "Bilder/Screenshots");
	}

	SECTION("with an absolute pictures directory") {
		WriteFile(temp.Path() / "config/user-dirs.dirs", "XDG_PICTURES_DIR=\"/data/pictures\"\n");
		CHECK(Paths::SystemScreenshotsDir() == "/data/pictures/Screenshots");
	}

	SECTION("the directory of a profile") {
		CHECK(Paths::ScreenshotsDir({}) == temp.Path() / "Pictures/Screenshots");
		CHECK(Paths::ScreenshotsDir("/data/shots") == "/data/shots");
		CHECK(Paths::ScreenshotsDir("shots") == std::filesystem::current_path() / "shots");
		// From a configuration made on Windows
		CHECK(Paths::ScreenshotsDir("D:\\Shots") == temp.Path() / "Pictures/Screenshots");
	}
}
