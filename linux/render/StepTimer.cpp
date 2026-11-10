#include "StepTimer.h"
#include <cassert>
#include <ctime>
#include <poll.h>
#include <sched.h>

using namespace std::chrono;

namespace Magpie {

void StepTimer::Initialize(float minFrameRate, std::optional<float> maxFrameRate) noexcept {
	assert(minFrameRate >= 0);
	if (minFrameRate > 0) {
		_maxInterval = duration_cast<nanoseconds>(duration<float>(1 / minFrameRate));
	}

	if (maxFrameRate) {
		assert(*maxFrameRate > 0);
		_minInterval = duration_cast<nanoseconds>(duration<float>(1 / *maxFrameRate));
		assert(_minInterval <= _maxInterval);

		if (_HasMaxInterval()) {
			// The longest interval is made a multiple of the shortest. This
			// keeps the intervals even, at the price of a minimum frame rate
			// that can be slightly above the requested one.
			_maxInterval = _maxInterval / _minInterval * _minInterval;
		}
	}
}

void StepTimer::SetWakeDescriptors(std::span<const int> fds) noexcept {
	_wakeFds.assign(fds.begin(), fds.end());
}

// A frame starts at the moment its loop iteration begins to capture, which is
// the only moment under our control. "wait" below is waiting for the frame
// rate limit and for a new frame; WaitForNextFrame runs several times during
// it. PrepareForRender runs before "render".
//
// _thisFrameStartTime      _nextFrameStartTime
//         │                         │
// ────────▼─────────┬────────┬──────▼─────────
//    wait │ capture │ render │ wait │ capture
//
StepTimerStatus StepTimer::WaitForNextFrame(bool waitForWake, bool& fpsUpdated) noexcept {
	fpsUpdated = false;

	// Updated again and again until a new frame arrives
	_nextFrameStartTime = steady_clock::now();

	if (_thisFrameStartTime == time_point<steady_clock>{}) {
		// Waiting for the first frame; no FPS to update
		if (waitForWake) {
			_WaitForWake(nanoseconds(-1));
		}

		return StepTimerStatus::WaitingForNewFrame;
	}

	// Capture and render time of the current frame and the time waited since
	const nanoseconds delta = _nextFrameStartTime - _thisFrameStartTime;

	if (delta >= _maxInterval) {
		return StepTimerStatus::ForceNewFrame;
	}

	// The FPS is updated without new frames too. A forced frame needs no
	// update here, because PrepareForRender follows.
	fpsUpdated = _UpdateFPS(_nextFrameStartTime);

	if (delta < _minInterval) {
		_WaitForWakeAndTimer(_minInterval - delta);
		return StepTimerStatus::WaitingForFPSLimiter;
	}

	if (waitForWake) {
		if (_HasMaxInterval()) {
			_WaitForWakeAndTimer(_maxInterval - delta);
		} else {
			// Without a minimum frame rate only a wake-up ends the wait. It
			// is limited to 500 ms so that the FPS is updated in time.
			_WaitForWake(500ms);
		}
	}

	return StepTimerStatus::WaitingForNewFrame;
}

void StepTimer::PrepareForRender() noexcept {
	// A new frame begins; find its start time
	if (_HasMinInterval()) {
		// With a frame rate limit the intervals have to be multiples of the
		// shortest interval, so the start time is moved back a little. The
		// longest interval is a multiple of the shortest for the same reason.
		_thisFrameStartTime = _nextFrameStartTime -
			(_nextFrameStartTime - _thisFrameStartTime) % _minInterval;
	} else {
		_thisFrameStartTime = _nextFrameStartTime;
	}

	++_framesThisSecond;
	++_frameCount;

	_UpdateFPS(steady_clock::now());
}

void StepTimer::_WaitForWake(nanoseconds time) noexcept {
	if (_wakeFds.empty() && time < 0ns) {
		// Nothing could end the wait
		return;
	}

	std::vector<pollfd> fds(_wakeFds.size());
	for (size_t i = 0; i < fds.size(); ++i) {
		fds[i] = { _wakeFds[i], POLLIN, 0 };
	}

	timespec timeout{};
	if (time >= 0ns) {
		timeout.tv_sec = time_t(time.count() / 1'000'000'000);
		timeout.tv_nsec = long(time.count() % 1'000'000'000);
	}
	ppoll(fds.data(), fds.size(), time >= 0ns ? &timeout : nullptr, nullptr);
}

void StepTimer::_WaitForWakeAndTimer(nanoseconds time) noexcept {
	if (time > 1ms) {
		// A wake-up ends the wait early
		_WaitForWake(time - 1ms);
	} else {
		// With less than a millisecond left, wait busily
		sched_yield();
	}
}

// Returns true when the FPS changed
bool StepTimer::_UpdateFPS(time_point<steady_clock> now) noexcept {
	if (_lastSecondTime == time_point<steady_clock>{}) {
		// First frame
		_lastSecondTime = now;
	} else {
		const nanoseconds delta = now - _lastSecondTime;
		if (delta < 1s) {
			return false;
		}

		_lastSecondTime = now - delta % 1s;
	}

	const uint32_t oldFPS = _framesPerSecond.exchange(_framesThisSecond, std::memory_order_relaxed);
	const bool changed = oldFPS != _framesThisSecond;
	_framesThisSecond = 0;

	return changed;
}

bool StepTimer::_HasMinInterval() const noexcept {
	return _minInterval.count() != 0;
}

bool StepTimer::_HasMaxInterval() const noexcept {
	return _maxInterval.count() != std::numeric_limits<nanoseconds::rep>::max();
}

}
