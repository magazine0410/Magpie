// The parts of the settings application that need no window.
#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QUrl>
#include "AppSettings.h"
#include "AutoStart.h"
#include "EffectsService.h"
#include "Localization.h"
#include "ProfileActions.h"
#include "ScalingModesViewModel.h"
#include "SettingsJson.h"
#include "SettingsStore.h"

using namespace Magpie;

TEST_CASE("The language of the user interface follows the languages of the system", "[app][localization]") {
	CHECK(Localization::BestLanguage({ "de-DE", "en-US" }) == "de");
	CHECK(Localization::BestLanguage({ "de_AT" }) == "de");
	CHECK(Localization::BestLanguage({ "en-GB" }) == "en-us");
	CHECK(Localization::BestLanguage({ "pt-PT" }) == "pt-br");
	CHECK(Localization::BestLanguage({ "pt-BR" }) == "pt-br");
	CHECK(Localization::BestLanguage({ "zh-Hans-CN", "zh-CN", "zh" }) == "zh-hans");
	CHECK(Localization::BestLanguage({ "zh-Hant-TW" }) == "zh-hant");
	CHECK(Localization::BestLanguage({ "zh-TW" }) == "zh-hant");
	CHECK(Localization::BestLanguage({ "zh-HK" }) == "zh-hant");
	CHECK(Localization::BestLanguage({ "zh-CN" }) == "zh-hans");
	CHECK(Localization::BestLanguage({ "zh" }) == "zh-hans");
	// The first language that has a translation wins
	CHECK(Localization::BestLanguage({ "nl-NL", "fr-BE", "de" }) == "fr");
	CHECK(Localization::BestLanguage({ "nl-NL" }) == "en-us");
	CHECK(Localization::BestLanguage({}) == "en-us");

	const QStringList names = Localization::LanguageNames();
	REQUIRE(names.size() == (qsizetype)SUPPORTED_LANGUAGES.size());
	CHECK(names[1] == "Deutsch");
	CHECK(names[2].startsWith("American English") | names[2].startsWith("English"));
	CHECK(names[2].contains("("));
}

TEST_CASE("Every language has its texts, and English stands in for missing ones", "[app][localization]") {
	for (const std::string_view tag : SUPPORTED_LANGUAGES) {
		INFO(tag);
		CHECK(QFileInfo::exists(QString::fromStdString(":/translations/magpie_" + std::string(tag) + ".qm")));
	}

	// The tests run in English or in the language of the system; either way
	// a text of the Windows files and a text of the Linux file are found
	Localization::Initialize("en-us");
	CHECK(qtTrId("Home_PageFrame.Title") == "Home");
	CHECK(qtTrId("Linux_SystemDefault") == "System default");
	CHECK(Localization::Format("Home_Activation_Timer_Description", "3") == "Scale the foreground window after 3s");
}

TEST_CASE("The autostart entry starts the program without its window", "[app][autostart]") {
	// XDG_CONFIG_HOME points into a temporary directory in these tests
	REQUIRE(AutoStart::FilePath().endsWith("/autostart/" MAGPIE_APP_ID ".desktop"));
	REQUIRE_FALSE(AutoStart::IsEnabled());

	REQUIRE(AutoStart::Enable("/opt/My \"Apps\"/magpie"));
	CHECK(AutoStart::IsEnabled());

	QFile file(AutoStart::FilePath());
	REQUIRE(file.open(QIODevice::ReadOnly));
	const QString entry = QString::fromUtf8(file.readAll());
	CHECK(entry.startsWith("[Desktop Entry]\n"));
	CHECK(entry.contains("Type=Application\n"));
	// Quoted as the desktop entry specification asks
	CHECK(entry.contains("Exec=\"/opt/My \\\"Apps\\\"/magpie\" -t\n"));

	REQUIRE(AutoStart::Disable());
	CHECK_FALSE(AutoStart::IsEnabled());
	// Nothing to remove is no failure
	CHECK(AutoStart::Disable());
}

TEST_CASE("The effects are listed with their parameters", "[app][effects]") {
	EffectsService effects;
	effects.Initialize(MAGPIE_EFFECTS_DIR);

	// All effects of the repository parse
	CHECK(effects.Effects().size() == 117);

	const EffectInfo* lanczos = effects.GetEffect("Lanczos");
	REQUIRE(lanczos);
	CHECK(lanczos->canScale);
	CHECK(lanczos->params.size() == 1);

	const EffectInfo* rcas = effects.GetEffect("FSR\\FSR_RCAS");
	REQUIRE(rcas);
	CHECK_FALSE(rcas->canScale);
	REQUIRE(rcas->params.size() == 1);
	CHECK(rcas->params[0].name == "sharpness");

	CHECK(effects.GetEffect("FSR/FSR_RCAS") == nullptr);
	CHECK(effects.GetEffect("NoSuchEffect") == nullptr);
	CHECK(EffectsService::DisplayName("FSR\\FSR_RCAS") == "FSR_RCAS");
	CHECK(EffectsService::DisplayName("Lanczos") == "Lanczos");
}

TEST_CASE("Scaling modes are edited through the view model", "[app][scalingmodes]") {
	std::filesystem::remove(SettingsStore::ConfigPath());
	AppSettings settings;
	REQUIRE(settings.Load() == SettingsStore::LoadResult::Initial);
	EffectsService effects;
	effects.Initialize(MAGPIE_EFFECTS_DIR);
	ScalingModesViewModel vm(settings, effects);
	Settings& data = settings.Data();

	REQUIRE(vm.Count() == 7);
	CHECK(vm.modeName(1) == "FSR");
	CHECK(vm.modeDescription(1) == "FSR_EASU > FSR_RCAS");
	CHECK(vm.effectCanScale(1, 0));
	CHECK_FALSE(vm.effectCanScale(1, 1));
	CHECK(vm.effectHasParameters(1, 1));

	SECTION("moving and removing a scaling mode keeps the profiles on theirs") {
		data.defaultProfile.scalingMode = 1;
		data.profiles.emplace_back().scalingMode = 2;
		data.profiles.emplace_back().scalingMode = 4;
		CHECK(vm.linkedProfiles(1) == QStringList{ "Defaults" });

		vm.moveMode(1, false);
		CHECK(vm.modeName(2) == "FSR");
		CHECK(data.defaultProfile.scalingMode == 2);
		CHECK(data.profiles[0].scalingMode == 1);
		CHECK(data.profiles[1].scalingMode == 4);

		vm.removeMode(2);
		CHECK(vm.Count() == 6);
		CHECK(data.defaultProfile.scalingMode == -1);
		CHECK(data.profiles[0].scalingMode == 1);
		CHECK(data.profiles[1].scalingMode == 3);

		// Out of range: nothing happens
		vm.moveMode(0, true);
		vm.removeMode(99);
		CHECK(vm.Count() == 6);
	}

	SECTION("a new scaling mode is empty or a copy") {
		CHECK_FALSE(vm.canAddMode("   "));
		vm.addMode("  Mine ", 2);
		REQUIRE(vm.Count() == 8);
		CHECK(vm.modeName(7) == "Mine");
		CHECK(vm.modeDescription(7) == "FSR_EASU > FSR_RCAS");
		CHECK(vm.isExpanded(7));

		vm.addMode("Empty", 0);
		CHECK(vm.effectCount(8) == 0);

		CHECK_FALSE(vm.canRenameMode(7, "Mine"));
		CHECK(vm.canRenameMode(7, "Yours"));
		vm.renameMode(7, " Yours ");
		CHECK(vm.modeName(7) == "Yours");
	}

	SECTION("effects are added, moved and removed") {
		vm.addEffect(1, "Lanczos");
		REQUIRE(vm.effectCount(1) == 3);
		// An effect that can scale starts with the size that fits the screen
		CHECK(data.scalingModes[1].effects[2].scalingType == ScalingType::Fit);
		vm.addEffect(1, "NoSuchEffect");
		CHECK(vm.effectCount(1) == 3);

		vm.moveEffect(1, 2, 0);
		CHECK(vm.modeDescription(1) == "Lanczos > FSR_EASU > FSR_RCAS");
		vm.moveEffect(1, 0, 1);
		CHECK(vm.modeDescription(1) == "FSR_EASU > Lanczos > FSR_RCAS");
		vm.removeEffect(1, 1);
		CHECK(vm.modeDescription(1) == "FSR_EASU > FSR_RCAS");
	}

	SECTION("an effect that does not exist is shown as unknown") {
		data.scalingModes[1].effects[0].name = "Gone\\Effect";
		CHECK(vm.hasUnknownEffects(1));
		CHECK(vm.modeDescription(1) == "(Unknown effect) > FSR_RCAS");
		CHECK(vm.effectName(1, 0) == "Unknown effect (Gone\\Effect)");
		CHECK_FALSE(vm.effectCanScale(1, 0));
		CHECK_FALSE(vm.hasUnknownEffects(0));
	}

	SECTION("how an effect scales") {
		// FSR_EASU: fit, with the factors 1
		CHECK(vm.scalingType(1, 0) == (int)ScalingType::Fit);
		vm.setScaleX(1, 0, 1.5);
		vm.setScaleY(1, 0, -2.0);
		CHECK(vm.scaleX(1, 0) == 1.5);
		CHECK(vm.scaleY(1, 0) == 1.0);

		// To a size in pixels: starts with the size of the screen, whole numbers
		vm.setScalingType(1, 0, (int)ScalingType::Absolute);
		CHECK(vm.scaleX(1, 0) >= 300.0);
		vm.setScaleX(1, 0, 1280.4);
		vm.setScaleY(1, 0, 0.2);
		CHECK(vm.scaleX(1, 0) == 1280.0);
		CHECK(vm.scaleY(1, 0) >= 300.0);

		// Back to a factor: 1
		vm.setScalingType(1, 0, (int)ScalingType::Normal);
		CHECK(vm.scaleX(1, 0) == 1.0);
		CHECK(vm.scaleY(1, 0) == 1.0);
	}

	SECTION("the parameters of an effect") {
		const QVariantList parameters = vm.parameters(1, 1);
		REQUIRE(parameters.size() == 1);
		const QVariantMap sharpness = parameters[0].toMap();
		CHECK(sharpness["label"].toString() == "Sharpness");
		CHECK_FALSE(sharpness["isBool"].toBool());
		// From the initial settings, not the default of the effect
		CHECK(sharpness["value"].toDouble() == (double)0.87f);
		CHECK(sharpness["minimum"].toDouble() == 0.0);
		CHECK(sharpness["maximum"].toDouble() == 1.0);

		vm.setParameter(1, 1, 0, 0.5);
		CHECK(data.scalingModes[1].effects[1].parameters["sharpness"] == 0.5f);
		vm.setParameter(1, 1, 5, 0.5);
		CHECK(data.scalingModes[1].effects[1].parameters.size() == 1);
	}

	SECTION("the menu of effects has folders first") {
		const QVariantList menu = vm.effectMenu();
		REQUIRE(menu.size() > 14);
		CHECK(menu[0].toMap()["folder"].toString() == "Anime4K");
		CHECK(menu[0].toMap()["items"].toList().size() > 5);
		const QVariantMap last = menu.back().toMap();
		CHECK_FALSE(last.contains("folder"));
		CHECK(last["name"] == last["fullName"]);
	}

	SECTION("scaling modes are exported and imported") {
		QTemporaryDir directory;
		const QUrl file = QUrl::fromLocalFile(directory.path() + "/ScalingModes.json");
		REQUIRE(vm.exportTo(file));
		REQUIRE(vm.importFrom(file));
		REQUIRE(vm.Count() == 14);
		CHECK(data.scalingModes[8] == data.scalingModes[1]);

		// A file that is not what it should be changes nothing
		QFile broken(directory.path() + "/broken.json");
		REQUIRE(broken.open(QIODevice::WriteOnly));
		broken.write(R"({ "scalingModes": [ { "name": "A" }, { "name": "B", "effects": {} } ] })");
		broken.close();
		CHECK_FALSE(vm.importFrom(QUrl::fromLocalFile(broken.fileName())));
		CHECK_FALSE(vm.importFrom(QUrl::fromLocalFile(directory.path() + "/missing.json")));
		CHECK(vm.Count() == 14);
	}
}

TEST_CASE("Importing scaling modes is strict", "[app][scalingmodes]") {
	std::vector<ScalingMode> modes;

	CHECK(ReadScalingModesJson(R"({ "scalingModes": [
		{ "name": "A", "effects": [
			{ "name": "Lanczos", "scalingType": 1, "scale": { "x": 2, "y": 2 }, "parameters": { "ARStrength": 0.5 } },
		] },
	] })", modes));
	REQUIRE(modes.size() == 1);
	CHECK(modes[0].effects[0].scale == std::pair(2.0f, 2.0f));
	CHECK(modes[0].effects[0].parameters.at("ARStrength") == 0.5f);

	// No scaling modes at all is fine
	CHECK(ReadScalingModesJson("{}", modes));
	CHECK(ReadScalingModesJson(R"({ "scalingModes": [] })", modes));
	CHECK(modes.size() == 1);

	for (const char* json : {
		"",
		"[]",
		R"({ "scalingModes": {} })",
		R"({ "scalingModes": [ 1 ] })",
		R"({ "scalingModes": [ { "name": 1 } ] })",
		R"({ "scalingModes": [ { "name": "A", "effects": {} } ] })",
		R"({ "scalingModes": [ { "name": "A", "effects": [ 1 ] } ] })",
		R"({ "scalingModes": [ { "name": "A", "effects": [ { "name": 1 } ] } ] })",
		R"({ "scalingModes": [ { "name": "A", "effects": [ { "name": "L", "scalingType": "x" } ] } ] })",
		R"({ "scalingModes": [ { "name": "A", "effects": [ { "name": "L", "scale": { "x": 0, "y": 1 } } ] } ] })",
		R"({ "scalingModes": [ { "name": "A", "effects": [ { "name": "L", "parameters": { "p": "x" } } ] } ] })"
	}) {
		INFO(json);
		CHECK_FALSE(ReadScalingModesJson(json, modes));
	}
	CHECK(modes.size() == 1);

	// As on Windows, a scaling mode or an effect without a name is taken,
	// with an empty name (section 11 of LINUX_PORT_PLAN.md, U3)
	CHECK(ReadScalingModesJson(R"({ "scalingModes": [ { "effects": [ {} ] } ] })", modes));
	REQUIRE(modes.size() == 2);
	CHECK(modes[1].name.empty());
	REQUIRE(modes[1].effects.size() == 1);
	CHECK(modes[1].effects[0].name.empty());
}

TEST_CASE("What a profile starts is its launcher or the program of its rule", "[app][profiles]") {
	Profile profile;
	profile.pathRule = "/usr/bin/env";
	CHECK(ProfileActions::ProgramToLaunch(profile) == "/usr/bin/env");
	CHECK(ProfileActions::ProgramLocation(profile) == "/usr/bin");

	// A Windows program under Wine names no file of this system
	profile.pathRule = "C:\\Games\\Game.exe";
	CHECK(ProfileActions::ProgramToLaunch(profile).isEmpty());
	CHECK(ProfileActions::ProgramLocation(profile).isEmpty());
	CHECK_FALSE(ProfileActions::Launch(profile));

	profile.launcherPath = "/usr/bin/true";
	CHECK(ProfileActions::ProgramToLaunch(profile) == "/usr/bin/true");
	profile.launchParameters = "--some \"quoted argument\"";
	CHECK(ProfileActions::Launch(profile));

	// A launcher that is gone
	profile.launcherPath = "/no/such/launcher";
	CHECK(ProfileActions::ProgramToLaunch(profile).isEmpty());

	// An app ID without a .desktop file gets the general icon
	CHECK(ProfileActions::IconName("no.such.application") == "application-x-executable");
	CHECK(ProfileActions::ApplicationName("no.such.application").isEmpty());
}
