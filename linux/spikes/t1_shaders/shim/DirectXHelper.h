// Stand-in for DirectXHelper: instead of calling D3DCompile, the generated
// pass source and its macros are recorded so that fxdump can write them out.
#pragma once
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Magpie {
struct DirectXHelper {
	static bool CompileComputeShader(
		std::string_view hlsl,
		const char* entryPoint,
		ID3DBlob** blob,
		const char* sourceName = nullptr,
		ID3DInclude* include = nullptr,
		const std::vector<std::pair<std::string, std::string>>& macros = {},
		bool warningsAreErrors = false
	);
};
}
