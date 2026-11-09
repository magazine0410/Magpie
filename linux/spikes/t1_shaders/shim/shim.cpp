#include "pch.h"
#include "shim.h"
#include "DirectXHelper.h"
#include "StrHelper.h"
#include "Win32Helper.h"
#include <fstream>
#include <sstream>

namespace Spike {

std::filesystem::path effectsRoot;
std::vector<DumpedPass> dumpedPasses;

// "effects\Anime4K\x.hlsl" -> <effectsRoot>/Anime4K/x.hlsl
static std::filesystem::path MapPath(const wchar_t* fileName) {
	std::string path = Magpie::StrHelper::UTF16ToUTF8(fileName);
	std::replace(path.begin(), path.end(), '\\', '/');

	static constexpr std::string_view PREFIX = "effects/";
	if (path.starts_with(PREFIX)) {
		return effectsRoot / path.substr(PREFIX.size());
	}
	return path;
}

}

namespace Magpie {

// wchar_t is UTF-32 on Linux. The names are kept so the upstream code compiles.
std::wstring StrHelper::UTF8ToUTF16(std::string_view str) noexcept {
	std::wstring result;
	result.reserve(str.size());

	for (size_t i = 0; i < str.size();) {
		const unsigned char c = (unsigned char)str[i];
		uint32_t cp;
		size_t len;
		if (c < 0x80) {
			cp = c; len = 1;
		} else if ((c >> 5) == 0x6) {
			cp = c & 0x1F; len = 2;
		} else if ((c >> 4) == 0xE) {
			cp = c & 0x0F; len = 3;
		} else {
			cp = c & 0x07; len = 4;
		}

		for (size_t j = 1; j < len && i + j < str.size(); ++j) {
			cp = (cp << 6) | ((unsigned char)str[i + j] & 0x3F);
		}

		result.push_back((wchar_t)cp);
		i += len;
	}

	return result;
}

std::string StrHelper::UTF16ToUTF8(std::wstring_view str) noexcept {
	std::string result;
	result.reserve(str.size());

	for (wchar_t wc : str) {
		const uint32_t cp = (uint32_t)wc;
		if (cp < 0x80) {
			result.push_back((char)cp);
		} else if (cp < 0x800) {
			result.push_back((char)(0xC0 | (cp >> 6)));
			result.push_back((char)(0x80 | (cp & 0x3F)));
		} else if (cp < 0x10000) {
			result.push_back((char)(0xE0 | (cp >> 12)));
			result.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
			result.push_back((char)(0x80 | (cp & 0x3F)));
		} else {
			result.push_back((char)(0xF0 | (cp >> 18)));
			result.push_back((char)(0x80 | ((cp >> 12) & 0x3F)));
			result.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
			result.push_back((char)(0x80 | (cp & 0x3F)));
		}
	}

	return result;
}

bool Win32Helper::ReadTextFile(const wchar_t* fileName, std::string& result) noexcept {
	std::ifstream file(Spike::MapPath(fileName), std::ios::binary);
	if (!file) {
		return false;
	}

	std::ostringstream ss;
	ss << file.rdbuf();
	result = std::move(ss).str();
	return true;
}

bool Win32Helper::WriteTextFile(const wchar_t*, std::string_view) noexcept {
	return true;
}

bool Win32Helper::DirExists(const wchar_t*) noexcept {
	return true;
}

bool Win32Helper::CreateDir(const std::wstring&, bool) noexcept {
	return true;
}

void Win32Helper::RunParallel(std::function<void(uint32_t)> func, uint32_t times) noexcept {
	for (uint32_t i = 0; i < times; ++i) {
		func(i);
	}
}

bool DirectXHelper::CompileComputeShader(
	std::string_view hlsl,
	const char* /*entryPoint*/,
	ID3DBlob** blob,
	const char* sourceName,
	ID3DInclude* /*include*/,
	const std::vector<std::pair<std::string, std::string>>& macros,
	bool /*warningsAreErrors*/
) {
	Spike::dumpedPasses.push_back({ sourceName ? sourceName : "", std::string(hlsl), macros });

	// EffectCompiler only checks that the blob is not null.
	static ID3DBlob dummy;
	*blob = &dummy;
	return true;
}

}
