cmake_minimum_required(VERSION 3.16)
project(adillor C)
# Armadillo Racing (SUPER System 22) on the shared Super 22 host (../engine/ss22_*): the lifted 68K (gen/am_lifted.c, tools/namco22/lift.py) and ONE table
# (src/am_game.c). Time Crisis / Dirt Dash's build is the template (../dirtdash/CMakeLists.txt). HARD RULE 3: no copies of the engine.
if(NOT CMAKE_C_COMPILER_ID STREQUAL "GNU")
  message(FATAL_ERROR "gcc required (the lifted code uses computed labels and statement expressions)")
endif()
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_EXTENSIONS ON)
add_compile_options(-fno-strict-aliasing -fwrapv)
option(PORTABLE "Bind glibc symbols to baseline versions (runs on glibc >= 2.34)" ON)
if(PORTABLE AND NOT WIN32 AND CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
  add_compile_options(-include ${CMAKE_CURRENT_SOURCE_DIR}/../include/glibc_compat.h)
endif()
include_directories(include gen)
include(${CMAKE_CURRENT_SOURCE_DIR}/../engine/engine.cmake)

# gen/am_lifted_NN.c + gen/am_lifted_tab.c (tools/pipeline.sh makes them):
#   python3 ../tools/namco22/lift.py decompiled/pcode.txt gen am adillor_main.bin && python3 ../tools/namco22/lift_split.py gen/am_lifted.c 16
file(GLOB AM_LIFTED CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/gen/am_lifted_*.c)
if(NOT AM_LIFTED)
  message(FATAL_ERROR "gen/am_lifted_NN.c missing (the lifted 68K program): run tools/pipeline.sh")
endif()
set_source_files_properties(${AM_LIFTED} PROPERTIES COMPILE_OPTIONS "-O1;-w")

# The MASTER DSP program (block at ROM 0x204634: word for word Dirt Dash's 0x57F00), translated by ../tools/gen/c25_translate.py --game am.
# tools/gen/c25.cov starts as Dirt Dash's coverage with the block's tag renamed (the same program); T4 re-gates it on this game.
# --block 208E8E: the TEST-MODE master program (1,332 words; the operator menu's DSP test uploads it -- without it the translated master
# stopped at 0x4519, 2026-10-08). Its coverage: oracle test-mode walks, `c25_translate.py --attribute` (as Time Crisis).
find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_custom_command(OUTPUT ${CMAKE_CURRENT_SOURCE_DIR}/gen/am_c25.c
  COMMAND ${Python3_EXECUTABLE} ../tools/gen/c25_translate.py --game am --roms extracted --block 204634 --block 208E8E --block 20C50A --block 20C50E --block 20C528 --block 20C6C4 --block 20C84C --block 20C93C --block 20CB3E
          --block 20CFE8 --block 20D076 --block 20D454 --block 20D8BA --block 20E14C
          --cov tools/gen/c25.cov --out gen/am_c25.c --func am_c25_exec
  DEPENDS ../tools/gen/c25_translate.py tools/gen/c25.cov ../engine/c25/c71_bios.inc
  WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
  COMMENT "Translating the master DSP program to C")
set_source_files_properties(gen/am_c25.c PROPERTIES COMPILE_OPTIONS "-O2;-w")

find_package(PkgConfig REQUIRED)
pkg_check_modules(SDL2 REQUIRED sdl2)
if(WIN32)
  set(ZLIB_USE_STATIC_LIBS ON)
endif()
find_package(ZLIB REQUIRED)
set(OpenGL_GL_PREFERENCE GLVND)
find_package(OpenGL REQUIRED)
include_directories(${SDL2_INCLUDE_DIRS})
# Windows (MinGW-w64): same block as Dirt Dash -- win_compat.h, no console, everything static; OpenGL resolved at run time (gl_dyn.c)
if(WIN32)
  add_compile_options(-include ${CMAKE_CURRENT_SOURCE_DIR}/../include/win_compat.h)
  add_link_options(-mwindows -static-libgcc -static)
  set(SDL2_LIBRARIES ${SDL2_STATIC_LDFLAGS} ws2_32)   # engine/ss22_out.c: the MAME network-output socket (Winsock2)
  set(AM_GL_LIB "")
  set(AM_GL_SRC ${NAMCO22_ENGINE_DIR}/gl_dyn.c)
else()
  set(AM_GL_LIB OpenGL::GL)
  set(AM_GL_SRC "")
endif()

# The SOUND MCU program (am1data.8k), translated to C from the ROM file by the shared tools/gen/snd_translate.py (--game am). tools/gen/snd.cov: the
# (address, M/X, length) of every instruction the oracle build executed (addresses only, no ROM data).
add_custom_command(OUTPUT ${CMAKE_CURRENT_SOURCE_DIR}/gen/am_snd_driver.c
  COMMAND ${Python3_EXECUTABLE} ../tools/gen/snd_translate.py tools/gen/snd.cov --game am --roms extracted
          --out gen/am_snd_driver.c
  DEPENDS ../tools/gen/snd_translate.py ../tools/gen/snd_rom.py ../tools/gen/snd_spin.inc ../tools/gen/snd_dispatch.py ../tools/gen/m7700_decode.py tools/gen/snd.cov
  WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
  COMMENT "Translating the sound MCU program to C")
set_source_files_properties(gen/am_snd_driver.c PROPERTIES COMPILE_OPTIONS "-O1;-w")

set(AM_CORE_SRC src/am_game.c
    ${NAMCO22_ENGINE_LIFT_SRC} ${NAMCO22_ENGINE_C25_SRC} ${NAMCO22_ENGINE_SND_SRC}
    ${NAMCO22_ENGINE_GL_SRC} ${NAMCO22_ENGINE_SS22_SRC} ${NAMCO22_ENGINE_SS22_HOST_SRC} ${NAMCO22_ENGINE_UI_SRC} ${NAMCO22_ENGINE_ROMZIP_SRC} ${AM_GL_SRC}
    ${AM_LIFTED} gen/am_c25.c)

# THE GAME: the 68K lifted, the master DSP and the sound MCU translated -- no interpreter anywhere (HARD RULE 1/2)
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/tools/gen/snd.cov)
add_executable(am ${AM_CORE_SRC} gen/am_snd_driver.c)
target_compile_options(am PRIVATE -Wall)
target_link_libraries(am ${SDL2_LIBRARIES} ${AM_GL_LIB} ZLIB::ZLIB m)
endif()

# DEV builds (HARD RULE 2: the DSP and the sound MCU INTERPRETED, never shipped): the window / headless run, and the instruction trace for the T3 proof
if(EXISTS ${NAMCO22_TOOLS_DIR}/sndoracle/sound_oracle.c)
add_executable(am_oracle ${AM_CORE_SRC} ${NAMCO22_C25_ORACLE_SRC}
    ${NAMCO22_TOOLS_DIR}/sndoracle/m37710.c ${NAMCO22_TOOLS_DIR}/sndoracle/sound_oracle.c)
target_compile_options(am_oracle PRIVATE -Wall)
target_compile_definitions(am_oracle PRIVATE AM_ORACLE SS22_ORACLE C25_DEV_HOOKS)
target_link_libraries(am_oracle ${SDL2_LIBRARIES} ${AM_GL_LIB} ZLIB::ZLIB m)

add_executable(am_trace ${AM_CORE_SRC} ${NAMCO22_C25_ORACLE_SRC}
    ${NAMCO22_TOOLS_DIR}/sndoracle/m37710.c ${NAMCO22_TOOLS_DIR}/sndoracle/sound_oracle.c)
target_compile_options(am_trace PRIVATE -Wall)
target_compile_definitions(am_trace PRIVATE RR_TRACE AM_ORACLE SS22_ORACLE C25_DEV_HOOKS)
target_link_libraries(am_trace ${SDL2_LIBRARIES} ${AM_GL_LIB} ZLIB::ZLIB m)
endif()
