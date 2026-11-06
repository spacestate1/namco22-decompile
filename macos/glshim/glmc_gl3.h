/* glmc_gl3.h — the post-GL-1.1 enums the engine's framebuffer, shader and
 * vertex-array code needs (values match the OpenGL specification). */
#ifndef GLMC_GL3_H
#define GLMC_GL3_H

/* framebuffer objects */
#define GL_FRAMEBUFFER              0x8D40
#define GL_READ_FRAMEBUFFER         0x8CA8
#define GL_DRAW_FRAMEBUFFER         0x8CA9
#define GL_RENDERBUFFER             0x8D41
#define GL_COLOR_ATTACHMENT0        0x8CE0
#define GL_DEPTH_ATTACHMENT         0x8D00
#define GL_STENCIL_ATTACHMENT       0x8D20
#define GL_FRAMEBUFFER_BINDING      0x8CA6
#define GL_FRAMEBUFFER_COMPLETE     0x8CD5
#define GL_FRAMEBUFFER_INCOMPLETE   0x8CDB
#define GL_COLOR_ATTACHMENT1        0x8CE1

/* shaders */
#define GL_FRAGMENT_SHADER          0x8B30
#define GL_VERTEX_SHADER            0x8B31
#define GL_COMPILE_STATUS           0x8B81
#define GL_LINK_STATUS              0x8B82
#define GL_SHADER_TYPE              0x8B4F
#define GL_CURRENT_PROGRAM          0x8B8D
#define GL_ACTIVE_UNIFORMS          0x8B86
#define GL_ACTIVE_ATTRIBUTES        0x8B89
#define GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS 0x8B4D
#define GL_UNIFORM_TYPE             0x8A86
#define GL_INFO_LOG_LENGTH          0x8B84

/* uniform types seen in the known programs */
#define GL_SAMPLER_2D               0x8B5E
#define GL_INT_VEC2                 0x8B53
#define GL_INT_VEC3                 0x8B54
#define GL_INT_VEC4                 0x8B55
#define GL_FLOAT_VEC2               0x8B50
#define GL_FLOAT_VEC3               0x8B51
#define GL_FLOAT_VEC4               0x8B52

/* buffers */
#define GL_ARRAY_BUFFER             0x8892
#define GL_ELEMENT_ARRAY_BUFFER     0x8893
#define GL_ARRAY_BUFFER_BINDING     0x8894
#define GL_ELEMENT_ARRAY_BUFFER_BINDING 0x8895
#define GL_STATIC_DRAW              0x88E4
#define GL_STREAM_DRAW              0x88E0
#define GL_BUFFER_SIZE              0x8764

#endif /* GLMC_GL3_H */
