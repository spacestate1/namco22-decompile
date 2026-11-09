#!/usr/bin/env python3
"""Post-build patch for the browser build (timecris/build-web/tc.js).

Emscripten's LEGACY_GL_EMULATION samples every texture as
    texture2D(u_texUnitN, coord.xy)
which drops the q of the engine's homogeneous texture coordinates
(quad_gl.c submits (s*q, t*q, 0, q), the System 22's perspective-correct
texturing). This rewrites the generated sample expression to
    texture2DProj(u_texUnitN, vec4(coord.xy, 0.0, coord.w))
which divides by q again. A 2-component texcoord attribute gets w=1
automatically, so the 2D layers (sprites, text) are unchanged.

Usage: glemu_texproj.py <path/to/tc.js>
"""
import sys

OLD = "return`${func}(${TEX_UNIT_UNIFORM_PREFIX}${texUnitID}, ${texCoordExpr}.xy)`"
NEW = ("return func==\"texture2D\""
       "?`texture2DProj(${TEX_UNIT_UNIFORM_PREFIX}${texUnitID}, vec4(${texCoordExpr}.xy, 0.0, ${texCoordExpr}.w))`"
       ":`${func}(${TEX_UNIT_UNIFORM_PREFIX}${texUnitID}, ${texCoordExpr}.xy)`")


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__.strip())
    path = sys.argv[1]
    with open(path) as f:
        s = f.read()
    if NEW in s:
        print(f"[glemu_texproj] {path}: already patched")
        return
    if OLD not in s:
        sys.exit(f"[glemu_texproj] {path}: pattern not found -- emscripten's "
                 "libglemu.js changed; update this script")
    s = s.replace(OLD, NEW)
    # The combiner's texture-load cache splits sample expressions with the
    # non-greedy regex /(texture.*?\(.*?\))/g, which stops at the FIRST ')' --
    # inside the '(u_textureMatrixN * v_texCoordN)' parentheses -- and shreds
    # every GL_COMBINE shader into invalid GLSL. Disable the cache (duplicate
    # texture2DProj calls are CSE'd by the driver's compiler anyway).
    s = s.replace("/(texture.*?\\(.*?\\))/g", "/(?!x)x/g").replace("/(texture.*\\(.*\\))/g", "/(?!x)x/g")
    # mediump is FP16 on Apple GPUs: the homogeneous texcoords (texels x q)
    # overflow 65504 on large polygons -> NaN -> black walls/floors/sea.
    # SwiftShader's FP32 mediump hid it. Generate highp fragment shaders.
    s = s.replace('["precision mediump float;",texUnitVaryingList',
                  '["precision highp float;",texUnitVaryingList')
    with open(path, "w") as f:
        f.write(s)
    print(f"[glemu_texproj] {path}: texture2DProj + combiner-cache patches applied")


if __name__ == "__main__":
    main()
