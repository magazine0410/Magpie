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
		REQUIRE(ScalingSession::PlanVirtualOutput({ 4480, 264, 5120, 772 }, { 640, 480 }, THREE_IN_A_ROW, plan));
		CHECK(plan.monitor.name == "DP-6");
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
		REQUIRE(ScalingSession::PlanVirtualOutput({ 2000, 100, 2800, 700 }, { 800, 600 }, THREE_IN_A_ROW, plan));
		CHECK(plan.monitor.name == "DP-5");
		CHECK(plan.ringLeft == 1);
		CHECK(plan.ringRight == 1);
		CHECK(plan.padRight == 6);
		CHECK(plan.size == Size{ 808, 600 });
	}

	SECTION("a monitor with 60 Hz needs no rounding") {
		REQUIRE(ScalingSession::PlanVirtualOutput({ 100, 100, 743, 600 }, { 643, 500 }, THREE_IN_A_ROW, plan));
		CHECK(plan.monitor.name == "HDMI-A-2");
		CHECK(plan.ringLeft == 0);
		CHECK(plan.ringRight == 1);
		CHECK(plan.padRight == 0);
		CHECK(plan.size == Size{ 644, 500 });
	}

	SECTION("a single monitor has no strip at all") {
		const std::vector<OutputInfo> single{ { "eDP-1", { 0, 0, 1920, 1200 }, 60'000 } };
		REQUIRE(ScalingSession::PlanVirtualOutput({ 100, 100, 740, 580 }, { 640, 480 }, single, plan));
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
		REQUIRE(ScalingSession::PlanVirtualOutput({ 100, 1200, 740, 1680 }, { 640, 480 }, stacked, plan));
		CHECK(plan.monitor.name == "middle");
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
		REQUIRE(ScalingSession::PlanVirtualOutput({ 4480, 264, 5120, 772 }, { 640, 480 }, outputs, plan));
		CHECK(plan.otherMonitors.size() == 2);
		CHECK(plan.position == Point{ 0, 1080 + 16384 });
	}

	SECTION("a window on no monitor cannot be scaled") {
		CHECK_FALSE(ScalingSession::PlanVirtualOutput({ 9000, 100, 9640, 580 }, { 640, 480 }, THREE_IN_A_ROW, plan));
	}

	SECTION("the output stays within what X11 clients can address") {
		// Monitors that reach far down leave less room below
		const std::vector<OutputInfo> tall{ { "tall", { 0, 0, 3840, 14000 }, 60'000 } };
		REQUIRE(ScalingSession::PlanVirtualOutput({ 0, 0, 1920, 1080 }, { 1920, 1080 }, tall, plan));
		CHECK(plan.position.y + plan.size.height <= 32767);
		CHECK(plan.position.y - 14000 >= 8192);

		const std::vector<OutputInfo> tooTall{ { "tall", { 0, 0, 3840, 26000 }, 60'000 } };
		CHECK_FALSE(ScalingSession::PlanVirtualOutput({ 0, 0, 1920, 1080 }, { 1920, 1080 }, tooTall, plan));
	}
}
