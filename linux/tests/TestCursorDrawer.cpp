// CursorDrawer against a reference computed on the CPU, and the geometry
// that places the cursor.
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include "CursorDrawer.h"
#include "RenderTestHelpers.h"
#include "ScalingGeometry.h"

using namespace Magpie;
using namespace RenderTest;

namespace {

constexpr int32_t TARGET_WIDTH = 96;
constexpr int32_t TARGET_HEIGHT = 64;

struct CursorPicture {
	// Red, green, blue, alpha; colours multiplied by alpha
	std::vector<uint8_t> pixels;
	Size size;
	Point hotSpot;
};

// Opaque, half transparent and transparent pixels in several colours
CursorPicture MakeCursor(int32_t width, int32_t height, Point hotSpot) {
	CursorPicture cursor{ .size = { width, height }, .hotSpot = hotSpot };
	cursor.pixels.resize((size_t)width * height * 4);
	for (int32_t y = 0; y < height; ++y) {
		for (int32_t x = 0; x < width; ++x) {
			static constexpr uint8_t ALPHAS[]{ 255, 128, 0, 200, 64 };
			const uint8_t alpha = ALPHAS[(x + y * 2) % std::size(ALPHAS)];
			const uint8_t colour[3]{ uint8_t(40 + x * 37), uint8_t(250 - y * 41), uint8_t(90 + (x ^ y) * 23) };
			uint8_t* pixel = &cursor.pixels[((size_t)y * width + x) * 4];
			for (int c = 0; c < 3; ++c) {
				pixel[c] = uint8_t(colour[c] * alpha / 255);
			}
			pixel[3] = alpha;
		}
	}
	return cursor;
}

std::vector<uint8_t> MakeBackground() {
	std::vector<uint8_t> pixels((size_t)TARGET_WIDTH * TARGET_HEIGHT * 4);
	for (int32_t y = 0; y < TARGET_HEIGHT; ++y) {
		for (int32_t x = 0; x < TARGET_WIDTH; ++x) {
			uint8_t* pixel = &pixels[((size_t)y * TARGET_WIDTH + x) * 4];
			pixel[0] = uint8_t(x * 2 + 20);
			pixel[1] = uint8_t(y * 3 + 10);
			pixel[2] = uint8_t(200 - x - y);
			pixel[3] = 255;
		}
	}
	return pixels;
}

// Draws the cursor on the background and returns the result
std::vector<uint8_t> DrawCursor(
	VulkanDevice& device,
	const CursorDrawer& drawer,
	Point cursorPos,
	Size srcSize,
	const Rect& destRect,
	const ScalingOptions& options
) {
	const std::shared_ptr<Texture> target = device.CreateTexture(VK_FORMAT_R8G8B8A8_UNORM,
		(uint32_t)TARGET_WIDTH, (uint32_t)TARGET_HEIGHT, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
		VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
	REQUIRE(target);
	REQUIRE(device.UploadTexture(*target, MakeBackground(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL));

	REQUIRE(device.Execute([&](VkCommandBuffer cmd) {
		drawer.Draw(cmd, { target->image, target->view, { TARGET_WIDTH, TARGET_HEIGHT }, target->layout },
			cursorPos, srcSize, destRect, options);
	}));

	std::vector<uint8_t> pixels;
	REQUIRE(device.ReadTexture(*target, pixels));
	return pixels;
}

// What CursorDrawer should produce: the cursor image enlarged to cursorRect,
// sampled like a texture with clamping at its edges, blended over the
// background inside destRect
std::vector<uint8_t> Reference(const CursorPicture& cursor, const Rect& cursorRect, const Rect& destRect, bool bilinear) {
	std::vector<uint8_t> pixels = MakeBackground();

	// Channel c of the texel, as the texture holds it: alpha inverted
	const auto texel = [&](int32_t x, int32_t y, int c) {
		x = std::clamp(x, 0, cursor.size.width - 1);
		y = std::clamp(y, 0, cursor.size.height - 1);
		const uint8_t value = cursor.pixels[((size_t)y * cursor.size.width + x) * 4 + c];
		return (c == 3 ? 255 - value : value) / 255.0;
	};

	for (int32_t y = std::max({ cursorRect.top, destRect.top, 0 });
		y < std::min({ cursorRect.bottom, destRect.bottom, TARGET_HEIGHT }); ++y) {
		for (int32_t x = std::max({ cursorRect.left, destRect.left, 0 });
			x < std::min({ cursorRect.right, destRect.right, TARGET_WIDTH }); ++x) {
			// Position in texels of the cursor image
			const double u = (x + 0.5 - cursorRect.left) / cursorRect.Width() * cursor.size.width;
			const double v = (y + 0.5 - cursorRect.top) / cursorRect.Height() * cursor.size.height;

			double sample[4];
			for (int c = 0; c < 4; ++c) {
				if (bilinear) {
					const int32_t x0 = (int32_t)std::floor(u - 0.5), y0 = (int32_t)std::floor(v - 0.5);
					const double fx = u - 0.5 - x0, fy = v - 0.5 - y0;
					sample[c] = (texel(x0, y0, c) * (1 - fx) + texel(x0 + 1, y0, c) * fx) * (1 - fy) +
						(texel(x0, y0 + 1, c) * (1 - fx) + texel(x0 + 1, y0 + 1, c) * fx) * fy;
				} else {
					sample[c] = texel((int32_t)std::floor(u), (int32_t)std::floor(v), c);
				}
			}

			uint8_t* pixel = &pixels[((size_t)y * TARGET_WIDTH + x) * 4];
			for (int c = 0; c < 3; ++c) {
				const double value = pixel[c] / 255.0 * sample[3] + sample[c];
				pixel[c] = (uint8_t)std::lround(std::clamp(value, 0.0, 1.0) * 255.0);
			}
		}
	}

	return pixels;
}

// Largest difference in red, green and blue, and whether everything outside
// `changedRect` is exactly equal
struct Difference {
	int maxDiff = 0;
	bool isOutsideEqual = true;
};

Difference Compare(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b, const Rect& changedRect) {
	Difference result;
	for (int32_t y = 0; y < TARGET_HEIGHT; ++y) {
		for (int32_t x = 0; x < TARGET_WIDTH; ++x) {
			const size_t i = ((size_t)y * TARGET_WIDTH + x) * 4;
			const bool inside = x >= changedRect.left && x < changedRect.right &&
				y >= changedRect.top && y < changedRect.bottom;
			for (int c = 0; c < 3; ++c) {
				const int diff = std::abs((int)a[i + c] - (int)b[i + c]);
				result.maxDiff = std::max(result.maxDiff, diff);
				if (!inside && diff != 0) {
					result.isOutsideEqual = false;
				}
			}
		}
	}
	return result;
}

const Rect WHOLE_TARGET{ 0, 0, TARGET_WIDTH, TARGET_HEIGHT };

}

TEST_CASE("The cursor rectangle follows the Windows rules", "[cursor]") {
	// Scale 1: the hot spot is at the cursor position
	CHECK(CursorDrawer::CalcCursorRect({ 100, 50 }, { 32, 32 }, { 4, 6 }, 1.0f) == Rect{ 96, 44, 128, 76 });
	// The hot spot offset and the size are scaled and rounded separately
	CHECK(CursorDrawer::CalcCursorRect({ 100, 50 }, { 32, 32 }, { 4, 6 }, 1.5f) == Rect{ 94, 41, 142, 89 });
	CHECK(CursorDrawer::CalcCursorRect({ 10, 10 }, { 7, 5 }, { 1, 1 }, 2.5f) == Rect{ 8, 8, 26, 21 });
	CHECK(CursorDrawer::CalcCursorRect({ 0, 0 }, { 16, 16 }, { 8, 8 }, 1.0f) == Rect{ -8, -8, 8, 8 });

	CHECK(CursorDrawer::CalcScaleFactor(1.25f, { 100, 50 }, { 0, 0, 300, 200 }) == 1.25f);
	// Zero: the mean of the horizontal and the vertical scaling of the source
	CHECK(CursorDrawer::CalcScaleFactor(0.0f, { 100, 50 }, { 10, 0, 310, 200 }) == 3.5f);
}

TEST_CASE("A point of the source is mapped to the scaled picture", "[cursor]") {
	const Rect srcRect{ 10, 20, 110, 70 };
	const Rect destRect{ 0, 0, 300, 200 };
	const Rect rendererRect{ -50, 0, 350, 200 };

	// First pixel to first pixel, last to last
	CHECK(ScalingGeometry::SrcToScaling({ 10, 20 }, srcRect, destRect, rendererRect, false) == Point{ 0, 0 });
	CHECK(ScalingGeometry::SrcToScaling({ 109, 69 }, srcRect, destRect, rendererRect, false) == Point{ 299, 199 });
	// In between: 50/99 of 299 and 25/49 of 199
	CHECK(ScalingGeometry::SrcToScaling({ 60, 45 }, srcRect, destRect, rendererRect, false) == Point{ 151, 102 });

	// Outside the source the distance is not scaled
	CHECK(ScalingGeometry::SrcToScaling({ 115, 75 }, srcRect, destRect, rendererRect, false) == Point{ 305, 205 });
	CHECK(ScalingGeometry::SrcToScaling({ 7, 18 }, srcRect, destRect, rendererRect, false) == Point{ -3, -2 });
	// And with skipBorder it is counted from the renderer's edge
	CHECK(ScalingGeometry::SrcToScaling({ 115, 45 }, srcRect, destRect, rendererRect, true) == Point{ 355, 102 });
	CHECK(ScalingGeometry::SrcToScaling({ 7, 45 }, srcRect, destRect, rendererRect, true) == Point{ -53, 102 });

	// A destination that does not start at the origin
	const Rect shifted{ 40, 30, 240, 130 };
	CHECK(ScalingGeometry::SrcToScaling({ 10, 20 }, srcRect, shifted, rendererRect, false) == Point{ 40, 30 });
	CHECK(ScalingGeometry::SrcToScaling({ 109, 69 }, srcRect, shifted, rendererRect, false) == Point{ 239, 129 });
}

TEST_CASE("The cursor is blended over the picture", "[cursor]") {
	ForEachDevice([](VulkanDevice& device, const GraphicsCardInfo&) {
		CursorDrawer drawer;
		REQUIRE(drawer.Initialize(device, VK_FORMAT_R8G8B8A8_UNORM));

		const CursorPicture cursor = MakeCursor(9, 7, { 2, 3 });
		REQUIRE(drawer.SetImage(cursor.pixels, cursor.size, cursor.hotSpot));

		ScalingOptions options;

		SECTION("at its own size") {
			options.cursorScaleFactor = 1.0f;
			const Rect cursorRect{ 28, 17, 37, 24 };
			const std::vector<uint8_t> result = DrawCursor(device, drawer, { 30, 20 }, { 48, 32 }, WHOLE_TARGET, options);
			const Difference difference = Compare(result, Reference(cursor, cursorRect, WHOLE_TARGET, false), cursorRect);
			CHECK(difference.maxDiff <= 1);
			CHECK(difference.isOutsideEqual);
			// It did draw something
			CHECK(Compare(result, MakeBackground(), {}).maxDiff > 50);
		}

		SECTION("enlarged with nearest-neighbour") {
			options.cursorScaleFactor = 3.0f;
			const Rect cursorRect{ 24, 11, 51, 32 };
			const std::vector<uint8_t> result = DrawCursor(device, drawer, { 30, 20 }, { 48, 32 }, WHOLE_TARGET, options);
			const Difference difference = Compare(result, Reference(cursor, cursorRect, WHOLE_TARGET, false), cursorRect);
			CHECK(difference.maxDiff <= 1);
			CHECK(difference.isOutsideEqual);
		}

		SECTION("enlarged with bilinear interpolation") {
			options.cursorScaleFactor = 3.0f;
			options.cursorInterpolationMode = CursorInterpolationMode::Bilinear;
			const Rect cursorRect{ 24, 11, 51, 32 };
			const std::vector<uint8_t> result = DrawCursor(device, drawer, { 30, 20 }, { 48, 32 }, WHOLE_TARGET, options);
			const Difference difference = Compare(result, Reference(cursor, cursorRect, WHOLE_TARGET, true), cursorRect);
			CHECK(difference.maxDiff <= 3);
			CHECK(difference.isOutsideEqual);
			// And it differs from nearest-neighbour
			CHECK(Compare(result, Reference(cursor, cursorRect, WHOLE_TARGET, false), cursorRect).maxDiff > 20);
		}

		SECTION("bilinear interpolation is not used at scale 1") {
			options.cursorScaleFactor = 1.0f;
			options.cursorInterpolationMode = CursorInterpolationMode::Bilinear;
			const Rect cursorRect{ 28, 17, 37, 24 };
			const std::vector<uint8_t> result = DrawCursor(device, drawer, { 30, 20 }, { 48, 32 }, WHOLE_TARGET, options);
			CHECK(Compare(result, Reference(cursor, cursorRect, WHOLE_TARGET, false), cursorRect).maxDiff <= 1);
		}

		SECTION("scaled like the source when the factor is zero") {
			options.cursorScaleFactor = 0.0f;
			// 48x32 to 96x64 is a factor of 2
			const Rect cursorRect{ 26, 14, 44, 28 };
			const std::vector<uint8_t> result = DrawCursor(device, drawer, { 30, 20 }, { 48, 32 }, WHOLE_TARGET, options);
			const Difference difference = Compare(result, Reference(cursor, cursorRect, WHOLE_TARGET, false), cursorRect);
			CHECK(difference.maxDiff <= 1);
			CHECK(difference.isOutsideEqual);
		}

		SECTION("cut off at the edges of the destination") {
			options.cursorScaleFactor = 2.0f;
			// The picture has black bars left and right; the cursor hangs over its left edge
			const Rect destRect{ 20, 0, 76, 64 };
			const Rect cursorRect{ 14, 24, 32, 38 };
			const std::vector<uint8_t> result = DrawCursor(device, drawer, { 18, 30 }, { 28, 32 }, destRect, options);
			const Rect visible{ 20, 24, 32, 38 };
			const Difference difference = Compare(result, Reference(cursor, cursorRect, destRect, false), visible);
			CHECK(difference.maxDiff <= 1);
			CHECK(difference.isOutsideEqual);
			CHECK(Compare(result, MakeBackground(), {}).maxDiff > 50);
		}

		SECTION("with a destination larger than the target") {
			options.cursorScaleFactor = 2.0f;
			const Rect destRect{ -40, -10, 140, 90 };
			// Over the right edge of the target
			const Rect cursorRect{ 86, 24, 104, 38 };
			const std::vector<uint8_t> result = DrawCursor(device, drawer, { 90, 30 }, { 90, 50 }, destRect, options);
			const Difference difference = Compare(result, Reference(cursor, cursorRect, destRect, false), cursorRect);
			CHECK(difference.maxDiff <= 1);
			CHECK(difference.isOutsideEqual);
		}

		SECTION("and not at all outside the destination") {
			options.cursorScaleFactor = 1.0f;
			const Rect destRect{ 20, 0, 76, 64 };
			const std::vector<uint8_t> result = DrawCursor(device, drawer, { 5, 30 }, { 28, 32 }, destRect, options);
			CHECK(result == MakeBackground());
		}
	});
}

TEST_CASE("Without a cursor image nothing is drawn", "[cursor]") {
	ForEachDevice([](VulkanDevice& device, const GraphicsCardInfo&) {
		CursorDrawer drawer;
		REQUIRE(drawer.Initialize(device, VK_FORMAT_R8G8B8A8_UNORM));

		const ScalingOptions options;
		CHECK(DrawCursor(device, drawer, { 30, 20 }, { 48, 32 }, WHOLE_TARGET, options) == MakeBackground());

		// An image, then an empty one, as a screen cast sends for a hidden cursor
		const CursorPicture cursor = MakeCursor(9, 7, { 2, 3 });
		REQUIRE(drawer.SetImage(cursor.pixels, cursor.size, cursor.hotSpot));
		CHECK(DrawCursor(device, drawer, { 30, 20 }, { 48, 32 }, WHOLE_TARGET, options) != MakeBackground());
		REQUIRE(drawer.SetImage({}, {}, {}));
		CHECK(DrawCursor(device, drawer, { 30, 20 }, { 48, 32 }, WHOLE_TARGET, options) == MakeBackground());

		// A size that does not match the data is refused
		CHECK_FALSE(drawer.SetImage(cursor.pixels, { 5, 5 }, {}));
	});
}

TEST_CASE("The cursor is drawn on a target with blue first", "[cursor]") {
	// The swap chain usually has this format
	ForEachDevice([](VulkanDevice& device, const GraphicsCardInfo&) {
		CursorDrawer drawer;
		REQUIRE(drawer.Initialize(device, VK_FORMAT_B8G8R8A8_UNORM));

		// One opaque pixel: red 200, green 100, blue 50
		const uint8_t pixel[4]{ 200, 100, 50, 255 };
		REQUIRE(drawer.SetImage(pixel, { 1, 1 }, { 0, 0 }));

		const std::shared_ptr<Texture> target = device.CreateTexture(VK_FORMAT_B8G8R8A8_UNORM, 16, 16,
			VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
			VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
		REQUIRE(target);
		REQUIRE(device.UploadTexture(*target, std::vector<uint8_t>(16 * 16 * 4, 0), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL));

		ScalingOptions options;
		options.cursorScaleFactor = 1.0f;
		REQUIRE(device.Execute([&](VkCommandBuffer cmd) {
			drawer.Draw(cmd, { target->image, target->view, { 16, 16 }, target->layout },
				{ 5, 6 }, { 16, 16 }, { 0, 0, 16, 16 }, options);
		}));

		std::vector<uint8_t> result;
		REQUIRE(device.ReadTexture(*target, result));
		// Stored as blue, green, red
		const uint8_t* drawn = &result[(6 * 16 + 5) * 4];
		CHECK(drawn[0] == 50);
		CHECK(drawn[1] == 100);
		CHECK(drawn[2] == 200);
	});
}
