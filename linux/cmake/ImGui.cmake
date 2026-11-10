# Provides the target magpie_imgui: Dear ImGui with its Vulkan back end, for
# the toolbar and the profiler that are drawn over the scaled picture. The
# version is the one the Windows build takes from Conan.
#
# Search order:
#   1. IMGUI_ROOT, a directory with imgui.h and backends/imgui_impl_vulkan.cpp
#   2. if MAGPIE_FETCH_IMGUI is ON: the pinned release, downloaded into the
#      build directory and checked against its SHA-256
#
# Dear ImGui has no build files of its own and is meant to be compiled into
# the program that uses it; Fedora does not package it.

option(MAGPIE_FETCH_IMGUI "Download the pinned Dear ImGui release if IMGUI_ROOT is not set" ON)
set(IMGUI_ROOT "" CACHE PATH "Directory of the Dear ImGui sources")

set(MAGPIE_IMGUI_VERSION "1.91.8")
set(MAGPIE_IMGUI_SHA256 "db3a2e02bfd6c269adf0968950573053d002f40bdfb9ef2e4a90bce804b0f286")
set(MAGPIE_IMGUI_URL "https://github.com/ocornut/imgui/archive/refs/tags/v${MAGPIE_IMGUI_VERSION}.tar.gz")

if(IMGUI_ROOT)
	set(imgui_dir "${IMGUI_ROOT}")
else()
	if(NOT MAGPIE_FETCH_IMGUI)
		message(FATAL_ERROR
			"Dear ImGui is needed. Set IMGUI_ROOT to its sources, "
			"or configure with -DMAGPIE_FETCH_IMGUI=ON to download release ${MAGPIE_IMGUI_VERSION}.")
	endif()

	set(imgui_dir "${CMAKE_BINARY_DIR}/_deps/imgui-${MAGPIE_IMGUI_VERSION}")
	set(imgui_archive "${CMAKE_BINARY_DIR}/_deps/imgui-${MAGPIE_IMGUI_VERSION}.tar.gz")
	if(NOT EXISTS "${imgui_dir}/imgui.h")
		message(STATUS "Downloading Dear ImGui ${MAGPIE_IMGUI_VERSION}")
		file(DOWNLOAD "${MAGPIE_IMGUI_URL}" "${imgui_archive}"
			EXPECTED_HASH SHA256=${MAGPIE_IMGUI_SHA256}
			TLS_VERIFY ON
			STATUS imgui_status)
		list(GET imgui_status 0 imgui_code)
		if(NOT imgui_code EQUAL 0)
			list(GET imgui_status 1 imgui_message)
			message(FATAL_ERROR "Downloading ${MAGPIE_IMGUI_URL} failed: ${imgui_message}")
		endif()
		# The archive has one directory, imgui-<version>
		file(ARCHIVE_EXTRACT INPUT "${imgui_archive}" DESTINATION "${CMAKE_BINARY_DIR}/_deps")
	endif()
endif()

if(NOT EXISTS "${imgui_dir}/imgui.h" OR NOT EXISTS "${imgui_dir}/backends/imgui_impl_vulkan.cpp")
	message(FATAL_ERROR "${imgui_dir} does not contain imgui.h and backends/imgui_impl_vulkan.cpp")
endif()
message(STATUS "Dear ImGui: ${imgui_dir}")

find_package(Vulkan REQUIRED)
add_library(magpie_imgui STATIC
	${imgui_dir}/imgui.cpp
	${imgui_dir}/imgui_draw.cpp
	${imgui_dir}/imgui_tables.cpp
	${imgui_dir}/imgui_widgets.cpp
	${imgui_dir}/backends/imgui_impl_vulkan.cpp
)
# As a system directory, so that its headers are exempt from our warnings
target_include_directories(magpie_imgui SYSTEM PUBLIC ${imgui_dir} ${imgui_dir}/backends)
target_link_libraries(magpie_imgui PUBLIC Vulkan::Vulkan)
# The obsolete names stay available: the code ported from the Windows
# version uses some of them
target_compile_definitions(magpie_imgui PUBLIC IMGUI_DISABLE_DEMO_WINDOWS)
set_target_properties(magpie_imgui PROPERTIES POSITION_INDEPENDENT_CODE ON)

# The icons of the toolbar: Microsoft's Fluent UI System Icons (MIT licence),
# the counterpart of the Segoe icon fonts that the Windows version takes from
# the system. The font is loaded at run time; without it the buttons carry
# text.
option(MAGPIE_FETCH_ICON_FONT "Download the pinned icon font for the toolbar" ON)
set(MAGPIE_ICON_FONT "" CACHE FILEPATH "FluentSystemIcons-Resizable.ttf")

set(MAGPIE_ICON_FONT_COMMIT "9eb184e96268d3293462f9d9d19225b4c7b07774")
set(MAGPIE_ICON_FONT_SHA256 "cd88e8ec0fa9848717fd6c5785443f4e4229b3f28fdeecc06c97ac34acbc95db")
set(MAGPIE_ICON_FONT_URL
	"https://raw.githubusercontent.com/microsoft/fluentui-system-icons/${MAGPIE_ICON_FONT_COMMIT}/fonts/FluentSystemIcons-Resizable.ttf")

if(NOT MAGPIE_ICON_FONT AND MAGPIE_FETCH_ICON_FONT)
	set(icon_font "${CMAKE_BINARY_DIR}/_deps/fluent-icons/FluentSystemIcons-Resizable.ttf")
	if(NOT EXISTS "${icon_font}")
		message(STATUS "Downloading the icon font")
		file(DOWNLOAD "${MAGPIE_ICON_FONT_URL}" "${icon_font}"
			EXPECTED_HASH SHA256=${MAGPIE_ICON_FONT_SHA256}
			TLS_VERIFY ON
			STATUS icon_font_status)
		list(GET icon_font_status 0 icon_font_code)
		if(NOT icon_font_code EQUAL 0)
			list(GET icon_font_status 1 icon_font_message)
			file(REMOVE "${icon_font}")
			message(FATAL_ERROR "Downloading ${MAGPIE_ICON_FONT_URL} failed: ${icon_font_message}")
		endif()
	endif()
	set(MAGPIE_ICON_FONT "${icon_font}" CACHE FILEPATH "FluentSystemIcons-Resizable.ttf" FORCE)
endif()
message(STATUS "Icon font: ${MAGPIE_ICON_FONT}")
