# PokeDaisy for BananaOS: libmgba (the submodule, or POKEDAISY_MGBA_DIR - the
# BananaLinux package downloads it beside the sources) + the portable companion
# core (../3ds/core) + this host, one static binary that needs only libc
# (ALSA is loaded at run time). `make -C banana` builds it for this computer;
# BananaLinux builds it for the handhelds (buildroot/package/pokedaisy).
cmake_minimum_required(VERSION 3.13)
project(pokedaisy_banana C)

set(POKEDAISY_MGBA_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/mgba" CACHE PATH "mGBA's sources (0.10.5)")
set(POKEDAISY_VERSION "dev" CACHE STRING "the version it reports")
set(POKEDAISY_CPU "" CACHE STRING "-mcpu for the handhelds (cortex-a55), empty for this computer")
option(POKEDAISY_WERROR "warnings are errors (this host's sources)" OFF)

get_filename_component(MGBA_DIR "${POKEDAISY_MGBA_DIR}" ABSOLUTE)
if(NOT EXISTS "${MGBA_DIR}/CMakeLists.txt")
    message(FATAL_ERROR "mGBA missing at ${MGBA_DIR} - run: git submodule update --init third_party/mgba")
endif()

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()
if(POKEDAISY_CPU)
    add_compile_options(-mcpu=${POKEDAISY_CPU})
endif()

# libmgba only, GBA only - the same switches as the 3DS and Android builds.
set(BUILD_STATIC        ON  CACHE BOOL "" FORCE)
set(BUILD_SHARED        OFF CACHE BOOL "" FORCE)
set(DISABLE_FRONTENDS   ON  CACHE BOOL "" FORCE)
set(DISABLE_DEPS        ON  CACHE BOOL "" FORCE)
set(SKIP_LIBRARY        OFF CACHE BOOL "" FORCE)
set(BUILD_GL            OFF CACHE BOOL "" FORCE)
set(BUILD_GLES2         OFF CACHE BOOL "" FORCE)
set(BUILD_GLES3         OFF CACHE BOOL "" FORCE)
set(BUILD_PERF          OFF CACHE BOOL "" FORCE)
set(BUILD_TEST          OFF CACHE BOOL "" FORCE)
set(BUILD_SUITE         OFF CACHE BOOL "" FORCE)
set(BUILD_LIBRETRO      OFF CACHE BOOL "" FORCE)
set(BUILD_PYTHON        OFF CACHE BOOL "" FORCE)
set(BUILD_QT            OFF CACHE BOOL "" FORCE)
set(BUILD_SDL           OFF CACHE BOOL "" FORCE)
set(USE_DEBUGGERS       OFF CACHE BOOL "" FORCE)
set(USE_GDB_STUB        OFF CACHE BOOL "" FORCE)
set(USE_EDITLINE        OFF CACHE BOOL "" FORCE)
set(USE_FFMPEG          OFF CACHE BOOL "" FORCE)
set(USE_DISCORD_RPC     OFF CACHE BOOL "" FORCE)
set(USE_LZMA            OFF CACHE BOOL "" FORCE)
set(USE_LUA             OFF CACHE BOOL "" FORCE)
set(USE_ELF             OFF CACHE BOOL "" FORCE)
set(USE_SQLITE3         OFF CACHE BOOL "" FORCE)
set(USE_LIBZIP          OFF CACHE BOOL "" FORCE)
set(USE_MINIZIP         OFF CACHE BOOL "" FORCE)
set(USE_PNG             OFF CACHE BOOL "" FORCE)
set(USE_ZLIB            OFF CACHE BOOL "" FORCE)
set(USE_EPOXY           OFF CACHE BOOL "" FORCE)
set(USE_FREETYPE        OFF CACHE BOOL "" FORCE)
set(M_CORE_GBA          ON  CACHE BOOL "" FORCE)
set(M_CORE_GB           OFF CACHE BOOL "" FORCE)
set(BUILD_LTO           OFF CACHE BOOL "" FORCE)
set(SKIP_INSTALL_ALL    ON  CACHE BOOL "" FORCE)
set(ENABLE_SCRIPTING    OFF CACHE BOOL "" FORCE)

add_subdirectory("${MGBA_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/mgba" EXCLUDE_FROM_ALL)

file(GLOB CORE_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/../3ds/core/*.c")
set(HOST_SOURCES
    src/main.c src/display.c src/scale.c src/input.c src/audio.c src/files.c
    ../3ds/desktop/pd_png.c)

add_executable(pokedaisy ${HOST_SOURCES} ${CORE_SOURCES})
target_include_directories(pokedaisy PRIVATE
    "${MGBA_DIR}/include"
    "${MGBA_DIR}/src"
    "${CMAKE_CURRENT_BINARY_DIR}/mgba/include")
# the defines mGBA's public headers are sized by must match the library's (32-bit colour here)
target_compile_definitions(pokedaisy PRIVATE M_CORE_GBA BUILD_STATIC _GNU_SOURCE
    POKEDAISY_VERSION="${POKEDAISY_VERSION}")
target_compile_options(pokedaisy PRIVATE -std=gnu11 -O2 -Wall -Wextra)
if(POKEDAISY_WERROR)
    # this host's sources only: the core is the 3DS port's, kept as it builds there
    set_source_files_properties(${HOST_SOURCES} PROPERTIES COMPILE_OPTIONS "-Werror")
endif()
target_link_libraries(pokedaisy mgba m dl pthread)

install(TARGETS pokedaisy RUNTIME DESTINATION bin)
