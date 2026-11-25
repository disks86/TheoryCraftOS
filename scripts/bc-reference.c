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

// The reference data of tests/BlockCompressionTest.cpp for BC4, BC5, BC6H and BC7 (RGTC and BPTC in OpenGL): random blocks (deterministic, with every BC6H and BC7
// mode forced into some of them) decoded by Mesa's softpipe driver through glCompressedTexImage2D and glGetTexImage, written to tests/data/bc/reference.bin.
// softpipe, not llvmpipe: llvmpipe samples RGTC natively with approximate divisions (x / 5 as x * 51 >> 8: up to 2 / 255 off), softpipe decodes it with Mesa's C decoder
// (util_format, 8-bit interpolation truncated). BPTC comes out the same from both (Mesa transcodes it on the CPU at upload).
//
// Build and run (from the repository root):
//   gcc -O1 -o /tmp/bc-reference scripts/bc-reference.c -lEGL
//   __EGL_VENDOR_LIBRARY_FILENAMES=/usr/share/glvnd/egl_vendor.d/50_mesa.json LIBGL_ALWAYS_SOFTWARE=1 GALLIUM_DRIVER=softpipe /tmp/bc-reference tests/data/bc/reference.bin
//
// The file: "TCBC", a u32 count, then per format: u32 GL internal format, u32 width, u32 height, u32 GL read-back type, u32 byte count of the blocks, the blocks,
// u32 byte count of the pixels, the pixels (RGBA, rows top to bottom as the blocks are). Read-back types: GL_UNSIGNED_BYTE (RGTC unorm, BPTC unorm and sRGB: the
// 8-bit values; sRGB is not linearised by glGetTexImage), GL_BYTE (RGTC signed: the signed bytes -127 .. 127 Mesa decodes to), GL_HALF_FLOAT (BPTC float: the half bits).
// All values little endian.
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef unsigned int GLenum;
typedef unsigned int GLuint;
typedef int GLint;
typedef int GLsizei;
typedef void (*PFN_GenTextures)(GLsizei, GLuint*);
typedef void (*PFN_BindTexture)(GLenum, GLuint);
typedef void (*PFN_CompressedTexImage2D)(GLenum, GLint, GLenum, GLsizei, GLsizei, GLint, GLsizei, const void*);
typedef void (*PFN_GetTexImage)(GLenum, GLint, GLenum, GLenum, void*);
typedef void (*PFN_PixelStorei)(GLenum, GLint);
typedef GLenum (*PFN_GetError)(void);

#define GL_TEXTURE_2D 0x0DE1
#define GL_RGBA 0x1908
#define GL_BYTE 0x1400
#define GL_UNSIGNED_BYTE 0x1401
#define GL_HALF_FLOAT 0x140B
#define GL_PACK_ALIGNMENT 0x0D05
#define GL_UNPACK_ALIGNMENT 0x0CF5

enum { kWidth = 64, kHeight = 64, kBlocks = (kWidth / 4) * (kHeight / 4) };

static uint64_t g_state = 0x9E3779B97F4A7C15ull;
static uint32_t next_random(void) {
  g_state ^= g_state << 13;
  g_state ^= g_state >> 7;
  g_state ^= g_state << 17;
  return (uint32_t)(g_state >> 16);
}

static void put32(FILE* f, uint32_t v) {
  uint8_t b[4] = {(uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24)};
  fwrite(b, 1, 4, f);
}

// The low bits of the first byte of BC6H blocks: the two 2-bit modes (twice, they are common), the twelve 5-bit modes and the four reserved ones.
static const uint8_t kBc6Modes[] = {0x00, 0x01, 0x00, 0x01, 0x02, 0x06, 0x0A, 0x0E, 0x12, 0x16, 0x1A, 0x1E, 0x03, 0x07, 0x0B, 0x0F, 0x13, 0x17, 0x1B, 0x1F};

static void make_blocks(GLenum format, uint8_t* blocks, uint32_t block_bytes) {
  for (uint32_t i = 0; i < kBlocks; ++i) {
    uint8_t* b = blocks + i * block_bytes;
    for (uint32_t j = 0; j < block_bytes; ++j) b[j] = (uint8_t)next_random();
    if (format == 0x8E8C || format == 0x8E8D) {   // BC7: mode i % 9 (8: no mode bit)
      const uint32_t mode = i % 9;
      if (mode == 8) b[0] = 0;
      else b[0] = (uint8_t)((b[0] & ~((2u << mode) - 1)) | (1u << mode));
    } else if (format == 0x8E8E || format == 0x8E8F) {
      const uint8_t m = kBc6Modes[i % sizeof kBc6Modes];
      b[0] = (uint8_t)((b[0] & (m <= 1 ? ~3u : ~31u)) | m);
    } else if (i % 4 == 1) {   // RGTC: make both ramp modes common, and the -128 endpoint
      if (b[0] > b[1]) { uint8_t t = b[0]; b[0] = b[1]; b[1] = t; }
    } else if (i % 4 == 2) {
      b[0] = 0x80;
    }
    if ((format == 0x8DBD || format == 0x8DBE) && i % 4 == 3) b[9] = 0x80;
  }
}

int main(int argc, char** argv) {
  if (argc != 2) { fprintf(stderr, "usage: %s out.bin\n", argv[0]); return 2; }
  PFNEGLGETPLATFORMDISPLAYEXTPROC get_display = (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
  EGLDisplay dpy = get_display ? get_display(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL) : EGL_NO_DISPLAY;
  if (dpy == EGL_NO_DISPLAY || !eglInitialize(dpy, NULL, NULL)) { fprintf(stderr, "no surfaceless display\n"); return 1; }
  if (!eglBindAPI(EGL_OPENGL_API)) { fprintf(stderr, "no desktop GL\n"); return 1; }
  const EGLint cfg_attr[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT, EGL_NONE};
  EGLConfig cfg;
  EGLint n = 0;
  if (!eglChooseConfig(dpy, cfg_attr, &cfg, 1, &n) || n < 1) { fprintf(stderr, "no config\n"); return 1; }
  const EGLint ctx_attr[] = {EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 3, EGL_NONE};
  EGLContext ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctx_attr);
  if (ctx == EGL_NO_CONTEXT || !eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx)) { fprintf(stderr, "no context\n"); return 1; }
  const char* (*get_string)(GLenum) = (const char* (*)(GLenum))eglGetProcAddress("glGetString");
  fprintf(stderr, "renderer: %s, version %s\n", get_string(0x1F01), get_string(0x1F02));
  PFN_GenTextures gen = (PFN_GenTextures)eglGetProcAddress("glGenTextures");
  PFN_BindTexture bind = (PFN_BindTexture)eglGetProcAddress("glBindTexture");
  PFN_CompressedTexImage2D upload = (PFN_CompressedTexImage2D)eglGetProcAddress("glCompressedTexImage2D");
  PFN_GetTexImage readback = (PFN_GetTexImage)eglGetProcAddress("glGetTexImage");
  PFN_PixelStorei store = (PFN_PixelStorei)eglGetProcAddress("glPixelStorei");
  PFN_GetError error = (PFN_GetError)eglGetProcAddress("glGetError");
  store(GL_PACK_ALIGNMENT, 1);
  store(GL_UNPACK_ALIGNMENT, 1);

  static const struct { GLenum format, type; uint32_t block_bytes, pixel_bytes; } kFormats[] = {
      {0x8DBB, GL_UNSIGNED_BYTE, 8, 4},  {0x8DBC, GL_BYTE, 8, 4}, {0x8DBD, GL_UNSIGNED_BYTE, 16, 4}, {0x8DBE, GL_BYTE, 16, 4},
      {0x8E8C, GL_UNSIGNED_BYTE, 16, 4}, {0x8E8D, GL_UNSIGNED_BYTE, 16, 4}, {0x8E8E, GL_HALF_FLOAT, 16, 8}, {0x8E8F, GL_HALF_FLOAT, 16, 8},
  };
  FILE* f = fopen(argv[1], "wb");
  if (!f) { perror(argv[1]); return 1; }
  fwrite("TCBC", 1, 4, f);
  put32(f, sizeof kFormats / sizeof kFormats[0]);
  static uint8_t blocks[kBlocks * 16], pixels[kWidth * kHeight * 8];
  for (size_t i = 0; i < sizeof kFormats / sizeof kFormats[0]; ++i) {
    const uint32_t block_bytes = kFormats[i].block_bytes, size = kBlocks * block_bytes, pixel_size = kWidth * kHeight * kFormats[i].pixel_bytes;
    make_blocks(kFormats[i].format, blocks, block_bytes);
    GLuint tex = 0;
    gen(1, &tex);
    bind(GL_TEXTURE_2D, tex);
    upload(GL_TEXTURE_2D, 0, kFormats[i].format, kWidth, kHeight, 0, (GLsizei)size, blocks);
    memset(pixels, 0xCD, sizeof pixels);
    readback(GL_TEXTURE_2D, 0, GL_RGBA, kFormats[i].type, pixels);
    const GLenum e = error();
    if (e) { fprintf(stderr, "format 0x%X: GL error 0x%X\n", kFormats[i].format, e); return 1; }
    put32(f, kFormats[i].format);
    put32(f, kWidth);
    put32(f, kHeight);
    put32(f, kFormats[i].type);
    put32(f, size);
    fwrite(blocks, 1, size, f);
    put32(f, pixel_size);
    fwrite(pixels, 1, pixel_size, f);
  }
  fclose(f);
  return 0;
}
