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
# the Super 22 layers every Super 22 game shares: depth fog + mixer state, the C374 sprites, the text tilemap. Each
# game fills them from its own RAM (Prop Cycle src/pc_live2d.c, Tokyo Wars tokyowar/src/tw_video.c)
set(NAMCO22_ENGINE_SS22_SRC
    ${NAMCO22_ENGINE_DIR}/fog_hw.c
    ${NAMCO22_ENGINE_DIR}/sprite_hw.c
    ${NAMCO22_ENGINE_DIR}/text_hw.c
    ${NAMCO22_ENGINE_DIR}/ss22_gl.c
    ${NAMCO22_ENGINE_DIR}/frame_rule.c
)
# THE MENU AND THE DISPLAY CHOICES (Nuklear): a settings file, the display modes (widescreen, window mode/size, resolution,
# aspect, scaling) and the menu bar every game's Escape menu is. Its own list because eng_ui.c defines Nuklear's
# implementation: a binary links it OR Prop Cycle's src/ui_menu.c, never both.
set(NAMCO22_ENGINE_UI_SRC
    ${NAMCO22_ENGINE_DIR}/eng_cfg.c
    ${NAMCO22_ENGINE_DIR}/eng_display.c
    ${NAMCO22_ENGINE_DIR}/eng_ui.c
)
# first-run ROM setup from MAME's zips (table-driven: the game supplies its chip list) and Windows start-up; needs zlib
set(NAMCO22_ENGINE_ROMZIP_SRC
    ${NAMCO22_ENGINE_DIR}/romzip.c
    ${NAMCO22_ENGINE_DIR}/win_startup.c
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
# the 68K host runtime of the LIFTED games (register file, shadow return stack, interrupt entry,
# trace hook); the memory map, devices and scheduler stay with each game
set(NAMCO22_ENGINE_LIFT_SRC
    ${NAMCO22_ENGINE_DIR}/lift_cpu.c
)
set(NAMCO22_ENGINE_SND_SRC
    ${NAMCO22_ENGINE_DIR}/c352.c
    ${NAMCO22_ENGINE_DIR}/audio_out.c
    ${NAMCO22_ENGINE_DIR}/snd/m377_periph.c
)
include_directories(${NAMCO22_ENGINE_DIR} ${NAMCO22_ENGINE_DIR}/snd ${NAMCO22_ENGINE_DIR}/c25 ${NAMCO22_TOOLS_DIR}/c25oracle)
set_source_files_properties(${NAMCO22_ENGINE_GL_SRC} ${NAMCO22_ENGINE_SS22_SRC} ${NAMCO22_ENGINE_UI_SRC} ${NAMCO22_ENGINE_ROMZIP_SRC} ${NAMCO22_ENGINE_SND_SRC} ${NAMCO22_ENGINE_C25_SRC} ${NAMCO22_ENGINE_LIFT_SRC}
    PROPERTIES COMPILE_FLAGS "-Wall -Wno-unused-variable -Wno-unused-function -Wno-misleading-indentation")
