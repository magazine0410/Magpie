// The data of the about page. Linux counterpart of AboutViewModel
// (src/Magpie) without the update check, which is not built: updates come
// from the package manager.
#pragma once
#include <QObject>
#include "AppSettings.h"

namespace Magpie {

class AboutViewModel : public QObject {
	Q_OBJECT
	Q_PROPERTY(QString version READ Version CONSTANT)
	Q_PROPERTY(bool isDeveloperMode READ IsDeveloperMode NOTIFY Changed)
	Q_PROPERTY(QString repositoryUrl READ RepositoryUrl CONSTANT)

public:
	explicit AboutViewModel(AppSettings& settings, QObject* parent = nullptr);

	// For example "Version dev | Commit 1a2b3c4 | x86_64"
	QString Version() const;

	// Where this version of Magpie is developed: bug reports and requests
	// about it go there, not to the project of the Windows version
	QString RepositoryUrl() const {
		return MAGPIE_REPOSITORY_URL;
	}

	bool IsDeveloperMode() const {
		return _settings.Data().isDeveloperMode;
	}

	// What a double click on the version with the Alt key held does
	Q_INVOKABLE void enableDeveloperMode();

Q_SIGNALS:
	void Changed();

private:
	AppSettings& _settings;
};

}
