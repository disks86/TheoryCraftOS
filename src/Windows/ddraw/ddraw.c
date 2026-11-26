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
/* ddraw.dll for TheoryCraftOS: DirectDraw, every interface version (docs/DirectDraw.md).
 *
 * One DirectDraw object answers IDirectDraw, IDirectDraw2, IDirectDraw4 and IDirectDraw7 (each interface with its own reference count, as Windows
 * counts them) and has room for the IDirect3D .. IDirect3D7 interfaces of Direct3D 1 .. 7, which the Direct3D part of this library fills in
 * (DdD3dHooks below). A surface answers IDirectDrawSurface .. IDirectDrawSurface7 and IDirectDrawGammaControl, and has room for IDirect3DTexture and
 * IDirect3DTexture2. Surfaces made by one CreateSurface (a flipping chain, the levels of a mipmap, the faces of a cube map) form a group that lives as
 * long as one of them has references.
 *
 * Pixels: every surface's memory is in system memory and is what counts (the "video memory" of the caps is this memory too). The blitter, the formats
 * and the conversions are in ddraw_pixels.h (no library, tested on the host: tests/DdrawPixelsTest.cpp). The primary surface is shown through GDI: in
 * the normal cooperative level it is the desktop (what is drawn into it goes to the screen, clipped by a clipper's window), in exclusive full-screen
 * mode it and its flipping chain fill the device window, which covers the screen. A display mode of 8 or 16 bits is shown through the primary's palette
 * or converted (the kernel scans out 32 bits; present.inc).
 *
 * Files: ddraw.c (objects, lock, hooks, exports, class factory), formats.inc (DDPIXELFORMAT and the formats of ddraw_pixels.h, surface descriptions),
 * display.inc (display modes, cooperative levels, the focus window, vertical blank, caps), ddobj.inc (the IDirectDraw methods), surface.inc (surfaces:
 * creation, references, attachments, Lock, GetDC, colour keys, private data ...), blit.inc (Blt, BltFast, BltBatch, Flip, clippers in blits), present.inc
 * (the primary surface on the screen), palclip.inc (palettes, clippers), vtbl.inc (the vtables of every interface version), legacy.inc (the exports
 * Windows' ddraw.dll has for its own components). */
#define INITGUID
#define COBJMACROS
#define CINTERFACE
#define WIN32_LEAN_AND_MEAN
#include "../dxcommon.h"
#include <stddef.h>
#include <ddraw.h>
#include <d3d.h>
#include "ddraw_pixels.h"

#pragma GCC diagnostic ignored "-Wunused-function"

__asm__(".text\n.globl ___chkstk_ms\n___chkstk_ms:\n ret\n");

/* (values of the DirectX SDK that mingw's ddraw.h has not) */
#ifndef DDSMT_ISTESTREQUIRED
#define DDSMT_ISTESTREQUIRED 0x00000001u
#endif
#ifndef DDEM_MODEPASSED
#define DDEM_MODEPASSED 0x00000001u
#endif
#ifndef DDEM_MODEFAILED
#define DDEM_MODEFAILED 0x00000002u
#endif

/* ---- the objects ----------------------------------------------------------------------------------------------------------------------------------- */

typedef struct DDraw DDraw;
typedef struct Surface Surface;
typedef struct SurfaceGroup SurfaceGroup;
typedef struct Palette Palette;
typedef struct Clipper Clipper;

typedef struct Private {      /* SetPrivateData: a copy of the bytes, or an interface it holds a reference of */
  GUID guid;
  DWORD size;
  BOOL object;
  void *data;
  struct Private *next;
} Private;

/* The copy of a surface's pixels the Direct3D part keeps in a Direct3D 9 resource. The CPU memory of the surface is what counts; `state` says which
 * of the two is newer (DD_SHADOW_*). The Direct3D part owns `resource` (opaque here) and frees it in DdD3dHooks.surface_destroyed. */
#define DD_SHADOW_CPU_NEWER 0x1u      /* the CPU wrote the surface since the shadow was filled: the shadow must be updated before Direct3D uses it */
#define DD_SHADOW_GPU_NEWER 0x2u      /* Direct3D drew into the shadow: the CPU memory must be brought up to date (DdD3dHooks.pull) before it is read */
typedef struct DdShadow {
  void *resource;
  DWORD state;
} DdShadow;

/* What Flip exchanges between the surfaces of a flipping chain: the memory and the shadow that goes with it. */
typedef struct SurfaceStore {
  BYTE *mem;             /* the pixels (every plane) */
  SIZE_T size;           /* bytes of `mem` */
  BOOL own;              /* allocated here (not the program's DDSD_LPSURFACE memory) */
  LONG *shared;          /* DuplicateSurface: the surfaces that share `mem` (it is freed with the last); NULL: not shared */
  SIZE_T vidmem;         /* bytes of video memory it counts (GetAvailableVidMem) */
  DdShadow shadow;
} SurfaceStore;
static UINT64 g_vidmem_used;     /* the video memory surfaces take (under g_lock) */

struct SurfaceGroup {
  LONG ref;              /* 1 while the root has not been destroyed, and 1 for every surface of the group whose interfaces have references */
  Surface *root;
  Surface **members;     /* every surface of the group (freed with it) */
  UINT count;
  DDraw *dd;
};

#define MAX_COMPLEX 6u

struct Surface {
  /* the interfaces: each field is an interface pointer (its vtable comes first) */
  const IDirectDrawSurface7Vtbl *vt7;
  const IDirectDrawSurface4Vtbl *vt4;
  const IDirectDrawSurface3Vtbl *vt3;
  const IDirectDrawSurface2Vtbl *vt2;
  const IDirectDrawSurfaceVtbl *vt1;
  const IDirectDrawGammaControlVtbl *vt_gamma;
  const IDirect3DTexture2Vtbl *vt_tex2;   /* the Direct3D part's (DdD3dHooks.texture2_vtbl); NULL: QueryInterface refuses */
  const IDirect3DTextureVtbl *vt_tex1;
  LONG ref7, ref4, ref3, ref2, ref1, ref_gamma;
  LONG iface_count;      /* interfaces that have references */
  IUnknown *release_iface;   /* the DirectDraw interface the surface holds a reference of (made by IDirectDraw4/7; NULL for 1/2) */
  IUnknown *texture_outer;   /* the interface IDirect3DTexture(2)::AddRef/Release count on (the creation version's) */
  UINT version;          /* 7, 4 or 1 (IDirectDraw and IDirectDraw2 make version 1 surfaces) */
  DDraw *dd;
  SurfaceGroup *group;
  /* attachments */
  Surface *complex[MAX_COMPLEX];   /* the surfaces made with this one: the next back buffer, the next mipmap level, a cube map's faces */
  Surface *next_attached;    /* attached with AddAttachedSurface: a list from first_attached */
  Surface *first_attached;
  IUnknown *attached_iface;  /* the interface of this surface its attachment holds a reference of */
  DWORD attach_saved_caps;   /* caps AddAttachedSurface changed (a version 1 surface joining a flipping chain) */
  BOOL implicit;             /* made by a complex CreateSurface, not its root: lives as long as the group */
  BOOL flip_chain_start;
  BOOL lost;
  DWORD made_bpp;           /* a primary: the display depth it was made in (Restore in another depth: DDERR_WRONGMODE, though its description follows) */
  BOOL sysmem_fallback;      /* asked for no memory and got system memory (it can still be lost) */
  UINT face, level;
  /* what it is */
  DDSURFACEDESC2 desc;       /* the description GetSurfaceDesc gives (lpSurface NULL, DDSD_LPSURFACE clear) */
  UINT fmt;                  /* DD_FMT_* */
  SurfaceStore store;
  /* locks and the device context */
  UINT lock_count;
  RECT lock_rect;
  BOOL lock_whole;
  DWORD lock_flags;
  HDC dc;
  HBITMAP dib;               /* the DIB of that DC (on the surface's memory: D3DKMTCreateDCFromMemory) */
  /* what is attached */
  Palette *palette;
  Clipper *clipper;
  Private *priv;
  DWORD uniqueness;
  DWORD priority, lod;
  DDGAMMARAMP gamma;
  BOOL gamma_set;
  UINT page_locks;           /* PageLock without PageUnlock yet (system memory surfaces) */
  void *d3d;                 /* the Direct3D part's per-surface state (opaque here) */
  Surface *list_prev, *list_next;   /* the DirectDraw object's surfaces */
};

struct Palette {
  const IDirectDrawPaletteVtbl *lpVtbl;
  LONG ref;
  DWORD flags;               /* DDPCAPS_* (with DDPCAPS_PRIMARYSURFACE while it is the primary surface's) */
  UINT count;                /* 2, 4, 16 or 256 entries */
  PALETTEENTRY entries[256];
  DDraw *dd;
  IUnknown *release_iface;   /* the DirectDraw interface it holds a reference of (made by IDirectDraw4/7) */
};

struct Clipper {
  const IDirectDrawClipperVtbl *lpVtbl;
  LONG ref;
  HWND window;
  HRGN region;               /* SetClipList's (NULL: none) */
  BOOL initialized;
  RECT last_window_rect;     /* IsClipListChanged: the window's visible rectangle when it was last asked */
  BOOL last_valid;
};

enum { DD_STATE_OK = 0, DD_STATE_LOST, DD_STATE_NOT_RESTORED };

#define DD_INITIALIZED   0x01u
#define DD_RESTORE_MODE  0x02u      /* SetDisplayMode changed the mode: RestoreDisplayMode (and the release) put the registry's back */
#define DD_SCL_DDRAW1    0x04u      /* SetCooperativeLevel of IDirectDraw: leaving exclusive mode keeps the display mode */
#define DD_SCL_RECURSIVE 0x08u
#define DD_DEAD          0x10u      /* every interface released: what is left lives until its last surface, palette and clipper go */

struct DDraw {
  const IDirectDraw7Vtbl *vt7;
  const IDirectDraw4Vtbl *vt4;
  const IDirectDraw2Vtbl *vt2;
  const IDirectDrawVtbl *vt1;
  /* Direct3D's interfaces on the DirectDraw object (DdD3dHooks: the Direct3D part fills them in when the object is made) */
  const IDirect3D7Vtbl *vt_d3d7;
  const IDirect3D3Vtbl *vt_d3d3;
  const IDirect3D2Vtbl *vt_d3d2;
  const IDirect3DVtbl *vt_d3d1;
  LONG ref7, ref4, ref2, ref1, ifaces;
  LONG keep;                 /* surface groups and palettes that still point here: the memory is freed when this and `ifaces` are 0 */
  DWORD flags;
  UINT kind;                 /* DD_DRIVER_*: the driver asked for (DDCREATE_HARDWAREONLY, DDCREATE_EMULATIONONLY) */
  LONG device_state;         /* DD_STATE_* */
  DWORD coop;                /* the cooperative level (DDSCL_*) */
  HWND focus_window, device_window, dest_window;
  HWND hooked_window;        /* the window whose procedure ddraw watches for this object (exclusive mode: display.inc's table of hooked windows) */
  Surface *primary;
  RECT primary_lock;
  Surface *surfaces;         /* list of every surface (list_next) */
  int d3d_version;           /* the IDirect3D version asked for last (1, 2, 3 or 7) */
  void *d3d;                 /* the Direct3D part's state of this object (opaque here) */
  /* StartModeTest / EvaluateMode */
  SIZE *test_modes;
  DWORD test_count, test_index;
  DWORD test_start_tick;
  DDraw *next;               /* every DirectDraw object of the process */
};

/* ---- the lock --------------------------------------------------------------------------------------------------------------------------------------
 * One lock for all of DirectDraw (as Windows' ddraw.dll has: AcquireDDThreadLock takes it). A program may call back into DirectDraw from the
 * callbacks of the Enum* methods: the lock is a critical section, which a thread can take again. */
static CRITICAL_SECTION g_lock;
static void dd_lock(void) { EnterCriticalSection(&g_lock); }
static void dd_unlock(void) { LeaveCriticalSection(&g_lock); }

static DDraw *g_ddraws;            /* every DirectDraw object (under g_lock) */
static DDraw *g_exclusive;         /* the object in exclusive mode, if one is */
static HWND g_exclusive_window;
static HINSTANCE g_module;
static LONG g_objects;             /* objects alive (DllCanUnloadNow) */
static DWORD g_uniqueness = 1;     /* the next uniqueness value of a surface */

/* ---- the hooks of the Direct3D part ----------------------------------------------------------------------------------------------------------------
 * The Direct3D interfaces (IDirect3D .. IDirect3D7, devices, viewports, materials, lights, execute buffers, vertex buffers, textures) are written
 * beside this code in the same library. They plug in by filling g_d3d (dd_d3d_register, from DllMain before anything is made); every field is
 * optional and left NULL nothing of Direct3D is answered (QueryInterface says E_NOINTERFACE). See docs/DirectDraw.md. */
typedef struct DdD3dHooks {
  /* the vtables of the interfaces a DirectDraw object carries (set on every new object), and those of a surface's texture interfaces */
  const IDirect3D7Vtbl *d3d7_vtbl;
  const IDirect3D3Vtbl *d3d3_vtbl;
  const IDirect3D2Vtbl *d3d2_vtbl;
  const IDirect3DVtbl *d3d1_vtbl;
  const IDirect3DTexture2Vtbl *texture2_vtbl;
  const IDirect3DTextureVtbl *texture1_vtbl;
  /* QueryInterface of a DirectDraw object for an interface DirectDraw does not know (IID_IDirect3D* are answered here from the vtables above):
   * S_OK with *out set and a reference taken, or E_NOINTERFACE */
  HRESULT (*ddraw_query)(DDraw *dd, REFIID riid, void **out);
  /* ... of a surface: a Direct3D device by its GUID (IID_IDirect3DHALDevice, IID_IDirect3DRGBDevice ... on version 1-4 surfaces) */
  HRESULT (*surface_query)(Surface *s, REFIID riid, void **out);
  /* The CPU is about to read or write `rect` of the surface (NULL: all of it): if the shadow is newer (DD_SHADOW_GPU_NEWER), copy it into
   * s->store.mem and clear the flag. Called before Lock, GetDC, Blt/BltFast (source and destination), Flip and the presentation of the primary. */
  void (*pull)(Surface *s, const RECT *rect);
  /* The CPU wrote `rect` of the surface (NULL: all): the shadow is stale (DD_SHADOW_CPU_NEWER is set before this is called). Called after Unlock of a
   * writing lock, ReleaseDC, Blt/BltFast/colour and depth fills into it, SetSurfaceDesc, Restore, SetColorKey, SetPalette, a palette's SetEntries and SetLOD
   * (Flip moves the shadows with the memory: `flipped` below). */
  void (*cpu_wrote)(Surface *s, const RECT *rect);
  /* A surface is being freed (its store.shadow.resource and s->d3d are the Direct3D part's to release); and a DirectDraw object (dd->d3d). */
  void (*surface_destroyed)(Surface *s);
  void (*ddraw_destroyed)(DDraw *dd);
  /* The surfaces of a flipping chain exchanged their stores (Flip): `count` surfaces from the front buffer on, in the chain's order. */
  void (*flipped)(Surface **chain, UINT count);
} DdD3dHooks;
static DdD3dHooks g_d3d;

/* The Direct3D part's registration (d3d_vtbl.inc). */
void dd_d3d_register(DdD3dHooks *hooks);

/* ---- small helpers --------------------------------------------------------------------------------------------------------------------------------- */

#define DD_IMPL(type, field, iface) ((type *)((BYTE *)(iface) - offsetof(type, field)))
#define DD7(i) DD_IMPL(DDraw, vt7, i)
#define DD4(i) DD_IMPL(DDraw, vt4, i)
#define DD2(i) DD_IMPL(DDraw, vt2, i)
#define DD1(i) DD_IMPL(DDraw, vt1, i)
#define S7(i) DD_IMPL(Surface, vt7, i)
#define S4(i) DD_IMPL(Surface, vt4, i)
#define S3(i) DD_IMPL(Surface, vt3, i)
#define S2(i) DD_IMPL(Surface, vt2, i)
#define S1(i) DD_IMPL(Surface, vt1, i)

#define IFACE7(s) ((IDirectDrawSurface7 *)&(s)->vt7)
#define IFACE4(s) ((IDirectDrawSurface4 *)&(s)->vt4)
#define IFACE3(s) ((IDirectDrawSurface3 *)&(s)->vt3)
#define IFACE2(s) ((IDirectDrawSurface2 *)&(s)->vt2)
#define IFACE1(s) ((IDirectDrawSurface *)&(s)->vt1)
#define DDI7(d) ((IDirectDraw7 *)&(d)->vt7)
#define DDI4(d) ((IDirectDraw4 *)&(d)->vt4)
#define DDI2(d) ((IDirectDraw2 *)&(d)->vt2)
#define DDI1(d) ((IDirectDraw *)&(d)->vt1)

static void zero(void *p, SIZE_T n) { memset(p, 0, n); }
static void rect_set(RECT *r, LONG l, LONG t, LONG ri, LONG b) { r->left = l; r->top = t; r->right = ri; r->bottom = b; }
static BOOL rect_empty(const RECT *r) { return r->left >= r->right || r->top >= r->bottom; }
static BOOL rect_intersect(RECT *out, const RECT *a, const RECT *b) {
  out->left = a->left > b->left ? a->left : b->left;
  out->top = a->top > b->top ? a->top : b->top;
  out->right = a->right < b->right ? a->right : b->right;
  out->bottom = a->bottom < b->bottom ? a->bottom : b->bottom;
  if (rect_empty(out)) { rect_set(out, 0, 0, 0, 0); return FALSE; }
  return TRUE;
}
static void rect_union(RECT *acc, const RECT *r) {
  if (rect_empty(r)) return;
  if (rect_empty(acc)) { *acc = *r; return; }
  if (r->left < acc->left) acc->left = r->left;
  if (r->top < acc->top) acc->top = r->top;
  if (r->right > acc->right) acc->right = r->right;
  if (r->bottom > acc->bottom) acc->bottom = r->bottom;
}
/* Copies at most `out_size` bytes of `in` (a structure whose first DWORD is its size) the way DirectDraw copies versioned structures: the smaller of
 * the two sizes, the size field of the destination kept. */
static void copy_by_size(void *out, DWORD out_size, const void *in, DWORD in_size) {
  memcpy(out, in, out_size < in_size ? out_size : in_size);
  *(DWORD *)out = out_size;
}

/* The Direct3D part's view of the CPU memory (see DdD3dHooks). */
static void cpu_access(Surface *s, const RECT *r) {
  if (g_d3d.pull && (s->store.shadow.state & DD_SHADOW_GPU_NEWER)) g_d3d.pull(s, r);
}
static void cpu_wrote(Surface *s, const RECT *r) {
  s->store.shadow.state |= DD_SHADOW_CPU_NEWER;
  s->uniqueness = g_uniqueness++;
  if (!g_uniqueness) g_uniqueness = 1;
  if (g_d3d.cpu_wrote) g_d3d.cpu_wrote(s, r);
}

#ifndef DDSGR_CALIBRATE
#define DDSGR_CALIBRATE 0x00000001u
#endif
#ifndef SELFREG_E_CLASS
#define SELFREG_E_CLASS ((HRESULT)0x80040201u)
#endif

static void ddraw_free_if_unused(DDraw *dd);
static BOOL driver_guid_ok(const GUID *guid);
/* What a DirectDraw object of a driver GUID is: the display's hardware and its software emulation (NULL, the display's GUID), the hardware only
 * (DDCREATE_HARDWAREONLY: no HEL), or the emulation only (DDCREATE_EMULATIONONLY: no HAL, every surface in system memory). */
enum { DD_DRIVER_BOTH = 0, DD_DRIVER_HARDWARE, DD_DRIVER_EMULATION };
static UINT driver_kind(const GUID *guid) {
  if (guid == (const GUID *)DDCREATE_EMULATIONONLY) return DD_DRIVER_EMULATION;
  if (guid == (const GUID *)DDCREATE_HARDWAREONLY) return DD_DRIVER_HARDWARE;
  return DD_DRIVER_BOTH;
}

#include "formats.inc"
#include "present.inc"
#include "display.inc"
#include "palclip.inc"
#include "surface.inc"
#include "blit.inc"
#include "ddobj.inc"
#include "vtbl.inc"
#include "legacy.inc"
/* the Direct3D part (Direct3D 1, 2, 3 and 7 on d3d9.dll: d3d.inc) */
#include "d3d.inc"
#include "d3d_state.inc"
#include "d3d_draw.inc"
#include "d3d_device.inc"
#include "d3d_methods.inc"
#include "d3d_objects.inc"
#include "d3d_vtbl.inc"

/* ---- creation -------------------------------------------------------------------------------------------------------------------------------------- */

/* A new DirectDraw object with one reference on IDirectDraw7 (DirectDrawCreate* turn it into the interface asked for). `initialized`: made by
 * DirectDrawCreate(Ex) (CoCreateInstance makes one that waits for Initialize). */
static HRESULT ddraw_create(DDraw **out, BOOL initialized) {
  DDraw *dd = mem_alloc(sizeof *dd);
  if (!dd) return DDERR_OUTOFMEMORY;
  dd->vt7 = &dd7_vtbl;
  dd->vt4 = &dd4_vtbl;
  dd->vt2 = &dd2_vtbl;
  dd->vt1 = &dd1_vtbl;
  dd->vt_d3d7 = g_d3d.d3d7_vtbl;
  dd->vt_d3d3 = g_d3d.d3d3_vtbl;
  dd->vt_d3d2 = g_d3d.d3d2_vtbl;
  dd->vt_d3d1 = g_d3d.d3d1_vtbl;
  dd->ref7 = 1;
  dd->ifaces = 1;
  dd->flags = initialized ? DD_INITIALIZED : 0;
  dd->device_state = DD_STATE_OK;
  dd_lock();
  dd->next = g_ddraws;
  g_ddraws = dd;
  dd_unlock();
  InterlockedIncrement(&g_objects);
  *out = dd;
  return DD_OK;
}

/* The memory of a DirectDraw object goes when its interfaces and everything that points to it are gone. */
static void ddraw_free_if_unused(DDraw *dd) {
  if (!(dd->flags & DD_DEAD) || dd->keep > 0) return;
  dd_lock();
  for (DDraw **p = &g_ddraws; *p; p = &(*p)->next)
    if (*p == dd) { *p = dd->next; break; }
  dd_unlock();
  mem_free(dd->test_modes);
  mem_free(dd);
  InterlockedDecrement(&g_objects);
}

/* The DirectDraw object of a driver GUID (NULL, DDCREATE_EMULATIONONLY, DDCREATE_HARDWAREONLY or the display's own GUID). */
static const GUID kDisplayGuid = {0x67685559, 0x3106, 0x11d0, {0xb9, 0x71, 0x00, 0xaa, 0x00, 0x34, 0x2f, 0x9f}};
static BOOL driver_guid_ok(const GUID *guid) {
  if (!guid || guid == (const GUID *)DDCREATE_EMULATIONONLY || guid == (const GUID *)DDCREATE_HARDWAREONLY) return TRUE;
  return guid_eq(guid, &kDisplayGuid);
}

EXPORT HRESULT WINAPI DirectDrawCreate(GUID *driver, IDirectDraw **out, IUnknown *outer) {
  if (!out) return DDERR_INVALIDPARAMS;
  *out = NULL;
  if (outer) return CLASS_E_NOAGGREGATION;
  if (!driver_guid_ok(driver)) return DDERR_INVALIDDIRECTDRAWGUID;
  DDraw *dd;
  HRESULT hr = ddraw_create(&dd, TRUE); if (SUCCEEDED(hr)) dd->kind = driver_kind(driver);
  if (FAILED(hr)) return hr;
  /* (the object's one reference moves to IDirectDraw) */
  dd->ref7 = 0;
  dd->ref1 = 1;
  *out = DDI1(dd);
  return DD_OK;
}

EXPORT HRESULT WINAPI DirectDrawCreateEx(GUID *driver, void **out, REFIID iid, IUnknown *outer) {
  if (!out) return DDERR_INVALIDPARAMS;
  *out = NULL;
  if (!iid || !guid_eq(iid, &IID_IDirectDraw7)) return DDERR_INVALIDPARAMS;
  if (outer) return CLASS_E_NOAGGREGATION;
  if (!driver_guid_ok(driver)) return DDERR_INVALIDDIRECTDRAWGUID;
  DDraw *dd;
  HRESULT hr = ddraw_create(&dd, TRUE); if (SUCCEEDED(hr)) dd->kind = driver_kind(driver);
  if (FAILED(hr)) return hr;
  *out = DDI7(dd);
  return DD_OK;
}

EXPORT HRESULT WINAPI DirectDrawCreateClipper(DWORD flags, IDirectDrawClipper **out, IUnknown *outer) {
  if (!out) return DDERR_INVALIDPARAMS;
  if (outer) return CLASS_E_NOAGGREGATION;
  Clipper *c;
  HRESULT hr = clipper_create(flags, &c);
  if (FAILED(hr)) return hr;
  *out = (IDirectDrawClipper *)c;
  return DD_OK;
}

/* ---- enumeration ------------------------------------------------------------------------------------------------------------------------------------
 * One display device: the primary display (NULL GUID, "Primary Display Driver" on Windows; Wine says "DirectDraw HAL"), and with
 * DDENUM_ATTACHEDSECONDARYDEVICES the same display under its own GUID and monitor. The W forms are DDERR_UNSUPPORTED on Windows as well. */
static const char kDriverDescription[] = "Primary Display Driver";
static const char kDriverName[] = "display";

typedef struct { LPDDENUMCALLBACKA callback; void *context; } EnumA;
static BOOL WINAPI enum_a_thunk(GUID *guid, LPSTR description, LPSTR name, void *context, HMONITOR monitor) {
  const EnumA *e = context;
  (void)monitor;
  return e->callback(guid, description, name, e->context);
}

EXPORT HRESULT WINAPI DirectDrawEnumerateExA(LPDDENUMCALLBACKEXA callback, void *context, DWORD flags) {
  if (!callback) return DDERR_INVALIDPARAMS;
  if (flags & ~(DWORD)(DDENUM_ATTACHEDSECONDARYDEVICES | DDENUM_DETACHEDSECONDARYDEVICES | DDENUM_NONDISPLAYDEVICES)) return DDERR_INVALIDPARAMS;
  char description[sizeof kDriverDescription], name[sizeof kDriverName];
  memcpy(description, kDriverDescription, sizeof description);
  memcpy(name, kDriverName, sizeof name);
  if (!callback(NULL, description, name, context, NULL)) return DD_OK;
  if (flags & DDENUM_ATTACHEDSECONDARYDEVICES) {
    /* (the display once more by its GUID and monitor; there are no detached displays or non-display devices) */
    GUID guid = kDisplayGuid;
    char device[] = "\\\\.\\DISPLAY1";
    char adapter[] = "TheoryCraft Display Adapter";
    const POINT origin = {0, 0};
    callback(&guid, adapter, device, context, MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY));
  }
  return DD_OK;
}
EXPORT HRESULT WINAPI DirectDrawEnumerateA(LPDDENUMCALLBACKA callback, void *context) {
  if (!callback) return DDERR_INVALIDPARAMS;
  EnumA e = {callback, context};
  return DirectDrawEnumerateExA(enum_a_thunk, &e, 0);
}
/* (Windows' ddraw.dll has no Unicode enumeration: both W forms answer DDERR_UNSUPPORTED, and DirectDrawEnumerateW DDERR_INVALIDPARAMS without a
 * callback) */
EXPORT HRESULT WINAPI DirectDrawEnumerateW(LPDDENUMCALLBACKW callback, void *context) {
  (void)context;
  return callback ? DDERR_UNSUPPORTED : DDERR_INVALIDPARAMS;
}
EXPORT HRESULT WINAPI DirectDrawEnumerateExW(LPDDENUMCALLBACKEXW callback, void *context, DWORD flags) {
  (void)callback; (void)context; (void)flags;
  return DDERR_UNSUPPORTED;
}

/* ---- the class factory ------------------------------------------------------------------------------------------------------------------------------
 * CLSID_DirectDraw and CLSID_DirectDraw7 make a DirectDraw object that waits for Initialize, CLSID_DirectDrawClipper a clipper that waits for its
 * Initialize (CoCreateInstance; ole32 knows this library serves them). */
typedef struct Factory { const IClassFactoryVtbl *lpVtbl; LONG ref; int kind; } Factory;
enum { FACTORY_DDRAW = 1, FACTORY_CLIPPER };

static HRESULT STDMETHODCALLTYPE factory_QueryInterface(IClassFactory *iface, REFIID riid, void **out) {
  if (!out) return E_POINTER;
  if (guid_eq(riid, &IID_IUnknown) || guid_eq(riid, &IID_IClassFactory)) {
    IClassFactory_AddRef(iface);
    *out = iface;
    return S_OK;
  }
  *out = NULL;
  return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE factory_AddRef(IClassFactory *iface) { return (ULONG)InterlockedIncrement(&((Factory *)iface)->ref); }
static ULONG STDMETHODCALLTYPE factory_Release(IClassFactory *iface) {
  Factory *f = (Factory *)iface;
  const LONG r = InterlockedDecrement(&f->ref);
  if (!r) { mem_free(f); InterlockedDecrement(&g_objects); }
  return (ULONG)r;
}
static HRESULT STDMETHODCALLTYPE factory_CreateInstance(IClassFactory *iface, IUnknown *outer, REFIID riid, void **out) {
  Factory *f = (Factory *)iface;
  if (!out) return E_POINTER;
  *out = NULL;
  if (outer) return CLASS_E_NOAGGREGATION;
  if (f->kind == FACTORY_CLIPPER) {
    Clipper *c;
    HRESULT hr = clipper_create(0, &c);
    if (FAILED(hr)) return hr;
    c->initialized = FALSE;
    hr = IDirectDrawClipper_QueryInterface((IDirectDrawClipper *)c, riid, out);
    IDirectDrawClipper_Release((IDirectDrawClipper *)c);
    return hr;
  }
  DDraw *dd;
  HRESULT hr = ddraw_create(&dd, FALSE);
  if (FAILED(hr)) return hr;
  hr = IDirectDraw7_QueryInterface(DDI7(dd), riid, out);
  IDirectDraw7_Release(DDI7(dd));
  return hr;
}
static HRESULT STDMETHODCALLTYPE factory_LockServer(IClassFactory *iface, BOOL lock) {
  (void)iface;
  if (lock) InterlockedIncrement(&g_objects); else InterlockedDecrement(&g_objects);
  return S_OK;
}
static const IClassFactoryVtbl factory_vtbl = {factory_QueryInterface, factory_AddRef, factory_Release, factory_CreateInstance, factory_LockServer};

EXPORT HRESULT WINAPI DllGetClassObject(REFCLSID clsid, REFIID riid, void **out) {
  if (!out) return E_POINTER;
  *out = NULL;
  int kind = 0;
  if (guid_eq(clsid, &CLSID_DirectDraw) || guid_eq(clsid, &CLSID_DirectDraw7)) kind = FACTORY_DDRAW;
  else if (guid_eq(clsid, &CLSID_DirectDrawClipper)) kind = FACTORY_CLIPPER;
  else return CLASS_E_CLASSNOTAVAILABLE;
  Factory *f = mem_alloc(sizeof *f);
  if (!f) return E_OUTOFMEMORY;
  f->lpVtbl = &factory_vtbl;
  f->ref = 1;
  f->kind = kind;
  InterlockedIncrement(&g_objects);
  HRESULT hr = factory_QueryInterface((IClassFactory *)f, riid, out);
  factory_Release((IClassFactory *)f);
  return hr;
}
EXPORT HRESULT WINAPI DllCanUnloadNow(void) { return g_objects ? S_FALSE : S_OK; }

/* DllRegisterServer / DllUnregisterServer: the classes in HKEY_CLASSES_ROOT\CLSID (InprocServer32 = this library, ThreadingModel Both). */
static LONG set_class_key(const char *clsid, const char *name) {
  char key[96] = "CLSID\\";
  SIZE_T n = alen(key);
  for (SIZE_T i = 0; clsid[i] && n + 1 < sizeof key; ++i) key[n++] = clsid[i];
  key[n] = 0;
  HKEY k = NULL, server = NULL;
  LONG st = RegCreateKeyExA(HKEY_CLASSES_ROOT, key, 0, NULL, 0, KEY_WRITE, NULL, &k, NULL);
  if (st) return st;
  st = RegSetValueExA(k, NULL, 0, REG_SZ, (const BYTE *)name, (DWORD)alen(name) + 1);
  if (!st) st = RegCreateKeyExA(k, "InprocServer32", 0, NULL, 0, KEY_WRITE, NULL, &server, NULL);
  if (!st) st = RegSetValueExA(server, NULL, 0, REG_SZ, (const BYTE *)"ddraw.dll", 10);
  if (!st) st = RegSetValueExA(server, "ThreadingModel", 0, REG_SZ, (const BYTE *)"Both", 5);
  if (server) RegCloseKey(server);
  RegCloseKey(k);
  return st;
}
static const struct { const char *clsid, *name; } kClasses[] = {
  {"{D7B70EE0-4340-11CF-B063-0020AFC2CD35}", "DirectDraw Object"},
  {"{3C305196-50DB-11D3-9CFE-00C04FD930C5}", "DirectDraw 7 Object"},
  {"{593817A0-7DB3-11CF-A2DE-00AA00B93356}", "DirectDraw Clipper Object"},
};
EXPORT HRESULT WINAPI DllRegisterServer(void) {
  for (UINT i = 0; i < sizeof kClasses / sizeof kClasses[0]; ++i)
    if (set_class_key(kClasses[i].clsid, kClasses[i].name)) return SELFREG_E_CLASS;
  return S_OK;
}
EXPORT HRESULT WINAPI DllUnregisterServer(void) {
  for (UINT i = 0; i < sizeof kClasses / sizeof kClasses[0]; ++i) {
    char key[96] = "CLSID\\";
    SIZE_T n = alen(key);
    for (SIZE_T k = 0; kClasses[i].clsid[k] && n + 1 < sizeof key; ++k) key[n++] = kClasses[i].clsid[k];
    key[n] = 0;
    HKEY k = NULL;
    if (!RegOpenKeyExA(HKEY_CLASSES_ROOT, key, 0, KEY_WRITE, &k)) {
      RegDeleteKeyA(k, "InprocServer32");
      RegCloseKey(k);
    }
    RegDeleteKeyA(HKEY_CLASSES_ROOT, key);
  }
  return S_OK;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved) {
  (void)reserved;
  if (reason == DLL_PROCESS_ATTACH) {
    g_module = instance;
    InitializeCriticalSection(&g_lock);
    dd_d3d_register(&g_d3d);      /* (the Direct3D part: d3d.inc) */
  }
  return TRUE;
}
