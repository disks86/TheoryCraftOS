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

/* Part of d3d10.c: what Direct3D 10 does with compiled shaders - reflection (D3D10ReflectShader: ID3D10ShaderReflection and its constant buffers, variables and
 * types, over the RDEF, signature and STAT parts), the signature and debug parts as blobs, the disassembler and the HLSL preprocessor. The reading of the containers
 * is dxbc_reflect.h's, the listing dxbc_disasm.h's, the preprocessor hlsl_pp.h's (headers without a library underneath, which tests/DxbcReflectTest.cpp includes).
 *
 * Direct3D 10's reflection is of shader model 4 (4.0, 4.1, and 4.0 of level 9): a container without resource definitions, of shader model 5, or of a compute shader is
 * E_INVALIDARG, as Windows' d3d10.dll answers it; and ID3D10ShaderReflection is all it gives (QueryInterface refuses ID3D10ShaderReflection1 and the later ones, as
 * Windows does). Bad indices and names give the empty constant buffer, variable and type objects, whose GetDesc is E_FAIL. */
#define DXD_REALLOC(p, n) mem_grow((p), (n))
#define DXD_FREE(p) mem_free(p)
#include "dxbc_reflect.h"
#include "dxbc_disasm.h"
#include "hlsl_pp.h"

extern const ID3D10ShaderReflectionVtbl refl_vtbl;
extern const ID3D10ShaderReflectionConstantBufferVtbl reflcb_vtbl;
extern const ID3D10ShaderReflectionVariableVtbl reflvar_vtbl;
extern const ID3D10ShaderReflectionTypeVtbl refltype_vtbl;

typedef struct ReflType {
  const ID3D10ShaderReflectionTypeVtbl *lpVtbl;
  BOOL valid;
  D3D10_SHADER_TYPE_DESC desc;
  struct ReflType **members;
  const char **member_names;
  UINT member_count;
  struct ReflType *next;      /* (every type of a reflection, for its release) */
} ReflType;
typedef struct ReflVar { const ID3D10ShaderReflectionVariableVtbl *lpVtbl; BOOL valid; D3D10_SHADER_VARIABLE_DESC desc; ReflType *type; } ReflVar;
typedef struct ReflCb { const ID3D10ShaderReflectionConstantBufferVtbl *lpVtbl; BOOL valid; D3D10_SHADER_BUFFER_DESC desc; ReflVar *vars; } ReflCb;
typedef struct Refl {
  DxObj o;
  BYTE *blob;                 /* (a copy of the container: the names handed out point into it) */
  SIZE_T size;
  D3D10_SHADER_DESC desc;
  ReflCb *cbs;
  D3D10_SHADER_INPUT_BIND_DESC *binds;
  D3D10_SIGNATURE_PARAMETER_DESC *in, *out;
  ReflType *types;
  UINT type_count;
} Refl;

/* The objects of what there is not (shared by every reflection: they hold nothing). */
static ReflType g_null_type = {&refltype_vtbl, FALSE, {0}, NULL, NULL, 0, NULL};
static ReflVar g_null_var = {&reflvar_vtbl, FALSE, {0}, &g_null_type};
static ReflCb g_null_cb = {&reflcb_vtbl, FALSE, {0}, NULL};

#define REFL_MAX_TYPES 4096u      /* (types of one reflection: a container whose members point at each other is refused, not followed for ever) */
#define REFL_MAX_DEPTH 32

static void refl_destroy(DxObj *o) {
  Refl *r = (Refl *)o;
  for (ReflType *t = r->types; t;) {
    ReflType *next = t->next;
    mem_free(t->members);
    mem_free(t->member_names);
    mem_free(t);
    t = next;
  }
  if (r->cbs) for (UINT i = 0; i < r->desc.ConstantBuffers; ++i) mem_free(r->cbs[i].vars);
  mem_free(r->cbs);
  mem_free(r->binds);
  mem_free(r->in);
  mem_free(r->out);
  mem_free(r->blob);
}
/* The type at `at` of the resource definitions, as a member at `offset` of its parent (0 for a variable's). NULL when it cannot be read. */
static ReflType *refl_type(Refl *r, const DxrRdef *d, uint32_t at, uint32_t offset, int depth) {
  DxrType t;
  if (depth > REFL_MAX_DEPTH || r->type_count >= REFL_MAX_TYPES || !dxr_rdef_type(d, at, &t)) return NULL;
  ReflType *x = mem_alloc(sizeof *x);
  if (!x) return NULL;
  ++r->type_count;
  x->lpVtbl = &refltype_vtbl;
  x->valid = TRUE;
  x->next = r->types;
  r->types = x;
  x->desc.Class = (D3D10_SHADER_VARIABLE_CLASS)t.cls;
  x->desc.Type = (D3D10_SHADER_VARIABLE_TYPE)t.type;
  x->desc.Rows = t.rows;
  x->desc.Columns = t.columns;
  x->desc.Elements = t.elements;
  x->desc.Members = t.members;
  x->desc.Offset = offset;
  if (t.members) {
    x->members = mem_alloc(sizeof *x->members * t.members);
    x->member_names = mem_alloc(sizeof *x->member_names * t.members);
    if (!x->members || !x->member_names) return NULL;
    for (uint32_t k = 0; k < t.members; ++k) {
      DxrMember m;
      if (!dxr_rdef_member(d, &t, k, &m)) return NULL;
      x->member_names[k] = m.name;
      if (!(x->members[k] = refl_type(r, d, m.type_offset, m.offset, depth + 1))) return NULL;
    }
    x->member_count = t.members;
  }
  return x;
}
/* Reads the parts of the copy into the reflection: FALSE for a container Direct3D 10 does not reflect. */
static BOOL refl_read(Refl *r) {
  const uint8_t *data = r->blob, *chunk;
  const size_t size = r->size;
  size_t bytes;
  DxrRdef d;
  if (!dxr_find(data, size, DXR_RDEF, &chunk, &bytes) || !dxr_rdef(chunk, bytes, &d)) return FALSE;
  const uint32_t kind = d.target >> 16, major = (d.target >> 8) & 0xFF, minor = d.target & 0xFF;
  if (major != 4 || minor > 1 || (kind != 0xFFFF && kind != 0xFFFE && kind != 0x4753)) return FALSE;      /* (pixel, vertex, geometry shaders of 4.0 / 4.1) */
  const uint8_t *code;
  size_t words;
  int shex;
  const int has_code = dxr_code(data, size, &code, &words, &shex);
  if (has_code && shex) return FALSE;
  D3D10_SHADER_DESC *s = &r->desc;
  s->Version = has_code ? dxr_u32(code) : ((kind == 0xFFFF ? 0u : kind == 0xFFFE ? 1u : 2u) << 16) | (major << 4) | minor;
  s->Creator = d.creator;
  s->Flags = d.flags;
  /* constant buffers, their variables and types */
  if (d.cb_count) {
    if (!(r->cbs = mem_alloc(sizeof *r->cbs * d.cb_count))) return FALSE;
    for (uint32_t i = 0; i < d.cb_count; ++i) {
      DxrBuffer b;
      ReflCb *cb = &r->cbs[i];
      cb->lpVtbl = &reflcb_vtbl;
      s->ConstantBuffers = i + 1;      /* (what the release frees) */
      if (!dxr_rdef_buffer(&d, i, &b)) return FALSE;
      cb->valid = TRUE;
      cb->desc.Name = b.name;
      cb->desc.Type = (D3D10_CBUFFER_TYPE)b.type;
      cb->desc.Variables = b.var_count;
      cb->desc.Size = b.size;
      cb->desc.uFlags = b.flags;
      if (!b.var_count) continue;
      if (!(cb->vars = mem_alloc(sizeof *cb->vars * b.var_count))) return FALSE;
      for (uint32_t k = 0; k < b.var_count; ++k) {
        DxrVariable v;
        ReflVar *x = &cb->vars[k];
        x->lpVtbl = &reflvar_vtbl;
        x->type = &g_null_type;
        if (!dxr_rdef_variable(&d, &b, k, &v)) return FALSE;
        x->valid = TRUE;
        x->desc.Name = v.name;
        x->desc.StartOffset = v.offset;
        x->desc.Size = v.size;
        x->desc.uFlags = v.flags;
        x->desc.DefaultValue = v.default_offset ? (void *)(chunk + v.default_offset) : NULL;
        if (!(x->type = refl_type(r, &d, v.type_offset, 0, 0))) return FALSE;
      }
    }
  }
  /* the bindings (Direct3D 10 gives no texture component flags) */
  if (d.bind_count) {
    if (!(r->binds = mem_alloc(sizeof *r->binds * d.bind_count))) return FALSE;
    for (uint32_t i = 0; i < d.bind_count; ++i) {
      DxrBinding b;
      if (!dxr_rdef_binding(&d, i, &b)) return FALSE;
      D3D10_SHADER_INPUT_BIND_DESC *x = &r->binds[i];
      x->Name = b.name;
      x->Type = (D3D10_SHADER_INPUT_TYPE)b.type;
      x->BindPoint = b.bind_point;
      x->BindCount = b.bind_count;
      x->uFlags = b.flags & ~(UINT)D3D10_SIF_TEXTURE_COMPONENTS;
      x->ReturnType = (D3D10_RESOURCE_RETURN_TYPE)b.return_type;
      x->Dimension = (D3D10_SRV_DIMENSION)b.dimension;
      x->NumSamples = b.samples;
    }
    s->BoundResources = d.bind_count;
  }
  /* the signatures */
  for (int side = 0; side < 2; ++side) {
    uint32_t tag;
    if (!(side ? dxr_output_signature(data, size, &chunk, &bytes, &tag) : dxr_input_signature(data, size, &chunk, &bytes, &tag))) continue;
    const int64_t n = dxr_signature_count(chunk, bytes, tag);
    if (n < 0) return FALSE;
    if (!n) continue;
    D3D10_SIGNATURE_PARAMETER_DESC *p = mem_alloc(sizeof *p * (SIZE_T)n);
    if (!p) return FALSE;
    if (side) r->out = p; else r->in = p;
    for (uint32_t i = 0; i < (uint32_t)n; ++i) {
      DxrSignature e;
      if (!dxr_signature_element(chunk, bytes, tag, i, &e)) return FALSE;
      p[i].SemanticName = e.name;
      p[i].SemanticIndex = e.semantic_index;
      p[i].Register = e.reg;
      p[i].SystemValueType = (D3D10_NAME)e.system_value;
      p[i].ComponentType = (D3D10_REGISTER_COMPONENT_TYPE)e.component_type;
      p[i].Mask = e.mask;
      p[i].ReadWriteMask = e.rw_mask;
    }
    if (side) s->OutputParameters = (UINT)n; else s->InputParameters = (UINT)n;
  }
  /* the statistics */
  uint32_t st[DXR_STAT_WORDS];
  dxr_stat(data, size, st, DXR_STAT_WORDS);
  s->InstructionCount = st[DXR_STAT_INSTRUCTIONS];
  s->TempRegisterCount = st[DXR_STAT_TEMPS];
  s->TempArrayCount = st[DXR_STAT_TEMP_ARRAYS];
  s->DefCount = st[DXR_STAT_DEFS];
  s->DclCount = st[DXR_STAT_DCLS];
  s->TextureNormalInstructions = st[DXR_STAT_TEX_NORMAL];
  s->TextureLoadInstructions = st[DXR_STAT_TEX_LOAD];
  s->TextureCompInstructions = st[DXR_STAT_TEX_COMP];
  s->TextureBiasInstructions = st[DXR_STAT_TEX_BIAS];
  s->TextureGradientInstructions = st[DXR_STAT_TEX_GRAD];
  s->FloatInstructionCount = st[DXR_STAT_FLOAT];
  s->IntInstructionCount = st[DXR_STAT_INT];
  s->UintInstructionCount = st[DXR_STAT_UINT];
  s->StaticFlowControlCount = st[DXR_STAT_STATIC_FLOW];
  s->DynamicFlowControlCount = st[DXR_STAT_DYNAMIC_FLOW];
  s->MacroInstructionCount = st[DXR_STAT_MACRO];
  s->ArrayInstructionCount = st[DXR_STAT_ARRAY];
  s->CutInstructionCount = st[DXR_STAT_CUT];
  s->EmitInstructionCount = st[DXR_STAT_EMIT];
  s->GSOutputTopology = (D3D10_PRIMITIVE_TOPOLOGY)st[DXR_STAT_GS_TOPOLOGY];
  s->GSMaxOutputVertexCount = st[DXR_STAT_GS_MAX_VERTICES];
  return TRUE;
}

EXPORT HRESULT WINAPI D3D10ReflectShader(const void *data, SIZE_T size, ID3D10ShaderReflection **out) {
  static const GUID *const iids[] = {&IID_ID3D10ShaderReflection, NULL};
  size_t total;
  uint32_t chunks;
  if (!out) return E_INVALIDARG;
  *out = NULL;
  if (!data || !dxr_container(data, size, &total, &chunks)) return E_INVALIDARG;
  Refl *r = mem_alloc(sizeof *r);
  if (!r) return E_OUTOFMEMORY;
  dx_init(&r->o, &refl_vtbl, iids, refl_destroy);
  r->size = total;
  r->blob = mem_alloc(total);
  if (!r->blob) { dx_Release(OBJ(r)); return E_OUTOFMEMORY; }
  memcpy(r->blob, data, total);
  if (!refl_read(r)) { dx_Release(OBJ(r)); return E_INVALIDARG; }
  *out = (ID3D10ShaderReflection *)r;
  return S_OK;
}

/* ---- ID3D10ShaderReflection ---- */
static HRESULT STDMETHODCALLTYPE refl_QueryInterface(Refl *r, REFIID riid, void **out) {
  if (!out) return E_POINTER;
  *out = NULL;
  if (!guid_eq(riid, &IID_IUnknown) && !guid_eq(riid, &IID_ID3D10ShaderReflection)) return E_NOINTERFACE;
  dx_AddRef(OBJ(r));
  *out = r;
  return S_OK;
}
static HRESULT STDMETHODCALLTYPE refl_GetDesc(Refl *r, D3D10_SHADER_DESC *desc) {
  if (!desc) return E_FAIL;
  *desc = r->desc;
  return S_OK;
}
static ID3D10ShaderReflectionConstantBuffer *STDMETHODCALLTYPE refl_GetConstantBufferByIndex(Refl *r, UINT index) {
  return (ID3D10ShaderReflectionConstantBuffer *)(index < r->desc.ConstantBuffers ? &r->cbs[index] : &g_null_cb);
}
static ID3D10ShaderReflectionConstantBuffer *STDMETHODCALLTYPE refl_GetConstantBufferByName(Refl *r, const char *name) {
  for (UINT i = 0; name && i < r->desc.ConstantBuffers; ++i)
    if (dxr_streq(r->cbs[i].desc.Name, name)) return (ID3D10ShaderReflectionConstantBuffer *)&r->cbs[i];
  return (ID3D10ShaderReflectionConstantBuffer *)&g_null_cb;
}
static HRESULT STDMETHODCALLTYPE refl_GetResourceBindingDesc(Refl *r, UINT index, D3D10_SHADER_INPUT_BIND_DESC *desc) {
  if (!desc || index >= r->desc.BoundResources) return E_INVALIDARG;
  *desc = r->binds[index];
  return S_OK;
}
static HRESULT STDMETHODCALLTYPE refl_GetInputParameterDesc(Refl *r, UINT index, D3D10_SIGNATURE_PARAMETER_DESC *desc) {
  if (!desc || index >= r->desc.InputParameters) return E_INVALIDARG;
  *desc = r->in[index];
  return S_OK;
}
static HRESULT STDMETHODCALLTYPE refl_GetOutputParameterDesc(Refl *r, UINT index, D3D10_SIGNATURE_PARAMETER_DESC *desc) {
  if (!desc || index >= r->desc.OutputParameters) return E_INVALIDARG;
  *desc = r->out[index];
  return S_OK;
}

/* ---- ID3D10ShaderReflectionConstantBuffer ---- */
static HRESULT STDMETHODCALLTYPE reflcb_GetDesc(ReflCb *c, D3D10_SHADER_BUFFER_DESC *desc) {
  if (!desc || !c->valid) return E_FAIL;
  *desc = c->desc;
  return S_OK;
}
static ID3D10ShaderReflectionVariable *STDMETHODCALLTYPE reflcb_GetVariableByIndex(ReflCb *c, UINT index) {
  return (ID3D10ShaderReflectionVariable *)(c->valid && index < c->desc.Variables ? &c->vars[index] : &g_null_var);
}
static ID3D10ShaderReflectionVariable *STDMETHODCALLTYPE reflcb_GetVariableByName(ReflCb *c, const char *name) {
  for (UINT i = 0; c->valid && name && i < c->desc.Variables; ++i)
    if (dxr_streq(c->vars[i].desc.Name, name)) return (ID3D10ShaderReflectionVariable *)&c->vars[i];
  return (ID3D10ShaderReflectionVariable *)&g_null_var;
}

/* ---- ID3D10ShaderReflectionVariable ---- */
static HRESULT STDMETHODCALLTYPE reflvar_GetDesc(ReflVar *v, D3D10_SHADER_VARIABLE_DESC *desc) {
  if (!desc || !v->valid) return E_FAIL;
  *desc = v->desc;
  return S_OK;
}
static ID3D10ShaderReflectionType *STDMETHODCALLTYPE reflvar_GetType(ReflVar *v) { return (ID3D10ShaderReflectionType *)v->type; }

/* ---- ID3D10ShaderReflectionType ---- */
static HRESULT STDMETHODCALLTYPE refltype_GetDesc(ReflType *t, D3D10_SHADER_TYPE_DESC *desc) {
  if (!desc || !t->valid) return E_FAIL;
  *desc = t->desc;
  return S_OK;
}
static ID3D10ShaderReflectionType *STDMETHODCALLTYPE refltype_GetMemberTypeByIndex(ReflType *t, UINT index) {
  return (ID3D10ShaderReflectionType *)(t->valid && index < t->member_count ? t->members[index] : &g_null_type);
}
static ID3D10ShaderReflectionType *STDMETHODCALLTYPE refltype_GetMemberTypeByName(ReflType *t, const char *name) {
  for (UINT i = 0; t->valid && name && i < t->member_count; ++i)
    if (dxr_streq(t->member_names[i], name)) return (ID3D10ShaderReflectionType *)t->members[i];
  return (ID3D10ShaderReflectionType *)&g_null_type;
}
static const char *STDMETHODCALLTYPE refltype_GetMemberTypeName(ReflType *t, UINT index) {
  if (!t->valid) return "$Invalid";
  return index < t->member_count ? t->member_names[index] : NULL;
}

/* ---- signatures and debug information as blobs ------------------------------------------------------------------------------------------------------------------- */

/* A container of the signature parts asked for (1 input, 2 output, 3 both), with its checksum, as Windows makes it: E_FAIL when the shader has none of them. */
static HRESULT signature_blob(const void *data, SIZE_T size, int which, ID3D10Blob **out) {
  size_t total;
  uint32_t chunks;
  if (!out) return E_INVALIDARG;
  *out = NULL;
  if (!data || !dxr_container(data, size, &total, &chunks)) return E_FAIL;
  uint32_t tags[2];
  const uint8_t *parts[2];
  size_t sizes[2];
  uint32_t count = 0;
  if ((which & 1) && dxr_input_signature(data, total, &parts[count], &sizes[count], &tags[count])) ++count;
  if ((which & 2) && dxr_output_signature(data, total, &parts[count], &sizes[count], &tags[count])) ++count;
  if (!count || (which == 3 && count != 2)) return E_FAIL;
  const size_t bytes = dxr_container_bytes(sizes, count);
  if (!bytes) return E_FAIL;
  ID3D10Blob *blob;
  HRESULT hr = blob_make(bytes, NULL, &blob);
  if (FAILED(hr)) return hr;
  dxr_container_write(ID3D10Blob_GetBufferPointer(blob), bytes, tags, parts, sizes, count);
  *out = blob;
  return S_OK;
}
EXPORT HRESULT WINAPI D3D10GetInputSignatureBlob(const void *data, SIZE_T size, ID3D10Blob **out) { return signature_blob(data, size, 1, out); }
EXPORT HRESULT WINAPI D3D10GetOutputSignatureBlob(const void *data, SIZE_T size, ID3D10Blob **out) { return signature_blob(data, size, 2, out); }
EXPORT HRESULT WINAPI D3D10GetInputAndOutputSignatureBlob(const void *data, SIZE_T size, ID3D10Blob **out) { return signature_blob(data, size, 3, out); }
/* The SDBG part (the compiler's debug information, with /Zi): E_FAIL without one. */
EXPORT HRESULT WINAPI D3D10GetShaderDebugInfo(const void *data, SIZE_T size, ID3D10Blob **out) {
  const uint8_t *chunk;
  size_t bytes;
  if (!out) return E_INVALIDARG;
  *out = NULL;
  if (!data || !dxr_find(data, size, DXR_SDBG, &chunk, &bytes)) return E_FAIL;
  return blob_make(bytes, chunk, out);
}

/* ---- the disassembler -------------------------------------------------------------------------------------------------------------------------------------------- */

/* The listing of a shader as a text blob (with its NUL). `flags`: D3D_DISASM_ENABLE_COLOR_CODE for an HTML page with colours. (effect.inc lists its shaders with this) */
static HRESULT shader_disassemble(const void *code, SIZE_T size, UINT flags, const char *comments, ID3D10Blob **out) {
  if (!out) return E_INVALIDARG;
  *out = NULL;
  if (!code) return E_INVALIDARG;
  DxdText t = {0, 0, 0, 0, (flags & D3D_DISASM_ENABLE_COLOR_CODE) != 0};
  const int ok = dxd_disassemble(&t, code, size, comments);
  if (t.failed) { mem_free(t.data); return E_OUTOFMEMORY; }
  if (!ok) { mem_free(t.data); return E_FAIL; }
  const HRESULT hr = blob_make(t.size + 1, t.data, out);
  mem_free(t.data);
  return hr;
}
EXPORT HRESULT WINAPI D3D10DisassembleShader(const void *code, SIZE_T size, BOOL color_code, const char *comments, ID3D10Blob **out) {
  return shader_disassemble(code, size, color_code ? D3D_DISASM_ENABLE_COLOR_CODE : 0, comments, out);
}

/* ---- the preprocessor --------------------------------------------------------------------------------------------------------------------------------------------- */

/* The opener of the includes: the program's ID3D10Include, or - D3D_COMPILE_STANDARD_FILE_INCLUDE - the files, relative to the folder of the file that includes them. */
typedef struct PpIo { ID3D10Include *include; const char *file; } PpIo;
static int pp_open(void *user, int system, const char *name, const char *parent, const char **data, size_t *bytes) {
  PpIo *io = user;
  if (!io->include) return 0;
  if (io->include != (ID3D10Include *)(UINT_PTR)1) {
    LPCVOID got = NULL;
    UINT n = 0;
    if (FAILED(ID3DInclude_Open((ID3DInclude *)io->include, system ? D3D10_INCLUDE_SYSTEM : D3D10_INCLUDE_LOCAL, name, parent, &got, &n)) || (!got && n)) return 0;
    *data = got ? got : "";
    *bytes = n;
    return 1;
  }
  /* (the standard opener: the folder of the top file, or the current one) */
  char path[MAX_PATH];
  SIZE_T at = 0;
  const SIZE_T name_length = alen(name);
  int absolute = name[0] == '\\' || name[0] == '/' || (name[0] && name[1] == ':');
  if (!absolute && io->file) {
    SIZE_T cut = 0;
    for (SIZE_T i = 0; io->file[i]; ++i) if (io->file[i] == '\\' || io->file[i] == '/') cut = i + 1;
    if (cut >= sizeof path) return 0;
    memcpy(path, io->file, cut);
    at = cut;
  }
  if (name_length >= sizeof path - at) return 0;
  memcpy(path + at, name, name_length + 1);
  HANDLE f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
  if (f == INVALID_HANDLE_VALUE) return 0;
  LARGE_INTEGER length;
  if (!GetFileSizeEx(f, &length) || length.QuadPart > 0x7FFFFFFF) { CloseHandle(f); return 0; }
  char *text = mem_alloc((SIZE_T)length.QuadPart + 1);
  DWORD read = 0;
  if (!text || !ReadFile(f, text, (DWORD)length.QuadPart, &read, NULL) || read != (DWORD)length.QuadPart) { mem_free(text); CloseHandle(f); return 0; }
  CloseHandle(f);
  *data = text;
  *bytes = read;
  return 1;
}
static void pp_close(void *user, const char *data) {
  PpIo *io = user;
  if (io->include == (ID3D10Include *)(UINT_PTR)1) mem_free((void *)data);
  else if (io->include) ID3DInclude_Close((ID3DInclude *)io->include, data);
}
EXPORT HRESULT WINAPI D3D10PreprocessShader(const char *data, SIZE_T size, const char *file, const D3D10_SHADER_MACRO *defines, ID3D10Include *include, ID3D10Blob **text,
                                            ID3D10Blob **errors) {
  if (text) *text = NULL;
  if (errors) *errors = NULL;
  if (!data || !text) return E_INVALIDARG;
  /* the defines: names and values up to the {NULL, NULL} that ends them */
  SIZE_T count = 0;
  while (defines && defines[count].Name) ++count;
  const char **names = count ? mem_alloc(sizeof *names * count) : NULL, **values = count ? mem_alloc(sizeof *values * count) : NULL;
  if (count && (!names || !values)) { mem_free(names); mem_free(values); return E_OUTOFMEMORY; }
  for (SIZE_T i = 0; i < count; ++i) { names[i] = defines[i].Name; values[i] = defines[i].Definition; }
  PpIo io = {include, file};
  const HppIo hio = {&io, pp_open, pp_close};
  DxdText out = {0, 0, 0, 0, 0}, err = {0, 0, 0, 0, 0};
  const int ok = hpp_run(data, size, file, names, values, count, &hio, &out, &err);
  mem_free(names);
  mem_free(values);
  HRESULT hr = S_OK;
  if (out.failed || err.failed) hr = E_OUTOFMEMORY;
  else if (!ok) hr = E_FAIL;
  if (SUCCEEDED(hr)) hr = blob_make(out.size + 1, out.data ? out.data : "", text);
  if (err.size && errors) blob_make(err.size + 1, err.data, errors);
  mem_free(out.data);
  mem_free(err.data);
  return hr;
}
