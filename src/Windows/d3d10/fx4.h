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

/* The binary of a Direct3D 10 effect (fx_4_0 / fx_4_1: the FX10 part of a DXBC container) as a plain description, for d3d10.dll's effects (effect.inc) and for a host
 * test (tests/Fx4ParserTest.cpp). Dependency-free C: no windows.h, no C library - memory comes from the caller's allocator, every offset and count read from the input is
 * checked against the input's size before it is used, and size arithmetic is written so that it cannot wrap.
 *
 * The FX10 part: a header of 19 words (the version, the counts of what follows, the size of the unstructured data), the unstructured data (names, types, default values,
 * shaders, expressions - found by offsets from its start) and the structured data read in order: the local constant buffers with their variables, the object variables,
 * the shared buffers and objects (a child effect's description of what its pool has), the techniques with their passes. State assignments (of passes and of state
 * objects) are an id from the table of properties, an index, an operation and the offset of the operation's data: constants, a variable, an element of an array variable,
 * an expression for the index of an array variable, an expression for the value, or a shader of its own. Expressions are "preshaders": a DXBC container of a literal table
 * (CLI4), the variables they read (CTAB) and the instructions (FXLC) - fx4_preshader_* validates them and evaluates them.
 *
 * Strings point into the input (which must outlive the description); empty strings are NULL, as the effect interfaces answer them. */
#ifndef TCOS_FX4_H
#define TCOS_FX4_H

#include <stddef.h>
#include <stdint.h>

/* ---- results -------------------------------------------------------------------------------------------------------------------------------------------------------- */

enum { FX4_OK = 0, FX4_BAD = 1, FX4_NO_MEMORY = 2 };

typedef struct Fx4Alloc {
  void *(*alloc)(void *ctx, size_t bytes);      /* zeroed memory, or NULL */
  void (*release)(void *ctx, void *p);
  void *ctx;
} Fx4Alloc;

/* ---- the description ---------------------------------------------------------------------------------------------------------------------------------------------- */

#define FX4_VERSION_4_0 0xFEFF1001u
#define FX4_VERSION_4_1 0xFEFF1011u

enum { FX4_KIND_NUMERIC = 1, FX4_KIND_OBJECT = 2, FX4_KIND_STRUCT = 3 };
/* (the classes and base types of numeric types, as the binary has them) */
enum { FX4_CLASS_SCALAR = 1, FX4_CLASS_VECTOR = 2, FX4_CLASS_MATRIX = 3 };
enum { FX4_BASE_FLOAT = 1, FX4_BASE_INT = 2, FX4_BASE_UINT = 3, FX4_BASE_BOOL = 4 };
/* (the object kinds) */
enum {
  FX4_OBJ_STRING = 1, FX4_OBJ_BLEND = 2, FX4_OBJ_DEPTHSTENCIL = 3, FX4_OBJ_RASTERIZER = 4, FX4_OBJ_PIXELSHADER = 5, FX4_OBJ_VERTEXSHADER = 6, FX4_OBJ_GEOMETRYSHADER = 7,
  FX4_OBJ_GEOMETRYSHADER_SO = 8, FX4_OBJ_TEXTURE = 9, FX4_OBJ_TEXTURE1D = 10, FX4_OBJ_TEXTURE1DARRAY = 11, FX4_OBJ_TEXTURE2D = 12, FX4_OBJ_TEXTURE2DARRAY = 13,
  FX4_OBJ_TEXTURE2DMS = 14, FX4_OBJ_TEXTURE2DMSARRAY = 15, FX4_OBJ_TEXTURE3D = 16, FX4_OBJ_TEXTURECUBE = 17, FX4_OBJ_RENDERTARGETVIEW = 19, FX4_OBJ_DEPTHSTENCILVIEW = 20,
  FX4_OBJ_SAMPLER = 21, FX4_OBJ_BUFFER = 22, FX4_OBJ_TEXTURECUBEARRAY = 23
};

typedef struct Fx4Type Fx4Type;
typedef struct Fx4Member { const char *name, *semantic; uint32_t offset; Fx4Type *type; } Fx4Member;
struct Fx4Type {
  uint32_t id;                         /* where it is in the unstructured data (types are shared by that) */
  const char *name;
  uint32_t kind, elements, unpacked, stride, packed;
  uint32_t numeric_class, numeric_base, rows, columns, column_major;      /* (numeric) */
  uint32_t object;                     /* (object: FX4_OBJ_*) */
  uint32_t member_count;               /* (struct) */
  Fx4Member *members;
};

/* An annotation: numeric values (packed: type->packed bytes) or the strings of a string (one, or one an element). */
typedef struct Fx4Annotation { const char *name; Fx4Type *type; const uint8_t *values; const char **strings; } Fx4Annotation;
typedef struct Fx4Annotations { uint32_t count; Fx4Annotation *items; } Fx4Annotations;

/* An expression: the literals (CLI4), the instructions (FXLC: a count, then each instruction's token, input count and (flags, table, offset) a parameter), the variables read
 * into the constant table (CTAB: a name and the registers, four floats each) and the sizes the tables must have. */
enum { FX4_TABLE_CONSTANTS = 1, FX4_TABLE_CB = 2, FX4_TABLE_RESULT = 4, FX4_TABLE_TEMP = 7, FX4_TABLES = 8 };
typedef struct Fx4CtabVar { const char *name; uint32_t reg_index, reg_count; } Fx4CtabVar;
typedef struct Fx4Preshader {
  const uint8_t *literals;             /* floats (unaligned bytes) */
  uint32_t literal_count;
  const uint8_t *code;                 /* the FXLC words */
  uint32_t code_words;
  uint32_t var_count;
  Fx4CtabVar *vars;
  uint32_t table_size[FX4_TABLES];     /* the floats each table needs (CB: four a register of the CTAB; constants: the literals) */
} Fx4Preshader;

/* The operations of a state assignment. */
enum { FX4_OP_CONST = 1, FX4_OP_VAR = 2, FX4_OP_CONST_INDEX = 3, FX4_OP_VAR_INDEX = 4, FX4_OP_INDEX_EXPRESSION = 5, FX4_OP_VALUE_EXPRESSION = 6, FX4_OP_ANONYMOUS_SHADER = 7 };
typedef struct Fx4Value { uint32_t type, bits; } Fx4Value;     /* (a constant: its base type FX4_BASE_*, and its 32 bits) */
typedef struct Fx4Shader { const uint8_t *code; uint32_t size; const char *so_decl; } Fx4Shader;      /* (code NULL: none) */
typedef struct Fx4Assignment {
  uint32_t id, index, op;
  uint32_t value_count; Fx4Value *values;          /* CONST */
  const char *name;                                /* VAR, CONST_INDEX, INDEX_EXPRESSION: the variable */
  uint32_t element;                                /* CONST_INDEX: the element (or the float a numeric variable is read from) */
  Fx4Preshader expression;                         /* INDEX_EXPRESSION, VALUE_EXPRESSION */
  Fx4Shader shader;                                /* ANONYMOUS_SHADER */
} Fx4Assignment;
typedef struct Fx4Assignments { uint32_t count; Fx4Assignment *items; } Fx4Assignments;

typedef struct Fx4Numeric { const char *name, *semantic; Fx4Type *type; uint32_t offset, flags; const uint8_t *defaults; Fx4Annotations annotations; } Fx4Numeric;
typedef struct Fx4Buffer { const char *name; uint32_t size, tbuffer, bind; uint32_t count; Fx4Numeric *vars; Fx4Annotations annotations; } Fx4Buffer;
/* An object variable and what each of its elements (max(1, elements)) is: a shader, a string, or the assignments of a state object (textures and views have nothing). */
typedef struct Fx4Element { Fx4Shader shader; const char *string; Fx4Assignments state; } Fx4Element;
typedef struct Fx4Object { const char *name, *semantic; Fx4Type *type; uint32_t bind; uint32_t element_count; Fx4Element *elements; Fx4Annotations annotations; } Fx4Object;
typedef struct Fx4Pass { const char *name; Fx4Assignments state; Fx4Annotations annotations; } Fx4Pass;
typedef struct Fx4Technique { const char *name; uint32_t pass_count; Fx4Pass *passes; Fx4Annotations annotations; } Fx4Technique;

typedef struct Fx4Effect {
  uint32_t version;
  uint32_t numeric_count, string_count, texture_count;
  uint32_t ds_count, blend_count, rs_count, sampler_count, rtv_count, dsv_count, shader_count, anonymous_count;
  uint32_t buffer_count; Fx4Buffer *buffers;
  uint32_t object_count; Fx4Object *objects;
  uint32_t shared_buffer_count; Fx4Buffer *shared_buffers;      /* (a child effect's: types and names only) */
  uint32_t shared_object_count; Fx4Object *shared_objects;
  uint32_t technique_count; Fx4Technique *techniques;
  uint32_t type_count; Fx4Type **types;
  /* (where everything was allocated: one list, freed by fx4_free) */
  void *blocks;
  Fx4Alloc alloc;
  const uint8_t *unstructured;
  uint32_t unstructured_size;
} Fx4Effect;

/* ---- the properties of state assignments ------------------------------------------------------------------------------------------------------------------------------ */

enum { FX4_IN_PASS = 1, FX4_IN_RASTERIZER = 2, FX4_IN_DEPTHSTENCIL = 3, FX4_IN_BLEND = 4, FX4_IN_SAMPLER = 5 };
/* What a property holds: an object of a kind (FX4_PT_* above 16), or `size` numbers of a type. */
enum { FX4_PT_FLOAT = 1, FX4_PT_INT = 2, FX4_PT_UINT = 3, FX4_PT_BOOL = 4, FX4_PT_UINT8 = 5,
       FX4_PT_RASTERIZER = 17, FX4_PT_DEPTHSTENCIL, FX4_PT_BLEND, FX4_PT_RTV, FX4_PT_DSV, FX4_PT_VS, FX4_PT_PS, FX4_PT_GS, FX4_PT_TEXTURE, FX4_PT_NONE };
typedef struct Fx4Property { const char *name; uint8_t container, type, size, count, count_4_1; } Fx4Property;
#define FX4_PROPERTY_COUNT 56u
static const Fx4Property fx4_properties[FX4_PROPERTY_COUNT] = {
  {"RasterizerState", FX4_IN_PASS, FX4_PT_RASTERIZER, 1, 1, 1}, {"DepthStencilState", FX4_IN_PASS, FX4_PT_DEPTHSTENCIL, 1, 1, 1}, {"BlendState", FX4_IN_PASS, FX4_PT_BLEND, 1, 1, 1},
  {"RenderTargetView", FX4_IN_PASS, FX4_PT_RTV, 1, 8, 8}, {"DepthStencilView", FX4_IN_PASS, FX4_PT_DSV, 1, 1, 1}, {"GenerateMips", FX4_IN_PASS, FX4_PT_NONE, 0, 0, 0},
  {"VertexShader", FX4_IN_PASS, FX4_PT_VS, 1, 1, 1}, {"PixelShader", FX4_IN_PASS, FX4_PT_PS, 1, 1, 1}, {"GeometryShader", FX4_IN_PASS, FX4_PT_GS, 1, 1, 1},
  {"StencilRef", FX4_IN_PASS, FX4_PT_UINT, 1, 1, 1}, {"BlendFactor", FX4_IN_PASS, FX4_PT_FLOAT, 4, 1, 1}, {"SampleMask", FX4_IN_PASS, FX4_PT_UINT, 1, 1, 1},
  {"FillMode", FX4_IN_RASTERIZER, FX4_PT_INT, 1, 1, 1}, {"CullMode", FX4_IN_RASTERIZER, FX4_PT_INT, 1, 1, 1},
  {"FrontCounterClockwise", FX4_IN_RASTERIZER, FX4_PT_BOOL, 1, 1, 1}, {"DepthBias", FX4_IN_RASTERIZER, FX4_PT_INT, 1, 1, 1},
  {"DepthBiasClamp", FX4_IN_RASTERIZER, FX4_PT_FLOAT, 1, 1, 1}, {"SlopeScaledDepthBias", FX4_IN_RASTERIZER, FX4_PT_FLOAT, 1, 1, 1},
  {"DepthClipEnable", FX4_IN_RASTERIZER, FX4_PT_BOOL, 1, 1, 1}, {"ScissorEnable", FX4_IN_RASTERIZER, FX4_PT_BOOL, 1, 1, 1},
  {"MultisampleEnable", FX4_IN_RASTERIZER, FX4_PT_BOOL, 1, 1, 1}, {"AntialiasedLineEnable", FX4_IN_RASTERIZER, FX4_PT_BOOL, 1, 1, 1},
  {"DepthEnable", FX4_IN_DEPTHSTENCIL, FX4_PT_BOOL, 1, 1, 1}, {"DepthWriteMask", FX4_IN_DEPTHSTENCIL, FX4_PT_INT, 1, 1, 1},
  {"DepthFunc", FX4_IN_DEPTHSTENCIL, FX4_PT_INT, 1, 1, 1}, {"StencilEnable", FX4_IN_DEPTHSTENCIL, FX4_PT_BOOL, 1, 1, 1},
  {"StencilReadMask", FX4_IN_DEPTHSTENCIL, FX4_PT_UINT8, 1, 1, 1}, {"StencilWriteMask", FX4_IN_DEPTHSTENCIL, FX4_PT_UINT8, 1, 1, 1},
  {"FrontFaceStencilFail", FX4_IN_DEPTHSTENCIL, FX4_PT_INT, 1, 1, 1}, {"FrontFaceStencilDepthFail", FX4_IN_DEPTHSTENCIL, FX4_PT_INT, 1, 1, 1},
  {"FrontFaceStencilPass", FX4_IN_DEPTHSTENCIL, FX4_PT_INT, 1, 1, 1}, {"FrontFaceStencilFunc", FX4_IN_DEPTHSTENCIL, FX4_PT_INT, 1, 1, 1},
  {"BackFaceStencilFail", FX4_IN_DEPTHSTENCIL, FX4_PT_INT, 1, 1, 1}, {"BackFaceStencilDepthFail", FX4_IN_DEPTHSTENCIL, FX4_PT_INT, 1, 1, 1},
  {"BackFaceStencilPass", FX4_IN_DEPTHSTENCIL, FX4_PT_INT, 1, 1, 1}, {"BackFaceStencilFunc", FX4_IN_DEPTHSTENCIL, FX4_PT_INT, 1, 1, 1},
  /* (Direct3D 10.1's blend - fx_4_1 - has a blend of each target: count_4_1) */
  {"AlphaToCoverageEnable", FX4_IN_BLEND, FX4_PT_BOOL, 1, 1, 1}, {"BlendEnable", FX4_IN_BLEND, FX4_PT_BOOL, 1, 8, 8},
  {"SrcBlend", FX4_IN_BLEND, FX4_PT_INT, 1, 1, 8}, {"DestBlend", FX4_IN_BLEND, FX4_PT_INT, 1, 1, 8}, {"BlendOp", FX4_IN_BLEND, FX4_PT_INT, 1, 1, 8},
  {"SrcBlendAlpha", FX4_IN_BLEND, FX4_PT_INT, 1, 1, 8}, {"DestBlendAlpha", FX4_IN_BLEND, FX4_PT_INT, 1, 1, 8}, {"BlendOpAlpha", FX4_IN_BLEND, FX4_PT_INT, 1, 1, 8},
  {"RenderTargetWriteMask", FX4_IN_BLEND, FX4_PT_UINT8, 1, 8, 8},
  {"Filter", FX4_IN_SAMPLER, FX4_PT_INT, 1, 1, 1}, {"AddressU", FX4_IN_SAMPLER, FX4_PT_INT, 1, 1, 1}, {"AddressV", FX4_IN_SAMPLER, FX4_PT_INT, 1, 1, 1},
  {"AddressW", FX4_IN_SAMPLER, FX4_PT_INT, 1, 1, 1}, {"MipLODBias", FX4_IN_SAMPLER, FX4_PT_FLOAT, 1, 1, 1}, {"MaxAnisotropy", FX4_IN_SAMPLER, FX4_PT_UINT, 1, 1, 1},
  {"ComparisonFunc", FX4_IN_SAMPLER, FX4_PT_INT, 1, 1, 1}, {"BorderColor", FX4_IN_SAMPLER, FX4_PT_FLOAT, 4, 1, 1}, {"MinLOD", FX4_IN_SAMPLER, FX4_PT_FLOAT, 1, 1, 1},
  {"MaxLOD", FX4_IN_SAMPLER, FX4_PT_FLOAT, 1, 1, 1}, {"Texture", FX4_IN_SAMPLER, FX4_PT_TEXTURE, 1, 1, 1},
};

/* ---- reading ------------------------------------------------------------------------------------------------------------------------------------------------------- */

static uint32_t fx4_load32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static float fx4_bits_float(uint32_t v) { union { uint32_t u; float f; } c; c.u = v; return c.f; }
static uint32_t fx4_float_bits(float f) { union { uint32_t u; float f; } c; c.f = f; return c.u; }

/* A word at `offset` of `size` bytes: 0 when it is not all there. */
static int fx4_word(const uint8_t *data, uint32_t size, uint32_t offset, uint32_t *out) {
  if (offset > size || size - offset < 4) return 0;
  *out = fx4_load32(data + offset);
  return 1;
}
/* `count` items of `item` bytes at `offset` are within `size` bytes. */
static int fx4_fits(uint32_t size, uint32_t offset, uint32_t count, uint32_t item) {
  if (offset > size) return 0;
  return !count || (size - offset) / count >= item;
}
/* A string at `offset` of the unstructured data: NUL-terminated within it (empty: NULL). */
static int fx4_string(const Fx4Effect *e, uint32_t offset, const char **out) {
  *out = NULL;
  if (offset >= e->unstructured_size) return 0;
  const uint8_t *p = e->unstructured + offset;
  uint32_t n = 0;
  while (offset + n < e->unstructured_size && p[n]) ++n;
  if (offset + n == e->unstructured_size) return 0;      /* (no NUL before the end) */
  if (n) *out = (const char *)p;
  return 1;
}

/* The structured data, read in order. */
typedef struct Fx4Cursor { const uint8_t *data; uint32_t size, at; int bad; } Fx4Cursor;
static uint32_t fx4_next(Fx4Cursor *c) {
  uint32_t v = 0;
  if (c->bad || !fx4_word(c->data, c->size, c->at, &v)) { c->bad = 1; return 0; }
  c->at += 4;
  return v;
}

static void *fx4_new(Fx4Effect *e, size_t bytes) {
  /* (every block has a header with the next one: fx4_free gives them all back) */
  if (bytes > (size_t)-1 - 16) return NULL;
  void **block = (void **)e->alloc.alloc(e->alloc.ctx, bytes + 16);
  if (!block) return NULL;
  block[0] = e->blocks;
  e->blocks = block;
  return (uint8_t *)block + 16;
}
/* `count` items of `item` bytes (zeroed), or NULL when that overflows or there is no memory; a count of 0 is a valid empty array (NULL too). */
static void *fx4_array(Fx4Effect *e, uint32_t count, size_t item, int *status) {
  if (!count) return NULL;
  if (item && count > ((size_t)-1 - 16) / item) { *status = FX4_NO_MEMORY; return NULL; }
  void *p = fx4_new(e, (size_t)count * item);
  if (!p) *status = FX4_NO_MEMORY;
  return p;
}

static void fx4_free(Fx4Effect *e) {
  void **block = (void **)e->blocks;
  while (block) {
    void **next = (void **)block[0];
    e->alloc.release(e->alloc.ctx, block);
    block = next;
  }
  e->blocks = NULL;
}

/* ---- types ------------------------------------------------------------------------------------------------------------------------------------------------------------- */

#define FX4_MAX_DEPTH 32u      /* (struct types inside struct types: a binary that nests them deeper, or in a circle, is refused) */
static Fx4Type *fx4_type(Fx4Effect *e, uint32_t offset, int *status, uint32_t depth);
static int fx4_type_parse(Fx4Effect *e, uint32_t offset, Fx4Type *t, int *status, uint32_t depth) {
  const uint8_t *u = e->unstructured;
  const uint32_t size = e->unstructured_size;
  uint32_t name, w;
  if (!fx4_fits(size, offset, 6, 4)) return 0;
  name = fx4_load32(u + offset);
  if (!fx4_string(e, name, &t->name)) return 0;
  t->kind = fx4_load32(u + offset + 4);
  t->elements = fx4_load32(u + offset + 8);
  t->unpacked = fx4_load32(u + offset + 12);
  t->stride = fx4_load32(u + offset + 16);
  t->packed = fx4_load32(u + offset + 20);
  const uint32_t at = offset + 24;
  switch (t->kind) {
    case FX4_KIND_NUMERIC:
      if (!fx4_word(u, size, at, &w)) return 0;
      t->numeric_class = w & 7;
      t->numeric_base = (w >> 3) & 31;
      t->rows = (w >> 8) & 7;
      t->columns = (w >> 11) & 7;
      t->column_major = (w >> 14) & 1;
      if (t->numeric_class < FX4_CLASS_SCALAR || t->numeric_class > FX4_CLASS_MATRIX || t->numeric_base < FX4_BASE_FLOAT || t->numeric_base > FX4_BASE_BOOL || !t->rows ||
          !t->columns || t->rows > 4 || t->columns > 4)
        return 0;
      if ((t->numeric_class == FX4_CLASS_SCALAR && (t->rows != 1 || t->columns != 1)) || (t->numeric_class == FX4_CLASS_VECTOR && t->rows != 1)) return 0;
      break;
    case FX4_KIND_OBJECT:
      if (!fx4_word(u, size, at, &t->object)) return 0;
      if (t->object < FX4_OBJ_STRING || t->object > FX4_OBJ_TEXTURECUBEARRAY || t->object == 18) return 0;
      break;
    case FX4_KIND_STRUCT: {
      if (!fx4_word(u, size, at, &t->member_count)) return 0;
      if (!fx4_fits(size, at + 4, t->member_count, 16)) return 0;
      t->members = (Fx4Member *)fx4_array(e, t->member_count, sizeof *t->members, status);
      if (t->member_count && !t->members) return 0;
      for (uint32_t i = 0; i < t->member_count; ++i) {
        const uint8_t *m = u + at + 4 + 16 * (size_t)i;
        Fx4Member *member = &t->members[i];
        if (!fx4_string(e, fx4_load32(m), &member->name) || !fx4_string(e, fx4_load32(m + 4), &member->semantic)) return 0;
        member->offset = fx4_load32(m + 8);
        if (!(member->type = fx4_type(e, fx4_load32(m + 12), status, depth + 1))) return 0;
        if (member->offset > t->unpacked || member->type->unpacked > t->unpacked - member->offset) return 0;      /* (a member past its structure) */
      }
      break;
    }
    default: return 0;
  }
  /* (an element of an array: the stride a step; the last fits in the unpacked size) */
  if (t->elements) {
    if (t->elements > 0xFFFFu || t->packed % t->elements) return 0;
    if (t->kind != FX4_KIND_OBJECT && t->stride && (t->elements - 1) > (t->unpacked) / t->stride) return 0;
  }
  return 1;
}
static Fx4Type *fx4_type(Fx4Effect *e, uint32_t offset, int *status, uint32_t depth) {
  for (uint32_t i = 0; i < e->type_count; ++i)
    if (e->types[i]->id == offset) return e->types[i];
  if (depth > FX4_MAX_DEPTH) return NULL;
  /* (the list of types grows by doubling) */
  if (!(e->type_count & (e->type_count - 1))) {
    const uint32_t capacity = e->type_count ? e->type_count * 2 : 8;
    if (capacity > 0x10000u) return NULL;
    Fx4Type **list = (Fx4Type **)fx4_array(e, capacity, sizeof *list, status);
    if (!list) return NULL;
    for (uint32_t i = 0; i < e->type_count; ++i) list[i] = e->types[i];
    e->types = list;
  }
  Fx4Type *t = (Fx4Type *)fx4_new(e, sizeof *t);
  if (!t) { *status = FX4_NO_MEMORY; return NULL; }
  t->id = offset;
  if (!fx4_type_parse(e, offset, t, status, depth)) return NULL;
  e->types[e->type_count++] = t;
  return t;
}
static int fx4_is_shader(const Fx4Type *t) {
  return t->kind == FX4_KIND_OBJECT && (t->object == FX4_OBJ_VERTEXSHADER || t->object == FX4_OBJ_PIXELSHADER || t->object == FX4_OBJ_GEOMETRYSHADER ||
                                         t->object == FX4_OBJ_GEOMETRYSHADER_SO);
}
static int fx4_is_state(const Fx4Type *t) {
  return t->kind == FX4_KIND_OBJECT && (t->object == FX4_OBJ_BLEND || t->object == FX4_OBJ_DEPTHSTENCIL || t->object == FX4_OBJ_RASTERIZER || t->object == FX4_OBJ_SAMPLER);
}

/* ---- annotations ---------------------------------------------------------------------------------------------------------------------------------------------------- */

static int fx4_annotations(Fx4Effect *e, Fx4Cursor *c, Fx4Annotations *a, int *status) {
  a->count = fx4_next(c);
  if (c->bad || a->count > c->size / 12) return 0;      /* (each takes three words at least) */
  a->items = (Fx4Annotation *)fx4_array(e, a->count, sizeof *a->items, status);
  if (a->count && !a->items) return 0;
  for (uint32_t i = 0; i < a->count; ++i) {
    Fx4Annotation *item = &a->items[i];
    if (!fx4_string(e, fx4_next(c), &item->name)) return 0;
    const uint32_t type = fx4_next(c);
    if (c->bad || !(item->type = fx4_type(e, type, status, 0))) return 0;
    if (item->type->kind == FX4_KIND_NUMERIC) {
      const uint32_t at = fx4_next(c);
      if (c->bad || !fx4_fits(e->unstructured_size, at, 1, item->type->packed)) return 0;
      item->values = e->unstructured + at;
    } else if (item->type->kind == FX4_KIND_OBJECT && item->type->object == FX4_OBJ_STRING) {
      const uint32_t n = item->type->elements ? item->type->elements : 1;
      item->strings = (const char **)fx4_array(e, n, sizeof *item->strings, status);
      if (!item->strings) return 0;
      for (uint32_t k = 0; k < n; ++k)
        if (!fx4_string(e, fx4_next(c), &item->strings[k]) && !c->bad) return 0;
      if (c->bad) return 0;
    } else {
      return 0;      /* (annotations are numbers and strings) */
    }
  }
  return 1;
}

/* ---- expressions ----------------------------------------------------------------------------------------------------------------------------------------------------- */

/* The operations an expression may have: the opcode, and how many inputs it takes (0: any up to 8). */
typedef struct Fx4Op { uint16_t opcode; uint8_t inputs; } Fx4Op;
static const Fx4Op fx4_ops[] = {
  {0x100, 1}, {0x101, 1}, {0x103, 1}, {0x104, 1}, {0x105, 1}, {0x106, 1}, {0x107, 1}, {0x108, 1}, {0x109, 1}, {0x10a, 1}, {0x10b, 1}, {0x10c, 1}, {0x112, 1},
  {0x120, 1}, {0x121, 1}, {0x130, 1}, {0x131, 1}, {0x133, 1}, {0x137, 1}, {0x139, 1}, {0x13a, 1},
  {0x200, 2}, {0x201, 2}, {0x204, 2}, {0x205, 2}, {0x206, 2}, {0x208, 2}, {0x210, 2}, {0x211, 2}, {0x212, 2}, {0x213, 2}, {0x214, 2}, {0x215, 2}, {0x216, 2}, {0x219, 2},
  {0x21a, 2}, {0x21d, 2}, {0x21e, 2}, {0x21f, 2}, {0x220, 2}, {0x230, 2}, {0x231, 2}, {0x233, 2}, {0x234, 2}, {0x235, 2}, {0x236, 2},
  {0x301, 3}, {0x500, 2}, {0x70e, 0},
};
static const Fx4Op *fx4_op(uint32_t opcode) {
  for (uint32_t i = 0; i < sizeof fx4_ops / sizeof fx4_ops[0]; ++i)
    if (fx4_ops[i].opcode == opcode) return &fx4_ops[i];
  return NULL;
}
#define FX4_COMPONENTS(token) ((token) & 0xFFFFu)
#define FX4_OPCODE(token) (((token) >> 20) & 0x7FFu)
#define FX4_SCALAR(token) ((token) >> 31)

/* A DXBC container's parts, one by one: part `index`, its tag and its bytes. 0 past the last or for a container that is not one. */
static int fx4_dxbc_part(const uint8_t *data, uint32_t size, uint32_t index, uint32_t *tag, const uint8_t **part, uint32_t *part_size) {
  uint32_t count, at, length;
  if (size < 32 || data[0] != 'D' || data[1] != 'X' || data[2] != 'B' || data[3] != 'C') return 0;
  count = fx4_load32(data + 28);
  if (index >= count || !fx4_fits(size, 32, count, 4)) return 0;
  at = fx4_load32(data + 32 + 4 * (size_t)index);
  if (!fx4_fits(size, at, 1, 8)) return 0;
  length = fx4_load32(data + at + 4);
  if (!fx4_fits(size, at + 8, 1, length)) return 0;
  *tag = fx4_load32(data + at);
  *part = data + at + 8;
  *part_size = length;
  return 1;
}
#define FX4_TAG(a, b, c, d) ((uint32_t)(a) | ((uint32_t)(b) << 8) | ((uint32_t)(c) << 16) | ((uint32_t)(d) << 24))

/* An expression's container: its parts checked, every instruction known and every parameter within the table it names. */
static int fx4_preshader(Fx4Effect *e, const uint8_t *data, uint32_t size, Fx4Preshader *p, int *status) {
  uint32_t tag, part_size;
  const uint8_t *part;
  int have_code = 0;
  uint32_t parts = 0;
  if (size >= 32 && fx4_load32(data + 28) <= size / 4) parts = fx4_load32(data + 28);
  else return 0;
  for (uint32_t i = 0; i < parts; ++i) {
    if (!fx4_dxbc_part(data, size, i, &tag, &part, &part_size)) return 0;
    if (tag == FX4_TAG('C', 'L', 'I', '4')) {
      uint32_t count;
      if (!fx4_word(part, part_size, 0, &count) || !fx4_fits(part_size, 4, count, 4)) return 0;
      p->literals = part + 4;
      p->literal_count = count;
    } else if (tag == FX4_TAG('F', 'X', 'L', 'C')) {
      if (part_size % 4) return 0;
      p->code = part;
      p->code_words = part_size / 4;
      have_code = 1;
    } else if (tag == FX4_TAG('C', 'T', 'A', 'B')) {
      /* (Direct3D 9's constant table: a header of seven words, then the constants - a name, the register set, index and count, the type and the default) */
      uint32_t constants, info;
      if (part_size < 28) return 0;
      constants = fx4_load32(part + 12);
      info = fx4_load32(part + 16);
      if (!fx4_fits(part_size, info, constants, 20)) return 0;
      p->var_count = constants;
      p->vars = (Fx4CtabVar *)fx4_array(e, constants, sizeof *p->vars, status);
      if (constants && !p->vars) return 0;
      for (uint32_t k = 0; k < constants; ++k) {
        const uint8_t *c = part + info + 20 * (size_t)k;
        const uint32_t name = fx4_load32(c);
        uint32_t n = 0;
        if (name >= part_size) return 0;
        while (name + n < part_size && part[name + n]) ++n;
        if (name + n == part_size || !n) return 0;
        p->vars[k].name = (const char *)part + name;
        p->vars[k].reg_index = (uint32_t)c[6] | ((uint32_t)c[7] << 8);
        p->vars[k].reg_count = (uint32_t)c[8] | ((uint32_t)c[9] << 8);
        const uint32_t end = (p->vars[k].reg_index + p->vars[k].reg_count) * 4;      /* (16-bit numbers: no overflow) */
        if (end > p->table_size[FX4_TABLE_CB]) p->table_size[FX4_TABLE_CB] = end;
      }
    }
  }
  if (!have_code) return 0;
  /* (the instructions: the count, then each its token, its input count and (flags, table, offset) for each input and the output) */
  uint32_t at = 0, count, need[FX4_TABLES] = {0};
  if (!fx4_word(p->code, p->code_words * 4, 0, &count)) return 0;
  at = 4;
  for (uint32_t k = 0; k < count; ++k) {
    uint32_t token, inputs;
    if (!fx4_word(p->code, p->code_words * 4, at, &token) || !fx4_word(p->code, p->code_words * 4, at + 4, &inputs)) return 0;
    at += 8;
    const Fx4Op *op = fx4_op(FX4_OPCODE(token));
    const uint32_t comps = FX4_COMPONENTS(token);
    if (!op || !comps || comps > 4 || inputs > 8 || (op->inputs && inputs != op->inputs) || (!op->inputs && inputs != 6 && inputs != 8)) return 0;
    if (!fx4_fits(p->code_words * 4, at, inputs + 1, 12)) return 0;
    for (uint32_t a = 0; a <= inputs; ++a) {
      const uint32_t flags = fx4_load32(p->code + at), table = fx4_load32(p->code + at + 4), offset = fx4_load32(p->code + at + 8);
      at += 12;
      if (flags || (table != FX4_TABLE_CONSTANTS && table != FX4_TABLE_CB && table != FX4_TABLE_RESULT && table != FX4_TABLE_TEMP)) return 0;
      if (a == inputs && table != FX4_TABLE_RESULT && table != FX4_TABLE_TEMP) return 0;      /* (the output is written) */
      /* (what the parameter reaches: the first input of a scalar instruction and every input of the swizzled dot is one component) */
      const uint32_t used = ((FX4_SCALAR(token) && a == 0) || !op->inputs || (op->opcode == 0x121 && a == 0)) && a != inputs ? 1 : comps;
      if (offset > 0x10000u) return 0;
      if (offset + used > need[table]) need[table] = offset + used;
    }
  }
  if (need[FX4_TABLE_CONSTANTS] > p->literal_count || need[FX4_TABLE_CB] > p->table_size[FX4_TABLE_CB]) return 0;
  p->table_size[FX4_TABLE_CONSTANTS] = p->literal_count;
  p->table_size[FX4_TABLE_RESULT] = need[FX4_TABLE_RESULT];
  p->table_size[FX4_TABLE_TEMP] = need[FX4_TABLE_TEMP];
  if (!p->table_size[FX4_TABLE_RESULT]) return 0;      /* (an expression gives a value) */
  return 1;
}


/* ---- the math of expressions (no C library: computed in double, rounded once) -------------------------------------------------------------------------------------- */

static float fx4_fabs(float x) { return fx4_bits_float(fx4_float_bits(x) & 0x7FFFFFFFu); }
static int fx4_isnan(float x) { return (fx4_float_bits(x) & 0x7FFFFFFFu) > 0x7F800000u; }
static float fx4_nan(void) { return fx4_bits_float(0x7FC00000u); }
static float fx4_floor(float x) {
  if (fx4_isnan(x) || fx4_fabs(x) >= 8388608.0f) return x;      /* (no fraction past 2^23) */
  const float t = (float)(int32_t)x;
  return t > x ? t - 1.0f : t;
}
static float fx4_ceil(float x) {
  if (fx4_isnan(x) || fx4_fabs(x) >= 8388608.0f) return x;
  const float t = (float)(int32_t)x;
  return t < x ? t + 1.0f : t;
}
/* (float to integer as the hardware does it: NaN 0, out of range saturated - not C's undefined conversion) */
static int32_t fx4_f2i(float f) {
  if (fx4_isnan(f)) return 0;
  if (f >= 2147483648.0f) return 0x7FFFFFFF;
  if (f <= -2147483648.0f) return (int32_t)0x80000000u;
  return (int32_t)f;
}
static uint32_t fx4_f2u(float f) {
  if (fx4_isnan(f) || f <= 0.0f) return 0;
  if (f >= 4294967296.0f) return 0xFFFFFFFFu;
  return (uint32_t)f;
}
static double fx4_sqrtd(double x) {
  if (!(x > 0.0)) return x == 0.0 ? 0.0 : (double)fx4_nan();
  if (x > 1e300) return x;
  double y = x >= 1.0 ? x : 1.0;
  while (y * y > 4.0 * x) y *= 0.5;
  for (int i = 0; i < 200; ++i) {
    const double next = 0.5 * (y + x / y);
    if (next == y) break;
    y = next;
  }
  return y;
}
static float fx4_sqrt(float x) {
  if (fx4_isnan(x) || x < 0.0f) return fx4_nan();
  if (fx4_float_bits(x) == 0x7F800000u) return x;
  return (float)fx4_sqrtd(x);
}
static double fx4_exp2d(double x) {
  if (x >= 128.0) return 1e300 * 1e300;
  if (x < -160.0) return 0.0;
  double n = (double)(int32_t)x;
  if (n > x) n -= 1.0;
  const double f = x - n, ln2 = 0.69314718055994530942;      /* (0 <= f < 1) */
  double term = 1.0, sum = 1.0;
  for (int k = 1; k < 30; ++k) { term *= f * ln2 / k; sum += term; }
  for (int e = (int)n; e > 0; --e) sum *= 2.0;
  for (int e = (int)n; e < 0; ++e) sum *= 0.5;
  return sum;
}
static float fx4_exp2(float x) { return fx4_isnan(x) ? x : (float)fx4_exp2d(x); }
static float fx4_log2(float x) {
  if (fx4_isnan(x) || x < 0.0f) return fx4_nan();
  if (x == 0.0f) return fx4_bits_float(0xFF800000u);
  if (fx4_float_bits(x) == 0x7F800000u) return x;
  /* x = m 2^e, m in [1, 2): log2 x = e + 2 atanh((m - 1) / (m + 1)) / ln 2 */
  double m = x;
  int e = 0;
  while (m >= 2.0) { m *= 0.5; ++e; }
  while (m < 1.0) { m *= 2.0; --e; }
  const double s = (m - 1.0) / (m + 1.0), s2 = s * s;
  double term = s, sum = 0.0;
  for (int k = 1; k < 60; k += 2) { sum += term / k; term *= s2; }
  return (float)((double)e + 2.0 * sum / 0.69314718055994530942);
}
#define FX4_PI 3.14159265358979323846
static double fx4_sind(double x) {
  if (x > 1e9 || x < -1e9) return 0.0;
  const double two_pi = 2.0 * FX4_PI;
  x -= (double)(int64_t)(x / two_pi) * two_pi;      /* (now within +-2 pi, then +-pi) */
  if (x > FX4_PI) x -= two_pi;
  if (x < -FX4_PI) x += two_pi;
  double term = x, sum = x;
  for (int n = 1; n < 24; ++n) { term *= -x * x / ((2.0 * n) * (2.0 * n + 1.0)); sum += term; }
  return sum;
}
static float fx4_sin(float x) { return fx4_isnan(x) ? x : (float)fx4_sind(x); }
static float fx4_cos(float x) { return fx4_isnan(x) ? x : (float)fx4_sind((double)x + FX4_PI / 2.0); }
static double fx4_atand(double x) {
  int invert = 0, negative = 0;
  if (x < 0.0) { x = -x; negative = 1; }
  if (x > 1e300) { x = 0.0; invert = 1; }
  else if (x > 1.0) { x = 1.0 / x; invert = 1; }
  /* (the angle halved twice - atan x = 2 atan(x / (1 + sqrt(1 + x^2))) - then the series) */
  double y = x;
  for (int i = 0; i < 2; ++i) y = y / (1.0 + fx4_sqrtd(1.0 + y * y));
  const double y2 = y * y;
  double term = y, sum = 0.0;
  for (int k = 1; k < 80; k += 2) { sum += term / k; term *= -y2; }
  sum *= 4.0;
  if (invert) sum = FX4_PI / 2.0 - sum;
  return negative ? -sum : sum;
}
static float fx4_atan(float x) { return fx4_isnan(x) ? x : (float)fx4_atand(x); }
static float fx4_atan2(float y, float x) {
  if (fx4_isnan(x) || fx4_isnan(y)) return fx4_nan();
  if (x == 0.0f) return y == 0.0f ? 0.0f : (y > 0.0f ? (float)(FX4_PI / 2.0) : (float)(-FX4_PI / 2.0));
  double a = fx4_atand((double)y / (double)x);
  if (x < 0.0f) a += y < 0.0f ? -FX4_PI : FX4_PI;
  return (float)a;
}
static float fx4_asin(float x) {
  if (fx4_isnan(x) || x > 1.0f || x < -1.0f) return fx4_nan();
  const double c = fx4_sqrtd(1.0 - (double)x * x);
  if (c == 0.0) return x > 0.0f ? (float)(FX4_PI / 2.0) : (float)(-FX4_PI / 2.0);
  return (float)fx4_atand((double)x / c);
}
static float fx4_acos(float x) {
  if (fx4_isnan(x) || x > 1.0f || x < -1.0f) return fx4_nan();
  if (x == 0.0f) return (float)(FX4_PI / 2.0);
  double a = fx4_atand(fx4_sqrtd(1.0 - (double)x * x) / (double)x);
  if (x < 0.0f) a += FX4_PI;
  return (float)a;
}

/* ---- evaluating an expression ---------------------------------------------------------------------------------------------------------------------------------------- */

/* Runs a checked expression (fx4_preshader) over its tables: tables[FX4_TABLE_CONSTANTS] the literals, [CB] the variables it reads, [RESULT] and [TEMP] of the sizes the
 * expression has (table_size). Every parameter was checked against those sizes when it was read. */
static void fx4_preshader_eval(const Fx4Preshader *p, float *const tables[FX4_TABLES]) {
  const uint8_t *code = p->code;
  uint32_t at = 4;
  const uint32_t count = fx4_load32(code);
  for (uint32_t i = 0; i < p->table_size[FX4_TABLE_RESULT]; ++i) tables[FX4_TABLE_RESULT][i] = 0.0f;
  for (uint32_t k = 0; k < count; ++k) {
    const uint32_t token = fx4_load32(code + at), inputs = fx4_load32(code + at + 4);
    at += 8;
    float *args[9];
    for (uint32_t a = 0; a <= inputs; ++a) {
      args[a] = tables[fx4_load32(code + at + 4)] + fx4_load32(code + at + 8);
      at += 12;
    }
    const uint32_t n = FX4_COMPONENTS(token), op = FX4_OPCODE(token), s = FX4_SCALAR(token);
    float *out = args[inputs];
    if (op == 0x500 || op == 0x70e) {
      /* (dot products: one value) */
      double sum = 0.0;
      if (op == 0x500) for (uint32_t i = 0; i < n; ++i) sum += (double)args[0][s ? 0 : i] * args[1][i];
      else for (uint32_t i = 0; i < inputs / 2; ++i) sum += (double)args[i][0] * args[i + inputs / 2][0];
      out[0] = (float)sum;
      continue;
    }
    float r[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    uint32_t u[4] = {0, 0, 0, 0};
    int bits = 0;      /* (the results are integer bits, u[]) */
    for (uint32_t i = 0; i < n; ++i) {
      const float x = args[0][i];
      const float a0 = inputs >= 2 ? args[0][s ? 0 : i] : x, b = inputs >= 2 ? args[1][i] : 0.0f;
      const uint32_t ua = fx4_float_bits(a0), ub = fx4_float_bits(b);
      const int32_t ia = (int32_t)ua, ib = (int32_t)ub;
      switch (op) {
        case 0x100: r[i] = x; break;
        case 0x101: r[i] = -x; break;
        case 0x103: r[i] = 1.0f / x; break;
        case 0x104: r[i] = x - fx4_floor(x); break;
        case 0x105: r[i] = fx4_exp2(x); break;
        case 0x106: r[i] = x == 0.0f ? 0.0f : fx4_log2(fx4_fabs(x)); break;
        case 0x107: r[i] = 1.0f / fx4_sqrt(x); break;
        case 0x108: r[i] = fx4_sin(x); break;
        case 0x109: r[i] = fx4_cos(x); break;
        case 0x10a: r[i] = fx4_asin(x); break;
        case 0x10b: r[i] = fx4_acos(x); break;
        case 0x10c: r[i] = fx4_atan(x); break;
        case 0x112: r[i] = fx4_sqrt(x); break;
        case 0x120: bits = 1; u[i] = 0u - fx4_float_bits(x); break;
        case 0x121: bits = 1; u[i] = ~fx4_float_bits(args[0][0]); break;      /* (the first component, for every one: what Windows does) */
        case 0x130: r[i] = (float)(int32_t)fx4_float_bits(x); break;
        case 0x131: r[i] = (float)fx4_float_bits(x); break;
        case 0x133: bits = 1; u[i] = fx4_f2u(x); break;
        case 0x137: bits = 1; u[i] = x == 0.0f ? 0 : ~0u; break;
        case 0x139: r[i] = fx4_floor(args[0][0]); break;      /* (the first component, for every one) */
        case 0x13a: r[i] = fx4_ceil(args[0][0]); break;
        case 0x200: r[i] = a0 < b ? a0 : b; break;
        case 0x201: r[i] = a0 > b ? a0 : b; break;
        case 0x204: r[i] = a0 + b; break;
        case 0x205: r[i] = a0 * b; break;
        case 0x206: r[i] = fx4_atan2(a0, b); break;
        case 0x208: r[i] = a0 / b; break;
        case 0x210: bits = 1; u[i] = ia < ib ? ~0u : 0; break;
        case 0x211: bits = 1; u[i] = ia >= ib ? ~0u : 0; break;
        case 0x212: bits = 1; u[i] = ua == ub ? ~0u : 0; break;
        case 0x213: bits = 1; u[i] = ua != ub ? ~0u : 0; break;
        case 0x214: bits = 1; u[i] = ua >= ub ? ~0u : 0; break;
        case 0x215: bits = 1; u[i] = ua < ub ? ~0u : 0; break;
        case 0x216: bits = 1; u[i] = ua + ub; break;
        case 0x219: bits = 1; u[i] = ua * ub; break;
        case 0x21a: bits = 1; u[i] = ub ? ua / ub : 0xFFFFFFFFu; break;
        case 0x21d: bits = 1; u[i] = (uint32_t)(ia < ib ? ia : ib); break;
        case 0x21e: bits = 1; u[i] = (uint32_t)(ia > ib ? ia : ib); break;
        case 0x21f: bits = 1; u[i] = ua < ub ? ua : ub; break;
        case 0x220: bits = 1; u[i] = ua > ub ? ua : ub; break;
        case 0x230: bits = 1; u[i] = ua & ub; break;
        case 0x231: bits = 1; u[i] = fx4_float_bits(args[0][0]) | fx4_float_bits(args[1][0]); break;      /* (the first components, for every one: as Windows) */
        case 0x233: bits = 1; u[i] = ua ^ ub; break;
        case 0x234: bits = 1; u[i] = ua << (ub % 32); break;
        case 0x235: bits = 1; u[i] = (uint32_t)(ia >> (ub % 32)); break;
        case 0x236: bits = 1; u[i] = ua >> (ub % 32); break;
        case 0x301: r[i] = fx4_float_bits(args[0][i]) & 0x7FFFFFFFu ? args[1][i] : args[2][i]; break;
        default: break;
      }
    }
    for (uint32_t i = 0; i < n; ++i) out[i] = bits ? fx4_bits_float(u[i]) : r[i];
  }
}

/* ---- variables, objects, techniques -------------------------------------------------------------------------------------------------------------------------------- */

/* The bytes a numeric (or struct) type's values reach from where it starts - registers of 16 bytes: a row each, or a column of a column-major matrix -: what writes and
 * reads of it touch. 0xFFFFFFFF for a type that is not numeric or a struct of such (or is nested past FX4_MAX_DEPTH). */
static uint32_t fx4_footprint(const Fx4Type *t, uint32_t depth) {
  uint32_t one = 0;
  if (depth > FX4_MAX_DEPTH) return 0xFFFFFFFFu;
  if (t->kind == FX4_KIND_NUMERIC) {
    const int matrix = t->numeric_class == FX4_CLASS_MATRIX;
    const uint32_t lines = matrix ? (t->column_major ? t->columns : t->rows) : 1;
    const uint32_t width = matrix ? (t->column_major ? t->rows : t->columns) : t->columns;
    one = (lines - 1) * 16 + width * 4;
  } else if (t->kind == FX4_KIND_STRUCT) {
    for (uint32_t i = 0; i < t->member_count; ++i) {
      const uint32_t m = fx4_footprint(t->members[i].type, depth + 1);
      if (m == 0xFFFFFFFFu || t->members[i].offset > 0xFFFFFFFEu - m) return 0xFFFFFFFFu;
      if (t->members[i].offset + m > one) one = t->members[i].offset + m;
    }
  } else {
    return 0xFFFFFFFFu;
  }
  if (!t->elements) return one;
  const uint64_t all = (uint64_t)(t->elements - 1) * t->stride + one;
  return all >= 0xFFFFFFFFull ? 0xFFFFFFFFu : (uint32_t)all;
}
/* The packed bytes of a type's values (its default value's size). */
static uint64_t fx4_packed_bytes(const Fx4Type *t, uint32_t depth) {
  uint64_t one = 0;
  if (depth > FX4_MAX_DEPTH) return ~0ull;
  if (t->kind == FX4_KIND_NUMERIC) one = (uint64_t)t->rows * t->columns * 4;
  else if (t->kind == FX4_KIND_STRUCT)
    for (uint32_t i = 0; i < t->member_count; ++i) {
      const uint64_t m = fx4_packed_bytes(t->members[i].type, depth + 1);
      if (m == ~0ull) return m;
      one += m;
    }
  else return ~0ull;
  return one * (t->elements ? t->elements : 1);
}

static int fx4_numeric(Fx4Effect *e, Fx4Cursor *c, Fx4Numeric *v, uint32_t buffer_size, int local, int *status) {
  if (!fx4_string(e, fx4_next(c), &v->name)) return 0;
  const uint32_t type = fx4_next(c);
  if (c->bad || !(v->type = fx4_type(e, type, status, 0))) return 0;
  if (!fx4_string(e, fx4_next(c), &v->semantic) && !c->bad) return 0;
  v->offset = fx4_next(c);
  const uint32_t defaults = fx4_next(c);
  v->flags = fx4_next(c);
  if (c->bad) return 0;
  if (v->type->kind == FX4_KIND_OBJECT) return 0;      /* (objects are not in constant buffers) */
  const uint32_t reach = fx4_footprint(v->type, 0);
  if (reach == 0xFFFFFFFFu || reach > v->type->unpacked || v->offset > buffer_size || v->type->unpacked > buffer_size - v->offset) return 0;
  if (defaults) {
    const uint64_t packed = fx4_packed_bytes(v->type, 0);
    if (packed > e->unstructured_size || !fx4_fits(e->unstructured_size, defaults, 1, (uint32_t)packed)) return 0;
    v->defaults = e->unstructured + defaults;
  }
  return !local || fx4_annotations(e, c, &v->annotations, status);      /* (a shared buffer's variables have no annotations here: the pool's have them) */
}

static int fx4_buffer(Fx4Effect *e, Fx4Cursor *c, Fx4Buffer *b, int local, int *status) {
  if (!fx4_string(e, fx4_next(c), &b->name) && !c->bad) return 0;
  b->size = fx4_next(c);
  const uint32_t flags = fx4_next(c);
  b->count = fx4_next(c);
  b->bind = fx4_next(c);
  if (c->bad || (flags & ~1u) || b->count > c->size / 28) return 0;
  b->tbuffer = flags & 1;
  if (local && !fx4_annotations(e, c, &b->annotations, status)) return 0;
  b->vars = (Fx4Numeric *)fx4_array(e, b->count, sizeof *b->vars, status);
  if (b->count && !b->vars) return 0;
  for (uint32_t i = 0; i < b->count; ++i)
    if (!fx4_numeric(e, c, &b->vars[i], b->size, local, status)) return 0;
  return 1;
}

/* A shader at `offset`: its size and its code (size 0: none). */
static int fx4_shader_at(const Fx4Effect *e, uint32_t offset, Fx4Shader *s) {
  uint32_t size;
  if (!fx4_word(e->unstructured, e->unstructured_size, offset, &size) || !fx4_fits(e->unstructured_size, offset + 4, 1, size)) return 0;
  s->size = size;
  s->code = size ? e->unstructured + offset + 4 : NULL;
  return 1;
}
static int fx4_expression_at(Fx4Effect *e, uint32_t offset, Fx4Preshader *p, int *status) {
  uint32_t size;
  if (!fx4_word(e->unstructured, e->unstructured_size, offset, &size) || !fx4_fits(e->unstructured_size, offset + 4, 1, size)) return 0;
  return fx4_preshader(e, e->unstructured + offset + 4, size, p, status);
}

/* One state assignment, of a pass or a state object (`container`); `allow_10_1`: the per-target blend of Direct3D 10.1 (an fx_4_1 effect in d3d10_1.dll). */
static int fx4_assignment(Fx4Effect *e, Fx4Cursor *c, Fx4Assignment *a, uint32_t container, int allow_10_1, uint32_t *shaders, uint32_t *anonymous, int *status) {
  a->id = fx4_next(c);
  a->index = fx4_next(c);
  a->op = fx4_next(c);
  const uint32_t value = fx4_next(c);
  if (c->bad || a->id >= FX4_PROPERTY_COUNT) return 0;
  const Fx4Property *p = &fx4_properties[a->id];
  const uint32_t count = allow_10_1 && e->version == FX4_VERSION_4_1 ? p->count_4_1 : p->count;
  if (p->container != container || a->index >= count || p->type == FX4_PT_NONE) return 0;
  const uint8_t *u = e->unstructured;
  const uint32_t size = e->unstructured_size;
  uint32_t w0, w1;
  switch (a->op) {
    case FX4_OP_CONST:
      if (!fx4_word(u, size, value, &a->value_count) || a->value_count != p->size || !fx4_fits(size, value + 4, a->value_count, 8)) return 0;
      a->values = (Fx4Value *)fx4_array(e, a->value_count, sizeof *a->values, status);
      if (a->value_count && !a->values) return 0;
      for (uint32_t i = 0; i < a->value_count; ++i) {
        a->values[i].type = fx4_load32(u + value + 4 + 8 * (size_t)i);
        a->values[i].bits = fx4_load32(u + value + 8 + 8 * (size_t)i);
        if (a->values[i].type < FX4_BASE_FLOAT || a->values[i].type > FX4_BASE_BOOL) return 0;
      }
      return 1;
    case FX4_OP_VAR:
      return fx4_string(e, value, &a->name) && a->name;
    case FX4_OP_CONST_INDEX:
      if (!fx4_word(u, size, value, &w0) || !fx4_word(u, size, value + 4, &a->element)) return 0;
      return fx4_string(e, w0, &a->name) && a->name;
    case FX4_OP_INDEX_EXPRESSION:
      if (!fx4_word(u, size, value, &w0) || !fx4_word(u, size, value + 4, &w1)) return 0;
      if (!fx4_string(e, w0, &a->name) || !a->name) return 0;
      if (p->type < FX4_PT_RASTERIZER) return 0;      /* (an index picks an object of an array) */
      return fx4_expression_at(e, w1, &a->expression, status);
    case FX4_OP_VALUE_EXPRESSION:
      if (p->type >= FX4_PT_RASTERIZER) return 0;
      return fx4_expression_at(e, value, &a->expression, status);
    case FX4_OP_ANONYMOUS_SHADER:
      if (p->type != FX4_PT_VS && p->type != FX4_PT_PS && p->type != FX4_PT_GS) return 0;
      if (!fx4_word(u, size, value, &w0) || !fx4_word(u, size, value + 4, &w1)) return 0;
      if (!fx4_shader_at(e, w0, &a->shader)) return 0;
      if (w1 && !fx4_string(e, w1, &a->shader.so_decl)) return 0;
      ++*shaders;
      ++*anonymous;
      return 1;
    default:
      return 0;      /* (FX4_OP_VAR_INDEX: no fx_4 compiler emits it and its layout is not known; refused, as unknown operations are) */
  }
}
static int fx4_assignments(Fx4Effect *e, Fx4Cursor *c, Fx4Assignments *list, uint32_t count, uint32_t container, int allow_10_1, uint32_t *shaders, uint32_t *anonymous,
                           int *status) {
  if (count > c->size / 16) return 0;
  list->count = count;
  list->items = (Fx4Assignment *)fx4_array(e, count, sizeof *list->items, status);
  if (count && !list->items) return 0;
  for (uint32_t i = 0; i < count; ++i)
    if (!fx4_assignment(e, c, &list->items[i], container, allow_10_1, shaders, anonymous, status)) return 0;
  return 1;
}

typedef struct Fx4Counts { uint32_t shaders, anonymous, blend, ds, rs, sampler; } Fx4Counts;
static int fx4_object(Fx4Effect *e, Fx4Cursor *c, Fx4Object *o, int shared, int allow_10_1, Fx4Counts *n, int *status) {
  if (!fx4_string(e, fx4_next(c), &o->name) && !c->bad) return 0;
  const uint32_t type = fx4_next(c);
  if (c->bad || !(o->type = fx4_type(e, type, status, 0))) return 0;
  if (!fx4_string(e, fx4_next(c), &o->semantic) && !c->bad) return 0;
  o->bind = fx4_next(c);
  if (c->bad || o->type->kind != FX4_KIND_OBJECT) return 0;
  if (shared) return 1;      /* (a shared object's description is its type: the pool has the rest) */
  const Fx4Type *t = o->type;
  o->element_count = t->elements ? t->elements : 1;
  if (o->element_count > c->size / 4 + 1) return 0;
  o->elements = (Fx4Element *)fx4_array(e, o->element_count, sizeof *o->elements, status);
  if (!o->elements) return 0;
  for (uint32_t i = 0; i < o->element_count; ++i) {
    Fx4Element *el = &o->elements[i];
    if (t->object == FX4_OBJ_STRING) {
      if (!fx4_string(e, fx4_next(c), &el->string) && !c->bad) return 0;
    } else if (fx4_is_shader(t)) {
      const uint32_t at = fx4_next(c);
      if (c->bad || !fx4_shader_at(e, at, &el->shader)) return 0;
      if (t->object == FX4_OBJ_GEOMETRYSHADER_SO && !fx4_string(e, fx4_next(c), &el->shader.so_decl) && !c->bad) return 0;
      ++n->shaders;
    } else if (fx4_is_state(t)) {
      const uint32_t container = t->object == FX4_OBJ_BLEND ? FX4_IN_BLEND : t->object == FX4_OBJ_DEPTHSTENCIL ? FX4_IN_DEPTHSTENCIL :
                                 t->object == FX4_OBJ_RASTERIZER ? FX4_IN_RASTERIZER : FX4_IN_SAMPLER;
      const uint32_t count = fx4_next(c);
      if (c->bad || !fx4_assignments(e, c, &el->state, count, container, allow_10_1, &n->shaders, &n->anonymous, status)) return 0;
      if (t->object == FX4_OBJ_BLEND) ++n->blend;
      else if (t->object == FX4_OBJ_DEPTHSTENCIL) ++n->ds;
      else if (t->object == FX4_OBJ_RASTERIZER) ++n->rs;
      else ++n->sampler;
    }
    if (c->bad) return 0;
  }
  return fx4_annotations(e, c, &o->annotations, status);
}

static int fx4_technique(Fx4Effect *e, Fx4Cursor *c, Fx4Technique *t, int allow_10_1, Fx4Counts *n, int *status) {
  if (!fx4_string(e, fx4_next(c), &t->name) && !c->bad) return 0;
  t->pass_count = fx4_next(c);
  if (c->bad || t->pass_count > c->size / 12 || !fx4_annotations(e, c, &t->annotations, status)) return 0;
  t->passes = (Fx4Pass *)fx4_array(e, t->pass_count, sizeof *t->passes, status);
  if (t->pass_count && !t->passes) return 0;
  for (uint32_t i = 0; i < t->pass_count; ++i) {
    Fx4Pass *p = &t->passes[i];
    if (!fx4_string(e, fx4_next(c), &p->name) && !c->bad) return 0;
    const uint32_t count = fx4_next(c);
    if (c->bad || !fx4_annotations(e, c, &p->annotations, status)) return 0;
    if (!fx4_assignments(e, c, &p->state, count, FX4_IN_PASS, allow_10_1, &n->shaders, &n->anonymous, status)) return 0;
  }
  return 1;
}

/* Reads an effect (the DXBC container with an FX10 part): FX4_OK, FX4_BAD for input that is not an effect this reads (or is not whole), FX4_NO_MEMORY. On anything but
 * FX4_OK what was made is freed. */
static int fx4_parse(const uint8_t *data, size_t size, int allow_10_1, const Fx4Alloc *alloc, Fx4Effect *e) {
  int status = FX4_OK;
  uint32_t tag = 0, part_size = 0;
  const uint8_t *part = NULL;
  for (size_t k = 0; k < sizeof *e; ++k) ((uint8_t *)e)[k] = 0;
  e->alloc = *alloc;
  if (!data || size > 0xFFFFFFFFu) return FX4_BAD;
  uint32_t i = 0, found = 0;
  while (fx4_dxbc_part(data, (uint32_t)size, i++, &tag, &part, &part_size))
    if (tag == FX4_TAG('F', 'X', '1', '0')) { found = 1; break; }
  if (!found || part_size < 76) return FX4_BAD;
  uint32_t h[19];
  for (uint32_t k = 0; k < 19; ++k) h[k] = fx4_load32(part + 4 * k);
  e->version = h[0];
  if (e->version != FX4_VERSION_4_0 && e->version != FX4_VERSION_4_1) return FX4_BAD;
  e->buffer_count = h[1];
  e->numeric_count = h[2];
  e->object_count = h[3];
  e->shared_buffer_count = h[4];
  e->shared_object_count = h[6];
  e->technique_count = h[7];
  e->unstructured_size = h[8];
  e->string_count = h[9];
  e->texture_count = h[10];
  e->ds_count = h[11];
  e->blend_count = h[12];
  e->rs_count = h[13];
  e->sampler_count = h[14];
  e->rtv_count = h[15];
  e->dsv_count = h[16];
  e->shader_count = h[17];
  e->anonymous_count = h[18];
  if (!fx4_fits(part_size, 76, 1, e->unstructured_size)) return FX4_BAD;
  e->unstructured = part + 76;
  Fx4Cursor c = {part + 76 + e->unstructured_size, part_size - 76 - e->unstructured_size, 0, 0};
  Fx4Counts n = {0, 0, 0, 0, 0, 0};
  /* (every count is bounded by what the rest could hold: a word each at least) */
  if (e->buffer_count > c.size / 4 || e->object_count > c.size / 4 || e->shared_buffer_count > c.size / 4 || e->shared_object_count > c.size / 4 ||
      e->technique_count > c.size / 4)
    return FX4_BAD;
  int ok = 1;
  e->buffers = (Fx4Buffer *)fx4_array(e, e->buffer_count, sizeof *e->buffers, &status);
  e->objects = (Fx4Object *)fx4_array(e, e->object_count, sizeof *e->objects, &status);
  e->shared_buffers = (Fx4Buffer *)fx4_array(e, e->shared_buffer_count, sizeof *e->shared_buffers, &status);
  e->shared_objects = (Fx4Object *)fx4_array(e, e->shared_object_count, sizeof *e->shared_objects, &status);
  e->techniques = (Fx4Technique *)fx4_array(e, e->technique_count, sizeof *e->techniques, &status);
  if (status != FX4_OK) ok = 0;
  for (uint32_t k = 0; ok && k < e->buffer_count; ++k) ok = fx4_buffer(e, &c, &e->buffers[k], 1, &status);
  for (uint32_t k = 0; ok && k < e->object_count; ++k) ok = fx4_object(e, &c, &e->objects[k], 0, allow_10_1, &n, &status);
  for (uint32_t k = 0; ok && k < e->shared_buffer_count; ++k) ok = fx4_buffer(e, &c, &e->shared_buffers[k], 0, &status);
  for (uint32_t k = 0; ok && k < e->shared_object_count; ++k) ok = fx4_object(e, &c, &e->shared_objects[k], 1, allow_10_1, &n, &status);
  for (uint32_t k = 0; ok && k < e->technique_count; ++k) ok = fx4_technique(e, &c, &e->techniques[k], allow_10_1, &n, &status);
  /* (the header's counts of shaders and states: the rest has not more of them) */
  if (ok && (n.shaders > e->shader_count || n.anonymous > e->anonymous_count || n.blend > e->blend_count || n.ds > e->ds_count || n.rs > e->rs_count ||
             n.sampler > e->sampler_count))
    ok = 0;
  if (!ok) {
    fx4_free(e);
    return status == FX4_NO_MEMORY ? FX4_NO_MEMORY : FX4_BAD;
  }
  return FX4_OK;
}

/* ---- default values ------------------------------------------------------------------------------------------------------------------------------------------------- */

/* Writes a numeric (or struct) variable's packed values (`src`, `available` bytes) into its unpacked place `dst` (fx4_footprint bytes from it): row by row, a row (or a
 * column of a column-major matrix) a 16-byte register. Returns the packed bytes taken. */
static uint32_t fx4_unpack(const Fx4Type *t, const uint8_t *src, uint32_t available, uint8_t *dst, uint32_t depth) {
  const uint32_t elements = t->elements ? t->elements : 1;
  uint32_t taken = 0;
  if (depth > FX4_MAX_DEPTH) return 0;
  for (uint32_t el = 0; el < elements; ++el) {
    uint8_t *base = dst + (size_t)el * t->stride;
    if (t->kind == FX4_KIND_STRUCT) {
      for (uint32_t m = 0; m < t->member_count; ++m) taken += fx4_unpack(t->members[m].type, src + taken, available - taken, base + t->members[m].offset, depth + 1);
      continue;
    }
    const int column_major = t->numeric_class == FX4_CLASS_MATRIX && t->column_major;
    for (uint32_t r = 0; r < t->rows; ++r)
      for (uint32_t col = 0; col < t->columns; ++col) {
        if (available - taken < 4) return taken;
        const size_t at = column_major ? (size_t)col * 16 + r * 4 : (size_t)r * 16 + col * 4;
        for (uint32_t b = 0; b < 4; ++b) base[at + b] = src[taken + b];
        taken += 4;
      }
  }
  return taken;
}

#endif
