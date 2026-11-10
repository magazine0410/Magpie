// GPU time of every effect pass, measured with timestamp queries. Linux
// counterpart of src/Magpie.Core/EffectsProfiler.h.
#pragma once
#include <cstdint>
#include <mutex>
#include <vector>
#include <vulkan/vulkan.h>
#include "SmallVector.h"

namespace Magpie {

class VulkanDevice;

class EffectsProfiler {
public:
	EffectsProfiler() = default;
	EffectsProfiler(const EffectsProfiler&) = delete;
	EffectsProfiler(EffectsProfiler&&) = delete;
	~EffectsProfiler();

	// Fails if the device has no timestamps
	bool Start(VulkanDevice& device, uint32_t passCount) noexcept;

	void Stop() noexcept;

	bool IsProfiling() const noexcept {
		return _queryPool != VK_NULL_HANDLE;
	}

	// No GPU work that uses the profiler may be pending
	void SetPassCount(uint32_t passCount) noexcept;

	void OnBeginEffects(VkCommandBuffer cmd) noexcept;

	void OnEndPass(VkCommandBuffer cmd) noexcept;

	// Call when the command buffer of the frame has been executed
	void QueryTimings() noexcept;

	// Milliseconds per pass of the last frame, and empty if no frame has been
	// rendered since the previous call. May be called from another thread.
	SmallVector<float> GetTimings() noexcept;

private:
	bool _CreateQueryPool() noexcept;

	VulkanDevice* _device = nullptr;
	VkQueryPool _queryPool = VK_NULL_HANDLE;
	uint32_t _passCount = 0;
	uint32_t _curPass = 0;

	std::mutex _timingsMutex;
	SmallVector<float> _timings;
};

}
