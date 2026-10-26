cmake_minimum_required(VERSION 3.16)
project(ridgera2 C)
# Ridge Racer 2 (System 22) on the SHARED runtime: ../raverace/src (the board, its devices, the host) + ../engine, with this game's
# own lifted 68K (gen/rr_lifted.c, the rr_* ABI) and one table (src/r2_game.c).  HARD RULE 3: no copies.
if(NOT CMAKE_C_COMPILER_ID STREQUAL "GNU")
  message(FATAL_ERROR "gcc required (the lifted code uses computed labels and statement expressions)")
endif()
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_EXTENSIONS ON)
add_compile_options(-fno-strict-aliasing -fwrapv)
set(RR ${CMAKE_CURRENT_SOURCE_DIR}/../raverace)
# PORTABLE (default ON): pin glibc symbol versions so a binary built here runs on older systems -- the Steam Deck included
option(PORTABLE "Bind glibc symbols to baseline versions (runs on glibc >= 2.34)" ON)
if(PORTABLE AND NOT WIN32 AND CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
  add_compile_options(-include ${RR}/include/glibc_compat.h)
endif()
include_directories(${RR}/include gen)
include(${CMAKE_CURRENT_SOURCE_DIR}/../engine/engine.cmake)
set_source_files_properties(gen/rr_lifted.c PROPERTIES COMPILE_OPTIONS "-O1;-w")
find_package(PkgConfig REQUIRED)
pkg_check_modules(SDL2 REQUIRED sdl2)
if(WIN32)
  set(ZLIB_USE_STATIC_LIBS ON)
endif()
find_package(ZLIB REQUIRED)
set(OpenGL_GL_PREFERENCE GLVND)
find_package(OpenGL REQUIRED)

# The master DSP programs -- the C71 BIOS + the game program the 68K uploads (count word at ROM 0xCA7AA) + the seven
# MPU/DSP BOARD test programs (test mode, 0x2B30A..0x2BB70, each loaded by `lea block(pc),a1` at 0x2ACE4..0x2B24C) -- TRANSLATED to C by the shared ../tools/gen/c25_translate.py from the ROM files and tools/gen/c25.cov
# (addresses + block, no ROM data: regenerate with r2_oracle RR_C25=oracle C71_COV=... over the scenarios, then --attribute).
find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_custom_command(OUTPUT ${CMAKE_CURRENT_SOURCE_DIR}/gen/rr_c25.c
  COMMAND ${Python3_EXECUTABLE} ../tools/gen/c25_translate.py --game rr --roms extracted --main-rom ridgera2_main.bin --block CA7AA
          --block 2B30A --block 2B36A --block 2B462 --block 2B5A2 --block 2B822 --block 2BB12 --block 2BB70
          --cov tools/gen/c25.cov --out gen/rr_c25.c --func rr_c25_exec
  WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
  DEPENDS ../tools/gen/c25_translate.py tools/gen/c25.cov ../engine/c25/c71_bios.inc
  COMMENT "translating Ridge Racer 2's master DSP program")
set_source_files_properties(gen/rr_c25.c PROPERTIES COMPILE_OPTIONS "-O2;-w")

# The sound program -- the C74 BIOS (c74.bin) + rrs1data.6r, the BIOS copying the data ROM's code from offset 0x4E to MCU RAM 0xBC00
# (Cyber Commando's map; measured: all 138 executed RAM instructions decode to their covered length only at that offset, next best 62)
# -- TRANSLATED by Rave Racer's tools/gen/snd_translate.py from tools/gen/snd.cov (r2_oracle RR_SNDCOV=... over attract, a coined race,
# the six tools/scenarios and --sndsweep 4:{0,5,6,7}:0:31:0xFF; work/sndcov/run.sh). No executed code below 0xBC00: the 68K uploads
# no sound code, SND_UPLOAD=none.
add_custom_command(OUTPUT ${CMAKE_CURRENT_SOURCE_DIR}/gen/snd_driver.c
  COMMAND ${CMAKE_COMMAND} -E env SND_DATA=rrs1data.6r SND_RAMCODE=BC00:C000:BBB2 SND_UPLOAD=none
          ${Python3_EXECUTABLE} ${RR}/tools/gen/snd_translate.py tools/gen/snd.cov
  WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
  DEPENDS ${RR}/tools/gen/snd_translate.py ${RR}/tools/gen/m7700_decode.py ${RR}/tools/gen/snd_rom.py ../tools/gen/snd_spin.inc ../tools/gen/snd_dispatch.py tools/gen/snd.cov
  COMMENT "translating Ridge Racer 2's sound program")
set_source_files_properties(gen/snd_driver.c PROPERTIES COMPILE_OPTIONS "-O1;-w")

# Windows (MinGW-w64): Rave Racer's block, as acedriver/CMakeLists.txt
if(WIN32)
  add_compile_options(-include ${RR}/include/win_compat.h)
  add_link_options(-mwindows -static-libgcc -static)
  set(SDL2_LIBRARIES ${SDL2_STATIC_LDFLAGS})
  set(R2_GL_LIB "")
  set(R2_GL_SRC ${NAMCO22_ENGINE_DIR}/gl_dyn.c)
  set(R2_NET_LIB ws2_32)
else()
  set(R2_GL_LIB OpenGL::GL)
  set(R2_GL_SRC "")
  set(R2_NET_LIB "")
endif()

set(R2_RD ${RR}/src/rd/rd_core.c src/r2_rd_stub.c)
set(R2_CORE ${RR}/src/rr_romzip.c ${NAMCO22_ENGINE_DIR}/romzip.c ${RR}/src/rr_main.c ${NAMCO22_ENGINE_LIFT_SRC}
    ${RR}/src/rr_mem.c ${RR}/src/rr_hw.c ${RR}/src/rr_env.c src/r2_game.c ${RR}/src/rr_link.c ${RR}/src/rr_net.c ${NAMCO22_ENGINE_NET_SRC} ${RR}/src/rr_dsp.c
    ${NAMCO22_ENGINE_C25_SRC} gen/rr_c25.c ${RR}/src/rr_scene.c ${RR}/src/rr_gl.c ${NAMCO22_ENGINE_GL_SRC}
    ${RR}/src/rr_host.c ${RR}/src/rr_ui.c ${RR}/src/rr_input.c ${NAMCO22_ENGINE_DIR}/twin_stick.c ${NAMCO22_ENGINE_FFB_SRC} ${RR}/src/rr_audio.c ${NAMCO22_ENGINE_SND_SRC}
    ${RR}/src/snd/snd_board.c gen/rr_lifted.c ${R2_RD})

# THE GAME: the lifted 68K, the translated master DSP and sound programs, the shared engine. No interpreter of any kind
# (HARD RULE 1/2: nm shows no c25_interp_exec, no m37710 fetch/decode).
add_executable(r2 ${RR}/src/rr_win.c ${R2_CORE} ${R2_GL_SRC} gen/snd_driver.c)
target_include_directories(r2 PRIVATE ${SDL2_INCLUDE_DIRS} ${RR}/src/snd)
target_link_libraries(r2 ${SDL2_LIBRARIES} ${R2_GL_LIB} ZLIB::ZLIB ${R2_NET_LIB} m)

# Development tools: only when their sources are present -- the public tree has just the game.
if(EXISTS ${NAMCO22_TOOLS_DIR}/c25oracle/c25_interp.c AND EXISTS ${RR}/tools/sndoracle/sound_oracle.c)
# DEV, translated sound: the DSP interpreter (RR_C25=oracle) and the software renderer oracle beside the translations -- the sound
# gate's translated side.
add_executable(r2_dev ${NAMCO22_C25_ORACLE_SRC} ${R2_CORE} ${RR}/src/rr_video.c gen/snd_driver.c)
target_include_directories(r2_dev PRIVATE ${SDL2_INCLUDE_DIRS} ${RR}/src/snd)
target_compile_definitions(r2_dev PRIVATE RR_ORACLE C25_DEV_HOOKS)
target_link_libraries(r2_dev ${SDL2_LIBRARIES} OpenGL::GL ZLIB::ZLIB m)

# DEV: the all-oracle build -- the sound MCU INTERPRETED, the master DSP interpreter (run with RR_C25=oracle), the instruction trace
# (RR_TRACE_OFF=1 or it stops after 2M instructions). Never shipped (HARD RULE 2).
add_executable(r2_oracle ${NAMCO22_C25_ORACLE_SRC} ${R2_CORE} ${RR}/src/rr_video.c ${RR}/tools/sndoracle/sound_oracle.c ${RR}/tools/sndoracle/m37710.c)
target_include_directories(r2_oracle PRIVATE ${SDL2_INCLUDE_DIRS} ${RR}/src/snd ${RR}/tools/sndoracle)
target_compile_definitions(r2_oracle PRIVATE RR_TRACE RR_ORACLE C25_DEV_HOOKS)
target_link_libraries(r2_oracle ${SDL2_LIBRARIES} OpenGL::GL ZLIB::ZLIB m)
endif()
