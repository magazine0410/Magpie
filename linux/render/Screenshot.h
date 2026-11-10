// Saves what an effect has drawn as an image file. Linux counterpart of
// Renderer::_TakeScreenshotImpl and AppendSuffixToScreenshotFilename in
// src/Magpie.Core/Renderer.cpp, from the point where the pixels are in
// memory.
#pragma once
#include <filesystem>
#include <string>
#include <string_view>
#include "ImageFile.h"

namespace Magpie {

struct Screenshot {
	enum class Result {
		Success,
		InvalidDirectory,
		InvalidFilenameTemplate,
		InternalError
	};

	// Turns pixels as VulkanDevice::ReadTexture returns them into
	// VK_FORMAT_R8G8B8A8_UNORM. Returns false for a format that an effect
	// output does not have.
	static bool ConvertToRgba8(ImageData& image) noexcept;

	// The name for a new file in `dir`: `baseName` itself, or
	// "baseName (n)" if files with that name exist, whatever their
	// extension. n is one more than the highest in use and at least 2.
	static std::string AppendSuffix(const std::filesystem::path& dir, std::string baseName) noexcept;

	// Writes `image` as a PNG file into `dir`, which is created if needed,
	// under a name built from the template. May be called from any thread.
	static Result Save(
		ImageData image,
		const std::filesystem::path& dir,
		std::string_view filenameTemplate,
		std::string_view windowTitle,
		std::string_view processName,
		std::filesystem::path& savedFile
	) noexcept;
};

}
