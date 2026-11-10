// magpie: the settings application with the scaling service inside. Linux
// counterpart of the Windows program Magpie.exe.
#include <csignal>
#include <cstdio>
#include <string_view>
#include <sys/signalfd.h>
#include <unistd.h>
#include <QApplication>
#include <QIcon>
#include <QSocketNotifier>
#include <systemd/sd-bus.h>
#include "Application.h"
#include "Logger.h"
#include "Paths.h"

using namespace Magpie;

static void PrintUsage() {
	std::puts(
		"Usage: magpie [options]\n"
		"\n"
		"Shows Magpie's main window. If Magpie runs already, that one shows its window.\n"
		"\n"
		"Options:\n"
		"  -t  start without the main window if the tray icon is shown");
}

// Asks a running Magpie for its main window. False if none is running.
static bool ShowRunningInstance() {
	sd_bus* bus = nullptr;
	if (sd_bus_open_user(&bus) < 0) {
		return false;
	}
	const int result = sd_bus_call_method(bus, MAGPIE_APP_ID, MAGPIE_OBJECT_PATH, MAGPIE_APP_ID,
		"ShowWindow", nullptr, nullptr, "");
	sd_bus_flush_close_unref(bus);
	return result >= 0;
}

int main(int argc, char* argv[]) {
	Application::Options options;
	for (int i = 1; i < argc; ++i) {
		const std::string_view arg = argv[i];
		if (arg == "-t") {
			options.startInTray = true;
		} else {
			PrintUsage();
			return arg == "-h" || arg == "--help" ? 0 : 1;
		}
	}

	if (ShowRunningInstance()) {
		std::puts("Magpie is running already.");
		return 0;
	}

	// The signals that end the program arrive through a descriptor, so that
	// scaling is stopped properly and the scaled window gets its place back.
	// They are blocked before anything starts a thread, because a thread
	// without the block would take them.
	sigset_t signals;
	sigemptyset(&signals);
	sigaddset(&signals, SIGINT);
	sigaddset(&signals, SIGTERM);
	sigprocmask(SIG_BLOCK, &signals, nullptr);
	const int signalFd = signalfd(-1, &signals, SFD_NONBLOCK | SFD_CLOEXEC);

	Logger::Get().Initialize(spdlog::level::info, Paths::LogsDir() / "magpie.log", 500000, 1);

	// Under this name the desktop knows the program: it finds the .desktop
	// file with it, which has the icon of the windows. Set first, because
	// Qt tells the desktop the name when the application object is made.
	QApplication::setDesktopFileName(MAGPIE_APP_ID);
	QApplication qtApplication(argc, argv);
	QApplication::setApplicationName("Magpie");
	QApplication::setWindowIcon(QIcon(":/icons/magpie.svg"));
	// The program goes on in the tray
	QApplication::setQuitOnLastWindowClosed(false);

	QSocketNotifier signalNotifier(signalFd, QSocketNotifier::Read);
	QObject::connect(&signalNotifier, &QSocketNotifier::activated, &qtApplication, &QApplication::quit);

	Application application;
	if (!application.Initialize(options)) {
		return 1;
	}
	return QApplication::exec();
}
