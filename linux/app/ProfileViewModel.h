// The data of a profile page: the default profile, or the profile of a
// program. Linux counterpart of ProfileViewModel (src/Magpie). Not here: the
// capture method and "disable DirectFlip", which have no counterpart.
// Renaming, moving and deleting the profile are functions of RootViewModel,
// because the list of the main window changes with them.
#pragma once
#include <QObject>
#include <QStringList>
#include <QUrl>
#include "AppSettings.h"
#include "ViewModelBase.h"

namespace Magpie {

class Application;

class ProfileViewModel : public QObject {
	Q_OBJECT
	Q_PROPERTY(bool isDefaultProfile READ IsDefaultProfile CONSTANT)
	Q_PROPERTY(QString name READ Name NOTIFY Changed)
	Q_PROPERTY(QString icon READ Icon CONSTANT)
	Q_PROPERTY(bool canLaunch READ CanLaunch NOTIFY Changed)
	Q_PROPERTY(bool canOpenProgramLocation READ CanOpenProgramLocation CONSTANT)
	Q_PROPERTY(QUrl launcherFolder READ LauncherFolder NOTIFY Changed)

	Q_PROPERTY(QStringList scalingModes READ ScalingModes CONSTANT)
	Q_PROPERTY(int scalingMode READ ScalingMode WRITE SetScalingMode NOTIFY Changed)
	Q_PROPERTY(int autoScale READ AutoScale WRITE SetAutoScale NOTIFY Changed)
	Q_PROPERTY(bool is3DGameMode READ Is3DGameMode WRITE SetIs3DGameMode NOTIFY Changed)
	Q_PROPERTY(bool hasMultipleMonitors READ HasMultipleMonitors CONSTANT)
	Q_PROPERTY(int multiMonitorUsage READ MultiMonitorUsage WRITE SetMultiMonitorUsage NOTIFY Changed)

	Q_PROPERTY(int initialWindowedScaleFactor READ InitialWindowedScaleFactor WRITE SetInitialWindowedScaleFactor NOTIFY Changed)
	Q_PROPERTY(double customInitialWindowedScaleFactor READ CustomInitialWindowedScaleFactor WRITE SetCustomInitialWindowedScaleFactor NOTIFY Changed)

	Q_PROPERTY(QString initialToolbarStateDescription READ InitialToolbarStateDescription NOTIFY Changed)
	Q_PROPERTY(int fullscreenInitialToolbarState READ FullscreenInitialToolbarState WRITE SetFullscreenInitialToolbarState NOTIFY Changed)
	Q_PROPERTY(int windowedInitialToolbarState READ WindowedInitialToolbarState WRITE SetWindowedInitialToolbarState NOTIFY Changed)
	Q_PROPERTY(QString screenshotSaveDirectory READ ScreenshotSaveDirectory NOTIFY Changed)
	Q_PROPERTY(QString screenshotFilenameTemplate READ ScreenshotFilenameTemplate WRITE SetScreenshotFilenameTemplate NOTIFY Changed)
	Q_PROPERTY(bool isScreenshotFilenameTemplateValid READ IsScreenshotFilenameTemplateValid NOTIFY Changed)

	Q_PROPERTY(QStringList graphicsCards READ GraphicsCards CONSTANT)
	Q_PROPERTY(int graphicsCard READ GraphicsCard WRITE SetGraphicsCard NOTIFY Changed)
	Q_PROPERTY(bool isShowGraphicsCardSettingsCard READ IsShowGraphicsCardSettingsCard CONSTANT)
	Q_PROPERTY(bool isNoGraphicsCard READ IsNoGraphicsCard CONSTANT)
	Q_PROPERTY(bool isFrameRateLimiterEnabled READ IsFrameRateLimiterEnabled WRITE SetIsFrameRateLimiterEnabled NOTIFY Changed)
	Q_PROPERTY(double maxFrameRate READ MaxFrameRate WRITE SetMaxFrameRate NOTIFY Changed)

	Q_PROPERTY(bool isCaptureTitleBar READ IsCaptureTitleBar WRITE SetIsCaptureTitleBar NOTIFY Changed)
	Q_PROPERTY(bool isCroppingEnabled READ IsCroppingEnabled WRITE SetIsCroppingEnabled NOTIFY Changed)
	Q_PROPERTY(double croppingLeft READ CroppingLeft WRITE SetCroppingLeft NOTIFY Changed)
	Q_PROPERTY(double croppingTop READ CroppingTop WRITE SetCroppingTop NOTIFY Changed)
	Q_PROPERTY(double croppingRight READ CroppingRight WRITE SetCroppingRight NOTIFY Changed)
	Q_PROPERTY(double croppingBottom READ CroppingBottom WRITE SetCroppingBottom NOTIFY Changed)

	Q_PROPERTY(int cursorScaling READ CursorScaling WRITE SetCursorScaling NOTIFY Changed)
	Q_PROPERTY(double customCursorScaleFactor READ CustomCursorScaleFactor WRITE SetCustomCursorScaleFactor NOTIFY Changed)
	Q_PROPERTY(int cursorInterpolationMode READ CursorInterpolationMode WRITE SetCursorInterpolationMode NOTIFY Changed)
	Q_PROPERTY(bool isAutoHideCursorEnabled READ IsAutoHideCursorEnabled WRITE SetIsAutoHideCursorEnabled NOTIFY Changed)
	Q_PROPERTY(double autoHideCursorDelay READ AutoHideCursorDelay WRITE SetAutoHideCursorDelay NOTIFY Changed)
	Q_PROPERTY(bool isAdjustCursorSpeed READ IsAdjustCursorSpeed WRITE SetIsAdjustCursorSpeed NOTIFY Changed)

	Q_PROPERTY(QString launchParameters READ LaunchParameters WRITE SetLaunchParameters NOTIFY Changed)
	// Only on Linux: start the program in a nested session, and the size
	// that session offers it
	Q_PROPERTY(bool isLaunchedNested READ IsLaunchedNested WRITE SetIsLaunchedNested NOTIFY Changed)
	Q_PROPERTY(double nestedWidth READ NestedWidth WRITE SetNestedWidth NOTIFY Changed)
	Q_PROPERTY(double nestedHeight READ NestedHeight WRITE SetNestedHeight NOTIFY Changed)
	// The desktop has no back end, so the program is started that way
	// whatever the profile says
	Q_PROPERTY(bool isNestedOnly READ IsNestedOnly CONSTANT)
	Q_PROPERTY(int outputAlignment READ OutputAlignment WRITE SetOutputAlignment NOTIFY Changed)

public:
	// `index` is the place of the profile, or negative for the default
	// profile
	ProfileViewModel(Application& application, int index, QObject* parent = nullptr);

	bool IsDefaultProfile() const {
		return _index < 0;
	}

	QString Name() const;
	QString Icon() const;

	bool CanLaunch() const;
	Q_INVOKABLE void launch();
	bool CanOpenProgramLocation() const;
	Q_INVOKABLE void openProgramLocation();
	// Where the dialog for the launcher starts
	QUrl LauncherFolder() const;
	// The file that is started in place of the program of the rule
	Q_INVOKABLE void changeLauncher(const QUrl& file);

	// "None" first, then the names of the scaling modes
	QStringList ScalingModes() const;
	int ScalingMode() const {
		return _data().scalingMode + 1;
	}
	void SetScalingMode(int value);

	int AutoScale() const {
		return (int)_data().autoScale;
	}
	void SetAutoScale(int value);

	bool Is3DGameMode() const {
		return _data().Is3DGameMode();
	}
	void SetIs3DGameMode(bool value);

	bool HasMultipleMonitors() const;
	MAGPIE_SETTING(int, MultiMonitorUsage, _data().multiMonitorUsage)

	MAGPIE_SETTING(int, InitialWindowedScaleFactor, _data().initialWindowedScaleFactor)
	double CustomInitialWindowedScaleFactor() const {
		return _data().customInitialWindowedScaleFactor;
	}
	void SetCustomInitialWindowedScaleFactor(double value);

	QString InitialToolbarStateDescription() const;
	MAGPIE_SETTING(int, FullscreenInitialToolbarState, _data().fullscreenInitialToolbarState)
	MAGPIE_SETTING(int, WindowedInitialToolbarState, _data().windowedInitialToolbarState)

	QString ScreenshotSaveDirectory() const;
	Q_INVOKABLE void openScreenshotSaveDirectory();
	Q_INVOKABLE void changeScreenshotSaveDirectory(const QUrl& directory);
	QString ScreenshotFilenameTemplate() const;
	void SetScreenshotFilenameTemplate(const QString& value);
	bool IsScreenshotFilenameTemplateValid() const;

	// The default first, then the graphics cards
	QStringList GraphicsCards() const;
	int GraphicsCard() const;
	void SetGraphicsCard(int value);
	bool IsShowGraphicsCardSettingsCard() const;
	bool IsNoGraphicsCard() const;

	MAGPIE_SETTING(bool, IsFrameRateLimiterEnabled, _data().isFrameRateLimiterEnabled)
	double MaxFrameRate() const {
		return _data().maxFrameRate;
	}
	void SetMaxFrameRate(double value);

	bool IsCaptureTitleBar() const {
		return _data().IsCaptureTitleBar();
	}
	void SetIsCaptureTitleBar(bool value);

	MAGPIE_SETTING(bool, IsCroppingEnabled, _data().isCroppingEnabled)
	double CroppingLeft() const {
		return _data().cropping.Left;
	}
	double CroppingTop() const {
		return _data().cropping.Top;
	}
	double CroppingRight() const {
		return _data().cropping.Right;
	}
	double CroppingBottom() const {
		return _data().cropping.Bottom;
	}
	void SetCroppingLeft(double value);
	void SetCroppingTop(double value);
	void SetCroppingRight(double value);
	void SetCroppingBottom(double value);

	MAGPIE_SETTING(int, CursorScaling, _data().cursorScaling)
	double CustomCursorScaleFactor() const {
		return _data().customCursorScaleFactor;
	}
	void SetCustomCursorScaleFactor(double value);
	MAGPIE_SETTING(int, CursorInterpolationMode, _data().cursorInterpolationMode)
	MAGPIE_SETTING(bool, IsAutoHideCursorEnabled, _data().isAutoHideCursorEnabled)
	double AutoHideCursorDelay() const {
		return _data().autoHideCursorDelay;
	}
	void SetAutoHideCursorDelay(double value);

	bool IsAdjustCursorSpeed() const {
		return _data().IsAdjustCursorSpeed();
	}
	void SetIsAdjustCursorSpeed(bool value);

	QString LaunchParameters() const;
	void SetLaunchParameters(const QString& value);

	MAGPIE_SETTING(bool, IsLaunchedNested, _data().isLaunchedNested)
	double NestedWidth() const {
		return _data().nestedWidth;
	}
	void SetNestedWidth(double value);
	double NestedHeight() const {
		return _data().nestedHeight;
	}
	void SetNestedHeight(double value);
	bool IsNestedOnly() const;
	MAGPIE_SETTING(int, OutputAlignment, _data().outputAlignment)

Q_SIGNALS:
	void Changed();

private:
	Profile& _data() const;
	void _Save();
	void _SetCropping(float& side, double value);

	Application& _application;
	AppSettings& _settings;
	int _index;
};

}
