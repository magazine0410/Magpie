// The parts of a scaling session that need no desktop: where the virtual
// output goes and what it looks like.
#include <catch2/catch_test_macros.hpp>
#include "ScalingSession.h"

using namespace Magpie;

namespace {

// Three monitors of 1920x1080 side by side, as on the reference machine
const std::vector<OutputInfo> THREE_IN_A_ROW{
	{ "HDMI-A-2", { 0, 0, 1920, 1080 }, 60'000 },
	{ "DP-5", { 1920, 0, 3840, 1080 }, 239'960 },
	{ "DP-6", { 3840, 0, 5760, 1080 }, 200'000 }
};

}

TEST_CASE("The virtual output is planned from the window and the monitors", "[session]") {
	ScalingSession::VirtualOutputPlan plan;

	SECTION("a window on the monitor at the right end") {
		REQUIRE(ScalingSession::PlanVirtualOutput({ .frameRect = { 4480, 264, 5120, 772 }, .sourceSize = { 640, 480 } }, THREE_IN_A_ROW, plan));
		CHECK(plan.monitors[0].name == "DP-6");
		CHECK(plan.otherMonitors.size() == 2);
		// A neighbour on the left only
		CHECK(plan.ringLeft == 1);
		CHECK(plan.ringRight == 0);
		CHECK(plan.ringTop == 0);
		CHECK(plan.ringBottom == 0);
		// 641 is rounded up to a multiple of 8, because the monitor has 200 Hz
		CHECK(plan.padRight == 7);
		CHECK(plan.size == Size{ 648, 480 });
		// Far below the monitors, at their left edge
		CHECK(plan.position == Point{ 0, 1080 + 16384 });
	}

	SECTION("a window on the monitor in the middle") {
		REQUIRE(ScalingSession::PlanVirtualOutput({ .frameRect = { 2000, 100, 2800, 700 }, .sourceSize = { 800, 600 } }, THREE_IN_A_ROW, plan));
		CHECK(plan.monitors[0].name == "DP-5");
		CHECK(plan.ringLeft == 1);
		CHECK(plan.ringRight == 1);
		CHECK(plan.padRight == 6);
		CHECK(plan.size == Size{ 808, 600 });
	}

	SECTION("a monitor with 60 Hz needs no rounding") {
		REQUIRE(ScalingSession::PlanVirtualOutput({ .frameRect = { 100, 100, 743, 600 }, .sourceSize = { 643, 500 } }, THREE_IN_A_ROW, plan));
		CHECK(plan.monitors[0].name == "HDMI-A-2");
		CHECK(plan.ringLeft == 0);
		CHECK(plan.ringRight == 1);
		CHECK(plan.padRight == 0);
		CHECK(plan.size == Size{ 644, 500 });
	}

	SECTION("a single monitor has no strip at all") {
		const std::vector<OutputInfo> single{ { "eDP-1", { 0, 0, 1920, 1200 }, 60'000 } };
		REQUIRE(ScalingSession::PlanVirtualOutput({ .frameRect = { 100, 100, 740, 580 }, .sourceSize = { 640, 480 } }, single, plan));
		CHECK(plan.ringLeft + plan.ringRight + plan.ringTop + plan.ringBottom == 0);
		CHECK(plan.size == Size{ 640, 480 });
		CHECK(plan.position == Point{ 0, 1200 + 16384 });
	}

	SECTION("monitors above and below, and one that only touches a corner") {
		const std::vector<OutputInfo> stacked{
			{ "top", { 0, 0, 1920, 1080 }, 60'000 },
			{ "middle", { 0, 1080, 1920, 2160 }, 60'000 },
			{ "bottom", { 0, 2160, 1920, 3240 }, 60'000 },
			// Right of "top" only; it shares no edge with "middle"
			{ "corner", { 1920, 0, 3840, 1080 }, 60'000 }
		};
		REQUIRE(ScalingSession::PlanVirtualOutput({ .frameRect = { 100, 1200, 740, 1680 }, .sourceSize = { 640, 480 } }, stacked, plan));
		CHECK(plan.monitors[0].name == "middle");
		CHECK(plan.ringTop == 1);
		CHECK(plan.ringBottom == 1);
		CHECK(plan.ringLeft == 0);
		CHECK(plan.ringRight == 0);
		CHECK(plan.size == Size{ 640, 482 });
		CHECK(plan.position == Point{ 0, 3240 + 16384 });
	}

	SECTION("an existing virtual output is not a monitor") {
		std::vector<OutputInfo> outputs = THREE_IN_A_ROW;
		outputs.push_back({ "Virtual-Magpie", { 0, 17464, 648, 17944 }, 200'000 });
		REQUIRE(ScalingSession::PlanVirtualOutput({ .frameRect = { 4480, 264, 5120, 772 }, .sourceSize = { 640, 480 } }, outputs, plan));
		CHECK(plan.otherMonitors.size() == 2);
		CHECK(plan.position == Point{ 0, 1080 + 16384 });
	}

	SECTION("a window gets the place of its content on the output") {
		REQUIRE(ScalingSession::PlanVirtualOutput({ .frameRect = { 4480, 264, 5120, 772 }, .sourceSize = { 640, 480 } }, THREE_IN_A_ROW, plan));
		CHECK(plan.exitLeft);
		CHECK_FALSE(plan.exitRight);
		CHECK(plan.sourceRect == Rect{ 1, 0, 641, 480 });
		CHECK(plan.pointerRect == plan.sourceRect);
	}

	SECTION("a fullscreen window fills an output of its own size") {
		REQUIRE(ScalingSession::PlanVirtualOutput({ .frameRect = { 3840, 0, 5760, 1080 }, .sourceSize = { 1920, 1080 }, .fillsOutput = true }, THREE_IN_A_ROW, plan));
		CHECK(plan.monitors[0].name == "DP-6");
		// No strip around it and nothing to cut off
		CHECK(plan.ringLeft == 0);
		CHECK(plan.padRight == 0);
		CHECK(plan.size == Size{ 1920, 1080 });
		CHECK(plan.sourceRect == Rect{ 0, 0, 1920, 1080 });
		// Its first column is where the pointer leaves to the left
		CHECK(plan.exitLeft);
		CHECK(plan.pointerRect == Rect{ 1, 0, 1920, 1080 });
	}

	SECTION("a maximized window becomes wider if the refresh rate asks for it") {
		// 1366 is no multiple of 8
		const std::vector<OutputInfo> single{ { "eDP-1", { 0, 0, 1366, 768 }, 144'000 } };
		REQUIRE(ScalingSession::PlanVirtualOutput({ .frameRect = { 0, 0, 1366, 724 }, .sourceSize = { 1366, 696 }, .fillsOutput = true }, single, plan));
		CHECK(plan.size == Size{ 1368, 696 });
		CHECK(plan.sourceRect == Rect{ 0, 0, 1368, 696 });
		CHECK(plan.pointerRect == plan.sourceRect);

		const std::vector<OutputInfo> slow{ { "eDP-1", { 0, 0, 1366, 768 }, 60'000 } };
		REQUIRE(ScalingSession::PlanVirtualOutput({ .frameRect = { 0, 0, 1366, 724 }, .sourceSize = { 1366, 696 }, .fillsOutput = true }, slow, plan));
		CHECK(plan.size == Size{ 1366, 696 });
	}

	SECTION("the picture covers the monitors that the window touches") {
		// On the middle monitor, reaching onto the right one
		const ScalingSession::PlanRequest request{
			.frameRect = { 3000, 100, 4000, 800 }, .sourceSize = { 1000, 700 },
			.multiMonitorUsage = MultiMonitorUsage::Intersected
		};
		REQUIRE(ScalingSession::PlanVirtualOutput(request, THREE_IN_A_ROW, plan));
		REQUIRE(plan.monitors.size() == 2);
		CHECK(plan.monitors[0].name == "DP-5");
		CHECK(plan.monitors[1].name == "DP-6");
		CHECK(plan.otherMonitors.size() == 1);
		CHECK(plan.rendererRect == Rect{ 1920, 0, 5760, 1080 });
		// The faster of the two
		CHECK(plan.refreshRate == 239'960);
		// The remaining monitor is on the left
		CHECK(plan.exitLeft);
		CHECK_FALSE(plan.exitRight);

		SECTION("but a maximized window is on one monitor") {
			ScalingSession::PlanRequest maximized = request;
			maximized.fillsOutput = true;
			REQUIRE(ScalingSession::PlanVirtualOutput(maximized, THREE_IN_A_ROW, plan));
			CHECK(plan.monitors.size() == 1);
		}
	}

	SECTION("the picture covers all monitors") {
		REQUIRE(ScalingSession::PlanVirtualOutput({
			.frameRect = { 100, 100, 740, 580 }, .sourceSize = { 640, 480 },
			.multiMonitorUsage = MultiMonitorUsage::All
		}, THREE_IN_A_ROW, plan));
		REQUIRE(plan.monitors.size() == 3);
		// The one with the window comes first
		CHECK(plan.monitors[0].name == "HDMI-A-2");
		CHECK(plan.otherMonitors.empty());
		CHECK(plan.rendererRect == Rect{ 0, 0, 5760, 1080 });
		// Nowhere to leave to
		CHECK_FALSE(plan.exitLeft);
		CHECK_FALSE(plan.exitRight);
		CHECK(plan.sourceRect == Rect{ 0, 0, 640, 480 });
	}

	SECTION("a window can be left on every side") {
		REQUIRE(ScalingSession::PlanVirtualOutput({
			.frameRect = { 4480, 264, 5120, 772 }, .sourceSize = { 640, 480 }, .isWindowedMode = true,
			// Has no meaning for a window
			.multiMonitorUsage = MultiMonitorUsage::All
		}, THREE_IN_A_ROW, plan));
		REQUIRE(plan.monitors.size() == 1);
		CHECK(plan.monitors[0].name == "DP-6");
		CHECK(plan.otherMonitors.size() == 2);
		CHECK(plan.exitLeft);
		CHECK(plan.exitTop);
		CHECK(plan.exitRight);
		CHECK(plan.exitBottom);
		// 642 rounded up to a multiple of 8
		CHECK(plan.size == Size{ 648, 482 });
		CHECK(plan.sourceRect == Rect{ 1, 1, 641, 481 });
	}

	SECTION("in 3D game mode the pointer cannot leave") {
		REQUIRE(ScalingSession::PlanVirtualOutput({
			.frameRect = { 4480, 264, 5120, 772 }, .sourceSize = { 640, 480 }, .is3DGameMode = true
		}, THREE_IN_A_ROW, plan));
		CHECK_FALSE(plan.exitLeft);
		CHECK(plan.ringLeft == 0);
		CHECK(plan.size == Size{ 640, 480 });
		CHECK(plan.pointerRect == Rect{ 0, 0, 640, 480 });
	}

	SECTION("a window on no monitor cannot be scaled") {
		CHECK_FALSE(ScalingSession::PlanVirtualOutput({ .frameRect = { 9000, 100, 9640, 580 }, .sourceSize = { 640, 480 } }, THREE_IN_A_ROW, plan));
	}

	SECTION("the output stays within what X11 clients can address") {
		// Monitors that reach far down leave less room below
		const std::vector<OutputInfo> tall{ { "tall", { 0, 0, 3840, 14000 }, 60'000 } };
		REQUIRE(ScalingSession::PlanVirtualOutput({ .frameRect = { 0, 0, 1920, 1080 }, .sourceSize = { 1920, 1080 } }, tall, plan));
		CHECK(plan.position.y + plan.size.height <= 32767);
		CHECK(plan.position.y - 14000 >= 8192);

		const std::vector<OutputInfo> tooTall{ { "tall", { 0, 0, 3840, 26000 }, 60'000 } };
		CHECK_FALSE(ScalingSession::PlanVirtualOutput({ .frameRect = { 0, 0, 1920, 1080 }, .sourceSize = { 1920, 1080 } }, tooTall, plan));
	}
}
