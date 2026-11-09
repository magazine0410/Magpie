find_package(X11 COMPONENTS Xfixes)
if(NOT X11_Xfixes_FOUND)
	message(STATUS "libXfixes not found, skipping x11_cursor")
	return()
endif()

add_executable(x11_cursor main.c)
target_link_libraries(x11_cursor PRIVATE X11::X11 X11::Xfixes)
