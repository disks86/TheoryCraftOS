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

/* d3d12.dll: command allocators and command lists (ID3D12CommandAllocator, ID3D12GraphicsCommandList). Included by d3d12.c.
 *
 * A command list records the stream of the kernel's command format (src/Kernel/Dxgkrnl/CommandStream.hpp) as it is called; ExecuteCommandLists sends it with D3DKMTRender.
 * Where Direct3D 12 and the stream differ:
 *   - The stream names allocations by their number in an allocation list that goes with it, one list of at most 1024 entries for at most 64 KiB of commands, so a long list is
 *     recorded as several chunks, each a stream of its own, cut where no render pass is open (and all state is sent again at the start of the next chunk).
 *   - The stream has render passes (BeginPass names the targets and may clear them) where Direct3D 12 sets targets and clears and draws as it likes: a pass is begun at the first
 *     draw after the targets were set and ended before anything that is not a draw. ClearRenderTargetView is a pass of its own that clears; ClearDepthStencilView waits for the
 *     next pass that uses the view (a pass needs a colour target, so depth cannot be cleared alone).
 *   - State does not carry over from stream to stream: the pipeline, buffers, viewport, root signature and its arguments are sent again before the first draw of a chunk, and again
 *     whenever they changed. The graphics and compute root signatures are separate in Direct3D 12 and one in the stream: the one that is not in use is sent again when used.
 *   - The vertex buffer is one (slot 0); the stride is part of the kernel's pipeline, so the pipeline is chosen at the draw (pso_pipeline).
 *   - Resource barriers are not forwarded (the allocations are not state tracked) except for aliasing; queries, predication, indirect execution, stream output and bundles are not
 *     implemented (their calls do nothing, ExecuteBundle and ExecuteIndirect report nothing: the debug layer is not here to complain). */

#define CHUNK_BYTES 65000
#define CHUNK_ALLOCS 1000
#define ROOM_BYTES 4096
#define ROOM_ALLOCS 24

typedef struct Chunk {
  BYTE *bytes;
  UINT size;
  UINT allocs[CHUNK_ALLOCS];
  UINT nalloc;
} Chunk;

typedef struct RootArg {
  UINT kind;           /* 0 not set, 1 constants, 2 constant buffer, 3 shader resource, 4 unordered access, 5 table */
  UINT count;          /* constants: the number of values set */
  UINT values[64];
  UINT alloc;          /* a root descriptor's buffer */
  UINT64 offset;
  UINT64 gpu;          /* a table's first descriptor (GPU handle) */
} RootArg;

typedef struct Binding {
  RootSig *rs;
  RootArg args[16];
} Binding;

typedef struct CmdAlloc {
  DObj d;
  D3D12_COMMAND_LIST_TYPE type;
} CmdAlloc;

typedef struct Dirty { unsigned all : 1; } Dirty;

typedef struct CmdList {
  DObj d;
  D3D12_COMMAND_LIST_TYPE type;
  int closed;
  Chunk **chunks;
  UINT nchunks, cap_chunks;
  IUnknown **refs;
  UINT nrefs, cap_refs;
  /* the state of the recording */
  Pso *pso;
  Binding gfx, cs;
  DescHeap *heaps[2];
  DescHeap *heaps_used[8];       /* every shader-visible heap the list set: their kernel copies are brought up to date when it is executed */
  UINT nheaps_used;
  D3D12_PRIMITIVE_TOPOLOGY topology;
  UINT vb_alloc, vb_stride;
  UINT64 vb_offset, vb_size;
  UINT ib_alloc, ib_format;
  UINT64 ib_offset;
  D3D12_VIEWPORT viewport;
  D3D12_RECT scissor;
  int have_viewport, have_scissor;
  UINT stencil_ref;
  UINT rtv[4];                 /* the kernel's allocations of the colour targets */
  UINT rtv_count;
  UINT dsv;
  int pass_open;
  int pending_clear;           /* a depth/stencil clear waiting for a pass */
  UINT pending_alloc, pending_flags;
  float pending_depth;
  UINT pending_stencil;
  /* what the chunk being recorded has already sent */
  int sent_valid;
  UINT sent_pipeline;
  UINT sent_topology;
  UINT sent_vb_alloc, sent_ib_alloc;
  UINT64 sent_vb_offset, sent_ib_offset;
  UINT sent_ib_format;
  int sent_viewport, sent_scissor;
  int sent_binding;            /* 0 none, 1 graphics, 2 compute */
  int dirty_gfx, dirty_cs, dirty_heaps;
} CmdList;

static const GUID *const g_alloc_iids[] = {&IID_ID3D12Object, &IID_ID3D12DeviceChild, &IID_ID3D12Pageable, &IID_ID3D12CommandAllocator, NULL};
static const GUID *const g_list_iids[] = {&IID_ID3D12Object, &IID_ID3D12DeviceChild, &IID_ID3D12CommandList, &IID_ID3D12GraphicsCommandList, NULL};

/* ---- allocators --------------------------------------------------------------------------------------------------------------------------------------------------------------- */

static HRESULT STDMETHODCALLTYPE dev_CreateCommandAllocator(Dev *This, D3D12_COMMAND_LIST_TYPE type, REFIID riid, void **out) {
  extern const ID3D12CommandAllocatorVtbl alloc_vtbl;
  if (type != D3D12_COMMAND_LIST_TYPE_DIRECT && type != D3D12_COMMAND_LIST_TYPE_COMPUTE && type != D3D12_COMMAND_LIST_TYPE_COPY && type != D3D12_COMMAND_LIST_TYPE_BUNDLE) return E_INVALIDARG;
  CmdAlloc *a = dobj_new(sizeof *a, This, &alloc_vtbl, g_alloc_iids, dobj_child_destroy);
  if (!a) return E_OUTOFMEMORY;
  a->type = type;
  return query_out((IUnknown *)a, riid, out);
}
static HRESULT STDMETHODCALLTYPE alloc_Reset(CmdAlloc *This) { return S_OK; }

/* ---- the stream ------------------------------------------------------------------------------------------------------------------------------------------------------------- */

static Chunk *chunk_cur(CmdList *l) { return l->nchunks ? l->chunks[l->nchunks - 1] : NULL; }

static int chunk_new(CmdList *l) {
  if (l->nchunks == l->cap_chunks) {
    const UINT cap = l->cap_chunks ? l->cap_chunks * 2 : 4;
    Chunk **n = mem_grow(l->chunks, cap * sizeof *n);
    if (!n) return 0;
    l->chunks = n;
    l->cap_chunks = cap;
  }
  Chunk *c = mem_alloc(sizeof *c);
  if (!c) return 0;
  c->bytes = mem_alloc(CHUNK_BYTES + 256);
  if (!c->bytes) { mem_free(c); return 0; }
  l->chunks[l->nchunks++] = c;
  l->sent_valid = 0;
  l->sent_binding = 0;
  l->sent_pipeline = 0;
  l->sent_topology = 0xFFFFFFFFu;
  l->sent_vb_alloc = l->sent_ib_alloc = 0;
  l->sent_viewport = l->sent_scissor = 0;
  l->dirty_heaps = 1;
  return 1;
}

static void w32(Chunk *c, UINT v) { memcpy(c->bytes + c->size, &v, 4); c->size += 4; }

static void packet(CmdList *l, UINT op, const UINT *words, UINT n) {
  Chunk *c = chunk_cur(l);
  if (!c) return;
  w32(c, op | ((n + 1) << 16));
  for (UINT i = 0; i < n; ++i) w32(c, words[i]);
}

/* The index of a kernel allocation in the chunk's list. */
static UINT alloc_index(CmdList *l, UINT handle) {
  Chunk *c = chunk_cur(l);
  for (UINT i = 0; i < c->nalloc; ++i)
    if (c->allocs[i] == handle) return i;
  c->allocs[c->nalloc] = handle;
  return c->nalloc++;
}

/* Starts a new chunk when the current one cannot take a sequence of commands needing `bytes` and `allocs`. Only between passes. */
static void ensure_room(CmdList *l, UINT bytes, UINT allocs) {
  Chunk *c = chunk_cur(l);
  if (c && c->size + bytes <= CHUNK_BYTES && c->nalloc + allocs <= CHUNK_ALLOCS) return;
  if (l->pass_open) { packet(l, 2, NULL, 0); l->pass_open = 0; }
  chunk_new(l);
}

enum { OP_BEGIN_PASS = 1, OP_END_PASS, OP_BIND_PIPELINE, OP_BIND_VB, OP_BIND_IB, OP_PUSH_CONSTANTS, OP_BIND_TEXTURE, OP_SET_VIEWPORT, OP_SET_SCISSOR, OP_DRAW, OP_DRAW_INDEXED, OP_COPY_REGION,
       OP_GENERATE_MIPMAPS, OP_SET_TOPOLOGY, OP_DRAW_INSTANCED, OP_DRAW_INDEXED_INSTANCED, OP_BEGIN_QUERY, OP_END_QUERY, OP_BIND_UAV, OP_DISPATCH, OP_RESOLVE_REGION, OP_SET_STREAM_OUTPUT,
       OP_SET_ROOT_SIGNATURE, OP_SET_DESCRIPTOR_HEAPS, OP_SET_ROOT_CONSTANTS, OP_SET_ROOT_DESCRIPTOR, OP_SET_ROOT_TABLE, OP_RESOURCE_BARRIER, OP_COPY_BUFFER, OP_COPY_BUFFER_IMAGE };

static void end_pass(CmdList *l) {
  if (!l->pass_open) return;
  packet(l, OP_END_PASS, NULL, 0);
  l->pass_open = 0;
}

static void keep(CmdList *l, IUnknown *o) {
  if (!o) return;
  for (UINT i = 0; i < l->nrefs; ++i)
    if (l->refs[i] == o) return;
  if (l->nrefs == l->cap_refs) {
    const UINT cap = l->cap_refs ? l->cap_refs * 2 : 16;
    IUnknown **n = mem_grow(l->refs, cap * sizeof *n);
    if (!n) return;
    l->refs = n;
    l->cap_refs = cap;
  }
  o->lpVtbl->AddRef(o);
  l->refs[l->nrefs++] = o;
}

static void list_free_recording(CmdList *l) {
  for (UINT i = 0; i < l->nchunks; ++i) { mem_free(l->chunks[i]->bytes); mem_free(l->chunks[i]); }
  l->nchunks = 0;
  for (UINT i = 0; i < l->nrefs; ++i) l->refs[i]->lpVtbl->Release(l->refs[i]);
  l->nrefs = 0;
}

static void list_reset_state(CmdList *l) {
  memset(&l->pso, 0, sizeof *l - offsetof(CmdList, pso));
  l->topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
  l->dirty_gfx = l->dirty_cs = l->dirty_heaps = 1;
}

static void list_destroy(DxObj *o) {
  CmdList *l = (CmdList *)o;
  list_free_recording(l);
  mem_free(l->chunks);
  mem_free(l->refs);
  dobj_child_destroy(o);
}

static HRESULT STDMETHODCALLTYPE dev_CreateCommandList(Dev *This, UINT mask, D3D12_COMMAND_LIST_TYPE type, ID3D12CommandAllocator *allocator, ID3D12PipelineState *initial, REFIID riid, void **out) {
  extern const ID3D12GraphicsCommandListVtbl list_vtbl;
  if (!allocator || (type != D3D12_COMMAND_LIST_TYPE_DIRECT && type != D3D12_COMMAND_LIST_TYPE_COMPUTE && type != D3D12_COMMAND_LIST_TYPE_COPY)) return E_INVALIDARG;
  CmdList *l = dobj_new(sizeof *l, This, &list_vtbl, g_list_iids, list_destroy);
  if (!l) return E_OUTOFMEMORY;
  l->type = type;
  list_reset_state(l);
  if (!chunk_new(l)) { dx_Release(OBJ(l)); return E_OUTOFMEMORY; }
  if (initial) { l->pso = (Pso *)initial; keep(l, (IUnknown *)initial); }
  return query_out((IUnknown *)l, riid, out);
}

static D3D12_COMMAND_LIST_TYPE STDMETHODCALLTYPE list_GetType(CmdList *This) { return This->type; }
static HRESULT STDMETHODCALLTYPE list_Close(CmdList *This) {
  if (This->closed) return E_FAIL;
  end_pass(This);
  This->closed = 1;
  return S_OK;
}
static HRESULT STDMETHODCALLTYPE list_Reset(CmdList *This, ID3D12CommandAllocator *allocator, ID3D12PipelineState *initial) {
  if (!This->closed || !allocator) return E_FAIL;
  list_free_recording(This);
  list_reset_state(This);
  This->closed = 0;
  if (!chunk_new(This)) return E_OUTOFMEMORY;
  if (initial) { This->pso = (Pso *)initial; keep(This, (IUnknown *)initial); }
  return S_OK;
}
static void STDMETHODCALLTYPE list_ClearState(CmdList *This, ID3D12PipelineState *initial) {
  end_pass(This);
  list_reset_state(This);
  if (initial) { This->pso = (Pso *)initial; keep(This, (IUnknown *)initial); }
}

/* ---- sending the state ---------------------------------------------------------------------------------------------------------------------------------------------------- */

/* The RTV/DSV a handle names. */
static SoftDesc *view_at(D3D12_CPU_DESCRIPTOR_HANDLE h) { return desc_at(h); }

static void begin_pass_packet(CmdList *l, UINT color_alloc, UINT depth_alloc, UINT flags, const float color[4], float depth, UINT stencil) {
  UINT w[9 + 3];
  UINT n = 0;
  w[n++] = alloc_index(l, color_alloc);
  w[n++] = depth_alloc ? alloc_index(l, depth_alloc) : 0xFFFFFFFFu;
  w[n++] = flags;
  w[n++] = bits_of(color[0]);
  w[n++] = bits_of(color[1]);
  w[n++] = bits_of(color[2]);
  w[n++] = bits_of(color[3]);
  w[n++] = bits_of(depth);
  w[n++] = stencil;
  /* (the other colour targets, when the pass has them; clears of those are separate passes) */
  for (UINT i = 1; i < l->rtv_count && i < 4 && flags != 0xFFFFFFFFu; ++i) w[n++] = alloc_index(l, l->rtv[i]);
  packet(l, OP_BEGIN_PASS, w, n);
}

/* Begins the pass of the current targets if there is none. Returns 0 when there is nothing to draw on. */
static int ensure_pass(CmdList *l) {
  static const float zero[4] = {0, 0, 0, 0};
  if (l->pass_open) return 1;
  if (l->rtv_count == 0 || !l->rtv[0]) return 0;
  UINT flags = 0;
  float depth = 1.0f;
  UINT stencil = 0;
  if (l->dsv && l->pending_clear && l->pending_alloc == l->dsv) {
    flags = l->pending_flags;
    depth = l->pending_depth;
    stencil = l->pending_stencil;
    l->pending_clear = 0;
  }
  begin_pass_packet(l, l->rtv[0], l->dsv, flags, zero, depth, stencil);
  l->pass_open = 1;
  /* a new pass forgets what was bound (a device may have its own idea of what carries over) */
  l->sent_pipeline = 0;
  l->sent_topology = 0xFFFFFFFFu;
  l->sent_vb_alloc = l->sent_ib_alloc = 0;
  l->sent_viewport = l->sent_scissor = 0;
  l->sent_binding = 0;
  return 1;
}

static DescHeap *heap_for_gpu(CmdList *l, UINT64 gpu, UINT *index) {
  for (int i = 0; i < 2; ++i) {
    DescHeap *h = l->heaps[i];
    if (!h) continue;
    const UINT64 span = (UINT64)h->desc.NumDescriptors * DESCRIPTOR_STRIDE;
    if (gpu >= h->gpu_base && gpu - h->gpu_base < span) {
      *index = (UINT)((gpu - h->gpu_base) / DESCRIPTOR_STRIDE);
      return h;
    }
  }
  return NULL;
}

/* Sends the root signature and its arguments of a binding (graphics 1, compute 2). */
static void send_binding(CmdList *l, Binding *b, int which) {
  if (!b->rs) return;
  UINT w[8 + 64];
  w[0] = b->rs->khandle;
  packet(l, OP_SET_ROOT_SIGNATURE, w, 1);
  if (l->heaps[0] || l->heaps[1]) {
    UINT n = 1;
    w[0] = 0;
    for (int i = 0; i < 2; ++i)
      if (l->heaps[i] && l->heaps[i]->khandle) w[n++] = l->heaps[i]->khandle;
    w[0] = n - 1;
    if (n > 1) packet(l, OP_SET_DESCRIPTOR_HEAPS, w, n);
  }
  for (UINT i = 0; i < b->rs->parameter_count && i < 16; ++i) {
    RootArg *a = &b->args[i];
    switch (a->kind) {
      case 1: {
        UINT n = a->count;
        if (n > b->rs->pconstants[i]) n = b->rs->pconstants[i];
        if (!n) break;
        w[0] = i; w[1] = 0; w[2] = n;
        memcpy(w + 3, a->values, n * 4);
        packet(l, OP_SET_ROOT_CONSTANTS, w, 3 + n);
        break;
      }
      case 2: case 3: case 4:
        if (!a->alloc) break;
        w[0] = i; w[1] = alloc_index(l, a->alloc); w[2] = (UINT)a->offset;
        packet(l, OP_SET_ROOT_DESCRIPTOR, w, 3);
        break;
      case 5: {
        UINT index = 0;
        DescHeap *h = heap_for_gpu(l, a->gpu, &index);
        if (!h) break;
        w[0] = i; w[1] = index;
        packet(l, OP_SET_ROOT_TABLE, w, 2);
        break;
      }
      default: break;
    }
  }
  l->sent_binding = which;
  if (which == 1) l->dirty_gfx = 0; else l->dirty_cs = 0;
}

static UINT topology_code(D3D12_PRIMITIVE_TOPOLOGY t) {
  switch (t) {
    case D3D_PRIMITIVE_TOPOLOGY_POINTLIST: return 5;
    case D3D_PRIMITIVE_TOPOLOGY_LINELIST: return 3;
    case D3D_PRIMITIVE_TOPOLOGY_LINESTRIP: return 4;
    case D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST: return 0;
    case D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP: return 1;
    case D3D_PRIMITIVE_TOPOLOGY_LINELIST_ADJ: return 0x100;
    case D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST_ADJ: return 0x101;
    default:
      if (t >= D3D_PRIMITIVE_TOPOLOGY_1_CONTROL_POINT_PATCHLIST && t <= D3D_PRIMITIVE_TOPOLOGY_32_CONTROL_POINT_PATCHLIST) return 5 + (UINT)(t - D3D_PRIMITIVE_TOPOLOGY_1_CONTROL_POINT_PATCHLIST + 1);
      return 0;
  }
}

/* Everything a draw needs sent: the pass, the pipeline, the root arguments, the buffers, the viewport. Returns 0 when the draw cannot be made. */
static int prepare_draw(CmdList *l, int indexed) {
  if (!l->pso || l->pso->compute || !l->gfx.rs) return 0;
  ensure_room(l, ROOM_BYTES, ROOM_ALLOCS);
  if (!ensure_pass(l)) return 0;
  if (l->sent_binding != 1 || l->dirty_gfx || l->dirty_heaps) {
    send_binding(l, &l->gfx, 1);
    l->dirty_heaps = 0;
  }
  const UINT pipeline = pso_pipeline(l->pso, l->vb_alloc ? l->vb_stride : (l->pso->attr_count ? 0 : 0), l->stencil_ref);
  if (!pipeline) return 0;
  if (pipeline != l->sent_pipeline) {
    packet(l, OP_BIND_PIPELINE, &pipeline, 1);
    l->sent_pipeline = pipeline;
  }
  const UINT topology = topology_code(l->topology);
  if (topology != l->sent_topology) {
    packet(l, OP_SET_TOPOLOGY, &topology, 1);
    l->sent_topology = topology;
  }
  if (l->vb_alloc && (l->vb_alloc != l->sent_vb_alloc || l->vb_offset != l->sent_vb_offset)) {
    UINT w[2] = {alloc_index(l, l->vb_alloc), (UINT)l->vb_offset};
    packet(l, OP_BIND_VB, w, 2);
    l->sent_vb_alloc = l->vb_alloc;
    l->sent_vb_offset = l->vb_offset;
  }
  if (indexed && l->ib_alloc && (l->ib_alloc != l->sent_ib_alloc || l->ib_offset != l->sent_ib_offset || l->ib_format != l->sent_ib_format)) {
    UINT w[3] = {alloc_index(l, l->ib_alloc), l->ib_format == DXGI_FORMAT_R32_UINT ? 1u : 0u, (UINT)l->ib_offset};
    packet(l, OP_BIND_IB, w, 3);
    l->sent_ib_alloc = l->ib_alloc;
    l->sent_ib_offset = l->ib_offset;
    l->sent_ib_format = l->ib_format;
  }
  if (l->have_viewport && !l->sent_viewport) {
    UINT w[6] = {bits_of(l->viewport.TopLeftX), bits_of(l->viewport.TopLeftY), bits_of(l->viewport.Width), bits_of(l->viewport.Height), bits_of(l->viewport.MinDepth), bits_of(l->viewport.MaxDepth)};
    packet(l, OP_SET_VIEWPORT, w, 6);
    l->sent_viewport = 1;
  }
  if (l->have_scissor && !l->sent_scissor) {
    LONG width = l->scissor.right - l->scissor.left, height = l->scissor.bottom - l->scissor.top;
    UINT w[4] = {(UINT)l->scissor.left, (UINT)l->scissor.top, width > 0 ? (UINT)width : 0, height > 0 ? (UINT)height : 0};
    packet(l, OP_SET_SCISSOR, w, 4);
    l->sent_scissor = 1;
  }
  return 1;
}

/* ---- drawing and dispatching ------------------------------------------------------------------------------------------------------------------------------------------- */

static void STDMETHODCALLTYPE list_DrawInstanced(CmdList *l, UINT vertices, UINT instances, UINT first_vertex, UINT first_instance) {
  if (!vertices || !instances || !prepare_draw(l, 0)) return;
  if (instances == 1 && first_instance == 0) {
    UINT w[2] = {vertices, first_vertex};
    packet(l, OP_DRAW, w, 2);
  } else {
    UINT w[4] = {vertices, first_vertex, instances, first_instance};
    packet(l, OP_DRAW_INSTANCED, w, 4);
  }
}
static void STDMETHODCALLTYPE list_DrawIndexedInstanced(CmdList *l, UINT indices, UINT instances, UINT first_index, INT base_vertex, UINT first_instance) {
  if (!indices || !instances || !l->ib_alloc || !prepare_draw(l, 1)) return;
  if (instances == 1 && first_instance == 0) {
    UINT w[3] = {indices, first_index, (UINT)base_vertex};
    packet(l, OP_DRAW_INDEXED, w, 3);
  } else {
    UINT w[5] = {indices, first_index, (UINT)base_vertex, instances, first_instance};
    packet(l, OP_DRAW_INDEXED_INSTANCED, w, 5);
  }
}
static void STDMETHODCALLTYPE list_Dispatch(CmdList *l, UINT x, UINT y, UINT z) {
  if (!l->pso || !l->pso->compute || !l->cs.rs || !x || !y || !z) return;
  end_pass(l);
  ensure_room(l, ROOM_BYTES, ROOM_ALLOCS);
  if (l->sent_binding != 2 || l->dirty_cs || l->dirty_heaps) {
    send_binding(l, &l->cs, 2);
    l->dirty_heaps = 0;
  }
  UINT pipeline = l->pso->compute_pipe;
  packet(l, OP_BIND_PIPELINE, &pipeline, 1);
  l->sent_pipeline = 0;
  UINT w[3] = {x, y, z};
  packet(l, OP_DISPATCH, w, 3);
}
static void STDMETHODCALLTYPE list_ExecuteIndirect(CmdList *l, ID3D12CommandSignature *sig, UINT max, ID3D12Resource *args, UINT64 args_offset, ID3D12Resource *count, UINT64 count_offset) {}

/* ---- state setting -------------------------------------------------------------------------------------------------------------------------------------------------------------- */

static void STDMETHODCALLTYPE list_SetPipelineState(CmdList *l, ID3D12PipelineState *p) {
  l->pso = (Pso *)p;
  keep(l, (IUnknown *)p);
}
static void STDMETHODCALLTYPE list_IASetPrimitiveTopology(CmdList *l, D3D12_PRIMITIVE_TOPOLOGY t) { l->topology = t; }
static void STDMETHODCALLTYPE list_RSSetViewports(CmdList *l, UINT n, const D3D12_VIEWPORT *v) {
  if (!n || !v) return;
  l->viewport = v[0];
  l->have_viewport = 1;
  l->sent_viewport = 0;
}
static void STDMETHODCALLTYPE list_RSSetScissorRects(CmdList *l, UINT n, const D3D12_RECT *r) {
  if (!n || !r) return;
  l->scissor = r[0];
  l->have_scissor = 1;
  l->sent_scissor = 0;
}
static void STDMETHODCALLTYPE list_OMSetBlendFactor(CmdList *l, const FLOAT factor[4]) {}
static void STDMETHODCALLTYPE list_OMSetStencilRef(CmdList *l, UINT ref) { l->stencil_ref = ref; }
static void STDMETHODCALLTYPE list_IASetIndexBuffer(CmdList *l, const D3D12_INDEX_BUFFER_VIEW *view) {
  if (!view || !view->BufferLocation) { l->ib_alloc = 0; return; }
  UINT64 offset = 0;
  Resource *r = va_find(view->BufferLocation, &offset);
  l->ib_alloc = r ? r->alloc : 0;
  l->ib_offset = offset;
  l->ib_format = view->Format;
}
static void STDMETHODCALLTYPE list_IASetVertexBuffers(CmdList *l, UINT start, UINT count, const D3D12_VERTEX_BUFFER_VIEW *views) {
  if (start != 0 || !count) return;      /* (one vertex buffer: slot 0) */
  if (!views || !views[0].BufferLocation) { l->vb_alloc = 0; return; }
  UINT64 offset = 0;
  Resource *r = va_find(views[0].BufferLocation, &offset);
  l->vb_alloc = r ? r->alloc : 0;
  l->vb_offset = offset;
  l->vb_stride = views[0].StrideInBytes;
  l->vb_size = views[0].SizeInBytes;
}
static void STDMETHODCALLTYPE list_SOSetTargets(CmdList *l, UINT start, UINT count, const D3D12_STREAM_OUTPUT_BUFFER_VIEW *views) {}

static void STDMETHODCALLTYPE list_OMSetRenderTargets(CmdList *l, UINT count, const D3D12_CPU_DESCRIPTOR_HANDLE *rtvs, BOOL single, const D3D12_CPU_DESCRIPTOR_HANDLE *dsv) {
  end_pass(l);
  l->rtv_count = 0;
  for (UINT i = 0; i < count && i < 4 && rtvs; ++i) {
    D3D12_CPU_DESCRIPTOR_HANDLE h = single ? (D3D12_CPU_DESCRIPTOR_HANDLE){rtvs[0].ptr + (SIZE_T)i * DESCRIPTOR_STRIDE} : rtvs[i];
    const SoftDesc *d = view_at(h);
    l->rtv[l->rtv_count++] = d && d->kind == DK_RTV ? d->alloc : 0;
  }
  l->dsv = 0;
  if (dsv && dsv->ptr) {
    const SoftDesc *d = view_at(*dsv);
    if (d && d->kind == DK_DSV) l->dsv = d->alloc;
  }
}

static void STDMETHODCALLTYPE list_SetDescriptorHeaps(CmdList *l, UINT count, ID3D12DescriptorHeap *const *heaps) {
  l->heaps[0] = l->heaps[1] = NULL;
  for (UINT i = 0; i < count; ++i) {
    DescHeap *h = (DescHeap *)heaps[i];
    if (!h) continue;
    l->heaps[h->desc.Type == D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER ? 1 : 0] = h;
    keep(l, (IUnknown *)h);
    int known = 0;
    for (UINT k = 0; k < l->nheaps_used; ++k) known |= l->heaps_used[k] == h;
    if (!known && l->nheaps_used < 8) l->heaps_used[l->nheaps_used++] = h;
  }
  l->dirty_heaps = l->dirty_gfx = l->dirty_cs = 1;
}

/* ---- the root signature ------------------------------------------------------------------------------------------------------------------------------------------------------- */

static void set_signature(CmdList *l, Binding *b, ID3D12RootSignature *rs, int *dirty) {
  b->rs = (RootSig *)rs;
  memset(b->args, 0, sizeof b->args);
  keep(l, (IUnknown *)rs);
  *dirty = 1;
}
static void STDMETHODCALLTYPE list_SetGraphicsRootSignature(CmdList *l, ID3D12RootSignature *rs) { set_signature(l, &l->gfx, rs, &l->dirty_gfx); }
static void STDMETHODCALLTYPE list_SetComputeRootSignature(CmdList *l, ID3D12RootSignature *rs) { set_signature(l, &l->cs, rs, &l->dirty_cs); }

static RootArg *root_arg(Binding *b, UINT index) { return index < 16 ? &b->args[index] : NULL; }

static void set_constants(CmdList *l, Binding *b, int *dirty, UINT index, UINT count, const UINT *data, UINT dst) {
  RootArg *a = root_arg(b, index);
  if (!a || dst > 64 || count > 64 - dst) return;
  a->kind = 1;
  memcpy(a->values + dst, data, (SIZE_T)count * 4);
  if (dst + count > a->count) a->count = dst + count;
  *dirty = 1;
}
static void set_view(Binding *b, int *dirty, UINT index, UINT kind, UINT64 address) {
  RootArg *a = root_arg(b, index);
  if (!a) return;
  UINT64 offset = 0;
  Resource *r = va_find(address, &offset);
  a->kind = kind;
  a->alloc = r ? r->alloc : 0;
  a->offset = offset;
  *dirty = 1;
}
static void set_table(Binding *b, int *dirty, UINT index, UINT64 gpu) {
  RootArg *a = root_arg(b, index);
  if (!a) return;
  a->kind = 5;
  a->gpu = gpu;
  *dirty = 1;
}
static void STDMETHODCALLTYPE list_SetGraphicsRoot32BitConstant(CmdList *l, UINT i, UINT v, UINT dst) { set_constants(l, &l->gfx, &l->dirty_gfx, i, 1, &v, dst); }
static void STDMETHODCALLTYPE list_SetComputeRoot32BitConstant(CmdList *l, UINT i, UINT v, UINT dst) { set_constants(l, &l->cs, &l->dirty_cs, i, 1, &v, dst); }
static void STDMETHODCALLTYPE list_SetGraphicsRoot32BitConstants(CmdList *l, UINT i, UINT n, const void *d, UINT dst) { set_constants(l, &l->gfx, &l->dirty_gfx, i, n, d, dst); }
static void STDMETHODCALLTYPE list_SetComputeRoot32BitConstants(CmdList *l, UINT i, UINT n, const void *d, UINT dst) { set_constants(l, &l->cs, &l->dirty_cs, i, n, d, dst); }
static void STDMETHODCALLTYPE list_SetGraphicsRootConstantBufferView(CmdList *l, UINT i, UINT64 a) { set_view(&l->gfx, &l->dirty_gfx, i, 2, a); }
static void STDMETHODCALLTYPE list_SetComputeRootConstantBufferView(CmdList *l, UINT i, UINT64 a) { set_view(&l->cs, &l->dirty_cs, i, 2, a); }
static void STDMETHODCALLTYPE list_SetGraphicsRootShaderResourceView(CmdList *l, UINT i, UINT64 a) { set_view(&l->gfx, &l->dirty_gfx, i, 3, a); }
static void STDMETHODCALLTYPE list_SetComputeRootShaderResourceView(CmdList *l, UINT i, UINT64 a) { set_view(&l->cs, &l->dirty_cs, i, 3, a); }
static void STDMETHODCALLTYPE list_SetGraphicsRootUnorderedAccessView(CmdList *l, UINT i, UINT64 a) { set_view(&l->gfx, &l->dirty_gfx, i, 4, a); }
static void STDMETHODCALLTYPE list_SetComputeRootUnorderedAccessView(CmdList *l, UINT i, UINT64 a) { set_view(&l->cs, &l->dirty_cs, i, 4, a); }
static void STDMETHODCALLTYPE list_SetGraphicsRootDescriptorTable(CmdList *l, UINT i, D3D12_GPU_DESCRIPTOR_HANDLE h) { set_table(&l->gfx, &l->dirty_gfx, i, h.ptr); }
static void STDMETHODCALLTYPE list_SetComputeRootDescriptorTable(CmdList *l, UINT i, D3D12_GPU_DESCRIPTOR_HANDLE h) { set_table(&l->cs, &l->dirty_cs, i, h.ptr); }

/* ---- clears -------------------------------------------------------------------------------------------------------------------------------------------------------------------- */

static void STDMETHODCALLTYPE list_ClearRenderTargetView(CmdList *l, D3D12_CPU_DESCRIPTOR_HANDLE rtv, const FLOAT color[4], UINT rect_count, const D3D12_RECT *rects) {
  const SoftDesc *d = view_at(rtv);
  if (!d || d->kind != DK_RTV || !d->alloc) return;
  end_pass(l);
  ensure_room(l, ROOM_BYTES, ROOM_ALLOCS);
  const UINT saved = l->rtv_count;
  l->rtv_count = 1;
  begin_pass_packet(l, d->alloc, 0, 1 /* clear colour */, color, 1.0f, 0);
  l->rtv_count = saved;
  packet(l, OP_END_PASS, NULL, 0);
}
static void STDMETHODCALLTYPE list_ClearDepthStencilView(CmdList *l, D3D12_CPU_DESCRIPTOR_HANDLE dsv, D3D12_CLEAR_FLAGS flags, FLOAT depth, UINT8 stencil, UINT rect_count, const D3D12_RECT *rects) {
  const SoftDesc *d = view_at(dsv);
  if (!d || d->kind != DK_DSV || !d->alloc) return;
  if (l->pass_open && l->dsv == d->alloc) end_pass(l);
  /* (kept until the next pass that uses the buffer: it needs a colour target) */
  l->pending_clear = 1;
  l->pending_alloc = d->alloc;
  l->pending_flags = ((flags & D3D12_CLEAR_FLAG_DEPTH) ? 2u : 0u) | ((flags & D3D12_CLEAR_FLAG_STENCIL) ? 4u : 0u);
  l->pending_depth = depth < 0.0f ? 0.0f : depth > 1.0f ? 1.0f : depth;
  l->pending_stencil = stencil;
}
static void STDMETHODCALLTYPE list_ClearUnorderedAccessViewUint(CmdList *l, D3D12_GPU_DESCRIPTOR_HANDLE g, D3D12_CPU_DESCRIPTOR_HANDLE c, ID3D12Resource *r, const UINT v[4], UINT n, const D3D12_RECT *rects) {}
static void STDMETHODCALLTYPE list_ClearUnorderedAccessViewFloat(CmdList *l, D3D12_GPU_DESCRIPTOR_HANDLE g, D3D12_CPU_DESCRIPTOR_HANDLE c, ID3D12Resource *r, const float v[4], UINT n, const D3D12_RECT *rects) {}
static void STDMETHODCALLTYPE list_DiscardResource(CmdList *l, ID3D12Resource *r, const D3D12_DISCARD_REGION *region) {}

/* ---- copies ------------------------------------------------------------------------------------------------------------------------------------------------------------------- */

static void STDMETHODCALLTYPE list_CopyBufferRegion(CmdList *l, ID3D12Resource *dst, UINT64 dst_offset, ID3D12Resource *src, UINT64 src_offset, UINT64 bytes) {
  Resource *d = (Resource *)dst, *s = (Resource *)src;
  if (!d || !s || !bytes || dst_offset > 0xFFFFFFFFu || src_offset > 0xFFFFFFFFu || bytes > 0xFFFFFFFFu) return;
  end_pass(l);
  ensure_room(l, 256, 4);
  UINT w[5] = {alloc_index(l, d->alloc), (UINT)dst_offset, alloc_index(l, s->alloc), (UINT)src_offset, (UINT)bytes};
  packet(l, OP_COPY_BUFFER, w, 5);
}

/* The mip level and layer a subresource index names. */
static void sub_split(const Resource *r, UINT sub, UINT *level, UINT *layer) {
  *level = sub % r->mips;
  *layer = sub / r->mips;
}

static void STDMETHODCALLTYPE list_CopyTextureRegion(CmdList *l, const D3D12_TEXTURE_COPY_LOCATION *dst, UINT x, UINT y, UINT z, const D3D12_TEXTURE_COPY_LOCATION *src, const D3D12_BOX *box) {
  Resource *d = (Resource *)dst->pResource, *s = (Resource *)src->pResource;
  if (!d || !s) return;
  end_pass(l);
  ensure_room(l, 1024, 8);
  if (dst->Type == D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX && src->Type == D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT) {
    /* buffer to texture */
    const D3D12_SUBRESOURCE_FOOTPRINT *fp = &src->PlacedFootprint.Footprint;
    UINT level, layer;
    sub_split(d, dst->SubresourceIndex, &level, &layer);
    const UINT bytes = d->fmt ? d->fmt->bytes : 4;
    const UINT left = box ? box->left : 0, top = box ? box->top : 0, front = box ? box->front : 0;
    const UINT w = box ? box->right - box->left : fp->Width, h = box ? box->bottom - box->top : fp->Height, depth = box ? box->back - box->front : fp->Depth;
    for (UINT k = 0; k < depth; ++k) {
      UINT64 offset = src->PlacedFootprint.Offset + (UINT64)(front + k) * fp->RowPitch * fp->Height + (UINT64)top * fp->RowPitch + (UINT64)left * bytes;
      UINT image_layer = d->kind == 1 ? z + k : layer;
      UINT payload[11] = {0, alloc_index(l, d->alloc), level, image_layer, x, y, w, h, alloc_index(l, s->alloc), (UINT)offset, fp->RowPitch};
      packet(l, OP_COPY_BUFFER_IMAGE, payload, 11);
    }
  } else if (dst->Type == D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT && src->Type == D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX) {
    /* texture to buffer */
    const D3D12_SUBRESOURCE_FOOTPRINT *fp = &dst->PlacedFootprint.Footprint;
    UINT level, layer;
    sub_split(s, src->SubresourceIndex, &level, &layer);
    const UINT bytes = s->fmt ? s->fmt->bytes : 4;
    const UINT left = box ? box->left : 0, top = box ? box->top : 0, front = box ? box->front : 0;
    const UINT w = box ? box->right - box->left : fp->Width, h = box ? box->bottom - box->top : fp->Height, depth = box ? box->back - box->front : fp->Depth;
    for (UINT k = 0; k < depth; ++k) {
      UINT64 offset = dst->PlacedFootprint.Offset + (UINT64)(z + k) * fp->RowPitch * fp->Height + (UINT64)y * fp->RowPitch + (UINT64)x * bytes;
      UINT image_layer = s->kind == 1 ? front + k : layer;
      UINT payload[11] = {1, alloc_index(l, s->alloc), level, image_layer, left, top, w, h, alloc_index(l, d->alloc), (UINT)offset, fp->RowPitch};
      packet(l, OP_COPY_BUFFER_IMAGE, payload, 11);
    }
  } else if (dst->Type == D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX && src->Type == D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX && d->mips == 1 && s->mips == 1 && d->kind == 0 && s->kind == 0) {
    const UINT w = box ? box->right - box->left : (UINT)s->desc.Width, h = box ? box->bottom - box->top : s->desc.Height;
    UINT payload[8] = {alloc_index(l, d->alloc), x, y, alloc_index(l, s->alloc), box ? box->left : 0, box ? box->top : 0, w, h};
    packet(l, OP_COPY_REGION, payload, 8);
  }
}

static void STDMETHODCALLTYPE list_CopyResource(CmdList *l, ID3D12Resource *dst, ID3D12Resource *src) {
  Resource *d = (Resource *)dst, *s = (Resource *)src;
  if (!d || !s) return;
  if (d->desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER && s->desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {
    list_CopyBufferRegion(l, dst, 0, src, 0, s->desc.Width < d->desc.Width ? s->desc.Width : d->desc.Width);
  } else if (d->mips == 1 && s->mips == 1 && d->kind == 0 && s->kind == 0 && d->fmt && s->fmt) {
    end_pass(l);
    ensure_room(l, 256, 4);
    UINT payload[8] = {alloc_index(l, d->alloc), 0, 0, alloc_index(l, s->alloc), 0, 0, (UINT)s->desc.Width, s->desc.Height};
    packet(l, OP_COPY_REGION, payload, 8);
  }
}
static void STDMETHODCALLTYPE list_CopyTiles(CmdList *l, ID3D12Resource *r, const D3D12_TILED_RESOURCE_COORDINATE *c, const D3D12_TILE_REGION_SIZE *s, ID3D12Resource *b, UINT64 o, D3D12_TILE_COPY_FLAGS f) {}
static void STDMETHODCALLTYPE list_ResolveSubresource(CmdList *l, ID3D12Resource *d, UINT ds, ID3D12Resource *s, UINT ss, DXGI_FORMAT f) {}

/* ---- barriers, queries, markers ------------------------------------------------------------------------------------------------------------------------------------ */

static void STDMETHODCALLTYPE list_ResourceBarrier(CmdList *l, UINT count, const D3D12_RESOURCE_BARRIER *barriers) {
  for (UINT i = 0; i < count; ++i) {
    if (barriers[i].Type != D3D12_RESOURCE_BARRIER_TYPE_ALIASING) continue;
    const Resource *before = (const Resource *)barriers[i].Aliasing.pResourceBefore, *after = (const Resource *)barriers[i].Aliasing.pResourceAfter;
    end_pass(l);
    ensure_room(l, 256, 4);
    UINT w[7] = {1, 2, before ? alloc_index(l, before->alloc) : 0xFFFFFFFFu, after ? alloc_index(l, after->alloc) : 0xFFFFFFFFu, 0, 0, 0};
    packet(l, OP_RESOURCE_BARRIER, w, 6);
  }
}
static void STDMETHODCALLTYPE list_ExecuteBundle(CmdList *l, ID3D12GraphicsCommandList *bundle) {}
static void STDMETHODCALLTYPE list_BeginQuery(CmdList *l, ID3D12QueryHeap *h, D3D12_QUERY_TYPE t, UINT i) {}
static void STDMETHODCALLTYPE list_EndQuery(CmdList *l, ID3D12QueryHeap *h, D3D12_QUERY_TYPE t, UINT i) {}
static void STDMETHODCALLTYPE list_ResolveQueryData(CmdList *l, ID3D12QueryHeap *h, D3D12_QUERY_TYPE t, UINT s, UINT n, ID3D12Resource *d, UINT64 o) {}
static void STDMETHODCALLTYPE list_SetPredication(CmdList *l, ID3D12Resource *b, UINT64 o, D3D12_PREDICATION_OP op) {}
static void STDMETHODCALLTYPE list_SetMarker(CmdList *l, UINT m, const void *d, UINT s) {}
static void STDMETHODCALLTYPE list_BeginEvent(CmdList *l, UINT m, const void *d, UINT s) {}
static void STDMETHODCALLTYPE list_EndEvent(CmdList *l) {}
