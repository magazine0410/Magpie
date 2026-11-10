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

include(CTest)
if(BUILD_TESTING)
	find_package(Catch2 3 REQUIRED)
	add_subdirectory(tests)
endif()
