#include "EffectDrawer.h"
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstring>
#include <fmt/format.h>
#include "EffectHelper.h"
#include "EffectsProfiler.h"
#include "ImageFile.h"
#include "Logger.h"
#include "ScalingOptions.h"
#include "StrHelper.h"
#include "VulkanDevice.h"

namespace Magpie {

// Layouts and synchronisation: every texture an effect writes stays in
// VK_IMAGE_LAYOUT_GENERAL, which allows both sampling and writing, so a pass
// never changes a layout. A memory barrier after each dispatch makes the
// written pixels visible to the following passes.

static Size SizeOf(const Texture& texture) noexcept {
	return { (int32_t)texture.width, (int32_t)texture.height };
}

EffectDrawer::~EffectDrawer() {
	if (!_device) {
		return;
	}

	const VkDevice device = _device->Device();
	for (const Pass& pass : _passes) {
		if (pass.pipeline) {
			vkDestroyPipeline(device, pass.pipeline, nullptr);
		}
		if (pass.pipelineLayout) {
			vkDestroyPipelineLayout(device, pass.pipelineLayout, nullptr);
		}
		if (pass.setLayout) {
			vkDestroyDescriptorSetLayout(device, pass.setLayout, nullptr);
		}
	}
	if (_descriptorPool) {
		// Frees the descriptor sets as well
		vkDestroyDescriptorPool(device, _descriptorPool, nullptr);
	}
}

bool EffectDrawer::Initialize(
	const EffectDesc& desc,
	const EffectOption& option,
	const Environment& env,
	std::shared_ptr<Texture>& inOutTexture
) noexcept {
	_device = env.device;

	const Size inputSize = SizeOf(*inOutTexture);
	const Size outputSize = _CalcOutputSize(desc, option, env, inputSize);
	if (outputSize.width <= 0 || outputSize.height <= 0) {
		Logger::Get().Error("Invalid output size");
		return false;
	}

	_samplers.resize(desc.samplers.size());
	for (uint32_t i = 0; i < _samplers.size(); ++i) {
		const EffectSamplerDesc& samDesc = desc.samplers[i];
		_samplers[i] = _device->GetSampler(
			samDesc.filterType == EffectSamplerFilterType::Linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST,
			samDesc.addressType == EffectSamplerAddressType::Clamp
				? VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE : VK_SAMPLER_ADDRESS_MODE_REPEAT
		);

		if (!_samplers[i]) {
			Logger::Get().Error(fmt::format("Creating sampler {} failed", samDesc.name));
			return false;
		}
	}

	// The first texture is INPUT, the second is OUTPUT
	_textures.resize(desc.textures.size());
	_textures[0] = inOutTexture;

	// The format of the output is always R8G8B8A8_UNORM
	_textures[1] = _CreateEffectTexture(desc.textures[1].format, outputSize);
	if (!_textures[1]) {
		Logger::Get().Error("Creating the output texture failed");
		return false;
	}
	inOutTexture = _textures[1];

	for (size_t i = 2; i < desc.textures.size(); ++i) {
		const EffectIntermediateTextureDesc& texDesc = desc.textures[i];

		if (!texDesc.source.empty()) {
			_textures[i] = _LoadSourceTexture(desc, texDesc, env);
			if (!_textures[i]) {
				Logger::Get().Error(fmt::format("Loading texture {} failed", texDesc.source));
				return false;
			}
		} else {
			const Size texSize = _CalcTextureSize(texDesc.sizeExpr);
			if (texSize.width <= 0 || texSize.height <= 0) {
				Logger::Get().Error(fmt::format("Invalid size of texture {}", texDesc.name));
				return false;
			}

			_textures[i] = _CreateEffectTexture(texDesc.format, texSize);
			if (!_textures[i]) {
				Logger::Get().Error(fmt::format("Creating texture {} failed", texDesc.name));
				return false;
			}
		}
	}

	if (!_CreatePasses(desc)) {
		Logger::Get().Error("_CreatePasses failed");
		return false;
	}

	// The descriptors refer to the constant buffer, so it comes first
	if (!_UpdateConstants(desc, option, inputSize, outputSize)) {
		Logger::Get().Error("_UpdateConstants failed");
		return false;
	}

	if (!_UpdatePassResources(desc, env)) {
		Logger::Get().Error("_UpdatePassResources failed");
		return false;
	}

	return true;
}

void EffectDrawer::Draw(VkCommandBuffer cmd, EffectsProfiler& profiler) const noexcept {
	static constexpr VkMemoryBarrier BARRIER{
		.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
		.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
		.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT
	};

	for (const Pass& pass : _passes) {
		vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pass.pipeline);
		vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
			pass.pipelineLayout, 0, 1, &pass.descriptorSet, 0, nullptr);
		vkCmdDispatch(cmd, pass.groupCountX, pass.groupCountY, 1);

		// The next reader of the outputs is a later pass or a copy
		vkCmdPipelineBarrier(cmd,
			VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
			VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
			0, 1, &BARRIER, 0, nullptr, 0, nullptr);

		profiler.OnEndPass(cmd);
	}
}

bool EffectDrawer::ResizeTextures(
	const EffectDesc& desc,
	const EffectOption& option,
	const Environment& env,
	std::shared_ptr<Texture>& inOutTexture
) noexcept {
	bool anyChange = false;

	if (inOutTexture != _textures[0]) {
		_textures[0] = inOutTexture;
		anyChange = true;
	}

	const Size inputSize = SizeOf(*_textures[0]);
	const Size outputSize = _CalcOutputSize(desc, option, env, inputSize);
	if (outputSize.width <= 0 || outputSize.height <= 0) {
		Logger::Get().Error("Invalid output size");
		return false;
	}

	if (SizeOf(*_textures[1]) != outputSize) {
		_textures[1] = _CreateEffectTexture(desc.textures[1].format, outputSize);
		if (!_textures[1]) {
			Logger::Get().Error("Creating the output texture failed");
			return false;
		}

		anyChange = true;
	}

	inOutTexture = _textures[1];

	for (size_t i = 2; i < _textures.size(); ++i) {
		const EffectIntermediateTextureDesc& texDesc = desc.textures[i];
		if (texDesc.sizeExpr.first.empty()) {
			// Textures loaded from files keep their size
			continue;
		}

		const Size texSize = _CalcTextureSize(texDesc.sizeExpr);
		if (texSize.width <= 0 || texSize.height <= 0) {
			Logger::Get().Error(fmt::format("Invalid size of texture {}", texDesc.name));
			return false;
		}

		if (SizeOf(*_textures[i]) != texSize) {
			_textures[i] = _CreateEffectTexture(texDesc.format, texSize);
			if (!_textures[i]) {
				Logger::Get().Error(fmt::format("Creating texture {} failed", texDesc.name));
				return false;
			}

			anyChange = true;
		}
	}

	if (!anyChange) {
		return true;
	}

	if (!_UpdateConstants(desc, option, inputSize, outputSize)) {
		Logger::Get().Error("_UpdateConstants failed");
		return false;
	}

	if (!_UpdatePassResources(desc, env)) {
		Logger::Get().Error("_UpdatePassResources failed");
		return false;
	}

	return true;
}

Size EffectDrawer::_CalcOutputSize(
	const EffectDesc& desc,
	const EffectOption& option,
	const Environment& env,
	Size inputSize
) const noexcept {
	_exprParser.DefineConst("INPUT_WIDTH", inputSize.width);
	_exprParser.DefineConst("INPUT_HEIGHT", inputSize.height);

	Size outputSize;
	const std::pair<std::string, std::string>& outputSizeExpr = desc.GetOutputSizeExpr();

	if (outputSizeExpr.first.empty()) {
		const Size rendererSize = env.rendererSize;

		switch (option.scalingType) {
		case ScalingType::Normal:
		{
			outputSize.width = (int32_t)std::lround(inputSize.width * option.scale.first);
			outputSize.height = (int32_t)std::lround(inputSize.height * option.scale.second);
			break;
		}
		case ScalingType::Absolute:
		{
			outputSize.width = (int32_t)std::lround(option.scale.first);
			outputSize.height = (int32_t)std::lround(option.scale.second);
			break;
		}
		case ScalingType::Fit:
		{
			// In windowed mode a Fit with a factor of 1 is treated as Fill. The
			// scaling is known to keep the aspect ratio there, but rounding can
			// be off by one pixel. Take a window of 100 by 50: widening it to 101
			// rounds the height to 51, and widening it to 102 keeps 51. Fit
			// would add a black bar in one of the two steps, and that would also
			// decide whether Bicubic is appended.
			const bool treatFitAsFill = env.isWindowedMode &&
				IsApprox(option.scale.first, 1.0f) && IsApprox(option.scale.second, 1.0f);

			if (!treatFitAsFill) {
				const float fillScale = std::min(
					float(rendererSize.width) / inputSize.width,
					float(rendererSize.height) / inputSize.height
				);
				outputSize.width = (int32_t)std::lround(inputSize.width * fillScale * option.scale.first);
				outputSize.height = (int32_t)std::lround(inputSize.height * fillScale * option.scale.second);
				break;
			}
			[[fallthrough]];
		}
		case ScalingType::Fill:
		{
			outputSize = rendererSize;
			break;
		}
		default:
			assert(false);
			return {};
		}
	} else {
		assert(!outputSizeExpr.second.empty());

		outputSize = _CalcTextureSize(outputSizeExpr);
		if (outputSize == Size{}) {
			return {};
		}
	}

	_exprParser.DefineConst("OUTPUT_WIDTH", outputSize.width);
	_exprParser.DefineConst("OUTPUT_HEIGHT", outputSize.height);

	return outputSize;
}

Size EffectDrawer::_CalcTextureSize(const std::pair<std::string, std::string>& sizeExpr) const noexcept {
	try {
		Size size;
		_exprParser.SetExpr(sizeExpr.first);
		size.width = (int32_t)std::lround(_exprParser.Eval());
		_exprParser.SetExpr(sizeExpr.second);
		size.height = (int32_t)std::lround(_exprParser.Eval());
		return size;
	} catch (const mu::ParserError& e) {
		Logger::Get().Error(fmt::format("Evaluating the size expression {} failed: {}", e.GetExpr(), e.GetMsg()));
		return {};
	}
}

std::shared_ptr<Texture> EffectDrawer::_CreateEffectTexture(
	EffectIntermediateTextureFormat format,
	Size size
) const noexcept {
	const auto& formatDesc = EffectHelper::FORMAT_DESCS[(uint32_t)format];
	if (!_device->IsFormatSupported(format)) {
		Logger::Get().Error(fmt::format("The graphics card does not support {} for effect textures", formatDesc.name));
		return nullptr;
	}

	std::shared_ptr<Texture> texture = _device->CreateTexture(
		formatDesc.vkFormat, (uint32_t)size.width, (uint32_t)size.height, VulkanDevice::EFFECT_TEXTURE_USAGE);
	if (!texture || !_device->SetInitialLayout(*texture, VK_IMAGE_LAYOUT_GENERAL)) {
		return nullptr;
	}

	return texture;
}

std::shared_ptr<Texture> EffectDrawer::_LoadSourceTexture(
	const EffectDesc& desc,
	const EffectIntermediateTextureDesc& texDesc,
	const Environment& env
) const noexcept {
	// The file is next to the effect. Effect names use the Windows separator.
	std::string relativePath;
	if (const size_t delimPos = desc.name.find_last_of('\\'); delimPos != std::string::npos) {
		relativePath = desc.name.substr(0, delimPos + 1);
	}
	relativePath += texDesc.source;
	std::replace(relativePath.begin(), relativePath.end(), '\\', '/');

	ImageData image;
	if (!ImageFile::Load(env.effectsDir / relativePath, image)) {
		return nullptr;
	}

	if (texDesc.format != EffectIntermediateTextureFormat::UNKNOWN &&
		image.format != EffectHelper::FORMAT_DESCS[(uint32_t)texDesc.format].vkFormat) {
		Logger::Get().Error("The format of the SOURCE texture does not match its FORMAT");
		return nullptr;
	}

	std::shared_ptr<Texture> texture = _device->CreateTexture(image.format, image.width, image.height,
		VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
	if (!texture || !_device->UploadTexture(*texture, image.pixels, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)) {
		return nullptr;
	}

	return texture;
}

bool EffectDrawer::_CreatePasses(const EffectDesc& desc) noexcept {
	const VkDevice device = _device->Device();
	const uint32_t passCount = (uint32_t)desc.passes.size();
	const uint32_t samplerCount = (uint32_t)desc.samplers.size();
	const bool useDynamic = desc.flags & EffectFlags::UseDynamic;

	{
		uint32_t sampledCount = 0;
		uint32_t storageCount = 0;
		for (const EffectPassDesc& passDesc : desc.passes) {
			sampledCount += (uint32_t)passDesc.inputs.size();
			storageCount += (uint32_t)passDesc.outputs.size();
		}

		SmallVector<VkDescriptorPoolSize, 4> poolSizes;
		poolSizes.push_back({ VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, passCount * (useDynamic ? 2 : 1) });
		if (sampledCount > 0) {
			poolSizes.push_back({ VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, sampledCount });
		}
		if (storageCount > 0) {
			poolSizes.push_back({ VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, storageCount });
		}
		if (samplerCount > 0) {
			poolSizes.push_back({ VK_DESCRIPTOR_TYPE_SAMPLER, passCount * samplerCount });
		}

		const VkDescriptorPoolCreateInfo poolInfo{
			.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
			.maxSets = passCount,
			.poolSizeCount = (uint32_t)poolSizes.size(),
			.pPoolSizes = poolSizes.data()
		};
		if (vkCreateDescriptorPool(device, &poolInfo, nullptr, &_descriptorPool) != VK_SUCCESS) {
			Logger::Get().Error("vkCreateDescriptorPool failed");
			return false;
		}
	}

	_passes.resize(passCount);

	for (uint32_t i = 0; i < passCount; ++i) {
		const EffectPassDesc& passDesc = desc.passes[i];
		Pass& pass = _passes[i];

		if (passDesc.spirv.empty()) {
			Logger::Get().Error(fmt::format("Pass {} is not compiled", i + 1));
			return false;
		}

		const uint32_t inputCount = (uint32_t)passDesc.inputs.size();
		const uint32_t outputCount = (uint32_t)passDesc.outputs.size();

		// The numbering is defined in EffectHelper.h
		SmallVector<VkDescriptorSetLayoutBinding, 16> bindings;
		const auto addBinding = [&](uint32_t binding, VkDescriptorType type) {
			bindings.push_back(VkDescriptorSetLayoutBinding{
				.binding = binding,
				.descriptorType = type,
				.descriptorCount = 1,
				.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT
			});
		};

		addBinding(EffectHelper::BINDING_CB1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
		if (useDynamic) {
			addBinding(EffectHelper::BINDING_CB2, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
		}
		for (uint32_t j = 0; j < inputCount; ++j) {
			addBinding(EffectHelper::InputBinding(j), VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE);
		}
		for (uint32_t j = 0; j < outputCount; ++j) {
			addBinding(EffectHelper::OutputBinding(inputCount, j), VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
		}
		for (uint32_t j = 0; j < samplerCount; ++j) {
			addBinding(EffectHelper::SamplerBinding(inputCount, outputCount, j), VK_DESCRIPTOR_TYPE_SAMPLER);
		}

		const VkDescriptorSetLayoutCreateInfo setLayoutInfo{
			.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
			.bindingCount = (uint32_t)bindings.size(),
			.pBindings = bindings.data()
		};
		if (vkCreateDescriptorSetLayout(device, &setLayoutInfo, nullptr, &pass.setLayout) != VK_SUCCESS) {
			Logger::Get().Error("vkCreateDescriptorSetLayout failed");
			return false;
		}

		const VkPipelineLayoutCreateInfo pipelineLayoutInfo{
			.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
			.setLayoutCount = 1,
			.pSetLayouts = &pass.setLayout
		};
		if (vkCreatePipelineLayout(device, &pipelineLayoutInfo, nullptr, &pass.pipelineLayout) != VK_SUCCESS) {
			Logger::Get().Error("vkCreatePipelineLayout failed");
			return false;
		}

		const VkShaderModuleCreateInfo moduleInfo{
			.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
			.codeSize = passDesc.spirv.size() * sizeof(uint32_t),
			.pCode = passDesc.spirv.data()
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
				// The entry point that EffectCompiler generates
				.pName = "__M"
			},
			.layout = pass.pipelineLayout
		};
		const VkResult result = vkCreateComputePipelines(
			device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &pass.pipeline);
		vkDestroyShaderModule(device, module, nullptr);
		if (result != VK_SUCCESS) {
			Logger::Get().Error(fmt::format("Creating the pipeline of pass {} failed", i + 1));
			return false;
		}

		const VkDescriptorSetAllocateInfo setInfo{
			.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
			.descriptorPool = _descriptorPool,
			.descriptorSetCount = 1,
			.pSetLayouts = &pass.setLayout
		};
		if (vkAllocateDescriptorSets(device, &setInfo, &pass.descriptorSet) != VK_SUCCESS) {
			Logger::Get().Error("vkAllocateDescriptorSets failed");
			return false;
		}
	}

	return true;
}

bool EffectDrawer::_UpdatePassResources(const EffectDesc& desc, const Environment& env) noexcept {
	const uint32_t passCount = (uint32_t)desc.passes.size();
	const uint32_t samplerCount = (uint32_t)desc.samplers.size();
	const bool useDynamic = desc.flags & EffectFlags::UseDynamic;

	if (useDynamic && !env.dynamicCB) {
		Logger::Get().Error("The effect needs the dynamic constant buffer");
		return false;
	}

	const VkDescriptorBufferInfo cb1Info{ _constantBuffer->buffer, 0, VK_WHOLE_SIZE };
	const VkDescriptorBufferInfo cb2Info{ env.dynamicCB, 0, VK_WHOLE_SIZE };

	for (uint32_t i = 0; i < passCount; ++i) {
		const SmallVector<uint32_t>& inputs = desc.passes[i].inputs;
		const SmallVector<uint32_t>& outputs = desc.passes[i].outputs;
		const std::pair<uint32_t, uint32_t>& blockSize = desc.passes[i].blockSize;
		const uint32_t inputCount = (uint32_t)inputs.size();
		const uint32_t outputCount = (uint32_t)outputs.size();
		Pass& pass = _passes[i];

		// The writes point into imageInfos, so it must not reallocate
		std::vector<VkDescriptorImageInfo> imageInfos;
		imageInfos.reserve(inputCount + outputCount + samplerCount);
		std::vector<VkWriteDescriptorSet> writes;

		const auto addWrite = [&](uint32_t binding, VkDescriptorType type) -> VkWriteDescriptorSet& {
			return writes.emplace_back(VkWriteDescriptorSet{
				.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
				.dstSet = pass.descriptorSet,
				.dstBinding = binding,
				.descriptorCount = 1,
				.descriptorType = type
			});
		};

		addWrite(EffectHelper::BINDING_CB1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER).pBufferInfo = &cb1Info;
		if (useDynamic) {
			addWrite(EffectHelper::BINDING_CB2, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER).pBufferInfo = &cb2Info;
		}

		for (uint32_t j = 0; j < inputCount; ++j) {
			const Texture& texture = *_textures[inputs[j]];
			imageInfos.push_back({ VK_NULL_HANDLE, texture.view, texture.layout });
			addWrite(EffectHelper::InputBinding(j), VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE).pImageInfo = &imageInfos.back();
		}

		for (uint32_t j = 0; j < outputCount; ++j) {
			const Texture& texture = *_textures[outputs[j]];
			imageInfos.push_back({ VK_NULL_HANDLE, texture.view, VK_IMAGE_LAYOUT_GENERAL });
			addWrite(EffectHelper::OutputBinding(inputCount, j), VK_DESCRIPTOR_TYPE_STORAGE_IMAGE).pImageInfo =
				&imageInfos.back();
		}

		for (uint32_t j = 0; j < samplerCount; ++j) {
			imageInfos.push_back({ _samplers[j], VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED });
			addWrite(EffectHelper::SamplerBinding(inputCount, outputCount, j), VK_DESCRIPTOR_TYPE_SAMPLER).pImageInfo =
				&imageInfos.back();
		}

		vkUpdateDescriptorSets(_device->Device(), (uint32_t)writes.size(), writes.data(), 0, nullptr);

		const Texture& output = *_textures[outputs[0]];
		pass.groupCountX = (output.width + blockSize.first - 1) / blockSize.first;
		pass.groupCountY = (output.height + blockSize.second - 1) / blockSize.second;
	}

	return true;
}

bool EffectDrawer::_UpdateConstants(
	const EffectDesc& desc,
	const EffectOption& option,
	Size inputSize,
	Size outputSize
) noexcept {
	const bool isInlineParams = desc.flags & EffectFlags::InlineParams;

	SmallVector<EffectHelper::Constant32, 32> constants;

	// The size must be a multiple of 4
	const size_t builtinConstantCount = 10;
	size_t psStylePassParams = 0;
	for (uint32_t i = 0, end = (uint32_t)desc.passes.size() - 1; i < end; ++i) {
		if (desc.passes[i].flags & EffectPassFlags::PSStyle) {
			psStylePassParams += 4;
		}
	}
	constants.resize((builtinConstantCount + psStylePassParams + (isInlineParams ? 0 : desc.params.size()) + 3) / 4 * 4);
	// cbuffer __CB1 {
	//     uint2 __inputSize;
	//     uint2 __outputSize;
	//     float2 __inputPt;
	//     float2 __outputPt;
	//     float2 __scale;
	//     [PARAMETERS...]
	// );
	constants[0].uintVal = (uint32_t)inputSize.width;
	constants[1].uintVal = (uint32_t)inputSize.height;
	constants[2].uintVal = (uint32_t)outputSize.width;
	constants[3].uintVal = (uint32_t)outputSize.height;
	constants[4].floatVal = 1.0f / inputSize.width;
	constants[5].floatVal = 1.0f / inputSize.height;
	constants[6].floatVal = 1.0f / outputSize.width;
	constants[7].floatVal = 1.0f / outputSize.height;
	constants[8].floatVal = outputSize.width / (float)inputSize.width;
	constants[9].floatVal = outputSize.height / (float)inputSize.height;

	// Parameters of the passes in pixel shader style
	EffectHelper::Constant32* pCurParam = constants.data() + builtinConstantCount;
	if (psStylePassParams > 0) {
		for (uint32_t i = 0, end = (uint32_t)desc.passes.size() - 1; i < end; ++i) {
			if (desc.passes[i].flags & EffectPassFlags::PSStyle) {
				const Texture& output = *_textures[desc.passes[i].outputs[0]];
				pCurParam->uintVal = output.width;
				++pCurParam;
				pCurParam->uintVal = output.height;
				++pCurParam;
				pCurParam->floatVal = 1.0f / output.width;
				++pCurParam;
				pCurParam->floatVal = 1.0f / output.height;
				++pCurParam;
			}
		}
	}

	if (!isInlineParams) {
		for (uint32_t i = 0; i < desc.params.size(); ++i) {
			const auto& paramDesc = desc.params[i];
			auto it = option.parameters.find(paramDesc.name);

			if (paramDesc.constant.index() == 0) {
				const EffectConstant<float>& constant = std::get<0>(paramDesc.constant);
				float value = constant.defaultValue;

				if (it != option.parameters.end()) {
					value = it->second;

					if (value < constant.minValue || value > constant.maxValue) {
						Logger::Get().Error(fmt::format("Invalid value of parameter {}", paramDesc.name));
						return false;
					}
				}

				pCurParam->floatVal = value;
			} else {
				const EffectConstant<int>& constant = std::get<1>(paramDesc.constant);
				int value = constant.defaultValue;

				if (it != option.parameters.end()) {
					value = (int)std::lround(it->second);

					if ((value < constant.minValue) || (value > constant.maxValue)) {
						Logger::Get().Error(fmt::format("Invalid value of parameter {}", paramDesc.name));
						return false;
					}
				}

				pCurParam->intVal = value;
			}

			++pCurParam;
		}
	}

	const size_t byteSize = 4 * constants.size();
	if (!_constantBuffer) {
		_constantBuffer = _device->CreateBuffer(byteSize, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
		if (!_constantBuffer) {
			Logger::Get().Error("Creating the constant buffer failed");
			return false;
		}
	}

	// No command that reads the buffer is pending at this point
	assert(_constantBuffer->size == byteSize);
	std::memcpy(_constantBuffer->data, constants.data(), byteSize);

	return true;
}

}
