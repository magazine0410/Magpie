// magpie-scale: scales one window to fullscreen until it is told to stop.
// It runs a single scaling session in the foreground, which is what the
// engine service does when a hotkey is pressed, and serves to try and to
// test fullscreen scaling without that service.
#include <chrono>
#include <csignal>
#include <cstdio>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <sys/signalfd.h>
#include <sys/timerfd.h>
#include <thread>
#include <unistd.h>
#include <spdlog/sinks/stdout_sinks.h>
#include "KWinBridge.h"
#include "Logger.h"
#include "Paths.h"
#include "ScalingSession.h"
#include "ToolOptions.h"

using namespace Magpie;
using namespace Magpie::ToolOptions;

static void PrintUsage() {
	std::puts(
		"Usage: magpie-scale (--window QUERY | --active) [options]\n"
		"\n"
		"Scales a window to the whole monitor it is on. While it is scaled the window\n"
		"is on a virtual monitor; mouse and keyboard act on it through the scaled\n"
		"picture. Ctrl+C in the terminal, or SIGTERM, ends scaling and puts the\n"
		"window back.\n"
		"\n"
		"Source:\n"
		"  --window QUERY     a window id, an app ID or a part of a title, as\n"
		"                     magpie-viewer --list-windows prints them\n"
		"  --active           the window that is active when scaling starts\n"
		"  --delay N          wait N seconds before starting, to give time to\n"
		"                     activate the window\n"
		"  --crop L,T,R,B     remove this many pixels from the edges of the source\n"
		"\n"
		"Effects:\n"
		"  --effect SPEC      add an effect; may be given several times. Without\n"
		"                     --effect and --mode: Bilinear,type=fit\n"
		"  --mode NAME        use the effects of scaling mode NAME from the configuration\n"
		"  --config FILE      config.json to read scaling modes from\n"
		"  --effects-dir DIR  directory of the .hlsl files\n"
		"\n"
		"Picture and cursor:\n"
		"  --align POS        lefttop, top, righttop, left, center (default), right,\n"
		"                     leftbottom, bottom or rightbottom\n"
		"  --cursor-scale F   size of the cursor relative to its own (default 1); 0\n"
		"                     scales it like the source\n"
		"  --cursor-bilinear  interpolate the scaled cursor\n"
		"\n"
		"Rendering:\n"
		"  --present-mode M   mailbox (default), fifo or immediate\n"
		"  --max-fps N        frame rate limit\n"
		"  --min-fps N        frames are repeated to keep this rate\n"
		"  --validate         use the Vulkan validation layers\n"
		"\n"
		"Run:\n"
		"  --seconds N        end after N seconds\n"
		"  -v                 print the log");
}

static const char* Describe(ScalingSession::EndReason reason) {
	using enum ScalingSession::EndReason;
	switch (reason) {
	case SourceClosed: return "the source window was closed";
	case SourceMinimized: return "the source window was minimized";
	case SourceResized: return "the source window changed its size";
	case SourceMoved: return "the source window was moved";
	case FocusLost: return "another window on the same monitor became active";
	case CaptureFailed: return "capturing failed";
	case Failed: return "of an error";
	default: return "it was asked to";
	}
}

static const char* Describe(ScalingError error) {
	switch (error) {
	case ScalingError::InvalidScalingMode: return "the scaling mode has no effects";
	case ScalingError::InvalidSourceWindow: return "the window cannot be scaled";
	case ScalingError::Maximized: return "the window is maximized or fullscreen";
	case ScalingError::InvalidCropping: return "too little of the window is left after cropping";
	case ScalingError::CaptureFailed: return "capturing failed";
	default: return "scaling could not be set up";
	}
}

int main(int argc, char* argv[]) {
	ScalingOptions options;
	ScalingSession::Config config;
	config.effectsDir = MAGPIE_DEFAULT_EFFECTS_DIR;
	config.stateFile = Paths::StateDir() / "scaling-state";

	const char* configFile = nullptr;
	const char* modeName = nullptr;
	std::string windowQuery;
	bool useActive = false;
	float delay = 0.0f;
	float seconds = 0.0f;
	bool verbose = false;

	for (int i = 1; i < argc; ++i) {
		const std::string_view arg = argv[i];

		const auto value = [&]() -> const char* {
			if (i + 1 >= argc) {
				std::fprintf(stderr, "%s needs a value.\n", argv[i]);
				std::exit(1);
			}
			return argv[++i];
		};
		const auto invalid = [&](const char* text) {
			std::fprintf(stderr, "Invalid value for %.*s: %s\n", (int)arg.size(), arg.data(), text);
			std::exit(1);
		};
		const auto number = [&](float min) {
			const char* text = value();
			float result = 0.0f;
			if (!ParseFloat(text, result) || result < min) {
				invalid(text);
			}
			return result;
		};

		if (arg == "--window") {
			windowQuery = value();
		} else if (arg == "--active") {
			useActive = true;
		} else if (arg == "--delay") {
			delay = number(0.0f);
		} else if (arg == "--crop") {
			const char* text = value();
			if (!ParseCropping(text, options.cropping)) {
				invalid(text);
			}
		} else if (arg == "--effect") {
			const char* spec = value();
			if (!ParseEffectSpec(spec, options.effects.emplace_back())) {
				invalid(spec);
			}
		} else if (arg == "--mode") {
			modeName = value();
		} else if (arg == "--config") {
			configFile = value();
		} else if (arg == "--effects-dir") {
			config.effectsDir = value();
		} else if (arg == "--align") {
			const char* text = value();
			const std::optional<OutputAlignment> alignment = ParseAlignment(text);
			if (!alignment) {
				invalid(text);
			}
			options.outputAlignment = *alignment;
		} else if (arg == "--cursor-scale") {
			options.cursorScaleFactor = number(0.0f);
		} else if (arg == "--cursor-bilinear") {
			options.cursorInterpolationMode = CursorInterpolationMode::Bilinear;
		} else if (arg == "--present-mode") {
			const std::string_view mode = value();
			if (mode == "fifo") {
				config.presentMode = VK_PRESENT_MODE_FIFO_KHR;
			} else if (mode == "mailbox") {
				config.presentMode = VK_PRESENT_MODE_MAILBOX_KHR;
			} else if (mode == "immediate") {
				config.presentMode = VK_PRESENT_MODE_IMMEDIATE_KHR;
			} else {
				invalid(mode.data());
			}
		} else if (arg == "--max-fps") {
			options.maxFrameRate = number(1.0f);
		} else if (arg == "--min-fps") {
			options.minFrameRate = number(0.0f);
		} else if (arg == "--validate") {
			config.validation = true;
		} else if (arg == "--seconds") {
			seconds = number(0.0f);
		} else if (arg == "-v") {
			verbose = true;
		} else {
			PrintUsage();
			return arg == "-h" || arg == "--help" ? 0 : 1;
		}
	}

	Logger::Get().InitializeWithSink(
		verbose ? spdlog::level::info : spdlog::level::warn,
		std::make_shared<spdlog::sinks::stderr_sink_mt>());

	if (windowQuery.empty() == !useActive) {
		std::fprintf(stderr, "Give either --window or --active.\n");
		return 1;
	}
	if (modeName) {
		if (!options.effects.empty()) {
			std::fprintf(stderr, "--mode and --effect cannot be combined.\n");
			return 1;
		}
		if (!LoadMode(configFile, modeName, options.effects)) {
			return 1;
		}
	}
	if (options.effects.empty()) {
		options.effects.push_back({ .name = "Bilinear", .scalingType = ScalingType::Fit });
	}

	// The signals that end scaling arrive through a descriptor, so that the
	// loop below can wait for them together with everything else
	sigset_t signals;
	sigemptyset(&signals);
	sigaddset(&signals, SIGINT);
	sigaddset(&signals, SIGTERM);
	sigprocmask(SIG_BLOCK, &signals, nullptr);
	const int signalFd = signalfd(-1, &signals, SFD_NONBLOCK | SFD_CLOEXEC);

	KWinBridge bridge;
	if (!bridge.Initialize()) {
		std::fprintf(stderr, "This desktop does not offer KWin's interfaces.\n");
		return 1;
	}
	ScalingSession::RecoverAfterCrash(bridge, config.stateFile);

	if (delay > 0) {
		std::this_thread::sleep_for(std::chrono::duration<float>(delay));
	}

	std::string windowId;
	{
		const std::vector<WindowInfo> windows = bridge.ListWindows();
		if (useActive) {
			windowId = bridge.ActiveWindowId();
			if (windowId.empty()) {
				std::fprintf(stderr, "No window is active.\n");
				return 1;
			}
		} else {
			const std::vector<const WindowInfo*> found = FindWindows(windows, windowQuery);
			if (found.size() != 1) {
				std::fprintf(stderr, "\"%s\" matches %zu windows; magpie-viewer --list-windows prints them.\n",
					windowQuery.c_str(), found.size());
				return 1;
			}
			windowId = found[0]->id;
		}
	}

	ScalingSession session;
	const ScalingError error = session.Start(bridge, windowId, options, config);
	if (error != ScalingError::NoError) {
		std::fprintf(stderr, "Scaling did not start: %s.\n", Describe(error));
		return 1;
	}
	std::printf("Scaling %s\n", session.Describe().c_str());
	std::fflush(stdout);

	int timerFd = -1;
	if (seconds > 0) {
		timerFd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
		const long long nanoseconds = (long long)(seconds * 1e9);
		const itimerspec spec{ {}, { nanoseconds / 1'000'000'000, nanoseconds % 1'000'000'000 } };
		timerfd_settime(timerFd, 0, &spec, nullptr);
	}

	const int wakeFds[2]{ signalFd, timerFd };
	const std::span<const int> wake(wakeFds, timerFd >= 0 ? 2 : 1);
	const auto start = std::chrono::steady_clock::now();

	const char* endText = nullptr;
	while (session.Step(wake)) {
		signalfd_siginfo info;
		uint64_t expirations;
		if (read(signalFd, &info, sizeof(info)) == sizeof(info)) {
			endText = "a signal asked for it";
			break;
		}
		if (timerFd >= 0 && read(timerFd, &expirations, sizeof(expirations)) == sizeof(expirations)) {
			endText = "the time is up";
			break;
		}
	}
	if (!endText) {
		endText = Describe(session.GetEndReason());
	}

	const double runTime = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
	const ScalingSession::Statistics statistics = session.GetStatistics();
	const bool failed = session.GetEndReason() == ScalingSession::EndReason::Failed ||
		session.GetEndReason() == ScalingSession::EndReason::CaptureFailed;
	session.Stop();

	std::printf("Scaling ended after %.1f s because %s.\n", runTime, endText);
	std::printf("Frames:  %llu scaled (%.1f per second), %llu presented (%.1f per second)\n",
		(unsigned long long)statistics.scaledFrames, statistics.scaledFrames / runTime,
		(unsigned long long)statistics.presentedFrames, statistics.presentedFrames / runTime);
	std::printf("Pointer: sent to the source %u times, to another monitor %u times\n",
		statistics.pointerCaptures, statistics.pointerReleases);
	return failed ? 1 : 0;
}
