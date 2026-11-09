find_package(Qt6 COMPONENTS Gui)
if(NOT Qt6Gui_FOUND)
	message(STATUS "Qt6 Gui not found, skipping testsource")
	return()
endif()

add_executable(testsource main.cpp)
target_link_libraries(testsource PRIVATE Qt6::Gui)
