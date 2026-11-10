// The data of the settings page. Linux counterpart of SettingsViewModel
// (src/Magpie). Not here: portable mode, which is chosen with the
// environment variable MAGPIE_CONFIG_DIR, and "always run as administrator".
#pragma once
#include <QObject>
#include <QStringList>
#include "AppSettings.h"
#include "ViewModelBase.h"

namespace Magpie {

class Application;

class SettingsViewModel : public QObject {
	Q_OBJECT
	Q_PROPERTY(QStringList languages READ Languages CONSTANT)
	Q_PROPERTY(int language READ Language WRITE SetLanguage NOTIFY Changed)
	Q_PROPERTY(bool requireRestart READ RequireRestart NOTIFY Changed)
	Q_PROPERTY(bool canChooseTheme READ CanChooseTheme CONSTANT)
	Q_PROPERTY(int theme READ Theme WRITE SetTheme NOTIFY Changed)
	Q_PROPERTY(QString configPath READ ConfigPath CONSTANT)
	Q_PROPERTY(bool isShowNotifyIcon READ IsShowNotifyIcon WRITE SetIsShowNotifyIcon NOTIFY Changed)
	Q_PROPERTY(bool isRunAtStartup READ IsRunAtStartup WRITE SetIsRunAtStartup NOTIFY Changed)

public:
	SettingsViewModel(Application& application, QObject* parent = nullptr);

	// The system's language first, then the supported ones
	QStringList Languages() const;
	int Language() const;
	void SetLanguage(int value);
	bool RequireRestart() const;
	Q_INVOKABLE void restart();

	// False with the style of the KDE desktop, which takes its colours
	// from the desktop and gives a program no say in them. The setting is
	// not shown then.
	bool CanChooseTheme() const;
	// 0 for the system's, 1 for light, 2 for dark
	int Theme() const;
	void SetTheme(int value);

	QString ConfigPath() const;
	Q_INVOKABLE void openConfigLocation();

	bool IsShowNotifyIcon() const;
	void SetIsShowNotifyIcon(bool value);

	bool IsRunAtStartup() const;
	void SetIsRunAtStartup(bool value);

Q_SIGNALS:
	void Changed();

private:
	Application& _application;
	AppSettings& _settings;
};

}
