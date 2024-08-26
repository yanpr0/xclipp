cmake_minimum_required(VERSION 3.20 FATAL_ERROR)
project(xclipp)

find_package(X11 REQUIRED)

add_executable(xclipp main.cpp clipper.cpp utils.cpp)
target_include_directories(xclipp PRIVATE ${X11_xcb_INCLUDE_PATH})
target_link_libraries(xclipp PRIVATE ${X11_xcb_LIB})

target_compile_options(xclipp PRIVATE -Wall -Wextra -Wpedantic)
set_property(TARGET xclipp PROPERTY CXX_STANDARD 20)
set(CMAKE_BUILD_TYPE Release)

