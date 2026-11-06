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

/* SPIR-V to DXBC (Shader Model 4.0), see spirv.h for the interface and for why. How it works:
 *
 *  - Every SPIR-V value is a list of "leaves": a scalar or a vector of up to four components (a matrix is its columns, an array its elements, a struct its members, each flattened
 *    the same way). A leaf is an operand: a virtual temporary, an input register, a constant buffer register, or an immediate, with a swizzle. Reading a member of a uniform block,
 *    an input, a constant, or taking components out of a vector costs no instruction: it only makes a new operand.
 *  - Variables (Function / Private / Output) live in virtual temporaries or output registers; a load copies, a store is a mov.
 *  - Instructions are collected in a list over virtual temporaries; the registers are given out afterwards by interval colouring (the kernel's DXBC front end has 32 temporaries).
 *  - The structured control flow of SPIR-V (OpSelectionMerge / OpLoopMerge) becomes if / else / endif and loop / endloop. A loop whose continue target has several
 *    predecessors is wrapped in a second loop that runs once, so that `continue` still reaches the continue construct. A function with more than one return runs inside a
 *    loop that runs once too. Phi nodes are variables written on the edges.
 *  - Vulkan's clip space has Y down, the kernel's Direct3D shaders Y up (it flips them): the position's Y is negated when it is written.
 *  - Resource numbering comes from the caller (SpvResource::slot, ::cb_vec4, ::uav): a uniform or push constant block is a constant buffer cb<slot> of cb_vec4 registers; a sampled image
 *    is texture t<slot> with sampler s<slot>; a storage buffer is the raw buffer u<slot> (read and written) or t<slot> (read only). Constant buffers are std140 / std430 laid out with the
 *    offsets of the module (Offset, ArrayStride, MatrixStride decorations); a column-major matrix is a register per column.
 *  - Memory (storage buffers and Workgroup variables) is addressed by bytes: an access chain into it is a constant offset plus, for dynamic indices, an integer in a temporary; a load is
 *    ld_raw, a store store_raw, an atomic imm_atomic_*. All the Workgroup variables of a compute shader share the group-shared memory g0, one after the other.
 *  - A dynamic index into something held in registers (a local array, a vector's component) is a chain of comparisons and movc over the elements: a load picks, a store writes
 *    each element under its comparison.
 * Not taken (spv_error says which): double precision, 64-bit integers, storage images and texel buffers, compare-exchange atomics, the length of a runtime array, tessellation /
 * geometry stages, more than four dynamic indices into registers (or 256 places they may pick), row-major matrices, subgroup operations, OpSpecConstantOp. */
#include "spirv.h"

typedef unsigned long long sp_u64;
typedef unsigned char sp_u8;

static void sp_zero(void *p, sp_u64 n) {
  sp_u8 *b = (sp_u8 *)p;
  while (n--) *b++ = 0;
}
static void sp_copy(void *d, const void *s, sp_u64 n) {
  sp_u8 *a = (sp_u8 *)d;
  const sp_u8 *b = (const sp_u8 *)s;
  while (n--) *a++ = *b++;
}

/* ---- SPIR-V numbers ---------------------------------------------------------------------------------------------------------------------------------------------------- */
enum {
  OpName = 5, OpExtInstImport = 11, OpExtInst = 12, OpEntryPoint = 15, OpExecutionMode = 16, OpTypeVoid = 19, OpTypeBool, OpTypeInt, OpTypeFloat, OpTypeVector, OpTypeMatrix, OpTypeImage, OpTypeSampler,
  OpTypeSampledImage, OpTypeArray, OpTypeRuntimeArray, OpTypeStruct, OpTypePointer = 32, OpTypeFunction, OpConstantTrue = 41, OpConstantFalse, OpConstant, OpConstantComposite, OpConstantNull = 46,
  OpSpecConstantTrue = 48, OpSpecConstantFalse, OpSpecConstant, OpSpecConstantComposite, OpSpecConstantOp, OpFunction = 54, OpFunctionParameter, OpFunctionEnd, OpFunctionCall, OpVariable = 59,
  OpLoad = 61, OpStore, OpCopyMemory, OpAccessChain = 65, OpInBoundsAccessChain, OpDecorate = 71, OpMemberDecorate, OpVectorExtractDynamic = 77, OpVectorInsertDynamic, OpVectorShuffle,
  OpCompositeConstruct, OpCompositeExtract, OpCompositeInsert, OpCopyObject, OpTranspose, OpSampledImage = 86, OpImageSampleImplicitLod, OpImageSampleExplicitLod, OpImageSampleDrefImplicitLod,
  OpImageSampleDrefExplicitLod, OpImageSampleProjImplicitLod, OpImageSampleProjExplicitLod, OpImageSampleProjDrefImplicitLod, OpImageSampleProjDrefExplicitLod, OpImageFetch, OpImageGather,
  OpImageDrefGather, OpImageRead, OpImageWrite, OpImage, OpImageQueryFormat, OpImageQueryOrder, OpImageQuerySizeLod, OpImageQuerySize, OpImageQueryLod, OpImageQueryLevels, OpImageQuerySamples,
  OpConvertFToU = 109, OpConvertFToS, OpConvertSToF, OpConvertUToF, OpUConvert, OpSConvert, OpFConvert, OpBitcast = 124, OpSNegate = 126, OpFNegate, OpIAdd, OpFAdd, OpISub, OpFSub, OpIMul, OpFMul,
  OpUDiv, OpSDiv, OpFDiv, OpUMod, OpSRem, OpSMod, OpFRem, OpFMod, OpVectorTimesScalar, OpMatrixTimesScalar, OpVectorTimesMatrix, OpMatrixTimesVector, OpMatrixTimesMatrix, OpOuterProduct, OpDot,
  OpAny = 154, OpAll, OpIsNan, OpIsInf, OpLogicalEqual = 164, OpLogicalNotEqual, OpLogicalOr, OpLogicalAnd, OpLogicalNot, OpSelect, OpIEqual, OpINotEqual, OpUGreaterThan, OpSGreaterThan,
  OpUGreaterThanEqual, OpSGreaterThanEqual, OpULessThan, OpSLessThan, OpULessThanEqual, OpSLessThanEqual, OpFOrdEqual, OpFUnordEqual, OpFOrdNotEqual, OpFUnordNotEqual, OpFOrdLessThan,
  OpFUnordLessThan, OpFOrdGreaterThan, OpFUnordGreaterThan, OpFOrdLessThanEqual, OpFUnordLessThanEqual, OpFOrdGreaterThanEqual, OpFUnordGreaterThanEqual, OpShiftRightLogical = 194,
  OpShiftRightArithmetic, OpShiftLeftLogical, OpBitwiseOr, OpBitwiseXor, OpBitwiseAnd, OpNot, OpDPdx = 207, OpDPdy, OpFwidth, OpDPdxFine, OpDPdyFine, OpFwidthFine, OpDPdxCoarse, OpDPdyCoarse,
  OpFwidthCoarse, OpPhi = 245, OpLoopMerge, OpSelectionMerge, OpLabel, OpBranch, OpBranchConditional, OpSwitch, OpKill, OpReturn, OpReturnValue, OpUnreachable,
  OpSourceContinued = 2, OpSource = 3, OpSourceExtension = 4, OpString = 7, OpLine = 8, OpNoLine = 317, OpCapability = 17, OpMemoryModel = 14, OpUndef = 1, OpNop = 0, OpMemberName = 6,
  OpDecorationGroup = 73, OpExtension = 10, OpTerminateInvocation = 4416, OpDemoteToHelperInvocation = 5380, OpModuleProcessed = 330, OpExecutionModeId = 331, OpArrayLength = 68,
  OpControlBarrier = 224, OpMemoryBarrier, OpAtomicLoad = 227, OpAtomicStore, OpAtomicExchange, OpAtomicCompareExchange, OpAtomicCompareExchangeWeak, OpAtomicIIncrement, OpAtomicIDecrement,
  OpAtomicIAdd, OpAtomicISub, OpAtomicSMin, OpAtomicUMin, OpAtomicSMax, OpAtomicUMax, OpAtomicAnd, OpAtomicOr, OpAtomicXor
};
enum { DecoBlock = 2, DecoBufferBlock = 3, DecoRowMajor = 4, DecoColMajor = 5, DecoArrayStride = 6, DecoMatrixStride = 7, DecoBuiltIn = 11, DecoFlat = 14, DecoLocation = 30, DecoComponent = 31,
       DecoBinding = 33, DecoDescriptorSet = 34, DecoOffset = 35, DecoSpecId = 1, DecoNonWritable = 24 };
enum { BuiltInPosition = 0, BuiltInPointSize = 1, BuiltInClipDistance = 3, BuiltInCullDistance = 4, BuiltInVertexId = 5, BuiltInInstanceId = 6, BuiltInFragCoord = 15, BuiltInPointCoord = 16,
       BuiltInFrontFacing = 17, BuiltInFragDepth = 22, BuiltInVertexIndex = 42, BuiltInInstanceIndex = 43, BuiltInNumWorkgroups = 24, BuiltInWorkgroupSize = 25, BuiltInWorkgroupId = 26,
       BuiltInLocalInvocationId = 27, BuiltInGlobalInvocationId = 28, BuiltInLocalInvocationIndex = 29 };
enum { SCUniformConstant = 0, SCInput = 1, SCUniform = 2, SCOutput = 3, SCWorkgroup = 4, SCPrivate = 6, SCFunction = 7, SCPushConstant = 9, SCStorageBuffer = 12 };

/* ---- DXBC numbers ------------------------------------------------------------------------------------------------------------------------------------------------------ */
enum {
  D_ADD = 0, D_AND = 1, D_BREAK = 2, D_BREAKC = 3, D_CONTINUE = 7, D_DISCARD = 13, D_DIV = 14, D_DP2 = 15, D_DP3 = 16, D_DP4 = 17, D_ELSE = 18, D_ENDIF = 21, D_ENDLOOP = 22, D_EQ = 24, D_EXP = 25,
  D_FRC = 26, D_FTOI = 27, D_FTOU = 28, D_GE = 29, D_IADD = 30, D_IF = 31, D_IEQ = 32, D_IGE = 33, D_ILT = 34, D_IMAD = 35, D_IMAX = 36, D_IMIN = 37, D_IMUL = 38, D_INE = 39, D_INEG = 40,
  D_ISHL = 41, D_ISHR = 42, D_ITOF = 43, D_LD = 45, D_LOG = 47, D_LOOP = 48, D_LT = 49, D_MAD = 50, D_MIN = 51, D_MAX = 52, D_MOV = 54, D_MOVC = 55, D_MUL = 56, D_NE = 57, D_NOT = 59, D_OR = 60,
  D_RESINFO = 61, D_RET = 62, D_ROUND_NE = 64, D_ROUND_NI = 65, D_ROUND_PI = 66, D_ROUND_Z = 67, D_RSQ = 68, D_SAMPLE = 69, D_SAMPLE_C = 70, D_SAMPLE_C_LZ = 71, D_SAMPLE_L = 72, D_SAMPLE_D = 73,
  D_SAMPLE_B = 74, D_SQRT = 75, D_SINCOS = 77, D_UDIV = 78, D_ULT = 79, D_UGE = 80, D_UMUL = 81, D_UMAX = 83, D_UMIN = 84, D_USHR = 85, D_UTOF = 86, D_XOR = 87, D_DDX = 11, D_DDY = 12,
  D_DCL_RESOURCE = 88, D_DCL_CB = 89, D_DCL_SAMPLER = 90, D_DCL_INPUT = 95, D_DCL_INPUT_SGV = 96, D_DCL_INPUT_PS = 98, D_DCL_INPUT_PS_SIV = 100, D_DCL_OUTPUT = 101, D_DCL_OUTPUT_SIV = 103,
  D_DCL_TEMPS = 104, D_DCL_GLOBAL_FLAGS = 106, D_DCL_THREAD_GROUP = 155, D_DCL_UAV_RAW = 157, D_DCL_TGSM_RAW = 159, D_DCL_RESOURCE_RAW = 161, D_LD_RAW = 165, D_STORE_RAW = 166,
  D_IMM_ATOMIC_IADD = 180, D_IMM_ATOMIC_AND, D_IMM_ATOMIC_OR, D_IMM_ATOMIC_XOR, D_IMM_ATOMIC_EXCH, D_IMM_ATOMIC_CMP_EXCH, D_IMM_ATOMIC_IMAX, D_IMM_ATOMIC_IMIN, D_IMM_ATOMIC_UMAX,
  D_IMM_ATOMIC_UMIN, D_SYNC = 190
};

/* ---- the operand and instruction lists ------------------------------------------------------------------------------------------------------------------------------- */
enum { OF_TEMP = 0, OF_IN = 1, OF_OUT = 2, OF_IMM = 4, OF_CB = 8, OF_SAMP = 6, OF_RES = 7, OF_NULL = 13, OF_UAV = 30, OF_TGSM = 31, OF_THREAD_ID = 32, OF_GROUP_ID = 33, OF_THREAD_IN_GROUP = 34,
       OF_THREAD_INDEX = 36, OF_NONE = 255 };
/* (the compute shader's system values are operands of their own, with no register number) */
#define IS_SYSTEM_FILE(f) ((f) >= OF_THREAD_ID && (f) <= OF_THREAD_INDEX)
enum { CLS_F = 0, CLS_I = 1, CLS_U = 2, CLS_B = 3 };

typedef struct SpOp {
  sp_u8 file, mask, neg, abs, rel, rel_comp, ncomp, pad;
  sp_u8 swz[4];
  uint32_t idx;        /* register (a virtual one for OF_TEMP), constant buffer slot, texture / sampler unit */
  uint32_t idx2;       /* constant buffer element */
  uint32_t rel_reg;    /* relative addressing: the virtual temporary that holds the integer */
  uint32_t imm[4];
} SpOp;

typedef struct SpIns {
  uint16_t op;
  sp_u8 sat, test, ndst, nsrc, nflags, pad;
  uint32_t extra;      /* resinfo: the return type; dcl: the mode */
  SpOp dst[2];
  SpOp src[6];
} SpIns;

typedef struct Leaf {
  SpOp op;
  sp_u8 n, cls;
} Leaf;

typedef struct Val {
  uint32_t type;
  uint32_t nleaf;
  Leaf *leaf;
  /* a pointer */
  sp_u8 is_ptr, is_cb, is_image, comp;     /* comp: 0..3 the pointer is to one component of a vector leaf, 4: the whole leaf */
  uint32_t var;                           /* the variable id */
  uint32_t leaf0;                         /* register-resident variables: the first leaf the pointer refers to */
  uint32_t byte_off;                      /* constant buffer: the offset in the block */
  uint32_t mstride;                       /* constant buffer: the MatrixStride in force */
  uint32_t dyn_reg, dyn_on, dyn_comp;     /* constant buffer: a dynamic register offset (integer in a virtual temporary); memory: a dynamic byte offset; registers: a dynamic element */
  /* registers with dynamic indices: up to four, each `rcnt` elements of `rstr` leaves; the indices make one flat number in dyn_reg (the first the most significant digit), and
   * candidate f's leaves start at leaf0 + the sum of its digits times their strides. rcount: the candidates (the product of the counts). */
  uint32_t rdims, rcnt[4], rstr[4], rcount;
  uint32_t cdyn_on, cdyn_reg, cdyn_comp;  /* registers: a dynamic component of a vector leaf */
  uint32_t image_res, image_unit_off;     /* an image / sampler / sampled image: the resource and the element */
  uint32_t sampler_res;
  /* a pointer into memory (a storage buffer or Workgroup memory): byte_off and the dynamic offset dyn_* are in bytes */
  sp_u8 is_mem, mem_file, res_array, pad2;   /* mem_file: OF_UAV, OF_RES (read only) or OF_TGSM; res_array: the next index picks the buffer of an array of storage buffers */
  uint32_t mem_idx;                       /* the register: u# / t# / g# */
} Val;

enum { T_VOID = 1, T_BOOL, T_INT, T_FLOAT, T_VEC, T_MAT, T_ARRAY, T_STRUCT, T_PTR, T_IMAGE, T_SAMPLER, T_SAMPLED, T_FUNC };
typedef struct Type {
  sp_u8 kind, width, sign, cls;
  uint32_t count, elem, storage, nmem;
  uint32_t *mem;
  uint32_t dim, arrayed, depth, sampled, ms;
  int32_t nleaf;            /* -1 until computed */
} Type;

typedef struct Deco {
  uint32_t id, member, deco, arg;
} Deco;

typedef struct Block {
  uint32_t label, start, term;     /* word offsets: the first instruction after OpLabel, the terminator */
  uint32_t merge_kind, merge, cont;   /* OpSelectionMerge (1) / OpLoopMerge (2) before the terminator */
  uint32_t npred, loop_header_seen;
  uint32_t order;
} Block;

typedef struct Func {
  uint32_t id, offset, nblocks;
  uint32_t first_label;
  uint32_t nparams, ret_type;
  uint32_t nreturns, last_label;
} Func;

/* (the kernel's DXBC front end takes 65536 instructions and 256 temporaries: D3d9Program::kMaxInstructions, d3d9::kMaxTemps) */
#define SP_MAX_CODE 60000
#define SP_MAX_NEST 64      /* constructs (loops, selections) one inside another */
#define SP_MAX_CALLS 64     /* function calls one inside another (they are inlined) */

/* A DXBC loop being written (a real loop, or a loop that runs once so that its `break` can leave a construct: a switch, the continue construct of a loop, a function with several
 * returns). Leaving it from inside is `break`; from inside other loops too, a flag per target loop that every loop in between tests after its endloop. */
#define RET_TARGET 0xFFFFFFFEu
typedef struct Ctx {
  uint32_t exit_target;     /* a branch to this block (RET_TARGET: a return) leaves the loop */
  uint32_t flag;            /* virtual temporary: set when the exit was taken from inside a loop within this one */
  uint32_t crossed[SP_MAX_NEST], ncrossed;   /* the flags of outer loops to test after this loop's endloop */
} Ctx;

typedef struct Frame {
  uint32_t func, wrapper;
  Val *ret;
} Frame;

typedef struct Range {
  uint32_t begin, end;
} Range;

#define SP_MAX_ID_LEAVES 192
#define SP_MAX_RES 32
#define SP_ERR_LEN 160

struct SpvModule {
  SpvAllocator a;
  void *blocks;
  sp_u8 *cur;
  sp_u64 left;
  const uint32_t *w;
  uint32_t nw, bound;
  Type **types;
  Val **vals;
  uint32_t *inst;           /* by id: the word offset of the defining instruction (0: none) */
  Deco *decos;
  uint32_t ndeco, cap_deco;
  const uint32_t *spec_ids, *spec_values;
  uint32_t spec_count;
  char error[SP_ERR_LEN];
  /* reflection */
  uint32_t stage;
  uint32_t entry_func, entry_name_ok;
  uint32_t interface_ids[64], ninterface;
  SpvResource res[SP_MAX_RES];
  uint32_t res_var[SP_MAX_RES];
  uint32_t nres;
  uint32_t input_mask, output_mask, frag_coord, uses_vertex_id, uses_instance_id, depth_replacing;
  uint32_t local_size[3], local_size_id[3];
  uint32_t shared_bytes;    /* Workgroup variables, all in g0 */
  uint32_t uav_used, srv_used, system_used;   /* registers u# / t# of storage buffers used, compute system values (bit OF_* - OF_THREAD_ID) */
  uint32_t uses_memory;     /* raw buffers: Shader Model 5 */
  /* functions */
  Func *funcs;
  uint32_t nfuncs;
  Block **blk;              /* by label id */
  /* generation */
  SpIns *code;
  uint32_t ncode, cap_code;
  uint32_t nvreg;
  sp_u8 *vreg_var;
  uint32_t cap_vreg;
  Range *loops;
  uint32_t nloops, cap_loops;
  Ctx cstack[SP_MAX_NEST];
  uint32_t ncstack;
  Frame fstack[SP_MAX_CALLS];
  uint32_t nfstack;
  uint32_t used_samplers;
  uint32_t ncb;
  uint32_t cb_slot_used[16];
  uint32_t ps_input_mask_regs;
  uint32_t out_regs_used;   /* bits of the output registers written */
  uint32_t temps_used;
  sp_u8 *out;
  uint32_t out_size;
  uint32_t failed;
  /* unit state for the writer */
  uint32_t image_dim_by_unit[8], image_depth_by_unit[8], unit_used[8], nunits;
  uint32_t steps;
  sp_u8 in_decl[16], out_decl[16];
  uint32_t cb_decl[16];
};
typedef struct SpvModule Mod;

/* ---- memory ------------------------------------------------------------------------------------------------------------------------------------------------------------ */
static void *sp_alloc(Mod *m, sp_u64 n) {
  n = (n + 15) & ~(sp_u64)15;
  if (m->failed) return 0;
  if (n > m->left) {
    sp_u64 size = n + 16 > 65536 ? n + 16 : 65536;
    sp_u8 *block = (sp_u8 *)m->a.alloc(m->a.user, size);
    if (!block) {
      m->failed = 1;
      return 0;
    }
    *(void **)block = m->blocks;
    m->blocks = block;
    m->cur = block + 16;
    m->left = size - 16;
  }
  void *p = m->cur;
  m->cur += n;
  m->left -= n;
  return p;
}

static int sp_fail(Mod *m, const char *what) {
  if (m->error[0]) return 1;
  uint32_t i = 0;
  for (; what[i] && i + 1 < SP_ERR_LEN; ++i) m->error[i] = what[i];
  m->error[i] = 0;
  m->failed = 1;
  return 1;
}
static int sp_fail2(Mod *m, const char *what, uint32_t number) {
  char buffer[SP_ERR_LEN];
  uint32_t n = 0;
  for (; what[n] && n < 120; ++n) buffer[n] = what[n];
  buffer[n++] = ' ';
  char digits[12];
  int d = 0;
  do {
    digits[d++] = (char)('0' + number % 10);
    number /= 10;
  } while (number);
  while (d) buffer[n++] = digits[--d];
  buffer[n] = 0;
  return sp_fail(m, buffer);
}

#define OPC(off) (m->w[off] & 0xFFFF)
#define LEN(off) (m->w[off] >> 16)

static Type *get_type(Mod *m, uint32_t id) { return id < m->bound ? m->types[id] : 0; }
static void set_val(Mod *m, uint32_t id, Val *v) {
  if (id < m->bound) m->vals[id] = v;
  else sp_fail(m, "result id out of range");
}

static int deco_find(Mod *m, uint32_t id, uint32_t member, uint32_t deco, uint32_t *arg) {
  for (uint32_t i = 0; i < m->ndeco; ++i)
    if (m->decos[i].id == id && m->decos[i].member == member && m->decos[i].deco == deco) {
      if (arg) *arg = m->decos[i].arg;
      return 1;
    }
  return 0;
}

static void add_deco(Mod *m, uint32_t id, uint32_t member, uint32_t deco, uint32_t arg) {
  if (m->ndeco == m->cap_deco) {
    uint32_t cap = m->cap_deco ? m->cap_deco * 2 : 64;
    Deco *n = (Deco *)sp_alloc(m, (sp_u64)cap * sizeof(Deco));
    if (!n) return;
    if (m->decos) sp_copy(n, m->decos, (sp_u64)m->ndeco * sizeof(Deco));
    m->decos = n;
    m->cap_deco = cap;
  }
  Deco *d = &m->decos[m->ndeco++];
  d->id = id, d->member = member, d->deco = deco, d->arg = arg;
}

/* ---- types ------------------------------------------------------------------------------------------------------------------------------------------------------------- */
static int leaves_of(Mod *m, uint32_t tid, Leaf *out, uint32_t *count, uint32_t cap) {
  Type *t = get_type(m, tid);
  if (!t) return sp_fail(m, "unknown type");
  switch (t->kind) {
    case T_BOOL: case T_INT: case T_FLOAT:
      if (*count >= cap) return sp_fail(m, "value too large");
      out[*count].n = 1, out[*count].cls = t->cls;
      ++*count;
      return 0;
    case T_VEC: {
      Type *e = get_type(m, t->elem);
      if (*count >= cap) return sp_fail(m, "value too large");
      out[*count].n = (sp_u8)t->count, out[*count].cls = e->cls;
      ++*count;
      return 0;
    }
    case T_MAT:
      for (uint32_t i = 0; i < t->count; ++i)
        if (leaves_of(m, t->elem, out, count, cap)) return 1;
      return 0;
    case T_ARRAY:
      for (uint32_t i = 0; i < t->count; ++i)
        if (leaves_of(m, t->elem, out, count, cap)) return 1;
      return 0;
    case T_STRUCT:
      for (uint32_t i = 0; i < t->nmem; ++i)
        if (leaves_of(m, t->mem[i], out, count, cap)) return 1;
      return 0;
    default:
      return sp_fail(m, "unsupported value type");
  }
}

static int type_nleaf(Mod *m, uint32_t tid) {
  Type *t = get_type(m, tid);
  if (!t) return -1;
  if (t->nleaf >= 0) return t->nleaf;
  int n = 0;
  switch (t->kind) {
    case T_BOOL: case T_INT: case T_FLOAT: case T_VEC: n = 1; break;
    case T_MAT: case T_ARRAY: {
      int e = type_nleaf(m, t->elem);
      if (e < 0) return -1;
      n = (int)(e * t->count);
      break;
    }
    case T_STRUCT:
      for (uint32_t i = 0; i < t->nmem; ++i) {
        int e = type_nleaf(m, t->mem[i]);
        if (e < 0) return -1;
        n += e;
      }
      break;
    default: n = 0; break;
  }
  if (n > SP_MAX_ID_LEAVES) {
    sp_fail(m, "composite value too large");
    return -1;
  }
  t->nleaf = n;
  return n;
}

/* The byte size of a type in a block (std140 / std430 as the decorations say). */
static uint32_t block_bytes(Mod *m, uint32_t tid, uint32_t owner, uint32_t member) {
  Type *t = get_type(m, tid);
  if (!t) return 0;
  switch (t->kind) {
    case T_BOOL: case T_INT: case T_FLOAT: return 4;
    case T_VEC: return 4 * t->count;
    case T_MAT: {
      uint32_t stride = 16;
      if (owner) deco_find(m, owner, member, DecoMatrixStride, &stride);
      return stride * t->count;
    }
    case T_ARRAY: {
      uint32_t stride = 0;
      deco_find(m, tid, 0xFFFFFFFFu, DecoArrayStride, &stride);
      if (!stride) stride = block_bytes(m, t->elem, 0, 0);
      return stride * t->count;
    }
    case T_STRUCT: {
      uint32_t end = 0;
      for (uint32_t i = 0; i < t->nmem; ++i) {
        uint32_t off = 0;
        deco_find(m, tid, i, DecoOffset, &off);
        uint32_t size = block_bytes(m, t->mem[i], tid, i);
        if (off + size > end) end = off + size;
      }
      return end;
    }
    default: return 0;
  }
}

/* The layout of memory: what the decorations say (storage buffers), or, where there are none (Workgroup variables), the members one after the other, 4-byte scalars, arrays and
 * matrix columns as long as their elements. */
static uint32_t mem_size(Mod *m, uint32_t tid, uint32_t owner, uint32_t member);
static uint32_t mem_array_stride(Mod *m, uint32_t tid) {
  Type *t = get_type(m, tid);
  uint32_t stride = 0;
  if (!t || deco_find(m, tid, 0xFFFFFFFFu, DecoArrayStride, &stride)) return stride;
  return mem_size(m, t->elem, 0, 0);
}
/* The stride of the columns of a matrix of type `tid` (0 from the caller: none was decorated). */
static uint32_t mem_column_stride(Mod *m, uint32_t tid, uint32_t mstride) {
  Type *t = get_type(m, tid);
  Type *col = t ? get_type(m, t->elem) : 0;
  return mstride ? mstride : col ? 4 * col->count : 16;
}
static uint32_t mem_member_offset(Mod *m, uint32_t tid, uint32_t index) {
  Type *t = get_type(m, tid);
  uint32_t off = 0, end = 0;
  if (deco_find(m, tid, index, DecoOffset, &off)) return off;
  for (uint32_t i = 0; t && i < index && i < t->nmem; ++i) {
    uint32_t at = end;
    deco_find(m, tid, i, DecoOffset, &at);
    end = at + mem_size(m, t->mem[i], tid, i);
  }
  return end;
}
static uint32_t mem_size(Mod *m, uint32_t tid, uint32_t owner, uint32_t member) {
  Type *t = get_type(m, tid);
  if (!t) return 0;
  switch (t->kind) {
    case T_BOOL: case T_INT: case T_FLOAT: return 4;
    case T_VEC: return 4 * t->count;
    case T_MAT: {
      uint32_t stride = 0;
      if (owner) deco_find(m, owner, member, DecoMatrixStride, &stride);
      return mem_column_stride(m, tid, stride) * t->count;
    }
    case T_ARRAY: return mem_array_stride(m, tid) * t->count;   /* (a runtime array: 0) */
    case T_STRUCT: {
      uint32_t end = 0;
      for (uint32_t i = 0; i < t->nmem; ++i) {
        uint32_t e = mem_member_offset(m, tid, i) + mem_size(m, t->mem[i], tid, i);
        if (e > end) end = e;
      }
      return end;
    }
    default: return 0;
  }
}

#include "spirv_parse.inc"
#include "spirv_gen.inc"
#include "spirv_flow.inc"
#include "spirv_out.inc"
