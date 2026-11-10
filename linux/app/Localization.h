// The texts of the user interface. Linux counterpart of LocalizationService
// (src/Magpie.Core).
//
// The texts come from the .resw files of the Windows user interface and are
// asked for by the names they have there: qsTrId("Home_PageFrame.Title") in
// QML, qtTrId in C++. The texts that only this application has are in
// translations/Linux.language-*.resw.
#pragma once
#include <string>
#include <QString>
#include <QStringList>

namespace Magpie {

struct Localization {
	// Installs the texts of this language (one of SUPPORTED_LANGUAGES), or
	// of the one that fits the system's languages best if it is empty.
	// English texts stand in for those a language lacks. Needs the
	// QCoreApplication.
	static void Initialize(const std::string& language);

	// The tag of the language in use
	static const std::string& Language() noexcept;

	// The language that fits these languages of the user best; they are
	// given in order of preference, as QLocale::uiLanguages has them
	static std::string BestLanguage(const QStringList& uiLanguages);

	// The names of SUPPORTED_LANGUAGES in their own language, in that order
	static QStringList LanguageNames();

	// A text with one "{}", which is replaced
	static QString Format(const char* id, const QString& argument);
};

}
