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

/* The texture stages of Direct3D 9's fixed-function pipeline as a pixel shader 2.0: what the stage states say is brought into one small key
 * (ffs_normalize), and a key is written out as the tokens of a shader (ffs_tokens) that sm_to_dxbc.h translates like a program's own - which also
 * gives it the alpha test and fog at its end. Used by d3d9.dll (docs/Direct3D9.md), which keeps the shaders it has made by their keys, and on the
 * host by tests/FfStagesTest.cpp. Plain C that is also C++; it needs <stdint.h>, <stddef.h> and memset from whoever includes it.
 *
 * What a shader made here reads:
 *   v0, v1      the diffuse and the specular colour
 *   t<n>        the texture coordinates of stage n, divided by their fourth number when they are looked up
 *   s<n>        the texture of stage n
 *   c0          the texture factor (D3DRS_TEXTUREFACTOR)
 *   c1          numbers of its own (it defines them)
 *   c<2 + n>    the constant of stage n (D3DTSS_CONSTANT)
 *   c<10 + n>   the bump environment matrix of stage n (D3DTSS_BUMPENVMAT00, 01, 10, 11)
 *   c<18 + n>   its luminance scale and offset (D3DTSS_BUMPENVLSCALE, BUMPENVLOFFSET, 0, 0)
 *
 * The bump environment operations (D3DTOP_BUMPENVMAP, BUMPENVMAPLUMINANCE: colour operations only) sample their stage's texture as du, dv (and a
 * luminance) and move the coordinates the next stage samples at by the stage's matrix times (du, dv): u + M00 du + M10 dv, v + M01 du + M11 dv; the
 * luminance one then scales what the next stage sampled by saturate(l * scale + offset). The colour such a stage passes on is the current one.
 * Not there: D3DTOP_PREMODULATE, which is the first argument (no hardware of Direct3D 9 has it: the caps say so). */
#ifndef TCOS_FF_STAGES_H
#define TCOS_FF_STAGES_H

#define FFS_STAGES 8u
#define FFS_CONSTANTS (2u + 3u * FFS_STAGES)      /* the constant registers such a shader has */
#define FFS_BUMP_MATRIX (2u + FFS_STAGES)         /* c<FFS_BUMP_MATRIX + n>: stage n's bump environment matrix */
#define FFS_BUMP_LUMINANCE (2u + 2u * FFS_STAGES) /* c<FFS_BUMP_LUMINANCE + n>: its luminance scale and offset */
#define FFS_MAX_TOKENS 1024u                 /* the tokens of one, at most */

/* The operations (D3DTOP_*) and the arguments (D3DTA_*), by Direct3D 9's numbers. */
enum { FFS_DISABLE = 1, FFS_SELECTARG1, FFS_SELECTARG2, FFS_MODULATE, FFS_MODULATE2X, FFS_MODULATE4X, FFS_ADD, FFS_ADDSIGNED, FFS_ADDSIGNED2X, FFS_SUBTRACT, FFS_ADDSMOOTH,
       FFS_BLENDDIFFUSEALPHA, FFS_BLENDTEXTUREALPHA, FFS_BLENDFACTORALPHA, FFS_BLENDTEXTUREALPHAPM, FFS_BLENDCURRENTALPHA, FFS_PREMODULATE, FFS_MODULATEALPHA_ADDCOLOR,
       FFS_MODULATECOLOR_ADDALPHA, FFS_MODULATEINVALPHA_ADDCOLOR, FFS_MODULATEINVCOLOR_ADDALPHA, FFS_BUMPENVMAP, FFS_BUMPENVMAPLUMINANCE, FFS_DOTPRODUCT3, FFS_MULTIPLYADD,
       FFS_LERP };
enum { FFS_DIFFUSE = 0, FFS_CURRENT, FFS_TEXTURE, FFS_TFACTOR, FFS_SPECULAR, FFS_TEMP, FFS_CONSTANT };
#define FFS_SELECT 0x0Fu
#define FFS_COMPLEMENT 0x10u            /* 1 - the argument */
#define FFS_ALPHAREPLICATE 0x20u        /* its alpha, in all four */

/* One stage: an operation and its arguments ([0] is D3DTSS_*ARG0, the third argument of FFS_MULTIPLYADD and FFS_LERP) for the colour and for the
 * alpha, and where the result goes. */
typedef struct FfsStage {
  uint8_t colour_op, colour_arg[3];
  uint8_t alpha_op, alpha_arg[3];
  uint8_t to_temp;                      /* the result goes to the temporary register, not on to the next stage (D3DTSS_RESULTARG) */
  uint8_t sample;                       /* (ffs_normalize says) the stage looks up its texture */
  uint8_t kill;                         /* a texel of alpha 0 takes the pixel away (DirectDraw's colour key: TCOS_TSS_KEY_KILL); the stage samples */
  uint8_t bump;                         /* (ffs_normalize says) the colour operation is a bump environment one that moves the next stage's coordinates: 1
                                         * D3DTOP_BUMPENVMAP, 2 BUMPENVMAPLUMINANCE; the stage samples */
} FfsStage;
/* All of a pixel shader made here: two shaders of the same key are the same shader. */
typedef struct FfsKey {
  uint8_t stages;                       /* the stages that do something: the first of them */
  uint8_t specular;                     /* the specular colour is added after the stages (D3DRS_SPECULARENABLE) */
  FfsStage stage[FFS_STAGES];
} FfsKey;

/* Which of its three arguments an operation reads: bit n for argument n. */
static unsigned ffs_arguments(unsigned op) {
  switch (op) {
    case FFS_SELECTARG1: case FFS_PREMODULATE: return 2u;
    case FFS_SELECTARG2: return 4u;
    case FFS_MULTIPLYADD: case FFS_LERP: return 7u;
    case FFS_BUMPENVMAP: case FFS_BUMPENVMAPLUMINANCE: return 0u;
    default: return 6u;
  }
}
static int ffs_reads_texture(unsigned op, const uint8_t arg[3]) {
  const unsigned used = ffs_arguments(op);
  if (op == FFS_BLENDTEXTUREALPHA || op == FFS_BLENDTEXTUREALPHAPM) return 1;
  for (unsigned n = 0; n < 3; ++n)
    if ((used >> n & 1) && (arg[n] & FFS_SELECT) == FFS_TEXTURE) return 1;
  return 0;
}
/* Brings one half of a stage - the colour's or the alpha's - into the form the key has: an operation that is one, only the arguments it reads, and
 * with no texture set nothing that would read one - such a stage passes on what it is given, as Direct3D 9 has it. `bump`: a bump environment
 * operation may stay (the colour half of a stage with a texture and a stage after it). */
static void ffs_normalize_half(uint8_t *op, uint8_t arg[3], int textured, int bump) {
  unsigned used;
  if (*op == FFS_PREMODULATE) *op = FFS_SELECTARG1;
  if ((*op == FFS_BUMPENVMAP || *op == FFS_BUMPENVMAPLUMINANCE) && bump && textured) {
    arg[0] = arg[1] = arg[2] = 0;
    return;
  }
  if (*op <= FFS_DISABLE || *op > FFS_LERP || *op == FFS_BUMPENVMAP || *op == FFS_BUMPENVMAPLUMINANCE || (!textured && ffs_reads_texture(*op, arg))) {
    *op = FFS_SELECTARG1;
    arg[1] = FFS_CURRENT;
  }
  used = ffs_arguments(*op);
  for (unsigned n = 0; n < 3; ++n) {
    if (!(used >> n & 1)) arg[n] = 0;
    else {
      arg[n] &= FFS_SELECT | FFS_COMPLEMENT | FFS_ALPHAREPLICATE;
      if ((arg[n] & FFS_SELECT) > FFS_CONSTANT) arg[n] = (uint8_t)((arg[n] & ~FFS_SELECT) | FFS_CURRENT);
    }
  }
}
/* Makes the key of what a program set: `key` comes with the operations, arguments and `to_temp` of all the stages and with `specular`, as the
 * states have them (their low eight bits), and `textured` has bit n where stage n has a texture. The stages end at the first whose colour operation
 * is D3DTOP_DISABLE. */
static void ffs_normalize(FfsKey *key, unsigned textured) {
  unsigned count = 0;
  key->specular = key->specular ? 1 : 0;
  while (count < FFS_STAGES && key->stage[count].colour_op != FFS_DISABLE && key->stage[count].colour_op != 0 && key->stage[count].colour_op <= FFS_LERP) {
    FfsStage *s = &key->stage[count];
    const int has = (int)(textured >> count & 1);
    /* (a bump environment stage moves the next stage's coordinates: there must be one, which does something) */
    const int next = count + 1 < FFS_STAGES && key->stage[count + 1].colour_op != FFS_DISABLE && key->stage[count + 1].colour_op != 0 &&
                     key->stage[count + 1].colour_op <= FFS_LERP;
    ffs_normalize_half(&s->colour_op, s->colour_arg, has, next);
    ffs_normalize_half(&s->alpha_op, s->alpha_arg, has, 0);
    s->to_temp = s->to_temp ? 1 : 0;
    s->kill = (uint8_t)(s->kill && has);
    s->bump = (uint8_t)(s->colour_op == FFS_BUMPENVMAP ? 1 : s->colour_op == FFS_BUMPENVMAPLUMINANCE ? 2 : 0);
    s->sample = (uint8_t)(s->kill || s->bump || ffs_reads_texture(s->colour_op, s->colour_arg) || ffs_reads_texture(s->alpha_op, s->alpha_arg));
    ++count;
  }
  key->stages = (uint8_t)count;
  if (count < FFS_STAGES) memset(&key->stage[count], 0, sizeof(FfsStage) * (FFS_STAGES - count));
}

/* ---- the tokens ------------------------------------------------------------------------------------------------------------------------------------- */

/* The registers of a shader made here: r0 what goes from stage to stage, r1 the temporary register of the stages, r2 the stage's texture, r3 .. r5
 * an argument that is 1 - something, r6 the stage's result before it is clamped, r7 .. r9 what an operation needs for itself (r9 also the moved
 * coordinates of a stage after a bump environment one), r10 the move a bump environment stage worked out, r11 its luminance factor. */
enum { FFS_R_CURRENT = 0, FFS_R_TEMP, FFS_R_TEXEL, FFS_R_ARGUMENT, FFS_R_RESULT = 6, FFS_R_SCRATCH, FFS_R_BUMP = 10, FFS_R_LUMINANCE = 11 };
enum { FFS_T_TEMP = 0, FFS_T_INPUT = 1, FFS_T_CONST = 2, FFS_T_COORDINATE = 3, FFS_T_COLOUR_OUT = 8, FFS_T_SAMPLER = 10 };
enum { FFS_I_MOV = 1, FFS_I_ADD = 2, FFS_I_SUB = 3, FFS_I_MAD = 4, FFS_I_MUL = 5, FFS_I_DP3 = 8, FFS_I_LRP = 18, FFS_I_DCL = 31, FFS_I_TEXKILL = 65, FFS_I_TEXLD = 66,
       FFS_I_DEF = 81 };
#define FFS_XYZW 0xE4u
#define FFS_WWWW 0xFFu
#define FFS_HALF 0x00u                  /* c1: 0.5, 1, 4, 0 */
#define FFS_ONE 0x55u
#define FFS_FOUR 0xAAu
#define FFS_ZERO 0xFFu

typedef struct FfsWriter { uint32_t *out; size_t count, capacity; int failed; } FfsWriter;
typedef struct FfsSource { unsigned type, number, swizzle; } FfsSource;

static void ffs_put(FfsWriter *w, uint32_t token) {
  if (w->count < w->capacity) w->out[w->count++] = token;
  else w->failed = 1;
}
static uint32_t ffs_register(unsigned type, unsigned number) { return 0x80000000u | ((type & 7u) << 28) | ((type & 0x18u) << 8) | number; }
static uint32_t ffs_source_token(FfsSource s) { return ffs_register(s.type, s.number) | (s.swizzle << 16); }
static FfsSource ffs_source(unsigned type, unsigned number, unsigned swizzle) {
  FfsSource s;
  s.type = type; s.number = number; s.swizzle = swizzle;
  return s;
}
/* The fourth number of a source, in all four. */
static FfsSource ffs_alpha_of(FfsSource s) { return ffs_source(s.type, s.number, ((s.swizzle >> 6) & 3u) * 0x55u); }
static FfsSource ffs_number(unsigned swizzle) { return ffs_source(FFS_T_CONST, 1, swizzle); }
static FfsSource ffs_temp(unsigned number) { return ffs_source(FFS_T_TEMP, number, FFS_XYZW); }
/* An instruction: a destination (a temporary, or with `number` 255 the colour the shader gives) and up to three sources. */
static void ffs_instruction(FfsWriter *w, unsigned op, unsigned number, unsigned mask, int saturate, unsigned sources, FfsSource a, FfsSource b, FfsSource c) {
  ffs_put(w, op | ((1u + sources) << 24));
  ffs_put(w, (number == 255u ? ffs_register(FFS_T_COLOUR_OUT, 0) : ffs_register(FFS_T_TEMP, number)) | (mask << 16) | (saturate ? 1u << 20 : 0u));
  if (sources > 0) ffs_put(w, ffs_source_token(a));
  if (sources > 1) ffs_put(w, ffs_source_token(b));
  if (sources > 2) ffs_put(w, ffs_source_token(c));
}
/* Argument `n` of a stage as something an instruction reads; `alpha`: for the alpha's half, where it is the fourth number of what is named. */
static FfsSource ffs_argument(FfsWriter *w, unsigned stage, unsigned arg, unsigned n, int alpha) {
  FfsSource s;
  switch (arg & FFS_SELECT) {
    case FFS_DIFFUSE: s = ffs_source(FFS_T_INPUT, 0, FFS_XYZW); break;
    case FFS_TEXTURE: s = ffs_temp(FFS_R_TEXEL); break;
    case FFS_TFACTOR: s = ffs_source(FFS_T_CONST, 0, FFS_XYZW); break;
    case FFS_SPECULAR: s = ffs_source(FFS_T_INPUT, 1, FFS_XYZW); break;
    case FFS_TEMP: s = ffs_temp(FFS_R_TEMP); break;
    case FFS_CONSTANT: s = ffs_source(FFS_T_CONST, 2 + stage, FFS_XYZW); break;
    default: s = ffs_temp(FFS_R_CURRENT); break;
  }
  if (alpha || (arg & FFS_ALPHAREPLICATE)) s.swizzle = FFS_WWWW;
  if (arg & FFS_COMPLEMENT) {
    ffs_instruction(w, FFS_I_SUB, FFS_R_ARGUMENT + n, 0xF, 0, 2, ffs_number(FFS_ONE), s, s);
    s = ffs_temp(FFS_R_ARGUMENT + n);
  }
  return s;
}
/* One half of a stage: the operation on its arguments, into the components `mask` of the result register. */
static void ffs_half(FfsWriter *w, unsigned stage, unsigned op, const uint8_t arg[3], unsigned mask, int alpha) {
  const unsigned used = ffs_arguments(op), d = FFS_R_RESULT, t = FFS_R_SCRATCH;
  const FfsSource none = ffs_number(FFS_ZERO), half = ffs_number(FFS_HALF), one = ffs_number(FFS_ONE), four = ffs_number(FFS_FOUR), scratch = ffs_temp(t);
  const FfsSource a0 = (used & 1u) ? ffs_argument(w, stage, arg[0], 0, alpha) : none;
  const FfsSource a1 = (used & 2u) ? ffs_argument(w, stage, arg[1], 1, alpha) : none;
  const FfsSource a2 = (used & 4u) ? ffs_argument(w, stage, arg[2], 2, alpha) : none;
  switch (op) {
    case FFS_SELECTARG2: ffs_instruction(w, FFS_I_MOV, d, mask, 0, 1, a2, none, none); break;
    case FFS_MODULATE: ffs_instruction(w, FFS_I_MUL, d, mask, 0, 2, a1, a2, none); break;
    case FFS_MODULATE2X:
      ffs_instruction(w, FFS_I_MUL, t, 0xF, 0, 2, a1, a2, none);
      ffs_instruction(w, FFS_I_ADD, d, mask, 0, 2, scratch, scratch, none);
      break;
    case FFS_MODULATE4X:
      ffs_instruction(w, FFS_I_MUL, t, 0xF, 0, 2, a1, a2, none);
      ffs_instruction(w, FFS_I_MUL, d, mask, 0, 2, scratch, four, none);
      break;
    case FFS_ADD: ffs_instruction(w, FFS_I_ADD, d, mask, 0, 2, a1, a2, none); break;
    case FFS_ADDSIGNED:      /* a1 + a2 - 0.5 */
      ffs_instruction(w, FFS_I_ADD, t, 0xF, 0, 2, a1, a2, none);
      ffs_instruction(w, FFS_I_SUB, d, mask, 0, 2, scratch, half, none);
      break;
    case FFS_ADDSIGNED2X:
      ffs_instruction(w, FFS_I_ADD, t, 0xF, 0, 2, a1, a2, none);
      ffs_instruction(w, FFS_I_SUB, t, 0xF, 0, 2, scratch, half, none);
      ffs_instruction(w, FFS_I_ADD, d, mask, 0, 2, scratch, scratch, none);
      break;
    case FFS_SUBTRACT: ffs_instruction(w, FFS_I_SUB, d, mask, 0, 2, a1, a2, none); break;
    case FFS_ADDSMOOTH: ffs_instruction(w, FFS_I_LRP, d, mask, 0, 3, a1, one, a2); break;      /* a1 + a2 * (1 - a1) */
    /* a1 * alpha + a2 * (1 - alpha), by the alpha of something */
    case FFS_BLENDDIFFUSEALPHA: ffs_instruction(w, FFS_I_LRP, d, mask, 0, 3, ffs_source(FFS_T_INPUT, 0, FFS_WWWW), a1, a2); break;
    case FFS_BLENDTEXTUREALPHA: ffs_instruction(w, FFS_I_LRP, d, mask, 0, 3, ffs_source(FFS_T_TEMP, FFS_R_TEXEL, FFS_WWWW), a1, a2); break;
    case FFS_BLENDFACTORALPHA: ffs_instruction(w, FFS_I_LRP, d, mask, 0, 3, ffs_source(FFS_T_CONST, 0, FFS_WWWW), a1, a2); break;
    case FFS_BLENDCURRENTALPHA: ffs_instruction(w, FFS_I_LRP, d, mask, 0, 3, ffs_source(FFS_T_TEMP, FFS_R_CURRENT, FFS_WWWW), a1, a2); break;
    case FFS_BLENDTEXTUREALPHAPM:      /* a1 + a2 * (1 - the texture's alpha) */
      ffs_instruction(w, FFS_I_SUB, t, 0xF, 0, 2, one, ffs_source(FFS_T_TEMP, FFS_R_TEXEL, FFS_WWWW), none);
      ffs_instruction(w, FFS_I_MAD, d, mask, 0, 3, a2, scratch, a1);
      break;
    case FFS_MODULATEALPHA_ADDCOLOR: ffs_instruction(w, FFS_I_MAD, d, mask, 0, 3, ffs_alpha_of(a1), a2, a1); break;      /* a1 + a1's alpha * a2 */
    case FFS_MODULATECOLOR_ADDALPHA: ffs_instruction(w, FFS_I_MAD, d, mask, 0, 3, a1, a2, ffs_alpha_of(a1)); break;      /* a1 * a2 + a1's alpha */
    case FFS_MODULATEINVALPHA_ADDCOLOR:      /* (1 - a1's alpha) * a2 + a1 */
      ffs_instruction(w, FFS_I_SUB, t, 0xF, 0, 2, one, ffs_alpha_of(a1), none);
      ffs_instruction(w, FFS_I_MAD, d, mask, 0, 3, scratch, a2, a1);
      break;
    case FFS_MODULATEINVCOLOR_ADDALPHA:      /* (1 - a1) * a2 + a1's alpha */
      ffs_instruction(w, FFS_I_SUB, t, 0xF, 0, 2, one, a1, none);
      ffs_instruction(w, FFS_I_MAD, d, mask, 0, 3, scratch, a2, ffs_alpha_of(a1));
      break;
    case FFS_DOTPRODUCT3:      /* 4 * ((a1 - 0.5) . (a2 - 0.5)), in every component */
      ffs_instruction(w, FFS_I_SUB, t, 0xF, 0, 2, a1, half, none);
      ffs_instruction(w, FFS_I_SUB, t + 1, 0xF, 0, 2, a2, half, none);
      ffs_instruction(w, FFS_I_DP3, t + 2, 0xF, 0, 2, scratch, ffs_temp(t + 1), none);
      ffs_instruction(w, FFS_I_MUL, d, mask, 0, 2, ffs_temp(t + 2), four, none);
      break;
    case FFS_MULTIPLYADD: ffs_instruction(w, FFS_I_MAD, d, mask, 0, 3, a1, a2, a0); break;      /* a0 + a1 * a2 */
    case FFS_BUMPENVMAP: case FFS_BUMPENVMAPLUMINANCE: ffs_instruction(w, FFS_I_MOV, d, mask, 0, 1, ffs_temp(FFS_R_CURRENT), none, none); break;      /* (what it is given) */
    case FFS_LERP: ffs_instruction(w, FFS_I_LRP, d, mask, 0, 3, a0, a1, a2); break;             /* a0 * a1 + (1 - a0) * a2 */
    default: ffs_instruction(w, FFS_I_MOV, d, mask, 0, 1, a1, none, none); break;               /* FFS_SELECTARG1 */
  }
}
static int ffs_half_reads(unsigned op, const uint8_t arg[3], unsigned what) {
  const unsigned used = ffs_arguments(op);
  for (unsigned n = 0; n < 3; ++n)
    if ((used >> n & 1) && (arg[n] & FFS_SELECT) == what) return 1;
  return 0;
}
/* Whether a shader of the key reads the specular colour, or the temporary register. */
static int ffs_reads(const FfsKey *key, unsigned what) {
  for (unsigned n = 0; n < key->stages && n < FFS_STAGES; ++n)
    if (ffs_half_reads(key->stage[n].colour_op, key->stage[n].colour_arg, what) || ffs_half_reads(key->stage[n].alpha_op, key->stage[n].alpha_arg, what)) return 1;
  return 0;
}
/* Writes the pixel shader of a key (one ffs_normalize made) to `out`, which has room for `capacity` tokens - FFS_MAX_TOKENS are enough for any -
 * and gives the count of tokens; 0 where there was no room. */
static size_t ffs_tokens(const FfsKey *key, uint32_t *out, size_t capacity) {
  FfsWriter writer, *w = &writer;
  const unsigned stages = key->stages < FFS_STAGES ? key->stages : FFS_STAGES;
  const FfsSource none = ffs_number(FFS_ZERO);
  static const float numbers[4] = {0.5f, 1.0f, 4.0f, 0.0f};
  w->out = out; w->count = 0; w->capacity = capacity; w->failed = 0;
  ffs_put(w, 0xFFFF0200u);      /* ps_2_0 */
  ffs_put(w, FFS_I_DEF | (5u << 24));
  ffs_put(w, ffs_register(FFS_T_CONST, 1) | (0xFu << 16));
  for (unsigned n = 0; n < 4; ++n) {
    uint32_t bits;
    memcpy(&bits, &numbers[n], 4);
    ffs_put(w, bits);
  }
  ffs_put(w, FFS_I_DCL | (2u << 24)); ffs_put(w, 0x80000000u); ffs_put(w, ffs_register(FFS_T_INPUT, 0) | (0xFu << 16));
  if (key->specular || ffs_reads(key, FFS_SPECULAR)) {
    ffs_put(w, FFS_I_DCL | (2u << 24)); ffs_put(w, 0x80000000u); ffs_put(w, ffs_register(FFS_T_INPUT, 1) | (0xFu << 16));
  }
  for (unsigned n = 0; n < stages; ++n) {
    if (!key->stage[n].sample) continue;
    ffs_put(w, FFS_I_DCL | (2u << 24)); ffs_put(w, 0x80000000u); ffs_put(w, ffs_register(FFS_T_COORDINATE, n) | (0xFu << 16));
    ffs_put(w, FFS_I_DCL | (2u << 24)); ffs_put(w, 0x90000000u); ffs_put(w, ffs_register(FFS_T_SAMPLER, n) | (0xFu << 16));      /* (two-dimensional) */
  }
  ffs_instruction(w, FFS_I_MOV, FFS_R_CURRENT, 0xF, 0, 1, ffs_source(FFS_T_INPUT, 0, FFS_XYZW), none, none);
  if (ffs_reads(key, FFS_TEMP)) ffs_instruction(w, FFS_I_MOV, FFS_R_TEMP, 0xF, 0, 1, none, none, none);      /* (nothing, until a stage writes it) */
  for (unsigned n = 0; n < stages; ++n) {
    const FfsStage *s = &key->stage[n];
    const int moved = n > 0 && key->stage[n - 1].bump;
    if (s->sample) {      /* texldp: the coordinates divided by their fourth number */
      FfsSource at = ffs_source(FFS_T_COORDINATE, n, FFS_XYZW);
      if (moved) {      /* (after a bump environment stage: u, v moved by what it worked out - times the fourth number, which the lookup divides by) */
        ffs_instruction(w, FFS_I_MOV, FFS_R_SCRATCH + 2, 0xF, 0, 1, at, none, none);
        ffs_instruction(w, FFS_I_MAD, FFS_R_SCRATCH + 2, 0x3, 0, 3, ffs_temp(FFS_R_BUMP), ffs_alpha_of(at), at);
        at = ffs_temp(FFS_R_SCRATCH + 2);
      }
      ffs_put(w, FFS_I_TEXLD | (1u << 16) | (3u << 24));
      ffs_put(w, ffs_register(FFS_T_TEMP, FFS_R_TEXEL) | (0xFu << 16));
      ffs_put(w, ffs_source_token(at));
      ffs_put(w, ffs_source_token(ffs_source(FFS_T_SAMPLER, n, FFS_XYZW)));
      if (moved && key->stage[n - 1].bump == 2)      /* (and scaled by the luminance factor) */
        ffs_instruction(w, FFS_I_MUL, FFS_R_TEXEL, 0xF, 0, 2, ffs_temp(FFS_R_TEXEL), ffs_temp(FFS_R_LUMINANCE), none);
    }
    if (s->bump) {      /* the move: du * (M00, M01) + dv * (M10, M11); the luminance factor saturate(l * scale + offset) */
      ffs_instruction(w, FFS_I_MUL, FFS_R_BUMP, 0xF, 0, 2, ffs_source(FFS_T_TEMP, FFS_R_TEXEL, 0x00u), ffs_source(FFS_T_CONST, FFS_BUMP_MATRIX + n, 0x44u), none);
      ffs_instruction(w, FFS_I_MAD, FFS_R_BUMP, 0xF, 0, 3, ffs_source(FFS_T_TEMP, FFS_R_TEXEL, 0x55u), ffs_source(FFS_T_CONST, FFS_BUMP_MATRIX + n, 0xEEu), ffs_temp(FFS_R_BUMP));
      if (s->bump == 2)
        ffs_instruction(w, FFS_I_MAD, FFS_R_LUMINANCE, 0xF, 1, 3, ffs_source(FFS_T_TEMP, FFS_R_TEXEL, 0xAAu), ffs_source(FFS_T_CONST, FFS_BUMP_LUMINANCE + n, 0x00u),
                        ffs_source(FFS_T_CONST, FFS_BUMP_LUMINANCE + n, 0x55u));
    }
    if (s->kill) {      /* alpha * 256 - 0.5: below 0 for alpha 0 only (the smallest other alpha is 1/255) - then texkill */
      ffs_instruction(w, FFS_I_MUL, FFS_R_SCRATCH, 0xF, 0, 2, ffs_source(FFS_T_TEMP, FFS_R_TEXEL, FFS_WWWW), ffs_number(FFS_FOUR), none);
      for (unsigned k = 0; k < 3; ++k) ffs_instruction(w, FFS_I_MUL, FFS_R_SCRATCH, 0xF, 0, 2, ffs_temp(FFS_R_SCRATCH), ffs_number(FFS_FOUR), none);
      ffs_instruction(w, FFS_I_SUB, FFS_R_SCRATCH, 0xF, 0, 2, ffs_temp(FFS_R_SCRATCH), ffs_number(FFS_HALF), none);
      ffs_put(w, FFS_I_TEXKILL | (1u << 24));
      ffs_put(w, ffs_register(FFS_T_TEMP, FFS_R_SCRATCH) | (0xFu << 16));
    }
    ffs_half(w, n, s->colour_op, s->colour_arg, 0x7, 0);
    ffs_half(w, n, s->alpha_op, s->alpha_arg, 0x8, 1);
    ffs_instruction(w, FFS_I_MOV, s->to_temp ? FFS_R_TEMP : FFS_R_CURRENT, 0xF, 1, 1, ffs_temp(FFS_R_RESULT), none, none);      /* (a stage's result is clamped) */
  }
  if (key->specular) ffs_instruction(w, FFS_I_ADD, FFS_R_CURRENT, 0x7, 0, 2, ffs_temp(FFS_R_CURRENT), ffs_source(FFS_T_INPUT, 1, FFS_XYZW), none);
  ffs_instruction(w, FFS_I_MOV, 255u, 0xF, 0, 1, ffs_temp(FFS_R_CURRENT), none, none);
  ffs_put(w, 0x0000FFFFu);
  return w->failed ? 0 : w->count;
}

#endif
