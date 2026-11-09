add_executable(magpie_tests
	TestEffectCacheManager.cpp
	TestEffectCompiler.cpp
	TestLogger.cpp
	TestPaths.cpp
	TestScalingOptions.cpp
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

# Every bundled effect in every combination of FP16 mode and inline parameters.
# Takes about a minute on a twelve-thread machine.
add_test(NAME effects-compile-all
	COMMAND magpie-fxc -q --matrix --all "${MAGPIE_EFFECTS_DIR}")
set_tests_properties(effects-compile-all PROPERTIES LABELS slow TIMEOUT 3600)

add_test(NAME upstream-sync
	COMMAND ${CMAKE_COMMAND} "-DREPO_ROOT=${CMAKE_SOURCE_DIR}/.." -P "${CMAKE_CURRENT_SOURCE_DIR}/UpstreamSync.cmake")
