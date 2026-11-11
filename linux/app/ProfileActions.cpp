#include "ProfileActions.h"
#include <QDir>
#include <QFileInfo>
#include <QLocale>
#include <QProcess>
#include <QSettings>
#include <QStandardPaths>

namespace Magpie {

QString ProfileActions::ProgramToLaunch(const Profile& profile) {
	if (!profile.launcherPath.empty()) {
		const QString launcher = QString::fromStdString(profile.launcherPath.string());
		if (QFileInfo(launcher).isFile()) {
			return launcher;
		}
	}

	const QFileInfo program(QString::fromStdString(profile.pathRule));
	return program.isAbsolute() && program.isFile() && program.isExecutable() ? program.filePath() : QString();
}

bool ProfileActions::Launch(const Profile& profile) {
	const QString program = ProgramToLaunch(profile);
	if (program.isEmpty()) {
		return false;
	}
	return QProcess::startDetached(
		program,
		QProcess::splitCommand(QString::fromStdString(profile.launchParameters)),
		QFileInfo(program).path());
}

QStringList ProfileActions::LaunchCommand(const Profile& profile) {
	const QString program = ProgramToLaunch(profile);
	if (program.isEmpty()) {
		return {};
	}
	return QStringList(program) + QProcess::splitCommand(QString::fromStdString(profile.launchParameters));
}

QString ProfileActions::ProgramLocation(const Profile& profile) {
	const QFileInfo program(QString::fromStdString(profile.pathRule));
	return program.isAbsolute() && program.isFile() ? program.path() : QString();
}

// The value of a key of the .desktop file of an application; with
// `localized` in the language of the user interface if the file has it
static QString DesktopEntryValue(const QString& appId, const QString& key, bool localized) {
	if (appId.isEmpty()) {
		return {};
	}

	const QString file = QStandardPaths::locate(QStandardPaths::ApplicationsLocation, appId + ".desktop");
	if (file.isEmpty()) {
		return {};
	}

	// A .desktop file has the form of an INI file
	QSettings entry(file, QSettings::IniFormat);
	entry.beginGroup("Desktop Entry");
	if (localized) {
		const QString locale = QLocale().name();
		for (const QString& suffix : { locale, locale.section('_', 0, 0) }) {
			const QString value = entry.value(QStringLiteral("%1[%2]").arg(key, suffix)).toString();
			if (!value.isEmpty()) {
				return value;
			}
		}
	}
	// A value with commas is read as a list
	return entry.value(key).toStringList().join(", ");
}

QString ProfileActions::IconName(const QString& appId) {
	const QString icon = DesktopEntryValue(appId, "Icon", false);
	return icon.isEmpty() ? QStringLiteral("application-x-executable") : icon;
}

QString ProfileActions::ApplicationName(const QString& appId) {
	return DesktopEntryValue(appId, "Name", true);
}

}
