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

/* A mirror of planes of a planar texture (the kernel's VidMm::set_mirror): an image of the kernel's whose contents are those planes', brought up to date by the streams that
 * use it - what a view the kernel's images cannot be is made of. A 2D array of slices `first` .. `first + count - 1` of a plane (a Texture2DArray view), or a 2D image of the
 * plane of slice `first` in the integer format of the same bits (an R8_UINT / R8G8_UINT / R16_UINT / R16G16_UINT view; MIRROR_WRITE_BACK: a render target whose `channels` go
 * back to the plane after each pass). Made when a view first needs it, kept until the resource goes. */
enum { MIRROR_INTEGER = 1, MIRROR_WRITE_BACK = 2 };      /* (kMirrorInteger, kMirrorWriteBack) */
/* (this DLL's own: the mirror is an unordered access view's - a storage image the shaders write, not a render target - of a plane, or of a narrow texture's one image) */
enum { MIRROR_UAV = 4 };
typedef struct Mirror { UINT alloc, plane, first, count, array, mode, channels; } Mirror;

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
  /* A planar texture (NV12, P010, P016: FormatInfo.planar): every plane of every array slice is a 2D allocation of its own - plane_alloc[plane * layers + layer], plane 1 half
   * the size - since the kernel's arrays and render targets are of one plane. `alloc` is plane_alloc[0]. Its subresources are Direct3D's: plane * layers + layer (one level). */
  UINT planes;
  UINT *plane_alloc;
  Mirror *mirrors;             /* ... and the mirrors its views have needed (g_lock) */
  UINT mirror_count;
  /* A CPU-visible texture of D3D12_TEXTURE_LAYOUT_64KB_STANDARD_SWIZZLE: Map with a pointer hands out each subresource in the standard swizzle (src/Windows/tcos_swizzle.h),
   * a copy made from the image at the first Map and put back at each Unmap that wrote (by subresource, with its Map count; g_lock) */
  BYTE **swizzled;
  LONG *swizzle_maps;
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

/* The tile numbering the kernel has (VidMm.cpp): a buffer is a row of 64 KiB tiles; a texture has the tiles of its standard mip levels (at least a tile in both directions), level after level,
 * row by row, and then the packed levels (the mip tail): their texels one level after the other, over as many tiles as they fill. A tile has Direct3D's standard shape for the
 * bytes of a texel of the format (tile_texels): 8 bits 256 x 256, 16 bits 256 x 128, 32 bits 128 x 128, 64 bits 128 x 64, 128 bits 64 x 64; a tile in a heap is its rows one
 * after the other, `tile_w` texels each (the kernel widens the texels of R8 / R8G8 / R16 / R16G16 into its images and narrows them back: AllocationDesc::tile_narrow). */
typedef struct TileLayout {
  UINT standard;               /* the standard mip levels */
  UINT mips;
  UINT total;
  UINT first[16], across[16], down[16];
  UINT tail_first;             /* the first tile of the packed levels (when standard < mips) */
  UINT tail_tiles;             /* ... and how many they fill */
  UINT tile_w, tile_h;         /* a texture's tile shape, in texels */
  UINT texel;                  /* ... and the bytes of a texel in the heap */
  int buffer;                  /* a buffer: its tiles are numbered by X alone */
} TileLayout;

/* The standard tile shape for texels of `bytes` (0 x 0 for a size that has none). */
static void tile_texels(UINT bytes, UINT *w, UINT *h) {
  switch (bytes) {
    case 1: *w = 256; *h = 256; return;
    case 2: *w = 256; *h = 128; return;
    case 4: *w = 128; *h = 128; return;
    case 8: *w = 128; *h = 64; return;
    case 16: *w = 64; *h = 64; return;
    default: *w = *h = 0; return;
  }
}

static void tile_layout(const D3D12_RESOURCE_DESC *d, TileLayout *t) {
  memset(t, 0, sizeof *t);
  if (d->Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {
    t->buffer = 1;
    t->mips = 1;
    t->total = t->across[0] = (UINT)((d->Width + KTILE_BYTES - 1) / KTILE_BYTES);
    t->down[0] = 1;
    return;
  }
  const FormatInfo *fi = format_info(d->Format);
  t->texel = fi ? fi->bytes : 4;
  tile_texels(t->texel, &t->tile_w, &t->tile_h);
  if (!t->tile_w) return;
  t->mips = d->MipLevels ? d->MipLevels : 1;
  if (t->mips > 16) t->mips = 16;
  while (t->standard < t->mips && mip_extent((UINT)d->Width, t->standard) >= t->tile_w && mip_extent(d->Height, t->standard) >= t->tile_h) {
    const UINT l = t->standard++;
    t->first[l] = t->total;
    t->across[l] = (mip_extent((UINT)d->Width, l) + t->tile_w - 1) / t->tile_w;
    t->down[l] = (mip_extent(d->Height, l) + t->tile_h - 1) / t->tile_h;
    t->total += t->across[l] * t->down[l];
  }
  t->tail_first = t->total;
  UINT64 packed = 0;
  for (UINT l = t->standard; l < t->mips; ++l) packed += (UINT64)mip_extent((UINT)d->Width, l) * mip_extent(d->Height, l) * t->texel;
  t->tail_tiles = (UINT)((packed + KTILE_BYTES - 1) / KTILE_BYTES);
  t->total += t->tail_tiles;
}

/* The linear number of the tile at a coordinate (Z is not used: textures are 2D); (UINT)-1 for one that is not there. */
static UINT tile_number(const TileLayout *t, D3D12_TILED_RESOURCE_COORDINATE c) {
  UINT n;
  if (t->buffer) n = c.Subresource == 0 ? c.X : (UINT)-1;
  else if (c.Subresource < t->standard) {
    if (c.X >= t->across[c.Subresource] || c.Y >= t->down[c.Subresource]) return (UINT)-1;
    n = t->first[c.Subresource] + c.Y * t->across[c.Subresource] + c.X;
  }
  else if (c.Subresource < t->mips) n = c.X < t->tail_tiles ? t->tail_first + c.X : (UINT)-1;      /* (a packed level: X counts the packed tiles) */
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
static UINT res_subresources_of(const Resource *r);
static void resource_destroy(DxObj *o) {
  Resource *r = (Resource *)o;
  if (r->alloc && (r->desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL)) depth_companion_drop(r->d.dev, r->alloc);
  for (UINT i = 0; i < r->mirror_count; ++i) kmt_destroy_allocation(r->d.dev->kdev, r->mirrors[i].alloc);      /* (each holds its planes in the kernel until it goes) */
  mem_free(r->mirrors);
  if (r->plane_alloc) {
    for (UINT i = 1; i < r->planes * r->layers; ++i)      /* (the first is `alloc`, given back below) */
      if (r->plane_alloc[i]) kmt_destroy_allocation(r->d.dev->kdev, r->plane_alloc[i]);
    mem_free(r->plane_alloc);
  }
  EnterCriticalSection(&g_lock);
  for (Resource **pp = &g_cpu_resources; *pp; pp = &(*pp)->next_cpu)
    if (*pp == r) { *pp = r->next_cpu; break; }
  LeaveCriticalSection(&g_lock);
  if (r->va) va_unregister(r->va);
  if (r->alloc) kmt_destroy_allocation(r->d.dev->kdev, r->alloc);
  mem_free(r->shadow);
  mem_free(r->tile_map);
  if (r->swizzled) {
    for (UINT i = 0; i < res_subresources_of(r); ++i) mem_free(r->swizzled[i]);
    mem_free(r->swizzled);
    mem_free(r->swizzle_maps);
  }
  if (r->heap) dx_Release(OBJ(r->heap));
  dobj_child_destroy(o);
}

/* ---- mirrors of planes ------------------------------------------------------------------------------------------------------------------------------------------------------ */

static void planar_extent(const D3D12_RESOURCE_DESC *d, const FormatInfo *fi, UINT layers, UINT i, UINT *w, UINT *h);
/* The integer format of a plane's bits: the kernel's R8G8B8A8_UINT of an 8-bit plane, R16G16B16A16_UINT of a 16-bit one. */
static UINT plane_integer_kernel_format(const FormatInfo *fi) { return fi->planar == PLANAR_16 ? KF_RGBA16UINT : KF_RGBA8UINT; }
/* Whether `f` is an integer view of a plane of r (R8_UINT the luma of NV12, R8G8_UINT its U and V; R16_UINT / R16G16_UINT those of P010 and P016): the plane and the
 * channels the view has (1: red, 3: red and green). */
static int plane_integer_view(const Resource *r, DXGI_FORMAT f, UINT *plane, UINT *channels) {
  if (!r->planes || !r->fmt || !r->fmt->planar) return 0;
  const int sixteen = r->fmt->planar == PLANAR_16;
  /* (V208 / V408: three R8 planes, named by the PlaneSlice) */
  if (format_planes(r->fmt) == 3) { if (f != DXGI_FORMAT_R8_UINT) return 0; *channels = 1; return 1; }
  if (f == (sixteen ? DXGI_FORMAT_R16_UINT : DXGI_FORMAT_R8_UINT)) { *plane = 0; *channels = 1; return 1; }
  if (f == (sixteen ? DXGI_FORMAT_R16G16_UINT : DXGI_FORMAT_R8G8_UINT)) { *plane = 1; *channels = 3; return 1; }
  return 0;
}
/* The kernel's allocation of a mirror of r (made the first time it is asked for, then the same one): 0 when the description is not one of r's planes and slices or the
 * kernel cannot make it (a device without arrays of that format, or without integer formats). */
static UINT res_mirror(Resource *r, UINT plane, UINT first, UINT count, int array, UINT mode, UINT channels) {
  /* (a texture that is not planar: its one image - a narrow texture of one level and slice - as "plane 0" of its one slice) */
  const int single = !r->planes && r->fmt && r->kind == 0 && r->mips == 1 && plane == 0 && first == 0 && count == 1 && !array;
  if (!single && (!r->planes || plane >= r->planes || count == 0 || first >= r->layers || count > r->layers - first || (!array && count != 1))) return 0;
  EnterCriticalSection(&g_lock);
  for (UINT i = 0; i < r->mirror_count; ++i) {
    const Mirror *m = &r->mirrors[i];
    if (m->plane == plane && m->first == first && m->count == count && m->array == (UINT)array && m->mode == mode && m->channels == channels) {
      const UINT found = m->alloc;
      LeaveCriticalSection(&g_lock);
      return found;
    }
  }
  UINT alloc = 0;
  Mirror *grown = mem_grow(r->mirrors, sizeof(Mirror) * (r->mirror_count + 1));
  if (grown) {
    r->mirrors = grown;
    KmtAllocDesc md;
    memset(&md, 0, sizeof md);
    md.flags = KA_TEXTURE | ((mode & MIRROR_UAV) ? KA_STORAGE_IMAGE : (mode & MIRROR_WRITE_BACK) ? KA_RENDER_TARGET : 0);
    md.format = (mode & MIRROR_INTEGER) ? plane_integer_kernel_format(r->fmt) : r->fmt->kernel;
    if (single) md.width = format_kw(r->fmt, (UINT)r->desc.Width), md.height = r->desc.Height;
    else planar_extent(&r->desc, r->fmt, r->layers, plane * r->layers, &md.width, &md.height);
    md.mips = md.samples = 1;
    md.kind = array ? 2 : 0;
    md.layers = array ? count : 1;
    if (KMT_OK(kmt_create_allocation(r->d.dev->kdev, &md, &alloc, NULL, NULL))) {
      UINT request[5 + 128];
      request[0] = ESC_SET_MIRROR;
      request[1] = alloc;
      request[2] = mode & (MIRROR_INTEGER | MIRROR_WRITE_BACK);
      request[3] = channels;
      request[4] = count;
      for (UINT i = 0; i < count; ++i) request[5 + i] = single ? r->alloc : r->plane_alloc[plane * r->layers + first + i];
      if (KMT_OK(dev_escape(r->d.dev, request, 5 + count, NULL, 0))) {
        const Mirror m = {alloc, plane, first, count, (UINT)array, mode, channels};
        r->mirrors[r->mirror_count++] = m;
      } else {
        kmt_destroy_allocation(r->d.dev->kdev, alloc);
        alloc = 0;
      }
    } else {
      alloc = 0;
    }
  }
  LeaveCriticalSection(&g_lock);
  return alloc;
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
  const FormatInfo *base_fi = format_info(d->Format);
  const UINT bytes = format_bytes(d->Format);
  /* (a planar format: the subresources of plane 1 follow those of plane 0, each plane a footprint of its own format and size) */
  const UINT plane_subs = base_fi && base_fi->planar ? mips * depth_or_array : 0;      /* (the subresources of a plane) */
  for (UINT i = 0; i < count; ++i) {
    const UINT sub = first + i;
    const UINT plane = plane_subs ? sub / plane_subs : 0;
    const FormatInfo *fi = plane_format(base_fi, plane);
    const UINT level = sub % mips;
    UINT w = mip_extent((UINT)d->Width, level), h = mip_extent(d->Height, level);
    plane_extent(base_fi, plane, &w, &h);
    const UINT depth = d->Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D ? mip_extent(depth_or_array, level) : 1;
    const UINT64 row_size = fi ? format_row_bytes(fi, w) : (UINT64)w * bytes;
    const UINT pitch = (UINT)KALIGN(row_size, 256);
    const UINT rows_of = fi ? format_rows(fi, h) : h;
    offset = KALIGN(offset, 512);
    if (layouts) {
      memset(&layouts[i], 0, sizeof layouts[i]);
      layouts[i].Offset = offset;
      layouts[i].Footprint.Format = plane_subs ? fi->dxgi : d->Format;
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

/* A resource made around the allocations another process made (OpenSharedHandle): its handles and backing size - a planar texture's `count` allocations, in its order of
 * planes and slices (plane_alloc), `alloc` being the first. */
typedef struct ResourceSource { UINT alloc, kresource; UINT64 bytes; const UINT *allocs; UINT count; } ResourceSource;

/* The bytes of a pixel of the kernel's format (of a 4 x 4 block of a compressed one): what its images keep in memory, which a narrow format's are wider than. */
static UINT kernel_pixel_bytes(UINT kf) {
  switch (kf) {
    case KF_RGBA16F: case KF_RGBA16: case KF_RGBA16UINT: case KF_RGBA16SINT: case KF_RGBA16SNORM: case KF_BC1: case KF_BC1_SRGB: return 8;
    case KF_RGBA32F: case KF_RGBA32UINT: case KF_RGBA32SINT: case KF_BC2: case KF_BC3: case KF_BC2_SRGB: case KF_BC3_SRGB: return 16;
    default: return 4;
  }
}
/* Whether the kernel's images of the format may have mip levels: its 8-bit and block-compressed ones do, its wider ones (16 and 32-bit channels, 10-bit, integer) have one. */
static int kernel_format_levels(UINT kf) {
  return (kf >= KF_A8R8G8B8 && kf <= KF_X8B8G8R8) || kf == KF_BGRA8_SRGB || kf == KF_RGBA8_SRGB || (kf >= KF_BC1 && kf <= KF_BC3_SRGB);
}
/* The size of plane allocation `i` of a planar texture (plane_alloc's order: plane * layers + slice) of `fi`: the kernel's image of the plane (plane_extent; a packed YUV
 * array's one plane in the kernel's pixels, format_kw). */
static void planar_extent(const D3D12_RESOURCE_DESC *d, const FormatInfo *fi, UINT layers, UINT i, UINT *w, UINT *h) {
  *w = format_kw(fi, (UINT)d->Width);
  *h = d->Height;
  plane_extent(fi, i / layers, w, h);
}
#define PLANE_PLACEMENT 4096u      /* (where each plane of a placed planar texture begins in the heap: a multiple of this) */
/* Where in a heap each of the `count` plane allocations of a placed planar texture of `fi` begins (offsets, from the resource's own offset), and the bytes of all of
 * them: the kernel's images of the planes - widened pixels - one after the other. */
static UINT64 planar_layout(const D3D12_RESOURCE_DESC *d, const FormatInfo *fi, UINT layers, UINT count, UINT64 *offsets) {
  UINT64 at = 0;
  for (UINT i = 0; i < count; ++i) {
    UINT w, h;
    planar_extent(d, fi, layers, i, &w, &h);
    at = KALIGN(at, PLANE_PLACEMENT);
    if (offsets) offsets[i] = at;
    at += (UINT64)w * h * kernel_pixel_bytes(fi->kernel);
  }
  return at;
}
/* The bytes the kernel's image of a texture of this description has in memory (every level and layer, or every plane of a planar one): at least what a placed one
 * takes of its heap (GetResourceAllocationInfo). */
static UINT64 texture_kernel_bytes(const D3D12_RESOURCE_DESC *d) {
  const FormatInfo *fi = format_info(d->Format);
  if (!fi || !fi->kernel) return 0;
  const UINT layers = d->DepthOrArraySize ? d->DepthOrArraySize : 1;
  if (fi->planar) return planar_layout(d, fi, layers, format_planes(fi) * layers, NULL);
  if (format_packed_yuv(d->Format) && layers > 1) return planar_layout(d, fi, layers, layers, NULL);      /* (a packed YUV array: a slice an allocation) */
  const UINT mips = d->MipLevels ? d->MipLevels : mip_count_for((UINT)d->Width, d->Height);
  UINT64 total = 0;
  for (UINT level = 0; level < mips; ++level) {
    const UINT w = format_kw(fi, mip_extent((UINT)d->Width, level)), h = mip_extent(d->Height, level);
    total += fi->compressed ? (UINT64)((w + 3) / 4) * ((h + 3) / 4) * kernel_pixel_bytes(fi->kernel) : (UINT64)w * h * kernel_pixel_bytes(fi->kernel);
  }
  return total * layers;
}
static HRESULT resource_make_from(Dev *dev, const D3D12_HEAP_PROPERTIES *props, D3D12_HEAP_FLAGS hflags, const D3D12_RESOURCE_DESC *desc, HeapObj *heap, UINT64 heap_offset,
                                  int mode, const ResourceSource *source, Resource **out);
static HRESULT resource_make(Dev *dev, const D3D12_HEAP_PROPERTIES *props, D3D12_HEAP_FLAGS hflags, const D3D12_RESOURCE_DESC *desc, HeapObj *heap, UINT64 heap_offset, int mode,
                             Resource **out) {
  return resource_make_from(dev, props, hflags, desc, heap, heap_offset, mode, NULL, out);
}
/* Every pixel of an allocation `w` x `h` (`layers` of it in each of its `mips` levels) to zeros with the maximum alpha (1 for an integer format: `sixteen` 2): what a narrow
 * format's image starts as. The kernel's lock of a level is that level's texels, all layers, rows exactly the level's width. */
static void res_fill_alpha(Dev *dev, UINT alloc, UINT w, UINT h, UINT layers, UINT mips, int sixteen) {
  for (UINT level = 0; level < mips; ++level) {
    void *data = NULL;
    if (!KMT_OK(kmt_lock(dev->kdev, alloc, level, 0, &data)) || !data) return;
    const UINT64 pixels = (UINT64)mip_extent(w, level) * mip_extent(h, level) * layers;
    if (sixteen) { UINT16 *p = data; for (UINT64 i = 0; i < pixels; ++i) { p[4 * i] = p[4 * i + 1] = p[4 * i + 2] = 0; p[4 * i + 3] = sixteen == 2 ? 1 : 0xFFFF; } }
    else { UINT *p = data; for (UINT64 i = 0; i < pixels; ++i) p[i] = 0xFF000000u; }
    kmt_unlock(dev->kdev, alloc);
  }
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
  /* (the 64 KiB layouts: UNDEFINED_SWIZZLE is a reserved resource's; STANDARD_SWIZZLE a 2D or 3D texture of one sample, not a depth buffer, a video format or R32G32B32, of
   * elements of 1 to 16 bytes - the kernel's image is what draws and copies use; a CPU-visible one is mapped in that layout: res_Map) */
  if (!reserved && desc->Layout != D3D12_TEXTURE_LAYOUT_UNKNOWN && desc->Layout != D3D12_TEXTURE_LAYOUT_ROW_MAJOR && desc->Layout != D3D12_TEXTURE_LAYOUT_64KB_STANDARD_SWIZZLE)
    return E_INVALIDARG;
  if (desc->Layout == D3D12_TEXTURE_LAYOUT_64KB_STANDARD_SWIZZLE && !reserved) {
    const FormatInfo *sf = format_info(desc->Format);
    const UINT bpe = sf ? sf->bytes : 0;
    if ((desc->Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D && desc->Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE3D) || desc->SampleDesc.Count > 1 ||
        (desc->Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) || !sf || sf->planar || sf->pair || format_packed_yuv(desc->Format) || sf->depth ||
        (bpe != 1 && bpe != 2 && bpe != 4 && bpe != 8 && bpe != 16))
      return E_INVALIDARG;
  }
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
      if (props->Type != D3D12_HEAP_TYPE_CUSTOM || (desc->Layout != D3D12_TEXTURE_LAYOUT_UNKNOWN && desc->Layout != D3D12_TEXTURE_LAYOUT_64KB_STANDARD_SWIZZLE) || samples > 1 || reserved ||
          (desc->Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL)) { hr = E_INVALIDARG; goto fail; }
      r->cpu_texture = 1;
      r->cpu_upload = r->cpu_readback = 0;      /* (no shadow: the image itself is written and read) */
    }
    if (desc->Format == DXGI_FORMAT_R32_TYPELESS && want_depth) fi = format_info(DXGI_FORMAT_D32_FLOAT);
    if (desc->Format == DXGI_FORMAT_R24G8_TYPELESS && want_depth) fi = format_info(DXGI_FORMAT_D24_UNORM_S8_UINT);
    if (desc->Format == DXGI_FORMAT_R16_TYPELESS && want_depth) fi = format_info(DXGI_FORMAT_D16_UNORM);
    if (desc->Format == DXGI_FORMAT_R32G8X24_TYPELESS && want_depth) fi = format_info(DXGI_FORMAT_D32_FLOAT_S8X24_UINT);
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
    if (fi->planar) {
      /* (NV12, P010, P016, P208, V208, V408: a 2D texture or array of one level, not multisampled or depth; each plane of each slice is a 2D image of the kernel's, so the
       * slices may be render targets. Committed, placed (the planes one after the other in the heap: planar_layout), shared (every plane allocation in one resource of
       * win32k's) or in a custom heap the CPU sees (each plane a subresource of Write/ReadFromSubresource: res_texture_copy). Not reserved: tiling is not one of these
       * formats' supports (D3D12_FORMAT_SUPPORT2_TILED - DXGI's format tables have "Tiled Resource" disallowed for every video format -), so a reserved one is a
       * description the device refuses) */
      /* (P208's chroma is half as wide - its width even -, V208's half as high - its height even -, V408's full size; the 4:2:0 formats' half of both) */
      const int even_w = fi->planar != PLANAR_V208 && fi->planar != PLANAR_V408, even_h = fi->planar != PLANAR_P208 && fi->planar != PLANAR_V408;
      if (desc->Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || cube || r->mips != 1 || samples > 1 || want_depth || (even_w && (desc->Width & 1)) || (even_h && (desc->Height & 1)) || reserved) { hr = E_INVALIDARG; goto fail; }
      if (format_planes(fi) * r->layers > KMT_MAX_RESOURCE_ALLOCATIONS) { hr = E_INVALIDARG; goto fail; }      /* (every plane of every slice in one resource) */
      r->kind = 0;
      r->planes = format_planes(fi);
    }
    if (format_packed_yuv(desc->Format)) {
      /* (the packed YUV formats: 2D textures and arrays, not multisampled, not reserved (no video format is tiled: format_tiled); mip levels for AYUV and Y416 alone, as DXGI's
       * format tables have them; YUY2, Y210, Y216 of even width (a pixel of the kernel's image is a pair); render targets of AYUV alone - DXGI gives the others shader resource
       * and unordered access views only) */
      if (desc->Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || cube || (r->mips != 1 && !format_packed_yuv_mips(desc->Format)) || samples > 1 || (fi->pair && (desc->Width & 1)) ||
          (target && desc->Format != DXGI_FORMAT_AYUV) || reserved) { hr = E_INVALIDARG; goto fail; }
      /* (Y416's levels: Windows has them, the kernel's 16-bit images have one level - as for every 64-bit format here) */
      if (r->mips > 1 && !kernel_format_levels(fi->kernel)) { hr = DXGI_ERROR_UNSUPPORTED; goto fail; }
      if (r->layers > 1) {
        /* (an array: each slice an allocation of its own, as a planar texture's planes - `planes` 1 - so that a slice is a 2D image the decoder and the processor write;
         * committed, placed, shared or CPU visible as a planar texture is. Of one level: the kernel's images of several layers have one, as for every format) */
        if (r->mips > 1) { hr = DXGI_ERROR_UNSUPPORTED; goto fail; }
        if (r->layers > KMT_MAX_RESOURCE_ALLOCATIONS) { hr = E_INVALIDARG; goto fail; }      /* (every slice in one resource) */
        r->kind = 0;
        r->planes = 1;
      }
    }
    /* (the kernel's arrays, volumes and cubes have one level; render targets and depth buffers one level too, and of several slices on a device that draws into layers) */
    if ((r->kind != 0 && r->mips > 1) || (r->mips > 1 && (target || want_depth))) { hr = DXGI_ERROR_UNSUPPORTED; goto fail; }
    if (r->kind != 0 && (target || want_depth) && !(dev->caps.features2 & DEV_FEATURE2_LAYERED_TARGETS)) { hr = DXGI_ERROR_UNSUPPORTED; goto fail; }
    if (r->kind == 1 && want_depth) { hr = E_INVALIDARG; goto fail; }      /* (a volume is not a depth buffer) */
    /* (unordered access: the kernel's storage images, of a colour format that is not sRGB and one sample; a planar or narrow texture is written through mirrors of its planes
     * - res_mirror -, so its own images are not storage images; a cube's views are of an array, which the kernel's cube is not: no unordered access to cubes) */
    const int uav = (desc->Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) != 0;
    if (uav && (fi->depth || fi->kernel == KF_BGRA8_SRGB || fi->kernel == KF_RGBA8_SRGB || samples > 1)) { hr = E_INVALIDARG; goto fail; }
    if (uav && (!(dev->caps.features2 & DEV_FEATURE2_STORAGE_IMAGES) || r->kind == 3)) { hr = DXGI_ERROR_UNSUPPORTED; goto fail; }
    ad.flags = want_depth ? KA_DEPTH : (target ? (KA_RENDER_TARGET | KA_TEXTURE) : KA_TEXTURE);
    /* (a depth buffer is a texture too - shader resource views of its depth, or of a D24S8's stencil: shadow maps - unless the program says it never will be, on a device that
     * reads them; one of several samples is drawn into only) */
    if (want_depth && !(desc->Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE) && samples == 1 && (dev->caps.features2 & DEV_FEATURE2_DEPTH_TEXTURES)) ad.flags |= KA_TEXTURE;
    if (uav && !r->planes && !fi->narrow) ad.flags |= KA_STORAGE_IMAGE;
    if (reserved) {
      /* (the kernel's reserved textures: of the formats tiling is a support of (format_tiled: not the video formats, whose tiles Direct3D does not define, nor
       * block-compressed or depth ones), any size, with mip levels - the 64-bit and 128-bit formats one: the kernel's wide images have one -, only sampled) */
      if (!format_tiled(fi) || r->planes) { hr = E_INVALIDARG; goto fail; }
      if (r->kind != 0 || target || want_depth || (r->mips > 1 && !kernel_format_levels(fi->kernel))) { hr = DXGI_ERROR_UNSUPPORTED; goto fail; }
      ad.flags = KA_TEXTURE | KA_RESERVED;
      ad.tile_narrow = fi->narrow;      /* (NARROW_* is the kernel's NarrowLayout) */
      r->reserved = 1;
    }
    ad.format = fi->kernel;
    ad.width = format_kw(fi, (UINT)desc->Width);
    ad.height = desc->Height;
    ad.mips = r->mips;
    ad.kind = r->kind;
    ad.layers = r->planes ? 1 : r->layers;
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
    tile_layout(&r->desc, &layout);      /* (the description with its mip count made whole) */
    r->tile_count = layout.total;
    r->tile_map = mem_alloc((SIZE_T)layout.total * sizeof(TileBind));
    if (!r->tile_map) { hr = E_OUTOFMEMORY; goto fail; }
  }
  /* (shared: a resource of win32k's that NT handles share, with what an opener needs to make the same resource; GPU memory only, committed, not tiled) */
  NTSTATUS st;
  const UINT extra_size = dev->next_share_extra_size;
  dev->next_share_extra_size = 0;      /* (it was for this resource) */
  /* (a planar texture: plane 0 of every slice, then plane 1 of every slice - each an allocation, sized and placed as planar_extent / planar_layout say) */
  const UINT plane_count = r->planes * r->layers;
  KmtAllocDesc *plane_descs = NULL;
  if (r->planes) {
    r->plane_alloc = mem_alloc(sizeof(UINT) * plane_count);
    plane_descs = mem_alloc(sizeof(KmtAllocDesc) * plane_count);
    UINT64 *offsets = mem_alloc(sizeof(UINT64) * plane_count);
    if (!r->plane_alloc || !plane_descs || !offsets) { mem_free(plane_descs); mem_free(offsets); hr = E_OUTOFMEMORY; goto fail; }
    planar_layout(desc, r->fmt, r->layers, plane_count, offsets);
    for (UINT i = 0; i < plane_count; ++i) {
      plane_descs[i] = ad;
      planar_extent(desc, r->fmt, r->layers, i, &plane_descs[i].width, &plane_descs[i].height);
      if (heap) plane_descs[i].heap_offset = heap_offset + offsets[i];
    }
    mem_free(offsets);
  }
  if (source) {
    /* (opened: the allocations are the resource's from now on) */
    if ((r->planes && (source->count != plane_count || !source->allocs)) || (!r->planes && source->count > 1)) { mem_free(plane_descs); hr = E_INVALIDARG; goto fail; }
    if (r->planes) memcpy(r->plane_alloc, source->allocs, sizeof(UINT) * plane_count);
    r->alloc = r->planes ? r->plane_alloc[0] : source->alloc;
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
    UINT *global_out = global_share && !(hflags & D3D12_HEAP_FLAG_SHARED) ? &r->global_share : NULL;
    if (r->planes) {
      /* (all the planes of all the slices one resource of win32k's: an opener gets every allocation) */
      UINT64 *plane_bytes = mem_alloc(sizeof(UINT64) * plane_count);
      st = plane_bytes ? kmt_create_allocations_shared(dev->kdev, plane_descs, plane_count, blob, sizeof blob, &r->kresource, global_out, r->plane_alloc, NULL, plane_bytes)
                       : (NTSTATUS)0xC0000017u;
      r->bytes = 0;
      for (UINT i = 0; KMT_OK(st) && i < plane_count; ++i) r->bytes += plane_bytes[i];
      mem_free(plane_bytes);
      if (KMT_OK(st)) r->alloc = r->plane_alloc[0];
    } else {
      st = kmt_create_allocation_shared(dev->kdev, &ad, blob, sizeof blob, &r->kresource, global_out, &r->alloc, &pitch, &r->bytes);
    }
  } else if (r->planes) {
    st = 0;
    r->bytes = 0;
    for (UINT i = 0; KMT_OK(st) && i < plane_count; ++i) {
      UINT64 plane_bytes = 0;
      st = kmt_create_allocation(dev->kdev, &plane_descs[i], &r->plane_alloc[i], &pitch, &plane_bytes);
      r->bytes += plane_bytes;
    }
    r->alloc = r->plane_alloc[0];
  } else {
    st = kmt_create_allocation(dev->kdev, &ad, &r->alloc, &pitch, &r->bytes);
  }
  mem_free(plane_descs);
  if (!KMT_OK(st)) { hr = hr_from(st); goto fail; }
  /* (a narrow format's image starts as zeros in its channels and the maximum in the alpha the kernel keeps, as a copy into it would leave it) */
  if (r->fmt && r->fmt->narrow && !source && !heap && !reserved) {
    const UINT count = r->planes ? r->planes * r->layers : 1;
    for (UINT i = 0; i < count; ++i) {
      UINT w = (UINT)desc->Width, h = desc->Height;
      if (r->planes) plane_extent(r->fmt, i / r->layers, &w, &h);
      res_fill_alpha(dev, r->planes ? r->plane_alloc[i] : r->alloc, w, h, r->planes ? 1 : r->layers, r->mips, r->fmt->kernel == KF_RGBA16 ? 1 : r->fmt->kernel == KF_RGBA16SINT ? 2 : 0);
    }
  }
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
  if (source) {
    /* (the opened allocations stay the caller's, which gives them back) */
    r->alloc = 0;
    if (r->plane_alloc) memset(r->plane_alloc, 0, sizeof(UINT) * r->planes * r->layers);
  }
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
      const FormatInfo *dfi = format_info(d.Format);
      const UINT subs = mips * (d.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D ? 1 : (d.DepthOrArraySize ? d.DepthOrArraySize : 1)) * (dfi && dfi->planar ? format_planes(dfi) : 1);
      get_footprints(&d, 0, subs, 0, NULL, NULL, NULL, &n);
      /* (what the kernel keeps of it: a narrow format's and a plane's pixels are widened, so a placed one needs more of its heap than its footprints) */
      const UINT64 kernel = texture_kernel_bytes(&d);
      if (kernel > n) n = kernel;
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

static UINT res_subresources(const Resource *r) {
  return r->desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER ? 1 : r->mips * (r->kind == 1 ? 1 : r->layers) * (r->planes ? r->planes : 1);
}
static UINT res_subresources_of(const Resource *r) { return res_subresources(r); }
static HRESULT res_texture_copy(Resource *r, UINT sub, const D3D12_BOX *box, BYTE *mem, UINT row_pitch, UINT slice_pitch, int write);
/* A subresource of a standard-swizzle texture between its swizzled copy and the image (`to_image`), through rows of the format's texels. */
static HRESULT res_swizzle_sync(Resource *r, UINT sub, int to_image) {
  const FormatInfo *fi = r->fmt;
  const UINT level = sub % r->mips, w = mip_extent((UINT)r->desc.Width, level), h = mip_extent(r->desc.Height, level), d = r->kind == 1 ? mip_extent(r->layers, level) : 1;
  const UINT ew = fi->compressed ? (w + 3) / 4 : w, eh = fi->compressed ? (h + 3) / 4 : h;
  TcosSwizzle s;
  if (!tcos_swizzle_init(&s, fi->bytes, r->kind == 1)) return E_INVALIDARG;
  const SIZE_T bytes = tcos_swizzle_bytes(&s, ew, eh, d), pitch = (SIZE_T)format_row_bytes(fi, w), slice = pitch * format_rows(fi, h);
  if (!bytes || slice > 0xFFFFFFFFu) return E_OUTOFMEMORY;
  BYTE *linear = mem_alloc(slice * d);
  if (!linear) return E_OUTOFMEMORY;
  HRESULT hr = S_OK;
  if (!to_image) hr = res_texture_copy(r, sub, NULL, linear, (UINT)pitch, (UINT)slice, 0);
  if (SUCCEEDED(hr) && !tcos_swizzle_copy(&s, r->swizzled[sub], bytes, ew, eh, d, 0, 0, 0, ew, eh, d, linear, pitch, slice, !to_image)) hr = E_FAIL;
  if (SUCCEEDED(hr) && to_image) hr = res_texture_copy(r, sub, NULL, linear, (UINT)pitch, (UINT)slice, 1);
  mem_free(linear);
  return hr;
}
static HRESULT STDMETHODCALLTYPE res_Map(Resource *This, UINT sub, const D3D12_RANGE *read, void **data) {
  if (This->cpu_texture) {
    /* (a texture of the driver's layout has no address the program could use: Map with no pointer only says it will be written and read with Write/ReadFromSubresource. One
     * of the standard swizzle is mapped in that layout) */
    if (data) *data = NULL;
    if (sub >= res_subresources(This)) return E_INVALIDARG;
    if (data && This->desc.Layout == D3D12_TEXTURE_LAYOUT_64KB_STANDARD_SWIZZLE) {
      HRESULT hr = S_OK;
      EnterCriticalSection(&g_lock);
      const UINT subs = res_subresources(This);
      if (!This->swizzled) {
        This->swizzled = mem_alloc(sizeof(BYTE *) * subs);
        This->swizzle_maps = mem_alloc(sizeof(LONG) * subs);
        if (This->swizzled) memset(This->swizzled, 0, sizeof(BYTE *) * subs);
        if (This->swizzle_maps) memset(This->swizzle_maps, 0, sizeof(LONG) * subs);
      }
      if (!This->swizzled || !This->swizzle_maps) hr = E_OUTOFMEMORY;
      else if (!This->swizzled[sub]) {
        const FormatInfo *fi = This->fmt;
        const UINT level = sub % This->mips, w = mip_extent((UINT)This->desc.Width, level), h = mip_extent(This->desc.Height, level);
        const UINT d = This->kind == 1 ? mip_extent(This->layers, level) : 1;
        TcosSwizzle s;
        const SIZE_T bytes = fi && tcos_swizzle_init(&s, fi->bytes, This->kind == 1) ? tcos_swizzle_bytes(&s, fi->compressed ? (w + 3) / 4 : w, fi->compressed ? (h + 3) / 4 : h, d) : 0;
        This->swizzled[sub] = bytes ? mem_alloc(bytes) : NULL;
        if (!This->swizzled[sub]) hr = E_OUTOFMEMORY;
        else {
          memset(This->swizzled[sub], 0, bytes);
          hr = res_swizzle_sync(This, sub, 0);
          if (FAILED(hr)) { mem_free(This->swizzled[sub]); This->swizzled[sub] = NULL; }
        }
      }
      if (SUCCEEDED(hr)) { This->swizzle_maps[sub]++; *data = This->swizzled[sub]; }
      LeaveCriticalSection(&g_lock);
      if (SUCCEEDED(hr)) InterlockedIncrement(&This->map_count);
      return hr;
    }
    if (data) return E_INVALIDARG;
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
  if (This->cpu_texture && This->swizzled && sub < res_subresources(This) && This->swizzled[sub]) {
    /* (a swizzled copy: put back unless the program says it wrote nothing - an empty range -; let go with the last Map) */
    EnterCriticalSection(&g_lock);
    if (!written || written->Begin < written->End) res_swizzle_sync(This, sub, 1);
    if (This->swizzle_maps[sub] > 0 && --This->swizzle_maps[sub] == 0) { mem_free(This->swizzled[sub]); This->swizzled[sub] = NULL; }
    LeaveCriticalSection(&g_lock);
  }
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
 * volume has all its layers one after the other. A planar texture's subresource is a plane of a slice (plane * slices + slice), a packed YUV array's a slice: each an image of
 * its own. The program's texels are the subresource's format (a plane's: R8 / R8G8 / R16 / R16G16); where the kernel keeps them widened (narrow formats and planes) a written
 * texel becomes (r, g or 0, 0, the maximum - 1 for an integer image), as a copy into the image makes it, and a read one is the pixel's first channels. A box that is empty
 * copies nothing. */
static HRESULT res_texture_copy(Resource *r, UINT sub, const D3D12_BOX *box, BYTE *mem, UINT row_pitch, UINT slice_pitch, int write) {
  if (!r->cpu_texture || !mem || !r->fmt || sub >= res_subresources(r)) return E_INVALIDARG;
  const FormatInfo *fi = r->fmt;
  UINT alloc = r->alloc, level = sub % r->mips, layer = sub / r->mips, w, h;
  const FormatInfo *texel = fi;      /* (the format of the program's texels) */
  if (r->planes) {
    /* (a plane of a slice, or a slice of a packed YUV array: an allocation of one level and layer) */
    alloc = r->plane_alloc[sub];
    texel = plane_format(fi, sub / r->layers);
    w = (UINT)r->desc.Width;
    h = r->desc.Height;
    plane_extent(fi, sub / r->layers, &w, &h);
    level = layer = 0;
  } else {
    w = mip_extent((UINT)r->desc.Width, level);
    h = mip_extent(r->desc.Height, level);
  }
  if (!texel) return E_INVALIDARG;
  const UINT d = r->kind == 1 ? mip_extent(r->layers, level) : 1;
  D3D12_BOX b = {0, 0, 0, w, h, d};
  if (box) b = *box;
  if (b.left >= b.right || b.top >= b.bottom || b.front >= b.back) return S_OK;
  if (b.right > w || b.bottom > h || b.back > d) return E_INVALIDARG;
  if (fi->compressed && (b.left % 4 || b.top % 4 || (b.right % 4 && b.right != w) || (b.bottom % 4 && b.bottom != h))) return E_INVALIDARG;
  if (fi->pair && (b.left % 2 || (b.right % 2 && b.right != w))) return E_INVALIDARG;      /* (whole pairs of pixels) */
  /* (the kernel's image: a pixel kernel_pixel_bytes wide - a pair's, a block's -, or the texel itself) */
  const UINT kpixel = kernel_pixel_bytes(fi->kernel);
  const int narrow = texel->narrow != NARROW_NONE;
  const UINT64 pitch = narrow ? (UINT64)w * kpixel : format_row_bytes(fi, w), plane = pitch * format_rows(fi, h);
  const UINT64 x_bytes = narrow ? (UINT64)b.left * kpixel : format_row_bytes(fi, b.left);
  const UINT64 row_bytes = format_row_bytes(texel, b.right) - format_row_bytes(texel, b.left);      /* (the program's) */
  const UINT first_row = fi->compressed ? b.top / 4 : b.top, rows = format_rows(fi, b.bottom) - first_row;
  /* (the kernel's subresource: the level of a 2D texture; a layered one has one level, the layers in it) */
  const UINT ksub = r->kind == 0 ? level : 0;
  const UINT64 base = r->kind == 1 || r->planes ? 0 : (UINT64)layer * plane;
  void *data = NULL;
  NTSTATUS st = 0;
  for (UINT tries = 0; tries < 2000; ++tries) {      /* (the GPU's work that still uses it, a queue waiting for a fence: up to two seconds) */
    st = kmt_lock(r->d.dev->kdev, alloc, ksub, write ? 0 : KMT_LOCK_READ_ONLY, &data);
    if (st != (NTSTATUS)0xC01E0102L) break;      /* (STATUS_GRAPHICS_ALLOCATION_BUSY) */
    Sleep(1);
  }
  if (!KMT_OK(st)) return hr_from(st);
  if (!data) { kmt_unlock(r->d.dev->kdev, alloc); return E_FAIL; }
  BYTE *image = data;
  /* (a narrow texel: one or two channels of 8 or 16 bits in a pixel of four; the missing alpha is the maximum, or 1 in an integer image) */
  const UINT sample = narrow ? (texel->narrow == NARROW_R16 || texel->narrow == NARROW_R16G16 ? 2 : 1) : 0, texel_bytes = texel->bytes;
  const int integer = fi->kernel == KF_RGBA16SINT || fi->kernel == KF_RGBA16UINT || fi->kernel == KF_RGBA8UINT;
  for (UINT z = b.front; z < b.back; ++z)
    for (UINT y = 0; y < rows; ++y) {
      BYTE *pixels = image + base + (UINT64)z * plane + (UINT64)(first_row + y) * pitch + x_bytes;
      BYTE *program = mem + (UINT64)(z - b.front) * slice_pitch + (UINT64)y * row_pitch;
      if (!narrow) {
        if (write) memcpy(pixels, program, (SIZE_T)row_bytes);
        else memcpy(program, pixels, (SIZE_T)row_bytes);
        continue;
      }
      for (UINT x = 0; x < b.right - b.left; ++x) {
        BYTE *px = pixels + (UINT64)x * kpixel, *tx = program + (UINT64)x * texel_bytes;
        if (!write) { memcpy(tx, px, texel_bytes); continue; }
        memset(px, 0, kpixel);
        memcpy(px, tx, texel_bytes);
        if (integer) px[3 * sample] = 1;
        else memset(px + 3 * sample, 0xFF, sample);
      }
    }
  kmt_unlock(r->d.dev->kdev, alloc);
  return S_OK;
}
static HRESULT STDMETHODCALLTYPE res_WriteToSubresource(Resource *This, UINT sub, const D3D12_BOX *box, const void *src, UINT row_pitch, UINT slice_pitch) {
  return res_texture_copy(This, sub, box, (BYTE *)(UINT_PTR)src, row_pitch, slice_pitch, 1);      /* (only read from: the one helper takes both directions) */
}
static HRESULT STDMETHODCALLTYPE res_ReadFromSubresource(Resource *This, void *dst, UINT row_pitch, UINT slice_pitch, UINT sub, const D3D12_BOX *box) {
  return res_texture_copy(This, sub, box, dst, row_pitch, slice_pitch, 0);
}
