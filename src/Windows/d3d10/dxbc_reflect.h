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

/* The parts of a DXBC container (a compiled Direct3D 10 / 11 shader) that d3d10.dll reads: the container itself and its checksum, the input and output signatures
 * (ISGN, OSGN, OSG5, ISG1, OSG1), the resource definitions (RDEF: constant buffers, their variables and types, the resource bindings), the statistics (STAT) and the
 * version token of the code (SHDR / SHEX). Plain C with no library underneath (a host test includes it): the input is untrusted - every offset and count is checked
 * against the bytes there are, with arithmetic that cannot wrap, and a string is only handed out when its terminating NUL is inside its chunk.
 *
 * Nothing is kept: each function reads the bytes it is given, so a caller asks again for what it needs (dxr_* "views" are pointers into the caller's buffer). */
#ifndef TCOS_DXBC_REFLECT_H
#define TCOS_DXBC_REFLECT_H

#include <stddef.h>
#include <stdint.h>

#define DXR_TAG(a, b, c, d) ((uint32_t)(a) | ((uint32_t)(b) << 8) | ((uint32_t)(c) << 16) | ((uint32_t)(d) << 24))
#define DXR_DXBC DXR_TAG('D', 'X', 'B', 'C')
#define DXR_RDEF DXR_TAG('R', 'D', 'E', 'F')
#define DXR_ISGN DXR_TAG('I', 'S', 'G', 'N')
#define DXR_OSGN DXR_TAG('O', 'S', 'G', 'N')
#define DXR_OSG5 DXR_TAG('O', 'S', 'G', '5')
#define DXR_ISG1 DXR_TAG('I', 'S', 'G', '1')
#define DXR_OSG1 DXR_TAG('O', 'S', 'G', '1')
#define DXR_PCSG DXR_TAG('P', 'C', 'S', 'G')
#define DXR_SHDR DXR_TAG('S', 'H', 'D', 'R')
#define DXR_SHEX DXR_TAG('S', 'H', 'E', 'X')
#define DXR_STAT DXR_TAG('S', 'T', 'A', 'T')
#define DXR_SDBG DXR_TAG('S', 'D', 'B', 'G')
#define DXR_AON9 DXR_TAG('A', 'o', 'n', '9')

static uint32_t dxr_u32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint16_t dxr_u16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static void dxr_put32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }
/* Whether `count` records of `each` bytes at `at` fit in `size` bytes (no wrap). */
static int dxr_fits(size_t size, size_t at, size_t count, size_t each) {
  if (at > size) return 0;
  if (each && count > (size - at) / each) return 0;
  return 1;
}
/* The NUL-terminated string at `at` of a chunk (NULL when it does not end inside it). */
static const char *dxr_string(const uint8_t *chunk, size_t size, uint32_t at) {
  if (at >= size) return NULL;
  for (size_t i = at; i < size; ++i)
    if (!chunk[i]) return (const char *)chunk + at;
  return NULL;
}
static int dxr_streq(const char *a, const char *b) {
  if (!a || !b) return 0;
  while (*a && *a == *b) ++a, ++b;
  return *a == *b;
}

/* ---- the container ----------------------------------------------------------------------------------------------------------------------------------------------- */

/* A container that is one: the magic, its size within the buffer, and every chunk header within it. The bytes of the container are `total` (from its header). */
static int dxr_container(const uint8_t *data, size_t size, size_t *total, uint32_t *chunks) {
  if (!data || size < 32 || dxr_u32(data) != DXR_DXBC) return 0;
  const uint32_t t = dxr_u32(data + 24), n = dxr_u32(data + 28);
  if (t < 32 || t > size || !dxr_fits(t, 32, n, 4)) return 0;
  for (uint32_t i = 0; i < n; ++i) {
    const uint32_t at = dxr_u32(data + 32 + 4 * (size_t)i);
    if (!dxr_fits(t, at, 1, 8)) return 0;
    if (dxr_u32(data + at + 4) > t - at - 8) return 0;
  }
  *total = t;
  *chunks = n;
  return 1;
}
/* Chunk `index`: its tag, its data and their bytes (the container must have passed dxr_container). */
static uint32_t dxr_chunk_at(const uint8_t *data, uint32_t index, const uint8_t **out, size_t *bytes) {
  const uint32_t at = dxr_u32(data + 32 + 4 * (size_t)index);
  *out = data + at + 8;
  *bytes = dxr_u32(data + at + 4);
  return dxr_u32(data + at);
}
/* The first chunk of a tag (0 when there is none, or the container is not one). */
static int dxr_find(const uint8_t *data, size_t size, uint32_t tag, const uint8_t **out, size_t *bytes) {
  size_t total;
  uint32_t n;
  if (!dxr_container(data, size, &total, &n)) return 0;
  for (uint32_t i = 0; i < n; ++i)
    if (dxr_chunk_at(data, i, out, bytes) == tag) return 1;
  return 0;
}

/* The container's checksum: MD5 over the bytes after the checksum field, with the length put in the last block as Direct3D does it (the bit count in the first word
 * of the last block, and (bits >> 2) | 1 in its last word, instead of MD5's 64-bit length). */
static uint32_t dxr_rol(uint32_t v, int s) { return (v << s) | (v >> (32 - s)); }
static void dxr_md5_block(uint32_t h[4], const uint8_t block[64]) {
  static const uint32_t k[64] = {
      0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501, 0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193,
      0xa679438e, 0x49b40821, 0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8, 0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed,
      0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a, 0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70, 0x289b7ec6, 0xeaa127fa,
      0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665, 0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
      0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391};
  static const int r[64] = {7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20,
                            4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};
  uint32_t m[16];
  for (int i = 0; i < 16; ++i) m[i] = dxr_u32(block + 4 * i);
  uint32_t a = h[0], b = h[1], c = h[2], d = h[3];
  for (int i = 0; i < 64; ++i) {
    uint32_t f;
    int g;
    if (i < 16) { f = (b & c) | (~b & d); g = i; }
    else if (i < 32) { f = (d & b) | (~d & c); g = (5 * i + 1) % 16; }
    else if (i < 48) { f = b ^ c ^ d; g = (3 * i + 5) % 16; }
    else { f = c ^ (b | ~d); g = (7 * i) % 16; }
    const uint32_t t = d;
    d = c;
    c = b;
    b = b + dxr_rol(a + f + k[i] + m[g], r[i]);
    a = t;
  }
  h[0] += a; h[1] += b; h[2] += c; h[3] += d;
}
/* The checksum of the `size` bytes of a container (size >= 20). */
static void dxr_checksum(const uint8_t *data, size_t size, uint32_t out[4]) {
  uint32_t h[4] = {0x67452301u, 0xefcdab89u, 0x98badcfeu, 0x10325476u};
  const uint8_t *p = data + 20;
  size_t left = size - 20;
  const uint32_t bits = (uint32_t)(left * 8);
  while (left >= 64) { dxr_md5_block(h, p); p += 64; left -= 64; }
  uint8_t block[64];
  for (int i = 0; i < 64; ++i) block[i] = 0;
  if (left >= 56) {
    for (size_t i = 0; i < left; ++i) block[i] = p[i];
    block[left] = 0x80;
    dxr_md5_block(h, block);
    for (int i = 0; i < 64; ++i) block[i] = 0;
    dxr_put32(block, bits);
    dxr_put32(block + 60, (bits >> 2) | 1);
    dxr_md5_block(h, block);
  } else {
    dxr_put32(block, bits);
    for (size_t i = 0; i < left; ++i) block[4 + i] = p[i];
    block[4 + left] = 0x80;
    dxr_put32(block + 60, (bits >> 2) | 1);
    dxr_md5_block(h, block);
  }
  for (int i = 0; i < 4; ++i) out[i] = h[i];
}
/* The bytes a container of `count` chunks of the given data sizes takes (0 when that overflows 32 bits). */
static size_t dxr_container_bytes(const size_t *sizes, uint32_t count) {
  uint64_t total = 32 + 4 * (uint64_t)count;
  for (uint32_t i = 0; i < count; ++i) total += 8 + (uint64_t)sizes[i];
  return total > 0xFFFFFFFFu ? 0 : (size_t)total;
}
/* Writes a container of the chunks into `out` (dxr_container_bytes of them), with its checksum. */
static void dxr_container_write(uint8_t *out, size_t bytes, const uint32_t *tags, const uint8_t *const *chunks, const size_t *sizes, uint32_t count) {
  dxr_put32(out, DXR_DXBC);
  for (int i = 4; i < 20; ++i) out[i] = 0;
  dxr_put32(out + 20, 1);
  dxr_put32(out + 24, (uint32_t)bytes);
  dxr_put32(out + 28, count);
  size_t at = 32 + 4 * (size_t)count;
  for (uint32_t i = 0; i < count; ++i) {
    dxr_put32(out + 32 + 4 * (size_t)i, (uint32_t)at);
    dxr_put32(out + at, tags[i]);
    dxr_put32(out + at + 4, (uint32_t)sizes[i]);
    for (size_t k = 0; k < sizes[i]; ++k) out[at + 8 + k] = chunks[i][k];
    at += 8 + sizes[i];
  }
  uint32_t sum[4];
  dxr_checksum(out, bytes, sum);
  for (int i = 0; i < 4; ++i) dxr_put32(out + 4 + 4 * i, sum[i]);
}

/* ---- signatures -------------------------------------------------------------------------------------------------------------------------------------------------- */

typedef struct DxrSignature {
  const char *name;
  uint32_t semantic_index, system_value, component_type, reg, stream, min_precision;
  uint8_t mask, rw_mask;
} DxrSignature;
/* The bytes of an element of a signature chunk: 24 (ISGN, OSGN), 28 (OSG5: the stream first), 32 (ISG1, OSG1: the stream first, the minimum precision last). */
static size_t dxr_signature_stride(uint32_t tag) { return tag == DXR_OSG5 ? 28 : (tag == DXR_ISG1 || tag == DXR_OSG1) ? 32 : 24; }
/* How many elements a signature chunk has (-1: not one). */
static int64_t dxr_signature_count(const uint8_t *chunk, size_t size, uint32_t tag) {
  if (size < 8) return -1;
  const uint32_t n = dxr_u32(chunk);
  if (!dxr_fits(size, 8, n, dxr_signature_stride(tag))) return -1;
  return n;
}
static int dxr_signature_element(const uint8_t *chunk, size_t size, uint32_t tag, uint32_t index, DxrSignature *e) {
  const int64_t n = dxr_signature_count(chunk, size, tag);
  if (n < 0 || index >= (uint64_t)n) return 0;
  const size_t stride = dxr_signature_stride(tag);
  const uint8_t *p = chunk + 8 + stride * index;
  uint32_t stream = 0;
  if (stride >= 28) { stream = dxr_u32(p); p += 4; }
  e->name = dxr_string(chunk, size, dxr_u32(p));
  if (!e->name) return 0;
  e->semantic_index = dxr_u32(p + 4);
  e->system_value = dxr_u32(p + 8);
  e->component_type = dxr_u32(p + 12);
  e->reg = dxr_u32(p + 16);
  e->mask = p[20];
  e->rw_mask = p[21];
  e->stream = stream;
  e->min_precision = stride == 32 ? dxr_u32(p + 24) : 0;
  return 1;
}
/* The output signature of a container: OSG1, OSG5 or OSGN, whichever there is first in that order. */
static int dxr_output_signature(const uint8_t *data, size_t size, const uint8_t **chunk, size_t *bytes, uint32_t *tag) {
  static const uint32_t tags[3] = {DXR_OSG1, DXR_OSG5, DXR_OSGN};
  for (int i = 0; i < 3; ++i)
    if (dxr_find(data, size, tags[i], chunk, bytes)) { *tag = tags[i]; return 1; }
  return 0;
}
static int dxr_input_signature(const uint8_t *data, size_t size, const uint8_t **chunk, size_t *bytes, uint32_t *tag) {
  if (dxr_find(data, size, DXR_ISG1, chunk, bytes)) { *tag = DXR_ISG1; return 1; }
  if (dxr_find(data, size, DXR_ISGN, chunk, bytes)) { *tag = DXR_ISGN; return 1; }
  return 0;
}

/* ---- resource definitions ------------------------------------------------------------------------------------------------------------------------------------ */

typedef struct DxrRdef {
  const uint8_t *chunk;
  size_t size;
  uint32_t cb_count, cb_offset, bind_count, bind_offset, target, flags;
  const char *creator;
  uint32_t var_stride, type_stride;      /* (24 and 16 below shader model 5; 40 and 36 from it, after the RD11 header) */
} DxrRdef;
typedef struct DxrBinding { const char *name; uint32_t type, return_type, dimension, samples, bind_point, bind_count, flags, space, id; } DxrBinding;
typedef struct DxrBuffer { const char *name; uint32_t var_count, var_offset, size, flags, type; } DxrBuffer;
typedef struct DxrVariable { const char *name; uint32_t offset, size, flags, type_offset, default_offset; } DxrVariable;
typedef struct DxrType { uint32_t cls, type, rows, columns, elements, members, member_offset; const char *name; } DxrType;
typedef struct DxrMember { const char *name; uint32_t type_offset, offset; } DxrMember;

static int dxr_rdef(const uint8_t *chunk, size_t size, DxrRdef *r) {
  if (size < 28) return 0;
  r->chunk = chunk;
  r->size = size;
  r->cb_count = dxr_u32(chunk);
  r->cb_offset = dxr_u32(chunk + 4);
  r->bind_count = dxr_u32(chunk + 8);
  r->bind_offset = dxr_u32(chunk + 12);
  r->target = dxr_u32(chunk + 16);
  r->flags = dxr_u32(chunk + 20);
  r->creator = dxr_string(chunk, size, dxr_u32(chunk + 24));
  r->var_stride = 24;
  r->type_stride = 16;
  if (((r->target >> 8) & 0xFF) >= 5) {
    if (size < 60 || dxr_u32(chunk + 28) != DXR_TAG('R', 'D', '1', '1')) return 0;
    r->var_stride = 40;
    r->type_stride = 36;
  }
  if (!dxr_fits(size, r->cb_offset, r->cb_count, 24) || !dxr_fits(size, r->bind_offset, r->bind_count, r->var_stride == 40 ? 40 : 32)) return 0;
  return 1;
}
static int dxr_rdef_binding(const DxrRdef *r, uint32_t index, DxrBinding *b) {
  if (index >= r->bind_count) return 0;
  const size_t stride = r->var_stride == 40 ? 40 : 32;
  const uint8_t *p = r->chunk + r->bind_offset + stride * index;
  b->name = dxr_string(r->chunk, r->size, dxr_u32(p));
  if (!b->name) return 0;
  b->type = dxr_u32(p + 4);
  b->return_type = dxr_u32(p + 8);
  b->dimension = dxr_u32(p + 12);
  b->samples = dxr_u32(p + 16);
  b->bind_point = dxr_u32(p + 20);
  b->bind_count = dxr_u32(p + 24);
  b->flags = dxr_u32(p + 28);
  b->space = stride == 40 ? dxr_u32(p + 32) : 0;
  b->id = stride == 40 ? dxr_u32(p + 36) : 0;
  return 1;
}
static int dxr_rdef_buffer(const DxrRdef *r, uint32_t index, DxrBuffer *c) {
  if (index >= r->cb_count) return 0;
  const uint8_t *p = r->chunk + r->cb_offset + 24 * (size_t)index;
  c->name = dxr_string(r->chunk, r->size, dxr_u32(p));
  if (!c->name) return 0;
  c->var_count = dxr_u32(p + 4);
  c->var_offset = dxr_u32(p + 8);
  c->size = dxr_u32(p + 12);
  c->flags = dxr_u32(p + 16);
  c->type = dxr_u32(p + 20);
  if (!dxr_fits(r->size, c->var_offset, c->var_count, r->var_stride)) return 0;
  return 1;
}
static int dxr_rdef_variable(const DxrRdef *r, const DxrBuffer *c, uint32_t index, DxrVariable *v) {
  if (index >= c->var_count) return 0;
  const uint8_t *p = r->chunk + c->var_offset + (size_t)r->var_stride * index;
  v->name = dxr_string(r->chunk, r->size, dxr_u32(p));
  if (!v->name) return 0;
  v->offset = dxr_u32(p + 4);
  v->size = dxr_u32(p + 8);
  v->flags = dxr_u32(p + 12);
  v->type_offset = dxr_u32(p + 16);
  v->default_offset = dxr_u32(p + 20);
  if (v->default_offset && (v->default_offset > r->size || v->size > r->size - v->default_offset)) return 0;      /* (the default value is the variable's bytes) */
  return 1;
}
static int dxr_rdef_type(const DxrRdef *r, uint32_t at, DxrType *t) {
  if (!dxr_fits(r->size, at, 1, r->type_stride)) return 0;
  const uint8_t *p = r->chunk + at;
  t->cls = dxr_u16(p);
  t->type = dxr_u16(p + 2);
  t->rows = dxr_u16(p + 4);
  t->columns = dxr_u16(p + 6);
  t->elements = dxr_u16(p + 8);
  t->members = dxr_u16(p + 10);
  t->member_offset = dxr_u32(p + 12);
  t->name = NULL;
  if (r->type_stride == 36) {
    const uint32_t name = dxr_u32(p + 32);
    if (name) t->name = dxr_string(r->chunk, r->size, name);
  }
  if (t->members && !dxr_fits(r->size, t->member_offset, t->members, 12)) return 0;
  return 1;
}
static int dxr_rdef_member(const DxrRdef *r, const DxrType *t, uint32_t index, DxrMember *m) {
  if (index >= t->members) return 0;
  const uint8_t *p = r->chunk + t->member_offset + 12 * (size_t)index;
  m->name = dxr_string(r->chunk, r->size, dxr_u32(p));
  if (!m->name) return 0;
  m->type_offset = dxr_u32(p + 4);
  m->offset = dxr_u32(p + 8);
  return 1;
}

/* ---- the code and its statistics ------------------------------------------------------------------------------------------------------------------------------- */

/* The code chunk (SHDR, or SHEX of shader model 5): its words. FALSE without one, or with a version token and length that do not fit. */
static int dxr_code(const uint8_t *data, size_t size, const uint8_t **code, size_t *words, int *shex) {
  const uint8_t *c;
  size_t bytes;
  *shex = 0;
  if (!dxr_find(data, size, DXR_SHDR, &c, &bytes)) {
    if (!dxr_find(data, size, DXR_SHEX, &c, &bytes)) return 0;
    *shex = 1;
  }
  if (bytes < 8) return 0;
  const uint32_t n = dxr_u32(c + 4);
  if (n < 2 || n > bytes / 4) return 0;
  *code = c;
  *words = n;
  return 1;
}
/* STAT: the counts the compiler gave (29 words below shader model 5, 37 from it). `count` words are read; the rest are 0. */
static int dxr_stat(const uint8_t *data, size_t size, uint32_t *out, uint32_t count) {
  const uint8_t *c;
  size_t bytes;
  for (uint32_t i = 0; i < count; ++i) out[i] = 0;
  if (!dxr_find(data, size, DXR_STAT, &c, &bytes)) return 0;
  for (uint32_t i = 0; i < count && 4 * (size_t)i + 4 <= bytes; ++i) out[i] = dxr_u32(c + 4 * (size_t)i);
  return 1;
}
#define DXR_STAT_INSTRUCTIONS 0
#define DXR_STAT_TEMPS 1
#define DXR_STAT_DEFS 2
#define DXR_STAT_DCLS 3
#define DXR_STAT_FLOAT 4
#define DXR_STAT_INT 5
#define DXR_STAT_UINT 6
#define DXR_STAT_STATIC_FLOW 7
#define DXR_STAT_DYNAMIC_FLOW 8
#define DXR_STAT_MACRO 9
#define DXR_STAT_TEMP_ARRAYS 10
#define DXR_STAT_ARRAY 11
#define DXR_STAT_CUT 12
#define DXR_STAT_EMIT 13
#define DXR_STAT_TEX_NORMAL 14
#define DXR_STAT_TEX_LOAD 15
#define DXR_STAT_TEX_COMP 16
#define DXR_STAT_TEX_BIAS 17
#define DXR_STAT_TEX_GRAD 18
#define DXR_STAT_MOV 19
#define DXR_STAT_MOVC 20
#define DXR_STAT_CONVERSION 21
#define DXR_STAT_BITWISE 22
#define DXR_STAT_INPUT_PRIMITIVE 23
#define DXR_STAT_GS_TOPOLOGY 24
#define DXR_STAT_GS_MAX_VERTICES 25
#define DXR_STAT_SAMPLE_FREQUENCY 28
#define DXR_STAT_WORDS 29

#endif
