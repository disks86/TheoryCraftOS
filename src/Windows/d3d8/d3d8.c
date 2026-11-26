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

/* d3d8.dll for TheoryCraftOS: Direct3D 8 as a layer on the COM interfaces of this system's Direct3D 9 (d3d9.dll; docs/Direct3D8.md).
 *
 * Every Direct3D 8 object is a wrapper of the Direct3D 9 object that does its work (Object8.inner), and the wrapper of a Direct3D 9 object is always the
 * same one: the device keeps a table from the Direct3D 9 object to its wrapper, so what GetTexture, GetRenderTarget, GetStreamSource ... hand back is
 * the object the program made. References follow Direct3D 8's rules, not Direct3D 9's: a resource holds a reference of its device, a level of a texture
 * has the texture's references, the back buffers and the depth buffer made with the device have references of their own that start at 0 and hold the
 * device while they have some, shaders and state blocks are numbers (handles) that hold nothing.
 *
 * What is Direct3D 8's own is done here: the caps (shader versions 1.x), the presentation parameters, the render states and texture stage states
 * that are sampler states in Direct3D 9 (and the ones Direct3D 9 dropped), SetIndices with its base vertex index, SetRenderTarget with the depth
 * buffer, CopyRects, state blocks, vertex and pixel shaders by handle - a declaration of D3DVSD tokens becomes a Direct3D 9 vertex declaration and the
 * shader gets dcl instructions (d3d8_shader.h) -, private data, the sizes in the descriptions of surfaces and volumes, ValidateVertexShader and
 * ValidatePixelShader. The interfaces of Direct3D 9 are declared in d3d9_abi.h (d3d8.h and d3d9.h cannot be in one file). */
#define INITGUID
#define COBJMACROS
#define CONST_VTABLE
#define WIN32_LEAN_AND_MEAN
#include "../dxcommon.h"
#include <stddef.h>
#include <d3d8.h>
#include "d3d9_abi.h"
#include "d3d8_shader.h"

#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wcast-function-type"

__asm__(".text\n.globl ___chkstk_ms\n___chkstk_ms:\n ret\n");

/* Direct3D 9's own values that d3d8.h has not (or has with Direct3D 8's meaning). */
#define D3D9_SDK_VERSION 32u
#define D3D9_RS_DEPTHBIAS 195u
#define D3D9_RS_ANTIALIASEDLINEENABLE 176u
#define D3D9_SWAPEFFECT_COPY 3u
#define D3D9_PRESENT_INTERVAL_DEFAULT 0u
#define D3D9_ENUM_WHQL_LEVEL 2u
#define D3D9_DECL_END_STREAM 0xFFu
#define D3D9_PRASTERCAPS_DEPTHBIAS 0x04000000u
#define D3D9_LINECAPS_ANTIALIAS 0x20u
#define D3D9_DTCAPS_UBYTE4 0x1u
#ifndef D3DPRESENT_BACK_BUFFERS_MAX
#define D3DPRESENT_BACK_BUFFERS_MAX 3u
#endif
#ifndef D3DDEVINFOID_VCACHE
#define D3DDEVINFOID_VCACHE 4u
#endif
#define D3DFMT_A2R10G10B10 ((D3DFORMAT)35)      /* (a display format of Direct3D 9 that d3d8types.h has not) */

/* The sizes of Direct3D 9's structures as d3d9.h has them (d3d9_abi.h is generated from it; checked here). */
_Static_assert(sizeof(D3DCAPS9) == 304, "D3DCAPS9");
_Static_assert(sizeof(D3DPRESENT_PARAMETERS9) == (sizeof(void *) == 8 ? 64 : 56), "D3DPRESENT_PARAMETERS");
_Static_assert(sizeof(D3DSURFACE_DESC9) == 32 && sizeof(D3DVOLUME_DESC9) == 28, "D3DSURFACE_DESC, D3DVOLUME_DESC");
_Static_assert(sizeof(D3DADAPTER_IDENTIFIER9) == (sizeof(void *) == 8 ? 1104 : 1100), "D3DADAPTER_IDENTIFIER9");
_Static_assert(sizeof(IDirect3DDevice9Vtbl) == 119 * sizeof(void *), "IDirect3DDevice9");
_Static_assert(sizeof(D8Element) == sizeof(D3DVERTEXELEMENT9), "D3DVERTEXELEMENT9");

/* ---- the objects ----------------------------------------------------------------------------------------------------------------------------------- */

enum {
  KIND_DIRECT3D = 1, KIND_DEVICE, KIND_VERTEX_BUFFER, KIND_INDEX_BUFFER, KIND_TEXTURE, KIND_CUBE_TEXTURE, KIND_VOLUME_TEXTURE, KIND_SURFACE, KIND_VOLUME,
  KIND_SWAP_CHAIN
};

typedef struct Private8 {      /* SetPrivateData: a copy of the bytes, or an interface it holds a reference of */
  GUID guid;
  DWORD size;
  BOOL object;
  void *data;
  struct Private8 *next;
} Private8;

typedef struct Device8 Device8;

/* The head of every Direct3D 8 object (the object is its interface pointer: the vtable comes first). */
typedef struct Object8 {
  const void *lpVtbl;
  LONG ref;
  UINT kind;
  Device8 *device;               /* the device of a resource or swap chain (one reference, except for the implicit surfaces: see obj_AddRef) */
  IUnknown *inner;               /* the Direct3D 9 object that does the work (one reference); NULL for a volume, which is its texture's level */
  struct Object8 *container;     /* a level of a texture: its references are the texture's, and it goes with the texture */
  BOOL implicit;                 /* a back buffer or a depth buffer that came with the device or a swap chain: kept at 0 references */
  BOOL in_table;
  BOOL dead;                     /* released to 0 and emptied, kept a while for a program that releases it once more (Direct3D 8 answers 0 then) */
  Private8 *priv;
  struct Object8 *next_in_bucket;
} Object8;

typedef struct Direct3D8 { Object8 o; IDirect3D9 *d9; } Direct3D8;

typedef struct Surface8 { Object8 o; UINT level, face; } Surface8;      /* (inner: an IDirect3DSurface9) */
typedef struct Volume8 { Object8 o; UINT level; } Volume8;              /* (its container's level: LockBox of the volume texture) */
typedef struct Texture8 {
  Object8 o;                     /* inner: the IDirect3DBaseTexture9 (a texture, cube or volume texture by o.kind) */
  UINT levels, faces;
  Object8 **subs;                /* face * levels + level: the Surface8 (or Volume8) of a level once it was asked for */
} Texture8;
typedef struct SwapChain8 {
  Object8 o;                     /* inner: the IDirect3DSwapChain9 */
  Surface8 *back[3];             /* its back buffers once they were asked for (implicit) */
} SwapChain8;

#define VS_HANDLE_BASE 0xF0000000u      /* a vertex shader handle above this is a shader of CreateVertexShader, one at or below it an FVF code */
#define HANDLE_TABLE_LIMIT 65536u

typedef struct VertexShader8 {
  DWORD *declaration;            /* the program's tokens, as GetVertexShaderDeclaration gives them back */
  UINT declaration_bytes;
  DWORD *function;               /* NULL: the fixed-function pipeline with this declaration */
  UINT function_bytes;
  D8Load loads[D8_MAX_LOADS];    /* the constants the declaration loads when the shader is set, ... */
  UINT load_count;
  float *load_data;              /* ... their numbers, one after the other */
  IDirect3DVertexDeclaration9 *decl9;
  IDirect3DVertexShader9 *vs9;
} VertexShader8;
typedef struct PixelShader8 {
  DWORD *function;
  UINT function_bytes;
  IDirect3DPixelShader9 *ps9;
} PixelShader8;

/* The state that is Direct3D 8's own and not in Direct3D 9's device: what a Direct3D 8 state block records besides its Direct3D 9 block. */
#define EXTRA_LINEPATTERN 0
#define EXTRA_ZVISIBLE 1
#define EXTRA_ZBIAS 2
#define EXTRA_SOFTWAREVERTEXPROCESSING 3
#define EXTRA_PATCHSEGMENTS 4
#define EXTRA_STATES 5
typedef struct State8 {
  DWORD vs, ps;                  /* the handles set */
  UINT base_vertex;              /* SetIndices' base vertex index */
  DWORD extra[EXTRA_STATES];     /* render states Direct3D 9 has not (or keeps in another form) */
} State8;
#define HAS_VS 1u
#define HAS_PS 2u
#define HAS_BASE 4u
#define HAS_EXTRA(i) (8u << (i))
#define HAS_ALL (HAS_VS | HAS_PS | HAS_BASE | HAS_EXTRA(0) | HAS_EXTRA(1) | HAS_EXTRA(2) | HAS_EXTRA(3) | HAS_EXTRA(4))

typedef struct StateBlock8 {
  IDirect3DStateBlock9 *sb9;
  State8 state;
  UINT has;                      /* which of State8 the block holds (HAS_*) */
} StateBlock8;

typedef struct HandleTable { void **entries; UINT count, capacity; } HandleTable;

#define TABLE_BUCKETS 512u
#define GRAVES 64u                       /* resources released to 0 whose empty heads are kept (see object_destroy) */

struct Device8 {
  Object8 o;                     /* inner: the IDirect3DDevice9 */
  IDirect3DDevice9 *d9;
  Direct3D8 *parent;             /* (a reference) */
  CRITICAL_SECTION lock;         /* the wrapper table, the handle tables, the state here */
  Object8 *buckets[TABLE_BUCKETS];
  Object8 *graves[GRAVES];
  UINT grave_next;
  HandleTable vertex_shaders, pixel_shaders, state_blocks;
  State8 state;
  StateBlock8 *recording;        /* between BeginStateBlock and EndStateBlock */
  Surface8 *back[3];             /* the implicit surfaces: the back buffers and the depth buffer made with the device */
  Surface8 *auto_depth;
  D3DPRESENT_PARAMETERS present; /* as the program gave them (with what was chosen for it) */
  DWORD behaviour;
  D3DCAPS9 caps9;
  UINT vs_constants;
  BOOL not_reset;                /* a Reset failed: the device is lost until one succeeds */             /* the vertex shader constants Direct3D 8 lets a program set (the caps' MaxVertexShaderConst) */
};

extern const IDirect3D8Vtbl d3d_vtbl;
extern const IDirect3DDevice8Vtbl dev_vtbl;
extern const IDirect3DVertexBuffer8Vtbl vb_vtbl;
extern const IDirect3DIndexBuffer8Vtbl ib_vtbl;
extern const IDirect3DTexture8Vtbl tex_vtbl;
extern const IDirect3DCubeTexture8Vtbl cube_vtbl;
extern const IDirect3DVolumeTexture8Vtbl vol_vtbl;
extern const IDirect3DSurface8Vtbl surf_vtbl;
extern const IDirect3DVolume8Vtbl volume_vtbl;
extern const IDirect3DSwapChain8Vtbl sc_vtbl;

#define OBJ8(x) ((Object8 *)(x))
#define D9(o) ((o)->d9)
/* The Direct3D 9 interfaces of a wrapper's inner object (the casts are between COM interface pointers of the same object). */
#define INNER_SURFACE(s) ((IDirect3DSurface9 *)OBJ8(s)->inner)
#define INNER_TEXTURE(t) ((IDirect3DTexture9 *)OBJ8(t)->inner)
#define INNER_CUBE(t) ((IDirect3DCubeTexture9 *)OBJ8(t)->inner)
#define INNER_VOLUME_TEXTURE(t) ((IDirect3DVolumeTexture9 *)OBJ8(t)->inner)
#define INNER_BASE(t) ((IDirect3DBaseTexture9 *)OBJ8(t)->inner)
#define INNER_VB(b) ((IDirect3DVertexBuffer9 *)OBJ8(b)->inner)
#define INNER_IB(b) ((IDirect3DIndexBuffer9 *)OBJ8(b)->inner)
#define INNER_SWAP_CHAIN(s) ((IDirect3DSwapChain9 *)OBJ8(s)->inner)

/* The Direct3D 9 calls, through the vtables of d3d9_abi.h. */
#define CALL9(object, method, ...) ((object)->lpVtbl->method((object), ##__VA_ARGS__))

static void unknown_release(IUnknown *u) { if (u) u->lpVtbl->Release(u); }

/* ---- the table from Direct3D 9 objects to their wrappers ------------------------------------------------------------------------------------------- */

static UINT bucket_of(const void *inner) { return (UINT)(((ULONG_PTR)inner >> 4) * 2654435761u) % TABLE_BUCKETS; }
static void table_add(Device8 *d, Object8 *o) {
  if (!d || !o->inner || o->in_table) return;
  EnterCriticalSection(&d->lock);
  const UINT b = bucket_of(o->inner);
  o->next_in_bucket = d->buckets[b];
  d->buckets[b] = o;
  o->in_table = TRUE;
  LeaveCriticalSection(&d->lock);
}
static void table_remove(Device8 *d, Object8 *o) {
  if (!d || !o->in_table) return;
  EnterCriticalSection(&d->lock);
  for (Object8 **p = &d->buckets[bucket_of(o->inner)]; *p; p = &(*p)->next_in_bucket)
    if (*p == o) { *p = o->next_in_bucket; break; }
  o->in_table = FALSE;
  o->next_in_bucket = NULL;
  LeaveCriticalSection(&d->lock);
}
/* The wrapper of a Direct3D 9 object, or NULL (no reference is taken). */
static Object8 *table_find(Device8 *d, const void *inner) {
  if (!inner) return NULL;
  EnterCriticalSection(&d->lock);
  Object8 *o = d->buckets[bucket_of(inner)];
  while (o && o->inner != (IUnknown *)inner) o = o->next_in_bucket;
  LeaveCriticalSection(&d->lock);
  return o;
}

/* ---- references -------------------------------------------------------------------------------------------------------------------------------------- */

static void object_destroy(Object8 *o);

static ULONG STDMETHODCALLTYPE obj_AddRef(Object8 *o) {
  if (o->dead) return 0;
  if (o->container) return CALL9((IUnknown *)o->container, AddRef);
  const LONG n = InterlockedIncrement(&o->ref);
  if (o->implicit && n == 1 && o->device) CALL9((IUnknown *)o->device, AddRef);
  return (ULONG)n;
}
static ULONG STDMETHODCALLTYPE obj_Release(Object8 *o) {
  if (o->dead) return 0;
  if (o->container) return CALL9((IUnknown *)o->container, Release);
  if (o->implicit) {
    /* (an implicit surface at 0 stays at 0: Direct3D 8 lets a program release it more often than it got it, and it goes with the device) */
    LONG n;
    do {
      n = o->ref;
      if (n <= 0) return 0;
    } while (InterlockedCompareExchange(&o->ref, n - 1, n) != n);
    if (n == 1 && o->device) CALL9((IUnknown *)o->device, Release);
    return (ULONG)(n - 1);
  }
  const LONG n = InterlockedDecrement(&o->ref);
  if (n == 0) object_destroy(o);
  return (ULONG)n;
}

/* The interfaces each kind answers QueryInterface for (IUnknown always). */
static int kind_has_interface(UINT kind, REFIID riid) {
  if (guid_eq(riid, &IID_IUnknown)) return 1;
  switch (kind) {
  case KIND_DIRECT3D: return guid_eq(riid, &IID_IDirect3D8);
  case KIND_DEVICE: return guid_eq(riid, &IID_IDirect3DDevice8);
  case KIND_VERTEX_BUFFER: return guid_eq(riid, &IID_IDirect3DResource8) || guid_eq(riid, &IID_IDirect3DVertexBuffer8);
  case KIND_INDEX_BUFFER: return guid_eq(riid, &IID_IDirect3DResource8) || guid_eq(riid, &IID_IDirect3DIndexBuffer8);
  case KIND_TEXTURE: return guid_eq(riid, &IID_IDirect3DResource8) || guid_eq(riid, &IID_IDirect3DBaseTexture8) || guid_eq(riid, &IID_IDirect3DTexture8);
  case KIND_CUBE_TEXTURE: return guid_eq(riid, &IID_IDirect3DResource8) || guid_eq(riid, &IID_IDirect3DBaseTexture8) || guid_eq(riid, &IID_IDirect3DCubeTexture8);
  case KIND_VOLUME_TEXTURE: return guid_eq(riid, &IID_IDirect3DResource8) || guid_eq(riid, &IID_IDirect3DBaseTexture8) || guid_eq(riid, &IID_IDirect3DVolumeTexture8);
  case KIND_SURFACE: return guid_eq(riid, &IID_IDirect3DResource8) || guid_eq(riid, &IID_IDirect3DSurface8);
  case KIND_VOLUME: return guid_eq(riid, &IID_IDirect3DVolume8);
  case KIND_SWAP_CHAIN: return guid_eq(riid, &IID_IDirect3DSwapChain8);
  default: return 0;
  }
}
static HRESULT STDMETHODCALLTYPE obj_QueryInterface(Object8 *o, REFIID riid, void **out) {
  if (!out) return E_POINTER;
  if (!riid) { *out = NULL; return E_POINTER; }
  if (!kind_has_interface(o->kind, riid)) {
    *out = NULL;
    return E_NOINTERFACE;
  }
  obj_AddRef(o);
  *out = o;
  return S_OK;
}

static void obj_init(Object8 *o, const void *vtbl, UINT kind, Device8 *device, IUnknown *inner) {
  o->lpVtbl = vtbl;
  o->ref = 1;
  o->kind = kind;
  o->device = device;
  o->inner = inner;
}
/* A new resource or swap chain of `d`: one reference, and the device's; registered by its inner object. */
static void *resource_new(SIZE_T size, const void *vtbl, UINT kind, Device8 *d, IUnknown *inner) {
  Object8 *o = mem_alloc(size);
  if (!o) return NULL;
  obj_init(o, vtbl, kind, d, inner);
  CALL9((IUnknown *)d, AddRef);
  table_add(d, o);
  return o;
}

/* ---- private data (SetPrivateData of every resource) ------------------------------------------------------------------------------------------------ */

static CRITICAL_SECTION g_private_lock8;

static void private_drop(Private8 *p) {
  if (p->object) unknown_release((IUnknown *)p->data);
  else mem_free(p->data);
  mem_free(p);
}
static void private_clear(Object8 *o) {
  Private8 *p = o->priv;
  o->priv = NULL;
  while (p) { Private8 *next = p->next; private_drop(p); p = next; }
}
static HRESULT STDMETHODCALLTYPE obj_SetPrivateData(Object8 *o, REFGUID guid, const void *data, DWORD size, DWORD flags) {
  if (!guid) return D3DERR_INVALIDCALL;
  const BOOL object = (flags & D3DSPD_IUNKNOWN) != 0;
  if (object && size != sizeof(IUnknown *)) return D3DERR_INVALIDCALL;
  if (!data && size) return D3DERR_INVALIDCALL;
  Private8 *p = mem_alloc(sizeof *p);
  if (!p) return E_OUTOFMEMORY;
  p->guid = *guid;
  p->size = size;
  p->object = object;
  if (object) {
    p->data = (void *)data;
    if (data) ((IUnknown *)data)->lpVtbl->AddRef((IUnknown *)data);
  } else {
    p->data = mem_alloc(size);
    if (!p->data) { mem_free(p); return E_OUTOFMEMORY; }
    if (size) memcpy(p->data, data, size);
  }
  EnterCriticalSection(&g_private_lock8);
  Private8 *old = NULL;
  for (Private8 **pp = &o->priv; *pp; pp = &(*pp)->next)
    if (guid_eq(&(*pp)->guid, guid)) { old = *pp; *pp = old->next; break; }
  p->next = o->priv;
  o->priv = p;
  LeaveCriticalSection(&g_private_lock8);
  if (old) private_drop(old);
  return D3D_OK;
}
static HRESULT STDMETHODCALLTYPE obj_GetPrivateData(Object8 *o, REFGUID guid, void *data, DWORD *size) {
  if (!guid) return D3DERR_INVALIDCALL;
  HRESULT hr = D3DERR_NOTFOUND;
  EnterCriticalSection(&g_private_lock8);
  for (Private8 *p = o->priv; p; p = p->next) {
    if (!guid_eq(&p->guid, guid)) continue;
    if (!size) { hr = D3DERR_INVALIDCALL; break; }
    if (!data) { *size = p->size; hr = D3D_OK; break; }
    if (*size < p->size) { *size = p->size; hr = D3DERR_MOREDATA; break; }
    if (p->object) {
      *(IUnknown **)data = (IUnknown *)p->data;
      if (p->data) ((IUnknown *)p->data)->lpVtbl->AddRef((IUnknown *)p->data);
    } else if (p->size) {
      memcpy(data, p->data, p->size);
    }
    *size = p->size;
    hr = D3D_OK;
    break;
  }
  LeaveCriticalSection(&g_private_lock8);
  return hr;
}
static HRESULT STDMETHODCALLTYPE obj_FreePrivateData(Object8 *o, REFGUID guid) {
  if (!guid) return D3DERR_INVALIDCALL;
  Private8 *found = NULL;
  EnterCriticalSection(&g_private_lock8);
  for (Private8 **pp = &o->priv; *pp; pp = &(*pp)->next)
    if (guid_eq(&(*pp)->guid, guid)) { found = *pp; *pp = found->next; break; }
  LeaveCriticalSection(&g_private_lock8);
  if (!found) return D3DERR_NOTFOUND;
  private_drop(found);
  return D3D_OK;
}

/* ---- handles (shaders, state blocks) ---------------------------------------------------------------------------------------------------------------- */

/* A handle is the entry's index and one; 0 is no handle. Returns 0 when the table is full or there is no memory. */
static DWORD handle_add(HandleTable *t, void *entry) {
  for (UINT i = 0; i < t->count; ++i)
    if (!t->entries[i]) { t->entries[i] = entry; return i + 1; }
  if (t->count == t->capacity) {
    if (t->capacity >= HANDLE_TABLE_LIMIT) return 0;
    const UINT capacity = t->capacity ? t->capacity * 2 : 16;
    void **grown = mem_grow(t->entries, (SIZE_T)capacity * sizeof *grown);
    if (!grown) return 0;
    t->entries = grown;
    t->capacity = capacity;
  }
  t->entries[t->count++] = entry;
  return t->count;
}
static void *handle_get(const HandleTable *t, DWORD handle) { return handle && handle <= t->count ? t->entries[handle - 1] : NULL; }
static void *handle_remove(HandleTable *t, DWORD handle) {
  void *entry = handle_get(t, handle);
  if (entry) t->entries[handle - 1] = NULL;
  return entry;
}

/* ---- the parts ------------------------------------------------------------------------------------------------------------------------------------- */

#include "convert.inc"
#include "resources.inc"
#include "shaders.inc"
#include "state.inc"
#include "device.inc"
#include "direct3d.inc"

/* Frees what an object holds when its last reference goes (implicit surfaces and levels go with their device or container instead). */
static void object_destroy(Object8 *o) {
  Device8 *d = o->device;
  private_clear(o);
  switch (o->kind) {
  case KIND_DEVICE: device_destroy((Device8 *)o); return;
  case KIND_DIRECT3D: unknown_release(o->inner); mem_free(o); return;
  case KIND_TEXTURE: case KIND_CUBE_TEXTURE: case KIND_VOLUME_TEXTURE: texture_free_levels((Texture8 *)o); break;
  case KIND_SWAP_CHAIN: swap_chain_free_buffers((SwapChain8 *)o); break;
  default: break;
  }
  table_remove(d, o);
  unknown_release(o->inner);
  o->inner = NULL;
  /* A resource's head is kept for a while, emptied: Direct3D 8 answers a Release of an object that has no references left with 0 (Wine's
   * test_refcount releases a texture twice more), so the memory must still be there. The oldest of GRAVES such heads is freed for a new one. */
  if (d) {
    o->dead = TRUE;
    EnterCriticalSection(&d->lock);
    Object8 *oldest = d->graves[d->grave_next];
    d->graves[d->grave_next] = o;
    d->grave_next = (d->grave_next + 1) % GRAVES;
    LeaveCriticalSection(&d->lock);
    mem_free(oldest);
    CALL9((IUnknown *)d, Release);
  } else {
    mem_free(o);
  }
}

#include "vtbl.inc"

/* ---- the entry points ------------------------------------------------------------------------------------------------------------------------------- */

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved) {
  (void)instance; (void)reserved;
  if (reason == DLL_PROCESS_ATTACH) {
    InitializeCriticalSection(&g_private_lock8);
    InitializeCriticalSection(&g_private_lock);      /* (dxcommon.h's, unused here but initialised as every library of it does) */
  }
  return TRUE;
}

/* Implemented in d3d9.dll. */
__declspec(dllimport) IDirect3D9 *WINAPI Direct3DCreate9(UINT sdk_version);

EXPORT IDirect3D8 *WINAPI Direct3DCreate8(UINT sdk_version) {
  (void)sdk_version;      /* (Direct3D 8 takes any: programs of the 8.0 SDK pass 120, of 8.1 220) */
  IDirect3D9 *d9 = Direct3DCreate9(D3D9_SDK_VERSION);
  if (!d9) return NULL;
  Direct3D8 *o = mem_alloc(sizeof *o);
  if (!o) { CALL9(d9, Release); return NULL; }
  obj_init(&o->o, &d3d_vtbl, KIND_DIRECT3D, NULL, (IUnknown *)d9);
  o->d9 = d9;
  return (IDirect3D8 *)o;
}

/* The debug runtime's switch for its messages: the retail runtime has none to mute. */
EXPORT void WINAPI DebugSetMute(void) {}

/* Writes the text of a validation's result where `errors` points (a string of the process heap the caller frees), empty unless `return_error`. */
static void validation_text(char **errors, BOOL return_error, int result) {
  if (!errors) return;
  const char *message = return_error ? d8_validation_messages[result] : "";
  const SIZE_T n = alen(message) + 1;
  char *text = HeapAlloc(GetProcessHeap(), 0, n);
  if (text) memcpy(text, message, n);
  *errors = text;
}

EXPORT HRESULT WINAPI ValidateVertexShader(const DWORD *code, const DWORD *declaration, const D3DCAPS8 *caps, BOOL return_error, char **errors) {
  const int result = d8_validate_vertex_shader((const uint32_t *)code, code ? SIZE_MAX / 8 : 0, (const uint32_t *)declaration, SIZE_MAX / 8,
                                               caps ? caps->VertexShaderVersion : 0, caps ? caps->MaxVertexShaderConst : 0);
  validation_text(errors, return_error, result);
  return result == D8V_OK ? S_OK : E_FAIL;
}
EXPORT HRESULT WINAPI ValidatePixelShader(const DWORD *code, const D3DCAPS8 *caps, BOOL return_error, char **errors) {
  if (!code) return E_FAIL;      /* (no text for no code: Windows leaves `errors` as it is) */
  const int result = d8_validate_pixel_shader((const uint32_t *)code, SIZE_MAX / 8, caps ? caps->PixelShaderVersion : 0);
  validation_text(errors, return_error, result);
  return result == D8V_OK ? S_OK : E_FAIL;
}

/* D3D8GetSWInfo (an export of Windows' d3d8.dll for its software rasterizer, which takes no parameters): the description of the software device's
 * callbacks, which this Direct3D 8 has not - it draws through Direct3D 9 only. */
EXPORT void *WINAPI D3D8GetSWInfo(void) { return NULL; }
