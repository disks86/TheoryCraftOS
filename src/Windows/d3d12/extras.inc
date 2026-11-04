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

/* d3d12.dll: queries (ID3D12QueryHeap), indirect execution (ID3D12CommandSignature), bundles and tiled (reserved) resources. Included by d3d12.c after the command lists.
 *
 * Queries are the kernel's (BeginQuery / EndQuery in a render pass write 64-bit counts into a kAllocQueryResult buffer): occlusion and binary occlusion, and timestamps (nanoseconds,
 * GetTimestampFrequency says 10^9). Pipeline statistics and stream output statistics are not made. A query is a begin and an end in one pass; a pass the list has to end early (a
 * copy, a clear, a full chunk) ends the query with it. ResolveQueryData copies the results with CopyBuffer.
 *
 * The kernel has no indirect draws, so ExecuteIndirect is made when the list is executed: the list keeps a copy of its state at the call, the argument buffer is read then (after
 * everything before it has run) and the commands it holds are recorded as a list of their own and run. The arguments may set root constants, root views, a vertex buffer and an index buffer.
 *
 * A bundle records the calls it gets (BUNDLE_LOG) and ExecuteBundle makes them on the list that executes it, so it takes the targets, viewport and the rest from that list.
 *
 * Tiled resources are the kernel's reserved allocations: buffers, and textures of 4-byte colour pixels with a size in tiles of 128 x 128 and one mip level. UpdateTileMappings maps their
 * 64 KiB tiles to places in heaps at once (the queue is synchronous, so that is in order with the lists executed before). Not done: CopyTileMappings, CopyTiles, mip tails. */

/* ---- reading a buffer ----------------------------------------------------------------------------------------------------------------------------------------------------- */

static int res_read(Resource *r, UINT64 offset, UINT64 n, void *dst) {
  if (!r || offset > r->desc.Width || n > r->desc.Width - offset) return 0;
  if (r->shadow) { memcpy(dst, r->shadow + offset, (SIZE_T)n); return 1; }
  void *data = NULL;
  if (!KMT_OK(kmt_lock(r->d.dev->kdev, r->alloc, 0, KMT_LOCK_READ_ONLY, &data))) return 0;
  memcpy(dst, (BYTE *)data + offset, (SIZE_T)n);
  kmt_unlock(r->d.dev->kdev, r->alloc);
  return 1;
}

/* ---- queries ----------------------------------------------------------------------------------------------------------------------------------------------------------------- */

typedef struct QHeap {
  DObj d;
  D3D12_QUERY_HEAP_DESC desc;
  UINT alloc;
} QHeap;
static const GUID *const g_qheap_iids[] = {&IID_ID3D12Object, &IID_ID3D12DeviceChild, &IID_ID3D12Pageable, &IID_ID3D12QueryHeap, NULL};

static void qheap_destroy(DxObj *o) {
  QHeap *h = (QHeap *)o;
  if (h->alloc) kmt_destroy_allocation(h->d.dev->kdev, h->alloc);
  dobj_child_destroy(o);
}

static HRESULT STDMETHODCALLTYPE dev_CreateQueryHeap(Dev *This, const D3D12_QUERY_HEAP_DESC *desc, REFIID riid, void **out) {
  extern const ID3D12QueryHeapVtbl qheap_vtbl;
  if (!desc || desc->Count == 0 || desc->Count > 131072) return E_INVALIDARG;
  if (desc->Type != D3D12_QUERY_HEAP_TYPE_OCCLUSION && desc->Type != D3D12_QUERY_HEAP_TYPE_TIMESTAMP) return DXGI_ERROR_UNSUPPORTED;
  QHeap *h = dobj_new(sizeof *h, This, &qheap_vtbl, g_qheap_iids, qheap_destroy);
  if (!h) return E_OUTOFMEMORY;
  h->desc = *desc;
  KmtAllocDesc ad;
  memset(&ad, 0, sizeof ad);
  ad.flags = KA_QUERY;
  ad.size = (UINT64)desc->Count * 8;
  const NTSTATUS st = kmt_create_allocation(This->kdev, &ad, &h->alloc, NULL, NULL);
  if (!KMT_OK(st)) { dx_Release(OBJ(h)); return hr_from(st); }
  return query_out((IUnknown *)h, riid, out);
}

/* The kernel's query kind of a Direct3D 12 one (0 occlusion, 1 occlusion predicate, 6 timestamp). */
static UINT query_kind(D3D12_QUERY_TYPE t) { return t == D3D12_QUERY_TYPE_OCCLUSION ? 0 : t == D3D12_QUERY_TYPE_BINARY_OCCLUSION ? 1 : 6; }

static void STDMETHODCALLTYPE list_BeginQuery(CmdList *l, ID3D12QueryHeap *heap, D3D12_QUERY_TYPE type, UINT index) {
  QHeap *h = (QHeap *)heap;
  if (l->bundle || !h || index >= h->desc.Count || type == D3D12_QUERY_TYPE_TIMESTAMP || l->query_open) return;
  if (type != D3D12_QUERY_TYPE_OCCLUSION && type != D3D12_QUERY_TYPE_BINARY_OCCLUSION) return;
  ensure_room(l, 512, 4);
  if (chunk_cur(l)->queries >= 60) { end_pass(l); chunk_new(l); }
  if (!ensure_pass(l)) return;
  UINT w[3] = {alloc_index(l, h->alloc), index * 8, query_kind(type)};
  packet(l, OP_BEGIN_QUERY, w, type == D3D12_QUERY_TYPE_OCCLUSION ? 2 : 3);
  chunk_cur(l)->queries++;
  l->query_open = 1;
}

static void STDMETHODCALLTYPE list_EndQuery(CmdList *l, ID3D12QueryHeap *heap, D3D12_QUERY_TYPE type, UINT index) {
  QHeap *h = (QHeap *)heap;
  if (l->bundle || !h || index >= h->desc.Count) return;
  if (type == D3D12_QUERY_TYPE_TIMESTAMP) {
    if (l->query_open) return;      /* (the kernel runs one query at a time) */
    ensure_room(l, 512, 4);
    if (chunk_cur(l)->queries >= 60) { end_pass(l); chunk_new(l); }
    if (!ensure_pass(l)) return;
    UINT w[3] = {alloc_index(l, h->alloc), index * 8, query_kind(type)};
    packet(l, OP_BEGIN_QUERY, w, 3);
    packet(l, OP_END_QUERY, NULL, 0);
    chunk_cur(l)->queries++;
    return;
  }
  if (!l->query_open) return;
  packet(l, OP_END_QUERY, NULL, 0);
  l->query_open = 0;
}

static void STDMETHODCALLTYPE list_ResolveQueryData(CmdList *l, ID3D12QueryHeap *heap, D3D12_QUERY_TYPE type, UINT start, UINT count, ID3D12Resource *dst, UINT64 offset) {
  QHeap *h = (QHeap *)heap;
  Resource *d = (Resource *)dst;
  if (l->bundle || !h || !d || !count || start > h->desc.Count || count > h->desc.Count - start || offset > 0xFFFFFFFFu) return;
  end_pass(l);
  ensure_room(l, 256, 4);
  UINT w[5] = {alloc_index(l, d->alloc), (UINT)offset, alloc_index(l, h->alloc), start * 8, count * 8};
  packet(l, OP_COPY_BUFFER, w, 5);
}

/* ---- command signatures and indirect execution ------------------------------------------------------------------------------------------------------------------------- */

typedef struct CmdSig {
  DObj d;
  UINT stride, nargs;
  D3D12_INDIRECT_ARGUMENT_DESC args[8];
  RootSig *rs;
} CmdSig;
static const GUID *const g_sig_iids[] = {&IID_ID3D12Object, &IID_ID3D12DeviceChild, &IID_ID3D12Pageable, &IID_ID3D12CommandSignature, NULL};

static void sig_destroy(DxObj *o) {
  CmdSig *s = (CmdSig *)o;
  if (s->rs) dx_Release(OBJ(s->rs));
  dobj_child_destroy(o);
}

static HRESULT STDMETHODCALLTYPE dev_CreateCommandSignature(Dev *This, const D3D12_COMMAND_SIGNATURE_DESC *desc, ID3D12RootSignature *rs, REFIID riid, void **out) {
  extern const ID3D12CommandSignatureVtbl sig_vtbl;
  if (!desc || desc->NumArgumentDescs == 0 || desc->NumArgumentDescs > 8 || !desc->pArgumentDescs || desc->ByteStride == 0) return E_INVALIDARG;
  int needs_signature = 0;
  for (UINT i = 0; i < desc->NumArgumentDescs; ++i) {
    const D3D12_INDIRECT_ARGUMENT_TYPE t = desc->pArgumentDescs[i].Type;
    if (t > D3D12_INDIRECT_ARGUMENT_TYPE_UNORDERED_ACCESS_VIEW) return DXGI_ERROR_UNSUPPORTED;
    if (t >= D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT && t != D3D12_INDIRECT_ARGUMENT_TYPE_VERTEX_BUFFER_VIEW && t != D3D12_INDIRECT_ARGUMENT_TYPE_INDEX_BUFFER_VIEW) needs_signature = 1;
    if (t == D3D12_INDIRECT_ARGUMENT_TYPE_VERTEX_BUFFER_VIEW && desc->pArgumentDescs[i].VertexBuffer.Slot != 0) return DXGI_ERROR_UNSUPPORTED;
  }
  const D3D12_INDIRECT_ARGUMENT_TYPE last = desc->pArgumentDescs[desc->NumArgumentDescs - 1].Type;
  if (last != D3D12_INDIRECT_ARGUMENT_TYPE_DRAW && last != D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED && last != D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH) return E_INVALIDARG;
  if (needs_signature && !rs) return E_INVALIDARG;
  CmdSig *s = dobj_new(sizeof *s, This, &sig_vtbl, g_sig_iids, sig_destroy);
  if (!s) return E_OUTOFMEMORY;
  s->stride = desc->ByteStride;
  s->nargs = desc->NumArgumentDescs;
  memcpy(s->args, desc->pArgumentDescs, s->nargs * sizeof s->args[0]);
  if (rs) { s->rs = (RootSig *)rs; dx_AddRef(OBJ(rs)); }
  return query_out((IUnknown *)s, riid, out);
}

typedef struct Indirect {
  CmdList *state;           /* a copy of the list at the call */
  CmdSig *sig;
  Resource *args, *count;
  UINT64 args_offset, count_offset;
  UINT max;
} Indirect;

static void indirect_free(struct Indirect *i) {
  mem_free(i->state);
  mem_free(i);
}

static void chunk_item(CmdList *l, Indirect *in) {
  if (l->nchunks == l->cap_chunks) {
    const UINT cap = l->cap_chunks ? l->cap_chunks * 2 : 4;
    Chunk **n = mem_grow(l->chunks, cap * sizeof *n);
    if (!n) { indirect_free(in); return; }
    l->chunks = n;
    l->cap_chunks = cap;
  }
  Chunk *c = mem_alloc(sizeof *c);
  if (!c) { indirect_free(in); return; }
  c->indirect = in;
  l->chunks[l->nchunks++] = c;
}

static void STDMETHODCALLTYPE list_ExecuteIndirect(CmdList *l, ID3D12CommandSignature *sig, UINT max, ID3D12Resource *args, UINT64 args_offset, ID3D12Resource *count, UINT64 count_offset) {
  keep(l, (IUnknown *)sig);
  BUNDLE_LOG(l, B_INDIRECT, (UINT64)(UINT_PTR)sig, max, (UINT64)(UINT_PTR)args, args_offset, (UINT64)(UINT_PTR)count, count_offset);
  if (!sig || !args || !max) return;
  end_pass(l);
  Indirect *in = mem_alloc(sizeof *in);
  if (!in) return;
  in->state = mem_alloc(sizeof(CmdList));
  if (!in->state) { mem_free(in); return; }
  memcpy(in->state, l, sizeof *l);
  CmdList *s = in->state;
  s->chunks = NULL; s->nchunks = s->cap_chunks = 0;
  s->refs = NULL; s->nrefs = s->cap_refs = 0;
  s->log = NULL; s->nlog = s->cap_log = 0;
  s->bundle = 0; s->pass_open = 0; s->query_open = 0; s->closed = 0;
  in->sig = (CmdSig *)sig;
  in->args = (Resource *)args;
  in->count = (Resource *)count;
  in->args_offset = args_offset;
  in->count_offset = count_offset;
  in->max = max;
  l->pending_clear = 0;      /* (the copy has it: it is applied by the first pass made from it) */
  chunk_item(l, in);
  chunk_new(l);
}

/* The commands of the call as a list of their own (the caller runs its chunks and frees it); NULL if the arguments cannot be read. */
static CmdList *indirect_expand(Indirect *in) {
  const CmdSig *sig = in->sig;
  UINT n = in->max;
  if (in->count) {
    UINT c = 0;
    if (!res_read(in->count, in->count_offset, 4, &c)) return NULL;
    if (c < n) n = c;
  }
  if (!n) return NULL;
  if ((UINT64)n * sig->stride > (1u << 22)) return NULL;
  BYTE *data = mem_alloc((SIZE_T)n * sig->stride);
  if (!data) return NULL;
  if (!res_read(in->args, in->args_offset, (UINT64)n * sig->stride, data)) { mem_free(data); return NULL; }
  CmdList *t = mem_alloc(sizeof *t);
  if (!t) { mem_free(data); return NULL; }
  memcpy(t, in->state, sizeof *t);
  t->dirty_gfx = t->dirty_cs = t->dirty_heaps = 1;
  if (!chunk_new(t)) { mem_free(t); mem_free(data); return NULL; }
  const int compute = sig->args[sig->nargs - 1].Type == D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH;
  Binding *binding = compute ? &t->cs : &t->gfx;
  int *dirty = compute ? &t->dirty_cs : &t->dirty_gfx;
  for (UINT i = 0; i < n; ++i) {
    const BYTE *p = data + (SIZE_T)i * sig->stride;
    for (UINT k = 0; k < sig->nargs; ++k) {
      const D3D12_INDIRECT_ARGUMENT_DESC *a = &sig->args[k];
      const UINT *w = (const UINT *)p;
      switch (a->Type) {
        case D3D12_INDIRECT_ARGUMENT_TYPE_DRAW: list_DrawInstanced(t, w[0], w[1], w[2], w[3]); p += 16; break;
        case D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED: list_DrawIndexedInstanced(t, w[0], w[1], w[2], (INT)w[3], w[4]); p += 20; break;
        case D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH: list_Dispatch(t, w[0], w[1], w[2]); p += 12; break;
        case D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT:
          set_constants(t, binding, dirty, a->Constant.RootParameterIndex, a->Constant.Num32BitValuesToSet, w, a->Constant.DestOffsetIn32BitValues);
          p += 4 * a->Constant.Num32BitValuesToSet;
          break;
        case D3D12_INDIRECT_ARGUMENT_TYPE_VERTEX_BUFFER_VIEW: {
          const D3D12_VERTEX_BUFFER_VIEW v = {*(const UINT64 *)p, w[2], w[3]};
          list_IASetVertexBuffers(t, 0, 1, &v);
          p += 16;
          break;
        }
        case D3D12_INDIRECT_ARGUMENT_TYPE_INDEX_BUFFER_VIEW: {
          const D3D12_INDEX_BUFFER_VIEW v = {*(const UINT64 *)p, w[2], (DXGI_FORMAT)w[3]};
          list_IASetIndexBuffer(t, &v);
          p += 16;
          break;
        }
        case D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT_BUFFER_VIEW: set_view(binding, dirty, a->ConstantBufferView.RootParameterIndex, 2, *(const UINT64 *)p); p += 8; break;
        case D3D12_INDIRECT_ARGUMENT_TYPE_SHADER_RESOURCE_VIEW: set_view(binding, dirty, a->ShaderResourceView.RootParameterIndex, 3, *(const UINT64 *)p); p += 8; break;
        case D3D12_INDIRECT_ARGUMENT_TYPE_UNORDERED_ACCESS_VIEW: set_view(binding, dirty, a->UnorderedAccessView.RootParameterIndex, 4, *(const UINT64 *)p); p += 8; break;
        default: break;
      }
    }
  }
  mem_free(data);
  end_pass(t);
  return t;
}

/* ---- bundles ---------------------------------------------------------------------------------------------------------------------------------------------------------------- */

static void bundle_log(CmdList *l, UINT id, const UINT64 a[6], const UINT *data, UINT ndata) {
  if (l->nlog == l->cap_log) {
    const UINT cap = l->cap_log ? l->cap_log * 2 : 32;
    LogEntry *n = mem_grow(l->log, cap * sizeof *n);
    if (!n) return;
    l->log = n;
    l->cap_log = cap;
  }
  LogEntry *e = &l->log[l->nlog];
  memset(e, 0, sizeof *e);
  e->id = id;
  memcpy(e->a, a, sizeof e->a);
  if (data && ndata && ndata <= 64) {
    e->data = mem_alloc((SIZE_T)ndata * 4);
    if (!e->data) return;
    memcpy(e->data, data, (SIZE_T)ndata * 4);
    e->ndata = ndata;
  }
  l->nlog++;
}

static void STDMETHODCALLTYPE list_ExecuteBundle(CmdList *l, ID3D12GraphicsCommandList *bundle) {
  CmdList *b = (CmdList *)bundle;
  if (!b || !b->bundle || !b->closed || l->bundle) return;
  keep(l, (IUnknown *)bundle);
  for (UINT i = 0; i < b->nlog; ++i) {
    const LogEntry *e = &b->log[i];
    const UINT64 *a = e->a;
    switch (e->id) {
      case B_PSO: list_SetPipelineState(l, (ID3D12PipelineState *)(UINT_PTR)a[0]); break;
      case B_TOPOLOGY: list_IASetPrimitiveTopology(l, (D3D12_PRIMITIVE_TOPOLOGY)a[0]); break;
      case B_STENCIL_REF: list_OMSetStencilRef(l, (UINT)a[0]); break;
      case B_INDEX_BUFFER: {
        const D3D12_INDEX_BUFFER_VIEW v = {a[0], (UINT)a[1], (DXGI_FORMAT)a[2]};
        list_IASetIndexBuffer(l, a[0] ? &v : NULL);
        break;
      }
      case B_VERTEX_BUFFERS: {
        const D3D12_VERTEX_BUFFER_VIEW v = {a[2], (UINT)a[3], (UINT)a[4]};
        if (a[1]) list_IASetVertexBuffers(l, (UINT)a[0], (UINT)a[1], &v);
        break;
      }
      case B_HEAPS: {
        ID3D12DescriptorHeap *hs[2] = {(ID3D12DescriptorHeap *)(UINT_PTR)a[1], (ID3D12DescriptorHeap *)(UINT_PTR)a[2]};
        list_SetDescriptorHeaps(l, (UINT)a[0], hs);
        break;
      }
      case B_GRAPHICS_SIGNATURE: list_SetGraphicsRootSignature(l, (ID3D12RootSignature *)(UINT_PTR)a[0]); break;
      case B_COMPUTE_SIGNATURE: list_SetComputeRootSignature(l, (ID3D12RootSignature *)(UINT_PTR)a[0]); break;
      case B_GRAPHICS_CONSTANTS: list_SetGraphicsRoot32BitConstants(l, (UINT)a[0], e->ndata, e->data, (UINT)a[1]); break;
      case B_COMPUTE_CONSTANTS: list_SetComputeRoot32BitConstants(l, (UINT)a[0], e->ndata, e->data, (UINT)a[1]); break;
      case B_GRAPHICS_CBV: list_SetGraphicsRootConstantBufferView(l, (UINT)a[0], a[1]); break;
      case B_GRAPHICS_SRV: list_SetGraphicsRootShaderResourceView(l, (UINT)a[0], a[1]); break;
      case B_GRAPHICS_UAV: list_SetGraphicsRootUnorderedAccessView(l, (UINT)a[0], a[1]); break;
      case B_COMPUTE_CBV: list_SetComputeRootConstantBufferView(l, (UINT)a[0], a[1]); break;
      case B_COMPUTE_SRV: list_SetComputeRootShaderResourceView(l, (UINT)a[0], a[1]); break;
      case B_COMPUTE_UAV: list_SetComputeRootUnorderedAccessView(l, (UINT)a[0], a[1]); break;
      case B_GRAPHICS_TABLE: list_SetGraphicsRootDescriptorTable(l, (UINT)a[0], (D3D12_GPU_DESCRIPTOR_HANDLE){a[1]}); break;
      case B_COMPUTE_TABLE: list_SetComputeRootDescriptorTable(l, (UINT)a[0], (D3D12_GPU_DESCRIPTOR_HANDLE){a[1]}); break;
      case B_DRAW: list_DrawInstanced(l, (UINT)a[0], (UINT)a[1], (UINT)a[2], (UINT)a[3]); break;
      case B_DRAW_INDEXED: list_DrawIndexedInstanced(l, (UINT)a[0], (UINT)a[1], (UINT)a[2], (INT)(UINT)a[3], (UINT)a[4]); break;
      case B_DISPATCH: list_Dispatch(l, (UINT)a[0], (UINT)a[1], (UINT)a[2]); break;
      case B_INDIRECT:
        list_ExecuteIndirect(l, (ID3D12CommandSignature *)(UINT_PTR)a[0], (UINT)a[1], (ID3D12Resource *)(UINT_PTR)a[2], a[3], (ID3D12Resource *)(UINT_PTR)a[4], a[5]);
        break;
      default: break;
    }
  }
}

/* ---- tiled resources ----------------------------------------------------------------------------------------------------------------------------------------------------- */

#define TILE_TEXELS 128

static HRESULT STDMETHODCALLTYPE dev_CreateReservedResource(Dev *This, const D3D12_RESOURCE_DESC *desc, D3D12_RESOURCE_STATES state, const D3D12_CLEAR_VALUE *clear, REFIID riid, void **out) {
  D3D12_HEAP_PROPERTIES props;
  Resource *r = NULL;
  if (!desc) return E_INVALIDARG;
  memset(&props, 0, sizeof props);
  props.Type = D3D12_HEAP_TYPE_DEFAULT;
  props.CreationNodeMask = props.VisibleNodeMask = 1;
  const HRESULT hr = resource_make(This, &props, D3D12_HEAP_FLAG_NONE, desc, NULL, 0, 2, &r);
  if (FAILED(hr)) return hr;
  return query_out((IUnknown *)r, riid, out);
}

/* The tiles across a resource (a buffer is one row). */
static UINT tiles_across(const Resource *r) { return r->desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER ? (UINT)((r->desc.Width + KTILE_BYTES - 1) / KTILE_BYTES) : (UINT)(r->desc.Width / TILE_TEXELS); }
static UINT tiles_down(const Resource *r) { return r->desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER ? 1 : r->desc.Height / TILE_TEXELS; }

static void STDMETHODCALLTYPE dev_GetResourceTiling(Dev *This, ID3D12Resource *resource, UINT *total, D3D12_PACKED_MIP_INFO *packed, D3D12_TILE_SHAPE *shape, UINT *count, UINT first,
                                                    D3D12_SUBRESOURCE_TILING *tilings) {
  const Resource *r = (const Resource *)resource;
  const int reserved = r && r->reserved;
  if (total) *total = reserved ? tiles_across(r) * tiles_down(r) : 0;
  if (packed) memset(packed, 0, sizeof *packed);
  if (shape) {
    memset(shape, 0, sizeof *shape);
    if (reserved && r->desc.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER) { shape->WidthInTexels = TILE_TEXELS; shape->HeightInTexels = TILE_TEXELS; shape->DepthInTexels = 1; }
  }
  if (count) {
    const UINT room = *count;
    *count = reserved && r->desc.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER ? 1 : 0;
    if (tilings && room && *count && first == 0) {
      tilings[0].WidthInTiles = tiles_across(r);
      tilings[0].HeightInTiles = (UINT16)tiles_down(r);
      tilings[0].DepthInTiles = 1;
      tilings[0].StartTileIndexInOverallResource = 0;
    }
  }
}

static void tile_flush(Dev *dev, Resource *r, UINT64 first, HeapObj *heap, UINT64 heap_tile, UINT64 count) {
  UINT request[7] = {ESC_UPDATE_TILE_MAPPINGS, r->alloc, (UINT)first, heap ? heap->alloc : 0, (UINT)(heap_tile * KTILE_BYTES), (UINT)((heap_tile * KTILE_BYTES) >> 32), (UINT)count};
  dev_escape(dev, request, 7, NULL, 0);
}

static void STDMETHODCALLTYPE queue_UpdateTileMappings(void *This, ID3D12Resource *resource, UINT regions, const D3D12_TILED_RESOURCE_COORDINATE *starts, const D3D12_TILE_REGION_SIZE *sizes,
                                                       ID3D12Heap *heap, UINT ranges, const D3D12_TILE_RANGE_FLAGS *flags, const UINT *heap_starts, const UINT *counts, D3D12_TILE_MAPPING_FLAGS mapping) {
  Resource *r = (Resource *)resource;
  HeapObj *h = (HeapObj *)heap;
  if (!r || !r->reserved) return;
  const UINT across = tiles_across(r), down = tiles_down(r), total_tiles = across * down;
  /* the tiles of the regions, in order */
  UINT *tiles = mem_alloc((SIZE_T)65536 * 4);
  UINT n = 0;
  if (!tiles) return;
  for (UINT i = 0; i < (regions ? regions : 1) && n < 65536; ++i) {
    const D3D12_TILED_RESOURCE_COORDINATE c = starts ? starts[i] : (D3D12_TILED_RESOURCE_COORDINATE){0, 0, 0, 0};
    const D3D12_TILE_REGION_SIZE sz = sizes ? sizes[i] : (D3D12_TILE_REGION_SIZE){1, FALSE, 0, 0, 0};
    if (sz.UseBox) {
      for (UINT y = 0; y < sz.Height; ++y)
        for (UINT x = 0; x < sz.Width && n < 65536; ++x) tiles[n++] = (c.Y + y) * across + c.X + x;
    } else {
      for (UINT k = 0; k < sz.NumTiles && n < 65536; ++k) tiles[n++] = c.Y * across + c.X + k;
    }
  }
  UINT pos = 0;
  UINT64 run_first = 0, run_heap = 0, run_len = 0;
  int run_unmap = 0;
  for (UINT j = 0; j < (ranges ? ranges : 1) && pos < n; ++j) {
    const UINT f = flags ? (UINT)flags[j] : 0;
    const UINT c = counts ? counts[j] : (ranges ? 1 : n);
    const UINT start = heap_starts ? heap_starts[j] : 0;
    for (UINT k = 0; k < c && pos < n; ++k, ++pos) {
      const UINT tile = tiles[pos];
      if (tile >= total_tiles || f == D3D12_TILE_RANGE_FLAG_SKIP) {
        if (run_len) { tile_flush(r->d.dev, r, run_first, run_unmap ? NULL : h, run_heap, run_len); run_len = 0; }
        continue;
      }
      const int unmap = f == D3D12_TILE_RANGE_FLAG_NULL || !h;
      const UINT64 heap_tile = f == D3D12_TILE_RANGE_FLAG_REUSE_SINGLE_TILE ? start : (UINT64)start + k;
      const int joins = run_len && run_unmap == unmap && tile == run_first + run_len && (unmap || (f != D3D12_TILE_RANGE_FLAG_REUSE_SINGLE_TILE && heap_tile == run_heap + run_len));
      if (joins) { run_len++; continue; }
      if (run_len) tile_flush(r->d.dev, r, run_first, run_unmap ? NULL : h, run_heap, run_len);
      run_first = tile; run_heap = unmap ? 0 : heap_tile; run_len = 1; run_unmap = unmap;
    }
  }
  if (run_len) tile_flush(r->d.dev, r, run_first, run_unmap ? NULL : h, run_heap, run_len);
  mem_free(tiles);
}
