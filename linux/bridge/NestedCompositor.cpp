#include "NestedCompositor.h"
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <fmt/format.h>
#include "Logger.h"
#include "SwayIpc.h"
#include "WlrInput.h"

extern char** environ;

using namespace std::chrono;

namespace Magpie {

static std::filesystem::path FindProgram(const char* name) noexcept {
	const char* path = std::getenv("PATH");
	if (!path) {
		return {};
	}
	std::string_view rest = path;
	while (!rest.empty()) {
		const size_t end = rest.find(':');
		const std::filesystem::path candidate = std::filesystem::path(rest.substr(0, end)) / name;
		if (access(candidate.c_str(), X_OK) == 0) {
			return candidate;
		}
		rest = end == std::string_view::npos ? std::string_view() : rest.substr(end + 1);
	}
	return {};
}

NestedCompositor::~NestedCompositor() {
	Stop();
}

bool NestedCompositor::IsAvailable() noexcept {
	return !FindProgram("sway").empty();
}

std::string NestedCompositor::ShellQuoted(const std::string& argument) noexcept {
	std::string result = "'";
	for (char c : argument) {
		if (c == '\'') {
			// Out of the quotes, a quote, and in again
			result += "'\\''";
		} else {
			result += c;
		}
	}
	return result + "'";
}

bool NestedCompositor::Start(const Options& options, std::string& error) noexcept {
	Stop();

	const std::filesystem::path sway = FindProgram("sway");
	if (sway.empty()) {
		error = "A nested session needs the program \"sway\", which is not installed.";
		return false;
	}

	std::filesystem::path parent = options.parentDirectory;
	if (parent.empty()) {
		const char* runtimeDir = std::getenv("XDG_RUNTIME_DIR");
		if (!runtimeDir || !*runtimeDir) {
			error = "The session has no runtime directory (XDG_RUNTIME_DIR).";
			return false;
		}
		parent = runtimeDir;
	}
	static std::atomic<uint32_t> counter = 0;
	_directory = parent / fmt::format("magpie-nested-{}-{}", getpid(), ++counter);
	if (mkdir(_directory.c_str(), 0700) != 0) {
		error = "The directory for the nested session cannot be made.";
		_directory.clear();
		return false;
	}

	const std::string socketPath = (_directory / "sway.sock").string();
	const std::filesystem::path displayFile = _directory / "display";
	const std::filesystem::path configFile = _directory / "config";
	const std::filesystem::path logFile = _directory / "sway.log";
	{
		std::ofstream config(configFile);
		config << "# Written by Magpie for a nested session; removed when it ends\n"
			// The program's windows have neither title bar nor border
			<< "default_border none\n"
			<< "default_floating_border none\n"
			// The pointer is where Magpie puts it
			<< "mouse_warping none\n"
			<< "focus_follows_mouse no\n"
			// The name of its Wayland socket is known only to itself
			<< "exec sh -c 'printf %s \"$WAYLAND_DISPLAY\" > " << ShellQuoted(displayFile.string()) << "'\n";
		if (!config) {
			error = "The configuration for the nested session cannot be written.";
			Stop();
			return false;
		}
	}

	// The environment of the Sway and, through it, of the program: that of
	// Magpie, but without a desktop to connect to, so that Sway takes its
	// headless back end
	std::vector<std::string> environment;
	for (char** entry = environ; *entry; ++entry) {
		const std::string_view text = *entry;
		if (text.starts_with("WAYLAND_DISPLAY=") || text.starts_with("DISPLAY=") || text.starts_with("SWAYSOCK=") ||
			text.starts_with("I3SOCK=") || text.starts_with("WLR_BACKENDS=") ||
			text.starts_with("WLR_LIBINPUT_NO_DEVICES=")) {
			continue;
		}
		environment.emplace_back(text);
	}
	environment.push_back("SWAYSOCK=" + socketPath);
	environment.push_back("WLR_BACKENDS=headless");
	environment.push_back("WLR_LIBINPUT_NO_DEVICES=1");
	std::vector<char*> envp;
	for (std::string& entry : environment) {
		envp.push_back(entry.data());
	}
	envp.push_back(nullptr);

	const std::string swayPath = sway.string();
	const std::string configPath = configFile.string();
	const std::string logPath = logFile.string();
	char* const argv[]{ (char*)"sway", (char*)"-c", (char*)configPath.c_str(), nullptr };

	_pid = fork();
	if (_pid == 0) {
		// Only calls that are safe between fork and exec in a program with
		// several threads
		prctl(PR_SET_PDEATHSIG, SIGTERM);
		const int log = open(logPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
		if (log >= 0) {
			dup2(log, STDOUT_FILENO);
			dup2(log, STDERR_FILENO);
		}
		// The signals that the caller holds back for itself
		sigset_t none;
		sigemptyset(&none);
		sigprocmask(SIG_SETMASK, &none, nullptr);
		execve(swayPath.c_str(), argv, envp.data());
		_exit(127);
	}
	if (_pid < 0) {
		error = "The nested session could not be started.";
		Stop();
		return false;
	}

	// Until it has written the name of its socket and answers
	const auto deadline = steady_clock::now() + 10s;
	std::string display;
	while (true) {
		if (display.empty()) {
			std::ifstream file(displayFile);
			std::getline(file, display);
		}
		if (!display.empty()) {
			_bridge = std::make_unique<SwayBridge>();
			if (_bridge->Initialize({ socketPath, display })) {
				break;
			}
			_bridge.reset();
		}
		if (!IsRunning() || steady_clock::now() > deadline) {
			error = "The nested session did not start; its log is " + logPath + ".";
			Logger::Get().Error(error);
			// The log is kept for looking at
			if (_pid > 0) {
				kill(_pid, SIGKILL);
				waitpid(_pid, nullptr, 0);
				_pid = -1;
			}
			_directory.clear();
			return false;
		}
		std::this_thread::sleep_for(20ms);
	}

	if (!SetOutput(options.size, options.refreshRate)) {
		error = "The nested session did not take the size that was asked for.";
		Stop();
		return false;
	}
	// A seat without a pointing device has no pointer for the programs; the
	// virtual one comes into being with its first use
	_bridge->Input().MoveAbsolute({ options.size.width / 2, options.size.height / 2 }, options.size);
	_bridge->Input().Flush();

	Logger::Get().Info(fmt::format("Nested session started: {}x{} at {:.2f} Hz, Wayland socket {}",
		options.size.width, options.size.height, options.refreshRate / 1000.0, display));
	return true;
}

bool NestedCompositor::SetOutput(Size size, int32_t refreshRate) noexcept {
	if (!_bridge || !_bridge->ConfigureOutput(OUTPUT_NAME, size, { 0, 0 }, refreshRate)) {
		return false;
	}
	_size = size;
	_refreshRate = refreshRate;
	return true;
}

bool NestedCompositor::Launch(
	const std::vector<std::string>& command,
	const std::filesystem::path& workingDirectory
) noexcept {
	if (!_bridge || command.empty()) {
		return false;
	}

	// Sway hands the text after "exec" to a shell
	std::string line = "exec ";
	if (!workingDirectory.empty()) {
		line += "cd " + ShellQuoted(workingDirectory.string()) + " && ";
	}
	line += "exec";
	for (const std::string& argument : command) {
		line += ' ' + ShellQuoted(argument);
	}
	Logger::Get().Info("Nested session: " + line);
	return _bridge->Ipc().Command(line);
}

bool NestedCompositor::IsRunning() noexcept {
	if (_pid <= 0) {
		return false;
	}
	int status;
	if (waitpid(_pid, &status, WNOHANG) == _pid) {
		_pid = -1;
		return false;
	}
	return true;
}

void NestedCompositor::Stop() noexcept {
	if (_pid > 0) {
		// Sway ends its clients with it
		if (_bridge) {
			_bridge->Ipc().Command("exit", true);
		}
		const auto deadline = steady_clock::now() + 2s;
		while (IsRunning() && steady_clock::now() < deadline) {
			std::this_thread::sleep_for(10ms);
		}
		if (_pid > 0) {
			kill(_pid, SIGTERM);
			const auto killDeadline = steady_clock::now() + 2s;
			while (IsRunning() && steady_clock::now() < killDeadline) {
				std::this_thread::sleep_for(10ms);
			}
		}
		if (_pid > 0) {
			kill(_pid, SIGKILL);
			waitpid(_pid, nullptr, 0);
			_pid = -1;
		}
		Logger::Get().Info("Nested session ended");
	}
	_bridge.reset();

	if (!_directory.empty()) {
		// The files this class made, one by one, and then the directory,
		// which goes only if nothing else is in it
		std::error_code ec;
		for (const char* name : { "config", "display", "sway.log", "sway.sock" }) {
			std::filesystem::remove(_directory / name, ec);
		}
		std::filesystem::remove(_directory, ec);
		_directory.clear();
	}
}

}
