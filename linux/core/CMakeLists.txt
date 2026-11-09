# Platform-neutral code shared by the engine, the tools and the application.
add_library(magpie_core STATIC
	Logger.cpp
	Paths.cpp
	ScalingOptions.cpp
	Settings.cpp
	SettingsJson.cpp
	SettingsStore.cpp
	SmallVector.cpp
	Version.cpp
)

target_include_directories(magpie_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(magpie_core
	PUBLIC fmt::fmt spdlog::spdlog
	PRIVATE magpie_warnings
)
