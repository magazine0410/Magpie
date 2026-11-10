// What is known about a process from its ID.
#pragma once
#include <cstdint>
#include <string>
#include <string_view>

namespace Magpie {

struct ProcessInfo {
	// The name of the program a process runs, without directory: the first
	// word of its command line. For a Windows program under Wine that is the
	// name of the Windows executable, and ".exe" is removed as the Windows
	// version does. Empty if the process cannot be read.
	static std::string GetName(uint32_t pid) noexcept;

	// The same from a command line, of which only the first argument is given
	static std::string NameFromCommand(std::string_view command) noexcept;
};

}
