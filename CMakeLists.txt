cmake_minimum_required(VERSION 3.16)
project(webcamproxy LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_library(TURBOJPEG_LIB turbojpeg REQUIRED)
find_package(JPEG REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(YAMLCPP REQUIRED yaml-cpp)

add_executable(webcamproxy
    src/main.cpp
    src/v4l2.cpp
    src/rotate.cpp
    src/mjpeg.cpp
    src/config.cpp
)
target_link_libraries(webcamproxy PRIVATE ${TURBOJPEG_LIB} JPEG::JPEG
    ${YAMLCPP_LIBRARIES})
target_include_directories(webcamproxy PRIVATE ${YAMLCPP_INCLUDE_DIRS})
