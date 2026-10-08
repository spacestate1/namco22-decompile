/* eng_net_ui.h -- the Online page of the shared menu (engine/eng_net_ui.c). After eng_cfg_load() and eng_net_init():
 *     eng_ui_add_page(eng_net_ui_page());
 * It reads/saves net_server (default zonesync.net) and net_name in the game's cfg; connecting is always a menu action. */
#ifndef ENG_NET_UI_H
#define ENG_NET_UI_H
#include "eng_ui.h"
const eng_ui_page *eng_net_ui_page(void);
#endif
