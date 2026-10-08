# cybsled/src/dsp -- the System 21 DSP board (module B). Standalone:  cmake -S src/dsp -B build_dsp && make -C build_dsp
# Included by cybsled/CMakeLists.txt: it exports the library `s21_dsp` (game code) and, in dev, `s21_dsp_dev` + the gate.
cmake_minimum_required(VERSION 3.10)
project(s21_dsp C)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()
set(CMAKE_C_STANDARD 11)
get_filename_component(S21_REPO "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)
get_filename_component(S21_CS "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
set(S21_ENGINE_C25 "${S21_REPO}/engine/c25")
set(S21_ORACLE "${S21_REPO}/tools/c25oracle")

# engine/c25 carries what the C67 programs need (TBLR/TBLW under RPT, the OV flag with BV/BNV, SST/SST1 direct = page 0), 2026-10-06.

# THE C67 PROGRAMS, TRANSLATED (HARD RULE 1, module E): the shared ../tools/gen/c25_translate.py --game cs turns c67.bin (the
# BIOS, program 0) + the data ROM's two uploaded blocks (master program: count word at data 0x0, slave program: at 0x1E16; both
# land at program 0x8000) into one C function per chip, from tools/gen/c67_{master,slave}.cov (addresses + program tag, no ROM
# data; regrow: PLAN.md "Module E"). Untranslated code TRAPS. gen/ is ROM-derived: never committed.
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(S21_C67_GEN "")
foreach(chip master slave)
  if(chip STREQUAL "master")
    set(blk 0)
  else()
    set(blk 1E16)
  endif()
  set(out ${S21_CS}/gen/cs_c67_${chip}.c)
  add_custom_command(OUTPUT ${out}
    COMMAND ${Python3_EXECUTABLE} ${S21_REPO}/tools/gen/c25_translate.py --game cs --roms extracted --block ${blk}
            --cov tools/gen/c67_${chip}.cov --out gen/cs_c67_${chip}.c --func cs_c67_${chip}
    WORKING_DIRECTORY ${S21_CS}
    DEPENDS ${S21_REPO}/tools/gen/c25_translate.py ${S21_CS}/tools/gen/c67_${chip}.cov ${S21_CS}/extracted/c67.bin
            ${S21_CS}/extracted/cy1-data-u.3a ${S21_CS}/extracted/cy1-data-l.1a
    COMMENT "translating the C67 ${chip} DSP program")
  set_source_files_properties(${out} PROPERTIES COMPILE_OPTIONS "-O2;-w")
  list(APPEND S21_C67_GEN ${out})
endforeach()

# the game library: the board glue + the C67 bus on the engine's C25 core + the translated programs. No interpreter.
add_library(s21_dsp STATIC s21_dsp.c ${S21_ENGINE_C25}/c25_core.c ${S21_C67_GEN})
target_include_directories(s21_dsp PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} ${S21_ENGINE_C25})
target_compile_options(s21_dsp PRIVATE -Wall -fno-strict-aliasing -fwrapv)

# DEV: the same with the interpreter oracle (tools/c25oracle) and the engine's dev hooks: s21_dsp_use_oracle(), s21_dsp_lockstep()
if(EXISTS "${S21_ORACLE}/c25_interp.c")
  add_library(s21_dsp_dev STATIC s21_dsp.c ${S21_ENGINE_C25}/c25_core.c ${S21_ORACLE}/c25_interp.c ${S21_C67_GEN})
  target_include_directories(s21_dsp_dev PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} ${S21_ENGINE_C25} ${S21_ORACLE})
  target_compile_definitions(s21_dsp_dev PUBLIC C25_DEV_HOOKS)
  target_compile_options(s21_dsp_dev PRIVATE -Wall -fno-strict-aliasing -fwrapv)
  add_executable(s21_dsp_gate s21_dsp_gate.c)
  target_link_libraries(s21_dsp_gate s21_dsp_dev)
  # the GAME library driven by the same replay: the shipped path's numbers (no hooks, c71_run's spin fast-forward compiled in)
  add_executable(s21_dsp_gate_game s21_dsp_gate.c)
  target_compile_definitions(s21_dsp_gate_game PRIVATE S21_GATE_GAME)
  target_include_directories(s21_dsp_gate_game PRIVATE ${S21_ENGINE_C25})
  target_link_libraries(s21_dsp_gate_game s21_dsp)
endif()
