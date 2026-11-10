// Runs the passes of one effect: one compute dispatch per pass. Linux
// counterpart of src/Magpie.Core/EffectDrawer.h.
#pragma once
#include <filesystem>
#include <memory>
#include <vector>
#include <muParser.h>
#include <vulkan/vulkan.h>
#include "EffectDesc.h"
#include "Geometry.h"
#include "SmallVector.h"

namespace Magpie {

struct EffectOption;
struct Texture;
struct Buffer;
class VulkanDevice;
class EffectsProfiler;

class EffectDrawer {
public:
	// What the Windows code reads from the scaling window and the renderer
	struct Environment {
		VulkanDevice* device = nullptr;
		// Directory of the .hlsl files. Textures named by SOURCE are loaded
		// from the directory of their effect.
		std::filesystem::path effectsDir;
		// Size of the area that shows the scaled image
		Size rendererSize;
		bool isWindowedMode = false;
		// Constant buffer __CB2, shared by all effects of the chain. Only
		// needed for effects with EffectFlags::UseDynamic.
		VkBuffer dynamicCB = VK_NULL_HANDLE;
	};

	EffectDrawer() = default;
	EffectDrawer(const EffectDrawer&) = delete;
	EffectDrawer(EffectDrawer&&) = delete;

	// No GPU work that uses the drawer may be pending
	~EffectDrawer();

	// `inOutTexture` is the input on entry and the output on return. The
	// passes of `desc` must be compiled.
	bool Initialize(
		const EffectDesc& desc,
		const EffectOption& option,
		const Environment& env,
		std::shared_ptr<Texture>& inOutTexture
	) noexcept;

	void Draw(VkCommandBuffer cmd, EffectsProfiler& profiler) const noexcept;

	// Adapts to a new input texture or renderer size. No GPU work that uses
	// the drawer may be pending.
	bool ResizeTextures(
		const EffectDesc& desc,
		const EffectOption& option,
		const Environment& env,
		std::shared_ptr<Texture>& inOutTexture
	) noexcept;

	const std::shared_ptr<Texture>& GetOutputTexture() const noexcept {
		return _textures[1];
	}

	const std::shared_ptr<Texture>& GetTexture(uint32_t idx) const noexcept {
		return _textures[idx];
	}

private:
	struct Pass {
		VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
		VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
		VkPipeline pipeline = VK_NULL_HANDLE;
		VkDescriptorSet descriptorSet = VK_NULL_HANDLE;
		uint32_t groupCountX = 0;
		uint32_t groupCountY = 0;
	};

	Size _CalcOutputSize(
		const EffectDesc& desc,
		const EffectOption& option,
		const Environment& env,
		Size inputSize
	) const noexcept;

	// Returns an empty size on failure
	Size _CalcTextureSize(const std::pair<std::string, std::string>& sizeExpr) const noexcept;

	std::shared_ptr<Texture> _CreateEffectTexture(
		EffectIntermediateTextureFormat format, Size size) const noexcept;

	std::shared_ptr<Texture> _LoadSourceTexture(
		const EffectDesc& desc,
		const EffectIntermediateTextureDesc& texDesc,
		const Environment& env
	) const noexcept;

	bool _CreatePasses(const EffectDesc& desc) noexcept;

	bool _UpdatePassResources(const EffectDesc& desc, const Environment& env) noexcept;

	bool _UpdateConstants(
		const EffectDesc& desc,
		const EffectOption& option,
		Size inputSize,
		Size outputSize
	) noexcept;

	VulkanDevice* _device = nullptr;

	SmallVector<VkSampler> _samplers;
	// [0] is the input and belongs to the previous drawer or the frame source
	std::vector<std::shared_ptr<Texture>> _textures;
	std::unique_ptr<Buffer> _constantBuffer;

	VkDescriptorPool _descriptorPool = VK_NULL_HANDLE;
	std::vector<Pass> _passes;

	mutable mu::Parser _exprParser;
};

}
