// magpie-present-test: shows a moving test pattern, scaled by an effect chain,
// in a plain Wayland window, and reports how evenly the frames were
// presented. It exercises the swap chain, the frame pacing and the GPU timing
// without a capture source. The loop follows Renderer::_BackendThreadProc.
#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include <poll.h>
#include <sys/timerfd.h>
#include <unistd.h>
#include <spdlog/sinks/stdout_sinks.h>
#include <wayland-client.h>
#include <vulkan/vulkan.h>
#include "EffectChain.h"
#include "Logger.h"
#include "ScalingGeometry.h"
#include "StepTimer.h"
#include "Swapchain.h"
#include "VulkanDevice.h"
#include "xdg-decoration-unstable-v1-client-protocol.h"
#include "xdg-shell-client-protocol.h"

using namespace Magpie;
using namespace std::chrono;

static void PrintUsage() {
	std::puts(
		"Usage: magpie-present-test [options]\n"
		"\n"
		"Opens a window, scales a moving test pattern into it and prints frame\n"
		"statistics when it ends.\n"
		"\n"
		"Options:\n"
		"  --effect NAME      effect to scale with (default Bilinear); it is used\n"
		"                     with the scaling type \"fit\"\n"
		"  --source-size WxH  size of the test pattern (default 640x360)\n"
		"  --source-fps N     rate at which the pattern changes; 0 (default) gives a\n"
		"                     new pattern for every presented frame\n"
		"  --window-size WxH  initial size of the window (default 1280x720)\n"
		"  --fullscreen       ask for a fullscreen window\n"
		"  --present-mode M   fifo (default), mailbox or immediate\n"
		"  --max-fps N        frame rate limit\n"
		"  --min-fps N        frames are repeated to keep this rate\n"
		"  --seconds N        run time (default 5)\n"
		"  --effects-dir DIR  directory of the .hlsl files\n"
		"  -v                 print the log");
}

///////////////////////////////////////////////////////////////////////////////
// Window

struct Window {
	wl_display* display = nullptr;
	wl_compositor* compositor = nullptr;
	xdg_wm_base* wmBase = nullptr;
	zxdg_decoration_manager_v1* decorationManager = nullptr;
	wl_surface* surface = nullptr;
	xdg_surface* xdgSurface = nullptr;
	xdg_toplevel* toplevel = nullptr;

	// Size asked for by the compositor; zero means "choose yourself"
	Size configuredSize;
	bool isConfigured = false;
	bool isClosed = false;
};

static void OnRegistryGlobal(void* data, wl_registry* registry, uint32_t name, const char* interface, uint32_t) {
	Window& window = *(Window*)data;
	if (std::strcmp(interface, wl_compositor_interface.name) == 0) {
		window.compositor = (wl_compositor*)wl_registry_bind(registry, name, &wl_compositor_interface, 4);
	} else if (std::strcmp(interface, xdg_wm_base_interface.name) == 0) {
		window.wmBase = (xdg_wm_base*)wl_registry_bind(registry, name, &xdg_wm_base_interface, 1);
	} else if (std::strcmp(interface, zxdg_decoration_manager_v1_interface.name) == 0) {
		window.decorationManager = (zxdg_decoration_manager_v1*)wl_registry_bind(
			registry, name, &zxdg_decoration_manager_v1_interface, 1);
	}
}

static const wl_registry_listener REGISTRY_LISTENER{
	.global = OnRegistryGlobal,
	.global_remove = [](void*, wl_registry*, uint32_t) {}
};

static const xdg_wm_base_listener WM_BASE_LISTENER{
	.ping = [](void*, xdg_wm_base* wmBase, uint32_t serial) { xdg_wm_base_pong(wmBase, serial); }
};

static const xdg_surface_listener XDG_SURFACE_LISTENER{
	.configure = [](void* data, xdg_surface* xdgSurface, uint32_t serial) {
		xdg_surface_ack_configure(xdgSurface, serial);
		((Window*)data)->isConfigured = true;
	}
};

static const xdg_toplevel_listener TOPLEVEL_LISTENER{
	.configure = [](void* data, xdg_toplevel*, int32_t width, int32_t height, wl_array*) {
		((Window*)data)->configuredSize = { width, height };
	},
	.close = [](void* data, xdg_toplevel*) { ((Window*)data)->isClosed = true; }
};

static bool CreateWindow(Window& window, bool fullscreen) {
	window.display = wl_display_connect(nullptr);
	if (!window.display) {
		std::fprintf(stderr, "Cannot connect to a Wayland compositor.\n");
		return false;
	}

	wl_registry* registry = wl_display_get_registry(window.display);
	wl_registry_add_listener(registry, &REGISTRY_LISTENER, &window);
	wl_display_roundtrip(window.display);
	if (!window.compositor || !window.wmBase) {
		std::fprintf(stderr, "The compositor lacks wl_compositor or xdg_wm_base.\n");
		return false;
	}
	xdg_wm_base_add_listener(window.wmBase, &WM_BASE_LISTENER, nullptr);

	window.surface = wl_compositor_create_surface(window.compositor);
	window.xdgSurface = xdg_wm_base_get_xdg_surface(window.wmBase, window.surface);
	xdg_surface_add_listener(window.xdgSurface, &XDG_SURFACE_LISTENER, &window);
	window.toplevel = xdg_surface_get_toplevel(window.xdgSurface);
	xdg_toplevel_add_listener(window.toplevel, &TOPLEVEL_LISTENER, &window);
	xdg_toplevel_set_title(window.toplevel, "Magpie present test");
	xdg_toplevel_set_app_id(window.toplevel, "magpie-present-test");
	if (fullscreen) {
		xdg_toplevel_set_fullscreen(window.toplevel, nullptr);
	}
	if (window.decorationManager) {
		// Ask the compositor to draw the title bar
		zxdg_toplevel_decoration_v1* decoration = zxdg_decoration_manager_v1_get_toplevel_decoration(
			window.decorationManager, window.toplevel);
		zxdg_toplevel_decoration_v1_set_mode(decoration, ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
	}
	wl_surface_commit(window.surface);

	while (!window.isConfigured) {
		if (wl_display_dispatch(window.display) < 0) {
			return false;
		}
	}
	return true;
}

// Processes the events that have arrived without waiting for more
static bool PumpEvents(wl_display* display) {
	while (wl_display_prepare_read(display) != 0) {
		wl_display_dispatch_pending(display);
	}
	wl_display_flush(display);

	pollfd fd{ wl_display_get_fd(display), POLLIN, 0 };
	if (poll(&fd, 1, 0) > 0) {
		wl_display_read_events(display);
	} else {
		wl_display_cancel_read(display);
	}
	return wl_display_dispatch_pending(display) >= 0;
}

///////////////////////////////////////////////////////////////////////////////
// Test pattern

// A set of frames that differ in the position of a bar and of a marker, so
// that motion is visible and no two successive frames are equal.
static constexpr uint32_t PATTERN_FRAME_COUNT = 16;

static std::vector<uint8_t> MakePattern(Size size, uint32_t index) {
	std::vector<uint8_t> pixels((size_t)size.width * size.height * 4);
	const int32_t barWidth = std::max(size.width / 40, 2);
	const int32_t barLeft = int32_t((int64_t)(size.width - barWidth) * index / (PATTERN_FRAME_COUNT - 1));
	const int32_t cell = std::max(size.height / 18, 4);

	for (int32_t y = 0; y < size.height; ++y) {
		for (int32_t x = 0; x < size.width; ++x) {
			uint8_t* pixel = &pixels[((size_t)y * size.width + x) * 4];
			// Checkerboard with a colour gradient
			const bool dark = ((x / cell) + (y / cell)) % 2;
			pixel[0] = uint8_t(dark ? 40 : 60 + 150 * x / size.width);
			pixel[1] = uint8_t(dark ? 44 : 60 + 150 * y / size.height);
			pixel[2] = uint8_t(dark ? 52 : 200);
			pixel[3] = 255;

			if (x >= barLeft && x < barLeft + barWidth) {
				pixel[0] = pixel[1] = pixel[2] = 255;
			}
			// One lit cell in the top row per frame
			if (y < cell && x / cell == (int32_t)index) {
				pixel[0] = 255;
				pixel[1] = 200;
				pixel[2] = 0;
			}
		}
	}
	return pixels;
}

///////////////////////////////////////////////////////////////////////////////

static bool ParseSize(std::string_view text, Size& size) {
	const size_t delim = text.find('x');
	if (delim == std::string_view::npos) {
		return false;
	}
	const auto parse = [](std::string_view part, int32_t& value) {
		const auto [end, ec] = std::from_chars(part.data(), part.data() + part.size(), value);
		return ec == std::errc() && end == part.data() + part.size() && value > 0;
	};
	return parse(text.substr(0, delim), size.width) && parse(text.substr(delim + 1), size.height);
}

int main(int argc, char* argv[]) {
	std::string effectName = "Bilinear";
	std::filesystem::path effectsDir = MAGPIE_DEFAULT_EFFECTS_DIR;
	Size sourceSize{ 640, 360 };
	Size windowSize{ 1280, 720 };
	float sourceFps = 0.0f;
	float minFps = 0.0f;
	std::optional<float> maxFps;
	float seconds = 5.0f;
	bool fullscreen = false;
	bool verbose = false;
	VkPresentModeKHR presentMode = VK_PRESENT_MODE_FIFO_KHR;

	for (int i = 1; i < argc; ++i) {
		const std::string_view arg = argv[i];
		const auto value = [&]() -> std::string_view {
			if (i + 1 >= argc) {
				std::fprintf(stderr, "%s needs a value.\n", argv[i]);
				std::exit(1);
			}
			return argv[++i];
		};
		const auto number = [&]() {
			const std::string_view text = value();
			float result = 0.0f;
			const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), result);
			if (ec != std::errc() || end != text.data() + text.size() || result < 0) {
				std::fprintf(stderr, "Invalid number: %.*s\n", (int)text.size(), text.data());
				std::exit(1);
			}
			return result;
		};
		const auto size = [&](Size& result) {
			const std::string_view text = value();
			if (!ParseSize(text, result)) {
				std::fprintf(stderr, "Invalid size: %.*s\n", (int)text.size(), text.data());
				std::exit(1);
			}
		};

		if (arg == "--effect") {
			effectName = value();
			std::replace(effectName.begin(), effectName.end(), '/', '\\');
		} else if (arg == "--effects-dir") {
			effectsDir = value();
		} else if (arg == "--source-size") {
			size(sourceSize);
		} else if (arg == "--window-size") {
			size(windowSize);
		} else if (arg == "--source-fps") {
			sourceFps = number();
		} else if (arg == "--max-fps") {
			maxFps = number();
		} else if (arg == "--min-fps") {
			minFps = number();
		} else if (arg == "--seconds") {
			seconds = number();
		} else if (arg == "--fullscreen") {
			fullscreen = true;
		} else if (arg == "--present-mode") {
			const std::string_view mode = value();
			if (mode == "fifo") {
				presentMode = VK_PRESENT_MODE_FIFO_KHR;
			} else if (mode == "mailbox") {
				presentMode = VK_PRESENT_MODE_MAILBOX_KHR;
			} else if (mode == "immediate") {
				presentMode = VK_PRESENT_MODE_IMMEDIATE_KHR;
			} else {
				std::fprintf(stderr, "Unknown present mode.\n");
				return 1;
			}
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

	Window window;
	if (!CreateWindow(window, fullscreen)) {
		return 1;
	}
	if (window.configuredSize.width > 0 && window.configuredSize.height > 0) {
		windowSize = window.configuredSize;
	}

	// Everything that uses the device is declared after it, so that it is
	// destroyed first.
	VulkanDevice device;
	if (!device.Initialize({ .presentation = true })) {
		return 1;
	}

	VkSurfaceKHR surface = VK_NULL_HANDLE;
	const VkWaylandSurfaceCreateInfoKHR surfaceInfo{
		.sType = VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR,
		.display = window.display,
		.surface = window.surface
	};
	if (vkCreateWaylandSurfaceKHR(device.Instance(), &surfaceInfo, nullptr, &surface) != VK_SUCCESS) {
		std::fprintf(stderr, "vkCreateWaylandSurfaceKHR failed.\n");
		return 1;
	}

	Swapchain swapchain;
	if (!swapchain.Initialize(device, surface, windowSize, presentMode)) {
		return 1;
	}

	// The source frames
	std::vector<std::shared_ptr<Texture>> patterns;
	for (uint32_t i = 0; i < PATTERN_FRAME_COUNT; ++i) {
		std::shared_ptr<Texture> texture = device.CreateTexture(VK_FORMAT_R8G8B8A8_UNORM,
			(uint32_t)sourceSize.width, (uint32_t)sourceSize.height,
			VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
		if (!texture || !device.UploadTexture(*texture, MakePattern(sourceSize, i), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)) {
			return 1;
		}
		patterns.push_back(std::move(texture));
	}

	ScalingOptions options;
	options.effects.push_back({ .name = effectName, .scalingType = ScalingType::Fit });

	EffectChain chain;
	if (!chain.Initialize(device, options, { effectsDir, swapchain.GetSize() }, patterns[0])) {
		return 1;
	}
	chain.Profiler().Start(device, chain.PassCount());

	// A timer stands in for the capture source's "new frame" signal
	int sourceTimer = -1;
	if (sourceFps > 0) {
		sourceTimer = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK);
		const long interval = long(1e9 / sourceFps);
		const itimerspec spec{ { interval / 1'000'000'000, interval % 1'000'000'000 }, { 0, 1 } };
		timerfd_settime(sourceTimer, 0, &spec, nullptr);
	}

	StepTimer stepTimer;
	stepTimer.Initialize(minFps, maxFps);
	const int wakeFds[2]{ wl_display_get_fd(window.display), sourceTimer };
	stepTimer.SetWakeDescriptors(std::span(wakeFds, sourceTimer >= 0 ? 2 : 1));
	const bool waitForWake = sourceTimer >= 0;

	std::vector<float> frameTimes;	// when each frame had been presented, in ms
	std::vector<double> passTimes(chain.PassCount());
	uint32_t timedFrames = 0;
	uint32_t patternIndex = 0;
	uint32_t resizeCount = 0;

	const auto start = steady_clock::now();
	const auto elapsedMs = [&] {
		return duration<float, std::milli>(steady_clock::now() - start).count();
	};

	StepTimerStatus status = StepTimerStatus::WaitingForNewFrame;
	bool failed = false;
	while (!window.isClosed && !failed && elapsedMs() < seconds * 1000) {
		bool fpsUpdated = false;
		status = stepTimer.WaitForNextFrame(
			waitForWake && status != StepTimerStatus::WaitingForFPSLimiter, fpsUpdated);

		if (!PumpEvents(window.display)) {
			break;
		}

		if (status == StepTimerStatus::WaitingForFPSLimiter) {
			continue;
		}

		// The frame source: is there a new frame?
		bool isNewFrame = true;
		if (sourceTimer >= 0) {
			uint64_t expirations = 0;
			isNewFrame = read(sourceTimer, &expirations, sizeof(expirations)) == sizeof(expirations);
		}
		if (isNewFrame) {
			patternIndex = (patternIndex + 1) % PATTERN_FRAME_COUNT;
		} else if (status != StepTimerStatus::ForceNewFrame) {
			continue;
		}

		// The window was resized
		const Size wantedSize = window.configuredSize.width > 0 && window.configuredSize.height > 0
			? window.configuredSize : swapchain.GetSize();
		if (wantedSize != swapchain.GetSize()) {
			if (!swapchain.Resize(wantedSize)) {
				break;
			}
			++resizeCount;
		}
		// The chain follows the size of the swap chain and the current frame
		if (!chain.Resize(swapchain.GetSize(), patterns[patternIndex])) {
			break;
		}

		stepTimer.PrepareForRender();

		const Swapchain::FrameResult result = swapchain.PresentFrame([&](VkCommandBuffer cmd, VkImage image) {
			chain.Draw(cmd, stepTimer.GetFrameCount());

			const Texture& output = *chain.Output();
			const Rect destRect = ScalingGeometry::CalcDestRect(swapchain.GetSize(),
				{ (int32_t)output.width, (int32_t)output.height }, options.outputAlignment);
			Swapchain::Compose(cmd, output, image, swapchain.GetSize(), destRect);
		});

		if (result == Swapchain::FrameResult::NeedsResize) {
			if (!swapchain.Resize(wantedSize)) {
				break;
			}
			++resizeCount;
			continue;
		}
		if (result == Swapchain::FrameResult::Failed) {
			failed = true;
			break;
		}

		frameTimes.push_back(elapsedMs());

		chain.Profiler().QueryTimings();
		const SmallVector<float> timings = chain.Profiler().GetTimings();
		if (timings.size() == passTimes.size()) {
			for (size_t i = 0; i < timings.size(); ++i) {
				passTimes[i] += timings[i];
			}
			++timedFrames;
		}
	}

	if (sourceTimer >= 0) {
		close(sourceTimer);
	}

	///////////////////////////////////////////////////////////////////////////
	// Report

	static constexpr const char* MODE_NAMES[] = { "immediate", "mailbox", "fifo", "fifo relaxed" };
	const auto modeName = [](VkPresentModeKHR mode) {
		return (uint32_t)mode < std::size(MODE_NAMES) ? MODE_NAMES[mode] : "other";
	};

	std::printf("Graphics card:   %s\n", device.GraphicsCards()[device.GraphicsCardIdx()].name.c_str());
	std::printf("Window:          %dx%d, %s", swapchain.GetSize().width, swapchain.GetSize().height,
		swapchain.Format() == VK_FORMAT_B8G8R8A8_UNORM ? "B8G8R8A8" : "R8G8B8A8");
	if (resizeCount > 0) {
		std::printf(", resized %u times", resizeCount);
	}
	std::printf("\nPresent mode:    %s (available:", modeName(swapchain.PresentMode()));
	for (VkPresentModeKHR mode : swapchain.AvailablePresentModes()) {
		std::printf(" %s", modeName(mode));
	}
	std::printf(")\nSource:          %dx%d, %s, scaled with %s to %ux%u\n", sourceSize.width, sourceSize.height,
		sourceFps > 0 ? (std::to_string((int)sourceFps) + " frames per second").c_str() : "a new frame for every presented one",
		effectName.c_str(), chain.Output()->width, chain.Output()->height);

	if (frameTimes.size() < 3) {
		std::printf("Too few frames were presented.\n");
		return 1;
	}

	// The first frames include start-up work
	const size_t skip = std::min<size_t>(frameTimes.size() / 10, 20);
	std::vector<float> intervals;
	for (size_t i = skip + 1; i < frameTimes.size(); ++i) {
		intervals.push_back(frameTimes[i] - frameTimes[i - 1]);
	}
	std::vector<float> sorted = intervals;
	std::sort(sorted.begin(), sorted.end());
	const auto percentile = [&](double p) {
		return sorted[std::min(sorted.size() - 1, size_t(p * sorted.size()))];
	};
	const float median = percentile(0.5);
	const size_t late = std::count_if(intervals.begin(), intervals.end(), [&](float interval) {
		return interval > median * 1.5f;
	});
	const float measuredSeconds = (frameTimes.back() - frameTimes[skip]) / 1000.0f;

	std::printf("Frames:          %zu in %.1f s, %.1f per second\n",
		frameTimes.size(), frameTimes.back() / 1000.0f, intervals.size() / measuredSeconds);
	std::printf("Frame interval:  median %.2f ms, 99%% below %.2f ms, longest %.2f ms\n",
		median, percentile(0.99), sorted.back());
	std::printf("Late frames:     %zu of %zu (interval above 1.5 times the median)\n", late, intervals.size());

	if (timedFrames > 0) {
		double total = 0.0;
		for (double time : passTimes) {
			total += time / timedFrames;
		}
		std::printf("GPU time:        %.3f ms per frame in %zu passes\n", total, passTimes.size());
	}

	return failed ? 1 : 0;
}
