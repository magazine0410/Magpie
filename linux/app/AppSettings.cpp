#include "AppSettings.h"
#include "Logger.h"

namespace Magpie {

AppSettings::AppSettings(QObject* parent) : QObject(parent) {
	_saveTimer.setSingleShot(true);
	_saveTimer.setInterval(500);
	connect(&_saveTimer, &QTimer::timeout, this, &AppSettings::Flush);
	_snapshot = std::make_shared<const Settings>(_data);
}

AppSettings::~AppSettings() {
	Flush();
}

SettingsStore::LoadResult AppSettings::Load() {
	_data = Settings();
	const SettingsStore::LoadResult result = SettingsStore::Load(_data);
	_canSave = result != SettingsStore::LoadResult::Failed;
	{
		const std::scoped_lock lock(_snapshotMutex);
		_snapshot = std::make_shared<const Settings>(_data);
	}
	return result;
}

void AppSettings::Save() {
	{
		const std::scoped_lock lock(_snapshotMutex);
		_snapshot = std::make_shared<const Settings>(_data);
	}

	if (_canSave) {
		_isSavePending = true;
		_saveTimer.start();
	}
	Q_EMIT Saved();
}

void AppSettings::Flush() {
	_saveTimer.stop();
	if (!_isSavePending) {
		return;
	}
	_isSavePending = false;
	if (!SettingsStore::Save(_data)) {
		Logger::Get().Error("config.json could not be written");
	}
}

std::shared_ptr<const Settings> AppSettings::Snapshot() const {
	const std::scoped_lock lock(_snapshotMutex);
	return _snapshot;
}

}
