#include "ShaderCompiler.h"
#include <cstring>
#include <fmt/format.h>
// Declares a subset of the Windows API for itself on Linux.
#include <dxc/dxcapi.h>

namespace Magpie {

// The compiler takes its arguments as wide strings.
static std::wstring ToWide(std::string_view utf8) noexcept {
	std::wstring result;
	result.reserve(utf8.size());

	for (size_t i = 0; i < utf8.size();) {
		const unsigned char c = (unsigned char)utf8[i];
		uint32_t codePoint;
		size_t length;
		if (c < 0x80) {
			codePoint = c;
			length = 1;
		} else if ((c >> 5) == 0x6) {
			codePoint = c & 0x1F;
			length = 2;
		} else if ((c >> 4) == 0xE) {
			codePoint = c & 0x0F;
			length = 3;
		} else {
			codePoint = c & 0x07;
			length = 4;
		}

		for (size_t j = 1; j < length && i + j < utf8.size(); ++j) {
			codePoint = (codePoint << 6) | ((unsigned char)utf8[i + j] & 0x3F);
		}

		result.push_back((wchar_t)codePoint);
		i += length;
	}

	return result;
}

// One compiler instance per thread, as the library requires.
struct ThreadCompiler {
	CComPtr<IDxcUtils> utils;
	CComPtr<IDxcCompiler3> compiler;
	CComPtr<IDxcIncludeHandler> includeHandler;
	bool ok = false;

	ThreadCompiler() noexcept {
		ok = SUCCEEDED(DxcCreateInstance(CLSID_DxcUtils, IID_PPV_ARGS(&utils))) &&
			SUCCEEDED(DxcCreateInstance(CLSID_DxcCompiler, IID_PPV_ARGS(&compiler))) &&
			SUCCEEDED(utils->CreateDefaultIncludeHandler(&includeHandler));
	}
};

static ThreadCompiler& GetThreadCompiler() noexcept {
	thread_local ThreadCompiler instance;
	return instance;
}

bool ShaderCompiler::CompileShader(
	Stage stage,
	std::string_view hlsl,
	const char* entryPoint,
	const char* sourceName,
	const Options& options,
	std::vector<uint32_t>& spirv,
	std::string& messages
) noexcept {
	ThreadCompiler& tc = GetThreadCompiler();
	if (!tc.ok) {
		messages = "The DirectX Shader Compiler could not be initialised";
		return false;
	}

	static constexpr const wchar_t* PROFILES[][2] = {
		{ L"cs_6_0", L"cs_6_2" },
		{ L"vs_6_0", L"vs_6_2" },
		{ L"ps_6_0", L"ps_6_2" }
	};

	std::vector<std::wstring> args{
		ToWide(sourceName ? sourceName : "shader.hlsl"),
		L"-E", ToWide(entryPoint),
		// 16-bit types need shader model 6.2
		L"-T", PROFILES[(size_t)stage][options.nativeFP16 ? 1 : 0],
		L"-spirv",
		L"-fspv-target-env=vulkan1.3",
		// HLSL 2018 keeps the language rules of the Windows compiler (FXC);
		// 2021 changes the ternary operator on vectors, among other things.
		L"-HV", L"2018",
		L"-O3",
		// Same packing of constant buffers as Direct3D, so the code that fills
		// them is the same on both systems.
		L"-fvk-use-dx-layout",
		L"-I", ToWide(options.includeDir.string()),
	};
	if (options.nativeFP16) {
		args.push_back(L"-enable-16bit-types");
		// Textures hold 32-bit floats; the conversions to the 16-bit shader
		// variables are intended.
		args.push_back(L"-Wno-conversion");
	}
	if (options.warningsAreErrors) {
		args.push_back(L"-WX");
	}
	for (const auto& [name, value] : options.macros) {
		args.push_back(L"-D");
		args.push_back(ToWide(value.empty() ? name : fmt::format("{}={}", name, value)));
	}

	std::vector<LPCWSTR> argPointers;
	argPointers.reserve(args.size());
	for (const std::wstring& arg : args) {
		argPointers.push_back(arg.c_str());
	}

	const DxcBuffer source{ hlsl.data(), hlsl.size(), DXC_CP_UTF8 };
	CComPtr<IDxcResult> result;
	HRESULT hr = tc.compiler->Compile(&source, argPointers.data(), (UINT32)argPointers.size(),
		tc.includeHandler, IID_PPV_ARGS(&result));
	if (FAILED(hr) || !result) {
		messages = fmt::format("IDxcCompiler3::Compile failed (0x{:08x})", (uint32_t)hr);
		return false;
	}

	CComPtr<IDxcBlobUtf8> errors;
	if (SUCCEEDED(result->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(&errors), nullptr)) && errors) {
		messages.assign(errors->GetStringPointer(), errors->GetStringLength());
	} else {
		messages.clear();
	}

	HRESULT status = E_FAIL;
	result->GetStatus(&status);
	if (FAILED(status)) {
		if (messages.empty()) {
			messages = fmt::format("compilation failed (0x{:08x})", (uint32_t)status);
		}
		return false;
	}

	CComPtr<IDxcBlob> object;
	hr = result->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&object), nullptr);
	if (FAILED(hr) || !object || object->GetBufferSize() == 0 || object->GetBufferSize() % 4 != 0) {
		messages = "The compiler returned no SPIR-V";
		return false;
	}

	spirv.resize(object->GetBufferSize() / 4);
	std::memcpy(spirv.data(), object->GetBufferPointer(), object->GetBufferSize());
	return true;
}

std::string ShaderCompiler::Version() noexcept {
	ThreadCompiler& tc = GetThreadCompiler();
	if (!tc.ok) {
		return {};
	}

	CComPtr<IDxcVersionInfo2> info;
	if (FAILED(tc.compiler->QueryInterface(IID_PPV_ARGS(&info))) || !info) {
		return {};
	}

	UINT32 major = 0, minor = 0, commitCount = 0;
	char* commitHash = nullptr;
	info->GetVersion(&major, &minor);
	if (FAILED(info->GetCommitInfo(&commitCount, &commitHash)) || !commitHash) {
		return fmt::format("{}.{}", major, minor);
	}

	std::string result = fmt::format("{}.{}.{}-{}", major, minor, commitCount, commitHash);
	CoTaskMemFree(commitHash);
	return result;
}

}
