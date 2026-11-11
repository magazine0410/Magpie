# What turns a window into a scaled picture: the frame sources (a screen cast
# from PipeWire, an output copied over Wayland), the surface that shows the
# picture, and the scaling session that ties them to the desktop.
find_program(WAYLAND_SCANNER wayland-scanner REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(PIPEWIRE REQUIRED IMPORTED_TARGET libpipewire-0.3)
pkg_check_modules(WAYLAND_CLIENT REQUIRED IMPORTED_TARGET wayland-client)
# The arrow of the desktop's cursor theme, for the pointer on the toolbar
pkg_check_modules(WAYLAND_CURSOR REQUIRED IMPORTED_TARGET wayland-cursor)
pkg_get_variable(WAYLAND_PROTOCOLS_DIR wayland-protocols pkgdatadir)
# The layer shell, for a surface above all windows that takes no focus
pkg_get_variable(WLR_PROTOCOLS_DIR wlr-protocols pkgdatadir)

set(GENERATED)
foreach(xml IN ITEMS
	${WLR_PROTOCOLS_DIR}/unstable/wlr-layer-shell-unstable-v1.xml
	# The layer shell refers to its popups; the scaled window of windowed mode
	${WAYLAND_PROTOCOLS_DIR}/stable/xdg-shell/xdg-shell.xml
	# Title bar and borders from the desktop
	${WAYLAND_PROTOCOLS_DIR}/unstable/xdg-decoration/xdg-decoration-unstable-v1.xml
	# Copying an output's picture and cursor, for Sway and the nested session;
	# the second names the windows the first could capture as well
	${WAYLAND_PROTOCOLS_DIR}/staging/ext-image-copy-capture/ext-image-copy-capture-v1.xml
	${WAYLAND_PROTOCOLS_DIR}/staging/ext-image-capture-source/ext-image-capture-source-v1.xml
	${WAYLAND_PROTOCOLS_DIR}/staging/ext-foreign-toplevel-list/ext-foreign-toplevel-list-v1.xml
	# The nested session: the pointer held in place, and how far it would
	# have moved
	${WAYLAND_PROTOCOLS_DIR}/unstable/pointer-constraints/pointer-constraints-unstable-v1.xml
	${WAYLAND_PROTOCOLS_DIR}/unstable/relative-pointer/relative-pointer-unstable-v1.xml
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

add_library(magpie_engine STATIC
	FrameSource.cpp
	ImageCopyFrameSource.cpp
	PipeWireFrameSource.cpp
	ScalingService.cpp
	ScalingSession.cpp
	ScalingSurface.cpp
	${GENERATED}
)

target_include_directories(magpie_engine
	PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}
	PRIVATE ${CMAKE_CURRENT_BINARY_DIR}
)
target_link_libraries(magpie_engine
	PUBLIC magpie_render magpie_bridge
	PRIVATE PkgConfig::PIPEWIRE PkgConfig::WAYLAND_CLIENT PkgConfig::WAYLAND_CURSOR magpie_warnings
)
target_compile_definitions(magpie_engine PRIVATE VK_USE_PLATFORM_WAYLAND_KHR)
# See render/CMakeLists.txt
target_compile_options(magpie_engine PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-Wno-missing-field-initializers>)
