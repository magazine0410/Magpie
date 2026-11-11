// The test window as a program of its own, for tests that need a window of
// another process, or a program to start in a nested session. It prints what
// the pointer and the keyboard do on it, one event per line.
//
// Usage: magpie_test_window [--display NAME] [--title TEXT] [--app-id ID]
//        [--size WxH] [--resizable] [--hide-cursor] [--duration SECONDS]
#include <cstdio>
#include <cstdlib>
#include <string_view>
#include "SwayTestHelpers.h"

using namespace SwayTest;

int main(int argc, char** argv) {
	TestWindowOptions options;
	if (const char* display = std::getenv("WAYLAND_DISPLAY")) {
		options.display = display;
	}
	double duration = 0.0;

	for (int i = 1; i < argc; ++i) {
		const std::string_view arg = argv[i];
		const auto value = [&]() -> const char* {
			if (i + 1 >= argc) {
				std::fprintf(stderr, "%s needs a value\n", argv[i]);
				std::exit(2);
			}
			return argv[++i];
		};
		if (arg == "--display") {
			options.display = value();
		} else if (arg == "--title") {
			options.title = value();
		} else if (arg == "--app-id") {
			options.appId = value();
		} else if (arg == "--size") {
			if (std::sscanf(value(), "%dx%d", &options.size.width, &options.size.height) != 2) {
				std::fprintf(stderr, "--size takes WIDTHxHEIGHT\n");
				return 2;
			}
		} else if (arg == "--resizable") {
			options.isFixedSize = false;
		} else if (arg == "--hide-cursor") {
			options.hidesCursor = true;
		} else if (arg == "--duration") {
			duration = std::atof(value());
		} else {
			std::fprintf(stderr, "Unknown option %s\n", argv[i]);
			return 2;
		}
	}

	TestWindow window(options);
	if (!window.WaitUntilShown()) {
		std::fprintf(stderr, "The window did not appear\n");
		return 1;
	}
	const Size size = window.GetSize();
	std::printf("shown %dx%d\n", size.width, size.height);
	std::fflush(stdout);

	const auto end = std::chrono::steady_clock::now() + std::chrono::duration<double>(duration);
	while (!window.IsClosed() && (duration <= 0.0 || std::chrono::steady_clock::now() < end)) {
		for (const TestPointerEvent& event : window.TakePointerEvents()) {
			switch (event.type) {
			case TestPointerEvent::Type::Enter:
				std::printf("enter %d %d\n", (int)event.x, (int)event.y);
				break;
			case TestPointerEvent::Type::Leave:
				std::printf("leave\n");
				break;
			case TestPointerEvent::Type::Motion:
				std::printf("motion %d %d\n", (int)event.x, (int)event.y);
				break;
			case TestPointerEvent::Type::Button:
				std::printf("button %u %s %d %d\n", event.button, event.isPressed ? "down" : "up", (int)event.x, (int)event.y);
				break;
			}
		}
		for (const TestKeyEvent& event : window.TakeKeyEvents()) {
			std::printf("key %u %s\n", event.key, event.isPressed ? "down" : "up");
		}
		std::fflush(stdout);
		std::this_thread::sleep_for(10ms);
	}
	return 0;
}
