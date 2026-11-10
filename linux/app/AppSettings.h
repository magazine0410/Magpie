// The settings of the running application: it reads config.json at start,
// writes it shortly after a change and hands the scaling service a copy that
// does not change under it. Linux counterpart of the AppSettings class of
// src/Magpie, whose data members are the Settings structure of linux/core.
#pragma once
#include <memory>
#include <mutex>
#include <QObject>
#include <QTimer>
#include "Settings.h"
#include "SettingsStore.h"

namespace Magpie {

class AppSettings : public QObject {
	Q_OBJECT

public:
	explicit AppSettings(QObject* parent = nullptr);
	// Writes what has not been written yet
	~AppSettings() override;

	// Reads config.json, or takes the initial settings if there is none.
	// After a failure nothing is ever written, so the file stays as it is.
	SettingsStore::LoadResult Load();

	// What the user interface reads and changes. After a change call Save.
	Settings& Data() noexcept {
		return _data;
	}

	const Settings& Data() const noexcept {
		return _data;
	}

	// Makes the change known to the scaling service at once and writes the
	// file a moment later, so that a row of changes makes one write. As
	// AppSettings::SaveAsync on Windows.
	void Save();

	// Writes now if something is waiting to be written
	void Flush();

	// The settings as of the last Save. Callable from any thread.
	std::shared_ptr<const Settings> Snapshot() const;

Q_SIGNALS:
	// After every Save
	void Saved();

private:
	Settings _data;
	QTimer _saveTimer;
	bool _canSave = false;
	bool _isSavePending = false;

	mutable std::mutex _snapshotMutex;
	std::shared_ptr<const Settings> _snapshot;
};

}
