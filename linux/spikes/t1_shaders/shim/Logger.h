// Stand-in for src/Shared/Logger.h: messages are collected for the report.
#pragma once
#include <string>
#include <string_view>
#include <vector>

namespace Magpie {
class Logger {
public:
	static Logger& Get() noexcept {
		static Logger instance;
		return instance;
	}
	void Info(std::string_view msg) { messages.emplace_back(std::string("info: ").append(msg)); }
	void Warn(std::string_view msg) { messages.emplace_back(std::string("warn: ").append(msg)); }
	void Error(std::string_view msg) { messages.emplace_back(std::string("error: ").append(msg)); }
	void Win32Error(std::string_view msg) { Error(msg); }
	void ComError(std::string_view msg, int) { Error(msg); }

	std::vector<std::string> messages;
};
}
