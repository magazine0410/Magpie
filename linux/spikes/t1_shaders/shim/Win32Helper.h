// Stand-in for the few Win32Helper functions EffectCompiler.cpp calls.
#pragma once
#include <functional>
#include <string>
#include <string_view>

namespace Magpie {
struct Win32Helper {
	static bool ReadTextFile(const wchar_t* fileName, std::string& result) noexcept;
	static bool WriteTextFile(const wchar_t* fileName, std::string_view text) noexcept;
	static bool DirExists(const wchar_t* fileName) noexcept;
	static bool CreateDir(const std::wstring& path, bool recursive = false) noexcept;
	static void RunParallel(std::function<void(uint32_t)> func, uint32_t times) noexcept;
};
}
