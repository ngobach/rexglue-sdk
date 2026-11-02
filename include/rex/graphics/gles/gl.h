// Minimal OpenGL ES 3.1 entry point declarations loaded at runtime.
//
// The backend never links libGLESv2/EGL directly: SDL owns the GL context and
// the functions are resolved through SDL_GL_GetProcAddress. That keeps the
// plugin loadable on hosts without GLES (it simply fails to create a context)
// and lets the same code build on macOS for compile validation.
#pragma once

#include <cstddef>
#include <cstdint>

namespace rex::gles {

using GLenum = uint32_t;
using GLboolean = uint8_t;
using GLbitfield = uint32_t;
using GLbyte = int8_t;
using GLubyte = uint8_t;
using GLshort = int16_t;
using GLushort = uint16_t;
using GLint = int32_t;
using GLuint = uint32_t;
using GLsizei = int32_t;
using GLfloat = float;
using GLclampf = float;
using GLchar = char;
using GLsizeiptr = std::ptrdiff_t;
using GLintptr = std::ptrdiff_t;
using GLsync = void*;
using GLint64 = int64_t;
using GLuint64 = uint64_t;

constexpr GLenum GL_FALSE = 0;
constexpr GLenum GL_TRUE = 1;
constexpr GLenum GL_ACTIVE_TEXTURE = 0x84E0;
constexpr GLenum GL_SAMPLER_BINDING = 0x8919;
constexpr GLenum GL_TEXTURE_BINDING_2D = 0x8069;
constexpr GLenum GL_TEXTURE_BINDING_2D_ARRAY = 0x8C1D;
constexpr GLenum GL_NO_ERROR = 0;
constexpr GLenum GL_INVALID_ENUM = 0x0500;
constexpr GLenum GL_INVALID_VALUE = 0x0501;
constexpr GLenum GL_INVALID_OPERATION = 0x0502;
constexpr GLenum GL_OUT_OF_MEMORY = 0x0505;
constexpr GLenum GL_ZERO = 0;
constexpr GLenum GL_ONE = 1;
constexpr GLenum GL_SRC_COLOR = 0x0300;
constexpr GLenum GL_ONE_MINUS_SRC_COLOR = 0x0301;
constexpr GLenum GL_SRC_ALPHA = 0x0302;
constexpr GLenum GL_ONE_MINUS_SRC_ALPHA = 0x0303;
constexpr GLenum GL_DST_ALPHA = 0x0304;
constexpr GLenum GL_ONE_MINUS_DST_ALPHA = 0x0305;
constexpr GLenum GL_DST_COLOR = 0x0306;
constexpr GLenum GL_ONE_MINUS_DST_COLOR = 0x0307;
constexpr GLenum GL_SRC_ALPHA_SATURATE = 0x0308;
constexpr GLenum GL_CONSTANT_COLOR = 0x8001;
constexpr GLenum GL_ONE_MINUS_CONSTANT_COLOR = 0x8002;
constexpr GLenum GL_CONSTANT_ALPHA = 0x8003;
constexpr GLenum GL_ONE_MINUS_CONSTANT_ALPHA = 0x8004;
constexpr GLenum GL_FUNC_ADD = 0x8006;
constexpr GLenum GL_FUNC_SUBTRACT = 0x800A;
constexpr GLenum GL_FUNC_REVERSE_SUBTRACT = 0x800B;
constexpr GLenum GL_MIN = 0x8007;
constexpr GLenum GL_MAX = 0x8008;
constexpr GLenum GL_NEVER = 0x0200;
constexpr GLenum GL_LESS = 0x0201;
constexpr GLenum GL_EQUAL = 0x0202;
constexpr GLenum GL_LEQUAL = 0x0203;
constexpr GLenum GL_GREATER = 0x0204;
constexpr GLenum GL_NOTEQUAL = 0x0205;
constexpr GLenum GL_GEQUAL = 0x0206;
constexpr GLenum GL_ALWAYS = 0x0207;
constexpr GLenum GL_KEEP = 0x1E00;
constexpr GLenum GL_REPLACE = 0x1E01;
constexpr GLenum GL_INCR = 0x1E02;
constexpr GLenum GL_DECR = 0x1E03;
constexpr GLenum GL_INVERT = 0x150A;
constexpr GLenum GL_INCR_WRAP = 0x8507;
constexpr GLenum GL_DECR_WRAP = 0x8508;
constexpr GLenum GL_POINTS = 0x0000;
constexpr GLenum GL_LINES = 0x0001;
constexpr GLenum GL_LINE_LOOP = 0x0002;
constexpr GLenum GL_LINE_STRIP = 0x0003;
constexpr GLenum GL_TRIANGLES = 0x0004;
constexpr GLenum GL_TRIANGLE_STRIP = 0x0005;
constexpr GLenum GL_TRIANGLE_FAN = 0x0006;
constexpr GLenum GL_UNSIGNED_BYTE = 0x1401;
constexpr GLenum GL_UNSIGNED_SHORT = 0x1403;
constexpr GLenum GL_UNSIGNED_INT = 0x1405;
constexpr GLenum GL_FLOAT = 0x1406;
constexpr GLenum GL_DEPTH_COMPONENT = 0x1902;
constexpr GLenum GL_ALPHA = 0x1906;
constexpr GLenum GL_RGB = 0x1907;
constexpr GLenum GL_RGBA = 0x1908;
constexpr GLenum GL_LUMINANCE = 0x1909;
constexpr GLenum GL_LUMINANCE_ALPHA = 0x190A;
constexpr GLenum GL_RED = 0x1903;
constexpr GLenum GL_GREEN = 0x1904;
constexpr GLenum GL_BLUE = 0x1905;
constexpr GLenum GL_RG = 0x8227;
constexpr GLenum GL_R8 = 0x8229;
constexpr GLenum GL_RG8 = 0x822B;
constexpr GLenum GL_RGB8 = 0x8051;
constexpr GLenum GL_RGBA8 = 0x8058;
constexpr GLenum GL_R16F = 0x822D;
constexpr GLenum GL_RG16F = 0x822F;
constexpr GLenum GL_RGBA16F = 0x881A;
constexpr GLenum GL_R32F = 0x822E;
constexpr GLenum GL_RG32F = 0x8230;
constexpr GLenum GL_RGBA32F = 0x8814;
constexpr GLenum GL_RGBA4 = 0x8056;
constexpr GLenum GL_RGB5_A1 = 0x8057;
constexpr GLenum GL_RGB565 = 0x8D62;
constexpr GLenum GL_RGB10_A2 = 0x8059;
constexpr GLenum GL_RGBA16UI = 0x8D76;
constexpr GLenum GL_R32UI = 0x8236;
constexpr GLenum GL_RED_INTEGER = 0x8D94;
constexpr GLenum GL_RGBA32UI = 0x8D70;
constexpr GLenum GL_DEPTH_COMPONENT16 = 0x81A5;
constexpr GLenum GL_DEPTH_COMPONENT24 = 0x81A6;
constexpr GLenum GL_DEPTH24_STENCIL8 = 0x88F0;
constexpr GLenum GL_DEPTH_COMPONENT32F = 0x8CAC;
constexpr GLenum GL_DEPTH_STENCIL = 0x84F9;
constexpr GLenum GL_UNSIGNED_INT_24_8 = 0x84FA;
constexpr GLenum GL_DEPTH_BUFFER_BIT = 0x00000100;
constexpr GLenum GL_STENCIL_BUFFER_BIT = 0x00000400;
constexpr GLenum GL_COLOR_BUFFER_BIT = 0x00004000;
constexpr GLenum GL_VENDOR = 0x1F00;
constexpr GLenum GL_RENDERER = 0x1F01;
constexpr GLenum GL_VERSION = 0x1F02;
constexpr GLenum GL_EXTENSIONS = 0x1F03;
constexpr GLenum GL_TEXTURE_2D = 0x0DE1;
constexpr GLenum GL_TEXTURE_2D_ARRAY = 0x8C1A;
constexpr GLenum GL_TEXTURE_3D = 0x806F;
constexpr GLenum GL_TEXTURE_CUBE_MAP = 0x8513;
constexpr GLenum GL_TEXTURE_CUBE_MAP_POSITIVE_X = 0x8515;
constexpr GLenum GL_TEXTURE_CUBE_MAP_NEGATIVE_X = 0x8516;
constexpr GLenum GL_TEXTURE_CUBE_MAP_POSITIVE_Y = 0x8517;
constexpr GLenum GL_TEXTURE_CUBE_MAP_NEGATIVE_Y = 0x8518;
constexpr GLenum GL_TEXTURE_CUBE_MAP_POSITIVE_Z = 0x8519;
constexpr GLenum GL_TEXTURE_CUBE_MAP_NEGATIVE_Z = 0x851A;
constexpr GLenum GL_TEXTURE_CUBE_MAP_ARRAY = 0x9009;
constexpr GLenum GL_TEXTURE0 = 0x84C0;
constexpr GLenum GL_TEXTURE_MIN_FILTER = 0x2801;
constexpr GLenum GL_TEXTURE_MAG_FILTER = 0x2800;
constexpr GLenum GL_TEXTURE_WRAP_S = 0x2802;
constexpr GLenum GL_TEXTURE_WRAP_T = 0x2803;
constexpr GLenum GL_TEXTURE_WRAP_R = 0x8072;
constexpr GLenum GL_TEXTURE_BASE_LEVEL = 0x813C;
constexpr GLenum GL_TEXTURE_MAX_LEVEL = 0x813D;
constexpr GLenum GL_TEXTURE_MAX_LOD = 0x813B;
constexpr GLenum GL_TEXTURE_MIN_LOD = 0x813A;
constexpr GLenum GL_TEXTURE_SWIZZLE_R = 0x8E42;
constexpr GLenum GL_TEXTURE_SWIZZLE_G = 0x8E43;
constexpr GLenum GL_TEXTURE_SWIZZLE_B = 0x8E44;
constexpr GLenum GL_TEXTURE_SWIZZLE_A = 0x8E45;
constexpr GLenum GL_TEXTURE_SWIZZLE_RGBA = 0x8E46;
constexpr GLenum GL_TEXTURE_COMPARE_MODE = 0x884C;
constexpr GLenum GL_TEXTURE_COMPARE_FUNC = 0x884D;
constexpr GLenum GL_COMPARE_REF_TO_TEXTURE = 0x884E;
constexpr GLenum GL_NONE = 0;
constexpr GLenum GL_LINEAR = 0x2601;
constexpr GLenum GL_NEAREST = 0x2600;
constexpr GLenum GL_LINEAR_MIPMAP_LINEAR = 0x2703;
constexpr GLenum GL_LINEAR_MIPMAP_NEAREST = 0x2701;
constexpr GLenum GL_NEAREST_MIPMAP_LINEAR = 0x2702;
constexpr GLenum GL_NEAREST_MIPMAP_NEAREST = 0x2700;
constexpr GLenum GL_REPEAT = 0x2901;
constexpr GLenum GL_CLAMP_TO_EDGE = 0x812F;
constexpr GLenum GL_MIRRORED_REPEAT = 0x8370;
constexpr GLenum GL_CLAMP_TO_BORDER = 0x812D;
constexpr GLenum GL_MIRROR_CLAMP_TO_EDGE = 0x8743;
constexpr GLenum GL_ARRAY_BUFFER = 0x8892;
constexpr GLenum GL_ELEMENT_ARRAY_BUFFER = 0x8893;
constexpr GLenum GL_UNIFORM_BUFFER = 0x8A11;
constexpr GLenum GL_SHADER_STORAGE_BUFFER = 0x90D2;
constexpr GLenum GL_PIXEL_PACK_BUFFER = 0x88EB;
constexpr GLenum GL_PIXEL_UNPACK_BUFFER = 0x88EC;
constexpr GLenum GL_STREAM_DRAW = 0x88E0;
constexpr GLenum GL_STATIC_DRAW = 0x88E4;
constexpr GLenum GL_DYNAMIC_DRAW = 0x88E8;
constexpr GLenum GL_FRAGMENT_SHADER = 0x8B30;
constexpr GLenum GL_VERTEX_SHADER = 0x8B31;
constexpr GLenum GL_COMPUTE_SHADER = 0x91B9;
constexpr GLenum GL_COMPILE_STATUS = 0x8B81;
constexpr GLenum GL_LINK_STATUS = 0x8B82;
constexpr GLenum GL_VALIDATE_STATUS = 0x8B83;
constexpr GLenum GL_INFO_LOG_LENGTH = 0x8B84;
constexpr GLenum GL_ACTIVE_UNIFORMS = 0x8B86;
constexpr GLenum GL_ACTIVE_UNIFORM_MAX_LENGTH = 0x8B87;
constexpr GLenum GL_ACTIVE_ATTRIBUTES = 0x8B89;
constexpr GLenum GL_FRAMEBUFFER = 0x8D40;
constexpr GLenum GL_READ_FRAMEBUFFER = 0x8CA8;
constexpr GLenum GL_DRAW_FRAMEBUFFER = 0x8CA9;
constexpr GLenum GL_RENDERBUFFER = 0x8D41;
constexpr GLenum GL_COLOR_ATTACHMENT0 = 0x8CE0;
constexpr GLenum GL_DEPTH_ATTACHMENT = 0x8D00;
constexpr GLenum GL_STENCIL_ATTACHMENT = 0x8D20;
constexpr GLenum GL_DEPTH_STENCIL_ATTACHMENT = 0x821A;
constexpr GLenum GL_FRAMEBUFFER_COMPLETE = 0x8CD5;
constexpr GLenum GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT = 0x8CD6;
constexpr GLenum GL_FRAMEBUFFER_INCOMPLETE_MISSING_ATTACHMENT = 0x8CD7;
constexpr GLenum GL_FRAMEBUFFER_UNSUPPORTED = 0x8CDD;
constexpr GLenum GL_BLEND = 0x0BE2;
constexpr GLenum GL_DEPTH_TEST = 0x0B71;
constexpr GLenum GL_STENCIL_TEST = 0x0B90;
constexpr GLenum GL_CULL_FACE = 0x0B44;
constexpr GLenum GL_SCISSOR_TEST = 0x0C11;
constexpr GLenum GL_POLYGON_OFFSET_FILL = 0x8037;
constexpr GLenum GL_DITHER = 0x0BD0;
constexpr GLenum GL_FRONT = 0x0404;
constexpr GLenum GL_BACK = 0x0405;
constexpr GLenum GL_FRONT_AND_BACK = 0x0408;
constexpr GLenum GL_CW = 0x0900;
constexpr GLenum GL_CCW = 0x0901;
constexpr GLenum GL_PACK_ALIGNMENT = 0x0D05;
constexpr GLenum GL_UNPACK_ALIGNMENT = 0x0CF5;
constexpr GLenum GL_RGBA_INTEGER = 0x8D99;
constexpr GLenum GL_UNSIGNED_INT_8_8_8_8 = 0x8035;
constexpr GLenum GL_UNSIGNED_INT_8_8_8_8_REV = 0x8367;
constexpr GLenum GL_MAX_TEXTURE_SIZE = 0x0D33;
constexpr GLenum GL_MAX_3D_TEXTURE_SIZE = 0x8073;
constexpr GLenum GL_MAX_ARRAY_TEXTURE_LAYERS = 0x88FF;
constexpr GLenum GL_MAX_CUBE_MAP_TEXTURE_SIZE = 0x851C;
constexpr GLenum GL_MAX_COLOR_ATTACHMENTS = 0x8CDF;
constexpr GLenum GL_MAX_TEXTURE_IMAGE_UNITS = 0x8872;
constexpr GLenum GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS = 0x8B4D;
constexpr GLenum GL_MAX_VERTEX_TEXTURE_IMAGE_UNITS = 0x8B4C;
constexpr GLenum GL_MAX_DRAW_BUFFERS = 0x8824;
constexpr GLenum GL_MAX_RENDERBUFFER_SIZE = 0x84E8;
constexpr GLenum GL_MAX_SHADER_STORAGE_BLOCK_SIZE = 0x90DE;
constexpr GLenum GL_MAX_SHADER_STORAGE_BUFFER_BINDINGS = 0x90DD;
constexpr GLenum GL_MAX_UNIFORM_BLOCK_SIZE = 0x8A30;
constexpr GLenum GL_MAX_UNIFORM_BUFFER_BINDINGS = 0x8A2F;
constexpr GLenum GL_MAX_SAMPLES = 0x8D57;
constexpr GLenum GL_SAMPLES = 0x80A9;
constexpr GLenum GL_MAX_VERTEX_UNIFORM_VECTORS = 0x8DFB;
constexpr GLenum GL_MAX_FRAGMENT_UNIFORM_VECTORS = 0x8DFD;
constexpr GLenum GL_NUM_EXTENSIONS = 0x821D;
constexpr GLenum GL_CONTEXT_PROFILE_MASK = 0x9126;
constexpr GLenum GL_RENDERER_ANGLE = 0x8E42;  // unused placeholder
constexpr GLenum GL_DEBUG_OUTPUT = 0x92E0;
constexpr GLenum GL_DEBUG_SOURCE_API = 0x8246;
constexpr GLenum GL_DEBUG_TYPE_ERROR = 0x824C;
constexpr GLenum GL_DEBUG_TYPE_OTHER = 0x8251;
constexpr GLenum GL_DEBUG_SEVERITY_HIGH = 0x9146;
constexpr GLuint GL_INVALID_INDEX = 0xFFFFFFFF;

// X(return type, name, argument list)
#define REX_GL_FUNCTION_LIST(X)                                                     \
  X(const GLubyte*, GetString, (GLenum))                                            \
  X(GLenum, GetError, (void))                                                       \
  X(void, GetIntegerv, (GLenum, GLint*))                                            \
  X(void, GetIntegeri_v, (GLenum, GLuint, GLint*))                                  \
  X(void, GetFloatv, (GLenum, GLfloat*))                                            \
  X(const GLubyte*, GetStringi, (GLenum, GLuint))                                   \
  X(void, Viewport, (GLint, GLint, GLsizei, GLsizei))                               \
  X(void, Scissor, (GLint, GLint, GLsizei, GLsizei))                                \
  X(void, ClearColor, (GLfloat, GLfloat, GLfloat, GLfloat))                         \
  X(void, ClearDepthf, (GLfloat))                                                   \
  X(void, ClearStencil, (GLint))                                                    \
  X(void, Clear, (GLbitfield))                                                      \
  X(void, Finish, (void))                                                           \
  X(void, Flush, (void))                                                            \
  X(void, Enable, (GLenum))                                                         \
  X(void, Disable, (GLenum))                                                        \
  X(GLboolean, IsEnabled, (GLenum))                                                 \
  X(void, BlendFunc, (GLenum, GLenum))                                              \
  X(void, BlendFuncSeparate, (GLenum, GLenum, GLenum, GLenum))                      \
  X(void, BlendEquationSeparate, (GLenum, GLenum))                                  \
  X(void, BlendColor, (GLfloat, GLfloat, GLfloat, GLfloat))                         \
  X(void, ColorMask, (GLboolean, GLboolean, GLboolean, GLboolean))                  \
  X(void, DepthFunc, (GLenum))                                                      \
  X(void, DepthMask, (GLboolean))                                                   \
  X(void, DepthRangef, (GLfloat, GLfloat))                                          \
  X(void, StencilFunc, (GLenum, GLint, GLuint))                                     \
  X(void, StencilFuncSeparate, (GLenum, GLenum, GLint, GLuint))                     \
  X(void, StencilOp, (GLenum, GLenum, GLenum))                                      \
  X(void, StencilOpSeparate, (GLenum, GLenum, GLenum, GLenum))                      \
  X(void, StencilMask, (GLuint))                                                    \
  X(void, StencilMaskSeparate, (GLenum, GLuint))                                    \
  X(void, CullFace, (GLenum))                                                       \
  X(void, FrontFace, (GLenum))                                                      \
  X(void, PolygonOffset, (GLfloat, GLfloat))                                        \
  X(void, LineWidth, (GLfloat))                                                     \
  X(void, PixelStorei, (GLenum, GLint))                                             \
  X(void, ReadPixels, (GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void*))      \
  X(void, ReadBuffer, (GLenum))                                                     \
  X(void, GenTextures, (GLsizei, GLuint*))                                          \
  X(void, DeleteTextures, (GLsizei, const GLuint*))                                 \
  X(void, BindTexture, (GLenum, GLuint))                                            \
  X(void, ActiveTexture, (GLenum))                                                  \
  X(void, TexImage2D,                                                               \
    (GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void*))   \
  X(void, TexImage3D,                                                               \
    (GLenum, GLint, GLint, GLsizei, GLsizei, GLsizei, GLint, GLenum, GLenum,        \
     const void*))                                                                  \
  X(void, TexSubImage2D,                                                            \
    (GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, const void*))   \
  X(void, TexSubImage3D,                                                            \
    (GLenum, GLint, GLint, GLint, GLint, GLsizei, GLsizei, GLsizei, GLenum, GLenum, \
     const void*))                                                                  \
  X(void, TexParameteri, (GLenum, GLenum, GLint))                                   \
  X(void, TexParameterf, (GLenum, GLenum, GLfloat))                                 \
  X(void, TexParameterfv, (GLenum, GLenum, const GLfloat*))                         \
  X(void, GetTexParameteriv, (GLenum, GLenum, GLint*))                              \
  X(GLboolean, IsTexture, (GLuint))                                                 \
  X(void, GenerateMipmap, (GLenum))                                                 \
  X(void, GenSamplers, (GLsizei, GLuint*))                                          \
  X(void, DeleteSamplers, (GLsizei, const GLuint*))                                 \
  X(void, BindSampler, (GLuint, GLuint))                                            \
  X(void, SamplerParameteri, (GLuint, GLenum, GLint))                               \
  X(void, SamplerParameterf, (GLuint, GLenum, GLfloat))                             \
  X(void, SamplerParameterfv, (GLuint, GLenum, const GLfloat*))                     \
  X(void, GenBuffers, (GLsizei, GLuint*))                                           \
  X(void, DeleteBuffers, (GLsizei, const GLuint*))                                  \
  X(void, BindBuffer, (GLenum, GLuint))                                             \
  X(void, BufferData, (GLenum, GLsizeiptr, const void*, GLenum))                    \
  X(void, BufferSubData, (GLenum, GLintptr, GLsizeiptr, const void*))               \
  X(void, BindBufferBase, (GLenum, GLuint, GLuint))                                 \
  X(void, BindBufferRange, (GLenum, GLuint, GLuint, GLintptr, GLsizeiptr))          \
  X(void*, MapBufferRange, (GLenum, GLintptr, GLsizeiptr, GLbitfield))              \
  X(GLboolean, UnmapBuffer, (GLenum))                                               \
  X(void, GetBufferParameteriv, (GLenum, GLenum, GLint*))                           \
  X(void, GenVertexArrays, (GLsizei, GLuint*))                                      \
  X(void, DeleteVertexArrays, (GLsizei, const GLuint*))                             \
  X(void, BindVertexArray, (GLuint))                                                \
  X(void, EnableVertexAttribArray, (GLuint))                                        \
  X(void, DisableVertexAttribArray, (GLuint))                                       \
  X(void, VertexAttribPointer, (GLuint, GLint, GLenum, GLboolean, GLsizei, const void*)) \
  X(void, DrawArrays, (GLenum, GLint, GLsizei))                                     \
  X(void, DrawElements, (GLenum, GLsizei, GLenum, const void*))                     \
  X(GLuint, CreateShader, (GLenum))                                                 \
  X(void, DeleteShader, (GLuint))                                                   \
  X(void, ShaderSource, (GLuint, GLsizei, const GLchar* const*, const GLint*))      \
  X(void, CompileShader, (GLuint))                                                  \
  X(void, GetShaderiv, (GLuint, GLenum, GLint*))                                    \
  X(void, GetShaderInfoLog, (GLuint, GLsizei, GLsizei*, GLchar*))                   \
  X(GLuint, CreateProgram, (void))                                                  \
  X(void, DeleteProgram, (GLuint))                                                  \
  X(void, AttachShader, (GLuint, GLuint))                                           \
  X(void, DetachShader, (GLuint, GLuint))                                           \
  X(void, LinkProgram, (GLuint))                                                    \
  X(void, ValidateProgram, (GLuint))                                                \
  X(void, GetProgramiv, (GLuint, GLenum, GLint*))                                   \
  X(void, GetProgramInfoLog, (GLuint, GLsizei, GLsizei*, GLchar*))                  \
  X(void, UseProgram, (GLuint))                                                     \
  X(GLint, GetUniformLocation, (GLuint, const GLchar*))                             \
  X(void, GetActiveUniform, (GLuint, GLuint, GLsizei, GLsizei*, GLint*, GLenum*, GLchar*)) \
  X(void, GetUniformfv, (GLuint, GLint, GLfloat*))                                  \
  X(void, GetUniformiv, (GLuint, GLint, GLint*))                                    \
  X(void, GetUniformuiv, (GLuint, GLint, GLuint*))                                  \
  X(GLuint, GetUniformBlockIndex, (GLuint, const GLchar*))                           \
  X(void, UniformBlockBinding, (GLuint, GLuint, GLuint))                            \
  X(void, Uniform1i, (GLint, GLint))                                                \
  X(void, Uniform1ui, (GLint, GLuint))                                              \
  X(void, Uniform1f, (GLint, GLfloat))                                              \
  X(void, Uniform1fv, (GLint, GLsizei, const GLfloat*))                             \
  X(void, Uniform1iv, (GLint, GLsizei, const GLint*))                               \
  X(void, Uniform1uiv, (GLint, GLsizei, const GLuint*))                             \
  X(void, Uniform2f, (GLint, GLfloat, GLfloat))                                     \
  X(void, Uniform2fv, (GLint, GLsizei, const GLfloat*))                             \
  X(void, Uniform2uiv, (GLint, GLsizei, const GLuint*))                             \
  X(void, Uniform3f, (GLint, GLfloat, GLfloat, GLfloat))                            \
  X(void, Uniform3fv, (GLint, GLsizei, const GLfloat*))                             \
  X(void, Uniform4f, (GLint, GLfloat, GLfloat, GLfloat, GLfloat))                   \
  X(void, Uniform4fv, (GLint, GLsizei, const GLfloat*))                             \
  X(void, Uniform4uiv, (GLint, GLsizei, const GLuint*))                             \
  X(void, GenFramebuffers, (GLsizei, GLuint*))                                      \
  X(void, DeleteFramebuffers, (GLsizei, const GLuint*))                             \
  X(void, BindFramebuffer, (GLenum, GLuint))                                        \
  X(void, FramebufferTexture2D, (GLenum, GLenum, GLenum, GLuint, GLint))            \
  X(void, FramebufferTextureLayer, (GLenum, GLenum, GLuint, GLint, GLint))          \
  X(GLenum, CheckFramebufferStatus, (GLenum))                                       \
  X(void, DrawBuffers, (GLsizei, const GLenum*))                                    \
  X(void, ClearBufferfv, (GLenum, GLint, const GLfloat*))                           \
  X(void, BlitFramebuffer,                                                          \
    (GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum))    \
  X(void, InvalidateFramebuffer, (GLenum, GLsizei, const GLenum*))                  \
  X(void, PushDebugGroup, (GLenum, GLuint, GLsizei, const GLchar*))                 \
  X(void, PopDebugGroup, (void))                                                    \
  X(void, DebugMessageCallback, (void (*)(GLenum, GLenum, GLuint, GLenum, GLsizei, const GLchar*, const void*), const void*)) \
  X(void, DebugMessageControl, (GLenum, GLenum, GLenum, GLsizei, const GLuint*, GLboolean))

#define REX_GL_DECLARE(ret, name, args) using PFN_##name = ret (*) args;
REX_GL_FUNCTION_LIST(REX_GL_DECLARE)
#undef REX_GL_DECLARE

struct Functions {
#define REX_GL_MEMBER(ret, name, args) PFN_##name name = nullptr;
  REX_GL_FUNCTION_LIST(REX_GL_MEMBER)
#undef REX_GL_MEMBER
};

// The GL entry points live in the runtime library (rexruntime), because the
// presenter loads them there and the graphics plugin is a separately loaded
// dylib - an inline variable would give the plugin its own, empty copy.
Functions& GetFunctions();
using GetProcAddressFn = void* (*)(const char*);
bool LoadFunctions(GetProcAddressFn get_proc_address);

}  // namespace rex::gles
