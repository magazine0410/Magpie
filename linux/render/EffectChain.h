// The effects of a scaling mode as one unit: compiles them, creates a drawer
// for each, appends Bicubic where the result does not fit the renderer area,
// and draws them in order. Linux counterpart of the effect handling in
// src/Magpie.Core/Renderer.cpp (_BuildEffects, _ShouldAppendBicubic,
// _AppendBicubic, _ResizeEffects, _UpdateDynamicConstants and the effect loop
// of _BackendRender).
#pragma once
#include <filesystem>
#include <memory>
#include <vector>
#include <vulkan/vulkan.h>
#include "EffectCompiler.h"
#include "EffectDesc.h"
#include "EffectDrawer.h"
#include "EffectsProfiler.h"
#include "Geometry.h"
#include "ScalingOptions.h"

namespace Magpie {

class EffectChain {
public:
	struct Config {
		// Directory of the .hlsl files
		std::filesystem::path effectsDir;
		// Size of the area that shows the scaled image
		Size rendererSize;
		// Effects that support FP16 are compiled with real 16-bit floats if the
		// graphics card has them and without FP16 otherwise. This flag selects
		// relaxed precision instead, which leaves the choice to the driver as
		// min16float does on Windows. It is not the default because it gained
		// nothing on the drivers measured and one of them miscompiled it.
		bool relaxedFP16 = false;
	};

	EffectChain() = default;
	EffectChain(const EffectChain&) = delete;
	EffectChain(EffectChain&&) = delete;

	// Uses these members of `options`: effects, and the flags for windowed
	// mode, effect cache, saving sources, warnings as errors, inline
	// parameters and FP16. `input` must be in a layout that can be sampled.
	bool Initialize(
		VulkanDevice& device,
		const ScalingOptions& options,
		const Config& config,
		std::shared_ptr<Texture> input
	) noexcept;

	// Adapts to a new renderer size or input texture. No GPU work that uses
	// the chain may be pending.
	bool Resize(Size rendererSize, std::shared_ptr<Texture> input) noexcept;

	// Records all passes. `frameCount` is what dynamic effects see as the
	// frame counter. The previous frame must have been executed, because the
	// counter is written to a buffer that its commands read.
	void Draw(VkCommandBuffer cmd, uint32_t frameCount) noexcept;

	const std::shared_ptr<Texture>& Output() const noexcept {
		return _effectDrawers.back()->GetOutputTexture();
	}

	// What effect number `index` of ActiveEffectDescs drew last
	const std::shared_ptr<Texture>& EffectOutput(size_t index) const noexcept {
		return _effectDrawers[index]->GetOutputTexture();
	}

	// The effects that are drawn, including an appended Bicubic
	const std::vector<const EffectDesc*>& ActiveEffectDescs() const noexcept {
		return _activeEffectDescs;
	}

	uint32_t PassCount() const noexcept;

	EffectsProfiler& Profiler() noexcept {
		return _effectsProfiler;
	}

private:
	EffectDrawer::Environment _Environment() const noexcept;

	void _UpdateActiveEffectDescs() noexcept;

	bool _ShouldAppendBicubic(const Texture& outTexture) const noexcept;

	bool _AppendBicubic(std::shared_ptr<Texture>& inOutTexture) noexcept;

	EffectOption _BicubicOption() const noexcept;

	VulkanDevice* _device = nullptr;
	Config _config;
	std::vector<EffectOption> _effects;
	EffectCompilerFlags _compilerFlags = EffectCompilerFlags::None;
	bool _isWindowedMode = false;

	std::vector<EffectDesc> _effectDescs;
	EffectDesc _bicubicDesc;
	std::vector<const EffectDesc*> _activeEffectDescs;
	std::vector<std::unique_ptr<EffectDrawer>> _effectDrawers;

	// __CB2: { uint __frameCount; }
	std::unique_ptr<Buffer> _dynamicCB;
	EffectsProfiler _effectsProfiler;
};

}
