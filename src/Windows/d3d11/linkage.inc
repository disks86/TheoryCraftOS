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

/* Part of d3d11.c: dynamic shader linkage (ID3D11ClassLinkage, ID3D11ClassInstance, the class instances bound with *SetShader).
 *
 * A shader made with a linkage gives the linkage the class instances it declares (its IFCE part, dxbc_link.h), which GetClassInstance then finds by name
 * and index (an element n of an instance array has its data n times the type's size after the first). CreateClassInstance makes an instance of a type
 * by name whose data is where the program says. A shader with interface slots is specialised for the instances bound with it when a draw or dispatch
 * needs it (dl_specialise: each call inlined, `this` the instance's ConstantBuffer, BaseConstantBufferOffset, BaseTexture and BaseSampler); the
 * specialised shaders are kept with the shader (SHADER_VARIANTS of them, the oldest made again when it is needed) and go into the pipeline as its code.
 * A binding the shader cannot take - an instance of a class the slot has no table for - leaves the draw out, as a driver cannot run it either. */

#define SHADER_VARIANTS 8u
#define LINK_NAME 64u

typedef struct Variant {
  UINT key_count;
  DlBinding key[DL_MAX_SLOTS];     /* the bindings it is for (key_count of them) */
  BYTE *code;
  SIZE_T bytes;
  UINT id;                          /* the number the pipeline cache knows it by */
  ID3D12PipelineState *compute;     /* a compute shader's pipeline, made at its first dispatch */
  UINT used;
  UINT last_used;
} Variant;

typedef struct ShaderLink {
  DlInfo info;
  Variant *variants[SHADER_VARIANTS];
  UINT clock;
} ShaderLink;

/* What GetClassInstance can find: the instances of the shaders made with the linkage. */
typedef struct LinkInstance {
  char name[LINK_NAME], type_name[LINK_NAME];
  UINT instance_id, type_id, count;
  UINT cb, cb_offset, texture, sampler;
  UINT cb_stride, textures, samplers;
} LinkInstance;
typedef struct Linkage {
  Child c;
  CRITICAL_SECTION lock;
  LinkInstance *instances;
  UINT count, cap;
} Linkage;
typedef struct ClassInst {
  Child c;
  Linkage *linkage;            /* a reference */
  D3D11_CLASS_INSTANCE_DESC desc;
  char type_name[256];
  char instance_name[256];
} ClassInst;

static void variant_free(Dev *d, Variant *v) {
  if (!v) return;
  if (v->compute) { settle(d, v->used); ID3D12PipelineState_Release(v->compute); }
  mem_free(v->code);
  mem_free(v);
}
static void shader_link_free(Dev *d, ShaderLink *link) {
  if (!link) return;
  for (UINT i = 0; i < SHADER_VARIANTS; ++i) variant_free(d, link->variants[i]);
  mem_free(link);
}

/* The class instances a shader declares into the linkage it is made with. */
static void linkage_add(Linkage *l, const DlInfo *info) {
  EnterCriticalSection(&l->lock);
  for (UINT i = 0; i < info->instance_count; ++i) {
    const DlInstance *in = &info->instances[i];
    const DlType *type = &info->types[in->type];
    BOOL known = FALSE;
    for (UINT k = 0; k < l->count && !known; ++k) known = strcmp(l->instances[k].name, in->name) == 0;
    if (known) continue;      /* (another shader of the linkage declared it first) */
    if (l->count == l->cap) {
      const UINT cap = l->cap ? l->cap * 2 : 16;
      LinkInstance *n = mem_grow(l->instances, (SIZE_T)cap * sizeof *n);
      if (!n) break;
      l->instances = n;
      l->cap = cap;
    }
    LinkInstance *e = &l->instances[l->count++];
    memcpy(e->name, in->name, sizeof e->name);
    memcpy(e->type_name, type->name, sizeof e->type_name);
    e->instance_id = i;
    e->type_id = in->type;
    e->count = in->count;
    e->cb = in->cb;
    e->cb_offset = in->cb_offset;
    e->texture = in->texture;
    e->sampler = in->sampler;
    e->cb_stride = type->cb_stride;
    e->textures = type->textures;
    e->samplers = type->samplers;
  }
  LeaveCriticalSection(&l->lock);
}

/* A shader's interfaces (none: no link), and its instances into the linkage it is made with. E_INVALIDARG for an IFCE part that is not right. */
static HRESULT shader_link(Shader *s, ID3D11ClassLinkage *linkage) {
  extern const ID3D11ClassLinkageVtbl link_vtbl;
  ShaderLink *link = mem_alloc(sizeof *link);
  if (!link) return E_OUTOFMEMORY;
  const int r = dl_read_interfaces(s->code, s->bytes, &link->info);
  if (r != DL_OK) { mem_free(link); return E_INVALIDARG; }
  Linkage *own_linkage = (Linkage *)tcos_own(linkage, &link_vtbl);      /* (tcos_object.h) */
  if (own_linkage && link->info.instance_count) linkage_add(own_linkage, &link->info);
  if (!link->info.slot_count) { mem_free(link); return S_OK; }
  s->link = link;
  return S_OK;
}

/* The code of a stage's shader for the instances bound with it (the shader's own when it has no interfaces): FALSE when it cannot be made. `*compute`:
 * where the variant keeps its compute pipeline (NULL for the shader's own). */
static BOOL shader_code(Dev *d, Ctx *c, UINT stage, Shader *s, const BYTE **code, SIZE_T *bytes, UINT *id, Variant **variant) {
  *variant = NULL;
  if (!s->link) { *code = s->code; *bytes = s->bytes; *id = s->c.id; return TRUE; }
  ShaderLink *link = s->link;
  DlBinding key[DL_MAX_SLOTS];
  const UINT slots = link->info.slot_count;
  memset(key, 0, sizeof key[0] * slots);
  for (UINT i = 0; i < slots; ++i) {
    const ClassInst *inst = i < c->instance_count[stage] ? c->instances[stage][i] : NULL;
    if (!inst) continue;
    const UINT type = dl_type_named(&link->info, inst->type_name);
    if (type == DL_NONE) {
      static volatile LONG said;
      if (!InterlockedExchange(&said, 1)) dx_say("dx: refused: a class instance of a type the shader does not have (see docs/Direct3D11.md)");
      return FALSE;
    }
    key[i].bound = 1;
    key[i].type = type;
    key[i].this_values[0] = inst->desc.ConstantBuffer;
    key[i].this_values[1] = inst->desc.BaseConstantBufferOffset;
    key[i].this_values[2] = inst->desc.BaseTexture;
    key[i].this_values[3] = inst->desc.BaseSampler;
  }
  UINT oldest = 0;
  for (UINT i = 0; i < SHADER_VARIANTS; ++i) {
    Variant *v = link->variants[i];
    if (!v) { oldest = i; continue; }
    if (v->key_count == slots && memcmp(v->key, key, sizeof key[0] * slots) == 0) {
      v->last_used = ++link->clock;
      *code = v->code; *bytes = v->bytes; *id = v->id; *variant = v;
      return TRUE;
    }
    if (link->variants[oldest] && v->last_used < link->variants[oldest]->last_used) oldest = i;
  }
  DlWork *work = mem_alloc(sizeof *work);
  Variant *v = mem_alloc(sizeof *v);
  if (!work || !v) { mem_free(work); mem_free(v); return FALSE; }
  const int r = dl_specialise(s->code, s->bytes, &link->info, key, slots, work);
  if (r != DL_OK) {
    mem_free(work); mem_free(v);
    static volatile LONG said;
    if (!InterlockedExchange(&said, 1)) dx_say("dx: refused: class instances the shader's interface slots do not take (see docs/Direct3D11.md)");
    return FALSE;
  }
  v->code = mem_alloc(work->dxbc_size);
  if (!v->code) { mem_free(work); mem_free(v); return FALSE; }
  memcpy(v->code, work->dxbc, work->dxbc_size);
  v->bytes = work->dxbc_size;
  mem_free(work);
  v->key_count = slots;
  memcpy(v->key, key, sizeof key[0] * slots);
  v->id = (UINT)InterlockedIncrement(&g_next_id);
  v->last_used = ++link->clock;
  if (link->variants[oldest]) {
    flush(d);      /* (a recorded dispatch may use the pipeline that goes) */
    variant_free(d, link->variants[oldest]);
  }
  link->variants[oldest] = v;
  *code = v->code; *bytes = v->bytes; *id = v->id; *variant = v;
  return TRUE;
}

/* ---- the objects --------------------------------------------------------------------------------------------------------------------------------- */

static void linkage_destroy(DxObj *o) {
  Linkage *l = (Linkage *)o;
  DeleteCriticalSection(&l->lock);
  mem_free(l->instances);
  child_destroy(o);
}
static HRESULT STDMETHODCALLTYPE dev_CreateClassLinkage(Dev *d, ID3D11ClassLinkage **out) {
  extern const ID3D11ClassLinkageVtbl link_vtbl;
  static const GUID *const iids[] = {&IID_ID3D11DeviceChild, &IID_ID3D11ClassLinkage, NULL};
  if (!out) return E_INVALIDARG;
  *out = NULL;
  Linkage *l = child_new(sizeof *l, d, &link_vtbl, iids, linkage_destroy);
  if (!l) return E_OUTOFMEMORY;
  InitializeCriticalSection(&l->lock);
  *out = (ID3D11ClassLinkage *)l;
  return S_OK;
}
static void inst_destroy(DxObj *o) {
  ClassInst *i = (ClassInst *)o;
  unhold(i->linkage);
  child_destroy(o);
}
/* A string into a buffer of `size` (`*length` in: its room, out: the length with the NUL; no buffer: only the length). */
static void name_out(const char *name, LPSTR buffer, SIZE_T *length) {
  if (!length) return;
  SIZE_T n = 0;
  while (name[n]) ++n;
  if (buffer && *length) {
    const SIZE_T copied = n + 1 <= *length ? n : *length - 1;
    memcpy(buffer, name, copied);
    buffer[copied] = 0;
  }
  *length = n + 1;
}
static void STDMETHODCALLTYPE inst_GetClassLinkage(ClassInst *i, ID3D11ClassLinkage **out) { if (!out) return; child_ref(i->linkage); *out = (ID3D11ClassLinkage *)i->linkage; }
static void STDMETHODCALLTYPE inst_GetDesc(ClassInst *i, D3D11_CLASS_INSTANCE_DESC *desc) { if (desc) *desc = i->desc; }
static void STDMETHODCALLTYPE inst_GetInstanceName(ClassInst *i, LPSTR name, SIZE_T *length) { name_out(i->instance_name, name, length); }
static void STDMETHODCALLTYPE inst_GetTypeName(ClassInst *i, LPSTR name, SIZE_T *length) { name_out(i->type_name, name, length); }

static ClassInst *inst_new(Linkage *l) {
  extern const ID3D11ClassInstanceVtbl inst_vtbl;
  static const GUID *const iids[] = {&IID_ID3D11DeviceChild, &IID_ID3D11ClassInstance, NULL};
  ClassInst *i = child_new(sizeof *i, l->c.dev, &inst_vtbl, iids, inst_destroy);
  if (!i) return NULL;
  hold(l);
  i->linkage = l;
  return i;
}
/* An instance that is not a shader's variable: the type, and where its data is (the constant buffer, the offset in it in vectors, the first texture and sampler).
 * Its instance name is empty, and it counts as made (Created); its TypeId is the type's in the first shader of the linkage that has the type. */
static HRESULT STDMETHODCALLTYPE link_CreateClassInstance(Linkage *l, LPCSTR type_name, UINT cb, UINT cb_offset, UINT texture, UINT sampler, ID3D11ClassInstance **out) {
  if (out) *out = NULL;
  if (!type_name || !out) return E_INVALIDARG;
  SIZE_T n = 0;
  while (type_name[n] && n < 256) ++n;
  if (n == 0 || n >= sizeof ((ClassInst *)0)->type_name) return E_INVALIDARG;
  ClassInst *i = inst_new(l);
  if (!i) return E_OUTOFMEMORY;
  memcpy(i->type_name, type_name, n + 1);
  i->instance_name[0] = 0;
  EnterCriticalSection(&l->lock);
  for (UINT k = 0; k < l->count; ++k)
    if (strcmp(l->instances[k].type_name, type_name) == 0) { i->desc.TypeId = l->instances[k].type_id; break; }
  LeaveCriticalSection(&l->lock);
  i->desc.InstanceId = 0;
  i->desc.InstanceIndex = 0;
  i->desc.ConstantBuffer = cb;
  i->desc.BaseConstantBufferOffset = cb_offset;
  i->desc.BaseTexture = texture;
  i->desc.BaseSampler = sampler;
  i->desc.Created = TRUE;
  *out = (ID3D11ClassInstance *)i;
  return S_OK;
}
/* A class instance a shader made with the linkage declares, by its name (element `index` of an array of them). E_INVALIDARG for a name no such shader has, or
 * an index past the array's end. */
static HRESULT STDMETHODCALLTYPE link_GetClassInstance(Linkage *l, LPCSTR name, UINT index, ID3D11ClassInstance **out) {
  if (out) *out = NULL;
  if (!name || !out) return E_INVALIDARG;
  LinkInstance found;
  BOOL have = FALSE;
  EnterCriticalSection(&l->lock);
  for (UINT k = 0; k < l->count && !have; ++k)
    if (strcmp(l->instances[k].name, name) == 0) { found = l->instances[k]; have = TRUE; }
  LeaveCriticalSection(&l->lock);
  if (!have || index >= found.count) return E_INVALIDARG;
  ClassInst *i = inst_new(l);
  if (!i) return E_OUTOFMEMORY;
  memcpy(i->type_name, found.type_name, sizeof found.type_name);
  memcpy(i->instance_name, found.name, sizeof found.name);
  i->desc.InstanceId = found.instance_id;
  i->desc.InstanceIndex = index;
  i->desc.TypeId = found.type_id;
  /* (what the instance has none of stays at the shader's 0xFFFF; the data of element n is n types' worth after the first) */
  i->desc.ConstantBuffer = found.cb;
  i->desc.BaseConstantBufferOffset = found.cb_offset == 0xFFFF ? found.cb_offset : found.cb_offset + index * found.cb_stride;
  i->desc.BaseTexture = found.texture == 0xFFFF ? found.texture : found.texture + index * found.textures;
  i->desc.BaseSampler = found.sampler == 0xFFFF ? found.sampler : found.sampler + index * found.samplers;
  i->desc.Created = FALSE;
  *out = (ID3D11ClassInstance *)i;
  return S_OK;
}
