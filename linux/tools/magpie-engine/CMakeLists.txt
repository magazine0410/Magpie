add_executable(magpie-engine main.cpp)
target_link_libraries(magpie-engine PRIVATE magpie_engine magpie_bridge magpie_tools_common magpie_warnings)
target_compile_definitions(magpie-engine PRIVATE
	MAGPIE_DEFAULT_EFFECTS_DIR="${MAGPIE_EFFECTS_DIR}"
	MAGPIE_APP_ID="${MAGPIE_APP_ID}"
	MAGPIE_OBJECT_PATH="${MAGPIE_OBJECT_PATH}"
)
# See render/CMakeLists.txt
target_compile_options(magpie-engine PRIVATE -Wno-missing-field-initializers)

# The .desktop file that makes the program known to the desktop. In a build
# tree it points to the executable there; copy it to
# ~/.local/share/applications to use the global shortcuts with that build.
file(GENERATE
	OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/${MAGPIE_APP_ID}.desktop"
	INPUT "${CMAKE_CURRENT_SOURCE_DIR}/../../data/magpie.desktop.in"
)
