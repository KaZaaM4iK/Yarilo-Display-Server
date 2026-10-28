cmake_minimum_required(VERSION 3.25)
project(Yarilo LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_compile_options(-Wall -Wextra -Wpedantic -Werror=return-type)

# Sprint 6.1 Input Subsystem строго зафиксирован под Linux (libudev + libinput)
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "Yarilo Sprint 6.1 Input Subsystem currently requires Linux (libudev + libinput). FreeBSD backend is scheduled for Sprint 7.")
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBDRM REQUIRED IMPORTED_TARGET libdrm)
pkg_check_modules(LIBSEAT REQUIRED IMPORTED_TARGET libseat)
pkg_check_modules(LIBINPUT REQUIRED IMPORTED_TARGET libinput)
pkg_check_modules(LIBUDEV REQUIRED IMPORTED_TARGET libudev)

# Основной сервер Yarilo
add_executable(yarilo
    src/main.cpp
    src/core/error.cpp
    src/core/surface.cpp
    src/platform/seat.cpp
    src/platform/drm/output.cpp
    src/ipc/server_socket.cpp
)

target_include_directories(yarilo PRIVATE include)
target_link_libraries(yarilo PRIVATE 
    PkgConfig::LIBDRM 
    PkgConfig::LIBSEAT
    PkgConfig::LIBINPUT
    PkgConfig::LIBUDEV
)

# Тестовый клиент Yarilo
add_executable(yarilo_client
    tools/client_test.cpp
)
target_include_directories(yarilo_client PRIVATE include)

# Внешний Window Manager Yarilo
add_executable(yarilo_wm
    tools/wm_test.cpp
)
target_include_directories(yarilo_wm PRIVATE include)