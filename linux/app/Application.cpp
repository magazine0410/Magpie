#include "Application.h"
#include <QAction>
#include <QCoreApplication>
#include <QGuiApplication>
#include <QIcon>
#include <QMenu>
#include <QMessageBox>
#include <QProcess>
#include <QStyleHints>
#include <QSystemTrayIcon>
#include "Localization.h"
#include "Logger.h"
#include "Paths.h"
#include "MainWindow.h"
#include "ScalingController.h"
#include "SettingsStore.h"
#include "VulkanDevice.h"

namespace Magpie {

Application::Application(QObject* parent) : QObject(parent) {}

Application::~Application() {
	// Before the settings, which they use
	_mainWindow.reset();
	_scaling.reset();
	if (_graphicsCardsFuture.valid()) {
		_graphicsCardsFuture.wait();
	}
}

bool Application::Initialize(const Options& options) {
	const SettingsStore::LoadResult loadResult = _settings.Load();
	_initialLanguage = _settings.Data().language;
	Localization::Initialize(_initialLanguage);

	if (loadResult == SettingsStore::LoadResult::Failed) {
		// As on Windows: the file is left alone and the program ends
		QMessageBox box(
			QMessageBox::Critical,
			qtTrId("AppSettings_Dialog_Error"),
			qtTrId("AppSettings_ErrorDialog_ReadFailed"));
		box.setInformativeText(Localization::Format("AppSettings_ErrorDialog_ConfigLocation",
			QString::fromStdString(SettingsStore::ConfigPath().string())));
		box.addButton(qtTrId("AppSettings_Dialog_Exit"), QMessageBox::RejectRole);
		box.exec();
		return false;
	}
	if (loadResult == SettingsStore::LoadResult::Initial) {
		// The first start writes the file
		_settings.Save();
		_settings.Flush();
	}

	ApplyTheme();

	// Read in the background; the scaling modes page needs them first
	_effects.Initialize(Paths::EffectsDir(MAGPIE_DEFAULT_EFFECTS_DIR));

	_graphicsCardsFuture = std::async(std::launch::async, [] {
		std::vector<GraphicsCardEntry> cards;
		VulkanDevice device;
		if (device.Initialize({})) {
			int idx = 0;
			for (const GraphicsCardInfo& card : device.GraphicsCards()) {
				if (card.isUsable && !card.isSoftware) {
					cards.push_back({ idx, card.vendorId, card.deviceId, QString::fromStdString(card.name) });
				}
				++idx;
			}
		}
		return cards;
	});

	_scaling = std::make_unique<ScalingController>(_settings);
	connect(_scaling.get(), &ScalingController::ShowWindowRequested, this, &Application::ShowMainWindow);
	if (options.startEngine) {
		_scaling->Start({
			.effectsDir = Paths::EffectsDir(MAGPIE_DEFAULT_EFFECTS_DIR),
			.iconFontPath = Paths::IconFont(MAGPIE_DEFAULT_ICON_FONT),
			.appId = MAGPIE_APP_ID,
			.objectPath = MAGPIE_OBJECT_PATH
		});
	}

	UpdateTrayIcon();

	// Without a tray icon there would be no way to reach the program
	if (!(options.startInTray && _IsTrayIconShown())) {
		ShowMainWindow();
	}
	return true;
}

const std::vector<GraphicsCardEntry>& Application::GraphicsCards() {
	if (_graphicsCardsFuture.valid()) {
		_graphicsCards = _graphicsCardsFuture.get();
	}
	return _graphicsCards;
}

void Application::ApplyTheme() {
	Qt::ColorScheme scheme = Qt::ColorScheme::Unknown;
	if (_settings.Data().theme == AppTheme::Light) {
		scheme = Qt::ColorScheme::Light;
	} else if (_settings.Data().theme == AppTheme::Dark) {
		scheme = Qt::ColorScheme::Dark;
	}
	QGuiApplication::styleHints()->setColorScheme(scheme);
}

bool Application::_IsTrayIconShown() const {
	return _trayIcon && _trayIcon->isVisible();
}

void Application::UpdateTrayIcon() {
	if (!_settings.Data().isShowNotifyIcon || !QSystemTrayIcon::isSystemTrayAvailable()) {
		_trayIcon.reset();
		_trayMenu.reset();
		return;
	}
	if (_trayIcon) {
		return;
	}

	// As NotifyIconService on Windows: the main window, the two countdowns
	// and the exit
	_trayMenu = std::make_unique<QMenu>();
	_trayMenu->addAction(qtTrId("NotifyIcon_MainWindow"), this, &Application::ShowMainWindow);
	QAction* fullscreenTimer = _trayMenu->addAction(QString(), this, [this] { _scaling->StartTimer(false); });
	QAction* windowedTimer = _trayMenu->addAction(QString(), this, [this] { _scaling->StartTimer(true); });
	_trayMenu->addAction(qtTrId("NotifyIcon_Exit"), this, &Application::Quit);

	// The length of the countdown may have changed since the menu was shown
	const auto updateTimerTexts = [this, fullscreenTimer, windowedTimer] {
		const QString seconds = QString::number(_settings.Data().countdownSeconds);
		fullscreenTimer->setText(Localization::Format("NotifyIcon_Timer_Fullscreen", seconds));
		windowedTimer->setText(Localization::Format("NotifyIcon_Timer_Windowed", seconds));
	};
	updateTimerTexts();
	connect(_trayMenu.get(), &QMenu::aboutToShow, this, updateTimerTexts);

	_trayIcon = std::make_unique<QSystemTrayIcon>(QIcon(":/icons/magpie.svg"));
	_trayIcon->setToolTip("Magpie");
	_trayIcon->setContextMenu(_trayMenu.get());
	connect(_trayIcon.get(), &QSystemTrayIcon::activated, this, [this](QSystemTrayIcon::ActivationReason reason) {
		if (reason == QSystemTrayIcon::Trigger) {
			ShowMainWindow();
		}
	});
	_trayIcon->show();
}

void Application::ShowMainWindow() {
	if (!_mainWindow) {
		_mainWindow = std::make_unique<MainWindow>(*this);
		connect(_mainWindow.get(), &MainWindow::Closed, this, &Application::_OnMainWindowClosed,
			Qt::QueuedConnection);
		if (!_mainWindow->Create()) {
			Logger::Get().Error("The main window could not be created");
			_mainWindow.reset();
			Quit();
			return;
		}
	}
	_mainWindow->Show();
}

void Application::_OnMainWindowClosed() {
	// The window is freed while it is closed, as on Windows
	_mainWindow.reset();
	_settings.Flush();

	// Without a tray icon, closing the main window ends the program
	if (!_IsTrayIconShown()) {
		Quit();
	}
}

void Application::Quit() {
	QCoreApplication::quit();
}

void Application::Restart() {
	_mainWindow.reset();
	_settings.Flush();
	// Frees the name on D-Bus, which the new process takes
	_scaling->Stop();
	QProcess::startDetached(QCoreApplication::applicationFilePath(), {});
	Quit();
}

}
