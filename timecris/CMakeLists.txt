cmake_minimum_required(VERSION 3.16)
project(timecris C)
# Time Crisis (SUPER System 22) on the shared Super 22 host (../engine/ss22_*): the lifted 68K (gen/tc_lifted.c, tools/namco22/lift.py) and ONE table
# (src/tc_game.c). Dirt Dash's build is the template (../dirtdash/CMakeLists.txt). HARD RULE 3: no copies of the engine.
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

# gen/tc_lifted_NN.c + gen/tc_lifted_tab.c:
#   python3 ../tools/namco22/lift.py decompiled/pcode.txt gen tc timecris_main.bin && python3 ../tools/namco22/lift_split.py gen/tc_lifted.c 16
# 58 MB of goto-heavy C in ONE file needed more than 8 GB of gcc; the parts (shared tool lift_split.py) need about 1 GB each. -O1 is plenty.
file(GLOB TC_LIFTED CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/gen/tc_lifted_*.c)
set_source_files_properties(${TC_LIFTED} PROPERTIES COMPILE_OPTIONS "-O1;-w")

# The MASTER DSP program (block at ROM 0xABA8C), translated from the ROM by ../tools/gen/c25_translate.py --game tc. tools/gen/c25.cov is EMPTY until
# the oracle build has run the game: every address is then a static one and anything the oracle would need traps.
# --block: the game's program (0xABA8C) and the TEST-MODE master programs the operator menu's DSP tests upload, listed by the 68K's
# table at ROM 0xB91C4 (12 entries; GitHub #30 follow-up, 2026-10-05: without them test mode stopped the translated master at 0x400B..0x413E).
find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_custom_command(OUTPUT ${CMAKE_CURRENT_SOURCE_DIR}/gen/tc_c25.c
  COMMAND ${Python3_EXECUTABLE} ../tools/gen/c25_translate.py --game tc --roms extracted --main-rom timecris_main.bin
          --block ABA8C --block B91F4 --block B91F8 --block B9212 --block B93AE --block B9536 --block B9626 --block B9804
          --block B9CAE --block B9D3C --block BA11A --block BA580 --block BAE16
          --cov tools/gen/c25.cov --out gen/tc_c25.c --func tc_c25_exec
  DEPENDS ../tools/gen/c25_translate.py tools/gen/c25.cov ../engine/c25/c71_bios.inc
  WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
  COMMENT "Translating the master DSP program to C")
set_source_files_properties(gen/tc_c25.c PROPERTIES COMPILE_OPTIONS "-O2;-w")

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
  set(TC_GL_LIB "")
  set(TC_GL_SRC ${NAMCO22_ENGINE_DIR}/gl_dyn.c)
else()
  set(TC_GL_LIB OpenGL::GL)
  set(TC_GL_SRC "")
endif()

# The SOUND MCU program (ts1data.8k), translated to C from the ROM file by the shared tools/gen/snd_translate.py (--game tc). tools/gen/snd.cov: the
# (address, M/X, length) of every instruction the oracle build executed (addresses only, no ROM data).
add_custom_command(OUTPUT ${CMAKE_CURRENT_SOURCE_DIR}/gen/tc_snd_driver.c
  COMMAND ${Python3_EXECUTABLE} ../tools/gen/snd_translate.py tools/gen/snd.cov --game tc --roms extracted
          --out gen/tc_snd_driver.c
  DEPENDS ../tools/gen/snd_translate.py ../tools/gen/snd_rom.py ../tools/gen/snd_spin.inc ../tools/gen/m7700_decode.py tools/gen/snd.cov
  WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
  COMMENT "Translating the sound MCU program to C")
set_source_files_properties(gen/tc_snd_driver.c PROPERTIES COMPILE_OPTIONS "-O1;-w")

set(TC_CORE_SRC src/tc_game.c
    ${NAMCO22_ENGINE_LIFT_SRC} ${NAMCO22_ENGINE_C25_SRC} ${NAMCO22_ENGINE_SND_SRC}
    ${NAMCO22_ENGINE_GL_SRC} ${NAMCO22_ENGINE_SS22_SRC} ${NAMCO22_ENGINE_SS22_HOST_SRC} ${NAMCO22_ENGINE_UI_SRC} ${NAMCO22_ENGINE_ROMZIP_SRC} ${TC_GL_SRC}
    ${TC_LIFTED} gen/tc_c25.c)

# THE GAME: the 68K lifted, the master DSP and the sound MCU translated -- no interpreter anywhere (HARD RULE 1/2)
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/tools/gen/snd.cov)
add_executable(tc ${TC_CORE_SRC} gen/tc_snd_driver.c)
target_compile_options(tc PRIVATE -Wall)
target_link_libraries(tc ${SDL2_LIBRARIES} ${TC_GL_LIB} ZLIB::ZLIB m)
endif()

# DEV builds (HARD RULE 2: the DSP and the sound MCU INTERPRETED, never shipped): the window / headless run, and the instruction trace for the T3 proof
if(EXISTS ${NAMCO22_TOOLS_DIR}/sndoracle/sound_oracle.c)
add_executable(tc_oracle ${TC_CORE_SRC} ${NAMCO22_C25_ORACLE_SRC}
    ${NAMCO22_TOOLS_DIR}/sndoracle/m37710.c ${NAMCO22_TOOLS_DIR}/sndoracle/sound_oracle.c)
target_compile_options(tc_oracle PRIVATE -Wall)
target_compile_definitions(tc_oracle PRIVATE TC_ORACLE SS22_ORACLE C25_DEV_HOOKS)
target_link_libraries(tc_oracle ${SDL2_LIBRARIES} ${TC_GL_LIB} ZLIB::ZLIB m)

add_executable(tc_trace ${TC_CORE_SRC} ${NAMCO22_C25_ORACLE_SRC}
    ${NAMCO22_TOOLS_DIR}/sndoracle/m37710.c ${NAMCO22_TOOLS_DIR}/sndoracle/sound_oracle.c)
target_compile_options(tc_trace PRIVATE -Wall)
target_compile_definitions(tc_trace PRIVATE RR_TRACE TC_ORACLE SS22_ORACLE C25_DEV_HOOKS)
target_link_libraries(tc_trace ${SDL2_LIBRARIES} ${TC_GL_LIB} ZLIB::ZLIB m)
endif()
