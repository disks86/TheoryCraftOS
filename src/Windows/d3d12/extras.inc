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
 * Tiled resources are the kernel's reserved allocations: buffers, and textures of 4-byte colour pixels with a size in whole tiles of 128 x 128 (the levels below a tile in either direction
 * are the mip tail, one tile). UpdateTileMappings maps their 64 KiB tiles to places in heaps at once (the queue is synchronous, so that is in order with the lists executed before) and
 * keeps the mapping for CopyTileMappings; CopyTiles moves the bytes of tiles to and from a buffer with CopyBuffer / CopyBufferImage.
 *
 * Predication (SetPredication) cuts the list into chunks that carry the predicate: when a chunk is run its 64-bit value is read and the chunk is skipped or not.
 *
 * Stream output (SOSetTargets): a target is a buffer made with the kernel's stream output flag; it is bound before the draws and starts at the view's offset. A draw with no render target
 * gets a 1 x 1 one of the device's, since the kernel's passes need one. After the list ran, BufferFilledSizeLocation gets the bytes written since the bind (kQueryStreamOutput). */

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

static int res_write(Resource *r, UINT64 offset, const void *src, UINT64 n) {
  if (!r || offset > r->desc.Width || n > r->desc.Width - offset) return 0;
  if (r->shadow) { memcpy(r->shadow + offset, src, (SIZE_T)n); r->dirty = 1; return 1; }
  void *data = NULL;
  if (!KMT_OK(kmt_lock(r->d.dev->kdev, r->alloc, 0, 0, &data))) return 0;
  memcpy((BYTE *)data + offset, src, (SIZE_T)n);
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
    if (t == D3D12_INDIRECT_ARGUMENT_TYPE_VERTEX_BUFFER_VIEW && desc->pArgumentDescs[i].VertexBuffer.Slot >= This->caps.max_vertex_buffers) return DXGI_ERROR_UNSUPPORTED;
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
  c->pred = l->pred;
  c->pred_off = l->pred_off;
  c->pred_op = l->pred_op;
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
  t->pred = NULL;      /* (the call as a whole is the predicated thing) */
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
          list_IASetVertexBuffers(t, a->VertexBuffer.Slot, 1, &v);
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
        D3D12_VERTEX_BUFFER_VIEW v[8];
        const UINT n = e->ndata / 4 < 8 ? e->ndata / 4 : 8;
        for (UINT k = 0; k < n; ++k) {
          v[k].BufferLocation = e->data[4 * k] | ((UINT64)e->data[4 * k + 1] << 32);
          v[k].SizeInBytes = e->data[4 * k + 2];
          v[k].StrideInBytes = e->data[4 * k + 3];
        }
        if (n) list_IASetVertexBuffers(l, (UINT)a[0], n, v);
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

static void STDMETHODCALLTYPE dev_GetResourceTiling(Dev *This, ID3D12Resource *resource, UINT *total, D3D12_PACKED_MIP_INFO *packed, D3D12_TILE_SHAPE *shape, UINT *count, UINT first,
                                                    D3D12_SUBRESOURCE_TILING *tilings) {
  const Resource *r = (const Resource *)resource;
  const int reserved = r && r->reserved;
  TileLayout t;
  memset(&t, 0, sizeof t);
  if (reserved) tile_layout(&r->desc, &t);
  const int buffer = !reserved || r->desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER;
  if (total) *total = t.total;
  if (packed) {
    memset(packed, 0, sizeof *packed);
    if (!buffer) {
      packed->NumStandardMips = (UINT8)t.standard;
      packed->NumPackedMips = (UINT8)(t.mips - t.standard);
      packed->NumTilesForPackedMips = t.standard < t.mips ? 1 : 0;
      packed->StartTileIndexInOverallResource = t.tail_first;
    }
  }
  if (shape) {
    memset(shape, 0, sizeof *shape);
    if (!buffer) { shape->WidthInTexels = TILE_TEXELS; shape->HeightInTexels = TILE_TEXELS; shape->DepthInTexels = 1; }
  }
  if (count) {
    const UINT room = *count;
    UINT n = buffer || first >= t.mips ? 0 : t.mips - first;
    if (n > room) n = room;
    if (tilings)
      for (UINT i = 0; i < n; ++i) {
        const UINT sub = first + i;
        memset(&tilings[i], 0, sizeof tilings[i]);
        if (sub < t.standard) {
          tilings[i].WidthInTiles = t.across[sub];
          tilings[i].HeightInTiles = (UINT16)t.down[sub];
          tilings[i].DepthInTiles = 1;
          tilings[i].StartTileIndexInOverallResource = t.first[sub];
        } else {
          tilings[i].StartTileIndexInOverallResource = 0xFFFFFFFFu;      /* (packed: no tiles of its own) */
        }
      }
    *count = n;
  }
}

/* Maps `count` tiles of the resource from `first` to the heap from heap tile `heap_tile` on (a NULL heap: nowhere), in the kernel and in the copy kept here. */
static void tile_flush(Dev *dev, Resource *r, UINT64 first, HeapObj *heap, UINT64 heap_tile, UINT64 count) {
  UINT request[7] = {ESC_UPDATE_TILE_MAPPINGS, r->alloc, (UINT)first, heap ? heap->alloc : 0, (UINT)(heap_tile * KTILE_BYTES), (UINT)((heap_tile * KTILE_BYTES) >> 32), (UINT)count};
  if (!KMT_OK(dev_escape(dev, request, 7, NULL, 0))) return;
  for (UINT64 k = 0; k < count && first + k < r->tile_count; ++k) {
    r->tile_map[first + k].heap = heap;
    r->tile_map[first + k].tile = heap_tile + k;
  }
}

#define MAX_REGION_TILES 65536

static void STDMETHODCALLTYPE queue_UpdateTileMappings(void *This, ID3D12Resource *resource, UINT regions, const D3D12_TILED_RESOURCE_COORDINATE *starts, const D3D12_TILE_REGION_SIZE *sizes,
                                                       ID3D12Heap *heap, UINT ranges, const D3D12_TILE_RANGE_FLAGS *flags, const UINT *heap_starts, const UINT *counts, D3D12_TILE_MAPPING_FLAGS mapping) {
  Resource *r = (Resource *)resource;
  HeapObj *h = (HeapObj *)heap;
  if (!r || !r->reserved) return;
  TileLayout layout;
  tile_layout(&r->desc, &layout);
  UINT *tiles = mem_alloc((SIZE_T)MAX_REGION_TILES * 4);
  UINT n = 0;
  if (!tiles) return;
  for (UINT i = 0; i < (regions ? regions : 1) && n < MAX_REGION_TILES; ++i) {
    const D3D12_TILED_RESOURCE_COORDINATE c = starts ? starts[i] : (D3D12_TILED_RESOURCE_COORDINATE){0, 0, 0, 0};
    const D3D12_TILE_REGION_SIZE sz = sizes ? sizes[i] : (D3D12_TILE_REGION_SIZE){1, FALSE, 0, 0, 0};
    n = region_tiles(&layout, c, sz, tiles, n, MAX_REGION_TILES);
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
      if (tile >= layout.total || f == D3D12_TILE_RANGE_FLAG_SKIP) {
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

static void STDMETHODCALLTYPE queue_CopyTileMappings(void *This, ID3D12Resource *dst, const D3D12_TILED_RESOURCE_COORDINATE *dst_start, ID3D12Resource *src,
                                                     const D3D12_TILED_RESOURCE_COORDINATE *src_start, const D3D12_TILE_REGION_SIZE *size, D3D12_TILE_MAPPING_FLAGS flags) {
  Resource *d = (Resource *)dst, *s = (Resource *)src;
  if (!d || !s || !d->reserved || !s->reserved || !dst_start || !src_start || !size) return;
  TileLayout dl, sl;
  tile_layout(&d->desc, &dl);
  tile_layout(&s->desc, &sl);
  UINT *a = mem_alloc((SIZE_T)MAX_REGION_TILES * 8);
  if (!a) return;
  UINT *b = a + MAX_REGION_TILES;
  const UINT na = region_tiles(&dl, *dst_start, *size, a, 0, MAX_REGION_TILES), nb = region_tiles(&sl, *src_start, *size, b, 0, MAX_REGION_TILES);
  /* (the source's mapping is read first: the regions may overlap) */
  TileBind *copy = mem_alloc((SIZE_T)(na < nb ? na : nb) * sizeof(TileBind) + 8);
  const UINT n = na < nb ? na : nb;
  if (copy) {
    for (UINT k = 0; k < n; ++k) copy[k] = b[k] < s->tile_count ? s->tile_map[b[k]] : (TileBind){0, 0};
    for (UINT k = 0; k < n; ++k)
      if (a[k] < d->tile_count) tile_flush(d->d.dev, d, a[k], copy[k].heap, copy[k].tile, 1);
    mem_free(copy);
  }
  mem_free(a);
}

static void emit_buffer_image(CmdList *l, UINT direction, const Resource *image, UINT level, UINT x, UINT y, UINT w, UINT h, const Resource *buffer, UINT64 offset, UINT pitch) {
  UINT payload[11] = {direction, alloc_index(l, image->alloc), level, 0, x, y, w, h, alloc_index(l, buffer->alloc), (UINT)offset, pitch};
  packet(l, OP_COPY_BUFFER_IMAGE, payload, 11);
}

static void STDMETHODCALLTYPE list_CopyTiles(CmdList *l, ID3D12Resource *resource, const D3D12_TILED_RESOURCE_COORDINATE *start, const D3D12_TILE_REGION_SIZE *size, ID3D12Resource *buffer,
                                             UINT64 buffer_offset, D3D12_TILE_COPY_FLAGS flags) {
  Resource *r = (Resource *)resource, *b = (Resource *)buffer;
  if (l->bundle || !r || !b || !r->reserved || !start || !size) return;
  const int to_resource = (flags & D3D12_TILE_COPY_FLAG_LINEAR_BUFFER_TO_SWIZZLED_TILED_RESOURCE) != 0;
  if (!to_resource && !(flags & D3D12_TILE_COPY_FLAG_SWIZZLED_TILED_RESOURCE_TO_LINEAR_BUFFER)) return;
  TileLayout layout;
  tile_layout(&r->desc, &layout);
  UINT *tiles = mem_alloc((SIZE_T)MAX_REGION_TILES * 4);
  if (!tiles) return;
  const UINT n = region_tiles(&layout, *start, *size, tiles, 0, MAX_REGION_TILES);
  end_pass(l);
  for (UINT k = 0; k < n; ++k) {
    const UINT tile = tiles[k];
    const UINT64 at = buffer_offset + (UINT64)k * KTILE_BYTES;
    if (tile >= layout.total || at + KTILE_BYTES > b->desc.Width || at > 0xFFFFFFFFu) continue;
    ensure_room(l, 2048, 4);
    if (r->desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {
      UINT w[5];
      if (to_resource) { w[0] = alloc_index(l, r->alloc); w[1] = tile * KTILE_BYTES; w[2] = alloc_index(l, b->alloc); w[3] = (UINT)at; }
      else { w[0] = alloc_index(l, b->alloc); w[1] = (UINT)at; w[2] = alloc_index(l, r->alloc); w[3] = tile * KTILE_BYTES; }
      w[4] = KTILE_BYTES;
      packet(l, OP_COPY_BUFFER, w, 5);
      continue;
    }
    UINT level = 0;
    while (level < layout.standard && tile >= layout.first[level] + layout.across[level] * layout.down[level]) ++level;
    if (level < layout.standard) {
      const UINT local = tile - layout.first[level], tx = local % layout.across[level], ty = local / layout.across[level];
      const UINT lw = mip_extent((UINT)r->desc.Width, level), lh = mip_extent(r->desc.Height, level);
      const UINT x = tx * TILE_TEXELS, y = ty * TILE_TEXELS;
      emit_buffer_image(l, to_resource ? 0 : 1, r, level, x, y, lw - x < TILE_TEXELS ? lw - x : TILE_TEXELS, lh - y < TILE_TEXELS ? lh - y : TILE_TEXELS, b, at, TILE_TEXELS * 4);
    } else {
      /* the mip tail: the packed levels one after the other, as in the backing */
      UINT64 offset = at;
      for (UINT lv = layout.standard; lv < layout.mips; ++lv) {
        const UINT lw = mip_extent((UINT)r->desc.Width, lv), lh = mip_extent(r->desc.Height, lv);
        emit_buffer_image(l, to_resource ? 0 : 1, r, lv, 0, 0, lw, lh, b, offset, lw * 4);
        offset += (UINT64)lw * lh * 4;
      }
    }
  }
  mem_free(tiles);
}

/* ---- predication ------------------------------------------------------------------------------------------------------------------------------------------------------------ */

/* D3D12_PREDICATION_OP_EQUAL_ZERO skips the commands when the 64-bit value is zero, NOT_EQUAL_ZERO when it is not. */
static int chunk_skipped(const Chunk *c) {
  UINT64 v = 1;
  if (!c->pred) return 0;
  if (!res_read(c->pred, c->pred_off, 8, &v)) return 0;
  return c->pred_op == D3D12_PREDICATION_OP_EQUAL_ZERO ? v == 0 : v != 0;
}

static void STDMETHODCALLTYPE list_SetPredication(CmdList *l, ID3D12Resource *buffer, UINT64 offset, D3D12_PREDICATION_OP op) {
  if (l->bundle) return;
  end_pass(l);
  l->pred = (Resource *)buffer;
  l->pred_off = offset;
  l->pred_op = (int)op;
  chunk_new(l);      /* (the chunk is the unit that is skipped) */
}

/* ---- stream output ------------------------------------------------------------------------------------------------------------------------------------------------------- */

static void STDMETHODCALLTYPE list_SOSetTargets(CmdList *l, UINT start, UINT count, const D3D12_STREAM_OUTPUT_BUFFER_VIEW *views) {
  if (l->bundle) return;
  for (UINT i = 0; i < count && start + i < 4; ++i) {
    SoTarget *t = &l->so[start + i];
    memset(t, 0, sizeof *t);
    if (!views || !views[i].BufferLocation) continue;
    UINT64 offset = 0;
    Resource *r = va_find(views[i].BufferLocation, &offset);
    if (!r) continue;
    t->alloc = r->alloc;
    t->offset = offset;
    UINT64 fill_offset = 0;
    t->fill = views[i].BufferFilledSizeLocation ? va_find(views[i].BufferFilledSizeLocation, &fill_offset) : NULL;
    t->fill_offset = fill_offset;
    if (t->fill && l->nfills < 16) l->fills[l->nfills++] = *t;
  }
  l->so_dirty = 1;
}

/* The bytes of each target the list wrote, into the places the program named (after the list ran). */
static void list_apply_fills(CmdList *l) {
  for (UINT i = 0; i < l->nfills; ++i) {
    const SoTarget *t = &l->fills[i];
    UINT request[2] = {ESC_QUERY_STREAM_OUTPUT, t->alloc}, reply[2] = {0, 0};
    if (!KMT_OK(dev_escape(l->d.dev, request, 2, reply, 2))) continue;
    UINT64 filled = reply[0] | ((UINT64)reply[1] << 32);
    filled = filled > t->offset ? filled - t->offset : 0;
    res_write(t->fill, t->fill_offset, &filled, 8);
  }
}
