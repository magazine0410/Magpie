# MagpieFX effects: parser, pass source generator, SPIR-V compilation and cache.
find_package(VulkanHeaders REQUIRED)

add_library(magpie_fx STATIC
	EffectCacheManager.cpp
	EffectCompiler.cpp
	ShaderCompiler.cpp
)

target_include_directories(magpie_fx PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(magpie_fx
	PUBLIC magpie_core Vulkan::Headers
	PRIVATE DXC::dxcompiler magpie_warnings
)
