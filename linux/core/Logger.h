// Linux counterpart of src/Shared/Logger.h: a rotating log file through
// spdlog. The Win32, NTSTATUS and HRESULT variants are replaced by one variant
// for errno.
#pragma once
#include <cerrno>
#include <filesystem>
#include <memory>
#include <source_location>
#include <string_view>
#include <spdlog/spdlog.h>

namespace Magpie {

class Logger {
public:
	Logger(const Logger&) = delete;
	Logger(Logger&&) = delete;

	static Logger& Get() noexcept {
		static Logger instance;
		return instance;
	}

	// Until this succeeds, messages are discarded.
	bool Initialize(
		spdlog::level::level_enum logLevel,
		const std::filesystem::path& logFile,
		size_t archiveAboveSize,
		size_t maxArchiveFiles
	) noexcept;

	void SetLevel(spdlog::level::level_enum logLevel) noexcept;

	void Flush() noexcept {
		if (_logger) {
			_logger->flush();
		}
	}

	void Info(std::string_view msg, const std::source_location& location = std::source_location::current()) noexcept {
		_Log(spdlog::level::info, msg, location);
	}

	void Warn(std::string_view msg, const std::source_location& location = std::source_location::current()) noexcept {
		_Log(spdlog::level::warn, msg, location);
	}

	void Error(std::string_view msg, const std::source_location& location = std::source_location::current()) noexcept {
		_Log(spdlog::level::err, msg, location);
	}

	// Appends the text and number of a system error code. Pass errno at the
	// call site; it is the default so that the value is read before any
	// other call can overwrite it.
	void ErrnoError(
		std::string_view msg,
		int error = errno,
		const std::source_location& location = std::source_location::current()
	) noexcept;

private:
	Logger() = default;

	void _Log(spdlog::level::level_enum logLevel, std::string_view msg, const std::source_location& location) noexcept;

	std::shared_ptr<spdlog::logger> _logger;
};

}
