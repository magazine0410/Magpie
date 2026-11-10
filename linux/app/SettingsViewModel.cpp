#include "SettingsViewModel.h"
#include <algorithm>
#include <QCoreApplication>
#include <QDesktopServices>
#include <QQuickStyle>
#include <QUrl>
#include "Application.h"
#include "AutoStart.h"
#include "Localization.h"
#include "SettingsStore.h"

namespace Magpie {

SettingsViewModel::SettingsViewModel(Application& application, QObject* parent) :
	QObject(parent), _application(application), _settings(application.Settings()) {}

QStringList SettingsViewModel::Languages() const {
	QStringList languages = Localization::LanguageNames();
	languages.push_front(qtTrId("Linux_SystemDefault"));
	return languages;
}

int SettingsViewModel::Language() const {
	const auto it = std::ranges::find(SUPPORTED_LANGUAGES, std::string_view(_settings.Data().language));
	return it == SUPPORTED_LANGUAGES.end() ? 0 : int(it - SUPPORTED_LANGUAGES.begin()) + 1;
}

void SettingsViewModel::SetLanguage(int value) {
	if (value < 0 || value > (int)SUPPORTED_LANGUAGES.size() || value == Language()) {
		return;
	}
	_settings.Data().language = value == 0 ? std::string() : std::string(SUPPORTED_LANGUAGES[(size_t)value - 1]);
	_settings.Save();
	Q_EMIT Changed();
}

bool SettingsViewModel::RequireRestart() const {
	return _settings.Data().language != _application.InitialLanguage();
}

void SettingsViewModel::restart() {
	_application.Restart();
}

bool SettingsViewModel::CanChooseTheme() const {
	return QQuickStyle::name() != "org.kde.desktop";
}

int SettingsViewModel::Theme() const {
	switch (_settings.Data().theme) {
	case AppTheme::Light:
		return 1;
	case AppTheme::Dark:
		return 2;
	default:
		return 0;
	}
}

void SettingsViewModel::SetTheme(int value) {
	if (value < 0 || value > 2 || value == Theme()) {
		return;
	}
	_settings.Data().theme = value == 1 ? AppTheme::Light : value == 2 ? AppTheme::Dark : AppTheme::System;
	_settings.Save();
	_application.ApplyTheme();
	Q_EMIT Changed();
}

QString SettingsViewModel::ConfigPath() const {
	return QString::fromStdString(SettingsStore::ConfigPath().string());
}

void SettingsViewModel::openConfigLocation() {
	QDesktopServices::openUrl(QUrl::fromLocalFile(
		QString::fromStdString(SettingsStore::ConfigPath().parent_path().string())));
}

bool SettingsViewModel::IsShowNotifyIcon() const {
	return _settings.Data().isShowNotifyIcon;
}

void SettingsViewModel::SetIsShowNotifyIcon(bool value) {
	if (_settings.Data().isShowNotifyIcon == value) {
		return;
	}
	_settings.Data().isShowNotifyIcon = value;
	_settings.Save();
	_application.UpdateTrayIcon();
	Q_EMIT Changed();
}

bool SettingsViewModel::IsRunAtStartup() const {
	return AutoStart::IsEnabled();
}

void SettingsViewModel::SetIsRunAtStartup(bool value) {
	if (value) {
		AutoStart::Enable(QCoreApplication::applicationFilePath());
	} else {
		AutoStart::Disable();
	}
	Q_EMIT Changed();
}

}
