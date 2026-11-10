# The settings application: a Qt Quick window, the tray icon, and the scaling
# service of linux/engine on a thread of its own. Linux counterpart of
# src/Magpie.
find_package(Qt6 6.5 REQUIRED COMPONENTS Core Gui Widgets Qml Quick QuickControls2 LinguistTools)
# For the script that converts the texts of the Windows user interface
find_package(Python3 REQUIRED COMPONENTS Interpreter)

# What the About page shows. A build from a git checkout names its commit.
set(MAGPIE_VERSION_STRING "dev" CACHE STRING "Version shown to the user")
set(MAGPIE_REPOSITORY_URL "https://github.com/magazine0410/Magpie" CACHE STRING
	"Where bug reports and requests go")
set(MAGPIE_COMMIT_ID "")
find_package(Git QUIET)
if(Git_FOUND)
	execute_process(
		COMMAND ${GIT_EXECUTABLE} rev-parse --short HEAD
		WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
		OUTPUT_VARIABLE MAGPIE_COMMIT_ID
		OUTPUT_STRIP_TRAILING_WHITESPACE
		ERROR_QUIET
	)
endif()

# The texts: one .qm file per language, made from the .resw files of the
# Windows user interface and from the texts that only this application has.
get_filename_component(WINDOWS_UI_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../src/Magpie" ABSOLUTE)
set(TRANSLATIONS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/translations")
set(ENGLISH_SOURCES
	"${WINDOWS_UI_DIR}/Resources.language-en-US.resw"
	"${TRANSLATIONS_DIR}/Linux.language-en-US.resw"
)
# The languages the Windows version offers (LocalizationService.cpp)
set(LANGUAGES ar de en-US es fi fr id it ja ko pl pt-BR ru ta tr uk vi zh-Hans zh-Hant)
set(QM_FILES)
foreach(language IN LISTS LANGUAGES)
	string(TOLOWER ${language} tag)
	set(inputs "${WINDOWS_UI_DIR}/Resources.language-${language}.resw")
	if(EXISTS "${TRANSLATIONS_DIR}/Linux.language-${language}.resw")
		list(APPEND inputs "${TRANSLATIONS_DIR}/Linux.language-${language}.resw")
	endif()
	set(ts "${CMAKE_CURRENT_BINARY_DIR}/translations/magpie_${tag}.ts")
	set(qm "${CMAKE_CURRENT_BINARY_DIR}/translations/magpie_${tag}.qm")
	add_custom_command(
		OUTPUT ${qm}
		COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/translations"
		COMMAND Python3::Interpreter "${TRANSLATIONS_DIR}/resw2ts.py"
			--language ${language}
			--source "${WINDOWS_UI_DIR}/Resources.language-en-US.resw"
			--source "${TRANSLATIONS_DIR}/Linux.language-en-US.resw"
			--output ${ts} ${inputs}
		# The messages are found by their ids. Newer versions of lrelease see
		# that themselves and say that the option is no longer needed.
		COMMAND Qt6::lrelease -silent -idbased ${ts} -qm ${qm}
		DEPENDS "${TRANSLATIONS_DIR}/resw2ts.py" ${ENGLISH_SOURCES} ${inputs}
		VERBATIM
	)
	list(APPEND QM_FILES ${qm})
endforeach()

add_library(magpie_app STATIC
	AboutViewModel.cpp
	AppSettings.cpp
	Application.cpp
	AutoStart.cpp
	EffectsService.cpp
	HomeViewModel.cpp
	Localization.cpp
	MainWindow.cpp
	ProfileActions.cpp
	ProfileViewModel.cpp
	RootViewModel.cpp
	ScalingController.cpp
	ScalingModesViewModel.cpp
	SettingsViewModel.cpp
)
set_target_properties(magpie_app PROPERTIES AUTOMOC ON)
target_include_directories(magpie_app PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(magpie_app
	PUBLIC magpie_core magpie_fx magpie_engine Qt6::Widgets Qt6::Quick Qt6::QuickControls2
	PRIVATE magpie_warnings
)
target_compile_definitions(magpie_app
	# Qt's "signals", "slots" and "emit" are ordinary words in other headers
	PUBLIC QT_NO_KEYWORDS
	PRIVATE
		MAGPIE_DEFAULT_EFFECTS_DIR="${MAGPIE_EFFECTS_DIR}"
		MAGPIE_DEFAULT_ICON_FONT="${MAGPIE_ICON_FONT}"
		MAGPIE_APP_ID="${MAGPIE_APP_ID}"
		MAGPIE_OBJECT_PATH="${MAGPIE_OBJECT_PATH}"
		MAGPIE_VERSION_STRING="${MAGPIE_VERSION_STRING}"
		MAGPIE_COMMIT_ID="${MAGPIE_COMMIT_ID}"
		MAGPIE_REPOSITORY_URL="${MAGPIE_REPOSITORY_URL}"
)
# See render/CMakeLists.txt
target_compile_options(magpie_app PRIVATE -Wno-missing-field-initializers)

set(QML_FILES
	qml/Main.qml
	qml/NewProfilePopup.qml
	qml/controls/ExpanderItem.qml
	qml/controls/InfoBar.qml
	qml/controls/NumberField.qml
	qml/controls/PageFrame.qml
	qml/controls/SettingsCard.qml
	qml/controls/SettingsExpander.qml
	qml/controls/SettingsGroup.qml
	qml/controls/ShortcutView.qml
	qml/controls/qmldir
	qml/pages/AboutPage.qml
	qml/pages/HomePage.qml
	qml/pages/ProfilePage.qml
	qml/pages/ScalingModesPage.qml
	qml/pages/SettingsPage.qml
)
qt_add_resources(magpie_app "magpie_qml" PREFIX "/" FILES ${QML_FILES})

set(ICON_FILE "${WINDOWS_UI_DIR}/Icons/SVG/Magpie Icon Full Enabled.svg")
set_source_files_properties(${ICON_FILE} PROPERTIES QT_RESOURCE_ALIAS "magpie.svg")
qt_add_resources(magpie_app "magpie_icons" PREFIX "/icons" FILES ${ICON_FILE})

foreach(qm IN LISTS QM_FILES)
	get_filename_component(name ${qm} NAME)
	set_source_files_properties(${qm} PROPERTIES QT_RESOURCE_ALIAS ${name})
endforeach()
qt_add_resources(magpie_app "magpie_translations" PREFIX "/translations" FILES ${QM_FILES})

add_executable(magpie main.cpp)
target_link_libraries(magpie PRIVATE magpie_app magpie_bridge magpie_warnings)
target_compile_definitions(magpie PRIVATE
	MAGPIE_APP_ID="${MAGPIE_APP_ID}"
	MAGPIE_OBJECT_PATH="${MAGPIE_OBJECT_PATH}"
)

# In a build tree the icon is the file of the source tree
set_target_properties(magpie PROPERTIES MAGPIE_ICON_FILE "${ICON_FILE}")

# The .desktop file that makes the program known to the desktop: the global
# shortcuts need it, and the icon of the windows comes from it. In a build
# tree it points to the executable there; copy it to
# ~/.local/share/applications to use that build.
file(GENERATE
	OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/${MAGPIE_APP_ID}.desktop"
	INPUT "${CMAKE_CURRENT_SOURCE_DIR}/../data/magpie.desktop.in"
)
