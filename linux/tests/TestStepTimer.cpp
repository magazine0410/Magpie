// Frame pacing. The tests measure real time, so their limits are wide enough
// for a busy machine.
#include <chrono>
#include <thread>
#include <unistd.h>
#include <catch2/catch_test_macros.hpp>
#include "StepTimer.h"

using namespace Magpie;
using namespace std::chrono;

namespace {

struct Pipe {
	Pipe() {
		REQUIRE(pipe(fds) == 0);
	}

	~Pipe() {
		close(fds[0]);
		close(fds[1]);
	}

	void Signal() {
		REQUIRE(write(fds[1], "x", 1) == 1);
	}

	void Drain() {
		char byte;
		REQUIRE(read(fds[0], &byte, 1) == 1);
	}

	int fds[2];
};

}

TEST_CASE("StepTimer limits the frame rate", "[render]") {
	StepTimer timer;
	timer.Initialize(0.0f, 100.0f);

	// A source that has a new frame whenever it is asked
	uint32_t frames = 0;
	const auto start = steady_clock::now();
	while (steady_clock::now() - start < 300ms) {
		bool fpsUpdated;
		if (timer.WaitForNextFrame(false, fpsUpdated) == StepTimerStatus::WaitingForFPSLimiter) {
			continue;
		}
		timer.PrepareForRender();
		++frames;
	}

	// 10 ms apart: 30 or 31 frames
	CHECK(frames >= 22);
	CHECK(frames <= 32);
	CHECK(timer.GetFrameCount() == frames);
}

TEST_CASE("StepTimer forces frames to keep the minimum rate", "[render]") {
	Pipe wake;
	StepTimer timer;
	timer.Initialize(50.0f, std::nullopt);
	timer.SetWakeDescriptors(wake.fds);

	// The first frame is waited for without a time limit
	wake.Signal();
	bool fpsUpdated;
	CHECK(timer.WaitForNextFrame(true, fpsUpdated) == StepTimerStatus::WaitingForNewFrame);
	wake.Drain();
	timer.PrepareForRender();

	// From now on the source is silent
	uint32_t forced = 0;
	const auto start = steady_clock::now();
	while (steady_clock::now() - start < 210ms) {
		if (timer.WaitForNextFrame(true, fpsUpdated) == StepTimerStatus::ForceNewFrame) {
			timer.PrepareForRender();
			++forced;
		}
	}

	// 20 ms apart: 10 frames
	CHECK(forced >= 7);
	CHECK(forced <= 11);
}

TEST_CASE("A wake descriptor ends the wait for a new frame", "[render]") {
	Pipe wake;
	StepTimer timer;
	// One forced frame per 10 seconds, so that the wait below would be long
	timer.Initialize(0.1f, std::nullopt);
	timer.SetWakeDescriptors(wake.fds);

	wake.Signal();
	bool fpsUpdated;
	timer.WaitForNextFrame(true, fpsUpdated);
	wake.Drain();
	timer.PrepareForRender();

	std::jthread source([&] {
		std::this_thread::sleep_for(30ms);
		wake.Signal();
	});

	const auto start = steady_clock::now();
	CHECK(timer.WaitForNextFrame(true, fpsUpdated) == StepTimerStatus::WaitingForNewFrame);
	const auto waited = steady_clock::now() - start;
	CHECK(waited >= 25ms);
	CHECK(waited < 2s);
}

TEST_CASE("A wake time ends the wait for a new frame", "[render]") {
	Pipe wake;
	StepTimer timer;
	timer.Initialize(0.1f, std::nullopt);
	timer.SetWakeDescriptors(wake.fds);

	wake.Signal();
	bool fpsUpdated;
	timer.WaitForNextFrame(true, fpsUpdated);
	wake.Drain();
	timer.PrepareForRender();

	// Nothing signals the descriptor
	const auto start = steady_clock::now();
	timer.SetWakeTime(start + 30ms);
	CHECK(timer.WaitForNextFrame(true, fpsUpdated) == StepTimerStatus::WaitingForNewFrame);
	const auto waited = steady_clock::now() - start;
	CHECK(waited >= 25ms);
	CHECK(waited < 2s);

	// A wake time in the past does not block
	const auto second = steady_clock::now();
	timer.WaitForNextFrame(true, fpsUpdated);
	CHECK(steady_clock::now() - second < 20ms);
}

TEST_CASE("Without wake descriptors the first wait returns at once", "[render]") {
	StepTimer timer;
	timer.Initialize(0.0f, std::nullopt);

	bool fpsUpdated = true;
	const auto start = steady_clock::now();
	CHECK(timer.WaitForNextFrame(true, fpsUpdated) == StepTimerStatus::WaitingForNewFrame);
	CHECK(steady_clock::now() - start < 1s);
	CHECK_FALSE(fpsUpdated);
}
