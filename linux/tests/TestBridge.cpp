// The parts of the bridge that need no desktop.
#include <catch2/catch_test_macros.hpp>
#include <cstring>
#include "Bridge.h"
#include "KWinBridge.h"
#include "KWinPointerSpeed.h"

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

TEST_CASE("The pointer speed setting for a scaled picture", "[bridge]") {
	// With the flat profile the speed is 1 + setting
	CHECK(PointerDevice::Slowed(0.0, 2.0) == -0.5);
	CHECK(PointerDevice::Slowed(0.0, 2.25) == 1.0 / 2.25 - 1.0);
	CHECK(PointerDevice::Slowed(0.5, 3.0) == -0.5);
	CHECK(PointerDevice::Slowed(-0.5, 2.0) == -0.75);
	// A picture of the source's size changes nothing
	CHECK(PointerDevice::Slowed(0.25, 1.0) == 0.25);
	// A smaller picture makes the pointer faster, up to the limit of the setting
	CHECK(PointerDevice::Slowed(0.0, 0.8) == 0.25);
	CHECK(PointerDevice::Slowed(0.5, 0.5) == 1.0);
	CHECK(PointerDevice::Slowed(0.25, 0.0) == 0.25);
}

TEST_CASE("The mode of a virtual output is chosen from the modes it has", "[bridge]") {
	// As "kscreen-doctor -j" prints it, shortened
	const std::string json = R"({
		"outputs": [
			{ "name": "DP-6", "currentModeId": "2", "modes": [
				{ "id": "1", "name": "1920x1080@60", "refreshRate": 60, "size": { "height": 1080, "width": 1920 } },
				{ "id": "2", "name": "1920x1080@200", "refreshRate": 200.0, "size": { "height": 1080, "width": 1920 } }
			] },
			{ "name": "Virtual-Magpie", "currentModeId": "5", "modes": [
				{ "id": "3", "name": "592x403@60", "refreshRate": 60, "size": { "height": 403, "width": 592 } },
				{ "id": "4", "name": "592x403@199", "refreshRate": 198.99, "size": { "height": 403, "width": 592 } },
				{ "id": "5", "name": "648x480@200", "refreshRate": 199.61, "size": { "height": 480, "width": 648 } },
				{ "id": "6", "name": "648x480@240", "refreshRate": 239.58, "size": { "height": 480, "width": 648 } },
				{ "id": 7, "name": "broken" }
			] }
		],
		"screen": {}
	})";

	const std::vector<KWinOutputModes::Mode> modes = KWinOutputModes::Parse(json, "Virtual-Magpie");
	REQUIRE(modes.size() == 4);
	CHECK(modes[1].id == "4");
	CHECK(modes[1].size == Size{ 592, 403 });
	CHECK(modes[1].refreshRate == 198.99);

	CHECK(KWinOutputModes::Parse(json, "DP-5").empty());
	CHECK(KWinOutputModes::Parse("not JSON", "Virtual-Magpie").empty());
	CHECK(KWinOutputModes::Parse("[]", "Virtual-Magpie").empty());

	SECTION("the size has to match and the refresh rate to be the closest") {
		// The rate that was computed for 200 Hz at this size is 198.99 Hz; a
		// name with the rounded rate would not find it
		const KWinOutputModes::Mode* mode = KWinOutputModes::Choose(modes, { 592, 403 }, 200'000);
		REQUIRE(mode);
		CHECK(mode->id == "4");
		CHECK(KWinOutputModes::IsCloseEnough(*mode, 200'000));

		mode = KWinOutputModes::Choose(modes, { 648, 480 }, 239'960);
		REQUIRE(mode);
		CHECK(mode->id == "6");
		CHECK(KWinOutputModes::IsCloseEnough(*mode, 239'960));

		mode = KWinOutputModes::Choose(modes, { 592, 403 }, 60'000);
		REQUIRE(mode);
		CHECK(mode->id == "3");

		// The mode KWin remembers has another size and is never taken
		CHECK(KWinOutputModes::Choose(modes, { 800, 600 }, 200'000) == nullptr);
	}

	SECTION("a mode that is too slow means that one has to be made") {
		const std::vector<KWinOutputModes::Mode> onlyNative{ { "1", { 800, 600 }, 60.0 } };
		const KWinOutputModes::Mode* mode = KWinOutputModes::Choose(onlyNative, { 800, 600 }, 200'000);
		REQUIRE(mode);
		CHECK_FALSE(KWinOutputModes::IsCloseEnough(*mode, 200'000));
		CHECK(KWinOutputModes::IsCloseEnough(*mode, 60'000));
	}
}
