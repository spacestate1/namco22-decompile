/* frame_rule.c -- see frame_rule.h. Rave Racer's raverace/src/rr_scene.c is this rule's first, MAME-gated port. */
#include <stdint.h>
#include "frame_rule.h"

static bool     pdp_render_done, render_refresh;
static uint64_t pdp_frame, cur_frame;

void eng_pdp_begin(bool in_vblank) { pdp_frame = cur_frame + (in_vblank ? 1 : 0); pdp_render_done = true; }
void eng_render_refresh(void)      { render_refresh = true; }

bool eng_frame_walks(bool slave_active)
{
    cur_frame++;
    if (cur_frame > pdp_frame && render_refresh) pdp_render_done = false;
    render_refresh = false;
    return pdp_render_done && slave_active;
}

void eng_frame_force_walk(void) { pdp_render_done = true; render_refresh = false; pdp_frame = cur_frame + 1; }
