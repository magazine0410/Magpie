#include "ScreenshotFilenameTemplateHelper.h"
#include <algorithm>
#include <cassert>
#include <charconv>
#include <cstdint>
#include <limits>
#include <vector>
#include <fmt/format.h>
#include "StrHelper.h"

namespace Magpie {

enum class TemplateTokenType : uint8_t {
	Character,
	// %WT:nn%: the number of characters can be limited
	WindowTitle,
	// %PN:nn%: the number of characters can be limited
	ProcessName,
	// %Y%
	Year,
	// %y%: the last two digits of the year
	ShortYear,
	// %m%
	Month,
	// %D%
	Day,
	// %H%: 24 hours
	Hour,
	// %M%
	Minite,
	// %S%
	Second,
	// %MS%
	Millisecond
};

struct TemplateToken {
	TemplateTokenType type;
	// Character: a byte of UTF-8
	// WindowTitle and ProcessName: the largest number of characters; zero
	// or less means no limit
	// Not used by the other types
	int8_t value;
};

// Characters that must not appear in file names on Windows, from
// https://learn.microsoft.com/en-us/windows/win32/fileio/naming-a-file#naming-conventions
// Linux only forbids the slash, but the list is kept so that a template is
// valid on both systems and the files can be copied to Windows.
constexpr std::string_view FILENAME_FORBIDDEN_CHARS = "<>:\"/\\|?*";

static TemplateToken GetNextToken(std::string_view& str) noexcept {
	assert(!str.empty());

	const char curChar = str[0];
	if (curChar != '%' || str.size() < 3) {
		// "\%" stands for the character %
		if (curChar == '\\' && str.size() >= 2 && str[1] == '%') {
			str.remove_prefix(2);
			return { TemplateTokenType::Character, '%' };
		} else {
			str.remove_prefix(1);
			return { TemplateTokenType::Character, (int8_t)curChar };
		}
	}

	const char nextChar = str[1];

	if (str[2] == '%') {
		switch (nextChar) {
		case 'Y':
			str.remove_prefix(3);
			return { TemplateTokenType::Year, 0 };
		case 'y':
			str.remove_prefix(3);
			return { TemplateTokenType::ShortYear, 0 };
		case 'm':
			str.remove_prefix(3);
			return { TemplateTokenType::Month, 0 };
		case 'D':
			str.remove_prefix(3);
			return { TemplateTokenType::Day, 0 };
		case 'H':
			str.remove_prefix(3);
			return { TemplateTokenType::Hour, 0 };
		case 'M':
			str.remove_prefix(3);
			return { TemplateTokenType::Minite, 0 };
		case 'S':
			str.remove_prefix(3);
			return { TemplateTokenType::Second, 0 };
		default:
			str.remove_prefix(1);
			return { TemplateTokenType::Character, (int8_t)curChar };
		}
	}

	// %WT[:n]%, %PN[:n]% and %MS%
	if (str.size() >= 4) {
		if (nextChar == 'M') {
			if (str[2] == 'S' && str[3] == '%') {
				str.remove_prefix(4);
				return { TemplateTokenType::Millisecond, 0 };
			}
		} else if ((nextChar == 'W' && str[2] == 'T') || (nextChar == 'P' && str[2] == 'N')) {
			if (str[3] == '%') {
				str.remove_prefix(4);
				return { nextChar == 'W' ? TemplateTokenType::WindowTitle :
					TemplateTokenType::ProcessName, 0 };
			} else if (str[3] == ':') {
				uint8_t num;
				auto result = std::from_chars(str.data() + 4, str.data() + str.size(), num);
				if (result.ptr != str.data() + str.size() && *result.ptr == '%') {
					int8_t limit;
					// Limited to the largest value of int8_t
					if (result.ec == std::errc{}) {
						limit = (int8_t)std::min(num, (uint8_t)std::numeric_limits<int8_t>::max());
					} else if (result.ec == std::errc::result_out_of_range) {
						limit = std::numeric_limits<int8_t>::max();
					} else {
						limit = 0;
					}

					if (limit > 0) {
						str.remove_prefix(result.ptr - str.data() + 1);
						return { nextChar == 'W' ? TemplateTokenType::WindowTitle :
							TemplateTokenType::ProcessName, limit };
					}
				}
			}
		}
	}

	str.remove_prefix(1);
	return { TemplateTokenType::Character, (int8_t)curChar };
}

static bool IsValidUnit(TemplateToken token) noexcept {
	if (token.type == TemplateTokenType::Character) {
		return FILENAME_FORBIDDEN_CHARS.find(token.value) == std::string_view::npos;
	} else {
		return true;
	}
}

bool ScreenshotFilenameTemplateHelper::IsValid(std::string_view templateStr) noexcept {
	assert(!templateStr.empty());

	do {
		if (!IsValidUnit(GetNextToken(templateStr))) {
			return false;
		}
	} while (!templateStr.empty());

	return true;
}

// Replaces the special characters so that the text can be a file name
static void MakeValidFilename(std::string& str) noexcept {
	for (char& c : str) {
		if (FILENAME_FORBIDDEN_CHARS.find(c) != std::string_view::npos) {
			c = '#';
		}
	}
}

// `str` is taken to be valid UTF-8
static std::string_view ExtractUTF8Chars(const std::string& str, int8_t codePointCount) noexcept {
	assert(!str.empty());

	// Zero or less: no limit
	if (codePointCount <= 0) {
		return str;
	}

	int8_t curCodePointCount = 0;
	size_t i = 0;
	while (true) {
		const uint8_t byte = (uint8_t)str[i++];

		// 0xxxxxxx - ASCII
		// 11yyyxxx - first byte of a character of several bytes
		// 10xxxxxx - a following byte
		if ((byte & (uint8_t)0b11000000) == (uint8_t)0b11000000) {
			// Skip the following bytes
			if ((byte & (uint8_t)0b11100000) == (uint8_t)0b11000000) {
				i += 1;
			} else if ((byte & (uint8_t)0b11110000) == (uint8_t)0b11100000) {
				i += 2;
			} else {
				i += 3;
			}
		}

		if (i >= str.size()) {
			// The whole text
			return str;
		}

		if (++curCodePointCount == codePointCount) {
			return std::string_view(str.data(), i);
		}
	}
}

bool ScreenshotFilenameTemplateHelper::Apply(
	std::string_view templateStr,
	std::string_view windowTitleRaw,
	std::string_view processNameRaw,
	const std::tm& localTime,
	int millisecondsValue,
	std::string& result
) noexcept {
	assert(!templateStr.empty());

	std::vector<TemplateToken> tokens;
	do {
		TemplateToken token = GetNextToken(templateStr);
		if (!IsValidUnit(token)) {
			return false;
		}
		tokens.push_back(token);
	} while (!templateStr.empty());

	std::string windowTitle(windowTitleRaw);
	StrHelper::Trim(windowTitle);
	MakeValidFilename(windowTitle);

	std::string processName(processNameRaw);
	StrHelper::Trim(processName);
	MakeValidFilename(processName);

	const std::string year = fmt::format("{}", localTime.tm_year + 1900);
	const std::string_view shortYear = year.size() == 4 ? std::string_view(year).substr(2) : std::string_view(year);

	for (TemplateToken token : tokens) {
		switch (token.type) {
		case TemplateTokenType::Character:
			result.push_back(token.value);
			break;
		case TemplateTokenType::WindowTitle:
			result += windowTitle.empty() ? "[Empty]" : ExtractUTF8Chars(windowTitle, token.value);
			break;
		case TemplateTokenType::ProcessName:
			result += processName.empty() ? "[Unknown]" : ExtractUTF8Chars(processName, token.value);
			break;
		case TemplateTokenType::Year:
			result += year;
			break;
		case TemplateTokenType::ShortYear:
			result += shortYear;
			break;
		case TemplateTokenType::Month:
			result += fmt::format("{:02}", localTime.tm_mon + 1);
			break;
		case TemplateTokenType::Day:
			result += fmt::format("{:02}", localTime.tm_mday);
			break;
		case TemplateTokenType::Hour:
			result += fmt::format("{:02}", localTime.tm_hour);
			break;
		case TemplateTokenType::Minite:
			result += fmt::format("{:02}", localTime.tm_min);
			break;
		case TemplateTokenType::Second:
			result += fmt::format("{:02}", localTime.tm_sec);
			break;
		default:
			assert(token.type == TemplateTokenType::Millisecond);
			result += fmt::format("{:03}", millisecondsValue);
			break;
		}
	}

	assert(!result.empty());
	return true;
}

}
