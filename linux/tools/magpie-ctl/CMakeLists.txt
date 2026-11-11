find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBSYSTEMD REQUIRED IMPORTED_TARGET libsystemd)

add_executable(magpie-ctl main.cpp)
target_link_libraries(magpie-ctl PRIVATE PkgConfig::LIBSYSTEMD magpie_warnings)
target_compile_definitions(magpie-ctl PRIVATE
	MAGPIE_APP_ID="${MAGPIE_APP_ID}"
	MAGPIE_OBJECT_PATH="${MAGPIE_OBJECT_PATH}"
)

install(TARGETS magpie-ctl RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
