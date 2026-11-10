// Parts of the renderer that need no graphics device.
#include <fstream>
#include <catch2/catch_test_macros.hpp>
#include "ImageFile.h"
#include "ScalingGeometry.h"
#include "TestHelpers.h"
#include "VulkanDevice.h"

using namespace Magpie;

static GraphicsCardInfo Card(uint32_t vendorId, uint32_t deviceId, bool isUsable = true, bool isSoftware = false) {
	return { vendorId, deviceId, "", isSoftware, isUsable };
}

TEST_CASE("SelectGraphicsCard follows the rules of the Windows version", "[render]") {
	// A dedicated card, an integrated one and a software implementation
	const std::vector<GraphicsCardInfo> cards{
		Card(0x10de, 0x2705), Card(0x1002, 0x164e), Card(0x10005, 0, true, true)
	};

	SECTION("the default is the first card that is not software") {
		CHECK(SelectGraphicsCard(cards, {}) == 0);
		// IDs without an index mean "absent for now" and do not select anything
		CHECK(SelectGraphicsCard(cards, { -1, 0x1002, 0x164e }) == 0);
	}

	SECTION("index and IDs select a card") {
		CHECK(SelectGraphicsCard(cards, { 1, 0x1002, 0x164e }) == 1);
		CHECK(SelectGraphicsCard(cards, { 2, 0x10005, 0 }) == 2);
	}

	SECTION("a wrong index is corrected by the IDs") {
		CHECK(SelectGraphicsCard(cards, { 0, 0x1002, 0x164e }) == 1);
		CHECK(SelectGraphicsCard(cards, { 7, 0x1002, 0x164e }) == 1);
	}

	SECTION("a card that no longer exists gives the default") {
		CHECK(SelectGraphicsCard(cards, { 1, 0x8086, 0x1234 }) == 0);
	}

	SECTION("two cards of the same model are told apart by the index") {
		const std::vector<GraphicsCardInfo> twins{ Card(0x1002, 0x7480), Card(0x1002, 0x7480) };
		CHECK(SelectGraphicsCard(twins, { 1, 0x1002, 0x7480 }) == 1);
		CHECK(SelectGraphicsCard(twins, { 0, 0x1002, 0x7480 }) == 0);
	}

	SECTION("a chosen card that is not usable is replaced") {
		const std::vector<GraphicsCardInfo> old{ Card(0x8086, 0x0166, false), Card(0x10de, 0x2705) };
		CHECK(SelectGraphicsCard(old, { 0, 0x8086, 0x0166 }) == 1);
	}

	SECTION("software rendering is the last resort") {
		const std::vector<GraphicsCardInfo> none{ Card(0x8086, 0x0166, false), Card(0x10005, 0, true, true) };
		CHECK(SelectGraphicsCard(none, {}) == 1);

		const std::vector<GraphicsCardInfo> softwareFirst{ Card(0x10005, 0, true, true), Card(0x10de, 0x2705) };
		CHECK(SelectGraphicsCard(softwareFirst, {}) == 1);
	}

	SECTION("nothing usable") {
		const std::vector<GraphicsCardInfo> none{ Card(0x8086, 0x0166, false) };
		CHECK(SelectGraphicsCard(none, {}) == -1);
		CHECK(SelectGraphicsCard({}, {}) == -1);
	}
}

TEST_CASE("CalcDestRect places the output inside the renderer area", "[render]") {
	const Size renderer{ 300, 200 };
	const Size output{ 100, 50 };
	using enum OutputAlignment;

	CHECK(ScalingGeometry::CalcDestRect(renderer, output, LeftTop) == Rect{ 0, 0, 100, 50 });
	CHECK(ScalingGeometry::CalcDestRect(renderer, output, Top) == Rect{ 100, 0, 200, 50 });
	CHECK(ScalingGeometry::CalcDestRect(renderer, output, RightTop) == Rect{ 200, 0, 300, 50 });
	CHECK(ScalingGeometry::CalcDestRect(renderer, output, Left) == Rect{ 0, 75, 100, 125 });
	CHECK(ScalingGeometry::CalcDestRect(renderer, output, Center) == Rect{ 100, 75, 200, 125 });
	CHECK(ScalingGeometry::CalcDestRect(renderer, output, Right) == Rect{ 200, 75, 300, 125 });
	CHECK(ScalingGeometry::CalcDestRect(renderer, output, LeftBottom) == Rect{ 0, 150, 100, 200 });
	CHECK(ScalingGeometry::CalcDestRect(renderer, output, Bottom) == Rect{ 100, 150, 200, 200 });
	CHECK(ScalingGeometry::CalcDestRect(renderer, output, RightBottom) == Rect{ 200, 150, 300, 200 });

	// An output that fills the area is not moved
	CHECK(ScalingGeometry::CalcDestRect(renderer, renderer, RightBottom) == Rect{ 0, 0, 300, 200 });
	// Odd remainders are rounded down
	CHECK(ScalingGeometry::CalcDestRect({ 301, 201 }, output, Center) == Rect{ 100, 75, 200, 125 });
}

TEST_CASE("CropSourceRect removes the edges and keeps a minimum size", "[render]") {
	Rect rect{ 10, 20, 410, 320 };
	REQUIRE(ScalingGeometry::CropSourceRect(rect, { 5.0f, 10.4f, 20.0f, 30.6f }));
	CHECK(rect == Rect{ 15, 30, 390, 289 });

	// 64 pixels have to remain
	Rect narrow{ 0, 0, 100, 100 };
	CHECK_FALSE(ScalingGeometry::CropSourceRect(narrow, { 20.0f, 0.0f, 20.0f, 0.0f }));
	CHECK(narrow == Rect{ 0, 0, 100, 100 });
	CHECK(ScalingGeometry::CropSourceRect(narrow, { 18.0f, 0.0f, 18.0f, 0.0f }));
	CHECK(narrow.Width() == 64);
}

TEST_CASE("PNG files are written and read back unchanged", "[render]") {
	TempDir dir;

	ImageData image;
	image.format = VK_FORMAT_R8G8B8A8_UNORM;
	image.width = 5;
	image.height = 3;
	for (uint32_t i = 0; i < image.width * image.height * 4; ++i) {
		image.pixels.push_back(uint8_t(i * 37));
	}

	SECTION("with alpha") {
		REQUIRE(ImageFile::SavePng(dir.Path() / "a.png", image, true));

		ImageData loaded;
		REQUIRE(ImageFile::Load(dir.Path() / "a.png", loaded));
		CHECK(loaded.width == 5);
		CHECK(loaded.height == 3);
		CHECK(loaded.format == VK_FORMAT_R8G8B8A8_UNORM);
		CHECK(loaded.pixels == image.pixels);
	}

	SECTION("without alpha the file is opaque") {
		REQUIRE(ImageFile::SavePng(dir.Path() / "b.png", image, false));

		ImageData loaded;
		REQUIRE(ImageFile::LoadPng(dir.Path() / "b.png", loaded));
		REQUIRE(loaded.pixels.size() == image.pixels.size());
		for (size_t i = 0; i < image.pixels.size(); ++i) {
			CHECK(loaded.pixels[i] == (i % 4 == 3 ? 255 : image.pixels[i]));
		}
	}

	SECTION("failures") {
		ImageData loaded;
		CHECK_FALSE(ImageFile::LoadPng(dir.Path() / "missing.png", loaded));
		CHECK_FALSE(ImageFile::Load(dir.Path() / "picture.jpg", loaded));

		image.format = VK_FORMAT_R16G16B16A16_SFLOAT;
		CHECK_FALSE(ImageFile::SavePng(dir.Path() / "c.png", image, true));
	}
}

TEST_CASE("The DDS files of the bundled effects are read", "[render]") {
	const std::filesystem::path effectsDir = MAGPIE_EFFECTS_DIR;
	ImageData image;

	// 32 bits per pixel with channel masks
	REQUIRE(ImageFile::Load(effectsDir / "SMAA" / "AreaTex.dds", image));
	CHECK(image.format == VK_FORMAT_R8G8B8A8_UNORM);
	CHECK(image.width == 160);
	CHECK(image.height == 560);
	CHECK(image.pixels.size() == 160 * 560 * 4);

	// 8 bits of luminance
	REQUIRE(ImageFile::Load(effectsDir / "SMAA" / "SearchTex.dds", image));
	CHECK(image.format == VK_FORMAT_R8_UNORM);
	CHECK(image.width == 64);
	CHECK(image.height == 16);
	CHECK(image.pixels.size() == 64 * 16);

	// 16-bit floats, named by a D3DFMT number
	REQUIRE(ImageFile::Load(effectsDir / "NIS" / "Coef_Scale.dds", image));
	CHECK(image.format == VK_FORMAT_R16G16B16A16_SFLOAT);
	CHECK(image.width == 2);
	CHECK(image.height == 64);
	CHECK(image.pixels.size() == 2 * 64 * 8);

	SECTION("every file that an effect names can be read") {
		uint32_t count = 0;
		for (const auto& entry : std::filesystem::recursive_directory_iterator(effectsDir)) {
			if (entry.path().extension() == ".dds") {
				CAPTURE(entry.path());
				CHECK(ImageFile::Load(entry.path(), image));
				++count;
			}
		}
		CHECK(count == 17);
	}

	SECTION("a truncated file is refused") {
		TempDir dir;
		const std::string content = ReadFile(effectsDir / "SMAA" / "SearchTex.dds");
		std::ofstream(dir.Path() / "short.dds", std::ios::binary).write(content.data(), 500);
		CHECK_FALSE(ImageFile::Load(dir.Path() / "short.dds", image));

		std::ofstream(dir.Path() / "other.dds", std::ios::binary) << "not a texture";
		CHECK_FALSE(ImageFile::Load(dir.Path() / "other.dds", image));
	}
}
