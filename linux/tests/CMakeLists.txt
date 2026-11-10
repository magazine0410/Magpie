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

add_executable(magpie_engine_tests
	RenderTestListener.cpp
	TestBridge.cpp
	TestFrameSource.cpp
	TestScalingSession.cpp
)

target_link_libraries(magpie_engine_tests PRIVATE
	magpie_engine magpie_bridge PkgConfig::PIPEWIRE magpie_warnings Catch2::Catch2WithMain)
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
		TestAppWindow.cpp
	)
	target_link_libraries(magpie_app_tests PRIVATE magpie_app Qt6::Test magpie_warnings Catch2::Catch2)
	# See render/CMakeLists.txt
	target_compile_options(magpie_app_tests PRIVATE -Wno-missing-field-initializers)
	catch_discover_tests(magpie_app_tests PROPERTIES LABELS app)
endif()
