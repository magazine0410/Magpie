#include "Localization.h"
#include <algorithm>
#include <vector>
#include <QCoreApplication>
#include <QGuiApplication>
#include <QLocale>
#include <QTranslator>
#include "Settings.h"

namespace Magpie {

static std::string currentLanguage;

static bool IsSupported(const std::string& tag) noexcept {
	return std::ranges::find(SUPPORTED_LANGUAGES, std::string_view(tag)) != SUPPORTED_LANGUAGES.end();
}

std::string Localization::BestLanguage(const QStringList& uiLanguages) {
	for (const QString& uiLanguage : uiLanguages) {
		// "zh-Hans-CN" fits "zh-hans", "de-AT" fits "de"
		std::string tag = uiLanguage.toLower().replace('_', '-').toStdString();
		while (true) {
			if (IsSupported(tag)) {
				return tag;
			}
			const size_t pos = tag.rfind('-');
			if (pos == std::string::npos) {
				break;
			}
			tag.resize(pos);
		}

		// Only the language is left. Chinese without a script is told
		// apart by the region; any other language takes the variant
		// there is ("en" and "en-GB" take "en-us", "pt" takes "pt-br").
		if (tag == "zh") {
			const QString lower = uiLanguage.toLower();
			const bool traditional = lower.contains("tw") || lower.contains("hk") || lower.contains("mo");
			return traditional ? "zh-hant" : "zh-hans";
		}
		for (const std::string_view supported : SUPPORTED_LANGUAGES) {
			if (supported.starts_with(tag + "-")) {
				return std::string(supported);
			}
		}
	}
	return "en-us";
}

static std::vector<QTranslator*> translators;

static void Install(const std::string& tag) {
	QTranslator* translator = new QTranslator(QCoreApplication::instance());
	if (translator->load(QString::fromStdString(":/translations/magpie_" + tag + ".qm"))) {
		QCoreApplication::installTranslator(translator);
		translators.push_back(translator);
	} else {
		delete translator;
	}
}

void Localization::Initialize(const std::string& language) {
	// A second call replaces the texts of the first
	for (QTranslator* translator : translators) {
		QCoreApplication::removeTranslator(translator);
		delete translator;
	}
	translators.clear();

	currentLanguage = IsSupported(language) ? language : BestLanguage(QLocale::system().uiLanguages());

	// The translator that was installed last is asked first
	Install("en-us");
	if (currentLanguage != "en-us") {
		Install(currentLanguage);
	}

	// Arabic is written from right to left, and the window is laid out so
	if (qobject_cast<QGuiApplication*>(QCoreApplication::instance())) {
		QGuiApplication::setLayoutDirection(QLocale(QString::fromStdString(currentLanguage)).textDirection());
	}
}

const std::string& Localization::Language() noexcept {
	return currentLanguage;
}

QStringList Localization::LanguageNames() {
	QStringList names;
	for (const std::string_view tag : SUPPORTED_LANGUAGES) {
		const QLocale locale(QString::fromUtf8(tag.data(), (qsizetype)tag.size()));
		QString name = locale.nativeLanguageName();
		// Tags with a region, as "en-us" and "pt-br"; "zh-hans" names a
		// script, which the name of the language includes
		if (tag.size() == 5 && tag[2] == '-') {
			name += " (" + locale.nativeTerritoryName() + ")";
		}
		names.push_back(name);
	}
	return names;
}

QString Localization::Format(const char* id, const QString& argument) {
	QString text = qtTrId(id);
	const qsizetype pos = text.indexOf("{}");
	return pos < 0 ? text : text.replace(pos, 2, argument);
}

}
