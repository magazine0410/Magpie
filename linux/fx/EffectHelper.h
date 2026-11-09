// Linux counterpart of src/Magpie.Core/EffectHelper.h: properties of the
// texture formats an effect may name, and the descriptor bindings of a pass.
#pragma once
#include <cstdint>
#include <vulkan/vulkan_core.h>

namespace Magpie {

struct EffectHelper {
	struct EffectIntermediateTextureFormatDesc {
		const char* name;
		VkFormat vkFormat;
		uint32_t nChannel;
		// Element types of the texture as a sampled input and as a storage
		// output, as on Windows. MF is float, or min16float with FP16. The
		// generator replaces MF by float where DXC or Vulkan require a 32-bit
		// element type (see TexelType in EffectCompiler.cpp).
		const char* srvTexelType;
		const char* uavTexelType;
		// Argument of [[vk::image_format]]. Without it DXC derives the format
		// from the element type, which does not match the texture.
		const char* imageFormat;
	};

	// Indexed by EffectIntermediateTextureFormat
	static constexpr EffectIntermediateTextureFormatDesc FORMAT_DESCS[] = {
		{"R32G32B32A32_FLOAT", VK_FORMAT_R32G32B32A32_SFLOAT, 4, "float4", "float4", "rgba32f"},
		{"R16G16B16A16_FLOAT", VK_FORMAT_R16G16B16A16_SFLOAT, 4, "MF4", "MF4", "rgba16f"},
		{"R16G16B16A16_UNORM", VK_FORMAT_R16G16B16A16_UNORM, 4, "MF4", "unorm MF4", "rgba16"},
		{"R16G16B16A16_SNORM", VK_FORMAT_R16G16B16A16_SNORM, 4, "MF4", "snorm MF4", "rgba16snorm"},
		{"R32G32_FLOAT", VK_FORMAT_R32G32_SFLOAT, 2, "float2", "float2", "rg32f"},
		{"R10G10B10A2_UNORM", VK_FORMAT_A2B10G10R10_UNORM_PACK32, 4, "MF4", "unorm MF4", "rgb10a2"},
		{"R11G11B10_FLOAT", VK_FORMAT_B10G11R11_UFLOAT_PACK32, 3, "MF3", "MF3", "r11g11b10f"},
		{"R8G8B8A8_UNORM", VK_FORMAT_R8G8B8A8_UNORM, 4, "MF4", "unorm MF4", "rgba8"},
		{"R8G8B8A8_SNORM", VK_FORMAT_R8G8B8A8_SNORM, 4, "MF4", "snorm MF4", "rgba8snorm"},
		{"R16G16_FLOAT", VK_FORMAT_R16G16_SFLOAT, 2, "MF2", "MF2", "rg16f"},
		{"R16G16_UNORM", VK_FORMAT_R16G16_UNORM, 2, "MF2", "unorm MF2", "rg16"},
		{"R16G16_SNORM", VK_FORMAT_R16G16_SNORM, 2, "MF2", "snorm MF2", "rg16snorm"},
		{"R32_FLOAT", VK_FORMAT_R32_SFLOAT, 1, "float", "float", "r32f"},
		{"R8G8_UNORM", VK_FORMAT_R8G8_UNORM, 2, "MF2", "unorm MF2", "rg8"},
		{"R8G8_SNORM", VK_FORMAT_R8G8_SNORM, 2, "MF2", "snorm MF2", "rg8snorm"},
		{"R16_FLOAT", VK_FORMAT_R16_SFLOAT, 1, "MF", "MF", "r16f"},
		{"R16_UNORM", VK_FORMAT_R16_UNORM, 1, "MF", "unorm MF", "r16"},
		{"R16_SNORM", VK_FORMAT_R16_SNORM, 1, "MF", "snorm MF", "r16snorm"},
		{"R8_UNORM", VK_FORMAT_R8_UNORM, 1, "MF", "unorm MF", "r8"},
		{"R8_SNORM", VK_FORMAT_R8_SNORM, 1, "MF", "snorm MF", "r8snorm"},
		{"UNKNOWN", VK_FORMAT_UNDEFINED, 4, "float4", "float4", nullptr}
	};

	// Descriptor bindings of a pass. Everything is in set 0:
	//   0        constant buffer __CB1 (sizes and parameters)
	//   1        constant buffer __CB2 (frame counter; only effects with "USE Dynamic")
	//   2 ...    the pass's input textures, in the order of its IN list
	//   then     the pass's output textures, in the order of its OUT list
	//   then     all samplers of the effect, in the order of their declaration
	// The pass source generator and the renderer both take the numbers from
	// here.
	static constexpr uint32_t BINDING_CB1 = 0;
	static constexpr uint32_t BINDING_CB2 = 1;

	static constexpr uint32_t InputBinding(uint32_t inputIdx) noexcept {
		return 2 + inputIdx;
	}

	static constexpr uint32_t OutputBinding(uint32_t inputCount, uint32_t outputIdx) noexcept {
		return 2 + inputCount + outputIdx;
	}

	static constexpr uint32_t SamplerBinding(uint32_t inputCount, uint32_t outputCount, uint32_t samplerIdx) noexcept {
		return 2 + inputCount + outputCount + samplerIdx;
	}

	union Constant32 {
		float floatVal;
		uint32_t uintVal;
		int intVal;
	};
};

}
