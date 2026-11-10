#include "EffectChain.h"
#include <cassert>
#include <chrono>
#include <cstring>
#include <optional>
#include <thread>
#include <fmt/format.h>
#include "Logger.h"
#include "VulkanDevice.h"

namespace Magpie {

static std::optional<EffectDesc> CompileEffect(
	const EffectOption& effectOption,
	EffectCompilerFlags flags,
	const std::filesystem::path& effectsDir
) noexcept {
	EffectDesc result{ .name = effectOption.name };

	const auto start = std::chrono::steady_clock::now();
	const bool success = !EffectCompiler::Compile(result, flags, effectsDir, &effectOption.parameters);
	const std::chrono::duration<float, std::milli> duration = std::chrono::steady_clock::now() - start;

	if (success) {
		Logger::Get().Info(fmt::format("Compiling {}.hlsl took {:.1f} ms", effectOption.name, duration.count()));
		return result;
	} else {
		Logger::Get().Error(fmt::format("Compiling {}.hlsl failed", effectOption.name));
		return std::nullopt;
	}
}

bool EffectChain::Initialize(
	VulkanDevice& device,
	const ScalingOptions& options,
	const Config& config,
	std::shared_ptr<Texture> input
) noexcept {
	_device = &device;
	_config = config;
	_effects = options.effects;
	_isWindowedMode = options.IsWindowedMode();

	assert(!_effects.empty());
	const uint32_t effectCount = (uint32_t)_effects.size();

	if (options.IsEffectCacheDisabled()) {
		_compilerFlags |= EffectCompilerFlags::NoCache;
	}
	if (options.IsSaveEffectSources()) {
		_compilerFlags |= EffectCompilerFlags::SaveSources;
	}
	if (options.IsWarningsAreErrors()) {
		_compilerFlags |= EffectCompilerFlags::WarningsAreErrors;
	}

	EffectCompilerFlags effectFlags = _compilerFlags;
	if (options.IsInlineParams()) {
		effectFlags |= EffectCompilerFlags::InlineParams;
	}
	if (options.IsFP16Disabled()) {
		effectFlags |= EffectCompilerFlags::NoFP16;
	} else if (!config.relaxedFP16) {
		effectFlags |= device.IsFP16Supported() ? EffectCompilerFlags::NativeFP16 : EffectCompilerFlags::NoFP16;
	}

	// Compile all effects in parallel
	_effectDescs.resize(effectCount);
	std::vector<char> succeeded(effectCount);

	const auto start = std::chrono::steady_clock::now();
	{
		const auto compile = [&](uint32_t id) {
			if (std::optional<EffectDesc> desc = CompileEffect(_effects[id], effectFlags, _config.effectsDir)) {
				_effectDescs[id] = std::move(*desc);
				succeeded[id] = 1;
			}
		};

		std::vector<std::jthread> threads;
		for (uint32_t i = 1; i < effectCount; ++i) {
			threads.emplace_back(compile, i);
		}
		compile(0);
	}
	const std::chrono::duration<float, std::milli> duration = std::chrono::steady_clock::now() - start;

	for (char success : succeeded) {
		if (!success) {
			return false;
		}
	}

	if (effectCount > 1) {
		Logger::Get().Info(fmt::format("Compiling all effects took {:.1f} ms", duration.count()));
	}

	// The constant buffer that all dynamic effects share. The drawers refer
	// to it, so it is created before them.
	for (const EffectDesc& effectDesc : _effectDescs) {
		if (effectDesc.flags & EffectFlags::UseDynamic) {
			// Only 4 bytes are used
			_dynamicCB = _device->CreateBuffer(16, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
			if (!_dynamicCB) {
				Logger::Get().Error("Creating the dynamic constant buffer failed");
				return false;
			}
			std::memset(_dynamicCB->data, 0, 16);
			break;
		}
	}

	const EffectDrawer::Environment env = _Environment();

	std::shared_ptr<Texture> inOutTexture = std::move(input);
	for (uint32_t i = 0; i < effectCount; ++i) {
		_effectDrawers.push_back(std::make_unique<EffectDrawer>());
		if (!_effectDrawers.back()->Initialize(_effectDescs[i], _effects[i], env, inOutTexture)) {
			Logger::Get().Error(fmt::format("Initializing effect #{} ({}) failed", i, _effects[i].name));
			return false;
		}

		// The SPIR-V is no longer needed
		for (EffectPassDesc& passDesc : _effectDescs[i].passes) {
			passDesc.spirv = {};
		}
	}

	if (_ShouldAppendBicubic(*inOutTexture)) {
		if (!_AppendBicubic(inOutTexture)) {
			Logger::Get().Error("_AppendBicubic failed");
			return false;
		}
	}

	_UpdateActiveEffectDescs();
	return true;
}

bool EffectChain::Resize(Size rendererSize, std::shared_ptr<Texture> input) noexcept {
	_config.rendererSize = rendererSize;

	const EffectDrawer::Environment env = _Environment();
	const uint32_t effectCount = (uint32_t)_effects.size();

	std::shared_ptr<Texture> inOutTexture = std::move(input);
	for (uint32_t i = 0; i < effectCount; ++i) {
		if (!_effectDrawers[i]->ResizeTextures(_effectDescs[i], _effects[i], env, inOutTexture)) {
			Logger::Get().Error(fmt::format("Resizing effect #{} ({}) failed", i, _effects[i].name));
			return false;
		}
	}

	// The appended Bicubic
	bool changed = false;
	if (_ShouldAppendBicubic(*inOutTexture)) {
		if (_effectDrawers.size() > effectCount) {
			if (!_effectDrawers.back()->ResizeTextures(_bicubicDesc, _BicubicOption(), env, inOutTexture)) {
				Logger::Get().Error("Resizing effect Bicubic failed");
				return false;
			}
		} else {
			if (!_AppendBicubic(inOutTexture)) {
				Logger::Get().Error("_AppendBicubic failed");
				return false;
			}
			changed = true;
		}
	} else {
		if (_effectDrawers.size() > effectCount) {
			_effectDrawers.resize(effectCount);
			changed = true;
		}
	}

	if (changed) {
		_UpdateActiveEffectDescs();
		_effectsProfiler.SetPassCount(PassCount());
	}

	return true;
}

void EffectChain::Draw(VkCommandBuffer cmd, uint32_t frameCount) noexcept {
	if (_dynamicCB) {
		// cbuffer __CB2 { uint __frameCount; };
		std::memcpy(_dynamicCB->data, &frameCount, 4);
	}

	_effectsProfiler.OnBeginEffects(cmd);

	for (const std::unique_ptr<EffectDrawer>& effectDrawer : _effectDrawers) {
		effectDrawer->Draw(cmd, _effectsProfiler);
	}
}

uint32_t EffectChain::PassCount() const noexcept {
	uint32_t passCount = 0;
	for (const EffectDesc* desc : _activeEffectDescs) {
		passCount += (uint32_t)desc->passes.size();
	}
	return passCount;
}

EffectDrawer::Environment EffectChain::_Environment() const noexcept {
	return {
		.device = _device,
		.effectsDir = _config.effectsDir,
		.rendererSize = _config.rendererSize,
		.isWindowedMode = _isWindowedMode,
		.dynamicCB = _dynamicCB ? _dynamicCB->buffer : VK_NULL_HANDLE
	};
}

void EffectChain::_UpdateActiveEffectDescs() noexcept {
	const uint32_t effectCount = (uint32_t)_effectDescs.size();
	const uint32_t drawerCount = (uint32_t)_effectDrawers.size();

	_activeEffectDescs.resize(drawerCount);

	for (uint32_t i = 0; i < effectCount; ++i) {
		_activeEffectDescs[i] = &_effectDescs[i];
	}

	if (drawerCount > effectCount) {
		// Bicubic has been appended
		assert(drawerCount == effectCount + 1);
		_activeEffectDescs[effectCount] = &_bicubicDesc;
	}
}

bool EffectChain::_ShouldAppendBicubic(const Texture& outTexture) const noexcept {
	const Size lastOutputSize{ (int32_t)outTexture.width, (int32_t)outTexture.height };
	const Size rendererSize = _config.rendererSize;

	if (_isWindowedMode) {
		// Windowed scaling enlarges with Bicubic. With B=0 and C=0.5 it is
		// about as sharp as Lanczos.
		return lastOutputSize != rendererSize;
	} else {
		// An output larger than the renderer area has to be reduced
		return lastOutputSize.width > rendererSize.width || lastOutputSize.height > rendererSize.height;
	}
}

EffectOption EffectChain::_BicubicOption() const noexcept {
	return {
		.name = "Bicubic",
		.parameters{
			{"paramB", 0.0f},
			{"paramC", 0.5f}
		},
		.scalingType = _isWindowedMode ? ScalingType::Fill : ScalingType::Fit
	};
}

bool EffectChain::_AppendBicubic(std::shared_ptr<Texture>& inOutTexture) noexcept {
	const EffectOption bicubicOption = _BicubicOption();

	if (_bicubicDesc.name.empty()) {
		// The parameters never change, so they can be inlined
		std::optional<EffectDesc> desc = CompileEffect(
			bicubicOption,
			_compilerFlags | EffectCompilerFlags::NoFP16 | EffectCompilerFlags::InlineParams,
			_config.effectsDir
		);
		if (!desc) {
			Logger::Get().Error("Compiling the effect for reducing the output failed");
			return false;
		}

		_bicubicDesc = std::move(*desc);
	}

	_effectDrawers.push_back(std::make_unique<EffectDrawer>());
	if (!_effectDrawers.back()->Initialize(_bicubicDesc, bicubicOption, _Environment(), inOutTexture)) {
		Logger::Get().Error("Initializing the effect for reducing the output failed");
		return false;
	}

	return true;
}

}
