#include "ScalingController.h"
#include <limits>
#include <QCoreApplication>
#include <QPointer>
#include "AppSettings.h"

namespace Magpie {

ScalingController::ScalingController(AppSettings& settings, QObject* parent) :
	QObject(parent), _settings(settings) {
	_countdownTimer.setInterval(25);
	connect(&_countdownTimer, &QTimer::timeout, this, [this] {
		// Within 10 ms of the end counts as the end
		if (SecondsLeft() < 0.01) {
			const bool windowedMode = _isCountdownWindowedMode;
			StopTimer();
			if (_isAvailable) {
				_service.ScaleActiveWindow(windowedMode);
			}
			return;
		}
		Q_EMIT TimerTick();
	});
}

ScalingController::~ScalingController() {
	Stop();
}

void ScalingController::Start(ScalingService::Config config) {
	// What the service reports arrives on its thread and is passed on to
	// this object's
	config.settings = [&settings = _settings] {
		return settings.Snapshot();
	};
	config.stateChanged = [this](ScalingService::State state, bool) {
		QMetaObject::invokeMethod(this, [this, state] { _OnStateChanged(state); }, Qt::QueuedConnection);
	};
	config.shortcutsChanged = [this](const ScalingService::BoundShortcuts& shortcuts) {
		QMetaObject::invokeMethod(this, [this, shortcuts] {
			_shortcuts = shortcuts;
			Q_EMIT ShortcutsChanged();
		}, Qt::QueuedConnection);
	};
	config.overlayOptionsChanged = [this](const std::map<std::string, OverlayWindowOption>& windows) {
		QMetaObject::invokeMethod(this, [this, windows] {
			if (_settings.Data().overlayWindowOptions != windows) {
				_settings.Data().overlayWindowOptions = windows;
				_settings.Save();
			}
		}, Qt::QueuedConnection);
	};
	config.showWindowRequested = [this] {
		QMetaObject::invokeMethod(this, [this] { Q_EMIT ShowWindowRequested(); }, Qt::QueuedConnection);
	};

	_isAvailable = true;
	_thread = std::thread([this, config = std::move(config)]() mutable {
		std::string error;
		if (_service.Initialize(std::move(config), error)) {
			_service.Run();
			error = "The scaling service has ended.";
		}
		// The service could not start or lost the desktop, unless Stop asked
		// for its end
		QMetaObject::invokeMethod(this, [this, error] {
			if (_isStopping) {
				return;
			}
			_isAvailable = false;
			_OnStateChanged(ScalingService::State::Idle);
			Q_EMIT Failed(QString::fromStdString(error));
		}, Qt::QueuedConnection);
	});
}

void ScalingController::Stop() {
	_isStopping = true;
	if (_thread.joinable()) {
		_service.Quit();
		_thread.join();
	}
	_isAvailable = false;
}

void ScalingController::_OnStateChanged(ScalingService::State state) {
	const bool isScaling = state == ScalingService::State::Scaling;
	if (isScaling) {
		StopTimer();
	}
	if (_isScaling != isScaling) {
		_isScaling = isScaling;
		Q_EMIT IsScalingChanged();
	}
}

void ScalingController::StartTimer(bool windowedMode) {
	_countdownSeconds = _settings.Data().countdownSeconds;
	_isCountdownWindowedMode = windowedMode;
	_countdownStart = std::chrono::steady_clock::now();
	// A running timer starts anew, which is what is wanted
	_countdownTimer.start();
	Q_EMIT IsTimerOnChanged();
}

void ScalingController::StopTimer() {
	if (_countdownSeconds == 0) {
		return;
	}
	_countdownSeconds = 0;
	_countdownTimer.stop();
	Q_EMIT IsTimerOnChanged();
}

double ScalingController::SecondsLeft() const noexcept {
	if (!IsTimerOn()) {
		return std::numeric_limits<double>::max();
	}
	const auto end = _countdownStart + std::chrono::seconds(_countdownSeconds);
	return std::chrono::duration<double>(end - std::chrono::steady_clock::now()).count();
}

double ScalingController::TimerProgress() const noexcept {
	return IsTimerOn() ? SecondsLeft() / _countdownSeconds : 1.0;
}

QString ScalingController::ShortcutKey(const char* id) const {
	for (const auto& [shortcutId, key] : _shortcuts) {
		if (shortcutId == id) {
			return QString::fromStdString(key);
		}
	}
	return {};
}

void ScalingController::ConfigureShortcuts() {
	if (_isAvailable) {
		_service.ConfigureShortcuts();
	}
}

void ScalingController::RebindShortcuts(bool bound) {
	if (_isAvailable) {
		_service.RebindShortcuts(bound);
	}
}

void ScalingController::LaunchNested(ScalingService::LaunchRequest request) {
	if (IsAvailable()) {
		_service.LaunchNested(std::move(request));
	}
}

void ScalingController::CheckForeground() {
	if (_isAvailable) {
		_service.CheckForeground();
	}
}

void ScalingController::ListWindows(
	QObject* context,
	std::function<void(std::vector<ScalingService::Window>)> reply
) {
	if (!_isAvailable) {
		reply({});
		return;
	}
	_service.ListWindows([context = QPointer<QObject>(context), reply = std::move(reply)](
		std::vector<ScalingService::Window> windows) mutable {
		// The context may be deleted on its thread at any time, so it is
		// only looked at there
		QMetaObject::invokeMethod(qApp, [context, reply = std::move(reply), windows = std::move(windows)]() mutable {
			if (context) {
				reply(std::move(windows));
			}
		}, Qt::QueuedConnection);
	});
}

}
