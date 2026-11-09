#include <catch2/catch_test_macros.hpp>
#include <cerrno>
#include "Logger.h"
#include "TestHelpers.h"

using Magpie::Logger;

TEST_CASE("Logger writes level, source location and message to the file", "[Logger]") {
	TempDir dir;
	const std::filesystem::path logFile = dir.Path() / "logs" / "magpie.log";

	REQUIRE(Logger::Get().Initialize(spdlog::level::info, logFile, 100000, 1));
	Logger::Get().Info("first message");
	Logger::Get().Warn("second message");
	Logger::Get().ErrnoError("open failed", ENOENT);
	Logger::Get().Flush();

	const std::string content = ReadFile(logFile);
	CHECK(content.find("|info|") != std::string::npos);
	CHECK(content.find("first message") != std::string::npos);
	CHECK(content.find("|warning|") != std::string::npos);
	CHECK(content.find("TestLogger.cpp") != std::string::npos);
	CHECK(content.find("open failed") != std::string::npos);
	CHECK(content.find("errno: 2") != std::string::npos);
}

TEST_CASE("Messages below the level are left out", "[Logger]") {
	TempDir dir;
	const std::filesystem::path logFile = dir.Path() / "magpie.log";

	REQUIRE(Logger::Get().Initialize(spdlog::level::warn, logFile, 100000, 1));
	Logger::Get().Info("not written");
	Logger::Get().Error("written");
	Logger::Get().Flush();

	const std::string content = ReadFile(logFile);
	CHECK(content.find("not written") == std::string::npos);
	CHECK(content.find("written") != std::string::npos);
}

TEST_CASE("Initialize fails if the log file cannot be created", "[Logger]") {
	TempDir dir;
	// A regular file where a directory is needed
	const std::filesystem::path blocker = dir.Path() / "blocker";
	std::ofstream(blocker) << "x";

	CHECK_FALSE(Logger::Get().Initialize(spdlog::level::info, blocker / "magpie.log", 100000, 1));
	// Logging without a file must not crash
	Logger::Get().Error("discarded");
}
