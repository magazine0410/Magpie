add_executable(magpie_tests
	TestEffectCacheManager.cpp
	TestEffectCompiler.cpp
	TestLogger.cpp
	TestPaths.cpp
	TestProfileOptions.cpp
	TestProfileRules.cpp
	TestScalingOptions.cpp
	TestScreenshotFilename.cpp
	TestSettingsJson.cpp
	TestSettingsStore.cpp
	TestShaderCompiler.cpp
	TestStrHelper.cpp
	TestVersion.cpp
)

target_link_libraries(magpie_tests PRIVATE magpie_core magpie_fx magpie_warnings Catch2::Catch2WithMain)
target_compile_definitions(magpie_tests PRIVATE
	MAGPIE_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data"
	MAGPIE_EFFECTS_DIR="${MAGPIE_EFFECTS_DIR}"
)

include(Catch)
catch_discover_tests(magpie_tests)

# The renderer tests need a Vulkan device; a software implementation such as
# llvmpipe is enough. MAGPIE_TEST_GPUS limits them to some of the devices.
add_executable(magpie_render_tests
	RenderTestListener.cpp
	TestCursorDrawer.cpp
	TestDmabuf.cpp
	TestDuplicateFrames.cpp
	TestOverlayDrawer.cpp
	TestRenderBasics.cpp
	TestRenderer.cpp
	TestScreenshot.cpp
	TestStepTimer.cpp
)

target_link_libraries(magpie_render_tests PRIVATE magpie_render magpie_warnings Catch2::Catch2WithMain)
target_compile_definitions(magpie_render_tests PRIVATE
	MAGPIE_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data"
	MAGPIE_EFFECTS_DIR="${MAGPIE_EFFECTS_DIR}"
	MAGPIE_ICON_FONT="${MAGPIE_ICON_FONT}"
)
# See render/CMakeLists.txt
target_compile_options(magpie_render_tests PRIVATE -Wno-missing-field-initializers)

catch_discover_tests(magpie_render_tests PROPERTIES LABELS gpu TIMEOUT 600)

# The capture tests feed the frame source from a PipeWire stream of their own.
# They need a Vulkan device and a running PipeWire session with a session
# manager, and skip themselves without the latter.
find_package(PkgConfig REQUIRED)
pkg_check_modules(PIPEWIRE REQUIRED IMPORTED_TARGET libpipewire-0.3)

# The tests of the Sway back end start a Sway of their own that shows nothing,
# and skip themselves if there is no program "sway". Their test window is a
# Wayland client, and their keyboard needs a keymap from libxkbcommon.
pkg_check_modules(WAYLAND_CLIENT REQUIRED IMPORTED_TARGET wayland-client)
pkg_check_modules(XKBCOMMON REQUIRED IMPORTED_TARGET xkbcommon)

add_executable(magpie_engine_tests
	RenderTestListener.cpp
	TestBridge.cpp
	TestFrameSource.cpp
	TestScalingSession.cpp
	TestSway.cpp
)

target_link_libraries(magpie_engine_tests PRIVATE
	magpie_engine magpie_bridge PkgConfig::PIPEWIRE PkgConfig::WAYLAND_CLIENT PkgConfig::XKBCOMMON
	magpie_warnings Catch2::Catch2WithMain)
# The header of xdg-shell that the engine generates for itself
target_include_directories(magpie_engine_tests PRIVATE $<TARGET_PROPERTY:magpie_engine,BINARY_DIR>)

# The window of those tests as a program, for a window of another process
add_executable(magpie_test_window TestWindowMain.cpp)
target_link_libraries(magpie_test_window PRIVATE magpie_engine magpie_bridge PkgConfig::WAYLAND_CLIENT magpie_warnings)
target_include_directories(magpie_test_window PRIVATE $<TARGET_PROPERTY:magpie_engine,BINARY_DIR>)
target_compile_options(magpie_test_window PRIVATE -Wno-missing-field-initializers)
add_dependencies(magpie_engine_tests magpie_test_window)
target_compile_definitions(magpie_engine_tests PRIVATE
	MAGPIE_TEST_WINDOW_PROGRAM="$<TARGET_FILE:magpie_test_window>")
target_compile_definitions(magpie_engine_tests PRIVATE
	MAGPIE_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data"
	MAGPIE_EFFECTS_DIR="${MAGPIE_EFFECTS_DIR}"
)
target_compile_options(magpie_engine_tests PRIVATE -Wno-missing-field-initializers)

catch_discover_tests(magpie_engine_tests PROPERTIES LABELS gpu TIMEOUT 600)

# Every bundled effect in every combination of FP16 mode and inline parameters.
# Takes about a minute on a twelve-thread machine.
add_test(NAME effects-compile-all
	COMMAND magpie-fxc -q --matrix --all "${MAGPIE_EFFECTS_DIR}")
set_tests_properties(effects-compile-all PROPERTIES LABELS slow TIMEOUT 3600)

add_test(NAME upstream-sync
	COMMAND ${CMAKE_COMMAND} "-DREPO_ROOT=${CMAKE_SOURCE_DIR}/.." -P "${CMAKE_CURRENT_SOURCE_DIR}/UpstreamSync.cmake")

# The settings application: its view models and its window, which is loaded
# without being shown
if(MAGPIE_BUILD_APP)
	find_package(Qt6 REQUIRED COMPONENTS Test)
	add_executable(magpie_app_tests
		AppTestMain.cpp
		TestAppModels.cpp
		TestAppWindow.cpp
	)
	target_link_libraries(magpie_app_tests PRIVATE magpie_app Qt6::Test magpie_warnings Catch2::Catch2)
	target_compile_definitions(magpie_app_tests PRIVATE
		MAGPIE_EFFECTS_DIR="${MAGPIE_EFFECTS_DIR}"
		MAGPIE_APP_ID="${MAGPIE_APP_ID}"
	)
	# See render/CMakeLists.txt
	target_compile_options(magpie_app_tests PRIVATE -Wno-missing-field-initializers)
	catch_discover_tests(magpie_app_tests PROPERTIES LABELS app)
endif()
