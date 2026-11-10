# Code that several tools share: option parsing (a header) and a plain
# Wayland window. Client code for the two protocols the window needs is
# generated from the XML files of wayland-protocols.
find_program(WAYLAND_SCANNER wayland-scanner REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(WAYLAND_CLIENT REQUIRED IMPORTED_TARGET wayland-client)
pkg_get_variable(WAYLAND_PROTOCOLS_DIR wayland-protocols pkgdatadir)

set(GENERATED)
foreach(xml IN ITEMS
	${WAYLAND_PROTOCOLS_DIR}/stable/xdg-shell/xdg-shell.xml
	${WAYLAND_PROTOCOLS_DIR}/unstable/xdg-decoration/xdg-decoration-unstable-v1.xml
)
	get_filename_component(name ${xml} NAME_WE)
	set(header ${CMAKE_CURRENT_BINARY_DIR}/${name}-client-protocol.h)
	set(code ${CMAKE_CURRENT_BINARY_DIR}/${name}-protocol.c)
	add_custom_command(OUTPUT ${header}
		COMMAND ${WAYLAND_SCANNER} client-header ${xml} ${header}
		DEPENDS ${xml} VERBATIM)
	add_custom_command(OUTPUT ${code}
		COMMAND ${WAYLAND_SCANNER} private-code ${xml} ${code}
		DEPENDS ${xml} VERBATIM)
	list(APPEND GENERATED ${header} ${code})
endforeach()

add_library(magpie_tools_common STATIC WaylandWindow.cpp ${GENERATED})
target_include_directories(magpie_tools_common
	PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}
	PRIVATE ${CMAKE_CURRENT_BINARY_DIR}
)
target_link_libraries(magpie_tools_common
	PUBLIC magpie_core PkgConfig::WAYLAND_CLIENT
	PRIVATE magpie_warnings
)
# See render/CMakeLists.txt
target_compile_options(magpie_tools_common PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-Wno-missing-field-initializers>)
