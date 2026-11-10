add_executable(magpie-scale main.cpp)
target_link_libraries(magpie-scale PRIVATE magpie_engine magpie_bridge magpie_tools_common magpie_warnings)
target_compile_definitions(magpie-scale PRIVATE
	MAGPIE_DEFAULT_EFFECTS_DIR="${MAGPIE_EFFECTS_DIR}"
	MAGPIE_DEFAULT_ICON_FONT="${MAGPIE_ICON_FONT}"
	MAGPIE_APP_ID="${MAGPIE_APP_ID}"
)
# See render/CMakeLists.txt
target_compile_options(magpie-scale PRIVATE -Wno-missing-field-initializers)
