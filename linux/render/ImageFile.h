// Image files: PNG for the tools, and the DDS variants that effects load with
// SOURCE. Linux counterpart of the loading part of
// src/Magpie.Core/TextureHelper.cpp and DDSHelper.cpp.
#pragma once
#include <cstdint>
#include <filesystem>
#include <vector>
#include <vulkan/vulkan_core.h>

namespace Magpie {

struct ImageData {
	VkFormat format = VK_FORMAT_UNDEFINED;
	uint32_t width = 0;
	uint32_t height = 0;
	// Tightly packed rows, top row first
	std::vector<uint8_t> pixels;
};

struct ImageFile {
	// Reads a PNG file as VK_FORMAT_R8G8B8A8_UNORM. The values are passed on
	// as stored; no colour space conversion takes place.
	static bool LoadPng(const std::filesystem::path& fileName, ImageData& image) noexcept;

	// `image` must be VK_FORMAT_R8G8B8A8_UNORM. With keepAlpha false the file
	// has no alpha channel.
	static bool SavePng(const std::filesystem::path& fileName, const ImageData& image, bool keepAlpha) noexcept;

	// Reads an uncompressed 2D texture without mip maps. Supported pixel
	// formats: R16G16B16A16_FLOAT, R32G32B32A32_FLOAT, R8G8B8A8_UNORM,
	// B8G8R8A8_UNORM and R8_UNORM, in the legacy or the DX10 header.
	static bool LoadDds(const std::filesystem::path& fileName, ImageData& image) noexcept;

	// By file extension: .dds or .png
	static bool Load(const std::filesystem::path& fileName, ImageData& image) noexcept;
};

}
