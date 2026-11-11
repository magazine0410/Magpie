// The main window as a whole: its pages are loaded without being shown on a
// screen, and worked with clicks as a user would.
#include <catch2/catch_test_macros.hpp>
#include <cstdlib>
#include <QCoreApplication>
#include <QDir>
#include <QImage>
#include <QQuickItem>
#include <QQuickWindow>
#include <QTest>
#include "AppTestHelpers.h"
#include "Application.h"
#include "Localization.h"
#include "MainWindow.h"
#include "RootViewModel.h"
#include "SettingsStore.h"

using namespace Magpie;

namespace {

// With MAGPIE_APP_SHOTS_DIR set, pictures of the window are saved there
void SavePicture(QQuickWindow* window, const QString& name) {
	const char* directory = std::getenv("MAGPIE_APP_SHOTS_DIR");
	if (!directory) {
		return;
	}
	QDir().mkpath(directory);
	const QImage image = window->grabWindow();
	REQUIRE(!image.isNull());
	REQUIRE(image.save(QString(directory) + "/" + name + ".png"));
}

struct TestWindow {
	explicit TestWindow(const char* language = "en-us") {
		TakeQtWarnings();
		// Every test starts with the initial settings, and with a window
		// high enough for a page with an opened scaling mode or profile, so
		// that what the tests click on is in the window
		Settings initial;
		initial.SetInitialSettings();
		initial.language = language;
		initial.mainWindowWidth = 1000;
		initial.mainWindowHeight = 1200;
		REQUIRE(SettingsStore::Save(initial));
		REQUIRE(application.Initialize({ .startEngine = false }));
		REQUIRE(application.GetMainWindow());
		window = application.GetMainWindow()->Window();
		REQUIRE(window);
		REQUIRE(QTest::qWaitForWindowExposed(window));
	}

	void ShowPage(const char* page) {
		window->setProperty("currentPage", page);
		QTest::qWait(100);
	}

	// Among the items as they are shown: what a Repeater made is no child
	// of the window as QObject::findChild understands it
	static QQuickItem* FindIn(QQuickItem* parent, const QString& objectName) {
		for (QQuickItem* child : parent->childItems()) {
			if (child->objectName() == objectName) {
				return child;
			}
			if (QQuickItem* found = FindIn(child, objectName)) {
				return found;
			}
		}
		return nullptr;
	}

	QQuickItem* Find(const QString& objectName) {
		QQuickItem* item = FindIn(window->contentItem(), objectName);
		INFO("Item " << objectName.toStdString());
		REQUIRE(item);
		return item;
	}

	// The middle of an item in the window
	QPoint Center(QQuickItem* item) {
		return item->mapToScene(QPointF(item->width() / 2, item->height() / 2)).toPoint();
	}

	// Scrolls the page so that the item is in the window
	void ScrollIntoView(QQuickItem* item) {
		for (QQuickItem* parent = item ? item->parentItem() : nullptr; parent; parent = parent->parentItem()) {
			if (!parent->inherits("QQuickFlickable")) {
				continue;
			}
			QQuickItem* content = parent->property("contentItem").value<QQuickItem*>();
			if (!content) {
				return;
			}
			const qreal top = item->mapToItem(content, QPointF(0, 0)).y();
			const qreal shown = parent->property("contentY").toReal();
			if (top < shown || top + item->height() > shown + parent->height()) {
				const qreal last = std::max(parent->property("contentHeight").toReal() - parent->height(), 0.0);
				parent->setProperty("contentY", std::clamp(top - parent->height() / 2, 0.0, last));
				QTest::qWait(50);
			}
			return;
		}
	}

	void Click(const QString& objectName) {
		QQuickItem* item = Find(objectName);
		ScrollIntoView(item);
		QTest::mouseClick(window, Qt::LeftButton, {}, Center(item));
		QTest::qWait(150);
	}

	void PressKey(Qt::Key key, Qt::KeyboardModifiers modifiers = {}) {
		QTest::keyClick(window, key, modifiers);
		QTest::qWait(150);
	}

	void Type(const char* text) {
		for (; *text; ++text) {
			QTest::keyClick(window, *text);
		}
		QTest::qWait(100);
	}

	RootViewModel& Root() {
		return *qobject_cast<RootViewModel*>(application.GetMainWindow()->Root());
	}

	QString CurrentPage() {
		return window->property("currentPage").toString();
	}

	Settings& Data() {
		return application.Settings().Data();
	}

	~TestWindow() {
		// The tests that follow compare English texts
		Localization::Initialize("en-us");
	}

	Application application;
	QQuickWindow* window = nullptr;
};

void CheckNoWarnings() {
	const QStringList warnings = TakeQtWarnings();
	INFO(warnings.join("\n").toStdString());
	CHECK(warnings.isEmpty());
}

}

TEST_CASE("Every page of the main window loads without a warning", "[app]") {
	TestWindow test;

	for (const char* page : { "home", "scalingModes", "profile:-1", "settings", "about" }) {
		test.ShowPage(page);
		SavePicture(test.window, page);
	}
	CheckNoWarnings();
}

TEST_CASE("The scaling modes page changes scaling modes and their effects", "[app]") {
	TestWindow test;
	test.ShowPage("scalingModes");

	// The second scaling mode is FSR with the effects FSR_EASU and FSR_RCAS
	REQUIRE(test.Data().scalingModes.size() > 1);
	std::vector<EffectItem>& effects = test.Data().scalingModes[1].effects;
	REQUIRE(effects.size() == 2);
	REQUIRE(effects[0].name == "FSR\\FSR_EASU");

	test.Click("scalingMode1Toggle");
	SavePicture(test.window, "scalingModes-expanded");

	SECTION("the parameters of an effect") {
		test.Click("parameters1_1");
		SavePicture(test.window, "scalingModes-parameters");
		test.PressKey(Qt::Key_Escape);
	}

	SECTION("how an effect scales") {
		test.Click("scale1_0");
		SavePicture(test.window, "scalingModes-scale");
		test.PressKey(Qt::Key_Escape);
	}

	SECTION("an effect dragged below the next one changes places with it") {
		QQuickItem* first = test.Find("effect1_0");
		// Next to the name, away from the buttons on the right
		const QPoint start = first->mapToScene(QPointF(100, first->height() / 2)).toPoint();
		const int distance = (int)first->height();

		QTest::mousePress(test.window, Qt::LeftButton, {}, start);
		for (int y = 4; y <= distance; y += 4) {
			QTest::mouseMove(test.window, start + QPoint(0, y));
			QTest::qWait(10);
		}
		SavePicture(test.window, "scalingModes-dragging");
		QTest::mouseRelease(test.window, Qt::LeftButton, {}, start + QPoint(0, distance));
		QTest::qWait(150);

		CHECK(effects[0].name == "FSR\\FSR_RCAS");
		CHECK(effects[1].name == "FSR\\FSR_EASU");
	}

	SECTION("an effect is removed with its button") {
		test.Click("remove1_0");
		REQUIRE(effects.size() == 1);
		CHECK(effects[0].name == "FSR\\FSR_RCAS");
	}

	SECTION("the menu of effects") {
		test.Click("addEffect1");
		SavePicture(test.window, "scalingModes-addEffect");
		test.PressKey(Qt::Key_Escape);
	}

	SECTION("a new scaling mode") {
		const size_t count = test.Data().scalingModes.size();
		test.Click("newScalingMode");
		test.Type("My mode");
		SavePicture(test.window, "scalingModes-new");
		test.PressKey(Qt::Key_Return);

		REQUIRE(test.Data().scalingModes.size() == count + 1);
		CHECK(test.Data().scalingModes.back().name == "My mode");
		CHECK(test.Data().scalingModes.back().effects.empty());
	}

	CheckNoWarnings();
}

TEST_CASE("Profiles are made, shown, changed, moved and deleted", "[app]") {
	TestWindow test;

	const ScalingService::Window kate{
		.info = { .title = "Untitled - Kate" },
		.className = "org.kde.kate",
		.path = "/usr/bin/kate"
	};
	const ScalingService::Window game{
		.info = { .title = "Game" },
		.className = "game.exe",
		.path = "C:\\Games\\Game.exe"
	};

	test.Data().defaultProfile.maxFrameRate = 30.0f;
	REQUIRE(test.Root().AddProfile(kate, " Kate ", 0));
	QTest::qWait(150);

	// The new profile has the settings of the defaults and is shown at once
	REQUIRE(test.Data().profiles.size() == 1);
	CHECK(test.Data().profiles[0].name == "Kate");
	CHECK(test.Data().profiles[0].classNameRule == "org.kde.kate");
	CHECK(test.Data().profiles[0].pathRule == "/usr/bin/kate");
	CHECK(test.Data().profiles[0].maxFrameRate == 30.0f);
	CHECK(test.CurrentPage() == "profile:0");
	CHECK(test.Find("navProfile0")->property("text").toString() == "Kate");

	// No second profile for the same program
	CHECK_FALSE(test.Root().AddProfile(kate, "Kate again", 0));

	SECTION("the page changes the profile it shows") {
		test.Click("profile3DGameMode");
		CHECK(test.Data().profiles[0].Is3DGameMode());
		CHECK_FALSE(test.Data().defaultProfile.Is3DGameMode());

		test.Click("profileFrameRateLimiter");
		CHECK(test.Data().profiles[0].isFrameRateLimiterEnabled);
		SavePicture(test.window, "profile");

		// The frame rate has limits; text that is no number changes nothing
		test.Click("profileMaxFrameRate");
		test.PressKey(Qt::Key_A, Qt::ControlModifier);
		test.Type("5000");
		test.PressKey(Qt::Key_Return);
		CHECK(test.Data().profiles[0].maxFrameRate == 1000.0f);

		test.PressKey(Qt::Key_A, Qt::ControlModifier);
		test.Type("abc");
		test.PressKey(Qt::Key_Return);
		CHECK(test.Data().profiles[0].maxFrameRate == 1000.0f);
		CHECK(test.Find("profileMaxFrameRate")->property("text").toString() == "1000");
	}

	SECTION("the size of a nested session has limits") {
		// Without a scaling service there is no back end for the desktop,
		// so the page says that the program is started in a nested session
		// in any case, and shows the size
		QQuickItem* nestedSwitch = test.Find("profileLaunchNested");
		REQUIRE(nestedSwitch);
		CHECK(nestedSwitch->property("checked").toBool());
		CHECK_FALSE(nestedSwitch->property("enabled").toBool());
		CHECK_FALSE(test.Data().profiles[0].isLaunchedNested);

		CHECK(test.Find("profileNestedWidth")->property("text").toString() == "1280");
		test.Click("profileNestedWidth");
		test.PressKey(Qt::Key_A, Qt::ControlModifier);
		test.Type("800");
		test.PressKey(Qt::Key_Return);
		CHECK(test.Data().profiles[0].nestedWidth == 800);

		test.Click("profileNestedHeight");
		test.PressKey(Qt::Key_A, Qt::ControlModifier);
		test.Type("99999");
		test.PressKey(Qt::Key_Return);
		CHECK(test.Data().profiles[0].nestedHeight == 16384);
		CHECK(test.Find("profileNestedHeight")->property("text").toString() == "16384");

		// Off unless the user turns it on
		QQuickItem* tearingSwitch = test.Find("profileAllowTearing");
		REQUIRE(tearingSwitch);
		CHECK_FALSE(tearingSwitch->property("checked").toBool());
		test.Click("profileAllowTearing");
		CHECK(test.Data().profiles[0].isTearingAllowed);
		SavePicture(test.window, "profile-nested");
	}

	SECTION("a profile is copied from another one, renamed, moved and deleted") {
		test.Data().profiles[0].autoScale = AutoScale::Windowed;
		REQUIRE(test.Root().AddProfile(game, "Game", 1));
		QTest::qWait(150);
		REQUIRE(test.Data().profiles.size() == 2);
		CHECK(test.Data().profiles[1].autoScale == AutoScale::Windowed);
		CHECK(test.CurrentPage() == "profile:1");
		SavePicture(test.window, "profile-wine");

		test.Root().renameProfile(1, "My game");
		QTest::qWait(150);
		CHECK(test.Data().profiles[1].name == "My game");
		CHECK(test.Find("navProfile1")->property("text").toString() == "My game");

		// The page follows its profile
		test.Root().moveProfile(1, true);
		QTest::qWait(150);
		CHECK(test.Data().profiles[0].name == "My game");
		CHECK(test.Data().profiles[1].name == "Kate");
		CHECK(test.CurrentPage() == "profile:0");

		// The delete button of the page, after its question
		test.Click("profileMore");
		SavePicture(test.window, "profile-menu");
		test.PressKey(Qt::Key_Escape);
		test.Root().removeProfile(0);
		QTest::qWait(150);
		REQUIRE(test.Data().profiles.size() == 1);
		CHECK(test.Data().profiles[0].name == "Kate");
		CHECK(test.CurrentPage() == "profile:-1");
	}

	CheckNoWarnings();
}

TEST_CASE("The dialog for a new profile says so when there is no window", "[app]") {
	TestWindow test;

	// Without the scaling service there are no windows to choose from
	test.Click("navNewProfile");
	SavePicture(test.window, "newProfile-empty");
	test.PressKey(Qt::Key_Escape);
	CHECK(test.Data().profiles.empty());
	CheckNoWarnings();
}

TEST_CASE("The window speaks the language of the settings", "[app]") {
	const QString english = qtTrId("Root_ScalingModes.Content");
	REQUIRE(english == "Scaling modes");

	for (const char* language : { "de", "ja", "ar" }) {
		INFO(language);
		TestWindow test(language);
		const QString text = test.Find("navScalingModes")->property("text").toString();
		CHECK(text == qtTrId("Root_ScalingModes.Content"));
		CHECK(text != english);
		// A text that only this application has is English in every language
		CHECK(test.Find("navSettings")->property("text").toString() == "Settings");

		test.ShowPage("profile:-1");
		SavePicture(test.window, QString("language-") + language);
		CheckNoWarnings();
	}
}

