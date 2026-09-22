/*
 * Prop Cycle - Video Hardware Formats
 * Decoded from MAME video/namcos22.cpp
 */
#ifndef VIDEO_FORMATS_H
#define VIDEO_FORMATS_H

#include <stdint.h>

/* ========== PALETTE FORMAT ========== */
/*
 * 3 separate planes (one per color channel), NOT interleaved:
 *   Plane 0 (R): palette_ram[0x00000 + index]
 *   Plane 1 (G): palette_ram[0x08000 + index]
 *   Plane 2 (B): palette_ram[0x10000 + index]
 * Total: 0x8000 entries (2048 colors for SS22, more for S22)
 */
#define PALETTE_PLANE_SIZE  0x8000
#define PALETTE_R_OFFSET    0x00000
#define PALETTE_G_OFFSET    0x08000
#define PALETTE_B_OFFSET    0x10000

/* ========== TEXT/TILEMAP FORMAT ========== */
/*
 * Text RAM: 64x64 entries, each 16-bit big-endian
 *   Bits 15-12: palette select (4 bits)
 *   Bit 11:     flip Y
 *   Bit 10:     flip X
 *   Bits 9-0:   tile code (0-1023)
 *
 * Each tile is 8x8 pixels, stored in CGRAM
 *
 * Tilemap Attributes (0x8A0000):
 *   [0]: X scroll (-= 0x35C for display offset)
 *   [1]: Y scroll
 *   [2]: 0x006E (constant)
 *   [4]: Position IRQ scanline
 */
#define TEXT_TILE_PALETTE_SHIFT  12
#define TEXT_TILE_PALETTE_MASK   0xF000
#define TEXT_TILE_FLIPY          0x0800
#define TEXT_TILE_FLIPX          0x0400
#define TEXT_TILE_CODE_MASK      0x03FF
#define TEXT_SCROLL_X_BIAS       0x035C

/* ========== SPRITE FORMAT (C374 / VICS) ========== */
/*
 * Sprite RAM header: 0x980000 - 0x9801FF (control registers)
 * Sprite data: 0x984000+ (8 words per sprite, base*4)
 * Sprite attrs: 0x9A0000+ (color/z data, base*2)
 * Window clip: 0x980200 - 0x98023F
 *
 * Per sprite data (8 words = 16 bytes):
 *   word 0: X position (signed 16-bit)
 *   word 1: Y position (signed 16-bit)
 *   word 2: X size, Y size
 *   word 3: control (linktype, justify, cols/rows, flip)
 *   word 4: tile number (high bits)
 *   word 5: tile number (low bits) + translucency
 *   word 6: reserved
 *   word 7: reserved
 *
 * Per sprite attribute (4 bytes):
 *   byte 0-2: Z coordinate (24-bit)
 *   byte 3:   color (6 bits) | cz_enable | flags
 */

/* ========== TEXTURE TILE FORMAT ========== */
/*
 * Texture tiles: 16x16 pixels, 8bpp (256 bytes per tile)
 * Stored in pr1cg0-7 ROMs (16 MB total, 65536 tiles)
 *
 * Texture tilemap: pr1ccrl.3d + pr1ccrh.1d
 *   Maps (u,v) to tile index
 *   Lookup: tile_offset = ((v & 0xFFF0) << 4) | ((u & 0xFF0) >> 4)
 *           tile_index = texture_tilemap[tile_offset]
 *           pixel = texture_data[tile_index * 256 + local_offset]
 *
 * Tile attributes (4-bit per tile):
 *   Bit 2 (0x4): Flip X
 *   Bit 1 (0x2): Flip Y
 *   Bit 3 (0x8): Rotate 90 degrees
 *
 * Sub-tile pixel lookup uses tt_ayx_to_pixel[attr<<8 | (v&0xF)<<4 | (u&0xF)]
 */

/* ========== POINT ROM FORMAT ========== */
/*
 * 3D model data in pr1ptr* ROMs
 * Stored as 3 separate byte planes:
 *   pr1ptrl0-2: Low byte
 *   pr1ptrm0-2: Mid byte
 *   pr1ptru0-2: High byte
 * Each 24-bit value: signed24(high << 16 | mid << 8 | low)
 *
 * Polygon packet types:
 *   0x17: Simple quad (opcode, flags, color + 4 vertices)
 *   0x18: Quad with depth bias
 *   0x10: Vertex lighting (4 normal vectors)
 *   0x0d: Additional normals
 *
 * Per vertex:
 *   x, y, z: signed 24-bit world coordinates
 *   u, v: 12-bit texture coordinates (0..0xFFF)
 *   bri: 8-bit brightness (bits 23-16 of vertex word)
 */
typedef struct {
    float x, y, z;
    int u, v;       /* 0..0xFFF */
    int bri;        /* 0..0xFF */
} PointRomVertex;

/* ========== VIDEO MIXER REGISTERS (0x824000, SS22) ========== */
/*
 * 0x00-0x02: Polygon fade RGB (0xFF = disabled)
 * 0x05-0x07: Fog RGB color
 * 0x11:      Polygon translucency (0xFF=opaque, 0x00=transparent)
 * 0x16-0x18: Screen fade RGB (0x100 scale)
 * 0x19:      Screen fade factor
 * 0x1A:      Mixer flags (bit0=global fade, bit1=poly fade, bit8=shadow)
 * 0x1B:      Text palette base (<<8 & 0x7F00)
 * 0x1F:      Layer enable (bit0=polygons, bit1=sprites, bit2=text)
 * 0x100-0x103: Fog R per CZ type
 * 0x180-0x183: Fog G per CZ type
 * 0x200-0x203: Fog B per CZ type
 */
#define VMIX_POLY_FADE_R    0x00
#define VMIX_POLY_FADE_G    0x01
#define VMIX_POLY_FADE_B    0x02
#define VMIX_FOG_R          0x05
#define VMIX_FOG_G          0x06
#define VMIX_FOG_B          0x07
#define VMIX_POLY_ALPHA     0x11
#define VMIX_FADE_R         0x16
#define VMIX_FADE_G         0x17
#define VMIX_FADE_B         0x18
#define VMIX_FADE_FACTOR    0x19
#define VMIX_FLAGS          0x1A
#define VMIX_TEXT_PALBASE   0x1B
#define VMIX_LAYER_ENABLE   0x1F

/* ========== DSP/PDP COMMANDS ========== */
/*
 * PDP commands in polygon RAM (processed by DSPs on real hardware):
 *
 * 0x15 words: Viewport/camera setup
 *   ambient, power, light_dir[3], priority, camera_pos[3], zoom, frustum[4]
 *
 * 0x10 words: Model render options
 *   cz_adjust, z_bias, far_plane
 *
 * 0x0A words: Modify view transform
 *   3x3 rotation matrix (9 values)
 *
 * 0x0D words: Render primitive
 *   object_code, 3x3 transform, xyz position
 *
 * Point RAM I/O:
 *   0xFFF5: Write word
 *   0xFFF6: Read word
 *   0xFFFA: Read block
 *   0xFFFB: Write block
 *   0xFFFC: Block copy
 *   0xFFFF: Goto (redirect command stream)
 */

/* ========== CZ (DEPTH-CUEING / FOG) ========== */
/*
 * CZRAM: 0x2000 entries lookup table
 * CZ calculation per pixel:
 *   cz = ooz + cz_adjust
 *   if (cz < 0x200000) cz >>= 8; else cz = clamp(0, 0x1FFF)
 *   fog_factor = czram[cz] + cz_sdelta
 *   blend(pixel_rgb, fog_rgb, 0xFF - fog_factor)
 */

#endif /* VIDEO_FORMATS_H */
