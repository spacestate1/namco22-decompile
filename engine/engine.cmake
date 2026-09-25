# engine/engine.cmake -- the shared Namco System 22 / Super 22 engine.
# Included by every game's CMakeLists (Prop Cycle at the root, raverace/, ...):
#   include(<path>/engine/engine.cmake)
# gives NAMCO22_ENGINE_DIR, NAMCO22_ENGINE_GL_SRC (geometry, display-list walker,
# texture bake, GL rasteriser, render target) and NAMCO22_ENGINE_SND_SRC (C352,
# the sound MCU's on-chip peripherals). A fix here is a fix for every game.
get_filename_component(NAMCO22_ENGINE_DIR ${CMAKE_CURRENT_LIST_DIR} ABSOLUTE)
set(NAMCO22_ENGINE_GL_SRC
    ${NAMCO22_ENGINE_DIR}/eng.c
    ${NAMCO22_ENGINE_DIR}/geo_hw.c
    ${NAMCO22_ENGINE_DIR}/slave_list.c
    ${NAMCO22_ENGINE_DIR}/tex_bake.c
    ${NAMCO22_ENGINE_DIR}/render_target.c
    ${NAMCO22_ENGINE_DIR}/quad_gl.c
    ${NAMCO22_ENGINE_DIR}/post_gl.c
)
# the master DSP: bus, semantics, step (the program itself is each game's
# translation, gen/<game>_c25.c, made by tools/gen/c25_translate.py)
set(NAMCO22_ENGINE_C25_SRC
    ${NAMCO22_ENGINE_DIR}/c25/c25_bus.c
    ${NAMCO22_ENGINE_DIR}/c25/c25_core.c
)
# the interpreter ORACLE the translations are gated against: dev builds only
get_filename_component(NAMCO22_TOOLS_DIR ${NAMCO22_ENGINE_DIR}/../tools ABSOLUTE)
set(NAMCO22_C25_ORACLE_SRC ${NAMCO22_TOOLS_DIR}/c25oracle/c25_interp.c)
set(NAMCO22_ENGINE_SND_SRC
    ${NAMCO22_ENGINE_DIR}/c352.c
    ${NAMCO22_ENGINE_DIR}/snd/m377_periph.c
)
include_directories(${NAMCO22_ENGINE_DIR} ${NAMCO22_ENGINE_DIR}/snd ${NAMCO22_ENGINE_DIR}/c25 ${NAMCO22_TOOLS_DIR}/c25oracle)
set_source_files_properties(${NAMCO22_ENGINE_GL_SRC} ${NAMCO22_ENGINE_SND_SRC} ${NAMCO22_ENGINE_C25_SRC}
    PROPERTIES COMPILE_FLAGS "-Wall -Wno-unused-variable -Wno-unused-function -Wno-misleading-indentation")
