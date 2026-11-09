// Linux counterpart of src/Magpie.Core/include/EffectCompiler.h: parses a
// MagpieFX effect and compiles its passes to SPIR-V.
#pragma once
#include <filesystem>
#include <map>
#include <string>
#include "CommonDefines.h"

namespace Magpie {

struct EffectDesc;

enum class EffectCompilerFlags {
	// Flags that change the compiled code are in the low 16 bits, so that the
	// cache can keep one file per combination.
	None,
	InlineParams = 1,
	NoFP16 = 1 << 1,
	// Linux only: compile the FP16 variant with real 16-bit floats instead of
	// relaxed precision. The device must support shaderFloat16.
	NativeFP16 = 1 << 2,
	// Only parse output size and parameters, for the user interface
	NoCompile = 1 << 16,
	NoCache = 1 << 17,
	SaveSources = 1 << 18,
	WarningsAreErrors = 1 << 19
};
DEFINE_ENUM_FLAG_OPERATORS(EffectCompilerFlags)

struct EffectCompiler {
	// The caller fills in desc.name (for example "FSR\FSR_EASU") and
	// desc.flags. `effectsDir` is the directory that holds the .hlsl files.
	// Returns 0 on success.
	static uint32_t Compile(
		EffectDesc& desc,
		EffectCompilerFlags flags,
		const std::filesystem::path& effectsDir,
		const std::map<std::string, float>* inlineParams = nullptr
	) noexcept;
};

}
