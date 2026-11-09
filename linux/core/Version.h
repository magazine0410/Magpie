// Linux counterpart of src/Shared/Version.h.
#pragma once
#include <compare>
#include <cstdint>
#include <string>
#include <string_view>

namespace Magpie {

struct Version {
	constexpr Version() {}
	constexpr Version(uint32_t major, uint32_t minor, uint32_t patch)
		: major(major), minor(minor), patch(patch) {}

	// Member-wise comparison in declaration order.
	std::strong_ordering operator<=>(const Version&) const = default;

	// Accepts "major.minor" and "major.minor.patch".
	bool Parse(std::string_view str) noexcept;

	std::string ToString() const noexcept;

	uint32_t major = 0;
	uint32_t minor = 0;
	uint32_t patch = 0;
};

}
