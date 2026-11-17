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

/* Part of d3d9.c: what a program makes on a device - vertex and index buffers, textures and their surfaces, vertex declarations - and the pixel
 * formats. */

/* ---- what every resource has ------------------------------------------------------------------------------------------------------------------------ */

static DWORD STDMETHODCALLTYPE res9_SetPriority(Child *c, DWORD priority) { (void)c; (void)priority; return 0; }
static DWORD STDMETHODCALLTYPE res9_GetPriority(Child *c) { (void)c; return 0; }
static void STDMETHODCALLTYPE res9_PreLoad(Child *c) { (void)c; }

/* ---- pixel formats ---------------------------------------------------------------------------------------------------------------------------------- */

/* The bytes of a pixel of a format a texture or surface may have; 0 for one this runtime has not. */
static UINT format_bytes(D3DFORMAT format) {
  switch (format) {
    case D3DFMT_A8R8G8B8: case D3DFMT_X8R8G8B8: return 4;
    case D3DFMT_R8G8B8: return 3;
    case D3DFMT_R5G6B5: case D3DFMT_X1R5G5B5: case D3DFMT_A1R5G5B5: case D3DFMT_A4R4G4B4: case D3DFMT_A8L8: return 2;
    case D3DFMT_A8: case D3DFMT_L8: return 1;
    default: return 0;
  }
}
static BOOL format_is_depth(D3DFORMAT format) {
  return format == D3DFMT_D16 || format == D3DFMT_D24S8 || format == D3DFMT_D24X8 || format == D3DFMT_D32 || format == D3DFMT_D16_LOCKABLE || format == D3DFMT_D24X4S4 ||
         format == D3DFMT_D15S1;
}
/* A row of pixels of `format` as B8G8R8A8, the one format the Direct3D 12 textures have. */
static void row_to_bgra(D3DFORMAT format, const BYTE *in, UINT pixels, BYTE *out) {
  for (UINT i = 0; i < pixels; ++i, out += 4) {
    UINT v;
    switch (format) {
      case D3DFMT_A8R8G8B8: memcpy(out, in + 4 * i, 4); break;
      case D3DFMT_X8R8G8B8: memcpy(out, in + 4 * i, 3); out[3] = 255; break;
      case D3DFMT_R8G8B8: memcpy(out, in + 3 * i, 3); out[3] = 255; break;
      case D3DFMT_R5G6B5:
        v = in[2 * i] | (UINT)in[2 * i + 1] << 8;
        out[0] = (BYTE)((v & 31) * 255 / 31); out[1] = (BYTE)(((v >> 5) & 63) * 255 / 63); out[2] = (BYTE)((v >> 11) * 255 / 31); out[3] = 255;
        break;
      case D3DFMT_X1R5G5B5: case D3DFMT_A1R5G5B5:
        v = in[2 * i] | (UINT)in[2 * i + 1] << 8;
        out[0] = (BYTE)((v & 31) * 255 / 31); out[1] = (BYTE)(((v >> 5) & 31) * 255 / 31); out[2] = (BYTE)(((v >> 10) & 31) * 255 / 31);
        out[3] = format == D3DFMT_X1R5G5B5 || (v & 0x8000) ? 255 : 0;
        break;
      case D3DFMT_A4R4G4B4:
        v = in[2 * i] | (UINT)in[2 * i + 1] << 8;
        out[0] = (BYTE)((v & 15) * 17); out[1] = (BYTE)(((v >> 4) & 15) * 17); out[2] = (BYTE)(((v >> 8) & 15) * 17); out[3] = (BYTE)((v >> 12) * 17);
        break;
      case D3DFMT_A8L8: out[0] = out[1] = out[2] = in[2 * i]; out[3] = in[2 * i + 1]; break;
      case D3DFMT_L8: out[0] = out[1] = out[2] = in[i]; out[3] = 255; break;
      case D3DFMT_A8: out[0] = out[1] = out[2] = 0; out[3] = in[i]; break;
      default: out[0] = out[1] = out[2] = out[3] = 255; break;
    }
  }
}

/* ---- vertex and index buffers ----------------------------------------------------------------------------------------------------------------------- */

static void buffer_destroy(DxObj *o) {
  mem_free(((Buffer *)o)->data);
  child_destroy(o);
}
static HRESULT buffer_make(Dev *d, UINT length, DWORD usage, DWORD fvf, D3DFORMAT format, D3DPOOL pool, const void *vtbl, const GUID *const *iids, void **out) {
  if (!out) return D3DERR_INVALIDCALL;
  *out = NULL;
  if (length == 0 || length > (256u << 20)) return D3DERR_INVALIDCALL;
  Buffer *b = child_new(sizeof *b, d, vtbl, iids, buffer_destroy);
  if (!b) return E_OUTOFMEMORY;
  b->data = mem_alloc(length);
  if (!b->data) { dx_Release(OBJ(b)); return E_OUTOFMEMORY; }
  b->length = length;
  b->usage = usage;
  b->fvf = fvf;
  b->format = format;
  b->pool = pool;
  *out = b;
  return D3D_OK;
}
static HRESULT STDMETHODCALLTYPE buf_Lock(Buffer *b, UINT offset, UINT size, void **data, DWORD flags) {
  (void)size; (void)flags;
  if (!data) return D3DERR_INVALIDCALL;
  *data = NULL;
  if (offset >= b->length && !(offset == 0 && b->length == 0)) return D3DERR_INVALIDCALL;
  *data = b->data + offset;
  return D3D_OK;
}
static HRESULT STDMETHODCALLTYPE buf_Unlock(Buffer *b) { (void)b; return D3D_OK; }
static D3DRESOURCETYPE STDMETHODCALLTYPE vb_GetType(Buffer *b) { (void)b; return D3DRTYPE_VERTEXBUFFER; }
static D3DRESOURCETYPE STDMETHODCALLTYPE ib_GetType(Buffer *b) { (void)b; return D3DRTYPE_INDEXBUFFER; }
static HRESULT STDMETHODCALLTYPE vb_GetDesc(Buffer *b, D3DVERTEXBUFFER_DESC *desc) {
  if (!desc) return D3DERR_INVALIDCALL;
  desc->Format = D3DFMT_VERTEXDATA;
  desc->Type = D3DRTYPE_VERTEXBUFFER;
  desc->Usage = b->usage;
  desc->Pool = b->pool;
  desc->Size = b->length;
  desc->FVF = b->fvf;
  return D3D_OK;
}
static HRESULT STDMETHODCALLTYPE ib_GetDesc(Buffer *b, D3DINDEXBUFFER_DESC *desc) {
  if (!desc) return D3DERR_INVALIDCALL;
  desc->Format = b->format;
  desc->Type = D3DRTYPE_INDEXBUFFER;
  desc->Usage = b->usage;
  desc->Pool = b->pool;
  desc->Size = b->length;
  return D3D_OK;
}
static HRESULT STDMETHODCALLTYPE dev_CreateVertexBuffer(Dev *d, UINT length, DWORD usage, DWORD fvf, D3DPOOL pool, IDirect3DVertexBuffer9 **out, HANDLE *shared) {
  if (shared) return D3DERR_INVALIDCALL;
  return buffer_make(d, length, usage, fvf, D3DFMT_VERTEXDATA, pool, &vb_vtbl, g_vb_iids, (void **)out);
}
static HRESULT STDMETHODCALLTYPE dev_CreateIndexBuffer(Dev *d, UINT length, DWORD usage, D3DFORMAT format, D3DPOOL pool, IDirect3DIndexBuffer9 **out, HANDLE *shared) {
  if (shared || (format != D3DFMT_INDEX16 && format != D3DFMT_INDEX32)) return D3DERR_INVALIDCALL;
  return buffer_make(d, length, usage, 0, format, pool, &ib_vtbl, g_ib_iids, (void **)out);
}

/* ---- textures --------------------------------------------------------------------------------------------------------------------------------------- */

static UINT level_size(UINT size, UINT level) { size >>= level; return size ? size : 1; }

/* Sends a level's pixels (the program's copy) to the Direct3D 12 texture. The list is sent with it. */
static HRESULT tex_upload(Dev *d, Tex *t, UINT level) {
  if (!t->res || t->depth || !t->shadow[level]) return D3D_OK;
  const UINT w = level_size(t->width, level), h = level_size(t->height, level), bytes = format_bytes(t->format);
  const UINT row_pitch = (w * 4 + 255u) & ~255u;
  ID3D12Resource *upload = make_buffer(d, D3D12_HEAP_TYPE_UPLOAD, (UINT64)row_pitch * h);
  BYTE *memory = NULL;
  if (!upload || FAILED(ID3D12Resource_Map(upload, 0, NULL, (void **)&memory)) || !memory) { if (upload) ID3D12Resource_Release(upload); return E_OUTOFMEMORY; }
  for (UINT row = 0; row < h; ++row) row_to_bgra(t->format, t->shadow[level] + (SIZE_T)row * w * bytes, w, memory + (SIZE_T)row * row_pitch);
  ID3D12Resource_Unmap(upload, 0, NULL);
  D3D12_TEXTURE_COPY_LOCATION to, from;
  memset(&to, 0, sizeof to);
  memset(&from, 0, sizeof from);
  to.pResource = t->res;
  to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  to.SubresourceIndex = level;
  from.pResource = upload;
  from.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
  from.PlacedFootprint.Footprint.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  from.PlacedFootprint.Footprint.Width = w;
  from.PlacedFootprint.Footprint.Height = h;
  from.PlacedFootprint.Footprint.Depth = 1;
  from.PlacedFootprint.Footprint.RowPitch = row_pitch;
  ID3D12GraphicsCommandList_CopyTextureRegion(d->list, &to, 0, 0, 0, &from, NULL);
  d->recorded = 1;
  flush(d);
  ID3D12Resource_Release(upload);
  return D3D_OK;
}
/* Fetches a level of a Direct3D 12 texture as rows of B8G8R8A8, `pitch` bytes apart. */
static HRESULT tex_download(Dev *d, Tex *t, UINT level, BYTE *rows, UINT pitch) {
  if (!t->res || t->depth) return D3DERR_INVALIDCALL;
  const UINT w = level_size(t->width, level), h = level_size(t->height, level);
  const UINT row_pitch = (w * 4 + 255u) & ~255u;
  ID3D12Resource *readback = make_buffer(d, D3D12_HEAP_TYPE_READBACK, (UINT64)row_pitch * h);
  if (!readback) return E_OUTOFMEMORY;
  D3D12_TEXTURE_COPY_LOCATION to, from;
  memset(&to, 0, sizeof to);
  memset(&from, 0, sizeof from);
  to.pResource = readback;
  to.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
  to.PlacedFootprint.Footprint.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  to.PlacedFootprint.Footprint.Width = w;
  to.PlacedFootprint.Footprint.Height = h;
  to.PlacedFootprint.Footprint.Depth = 1;
  to.PlacedFootprint.Footprint.RowPitch = row_pitch;
  from.pResource = t->res;
  from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  from.SubresourceIndex = level;
  ID3D12GraphicsCommandList_CopyTextureRegion(d->list, &to, 0, 0, 0, &from, NULL);
  d->recorded = 1;
  flush(d);
  const BYTE *memory = NULL;
  HRESULT hr = ID3D12Resource_Map(readback, 0, NULL, (void **)&memory);
  if (SUCCEEDED(hr) && memory) {
    for (UINT row = 0; row < h; ++row) memcpy(rows + (SIZE_T)row * pitch, memory + (SIZE_T)row * row_pitch, (SIZE_T)w * 4);
    ID3D12Resource_Unmap(readback, 0, NULL);
  } else if (SUCCEEDED(hr)) hr = E_FAIL;
  ID3D12Resource_Release(readback);
  return hr;
}

static void tex_destroy(DxObj *o) {
  Tex *t = (Tex *)o;
  Dev *d = t->c.dev;
  settle(d, t->used);
  for (UINT i = 0; i < d->clear_count;) {      /* (a clear nothing drew after: it goes with the buffer) */
    if (d->clears[i].depth == t) d->clears[i] = d->clears[--d->clear_count]; else ++i;
  }
  if (t->target && d->rtv_free_count < RTV_SLOTS) d->rtv_free[d->rtv_free_count++] = t->rtv_slot;
  if (t->depth && d->dsv_free_count < DSV_SLOTS) d->dsv_free[d->dsv_free_count++] = t->dsv_slot;
  for (UINT i = 0; i < MAX_LEVELS; ++i) mem_free(t->shadow[i]);
  if (t->res) ID3D12Resource_Release(t->res);
  child_destroy(o);
}

/* A texture, or what stands behind a surface. `memory_only`: no Direct3D 12 texture (a system-memory surface). `existing`: the Direct3D 12 texture
 * to use (the back buffer), a reference is taken. */
static HRESULT tex_make(Dev *d, UINT width, UINT height, UINT levels, DWORD usage, D3DFORMAT format, D3DPOOL pool, BOOL memory_only, ID3D12Resource *existing, Tex **out) {
  *out = NULL;
  const BOOL depth = format_is_depth(format);
  if (width == 0 || height == 0 || width > 4096 || height > 4096 || (!depth && !format_bytes(format))) return D3DERR_INVALIDCALL;
  const BOOL target = !depth && (usage & D3DUSAGE_RENDERTARGET) != 0;
  if (depth && d->dsv_free_count == 0) return E_OUTOFMEMORY;
  if (target && d->rtv_free_count == 0) return E_OUTOFMEMORY;
  UINT full = 1;
  for (UINT size = width > height ? width : height; size > 1; size >>= 1) ++full;
  if (target || depth || (usage & D3DUSAGE_AUTOGENMIPMAP) || memory_only) levels = 1;      /* (a target has one level here: the kernel draws into those only) */
  else if (levels == 0 || levels > full) levels = full;
  Tex *t = child_new(sizeof *t, d, &tex_vtbl, g_tex_iids, tex_destroy);
  if (!t) return E_OUTOFMEMORY;
  t->width = width;
  t->height = height;
  t->levels = levels;
  t->format = format;
  t->usage = usage;
  t->pool = pool;
  if (existing) {
    ID3D12Resource_AddRef(existing);
    t->res = existing;
  } else if (!memory_only) {
    D3D12_HEAP_PROPERTIES props = {D3D12_HEAP_TYPE_DEFAULT, D3D12_CPU_PAGE_PROPERTY_UNKNOWN, D3D12_MEMORY_POOL_UNKNOWN, 1, 1};
    D3D12_RESOURCE_DESC rd = {D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, width, height, 1, (UINT16)levels,
                              depth ? (format == D3DFMT_D24S8 || format == D3DFMT_D24X4S4 || format == D3DFMT_D15S1 ? DXGI_FORMAT_D24_UNORM_S8_UINT : DXGI_FORMAT_D32_FLOAT)
                                    : DXGI_FORMAT_B8G8R8A8_UNORM,
                              {1, 0}, D3D12_TEXTURE_LAYOUT_UNKNOWN, depth ? D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL : target ? D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET : D3D12_RESOURCE_FLAG_NONE};
    const HRESULT hr = ID3D12Device_CreateCommittedResource(d->d12, &props, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COMMON, NULL, &IID_ID3D12Resource, (void **)&t->res);
    if (FAILED(hr) || !t->res) { dx_Release(OBJ(t)); return D3DERR_INVALIDCALL; }
  }
  /* (the slots are taken only now: tex_destroy gives back what the flags say was taken) */
  if (depth && t->res) {
    t->depth = 1;
    t->dsv_slot = d->dsv_free[--d->dsv_free_count];
    t->view = ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(d->dsv_heap);
    t->view.ptr += (SIZE_T)t->dsv_slot * ID3D12Device_GetDescriptorHandleIncrementSize(d->d12, D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
    ID3D12Device_CreateDepthStencilView(d->d12, t->res, NULL, t->view);
  } else if ((target || existing) && t->res) {
    t->target = 1;
    t->rtv_slot = d->rtv_free[--d->rtv_free_count];
    t->view = ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(d->rtv_heap);
    t->view.ptr += (SIZE_T)t->rtv_slot * ID3D12Device_GetDescriptorHandleIncrementSize(d->d12, D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    ID3D12Device_CreateRenderTargetView(d->d12, t->res, NULL, t->view);
  }
  *out = t;
  return D3D_OK;
}

/* Locks a level: the program gets its copy of the pixels (fetched first for something that is drawn into). */
static HRESULT tex_lock(Tex *t, UINT level, D3DLOCKED_RECT *out, const RECT *rect, DWORD flags) {
  Dev *d = t->c.dev;
  if (!out) return D3DERR_INVALIDCALL;
  out->pBits = NULL;
  out->Pitch = 0;
  const UINT bytes = format_bytes(t->format);
  if (level >= t->levels || t->depth || !bytes || (t->locked[level] & 0x80000000u)) return D3DERR_INVALIDCALL;
  const UINT w = level_size(t->width, level), h = level_size(t->height, level);
  if (rect && (rect->left < 0 || rect->top < 0 || rect->right > (LONG)w || rect->bottom > (LONG)h || rect->left >= rect->right || rect->top >= rect->bottom)) return D3DERR_INVALIDCALL;
  HRESULT hr = D3D_OK;
  EnterCriticalSection(&d->lock);
  if (!t->shadow[level]) t->shadow[level] = mem_alloc((SIZE_T)w * h * bytes);
  if (!t->shadow[level]) hr = E_OUTOFMEMORY;
  else if (t->target && t->res && !(flags & D3DLOCK_DISCARD)) hr = bytes == 4 ? tex_download(d, t, level, t->shadow[level], w * 4) : D3DERR_INVALIDCALL;
  LeaveCriticalSection(&d->lock);
  if (FAILED(hr)) return hr;
  t->locked[level] = flags | 0x80000000u;
  out->Pitch = (INT)(w * bytes);
  out->pBits = t->shadow[level] + (rect ? (SIZE_T)rect->top * w * bytes + (SIZE_T)rect->left * bytes : 0);
  return D3D_OK;
}
static HRESULT tex_unlock(Tex *t, UINT level) {
  Dev *d = t->c.dev;
  if (level >= t->levels || !(t->locked[level] & 0x80000000u)) return D3DERR_INVALIDCALL;
  const DWORD flags = t->locked[level];
  t->locked[level] = 0;
  if (flags & D3DLOCK_READONLY) return D3D_OK;
  EnterCriticalSection(&d->lock);
  const HRESULT hr = tex_upload(d, t, level);
  LeaveCriticalSection(&d->lock);
  return hr;
}
static void tex_describe(const Tex *t, UINT level, D3DSURFACE_DESC *desc) {
  desc->Format = t->format;
  desc->Type = D3DRTYPE_SURFACE;
  desc->Usage = t->usage;
  desc->Pool = t->pool;
  desc->MultiSampleType = D3DMULTISAMPLE_NONE;
  desc->MultiSampleQuality = 0;
  desc->Width = level_size(t->width, level);
  desc->Height = level_size(t->height, level);
}

static D3DRESOURCETYPE STDMETHODCALLTYPE tex_GetType(Tex *t) { (void)t; return D3DRTYPE_TEXTURE; }
static DWORD STDMETHODCALLTYPE tex_SetLOD(Tex *t, DWORD lod) { (void)t; (void)lod; return 0; }
static DWORD STDMETHODCALLTYPE tex_GetLOD(Tex *t) { (void)t; return 0; }
static DWORD STDMETHODCALLTYPE tex_GetLevelCount(Tex *t) { return t->levels; }
static HRESULT STDMETHODCALLTYPE tex_SetAutoGenFilterType(Tex *t, D3DTEXTUREFILTERTYPE type) { (void)t; (void)type; return D3D_OK; }
static D3DTEXTUREFILTERTYPE STDMETHODCALLTYPE tex_GetAutoGenFilterType(Tex *t) { (void)t; return D3DTEXF_LINEAR; }
static void STDMETHODCALLTYPE tex_GenerateMipSubLevels(Tex *t) { (void)t; }
static HRESULT STDMETHODCALLTYPE tex_GetLevelDesc(Tex *t, UINT level, D3DSURFACE_DESC *desc) {
  if (!desc || level >= t->levels) return D3DERR_INVALIDCALL;
  tex_describe(t, level, desc);
  return D3D_OK;
}
static HRESULT STDMETHODCALLTYPE tex_LockRect(Tex *t, UINT level, D3DLOCKED_RECT *out, const RECT *rect, DWORD flags) { return tex_lock(t, level, out, rect, flags); }
static HRESULT STDMETHODCALLTYPE tex_UnlockRect(Tex *t, UINT level) { return tex_unlock(t, level); }
static HRESULT STDMETHODCALLTYPE tex_AddDirtyRect(Tex *t, const RECT *rect) { (void)t; (void)rect; return D3D_OK; }

/* ---- surfaces --------------------------------------------------------------------------------------------------------------------------------------- */

static void surface_destroy(DxObj *o) {
  Surface *s = (Surface *)o;
  if (s->tex) dx_Release(OBJ(s->tex));
  child_destroy(o);
}
static Surface *surface_make(Dev *d, Tex *t, UINT level, int standalone) {
  Surface *s = child_new(sizeof *s, d, &surf_vtbl, g_surf_iids, surface_destroy);
  if (!s) return NULL;
  dx_AddRef(OBJ(t));
  s->tex = t;
  s->level = level;
  s->standalone = standalone;
  return s;
}
static HRESULT STDMETHODCALLTYPE tex_GetSurfaceLevel(Tex *t, UINT level, IDirect3DSurface9 **out) {
  if (!out) return D3DERR_INVALIDCALL;
  *out = NULL;
  if (level >= t->levels) return D3DERR_INVALIDCALL;
  *out = (IDirect3DSurface9 *)surface_make(t->c.dev, t, level, 0);
  return *out ? D3D_OK : E_OUTOFMEMORY;
}
static D3DRESOURCETYPE STDMETHODCALLTYPE surf_GetType(Surface *s) { (void)s; return D3DRTYPE_SURFACE; }
static HRESULT STDMETHODCALLTYPE surf_GetContainer(Surface *s, REFIID riid, void **out) {
  if (!out) return D3DERR_INVALIDCALL;
  *out = NULL;
  if (s->standalone) return guid_eq(riid, &IID_IDirect3DDevice9) || guid_eq(riid, &IID_IUnknown) ? dx_QueryInterface(OBJ(s->c.dev), riid, out) : E_NOINTERFACE;
  return dx_QueryInterface(OBJ(s->tex), riid, out);
}
static HRESULT STDMETHODCALLTYPE surf_GetDesc(Surface *s, D3DSURFACE_DESC *desc) {
  if (!desc) return D3DERR_INVALIDCALL;
  tex_describe(s->tex, s->level, desc);
  return D3D_OK;
}
static HRESULT STDMETHODCALLTYPE surf_LockRect(Surface *s, D3DLOCKED_RECT *out, const RECT *rect, DWORD flags) { return tex_lock(s->tex, s->level, out, rect, flags); }
static HRESULT STDMETHODCALLTYPE surf_UnlockRect(Surface *s) { return tex_unlock(s->tex, s->level); }
static HRESULT STDMETHODCALLTYPE surf_GetDC(Surface *s, HDC *dc) { (void)s; if (dc) *dc = NULL; return D3DERR_INVALIDCALL; }
static HRESULT STDMETHODCALLTYPE surf_ReleaseDC(Surface *s, HDC dc) { (void)s; (void)dc; return D3DERR_INVALIDCALL; }

static HRESULT STDMETHODCALLTYPE dev_CreateTexture(Dev *d, UINT width, UINT height, UINT levels, DWORD usage, D3DFORMAT format, D3DPOOL pool, IDirect3DTexture9 **out, HANDLE *shared) {
  if (!out) return D3DERR_INVALIDCALL;
  *out = NULL;
  if (shared) return D3DERR_INVALIDCALL;
  Tex *t = NULL;
  EnterCriticalSection(&d->lock);
  const HRESULT hr = tex_make(d, width, height, levels, usage, format, pool, FALSE, NULL, &t);
  LeaveCriticalSection(&d->lock);
  *out = (IDirect3DTexture9 *)t;
  return hr;
}
/* A surface on its own: a texture of one level nobody sees, and the surface of that level. */
static HRESULT surface_alone(Dev *d, UINT width, UINT height, DWORD usage, D3DFORMAT format, D3DPOOL pool, BOOL memory_only, IDirect3DSurface9 **out) {
  if (!out) return D3DERR_INVALIDCALL;
  *out = NULL;
  Tex *t = NULL;
  EnterCriticalSection(&d->lock);
  const HRESULT hr = tex_make(d, width, height, 1, usage, format, pool, memory_only, NULL, &t);
  LeaveCriticalSection(&d->lock);
  if (FAILED(hr)) return hr;
  Surface *s = surface_make(d, t, 0, 1);
  dx_Release(OBJ(t));
  *out = (IDirect3DSurface9 *)s;
  return s ? D3D_OK : E_OUTOFMEMORY;
}
static HRESULT STDMETHODCALLTYPE dev_CreateRenderTarget(Dev *d, UINT width, UINT height, D3DFORMAT format, D3DMULTISAMPLE_TYPE samples, DWORD quality, BOOL lockable, IDirect3DSurface9 **out,
                                                        HANDLE *shared) {
  (void)quality; (void)lockable;
  if (shared || samples > D3DMULTISAMPLE_NONMASKABLE) return D3DERR_INVALIDCALL;
  return surface_alone(d, width, height, D3DUSAGE_RENDERTARGET, format, D3DPOOL_DEFAULT, FALSE, out);
}
static HRESULT STDMETHODCALLTYPE dev_CreateDepthStencilSurface(Dev *d, UINT width, UINT height, D3DFORMAT format, D3DMULTISAMPLE_TYPE samples, DWORD quality, BOOL discard,
                                                               IDirect3DSurface9 **out, HANDLE *shared) {
  (void)quality; (void)discard;
  if (shared || samples > D3DMULTISAMPLE_NONMASKABLE || !format_is_depth(format)) return D3DERR_INVALIDCALL;
  return surface_alone(d, width, height, D3DUSAGE_DEPTHSTENCIL, format, D3DPOOL_DEFAULT, FALSE, out);
}
static HRESULT STDMETHODCALLTYPE dev_CreateOffscreenPlainSurface(Dev *d, UINT width, UINT height, D3DFORMAT format, D3DPOOL pool, IDirect3DSurface9 **out, HANDLE *shared) {
  if (shared) return D3DERR_INVALIDCALL;
  return surface_alone(d, width, height, 0, format, pool, TRUE, out);
}

/* ---- vertex declarations ---------------------------------------------------------------------------------------------------------------------------- */

static const Attribute g_no_attribute = {0, 255, 0};

/* What the shaders take, out of a declaration's elements. */
static void layout_of_elements(const D3DVERTEXELEMENT9 *e, UINT count, Layout *l) {
  l->position = l->colour = l->specular = l->texture[0] = l->texture[1] = g_no_attribute;
  l->transformed = 0;
  for (UINT i = 0; i < count && e[i].Stream != 0xFF; ++i) {
    const Attribute a = {(BYTE)e[i].Stream, e[i].Type, e[i].Offset};
    if (e[i].Stream >= STREAMS) continue;
    if ((e[i].Usage == D3DDECLUSAGE_POSITION || e[i].Usage == D3DDECLUSAGE_POSITIONT) && e[i].UsageIndex == 0) {
      l->position = a;
      l->transformed = e[i].Usage == D3DDECLUSAGE_POSITIONT;
    } else if (e[i].Usage == D3DDECLUSAGE_COLOR && e[i].UsageIndex == 0) l->colour = a;
    else if (e[i].Usage == D3DDECLUSAGE_COLOR && e[i].UsageIndex == 1) l->specular = a;
    else if (e[i].Usage == D3DDECLUSAGE_TEXCOORD && e[i].UsageIndex < 2) l->texture[e[i].UsageIndex] = a;
  }
}
/* An FVF code as the elements of a declaration (the end marker is written, and not counted): the position, the blend weights, the normal, the
 * point size, the two colours and the texture coordinates follow each other in the vertex in that order. `e` has room for 16. */
static UINT fvf_elements(DWORD fvf, D3DVERTEXELEMENT9 *e) {
  UINT n = 0, offset = 0;
#define FVF_ELEMENT(type, usage, index, bytes) \
  do { \
    const D3DVERTEXELEMENT9 element = {0, (WORD)offset, (BYTE)(type), D3DDECLMETHOD_DEFAULT, (BYTE)(usage), (BYTE)(index)}; \
    e[n++] = element; \
    offset += (bytes); \
  } while (0)
  const DWORD position = fvf & D3DFVF_POSITION_MASK;
  if (position == D3DFVF_XYZRHW) FVF_ELEMENT(D3DDECLTYPE_FLOAT4, D3DDECLUSAGE_POSITIONT, 0, 16);
  else if (position == D3DFVF_XYZW) FVF_ELEMENT(D3DDECLTYPE_FLOAT4, D3DDECLUSAGE_POSITION, 0, 16);
  else if (position) {
    FVF_ELEMENT(D3DDECLTYPE_FLOAT3, D3DDECLUSAGE_POSITION, 0, 12);
    if (position >= D3DFVF_XYZB1) {
      const UINT weights = (position - D3DFVF_XYZB1) / 2 + 1;
      FVF_ELEMENT(D3DDECLTYPE_FLOAT1 + (weights > 4 ? 4 : weights) - 1, D3DDECLUSAGE_BLENDWEIGHT, 0, 4 * weights);
    }
  }
  if (fvf & D3DFVF_NORMAL) FVF_ELEMENT(D3DDECLTYPE_FLOAT3, D3DDECLUSAGE_NORMAL, 0, 12);
  if (fvf & D3DFVF_PSIZE) FVF_ELEMENT(D3DDECLTYPE_FLOAT1, D3DDECLUSAGE_PSIZE, 0, 4);
  if (fvf & D3DFVF_DIFFUSE) FVF_ELEMENT(D3DDECLTYPE_D3DCOLOR, D3DDECLUSAGE_COLOR, 0, 4);
  if (fvf & D3DFVF_SPECULAR) FVF_ELEMENT(D3DDECLTYPE_D3DCOLOR, D3DDECLUSAGE_COLOR, 1, 4);
  const UINT count = (fvf & D3DFVF_TEXCOUNT_MASK) >> D3DFVF_TEXCOUNT_SHIFT;
  for (UINT i = 0; i < count && i < 8; ++i) {
    static const BYTE floats[4] = {2, 3, 4, 1};      /* (the size code of a coordinate set: two bits a set, from bit 16) */
    const UINT size = floats[(fvf >> (16 + 2 * i)) & 3];
    FVF_ELEMENT(D3DDECLTYPE_FLOAT1 + size - 1, D3DDECLUSAGE_TEXCOORD, i, 4 * size);
  }
#undef FVF_ELEMENT
  const D3DVERTEXELEMENT9 end = D3DDECL_END();
  e[n] = end;
  return n;
}
static void layout_of_fvf(DWORD fvf, Layout *l) {
  D3DVERTEXELEMENT9 elements[16];
  layout_of_elements(elements, fvf_elements(fvf, elements), l);
}

static HRESULT STDMETHODCALLTYPE decl_GetDeclaration(Decl *decl, D3DVERTEXELEMENT9 *elements, UINT *count) {
  if (!count) return D3DERR_INVALIDCALL;
  *count = decl->count;
  if (elements) memcpy(elements, decl->elements, sizeof(D3DVERTEXELEMENT9) * decl->count);
  return D3D_OK;
}
static HRESULT STDMETHODCALLTYPE dev_CreateVertexDeclaration(Dev *d, const D3DVERTEXELEMENT9 *elements, IDirect3DVertexDeclaration9 **out) {
  if (!out) return D3DERR_INVALIDCALL;
  *out = NULL;
  if (!elements) return D3DERR_INVALIDCALL;
  UINT count = 0;
  while (count < MAX_ELEMENTS && elements[count].Stream != 0xFF) ++count;
  if (elements[count].Stream != 0xFF) return D3DERR_INVALIDCALL;
  Decl *decl = child_new(sizeof *decl, d, &decl_vtbl, g_decl_iids, child_destroy);
  if (!decl) return E_OUTOFMEMORY;
  memcpy(decl->elements, elements, sizeof(D3DVERTEXELEMENT9) * (count + 1));
  decl->count = count + 1;
  layout_of_elements(elements, count, &decl->layout);
  *out = (IDirect3DVertexDeclaration9 *)decl;
  return D3D_OK;
}
