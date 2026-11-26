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

/* d3d8.dll: IDirect3DDevice8 - its own methods, the resources it makes, what is drawn into and copies between surfaces, the streams and indices,
 * the draws. (Render states, stage states and state blocks: state.inc; shaders: shaders.inc.) Included by d3d8.c. */

#define STREAMS8 16u
#define DEAD_DEVICES 8u

/* ---- the implicit surfaces --------------------------------------------------------------------------------------------------------------------------- */

/* The device's back buffers and depth buffer, after the device was made or Reset: the same wrappers, now with Direct3D 9's surfaces. */
static void implicit_refresh(Device8 *d) {
  EnterCriticalSection(&d->lock);
  for (UINT i = 0; i < 3; ++i) {
    IDirect3DSurface9 *s9 = NULL;
    if (FAILED(CALL9(D9(d), GetBackBuffer, 0, i, D3DBACKBUFFER_TYPE_MONO, &s9))) s9 = NULL;
    if (d->back[i]) implicit_repoint(d, d->back[i], s9);
    else if (s9) d->back[i] = implicit_new(d, s9);
  }
  IDirect3DSurface9 *ds = NULL;
  if (FAILED(CALL9(D9(d), GetDepthStencilSurface, &ds))) ds = NULL;
  if (d->auto_depth) implicit_repoint(d, d->auto_depth, ds);
  else if (ds) d->auto_depth = implicit_new(d, ds);
  LeaveCriticalSection(&d->lock);
}

static void device_destroy(Device8 *d) {
  state_block_free(d->recording);
  for (UINT i = 0; i < d->state_blocks.count; ++i) state_block_free(d->state_blocks.entries[i]);
  for (UINT i = 0; i < d->vertex_shaders.count; ++i) vertex_shader_free(d->vertex_shaders.entries[i]);
  for (UINT i = 0; i < d->pixel_shaders.count; ++i) pixel_shader_free(d->pixel_shaders.entries[i]);
  mem_free(d->state_blocks.entries);
  mem_free(d->vertex_shaders.entries);
  mem_free(d->pixel_shaders.entries);
  for (UINT i = 0; i < 3; ++i) implicit_free(d, d->back[i]);
  implicit_free(d, d->auto_depth);
  for (UINT i = 0; i < GRAVES; ++i) mem_free(d->graves[i]);
  CALL9(D9(d), Release);
  CALL9((IUnknown *)d->parent, Release);
  DeleteCriticalSection(&d->lock);
  /* The device's head is kept a while too (see object_destroy): a program may still ask a device whose last reference its last resource took
   * away what its count is (Wine's test_refcount does), and Direct3D 8 answers 0. The oldest of DEAD_DEVICES such heads is freed. */
  static Device8 *dead[DEAD_DEVICES];
  static LONG next_dead;
  d->o.dead = TRUE;
  const LONG slot = (InterlockedIncrement(&next_dead) - 1) % (LONG)DEAD_DEVICES;
  mem_free(InterlockedExchangePointer((void **)&dead[slot], d));
}

/* ---- the device's own methods ----------------------------------------------------------------------------------------------------------------------- */

static HRESULT STDMETHODCALLTYPE dev_TestCooperativeLevel(Device8 *d) { return d->not_reset ? D3DERR_DEVICENOTRESET : CALL9(D9(d), TestCooperativeLevel); }
static UINT STDMETHODCALLTYPE dev_GetAvailableTextureMem(Device8 *d) { return CALL9(D9(d), GetAvailableTextureMem); }
/* (Direct3D 8 evicts all managed resources whatever the number of bytes: Direct3D 9's EvictManagedResources) */
static HRESULT STDMETHODCALLTYPE dev_ResourceManagerDiscardBytes(Device8 *d, DWORD bytes) { (void)bytes; return CALL9(D9(d), EvictManagedResources); }
static HRESULT STDMETHODCALLTYPE dev_GetDirect3D(Device8 *d, IDirect3D8 **out) {
  if (!out) return D3DERR_INVALIDCALL;
  CALL9((IUnknown *)d->parent, AddRef);
  *out = (IDirect3D8 *)d->parent;
  return D3D_OK;
}
static HRESULT STDMETHODCALLTYPE dev_GetDeviceCaps(Device8 *d, D3DCAPS8 *caps) {
  if (!caps) return D3DERR_INVALIDCALL;
  D3DCAPS9 c9;
  const HRESULT hr = CALL9(D9(d), GetDeviceCaps, &c9);
  if (SUCCEEDED(hr)) caps_from9(&c9, caps);
  return hr;
}
static HRESULT STDMETHODCALLTYPE dev_GetDisplayMode(Device8 *d, D3DDISPLAYMODE *mode) {
  if (!mode) return D3DERR_INVALIDCALL;
  return CALL9(D9(d), GetDisplayMode, 0, mode);
}
static HRESULT STDMETHODCALLTYPE dev_GetCreationParameters(Device8 *d, D3DDEVICE_CREATION_PARAMETERS *parameters) {
  if (!parameters) return D3DERR_INVALIDCALL;
  return CALL9(D9(d), GetCreationParameters, parameters);
}
static HRESULT STDMETHODCALLTYPE dev_SetCursorProperties(Device8 *d, UINT x, UINT y, IDirect3DSurface8 *bitmap) {
  BOOL bad = FALSE;
  IDirect3DSurface9 *s9 = (IDirect3DSurface9 *)unwrap(d, bitmap, KIND_SURFACE, &bad);
  if (!bitmap || bad) return D3DERR_INVALIDCALL;
  return CALL9(D9(d), SetCursorProperties, x, y, s9);
}
static void STDMETHODCALLTYPE dev_SetCursorPosition(Device8 *d, UINT x, UINT y, DWORD flags) { CALL9(D9(d), SetCursorPosition, (int)x, (int)y, flags); }
static BOOL STDMETHODCALLTYPE dev_ShowCursor(Device8 *d, BOOL show) { return CALL9(D9(d), ShowCursor, show); }

static HRESULT STDMETHODCALLTYPE dev_CreateAdditionalSwapChain(Device8 *d, D3DPRESENT_PARAMETERS *parameters, IDirect3DSwapChain8 **out) {
  if (!out) return D3DERR_INVALIDCALL;
  *out = NULL;
  D3DPRESENT_PARAMETERS9 p9;
  HRESULT hr = present_to9(parameters, &p9, FALSE);
  if (FAILED(hr)) return hr;
  IDirect3DSwapChain9 *c9 = NULL;
  hr = CALL9(D9(d), CreateAdditionalSwapChain, &p9, &c9);
  if (FAILED(hr)) return hr;
  present_from9(&p9, parameters);
  SwapChain8 *c = resource_new(sizeof *c, &sc_vtbl, KIND_SWAP_CHAIN, d, (IUnknown *)c9);
  if (!c) { CALL9(c9, Release); return E_OUTOFMEMORY; }
  *out = (IDirect3DSwapChain8 *)c;
  return D3D_OK;
}

/* Whether a full screen of these parameters is a mode the adapter has (of the display format the back buffer goes with). */
static BOOL full_screen_mode_exists(Device8 *d, const D3DPRESENT_PARAMETERS9 *p9) {
  if (p9->Windowed) return TRUE;
  D3DDEVICE_CREATION_PARAMETERS creation;
  if (FAILED(CALL9(D9(d), GetCreationParameters, &creation))) return FALSE;
  const D3DFORMAT display = p9->BackBufferFormat == D3DFMT_A8R8G8B8 ? D3DFMT_X8R8G8B8 : p9->BackBufferFormat == D3DFMT_A1R5G5B5 ? D3DFMT_X1R5G5B5 : p9->BackBufferFormat;
  IDirect3D9 *d3d9 = d->parent->d9;
  const UINT modes = CALL9(d3d9, GetAdapterModeCount, creation.AdapterOrdinal, display);
  for (UINT i = 0; i < modes; ++i) {
    D3DDISPLAYMODE mode;
    if (SUCCEEDED(CALL9(d3d9, EnumAdapterModes, creation.AdapterOrdinal, display, i, &mode)) && mode.Width == p9->BackBufferWidth && mode.Height == p9->BackBufferHeight)
      return TRUE;
  }
  return FALSE;
}

static HRESULT STDMETHODCALLTYPE dev_Reset(Device8 *d, D3DPRESENT_PARAMETERS *parameters) {
  D3DPRESENT_PARAMETERS9 p9;
  HRESULT hr = present_to9(parameters, &p9, TRUE);
  if (FAILED(hr)) return hr;
  /* A full screen of no mode the adapter has is refused, and the device is not reset (as a Reset that fails leaves it). */
  if (!full_screen_mode_exists(d, &p9)) {
    d->not_reset = TRUE;
    return D3DERR_INVALIDCALL;
  }
  EnterCriticalSection(&d->lock);
  /* Direct3D 9 resets only when the program holds none of the swap chain's buffers: the implicit surfaces the program holds no reference of
   * let theirs go (and take the new ones after); one the program holds keeps the Reset from happening, as in Direct3D 8. */
  for (UINT i = 0; i < 4; ++i) {
    Surface8 *s = i < 3 ? d->back[i] : d->auto_depth;
    if (s && s->o.ref == 0) implicit_repoint(d, s, NULL);
  }
  hr = CALL9(D9(d), Reset, &p9);
  if (FAILED(hr)) {
    /* (Direct3D 9 refuses with D3DERR_INVALIDCALL what the program still holds or binds of the default pool; Direct3D 8 answers that
     * D3DERR_DEVICELOST, and the device stays lost - TestCooperativeLevel says D3DERR_DEVICENOTRESET - until a Reset succeeds) */
    implicit_refresh(d);
    d->not_reset = TRUE;
    if (hr == D3DERR_INVALIDCALL) hr = D3DERR_DEVICELOST;
  }
  if (SUCCEEDED(hr)) {
    d->not_reset = FALSE;
    defaults8_apply(d);
    present_from9(&p9, parameters);
    d->present = *parameters;
    /* Direct3D 9 gave its device the state of a new device: so does Direct3D 8 with its own (a recording state block ends unmade). */
    state_block_free(d->recording);
    d->recording = NULL;
    state8_defaults(d);
    implicit_refresh(d);
  }
  LeaveCriticalSection(&d->lock);
  return hr;
}
static HRESULT STDMETHODCALLTYPE dev_Present(Device8 *d, const RECT *source, const RECT *dest, HWND window, const RGNDATA *dirty) {
  if (d->not_reset) return D3DERR_DEVICELOST;
  return CALL9(D9(d), Present, source, dest, window, dirty);
}
static HRESULT STDMETHODCALLTYPE dev_GetBackBuffer(Device8 *d, UINT index, D3DBACKBUFFER_TYPE type, IDirect3DSurface8 **out) {
  if (!out) return D3DERR_INVALIDCALL;
  *out = NULL;      /* (the device's sets NULL when it fails, as Wine's test_swapchain finds Windows doing; a swap chain's leaves the pointer) */
  if (index >= 3) return D3DERR_INVALIDCALL;
  IDirect3DSurface9 *s9 = NULL;
  const HRESULT hr = CALL9(D9(d), GetBackBuffer, 0, index, type, &s9);
  if (FAILED(hr)) return hr;
  EnterCriticalSection(&d->lock);
  if (!d->back[index]) d->back[index] = implicit_new(d, s9);
  else implicit_repoint(d, d->back[index], s9);
  Surface8 *s = d->back[index];
  LeaveCriticalSection(&d->lock);
  if (!s) return E_OUTOFMEMORY;
  obj_AddRef(&s->o);
  *out = (IDirect3DSurface8 *)s;
  return D3D_OK;
}
static HRESULT STDMETHODCALLTYPE dev_GetRasterStatus(Device8 *d, D3DRASTER_STATUS *status) {
  if (!status) return D3DERR_INVALIDCALL;
  return CALL9(D9(d), GetRasterStatus, 0, status);
}
static void STDMETHODCALLTYPE dev_SetGammaRamp(Device8 *d, DWORD flags, const D3DGAMMARAMP *ramp) { if (ramp) CALL9(D9(d), SetGammaRamp, 0, flags, ramp); }
static void STDMETHODCALLTYPE dev_GetGammaRamp(Device8 *d, D3DGAMMARAMP *ramp) { if (ramp) CALL9(D9(d), GetGammaRamp, 0, ramp); }

/* ---- resources ---------------------------------------------------------------------------------------------------------------------------------- */

static HRESULT STDMETHODCALLTYPE dev_CreateTexture(Device8 *d, UINT width, UINT height, UINT levels, DWORD usage, D3DFORMAT format, D3DPOOL pool, IDirect3DTexture8 **out) {
  if (!out || format == D3DFMT_UNKNOWN) return D3DERR_INVALIDCALL;      /* (the program's pointer is left as it was, as in Direct3D 8) */
  *out = NULL;
  IDirect3DTexture9 *t9 = NULL;
  const HRESULT hr = CALL9(D9(d), CreateTexture, width, height, levels, usage & USAGE8_MASK, format, pool, &t9, NULL);
  if (FAILED(hr)) return hr;
  Texture8 *t = texture_new(d, KIND_TEXTURE, (IDirect3DBaseTexture9 *)t9);
  if (!t) return E_OUTOFMEMORY;
  *out = (IDirect3DTexture8 *)t;
  return D3D_OK;
}
static HRESULT STDMETHODCALLTYPE dev_CreateVolumeTexture(Device8 *d, UINT width, UINT height, UINT depth, UINT levels, DWORD usage, D3DFORMAT format, D3DPOOL pool,
                                                         IDirect3DVolumeTexture8 **out) {
  if (!out || format == D3DFMT_UNKNOWN) return D3DERR_INVALIDCALL;      /* (the program's pointer is left as it was, as in Direct3D 8) */
  *out = NULL;
  IDirect3DVolumeTexture9 *t9 = NULL;
  const HRESULT hr = CALL9(D9(d), CreateVolumeTexture, width, height, depth, levels, usage & USAGE8_MASK, format, pool, &t9, NULL);
  if (FAILED(hr)) return hr;
  Texture8 *t = texture_new(d, KIND_VOLUME_TEXTURE, (IDirect3DBaseTexture9 *)t9);
  if (!t) return E_OUTOFMEMORY;
  *out = (IDirect3DVolumeTexture8 *)t;
  return D3D_OK;
}
static HRESULT STDMETHODCALLTYPE dev_CreateCubeTexture(Device8 *d, UINT edge, UINT levels, DWORD usage, D3DFORMAT format, D3DPOOL pool, IDirect3DCubeTexture8 **out) {
  if (!out || format == D3DFMT_UNKNOWN) return D3DERR_INVALIDCALL;      /* (the program's pointer is left as it was, as in Direct3D 8) */
  *out = NULL;
  IDirect3DCubeTexture9 *t9 = NULL;
  const HRESULT hr = CALL9(D9(d), CreateCubeTexture, edge, levels, usage & USAGE8_MASK, format, pool, &t9, NULL);
  if (FAILED(hr)) return hr;
  Texture8 *t = texture_new(d, KIND_CUBE_TEXTURE, (IDirect3DBaseTexture9 *)t9);
  if (!t) return E_OUTOFMEMORY;
  *out = (IDirect3DCubeTexture8 *)t;
  return D3D_OK;
}
static HRESULT STDMETHODCALLTYPE dev_CreateVertexBuffer(Device8 *d, UINT length, DWORD usage, DWORD fvf, D3DPOOL pool, IDirect3DVertexBuffer8 **out) {
  if (!out) return D3DERR_INVALIDCALL;
  *out = NULL;
  IDirect3DVertexBuffer9 *b9 = NULL;
  const HRESULT hr = CALL9(D9(d), CreateVertexBuffer, length, usage & USAGE8_MASK, fvf, pool, &b9, NULL);
  if (FAILED(hr)) return hr;
  Object8 *b = resource_new(sizeof *b, &vb_vtbl, KIND_VERTEX_BUFFER, d, (IUnknown *)b9);
  if (!b) { CALL9(b9, Release); return E_OUTOFMEMORY; }
  *out = (IDirect3DVertexBuffer8 *)b;
  return D3D_OK;
}
static HRESULT STDMETHODCALLTYPE dev_CreateIndexBuffer(Device8 *d, UINT length, DWORD usage, D3DFORMAT format, D3DPOOL pool, IDirect3DIndexBuffer8 **out) {
  if (!out) return D3DERR_INVALIDCALL;
  *out = NULL;
  if (format != D3DFMT_INDEX16 && format != D3DFMT_INDEX32) return D3DERR_INVALIDCALL;
  IDirect3DIndexBuffer9 *b9 = NULL;
  const HRESULT hr = CALL9(D9(d), CreateIndexBuffer, length, usage & USAGE8_MASK, format, pool, &b9, NULL);
  if (FAILED(hr)) return hr;
  Object8 *b = resource_new(sizeof *b, &ib_vtbl, KIND_INDEX_BUFFER, d, (IUnknown *)b9);
  if (!b) { CALL9(b9, Release); return E_OUTOFMEMORY; }
  *out = (IDirect3DIndexBuffer8 *)b;
  return D3D_OK;
}
/* (`hr` and `s9` of a create call made before: an argument list does not order a call and the reading of what it wrote) */
static HRESULT surface_out(Device8 *d, HRESULT hr, IDirect3DSurface9 *s9, IDirect3DSurface8 **out) {
  if (FAILED(hr)) return hr;
  Surface8 *s = resource_new(sizeof *s, &surf_vtbl, KIND_SURFACE, d, (IUnknown *)s9);
  if (!s) { CALL9(s9, Release); return E_OUTOFMEMORY; }
  *out = (IDirect3DSurface8 *)s;
  return D3D_OK;
}
static HRESULT STDMETHODCALLTYPE dev_CreateRenderTarget(Device8 *d, UINT width, UINT height, D3DFORMAT format, D3DMULTISAMPLE_TYPE samples, BOOL lockable, IDirect3DSurface8 **out) {
  if (!out || format == D3DFMT_UNKNOWN) return D3DERR_INVALIDCALL;      /* (the program's pointer is left as it was, as in Direct3D 8) */
  *out = NULL;
  IDirect3DSurface9 *s9 = NULL;
  const HRESULT hr = CALL9(D9(d), CreateRenderTarget, width, height, format, samples, 0, lockable, &s9, NULL);
  return surface_out(d, hr, s9, out);
}
static HRESULT STDMETHODCALLTYPE dev_CreateDepthStencilSurface(Device8 *d, UINT width, UINT height, D3DFORMAT format, D3DMULTISAMPLE_TYPE samples, IDirect3DSurface8 **out) {
  if (!out || format == D3DFMT_UNKNOWN) return D3DERR_INVALIDCALL;      /* (the program's pointer is left as it was, as in Direct3D 8) */
  *out = NULL;
  IDirect3DSurface9 *s9 = NULL;
  const HRESULT hr = CALL9(D9(d), CreateDepthStencilSurface, width, height, format, samples, 0, FALSE, &s9, NULL);
  return surface_out(d, hr, s9, out);
}
/* (an image surface of Direct3D 8 is a surface in system memory) */
static HRESULT STDMETHODCALLTYPE dev_CreateImageSurface(Device8 *d, UINT width, UINT height, D3DFORMAT format, IDirect3DSurface8 **out) {
  if (!out) return D3DERR_INVALIDCALL;
  *out = NULL;
  IDirect3DSurface9 *s9 = NULL;
  const HRESULT hr = CALL9(D9(d), CreateOffscreenPlainSurface, width, height, format, D3DPOOL_SYSTEMMEM, &s9, NULL);
  return surface_out(d, hr, s9, out);
}

/* ---- copies --------------------------------------------------------------------------------------------------------------------------------------- */

/* One rectangle copied by locking both surfaces, a row at a time (rows of blocks for the compressed formats). */
static HRESULT copy_by_lock(IDirect3DSurface9 *src, const RECT *rect, IDirect3DSurface9 *dst, const POINT *point, D3DFORMAT format) {
  const LONG w = rect->right - rect->left, h = rect->bottom - rect->top;
  const RECT to = {point->x, point->y, point->x + w, point->y + h};
  const UINT block = format_block_bytes(format), bits = format_bits(format);
  if (!block && !bits) return D3DERR_INVALIDCALL;
  const SIZE_T row = block ? (SIZE_T)((w + 3) / 4) * block : (SIZE_T)w * bits / 8;
  const UINT rows = block ? (UINT)((h + 3) / 4) : (UINT)h;
  D3DLOCKED_RECT from, into;
  HRESULT hr = CALL9(src, LockRect, &from, rect, D3DLOCK_READONLY);
  if (FAILED(hr)) return hr;
  hr = CALL9(dst, LockRect, &into, &to, 0);
  if (FAILED(hr)) { CALL9(src, UnlockRect); return hr; }
  for (UINT y = 0; y < rows; ++y) memcpy((BYTE *)into.pBits + (SSIZE_T)y * into.Pitch, (const BYTE *)from.pBits + (SSIZE_T)y * from.Pitch, row);
  CALL9(dst, UnlockRect);
  CALL9(src, UnlockRect);
  return D3D_OK;
}
/* One rectangle, by what Direct3D 9 has for the two pools: UpdateSurface from memory into the device, StretchRect of one size between surfaces of
 * the device, GetRenderTargetData of a target into memory (of all of it into a surface of its size first when only a part is wanted, or the
 * sizes differ), and locking both where neither is in the device (Direct3D 9 has no copy between surfaces in memory). */
static HRESULT copy_rect(Device8 *d, IDirect3DSurface9 *src, const D3DSURFACE_DESC9 *sd, const RECT *rect, IDirect3DSurface9 *dst, const D3DSURFACE_DESC9 *dd,
                         const POINT *point) {
  const LONG w = rect->right - rect->left, h = rect->bottom - rect->top;
  const RECT to = {point->x, point->y, point->x + w, point->y + h};
  if (sd->Pool != D3DPOOL_DEFAULT && dd->Pool == D3DPOOL_DEFAULT) return CALL9(D9(d), UpdateSurface, src, rect, dst, point);
  if (sd->Pool == D3DPOOL_DEFAULT && dd->Pool == D3DPOOL_DEFAULT) return CALL9(D9(d), StretchRect, src, rect, dst, &to, D3DTEXF_NONE);
  if (sd->Pool != D3DPOOL_DEFAULT) return copy_by_lock(src, rect, dst, point, sd->Format);
  /* out of the device into memory: a target's pixels through GetRenderTargetData */
  if (!(sd->Usage & D3DUSAGE_RENDERTARGET) || dd->Pool != D3DPOOL_SYSTEMMEM) return D3DERR_INVALIDCALL;
  if (!rect->left && !rect->top && !point->x && !point->y && (UINT)w == sd->Width && (UINT)h == sd->Height && sd->Width == dd->Width && sd->Height == dd->Height)
    return CALL9(D9(d), GetRenderTargetData, src, dst);
  IDirect3DSurface9 *whole = NULL;
  HRESULT hr = CALL9(D9(d), CreateOffscreenPlainSurface, sd->Width, sd->Height, sd->Format, D3DPOOL_SYSTEMMEM, &whole, NULL);
  if (SUCCEEDED(hr)) hr = CALL9(D9(d), GetRenderTargetData, src, whole);
  if (SUCCEEDED(hr)) hr = copy_by_lock(whole, rect, dst, point, sd->Format);
  if (whole) CALL9(whole, Release);
  return hr;
}

static HRESULT STDMETHODCALLTYPE dev_CopyRects(Device8 *d, IDirect3DSurface8 *src8, const RECT *rects, UINT count, IDirect3DSurface8 *dst8, const POINT *points) {
  BOOL bad = FALSE;
  IDirect3DSurface9 *src = (IDirect3DSurface9 *)unwrap(d, src8, KIND_SURFACE, &bad), *dst = (IDirect3DSurface9 *)unwrap(d, dst8, KIND_SURFACE, &bad);
  if (!src || !dst || bad || src == dst) return D3DERR_INVALIDCALL;
  D3DSURFACE_DESC9 sd, dd;
  if (FAILED(CALL9(src, GetDesc, &sd)) || FAILED(CALL9(dst, GetDesc, &dd))) return D3DERR_INVALIDCALL;
  if (sd.Format != dd.Format || format_is_depth(sd.Format)) return D3DERR_INVALIDCALL;
  const RECT whole = {0, 0, (LONG)sd.Width, (LONG)sd.Height};
  const UINT n = rects ? count : 1;
  const UINT block = format_block_bytes(sd.Format) ? 4u : 1u;
  /* Every rectangle is checked before anything is copied. */
  for (UINT i = 0; i < n; ++i) {
    const RECT *r = rects ? &rects[i] : &whole;
    const POINT p = points ? points[i] : (POINT){r->left, r->top};
    if (r->left < 0 || r->top < 0 || r->left >= r->right || r->top >= r->bottom || r->right > (LONG)sd.Width || r->bottom > (LONG)sd.Height) return D3DERR_INVALIDCALL;
    const LONG w = r->right - r->left, h = r->bottom - r->top;
    if (p.x < 0 || p.y < 0 || p.x > (LONG)dd.Width - w || p.y > (LONG)dd.Height - h) return D3DERR_INVALIDCALL;
    if (block > 1 && ((r->left | r->top | p.x | p.y) & 3)) return D3DERR_INVALIDCALL;      /* (compressed: whole blocks) */
  }
  for (UINT i = 0; i < n; ++i) {
    const RECT *r = rects ? &rects[i] : &whole;
    const POINT p = points ? points[i] : (POINT){r->left, r->top};
    const HRESULT hr = copy_rect(d, src, &sd, r, dst, &dd, &p);
    if (FAILED(hr)) return hr;
  }
  return D3D_OK;
}
static HRESULT STDMETHODCALLTYPE dev_UpdateTexture(Device8 *d, IDirect3DBaseTexture8 *src8, IDirect3DBaseTexture8 *dst8) {
  BOOL bad = FALSE;
  IDirect3DBaseTexture9 *src = (IDirect3DBaseTexture9 *)unwrap(d, src8, KIND_TEXTURE, &bad), *dst = (IDirect3DBaseTexture9 *)unwrap(d, dst8, KIND_TEXTURE, &bad);
  if (!src || !dst || bad) return D3DERR_INVALIDCALL;
  return CALL9(D9(d), UpdateTexture, src, dst);
}
static HRESULT STDMETHODCALLTYPE dev_GetFrontBuffer(Device8 *d, IDirect3DSurface8 *dst8) {
  BOOL bad = FALSE;
  IDirect3DSurface9 *dst = (IDirect3DSurface9 *)unwrap(d, dst8, KIND_SURFACE, &bad);
  if (!dst || bad) return D3DERR_INVALIDCALL;
  return CALL9(D9(d), GetFrontBufferData, 0, dst);
}

/* ---- what is drawn into ---------------------------------------------------------------------------------------------------------------------------- */

/* SetRenderTarget(target, depth): a NULL target keeps the target that is set, a NULL depth buffer sets none. The depth buffer must be at least as
 * large as the target, and of the same multisampling. */
static HRESULT STDMETHODCALLTYPE dev_SetRenderTarget(Device8 *d, IDirect3DSurface8 *rt8, IDirect3DSurface8 *ds8) {
  BOOL bad = FALSE;
  IDirect3DSurface9 *rt = (IDirect3DSurface9 *)unwrap(d, rt8, KIND_SURFACE, &bad), *ds = (IDirect3DSurface9 *)unwrap(d, ds8, KIND_SURFACE, &bad);
  if (bad) return D3DERR_INVALIDCALL;
  EnterCriticalSection(&d->lock);
  IDirect3DSurface9 *current = NULL;
  HRESULT hr = D3D_OK;
  if (!rt) {
    hr = CALL9(D9(d), GetRenderTarget, 0, &current);
    rt = current;
  }
  D3DSURFACE_DESC9 rd, dd;
  if (SUCCEEDED(hr)) hr = rt ? CALL9(rt, GetDesc, &rd) : D3DERR_INVALIDCALL;
  if (SUCCEEDED(hr) && !(rd.Usage & D3DUSAGE_RENDERTARGET)) hr = D3DERR_INVALIDCALL;
  if (SUCCEEDED(hr) && ds) {
    hr = CALL9(ds, GetDesc, &dd);
    if (SUCCEEDED(hr) && (!(dd.Usage & D3DUSAGE_DEPTHSTENCIL) || dd.Width < rd.Width || dd.Height < rd.Height || dd.MultiSampleType != rd.MultiSampleType)) hr = D3DERR_INVALIDCALL;
  }
  if (SUCCEEDED(hr) && rt8) hr = CALL9(D9(d), SetRenderTarget, 0, rt);
  if (SUCCEEDED(hr)) hr = CALL9(D9(d), SetDepthStencilSurface, ds);
  /* (D3DRS_ZBIAS is in units of the depth buffer: of this one now) */
  if (SUCCEEDED(hr) && d->state.extra[EXTRA_ZBIAS]) extra_apply(d, EXTRA_ZBIAS, d->state.extra[EXTRA_ZBIAS]);
  LeaveCriticalSection(&d->lock);
  if (current) CALL9(current, Release);
  return hr;
}
static HRESULT STDMETHODCALLTYPE dev_GetRenderTarget(Device8 *d, IDirect3DSurface8 **out) {
  if (!out) return D3DERR_INVALIDCALL;
  *out = NULL;
  IDirect3DSurface9 *s9 = NULL;
  const HRESULT hr = CALL9(D9(d), GetRenderTarget, 0, &s9);
  if (FAILED(hr)) return hr;
  Object8 *s = wrap9(d, (IUnknown *)s9, KIND_SURFACE);
  if (!s) return E_OUTOFMEMORY;
  *out = (IDirect3DSurface8 *)s;
  return D3D_OK;
}
static HRESULT STDMETHODCALLTYPE dev_GetDepthStencilSurface(Device8 *d, IDirect3DSurface8 **out) {
  if (!out) return D3DERR_INVALIDCALL;
  *out = NULL;
  IDirect3DSurface9 *s9 = NULL;
  const HRESULT hr = CALL9(D9(d), GetDepthStencilSurface, &s9);
  if (FAILED(hr)) return hr;
  if (!s9) return D3DERR_NOTFOUND;
  Object8 *s = wrap9(d, (IUnknown *)s9, KIND_SURFACE);
  if (!s) return E_OUTOFMEMORY;
  *out = (IDirect3DSurface8 *)s;
  return D3D_OK;
}

/* ---- scenes, clears, the fixed-function state that is the same ------------------------------------------------------------------------------------- */

static HRESULT STDMETHODCALLTYPE dev_BeginScene(Device8 *d) { return CALL9(D9(d), BeginScene); }
static HRESULT STDMETHODCALLTYPE dev_EndScene(Device8 *d) { return CALL9(D9(d), EndScene); }
static HRESULT STDMETHODCALLTYPE dev_Clear(Device8 *d, DWORD count, const D3DRECT *rects, DWORD flags, D3DCOLOR colour, float z, DWORD stencil) {
  return CALL9(D9(d), Clear, count, rects, flags, colour, z, stencil);
}
static HRESULT STDMETHODCALLTYPE dev_SetTransform(Device8 *d, D3DTRANSFORMSTATETYPE state, const D3DMATRIX *matrix) {
  if (!matrix) return D3DERR_INVALIDCALL;
  return CALL9(D9(d), SetTransform, state, matrix);
}
static HRESULT STDMETHODCALLTYPE dev_GetTransform(Device8 *d, D3DTRANSFORMSTATETYPE state, D3DMATRIX *matrix) {
  if (!matrix) return D3DERR_INVALIDCALL;
  return CALL9(D9(d), GetTransform, state, matrix);
}
static HRESULT STDMETHODCALLTYPE dev_MultiplyTransform(Device8 *d, D3DTRANSFORMSTATETYPE state, const D3DMATRIX *matrix) {
  if (!matrix) return D3DERR_INVALIDCALL;
  return CALL9(D9(d), MultiplyTransform, state, matrix);
}
static HRESULT STDMETHODCALLTYPE dev_SetViewport(Device8 *d, const D3DVIEWPORT8 *viewport) {
  if (!viewport) return D3DERR_INVALIDCALL;
  return CALL9(D9(d), SetViewport, viewport);
}
static HRESULT STDMETHODCALLTYPE dev_GetViewport(Device8 *d, D3DVIEWPORT8 *viewport) {
  if (!viewport) return D3DERR_INVALIDCALL;
  return CALL9(D9(d), GetViewport, viewport);
}
static HRESULT STDMETHODCALLTYPE dev_SetMaterial(Device8 *d, const D3DMATERIAL8 *material) {
  if (!material) return D3DERR_INVALIDCALL;
  return CALL9(D9(d), SetMaterial, material);
}
static HRESULT STDMETHODCALLTYPE dev_GetMaterial(Device8 *d, D3DMATERIAL8 *material) {
  if (!material) return D3DERR_INVALIDCALL;
  return CALL9(D9(d), GetMaterial, material);
}
static HRESULT STDMETHODCALLTYPE dev_SetLight(Device8 *d, DWORD index, const D3DLIGHT8 *light) {
  if (!light) return D3DERR_INVALIDCALL;
  return CALL9(D9(d), SetLight, index, light);
}
static HRESULT STDMETHODCALLTYPE dev_GetLight(Device8 *d, DWORD index, D3DLIGHT8 *light) {
  if (!light) return D3DERR_INVALIDCALL;
  return CALL9(D9(d), GetLight, index, light);
}
static HRESULT STDMETHODCALLTYPE dev_LightEnable(Device8 *d, DWORD index, BOOL enable) { return CALL9(D9(d), LightEnable, index, enable); }
static HRESULT STDMETHODCALLTYPE dev_GetLightEnable(Device8 *d, DWORD index, BOOL *enable) {
  if (!enable) return D3DERR_INVALIDCALL;
  return CALL9(D9(d), GetLightEnable, index, enable);
}
static HRESULT STDMETHODCALLTYPE dev_SetClipPlane(Device8 *d, DWORD index, const float *plane) {
  if (!plane) return D3DERR_INVALIDCALL;
  return CALL9(D9(d), SetClipPlane, index, plane);
}
static HRESULT STDMETHODCALLTYPE dev_GetClipPlane(Device8 *d, DWORD index, float *plane) {
  if (!plane) return D3DERR_INVALIDCALL;
  return CALL9(D9(d), GetClipPlane, index, plane);
}
static HRESULT STDMETHODCALLTYPE dev_SetClipStatus(Device8 *d, const D3DCLIPSTATUS8 *status) {
  if (!status) return D3DERR_INVALIDCALL;
  return CALL9(D9(d), SetClipStatus, status);
}
static HRESULT STDMETHODCALLTYPE dev_GetClipStatus(Device8 *d, D3DCLIPSTATUS8 *status) {
  if (!status) return D3DERR_INVALIDCALL;
  return CALL9(D9(d), GetClipStatus, status);
}

static HRESULT STDMETHODCALLTYPE dev_GetTexture(Device8 *d, DWORD stage, IDirect3DBaseTexture8 **out) {
  if (!out) return D3DERR_INVALIDCALL;
  *out = NULL;
  if (stage >= STAGES8) return D3DERR_INVALIDCALL;
  IDirect3DBaseTexture9 *t9 = NULL;
  const HRESULT hr = CALL9(D9(d), GetTexture, stage, &t9);
  if (FAILED(hr) || !t9) return hr;
  Object8 *t = wrap9(d, (IUnknown *)t9, KIND_TEXTURE);
  if (!t) return E_OUTOFMEMORY;
  *out = (IDirect3DBaseTexture8 *)t;
  return D3D_OK;
}
static HRESULT STDMETHODCALLTYPE dev_SetTexture(Device8 *d, DWORD stage, IDirect3DBaseTexture8 *texture) {
  if (stage >= STAGES8) return D3DERR_INVALIDCALL;
  BOOL bad = FALSE;
  IDirect3DBaseTexture9 *t9 = (IDirect3DBaseTexture9 *)unwrap(d, texture, KIND_TEXTURE, &bad);
  if (bad) return D3DERR_INVALIDCALL;
  return CALL9(D9(d), SetTexture, stage, t9);
}
static HRESULT STDMETHODCALLTYPE dev_ValidateDevice(Device8 *d, DWORD *passes) {
  if (!passes) return D3DERR_INVALIDCALL;
  return CALL9(D9(d), ValidateDevice, passes);
}
/* GetInfo: the vertex cache, resource manager and vertex statistics (4 .. 6) are what the retail runtime of Direct3D 8 has no numbers of (S_FALSE),
 * the texture manager and texturing ones (1 .. 3) are the debug runtime's only (E_FAIL), and other numbers are none. */
static HRESULT STDMETHODCALLTYPE dev_GetInfo(Device8 *d, DWORD id, void *data, DWORD size) {
  (void)d; (void)data; (void)size;
  if (id >= D3DDEVINFOID_VCACHE && id <= D3DDEVINFOID_VERTEXSTATS) return S_FALSE;
  return E_FAIL;
}
static HRESULT STDMETHODCALLTYPE dev_SetPaletteEntries(Device8 *d, UINT palette, const PALETTEENTRY *entries) {
  if (!entries) return D3DERR_INVALIDCALL;
  return CALL9(D9(d), SetPaletteEntries, palette, entries);
}
static HRESULT STDMETHODCALLTYPE dev_GetPaletteEntries(Device8 *d, UINT palette, PALETTEENTRY *entries) {
  if (!entries) return D3DERR_INVALIDCALL;
  return CALL9(D9(d), GetPaletteEntries, palette, entries);
}
static HRESULT STDMETHODCALLTYPE dev_SetCurrentTexturePalette(Device8 *d, UINT palette) { return CALL9(D9(d), SetCurrentTexturePalette, palette); }
static HRESULT STDMETHODCALLTYPE dev_GetCurrentTexturePalette(Device8 *d, UINT *palette) {
  if (!palette) return D3DERR_INVALIDCALL;
  return CALL9(D9(d), GetCurrentTexturePalette, palette);
}

/* ---- streams, indices and draws -------------------------------------------------------------------------------------------------------------------- */

static HRESULT STDMETHODCALLTYPE dev_SetStreamSource(Device8 *d, UINT stream, IDirect3DVertexBuffer8 *buffer, UINT stride) {
  if (stream >= STREAMS8) return D3DERR_INVALIDCALL;
  BOOL bad = FALSE;
  IDirect3DVertexBuffer9 *b9 = (IDirect3DVertexBuffer9 *)unwrap(d, buffer, KIND_VERTEX_BUFFER, &bad);
  if (bad) return D3DERR_INVALIDCALL;
  if (!b9) {
    /* (no buffer: Direct3D 8 keeps the stride that was set) */
    IDirect3DVertexBuffer9 *old = NULL;
    UINT offset = 0, old_stride = 0;
    if (SUCCEEDED(CALL9(D9(d), GetStreamSource, stream, &old, &offset, &old_stride))) stride = old_stride;
    if (old) CALL9(old, Release);
  }
  return CALL9(D9(d), SetStreamSource, stream, b9, 0, stride);
}
static HRESULT STDMETHODCALLTYPE dev_GetStreamSource(Device8 *d, UINT stream, IDirect3DVertexBuffer8 **out, UINT *stride) {
  if (!out || !stride) return D3DERR_INVALIDCALL;
  *out = NULL;
  if (stream >= STREAMS8) return D3DERR_INVALIDCALL;
  IDirect3DVertexBuffer9 *b9 = NULL;
  UINT offset = 0;
  const HRESULT hr = CALL9(D9(d), GetStreamSource, stream, &b9, &offset, stride);
  if (FAILED(hr) || !b9) return hr;
  Object8 *b = wrap9(d, (IUnknown *)b9, KIND_VERTEX_BUFFER);
  if (!b) return E_OUTOFMEMORY;
  *out = (IDirect3DVertexBuffer8 *)b;
  return D3D_OK;
}
static HRESULT STDMETHODCALLTYPE dev_SetIndices(Device8 *d, IDirect3DIndexBuffer8 *buffer, UINT base_vertex) {
  BOOL bad = FALSE;
  IDirect3DIndexBuffer9 *b9 = (IDirect3DIndexBuffer9 *)unwrap(d, buffer, KIND_INDEX_BUFFER, &bad);
  if (bad) return D3DERR_INVALIDCALL;
  EnterCriticalSection(&d->lock);
  const HRESULT hr = CALL9(D9(d), SetIndices, b9);
  if (SUCCEEDED(hr)) state_target(d, HAS_BASE)->base_vertex = base_vertex;
  LeaveCriticalSection(&d->lock);
  return hr;
}
static HRESULT STDMETHODCALLTYPE dev_GetIndices(Device8 *d, IDirect3DIndexBuffer8 **out, UINT *base_vertex) {
  if (!out || !base_vertex) return D3DERR_INVALIDCALL;
  *out = NULL;
  IDirect3DIndexBuffer9 *b9 = NULL;
  const HRESULT hr = CALL9(D9(d), GetIndices, &b9);
  if (FAILED(hr)) return hr;
  *base_vertex = d->state.base_vertex;
  if (!b9) return D3D_OK;
  Object8 *b = wrap9(d, (IUnknown *)b9, KIND_INDEX_BUFFER);
  if (!b) return E_OUTOFMEMORY;
  *out = (IDirect3DIndexBuffer8 *)b;
  return D3D_OK;
}

/* A draw of no primitives draws nothing and succeeds, before anything is looked at (and the UP draws leave the streams as they are then). */
static HRESULT STDMETHODCALLTYPE dev_DrawPrimitive(Device8 *d, D3DPRIMITIVETYPE type, UINT start, UINT count) {
  if (!count) return D3D_OK;
  return CALL9(D9(d), DrawPrimitive, type, start, count);
}
/* (Direct3D 8's indices count from SetIndices' base vertex index: Direct3D 9's BaseVertexIndex of the draw) */
static HRESULT STDMETHODCALLTYPE dev_DrawIndexedPrimitive(Device8 *d, D3DPRIMITIVETYPE type, UINT min_index, UINT vertices, UINT start_index, UINT count) {
  if (!count) return D3D_OK;
  return CALL9(D9(d), DrawIndexedPrimitive, type, (INT)d->state.base_vertex, min_index, vertices, start_index, count);
}
static HRESULT STDMETHODCALLTYPE dev_DrawPrimitiveUP(Device8 *d, D3DPRIMITIVETYPE type, UINT count, const void *data, UINT stride) {
  if (!count) return D3D_OK;
  if (!data) return D3DERR_INVALIDCALL;
  return CALL9(D9(d), DrawPrimitiveUP, type, count, data, stride);
}
static HRESULT STDMETHODCALLTYPE dev_DrawIndexedPrimitiveUP(Device8 *d, D3DPRIMITIVETYPE type, UINT min_index, UINT vertices, UINT count, const void *indices,
                                                            D3DFORMAT format, const void *data, UINT stride) {
  if (!count) return D3D_OK;
  if (!indices || !data) return D3DERR_INVALIDCALL;
  const HRESULT hr = CALL9(D9(d), DrawIndexedPrimitiveUP, type, min_index, vertices, count, indices, format, data, stride);
  /* (the draw leaves no index buffer set, as in Direct3D 9, and with it no base vertex index) */
  if (SUCCEEDED(hr)) d->state.base_vertex = 0;
  return hr;
}
static HRESULT STDMETHODCALLTYPE dev_ProcessVertices(Device8 *d, UINT src_start, UINT dst_index, UINT count, IDirect3DVertexBuffer8 *dst8, DWORD flags) {
  BOOL bad = FALSE;
  IDirect3DVertexBuffer9 *dst = (IDirect3DVertexBuffer9 *)unwrap(d, dst8, KIND_VERTEX_BUFFER, &bad);
  if (!dst || bad) return D3DERR_INVALIDCALL;
  return CALL9(D9(d), ProcessVertices, src_start, dst_index, count, dst, NULL, flags);
}
static HRESULT STDMETHODCALLTYPE dev_DrawRectPatch(Device8 *d, UINT handle, const float *segments, const D3DRECTPATCH_INFO *info) {
  return CALL9(D9(d), DrawRectPatch, handle, segments, info);
}
static HRESULT STDMETHODCALLTYPE dev_DrawTriPatch(Device8 *d, UINT handle, const float *segments, const D3DTRIPATCH_INFO *info) {
  return CALL9(D9(d), DrawTriPatch, handle, segments, info);
}
static HRESULT STDMETHODCALLTYPE dev_DeletePatch(Device8 *d, UINT handle) { return CALL9(D9(d), DeletePatch, handle); }
