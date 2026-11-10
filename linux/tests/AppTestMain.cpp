// The tests of the settings application run with a QApplication that shows
// nothing: the platform "offscreen" and the software renderer of Qt Quick.
// Configuration and autostart entry go to a temporary directory.
#include <cstdlib>
#include <catch2/catch_session.hpp>
#include <QApplication>
#include <QQuickWindow>
#include <QTemporaryDir>
#include "AppTestHelpers.h"
#include "Localization.h"

static QStringList messages;

static void MessageHandler(QtMsgType type, const QMessageLogContext& context, const QString& message) {
	// Not about the application: the platform "offscreen" says the first
	// whenever a window is brought to the front, and the second comes from
	// Qt's start in a session whose portal knows the test program already
	if ((type == QtWarningMsg || type == QtCriticalMsg || type == QtFatalMsg) &&
		!message.contains("does not support raise()") &&
		!message.contains("register with host portal")) {
		messages.push_back(QStringLiteral("%1:%2: %3").arg(context.file ? context.file : "").arg(context.line).arg(message));
	}
	std::fprintf(stderr, "%s\n", qPrintable(message));
}

QStringList TakeQtWarnings() {
	return std::exchange(messages, {});
}

int main(int argc, char* argv[]) {
	QTemporaryDir directory;
	// Before anything asks for them
	setenv("MAGPIE_CONFIG_DIR", qPrintable(directory.path() + "/magpie"), 1);
	// MAGPIE_TEST_CONFIG_HOME names a prepared directory instead, for a run
	// that is to look as the desktop does (its kdeglobals has the colours)
	const char* configHome = std::getenv("MAGPIE_TEST_CONFIG_HOME");
	setenv("XDG_CONFIG_HOME", configHome ? configHome : qPrintable(directory.path() + "/config"), 1);
	setenv("QT_QPA_PLATFORM", "offscreen", 0);

	QQuickWindow::setGraphicsApi(QSGRendererInterface::Software);
	qInstallMessageHandler(MessageHandler);

	int qtArgc = 1;
	QApplication application(qtArgc, argv);
	QApplication::setQuitOnLastWindowClosed(false);
	// The tests compare English texts, whatever the language of the system
	Magpie::Localization::Initialize("en-us");
	return Catch::Session().run(argc, argv);
}
