// The profiles as the main window lists them, and the making of a new one.
// Linux counterpart of the profile handling of RootPage, of
// NewProfileViewModel and of ProfileService (src/Magpie).
#pragma once
#include <vector>
#include <QObject>
#include <QStringList>
#include <QVariantList>
#include "AppSettings.h"
#include "ScalingService.h"

namespace Magpie {

class ScalingController;

class RootViewModel : public QObject {
	Q_OBJECT
	// For every profile a map with "name", "icon" and "canLaunch"
	Q_PROPERTY(QVariantList profiles READ Profiles NOTIFY profilesChanged)
	// The windows a profile can be made for: maps with "title" and "icon"
	Q_PROPERTY(QVariantList candidateWindows READ CandidateWindows NOTIFY candidateWindowsChanged)

public:
	RootViewModel(AppSettings& settings, ScalingController& scaling, QObject* parent = nullptr);

	QVariantList Profiles() const;

	Q_INVOKABLE bool canRenameProfile(int profile, const QString& name) const;
	Q_INVOKABLE void renameProfile(int profile, const QString& name);
	Q_INVOKABLE void removeProfile(int profile);
	Q_INVOKABLE void moveProfile(int profile, bool up);
	Q_INVOKABLE void launch(int profile);

	// Starts the program of a profile: in a nested session if the profile
	// says so or the desktop has no back end, otherwise as it is, to be
	// scaled like any window
	static void Launch(const Profile& profile, ScalingController& scaling);
	Q_INVOKABLE void openProgramLocation(int profile);
	Q_INVOKABLE bool canOpenProgramLocation(int profile) const;

	// Asks the desktop for the windows; candidateWindows changes when the
	// answer is there
	Q_INVOKABLE void prepareNewProfile();
	QVariantList CandidateWindows() const;
	// What the new profile is called unless the user says otherwise: the
	// name of the application, or the title of the window
	Q_INVOKABLE QString defaultProfileName(int window) const;
	Q_INVOKABLE QString windowTitle(int window) const;
	Q_INVOKABLE QString processName(int window) const;
	// "Defaults" first, then the names of the profiles
	Q_INVOKABLE QStringList copyFromList() const;
	Q_INVOKABLE bool canAddProfile(int window, const QString& name) const;
	// `copyFrom` is an index into copyFromList
	Q_INVOKABLE void addProfile(int window, const QString& name, int copyFrom);

	// Without the desktop: for a window that is known already
	bool AddProfile(const ScalingService::Window& window, const QString& name, int copyFrom);

Q_SIGNALS:
	void profilesChanged();
	void candidateWindowsChanged();
	// What the window does next: show the new profile, the defaults, or
	// the profile at its new place
	void profileAdded(int profile);
	void profileRemoved(int profile);
	void profileMoved(int from, int to);

private:
	bool _IsValid(int profile) const;
	void _Save();

	AppSettings& _settings;
	ScalingController& _scaling;
	std::vector<ScalingService::Window> _candidateWindows;
};

}
