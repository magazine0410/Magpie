// Shared by the renderer tests: the Vulkan devices to test on, running an
// effect chain on an image, comparing images, and CPU reference scalers.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include <catch2/catch_test_macros.hpp>
#include "ImageScaler.h"
#include "StrHelper.h"
#include "VulkanDevice.h"

namespace RenderTest {

using namespace Magpie;

inline const std::filesystem::path BUNDLED_EFFECTS_DIR = MAGPIE_EFFECTS_DIR;
inline const std::filesystem::path TEST_EFFECTS_DIR = std::filesystem::path(MAGPIE_TEST_DATA_DIR) / "effects";
inline const std::filesystem::path REFERENCE_DIR = std::filesystem::path(MAGPIE_TEST_DATA_DIR) / "reference";

struct TestDevice {
	std::unique_ptr<VulkanDevice> device;
	GraphicsCardInfo info;
};

// Every usable graphics card, with the validation layers if they are
// installed. MAGPIE_TEST_GPUS limits the list to the given numbers (as
// printed by "magpie-scale-image --list-gpus"), for example "0,2".
inline std::vector<TestDevice>& TestDevices() {
	static std::vector<TestDevice> devices = [] {
		std::vector<TestDevice> result;

		std::vector<GraphicsCardInfo> cards;
		{
			VulkanDevice probe;
			if (!probe.Initialize({})) {
				return result;
			}
			cards = probe.GraphicsCards();
		}

		std::vector<int> wanted;
		if (const char* env = std::getenv("MAGPIE_TEST_GPUS")) {
			for (std::string_view part : StrHelper::Split(env, ',')) {
				wanted.push_back(std::atoi(std::string(part).c_str()));
			}
		}

		for (int i = 0; i < (int)cards.size(); ++i) {
			if (!cards[i].isUsable ||
				(!wanted.empty() && std::find(wanted.begin(), wanted.end(), i) == wanted.end())) {
				continue;
			}

			VulkanDevice::Options options;
			options.graphicsCardId = { i, cards[i].vendorId, cards[i].deviceId };

			options.validation = true;
			auto device = std::make_unique<VulkanDevice>();
			if (!device->Initialize(options)) {
				options.validation = false;
				device = std::make_unique<VulkanDevice>();
				if (!device->Initialize(options)) {
					continue;
				}
			}

			result.push_back({ std::move(device), cards[i] });
		}

		return result;
	}();
	return devices;
}

// Runs `test` once per device, in a section named after the card, and checks
// afterwards that the validation layers stayed silent.
inline void ForEachDevice(const std::function<void(VulkanDevice&, const GraphicsCardInfo&)>& test) {
	REQUIRE_FALSE(TestDevices().empty());

	for (TestDevice& testDevice : TestDevices()) {
		DYNAMIC_SECTION("on " << testDevice.info.name) {
			const uint32_t messagesBefore = testDevice.device->ValidationMessageCount();
			test(*testDevice.device, testDevice.info);
			CHECK(testDevice.device->ValidationMessageCount() == messagesBefore);
		}
	}
}

// On the integrated graphics of the Ryzen 7600X with Mesa 26.2.6, effects
// that use group-shared memory return wrong pixels in some runs. The cause
// was not found; the other two devices never showed it. Tests that depend on
// such effects report a warning instead of a failure on this card.
inline bool HasUnreliableSharedMemory(const GraphicsCardInfo& info) noexcept {
	return info.vendorId == 0x1002 && info.deviceId == 0x164e;
}

inline EffectOption Effect(
	std::string name,
	ScalingType scalingType = ScalingType::Normal,
	std::pair<float, float> scale = { 1.0f, 1.0f },
	std::map<std::string, float> parameters = {}
) {
	std::replace(name.begin(), name.end(), '/', '\\');
	return { .name = std::move(name), .parameters = std::move(parameters), .scalingType = scalingType, .scale = scale };
}

struct RunOptions {
	std::filesystem::path effectsDir = BUNDLED_EFFECTS_DIR;
	Size rendererSize;	// default: twice the input
	bool windowed = false;
	bool inlineParams = false;
	bool noFP16 = false;
	uint32_t frameCount = 0;
};

inline ImageScaler::Result Run(
	VulkanDevice& device,
	const ImageData& input,
	std::vector<EffectOption> effects,
	const RunOptions& runOptions = {}
) {
	ImageScaler::Request request;
	request.input = &input;
	request.options.effects = std::move(effects);
	request.options.IsWindowedMode(runOptions.windowed);
	request.options.IsInlineParams(runOptions.inlineParams);
	request.options.IsFP16Disabled(runOptions.noFP16);
	// Every run compiles, so that a stale cache cannot hide a change
	request.options.IsEffectCacheDisabled(true);
	request.config.effectsDir = runOptions.effectsDir;
	request.config.rendererSize = runOptions.rendererSize.width > 0
		? runOptions.rendererSize : Size{ (int32_t)input.width * 2, (int32_t)input.height * 2 };
	request.frameCount = runOptions.frameCount;

	ImageScaler::Result result;
	REQUIRE(ImageScaler::Scale(device, request, result));
	return result;
}

inline const uint8_t* Pixel(const ImageData& image, uint32_t x, uint32_t y) noexcept {
	return &image.pixels[((size_t)y * image.width + x) * 4];
}

struct ImageDiff {
	bool sameSize = false;
	int maxDiff = 255;
	double meanDiff = 255.0;
	// Share of the pixels with a channel that differs by more than 3
	double share3 = 1.0;
};

// Compares red, green and blue
inline ImageDiff Compare(const ImageData& a, const ImageData& b) noexcept {
	ImageDiff diff;
	if (a.width != b.width || a.height != b.height || a.pixels.size() != b.pixels.size()) {
		return diff;
	}

	diff.sameSize = true;
	diff.maxDiff = 0;
	uint64_t sum = 0;
	size_t above3 = 0;
	const size_t pixelCount = (size_t)a.width * a.height;
	for (size_t i = 0; i < pixelCount; ++i) {
		int pixelMax = 0;
		for (int c = 0; c < 3; ++c) {
			const int d = std::abs((int)a.pixels[i * 4 + c] - (int)b.pixels[i * 4 + c]);
			sum += (uint64_t)d;
			pixelMax = std::max(pixelMax, d);
		}
		diff.maxDiff = std::max(diff.maxDiff, pixelMax);
		above3 += pixelMax > 3;
	}
	diff.meanDiff = double(sum) / double(pixelCount * 3);
	diff.share3 = double(above3) / double(pixelCount);
	return diff;
}

inline ImageData LoadTestPicture() {
	ImageData image;
	REQUIRE(ImageFile::LoadPng(std::filesystem::path(MAGPIE_TEST_DATA_DIR) / "images" / "test96.png", image));
	return image;
}

// Reference scaler on the CPU: every output pixel is the weighted sum of the
// source texels at the offsets firstTap ... firstTap + tapCount - 1 from the
// texel left of (above) the sampling position, with texels outside the image
// taken from the edge. weight(d) is the kernel at the distance d in texels.
// With normalize the weights of each direction are divided by their sum.
// arStrength blends towards the result clamped to the range of the four
// texels around the sampling position (anti-ringing).
inline ImageData ScaleWithKernel(
	const ImageData& src,
	uint32_t width,
	uint32_t height,
	int firstTap,
	int tapCount,
	const std::function<double(double)>& weight,
	bool normalize,
	double arStrength = 0.0
) {
	ImageData dst;
	dst.format = src.format;
	dst.width = width;
	dst.height = height;
	dst.pixels.resize((size_t)width * height * 4);

	const auto texel = [&](int x, int y, int c) {
		x = std::clamp(x, 0, (int)src.width - 1);
		y = std::clamp(y, 0, (int)src.height - 1);
		return src.pixels[((size_t)y * src.width + x) * 4 + c] / 255.0;
	};

	const auto taps = [&](double pos, int& base, std::vector<double>& weights) {
		// pos is in texels; texel i covers [i, i + 1) and has its centre at i + 0.5
		base = (int)std::floor(pos - 0.5);
		const double f = pos - 0.5 - base;
		double sum = 0.0;
		for (int k = 0; k < tapCount; ++k) {
			weights[k] = weight(std::abs(firstTap + k - f));
			sum += weights[k];
		}
		if (normalize) {
			for (double& w : weights) {
				w /= sum;
			}
		}
	};

	std::vector<double> wx(tapCount), wy(tapCount);
	for (uint32_t y = 0; y < height; ++y) {
		int baseY;
		taps((y + 0.5) * src.height / height, baseY, wy);

		for (uint32_t x = 0; x < width; ++x) {
			int baseX;
			taps((x + 0.5) * src.width / width, baseX, wx);

			for (int c = 0; c < 3; ++c) {
				double value = 0.0;
				for (int j = 0; j < tapCount; ++j) {
					for (int i = 0; i < tapCount; ++i) {
						value += wy[j] * wx[i] * texel(baseX + firstTap + i, baseY + firstTap + j, c);
					}
				}

				if (arStrength > 0.0) {
					const double a = texel(baseX, baseY, c), b = texel(baseX + 1, baseY, c);
					const double d = texel(baseX, baseY + 1, c), e = texel(baseX + 1, baseY + 1, c);
					const double clamped = std::clamp(value, std::min({ a, b, d, e }), std::max({ a, b, d, e }));
					value += (clamped - value) * arStrength;
				}

				dst.pixels[((size_t)y * width + x) * 4 + c] =
					(uint8_t)std::lround(std::clamp(value, 0.0, 1.0) * 255.0);
			}
			dst.pixels[((size_t)y * width + x) * 4 + 3] = 255;
		}
	}

	return dst;
}

}
