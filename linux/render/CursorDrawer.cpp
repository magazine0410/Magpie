#include "CursorDrawer.h"
#include <algorithm>
#include <cmath>
#include <vector>
#include <fmt/format.h>
#include "CommonDefines.h"
#include "Logger.h"
#include "ShaderCompiler.h"
#include "VulkanDevice.h"

namespace Magpie {

// The rectangle comes as a push constant instead of the vertex buffer that
// src/Magpie.Core/shaders/SimpleVS.hlsl reads, which saves a buffer that would
// be rewritten for every frame. The four vertices form a triangle strip.
static constexpr const char* CURSOR_VS = R"(
struct Constants {
	// Left, top, right and bottom in clip coordinates
	float4 rect;
};
[[vk::push_constant]] Constants constants;

void main(
	uint id : SV_VertexID,
	out noperspective float2 outCoord : TEXCOORD,
	out noperspective float4 outPos : SV_POSITION
) {
	outCoord = float2(id & 1, id >> 1);
	outPos = float4(lerp(constants.rect.xy, constants.rect.zw, outCoord), 0, 1);
}
)";

// src/Magpie.Core/shaders/SimplePS.hlsl with Vulkan bindings
static constexpr const char* CURSOR_PS = R"(
[[vk::binding(0, 0)]] Texture2D tex;
[[vk::binding(1, 0)]] SamplerState sam;

float4 main(noperspective float2 coord : TEXCOORD) : SV_Target {
	return tex.Sample(sam, coord);
}
)";

CursorDrawer::~CursorDrawer() {
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

static VkShaderModule CreateShaderModule(
	VkDevice device,
	ShaderCompiler::Stage stage,
	const char* hlsl,
	const char* name
) noexcept {
	std::vector<uint32_t> spirv;
	std::string messages;
	if (!ShaderCompiler::CompileShader(stage, hlsl, "main", name, {}, spirv, messages)) {
		Logger::Get().Error(fmt::format("Compiling {} failed:\n{}", name, messages));
		return VK_NULL_HANDLE;
	}

	const VkShaderModuleCreateInfo moduleInfo{
		.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
		.codeSize = spirv.size() * sizeof(uint32_t),
		.pCode = spirv.data()
	};
	VkShaderModule module = VK_NULL_HANDLE;
	if (vkCreateShaderModule(device, &moduleInfo, nullptr, &module) != VK_SUCCESS) {
		Logger::Get().Error("vkCreateShaderModule failed");
		return VK_NULL_HANDLE;
	}
	return module;
}

bool CursorDrawer::Initialize(VulkanDevice& vulkanDevice, VkFormat targetFormat) noexcept {
	_device = &vulkanDevice;
	const VkDevice device = vulkanDevice.Device();

	const VkDescriptorSetLayoutBinding bindings[]{
		{ 0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr },
		{ 1, VK_DESCRIPTOR_TYPE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr }
	};
	const VkDescriptorSetLayoutCreateInfo setLayoutInfo{
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
		.bindingCount = (uint32_t)std::size(bindings),
		.pBindings = bindings
	};
	if (vkCreateDescriptorSetLayout(device, &setLayoutInfo, nullptr, &_setLayout) != VK_SUCCESS) {
		Logger::Get().Error("vkCreateDescriptorSetLayout failed");
		return false;
	}

	const VkPushConstantRange pushConstantRange{ VK_SHADER_STAGE_VERTEX_BIT, 0, 4 * sizeof(float) };
	const VkPipelineLayoutCreateInfo pipelineLayoutInfo{
		.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
		.setLayoutCount = 1,
		.pSetLayouts = &_setLayout,
		.pushConstantRangeCount = 1,
		.pPushConstantRanges = &pushConstantRange
	};
	if (vkCreatePipelineLayout(device, &pipelineLayoutInfo, nullptr, &_pipelineLayout) != VK_SUCCESS) {
		Logger::Get().Error("vkCreatePipelineLayout failed");
		return false;
	}

	const VkShaderModule vertexShader = CreateShaderModule(device, ShaderCompiler::Stage::Vertex, CURSOR_VS, "CursorVS");
	const VkShaderModule pixelShader = CreateShaderModule(device, ShaderCompiler::Stage::Pixel, CURSOR_PS, "CursorPS");

	const VkPipelineShaderStageCreateInfo stages[]{
		{
			.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
			.stage = VK_SHADER_STAGE_VERTEX_BIT,
			.module = vertexShader,
			.pName = "main"
		},
		{
			.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
			.stage = VK_SHADER_STAGE_FRAGMENT_BIT,
			.module = pixelShader,
			.pName = "main"
		}
	};
	const VkPipelineVertexInputStateCreateInfo vertexInput{
		.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO
	};
	const VkPipelineInputAssemblyStateCreateInfo inputAssembly{
		.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
		.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP
	};
	// Viewport and scissor are set for every draw
	const VkPipelineViewportStateCreateInfo viewport{
		.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
		.viewportCount = 1,
		.scissorCount = 1
	};
	const VkPipelineRasterizationStateCreateInfo rasterization{
		.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
		.polygonMode = VK_POLYGON_MODE_FILL,
		.cullMode = VK_CULL_MODE_NONE,
		.lineWidth = 1.0f
	};
	const VkPipelineMultisampleStateCreateInfo multisample{
		.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
		.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT
	};
	// FinalColor = ScreenColor * CursorColor.a + CursorColor, as
	// CursorDrawer::_SetPremultipliedAlphaBlend sets it up on Windows
	const VkPipelineColorBlendAttachmentState blendAttachment{
		.blendEnable = VK_TRUE,
		.srcColorBlendFactor = VK_BLEND_FACTOR_ONE,
		.dstColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA,
		.colorBlendOp = VK_BLEND_OP_ADD,
		.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
		.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO,
		.alphaBlendOp = VK_BLEND_OP_ADD,
		.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
			VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT
	};
	const VkPipelineColorBlendStateCreateInfo blend{
		.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
		.attachmentCount = 1,
		.pAttachments = &blendAttachment
	};
	const VkDynamicState dynamicStates[]{ VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
	const VkPipelineDynamicStateCreateInfo dynamic{
		.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
		.dynamicStateCount = (uint32_t)std::size(dynamicStates),
		.pDynamicStates = dynamicStates
	};
	// Drawn with dynamic rendering, so the pipeline names the format of its
	// target in place of a render pass
	const VkPipelineRenderingCreateInfo rendering{
		.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
		.colorAttachmentCount = 1,
		.pColorAttachmentFormats = &targetFormat
	};
	const VkGraphicsPipelineCreateInfo pipelineInfo{
		.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
		.pNext = &rendering,
		.stageCount = (uint32_t)std::size(stages),
		.pStages = stages,
		.pVertexInputState = &vertexInput,
		.pInputAssemblyState = &inputAssembly,
		.pViewportState = &viewport,
		.pRasterizationState = &rasterization,
		.pMultisampleState = &multisample,
		.pColorBlendState = &blend,
		.pDynamicState = &dynamic,
		.layout = _pipelineLayout
	};
	const VkResult result = vertexShader && pixelShader
		? vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &_pipeline)
		: VK_ERROR_UNKNOWN;
	if (vertexShader) {
		vkDestroyShaderModule(device, vertexShader, nullptr);
	}
	if (pixelShader) {
		vkDestroyShaderModule(device, pixelShader, nullptr);
	}
	if (result != VK_SUCCESS) {
		Logger::Get().Error("Creating the pipeline of the cursor failed");
		return false;
	}

	const VkDescriptorPoolSize poolSizes[]{
		{ VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 2 },
		{ VK_DESCRIPTOR_TYPE_SAMPLER, 2 }
	};
	const VkDescriptorPoolCreateInfo poolInfo{
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
		.maxSets = 2,
		.poolSizeCount = (uint32_t)std::size(poolSizes),
		.pPoolSizes = poolSizes
	};
	if (vkCreateDescriptorPool(device, &poolInfo, nullptr, &_descriptorPool) != VK_SUCCESS) {
		Logger::Get().Error("vkCreateDescriptorPool failed");
		return false;
	}

	const VkDescriptorSetLayout setLayouts[]{ _setLayout, _setLayout };
	const VkDescriptorSetAllocateInfo setInfo{
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
		.descriptorPool = _descriptorPool,
		.descriptorSetCount = 2,
		.pSetLayouts = setLayouts
	};
	if (vkAllocateDescriptorSets(device, &setInfo, _descriptorSets) != VK_SUCCESS) {
		Logger::Get().Error("vkAllocateDescriptorSets failed");
		return false;
	}

	return true;
}

bool CursorDrawer::SetImage(std::span<const uint8_t> pixels, Size size, Point hotSpot) noexcept {
	_texture = nullptr;
	_imageSize = {};
	_hotSpot = {};

	if (size.width <= 0 || size.height <= 0) {
		return true;
	}
	if (pixels.size() != (size_t)size.width * size.height * 4) {
		Logger::Get().Error("The cursor image does not have the size it claims");
		return false;
	}

	// The colours are multiplied by alpha already; alpha is inverted so that
	// the shader need not do it
	std::vector<uint8_t> converted(pixels.begin(), pixels.end());
	for (size_t i = 3; i < converted.size(); i += 4) {
		converted[i] = uint8_t(255 - converted[i]);
	}

	std::shared_ptr<Texture> texture = _device->CreateTexture(VK_FORMAT_R8G8B8A8_UNORM,
		(uint32_t)size.width, (uint32_t)size.height, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
	if (!texture || !_device->UploadTexture(*texture, converted, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)) {
		return false;
	}

	static constexpr VkFilter FILTERS[2]{ VK_FILTER_NEAREST, VK_FILTER_LINEAR };
	const VkDescriptorImageInfo imageInfo{ VK_NULL_HANDLE, texture->view, texture->layout };
	VkDescriptorImageInfo samplerInfos[2];
	VkWriteDescriptorSet writes[4];
	for (uint32_t i = 0; i < 2; ++i) {
		samplerInfos[i] = { _device->GetSampler(FILTERS[i], VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE),
			VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED };
		if (!samplerInfos[i].sampler) {
			return false;
		}

		writes[i * 2] = {
			.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
			.dstSet = _descriptorSets[i],
			.dstBinding = 0,
			.descriptorCount = 1,
			.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
			.pImageInfo = &imageInfo
		};
		writes[i * 2 + 1] = {
			.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
			.dstSet = _descriptorSets[i],
			.dstBinding = 1,
			.descriptorCount = 1,
			.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER,
			.pImageInfo = &samplerInfos[i]
		};
	}
	vkUpdateDescriptorSets(_device->Device(), 4, writes, 0, nullptr);

	_texture = std::move(texture);
	_imageSize = size;
	_hotSpot = hotSpot;
	return true;
}

float CursorDrawer::CalcScaleFactor(float optionValue, Size srcSize, const Rect& destRect) noexcept {
	if (optionValue >= FLOAT_EPSILON<float>) {
		return optionValue;
	}

	// The cursor is scaled like the source
	return (((float)destRect.Width() / srcSize.width) + ((float)destRect.Height() / srcSize.height)) / 2;
}

Rect CursorDrawer::CalcCursorRect(Point cursorPos, Size imageSize, Point hotSpot, float scaleFactor) noexcept {
	Rect cursorRect;
	cursorRect.left = (int32_t)std::lround(cursorPos.x - hotSpot.x * scaleFactor);
	cursorRect.top = (int32_t)std::lround(cursorPos.y - hotSpot.y * scaleFactor);
	cursorRect.right = cursorRect.left + (int32_t)std::lround(imageSize.width * scaleFactor);
	cursorRect.bottom = cursorRect.top + (int32_t)std::lround(imageSize.height * scaleFactor);
	return cursorRect;
}

void CursorDrawer::Draw(
	VkCommandBuffer cmd,
	const Target& target,
	Point cursorPos,
	Size srcSize,
	const Rect& destRect,
	const ScalingOptions& options
) const noexcept {
	if (!_texture) {
		return;
	}

	const float cursorScaleFactor = CalcScaleFactor(options.cursorScaleFactor, srcSize, destRect);
	const Rect cursorRect = CalcCursorRect(cursorPos, _imageSize, _hotSpot, cursorScaleFactor);

	// The viewport is the destination rectangle, which cuts the cursor off at
	// the edges of the picture
	const Rect& viewportRect = destRect;
	if (cursorRect.left >= viewportRect.right ||
		cursorRect.top >= viewportRect.bottom ||
		cursorRect.right <= viewportRect.left ||
		cursorRect.bottom <= viewportRect.top
	) {
		return;
	}

	// The part of the viewport inside the target; nothing may be drawn
	// outside of an image
	const Rect visibleRect{
		std::max(viewportRect.left, 0), std::max(viewportRect.top, 0),
		std::min(viewportRect.right, target.size.width), std::min(viewportRect.bottom, target.size.height)
	};
	if (visibleRect.Width() <= 0 || visibleRect.Height() <= 0) {
		return;
	}
	const VkRect2D scissor{
		{ visibleRect.left, visibleRect.top },
		{ (uint32_t)visibleRect.Width(), (uint32_t)visibleRect.Height() }
	};

	// In clip coordinates of the viewport. Vulkan's y axis points down.
	const float viewportWidth = (float)viewportRect.Width();
	const float viewportHeight = (float)viewportRect.Height();
	float rect[4];
	rect[0] = (cursorRect.left - viewportRect.left) / viewportWidth * 2 - 1.0f;
	rect[1] = (cursorRect.top - viewportRect.top) / viewportHeight * 2 - 1.0f;
	rect[2] = rect[0] + cursorRect.Width() / viewportWidth * 2;
	rect[3] = rect[1] + cursorRect.Height() / viewportHeight * 2;

	const bool useBilinear = options.cursorInterpolationMode == CursorInterpolationMode::Bilinear &&
		std::abs(options.cursorScaleFactor - 1.0f) > 1e-3;

	TransitionImage(cmd, target.image, target.layout, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);

	const VkRenderingAttachmentInfo attachment{
		.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
		.imageView = target.view,
		.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
		.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
		.storeOp = VK_ATTACHMENT_STORE_OP_STORE
	};
	const VkRenderingInfo renderingInfo{
		.sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
		.renderArea = scissor,
		.layerCount = 1,
		.colorAttachmentCount = 1,
		.pColorAttachments = &attachment
	};
	vkCmdBeginRendering(cmd, &renderingInfo);

	const VkViewport viewport{
		(float)viewportRect.left, (float)viewportRect.top, viewportWidth, viewportHeight, 0.0f, 1.0f
	};
	vkCmdSetViewport(cmd, 0, 1, &viewport);
	vkCmdSetScissor(cmd, 0, 1, &scissor);
	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, _pipeline);
	vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, _pipelineLayout, 0, 1,
		&_descriptorSets[useBilinear ? 1 : 0], 0, nullptr);
	vkCmdPushConstants(cmd, _pipelineLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(rect), rect);
	vkCmdDraw(cmd, 4, 1, 0, 0);

	vkCmdEndRendering(cmd);

	TransitionImage(cmd, target.image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, target.layout);
}

}
