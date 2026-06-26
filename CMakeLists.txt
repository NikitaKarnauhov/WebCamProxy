cmake_minimum_required(VERSION 3.16)
project(webcamproxy LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(GNUInstallDirs)

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

install(TARGETS webcamproxy RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
if(CMAKE_INSTALL_PREFIX MATCHES "^/usr")
    install(FILES config.example.yml
            DESTINATION ${CMAKE_INSTALL_SYSCONFDIR}/WebCamProxy
            RENAME config.yml
            OPTIONAL)
    set(CONFIG_PATH ${CMAKE_INSTALL_FULL_SYSCONFDIR}/WebCamProxy/config.yml)
else()
    install(FILES config.example.yml
            DESTINATION $ENV{HOME}/.config/WebCamProxy
            RENAME config.yml
            OPTIONAL)
    set(CONFIG_PATH $ENV{HOME}/.config/WebCamProxy/config.yml)
endif()

# Systemd unit
configure_file(webcamproxy.service.in webcamproxy.service @ONLY)
pkg_check_modules(SYSTEMD systemd)
if(SYSTEMD_FOUND)
    if(CMAKE_INSTALL_PREFIX MATCHES "^/usr")
        # System-wide: use systemd's system unit path
        pkg_get_variable(SYSTEMD_SYSTEMUNITDIR systemd systemd_system_unit_dir)
        install(FILES ${CMAKE_CURRENT_BINARY_DIR}/webcamproxy.service
                DESTINATION ${SYSTEMD_SYSTEMUNITDIR})
    else()
        # User-local: systemd looks in ~/.config/systemd/user
        install(FILES ${CMAKE_CURRENT_BINARY_DIR}/webcamproxy.service
                DESTINATION $ENV{HOME}/.config/systemd/user)
    endif()
else()
    message(WARNING "systemd pkg-config not found, skipping unit install")
endif()
