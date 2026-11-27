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

/* Direct3D 9 shader bytecode (vs_1_1, vs_2_x, vs_3_0, ps_1_1 to ps_1_4, ps_2_x, ps_3_0) into a DXBC container of Shader Model 4, which is what the kernel's Direct3D
 * compiles most of (src/Kernel/Dxgkrnl/D3d9Dxbc.cpp). Used by d3d9.dll (docs/Direct3D9.md) and, on the host, by tests/SmToDxbcTest.cpp. Plain C that
 * is also C++; it needs <stdint.h>, <stddef.h> and memcpy / memset from whoever includes it.
 *
 * What a translated shader expects:
 *   constants   cb0, 16 bytes a register: the float constants c0 .. c<float_registers - 1>, and after them - if `bools_used` - the 16 bool
 *               constants b0 .. b15, one register each (its first word all ones for true). A vertex shader that addresses its constants through a0
 *               has SM_RELATIVE_REGISTERS float registers. Constants a shader defines itself (def, defb) are in its code.
 *   inputs      a vertex shader's v<n> is input register n, in the signature as ATTR<n>: `inputs` says what each is by its declaration. A pixel
 *               shader's inputs are the vertex shader's outputs by their semantics.
 *   between     COLOR0 / COLOR1, TEXCOORD0 .. TEXCOORD7; the fog factor is TEXCOORD8, and the other semantics of Shader Model 3 are TEXCOORD9 .. 12
 *               (NORMAL, TANGENT, BINORMAL, BLENDWEIGHT, each with index 0).
 *   state       a pixel shader ends with what Direct3D 9 does to its colour afterwards, the alpha test and (below Shader Model 3) fog, steered by
 *               SM_STATE_REGISTERS registers of cb0 after the floats and bools, from `state_register` on (SM_STATE_*): whoever draws fills them
 *               in from the render states - sm_state_none() for a draw with neither. The fog factor is the vertex shader's (TEXCOORD8) or made of
 *               the pixel's depth - z, or the w the projection gave it - which the shader reads as its position (input register 16).
 *   samplers    s<n> is texture t<n> with sampler s<n>: two-dimensional, a cube (dcl_cube: TextureCube, sampled by direction) or a volume (dcl_volume:
 *               Texture3D), as the shader declares it (`sampler_kind`). A pixel shader 1.x names no samplers: its stage n is that pair, two-dimensional.
 *
 * Pixel shaders 1.x are another instruction set over the same tokens: the texture registers t<n> are filled by instructions that sample stage n at
 * the texture coordinates n (or at something worked out of them), sources have modifiers that are arithmetic (_bias, _bx2, 1 - x, _x2, _dz, _dw),
 * destinations a factor (_x2, _x4, _d2), and the colour is what r0 holds at the end. Their r<n> are the temporaries r<n> here, their t<n> (below
 * 1.4, where they are written) the temporaries r<8 + n>.
 *
 *   bump        a pixel shader 1.x with texbem, texbeml or bem: after its fill registers (from `bump_register` on), two registers for each stage up to the
 *               highest whose bump environment matrix it uses: stage n's matrix (D3DTSS_BUMPENVMAT00, 01, 10, 11) at 2n, its luminance scale and
 *               offset (D3DTSS_BUMPENVLSCALE, BUMPENVLOFFSET) at 2n + 1 (`bump_stages` of them).
 *
 * Refused (SM_UNSUPPORTED): of pixel shaders 1.x the ones that look up a cube map
 * (texm3x3tex, texm3x3spec, texm3x3vspec) and the ones that write the depth (texdepth, texm3x2depth); loops, subroutines and predicates (loop, rep, call, label, setp, break*); integer constants;
 * samplers of a kind other than 2D, cube and volume; texldl and texldd; the depth output; vPos and vFace; textures in a vertex shader. A malformed shader - a token stream that ends early, a register that is not there - is SM_INVALID. */
#ifndef TCOS_SM_TO_DXBC_H
#define TCOS_SM_TO_DXBC_H

#define SM_OK 0
#define SM_INVALID 1
#define SM_UNSUPPORTED 2

#define SM_CODE_WORDS 12288u            /* the Shader Model 4 tokens of one shader, at most */
#define SM_DXBC_BYTES (SM_CODE_WORDS * 4u + 2048u)
#define SM_MAX_TOKENS 16384u            /* the tokens of a Direct3D 9 shader that are looked at, at most */
#define SM_RELATIVE_REGISTERS 224u
#define SM_BOOL_REGISTERS 16u

typedef struct SmInput { uint8_t usage, index, reg; } SmInput;

typedef struct SmShader {
  int vertex;
  unsigned major, minor;
  unsigned float_registers;             /* c0 .. c<n-1> are read from cb0 */
  int relative;                         /* ... through a0 as well */
  int bools_used;                       /* b0 .. b15 follow the floats */
  unsigned constant_registers;          /* all of cb0: the floats, the bools if used, and a pixel shader's state registers */
  unsigned state_register;              /* the first of its state registers: a pixel shader's SM_STATE_REGISTERS, a vertex shader's SM_VS_STATE_REGISTERS */
  unsigned fill_register;               /* a pixel shader: the first of its fill registers after the state ones, one for each sampler up to the highest it reads */
  unsigned bump_register;               /* a pixel shader 1.x: the first of its bump registers after the fill ones (see above), `bump_stages` stages of two */
  unsigned bump_stages;
  unsigned sampler_mask;
  uint8_t sampler_kind[16];             /* what each sampler of sampler_mask is: SM_SAMPLER_2D, SM_SAMPLER_VOLUME or SM_SAMPLER_CUBE (the kernel's TextureKind) */
  SmInput inputs[16];                   /* a vertex shader's declared inputs */
  unsigned input_count;
  unsigned varying_mask;                /* the slots (SM_SLOT_*) a vertex shader writes or a pixel shader reads */
  unsigned target_mask;                 /* a pixel shader: the render targets it writes (oC0 .. oC3; oC0 always) */
  size_t tokens;                        /* the tokens of the shader, its end included */
  uint8_t dxbc[SM_DXBC_BYTES];
  size_t dxbc_size;
} SmShader;

/* The kinds of texture a sampler is (SmShader.sampler_kind): the numbers of the kernel's TextureKind. */
#define SM_SAMPLER_2D 0u
#define SM_SAMPLER_VOLUME 1u
#define SM_SAMPLER_CUBE 3u

/* The slots of what goes from the vertex to the pixel shader: 0 the position, 1 and 2 the colours, 3 + n TEXCOORD<n>. */
#define SM_SLOT_POSITION 0
#define SM_SLOT_COLOR 1
#define SM_SLOT_TEXCOORD 3
#define SM_SLOTS 16

/* The state registers of a pixel shader, from `state_register` on. */
#define SM_STATE_REGISTERS 5u
#define SM_STATE_ALPHA 0u               /* x: the reference of the alpha test, 0 .. 255; y, z: the depth fog is made of is y times the pixel's z in the
                                           target plus z times the w it had before the projection divided */
#define SM_STATE_ALPHA_KEEP 1u          /* x, y, z: 1 where an alpha below, equal to, above the reference keeps the pixel, else 0 */
#define SM_STATE_FOG_COLOUR 2u          /* x, y, z: the fog's colour */
#define SM_STATE_FOG 3u                 /* x, y: the linear factor is depth * x + y; z, w: the exponential one is 2 ^ -(depth * (z + w * depth)) */
#define SM_STATE_FOG_SOURCE 4u          /* the factor is x * the vertex shader's + y * the linear + z * the exponential + w */

/* The state registers of a draw with no alpha test and no fog. */
/* The state register of a vertex shader, after its floats and bools: x, y what moves its position by half a pixel (times its w), so that the rasterizer's
 * pixel centres, at halves, are where Direct3D 9 has them, at whole numbers: 1 / the viewport's width and -1 / its height. 0, 0 leaves the position as it is. */
#define SM_VS_STATE_REGISTERS 1u
static void sm_state_none(float state[SM_STATE_REGISTERS][4]) {
  memset(state, 0, sizeof(float) * 4 * SM_STATE_REGISTERS);
  state[SM_STATE_ALPHA][1] = 1.0f;
  state[SM_STATE_ALPHA_KEEP][0] = state[SM_STATE_ALPHA_KEEP][1] = state[SM_STATE_ALPHA_KEEP][2] = 1.0f;
  state[SM_STATE_FOG_SOURCE][3] = 1.0f;
}

/* ---- the Direct3D 9 side ---------------------------------------------------------------------------------------------------------------------------- */

enum { SMR_TEMP = 0, SMR_INPUT, SMR_CONST, SMR_ADDR, SMR_RASTOUT, SMR_ATTROUT, SMR_OUTPUT, SMR_CONSTINT, SMR_COLOROUT, SMR_DEPTHOUT, SMR_SAMPLER, SMR_CONST2, SMR_CONST3,
       SMR_CONST4, SMR_CONSTBOOL, SMR_LOOP, SMR_TEMPFLOAT16, SMR_MISC, SMR_LABEL, SMR_PREDICATE };
enum { SMO_NOP = 0, SMO_MOV, SMO_ADD, SMO_SUB, SMO_MAD, SMO_MUL, SMO_RCP, SMO_RSQ, SMO_DP3, SMO_DP4, SMO_MIN, SMO_MAX, SMO_SLT, SMO_SGE, SMO_EXP, SMO_LOG, SMO_LIT, SMO_DST, SMO_LRP,
       SMO_FRC, SMO_M4X4, SMO_M4X3, SMO_M3X4, SMO_M3X3, SMO_M3X2, SMO_CALL, SMO_CALLNZ, SMO_LOOP, SMO_RET, SMO_ENDLOOP, SMO_LABEL, SMO_DCL, SMO_POW, SMO_CRS, SMO_SGN, SMO_ABS,
       SMO_NRM, SMO_SINCOS, SMO_REP, SMO_ENDREP, SMO_IF, SMO_IFC, SMO_ELSE, SMO_ENDIF, SMO_BREAK, SMO_BREAKC, SMO_MOVA, SMO_DEFB, SMO_DEFI, SMO_TEXCOORD = 64, SMO_TEXKILL = 65, SMO_TEXLD = 66,
       SMO_TEXBEM = 67, SMO_TEXBEML = 68, SMO_TEXREG2AR = 69, SMO_TEXREG2GB = 70, SMO_TEXM3X2PAD = 71, SMO_TEXM3X2TEX = 72, SMO_TEXM3X3PAD = 73, SMO_TEXM3X3TEX = 74,
       SMO_TEXM3X3SPEC = 76, SMO_TEXM3X3VSPEC = 77, SMO_TEXREG2RGB = 82, SMO_TEXDP3TEX = 83, SMO_TEXM3X2DEPTH = 84, SMO_TEXDP3 = 85, SMO_TEXM3X3 = 86, SMO_TEXDEPTH = 87,
       SMO_BEM = 89, SMO_PHASE = 0xFFFD,
       SMO_EXPP = 78, SMO_LOGP = 79, SMO_CND = 80, SMO_DEF = 81, SMO_CMP = 88, SMO_DP2ADD = 90, SMO_DSX = 91, SMO_DSY = 92, SMO_TEXLDD = 93, SMO_SETP = 94, SMO_TEXLDL = 95,
       SMO_BREAKP = 96, SMO_COMMENT = 0xFFFE, SMO_END = 0xFFFF };

typedef struct SmParam {
  unsigned type, number;
  unsigned mask;                        /* a destination's write mask */
  unsigned swizzle;                     /* a source's, two bits a component */
  unsigned modifier;                    /* a source's: 0 none, 1 negate, 11 abs, 12 negated abs; 2 .. 10 are those of pixel shaders 1.x */
  int saturate;                         /* a destination's */
  unsigned shift;                       /* a destination's in a pixel shader 1.x: 1, 2, 3 times 2, 4, 8; 15, 14, 13 divided by 2, 4, 8 */
  int relative;                         /* a constant addressed through a0 */
  unsigned relative_component;
} SmParam;

/* ---- the Shader Model 4 side ------------------------------------------------------------------------------------------------------------------------- */

enum { DXO_ADD = 0, DXO_AND = 1, DXO_DERIV_RTX = 11, DXO_DERIV_RTY = 12, DXO_DISCARD = 13, DXO_DIV = 14, DXO_DP2 = 15, DXO_DP3 = 16, DXO_DP4 = 17, DXO_ELSE = 18, DXO_ENDIF = 21,
       DXO_EQ = 24, DXO_EXP = 25, DXO_FRC = 26, DXO_FTOI = 27, DXO_GE = 29, DXO_IF = 31, DXO_LOG = 47, DXO_LT = 49, DXO_MAD = 50, DXO_MIN = 51, DXO_MAX = 52, DXO_MOV = 54,
       DXO_MOVC = 55, DXO_MUL = 56, DXO_NE = 57, DXO_OR = 60, DXO_RET = 62, DXO_ROUND_NE = 64, DXO_ROUND_NI = 65, DXO_RSQ = 68, DXO_SAMPLE = 69, DXO_SAMPLE_B = 74, DXO_SINCOS = 77,
       DXO_DCL_RESOURCE = 88, DXO_DCL_CONSTANT_BUFFER = 89, DXO_DCL_TEMPS = 104 };
enum { DXT_TEMP = 0, DXT_INPUT = 1, DXT_OUTPUT = 2, DXT_IMMEDIATE = 4, DXT_SAMPLER = 6, DXT_RESOURCE = 7, DXT_CONSTANT_BUFFER = 8 };
#define DX_SATURATE (1u << 13)
#define DX_NONZERO (1u << 18)
#define DX_SWIZZLE_XYZW 0xE4u
#define DX_ONE_BITS 0x3F800000u
#define DX_TEMP_ADDRESS 32u             /* the integer copy of a0 */
#define DX_TEMP_SCRATCH 33u             /* 33 .. 37: what a translated instruction needs for itself */
#define DX_TEMP_COLOUR 38u              /* a pixel shader's colour (oC0) until its end */
#define DX_TEMP_POSITION 39u            /* a vertex shader's position (oPos) until its end */
#define DX_TEMP_FILL 40u                /* 40, 41: a texture's sample, until the channels its format has not are made 1 (sm_sample_fill) */
#define DX_TEMPS 42u
#define DX_INPUT_POSITION 16u           /* a pixel shader's position, declared as such */
#define DX_TEMP_PS1_TEXTURE 8u          /* a pixel shader 1.1 to 1.3: 8 .. 13 are its t0 .. t5 */
#define DX_TEMP_PS1_SOURCE 16u          /* ... 16 .. 19: a source with one of its modifiers, worked out */
#define DX_TEMP_PS1_ROWS 20u            /* ... the rows of a matrix that texm3x2pad and texm3x3pad leave for the instruction that ends it */

typedef struct DxOperand { uint32_t w[8]; unsigned n; } DxOperand;

typedef struct SmContext {
  SmShader *out;
  uint32_t code[SM_CODE_WORDS];
  unsigned words;
  int failed;                           /* the code did not fit */
  /* the shader's own constants */
  float def_value[256][4];
  uint8_t def_set[256];
  uint8_t defb_set[16], defb_value[16];
  /* the pixel shader's declared inputs (Shader Model 3) and the vertex shader's declared outputs: register to slot, 255 not declared */
  uint8_t slot_of_register[16];
  uint8_t sampler_declared[16];
  /* where the code reads a bool constant of the device's: the word that holds its element of cb0, which is known when the floats are counted */
  uint16_t bool_word[64];
  unsigned bool_words;
  uint16_t fill_word[256];              /* the words that name a sampler's fill register by the sampler's number (sm_translate adds where they begin) */
  unsigned fill_words;
  uint16_t bump_word[64];               /* ... and those that name a bump register by its number from the first (2 * stage, + 1 for the luminance) */
  unsigned bump_words;
  unsigned pad_rows;                    /* a pixel shader 1.x: the rows texm3x2pad / texm3x3pad have left so far */
} SmContext;
static void sm_sample_fill(SmContext *c, DxOperand d, unsigned saturate, unsigned n);

static void sm_emit(SmContext *c, uint32_t opcode, uint32_t bits, const DxOperand *operands, unsigned count) {
  unsigned words = 1;
  for (unsigned i = 0; i < count; ++i) words += operands[i].n;
  if (words > 31 || c->words + words > SM_CODE_WORDS) { c->failed = 1; return; }
  c->code[c->words++] = opcode | bits | (words << 24);
  for (unsigned i = 0; i < count; ++i)
    for (unsigned k = 0; k < operands[i].n; ++k) c->code[c->words++] = operands[i].w[k];
}
static void sm_emit1(SmContext *c, uint32_t op, uint32_t bits, DxOperand a) { sm_emit(c, op, bits, &a, 1); }
static void sm_emit2(SmContext *c, uint32_t op, uint32_t bits, DxOperand a, DxOperand b) { DxOperand o[2]; o[0] = a; o[1] = b; sm_emit(c, op, bits, o, 2); }
static void sm_emit3(SmContext *c, uint32_t op, uint32_t bits, DxOperand a, DxOperand b, DxOperand d) { DxOperand o[3]; o[0] = a; o[1] = b; o[2] = d; sm_emit(c, op, bits, o, 3); }
static void sm_emit4(SmContext *c, uint32_t op, uint32_t bits, DxOperand a, DxOperand b, DxOperand d, DxOperand e) {
  DxOperand o[4]; o[0] = a; o[1] = b; o[2] = d; o[3] = e; sm_emit(c, op, bits, o, 4);
}

/* A destination: a register and a write mask. */
static DxOperand dx_dst(unsigned type, unsigned index, unsigned mask) {
  DxOperand o;
  o.w[0] = 2u | ((mask & 0xF) << 4) | (type << 12) | (1u << 20);
  o.w[1] = index;
  o.n = 2;
  return o;
}
/* A source: a register, a swizzle (two bits a component) and a modifier (0 none, 1 negate, 2 abs, 3 negated abs). */
static DxOperand dx_src(unsigned type, unsigned index, unsigned swizzle, unsigned modifier) {
  DxOperand o;
  o.n = 0;
  o.w[o.n++] = 2u | (1u << 2) | ((swizzle & 0xFF) << 4) | (type << 12) | (1u << 20) | (modifier ? 1u << 31 : 0u);
  if (modifier) o.w[o.n++] = 1u | (modifier << 6);
  o.w[o.n++] = index;
  return o;
}
static DxOperand dx_temp(unsigned index, unsigned swizzle) { return dx_src(DXT_TEMP, index, swizzle, 0); }
/* One component of a register (what if and discard test). */
static DxOperand dx_scalar(unsigned type, unsigned index, unsigned component) {
  DxOperand o;
  o.w[0] = 2u | (2u << 2) | ((component & 3) << 4) | (type << 12) | (1u << 20);
  o.w[1] = index;
  o.n = 2;
  return o;
}
/* cb0[element], or cb0[element + r<temp>.<component>]. */
static DxOperand dx_constant(unsigned element, unsigned swizzle, unsigned modifier, int relative, unsigned component) {
  DxOperand o;
  o.n = 0;
  o.w[o.n++] = 2u | (1u << 2) | ((swizzle & 0xFF) << 4) | ((unsigned)DXT_CONSTANT_BUFFER << 12) | (2u << 20) | (relative ? 3u << 25 : 0u) | (modifier ? 1u << 31 : 0u);
  if (modifier) o.w[o.n++] = 1u | (modifier << 6);
  o.w[o.n++] = 0;
  o.w[o.n++] = element;
  if (relative) {
    o.w[o.n++] = 2u | (2u << 2) | ((component & 3) << 4) | (1u << 20);
    o.w[o.n++] = DX_TEMP_ADDRESS;
  }
  return o;
}
static uint32_t sm_float_bits(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
static DxOperand dx_immediate(float x, float y, float z, float w) {
  DxOperand o;
  o.w[0] = 0x4002u;
  o.w[1] = sm_float_bits(x); o.w[2] = sm_float_bits(y); o.w[3] = sm_float_bits(z); o.w[4] = sm_float_bits(w);
  o.n = 5;
  return o;
}
static DxOperand dx_immediate_bits(uint32_t bits) {
  DxOperand o;
  o.w[0] = 0x4002u;
  o.w[1] = o.w[2] = o.w[3] = o.w[4] = bits;
  o.n = 5;
  return o;
}

/* ---- registers ---------------------------------------------------------------------------------------------------------------------------------------- */

/* The slot of a semantic of a declaration (D3DDECLUSAGE), or 255: not carried. PSIZE (4) is 254: taken and dropped. */
static unsigned sm_slot_of_usage(unsigned usage, unsigned index) {
  switch (usage) {
    case 0: return index == 0 ? (unsigned)SM_SLOT_POSITION : 255u;
    case 10: return index < 2 ? SM_SLOT_COLOR + index : 255u;
    case 5: return index < 8 ? SM_SLOT_TEXCOORD + index : 255u;
    case 11: return index == 0 ? SM_SLOT_TEXCOORD + 8u : 255u;      /* fog */
    case 3: return index == 0 ? SM_SLOT_TEXCOORD + 9u : 255u;       /* normal */
    case 6: return index == 0 ? SM_SLOT_TEXCOORD + 10u : 255u;      /* tangent */
    case 7: return index == 0 ? SM_SLOT_TEXCOORD + 11u : 255u;      /* binormal */
    case 1: return index == 0 ? SM_SLOT_TEXCOORD + 12u : 255u;      /* blend weight */
    case 4: return 254u;
    default: return 255u;
  }
}

/* The Shader Model 4 modifier of a source's: 0 none, 1 negate, 2 abs, 3 negated abs; 255 for one that is not taken. */
static unsigned sm_modifier(unsigned modifier) { return modifier == 0 ? 0u : modifier == 1 ? 1u : modifier == 11 ? 2u : modifier == 12 ? 3u : 255u; }

/* The float constants a shader of its version has (c0 .. c<limit - 1>): a vertex shader 256 (D3DCAPS9.MaxVertexShaderConst), a pixel shader 1.x 8, 2.x 32, 3.0 224;
 * and whether it has bool and integer constants (all but pixel shaders 1.x and 2.0). A shader that names more cannot be made, as Direct3D 9 refuses it. */
static unsigned sm_float_limit(const SmShader *s) { return s->vertex ? 256u : s->major == 1 ? 8u : s->major == 2 ? 32u : 224u; }
static int sm_has_bools_ints(const SmShader *s) { return s->vertex || s->major == 3 || (s->major == 2 && s->minor > 0); }

/* A source parameter as an operand; SM_OK, or why not. `negate`: with its sign turned (sub). */
static int sm_source(SmContext *c, const SmParam *p, int negate, DxOperand *o) {
  SmShader *s = c->out;
  unsigned modifier = sm_modifier(p->modifier);
  if (modifier == 255) return SM_UNSUPPORTED;
  if (negate) modifier ^= 1u;
  switch (p->type) {
    case SMR_TEMP:
      if (p->number >= 32) return SM_INVALID;
      *o = dx_src(DXT_TEMP, p->number, p->swizzle, modifier);
      return SM_OK;
    case SMR_INPUT: {
      if (p->number >= 16) return SM_INVALID;
      unsigned reg = p->number;
      if (!s->vertex) {      /* v0 / v1 are the colours below Shader Model 3; a declared input from then on */
        const unsigned slot = s->major >= 3 ? c->slot_of_register[p->number] : SM_SLOT_COLOR + p->number;
        if (slot >= SM_SLOTS || (s->major < 3 && p->number >= 2)) return s->major >= 3 ? SM_UNSUPPORTED : SM_INVALID;
        s->varying_mask |= 1u << slot;
        reg = slot;
      }
      *o = dx_src(DXT_INPUT, reg, p->swizzle, modifier);
      return SM_OK;
    }
    case SMR_ADDR:      /* a pixel shader's t<n> (the same number as a vertex shader's a0, which is no source) */
      if (s->vertex || s->major >= 3 || p->number >= 8) return SM_INVALID;
      if (s->major == 1 && s->minor < 4) {      /* (what an instruction put there, not the coordinates) */
        if (p->number >= 6) return SM_INVALID;
        *o = dx_src(DXT_TEMP, DX_TEMP_PS1_TEXTURE + p->number, p->swizzle, modifier);
        return SM_OK;
      }
      s->varying_mask |= 1u << (SM_SLOT_TEXCOORD + p->number);
      *o = dx_src(DXT_INPUT, SM_SLOT_TEXCOORD + p->number, p->swizzle, modifier);
      return SM_OK;
    case SMR_CONST: {
      if (p->number >= sm_float_limit(s)) return SM_INVALID;
      if (p->relative) {
        if (!s->vertex || p->number >= SM_RELATIVE_REGISTERS) return SM_UNSUPPORTED;
        s->relative = 1;
        *o = dx_constant(p->number, p->swizzle, modifier, 1, p->relative_component);
        return SM_OK;
      }
      if (c->def_set[p->number]) {      /* a constant of the shader's own: the values, swizzled and with the modifier */
        float v[4];
        for (unsigned i = 0; i < 4; ++i) {
          float f = c->def_value[p->number][(p->swizzle >> (2 * i)) & 3];
          if (modifier & 2) f = f < 0 ? -f : f;
          if (modifier & 1) f = -f;
          v[i] = f;
        }
        *o = dx_immediate(v[0], v[1], v[2], v[3]);
        return SM_OK;
      }
      if (p->number + 1 > s->float_registers) s->float_registers = p->number + 1;
      *o = dx_constant(p->number, p->swizzle, modifier, 0, 0);
      return SM_OK;
    }
    default:
      return SM_UNSUPPORTED;      /* integer constants, vPos and vFace, the loop counter, predicates */
  }
}

/* A destination parameter as an operand of the mask it has. Position, colours and texture coordinates are output registers by their slot; a0 is an
 * integer temporary (the instruction that writes it converts); the point size goes nowhere. */
static int sm_destination(SmContext *c, const SmParam *p, DxOperand *o) {
  SmShader *s = c->out;
  unsigned slot = 255;
  switch (p->type) {
    case SMR_TEMP:
      if (p->number >= 32) return SM_INVALID;
      *o = dx_dst(DXT_TEMP, p->number, p->mask);
      return SM_OK;
    case SMR_ADDR:
      if (!s->vertex && s->major == 1 && s->minor < 4 && p->number < 6) {
        *o = dx_dst(DXT_TEMP, DX_TEMP_PS1_TEXTURE + p->number, p->mask);
        return SM_OK;
      }
      if (!s->vertex || p->number != 0) return SM_INVALID;
      *o = dx_dst(DXT_TEMP, DX_TEMP_ADDRESS, p->mask);
      return SM_OK;
    case SMR_RASTOUT:
      if (!s->vertex || p->number > 2) return SM_INVALID;
      slot = p->number == 0 ? (unsigned)SM_SLOT_POSITION : p->number == 1 ? SM_SLOT_TEXCOORD + 8u : 254u;
      break;
    case SMR_ATTROUT:
      if (!s->vertex || p->number >= 2) return SM_INVALID;
      slot = SM_SLOT_COLOR + p->number;
      break;
    case SMR_OUTPUT:      /* oT<n> below Shader Model 3, a declared output from then on */
      if (!s->vertex || p->number >= (s->major >= 3 ? 12u : 8u)) return SM_INVALID;
      slot = s->major >= 3 ? c->slot_of_register[p->number] : SM_SLOT_TEXCOORD + p->number;
      if (slot == 255) return SM_UNSUPPORTED;
      break;
    case SMR_COLOROUT:
      if (s->vertex || p->number >= 4 || (p->number && s->major < 2)) return SM_INVALID;
      if (p->number) {      /* oC1 .. oC3: the other render targets, written as they are (the alpha test and fog are oC0's) */
        s->target_mask |= 1u << p->number;
        *o = dx_dst(DXT_OUTPUT, p->number, p->mask);
        return SM_OK;
      }
      *o = dx_dst(DXT_TEMP, DX_TEMP_COLOUR, p->mask);      /* (the end of the shader does the alpha test and fog, and writes it out) */
      return SM_OK;
    default:
      return SM_UNSUPPORTED;
  }
  if (slot == 254) {
    *o = dx_dst(DXT_TEMP, DX_TEMP_SCRATCH + 4, p->mask);
    return SM_OK;
  }
  if (slot >= SM_SLOTS) return SM_UNSUPPORTED;
  s->varying_mask |= 1u << slot;
  if (slot == SM_SLOT_POSITION) {      /* (the end of the shader moves it by half a pixel, and writes it out) */
    *o = dx_dst(DXT_TEMP, DX_TEMP_POSITION, p->mask);
    return SM_OK;
  }
  *o = dx_dst(DXT_OUTPUT, slot, p->mask);
  return SM_OK;
}

/* ---- instructions --------------------------------------------------------------------------------------------------------------------------------------- */

#define SM_TRY(x) do { const int sm_status_ = (x); if (sm_status_ != SM_OK) return sm_status_; } while (0)
#define SM_SWIZZLE_OF(i) ((i) | (i) << 2 | (i) << 4 | (i) << 6)

/* A swizzle of a swizzled source: component i of the result is component pattern[i] of what `swizzle` gives. */
static unsigned sm_compose(unsigned swizzle, unsigned pattern) {
  unsigned out = 0;
  for (unsigned i = 0; i < 4; ++i) out |= ((swizzle >> (2 * ((pattern >> (2 * i)) & 3))) & 3) << (2 * i);
  return out;
}
static DxOperand sm_scratch(unsigned n, unsigned mask) { return dx_dst(DXT_TEMP, DX_TEMP_SCRATCH + n, mask); }
static DxOperand sm_scratch_source(unsigned n) { return dx_temp(DX_TEMP_SCRATCH + n, DX_SWIZZLE_XYZW); }

/* One instruction: `op` with its parameters p[0 .. count - 1] (the destination first, where there is one). */
static int sm_instruction(SmContext *c, unsigned op, unsigned flags, const SmParam *p, unsigned count) {
  SmShader *s = c->out;
  DxOperand d, a, b, e;
  const uint32_t saturate = count && p[0].saturate ? DX_SATURATE : 0u;
  static const unsigned sources_of[] = {0, 1, 2, 2, 3, 2, 1, 1, 2, 2, 2, 2, 2, 2, 1, 1, 1, 2, 3, 1};      /* nop .. frc */
  if (op <= SMO_FRC && count != sources_of[op] + (op == SMO_NOP ? 0u : 1u)) return SM_INVALID;
  switch (op) {
    case SMO_NOP: return SM_OK;
    case SMO_MOV: case SMO_MOVA:
      SM_TRY(sm_destination(c, &p[0], &d));
      SM_TRY(sm_source(c, &p[1], 0, &a));
      if (p[0].type == SMR_ADDR && s->vertex) {      /* a0: the nearest whole number (mova), or the one below (a mov of Shader Model 1) */
        sm_emit2(c, op == SMO_MOVA ? (unsigned)DXO_ROUND_NE : (unsigned)DXO_ROUND_NI, 0, sm_scratch(0, 0xF), a);
        sm_emit2(c, DXO_FTOI, 0, d, sm_scratch_source(0));
      } else {
        if (count != 2) return SM_INVALID;
        sm_emit2(c, DXO_MOV, saturate, d, a);
      }
      return SM_OK;
    case SMO_ADD: case SMO_SUB: case SMO_MUL: case SMO_MIN: case SMO_MAX: case SMO_DP3: case SMO_DP4:
      SM_TRY(sm_destination(c, &p[0], &d));
      SM_TRY(sm_source(c, &p[1], 0, &a));
      SM_TRY(sm_source(c, &p[2], op == SMO_SUB, &b));
      sm_emit3(c, op == SMO_MUL ? (unsigned)DXO_MUL : op == SMO_MIN ? (unsigned)DXO_MIN : op == SMO_MAX ? (unsigned)DXO_MAX : op == SMO_DP3 ? (unsigned)DXO_DP3
                  : op == SMO_DP4 ? (unsigned)DXO_DP4 : (unsigned)DXO_ADD, saturate, d, a, b);
      return SM_OK;
    case SMO_MAD:
      SM_TRY(sm_destination(c, &p[0], &d));
      SM_TRY(sm_source(c, &p[1], 0, &a));
      SM_TRY(sm_source(c, &p[2], 0, &b));
      SM_TRY(sm_source(c, &p[3], 0, &e));
      sm_emit4(c, DXO_MAD, saturate, d, a, b, e);
      return SM_OK;
    case SMO_RCP:
      SM_TRY(sm_destination(c, &p[0], &d));
      SM_TRY(sm_source(c, &p[1], 0, &a));
      sm_emit3(c, DXO_DIV, saturate, d, dx_immediate(1, 1, 1, 1), a);
      return SM_OK;
    case SMO_RSQ: case SMO_LOG: case SMO_LOGP: {      /* (of the absolute value, as Direct3D 9 has them) */
      SmParam source;
      if (count != 2) return SM_INVALID;
      source = p[1];
      if (source.modifier == 0 || source.modifier == 1) source.modifier = 11;
      SM_TRY(sm_destination(c, &p[0], &d));
      SM_TRY(sm_source(c, &source, 0, &a));
      sm_emit2(c, op == SMO_RSQ ? (unsigned)DXO_RSQ : (unsigned)DXO_LOG, saturate, d, a);
      return SM_OK;
    }
    case SMO_EXP: case SMO_EXPP: case SMO_FRC: case SMO_ABS: case SMO_DSX: case SMO_DSY: {
      SmParam source;
      if (count != 2) return SM_INVALID;
      if ((op == SMO_DSX || op == SMO_DSY) && s->vertex) return SM_INVALID;
      source = p[1];
      if (op == SMO_ABS) source.modifier = 11;
      SM_TRY(sm_destination(c, &p[0], &d));
      SM_TRY(sm_source(c, &source, 0, &a));
      sm_emit2(c, op == SMO_FRC ? (unsigned)DXO_FRC : op == SMO_ABS ? (unsigned)DXO_MOV : op == SMO_DSX ? (unsigned)DXO_DERIV_RTX : op == SMO_DSY ? (unsigned)DXO_DERIV_RTY
                  : (unsigned)DXO_EXP, saturate, d, a);
      return SM_OK;
    }
    case SMO_SLT: case SMO_SGE:      /* 1 or 0 */
      SM_TRY(sm_destination(c, &p[0], &d));
      SM_TRY(sm_source(c, &p[1], 0, &a));
      SM_TRY(sm_source(c, &p[2], 0, &b));
      sm_emit3(c, op == SMO_SLT ? (unsigned)DXO_LT : (unsigned)DXO_GE, 0, sm_scratch(0, 0xF), a, b);
      sm_emit3(c, DXO_AND, 0, d, sm_scratch_source(0), dx_immediate_bits(DX_ONE_BITS));
      return SM_OK;
    case SMO_POW:      /* 2 ^ (b * log2 |a|) */
      if (count != 3) return SM_INVALID;
      {
        SmParam base = p[1];
        if (base.modifier == 0 || base.modifier == 1) base.modifier = 11;
        SM_TRY(sm_destination(c, &p[0], &d));
        SM_TRY(sm_source(c, &base, 0, &a));
        SM_TRY(sm_source(c, &p[2], 0, &b));
      }
      sm_emit2(c, DXO_LOG, 0, sm_scratch(0, 0xF), a);
      sm_emit3(c, DXO_MUL, 0, sm_scratch(0, 0xF), sm_scratch_source(0), b);
      sm_emit2(c, DXO_EXP, saturate, d, sm_scratch_source(0));
      return SM_OK;
    case SMO_LRP:      /* a * (b - e) + e */
      SM_TRY(sm_destination(c, &p[0], &d));
      SM_TRY(sm_source(c, &p[1], 0, &a));
      SM_TRY(sm_source(c, &p[2], 0, &b));
      SM_TRY(sm_source(c, &p[3], 1, &e));
      sm_emit3(c, DXO_ADD, 0, sm_scratch(0, 0xF), b, e);
      SM_TRY(sm_source(c, &p[3], 0, &e));
      sm_emit4(c, DXO_MAD, saturate, d, a, sm_scratch_source(0), e);
      return SM_OK;
    case SMO_DST: {      /* 1, a.y * b.y, a.z, b.w */
      SM_TRY(sm_destination(c, &p[0], &d));
      SM_TRY(sm_source(c, &p[1], 0, &a));
      SM_TRY(sm_source(c, &p[2], 0, &b));
      sm_emit3(c, DXO_MUL, 0, sm_scratch(0, 0x2), a, b);
      sm_emit2(c, DXO_MOV, 0, sm_scratch(0, 0x4), a);
      sm_emit2(c, DXO_MOV, 0, sm_scratch(0, 0x8), b);
      sm_emit2(c, DXO_MOV, 0, sm_scratch(0, 0x1), dx_immediate(1, 1, 1, 1));
      sm_emit2(c, DXO_MOV, saturate, d, sm_scratch_source(0));
      return SM_OK;
    }
    case SMO_LIT: {      /* 1, max(a.x, 0), a.x > 0 and a.y > 0 ? a.y ^ a.w : 0, 1 */
      SM_TRY(sm_destination(c, &p[0], &d));
      SM_TRY(sm_source(c, &p[1], 0, &a));
      sm_emit2(c, DXO_MOV, 0, sm_scratch(1, 0xF), a);
      sm_emit3(c, DXO_MAX, 0, sm_scratch(0, 0x2), dx_temp(DX_TEMP_SCRATCH + 1, 0), dx_immediate(0, 0, 0, 0));
      /* the power: the exponent within +-127.9961, the base at least a tiny positive number */
      sm_emit3(c, DXO_MAX, 0, sm_scratch(2, 0x1), dx_temp(DX_TEMP_SCRATCH + 1, SM_SWIZZLE_OF(3)), dx_immediate(-127.9961f, 0, 0, 0));
      sm_emit3(c, DXO_MIN, 0, sm_scratch(2, 0x1), dx_temp(DX_TEMP_SCRATCH + 2, 0), dx_immediate(127.9961f, 0, 0, 0));
      sm_emit3(c, DXO_MAX, 0, sm_scratch(2, 0x2), dx_temp(DX_TEMP_SCRATCH + 1, SM_SWIZZLE_OF(1)), dx_immediate(1e-30f, 1e-30f, 1e-30f, 1e-30f));
      sm_emit2(c, DXO_LOG, 0, sm_scratch(2, 0x2), dx_temp(DX_TEMP_SCRATCH + 2, SM_SWIZZLE_OF(1)));
      sm_emit3(c, DXO_MUL, 0, sm_scratch(2, 0x2), dx_temp(DX_TEMP_SCRATCH + 2, SM_SWIZZLE_OF(1)), dx_temp(DX_TEMP_SCRATCH + 2, 0));
      sm_emit2(c, DXO_EXP, 0, sm_scratch(2, 0x2), dx_temp(DX_TEMP_SCRATCH + 2, SM_SWIZZLE_OF(1)));
      /* ... kept where both a.x and a.y are above 0 */
      sm_emit3(c, DXO_LT, 0, sm_scratch(3, 0x3), dx_immediate(0, 0, 0, 0), sm_scratch_source(1));
      sm_emit3(c, DXO_AND, 0, sm_scratch(3, 0x1), dx_temp(DX_TEMP_SCRATCH + 3, 0), dx_temp(DX_TEMP_SCRATCH + 3, SM_SWIZZLE_OF(1)));
      sm_emit4(c, DXO_MOVC, 0, sm_scratch(0, 0x4), dx_temp(DX_TEMP_SCRATCH + 3, 0), dx_temp(DX_TEMP_SCRATCH + 2, SM_SWIZZLE_OF(1)), dx_immediate(0, 0, 0, 0));
      sm_emit2(c, DXO_MOV, 0, sm_scratch(0, 0x9), dx_immediate(1, 1, 1, 1));
      sm_emit2(c, DXO_MOV, saturate, d, sm_scratch_source(0));
      return SM_OK;
    }
    case SMO_M4X4: case SMO_M4X3: case SMO_M3X4: case SMO_M3X3: case SMO_M3X2: {      /* a dot product a row of the matrix, which is rows of constants */
      const unsigned rows = op == SMO_M4X4 || op == SMO_M3X4 ? 4u : op == SMO_M3X2 ? 2u : 3u;
      const unsigned dot = op == SMO_M4X4 || op == SMO_M4X3 ? (unsigned)DXO_DP4 : (unsigned)DXO_DP3;
      if (count != 3) return SM_INVALID;
      SM_TRY(sm_destination(c, &p[0], &d));
      SM_TRY(sm_source(c, &p[1], 0, &a));
      for (unsigned row = 0; row < rows; ++row) {
        SmParam matrix = p[2];
        matrix.number += row;
        matrix.swizzle = DX_SWIZZLE_XYZW;
        SM_TRY(sm_source(c, &matrix, 0, &b));
        sm_emit3(c, dot, 0, sm_scratch(0, 1u << row), a, b);
      }
      sm_emit2(c, DXO_MOV, saturate, d, sm_scratch_source(0));
      return SM_OK;
    }
    case SMO_CRS: {      /* a.yzx * b.zxy - a.zxy * b.yzx */
      SmParam first, second;
      if (count != 3) return SM_INVALID;
      first = p[1]; second = p[2];
      SM_TRY(sm_destination(c, &p[0], &d));
      first.swizzle = sm_compose(p[1].swizzle, 0xD2 /* zxyw */); second.swizzle = sm_compose(p[2].swizzle, 0xC9 /* yzxw */);
      SM_TRY(sm_source(c, &first, 0, &a));
      SM_TRY(sm_source(c, &second, 0, &b));
      sm_emit3(c, DXO_MUL, 0, sm_scratch(0, 0x7), a, b);
      first.swizzle = sm_compose(p[1].swizzle, 0xC9); second.swizzle = sm_compose(p[2].swizzle, 0xD2);
      SM_TRY(sm_source(c, &first, 0, &a));
      SM_TRY(sm_source(c, &second, 0, &b));
      sm_emit4(c, DXO_MAD, 0, sm_scratch(0, 0x7), a, b, dx_src(DXT_TEMP, DX_TEMP_SCRATCH, DX_SWIZZLE_XYZW, 1));
      sm_emit2(c, DXO_MOV, saturate, d, sm_scratch_source(0));
      return SM_OK;
    }
    case SMO_SGN:      /* 1, 0 or -1 (the two more parameters Shader Model 2 gives it are scratch registers) */
      if (count < 2) return SM_INVALID;
      SM_TRY(sm_destination(c, &p[0], &d));
      SM_TRY(sm_source(c, &p[1], 0, &a));
      sm_emit3(c, DXO_LT, 0, sm_scratch(0, 0xF), dx_immediate(0, 0, 0, 0), a);
      sm_emit3(c, DXO_AND, 0, sm_scratch(0, 0xF), sm_scratch_source(0), dx_immediate_bits(DX_ONE_BITS));
      sm_emit3(c, DXO_LT, 0, sm_scratch(1, 0xF), a, dx_immediate(0, 0, 0, 0));
      sm_emit3(c, DXO_AND, 0, sm_scratch(1, 0xF), sm_scratch_source(1), dx_immediate_bits(DX_ONE_BITS));
      sm_emit3(c, DXO_ADD, saturate, d, sm_scratch_source(0), dx_src(DXT_TEMP, DX_TEMP_SCRATCH + 1, DX_SWIZZLE_XYZW, 1));
      return SM_OK;
    case SMO_NRM:      /* a / the length of a.xyz */
      if (count != 2) return SM_INVALID;
      SM_TRY(sm_destination(c, &p[0], &d));
      SM_TRY(sm_source(c, &p[1], 0, &a));
      sm_emit3(c, DXO_DP3, 0, sm_scratch(0, 0x1), a, a);
      sm_emit2(c, DXO_RSQ, 0, sm_scratch(0, 0x1), dx_temp(DX_TEMP_SCRATCH, 0));
      sm_emit3(c, DXO_MUL, saturate, d, a, dx_temp(DX_TEMP_SCRATCH, 0));
      return SM_OK;
    case SMO_SINCOS:      /* x: the cosine, y: the sine (Shader Model 2 gives it two constants more, not needed) */
      if (count < 2) return SM_INVALID;
      SM_TRY(sm_destination(c, &p[0], &d));
      SM_TRY(sm_source(c, &p[1], 0, &a));
      sm_emit3(c, DXO_SINCOS, 0, sm_scratch(0, 0x2), sm_scratch(0, 0x1), a);
      sm_emit2(c, DXO_MOV, saturate, d, sm_scratch_source(0));
      return SM_OK;
    case SMO_CMP: case SMO_CND:      /* a >= 0 (cnd: a > 0.5) ? b : e */
      if (count != 4 || s->vertex) return SM_INVALID;
      SM_TRY(sm_destination(c, &p[0], &d));
      SM_TRY(sm_source(c, &p[1], 0, &a));
      SM_TRY(sm_source(c, &p[2], 0, &b));
      SM_TRY(sm_source(c, &p[3], 0, &e));
      if (op == SMO_CMP) sm_emit3(c, DXO_GE, 0, sm_scratch(0, 0xF), a, dx_immediate(0, 0, 0, 0));
      else sm_emit3(c, DXO_LT, 0, sm_scratch(0, 0xF), dx_immediate(0.5f, 0.5f, 0.5f, 0.5f), a);
      sm_emit4(c, DXO_MOVC, saturate, d, sm_scratch_source(0), b, e);
      return SM_OK;
    case SMO_DP2ADD:      /* a.xy . b.xy + e */
      if (count != 4) return SM_INVALID;
      SM_TRY(sm_destination(c, &p[0], &d));
      SM_TRY(sm_source(c, &p[1], 0, &a));
      SM_TRY(sm_source(c, &p[2], 0, &b));
      SM_TRY(sm_source(c, &p[3], 0, &e));
      sm_emit3(c, DXO_DP2, 0, sm_scratch(0, 0x1), a, b);
      sm_emit3(c, DXO_ADD, saturate, d, dx_temp(DX_TEMP_SCRATCH, 0), e);
      return SM_OK;
    case SMO_IF:      /* a bool constant: the shader's own, or one of the device's */
      if (count != 1 || p[0].type != SMR_CONSTBOOL || p[0].number >= 16 || !sm_has_bools_ints(s)) return count == 1 && p[0].type == SMR_PREDICATE ? SM_UNSUPPORTED : SM_INVALID;
      if (c->defb_set[p[0].number]) {
        sm_emit2(c, DXO_MOV, 0, sm_scratch(0, 0x1), dx_immediate_bits(c->defb_value[p[0].number] ? 0xFFFFFFFFu : 0u));
        sm_emit1(c, DXO_IF, DX_NONZERO, dx_scalar(DXT_TEMP, DX_TEMP_SCRATCH, 0));
      } else {
        /* (the bools follow the floats, whose number is known only when the whole shader was read: the element is made right then, see sm_translate) */
        if (c->bool_words >= 64) return SM_UNSUPPORTED;
        s->bools_used = 1;
        sm_emit2(c, DXO_MOV, 0, sm_scratch(0, 0x1), dx_constant(p[0].number, 0, 0, 0, 0));
        if (!c->failed) c->bool_word[c->bool_words++] = (uint16_t)(c->words - 1);      /* (the element is the last word of the instruction) */
        sm_emit1(c, DXO_IF, DX_NONZERO, dx_scalar(DXT_TEMP, DX_TEMP_SCRATCH, 0));
      }
      return SM_OK;
    case SMO_IFC: {      /* a comparison of two numbers (their first components as swizzled) */
      const unsigned comparison = flags & 7;
      if (count != 2 || comparison < 1 || comparison > 6) return SM_INVALID;
      SM_TRY(sm_source(c, &p[0], 0, &a));
      SM_TRY(sm_source(c, &p[1], 0, &b));
      switch (comparison) {
        case 1: sm_emit3(c, DXO_LT, 0, sm_scratch(0, 0x1), b, a); break;      /* > */
        case 2: sm_emit3(c, DXO_EQ, 0, sm_scratch(0, 0x1), a, b); break;
        case 3: sm_emit3(c, DXO_GE, 0, sm_scratch(0, 0x1), a, b); break;
        case 4: sm_emit3(c, DXO_LT, 0, sm_scratch(0, 0x1), a, b); break;
        case 5: sm_emit3(c, DXO_NE, 0, sm_scratch(0, 0x1), a, b); break;
        default: sm_emit3(c, DXO_GE, 0, sm_scratch(0, 0x1), b, a); break;     /* <= */
      }
      sm_emit1(c, DXO_IF, DX_NONZERO, dx_scalar(DXT_TEMP, DX_TEMP_SCRATCH, 0));
      return SM_OK;
    }
    case SMO_ELSE: sm_emit(c, DXO_ELSE, 0, NULL, 0); return count == 0 ? SM_OK : SM_INVALID;
    case SMO_ENDIF: sm_emit(c, DXO_ENDIF, 0, NULL, 0); return count == 0 ? SM_OK : SM_INVALID;
    case SMO_TEXKILL:      /* the pixel goes when one of the first three components is below 0 */
      if (count != 1 || s->vertex) return SM_INVALID;
      {
        SmParam source = p[0];
        source.swizzle = DX_SWIZZLE_XYZW;
        source.modifier = 0;
        if (source.type != SMR_TEMP && source.type != SMR_ADDR && source.type != SMR_INPUT) return SM_INVALID;
        SM_TRY(sm_source(c, &source, 0, &a));
      }
      sm_emit3(c, DXO_LT, 0, sm_scratch(0, 0x7), a, dx_immediate(0, 0, 0, 0));
      sm_emit3(c, DXO_OR, 0, sm_scratch(0, 0x1), dx_temp(DX_TEMP_SCRATCH, 0), dx_temp(DX_TEMP_SCRATCH, SM_SWIZZLE_OF(1)));
      sm_emit3(c, DXO_OR, 0, sm_scratch(0, 0x1), dx_temp(DX_TEMP_SCRATCH, 0), dx_temp(DX_TEMP_SCRATCH, SM_SWIZZLE_OF(2)));
      sm_emit1(c, DXO_DISCARD, DX_NONZERO, dx_scalar(DXT_TEMP, DX_TEMP_SCRATCH, 0));
      return SM_OK;
    case SMO_TEXLD: {      /* the texture of a sampler at a coordinate; texldp divides by w first, texldb takes a bias from w */
      const int project = (flags & 1) != 0, bias = (flags & 2) != 0;
      DxOperand resource, sampler;
      if (count != 3 || s->vertex || p[2].type != SMR_SAMPLER || p[2].number >= 16) return s->vertex && count == 3 ? SM_UNSUPPORTED : SM_INVALID;
      if (c->sampler_declared[p[2].number] < 2) return c->sampler_declared[p[2].number] ? SM_UNSUPPORTED : SM_INVALID;
      SM_TRY(sm_destination(c, &p[0], &d));
      SM_TRY(sm_source(c, &p[1], 0, &a));
      s->sampler_mask |= 1u << p[2].number;
      s->sampler_kind[p[2].number] = (uint8_t)(c->sampler_declared[p[2].number] == 3 ? SM_SAMPLER_CUBE : c->sampler_declared[p[2].number] == 4 ? SM_SAMPLER_VOLUME : SM_SAMPLER_2D);
      resource = dx_src(DXT_RESOURCE, p[2].number, p[2].swizzle, 0);
      sampler.w[0] = ((unsigned)DXT_SAMPLER << 12) | (1u << 20);
      sampler.w[1] = p[2].number;
      sampler.n = 2;
      sm_emit2(c, DXO_MOV, 0, sm_scratch(1, 0xF), a);
      if (project) sm_emit3(c, DXO_DIV, 0, sm_scratch(1, 0x7), sm_scratch_source(1), dx_temp(DX_TEMP_SCRATCH + 1, SM_SWIZZLE_OF(3)));
      const DxOperand fill = dx_dst(DXT_TEMP, DX_TEMP_FILL, 0xF);
      if (bias) {
        DxOperand o[5];
        o[0] = fill; o[1] = sm_scratch_source(1); o[2] = resource; o[3] = sampler; o[4] = dx_scalar(DXT_TEMP, DX_TEMP_SCRATCH + 1, 3);
        sm_emit(c, DXO_SAMPLE_B, 0, o, 5);
      } else sm_emit4(c, DXO_SAMPLE, 0, fill, sm_scratch_source(1), resource, sampler);
      sm_sample_fill(c, d, saturate, p[2].number);
      return SM_OK;
    }
    default:
      return SM_UNSUPPORTED;      /* loops, subroutines, predicates, texldl and texldd */
  }
}

/* ---- pixel shaders 1.x ----------------------------------------------------------------------------------------------------------------------------------- */

/* The texture coordinates n as the vertex shader gave them. */
static int sm_ps1_coordinates(SmContext *c, unsigned n, unsigned swizzle, DxOperand *o) {
  if (n >= 6) return SM_INVALID;
  c->out->varying_mask |= 1u << (SM_SLOT_TEXCOORD + n);
  *o = dx_src(DXT_INPUT, SM_SLOT_TEXCOORD + n, swizzle, 0);
  return SM_OK;
}
/* Stage n's texture at `at`, into `d`. */
static int sm_ps1_sample(SmContext *c, DxOperand d, DxOperand at, unsigned n) {
  DxOperand sampler;
  if (n >= 6) return SM_INVALID;
  c->out->sampler_mask |= 1u << n;
  sampler.w[0] = ((unsigned)DXT_SAMPLER << 12) | (1u << 20);
  sampler.w[1] = n;
  sampler.n = 2;
  sm_emit4(c, DXO_SAMPLE, 0, dx_dst(DXT_TEMP, DX_TEMP_FILL, 0xF), at, dx_src(DXT_RESOURCE, n, DX_SWIZZLE_XYZW, 0), sampler);
  sm_sample_fill(c, d, 0, n);
  return SM_OK;
}
/* After a sample into DX_TEMP_FILL: the channels the texture's format has not become 1, where Direct3D 9 reads 1 and Direct3D 12 0 - by sampler n's fill register
 * (1 where so; the draw fills it in from the format) - and the sample goes to `d`: d = s - s * fill + fill. */
static void sm_sample_fill(SmContext *c, DxOperand d, unsigned saturate, unsigned n) {
  if (c->fill_words + 2 > 256) { c->failed = 1; return; }
  sm_emit3(c, DXO_MUL, 0, dx_dst(DXT_TEMP, DX_TEMP_FILL + 1, 0xF), dx_temp(DX_TEMP_FILL, DX_SWIZZLE_XYZW), dx_constant(n, DX_SWIZZLE_XYZW, 0, 0, 0));
  if (!c->failed) c->fill_word[c->fill_words++] = (uint16_t)(c->words - 1);      /* (the constant's element is the last word) */
  sm_emit3(c, DXO_ADD, 0, dx_dst(DXT_TEMP, DX_TEMP_FILL + 1, 0xF), dx_temp(DX_TEMP_FILL, DX_SWIZZLE_XYZW), dx_src(DXT_TEMP, DX_TEMP_FILL + 1, DX_SWIZZLE_XYZW, 1));
  sm_emit3(c, DXO_ADD, saturate, d, dx_temp(DX_TEMP_FILL + 1, DX_SWIZZLE_XYZW), dx_constant(n, DX_SWIZZLE_XYZW, 0, 0, 0));
  if (!c->failed) c->fill_word[c->fill_words++] = (uint16_t)(c->words - 1);
}
/* The sources of an instruction that have one of the modifiers of pixel shaders 1.x: each is worked out into a temporary of its own, and the
 * parameter becomes that temporary (with the swizzle it had, and negated where the modifier says so). */
static int sm_ps1_modifiers(SmContext *c, SmParam *p, unsigned first, unsigned count) {
  for (unsigned i = first; i < count && i < 5; ++i) {
    const unsigned modifier = p[i].modifier;
    SmParam plain = p[i];
    DxOperand a, d;
    if (modifier < 2 || modifier > 10) continue;
    plain.modifier = 0;
    plain.swizzle = DX_SWIZZLE_XYZW;
    d = dx_dst(DXT_TEMP, DX_TEMP_PS1_SOURCE + i - 1 + (first == 0), 0xF);
    switch (modifier) {
      case 2: case 3:      /* x - 0.5 */
        SM_TRY(sm_source(c, &plain, 0, &a));
        sm_emit3(c, DXO_ADD, 0, d, a, dx_immediate(-0.5f, -0.5f, -0.5f, -0.5f));
        break;
      case 4: case 5:      /* 2 x - 1 */
        SM_TRY(sm_source(c, &plain, 0, &a));
        sm_emit4(c, DXO_MAD, 0, d, a, dx_immediate(2, 2, 2, 2), dx_immediate(-1, -1, -1, -1));
        break;
      case 6:              /* 1 - x */
        SM_TRY(sm_source(c, &plain, 1, &a));
        sm_emit3(c, DXO_ADD, 0, d, a, dx_immediate(1, 1, 1, 1));
        break;
      case 7: case 8:      /* 2 x */
        SM_TRY(sm_source(c, &plain, 0, &a));
        sm_emit3(c, DXO_ADD, 0, d, a, a);
        break;
      default: {           /* x / x.z, x / x.w */
        DxOperand by;
        SmParam component = plain;
        component.swizzle = SM_SWIZZLE_OF(modifier == 9 ? 2u : 3u);
        SM_TRY(sm_source(c, &plain, 0, &a));
        SM_TRY(sm_source(c, &component, 0, &by));
        sm_emit3(c, DXO_DIV, 0, d, a, by);
        break;
      }
    }
    p[i].type = SMR_TEMP;
    p[i].number = DX_TEMP_PS1_SOURCE + i - 1 + (first == 0);
    p[i].modifier = modifier == 3 || modifier == 5 || modifier == 8 ? 1u : 0u;
  }
  return SM_OK;
}
/* A row of the matrix the texm3x* instructions build: the texture coordinates of the destination's stage, dot the source. */
static int sm_ps1_row(SmContext *c, const SmParam *p, unsigned row) {
  DxOperand at, a;
  SM_TRY(sm_ps1_coordinates(c, p[0].number, DX_SWIZZLE_XYZW, &at));
  SM_TRY(sm_source(c, &p[1], 0, &a));
  sm_emit3(c, DXO_DP3, 0, dx_dst(DXT_TEMP, DX_TEMP_PS1_ROWS, 1u << row), at, a);
  return SM_OK;
}

/* An instruction whose last operand is bump register `which` (2 * stage: the matrix, + 1: the luminance scale and offset) of a pixel shader 1.x, swizzled. */
static void sm_emit_bump(SmContext *c, uint32_t op, DxOperand d, DxOperand a, unsigned which, unsigned swizzle) {
  if (c->bump_words >= 64) { c->failed = 1; return; }
  sm_emit3(c, op, 0, d, a, dx_constant(which, swizzle, 0, 0, 0));
  if (!c->failed) c->bump_word[c->bump_words++] = (uint16_t)(c->words - 1);      /* (the constant's element is the last word) */
  if (c->out->bump_stages < which / 2 + 1) c->out->bump_stages = which / 2 + 1;
}
/* Into scratch 1, x and y: stage n's bump environment matrix times source p's (du, dv) - M00 du + M10 dv, M01 du + M11 dv. */
static int sm_ps1_bump_move(SmContext *c, const SmParam *p, unsigned n) {
  SmParam du = *p, dv = *p;
  DxOperand a, b;
  du.swizzle = sm_compose(p->swizzle, SM_SWIZZLE_OF(0));
  dv.swizzle = sm_compose(p->swizzle, SM_SWIZZLE_OF(1));
  SM_TRY(sm_source(c, &du, 0, &a));
  SM_TRY(sm_source(c, &dv, 0, &b));
  sm_emit_bump(c, DXO_MUL, sm_scratch(1, 0x3), a, 2 * n, 0x44u /* x y x y */);
  sm_emit_bump(c, DXO_MUL, sm_scratch(2, 0x3), b, 2 * n, 0xEEu /* z w z w */);
  sm_emit3(c, DXO_ADD, 0, sm_scratch(1, 0x3), sm_scratch_source(1), sm_scratch_source(2));
  return SM_OK;
}

/* One instruction of a pixel shader 1.x: those that fill a texture register here, the arithmetic ones as in the later models. */
static int sm_instruction_ps1(SmContext *c, unsigned op, SmParam *p, unsigned count) {
  SmShader *s = c->out;
  const int old = s->minor < 4;      /* (1.1 to 1.3: the destination of a texture instruction is t<n>, and n is the stage and the coordinates) */
  DxOperand d, a, at;
  SM_TRY(sm_ps1_modifiers(c, p, op == SMO_TEXKILL ? 0u : 1u, count));
  switch (op) {
    case SMO_PHASE: return SM_OK;
    case SMO_TEXLD:
      if (old) {      /* tex t<n> */
        if (count != 1 || p[0].type != SMR_ADDR) return SM_INVALID;
        SM_TRY(sm_ps1_coordinates(c, p[0].number, DX_SWIZZLE_XYZW, &at));
      } else {        /* texld r<n>, t<m> or r<m> */
        if (count != 2 || p[0].type != SMR_TEMP) return SM_INVALID;
        SM_TRY(sm_source(c, &p[1], 0, &at));
      }
      SM_TRY(sm_destination(c, &p[0], &d));
      return sm_ps1_sample(c, d, at, p[0].number);
    case SMO_TEXCOORD:
      if (old) {      /* texcoord t<n>: the coordinates, between 0 and 1 */
        if (count != 1 || p[0].type != SMR_ADDR) return SM_INVALID;
        SM_TRY(sm_ps1_coordinates(c, p[0].number, DX_SWIZZLE_XYZW, &at));
        SM_TRY(sm_destination(c, &p[0], &d));
        sm_emit2(c, DXO_MOV, DX_SATURATE, d, at);
        p[0].mask = 0x8;
        SM_TRY(sm_destination(c, &p[0], &d));
        sm_emit2(c, DXO_MOV, 0, d, dx_immediate(1, 1, 1, 1));
      } else {        /* texcrd r<n>, t<m> */
        if (count != 2) return SM_INVALID;
        SM_TRY(sm_destination(c, &p[0], &d));
        SM_TRY(sm_source(c, &p[1], 0, &a));
        sm_emit2(c, DXO_MOV, 0, d, a);
      }
      return SM_OK;
    case SMO_TEXKILL:
      if (count != 1) return SM_INVALID;
      if (old) {
        if (p[0].type != SMR_ADDR) return SM_INVALID;
        SM_TRY(sm_ps1_coordinates(c, p[0].number, DX_SWIZZLE_XYZW, &a));
      } else {
        p[0].swizzle = DX_SWIZZLE_XYZW;
        SM_TRY(sm_source(c, &p[0], 0, &a));
      }
      sm_emit3(c, DXO_LT, 0, sm_scratch(0, 0x7), a, dx_immediate(0, 0, 0, 0));
      sm_emit3(c, DXO_OR, 0, sm_scratch(0, 0x1), dx_temp(DX_TEMP_SCRATCH, 0), dx_temp(DX_TEMP_SCRATCH, SM_SWIZZLE_OF(1)));
      sm_emit3(c, DXO_OR, 0, sm_scratch(0, 0x1), dx_temp(DX_TEMP_SCRATCH, 0), dx_temp(DX_TEMP_SCRATCH, SM_SWIZZLE_OF(2)));
      sm_emit1(c, DXO_DISCARD, DX_NONZERO, dx_scalar(DXT_TEMP, DX_TEMP_SCRATCH, 0));
      return SM_OK;
    case SMO_TEXREG2AR: case SMO_TEXREG2GB: case SMO_TEXREG2RGB:      /* stage n at two (three) components of another texture register */
      if (!old || count != 2 || p[0].type != SMR_ADDR) return SM_INVALID;
      p[1].swizzle = op == SMO_TEXREG2AR ? 0x03u /* a r */ : op == SMO_TEXREG2GB ? 0x09u /* g b */ : DX_SWIZZLE_XYZW;
      SM_TRY(sm_source(c, &p[1], 0, &at));
      SM_TRY(sm_destination(c, &p[0], &d));
      return sm_ps1_sample(c, d, at, p[0].number);
    case SMO_TEXBEM: case SMO_TEXBEML: {      /* stage n at its coordinates moved by its bump environment matrix times the source's du, dv (texbeml: then scaled) */
      if (!old || count != 2 || p[0].type != SMR_ADDR || p[0].number >= 6) return SM_INVALID;
      const unsigned n = p[0].number;
      SM_TRY(sm_ps1_coordinates(c, n, DX_SWIZZLE_XYZW, &at));
      SM_TRY(sm_ps1_bump_move(c, &p[1], n));
      sm_emit3(c, DXO_ADD, 0, sm_scratch(1, 0x3), sm_scratch_source(1), at);
      sm_emit2(c, DXO_MOV, 0, sm_scratch(1, 0xC), at);
      SM_TRY(sm_destination(c, &p[0], &d));
      SM_TRY(sm_ps1_sample(c, d, sm_scratch_source(1), n));
      if (op == SMO_TEXBEML) {      /* t<n> * (the source's third number * the luminance scale + the offset), not clamped */
        SmParam l = p[1];
        l.swizzle = sm_compose(p[1].swizzle, SM_SWIZZLE_OF(2));
        SM_TRY(sm_source(c, &l, 0, &a));
        sm_emit_bump(c, DXO_MUL, sm_scratch(3, 0xF), a, 2 * n + 1, SM_SWIZZLE_OF(0));
        sm_emit_bump(c, DXO_ADD, sm_scratch(3, 0xF), sm_scratch_source(3), 2 * n + 1, SM_SWIZZLE_OF(1));
        sm_emit3(c, DXO_MUL, 0, d, dx_temp(DX_TEMP_PS1_TEXTURE + n, DX_SWIZZLE_XYZW), sm_scratch_source(3));
      }
      return SM_OK;
    }
    case SMO_BEM: {      /* (1.4) dst.rg = src0.rg + stage dst's matrix times src1's r, g */
      if (old || count != 3 || p[0].type != SMR_TEMP || p[0].number >= 6 || (p[0].mask & 0xC) || p[0].shift) return SM_INVALID;
      SM_TRY(sm_ps1_bump_move(c, &p[2], p[0].number));
      SM_TRY(sm_source(c, &p[1], 0, &a));
      SM_TRY(sm_destination(c, &p[0], &d));
      sm_emit3(c, DXO_ADD, p[0].saturate ? DX_SATURATE : 0u, d, sm_scratch_source(1), a);
      return SM_OK;
    }
    case SMO_TEXM3X2PAD: case SMO_TEXM3X3PAD:
      if (!old || count != 2 || p[0].type != SMR_ADDR || c->pad_rows >= 2) return SM_INVALID;
      SM_TRY(sm_ps1_row(c, p, c->pad_rows));
      ++c->pad_rows;
      return SM_OK;
    case SMO_TEXM3X2TEX:      /* stage n at the two rows */
      if (!old || count != 2 || p[0].type != SMR_ADDR || c->pad_rows != 1) return SM_INVALID;
      SM_TRY(sm_ps1_row(c, p, 1));
      c->pad_rows = 0;
      SM_TRY(sm_destination(c, &p[0], &d));
      return sm_ps1_sample(c, d, dx_temp(DX_TEMP_PS1_ROWS, DX_SWIZZLE_XYZW), p[0].number);
    case SMO_TEXM3X3:         /* the three rows themselves */
      if (!old || count != 2 || p[0].type != SMR_ADDR || c->pad_rows != 2) return SM_INVALID;
      SM_TRY(sm_ps1_row(c, p, 2));
      c->pad_rows = 0;
      sm_emit2(c, DXO_MOV, 0, dx_dst(DXT_TEMP, DX_TEMP_PS1_ROWS, 0x8), dx_immediate(1, 1, 1, 1));
      SM_TRY(sm_destination(c, &p[0], &d));
      sm_emit2(c, DXO_MOV, 0, d, dx_temp(DX_TEMP_PS1_ROWS, DX_SWIZZLE_XYZW));
      return SM_OK;
    case SMO_TEXDP3: case SMO_TEXDP3TEX:      /* the coordinates dot the source: the number itself, or stage n at it */
      if (!old || count != 2 || p[0].type != SMR_ADDR) return SM_INVALID;
      SM_TRY(sm_ps1_coordinates(c, p[0].number, DX_SWIZZLE_XYZW, &at));
      SM_TRY(sm_source(c, &p[1], 0, &a));
      sm_emit2(c, DXO_MOV, 0, sm_scratch(1, 0xF), dx_immediate(0, 0, 0, 0));
      sm_emit3(c, DXO_DP3, 0, sm_scratch(1, op == SMO_TEXDP3 ? 0xFu : 0x1u), at, a);
      SM_TRY(sm_destination(c, &p[0], &d));
      if (op == SMO_TEXDP3TEX) return sm_ps1_sample(c, d, sm_scratch_source(1), p[0].number);
      sm_emit2(c, DXO_MOV, 0, d, sm_scratch_source(1));
      return SM_OK;
    case SMO_NOP: case SMO_MOV: case SMO_ADD: case SMO_SUB: case SMO_MAD: case SMO_MUL: case SMO_DP3: case SMO_DP4: case SMO_LRP: case SMO_CND: case SMO_CMP: {
      /* (the factor of the destination comes after the instruction, and the saturation after the factor) */
      const unsigned shift = count ? p[0].shift : 0u;
      const int saturate = count ? p[0].saturate : 0;
      if (!shift) return sm_instruction(c, op, 0, p, count);
      if (shift > 3 && shift < 13) return SM_INVALID;
      p[0].saturate = 0;
      SM_TRY(sm_instruction(c, op, 0, p, count));
      {
        static const float factor[16] = {1, 2, 4, 8, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0.125f, 0.25f, 0.5f};
        const float f = factor[shift];
        SmParam result = p[0];
        result.swizzle = DX_SWIZZLE_XYZW;
        result.modifier = 0;
        SM_TRY(sm_destination(c, &p[0], &d));
        SM_TRY(sm_source(c, &result, 0, &a));
        sm_emit3(c, DXO_MUL, saturate ? DX_SATURATE : 0u, d, a, dx_immediate(f, f, f, f));
      }
      return SM_OK;
    }
    default:
      return SM_UNSUPPORTED;      /* texm3x3tex, texm3x3spec, texm3x3vspec; texdepth, texm3x2depth */
  }
}
/* The parameters an instruction of a pixel shader 1.x has; 255 for one that is not one of theirs. */
static unsigned sm_parameters_ps1(unsigned op, unsigned minor) {
  switch (op) {
    case SMO_NOP: case SMO_PHASE: return 0;
    case SMO_TEXKILL: case SMO_TEXDEPTH: return 1;
    case SMO_TEXCOORD: case SMO_TEXLD: return minor < 4 ? 1 : 2;
    case SMO_MOV: case SMO_TEXBEM: case SMO_TEXBEML: case SMO_TEXREG2AR: case SMO_TEXREG2GB: case SMO_TEXM3X2PAD: case SMO_TEXM3X2TEX: case SMO_TEXM3X3PAD:
    case SMO_TEXM3X3TEX: case SMO_TEXM3X3VSPEC: case SMO_TEXREG2RGB: case SMO_TEXDP3TEX: case SMO_TEXM3X2DEPTH: case SMO_TEXDP3: case SMO_TEXM3X3: return 2;
    case SMO_ADD: case SMO_SUB: case SMO_MUL: case SMO_DP3: case SMO_DP4: case SMO_TEXM3X3SPEC: case SMO_BEM: return 3;
    case SMO_MAD: case SMO_LRP: case SMO_CND: case SMO_CMP: return 4;
    default: return 255;
  }
}

/* The end of a pixel shader: the alpha test and fog on the colour it made, by the state registers (from `state` on), and the colour out. */
static void sm_pixel_end(SmContext *c, unsigned state) {
  const SmShader *s = c->out;
  const unsigned colour = s->major == 1 ? 0u : (unsigned)DX_TEMP_COLOUR;      /* (pixel shaders 1.x: r0) */
  const DxOperand reference = dx_constant(state + SM_STATE_ALPHA, 0, 0, 0, 0);
  /* the alpha as the eight bits Direct3D 9 compares, against the reference: below, equal, above as 1 or 0, times what each keeps */
  sm_emit2(c, DXO_MOV, DX_SATURATE, sm_scratch(0, 0x8), dx_temp(colour, SM_SWIZZLE_OF(3)));
  sm_emit3(c, DXO_MUL, 0, sm_scratch(0, 0x8), dx_temp(DX_TEMP_SCRATCH, SM_SWIZZLE_OF(3)), dx_immediate(255, 255, 255, 255));
  sm_emit2(c, DXO_ROUND_NE, 0, sm_scratch(0, 0x8), dx_temp(DX_TEMP_SCRATCH, SM_SWIZZLE_OF(3)));
  sm_emit3(c, DXO_LT, 0, sm_scratch(1, 0x1), dx_temp(DX_TEMP_SCRATCH, SM_SWIZZLE_OF(3)), reference);
  sm_emit3(c, DXO_EQ, 0, sm_scratch(1, 0x2), dx_temp(DX_TEMP_SCRATCH, SM_SWIZZLE_OF(3)), reference);
  sm_emit3(c, DXO_LT, 0, sm_scratch(1, 0x4), reference, dx_temp(DX_TEMP_SCRATCH, SM_SWIZZLE_OF(3)));
  sm_emit3(c, DXO_AND, 0, sm_scratch(1, 0x7), sm_scratch_source(1), dx_immediate_bits(DX_ONE_BITS));
  sm_emit3(c, DXO_DP3, 0, sm_scratch(1, 0x1), sm_scratch_source(1), dx_constant(state + SM_STATE_ALPHA_KEEP, DX_SWIZZLE_XYZW, 0, 0, 0));
  sm_emit1(c, DXO_DISCARD, 0, dx_scalar(DXT_TEMP, DX_TEMP_SCRATCH + 1, 0));
  if (s->major < 3) {      /* fog: the colour goes towards the fog's by a factor (Shader Model 3 leaves fog to the shader) */
    /* the depth: the position's z, or its w - which reaches a pixel shader as 1 / w - as the state weighs them */
    const DxOperand depth = dx_temp(DX_TEMP_SCRATCH + 2, 0);
    const DxOperand fog_colour = dx_constant(state + SM_STATE_FOG_COLOUR, DX_SWIZZLE_XYZW, 0, 0, 0);
    c->out->varying_mask |= 1u << (SM_SLOT_TEXCOORD + 8);
    sm_emit3(c, DXO_MAX, 0, sm_scratch(2, 0x1), dx_src(DXT_INPUT, DX_INPUT_POSITION, SM_SWIZZLE_OF(3), 0), dx_immediate(1e-30f, 1e-30f, 1e-30f, 1e-30f));
    sm_emit3(c, DXO_DIV, 0, sm_scratch(2, 0x1), dx_constant(state + SM_STATE_ALPHA, SM_SWIZZLE_OF(2), 0, 0, 0), dx_temp(DX_TEMP_SCRATCH + 2, 0));
    sm_emit4(c, DXO_MAD, 0, sm_scratch(2, 0x1), dx_src(DXT_INPUT, DX_INPUT_POSITION, SM_SWIZZLE_OF(2), 0), dx_constant(state + SM_STATE_ALPHA, SM_SWIZZLE_OF(1), 0, 0, 0),
             dx_temp(DX_TEMP_SCRATCH + 2, 0));
    sm_emit2(c, DXO_MOV, DX_SATURATE, sm_scratch(0, 0x1), dx_src(DXT_INPUT, SM_SLOT_TEXCOORD + 8, 0, 0));
    sm_emit4(c, DXO_MAD, DX_SATURATE, sm_scratch(0, 0x2), depth, dx_constant(state + SM_STATE_FOG, 0, 0, 0, 0), dx_constant(state + SM_STATE_FOG, SM_SWIZZLE_OF(1), 0, 0, 0));
    sm_emit4(c, DXO_MAD, 0, sm_scratch(0, 0x4), depth, dx_constant(state + SM_STATE_FOG, SM_SWIZZLE_OF(3), 0, 0, 0), dx_constant(state + SM_STATE_FOG, SM_SWIZZLE_OF(2), 0, 0, 0));
    sm_emit3(c, DXO_MUL, 0, sm_scratch(0, 0x4), dx_temp(DX_TEMP_SCRATCH, SM_SWIZZLE_OF(2)), depth);
    sm_emit2(c, DXO_EXP, DX_SATURATE, sm_scratch(0, 0x4), dx_src(DXT_TEMP, DX_TEMP_SCRATCH, SM_SWIZZLE_OF(2), 1));
    sm_emit2(c, DXO_MOV, 0, sm_scratch(0, 0x8), dx_immediate(1, 1, 1, 1));
    sm_emit3(c, DXO_DP4, 0, sm_scratch(0, 0x1), sm_scratch_source(0), dx_constant(state + SM_STATE_FOG_SOURCE, DX_SWIZZLE_XYZW, 0, 0, 0));
    sm_emit3(c, DXO_ADD, 0, sm_scratch(1, 0x7), dx_temp(colour, DX_SWIZZLE_XYZW), dx_constant(state + SM_STATE_FOG_COLOUR, DX_SWIZZLE_XYZW, 1, 0, 0));
    sm_emit4(c, DXO_MAD, 0, dx_dst(DXT_TEMP, colour, 0x7), dx_temp(DX_TEMP_SCRATCH, 0), sm_scratch_source(1), fog_colour);
  }
  sm_emit2(c, DXO_MOV, 0, dx_dst(DXT_OUTPUT, 0, 0xF), dx_temp(colour, DX_SWIZZLE_XYZW));
}

/* ---- reading a shader ---------------------------------------------------------------------------------------------------------------------------------- */

static void sm_put32(uint8_t *at, uint32_t v) { at[0] = (uint8_t)v; at[1] = (uint8_t)(v >> 8); at[2] = (uint8_t)(v >> 16); at[3] = (uint8_t)(v >> 24); }

/* One parameter token (and, for a constant addressed through a register from Shader Model 2 on, the token after it). */
static int sm_parameter(const SmShader *s, const uint32_t *tokens, size_t available, int destination, SmParam *p, unsigned *used) {
  uint32_t t;
  if (available < 1) return SM_INVALID;
  t = tokens[0];
  memset(p, 0, sizeof *p);
  p->type = ((t >> 28) & 7) | ((t >> 8) & 0x18);
  p->number = t & 0x7FF;
  *used = 1;
  if (destination) {
    p->mask = (t >> 16) & 0xF;
    p->saturate = ((t >> 20) & 1) != 0;
    p->shift = (t >> 24) & 0xF;
    if (p->shift && (s->vertex || s->major != 1)) return SM_INVALID;      /* (a factor: pixel shaders 1.x only) */
    if (t & 0x2000) return SM_UNSUPPORTED;           /* (a destination addressed through a register) */
  } else {
    p->swizzle = (t >> 16) & 0xFF;
    p->modifier = (t >> 24) & 0xF;
    if (t & 0x2000) {
      p->relative = 1;
      if (s->major >= 2) {
        uint32_t r;
        if (available < 2) return SM_INVALID;
        r = tokens[1];
        *used = 2;
        if ((((r >> 28) & 7) | ((r >> 8) & 0x18)) != SMR_ADDR || (r & 0x7FF) != 0) return SM_UNSUPPORTED;      /* (the loop counter) */
        p->relative_component = (r >> 16) & 3;
      }
    }
  }
  return SM_OK;
}

/* The parameters an instruction of Shader Model 1 has (it does not say itself); 255 for one that is not one of vs_1_1's. */
static unsigned sm_parameters_v1(unsigned op) {
  switch (op) {
    case SMO_NOP: return 0;
    case SMO_MOV: case SMO_RCP: case SMO_RSQ: case SMO_EXP: case SMO_LOG: case SMO_LIT: case SMO_FRC: case SMO_EXPP: case SMO_LOGP: return 2;
    case SMO_ADD: case SMO_SUB: case SMO_MUL: case SMO_DP3: case SMO_DP4: case SMO_MIN: case SMO_MAX: case SMO_SLT: case SMO_SGE: case SMO_DST: case SMO_M4X4: case SMO_M4X3:
    case SMO_M3X4: case SMO_M3X3: case SMO_M3X2: return 3;
    case SMO_MAD: return 4;
    default: return 255;
  }
}

/* Translates the shader at `tokens` (at most `available` tokens of it are there). SM_OK and *out filled in, or why not. */
static int sm_translate(const uint32_t *tokens, size_t available, SmShader *out, SmContext *c) {
  size_t at = 1;
  int ended = 0;
  memset(out, 0, sizeof *out);
  memset(c, 0, sizeof *c);
  c->out = out;
  memset(c->slot_of_register, 255, sizeof c->slot_of_register);
  if (!tokens || available < 2) return SM_INVALID;
  if ((tokens[0] >> 16) == 0xFFFE) out->vertex = 1;
  else if ((tokens[0] >> 16) != 0xFFFF) return SM_INVALID;
  out->major = (tokens[0] >> 8) & 0xFF;
  out->minor = tokens[0] & 0xFF;
  if (out->major < 1 || out->major > 3) return SM_INVALID;
  if (!out->vertex && out->major == 1 && (out->minor < 1 || out->minor > 4)) return SM_INVALID;
  if (available > SM_MAX_TOKENS) available = SM_MAX_TOKENS;
  /* (a vertex shader below 3.0 whose oD0 is not written gives white, as Direct3D 9 has it: it starts written so) */
  if (out->vertex && out->major < 3) {
    sm_emit2(c, DXO_MOV, 0, dx_dst(DXT_OUTPUT, SM_SLOT_COLOR, 0xF), dx_immediate(1, 1, 1, 1));
    out->varying_mask |= 1u << SM_SLOT_COLOR;
  }

  while (at < available) {
    const uint32_t t = tokens[at];
    const unsigned op = t & 0xFFFF;
    unsigned count, used = 0;
    SmParam p[5];
    if (op == SMO_END) { ended = 1; out->tokens = at + 1; break; }
    if (op == SMO_COMMENT) {
      const size_t length = (t >> 16) & 0x7FFF;
      if (length >= available - at) return SM_INVALID;
      at += 1 + length;
      continue;
    }
    if (t & 0x80000000u) return SM_INVALID;
    ++at;
    if (op == SMO_DCL) {      /* what a register is: an input, a sampler, an output of Shader Model 3 */
      uint32_t usage_token, r;
      unsigned type, number;
      if (available - at < 2) return SM_INVALID;
      usage_token = tokens[at];
      r = tokens[at + 1];
      at += 2;
      type = ((r >> 28) & 7) | ((r >> 8) & 0x18);
      number = r & 0x7FF;
      if (type == SMR_SAMPLER) {
        if (number >= 16) return SM_INVALID;
        /* (2 two-dimensional, 3 a cube, 4 a volume - D3DSTT_* - or 1, a kind that is not taken) */
        const unsigned texture_type = (usage_token >> 27) & 0xF;
        c->sampler_declared[number] = (uint8_t)(texture_type >= 2 && texture_type <= 4 ? texture_type : 1);
      } else if (type == SMR_INPUT && out->vertex) {
        if (number >= 16 || out->input_count >= 16) return SM_INVALID;
        out->inputs[out->input_count].usage = (uint8_t)(usage_token & 0x1F);
        out->inputs[out->input_count].index = (uint8_t)((usage_token >> 16) & 0xF);
        out->inputs[out->input_count].reg = (uint8_t)number;
        ++out->input_count;
      } else if ((type == SMR_INPUT && !out->vertex && out->major >= 3) || (type == SMR_OUTPUT && out->vertex && out->major >= 3)) {
        if (number >= 16) return SM_INVALID;
        c->slot_of_register[number] = (uint8_t)sm_slot_of_usage(usage_token & 0x1F, (usage_token >> 16) & 0xF);
      } else if (type == SMR_INPUT || type == SMR_ADDR) {
        /* (a pixel shader below 3.0 declares its colours and texture coordinates: they are what their numbers say) */
      } else return SM_UNSUPPORTED;      /* (vPos, vFace) */
      continue;
    }
    if (op == SMO_DEF || op == SMO_DEFB || op == SMO_DEFI) {
      uint32_t r;
      unsigned type, number;
      const size_t values = op == SMO_DEFB ? 1 : 4;
      if (available - at < 1 + values) return SM_INVALID;
      r = tokens[at];
      type = ((r >> 28) & 7) | ((r >> 8) & 0x18);
      number = r & 0x7FF;
      if (op == SMO_DEF) {
        if (type != SMR_CONST || number >= sm_float_limit(out)) return SM_INVALID;
        memcpy(c->def_value[number], &tokens[at + 1], 16);
        c->def_set[number] = 1;
      } else if (op == SMO_DEFB) {
        if (type != SMR_CONSTBOOL || number >= 16 || !sm_has_bools_ints(out)) return SM_INVALID;
        c->defb_set[number] = 1;
        c->defb_value[number] = tokens[at + 1] != 0;
      } else if (type != SMR_CONSTINT || number >= 16 || !sm_has_bools_ints(out)) return SM_INVALID;      /* (defi: integer constants are for loops) */
      at += 1 + values;
      continue;
    }
    count = out->major >= 2 ? (t >> 24) & 0xF : out->vertex ? sm_parameters_v1(op) : sm_parameters_ps1(op, out->minor);
    if (count == 255) return SM_UNSUPPORTED;
    if (count > 5 && op != SMO_SINCOS && op != SMO_SGN) return SM_INVALID;
    {
      /* (if, ifc, texkill and the flow instructions have no destination; texkill's one register is read) */
      const int first_is_destination = !(op == SMO_IF || op == SMO_IFC || op == SMO_TEXKILL || op == SMO_ELSE || op == SMO_ENDIF);
      unsigned read = 0, kept = 0;
      while (read < count) {
        SmParam parameter;
        const int status = sm_parameter(out, tokens + at, available - at, first_is_destination && kept == 0 && read == 0, &parameter, &used);
        if (status != SM_OK) return status;
        at += used;
        read += used;
        if (kept < 5) p[kept++] = parameter;
      }
      if (read != count) return SM_INVALID;
      {
        const int status = !out->vertex && out->major == 1 ? sm_instruction_ps1(c, op, p, kept) : sm_instruction(c, op, (t >> 16) & 0xFF, p, kept);
        if (status != SM_OK) return status;
      }
    }
    if (c->failed) return SM_UNSUPPORTED;      /* (too long) */
  }
  if (!ended) return SM_INVALID;
  /* (the state registers come after the floats and bools, which are all known now - all 224 of a vertex shader that addresses them through a0) */
  out->state_register = (out->vertex && out->relative ? SM_RELATIVE_REGISTERS : out->float_registers) + (out->bools_used ? SM_BOOL_REGISTERS : 0u);
  if (!out->vertex) {
    sm_pixel_end(c, out->state_register);
    out->fill_register = out->state_register + SM_STATE_REGISTERS;
  }
  else if (out->varying_mask & 1u) {
    /* the position out, moved by half a pixel: xy + (state.xy * w) */
    sm_emit4(c, DXO_MAD, 0, dx_dst(DXT_OUTPUT, SM_SLOT_POSITION, 0x3), dx_temp(DX_TEMP_POSITION, SM_SWIZZLE_OF(3)), dx_constant(out->state_register, DX_SWIZZLE_XYZW, 0, 0, 0),
             dx_temp(DX_TEMP_POSITION, DX_SWIZZLE_XYZW));
    sm_emit2(c, DXO_MOV, 0, dx_dst(DXT_OUTPUT, SM_SLOT_POSITION, 0xC), dx_temp(DX_TEMP_POSITION, DX_SWIZZLE_XYZW));
  }
  sm_emit(c, DXO_RET, 0, NULL, 0);
  if (c->failed) return SM_UNSUPPORTED;
  if (out->vertex && !(out->varying_mask & 1u)) return SM_INVALID;      /* (no position) */

  /* cb0: the floats, then the bools */
  if (out->relative) {
    if (out->float_registers > SM_RELATIVE_REGISTERS) return SM_UNSUPPORTED;
    out->float_registers = SM_RELATIVE_REGISTERS;
  }
  unsigned fills = 0;
  for (unsigned i = 0; i < 16; ++i) if (out->sampler_mask >> i & 1) fills = i + 1;
  out->bump_register = out->vertex ? 0u : out->fill_register + fills;
  out->constant_registers = out->float_registers + (out->bools_used ? SM_BOOL_REGISTERS : 0u) + (out->vertex ? SM_VS_STATE_REGISTERS : SM_STATE_REGISTERS + fills + 2u * out->bump_stages);
  if (out->constant_registers > 512) return SM_UNSUPPORTED;      /* (more than any version has: 256 + 16 + 1, 224 + 16 + 5 + 16) */
  for (unsigned i = 0; i < c->bool_words; ++i) c->code[c->bool_word[i]] += out->float_registers;      /* (the bools' elements were written as their numbers) */
  for (unsigned i = 0; i < c->fill_words; ++i) c->code[c->fill_word[i]] += out->fill_register;      /* (and the fills' as their samplers') */
  for (unsigned i = 0; i < c->bump_words; ++i) c->code[c->bump_word[i]] += out->bump_register;      /* (and the bump registers') */

  /* the container: the two signatures, then the program with its declarations */
  {
    uint8_t *b = out->dxbc;
    size_t size = 32 + 12, chunk_at[3];
    uint32_t declarations[4 + 16 * 4 + 4 + 2];
    unsigned declaration_words = 0;
    static const char position_name[] = "SV_Position", colour_name[] = "COLOR", coordinate_name[] = "TEXCOORD", target_name[] = "SV_Target", attribute_name[] = "ATTR";
    for (unsigned chunk = 0; chunk < 2; ++chunk) {      /* 0: the inputs, 1: the outputs */
      unsigned elements = 0, names_at, name_cursor;
      uint8_t *start;
      const int varyings = out->vertex ? chunk == 1 : chunk == 0;
      struct { const char *name; unsigned index, reg; } element[20];
      if (varyings) {
        for (unsigned slot = 0; slot < SM_SLOTS; ++slot) {
          if (!(out->varying_mask >> slot & 1) || (slot == SM_SLOT_POSITION && !out->vertex)) continue;
          element[elements].name = slot == SM_SLOT_POSITION ? position_name : slot < SM_SLOT_TEXCOORD ? colour_name : coordinate_name;
          element[elements].index = slot == SM_SLOT_POSITION ? 0 : slot < SM_SLOT_TEXCOORD ? slot - SM_SLOT_COLOR : slot - SM_SLOT_TEXCOORD;
          element[elements++].reg = slot;
        }
      } else if (out->vertex) {
        for (unsigned i = 0; i < out->input_count; ++i) {
          element[elements].name = attribute_name;
          element[elements].index = out->inputs[i].reg;
          element[elements++].reg = out->inputs[i].reg;
        }
      } else {
        for (unsigned target = 0; target < 4; ++target) {
          if (!((out->target_mask | 1u) >> target & 1)) continue;
          element[elements].name = target_name;
          element[elements].index = target;
          element[elements++].reg = target;
        }
      }
      chunk_at[chunk] = size;
      start = b + size;
      memcpy(start, chunk == 0 ? "ISGN" : "OSGN", 4);
      sm_put32(start + 8, elements);
      sm_put32(start + 12, 8);
      names_at = 8 + 24 * elements;
      name_cursor = names_at;
      for (unsigned i = 0; i < elements; ++i) {
        uint8_t *e = start + 8 + 8 + 24 * i;
        size_t length = 0;
        while (element[i].name[length]) ++length;
        sm_put32(e, name_cursor);
        sm_put32(e + 4, element[i].index);
        sm_put32(e + 8, element[i].name == position_name ? 1 : 0);      /* (the system value: the position) */
        sm_put32(e + 12, 3);                                            /* (floats) */
        sm_put32(e + 16, element[i].reg);
        sm_put32(e + 20, 0x0F0F);
        memcpy(start + 8 + name_cursor, element[i].name, length + 1);
        name_cursor += (unsigned)length + 1;
      }
      while (name_cursor % 4) start[8 + name_cursor++] = 0;
      sm_put32(start + 4, name_cursor);
      size += 8 + name_cursor;
    }
    if (out->constant_registers) {
      declarations[declaration_words++] = DXO_DCL_CONSTANT_BUFFER | (4u << 24);
      declarations[declaration_words++] = 0x208E46;      /* cb0[..].xyzw, as the compiler writes it */
      declarations[declaration_words++] = 0;
      declarations[declaration_words++] = out->constant_registers;
    }
    for (unsigned unit = 0; unit < 16; ++unit) {
      if (!(out->sampler_mask >> unit & 1)) continue;
      /* dcl_resource_texture2d / texturecube / texture3d t<unit>, of floats */
      const unsigned dimension = out->sampler_kind[unit] == SM_SAMPLER_CUBE ? 6u : out->sampler_kind[unit] == SM_SAMPLER_VOLUME ? 5u : 3u;
      declarations[declaration_words++] = DXO_DCL_RESOURCE | (dimension << 11) | (4u << 24);
      declarations[declaration_words++] = ((unsigned)DXT_RESOURCE << 12) | (1u << 20);
      declarations[declaration_words++] = unit;
      declarations[declaration_words++] = 0x5555;
    }
    if (!out->vertex && out->major < 3) {      /* dcl_input_ps_siv linear v16.xyzw, position: the depth fog is made of */
      declarations[declaration_words++] = 100u | (2u << 11) | (4u << 24);
      declarations[declaration_words++] = 2u | (0xFu << 4) | ((unsigned)DXT_INPUT << 12) | (1u << 20);
      declarations[declaration_words++] = DX_INPUT_POSITION;
      declarations[declaration_words++] = 1;
    }
    declarations[declaration_words++] = DXO_DCL_TEMPS | (2u << 24);
    declarations[declaration_words++] = DX_TEMPS;
    chunk_at[2] = size;
    if (size + 8 + 8 + 4 * ((size_t)declaration_words + c->words) > SM_DXBC_BYTES) return SM_UNSUPPORTED;
    memcpy(b + size, "SHDR", 4);
    sm_put32(b + size + 4, 8 + 4 * (declaration_words + c->words));
    sm_put32(b + size + 8, ((out->vertex ? 1u : 0u) << 16) | 0x40);
    sm_put32(b + size + 12, 2 + declaration_words + c->words);
    size += 16;
    for (unsigned w = 0; w < declaration_words; ++w, size += 4) sm_put32(b + size, declarations[w]);
    for (unsigned w = 0; w < c->words; ++w, size += 4) sm_put32(b + size, c->code[w]);
    memcpy(b, "DXBC", 4);      /* (the checksum stays 0: the kernel does not look at it) */
    sm_put32(b + 20, 1);
    sm_put32(b + 24, (uint32_t)size);
    sm_put32(b + 28, 3);
    for (unsigned chunk = 0; chunk < 3; ++chunk) sm_put32(b + 32 + 4 * chunk, (uint32_t)chunk_at[chunk]);
    out->dxbc_size = size;
  }
  return SM_OK;
}

#endif
