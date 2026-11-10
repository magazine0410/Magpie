// T8: test source window.
//
// A window that stands in for a game in the other tests:
//   * redraws on every frame callback and prints its frame rate once a second,
//     so a test can see whether the compositor throttles it;
//   * draws the time of each frame as a row of black/white cells, so a
//     capturing program can read how old the frame it received is;
//   * draws a crosshair at the last pointer position and prints every button
//     and key event with coordinates, so input mapping can be checked;
//   * prints screen changes, for the virtual-output test.
//
// Runs as a native Wayland client by default and as an Xwayland client with
// QT_QPA_PLATFORM=xcb, which is how Wine games appear.
//
// Usage: testsource [--size WxH] [--duration seconds] [--title text] [--no-focus] [--frameless]
//                   [--resizable] [--static] [--message text]
#include <QCommandLineParser>
#include <QElapsedTimer>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QRasterWindow>
#include <QScreen>
#include <QTimer>
#include <cstdio>
#include <ctime>

// Layout of the timestamp row, shared with the capture tests.
namespace Stamp {
constexpr int CELL = 8;        // cell size in pixels
constexpr int SYNC_CELLS = 4;  // white, black, white, black
constexpr int BITS = 48;       // CLOCK_MONOTONIC in microseconds, MSB first
}

static qint64 MonotonicMicros() {
	timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return qint64(ts.tv_sec) * 1000000 + ts.tv_nsec / 1000;
}

class SourceWindow : public QRasterWindow {
public:
	// Paint once and then only when the compositor asks (no animation).
	bool staticContent = false;
	QString message;

	SourceWindow() {
		_fpsTimer.start();
		_motionTimer.start();
		connect(this, &QWindow::screenChanged, this, [](QScreen* screen) {
			std::printf("screen-changed name=%s geometry=%dx%d+%d+%d refresh=%.2f\n",
				qPrintable(screen->name()), screen->geometry().width(), screen->geometry().height(),
				screen->geometry().x(), screen->geometry().y(), screen->refreshRate());
			std::fflush(stdout);
		});
	}

protected:
	void paintEvent(QPaintEvent*) override {
		QPainter p(this);
		p.fillRect(QRect(QPoint(0, 0), size()), _clicks % 2 ? QColor(40, 90, 40) : QColor(40, 40, 90));

		// Timestamp row.
		const quint64 stamp = quint64(MonotonicMicros());
		int x = 0;
		for (int i = 0; i < Stamp::SYNC_CELLS; ++i, x += Stamp::CELL) {
			p.fillRect(x, 0, Stamp::CELL, Stamp::CELL, i % 2 ? Qt::black : Qt::white);
		}
		for (int i = Stamp::BITS - 1; i >= 0; --i, x += Stamp::CELL) {
			p.fillRect(x, 0, Stamp::CELL, Stamp::CELL, (stamp >> i) & 1 ? Qt::white : Qt::black);
		}

		// Something that moves every frame, so dropped frames are visible.
		const int barX = int(_frame % quint64(qMax(1, width())));
		p.fillRect(barX, height() - 24, 4, 24, Qt::yellow);

		// One-pixel border, to check cropping at the edges after scaling.
		p.setPen(Qt::red);
		p.drawRect(0, 0, width() - 1, height() - 1);

		// Crosshair at the last pointer position.
		p.setPen(Qt::white);
		p.drawLine(_pos.x() - 12, _pos.y(), _pos.x() + 12, _pos.y());
		p.drawLine(_pos.x(), _pos.y() - 12, _pos.x(), _pos.y() + 12);

		p.drawText(12, 40, QStringLiteral("frame %1   %2 fps   %3x%4")
			.arg(_frame).arg(_fps, 0, 'f', 1).arg(width()).arg(height()));
		p.drawText(12, 60, QStringLiteral("pointer %1,%2   buttons %3   clicks %4")
			.arg(_pos.x()).arg(_pos.y()).arg(int(_buttons)).arg(_clicks));
		p.drawText(12, 80, QStringLiteral("last key: %1   screen: %2")
			.arg(_lastKey, screen() ? screen()->name() : QStringLiteral("?")));

		if (!message.isEmpty()) {
			QFont font = p.font();
			font.setPixelSize(22);
			p.setFont(font);
			p.drawText(QRect(12, 110, width() - 24, height() - 150), Qt::AlignCenter | Qt::TextWordWrap, message);
		}

		++_frame;
		++_framesSinceReport;
		if (_fpsTimer.elapsed() >= 1000) {
			_fps = _framesSinceReport * 1000.0 / _fpsTimer.elapsed();
			std::printf("fps=%.1f mono=%.2f frame=%llu size=%dx%d screen=%s exposed=%d active=%d\n",
				_fps, MonotonicMicros() / 1e6, (unsigned long long)_frame, width(), height(),
				screen() ? qPrintable(screen()->name()) : "?", int(isExposed()), int(isActive()));
			std::fflush(stdout);
			_framesSinceReport = 0;
			_fpsTimer.restart();
		}

		// Ask for the next frame. Qt delivers it on the compositor's frame callback.
		// update() and not requestUpdate(): a QRasterWindow repaints only if it
		// has a dirty region.
		if (!staticContent) {
			update();
		}
	}

	void mouseMoveEvent(QMouseEvent* e) override {
		_pos = e->position().toPoint();
		_buttons = e->buttons();
		++_motionEvents;
		// At most ten lines a second.
		if (_motionTimer.elapsed() >= 100) {
			std::printf("motion x=%d y=%d events=%d\n", _pos.x(), _pos.y(), _motionEvents);
			std::fflush(stdout);
			_motionEvents = 0;
			_motionTimer.restart();
		}
	}

	bool event(QEvent* e) override {
		if (e->type() == QEvent::Enter || e->type() == QEvent::Leave) {
			std::printf("pointer-%s\n", e->type() == QEvent::Enter ? "enter" : "leave");
			std::fflush(stdout);
		} else if (e->type() == QEvent::FocusIn || e->type() == QEvent::FocusOut) {
			std::printf("focus-%s\n", e->type() == QEvent::FocusIn ? "in" : "out");
			std::fflush(stdout);
		}
		return QRasterWindow::event(e);
	}

	void mousePressEvent(QMouseEvent* e) override {
		_pos = e->position().toPoint();
		_buttons = e->buttons();
		++_clicks;
		Report("press", e->button());
	}

	void mouseReleaseEvent(QMouseEvent* e) override {
		_pos = e->position().toPoint();
		_buttons = e->buttons();
		Report("release", e->button());
	}

	void wheelEvent(QWheelEvent* e) override {
		std::printf("wheel x=%d y=%d delta=%d\n",
			int(e->position().x()), int(e->position().y()), e->angleDelta().y());
		std::fflush(stdout);
	}

	void keyPressEvent(QKeyEvent* e) override {
		_lastKey = QKeySequence(e->key()).toString();
		std::printf("key %s\n", qPrintable(_lastKey));
		std::fflush(stdout);
		if (e->key() == Qt::Key_Escape) {
			close();
		}
	}

private:
	void Report(const char* what, Qt::MouseButton button) {
		std::printf("%s button=%d x=%d y=%d\n", what, int(button), _pos.x(), _pos.y());
		std::fflush(stdout);
	}

	QElapsedTimer _fpsTimer;
	QElapsedTimer _motionTimer;
	int _motionEvents = 0;
	QPoint _pos{ -100, -100 };
	Qt::MouseButtons _buttons;
	QString _lastKey = QStringLiteral("-");
	quint64 _frame = 0;
	int _framesSinceReport = 0;
	int _clicks = 0;
	double _fps = 0;
};

int main(int argc, char* argv[]) {
	QGuiApplication app(argc, argv);

	QCommandLineParser parser;
	parser.addHelpOption();
	parser.addOption({ "size", "Client size, default 640x480.", "WxH", "640x480" });
	parser.addOption({ "duration", "Exit after this many seconds (0 = never).", "seconds", "0" });
	parser.addOption({ "title", "Window title.", "text", "Magpie test source" });
	parser.addOption({ "no-focus", "Do not take keyboard focus when shown." });
	parser.addOption({ "frameless", "No title bar or border." });
	parser.addOption({ "resizable", "Let the window be resized (the default is a fixed size)." });
	parser.addOption({ "static", "Do not animate: the window content stays unchanged." });
	parser.addOption({ "message", "Text shown in the window.", "text" });
	parser.process(app);

	const QStringList size = parser.value("size").split('x');
	if (size.size() != 2) {
		std::fprintf(stderr, "bad --size\n");
		return 2;
	}

	SourceWindow window;
	window.setTitle(parser.value("title"));
	window.staticContent = parser.isSet("static");
	window.message = parser.value("message");
	Qt::WindowFlags flags = Qt::Window;
	if (parser.isSet("no-focus")) {
		flags |= Qt::WindowDoesNotAcceptFocus;
	}
	if (parser.isSet("frameless")) {
		flags |= Qt::FramelessWindowHint;
	}
	window.setFlags(flags);
	window.resize(size[0].toInt(), size[1].toInt());
	// A game does not resize itself; fixed size also keeps the scale factor stable.
	if (!parser.isSet("resizable")) {
		window.setMinimumSize(window.size());
		window.setMaximumSize(window.size());
	}
	window.show();

	std::printf("started platform=%s\n", qPrintable(QGuiApplication::platformName()));
	std::fflush(stdout);

	if (const int duration = parser.value("duration").toInt(); duration > 0) {
		QTimer::singleShot(duration * 1000, &app, &QCoreApplication::quit);
	}

	return app.exec();
}
