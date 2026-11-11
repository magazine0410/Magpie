#include "RootViewModel.h"
#include <QFileInfo>
#include <algorithm>
#include <QCollator>
#include <QDesktopServices>
#include <QUrl>
#include "ProcessInfo.h"
#include "ProfileActions.h"
#include "ProfileRules.h"
#include "ScalingController.h"

namespace Magpie {

RootViewModel::RootViewModel(AppSettings& settings, ScalingController& scaling, QObject* parent) :
	QObject(parent), _settings(settings), _scaling(scaling) {}

bool RootViewModel::_IsValid(int profile) const {
	return profile >= 0 && profile < (int)_settings.Data().profiles.size();
}

void RootViewModel::_Save() {
	_settings.Save();
	Q_EMIT profilesChanged();
	// Another profile may apply to the active window now
	_scaling.CheckForeground();
}

QVariantList RootViewModel::Profiles() const {
	QVariantList profiles;
	for (const Profile& profile : _settings.Data().profiles) {
		profiles.push_back(QVariantMap{
			{ "name", QString::fromStdString(profile.name) },
			{ "icon", ProfileActions::IconName(QString::fromStdString(profile.classNameRule)) },
			{ "canLaunch", !ProfileActions::ProgramToLaunch(profile).isEmpty() }
		});
	}
	return profiles;
}

bool RootViewModel::canRenameProfile(int profile, const QString& name) const {
	const QString trimmed = name.trimmed();
	return _IsValid(profile) && !trimmed.isEmpty() &&
		trimmed.toStdString() != _settings.Data().profiles[(size_t)profile].name;
}

void RootViewModel::renameProfile(int profile, const QString& name) {
	if (!canRenameProfile(profile, name)) {
		return;
	}
	_settings.Data().profiles[(size_t)profile].name = name.trimmed().toStdString();
	_Save();
}

void RootViewModel::removeProfile(int profile) {
	if (!_IsValid(profile)) {
		return;
	}
	std::vector<Profile>& profiles = _settings.Data().profiles;
	profiles.erase(profiles.begin() + profile);
	_Save();
	Q_EMIT profileRemoved(profile);
}

void RootViewModel::moveProfile(int profile, bool up) {
	const int target = up ? profile - 1 : profile + 1;
	if (!_IsValid(profile) || !_IsValid(target)) {
		return;
	}
	std::vector<Profile>& profiles = _settings.Data().profiles;
	std::swap(profiles[(size_t)profile], profiles[(size_t)target]);
	_Save();
	Q_EMIT profileMoved(profile, target);
}

void RootViewModel::Launch(const Profile& profile, ScalingController& scaling) {
	if (!profile.isLaunchedNested && scaling.HasDesktopBackEnd()) {
		ProfileActions::Launch(profile);
		return;
	}

	const QStringList command = ProfileActions::LaunchCommand(profile);
	if (command.isEmpty()) {
		return;
	}
	ScalingService::LaunchRequest request;
	for (const QString& argument : command) {
		request.command.push_back(argument.toStdString());
	}
	request.workingDirectory = QFileInfo(command[0]).path().toStdString();
	request.profileName = profile.name;
	// The way the profile scales by itself, fullscreen if it does not
	request.windowedMode = profile.autoScale == AutoScale::Windowed;
	scaling.LaunchNested(std::move(request));
}

void RootViewModel::launch(int profile) {
	if (_IsValid(profile)) {
		Launch(_settings.Data().profiles[(size_t)profile], _scaling);
	}
}

bool RootViewModel::canOpenProgramLocation(int profile) const {
	return _IsValid(profile) && !ProfileActions::ProgramLocation(_settings.Data().profiles[(size_t)profile]).isEmpty();
}

void RootViewModel::openProgramLocation(int profile) {
	if (canOpenProgramLocation(profile)) {
		QDesktopServices::openUrl(QUrl::fromLocalFile(
			ProfileActions::ProgramLocation(_settings.Data().profiles[(size_t)profile])));
	}
}

void RootViewModel::prepareNewProfile() {
	_scaling.ListWindows(this, [this](std::vector<ScalingService::Window> windows) {
		// As NewProfileViewModel on Windows: windows with a title and some
		// size for which no profile exists yet, ordered by title
		std::erase_if(windows, [&](const ScalingService::Window& window) {
			const Rect& rect = window.info.frameRect;
			return window.info.title.empty() ||
				(rect.right - rect.left < 50 && rect.bottom - rect.top < 50) ||
				!TestNewProfile(_settings.Data(), window.className, window.path);
		});

		QCollator collator;
		collator.setCaseSensitivity(Qt::CaseInsensitive);
		std::ranges::sort(windows, [&](const ScalingService::Window& l, const ScalingService::Window& r) {
			return collator.compare(QString::fromStdString(l.info.title), QString::fromStdString(r.info.title)) < 0;
		});

		_candidateWindows = std::move(windows);
		Q_EMIT candidateWindowsChanged();
	});
}

QVariantList RootViewModel::CandidateWindows() const {
	QVariantList windows;
	for (const ScalingService::Window& window : _candidateWindows) {
		windows.push_back(QVariantMap{
			{ "title", QString::fromStdString(window.info.title) },
			{ "icon", ProfileActions::IconName(QString::fromStdString(window.className)) }
		});
	}
	return windows;
}

QString RootViewModel::windowTitle(int window) const {
	return window >= 0 && window < (int)_candidateWindows.size() ?
		QString::fromStdString(_candidateWindows[(size_t)window].info.title) : QString();
}

QString RootViewModel::processName(int window) const {
	return window >= 0 && window < (int)_candidateWindows.size() ?
		QString::fromStdString(ProcessInfo::NameFromCommand(_candidateWindows[(size_t)window].path)) : QString();
}

QString RootViewModel::defaultProfileName(int window) const {
	if (window < 0 || window >= (int)_candidateWindows.size()) {
		return {};
	}
	const QString name = ProfileActions::ApplicationName(
		QString::fromStdString(_candidateWindows[(size_t)window].className));
	return name.isEmpty() ? windowTitle(window) : name;
}

QStringList RootViewModel::copyFromList() const {
	QStringList list{ qtTrId("Root_Defaults.Content") };
	for (const Profile& profile : _settings.Data().profiles) {
		list.push_back(QString::fromStdString(profile.name));
	}
	return list;
}

bool RootViewModel::canAddProfile(int window, const QString& name) const {
	return window >= 0 && window < (int)_candidateWindows.size() && !name.trimmed().isEmpty();
}

void RootViewModel::addProfile(int window, const QString& name, int copyFrom) {
	if (canAddProfile(window, name)) {
		AddProfile(_candidateWindows[(size_t)window], name, copyFrom);
	}
}

bool RootViewModel::AddProfile(const ScalingService::Window& window, const QString& name, int copyFrom) {
	Settings& data = _settings.Data();
	if (name.trimmed().isEmpty() || !TestNewProfile(data, window.className, window.path)) {
		return false;
	}

	Profile profile;
	profile.Copy(_IsValid(copyFrom - 1) ? data.profiles[(size_t)copyFrom - 1] : data.defaultProfile);
	profile.name = name.trimmed().toStdString();
	profile.classNameRule = window.className;
	profile.pathRule = window.path;
	data.profiles.push_back(std::move(profile));

	_Save();
	Q_EMIT profileAdded((int)data.profiles.size() - 1);
	return true;
}

}
