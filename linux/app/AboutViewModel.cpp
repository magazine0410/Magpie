#include "AboutViewModel.h"
#include <QSysInfo>
#include <QtGlobal>

namespace Magpie {

AboutViewModel::AboutViewModel(AppSettings& settings, QObject* parent) : QObject(parent), _settings(settings) {}

QString AboutViewModel::Version() const {
	QString version = qtTrId("About_Version_Version") + " " MAGPIE_VERSION_STRING;
	if (MAGPIE_COMMIT_ID[0]) {
		version += " | " + qtTrId("About_Version_CommitId") + " " MAGPIE_COMMIT_ID;
	}
	return version + " | " + QSysInfo::buildCpuArchitecture();
}

void AboutViewModel::enableDeveloperMode() {
	if (_settings.Data().isDeveloperMode) {
		return;
	}
	_settings.Data().isDeveloperMode = true;
	_settings.Save();
	Q_EMIT Changed();
}

}
