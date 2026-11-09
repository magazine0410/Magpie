cmake_minimum_required(VERSION 3.25)
project(magpie_spikes C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE)
	set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

# Root of the Windows source tree (read only, never modified by the spikes).
set(MAGPIE_SRC "${CMAKE_CURRENT_SOURCE_DIR}/../../src")

add_subdirectory(t1_shaders)
add_subdirectory(t8_testsource)
add_subdirectory(protocols)
add_subdirectory(kwin_windows)
add_subdirectory(kwin_cast)
add_subdirectory(overlay)
add_subdirectory(kwin_pointer)
add_subdirectory(x11_cursor)
