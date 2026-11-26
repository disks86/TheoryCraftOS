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

/* Part of d3d9.c: what a program makes on a device - vertex and index buffers, textures and their levels (surfaces, volumes), surfaces of their own,
 * vertex declarations - and what every resource has (private data, priority). */

static const PALETTEENTRY *current_palette(Dev *d);

/* ---- what every resource has ------------------------------------------------------------------------------------------------------------------------ */

/* The pool of a resource (buffers, textures, surfaces, volumes). */
static D3DPOOL resource_pool(Child *c);
/* Private data, as Direct3D 9 has it: bytes, or (D3DSPD_IUNKNOWN) an interface that is held while it is there - `data` is the interface then, and the size that of
 * a pointer. A failed set leaves what was there. */
static HRESULT STDMETHODCALLTYPE res9_SetPrivateData(Child *c, REFGUID guid, const void *data, DWORD size, DWORD flags) {
  if (!guid) return D3DERR_INVALIDCALL;
  const BOOL unknown = (flags & D3DSPD_IUNKNOWN) != 0;
  if (unknown && size != sizeof(IUnknown *)) return D3DERR_INVALIDCALL;
  if (!data && size) return D3DERR_INVALIDCALL;
  DxPrivate *p = mem_alloc(sizeof *p);
  void *copy = NULL;
  if (!p) return E_OUTOFMEMORY;
  if (unknown) {
    copy = (void *)data;
    if (copy) ((IUnknown *)copy)->lpVtbl->AddRef((IUnknown *)copy);
  } else {
    copy = mem_alloc(size);
    if (!copy) { mem_free(p); return E_OUTOFMEMORY; }
    if (size) memcpy(copy, data, size);
  }
  EnterCriticalSection(&g_private_lock);
  DxPrivate **pp = dx_find_private(&c->o, guid);
  if (*pp) { DxPrivate *old = *pp; *pp = old->next; dx_drop_private(old); }
  p->guid = *guid;
  p->size = unknown ? 0xFFFFFFFFu : size;
  p->data = copy;
  p->next = c->o.priv;
  c->o.priv = p;
  LeaveCriticalSection(&g_private_lock);
  return D3D_OK;
}
static HRESULT STDMETHODCALLTYPE res9_GetPrivateData(Child *c, REFGUID guid, void *data, DWORD *size) {
  if (!guid) return D3DERR_INVALIDCALL;
  HRESULT hr = D3D_OK;
  EnterCriticalSection(&g_private_lock);
  DxPrivate *p = *dx_find_private(&c->o, guid);
  if (!p) hr = D3DERR_NOTFOUND;
  else if (!size) hr = D3DERR_INVALIDCALL;
  else {
    const DWORD needed = p->size == 0xFFFFFFFFu ? (DWORD)sizeof(IUnknown *) : p->size;
    if (!data) *size = needed;
    else if (*size < needed) { *size = needed; hr = D3DERR_MOREDATA; }
    else {
      if (p->size == 0xFFFFFFFFu) {
        *(IUnknown **)data = p->data;
        if (p->data) ((IUnknown *)p->data)->lpVtbl->AddRef((IUnknown *)p->data);
      } else memcpy(data, p->data, needed);
      *size = needed;
    }
  }
  LeaveCriticalSection(&g_private_lock);
  return hr;
}
static HRESULT STDMETHODCALLTYPE res9_FreePrivateData(Child *c, REFGUID guid) {
  if (!guid) return D3DERR_INVALIDCALL;
  EnterCriticalSection(&g_private_lock);
  DxPrivate **pp = dx_find_private(&c->o, guid);
  DxPrivate *p = *pp;
  if (p) *pp = p->next;
  LeaveCriticalSection(&g_private_lock);
  if (!p) return D3DERR_NOTFOUND;
  dx_drop_private(p);
  return D3D_OK;
}
/* The priority of a resource: only a managed one keeps one (it says what goes out of video memory first); the others answer 0. */
static DWORD STDMETHODCALLTYPE res9_SetPriority(Child *c, DWORD priority) {
  if (resource_pool(c) != D3DPOOL_MANAGED) return 0;
  const DWORD old = c->priority;
  c->priority = priority;
  return old;
}
static DWORD STDMETHODCALLTYPE res9_GetPriority(Child *c) { return resource_pool(c) == D3DPOOL_MANAGED ? c->priority : 0; }
/* (a managed resource is in the device's memory whenever it is set: there is nothing to bring in ahead) */
static void STDMETHODCALLTYPE res9_PreLoad(Child *c) { (void)c; }

/* Whether `pool` is one a resource may be made in (D3DPOOL_MANAGED not on a 9Ex device). */
static BOOL pool_valid(const Dev *d, D3DPOOL pool) {
  return pool == D3DPOOL_DEFAULT || pool == D3DPOOL_SYSTEMMEM || pool == D3DPOOL_SCRATCH || (pool == D3DPOOL_MANAGED && !d->ex);
}
/* The count of the program's resources in D3DPOOL_DEFAULT (Reset of a device that is not Ex fails while there are any). */
static void default_resource_made(Dev *d, D3DPOOL pool) { if (pool == D3DPOOL_DEFAULT) InterlockedIncrement(&d->default_resources); }
static void default_resource_gone(Dev *d, D3DPOOL pool) { if (pool == D3DPOOL_DEFAULT) InterlockedDecrement(&d->default_resources); }

/* ---- vertex and index buffers ----------------------------------------------------------------------------------------------------------------------- */

static void buffer_destroy(DxObj *o) {
  Buffer *b = (Buffer *)o;
  default_resource_gone(b->c.dev, b->pool);
  if (b->pool == D3DPOOL_DEFAULT || b->pool == D3DPOOL_MANAGED) b->c.dev->video_bytes -= b->length;
  mem_free(b->data);
}
static HRESULT buffer_make(Dev *d, UINT length, DWORD usage, DWORD fvf, D3DFORMAT format, D3DPOOL pool, const void *vtbl, const GUID *const *iids, void **out) {
  if (!out) return D3DERR_INVALIDCALL;
  *out = NULL;
  /* (scratch buffers are not there; a dynamic buffer is not managed) */
  if (length == 0 || length > (256u << 20) || !pool_valid(d, pool) || pool == D3DPOOL_SCRATCH || ((usage & D3DUSAGE_DYNAMIC) && pool == D3DPOOL_MANAGED)) return D3DERR_INVALIDCALL;
  if (usage & ~(D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY | D3DUSAGE_DONOTCLIP | D3DUSAGE_NPATCHES | D3DUSAGE_POINTS | D3DUSAGE_RTPATCHES | D3DUSAGE_SOFTWAREPROCESSING | D3DUSAGE_TEXTAPI))
    return D3DERR_INVALIDCALL;
  Buffer *b = child_new(sizeof *b, d, vtbl, iids, buffer_destroy);
  if (!b) return E_OUTOFMEMORY;
  b->data = mem_alloc(length);
  if (!b->data) { b->pool = D3DPOOL_SYSTEMMEM; obj_free(OBJ(b)); dx_Release(OBJ(d)); return E_OUTOFMEMORY; }
  b->length = length;
  b->usage = usage;
  b->fvf = fvf;
  b->format = format;
  b->pool = pool;
  default_resource_made(d, pool);
  if (pool == D3DPOOL_DEFAULT || pool == D3DPOOL_MANAGED) d->video_bytes += length;
  *out = b;
  return D3D_OK;
}
/* A buffer is the program's memory: a lock hands it out (an offset past the end, or a size that goes past it, gives the end; a size of 0 the rest).
 * The buffer counts its locks: each Unlock ends one. */
static HRESULT STDMETHODCALLTYPE buf_Lock(Buffer *b, UINT offset, UINT size, void **data, DWORD flags) {
  (void)flags;
  if (!data) return D3DERR_INVALIDCALL;
  *data = NULL;
  Dev *d = b->c.dev;
  EnterCriticalSection(&d->lock);
  /* (the draws recorded before read the buffer when the list is sent: they get what it had then) */
  if (d->recorded) flush(d);
  LeaveCriticalSection(&d->lock);
  (void)size;
  *data = b->data + (offset < b->length ? offset : b->length);
  ++b->lock_count;
  b->locked = TRUE;
  return D3D_OK;
}
static HRESULT STDMETHODCALLTYPE buf_Unlock(Buffer *b) {
  if (b->lock_count) --b->lock_count;
  b->locked = b->lock_count != 0;
  return D3D_OK;
}
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
static HRESULT shared_refused(Dev *d, HANDLE *shared, D3DPOOL pool);
static HRESULT STDMETHODCALLTYPE dev_CreateVertexBuffer(Dev *d, UINT length, DWORD usage, DWORD fvf, D3DPOOL pool, IDirect3DVertexBuffer9 **out, HANDLE *shared) {
  const HRESULT refused = shared_refused(d, shared, pool);
  if (FAILED(refused)) { if (out) *out = NULL; return refused; }
  return buffer_make(d, length, usage, fvf, D3DFMT_VERTEXDATA, pool, &vb_vtbl, g_vb_iids, (void **)out);
}
static HRESULT STDMETHODCALLTYPE dev_CreateIndexBuffer(Dev *d, UINT length, DWORD usage, D3DFORMAT format, D3DPOOL pool, IDirect3DIndexBuffer9 **out, HANDLE *shared) {
  const HRESULT refused = shared_refused(d, shared, pool);
  if (FAILED(refused) || (format != D3DFMT_INDEX16 && format != D3DFMT_INDEX32)) { if (out) *out = NULL; return FAILED(refused) ? refused : D3DERR_INVALIDCALL; }
  return buffer_make(d, length, usage, 0, format, pool, &ib_vtbl, g_ib_iids, (void **)out);
}

/* ---- textures: their pixels ------------------------------------------------------------------------------------------------------------------------- */

static UINT level_size(UINT size, UINT level) { size >>= level; return size ? size : 1; }
/* The subresource of a face's level, and the other way round. */
static UINT tex_sub(const Tex *t, UINT face, UINT level) { return face * t->levels + level; }
static UINT sub_level(const Tex *t, UINT sub) { return sub % t->levels; }
static UINT sub_face(const Tex *t, UINT sub) { return sub / t->levels; }
/* The number of subresources. */
static UINT tex_subresources(const Tex *t) { return t->levels * (t->kind == 3 ? 6 : 1); }
/* The slices of a subresource of a volume; 1 for the other kinds. */
static UINT tex_slices(const Tex *t, UINT sub) { return t->kind == 1 ? level_size(t->slices, sub_level(t, sub)) : 1; }
static UINT sub_width(const Tex *t, UINT sub) { return level_size(t->width, sub_level(t, sub)); }
static UINT sub_height(const Tex *t, UINT sub) { return level_size(t->height, sub_level(t, sub)); }
/* The bytes of the program's copy of a subresource. */
static SIZE_T tex_sub_bytes(const Tex *t, UINT sub) {
  return (SIZE_T)format_pitch(t->format, sub_width(t, sub)) * format_rows(t->format, sub_height(t, sub)) * tex_slices(t, sub);
}
/* What counts of a texture's pixels is on the device (it is drawn into, is a depth buffer, or is shared with another device): a lock fetches it. The
 * others' pixels are the program's copy, which is sent to the device when it changes. */
static BOOL tex_device_owned(const Tex *t) { return t->res && (t->target || t->depth || t->shared); }
/* The program's copy of a subresource, made (zeroed) if there was none. */
static BYTE *tex_shadow(Tex *t, UINT sub) {
  if (t->shadow[sub]) return t->shadow[sub];
  /* (the first copy made: one block for every subresource, each where the ones before it end; a texture whose copies were made one by one - the device's
   * own white ones, a 9Ex surface on the program's memory - goes on so) */
  BOOL none = !t->user_memory;
  for (UINT i = 0; i < MAX_SUBS && none; ++i) none = t->shadow[i] == NULL;
  const UINT subs = tex_subresources(t);
  if (none && subs <= MAX_SUBS) {
    /* (a cube's faces each begin on 16 bytes from the start, as on Windows) */
    SIZE_T total = 0;
    for (UINT i = 0; i < subs; ++i) {
      if (t->kind == 3 && i && sub_level(t, i) == 0) total = (total + 15) & ~(SIZE_T)15;
      total += tex_sub_bytes(t, i);
    }
    t->shadow_block = mem_alloc(total);
    if (!t->shadow_block) return NULL;
    SIZE_T at = 0;
    for (UINT i = 0; i < subs; ++i) {
      if (t->kind == 3 && i && sub_level(t, i) == 0) at = (at + 15) & ~(SIZE_T)15;
      t->shadow[i] = t->shadow_block + at;
      at += tex_sub_bytes(t, i);
    }
    return t->shadow[sub];
  }
  t->shadow[sub] = mem_alloc(tex_sub_bytes(t, sub));
  return t->shadow[sub];
}
/* Frees the program's copies of a texture's subresources (not the program's own memory). */
static void tex_free_shadows(Tex *t) {
  /* (a block holds every copy there is, or there is none and each was made on its own: tex_shadow) */
  for (UINT i = 0; i < MAX_SUBS; ++i) {
    if (!t->shadow_block && t->shadow[i] != t->user_memory) mem_free(t->shadow[i]);
    t->shadow[i] = NULL;
  }
  mem_free(t->shadow_block);
  t->shadow_block = NULL;
}
/* The Direct3D 12 subresource of a subresource (the kernel keeps all levels of a 2D texture, one of a cube's or a volume's). */
static UINT tex_d12_sub(const Tex *t, UINT sub) { return sub_level(t, sub) + sub_face(t, sub) * t->kmips; }

/* Sends rows of pixels as the Direct3D 12 texture has them (B8G8R8A8, a wide format's own, or a compressed format's blocks) into a part of a subresource: `rect`
 * of it (NULL: the whole), from `pixels` with rows `pitch` bytes apart, slices `slice_pitch` apart for a volume. The list is sent with it. */
static HRESULT tex_upload_rows(Dev *d, Tex *t, UINT sub, const RECT *rect, const BYTE *pixels, UINT pitch, UINT slice_pitch) {
  if (!t->res || t->depth || sub_level(t, sub) >= t->kmips) return D3D_OK;      /* (a level the kernel does not keep) */
  const UINT w = sub_width(t, sub), h = sub_height(t, sub), slices = tex_slices(t, sub), block = dxt_block(t->format);
  RECT r = {0, 0, (LONG)w, (LONG)h};
  if (rect) r = *rect;
  if (r.left < 0 || r.top < 0 || r.right > (LONG)w || r.bottom > (LONG)h || r.left >= r.right || r.top >= r.bottom) return D3DERR_INVALIDCALL;
  const UINT rw = (UINT)(r.right - r.left), rh = (UINT)(r.bottom - r.top);
  const UINT row_bytes = block ? (rw + 3) / 4 * block : rw * gpu_bytes(t->format), rows = block ? (rh + 3) / 4 : rh;
  const UINT row_pitch = (row_bytes + 255u) & ~255u;
  settle(d, t->used);
  ID3D12Resource *upload = make_buffer(d, D3D12_HEAP_TYPE_UPLOAD, (UINT64)row_pitch * rows * slices);
  BYTE *memory = NULL;
  if (!upload || FAILED(ID3D12Resource_Map(upload, 0, NULL, (void **)&memory)) || !memory) { if (upload) ID3D12Resource_Release(upload); return E_OUTOFMEMORY; }
  for (UINT s = 0; s < slices; ++s)
    for (UINT row = 0; row < rows; ++row) memcpy(memory + ((SIZE_T)s * rows + row) * row_pitch, pixels + (SIZE_T)s * slice_pitch + (SIZE_T)row * pitch, row_bytes);
  ID3D12Resource_Unmap(upload, 0, NULL);
  D3D12_TEXTURE_COPY_LOCATION to, from;
  memset(&to, 0, sizeof to);
  memset(&from, 0, sizeof from);
  to.pResource = t->res;
  to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  to.SubresourceIndex = tex_d12_sub(t, sub);
  from.pResource = upload;
  from.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
  from.PlacedFootprint.Footprint.Format = gpu_dxgi(t->format);
  from.PlacedFootprint.Footprint.Width = block ? (rw + 3) & ~3u : rw;
  from.PlacedFootprint.Footprint.Height = block ? (rh + 3) & ~3u : rh;
  from.PlacedFootprint.Footprint.Depth = slices;
  from.PlacedFootprint.Footprint.RowPitch = row_pitch;
  ID3D12GraphicsCommandList_CopyTextureRegion(d->list, &to, (UINT)r.left, (UINT)r.top, 0, &from, NULL);
  d->recorded = 1;
  flush(d);
  ID3D12Resource_Release(upload);
  return D3D_OK;
}
/* The depth of a lockable depth buffer (D16_LOCKABLE, D32F_LOCKABLE and D32_LOCKABLE are D32_FLOAT in Direct3D 12) between the device and the program's copy:
 * the whole buffer through a buffer of floats (Direct3D 12 copies depth buffers whole), converted to the lock's format (16 or 32 bits of the depth range, or floats). */
static HRESULT tex_depth_copy(Dev *d, Tex *t, UINT sub, BOOL to_device) {
  BYTE *shadow = tex_shadow(t, sub);
  if (!shadow) return E_OUTOFMEMORY;
  if (!t->res || !t->depth || !format_depth_lockable(t->format) || t->kind != 0) return D3D_OK;
  const UINT w = sub_width(t, sub), h = sub_height(t, sub), pitch = format_pitch(t->format, w);
  const UINT row_pitch = (w * 4u + 255u) & ~255u;
  ID3D12Resource *buffer = make_buffer(d, to_device ? D3D12_HEAP_TYPE_UPLOAD : D3D12_HEAP_TYPE_READBACK, (UINT64)row_pitch * h);
  if (!buffer) return E_OUTOFMEMORY;
  BYTE *memory = NULL;
  HRESULT hr = D3D_OK;
  if (to_device) {
    hr = ID3D12Resource_Map(buffer, 0, NULL, (void **)&memory);
    if (SUCCEEDED(hr) && memory) {
      for (UINT y = 0; y < h; ++y) {
        float *row = (float *)(memory + (SIZE_T)y * row_pitch);
        const BYTE *in = shadow + (SIZE_T)y * pitch;
        for (UINT x = 0; x < w; ++x) {
          if (t->format == D3DFMT_D16_LOCKABLE) row[x] = ((const WORD *)in)[x] / 65535.0f;
          else if (t->format == D3DFMT_D32_LOCKABLE) row[x] = (float)(((const DWORD *)in)[x] / 4294967295.0);
          else memcpy(&row[x], in + (SIZE_T)x * 4, 4);
        }
      }
      ID3D12Resource_Unmap(buffer, 0, NULL);
    } else if (SUCCEEDED(hr)) hr = E_FAIL;
    if (FAILED(hr)) { ID3D12Resource_Release(buffer); return hr; }
    /* (what the program wrote is the depth: a clear waiting for the next pass would undo it) */
    for (UINT i = 0; i < d->clear_count;) {
      if (d->clears[i].depth == t) d->clears[i] = d->clears[--d->clear_count]; else ++i;
    }
  }
  D3D12_TEXTURE_COPY_LOCATION tex, buf;
  memset(&tex, 0, sizeof tex);
  memset(&buf, 0, sizeof buf);
  tex.pResource = t->res;
  tex.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  tex.SubresourceIndex = tex_d12_sub(t, sub);
  buf.pResource = buffer;
  buf.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
  buf.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R32_TYPELESS;
  buf.PlacedFootprint.Footprint.Width = w;
  buf.PlacedFootprint.Footprint.Height = h;
  buf.PlacedFootprint.Footprint.Depth = 1;
  buf.PlacedFootprint.Footprint.RowPitch = row_pitch;
  if (to_device) ID3D12GraphicsCommandList_CopyTextureRegion(d->list, &tex, 0, 0, 0, &buf, NULL);
  else ID3D12GraphicsCommandList_CopyTextureRegion(d->list, &buf, 0, 0, 0, &tex, NULL);
  t->used = d->epoch;
  d->recorded = 1;
  flush(d);
  if (!to_device) {
    hr = ID3D12Resource_Map(buffer, 0, NULL, (void **)&memory);
    if (SUCCEEDED(hr) && memory) {
      for (UINT y = 0; y < h; ++y) {
        const float *row = (const float *)(memory + (SIZE_T)y * row_pitch);
        BYTE *out = shadow + (SIZE_T)y * pitch;
        for (UINT x = 0; x < w; ++x) {
          const float z = row[x] < 0.0f ? 0.0f : row[x] > 1.0f ? 1.0f : row[x];
          if (t->format == D3DFMT_D16_LOCKABLE) ((WORD *)out)[x] = (WORD)(z * 65535.0f + 0.5f);
          else if (t->format == D3DFMT_D32_LOCKABLE) ((DWORD *)out)[x] = (DWORD)(z * 4294967295.0 + 0.5);
          else memcpy(out + (SIZE_T)x * 4, &row[x], 4);
        }
      }
      ID3D12Resource_Unmap(buffer, 0, NULL);
    } else if (SUCCEEDED(hr)) hr = E_FAIL;
  }
  ID3D12Resource_Release(buffer);
  return hr;
}
/* Sends a subresource's pixels (the program's copy) to the Direct3D 12 texture: converted to B8G8R8A8 (through the current palette for P8 and A8P8). */
static HRESULT tex_upload(Dev *d, Tex *t, UINT sub) {
  if (t->depth && t->res && t->shadow[sub] && format_depth_lockable(t->format)) return tex_depth_copy(d, t, sub, TRUE);
  if (!t->res || t->depth || !t->shadow[sub] || sub_level(t, sub) >= t->kmips) return D3D_OK;
  const UINT w = sub_width(t, sub), h = sub_height(t, sub), slices = tex_slices(t, sub), bytes = format_bytes(t->format);
  if (dxt_block(t->format)) return tex_upload_rows(d, t, sub, NULL, t->shadow[sub], format_pitch(t->format, w), format_pitch(t->format, w) * format_rows(t->format, h));
  if (format_is_wide(t->format)) {      /* (its own pixels: as they are, but for A2R10G10B10's red and blue) */
    BYTE *rows = mem_alloc((SIZE_T)w * bytes * h * slices);
    if (!rows) return E_OUTOFMEMORY;
    for (UINT row = 0; row < h * slices; ++row) row_to_gpu(t->format, t->shadow[sub] + (SIZE_T)row * w * bytes, w, rows + (SIZE_T)row * w * bytes);
    const HRESULT hr = tex_upload_rows(d, t, sub, NULL, rows, w * bytes, w * bytes * h);
    mem_free(rows);
    return hr;
  }
  BYTE *bgra = mem_alloc((SIZE_T)w * 4 * h * slices);
  if (!bgra) return E_OUTOFMEMORY;
  const PALETTEENTRY *palette = format_is_palette(t->format) ? current_palette(d) : NULL;
  for (UINT row = 0; row < h * slices; ++row) row_to_bgra(t->format, t->shadow[sub] + (SIZE_T)row * w * bytes, w, bgra + (SIZE_T)row * w * 4, palette);
  if (format_is_palette(t->format)) { t->palette = d->current_palette; t->palette_version = d->palette_version; }
  const HRESULT hr = tex_upload_rows(d, t, sub, NULL, bgra, w * 4, w * 4 * h);
  mem_free(bgra);
  return hr;
}
/* Fetches a subresource of a Direct3D 12 texture (a 2D level or a cube's face) as the texture has its rows (B8G8R8A8, or a wide format's own), `pitch` bytes apart. */
static HRESULT tex_download(Dev *d, Tex *t, UINT sub, BYTE *rows, UINT pitch) {
  if (!t->res || t->depth || dxt_block(t->format) || t->kind == 1) return D3DERR_INVALIDCALL;
  const UINT w = sub_width(t, sub), h = sub_height(t, sub), bytes = gpu_bytes(t->format);
  const UINT row_pitch = (w * bytes + 255u) & ~255u;
  ID3D12Resource *readback = make_buffer(d, D3D12_HEAP_TYPE_READBACK, (UINT64)row_pitch * h);
  if (!readback) return E_OUTOFMEMORY;
  D3D12_TEXTURE_COPY_LOCATION to, from;
  memset(&to, 0, sizeof to);
  memset(&from, 0, sizeof from);
  to.pResource = readback;
  to.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
  to.PlacedFootprint.Footprint.Format = gpu_dxgi(t->format);
  to.PlacedFootprint.Footprint.Width = w;
  to.PlacedFootprint.Footprint.Height = h;
  to.PlacedFootprint.Footprint.Depth = 1;
  to.PlacedFootprint.Footprint.RowPitch = row_pitch;
  from.pResource = t->res;
  from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  from.SubresourceIndex = tex_d12_sub(t, sub);
  ID3D12GraphicsCommandList_CopyTextureRegion(d->list, &to, 0, 0, 0, &from, NULL);
  d->recorded = 1;
  flush(d);
  const BYTE *memory = NULL;
  HRESULT hr = ID3D12Resource_Map(readback, 0, NULL, (void **)&memory);
  if (SUCCEEDED(hr) && memory) {
    for (UINT row = 0; row < h; ++row) memcpy(rows + (SIZE_T)row * pitch, memory + (SIZE_T)row * row_pitch, (SIZE_T)w * bytes);
    ID3D12Resource_Unmap(readback, 0, NULL);
  } else if (SUCCEEDED(hr)) hr = E_FAIL;
  ID3D12Resource_Release(readback);
  return hr;
}
/* The program's copy of a subresource brought up to what the device has (for what the device owns: tex_device_owned), in the subresource's format. */
static HRESULT tex_fetch(Dev *d, Tex *t, UINT sub) {
  BYTE *shadow = tex_shadow(t, sub);
  if (!shadow) return E_OUTOFMEMORY;
  if (t->depth && t->res && format_depth_lockable(t->format)) return tex_depth_copy(d, t, sub, FALSE);
  if (!tex_device_owned(t) || t->depth) return D3D_OK;
  const UINT w = sub_width(t, sub), h = sub_height(t, sub);
  if (format_is_wide(t->format)) {
    const HRESULT hr = tex_download(d, t, sub, shadow, w * format_bytes(t->format));
    if (SUCCEEDED(hr)) for (UINT row = 0; row < h; ++row) row_from_gpu(t->format, shadow + (SIZE_T)row * w * format_bytes(t->format), w, shadow + (SIZE_T)row * w * format_bytes(t->format));
    return hr;
  }
  if (t->format == D3DFMT_A8R8G8B8 || t->format == D3DFMT_X8R8G8B8) {
    const HRESULT hr = tex_download(d, t, sub, shadow, w * 4);
    if (SUCCEEDED(hr) && t->format == D3DFMT_X8R8G8B8) for (SIZE_T i = 3; i < (SIZE_T)w * h * 4; i += 4) shadow[i] = 0xFF;      /* (X8: the top byte reads as 0xFF) */
    return hr;
  }
  BYTE *bgra = mem_alloc((SIZE_T)w * h * 4);
  if (!bgra) return E_OUTOFMEMORY;
  HRESULT hr = tex_download(d, t, sub, bgra, w * 4);
  if (SUCCEEDED(hr)) for (UINT row = 0; row < h; ++row) row_from_bgra(t->format, bgra + (SIZE_T)row * w * 4, w, shadow + (SIZE_T)row * format_pitch(t->format, w));
  mem_free(bgra);
  return hr;
}

/* ---- textures: the objects -------------------------------------------------------------------------------------------------------------------------- */

static void surface_destroy(DxObj *o);
static void volume_destroy(DxObj *o);
static void tex_destroy(DxObj *o) {
  Tex *t = (Tex *)o;
  Dev *d = t->c.dev;
  settle(d, t->used);
  for (UINT i = 0; i < d->clear_count;) {      /* (a clear nothing drew after: it goes with the buffer) */
    if (d->clears[i].depth == t) d->clears[i] = d->clears[--d->clear_count]; else ++i;
  }
  if (t->target && d->rtv_free_count < RTV_SLOTS) d->rtv_free[d->rtv_free_count++] = t->rtv_slot;
  if (t->depth && t->res && d->dsv_free_count < DSV_SLOTS) d->dsv_free[d->dsv_free_count++] = t->dsv_slot;
  for (UINT i = 0; i < MAX_SUBS; ++i) {
    if (t->surface[i]) obj_free(OBJ(t->surface[i]));
  }
  tex_free_shadows(t);
  for (UINT i = 0; i < MAX_LEVELS; ++i) if (t->volume[i]) obj_free(OBJ(t->volume[i]));
  if (t->res) ID3D12Resource_Release(t->res);
  d->video_bytes -= t->video;
  if (!t->internal && !t->surface_alone) default_resource_gone(d, t->pool);
}

/* Makes the views of a target or depth buffer in the device's heaps (again, when the resource behind changed). */
static void tex_make_view(Dev *d, Tex *t) {
  if (t->depth) {
    t->view = ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(d->dsv_heap);
    t->view.ptr += (SIZE_T)t->dsv_slot * ID3D12Device_GetDescriptorHandleIncrementSize(d->d12, D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
    ID3D12Device_CreateDepthStencilView(d->d12, t->res, NULL, t->view);
  } else {
    t->view = ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(d->rtv_heap);
    t->view.ptr += (SIZE_T)t->rtv_slot * ID3D12Device_GetDescriptorHandleIncrementSize(d->d12, D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    ID3D12Device_CreateRenderTargetView(d->d12, t->res, NULL, t->view);
  }
}

/* The flags of tex_make_kind. */
#define TEX_MEMORY 1u        /* no Direct3D 12 texture: memory only (a surface in system memory or a scratch one) */
#define TEX_INTERNAL 2u      /* the device's own: not one of the program's resources */
#define TEX_LOCKABLE 4u      /* a target the program may lock */
#define TEX_ALONE 8u         /* behind a surface of its own */
#define TEX_IMPLICIT 16u     /* kept by what made it (a swap chain's buffer, the device's depth buffer) */
#define TEX_SHARED 32u       /* shared: a global share handle is made for it (Tex.shared) */
/* A texture, or what stands behind a surface. `existing`: the Direct3D 12 texture to use (a swap chain's buffer), a reference is taken. `kind`: 0 a 2D texture,
 * 1 a volume of `slices`, 3 a cube (Tex.kind). It is made with one reference (of the device too), or with none if TEX_IMPLICIT or TEX_INTERNAL. */
static HRESULT tex_make_kind(Dev *d, UINT kind, UINT width, UINT height, UINT slices, UINT levels, DWORD usage, D3DFORMAT format, D3DPOOL pool, UINT flags, ID3D12Resource *existing,
                             Tex **out) {
  *out = NULL;
  const BOOL depth = format_is_depth(format), memory_only = (flags & TEX_MEMORY) != 0;
  if (width == 0 || height == 0 || width > 4096 || height > 4096 || (!depth && !format_bytes(format) && !dxt_block(format) && !format_is_yuv(format))) return D3DERR_INVALIDCALL;
  if (format_is_yuv(format) && pool != D3DPOOL_SCRATCH) return D3DERR_INVALIDCALL;      /* (YUY2, UYVY: memory of the scratch pool alone) */
  /* (compressed: sampled 2D textures and cubes, and volumes of the scratch pool, which the device never sees; Windows answers D3DERR_INVALIDCALL for the others) */
  if (dxt_block(format) && ((kind == 1 && pool != D3DPOOL_SCRATCH) || (usage & (D3DUSAGE_RENDERTARGET | D3DUSAGE_DEPTHSTENCIL)) || existing)) return D3DERR_INVALIDCALL;
  if (kind == 1 && (slices == 0 || slices > 256)) return D3DERR_INVALIDCALL;
  if (kind == 3 && width != height) return D3DERR_INVALIDCALL;
  if (depth && (usage & D3DUSAGE_RENDERTARGET)) return D3DERR_INVALIDCALL;      /* (a depth format is no colour target: CreateRenderTarget(D3DFMT_D16) is refused) */
  const BOOL target = !depth && (usage & D3DUSAGE_RENDERTARGET) != 0;
  if (target && !format_is_target(format)) return D3DERR_NOTAVAILABLE;
  if ((target || depth) && memory_only) return D3DERR_INVALIDCALL;
  if (format_is_palette(format) && (target || depth)) return D3DERR_NOTAVAILABLE;
  /* (the kernel samples volumes and cubes, and draws into the faces of a cube; not into a volume, nor a cube of depth) */
  if (kind && (depth || existing || (target && kind == 1))) return D3DERR_NOTAVAILABLE;
  if (depth && !memory_only && d->dsv_free_count == 0) return E_OUTOFMEMORY;
  if (target && d->rtv_free_count == 0) return E_OUTOFMEMORY;
  UINT full = 1;
  UINT largest = width > height ? width : height;
  if (kind == 1 && slices > largest) largest = slices;
  for (UINT size = largest; size > 1; size >>= 1) ++full;
  if (levels == 0 || levels > full) levels = full;
  /* (the program sees one level of D3DUSAGE_AUTOGENMIPMAP; the others are made from it where the format can be drawn into - D3DOK_NOAUTOGEN else) */
  if (usage & D3DUSAGE_AUTOGENMIPMAP) levels = kind == 0 && format_is_target(format) && pool != D3DPOOL_SYSTEMMEM && pool != D3DPOOL_SCRATCH ? full : 1;
  if (kind && !adapter_mipmapped_layers()) levels = 1;      /* (cubes and volumes of one level where the kernel keeps one: the caps have no MIPCUBEMAP) */
  if (target || depth) levels = kind == 0 && target && !(flags & (TEX_ALONE | TEX_IMPLICIT)) ? levels : 1;
  if (levels > MAX_LEVELS) levels = MAX_LEVELS;
  const BOOL implicit = (flags & (TEX_IMPLICIT | TEX_INTERNAL)) != 0;
  Tex *t = child_new_ex(sizeof *t, d, kind == 3 ? (const void *)&cube_vtbl : kind == 1 ? (const void *)&vol_vtbl : (const void *)&tex_vtbl,
                        kind == 3 ? g_cube_iids : kind == 1 ? g_vol_iids : g_tex_iids, tex_destroy, implicit);
  if (!t) return E_OUTOFMEMORY;
  t->width = width;
  t->height = height;
  t->levels = levels;
  t->kind = kind;
  t->slices = kind == 1 ? slices : 1;
  t->kmips = levels;
  t->format = format;
  t->usage = usage;
  t->pool = pool;
  t->internal = (flags & TEX_INTERNAL) != 0 || (flags & TEX_IMPLICIT) != 0;
  t->surface_alone = (flags & TEX_ALONE) != 0;
  t->lockable = (flags & TEX_LOCKABLE) != 0 || (depth && format_depth_lockable(format));
  t->autogen_filter = D3DTEXF_LINEAR;
  t->palette = 0xFFFFFFFFu;
  if (!t->internal && !t->surface_alone) default_resource_made(d, pool);
  /* (a new texture is dirty all over: UpdateTexture copies all of it the first time) */
  for (UINT face = 0; face < 6; ++face) { t->dirty[face] = 1; SetRect(&t->dirty_rect[face], 0, 0, (int)width, (int)height); }
  t->dirty_box.Right = width, t->dirty_box.Bottom = height, t->dirty_box.Back = t->slices;
  if (existing) {
    ID3D12Resource_AddRef(existing);
    t->res = existing;
  } else if (!memory_only) {
    D3D12_HEAP_PROPERTIES props = {D3D12_HEAP_TYPE_DEFAULT, D3D12_CPU_PAGE_PROPERTY_UNKNOWN, D3D12_MEMORY_POOL_UNKNOWN, 1, 1};
    D3D12_RESOURCE_DESC rd = {kind == 1 ? D3D12_RESOURCE_DIMENSION_TEXTURE3D : D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, width, height,
                              (UINT16)(kind == 1 ? slices : kind == 3 ? 6 : 1), (UINT16)t->kmips,
                              depth ? depth_dxgi(format) : gpu_dxgi(format),
                              {1, 0}, D3D12_TEXTURE_LAYOUT_UNKNOWN, depth ? D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL : target ? D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET : D3D12_RESOURCE_FLAG_NONE};
    /* (Direct3D 12 knows a cube only by its views; the kernel has to know it now. A shared one gets a global share handle, the legacy kind Direct3D 9 hands out,
     * and carries its description for the device that opens it) */
    if (kind == 3 || (flags & TEX_SHARED)) TcosD3D12ResourceOptions(d->d12, (kind == 3 ? TCOS_RESOURCE_CUBE : 0u) | ((flags & TEX_SHARED) ? TCOS_RESOURCE_GLOBAL_SHARE : 0u));
    if (flags & TEX_SHARED) {
      const DWORD blob[8] = {0x54333944u /* "D93T" */, kind, width, height, kind == 1 ? slices : 1, levels, (DWORD)format, usage};
      TcosD3D12ShareExtra(d->d12, blob, sizeof blob);
    }
    const HRESULT hr = ID3D12Device_CreateCommittedResource(d->d12, &props, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COMMON, NULL, &IID_ID3D12Resource, (void **)&t->res);
    if (FAILED(hr) || !t->res) {
      t->res = NULL;
      if (implicit) obj_free(OBJ(t)); else { obj_free(OBJ(t)); dx_Release(OBJ(d)); }
      return hr == E_OUTOFMEMORY ? D3DERR_OUTOFVIDEOMEMORY : kind ? D3DERR_NOTAVAILABLE : D3DERR_INVALIDCALL;
    }
  }
  if (t->res && !existing) {      /* (what it takes of the adapter's memory: its levels' bytes) */
    for (UINT l = 0; l < t->kmips; ++l)
      t->video += (UINT64)(dxt_block(format) ? format_pitch(format, level_size(width, l)) * format_rows(format, level_size(height, l)) : level_size(width, l) * 4u * level_size(height, l)) *
                  (kind == 1 ? level_size(slices, l) : kind == 3 ? 6u : 1u);
    d->video_bytes += t->video;
  }
  if ((flags & TEX_SHARED) && t->res) t->shared = (HANDLE)(UINT_PTR)TcosD3D12GlobalShare(t->res);
  else if (existing && TcosD3D12GlobalShare(existing)) t->shared = (HANDLE)(UINT_PTR)TcosD3D12GlobalShare(existing);
  /* (the slots are taken only now: tex_destroy gives back what the flags say was taken) */
  if (depth) {
    t->depth = 1;
    if (t->res) {
      t->dsv_slot = d->dsv_free[--d->dsv_free_count];
      tex_make_view(d, t);
    }
  } else if ((target || existing) && t->res) {
    t->target = 1;
    t->rtv_slot = d->rtv_free[--d->rtv_free_count];
    tex_make_view(d, t);
  }
  *out = t;
  return D3D_OK;
}
static HRESULT tex_make(Dev *d, UINT width, UINT height, UINT levels, DWORD usage, D3DFORMAT format, D3DPOOL pool, UINT flags, ID3D12Resource *existing, Tex **out) {
  return tex_make_kind(d, 0, width, height, 1, levels, usage, format, pool, flags, existing, out);
}

/* The levels a program sees: one of a texture that makes its others (D3DUSAGE_AUTOGENMIPMAP). */
static UINT tex_visible_levels(const Tex *t) { return (t->usage & D3DUSAGE_AUTOGENMIPMAP) ? 1 : t->levels; }

/* Adds a rectangle of a level (`level`'s pixels; NULL: all of it) of a face to what UpdateTexture copies, in the top level's pixels. */
static void tex_add_dirty(Tex *t, UINT face, UINT level, const RECT *rect) {
  RECT r = {0, 0, (LONG)t->width, (LONG)t->height};
  if (rect) SetRect(&r, rect->left << level, rect->top << level, rect->right << level, rect->bottom << level);
  if (r.right > (LONG)t->width) r.right = (LONG)t->width;
  if (r.bottom > (LONG)t->height) r.bottom = (LONG)t->height;
  if (!t->dirty[face]) t->dirty_rect[face] = r;
  else {
    RECT *u = &t->dirty_rect[face];
    if (r.left < u->left) u->left = r.left;
    if (r.top < u->top) u->top = r.top;
    if (r.right > u->right) u->right = r.right;
    if (r.bottom > u->bottom) u->bottom = r.bottom;
  }
  t->dirty[face] = 1;
}

/* Whether the program may lock a subresource of the texture: not one of D3DPOOL_DEFAULT that is not dynamic (or a target or depth buffer that is not lockable). */
static BOOL tex_lockable(const Tex *t) {
  if (t->pool != D3DPOOL_DEFAULT) return TRUE;
  if (t->target || t->depth) return t->lockable;
  return (t->usage & D3DUSAGE_DYNAMIC) != 0 || !t->res || t->surface_alone;      /* (an off-screen plain surface of the default pool may be locked) */
}

/* Locks a subresource: the program gets its copy of the pixels (fetched first for what the device owns). `rect` must be inside (on whole blocks of a compressed
 * format, but for the right and bottom edges of a level). */
static HRESULT tex_lock(Tex *t, UINT sub, D3DLOCKED_RECT *out, const RECT *rect, DWORD flags) {
  Dev *d = t->c.dev;
  if (!out) return D3DERR_INVALIDCALL;
  const UINT bytes = format_bytes(t->format), block = dxt_block(t->format);
  if (sub >= tex_subresources(t) || (t->locked[sub] & 0x80000000u) || !tex_lockable(t)) return D3DERR_INVALIDCALL;
  if (t->depth && !format_depth_lockable(t->format)) return D3DERR_INVALIDCALL;
  if (!t->depth && !bytes && !block && !format_is_yuv(t->format)) return D3DERR_INVALIDCALL;
  const UINT w = sub_width(t, sub), h = sub_height(t, sub);
  /* (Direct3D 9 checks the rectangle of a compressed level only in the default pool: in the others any rectangle locks - Wine's test_surface_blocks
   * holds what Windows does -, and one that is not inside the level, or not on its blocks, hands out the level's first block) */
  BOOL rect_inside = TRUE;
  if (rect) {
    rect_inside = rect->left >= 0 && rect->top >= 0 && rect->right <= (LONG)w && rect->bottom <= (LONG)h && rect->left < rect->right && rect->top < rect->bottom;
    const BOOL on_blocks = !block || (!(rect->left & 3) && !(rect->top & 3) && (!(rect->right & 3) || rect->right == (LONG)w) && (!(rect->bottom & 3) || rect->bottom == (LONG)h));
    if ((!rect_inside || !on_blocks) && (!block || t->pool == D3DPOOL_DEFAULT)) return D3DERR_INVALIDCALL;
    if (!on_blocks) rect_inside = FALSE;
  }
  for (UINT i = 0; i < MAX_SUBS; ++i)
    if (t->surface[i] && t->surface[i]->dc && i == sub) return D3DERR_INVALIDCALL;      /* (a surface with its DC out) */
  HRESULT hr = D3D_OK;
  EnterCriticalSection(&d->lock);
  if (!tex_shadow(t, sub)) hr = E_OUTOFMEMORY;
  else if (tex_device_owned(t) && !(flags & D3DLOCK_DISCARD)) hr = tex_fetch(d, t, sub);
  else if (!(flags & D3DLOCK_READONLY)) settle(d, t->used);
  LeaveCriticalSection(&d->lock);
  if (FAILED(hr)) return hr;
  t->locked[sub] = flags | 0x80000000u;
  if (!(flags & (D3DLOCK_READONLY | D3DLOCK_NO_DIRTY_UPDATE)) && sub_level(t, sub) == 0) tex_add_dirty(t, t->kind == 3 ? sub_face(t, sub) : 0, 0, rect_inside ? rect : NULL);
  out->Pitch = (INT)format_pitch(t->format, w);
  /* (a rectangle of a compressed level starts at its block) */
  const BOOL at_rect = rect && rect_inside;
  const SIZE_T row = at_rect ? (SIZE_T)(block ? (UINT)rect->top / 4 : (UINT)rect->top) : 0, column = at_rect ? (block ? (SIZE_T)rect->left / 4 * block : format_is_yuv(t->format) ? (SIZE_T)rect->left / 2 * 4 : (SIZE_T)rect->left * bytes) : 0;
  out->pBits = t->shadow[sub] + row * (SIZE_T)out->Pitch + column;
  return D3D_OK;
}
static void tex_generate_levels(Dev *d, Tex *t);
static HRESULT tex_unlock(Tex *t, UINT sub) {
  Dev *d = t->c.dev;
  if (sub >= tex_subresources(t) || !(t->locked[sub] & 0x80000000u)) return D3DERR_INVALIDCALL;
  const DWORD flags = t->locked[sub];
  t->locked[sub] = 0;
  if (flags & D3DLOCK_READONLY) return D3D_OK;
  EnterCriticalSection(&d->lock);
  HRESULT hr = tex_upload(d, t, sub);
  if (SUCCEEDED(hr) && (t->usage & D3DUSAGE_AUTOGENMIPMAP) && sub_level(t, sub) == 0) t->autogen_dirty = 1;
  LeaveCriticalSection(&d->lock);
  return hr;
}
static void tex_describe(const Tex *t, UINT sub, D3DSURFACE_DESC *desc) {
  desc->Format = t->format;
  desc->Type = D3DRTYPE_SURFACE;
  desc->Usage = t->usage;
  desc->Pool = t->pool;
  desc->MultiSampleType = t->samples;
  desc->MultiSampleQuality = t->quality;
  desc->Width = sub_width(t, sub);
  desc->Height = sub_height(t, sub);
}

/* Makes the levels of a D3DUSAGE_AUTOGENMIPMAP texture from its top level (each from the one above, two by two pixels into one, as the filter type asks:
 * a box for D3DTEXF_LINEAR and the others, the top left one for D3DTEXF_POINT). */
static void tex_generate_wide_levels(Dev *d, Tex *t);
static void tex_generate_levels(Dev *d, Tex *t) {
  if (!t->res || t->levels < 2 || t->kind != 0) { t->autogen_dirty = 0; return; }
  if (format_is_wide(t->format)) { tex_generate_wide_levels(d, t); return; }
  const UINT w = t->width, h = t->height;
  BYTE *level = mem_alloc((SIZE_T)w * h * 4), *next = mem_alloc((SIZE_T)w * h * 4);
  if (!level || !next) { mem_free(level); mem_free(next); return; }
  HRESULT hr = D3D_OK;
  if (tex_device_owned(t)) hr = tex_download(d, t, 0, level, w * 4);
  else if (t->shadow[0]) for (UINT row = 0; row < h; ++row) row_to_bgra(t->format, t->shadow[0] + (SIZE_T)row * format_pitch(t->format, w), w, level + (SIZE_T)row * w * 4, current_palette(d));
  else memset(level, 0, (SIZE_T)w * h * 4);
  for (UINT l = 1; SUCCEEDED(hr) && l < t->levels; ++l) {
    const UINT pw = level_size(w, l - 1), ph = level_size(h, l - 1), lw = level_size(w, l), lh = level_size(h, l);
    for (UINT y = 0; y < lh; ++y)
      for (UINT x = 0; x < lw; ++x)
        for (UINT c = 0; c < 4; ++c) {
          const UINT x0 = x * 2 < pw ? x * 2 : pw - 1, y0 = y * 2 < ph ? y * 2 : ph - 1, x1 = x0 + 1 < pw ? x0 + 1 : x0, y1 = y0 + 1 < ph ? y0 + 1 : y0;
          UINT v;
          if (t->autogen_filter == D3DTEXF_POINT) v = level[((SIZE_T)y0 * pw + x0) * 4 + c];
          else v = (level[((SIZE_T)y0 * pw + x0) * 4 + c] + level[((SIZE_T)y0 * pw + x1) * 4 + c] + level[((SIZE_T)y1 * pw + x0) * 4 + c] + level[((SIZE_T)y1 * pw + x1) * 4 + c] + 2) / 4;
          next[((SIZE_T)y * lw + x) * 4 + c] = (BYTE)v;
        }
    hr = tex_upload_rows(d, t, l, NULL, next, lw * 4, lw * 4 * lh);
    BYTE *swap = level; level = next; next = swap;
  }
  mem_free(level);
  mem_free(next);
  t->autogen_dirty = 0;
}

/* The same for a wide format: each level a box (or the top left one) of the one above, as four floats a pixel, sent as the format's pixels. */
static void tex_generate_wide_levels(Dev *d, Tex *t) {
  const UINT w = t->width, h = t->height, bytes = format_bytes(t->format);
  float *level = mem_alloc((SIZE_T)w * h * 16), *next = mem_alloc((SIZE_T)w * h * 16);
  BYTE *raw = mem_alloc((SIZE_T)w * h * bytes);
  HRESULT hr = level && next && raw ? D3D_OK : E_OUTOFMEMORY;
  if (SUCCEEDED(hr)) {
    if (tex_device_owned(t)) {
      hr = tex_download(d, t, 0, raw, w * bytes);
      if (SUCCEEDED(hr)) for (UINT row = 0; row < h; ++row) row_from_gpu(t->format, raw + (SIZE_T)row * w * bytes, w, raw + (SIZE_T)row * w * bytes);
    } else if (t->shadow[0]) memcpy(raw, t->shadow[0], (SIZE_T)w * h * bytes);
    else memset(raw, 0, (SIZE_T)w * h * bytes);
  }
  if (SUCCEEDED(hr)) for (SIZE_T i = 0; i < (SIZE_T)w * h; ++i) wide_to_float(t->format, raw + i * bytes, level + i * 4);
  for (UINT l = 1; SUCCEEDED(hr) && l < t->levels; ++l) {
    const UINT pw = level_size(w, l - 1), ph = level_size(h, l - 1), lw = level_size(w, l), lh = level_size(h, l);
    for (UINT y = 0; y < lh; ++y)
      for (UINT x = 0; x < lw; ++x)
        for (UINT c = 0; c < 4; ++c) {
          const UINT x0 = x * 2 < pw ? x * 2 : pw - 1, y0 = y * 2 < ph ? y * 2 : ph - 1, x1 = x0 + 1 < pw ? x0 + 1 : x0, y1 = y0 + 1 < ph ? y0 + 1 : y0;
          next[((SIZE_T)y * lw + x) * 4 + c] = t->autogen_filter == D3DTEXF_POINT ? level[((SIZE_T)y0 * pw + x0) * 4 + c]
                                               : (level[((SIZE_T)y0 * pw + x0) * 4 + c] + level[((SIZE_T)y0 * pw + x1) * 4 + c] + level[((SIZE_T)y1 * pw + x0) * 4 + c] +
                                                  level[((SIZE_T)y1 * pw + x1) * 4 + c]) * 0.25f;
        }
    for (SIZE_T i = 0; i < (SIZE_T)lw * lh; ++i) float_to_wide(t->format, next + i * 4, raw + i * bytes);
    for (UINT row = 0; row < lh; ++row) row_to_gpu(t->format, raw + (SIZE_T)row * lw * bytes, lw, raw + (SIZE_T)row * lw * bytes);
    hr = tex_upload_rows(d, t, l, NULL, raw, lw * bytes, lw * bytes * lh);
    float *swap = level; level = next; next = swap;
  }
  mem_free(level);
  mem_free(next);
  mem_free(raw);
  t->autogen_dirty = 0;
}

static D3DRESOURCETYPE STDMETHODCALLTYPE tex_GetType(Tex *t) { (void)t; return D3DRTYPE_TEXTURE; }
/* The most detailed level a managed texture is sampled from (SetLOD): the others are not set; it answers the one before. */
static DWORD STDMETHODCALLTYPE tex_SetLOD(Tex *t, DWORD lod) {
  if (t->pool != D3DPOOL_MANAGED) return 0;
  const DWORD old = t->lod;
  t->lod = lod < tex_visible_levels(t) ? lod : tex_visible_levels(t) - 1;
  return old;
}
static DWORD STDMETHODCALLTYPE tex_GetLOD(Tex *t) { return t->pool == D3DPOOL_MANAGED ? t->lod : 0; }
static DWORD STDMETHODCALLTYPE tex_GetLevelCount(Tex *t) { return tex_visible_levels(t); }
static HRESULT STDMETHODCALLTYPE tex_SetAutoGenFilterType(Tex *t, D3DTEXTUREFILTERTYPE type) {
  if (type == D3DTEXF_NONE || type > D3DTEXF_GAUSSIANQUAD) return D3DERR_INVALIDCALL;
  if (!(t->usage & D3DUSAGE_AUTOGENMIPMAP)) return D3DERR_INVALIDCALL;
  t->autogen_filter = type;
  t->autogen_dirty = 1;
  return D3D_OK;
}
static D3DTEXTUREFILTERTYPE STDMETHODCALLTYPE tex_GetAutoGenFilterType(Tex *t) { return (t->usage & D3DUSAGE_AUTOGENMIPMAP) ? t->autogen_filter : D3DTEXF_NONE; }
static void STDMETHODCALLTYPE tex_GenerateMipSubLevels(Tex *t) {
  if (!(t->usage & D3DUSAGE_AUTOGENMIPMAP)) return;
  Dev *d = t->c.dev;
  EnterCriticalSection(&d->lock);
  tex_generate_levels(d, t);
  LeaveCriticalSection(&d->lock);
}
static HRESULT STDMETHODCALLTYPE tex_GetLevelDesc(Tex *t, UINT level, D3DSURFACE_DESC *desc) {
  if (!desc || level >= tex_visible_levels(t)) return D3DERR_INVALIDCALL;
  tex_describe(t, level, desc);
  return D3D_OK;
}
static HRESULT STDMETHODCALLTYPE tex_LockRect(Tex *t, UINT level, D3DLOCKED_RECT *out, const RECT *rect, DWORD flags) {
  if (level >= tex_visible_levels(t)) return D3DERR_INVALIDCALL;
  return tex_lock(t, level, out, rect, flags);
}
static HRESULT STDMETHODCALLTYPE tex_UnlockRect(Tex *t, UINT level) {
  if (level >= tex_visible_levels(t)) return D3DERR_INVALIDCALL;
  if (!(t->locked[level] & 0x80000000u)) return D3D_OK;      /* (Direct3D 9 answers D3D_OK for a texture's level that is not locked) */
  return tex_unlock(t, level);
}
static HRESULT STDMETHODCALLTYPE tex_AddDirtyRect(Tex *t, const RECT *rect) {
  if (rect && (rect->left < 0 || rect->top < 0 || rect->left >= rect->right || rect->top >= rect->bottom)) return D3DERR_INVALIDCALL;
  tex_add_dirty(t, 0, 0, rect);
  return D3D_OK;
}

/* ---- cube and volume textures ------------------------------------------------------------------------------------------------------------------------ */

/* A cube's faces are its subresources face * levels + level (D3DCUBEMAP_FACE_POSITIVE_X .. NEGATIVE_Z: the kernel's order, +X -X +Y -Y +Z -Z). */
static D3DRESOURCETYPE STDMETHODCALLTYPE cube_GetType(Tex *t) { (void)t; return D3DRTYPE_CUBETEXTURE; }
static HRESULT STDMETHODCALLTYPE cube_GetLevelDesc(Tex *t, UINT level, D3DSURFACE_DESC *desc) { return tex_GetLevelDesc(t, level, desc); }
static HRESULT STDMETHODCALLTYPE cube_GetCubeMapSurface(Tex *t, D3DCUBEMAP_FACES face, UINT level, IDirect3DSurface9 **out);
static HRESULT STDMETHODCALLTYPE cube_LockRect(Tex *t, D3DCUBEMAP_FACES face, UINT level, D3DLOCKED_RECT *out, const RECT *rect, DWORD flags) {
  if ((UINT)face >= 6 || level >= tex_visible_levels(t)) return D3DERR_INVALIDCALL;
  return tex_lock(t, tex_sub(t, (UINT)face, level), out, rect, flags);
}
static HRESULT STDMETHODCALLTYPE cube_UnlockRect(Tex *t, D3DCUBEMAP_FACES face, UINT level) {
  if ((UINT)face >= 6 || level >= tex_visible_levels(t)) return D3DERR_INVALIDCALL;
  return tex_unlock(t, tex_sub(t, (UINT)face, level));
}
static HRESULT STDMETHODCALLTYPE cube_AddDirtyRect(Tex *t, D3DCUBEMAP_FACES face, const RECT *rect) {
  if ((UINT)face >= 6) return D3DERR_INVALIDCALL;
  tex_add_dirty(t, (UINT)face, 0, rect);
  return D3D_OK;
}

static D3DRESOURCETYPE STDMETHODCALLTYPE vol_GetType(Tex *t) { (void)t; return D3DRTYPE_VOLUMETEXTURE; }
static void volume_describe(const Tex *t, UINT level, D3DVOLUME_DESC *desc) {
  desc->Format = t->format;
  desc->Type = D3DRTYPE_VOLUME;
  desc->Usage = t->usage;
  desc->Pool = t->pool;
  desc->Width = level_size(t->width, level);
  desc->Height = level_size(t->height, level);
  desc->Depth = level_size(t->slices, level);
}
static HRESULT STDMETHODCALLTYPE vol_GetLevelDesc(Tex *t, UINT level, D3DVOLUME_DESC *desc) {
  if (!desc || level >= t->levels) return D3DERR_INVALIDCALL;
  volume_describe(t, level, desc);
  return D3D_OK;
}
/* Locks a level of a volume: its slices one after the other, rows of the program's format; `box` must be inside (on whole blocks). */
static HRESULT volume_lock(Tex *t, UINT level, D3DLOCKED_BOX *out, const D3DBOX *box, DWORD flags) {
  Dev *d = t->c.dev;
  if (!out) return D3DERR_INVALIDCALL;
  const UINT bytes = format_bytes(t->format), block = dxt_block(t->format);
  if (level >= t->levels || (!bytes && !block && !format_is_yuv(t->format)) || (t->locked[level] & 0x80000000u) || !tex_lockable(t)) {
    out->pBits = NULL;
    out->RowPitch = out->SlicePitch = 0;
    return D3DERR_INVALIDCALL;
  }
  const UINT w = level_size(t->width, level), h = level_size(t->height, level), slices = tex_slices(t, level);
  if (box && (box->Left >= box->Right || box->Top >= box->Bottom || box->Front >= box->Back || box->Right > w || box->Bottom > h || box->Back > slices ||
              (block && ((box->Left & 3) || (box->Top & 3) || ((box->Right & 3) && box->Right != w) || ((box->Bottom & 3) && box->Bottom != h))))) {
    out->pBits = NULL;
    out->RowPitch = out->SlicePitch = 0;
    return D3DERR_INVALIDCALL;
  }
  EnterCriticalSection(&d->lock);
  if (!(flags & D3DLOCK_READONLY)) settle(d, t->used);
  BYTE *shadow = tex_shadow(t, level);
  LeaveCriticalSection(&d->lock);
  if (!shadow) return E_OUTOFMEMORY;
  t->locked[level] = flags | 0x80000000u;
  if (!(flags & (D3DLOCK_READONLY | D3DLOCK_NO_DIRTY_UPDATE)) && level == 0) {
    if (!box) { t->dirty_box.Left = t->dirty_box.Top = t->dirty_box.Front = 0; t->dirty_box.Right = w; t->dirty_box.Bottom = h; t->dirty_box.Back = slices; }
    else if (!t->dirty[0]) t->dirty_box = *box;
    else {
      D3DBOX *u = &t->dirty_box;
      if (box->Left < u->Left) u->Left = box->Left;
      if (box->Top < u->Top) u->Top = box->Top;
      if (box->Front < u->Front) u->Front = box->Front;
      if (box->Right > u->Right) u->Right = box->Right;
      if (box->Bottom > u->Bottom) u->Bottom = box->Bottom;
      if (box->Back > u->Back) u->Back = box->Back;
    }
    t->dirty[0] = 1;
  }
  out->RowPitch = (INT)format_pitch(t->format, w);
  out->SlicePitch = (INT)(format_pitch(t->format, w) * format_rows(t->format, h));
  out->pBits = shadow + (box ? (SIZE_T)box->Front * (SIZE_T)out->SlicePitch + (SIZE_T)(block ? box->Top / 4 : box->Top) * (SIZE_T)out->RowPitch +
                               (SIZE_T)(block ? box->Left / 4 * block : format_is_yuv(t->format) ? box->Left / 2 * 4 : box->Left * bytes) : 0);
  return D3D_OK;
}
static HRESULT STDMETHODCALLTYPE vol_LockBox(Tex *t, UINT level, D3DLOCKED_BOX *out, const D3DBOX *box, DWORD flags) { return volume_lock(t, level, out, box, flags); }
static HRESULT STDMETHODCALLTYPE vol_UnlockBox(Tex *t, UINT level) { return tex_unlock(t, level); }
static HRESULT STDMETHODCALLTYPE vol_AddDirtyBox(Tex *t, const D3DBOX *box) {
  if (box && (box->Left >= box->Right || box->Top >= box->Bottom || box->Front >= box->Back)) return D3DERR_INVALIDCALL;
  if (!box || !t->dirty[0]) {
    if (box) t->dirty_box = *box;
    else { t->dirty_box.Left = t->dirty_box.Top = t->dirty_box.Front = 0; t->dirty_box.Right = t->width; t->dirty_box.Bottom = t->height; t->dirty_box.Back = t->slices; }
  } else {
    D3DBOX *u = &t->dirty_box;
    if (box->Left < u->Left) u->Left = box->Left;
    if (box->Top < u->Top) u->Top = box->Top;
    if (box->Front < u->Front) u->Front = box->Front;
    if (box->Right > u->Right) u->Right = box->Right;
    if (box->Bottom > u->Bottom) u->Bottom = box->Bottom;
    if (box->Back > u->Back) u->Back = box->Back;
  }
  t->dirty[0] = 1;
  return D3D_OK;
}

/* The levels of a volume texture as objects: made on the first ask, they count with the texture and go with it. */
static void volume_destroy(DxObj *o) { (void)o; }
static HRESULT STDMETHODCALLTYPE vol_GetVolumeLevel(Tex *t, UINT level, IDirect3DVolume9 **out) {
  if (!out) return D3DERR_INVALIDCALL;
  *out = NULL;
  if (level >= t->levels) return D3DERR_INVALIDCALL;
  if (!t->volume[level]) {
    Volume *v = child_new_ex(sizeof *v, t->c.dev, &volume_vtbl, g_volume_iids, volume_destroy, 1);
    if (!v) return E_OUTOFMEMORY;
    v->c.kept = 1;
    v->c.forward = OBJ(t);
    v->tex = t;
    v->level = level;
    t->volume[level] = v;
  }
  child_AddRef(&t->volume[level]->c);
  *out = (IDirect3DVolume9 *)t->volume[level];
  return D3D_OK;
}
static HRESULT STDMETHODCALLTYPE volume_GetContainer(Volume *v, REFIID riid, void **out) {
  if (!out) return D3DERR_INVALIDCALL;
  *out = NULL;
  return child_QueryInterface(&v->tex->c, riid, out);
}
static HRESULT STDMETHODCALLTYPE volume_GetDesc(Volume *v, D3DVOLUME_DESC *desc) {
  if (!desc) return D3DERR_INVALIDCALL;
  volume_describe(v->tex, v->level, desc);
  return D3D_OK;
}
static HRESULT STDMETHODCALLTYPE volume_LockBox(Volume *v, D3DLOCKED_BOX *out, const D3DBOX *box, DWORD flags) { return volume_lock(v->tex, v->level, out, box, flags); }
static HRESULT STDMETHODCALLTYPE volume_UnlockBox(Volume *v) { return tex_unlock(v->tex, v->level); }

static HRESULT texture_create(Dev *d, UINT kind, UINT width, UINT height, UINT slices, UINT levels, DWORD usage, D3DFORMAT format, D3DPOOL pool, HANDLE *shared, Tex **out);
static HRESULT STDMETHODCALLTYPE dev_CreateCubeTexture(Dev *d, UINT edge, UINT levels, DWORD usage, D3DFORMAT format, D3DPOOL pool, IDirect3DCubeTexture9 **out, HANDLE *shared) {
  if (!out) return D3DERR_INVALIDCALL;
  return texture_create(d, 3, edge, edge, 1, levels, usage, format, pool, shared, (Tex **)out);
}
static HRESULT STDMETHODCALLTYPE dev_CreateVolumeTexture(Dev *d, UINT width, UINT height, UINT depth, UINT levels, DWORD usage, D3DFORMAT format, D3DPOOL pool,
                                                         IDirect3DVolumeTexture9 **out, HANDLE *shared) {
  if (!out) return D3DERR_INVALIDCALL;
  return texture_create(d, 1, width, height, depth, levels, usage, format, pool, shared, (Tex **)out);
}

/* ---- surfaces --------------------------------------------------------------------------------------------------------------------------------------- */

static void surface_release_dc(Surface *s);
static void surface_destroy(DxObj *o) {
  Surface *s = (Surface *)o;
  if (s->dc) surface_release_dc(s);
  if (s->has_view) {
    Dev *d = s->c.dev;
    if (d->rtv_free_count < RTV_SLOTS) d->rtv_free[d->rtv_free_count++] = s->view_slot;
    s->has_view = FALSE;
  }
  if (s->kind == SURFACE_ALONE && s->tex) {
    default_resource_gone(s->c.dev, s->tex->pool);
    obj_free(OBJ(s->tex));
  }
}
/* Whether a surface can be drawn into: a target's level the kernel keeps (of a cube: of each face). */
static BOOL surface_drawable(const Surface *s) { return s->tex->target && s->tex->res && sub_level(s->tex, s->sub) < s->tex->kmips; }
/* The render target view of a surface: the texture's own for level 0 of a 2D one (a swap chain's buffers change resource behind it: tex_make_view), else one of
 * the surface's, of its level and face, made on the first draw into it. A zero handle when there are no slots left. */
static D3D12_CPU_DESCRIPTOR_HANDLE surface_target_view(Dev *d, Surface *s) {
  Tex *t = s->tex;
  if (s->sub == 0 && t->kind == 0) return t->view;
  if (!s->has_view) {
    D3D12_CPU_DESCRIPTOR_HANDLE none = {0};
    if (d->rtv_free_count == 0) return none;
    s->view_slot = d->rtv_free[--d->rtv_free_count];
    s->view = ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(d->rtv_heap);
    s->view.ptr += (SIZE_T)s->view_slot * ID3D12Device_GetDescriptorHandleIncrementSize(d->d12, D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    D3D12_RENDER_TARGET_VIEW_DESC desc;
    memset(&desc, 0, sizeof desc);
    desc.Format = gpu_dxgi(t->format);
    if (t->kind == 3) {
      desc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
      desc.Texture2DArray.MipSlice = sub_level(t, s->sub);
      desc.Texture2DArray.FirstArraySlice = sub_face(t, s->sub);
      desc.Texture2DArray.ArraySize = 1;
    } else {
      desc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
      desc.Texture2D.MipSlice = sub_level(t, s->sub);
    }
    ID3D12Device_CreateRenderTargetView(d->d12, t->res, &desc, s->view);
    s->has_view = TRUE;
  }
  return s->view;
}
/* A surface of a level of a texture: made on the first ask, it counts with the texture (and is freed with it). */
static Surface *surface_of_level(Tex *t, UINT sub) {
  if (!t->surface[sub]) {
    Surface *s = child_new_ex(sizeof *s, t->c.dev, &surf_vtbl, g_surf_iids, surface_destroy, 1);
    if (!s) return NULL;
    s->c.kept = 1;
    s->c.forward = OBJ(t);
    s->tex = t;
    s->sub = sub;
    s->kind = SURFACE_LEVEL;
    s->container = OBJ(t);
    t->surface[sub] = s;
  }
  return t->surface[sub];
}
static HRESULT STDMETHODCALLTYPE tex_GetSurfaceLevel(Tex *t, UINT level, IDirect3DSurface9 **out) {
  if (!out) return D3DERR_INVALIDCALL;
  *out = NULL;
  if (level >= tex_visible_levels(t)) return D3DERR_INVALIDCALL;
  Surface *s = surface_of_level(t, level);
  if (!s) return E_OUTOFMEMORY;
  child_AddRef(&s->c);
  *out = (IDirect3DSurface9 *)s;
  return D3D_OK;
}
static HRESULT STDMETHODCALLTYPE cube_GetCubeMapSurface(Tex *t, D3DCUBEMAP_FACES face, UINT level, IDirect3DSurface9 **out) {
  if (!out) return D3DERR_INVALIDCALL;
  *out = NULL;
  if ((UINT)face >= 6 || level >= tex_visible_levels(t)) return D3DERR_INVALIDCALL;
  Surface *s = surface_of_level(t, tex_sub(t, (UINT)face, level));
  if (!s) return E_OUTOFMEMORY;
  child_AddRef(&s->c);
  *out = (IDirect3DSurface9 *)s;
  return D3D_OK;
}
/* A surface's own object: one that stands alone (its texture is its own) or that begins at no reference (a swap chain's buffer, the device's depth buffer). */
static Surface *surface_make(Dev *d, Tex *t, int kind, DxObj *container) {
  Surface *s = child_new_ex(sizeof *s, d, &surf_vtbl, g_surf_iids, surface_destroy, kind == SURFACE_IMPLICIT);
  if (!s) return NULL;
  s->tex = t;
  s->sub = 0;
  s->kind = kind;
  s->container = container;
  return s;
}
static D3DRESOURCETYPE STDMETHODCALLTYPE surf_GetType(Surface *s) { (void)s; return D3DRTYPE_SURFACE; }
static HRESULT STDMETHODCALLTYPE surf_GetContainer(Surface *s, REFIID riid, void **out) {
  if (!out) return D3DERR_INVALIDCALL;
  *out = NULL;
  if (!s->container) return dx_QueryInterface(OBJ(s->c.dev), riid, out);
  return ((IUnknown *)s->container)->lpVtbl->QueryInterface((IUnknown *)s->container, riid, out);
}
static HRESULT STDMETHODCALLTYPE surf_GetDesc(Surface *s, D3DSURFACE_DESC *desc) {
  if (!desc) return D3DERR_INVALIDCALL;
  tex_describe(s->tex, s->sub, desc);
  if (s->kind == SURFACE_LEVEL && s->tex->kind == 0 && (s->tex->target || s->tex->depth)) desc->Usage = s->tex->usage;
  return D3D_OK;
}
static HRESULT STDMETHODCALLTYPE surf_LockRect(Surface *s, D3DLOCKED_RECT *out, const RECT *rect, DWORD flags) {
  if (s->dc) return D3DERR_INVALIDCALL;
  return tex_lock(s->tex, s->sub, out, rect, flags);
}
static HRESULT STDMETHODCALLTYPE surf_UnlockRect(Surface *s) {
  /* (a level of a 2D texture that is not locked: D3D_OK, as Direct3D 9 answers; any other surface: D3DERR_INVALIDCALL) */
  if (s->kind == SURFACE_LEVEL && s->tex->kind == 0 && !(s->tex->locked[s->sub] & 0x80000000u)) return D3D_OK;
  return tex_unlock(s->tex, s->sub);
}

/* GetDC: a device context with a DIB section of the surface's pixels (the 16-, 24- and 32-bit RGB formats), filled from the surface; ReleaseDC puts what
 * was drawn back. The surface is locked meanwhile (LockRect and a second GetDC fail). */
static HRESULT STDMETHODCALLTYPE surf_GetDC(Surface *s, HDC *out) {
  if (!out) return D3DERR_INVALIDCALL;
  Tex *t = s->tex;
  Dev *d = s->c.dev;
  UINT bits;
  DWORD masks[3] = {0, 0, 0};
  switch (t->format) {
    case D3DFMT_A8R8G8B8: case D3DFMT_X8R8G8B8: bits = 32; break;
    case D3DFMT_R8G8B8: bits = 24; break;
    case D3DFMT_R5G6B5: bits = 16; masks[0] = 0xF800; masks[1] = 0x07E0; masks[2] = 0x001F; break;
    case D3DFMT_X1R5G5B5: case D3DFMT_A1R5G5B5: bits = 16; masks[0] = 0x7C00; masks[1] = 0x03E0; masks[2] = 0x001F; break;
    default: return D3DERR_INVALIDCALL;
  }
  if (s->dc || (t->locked[s->sub] & 0x80000000u) || !tex_lockable(t)) return D3DERR_INVALIDCALL;
  const UINT w = sub_width(t, s->sub), h = sub_height(t, s->sub);
  struct { BITMAPINFOHEADER header; DWORD masks[3]; } info;
  memset(&info, 0, sizeof info);
  info.header.biSize = sizeof(BITMAPINFOHEADER);
  info.header.biWidth = (LONG)w;
  info.header.biHeight = (LONG)h;      /* (bottom-up, as Windows makes it - test_getdc: the rows are turned over going in and out) */
  info.header.biPlanes = 1;
  info.header.biBitCount = (WORD)bits;
  info.header.biCompression = bits == 16 ? BI_BITFIELDS : BI_RGB;
  memcpy(info.masks, masks, sizeof masks);
  HDC dc = CreateCompatibleDC(NULL);
  void *dib = NULL;
  HBITMAP bitmap = dc ? CreateDIBSection(dc, (const BITMAPINFO *)&info, DIB_RGB_COLORS, &dib, NULL, 0) : NULL;
  if (!dc || !bitmap || !dib) {
    if (bitmap) DeleteObject(bitmap);
    if (dc) DeleteDC(dc);
    return D3DERR_INVALIDCALL;
  }
  EnterCriticalSection(&d->lock);
  HRESULT hr = tex_shadow(t, s->sub) ? D3D_OK : E_OUTOFMEMORY;
  if (SUCCEEDED(hr) && tex_device_owned(t)) hr = tex_fetch(d, t, s->sub);
  LeaveCriticalSection(&d->lock);
  if (FAILED(hr)) { DeleteObject(bitmap); DeleteDC(dc); return hr; }
  const UINT pitch = format_pitch(t->format, w), dib_pitch = ((w * bits + 31) / 32) * 4;
  for (UINT row = 0; row < h; ++row) memcpy((BYTE *)dib + (SIZE_T)(h - 1 - row) * dib_pitch, t->shadow[s->sub] + (SIZE_T)row * pitch, pitch);
  s->old_bitmap = SelectObject(dc, bitmap);
  s->dc = dc;
  s->bitmap = bitmap;
  s->dib = dib;
  t->locked[s->sub] = 0x80000000u;
  ++t->dc_count;
  *out = dc;
  return D3D_OK;
}
static void surface_release_dc(Surface *s) {
  Tex *t = s->tex;
  Dev *d = s->c.dev;
  GdiFlush();
  const UINT w = sub_width(t, s->sub), h = sub_height(t, s->sub), bits = format_bytes(t->format) * 8;
  const UINT pitch = format_pitch(t->format, w), dib_pitch = ((w * bits + 31) / 32) * 4;
  if (t->shadow[s->sub]) for (UINT row = 0; row < h; ++row) memcpy(t->shadow[s->sub] + (SIZE_T)row * pitch, s->dib + (SIZE_T)(h - 1 - row) * dib_pitch, pitch);
  SelectObject(s->dc, s->old_bitmap);
  DeleteObject(s->bitmap);
  DeleteDC(s->dc);
  s->dc = NULL;
  s->bitmap = NULL;
  s->dib = NULL;
  t->locked[s->sub] = 0;
  if (t->dc_count) --t->dc_count;
  if (s->sub == 0 && t->kind != 1) tex_add_dirty(t, t->kind == 3 ? sub_face(t, s->sub) : 0, 0, NULL);
  EnterCriticalSection(&d->lock);
  tex_upload(d, t, s->sub);
  LeaveCriticalSection(&d->lock);
}
static HRESULT STDMETHODCALLTYPE surf_ReleaseDC(Surface *s, HDC dc) {
  if (!s->dc || dc != s->dc) return D3DERR_INVALIDCALL;
  surface_release_dc(s);
  return D3D_OK;
}

/* A surface on its own: a texture of one level nobody else sees, and the surface of it. */
static HRESULT surface_alone(Dev *d, UINT width, UINT height, DWORD usage, D3DFORMAT format, D3DPOOL pool, UINT flags, D3DMULTISAMPLE_TYPE samples, DWORD quality, IDirect3DSurface9 **out) {
  if (!out) return D3DERR_INVALIDCALL;
  *out = NULL;
  Tex *t = NULL;
  EnterCriticalSection(&d->lock);
  HRESULT hr = tex_make(d, width, height, 1, usage, format, pool, flags | TEX_ALONE | TEX_INTERNAL, NULL, &t);
  LeaveCriticalSection(&d->lock);
  if (FAILED(hr)) return hr;
  t->internal = 0;
  t->samples = samples;
  t->quality = quality;
  Surface *s = surface_make(d, t, SURFACE_ALONE, NULL);
  if (!s) { obj_free(OBJ(t)); return E_OUTOFMEMORY; }
  t->c.kept = 1;      /* (it goes with its surface) */
  default_resource_made(d, pool);
  *out = (IDirect3DSurface9 *)s;
  return D3D_OK;
}

/* ---- vertex declarations ---------------------------------------------------------------------------------------------------------------------------- */

static const Attribute g_no_attribute = {0, 255, 0};

/* What the fixed-function shaders take, out of a declaration's elements. */
static void layout_of_elements(const D3DVERTEXELEMENT9 *e, UINT count, Layout *l) {
  l->position = l->colour = l->specular = l->normal = l->weights = l->indices = l->point_size = l->position1 = l->normal1 = g_no_attribute;
  for (UINT i = 0; i < TEXTURE_SETS; ++i) l->texture[i] = g_no_attribute;
  l->transformed = 0;
  for (UINT i = 0; i < count && e[i].Stream != 0xFF; ++i) {
    const Attribute a = {(BYTE)e[i].Stream, e[i].Type, e[i].Offset};
    if (e[i].Stream >= STREAMS) continue;
    if ((e[i].Usage == D3DDECLUSAGE_POSITION || e[i].Usage == D3DDECLUSAGE_POSITIONT) && e[i].UsageIndex == 0) {
      l->position = a;
      l->transformed = e[i].Usage == D3DDECLUSAGE_POSITIONT;
    } else if (e[i].Usage == D3DDECLUSAGE_COLOR && e[i].UsageIndex == 0) l->colour = a;
    else if (e[i].Usage == D3DDECLUSAGE_COLOR && e[i].UsageIndex == 1) l->specular = a;
    else if (e[i].Usage == D3DDECLUSAGE_NORMAL && e[i].UsageIndex == 0) l->normal = a;
    else if (e[i].Usage == D3DDECLUSAGE_BLENDWEIGHT && e[i].UsageIndex == 0) l->weights = a;
    else if (e[i].Usage == D3DDECLUSAGE_BLENDINDICES && e[i].UsageIndex == 0) l->indices = a;
    else if (e[i].Usage == D3DDECLUSAGE_PSIZE && e[i].UsageIndex == 0) l->point_size = a;
    else if (e[i].Usage == D3DDECLUSAGE_POSITION && e[i].UsageIndex == 1) l->position1 = a;
    else if (e[i].Usage == D3DDECLUSAGE_NORMAL && e[i].UsageIndex == 1) l->normal1 = a;
    else if (e[i].Usage == D3DDECLUSAGE_TEXCOORD && e[i].UsageIndex < TEXTURE_SETS) l->texture[e[i].UsageIndex] = a;
  }
}
/* An FVF code as the elements of a declaration (the end marker is written, and not counted): the position, the blend weights (and the indices of the matrices
 * where D3DFVF_LASTBETA_UBYTE4 or D3DFVF_LASTBETA_D3DCOLOR says the last is that), the normal, the point size, the two colours and the texture coordinates follow
 * each other in the vertex in that order. `e` has room for 17. */
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
      /* (the last "weight" is the indices of the matrices with a D3DFVF_LASTBETA_* flag, and always of five; as Windows converts them, two weights with
       * D3DFVF_LASTBETA_D3DCOLOR are a D3DCOLOR of weights and UBYTE4 indices) */
      UINT weights = (position - D3DFVF_XYZB1) / 2 + 1;
      const BOOL indices = position == D3DFVF_XYZB5 || (fvf & (D3DFVF_LASTBETA_UBYTE4 | D3DFVF_LASTBETA_D3DCOLOR)) != 0;
      const BOOL quirk = position == D3DFVF_XYZB2 && (fvf & D3DFVF_LASTBETA_D3DCOLOR);
      if (indices) --weights;
      if (weights) FVF_ELEMENT(quirk ? D3DDECLTYPE_D3DCOLOR : D3DDECLTYPE_FLOAT1 + weights - 1, D3DDECLUSAGE_BLENDWEIGHT, 0, quirk ? 4 : 4 * weights);
      if (indices)
        FVF_ELEMENT((fvf & D3DFVF_LASTBETA_UBYTE4) || quirk ? D3DDECLTYPE_UBYTE4 : (fvf & D3DFVF_LASTBETA_D3DCOLOR) ? D3DDECLTYPE_D3DCOLOR : D3DDECLTYPE_FLOAT1,
                    D3DDECLUSAGE_BLENDINDICES, 0, 4);
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
  D3DVERTEXELEMENT9 elements[17];
  layout_of_elements(elements, fvf_elements(fvf, elements), l);
}
/* The FVF code Direct3D 9 gives a declaration (GetFVF after SetVertexDeclaration): D3DFVF_XYZ for a position of three floats, D3DFVF_XYZRHW for a transformed one
 * of four, 0 otherwise - the other elements do not count. */
static DWORD decl_fvf(const D3DVERTEXELEMENT9 *e, UINT count) {
  for (UINT i = 0; i < count && e[i].Stream != 0xFF; ++i) {
    if (e[i].UsageIndex != 0) continue;
    if (e[i].Usage == D3DDECLUSAGE_POSITION && e[i].Type == D3DDECLTYPE_FLOAT3) return D3DFVF_XYZ;
    if (e[i].Usage == D3DDECLUSAGE_POSITIONT && e[i].Type == D3DDECLTYPE_FLOAT4) return D3DFVF_XYZRHW;
  }
  return 0;
}

static HRESULT STDMETHODCALLTYPE decl_GetDeclaration(Decl *decl, D3DVERTEXELEMENT9 *elements, UINT *count) {
  if (!count) return D3DERR_INVALIDCALL;
  if (elements) memcpy(elements, decl->elements, sizeof(D3DVERTEXELEMENT9) * decl->count);
  *count = decl->count;
  return D3D_OK;
}
static UINT type_bytes(BYTE type);
/* Makes a declaration of `elements` (ended by D3DDECL_END). `implicit`: the device's own (of an FVF code): it begins at no reference. */
static HRESULT decl_make(Dev *d, const D3DVERTEXELEMENT9 *elements, int implicit, Decl **out) {
  *out = NULL;
  if (!elements) return D3DERR_INVALIDCALL;
  UINT count = 0;
  while (count < MAX_ELEMENTS && elements[count].Stream != 0xFF) {
    const D3DVERTEXELEMENT9 *e = &elements[count];
    /* (Direct3D 9 answers E_FAIL for an element that is not on four bytes, and for one of D3DDECLTYPE_UNUSED) */
    if (e->Offset & 3 || e->Type == D3DDECLTYPE_UNUSED) return E_FAIL;
    if (e->Stream >= STREAMS || e->Type > D3DDECLTYPE_FLOAT16_4 || e->Method > D3DDECLMETHOD_LOOKUPPRESAMPLED || e->Usage > D3DDECLUSAGE_SAMPLE) return D3DERR_INVALIDCALL;
    ++count;
  }
  if (elements[count].Stream != 0xFF || elements[count].Type != D3DDECLTYPE_UNUSED) return D3DERR_INVALIDCALL;
  Decl *decl = child_new_ex(sizeof *decl, d, &decl_vtbl, g_decl_iids, child_destroy, implicit);
  if (!decl) return E_OUTOFMEMORY;
  memcpy(decl->elements, elements, sizeof(D3DVERTEXELEMENT9) * (count + 1));
  decl->count = count + 1;
  decl->fvf = decl_fvf(elements, count);
  layout_of_elements(elements, count, &decl->layout);
  *out = decl;
  return D3D_OK;
}
static HRESULT STDMETHODCALLTYPE dev_CreateVertexDeclaration(Dev *d, const D3DVERTEXELEMENT9 *elements, IDirect3DVertexDeclaration9 **out) {
  if (!out) return D3DERR_INVALIDCALL;
  Decl *decl = NULL;
  const HRESULT hr = decl_make(d, elements, 0, &decl);
  *out = (IDirect3DVertexDeclaration9 *)decl;
  return hr;
}
