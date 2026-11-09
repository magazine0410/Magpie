add_executable(magpie_tests
	TestLogger.cpp
	TestPaths.cpp
	TestScalingOptions.cpp
	TestSettingsJson.cpp
	TestSettingsStore.cpp
	TestStrHelper.cpp
	TestVersion.cpp
)

target_link_libraries(magpie_tests PRIVATE magpie_core magpie_warnings Catch2::Catch2WithMain)
target_compile_definitions(magpie_tests PRIVATE
	MAGPIE_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data"
)

include(Catch)
catch_discover_tests(magpie_tests)
