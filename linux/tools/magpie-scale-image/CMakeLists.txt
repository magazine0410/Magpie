add_executable(magpie-scale-image main.cpp)
target_link_libraries(magpie-scale-image PRIVATE magpie_render magpie_tools_common magpie_warnings)
# In a build tree the effects are read from the source tree.
target_compile_definitions(magpie-scale-image PRIVATE
	MAGPIE_DEFAULT_EFFECTS_DIR="${MAGPIE_EFFECTS_DIR}"
)
