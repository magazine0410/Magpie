// Duplicate frame detection on real Vulkan devices.
#include <catch2/catch_test_macros.hpp>
#include "DuplicateFrameDetector.h"
#include "RenderTestHelpers.h"

using namespace Magpie;
using namespace RenderTest;

namespace {

// A frame of 37x21 pixels, cut from the test picture: odd in both directions
// and not a multiple of the block size of the shader.
ImageData Frame(int variant = 0) {
	const ImageData picture = LoadTestPicture();

	ImageData frame;
	frame.format = picture.format;
	frame.width = 37;
	frame.height = 21;
	frame.pixels.resize((size_t)frame.width * frame.height * 4);
	for (uint32_t y = 0; y < frame.height; ++y) {
		std::memcpy(&frame.pixels[(size_t)y * frame.width * 4], Pixel(picture, 20, y + 30), (size_t)frame.width * 4);
	}

	// Variants differ from each other in the first pixel
	frame.pixels[0] = uint8_t(frame.pixels[0] + variant);
	return frame;
}

std::shared_ptr<Texture> Upload(VulkanDevice& device, const ImageData& image) {
	std::shared_ptr<Texture> texture = device.CreateTexture(image.format, image.width, image.height,
		VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
	REQUIRE(texture);
	REQUIRE(device.UploadTexture(*texture, image.pixels, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL));
	return texture;
}

}

TEST_CASE("Always mode drops every frame that equals the previous one", "[render]") {
	ForEachDevice([&](VulkanDevice& device, const GraphicsCardInfo&) {
		const ImageData image = Frame();
		const std::shared_ptr<Texture> a = Upload(device, image);
		const std::shared_ptr<Texture> sameAsA = Upload(device, image);

		DuplicateFrameDetector detector;
		detector.Initialize(device, DuplicateFrameDetectionMode::Always, false);

		// The first frame is always new
		CHECK_FALSE(detector.OnNewFrame(*a));
		CHECK(detector.OnNewFrame(*a));
		CHECK(detector.OnNewFrame(*sameAsA));

		// One step in one channel of one pixel is a difference, wherever the
		// pixel is
		const std::pair<uint32_t, uint32_t> positions[]{ {0, 0}, {36, 0}, {0, 20}, {36, 20}, {17, 9}, {32, 16} };
		for (const auto& [x, y] : positions) {
			for (int channel = 0; channel < 3; ++channel) {
				CAPTURE(x, y, channel);
				ImageData changed = image;
				changed.pixels[((size_t)y * changed.width + x) * 4 + channel] ^= 1;
				const std::shared_ptr<Texture> b = Upload(device, changed);

				CHECK_FALSE(detector.OnNewFrame(*b));
				CHECK(detector.OnNewFrame(*b));
				CHECK_FALSE(detector.OnNewFrame(*a));
			}
		}

		// Alpha is not compared, as on Windows
		ImageData otherAlpha = image;
		otherAlpha.pixels[3] ^= 0x80;
		CHECK(detector.OnNewFrame(*Upload(device, otherAlpha)));
	});
}

TEST_CASE("Dynamic mode compares in rounds and skips frames in between", "[render]") {
	ForEachDevice([&](VulkanDevice& device, const GraphicsCardInfo&) {
		const std::shared_ptr<Texture> x = Upload(device, Frame(0));
		const std::shared_ptr<Texture> y = Upload(device, Frame(1));
		// Alternating between two frames gives a sequence without duplicates
		const auto distinct = [&](DuplicateFrameDetector& detector, uint32_t count, uint32_t& n) {
			for (uint32_t i = 0; i < count; ++i) {
				CHECK_FALSE(detector.OnNewFrame(++n % 2 ? *x : *y));
			}
		};
		const auto last = [&](uint32_t n) -> const Texture& {
			return n % 2 ? *x : *y;
		};

		SECTION("the rounds: 16 compared, 1 skipped, 10 compared, 2 skipped") {
			DuplicateFrameDetector detector;
			detector.Initialize(device, DuplicateFrameDetectionMode::Dynamic, true);

			uint32_t n = 0;
			// The first frame only starts the detection
			distinct(detector, 1 + 16, n);
			CHECK(detector.GetStatisticsForDynamicDetection() == std::pair(0u, 0u));
			distinct(detector, 1, n);
			CHECK(detector.GetStatisticsForDynamicDetection() == std::pair(0u, 1u));
			distinct(detector, 10, n);
			CHECK(detector.GetStatisticsForDynamicDetection() == std::pair(0u, 1u));
			distinct(detector, 2, n);
			CHECK(detector.GetStatisticsForDynamicDetection() == std::pair(0u, 3u));
		}

		SECTION("a duplicate in a skipped frame is missed, and counted with statistics") {
			for (const bool statistics : { false, true }) {
				DuplicateFrameDetector detector;
				detector.Initialize(device, DuplicateFrameDetectionMode::Dynamic, statistics);

				uint32_t n = 0;
				distinct(detector, 1 + 16, n);
				// The skipped frame repeats its predecessor
				CHECK_FALSE(detector.OnNewFrame(last(n)));
				CHECK(detector.GetStatisticsForDynamicDetection() == std::pair(statistics ? 1u : 0u, statistics ? 1u : 0u));
				// The next frame is compared again
				CHECK(detector.OnNewFrame(last(n)));
			}
		}

		SECTION("a duplicate starts a full round of 16 again") {
			DuplicateFrameDetector detector;
			detector.Initialize(device, DuplicateFrameDetectionMode::Dynamic, true);

			uint32_t n = 0;
			distinct(detector, 1 + 5, n);
			CHECK(detector.OnNewFrame(last(n)));
			distinct(detector, 16, n);
			CHECK(detector.GetStatisticsForDynamicDetection().second == 0);
			distinct(detector, 1, n);
			CHECK(detector.GetStatisticsForDynamicDetection().second == 1);
		}
	});
}
