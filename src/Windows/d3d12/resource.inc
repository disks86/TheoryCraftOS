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

/* d3d12.dll: resources (ID3D12Resource), heaps (ID3D12Heap) and the footprints of subresources. Included by d3d12.c.
 *
 * A resource is one allocation of the kernel (src/Kernel/Dxgkrnl/VidMm.hpp). The kernel's allocations cannot be mapped for a long time (Lock gives a copy that Unlock writes back),
 * so a buffer in an upload or readback heap keeps its CPU copy here (`shadow`): Map hands that out; the copy goes to the kernel before the GPU work that may read it runs
 * (res_push, from ExecuteCommandLists), and a readback buffer is read from the kernel when it is mapped (res_pull). */

typedef struct TileBind {
  struct HeapObj *heap;      /* a reserved resource's tile: where it is (NULL: nowhere) */
  UINT64 tile;
} TileBind;

typedef struct HeapObj {
  DObj d;
  D3D12_HEAP_DESC desc;
  UINT alloc;
  UINT kresource;              /* made D3D12_HEAP_FLAG_SHARED (or opened from an NT handle): win32k's resource, what CreateSharedHandle shares */
} HeapObj;

struct Resource {
  DObj d;
  D3D12_RESOURCE_DESC desc;
  D3D12_HEAP_PROPERTIES props;
  D3D12_HEAP_FLAGS heap_flags;
  UINT alloc;                  /* the kernel's allocation */
  UINT64 va;                   /* buffers: the made-up GPU address */
  UINT64 bytes;                /* the kernel's backing */
  const FormatInfo *fmt;       /* textures */
  UINT mips, layers, kind;     /* kind: the kernel's TextureKind (0 2D, 1 3D, 2 2D array, 3 cube) */
  BYTE *shadow;                /* the CPU's copy (upload / readback buffers) */
  int cpu_upload;              /* the CPU writes it, the GPU reads (shadow goes to the kernel) */
  int cpu_readback;            /* the GPU writes it, the CPU reads */
  LONG map_count;
  int dirty;
  UINT64 sum;                  /* of the shadow as the kernel has it */
  int pushed;
  int cpu_texture;             /* a texture of a custom heap the CPU sees: WriteToSubresource / ReadFromSubresource lock the kernel's image */
  UINT kresource;              /* made D3D12_HEAP_FLAG_SHARED (or opened from an NT handle): win32k's resource, what CreateSharedHandle shares */
  BYTE share_extra[64];        /* ... and what it carries for the runtime above (TcosD3D12ShareExtra / TcosD3D12SharedExtra) */
  UINT global_share;           /* made TCOS_RESOURCE_GLOBAL_SHARE (or opened by one): its global share handle, the runtime above's legacy shared handle */
  UINT share_extra_size;
  int reserved;                /* a tiled resource: the tiles are mapped to heaps with UpdateTileMappings */
  TileBind *tile_map;          /* ... and where each tile is, as UpdateTileMappings left it (CopyTileMappings copies from here) */
  UINT tile_count;
  HeapObj *heap;               /* placed: the heap (a reference) */
  struct Resource *next_cpu;   /* the list of the resources with a shadow */
};

static Resource *g_cpu_resources;

static const GUID *const g_resource_iids[] = {&IID_ID3D12Object, &IID_ID3D12DeviceChild, &IID_ID3D12Pageable, &IID_ID3D12Resource, &IID_ID3D12Resource1, &IID_ID3D12Resource2, NULL};
static const GUID *const g_heap_iids[] = {&IID_ID3D12Object, &IID_ID3D12DeviceChild, &IID_ID3D12Pageable, &IID_ID3D12Heap, &IID_ID3D12Heap1, NULL};

static UINT64 fnv(const BYTE *p, UINT64 n) {
  UINT64 h = 1469598103934665603ull;
  for (UINT64 i = 0; i < n; ++i) { h ^= p[i]; h *= 1099511628211ull; }
  return h;
}

/* Sends the CPU's copy to the kernel when it changed. A buffer mapped now may have been written at any time, so its copy is compared by a hash with what was
 * sent; one unmapped since (`dirty`) is known to have changed and goes without the hash (hashing every buffer at every ExecuteCommandLists, byte by byte,
 * was the biggest cost of d3d12.dll in Chrome). */
static void res_push(Resource *r) {
  if (!r->shadow || !r->alloc) return;
  const UINT64 n = r->desc.Width;
  const int watched = r->map_count > 0;
  const UINT64 sum = watched ? fnv(r->shadow, n) : 0;
  if (watched && r->pushed && sum == r->sum) { r->dirty = 0; return; }
  void *data = NULL;
  if (!KMT_OK(kmt_lock(r->d.dev->kdev, r->alloc, 0, 0, &data))) return;
  memcpy(data, r->shadow, (SIZE_T)(n < r->bytes ? n : r->bytes));
  kmt_unlock(r->d.dev->kdev, r->alloc);
  r->sum = watched ? sum : ~sum;      /* (unwatched: no hash taken; the next comparison, once mapped again, sends it) */
  r->pushed = 1;
  r->dirty = 0;
}
/* Reads the kernel's copy into the CPU's. */
static void res_pull(Resource *r) {
  if (!r->shadow || !r->alloc) return;
  void *data = NULL;
  if (!KMT_OK(kmt_lock(r->d.dev->kdev, r->alloc, 0, KMT_LOCK_READ_ONLY, &data))) return;
  memcpy(r->shadow, data, (SIZE_T)(r->desc.Width < r->bytes ? r->desc.Width : r->bytes));
  kmt_unlock(r->d.dev->kdev, r->alloc);
}
static void res_push_all(void) {
  EnterCriticalSection(&g_lock);
  for (Resource *r = g_cpu_resources; r; r = r->next_cpu)
    if (r->cpu_upload && (r->map_count > 0 || r->dirty || !r->pushed)) res_push(r);
  LeaveCriticalSection(&g_lock);
}

static UINT mip_count_for(UINT w, UINT h) {
  UINT n = 1, m = w > h ? w : h;
  while (m > 1) { m >>= 1; ++n; }
  return n;
}
static UINT mip_extent(UINT size, UINT level) { UINT v = size >> level; return v ? v : 1; }

/* ---- the tiles of a reserved resource ---------------------------------------------------------------------------------------------------------------------------------- */

#define TILE_TEXELS 128
/* The tile numbering the kernel has (VidMm.cpp): a buffer is a row of 64 KiB tiles; a texture has the tiles of its standard mip levels (at least a tile in both directions), level after level,
 * row by row, and then one tile for all the smaller levels together (the mip tail). */
typedef struct TileLayout {
  UINT standard;               /* the standard mip levels */
  UINT mips;
  UINT total;
  UINT first[16], across[16], down[16];
  UINT tail_first;             /* the tail tile (when standard < mips) */
} TileLayout;

static void tile_layout(const D3D12_RESOURCE_DESC *d, TileLayout *t) {
  memset(t, 0, sizeof *t);
  if (d->Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {
    t->mips = 1;
    t->total = t->across[0] = (UINT)((d->Width + KTILE_BYTES - 1) / KTILE_BYTES);
    t->down[0] = 1;
    return;
  }
  t->mips = d->MipLevels ? d->MipLevels : 1;
  if (t->mips > 16) t->mips = 16;
  while (t->standard < t->mips && mip_extent((UINT)d->Width, t->standard) >= TILE_TEXELS && mip_extent(d->Height, t->standard) >= TILE_TEXELS) {
    const UINT l = t->standard++;
    t->first[l] = t->total;
    t->across[l] = (mip_extent((UINT)d->Width, l) + TILE_TEXELS - 1) / TILE_TEXELS;
    t->down[l] = (mip_extent(d->Height, l) + TILE_TEXELS - 1) / TILE_TEXELS;
    t->total += t->across[l] * t->down[l];
  }
  t->tail_first = t->total;
  if (t->standard < t->mips) t->total++;
}

/* The linear number of the tile at a coordinate (Z is not used: textures are 2D); (UINT)-1 for one that is not there. */
static UINT tile_number(const TileLayout *t, D3D12_TILED_RESOURCE_COORDINATE c) {
  UINT n;
  if (c.Subresource < t->standard) n = t->first[c.Subresource] + c.Y * t->across[c.Subresource] + c.X;
  else if (c.Subresource < t->mips) n = t->tail_first + c.X;
  else if (t->mips == 1 && t->standard == 0) n = c.X;
  else return (UINT)-1;
  return n < t->total ? n : (UINT)-1;
}

/* The tiles of a region, appended to out (at most `room`); returns the new count. */
static UINT region_tiles(const TileLayout *t, D3D12_TILED_RESOURCE_COORDINATE c, D3D12_TILE_REGION_SIZE sz, UINT *out, UINT n, UINT room) {
  if (sz.UseBox) {
    for (UINT y = 0; y < sz.Height; ++y)
      for (UINT x = 0; x < sz.Width && n < room; ++x) {
        D3D12_TILED_RESOURCE_COORDINATE at = c;
        at.X += x;
        at.Y += y;
        out[n++] = tile_number(t, at);
      }
  } else {
    const UINT first = tile_number(t, c);
    for (UINT k = 0; k < sz.NumTiles && n < room; ++k) out[n++] = first == (UINT)-1 ? first : first + k;
  }
  return n;
}

static void depth_companion_drop(Dev *dev, UINT depth);      /* (cmdlist.inc) */
static void resource_destroy(DxObj *o) {
  Resource *r = (Resource *)o;
  if (r->alloc && (r->desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL)) depth_companion_drop(r->d.dev, r->alloc);
  EnterCriticalSection(&g_lock);
  for (Resource **pp = &g_cpu_resources; *pp; pp = &(*pp)->next_cpu)
    if (*pp == r) { *pp = r->next_cpu; break; }
  LeaveCriticalSection(&g_lock);
  if (r->va) va_unregister(r->va);
  if (r->alloc) kmt_destroy_allocation(r->d.dev->kdev, r->alloc);
  mem_free(r->shadow);
  mem_free(r->tile_map);
  if (r->heap) dx_Release(OBJ(r->heap));
  dobj_child_destroy(o);
}

/* ---- footprints ---------------------------------------------------------------------------------------------------------------------------------------------------------------- */

/* The layout of subresources `first` .. `first + count - 1` of a resource described by `d` in a buffer from `base` on (the rows 256 apart, each subresource 512 aligned). */
static void get_footprints(const D3D12_RESOURCE_DESC *d, UINT first, UINT count, UINT64 base, D3D12_PLACED_SUBRESOURCE_FOOTPRINT *layouts, UINT *rows, UINT64 *row_sizes, UINT64 *total) {
  UINT64 offset = base;
  UINT64 end = base;
  if (d->Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {
    if (layouts && count) {
      memset(&layouts[0], 0, sizeof layouts[0]);
      layouts[0].Offset = base;
      layouts[0].Footprint.Format = DXGI_FORMAT_UNKNOWN;
      layouts[0].Footprint.Width = (UINT)d->Width;
      layouts[0].Footprint.Height = 1;
      layouts[0].Footprint.Depth = 1;
      layouts[0].Footprint.RowPitch = (UINT)d->Width;
    }
    if (rows && count) rows[0] = 1;
    if (row_sizes && count) row_sizes[0] = d->Width;
    if (total) *total = d->Width;
    return;
  }
  const UINT mips = d->MipLevels ? d->MipLevels : mip_count_for((UINT)d->Width, d->Height);
  const UINT depth_or_array = d->DepthOrArraySize ? d->DepthOrArraySize : 1;
  const FormatInfo *fi = format_info(d->Format);
  const UINT bytes = format_bytes(d->Format);
  for (UINT i = 0; i < count; ++i) {
    const UINT sub = first + i;
    const UINT level = sub % mips;
    const UINT w = mip_extent((UINT)d->Width, level), h = mip_extent(d->Height, level);
    const UINT depth = d->Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D ? mip_extent(depth_or_array, level) : 1;
    const UINT64 row_size = fi ? format_row_bytes(fi, w) : (UINT64)w * bytes;
    const UINT pitch = (UINT)KALIGN(row_size, 256);
    const UINT rows_of = fi ? format_rows(fi, h) : h;
    offset = KALIGN(offset, 512);
    if (layouts) {
      memset(&layouts[i], 0, sizeof layouts[i]);
      layouts[i].Offset = offset;
      layouts[i].Footprint.Format = d->Format;
      layouts[i].Footprint.Width = w;
      layouts[i].Footprint.Height = h;
      layouts[i].Footprint.Depth = depth;
      layouts[i].Footprint.RowPitch = pitch;
    }
    if (rows) rows[i] = rows_of;
    if (row_sizes) row_sizes[i] = row_size;
    /* (the last row of the last slice is not padded) */
    end = offset + (UINT64)pitch * rows_of * (depth - 1) + (UINT64)pitch * (rows_of - 1) + row_size;
    offset += (UINT64)pitch * rows_of * depth;
  }
  if (total) *total = end - base;
}

/* ---- creation ----------------------------------------------------------------------------------------------------------------------------------------------------------------- */

static int heap_cpu_access(const D3D12_HEAP_PROPERTIES *p, int *upload, int *readback) {
  *upload = *readback = 0;
  switch (p->Type) {
    case D3D12_HEAP_TYPE_UPLOAD: *upload = 1; return 1;
    case D3D12_HEAP_TYPE_READBACK: *readback = 1; return 1;
    case D3D12_HEAP_TYPE_CUSTOM:
      if (p->CPUPageProperty == D3D12_CPU_PAGE_PROPERTY_NOT_AVAILABLE) return 0;
      *upload = 1;
      *readback = p->CPUPageProperty == D3D12_CPU_PAGE_PROPERTY_WRITE_BACK;
      return 1;
    default: return 0;
  }
}

/* ---- what a shared resource or heap tells the process that opens it ------------------------------------------------------------------------------------------- */
/* The runtime data of win32k's resource (D3DKMT's PrivateRuntimeData): magic "TCOSD12S", version, kind (0 a resource, 1 a heap), then the description, the heap
 * properties and flags, the heap's size and alignment, and whether the texture is a cube - every field at a fixed offset, the same in both widths (a 32-bit and a
 * 64-bit program share them). */
#define SHARE_DESC_BYTES 192      /* (the fixed part, then at 128 up to 64 bytes of the runtime above: its size at 116) */
static const char g_share_magic[8] = {'T', 'C', 'O', 'S', 'D', '1', '2', 'S'};
typedef struct ShareDesc {
  UINT kind;
  D3D12_RESOURCE_DESC desc;
  D3D12_HEAP_PROPERTIES props;
  D3D12_HEAP_FLAGS flags;
  UINT64 heap_size, heap_alignment;
  UINT cube;
  UINT extra_size;
  BYTE extra[64];
} ShareDesc;
static void share_desc_write(BYTE out[SHARE_DESC_BYTES], const ShareDesc *s) {
  memset(out, 0, SHARE_DESC_BYTES);
  memcpy(out, g_share_magic, 8);
  KPUT32(out, 8, 1);
  KPUT32(out, 12, s->kind);
  KPUT32(out, 16, s->desc.Dimension);
  KPUT64(out, 24, s->desc.Alignment);
  KPUT64(out, 32, s->desc.Width);
  KPUT32(out, 40, s->desc.Height);
  KPUT32(out, 44, (UINT)s->desc.DepthOrArraySize | ((UINT)s->desc.MipLevels << 16));
  KPUT32(out, 48, s->desc.Format);
  KPUT32(out, 52, s->desc.SampleDesc.Count);
  KPUT32(out, 56, s->desc.SampleDesc.Quality);
  KPUT32(out, 60, s->desc.Layout);
  KPUT32(out, 64, s->desc.Flags);
  KPUT32(out, 68, s->props.Type);
  KPUT32(out, 72, s->props.CPUPageProperty);
  KPUT32(out, 76, s->props.MemoryPoolPreference);
  KPUT32(out, 80, s->props.CreationNodeMask);
  KPUT32(out, 84, s->props.VisibleNodeMask);
  KPUT32(out, 88, s->flags);
  KPUT64(out, 96, s->heap_size);
  KPUT64(out, 104, s->heap_alignment);
  KPUT32(out, 112, s->cube);
  KPUT32(out, 116, s->extra_size);
  memcpy(out + 128, s->extra, s->extra_size <= 64 ? s->extra_size : 64);
}
static int share_desc_read(const BYTE *in, UINT size, ShareDesc *s) {
  if (size != SHARE_DESC_BYTES || memcmp(in, g_share_magic, 8) != 0 || KGET32(in, 8) != 1 || KGET32(in, 12) > 1) return 0;
  memset(s, 0, sizeof *s);
  s->kind = KGET32(in, 12);
  s->desc.Dimension = (D3D12_RESOURCE_DIMENSION)KGET32(in, 16);
  s->desc.Alignment = KGET64(in, 24);
  s->desc.Width = KGET64(in, 32);
  s->desc.Height = KGET32(in, 40);
  s->desc.DepthOrArraySize = (UINT16)KGET32(in, 44);
  s->desc.MipLevels = (UINT16)(KGET32(in, 44) >> 16);
  s->desc.Format = (DXGI_FORMAT)KGET32(in, 48);
  s->desc.SampleDesc.Count = KGET32(in, 52);
  s->desc.SampleDesc.Quality = KGET32(in, 56);
  s->desc.Layout = (D3D12_TEXTURE_LAYOUT)KGET32(in, 60);
  s->desc.Flags = (D3D12_RESOURCE_FLAGS)KGET32(in, 64);
  s->props.Type = (D3D12_HEAP_TYPE)KGET32(in, 68);
  s->props.CPUPageProperty = (D3D12_CPU_PAGE_PROPERTY)KGET32(in, 72);
  s->props.MemoryPoolPreference = (D3D12_MEMORY_POOL)KGET32(in, 76);
  s->props.CreationNodeMask = KGET32(in, 80);
  s->props.VisibleNodeMask = KGET32(in, 84);
  s->flags = (D3D12_HEAP_FLAGS)KGET32(in, 88);
  s->heap_size = KGET64(in, 96);
  s->heap_alignment = KGET64(in, 104);
  s->cube = KGET32(in, 112);
  s->extra_size = KGET32(in, 116);
  if (s->extra_size > 64) return 0;
  memcpy(s->extra, in + 128, s->extra_size);
  return 1;
}
/* Whether heap properties are of memory the CPU sees (a shared heap may not be: Direct3D 12 shares GPU memory). */
static int heap_cpu_visible(const D3D12_HEAP_PROPERTIES *p) {
  return p->Type == D3D12_HEAP_TYPE_UPLOAD || p->Type == D3D12_HEAP_TYPE_READBACK || (p->Type == D3D12_HEAP_TYPE_CUSTOM && p->CPUPageProperty != D3D12_CPU_PAGE_PROPERTY_NOT_AVAILABLE);
}

/* A resource made around an allocation another process made (OpenSharedHandle): its handles and backing size. */
typedef struct ResourceSource { UINT alloc, kresource; UINT64 bytes; } ResourceSource;
static HRESULT resource_make_from(Dev *dev, const D3D12_HEAP_PROPERTIES *props, D3D12_HEAP_FLAGS hflags, const D3D12_RESOURCE_DESC *desc, HeapObj *heap, UINT64 heap_offset,
                                  int mode, const ResourceSource *source, Resource **out);
static HRESULT resource_make(Dev *dev, const D3D12_HEAP_PROPERTIES *props, D3D12_HEAP_FLAGS hflags, const D3D12_RESOURCE_DESC *desc, HeapObj *heap, UINT64 heap_offset, int mode,
                             Resource **out) {
  return resource_make_from(dev, props, hflags, desc, heap, heap_offset, mode, NULL, out);
}
static HRESULT resource_make_from(Dev *dev, const D3D12_HEAP_PROPERTIES *props, D3D12_HEAP_FLAGS hflags, const D3D12_RESOURCE_DESC *desc, HeapObj *heap, UINT64 heap_offset,
                                  int mode, const ResourceSource *source, Resource **out) {
  extern const ID3D12Resource2Vtbl res_vtbl;
  KmtAllocDesc ad;
  memset(&ad, 0, sizeof ad);
  ad.mips = 1;
  ad.samples = 1;
  ad.layers = 1;
  /* (a multisampled image: a render target or depth buffer of two dimensions and one level, with a sample count the adapter has; drawn into and resolved) */
  const UINT samples = desc->SampleDesc.Count > 1 ? desc->SampleDesc.Count : 1;
  if (samples > 1 && (samples >= 32 || !((dev->caps.sample_counts >> samples) & 1) || desc->Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || desc->DepthOrArraySize > 1 || desc->MipLevels > 1 || mode == 2 ||
                      !(desc->Flags & (D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL))))
    return samples < 32 && ((dev->caps.sample_counts >> samples) & 1) ? E_INVALIDARG : DXGI_ERROR_UNSUPPORTED;
  ad.samples = samples;
  /* (mode: 0 an ordinary resource, 2 a reserved one, which has the 64 KiB layouts) */
  const int reserved = mode == 2;
  if (!reserved && desc->Layout != D3D12_TEXTURE_LAYOUT_UNKNOWN && desc->Layout != D3D12_TEXTURE_LAYOUT_ROW_MAJOR) return E_INVALIDARG;
  const int cube = (dev->next_resource_flags & TCOS_RESOURCE_CUBE) != 0;
  const int global_share = (dev->next_resource_flags & TCOS_RESOURCE_GLOBAL_SHARE) != 0;
  dev->next_resource_flags = 0;      /* (the options were for this resource) */
  Resource *r = dobj_new(sizeof *r, dev, &res_vtbl, g_resource_iids, resource_destroy);
  if (!r) return E_OUTOFMEMORY;
  r->desc = *desc;
  if (props) r->props = *props;
  r->heap_flags = hflags;
  int cpu = props ? heap_cpu_access(props, &r->cpu_upload, &r->cpu_readback) : 0;
  HRESULT hr = S_OK;
  if (desc->Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {
    if (desc->Width == 0 || desc->Width > 256ull * 1024 * 1024) { hr = E_INVALIDARG; goto fail; }
    ad.flags = KA_VERTEX | KA_INDEX | ((desc->Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) ? KA_UAV : 0);
    /* (any buffer of the GPU's may be a stream output target: Direct3D 12 has no flag for it) */
    if (!cpu && (dev->caps.features & DEV_FEATURE_STREAM_OUTPUT)) ad.flags |= KA_STREAM_OUTPUT;
    ad.size = KALIGN(desc->Width, 16);
    if (ad.size < 16) ad.size = 16;
    if (reserved) { ad.size = KALIGN(desc->Width, KTILE_BYTES); ad.flags |= KA_RESERVED; r->reserved = 1; }
  } else {
    const FormatInfo *fi = format_info(desc->Format);
    UINT depth_or_array = desc->DepthOrArraySize ? desc->DepthOrArraySize : 1;
    const int want_depth = (desc->Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) != 0;
    /* (a one-dimensional texture is a two-dimensional one a texel high - the kernel's shaders take 1D textures as that - and an array of them an array of those) */
    if (desc->Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE1D && desc->Height != 1) { hr = E_INVALIDARG; goto fail; }
    /* (a texture the CPU sees: only in a custom heap - an upload or readback heap holds buffers - with the driver's layout, not a depth buffer or multisampled, and
     * reached with WriteToSubresource / ReadFromSubresource) */
    if (cpu) {
      if (props->Type != D3D12_HEAP_TYPE_CUSTOM || desc->Layout != D3D12_TEXTURE_LAYOUT_UNKNOWN || samples > 1 || reserved ||
          (desc->Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL)) { hr = E_INVALIDARG; goto fail; }
      r->cpu_texture = 1;
      r->cpu_upload = r->cpu_readback = 0;      /* (no shadow: the image itself is written and read) */
    }
    if (desc->Format == DXGI_FORMAT_R32_TYPELESS && want_depth) fi = format_info(DXGI_FORMAT_D32_FLOAT);
    if (desc->Format == DXGI_FORMAT_R24G8_TYPELESS && want_depth) fi = format_info(DXGI_FORMAT_D24_UNORM_S8_UINT);
    if (!fi || !fi->kernel || !!fi->depth != want_depth) { hr = fi && fi->kernel ? E_INVALIDARG : DXGI_ERROR_UNSUPPORTED; goto fail; }
    if (fi->compressed && (desc->Flags & (D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS))) { hr = E_INVALIDARG; goto fail; }      /* (sampled only) */
    if (desc->Width == 0 || desc->Height == 0 || desc->Width > KMAX_TEXTURE_DIMENSION || desc->Height > KMAX_TEXTURE_DIMENSION) { hr = E_INVALIDARG; goto fail; }
    r->fmt = fi;
    r->mips = samples > 1 ? 1 : desc->MipLevels ? desc->MipLevels : mip_count_for((UINT)desc->Width, desc->Height);
    r->layers = 1;
    r->kind = 0;
    if (desc->Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D) { r->kind = 1; r->layers = depth_or_array; }
    else if (cube) {
      /* (a cube: what the runtime above said of it - Direct3D 12 tells a cube from an array of six only by its views; the kernel's cube is six square faces) */
      if (depth_or_array != 6 || desc->Width != desc->Height) { hr = DXGI_ERROR_UNSUPPORTED; goto fail; }
      r->kind = 3;
      r->layers = 6;
    } else if (depth_or_array > 1) { r->kind = 2; r->layers = depth_or_array; }
    const int target = (desc->Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) != 0;
    if ((r->kind != 0 || r->mips > 1) && (target || want_depth || (r->kind != 0 && r->mips > 1))) { hr = DXGI_ERROR_UNSUPPORTED; goto fail; }
    ad.flags = want_depth ? KA_DEPTH : (target ? (KA_RENDER_TARGET | KA_TEXTURE) : KA_TEXTURE);
    if (reserved) {
      /* (the kernel's reserved textures: 4-byte colour pixels, whole tiles, one mip level, only sampled) */
      if (fi->bytes != 4 || fi->depth || !(fi->kernel <= KF_X8B8G8R8 || fi->kernel == KF_BGRA8_SRGB || fi->kernel == KF_RGBA8_SRGB) || desc->Width % 128 || desc->Height % 128 || r->kind != 0 || target || want_depth) { hr = DXGI_ERROR_UNSUPPORTED; goto fail; }
      ad.flags = KA_TEXTURE | KA_RESERVED;
      r->reserved = 1;
    }
    ad.format = fi->kernel;
    ad.width = (UINT)desc->Width;
    ad.height = desc->Height;
    ad.mips = r->mips;
    ad.kind = r->kind;
    ad.layers = r->layers;
    r->desc.MipLevels = (UINT16)r->mips;
  }
  if (heap) {
    ad.heap = heap->alloc;
    ad.heap_offset = heap_offset;
    r->heap = heap;
    dx_AddRef(OBJ(heap));
  }
  UINT pitch = 0;
  if (reserved) {
    TileLayout layout;
    tile_layout(desc, &layout);
    r->tile_count = layout.total;
    r->tile_map = mem_alloc((SIZE_T)layout.total * sizeof(TileBind));
    if (!r->tile_map) { hr = E_OUTOFMEMORY; goto fail; }
  }
  /* (shared: a resource of win32k's that NT handles share, with what an opener needs to make the same resource; GPU memory only, committed, not tiled) */
  NTSTATUS st;
  const UINT extra_size = dev->next_share_extra_size;
  dev->next_share_extra_size = 0;      /* (it was for this resource) */
  if (source) {
    r->alloc = source->alloc;
    r->kresource = source->kresource;
    r->bytes = source->bytes;
    st = 0;
  } else if (!heap && (global_share || (hflags & (D3D12_HEAP_FLAG_SHARED | D3D12_HEAP_FLAG_SHARED_CROSS_ADAPTER)))) {
    /* (a committed resource: a placed one is shared with its heap, whose flags it has) */
    if (hflags & D3D12_HEAP_FLAG_SHARED_CROSS_ADAPTER) { hr = DXGI_ERROR_UNSUPPORTED; goto fail; }      /* (the kernel has no allocations of two adapters) */
    if (cpu || r->cpu_texture || reserved || !props) { hr = E_INVALIDARG; goto fail; }
    ShareDesc sd;
    memset(&sd, 0, sizeof sd);
    sd.kind = 0;
    sd.desc = r->desc;
    sd.props = *props;
    sd.flags = hflags;
    sd.cube = r->kind == 3;
    sd.extra_size = extra_size <= 64 ? extra_size : 0;
    memcpy(sd.extra, dev->next_share_extra, sd.extra_size);
    memcpy(r->share_extra, sd.extra, sd.extra_size);
    r->share_extra_size = sd.extra_size;
    BYTE blob[SHARE_DESC_BYTES];
    share_desc_write(blob, &sd);
    /* (an NT-handle share - D3D12_HEAP_FLAG_SHARED - or, asked for alone, one with a global share handle) */
    st = kmt_create_allocation_shared(dev->kdev, &ad, blob, sizeof blob, &r->kresource, global_share && !(hflags & D3D12_HEAP_FLAG_SHARED) ? &r->global_share : NULL, &r->alloc,
                                      &pitch, &r->bytes);
  } else {
    st = kmt_create_allocation(dev->kdev, &ad, &r->alloc, &pitch, &r->bytes);
  }
  if (!KMT_OK(st)) { hr = hr_from(st); goto fail; }
  if (desc->Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {
    r->va = va_register(r, desc->Width);
    if (cpu && !reserved) {
      r->shadow = mem_alloc((SIZE_T)desc->Width);
      if (!r->shadow) { hr = E_OUTOFMEMORY; goto fail; }
      EnterCriticalSection(&g_lock);
      r->next_cpu = g_cpu_resources;
      g_cpu_resources = r;
      LeaveCriticalSection(&g_lock);
    }
  }
  *out = r;
  return S_OK;
fail:
  dx_Release(OBJ(r));
  return hr;
}

static HRESULT STDMETHODCALLTYPE dev_CreateCommittedResource(Dev *This, const D3D12_HEAP_PROPERTIES *props, D3D12_HEAP_FLAGS flags, const D3D12_RESOURCE_DESC *desc,
                                                              D3D12_RESOURCE_STATES state, const D3D12_CLEAR_VALUE *clear, REFIID riid, void **out) {
  Resource *r = NULL;
  if (!props || !desc) return E_INVALIDARG;
  const HRESULT hr = resource_make(This, props, flags, desc, NULL, 0, 0, &r);
  if (FAILED(hr)) return hr;
  return query_out((IUnknown *)r, riid, out);
}

static HRESULT STDMETHODCALLTYPE dev_CreatePlacedResource(Dev *This, ID3D12Heap *heap, UINT64 offset, const D3D12_RESOURCE_DESC *desc, D3D12_RESOURCE_STATES state,
                                                           const D3D12_CLEAR_VALUE *clear, REFIID riid, void **out) {
  Resource *r = NULL;
  HeapObj *h = (HeapObj *)heap;
  if (!heap || !desc) return E_INVALIDARG;
  const HRESULT hr = resource_make(This, &h->desc.Properties, h->desc.Flags, desc, h, offset, 0, &r);
  if (FAILED(hr)) return hr;
  return query_out((IUnknown *)r, riid, out);
}


static void heap_destroy(DxObj *o) {
  HeapObj *h = (HeapObj *)o;
  if (h->alloc) kmt_destroy_allocation(h->d.dev->kdev, h->alloc);
  dobj_child_destroy(o);
}

static HRESULT STDMETHODCALLTYPE dev_CreateHeap(Dev *This, const D3D12_HEAP_DESC *desc, REFIID riid, void **out) {
  extern const ID3D12Heap1Vtbl heap_vtbl;
  if (!desc || desc->SizeInBytes == 0 || desc->SizeInBytes > 256ull * 1024 * 1024) return E_INVALIDARG;
  HeapObj *h = dobj_new(sizeof *h, This, &heap_vtbl, g_heap_iids, heap_destroy);
  if (!h) return E_OUTOFMEMORY;
  h->desc = *desc;
  KmtAllocDesc ad;
  memset(&ad, 0, sizeof ad);
  ad.flags = KA_HEAP;
  ad.size = KALIGN(desc->SizeInBytes, 256);
  NTSTATUS st;
  if (desc->Flags & D3D12_HEAP_FLAG_SHARED_CROSS_ADAPTER) { dx_Release(OBJ(h)); return DXGI_ERROR_UNSUPPORTED; }
  if (desc->Flags & D3D12_HEAP_FLAG_SHARED) {
    /* (a heap other processes open: GPU memory, its description for them) */
    if (heap_cpu_visible(&desc->Properties)) { dx_Release(OBJ(h)); return E_INVALIDARG; }
    ShareDesc sd;
    memset(&sd, 0, sizeof sd);
    sd.kind = 1;
    sd.props = desc->Properties;
    sd.flags = desc->Flags;
    sd.heap_size = desc->SizeInBytes;
    sd.heap_alignment = desc->Alignment;
    BYTE blob[SHARE_DESC_BYTES];
    share_desc_write(blob, &sd);
    st = kmt_create_allocation_ex(This->kdev, &ad, blob, sizeof blob, &h->kresource, &h->alloc, NULL, NULL);
  } else {
    st = kmt_create_allocation(This->kdev, &ad, &h->alloc, NULL, NULL);
  }
  if (!KMT_OK(st)) { dx_Release(OBJ(h)); return hr_from(st); }
  return query_out((IUnknown *)h, riid, out);
}
static D3D12_HEAP_DESC *STDMETHODCALLTYPE heap_GetDesc(HeapObj *This, D3D12_HEAP_DESC *ret) { *ret = This->desc; return ret; }

static D3D12_RESOURCE_ALLOCATION_INFO *STDMETHODCALLTYPE dev_GetResourceAllocationInfo(Dev *This, D3D12_RESOURCE_ALLOCATION_INFO *ret, UINT mask, UINT count,
                                                                                         const D3D12_RESOURCE_DESC *descs) {
  UINT64 size = 0;
  for (UINT i = 0; i < count; ++i) {
    UINT64 n = 0;
    if (descs[i].Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {
      n = descs[i].Width;
    } else {
      D3D12_RESOURCE_DESC d = descs[i];
      const UINT mips = d.MipLevels ? d.MipLevels : mip_count_for((UINT)d.Width, d.Height);
      const UINT subs = mips * (d.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D ? 1 : (d.DepthOrArraySize ? d.DepthOrArraySize : 1));
      get_footprints(&d, 0, subs, 0, NULL, NULL, NULL, &n);
    }
    size += KALIGN(n, KTILE_BYTES);
  }
  ret->SizeInBytes = size;
  ret->Alignment = KTILE_BYTES;
  return ret;
}

static void STDMETHODCALLTYPE dev_GetCopyableFootprints(Dev *This, const D3D12_RESOURCE_DESC *desc, UINT first, UINT count, UINT64 base, D3D12_PLACED_SUBRESOURCE_FOOTPRINT *layouts,
                                                        UINT *rows, UINT64 *row_sizes, UINT64 *total) {
  get_footprints(desc, first, count, base, layouts, rows, row_sizes, total);
}

/* ---- ID3D12Resource ----------------------------------------------------------------------------------------------------------------------------------------------------------- */

static UINT res_subresources(const Resource *r) { return r->desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER ? 1 : r->mips * (r->kind == 1 ? 1 : r->layers); }
static HRESULT STDMETHODCALLTYPE res_Map(Resource *This, UINT sub, const D3D12_RANGE *read, void **data) {
  if (This->cpu_texture) {
    /* (a texture of the driver's layout has no address the program could use: Map with no pointer only says it will be written and read with Write/ReadFromSubresource) */
    if (data) *data = NULL;
    if (data || sub >= res_subresources(This)) return E_INVALIDARG;
    InterlockedIncrement(&This->map_count);
    return S_OK;
  }
  if (sub != 0 || !This->shadow) return E_INVALIDARG;
  if (This->cpu_readback) res_pull(This);
  InterlockedIncrement(&This->map_count);
  This->dirty = 1;
  if (data) *data = This->shadow;
  return S_OK;
}
static void STDMETHODCALLTYPE res_Unmap(Resource *This, UINT sub, const D3D12_RANGE *written) {
  if (This->map_count > 0) InterlockedDecrement(&This->map_count);
  if (!This->cpu_texture) This->dirty = 1;
}
static D3D12_RESOURCE_DESC *STDMETHODCALLTYPE res_GetDesc(Resource *This, D3D12_RESOURCE_DESC *ret) { *ret = This->desc; return ret; }
static D3D12_GPU_VIRTUAL_ADDRESS STDMETHODCALLTYPE res_GetGPUVirtualAddress(Resource *This) { return This->va; }
static HRESULT STDMETHODCALLTYPE res_GetHeapProperties(Resource *This, D3D12_HEAP_PROPERTIES *props, D3D12_HEAP_FLAGS *flags) {
  if (props) *props = This->heap ? This->heap->desc.Properties : This->props;
  if (flags) *flags = This->heap_flags;
  return S_OK;
}
/* WriteToSubresource / ReadFromSubresource: a box of a subresource of a CPU-visible texture, copied between the program's memory (rows `row_pitch` apart, slices
 * `slice_pitch`) and the kernel's image, which is locked for it (the copy is done when the call returns, as the CPU's copy is on Windows; the program keeps the GPU off
 * the texture meanwhile). The kernel's image is the texels in rows of exactly the level's width (blocks of 4 x 4 for a compressed format); a level of an array, cube or
 * volume has all its layers one after the other. A box that is empty copies nothing. */
static HRESULT res_texture_copy(Resource *r, UINT sub, const D3D12_BOX *box, BYTE *mem, UINT row_pitch, UINT slice_pitch, int write) {
  if (!r->cpu_texture || !mem || !r->fmt || sub >= res_subresources(r)) return E_INVALIDARG;
  const FormatInfo *fi = r->fmt;
  const UINT level = sub % r->mips, layer = sub / r->mips;
  const UINT w = mip_extent((UINT)r->desc.Width, level), h = mip_extent(r->desc.Height, level), d = r->kind == 1 ? r->layers : 1;
  D3D12_BOX b = {0, 0, 0, w, h, d};
  if (box) b = *box;
  if (b.left >= b.right || b.top >= b.bottom || b.front >= b.back) return S_OK;
  if (b.right > w || b.bottom > h || b.back > d) return E_INVALIDARG;
  if (fi->compressed && (b.left % 4 || b.top % 4 || (b.right % 4 && b.right != w) || (b.bottom % 4 && b.bottom != h))) return E_INVALIDARG;
  const UINT64 pitch = format_row_bytes(fi, w), plane = pitch * format_rows(fi, h);
  const UINT64 x_bytes = format_row_bytes(fi, b.left), row_bytes = format_row_bytes(fi, b.right) - x_bytes;
  const UINT first_row = fi->compressed ? b.top / 4 : b.top, rows = format_rows(fi, b.bottom) - first_row;
  /* (the kernel's subresource: the level of a 2D texture; a layered one has one level, the layers in it) */
  const UINT ksub = r->kind == 0 ? level : 0;
  const UINT64 base = r->kind == 1 ? 0 : (UINT64)layer * plane;
  void *data = NULL;
  NTSTATUS st = 0;
  for (UINT tries = 0; tries < 2000; ++tries) {      /* (the GPU's work that still uses it, a queue waiting for a fence: up to two seconds) */
    st = kmt_lock(r->d.dev->kdev, r->alloc, ksub, write ? 0 : KMT_LOCK_READ_ONLY, &data);
    if (st != (NTSTATUS)0xC01E0102L) break;      /* (STATUS_GRAPHICS_ALLOCATION_BUSY) */
    Sleep(1);
  }
  if (!KMT_OK(st)) return hr_from(st);
  if (!data) { kmt_unlock(r->d.dev->kdev, r->alloc); return E_FAIL; }
  BYTE *image = data;
  for (UINT z = b.front; z < b.back; ++z)
    for (UINT y = 0; y < rows; ++y) {
      BYTE *texels = image + base + (UINT64)z * plane + (UINT64)(first_row + y) * pitch + x_bytes;
      BYTE *program = mem + (UINT64)(z - b.front) * slice_pitch + (UINT64)y * row_pitch;
      if (write) memcpy(texels, program, (SIZE_T)row_bytes);
      else memcpy(program, texels, (SIZE_T)row_bytes);
    }
  kmt_unlock(r->d.dev->kdev, r->alloc);
  return S_OK;
}
static HRESULT STDMETHODCALLTYPE res_WriteToSubresource(Resource *This, UINT sub, const D3D12_BOX *box, const void *src, UINT row_pitch, UINT slice_pitch) {
  return res_texture_copy(This, sub, box, (BYTE *)(UINT_PTR)src, row_pitch, slice_pitch, 1);      /* (only read from: the one helper takes both directions) */
}
static HRESULT STDMETHODCALLTYPE res_ReadFromSubresource(Resource *This, void *dst, UINT row_pitch, UINT slice_pitch, UINT sub, const D3D12_BOX *box) {
  return res_texture_copy(This, sub, box, dst, row_pitch, slice_pitch, 0);
}
