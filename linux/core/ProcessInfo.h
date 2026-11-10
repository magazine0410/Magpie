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

	// The path of the program a process runs, by which profiles recognise
	// it: what /proc/PID/exe points to. For a Windows program under Wine
	// that would be Wine's loader for every program, so there it is the
	// first word of the command line, which is the Windows program as it was
	// given to Wine (for example "C:\Games\Game.exe"). Empty if the process
	// cannot be read.
	static std::string GetPath(uint32_t pid) noexcept;

	// The same from the two things read from /proc
	static std::string PathFromProcess(std::string_view executable, std::string_view command) noexcept;
};

}
