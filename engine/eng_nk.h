/* eng_nk.h -- Nuklear's API for engine files that draw their own windows inside the shared menu's frame (engine/eng_ui.c owns
 * the implementation and the context; a layer gets the context in its draw hook). The NK_INCLUDE_* set MUST match eng_ui.c's. */
#ifndef ENG_NK_H
#define ENG_NK_H
#define NK_INCLUDE_FIXED_TYPES
#define NK_INCLUDE_STANDARD_IO
#define NK_INCLUDE_STANDARD_VARARGS
#define NK_INCLUDE_DEFAULT_ALLOCATOR
#define NK_INCLUDE_VERTEX_BUFFER_OUTPUT
#define NK_INCLUDE_FONT_BAKING
#define NK_INCLUDE_DEFAULT_FONT
#include "../third_party/nuklear.h"
#endif
