#include "ImageFile.h"
#include <cstring>
#include <fstream>
#include <fmt/format.h>
#include <png.h>
#include "Logger.h"
#include "StrHelper.h"
#include "VulkanDevice.h"

namespace Magpie {

bool ImageFile::LoadPng(const std::filesystem::path& fileName, ImageData& image) noexcept {
	png_image png{};
	png.version = PNG_IMAGE_VERSION;

	if (!png_image_begin_read_from_file(&png, fileName.c_str())) {
		Logger::Get().Error(fmt::format("Reading {} failed: {}", fileName.string(), png.message));
		return false;
	}

	png.format = PNG_FORMAT_RGBA;
	image.format = VK_FORMAT_R8G8B8A8_UNORM;
	image.width = png.width;
	image.height = png.height;
	image.pixels.resize(PNG_IMAGE_SIZE(png));

	if (!png_image_finish_read(&png, nullptr, image.pixels.data(), 0, nullptr)) {
		Logger::Get().Error(fmt::format("Reading {} failed: {}", fileName.string(), png.message));
		png_image_free(&png);
		return false;
	}

	return true;
}

bool ImageFile::SavePng(const std::filesystem::path& fileName, const ImageData& image, bool keepAlpha) noexcept {
	if (image.format != VK_FORMAT_R8G8B8A8_UNORM ||
		image.pixels.size() != (size_t)image.width * image.height * 4) {
		Logger::Get().Error("Only R8G8B8A8_UNORM images can be saved as PNG");
		return false;
	}

	png_image png{};
	png.version = PNG_IMAGE_VERSION;
	png.width = image.width;
	png.height = image.height;

	const uint8_t* pixels = image.pixels.data();
	std::vector<uint8_t> rgb;
	if (keepAlpha) {
		png.format = PNG_FORMAT_RGBA;
	} else {
		png.format = PNG_FORMAT_RGB;
		rgb.resize((size_t)image.width * image.height * 3);
		for (size_t i = 0, count = (size_t)image.width * image.height; i < count; ++i) {
			std::memcpy(&rgb[i * 3], &image.pixels[i * 4], 3);
		}
		pixels = rgb.data();
	}

	if (!png_image_write_to_file(&png, fileName.c_str(), 0, pixels, 0, nullptr)) {
		Logger::Get().Error(fmt::format("Writing {} failed: {}", fileName.string(), png.message));
		return false;
	}

	return true;
}

// The parts of the DDS file format that are needed here. See
// https://learn.microsoft.com/en-us/windows/win32/direct3ddds/dx-graphics-dds-pguide
namespace {

struct DdsPixelFormat {
	uint32_t size;
	uint32_t flags;
	uint32_t fourCC;
	uint32_t rgbBitCount;
	uint32_t rBitMask;
	uint32_t gBitMask;
	uint32_t bBitMask;
	uint32_t aBitMask;
};

struct DdsHeader {
	uint32_t size;
	uint32_t flags;
	uint32_t height;
	uint32_t width;
	uint32_t pitchOrLinearSize;
	uint32_t depth;
	uint32_t mipMapCount;
	uint32_t reserved1[11];
	DdsPixelFormat pixelFormat;
	uint32_t caps;
	uint32_t caps2;
	uint32_t caps3;
	uint32_t caps4;
	uint32_t reserved2;
};

struct DdsHeaderDx10 {
	uint32_t dxgiFormat;
	uint32_t resourceDimension;
	uint32_t miscFlag;
	uint32_t arraySize;
	uint32_t miscFlags2;
};

constexpr uint32_t DDS_MAGIC = 0x20534444;	// "DDS "
constexpr uint32_t DDPF_FOURCC = 0x4;
constexpr uint32_t DDPF_RGB = 0x40;
constexpr uint32_t DDPF_LUMINANCE = 0x20000;
constexpr uint32_t FOURCC_DX10 = 0x30315844;	// "DX10"
constexpr uint32_t D3DFMT_A16B16G16R16F = 113;
constexpr uint32_t D3DFMT_A32B32G32R32F = 116;
constexpr uint32_t DDS_DIMENSION_TEXTURE2D = 3;

VkFormat FormatOfDxgi(uint32_t dxgiFormat) noexcept {
	switch (dxgiFormat) {
	case 2:		// DXGI_FORMAT_R32G32B32A32_FLOAT
		return VK_FORMAT_R32G32B32A32_SFLOAT;
	case 10:	// DXGI_FORMAT_R16G16B16A16_FLOAT
		return VK_FORMAT_R16G16B16A16_SFLOAT;
	case 28:	// DXGI_FORMAT_R8G8B8A8_UNORM
		return VK_FORMAT_R8G8B8A8_UNORM;
	case 87:	// DXGI_FORMAT_B8G8R8A8_UNORM
		return VK_FORMAT_B8G8R8A8_UNORM;
	case 61:	// DXGI_FORMAT_R8_UNORM
		return VK_FORMAT_R8_UNORM;
	default:
		return VK_FORMAT_UNDEFINED;
	}
}

VkFormat FormatOfLegacy(const DdsPixelFormat& pf) noexcept {
	if (pf.flags & DDPF_FOURCC) {
		switch (pf.fourCC) {
		case D3DFMT_A16B16G16R16F:
			return VK_FORMAT_R16G16B16A16_SFLOAT;
		case D3DFMT_A32B32G32R32F:
			return VK_FORMAT_R32G32B32A32_SFLOAT;
		default:
			return VK_FORMAT_UNDEFINED;
		}
	}

	if ((pf.flags & DDPF_RGB) && pf.rgbBitCount == 32 && pf.gBitMask == 0x0000ff00 && pf.aBitMask == 0xff000000) {
		if (pf.rBitMask == 0x000000ff && pf.bBitMask == 0x00ff0000) {
			return VK_FORMAT_R8G8B8A8_UNORM;
		}
		if (pf.rBitMask == 0x00ff0000 && pf.bBitMask == 0x000000ff) {
			return VK_FORMAT_B8G8R8A8_UNORM;
		}
	}

	if ((pf.flags & DDPF_LUMINANCE) && pf.rgbBitCount == 8) {
		return VK_FORMAT_R8_UNORM;
	}

	return VK_FORMAT_UNDEFINED;
}

}

bool ImageFile::LoadDds(const std::filesystem::path& fileName, ImageData& image) noexcept {
	const auto fail = [&](const char* reason) {
		Logger::Get().Error(fmt::format("Reading {} failed: {}", fileName.string(), reason));
		return false;
	};

	std::ifstream file(fileName, std::ios::binary);
	if (!file) {
		return fail("the file cannot be opened");
	}

	uint32_t magic = 0;
	DdsHeader header{};
	file.read((char*)&magic, sizeof(magic));
	file.read((char*)&header, sizeof(header));
	if (!file || magic != DDS_MAGIC || header.size != sizeof(DdsHeader) ||
		header.pixelFormat.size != sizeof(DdsPixelFormat)) {
		return fail("not a DDS file");
	}

	if ((header.pixelFormat.flags & DDPF_FOURCC) && header.pixelFormat.fourCC == FOURCC_DX10) {
		DdsHeaderDx10 headerDx10{};
		file.read((char*)&headerDx10, sizeof(headerDx10));
		if (!file || headerDx10.resourceDimension != DDS_DIMENSION_TEXTURE2D || headerDx10.arraySize > 1) {
			return fail("not a single 2D texture");
		}
		image.format = FormatOfDxgi(headerDx10.dxgiFormat);
	} else {
		image.format = FormatOfLegacy(header.pixelFormat);
	}

	if (image.format == VK_FORMAT_UNDEFINED) {
		return fail("unsupported pixel format");
	}
	if (header.width == 0 || header.height == 0 || header.width > 16384 || header.height > 16384) {
		return fail("invalid size");
	}

	// Only the first mip level is used
	image.width = header.width;
	image.height = header.height;
	image.pixels.resize((size_t)header.width * header.height * BytesPerPixel(image.format));
	file.read((char*)image.pixels.data(), (std::streamsize)image.pixels.size());
	if (!file) {
		return fail("the file is too short");
	}

	return true;
}

bool ImageFile::Load(const std::filesystem::path& fileName, ImageData& image) noexcept {
	std::string extension = fileName.extension().string();
	StrHelper::ToLowerCase(extension);

	if (extension == ".dds") {
		return LoadDds(fileName, image);
	} else if (extension == ".png") {
		return LoadPng(fileName, image);
	}

	Logger::Get().Error(fmt::format("Reading {} failed: only DDS and PNG files are supported", fileName.string()));
	return false;
}

}
