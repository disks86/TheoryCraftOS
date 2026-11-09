/*
 * TheoryCraftOS - a minimal UEFI x86_64 operating system.
 * Copyright (C) 2026 Christopher Schaefer
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published
 * by the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

/* The user-mode side of the kernel's OpenGL, shared by every personality's library (opengl32.dll; the host tests): GL's types and the command buffer that the generated entry
 * points (src/OpenGl/generated/GlEntries.inc) write into. The includer provides
 *   GlStream *gl_stream_current(void)                       the calling thread's buffer (never null)
 *   uint64_t gl_stream_submit(const uint32_t *words, uint32_t count)   sends `count` words to the kernel and returns the last result (0 with no current context)
 * The wire format is in src/OpenGl/GlStream.hpp. Plain C so a C library can include it. */
#ifndef TCOS_GL_CLIENT_H
#define TCOS_GL_CLIENT_H
#include <stddef.h>
#include <stdint.h>

#ifndef GLAPI
#define GLAPI
#endif
#ifndef APIENTRY
#define APIENTRY
#endif

typedef unsigned int GLenum;
typedef unsigned char GLboolean;
typedef unsigned int GLbitfield;
typedef void GLvoid;
typedef signed char GLbyte;
typedef short GLshort;
typedef int GLint;
typedef int GLclampx;
typedef unsigned char GLubyte;
typedef unsigned short GLushort;
typedef unsigned int GLuint;
typedef int GLsizei;
typedef float GLfloat;
typedef float GLclampf;
typedef double GLdouble;
typedef double GLclampd;
typedef char GLchar;
typedef char GLcharARB;
typedef unsigned short GLhalf;
typedef unsigned short GLhalfARB;
typedef unsigned short GLhalfNV;
typedef int GLfixed;
typedef unsigned int GLhandleARB;
typedef int64_t GLint64;
typedef int64_t GLint64EXT;
typedef uint64_t GLuint64;
typedef uint64_t GLuint64EXT;
typedef intptr_t GLintptr;
typedef intptr_t GLintptrARB;
typedef intptr_t GLsizeiptr;
typedef intptr_t GLsizeiptrARB;
typedef struct __GLsync *GLsync;
typedef void *GLeglImageOES;
typedef void *GLeglClientBufferEXT;
typedef intptr_t GLvdpauSurfaceNV;
struct _cl_context;
struct _cl_event;
typedef void (APIENTRY *GLDEBUGPROC)(GLenum source, GLenum type, GLuint id, GLenum severity, GLsizei length, const GLchar *message, const void *userParam);
typedef GLDEBUGPROC GLDEBUGPROCARB;
typedef GLDEBUGPROC GLDEBUGPROCKHR;
typedef void (APIENTRY *GLDEBUGPROCAMD)(GLuint id, GLenum category, GLenum severity, GLsizei length, const GLchar *message, void *userParam);
typedef void (APIENTRY *GLVULKANPROCNV)(void);

#define GL_STREAM_WORDS 16384u      /* kMaxStreamWords */

typedef struct GlStream {
  uint32_t used;
  uint32_t words[GL_STREAM_WORDS];
} GlStream;

GlStream *gl_stream_current(void);
uint64_t gl_stream_submit(const uint32_t *words, uint32_t count);

static inline uint32_t *gl_stream_begin(uint32_t op, uint32_t words) {
  GlStream *s = gl_stream_current();
  if (s->used + words > GL_STREAM_WORDS) {
    gl_stream_submit(s->words, s->used);
    s->used = 0;
  }
  uint32_t *at = &s->words[s->used];
  at[0] = op | words << 16;
  return at;
}
static inline void gl_stream_end(uint32_t words) { gl_stream_current()->used += words; }
static inline uint64_t gl_stream_sync(uint32_t words) {
  GlStream *s = gl_stream_current();
  s->used += words;
  const uint64_t r = gl_stream_submit(s->words, s->used);
  s->used = 0;
  return r;
}
/* Sends what is queued (before the thread's context changes, a swap, a flush). */
static inline void gl_stream_flush(void) {
  GlStream *s = gl_stream_current();
  if (s->used) gl_stream_submit(s->words, s->used);
  s->used = 0;
}
static inline uint32_t gl_stream_f32(float f) {
  union { float f; uint32_t u; } v;
  v.f = f;
  return v.u;
}
static inline void gl_stream_u64(uint32_t *at, uint64_t v) { at[0] = (uint32_t)v; at[1] = (uint32_t)(v >> 32); }
static inline void gl_stream_f64(uint32_t *at, double d) {
  union { double d; uint64_t u; } v;
  v.d = d;
  gl_stream_u64(at, v.u);
}
/* An array of a size the registry fixes: its bytes, rounded up to words (the tail of the last word is zero). */
static inline void gl_stream_copy(uint32_t *at, const void *from, uint32_t bytes) {
  const unsigned char *p = (const unsigned char *)from;
  uint32_t i;
  for (i = 0; i < (bytes + 3) / 4; ++i) at[i] = 0;
  for (i = 0; i < bytes; ++i) at[i / 4] |= (uint32_t)p[i] << (8 * (i % 4));
}

#endif
