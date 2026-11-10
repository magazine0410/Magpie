#include "EffectsProfiler.h"
#include <cassert>
#include "Logger.h"
#include "VulkanDevice.h"

namespace Magpie {

EffectsProfiler::~EffectsProfiler() {
	Stop();
}

bool EffectsProfiler::Start(VulkanDevice& device, uint32_t passCount) noexcept {
	assert(!IsProfiling() && passCount > 0);

	if (device.TimestampPeriod() == 0.0f) {
		Logger::Get().Error("The graphics card has no timestamps");
		return false;
	}

	_device = &device;
	_passCount = passCount;
	return _CreateQueryPool();
}

void EffectsProfiler::Stop() noexcept {
	if (_queryPool) {
		vkDestroyQueryPool(_device->Device(), _queryPool, nullptr);
		_queryPool = VK_NULL_HANDLE;
	}
}

void EffectsProfiler::SetPassCount(uint32_t passCount) noexcept {
	if (!IsProfiling() || passCount == _passCount) {
		return;
	}

	assert(passCount > 0);
	Stop();
	_passCount = passCount;
	_CreateQueryPool();
}

void EffectsProfiler::OnBeginEffects(VkCommandBuffer cmd) noexcept {
	if (!IsProfiling()) {
		return;
	}

	vkCmdResetQueryPool(cmd, _queryPool, 0, _passCount + 1);
	vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, _queryPool, 0);
	_curPass = 0;
}

void EffectsProfiler::OnEndPass(VkCommandBuffer cmd) noexcept {
	if (!IsProfiling()) {
		return;
	}

	assert(_curPass < _passCount);
	vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, _queryPool, ++_curPass);
}

void EffectsProfiler::QueryTimings() noexcept {
	if (!IsProfiling() || _curPass != _passCount) {
		return;
	}

	std::vector<uint64_t> timestamps(_passCount + 1);
	const VkResult result = vkGetQueryPoolResults(
		_device->Device(), _queryPool, 0, _passCount + 1,
		timestamps.size() * sizeof(uint64_t), timestamps.data(), sizeof(uint64_t),
		VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
	if (result != VK_SUCCESS) {
		return;
	}

	const float toMS = _device->TimestampPeriod() / 1e6f;

	const std::scoped_lock lock(_timingsMutex);
	_timings.resize(_passCount);
	for (uint32_t i = 0; i < _passCount; ++i) {
		_timings[i] = (timestamps[i + 1] - timestamps[i]) * toMS;
	}
}

SmallVector<float> EffectsProfiler::GetTimings() noexcept {
	const std::scoped_lock lock(_timingsMutex);

	SmallVector<float> result = std::move(_timings);
	_timings.clear();
	return result;
}

bool EffectsProfiler::_CreateQueryPool() noexcept {
	// One timestamp before the first pass and one after every pass
	const VkQueryPoolCreateInfo poolInfo{
		.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
		.queryType = VK_QUERY_TYPE_TIMESTAMP,
		.queryCount = _passCount + 1
	};
	const VkResult result = vkCreateQueryPool(_device->Device(), &poolInfo, nullptr, &_queryPool);
	if (result != VK_SUCCESS) {
		Logger::Get().Error("vkCreateQueryPool failed");
		_queryPool = VK_NULL_HANDLE;
		return false;
	}

	_curPass = 0;
	return true;
}

}
