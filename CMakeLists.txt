cmake_minimum_required(VERSION 3.25)
project(Yarilo LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(YARILO_B2_TEST_CONTROL "Build and enable B2 recovery test control plane" OFF)
if (YARILO_B2_TEST_CONTROL AND NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "YARILO_B2_TEST_CONTROL requires Linux (SO_PEERCRED)")
endif ()

add_compile_options(-Wall -Wextra -Wpedantic -Werror=return-type)

# Sprint 6.1 Input Subsystem строго зафиксирован под Linux (libudev + libinput)
if (NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "Yarilo Sprint 6.1 Input Subsystem currently requires Linux (libudev + libinput). FreeBSD backend is scheduled for Sprint 7.")
endif ()

find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBDRM REQUIRED IMPORTED_TARGET libdrm)
pkg_check_modules(LIBSEAT REQUIRED IMPORTED_TARGET libseat)
pkg_check_modules(LIBINPUT REQUIRED IMPORTED_TARGET libinput)
pkg_check_modules(LIBUDEV REQUIRED IMPORTED_TARGET libudev)

if (YARILO_B2_TEST_CONTROL)
    add_executable(yarilo_b2_coordinator tools/b2_coordinator.cpp)
    target_include_directories(yarilo_b2_coordinator PRIVATE include)
endif ()

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

if (YARILO_B2_TEST_CONTROL)
    target_compile_definitions(yarilo PRIVATE YARILO_B2_TEST_CONTROL=1)
    target_compile_definitions(yarilo_client PRIVATE YARILO_B2_TEST_CONTROL=1)
    target_compile_definitions(yarilo_b2_coordinator PRIVATE YARILO_B2_TEST_CONTROL=1)
endif ()

# Внешний Window Manager Yarilo
add_executable(yarilo_wm
        tools/wm_test.cpp
)
target_include_directories(yarilo_wm PRIVATE include)

# Интеграция CTest и unit-тестов ядра
include(CTest)

if (BUILD_TESTING)
    add_executable(test_surface_manager
            tests/test_surface_manager.cpp
            src/core/surface.cpp
            src/core/error.cpp
    )
    target_include_directories(test_surface_manager PRIVATE include)
    target_link_libraries(test_surface_manager PRIVATE PkgConfig::LIBDRM)

    add_test(NAME SurfaceManager_UnitTests COMMAND test_surface_manager)

    add_executable(test_server_orchestration
            tests/test_server_orchestration.cpp
    )
    target_include_directories(test_server_orchestration PRIVATE include)
    add_test(NAME ServerOrchestration_UnitTests COMMAND test_server_orchestration)

    if (CMAKE_SYSTEM_NAME STREQUAL "Linux")
        add_executable(test_b2_peer_credentials tests/test_b2_peer_credentials.cpp)
        target_include_directories(test_b2_peer_credentials PRIVATE include)
        add_test(NAME B2PeerCredentials_UnitTests COMMAND test_b2_peer_credentials)
    endif ()
endif ()