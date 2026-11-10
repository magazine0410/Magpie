// The overlay: where the toolbar is drawn, that its buttons react to the
// pointer, and the helpers that need no device.
#include <catch2/catch_test_macros.hpp>
#include <set>
#include "ImageFile.h"
#include "OverlayDrawer.h"
#include "RenderTestHelpers.h"

using namespace Magpie;
using namespace RenderTest;

namespace {

constexpr int32_t TARGET_WIDTH = 1000;
constexpr int32_t TARGET_HEIGHT = 700;
constexpr uint8_t GREY = 128;

EffectDesc MakeEffect(std::string name, size_t passCount) {
	EffectDesc desc;
	desc.name = std::move(name);
	desc.passes.resize(passCount);
	for (size_t i = 0; i < passCount; ++i) {
		desc.passes[i].desc = "Pass " + std::to_string(i + 1);
	}
	return desc;
}

// Draws what the overlay has laid out onto a grey target
std::vector<uint8_t> DrawOverlay(VulkanDevice& device, OverlayDrawer& overlay, Point offset) {
	const std::shared_ptr<Texture> target = device.CreateTexture(VK_FORMAT_R8G8B8A8_UNORM,
		(uint32_t)TARGET_WIDTH, (uint32_t)TARGET_HEIGHT, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
		VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
	REQUIRE(target);
	const std::vector<uint8_t> background((size_t)TARGET_WIDTH * TARGET_HEIGHT * 4, GREY);
	REQUIRE(device.UploadTexture(*target, background, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL));

	REQUIRE(device.Execute([&](VkCommandBuffer cmd) {
		overlay.Draw(cmd, target->image, target->view, { TARGET_WIDTH, TARGET_HEIGHT }, offset);
	}));

	std::vector<uint8_t> pixels;
	REQUIRE(device.ReadTexture(*target, pixels));

	// To look at what is drawn: set MAGPIE_TEST_DUMP_DIR
	if (const char* dir = std::getenv("MAGPIE_TEST_DUMP_DIR")) {
		static int count = 0;
		ImageFile::SavePng(std::filesystem::path(dir) / ("overlay" + std::to_string(++count) + ".png"),
			{ VK_FORMAT_R8G8B8A8_UNORM, (uint32_t)TARGET_WIDTH, (uint32_t)TARGET_HEIGHT, pixels }, false);
	}
	return pixels;
}

// The smallest rectangle around the pixels that are not grey
Rect ChangedRect(const std::vector<uint8_t>& pixels) {
	Rect rect{ TARGET_WIDTH, TARGET_HEIGHT, 0, 0 };
	for (int32_t y = 0; y < TARGET_HEIGHT; ++y) {
		for (int32_t x = 0; x < TARGET_WIDTH; ++x) {
			const uint8_t* pixel = &pixels[((size_t)y * TARGET_WIDTH + x) * 4];
			if (pixel[0] != GREY || pixel[1] != GREY || pixel[2] != GREY) {
				rect.left = std::min(rect.left, x);
				rect.top = std::min(rect.top, y);
				rect.right = std::max(rect.right, x + 1);
				rect.bottom = std::max(rect.bottom, y + 1);
			}
		}
	}
	return rect;
}

}

TEST_CASE("The toolbar fades with the distance of the pointer", "[overlay]") {
	CHECK(OverlayDrawer::CalcToolbarAlphaForDistance(0.0f) == 1.0f);
	CHECK(OverlayDrawer::CalcToolbarAlphaForDistance(10.0f) == 1.0f);
	CHECK(OverlayDrawer::CalcToolbarAlphaForDistance(30.0f) == 0.5f);
	CHECK(OverlayDrawer::CalcToolbarAlphaForDistance(50.0f) == 0.0f);
	CHECK(OverlayDrawer::CalcToolbarAlphaForDistance(500.0f) == 0.0f);
}

TEST_CASE("The colours of the profiler's timeline", "[overlay]") {
	const EffectDesc single = MakeEffect("Bicubic", 1);
	const EffectDesc two = MakeEffect("FSR/FSR_EASU", 2);
	const EffectDesc many = MakeEffect("Anime4K/Anime4K_Upscale_L", 12);

	SECTION("one effect with one pass needs none") {
		CHECK(OverlayDrawer::GenerateTimelineColors({ &single }).empty());
	}

	SECTION("one colour per effect, all different") {
		const EffectDesc other = MakeEffect("Lanczos", 1);
		const SmallVector<uint32_t> colors = OverlayDrawer::GenerateTimelineColors({ &single, &other });
		REQUIRE(colors.size() == 2);
		CHECK(colors[0] != colors[1]);
		CHECK(colors[0] < 7);
		CHECK(colors[1] < 7);
	}

	SECTION("effects with several passes get one per pass as well") {
		// Two for the effects, then two for the passes of the second
		const SmallVector<uint32_t> colors = OverlayDrawer::GenerateTimelineColors({ &single, &two });
		CHECK(colors.size() == 4);
		CHECK(std::set<uint32_t>(colors.begin(), colors.end()).size() == 4);
	}

	SECTION("with more passes than colours, neighbours still differ") {
		const SmallVector<uint32_t> colors = OverlayDrawer::GenerateTimelineColors({ &many });
		REQUIRE(colors.size() == 12);
		for (size_t i = 1; i < colors.size(); ++i) {
			CHECK(colors[i] != colors[i - 1]);
			CHECK(colors[i] < 7);
		}
	}

	SECTION("the same effects give the same colours") {
		CHECK(OverlayDrawer::GenerateTimelineColors({ &single, &two }) ==
			OverlayDrawer::GenerateTimelineColors({ &single, &two }));
	}
}

TEST_CASE("The toolbar is drawn at the top of the picture and its buttons work", "[overlay]") {
	const EffectDesc effect = MakeEffect("Bicubic", 1);

	ForEachDevice([&](VulkanDevice& device, const GraphicsCardInfo&) {
		// With and without the icon font
		for (const std::filesystem::path& iconFont : { std::filesystem::path(MAGPIE_ICON_FONT), std::filesystem::path() }) {
			INFO("icon font: " << iconFont);

			OverlayOptions overlayOptions;
			OverlayDrawer overlay;
			REQUIRE(overlay.Initialize(device, VK_FORMAT_R8G8B8A8_UNORM, { .iconFontPath = iconFont },
				overlayOptions, { &effect }));
			// The profiler has a default place
			CHECK(overlayOptions.windows.contains("profiler"));

			const Size displaySize{ 800, 600 };
			OverlayDrawer::Requests requests;

			// Off: nothing is drawn
			CHECK(overlay.GetToolbarState() == ToolbarState::Off);
			CHECK_FALSE(overlay.AnyVisibleWindow());
			overlay.Update({ .displaySize = displaySize }, requests);
			CHECK(ChangedRect(DrawOverlay(device, overlay, { 0, 0 })) == Rect{ TARGET_WIDTH, TARGET_HEIGHT, 0, 0 });

			// Always shown: 360 wide, in the middle of the top edge
			overlay.SetToolbarState(ToolbarState::AlwaysShow);
			overlay.Update({ .displaySize = displaySize, .fps = 60 }, requests);
			CHECK_FALSE(overlay.WantsMouse());
			Rect changed = ChangedRect(DrawOverlay(device, overlay, { 0, 0 }));
			CHECK(changed.left == 220);
			CHECK(changed.right == 580);
			CHECK(changed.top == 0);
			CHECK(changed.bottom == 31);

			// On a larger target with the picture elsewhere, it moves along
			changed = ChangedRect(DrawOverlay(device, overlay, { 100, 50 }));
			CHECK(changed == Rect{ 320, 50, 680, 81 });

			// Shown when the pointer is near: not with the pointer far away
			overlay.SetToolbarState(ToolbarState::AutoHide);
			overlay.Update({ .displaySize = displaySize, .mousePos = Point{ 400, 400 } }, requests);
			CHECK(ChangedRect(DrawOverlay(device, overlay, { 0, 0 })) == Rect{ TARGET_WIDTH, TARGET_HEIGHT, 0, 0 });
			CHECK(overlay.NeedRedraw(0, Point{ 400, 20 }));
			CHECK_FALSE(overlay.NeedRedraw(0, Point{ 400, 400 }));
			overlay.Update({ .displaySize = displaySize, .mousePos = Point{ 400, 20 } }, requests);
			CHECK(overlay.WantsMouse());
			changed = ChangedRect(DrawOverlay(device, overlay, { 0, 0 }));
			CHECK(changed.left == 220);
			CHECK(changed.right == 580);

			if (iconFont.empty()) {
				// With text on the buttons their places depend on the
				// system's font
				continue;
			}

			// A pointer that cannot click only looks: pressing does nothing
			const Point onStopButton{ 580 - 6 - 14, 14 };
			static constexpr std::pair<int, bool> PRESS[]{ { 0, true } };
			static constexpr std::pair<int, bool> RELEASE[]{ { 0, false } };
			static constexpr std::pair<int, bool> CLICK[]{ { 0, true }, { 0, false } };
			requests = {};
			overlay.Update({ .displaySize = displaySize, .mousePos = onStopButton, .buttonEvents = CLICK }, requests);
			CHECK_FALSE(requests.stopScaling);

			// The stop button at the right end
			overlay.Update({ .displaySize = displaySize, .mousePos = onStopButton, .canClick = true }, requests);
			CHECK(overlay.WantsMouse());
			overlay.Update({ .displaySize = displaySize, .mousePos = onStopButton, .canClick = true,
				.buttonEvents = PRESS }, requests);
			CHECK_FALSE(requests.stopScaling);
			overlay.Update({ .displaySize = displaySize, .mousePos = onStopButton, .canClick = true,
				.buttonEvents = RELEASE }, requests);
			CHECK(requests.stopScaling);
			CHECK_FALSE(requests.switchMode);
			CHECK_FALSE(requests.takeScreenshot);

			// The profiler button is the second from the left: its window
			// appears, and the timings are wanted
			const Point onProfilerButton{ 220 + 6 + 28 + 4 + 14, 14 };
			requests = {};
			CHECK_FALSE(overlay.IsProfilerVisible());
			overlay.Update({ .displaySize = displaySize, .mousePos = onProfilerButton, .canClick = true }, requests);
			// Press and release arrive together
			overlay.Update({ .displaySize = displaySize, .mousePos = onProfilerButton, .canClick = true,
				.buttonEvents = CLICK }, requests);
			CHECK(overlay.IsProfilerVisible());
			const float timings[]{ 1.25f };
			overlay.Update({ .displaySize = displaySize, .fps = 60, .effectTimings = timings }, requests);
			changed = ChangedRect(DrawOverlay(device, overlay, { 0, 0 }));
			// On the right, 60 from the edge, 310 wide
			CHECK(changed.right == 800 - 60);
			CHECK(changed.left == 800 - 60 - 310);
		}
	});
}
