#include "ProfileViewModel.h"
#include <algorithm>
#include <cmath>
#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QGuiApplication>
#include "Application.h"
#include "Paths.h"
#include "ProfileActions.h"
#include "ScalingController.h"
#include "ScreenshotFilenameTemplateHelper.h"

namespace Magpie {

ProfileViewModel::ProfileViewModel(Application& application, int index, QObject* parent) :
	QObject(parent), _application(application), _settings(application.Settings()),
	_index(index < (int)application.Settings().Data().profiles.size() ? index : -1) {}

Profile& ProfileViewModel::_data() const {
	Settings& data = _settings.Data();
	// The page of a profile that was deleted is replaced right away
	return _index >= 0 && _index < (int)data.profiles.size() ? data.profiles[(size_t)_index] : data.defaultProfile;
}

void ProfileViewModel::_Save() {
	_settings.Save();
	Q_EMIT Changed();
}

QString ProfileViewModel::Name() const {
	return IsDefaultProfile() ? qtTrId("Root_Defaults.Content") : QString::fromStdString(_data().name);
}

QString ProfileViewModel::Icon() const {
	return IsDefaultProfile() ? QString() : ProfileActions::IconName(QString::fromStdString(_data().classNameRule));
}

bool ProfileViewModel::CanLaunch() const {
	return !IsDefaultProfile() && !ProfileActions::ProgramToLaunch(_data()).isEmpty();
}

void ProfileViewModel::launch() {
	if (CanLaunch()) {
		ProfileActions::Launch(_data());
	}
}

bool ProfileViewModel::CanOpenProgramLocation() const {
	return !IsDefaultProfile() && !ProfileActions::ProgramLocation(_data()).isEmpty();
}

void ProfileViewModel::openProgramLocation() {
	if (CanOpenProgramLocation()) {
		QDesktopServices::openUrl(QUrl::fromLocalFile(ProfileActions::ProgramLocation(_data())));
	}
}

QUrl ProfileViewModel::LauncherFolder() const {
	// Where the launcher is, otherwise where the program is
	if (!_data().launcherPath.empty()) {
		const QFileInfo launcher(QString::fromStdString(_data().launcherPath.string()));
		if (launcher.dir().exists()) {
			return QUrl::fromLocalFile(launcher.path());
		}
	}
	const QString location = ProfileActions::ProgramLocation(_data());
	return location.isEmpty() ? QUrl() : QUrl::fromLocalFile(location);
}

void ProfileViewModel::changeLauncher(const QUrl& file) {
	const std::string path = file.toLocalFile().toStdString();
	if (IsDefaultProfile() || path.empty() || path == _data().pathRule || path == _data().launcherPath) {
		return;
	}
	_data().launcherPath = path;
	_Save();
}

QStringList ProfileViewModel::ScalingModes() const {
	QStringList scalingModes{ qtTrId("Profile_General_ScalingMode_None") };
	for (const Magpie::ScalingMode& scalingMode : _settings.Data().scalingModes) {
		scalingModes.push_back(QString::fromStdString(scalingMode.name));
	}
	return scalingModes;
}

void ProfileViewModel::SetScalingMode(int value) {
	if (value < 0 || value > (int)_settings.Data().scalingModes.size() || value == ScalingMode()) {
		return;
	}
	_data().scalingMode = value - 1;
	_Save();
}

void ProfileViewModel::SetAutoScale(int value) {
	if (value < 0 || value >= (int)AutoScale::COUNT || value == AutoScale()) {
		return;
	}
	_data().autoScale = (enum AutoScale)value;
	_Save();

	// The program of the profile may be the active window right now
	if (value != 0) {
		_application.Scaling().CheckForeground();
	}
}

void ProfileViewModel::SetIs3DGameMode(bool value) {
	if (Is3DGameMode() == value) {
		return;
	}
	_data().Is3DGameMode(value);
	_Save();
}

bool ProfileViewModel::HasMultipleMonitors() const {
	return QGuiApplication::screens().size() > 1;
}

void ProfileViewModel::SetCustomInitialWindowedScaleFactor(double value) {
	// As the number box of the Windows page: from 1.1 to 10, and 1 for no
	// number at all
	const float factor = std::isnan(value) ? 1.0f : (float)std::clamp(value, 1.1, 10.0);
	if (_data().customInitialWindowedScaleFactor == factor) {
		Q_EMIT Changed();
		return;
	}
	_data().customInitialWindowedScaleFactor = factor;
	_Save();
}

QString ProfileViewModel::InitialToolbarStateDescription() const {
	static constexpr const char* STATE_IDS[]{
		"Profile_Toolbar_InitialState_Off.Content",
		"Profile_Toolbar_InitialState_AlwaysShow.Content",
		"Profile_Toolbar_InitialState_AutoHide.Content"
	};

	const Profile& data = _data();
	const QString fullscreen = qtTrId(STATE_IDS[(size_t)data.fullscreenInitialToolbarState]);
	if (data.fullscreenInitialToolbarState == data.windowedInitialToolbarState) {
		return fullscreen;
	}
	return fullscreen + " | " + qtTrId(STATE_IDS[(size_t)data.windowedInitialToolbarState]);
}

QString ProfileViewModel::ScreenshotSaveDirectory() const {
	return QString::fromStdString(Paths::ScreenshotsDir(_data().screenshotsDir).string());
}

void ProfileViewModel::openScreenshotSaveDirectory() {
	const QString directory = ScreenshotSaveDirectory();
	if (QDir().mkpath(directory)) {
		QDesktopServices::openUrl(QUrl::fromLocalFile(directory));
	}
}

void ProfileViewModel::changeScreenshotSaveDirectory(const QUrl& directory) {
	const std::filesystem::path path = directory.toLocalFile().toStdString();
	if (path.empty() || path == Paths::ScreenshotsDir(_data().screenshotsDir)) {
		return;
	}
	// The system's directory is not named, so that it stays the system's
	_data().screenshotsDir = path == Paths::SystemScreenshotsDir() ? std::filesystem::path() : path;
	_Save();
}

QString ProfileViewModel::ScreenshotFilenameTemplate() const {
	return QString::fromStdString(_data().screenshotFilenameTemplate);
}

void ProfileViewModel::SetScreenshotFilenameTemplate(const QString& value) {
	const std::string text = value.trimmed().toStdString();
	if (_data().screenshotFilenameTemplate == text) {
		return;
	}
	_data().screenshotFilenameTemplate = text;
	_Save();
}

bool ProfileViewModel::IsScreenshotFilenameTemplateValid() const {
	const std::string& text = _data().screenshotFilenameTemplate;
	return text.empty() || ScreenshotFilenameTemplateHelper::IsValid(text);
}

QStringList ProfileViewModel::GraphicsCards() const {
	const std::vector<GraphicsCardEntry>& cards = _application.GraphicsCards();
	QStringList names;
	if (cards.size() > 1) {
		names.push_back(qtTrId("Profile_General_CaptureMethod_Default") + " (" + cards[0].name + ")");
		for (const GraphicsCardEntry& card : cards) {
			names.push_back(card.name);
		}
	}
	return names;
}

int ProfileViewModel::GraphicsCard() const {
	const GraphicsCardId& id = _data().graphicsCardId;
	if (id.idx < 0) {
		return 0;
	}
	const std::vector<GraphicsCardEntry>& cards = _application.GraphicsCards();
	const auto it = std::ranges::find(cards, id.idx, &GraphicsCardEntry::idx);
	return it == cards.end() ? 0 : int(it - cards.begin()) + 1;
}

void ProfileViewModel::SetGraphicsCard(int value) {
	const std::vector<GraphicsCardEntry>& cards = _application.GraphicsCards();
	GraphicsCardId id;
	if (value > 0 && value - 1 < (int)cards.size()) {
		const GraphicsCardEntry& card = cards[(size_t)value - 1];
		id = { .idx = card.idx, .vendorId = card.vendorId, .deviceId = card.deviceId };
	}
	if (value < 0 || _data().graphicsCardId == id) {
		return;
	}
	_data().graphicsCardId = id;
	_Save();
}

bool ProfileViewModel::IsShowGraphicsCardSettingsCard() const {
	return _application.GraphicsCards().size() > 1;
}

bool ProfileViewModel::IsNoGraphicsCard() const {
	return _application.GraphicsCards().empty();
}

void ProfileViewModel::SetMaxFrameRate(double value) {
	const float rate = std::isnan(value) ? 60.0f : (float)std::clamp(value, 10.0, 1000.0);
	if (_data().maxFrameRate == rate) {
		Q_EMIT Changed();
		return;
	}
	_data().maxFrameRate = rate;
	_Save();
}

void ProfileViewModel::SetIsCaptureTitleBar(bool value) {
	if (IsCaptureTitleBar() == value) {
		return;
	}
	_data().IsCaptureTitleBar(value);
	_Save();
}

void ProfileViewModel::_SetCropping(float& side, double value) {
	const float pixels = std::isnan(value) ? 0.0f : (float)std::max(value, 0.0);
	if (side == pixels) {
		Q_EMIT Changed();
		return;
	}
	side = pixels;
	_Save();
}

void ProfileViewModel::SetCroppingLeft(double value) {
	_SetCropping(_data().cropping.Left, value);
}

void ProfileViewModel::SetCroppingTop(double value) {
	_SetCropping(_data().cropping.Top, value);
}

void ProfileViewModel::SetCroppingRight(double value) {
	_SetCropping(_data().cropping.Right, value);
}

void ProfileViewModel::SetCroppingBottom(double value) {
	_SetCropping(_data().cropping.Bottom, value);
}

void ProfileViewModel::SetCustomCursorScaleFactor(double value) {
	const float factor = std::isnan(value) ? 1.0f : (float)std::clamp(value, 0.1, 10.0);
	if (_data().customCursorScaleFactor == factor) {
		Q_EMIT Changed();
		return;
	}
	_data().customCursorScaleFactor = factor;
	_Save();
}

void ProfileViewModel::SetAutoHideCursorDelay(double value) {
	const float delay = std::isnan(value) ? 3.0f : (float)std::clamp(value, 0.1, 5.0);
	if (_data().autoHideCursorDelay == delay) {
		return;
	}
	_data().autoHideCursorDelay = delay;
	_Save();
}

void ProfileViewModel::SetIsAdjustCursorSpeed(bool value) {
	if (IsAdjustCursorSpeed() == value) {
		return;
	}
	_data().IsAdjustCursorSpeed(value);
	_Save();
}

QString ProfileViewModel::LaunchParameters() const {
	return QString::fromStdString(_data().launchParameters);
}

void ProfileViewModel::SetLaunchParameters(const QString& value) {
	const std::string text = value.trimmed().toStdString();
	if (_data().launchParameters == text) {
		return;
	}
	_data().launchParameters = text;
	_Save();
}

}
