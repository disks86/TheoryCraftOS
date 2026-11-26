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

/* What d3d8.dll makes of a Direct3D 8 program's shaders for Direct3D 9 (docs/Direct3D8.md). It needs no library - the DLL has no C runtime - and the host
 * test (tests/D3d8ShaderTest.cpp) includes it as it is.
 *
 *   declarations  a vertex shader declaration of Direct3D 8 (the D3DVSD_* tokens: streams, registers of a stream, skipped data, the tessellator's
 *                 normal and coordinates, constants, extensions) becomes Direct3D 9 vertex elements. A register is given the usage Direct3D 8 gives it
 *                 (D3DVSDE_POSITION .. D3DVSDE_NORMAL2: position 0, blend weights, blend indices, normal 0, point size, colours 0 and 1, texture
 *                 coordinates 0 .. 7, position 1, normal 1). Constants (D3DVSD_CONST) are kept to be loaded when the shader is set.
 *   vertex        vs.1.0 and vs.1.1 have no dcl instructions (the declaration says what the registers are): vs_1_1 for Direct3D 9 gets a dcl for each
 *   shaders       register the declaration fills, with that register's usage, right after the version token; a register the shader reads that the
 *                 declaration has not is declared as an input no vertex element fills (binormal <register>), which Direct3D 9 reads as 0, 0, 0, 1.
 *   pixel         ps.1.0 .. ps.1.4: the same tokens in Direct3D 9, ps.1.0 as ps_1_1 (1.1 is 1.0 with more).
 *   shaders
 *   validation    the rules ValidateVertexShader and ValidatePixelShader check: the version, every instruction known to that version with its
 *                 parameters, registers of the kinds and numbers the version has, an end token; with a declaration, that every input the shader
 *                 reads is in it; with caps, the version they allow and (vertex shaders) the constants they have.
 *
 * Every function takes the most tokens it may read and refuses (D8_INVALID) what does not end within it. */
#ifndef TCOS_D3D8_SHADER_H
#define TCOS_D3D8_SHADER_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>      /* (memset: the DLL defines its own) */

enum { D8_OK = 0, D8_INVALID = 1 };

#define D8_REGISTERS 17u            /* v0 .. v16 of a declaration (D3DVSDE_NORMAL2 is 16); a shader reads v0 .. v15 */
#define D8_SHADER_INPUTS 16u
#define D8_MAX_ELEMENTS 40u         /* (17 registers and as many tessellator outputs, with room) */
#define D8_MAX_LOADS 64u            /* D3DVSD_CONST tokens of one declaration */
#define D8_CONSTANTS 256u           /* the vertex shader constants a declaration may load */
#define D8_END_TOKEN 0x0000FFFFu
#define D8_DECL_END 0xFFFFFFFFu

/* Direct3D 9's D3DVERTEXELEMENT9, member for member. */
typedef struct D8Element { uint16_t stream, offset; uint8_t type, method, usage, usage_index; } D8Element;
/* D3DVSD_CONST: `count` registers from `first`, their floats at declaration token `token` (4 a register). */
typedef struct D8Load { uint32_t first, count, token; } D8Load;

typedef struct D8Declaration {
  D8Element elements[D8_MAX_ELEMENTS];
  uint32_t element_count;
  uint32_t declared;                /* bit n: v<n> is filled by the declaration (a stream register or a tessellator output) */
  uint8_t usage[D8_REGISTERS], usage_index[D8_REGISTERS];
  D8Load loads[D8_MAX_LOADS];
  uint32_t load_count;
  uint32_t tokens;                  /* with the end token */
} D8Declaration;

typedef struct D8ShaderInfo {
  uint32_t version;                 /* the version token */
  uint32_t tokens;                  /* with the end token */
  uint32_t inputs_read;             /* a vertex shader: bit n, it reads v<n> */
  uint32_t constants_used;          /* the highest constant register read with an absolute number or defined (def), and one (0: none) */
  uint32_t instructions;
  uint32_t own_dcls;                /* a vertex shader: dcl instructions of its own (Direct3D 8 takes and ignores them: the declaration says) */
} D8ShaderInfo;

/* Direct3D 9 values (d3d9types.h) the translation writes. */
enum { D8_TYPE_FLOAT1 = 0, D8_TYPE_UNUSED = 17 };
enum { D8_METHOD_DEFAULT = 0, D8_METHOD_CROSSUV = 3, D8_METHOD_UV = 4 };
enum { D8_USAGE_POSITION = 0, D8_USAGE_BLENDWEIGHT = 1, D8_USAGE_BLENDINDICES = 2, D8_USAGE_NORMAL = 3, D8_USAGE_PSIZE = 4, D8_USAGE_TEXCOORD = 5, D8_USAGE_BINORMAL = 7,
       D8_USAGE_COLOR = 10 };

/* The bytes of each D3DVSDT_* type (FLOAT1 .. SHORT4, the same numbers as Direct3D 9's D3DDECLTYPE_*). */
static const uint8_t d8_type_bytes[8] = {4, 8, 12, 16, 4, 4, 4, 8};

/* The usage and index Direct3D 8 gives the registers of a declaration (D3DVSDE_*). */
static void d8_register_usage(uint32_t reg, uint8_t *usage, uint8_t *index) {
  static const uint8_t u[D8_REGISTERS] = {D8_USAGE_POSITION, D8_USAGE_BLENDWEIGHT, D8_USAGE_BLENDINDICES, D8_USAGE_NORMAL, D8_USAGE_PSIZE, D8_USAGE_COLOR, D8_USAGE_COLOR,
                                          D8_USAGE_TEXCOORD, D8_USAGE_TEXCOORD, D8_USAGE_TEXCOORD, D8_USAGE_TEXCOORD, D8_USAGE_TEXCOORD, D8_USAGE_TEXCOORD,
                                          D8_USAGE_TEXCOORD, D8_USAGE_TEXCOORD, D8_USAGE_POSITION, D8_USAGE_NORMAL};
  static const uint8_t n[D8_REGISTERS] = {0, 0, 0, 0, 0, 0, 1, 0, 1, 2, 3, 4, 5, 6, 7, 1, 1};
  *usage = reg < D8_REGISTERS ? u[reg] : (uint8_t)D8_USAGE_BINORMAL;
  *index = reg < D8_REGISTERS ? n[reg] : 0;
}

static int d8_add_element(D8Declaration *d, uint32_t stream, uint32_t offset, uint32_t type, uint32_t method, uint32_t reg) {
  if (d->element_count >= D8_MAX_ELEMENTS || reg >= D8_REGISTERS || (d->declared >> reg & 1u)) return D8_INVALID;
  D8Element *e = &d->elements[d->element_count++];
  e->stream = (uint16_t)stream;
  e->offset = (uint16_t)offset;
  e->type = (uint8_t)type;
  e->method = (uint8_t)method;
  d8_register_usage(reg, &e->usage, &e->usage_index);
  d->usage[reg] = e->usage;
  d->usage_index[reg] = e->usage_index;
  d->declared |= 1u << reg;
  return D8_OK;
}

/* Reads a declaration (at most `max` tokens) into `d`. */
static int d8_declaration_parse(const uint32_t *t, size_t max, D8Declaration *d) {
  if (!t || !d) return D8_INVALID;
  memset(d, 0, sizeof *d);
  uint32_t stream = 0, offset = 0;
  int have_stream = 0, tess_stream = 0;
  for (size_t i = 0; i < max; ++i) {
    const uint32_t tok = t[i];
    if (tok == D8_DECL_END) {
      d->tokens = (uint32_t)(i + 1);
      return D8_OK;
    }
    switch (tok >> 29) {
    case 0:      /* D3DVSD_NOP */
      if (tok) return D8_INVALID;
      break;
    case 1:      /* D3DVSD_STREAM(n), D3DVSD_STREAM_TESS() */
      if (tok & 0x10000000u) {
        if (tok & 0x0FFFFFFFu) return D8_INVALID;
        tess_stream = 1;
      } else {
        if (tok & 0x0FFFFFF0u) return D8_INVALID;
        tess_stream = 0;
        stream = tok & 0xFu;
      }
      have_stream = 1;
      offset = 0;
      break;
    case 2: {    /* D3DVSD_REG(register, type), D3DVSD_SKIP(dwords); D3DVSD_SKIPBYTES(bytes) as Xbox's Direct3D 8 writes it (bit 27 with the skip bit) */
      if (!have_stream || tess_stream) return D8_INVALID;
      uint32_t size;
      if (tok & 0x10000000u) {
        const uint32_t count = (tok >> 16) & 0xFu;
        if (tok & 0x07F0FFFFu) return D8_INVALID;
        size = (tok & 0x08000000u) ? count : count * 4u;
      } else {
        const uint32_t reg = tok & 0x1Fu, type = (tok >> 16) & 0xFu;
        if ((tok & 0x0FF0FFE0u) || type >= 8u || reg >= D8_REGISTERS) return D8_INVALID;
        if (d8_add_element(d, stream, offset, type, D8_METHOD_DEFAULT, reg)) return D8_INVALID;
        size = d8_type_bytes[type];
      }
      if (size > 0xFFFFu - offset) return D8_INVALID;
      offset += size;
      break;
    }
    case 3: {    /* D3DVSD_TESSNORMAL(in, out), D3DVSD_TESSUV(register): what the tessellator makes, read from no stream */
      if (!have_stream || !tess_stream) return D8_INVALID;
      const uint32_t out = tok & 0x1Fu;
      if (tok & 0x10000000u) {
        if ((tok & 0x0FF0FFE0u) || ((tok >> 16) & 0xFu) != 1u) return D8_INVALID;
        if (d8_add_element(d, 0, 0, D8_TYPE_UNUSED, D8_METHOD_UV, out)) return D8_INVALID;
      } else {
        const uint32_t in = (tok >> 20) & 0xFu;
        if ((tok & 0x0F00FFE0u) || ((tok >> 16) & 0xFu) != 2u || !(d->declared >> in & 1u)) return D8_INVALID;
        const D8Element *position = NULL;      /* (the normal is the cross product of the derivatives of the input register, a position) */
        for (uint32_t e = 0; e < d->element_count; ++e)
          if (d->elements[e].usage == d->usage[in] && d->elements[e].usage_index == d->usage_index[in]) position = &d->elements[e];
        if (!position || position->method != D8_METHOD_DEFAULT) return D8_INVALID;
        if (d8_add_element(d, position->stream, position->offset, D8_TYPE_UNUSED, D8_METHOD_CROSSUV, out)) return D8_INVALID;
      }
      break;
    }
    case 4: {    /* D3DVSD_CONST(first, count) and count * 4 floats */
      const uint32_t count = (tok >> 25) & 0xFu, first = tok & 0x7Fu;
      if ((tok & 0x01FFFF80u) || !count || first + count > D8_CONSTANTS || d->load_count >= D8_MAX_LOADS) return D8_INVALID;
      if ((size_t)count * 4u >= max - i) return D8_INVALID;
      D8Load *load = &d->loads[d->load_count++];
      load->first = first;
      load->count = count;
      load->token = (uint32_t)(i + 1);
      i += (size_t)count * 4u;
      break;
    }
    case 5: {    /* D3DVSD_EXT: so many tokens of an extension, which nothing here reads */
      const uint32_t count = (tok >> 24) & 0x1Fu;
      if ((size_t)count >= max - i) return D8_INVALID;
      i += count;
      break;
    }
    default:     /* (6 is no kind; 7 only as the end token) */
      return D8_INVALID;
    }
  }
  return D8_INVALID;
}

/* ---- shader tokens ---------------------------------------------------------------------------------------------------------------------------------- */

enum { D8R_TEMP = 0, D8R_INPUT = 1, D8R_CONST = 2, D8R_ADDR = 3, D8R_TEXTURE = 3, D8R_RASTOUT = 4, D8R_ATTROUT = 5, D8R_TEXCRDOUT = 6 };

static uint32_t d8_reg_type(uint32_t param) { return (param >> 28) & 7u; }
static uint32_t d8_reg_number(uint32_t param) { return param & 0x7FFu; }

/* The parameters an instruction of Shader Model 1 has (they carry no length), or -1 if `vertex` shaders of version `minor` (1.x) have no such
 * instruction. */
static int d8_parameters(uint32_t op, int vertex, uint32_t minor) {
  if (vertex) {
    switch (op) {
    case 0: return 0;                                                                    /* nop */
    case 1: case 6: case 7: case 14: case 15: case 16: case 19: case 78: case 79: return 2;   /* mov rcp rsq exp log lit frc expp logp */
    case 2: case 3: case 5: case 8: case 9: case 10: case 11: case 12: case 13: case 17: return 3;   /* add sub mul dp3 dp4 min max slt sge dst */
    case 20: case 21: case 22: case 23: case 24: return 3;                               /* m4x4 m4x3 m3x4 m3x3 m3x2 */
    case 4: return 4;                                                                    /* mad */
    case 31: return 2;                                                                   /* dcl (Direct3D 9's form; Direct3D 8 takes it) */
    case 81: return 5;                                                                   /* def: a register and four numbers */
    default: return -1;
    }
  }
  switch (op) {
  case 0: return 0;                                     /* nop */
  case 1: return 2;                                     /* mov */
  case 2: case 3: case 5: case 8: return 3;             /* add sub mul dp3 */
  case 4: case 18: case 80: return 4;                   /* mad lrp cnd */
  case 9: return minor >= 2 ? 3 : -1;                   /* dp4 */
  case 88: return minor >= 2 ? 4 : -1;                  /* cmp */
  case 81: return 5;                                    /* def: a register and four numbers */
  case 64: case 66: return minor >= 4 ? 2 : 1;          /* texcoord / texcrd, tex / texld */
  case 65: return 1;                                    /* texkill */
  case 67: case 68: case 69: case 70: case 71: case 72: case 73: case 74: case 77: return minor >= 4 ? -1 : 2;   /* texbem texbeml texreg2ar texreg2gb texm3x2pad texm3x2tex texm3x3pad texm3x3tex texm3x3vspec */
  case 76: return minor >= 4 ? -1 : 3;                  /* texm3x3spec */
  case 82: case 83: case 85: case 86: return minor >= 2 && minor < 4 ? 2 : -1;   /* texreg2rgb texdp3tex texdp3 texm3x3 */
  case 84: return minor == 3 ? 2 : -1;                  /* texm3x2depth */
  case 87: return minor >= 4 ? 1 : -1;                  /* texdepth */
  case 89: return minor >= 4 ? 3 : -1;                  /* bem */
  case 0xFFFD: return minor >= 4 ? 0 : -1;              /* phase */
  default: return -1;
  }
}

/* Whether a register of this kind and number is one the version has (as a destination when `dest`). */
static int d8_register_ok(uint32_t param, int vertex, uint32_t minor, int dest) {
  const uint32_t type = d8_reg_type(param), n = d8_reg_number(param);
  if ((param >> 11) & 3u) return 0;      /* (the high register type bits of Shader Model 2 and later) */
  if (vertex) {
    switch (type) {
    case D8R_TEMP: return n < 12;
    case D8R_INPUT: return !dest && n < D8_SHADER_INPUTS;
    case D8R_CONST: return !dest && n < 256;
    case D8R_ADDR: return dest && n == 0;      /* a0 is written (mov); read only through relative addressing */
    case D8R_RASTOUT: return dest && n < 3;
    case D8R_ATTROUT: return dest && n < 2;
    case D8R_TEXCRDOUT: return dest && n < 8;
    default: return 0;
    }
  }
  switch (type) {
  case D8R_TEMP: return n < (minor >= 4 ? 6u : 2u);
  case D8R_INPUT: return !dest && n < 2;
  case D8R_CONST: return !dest && n < 8;
  case D8R_TEXTURE: return n < (minor >= 4 ? 6u : 4u);
  default: return 0;
  }
}

/* Walks a shader of Direct3D 8: refuses what is not vs.1.0 / vs.1.1 (vertex) or ps.1.0 .. ps.1.4, an instruction the version has not, a register it
 * has not, and a stream with no end within `max` tokens; says what it reads. */
static int d8_shader_scan(const uint32_t *code, size_t max, int vertex, D8ShaderInfo *info) {
  if (!code || !info || !max) return D8_INVALID;
  memset(info, 0, sizeof *info);
  const uint32_t version = code[0];
  if (vertex ? (version != 0xFFFE0100u && version != 0xFFFE0101u) : (version < 0xFFFF0100u || version > 0xFFFF0104u)) return D8_INVALID;
  const uint32_t minor = version & 0xFFu;
  info->version = version;
  size_t i = 1;
  while (i < max) {
    const uint32_t tok = code[i];
    if (tok == D8_END_TOKEN) {
      info->tokens = (uint32_t)(i + 1);
      return D8_OK;
    }
    if ((tok & 0xFFFFu) == 0xFFFEu) {      /* a comment of so many tokens */
      const uint32_t words = (tok >> 16) & 0x7FFFu;
      if (tok & 0x80000000u || (size_t)words >= max - i) return D8_INVALID;
      i += 1u + words;
      continue;
    }
    if (tok & 0x80000000u) return D8_INVALID;
    if ((tok & 0x40000000u) && vertex) return D8_INVALID;      /* (co-issue is the pixel shaders') */
    const uint32_t op = tok & 0xFFFFu;
    const int n = d8_parameters(op, vertex, minor);
    if (n < 0 || (size_t)n >= max - i) return D8_INVALID;
    ++info->instructions;
    if (op == 31) ++info->own_dcls;
    for (int p = 0; p < n; ++p) {
      const uint32_t param = code[i + 1 + (size_t)p];
      if (op == 81 && p > 0) continue;      /* (def's numbers) */
      if (!(param & 0x80000000u)) return D8_INVALID;
      if (op == 81) {      /* def: the constant register it defines */
        const uint32_t number = d8_reg_number(param);
        if (d8_reg_type(param) != D8R_CONST || number >= (vertex ? 256u : 8u) || ((param >> 11) & 3u)) return D8_INVALID;
        if (number + 1u > info->constants_used) info->constants_used = number + 1u;
        continue;
      }
      if (op == 31) {      /* dcl: the usage, then the input register */
        if (p == 1 && (d8_reg_type(param) != D8R_INPUT || d8_reg_number(param) >= D8_SHADER_INPUTS || ((param >> 11) & 3u))) return D8_INVALID;
        continue;
      }
      /* The first parameter is what the instruction writes, but texkill's one parameter is a register it reads. */
      const int write = p == 0 && op != 65;
      if (!d8_register_ok(param, vertex, minor, write)) return D8_INVALID;
      if (!write) {
        const uint32_t type = d8_reg_type(param), number = d8_reg_number(param);
        if (vertex && type == D8R_INPUT) info->inputs_read |= 1u << number;
        if (type == D8R_CONST && !(param & 0x2000u) && number + 1u > info->constants_used) info->constants_used = number + 1u;
      }
    }
    i += 1u + (size_t)n;
  }
  return D8_INVALID;
}

/* The vs_1_1 for Direct3D 9 of a vs.1.0 / vs.1.1 of Direct3D 8 (scanned: `info`) and its declaration: the version, a dcl for each register the
 * declaration fills or the shader reads, the instructions. Returns the tokens it takes; writes them when `out` has room for them (`capacity`). */
static size_t d8_vertex_shader_tokens(const uint32_t *code, const D8ShaderInfo *info, const D8Declaration *d, uint32_t *out, size_t capacity) {
  const uint32_t registers = (d ? d->declared : 0u) | info->inputs_read;
  size_t dcls = 0;
  for (uint32_t r = 0; r < D8_SHADER_INPUTS; ++r) dcls += registers >> r & 1u;
  const size_t total = (size_t)info->tokens - 3u * info->own_dcls + 3u * dcls;
  if (!out || capacity < total) return total;
  size_t w = 0;
  out[w++] = 0xFFFE0101u;
  for (uint32_t r = 0; r < D8_SHADER_INPUTS; ++r) {
    if (!(registers >> r & 1u)) continue;
    uint8_t usage = D8_USAGE_BINORMAL, index = (uint8_t)r;      /* (read, but no element fills it: an input no element has) */
    if (d && (d->declared >> r & 1u)) { usage = d->usage[r]; index = d->usage_index[r]; }
    out[w++] = 0x0000001Fu;                                     /* dcl */
    out[w++] = 0x80000000u | usage | ((uint32_t)index << 16);
    out[w++] = 0x80000000u | ((uint32_t)D8R_INPUT << 28) | 0x000F0000u | r;      /* v<r>.xyzw */
  }
  /* The instructions, without the shader's own dcl instructions (the scan checked every one; comments and the end go as they are). */
  for (size_t i = 1; i < info->tokens;) {
    const uint32_t tok = code[i];
    size_t length = 1;
    if ((tok & 0xFFFFu) == 0xFFFEu) length += (tok >> 16) & 0x7FFFu;
    else if (tok != D8_END_TOKEN) length += (size_t)d8_parameters(tok & 0xFFFFu, 1, info->version & 0xFFu);
    if ((tok & 0xFFFFu) != 31u || tok == D8_END_TOKEN)
      for (size_t k = 0; k < length; ++k) out[w++] = code[i + k];
    i += length;
  }
  return w;
}

/* A pixel shader's tokens for Direct3D 9: as they are, ps.1.0 as ps_1_1. */
static size_t d8_pixel_shader_tokens(const uint32_t *code, const D8ShaderInfo *info, uint32_t *out, size_t capacity) {
  if (!out || capacity < info->tokens) return info->tokens;
  out[0] = info->version == 0xFFFF0100u ? 0xFFFF0101u : info->version;
  for (size_t i = 1; i < info->tokens; ++i) out[i] = code[i];
  return info->tokens;
}

/* ---- ValidateVertexShader, ValidatePixelShader ------------------------------------------------------------------------------------------------------ */

enum { D8V_OK = 0, D8V_NO_CODE, D8V_VERSION, D8V_CAPS_VERSION, D8V_CODE, D8V_CONSTANTS, D8V_DECLARATION, D8V_INPUT };
static const char *const d8_validation_messages[] = {
  "",
  "Invalid code pointer.\n",
  "Unsupported shader version.\n",
  "Shader version not supported by caps.\n",
  "Invalid instruction, parameter or register, or no end token.\n",
  "Constant register out of the range of the caps.\n",
  "Invalid vertex shader declaration.\n",
  "The shader reads an input register that is not in the declaration.\n",
};

/* `caps_version`: 0 without caps, else the caps' version token; `caps_constants`: the caps' MaxVertexShaderConst. */
static int d8_validate_vertex_shader(const uint32_t *code, size_t max, const uint32_t *declaration, size_t declaration_max, uint32_t caps_version, uint32_t caps_constants) {
  if (!code || !max) return D8V_NO_CODE;
  if (code[0] != 0xFFFE0100u && code[0] != 0xFFFE0101u) return D8V_VERSION;
  if (caps_version && (code[0] & 0xFFFFu) > (caps_version & 0xFFFFu)) return D8V_CAPS_VERSION;
  D8ShaderInfo info;
  if (d8_shader_scan(code, max, 1, &info)) return D8V_CODE;
  if (caps_version && info.constants_used > caps_constants) return D8V_CONSTANTS;
  if (declaration) {
    D8Declaration d;
    if (d8_declaration_parse(declaration, declaration_max, &d)) return D8V_DECLARATION;
    if (info.inputs_read & ~d.declared) return D8V_INPUT;
  }
  return D8V_OK;
}
static int d8_validate_pixel_shader(const uint32_t *code, size_t max, uint32_t caps_version) {
  if (!code || !max) return D8V_NO_CODE;
  if (code[0] < 0xFFFF0100u || code[0] > 0xFFFF0104u) return D8V_VERSION;
  if (caps_version && (code[0] & 0xFFFFu) > (caps_version & 0xFFFFu)) return D8V_CAPS_VERSION;
  D8ShaderInfo info;
  if (d8_shader_scan(code, max, 0, &info)) return D8V_CODE;
  return D8V_OK;
}

#endif
