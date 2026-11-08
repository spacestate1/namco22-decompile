/* glmc_gl1.h — GL 1.1 enum values exactly as the games use them (our own
 * subset copy of the classic names, so we never include Apple's OpenGL
 * headers). Values match the OpenGL specification. */
#ifndef GLMC_GL1_H
#define GLMC_GL1_H

#define GL_NO_ERROR                 0x0000
#define GL_INVALID_ENUM             0x0500
#define GL_INVALID_VALUE            0x0501
#define GL_INVALID_OPERATION        0x0502
#define GL_STACK_OVERFLOW           0x0503
#define GL_STACK_UNDERFLOW          0x0504
#define GL_OUT_OF_MEMORY            0x0505
#define GL_INVALID_FRAMEBUFFER_OPERATION 0x0506

#define GL_INVALID_INDEX            0xFFFFFFFFu
#define GL_VIEWPORT                 0x0BA2
#define GL_SCISSOR_BOX              0x0C10
#define GL_COLOR_BUFFER_BIT         0x00004000
#define GL_DEPTH_BUFFER_BIT         0x00000100
#define GL_STENCIL_BUFFER_BIT       0x00000400

#define GL_MODELVIEW                0x1700
#define GL_PROJECTION               0x1701
#define GL_TEXTURE                  0x1702
#define GL_MODELVIEW_STACK_DEPTH    0x0BA0
#define GL_PROJECTION_STACK_DEPTH   0x0BA1
#define GL_TEXTURE_STACK_DEPTH      0x0BA2

#define GL_POINTS                   0x0000
#define GL_LINES                    0x0001
#define GL_LINE_LOOP                0x0002
#define GL_LINE_STRIP               0x0003
#define GL_TRIANGLES                0x0004
#define GL_TRIANGLE_STRIP           0x0005
#define GL_TRIANGLE_FAN             0x0006
#define GL_QUADS                    0x0007
#define GL_POLYGON                  0x0009

#define GL_TEXTURE_2D               0x0DE1
#define GL_TEXTURE_BINDING_2D       0x8069
#define GL_MAX_TEXTURE_SIZE         0x0D33
#define GL_MAX_TEXTURE_UNITS        0x84E2
#define GL_RENDERER                 0x1F01
#define GL_VENDOR                   0x1F00
#define GL_VERSION                  0x1F02
#define GL_SHADING_LANGUAGE_VERSION 0x8B8C

#define GL_BYTE                     0x1400
#define GL_UNSIGNED_BYTE            0x1401
#define GL_SHORT                    0x1402
#define GL_UNSIGNED_SHORT           0x1403
#define GL_INT                      0x1404
#define GL_UNSIGNED_INT             0x1405
#define GL_FLOAT                    0x1406
#define GL_UNSIGNED_SHORT_5_6_5     0x8363

#define GL_RED                      0x1903
#define GL_ALPHA                    0x1906
#define GL_RGB                      0x1907
#define GL_RGBA                     0x1908
#define GL_LUMINANCE                0x1909
#define GL_LUMINANCE_ALPHA          0x190A
#define GL_BGRA                     0x80E1
#define GL_DEPTH_COMPONENT          0x1902

#define GL_ALPHA4                   0x803B
#define GL_ALPHA8                   0x803C
#define GL_ALPHA12                  0x803D
#define GL_ALPHA16                  0x803E
#define GL_LUMINANCE4               0x803F
#define GL_LUMINANCE8               0x8040
#define GL_LUMINANCE12              0x8041
#define GL_LUMINANCE16              0x8042
#define GL_LUMINANCE4_ALPHA4        0x8043
#define GL_LUMINANCE6_ALPHA2        0x8044
#define GL_LUMINANCE8_ALPHA8        0x8045
#define GL_LUMINANCE12_ALPHA4       0x8046
#define GL_LUMINANCE12_ALPHA12      0x8047
#define GL_LUMINANCE16_ALPHA16      0x8048
#define GL_RGB4                     0x804F
#define GL_RGB5                     0x8050
#define GL_RGB8                     0x8051
#define GL_RGB10                    0x8052
#define GL_RGB5_A1                  0x8057
#define GL_RGBA4                    0x8056
#define GL_RGBA8                    0x8058
#define GL_RGB565                   0x8D62
#define GL_RGB565_                  0x8D62
#define GL_DEPTH_COMPONENT16        0x81A5
#define GL_DEPTH_COMPONENT24        0x81A6
#define GL_DEPTH_COMPONENT32F       0x8CAC

#define GL_NEAREST                  0x2600
#define GL_LINEAR                   0x2601
#define GL_NEAREST_MIPMAP_NEAREST   0x2700
#define GL_LINEAR_MIPMAP_NEAREST    0x2701
#define GL_NEAREST_MIPMAP_LINEAR    0x2702
#define GL_LINEAR_MIPMAP_LINEAR     0x2703

#define GL_TEXTURE_MIN_FILTER       0x2801
#define GL_TEXTURE_MAG_FILTER       0x2800
#define GL_TEXTURE_WRAP_S           0x2802
#define GL_TEXTURE_WRAP_T           0x2803
#define GL_REPEAT                   0x2901
#define GL_CLAMP                    0x2900
#define GL_CLAMP_TO_EDGE            0x812F
#define GL_MIRRORED_REPEAT          0x8370
#define GL_TEXTURE_COMPARE_MODE     0x884C
#define GL_TEXTURE_COMPARE_FUNC     0x884D
#define GL_COMPARE_REF_TO_TEXTURE   0x884E
#define GL_NONE                     0

#define GL_TEXTURE_ENV              0x2300
#define GL_TEXTURE_ENV_MODE         0x2200
#define GL_TEXTURE_ENV_COLOR        0x2201
#define GL_DECAL                    0x2103
#define GL_MODULATE                 0x2100
#define GL_REPLACE                  0x1E01
#define GL_ADD                      0x0104
#define GL_BLEND                    0x0BE2
#define GL_COMBINE                  0x8570
#define GL_COMBINE_RGB              0x8571
#define GL_COMBINE_ALPHA            0x8572
#define GL_RGB_SCALE                0x8573
#define GL_ALPHA_SCALE              0x8574
#define GL_INTERPOLATE              0x8575
#define GL_CONSTANT                 0x8576
#define GL_PRIMARY_COLOR            0x8577
#define GL_PREVIOUS                 0x8578
#define GL_SOURCE0_RGB              0x8580
#define GL_SOURCE1_RGB              0x8581
#define GL_SOURCE2_RGB              0x8582
#define GL_SOURCE0_ALPHA            0x8588
#define GL_SOURCE1_ALPHA            0x8589
#define GL_SOURCE2_ALPHA            0x858A
#define GL_OPERAND0_RGB             0x8590
#define GL_OPERAND1_RGB             0x8591
#define GL_OPERAND2_RGB             0x8592
#define GL_OPERAND0_ALPHA           0x8598
#define GL_OPERAND1_ALPHA           0x8599
#define GL_OPERAND2_ALPHA           0x859A
#define GL_TEXTURE0                 0x84C0
#define GL_TEXTURE1                 0x84C1
#define GL_TEXTURE2                 0x84C2
#define GL_TEXTURE3                 0x84C3
#define GL_TEXTURE4                 0x84C4
#define GL_TEXTURE5                 0x84C5
#define GL_TEXTURE6                 0x84C6
#define GL_TEXTURE7                 0x84C7
#define GL_TEXTURE                  0x1702

#define GL_SRC_COLOR                0x0300
#define GL_SRC_ALPHA                0x0302
#define GL_ONE_MINUS_SRC_ALPHA      0x0303
#define GL_ONE_MINUS_SRC_COLOR      0x0301
#define GL_DST_COLOR                0x0306
#define GL_ONE_MINUS_DST_COLOR      0x0307
#define GL_ONE_MINUS_DST_ALPHA      0x0305
#define GL_DST_ALPHA                0x0304
#define GL_ONE                      1
#define GL_ZERO                     0
#define GL_SRC_ALPHA_SATURATE       0x0308

#define GL_BLEND_SRC                0x0BE8
#define GL_BLEND_DST                0x0BE9
#define GL_BLEND_SRC_RGB            0x80C9
#define GL_BLEND_DST_RGB            0x80C8
#define GL_BLEND_SRC_ALPHA          0x80CB
#define GL_BLEND_DST_ALPHA          0x80CA
#define GL_BLEND_EQUATION           0x8009
#define GL_BLEND_EQUATION_RGB       0x8009
#define GL_BLEND_EQUATION_ALPHA     0x883D
#define GL_FUNC_ADD                 0x8006
#define GL_FUNC_SUBTRACT            0x800A
#define GL_FUNC_REVERSE_SUBTRACT    0x800B
#define GL_MAX                      0x8008
#define GL_MIN                      0x8007

#define GL_ALPHA_TEST               0x0BC0
#define GL_ALPHA_TEST_FUNC          0x0BC1
#define GL_ALPHA_TEST_REF           0x0BC2
#define GL_GREATER                  0x0204
#define GL_GEQUAL                   0x0206
#define GL_LESS                     0x0201
#define GL_LEQUAL                   0x0203
#define GL_EQUAL                    0x0202
#define GL_NOTEQUAL                 0x0205
#define GL_NEVER                    0x0200
#define GL_ALWAYS                   0x0207

#define GL_DEPTH_TEST               0x0B71
#define GL_CULL_FACE                0x0B44
#define GL_SCISSOR_TEST             0x0C11
#define GL_DITHER                   0x0BD0
#define GL_FOG                      0x0B60
#define GL_LIGHTING                 0x0B50
#define GL_COLOR_LOGIC_OP           0x0BF2
#define GL_LINE_SMOOTH              0x0B20
#define GL_POLYGON_SMOOTH           0x0B41
#define GL_MAP_COLOR                0x0D10

#define GL_UNPACK_ALIGNMENT         0x0CF5
#define GL_UNPACK_ROW_LENGTH        0x0CF2
#define GL_UNPACK_SWAP_BYTES        0x0CF1
#define GL_PACK_ALIGNMENT           0x0D05
#define GL_PACK_ROW_LENGTH          0x0D02
#define GL_PACK_SWAP_BYTES          0x0D03

#define GL_BACK                     0x0405
#define GL_FRONT                    0x0404
#define GL_FRONT_AND_BACK           0x0408
#define GL_BACK_LEFT                0x0402
#define GL_BACK_RIGHT               0x0403
#define GL_FRONT_LEFT               0x0400
#define GL_FRONT_RIGHT              0x0401

#define GL_2_BYTES                  0x1407
#define GL_3_BYTES                  0x1408
#define GL_4_BYTES                  0x1409
#define GL_DOUBLE                   0x140A

#define GL_VERTEX_ARRAY             0x8073
#define GL_COLOR_ARRAY              0x8074
#define GL_NORMAL_ARRAY             0x8075
#define GL_TEXTURE_COORD_ARRAY      0x8078
#define GL_INDEX_ARRAY              0x8077
#define GL_EDGE_FLAG_ARRAY          0x8079

#define GL_2D                       0x0600
#define GL_3D                       0x0601

#define GL_SHADE_MODEL              0x0B54
#define GL_FLAT                     0x1D00
#define GL_SMOOTH                   0x1D01
#define GL_FASTEST                  0x1101
#define GL_NICEST                   0x1102
#define GL_DONT_CARE                0x1100
#define GL_PERSPECTIVE_HINT         0x0C50
#define GL_LINE_WIDTH               0x0B21
#define GL_LINE_WIDTH_RANGE         0x0B22
#define GL_POINT_SIZE               0x0B11

#define GL_READ_BUFFER              0x0C01
#define GL_DRAW_BUFFER              0x0C01
#define GL_MAP_R_TO_R               0x0D15
#define GL_MAP_G_TO_G               0x0D16
#define GL_MAP_B_TO_B               0x0D17
#define GL_MAP_A_TO_A               0x0D18
#define GL_PIXEL_MAP_R_TO_R         0x0D15
#define GL_PIXEL_MAP_G_TO_G         0x0D16
#define GL_PIXEL_MAP_B_TO_B         0x0D17
#define GL_PIXEL_MAP_A_TO_A         0x0D18

#define GL_ALL_ATTRIB_BITS          0x0007ffff
#define GL_ENABLE_BIT               0x00008000
#define GL_TRANSFORM_BIT            0x00001000
#define GL_LIGHTING_BIT             0x00004000
#define GL_FOG_BIT                  0x00002000
#define GL_HINT_BIT                 0x00008000
#define GL_POLYGON_BIT              0x00000800
#define GL_POINT_BIT                0x00000200
#define GL_LINE_BIT                 0x00000400
#define GL_SCISSOR_BIT              0x00080000
#define GL_TEXTURE_BIT              0x00040000

#define GL_MULTISAMPLE              0x809D

#endif /* GLMC_GL1_H */
