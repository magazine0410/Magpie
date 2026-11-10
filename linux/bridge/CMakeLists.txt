# The engine's access to the desktop: capability report, window lookup and
# capture sessions, with one back end for KWin and one for the desktop portal.
find_program(WAYLAND_SCANNER wayland-scanner REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(WAYLAND_CLIENT REQUIRED IMPORTED_TARGET wayland-client)
# sd-bus, for the portal
pkg_check_modules(LIBSYSTEMD REQUIRED IMPORTED_TARGET libsystemd)
# The XML descriptions of KWin's protocols; sets PLASMA_WAYLAND_PROTOCOLS_DIR
find_package(PlasmaWaylandProtocols REQUIRED)

set(GENERATED)
foreach(xml IN ITEMS
	${PLASMA_WAYLAND_PROTOCOLS_DIR}/fake-input.xml
	${PLASMA_WAYLAND_PROTOCOLS_DIR}/plasma-window-management.xml
	${PLASMA_WAYLAND_PROTOCOLS_DIR}/zkde-screencast-unstable-v1.xml
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

add_library(magpie_bridge STATIC
	Bridge.cpp
	KWinBridge.cpp
	KWinPointerSpeed.cpp
	PortalCapture.cpp
	PortalRequest.cpp
	PortalShortcuts.cpp
	SessionInhibitor.cpp
	${GENERATED}
)

target_include_directories(magpie_bridge
	PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}
	PRIVATE ${CMAKE_CURRENT_BINARY_DIR}
)
target_link_libraries(magpie_bridge
	PUBLIC magpie_core
	PUBLIC PkgConfig::LIBSYSTEMD
	PRIVATE PkgConfig::WAYLAND_CLIENT magpie_warnings
)
# The listener structures of the Wayland protocols are filled with designated
# initializers; see render/CMakeLists.txt
target_compile_options(magpie_bridge PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-Wno-missing-field-initializers>)
