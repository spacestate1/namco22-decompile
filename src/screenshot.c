/*
 * screenshot.c -- live-framebuffer PNG capture (see include/screenshot.h).
 *
 * PNG rather than PPM because these accumulate: a 1280x960 frame is ~3.5 MB
 * as PPM and ~100 KB deflated, and a PNG can be viewed or attached without a
 * conversion step.  zlib is the only new dependency and it was already in the
 * link closure.
 */
#include "screenshot.h"
#include "fog_hw.h"

#include <GL/gl.h>
#include <zlib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>

static void put32be(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}

/* One PNG chunk: length, type, data, CRC32 over (type + data). */
static int chunk(FILE *f, const char *type, const uint8_t *data, size_t len)
{
    uint8_t hdr[4];
    put32be(hdr, (uint32_t)len);
    if (fwrite(hdr, 1, 4, f) != 4) return 0;
    if (fwrite(type, 1, 4, f) != 4) return 0;
    if (len && fwrite(data, 1, len, f) != len) return 0;

    uLong c = crc32(0, (const Bytef *)type, 4);
    if (len) c = crc32(c, (const Bytef *)data, (uInt)len);
    put32be(hdr, (uint32_t)c);
    return fwrite(hdr, 1, 4, f) == 4;
}

/* rgb is top-down, 3 bytes per pixel, w*h*3 long. */
static int write_png(const char *path, const uint8_t *rgb, int w, int h)
{
    /* Raw PNG scanlines carry a leading filter byte; 0 = None.  Filtering
     * would compress better but costs code for no benefit at this size. */
    size_t raw_len = (size_t)h * (1 + (size_t)w * 3);
    uint8_t *raw = malloc(raw_len);
    if (!raw) return 0;
    for (int y = 0; y < h; y++) {
        uint8_t *dst = raw + (size_t)y * (1 + (size_t)w * 3);
        *dst++ = 0;
        memcpy(dst, rgb + (size_t)y * (size_t)w * 3, (size_t)w * 3);
    }

    uLongf zlen = compressBound((uLong)raw_len);
    uint8_t *z = malloc(zlen);
    if (!z) { free(raw); return 0; }
    int zr = compress2(z, &zlen, raw, (uLong)raw_len, 6);
    free(raw);
    if (zr != Z_OK) { free(z); return 0; }

    FILE *f = fopen(path, "wb");
    if (!f) { free(z); return 0; }

    static const uint8_t sig[8] = { 0x89,'P','N','G',0x0D,0x0A,0x1A,0x0A };
    uint8_t ihdr[13];
    put32be(ihdr, (uint32_t)w);
    put32be(ihdr + 4, (uint32_t)h);
    ihdr[8]  = 8;   /* bit depth   */
    ihdr[9]  = 2;   /* colour type 2 = truecolour RGB */
    ihdr[10] = 0;   /* deflate     */
    ihdr[11] = 0;   /* filter      */
    ihdr[12] = 0;   /* no interlace*/

    int ok = fwrite(sig, 1, 8, f) == 8
          && chunk(f, "IHDR", ihdr, sizeof ihdr)
          && chunk(f, "IDAT", z, zlen)
          && chunk(f, "IEND", NULL, 0);

    fclose(f);
    free(z);
    if (!ok) remove(path);
    return ok;
}

const char *screenshot_capture(SDL_Window *win, const char *dir, unsigned frame)
{
    static char path[512];

    int w = 0, h = 0;
    SDL_GL_GetDrawableSize(win, &w, &h);
    if (w <= 0 || h <= 0) {
        fprintf(stderr, "[shot] drawable is %dx%d, nothing to capture\n", w, h);
        return NULL;
    }

    uint8_t *px = malloc((size_t)w * (size_t)h * 3);
    if (!px) { fprintf(stderr, "[shot] out of memory for %dx%d\n", w, h); return NULL; }

    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, w, h, GL_RGB, GL_UNSIGNED_BYTE, px);

    /* Final-stage gamma, the last step of the reference pixel chain
     * (FAILED_APPROACHES.md 1.19).  The window itself is NOT gamma-corrected
     * -- save_screenshot() applies this on read-back too -- so a capture is
     * deliberately closer to hardware than what is on screen. */
    if (g_fog_valid && g_fog.have_gamma)
        for (long i = 0; i < (long)w * h; i++)
            fog_apply_gamma(&px[i*3], &px[i*3+1], &px[i*3+2]);

    /* GL reads bottom-up; PNG wants top-down. */
    uint8_t *flip = malloc((size_t)w * (size_t)h * 3);
    if (!flip) { free(px); fprintf(stderr, "[shot] out of memory\n"); return NULL; }
    for (int y = 0; y < h; y++)
        memcpy(flip + (size_t)y * w * 3,
               px + (size_t)(h - 1 - y) * w * 3, (size_t)w * 3);
    free(px);

    mkdir(dir, 0755);   /* EEXIST is the normal case and is fine */

    time_t t = time(NULL);
    struct tm tmv;
    localtime_r(&t, &tmv);
    char stamp[32];
    strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", &tmv);

    /* Timestamp AND frame, so repeated captures in one second and repeated
     * runs at the same frame both stay distinct. */
    snprintf(path, sizeof path, "%s/shot_%s_f%06u.png", dir, stamp, frame);
    for (int n = 1; n < 100; n++) {
        FILE *probe = fopen(path, "rb");
        if (!probe) break;
        fclose(probe);
        snprintf(path, sizeof path, "%s/shot_%s_f%06u_%d.png", dir, stamp, frame, n);
    }

    if (!write_png(path, flip, w, h)) {
        free(flip);
        fprintf(stderr, "[shot] failed to write %s\n", path);
        return NULL;
    }
    free(flip);
    printf("[shot] %s (%dx%d)\n", path, w, h);
    fflush(stdout);
    return path;
}
