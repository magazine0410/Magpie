#include "DuplicateFrameDetector.h"
#include <cstring>
#include <fmt/format.h>
#include "Logger.h"
#include "ShaderCompiler.h"
#include "VulkanDevice.h"

namespace Magpie {

static constexpr uint16_t INITIAL_CHECK_COUNT = 16;
static constexpr uint16_t INITIAL_SKIP_COUNT = 1;
static constexpr uint16_t MAX_SKIP_COUNT = 16;

// src/Magpie.Core/shaders/DuplicateFrameCS.hlsl with Vulkan bindings. The
// result is a storage buffer instead of a typed buffer, which needs no buffer
// view in Vulkan.
static constexpr const char* DUPLICATE_FRAME_CS = R"(
// The threads need no synchronisation: all of them write the same value
[[vk::binding(0, 0)]] RWStructuredBuffer<uint> result;

[[vk::binding(1, 0)]] Texture2D tex1;
[[vk::binding(2, 0)]] Texture2D tex2;

[[vk::binding(3, 0)]] SamplerState sam;

[numthreads(8, 8, 1)]
void main(uint3 tid : SV_GroupThreadID, uint3 gid : SV_GroupID) {
	if (result[0]) {
		return;
	}

	const int2 gxy = (gid.xy << 4) + (tid.xy << 1);

	uint width, height;
	tex1.GetDimensions(width, height);
	const float2 pos = (gxy + 1) / float2(width, height);

	if (any(tex1.GatherRed(sam, pos) != tex2.GatherRed(sam, pos))) {
		result[0] = 1u;
		return;
	}

	if (any(tex1.GatherGreen(sam, pos) != tex2.GatherGreen(sam, pos))) {
		result[0] = 1u;
		return;
	}

	if (any(tex1.GatherBlue(sam, pos) != tex2.GatherBlue(sam, pos))) {
		result[0] = 1u;
	}
}
)";

DuplicateFrameDetector::DuplicateFrameDetector() noexcept :
	_nextSkipCount(INITIAL_SKIP_COUNT), _framesLeft(INITIAL_CHECK_COUNT) {}

DuplicateFrameDetector::~DuplicateFrameDetector() {
	if (!_device) {
		return;
	}

	const VkDevice device = _device->Device();
	if (_pipeline) {
		vkDestroyPipeline(device, _pipeline, nullptr);
	}
	if (_pipelineLayout) {
		vkDestroyPipelineLayout(device, _pipelineLayout, nullptr);
	}
	if (_setLayout) {
		vkDestroyDescriptorSetLayout(device, _setLayout, nullptr);
	}
	if (_descriptorPool) {
		vkDestroyDescriptorPool(device, _descriptorPool, nullptr);
	}
}

void DuplicateFrameDetector::Initialize(
	VulkanDevice& device,
	DuplicateFrameDetectionMode mode,
	bool isStatisticsEnabled
) noexcept {
	_device = &device;
	_mode = mode;
	_isStatisticsEnabled = isStatisticsEnabled;
}

bool DuplicateFrameDetector::OnNewFrame(const Texture& frame) noexcept {
	if (_failed) {
		return false;
	}

	if (!_prevFrame) {
		if (_InitCheckingForDuplicateFrame(frame)) {
			_CopyToPrevFrame(frame);
		} else {
			Logger::Get().Error("_InitCheckingForDuplicateFrame failed");
			_prevFrame = nullptr;
			_failed = true;
		}

		return false;
	}

	if (_mode == DuplicateFrameDetectionMode::Always) {
		// Compare every frame
		if (_IsDuplicateFrame(frame)) {
			return true;
		} else {
			_CopyToPrevFrame(frame);
			return false;
		}
	}

	///////////////////////////////////////////////
	//
	// Dynamic detection, see #787
	//
	///////////////////////////////////////////////

	if (_isCheckingForDuplicateFrame) {
		if (--_framesLeft == 0) {
			_isCheckingForDuplicateFrame = false;
			_framesLeft = _nextSkipCount;
			if (_nextSkipCount < MAX_SKIP_COUNT) {
				// Skip one more frame next time
				++_nextSkipCount;
			}
		}

		if (_IsDuplicateFrame(frame)) {
			_isCheckingForDuplicateFrame = true;
			_framesLeft = INITIAL_CHECK_COUNT;
			_nextSkipCount = INITIAL_SKIP_COUNT;
			return true;
		} else {
			if (_isCheckingForDuplicateFrame || _isStatisticsEnabled) {
				_CopyToPrevFrame(frame);
			}
			return false;
		}
	} else {
		if (--_framesLeft == 0) {
			_isCheckingForDuplicateFrame = true;
			// The second round compares 10 frames in a row, later rounds
			// fewer, and from the 16th round on only 2
			_framesLeft = uint16_t((-4 * (int)_nextSkipCount + 78) / 7);

			if (!_isStatisticsEnabled) {
				// The next frame is compared with this one
				_CopyToPrevFrame(frame);
			}
		}

		if (_isStatisticsEnabled) {
			const bool isDuplicate = _IsDuplicateFrame(frame);
			if (!isDuplicate) {
				_CopyToPrevFrame(frame);
			}

			if (isDuplicate) {
				// Wrongly taken as new
				++_statistics.first;
			}
			// Frames not compared
			++_statistics.second;
		}

		return false;
	}
}

bool DuplicateFrameDetector::_InitCheckingForDuplicateFrame(const Texture& frame) noexcept {
	const VkDevice device = _device->Device();

	// Kept in the general layout, which allows both copying to it and sampling
	_prevFrame = _device->CreateTexture(frame.format, frame.width, frame.height,
		VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
	if (!_prevFrame || !_device->SetInitialLayout(*_prevFrame, VK_IMAGE_LAYOUT_GENERAL)) {
		return false;
	}

	_resultBuffer = _device->CreateBuffer(4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
	if (!_resultBuffer) {
		return false;
	}

	std::vector<uint32_t> spirv;
	std::string messages;
	if (!ShaderCompiler::CompileComputeShader(DUPLICATE_FRAME_CS, "main", "DuplicateFrameCS", {}, spirv, messages)) {
		Logger::Get().Error(fmt::format("Compiling DuplicateFrameCS failed:\n{}", messages));
		return false;
	}

	const VkDescriptorSetLayoutBinding bindings[]{
		{ 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr },
		{ 1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr },
		{ 2, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr },
		{ 3, VK_DESCRIPTOR_TYPE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr }
	};
	const VkDescriptorSetLayoutCreateInfo setLayoutInfo{
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
		.bindingCount = (uint32_t)std::size(bindings),
		.pBindings = bindings
	};
	const VkPipelineLayoutCreateInfo pipelineLayoutInfo{
		.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
		.setLayoutCount = 1,
		.pSetLayouts = &_setLayout
	};
	if (vkCreateDescriptorSetLayout(device, &setLayoutInfo, nullptr, &_setLayout) != VK_SUCCESS ||
		vkCreatePipelineLayout(device, &pipelineLayoutInfo, nullptr, &_pipelineLayout) != VK_SUCCESS) {
		Logger::Get().Error("Creating the pipeline layout failed");
		return false;
	}

	const VkShaderModuleCreateInfo moduleInfo{
		.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
		.codeSize = spirv.size() * sizeof(uint32_t),
		.pCode = spirv.data()
	};
	VkShaderModule module = VK_NULL_HANDLE;
	if (vkCreateShaderModule(device, &moduleInfo, nullptr, &module) != VK_SUCCESS) {
		Logger::Get().Error("vkCreateShaderModule failed");
		return false;
	}

	const VkComputePipelineCreateInfo pipelineInfo{
		.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
		.stage = {
			.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
			.stage = VK_SHADER_STAGE_COMPUTE_BIT,
			.module = module,
			.pName = "main"
		},
		.layout = _pipelineLayout
	};
	const VkResult result = vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &_pipeline);
	vkDestroyShaderModule(device, module, nullptr);
	if (result != VK_SUCCESS) {
		Logger::Get().Error("vkCreateComputePipelines failed");
		return false;
	}

	const VkDescriptorPoolSize poolSizes[]{
		{ VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1 },
		{ VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 2 },
		{ VK_DESCRIPTOR_TYPE_SAMPLER, 1 }
	};
	const VkDescriptorPoolCreateInfo poolInfo{
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
		.maxSets = 1,
		.poolSizeCount = (uint32_t)std::size(poolSizes),
		.pPoolSizes = poolSizes
	};
	if (vkCreateDescriptorPool(device, &poolInfo, nullptr, &_descriptorPool) != VK_SUCCESS) {
		Logger::Get().Error("vkCreateDescriptorPool failed");
		return false;
	}

	const VkDescriptorSetAllocateInfo setInfo{
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
		.descriptorPool = _descriptorPool,
		.descriptorSetCount = 1,
		.pSetLayouts = &_setLayout
	};
	if (vkAllocateDescriptorSets(device, &setInfo, &_descriptorSet) != VK_SUCCESS) {
		Logger::Get().Error("vkAllocateDescriptorSets failed");
		return false;
	}

	const VkSampler sampler = _device->GetSampler(VK_FILTER_NEAREST, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE);
	if (!sampler) {
		return false;
	}

	// The frame is bound before each comparison; the rest never changes
	const VkDescriptorBufferInfo bufferInfo{ _resultBuffer->buffer, 0, VK_WHOLE_SIZE };
	const VkDescriptorImageInfo prevFrameInfo{ VK_NULL_HANDLE, _prevFrame->view, VK_IMAGE_LAYOUT_GENERAL };
	const VkDescriptorImageInfo samplerInfo{ sampler, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED };
	const VkWriteDescriptorSet writes[]{
		{
			.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
			.dstSet = _descriptorSet,
			.dstBinding = 0,
			.descriptorCount = 1,
			.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
			.pBufferInfo = &bufferInfo
		},
		{
			.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
			.dstSet = _descriptorSet,
			.dstBinding = 2,
			.descriptorCount = 1,
			.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
			.pImageInfo = &prevFrameInfo
		},
		{
			.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
			.dstSet = _descriptorSet,
			.dstBinding = 3,
			.descriptorCount = 1,
			.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER,
			.pImageInfo = &samplerInfo
		}
	};
	vkUpdateDescriptorSets(device, (uint32_t)std::size(writes), writes, 0, nullptr);

	static constexpr std::pair<uint32_t, uint32_t> BLOCK_SIZE{ 16, 16 };
	_dispatchCount.first = (frame.width + BLOCK_SIZE.first - 1) / BLOCK_SIZE.first;
	_dispatchCount.second = (frame.height + BLOCK_SIZE.second - 1) / BLOCK_SIZE.second;

	return true;
}

bool DuplicateFrameDetector::_IsDuplicateFrame(const Texture& frame) noexcept {
	if (frame.view != _boundFrameView) {
		const VkDescriptorImageInfo frameInfo{ VK_NULL_HANDLE, frame.view, frame.layout };
		const VkWriteDescriptorSet write{
			.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
			.dstSet = _descriptorSet,
			.dstBinding = 1,
			.descriptorCount = 1,
			.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
			.pImageInfo = &frameInfo
		};
		vkUpdateDescriptorSets(_device->Device(), 1, &write, 0, nullptr);
		_boundFrameView = frame.view;
	}

	// The GPU is idle, so the buffer can be cleared from here
	uint32_t result = 0;
	std::memcpy(_resultBuffer->data, &result, 4);

	const bool executed = _device->Execute([&](VkCommandBuffer cmd) {
		vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, _pipeline);
		vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, _pipelineLayout, 0, 1, &_descriptorSet, 0, nullptr);
		vkCmdDispatch(cmd, _dispatchCount.first, _dispatchCount.second, 1);
	});
	if (!executed) {
		// As on Windows: a frame that cannot be compared counts as new
		return false;
	}

	std::memcpy(&result, _resultBuffer->data, 4);
	return result == 0;
}

void DuplicateFrameDetector::_CopyToPrevFrame(const Texture& frame) noexcept {
	// An image can be copied from in these two layouts
	const bool needsTransition = frame.layout != VK_IMAGE_LAYOUT_GENERAL &&
		frame.layout != VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
	const VkImageLayout copyLayout = needsTransition ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL : frame.layout;

	_device->Execute([&](VkCommandBuffer cmd) {
		if (needsTransition) {
			TransitionImage(cmd, frame.image, frame.layout, copyLayout);
		}

		const VkImageCopy region{
			.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
			.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
			.extent = { frame.width, frame.height, 1 }
		};
		vkCmdCopyImage(cmd, frame.image, copyLayout, _prevFrame->image, VK_IMAGE_LAYOUT_GENERAL, 1, &region);

		if (needsTransition) {
			TransitionImage(cmd, frame.image, copyLayout, frame.layout);
		}
	});
}

}
