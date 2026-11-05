#pragma once

// The OpenGL ES 2 subset the GE renderer uses. On the PS Vita it comes from
// vitaGL; on Windows from opengl32 plus the entry points loaded at run time
// (a hidden window provides the context, see gl_context_win.cpp).

#include <string>

#if defined(__vita__)
#include <vitaGL.h>
#else
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <GL/gl.h>

#include <cstddef>

using GLchar = char;
using GLsizeiptr = std::ptrdiff_t;

#define GL_FRAGMENT_SHADER 0x8B30
#define GL_VERTEX_SHADER 0x8B31
#define GL_COMPILE_STATUS 0x8B81
#define GL_LINK_STATUS 0x8B82
#define GL_INFO_LOG_LENGTH 0x8B84
#define GL_FRAMEBUFFER 0x8D40
#define GL_RENDERBUFFER 0x8D41
#define GL_COLOR_ATTACHMENT0 0x8CE0
#define GL_DEPTH_ATTACHMENT 0x8D00
#define GL_STENCIL_ATTACHMENT 0x8D20
#define GL_DEPTH_STENCIL_ATTACHMENT 0x821A
#define GL_DEPTH24_STENCIL8 0x88F0
#define GL_FRAMEBUFFER_COMPLETE 0x8CD5
#define GL_TEXTURE0 0x84C0
#define GL_CLAMP_TO_EDGE 0x812F
#define GL_FUNC_ADD 0x8006
#define GL_MIN 0x8007
#define GL_MAX 0x8008
#define GL_FUNC_SUBTRACT 0x800A
#define GL_FUNC_REVERSE_SUBTRACT 0x800B

#define BURNOUT_GL_FUNCTIONS(X)                                                                          \
    X(GLuint, glCreateShader, (GLenum type))                                                             \
    X(void, glShaderSource, (GLuint shader, GLsizei count, const GLchar *const *source, const GLint *len)) \
    X(void, glCompileShader, (GLuint shader))                                                            \
    X(void, glGetShaderiv, (GLuint shader, GLenum name, GLint *value))                                   \
    X(void, glGetShaderInfoLog, (GLuint shader, GLsizei size, GLsizei *length, GLchar *log))             \
    X(GLuint, glCreateProgram, ())                                                                       \
    X(void, glAttachShader, (GLuint program, GLuint shader))                                             \
    X(void, glBindAttribLocation, (GLuint program, GLuint index, const GLchar *name))                    \
    X(void, glLinkProgram, (GLuint program))                                                             \
    X(void, glGetProgramiv, (GLuint program, GLenum name, GLint *value))                                 \
    X(void, glGetProgramInfoLog, (GLuint program, GLsizei size, GLsizei *length, GLchar *log))           \
    X(void, glUseProgram, (GLuint program))                                                              \
    X(GLint, glGetUniformLocation, (GLuint program, const GLchar *name))                                 \
    X(void, glUniform1i, (GLint location, GLint v0))                                                     \
    X(void, glUniform1f, (GLint location, GLfloat v0))                                                   \
    X(void, glUniform2f, (GLint location, GLfloat v0, GLfloat v1))                                       \
    X(void, glUniform4f, (GLint location, GLfloat v0, GLfloat v1, GLfloat v2, GLfloat v3))               \
    X(void, glUniform4fv, (GLint location, GLsizei count, const GLfloat *v))                             \
    X(void, glUniformMatrix4fv, (GLint location, GLsizei count, GLboolean transpose, const GLfloat *v))  \
    X(void, glVertexAttrib4f, (GLuint index, GLfloat x, GLfloat y, GLfloat z, GLfloat w))                \
    X(void, glVertexAttribPointer,                                                                       \
      (GLuint index, GLint size, GLenum type, GLboolean normalized, GLsizei stride, const void *pointer)) \
    X(void, glEnableVertexAttribArray, (GLuint index))                                                   \
    X(void, glDisableVertexAttribArray, (GLuint index))                                                  \
    X(void, glGenFramebuffers, (GLsizei n, GLuint *framebuffers))                                        \
    X(void, glBindFramebuffer, (GLenum target, GLuint framebuffer))                                      \
    X(void, glFramebufferTexture2D, (GLenum target, GLenum attachment, GLenum textarget, GLuint texture, GLint level)) \
    X(void, glGenRenderbuffers, (GLsizei n, GLuint *renderbuffers))                                      \
    X(void, glBindRenderbuffer, (GLenum target, GLuint renderbuffer))                                    \
    X(void, glRenderbufferStorage, (GLenum target, GLenum format, GLsizei width, GLsizei height))        \
    X(void, glFramebufferRenderbuffer, (GLenum target, GLenum attachment, GLenum rbtarget, GLuint rb))   \
    X(GLenum, glCheckFramebufferStatus, (GLenum target))                                                 \
    X(void, glBlendEquation, (GLenum mode))                                                              \
    X(void, glActiveTexture, (GLenum texture))                                                           \
    X(void, glGenerateMipmap, (GLenum target))

#define BURNOUT_GL_DECLARE(ret, name, args) \
    using PFN_##name = ret(APIENTRY *) args; \
    extern PFN_##name name;
BURNOUT_GL_FUNCTIONS(BURNOUT_GL_DECLARE)
#undef BURNOUT_GL_DECLARE
#endif

namespace burnout::gl {

// Creates the platform context: a hidden 1x1 window with a compatibility
// context on Windows, vglInit on the Vita. Returns false with a message.
bool create_context(std::string &error);
// Shows the default framebuffer (Vita only; no-op on Windows).
void swap_buffers();

} // namespace burnout::gl
