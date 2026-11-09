find_package(PkgConfig REQUIRED)
pkg_check_modules(PIPEWIRE REQUIRED IMPORTED_TARGET libpipewire-0.3)
pkg_check_modules(DRM REQUIRED IMPORTED_TARGET libdrm)
find_package(Vulkan REQUIRED)

add_executable(kwin_cast main.cpp vk_import.cpp)
target_link_libraries(kwin_cast PRIVATE spike_protocols PkgConfig::PIPEWIRE PkgConfig::DRM Vulkan::Vulkan)
# The SPA headers are C headers that rely on GNU extensions when used from C++.
target_compile_options(kwin_cast PRIVATE -Wno-missing-field-initializers)
set_target_properties(kwin_cast PROPERTIES CXX_EXTENSIONS ON)
