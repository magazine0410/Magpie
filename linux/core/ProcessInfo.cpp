#include "ProcessInfo.h"
#include <fstream>
#include <fmt/format.h>

namespace Magpie {

std::string ProcessInfo::NameFromCommand(std::string_view command) noexcept {
	// Wine passes Windows paths, so both separators count
	if (const size_t pos = command.find_last_of("/\\"); pos != std::string_view::npos) {
		command.remove_prefix(pos + 1);
	}

	if (command.size() > 4) {
		const std::string_view extension = command.substr(command.size() - 4);
		if (extension == ".exe" || extension == ".EXE") {
			command.remove_suffix(4);
		}
	}
	return std::string(command);
}

std::string ProcessInfo::GetName(uint32_t pid) noexcept {
	if (pid == 0) {
		return {};
	}

	// The arguments are separated by zero bytes
	std::ifstream file(fmt::format("/proc/{}/cmdline", pid), std::ios::binary);
	std::string command;
	std::getline(file, command, '\0');
	if (!command.empty()) {
		return NameFromCommand(command);
	}

	// A process without a command line, for example a kernel thread
	file = std::ifstream(fmt::format("/proc/{}/comm", pid));
	std::getline(file, command);
	return command;
}

}
