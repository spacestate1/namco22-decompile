/*
 * frame_rule.h -- which screen updates draw the master DSP's polygons (MAME's namcos22.cpp pdp_begin_r /
 * render_frame_active), shared by every System 22 / Super 22 game.
 *
 * MAME does not draw the display list on every update. The master begins its frame with a read of its PDP port (2); that
 * marks the list current for the update that follows. A later "render refresh" (a write to its port 8) means the master has
 * started over, and once a screen update finds the list older than the current frame AND a refresh pending, the polygons
 * are dropped until the next PDP begin: the frames between an attract scene's end and the next scene's start show no 3D.
 * MAME sets the recorded frame one AHEAD when the PDP begin lands during the vblank (the master answers the vblank INT0
 * there), so the update after it keeps the list.
 */
#ifndef ENG_FRAME_RULE_H
#define ENG_FRAME_RULE_H
#include <stdbool.h>

void eng_pdp_begin(bool in_vblank);              /* master port 2 read */
void eng_render_refresh(void);                   /* master port 8 write */
/* Once per screen update: advances the frame and says whether the list is to be walked (slave DSP on and the list current). */
bool eng_frame_walks(bool slave_active);
void eng_frame_force_walk(void);                 /* the renderer gate: draw a captured state with no CPU running */
#endif
