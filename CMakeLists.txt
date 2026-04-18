cmake_minimum_required(VERSION 3.16)
project(lottie_salute VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(PkgConfig REQUIRED)
find_program(WAYLAND_SCANNER wayland-scanner)

set(PROTOCOL_DIR "${CMAKE_SOURCE_DIR}/protocol")

# ── Option: Wayland backend ──────────────────────────────────────────────────

set(ENABLE_WAYLAND OFF)
if(WAYLAND_SCANNER)
    pkg_check_modules(WAYLAND_DEPS IMPORTED_TARGET wayland-client)
    if(WAYLAND_DEPS_FOUND)
        set(ENABLE_WAYLAND ON)

        # wlr-layer-shell-unstable-v1
        set(LS_XML "${PROTOCOL_DIR}/wlr-layer-shell-unstable-v1.xml")
        set(LS_CODE "${CMAKE_CURRENT_BINARY_DIR}/wlr-layer-shell-protocol.c")
        set(LS_HEADER "${CMAKE_CURRENT_BINARY_DIR}/wlr-layer-shell-client-protocol.h")
        add_custom_command(
            OUTPUT "${LS_CODE}" "${LS_HEADER}"
            COMMAND "${WAYLAND_SCANNER}" private-code "${LS_XML}" "${LS_CODE}.tmp"
            COMMAND "${WAYLAND_SCANNER}" client-header "${LS_XML}" "${LS_HEADER}.tmp"
            COMMAND sed -i -e "s/ namespace/ _namespace/g" -e "s/\\*namespace/*_namespace/g" "${LS_CODE}.tmp" "${LS_HEADER}.tmp"
            COMMAND ${CMAKE_COMMAND} -E copy "${LS_CODE}.tmp" "${LS_CODE}"
            COMMAND ${CMAKE_COMMAND} -E copy "${LS_HEADER}.tmp" "${LS_HEADER}"
            DEPENDS "${LS_XML}"
            COMMENT "Generating wlr-layer-shell protocol"
        )

        # xdg-output-unstable-v1
        set(XO_XML "${PROTOCOL_DIR}/xdg-output-unstable-v1.xml")
        set(XO_CODE "${CMAKE_CURRENT_BINARY_DIR}/xdg-output-protocol.c")
        set(XO_HEADER "${CMAKE_CURRENT_BINARY_DIR}/xdg-output-unstable-v1-client-protocol.h")
        add_custom_command(
            OUTPUT "${XO_CODE}" "${XO_HEADER}"
            COMMAND "${WAYLAND_SCANNER}" private-code "${XO_XML}" "${XO_CODE}"
            COMMAND "${WAYLAND_SCANNER}" client-header "${XO_XML}" "${XO_HEADER}"
            DEPENDS "${XO_XML}"
            COMMENT "Generating xdg-output protocol"
        )

        add_library(ls_protos STATIC "${LS_CODE}")
        target_include_directories(ls_protos PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")

        add_library(xo_protos STATIC "${XO_CODE}")
        target_include_directories(xo_protos PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
    endif()
endif()

# ── Option: X11 backend ─────────────────────────────────────────────────────

set(ENABLE_X11 OFF)
pkg_check_modules(X11_DEPS IMPORTED_TARGET x11 xext xrender xrandr)
if(X11_DEPS_FOUND)
    set(ENABLE_X11 ON)
endif()

if(NOT ENABLE_WAYLAND AND NOT ENABLE_X11)
    message(FATAL_ERROR "No display backend available. Install Wayland or X11 development libraries.")
endif()

# ── rlottie (required) ──────────────────────────────────────────────────────

pkg_check_modules(RLOTTIE REQUIRED IMPORTED_TARGET rlottie)

# ── lottie_salute executable ────────────────────────────────────────────────

set(SALUTE_SOURCES src/main.cpp)
set(SALUTE_LIBS PkgConfig::RLOTTIE)
set(SALUTE_DEFINES VERSION="${PROJECT_VERSION}")

if(ENABLE_WAYLAND)
    list(APPEND SALUTE_SOURCES src/wayland_backend.cpp)
    list(APPEND SALUTE_LIBS ls_protos xo_protos PkgConfig::WAYLAND_DEPS)
    list(APPEND SALUTE_DEFINES ENABLE_WAYLAND)
    message(STATUS "Wayland backend: enabled")
else()
    message(STATUS "Wayland backend: disabled (wayland-client or wayland-scanner not found)")
endif()

if(ENABLE_X11)
    list(APPEND SALUTE_SOURCES src/x11_backend.cpp)
    list(APPEND SALUTE_LIBS PkgConfig::X11_DEPS)
    list(APPEND SALUTE_DEFINES ENABLE_X11)
    message(STATUS "X11 backend: enabled")
else()
    message(STATUS "X11 backend: disabled (x11/xrender not found)")
endif()

add_executable(lottie-salute ${SALUTE_SOURCES})
target_link_libraries(lottie-salute PRIVATE ${SALUTE_LIBS})
target_include_directories(lottie-salute PRIVATE "${CMAKE_CURRENT_BINARY_DIR}" "${CMAKE_SOURCE_DIR}/src")
target_compile_definitions(lottie-salute PRIVATE ${SALUTE_DEFINES})

# ── Install ──────────────────────────────────────────────────────────────────

install(TARGETS lottie-salute RUNTIME DESTINATION bin)
