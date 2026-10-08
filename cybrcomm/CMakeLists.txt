cmake_minimum_required(VERSION 3.16)
project(cybrcomm C)
# Cyber Commando (System 22) on the SHARED runtime: ../raverace/src (the board, its devices, the host) + ../engine, with this game's
# own lifted 68K (gen/rr_lifted.c, the rr_* ABI) and one table (src/cc_game.c).  HARD RULE 3: no copies.
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

# The master DSP program -- the C71 BIOS + the block the 68K uploads (count word 0x1753 at ROM 0x18AF2C, the routine at 0x18AD82
# the boot calls first) -- TRANSLATED to C by the shared ../tools/gen/c25_translate.py from the ROM files and tools/gen/c25.cov
# (addresses + block, no ROM data: regenerate with cc_oracle RR_C25=oracle C71_COV=... over the scenarios, then --attribute).
find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_custom_command(OUTPUT ${CMAKE_CURRENT_SOURCE_DIR}/gen/rr_c25.c
  COMMAND ${Python3_EXECUTABLE} ../tools/gen/c25_translate.py --game rr --roms extracted --main-rom cybrcomm_main.bin --block 18AF2C
          --cov tools/gen/c25.cov --out gen/rr_c25.c --func rr_c25_exec
  WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
  DEPENDS ../tools/gen/c25_translate.py tools/gen/c25.cov ../engine/c25/c71_bios.inc
  COMMENT "translating Cyber Commando's master DSP program")
set_source_files_properties(gen/rr_c25.c PROPERTIES COMPILE_OPTIONS "-O2;-w")

# The sound program -- the C74 BIOS (c74.bin) + cy1data.6r, the BIOS copying the data ROM's code from offset 0x4E to MCU RAM 0xBC00
# (one page above Ace Driver's 0xBB00; measured: every executed RAM instruction decodes to its covered length only at that offset,
# 107 of 107) -- TRANSLATED by Rave Racer's tools/gen/snd_translate.py from tools/gen/snd.cov (cc_oracle RR_SNDCOV=... over attract,
# a two-coin game, MAME's played session replayed and a --sndsweep of all 32 slots). The block the 68K uploads to 0x4200 is read
# as data, never executed (none of it is in the coverage), so SND_UPLOAD=none.
add_custom_command(OUTPUT ${CMAKE_CURRENT_SOURCE_DIR}/gen/snd_driver.c
  COMMAND ${CMAKE_COMMAND} -E env SND_DATA=cy1data.6r SND_RAMCODE=BC00:C000:BBB2 SND_UPLOAD=none
          ${Python3_EXECUTABLE} ${RR}/tools/gen/snd_translate.py tools/gen/snd.cov
  WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
  DEPENDS ${RR}/tools/gen/snd_translate.py ${RR}/tools/gen/m7700_decode.py ${RR}/tools/gen/snd_rom.py ../tools/gen/snd_spin.inc tools/gen/snd.cov
  COMMENT "translating Cyber Commando's sound program")
set_source_files_properties(gen/snd_driver.c PROPERTIES COMPILE_OPTIONS "-O1;-w")

# Windows (MinGW-w64): Rave Racer's block, as acedriver/CMakeLists.txt
if(WIN32)
  add_compile_options(-include ${RR}/include/win_compat.h)
  add_link_options(-mwindows -static-libgcc -static)
  set(SDL2_LIBRARIES ${SDL2_STATIC_LDFLAGS})
  set(CC_GL_LIB "")
  set(CC_GL_SRC ${NAMCO22_ENGINE_DIR}/gl_dyn.c)
  set(CC_NET_LIB ws2_32)
else()
  set(CC_GL_LIB OpenGL::GL)
  set(CC_GL_SRC "")
  set(CC_NET_LIB "")
endif()

set(CC_RD ${RR}/src/rd/rd_core.c src/cc_rd_stub.c)
set(CC_CORE ${RR}/src/rr_romzip.c ${NAMCO22_ENGINE_DIR}/romzip.c ${RR}/src/rr_main.c ${NAMCO22_ENGINE_LIFT_SRC}
    ${RR}/src/rr_mem.c ${RR}/src/rr_hw.c ${RR}/src/rr_env.c src/cc_game.c ${RR}/src/rr_link.c ${RR}/src/rr_net.c ${NAMCO22_ENGINE_NET_SRC} ${RR}/src/rr_dsp.c
    ${NAMCO22_ENGINE_C25_SRC} gen/rr_c25.c ${RR}/src/rr_scene.c ${RR}/src/rr_gl.c ${NAMCO22_ENGINE_GL_SRC}
    ${RR}/src/rr_host.c ${RR}/src/rr_ui.c ${RR}/src/rr_input.c ${NAMCO22_ENGINE_FFB_SRC} ${RR}/src/rr_audio.c ${NAMCO22_ENGINE_SND_SRC}
    ${RR}/src/snd/snd_board.c gen/rr_lifted.c ${CC_RD})

# THE GAME: the lifted 68K, the translated master DSP and sound programs, the shared engine. No interpreter of any kind
# (HARD RULE 1/2: nm shows no c25_interp_exec, no m37710 fetch/decode).
add_executable(cc ${RR}/src/rr_win.c ${CC_CORE} ${CC_GL_SRC} gen/snd_driver.c)
target_include_directories(cc PRIVATE ${SDL2_INCLUDE_DIRS} ${RR}/src/snd)
target_link_libraries(cc ${SDL2_LIBRARIES} ${CC_GL_LIB} ZLIB::ZLIB ${CC_NET_LIB} m)

# Development tools: only when their sources are present -- the public tree has just the game.
if(EXISTS ${NAMCO22_TOOLS_DIR}/c25oracle/c25_interp.c AND EXISTS ${RR}/tools/sndoracle/sound_oracle.c)
# DEV, translated sound: the DSP interpreter (RR_C25=oracle) and the software renderer oracle beside the translations -- the sound
# gate's translated side.
add_executable(cc_dev ${NAMCO22_C25_ORACLE_SRC} ${CC_CORE} ${RR}/src/rr_video.c gen/snd_driver.c)
target_include_directories(cc_dev PRIVATE ${SDL2_INCLUDE_DIRS} ${RR}/src/snd)
target_compile_definitions(cc_dev PRIVATE RR_ORACLE C25_DEV_HOOKS)
target_link_libraries(cc_dev ${SDL2_LIBRARIES} OpenGL::GL ZLIB::ZLIB m)

# DEV: the all-oracle build -- the sound MCU INTERPRETED, the master DSP interpreter (run with RR_C25=oracle), the instruction trace
# (RR_TRACE_OFF=1 or it stops after 2M instructions). Never shipped (HARD RULE 2).
add_executable(cc_oracle ${NAMCO22_C25_ORACLE_SRC} ${CC_CORE} ${RR}/src/rr_video.c ${RR}/tools/sndoracle/sound_oracle.c ${RR}/tools/sndoracle/m37710.c)
target_include_directories(cc_oracle PRIVATE ${SDL2_INCLUDE_DIRS} ${RR}/src/snd ${RR}/tools/sndoracle)
target_compile_definitions(cc_oracle PRIVATE RR_TRACE RR_ORACLE C25_DEV_HOOKS)
target_link_libraries(cc_oracle ${SDL2_LIBRARIES} OpenGL::GL ZLIB::ZLIB m)
endif()
