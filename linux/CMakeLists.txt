cmake_minimum_required(VERSION 3.25)
# C for the code that wayland-scanner generates
project(magpie LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
	set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

# Installation ("cmake --install"): the programs go to bin, the effects and
# the icon font to share/magpie, where the installed programs look for them
# (Paths::EffectsDir), and the shader compiler's library to a directory of
# Magpie's own, which the installed programs name as their library path.
include(GNUInstallDirs)
set(MAGPIE_INSTALL_DATADIR "${CMAKE_INSTALL_DATADIR}/magpie")
set(MAGPIE_INSTALL_LIBDIR "${CMAKE_INSTALL_LIBDIR}/magpie")
set(CMAKE_INSTALL_RPATH "${CMAKE_INSTALL_PREFIX}/${MAGPIE_INSTALL_LIBDIR}")

# The effects shared with the Windows build
set(MAGPIE_EFFECTS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../src/Effects")

option(MAGPIE_WARNINGS_AS_ERRORS "Fail the build on compiler warnings" ON)

# Names the program to the desktop: its D-Bus service, its .desktop file, and
# the entry under which the desktop lists its shortcuts
set(MAGPIE_APP_ID "io.github.magazine0410.Magpie" CACHE STRING "Application ID")
string(REPLACE "." "/" MAGPIE_OBJECT_PATH "/${MAGPIE_APP_ID}")

# Applied to Magpie's own targets, not to third-party code.
add_library(magpie_warnings INTERFACE)
target_compile_options(magpie_warnings INTERFACE -Wall -Wextra)
if(MAGPIE_WARNINGS_AS_ERRORS)
	target_compile_options(magpie_warnings INTERFACE -Werror)
endif()

find_package(fmt REQUIRED)
find_package(spdlog REQUIRED)
find_package(RapidJSON REQUIRED)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(Dxc)
include(ImGui)

add_subdirectory(core)
add_subdirectory(fx)
add_subdirectory(render)
add_subdirectory(bridge)
add_subdirectory(engine)
add_subdirectory(tools/common)
add_subdirectory(tools/magpie-fxc)
add_subdirectory(tools/magpie-scale-image)
add_subdirectory(tools/magpie-present-test)
add_subdirectory(tools/magpie-viewer)
add_subdirectory(tools/magpie-scale)
add_subdirectory(tools/magpie-engine)
add_subdirectory(tools/magpie-ctl)

# The settings application needs Qt 6; the engine and the tools do not
option(MAGPIE_BUILD_APP "Build the settings application" ON)
if(MAGPIE_BUILD_APP)
	add_subdirectory(app)
endif()

install(DIRECTORY "${MAGPIE_EFFECTS_DIR}/"
	DESTINATION "${MAGPIE_INSTALL_DATADIR}/effects"
	FILES_MATCHING PATTERN "*.hlsl" PATTERN "*.hlsli" PATTERN "*.dds"
)
if(MAGPIE_ICON_FONT)
	install(FILES "${MAGPIE_ICON_FONT}" DESTINATION "${MAGPIE_INSTALL_DATADIR}/fonts")
endif()
# No distribution packages the DirectX Shader Compiler, so its library is
# installed with Magpie unless it comes from the system's own directories
option(MAGPIE_INSTALL_DXC "Install the library of the DirectX Shader Compiler with Magpie" ON)
if(MAGPIE_INSTALL_DXC)
	install(FILES "${DXC_LIBRARY}" DESTINATION "${MAGPIE_INSTALL_LIBDIR}")
endif()

include(CTest)
if(BUILD_TESTING)
	find_package(Catch2 3 REQUIRED)
	add_subdirectory(tests)
endif()
