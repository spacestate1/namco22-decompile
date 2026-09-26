/*
 * eng_cfg.h -- a game's settings file: "key = value" lines (engine/eng_cfg.c).
 *
 * The menu (engine/eng_ui.c), the display settings (engine/eng_display.c) and a game's key bindings all keep their choices in
 * ONE small text file beside the binary (Prop Cycle's propcycl_controls.cfg, Rave Racer's rr_controls.cfg, Tokyo Wars'
 * tw_controls.cfg). Saving one option rewrites that one line and keeps every other line as the user wrote it. A run that never
 * loads the file (a headless gate) is unaffected by it.
 */
#ifndef ENG_CFG_H
#define ENG_CFG_H
#include <stdbool.h>

void        eng_cfg_load(const char *path);            /* read the file (missing = empty) and remember its path */
const char *eng_cfg_get(const char *key);              /* the value, or NULL */
int         eng_cfg_int(const char *key, int def);
bool        eng_cfg_set(const char *key, const char *val);   /* remember it and write it to the file */
bool        eng_cfg_set_int(const char *key, int val);
#endif
