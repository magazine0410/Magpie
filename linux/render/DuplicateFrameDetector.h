// Decides whether a captured frame equals the previous one, so that it need
// not be scaled again. Linux counterpart of the duplicate frame handling in
// src/Magpie.Core/FrameSourceBase.cpp.
#pragma once
#include <cstdint>
#include <memory>
#include <utility>
#include <vulkan/vulkan.h>
#include "ScalingOptions.h"

namespace Magpie {

struct Texture;
struct Buffer;
class VulkanDevice;

class DuplicateFrameDetector {
public:
	DuplicateFrameDetector() noexcept;
	DuplicateFrameDetector(const DuplicateFrameDetector&) = delete;
	DuplicateFrameDetector(DuplicateFrameDetector&&) = delete;

	// No GPU work that uses the detector may be pending
	~DuplicateFrameDetector();

	// `mode` must not be Never: a frame source that never checks does not
	// need a detector. With statistics enabled, Dynamic mode compares every
	// frame and counts the frames it would have judged wrongly.
	void Initialize(VulkanDevice& device, DuplicateFrameDetectionMode mode, bool isStatisticsEnabled) noexcept;

	// Call for every frame the source delivers. Returns true if the frame
	// should be dropped as a duplicate. `frame` needs the usages sampled and
	// transfer source, and all frames must have the same size and format. The
	// call submits GPU work and waits for it.
	bool OnNewFrame(const Texture& frame) noexcept;

	// (frames wrongly taken as new, frames not compared) since the start, for
	// Dynamic mode with statistics
	std::pair<uint32_t, uint32_t> GetStatisticsForDynamicDetection() const noexcept {
		return _statistics;
	}

private:
	bool _InitCheckingForDuplicateFrame(const Texture& frame) noexcept;

	bool _IsDuplicateFrame(const Texture& frame) noexcept;

	void _CopyToPrevFrame(const Texture& frame) noexcept;

	VulkanDevice* _device = nullptr;
	DuplicateFrameDetectionMode _mode = DuplicateFrameDetectionMode::Dynamic;
	bool _isStatisticsEnabled = false;

	std::shared_ptr<Texture> _prevFrame;
	// One uint that the shader sets when it finds a difference
	std::unique_ptr<Buffer> _resultBuffer;

	VkDescriptorSetLayout _setLayout = VK_NULL_HANDLE;
	VkPipelineLayout _pipelineLayout = VK_NULL_HANDLE;
	VkPipeline _pipeline = VK_NULL_HANDLE;
	VkDescriptorPool _descriptorPool = VK_NULL_HANDLE;
	VkDescriptorSet _descriptorSet = VK_NULL_HANDLE;
	// The frame the descriptor set refers to
	VkImageView _boundFrameView = VK_NULL_HANDLE;
	std::pair<uint32_t, uint32_t> _dispatchCount;

	std::pair<uint32_t, uint32_t> _statistics;
	uint16_t _nextSkipCount;
	uint16_t _framesLeft;
	bool _isCheckingForDuplicateFrame = true;
	// Set when the GPU resources could not be created; every frame is new then
	bool _failed = false;
};

}
