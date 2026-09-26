cmake_minimum_required(VERSION 3.16)
project(propcycl C)

set(CMAKE_C_STANDARD 11)

# PORTABLE (default ON): pin glibc symbol versions so a binary built here runs on
# older systems -- the Steam Deck included. See include/glibc_compat.h.
option(PORTABLE "Bind glibc symbols to baseline versions (runs on glibc >= 2.34)" ON)
if(PORTABLE AND CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
  add_compile_options(-include ${CMAKE_CURRENT_SOURCE_DIR}/include/glibc_compat.h)
endif()

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
    # dialog boxes. Static libgcc and SDL2 (below): the .exe needs no DLL beside it.
    add_link_options(-mwindows -static-libgcc -static)
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(SDL2 REQUIRED sdl2)
# Windows: link SDL2 STATICALLY (libSDL2.a + its Windows system libraries), so the
# .exe is one file. Everything else it imports ships with Windows -- OPENGL32.dll
# too, which hands over to the installed GPU driver and must never be bundled.
if(WIN32)
    set(SDL2_LINK_LIBRARIES ${SDL2_STATIC_LDFLAGS})
endif()
set(OpenGL_GL_PREFERENCE GLVND)
find_package(OpenGL REQUIRED)
find_package(ZLIB REQUIRED)

# THE SHARED ENGINE (engine/): one renderer, texture bake, C352 and sound-MCU
# peripheral model for every System 22 game in this tree (HARD RULE 3).
include(${CMAKE_CURRENT_SOURCE_DIR}/engine/engine.cmake)

# Prop Cycle sources shared by the game binary and the standalone rig viewer.
set(PROPCYCL_ENGINE_SRC
    src/master_dsp.c
    src/memory.c
    src/trace.c
    src/hw_regs.c
    src/renderer_2d.c
    src/renderer_3d.c
    src/audio.c
    src/pc_live2d.c
    src/ui_menu.c
    src/pause_world.c
    src/flight_rec.c
    src/level_select.c
    src/framedump.c
    src/pc_palette.c
    src/rom_zip.c
    src/input.c
    src/pedal_enc.c
    src/pedal_enc_core.c
    src/pedal_enc_serial.c
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
    src/audio_hle.c
    src/mcu_sound.c
    src/snd/snd_engine.c
    gen/snd_driver.c
    ${NAMCO22_ENGINE_GL_SRC}
    ${NAMCO22_ENGINE_SS22_SRC}
    ${NAMCO22_ENGINE_SND_SRC}
    ${NAMCO22_ENGINE_C25_SRC}
    gen/pc_c25.c
)


# The sound program (pr1data.8k), TRANSLATED to C at build time from the ROM and
# tools/gen/snd.cov (addresses only; see tools/gen/snd_translate.py). The game
# contains no fetch/decode loop; propcycl_sndoracle (below) is the test oracle.
find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_custom_command(OUTPUT ${CMAKE_CURRENT_SOURCE_DIR}/gen/snd_driver.c
  COMMAND ${Python3_EXECUTABLE} tools/gen/snd_translate.py tools/gen/snd.cov
  DEPENDS tools/gen/snd_translate.py tools/gen/m7700_decode.py tools/gen/snd_rom.py tools/gen/snd_spin.inc tools/gen/snd.cov src/snd/snd_engine.c
  WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
  COMMENT "Translating the sound program to C")
set_source_files_properties(gen/snd_driver.c PROPERTIES COMPILE_OPTIONS "-O1;-w")
include_directories(src/snd)

# The MASTER DSP program (the C71 BIOS + the block the 68K uploads to program
# 0x4000), translated to C from the ROM files by tools/gen/c25_translate.py --
# the tool Rave Racer uses too. tools/gen/c25_pc.cov: the addresses the
# interpreter oracle executed (addresses only; committed).
add_custom_command(OUTPUT ${CMAKE_CURRENT_SOURCE_DIR}/gen/pc_c25.c
  COMMAND ${Python3_EXECUTABLE} tools/gen/c25_translate.py --game pc --roms extracted
          --cov tools/gen/c25_pc.cov --out gen/pc_c25.c --func pc_c25_exec
  DEPENDS tools/gen/c25_translate.py tools/gen/c25_pc.cov
  WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
  COMMENT "Translating the master DSP program to C")
set_source_files_properties(gen/pc_c25.c PROPERTIES COMPILE_OPTIONS "-O1;-w")

add_executable(propcycl
    src/main.c
    ${PROPCYCL_ENGINE_SRC}
)

target_include_directories(propcycl PRIVATE include ${SDL2_INCLUDE_DIRS})
if(WIN32)
    # Windows: no opengl32 import -- src/gl_dyn.c resolves OpenGL at run time, so the
    # game can fall back to the bundled Mesa (mesa/) when there is no real driver
    target_sources(propcycl PRIVATE engine/gl_dyn.c)
    target_link_libraries(propcycl PRIVATE ${SDL2_LINK_LIBRARIES} m ZLIB::ZLIB)
else()
    target_link_libraries(propcycl PRIVATE ${SDL2_LINK_LIBRARIES} OpenGL::GL m ZLIB::ZLIB)
endif()
# Hand-written files get normal warnings
set_source_files_properties(
    src/main.c src/memory.c src/trace.c src/hw_regs.c
    src/renderer_2d.c src/renderer_3d.c src/pc_palette.c
    src/input.c src/pedal_enc.c src/pedal_enc_core.c src/pedal_enc_serial.c
    src/rom_loader.c src/game_logic.c src/flight_rec.c
    src/level_select.c
    src/rom_decode.c
    src/audio_hle.c
    src/mcu_sound.c
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

# ---------------------------------------------------------------------------
# Development tools: gates, viewers and test oracles. Not needed to play; the
# public source tree (tools/export_public.sh) leaves their sources out, so they
# are built only when those sources are present.
# ---------------------------------------------------------------------------
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/tools/master_gate.c)
  set(PROPCYCL_DEV_TOOLS_DEFAULT ON)
else()
  set(PROPCYCL_DEV_TOOLS_DEFAULT OFF)
endif()
option(PROPCYCL_DEV_TOOLS "Build the gates, viewers and test oracles" ${PROPCYCL_DEV_TOOLS_DEFAULT})
if(PROPCYCL_DEV_TOOLS)

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

# External-pedal (exercise bike / Peloton) logic, tested with synthetic input:
# quadrature, cadence estimator, the Peloton serial frames, text lines.
add_executable(pedal_enc_test tools/pedal_enc_test.c src/pedal_enc_core.c)
target_include_directories(pedal_enc_test PRIVATE include)
target_link_libraries(pedal_enc_test PRIVATE m)
target_compile_options(pedal_enc_test PRIVATE -Wall -Wextra)

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
    engine/c352.c
)
target_include_directories(c352_replay PRIVATE include)
target_compile_options(c352_replay PRIVATE -Wall -Wno-unused-variable)
target_link_libraries(c352_replay PRIVATE m)

# Master DSP (TMS320C25 "C71") port and its gate against MAME polygon-RAM
# dumps / the MiSTer project's Python interpreter.
add_executable(master_gate tools/master_gate.c ${NAMCO22_ENGINE_C25_SRC} ${NAMCO22_C25_ORACLE_SRC})
target_include_directories(master_gate PRIVATE include)
target_compile_options(master_gate PRIVATE -Wall)
add_executable(master_seq_gate tools/master_seq_gate.c ${NAMCO22_ENGINE_C25_SRC} ${NAMCO22_C25_ORACLE_SRC})
target_include_directories(master_seq_gate PRIVATE include)
target_compile_options(master_seq_gate PRIVATE -Wall)

# TEST ORACLE: the game with the ORIGINAL sound program fetched and decoded from
# memory in place of gen/snd_driver.c. Never shipped. See tools/sndoracle/.
set(PROPCYCL_ORACLE_SRC ${PROPCYCL_ENGINE_SRC})
list(REMOVE_ITEM PROPCYCL_ORACLE_SRC gen/snd_driver.c src/snd/snd_engine.c)
# THE MASTER-DSP ORACLE build: the game with the interpreter linked beside the
# translation (PROPCYCL_C25=oracle selects it) -- tools/c25_gate.sh runs both.
add_executable(propcycl_oracle src/main.c ${PROPCYCL_ENGINE_SRC} ${NAMCO22_C25_ORACLE_SRC})
target_include_directories(propcycl_oracle PRIVATE include ${SDL2_INCLUDE_DIRS})
target_link_libraries(propcycl_oracle PRIVATE ${SDL2_LINK_LIBRARIES} OpenGL::GL m ZLIB::ZLIB)
target_compile_definitions(propcycl_oracle PRIVATE PROPCYCL_ORACLE)

add_executable(propcycl_sndoracle src/main.c ${PROPCYCL_ORACLE_SRC} tools/sndoracle/m37710.c tools/sndoracle/sound_oracle.c)
target_include_directories(propcycl_sndoracle PRIVATE include ${SDL2_INCLUDE_DIRS})
target_link_libraries(propcycl_sndoracle PRIVATE ${SDL2_LINK_LIBRARIES} OpenGL::GL m ZLIB::ZLIB)

endif()  # PROPCYCL_DEV_TOOLS
