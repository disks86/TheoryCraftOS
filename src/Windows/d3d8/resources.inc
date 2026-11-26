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

/* d3d8.dll: the resources - vertex and index buffers, textures (2D, cube, volume), surfaces, volumes - and swap chains, each a wrapper of its
 * Direct3D 9 object, and the wrapping of Direct3D 9 objects a device method hands back. Included by d3d8.c. */

#define USAGE8_MASK 0x000003FBu      /* RENDERTARGET, DEPTHSTENCIL, WRITEONLY, SOFTWAREPROCESSING, DONOTCLIP, POINTS, RTPATCHES, NPATCHES, DYNAMIC */

/* ---- what every resource has ------------------------------------------------------------------------------------------------------------------------ */

static HRESULT STDMETHODCALLTYPE res_GetDevice(Object8 *o, IDirect3DDevice8 **out) {
  if (!out) return D3DERR_INVALIDCALL;
  CALL9((IUnknown *)o->device, AddRef);
  *out = (IDirect3DDevice8 *)o->device;
  return D3D_OK;
}
static DWORD STDMETHODCALLTYPE res_SetPriority(Object8 *o, DWORD priority) {
  IDirect3DResource9 *r = (IDirect3DResource9 *)o->inner;
  return CALL9(r, SetPriority, priority);
}
static DWORD STDMETHODCALLTYPE res_GetPriority(Object8 *o) {
  IDirect3DResource9 *r = (IDirect3DResource9 *)o->inner;
  return CALL9(r, GetPriority);
}
static void STDMETHODCALLTYPE res_PreLoad(Object8 *o) {
  IDirect3DResource9 *r = (IDirect3DResource9 *)o->inner;
  CALL9(r, PreLoad);
}

/* ---- vertex and index buffers ----------------------------------------------------------------------------------------------------------------------- */

static D3DRESOURCETYPE STDMETHODCALLTYPE vb_GetType(Object8 *o) { (void)o; return D3DRTYPE_VERTEXBUFFER; }
static HRESULT STDMETHODCALLTYPE vb_Lock(Object8 *o, UINT offset, UINT size, BYTE **data, DWORD flags) {
  if (!data) return D3DERR_INVALIDCALL;
  return CALL9(INNER_VB(o), Lock, offset, size, (void **)data, flags);
}
static HRESULT STDMETHODCALLTYPE vb_Unlock(Object8 *o) { return CALL9(INNER_VB(o), Unlock); }
static HRESULT STDMETHODCALLTYPE vb_GetDesc(Object8 *o, D3DVERTEXBUFFER_DESC *desc) {
  if (!desc) return D3DERR_INVALIDCALL;
  return CALL9(INNER_VB(o), GetDesc, desc);
}
static D3DRESOURCETYPE STDMETHODCALLTYPE ib_GetType(Object8 *o) { (void)o; return D3DRTYPE_INDEXBUFFER; }
static HRESULT STDMETHODCALLTYPE ib_Lock(Object8 *o, UINT offset, UINT size, BYTE **data, DWORD flags) {
  if (!data) return D3DERR_INVALIDCALL;
  return CALL9(INNER_IB(o), Lock, offset, size, (void **)data, flags);
}
static HRESULT STDMETHODCALLTYPE ib_Unlock(Object8 *o) { return CALL9(INNER_IB(o), Unlock); }
static HRESULT STDMETHODCALLTYPE ib_GetDesc(Object8 *o, D3DINDEXBUFFER_DESC *desc) {
  if (!desc) return D3DERR_INVALIDCALL;
  return CALL9(INNER_IB(o), GetDesc, desc);
}

/* ---- textures --------------------------------------------------------------------------------------------------------------------------------------- */

static const void *texture_vtbl_of(UINT kind) { return kind == KIND_CUBE_TEXTURE ? (const void *)&cube_vtbl : kind == KIND_VOLUME_TEXTURE ? (const void *)&vol_vtbl : (const void *)&tex_vtbl; }

/* A wrapper of a Direct3D 9 texture (one reference of it is taken over); NULL without memory (the reference is released then). */
static Texture8 *texture_new(Device8 *d, UINT kind, IDirect3DBaseTexture9 *inner) {
  const UINT levels = CALL9(inner, GetLevelCount), faces = kind == KIND_CUBE_TEXTURE ? 6 : 1;
  Object8 **subs = mem_alloc((SIZE_T)(levels ? levels : 1) * faces * sizeof *subs);
  Texture8 *t = subs ? resource_new(sizeof *t, texture_vtbl_of(kind), kind, d, (IUnknown *)inner) : NULL;
  if (!t) { mem_free(subs); CALL9(inner, Release); return NULL; }
  t->levels = levels;
  t->faces = faces;
  t->subs = subs;
  return t;
}
static void texture_free_levels(Texture8 *t) {
  if (!t->subs) return;
  for (UINT i = 0; i < t->levels * t->faces; ++i) {
    Object8 *s = t->subs[i];
    if (!s) continue;
    table_remove(t->o.device, s);
    private_clear(s);
    unknown_release(s->inner);
    mem_free(s);
  }
  mem_free(t->subs);
  t->subs = NULL;
}
/* The surface (or volume) of a level of a texture, made the first time it is asked for: the same object each time, with the texture's references. */
static Object8 *texture_sub(Texture8 *t, UINT face, UINT level, HRESULT *hr) {
  *hr = D3DERR_INVALIDCALL;
  if (face >= t->faces || level >= t->levels) return NULL;
  Device8 *d = t->o.device;
  const UINT i = face * t->levels + level;
  EnterCriticalSection(&d->lock);
  Object8 *s = t->subs[i];
  if (!s) {
    if (t->o.kind == KIND_VOLUME_TEXTURE) {
      Volume8 *v = mem_alloc(sizeof *v);
      if (v) {
        obj_init(&v->o, &volume_vtbl, KIND_VOLUME, d, NULL);
        v->level = level;
        v->o.container = &t->o;
        s = &v->o;
      } else {
        *hr = E_OUTOFMEMORY;
      }
    } else {
      IDirect3DSurface9 *s9 = NULL;
      *hr = t->o.kind == KIND_CUBE_TEXTURE ? CALL9(INNER_CUBE(t), GetCubeMapSurface, (D3DCUBEMAP_FACES)face, level, &s9) : CALL9(INNER_TEXTURE(t), GetSurfaceLevel, level, &s9);
      Surface8 *surface = SUCCEEDED(*hr) ? mem_alloc(sizeof *surface) : NULL;
      if (surface) {
        obj_init(&surface->o, &surf_vtbl, KIND_SURFACE, d, (IUnknown *)s9);
        surface->level = level;
        surface->face = face;
        surface->o.container = &t->o;
        s = &surface->o;
        table_add(d, s);
      } else {
        if (SUCCEEDED(*hr)) *hr = E_OUTOFMEMORY;
        if (s9) CALL9(s9, Release);
      }
    }
    t->subs[i] = s;
  }
  LeaveCriticalSection(&d->lock);
  if (s) *hr = D3D_OK;
  return s;
}

static DWORD STDMETHODCALLTYPE btex_SetLOD(Object8 *o, DWORD lod) { return CALL9(INNER_BASE(o), SetLOD, lod); }
static DWORD STDMETHODCALLTYPE btex_GetLOD(Object8 *o) { return CALL9(INNER_BASE(o), GetLOD); }
static DWORD STDMETHODCALLTYPE btex_GetLevelCount(Object8 *o) { return CALL9(INNER_BASE(o), GetLevelCount); }

static HRESULT level_desc(Texture8 *t, UINT level, D3DSURFACE_DESC *desc) {
  if (!desc || level >= t->levels) return D3DERR_INVALIDCALL;
  D3DSURFACE_DESC9 d9;
  const HRESULT hr = t->o.kind == KIND_CUBE_TEXTURE ? CALL9(INNER_CUBE(t), GetLevelDesc, level, &d9) : CALL9(INNER_TEXTURE(t), GetLevelDesc, level, &d9);
  if (SUCCEEDED(hr)) surface_desc_from9(&d9, desc);
  return hr;
}
static HRESULT sub_out(Texture8 *t, UINT face, UINT level, void **out) {
  if (!out) return D3DERR_INVALIDCALL;
  *out = NULL;
  HRESULT hr;
  Object8 *s = texture_sub(t, face, level, &hr);
  if (!s) return hr;
  obj_AddRef(s);
  *out = s;
  return D3D_OK;
}

static D3DRESOURCETYPE STDMETHODCALLTYPE tex_GetType(Texture8 *t) { (void)t; return D3DRTYPE_TEXTURE; }
static HRESULT STDMETHODCALLTYPE tex_GetLevelDesc(Texture8 *t, UINT level, D3DSURFACE_DESC *desc) { return level_desc(t, level, desc); }
static HRESULT STDMETHODCALLTYPE tex_GetSurfaceLevel(Texture8 *t, UINT level, IDirect3DSurface8 **out) { return sub_out(t, 0, level, (void **)out); }
static HRESULT STDMETHODCALLTYPE tex_LockRect(Texture8 *t, UINT level, D3DLOCKED_RECT *locked, const RECT *rect, DWORD flags) {
  if (!locked) return D3DERR_INVALIDCALL;
  return CALL9(INNER_TEXTURE(t), LockRect, level, locked, rect, flags);
}
static HRESULT STDMETHODCALLTYPE tex_UnlockRect(Texture8 *t, UINT level) { return CALL9(INNER_TEXTURE(t), UnlockRect, level); }
static HRESULT STDMETHODCALLTYPE tex_AddDirtyRect(Texture8 *t, const RECT *rect) { return CALL9(INNER_TEXTURE(t), AddDirtyRect, rect); }

static D3DRESOURCETYPE STDMETHODCALLTYPE cube_GetType(Texture8 *t) { (void)t; return D3DRTYPE_CUBETEXTURE; }
static HRESULT STDMETHODCALLTYPE cube_GetLevelDesc(Texture8 *t, UINT level, D3DSURFACE_DESC *desc) { return level_desc(t, level, desc); }
static HRESULT STDMETHODCALLTYPE cube_GetCubeMapSurface(Texture8 *t, D3DCUBEMAP_FACES face, UINT level, IDirect3DSurface8 **out) { return sub_out(t, (UINT)face, level, (void **)out); }
static HRESULT STDMETHODCALLTYPE cube_LockRect(Texture8 *t, D3DCUBEMAP_FACES face, UINT level, D3DLOCKED_RECT *locked, const RECT *rect, DWORD flags) {
  if (!locked) return D3DERR_INVALIDCALL;
  return CALL9(INNER_CUBE(t), LockRect, face, level, locked, rect, flags);
}
static HRESULT STDMETHODCALLTYPE cube_UnlockRect(Texture8 *t, D3DCUBEMAP_FACES face, UINT level) { return CALL9(INNER_CUBE(t), UnlockRect, face, level); }
static HRESULT STDMETHODCALLTYPE cube_AddDirtyRect(Texture8 *t, D3DCUBEMAP_FACES face, const RECT *rect) { return CALL9(INNER_CUBE(t), AddDirtyRect, face, rect); }

static D3DRESOURCETYPE STDMETHODCALLTYPE vol_GetType(Texture8 *t) { (void)t; return D3DRTYPE_VOLUMETEXTURE; }
static HRESULT STDMETHODCALLTYPE vol_GetLevelDesc(Texture8 *t, UINT level, D3DVOLUME_DESC *desc) {
  if (!desc || level >= t->levels) return D3DERR_INVALIDCALL;
  D3DVOLUME_DESC9 d9;
  const HRESULT hr = CALL9(INNER_VOLUME_TEXTURE(t), GetLevelDesc, level, &d9);
  if (SUCCEEDED(hr)) volume_desc_from9(&d9, desc);
  return hr;
}
static HRESULT STDMETHODCALLTYPE vol_GetVolumeLevel(Texture8 *t, UINT level, IDirect3DVolume8 **out) { return sub_out(t, 0, level, (void **)out); }
static HRESULT STDMETHODCALLTYPE vol_LockBox(Texture8 *t, UINT level, D3DLOCKED_BOX *locked, const D3DBOX *box, DWORD flags) {
  if (!locked) return D3DERR_INVALIDCALL;
  return CALL9(INNER_VOLUME_TEXTURE(t), LockBox, level, locked, box, flags);
}
static HRESULT STDMETHODCALLTYPE vol_UnlockBox(Texture8 *t, UINT level) { return CALL9(INNER_VOLUME_TEXTURE(t), UnlockBox, level); }
static HRESULT STDMETHODCALLTYPE vol_AddDirtyBox(Texture8 *t, const D3DBOX *box) { return CALL9(INNER_VOLUME_TEXTURE(t), AddDirtyBox, box); }

/* ---- surfaces and volumes --------------------------------------------------------------------------------------------------------------------------- */

static HRESULT STDMETHODCALLTYPE surf_GetDevice(Object8 *o, IDirect3DDevice8 **out) { return res_GetDevice(o, out); }
/* A level's container is its texture; the container of any other surface is the device (the back buffers, depth buffers, the surfaces made alone). */
static HRESULT STDMETHODCALLTYPE surf_GetContainer(Object8 *o, REFIID riid, void **out) {
  if (!out) return D3DERR_INVALIDCALL;
  return obj_QueryInterface(o->container ? o->container : &o->device->o, riid, out);
}
static HRESULT STDMETHODCALLTYPE surf_GetDesc(Object8 *o, D3DSURFACE_DESC *desc) {
  if (!desc) return D3DERR_INVALIDCALL;
  if (!o->inner) return D3DERR_INVALIDCALL;
  D3DSURFACE_DESC9 d9;
  const HRESULT hr = CALL9(INNER_SURFACE(o), GetDesc, &d9);
  if (SUCCEEDED(hr)) surface_desc_from9(&d9, desc);
  return hr;
}
static HRESULT STDMETHODCALLTYPE surf_LockRect(Object8 *o, D3DLOCKED_RECT *locked, const RECT *rect, DWORD flags) {
  if (!locked || !o->inner) return D3DERR_INVALIDCALL;
  return CALL9(INNER_SURFACE(o), LockRect, locked, rect, flags);
}
static HRESULT STDMETHODCALLTYPE surf_UnlockRect(Object8 *o) {
  if (!o->inner) return D3DERR_INVALIDCALL;
  return CALL9(INNER_SURFACE(o), UnlockRect);
}

static HRESULT STDMETHODCALLTYPE volume_GetDevice(Object8 *o, IDirect3DDevice8 **out) { return res_GetDevice(o, out); }
static HRESULT STDMETHODCALLTYPE volume_GetContainer(Object8 *o, REFIID riid, void **out) {
  if (!out) return D3DERR_INVALIDCALL;
  return obj_QueryInterface(o->container, riid, out);
}
static HRESULT STDMETHODCALLTYPE volume_GetDesc(Volume8 *v, D3DVOLUME_DESC *desc) { return vol_GetLevelDesc((Texture8 *)v->o.container, v->level, desc); }
static HRESULT STDMETHODCALLTYPE volume_LockBox(Volume8 *v, D3DLOCKED_BOX *locked, const D3DBOX *box, DWORD flags) { return vol_LockBox((Texture8 *)v->o.container, v->level, locked, box, flags); }
static HRESULT STDMETHODCALLTYPE volume_UnlockBox(Volume8 *v) { return vol_UnlockBox((Texture8 *)v->o.container, v->level); }

/* An implicit surface of the device or of a swap chain (a back buffer, the depth buffer made with the device): 0 references, the Direct3D 9
 * surface's reference taken over. */
static Surface8 *implicit_new(Device8 *d, IDirect3DSurface9 *s9) {
  Surface8 *s = mem_alloc(sizeof *s);
  if (!s) { CALL9(s9, Release); return NULL; }
  obj_init(&s->o, &surf_vtbl, KIND_SURFACE, d, (IUnknown *)s9);
  s->o.ref = 0;
  s->o.implicit = TRUE;
  table_add(d, &s->o);
  return s;
}
/* An implicit surface takes another Direct3D 9 surface (Reset made new back buffers): the program's pointer stays the same. */
static void implicit_repoint(Device8 *d, Surface8 *s, IDirect3DSurface9 *s9) {
  if (s->o.inner == (IUnknown *)s9) { if (s9) CALL9(s9, Release); return; }
  table_remove(d, &s->o);
  unknown_release(s->o.inner);
  s->o.inner = (IUnknown *)s9;
  if (s9) table_add(d, &s->o);
}
static void implicit_free(Device8 *d, Surface8 *s) {
  if (!s) return;
  table_remove(d, &s->o);
  private_clear(&s->o);
  unknown_release(s->o.inner);
  mem_free(s);
}

/* ---- swap chains ------------------------------------------------------------------------------------------------------------------------------------ */

static void swap_chain_free_buffers(SwapChain8 *c) {
  for (UINT i = 0; i < 3; ++i) { implicit_free(c->o.device, c->back[i]); c->back[i] = NULL; }
}
static HRESULT STDMETHODCALLTYPE sc_Present(SwapChain8 *c, const RECT *source, const RECT *dest, HWND window, const RGNDATA *dirty) {
  return CALL9(INNER_SWAP_CHAIN(c), Present, source, dest, window, dirty, 0);
}
static HRESULT STDMETHODCALLTYPE sc_GetBackBuffer(SwapChain8 *c, UINT index, D3DBACKBUFFER_TYPE type, IDirect3DSurface8 **out) {
  if (!out) return D3DERR_INVALIDCALL;
  if (index >= 3) return D3DERR_INVALIDCALL;
  IDirect3DSurface9 *s9 = NULL;
  const HRESULT hr = CALL9(INNER_SWAP_CHAIN(c), GetBackBuffer, index, type, &s9);
  if (FAILED(hr)) return hr;
  Device8 *d = c->o.device;
  EnterCriticalSection(&d->lock);
  if (!c->back[index]) c->back[index] = implicit_new(d, s9);
  else implicit_repoint(d, c->back[index], s9);
  Surface8 *s = c->back[index];
  LeaveCriticalSection(&d->lock);
  if (!s) return E_OUTOFMEMORY;
  obj_AddRef(&s->o);
  *out = (IDirect3DSurface8 *)s;
  return D3D_OK;
}

/* ---- wrappers of what a Direct3D 9 method hands back ------------------------------------------------------------------------------------------------ */

/* The wrapper of a Direct3D 9 object a Get method gave (its reference is taken over): the one the program has, with a reference for the caller; one
 * made now if the program let its wrapper go while Direct3D 9 still used the object (a state block, a binding). NULL for NULL. */
static Object8 *wrap9(Device8 *d, IUnknown *inner, UINT kind) {
  if (!inner) return NULL;
  EnterCriticalSection(&d->lock);
  Object8 *o = table_find(d, inner);
  if (o) {
    obj_AddRef(o);
    LeaveCriticalSection(&d->lock);
    inner->lpVtbl->Release(inner);
    return o;
  }
  LeaveCriticalSection(&d->lock);
  switch (kind) {
  case KIND_VERTEX_BUFFER: return resource_new(sizeof(Object8), &vb_vtbl, KIND_VERTEX_BUFFER, d, inner);
  case KIND_INDEX_BUFFER: return resource_new(sizeof(Object8), &ib_vtbl, KIND_INDEX_BUFFER, d, inner);
  case KIND_SURFACE: return resource_new(sizeof(Surface8), &surf_vtbl, KIND_SURFACE, d, inner);
  case KIND_TEXTURE: {
    const D3DRESOURCETYPE type = CALL9((IDirect3DBaseTexture9 *)inner, GetType);
    const UINT k = type == D3DRTYPE_CUBETEXTURE ? KIND_CUBE_TEXTURE : type == D3DRTYPE_VOLUMETEXTURE ? KIND_VOLUME_TEXTURE : KIND_TEXTURE;
    return (Object8 *)texture_new(d, k, (IDirect3DBaseTexture9 *)inner);
  }
  default:
    inner->lpVtbl->Release(inner);
    return NULL;
  }
}

/* The Direct3D 9 object of a wrapper a program passes; NULL for NULL or what is not one of this device's (`*bad` is set then). */
static IUnknown *unwrap(Device8 *d, const void *object, UINT kind, BOOL *bad) {
  if (!object) return NULL;
  const Object8 *o = object;
  const BOOL texture = kind == KIND_TEXTURE && (o->kind == KIND_TEXTURE || o->kind == KIND_CUBE_TEXTURE || o->kind == KIND_VOLUME_TEXTURE);
  if ((o->kind != kind && !texture) || o->device != d || !o->inner) { *bad = TRUE; return NULL; }
  return o->inner;
}
