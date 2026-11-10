// The main window: the QML engine that shows it, and the view models of its
// pages, which QML reaches as properties of the object "app".
#pragma once
#include <memory>
#include <QObject>

class QQmlApplicationEngine;
class QQuickWindow;

namespace Magpie {

class AboutViewModel;
class Application;
class HomeViewModel;
class RootViewModel;
class ScalingModesViewModel;
class SettingsViewModel;

class MainWindow : public QObject {
	Q_OBJECT
	Q_PROPERTY(QObject* root READ Root CONSTANT)
	Q_PROPERTY(QObject* home READ Home CONSTANT)
	Q_PROPERTY(QObject* scalingModes READ ScalingModes CONSTANT)
	Q_PROPERTY(QObject* settings READ Settings CONSTANT)
	Q_PROPERTY(QObject* about READ About CONSTANT)

public:
	explicit MainWindow(Application& application);
	~MainWindow() override;

	// Loads the window without showing it. Returns false if the QML files
	// have errors.
	bool Create();

	// Shows the window with the size it had last, or brings it to the front
	void Show();

	QQuickWindow* Window() const noexcept {
		return _window;
	}

	QObject* Root() const;
	QObject* Home() const;
	QObject* ScalingModes() const;
	QObject* Settings() const;
	QObject* About() const;

	// The data of a profile page; the page owns it. A negative index gives
	// the default profile.
	Q_INVOKABLE QObject* profile(int index);

	// A number as the pages show it: with at most five decimals and the
	// decimal separator of the language. As the DoubleFormatter of the
	// Windows user interface.
	Q_INVOKABLE QString formatNumber(double value) const;
	// The number of a text the user entered; not a number if it is none
	Q_INVOKABLE double parseNumber(const QString& text) const;

	// Called by the window when the user closes it
	Q_INVOKABLE void onClosing();

Q_SIGNALS:
	void Closed();

private:
	Application& _application;
	std::unique_ptr<RootViewModel> _root;
	std::unique_ptr<HomeViewModel> _home;
	std::unique_ptr<ScalingModesViewModel> _scalingModes;
	std::unique_ptr<SettingsViewModel> _settings;
	std::unique_ptr<AboutViewModel> _about;
	// After the view models, so that it is destroyed before them
	std::unique_ptr<QQmlApplicationEngine> _engine;
	QQuickWindow* _window = nullptr;
};

}
