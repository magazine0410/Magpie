// The data of the home page. Linux counterpart of HomeViewModel
// (src/Magpie). Not here: the update card, touch support, and the font
// cache of the developer options, which have no counterpart.
#pragma once
#include <QObject>
#include <QStringList>
#include "AppSettings.h"
#include "ViewModelBase.h"

namespace Magpie {

class ScalingController;

class HomeViewModel : public QObject {
	Q_OBJECT
	Q_PROPERTY(bool isEngineAvailable READ IsEngineAvailable NOTIFY Changed)
	Q_PROPERTY(QString engineError READ EngineError NOTIFY Changed)

	Q_PROPERTY(QString scaleShortcut READ ScaleShortcut NOTIFY Changed)
	Q_PROPERTY(QString windowedScaleShortcut READ WindowedScaleShortcut NOTIFY Changed)
	Q_PROPERTY(QString toolbarShortcut READ ToolbarShortcut NOTIFY Changed)
	Q_PROPERTY(QString screenshotShortcut READ ScreenshotShortcut NOTIFY Changed)
	Q_PROPERTY(bool hasShortcuts READ HasShortcuts NOTIFY Changed)
	Q_PROPERTY(bool canConfigureShortcuts READ CanConfigureShortcuts NOTIFY Changed)

	Q_PROPERTY(QString timerDescription READ TimerDescription NOTIFY Changed)
	Q_PROPERTY(bool isTimerOn READ IsTimerOn NOTIFY Changed)
	Q_PROPERTY(bool isFullscreenTimerOn READ IsFullscreenTimerOn NOTIFY Changed)
	Q_PROPERTY(bool isWindowedTimerOn READ IsWindowedTimerOn NOTIFY Changed)
	Q_PROPERTY(double timerProgress READ TimerProgress NOTIFY TimerChanged)
	Q_PROPERTY(QString timerLabelText READ TimerLabelText NOTIFY TimerChanged)
	Q_PROPERTY(bool isNotRunning READ IsNotRunning NOTIFY Changed)
	Q_PROPERTY(int delay READ Delay WRITE SetDelay NOTIFY Changed)

	Q_PROPERTY(bool isAllowScalingMaximized READ IsAllowScalingMaximized WRITE SetIsAllowScalingMaximized NOTIFY Changed)
	Q_PROPERTY(bool isKeepScreenOn READ IsKeepScreenOn WRITE SetIsKeepScreenOn NOTIFY Changed)
	Q_PROPERTY(bool isSimulateExclusiveFullscreen READ IsSimulateExclusiveFullscreen WRITE SetIsSimulateExclusiveFullscreen NOTIFY Changed)
	Q_PROPERTY(bool isInlineParams READ IsInlineParams WRITE SetIsInlineParams NOTIFY Changed)
	Q_PROPERTY(QStringList minFrameRateOptions READ MinFrameRateOptions CONSTANT)
	Q_PROPERTY(int minFrameRateIndex READ MinFrameRateIndex WRITE SetMinFrameRateIndex NOTIFY Changed)

	Q_PROPERTY(bool isDeveloperMode READ IsDeveloperMode WRITE SetIsDeveloperMode NOTIFY Changed)
	Q_PROPERTY(bool isDebugMode READ IsDebugMode WRITE SetIsDebugMode NOTIFY Changed)
	Q_PROPERTY(bool isBenchmarkMode READ IsBenchmarkMode WRITE SetIsBenchmarkMode NOTIFY Changed)
	Q_PROPERTY(bool isTopmostDisabled READ IsTopmostDisabled WRITE SetIsTopmostDisabled NOTIFY Changed)
	Q_PROPERTY(bool isEffectCacheDisabled READ IsEffectCacheDisabled WRITE SetIsEffectCacheDisabled NOTIFY Changed)
	Q_PROPERTY(bool isSaveEffectSources READ IsSaveEffectSources WRITE SetIsSaveEffectSources NOTIFY Changed)
	Q_PROPERTY(bool isWarningsAreErrors READ IsWarningsAreErrors WRITE SetIsWarningsAreErrors NOTIFY Changed)
	Q_PROPERTY(bool isFP16Disabled READ IsFP16Disabled WRITE SetIsFP16Disabled NOTIFY Changed)
	Q_PROPERTY(int duplicateFrameDetectionMode READ DuplicateFrameDetectionMode WRITE SetDuplicateFrameDetectionMode NOTIFY Changed)
	Q_PROPERTY(bool isDynamicDetection READ IsDynamicDetection NOTIFY Changed)
	Q_PROPERTY(bool isStatisticsForDynamicDetectionEnabled READ IsStatisticsForDynamicDetectionEnabled WRITE SetIsStatisticsForDynamicDetectionEnabled NOTIFY Changed)

public:
	HomeViewModel(AppSettings& settings, ScalingController& scaling, QObject* parent = nullptr);

	bool IsEngineAvailable() const;
	QString EngineError() const {
		return _engineError;
	}

	QString ScaleShortcut() const;
	QString WindowedScaleShortcut() const;
	QString ToolbarShortcut() const;
	QString ScreenshotShortcut() const;
	bool HasShortcuts() const;
	bool CanConfigureShortcuts() const;
	Q_INVOKABLE void configureShortcuts();

	QString TimerDescription() const;
	bool IsTimerOn() const;
	bool IsFullscreenTimerOn() const;
	bool IsWindowedTimerOn() const;
	double TimerProgress() const;
	QString TimerLabelText() const;
	bool IsNotRunning() const;
	Q_INVOKABLE void toggleTimer(bool windowedMode);

	int Delay() const {
		return (int)_data().countdownSeconds;
	}
	void SetDelay(int value);

	MAGPIE_SETTING(bool, IsAllowScalingMaximized, _data().isAllowScalingMaximized)
	MAGPIE_SETTING(bool, IsKeepScreenOn, _data().isKeepScreenOn)
	MAGPIE_SETTING(bool, IsSimulateExclusiveFullscreen, _data().isSimulateExclusiveFullscreen)
	MAGPIE_SETTING(bool, IsInlineParams, _data().isInlineParams)

	QStringList MinFrameRateOptions() const;
	int MinFrameRateIndex() const;
	void SetMinFrameRateIndex(int value);

	bool IsDeveloperMode() const {
		return _data().isDeveloperMode;
	}
	void SetIsDeveloperMode(bool value);
	Q_INVOKABLE void locateLogs();

	MAGPIE_SETTING(bool, IsDebugMode, _data().isDebugMode)
	MAGPIE_SETTING(bool, IsBenchmarkMode, _data().isBenchmarkMode)
	MAGPIE_SETTING(bool, IsTopmostDisabled, _data().isTopmostDisabled)
	MAGPIE_SETTING(bool, IsEffectCacheDisabled, _data().isEffectCacheDisabled)
	MAGPIE_SETTING(bool, IsSaveEffectSources, _data().isSaveEffectSources)
	MAGPIE_SETTING(bool, IsWarningsAreErrors, _data().isWarningsAreErrors)
	MAGPIE_SETTING(bool, IsFP16Disabled, _data().isFP16Disabled)

	int DuplicateFrameDetectionMode() const {
		return (int)_data().duplicateFrameDetectionMode;
	}
	void SetDuplicateFrameDetectionMode(int value);
	bool IsDynamicDetection() const {
		return _data().duplicateFrameDetectionMode == DuplicateFrameDetectionMode::Dynamic;
	}
	MAGPIE_SETTING(bool, IsStatisticsForDynamicDetectionEnabled, _data().isStatisticsForDynamicDetectionEnabled)

Q_SIGNALS:
	void Changed();
	// The countdown went on
	void TimerChanged();

private:
	Settings& _data() const {
		return _settings.Data();
	}

	void _Save() {
		_settings.Save();
		Q_EMIT Changed();
	}

	AppSettings& _settings;
	ScalingController& _scaling;
	QString _engineError;
};

}
