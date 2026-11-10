# What turns a capture into scaled frames: for now the frame source that
# receives a screen cast from PipeWire.
find_package(PkgConfig REQUIRED)
pkg_check_modules(PIPEWIRE REQUIRED IMPORTED_TARGET libpipewire-0.3)

add_library(magpie_engine STATIC
	PipeWireFrameSource.cpp
)

target_include_directories(magpie_engine PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(magpie_engine
	PUBLIC magpie_render
	PRIVATE PkgConfig::PIPEWIRE magpie_warnings
)
# See render/CMakeLists.txt
target_compile_options(magpie_engine PRIVATE -Wno-missing-field-initializers)
