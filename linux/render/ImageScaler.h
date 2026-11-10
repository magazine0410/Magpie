// Scales one image off screen with the effects of a scaling mode. Used by
// magpie-scale-image and by the tests; it makes the effect chain testable
// with no compositor involved.
#pragma once
#include <string>
#include <vector>
#include "EffectChain.h"
#include "Geometry.h"
#include "ImageFile.h"
#include "ScalingOptions.h"

namespace Magpie {

class VulkanDevice;

struct ImageScaler {
	struct Request {
		// Must be VK_FORMAT_R8G8B8A8_UNORM
		const ImageData* input = nullptr;
		// Used: effects, cropping, outputAlignment and the flags that
		// EffectChain::Initialize names
		ScalingOptions options;
		EffectChain::Config config;
		// What dynamic effects see as the frame counter
		uint32_t frameCount = 0;
		// Number of extra frames to render for measuring the GPU time of the
		// passes; 0 turns the measurement off
		uint32_t timedFrames = 0;
	};

	struct PassTiming {
		std::string name;
		// Average over the timed frames, in milliseconds
		float gpuTime = 0.0f;
	};

	struct Result {
		// Output of the last effect
		ImageData output;
		// Where the output goes inside the renderer area
		Rect destRect;
		// One entry per pass; empty unless timedFrames was set
		std::vector<PassTiming> timings;
	};

	static bool Scale(VulkanDevice& device, const Request& request, Result& result) noexcept;

	// The renderer area as it would be shown: the output at destRect, black
	// elsewhere.
	static ImageData ComposeFrame(const Result& result, Size rendererSize) noexcept;
};

}
