// The scaling service as the user interface sees it: it runs the service on
// a thread of its own, brings what the service reports to the thread of the
// user interface, and has the countdown. Linux counterpart of the parts of
// ScalingService (src/Magpie) that the service of linux/engine does not
// have itself.
#pragma once
#include <chrono>
#include <functional>
#include <thread>
#include <vector>
#include <QObject>
#include <QString>
#include <QTimer>
#include "ScalingService.h"

namespace Magpie {

class AppSettings;

class ScalingController : public QObject {
	Q_OBJECT

public:
	explicit ScalingController(AppSettings& settings, QObject* parent = nullptr);
	// Stops scaling and ends the service
	~ScalingController() override;

	// Starts the service. Of `config` the directories, the names and the
	// options are used; the functions are set here. Without this call the
	// object does nothing, which the tests use.
	void Start(ScalingService::Config config);

	// Ends the service and waits for it, so that its name on D-Bus is free
	void Stop();

	// The service was started and did not fail
	bool IsAvailable() const noexcept {
		return _isAvailable;
	}

	// A window is scaled; waiting for a minimized source does not count, as
	// on Windows
	bool IsScaling() const noexcept {
		return _isScaling;
	}

	// The countdown, after which the active window is scaled. As
	// ScalingService::StartTimer and what follows it.
	void StartTimer(bool windowedMode);
	void StopTimer();

	bool IsTimerOn() const noexcept {
		return _countdownSeconds != 0;
	}

	bool IsTimerOn(bool windowedMode) const noexcept {
		return IsTimerOn() && _isCountdownWindowedMode == windowedMode;
	}

	double SecondsLeft() const noexcept;
	// From 1 down to 0
	double TimerProgress() const noexcept;

	// The key the desktop bound to a shortcut (ScalingService::SHORTCUT_*);
	// empty if it has none
	QString ShortcutKey(const char* id) const;

	// The desktop registered the shortcuts
	bool HasShortcuts() const noexcept {
		return !_shortcuts.empty();
	}

	// The desktop has a dialog for the keys. Where it has none, the keys
	// are those of config.json.
	bool CanConfigureShortcuts() const noexcept {
		return _service.CanConfigureShortcuts();
	}

	// Opens the desktop's dialog for the keys
	void ConfigureShortcuts();

	// Looks at the active window again for auto-scale, after a change of
	// the profiles
	void CheckForeground();

	// The windows that can be scaled. `reply` is called on the thread of
	// the user interface unless `context` is gone by then.
	void ListWindows(QObject* context, std::function<void(std::vector<ScalingService::Window>)> reply);

Q_SIGNALS:
	void IsScalingChanged();
	void IsTimerOnChanged();
	void TimerTick();
	void ShortcutsChanged();
	// Another start of the program asks for the main window
	void ShowWindowRequested();
	// The service could not start or has ended; scaling is not available
	void Failed(const QString& message);

private:
	void _OnStateChanged(ScalingService::State state);

	AppSettings& _settings;
	ScalingService _service;
	std::thread _thread;
	bool _isAvailable = false;
	bool _isStopping = false;
	bool _isScaling = false;
	ScalingService::BoundShortcuts _shortcuts;

	QTimer _countdownTimer;
	uint32_t _countdownSeconds = 0;
	bool _isCountdownWindowedMode = false;
	std::chrono::steady_clock::time_point _countdownStart;
};

}
