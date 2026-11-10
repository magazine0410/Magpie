// The effect chain on real Vulkan devices. Every test runs on each usable
// graphics card, including the software implementation if it is installed.
#include <cmath>
#include <numbers>
#include <catch2/catch_test_macros.hpp>
#include <catch2/reporters/catch_reporter_event_listener.hpp>
#include <catch2/reporters/catch_reporter_registrars.hpp>
#include <spdlog/sinks/stdout_sinks.h>
#include "EffectChain.h"
#include "Logger.h"
#include "RenderTestHelpers.h"
#include "ScalingGeometry.h"
#include "ScalingMode.h"
#include "Settings.h"
#include "Swapchain.h"

using namespace Magpie;
using namespace RenderTest;

// The devices are destroyed at the end of the run and not with the other
// static objects: by then the validation layers have destroyed their own, and
// calling into them crashes.
class DeviceCleanup : public Catch::EventListenerBase {
public:
	using Catch::EventListenerBase::EventListenerBase;

	// Shows what the validation layers and the renderer report
	void testRunStarting(const Catch::TestRunInfo&) override {
		Logger::Get().InitializeWithSink(spdlog::level::warn, std::make_shared<spdlog::sinks::stderr_sink_mt>());
	}

	void testRunEnded(const Catch::TestRunStats&) override {
		TestDevices().clear();
	}
};
CATCH_REGISTER_LISTENER(DeviceCleanup)

TEST_CASE("Nearest scales by whole factors exactly", "[render]") {
	const ImageData input = LoadTestPicture();

	ForEachDevice([&](VulkanDevice& device, const GraphicsCardInfo&) {
		for (const uint32_t factor : { 2u, 3u }) {
			const ImageScaler::Result result = Run(device, input,
				{ Effect("Nearest", ScalingType::Normal, { (float)factor, (float)factor }) },
				{ .rendererSize = { 1000, 1000 } });

			REQUIRE(result.output.width == input.width * factor);
			REQUIRE(result.output.height == input.height * factor);

			bool equal = true;
			for (uint32_t y = 0; y < result.output.height && equal; ++y) {
				for (uint32_t x = 0; x < result.output.width && equal; ++x) {
					equal = std::memcmp(Pixel(result.output, x, y), Pixel(input, x / factor, y / factor), 3) == 0;
				}
			}
			CHECK(equal);
		}
	});
}

TEST_CASE("Bilinear, Bicubic and Lanczos match CPU references", "[render]") {
	const ImageData input = LoadTestPicture();
	// Not a whole factor, and different in both directions
	const uint32_t width = 163, height = 101;
	const auto scale = [&](std::string name, std::map<std::string, float> parameters = {}) {
		return Effect(std::move(name), ScalingType::Absolute, { (float)width, (float)height }, std::move(parameters));
	};

	// Mitchell-Netravali cubic
	const auto cubic = [](double B, double C) {
		return [=](double d) {
			if (d < 1.0) {
				return ((12 - 9 * B - 6 * C) * d * d * d + (-18 + 12 * B + 6 * C) * d * d + (6 - 2 * B)) / 6;
			} else if (d < 2.0) {
				return ((-B - 6 * C) * d * d * d + (6 * B + 30 * C) * d * d + (-12 * B - 48 * C) * d + (8 * B + 24 * C)) / 6;
			}
			return 0.0;
		};
	};
	const auto lanczos3 = [](double d) {
		const double x = std::max(d, 1e-5) * std::numbers::pi;
		return std::sin(x) * std::sin(x / 3) / (x * x);
	};

	ForEachDevice([&](VulkanDevice& device, const GraphicsCardInfo&) {
		const auto check = [&](const ImageData& output, const ImageData& reference, int maxDiff) {
			const ImageDiff diff = Compare(output, reference);
			CAPTURE(diff.maxDiff, diff.meanDiff);
			CHECK(diff.sameSize);
			CHECK(diff.maxDiff <= maxDiff);
			CHECK(diff.meanDiff <= 0.5);
		};

		SECTION("Bilinear") {
			// Graphics hardware filters with 8-bit weights
			check(Run(device, input, { scale("Bilinear") }).output,
				ScaleWithKernel(input, width, height, 0, 2, [](double d) { return std::max(0.0, 1.0 - d); }, false), 2);
		}

		SECTION("Bicubic with its default parameters") {
			check(Run(device, input, { scale("Bicubic") }).output,
				ScaleWithKernel(input, width, height, -1, 4, cubic(0.33, 0.33), false), 3);
		}

		SECTION("Bicubic with parameters from the options") {
			const ImageData reference = ScaleWithKernel(input, width, height, -1, 4, cubic(0.0, 0.75), false);
			const std::map<std::string, float> parameters{ {"paramB", 0.0f}, {"paramC", 0.75f} };

			check(Run(device, input, { scale("Bicubic", parameters) }).output, reference, 3);
			// The same parameters compiled into the shader
			check(Run(device, input, { scale("Bicubic", parameters) }, { .inlineParams = true }).output, reference, 3);
		}

		SECTION("Lanczos without and with anti-ringing") {
			check(Run(device, input, { scale("Lanczos", { {"ARStrength", 0.0f} }) }).output,
				ScaleWithKernel(input, width, height, -2, 6, lanczos3, true, 0.0), 2);
			check(Run(device, input, { scale("Lanczos", { {"ARStrength", 1.0f} }) }).output,
				ScaleWithKernel(input, width, height, -2, 6, lanczos3, true, 1.0), 2);
		}
	});
}

TEST_CASE("Constants and parameters reach the shaders", "[render]") {
	const ImageData input = LoadTestPicture();	// 96x72

	ForEachDevice([&](VulkanDevice& device, const GraphicsCardInfo&) {
		for (const bool inlineParams : { false, true }) {
			const ImageScaler::Result result = Run(device, input,
				{ Effect("TestConstants", ScalingType::Normal, { 1.0f, 1.0f }, { {"fParam", 0.5f}, {"iParam", 7.0f} }) },
				{ .effectsDir = TEST_EFFECTS_DIR, .inlineParams = inlineParams });

			// WIDTH INPUT_WIDTH * 2, HEIGHT INPUT_HEIGHT + 3
			REQUIRE(result.output.width == 192);
			REQUIRE(result.output.height == 75);

			const auto item = [&](uint32_t i) {
				const uint8_t* pixel = Pixel(result.output, i, 0);
				return std::pair<int, int>(pixel[0], pixel[1]);
			};
			CAPTURE(inlineParams);
			CHECK(item(0) == std::pair(96, 72));		// input size
			CHECK(item(1) == std::pair(192, 75));		// output size
			CHECK(item(2) == std::pair(96, 72));		// 1 / input texel size
			CHECK(item(3) == std::pair(192, 75));		// 1 / output texel size
			CHECK(item(4) == std::pair(40, 21));		// scale * 20: 2 and 75/72
			CHECK(item(5) == std::pair(50, 7));		// the parameters
			// Seen by a pass in pixel shader style that is not the last one:
			// the centre of texel (2, 3) of its output, times 2
			CHECK(item(6) == std::pair(5, 7));
			CHECK(item(7) == std::pair(50, 7));
		}

		// Without options the defaults apply
		const ImageScaler::Result defaults = Run(device, input, { Effect("TestConstants") },
			{ .effectsDir = TEST_EFFECTS_DIR });
		CHECK(Pixel(defaults.output, 5, 0)[0] == 25);
		CHECK(Pixel(defaults.output, 5, 0)[1] == 3);
	});
}

TEST_CASE("Every effect texture format holds the values its name promises", "[render]") {
	const ImageData input = LoadTestPicture();

	ForEachDevice([&](VulkanDevice& device, const GraphicsCardInfo&) {
		const ImageScaler::Result result = Run(device, input, { Effect("TestFormats") },
			{ .effectsDir = TEST_EFFECTS_DIR });

		REQUIRE(result.output.width == (uint32_t)EffectIntermediateTextureFormat::UNKNOWN);
		for (uint32_t i = 0; i < result.output.width; ++i) {
			CAPTURE(i);
			// Green means the format returned what was written
			CHECK(Pixel(result.output, i, 0)[0] == 0);
			CHECK(Pixel(result.output, i, 0)[1] == 255);
		}
	});
}

TEST_CASE("Dynamic effects see the frame counter", "[render]") {
	const ImageData input = LoadTestPicture();

	ForEachDevice([&](VulkanDevice& device, const GraphicsCardInfo&) {
		for (const uint32_t frameCount : { 0u, 200u }) {
			const ImageScaler::Result result = Run(device, input, { Effect("TestDynamic") },
				{ .effectsDir = TEST_EFFECTS_DIR, .frameCount = frameCount });
			CHECK(Pixel(result.output, 10, 10)[0] == frameCount);
		}
	});
}

TEST_CASE("Textures named by SOURCE are loaded from the effect's directory", "[render]") {
	const ImageData input = LoadTestPicture();

	ForEachDevice([&](VulkanDevice& device, const GraphicsCardInfo&) {
		const ImageScaler::Result result = Run(device, input, { Effect("TestSource") },
			{ .effectsDir = TEST_EFFECTS_DIR });

		REQUIRE(result.output.width == 8);
		REQUIRE(result.output.height == 2);
		for (uint32_t y = 0; y < 2; ++y) {
			for (uint32_t x = 0; x < 4; ++x) {
				CAPTURE(x, y);
				// TestSource8.dds
				const uint8_t* texel8 = Pixel(result.output, x, y);
				CHECK(texel8[0] == x * 60 + 10);
				CHECK(texel8[1] == y * 100 + 20);
				CHECK(texel8[2] == x * 40 + y * 30 + 5);

				// TestSource16F.dds holds (x, 2y + 0.5, (x + y) / 4); the effect divides by 4
				const uint8_t* texel16 = Pixel(result.output, x + 4, y);
				CHECK(std::abs(texel16[0] - x / 4.0 * 255) <= 1);
				CHECK(std::abs(texel16[1] - (y * 2 + 0.5) / 4.0 * 255) <= 1);
				CHECK(std::abs(texel16[2] - (x + y) / 16.0 * 255) <= 1);
			}
		}
	});
}

TEST_CASE("Reads outside a texture give zero and writes outside are dropped", "[render]") {
	const ImageData input = LoadTestPicture();

	ForEachDevice([&](VulkanDevice& device, const GraphicsCardInfo&) {
		const ImageScaler::Result result = Run(device, input, { Effect("TestBounds") },
			{ .effectsDir = TEST_EFFECTS_DIR });

		REQUIRE(result.output.width == 5);
		REQUIRE(result.output.height == 3);
		for (uint32_t y = 0; y < 3; ++y) {
			for (uint32_t x = 0; x < 5; ++x) {
				CAPTURE(x, y);
				CHECK(Pixel(result.output, x, y)[0] == 0);
				CHECK(Pixel(result.output, x, y)[1] == 255);
				CHECK(Pixel(result.output, x, y)[2] == Pixel(input, x, y)[2]);
			}
		}
	});
}

TEST_CASE("Group-shared memory keeps what the threads write", "[render]") {
	const ImageData input = LoadTestPicture();

	ForEachDevice([&](VulkanDevice& device, const GraphicsCardInfo& info) {
		const ImageScaler::Result result = Run(device, input, { Effect("TestSharedMemory") },
			{ .effectsDir = TEST_EFFECTS_DIR });

		const ImageDiff diff = Compare(result.output, input);
		if (diff.maxDiff != 0 && HasUnreliableSharedMemory(info)) {
			WARN("Group-shared memory returned wrong pixels on " << info.name << " (known for this card)");
		} else {
			CHECK(diff.maxDiff == 0);
		}
	});
}

TEST_CASE("Scaling types decide the output size", "[render]") {
	const ImageData input = LoadTestPicture();	// 96x72
	const Size rendererSize{ 300, 200 };

	ForEachDevice([&](VulkanDevice& device, const GraphicsCardInfo&) {
		const auto outputSize = [&](ScalingType type, std::pair<float, float> scale, bool windowed = false) {
			const ImageScaler::Result result = Run(device, input, { Effect("Bilinear", type, scale) },
				{ .rendererSize = rendererSize, .windowed = windowed });
			// The position follows from the size: centred by default
			CHECK(result.destRect.left == (rendererSize.width - (int32_t)result.output.width) / 2);
			CHECK(result.destRect.top == (rendererSize.height - (int32_t)result.output.height) / 2);
			return Size{ (int32_t)result.output.width, (int32_t)result.output.height };
		};

		CHECK(outputSize(ScalingType::Normal, { 1.5f, 2.0f }) == Size{ 144, 144 });
		CHECK(outputSize(ScalingType::Absolute, { 50.0f, 40.0f }) == Size{ 50, 40 });
		CHECK(outputSize(ScalingType::Fill, { 1.0f, 1.0f }) == Size{ 300, 200 });
		// The height limits: 200 / 72
		CHECK(outputSize(ScalingType::Fit, { 1.0f, 1.0f }) == Size{ 267, 200 });
		CHECK(outputSize(ScalingType::Fit, { 0.5f, 0.5f }) == Size{ 133, 100 });

		// Larger than the renderer area: Bicubic is appended and fits it in
		CHECK(outputSize(ScalingType::Normal, { 4.0f, 4.0f }) == Size{ 267, 200 });

		// A window is always filled: by Bicubic if the effects end at another
		// size, and a Fit by 1 counts as Fill
		CHECK(outputSize(ScalingType::Normal, { 1.5f, 1.5f }, true) == Size{ 300, 200 });
		CHECK(outputSize(ScalingType::Fit, { 1.0f, 1.0f }, true) == Size{ 300, 200 });
	});
}

TEST_CASE("A resized chain renders like a new one", "[render]") {
	const ImageData input = LoadTestPicture();

	ForEachDevice([&](VulkanDevice& device, const GraphicsCardInfo&) {
		const auto upload = [&](const ImageData& image) {
			std::shared_ptr<Texture> texture = device.CreateTexture(image.format, image.width, image.height,
				VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
			REQUIRE(texture);
			REQUIRE(device.UploadTexture(*texture, image.pixels, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL));
			return texture;
		};
		const auto draw = [&](EffectChain& chain) {
			REQUIRE(device.Execute([&](VkCommandBuffer cmd) { chain.Draw(cmd, 0); }));
			ImageData output;
			output.format = chain.Output()->format;
			output.width = chain.Output()->width;
			output.height = chain.Output()->height;
			REQUIRE(device.ReadTexture(*chain.Output(), output.pixels));
			return output;
		};

		// A fixed doubling to 192x144 followed by a sharpener
		ScalingOptions options;
		options.effects = { Effect("Anime4K/Anime4K_Upscale_S"), Effect("FSR/FSR_RCAS") };
		options.IsEffectCacheDisabled(true);

		const std::shared_ptr<Texture> texture = upload(input);
		const auto fresh = [&](Size rendererSize, const std::shared_ptr<Texture>& source) {
			EffectChain chain;
			REQUIRE(chain.Initialize(device, options, { BUNDLED_EFFECTS_DIR, rendererSize }, source));
			return draw(chain);
		};

		EffectChain chain;
		REQUIRE(chain.Initialize(device, options, { BUNDLED_EFFECTS_DIR, { 400, 300 } }, texture));
		CHECK(chain.ActiveEffectDescs().size() == 2);
		const ImageData large = draw(chain);
		CHECK(large.width == 192);

		// Too small for the doubled image: Bicubic is appended
		REQUIRE(chain.Resize({ 150, 100 }, texture));
		CHECK(chain.ActiveEffectDescs().size() == 3);
		const ImageData small = draw(chain);
		CHECK(small.width == 133);
		CHECK(small.height == 100);
		CHECK(Compare(small, fresh({ 150, 100 }, texture)).maxDiff == 0);

		// Another size with Bicubic still in place
		REQUIRE(chain.Resize({ 100, 150 }, texture));
		CHECK(Compare(draw(chain), fresh({ 100, 150 }, texture)).maxDiff == 0);

		// Large again: Bicubic is removed
		REQUIRE(chain.Resize({ 400, 300 }, texture));
		CHECK(chain.ActiveEffectDescs().size() == 2);
		CHECK(Compare(draw(chain), large).maxDiff == 0);

		// A source of another size
		ImageData half;
		half.format = input.format;
		half.width = input.width / 2;
		half.height = input.height;
		half.pixels.resize((size_t)half.width * half.height * 4);
		for (uint32_t y = 0; y < half.height; ++y) {
			std::memcpy(&half.pixels[(size_t)y * half.width * 4], Pixel(input, 0, y), (size_t)half.width * 4);
		}
		const std::shared_ptr<Texture> halfTexture = upload(half);
		REQUIRE(chain.Resize({ 400, 300 }, halfTexture));
		const ImageData fromHalf = draw(chain);
		CHECK(fromHalf.width == 96);
		CHECK(Compare(fromHalf, fresh({ 400, 300 }, halfTexture)).maxDiff == 0);
	});
}

TEST_CASE("Compose places the output in the frame and converts the channel order", "[render]") {
	const ImageData input = LoadTestPicture();
	const Size frameSize{ 150, 100 };

	ForEachDevice([&](VulkanDevice& device, const GraphicsCardInfo&) {
		// 96x72 placed in 150x100, which leaves bars on all sides for some alignments
		ImageScaler::Result result;
		result.output = input;

		const std::shared_ptr<Texture> source = device.CreateTexture(input.format, input.width, input.height,
			VulkanDevice::EFFECT_TEXTURE_USAGE);
		REQUIRE(source);
		REQUIRE(device.UploadTexture(*source, input.pixels, VK_IMAGE_LAYOUT_GENERAL));

		for (const VkFormat targetFormat : { VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_B8G8R8A8_UNORM }) {
			for (const OutputAlignment alignment : { OutputAlignment::Center, OutputAlignment::LeftTop, OutputAlignment::RightBottom }) {
				CAPTURE((int)targetFormat, (int)alignment);
				result.destRect = ScalingGeometry::CalcDestRect(frameSize, { 96, 72 }, alignment);

				const std::shared_ptr<Texture> target = device.CreateTexture(targetFormat,
					(uint32_t)frameSize.width, (uint32_t)frameSize.height,
					VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
				REQUIRE(target);
				REQUIRE(device.SetInitialLayout(*target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL));
				REQUIRE(device.Execute([&](VkCommandBuffer cmd) {
					Swapchain::Compose(cmd, *source, target->image, frameSize, result.destRect);
				}));

				ImageData frame;
				frame.format = VK_FORMAT_R8G8B8A8_UNORM;
				frame.width = target->width;
				frame.height = target->height;
				REQUIRE(device.ReadTexture(*target, frame.pixels));
				if (targetFormat == VK_FORMAT_B8G8R8A8_UNORM) {
					// The bytes are blue, green, red; bring them into the order of the reference
					for (size_t i = 0; i < frame.pixels.size(); i += 4) {
						std::swap(frame.pixels[i], frame.pixels[i + 2]);
					}
				}

				CHECK(Compare(frame, ImageScaler::ComposeFrame(result, frameSize)).maxDiff == 0);
			}
		}

		// A source that fills the frame leaves no bars and needs no clearing
		const std::shared_ptr<Texture> target = device.CreateTexture(VK_FORMAT_R8G8B8A8_UNORM, input.width, input.height,
			VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
		REQUIRE(target);
		REQUIRE(device.SetInitialLayout(*target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL));
		REQUIRE(device.Execute([&](VkCommandBuffer cmd) {
			Swapchain::Compose(cmd, *source, target->image, { 96, 72 }, { 0, 0, 96, 72 });
		}));
		ImageData copy;
		copy.format = input.format;
		copy.width = input.width;
		copy.height = input.height;
		REQUIRE(device.ReadTexture(*target, copy.pixels));
		CHECK(Compare(copy, input).maxDiff == 0);
	});
}

TEST_CASE("The profiler reports a time for every pass", "[render]") {
	const ImageData input = LoadTestPicture();

	ForEachDevice([&](VulkanDevice& device, const GraphicsCardInfo&) {
		ImageScaler::Request request;
		request.input = &input;
		request.options.effects = { Effect("FSR/FSR_EASU", ScalingType::Fit), Effect("FSR/FSR_RCAS") };
		request.options.IsEffectCacheDisabled(true);
		request.config = { BUNDLED_EFFECTS_DIR, { 192, 144 } };
		request.timedFrames = 3;

		ImageScaler::Result result;
		REQUIRE(ImageScaler::Scale(device, request, result));
		REQUIRE(result.timings.size() == 2);
		CHECK(result.timings[0].name == "FSR/FSR_EASU pass 1");
		CHECK(result.timings[1].name == "FSR/FSR_RCAS pass 1");
		CHECK(result.timings[0].gpuTime > 0.0f);
		CHECK(result.timings[1].gpuTime > 0.0f);
		// Far below a second for an image of this size, whatever the device
		CHECK(result.timings[0].gpuTime + result.timings[1].gpuTime < 1000.0f);
	});
}

// The pictures in tests/data/reference. Setting MAGPIE_UPDATE_REFERENCES
// writes the "mode_" files anew from the first device instead of comparing;
// the "mpv_" files come from tests/tools/render_mpv_references.sh.
static void CheckAgainstReference(
	const ImageData& output,
	const std::string& referenceName,
	const GraphicsCardInfo& info,
	bool usesSharedMemory,
	int maxDiff,
	double maxShare3
) {
	const std::filesystem::path fileName = REFERENCE_DIR / (referenceName + ".png");

	if (std::getenv("MAGPIE_UPDATE_REFERENCES") && referenceName.starts_with("mode_")) {
		if (&info == &TestDevices().front().info) {
			REQUIRE(ImageFile::SavePng(fileName, output, false));
		}
		return;
	}

	ImageData reference;
	REQUIRE(ImageFile::LoadPng(fileName, reference));

	const ImageDiff diff = Compare(output, reference);
	CAPTURE(referenceName, diff.maxDiff, diff.meanDiff, diff.share3);
	const bool matches = diff.sameSize && diff.maxDiff <= maxDiff && diff.share3 <= maxShare3 && diff.meanDiff <= 1.0;
	if (!matches && usesSharedMemory && HasUnreliableSharedMemory(info)) {
		WARN(referenceName << " differs on " << info.name << " (group-shared memory, known for this card)");
	} else {
		CHECK(matches);
	}
}

TEST_CASE("Anime4K effects match the original shaders rendered by mpv", "[render][reference]") {
	const ImageData input = LoadTestPicture();

	ForEachDevice([&](VulkanDevice& device, const GraphicsCardInfo& info) {
		for (const char* name : { "Upscale_S", "Upscale_L", "Upscale_Denoise_L", "Restore_M" }) {
			// With and without 16-bit floats
			for (const bool noFP16 : { false, true }) {
				const ImageScaler::Result result = Run(device, input,
					{ Effect(std::string("Anime4K/Anime4K_") + name) }, { .noFP16 = noFP16 });
				// mpv and this renderer agreed within 2 of 255 on all devices
				// when the reference pictures were made
				CheckAgainstReference(result.output, std::string("mpv_Anime4K_") + name, info, false, 3, 0.0);
			}
		}
	});
}

TEST_CASE("The initial scaling modes match their reference pictures", "[render][reference]") {
	const ImageData input = LoadTestPicture();

	Settings settings;
	settings.SetInitialSettings();
	REQUIRE(settings.scalingModes.size() == 7);

	ForEachDevice([&](VulkanDevice& device, const GraphicsCardInfo& info) {
		for (size_t i = 0; i < settings.scalingModes.size(); ++i) {
			const ScalingMode& mode = settings.scalingModes[i];

			std::vector<EffectOption> effects;
			bool usesSharedMemory = false;
			for (const EffectItem& item : mode.effects) {
				effects.push_back((EffectOption)item);
				usesSharedMemory |= item.name.starts_with("FSRCNNX");
			}

			std::string referenceName = "mode_" + std::to_string(i) + "_" + mode.name;
			std::replace(referenceName.begin(), referenceName.end(), ' ', '_');

			// A screen of twice the size of the source
			const ImageScaler::Result result = Run(device, input, std::move(effects));
			// Drivers differ in rounding, and effects that decide by thresholds
			// turn that into a few pixels that differ more
			CheckAgainstReference(result.output, referenceName, info, usesSharedMemory, 255, 0.01);
		}
	});
}
