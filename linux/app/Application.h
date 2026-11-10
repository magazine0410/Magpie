// The application: the settings, the scaling service, the tray icon and the
// main window, which exists only while it is shown. Linux counterpart of the
// App class of src/Magpie.
#pragma once
#include <memory>
#include <string>
#include <QObject>
#include "AppSettings.h"
#include "EffectsService.h"

#include <future>
#include <vector>
#include <QString>

class QMenu;
class QSystemTrayIcon;

namespace Magpie {

class MainWindow;
class ScalingController;

// A graphics card the user can choose for scaling
struct GraphicsCardEntry {
	// As GraphicsCardId has them
	int idx = -1;
	uint32_t vendorId = 0;
	uint32_t deviceId = 0;
	QString name;
};

class Application : public QObject {
	Q_OBJECT

public:
	struct Options {
		// Do not show the main window if the tray icon is shown; what the
		// autostart entry asks for
		bool startInTray = false;
		// False: the scaling service is not started. For tests.
		bool startEngine = true;
	};

	explicit Application(QObject* parent = nullptr);
	~Application() override;

	// Returns false if the program is to end; the user has been told why.
	bool Initialize(const Options& options);

	AppSettings& Settings() noexcept {
		return _settings;
	}

	ScalingController& Scaling() noexcept {
		return *_scaling;
	}

	EffectsService& Effects() noexcept {
		return _effects;
	}

	// The graphics cards that can scale, without software implementations.
	// They are listed in the background from the start of the program;
	// whoever asks before that is done waits. Counterpart of
	// AdaptersService on Windows, without the watching for changes.
	const std::vector<GraphicsCardEntry>& GraphicsCards();

	// The language setting the program started with; another one takes
	// effect after a restart
	const std::string& InitialLanguage() const noexcept {
		return _initialLanguage;
	}

	// Light, dark or as the desktop, as the setting says
	void ApplyTheme();

	// Shows or hides the tray icon, as the setting says
	void UpdateTrayIcon();

	// Null while the main window is closed
	MainWindow* GetMainWindow() noexcept {
		return _mainWindow.get();
	}

	void ShowMainWindow();
	void Quit();
	void Restart();

private:
	bool _IsTrayIconShown() const;
	void _OnMainWindowClosed();

	AppSettings _settings;
	EffectsService _effects;
	std::future<std::vector<GraphicsCardEntry>> _graphicsCardsFuture;
	std::vector<GraphicsCardEntry> _graphicsCards;
	std::string _initialLanguage;
	std::unique_ptr<ScalingController> _scaling;
	std::unique_ptr<MainWindow> _mainWindow;
	std::unique_ptr<QMenu> _trayMenu;
	std::unique_ptr<QSystemTrayIcon> _trayIcon;
};

}
