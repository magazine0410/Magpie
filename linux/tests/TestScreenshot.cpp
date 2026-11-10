// Saving screenshots: pixel formats, file names that are taken, the file.
#include <catch2/catch_test_macros.hpp>
#include <cstring>
#include <fstream>
#include "Screenshot.h"
#include "TestHelpers.h"

using namespace Magpie;

namespace {

void Touch(const std::filesystem::path& path) {
	std::ofstream file(path);
}

}

TEST_CASE("A taken file name gets a number", "[screenshot]") {
	TempDir temp;
	const std::filesystem::path& dir = temp.Path();

	CHECK(Screenshot::AppendSuffix(dir, "game") == "game");
	// A directory that does not exist has no files
	CHECK(Screenshot::AppendSuffix(dir / "missing", "game") == "game");

	Touch(dir / "game.png");
	CHECK(Screenshot::AppendSuffix(dir, "game") == "game (2)");
	// Other names do not count
	Touch(dir / "game2.png");
	Touch(dir / "games (7).png");
	Touch(dir / "game (x).png");
	Touch(dir / "game (1).png");
	CHECK(Screenshot::AppendSuffix(dir, "game") == "game (2)");

	// One more than the highest number, whatever the extension
	Touch(dir / "game (2).png");
	Touch(dir / "game (9).dds");
	CHECK(Screenshot::AppendSuffix(dir, "game") == "game (10)");

	// Without the plain name the numbers still count
	std::filesystem::remove(dir / "game.png");
	CHECK(Screenshot::AppendSuffix(dir, "game") == "game (10)");
	CHECK(Screenshot::AppendSuffix(dir, "other") == "other");
}

TEST_CASE("Texture content is converted for the PNG file", "[screenshot]") {
	SECTION("blue first") {
		ImageData image{ VK_FORMAT_B8G8R8A8_UNORM, 2, 1, { 1, 2, 3, 4, 5, 6, 7, 8 } };
		REQUIRE(Screenshot::ConvertToRgba8(image));
		CHECK(image.format == VK_FORMAT_R8G8B8A8_UNORM);
		CHECK(image.pixels == std::vector<uint8_t>{ 3, 2, 1, 4, 7, 6, 5, 8 });
	}

	SECTION("16-bit floats") {
		// 0, 0.5, 1 and 2 (clamped), then -1 (clamped), a denormal, 0.25, 1
		const uint16_t halves[]{ 0x0000, 0x3800, 0x3C00, 0x4000, 0xBC00, 0x0001, 0x3400, 0x3C00 };
		ImageData image{ VK_FORMAT_R16G16B16A16_SFLOAT, 2, 1, {} };
		image.pixels.resize(sizeof(halves));
		std::memcpy(image.pixels.data(), halves, sizeof(halves));
		REQUIRE(Screenshot::ConvertToRgba8(image));
		CHECK(image.format == VK_FORMAT_R8G8B8A8_UNORM);
		CHECK(image.pixels == std::vector<uint8_t>{ 0, 128, 255, 255, 0, 0, 64, 255 });
	}

	SECTION("a size that does not fit the format") {
		ImageData image{ VK_FORMAT_R8G8B8A8_UNORM, 2, 2, { 1, 2, 3, 4 } };
		CHECK_FALSE(Screenshot::ConvertToRgba8(image));
	}

	SECTION("another format") {
		ImageData image{ VK_FORMAT_R8_UNORM, 1, 1, { 1 } };
		CHECK_FALSE(Screenshot::ConvertToRgba8(image));
	}
}

TEST_CASE("A screenshot is saved under the name of its template", "[screenshot]") {
	TempDir temp;
	const std::filesystem::path dir = temp.Path() / "new" / "dir";
	const ImageData image{ VK_FORMAT_R8G8B8A8_UNORM, 2, 2,
		{ 255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 10, 20, 30, 255 } };

	std::filesystem::path saved;
	REQUIRE(Screenshot::Save(image, dir, "%PN%-%WT:3%", "Title", "game", saved) == Screenshot::Result::Success);
	CHECK(saved == dir / "game-Tit.png");

	ImageData loaded;
	REQUIRE(ImageFile::LoadPng(saved, loaded));
	CHECK(loaded.width == 2);
	CHECK(loaded.height == 2);
	CHECK(loaded.pixels == image.pixels);

	// The second one does not replace the first
	REQUIRE(Screenshot::Save(image, dir, "%PN%-%WT:3%", "Title", "game", saved) == Screenshot::Result::Success);
	CHECK(saved == dir / "game-Tit (2).png");

	CHECK(Screenshot::Save(image, {}, "%PN%", "t", "p", saved) == Screenshot::Result::InvalidDirectory);
	CHECK(Screenshot::Save(image, dir, "a/b", "t", "p", saved) == Screenshot::Result::InvalidFilenameTemplate);
	CHECK(Screenshot::Save(image, dir, "", "t", "p", saved) == Screenshot::Result::InvalidFilenameTemplate);
}
