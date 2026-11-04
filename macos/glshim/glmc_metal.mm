/* glmc_metal.mm — the Metal device layer behind the GL-call-shape adapter.
 *
 * This file owns every real GPU operation: the Metal device, textures, frame-
 * buffer objects, render passes, clear semantics, pipeline state keyed by the
 * per-draw pipekey, the copy/blit paths, CPU readback, and presenting the
 * default target to the window's CAMetalLayer. The GL state machine
 * (glmc_main.c) calls in through glmc_dev.h.
 *
 * Coordinate conventions.
 *   GL semantics (what every shader here, upstream included, assumes):
 *     NDC y up; framebuffer pixel rows addressed from the BOTTOM (row 0 is
 *     the visual bottom line); [[position]] == gl_FragCoord.
 *   Metal rasterises with the framebuffer origin at the TOP left: NDC -1
 *   lands in the LAST memory row, and the first memory row is the visual
 *   top. So raw geometry alone would come out mirrowed row-wise against GL.
 *   The port chooses GL memory order, which the shaders take for granted:
 *     - the game vertex shaders (ffp, tc2p, tc2q) negate NDC y; that makes
 *       texture memory row 0 the game's visual bottom (GL order) and makes
 *       [[position]] == gl_FragCoord exactly (both are (1+ygl)/2*vp from the
 *       viewport origin, memY = fbH - vpY - vpH compensates the viewport).
 *     - every rect this face receives is GL-encoded (bottom-left) and is
 *       mapped to the Metal framebuffer rect memY = fbH - y - h here.
 *     - the internal blit shader is NOT negated: it paints the whole
 *       viewport of its pass, and its source rect is a GL-row-space texel
 *       rect sampled as uv = sc.xy + c*sc.zw. With a positive sc.h that maps
 *       the source GL rows 1:1 to the destination GL rows in the same
 *       pass-viewport transform, and the present blit (GL-ordered source
 *       into CAMetalLayer rows) also needs no extra flip: sc=(0,0,w,h).
 *
 * Clears are always their own small render pass (loadAction .clear + store),
 * which keeps order with any draws the game already did, without pending-state
 * complexity. Back-to-back draws into one target share the open render pass.
 *
 * The face is a plain malloc'd C struct whose ObjC fields are
 * __unsafe_unretained. Nothing releases those fields (a C struct is not an
 * ARC object); in exchange the Metal objects simply persist until the
 * process exits. Games create exactly one face each, so this is contained.
 *
 * No OpenGL framework is ever loaded; nothing here dlopens it either. */
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#import <Foundation/Foundation.h>
#import <CoreGraphics/CoreGraphics.h>
#import <SDL.h>
#import <AppKit/NSCursor.h>

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

#include "glmc_dev.h"
#include "glmc_msl.h"

/* ------------------------------------------------------------------ types */
/* The face and its tables own their Objective-C objects with ARC __strong
 * ownership (the default). The face is a C++ struct allocated with new/delete
 * so its destructor runs and releases everything cleanly. */
struct glmcd_tex {
    id<MTLTexture> t = nil;         /* nil until storage/upload */
    int w = 0, h = 0, px = 0;       /* GLMC_PX_* */
    int live = 0;                   /* name reserved (may not be stored yet)  */
};
struct glmcd_fbo {
    uint32_t color = 0, depth = 0;  /* texture ids, 0 = none */
    int live = 0;                   /* name reserved */
};
struct glmcd_tgt {                  /* a resolved framebuffer binding */
    int w = 0, h = 0;
    id<MTLTexture> color = nil, depth = nil;  /* depth nil = no depth support */
    int color_px = 0, depth_px = 0; /* GLMC_PX_*; depth_px -1 when unattached */
};
struct glmcd_pskey {                /* full pipeline identity */
    glmcd_pipekey pk;
    int color_px = 0, depth_px = 0; /* -1 = no depth attachment */
};

enum {
    GLMC_TEX_MAX  = 4096,
    GLMC_FBO_MAX  = 256,
    GLMC_PIPE_MAX = 512,
    GLMC_SMP_MAX  = 128,
    GLMC_LIB_COUNT = 7              /* the units tests/compile_msl.m builds */
};

/* pipeline / depth-stencil / sampler cache entries (linear search; the games
 * use a handful of state combinations) */
struct glmcd_psc {
    struct glmcd_pskey k;
    id<MTLRenderPipelineState> p = nil;
};
struct glmcd_ds {
    int on = 0, func = 0, write = 0;
    id<MTLDepthStencilState> st = nil;
};
struct glmcd_smpc {
    int minf = 0, magf = 0, wraps = 0, wrapt = 0;
    id<MTLSamplerState> st = nil;
};
struct glmcd_blitc {
    int px = 0;                     /* MTLPixelFormat of the color attachment */
    id<MTLRenderPipelineState> p = nil;
};

struct glmcd_face {
    id<MTLDevice> dev = nil;
    id<MTLCommandQueue> q = nil;
    void *metal_view = nullptr;     /* SDL_MetalView (opaque) */
    CAMetalLayer *layer = nil;
    int win_w = 0, win_h = 0;       /* drawable pixels */
    float scale = 1.0f;

    struct glmcd_tex tex[GLMC_TEX_MAX];
    int cur_fbo = 0;                /* draw binding (fbo id; 0 = window) */
    int read_fbo = 0;               /* GL_READ_FRAMEBUFFER binding */

    /* the default target: an intermediate RGBA8 + DEPTH32F pair, GL-ordered
     * rows, resized with the drawable. The present blit copies it 1:1 (with
     * the natural head-rows swap) onto the CAMetalLayer drawable. */
    int def_w = 0, def_h = 0;
    id<MTLTexture> def_color = nil, def_depth = nil;
    id<MTLTexture> white = nil;     /* 1x1 white guard for disabled/absent   */

    /* GL viewport / scissor as passed in (GL encoding) + target dims */
    int vpx = 0, vpy = 0, vpw = 0, vph = 0, tgt_w = 0, tgt_h = 0;
    int sc_on = 0, scx = 0, scy = 0, scw = 0, sch = 0;

    float clear_rgba[4] = { 0, 0, 0, 0 };
    double clear_depth = 1.0;

    /* in-flight command buffer + open render pass */
    id<MTLCommandBuffer> cb = nil;
    id<MTLRenderCommandEncoder> enc = nil;
    int enc_fbo = -1, enc_w = 0, enc_h = 0;
    id<MTLTexture> enc_color = nil, enc_depth = nil;

    NSMutableArray<id<MTLBuffer>> *pending = nil;
    id<MTLLibrary> libtab[GLMC_LIB_COUNT];
    int n_ps = 0; struct glmcd_psc ps[GLMC_PIPE_MAX];
    int n_ds = 0; struct glmcd_ds ds[16];
    int n_smp = 0; struct glmcd_smpc smps[GLMC_SMP_MAX];
    int n_blit = 0; struct glmcd_blitc blit[8];

    int swap_interval = 1;

    glmcd_face() = default;
    ~glmcd_face();
};

glmcd_face::~glmcd_face()
{
    if (enc) [enc endEncoding];
    if (cb)  [cb commit];
    if (metal_view) glmc_sdl_metal_destroy(metal_view);
}

/* ------------------------------------------------ notes, formats, mapping */
void glmc_dev_note(const char *msg)
{
    static char seen[8][80]; static int n;
    if (n < 8) {
        for (int i = 0; i < n; i++)
            if (!strncmp(seen[i], msg, 79)) return;
        snprintf(seen[n], 80, "%s", msg);
        n++;
    }
    fprintf(stderr, "[glmc] %s\n", msg);
}

static MTLPixelFormat glmcd_mtlfmt(int px)
{
    switch (px) {
    case GLMC_PX_RGBA8:    return MTLPixelFormatRGBA8Unorm;
    case GLMC_PX_BGRA8:    return MTLPixelFormatBGRA8Unorm;
    case GLMC_PX_R8:       return MTLPixelFormatR8Unorm;
    case GLMC_PX_RG8:      return MTLPixelFormatRG8Unorm;
    case GLMC_PX_DEPTH32F: return MTLPixelFormatDepth32Float;
    case GLMC_PX_RGB8:     return MTLPixelFormatRGBA8Unorm; /* read-only alias */
    default:               return MTLPixelFormatInvalid;
    }
}
static int glmcd_bpp(int px)
{
    switch (px) {
    case GLMC_PX_RGBA8: case GLMC_PX_BGRA8: return 4;
    case GLMC_PX_RGB8:   return 3;              /* readback-only, 3 bytes/px */
    case GLMC_PX_R8:    return 1;
    case GLMC_PX_RG8:   return 2;
    default:            return 4;
    }
}

static NSString *glmcd_concat(const char *const *parts, int n)
{
    NSMutableString *s = [NSMutableString string];
    for (int i = 0; i < n; i++) {
        NSString *p = [NSString stringWithUTF8String:parts[i]];
        NSRange r = [p rangeOfString:@"\n" options:NSBackwardsSearch];
        while (r.length && r.location == p.length - 1) {
            p = [p substringToIndex:r.location];
            r = [p rangeOfString:@"\n" options:NSBackwardsSearch];
        }
        [s appendFormat:@"%@\n", p];
    }
    return s;
}

static const char *glmcd_lib_vfn(int lib)
{
    switch (lib) {
    case 0: case 1:     return "glmc_ffp_v";
    case 2: case 3:     return "glmc_tc2p_v";
    case 4: case 5:     return "glmc_tc2q_v";
    case 6:             return "glmc_blit_v";
    }
    return NULL;
}
static const char *glmcd_lib_ffield(int lib)
{
    switch (lib) {
    case 0:             return "glmc_ffp_f";
    case 1:             return "glmc_post_f";
    case 2: case 3:     return "glmc_tc2p_f";
    case 4:             return "glmc_tc2t_f";
    case 5:             return "glmc_tc2g_f";
    case 6:             return "glmc_blit_f";
    }
    return NULL;
}

/* the translation units, in tests/compile_msl.m's exact order */
static NSString *glmcd_unit(int i)
{
    static NSString *u[GLMC_LIB_COUNT]; static id dev_saved;
    (void)dev_saved;
    if (!u[0]) {
        u[0] = glmcd_concat((const char *[]){ GLMC_MSL_FFP }, 1);
        u[1] = glmcd_concat((const char *[]){ GLMC_MSL_FFP, GLMC_MSL_POST1 }, 2);
        u[2] = glmcd_concat((const char *[]){ GLMC_MSL_TC2P_V, GLMC_MSL_TC2P_SH0,
                                             GLMC_MSL_TC2P_PLAIN_HEAD, GLMC_MSL_TC2P_BODY,
                                             GLMC_MSL_TC2P_TAIL }, 5);
        u[3] = glmcd_concat((const char *[]){ GLMC_MSL_TC2P_V, GLMC_MSL_TC2P_SH0,
                                             GLMC_MSL_TC2P_DEPTH_HEAD, GLMC_MSL_TC2P_BODY,
                                             GLMC_MSL_TC2P_TAIL }, 5);
        u[4] = glmcd_concat((const char *[]){ GLMC_MSL_TC2Q_V, GLMC_MSL_TC2Q_TAILHEAD,
                                             GLMC_MSL_TC2_TEXT }, 3);
        u[5] = glmcd_concat((const char *[]){ GLMC_MSL_TC2Q_V, GLMC_MSL_TC2Q_TAILHEAD,
                                             GLMC_MSL_TC2_GAMMA }, 3);
        u[6] = glmcd_concat((const char *[]){ GLMC_MSL_BLIT }, 1);
    }
    return u[i];
}

/* ------------------------------------------------------------- lifecycle */
glmcd_face *glmc_dev_face_new(void *sdl_window, int w, int h, float scale)
{
    glmcd_face *f = new glmcd_face();
    if (!f) return NULL;
    f->dev = MTLCreateSystemDefaultDevice();
    if (!f->dev) { glmc_dev_note("no Metal device"); free(f); return NULL; }
    f->q = [f->dev newCommandQueue];
    f->pending = [NSMutableArray new];
    f->cur_fbo = f->read_fbo = 0;
    f->swap_interval = 1;
    if (getenv("GLMC_TRACE")) {
        fprintf(stderr, "[trace] face_new: pending=%p size=%zu | &pending=%p | dev=%p q=%p\n",
                (__bridge void *)f->pending, sizeof *f,
                (void *)&f->pending,
                (__bridge void *)f->dev, (__bridge void *)f->q);
    }
    glmc_dev_face_rewindowed(f, sdl_window, w, h, scale);
    return f;
}

void glmc_dev_face_rewindowed(glmcd_face *face, void *sdl_window, int w, int h, float scale)
{
    int dw = 0, dh = 0;
    if (sdl_window) {
        if (!face->metal_view) face->metal_view = glmc_sdl_metal_view(sdl_window);
        if (face->metal_view) face->layer = (__bridge CAMetalLayer *)glmc_sdl_metal_layer(face->metal_view);
        if (face->layer) {
            if (face->layer.device != face->dev) face->layer.device = face->dev;
            face->layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
            face->layer.framebufferOnly = YES;
            face->layer.maximumDrawableCount = 3;
            face->layer.opaque = YES;
            glmc_sdl_metal_size(sdl_window, &dw, &dh);
            face->layer.drawableSize = CGSizeMake((CGFloat)(dw ? dw : w),
                                                  (CGFloat)(dh ? dh : h));
            face->layer.displaySyncEnabled = face->swap_interval != 0;
        }
    }
    if (!dw && !dh) { dw = w; dh = h; }         /* windowless face fallback */
    if (dw != face->win_w || dh != face->win_h)
        glmc_dev_note("drawable size used directly (Metal view metrics)");
    face->win_w = dw; face->win_h = dh;
    face->scale = scale;
    face->def_w = face->win_w; face->def_h = face->win_h;
    /* an open pass is closed before its targets go: a dropped encoder stays
     * open on the command buffer, and Metal aborts on the next encoder or
     * commit (the window resized mid-frame, e.g. maximised at start-up) */
    if (face->enc) [face->enc endEncoding];
    face->def_color = face->def_depth = nil;    /* rebuilt lazily */
    face->enc = nil; face->enc_fbo = -1;        /* open pass invalidated */
    face->enc_color = face->enc_depth = nil;
    face->n_ps = face->n_ds = face->n_smp = face->n_blit = 0;
}

void glmc_dev_face_free(glmcd_face *face)
{
    if (!face) return;
    delete face;
}

void glmc_dev_face_refit(glmcd_face *face, void *sdl_window)
{
    if (!face || !sdl_window) return;
    int lw = 0, lh = 0, dw = 0, dh = 0;
    SDL_GetWindowSize((SDL_Window *)sdl_window, &lw, &lh);
    SDL_Metal_GetDrawableSize((SDL_Window *)sdl_window, &dw, &dh);
    if (dw < 1 || dh < 1) return;
    float scale = lw > 0 ? (float)dw / (float)lw : face->scale;
    if (dw == face->win_w && dh == face->win_h && dw == face->def_w && dh == face->def_h &&
        scale == face->scale) return;
    fprintf(stderr, "[glmc] window now %dx%d pixels (was %dx%d)\n", dw, dh, face->win_w, face->win_h);
    glmc_dev_face_rewindowed(face, sdl_window, dw, dh, scale);
}

void glmc_dev_bind(glmcd_face *f) { if (getenv("GLMC_TRACE")) fprintf(stderr,"[E] bind\n"); (void)f; }   /* single-threaded design */

void glmc_dev_set_swap_interval(glmcd_face *f, int v)
{
    if (getenv("GLMC_TRACE")) fprintf(stderr,"[E] swapint\n");
    f->swap_interval = v;
    if (f->layer) f->layer.displaySyncEnabled = v != 0;
}

/* ------------------------------------------------------- fbo id table ---- */
/* GL object space is process-global, so the fbo table is (glmc_main.c and the
 * face agree it lives here); its entries name texture ids per face. */
static struct glmcd_fbo glmcd_fbos[GLMC_FBO_MAX];
static struct glmcd_fbo *glmcd_fbo_rec(uint32_t id)
{
    if (id < 1 || id >= GLMC_FBO_MAX) return NULL;
    return &glmcd_fbos[id];
}

uint32_t glmc_fbo_new(void)
{
    for (uint32_t i = 1; i < GLMC_FBO_MAX; i++)
        if (!glmcd_fbos[i].live) {
            glmcd_fbos[i].live = 1;
            glmcd_fbos[i].color = glmcd_fbos[i].depth = 0;
            return i;
        }
    glmc_dev_note("fbo table full");
    return 0;
}
void glmc_fbo_del(uint32_t id)
{
    struct glmcd_fbo *r = glmcd_fbo_rec(id);
    if (r) { r->live = 0; r->color = r->depth = 0; }
}
void glmc_fbo_attach(uint32_t fbo, int slot, uint32_t tex)
{
    struct glmcd_fbo *r = glmcd_fbo_rec(fbo);
    if (!r) { glmc_dev_note("fbo attach on bad id"); return; }
    if (slot == 0) r->color = tex;
    else if (slot == 1) r->depth = tex;
    else glmc_dev_note("bad fbo attach slot");
}
int glmc_fbo_status(uint32_t fbo)
/* 0 = complete, nonzero reasons: every use is "is it usable?" */
{
    struct glmcd_fbo *r = glmcd_fbo_rec(fbo);
    if (!r || !r->color) return 1;              /* unnamed / no colour */
    return 0;
}

/* --------------------------------------------------------- textures ----- */
uint32_t glmc_tex_new(glmcd_face *f)
{
    for (int i = 0; i < GLMC_TEX_MAX; i++)
        if (!f->tex[i].live) {
            f->tex[i].live = 1;
            f->tex[i].t = nil;
            f->tex[i].w = f->tex[i].h = f->tex[i].px = 0;
            return (uint32_t)(i + 1);
        }
    glmc_dev_note("texture table full");
    return 0;
}
void glmc_tex_del(glmcd_face *f, uint32_t id)
{
    if (id < 1 || id > GLMC_TEX_MAX) return;
    struct glmcd_tex *t = &f->tex[id - 1];
    if (f->enc && (f->enc_color == t->t || f->enc_depth == t->t)) {
        [f->enc endEncoding];
        f->enc = nil; f->enc_fbo = -1; f->enc_color = f->enc_depth = nil;
    }
    t->t = nil;
    t->live = 0;
}

void glmc_tex_storage(glmcd_face *f, uint32_t id, int fmt, int w, int h)
{
    if (id < 1 || id > GLMC_TEX_MAX || w < 1 || h < 1) { glmc_dev_note("bad tex storage"); return; }
    MTLPixelFormat pf = glmcd_mtlfmt(fmt);
    MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:pf
                                    width:(NSUInteger)w height:(NSUInteger)h mipmapped:NO];
    d.usage = MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget;
    /* colour textures are CPU-uploaded via replaceRegion, which is only legal
     * on CPU-accessible storage (Shared on unified memory); depth textures are
     * render-target only and stay Private. */
    d.storageMode = (fmt == GLMC_PX_DEPTH32F) ? MTLStorageModePrivate
                                              : MTLStorageModeShared;
    struct glmcd_tex *t = &f->tex[id - 1];
    t->t = [f->dev newTextureWithDescriptor:d];
    t->w = w; t->h = h; t->px = fmt;
    if (getenv("GLMC_TRACE"))
        fprintf(stderr, "[trace] tex_storage face=%p tid=%u -> %p %dx%d fmt=%d\n",
                (void *)f, id, (__bridge void *)t->t, w, h, fmt);
}
static void glmcd_pass_close(glmcd_face *f);

void glmc_tex_upload(glmcd_face *f, uint32_t texid, int x, int y, int w, int h,
                     const void *data, int pitch_bytes)
{
    if (texid < 1 || texid > GLMC_TEX_MAX) return;
    struct glmcd_tex *t = &f->tex[texid - 1];
    if (!t->t) { glmc_dev_note("upload before storage"); return; }
    if (!data || w <= 0 || h <= 0) return;
    const size_t row_bytes = (size_t)w * (size_t)glmcd_bpp(t->px);
    if (pitch_bytes <= 0) pitch_bytes = (int)row_bytes;
    if ((size_t)pitch_bytes < row_bytes) { glmc_dev_note("short texture upload row"); return; }
    /* The game reuses a texture several times in one frame. CPU writes to a
     * shared texture race queued draws; an ordered blit retains each version
     * until the draw preceding the next update has consumed it. */
    const size_t aligned_row = (row_bytes + 255u) & ~(size_t)255u;
    id<MTLBuffer> upload = [f->dev newBufferWithLength:aligned_row * (size_t)h
                                               options:MTLResourceStorageModeShared];
    if (!upload) { glmc_dev_note("texture upload buffer allocation failed"); return; }
    uint8_t *dst = (uint8_t *)upload.contents;
    const uint8_t *src = (const uint8_t *)data;
    for (int row = 0; row < h; row++)
        memcpy(dst + (size_t)row * aligned_row, src + (size_t)row * (size_t)pitch_bytes, row_bytes);
    glmcd_pass_close(f);
    if (!f->cb) f->cb = [f->q commandBuffer];
    [f->pending addObject:upload];
    id<MTLBlitCommandEncoder> blit = [f->cb blitCommandEncoder];
    [blit copyFromBuffer:upload sourceOffset:0 sourceBytesPerRow:aligned_row
        sourceBytesPerImage:aligned_row * (size_t)h sourceSize:MTLSizeMake(w, h, 1)
        toTexture:t->t destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(x, y, 0)];
    [blit endEncoding];
    if (getenv("GLMC_TRACE")) {
        fprintf(stderr, "[trace] queued tex_upload face=%p tid=%u @(%d,%d) %dx%d pitch=%d fmt=%#x\n",
                (void *)f, texid, x, y, w, h, pitch_bytes, t->px);
    }
}
void glmc_tex_size(glmcd_face *f, uint32_t id, int *w, int *h, int *fmt)
{
    struct glmcd_tex *t = (id >= 1 && id <= GLMC_TEX_MAX) ? &f->tex[id - 1] : NULL;
    if (w) *w = t ? t->w : 0;
    if (h) *h = t ? t->h : 0;
    if (fmt) *fmt = t ? t->px : GLMC_PX_RGBA8;
}

/* CHUNK1END */

/* --------------------------------------------------- target resolution -- */
/* Resolves an fbo binding (0 = default window target) into textures + sizes.
 * The default target is an intermediate RGBA8 + D32F pair owned by the face;
 * its lifetime spans frames so back-to-back non-clearing draws behave like GL
 * back-buffer reuse. */
static void glmcd_tgt_of(glmcd_face *f, uint32_t fbo, struct glmcd_tgt *tgt)
{
    *tgt = glmcd_tgt{};
    if (fbo == 0 || fbo >= GLMC_FBO_MAX || glmcd_fbos[fbo].color == 0) {
        if (fbo != 0) { f->def_w = f->win_w; f->def_h = f->win_h; }
        if (!f->def_color || (int)f->def_color.width != f->def_w ||
                             (int)f->def_color.height != f->def_h) {
            if (getenv("GLMC_TRACE"))
                fprintf(stderr, "[trace] tgt_of: fbo=%u ret=%p creating def textures dev=%p w=%d h=%d\n",
                        fbo, __builtin_return_address(0), (__bridge void *)f->dev, f->def_w, f->def_h);
            MTLTextureDescriptor *cd = [MTLTextureDescriptor
                texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                width:(NSUInteger)f->def_w height:(NSUInteger)f->def_h mipmapped:NO];
            cd.usage = MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget;
            cd.storageMode = MTLStorageModePrivate;
            MTLTextureDescriptor *dd = [MTLTextureDescriptor
                texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float
                width:(NSUInteger)f->def_w height:(NSUInteger)f->def_h mipmapped:NO];
            dd.usage = MTLTextureUsageRenderTarget;
            dd.storageMode = MTLStorageModePrivate;
            f->def_color = [f->dev newTextureWithDescriptor:cd];
            f->def_depth = [f->dev newTextureWithDescriptor:dd];
            if (getenv("GLMC_TRACE"))
                fprintf(stderr, "[trace] def textures: color=%p (%dx%d) depth=%p\n",
                        (__bridge void *)f->def_color, (int)f->def_color.width,
                        (int)f->def_color.height, (__bridge void *)f->def_depth);
            if (f->enc) [f->enc endEncoding];   /* close, not drop, an open pass */
            f->enc = nil; f->enc_fbo = -1;      /* attached textures changed */
            f->enc_color = f->enc_depth = nil;
        }
        tgt->w = f->def_w; tgt->h = f->def_h;
        tgt->color = f->def_color; tgt->depth = f->def_depth;
        tgt->color_px = GLMC_PX_RGBA8; tgt->depth_px = GLMC_PX_DEPTH32F;
        return;
    }
    struct glmcd_fbo *r = &glmcd_fbos[fbo];
    struct glmcd_tex *c = (r->color >= 1 && r->color <= GLMC_TEX_MAX) ? &f->tex[r->color - 1] : NULL;
    struct glmcd_tex *d = (r->depth >= 1 && r->depth <= GLMC_TEX_MAX) ? &f->tex[r->depth - 1] : NULL;
    if (!c || !c->t) { glmc_dev_note("fbo without a stored colour texture"); return; }
    tgt->w = c->w; tgt->h = c->h; tgt->color = c->t; tgt->color_px = c->px;
    if (d && d->t) { tgt->depth = d->t; tgt->depth_px = d->px;
                     tgt->w = (int)fmaxf((float)tgt->w, (float)d->w);
                     tgt->h = (int)fmaxf((float)tgt->h, (float)d->h); }
    else tgt->depth_px = -1;
}

/* --------------------------------------------------- pass open / close --- */
static void glmcd_pass_close(glmcd_face *f)
{
    if (f->enc) { [f->enc endEncoding]; f->enc = nil; }
}
static void glmcd_pass_open(glmcd_face *f, MTLLoadAction color_load)
/* opens (or reuses) the render pass of the CURRENT draw binding. color_load is
 * MTLLoadActionLoad for draws and DontCare for fresh blit-only passes. */
{
    struct glmcd_tgt t; glmcd_tgt_of(f, (uint32_t)f->cur_fbo, &t);
    if (!t.color) { glmc_dev_note("draw into an unresolved target"); return; }
    if (f->enc && f->enc_fbo == f->cur_fbo && f->enc_w == t.w && f->enc_h == t.h &&
        f->enc_color == t.color && f->enc_depth == t.depth)
        return;                                     /* reuse the open pass */
    if (f->enc) glmcd_pass_close(f);
    if (!f->cb) f->cb = [f->q commandBuffer];

    MTLRenderPassDescriptor *d = [MTLRenderPassDescriptor renderPassDescriptor];
    d.colorAttachments[0].texture = t.color;
    d.colorAttachments[0].loadAction = color_load;
    d.colorAttachments[0].storeAction = MTLStoreActionStore;
    if (t.depth) {
        d.depthAttachment.texture = t.depth;
        d.depthAttachment.loadAction = MTLLoadActionLoad;
        d.depthAttachment.storeAction = MTLStoreActionStore;
    }
    f->enc = [f->cb renderCommandEncoderWithDescriptor:d];
    f->enc_fbo = f->cur_fbo; f->enc_w = t.w; f->enc_h = t.h;
    f->enc_color = t.color; f->enc_depth = t.depth;
}

/* ------------------------------------------- GL rect -> Metal rect mapping */
static void glmcd_mem_rect(glmcd_face *f, const struct glmcd_tgt *t,
                           int x, int y, int w, int h,
                           int *mx, int *my, int *mw, int *mh)
/* GL rect (bottom-left) -> Metal framebuffer rect, clipped. Pixels are stored
 * y-up (GL row r = memory row r, glmc_msl.h) and every vertex program negates
 * NDC y, so a GL rect sits at the same rows in memory. (It used to mirror the
 * rows, t->h - y - h: right only for full-height or centred rects, so an
 * off-centre viewport, scissor or blit landed mirrored top-to-bottom.) */
{
    *mx = x; *mw = w;
    *my = y;                        /* GL row y (rect bottom) = memory row     */
    *mh = h;
    if (*my < 0) { *mh += *my; *my = 0; }
    if (*mx < 0) { *mw += *mx; *mx = 0; }
    if (*my + *mh > t->h) *mh = t->h - *my;
    if (*mx + *mw > t->w) *mw = t->w - *mx;
    if (*mw < 0) *mw = 0;
    if (*mh < 0) *mh = 0;
}

void glmc_dev_fbo(glmcd_face *f, uint32_t fbo)
{
    if (getenv("GLMC_TRACE")) fprintf(stderr,"[E] fbo %d\n",(int)fbo);
    glmcd_pass_close(f);                     /* first draw/clear re-opens     */
    f->cur_fbo = (int)fbo;
    struct glmcd_tgt t; glmcd_tgt_of(f, fbo, &t);
    f->tgt_w = t.w; f->tgt_h = t.h;
}

void glmc_dev_read_fbo(glmcd_face *f, uint32_t fbo)
{
    if (getenv("GLMC_TRACE")) fprintf(stderr,"[E] rdfbo\n");
    f->read_fbo = (int)fbo;
}

void glmc_dev_fbo_size(glmcd_face *f, uint32_t fbo, int *w, int *h)
{
    if (getenv("GLMC_TRACE")) fprintf(stderr,"[E] fbosize\n");
    struct glmcd_tgt t; glmcd_tgt_of(f, fbo, &t);
    *w = t.w; *h = t.h;
}

/* ------------------------------------------------------------ clearing --- */
/* glClear: always its own small pass, so order with earlier draws holds even
 * if a pass was already open. Partial masks degrade to a full clear with a
 * note (upstream only clears with masks fully on). */
void glmc_dev_clear(glmcd_face *f, int mask, float r, float g, float b, float a, double depth)
{
    struct glmcd_tgt t; glmcd_tgt_of(f, (uint32_t)f->cur_fbo, &t);
    if (getenv("GLMC_TRACE"))
        fprintf(stderr, "[trace] clear mask=%d fbo=%d w=%d h=%d depth=%d\n",
                mask, f->cur_fbo, t.w, t.h, (int)(t.depth != nil));
    if (!t.color) return;
    if (!(mask & (GLMC_CLR_COLOR | GLMC_CLR_DEPTH))) return;
    int docolor = (mask & GLMC_CLR_COLOR) && t.color_px != GLMC_PX_DEPTH32F;
    int dodepth = (mask & GLMC_CLR_DEPTH) && t.depth;

    glmcd_pass_close(f);
    if (!f->cb) f->cb = [f->q commandBuffer];
    MTLRenderPassDescriptor *d = [MTLRenderPassDescriptor renderPassDescriptor];
    d.colorAttachments[0].texture = t.color;
    d.colorAttachments[0].loadAction = docolor ? MTLLoadActionClear : MTLLoadActionLoad;
    d.colorAttachments[0].storeAction = MTLStoreActionStore;
    d.colorAttachments[0].clearColor =
        MTLClearColorMake((double)r, (double)g, (double)b, (double)a);
    if (t.depth) {
        d.depthAttachment.texture = t.depth;
        d.depthAttachment.loadAction = dodepth ? MTLLoadActionClear : MTLLoadActionLoad;
        d.depthAttachment.storeAction = MTLStoreActionStore;
        d.depthAttachment.clearDepth = depth;
    }
    /* a clear pass draws nothing; encode-then-end defers the write into the
     * right submission position, after prior draws and before later ones    */
    [[f->cb renderCommandEncoderWithDescriptor:d] endEncoding];
}

void glmc_dev_viewport(glmcd_face *f, int x, int y, int w, int h, int fbo_w, int fbo_h)
{
    if (getenv("GLMC_TRACE")) fprintf(stderr,"[E] viewport\n");
    f->vpx = x; f->vpy = y; f->vpw = w; f->vph = h;
    f->tgt_w = fbo_w; f->tgt_h = fbo_h;
}

void glmc_dev_scissor(glmcd_face *f, int on, int x, int y, int w, int h, int fbo_w, int fbo_h)
{
    if (getenv("GLMC_TRACE")) fprintf(stderr,"[E] scissor\n");
    f->sc_on = on; f->scx = x; f->scy = y; f->scw = w; f->sch = h;
    if (fbo_w > 0) { f->tgt_w = fbo_w; f->tgt_h = fbo_h; }
}

/* CHUNK2END */

/* ------------------------------------------------ pipeline construction -- */
static id<MTLLibrary> glmcd_lib(glmcd_face *f, int lib)
{
    if (lib < 0 || lib >= GLMC_LIB_COUNT) { glmc_dev_note("bad MSL unit"); return nil; }
    if (f->libtab[lib]) return f->libtab[lib];
    NSError *err = nil;
    id<MTLLibrary> l = [f->dev newLibraryWithSource:glmcd_unit(lib)
                                            options:nil error:&err];
    if (!l) {
        fprintf(stderr, "[glmc] MSL unit %d failed: %s\n", lib,
                err.localizedDescription.UTF8String);
        glmc_dev_note("MSL library build failed");
        return nil;
    }
    f->libtab[lib] = l;
    return l;
}

static void glmcd_pipekey_norm(struct glmcd_pskey *k)
{
    glmcd_pipekey *p = &k->pk;
    memset(k, 0, sizeof *k);
    memset(p, 0, sizeof *p);            /* no uninitialised bytes in the key */
}

static MTLBlendFactor glmcd_blendf(int glf, int source_side)
{
    switch (glf) {
    case 0:      return MTLBlendFactorZero;
    case 1:      return MTLBlendFactorOne;
    case 0x0300: return MTLBlendFactorSourceColor;
    case 0x0301: return MTLBlendFactorOneMinusSourceColor;
    case 0x0302: return MTLBlendFactorSourceAlpha;
    case 0x0303: return MTLBlendFactorOneMinusSourceAlpha;
    case 0x0304: return MTLBlendFactorDestinationAlpha;
    case 0x0305: return MTLBlendFactorOneMinusDestinationAlpha;
    case 0x0306: return MTLBlendFactorDestinationColor;
    case 0x0307: return MTLBlendFactorOneMinusDestinationColor;
    case 0x0308:
        if (source_side) return MTLBlendFactorSourceAlphaSaturated;
        glmc_dev_note("SRC_ALPHA_SATURATE as a destination factor");
        return MTLBlendFactorZero;
    default:
        glmc_dev_note("unknown GL blend factor");
        return MTLBlendFactorZero;
    }
}
static MTLBlendOperation glmcd_blende(int gle)
{
    switch (gle) {
    case 0x8006: return MTLBlendOperationAdd;
    case 0x8007: return MTLBlendOperationMin;
    case 0x8008: return MTLBlendOperationMax;
    case 0x8009: return MTLBlendOperationSubtract;
    case 0x800B: return MTLBlendOperationReverseSubtract;
    default:
        glmc_dev_note("unknown GL blend equation");
        return MTLBlendOperationAdd;
    }
}
static MTLCompareFunction glmcd_cmpf(int glf)
{
    switch (glf) {
    case 0x0200: return MTLCompareFunctionNever;
    case 0x0201: return MTLCompareFunctionLess;
    case 0x0202: return MTLCompareFunctionEqual;
    case 0x0203: return MTLCompareFunctionLessEqual;
    case 0x0204: return MTLCompareFunctionGreater;
    case 0x0205: return MTLCompareFunctionNotEqual;
    case 0x0206: return MTLCompareFunctionGreaterEqual;
    case 0x0207: return MTLCompareFunctionAlways;
    default:
        glmc_dev_note("unknown GL compare function");
        return MTLCompareFunctionAlways;
    }
}

static id<MTLRenderPipelineState> glmcd_pipeline(glmcd_face *f, const glmcd_pipekey *pk,
                                                 const struct glmcd_tgt *t)
{
    struct glmcd_pskey k; glmcd_pipekey_norm(&k);
    k.pk = *pk;
    k.color_px = t->color_px;
    k.depth_px = t->depth ? t->depth_px : -1;

    for (int i = 0; i < f->n_ps; i++)
        if (!memcmp(&f->ps[i].k, &k, sizeof k)) return f->ps[i].p;
    if (f->n_ps >= GLMC_PIPE_MAX) { glmc_dev_note("pipeline cache full"); return nil; }

    int prog = k.pk.prog, lib;
    switch (prog) {
    case GLMC_PROG_FFP:       lib = 0; break;
    case GLMC_PROG_POST1:     lib = 1; break;
    case GLMC_PROG_TC2_POLY:  lib = 2 + (k.pk.fragdepth ? 1 : 0); break;
    case GLMC_PROG_TC2_TEXT:  lib = 4; break;
    case GLMC_PROG_TC2_GAMMA: lib = 5; break;
    default:
        glmc_dev_note("unknown program id");
        return nil;
    }
    id<MTLLibrary> l = glmcd_lib(f, lib);
    if (!l) return nil;

    MTLRenderPipelineDescriptor *d = [MTLRenderPipelineDescriptor new];
    d.vertexFunction   = [l newFunctionWithName:@(glmcd_lib_vfn(lib))];
    d.fragmentFunction = [l newFunctionWithName:@(glmcd_lib_ffield(lib))];
    d.colorAttachments[0].pixelFormat = glmcd_mtlfmt(k.color_px);
    d.colorAttachments[0].blendingEnabled = k.pk.blend.on ? YES : NO;
    d.colorAttachments[0].sourceRGBBlendFactor      = glmcd_blendf(k.pk.blend.f_srgb,   1);
    d.colorAttachments[0].sourceAlphaBlendFactor    = glmcd_blendf(k.pk.blend.f_salpha, 1);
    d.colorAttachments[0].destinationRGBBlendFactor = glmcd_blendf(k.pk.blend.f_drgb,   0);
    d.colorAttachments[0].destinationAlphaBlendFactor = glmcd_blendf(k.pk.blend.f_dalpha, 0);
    d.colorAttachments[0].rgbBlendOperation = glmcd_blende(k.pk.blend.eq_rgb);
    d.colorAttachments[0].alphaBlendOperation = glmcd_blende(k.pk.blend.eq_a);
    uint maskw = (uint)(k.pk.mask_r ? MTLColorWriteMaskRed   : 0) |
                 (uint)(k.pk.mask_g ? MTLColorWriteMaskGreen : 0) |
                 (uint)(k.pk.mask_b ? MTLColorWriteMaskBlue  : 0) |
                 (uint)(k.pk.mask_a ? MTLColorWriteMaskAlpha : 0);
    if (!maskw) maskw = 0;              /* all-off keeps the pipeline created */
    d.colorAttachments[0].writeMask = (MTLColorWriteMask)maskw;
    d.depthAttachmentPixelFormat = t->depth ? glmcd_mtlfmt(k.depth_px)
                                            : MTLPixelFormatInvalid;

    /* vertex layouts per program (the C layer emits exactly these shapes)  */
    if (prog == GLMC_PROG_FFP || prog == GLMC_PROG_POST1) {
        /* 48-byte stride: F4@0 position (w=1), F4@16 colour, F4@32 texcoord */
        d.vertexDescriptor.attributes[0].format = MTLVertexFormatFloat4;
        d.vertexDescriptor.attributes[0].offset = 0;
        d.vertexDescriptor.attributes[0].bufferIndex = 30;
        d.vertexDescriptor.attributes[1].format = MTLVertexFormatFloat4;
        d.vertexDescriptor.attributes[1].offset = 16;
        d.vertexDescriptor.attributes[1].bufferIndex = 30;
        d.vertexDescriptor.attributes[2].format = MTLVertexFormatFloat4;
        d.vertexDescriptor.attributes[2].offset = 32;
        d.vertexDescriptor.attributes[2].bufferIndex = 30;
        d.vertexDescriptor.layouts[30].stride = 48;
    } else if (prog == GLMC_PROG_TC2_POLY) {
        /* 80-byte aligned: F4@0 pos, F4@16 uvs(+pad), F4@32 par, F4@48 clip,
         * F4@64 ext(+pad); the C layer repacks tc2's packed 68-byte layout  */
        d.vertexDescriptor.attributes[0].format = MTLVertexFormatFloat4;
        d.vertexDescriptor.attributes[0].offset = 0;
        d.vertexDescriptor.attributes[0].bufferIndex = 30;
        d.vertexDescriptor.attributes[1].format = MTLVertexFormatFloat4;
        d.vertexDescriptor.attributes[1].offset = 16;
        d.vertexDescriptor.attributes[1].bufferIndex = 30;
        d.vertexDescriptor.attributes[2].format = MTLVertexFormatFloat4;
        d.vertexDescriptor.attributes[2].offset = 32;
        d.vertexDescriptor.attributes[2].bufferIndex = 30;
        d.vertexDescriptor.attributes[3].format = MTLVertexFormatFloat4;
        d.vertexDescriptor.attributes[3].offset = 48;
        d.vertexDescriptor.attributes[3].bufferIndex = 30;
        d.vertexDescriptor.attributes[4].format = MTLVertexFormatFloat4;
        d.vertexDescriptor.attributes[4].offset = 64;
        d.vertexDescriptor.attributes[4].bufferIndex = 30;
        d.vertexDescriptor.layouts[30].stride = 80;
    } else {
        /* tc2 text / gamma: a_pos only, 16-byte stride */
        d.vertexDescriptor.attributes[0].format = MTLVertexFormatFloat4;
        d.vertexDescriptor.attributes[0].offset = 0;
        d.vertexDescriptor.attributes[0].bufferIndex = 30;
        d.vertexDescriptor.layouts[30].stride = 16;
    }

    NSError *err = nil;
    id<MTLRenderPipelineState> p = [f->dev newRenderPipelineStateWithDescriptor:d
                                                                        error:&err];
    if (getenv("GLMC_TRACE"))
        fprintf(stderr, "[trace] pipe lib=%d a0=%d,%d a1=%d,%d a2=%d,%d a3=%d,%d a4=%d,%d stride=%zu\n",
                lib, (int)d.vertexDescriptor.attributes[0].format, (int)d.vertexDescriptor.attributes[0].offset,
                (int)d.vertexDescriptor.attributes[1].format, (int)d.vertexDescriptor.attributes[1].offset,
                (int)d.vertexDescriptor.attributes[2].format, (int)d.vertexDescriptor.attributes[2].offset,
                (int)d.vertexDescriptor.attributes[3].format, (int)d.vertexDescriptor.attributes[3].offset,
                (int)d.vertexDescriptor.attributes[4].format, (int)d.vertexDescriptor.attributes[4].offset,
                (size_t)d.vertexDescriptor.layouts[30].stride);
    if (!p) {
        fprintf(stderr, "[glmc] pipeline %d failed: %s\n", lib,
                err.localizedDescription.UTF8String);
        glmc_dev_note("pipeline build failed");
        return nil;
    }
    f->ps[f->n_ps].k = k; f->ps[f->n_ps].p = p; f->n_ps++;
    return p;
}

static id<MTLDepthStencilState> glmcd_dss(glmcd_face *f, const glmcd_pipekey *pk)
{
    int key = pk->depth_on, write = pk->depth_write;
    for (int i = 0; i < f->n_ds; i++)
        if (f->ds[i].on == key && f->ds[i].func == (int)pk->depth_func &&
            f->ds[i].write == write) return f->ds[i].st;
    if (f->n_ds >= 16) { glmc_dev_note("dss cache full"); return nil; }

    MTLDepthStencilDescriptor *d = [MTLDepthStencilDescriptor new];
    d.depthCompareFunction = pk->depth_on ? glmcd_cmpf(pk->depth_func)
                                          : MTLCompareFunctionAlways;
    d.depthWriteEnabled = pk->depth_write ? YES : NO;
    id<MTLDepthStencilState> st = [f->dev newDepthStencilStateWithDescriptor:d];
    f->ds[f->n_ds].on = key; f->ds[f->n_ds].func = pk->depth_func;
    f->ds[f->n_ds].write = write; f->ds[f->n_ds].st = st; f->n_ds++;
    return st;
}

static id<MTLSamplerState> glmcd_sampler(glmcd_face *f, int minf, int magf, int wraps, int wrapt)
{
    for (int i = 0; i < f->n_smp; i++)
        if (f->smps[i].minf == minf && f->smps[i].magf == magf &&
            f->smps[i].wraps == wraps && f->smps[i].wrapt == wrapt)
            return f->smps[i].st;
    if (f->n_smp >= GLMC_SMP_MAX) { glmc_dev_note("sampler cache full"); return nil; }

    /* GL linear-sensing texture filters map to the nearest/linear pair; mip
     * level selection collapses to the base level (the games never generate
     * mipmaps for these textures) */
    int near_min = (minf == 0x2600 || minf == 0x2700 || minf == 0x2702);
    int near_mag = (magf == 0x2600);
    MTLSamplerMinMagFilter fmin = near_min ? MTLSamplerMinMagFilterNearest
                                           : MTLSamplerMinMagFilterLinear;
    MTLSamplerMinMagFilter fmag = near_mag ? MTLSamplerMinMagFilterNearest
                                           : MTLSamplerMinMagFilterLinear;
    MTLSamplerAddressMode a;
    switch (wraps) {
    case 0x2900:      a = MTLSamplerAddressModeClampToEdge; break;
    case 0x812F:      a = MTLSamplerAddressModeClampToEdge; break;
    case 0x8370:      a = MTLSamplerAddressModeMirrorClampToEdge; break;
    case 0x848F:      a = MTLSamplerAddressModeClampToBorderColor; break;
    default:          a = MTLSamplerAddressModeRepeat; break;
    }
    MTLSamplerAddressMode b;
    switch (wrapt) {
    case 0x2900:      b = MTLSamplerAddressModeClampToEdge; break;
    case 0x812F:      b = MTLSamplerAddressModeClampToEdge; break;
    case 0x8370:      b = MTLSamplerAddressModeMirrorClampToEdge; break;
    case 0x848F:      b = MTLSamplerAddressModeClampToBorderColor; break;
    default:          b = MTLSamplerAddressModeRepeat; break;
    }
    MTLSamplerDescriptor *d = [MTLSamplerDescriptor new];
    d.minFilter = fmin; d.magFilter = fmag;
    d.sAddressMode = a; d.tAddressMode = b;
    d.mipFilter = MTLSamplerMipFilterNotMipmapped;
    id<MTLSamplerState> st = [f->dev newSamplerStateWithDescriptor:d];
    f->smps[f->n_smp].minf = minf; f->smps[f->n_smp].magf = magf;
    f->smps[f->n_smp].wraps = wraps; f->smps[f->n_smp].wrapt = wrapt;
    f->smps[f->n_smp].st = st; f->n_smp++;
    return st;
}

static id<MTLTexture> glmcd_white_tex(glmcd_face *f)
{
    if (!f->white) {
        MTLTextureDescriptor *d = [MTLTextureDescriptor
            texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
            width:1 height:1 mipmapped:NO];
        d.usage = MTLTextureUsageShaderRead;
        d.storageMode = MTLStorageModeShared;
        f->white = [f->dev newTextureWithDescriptor:d];
        const uint8_t px[4] = { 0xFF, 0xFF, 0xFF, 0xFF };
        [f->white replaceRegion:MTLRegionMake2D(0, 0, 1, 1) mipmapLevel:0
                      withBytes:px bytesPerRow:4];
    }
    return f->white;
}

static id<MTLBuffer> glmcd_pbuf(glmcd_face *f, const void *data, NSUInteger len)
/* per-draw transient vertex/index buffer; released with its command buffer */
{
    id<MTLBuffer> b = [f->dev newBufferWithBytes:data length:len options:0];
    [f->pending addObject:b];
    return b;
}

/* ------------------------------------------------------------ the draw --- */
void glmc_dev_draw(glmcd_face *f, const glmcd_drawreq *req)
{
    struct glmcd_tgt t;
    glmcd_tgt_of(f, (uint32_t)f->cur_fbo, &t);
    if (getenv("GLMC_TRACE"))
        fprintf(stderr, "[trace] draw fbo=%d tgt=%dx%d prog=%d topo=%d n=%d ub=%d\n",
                f->cur_fbo, t.w, t.h, req->pk->prog, req->pk->topo,
                req->count, req->uniform_len);
    if (!t.color || !req->count) return;
    glmcd_pass_open(f, MTLLoadActionLoad);
    if (!f->enc) return;

    const glmcd_pipekey *pk = req->pk;
    id<MTLRenderPipelineState> p = glmcd_pipeline(f, pk, &t);
    if (!p) return;
    [f->enc setRenderPipelineState:p];
    [f->enc setDepthStencilState:glmcd_dss(f, pk)];

    /* viewport (GL rect) -> Metal framebuffer rect; clamp to the target     */
    int mx, my, mw, mh;
    glmcd_mem_rect(f, &t, f->vpx, f->vpy, f->vpw, f->vph, &mx, &my, &mw, &mh);
    if (mw <= 0 || mh <= 0) return;
    MTLViewport vp;
    vp.originX = mx; vp.originY = (double)my;
    vp.width = (double)mw; vp.height = (double)mh; vp.znear = 0.0; vp.zfar = 1.0;
    [f->enc setViewport:vp];

    if (pk->scissor && f->sc_on) {
        int sx2, sy2, sw2, sh2;
        glmcd_mem_rect(f, &t, f->scx, f->scy, f->scw, f->sch, &sx2, &sy2, &sw2, &sh2);
        if (sw2 > 0 && sh2 > 0) {
            MTLScissorRect sr;
            sr.x = (NSUInteger)sx2; sr.y = (NSUInteger)sy2;
            sr.width = (NSUInteger)sw2; sr.height = (NSUInteger)sh2;
            [f->enc setScissorRect:sr];
        } else return;                  /* fully clipped: nothing to draw    */
    } else {
        MTLScissorRect sr;
        sr.x = 0; sr.y = 0; sr.width = (NSUInteger)t.w; sr.height = (NSUInteger)t.h;
        [f->enc setScissorRect:sr];
    }

    /* uniforms: the program's struct bytes, both stages (vertex uses uniform
     * block 0 only in the FFP path; the others ignore it)                    */
    if (req->uniform_len > 0 && req->uniforms) {
        [f->enc setVertexBytes:req->uniforms length:(NSUInteger)req->uniform_len atIndex:0];
        [f->enc setFragmentBytes:req->uniforms length:(NSUInteger)req->uniform_len atIndex:0];
        if (getenv("GLMC_TRACE") && req->uniform_len >= 208) {
            const int *e = (const int *)((const uint8_t *)req->uniforms + 96);
            fprintf(stderr, "[trace] uni e0..7: %d %d %d %d %d %d %d %d\n", e[0], e[1], e[2], e[3], e[4], e[5], e[6], e[7]);
            fprintf(stderr, "[trace] uni e16..27: %d %d %d %d %d %d %d %d %d %d %d %d\n",
                    e[16], e[17], e[18], e[19], e[20], e[21], e[22], e[23], e[24], e[25], e[26], e[27]);
        }
    }
    if (getenv("GLMC_TRACE") && req->verts && req->vert_bytes >= 16) {
        const float *vv = (const float *)req->verts;
        fprintf(stderr, "[trace] verts v0: %.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f\n",
                vv[0], vv[1], vv[2], vv[3], vv[4], vv[5], vv[6], vv[7], vv[8], vv[9], vv[10], vv[11]);
        fprintf(stderr, "[trace] verts v1: %.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f\n",
                vv[12], vv[13], vv[14], vv[15], vv[16], vv[17], vv[18], vv[19], vv[20], vv[21], vv[22], vv[23]);
    }

    /* texture nodes: samp[k] feeds MSL texture(k) / sampler(k)              */
    for (int k = 0; k < req->nsamp && k < 8; k++) {
        uint32_t tid = req->samp[k].tex;
        id<MTLTexture> tex = (tid >= 1 && tid <= GLMC_TEX_MAX) ? f->tex[tid - 1].t
                                                               : f->white;
        if (!tex) tex = glmcd_white_tex(f);
        if (getenv("GLMC_TRACE"))
            fprintf(stderr, "[trace] bind tex face=%p slot=%d tid=%u -> %p (%zux%zu fmt=%#lx)\n",
                    (void *)f, k, tid, (__bridge void *)tex,
                    tex ? (size_t)tex.width : (size_t)0,
                    tex ? (size_t)tex.height : (size_t)0,
                    tex ? (unsigned long)tex.pixelFormat : 0UL);
        if (getenv("GLMC_DEMO")) {   /* control: force-bind a blue 2x2 */
            MTLTextureDescriptor *dd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:2 height:2 mipmapped:NO];
            dd.storageMode = MTLStorageModeShared;
            id<MTLTexture> bleu = [f->dev newTextureWithDescriptor:dd];
            const uint8_t bl[16] = { 0,0,255,255, 0,0,255,255, 0,0,255,255, 0,0,255,255 };
            [bleu replaceRegion:MTLRegionMake2D(0,0,2,2) mipmapLevel:0 withBytes:bl bytesPerRow:8];
            [f->enc setFragmentTexture:bleu atIndex:(NSUInteger)k];
            fprintf(stderr, "[trace] DEMO blue bound at slot %d instead of %p\n", k, (__bridge void *)tex);
        } else {
        [f->enc setFragmentTexture:tex atIndex:(NSUInteger)k];
        }
        id<MTLSamplerState> st = glmcd_sampler(f, req->samp[k].minf, req->samp[k].magf,
                                               req->samp[k].wraps, req->samp[k].wrapt);
        [f->enc setFragmentSamplerState:st atIndex:(NSUInteger)k];
    }

    MTLPrimitiveType topo = MTLPrimitiveTypeTriangle;
    if (pk->topo == GLMC_TOPO_LINE_LIST) topo = MTLPrimitiveTypeLine;
    {
        id<MTLBuffer> vb = glmcd_pbuf(f, req->verts, (NSUInteger)req->vert_bytes);
        [f->enc setVertexBuffer:vb offset:0 atIndex:30];
        if (req->idx) {
            id<MTLBuffer> ib = glmcd_pbuf(f, req->idx, (NSUInteger)req->idx_bytes);
            [f->enc drawIndexedPrimitives:topo indexCount:(NSUInteger)req->count
                                indexType:MTLIndexTypeUInt16 indexBuffer:ib
                                indexBufferOffset:(NSUInteger)req->first * 2];
        } else
            [f->enc drawPrimitives:topo vertexStart:(NSUInteger)req->first
                       vertexCount:(NSUInteger)req->count];
    }
}

/* CHUNK3END */

/* ------------------------------------------------- commit / completion --- */
static void glmcd_commit(glmcd_face *f)
{
    if (!f->cb) return;
    glmcd_pass_close(f);
    id<MTLCommandBuffer> cb = f->cb;
    f->cb = nil;
    /* buffers used by THIS buffer only are released when it completes     */
    if (f->pending.count) {
        NSArray *drained = [f->pending copy];
        [f->pending removeAllObjects];
        [cb addCompletedHandler:^(id<MTLCommandBuffer>) { (void)drained; }];
    }
    [cb commit];
}
static void glmcd_finish(glmcd_face *f, int wait)
{
    if (!f->cb) return;                         /* nothing in flight       */
    id<MTLCommandBuffer> cb = f->cb;
    glmcd_commit(f);
    if (wait) [cb waitUntilCompleted];
}

/* -------------------------------------------------- GPU-GPU copy (tex) --- */
void glmc_dev_copy_tex(glmcd_face *f, uint32_t dst_tex, int dst_fmt,
                       int dx, int dy, int sx, int sy, int w, int h)
{
    if (getenv("GLMC_TRACE")) fprintf(stderr,"[E] copytex\n");
/* glCopyTexSubImage2D: from the READ framebuffer's colour image into the
 * texture. y is GL-row space in both spaces; our GL-memory-order target keeps
 * texel rows = GL rows, so texel indices copy 1:1 (no flip). */
    struct glmcd_tgt t; glmcd_tgt_of(f, (uint32_t)f->read_fbo, &t);
    if (!t.color || dst_tex < 1 || dst_tex > GLMC_TEX_MAX) {
        glmc_dev_note("copy from an unresolved read target");
        return;
    }
    struct glmcd_tex *dst = &f->tex[dst_tex - 1];
    if (!dst->t) { glmc_dev_note("copy into a texture without storage"); return; }
    (void)dst_fmt;
    glmcd_pass_close(f);                        /* blit work needs no pass */
    if (!f->cb) f->cb = [f->q commandBuffer];

    /* overlap-safe: route through a temporary region texture               */
    MTLTextureDescriptor *td = [MTLTextureDescriptor
        texture2DDescriptorWithPixelFormat:t.color.pixelFormat
        width:(NSUInteger)w height:(NSUInteger)h mipmapped:NO];
    td.usage = MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget;
    td.storageMode = MTLStorageModePrivate;
    id<MTLTexture> tmp = [f->dev newTextureWithDescriptor:td];

    id<MTLBlitCommandEncoder> be = [f->cb blitCommandEncoder];
    [be copyFromTexture:t.color sourceSlice:0 sourceLevel:0
               sourceOrigin:MTLOriginMake((NSUInteger)sx, (NSUInteger)sy, 0)
               sourceSize:MTLSizeMake((NSUInteger)w, (NSUInteger)h, 1)
                 toTexture:tmp destinationSlice:0 destinationLevel:0
              destinationOrigin:MTLOriginMake(0, 0, 0)];
    [be copyFromTexture:tmp sourceSlice:0 sourceLevel:0
               sourceOrigin:MTLOriginMake(0, 0, 0)
               sourceSize:MTLSizeMake((NSUInteger)w, (NSUInteger)h, 1)
                 toTexture:dst->t destinationSlice:0 destinationLevel:0
              destinationOrigin:MTLOriginMake((NSUInteger)dx, (NSUInteger)dy, 0)];
    [be endEncoding];
}

/* GL-style blit support --------------------------------------------------- */
static id<MTLRenderPipelineState> glmcd_blit_pipe(glmcd_face *f, int px)
{
    for (int i = 0; i < f->n_blit; i++)
        if (f->blit[i].px == px) return f->blit[i].p;
    if (f->n_blit >= 8) { glmc_dev_note("blit pipeline cache full"); return nil; }
    id<MTLLibrary> l = glmcd_lib(f, 6);
    if (!l) return nil;
    MTLRenderPipelineDescriptor *d = [MTLRenderPipelineDescriptor new];
    d.vertexFunction   = [l newFunctionWithName:@"glmc_blit_v"];
    d.fragmentFunction = [l newFunctionWithName:@"glmc_blit_f"];
    d.colorAttachments[0].pixelFormat = glmcd_mtlfmt(px);
    d.colorAttachments[0].writeMask = MTLColorWriteMaskAll;
    d.vertexDescriptor = nil;
    NSError *err = nil;
    id<MTLRenderPipelineState> p = [f->dev newRenderPipelineStateWithDescriptor:d
                                                                        error:&err];
    if (!p) { fprintf(stderr, "[glmc] blit pipeline failed: %s\n",
                      err.localizedDescription.UTF8String); return nil; }
    f->blit[f->n_blit].px = px; f->blit[f->n_blit].p = p; f->n_blit++;
    return p;
}

/* the MSL blitu uniform, mirrored here to pass raw bytes                   */
struct glmcd_blitu { float sc[4]; float sz[4]; };

void glmc_dev_blit(glmcd_face *f, int sx0, int sy0, int sx1, int sy1,
                   int dx0, int dy0, int dx1, int dy1, int linear)
{
    if (getenv("GLMC_TRACE")) fprintf(stderr,"[E] blit\n");
/* glBlitFramebuffer (colour only). src = the READ binding's colour image,
 * dst = the CURRENT draw binding. Both GL-row encoded, so the copy keeps
 * rows: the blit program does not negate NDC y (the present shares it and
 * writes top-down drawable rows), so the source is sampled from its top. */
    struct glmcd_tgt s; glmcd_tgt_of(f, (uint32_t)f->read_fbo, &s);
    if (!s.color) { glmc_dev_note("blit from an unresolved read target"); return; }
    struct glmcd_tgt d; glmcd_tgt_of(f, (uint32_t)f->cur_fbo, &d);
    if (!d.color) return;

    int w = sx1 - sx0, h = sy1 - sy0;
    int dw = dx1 - dx0, dh = dy1 - dy0;
    if (w <= 0 || h <= 0 || dw <= 0 || dh <= 0) return;
    (void)dw; (void)dh;

    /* close any open pass; give the blit its own targeted pass             */
    glmcd_pass_close(f);
    if (!f->cb) f->cb = [f->q commandBuffer];
    MTLRenderPassDescriptor *d_ = [MTLRenderPassDescriptor renderPassDescriptor];
    d_.colorAttachments[0].texture = d.color;
    d_.colorAttachments[0].loadAction = MTLLoadActionLoad;
    d_.colorAttachments[0].storeAction = MTLStoreActionStore;
    if (d.depth) {
        d_.depthAttachment.texture = d.depth;
        d_.depthAttachment.loadAction = MTLLoadActionLoad;
        d_.depthAttachment.storeAction = MTLStoreActionStore;
    }
    id<MTLRenderCommandEncoder> enc = [f->cb renderCommandEncoderWithDescriptor:d_];

    id<MTLRenderPipelineState> p = glmcd_blit_pipe(f, d.color_px);
    if (!p) { [enc endEncoding]; return; }
    [enc setRenderPipelineState:p];

    /* dst viewport: GL rect -> Metal framebuffer rect                      */
    int mx, my, mw, mh;
    glmcd_mem_rect(f, &d, dx0, dy0, dw, dh, &mx, &my, &mw, &mh);
    if (mw <= 0 || mh <= 0) { [enc endEncoding]; return; }
    MTLViewport vp;
    vp.originX = (double)mx; vp.originY = (double)my;
    vp.width = (double)mw; vp.height = (double)mh; vp.znear = 0.0; vp.zfar = 1.0;
    [enc setViewport:vp];
    MTLScissorRect sr;
    sr.x = (NSUInteger)mx; sr.y = (NSUInteger)my;
    sr.width = (NSUInteger)mw; sr.height = (NSUInteger)mh;
    [enc setScissorRect:sr];

    /* the blit quad: src rect in texels (GL row space, positive height)     */
    struct glmcd_blitu u;
    u.sc[0] = (float)sx0; u.sc[1] = (float)(sy0 + h); u.sc[2] = (float)w; u.sc[3] = (float)-h;
    u.sz[0] = 0.0f; u.sz[1] = 0.0f; u.sz[2] = (float)s.w; u.sz[3] = (float)s.h;

    [enc setVertexBytes:&u length:sizeof u atIndex:0];
    [enc setFragmentTexture:s.color atIndex:0];
    id<MTLSamplerState> st = glmcd_sampler(f, linear ? 0x2601 : 0x2600,
                                               linear ? 0x2601 : 0x2600,
                                               0x2900, 0x2900);
    [enc setFragmentSamplerState:st atIndex:0];
    [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:6];
    [enc endEncoding];
}

/* ------------------------------------------------------------ readback --- */
void glmc_dev_read(glmcd_face *f, int x, int y, int w, int h, void *out, int out_fmt)
{
    if (getenv("GLMC_TRACE")) fprintf(stderr,"[E] read fmt=%d\n",out_fmt);
    struct glmcd_tgt t; glmcd_tgt_of(f, (uint32_t)f->read_fbo, &t);
    if (!t.color) { glmc_dev_note("read from an unresolved target"); return; }
    if (w <= 0 || h <= 0) return;
    glmcd_finish(f, 1);                         /* land + wait for content */

    /* This machine's GPU driver faults inside getBytes on a Private texture
     * (reproduces in a standalone 20-line probe on the M5 Pro), so readback
     * goes through a Shared/Managed staging copy of the requested region.  */
    MTLPixelFormat pf = t.color.pixelFormat;
    MTLTextureDescriptor *sd = [MTLTextureDescriptor
        texture2DDescriptorWithPixelFormat:pf width:(NSUInteger)w height:(NSUInteger)h
        mipmapped:NO];
    sd.usage = MTLTextureUsageShaderRead;
    sd.storageMode = MTLStorageModeShared;
    id<MTLTexture> stg = [f->dev newTextureWithDescriptor:sd];
    if (stg) {
        glmcd_pass_close(f);
        if (!f->cb) f->cb = [f->q commandBuffer];
        id<MTLBlitCommandEncoder> be = [f->cb blitCommandEncoder];
        [be copyFromTexture:t.color sourceSlice:0 sourceLevel:0
               sourceOrigin:MTLOriginMake((NSUInteger)x, (NSUInteger)y, 0)
                sourceSize:MTLSizeMake((NSUInteger)w, (NSUInteger)h, 1)
                  toTexture:stg destinationSlice:0 destinationLevel:0
           destinationOrigin:MTLOriginMake(0, 0, 0)];
        [be endEncoding];
        glmcd_finish(f, 1);                     /* stage + wait            */
    } else {
        glmc_dev_note("readback staging texture failed");
        return;
    }

    int nbytes = (pf == MTLPixelFormatR8Unorm) ? 1 : (pf == MTLPixelFormatRG8Unorm) ? 2 : 4;
    NSUInteger row = (NSUInteger)((w * nbytes + 15) & ~15);
    if (row < (NSUInteger)(w * nbytes)) row = (NSUInteger)(w * nbytes);
    uint8_t *tmp = (uint8_t *)malloc(row * (NSUInteger)h);
    if (!tmp) return;
    [stg getBytes:tmp bytesPerRow:row bytesPerImage:0
           fromRegion:MTLRegionMake2D(0, 0, (NSUInteger)w, (NSUInteger)h)
           mipmapLevel:0 slice:0];
    /* GL rows == Metal texel rows under this port's conventions; BGRA-aware
     * channel order when the source stores that way.                        */
    uint8_t *o = (uint8_t *)out;
    int bpp = glmcd_bpp(out_fmt);
    int swz = t.color_px == GLMC_PX_BGRA8;
    for (int j = 0; j < h; j++) {
        const uint8_t *s = tmp + row * (NSUInteger)j;
        uint8_t *drow = o + (NSUInteger)(w * bpp) * (NSUInteger)j;
        for (int i = 0; i < w; i++, drow += bpp) {
            uint8_t r, g, b, a;
            if (nbytes == 1) { r = g = b = s[0]; a = 255; s += 1; }
            else if (nbytes == 2) { r = g = s[0]; b = 0; a = s[1]; s += 2; }
            else { r = s[0]; g = s[1]; b = s[2]; a = s[3]; s += 4; }
            if (swz) { uint8_t t8 = r; r = b; b = t8; }
            switch (out_fmt) {
            case GLMC_PX_RGBA8:
                drow[0] = r; drow[1] = g; drow[2] = b; drow[3] = a; break;
            case GLMC_PX_RGB8:
                drow[0] = r; drow[1] = g; drow[2] = b; break;
            case GLMC_PX_R8:   /* GL_LUMINANCE readback: L = the red channel */
                drow[0] = r; break;
            default:
                drow[0] = r; drow[1] = g; drow[2] = b; drow[3] = a; break;
            }
        }
    }
    free(tmp);
}

/* ------------------------------------------------------------ present ---- */
void glmc_dev_present(glmcd_face *f)
{
    if (getenv("GLMC_TRACE")) fprintf(stderr,"[E] present\n");
    if (!f->layer) { glmcd_finish(f, 0); return; }
    /* flush everything drawn so far; each command buffer presents its own
     * drawable, so the frame's ground work commits before our present pass */
    glmcd_commit(f);

    id<CAMetalDrawable> drw = [f->layer nextDrawable];
    if (!drw) { glmc_dev_note("no drawable available for present"); return; }

    /* the present pass: blit the default target onto the drawable 1:1      */
    struct glmcd_tgt t; glmcd_tgt_of(f, 0, &t);
    if (!t.color) return;
    f->cb = [f->q commandBuffer];
    MTLRenderPassDescriptor *d = [MTLRenderPassDescriptor renderPassDescriptor];
    d.colorAttachments[0].texture = drw.texture;
    d.colorAttachments[0].loadAction = MTLLoadActionDontCare;   /* full quad */
    d.colorAttachments[0].storeAction = MTLStoreActionStore;
    id<MTLRenderCommandEncoder> enc = [f->cb renderCommandEncoderWithDescriptor:d];
    id<MTLRenderPipelineState> p = glmcd_blit_pipe(f, GLMC_PX_BGRA8);
    if (!p) { [enc endEncoding]; return; }
    [enc setRenderPipelineState:p];
    MTLViewport vp;
    vp.originX = 0.0; vp.originY = 0.0;
    vp.width = (double)drw.texture.width; vp.height = (double)drw.texture.height;
    vp.znear = 0.0; vp.zfar = 1.0;
    [enc setViewport:vp];
    MTLScissorRect sr;
    sr.x = 0; sr.y = 0; sr.width = drw.texture.width; sr.height = drw.texture.height;
    [enc setScissorRect:sr];
    struct glmcd_blitu u;
    /* The 60 Hz window test confirms the positive-height presentation is
     * upright. High-refresh repeated frames need separate verification. */
    u.sc[0] = 0; u.sc[1] = 0; u.sc[2] = (float)t.w; u.sc[3] = (float)t.h;
    u.sz[0] = 0; u.sz[1] = 0; u.sz[2] = (float)t.w; u.sz[3] = (float)t.h;
    [enc setVertexBytes:&u length:sizeof u atIndex:0];
    [enc setFragmentTexture:t.color atIndex:0];
    id<MTLSamplerState> st = glmcd_sampler(f, 0x2601, 0x2601, 0x2900, 0x2900);
    [enc setFragmentSamplerState:st atIndex:0];
    [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:6];
    [enc endEncoding];

    [f->cb presentDrawable:drw];       /* schedule the drawable BEFORE commit */
    [f->cb commit];
    f->cb = [f->q commandBuffer];
}

void glmc_dev_flush(glmcd_face *f)      /* SDL SwapWindow before present   */
{
    if (getenv("GLMC_TRACE")) fprintf(stderr,"[E] flush\n");
    glmcd_commit(f);
}

void glmc_dev_window_size(glmcd_face *f, int *w, int *h)
{
    if (getenv("GLMC_TRACE")) fprintf(stderr,"[E] winsize\n");
    /* report the drawable's size, but leave the face's own record alone: the
     * resize check at the next swap compares against it, and updating it here
     * made that check miss the change (TC2's default target stayed at the
     * start-up window size, the picture drawn shifted and stretched) */
    *w = f->win_w; *h = f->win_h;
    if (f->layer) {
        int dw = (int)f->layer.drawableSize.width;
        int dh = (int)f->layer.drawableSize.height;
        if (dw > 0 && dh > 0) { *w = dw; *h = dh; }
    }
}

/* ------------------------------------------------------------ SDL glue --- */
void *glmc_sdl_metal_view(void *sdl_window)
{
    return SDL_Metal_CreateView((SDL_Window *)sdl_window);
}
void *glmc_sdl_metal_layer(void *metal_view)
{
    return (void *)SDL_Metal_GetLayer((SDL_MetalView)metal_view);
}
void glmc_sdl_metal_destroy(void *metal_view)
{
    SDL_Metal_DestroyView((SDL_MetalView)metal_view);
}
void glmc_sdl_metal_size(void *sdl_window_or_layer, int *w, int *h)
{
    if (!sdl_window_or_layer) { *w = 0; *h = 0; return; }
    SDL_Metal_GetDrawableSize((SDL_Window *)sdl_window_or_layer, w, h);
}

/* ------------------------------------------- the screen's refresh -- */
/* 60 Hz game frames on a 144 Hz screen alternate between 2 and 3 refreshes
 * (a judder), and 120 pictures/s cannot be paced on it at all. The window's
 * display therefore switches to the fastest refresh at the same
 * resolution that the cap divides (144 -> 120 Hz), while the game fills the
 * screen (fullscreen, or the window that fills the desktop):
 * kCGConfigureForAppOnly makes macOS restore it when the process exits, and
 * a sized window restores it at once. As in the Thief Gold port. */
static bool glmcd_refresh_switched;

static CGDirectDisplayID glmcd_window_display(SDL_Window *w)
{
    int x = 0, y = 0, ww = 0, wh = 0;
    SDL_GetWindowPosition(w, &x, &y);
    SDL_GetWindowSize(w, &ww, &wh);
    CGDirectDisplayID id = kCGNullDirectDisplay; uint32_t n = 0;
    if (CGGetDisplaysWithPoint(CGPointMake(x + ww / 2, y + wh / 2), 1, &id, &n) != kCGErrorSuccess || !n)
        return CGMainDisplayID();
    return id;
}

static long glmcd_hz(CGDisplayModeRef m) { return m ? lround(CGDisplayModeGetRefreshRate(m)) : 0; }

/* the mode at the current mode's size whose refresh is the fastest multiple of cap (retained), or NULL */
static CGDisplayModeRef glmcd_mode_for_cap(CGDirectDisplayID d, CGDisplayModeRef cur, int cap)
{
    const void *keys[] = { kCGDisplayShowDuplicateLowResolutionModes };
    const void *vals[] = { kCFBooleanTrue };
    CFDictionaryRef opt = CFDictionaryCreate(NULL, keys, vals, 1, &kCFTypeDictionaryKeyCallBacks,
                                             &kCFTypeDictionaryValueCallBacks);
    CFArrayRef modes = CGDisplayCopyAllDisplayModes(d, opt);
    CFRelease(opt);
    if (!modes) return NULL;
    CGDisplayModeRef best = NULL; long best_hz = 0;
    for (CFIndex i = 0; i < CFArrayGetCount(modes); i++) {
        CGDisplayModeRef m = (CGDisplayModeRef)CFArrayGetValueAtIndex(modes, i);
        const long hz = glmcd_hz(m);
        if (CGDisplayModeGetWidth(m) != CGDisplayModeGetWidth(cur) ||
            CGDisplayModeGetHeight(m) != CGDisplayModeGetHeight(cur) ||
            CGDisplayModeGetPixelWidth(m) != CGDisplayModeGetPixelWidth(cur) ||
            CGDisplayModeGetPixelHeight(m) != CGDisplayModeGetPixelHeight(cur) ||
            !CGDisplayModeIsUsableForDesktopGUI(m) || hz <= 0 || hz % cap || hz <= best_hz)
            continue;
        best = m; best_hz = hz;
    }
    if (best) CGDisplayModeRetain(best);
    CFRelease(modes);
    return best;
}

int glmc_display_match_refresh(void *sdl_window, int on, int cap)
{
    SDL_Window *w = (SDL_Window *)sdl_window;
    if (!w || getenv("GLMC_KEEP_REFRESH")) return 0;
    if (!on && glmcd_refresh_switched) {
        CGRestorePermanentDisplayConfiguration();
        glmcd_refresh_switched = false;
        fprintf(stderr, "[DISPLAY] refresh restored (the game no longer fills the screen)\n");
    }
    const CGDirectDisplayID d = glmcd_window_display(w);
    CGDisplayModeRef cur = CGDisplayCopyDisplayMode(d);
    if (!cur) return 0;
    long hz = glmcd_hz(cur);
    if (on && cap > 0 && hz > 0 && hz % cap) {
        CGDisplayModeRef m = glmcd_mode_for_cap(d, cur, cap);
        CGDisplayConfigRef cfg;
        if (!m) fprintf(stderr, "[DISPLAY] %ld Hz: this display has no mode at a multiple of %d Hz\n", hz, cap);
        else if (CGBeginDisplayConfiguration(&cfg) == kCGErrorSuccess) {
            CGConfigureDisplayWithDisplayMode(cfg, d, m, NULL);
            const CGError r = CGCompleteDisplayConfiguration(cfg, kCGConfigureForAppOnly);
            fprintf(stderr, "[DISPLAY] %ld Hz to %ld Hz while the game runs (%s)\n", hz, glmcd_hz(m),
                    r == kCGErrorSuccess ? "done" : "failed");
            if (r == kCGErrorSuccess) { glmcd_refresh_switched = true; hz = glmcd_hz(m); }
        }
        if (m) CGDisplayModeRelease(m);
    }
    CGDisplayModeRelease(cur);
    return (int)hz;
}

/* CHUNK4END */




/* THE POINTER IN PLAY: SDL hides it through the window's cursor rect, which left the arrow showing over a light-gun
 * game's crosshair. The system's own hide is used as well -- only while the pointer is over our window and the window has the keyboard, so the
 * arrow is there again over the Dock, the menu bar or another app; hide = 0 (the menu, a pause) shows it. Called every frame. */
extern "C" void glmc_pointer_hide(void *sdl_window, int hide)
{
    static int hidden;
    SDL_Window *w = (SDL_Window *)sdl_window;
    if (hide && w) {
        float gx = 0, gy = 0; int wx = 0, wy = 0, ww = 0, wh = 0;
        int ix, iy; SDL_GetGlobalMouseState(&ix, &iy); gx = (float)ix; gy = (float)iy;
        SDL_GetWindowPosition(w, &wx, &wy); SDL_GetWindowSize(w, &ww, &wh);
        hide = (SDL_GetWindowFlags(w) & SDL_WINDOW_INPUT_FOCUS) && gx >= wx && gy >= wy && gx < wx + ww && gy < wy + wh;
    }
    if (hide && !hidden) { [NSCursor hide]; hidden = 1; }
    else if (!hide && hidden) { [NSCursor unhide]; hidden = 0; }
}
