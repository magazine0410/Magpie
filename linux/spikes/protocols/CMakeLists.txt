# Client-side code for the Wayland protocols the spikes use, generated with
# wayland-scanner from the XML files installed by the distribution.
find_program(WAYLAND_SCANNER wayland-scanner REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(WAYLAND_CLIENT REQUIRED IMPORTED_TARGET wayland-client)

set(WP_DIR /usr/share/wayland-protocols)
set(PLASMA_DIR /usr/share/plasma-wayland-protocols)
set(WLR_DIR /usr/share/wlr-protocols)

set(PROTOCOLS
	${WP_DIR}/stable/xdg-shell/xdg-shell.xml
	${WP_DIR}/stable/viewporter/viewporter.xml
	${WP_DIR}/stable/presentation-time/presentation-time.xml
	${WP_DIR}/unstable/pointer-constraints/pointer-constraints-unstable-v1.xml
	${WP_DIR}/unstable/relative-pointer/relative-pointer-unstable-v1.xml
	${WP_DIR}/staging/pointer-warp/pointer-warp-v1.xml
	${WLR_DIR}/unstable/wlr-layer-shell-unstable-v1.xml
	${PLASMA_DIR}/zkde-screencast-unstable-v1.xml
	${PLASMA_DIR}/fake-input.xml
	${PLASMA_DIR}/plasma-window-management.xml
)

set(GENERATED)
foreach(xml IN LISTS PROTOCOLS)
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

add_library(spike_protocols STATIC ${GENERATED})
target_include_directories(spike_protocols PUBLIC ${CMAKE_CURRENT_BINARY_DIR})
target_link_libraries(spike_protocols PUBLIC PkgConfig::WAYLAND_CLIENT)
