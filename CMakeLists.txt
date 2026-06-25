cmake_minimum_required(VERSION 3.16)
project(webcamproxy LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(webcamproxy
    src/main.cpp
    src/v4l2.cpp
    src/rotate.cpp
    src/mjpeg.cpp
)

find_library(TURBOJPEG_LIB turbojpeg REQUIRED)
find_package(JPEG REQUIRED)
target_link_libraries(webcamproxy PRIVATE ${TURBOJPEG_LIB} JPEG::JPEG)
