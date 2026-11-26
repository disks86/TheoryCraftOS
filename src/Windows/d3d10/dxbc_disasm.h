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

/* The disassembler of Direct3D 10's shaders (D3D10DisassembleShader): a DXBC container as the text Microsoft's compiler lists it - the resource bindings and the
 * signatures as comments, the profile, then every declaration and instruction of the SHDR (shader model 4.0 / 4.1) or SHEX (5.x) code with its modifiers, operands,
 * masks, swizzles, indices and immediate values, and the instruction count. With colour coding the text is an HTML page whose opcodes, registers and comments have
 * colours of their own. Plain C with no library underneath (a host test includes it): the text grows through DXD_REALLOC (the includer defines it as realloc-like,
 * NULL on failure, and DXD_FREE), and every read of the code is bounds-checked - a token past an instruction's length ends that instruction as "(truncated)". */
#ifndef TCOS_DXBC_DISASM_H
#define TCOS_DXBC_DISASM_H

#include "dxbc_reflect.h"

typedef struct DxdText { char *data; size_t size, capacity; int failed; int html; } DxdText;

static void dxd_raw(DxdText *t, const char *s, size_t n) {
  if (t->failed) return;
  if (n > (size_t)-1 - t->size - 1) { t->failed = 1; return; }
  if (t->size + n + 1 > t->capacity) {
    size_t want = t->capacity ? t->capacity : 1024;
    while (want < t->size + n + 1) {
      if (want > (size_t)-1 / 2) { t->failed = 1; return; }
      want *= 2;
    }
    char *grown = (char *)DXD_REALLOC(t->data, want);
    if (!grown) { t->failed = 1; return; }
    t->data = grown;
    t->capacity = want;
  }
  for (size_t i = 0; i < n; ++i) t->data[t->size + i] = s[i];
  t->size += n;
  t->data[t->size] = 0;
}
/* Text of the program's (HTML-escaped in a colour-coded listing). */
static void dxd_str(DxdText *t, const char *s) {
  if (!s) s = "";
  for (; *s; ++s) {
    if (t->html && (*s == '<' || *s == '>' || *s == '&')) dxd_raw(t, *s == '<' ? "&lt;" : *s == '>' ? "&gt;" : "&amp;", *s == '&' ? 5 : 4);
    else dxd_raw(t, s, 1);
  }
}
static void dxd_lit(DxdText *t, const char *s) { size_t n = 0; while (s[n]) ++n; dxd_raw(t, s, n); }
static void dxd_color(DxdText *t, const char *color) { if (t->html) { dxd_lit(t, "<font color=\""); dxd_lit(t, color); dxd_lit(t, "\">"); } }
static void dxd_end_color(DxdText *t) { if (t->html) dxd_lit(t, "</font>"); }
static void dxd_uint(DxdText *t, uint64_t v) {
  char b[24];
  int n = 0;
  do { b[n++] = (char)('0' + v % 10); v /= 10; } while (v);
  while (n) dxd_raw(t, &b[--n], 1);
}
static void dxd_int(DxdText *t, int64_t v) {
  if (v < 0) { dxd_lit(t, "-"); dxd_uint(t, (uint64_t)0 - (uint64_t)v); }
  else dxd_uint(t, (uint64_t)v);
}
static void dxd_hex(DxdText *t, uint32_t v, int digits) {
  static const char x[] = "0123456789abcdef";
  dxd_lit(t, "0x");
  for (int i = digits - 1; i >= 0; --i) dxd_raw(t, &x[(v >> (4 * i)) & 15], 1);
}
/* A column of `width` characters: the text right-aligned (left with `left`). */
static void dxd_pad(DxdText *t, const char *s, int width, int left) {
  int n = 0;
  if (!s) s = "";
  while (s[n]) ++n;
  if (!left) for (int i = n; i < width; ++i) dxd_lit(t, " ");
  dxd_str(t, s);
  if (left) for (int i = n; i < width; ++i) dxd_lit(t, " ");
}
static void dxd_pad_uint(DxdText *t, uint32_t v, int width) {
  char b[12];
  int n = 0;
  do { b[n++] = (char)('0' + v % 10); v /= 10; } while (v);
  char s[13];
  for (int i = 0; i < n; ++i) s[i] = b[n - 1 - i];
  s[n] = 0;
  dxd_pad(t, s, width, 0);
}
/* A float as the compiler prints it: six decimals, or the exponent form past 1e15; inf and nan by name. */
static void dxd_float_bits(DxdText *t, uint32_t bits) {
  const int negative = (int)(bits >> 31);
  const uint32_t exponent = (bits >> 23) & 0xFF, mantissa = bits & 0x7FFFFF;
  if (exponent == 0xFF) { dxd_lit(t, mantissa ? "nan" : negative ? "-inf" : "inf"); return; }
  /* (the value as a 2^-150 fixed point number: an integer part and six decimals, rounded, from the exact bits) */
  const uint64_t m = exponent ? (mantissa | 0x800000u) : mantissa;
  const int shift = exponent ? (int)exponent - 150 : -149;      /* value = m * 2^shift */
  if (negative) dxd_lit(t, "-");
  if (shift >= 0) {
    if (shift > 40) {
      /* (huge: the exponent form, from the double the float is) */
      double v = (double)m;
      for (int i = 0; i < shift; ++i) v *= 2.0;
      int e = 0;
      while (v >= 10.0) { v /= 10.0; ++e; }
      uint64_t d = (uint64_t)(v * 1000000.0 + 0.5);
      dxd_uint(t, d / 1000000);
      dxd_lit(t, ".");
      char f[7];
      uint64_t frac = d % 1000000;
      for (int i = 5; i >= 0; --i) { f[i] = (char)('0' + frac % 10); frac /= 10; }
      dxd_raw(t, f, 6);
      dxd_lit(t, "e+");
      dxd_uint(t, (uint64_t)e);
      return;
    }
    dxd_uint(t, m << shift);
    dxd_lit(t, ".000000");
    return;
  }
  /* integer part and fraction of m / 2^-shift */
  const int down = -shift;
  uint64_t whole, scaled;
  if (down >= 64) whole = 0;
  else whole = m >> down;
  /* fraction * 10^6, rounded: computed in pieces so nothing overflows (m < 2^24) */
  uint64_t rem_num = down >= 64 ? m : (m & (((uint64_t)1 << down) - 1));
  /* rem_num / 2^down * 1e6 = rem_num * 1e6 >> down (rem_num * 1e6 < 2^44) */
  uint64_t prod = rem_num * 1000000u;
  if (down >= 64) scaled = 0;
  else {
    scaled = prod >> down;
    const uint64_t below = down ? (prod & (((uint64_t)1 << down) - 1)) : 0;
    if (down && below >= ((uint64_t)1 << (down - 1))) ++scaled;
  }
  if (down > 64 && prod) scaled = 0;
  if (scaled >= 1000000) { whole += 1; scaled -= 1000000; }
  dxd_uint(t, whole);
  dxd_lit(t, ".");
  char f[6];
  for (int i = 5; i >= 0; --i) { f[i] = (char)('0' + scaled % 10); scaled /= 10; }
  dxd_raw(t, f, 6);
}

/* ---- names ------------------------------------------------------------------------------------------------------------------------------------------------------ */

static const char *dxd_opcode_name(uint32_t op) {
  static const char *const names[] = {
      "add", "and", "break", "breakc", "call", "callc", "case", "continue", "continuec", "cut", "default", "deriv_rtx", "deriv_rty", "discard", "div", "dp2", "dp3", "dp4",
      "else", "emit", "emit_then_cut", "endif", "endloop", "endswitch", "eq", "exp", "frc", "ftoi", "ftou", "ge", "iadd", "if", "ieq", "ige", "ilt", "imad", "imax", "imin",
      "imul", "ine", "ineg", "ishl", "ishr", "itof", "label", "ld", "ld_ms", "log", "loop", "lt", "mad", "min", "max", "customdata", "mov", "movc", "mul", "ne", "nop", "not",
      "or", "resinfo", "ret", "retc", "round_ne", "round_ni", "round_pi", "round_z", "rsq", "sample", "sample_c", "sample_c_lz", "sample_l", "sample_d", "sample_b", "sqrt",
      "switch", "sincos", "udiv", "ult", "uge", "umul", "umad", "umax", "umin", "ushr", "utof", "xor", "dcl_resource", "dcl_constantbuffer", "dcl_sampler",
      "dcl_indexrange", "dcl_outputtopology", "dcl_inputprimitive", "dcl_maxout", "dcl_input", "dcl_input_sgv", "dcl_input_siv", "dcl_input_ps", "dcl_input_ps_sgv",
      "dcl_input_ps_siv", "dcl_output", "dcl_output_sgv", "dcl_output_siv", "dcl_temps", "dcl_indexableTemp", "dcl_globalFlags", "reserved_107", "lod", "gather4",
      "samplepos", "sample_info", "reserved_112", "hs_decls", "hs_control_point_phase", "hs_fork_phase", "hs_join_phase", "emit_stream", "cut_stream",
      "emit_then_cut_stream", "fcall", "bufinfo", "deriv_rtx_coarse", "deriv_rtx_fine", "deriv_rty_coarse", "deriv_rty_fine", "gather4_c", "gather4_po", "gather4_po_c",
      "rcp", "f32tof16", "f16tof32", "uaddc", "usubb", "countbits", "firstbit_hi", "firstbit_lo", "firstbit_shi", "ubfe", "ibfe", "bfi", "bfrev", "swapc", "dcl_stream",
      "dcl_function_body", "dcl_function_table", "dcl_interface", "dcl_input_control_point_count", "dcl_output_control_point_count", "dcl_tessellator_domain",
      "dcl_tessellator_partitioning", "dcl_tessellator_output_primitive", "dcl_hs_max_tessfactor", "dcl_hs_fork_phase_instance_count",
      "dcl_hs_join_phase_instance_count", "dcl_thread_group", "dcl_uav_typed", "dcl_uav_raw", "dcl_uav_structured", "dcl_tgsm_raw", "dcl_tgsm_structured",
      "dcl_resource_raw", "dcl_resource_structured", "ld_uav_typed", "store_uav_typed", "ld_raw", "store_raw", "ld_structured", "store_structured", "atomic_and",
      "atomic_or", "atomic_xor", "atomic_cmp_store", "atomic_iadd", "atomic_imax", "atomic_imin", "atomic_umax", "atomic_umin", "imm_atomic_alloc", "imm_atomic_consume",
      "imm_atomic_iadd", "imm_atomic_and", "imm_atomic_or", "imm_atomic_xor", "imm_atomic_exch", "imm_atomic_cmp_exch", "imm_atomic_imax", "imm_atomic_imin",
      "imm_atomic_umax", "imm_atomic_umin", "sync", "dadd", "dmax", "dmin", "dmul", "deq", "dge", "dlt", "dne", "dmov", "dmovc", "dtof", "ftod", "eval_snapped",
      "eval_sample_index", "eval_centroid", "dcl_gs_instance_count", "abort", "debug_break", "reserved_209", "ddiv", "dfma", "drcp", "msad", "dtoi", "dtou", "itod",
      "utod", "reserved_218", "gather4_s", "gather4_c_s", "gather4_po_s", "gather4_po_c_s", "ld_s", "ld_ms_s", "ld_uav_typed_s", "ld_raw_s", "ld_structured_s",
      "sample_l_s", "sample_c_lz_s", "sample_cl_s", "sample_b_cl_s", "sample_d_cl_s", "sample_c_cl_s", "check_access_fully_mapped"};
  return op < sizeof names / sizeof names[0] ? names[op] : NULL;
}
static const char *dxd_system_value(uint32_t v) {
  static const char *const names[] = {"undefined", "position", "clip_distance", "cull_distance", "rendertarget_array_index", "viewport_array_index", "vertex_id",
                                      "primitive_id", "instance_id", "is_front_face", "sampleIndex", "finalQuadUeq0EdgeTessFactor", "finalQuadVeq0EdgeTessFactor",
                                      "finalQuadUeq1EdgeTessFactor", "finalQuadVeq1EdgeTessFactor", "finalQuadUInsideTessFactor", "finalQuadVInsideTessFactor",
                                      "finalTriUeq0EdgeTessFactor", "finalTriVeq0EdgeTessFactor", "finalTriWeq0EdgeTessFactor", "finalTriInsideTessFactor",
                                      "finalLineDetailTessFactor", "finalLineDensityTessFactor"};
  return v < sizeof names / sizeof names[0] ? names[v] : NULL;
}
/* (the signature comments' names: as the compiler lists them) */
static const char *dxd_signature_value(uint32_t v) {
  static const char *const names[] = {"NONE", "POS", "CLIPDST", "CULLDST", "RTINDEX", "VPINDEX", "VERTID", "PRIMID", "INSTID", "FFACE", "SAMPLE", "QUADEDGE", "QUADEDGE",
                                      "QUADEDGE", "QUADEDGE", "QUADINT", "QUADINT", "TRIEDGE", "TRIEDGE", "TRIEDGE", "TRIINT", "LINEDET", "LINEDEN"};
  if (v == 64) return "TARGET";
  if (v == 65) return "DEPTH";
  if (v == 66) return "COVERAGE";
  if (v == 67) return "DEPTHGE";
  if (v == 68) return "DEPTHLE";
  return v < sizeof names / sizeof names[0] ? names[v] : "UNKNOWN";
}
static const char *dxd_resource_dimension(uint32_t d) {
  static const char *const names[] = {"unknown", "buffer", "texture1d", "texture2d", "texture2dms", "texture3d", "texturecube", "texture1darray", "texture2darray",
                                      "texture2dmsarray", "texturecubearray", "raw_buffer", "structured_buffer"};
  return d < sizeof names / sizeof names[0] ? names[d] : "unknown";
}
static const char *dxd_return_type(uint32_t r) {
  static const char *const names[] = {"(invalid)", "unorm", "snorm", "sint", "uint", "float", "mixed", "double", "continued", "unused"};
  return r < sizeof names / sizeof names[0] ? names[r] : "(invalid)";
}
static const char *dxd_interpolation(uint32_t m) {
  static const char *const names[] = {"undefined", "constant", "linear", "linear centroid", "linear noperspective", "linear noperspective centroid", "linear sample",
                                      "linear noperspective sample"};
  return m < sizeof names / sizeof names[0] ? names[m] : "undefined";
}
static const char *dxd_topology(uint32_t p) {
  switch (p) {
    case 1: return "pointlist";
    case 2: return "linelist";
    case 3: return "linestrip";
    case 4: return "trianglelist";
    case 5: return "trianglestrip";
    case 10: return "linelist_adj";
    case 11: return "linestrip_adj";
    case 12: return "trianglelist_adj";
    case 13: return "trianglestrip_adj";
    default: return "undefined";
  }
}
static const char *dxd_primitive(uint32_t p) {
  switch (p) {
    case 1: return "point";
    case 2: return "line";
    case 3: return "triangle";
    case 6: return "lineadj";
    case 7: return "triangleadj";
    default: return p >= 8 && p <= 39 ? "patch" : "undefined";
  }
}

/* ---- operands ---------------------------------------------------------------------------------------------------------------------------------------------------- */

typedef struct DxdCode { const uint8_t *words; size_t count; } DxdCode;      /* (the instruction's words: count of them) */
static int dxd_word(const DxdCode *c, size_t at, uint32_t *v) {
  if (at >= c->count) return 0;
  *v = dxr_u32(c->words + 4 * at);
  return 1;
}
/* Whether an opcode's immediate operands are integers (printed as such) rather than floats. */
static int dxd_integer_op(uint32_t op) {
  switch (op) {
    case 1: case 3: case 5: case 6: case 8: case 30: case 31: case 32: case 33: case 34: case 35: case 36: case 37: case 38: case 39: case 40: case 41: case 42:
    case 43: case 45: case 46: case 59: case 60: case 63: case 76: case 78: case 79: case 80: case 81: case 82: case 83: case 84: case 85: case 86: case 87: case 132:
    case 133: case 134: case 135: case 136: case 137: case 138: case 139: case 140: case 141: case 163: case 164: case 165: case 166: case 167: case 168:
      return 1;
    default: return op >= 169 && op <= 189;
  }
}
static int dxd_operand(DxdText *t, const DxdCode *c, size_t *at, uint32_t op, int is_dest);
/* One index of an operand: an immediate (32 or 64 bits), an operand (relative), or both added. */
static int dxd_index(DxdText *t, const DxdCode *c, size_t *at, uint32_t rep, uint32_t op) {
  uint32_t v, hi;
  switch (rep) {
    case 0: if (!dxd_word(c, (*at)++, &v)) return 0; dxd_uint(t, v); return 1;
    case 1: if (!dxd_word(c, (*at)++, &hi) || !dxd_word(c, (*at)++, &v)) return 0; dxd_uint(t, ((uint64_t)hi << 32) | v); return 1;
    case 2: return dxd_operand(t, c, at, op, 0);
    case 3: if (!dxd_word(c, (*at)++, &v)) return 0; if (!dxd_operand(t, c, at, op, 0)) return 0; dxd_lit(t, " + "); dxd_uint(t, v); return 1;
    case 4: if (!dxd_word(c, (*at)++, &hi) || !dxd_word(c, (*at)++, &v)) return 0; if (!dxd_operand(t, c, at, op, 0)) return 0; dxd_lit(t, " + ");
            dxd_uint(t, ((uint64_t)hi << 32) | v); return 1;
    default: return 0;
  }
}
static const char *dxd_register_prefix(uint32_t type) {
  static const char *const names[] = {"r", "v", "o", "x", "l", "d", "s", "t", "cb", "icb", "label", "vPrim", "oDepth", "null", "rasterizer", "oMask", "m", "fb", "ft",
                                      "fp", "fi", "fo", "vOutputControlPointID", "vForkInstanceID", "vJoinInstanceID", "vicp", "vocp", "vpc", "vDomain", "this", "u", "g",
                                      "vThreadID", "vThreadGroupID", "vThreadIDInGroup", "vCoverage", "vThreadIDInGroupFlattened", "vGSInstanceID", "oDepthGE",
                                      "oDepthLE", "vCycleCounter", "oStencilRef", "vInnerCoverage"};
  return type < sizeof names / sizeof names[0] ? names[type] : "?";
}
static int dxd_operand(DxdText *t, const DxdCode *c, size_t *at, uint32_t op, int is_dest) {
  uint32_t token, ext = 0;
  if (!dxd_word(c, (*at)++, &token)) return 0;
  if (token & 0x80000000u) {
    if (!dxd_word(c, (*at)++, &ext)) return 0;
    if (ext & 0x80000000u) { uint32_t more; if (!dxd_word(c, (*at)++, &more)) return 0; }      /* (a further extension: skipped) */
  }
  const uint32_t components = token & 3, mode = (token >> 2) & 3, type = (token >> 12) & 0xFF, dims = (token >> 20) & 3;
  const uint32_t modifier = (ext & 0x3F) == 1 ? (ext >> 6) & 0xFF : 0;
  if (modifier & 1) dxd_lit(t, "-");
  if (modifier & 2) dxd_lit(t, "|");
  if (type == 4 || type == 5) {
    /* l(...) / d(...): the values */
    const uint32_t n = components == 2 ? 4 : components == 1 ? 1 : 0;
    dxd_lit(t, type == 4 ? "l(" : "d(");
    for (uint32_t i = 0; i < (type == 5 ? (n == 4 ? 2u : 1u) : n); ++i) {
      uint32_t v, v2 = 0;
      if (!dxd_word(c, (*at)++, &v)) return 0;
      if (type == 5 && !dxd_word(c, (*at)++, &v2)) return 0;
      if (i) dxd_lit(t, ", ");
      if (type == 5) {
        /* (a double: shown as the float it rounds to) */
        uint64_t b = ((uint64_t)v2 << 32) | v;
        const int sign = (int)(b >> 63);
        const int e = (int)((b >> 52) & 0x7FF) - 1023 + 127;
        uint32_t f = (uint32_t)sign << 31;
        if (((b >> 52) & 0x7FF) == 0x7FF) f |= 0x7F800000u | (uint32_t)((b & 0xFFFFFFFFFFFFFull) ? 1 : 0);
        else if (e >= 255) f |= 0x7F800000u;
        else if (e > 0) f |= ((uint32_t)e << 23) | (uint32_t)((b >> 29) & 0x7FFFFF);
        dxd_float_bits(t, f);
      } else if (dxd_integer_op(op) || ((v & 0x7F800000u) == 0 && v != 0x80000000u && v < 0x100000u) || v == 0xFFFFFFFFu) {
        if (dxd_integer_op(op) && v > 0x7FFFFFFFu && v != 0xFFFFFFFFu) dxd_hex(t, v, 8);
        else dxd_int(t, (int32_t)v);
      } else dxd_float_bits(t, v);
    }
    dxd_lit(t, ")");
  } else {
    dxd_color(t, "#6060c0");
    dxd_lit(t, dxd_register_prefix(type));
    const uint32_t reps[3] = {(token >> 22) & 7, (token >> 25) & 7, (token >> 28) & 7};
    /* (the first index follows the name for the register files that are numbered; the others are in brackets, as are relative ones) */
    for (uint32_t d = 0; d < dims; ++d) {
      /* (two-dimensional inputs and outputs - a geometry shader's vertices, control points - have both in brackets: v[3][0]) */
      const int bracket = d > 0 || reps[d] >= 2 || type == 3 || type == 9 || type == 25 || type == 26 || (dims >= 2 && (type == 1 || type == 2));
      if (bracket && !(d == 0 && type == 9)) dxd_lit(t, "[");
      if (type == 9 && d == 0) dxd_lit(t, "[");
      if (!dxd_index(t, c, at, reps[d], op)) return 0;
      if (bracket || (type == 9 && d == 0)) dxd_lit(t, "]");
    }
    dxd_end_color(t);
    if (components == 2) {
      static const char xyzw[] = "xyzw";
      if (mode == 0) {
        const uint32_t mask = (token >> 4) & 15;
        if (mask && mask != 15) { dxd_lit(t, "."); for (int i = 0; i < 4; ++i) if (mask & (1u << i)) dxd_raw(t, &xyzw[i], 1); }
        else if (mask == 15 && is_dest) dxd_lit(t, ".xyzw");
      } else if (is_dest == 2) {
        /* (a declaration's swizzle says nothing: left out, as the compiler lists it) */
      } else if (mode == 1) {
        dxd_lit(t, ".");
        for (int i = 0; i < 4; ++i) dxd_raw(t, &xyzw[(token >> (4 + 2 * i)) & 3], 1);
      } else {
        dxd_lit(t, ".");
        dxd_raw(t, &xyzw[(token >> 4) & 3], 1);
      }
    }
  }
  if (modifier & 2) dxd_lit(t, "|");
  return 1;
}

/* ---- the listing ---------------------------------------------------------------------------------------------------------------------------------------------------- */

static const char *dxd_profile_prefix(uint32_t type) {
  static const char *const names[] = {"ps", "vs", "gs", "hs", "ds", "cs"};
  return type < 6 ? names[type] : "xs";
}
static void dxd_signature_comment(DxdText *t, const uint8_t *chunk, size_t size, uint32_t tag, const char *title, int output) {
  const int64_t n = dxr_signature_count(chunk, size, tag);
  if (n < 0) return;
  dxd_lit(t, "//\n// ");
  dxd_lit(t, title);
  dxd_lit(t, ":\n//\n// Name                 Index   Mask Register SysValue  Format   Used\n// -------------------- ----- ------ -------- -------- ------- ------\n");
  for (uint32_t i = 0; i < (uint32_t)n; ++i) {
    DxrSignature e;
    if (!dxr_signature_element(chunk, size, tag, i, &e)) continue;
    static const char xyzw[] = "xyzw";
    char mask[5], used[5];
    for (int k = 0; k < 4; ++k) {
      mask[k] = (e.mask >> k) & 1 ? xyzw[k] : ' ';
      const uint8_t u = output ? (uint8_t)(~e.rw_mask & e.mask) : e.rw_mask;      /* (an output's byte is what is never written) */
      used[k] = (u >> k) & 1 ? xyzw[k] : ' ';
    }
    mask[4] = used[4] = 0;
    dxd_lit(t, "// ");
    dxd_pad(t, e.name, 20, 1);
    dxd_lit(t, " ");
    dxd_pad_uint(t, e.semantic_index, 5);
    dxd_lit(t, "   ");
    dxd_str(t, mask);
    dxd_lit(t, " ");
    if (e.reg == 0xFFFFFFFFu) dxd_pad(t, "N/A", 8, 0);
    else dxd_pad_uint(t, e.reg, 8);
    dxd_lit(t, " ");
    dxd_pad(t, dxd_signature_value(e.system_value), 8, 0);
    dxd_lit(t, " ");
    dxd_pad(t, e.component_type == 1 ? "uint" : e.component_type == 2 ? "int" : e.component_type == 3 ? "float" : "unknown", 7, 0);
    dxd_lit(t, "   ");
    dxd_str(t, used);
    dxd_lit(t, "\n");
  }
  dxd_lit(t, "//\n");
}
static void dxd_bindings_comment(DxdText *t, const uint8_t *data, size_t size) {
  const uint8_t *chunk;
  size_t bytes;
  DxrRdef r;
  if (!dxr_find(data, size, DXR_RDEF, &chunk, &bytes) || !dxr_rdef(chunk, bytes, &r)) return;
  if (r.creator) { dxd_lit(t, "//\n// Generated by "); dxd_str(t, r.creator); dxd_lit(t, "\n"); }
  if (!r.bind_count) return;
  dxd_lit(t, "//\n//\n// Resource Bindings:\n//\n// Name                                 Type  Format         Dim Slot Elements\n"
             "// ------------------------------ ---------- ------- ----------- ---- --------\n");
  static const char *const types[] = {"cbuffer", "tbuffer", "texture", "sampler", "UAV", "UAV", "UAV", "UAV", "UAV", "UAV", "UAV", "UAV"};
  /* (the bindings' dimension is D3D_SRV_DIMENSION's order, not the shader tokens') */
  static const char *const dims[] = {"", "buf", "1d", "1darray", "2d", "2darray", "2dMS", "2darrayMS", "3d", "cube", "cubearray", "buf"};
  static const char *const formats[] = {"NA", "unorm", "snorm", "sint", "uint", "float", "mixed", "double", "cont", "NA"};
  for (uint32_t i = 0; i < r.bind_count; ++i) {
    DxrBinding b;
    if (!dxr_rdef_binding(&r, i, &b)) continue;
    dxd_lit(t, "// ");
    dxd_pad(t, b.name, 30, 1);
    dxd_lit(t, " ");
    dxd_pad(t, b.type < 12 ? types[b.type] : "unknown", 10, 0);
    dxd_lit(t, " ");
    dxd_pad(t, b.return_type < 10 && (b.type == 2 || b.type == 4) ? formats[b.return_type] : "NA", 7, 0);
    dxd_lit(t, " ");
    dxd_pad(t, b.dimension < 12 && b.type != 0 && b.type != 3 ? dims[b.dimension] : "NA", 11, 0);
    dxd_lit(t, " ");
    dxd_pad_uint(t, b.bind_point, 4);
    dxd_lit(t, " ");
    dxd_pad_uint(t, b.bind_count, 8);
    dxd_lit(t, "\n");
  }
  dxd_lit(t, "//\n");
}

/* One instruction (or declaration) at word `at` of the code: FALSE when its length runs past the code. `slots` counts the instructions that are not declarations. */
static int dxd_instruction(DxdText *t, const uint8_t *code, size_t words, size_t at, size_t *next, uint32_t *slots) {
  uint32_t token = dxr_u32(code + 4 * at);
  const uint32_t op = token & 0x7FF;
  size_t length = (token >> 24) & 0x7F;
  if (op == 53) {      /* customdata: the length is the next word */
    if (at + 1 >= words) return 0;
    length = dxr_u32(code + 4 * (at + 1));
  }
  if (length == 0 || length > words - at) return 0;
  *next = at + length;
  DxdCode c = {code + 4 * at, length};
  size_t w = 1;
  /* the extended opcode tokens: sample offsets, the resource dimension and return type (shader model 5) */
  int offsets = 0, aoff[3] = {0, 0, 0};
  uint32_t ext_token = token;
  while (ext_token & 0x80000000u) {
    if (!dxd_word(&c, w++, &ext_token)) return 0;
    if ((ext_token & 0x3F) == 1) {
      offsets = 1;
      for (int k = 0; k < 3; ++k) {
        int v = (int)((ext_token >> (9 + 4 * k)) & 15);
        aoff[k] = v >= 8 ? v - 16 : v;
      }
    }
  }
  const char *name = dxd_opcode_name(op);
  if (op == 53) {
    const uint32_t kind = token >> 11;
    if (kind == 3) {      /* an immediate constant buffer */
      dxd_color(t, "#408040");
      dxd_lit(t, "dcl_immediateConstantBuffer");
      dxd_end_color(t);
      dxd_lit(t, " { ");
      for (size_t k = 2; k + 3 < length; k += 4) {
        if (k > 2) dxd_lit(t, ",\n                              ");
        dxd_lit(t, "{ ");
        for (int j = 0; j < 4; ++j) {
          uint32_t v = dxr_u32(code + 4 * (at + k + j));
          if (j) dxd_lit(t, ", ");
          if ((v & 0x7F800000u) == 0 && v < 0x100000u) dxd_int(t, (int32_t)v);
          else dxd_float_bits(t, v);
        }
        dxd_lit(t, "}");
      }
      dxd_lit(t, " }\n");
    } else {
      dxd_lit(t, "// customdata ");
      dxd_lit(t, kind == 0 ? "comment" : kind == 1 ? "debuginfo" : kind == 2 ? "opaque" : "(unknown)");
      dxd_lit(t, ", ");
      dxd_uint(t, length - 2);
      dxd_lit(t, " words\n");
    }
    return 1;
  }
  const int decl = (op >= 88 && op <= 106) || (op >= 143 && op <= 162) || op == 206 || op == 113;
  if (!decl && op != 114 && op != 115 && op != 116) ++*slots;
  dxd_color(t, decl ? "#408040" : "#c06000");
  if (name) dxd_lit(t, name);
  else { dxd_lit(t, "opcode_"); dxd_uint(t, op); }
  /* the opcode's controls */
  const uint32_t controls = (token >> 11) & 0x1FFF;
  const int tests = op == 3 || op == 5 || op == 8 || op == 13 || op == 31 || op == 63;      /* (breakc, callc, continuec, discard, if, retc: the test of the value) */
  if (tests) dxd_lit(t, (token >> 18) & 1 ? "_nz" : "_z");
  if (op == 61) dxd_lit(t, (controls & 3) == 1 ? "_rcpFloat" : (controls & 3) == 2 ? "_uint" : "");
  if (op == 111 && (controls & 1)) dxd_lit(t, "_uint");
  if (!decl && !tests && op != 190 && (token >> 13) & 1) dxd_lit(t, "_sat");
  if (offsets) {
    dxd_lit(t, "_aoffimmi(");
    for (int k = 0; k < 3; ++k) { if (k) dxd_lit(t, ","); dxd_int(t, aoff[k]); }
    dxd_lit(t, ")");
  }
  if (op == 190) {      /* sync: its kinds */
    if (controls & 8) dxd_lit(t, "_uglobal");
    if (controls & 4) dxd_lit(t, "_ugroup");
    if (controls & 2) dxd_lit(t, "_g");
    if (controls & 1) dxd_lit(t, "_t");
  }
  if (op == 88 || op == 156) { dxd_lit(t, "_"); dxd_lit(t, dxd_resource_dimension((token >> 11) & 0x1F)); }
  dxd_end_color(t);
  dxd_lit(t, " ");
  int ok = 1;
  switch (op) {
    case 88: case 156: {      /* dcl_resource_* / dcl_uav_typed_*: (types) operand */
      uint32_t rt;
      size_t a = w;
      DxdText opnd = {0, 0, 0, 0, t->html};
      ok = dxd_operand(&opnd, &c, &a, op, 2);
      if (ok && dxd_word(&c, a++, &rt)) {
        dxd_lit(t, "(");
        for (int k = 0; k < 4; ++k) { if (k) dxd_lit(t, ","); dxd_lit(t, dxd_return_type((rt >> (4 * k)) & 15)); }
        dxd_lit(t, ") ");
        if (opnd.data) dxd_raw(t, opnd.data, opnd.size);
        const uint32_t dim = (token >> 11) & 0x1F;
        if (op == 88 && (dim == 4 || dim == 9)) { dxd_lit(t, ", "); dxd_uint(t, (token >> 16) & 0x7F); }
      } else ok = 0;
      if (opnd.data) DXD_FREE(opnd.data);
      w = a;
      break;
    }
    case 89: {
      size_t a = w;
      ok = dxd_operand(t, &c, &a, op, 2);
      dxd_lit(t, controls & 1 ? ", dynamicIndexed" : ", immediateIndexed");
      w = a;
      break;
    }
    case 90: {
      size_t a = w;
      ok = dxd_operand(t, &c, &a, op, 2);
      const uint32_t m = (token >> 11) & 15;
      dxd_lit(t, m == 1 ? ", mode_comparison" : m == 2 ? ", mode_mono" : ", mode_default");
      w = a;
      break;
    }
    case 92: dxd_lit(t, dxd_topology((token >> 11) & 0x7F)); break;
    case 93: dxd_lit(t, dxd_primitive((token >> 11) & 0x3F)); break;
    case 98: case 99: case 100: {
      dxd_lit(t, dxd_interpolation((token >> 11) & 15));
      dxd_lit(t, " ");
      size_t a = w;
      ok = dxd_operand(t, &c, &a, op, 2);
      if (ok && op != 98) { uint32_t sv; if (dxd_word(&c, a++, &sv)) { dxd_lit(t, ", "); const char *s = dxd_system_value(sv); if (s) dxd_lit(t, s); else dxd_uint(t, sv); } }
      w = a;
      break;
    }
    case 96: case 97: case 102: case 103: {
      size_t a = w;
      ok = dxd_operand(t, &c, &a, op, 2);
      uint32_t sv;
      if (ok && dxd_word(&c, a++, &sv)) { dxd_lit(t, ", "); const char *s = dxd_system_value(sv); if (s) dxd_lit(t, s); else dxd_uint(t, sv); }
      w = a;
      break;
    }
    case 106: {      /* dcl_globalFlags */
      static const char *const flags[] = {"refactoringAllowed", "enableDoublePrecisionFloatOps", "forceEarlyDepthStencil", "enableRawAndStructuredBuffers",
                                          "skipOptimization", "enableMinimumPrecision", "enable11_1DoubleExtensions", "enable11_1ShaderExtensions"};
      int first = 1;
      for (int k = 0; k < 8; ++k)
        if (controls & (1u << k)) { if (!first) dxd_lit(t, " | "); dxd_lit(t, flags[k]); first = 0; }
      break;
    }
    case 104: case 94: case 206: case 153: case 154: case 147: case 148: {
      uint32_t n;
      if (op == 147 || op == 148) dxd_uint(t, (token >> 11) & 0x3F);
      else if (dxd_word(&c, w++, &n)) dxd_uint(t, n);
      else ok = 0;
      break;
    }
    case 105: {      /* dcl_indexableTemp x#[size], components */
      uint32_t reg, size, comps;
      if (dxd_word(&c, w, &reg) && dxd_word(&c, w + 1, &size) && dxd_word(&c, w + 2, &comps)) {
        dxd_color(t, "#6060c0");
        dxd_lit(t, "x");
        dxd_uint(t, reg);
        dxd_lit(t, "[");
        dxd_uint(t, size);
        dxd_lit(t, "]");
        dxd_end_color(t);
        dxd_lit(t, ", ");
        dxd_uint(t, comps);
        w += 3;
      } else ok = 0;
      break;
    }
    case 155: {
      uint32_t x, y, z;
      if (dxd_word(&c, w, &x) && dxd_word(&c, w + 1, &y) && dxd_word(&c, w + 2, &z)) {
        dxd_uint(t, x); dxd_lit(t, ", "); dxd_uint(t, y); dxd_lit(t, ", "); dxd_uint(t, z);
        w += 3;
      } else ok = 0;
      break;
    }
    case 149: { const uint32_t d = (token >> 11) & 3; dxd_lit(t, d == 1 ? "domain_isoline" : d == 2 ? "domain_tri" : d == 3 ? "domain_quad" : "domain_undefined"); break; }
    case 150: {
      static const char *const p[] = {"partitioning_undefined", "partitioning_integer", "partitioning_pow2", "partitioning_fractional_odd", "partitioning_fractional_even"};
      const uint32_t v = (token >> 11) & 7;
      dxd_lit(t, v < 5 ? p[v] : "partitioning_undefined");
      break;
    }
    case 151: {
      static const char *const p[] = {"output_undefined", "output_point", "output_line", "output_triangle_cw", "output_triangle_ccw"};
      const uint32_t v = (token >> 11) & 7;
      dxd_lit(t, v < 5 ? p[v] : "output_undefined");
      break;
    }
    case 152: { uint32_t v; if (dxd_word(&c, w++, &v)) { dxd_lit(t, "l("); dxd_float_bits(t, v); dxd_lit(t, ")"); } else ok = 0; break; }
    default: {
      /* operands, then the words after them: the strides and counts of the declarations of index ranges, structured and raw memory (one operand first), the numbers
       * of function bodies, tables and interfaces (no operand) */
      const int raw_after = op == 91 || op == 158 || op == 159 || op == 160 || op == 162 ? 1 : (op == 144 || op == 145 || op == 146) ? 0 : -1;
      int index = 0;
      while (w < length && ok) {
        if (index) dxd_lit(t, ", ");
        if (raw_after >= 0 && index >= raw_after) {
          uint32_t v;
          if (!dxd_word(&c, w++, &v)) { ok = 0; break; }
          if (op == 144 && index == 0) dxd_lit(t, "fb");
          if (op == 145 && index == 0) dxd_lit(t, "ft");
          if (op == 146 && index == 0) dxd_lit(t, "fp");
          dxd_uint(t, v);
        } else {
          ok = dxd_operand(t, &c, &w, op, decl ? 2 : index == 0);
        }
        ++index;
      }
      break;
    }
  }
  if (!ok) dxd_lit(t, "(truncated)");
  dxd_lit(t, "\n");
  return 1;
}

/* The whole listing of a container: FALSE when it is not one, or has no code. `comments` (may be NULL) go first, each line as a comment. */
static int dxd_disassemble(DxdText *t, const uint8_t *data, size_t size, const char *comments) {
  const uint8_t *code;
  size_t words;
  int shex;
  if (!dxr_code(data, size, &code, &words, &shex)) return 0;
  if (t->html) dxd_lit(t, "<html><body><pre>\n");
  if (comments) {
    dxd_color(t, "#808080");
    int start = 1;
    for (const char *p = comments; *p; ++p) {
      if (start) { dxd_lit(t, "// "); start = 0; }
      if (*p == '\n') { dxd_lit(t, "\n"); start = 1; }
      else dxd_raw(t, p, 1);
    }
    if (!start) dxd_lit(t, "\n");
    dxd_end_color(t);
  }
  dxd_color(t, "#808080");
  dxd_bindings_comment(t, data, size);
  const uint8_t *chunk;
  size_t bytes;
  uint32_t tag;
  if (dxr_input_signature(data, size, &chunk, &bytes, &tag)) dxd_signature_comment(t, chunk, bytes, tag, "Input signature", 0);
  if (dxr_output_signature(data, size, &chunk, &bytes, &tag)) dxd_signature_comment(t, chunk, bytes, tag, "Output signature", 1);
  dxd_end_color(t);
  const uint32_t version = dxr_u32(code);
  dxd_color(t, "#408040");
  dxd_lit(t, dxd_profile_prefix(version >> 16));
  dxd_lit(t, "_");
  dxd_uint(t, (version >> 4) & 15);
  dxd_lit(t, "_");
  dxd_uint(t, version & 15);
  if (dxr_find(data, size, DXR_AON9, &chunk, &bytes)) dxd_lit(t, "_level_9_x");
  dxd_end_color(t);
  dxd_lit(t, "\n");
  uint32_t slots = 0;
  size_t at = 2;
  while (at < words) {
    size_t next;
    if (!dxd_instruction(t, code, words, at, &next, &slots)) { dxd_lit(t, "// (the code ends inside an instruction)\n"); break; }
    at = next;
  }
  dxd_color(t, "#808080");
  dxd_lit(t, "// Approximately ");
  uint32_t stat[DXR_STAT_WORDS];
  dxd_uint(t, dxr_stat(data, size, stat, DXR_STAT_WORDS) && stat[DXR_STAT_INSTRUCTIONS] ? stat[DXR_STAT_INSTRUCTIONS] : slots);
  dxd_lit(t, " instruction slots used\n");
  dxd_end_color(t);
  if (t->html) dxd_lit(t, "</pre></body></html>\n");
  return !t->failed;
}

#endif
