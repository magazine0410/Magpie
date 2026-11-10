// The template that names screenshot files. Linux counterpart of
// src/Magpie.Core/include/ScreenshotFilenameTemplateHelper.h.
//
// Tokens: %WT% and %WT:n% (window title, at most n characters), %PN% and
// %PN:n% (program name), %Y% %y% %m% %D% %H% %M% %S% %MS% (date and time),
// and \% for a per cent sign.
//
// The Windows version takes the source window and reads title, program name
// and clock itself. Here the caller passes them in, which also makes the
// result predictable in tests.
#pragma once
#include <ctime>
#include <string>
#include <string_view>

namespace Magpie {

struct ScreenshotFilenameTemplateHelper {
	static bool IsValid(std::string_view templateStr) noexcept;

	// Appends the file name without extension to `result`. Returns false if
	// the template contains a character that file names must not have.
	static bool Apply(
		std::string_view templateStr,
		std::string_view windowTitle,
		std::string_view processName,
		const std::tm& localTime,
		int milliseconds,
		std::string& result
	) noexcept;
};

}
