// HLSL to SPIR-V through the DirectX Shader Compiler. Linux counterpart of
// DirectXHelper::CompileComputeShader, which calls D3DCompile on Windows.
#pragma once
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Magpie {

struct ShaderCompiler {
	struct Options {
		// Searched for #include files
		std::filesystem::path includeDir;
		std::vector<std::pair<std::string, std::string>> macros;
		// min16float becomes a real 16-bit float instead of a 32-bit float
		// marked as relaxed precision. Needs the shaderFloat16 device feature.
		bool nativeFP16 = false;
		bool warningsAreErrors = false;
	};

	// Compiles a compute shader for Vulkan 1.3. `sourceName` only appears in
	// messages. Returns false on failure; `messages` receives the compiler's
	// errors and warnings in both cases. May be called from several threads.
	static bool CompileComputeShader(
		std::string_view hlsl,
		const char* entryPoint,
		const char* sourceName,
		const Options& options,
		std::vector<uint32_t>& spirv,
		std::string& messages
	) noexcept;

	// Version and commit of the compiler library, for cache keys. Empty if the
	// library cannot be queried.
	static std::string Version() noexcept;
};

}
