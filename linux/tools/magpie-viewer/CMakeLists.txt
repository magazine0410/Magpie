add_executable(magpie-viewer main.cpp)
target_link_libraries(magpie-viewer PRIVATE
	magpie_engine magpie_bridge magpie_tools_common magpie_warnings)
target_compile_definitions(magpie-viewer PRIVATE
	MAGPIE_DEFAULT_EFFECTS_DIR="${MAGPIE_EFFECTS_DIR}"
	VK_USE_PLATFORM_WAYLAND_KHR
)
# See render/CMakeLists.txt
target_compile_options(magpie-viewer PRIVATE -Wno-missing-field-initializers)
