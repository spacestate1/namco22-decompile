#ifndef PC_LIVE2D_H
#define PC_LIVE2D_H
/* Prop Cycle's live 2D/fog state from g_sys, for the shared engine's modules (src/pc_live2d.c). */
#include "fog_hw.h"
#include "text_hw.h"
#include "sprite_hw.h"
int fog_load_live(void);                 /* g_sys.videomix + g_sys.czram -> g_fog */
int text_load_live(text_state *st);
int sprite_load_live(sprite_state *st);
#endif
