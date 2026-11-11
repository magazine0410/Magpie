// A compositor of Magpie's own for one program: a Sway that shows nothing and
// has no input devices, started as a child process. The program is started
// inside it, its picture is taken from the Sway's one output, and keyboard
// and pointer are handed to it from the window that shows the scaled
// picture. This is the nested session, strategy C of the plan: it needs
// nothing from the desktop but a place for an ordinary window, so it works
// where no back end exists.
//
// The Sway is driven like the one of a desktop, through SwayBridge.
#pragma once
#include <filesystem>
#include <memory>
#include <string>
#include <sys/types.h>
#include <vector>
#include "Geometry.h"
#include "SwayBridge.h"

namespace Magpie {

class NestedCompositor {
public:
	// The name Sway gives its first output without a screen
	static constexpr const char* OUTPUT_NAME = "HEADLESS-1";

	struct Options {
		// The size of the output, which is the size the program is scaled
		// from, and its refresh rate in thousandths of a hertz
		Size size{ 1280, 720 };
		int32_t refreshRate = 60'000;
		// Where the directory with its socket and its files is made; empty
		// for the runtime directory of the session
		std::filesystem::path parentDirectory;
	};

	NestedCompositor() = default;
	NestedCompositor(const NestedCompositor&) = delete;
	NestedCompositor(NestedCompositor&&) = delete;

	// Ends it, and with it the programs inside
	~NestedCompositor();

	// Whether there is a program "sway" to start
	static bool IsAvailable() noexcept;

	// Returns false with a message for the user in `error`
	bool Start(const Options& options, std::string& error) noexcept;

	// Starts a program inside. The first entry of `command` is the program,
	// the others are its arguments.
	bool Launch(const std::vector<std::string>& command, const std::filesystem::path& workingDirectory = {}) noexcept;

	// Gives the output another size or rate
	bool SetOutput(Size size, int32_t refreshRate) noexcept;

	Size OutputSize() const noexcept {
		return _size;
	}

	int32_t RefreshRate() const noexcept {
		return _refreshRate;
	}

	// The Sway is still there
	bool IsRunning() noexcept;

	void Stop() noexcept;

	// Valid between Start and Stop
	SwayBridge& Bridge() noexcept {
		return *_bridge;
	}

	// One argument the way a shell takes it literally
	static std::string ShellQuoted(const std::string& argument) noexcept;

private:
	pid_t _pid = -1;
	std::filesystem::path _directory;
	std::unique_ptr<SwayBridge> _bridge;
	Size _size;
	int32_t _refreshRate = 0;
};

}
