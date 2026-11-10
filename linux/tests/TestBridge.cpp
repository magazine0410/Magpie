// The parts of the bridge that need no desktop.
#include <catch2/catch_test_macros.hpp>
#include <cstring>
#include "Bridge.h"

using namespace Magpie;

TEST_CASE("Windows are found by id, app ID, resource name or title", "[bridge]") {
	const std::vector<WindowInfo> windows{
		{ .id = "{1}", .title = "Tsukihime", .appId = "tsukihime.exe", .resourceName = "tsukihime.exe" },
		{ .id = "{2}", .title = "notes.txt - Editor", .appId = "org.example.Editor" },
		{ .id = "{3}", .title = "report.txt - Editor", .appId = "org.example.Editor" },
		{ .id = "{4}", .title = "{1} is the id of another window", .appId = "org.example.Viewer", .resourceName = "viewer" }
	};

	const auto ids = [&](std::string_view query) {
		std::string result;
		for (const WindowInfo* window : FindWindows(windows, query)) {
			result += window->id;
		}
		return result;
	};

	// The id wins over everything else
	CHECK(ids("{1}") == "{1}");
	// App ID and resource name, whatever the letter case
	CHECK(ids("TSUKIHIME.EXE") == "{1}");
	CHECK(ids("org.example.editor") == "{2}{3}");
	CHECK(ids("Viewer") == "{4}");
	// A part of the title, with its letter case
	CHECK(ids("report") == "{3}");
	CHECK(ids(".txt") == "{2}{3}");
	CHECK(ids("Report") == "");
	// An app ID is not searched as a part
	CHECK(ids("org.example") == "");
	CHECK(ids("") == "");
}

TEST_CASE("Every capability has a name", "[bridge]") {
	for (uint32_t i = 0; i < (uint32_t)Capability::COUNT; ++i) {
		const char* name = CapabilityReport::Name((Capability)i);
		REQUIRE(name);
		// "C1 ..." to "C9 ..."
		CHECK(name[0] == 'C');
		CHECK(name[1] == char('1' + i));
		CHECK(std::strlen(name) > 3);
	}
}
