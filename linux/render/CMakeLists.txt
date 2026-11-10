# Vulkan renderer: device, effect chain and what it needs to load and time
# effects.
find_package(Vulkan REQUIRED)
find_package(muparser REQUIRED)
find_package(PNG REQUIRED)

add_library(magpie_render STATIC
	EffectChain.cpp
	EffectDrawer.cpp
	EffectsProfiler.cpp
	ImageFile.cpp
	ImageScaler.cpp
	ScalingGeometry.cpp
	VulkanDevice.cpp
)

target_include_directories(magpie_render PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(magpie_render
	PUBLIC magpie_core magpie_fx Vulkan::Vulkan muparser::muparser
	PRIVATE PNG::PNG magpie_warnings
)
# Vulkan structures are filled with designated initializers that name only the
# members in use; the rest is zero, which is what Vulkan expects. GCC reports
# each omitted member otherwise.
target_compile_options(magpie_render PRIVATE -Wno-missing-field-initializers)
