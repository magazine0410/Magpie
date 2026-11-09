cmake_minimum_required(VERSION 3.25)
project(magpie LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
	set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

option(MAGPIE_WARNINGS_AS_ERRORS "Fail the build on compiler warnings" ON)

# Applied to Magpie's own targets, not to third-party code.
add_library(magpie_warnings INTERFACE)
target_compile_options(magpie_warnings INTERFACE -Wall -Wextra)
if(MAGPIE_WARNINGS_AS_ERRORS)
	target_compile_options(magpie_warnings INTERFACE -Werror)
endif()

find_package(fmt REQUIRED)
find_package(spdlog REQUIRED)
find_package(RapidJSON REQUIRED)

add_subdirectory(core)

include(CTest)
if(BUILD_TESTING)
	find_package(Catch2 3 REQUIRED)
	add_subdirectory(tests)
endif()
