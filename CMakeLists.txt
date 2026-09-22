cmake_minimum_required(VERSION 3.16)
project(propcycl C)

set(CMAKE_C_STANDARD 11)

# OPTIMISE BY DEFAULT.
#
# This file set no optimisation flags and nothing set CMAKE_BUILD_TYPE, so an
# ordinary `cmake .. && make` produced `-g -std=gnu11` -- i.e. **-O0**. The
# whole engine ran unoptimised: the texture bake, the _W[] sync (which moves
# ~8 MB per frame), geo_hw's per-vertex math, all of it. Measured over a
# 3600-frame autopilot level, -O0 against -O2 with IDENTICAL work done
# (608M texels baked, 50276 evictions, same realloc/reuse counts):
#
#     game logic   5.74s -> 2.16s      display-list walk  1.15s -> 0.47s
#     _W[] sync    5.63s -> 2.11s      text layer         2.42s -> 1.64s
#     texture bake 16.03s -> 10.32s    fog                0.38s -> 0.06s
#     game+render  32.0s -> 17.9s  (1.79x)
#     frames over the 16.67 ms budget  552 -> 284
#
# Output is BYTE-IDENTICAL: all six framedump reference frames and a full
# 3300-frame gameplay run md5-match the -O0 build, and the geo gate passes.
#
# -fno-strict-aliasing is NOT optional. This tree is transpiled C that reads
# the same memory through several types (the _W[] intptr_t array aliased as
# int*, ROM bytes read as words, spriteram as u8/u16/u32), which is exactly
# what -O2's strict-aliasing rules assume never happens. -fwrapv likewise:
# the transpiled arithmetic relies on signed overflow wrapping the way the
# 68K does. Do not drop either to chase another few percent.
#
# A caller can still override with -DCMAKE_BUILD_TYPE=Debug for gdb work.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()
add_compile_options(-fno-strict-aliasing -fwrapv)
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    message(WARNING "Debug build: -O0. The engine runs ~1.8x slower; "
                    "use RelWithDebInfo unless you are stepping in gdb.")
endif()

# The transpiled game_*.c files rely on gcc's gnu89 leniency (functions used
# before they are declared, then defined with another return type); clang
# rejects that as an error. ./build.sh always configures with gcc.
if(NOT CMAKE_C_COMPILER_ID STREQUAL "GNU")
    message(FATAL_ERROR "Prop Cycle must be built with gcc (found ${CMAKE_C_COMPILER_ID}). "
                        "Use ./build.sh, or configure with -DCMAKE_C_COMPILER=gcc.")
endif()

# -Werror only when asked for: a new compiler release adds warnings, and a
# player's build must not fail on one. Developers: -DPROPCYCL_WERROR=ON.
option(PROPCYCL_WERROR "Treat warnings as errors in the v2 tree" OFF)
if(PROPCYCL_WERROR)
    set(PROPCYCL_WERROR_FLAG "-Werror")
else()
    set(PROPCYCL_WERROR_FLAG "")
endif()

# Windows (MinGW-w64): map the handful of POSIX calls onto Windows. See
# include/win_compat.h and docs/WINDOWS.md.
if(WIN32)
    add_compile_options(-include ${CMAKE_SOURCE_DIR}/include/win_compat.h)
    # A normal windowed program (no console window). Errors are shown in
    # dialog boxes. Static libgcc so the .exe needs only SDL2.dll beside it.
    add_link_options(-mwindows -static-libgcc -static)
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(SDL2 REQUIRED sdl2)
set(OpenGL_GL_PREFERENCE GLVND)
find_package(OpenGL REQUIRED)
find_package(ZLIB REQUIRED)

# Engine sources shared by the game binary and the standalone rig viewer.
set(PROPCYCL_ENGINE_SRC
    src/memory.c
    src/trace.c
    src/hw_regs.c
    src/renderer_2d.c
    src/renderer_3d.c
    src/audio.c
    src/geo_hw.c
    src/fog_hw.c
    src/sprite_hw.c
    src/text_hw.c
    src/ui_menu.c
    src/flight_rec.c
    src/level_select.c
    src/framedump.c
    src/renderer_texture.c
    src/rom_zip.c
    src/input.c
    src/rom_loader.c
    src/game_logic.c
    src/game_debug.c
    src/game_dsp3d.c
    src/game_eeprom.c
    src/game_ending.c
    src/game_gameplay.c
    src/game_hud.c
    src/game_math.c
    src/game_misc.c
    src/game_objects.c
    src/game_player.c
    src/game_replay.c
    src/game_system.c
    src/game_terrain.c
    src/game_title.c
    src/game_core.c
    src/game_stubs.c
    src/rom_decode.c
    src/arcade_menu.c
    src/v2/game_math_v2.c
    src/screenshot.c
    src/game_props.c
    src/c352.c
    src/audio_hle.c
    src/mcu_sound.c
    src/m37710.c
)

add_executable(propcycl
    src/main.c
    ${PROPCYCL_ENGINE_SRC}
)

target_include_directories(propcycl PRIVATE include ${SDL2_INCLUDE_DIRS})
target_link_libraries(propcycl PRIVATE ${SDL2_LINK_LIBRARIES} OpenGL::GL m ZLIB::ZLIB)
# Hand-written files get normal warnings
set_source_files_properties(
    src/main.c src/memory.c src/trace.c src/hw_regs.c
    src/renderer_2d.c src/renderer_3d.c src/renderer_texture.c
    src/input.c src/rom_loader.c src/game_logic.c src/flight_rec.c
    src/level_select.c
    src/rom_decode.c
    src/c352.c
    src/audio_hle.c
    src/mcu_sound.c
    src/m37710.c
    PROPERTIES COMPILE_FLAGS "-Wall -Wno-unused-variable -Wno-unused-function"
)
# v2 regenerated tree: -Werror per GUARDRAILS L0
set_source_files_properties(
    src/v2/game_math_v2.c
    PROPERTIES COMPILE_FLAGS "-Wall ${PROPCYCL_WERROR_FLAG}"
)
# Transpiled/split files: gnu89 to allow implicit declarations + conflicting types
set_source_files_properties(
    src/game_debug.c
    src/game_dsp3d.c
    src/game_eeprom.c
    src/game_ending.c
    src/game_gameplay.c
    src/game_hud.c
    src/game_math.c
    src/game_misc.c
    src/game_objects.c
    src/game_player.c
    src/game_replay.c
    src/game_system.c
    src/game_terrain.c
    src/game_title.c
    PROPERTIES COMPILE_FLAGS "-w -std=gnu89 -Wno-int-conversion -Wno-incompatible-pointer-types -Wno-implicit-function-declaration"
)
# Hand-ported files: need C11 for for-loop declarations
set_source_files_properties(
    src/game_core.c
    src/game_stubs.c
    PROPERTIES COMPILE_FLAGS "-w -Wno-int-conversion -Wno-incompatible-pointer-types -Wno-implicit-function-declaration"
)

# L2 differential replay verifier (GUARDRAILS §5 L2) — replays MAME
# per-function traces into the v2 translations. v2 tree builds -Werror (L0).
add_executable(l2_verify
    tools/l3/l2_verify.c
    src/memory.c
    src/trace.c
    src/v2/game_math_v2.c
)
target_include_directories(l2_verify PRIVATE include)
target_compile_options(l2_verify PRIVATE -Wall ${PROPCYCL_WERROR_FLAG})

# Standalone model viewer
add_executable(model_viewer src/model_viewer.c)
target_include_directories(model_viewer PRIVATE ${SDL2_INCLUDE_DIRS})
target_link_libraries(model_viewer PRIVATE ${SDL2_LINK_LIBRARIES} OpenGL::GL m)
target_compile_options(model_viewer PRIVATE -Wall -Wno-unused-variable)

# Standalone course viewer (game-like terrain visibility)
add_executable(course_viewer src/course_viewer.c)
target_include_directories(course_viewer PRIVATE ${SDL2_INCLUDE_DIRS})
target_link_libraries(course_viewer PRIVATE ${SDL2_LINK_LIBRARIES} OpenGL::GL m)
target_compile_options(course_viewer PRIVATE -Wall -Wno-unused-variable)

# Standalone rider/bike rig viewer. Links the ENGINE, so the node chain, the
# 0x8008 wire format, the slot-table composition and geo_hw are the real ones
# -- it only filters the scene down to the rig and magnifies it. See the header
# comment in src/rig_viewer.c for why an eyeball tool is needed here at all.
add_executable(rig_viewer
    src/rig_viewer.c
    ${PROPCYCL_ENGINE_SRC}
)
target_include_directories(rig_viewer PRIVATE include ${SDL2_INCLUDE_DIRS})
target_link_libraries(rig_viewer PRIVATE ${SDL2_LINK_LIBRARIES} OpenGL::GL m ZLIB::ZLIB)
target_compile_options(rig_viewer PRIVATE -Wall -Wno-unused-variable -Wno-unused-function)

# Offline C352 gate: replays a captured register-write stream through our
# ported chip and writes a WAV -- see tools/overnight/c352_gate.py.
add_executable(c352_replay
    tools/overnight/c352_replay.c
    src/c352.c
)
target_include_directories(c352_replay PRIVATE include)
target_compile_options(c352_replay PRIVATE -Wall -Wno-unused-variable)
target_link_libraries(c352_replay PRIVATE m)
