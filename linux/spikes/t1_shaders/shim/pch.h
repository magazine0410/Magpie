// Stand-in for src/Magpie.Core/pch.h so that the unmodified EffectCompiler.cpp
// builds on Linux. Only what EffectCompiler.cpp needs is provided.
#pragma once

#include <algorithm>
#include <array>
#include <cassert>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <format>
#include <functional>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <variant>
#include <vector>

// ---- Win32 scalar types -----------------------------------------------------
using HRESULT = int32_t;
using UINT = unsigned int;
using LPCSTR = const char*;
using LPCVOID = const void*;
#define S_OK ((HRESULT)0)
#define E_FAIL ((HRESULT)0x80004005)
#define CALLBACK

// ---- <winnt.h> --------------------------------------------------------------
#define DEFINE_ENUM_FLAG_OPERATORS(E) \
	constexpr E operator|(E a, E b) noexcept { return E(std::underlying_type_t<E>(a) | std::underlying_type_t<E>(b)); } \
	constexpr E operator&(E a, E b) noexcept { return E(std::underlying_type_t<E>(a) & std::underlying_type_t<E>(b)); } \
	constexpr E operator~(E a) noexcept { return E(~std::underlying_type_t<E>(a)); } \
	constexpr E& operator|=(E& a, E b) noexcept { return a = a | b; } \
	constexpr E& operator&=(E& a, E b) noexcept { return a = a & b; }

// ---- D3D --------------------------------------------------------------------
enum D3D_INCLUDE_TYPE { D3D_INCLUDE_LOCAL, D3D_INCLUDE_SYSTEM };

struct ID3DInclude {
	virtual HRESULT Open(D3D_INCLUDE_TYPE, LPCSTR, LPCVOID, LPCVOID*, UINT*) noexcept = 0;
	virtual HRESULT Close(LPCVOID) noexcept = 0;
};

struct ID3DBlob {};

// ---- C++/WinRT --------------------------------------------------------------
namespace winrt {
template <typename T>
struct com_ptr {
	com_ptr() = default;
	com_ptr(std::nullptr_t) {}
	T** put() noexcept { return &_p; }
	T* get() const noexcept { return _p; }
	explicit operator bool() const noexcept { return _p != nullptr; }
private:
	T* _p = nullptr;
};
}

// ---- fmt --------------------------------------------------------------------
namespace fmt {
using std::format;
}

#include "CommonDefines.h"
