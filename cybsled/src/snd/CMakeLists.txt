# Module D (cybsled/DESIGN.md): the sound board and the C68 I/O MCU. DEV build:
#   cmake -S cybsled/src/snd -B cybsled/build_snd && cmake --build cybsled/build_snd
# The YM2151 is OUR OWN model (opm.c, module F). YMFM_DIR: ymfm, compiled from the REFERENCE tree (not copied in), is the
# DEV ORACLE only: opm_gate, and -DS21_OPM_ORACLE=ON (module D's library on ymfm instead, for A/B). The game never links it.
cmake_minimum_required(VERSION 3.16)
project(s21_snd C)
set(CMAKE_C_STANDARD 11)
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/s21_ym.cpp AND NOT WIN32)     # DEV only (the ymfm oracle): the public tree has no C++
  enable_language(CXX)
  set(CMAKE_CXX_STANDARD 17)
endif()
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()
set(YMFM_DIR "/storage01/code/c_things/reference/mame_s21/3rdparty/ymfm/src" CACHE PATH "ymfm source directory")

option(S21_OPM_ORACLE "DEV: build module D's library on the ymfm oracle instead of our YM2151" OFF)
if(S21_OPM_ORACLE)
  set(S21_YM_SRC s21_ym.cpp ${YMFM_DIR}/ymfm_opm.cpp)
else()
  set(S21_YM_SRC s21_ym_opm.c opm.c)
endif()

# THE 6809 PROGRAM, TRANSLATED (HARD RULE 1, module J of cybsled/PLAN.md): the shared ../tools/gen/m6809_translate.py turns
# cy1-snd0.8j into gen/cs_6809.c, from tools/gen/cs_6809.cov (executed addresses only, no ROM data; regrow: PLAN.md module J)
# plus a static walk from the vectors. Untranslated code TRAPS. gen/ is ROM-derived: never committed.
get_filename_component(S21_SND_CS "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
get_filename_component(S21_SND_REPO "${S21_SND_CS}/.." ABSOLUTE)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(S21_6809_GEN ${S21_SND_CS}/gen/cs_6809.c)
add_custom_command(OUTPUT ${S21_6809_GEN}
  COMMAND ${Python3_EXECUTABLE} ${S21_SND_REPO}/tools/gen/m6809_translate.py --game cs --roms extracted
          --cov tools/gen/cs_6809.cov --out gen/cs_6809.c
  WORKING_DIRECTORY ${S21_SND_CS}
  DEPENDS ${S21_SND_REPO}/tools/gen/m6809_translate.py ${S21_SND_REPO}/tools/gen/m6809_decode.py
          ${S21_SND_CS}/tools/gen/cs_6809.cov ${S21_SND_CS}/extracted/cy1-snd0.8j
  COMMENT "translating the 6809 sound program")
set_source_files_properties(${S21_6809_GEN} PROPERTIES COMPILE_OPTIONS "-O2;-w")

# the game library: the board + the translated program. No interpreter (nm: no m6809_step).
add_library(s21_snd STATIC s21_c140.c s21_snd.c ${S21_YM_SRC} ${S21_6809_GEN})
# DEV: the same + the interpreter (m6809.c, the oracle and this library's default; S21_SND_CPU=xlat runs the translation).
# The interpreters, gates and sweeps exist only in the development tree: the public tree builds just the game libraries.
set(S21_SND_LIBS s21_snd)
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/m6809.c)
  add_library(s21_snd_dev STATIC m6809.c s21_c140.c s21_snd.c ${S21_YM_SRC} ${S21_6809_GEN})
  target_compile_definitions(s21_snd_dev PRIVATE S21_SND_DEV)
  list(APPEND S21_SND_LIBS s21_snd_dev)
endif()
foreach(l ${S21_SND_LIBS})
  target_include_directories(${l} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
  if(S21_OPM_ORACLE)
    target_include_directories(${l} PRIVATE ${YMFM_DIR})
  endif()
  target_link_libraries(${l} s21_sched)   # the board timeline (../io/s21_sched.c); each executable picks its C68 library (module K)
  target_compile_options(${l} PRIVATE $<$<COMPILE_LANGUAGE:C>:-Wall -Wextra -Wno-misleading-indentation>)
endforeach()

# THE C68 I/O MCU (module K, PLAN.md): c68.bin TRANSLATED at build time by the shared ../tools/gen/m740_translate.py into
# gen/cs_c68.c (one case per instruction, executing ../io/m740_sem.h), from tools/gen/cs_c68.cov (addresses + lengths, no ROM
# data) and a static walk. s21_io = the game library (no interpreter); s21_io_dev adds the oracle m37450.c (S21_C68=xlat|oracle|lockstep).
find_package(Python3 REQUIRED COMPONENTS Interpreter)
get_filename_component(S21_CS_ "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
set(S21_C68_GEN ${S21_CS_}/gen/cs_c68.c)
add_custom_command(OUTPUT ${S21_C68_GEN}
  COMMAND ${Python3_EXECUTABLE} ${S21_CS_}/../tools/gen/m740_translate.py --rom extracted/c68.bin --base 0x8000
          --cov tools/gen/cs_c68.cov --out gen/cs_c68.c --prefix cs_c68
  WORKING_DIRECTORY ${S21_CS_}
  DEPENDS ${S21_CS_}/../tools/gen/m740_translate.py ${S21_CS_}/../tools/gen/m740_decode.py ${S21_CS_}/tools/gen/cs_c68.cov
          ${S21_CS_}/extracted/c68.bin
  COMMENT "translating the C68 I/O MCU program (c68.bin)")
set_source_files_properties(${S21_C68_GEN} PROPERTIES COMPILE_OPTIONS "-O2;-w")
add_library(s21_sched STATIC ../io/s21_sched.c)
target_include_directories(s21_sched PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../io)
add_library(s21_io STATIC ../io/s21_io.c ${S21_C68_GEN})
target_include_directories(s21_io PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../io)
target_link_libraries(s21_io s21_sched)
target_compile_options(s21_io PRIVATE -Wall -Wextra)
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/../io/m37450.c AND TARGET s21_snd_dev AND EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/s21_sndgate.c)
add_library(s21_io_dev STATIC ../io/m37450.c ../io/s21_io.c ${S21_C68_GEN})
target_include_directories(s21_io_dev PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../io)
target_compile_definitions(s21_io_dev PRIVATE S21_IO_DEV)
target_link_libraries(s21_io_dev s21_sched)
target_compile_options(s21_io_dev PRIVATE -Wall -Wextra)

add_executable(s21_sndgate s21_sndgate.c)
target_link_libraries(s21_sndgate s21_snd_dev s21_io_dev m)   # C68: S21_C68=xlat|oracle|lockstep (module K)
# the GAME library driven by the same replay: the shipped path (translation + fast-forward), module J's gates
add_executable(s21_sndgate_game s21_sndgate.c)
target_link_libraries(s21_sndgate_game s21_snd s21_io_dev m)   # the C68 side as in s21_sndgate (one compile of gen/cs_c68.c)
# module J: every sound command the 68000 can send, on the interpreter, for the 6809 coverage (tools/gen/cs_6809.cov)
add_executable(s21_sndsweep s21_sndsweep.c)
target_link_libraries(s21_sndsweep s21_snd_dev m)
endif()

# Module F: our own YM2151 (opm.c) vs the ymfm oracle, sample by sample -- DEV only (the oracle never reaches the game)
if(EXISTS ${YMFM_DIR}/ymfm_opm.cpp AND EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/opm_gate.c AND NOT WIN32)
  add_executable(opm_gate opm_gate.c opm.c s21_ym.cpp ${YMFM_DIR}/ymfm_opm.cpp)
  target_include_directories(opm_gate PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${YMFM_DIR})
  target_compile_options(opm_gate PRIVATE $<$<COMPILE_LANGUAGE:C>:-Wall -Wextra>)
  target_link_libraries(opm_gate m)
  if(NOT CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)   # inside the game's build (cybsled/CMakeLists.txt): not in 'all'
    set_target_properties(opm_gate PROPERTIES EXCLUDE_FROM_ALL TRUE)
  endif()
endif()
