// Linux counterpart of src/Shared/StrHelper.h. All text is UTF-8 in
// std::string, so the UTF-16 conversions and wide-character overloads of the
// Windows version are gone.
#pragma once
#include <cctype>
#include <string>
#include <string_view>
#include "SmallVector.h"

namespace Magpie {

struct StrHelper {
	static bool isspace(char c) noexcept {
		return (bool)std::isspace(static_cast<unsigned char>(c));
	}

	static bool isalpha(char c) noexcept {
		return (bool)std::isalpha(static_cast<unsigned char>(c));
	}

	static bool isdigit(char c) noexcept {
		return (bool)std::isdigit(static_cast<unsigned char>(c));
	}

	static bool isalnum(char c) noexcept {
		return (bool)std::isalnum(static_cast<unsigned char>(c));
	}

	static char toupper(char c) noexcept {
		return (char)std::toupper(static_cast<unsigned char>(c));
	}

	static char tolower(char c) noexcept {
		return (char)std::tolower(static_cast<unsigned char>(c));
	}

	static void Trim(std::string_view& str) noexcept {
		while (!str.empty() && isspace(str.front())) {
			str.remove_prefix(1);
		}
		while (!str.empty() && isspace(str.back())) {
			str.remove_suffix(1);
		}
	}

	static void Trim(std::string& str) noexcept {
		std::string_view sv(str);
		Trim(sv);
		str = std::string(sv);
	}

	template <uint32_t N = CalculateSmallVectorDefaultInlinedElements<std::string_view>::value>
	static SmallVector<std::string_view, N> Split(std::string_view str, char delimiter) noexcept {
		SmallVector<std::string_view, N> result;
		while (!str.empty()) {
			const size_t pos = str.find(delimiter, 0);
			result.push_back(str.substr(0, pos));

			if (pos == std::string_view::npos) {
				return result;
			}
			str.remove_prefix(pos + 1);
		}
		return result;
	}

	static void ToUpperCase(std::string& str) noexcept {
		for (char& c : str) {
			c = toupper(c);
		}
	}

	static std::string ToUpperCase(std::string_view str) noexcept {
		std::string result(str);
		ToUpperCase(result);
		return result;
	}

	static void ToLowerCase(std::string& str) noexcept {
		for (char& c : str) {
			c = tolower(c);
		}
	}

	static std::string ToLowerCase(std::string_view str) noexcept {
		std::string result(str);
		ToLowerCase(result);
		return result;
	}

	// Joins any number of pieces with a single allocation.
	template <typename... Args>
	static std::string Concat(const Args&... args) noexcept {
		std::string result;
		result.reserve((std::string_view(args).size() + ...));
		(result.append(std::string_view(args)), ...);
		return result;
	}
};

}
