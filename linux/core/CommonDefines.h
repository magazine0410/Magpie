// Linux counterpart of src/Shared/CommonDefines.h.
#pragma once
#include <chrono>
#include <cmath>
#include <limits>
#include <string>
#include <string_view>
#include <type_traits>

using namespace std::string_literals;
using namespace std::string_view_literals;
using namespace std::chrono_literals;

// Bit operators for a scoped enum used as a flag set. On Windows this macro
// comes from <winnt.h>.
#define DEFINE_ENUM_FLAG_OPERATORS(E) \
	constexpr E operator|(E a, E b) noexcept { return E(std::underlying_type_t<E>(a) | std::underlying_type_t<E>(b)); } \
	constexpr E operator&(E a, E b) noexcept { return E(std::underlying_type_t<E>(a) & std::underlying_type_t<E>(b)); } \
	constexpr E operator~(E a) noexcept { return E(~std::underlying_type_t<E>(a)); } \
	constexpr E& operator|=(E& a, E b) noexcept { return a = a | b; } \
	constexpr E& operator&=(E& a, E b) noexcept { return a = a & b; }

#define DEFINE_FLAG_ACCESSOR(Name, FlagBit, FlagsVar) \
	bool Name() const noexcept { return (FlagsVar & FlagBit) == FlagBit; } \
	void Name(bool value) noexcept { \
		if (value) { \
			FlagsVar |= FlagBit; \
		} else { \
			FlagsVar &= ~FlagBit; \
		} \
	}

namespace Magpie {

template <typename T>
constexpr inline T FLOAT_EPSILON = std::numeric_limits<T>::epsilon() * 100;

// NaN and infinity are not supported.
template <typename T>
bool IsApprox(T l, T r) noexcept {
	static_assert(std::is_floating_point_v<T>, "T must be a floating point type");
	return std::abs(l - r) < FLOAT_EPSILON<T>;
}

}
