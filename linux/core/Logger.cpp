#include "Logger.h"
#include <cassert>
#include <chrono>
#include <system_error>
#include <fmt/format.h>
#include <spdlog/sinks/rotating_file_sink.h>

using namespace std::chrono_literals;

namespace Magpie {

static constexpr const char* LOGGER_NAME = "magpie";

bool Logger::Initialize(
	spdlog::level::level_enum logLevel,
	const std::filesystem::path& logFile,
	size_t archiveAboveSize,
	size_t maxArchiveFiles
) noexcept {
	try {
		return InitializeWithSink(logLevel, std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
			logFile.string(), archiveAboveSize, maxArchiveFiles));
	} catch (const std::exception&) {
		_logger.reset();
		return false;
	}
}

bool Logger::InitializeWithSink(spdlog::level::level_enum logLevel, spdlog::sink_ptr sink) noexcept {
	try {
		_logger = std::make_shared<spdlog::logger>(LOGGER_NAME, std::move(sink));
		_logger->set_level(logLevel);
		_logger->set_pattern("%Y-%m-%d %H:%M:%S.%e|%l|%s:%#|%!|%v");
		_logger->flush_on(spdlog::level::warn);

		// Registered so that the periodic flush applies to it. Dropped first
		// because Initialize may be called again with another file.
		spdlog::drop(LOGGER_NAME);
		spdlog::register_logger(_logger);
#ifdef NDEBUG
		spdlog::flush_every(30s);
#else
		spdlog::flush_every(5s);
#endif
	} catch (const std::exception&) {
		_logger.reset();
		return false;
	}

	return true;
}

void Logger::SetLevel(spdlog::level::level_enum logLevel) noexcept {
	assert(_logger);

	_logger->flush();
	_logger->set_level(logLevel);

	static const char* LOG_LEVELS[7] = {
		"TRACE", "DEBUG", "INFO", "WARN", "ERROR", "CRITICAL", "OFF"
	};
	Info(fmt::format("Log level: {}", LOG_LEVELS[logLevel]));
}

void Logger::ErrnoError(std::string_view msg, int error, const std::source_location& location) noexcept {
	_Log(spdlog::level::err,
		fmt::format("{}\n\terrno: {} ({})", msg, error, std::generic_category().message(error)),
		location);
}

void Logger::_Log(spdlog::level::level_enum logLevel, std::string_view msg, const std::source_location& location) noexcept {
	assert(!msg.empty());

	if (_logger) {
		_logger->log(
			spdlog::source_loc{ location.file_name(), (int)location.line(), location.function_name() },
			logLevel,
			msg
		);
	}
}

}
