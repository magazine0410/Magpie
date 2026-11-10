#include "MainWindow.h"
#include <limits>
#include <QLocale>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickWindow>
#include "AboutViewModel.h"
#include "Application.h"
#include "HomeViewModel.h"
#include "ProfileViewModel.h"
#include "RootViewModel.h"
#include "ScalingModesViewModel.h"
#include "SettingsViewModel.h"

namespace Magpie {

MainWindow::MainWindow(Application& application) :
	_application(application),
	_root(std::make_unique<RootViewModel>(application.Settings(), application.Scaling())),
	_home(std::make_unique<HomeViewModel>(application.Settings(), application.Scaling())),
	_scalingModes(std::make_unique<ScalingModesViewModel>(application.Settings(), application.Effects())),
	_settings(std::make_unique<SettingsViewModel>(application)),
	_about(std::make_unique<AboutViewModel>(application.Settings())) {}

MainWindow::~MainWindow() = default;

QObject* MainWindow::Root() const {
	return _root.get();
}

QObject* MainWindow::profile(int index) {
	ProfileViewModel* viewModel = new ProfileViewModel(_application, index);
	QQmlEngine::setObjectOwnership(viewModel, QQmlEngine::JavaScriptOwnership);
	return viewModel;
}

QObject* MainWindow::Home() const {
	return _home.get();
}

QObject* MainWindow::ScalingModes() const {
	return _scalingModes.get();
}

QObject* MainWindow::Settings() const {
	return _settings.get();
}

QObject* MainWindow::About() const {
	return _about.get();
}

bool MainWindow::Create() {
	_engine = std::make_unique<QQmlApplicationEngine>();
	_engine->rootContext()->setContextProperty("app", this);
	_engine->load(QUrl("qrc:/qml/Main.qml"));
	if (_engine->rootObjects().isEmpty()) {
		return false;
	}

	_window = qobject_cast<QQuickWindow*>(_engine->rootObjects().first());
	return _window != nullptr;
}

void MainWindow::Show() {
	if (!_window->isVisible()) {
		// A desktop on Wayland places the window itself, so of what the
		// settings hold only the size and the maximized state are used
		const Magpie::Settings& data = _application.Settings().Data();
		if (data.mainWindowWidth > 0 && data.mainWindowHeight > 0) {
			_window->resize((int)data.mainWindowWidth, (int)data.mainWindowHeight);
		}
		if (data.isMainWindowMaximized) {
			_window->showMaximized();
		} else {
			_window->show();
		}
	}
	_window->raise();
	_window->requestActivate();
}

QString MainWindow::formatNumber(double value) const {
	QLocale locale;
	locale.setNumberOptions(QLocale::OmitGroupSeparator);
	QString text = locale.toString(value, 'f', 5);
	const QString point = locale.decimalPoint();
	if (text.contains(point)) {
		while (text.endsWith('0')) {
			text.chop(1);
		}
		if (text.endsWith(point)) {
			text.chop(point.size());
		}
	}
	return text;
}

double MainWindow::parseNumber(const QString& text) const {
	bool ok = false;
	const double value = QLocale().toDouble(text.trimmed(), &ok);
	return ok ? value : std::numeric_limits<double>::quiet_NaN();
}

void MainWindow::onClosing() {
	Magpie::Settings& data = _application.Settings().Data();
	const bool isMaximized = _window->visibility() == QWindow::Maximized;
	bool isChanged = data.isMainWindowMaximized != isMaximized;
	data.isMainWindowMaximized = isMaximized;
	// The size of the maximized window is not the one to come back to
	if (!isMaximized && (data.mainWindowWidth != (float)_window->width() ||
		data.mainWindowHeight != (float)_window->height())) {
		data.mainWindowWidth = (float)_window->width();
		data.mainWindowHeight = (float)_window->height();
		isChanged = true;
	}
	if (isChanged) {
		_application.Settings().Save();
	}
	Q_EMIT Closed();
}

}
