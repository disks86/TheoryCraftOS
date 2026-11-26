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

/* Part of d3d9.c: the Create methods of textures and surfaces - Direct3D 9's rules of pools and usages, and 9Ex's shared resources (a global share handle
 * made or opened through d3d12.dll: TcosD3D12GlobalShare, TcosD3D12OpenShared) and system-memory textures on the program's memory. */

static D3DPOOL resource_pool(Child *c) {
  const void *v = c->o.lpVtbl;
  if (v == &vb_vtbl || v == &ib_vtbl) return ((Buffer *)c)->pool;
  if (v == &tex_vtbl || v == &cube_vtbl || v == &vol_vtbl) return ((Tex *)c)->pool;
  if (v == &surf_vtbl) return ((Surface *)c)->tex->pool;
  if (v == &volume_vtbl) return ((Volume *)c)->tex->pool;
  return D3DPOOL_DEFAULT;
}
/* What keeps a set object alive (bind): a level's texture, a swap chain's buffer's swap chain (the implicit one's are kept by the device), else the object. */
static Child *binding_of(void *object) {
  Child *c = object;
  if (c->o.lpVtbl == &surf_vtbl) {
    Surface *s = object;
    if (s->kind == SURFACE_LEVEL) return &s->tex->c;
    if (s->kind == SURFACE_IMPLICIT && s->container && s->container->lpVtbl == &chain_vtbl) return (Child *)s->container;
  }
  if (c->o.lpVtbl == &volume_vtbl) return &((Volume *)c)->tex->c;
  return c;
}

/* pSharedHandle of the Create methods that take no part in sharing (buffers): D3DERR_NOTAVAILABLE for system-memory buffers, D3DERR_INVALIDCALL else (Windows since 7). */
static HRESULT shared_refused(Dev *d, HANDLE *shared, D3DPOOL pool) {
  (void)d;
  if (!shared) return D3D_OK;
  return pool == D3DPOOL_SYSTEMMEM ? D3DERR_NOTAVAILABLE : D3DERR_INVALIDCALL;
}
/* Opens the texture a global share handle names, which must have been made with the same description (by a Direct3D 9 device; or a Direct3D 10, 11 or 12 one,
 * of the same size and a format of the same pixels). */
static HRESULT shared_open(Dev *d, HANDLE handle, UINT kind, UINT width, UINT height, UINT slices, UINT levels, DWORD usage, D3DFORMAT format, D3DPOOL pool, UINT flags, Tex **out) {
  *out = NULL;
  ID3D12Resource *res = NULL;
  HRESULT hr = TcosD3D12OpenShared(d->d12, handle, 1, &IID_ID3D12Resource, (void **)&res);
  if (FAILED(hr) || !res) return D3DERR_INVALIDCALL;
  DWORD blob[8];
  const UINT size = TcosD3D12SharedExtra(res, blob, sizeof blob);
  const D3D12_RESOURCE_DESC rd = ID3D12Resource_GetDesc(res);
  BOOL same;
  if (size == sizeof blob && blob[0] == 0x54333944u)
    same = blob[1] == kind && blob[2] == width && blob[3] == height && blob[4] == (kind == 1 ? slices : 1) && (levels == 0 || blob[5] == levels) && blob[6] == (DWORD)format &&
           (blob[7] & (D3DUSAGE_RENDERTARGET | D3DUSAGE_DEPTHSTENCIL)) == (usage & (D3DUSAGE_RENDERTARGET | D3DUSAGE_DEPTHSTENCIL));
  else
    same = kind == 0 && rd.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && rd.Width == width && rd.Height == height &&
           ((rd.Format == DXGI_FORMAT_B8G8R8A8_UNORM && (format == D3DFMT_A8R8G8B8 || format == D3DFMT_X8R8G8B8)) || (rd.Format == DXGI_FORMAT_B8G8R8X8_UNORM && format == D3DFMT_X8R8G8B8));
  if (!same) { ID3D12Resource_Release(res); return D3DERR_INVALIDCALL; }
  EnterCriticalSection(&d->lock);
  hr = tex_make_kind(d, kind, width, height, slices, levels ? levels : rd.MipLevels, usage | ((rd.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) ? D3DUSAGE_RENDERTARGET : 0), format,
                     pool, flags, res, out);
  LeaveCriticalSection(&d->lock);
  ID3D12Resource_Release(res);
  if (SUCCEEDED(hr)) (*out)->shared = handle;
  return hr;
}
/* The usages a texture may be made with. */
#define TEXTURE_USAGES (D3DUSAGE_RENDERTARGET | D3DUSAGE_DEPTHSTENCIL | D3DUSAGE_DYNAMIC | D3DUSAGE_AUTOGENMIPMAP | D3DUSAGE_DMAP | D3DUSAGE_SOFTWAREPROCESSING | \
                        D3DUSAGE_NONSECURE | D3DUSAGE_TEXTAPI | D3DUSAGE_RESTRICTED_CONTENT | D3DUSAGE_RESTRICT_SHARED_RESOURCE | D3DUSAGE_RESTRICT_SHARED_RESOURCE_DRIVER)
/* CreateTexture, CreateCubeTexture, CreateVolumeTexture: the pool and usage rules of Direct3D 9, then the texture - made, shared (pSharedHandle pointing at NULL,
 * a 9Ex device's default pool), opened (pointing at a handle), or (9Ex, system memory, one level of a 2D texture) on the program's memory. */
static HRESULT texture_create(Dev *d, UINT kind, UINT width, UINT height, UINT slices, UINT levels, DWORD usage, D3DFORMAT format, D3DPOOL pool, HANDLE *shared, Tex **out) {
  *out = NULL;
  if (!pool_valid(d, pool) || (usage & ~(DWORD)TEXTURE_USAGES)) return D3DERR_INVALIDCALL;
  if ((usage & (D3DUSAGE_RENDERTARGET | D3DUSAGE_DEPTHSTENCIL)) && pool != D3DPOOL_DEFAULT) return D3DERR_INVALIDCALL;
  if ((usage & D3DUSAGE_DYNAMIC) && (pool == D3DPOOL_MANAGED || (usage & (D3DUSAGE_RENDERTARGET | D3DUSAGE_DEPTHSTENCIL)))) return D3DERR_INVALIDCALL;
  if ((usage & D3DUSAGE_AUTOGENMIPMAP) && (pool == D3DPOOL_SYSTEMMEM || kind == 1 || levels > 1 || dxt_block(format))) return D3DERR_INVALIDCALL;
  if ((usage & D3DUSAGE_DEPTHSTENCIL) && kind == 1) return D3DERR_INVALIDCALL;
  if (kind == 1 && format_is_depth(format)) return D3DERR_INVALIDCALL;
  UINT flags = 0;
  if (shared) {
    if (!d->ex) return D3DERR_INVALIDCALL;      /* (sharing is 9Ex's) */
    if (pool == D3DPOOL_SYSTEMMEM) {
      /* (a 2D texture of one level whose pixels are the program's memory) */
      if (kind != 0 || levels != 1 || !*shared) return D3DERR_INVALIDCALL;
      Tex *t = NULL;
      EnterCriticalSection(&d->lock);
      HRESULT hr = tex_make_kind(d, 0, width, height, 1, 1, usage, format, pool, TEX_MEMORY, NULL, &t);
      LeaveCriticalSection(&d->lock);
      if (FAILED(hr)) return hr;
      t->user_memory = *shared;
      t->shadow[0] = *shared;
      *out = t;
      return D3D_OK;
    }
    if (pool != D3DPOOL_DEFAULT) return D3DERR_INVALIDCALL;
    if (*shared) return shared_open(d, *shared, kind, width, height, slices, levels, usage, format, pool, 0, out);
    flags |= TEX_SHARED;
  }
  EnterCriticalSection(&d->lock);
  const HRESULT hr = tex_make_kind(d, kind, width, height, slices, levels, usage, format, pool, pool == D3DPOOL_SYSTEMMEM || pool == D3DPOOL_SCRATCH ? TEX_MEMORY | flags : flags, NULL, out);
  LeaveCriticalSection(&d->lock);
  if (SUCCEEDED(hr) && shared) *shared = (*out)->shared;
  return hr;
}
static HRESULT STDMETHODCALLTYPE dev_CreateTexture(Dev *d, UINT width, UINT height, UINT levels, DWORD usage, D3DFORMAT format, D3DPOOL pool, IDirect3DTexture9 **out, HANDLE *shared) {
  if (!out) return D3DERR_INVALIDCALL;
  return texture_create(d, 0, width, height, 1, levels, usage, format, pool, shared, (Tex **)out);
}
/* A surface on its own, shared or opened as a texture is (see texture_create; a surface in system memory may be the program's memory on 9Ex). */
static HRESULT surface_create(Dev *d, UINT width, UINT height, DWORD usage, D3DFORMAT format, D3DPOOL pool, UINT flags, D3DMULTISAMPLE_TYPE samples, DWORD quality, HANDLE *shared,
                              IDirect3DSurface9 **out) {
  if (!out) return D3DERR_INVALIDCALL;
  *out = NULL;
  if (samples != D3DMULTISAMPLE_NONE && samples != D3DMULTISAMPLE_NONMASKABLE) return D3DERR_NOTAVAILABLE;      /* (CheckDeviceMultiSampleType: no multisampling here) */
  if (samples == D3DMULTISAMPLE_NONMASKABLE && quality) return D3DERR_INVALIDCALL;
  if (!shared) return surface_alone(d, width, height, usage, format, pool, flags, samples, quality, out);
  if (!d->ex) return D3DERR_INVALIDCALL;
  if (pool == D3DPOOL_SYSTEMMEM) {
    if (!*shared || (usage & (D3DUSAGE_RENDERTARGET | D3DUSAGE_DEPTHSTENCIL))) return D3DERR_INVALIDCALL;
    HRESULT hr = surface_alone(d, width, height, usage, format, pool, flags | TEX_MEMORY, samples, quality, out);
    if (SUCCEEDED(hr)) {
      Tex *t = ((Surface *)*out)->tex;
      t->user_memory = *shared;
      t->shadow[0] = *shared;
    }
    return hr;
  }
  if (pool != D3DPOOL_DEFAULT) return D3DERR_INVALIDCALL;
  Tex *t = NULL;
  HRESULT hr;
  if (*shared) hr = shared_open(d, *shared, 0, width, height, 1, 1, usage, format, pool, (flags & ~TEX_MEMORY) | TEX_ALONE | TEX_INTERNAL, &t);
  else {
    EnterCriticalSection(&d->lock);
    hr = tex_make(d, width, height, 1, usage, format, pool, (flags & ~TEX_MEMORY) | TEX_ALONE | TEX_INTERNAL | TEX_SHARED, NULL, &t);
    LeaveCriticalSection(&d->lock);
  }
  if (FAILED(hr)) return hr;
  t->internal = 0;
  t->samples = samples;
  t->quality = quality;
  Surface *s = surface_make(d, t, SURFACE_ALONE, NULL);
  if (!s) { obj_free(OBJ(t)); return E_OUTOFMEMORY; }
  t->c.kept = 1;
  default_resource_made(d, pool);
  if (!*shared) *shared = t->shared;
  *out = (IDirect3DSurface9 *)s;
  return D3D_OK;
}
static HRESULT STDMETHODCALLTYPE dev_CreateRenderTarget(Dev *d, UINT width, UINT height, D3DFORMAT format, D3DMULTISAMPLE_TYPE samples, DWORD quality, BOOL lockable, IDirect3DSurface9 **out,
                                                        HANDLE *shared) {
  return surface_create(d, width, height, D3DUSAGE_RENDERTARGET, format, D3DPOOL_DEFAULT, lockable ? TEX_LOCKABLE : 0, samples, quality, shared, out);
}
static HRESULT STDMETHODCALLTYPE dev_CreateDepthStencilSurface(Dev *d, UINT width, UINT height, D3DFORMAT format, D3DMULTISAMPLE_TYPE samples, DWORD quality, BOOL discard,
                                                               IDirect3DSurface9 **out, HANDLE *shared) {
  (void)discard;
  if (out) *out = NULL;
  if (!format_is_depth(format)) return D3DERR_INVALIDCALL;
  return surface_create(d, width, height, D3DUSAGE_DEPTHSTENCIL, format, D3DPOOL_DEFAULT, 0, samples, quality, shared, out);
}
/* Off-screen plain surfaces of the default pool are memory here (the device copies them in when a copy reads them), unless shared. */
static HRESULT STDMETHODCALLTYPE dev_CreateOffscreenPlainSurface(Dev *d, UINT width, UINT height, D3DFORMAT format, D3DPOOL pool, IDirect3DSurface9 **out, HANDLE *shared) {
  if (out) *out = NULL;
  if (pool == D3DPOOL_MANAGED || !pool_valid(d, pool) || format_is_depth(format)) return D3DERR_INVALIDCALL;
  if (pool == D3DPOOL_SCRATCH && shared) return D3DERR_INVALIDCALL;
  return surface_create(d, width, height, 0, format, pool, shared && pool == D3DPOOL_DEFAULT ? 0 : TEX_MEMORY, D3DMULTISAMPLE_NONE, 0, shared, out);
}
/* (9Ex) the same with usage flags (D3DUSAGE_RESTRICTED_CONTENT and the like, which change nothing of what the surface is here) */
#define EX_SURFACE_USAGES (D3DUSAGE_RESTRICTED_CONTENT | D3DUSAGE_RESTRICT_SHARED_RESOURCE | D3DUSAGE_RESTRICT_SHARED_RESOURCE_DRIVER | D3DUSAGE_NONSECURE)
static HRESULT STDMETHODCALLTYPE dev_CreateRenderTargetEx(Dev *d, UINT width, UINT height, D3DFORMAT format, D3DMULTISAMPLE_TYPE samples, DWORD quality, BOOL lockable,
                                                          IDirect3DSurface9 **out, HANDLE *shared, DWORD usage) {
  if (usage & ~(DWORD)EX_SURFACE_USAGES) return D3DERR_INVALIDCALL;
  return surface_create(d, width, height, D3DUSAGE_RENDERTARGET | usage, format, D3DPOOL_DEFAULT, lockable ? TEX_LOCKABLE : 0, samples, quality, shared, out);
}
static HRESULT STDMETHODCALLTYPE dev_CreateOffscreenPlainSurfaceEx(Dev *d, UINT width, UINT height, D3DFORMAT format, D3DPOOL pool, IDirect3DSurface9 **out, HANDLE *shared, DWORD usage) {
  if ((usage & ~(DWORD)EX_SURFACE_USAGES) || pool == D3DPOOL_MANAGED || pool == D3DPOOL_SCRATCH || format_is_depth(format)) return D3DERR_INVALIDCALL;
  return surface_create(d, width, height, usage, format, pool, shared && pool == D3DPOOL_DEFAULT ? 0 : TEX_MEMORY, D3DMULTISAMPLE_NONE, 0, shared, out);
}
static HRESULT STDMETHODCALLTYPE dev_CreateDepthStencilSurfaceEx(Dev *d, UINT width, UINT height, D3DFORMAT format, D3DMULTISAMPLE_TYPE samples, DWORD quality, BOOL discard,
                                                                 IDirect3DSurface9 **out, HANDLE *shared, DWORD usage) {
  (void)discard;
  /* (a call refused for its arguments leaves *out as it was, as Windows does) */
  if (!format_is_depth(format) || (usage & ~(DWORD)EX_SURFACE_USAGES)) return D3DERR_INVALIDCALL;
  return surface_create(d, width, height, D3DUSAGE_DEPTHSTENCIL | usage, format, D3DPOOL_DEFAULT, 0, samples, quality, shared, out);
}
