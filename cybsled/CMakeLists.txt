# Cyber Sled (Namco System 21) -- the top build (cybsled/DESIGN.md: module A owns it).  cmake -S cybsled -B cybsled/build_a
#
# THE TWO 68000s: each lifted program becomes ONE relocatable unit -- gen/c{m,s}_lifted.c + its own copy of the shared runtime
# (engine/lift_cpu.c, engine/lift_env.c) + src/cpu68/s21_cpu_glue.c -- partially linked (ld -r) with every symbol but the
# s21_cpuN_* API made local (objcopy --keep-global-symbols), so the two register files and shadow stacks stay apart with the
# shared runtime used unchanged. Variants: plain (the game) and RR_TRACE (the trace oracle of tools/t3_run.sh).
cmake_minimum_required(VERSION 3.16)
project(cybsled C)
if(EXISTS ${CMAKE_CURRENT_LIST_DIR}/src/snd/s21_ym.cpp AND NOT WIN32)   # DEV only: the ymfm oracle (src/snd, opm_gate / S21_OPM_ORACLE); the game is all C
  enable_language(CXX)
endif()
if(NOT CMAKE_C_COMPILER_ID STREQUAL "GNU")
  message(FATAL_ERROR "gcc required (the lifted code uses computed labels and statement expressions)")
endif()
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()
set(CMAKE_C_STANDARD 11)
set(ENG ${CMAKE_CURRENT_LIST_DIR}/../engine)
set(CS ${CMAKE_CURRENT_LIST_DIR})
add_compile_options(-fno-strict-aliasing -fwrapv)
# PORTABLE (default ON): bind glibc symbols to baseline versions so a binary built here runs on glibc >= 2.34 (the Steam Deck included);
# the same ../include/glibc_compat.h every game uses
option(PORTABLE "Bind glibc symbols to baseline versions (runs on glibc >= 2.34)" ON)
if(PORTABLE AND NOT WIN32 AND CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
  add_compile_options(-include ${CS}/../include/glibc_compat.h)
endif()
# Windows (MinGW-w64, ../build-windows.sh): the few POSIX calls mapped in ../include/win_compat.h (force-included everywhere, the lifted
# units too), no console window, everything linked statically; OpenGL resolved at run time (engine/gl_dyn.c) so the bundled Mesa can
# stand in. The two 68000 coroutines use Win32 fibers there (src/cpu68/s21_cpu_glue.c).
if(WIN32)
  add_compile_options(-include ${CS}/../include/win_compat.h)
  add_link_options(-mwindows -static-libgcc -static-libstdc++ -static)
endif()

# the partial link uses the toolchain's own binutils (x86_64-w64-mingw32-ld / -objcopy when cross-building for Windows)
set(S21_LD ld)
set(S21_OBJCOPY objcopy)
if(CMAKE_LINKER)
  set(S21_LD ${CMAKE_LINKER})
endif()
if(CMAKE_OBJCOPY)
  set(S21_OBJCOPY ${CMAKE_OBJCOPY})
endif()
function(s21_cpu_unit N base variant)
  set(lib s21u_${variant}_${N})
  add_library(${lib} OBJECT ${CS}/gen/${base}_lifted.c ${ENG}/lift_cpu.c ${ENG}/lift_env.c ${CS}/src/cpu68/s21_cpu_glue.c)
  target_include_directories(${lib} PRIVATE ${ENG} ${CS}/gen ${CS}/src/cpu68)
  target_compile_definitions(${lib} PRIVATE S21_CPU=${N})
  if(WIN32)   # MinGW reaches extern data through .refptr stubs, which the partial link + symbol localising below leaves unrelocated
    target_compile_options(${lib} PRIVATE -mcmodel=small)   # (CyberSled.exe read address -1); small = direct RIP-relative access
  endif()
  if(variant STREQUAL "trace")
    target_compile_definitions(${lib} PRIVATE RR_TRACE)
  endif()
  set_source_files_properties(${CS}/gen/${base}_lifted.c TARGET_DIRECTORY ${lib} PROPERTIES COMPILE_OPTIONS "-O1;-w")
  set_source_files_properties(${ENG}/lift_cpu.c ${ENG}/lift_env.c TARGET_DIRECTORY ${lib} PROPERTIES COMPILE_OPTIONS "-Dgetenv=s21_unit_getenv")
  set(keep ${CMAKE_CURRENT_BINARY_DIR}/keep_${N}.txt)
  set(syms "")
  foreach(s reset run halted reg sr set_frame traps env_init env_active hooks)
    string(APPEND syms "s21_cpu${N}_${s}\n")
  endforeach()
  file(WRITE ${keep} "${syms}")
  set(out ${CMAKE_CURRENT_BINARY_DIR}/${lib}.o)
  add_custom_command(OUTPUT ${out}
    COMMAND ${S21_LD} -r -o ${out}.raw $<TARGET_OBJECTS:${lib}>
    COMMAND ${S21_OBJCOPY} --keep-global-symbols=${keep} ${out}.raw ${out}
    COMMAND ${CMAKE_COMMAND} -E remove ${out}.raw
    DEPENDS ${lib} $<TARGET_OBJECTS:${lib}> ${keep}
    COMMAND_EXPAND_LISTS VERBATIM)
  add_custom_target(${lib}_unit DEPENDS ${out})
  set(${lib}_O ${out} PARENT_SCOPE)
endfunction()

s21_cpu_unit(0 cm game)
s21_cpu_unit(1 cs game)
# the trace oracle units (dev, tools/t3_run.sh): only in the development tree -- the public tree builds just the game
set(S21_DEV_TRACE OFF)
if(EXISTS ${CS}/tools/t3_run.sh)
  set(S21_DEV_TRACE ON)
  s21_cpu_unit(0 cm trace)
  s21_cpu_unit(1 cs trace)
endif()

set(BOARD ${CS}/src/board/s21_board.c ${CS}/src/board/s21_stubs.c ${CS}/src/board/s21_debug.c ${CS}/src/link/s21_link.c ${CS}/src/link/s21_net.c ${ENG}/c139.c ${ENG}/net.c ${ENG}/net_host.c)   # link: the shared C139 + NMN2 client
set(MODINC ${CS}/src/dsp ${CS}/src/snd ${CS}/src/io ${CS}/src/link)     # the modules' headers are the board's interface to them

# THE GAME: both lifted 68000s, the board, the window (engine/ss22_host.c with the shared Nuklear menu and display modes), the picture
# (src/video, module C) and -- when present -- the DSP (B), sound and I/O (D) modules, which replace the weak stubs.
if(WIN32)                                              # Windows: SDL2 linked statically (pkg-config --static), like every game
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(SDL2 REQUIRED sdl2)
  set(SDL2_LIBRARIES ${SDL2_STATIC_LDFLAGS})
  set(ZLIB_USE_STATIC_LIBS ON)
else()
  find_package(SDL2 REQUIRED)
endif()
set(OpenGL_GL_PREFERENCE GLVND)
find_package(OpenGL REQUIRED)
find_package(ZLIB REQUIRED)                            # the first-run ROM unpacker (engine/romzip.c)
if(EXISTS ${CS}/src/video/s21_video.c)
  add_subdirectory(${CS}/src/video ${CMAKE_CURRENT_BINARY_DIR}/video)
endif()
set(HOST_ENG ${ENG}/ss22_host.c ${ENG}/eng_ffb.c ${ENG}/render_target.c ${ENG}/audio_out.c ${ENG}/eng_cfg.c ${ENG}/eng_display.c ${ENG}/eng_ui.c
             ${ENG}/quad_gl.c ${ENG}/tex_bake.c ${ENG}/geo_hw.c ${ENG}/eng.c)    # the shared GL rasteriser: the 3D of the GL picture (src/video/s21_video_gl.c)
set_source_files_properties(${HOST_ENG} PROPERTIES COMPILE_OPTIONS "-Wno-unused-variable;-Wno-unused-function;-Wno-misleading-indentation")
set(HOST_ROMZIP ${CS}/src/host/cs21_roms.c ${ENG}/romzip.c ${ENG}/win_startup.c)  # first run: the ROMs out of MAME's zips; Windows start-up (own folder, log file)
if(WIN32)
  set(CS21_GL_SRC ${ENG}/gl_dyn.c)                     # OpenGL resolved at run time (the bundled Mesa can stand in)
  set(CS21_GL_LIB "")
  set(CS21_SYS_LIB ws2_32)                             # Winsock: the C139 link's UDP transport, online play
else()
  set(CS21_GL_SRC "")
  set(CS21_GL_LIB OpenGL::GL)
  set(CS21_SYS_LIB "")
endif()
add_executable(cs21 ${HOST_ROMZIP} ${CS21_GL_SRC} ${CS}/src/host/cs21_main.c ${CS}/src/host/s21_glutil.c ${CS}/src/host/s21_input.c ${CS}/src/host/s21_debug_page.c ${ENG}/eng_net_ui.c ${HOST_ENG} ${BOARD}
                    ${s21u_game_0_O} ${s21u_game_1_O})
set_source_files_properties(${s21u_game_0_O} ${s21u_game_1_O} PROPERTIES EXTERNAL_OBJECT TRUE GENERATED TRUE)
add_dependencies(cs21 s21u_game_0_unit s21u_game_1_unit)
target_compile_definitions(cs21 PRIVATE S21_WITH_HOST)
target_include_directories(cs21 PRIVATE ${CS}/src/board ${CS}/src/cpu68 ${CS}/src/host ${ENG} ${MODINC} ${SDL2_INCLUDE_DIRS})
target_link_libraries(cs21 s21_video ${SDL2_LIBRARIES} ${CS21_GL_LIB} ZLIB::ZLIB ${CS21_SYS_LIB} m)
foreach(m dsp snd io)                                  # the other builders' modules: a library target s21_<m> in src/<m>/CMakeLists.txt
  if(EXISTS ${CS}/src/${m}/CMakeLists.txt AND NOT TARGET s21_${m})
    add_subdirectory(${CS}/src/${m} ${CMAKE_CURRENT_BINARY_DIR}/${m})
  endif()
  if(TARGET s21_${m}_dev AND NOT m MATCHES "^(dsp|snd|io)$")  # DEV until the module's program is translated (HARD RULE 1): its interpreter.
    target_link_libraries(cs21 s21_${m}_dev)           # dsp: translated (module E, gen/cs_c67_*.c), snd: the 6809 translated (module J,
                                                       # gen/cs_6809.c), io: the C68 translated (module K, gen/cs_c68.c)
                                                       # -- the game libraries, no interpreter
  elseif(TARGET s21_${m})
    target_link_libraries(cs21 s21_${m})
  else()
    string(TOUPPER ${m} M)
    target_compile_definitions(cs21 PRIVATE S21_STUB_${M})
    message(STATUS "cs21: no ${m} module -- stand-in")
  endif()
endforeach()

# the trace oracle (dev): both lifted programs built with RR_TRACE + engine/lift_env.c, no window (tools/t3_run.sh)
if(S21_DEV_TRACE)
add_executable(cs21_trace ${CS}/src/host/cs21_main.c ${BOARD} ${s21u_trace_0_O} ${s21u_trace_1_O})
set_source_files_properties(${s21u_trace_0_O} ${s21u_trace_1_O} PROPERTIES EXTERNAL_OBJECT TRUE GENERATED TRUE)
add_dependencies(cs21_trace s21u_trace_0_unit s21u_trace_1_unit)
target_include_directories(cs21_trace PRIVATE ${CS}/src/board ${CS}/src/cpu68 ${ENG} ${MODINC})
target_compile_definitions(cs21_trace PRIVATE S21_STUB_DSP S21_STUB_SND S21_STUB_IO S21_STUB_VIDEO)
target_include_directories(cs21_trace PRIVATE ${SDL2_INCLUDE_DIRS})
target_link_libraries(cs21_trace ${SDL2_LIBRARIES} ${CS21_SYS_LIB})   # engine/net.c: SDL_GetTicks / SDL_Delay (the link's online client)
endif()
