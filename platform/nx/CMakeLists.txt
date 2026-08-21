# The Switch build. devkitA64, an NRO, and borealis for the interface.
#
# This is the one target in the project that is CMake rather than a devkitPro
# Makefile, and the reason is borealis: it is a CMake project with a hundred or so
# compile options and include paths that follow from which backend it was built
# with. Copying that list into a Makefile would be two places to keep one truth,
# which is the mistake `CLAUDE.md` records about the Go version. Here the app links
# the `borealis` target and CMake carries the flags across.
#
# Build it the way everything else is built:
#
#   make docker-nx        from the repository root
#
# `platform/nx/Makefile` is a two line wrapper so that `make -C platform/nx` and CI
# keep working unchanged.

cmake_minimum_required(VERSION 3.13)

set(DAEMOON_ROOT ${CMAKE_CURRENT_LIST_DIR}/../..)
set(BOREALIS_LIBRARY ${DAEMOON_ROOT}/vendor/borealis/library)

# borealis decides the toolchain, so its options have to be answered before the
# toolchain file it picks is included, and both before project(). FORCE because
# these are choices this build makes rather than defaults a caller may flip: an NRO
# built for the desktop backend is not a thing.
set(PLATFORM_SWITCH ON CACHE BOOL "" FORCE)
set(USE_GLFW ON CACHE BOOL "" FORCE)
# deko3d would be the faster driver and this toolchain has no deko3d port, so the
# GLFW and EGL path is not a preference. If a deko3d package appears, the switch is
# this line plus `uam` shaders in the romfs - see gen_dksh in borealis.
set(USE_DEKO3D OFF CACHE BOOL "" FORCE)
# Resources are read out of the NRO's own romfs, so there is nothing to place on the
# card beside the binary. libromfs would bake them into the executable instead,
# which costs a build step and buys nothing here.
set(USE_LIBROMFS OFF CACHE BOOL "" FORCE)

include(${BOREALIS_LIBRARY}/cmake/commonOption.cmake)
include(${BOREALIS_LIBRARY}/cmake/toolchain.cmake)

project(daemoon C CXX ASM)

set(APP_TITLE   "DaeMoon")
set(APP_AUTHOR  "DaeMoon")
set(APP_VERSION "0.1.0")

add_subdirectory(${BOREALIS_LIBRARY} borealis)

# Vendored headers are not this project's to warn about. Without this, -Wall -Wextra
# on our own files reports every unused parameter in a borealis header they include,
# which buries the warnings that are actually ours.
get_target_property(BOREALIS_INCLUDE_DIRS borealis INTERFACE_INCLUDE_DIRECTORIES)
set_target_properties(borealis PROPERTIES
    INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${BOREALIS_INCLUDE_DIRS}")

# --------------------------------------------------------------------- sources
#
# The same core every other target links. Nothing in it knows what a Switch is, and
# CI greps to keep it that way.

file(GLOB DAEMOON_CORE_SRC
    ${DAEMOON_ROOT}/core/src/*.c
    ${DAEMOON_ROOT}/core/src/util/*.c)

# What turned out to be identical on both consoles: the SD card over newlib stdio,
# the libcurl request loop, the directory walk, and the key=value reader.
file(GLOB DAEMOON_COMMON_SRC ${DAEMOON_ROOT}/platform/common/*.c)

file(GLOB DAEMOON_NX_SRC
    ${CMAKE_CURRENT_LIST_DIR}/source/*.c
    ${CMAKE_CURRENT_LIST_DIR}/source/*.cpp
    ${CMAKE_CURRENT_LIST_DIR}/source/ui/*.cpp)

# The backend contract, checked on hardware rather than only on a desktop. The same
# file the 3DS build links and the same one `make core-test` runs against a stub: it
# is written against daemoon_save_backend_t and knows nothing about how a save is
# reached, which is why it can be pointed at this backend unchanged.
#
# Two files, not the directory: the rest of tools/test has a main() and wants
# fixtures.
set(DAEMOON_TEST_SRC
    ${DAEMOON_ROOT}/tools/test/backend_conformance.c
    ${DAEMOON_ROOT}/tools/test/test_failure.c)

# borealis supplies the Switch entry point. It sizes the heap for the graphics
# driver before main runs, which is not something an app can do afterwards.
set(BOREALIS_WRAPPER ${BOREALIS_LIBRARY}/lib/platforms/switch/switch_wrapper.c)

add_executable(daemoon
    ${DAEMOON_CORE_SRC}
    ${DAEMOON_COMMON_SRC}
    ${DAEMOON_NX_SRC}
    ${DAEMOON_TEST_SRC}
    ${DAEMOON_ROOT}/vendor/miniz/miniz.c
    ${BOREALIS_WRAPPER})

target_include_directories(daemoon PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/source
    ${DAEMOON_ROOT}/platform/common
    ${DAEMOON_ROOT}/core/include
    ${DAEMOON_ROOT}/vendor
    ${DAEMOON_ROOT}/tools/test)

set_target_properties(daemoon PROPERTIES
    C_STANDARD 11
    CXX_STANDARD 17
    OUTPUT_NAME daemoon)

target_compile_options(daemoon PRIVATE -Wall -Wextra -ffunction-sections -fdata-sections)

# The public root certificates, linked in rather than read off the SD card. Same
# reason as the 3DS build: a console that needs a .pem placed beside it before https
# works is one that works for whoever built it and nobody else.
dkp_add_embedded_binary_library(daemoon_cacert
    ${DAEMOON_ROOT}/vendor/cacert/data/cacert.bin)

# curl and mbedtls statically, for the same reason the 3DS build links them: a self
# hosted server is exactly the case where the operator cannot be asked to downgrade
# their TLS to suit a console. Order matters - curl needs mbedtls, which needs z.
target_link_libraries(daemoon PRIVATE
    daemoon_cacert
    borealis
    curl mbedtls mbedx509 mbedcrypto z)

target_link_options(daemoon PRIVATE -Wl,--gc-sections -Wl,-Map,daemoon.map)

# ---------------------------------------------------------------------- romfs
#
# borealis reads three things at runtime out of `romfs:/`: the icon font it draws
# button glyphs with, the touch cursor, and its own six hint strings.
#
# No text font is copied. On this platform borealis draws with the console's own
# shared fonts through plGetSharedFontByType, falling back per glyph across the
# Korean, both Chinese and the Nintendo extended sets - which is what makes this
# screen readable in eight languages where the text console it replaced could draw
# ASCII and nothing else.
#
# `platform/nx/romfs/i18n` is generated by `make gen` out of shared/lang, so those
# six strings are translated in the same place as every other string in the project
# rather than in borealis's own four languages.
set(DAEMOON_ROMFS ${CMAKE_CURRENT_BINARY_DIR}/romfs)
add_custom_target(daemoon_romfs
    COMMAND ${CMAKE_COMMAND} -E rm -rf ${DAEMOON_ROMFS}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${DAEMOON_ROMFS}/icon
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${DAEMOON_ROOT}/vendor/borealis/resources/material ${DAEMOON_ROMFS}/material
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${DAEMOON_ROOT}/vendor/borealis/resources/img ${DAEMOON_ROMFS}/img
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${CMAKE_CURRENT_LIST_DIR}/romfs/i18n ${DAEMOON_ROMFS}/i18n
    COMMAND ${CMAKE_COMMAND} -E copy
            ${CMAKE_CURRENT_LIST_DIR}/assets/icon.jpg ${DAEMOON_ROMFS}/icon/icon.jpg
    COMMENT "Assembling romfs")

# ------------------------------------------------------------------------ nro
#
# The NRO lands beside this file rather than in the build directory, because that
# is the path CI publishes and the path the deploy script uploads.
set(DAEMOON_NRO ${CMAKE_CURRENT_LIST_DIR}/daemoon.nro)
set(DAEMOON_NACP ${CMAKE_CURRENT_BINARY_DIR}/daemoon.nacp)

add_custom_command(OUTPUT ${DAEMOON_NACP}
    COMMAND ${NX_NACPTOOL_EXE} --create ${APP_TITLE} ${APP_AUTHOR} ${APP_VERSION} ${DAEMOON_NACP}
    COMMENT "Generating daemoon.nacp")

add_custom_command(OUTPUT ${DAEMOON_NRO}
    DEPENDS daemoon ${DAEMOON_NACP} daemoon_romfs
    COMMAND ${NX_ELF2NRO_EXE} $<TARGET_FILE:daemoon> ${DAEMOON_NRO}
            --icon=${CMAKE_CURRENT_LIST_DIR}/assets/icon.jpg
            --nacp=${DAEMOON_NACP}
            --romfsdir=${DAEMOON_ROMFS}
    COMMENT "Generating daemoon.nro")

add_custom_target(nro ALL DEPENDS ${DAEMOON_NRO})
