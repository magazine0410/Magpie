#include "Version.h"
#include <charconv>
#include <fmt/format.h>
#include "StrHelper.h"

namespace Magpie {

static bool ParseNumber(std::string_view str, uint32_t& value) noexcept {
	const char* end = str.data() + str.size();
	const auto result = std::from_chars(str.data(), end, value);
	return result.ec == std::errc{} && result.ptr == end;
}

bool Version::Parse(std::string_view str) noexcept {
	if (str.empty()) {
		return false;
	}

	const SmallVector<std::string_view> numbers = StrHelper::Split(str, '.');
	const size_t size = numbers.size();
	if (size != 2 && size != 3) {
		return false;
	}

	if (!ParseNumber(numbers[0], major) || !ParseNumber(numbers[1], minor)) {
		return false;
	}

	if (size == 3 && !ParseNumber(numbers[2], patch)) {
		return false;
	}

	return true;
}

std::string Version::ToString() const noexcept {
	return fmt::format("{}.{}.{}", major, minor, patch);
}

}
