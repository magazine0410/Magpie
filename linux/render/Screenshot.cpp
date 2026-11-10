#include "Screenshot.h"
#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>
#include <mutex>
#include "Logger.h"
#include "ScreenshotFilenameTemplateHelper.h"

namespace Magpie {

// IEEE 754 half precision to single precision
static float HalfToFloat(uint16_t half) noexcept {
	const uint32_t sign = half >> 15;
	const uint32_t exponent = (half >> 10) & 0x1F;
	const uint32_t mantissa = half & 0x3FF;

	float value;
	if (exponent == 0) {
		value = std::ldexp((float)mantissa, -24);
	} else if (exponent == 31) {
		value = mantissa ? 0.0f : std::numeric_limits<float>::infinity();
	} else {
		value = std::ldexp((float)(mantissa | 0x400), (int)exponent - 25);
	}
	return sign ? -value : value;
}

bool Screenshot::ConvertToRgba8(ImageData& image) noexcept {
	const size_t pixelCount = (size_t)image.width * image.height;

	switch (image.format) {
	case VK_FORMAT_R8G8B8A8_UNORM:
		return image.pixels.size() == pixelCount * 4;
	case VK_FORMAT_B8G8R8A8_UNORM:
		if (image.pixels.size() != pixelCount * 4) {
			return false;
		}
		for (size_t i = 0; i < pixelCount; ++i) {
			std::swap(image.pixels[i * 4], image.pixels[i * 4 + 2]);
		}
		image.format = VK_FORMAT_R8G8B8A8_UNORM;
		return true;
	case VK_FORMAT_R16G16B16A16_SFLOAT:
	{
		if (image.pixels.size() != pixelCount * 8) {
			return false;
		}
		std::vector<uint8_t> converted(pixelCount * 4);
		for (size_t i = 0; i < pixelCount * 4; ++i) {
			uint16_t half;
			std::memcpy(&half, &image.pixels[i * 2], 2);
			const float value = std::clamp(HalfToFloat(half), 0.0f, 1.0f);
			converted[i] = (uint8_t)std::lround(value * 255.0f);
		}
		image.pixels = std::move(converted);
		image.format = VK_FORMAT_R8G8B8A8_UNORM;
		return true;
	}
	default:
		return false;
	}
}

std::string Screenshot::AppendSuffix(const std::filesystem::path& dir, std::string baseName) noexcept {
	uint32_t suffixNum = 1;

	std::error_code ec;
	for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(dir, ec)) {
		// Without the extension
		const std::string stem = entry.path().stem().string();
		std::string_view fileName(stem);
		if (!fileName.starts_with(baseName)) {
			continue;
		}

		// The name itself counts as number 1
		if (fileName.size() == baseName.size()) {
			suffixNum = std::max(suffixNum, 2u);
			continue;
		}

		// The suffix has at least four characters: " (n)"
		if (fileName.size() < baseName.size() + 4) {
			continue;
		}
		fileName.remove_prefix(baseName.size());
		if (!fileName.starts_with(" (") || !fileName.ends_with(')')) {
			continue;
		}

		uint32_t curSuffixNum;
		const char* begin = fileName.data() + 2;
		const char* end = fileName.data() + fileName.size() - 1;
		const std::from_chars_result result = std::from_chars(begin, end, curSuffixNum);
		if (result.ec != std::errc{} || result.ptr != end || curSuffixNum < 2) {
			continue;
		}
		suffixNum = std::max(suffixNum, curSuffixNum + 1);
	}

	if (suffixNum > 1) {
		baseName += " (" + std::to_string(suffixNum) + ')';
	}
	return baseName;
}

Screenshot::Result Screenshot::Save(
	ImageData image,
	const std::filesystem::path& dir,
	std::string_view filenameTemplate,
	std::string_view windowTitle,
	std::string_view processName,
	std::filesystem::path& savedFile
) noexcept {
	if (!ConvertToRgba8(image)) {
		Logger::Get().Error("The texture has a format that cannot be saved as PNG");
		return Result::InternalError;
	}

	if (dir.empty()) {
		return Result::InvalidDirectory;
	}
	std::error_code ec;
	std::filesystem::create_directories(dir, ec);
	if (ec) {
		Logger::Get().Error("Cannot create " + dir.string() + ": " + ec.message());
		return Result::InvalidDirectory;
	}

	const auto now = std::chrono::system_clock::now();
	const std::time_t seconds = std::chrono::system_clock::to_time_t(now);
	const int milliseconds = (int)(std::chrono::duration_cast<std::chrono::milliseconds>(
		now.time_since_epoch()).count() % 1000);
	std::tm localTime{};
	localtime_r(&seconds, &localTime);

	std::string fileName;
	if (filenameTemplate.empty() || !ScreenshotFilenameTemplateHelper::Apply(
		filenameTemplate, windowTitle, processName, localTime, milliseconds, fileName)) {
		return Result::InvalidFilenameTemplate;
	}

	// Under a lock so that two screenshots do not get the same number
	static std::mutex mutex;
	std::scoped_lock lock(mutex);

	savedFile = dir / (AppendSuffix(dir, std::move(fileName)) + ".png");
	// The alpha channel of an effect's output has no meaning
	return ImageFile::SavePng(savedFile, image, false) ? Result::Success : Result::InternalError;
}

}
