#include "HomeViewModel.h"
#include <array>
#include <cmath>
#include <QDesktopServices>
#include <QUrl>
#include "Localization.h"
#include "Paths.h"
#include "ScalingController.h"
#include "ScalingOptions.h"

namespace Magpie {

static constexpr std::array MIN_FRAME_RATE_OPTIONS{ 0, 5, 10, 15, 20, 30, 60 };

HomeViewModel::HomeViewModel(AppSettings& settings, ScalingController& scaling, QObject* parent) :
	QObject(parent), _settings(settings), _scaling(scaling) {
	connect(&scaling, &ScalingController::IsScalingChanged, this, &HomeViewModel::Changed);
	connect(&scaling, &ScalingController::ShortcutsChanged, this, &HomeViewModel::Changed);
	connect(&scaling, &ScalingController::IsTimerOnChanged, this, [this] {
		Q_EMIT Changed();
		Q_EMIT TimerChanged();
	});
	connect(&scaling, &ScalingController::TimerTick, this, &HomeViewModel::TimerChanged);
	connect(&scaling, &ScalingController::Failed, this, [this](const QString& message) {
		_engineError = message;
		Q_EMIT Changed();
	});
}

bool HomeViewModel::IsEngineAvailable() const {
	return _scaling.IsAvailable();
}

QString HomeViewModel::ScaleShortcut() const {
	return _scaling.ShortcutKey(ScalingService::SHORTCUT_SCALE);
}

QString HomeViewModel::WindowedScaleShortcut() const {
	return _scaling.ShortcutKey(ScalingService::SHORTCUT_SCALE_WINDOWED);
}

QString HomeViewModel::ToolbarShortcut() const {
	return _scaling.ShortcutKey(ScalingService::SHORTCUT_TOOLBAR);
}

QString HomeViewModel::ScreenshotShortcut() const {
	return _scaling.ShortcutKey(ScalingService::SHORTCUT_SCREENSHOT);
}

bool HomeViewModel::HasShortcuts() const {
	return _scaling.HasShortcuts();
}

bool HomeViewModel::HasDesktopBackEnd() const {
	return _scaling.HasDesktopBackEnd();
}

bool HomeViewModel::CanConfigureShortcuts() const {
	return _scaling.CanConfigureShortcuts();
}

void HomeViewModel::configureShortcuts() {
	_scaling.ConfigureShortcuts();
}

// What Qt reports for a key press, as the settings store a shortcut: the
// code of the key as Windows numbers it, which for letters and digits is
// also Qt's number. A key that cannot be bound gives no code.
static Shortcut ShortcutFromKeyPress(int key, int modifiers) {
	Shortcut shortcut;
	shortcut.ctrl = modifiers & Qt::ControlModifier;
	shortcut.alt = modifiers & Qt::AltModifier;
	shortcut.shift = modifiers & Qt::ShiftModifier;
	shortcut.win = modifiers & Qt::MetaModifier;
	if ((key >= Qt::Key_A && key <= Qt::Key_Z) || (key >= Qt::Key_0 && key <= Qt::Key_9)) {
		shortcut.code = (uint8_t)key;
	} else if (key >= Qt::Key_F1 && key <= Qt::Key_F24) {
		// VK_F1 and the keys after it
		shortcut.code = (uint8_t)(0x70 + (key - Qt::Key_F1));
	}
	return shortcut;
}

bool HomeViewModel::CanRecordShortcuts() const {
	return _scaling.BindsOwnShortcuts();
}

void HomeViewModel::beginShortcutRecording() {
	_scaling.RebindShortcuts(false);
}

void HomeViewModel::endShortcutRecording() {
	_scaling.RebindShortcuts(true);
}

QString HomeViewModel::shortcutText(int key, int modifiers) const {
	return QString::fromStdString(ShortcutToText(ShortcutFromKeyPress(key, modifiers)));
}

QString HomeViewModel::shortcutProblem(int action, int key, int modifiers) const {
	if (action < 0 || action >= (int)ShortcutAction::COUNT_OR_NONE) {
		return qtTrId("ShortcutDialog_InvalidShortcut");
	}
	const Shortcut shortcut = ShortcutFromKeyPress(key, modifiers);
	if (!IsBindableShortcut(shortcut)) {
		return qtTrId("ShortcutDialog_InvalidShortcut");
	}
	for (int other = 0; other < (int)ShortcutAction::COUNT_OR_NONE; ++other) {
		if (other != action && _data().shortcuts[(size_t)other] == shortcut) {
			return qtTrId("ShortcutDialog_InUse");
		}
	}
	return {};
}

bool HomeViewModel::setShortcut(int action, int key, int modifiers) {
	if (!shortcutProblem(action, key, modifiers).isEmpty()) {
		return false;
	}
	_data().shortcuts[(size_t)action] = ShortcutFromKeyPress(key, modifiers);
	_Save();
	// The service binds the new keys when the dialog closes
	return true;
}

QString HomeViewModel::TimerDescription() const {
	return Localization::Format("Home_Activation_Timer_Description", QString::number(_data().countdownSeconds));
}

bool HomeViewModel::IsTimerOn() const {
	return _scaling.IsTimerOn();
}

bool HomeViewModel::IsFullscreenTimerOn() const {
	return _scaling.IsTimerOn(false);
}

bool HomeViewModel::IsWindowedTimerOn() const {
	return _scaling.IsTimerOn(true);
}

double HomeViewModel::TimerProgress() const {
	return _scaling.TimerProgress();
}

QString HomeViewModel::TimerLabelText() const {
	return _scaling.IsTimerOn() ? QString::number((int)std::ceil(_scaling.SecondsLeft())) : QString();
}

bool HomeViewModel::IsNotRunning() const {
	return !_scaling.IsScaling();
}

void HomeViewModel::toggleTimer(bool windowedMode) {
	if (_scaling.IsTimerOn(windowedMode)) {
		_scaling.StopTimer();
	} else {
		_scaling.StartTimer(windowedMode);
	}
}

void HomeViewModel::SetDelay(int value) {
	if (value < 1 || value > 5 || (int)_data().countdownSeconds == value) {
		return;
	}
	_data().countdownSeconds = (uint32_t)value;
	_Save();
}

QStringList HomeViewModel::MinFrameRateOptions() const {
	QStringList options;
	for (const int option : MIN_FRAME_RATE_OPTIONS) {
		options.push_back(QString::number(option));
	}
	return options;
}

int HomeViewModel::MinFrameRateIndex() const {
	for (size_t i = 0; i < MIN_FRAME_RATE_OPTIONS.size(); ++i) {
		if (IsApprox(_data().minFrameRate, (float)MIN_FRAME_RATE_OPTIONS[i])) {
			return (int)i;
		}
	}
	return -1;
}

void HomeViewModel::SetMinFrameRateIndex(int value) {
	if (value < 0 || value >= (int)MIN_FRAME_RATE_OPTIONS.size() || value == MinFrameRateIndex()) {
		return;
	}
	_data().minFrameRate = (float)MIN_FRAME_RATE_OPTIONS[(size_t)value];
	_Save();
}

void HomeViewModel::SetIsDeveloperMode(bool value) {
	Settings& data = _data();
	if (data.isDeveloperMode == value) {
		return;
	}

	data.isDeveloperMode = value;
	if (!value) {
		// Without developer mode none of its options is in effect
		data.isDebugMode = false;
		data.isBenchmarkMode = false;
		data.isEffectCacheDisabled = false;
		data.isFontCacheDisabled = false;
		data.isSaveEffectSources = false;
		data.isWarningsAreErrors = false;
		data.duplicateFrameDetectionMode = DuplicateFrameDetectionMode::Dynamic;
		data.isStatisticsForDynamicDetectionEnabled = false;
		data.isFP16Disabled = false;
	}
	_Save();
}

void HomeViewModel::locateLogs() {
	QDesktopServices::openUrl(QUrl::fromLocalFile(QString::fromStdString(Paths::LogsDir().string())));
}

void HomeViewModel::SetDuplicateFrameDetectionMode(int value) {
	if (value < 0 || value > (int)DuplicateFrameDetectionMode::Never ||
		(int)_data().duplicateFrameDetectionMode == value) {
		return;
	}

	_data().duplicateFrameDetectionMode = (enum DuplicateFrameDetectionMode)value;
	if (!IsDynamicDetection()) {
		_data().isStatisticsForDynamicDetectionEnabled = false;
	}
	_Save();
}

}
