add_executable(magpie-engine main.cpp)
target_link_libraries(magpie-engine PRIVATE magpie_engine magpie_bridge magpie_tools_common magpie_warnings)
target_compile_definitions(magpie-engine PRIVATE
	MAGPIE_DEFAULT_EFFECTS_DIR="${MAGPIE_EFFECTS_DIR}"
	MAGPIE_DEFAULT_ICON_FONT="${MAGPIE_ICON_FONT}"
	MAGPIE_APP_ID="${MAGPIE_APP_ID}"
	MAGPIE_OBJECT_PATH="${MAGPIE_OBJECT_PATH}"
)
# See render/CMakeLists.txt
target_compile_options(magpie-engine PRIVATE -Wno-missing-field-initializers)

install(TARGETS magpie-engine RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
