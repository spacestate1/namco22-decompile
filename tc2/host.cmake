# host.cmake -- THE SYSTEM 23 HOST, shared by every System 23 / Evolution 2 game (HARD RULE 3). A game's CMakeLists includes this and adds
# its own table (src/tc2_game.c here, ../crszone/src/csz_game.c), its lifted program and its two translated H8 programs.
set(TC2_HOST_DIR ${CMAKE_CURRENT_LIST_DIR})
set(TC2_HOST_SRC)
foreach(f src/tc2_cpu.c src/tc2_mem.c src/tc2_ctl.c src/tc2_chips.c src/tc2_screen.c src/tc2_text.c src/tc2_window.c src/tc2_main.c
          src/eng/eng_ui.c src/eng/eng_crosshair.c src/eng/eng_cfg.c src/eng/eng_display.c src/eng/ss22_input.c src/eng/eng_ffb.c src/tc2_input.c
          src/tc2_c435.c src/tc2_render.c src/tc2_gl.c src/h8_periph.c src/tc2_h8.c src/tc2_audio.c src/tc2_rom.c src/tc2_out.c
          src/eng/romzip.c src/eng/c352.c src/tc2_link.c src/eng/net.c src/eng/net_host.c src/eng/eng_net_ui.c src/eng/eng_rawptr.c src/tc2_subs.c)
  list(APPEND TC2_HOST_SRC ${TC2_HOST_DIR}/${f})
endforeach()
set(TC2_HOST_INCLUDE ${TC2_HOST_DIR}/include ${TC2_HOST_DIR}/src ${TC2_HOST_DIR}/src/eng)
