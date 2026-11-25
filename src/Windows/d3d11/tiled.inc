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
/* Part of d3d11.c: tiled resources (Direct3D 11.2 and 11.3, TiledResourcesTier 1 to 3 - what Direct3D 12 below has: dev_tiled_tier) over Direct3D 12's reserved resources and
 * heaps. Tier 2's shader instructions (the _s forms of the lookups, check_access_fully_mapped, the lookups' clamps) are in the DXBC the kernel takes, which refuses them on a
 * device without residency feedback.
 *
 * - A tile pool (D3D11_RESOURCE_MISC_TILE_POOL) is a list of Direct3D 12 heaps (TileChunk): the pool's tiles from `first` on are the chunk's heap's. ResizeTilePool adds a
 *   heap for the tiles it grows by; shrinking makes the tiles past the new size the pool's no longer (they may not be mapped again), the heaps staying until the pool goes,
 *   so that a mapping left on them never names freed memory - growing again uses them first. The pool's own `res` is a small buffer that nothing reads (a tile pool is bound
 *   to nothing).
 * - A tiled buffer or texture (D3D11_RESOURCE_MISC_TILED) is a reserved resource of Direct3D 12 (CreateReservedResource): a buffer of the GPU's memory, or a 2D texture, array
 *   or volume in the program's format (Direct3D's tile shapes are the format's texels or blocks). An unmapped tile reads as zero and drops writes (the kernel's reserved
 *   allocations). An array of more than one slice may not have packed levels (tiled_texture_check: Direct3D 11's tiers end at 3).
 * - UpdateTileMappings maps regions of the resource to ranges of the pool: it becomes Direct3D 12 UpdateTileMappings calls on the device's queue after what the context recorded
 *   was sent (the queue runs each list before it returns, so the new mapping is in order with the draws before and after): one for the ranges that unmap, one for each heap
 *   of the pool the ranges reach, the ranges of the others skipped in it (a range across two heaps is split). As in Direct3D 11, a resource is mapped into one pool at a time:
 *   given another pool, its tiles are all unmapped first; given none, the pool of before is used.
 * - CopyTileMappings is Direct3D 12's (the destination taking the source's pool), CopyTiles is Direct3D 12's CopyTiles recorded in the context's list (into a buffer of the
 *   CPU's memory by way of a buffer of the GPU's and a read back), UpdateTiles goes through an upload buffer and CopyTiles, TiledResourceBarrier is an aliasing barrier.
 * - A deferred context records the calls (REC_TILE_MAPPINGS ..., their arguments in the command's bytes) after checking them as the immediate context does.
 * - GetResourceTiling is Direct3D 12's of the reserved resource (the structures are the same). */

#define TILE_BYTES D3D11_2_TILED_RESOURCE_TILE_SIZE_IN_BYTES
typedef struct TileChunk { ID3D12Heap *heap; UINT64 first, count; } TileChunk;
typedef struct TilePool {
  TileChunk *chunks;
  UINT count, cap;
  UINT64 tiles;                /* the pool's size, in tiles (what ResizeTilePool set last) */
  UINT64 made;                 /* the tiles of the heaps (at least `tiles`) */
} TilePool;

static void tile_pool_free(TilePool *p) {
  for (UINT i = 0; i < p->count; ++i) ID3D12Heap_Release(p->chunks[i].heap);
  mem_free(p->chunks);
  mem_free(p);
}

/* The pool's tiles made `tiles` (a heap more for those beyond the heaps it has). */
static HRESULT tile_pool_size(Dev *d, TilePool *p, UINT64 tiles) {
  if (tiles > p->made) {
    const UINT64 more = tiles - p->made;
    if (more > (UINT64)-1 / TILE_BYTES) return E_OUTOFMEMORY;
    if (p->count == p->cap) {
      const UINT cap = p->cap ? p->cap * 2 : 4;
      TileChunk *n = mem_grow(p->chunks, (SIZE_T)cap * sizeof *n);
      if (!n) return E_OUTOFMEMORY;
      p->chunks = n;
      p->cap = cap;
    }
    D3D12_HEAP_DESC hd;
    memset(&hd, 0, sizeof hd);
    hd.SizeInBytes = more * TILE_BYTES;
    hd.Properties.Type = D3D12_HEAP_TYPE_DEFAULT;
    hd.Properties.CreationNodeMask = hd.Properties.VisibleNodeMask = 1;
    hd.Alignment = TILE_BYTES;
    hd.Flags = D3D12_HEAP_FLAG_NONE;
    ID3D12Heap *heap = NULL;
    const HRESULT hr = ID3D12Device_CreateHeap(d->d12, &hd, &IID_ID3D12Heap, (void **)&heap);
    if (FAILED(hr) || !heap) return FAILED(hr) ? hr : E_OUTOFMEMORY;
    p->chunks[p->count].heap = heap;
    p->chunks[p->count].first = p->made;
    p->chunks[p->count].count = more;
    p->count++;
    p->made = tiles;
  }
  p->tiles = tiles;
  return S_OK;
}

/* A tiled buffer or a tile pool, as CreateBuffer makes it (the description was checked: buffer_desc_valid). */
static HRESULT buf_tiled_make(Dev *d, Buf *b) {
  if (b->desc.MiscFlags & D3D11_RESOURCE_MISC_TILE_POOL) {
    TilePool *p = mem_alloc(sizeof *p);
    if (!p) return E_OUTOFMEMORY;
    memset(p, 0, sizeof *p);
    b->tile_pool = p;
    const HRESULT hr = tile_pool_size(d, p, b->desc.ByteWidth / TILE_BYTES);
    if (FAILED(hr)) return hr;
    b->gpu = 1;
    b->bytes = 16;
    b->res = make_buffer_flags(d, D3D12_HEAP_TYPE_DEFAULT, 16, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    return b->res ? S_OK : E_OUTOFMEMORY;
  }
  /* (a tiled buffer: a reserved buffer of the GPU's memory, which unordered access views and stream output may name too) */
  const D3D12_RESOURCE_DESC rd = {D3D12_RESOURCE_DIMENSION_BUFFER, 0, b->desc.ByteWidth, 1, 1, 1, DXGI_FORMAT_UNKNOWN, {1, 0}, D3D12_TEXTURE_LAYOUT_ROW_MAJOR,
                                  D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS};
  const HRESULT hr = ID3D12Device_CreateReservedResource(d->d12, &rd, D3D12_RESOURCE_STATE_COMMON, NULL, &IID_ID3D12Resource, (void **)&b->res);
  if (FAILED(hr) || !b->res) return FAILED(hr) ? hr : E_OUTOFMEMORY;
  b->gpu = 1;
  b->tiled = 1;
  b->bytes = b->desc.ByteWidth;
  return S_OK;
}

/* Whether textures of the format may be tiled: Direct3D 12's D3D12_FORMAT_SUPPORT2_TILED (the formats the kernel keeps of 1 to 16 bytes a texel, not block-compressed, depth
 * or video ones), for a format this runtime knows. */
static BOOL tex_tiled_format(Dev *d, DXGI_FORMAT format) {
  if (!tex_format(format)) return FALSE;
  D3D12_FEATURE_DATA_FORMAT_SUPPORT s = {format, D3D12_FORMAT_SUPPORT1_NONE, D3D12_FORMAT_SUPPORT2_NONE};
  return SUCCEEDED(ID3D12Device_CheckFeatureSupport(d->d12, D3D12_FEATURE_FORMAT_SUPPORT, &s, sizeof s)) && (s.Support2 & D3D12_FORMAT_SUPPORT2_TILED) != 0;
}

/* Whether a reserved texture just made is one Direct3D 11's tiers allow: an array of more than one slice whose levels are all standard - at tier 1 all whole tiles too. */
static BOOL tiled_texture_check(Dev *d, ID3D12Resource *res, const D3D12_RESOURCE_DESC *rd) {
  if (rd->Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || rd->DepthOrArraySize <= 1) return TRUE;
  D3D12_PACKED_MIP_INFO packed;
  D3D12_TILE_SHAPE shape;
  UINT total = 0, none = 0;
  ID3D12Device_GetResourceTiling(d->d12, res, &total, &packed, &shape, &none, 0, NULL);
  if (packed.NumPackedMips) return FALSE;
  if (dev_tiled_tier(d) == D3D11_TILED_RESOURCES_TIER_1 && shape.WidthInTexels && shape.HeightInTexels)
    for (UINT l = 0; l < rd->MipLevels; ++l) {
      const UINT w = (UINT)rd->Width >> l, h = rd->Height >> l;
      if ((w ? w : 1) % shape.WidthInTexels || (h ? h : 1) % shape.HeightInTexels) return FALSE;
    }
  return TRUE;
}

/* The reserved resource of a tiled buffer or texture (NULL: the resource is not tiled), and where it keeps its pool. */
static ID3D12Resource *tiled_of(ID3D11Resource *resource, Buf ***pool_slot) {
  Buf *b = buf_of_resource(resource);
  Tex *t = tex_of_resource(resource);
  if (b && b->tiled) { if (pool_slot) *pool_slot = &b->mapped_pool; return b->res; }
  if (t && t->tiled) { if (pool_slot) *pool_slot = &t->mapped_pool; return t->res; }
  return NULL;
}
static Buf *pool_of(ID3D11Buffer *buffer) {
  Buf *b = buf_of_resource((ID3D11Resource *)buffer);
  return b && b->tile_pool ? b : NULL;
}

/* What GetResourceTiling says of the reserved resource: its tiles, its packed levels and its subresources' tiling (as many as there are, at most `room`). */
typedef struct TileInfo {
  UINT total;
  D3D12_PACKED_MIP_INFO packed;
  UINT subresources;
  BOOL buffer;
  D3D12_SUBRESOURCE_TILING *tilings;      /* (mem_alloc'd; `subresources` of them) */
} TileInfo;
static BOOL tile_info(Dev *d, ID3D12Resource *res, TileInfo *out) {
  memset(out, 0, sizeof *out);
  const D3D12_RESOURCE_DESC rd = ID3D12Resource_GetDesc(res);
  out->buffer = rd.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER;
  const UINT subs = out->buffer ? 1 : (UINT)rd.MipLevels * (rd.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D ? 1u : rd.DepthOrArraySize);
  out->tilings = mem_alloc(sizeof *out->tilings * (subs ? subs : 1));
  if (!out->tilings) return FALSE;
  UINT count = subs;
  D3D12_TILE_SHAPE shape;
  ID3D12Device_GetResourceTiling(d->d12, res, &out->total, &out->packed, &shape, &count, 0, out->tilings);
  out->subresources = out->buffer ? 1 : count;
  if (out->buffer) { memset(out->tilings, 0, sizeof *out->tilings); out->tilings[0].WidthInTiles = out->total; out->tilings[0].HeightInTiles = out->tilings[0].DepthInTiles = 1; }
  return TRUE;
}

/* The tiles a region names (Direct3D 11's rules: a box of a subresource of standard tiles - its depth a volume's tiles in z, an array's slices of the same level -, or
 * `NumTiles` tiles on from the start - through the subresources that follow, for a standard subresource; within the slice's packed tiles, for a packed level), checked
 * against the resource. No size: one tile, or the whole resource for a call of one region with neither starts nor sizes (`whole`). 0: the region is not one of it. */
static UINT64 region_tiles_checked(const TileInfo *t, const D3D11_TILED_RESOURCE_COORDINATE *start, const D3D11_TILE_REGION_SIZE *size, BOOL whole) {
  if (whole && !start && !size) return t->total;
  const D3D11_TILED_RESOURCE_COORDINATE c = start ? *start : (D3D11_TILED_RESOURCE_COORDINATE){0, 0, 0, 0};
  const D3D11_TILE_REGION_SIZE s = size ? *size : (D3D11_TILE_REGION_SIZE){1, FALSE, 0, 0, 0};
  if (!s.NumTiles || c.Subresource >= t->subresources) return 0;
  const UINT mips = t->packed.NumStandardMips + t->packed.NumPackedMips;
  const UINT level = !t->buffer && mips ? c.Subresource % mips : 0;
  const BOOL packed = !t->buffer && level >= t->packed.NumStandardMips && t->packed.NumPackedMips;
  if (packed) {
    /* (a packed level: its tiles are the slice's packed ones, numbered by X) */
    if (s.bUseBox || c.Y || c.Z || c.X >= t->packed.NumTilesForPackedMips || s.NumTiles > t->packed.NumTilesForPackedMips - c.X) return 0;
    return s.NumTiles;
  }
  const D3D12_SUBRESOURCE_TILING *st = &t->tilings[c.Subresource];
  if (c.X >= st->WidthInTiles || c.Y >= st->HeightInTiles || c.Z >= st->DepthInTiles) return 0;
  if (s.bUseBox) {
    /* (an array's box goes on through the slices: as many as are left from this one) */
    const UINT64 deep = st->DepthInTiles > 1 || !mips ? (UINT64)st->DepthInTiles - c.Z : (UINT64)(t->subresources - c.Subresource + mips - 1) / mips;
    if (!s.Width || !s.Height || !s.Depth || (UINT64)s.Width * s.Height * s.Depth != s.NumTiles || s.Width > st->WidthInTiles - c.X || s.Height > st->HeightInTiles - c.Y ||
        s.Depth > deep)
      return 0;
    return s.NumTiles;
  }
  const UINT64 first = (UINT64)st->StartTileIndexInOverallResource + ((UINT64)c.Z * st->HeightInTiles + c.Y) * st->WidthInTiles + c.X;
  if (first >= t->total || s.NumTiles > t->total - first) return 0;
  return s.NumTiles;
}

/* Every tile of the resource unmapped (Direct3D 11, given a pool the resource is not mapped into, starts it over). */
static void tile_unmap_all(Dev *d, ID3D12Resource *res, UINT total) {
  if (!total) return;
  const D3D12_TILED_RESOURCE_COORDINATE start = {0, 0, 0, 0};
  const D3D12_TILE_REGION_SIZE size = {total, FALSE, 0, 0, 0};
  const D3D12_TILE_RANGE_FLAGS flag = D3D12_TILE_RANGE_FLAG_NULL;
  const UINT count = total;
  ID3D12CommandQueue_UpdateTileMappings(d->queue, res, 1, &start, &size, NULL, 1, &flag, NULL, &count, D3D12_TILE_MAPPING_FLAG_NONE);
}

/* The arguments of UpdateTileMappings, checked; `regions` and `ranges` at least one each where Direct3D 11 needs them. */
typedef struct TileMappingArgs {
  ID3D11Resource *resource;
  UINT regions;
  const D3D11_TILED_RESOURCE_COORDINATE *starts;
  const D3D11_TILE_REGION_SIZE *sizes;
  ID3D11Buffer *pool;
  UINT ranges;
  const UINT *range_flags, *pool_starts, *range_counts;
  UINT flags;
} TileMappingArgs;

static HRESULT tile_mappings_check(Dev *d, const TileMappingArgs *a, Buf *current_pool) {
  Buf **slot = NULL;
  ID3D12Resource *res = tiled_of(a->resource, &slot);
  if (!res || (a->flags & ~(UINT)D3D11_TILE_MAPPING_NO_OVERWRITE)) return E_INVALIDARG;
  Buf *pool = a->pool ? pool_of(a->pool) : current_pool;
  if (a->pool && !pool) return E_INVALIDARG;
  if (!a->regions) return S_OK;      /* (nothing to map) */
  if (!a->ranges || (!a->range_counts && a->ranges != 1)) return E_INVALIDARG;
  TileInfo info;
  if (!tile_info(d, res, &info)) return E_OUTOFMEMORY;
  UINT64 region_total = 0;
  HRESULT hr = S_OK;
  for (UINT i = 0; i < a->regions && SUCCEEDED(hr); ++i) {
    const UINT64 n = region_tiles_checked(&info, a->starts ? &a->starts[i] : NULL, a->sizes ? &a->sizes[i] : NULL, a->regions == 1);
    if (!n) hr = E_INVALIDARG;
    region_total += n;
  }
  mem_free(info.tilings);
  if (FAILED(hr)) return hr;
  UINT64 range_total = 0;
  for (UINT j = 0; j < a->ranges; ++j) {
    const UINT f = a->range_flags ? a->range_flags[j] : 0;
    const UINT64 n = a->range_counts ? a->range_counts[j] : region_total;
    if (f != 0 && f != D3D11_TILE_RANGE_NULL && f != D3D11_TILE_RANGE_SKIP && f != D3D11_TILE_RANGE_REUSE_SINGLE_TILE) return E_INVALIDARG;
    range_total += n;
    if (f == D3D11_TILE_RANGE_NULL || f == D3D11_TILE_RANGE_SKIP || !n) continue;
    /* (tiles of the pool: a pool is needed, and the range is within its size) */
    if (!pool) return E_INVALIDARG;
    const UINT64 start = a->pool_starts ? a->pool_starts[j] : 0, used = f == D3D11_TILE_RANGE_REUSE_SINGLE_TILE ? 1 : n;
    if (start >= pool->tile_pool->tiles || used > pool->tile_pool->tiles - start) return E_INVALIDARG;
  }
  return range_total == region_total ? S_OK : E_INVALIDARG;
}

/* The checked call made: Direct3D 12's UpdateTileMappings once for the ranges that unmap and once for each heap of the pool the others reach. */
static void tile_mappings_apply(Dev *d, const TileMappingArgs *a) {
  Buf **slot = NULL;
  ID3D12Resource *res = tiled_of(a->resource, &slot);
  if (!res) return;
  Buf *given = a->pool ? pool_of(a->pool) : NULL;
  if (given && given != *slot) {
    /* (another pool: everything unmapped first) */
    TileInfo info;
    if (*slot && tile_info(d, res, &info)) { tile_unmap_all(d, res, info.total); mem_free(info.tilings); }
    unhold(*slot);
    hold(given);
    *slot = given;
  }
  Buf *pool = *slot;
  if (!a->regions) return;
  /* (the ranges, split where a range crosses from one heap of the pool into the next: kind 0 map, 1 unmap, 2 skip, 4 one tile again) */
  typedef struct Piece { UINT kind; UINT chunk; UINT64 heap_tile; UINT count; } Piece;
  UINT64 region_total = 0;
  if (a->regions == 1 && !a->starts && !a->sizes) {
    /* (one region with neither a start nor a size: the whole resource) */
    TileInfo info;
    if (!tile_info(d, res, &info)) return;
    region_total = info.total;
    mem_free(info.tilings);
  } else {
    for (UINT i = 0; i < a->regions; ++i) region_total += a->sizes ? a->sizes[i].NumTiles : 1;
  }
  const TilePool *tp = pool ? pool->tile_pool : NULL;
  UINT cap = a->ranges + (tp ? tp->count : 0) * a->ranges + 1, n = 0;
  Piece *pieces = mem_alloc(sizeof *pieces * cap);
  D3D12_TILE_RANGE_FLAGS *flags = mem_alloc(sizeof *flags * cap);
  UINT *starts = mem_alloc(sizeof *starts * cap), *counts = mem_alloc(sizeof *counts * cap);
  if (!pieces || !flags || !starts || !counts) { mem_free(pieces); mem_free(flags); mem_free(starts); mem_free(counts); return; }
  BOOL unmaps = FALSE;
  for (UINT j = 0; j < a->ranges; ++j) {
    const UINT f = a->range_flags ? a->range_flags[j] : 0;
    UINT64 left = a->range_counts ? a->range_counts[j] : region_total;
    UINT64 at = a->pool_starts ? a->pool_starts[j] : 0;
    if (!left) continue;
    if (f == D3D11_TILE_RANGE_NULL || f == D3D11_TILE_RANGE_SKIP || !tp) {
      pieces[n++] = (Piece){f == D3D11_TILE_RANGE_SKIP ? 2u : 1u, 0, 0, (UINT)left};
      if (f != D3D11_TILE_RANGE_SKIP) unmaps = TRUE;
      continue;
    }
    while (left && n < cap) {
      UINT k = 0;
      while (k + 1 < tp->count && at >= tp->chunks[k].first + tp->chunks[k].count) ++k;
      const TileChunk *ch = &tp->chunks[k];
      if (f == D3D11_TILE_RANGE_REUSE_SINGLE_TILE) { pieces[n++] = (Piece){4, k, at - ch->first, (UINT)left}; break; }
      const UINT64 room = ch->first + ch->count - at, take = left < room ? left : room;
      pieces[n++] = (Piece){0, k, at - ch->first, (UINT)take};
      left -= take;
      at += take;
    }
  }
  const D3D12_TILED_RESOURCE_COORDINATE *starts12 = (const D3D12_TILED_RESOURCE_COORDINATE *)a->starts;      /* (the structures are the same) */
  const D3D12_TILE_REGION_SIZE *sizes12 = (const D3D12_TILE_REGION_SIZE *)a->sizes;
  /* (one call for each heap reached, and one with no heap for the ranges that unmap; the pieces of the others are skipped in it) */
  for (UINT pass = 0; pass <= (tp ? tp->count : 0); ++pass) {
    const BOOL null_pass = pass == (tp ? tp->count : 0);
    BOOL any = null_pass && unmaps;
    for (UINT i = 0; i < n; ++i) {
      const Piece *p = &pieces[i];
      const BOOL mine = null_pass ? p->kind == 1 : ((p->kind == 0 || p->kind == 4) && p->chunk == pass);
      flags[i] = !mine ? D3D12_TILE_RANGE_FLAG_SKIP : null_pass ? D3D12_TILE_RANGE_FLAG_NULL : p->kind == 4 ? D3D12_TILE_RANGE_FLAG_REUSE_SINGLE_TILE : D3D12_TILE_RANGE_FLAG_NONE;
      starts[i] = mine && !null_pass ? (UINT)p->heap_tile : 0;
      counts[i] = p->count;
      if (mine) any = TRUE;
    }
    if (!any || !n) continue;
    ID3D12CommandQueue_UpdateTileMappings(d->queue, res, a->regions, starts12, sizes12, null_pass ? NULL : tp->chunks[pass].heap, n, flags, starts, counts,
                                          D3D12_TILE_MAPPING_FLAG_NONE);
  }
  mem_free(pieces);
  mem_free(flags);
  mem_free(starts);
  mem_free(counts);
}

/* ---- the context's calls ------------------------------------------------------------------------------------------------------------------------------------------ */

/* The bytes a deferred context keeps of an UpdateTileMappings: the counts and which arrays there are, then the arrays. */
typedef struct TileMappingRecord { UINT regions, ranges, flags, has; } TileMappingRecord;      /* has: 1 starts, 2 sizes, 4 range flags, 8 pool starts, 16 counts */

static HRESULT STDMETHODCALLTYPE ctx_UpdateTileMappings(Ctx *c, ID3D11Resource *resource, UINT regions, const D3D11_TILED_RESOURCE_COORDINATE *starts,
                                                        const D3D11_TILE_REGION_SIZE *sizes, ID3D11Buffer *pool, UINT ranges, const UINT *range_flags,
                                                        const UINT *pool_starts, const UINT *range_counts, UINT flags) {
  const TileMappingArgs a = {resource, regions, starts, sizes, pool, ranges, range_flags, pool_starts, range_counts, flags};
  Dev *d = c->dev;
  Buf **slot = NULL;
  if (!tiled_of(resource, &slot)) return E_INVALIDARG;
  LOCK(c);
  HRESULT hr = tile_mappings_check(d, &a, *slot);
  if (SUCCEEDED(hr) && c->deferred) {
    /* (recorded: the arguments in the command's bytes) */
    const SIZE_T bytes = sizeof(TileMappingRecord) + (starts ? sizeof *starts * regions : 0) + (sizes ? sizeof *sizes * regions : 0) +
                         sizeof(UINT) * ranges * ((range_flags ? 1 : 0) + (pool_starts ? 1 : 0) + (range_counts ? 1 : 0));
    BYTE *blob = mem_alloc(bytes);
    if (!blob) hr = E_OUTOFMEMORY;
    else {
      TileMappingRecord head = {regions, ranges, flags, (starts ? 1u : 0u) | (sizes ? 2u : 0u) | (range_flags ? 4u : 0u) | (pool_starts ? 8u : 0u) | (range_counts ? 16u : 0u)};
      SIZE_T at = 0;
      memcpy(blob, &head, sizeof head); at += sizeof head;
      if (starts) { memcpy(blob + at, starts, sizeof *starts * regions); at += sizeof *starts * regions; }
      if (sizes) { memcpy(blob + at, sizes, sizeof *sizes * regions); at += sizeof *sizes * regions; }
      if (range_flags) { memcpy(blob + at, range_flags, sizeof(UINT) * ranges); at += sizeof(UINT) * ranges; }
      if (pool_starts) { memcpy(blob + at, pool_starts, sizeof(UINT) * ranges); at += sizeof(UINT) * ranges; }
      if (range_counts) { memcpy(blob + at, range_counts, sizeof(UINT) * ranges); at += sizeof(UINT) * ranges; }
      deferred_record(c, &(RecArgs){.op = REC_TILE_MAPPINGS, .obj = {resource, pool}, .data = blob, .bytes = bytes});
      mem_free(blob);
    }
  } else if (SUCCEEDED(hr)) {
    flush(d);      /* (what was recorded first runs with the mapping of before) */
    tile_mappings_apply(d, &a);
  }
  UNLOCK(c);
  return hr;
}

static HRESULT STDMETHODCALLTYPE ctx_CopyTileMappings(Ctx *c, ID3D11Resource *dst, const D3D11_TILED_RESOURCE_COORDINATE *dst_start, ID3D11Resource *src,
                                                      const D3D11_TILED_RESOURCE_COORDINATE *src_start, const D3D11_TILE_REGION_SIZE *size, UINT flags) {
  Dev *d = c->dev;
  Buf **dst_slot = NULL, **src_slot = NULL;
  ID3D12Resource *dst12 = tiled_of(dst, &dst_slot), *src12 = tiled_of(src, &src_slot);
  if (!dst12 || !src12 || !dst_start || !src_start || !size || (flags & ~(UINT)D3D11_TILE_MAPPING_NO_OVERWRITE)) return E_INVALIDARG;
  LOCK(c);
  TileInfo di, si;
  HRESULT hr = S_OK;
  if (!tile_info(d, dst12, &di)) hr = E_OUTOFMEMORY;
  else {
    if (!tile_info(d, src12, &si)) hr = E_OUTOFMEMORY;
    else {
      if (!region_tiles_checked(&di, dst_start, size, FALSE) || !region_tiles_checked(&si, src_start, size, FALSE)) hr = E_INVALIDARG;
      mem_free(si.tilings);
    }
    if (SUCCEEDED(hr) && c->deferred) {
      BYTE blob[sizeof *dst_start * 2 + sizeof *size];
      memcpy(blob, dst_start, sizeof *dst_start);
      memcpy(blob + sizeof *dst_start, src_start, sizeof *src_start);
      memcpy(blob + 2 * sizeof *dst_start, size, sizeof *size);
      deferred_record(c, &(RecArgs){.op = REC_COPY_TILE_MAPPINGS, .obj = {dst, src}, .u = {flags}, .data = blob, .bytes = sizeof blob});
    } else if (SUCCEEDED(hr)) {
      flush(d);
      /* (the destination takes the source's pool: mapped into another, it is unmapped first) */
      if (*src_slot != *dst_slot) {
        tile_unmap_all(d, dst12, di.total);
        unhold(*dst_slot);
        hold(*src_slot);
        *dst_slot = *src_slot;
      }
      ID3D12CommandQueue_CopyTileMappings(d->queue, dst12, (const D3D12_TILED_RESOURCE_COORDINATE *)dst_start, src12, (const D3D12_TILED_RESOURCE_COORDINATE *)src_start,
                                          (const D3D12_TILE_REGION_SIZE *)size, D3D12_TILE_MAPPING_FLAG_NONE);
    }
    mem_free(di.tilings);
  }
  UNLOCK(c);
  return hr;
}

/* The written resource changed: what reads it next sees it. */
static void tiled_written(Dev *d, ID3D11Resource *resource) {
  Buf *b = buf_of_resource(resource);
  Tex *t = tex_of_resource(resource);
  if (b) { b->used = b->written = d->epoch; ++b->version; }
  if (t) { t->used = t->written = d->epoch; ++t->version; }
}

/* CopyTiles on the immediate context, the arguments checked. */
static void tile_copy_apply(Ctx *c, ID3D11Resource *resource, const D3D11_TILED_RESOURCE_COORDINATE *start, const D3D11_TILE_REGION_SIZE *size, Buf *b, UINT64 offset, UINT flags,
                            UINT64 tiles) {
  Dev *d = c->dev;
  ID3D12Resource *res = tiled_of(resource, NULL);
  const BOOL to_resource = (flags & D3D11_TILE_COPY_LINEAR_BUFFER_TO_SWIZZLED_TILED_RESOURCE) != 0;
  const D3D12_TILE_COPY_FLAGS f12 = to_resource ? D3D12_TILE_COPY_FLAG_LINEAR_BUFFER_TO_SWIZZLED_TILED_RESOURCE : D3D12_TILE_COPY_FLAG_SWIZZLED_TILED_RESOURCE_TO_LINEAR_BUFFER;
  const UINT64 bytes = tiles * TILE_BYTES;
  if (b->gpu || to_resource) {
    /* (a buffer of the CPU's memory read by the copy: d3d12.dll sends its memory with the list) */
    if (!b->gpu) buf_touched(b);
    ID3D12GraphicsCommandList_CopyTiles(d->list, res, (const D3D12_TILED_RESOURCE_COORDINATE *)start, (const D3D12_TILE_REGION_SIZE *)size, b->res, offset, f12);
    d->recorded = 1;
    b->used = d->epoch;
    if (to_resource) tiled_written(d, resource);
    else { b->written = d->epoch; ++b->version; }
    return;
  }
  /* (into a buffer of the CPU's memory: by way of one of the GPU's, read back once the list ran) */
  ID3D12Resource *between = make_buffer_flags(d, D3D12_HEAP_TYPE_DEFAULT, bytes, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
  ID3D12Resource *readback = between ? make_buffer(d, D3D12_HEAP_TYPE_READBACK, bytes) : NULL;
  if (!between || !readback) { if (between) ID3D12Resource_Release(between); return; }
  ID3D12GraphicsCommandList_CopyTiles(d->list, res, (const D3D12_TILED_RESOURCE_COORDINATE *)start, (const D3D12_TILE_REGION_SIZE *)size, between, 0, f12);
  ID3D12GraphicsCommandList_CopyBufferRegion(d->list, readback, 0, between, 0, bytes);
  d->recorded = 1;
  flush(d);
  const BYTE *memory = NULL;
  if (SUCCEEDED(ID3D12Resource_Map(readback, 0, NULL, (void **)&memory)) && memory) {
    if (b->used == d->epoch && !buf_rename(d, b, TRUE)) settle(d, b->used);
    memcpy(b->data + offset, memory, (SIZE_T)bytes);
    ID3D12Resource_Unmap(readback, 0, NULL);
    buf_touched(b);
    ++b->version;
  }
  ID3D12Resource_Release(readback);
  ID3D12Resource_Release(between);
}

/* The tiles of a CopyTiles or UpdateTiles region (0: the arguments are not valid). */
static UINT64 tile_region_of(Dev *d, ID3D11Resource *resource, const D3D11_TILED_RESOURCE_COORDINATE *start, const D3D11_TILE_REGION_SIZE *size) {
  ID3D12Resource *res = tiled_of(resource, NULL);
  if (!res || !start || !size) return 0;
  TileInfo info;
  if (!tile_info(d, res, &info)) return 0;
  const UINT64 n = region_tiles_checked(&info, start, size, FALSE);
  mem_free(info.tilings);
  return n;
}

/* CopyTiles: exactly one direction (and NO_OVERWRITE, a promise only); the buffer's bytes from `offset` hold the region's tiles, 64 KiB each, one after the other. An
 * invalid call does nothing (the method returns nothing), as Direct3D 11 drops it. */
static void STDMETHODCALLTYPE ctx_CopyTiles(Ctx *c, ID3D11Resource *resource, const D3D11_TILED_RESOURCE_COORDINATE *start, const D3D11_TILE_REGION_SIZE *size,
                                            ID3D11Buffer *buffer, UINT64 offset, UINT flags) {
  Dev *d = c->dev;
  Buf *b = buf_of_resource((ID3D11Resource *)buffer);
  const UINT direction = flags & (D3D11_TILE_COPY_LINEAR_BUFFER_TO_SWIZZLED_TILED_RESOURCE | D3D11_TILE_COPY_SWIZZLED_TILED_RESOURCE_TO_LINEAR_BUFFER);
  if (!b || b->tile_pool || (void *)b == (void *)resource || (flags & ~(direction | D3D11_TILE_COPY_NO_OVERWRITE)) ||
      (direction != D3D11_TILE_COPY_LINEAR_BUFFER_TO_SWIZZLED_TILED_RESOURCE && direction != D3D11_TILE_COPY_SWIZZLED_TILED_RESOURCE_TO_LINEAR_BUFFER))
    return;
  LOCK(c);
  const UINT64 tiles = tile_region_of(d, resource, start, size);
  if (tiles && offset <= b->desc.ByteWidth && tiles <= (b->desc.ByteWidth - offset) / TILE_BYTES) {
    if (c->deferred) {
      BYTE blob[sizeof *start + sizeof *size];
      memcpy(blob, start, sizeof *start);
      memcpy(blob + sizeof *start, size, sizeof *size);
      deferred_record(c, &(RecArgs){.op = REC_COPY_TILES, .obj = {resource, buffer}, .u = {(UINT)offset, (UINT)(offset >> 32), flags}, .data = blob, .bytes = sizeof blob});
    } else {
      tile_copy_apply(c, resource, start, size, b, offset, flags, tiles);
    }
  }
  UNLOCK(c);
}

/* UpdateTiles: the region's tiles from the program's memory (64 KiB each, one after the other), through an upload buffer and CopyTiles. Only NO_OVERWRITE may be given. */
static void tile_update_apply(Ctx *c, ID3D11Resource *dst, const D3D11_TILED_RESOURCE_COORDINATE *start, const D3D11_TILE_REGION_SIZE *size, const void *data, UINT64 tiles) {
  Dev *d = c->dev;
  ID3D12Resource *res = tiled_of(dst, NULL);
  const UINT64 bytes = tiles * TILE_BYTES;
  if (!retire_room(d, bytes)) flush(d);
  BYTE *memory = NULL;
  UINT64 made = 0;
  ID3D12Resource *upload = take_upload(d, bytes, &memory, &made);
  if (!upload) return;
  memcpy(memory, data, (SIZE_T)bytes);
  ID3D12Resource_Unmap(upload, 0, NULL);
  ID3D12GraphicsCommandList_CopyTiles(d->list, res, (const D3D12_TILED_RESOURCE_COORDINATE *)start, (const D3D12_TILE_REGION_SIZE *)size, upload, 0,
                                      D3D12_TILE_COPY_FLAG_LINEAR_BUFFER_TO_SWIZZLED_TILED_RESOURCE);
  d->recorded = 1;
  tiled_written(d, dst);
  retire(d, upload, NULL, made);
}
static void STDMETHODCALLTYPE ctx_UpdateTiles(Ctx *c, ID3D11Resource *dst, const D3D11_TILED_RESOURCE_COORDINATE *start, const D3D11_TILE_REGION_SIZE *size,
                                              const void *data, UINT flags) {
  Dev *d = c->dev;
  if (!data || (flags & ~(UINT)D3D11_TILE_COPY_NO_OVERWRITE)) return;
  LOCK(c);
  const UINT64 tiles = tile_region_of(d, dst, start, size);
  if (tiles && tiles <= (UINT64)(256u << 20) / TILE_BYTES) {
    if (c->deferred) {
      const SIZE_T bytes = sizeof *start + sizeof *size + (SIZE_T)tiles * TILE_BYTES;
      BYTE *blob = mem_alloc(bytes);
      if (blob) {
        memcpy(blob, start, sizeof *start);
        memcpy(blob + sizeof *start, size, sizeof *size);
        memcpy(blob + sizeof *start + sizeof *size, data, (SIZE_T)tiles * TILE_BYTES);
        deferred_record(c, &(RecArgs){.op = REC_UPDATE_TILES, .obj = {dst}, .data = blob, .bytes = bytes});
        mem_free(blob);
      }
    } else {
      tile_update_apply(c, dst, start, size, data, tiles);
    }
  }
  UNLOCK(c);
}

/* ResizeTilePool: a whole number of 64 KiB tiles, or none. Growing adds a heap (E_OUTOFMEMORY: the pool stays as it was, its mappings too); shrinking - down to nothing -
 * takes the tiles past the size out of the pool: they may not be mapped again, but the mappings into them stay and work (the heaps stay until the pool goes). The buffer's
 * description has the new size. */
static HRESULT STDMETHODCALLTYPE ctx_ResizeTilePool(Ctx *c, ID3D11Buffer *pool, UINT64 size) {
  Buf *b = pool_of(pool);
  if (!b || size % TILE_BYTES || size > 0xFFFFFFFFull) return E_INVALIDARG;
  LOCK(c);
  HRESULT hr = S_OK;
  if (c->deferred) deferred_record(c, &(RecArgs){.op = REC_RESIZE_TILE_POOL, .obj = {pool}, .u = {(UINT)size}});
  else {
    flush(c->dev);
    hr = tile_pool_size(c->dev, b->tile_pool, size / TILE_BYTES);
    if (SUCCEEDED(hr)) b->desc.ByteWidth = (UINT)size;
  }
  UNLOCK(c);
  return hr;
}

/* TiledResourceBarrier: the uses of the one (a resource, or a view's) before it are done before the other's after it - Direct3D 12's aliasing barrier (the queue runs every
 * command in order besides). */
static ID3D12Resource *child_res(ID3D11DeviceChild *child) {
  if (!child) return NULL;
  Buf *b = buf_of_resource((ID3D11Resource *)child);
  Tex *t = tex_of_resource((ID3D11Resource *)child);
  if (b) return b->res;
  if (t) return t->res;
  ID3D11View *view = NULL;
  ID3D12Resource *res = NULL;
  if (SUCCEEDED(ID3D11DeviceChild_QueryInterface(child, &IID_ID3D11View, (void **)&view)) && view) {
    ID3D11Resource *r = NULL;
    ID3D11View_GetResource(view, &r);
    if (r) {
      Buf *vb = buf_of_resource(r);
      Tex *vt = tex_of_resource(r);
      res = vb ? vb->res : vt ? vt->res : NULL;
      ID3D11Resource_Release(r);
    }
    ID3D11View_Release(view);
  }
  return res;
}
static void STDMETHODCALLTYPE ctx_TiledResourceBarrier(Ctx *c, ID3D11DeviceChild *before, ID3D11DeviceChild *after) {
  LOCK(c);
  if (c->deferred) deferred_record(c, &(RecArgs){.op = REC_TILED_BARRIER, .obj = {before, after}});
  else {
    D3D12_RESOURCE_BARRIER barrier;
    memset(&barrier, 0, sizeof barrier);
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_ALIASING;
    barrier.Aliasing.pResourceBefore = child_res(before);
    barrier.Aliasing.pResourceAfter = child_res(after);
    ID3D12GraphicsCommandList_ResourceBarrier(c->dev->list, 1, &barrier);
    c->dev->recorded = 1;
  }
  UNLOCK(c);
}

/* A recorded tile call, run on the immediate context `c` (ExecuteCommandList). */
static void tile_command_run(Ctx *c, const Command *m) {
  const UINT *u = m->u;
  switch (m->op) {
    case REC_TILE_MAPPINGS: {
      TileMappingRecord head;
      if (!m->data || m->bytes < sizeof head) return;
      memcpy(&head, m->data, sizeof head);
      const BYTE *at = m->data + sizeof head;
      const D3D11_TILED_RESOURCE_COORDINATE *starts = NULL;
      const D3D11_TILE_REGION_SIZE *sizes = NULL;
      const UINT *range_flags = NULL, *pool_starts = NULL, *counts = NULL;
      if (head.has & 1) { starts = (const void *)at; at += sizeof *starts * head.regions; }
      if (head.has & 2) { sizes = (const void *)at; at += sizeof *sizes * head.regions; }
      if (head.has & 4) { range_flags = (const void *)at; at += sizeof(UINT) * head.ranges; }
      if (head.has & 8) { pool_starts = (const void *)at; at += sizeof(UINT) * head.ranges; }
      if (head.has & 16) { counts = (const void *)at; at += sizeof(UINT) * head.ranges; }
      ctx_UpdateTileMappings(c, (ID3D11Resource *)m->obj[0], head.regions, starts, sizes, (ID3D11Buffer *)m->obj[1], head.ranges, range_flags, pool_starts, counts, head.flags);
      break;
    }
    case REC_COPY_TILE_MAPPINGS: {
      D3D11_TILED_RESOURCE_COORDINATE dst_start, src_start;
      D3D11_TILE_REGION_SIZE size;
      if (!m->data || m->bytes != sizeof dst_start * 2 + sizeof size) return;
      memcpy(&dst_start, m->data, sizeof dst_start);
      memcpy(&src_start, m->data + sizeof dst_start, sizeof src_start);
      memcpy(&size, m->data + 2 * sizeof dst_start, sizeof size);
      ctx_CopyTileMappings(c, (ID3D11Resource *)m->obj[0], &dst_start, (ID3D11Resource *)m->obj[1], &src_start, &size, u[0]);
      break;
    }
    case REC_COPY_TILES: {
      D3D11_TILED_RESOURCE_COORDINATE start;
      D3D11_TILE_REGION_SIZE size;
      if (!m->data || m->bytes != sizeof start + sizeof size) return;
      memcpy(&start, m->data, sizeof start);
      memcpy(&size, m->data + sizeof start, sizeof size);
      ctx_CopyTiles(c, (ID3D11Resource *)m->obj[0], &start, &size, (ID3D11Buffer *)m->obj[1], u[0] | (UINT64)u[1] << 32, u[2]);
      break;
    }
    case REC_UPDATE_TILES: {
      D3D11_TILED_RESOURCE_COORDINATE start;
      D3D11_TILE_REGION_SIZE size;
      if (!m->data || m->bytes < sizeof start + sizeof size) return;
      memcpy(&start, m->data, sizeof start);
      memcpy(&size, m->data + sizeof start, sizeof size);
      ctx_UpdateTiles(c, (ID3D11Resource *)m->obj[0], &start, &size, m->data + sizeof start + sizeof size, 0);
      break;
    }
    case REC_RESIZE_TILE_POOL: ctx_ResizeTilePool(c, (ID3D11Buffer *)m->obj[0], u[0]); break;
    case REC_TILED_BARRIER: ctx_TiledResourceBarrier(c, (ID3D11DeviceChild *)m->obj[0], (ID3D11DeviceChild *)m->obj[1]); break;
    default: break;
  }
}

/* GetResourceTiling (ID3D11Device2): Direct3D 12's of the reserved resource. A resource that is not tiled has no tiles: the counts are 0 and the descriptions empty. */
static void tiled_resource_tiling(Dev *d, ID3D11Resource *resource, UINT *tile_count, D3D11_PACKED_MIP_DESC *mip_desc, D3D11_TILE_SHAPE *tile_shape, UINT *subresource_count,
                                  UINT first_subresource, D3D11_SUBRESOURCE_TILING *tilings) {
  ID3D12Resource *res = tiled_of(resource, NULL);
  if (!res) {
    if (tile_count) *tile_count = 0;
    if (mip_desc) memset(mip_desc, 0, sizeof *mip_desc);
    if (tile_shape) memset(tile_shape, 0, sizeof *tile_shape);
    if (subresource_count) *subresource_count = 0;
    return;
  }
  _Static_assert(sizeof(D3D11_PACKED_MIP_DESC) == sizeof(D3D12_PACKED_MIP_INFO) && sizeof(D3D11_TILE_SHAPE) == sizeof(D3D12_TILE_SHAPE) &&
                     sizeof(D3D11_SUBRESOURCE_TILING) == sizeof(D3D12_SUBRESOURCE_TILING) && sizeof(D3D11_TILED_RESOURCE_COORDINATE) == sizeof(D3D12_TILED_RESOURCE_COORDINATE) &&
                     sizeof(D3D11_TILE_REGION_SIZE) == sizeof(D3D12_TILE_REGION_SIZE),
                 "the tiling structures of Direct3D 11 and 12 are the same");
  ID3D12Device_GetResourceTiling(d->d12, res, tile_count, (D3D12_PACKED_MIP_INFO *)mip_desc, (D3D12_TILE_SHAPE *)tile_shape, subresource_count, first_subresource,
                                 (D3D12_SUBRESOURCE_TILING *)tilings);
}
