add_executable(magpie-present-test main.cpp)
target_link_libraries(magpie-present-test PRIVATE magpie_render magpie_tools_common magpie_warnings)
target_compile_definitions(magpie-present-test PRIVATE
	MAGPIE_DEFAULT_EFFECTS_DIR="${MAGPIE_EFFECTS_DIR}"
	VK_USE_PLATFORM_WAYLAND_KHR
)
# See render/CMakeLists.txt
target_compile_options(magpie-present-test PRIVATE -Wno-missing-field-initializers)
