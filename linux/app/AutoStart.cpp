#include "AutoStart.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>

namespace Magpie {

QString AutoStart::FilePath() {
	return QStandardPaths::writableLocation(QStandardPaths::ConfigLocation) +
		"/autostart/" MAGPIE_APP_ID ".desktop";
}

bool AutoStart::IsEnabled() {
	return QFileInfo::exists(FilePath());
}

QString AutoStart::Entry(const QString& executable) {
	// In the Exec key an argument in double quotes may hold any character;
	// four of them need a backslash
	QString quoted;
	for (const QChar c : executable) {
		if (c == '"' || c == '`' || c == '$' || c == '\\') {
			quoted += '\\';
		}
		quoted += c;
	}

	return QStringLiteral(
		"[Desktop Entry]\n"
		"Type=Application\n"
		"Name=Magpie\n"
		"Exec=\"%1\" -t\n"
		"Icon=" MAGPIE_APP_ID "\n"
		"Terminal=false\n").arg(quoted);
}

bool AutoStart::Enable(const QString& executable) {
	const QString path = FilePath();
	if (!QDir().mkpath(QFileInfo(path).path())) {
		return false;
	}

	QFile file(path);
	if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
		return false;
	}
	const QByteArray entry = Entry(executable).toUtf8();
	return file.write(entry) == entry.size();
}

bool AutoStart::Disable() {
	return !QFileInfo::exists(FilePath()) || QFile::remove(FilePath());
}

}
