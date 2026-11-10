// Frame pacing: limits the frame rate from above, forces frames to keep a
// minimum rate, and counts frames per second. Linux counterpart of
// src/Magpie.Core/StepTimer.h. Where the Windows version waits for window
// messages, this one waits for file descriptors to become readable.
#pragma once
#include <atomic>
#include <chrono>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <vector>

namespace Magpie {

enum class StepTimerStatus {
	WaitingForNewFrame,
	WaitingForFPSLimiter,
	ForceNewFrame
};

class StepTimer {
public:
	StepTimer() = default;

	StepTimer(const StepTimer&) = delete;
	StepTimer(StepTimer&&) = delete;

	void Initialize(float minFrameRate, std::optional<float> maxFrameRate) noexcept;

	// The descriptors that end a wait when they become readable, for example
	// the Wayland connection and the event descriptor of the frame source.
	// The caller reads from them after WaitForNextFrame returns.
	void SetWakeDescriptors(std::span<const int> fds) noexcept;

	// With waitForWake the call blocks until a descriptor is readable or the
	// time for a forced frame has come; otherwise it only applies the frame
	// rate limit.
	StepTimerStatus WaitForNextFrame(bool waitForWake, bool& fpsUpdated) noexcept;

	void PrepareForRender() noexcept;

	uint32_t GetFrameCount() const noexcept {
		return _frameCount;
	}

	// May be called from other threads
	uint32_t GetFPS() const noexcept {
		return _framesPerSecond.load(std::memory_order_relaxed);
	}

private:
	bool _HasMinInterval() const noexcept;
	bool _HasMaxInterval() const noexcept;

	// Negative: no time limit
	void _WaitForWake(std::chrono::nanoseconds time) noexcept;
	void _WaitForWakeAndTimer(std::chrono::nanoseconds time) noexcept;

	bool _UpdateFPS(std::chrono::time_point<std::chrono::steady_clock> now) noexcept;

	std::chrono::nanoseconds _minInterval{};
	std::chrono::nanoseconds _maxInterval{ std::numeric_limits<std::chrono::nanoseconds::rep>::max() };
	std::vector<int> _wakeFds;

	std::chrono::steady_clock::time_point _thisFrameStartTime;
	std::chrono::steady_clock::time_point _nextFrameStartTime;
	std::chrono::steady_clock::time_point _lastSecondTime;

	uint32_t _frameCount = 0;
	std::atomic<uint32_t> _framesPerSecond{ 0 };
	uint32_t _framesThisSecond = 0;
};

}
