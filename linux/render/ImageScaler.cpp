#include "ImageScaler.h"
#include <algorithm>
#include <cstring>
#include <fmt/format.h>
#include "Logger.h"
#include "ScalingGeometry.h"
#include "VulkanDevice.h"

namespace Magpie {

bool ImageScaler::Scale(VulkanDevice& device, const Request& request, Result& result) noexcept {
	const ImageData* input = request.input;
	if (input->format != VK_FORMAT_R8G8B8A8_UNORM) {
		Logger::Get().Error("The input image must be R8G8B8A8_UNORM");
		return false;
	}

	// Cropping. The minimum size of a source only applies here; an image may
	// be smaller than a window that Magpie would scale.
	ImageData cropped;
	const Cropping& cropping = request.options.cropping;
	if (cropping != Cropping{}) {
		Rect srcRect{ 0, 0, (int32_t)input->width, (int32_t)input->height };
		if (!ScalingGeometry::CropSourceRect(srcRect, cropping)) {
			Logger::Get().Error("Cropping leaves too little of the image");
			return false;
		}

		cropped.format = input->format;
		cropped.width = (uint32_t)srcRect.Width();
		cropped.height = (uint32_t)srcRect.Height();
		cropped.pixels.resize((size_t)cropped.width * cropped.height * 4);
		for (uint32_t y = 0; y < cropped.height; ++y) {
			std::memcpy(
				&cropped.pixels[(size_t)y * cropped.width * 4],
				&input->pixels[(((size_t)srcRect.top + y) * input->width + srcRect.left) * 4],
				(size_t)cropped.width * 4
			);
		}
		input = &cropped;
	}

	std::shared_ptr<Texture> inputTexture = device.CreateTexture(input->format, input->width, input->height,
		VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
	if (!inputTexture || !device.UploadTexture(*inputTexture, input->pixels, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)) {
		Logger::Get().Error("Creating the input texture failed");
		return false;
	}

	EffectChain chain;
	if (!chain.Initialize(device, request.options, request.config, inputTexture)) {
		Logger::Get().Error("Initializing the effects failed");
		return false;
	}

	const auto draw = [&] {
		return device.Execute([&](VkCommandBuffer cmd) {
			chain.Draw(cmd, request.frameCount);
		});
	};

	if (!draw()) {
		return false;
	}

	result.timings.clear();
	if (request.timedFrames > 0) {
		if (!chain.Profiler().Start(device, chain.PassCount())) {
			return false;
		}

		for (const EffectDesc* desc : chain.ActiveEffectDescs()) {
			for (size_t i = 0; i < desc->passes.size(); ++i) {
				std::string name = desc->name;
				std::replace(name.begin(), name.end(), '\\', '/');
				result.timings.push_back({ fmt::format("{} pass {}", name, i + 1), 0.0f });
			}
		}

		for (uint32_t frame = 0; frame < request.timedFrames; ++frame) {
			if (!draw()) {
				return false;
			}

			chain.Profiler().QueryTimings();
			const SmallVector<float> frameTimings = chain.Profiler().GetTimings();
			for (size_t i = 0; i < frameTimings.size() && i < result.timings.size(); ++i) {
				result.timings[i].gpuTime += frameTimings[i] / request.timedFrames;
			}
		}
	}

	const Texture& output = *chain.Output();
	result.output.format = output.format;
	result.output.width = output.width;
	result.output.height = output.height;
	if (!device.ReadTexture(output, result.output.pixels)) {
		Logger::Get().Error("Reading the output texture failed");
		return false;
	}

	result.destRect = ScalingGeometry::CalcDestRect(
		request.config.rendererSize,
		{ (int32_t)output.width, (int32_t)output.height },
		request.options.outputAlignment
	);

	return true;
}

ImageData ImageScaler::ComposeFrame(const Result& result, Size rendererSize) noexcept {
	ImageData frame;
	frame.format = VK_FORMAT_R8G8B8A8_UNORM;
	frame.width = (uint32_t)rendererSize.width;
	frame.height = (uint32_t)rendererSize.height;
	// Opaque black
	frame.pixels.assign((size_t)frame.width * frame.height * 4, 0);
	for (size_t i = 3; i < frame.pixels.size(); i += 4) {
		frame.pixels[i] = 255;
	}

	// The output can be larger than the renderer area only if the caller
	// composes with another size than it scaled for, so clip.
	const Rect& dest = result.destRect;
	const int32_t left = std::max(dest.left, 0);
	const int32_t right = std::min(dest.right, rendererSize.width);
	for (int32_t y = std::max(dest.top, 0), bottom = std::min(dest.bottom, rendererSize.height); y < bottom; ++y) {
		if (left >= right) {
			break;
		}

		std::memcpy(
			&frame.pixels[((size_t)y * frame.width + left) * 4],
			&result.output.pixels[((size_t)(y - dest.top) * result.output.width + (left - dest.left)) * 4],
			(size_t)(right - left) * 4
		);
	}

	return frame;
}

}
