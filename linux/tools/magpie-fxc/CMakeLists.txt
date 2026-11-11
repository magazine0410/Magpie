add_executable(magpie-fxc main.cpp)
target_link_libraries(magpie-fxc PRIVATE magpie_fx magpie_warnings)

install(TARGETS magpie-fxc RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
