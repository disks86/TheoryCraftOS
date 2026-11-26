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

/* Part of d3d10.c: the Effects 10 framework - ID3D10Effect and everything it hands out, over the fx_4_0 / fx_4_1 binaries fx4.h reads.
 *
 * An effect keeps a copy of its binary (the names it hands out point into it), the constant buffers with their memory and a Direct3D 10 buffer each (written when a pass
 * that uses them is applied after a change), the object variables (shaders made on the device with the bytecode's reflection, state objects made from their
 * descriptions, the views a program sets), and the techniques' passes. What a pass or a state object takes from a variable or from an expression is worked out again each
 * time it is used: Apply, the Get*Desc of a pass, GetBackingStore. A child effect (D3D10_EFFECT_COMPILE_CHILD_EFFECT) has the variables of its pool too - after its own,
 * by index; by name where it has none of its own.
 *
 * Lookups that find nothing give "null" objects (IsValid FALSE, GetDesc E_FAIL, the getters harmless), as Windows does. A pool's interfaces answer only ID3D10EffectPool,
 * and an effect's QueryInterface answers ID3D10EffectPool only, giving the interface back: Windows' behaviour, which programs and Wine's tests rely on.
 *
 * d3d10_1.dll's effects (TcosD3D10_1CreateEffectFromMemory, through its .def) are the same with Direct3D 10.1's blend: in an fx_4_1 effect a blend state has a blend for
 * each render target (its description is D3D10_BLEND_DESC1, made with CreateBlendState1), whose defaults are ONE / ZERO. d3d10.dll refuses those (E_FAIL), as Windows'. */
#include "fx4.h"
#include "fx4_disasm.h"      /* (D3D10DisassembleEffect; shader.inc has defined DXD_REALLOC and DXD_FREE) */

#define D3DERR_INVALIDCALL ((HRESULT)0x8876086Cu)

typedef struct FxEffect FxEffect;
typedef struct FxVar FxVar;
typedef struct FxType FxType;
typedef struct FxTypeMember { const char *name, *semantic; UINT offset; FxType *type; } FxTypeMember;
struct FxType {
  const ID3D10EffectTypeVtbl *lpVtbl;
  BOOL null;
  const char *name;
  D3D10_SHADER_VARIABLE_TYPE base;
  D3D10_SHADER_VARIABLE_CLASS cls;
  UINT gs_so;                       /* (a geometry shader with stream output) */
  UINT elements, unpacked, stride, packed, member_count, columns, rows;
  FxType *element_type;             /* (an array type's elements' type: the same without the elements) */
  FxTypeMember *members;
  const Fx4Type *source;            /* (the type of the binary it is made of; NULL for a buffer's or an anonymous shader's) */
};
typedef struct FxAnnotations { UINT count; FxVar *items; } FxAnnotations;

/* What a pass or a state object takes from a variable or an expression (a dependency), worked out again when it is used. */
typedef struct FxDep {
  UINT id, index, op;
  FxVar *var;                       /* VAR, CONST_INDEX: the variable; INDEX_EXPRESSION: the array */
  UINT offset;                      /* (a numeric variable read from its float `offset / 4`) */
  const Fx4Preshader *expression;   /* INDEX_EXPRESSION, VALUE_EXPRESSION */
  FxVar **inputs;                   /* (the variables of the expression's constant table, in its order) */
  float *tables[FX4_TABLES];        /* (the expression's tables: literals, the variables' registers, the result, the temporaries) */
} FxDep;
typedef struct FxDeps { UINT count; FxDep *items; } FxDeps;

typedef struct FxBinding { D3D10_SHADER_INPUT_TYPE type; UINT point, count; FxVar *var; } FxBinding;
/* A shader of the effect: its code (in the effect's copy), the device's shader, its reflection, its input signature (a DXBC container of ISGN, for CreateInputLayout) and
 * the variables its resource bindings are. */
typedef struct FxShader {
  const BYTE *code;
  UINT size;
  D3D10_SHADER_VARIABLE_TYPE kind;
  IUnknown *object;                 /* ID3D10VertexShader / PixelShader / GeometryShader (NULL: no code - a NULL shader) */
  ID3D10ShaderReflection *reflection;
  ID3D10Blob *input_signature;
  const char *so_decl;
  BOOL inline_;
  UINT binding_count;
  FxBinding *bindings;
} FxShader;
typedef struct FxSamplerDesc { D3D10_SAMPLER_DESC desc; FxVar *texture; } FxSamplerDesc;
typedef union FxStateDesc { D3D10_RASTERIZER_DESC rs; D3D10_DEPTH_STENCIL_DESC ds; D3D10_BLEND_DESC blend; D3D10_BLEND_DESC1 blend1; FxSamplerDesc sampler; } FxStateDesc;
typedef struct FxState {
  FxStateDesc desc;
  FxStateDesc made;                 /* (the description its object was made of) */
  BOOL ten_one;                     /* (a blend state of Direct3D 10.1: desc.blend1) */
  IUnknown *object;                 /* the device's state object */
  UINT index;                       /* (its place among the effect's states of its kind: GetBlendState(index) ... count from it) */
  FxDeps deps;
} FxState;

/* Every variable: numbers in a constant buffer (their storage the buffer's memory), the buffer itself, objects, annotations - and the members and elements of each. */
enum { FXV_NUMERIC, FXV_BUFFER, FXV_STRING, FXV_RESOURCE, FXV_RTV, FXV_DSV, FXV_SHADER, FXV_STATE, FXV_OTHER };
struct FxVar {
  const void *lpVtbl;
  BOOL null;
  UINT kind;                        /* FXV_* */
  FxEffect *effect;
  FxVar *buffer;                    /* the constant buffer its numbers are in (the null buffer for objects) */
  FxType *type;
  const char *name, *semantic;
  UINT buffer_offset, flags, explicit_bind;
  FxVar *elements, *members;
  FxAnnotations annotations;
  /* (FXV_BUFFER) */
  BYTE *data;                       /* its memory, `size` bytes */
  UINT size;
  BOOL changed;
  ID3D10Buffer *d3d;                /* the buffer made for it (NULL: none - an empty buffer) */
  ID3D10ShaderResourceView *srv;    /* (a texture buffer's view of it) */
  ID3D10Buffer *user_buffer;        /* SetConstantBuffer / SetTextureBuffer: what is bound instead (references) */
  ID3D10ShaderResourceView *user_srv;
  /* (FXV_RESOURCE, FXV_RTV, FXV_DSV: the views of each element, owned by the array; an element's are a slot of its array's) */
  IUnknown **views;
  BOOL owns_views;
  /* (FXV_STRING: a copy of its own - Windows hands out a different pointer for each string, equal ones too) */
  const char *string;
  char *owned_string;
  /* (FXV_SHADER, FXV_STATE: an element's own) */
  FxShader *shader;
  FxState *state;
};

typedef struct FxPass {
  const ID3D10EffectPassVtbl *lpVtbl;
  BOOL null;
  struct FxTechnique *technique;
  const char *name;
  FxAnnotations annotations;
  FxDeps deps;
  FxVar *vs, *gs, *ps;              /* (the null shader variable: not set by the pass) */
  UINT vs_index, gs_index, ps_index;
  FxVar *rs, *ds, *blend;           /* (NULL: not set) */
  FxVar *rtv[D3D10_SIMULTANEOUS_RENDER_TARGET_COUNT];
  FxVar *dsv;
  UINT rtv_count;                   /* (render targets the pass sets: 1 + the highest index) */
  BOOL views_set;
  UINT stencil_ref, sample_mask;
  float blend_factor[4];
} FxPass;
typedef struct FxTechnique {
  const ID3D10EffectTechniqueVtbl *lpVtbl;
  BOOL null;
  FxEffect *effect;
  const char *name;
  FxAnnotations annotations;
  UINT pass_count;
  FxPass *passes;
} FxTechnique;

struct FxEffect {
  const ID3D10EffectVtbl *lpVtbl;
  const ID3D10EffectPoolVtbl *pool_vtbl;      /* (ID3D10EffectPool, of a pool) */
  LONG ref;
  ID3D10Device *device;
  FxEffect *pool;                   /* (a child effect's pool: a reference) */
  BOOL is_pool, optimized, ten_one; /* (ten_one: made by d3d10_1.dll, Direct3D 10.1's blend for fx_4_1) */
  BOOL level_10_0;                  /* (the device's feature level is 10.0: a 10.1 blend state is made as Direct3D 10's, when it can be) */
  BYTE *copy;                       /* the binary (what fx and the names point into) */
  Fx4Effect fx;
  UINT type_count;
  FxType *types;                    /* (a type of the effect for each of the binary's) */
  UINT buffer_count;
  FxVar *buffers;
  FxType *buffer_types;
  UINT object_count;
  FxVar *objects;
  UINT anonymous_count;
  FxVar *anonymous;                 /* (anonymous shaders: "$Anonymous" variables of their own type) */
  FxType *anonymous_types;
  UINT technique_count;
  FxTechnique *techniques;
  /* the shaders and state objects in the order the binary has them (what GetShaderDesc(index) ... count from) */
  UINT shader_count, blend_count, ds_count, rs_count, sampler_count;
  FxVar **shader_list, **blend_list, **ds_list, **rs_list, **sampler_list;
  UINT shader_used, blend_used, ds_used, rs_used, sampler_used;
};

extern const ID3D10EffectVtbl fx_vtbl, fxpe_vtbl;
extern const ID3D10EffectPoolVtbl fxpool_vtbl;
extern const ID3D10EffectTechniqueVtbl fxt_vtbl;
extern const ID3D10EffectPassVtbl fxpass_vtbl;
extern const ID3D10EffectTypeVtbl fxtype_vtbl;
extern const ID3D10EffectVariableVtbl fxv_vtbl;
extern const ID3D10EffectConstantBufferVtbl fxcb_vtbl;
extern const ID3D10EffectScalarVariableVtbl fxs_vtbl;
extern const ID3D10EffectVectorVariableVtbl fxvec_vtbl;
extern const ID3D10EffectMatrixVariableVtbl fxm_vtbl;
extern const ID3D10EffectStringVariableVtbl fxstr_vtbl;
extern const ID3D10EffectShaderResourceVariableVtbl fxsrv_vtbl;
extern const ID3D10EffectRenderTargetViewVariableVtbl fxrtv_vtbl;
extern const ID3D10EffectDepthStencilViewVariableVtbl fxdsv_vtbl;
extern const ID3D10EffectShaderVariableVtbl fxsh_vtbl;
extern const ID3D10EffectBlendVariableVtbl fxblend_vtbl;
extern const ID3D10EffectDepthStencilVariableVtbl fxds_vtbl;
extern const ID3D10EffectRasterizerVariableVtbl fxrs_vtbl;
extern const ID3D10EffectSamplerVariableVtbl fxsamp_vtbl;

/* ---- the null objects ------------------------------------------------------------------------------------------------------------------------------------------------- */

static FxType g_fx_null_type = {.lpVtbl = &fxtype_vtbl, .null = TRUE};
#define FX_NULL_VAR(vtbl, k) {.lpVtbl = (const void *)&(vtbl), .null = TRUE, .kind = (k), .type = &g_fx_null_type}
static FxVar g_fx_null_cb = FX_NULL_VAR(fxcb_vtbl, FXV_BUFFER);
static FxVar g_fx_null_var = FX_NULL_VAR(fxv_vtbl, FXV_OTHER);
static FxVar g_fx_null_scalar = FX_NULL_VAR(fxs_vtbl, FXV_NUMERIC);
static FxVar g_fx_null_vector = FX_NULL_VAR(fxvec_vtbl, FXV_NUMERIC);
static FxVar g_fx_null_matrix = FX_NULL_VAR(fxm_vtbl, FXV_NUMERIC);
static FxVar g_fx_null_string = FX_NULL_VAR(fxstr_vtbl, FXV_STRING);
static FxVar g_fx_null_srv = FX_NULL_VAR(fxsrv_vtbl, FXV_RESOURCE);
static FxVar g_fx_null_rtv = FX_NULL_VAR(fxrtv_vtbl, FXV_RTV);
static FxVar g_fx_null_dsv = FX_NULL_VAR(fxdsv_vtbl, FXV_DSV);
static FxVar g_fx_null_shader = FX_NULL_VAR(fxsh_vtbl, FXV_SHADER);
static FxVar g_fx_null_blend = FX_NULL_VAR(fxblend_vtbl, FXV_STATE);
static FxVar g_fx_null_ds = FX_NULL_VAR(fxds_vtbl, FXV_STATE);
static FxVar g_fx_null_rs = FX_NULL_VAR(fxrs_vtbl, FXV_STATE);
static FxVar g_fx_null_sampler = FX_NULL_VAR(fxsamp_vtbl, FXV_STATE);
static FxTechnique g_fx_null_technique = {.lpVtbl = &fxt_vtbl, .null = TRUE};
static FxPass g_fx_null_pass = {.lpVtbl = &fxpass_vtbl, .null = TRUE};

/* The shaders a pass sets to NULL (SetVertexShader(NULL) ...): valid variables of no shader, "$Anonymous", shared by every effect. */
static FxType g_fx_anon_types[3] = {
  {.lpVtbl = &fxtype_vtbl, .name = "vertexshader", .base = D3D10_SVT_VERTEXSHADER, .cls = D3D10_SVC_OBJECT},
  {.lpVtbl = &fxtype_vtbl, .name = "pixelshader", .base = D3D10_SVT_PIXELSHADER, .cls = D3D10_SVC_OBJECT},
  {.lpVtbl = &fxtype_vtbl, .name = "geometryshader", .base = D3D10_SVT_GEOMETRYSHADER, .cls = D3D10_SVC_OBJECT},
};
static FxShader g_fx_anon_shaders[3] = {{.kind = D3D10_SVT_VERTEXSHADER, .inline_ = TRUE}, {.kind = D3D10_SVT_PIXELSHADER, .inline_ = TRUE},
                                        {.kind = D3D10_SVT_GEOMETRYSHADER, .inline_ = TRUE}};
#define FX_ANON(k) {.lpVtbl = (const void *)&fxsh_vtbl, .kind = FXV_SHADER, .buffer = &g_fx_null_cb, .type = &g_fx_anon_types[k], .name = "$Anonymous", \
                    .explicit_bind = ~0u, .shader = &g_fx_anon_shaders[k]}
static FxVar g_fx_anon[3] = {FX_ANON(0), FX_ANON(1), FX_ANON(2)};
#undef FX_ANON

/* ---- small helpers ------------------------------------------------------------------------------------------------------------------------------------------------ */

static int fx_strcmp(const char *a, const char *b) {
  while (*a && *a == *b) { ++a; ++b; }
  return (unsigned char)*a - (unsigned char)*b;
}
static int fx_stricmp(const char *a, const char *b) {
  for (;; ++a, ++b) {
    unsigned char x = (unsigned char)*a, y = (unsigned char)*b;
    if (x >= 'A' && x <= 'Z') x += 32;
    if (y >= 'A' && y <= 'Z') y += 32;
    if (x != y || !x) return x - y;
  }
}
static BOOL fx_name_is(const char *name, const char *want) { return name && want && !fx_strcmp(name, want); }
static void *fx_alloc_cb(void *ctx, size_t n) { (void)ctx; return mem_alloc(n); }
static void fx_release_cb(void *ctx, void *p) { (void)ctx; mem_free(p); }
/* `count` zeroed items of `size` bytes (count 0: NULL, which is not a failure; the caller tells them apart by count). */
static void *fx_new_array(UINT count, SIZE_T size) {
  if (!count) return NULL;
  if (size && count > ((SIZE_T)-1) / size) return NULL;
  return mem_alloc((SIZE_T)count * size);
}
static void fx_release(void *slot_address) {
  IUnknown **slot = slot_address;
  if (*slot) { (*slot)->lpVtbl->Release(*slot); *slot = NULL; }
}
/* A string variable's value, as a copy of its own (FALSE: no memory). */
static BOOL fx_set_string(FxVar *v, const char *s) {
  v->string = NULL;
  if (!s) return TRUE;
  const SIZE_T n = alen(s) + 1;
  char *copy = mem_alloc(n);
  if (!copy) return FALSE;
  memcpy(copy, s, n);
  v->owned_string = copy;
  v->string = copy;
  return TRUE;
}
static FxVar *fx_element(FxVar *v, UINT index) { return v->type->elements && v->elements ? &v->elements[index < v->type->elements ? index : 0] : v; }

/* ---- types --------------------------------------------------------------------------------------------------------------------------------------------------------------- */

static D3D10_SHADER_VARIABLE_TYPE fx_object_type(UINT object) {
  switch (object) {
    case FX4_OBJ_STRING: return D3D10_SVT_STRING;
    case FX4_OBJ_BLEND: return D3D10_SVT_BLEND;
    case FX4_OBJ_DEPTHSTENCIL: return D3D10_SVT_DEPTHSTENCIL;
    case FX4_OBJ_RASTERIZER: return D3D10_SVT_RASTERIZER;
    case FX4_OBJ_PIXELSHADER: return D3D10_SVT_PIXELSHADER;
    case FX4_OBJ_VERTEXSHADER: return D3D10_SVT_VERTEXSHADER;
    case FX4_OBJ_GEOMETRYSHADER: case FX4_OBJ_GEOMETRYSHADER_SO: return D3D10_SVT_GEOMETRYSHADER;
    case FX4_OBJ_TEXTURE: return D3D10_SVT_TEXTURE;
    case FX4_OBJ_TEXTURE1D: return D3D10_SVT_TEXTURE1D;
    case FX4_OBJ_TEXTURE1DARRAY: return D3D10_SVT_TEXTURE1DARRAY;
    case FX4_OBJ_TEXTURE2D: return D3D10_SVT_TEXTURE2D;
    case FX4_OBJ_TEXTURE2DARRAY: return D3D10_SVT_TEXTURE2DARRAY;
    case FX4_OBJ_TEXTURE2DMS: return D3D10_SVT_TEXTURE2DMS;
    case FX4_OBJ_TEXTURE2DMSARRAY: return D3D10_SVT_TEXTURE2DMSARRAY;
    case FX4_OBJ_TEXTURE3D: return D3D10_SVT_TEXTURE3D;
    case FX4_OBJ_TEXTURECUBE: return D3D10_SVT_TEXTURECUBE;
    case FX4_OBJ_TEXTURECUBEARRAY: return D3D10_SVT_TEXTURECUBEARRAY;
    case FX4_OBJ_RENDERTARGETVIEW: return D3D10_SVT_RENDERTARGETVIEW;
    case FX4_OBJ_DEPTHSTENCILVIEW: return D3D10_SVT_DEPTHSTENCILVIEW;
    case FX4_OBJ_SAMPLER: return D3D10_SVT_SAMPLER;
    case FX4_OBJ_BUFFER: return D3D10_SVT_BUFFER;
    default: return D3D10_SVT_VOID;
  }
}
static D3D10_SHADER_VARIABLE_TYPE fx_numeric_type(UINT base) {
  return base == FX4_BASE_FLOAT ? D3D10_SVT_FLOAT : base == FX4_BASE_INT ? D3D10_SVT_INT : base == FX4_BASE_UINT ? D3D10_SVT_UINT : D3D10_SVT_BOOL;
}
static FxType *fx_type_of(FxEffect *e, const Fx4Type *t) {
  for (UINT i = 0; i < e->type_count; ++i) if (e->types[i].source == t) return &e->types[i];
  return &g_fx_null_type;
}
/* The effect's type of each type of the binary (fx4 has them all, members' too: an array of them, and the element types of arrays). */
static BOOL fx_make_types(FxEffect *e) {
  e->type_count = e->fx.type_count;
  if (!e->type_count) return TRUE;
  if (!(e->types = fx_new_array(e->type_count, sizeof *e->types * 2))) return FALSE;      /* (each with room for its element type after the list) */
  for (UINT i = 0; i < e->type_count; ++i) {
    const Fx4Type *s = e->fx.types[i];
    FxType *t = &e->types[i];
    t->lpVtbl = &fxtype_vtbl;
    t->source = s;
    t->name = s->name;
    t->elements = s->elements;
    t->unpacked = s->unpacked;
    t->stride = s->stride;
    t->packed = s->packed;
    if (s->kind == FX4_KIND_NUMERIC) {
      t->base = fx_numeric_type(s->numeric_base);
      t->cls = s->numeric_class == FX4_CLASS_SCALAR ? D3D10_SVC_SCALAR : s->numeric_class == FX4_CLASS_VECTOR ? D3D10_SVC_VECTOR :
               s->column_major ? D3D10_SVC_MATRIX_COLUMNS : D3D10_SVC_MATRIX_ROWS;
      t->rows = s->rows;
      t->columns = s->columns;
    } else if (s->kind == FX4_KIND_OBJECT) {
      t->base = fx_object_type(s->object);
      t->cls = D3D10_SVC_OBJECT;
      t->gs_so = s->object == FX4_OBJ_GEOMETRYSHADER_SO;
    } else {
      t->base = D3D10_SVT_VOID;
      t->cls = D3D10_SVC_STRUCT;
      t->member_count = s->member_count;
    }
  }
  for (UINT i = 0; i < e->type_count; ++i) {
    FxType *t = &e->types[i];
    const Fx4Type *s = t->source;
    if (t->member_count) {
      if (!(t->members = fx_new_array(t->member_count, sizeof *t->members))) return FALSE;
      for (UINT m = 0; m < t->member_count; ++m) {
        t->members[m].name = s->members[m].name;
        t->members[m].semantic = s->members[m].semantic;
        t->members[m].offset = s->members[m].offset;
        t->members[m].type = fx_type_of(e, s->members[m].type);
      }
    }
    if (t->elements) {
      /* (an element: the same type with no elements, its sizes a share of the array's) */
      FxType *el = &e->types[e->type_count + i];
      *el = *t;
      el->source = NULL;
      el->elements = 0;
      el->unpacked = el->packed = t->packed / t->elements;
      t->element_type = el;
    }
  }
  return TRUE;
}

/* ---- variables: numbers ------------------------------------------------------------------------------------------------------------------------------------------------- */

static const void *fx_numeric_vtbl(const FxType *t) {
  switch (t->cls) {
    case D3D10_SVC_SCALAR: return &fxs_vtbl;
    case D3D10_SVC_VECTOR: return &fxvec_vtbl;
    case D3D10_SVC_MATRIX_ROWS: case D3D10_SVC_MATRIX_COLUMNS: return &fxm_vtbl;
    default: return &fxv_vtbl;
  }
}
static void fx_object_kind(FxVar *v) {
  switch (v->type->base) {
    case D3D10_SVT_STRING: v->lpVtbl = &fxstr_vtbl; v->kind = FXV_STRING; break;
    case D3D10_SVT_TEXTURE: case D3D10_SVT_TEXTURE1D: case D3D10_SVT_TEXTURE1DARRAY: case D3D10_SVT_TEXTURE2D: case D3D10_SVT_TEXTURE2DARRAY: case D3D10_SVT_TEXTURE2DMS:
    case D3D10_SVT_TEXTURE2DMSARRAY: case D3D10_SVT_TEXTURE3D: case D3D10_SVT_TEXTURECUBE: case D3D10_SVT_TEXTURECUBEARRAY: case D3D10_SVT_BUFFER:
      v->lpVtbl = &fxsrv_vtbl; v->kind = FXV_RESOURCE; break;
    case D3D10_SVT_RENDERTARGETVIEW: v->lpVtbl = &fxrtv_vtbl; v->kind = FXV_RTV; break;
    case D3D10_SVT_DEPTHSTENCILVIEW: v->lpVtbl = &fxdsv_vtbl; v->kind = FXV_DSV; break;
    case D3D10_SVT_VERTEXSHADER: case D3D10_SVT_PIXELSHADER: case D3D10_SVT_GEOMETRYSHADER: v->lpVtbl = &fxsh_vtbl; v->kind = FXV_SHADER; break;
    case D3D10_SVT_BLEND: v->lpVtbl = &fxblend_vtbl; v->kind = FXV_STATE; break;
    case D3D10_SVT_DEPTHSTENCIL: v->lpVtbl = &fxds_vtbl; v->kind = FXV_STATE; break;
    case D3D10_SVT_RASTERIZER: v->lpVtbl = &fxrs_vtbl; v->kind = FXV_STATE; break;
    case D3D10_SVT_SAMPLER: v->lpVtbl = &fxsamp_vtbl; v->kind = FXV_STATE; break;
    default: v->lpVtbl = &fxv_vtbl; v->kind = FXV_OTHER; break;
  }
}
/* A variable's members and elements, with their places in the buffer (members at their offsets, elements a stride apart). */
static BOOL fx_var_tree(FxVar *v, UINT depth) {
  if (depth > FX4_MAX_DEPTH) return FALSE;
  if (v->type->elements) {
    if (!(v->elements = fx_new_array(v->type->elements, sizeof *v->elements))) return FALSE;
    for (UINT i = 0; i < v->type->elements; ++i) {
      FxVar *el = &v->elements[i];
      el->effect = v->effect;
      el->buffer = v->buffer;
      el->type = v->type->element_type;
      el->name = v->name;
      el->semantic = v->semantic;
      el->flags = v->flags;
      el->explicit_bind = ~0u;
      el->buffer_offset = v->buffer_offset + i * v->type->stride;
      if (el->type->cls == D3D10_SVC_OBJECT) fx_object_kind(el);
      else { el->lpVtbl = fx_numeric_vtbl(el->type); el->kind = FXV_NUMERIC; }
      if (!fx_var_tree(el, depth + 1)) return FALSE;
    }
  }
  if (v->type->member_count) {
    if (!(v->members = fx_new_array(v->type->member_count, sizeof *v->members))) return FALSE;
    for (UINT i = 0; i < v->type->member_count; ++i) {
      FxVar *m = &v->members[i];
      const FxTypeMember *tm = &v->type->members[i];
      m->effect = v->effect;
      m->buffer = v->buffer;
      m->type = tm->type;
      m->name = tm->name;
      m->semantic = tm->semantic;
      m->flags = v->flags & D3D10_EFFECT_VARIABLE_POOLED;
      m->explicit_bind = ~0u;
      m->buffer_offset = v->buffer_offset + tm->offset;
      m->lpVtbl = fx_numeric_vtbl(m->type);
      m->kind = FXV_NUMERIC;
      if (!fx_var_tree(m, depth + 1)) return FALSE;
    }
  }
  return TRUE;
}
static void fx_var_tree_free(FxVar *v) {
  if (v->elements) { for (UINT i = 0; i < v->type->elements; ++i) fx_var_tree_free(&v->elements[i]); mem_free(v->elements); }
  if (v->members) { for (UINT i = 0; i < v->type->member_count; ++i) fx_var_tree_free(&v->members[i]); mem_free(v->members); }
  v->elements = v->members = NULL;
}

/* A numeric variable's memory: its buffer's bytes at its offset (the buffer checked by fx4 to hold its type's footprint). NULL for one with none (the null variables). */
static BYTE *fx_storage(const FxVar *v) { return v->buffer && v->buffer->data ? v->buffer->data + v->buffer_offset : NULL; }
static void fx_touch(FxVar *v) { if (v->buffer) v->buffer->changed = TRUE; }

/* (the conversions of Get/Set: what Windows does, a boolean being 0 or ~0) */
static BOOL fx_as_bool(UINT bits, D3D10_SHADER_VARIABLE_TYPE from) {
  return (from == D3D10_SVT_FLOAT || from == D3D10_SVT_INT || from == D3D10_SVT_UINT || from == D3D10_SVT_BOOL) && bits ? -1 : 0;
}
static int fx_as_int(UINT bits, D3D10_SHADER_VARIABLE_TYPE from) {
  switch (from) {
    case D3D10_SVT_FLOAT: return fx4_f2i(fx4_bits_float(bits));
    case D3D10_SVT_INT: case D3D10_SVT_UINT: return (int)bits;
    case D3D10_SVT_BOOL: return fx_as_bool(bits, from);
    default: return 0;
  }
}
static float fx_as_float(UINT bits, D3D10_SHADER_VARIABLE_TYPE from) {
  switch (from) {
    case D3D10_SVT_FLOAT: return fx4_bits_float(bits);
    case D3D10_SVT_INT: case D3D10_SVT_UINT: return (float)(int)bits;
    case D3D10_SVT_BOOL: return (float)fx_as_bool(bits, from);
    default: return 0.0f;
  }
}
static UINT fx_convert(UINT bits, D3D10_SHADER_VARIABLE_TYPE from, D3D10_SHADER_VARIABLE_TYPE to) {
  if (from == to) return bits;
  switch (to) {
    case D3D10_SVT_FLOAT: return fx4_float_bits(fx_as_float(bits, from));
    case D3D10_SVT_INT: case D3D10_SVT_UINT: return (UINT)fx_as_int(bits, from);
    case D3D10_SVT_BOOL: return (UINT)fx_as_bool(bits, from);
    default: return 0;
  }
}
static UINT fx_get32(const BYTE *p) { UINT v; memcpy(&v, p, 4); return v; }
static void fx_put32(BYTE *p, UINT v) { memcpy(p, &v, 4); }
/* `count` values (a scalar's 1, a vector's columns) between the program's memory (`values`, of type `as`) and the variable's at `place`. */
static void fx_move(BYTE *place, const FxType *t, void *values, D3D10_SHADER_VARIABLE_TYPE as, BOOL write) {
  BYTE *p = values;
  for (UINT i = 0; i < t->columns; ++i, p += 4) {
    if (write) fx_put32(place + 4 * i, fx_convert(fx_get32(p), as, t->base));
    else fx_put32(p, fx_convert(fx_get32(place + 4 * i), t->base, as));
  }
}
/* Get/Set of a scalar or vector variable, or of `count` elements of an array of them from `offset` (the program's values packed: an element after the other). */
static HRESULT fx_values(FxVar *v, void *values, D3D10_SHADER_VARIABLE_TYPE as, BOOL array, UINT offset, UINT count, BOOL write) {
  BYTE *place = fx_storage(v);
  if (v->null || !place) return E_FAIL;
  if (!values) return E_INVALIDARG;
  if (!array || !v->type->elements) {
    fx_move(place, v->type, values, as, write);
  } else {
    if (offset >= v->type->elements) return S_OK;
    if (count > v->type->elements - offset) count = v->type->elements - offset;
    BYTE *p = values;
    for (UINT i = 0; i < count; ++i, p += 4 * (SIZE_T)v->type->columns) fx_move(place + (SIZE_T)(offset + i) * v->type->stride, v->type, p, as, write);
  }
  if (write) fx_touch(v);
  return S_OK;
}
/* A matrix between the program's 4 x 4 floats (row major; `transpose`: column major) and the variable's registers (rows, or columns of a column-major matrix). */
static void fx_matrix_move(BYTE *place, const FxType *t, float m[16], BOOL transpose, BOOL write) {
  BOOL columns = t->cls == D3D10_SVC_MATRIX_COLUMNS;
  const UINT rows = transpose ? t->columns : t->rows, cols = transpose ? t->rows : t->columns;
  if (transpose) columns = !columns;
  for (UINT r = 0; r < rows; ++r)
    for (UINT c = 0; c < cols; ++c) {
      BYTE *at = place + 4 * (SIZE_T)(columns ? c * 4 + r : r * 4 + c);
      if (write) memcpy(at, &m[r * 4 + c], 4);
      else memcpy(&m[r * 4 + c], at, 4);
    }
}
static HRESULT fx_matrices(FxVar *v, float *data, BOOL array, UINT offset, UINT count, BOOL transpose, BOOL write) {
  BYTE *place = fx_storage(v);
  if (v->null || !place) return E_FAIL;
  if (!data) return E_INVALIDARG;
  if (!array || !v->type->elements) {
    fx_matrix_move(place, v->type, data, transpose, write);
  } else {
    if (offset >= v->type->elements) return S_OK;
    if (count > v->type->elements - offset) count = v->type->elements - offset;
    for (UINT i = 0; i < count; ++i) fx_matrix_move(place + (SIZE_T)(offset + i) * v->type->stride, v->type, data + 16 * (SIZE_T)i, transpose, write);
  }
  if (write) fx_touch(v);
  return S_OK;
}

/* ---- ID3D10EffectType ------------------------------------------------------------------------------------------------------------------------------------------------ */

static BOOL STDMETHODCALLTYPE fxtype_IsValid(FxType *t) { return !t->null; }
static HRESULT STDMETHODCALLTYPE fxtype_GetDesc(FxType *t, D3D10_EFFECT_TYPE_DESC *desc) {
  if (t->null) return E_FAIL;
  if (!desc) return E_INVALIDARG;
  desc->TypeName = t->name;
  desc->Class = t->cls;
  desc->Type = t->base;
  desc->Elements = t->elements;
  desc->Members = t->member_count;
  desc->Rows = t->rows;
  desc->Columns = t->columns;
  desc->PackedSize = t->packed;
  desc->UnpackedSize = t->unpacked;
  desc->Stride = t->stride;
  return S_OK;
}
static ID3D10EffectType *STDMETHODCALLTYPE fxtype_GetMemberTypeByIndex(FxType *t, UINT index) {
  return (ID3D10EffectType *)(index < t->member_count ? t->members[index].type : &g_fx_null_type);
}
static ID3D10EffectType *STDMETHODCALLTYPE fxtype_GetMemberTypeByName(FxType *t, const char *name) {
  for (UINT i = 0; name && i < t->member_count; ++i) if (fx_name_is(t->members[i].name, name)) return (ID3D10EffectType *)t->members[i].type;
  return (ID3D10EffectType *)&g_fx_null_type;
}
static ID3D10EffectType *STDMETHODCALLTYPE fxtype_GetMemberTypeBySemantic(FxType *t, const char *semantic) {
  for (UINT i = 0; semantic && i < t->member_count; ++i)
    if (t->members[i].semantic && !fx_stricmp(t->members[i].semantic, semantic)) return (ID3D10EffectType *)t->members[i].type;
  return (ID3D10EffectType *)&g_fx_null_type;
}
static const char *STDMETHODCALLTYPE fxtype_GetMemberName(FxType *t, UINT index) { return index < t->member_count ? t->members[index].name : NULL; }
static const char *STDMETHODCALLTYPE fxtype_GetMemberSemantic(FxType *t, UINT index) { return index < t->member_count ? t->members[index].semantic : NULL; }

/* ---- ID3D10EffectVariable: what every variable has ------------------------------------------------------------------------------------------------------------------- */

static FxVar *fx_annotation_by_index(const FxAnnotations *a, UINT index) { return index < a->count ? &a->items[index] : &g_fx_null_var; }
static FxVar *fx_annotation_by_name(const FxAnnotations *a, const char *name) {
  for (UINT i = 0; name && i < a->count; ++i) if (fx_name_is(a->items[i].name, name)) return &a->items[i];
  return &g_fx_null_var;
}

static BOOL STDMETHODCALLTYPE fxv_IsValid(FxVar *v) { return !v->null; }
static ID3D10EffectType *STDMETHODCALLTYPE fxv_GetType(FxVar *v) { return (ID3D10EffectType *)v->type; }
static HRESULT STDMETHODCALLTYPE fxv_GetDesc(FxVar *v, D3D10_EFFECT_VARIABLE_DESC *desc) {
  if (v->null) return E_FAIL;
  if (!desc) return E_INVALIDARG;
  memset(desc, 0, sizeof *desc);
  desc->Name = v->name;
  desc->Semantic = v->semantic;
  desc->Flags = v->flags;
  desc->Annotations = v->annotations.count;
  desc->BufferOffset = v->kind == FXV_NUMERIC ? v->buffer_offset : 0;
  if (v->flags & D3D10_EFFECT_VARIABLE_EXPLICIT_BIND_POINT) desc->ExplicitBindPoint = v->explicit_bind;
  return S_OK;
}
static ID3D10EffectVariable *STDMETHODCALLTYPE fxv_GetAnnotationByIndex(FxVar *v, UINT index) { return (ID3D10EffectVariable *)fx_annotation_by_index(&v->annotations, index); }
static ID3D10EffectVariable *STDMETHODCALLTYPE fxv_GetAnnotationByName(FxVar *v, const char *name) { return (ID3D10EffectVariable *)fx_annotation_by_name(&v->annotations, name); }
static ID3D10EffectVariable *STDMETHODCALLTYPE fxv_GetMemberByIndex(FxVar *v, UINT index) {
  return (ID3D10EffectVariable *)(v->members && index < v->type->member_count ? &v->members[index] : &g_fx_null_var);
}
static ID3D10EffectVariable *STDMETHODCALLTYPE fxv_GetMemberByName(FxVar *v, const char *name) {
  for (UINT i = 0; v->members && name && i < v->type->member_count; ++i) if (fx_name_is(v->members[i].name, name)) return (ID3D10EffectVariable *)&v->members[i];
  return (ID3D10EffectVariable *)&g_fx_null_var;
}
static ID3D10EffectVariable *STDMETHODCALLTYPE fxv_GetMemberBySemantic(FxVar *v, const char *semantic) {
  for (UINT i = 0; v->members && semantic && i < v->type->member_count; ++i)
    if (v->members[i].semantic && !fx_stricmp(v->members[i].semantic, semantic)) return (ID3D10EffectVariable *)&v->members[i];
  return (ID3D10EffectVariable *)&g_fx_null_var;
}
static ID3D10EffectVariable *STDMETHODCALLTYPE fxv_GetElement(FxVar *v, UINT index) {
  return (ID3D10EffectVariable *)(v->elements && index < v->type->elements ? &v->elements[index] : &g_fx_null_var);
}
static ID3D10EffectConstantBuffer *STDMETHODCALLTYPE fxv_GetParentConstantBuffer(FxVar *v) {
  return (ID3D10EffectConstantBuffer *)(v->buffer ? v->buffer : &g_fx_null_cb);
}
static ID3D10EffectScalarVariable *STDMETHODCALLTYPE fxv_AsScalar(FxVar *v) { return (ID3D10EffectScalarVariable *)(v->lpVtbl == (const void *)&fxs_vtbl ? v : &g_fx_null_scalar); }
static ID3D10EffectVectorVariable *STDMETHODCALLTYPE fxv_AsVector(FxVar *v) { return (ID3D10EffectVectorVariable *)(v->lpVtbl == (const void *)&fxvec_vtbl ? v : &g_fx_null_vector); }
static ID3D10EffectMatrixVariable *STDMETHODCALLTYPE fxv_AsMatrix(FxVar *v) { return (ID3D10EffectMatrixVariable *)(v->lpVtbl == (const void *)&fxm_vtbl ? v : &g_fx_null_matrix); }
static ID3D10EffectStringVariable *STDMETHODCALLTYPE fxv_AsString(FxVar *v) { return (ID3D10EffectStringVariable *)(v->lpVtbl == (const void *)&fxstr_vtbl ? v : &g_fx_null_string); }
static ID3D10EffectShaderResourceVariable *STDMETHODCALLTYPE fxv_AsShaderResource(FxVar *v) { return (ID3D10EffectShaderResourceVariable *)(v->lpVtbl == (const void *)&fxsrv_vtbl ? v : &g_fx_null_srv); }
static ID3D10EffectRenderTargetViewVariable *STDMETHODCALLTYPE fxv_AsRenderTargetView(FxVar *v) { return (ID3D10EffectRenderTargetViewVariable *)(v->lpVtbl == (const void *)&fxrtv_vtbl ? v : &g_fx_null_rtv); }
static ID3D10EffectDepthStencilViewVariable *STDMETHODCALLTYPE fxv_AsDepthStencilView(FxVar *v) { return (ID3D10EffectDepthStencilViewVariable *)(v->lpVtbl == (const void *)&fxdsv_vtbl ? v : &g_fx_null_dsv); }
static ID3D10EffectConstantBuffer *STDMETHODCALLTYPE fxv_AsConstantBuffer(FxVar *v) { return (ID3D10EffectConstantBuffer *)(v->lpVtbl == (const void *)&fxcb_vtbl ? v : &g_fx_null_cb); }
static ID3D10EffectShaderVariable *STDMETHODCALLTYPE fxv_AsShader(FxVar *v) { return (ID3D10EffectShaderVariable *)(v->lpVtbl == (const void *)&fxsh_vtbl ? v : &g_fx_null_shader); }
static ID3D10EffectBlendVariable *STDMETHODCALLTYPE fxv_AsBlend(FxVar *v) { return (ID3D10EffectBlendVariable *)(v->lpVtbl == (const void *)&fxblend_vtbl ? v : &g_fx_null_blend); }
static ID3D10EffectDepthStencilVariable *STDMETHODCALLTYPE fxv_AsDepthStencil(FxVar *v) { return (ID3D10EffectDepthStencilVariable *)(v->lpVtbl == (const void *)&fxds_vtbl ? v : &g_fx_null_ds); }
static ID3D10EffectRasterizerVariable *STDMETHODCALLTYPE fxv_AsRasterizer(FxVar *v) { return (ID3D10EffectRasterizerVariable *)(v->lpVtbl == (const void *)&fxrs_vtbl ? v : &g_fx_null_rs); }
static ID3D10EffectSamplerVariable *STDMETHODCALLTYPE fxv_AsSampler(FxVar *v) { return (ID3D10EffectSamplerVariable *)(v->lpVtbl == (const void *)&fxsamp_vtbl ? v : &g_fx_null_sampler); }
/* The bytes of a variable's numbers (a constant buffer's: its whole memory). Objects have none: D3DERR_INVALIDCALL; past the end: E_INVALIDARG. */
static HRESULT fx_raw(FxVar *v, void *data, UINT offset, UINT count, BOOL write) {
  if (v->null) return E_FAIL;
  BYTE *base;
  UINT size;
  if (v->kind == FXV_BUFFER) { base = v->data; size = v->size; }
  else if (v->kind == FXV_NUMERIC) {
    /* (from the variable on: its buffer's bytes past it too, as Windows reads and writes them) */
    base = fx_storage(v);
    size = base ? v->buffer->size - v->buffer_offset : 0;
  } else return D3DERR_INVALIDCALL;
  if (!count) return S_OK;
  if (!data || !base || offset > size || count > size - offset) return E_INVALIDARG;
  if (write) { memcpy(base + offset, data, count); if (v->kind == FXV_BUFFER) v->changed = TRUE; else fx_touch(v); }
  else memcpy(data, base + offset, count);
  return S_OK;
}
static HRESULT STDMETHODCALLTYPE fxv_SetRawValue(FxVar *v, void *data, UINT offset, UINT count) { return fx_raw(v, data, offset, count, TRUE); }
static HRESULT STDMETHODCALLTYPE fxv_GetRawValue(FxVar *v, void *data, UINT offset, UINT count) { return fx_raw(v, data, offset, count, FALSE); }

/* ---- ID3D10EffectConstantBuffer ----------------------------------------------------------------------------------------------------------------------------------------- */

static HRESULT STDMETHODCALLTYPE fxcb_SetConstantBuffer(FxVar *v, ID3D10Buffer *buffer) {
  if (v->null) return E_FAIL;
  if (v->type->base != D3D10_SVT_CBUFFER) return D3DERR_INVALIDCALL;
  /* (the program's buffer is bound in place of the effect's own, which is no longer written: NULL goes back to it) */
  if (buffer) ID3D10Buffer_AddRef(buffer);
  fx_release(&v->user_buffer);
  v->user_buffer = buffer;
  if (!buffer) v->changed = TRUE;
  return S_OK;
}
static HRESULT STDMETHODCALLTYPE fxcb_GetConstantBuffer(FxVar *v, ID3D10Buffer **out) {
  if (v->null) return E_FAIL;
  if (!out) return E_INVALIDARG;
  *out = NULL;
  if (v->type->base != D3D10_SVT_CBUFFER) return D3DERR_INVALIDCALL;
  *out = v->user_buffer ? v->user_buffer : v->d3d;
  if (*out) ID3D10Buffer_AddRef(*out);
  return S_OK;
}
static HRESULT STDMETHODCALLTYPE fxcb_SetTextureBuffer(FxVar *v, ID3D10ShaderResourceView *view) {
  if (v->null) return E_FAIL;
  if (v->type->base != D3D10_SVT_TBUFFER) return D3DERR_INVALIDCALL;
  if (view) ID3D10ShaderResourceView_AddRef(view);
  fx_release(&v->user_srv);
  v->user_srv = view;
  if (!view) v->changed = TRUE;
  return S_OK;
}
static HRESULT STDMETHODCALLTYPE fxcb_GetTextureBuffer(FxVar *v, ID3D10ShaderResourceView **out) {
  if (v->null) return E_FAIL;
  if (!out) return E_INVALIDARG;
  *out = NULL;
  if (v->type->base != D3D10_SVT_TBUFFER) return D3DERR_INVALIDCALL;
  *out = v->user_srv ? v->user_srv : v->srv;
  if (*out) ID3D10ShaderResourceView_AddRef(*out);
  return S_OK;
}

/* ---- ID3D10EffectScalarVariable (its arrays are written from their first element: the offset is ignored, as by Windows) ----------------------------------------------- */

static HRESULT STDMETHODCALLTYPE fxs_SetFloat(FxVar *v, float value) { return fx_values(v, &value, D3D10_SVT_FLOAT, FALSE, 0, 1, TRUE); }
static HRESULT STDMETHODCALLTYPE fxs_GetFloat(FxVar *v, float *value) { return fx_values(v, value, D3D10_SVT_FLOAT, FALSE, 0, 1, FALSE); }
static HRESULT STDMETHODCALLTYPE fxs_SetFloatArray(FxVar *v, float *values, UINT offset, UINT count) { (void)offset; return fx_values(v, values, D3D10_SVT_FLOAT, TRUE, 0, count, TRUE); }
static HRESULT STDMETHODCALLTYPE fxs_GetFloatArray(FxVar *v, float *values, UINT offset, UINT count) { (void)offset; return fx_values(v, values, D3D10_SVT_FLOAT, TRUE, 0, count, FALSE); }
static HRESULT STDMETHODCALLTYPE fxs_SetInt(FxVar *v, int value) { return fx_values(v, &value, D3D10_SVT_INT, FALSE, 0, 1, TRUE); }
static HRESULT STDMETHODCALLTYPE fxs_GetInt(FxVar *v, int *value) { return fx_values(v, value, D3D10_SVT_INT, FALSE, 0, 1, FALSE); }
static HRESULT STDMETHODCALLTYPE fxs_SetIntArray(FxVar *v, int *values, UINT offset, UINT count) { (void)offset; return fx_values(v, values, D3D10_SVT_INT, TRUE, 0, count, TRUE); }
static HRESULT STDMETHODCALLTYPE fxs_GetIntArray(FxVar *v, int *values, UINT offset, UINT count) { (void)offset; return fx_values(v, values, D3D10_SVT_INT, TRUE, 0, count, FALSE); }
static HRESULT STDMETHODCALLTYPE fxs_SetBool(FxVar *v, BOOL value) { return fx_values(v, &value, D3D10_SVT_BOOL, FALSE, 0, 1, TRUE); }
static HRESULT STDMETHODCALLTYPE fxs_GetBool(FxVar *v, BOOL *value) { return fx_values(v, value, D3D10_SVT_BOOL, FALSE, 0, 1, FALSE); }
static HRESULT STDMETHODCALLTYPE fxs_SetBoolArray(FxVar *v, BOOL *values, UINT offset, UINT count) { (void)offset; return fx_values(v, values, D3D10_SVT_BOOL, TRUE, 0, count, TRUE); }
static HRESULT STDMETHODCALLTYPE fxs_GetBoolArray(FxVar *v, BOOL *values, UINT offset, UINT count) { (void)offset; return fx_values(v, values, D3D10_SVT_BOOL, TRUE, 0, count, FALSE); }

/* ---- ID3D10EffectVectorVariable ----------------------------------------------------------------------------------------------------------------------------------------- */

static HRESULT STDMETHODCALLTYPE fxvec_SetBoolVector(FxVar *v, BOOL *value) { return fx_values(v, value, D3D10_SVT_BOOL, FALSE, 0, 1, TRUE); }
static HRESULT STDMETHODCALLTYPE fxvec_SetIntVector(FxVar *v, int *value) { return fx_values(v, value, D3D10_SVT_INT, FALSE, 0, 1, TRUE); }
static HRESULT STDMETHODCALLTYPE fxvec_SetFloatVector(FxVar *v, float *value) { return fx_values(v, value, D3D10_SVT_FLOAT, FALSE, 0, 1, TRUE); }
static HRESULT STDMETHODCALLTYPE fxvec_GetBoolVector(FxVar *v, BOOL *value) { return fx_values(v, value, D3D10_SVT_BOOL, FALSE, 0, 1, FALSE); }
static HRESULT STDMETHODCALLTYPE fxvec_GetIntVector(FxVar *v, int *value) { return fx_values(v, value, D3D10_SVT_INT, FALSE, 0, 1, FALSE); }
static HRESULT STDMETHODCALLTYPE fxvec_GetFloatVector(FxVar *v, float *value) { return fx_values(v, value, D3D10_SVT_FLOAT, FALSE, 0, 1, FALSE); }
static HRESULT STDMETHODCALLTYPE fxvec_SetBoolVectorArray(FxVar *v, BOOL *values, UINT offset, UINT count) { return fx_values(v, values, D3D10_SVT_BOOL, TRUE, offset, count, TRUE); }
static HRESULT STDMETHODCALLTYPE fxvec_SetIntVectorArray(FxVar *v, int *values, UINT offset, UINT count) { return fx_values(v, values, D3D10_SVT_INT, TRUE, offset, count, TRUE); }
static HRESULT STDMETHODCALLTYPE fxvec_SetFloatVectorArray(FxVar *v, float *values, UINT offset, UINT count) { return fx_values(v, values, D3D10_SVT_FLOAT, TRUE, offset, count, TRUE); }
static HRESULT STDMETHODCALLTYPE fxvec_GetBoolVectorArray(FxVar *v, BOOL *values, UINT offset, UINT count) { return fx_values(v, values, D3D10_SVT_BOOL, TRUE, offset, count, FALSE); }
static HRESULT STDMETHODCALLTYPE fxvec_GetIntVectorArray(FxVar *v, int *values, UINT offset, UINT count) { return fx_values(v, values, D3D10_SVT_INT, TRUE, offset, count, FALSE); }
static HRESULT STDMETHODCALLTYPE fxvec_GetFloatVectorArray(FxVar *v, float *values, UINT offset, UINT count) { return fx_values(v, values, D3D10_SVT_FLOAT, TRUE, offset, count, FALSE); }

/* ---- ID3D10EffectMatrixVariable ---------------------------------------------------------------------------------------------------------------------------------------- */

static HRESULT STDMETHODCALLTYPE fxm_SetMatrix(FxVar *v, float *data) { return fx_matrices(v, data, FALSE, 0, 1, FALSE, TRUE); }
static HRESULT STDMETHODCALLTYPE fxm_GetMatrix(FxVar *v, float *data) { return fx_matrices(v, data, FALSE, 0, 1, FALSE, FALSE); }
static HRESULT STDMETHODCALLTYPE fxm_SetMatrixArray(FxVar *v, float *data, UINT offset, UINT count) { return fx_matrices(v, data, TRUE, offset, count, FALSE, TRUE); }
static HRESULT STDMETHODCALLTYPE fxm_GetMatrixArray(FxVar *v, float *data, UINT offset, UINT count) { return fx_matrices(v, data, TRUE, offset, count, FALSE, FALSE); }
static HRESULT STDMETHODCALLTYPE fxm_SetMatrixTranspose(FxVar *v, float *data) { return fx_matrices(v, data, FALSE, 0, 1, TRUE, TRUE); }
static HRESULT STDMETHODCALLTYPE fxm_GetMatrixTranspose(FxVar *v, float *data) { return fx_matrices(v, data, FALSE, 0, 1, TRUE, FALSE); }
static HRESULT STDMETHODCALLTYPE fxm_SetMatrixTransposeArray(FxVar *v, float *data, UINT offset, UINT count) { return fx_matrices(v, data, TRUE, offset, count, TRUE, TRUE); }
static HRESULT STDMETHODCALLTYPE fxm_GetMatrixTransposeArray(FxVar *v, float *data, UINT offset, UINT count) { return fx_matrices(v, data, TRUE, offset, count, TRUE, FALSE); }

/* ---- ID3D10EffectStringVariable --------------------------------------------------------------------------------------------------------------------------------------- */

static HRESULT STDMETHODCALLTYPE fxstr_GetString(FxVar *v, const char **out) {
  if (v->null) return E_FAIL;
  if (!out) return E_INVALIDARG;
  const FxVar *one = v->elements ? &v->elements[0] : v;
  *out = one->string ? one->string : "";
  return S_OK;
}
static HRESULT STDMETHODCALLTYPE fxstr_GetStringArray(FxVar *v, const char **out, UINT offset, UINT count) {
  if (v->null) return E_FAIL;
  if (!out) return E_INVALIDARG;
  if (!v->elements) {
    if (offset || count > 1) return E_INVALIDARG;
    if (count) out[0] = v->string ? v->string : "";
    return S_OK;
  }
  if (offset >= v->type->elements || count > v->type->elements - offset) return E_INVALIDARG;
  for (UINT i = 0; i < count; ++i) out[i] = v->elements[offset + i].string ? v->elements[offset + i].string : "";
  return S_OK;
}

/* ---- the views of shader resource, render target and depth stencil variables -------------------------------------------------------------------------------------- */

/* Sets (or gets) `count` views of a view variable from `offset` (an element's are its own slot; past the elements nothing is taken, as Windows). */
static HRESULT fx_views(FxVar *v, IUnknown **views, UINT offset, UINT count, BOOL array, BOOL write) {
  if (v->null || !v->views) return E_FAIL;
  if (!views && count) return E_INVALIDARG;
  const UINT slots = v->owns_views && v->type->elements ? v->type->elements : 1;
  if (!array) { offset = 0; count = 1; }
  if (offset >= slots) return S_OK;
  if (count > slots - offset) count = slots - offset;
  for (UINT i = 0; i < count; ++i) {
    IUnknown **slot = &v->views[offset + i];
    if (write) {
      if (views[i] == *slot) continue;
      if (views[i]) views[i]->lpVtbl->AddRef(views[i]);
      fx_release(slot);
      *slot = views[i];
    } else {
      views[i] = *slot;
      if (views[i]) views[i]->lpVtbl->AddRef(views[i]);
    }
  }
  return S_OK;
}
static HRESULT STDMETHODCALLTYPE fxsrv_SetResource(FxVar *v, ID3D10ShaderResourceView *view) { return fx_views(v, (IUnknown **)&view, 0, 1, FALSE, TRUE); }
static HRESULT STDMETHODCALLTYPE fxsrv_GetResource(FxVar *v, ID3D10ShaderResourceView **view) {
  if (view) *view = NULL;
  return v->null ? E_FAIL : !view ? E_INVALIDARG : fx_views(v, (IUnknown **)view, 0, 1, FALSE, FALSE);
}
static HRESULT STDMETHODCALLTYPE fxsrv_SetResourceArray(FxVar *v, ID3D10ShaderResourceView **views, UINT offset, UINT count) {
  return fx_views(v, (IUnknown **)views, offset, count, TRUE, TRUE);
}
static HRESULT STDMETHODCALLTYPE fxsrv_GetResourceArray(FxVar *v, ID3D10ShaderResourceView **views, UINT offset, UINT count) {
  return fx_views(v, (IUnknown **)views, offset, count, TRUE, FALSE);
}
static HRESULT STDMETHODCALLTYPE fxrtv_SetRenderTarget(FxVar *v, ID3D10RenderTargetView *view) { return fx_views(v, (IUnknown **)&view, 0, 1, FALSE, TRUE); }
static HRESULT STDMETHODCALLTYPE fxrtv_GetRenderTarget(FxVar *v, ID3D10RenderTargetView **view) {
  if (view) *view = NULL;
  return v->null ? E_FAIL : !view ? E_INVALIDARG : fx_views(v, (IUnknown **)view, 0, 1, FALSE, FALSE);
}
static HRESULT STDMETHODCALLTYPE fxrtv_SetRenderTargetArray(FxVar *v, ID3D10RenderTargetView **views, UINT offset, UINT count) {
  return fx_views(v, (IUnknown **)views, offset, count, TRUE, TRUE);
}
static HRESULT STDMETHODCALLTYPE fxrtv_GetRenderTargetArray(FxVar *v, ID3D10RenderTargetView **views, UINT offset, UINT count) {
  return fx_views(v, (IUnknown **)views, offset, count, TRUE, FALSE);
}
static HRESULT STDMETHODCALLTYPE fxdsv_SetDepthStencil(FxVar *v, ID3D10DepthStencilView *view) { return fx_views(v, (IUnknown **)&view, 0, 1, FALSE, TRUE); }
static HRESULT STDMETHODCALLTYPE fxdsv_GetDepthStencil(FxVar *v, ID3D10DepthStencilView **view) {
  if (view) *view = NULL;
  return v->null ? E_FAIL : !view ? E_INVALIDARG : fx_views(v, (IUnknown **)view, 0, 1, FALSE, FALSE);
}
static HRESULT STDMETHODCALLTYPE fxdsv_SetDepthStencilArray(FxVar *v, ID3D10DepthStencilView **views, UINT offset, UINT count) {
  return fx_views(v, (IUnknown **)views, offset, count, TRUE, TRUE);
}
static HRESULT STDMETHODCALLTYPE fxdsv_GetDepthStencilArray(FxVar *v, ID3D10DepthStencilView **views, UINT offset, UINT count) {
  return fx_views(v, (IUnknown **)views, offset, count, TRUE, FALSE);
}

/* ---- ID3D10EffectShaderVariable --------------------------------------------------------------------------------------------------------------------------------------- */

/* The shader `index` of a shader variable: its own (index 0), or the index-th of the effect's shaders after it (what Windows counts: an array's elements are in order). */
static FxShader *fx_shader_of(FxVar *v, UINT index) {
  if (v->null) return NULL;
  v = fx_element(v, 0);
  if (!index) return v->shader;
  FxEffect *e = v->effect;
  if (!e) return NULL;
  for (UINT i = 0; i < e->shader_used; ++i)
    if (e->shader_list[i] == v) return index < e->shader_used - i ? e->shader_list[i + index]->shader : NULL;
  return NULL;
}
static HRESULT STDMETHODCALLTYPE fxsh_GetShaderDesc(FxVar *v, UINT index, D3D10_EFFECT_SHADER_DESC *desc) {
  if (v->null) return E_FAIL;
  if (!desc) return E_INVALIDARG;
  FxShader *s = fx_shader_of(v, index);
  if (!s) return E_FAIL;
  memset(desc, 0, sizeof *desc);
  if (s->input_signature) desc->pInputSignature = ID3D10Blob_GetBufferPointer(s->input_signature);
  desc->IsInline = s->inline_;
  desc->pBytecode = s->code;
  desc->BytecodeLength = s->code ? s->size : 0;
  desc->SODecl = s->so_decl;
  if (s->reflection) {
    D3D10_SHADER_DESC d;
    if (SUCCEEDED(s->reflection->lpVtbl->GetDesc(s->reflection, &d))) {
      desc->NumInputSignatureEntries = d.InputParameters;
      desc->NumOutputSignatureEntries = d.OutputParameters;
    }
  }
  return S_OK;
}
static HRESULT fx_shader_object(FxVar *v, UINT index, D3D10_SHADER_VARIABLE_TYPE kind, void **out) {
  if (!out) return E_INVALIDARG;
  *out = NULL;
  if (v->null) return E_FAIL;
  FxShader *s = fx_shader_of(v, index);
  if (!s) return E_FAIL;
  if (s->kind != kind) return D3DERR_INVALIDCALL;
  if ((*out = s->object)) s->object->lpVtbl->AddRef(s->object);
  return S_OK;
}
static HRESULT STDMETHODCALLTYPE fxsh_GetVertexShader(FxVar *v, UINT index, ID3D10VertexShader **out) { return fx_shader_object(v, index, D3D10_SVT_VERTEXSHADER, (void **)out); }
static HRESULT STDMETHODCALLTYPE fxsh_GetGeometryShader(FxVar *v, UINT index, ID3D10GeometryShader **out) { return fx_shader_object(v, index, D3D10_SVT_GEOMETRYSHADER, (void **)out); }
static HRESULT STDMETHODCALLTYPE fxsh_GetPixelShader(FxVar *v, UINT index, ID3D10PixelShader **out) { return fx_shader_object(v, index, D3D10_SVT_PIXELSHADER, (void **)out); }
static HRESULT fx_signature(FxVar *v, UINT index, UINT element, BOOL output, D3D10_SIGNATURE_PARAMETER_DESC *desc) {
  if (v->null) return E_FAIL;
  FxShader *s = fx_shader_of(v, index);
  if (!s) return E_FAIL;
  if (!s->reflection) return D3DERR_INVALIDCALL;
  return output ? s->reflection->lpVtbl->GetOutputParameterDesc(s->reflection, element, desc) : s->reflection->lpVtbl->GetInputParameterDesc(s->reflection, element, desc);
}
static HRESULT STDMETHODCALLTYPE fxsh_GetInputSignatureElementDesc(FxVar *v, UINT index, UINT element, D3D10_SIGNATURE_PARAMETER_DESC *desc) {
  return fx_signature(v, index, element, FALSE, desc);
}
static HRESULT STDMETHODCALLTYPE fxsh_GetOutputSignatureElementDesc(FxVar *v, UINT index, UINT element, D3D10_SIGNATURE_PARAMETER_DESC *desc) {
  return fx_signature(v, index, element, TRUE, desc);
}

/* ---- the state variables ---------------------------------------------------------------------------------------------------------------------------------------------- */

static void fx_state_refresh(FxVar *v);
/* The state object `index` of a state variable: counted from it among the effect's states of its kind (an array's elements are in order). */
static FxVar *fx_state_of(FxVar *v, UINT index, D3D10_SHADER_VARIABLE_TYPE kind) {
  if (v->null) return NULL;
  v = fx_element(v, 0);
  FxEffect *e = v->effect;
  if (!e || !v->state || v->type->base != kind) return NULL;
  FxVar **list = kind == D3D10_SVT_BLEND ? e->blend_list : kind == D3D10_SVT_DEPTHSTENCIL ? e->ds_list : kind == D3D10_SVT_RASTERIZER ? e->rs_list : e->sampler_list;
  const UINT count = kind == D3D10_SVT_BLEND ? e->blend_used : kind == D3D10_SVT_DEPTHSTENCIL ? e->ds_used : kind == D3D10_SVT_RASTERIZER ? e->rs_used : e->sampler_used;
  const UINT at = v->state->index;
  if (at >= count || index >= count - at) return NULL;
  FxVar *s = list[at + index];
  fx_state_refresh(s);
  return s;
}
static HRESULT fx_state_object(FxVar *v, UINT index, D3D10_SHADER_VARIABLE_TYPE kind, void **out) {
  if (v->null) return E_FAIL;
  if (!out) return E_INVALIDARG;
  *out = NULL;
  FxVar *s = fx_state_of(v, index, kind);
  if (!s) return E_FAIL;
  if ((*out = s->state->object)) s->state->object->lpVtbl->AddRef(s->state->object);
  return S_OK;
}
/* Direct3D 10's blend description of a blend state (one of Direct3D 10.1: its first target's blend). */
static void fx_blend_desc(const FxState *s, D3D10_BLEND_DESC *out) {
  if (!s->ten_one) { *out = s->desc.blend; return; }
  const D3D10_BLEND_DESC1 *b = &s->desc.blend1;
  out->AlphaToCoverageEnable = b->AlphaToCoverageEnable;
  out->SrcBlend = b->RenderTarget[0].SrcBlend;
  out->DestBlend = b->RenderTarget[0].DestBlend;
  out->BlendOp = b->RenderTarget[0].BlendOp;
  out->SrcBlendAlpha = b->RenderTarget[0].SrcBlendAlpha;
  out->DestBlendAlpha = b->RenderTarget[0].DestBlendAlpha;
  out->BlendOpAlpha = b->RenderTarget[0].BlendOpAlpha;
  for (UINT i = 0; i < 8; ++i) {
    out->BlendEnable[i] = b->RenderTarget[i].BlendEnable;
    out->RenderTargetWriteMask[i] = b->RenderTarget[i].RenderTargetWriteMask;
  }
}
static HRESULT STDMETHODCALLTYPE fxblend_GetBlendState(FxVar *v, UINT index, ID3D10BlendState **out) { return fx_state_object(v, index, D3D10_SVT_BLEND, (void **)out); }
static HRESULT STDMETHODCALLTYPE fxblend_GetBackingStore(FxVar *v, UINT index, D3D10_BLEND_DESC *desc) {
  if (v->null) return E_FAIL;
  if (!desc) return E_INVALIDARG;
  FxVar *s = fx_state_of(v, index, D3D10_SVT_BLEND);
  if (!s) return E_FAIL;
  fx_blend_desc(s->state, desc);
  return S_OK;
}
static HRESULT STDMETHODCALLTYPE fxds_GetDepthStencilState(FxVar *v, UINT index, ID3D10DepthStencilState **out) {
  return fx_state_object(v, index, D3D10_SVT_DEPTHSTENCIL, (void **)out);
}
static HRESULT STDMETHODCALLTYPE fxds_GetBackingStore(FxVar *v, UINT index, D3D10_DEPTH_STENCIL_DESC *desc) {
  if (v->null) return E_FAIL;
  if (!desc) return E_INVALIDARG;
  FxVar *s = fx_state_of(v, index, D3D10_SVT_DEPTHSTENCIL);
  if (!s) return E_FAIL;
  *desc = s->state->desc.ds;
  return S_OK;
}
static HRESULT STDMETHODCALLTYPE fxrs_GetRasterizerState(FxVar *v, UINT index, ID3D10RasterizerState **out) {
  return fx_state_object(v, index, D3D10_SVT_RASTERIZER, (void **)out);
}
static HRESULT STDMETHODCALLTYPE fxrs_GetBackingStore(FxVar *v, UINT index, D3D10_RASTERIZER_DESC *desc) {
  if (v->null) return E_FAIL;
  if (!desc) return E_INVALIDARG;
  FxVar *s = fx_state_of(v, index, D3D10_SVT_RASTERIZER);
  if (!s) return E_FAIL;
  *desc = s->state->desc.rs;
  return S_OK;
}
static HRESULT STDMETHODCALLTYPE fxsamp_GetSampler(FxVar *v, UINT index, ID3D10SamplerState **out) { return fx_state_object(v, index, D3D10_SVT_SAMPLER, (void **)out); }
static HRESULT STDMETHODCALLTYPE fxsamp_GetBackingStore(FxVar *v, UINT index, D3D10_SAMPLER_DESC *desc) {
  if (v->null) return E_FAIL;
  if (!desc) return E_INVALIDARG;
  FxVar *s = fx_state_of(v, index, D3D10_SVT_SAMPLER);
  if (!s) return E_FAIL;
  *desc = s->state->desc.sampler.desc;
  return S_OK;
}

/* ---- finding variables ----------------------------------------------------------------------------------------------------------------------------------------------- */

static FxVar *fx_find_buffer(FxEffect *e, const char *name) {
  for (; e; e = e->pool)
    for (UINT i = 0; i < e->buffer_count; ++i) if (fx_name_is(e->buffers[i].name, name)) return &e->buffers[i];
  return NULL;
}
static FxVar *fx_find_var(FxEffect *e, const char *name) {
  for (; e && name; e = e->pool) {
    for (UINT i = 0; i < e->buffer_count; ++i)
      for (UINT k = 0; k < e->buffers[i].type->member_count; ++k)
        if (fx_name_is(e->buffers[i].members[k].name, name)) return &e->buffers[i].members[k];
    for (UINT i = 0; i < e->object_count; ++i) if (fx_name_is(e->objects[i].name, name)) return &e->objects[i];
  }
  return NULL;
}

/* ---- state assignments ------------------------------------------------------------------------------------------------------------------------------------------------ */

/* Where property `id` (value `index`) of a container is: a place for its numbers, or for the variable it names (and a shader's index). */
typedef struct FxPlace { BYTE *at; FxVar **var; UINT *var_index; } FxPlace;
static BOOL fx_place(UINT container, void *c, BOOL ten_one, UINT id, UINT index, FxPlace *p) {
  p->at = NULL;
  p->var = NULL;
  p->var_index = NULL;
  switch (container) {
    case FX4_IN_PASS: {
      FxPass *pass = c;
      switch (id) {
        case 0: p->var = &pass->rs; return TRUE;
        case 1: p->var = &pass->ds; return TRUE;
        case 2: p->var = &pass->blend; return TRUE;
        case 3: p->var = &pass->rtv[index]; return TRUE;
        case 4: p->var = &pass->dsv; return TRUE;
        case 6: p->var = &pass->vs; p->var_index = &pass->vs_index; return TRUE;
        case 7: p->var = &pass->ps; p->var_index = &pass->ps_index; return TRUE;
        case 8: p->var = &pass->gs; p->var_index = &pass->gs_index; return TRUE;
        case 9: p->at = (BYTE *)&pass->stencil_ref; return TRUE;
        case 10: p->at = (BYTE *)pass->blend_factor; return TRUE;
        case 11: p->at = (BYTE *)&pass->sample_mask; return TRUE;
        default: return FALSE;
      }
    }
    case FX4_IN_RASTERIZER: p->at = (BYTE *)c + 4 * (id - 12); return TRUE;      /* (FillMode .. AntialiasedLineEnable: D3D10_RASTERIZER_DESC's order) */
    case FX4_IN_DEPTHSTENCIL: {
      D3D10_DEPTH_STENCIL_DESC *d = c;
      static const BYTE at[] = {offsetof(D3D10_DEPTH_STENCIL_DESC, DepthEnable), offsetof(D3D10_DEPTH_STENCIL_DESC, DepthWriteMask),
                                offsetof(D3D10_DEPTH_STENCIL_DESC, DepthFunc), offsetof(D3D10_DEPTH_STENCIL_DESC, StencilEnable),
                                offsetof(D3D10_DEPTH_STENCIL_DESC, StencilReadMask), offsetof(D3D10_DEPTH_STENCIL_DESC, StencilWriteMask),
                                offsetof(D3D10_DEPTH_STENCIL_DESC, FrontFace.StencilFailOp), offsetof(D3D10_DEPTH_STENCIL_DESC, FrontFace.StencilDepthFailOp),
                                offsetof(D3D10_DEPTH_STENCIL_DESC, FrontFace.StencilPassOp), offsetof(D3D10_DEPTH_STENCIL_DESC, FrontFace.StencilFunc),
                                offsetof(D3D10_DEPTH_STENCIL_DESC, BackFace.StencilFailOp), offsetof(D3D10_DEPTH_STENCIL_DESC, BackFace.StencilDepthFailOp),
                                offsetof(D3D10_DEPTH_STENCIL_DESC, BackFace.StencilPassOp), offsetof(D3D10_DEPTH_STENCIL_DESC, BackFace.StencilFunc)};
      p->at = (BYTE *)d + at[id - 22];
      return TRUE;
    }
    case FX4_IN_BLEND:
      if (ten_one) {
        D3D10_BLEND_DESC1 *b = c;
        if (id == 36) p->at = (BYTE *)&b->AlphaToCoverageEnable;
        else if (id == 37) p->at = (BYTE *)&b->RenderTarget[index].BlendEnable;
        else if (id == 44) p->at = (BYTE *)&b->RenderTarget[index].RenderTargetWriteMask;
        else p->at = (BYTE *)&b->RenderTarget[index].SrcBlend + 4 * (id - 38);      /* (SrcBlend .. BlendOpAlpha, in order) */
      } else {
        D3D10_BLEND_DESC *b = c;
        if (id == 36) p->at = (BYTE *)&b->AlphaToCoverageEnable;
        else if (id == 37) p->at = (BYTE *)&b->BlendEnable[index];
        else if (id == 44) p->at = (BYTE *)&b->RenderTargetWriteMask[index];
        else p->at = (BYTE *)&b->SrcBlend + 4 * (id - 38);
      }
      return TRUE;
    case FX4_IN_SAMPLER: {
      FxSamplerDesc *s = c;
      if (id == 55) { p->var = &s->texture; return TRUE; }
      p->at = id <= 52 ? (BYTE *)&s->desc + 4 * (id - 45) : id == 53 ? (BYTE *)&s->desc.MinLOD : (BYTE *)&s->desc.MaxLOD;
      return TRUE;
    }
    default: return FALSE;
  }
}
static UINT fx_base_of(D3D10_SHADER_VARIABLE_TYPE t) { return t == D3D10_SVT_FLOAT ? FX4_BASE_FLOAT : t == D3D10_SVT_INT ? FX4_BASE_INT : t == D3D10_SVT_UINT ? FX4_BASE_UINT : FX4_BASE_BOOL; }
/* One number into a property's place, as Windows converts them (a float from a boolean, and an 8-bit value from a float or a boolean, are refused). */
static BOOL fx_number(UINT from, UINT bits, UINT to, BYTE *at) {
  switch (to) {
    case FX4_PT_FLOAT: {
      float f;
      if (from == FX4_BASE_FLOAT) f = fx4_bits_float(bits);
      else if (from == FX4_BASE_INT) f = (float)(int)bits;
      else if (from == FX4_BASE_UINT) f = (float)bits;
      else return FALSE;
      memcpy(at, &f, 4);
      return TRUE;
    }
    case FX4_PT_INT: case FX4_PT_UINT: case FX4_PT_BOOL: {
      const UINT v = from == FX4_BASE_FLOAT ? (UINT)fx4_f2i(fx4_bits_float(bits)) : bits;
      memcpy(at, &v, 4);
      return TRUE;
    }
    case FX4_PT_UINT8:
      if (from != FX4_BASE_INT && from != FX4_BASE_UINT) return FALSE;
      *at = (BYTE)bits;
      return TRUE;
    default: return FALSE;
  }
}
static UINT fx_width(UINT type) { return type == FX4_PT_UINT8 ? 1 : 4; }
/* Whether a variable is of the kind an object property takes (a texture property takes every kind of texture). */
static BOOL fx_object_matches(UINT ptype, const FxVar *v) {
  const D3D10_SHADER_VARIABLE_TYPE b = v->type->base;
  switch (ptype) {
    case FX4_PT_RASTERIZER: return b == D3D10_SVT_RASTERIZER;
    case FX4_PT_DEPTHSTENCIL: return b == D3D10_SVT_DEPTHSTENCIL;
    case FX4_PT_BLEND: return b == D3D10_SVT_BLEND;
    case FX4_PT_RTV: return b == D3D10_SVT_RENDERTARGETVIEW;
    case FX4_PT_DSV: return b == D3D10_SVT_DEPTHSTENCILVIEW;
    case FX4_PT_VS: return b == D3D10_SVT_VERTEXSHADER;
    case FX4_PT_PS: return b == D3D10_SVT_PIXELSHADER;
    case FX4_PT_GS: return b == D3D10_SVT_GEOMETRYSHADER;
    case FX4_PT_TEXTURE:
      return b == D3D10_SVT_TEXTURE || b == D3D10_SVT_TEXTURE1D || b == D3D10_SVT_TEXTURE1DARRAY || b == D3D10_SVT_TEXTURE2D || b == D3D10_SVT_TEXTURE2DARRAY ||
             b == D3D10_SVT_TEXTURE2DMS || b == D3D10_SVT_TEXTURE2DMSARRAY || b == D3D10_SVT_TEXTURE3D || b == D3D10_SVT_TEXTURECUBE || b == D3D10_SVT_TEXTURECUBEARRAY;
    default: return FALSE;
  }
}
/* What a NULL constant gives an object property: no object (a shader property: a valid shader variable of no shader, which Apply sets). */
static FxVar *fx_null_for(UINT ptype) {
  switch (ptype) {
    case FX4_PT_VS: return &g_fx_anon[0];
    case FX4_PT_PS: return &g_fx_anon[1];
    case FX4_PT_GS: return &g_fx_anon[2];
    case FX4_PT_TEXTURE: return &g_fx_null_srv;
    case FX4_PT_RASTERIZER: return &g_fx_null_rs;
    case FX4_PT_DEPTHSTENCIL: return &g_fx_null_ds;
    case FX4_PT_BLEND: return &g_fx_null_blend;
    case FX4_PT_RTV: return &g_fx_null_rtv;
    case FX4_PT_DSV: return &g_fx_null_dsv;
    default: return NULL;
  }
}

/* An expression's tables and the variables its constant table reads (numbers of the effect or its pool). */
static HRESULT fx_expression(FxEffect *e, const Fx4Preshader *p, FxDep *d) {
  d->expression = p;
  if (p->var_count && !(d->inputs = fx_new_array(p->var_count, sizeof *d->inputs))) return E_OUTOFMEMORY;
  for (UINT i = 0; i < p->var_count; ++i) {
    FxVar *v = fx_find_var(e, p->vars[i].name);
    if (!v || v->kind != FXV_NUMERIC || !fx_storage(v)) return E_FAIL;
    d->inputs[i] = v;
  }
  for (UINT t = 0; t < FX4_TABLES; ++t) {
    if (t != FX4_TABLE_CONSTANTS && t != FX4_TABLE_CB && t != FX4_TABLE_RESULT && t != FX4_TABLE_TEMP) continue;
    const UINT n = p->table_size[t] ? p->table_size[t] : 1;
    if (!(d->tables[t] = fx_new_array(n, sizeof(float)))) return E_OUTOFMEMORY;
  }
  for (UINT i = 0; i < p->literal_count; ++i) memcpy(&d->tables[FX4_TABLE_CONSTANTS][i], p->literals + 4 * (SIZE_T)i, 4);
  return S_OK;
}
static void fx_evaluate(FxDep *d) {
  const Fx4Preshader *p = d->expression;
  float *cb = d->tables[FX4_TABLE_CB];
  memset(cb, 0, (SIZE_T)(p->table_size[FX4_TABLE_CB] ? p->table_size[FX4_TABLE_CB] : 1) * sizeof(float));
  for (UINT i = 0; i < p->var_count; ++i) {
    /* (a variable's registers: as many of its bytes as they hold) */
    const FxVar *v = d->inputs[i];
    UINT bytes = p->vars[i].reg_count * 16;
    if (bytes > v->type->unpacked) bytes = v->type->unpacked;
    memcpy(cb + 4 * (SIZE_T)p->vars[i].reg_index, fx_storage(v), bytes);
  }
  fx4_preshader_eval(p, d->tables);
}
static void fx_deps_free(FxDeps *deps) {
  for (UINT i = 0; i < deps->count; ++i) {
    mem_free(deps->items[i].inputs);
    for (UINT t = 0; t < FX4_TABLES; ++t) mem_free(deps->items[i].tables[t]);
  }
  mem_free(deps->items);
  deps->items = NULL;
  deps->count = 0;
}
/* What the dependencies of a container give it now. */
static void fx_deps_apply(const FxDeps *deps, UINT container, void *c, BOOL ten_one) {
  for (UINT i = 0; i < deps->count; ++i) {
    FxDep *d = &deps->items[i];
    const Fx4Property *prop = &fx4_properties[d->id];
    FxPlace p;
    if (!fx_place(container, c, ten_one, d->id, d->index, &p)) continue;
    switch (d->op) {
      case FX4_OP_VAR: case FX4_OP_CONST_INDEX: {
        const BYTE *src = fx_storage(d->var);
        if (!src || !p.at) break;
        for (UINT k = 0; k < prop->size; ++k)
          fx_number(fx_base_of(d->var->type->base), fx_get32(src + d->offset + 4 * k), prop->type, p.at + (SIZE_T)k * fx_width(prop->type));
        break;
      }
      case FX4_OP_INDEX_EXPRESSION: {
        fx_evaluate(d);
        UINT index = fx4_float_bits(d->tables[FX4_TABLE_RESULT][0]);
        if (index >= d->var->type->elements) index = 0;      /* (past the array: the first element, as Windows) */
        if (p.var_index) { *p.var = d->var; *p.var_index = index; }
        else if (p.var) *p.var = &d->var->elements[index];
        break;
      }
      case FX4_OP_VALUE_EXPRESSION: {
        fx_evaluate(d);
        if (!p.at) break;
        UINT n = d->expression->table_size[FX4_TABLE_RESULT];
        if (n > prop->size) n = prop->size;
        for (UINT k = 0; k < n; ++k) {
          const UINT bits = fx4_float_bits(d->tables[FX4_TABLE_RESULT][k]);
          if (prop->type == FX4_PT_UINT8) p.at[k] = (BYTE)bits;
          else memcpy(p.at + 4 * (SIZE_T)k, &bits, 4);
        }
        break;
      }
      default: break;
    }
  }
}

static HRESULT fx_make_shader(FxEffect *e, FxVar *v, const Fx4Shader *src, BOOL inline_);
/* One state assignment of the binary into a container: constants now, variables and expressions as dependencies. E_FAIL for one that does not fit the effect. */
static HRESULT fx_assign(FxEffect *e, UINT container, void *c, BOOL ten_one, const Fx4Assignment *a, FxDeps *deps) {
  const Fx4Property *prop = &fx4_properties[a->id];
  const BOOL object = prop->type >= FX4_PT_RASTERIZER;
  FxPlace p;
  FxDep d;
  HRESULT hr;
  memset(&d, 0, sizeof d);
  if (!fx_place(container, c, ten_one, a->id, a->index, &p)) return E_FAIL;
  if (object ? !p.var : !p.at) return E_FAIL;
  d.id = a->id;
  d.index = a->index;
  d.op = a->op;
  switch (a->op) {
    case FX4_OP_CONST:
      if (object) {
        if (a->value_count != 1 || a->values[0].bits) return E_FAIL;      /* (an object constant is NULL) */
        *p.var = fx_null_for(prop->type);
        if (p.var_index) *p.var_index = 0;
        return S_OK;
      }
      for (UINT k = 0; k < a->value_count; ++k)
        if (!fx_number(a->values[k].type, a->values[k].bits, prop->type, p.at + (SIZE_T)k * fx_width(prop->type))) return E_FAIL;
      return S_OK;
    case FX4_OP_VAR: case FX4_OP_CONST_INDEX: {
      FxVar *v = fx_find_var(e, a->name);
      if (!v) return E_FAIL;
      if (object) {
        if (a->op == FX4_OP_VAR ? v->type->elements != 0 : a->element >= v->type->elements) return E_FAIL;
        if (!fx_object_matches(prop->type, v)) return E_FAIL;
        if (p.var_index) { *p.var = v; *p.var_index = a->op == FX4_OP_VAR ? 0 : a->element; }
        else *p.var = a->op == FX4_OP_VAR ? v : &v->elements[a->element];
        return S_OK;
      }
      /* (numbers: read from the variable when used; its type has the floats asked for) */
      const UINT need = 4 * (UINT)prop->size;
      d.offset = a->op == FX4_OP_VAR ? 0 : a->element;
      if (a->op == FX4_OP_CONST_INDEX) { if (d.offset > 0x3FFFFFFFu) return E_FAIL; d.offset *= 4; }
      if (v->kind != FXV_NUMERIC || !fx_storage(v) || d.offset > v->type->unpacked || v->type->unpacked - d.offset < need) return E_FAIL;
      d.var = v;
      break;
    }
    case FX4_OP_INDEX_EXPRESSION: {
      FxVar *v = fx_find_var(e, a->name);
      if (!v || !v->type->elements || !v->elements || !fx_object_matches(prop->type, v)) return E_FAIL;
      d.var = v;
      if (FAILED(hr = fx_expression(e, &a->expression, &d))) { mem_free(d.inputs); for (UINT t = 0; t < FX4_TABLES; ++t) mem_free(d.tables[t]); return hr; }
      break;
    }
    case FX4_OP_VALUE_EXPRESSION:
      if (FAILED(hr = fx_expression(e, &a->expression, &d))) { mem_free(d.inputs); for (UINT t = 0; t < FX4_TABLES; ++t) mem_free(d.tables[t]); return hr; }
      break;
    case FX4_OP_ANONYMOUS_SHADER: {
      if (e->anonymous_count >= e->fx.anonymous_count) return E_FAIL;
      FxVar *v = &e->anonymous[e->anonymous_count];
      FxType *t = &e->anonymous_types[e->anonymous_count];
      const UINT k = prop->type == FX4_PT_VS ? 0 : prop->type == FX4_PT_PS ? 1 : 2;
      *t = g_fx_anon_types[k];
      v->lpVtbl = &fxsh_vtbl;
      v->kind = FXV_SHADER;
      v->effect = e;
      v->buffer = &g_fx_null_cb;
      v->type = t;
      v->name = "$Anonymous";
      v->explicit_bind = ~0u;
      ++e->anonymous_count;
      if (FAILED(hr = fx_make_shader(e, v, &a->shader, TRUE))) return hr;
      *p.var = v;
      if (p.var_index) *p.var_index = 0;
      return S_OK;
    }
    default: return E_FAIL;
  }
  /* (the dependency, kept; its first value now) */
  FxDep *list = mem_grow(deps->items, sizeof *list * ((SIZE_T)deps->count + 1));
  if (!list) { mem_free(d.inputs); for (UINT t = 0; t < FX4_TABLES; ++t) mem_free(d.tables[t]); return E_OUTOFMEMORY; }
  deps->items = list;
  list[deps->count++] = d;
  FxDeps last = {1, &list[deps->count - 1]};
  fx_deps_apply(&last, container, c, ten_one);
  return S_OK;
}

/* ---- shaders ------------------------------------------------------------------------------------------------------------------------------------------------------ */

/* A stream output declaration ("[slot:]SEMANTIC[index][.mask]; ..." with $SKIP for a gap) as entries, and the stride of slot 0. The text is cut in a copy (`buffer`). */
#define FX_SO_ENTRIES 64u
static HRESULT fx_so_decl(const char *text, char *buffer, SIZE_T capacity, D3D10_SO_DECLARATION_ENTRY *entries, UINT *count, UINT *stride) {
  SIZE_T n = 0;
  while (text[n]) { if (n + 1 >= capacity) return E_FAIL; buffer[n] = text[n]; ++n; }
  buffer[n] = 0;
  *count = *stride = 0;
  char *p = buffer;
  while (*p) {
    char *end = p;
    while (*end && *end != ';') ++end;
    char *next = *end ? end + 1 : end;
    *end = 0;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') ++p;
    char *last = p + alen(p);
    while (last > p && (last[-1] == ' ' || last[-1] == '\t' || last[-1] == '\r' || last[-1] == '\n')) *--last = 0;
    if (!*p) { p = next; continue; }
    if (*count >= FX_SO_ENTRIES) return E_FAIL;
    D3D10_SO_DECLARATION_ENTRY e = {NULL, 0, 0, 4, 0};
    /* the slot */
    char *colon = p;
    while (*colon && *colon != ':') ++colon;
    if (*colon) {
      if (colon == p) return E_FAIL;
      UINT slot = 0;
      for (char *q = p; q < colon; ++q) { if (*q < '0' || *q > '9' || slot > 9) return E_FAIL; slot = slot * 10 + (UINT)(*q - '0'); }
      e.OutputSlot = (BYTE)slot;
      p = colon + 1;
    }
    /* the mask: the components, in order, of xyzw or rgba */
    char *dot = p;
    while (*dot && *dot != '.') ++dot;
    if (*dot) {
      *dot = 0;
      const char *mask = dot + 1;
      static const char *const sets[2] = {"xyzw", "rgba"};
      BOOL found = FALSE;
      const SIZE_T len = alen(mask);
      for (UINT s = 0; s < 2 && !found && len && len <= 4; ++s)
        for (UINT start = 0; start + len <= 4 && !found; ++start) {
          SIZE_T k = 0;
          while (k < len && sets[s][start + k] == mask[k]) ++k;
          if (k == len) { e.StartComponent = (BYTE)start; e.ComponentCount = (BYTE)len; found = TRUE; }
        }
      if (!found) return E_FAIL;
    }
    /* the semantic and its index */
    SIZE_T len = alen(p);
    while (len && p[len - 1] >= '0' && p[len - 1] <= '9') --len;
    if (p[len]) {
      UINT index = 0;
      for (const char *q = p + len; *q; ++q) { if (index > 100000) return E_FAIL; index = index * 10 + (UINT)(*q - '0'); }
      e.SemanticIndex = index;
      p[len] = 0;
    }
    if (!len) return E_FAIL;
    e.SemanticName = fx_stricmp(p, "$SKIP") ? p : NULL;
    entries[(*count)++] = e;
    if (!e.OutputSlot) *stride += 4 * (UINT)e.ComponentCount;
    p = next;
  }
  return S_OK;
}
static FxShader *fx_new_shader(FxEffect *e, FxVar *v) {
  if (e->shader_used >= e->shader_count) return NULL;
  FxShader *s = mem_alloc(sizeof *s);
  if (!s) return NULL;
  v->shader = s;
  e->shader_list[e->shader_used++] = v;
  return s;
}
/* A shader of the effect: reflected, its resources found among the effect's variables, made on the device. */
static HRESULT fx_make_shader(FxEffect *e, FxVar *v, const Fx4Shader *src, BOOL inline_) {
  FxShader *s = fx_new_shader(e, v);
  HRESULT hr;
  if (!s) return v->shader ? E_FAIL : E_OUTOFMEMORY;
  s->kind = v->type->base;
  s->code = src->code;
  s->size = src->size;
  s->so_decl = src->so_decl;
  s->inline_ = inline_;
  if (!s->code) return S_OK;      /* (a NULL shader) */
  if (FAILED(hr = D3D10ReflectShader(s->code, s->size, &s->reflection))) return hr;
  if (!s->reflection) return E_FAIL;
  if (FAILED(D3D10GetInputSignatureBlob(s->code, s->size, &s->input_signature))) s->input_signature = NULL;
  D3D10_SHADER_DESC desc;
  if (FAILED(hr = s->reflection->lpVtbl->GetDesc(s->reflection, &desc))) return hr;
  if (desc.BoundResources) {
    if (!(s->bindings = fx_new_array(desc.BoundResources, sizeof *s->bindings))) return E_OUTOFMEMORY;
    for (UINT i = 0; i < desc.BoundResources; ++i) {
      D3D10_SHADER_INPUT_BIND_DESC b;
      if (FAILED(hr = s->reflection->lpVtbl->GetResourceBindingDesc(s->reflection, i, &b))) return hr;
      FxBinding *x = &s->bindings[s->binding_count++];
      x->type = b.Type;
      x->point = b.BindPoint;
      x->count = b.BindCount;
      if (b.Type == D3D10_SIT_CBUFFER || b.Type == D3D10_SIT_TBUFFER) {
        if (b.BindCount != 1) return E_UNEXPECTED;
        x->var = fx_find_buffer(e, b.Name);
      } else if (b.Type == D3D10_SIT_TEXTURE || b.Type == D3D10_SIT_SAMPLER) {
        x->var = fx_find_var(e, b.Name);
      }
      if (!x->var) return E_FAIL;
      if (x->point >= D3D10_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT || x->count > D3D10_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT - x->point) return E_FAIL;
    }
  }
  switch (s->kind) {
    case D3D10_SVT_VERTEXSHADER: hr = ID3D10Device_CreateVertexShader(e->device, s->code, s->size, (ID3D10VertexShader **)&s->object); break;
    case D3D10_SVT_PIXELSHADER: hr = ID3D10Device_CreatePixelShader(e->device, s->code, s->size, (ID3D10PixelShader **)&s->object); break;
    case D3D10_SVT_GEOMETRYSHADER:
      if (s->so_decl) {
        D3D10_SO_DECLARATION_ENTRY entries[FX_SO_ENTRIES];
        UINT count, stride;
        const SIZE_T capacity = alen(s->so_decl) + 1;
        char *buffer = mem_alloc(capacity);
        if (!buffer) return E_OUTOFMEMORY;
        hr = fx_so_decl(s->so_decl, buffer, capacity, entries, &count, &stride);
        if (SUCCEEDED(hr))
          hr = ID3D10Device_CreateGeometryShaderWithStreamOutput(e->device, s->code, s->size, entries, count, stride, (ID3D10GeometryShader **)&s->object);
        mem_free(buffer);
      } else {
        hr = ID3D10Device_CreateGeometryShader(e->device, s->code, s->size, (ID3D10GeometryShader **)&s->object);
      }
      break;
    default: hr = E_FAIL;
  }
  return hr;
}
static void fx_shader_free(FxShader *s) {
  if (!s) return;
  fx_release(&s->object);
  fx_release(&s->reflection);
  fx_release(&s->input_signature);
  mem_free(s->bindings);
  mem_free(s);
}

/* ---- state objects ---------------------------------------------------------------------------------------------------------------------------------------------------- */

static const D3D10_RASTERIZER_DESC g_fx_default_rs = {D3D10_FILL_SOLID, D3D10_CULL_BACK, FALSE, 0, 0.0f, 0.0f, TRUE, FALSE, FALSE, FALSE};
static const D3D10_DEPTH_STENCIL_DESC g_fx_default_ds = {TRUE, D3D10_DEPTH_WRITE_MASK_ALL, D3D10_COMPARISON_LESS, FALSE, 0xFF, 0xFF,
                                                         {D3D10_STENCIL_OP_KEEP, D3D10_STENCIL_OP_KEEP, D3D10_STENCIL_OP_KEEP, D3D10_COMPARISON_ALWAYS},
                                                         {D3D10_STENCIL_OP_KEEP, D3D10_STENCIL_OP_KEEP, D3D10_STENCIL_OP_KEEP, D3D10_COMPARISON_ALWAYS}};
static const D3D10_BLEND_DESC g_fx_default_blend = {FALSE, {FALSE, FALSE, FALSE, FALSE, FALSE, FALSE, FALSE, FALSE}, D3D10_BLEND_SRC_ALPHA, D3D10_BLEND_INV_SRC_ALPHA,
                                                    D3D10_BLEND_OP_ADD, D3D10_BLEND_SRC_ALPHA, D3D10_BLEND_INV_SRC_ALPHA, D3D10_BLEND_OP_ADD,
                                                    {0xF, 0xF, 0xF, 0xF, 0xF, 0xF, 0xF, 0xF}};
#define FX_FLT_MAX 3.402823466e+38f
static const D3D10_SAMPLER_DESC g_fx_default_sampler = {D3D10_FILTER_MIN_MAG_MIP_POINT, D3D10_TEXTURE_ADDRESS_WRAP, D3D10_TEXTURE_ADDRESS_WRAP, D3D10_TEXTURE_ADDRESS_WRAP, 0.0f,
                                                        16, D3D10_COMPARISON_NEVER, {0.0f, 0.0f, 0.0f, 0.0f}, 0.0f, FX_FLT_MAX};
static UINT fx_state_container(D3D10_SHADER_VARIABLE_TYPE t) {
  return t == D3D10_SVT_BLEND ? FX4_IN_BLEND : t == D3D10_SVT_DEPTHSTENCIL ? FX4_IN_DEPTHSTENCIL : t == D3D10_SVT_RASTERIZER ? FX4_IN_RASTERIZER : FX4_IN_SAMPLER;
}
/* Whether a Direct3D 10.1 blend description needs a blend of each target - what a device of feature level 10.0 cannot do: a target that blends with another blend
 * than the first target's. (Targets that do not blend, and the write masks, are Direct3D 10's too.) */
static BOOL fx_blend_independent(const D3D10_BLEND_DESC1 *b) {
  const D3D10_RENDER_TARGET_BLEND_DESC1 *first = &b->RenderTarget[0];
  for (UINT i = 1; i < 8; ++i) {
    const D3D10_RENDER_TARGET_BLEND_DESC1 *t = &b->RenderTarget[i];
    if (!t->BlendEnable) continue;
    if (t->SrcBlend != first->SrcBlend || t->DestBlend != first->DestBlend || t->BlendOp != first->BlendOp || t->SrcBlendAlpha != first->SrcBlendAlpha ||
        t->DestBlendAlpha != first->DestBlendAlpha || t->BlendOpAlpha != first->BlendOpAlpha)
      return TRUE;
  }
  return FALSE;
}
#define FX_E_NEEDS_10_1 ((HRESULT)0x80004005u)      /* (E_FAIL: a blend state a feature level 10.0 device cannot have) */
/* (re)makes the device's object of a state's description */
static HRESULT fx_state_create(FxEffect *e, FxVar *v) {
  FxState *s = v->state;
  IUnknown *object = NULL;
  HRESULT hr;
  switch (v->type->base) {
    case D3D10_SVT_RASTERIZER: hr = ID3D10Device_CreateRasterizerState(e->device, &s->desc.rs, (ID3D10RasterizerState **)&object); break;
    case D3D10_SVT_DEPTHSTENCIL: hr = ID3D10Device_CreateDepthStencilState(e->device, &s->desc.ds, (ID3D10DepthStencilState **)&object); break;
    case D3D10_SVT_SAMPLER: hr = ID3D10Device_CreateSamplerState(e->device, &s->desc.sampler.desc, (ID3D10SamplerState **)&object); break;
    case D3D10_SVT_BLEND:
      if (s->ten_one && e->level_10_0) {
        /* (feature level 10.0: Direct3D 10's blend - the first target's for all - or nothing; Windows makes the variable invalid then) */
        if (fx_blend_independent(&s->desc.blend1)) return FX_E_NEEDS_10_1;
        D3D10_BLEND_DESC plain;
        fx_blend_desc(s, &plain);
        hr = ID3D10Device_CreateBlendState(e->device, &plain, (ID3D10BlendState **)&object);
      } else if (s->ten_one) {
        ID3D10Device1 *d1 = NULL;
        hr = ID3D10Device_QueryInterface(e->device, &IID_ID3D10Device1, (void **)&d1);
        if (SUCCEEDED(hr)) { hr = ID3D10Device1_CreateBlendState1(d1, &s->desc.blend1, (ID3D10BlendState1 **)&object); ID3D10Device1_Release(d1); }
      } else {
        hr = ID3D10Device_CreateBlendState(e->device, &s->desc.blend, (ID3D10BlendState **)&object);
      }
      break;
    default: return E_FAIL;
  }
  if (FAILED(hr)) return hr;
  fx_release(&s->object);
  s->object = object;
  memcpy(&s->made, &s->desc, sizeof s->made);
  return S_OK;
}
/* A state's description from its dependencies now, and its object made again when that changed it. */
static void fx_state_refresh(FxVar *v) {
  FxState *s = v->state;
  if (!s || !s->deps.count || !v->effect) return;
  fx_deps_apply(&s->deps, fx_state_container(v->type->base), &s->desc, s->ten_one);
  if (memcmp(&s->made, &s->desc, sizeof s->made)) fx_state_create(v->effect, v);      /* (a description the device refuses keeps the object it had) */
}
static HRESULT fx_make_state(FxEffect *e, FxVar *v, const Fx4Assignments *list) {
  FxState *s = mem_alloc(sizeof *s);
  if (!s) return E_OUTOFMEMORY;
  v->state = s;
  FxVar ***slot;
  UINT *used, count;
  switch (v->type->base) {
    case D3D10_SVT_RASTERIZER: s->desc.rs = g_fx_default_rs; slot = &e->rs_list; used = &e->rs_used; count = e->rs_count; break;
    case D3D10_SVT_DEPTHSTENCIL: s->desc.ds = g_fx_default_ds; slot = &e->ds_list; used = &e->ds_used; count = e->ds_count; break;
    case D3D10_SVT_SAMPLER: s->desc.sampler.desc = g_fx_default_sampler; slot = &e->sampler_list; used = &e->sampler_used; count = e->sampler_count; break;
    case D3D10_SVT_BLEND:
      slot = &e->blend_list; used = &e->blend_used; count = e->blend_count;
      if (e->ten_one && e->fx.version == FX4_VERSION_4_1) {
        /* (Direct3D 10.1's: a blend a target, and its defaults) */
        s->ten_one = TRUE;
        s->desc.blend1.IndependentBlendEnable = TRUE;
        for (UINT i = 0; i < 8; ++i) {
          D3D10_RENDER_TARGET_BLEND_DESC1 *t = &s->desc.blend1.RenderTarget[i];
          t->SrcBlend = t->SrcBlendAlpha = D3D10_BLEND_ONE;
          t->DestBlend = t->DestBlendAlpha = D3D10_BLEND_ZERO;
          t->BlendOp = t->BlendOpAlpha = D3D10_BLEND_OP_ADD;
          t->RenderTargetWriteMask = D3D10_COLOR_WRITE_ENABLE_ALL;
        }
      } else {
        s->desc.blend = g_fx_default_blend;
      }
      break;
    default: return E_FAIL;
  }
  if (*used >= count) return E_FAIL;
  s->index = *used;
  (*slot)[(*used)++] = v;
  const UINT container = fx_state_container(v->type->base);
  for (UINT i = 0; i < list->count; ++i) {
    const HRESULT hr = fx_assign(e, container, &s->desc, s->ten_one, &list->items[i], &s->deps);
    if (FAILED(hr)) return hr;
  }
  const HRESULT hr = fx_state_create(e, v);
  if (hr == FX_E_NEEDS_10_1 && s->ten_one && e->level_10_0) { v->null = TRUE; return S_OK; }      /* (the effect is made; this variable is not valid) */
  return hr;
}

/* ---- ID3D10EffectPass ------------------------------------------------------------------------------------------------------------------------------------------------ */

static void fx_pass_refresh(FxPass *p) { if (!p->null) fx_deps_apply(&p->deps, FX4_IN_PASS, p, FALSE); }
static BOOL STDMETHODCALLTYPE fxpass_IsValid(FxPass *p) { return !p->null; }
static HRESULT STDMETHODCALLTYPE fxpass_GetDesc(FxPass *p, D3D10_PASS_DESC *desc) {
  if (p->null) return E_FAIL;
  if (!desc) return E_INVALIDARG;
  fx_pass_refresh(p);
  const FxVar *vs = fx_element(p->vs, p->vs_index);
  const ID3D10Blob *sig = vs->shader ? vs->shader->input_signature : NULL;
  desc->Name = p->name;
  desc->Annotations = p->annotations.count;
  desc->pIAInputSignature = sig ? ID3D10Blob_GetBufferPointer((ID3D10Blob *)sig) : NULL;
  desc->IAInputSignatureSize = sig ? ID3D10Blob_GetBufferSize((ID3D10Blob *)sig) : 0;
  desc->StencilRef = p->stencil_ref;
  desc->SampleMask = p->sample_mask;
  memcpy(desc->BlendFactor, p->blend_factor, sizeof desc->BlendFactor);
  return S_OK;
}
static HRESULT fx_pass_shader_desc(FxPass *p, FxVar *const *v, const UINT *index, D3D10_PASS_SHADER_DESC *desc) {
  if (p->null) return E_FAIL;
  if (!desc) return E_INVALIDARG;
  fx_pass_refresh(p);
  desc->pShaderVariable = (ID3D10EffectShaderVariable *)*v;
  desc->ShaderIndex = *index;
  return S_OK;
}
static HRESULT STDMETHODCALLTYPE fxpass_GetVertexShaderDesc(FxPass *p, D3D10_PASS_SHADER_DESC *desc) { return fx_pass_shader_desc(p, &p->vs, &p->vs_index, desc); }
static HRESULT STDMETHODCALLTYPE fxpass_GetGeometryShaderDesc(FxPass *p, D3D10_PASS_SHADER_DESC *desc) { return fx_pass_shader_desc(p, &p->gs, &p->gs_index, desc); }
static HRESULT STDMETHODCALLTYPE fxpass_GetPixelShaderDesc(FxPass *p, D3D10_PASS_SHADER_DESC *desc) { return fx_pass_shader_desc(p, &p->ps, &p->ps_index, desc); }
static ID3D10EffectVariable *STDMETHODCALLTYPE fxpass_GetAnnotationByIndex(FxPass *p, UINT index) { return (ID3D10EffectVariable *)fx_annotation_by_index(&p->annotations, index); }
static ID3D10EffectVariable *STDMETHODCALLTYPE fxpass_GetAnnotationByName(FxPass *p, const char *name) { return (ID3D10EffectVariable *)fx_annotation_by_name(&p->annotations, name); }

/* A constant buffer's memory into its Direct3D 10 buffer when it changed (not one the program set: that one is the program's). */
static void fx_buffer_upload(FxEffect *e, FxVar *b) {
  if (!b->changed || b->user_buffer || !b->d3d) return;
  ID3D10Device_UpdateSubresource(e->device, (ID3D10Resource *)b->d3d, 0, NULL, b->data, b->size, 0);
  b->changed = FALSE;
}
/* The resources of a shader into the device's slots of its stage: its constant and texture buffers, its textures' views, its samplers' objects. */
static void fx_apply_bindings(FxEffect *e, const FxShader *s) {
  ID3D10Device *d = e->device;
  for (UINT i = 0; i < s->binding_count; ++i) {
    const FxBinding *b = &s->bindings[i];
    FxVar *v = b->var;
    switch (b->type) {
      case D3D10_SIT_CBUFFER: {
        fx_buffer_upload(e, v);
        ID3D10Buffer *buf = v->user_buffer ? v->user_buffer : v->d3d;
        if (s->kind == D3D10_SVT_VERTEXSHADER) ID3D10Device_VSSetConstantBuffers(d, b->point, 1, &buf);
        else if (s->kind == D3D10_SVT_GEOMETRYSHADER) ID3D10Device_GSSetConstantBuffers(d, b->point, 1, &buf);
        else ID3D10Device_PSSetConstantBuffers(d, b->point, 1, &buf);
        break;
      }
      case D3D10_SIT_TBUFFER: case D3D10_SIT_TEXTURE: {
        ID3D10ShaderResourceView *one = NULL, **views = &one;
        UINT count = 1;
        if (b->type == D3D10_SIT_TBUFFER) {
          fx_buffer_upload(e, v);
          one = v->user_srv ? v->user_srv : v->srv;
        } else {
          if (v->kind == FXV_STATE && v->state) v = v->state->desc.sampler.texture;      /* (a sampler's texture, of the older syntax) */
          if (v && v->null) v = NULL;      /* (Texture = NULL: the null variable) */
          if (v) {
            if (!v->views) break;
            const UINT slots = v->owns_views && v->type->elements ? v->type->elements : 1;
            views = (ID3D10ShaderResourceView **)v->views;
            count = b->count < slots ? b->count : slots;
          }      /* (a sampler whose Texture is NULL: the slot is reset, as Windows') */
        }
        if (s->kind == D3D10_SVT_VERTEXSHADER) ID3D10Device_VSSetShaderResources(d, b->point, count, views);
        else if (s->kind == D3D10_SVT_GEOMETRYSHADER) ID3D10Device_GSSetShaderResources(d, b->point, count, views);
        else ID3D10Device_PSSetShaderResources(d, b->point, count, views);
        break;
      }
      case D3D10_SIT_SAMPLER:
        for (UINT k = 0; k < b->count; ++k) {
          FxVar *one = v->type->elements ? (k < v->type->elements ? &v->elements[k] : NULL) : (k ? NULL : v);
          if (!one || !one->state) continue;
          fx_state_refresh(one);
          ID3D10SamplerState *state = (ID3D10SamplerState *)one->state->object;
          if (s->kind == D3D10_SVT_VERTEXSHADER) ID3D10Device_VSSetSamplers(d, b->point + k, 1, &state);
          else if (s->kind == D3D10_SVT_GEOMETRYSHADER) ID3D10Device_GSSetSamplers(d, b->point + k, 1, &state);
          else ID3D10Device_PSSetSamplers(d, b->point + k, 1, &state);
        }
        break;
      default: break;
    }
  }
}
static void fx_apply_shader(FxEffect *e, FxVar *var, UINT index, D3D10_SHADER_VARIABLE_TYPE kind) {
  const FxVar *v = fx_element(var, index);
  const FxShader *s = v->shader;
  IUnknown *object = s ? s->object : NULL;
  if (kind == D3D10_SVT_VERTEXSHADER) ID3D10Device_VSSetShader(e->device, (ID3D10VertexShader *)object);
  else if (kind == D3D10_SVT_GEOMETRYSHADER) ID3D10Device_GSSetShader(e->device, (ID3D10GeometryShader *)object);
  else ID3D10Device_PSSetShader(e->device, (ID3D10PixelShader *)object);
  if (s) fx_apply_bindings(e, s);
}
static IUnknown *fx_state_object_now(FxVar *v) {
  if (!v || !v->state) return NULL;
  fx_state_refresh(v);
  return v->state->object;
}
/* (flags: none are defined) */
static HRESULT STDMETHODCALLTYPE fxpass_Apply(FxPass *p, UINT flags) {
  if (p->null) return E_FAIL;
  if (flags) return E_INVALIDARG;
  FxEffect *e = p->technique->effect;
  ID3D10Device *d = e->device;
  fx_pass_refresh(p);
  if (p->vs != &g_fx_null_shader) fx_apply_shader(e, p->vs, p->vs_index, D3D10_SVT_VERTEXSHADER);
  if (p->gs != &g_fx_null_shader) fx_apply_shader(e, p->gs, p->gs_index, D3D10_SVT_GEOMETRYSHADER);
  if (p->ps != &g_fx_null_shader) fx_apply_shader(e, p->ps, p->ps_index, D3D10_SVT_PIXELSHADER);
  if (p->rs) ID3D10Device_RSSetState(d, (ID3D10RasterizerState *)fx_state_object_now(p->rs));
  if (p->ds) ID3D10Device_OMSetDepthStencilState(d, (ID3D10DepthStencilState *)fx_state_object_now(p->ds), p->stencil_ref);
  if (p->blend) ID3D10Device_OMSetBlendState(d, (ID3D10BlendState *)fx_state_object_now(p->blend), p->blend_factor, p->sample_mask);
  if (p->views_set) {
    ID3D10RenderTargetView *rtv[D3D10_SIMULTANEOUS_RENDER_TARGET_COUNT];
    for (UINT i = 0; i < p->rtv_count; ++i) rtv[i] = p->rtv[i] && p->rtv[i]->views ? (ID3D10RenderTargetView *)p->rtv[i]->views[0] : NULL;
    ID3D10DepthStencilView *dsv = p->dsv && p->dsv->views ? (ID3D10DepthStencilView *)p->dsv->views[0] : NULL;
    ID3D10Device_OMSetRenderTargets(d, p->rtv_count, rtv, dsv);
  }
  return S_OK;
}
/* The state a pass changes: its shaders and their resources' slots, its states, its targets. */
static void fx_mask_shader(D3D10_STATE_BLOCK_MASK *mask, FxVar *var, UINT index, D3D10_SHADER_VARIABLE_TYPE kind) {
  const D3D10_DEVICE_STATE_TYPES stage = kind == D3D10_SVT_VERTEXSHADER ? D3D10_DST_VS : kind == D3D10_SVT_GEOMETRYSHADER ? D3D10_DST_GS : D3D10_DST_PS;
  D3D10StateBlockMaskEnableCapture(mask, stage, 0, 1);
  const FxShader *s = fx_element(var, index)->shader;
  for (UINT i = 0; s && i < s->binding_count; ++i) {
    const FxBinding *b = &s->bindings[i];
    if (b->type == D3D10_SIT_CBUFFER) D3D10StateBlockMaskEnableCapture(mask, (D3D10_DEVICE_STATE_TYPES)(stage + 3), b->point, 1);
    else if (b->type == D3D10_SIT_SAMPLER) D3D10StateBlockMaskEnableCapture(mask, (D3D10_DEVICE_STATE_TYPES)(stage + 1), b->point, b->count);
    else D3D10StateBlockMaskEnableCapture(mask, (D3D10_DEVICE_STATE_TYPES)(stage + 2), b->point, b->type == D3D10_SIT_TBUFFER ? 1 : b->count);
  }
}
static void fx_pass_mask(FxPass *p, D3D10_STATE_BLOCK_MASK *mask) {
  fx_pass_refresh(p);
  if (p->vs != &g_fx_null_shader) fx_mask_shader(mask, p->vs, p->vs_index, D3D10_SVT_VERTEXSHADER);
  if (p->gs != &g_fx_null_shader) fx_mask_shader(mask, p->gs, p->gs_index, D3D10_SVT_GEOMETRYSHADER);
  if (p->ps != &g_fx_null_shader) fx_mask_shader(mask, p->ps, p->ps_index, D3D10_SVT_PIXELSHADER);
  if (p->rs) D3D10StateBlockMaskEnableCapture(mask, D3D10_DST_RS_RASTERIZER_STATE, 0, 1);
  if (p->ds) D3D10StateBlockMaskEnableCapture(mask, D3D10_DST_OM_DEPTH_STENCIL_STATE, 0, 1);
  if (p->blend) D3D10StateBlockMaskEnableCapture(mask, D3D10_DST_OM_BLEND_STATE, 0, 1);
  if (p->views_set) D3D10StateBlockMaskEnableCapture(mask, D3D10_DST_OM_RENDER_TARGETS, 0, 1);
}
static HRESULT STDMETHODCALLTYPE fxpass_ComputeStateBlockMask(FxPass *p, D3D10_STATE_BLOCK_MASK *mask) {
  if (p->null) return E_FAIL;
  if (!mask) return E_INVALIDARG;
  fx_pass_mask(p, mask);
  return S_OK;
}

/* ---- ID3D10EffectTechnique ------------------------------------------------------------------------------------------------------------------------------------------- */

static BOOL STDMETHODCALLTYPE fxt_IsValid(FxTechnique *t) { return !t->null; }
static HRESULT STDMETHODCALLTYPE fxt_GetDesc(FxTechnique *t, D3D10_TECHNIQUE_DESC *desc) {
  if (t->null) return E_FAIL;
  if (!desc) return E_INVALIDARG;
  desc->Name = t->name;
  desc->Passes = t->pass_count;
  desc->Annotations = t->annotations.count;
  return S_OK;
}
static ID3D10EffectVariable *STDMETHODCALLTYPE fxt_GetAnnotationByIndex(FxTechnique *t, UINT index) { return (ID3D10EffectVariable *)fx_annotation_by_index(&t->annotations, index); }
static ID3D10EffectVariable *STDMETHODCALLTYPE fxt_GetAnnotationByName(FxTechnique *t, const char *name) { return (ID3D10EffectVariable *)fx_annotation_by_name(&t->annotations, name); }
static ID3D10EffectPass *STDMETHODCALLTYPE fxt_GetPassByIndex(FxTechnique *t, UINT index) {
  return (ID3D10EffectPass *)(index < t->pass_count ? &t->passes[index] : &g_fx_null_pass);
}
static ID3D10EffectPass *STDMETHODCALLTYPE fxt_GetPassByName(FxTechnique *t, const char *name) {
  for (UINT i = 0; name && i < t->pass_count; ++i) if (fx_name_is(t->passes[i].name, name)) return (ID3D10EffectPass *)&t->passes[i];
  return (ID3D10EffectPass *)&g_fx_null_pass;
}
static HRESULT STDMETHODCALLTYPE fxt_ComputeStateBlockMask(FxTechnique *t, D3D10_STATE_BLOCK_MASK *mask) {
  if (t->null) return E_FAIL;
  if (!mask) return E_INVALIDARG;
  for (UINT i = 0; i < t->pass_count; ++i) fx_pass_mask(&t->passes[i], mask);
  return S_OK;
}

/* ---- releasing an effect ---------------------------------------------------------------------------------------------------------------------------------------------- */

static void fx_annotations_free(FxAnnotations *a);
static ULONG fxeffect_release(FxEffect *e);
static void fx_var_free(FxVar *v) {
  /* (an element's views are its array's: only the array gives them back) */
  if (v->owns_views && v->views) {
    const UINT slots = v->type->elements ? v->type->elements : 1;
    for (UINT i = 0; i < slots; ++i) fx_release(&v->views[i]);
    mem_free(v->views);
  }
  v->views = NULL;
  mem_free(v->owned_string);
  v->owned_string = NULL;
  fx_shader_free(v->shader);
  v->shader = NULL;
  if (v->state) { fx_release(&v->state->object); fx_deps_free(&v->state->deps); mem_free(v->state); v->state = NULL; }
  fx_annotations_free(&v->annotations);
  if (v->elements) for (UINT i = 0; i < v->type->elements; ++i) fx_var_free(&v->elements[i]);
  if (v->members) for (UINT i = 0; i < v->type->member_count; ++i) fx_var_free(&v->members[i]);
  mem_free(v->elements);
  mem_free(v->members);
  v->elements = v->members = NULL;
}
static void fx_annotations_free(FxAnnotations *a) {
  for (UINT i = 0; i < a->count; ++i) {
    FxVar *v = &a->items[i];
    if (v->kind == FXV_NUMERIC && v->buffer && v->buffer != &g_fx_null_cb) { mem_free(v->buffer->data); mem_free(v->buffer); }
    fx_var_free(v);
  }
  mem_free(a->items);
  a->items = NULL;
  a->count = 0;
}
static void fx_destroy(FxEffect *e) {
  for (UINT i = 0; i < e->technique_count; ++i) {
    FxTechnique *t = &e->techniques[i];
    for (UINT k = 0; t->passes && k < t->pass_count; ++k) { fx_annotations_free(&t->passes[k].annotations); fx_deps_free(&t->passes[k].deps); }
    mem_free(t->passes);
    fx_annotations_free(&t->annotations);
  }
  mem_free(e->techniques);
  for (UINT i = 0; i < e->object_count; ++i) fx_var_free(&e->objects[i]);
  mem_free(e->objects);
  for (UINT i = 0; i < e->anonymous_count; ++i) fx_var_free(&e->anonymous[i]);
  mem_free(e->anonymous);
  mem_free(e->anonymous_types);
  for (UINT i = 0; i < e->buffer_count; ++i) {
    FxVar *b = &e->buffers[i];
    fx_var_free(b);
    fx_release(&b->d3d);
    fx_release(&b->srv);
    fx_release(&b->user_buffer);
    fx_release(&b->user_srv);
    mem_free(b->data);
  }
  mem_free(e->buffers);
  if (e->buffer_types) for (UINT i = 0; i < e->buffer_count; ++i) mem_free(e->buffer_types[i].members);
  mem_free(e->buffer_types);
  if (e->types) for (UINT i = 0; i < e->type_count; ++i) mem_free(e->types[i].members);
  mem_free(e->types);
  mem_free(e->shader_list);
  mem_free(e->blend_list);
  mem_free(e->ds_list);
  mem_free(e->rs_list);
  mem_free(e->sampler_list);
  fx4_free(&e->fx);
  mem_free(e->copy);
  if (e->pool) fxeffect_release(e->pool);
  if (e->device) ID3D10Device_Release(e->device);
  mem_free(e);
}

/* ---- ID3D10Effect, ID3D10EffectPool ------------------------------------------------------------------------------------------------------------------------------------ */

static FxEffect *fx_of_pool(ID3D10EffectPool *p) { return (FxEffect *)((BYTE *)p - offsetof(FxEffect, pool_vtbl)); }
static ULONG fxeffect_release(FxEffect *e) {
  const LONG n = InterlockedDecrement(&e->ref);
  if (!n) fx_destroy(e);
  return (ULONG)n;
}
/* (Windows answers only ID3D10EffectPool: of an effect, the effect's own interface; of a pool, the pool) */
static HRESULT STDMETHODCALLTYPE fx_QueryInterface(FxEffect *e, REFIID riid, void **out) {
  if (!out) return E_POINTER;
  *out = NULL;
  if (!guid_eq(riid, &IID_ID3D10EffectPool)) return E_NOINTERFACE;
  InterlockedIncrement(&e->ref);
  *out = e->is_pool ? (void *)&e->pool_vtbl : (void *)e;
  return S_OK;
}
static ULONG STDMETHODCALLTYPE fx_AddRef(FxEffect *e) { return (ULONG)InterlockedIncrement(&e->ref); }
static ULONG STDMETHODCALLTYPE fx_Release(FxEffect *e) { return fxeffect_release(e); }
static BOOL STDMETHODCALLTYPE fx_IsValid(FxEffect *e) { (void)e; return TRUE; }
static BOOL STDMETHODCALLTYPE fx_IsPool(FxEffect *e) { return e->is_pool; }
static HRESULT STDMETHODCALLTYPE fx_GetDevice(FxEffect *e, ID3D10Device **out) {
  if (!out) return E_INVALIDARG;
  ID3D10Device_AddRef(e->device);
  *out = e->device;
  return S_OK;
}
static void fx_desc_of(const FxEffect *e, D3D10_EFFECT_DESC *desc) {
  desc->IsChildEffect = e->pool != NULL;
  desc->ConstantBuffers = e->buffer_count;
  desc->SharedConstantBuffers = 0;
  desc->GlobalVariables = e->object_count + e->fx.numeric_count;
  desc->SharedGlobalVariables = 0;
  desc->Techniques = e->technique_count;
}
static HRESULT STDMETHODCALLTYPE fx_GetDesc(FxEffect *e, D3D10_EFFECT_DESC *desc) {
  if (!desc) return E_INVALIDARG;
  fx_desc_of(e, desc);
  if (e->pool) {
    D3D10_EFFECT_DESC pool;
    fx_desc_of(e->pool, &pool);
    desc->SharedConstantBuffers = pool.ConstantBuffers;
    desc->SharedGlobalVariables = pool.GlobalVariables;
  }
  return S_OK;
}
static ID3D10EffectConstantBuffer *STDMETHODCALLTYPE fx_GetConstantBufferByIndex(FxEffect *e, UINT index) {
  for (; e; e = e->pool) {
    if (index < e->buffer_count) return (ID3D10EffectConstantBuffer *)&e->buffers[index];
    index -= e->buffer_count;
  }
  return (ID3D10EffectConstantBuffer *)&g_fx_null_cb;
}
static ID3D10EffectConstantBuffer *STDMETHODCALLTYPE fx_GetConstantBufferByName(FxEffect *e, const char *name) {
  FxVar *b = name ? fx_find_buffer(e, name) : NULL;
  return (ID3D10EffectConstantBuffer *)(b ? b : &g_fx_null_cb);
}
static ID3D10EffectVariable *STDMETHODCALLTYPE fx_GetVariableByIndex(FxEffect *e, UINT index) {
  for (; e; e = e->pool) {
    for (UINT i = 0; i < e->buffer_count; ++i) {
      if (index < e->buffers[i].type->member_count) return (ID3D10EffectVariable *)&e->buffers[i].members[index];
      index -= e->buffers[i].type->member_count;
    }
    if (index < e->object_count) return (ID3D10EffectVariable *)&e->objects[index];
    index -= e->object_count;
  }
  return (ID3D10EffectVariable *)&g_fx_null_var;
}
static ID3D10EffectVariable *STDMETHODCALLTYPE fx_GetVariableByName(FxEffect *e, const char *name) {
  FxVar *v = fx_find_var(e, name);
  return (ID3D10EffectVariable *)(v ? v : &g_fx_null_var);
}
static ID3D10EffectVariable *STDMETHODCALLTYPE fx_GetVariableBySemantic(FxEffect *e, const char *semantic) {
  for (; e && semantic; e = e->pool) {
    for (UINT i = 0; i < e->buffer_count; ++i)
      for (UINT k = 0; k < e->buffers[i].type->member_count; ++k) {
        FxVar *v = &e->buffers[i].members[k];
        if (v->semantic && !fx_stricmp(v->semantic, semantic)) return (ID3D10EffectVariable *)v;
      }
    for (UINT i = 0; i < e->object_count; ++i)
      if (e->objects[i].semantic && !fx_stricmp(e->objects[i].semantic, semantic)) return (ID3D10EffectVariable *)&e->objects[i];
  }
  return (ID3D10EffectVariable *)&g_fx_null_var;
}
static ID3D10EffectTechnique *STDMETHODCALLTYPE fx_GetTechniqueByIndex(FxEffect *e, UINT index) {
  return (ID3D10EffectTechnique *)(index < e->technique_count ? &e->techniques[index] : &g_fx_null_technique);
}
static ID3D10EffectTechnique *STDMETHODCALLTYPE fx_GetTechniqueByName(FxEffect *e, const char *name) {
  for (UINT i = 0; name && i < e->technique_count; ++i) if (fx_name_is(e->techniques[i].name, name)) return (ID3D10EffectTechnique *)&e->techniques[i];
  return (ID3D10EffectTechnique *)&g_fx_null_technique;
}
/* What Optimize gives up, as Windows: the shaders' reflection, bytecode and stream output declarations, the names of techniques and passes, every annotation. The shaders,
 * their input signatures and everything a pass applies stay. */
static void fx_drop_annotations(FxAnnotations *a) { a->count = 0; }
static void fx_drop_var_annotations(FxVar *v) {
  fx_drop_annotations(&v->annotations);
  if (v->elements) for (UINT i = 0; i < v->type->elements; ++i) fx_drop_var_annotations(&v->elements[i]);
  if (v->members) for (UINT i = 0; i < v->type->member_count; ++i) fx_drop_var_annotations(&v->members[i]);
}
static HRESULT STDMETHODCALLTYPE fx_Optimize(FxEffect *e) {
  if (e->optimized) return S_OK;
  for (UINT i = 0; i < e->shader_used; ++i) {
    FxShader *s = e->shader_list[i]->shader;
    if (!s) continue;
    fx_release(&s->reflection);
    s->code = NULL;
    s->size = 0;
    s->so_decl = NULL;
  }
  for (UINT i = 0; i < e->technique_count; ++i) {
    FxTechnique *t = &e->techniques[i];
    t->name = NULL;
    fx_drop_annotations(&t->annotations);
    for (UINT k = 0; k < t->pass_count; ++k) { t->passes[k].name = NULL; fx_drop_annotations(&t->passes[k].annotations); }
  }
  for (UINT i = 0; i < e->buffer_count; ++i) fx_drop_var_annotations(&e->buffers[i]);
  for (UINT i = 0; i < e->object_count; ++i) fx_drop_var_annotations(&e->objects[i]);
  e->optimized = TRUE;
  return S_OK;
}
static BOOL STDMETHODCALLTYPE fx_IsOptimized(FxEffect *e) { return e->optimized; }

static HRESULT STDMETHODCALLTYPE fxpe_QueryInterface(FxEffect *e, REFIID riid, void **out) { return fx_QueryInterface(e, riid, out); }
static HRESULT STDMETHODCALLTYPE fxpool_QueryInterface(ID3D10EffectPool *p, REFIID riid, void **out) { return fx_QueryInterface(fx_of_pool(p), riid, out); }
static ULONG STDMETHODCALLTYPE fxpool_AddRef(ID3D10EffectPool *p) { return fx_AddRef(fx_of_pool(p)); }
static ULONG STDMETHODCALLTYPE fxpool_Release(ID3D10EffectPool *p) { return fxeffect_release(fx_of_pool(p)); }
static ID3D10Effect *STDMETHODCALLTYPE fxpool_AsEffect(ID3D10EffectPool *p) { return (ID3D10Effect *)fx_of_pool(p); }

/* ---- making an effect ------------------------------------------------------------------------------------------------------------------------------------------------- */

static D3D10_SHADER_VARIABLE_CLASS fx_class_of(const Fx4Type *t) {
  if (t->kind == FX4_KIND_OBJECT) return D3D10_SVC_OBJECT;
  if (t->kind == FX4_KIND_STRUCT) return D3D10_SVC_STRUCT;
  return t->numeric_class == FX4_CLASS_SCALAR ? D3D10_SVC_SCALAR : t->numeric_class == FX4_CLASS_VECTOR ? D3D10_SVC_VECTOR :
         t->column_major ? D3D10_SVC_MATRIX_COLUMNS : D3D10_SVC_MATRIX_ROWS;
}
/* Whether a child effect's description of a shared type is the pool's type (names, kinds, shapes, members). */
static BOOL fx_types_match(const Fx4Type *a, const FxType *b, UINT depth) {
  if (depth > FX4_MAX_DEPTH || b->null) return FALSE;
  if ((a->name || b->name) && (!a->name || !b->name || fx_strcmp(a->name, b->name))) return FALSE;
  const D3D10_SHADER_VARIABLE_TYPE base = a->kind == FX4_KIND_NUMERIC ? fx_numeric_type(a->numeric_base) : a->kind == FX4_KIND_OBJECT ? fx_object_type(a->object) : D3D10_SVT_VOID;
  if (base != b->base || fx_class_of(a) != b->cls || a->elements != b->elements) return FALSE;
  if (a->kind == FX4_KIND_NUMERIC && (a->rows != b->rows || a->columns != b->columns)) return FALSE;
  if (a->member_count != b->member_count) return FALSE;
  for (UINT i = 0; i < a->member_count; ++i) {
    const FxTypeMember *m = &b->members[i];
    if ((a->members[i].name || m->name) && (!a->members[i].name || !m->name || fx_strcmp(a->members[i].name, m->name))) return FALSE;
    if (a->members[i].offset != m->offset || !fx_types_match(a->members[i].type, m->type, depth + 1)) return FALSE;
  }
  return TRUE;
}

/* The annotations of the binary as variables (numbers in memory of their own, strings as they are). */
static HRESULT fx_make_annotations(FxEffect *e, const Fx4Annotations *src, FxAnnotations *out) {
  if (!src->count) return S_OK;
  if (!(out->items = fx_new_array(src->count, sizeof *out->items))) return E_OUTOFMEMORY;
  out->count = src->count;
  for (UINT i = 0; i < src->count; ++i) {
    const Fx4Annotation *a = &src->items[i];
    FxVar *v = &out->items[i];
    v->effect = e;
    v->buffer = &g_fx_null_cb;
    v->type = fx_type_of(e, a->type);
    v->name = a->name;
    v->flags = D3D10_EFFECT_VARIABLE_ANNOTATION | (e->is_pool ? D3D10_EFFECT_VARIABLE_POOLED : 0);
    v->explicit_bind = ~0u;
    if (a->type->kind == FX4_KIND_NUMERIC) {
      const UINT reach = fx4_footprint(a->type, 0);
      if (reach == 0xFFFFFFFFu) return E_FAIL;
      FxVar *store = mem_alloc(sizeof *store);
      BYTE *data = store ? mem_alloc(reach) : NULL;
      if (!data) { mem_free(store); return E_OUTOFMEMORY; }
      store->kind = FXV_BUFFER;
      store->lpVtbl = &fxcb_vtbl;
      store->null = TRUE;      /* (no constant buffer of the effect: what GetParentConstantBuffer says) */
      store->type = &g_fx_null_type;
      store->data = data;
      store->size = reach;
      fx4_unpack(a->type, a->values, a->type->packed, data, 0);
      v->buffer = store;
      v->kind = FXV_NUMERIC;
      v->lpVtbl = fx_numeric_vtbl(v->type);
      if (!fx_var_tree(v, 0)) return E_OUTOFMEMORY;
    } else {
      fx_object_kind(v);
      if (!fx_var_tree(v, 0)) return E_OUTOFMEMORY;
      if (!fx_set_string(v, a->strings[0])) return E_OUTOFMEMORY;
      for (UINT k = 0; v->elements && k < v->type->elements; ++k) if (!fx_set_string(&v->elements[k], a->strings[k])) return E_OUTOFMEMORY;
    }
  }
  return S_OK;
}

/* A local constant buffer: its type (the binary has none: "cbuffer" or "tbuffer", its members its variables, its stride worked out as the compiler lays variables out),
 * its memory with the defaults, its Direct3D 10 buffer (and view, a texture buffer's). */
static HRESULT fx_make_buffer(FxEffect *e, UINT index) {
  const Fx4Buffer *src = &e->fx.buffers[index];
  FxVar *b = &e->buffers[index];
  FxType *t = &e->buffer_types[index];
  HRESULT hr;
  t->lpVtbl = &fxtype_vtbl;
  t->name = src->tbuffer ? "tbuffer" : "cbuffer";
  t->base = src->tbuffer ? D3D10_SVT_TBUFFER : D3D10_SVT_CBUFFER;
  t->cls = D3D10_SVC_OBJECT;
  t->member_count = src->count;
  b->lpVtbl = &fxcb_vtbl;
  b->kind = FXV_BUFFER;
  b->effect = e;
  b->buffer = &g_fx_null_cb;
  b->type = t;
  b->name = src->name;
  b->flags = e->is_pool ? D3D10_EFFECT_VARIABLE_POOLED : 0;
  b->explicit_bind = src->bind;
  if (src->bind != ~0u) b->flags |= D3D10_EFFECT_VARIABLE_EXPLICIT_BIND_POINT;
  b->size = src->size;
  if (src->size && !(b->data = mem_alloc(src->size))) return E_OUTOFMEMORY;
  if (FAILED(hr = fx_make_annotations(e, &src->annotations, &b->annotations))) return hr;
  if (src->count) {
    if (!(t->members = fx_new_array(src->count, sizeof *t->members)) || !(b->members = fx_new_array(src->count, sizeof *b->members))) return E_OUTOFMEMORY;
  }
  UINT stride = 0;
  for (UINT i = 0; i < src->count; ++i) {
    const Fx4Numeric *n = &src->vars[i];
    FxVar *v = &b->members[i];
    v->effect = e;
    v->buffer = b;
    v->type = fx_type_of(e, n->type);
    v->name = n->name;
    v->semantic = n->semantic;
    v->buffer_offset = n->offset;
    v->flags = n->flags | (e->is_pool ? D3D10_EFFECT_VARIABLE_POOLED : 0);
    v->explicit_bind = (v->flags & D3D10_EFFECT_VARIABLE_EXPLICIT_BIND_POINT) ? n->offset : ~0u;
    v->kind = FXV_NUMERIC;
    v->lpVtbl = fx_numeric_vtbl(v->type);
    if (!fx_var_tree(v, 0)) return E_OUTOFMEMORY;
    if (FAILED(hr = fx_make_annotations(e, &n->annotations, &v->annotations))) return hr;
    if (n->defaults && b->data) fx4_unpack(n->type, n->defaults, (UINT)fx4_packed_bytes(n->type, 0), b->data + n->offset, 0);
    t->members[i].name = n->name;
    t->members[i].semantic = n->semantic;
    t->members[i].offset = n->offset;
    t->members[i].type = v->type;
    t->packed += v->type->packed;
    /* (the stride: a packoffset's variable ends where it ends; the others fill registers in order - one that would cross a register starts the next, a struct starts a
     * register of its own - and the whole is registers) */
    if (v->flags & D3D10_EFFECT_VARIABLE_EXPLICIT_BIND_POINT) {
      if (v->type->unpacked + v->buffer_offset > stride) stride = v->type->unpacked + v->buffer_offset;
    } else {
      if (v->type->cls == D3D10_SVC_STRUCT) stride = (stride + 15) & ~15u;
      if ((stride & 15) + v->type->unpacked > 16) stride = (stride + 15) & ~15u;
      stride += v->type->unpacked;
    }
  }
  t->stride = t->unpacked = (stride + 15) & ~15u;
  if (src->size) {
    const D3D10_BUFFER_DESC desc = {src->size, D3D10_USAGE_DEFAULT, src->tbuffer ? D3D10_BIND_SHADER_RESOURCE : D3D10_BIND_CONSTANT_BUFFER, 0, 0};
    const D3D10_SUBRESOURCE_DATA data = {b->data, 0, 0};
    if (FAILED(hr = ID3D10Device_CreateBuffer(e->device, &desc, &data, &b->d3d))) return hr;
    if (src->tbuffer) {
      D3D10_SHADER_RESOURCE_VIEW_DESC v;
      memset(&v, 0, sizeof v);
      v.Format = DXGI_FORMAT_R32G32B32A32_UINT;
      v.ViewDimension = D3D10_SRV_DIMENSION_BUFFER;
      v.Buffer.ElementOffset = 0;
      v.Buffer.ElementWidth = t->unpacked / 16;
      if (FAILED(hr = ID3D10Device_CreateShaderResourceView(e->device, (ID3D10Resource *)b->d3d, &v, &b->srv))) return hr;
    }
  }
  return S_OK;
}

static HRESULT fx_make_object(FxEffect *e, UINT index) {
  const Fx4Object *src = &e->fx.objects[index];
  FxVar *v = &e->objects[index];
  HRESULT hr;
  v->effect = e;
  v->buffer = &g_fx_null_cb;
  v->type = fx_type_of(e, src->type);
  v->name = src->name;
  v->semantic = src->semantic;
  v->explicit_bind = src->bind;
  v->flags = e->is_pool ? D3D10_EFFECT_VARIABLE_POOLED : 0;
  fx_object_kind(v);
  if (!fx_var_tree(v, 0)) return E_OUTOFMEMORY;
  const UINT n = src->element_count;
  if (v->kind == FXV_RESOURCE || v->kind == FXV_RTV || v->kind == FXV_DSV) {
    if (!(v->views = fx_new_array(n, sizeof *v->views))) return E_OUTOFMEMORY;
    v->owns_views = TRUE;
    for (UINT i = 0; v->elements && i < n; ++i) v->elements[i].views = &v->views[i];
  }
  for (UINT i = 0; i < n; ++i) {
    FxVar *el = fx_element(v, i);
    const Fx4Element *s = &src->elements[i];
    if (v->kind == FXV_STRING) { if (!fx_set_string(el, s->string)) return E_OUTOFMEMORY; }
    else if (v->kind == FXV_SHADER) { if (FAILED(hr = fx_make_shader(e, el, &s->shader, FALSE))) return hr; }
    else if (v->kind == FXV_STATE) {
      if (FAILED(hr = fx_make_state(e, el, &s->state))) return hr;
      if (el->null) v->null = TRUE;      /* (an element a feature level 10.0 device cannot have: the variable is invalid, as Windows') */
    }
  }
  if (v->kind == FXV_STRING && v->elements && !fx_set_string(v, src->elements[0].string)) return E_OUTOFMEMORY;
  return fx_make_annotations(e, &src->annotations, &v->annotations);
}

static HRESULT fx_make_technique(FxEffect *e, UINT index) {
  const Fx4Technique *src = &e->fx.techniques[index];
  FxTechnique *t = &e->techniques[index];
  HRESULT hr;
  t->lpVtbl = &fxt_vtbl;
  t->effect = e;
  t->name = src->name;
  if (FAILED(hr = fx_make_annotations(e, &src->annotations, &t->annotations))) return hr;
  if (src->pass_count && !(t->passes = fx_new_array(src->pass_count, sizeof *t->passes))) return E_OUTOFMEMORY;
  t->pass_count = src->pass_count;
  for (UINT i = 0; i < src->pass_count; ++i) {
    const Fx4Pass *sp = &src->passes[i];
    FxPass *p = &t->passes[i];
    p->lpVtbl = &fxpass_vtbl;
    p->technique = t;
    p->name = sp->name;
    p->vs = p->gs = p->ps = &g_fx_null_shader;
    if (FAILED(hr = fx_make_annotations(e, &sp->annotations, &p->annotations))) return hr;
    for (UINT k = 0; k < sp->state.count; ++k) {
      const Fx4Assignment *a = &sp->state.items[k];
      if (FAILED(hr = fx_assign(e, FX4_IN_PASS, p, FALSE, a, &p->deps))) return hr;
      if (a->id == 3 || a->id == 4) {
        p->views_set = TRUE;
        if (a->id == 3 && a->index + 1 > p->rtv_count) p->rtv_count = a->index + 1;
      }
    }
  }
  return S_OK;
}

static HRESULT fx_create(const void *data, SIZE_T size, ID3D10Device *device, FxEffect *pool, BOOL is_pool, BOOL ten_one, FxEffect **out) {
  *out = NULL;
  if (!device) return D3DERR_INVALIDCALL;
  if (!data || !size) return E_INVALIDARG;
  FxEffect *e = mem_alloc(sizeof *e);
  if (!e) return E_OUTOFMEMORY;
  e->lpVtbl = is_pool ? &fxpe_vtbl : &fx_vtbl;
  e->pool_vtbl = &fxpool_vtbl;
  e->ref = 1;
  e->is_pool = is_pool;
  e->ten_one = ten_one;
  if (ten_one) {
    ID3D10Device1 *d1 = NULL;
    if (SUCCEEDED(ID3D10Device_QueryInterface(device, &IID_ID3D10Device1, (void **)&d1))) {
      e->level_10_0 = ID3D10Device1_GetFeatureLevel(d1) == D3D10_FEATURE_LEVEL_10_0;
      ID3D10Device1_Release(d1);
    }
  }
  ID3D10Device_AddRef(device);
  e->device = device;
  if (pool) { InterlockedIncrement(&pool->ref); e->pool = pool; }
  HRESULT hr = E_OUTOFMEMORY;
  if (!(e->copy = mem_alloc(size))) goto fail;
  memcpy(e->copy, data, size);
  const Fx4Alloc alloc = {fx_alloc_cb, fx_release_cb, NULL};
  const int status = fx4_parse(e->copy, size, ten_one, &alloc, &e->fx);
  if (status != FX4_OK) { hr = status == FX4_NO_MEMORY ? E_OUTOFMEMORY : E_FAIL; goto fail; }
  hr = E_FAIL;
  if (!pool && (e->fx.shared_buffer_count || e->fx.shared_object_count)) goto fail;      /* (a child effect's binary needs its pool) */
  hr = E_OUTOFMEMORY;
  if (!fx_make_types(e)) goto fail;
  e->shader_count = e->fx.shader_count;
  e->blend_count = e->fx.blend_count;
  e->ds_count = e->fx.ds_count;
  e->rs_count = e->fx.rs_count;
  e->sampler_count = e->fx.sampler_count;
  if ((e->shader_count && !(e->shader_list = fx_new_array(e->shader_count, sizeof(FxVar *)))) ||
      (e->blend_count && !(e->blend_list = fx_new_array(e->blend_count, sizeof(FxVar *)))) ||
      (e->ds_count && !(e->ds_list = fx_new_array(e->ds_count, sizeof(FxVar *)))) ||
      (e->rs_count && !(e->rs_list = fx_new_array(e->rs_count, sizeof(FxVar *)))) ||
      (e->sampler_count && !(e->sampler_list = fx_new_array(e->sampler_count, sizeof(FxVar *)))))
    goto fail;
  if (e->fx.anonymous_count &&
      (!(e->anonymous = fx_new_array(e->fx.anonymous_count, sizeof *e->anonymous)) || !(e->anonymous_types = fx_new_array(e->fx.anonymous_count, sizeof *e->anonymous_types))))
    goto fail;
  if (e->fx.buffer_count &&
      (!(e->buffers = fx_new_array(e->fx.buffer_count, sizeof *e->buffers)) || !(e->buffer_types = fx_new_array(e->fx.buffer_count, sizeof *e->buffer_types))))
    goto fail;
  for (UINT i = 0; i < e->fx.buffer_count; ++i) {
    e->buffer_count = i + 1;      /* (what the release frees) */
    if (FAILED(hr = fx_make_buffer(e, i))) goto fail;
  }
  hr = E_OUTOFMEMORY;
  if (e->fx.object_count && !(e->objects = fx_new_array(e->fx.object_count, sizeof *e->objects))) goto fail;
  for (UINT i = 0; i < e->fx.object_count; ++i) {
    e->object_count = i + 1;
    if (FAILED(hr = fx_make_object(e, i))) goto fail;
  }
  /* (the shared variables a child effect describes: the pool has each, of the same type) */
  hr = E_INVALIDARG;
  for (UINT i = 0; i < e->fx.shared_buffer_count; ++i) {
    const Fx4Buffer *s = &e->fx.shared_buffers[i];
    FxVar *b = s->name ? fx_find_buffer(pool, s->name) : NULL;
    if (!b || b->type->member_count != s->count || (s->tbuffer ? D3D10_SVT_TBUFFER : D3D10_SVT_CBUFFER) != b->type->base) goto fail;
    for (UINT k = 0; k < s->count; ++k)
      if (!fx_name_is(b->members[k].name, s->vars[k].name) || b->members[k].buffer_offset != s->vars[k].offset ||
          !fx_types_match(s->vars[k].type, b->members[k].type, 0))
        goto fail;
  }
  for (UINT i = 0; i < e->fx.shared_object_count; ++i) {
    const Fx4Object *s = &e->fx.shared_objects[i];
    FxVar *v = s->name ? fx_find_var(pool, s->name) : NULL;
    if (!v || !fx_types_match(s->type, v->type, 0)) goto fail;
  }
  hr = E_OUTOFMEMORY;
  if (e->fx.technique_count && !(e->techniques = fx_new_array(e->fx.technique_count, sizeof *e->techniques))) goto fail;
  for (UINT i = 0; i < e->fx.technique_count; ++i) {
    e->technique_count = i + 1;
    if (FAILED(hr = fx_make_technique(e, i))) goto fail;
  }
  *out = e;
  return S_OK;
fail:
  fx_destroy(e);
  return hr;
}

/* D3D10CreateEffectFromMemory: an effect, or a child effect of a pool (D3D10_EFFECT_COMPILE_CHILD_EFFECT with a pool: both or neither). */
static HRESULT fx_create_effect(void *data, SIZE_T size, UINT flags, ID3D10Device *device, ID3D10EffectPool *pool, ID3D10Effect **out, BOOL ten_one) {
  if (!out) return E_INVALIDARG;
  *out = NULL;
  if (!(flags & D3D10_EFFECT_COMPILE_CHILD_EFFECT) != !pool) return E_INVALIDARG;
  if (pool && *(const void *const *)pool != (const void *)&fxpool_vtbl) return E_INVALIDARG;      /* (a pool of this library) */
  FxEffect *e = NULL;
  const HRESULT hr = fx_create(data, size, device, pool ? fx_of_pool(pool) : NULL, FALSE, ten_one, &e);
  if (SUCCEEDED(hr)) *out = (ID3D10Effect *)e;
  return hr;
}
static HRESULT fx_create_pool(void *data, SIZE_T size, UINT flags, ID3D10Device *device, ID3D10EffectPool **out, BOOL ten_one) {
  (void)flags;
  if (!out) return E_INVALIDARG;
  *out = NULL;
  if (!data) return E_INVALIDARG;
  FxEffect *e = NULL;
  const HRESULT hr = fx_create(data, size, device, NULL, TRUE, ten_one, &e);
  if (SUCCEEDED(hr)) *out = (ID3D10EffectPool *)&e->pool_vtbl;
  return hr;
}
EXPORT HRESULT WINAPI D3D10CreateEffectFromMemory(void *data, SIZE_T size, UINT flags, ID3D10Device *device, ID3D10EffectPool *pool, ID3D10Effect **out) {
  return fx_create_effect(data, size, flags, device, pool, out, FALSE);
}
EXPORT HRESULT WINAPI D3D10CreateEffectPoolFromMemory(void *data, SIZE_T size, UINT flags, ID3D10Device *device, ID3D10EffectPool **out) {
  return fx_create_pool(data, size, flags, device, out, FALSE);
}
/* (d3d10_1.dll's, through its .def: the same with Direct3D 10.1's blend of fx_4_1 effects) */
EXPORT HRESULT WINAPI TcosD3D10_1CreateEffectFromMemory(void *data, SIZE_T size, UINT flags, ID3D10Device *device, ID3D10EffectPool *pool, ID3D10Effect **out) {
  return fx_create_effect(data, size, flags, device, pool, out, TRUE);
}
EXPORT HRESULT WINAPI TcosD3D10_1CreateEffectPoolFromMemory(void *data, SIZE_T size, UINT flags, ID3D10Device *device, ID3D10EffectPool **out) {
  return fx_create_pool(data, size, flags, device, out, TRUE);
}

/* D3D10DisassembleEffect: the listing of an effect of this library (or of a pool, through its AsEffect) from its binary - what the effect compiler's listing shows, the
 * shaders as D3D10DisassembleShader lists them. `color_code`: an HTML page with colours. */
EXPORT HRESULT WINAPI D3D10DisassembleEffect(ID3D10Effect *effect, BOOL color_code, ID3D10Blob **out) {
  if (!out) return E_INVALIDARG;
  *out = NULL;
  if (!effect) return E_INVALIDARG;
  const void *vtbl = *(const void *const *)effect;
  if (vtbl != (const void *)&fx_vtbl && vtbl != (const void *)&fxpe_vtbl) return E_INVALIDARG;      /* (an effect of this library) */
  const FxEffect *e = (const FxEffect *)effect;
  DxdText t = {0, 0, 0, 0, color_code != 0};
  const int ok = fx4_disassemble(&t, &e->fx, e->pool != NULL);
  if (!ok) { mem_free(t.data); return E_OUTOFMEMORY; }
  const HRESULT hr = blob_make(t.size + 1, t.data, out);
  mem_free(t.data);
  return hr;
}
