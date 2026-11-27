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
/* Direct3D 1, 2, 3 and 7 (immediate mode) of ddraw.dll, on this system's d3d9.dll (docs/DirectDraw.md, "The Direct3D part"). Included by ddraw.c after
 * the DirectDraw part, whose hooks (DdD3dHooks) it fills in.
 *
 * Design:
 *   - One Direct3D 9 device per DirectDraw object (D3dDD, dd->d3d), made when the first Direct3D device or texture needs it, on a hidden window of its
 *     own; its back buffer is never shown. Every IDirect3DDevice of the object draws through it: the device whose states are on it is `active`, and
 *     another one puts all of its own back (d3_flush) before it draws.
 *   - The pixels of a DirectDraw surface stay what counts. A surface Direct3D draws into or samples has a Direct3D 9 copy (D3dRes, the store's
 *     shadow.resource, so Flip moves it with the memory): a render target (X8R8G8B8 / A8R8G8B8, with a system memory twin for the copies both ways),
 *     a depth buffer (D32F_LOCKABLE, or D24S8 when the format has stencil bits) or a texture (A8R8G8B8 / X8R8G8B8 with all its levels or faces).
 *     DD_SHADOW_CPU_NEWER (the CPU wrote it) sends the CPU's pixels before Direct3D uses the copy - a rectangle of one colour or depth as a Clear -,
 *     DD_SHADOW_GPU_NEWER (Direct3D drew into it) brings them back when the CPU reads (DdD3dHooks.pull: Lock, GetDC, Blt, the primary on the screen).
 *     Textures are converted on the CPU (palettes, every format, the colour key as alpha 0: dd_to_argb).
 *   - Direct3D 7 draws with Direct3D 9's fixed function (transforms, lights, materials, texture stages, clip planes). Direct3D 1, 2 and 3 transform,
 *     light and clip on the CPU (d3d_legacy.h: the legacy lighting model, the viewports' clip spaces) and draw the vertices transformed.
 *   - Execute buffers, handles, viewports, lights and materials are this layer's own objects.
 * Files: d3d.inc (this: objects, the Direct3D 9 device, the copies of surfaces, the hooks), d3d_state.inc (device states, state blocks, what goes to
 * Direct3D 9 before a draw), d3d_draw.inc (drawing, clearing), d3d_device.inc (the device interfaces), d3d_objects.inc (IDirect3D, viewports,
 * lights, materials, execute buffers, vertex buffers, textures), d3d_vtbl.inc (the vtables and the registration). */

#include "d3d9_ddraw.h"
#include "d3d_legacy.h"

/* (values of Direct3D 9 that are not in d3d9_ddraw.h: they are macros in d3d9types.h, or enumerations whose names Direct3D 7 has) */
#define D9_SDK_VERSION 32u
#define D9_CREATE_FPU_PRESERVE 0x00000002u
#define D9_CREATE_MULTITHREADED 0x00000004u
#define D9_CREATE_SOFTWARE_VP 0x00000020u
#define D9_USAGE_RENDERTARGET 0x00000001u
#define D9_USAGE_DEPTHSTENCIL 0x00000002u
#define D9_LOCK_READONLY 0x00000010u
#define D9_LOCK_DISCARD 0x00002000u
#define D9_CLEAR_TARGET 0x1u
#define D9_CLEAR_ZBUFFER 0x2u
#define D9_CLEAR_STENCIL 0x4u
#define D9_PT_POINTLIST 1u
#define D9_PT_LINELIST 2u
#define D9_PT_TRIANGLELIST 4u
#define D9_TS_VIEW 2u
#define D9_TS_PROJECTION 3u
#define D9_TS_TEXTURE0 16u
#define D9_TS_WORLD 256u
#define D9_RS_ALPHATESTENABLE 15u
#define D9_RS_ALPHAREF 24u
#define D9_RS_ALPHAFUNC 25u
#define D9_RS_CMP_GREATER 5u
#define D9_RS_CMP_GREATEREQUAL 7u
#define D9_RS_LIGHTING 137u
#define D9_RS_CLIPPING 136u
#define D9_RS_FOGTABLEMODE 35u
#define D9_RS_FOGVERTEXMODE 140u
#define D9_RS_ZENABLE 7u
#define D9_RS_TEXTUREFACTOR 60u
#define D9_TSS_COLOROP 1u
#define D9_TSS_ALPHAOP 4u
#define D9_TSS_TEXCOORDINDEX 11u
#define D9_TSS_TEXTURETRANSFORMFLAGS 24u
#define D9_SAMP_MAXMIPLEVEL 9u

#define D3D_CALL(obj, method, ...) ((obj)->lpVtbl->method((obj), ##__VA_ARGS__))

/* (Direct3D 7 values mingw's d3dtypes.h does not have) */
#ifndef D3DRENDERSTATE_STIPPLEPATTERN00
#define D3DRENDERSTATE_STIPPLEPATTERN00 64
#endif
#ifndef D3DERR_INVALIDVERTEXFORMAT
#define D3DERR_INVALIDVERTEXFORMAT MAKE_DDHRESULT(2138)
#endif

typedef struct D3dDD D3dDD;
typedef struct D3dDevice D3dDevice;
typedef struct D3dViewport D3dViewport;
typedef struct D3dLight D3dLight;
typedef struct D3dMaterial D3dMaterial;
typedef struct D3dExecuteBuffer D3dExecuteBuffer;
typedef struct D3dVertexBuffer D3dVertexBuffer;
typedef struct D3dStateBlock D3dStateBlock;

/* ---- handles ----------------------------------------------------------------------------------------------------------------------------------------
 * Direct3D 1-3 name materials, textures and matrices by handles (D3DMATERIALHANDLE ...), which the execute buffers and render states carry; Direct3D 7
 * names state blocks so. Materials, textures and matrices are in one table of the process (as Windows: a handle may be used with any device), state
 * blocks in their device's. A handle is its index + 1. */
enum { H_FREE = 0, H_MATERIAL, H_TEXTURE, H_MATRIX, H_STATEBLOCK };
typedef struct D3dHandle { UINT type; void *object; D3dDevice *owner; } D3dHandle;
typedef struct D3dHandles { D3dHandle *entries; UINT count, capacity, free_hint; } D3dHandles;
static D3dHandles g_handles;      /* (under dd_lock) */

static DWORD handle_alloc(D3dHandles *t, UINT type, void *object, D3dDevice *owner) {
  for (UINT i = 0; i < t->count; ++i) {
    const UINT k = (t->free_hint + i) % t->count;
    if (t->entries[k].type == H_FREE) {
      t->entries[k].type = type;
      t->entries[k].object = object;
      t->entries[k].owner = owner;
      t->free_hint = k + 1;
      return k + 1;
    }
  }
  if (t->count == t->capacity) {
    const UINT capacity = t->capacity ? t->capacity * 2 : 64;
    if (capacity > 0x100000u) return 0;
    D3dHandle *grown = mem_grow(t->entries, (SIZE_T)capacity * sizeof *grown);
    if (!grown) return 0;
    t->entries = grown;
    t->capacity = capacity;
  }
  D3dHandle *e = &t->entries[t->count++];
  e->type = type;
  e->object = object;
  e->owner = owner;
  return t->count;
}
static void *handle_get(const D3dHandles *t, DWORD handle, UINT type) {
  if (!handle || handle > t->count) return NULL;
  const D3dHandle *e = &t->entries[handle - 1];
  return e->type == type ? e->object : NULL;
}
static void *handle_free(D3dHandles *t, DWORD handle, UINT type) {
  if (!handle || handle > t->count) return NULL;
  D3dHandle *e = &t->entries[handle - 1];
  if (e->type != type) return NULL;
  void *object = e->object;
  e->type = H_FREE;
  e->object = NULL;
  e->owner = NULL;
  return object;
}

/* ---- the objects ------------------------------------------------------------------------------------------------------------------------------------ */

/* The Direct3D 9 copy of a surface's store (s->store.shadow.resource). */
enum { RES_TARGET = 1, RES_DEPTH, RES_TEXTURE };
typedef struct D3dRes {
  UINT kind;
  D3dDD *owner;                  /* the Direct3D 9 device it is of */
  IDirect3DSurface9 *surface;    /* a target or depth buffer: what is drawn into (a target texture's level 0) */
  IDirect3DSurface9 *sys;        /* a target's twin in system memory (GetRenderTargetData, UpdateSurface), made when it is first needed */
  IDirect3DTexture9 *texture;    /* a texture (with D3DUSAGE_RENDERTARGET when the surface is a 3D device too) */
  IDirect3DCubeTexture9 *cube;
  UINT width, height, levels;
  D3DFORMAT format;
  BOOL target_texture;           /* a texture that is drawn into (DDSCAPS_3DDEVICE): its levels go through `sys` */
  BOOL keyed;                    /* the texture was sent with the colour key as alpha 0 */
  DWORD key_low, key_high;
  BOOL has_alpha;                /* the texture's alpha means something (the format's alpha; a colour key is not) */
  BOOL fresh;                    /* just made: everything must be sent */
  BOOL depth_lockable;           /* a depth buffer with stencil that d3d9.dll locks (TCOS_USAGE_LOCKABLE_DEPTH: depth in the low 24 bits, stencil above) */
  DWORD lod;
} D3dRes;

/* What Direct3D keeps of a surface (s->d3d). */
typedef struct D3dSurf {
  D3dDevice *device1;            /* the IDirect3DDevice its QueryInterface made (aggregated into the surface) */
  DWORD handle;                  /* its texture handle (IDirect3DTexture(2)::GetHandle), 0 none */
} D3dSurf;

/* The Direct3D 9 device of a DirectDraw object and what is shared by its Direct3D devices (dd->d3d). */
#define DECL_CACHE 32u
typedef struct D3dDD {
  DDraw *dd;
  HMODULE module;
  IDirect3D9 *d9;
  IDirect3DDevice9 *dev;
  HWND window;
  D3DCAPS9 caps9;
  DlCaps9 caps;
  BOOL failed;                   /* making the Direct3D 9 device failed: Direct3D answers DDERR_GENERIC */
  D3dDevice *devices;            /* the devices (next) */
  D3dDevice *active;             /* whose states the Direct3D 9 device has */
  IDirect3DSurface9 *bound_target, *bound_depth;
  struct { DWORD fvf; IDirect3DVertexDeclaration9 *decl; } decls[DECL_CACHE];
  UINT decl_next;
  BYTE *scratch[4];
  SIZE_T scratch_size[4];
  DWORD tex_stamp;               /* changes when a texture copy is made again (the devices bind it again) */
  /* d3d9.dll's private calls (its device.inc), found when the device is this system's d3d9.dll's own (d3_dd: IID_ITcosObject gives the device itself back, and the
   * module has the exports). NULL for another Direct3D 9 - a proxy d3d9.dll whose wrappers are not seen through, or another runtime -: colour keys are then the
   * alpha test of the alpha-0 texels, and a depth buffer with stencil is not locked. */
  HRESULT(WINAPI *create_lockable_depth)(IDirect3DDevice9 *, UINT, UINT, D3DFORMAT, D3DMULTISAMPLE_TYPE, DWORD, BOOL, IDirect3DSurface9 **);
  HRESULT(WINAPI *set_key_kill)(IDirect3DDevice9 *, DWORD, BOOL);
} D3dDD;

/* A light of Direct3D 7 (SetLight / LightEnable, by index). */
typedef struct D3dLight7 { D3DLIGHT7 light; BOOL set, enabled; } D3dLight7;

#define D3D_STAGES 8u
#define D3D_TSS 32u
#define D3D_XF 24u               /* D3DTRANSFORMSTATETYPE numbers: 1 world, 2 view, 3 projection, 4-6 world 1-3, 16-23 texture 0-7 */
#define D3D_PLANES 32u

/* The states of a device a state block can hold. */
typedef struct D3dState {
  DWORD rs[256];
  DWORD tss[D3D_STAGES][D3D_TSS];
  Surface *texture[D3D_STAGES];
  DlMatrix xf[D3D_XF];
  D3DVIEWPORT7 viewport;
  D3DMATERIAL7 material;
  D3dLight7 *lights;
  UINT light_count;
  float plane[D3D_PLANES][4];
} D3dState;
/* Which of them (state blocks, and the dirty marks of what Direct3D 9 must still get). */
typedef struct D3dMask {
  DWORD rs[8];
  DWORD tss[D3D_STAGES];
  DWORD texture;
  DWORD xf;
  BOOL viewport, material;
  BYTE *lights;                  /* per light index: 1 its parameters, 2 its enable */
  UINT light_count;
  DWORD planes;
} D3dMask;

struct D3dStateBlock {
  D3dState state;
  D3dMask mask;
};

struct D3dDevice {
  const IDirect3DDevice7Vtbl *vt7;
  const IDirect3DDevice3Vtbl *vt3;
  const IDirect3DDevice2Vtbl *vt2;
  const IDirect3DDeviceVtbl *vt1;
  const IUnknownVtbl *vt_inner;
  LONG ref;
  UINT version;                  /* 1, 2, 3 or 7 */
  IUnknown *outer;               /* where AddRef/Release go: the surface for a device of Direct3D 1, the inner IUnknown else */
  DDraw *dd;
  D3dDD *dd3;
  int kind;                      /* DL_DEVICE_* */
  BOOL fpu_preserve;             /* made with DDSCL_FPUPRESERVE: Direct3D 7's methods set the FPU up and put the program's control word back */
  DWORD matrix_handle[4];        /* Direct3D 1: the matrices STATETRANSFORM made world, view, projection (SetMatrix of them changes the transform) */
  Surface *target;
  IUnknown *rt_iface;            /* the interface of the target the device holds a reference of (not for Direct3D 1) */
  D3dDevice *next;
  D3dHandles blocks;             /* state blocks (Direct3D 7) */
  /* states */
  D3dState st;
  float pv_power;                /* the last material power above 0 (Direct3D 7's ProcessVertices lights with it when the material's is 0) */
  D3dMask dirty;
  BOOL all_dirty;                /* everything goes to Direct3D 9 again (it was another device's) */
  DWORD light_state[9];          /* D3DLIGHTSTATE_* of Direct3D 2 and 3 (1 .. 8) */
  DWORD texture_map_blend;       /* D3DRENDERSTATE_TEXTUREMAPBLEND as set (0: texture stage states were set since) */
  BOOL legacy_blend;             /* TEXTUREMAPBLEND drives stage 0 */
  DlMatrix clip;                 /* Direct3D 1-3: the current viewport's clip space */
  D3DCLIPSTATUS clip_status;
  D3DSTATS stats;
  BOOL in_scene;
  D3dStateBlock *recording;      /* BeginStateBlock .. EndStateBlock */
  /* viewports (Direct3D 1-3) */
  D3dViewport *viewports;        /* AddViewport (next_in_device) */
  D3dViewport *current_viewport;
  /* Begin .. End (Direct3D 2 and 3) */
  BOOL begun, begun_indexed;
  DWORD begin_type, begin_fvf, begin_flags;
  BYTE *begin_vertices;
  UINT begin_vertex_count, begin_vertex_capacity;
  WORD *begin_indices;
  UINT begin_index_count, begin_index_capacity;
  /* Direct3D 9 objects of the device */
  DWORD bound_stamp[D3D_STAGES];  /* the texture copies Direct3D 9 has bound (D3dDD.tex_stamp when bound) */
  D3dRes *bound_res[D3D_STAGES];
  UINT bound_key_kill;           /* bit n: stage n's TCOS_TSS_KEY_KILL is set in Direct3D 9 (~0: not known) */
  BOOL key_alpha_test;           /* without d3d9.dll's key kill: the alpha test stands in for it (bind_textures), and the program's own is put back after */
  IDirect3DQuery9 *occlusion;    /* D3DRENDERSTATE_ZVISIBLE */
  /* Direct3D 1: the processed vertices of execute buffers (D3DTLVERTEX) and their clip-space forms */
  BYTE *tl;
  DlClipVertex *tl_clip;
  UINT tl_count;
  /* picking (IDirect3DDevice::Pick) */
  D3DPICKRECORD *picks;
  UINT pick_count;
};

/* IDirect3DViewport, 2 and 3: one object. */
struct D3dViewport {
  const IDirect3DViewport3Vtbl *vt;
  LONG ref;
  DDraw *dd;
  UINT set;                      /* 0: no viewport set yet, 1: SetViewport, 2: SetViewport2 */
  DlViewport1 vp1;
  DlViewport2 vp2;
  D3dDevice *device;             /* AddViewport's device */
  D3dViewport *next_in_device;
  D3dLight *lights;              /* AddLight (next_in_viewport) */
  DWORD background;              /* SetBackground's material handle */
  Surface *background_depth;     /* SetBackgroundDepth (a reference of background_depth_iface) */
  IUnknown *background_depth_iface;
};

/* IDirect3DLight. */
struct D3dLight {
  const IDirect3DLightVtbl *vt;
  LONG ref;
  DDraw *dd;
  DlLight2 light;                /* D3DLIGHT2 (a D3DLIGHT is kept with dwFlags D3DLIGHT_ACTIVE) */
  BOOL set;
  D3dViewport *viewport;
  D3dLight *next_in_viewport;
};

/* IDirect3DMaterial, 2 and 3: one object. */
struct D3dMaterial {
  const IDirect3DMaterial3Vtbl *vt3;
  const IDirect3DMaterial2Vtbl *vt2;
  const IDirect3DMaterialVtbl *vt1;
  LONG ref;
  DDraw *dd;
  DlMaterial material;
  /* GetHandle's handles: one for each device asked with (Windows gives another device another handle - Wine's test_multiple_devices -, and each
   * names the material for any device) */
  struct D3dMaterialHandle { const void *device; DWORD handle; } *handles;
  UINT handle_count;
};

/* IDirect3DExecuteBuffer. */
struct D3dExecuteBuffer {
  const IDirect3DExecuteBufferVtbl *vt;
  LONG ref;
  D3dDevice *device;
  BYTE *memory;                  /* the buffer (the program's own with D3DDEB_LPDATA) */
  BOOL own;
  DWORD size, caps;
  BOOL locked;
  DlExecuteData data;
};

/* IDirect3DVertexBuffer and IDirect3DVertexBuffer7: one object. */
struct D3dVertexBuffer {
  const IDirect3DVertexBuffer7Vtbl *vt7;
  const IDirect3DVertexBufferVtbl *vt1;
  LONG ref;
  UINT version;                  /* 3 or 7 */
  DDraw *dd;
  IUnknown *ddraw_iface;         /* the interface of the DirectDraw object it holds a reference of (version 7: IDirectDraw7; 3: none) */
  D3DVERTEXBUFFERDESC desc;
  DWORD stride;
  BYTE *memory;
  SIZE_T size;
  DWORD *clip_codes;             /* ProcessVertices with D3DVOP_CLIP */
  UINT lock_count;
  BOOL optimized;
};

#define D3D_IMPL(type, field, iface) ((type *)((BYTE *)(iface) - offsetof(type, field)))
#define DEV7(i) D3D_IMPL(D3dDevice, vt7, i)
#define DEV3(i) D3D_IMPL(D3dDevice, vt3, i)
#define DEV2(i) D3D_IMPL(D3dDevice, vt2, i)
#define DEV1(i) D3D_IMPL(D3dDevice, vt1, i)
#define DEVI7(d) ((IDirect3DDevice7 *)&(d)->vt7)
#define DEVI3(d) ((IDirect3DDevice3 *)&(d)->vt3)
#define DEVI2(d) ((IDirect3DDevice2 *)&(d)->vt2)
#define DEVI1(d) ((IDirect3DDevice *)&(d)->vt1)
#define D3DI7(dd) ((IDirect3D7 *)&(dd)->vt_d3d7)
#define D3DI3(dd) ((IDirect3D3 *)&(dd)->vt_d3d3)
#define D3DI2(dd) ((IDirect3D2 *)&(dd)->vt_d3d2)
#define D3DI1(dd) ((IDirect3D *)&(dd)->vt_d3d1)

static D3dSurf *d3_surf(Surface *s) {
  if (!s->d3d) s->d3d = mem_alloc(sizeof(D3dSurf));
  return s->d3d;
}

/* ---- the FPU ----------------------------------------------------------------------------------------------------------------------------------------
 * Without DDSCL_FPUPRESERVE, Direct3D sets the x87 FPU to single precision with every exception masked when a device is made (as Windows; Wine's
 * test_fpu_setup); with it, Direct3D 7 keeps the program's control word and works with its own (each method sets it and puts it back). */
static WORD fpu_get(void) {
#if defined(__i386__) || defined(__x86_64__)
  WORD cw;
  __asm__ __volatile__("fnstcw %0" : "=m"(cw));
  return cw;
#else
  return 0;
#endif
}
static void fpu_set(WORD cw) {
#if defined(__i386__) || defined(__x86_64__)
  __asm__ __volatile__("fldcw %0" : : "m"(cw));
#else
  (void)cw;
#endif
}
static WORD fpu_setup(void) {
  const WORD old = fpu_get();
  fpu_set((WORD)((old & ~0xF3Fu) | 0x3Fu));
  return old;
}

/* ---- the Direct3D 9 device -------------------------------------------------------------------------------------------------------------------------- */

static LRESULT CALLBACK d3_window_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) { return DefWindowProcA(hwnd, msg, wp, lp); }

static void d3_caps_from9(const D3DCAPS9 *c, DlCaps9 *out) {
  zero(out, sizeof *out);
  out->DevCaps = c->DevCaps;
  out->PrimitiveMiscCaps = c->PrimitiveMiscCaps;
  out->RasterCaps = c->RasterCaps;
  out->ZCmpCaps = c->ZCmpCaps;
  out->SrcBlendCaps = c->SrcBlendCaps;
  out->DestBlendCaps = c->DestBlendCaps;
  out->AlphaCmpCaps = c->AlphaCmpCaps;
  out->ShadeCaps = c->ShadeCaps;
  out->TextureCaps = c->TextureCaps;
  out->TextureFilterCaps = c->TextureFilterCaps;
  out->TextureAddressCaps = c->TextureAddressCaps;
  out->LineCaps = c->LineCaps;
  out->MaxTextureWidth = c->MaxTextureWidth;
  out->MaxTextureHeight = c->MaxTextureHeight;
  out->MaxTextureRepeat = c->MaxTextureRepeat;
  out->MaxTextureAspectRatio = c->MaxTextureAspectRatio;
  out->MaxAnisotropy = c->MaxAnisotropy;
  out->MaxVertexW = c->MaxVertexW;
  out->GuardBandLeft = c->GuardBandLeft;
  out->GuardBandTop = c->GuardBandTop;
  out->GuardBandRight = c->GuardBandRight;
  out->GuardBandBottom = c->GuardBandBottom;
  out->ExtentsAdjust = c->ExtentsAdjust;
  out->StencilCaps = c->StencilCaps;
  out->FVFCaps = c->FVFCaps;
  out->TextureOpCaps = c->TextureOpCaps;
  out->MaxTextureBlendStages = c->MaxTextureBlendStages;
  out->MaxSimultaneousTextures = c->MaxSimultaneousTextures;
  out->VertexProcessingCaps = c->VertexProcessingCaps;
  out->MaxActiveLights = c->MaxActiveLights;
  out->MaxUserClipPlanes = c->MaxUserClipPlanes;
  out->MaxVertexBlendMatrices = c->MaxVertexBlendMatrices;
  out->color_key = 1;      /* (colour keyed textures: alpha 0 and the alpha test, d3d_state.inc) */
}

/* The Direct3D state of a DirectDraw object, made with its Direct3D 9 device on first use. NULL when Direct3D 9 is not there (the caller answers
 * DDERR_GENERIC; Windows' answer for a driver without Direct3D). */
static D3dDD *d3_dd(DDraw *dd) {
  D3dDD *d = dd->d3d;
  if (d && d->dev) return d;
  if (d && d->failed) return NULL;
  if (!d) {
    d = mem_alloc(sizeof *d);
    if (!d) return NULL;
    d->dd = dd;
    dd->d3d = d;
  }
  const WORD cw = fpu_get();
  /* This OS's d3d9.dll by its System32 path (SysWOW64 for a 32-bit program: the loader's redirection), never a d3d9.dll the program brought or loaded: Windows'
     ddraw.dll does not use Direct3D 9 at all, so a program's own d3d9.dll (DXVK, a ReShade proxy) must not change what ddraw draws with (docs/WindowsUserMode.md). */
  {
    WCHAR path[MAX_PATH + 16];
    static const WCHAR file[] = {'\\', 'd', '3', 'd', '9', '.', 'd', 'l', 'l', 0};
    const UINT n = GetSystemDirectoryW(path, MAX_PATH);
    if (n > 0 && n < MAX_PATH) {
      for (UINT i = 0; i < sizeof file / sizeof file[0]; ++i) path[n + i] = file[i];
      d->module = LoadLibraryW(path);
    } else {
      d->module = NULL;
    }
  }
  PFN_Direct3DCreate9 create = d->module ? (PFN_Direct3DCreate9)(void *)GetProcAddress(d->module, "Direct3DCreate9") : NULL;
  d->d9 = create ? create(D9_SDK_VERSION) : NULL;
  if (d->d9) {
    static ATOM atom;
    if (!atom) {
      WNDCLASSA wc;
      zero(&wc, sizeof wc);
      wc.lpfnWndProc = d3_window_proc;
      wc.hInstance = g_module;
      wc.lpszClassName = "TcosDirect3DLegacy";
      atom = RegisterClassA(&wc);
    }
    d->window = CreateWindowExA(0, "TcosDirect3DLegacy", "Direct3D", WS_POPUP, 0, 0, 8, 8, NULL, NULL, g_module, NULL);
    D3DPRESENT_PARAMETERS pp;
    zero(&pp, sizeof pp);
    pp.BackBufferWidth = 8;
    pp.BackBufferHeight = 8;
    pp.BackBufferFormat = D3DFMT_X8R8G8B8;
    pp.BackBufferCount = 1;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.hDeviceWindow = d->window;
    pp.Windowed = TRUE;
    HRESULT hr = d->window ? D3D_CALL(d->d9, CreateDevice, 0, D3DDEVTYPE_HAL, d->window, D9_CREATE_FPU_PRESERVE | D9_CREATE_MULTITHREADED | D9_CREATE_SOFTWARE_VP,
                                      &pp, &d->dev)
                           : E_FAIL;
    if (FAILED(hr)) d->dev = NULL;
  }
  fpu_set(cw);
  if (!d->dev) {
    if (d->d9) D3D_CALL(d->d9, Release);
    d->d9 = NULL;
    if (d->window) DestroyWindow(d->window);
    d->window = NULL;
    d->failed = TRUE;
    return NULL;
  }
  D3D_CALL(d->dev, GetDeviceCaps, &d->caps9);
  d3_caps_from9(&d->caps9, &d->caps);
  /* (d3d9.dll's own extensions only on a device that is its own: one that answers IID_ITcosObject with itself - not a wrapper, not another runtime's - and a
   * module that has the private exports, which check the device again; src/Windows/tcos_object.h) */
  d->create_lockable_depth = NULL;
  d->set_key_kill = NULL;
  if (tcos_canonical(d->dev) == (IUnknown *)d->dev) {
    d->create_lockable_depth = (HRESULT(WINAPI *)(IDirect3DDevice9 *, UINT, UINT, D3DFORMAT, D3DMULTISAMPLE_TYPE, DWORD, BOOL, IDirect3DSurface9 **))(void *)GetProcAddress(
        d->module, "TcosD3D9CreateLockableDepthStencil");
    d->set_key_kill = (HRESULT(WINAPI *)(IDirect3DDevice9 *, DWORD, BOOL))(void *)GetProcAddress(d->module, "TcosD3D9SetStageKeyKill");
  }
  return d;
}

/* The caps of a DirectDraw object's Direct3D (made with its device): NULL if there is none. */
static const DlCaps9 *d3_caps(DDraw *dd) {
  D3dDD *d = d3_dd(dd);
  return d ? &d->caps : NULL;
}

/* A scratch buffer of the device (slot 0 .. 3), at least `size` bytes; NULL if out of memory. */
static BYTE *d3_scratch(D3dDD *d, UINT slot, SIZE_T size) {
  if (d->scratch_size[slot] >= size) return d->scratch[slot];
  SIZE_T n = d->scratch_size[slot] ? d->scratch_size[slot] : 4096;
  while (n < size) {
    if (n > ((SIZE_T)1 << 30)) return NULL;
    n *= 2;
  }
  BYTE *p = mem_alloc(n);
  if (!p) return NULL;
  mem_free(d->scratch[slot]);
  d->scratch[slot] = p;
  d->scratch_size[slot] = n;
  return p;
}

/* The Direct3D 9 declaration of an FVF code (cached; NULL if it cannot be made). */
static IDirect3DVertexDeclaration9 *d3_decl(D3dDD *d, DWORD fvf) {
  for (UINT i = 0; i < DECL_CACHE; ++i)
    if (d->decls[i].decl && d->decls[i].fvf == fvf) return d->decls[i].decl;
  DlVertexElement e[DL_MAX_ELEMENTS];
  if (!dl_fvf_elements(fvf, e)) return NULL;
  IDirect3DVertexDeclaration9 *decl = NULL;
  if (FAILED(D3D_CALL(d->dev, CreateVertexDeclaration, (const D3DVERTEXELEMENT9 *)e, &decl))) return NULL;
  const UINT slot = d->decl_next++ % DECL_CACHE;
  if (d->decls[slot].decl) D3D_CALL(d->decls[slot].decl, Release);
  d->decls[slot].fvf = fvf;
  d->decls[slot].decl = decl;
  return decl;
}

/* ---- the copies of surfaces ------------------------------------------------------------------------------------------------------------------------- */

static BOOL format_has_alpha(UINT fmt) { return dd_format(fmt)->a != 0 || dd_format_is_palette(fmt); }
static D3DFORMAT target_format(UINT fmt) { return dd_format(fmt)->a ? D3DFMT_A8R8G8B8 : D3DFMT_X8R8G8B8; }

static void res_free(D3dRes *r) {
  if (!r) return;
  if (r->sys) D3D_CALL(r->sys, Release);
  if (r->surface) D3D_CALL(r->surface, Release);
  if (r->texture) D3D_CALL(r->texture, Release);
  if (r->cube) D3D_CALL(r->cube, Release);
  mem_free(r);
}
/* The copy of a surface's store, if it has one of this device. */
static D3dRes *res_of(Surface *s, D3dDD *d) {
  D3dRes *r = s->store.shadow.resource;
  return r && r->owner == d ? r : NULL;
}

/* Whether a rectangle of an image is all one raw value (the CPU filled it: a Clear does the same on the GPU). */
static BOOL image_uniform(const DdImage *im, const RECT *r, uint32_t *value) {
  const DdFormatInfo *f = dd_format(im->fmt);
  if (!f->bytes || f->kind == DD_KIND_YUV_PACKED) return FALSE;
  const uint32_t first = dd_get_raw(im, (uint32_t)r->left, (uint32_t)r->top);
  for (LONG y = r->top; y < r->bottom; ++y) {
    const BYTE *row = im->data + (SIZE_T)y * im->pitch + (SIZE_T)r->left * f->bytes;
    if (f->bytes == 4) {
      const DWORD *p = (const DWORD *)row;
      for (LONG x = r->left; x < r->right; ++x, ++p) if (*p != first) return FALSE;
    } else if (f->bytes == 2) {
      const WORD *p = (const WORD *)row;
      for (LONG x = r->left; x < r->right; ++x, ++p) if (*p != (WORD)first) return FALSE;
    } else {
      for (LONG x = r->left; x < r->right; ++x)
        if (dd_get_raw(im, (uint32_t)x, (uint32_t)y) != first) return FALSE;
    }
  }
  *value = first;
  return TRUE;
}

static void surface_rect(const Surface *s, const RECT *in, RECT *out) {
  rect_set(out, 0, 0, (LONG)s->desc.dwWidth, (LONG)s->desc.dwHeight);
  if (in) {
    RECT r;
    if (!rect_intersect(&r, out, in)) { rect_set(out, 0, 0, 0, 0); return; }
    *out = r;
  }
}

/* Makes the system memory twin of a target. */
static HRESULT res_sys(D3dRes *r) {
  if (r->sys) return D3D_OK;
  return D3D_CALL(r->owner->dev, CreateOffscreenPlainSurface, r->width, r->height, r->format, D3DPOOL_SYSTEMMEM, &r->sys, NULL);
}

/* Sends a rectangle of a target's CPU pixels to its Direct3D 9 copy: one colour as a Clear, the rest through the twin and UpdateSurface. */
static HRESULT target_upload(Surface *s, D3dRes *r, const RECT *rect) {
  D3dDD *d = r->owner;
  RECT rc;
  surface_rect(s, rect, &rc);
  if (rect_empty(&rc)) return D3D_OK;
  DdImage im;
  uint32_t palette[256];
  surface_image(s, &im, palette);
  uint32_t raw;
  if (!dd_format_is_palette(s->fmt) && image_uniform(&im, &rc, &raw)) {
    const uint32_t argb = dd_raw_to_argb(&im, raw);
    D3DRECT dr;
    dr.x1 = rc.left; dr.y1 = rc.top; dr.x2 = rc.right; dr.y2 = rc.bottom;
    IDirect3DSurface9 *old = NULL;
    D3D_CALL(d->dev, GetRenderTarget, 0, &old);
    D3D_CALL(d->dev, SetRenderTarget, 0, r->surface);
    const HRESULT hr = D3D_CALL(d->dev, Clear, 1, (const D3DRECT *)&dr, D9_CLEAR_TARGET, argb, 0.0f, 0);
    if (old) { D3D_CALL(d->dev, SetRenderTarget, 0, old); D3D_CALL(old, Release); }
    d->bound_target = NULL;      /* (the viewport went with SetRenderTarget: the device puts its own back) */
    if (d->active) d->active->dirty.viewport = TRUE;
    return hr;
  }
  HRESULT hr = res_sys(r);
  if (FAILED(hr)) return hr;
  D3DLOCKED_RECT lr;
  hr = D3D_CALL(r->sys, LockRect, &lr, &rc, 0);
  if (FAILED(hr)) return hr;
  const uint32_t w = (uint32_t)(rc.right - rc.left), h = (uint32_t)(rc.bottom - rc.top);
  const DdRect dr = {rc.left, rc.top, rc.right, rc.bottom};
  if (s->fmt == DD_FMT_X8R8G8B8 || s->fmt == DD_FMT_A8R8G8B8) {
    for (uint32_t y = 0; y < h; ++y) memcpy((BYTE *)lr.pBits + (SIZE_T)y * lr.Pitch, im.data + (SIZE_T)(rc.top + y) * im.pitch + (SIZE_T)rc.left * 4, (SIZE_T)w * 4);
  } else {
    dd_to_argb(&im, &dr, lr.pBits, (size_t)lr.Pitch * (h - 1) + (size_t)w * 4, (uint32_t)lr.Pitch, 0, 0, 0);
  }
  D3D_CALL(r->sys, UnlockRect);
  const POINT at = {rc.left, rc.top};
  return D3D_CALL(d->dev, UpdateSurface, r->sys, &rc, r->surface, &at);
}

/* Brings a target's Direct3D 9 pixels into the surface's memory. */
static HRESULT target_download(Surface *s, D3dRes *r) {
  D3dDD *d = r->owner;
  HRESULT hr = res_sys(r);
  if (FAILED(hr)) return hr;
  hr = D3D_CALL(d->dev, GetRenderTargetData, r->surface, r->sys);
  if (FAILED(hr)) return hr;
  D3DLOCKED_RECT lr;
  hr = D3D_CALL(r->sys, LockRect, &lr, NULL, D9_LOCK_READONLY);
  if (FAILED(hr)) return hr;
  DdImage im;
  uint32_t palette[256];
  surface_image(s, &im, palette);
  const uint32_t w = s->desc.dwWidth < r->width ? s->desc.dwWidth : r->width, h = s->desc.dwHeight < r->height ? s->desc.dwHeight : r->height;
  if (s->fmt == DD_FMT_X8R8G8B8 || s->fmt == DD_FMT_A8R8G8B8) {
    for (uint32_t y = 0; y < h; ++y) memcpy(im.data + (SIZE_T)y * im.pitch, (const BYTE *)lr.pBits + (SIZE_T)y * lr.Pitch, (SIZE_T)w * 4);
  } else if (s->fmt == DD_FMT_R5G6B5) {
    for (uint32_t y = 0; y < h; ++y) {
      const DWORD *in = (const DWORD *)((const BYTE *)lr.pBits + (SIZE_T)y * lr.Pitch);
      WORD *out = (WORD *)(im.data + (SIZE_T)y * im.pitch);
      for (uint32_t x = 0; x < w; ++x) out[x] = (WORD)(((in[x] >> 8) & 0xF800u) | ((in[x] >> 5) & 0x07E0u) | ((in[x] >> 3) & 0x001Fu));
    }
  } else if (s->fmt == DD_FMT_X1R5G5B5 || s->fmt == DD_FMT_A1R5G5B5) {
    const WORD a = s->fmt == DD_FMT_A1R5G5B5 ? 0 : 0;
    for (uint32_t y = 0; y < h; ++y) {
      const DWORD *in = (const DWORD *)((const BYTE *)lr.pBits + (SIZE_T)y * lr.Pitch);
      WORD *out = (WORD *)(im.data + (SIZE_T)y * im.pitch);
      for (uint32_t x = 0; x < w; ++x) {
        const DWORD c = in[x];
        WORD v = (WORD)(((c >> 9) & 0x7C00u) | ((c >> 6) & 0x03E0u) | ((c >> 3) & 0x001Fu));
        if (s->fmt == DD_FMT_A1R5G5B5 && (c >> 31)) v |= 0x8000u;
        out[x] = (WORD)(v | a);
      }
    }
  } else {
    for (uint32_t y = 0; y < h; ++y) {
      const DWORD *in = (const DWORD *)((const BYTE *)lr.pBits + (SIZE_T)y * lr.Pitch);
      for (uint32_t x = 0; x < w; ++x) dd_write_argb(&im, x, y, r->format == D3DFMT_X8R8G8B8 ? (in[x] | 0xFF000000u) : in[x]);
    }
  }
  D3D_CALL(r->sys, UnlockRect);
  return D3D_OK;
}

/* The depth of a depth buffer's raw value as 0 .. 1, and back (the stencil bits of the CPU copy are kept). */
static float depth_unit(UINT fmt, uint32_t raw) { return (float)((double)dd_depth_to_u32(fmt, raw) / 4294967295.0); }
static uint32_t depth_raw(UINT fmt, float z) {
  if (!(z > 0.0f)) z = 0.0f;
  if (z > 1.0f) z = 1.0f;
  return dd_depth_from_u32(fmt, (uint32_t)((double)z * 4294967295.0 + 0.5));
}

/* A depth buffer's raw value as a 24-bit depth code (d3d9.dll's lockable D24S8). */
static uint32_t depth_code24(UINT fmt, uint32_t raw) { return (uint32_t)(((uint64_t)dd_depth_to_u32(fmt, raw) * 0xFFFFFFu + 0x7FFFFFFFu) / 0xFFFFFFFFu); }

static HRESULT depth_upload(Surface *s, D3dRes *r, const RECT *rect) {
  D3dDD *d = r->owner;
  RECT rc;
  surface_rect(s, rect, &rc);
  if (rect_empty(&rc)) return D3D_OK;
  DdImage im;
  surface_image(s, &im, NULL);
  const DdFormatInfo *f = dd_format(s->fmt);
  uint32_t raw;
  if (r->format == D3DFMT_D24S8 && r->depth_lockable) {      /* (d3d9.dll's lockable D24S8: depth codes in the low 24 bits, the stencil above) */
    D3DLOCKED_RECT lr;
    HRESULT hr = D3D_CALL(r->surface, LockRect, &lr, &rc, 0);
    if (FAILED(hr)) return hr;
    for (LONG y = rc.top; y < rc.bottom; ++y) {
      DWORD *out = (DWORD *)((BYTE *)lr.pBits + (SIZE_T)(y - rc.top) * lr.Pitch);
      for (LONG x = rc.left; x < rc.right; ++x) {
        const uint32_t v = dd_get_raw(&im, (uint32_t)x, (uint32_t)y);
        uint32_t stencil = f->g ? (v & f->g) >> dd_mask_shift(f->g) : 0;
        if (stencil > 255u) stencil = 255u;
        out[x - rc.left] = depth_code24(s->fmt, v) | stencil << 24;
      }
    }
    return D3D_CALL(r->surface, UnlockRect);
  }
  if (r->format == D3DFMT_D24S8) {
    /* (a depth buffer with stencil cannot be written by the CPU in Direct3D 9: the first value of the rectangle is cleared over it) */
    raw = dd_get_raw(&im, (uint32_t)rc.left, (uint32_t)rc.top);
    D3DRECT dr;
    dr.x1 = rc.left; dr.y1 = rc.top; dr.x2 = rc.right; dr.y2 = rc.bottom;
    IDirect3DSurface9 *old = NULL, *old_rt = NULL;
    D3D_CALL(d->dev, GetDepthStencilSurface, &old);
    D3D_CALL(d->dev, GetRenderTarget, 0, &old_rt);
    D3D_CALL(d->dev, SetDepthStencilSurface, r->surface);
    /* (Direct3D 9 clears what is under the viewport of the render target: a target as large as the buffer is needed) */
    IDirect3DSurface9 *rt = NULL;
    D3DSURFACE_DESC desc;
    HRESULT hr = D3D_OK;
    if (old_rt && SUCCEEDED(D3D_CALL(old_rt, GetDesc, &desc)) && desc.Width >= r->width && desc.Height >= r->height) rt = old_rt;
    IDirect3DSurface9 *temp = NULL;
    if (!rt && SUCCEEDED(D3D_CALL(d->dev, CreateRenderTarget, r->width, r->height, D3DFMT_X8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &temp, NULL))) {
      D3D_CALL(d->dev, SetRenderTarget, 0, temp);
      rt = temp;
    }
    DWORD flags = D9_CLEAR_ZBUFFER;
    uint32_t stencil = 0;
    if (f->g) { flags |= D9_CLEAR_STENCIL; stencil = (raw & f->g) >> dd_mask_shift(f->g); }
    if (rt) hr = D3D_CALL(d->dev, Clear, 1, (const D3DRECT *)&dr, flags, 0, depth_unit(s->fmt, raw), stencil);
    if (temp) { D3D_CALL(d->dev, SetRenderTarget, 0, old_rt); D3D_CALL(temp, Release); }
    D3D_CALL(d->dev, SetDepthStencilSurface, old);
    if (old) D3D_CALL(old, Release);
    if (old_rt) D3D_CALL(old_rt, Release);
    d->bound_target = d->bound_depth = NULL;
    if (d->active) d->active->dirty.viewport = TRUE;
    return hr;
  }
  D3DLOCKED_RECT lr;
  HRESULT hr = D3D_CALL(r->surface, LockRect, &lr, &rc, 0);
  if (FAILED(hr)) return hr;
  for (LONG y = rc.top; y < rc.bottom; ++y) {
    float *out = (float *)((BYTE *)lr.pBits + (SIZE_T)(y - rc.top) * lr.Pitch);
    for (LONG x = rc.left; x < rc.right; ++x) out[x - rc.left] = depth_unit(s->fmt, dd_get_raw(&im, (uint32_t)x, (uint32_t)y));
  }
  return D3D_CALL(r->surface, UnlockRect);
}

static HRESULT depth_download(Surface *s, D3dRes *r) {
  if (r->format != D3DFMT_D32F_LOCKABLE && !r->depth_lockable) return D3D_OK;      /* (a D24S8 that d3d9.dll would not make lockable: not given back) */
  D3DLOCKED_RECT lr;
  HRESULT hr = D3D_CALL(r->surface, LockRect, &lr, NULL, D9_LOCK_READONLY);
  if (FAILED(hr)) return hr;
  DdImage im;
  surface_image(s, &im, NULL);
  const DdFormatInfo *f = dd_format(s->fmt);
  const uint32_t w = s->desc.dwWidth < r->width ? s->desc.dwWidth : r->width, h = s->desc.dwHeight < r->height ? s->desc.dwHeight : r->height;
  for (uint32_t y = 0; y < h; ++y) {
    const BYTE *in = (const BYTE *)lr.pBits + (SIZE_T)y * lr.Pitch;
    for (uint32_t x = 0; x < w; ++x) {
      const uint32_t old = dd_get_raw(&im, x, y);
      if (r->format == D3DFMT_D32F_LOCKABLE) {
        float z;
        memcpy(&z, in + (SIZE_T)x * 4, 4);
        dd_put_raw(&im, x, y, (old & ~f->r) | depth_raw(s->fmt, z));
      } else {      /* (D24S8: the depth codes and the stencil both come back) */
        uint32_t v;
        memcpy(&v, in + (SIZE_T)x * 4, 4);
        const uint32_t code = v & 0xFFFFFFu, z = code << 8 | code >> 16;      /* (24 bits widened to 32, 0xFFFFFF to 0xFFFFFFFF) */
        uint32_t raw = (old & ~(f->r | f->g)) | dd_depth_from_u32(s->fmt, z);
        if (f->g) raw |= ((v >> 24) << dd_mask_shift(f->g)) & f->g;
        dd_put_raw(&im, x, y, raw);
      }
    }
  }
  return D3D_CALL(r->surface, UnlockRect);
}

/* The Direct3D 9 target of a surface (made, and sent the CPU's pixels if they are newer). NULL on failure. */
static D3dRes *target_res(D3dDD *d, Surface *s) {
  D3dRes *r = res_of(s, d);
  if (r && r->kind != RES_TARGET && !(r->kind == RES_TEXTURE && r->target_texture)) return NULL;
  if (!r) {
    if (s->store.shadow.resource) { res_free(s->store.shadow.resource); s->store.shadow.resource = NULL; }
    r = mem_alloc(sizeof *r);
    if (!r) return NULL;
    r->kind = RES_TARGET;
    r->owner = d;
    r->width = s->desc.dwWidth;
    r->height = s->desc.dwHeight;
    r->format = target_format(s->fmt);
    r->levels = 1;
    HRESULT hr;
    if (s->desc.ddsCaps.dwCaps & DDSCAPS_TEXTURE) {
      r->kind = RES_TEXTURE;
      r->target_texture = TRUE;
      r->has_alpha = dd_format(s->fmt)->a != 0;
      hr = D3D_CALL(d->dev, CreateTexture, r->width, r->height, 1, D9_USAGE_RENDERTARGET, r->format, D3DPOOL_DEFAULT, &r->texture, NULL);
      if (SUCCEEDED(hr)) hr = D3D_CALL(r->texture, GetSurfaceLevel, 0, &r->surface);
    } else {
      hr = D3D_CALL(d->dev, CreateRenderTarget, r->width, r->height, r->format, D3DMULTISAMPLE_NONE, 0, FALSE, &r->surface, NULL);
    }
    if (FAILED(hr)) { res_free(r); return NULL; }
    s->store.shadow.resource = r;
    s->store.shadow.state = DD_SHADOW_CPU_NEWER;
    ++d->tex_stamp;
  }
  if (s->store.shadow.state & DD_SHADOW_CPU_NEWER) {
    target_upload(s, r, NULL);
    s->store.shadow.state &= ~DD_SHADOW_CPU_NEWER;
  }
  return r;
}

static D3dRes *depth_res(D3dDD *d, Surface *s) {
  D3dRes *r = res_of(s, d);
  if (r && r->kind != RES_DEPTH) return NULL;
  if (!r) {
    if (s->store.shadow.resource) { res_free(s->store.shadow.resource); s->store.shadow.resource = NULL; }
    r = mem_alloc(sizeof *r);
    if (!r) return NULL;
    r->kind = RES_DEPTH;
    r->owner = d;
    r->width = s->desc.dwWidth;
    r->height = s->desc.dwHeight;
    r->levels = 1;
    r->format = dd_format(s->fmt)->g ? D3DFMT_D24S8 : D3DFMT_D32F_LOCKABLE;
    /* (with stencil: a D24S8 that d3d9.dll makes lockable, so that Lock reads what was drawn - its private TcosD3D9CreateLockableDepthStencil (TCOS_USAGE_LOCKABLE_DEPTH),
     * on its own device only; a plain D24S8, which Lock does not read back, on another Direct3D 9 or if that is refused) */
    if (r->format == D3DFMT_D24S8 && d->create_lockable_depth)
      r->depth_lockable = SUCCEEDED(d->create_lockable_depth(d->dev, r->width, r->height, r->format, D3DMULTISAMPLE_NONE, 0, FALSE, &r->surface));
    if (!r->depth_lockable && FAILED(D3D_CALL(d->dev, CreateDepthStencilSurface, r->width, r->height, r->format, D3DMULTISAMPLE_NONE, 0, FALSE, &r->surface, NULL))) {
      res_free(r);
      return NULL;
    }
    s->store.shadow.resource = r;
    s->store.shadow.state = DD_SHADOW_CPU_NEWER;
  }
  if (s->store.shadow.state & DD_SHADOW_CPU_NEWER) {
    depth_upload(s, r, NULL);
    s->store.shadow.state &= ~DD_SHADOW_CPU_NEWER;
  }
  return r;
}

/* The surfaces of a texture: level `level` of face `face` (NULL if the texture has none). */
static Surface *texture_level(Surface *root, UINT face, UINT level) {
  if (!face && !level) return root;
  SurfaceGroup *g = root->group;
  for (UINT i = 0; g && i < g->count; ++i) {
    Surface *m = g->members[i];
    if (m->face == face && m->level == root->level + level && (m->desc.ddsCaps.dwCaps & DDSCAPS_TEXTURE)) return m;
  }
  return NULL;
}
static UINT texture_levels(const Surface *root) {
  if ((root->desc.ddsCaps.dwCaps & DDSCAPS_MIPMAP) && (root->desc.dwFlags & DDSD_MIPMAPCOUNT) && root->desc.dwMipMapCount) return root->desc.dwMipMapCount;
  return 1;
}
static BOOL texture_is_cube(const Surface *root) { return (root->desc.ddsCaps.dwCaps2 & DDSCAPS2_CUBEMAP) != 0; }

/* The Direct3D 9 format of a bump map's (the signed formats, whose pixels go to d3d9.dll as they are: their layouts are the same), else 0. */
static D3DFORMAT bump_format(UINT fmt) {
  switch (fmt) {
    case DD_FMT_V8U8: return D3DFMT_V8U8;
    case DD_FMT_L6V5U5: return D3DFMT_L6V5U5;
    case DD_FMT_X8L8V8U8: return D3DFMT_X8L8V8U8;
    case DD_FMT_Q8W8V8U8: return D3DFMT_Q8W8V8U8;
    case DD_FMT_V16U16: return D3DFMT_V16U16;
    default: return (D3DFORMAT)0;
  }
}

/* The bytes of a pixel of a Direct3D 9 bump format; 0 for the others. */
static UINT bump_bytes(D3DFORMAT f) {
  return f == D3DFMT_V8U8 || f == D3DFMT_L6V5U5 ? 2u : f == D3DFMT_X8L8V8U8 || f == D3DFMT_Q8W8V8U8 || f == D3DFMT_V16U16 ? 4u : 0u;
}

/* Sends one level of a texture as A8R8G8B8 (the colour key as alpha 0 when `keyed`), or a bump map's pixels as they are. */
static HRESULT texture_send_level(D3dRes *r, Surface *level_surface, UINT face, UINT level, BOOL keyed, DWORD key_low, DWORD key_high) {
  D3DLOCKED_RECT lr;
  HRESULT hr;
  if (r->target_texture) {
    if (FAILED(hr = res_sys(r))) return hr;
    hr = D3D_CALL(r->sys, LockRect, &lr, NULL, 0);
  } else if (r->cube) {
    hr = D3D_CALL(r->cube, LockRect, (D3DCUBEMAP_FACES)face, level, &lr, NULL, 0);
  } else {
    hr = D3D_CALL(r->texture, LockRect, level, &lr, NULL, 0);
  }
  if (FAILED(hr)) return hr;
  const UINT w = r->width >> level ? r->width >> level : 1, h = r->height >> level ? r->height >> level : 1;
  DdImage im;
  uint32_t palette[256];
  if (level_surface) surface_image(level_surface, &im, palette);
  const BOOL readable = level_surface && dd_format_has_colour(level_surface->fmt) && level_surface->desc.dwWidth == w && level_surface->desc.dwHeight == h;
  const UINT bump = bump_bytes(r->format);
  if (bump && level_surface && bump_format(level_surface->fmt) == r->format && level_surface->desc.dwWidth == w && level_surface->desc.dwHeight == h && im.data &&
      im.pitch >= w * bump && (size_t)im.pitch * (h - 1) + (size_t)w * bump <= im.size) {
    for (UINT y = 0; y < h; ++y) memcpy((BYTE *)lr.pBits + (SIZE_T)y * lr.Pitch, im.data + (SIZE_T)y * im.pitch, (SIZE_T)w * bump);
  } else if (bump) {
    for (UINT y = 0; y < h; ++y) memset((BYTE *)lr.pBits + (SIZE_T)y * lr.Pitch, 0, (SIZE_T)w * bump);      /* (a level that is not there) */
  } else if (readable) {
    const DdRect dr = {0, 0, (LONG)w, (LONG)h};
    dd_to_argb(&im, &dr, lr.pBits, (size_t)lr.Pitch * (h - 1) + (size_t)w * 4, (uint32_t)lr.Pitch, keyed, key_low, key_high);
    if (r->format == D3DFMT_X8R8G8B8)
      for (UINT y = 0; y < h; ++y) {
        DWORD *row = (DWORD *)((BYTE *)lr.pBits + (SIZE_T)y * lr.Pitch);
        for (UINT x = 0; x < w; ++x) row[x] |= 0xFF000000u;
      }
  } else {
    /* (depth formats have no colours) */
    for (UINT y = 0; y < h; ++y) memset((BYTE *)lr.pBits + (SIZE_T)y * lr.Pitch, 0, (SIZE_T)w * 4);
  }
  if (r->target_texture) {
    D3D_CALL(r->sys, UnlockRect);
    return D3D_CALL(r->owner->dev, UpdateSurface, r->sys, NULL, r->surface, NULL);
  }
  if (r->cube) return D3D_CALL(r->cube, UnlockRect, (D3DCUBEMAP_FACES)face, level);
  return D3D_CALL(r->texture, UnlockRect, level);
}

/* The Direct3D 9 texture of a texture surface, up to date, keyed as `keyed` asks. NULL if it cannot be made. */
static D3dRes *texture_res(D3dDD *d, Surface *root, BOOL want_key) {
  const BOOL keyed = want_key && (root->desc.dwFlags & DDSD_CKSRCBLT);
  const DWORD key_low = root->desc.ddckCKSrcBlt.dwColorSpaceLowValue, key_high = root->desc.ddckCKSrcBlt.dwColorSpaceHighValue;
  D3dRes *r = res_of(root, d);
  if (r && r->kind == RES_TEXTURE && r->target_texture) {
    if (root->store.shadow.state & DD_SHADOW_CPU_NEWER) {
      texture_send_level(r, root, 0, 0, FALSE, 0, 0);
      root->store.shadow.state &= ~DD_SHADOW_CPU_NEWER;
    }
    return r;
  }
  if (r && r->kind != RES_TEXTURE) {
    if (r->kind != RES_TARGET) return NULL;
    /* (an off-screen target sampled as a texture would need a texture of its own: Direct3D 7 refuses textures that are not DDSCAPS_TEXTURE) */
    return NULL;
  }
  const BOOL cube = texture_is_cube(root);
  const UINT levels = texture_levels(root);
  const D3DFORMAT format = bump_format(root->fmt) ? bump_format(root->fmt)
                           : (keyed || format_has_alpha(root->fmt) || dd_format_is_blocks(root->fmt)) ? D3DFMT_A8R8G8B8 : D3DFMT_X8R8G8B8;
  BOOL all = FALSE;
  if (r && (r->format != format || r->levels != levels)) {
    res_free(r);
    root->store.shadow.resource = r = NULL;
  }
  if (!r) {
    if (root->store.shadow.resource) { res_free(root->store.shadow.resource); root->store.shadow.resource = NULL; }
    if (root->desc.ddsCaps.dwCaps & DDSCAPS_3DDEVICE) {
      r = target_res(d, root);
      return r;
    }
    r = mem_alloc(sizeof *r);
    if (!r) return NULL;
    r->kind = RES_TEXTURE;
    r->owner = d;
    r->width = root->desc.dwWidth;
    r->height = root->desc.dwHeight;
    r->levels = levels;
    r->format = format;
    HRESULT hr = cube ? D3D_CALL(d->dev, CreateCubeTexture, r->width, levels, 0, format, D3DPOOL_MANAGED, &r->cube, NULL)
                      : D3D_CALL(d->dev, CreateTexture, r->width, r->height, levels, 0, format, D3DPOOL_MANAGED, &r->texture, NULL);
    if (FAILED(hr)) { res_free(r); return NULL; }
    root->store.shadow.resource = r;
    ++d->tex_stamp;
    all = TRUE;
  }
  if (r->keyed != keyed || (keyed && (r->key_low != key_low || r->key_high != key_high))) all = TRUE;
  r->keyed = keyed;
  r->key_low = key_low;
  r->key_high = key_high;
  /* (a colour key is not alpha: MODULATE of a keyed 5-6-5 texture takes the vertex's alpha - the key works by itself, TCOS_TSS_KEY_KILL; Wine's
   * test_texturemapblend) */
  r->has_alpha = format == D3DFMT_A8R8G8B8 && (format_has_alpha(root->fmt) || dd_format_is_blocks(root->fmt));
  const UINT faces = cube ? 6 : 1;
  for (UINT face = 0; face < faces; ++face)
    for (UINT level = 0; level < levels; ++level) {
      Surface *ls = texture_level(root, face, level);
      if (!all && (!ls || !(ls->store.shadow.state & DD_SHADOW_CPU_NEWER))) continue;
      texture_send_level(r, ls, face, level, keyed, key_low, key_high);
      if (ls) ls->store.shadow.state &= ~DD_SHADOW_CPU_NEWER;
    }
  if (r->texture && r->lod != root->lod) {
    D3D_CALL(r->texture, SetLOD, root->lod);
    r->lod = root->lod;
  }
  return r;
}

/* The root of a texture a surface is part of (a level or a face names its whole texture). */
static Surface *texture_root(Surface *s) {
  if (!s->level && !s->face) return s;
  SurfaceGroup *g = s->group;
  if (g && g->root && (g->root->desc.ddsCaps.dwCaps & DDSCAPS_TEXTURE)) return g->root;
  return s;
}

/* ---- the hooks -------------------------------------------------------------------------------------------------------------------------------------- */

static void hook_pull(Surface *s, const RECT *rect) {
  (void)rect;
  D3dRes *r = s->store.shadow.resource;
  s->store.shadow.state &= ~DD_SHADOW_GPU_NEWER;
  if (!r || !r->owner || !r->owner->dev) return;
  const WORD cw = fpu_get();
  HRESULT hr = D3D_OK;
  if (r->kind == RES_DEPTH) hr = depth_download(s, r);
  else if (r->surface && (r->kind == RES_TARGET || r->target_texture)) hr = target_download(s, r);
  if (FAILED(hr)) { static volatile LONG said; dx_failed(&said, "ddraw: reading back a Direct3D target", hr); }
  fpu_set(cw);
}

static void hook_cpu_wrote(Surface *s, const RECT *rect) {
  D3dRes *r = s->store.shadow.resource;
  if (!r || !r->owner || !r->owner->dev) return;
  /* (a target or depth buffer the CPU wrote a part of: that part goes now, so the flag can stay a whole-surface one; a texture waits for its next use) */
  if (r->kind == RES_TEXTURE && !r->target_texture) return;
  const WORD cw = fpu_get();
  if (r->kind == RES_DEPTH) depth_upload(s, r, rect);
  else if (r->kind == RES_TEXTURE) texture_send_level(r, s, 0, 0, FALSE, 0, 0);
  else target_upload(s, r, rect);
  s->store.shadow.state &= ~DD_SHADOW_CPU_NEWER;
  fpu_set(cw);
}

static void d3_device_surface_gone(D3dDevice *dev, Surface *s);
static void hook_surface_destroyed(Surface *s) {
  D3dSurf *ds = s->d3d;
  if (ds) {
    if (ds->handle) handle_free(&g_handles, ds->handle, H_TEXTURE);
    D3dDevice *device1 = ds->device1;
    ds->device1 = NULL;
    if (device1) device1->vt_inner->Release((IUnknown *)&device1->vt_inner);
  }
  D3dDD *d = s->dd ? s->dd->d3d : NULL;
  if (d)
    for (D3dDevice *dev = d->devices; dev; dev = dev->next) d3_device_surface_gone(dev, s);
  D3dRes *r = s->store.shadow.resource;
  if (r) {
    if (d && (r->surface == d->bound_target || r->surface == d->bound_depth)) {
      if (d->dev && r->surface == d->bound_depth) D3D_CALL(d->dev, SetDepthStencilSurface, NULL);
      d->bound_target = d->bound_depth = NULL;
    }
    if (d) ++d->tex_stamp;
    res_free(r);
    s->store.shadow.resource = NULL;
  }
  mem_free(s->d3d);
  s->d3d = NULL;
}

static void hook_flipped(Surface **chain, UINT count) {
  (void)chain; (void)count;      /* (the shadows went round with the memory: the devices find their targets' new copies at the next draw) */
}

static void d3_device_detach(D3dDevice *dev);
static void hook_ddraw_destroyed(DDraw *dd) {
  D3dDD *d = dd->d3d;
  if (!d) return;
  /* (the surfaces of the object may live on: their copies of this device go now) */
  for (Surface *s = dd->surfaces; s; s = s->list_next) {
    D3dRes *r = s->store.shadow.resource;
    if (r && r->owner == d) {
      res_free(r);
      s->store.shadow.resource = NULL;
      s->store.shadow.state = 0;
    }
  }
  while (d->devices) d3_device_detach(d->devices);
  for (UINT i = 0; i < DECL_CACHE; ++i) if (d->decls[i].decl) D3D_CALL(d->decls[i].decl, Release);
  for (UINT i = 0; i < 4; ++i) mem_free(d->scratch[i]);
  if (d->dev) D3D_CALL(d->dev, Release);
  if (d->d9) D3D_CALL(d->d9, Release);
  if (d->window) DestroyWindow(d->window);
  mem_free(d);
  dd->d3d = NULL;
}
