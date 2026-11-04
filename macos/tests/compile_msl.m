/* macos/tests/compile_msl.m — compiles every MSL translation unit the
 * glshim device layer (glmc_metal.mm) builds, through the same Metal runtime
 * path the games will use. Exits 0 when all programs compile.
 *   build:  clang -fobjc-arc -fmodules -o compile_msl compile_msl.m \
 *               -I../glshim -framework Metal -framework Foundation
 * run:    ./compile_msl [build-dir]   (build-dir is where logs go) */
#include <stdio.h>
#include <string.h>
#import <Metal/Metal.h>
#import <Foundation/Foundation.h>
#import <Cocoa/Cocoa.h>

#include "glshim/glmc_msl.h"

static BOOL compile_one(id<MTLDevice> dev, const char *name, const char *flat[8], int n, NSString *dir)
{
    NSMutableString *src = [NSMutableString string];
    for (int i = 1; i < n; i++) {
        NSString *part = [NSString stringWithUTF8String:flat[i]];
        /* trim trailing blank lines the way the .mm does */
        NSRange r = [part rangeOfString:@"\n" options:NSBackwardsSearch];
        while (r.length && r.location == part.length - 1) {
            part = [part substringToIndex:r.location];
            r = [part rangeOfString:@"\n" options:NSBackwardsSearch];
        }
        [src appendFormat:@"%@\n", part];
    }
    NSError *err = nil;
    id<MTLLibrary> lib = [dev newLibraryWithSource:src options:nil error:&err];
    if (!lib) {
        fprintf(stderr, "MSL FAIL %s: %s\n", name, err.localizedDescription.UTF8String);
        NSString *log = dir ? [dir stringByAppendingPathComponent:[NSString stringWithFormat:@"%s.log", name]] : nil;
        if (log) [src writeToFile:log atomically:YES];
        return NO;
    }
    fprintf(stderr, "MSL ok %s (%lu functions)\n", name, (unsigned long)lib.functionNames.count);
    return YES;
}

int main(int argc, char **argv)
{
    @autoreleasepool {
        NSArray<id<MTLDevice>> * devs = MTLCopyAllDevices();
        if (!devs.count) { fprintf(stderr, "no Metal device\n"); return 1; }
        id<MTLDevice> dev = devs[0];
        fprintf(stderr, "device: %s\n", dev.name.UTF8String);
        NSString *dir = argc > 1 ? [NSString stringWithUTF8String:argv[1]]
                                 : [NSTemporaryDirectory() stringByAppendingString:@"glmc_msl"];
        [[NSFileManager defaultManager] createDirectoryAtPath:dir withIntermediateDirectories:YES attributes:nil error:nil];

#define STR(...) ((const char *[]){ __VA_ARGS__ })
        /* plain + depth tc2 variants built with the same head/body splits */
        BOOL ok = YES;
        const char *tc2p_plain[] = {"tc2p_plain", GLMC_MSL_TC2P_V, GLMC_MSL_TC2P_SH0, GLMC_MSL_TC2P_PLAIN_HEAD, GLMC_MSL_TC2P_BODY, GLMC_MSL_TC2P_TAIL};
        const char *tc2p_depth[] = {"tc2p_depth", GLMC_MSL_TC2P_V, GLMC_MSL_TC2P_SH0, GLMC_MSL_TC2P_DEPTH_HEAD, GLMC_MSL_TC2P_BODY, GLMC_MSL_TC2P_TAIL};
        const char *ffp[]        = {"ffp",        GLMC_MSL_FFP};
        const char *post1[]      = {"post1",      GLMC_MSL_FFP, GLMC_MSL_POST1};
        const char *tc2text[]    = {"tc2text",    GLMC_MSL_TC2Q_V, GLMC_MSL_TC2Q_TAILHEAD, GLMC_MSL_TC2_TEXT};
        const char *tc2gamma[]   = {"tc2gamma",   GLMC_MSL_TC2Q_V, GLMC_MSL_TC2Q_TAILHEAD, GLMC_MSL_TC2_GAMMA};
        const char *blit[]       = {"blit",       GLMC_MSL_BLIT};
        struct { const char *a[8]; } items[] = {
            { { tc2p_plain[0], tc2p_plain[1], tc2p_plain[2], tc2p_plain[3], tc2p_plain[4], tc2p_plain[5] } },
            { { tc2p_depth[0], tc2p_depth[1], tc2p_depth[2], tc2p_depth[3], tc2p_depth[4], tc2p_depth[5] } },
            { { ffp[0], ffp[1] } },
            { { post1[0], post1[1], post1[2] } },
            { { tc2text[0], tc2text[1], tc2text[2], tc2text[3] } },
            { { tc2gamma[0], tc2gamma[1], tc2gamma[2], tc2gamma[3] } },
            { { blit[0], blit[1] } },
        };
        for (unsigned i = 0; i < sizeof items / sizeof items[0]; i++) {
            /* the item lists end at NULL; each unit name slice holds them all */
            int flat_n = 0; const char *flat[8];
            while (flat_n < 8 && items[i].a[flat_n]) { flat[flat_n] = items[i].a[flat_n]; flat_n++; }
            if (argc > 2 && strcmp(flat[0], argv[2]) != 0) continue;
            ok &= compile_one(dev, flat[0], flat, flat_n, dir);
        }
        return ok ? 0 : 1;
    }
}
