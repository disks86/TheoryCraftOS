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
 *   - Resource barriers are not forwarded (the allocations are not state tracked) except for aliasing. Queries, predication, indirect execution, stream output and bundles are
 *     extras.inc's. */

#define CHUNK_BYTES 65000
#define CHUNK_ALLOCS 1000
#define ROOM_BYTES 4096
#define ROOM_ALLOCS 160      /* (what a draw uses itself, and the resources its tables of descriptors reach: TABLE_ALLOCS a table) */
#define TABLE_ALLOCS 32
#define MAX_OPEN_QUERIES 64  /* queries open at once in a list */
#define CHUNK_QUERIES 250    /* the device queries of a chunk (graphics::kMaxOcclusionQueries, 256, in the kernel) */

struct Indirect;
typedef struct Chunk {
  BYTE *bytes;
  UINT size;
  struct Indirect *indirect;   /* not a stream: ExecuteIndirect, made into one when the list is executed (the arguments are read then) */
  UINT queries;                /* the kernel's device queries the stream's queries take (it takes 256: CHUNK_QUERIES here) */
  struct Resource *pred;       /* predication: the 64-bit value that decides whether the chunk runs */
  UINT64 pred_off;
  int pred_op;
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

typedef struct SoTarget {
  UINT alloc;
  UINT64 offset;
  struct Resource *fill;       /* BufferFilledSizeLocation */
  UINT64 fill_offset;
} SoTarget;

typedef struct Binding {
  RootSig *rs;
  RootArg args[16];
} Binding;

typedef struct CmdAlloc {
  DObj d;
  D3D12_COMMAND_LIST_TYPE type;
} CmdAlloc;

typedef struct CmdList CmdList;
enum { B_PSO = 1, B_TOPOLOGY, B_STENCIL_REF, B_INDEX_BUFFER, B_VERTEX_BUFFERS, B_HEAPS, B_GRAPHICS_SIGNATURE, B_COMPUTE_SIGNATURE, B_GRAPHICS_CONSTANTS, B_COMPUTE_CONSTANTS,
       B_GRAPHICS_CBV, B_GRAPHICS_SRV, B_GRAPHICS_UAV, B_COMPUTE_CBV, B_COMPUTE_SRV, B_COMPUTE_UAV, B_GRAPHICS_TABLE, B_COMPUTE_TABLE, B_DRAW, B_DRAW_INDEXED, B_DISPATCH, B_INDIRECT,
       B_BLEND_FACTOR, B_STENCIL_REFS, B_DEPTH_BIAS, B_DEPTH_BOUNDS, B_VIEW_MASK, B_SHADING_RATE, B_SHADING_RATE_IMAGE };
static void bundle_log(CmdList *l, UINT id, const UINT64 a[6], const UINT *data, UINT ndata);
#define BUNDLE_LOG(l, id, ...) do { if ((l)->bundle) { UINT64 a_[6] = {__VA_ARGS__}; bundle_log((l), (id), a_, NULL, 0); return; } } while (0)
#define BUNDLE_LOG_DATA(l, id, data, n, ...) do { if ((l)->bundle) { UINT64 a_[6] = {__VA_ARGS__}; bundle_log((l), (id), a_, (data), (n)); return; } } while (0)

/* What a bundle records: the calls, made again on the list that executes it (a bundle sets no targets, viewports or barriers and takes what the caller has set). */
typedef struct LogEntry {
  UINT id;
  UINT64 a[6];
  UINT *data;
  UINT ndata;
} LogEntry;

struct CmdList {
  DObj d;
  D3D12_COMMAND_LIST_TYPE type;
  int closed;
  int bundle;
  LogEntry *log;
  UINT nlog, cap_log;
  Chunk **chunks;
  UINT nchunks, cap_chunks;
  IUnknown **refs;
  UINT nrefs, cap_refs;
  /* the render pass's resolve parameters, copied from the program's at BeginRenderPass (kept across resets: only the room) */
  D3D12_RENDER_PASS_ENDING_ACCESS_RESOLVE_SUBRESOURCE_PARAMETERS *rp_params;
  UINT rp_params_cap;
  /* video lists (video.inc): the kernel buffers the recorded DecodeFrames' arguments were copied to (given back when the list is reset), and where the last DecodeFrame's
   * status words are in the stream - an EndQuery of decode statistics points them at the query (chunk + 1: 0 none, and the byte offset in it) */
  UINT *temps;
  UINT ntemps, cap_temps;
  UINT last_decode_chunk, last_decode_at;
  /* the kernel pipelines the recording binds, each once (its PSO keeps it until the list is reset: pso_use / pso_unuse) */
  struct BoundPipe { Pso *pso; UINT handle; } *bound;
  UINT nbound, cap_bound;
  /* the state of the recording */
  Pso *pso;
  /* the queries open (extras.inc): each is begun again in every chunk it goes on into, adding to what the chunks before counted */
  struct OpenQuery { struct QHeap *heap; UINT index; D3D12_QUERY_TYPE type; UINT kind; } open_q[MAX_OPEN_QUERIES];
  UINT nopen_q;
  Resource *pred;              /* the predication in force */
  UINT64 pred_off;
  int pred_op;
  SoTarget so[4];              /* the stream output targets */
  int so_dirty;
  int so_started[4];           /* sent in this list (a target sent again after a new chunk appends where the last draws ended) */
  UINT sent_so_alloc[4];
  UINT64 sent_so_offset[4];
  SoTarget fills[16];
  UINT nfills;
  Binding gfx, cs;
  DescHeap *heaps[2];
  DescHeap *heaps_used[8];       /* every shader-visible heap the list set: their kernel copies are brought up to date when it is executed */
  UINT nheaps_used;
  D3D12_PRIMITIVE_TOPOLOGY topology;
  struct { UINT alloc, stride; UINT64 offset, size; } vb[8];      /* the vertex buffers (input slots) */
  UINT ib_alloc, ib_format;
  UINT64 ib_offset;
  D3D12_VIEWPORT viewports[16];      /* (D3D12_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE; the kernel's device takes them all with DEV_FEATURE2_VIEWPORTS, else the first) */
  D3D12_RECT scissors[16];
  UINT viewport_count, scissor_count;
  int have_viewport, have_scissor;
  UINT stencil_ref, stencil_ref_back;      /* OMSetStencilRef (both), OMSetFrontAndBackStencilRef (each its own) */
  float depth_bias[3];         /* RSSetDepthBias: the bias, its clamp and the slope - what a pipeline made with D3D12_PIPELINE_STATE_FLAG_DYNAMIC_DEPTH_BIAS draws with (0 when reset) */
  float blend_factor[4];       /* OMSetBlendFactor (0, 0, 0, 0 when the list is reset) */
  UINT rtv[8];                 /* the kernel's allocations of the colour targets */
  UINT rtv_count;
  UINT rtv_layers[8][3];       /* ... the first layer and the count of each (an array's slices, a volume's depth slices: 0, 1 for a 2D view), and the level */
  UINT dsv;
  UINT dsv_layers[3];          /* ... and of the depth buffer */
  int pass_open;
  UINT uav_only_samples, uav_only_width, uav_only_height;      /* the pass open is on a target of the device's for draws with no target (ensure_pass): its samples and size (0: not such a pass) */
  int pending_clear;           /* a depth/stencil clear waiting for a pass */
  UINT pending_width, pending_height, pending_samples;      /* (the buffer's size: for its companion, if no pass takes the clear) */
  UINT dsv_width, dsv_height, dsv_samples;                  /* the size of the depth buffer in `dsv` */
  UINT pending_alloc, pending_flags;
  UINT pending_layers[3];      /* (the cleared view's first layer, count and level: the pass that takes the clear draws into those) */
  float pending_depth;
  UINT pending_stencil;
  /* what the chunk being recorded has already sent */
  int sent_valid;
  UINT sent_pipeline;
  UINT sent_topology;
  UINT sent_vb_alloc[8], sent_ib_alloc;
  UINT64 sent_vb_offset[8], sent_ib_offset;
  UINT sent_ib_format;
  int sent_viewport, sent_scissor;
  int sent_binding;            /* 0 none, 1 graphics, 2 compute */
  int dirty_gfx, dirty_cs, dirty_heaps;
  /* (cmdlist1.inc) the upload buffer WriteBufferImmediate's values go through, and the render pass begun (its ending accesses, for EndRenderPass) */
  struct Resource *immediate;
  UINT immediate_used;
  int in_render_pass, rp_suspending;
  UINT rp_count;
  D3D12_RENDER_PASS_RENDER_TARGET_DESC rp_rts[8];
  D3D12_RENDER_PASS_DEPTH_STENCIL_DESC rp_ds;
  int rp_has_ds;
  /* (cmdlist1.inc) the optional rasterizer state: OMSetDepthBounds (0 .. 1 when reset), SetSamplePositions (no pixels: the standard positions), SetViewInstanceMask (0),
   * RSSetShadingRate / RSSetShadingRateImage (1x1, passthrough, none); and what the chunk being recorded has sent of it (raster_sent: words of each op, 0 not sent) */
  float depth_bounds[2];
  UINT sample_count_set, sample_pixels;
  D3D12_SAMPLE_POSITION sample_positions[16];
  UINT view_mask;
  UINT shading_rate, rate_combiners[2];
  struct Resource *rate_image;
  int sent_bounds, sent_positions, sent_rate;      /* (bounds, rate: 0 the stream's defaults, 1 the words, 2 not known) */
  UINT sent_bounds_words[3], sent_rate_words[4];
  UINT sent_view;              /* (the SetViewInstance words sent, view | viewport << 8 | layer << 12; 0xFFFFFFFF none) */
};

static const GUID *const g_alloc_iids[] = {&IID_ID3D12Object, &IID_ID3D12DeviceChild, &IID_ID3D12Pageable, &IID_ID3D12CommandAllocator, NULL};
static const GUID *const g_list_iids[] = {&IID_ID3D12Object, &IID_ID3D12DeviceChild, &IID_ID3D12CommandList, &IID_ID3D12GraphicsCommandList, &IID_ID3D12GraphicsCommandList1,
                                          &IID_ID3D12GraphicsCommandList2, &IID_ID3D12GraphicsCommandList3, &IID_ID3D12GraphicsCommandList4, &IID_ID3D12GraphicsCommandList5,
                                          &IID_ID3D12GraphicsCommandList6, &IID_ID3D12GraphicsCommandList7,
                                          &IID_ID3D12GraphicsCommandList8, &IID_ID3D12GraphicsCommandList9, &IID_ID3D12GraphicsCommandList10, NULL};

/* ---- allocators --------------------------------------------------------------------------------------------------------------------------------------------------------------- */

static HRESULT STDMETHODCALLTYPE dev_CreateCommandAllocator(Dev *This, D3D12_COMMAND_LIST_TYPE type, REFIID riid, void **out) {
  extern const ID3D12CommandAllocatorVtbl alloc_vtbl;
  if (type != D3D12_COMMAND_LIST_TYPE_DIRECT && type != D3D12_COMMAND_LIST_TYPE_COMPUTE && type != D3D12_COMMAND_LIST_TYPE_COPY && type != D3D12_COMMAND_LIST_TYPE_BUNDLE &&
      type != D3D12_COMMAND_LIST_TYPE_VIDEO_DECODE && type != D3D12_COMMAND_LIST_TYPE_VIDEO_PROCESS && type != D3D12_COMMAND_LIST_TYPE_VIDEO_ENCODE)
    return E_INVALIDARG;
  CmdAlloc *a = dobj_new(sizeof *a, This, &alloc_vtbl, g_alloc_iids, dobj_child_destroy);
  if (!a) return E_OUTOFMEMORY;
  a->type = type;
  return query_out((IUnknown *)a, riid, out);
}
static HRESULT STDMETHODCALLTYPE alloc_Reset(CmdAlloc *This) { return S_OK; }

/* ---- the stream ------------------------------------------------------------------------------------------------------------------------------------------------------------- */

static Chunk *chunk_cur(CmdList *l) { return l->nchunks ? l->chunks[l->nchunks - 1] : NULL; }

static void queries_suspend(CmdList *l);
static void queries_resume(CmdList *l);
static int chunk_new(CmdList *l) {
  if (l->nopen_q && chunk_cur(l) && !chunk_cur(l)->indirect) queries_suspend(l);      /* (the open queries' part in this stream ends with it) */
  if (l->nchunks == l->cap_chunks) {
    const UINT cap = l->cap_chunks ? l->cap_chunks * 2 : 4;
    Chunk **n = mem_grow(l->chunks, cap * sizeof *n);
    if (!n) return 0;
    l->chunks = n;
    l->cap_chunks = cap;
  }
  Chunk *c = mem_alloc(sizeof *c);
  if (!c) return 0;
  c->bytes = mem_alloc(CHUNK_BYTES + 256 + MAX_OPEN_QUERIES * 12);      /* (room for the EndQuery of every open query) */
  if (!c->bytes) { mem_free(c); return 0; }
  c->pred = l->pred;
  c->pred_off = l->pred_off;
  c->pred_op = l->pred_op;
  l->chunks[l->nchunks++] = c;
  l->sent_valid = 0;
  l->sent_so_alloc[0] = l->sent_so_alloc[1] = l->sent_so_alloc[2] = l->sent_so_alloc[3] = 0;
  l->sent_binding = 0;
  l->sent_pipeline = 0;
  l->sent_topology = 0xFFFFFFFFu;
  memset(l->sent_vb_alloc, 0, sizeof l->sent_vb_alloc);
  l->sent_ib_alloc = 0;
  l->sent_viewport = l->sent_scissor = 0;
  l->sent_bounds = l->sent_rate = 0;      /* (a stream starts with the kernel's defaults of the rasterizer state: the standard sample positions too) */
  l->sent_positions = l->sample_pixels == 0;
  l->sent_view = 0;
  l->dirty_heaps = 1;
  if (l->nopen_q) queries_resume(l);
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
static void end_pass(CmdList *l);
static void ensure_room(CmdList *l, UINT bytes, UINT allocs) {
  Chunk *c = chunk_cur(l);
  if (c && c->size + bytes <= CHUNK_BYTES && c->nalloc + allocs <= CHUNK_ALLOCS) return;
  end_pass(l);
  chunk_new(l);
}

enum { OP_BEGIN_PASS = 1, OP_END_PASS, OP_BIND_PIPELINE, OP_BIND_VB, OP_BIND_IB, OP_PUSH_CONSTANTS, OP_BIND_TEXTURE, OP_SET_VIEWPORT, OP_SET_SCISSOR, OP_DRAW, OP_DRAW_INDEXED, OP_COPY_REGION,
       OP_GENERATE_MIPMAPS, OP_SET_TOPOLOGY, OP_DRAW_INSTANCED, OP_DRAW_INDEXED_INSTANCED, OP_BEGIN_QUERY, OP_END_QUERY, OP_BIND_UAV, OP_DISPATCH, OP_RESOLVE_REGION, OP_SET_STREAM_OUTPUT,
       OP_SET_ROOT_SIGNATURE, OP_SET_DESCRIPTOR_HEAPS, OP_SET_ROOT_CONSTANTS, OP_SET_ROOT_DESCRIPTOR, OP_SET_ROOT_TABLE, OP_RESOURCE_BARRIER, OP_COPY_BUFFER, OP_COPY_BUFFER_IMAGE,
       OP_DECODE_FRAME, OP_VIDEO_PROCESS };
/* (the kernel's ops of the optional rasterizer state, numbered apart: CommandStream.hpp) */
enum { OP_SET_DEPTH_BOUNDS = 56, OP_SET_SAMPLE_POSITIONS = 57, OP_SET_VIEW_INSTANCE = 58, OP_SET_SHADING_RATE = 59 };
static void send_raster_state(CmdList *l);

static void end_pass(CmdList *l) {
  if (!l->pass_open) return;
  packet(l, OP_END_PASS, NULL, 0);
  l->pass_open = 0;
}

/* The list holds a reference to `o` until it is reset. Returns 0 when it could not (no memory): a caller that made `o` for the list must then let it go itself. */
static int keep(CmdList *l, IUnknown *o) {
  if (!o) return 1;
  for (UINT i = 0; i < l->nrefs; ++i)
    if (l->refs[i] == o) return 1;
  if (l->nrefs == l->cap_refs) {
    if (l->cap_refs > 0x7FFFFFFFu / sizeof(IUnknown *) / 2) return 0;
    const UINT cap = l->cap_refs ? l->cap_refs * 2 : 16;
    IUnknown **n = mem_grow(l->refs, cap * sizeof *n);
    if (!n) return 0;
    l->refs = n;
    l->cap_refs = cap;
  }
  o->lpVtbl->AddRef(o);
  l->refs[l->nrefs++] = o;
  return 1;
}

static void indirect_free(struct Indirect *i);
static void list_free_recording(CmdList *l) {
  for (UINT i = 0; i < l->nchunks; ++i) {
    if (l->chunks[i]->indirect) indirect_free(l->chunks[i]->indirect);
    mem_free(l->chunks[i]->bytes);
    mem_free(l->chunks[i]);
  }
  l->nchunks = 0;
  for (UINT i = 0; i < l->nlog; ++i) mem_free(l->log[i].data);
  l->nlog = 0;
  for (UINT i = 0; i < l->nbound; ++i) pso_unuse(l->bound[i].pso, l->bound[i].handle);      /* (before the references: the list holds the PSOs) */
  l->nbound = 0;
  for (UINT i = 0; i < l->nrefs; ++i) l->refs[i]->lpVtbl->Release(l->refs[i]);
  l->nrefs = 0;
  for (UINT i = 0; i < l->ntemps; ++i) kmt_destroy_allocation(l->d.dev->kdev, l->temps[i]);
  l->ntemps = 0;
  l->last_decode_chunk = 0;
}

static void list_reset_state(CmdList *l) {
  memset(&l->pso, 0, sizeof *l - offsetof(CmdList, pso));
  l->topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
  l->depth_bounds[1] = 1.0f;
  l->sent_view = 0xFFFFFFFFu;      /* (what the stream has is not known - ClearState in the middle of one -: sent again) */
  l->sent_bounds = l->sent_rate = 2;
  l->dirty_gfx = l->dirty_cs = l->dirty_heaps = 1;
}

static void list_destroy(DxObj *o) {
  CmdList *l = (CmdList *)o;
  list_free_recording(l);
  mem_free(l->chunks);
  mem_free(l->refs);
  mem_free(l->rp_params);
  mem_free(l->temps);
  mem_free(l->bound);
  mem_free(l->log);
  dobj_child_destroy(o);
}

static HRESULT video_list_make(Dev *This, D3D12_COMMAND_LIST_TYPE type, REFIID riid, void **out);
static HRESULT STDMETHODCALLTYPE dev_CreateCommandList(Dev *This, UINT mask, D3D12_COMMAND_LIST_TYPE type, ID3D12CommandAllocator *allocator, ID3D12PipelineState *initial, REFIID riid, void **out) {
  extern const ID3D12GraphicsCommandList10Vtbl list_vtbl;
  if (allocator && (type == D3D12_COMMAND_LIST_TYPE_VIDEO_DECODE || type == D3D12_COMMAND_LIST_TYPE_VIDEO_PROCESS || type == D3D12_COMMAND_LIST_TYPE_VIDEO_ENCODE))
    return video_list_make(This, type, riid, out);
  if (!allocator || (type != D3D12_COMMAND_LIST_TYPE_DIRECT && type != D3D12_COMMAND_LIST_TYPE_COMPUTE && type != D3D12_COMMAND_LIST_TYPE_COPY && type != D3D12_COMMAND_LIST_TYPE_BUNDLE)) return E_INVALIDARG;
  CmdList *l = dobj_new(sizeof *l, This, &list_vtbl, g_list_iids, list_destroy);
  if (!l) return E_OUTOFMEMORY;
  l->type = type;
  l->bundle = type == D3D12_COMMAND_LIST_TYPE_BUNDLE;
  list_reset_state(l);
  if (!l->bundle && !chunk_new(l)) { dx_Release(OBJ(l)); return E_OUTOFMEMORY; }
  if (initial) { l->pso = (Pso *)initial; keep(l, (IUnknown *)initial); }
  return query_out((IUnknown *)l, riid, out);
}

static D3D12_COMMAND_LIST_TYPE STDMETHODCALLTYPE list_GetType(CmdList *This) { return This->type; }
static void flush_pending_clear(CmdList *l);
static void queries_end_all(CmdList *l);
static HRESULT STDMETHODCALLTYPE list_Close(CmdList *This) {
  if (This->closed) return E_FAIL;
  end_pass(This);
  if (!This->bundle) flush_pending_clear(This);
  queries_end_all(This);      /* (a query begun and not ended: it ends with the list, as the kernel's stream needs every query ended) */
  This->closed = 1;
  return S_OK;
}
static HRESULT STDMETHODCALLTYPE list_Reset(CmdList *This, ID3D12CommandAllocator *allocator, ID3D12PipelineState *initial) {
  if (!This->closed || !allocator) return E_FAIL;
  list_free_recording(This);
  list_reset_state(This);
  This->closed = 0;
  if (!This->bundle && !chunk_new(This)) return E_OUTOFMEMORY;
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

/* `layers`: the first layer, the count and the level of each colour target and then of the depth buffer (NULL: layer 0 of level 0 of each); a pass of layers says them (kPassLayers),
 * one of levels says those (kPassLevels). */
static void begin_pass_packet_layers(CmdList *l, UINT color_alloc, UINT depth_alloc, UINT flags, const float color[4], float depth, UINT stencil, const UINT (*layers)[3], const UINT *depth_layers) {
  UINT w[9 + 7 + 18 + 9];
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
  UINT targets = 1;
  for (UINT i = 1; i < l->rtv_count && i < 8 && flags != 0xFFFFFFFFu && color_alloc == l->rtv[0]; ++i) w[n++] = alloc_index(l, l->rtv[i]), ++targets;
  int layered = 0;
  for (UINT i = 0; layers && i < targets; ++i) layered |= layers[i][0] != 0 || layers[i][1] > 1;
  if (depth_layers && depth_alloc) layered |= depth_layers[0] != 0 || depth_layers[1] > 1;
  if (layered) {
    /* (kPassLayers: for each colour target and the depth buffer, its first layer and count) */
    w[2] |= 8u;
    for (UINT i = 0; i < targets; ++i) w[n++] = layers ? layers[i][0] : 0, w[n++] = layers && layers[i][1] ? layers[i][1] : 1;
    w[n++] = depth_layers && depth_alloc ? depth_layers[0] : 0;
    w[n++] = depth_layers && depth_alloc && depth_layers[1] ? depth_layers[1] : 1;
  }
  int leveled = 0;
  for (UINT i = 0; layers && i < targets; ++i) leveled |= layers[i][2] != 0;
  if (depth_layers && depth_alloc) leveled |= depth_layers[2] != 0;
  if (leveled) {
    /* (kPassLevels: the level of each colour target and of the depth buffer) */
    w[2] |= 16u;
    for (UINT i = 0; i < targets; ++i) w[n++] = layers ? layers[i][2] : 0;
    w[n++] = depth_layers && depth_alloc ? depth_layers[2] : 0;
  }
  packet(l, OP_BEGIN_PASS, w, n);
}
static void begin_pass_packet(CmdList *l, UINT color_alloc, UINT depth_alloc, UINT flags, const float color[4], float depth, UINT stencil) {
  begin_pass_packet_layers(l, color_alloc, depth_alloc, flags, color, depth, stencil, NULL, NULL);
}

/* The kernel's passes have a colour target; Direct3D 12's need not (a shadow map is drawn with a depth buffer alone). Such a pass draws into a colour target of the depth
 * buffer's size that nothing reads: the buffer's companion, made the first time it is needed and kept until the buffer is destroyed (resource.inc drops it). It costs the
 * memory of a 32-bit image of that size; a device whose passes need no colour target would save it. Returns 0 if it cannot be made. */
static UINT depth_companion(Dev *dev, UINT depth, UINT width, UINT height, UINT samples) {
  UINT color = 0;
  if (!depth || !width || !height) return 0;
  AcquireSRWLockExclusive(&dev->companion_lock);
  for (UINT i = 0; i < dev->companion_count; ++i)
    if (dev->companions[i].depth == depth && dev->companions[i].width == width && dev->companions[i].height == height) { color = dev->companions[i].color; break; }
  if (!color) {
    if (dev->companion_count == dev->companion_capacity) {
      const UINT capacity = dev->companion_capacity ? dev->companion_capacity * 2 : 8;
      struct Companion *grown = mem_alloc((SIZE_T)capacity * sizeof *grown);
      if (grown) {
        if (dev->companion_count) memcpy(grown, dev->companions, (SIZE_T)dev->companion_count * sizeof *grown);
        mem_free(dev->companions);
        dev->companions = grown;
        dev->companion_capacity = capacity;
      }
    }
    if (dev->companion_count < dev->companion_capacity) {
      KmtAllocDesc ad;
      memset(&ad, 0, sizeof ad);
      ad.flags = KA_RENDER_TARGET;
      ad.format = KF_A8R8G8B8;
      ad.width = width;
      ad.height = height;
      ad.mips = ad.layers = 1;
      ad.samples = samples ? samples : 1;
      if (KMT_OK(kmt_create_allocation(dev->kdev, &ad, &color, NULL, NULL)) && color) {
        dev->companions[dev->companion_count].depth = depth;
        dev->companions[dev->companion_count].width = width;
        dev->companions[dev->companion_count].height = height;
        dev->companions[dev->companion_count++].color = color;
      } else {
        color = 0;
      }
    }
  }
  ReleaseSRWLockExclusive(&dev->companion_lock);
  return color;
}
/* (the depth buffer is being destroyed) */
static void depth_companion_drop(Dev *dev, UINT depth) {
  for (;;) {      /* (every level's companion of it) */
    UINT color = 0;
    AcquireSRWLockExclusive(&dev->companion_lock);
    for (UINT i = 0; i < dev->companion_count; ++i) {
      if (dev->companions[i].depth != depth) continue;
      color = dev->companions[i].color;
      dev->companions[i] = dev->companions[--dev->companion_count];
      break;
    }
    ReleaseSRWLockExclusive(&dev->companion_lock);
    if (!color) return;
    kmt_destroy_allocation(dev->kdev, color);
  }
}

/* The colour target of a pass with no render target and no depth buffer (a draw that writes unordered access views only: Direct3D's UAV-only rendering, ForcedSampleCount): the
 * kernel's passes need one, so it is a target of the device's of at least `width` x `height` with `samples` samples that nothing reads (the viewport says what is rasterized); made
 * the first time one that size is needed and kept until the device goes. 0 if it cannot be made. */
static UINT uav_only_target(Dev *dev, UINT width, UINT height, UINT samples) {
  UINT color = 0;
  if (!width || !height || width > 16384 || height > 16384) return 0;
  AcquireSRWLockExclusive(&dev->companion_lock);
  for (UINT i = 0; i < dev->uav_target_count; ++i)
    if (dev->uav_targets[i].width >= width && dev->uav_targets[i].height >= height && dev->uav_targets[i].samples == samples) { color = dev->uav_targets[i].color; break; }
  if (!color) {
    if (dev->uav_target_count == dev->uav_target_capacity) {
      const UINT capacity = dev->uav_target_capacity ? dev->uav_target_capacity * 2 : 4;
      struct UavOnlyTarget *grown = mem_alloc((SIZE_T)capacity * sizeof *grown);
      if (grown) {
        if (dev->uav_target_count) memcpy(grown, dev->uav_targets, (SIZE_T)dev->uav_target_count * sizeof *grown);
        mem_free(dev->uav_targets);
        dev->uav_targets = grown;
        dev->uav_target_capacity = capacity;
      }
    }
    if (dev->uav_target_count < dev->uav_target_capacity) {
      KmtAllocDesc ad;
      memset(&ad, 0, sizeof ad);
      ad.flags = KA_RENDER_TARGET;
      ad.format = KF_A8R8G8B8;
      ad.width = width;
      ad.height = height;
      ad.mips = ad.layers = 1;
      ad.samples = samples;
      if (KMT_OK(kmt_create_allocation(dev->kdev, &ad, &color, NULL, NULL)) && color) {
        struct UavOnlyTarget t = {color, width, height, samples};
        dev->uav_targets[dev->uav_target_count++] = t;
      } else {
        color = 0;
      }
    }
  }
  ReleaseSRWLockExclusive(&dev->companion_lock);
  return color;
}

/* A depth clear that no pass has taken (the next pass was of another depth buffer, or the list ends) is a pass of its own, on the buffer's companion. */
static void flush_pending_clear(CmdList *l) {
  static const float zero[4] = {0, 0, 0, 0};
  if (!l->pending_clear) return;
  l->pending_clear = 0;
  const UINT color = depth_companion(l->d.dev, l->pending_alloc, l->pending_width, l->pending_height, l->pending_samples);
  if (!color) return;
  ensure_room(l, ROOM_BYTES, ROOM_ALLOCS);
  const UINT saved = l->rtv_count;
  l->rtv_count = 1;
  begin_pass_packet_layers(l, color, l->pending_alloc, l->pending_flags, zero, l->pending_depth, l->pending_stencil, NULL, l->pending_layers);
  l->rtv_count = saved;
  packet(l, OP_END_PASS, NULL, 0);
}

/* Begins the pass of the current targets if there is none. Returns 0 when there is nothing to draw on. */
static int ensure_pass(CmdList *l) {
  static const float zero[4] = {0, 0, 0, 0};
  if (l->pass_open) return 1;
  UINT color = l->rtv_count ? l->rtv[0] : 0, depth = l->dsv;
  if (l->pending_clear && (l->pending_alloc != depth || l->pending_layers[0] != l->dsv_layers[0] || l->pending_layers[1] != l->dsv_layers[1] || l->pending_layers[2] != l->dsv_layers[2])) flush_pending_clear(l);
  const int companion = !color && depth && !(l->pso && l->pso->so_entries);
  if (companion) color = depth_companion(l->d.dev, depth, l->dsv_width, l->dsv_height, l->dsv_samples);
  if (!color && l->pso && l->pso->so_entries) {
    /* stream output without a target: the kernel's passes have one, a 1 x 1 target of the device's that nothing is rasterized into */
    if (!l->d.dev->null_target) {
      KmtAllocDesc ad;
      memset(&ad, 0, sizeof ad);
      ad.flags = KA_RENDER_TARGET | KA_TEXTURE;
      ad.format = KF_A8R8G8B8;
      ad.width = ad.height = 1;
      kmt_create_allocation(l->d.dev->kdev, &ad, &l->d.dev->null_target, NULL, NULL);
    }
    color = l->d.dev->null_target;
    depth = 0;
  }
  int uav_only = 0;
  if (!color && !depth && l->pso && !l->pso->compute) {
    /* no target at all: a draw that writes unordered access views only, over the viewport (with the pipeline's samples: ForcedSampleCount) */
    const float right = l->have_viewport ? l->viewports[0].TopLeftX + l->viewports[0].Width : 0.0f, bottom = l->have_viewport ? l->viewports[0].TopLeftY + l->viewports[0].Height : 0.0f;
    const UINT width = right >= 1.0f && right <= 16384.0f ? (UINT)(right + 0.999f) : 0, height = bottom >= 1.0f && bottom <= 16384.0f ? (UINT)(bottom + 0.999f) : 0;
    color = uav_only_target(l->d.dev, width, height, l->pso->samples);
    uav_only = color != 0;
    l->uav_only_samples = uav_only ? l->pso->samples : 0;
    l->uav_only_width = width, l->uav_only_height = height;
  } else {
    l->uav_only_samples = 0;
  }
  if (!color) return 0;
  UINT flags = 0;
  float depth_value = 1.0f;
  UINT stencil = 0;
  if (depth && l->pending_clear && l->pending_alloc == depth) {
    flags = l->pending_flags;
    depth_value = l->pending_depth;
    stencil = l->pending_stencil;
    l->pending_clear = 0;
  }
  if (companion) {      /* (the pass has the companion alone as its colour target, whatever the list's render targets are) */
    const UINT saved = l->rtv_count;
    l->rtv_count = 1;
    begin_pass_packet_layers(l, color, depth, flags, zero, depth_value, stencil, NULL, l->dsv_layers);
    l->rtv_count = saved;
  } else if (uav_only) {
    const UINT saved = l->rtv_count;
    l->rtv_count = 1;
    begin_pass_packet_layers(l, color, 0, flags, zero, depth_value, stencil, NULL, NULL);
    l->rtv_count = saved;
  } else {
    /* (the layers of the list's targets; a target of the device's own - stream output with none - is a 2D image) */
    begin_pass_packet_layers(l, color, depth, flags, zero, depth_value, stencil, l->rtv_count && color == l->rtv[0] ? (const UINT(*)[3])l->rtv_layers : NULL,
                             depth && depth == l->dsv ? l->dsv_layers : NULL);
  }
  l->pass_open = 1;
  /* a new pass forgets what was bound (a device may have its own idea of what carries over) */
  l->sent_pipeline = 0;
  l->sent_topology = 0xFFFFFFFFu;
  memset(l->sent_vb_alloc, 0, sizeof l->sent_vb_alloc);
  l->sent_ib_alloc = 0;
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
        /* The resources the descriptors of the table are views of go into the list of the chunk: the kernel binds a texture or a buffer by its place
         * there, and refuses a draw that reads one that is not in it. */
        if (h->desc.Type == D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV) {
          UINT reach = b->rs->pdescriptors[i], listed = 0;
          if (reach > h->desc.NumDescriptors - index) reach = h->desc.NumDescriptors - index;
          for (UINT k = 0; k < reach && listed < TABLE_ALLOCS; ++k) {
            const SoftDesc *sd = &h->descs[index + k];
            if (sd->alloc && (sd->kind == DK_SRV || sd->kind == DK_SRV_BUFFER || sd->kind == DK_UAV || sd->kind == DK_CBV || sd->kind == DK_UAV_TEXTURE)) { alloc_index(l, sd->alloc); ++listed; }
            if (sd->kind == DK_SRV && (sd->size & SRV_ALTERNATE) == SRV_ALTERNATE) { alloc_index(l, (UINT)sd->size); ++listed; }      /* (a view's array mirror too) */
            if (sd->kind == DK_UAV && sd->sampler[2]) { alloc_index(l, sd->sampler[2]); ++listed; }      /* (and an unordered access view's counter) */
          }
        }
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

/* The list binds the kernel pipeline `handle` of p, which pso_pipeline gave it with a use: the list keeps that use until it is reset (once for each pipeline: a
 * second use is given back). Returns 0 without memory to remember it (the use given back too). */
static int list_bind_pipeline(CmdList *l, Pso *p, UINT handle) {
  for (UINT i = 0; i < l->nbound; ++i)
    if (l->bound[i].pso == p && l->bound[i].handle == handle) { pso_unuse(p, handle); return 1; }
  if (l->nbound == l->cap_bound) {
    const UINT cap = l->cap_bound ? l->cap_bound * 2 : 8;
    struct BoundPipe *n = l->cap_bound < (1u << 20) ? mem_grow(l->bound, (SIZE_T)cap * sizeof *n) : NULL;
    if (!n) { pso_unuse(p, handle); return 0; }
    l->bound = n;
    l->cap_bound = cap;
  }
  l->bound[l->nbound].pso = p;
  l->bound[l->nbound].handle = handle;
  l->nbound++;
  return 1;
}

/* Whether the tables of a binding reach views of mirrors (SRV_MIRRORS: planes the kernel copies into an image of its own before they are read). */
static int binding_has_mirrors(CmdList *l, const Binding *b) {
  if (!b->rs) return 0;
  for (UINT i = 0; i < b->rs->parameter_count && i < 16; ++i) {
    const RootArg *a = &b->args[i];
    if (a->kind != 5) continue;
    UINT index = 0;
    const DescHeap *h = heap_for_gpu(l, a->gpu, &index);
    if (!h || h->desc.Type != D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV) continue;
    UINT reach = b->rs->pdescriptors[i];
    if (reach > h->desc.NumDescriptors - index) reach = h->desc.NumDescriptors - index;
    for (UINT k = 0; k < reach; ++k)
      if ((h->descs[index + k].kind == DK_SRV || h->descs[index + k].kind == DK_UAV_TEXTURE) && (h->descs[index + k].offset & SRV_MIRRORS)) return 1;
  }
  return 0;
}

/* Everything a draw needs sent: the pass, the pipeline, the root arguments, the buffers, the viewport. Returns 0 when the draw cannot be made. */
static int prepare_draw(CmdList *l, int indexed) {
  if (!l->pso || l->pso->compute || !l->gfx.rs) return 0;
  ensure_room(l, ROOM_BYTES, ROOM_ALLOCS);
  /* (the kernel brings the mirrors a table's views show up to date when the table is set outside a pass - inside one, what the GPU drew cannot be read back -: such a
   * binding goes before the pass begins, ending the pass it would have been sent in) */
  if ((l->sent_binding != 1 || l->dirty_gfx || l->dirty_heaps) && binding_has_mirrors(l, &l->gfx)) {
    end_pass(l);
    /* (the pipeline first: the kernel copies only the images it reads - a view's own, or its array for a unit declared one -; the pass sends it again) */
    UINT strides[8] = {0};
    for (UINT slot = 0; slot < 8; ++slot)
      if (l->pso->slots_used >> slot & 1) strides[slot] = l->vb[slot].stride;
    const UINT pipeline = pso_pipeline(l->pso, strides, l->stencil_ref, l->stencil_ref_back, l->depth_bias, l->blend_factor);
    if (pipeline && list_bind_pipeline(l, l->pso, pipeline)) packet(l, OP_BIND_PIPELINE, &pipeline, 1);
    send_binding(l, &l->gfx, 1);
    l->dirty_heaps = 0;
  }
  if (l->pass_open && l->uav_only_samples && !l->rtv_count && !l->dsv) {
    /* (a pass on the device's target for draws with no target: another for a pipeline of other samples, or a viewport past it) */
    const float right = l->have_viewport ? l->viewports[0].TopLeftX + l->viewports[0].Width : 0.0f, bottom = l->have_viewport ? l->viewports[0].TopLeftY + l->viewports[0].Height : 0.0f;
    if (l->pso->samples != l->uav_only_samples || right > (float)l->uav_only_width || bottom > (float)l->uav_only_height) end_pass(l);
  }
  if (!ensure_pass(l)) return 0;
  if (l->sent_binding != 1 || l->dirty_gfx || l->dirty_heaps) {
    send_binding(l, &l->gfx, 1);
    l->dirty_heaps = 0;
  }
  UINT strides[8] = {0};
  for (UINT slot = 0; slot < 8; ++slot)
    if (l->pso->slots_used >> slot & 1) strides[slot] = l->vb[slot].stride;
  const UINT pipeline = pso_pipeline(l->pso, strides, l->stencil_ref, l->stencil_ref_back, l->depth_bias, l->blend_factor);
  if (!pipeline) return 0;
  if (!list_bind_pipeline(l, l->pso, pipeline)) return 0;
  if (pipeline != l->sent_pipeline) {
    packet(l, OP_BIND_PIPELINE, &pipeline, 1);
    l->sent_pipeline = pipeline;
  }
  for (UINT s = 0; s < 4; ++s) {
    const SoTarget *t = &l->so[s];
    if (t->alloc == l->sent_so_alloc[s] && !(t->alloc && l->so_dirty)) continue;
    UINT w[3] = {s, t->alloc ? alloc_index(l, t->alloc) : 0xFFFFFFFFu, 0};
    /* (the first time a bind is sent it starts at the view's offset; sent again after a new chunk it goes on from where the draws before ended) */
    w[2] = !t->alloc ? 0 : l->so_started[s] && !l->so_dirty ? 0xFFFFFFFFu : (UINT)t->offset;
    packet(l, OP_SET_STREAM_OUTPUT, w, 3);
    l->sent_so_alloc[s] = t->alloc;
    l->so_started[s] = t->alloc != 0;
  }
  l->so_dirty = 0;
  const UINT topology = topology_code(l->topology);
  if (topology != l->sent_topology) {
    packet(l, OP_SET_TOPOLOGY, &topology, 1);
    l->sent_topology = topology;
  }
  for (UINT slot = 0; slot < 8; ++slot) {
    if (!(l->pso->slots_used >> slot & 1) || !l->vb[slot].alloc) continue;
    if (l->vb[slot].alloc == l->sent_vb_alloc[slot] && l->vb[slot].offset == l->sent_vb_offset[slot]) continue;
    UINT w[3] = {alloc_index(l, l->vb[slot].alloc), (UINT)l->vb[slot].offset, slot};
    packet(l, OP_BIND_VB, w, slot ? 3 : 2);      /* (the slot is a third word, left out for the first buffer) */
    l->sent_vb_alloc[slot] = l->vb[slot].alloc;
    l->sent_vb_offset[slot] = l->vb[slot].offset;
  }
  if (indexed && l->ib_alloc && (l->ib_alloc != l->sent_ib_alloc || l->ib_offset != l->sent_ib_offset || l->ib_format != l->sent_ib_format)) {
    UINT w[3] = {alloc_index(l, l->ib_alloc), l->ib_format == DXGI_FORMAT_R32_UINT ? 1u : 0u, (UINT)l->ib_offset};
    packet(l, OP_BIND_IB, w, 3);
    l->sent_ib_alloc = l->ib_alloc;
    l->sent_ib_offset = l->ib_offset;
    l->sent_ib_format = l->ib_format;
  }
  const UINT several = (l->d.dev->caps.features2 & DEV_FEATURE2_VIEWPORTS) ? 16u : 1u;
  if (l->have_viewport && !l->sent_viewport) {
    UINT w[6 * 16];
    const UINT n = l->viewport_count < several ? l->viewport_count : several;
    for (UINT i = 0; i < n; ++i) {
      const D3D12_VIEWPORT *v = &l->viewports[i];
      const UINT words[6] = {bits_of(v->TopLeftX), bits_of(v->TopLeftY), bits_of(v->Width), bits_of(v->Height), bits_of(v->MinDepth), bits_of(v->MaxDepth)};
      memcpy(w + 6 * i, words, sizeof words);
    }
    packet(l, OP_SET_VIEWPORT, w, 6 * n);
    l->sent_viewport = 1;
  }
  if (l->have_scissor && !l->sent_scissor) {
    UINT w[4 * 16];
    const UINT n = l->scissor_count < several ? l->scissor_count : several;
    for (UINT i = 0; i < n; ++i) {
      const D3D12_RECT *r = &l->scissors[i];
      const LONG width = r->right - r->left, height = r->bottom - r->top;
      const UINT words[4] = {(UINT)r->left, (UINT)r->top, width > 0 ? (UINT)width : 0, height > 0 ? (UINT)height : 0};
      memcpy(w + 4 * i, words, sizeof words);
    }
    packet(l, OP_SET_SCISSOR, w, 4 * n);
    l->sent_scissor = 1;
  }
  send_raster_state(l);
  return 1;
}

/* ---- drawing and dispatching ------------------------------------------------------------------------------------------------------------------------------------------- */

/* View instancing (a PSO with more than one view) is draw-call looping: the draw is made once for each view of the mask - all the PSO's views, or with
 * D3D12_VIEW_INSTANCING_FLAG_ENABLE_VIEW_INSTANCE_MASKING those of SetViewInstanceMask's mask -, each after the kernel's SetViewInstance of the view (SV_ViewID) and its
 * location (added to the shaders' viewport and render target array index). The views of one draw come one after the other: Direct3D 12 orders a view's primitives and
 * leaves the order between views open. A PSO without views draws as view 0 at no offset. Returns the views (bit n: view n) and sends view 0's state for a plain PSO. */
static UINT draw_views(CmdList *l) {
  const Pso *p = l->pso;
  if (!p->views_on) {
    if (l->sent_view != 0 && (l->d.dev->caps.features2 & DEV_FEATURE2_VIEW_INSTANCING)) {
      const UINT w[3] = {0, 0, 0};
      packet(l, OP_SET_VIEW_INSTANCE, w, 3);
      l->sent_view = 0;
    }
    return 1;
  }
  const UINT all = (1u << p->view_count) - 1;
  return (p->view_flags & D3D12_VIEW_INSTANCING_FLAG_ENABLE_VIEW_INSTANCE_MASKING) ? l->view_mask & all : all;
}
static void view_select(CmdList *l, UINT view) {
  const Pso *p = l->pso;
  if (!p->views_on) return;
  const UINT vp = p->view_loc[view].ViewportArrayIndex, rt = p->view_loc[view].RenderTargetArrayIndex;
  const UINT key = view | vp << 8 | rt << 12;
  if (key == l->sent_view) return;
  const UINT w[3] = {view, vp, rt};
  packet(l, OP_SET_VIEW_INSTANCE, w, 3);
  l->sent_view = key;
}
static void STDMETHODCALLTYPE list_DrawInstanced(CmdList *l, UINT vertices, UINT instances, UINT first_vertex, UINT first_instance) { BUNDLE_LOG(l, B_DRAW, vertices, instances, first_vertex, first_instance);
  if (!vertices || !instances || !prepare_draw(l, 0)) return;
  const UINT views = draw_views(l);
  for (UINT view = 0; view < 4; ++view) {
    if (!(views >> view & 1)) continue;
    view_select(l, view);
    if (instances == 1 && first_instance == 0) {
      UINT w[2] = {vertices, first_vertex};
      packet(l, OP_DRAW, w, 2);
    } else {
      UINT w[4] = {vertices, first_vertex, instances, first_instance};
      packet(l, OP_DRAW_INSTANCED, w, 4);
    }
  }
}
static void STDMETHODCALLTYPE list_DrawIndexedInstanced(CmdList *l, UINT indices, UINT instances, UINT first_index, INT base_vertex, UINT first_instance) { BUNDLE_LOG(l, B_DRAW_INDEXED, indices, instances, first_index, (UINT)base_vertex, first_instance);
  if (!indices || !instances || !l->ib_alloc || !prepare_draw(l, 1)) return;
  const UINT views = draw_views(l);
  for (UINT view = 0; view < 4; ++view) {
    if (!(views >> view & 1)) continue;
    view_select(l, view);
    if (instances == 1 && first_instance == 0) {
      UINT w[3] = {indices, first_index, (UINT)base_vertex};
      packet(l, OP_DRAW_INDEXED, w, 3);
    } else {
      UINT w[5] = {indices, first_index, (UINT)base_vertex, instances, first_instance};
      packet(l, OP_DRAW_INDEXED_INSTANCED, w, 5);
    }
  }
}
static void STDMETHODCALLTYPE list_Dispatch(CmdList *l, UINT x, UINT y, UINT z) { BUNDLE_LOG(l, B_DISPATCH, x, y, z);
  if (!l->pso || !l->pso->compute || !l->cs.rs || !x || !y || !z) return;
  end_pass(l);
  ensure_room(l, ROOM_BYTES, ROOM_ALLOCS);
  UINT pipeline = l->pso->compute_pipe;
  if (l->sent_binding != 2 || l->dirty_cs || l->dirty_heaps) {
    /* (a binding with mirrors goes after the pipeline, which says which images of the views the kernel copies; the pipeline is bound again after it all the same) */
    if (binding_has_mirrors(l, &l->cs)) packet(l, OP_BIND_PIPELINE, &pipeline, 1);
    send_binding(l, &l->cs, 2);
    l->dirty_heaps = 0;
  }
  packet(l, OP_BIND_PIPELINE, &pipeline, 1);
  l->sent_pipeline = 0;
  UINT w[3] = {x, y, z};
  packet(l, OP_DISPATCH, w, 3);
}

/* ---- state setting -------------------------------------------------------------------------------------------------------------------------------------------------------------- */

static void STDMETHODCALLTYPE list_SetPipelineState(CmdList *l, ID3D12PipelineState *p) { keep(l, (IUnknown *)p); BUNDLE_LOG(l, B_PSO, (UINT64)(UINT_PTR)p);
  l->pso = (Pso *)p;
  keep(l, (IUnknown *)p);
}
static void STDMETHODCALLTYPE list_IASetPrimitiveTopology(CmdList *l, D3D12_PRIMITIVE_TOPOLOGY t) { BUNDLE_LOG(l, B_TOPOLOGY, t); l->topology = t; }
/* (up to D3D12_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE: a primitive is drawn with the pair its SV_ViewportArrayIndex names; more than that is the debug layer's error,
 * and the first sixteen are taken) */
static void STDMETHODCALLTYPE list_RSSetViewports(CmdList *l, UINT n, const D3D12_VIEWPORT *v) {
  if (!n || !v) return;
  if (n > 16) n = 16;
  memcpy(l->viewports, v, (SIZE_T)n * sizeof *v);
  l->viewport_count = n;
  l->have_viewport = 1;
  l->sent_viewport = 0;
}
static void STDMETHODCALLTYPE list_RSSetScissorRects(CmdList *l, UINT n, const D3D12_RECT *r) {
  if (!n || !r) return;
  if (n > 16) n = 16;
  memcpy(l->scissors, r, (SIZE_T)n * sizeof *r);
  l->scissor_count = n;
  l->have_scissor = 1;
  l->sent_scissor = 0;
}
/* (NULL is 1, 1, 1, 1; the factor takes effect in the kernel pipeline the next draw makes or finds for it) */
static void STDMETHODCALLTYPE list_OMSetBlendFactor(CmdList *l, const FLOAT factor[4]) {
  const float ones[4] = {1, 1, 1, 1};
  const float *f = factor ? factor : ones;
  UINT bits[4];
  for (UINT c = 0; c < 4; ++c) bits[c] = bits_of(f[c]);
  BUNDLE_LOG(l, B_BLEND_FACTOR, bits[0], bits[1], bits[2], bits[3]);
  memcpy(l->blend_factor, f, sizeof l->blend_factor);
}
static void STDMETHODCALLTYPE list_OMSetStencilRef(CmdList *l, UINT ref) { BUNDLE_LOG(l, B_STENCIL_REF, ref); l->stencil_ref = l->stencil_ref_back = ref; }
static void STDMETHODCALLTYPE list_IASetIndexBuffer(CmdList *l, const D3D12_INDEX_BUFFER_VIEW *view) { BUNDLE_LOG(l, B_INDEX_BUFFER, view ? view->BufferLocation : 0, view ? view->SizeInBytes : 0, view ? view->Format : 0);
  if (!view || !view->BufferLocation) { l->ib_alloc = 0; return; }
  UINT64 offset = 0;
  Resource *r = va_find(view->BufferLocation, &offset);
  l->ib_alloc = r ? r->alloc : 0;
  l->ib_offset = offset;
  l->ib_format = view->Format;
}
static void STDMETHODCALLTYPE list_IASetVertexBuffers(CmdList *l, UINT start, UINT count, const D3D12_VERTEX_BUFFER_VIEW *views) {
  if (l->bundle) {
    /* (a bundle keeps the views, four values each: the address, the size and the stride) */
    UINT packed[64];
    UINT n = 0;
    for (UINT i = 0; i < count && i < 8 && views; ++i) {
      packed[n++] = (UINT)views[i].BufferLocation;
      packed[n++] = (UINT)(views[i].BufferLocation >> 32);
      packed[n++] = views[i].SizeInBytes;
      packed[n++] = views[i].StrideInBytes;
    }
    UINT64 a_[6] = {start, count};
    bundle_log(l, B_VERTEX_BUFFERS, a_, packed, n);
    return;
  }
  for (UINT i = 0; i < count && start + i < 8; ++i) {
    const UINT slot = start + i;
    l->vb[slot].alloc = 0;
    if (!views || !views[i].BufferLocation) continue;
    UINT64 offset = 0;
    Resource *r = va_find(views[i].BufferLocation, &offset);
    l->vb[slot].alloc = r ? r->alloc : 0;
    l->vb[slot].offset = offset;
    l->vb[slot].stride = views[i].StrideInBytes;
    l->vb[slot].size = views[i].SizeInBytes;
  }
}
static void STDMETHODCALLTYPE list_OMSetRenderTargets(CmdList *l, UINT count, const D3D12_CPU_DESCRIPTOR_HANDLE *rtvs, BOOL single, const D3D12_CPU_DESCRIPTOR_HANDLE *dsv) {
  end_pass(l);
  l->rtv_count = 0;
  for (UINT i = 0; i < count && i < 8 && i < l->d.dev->caps.max_color_targets && rtvs; ++i) {
    D3D12_CPU_DESCRIPTOR_HANDLE h = single ? (D3D12_CPU_DESCRIPTOR_HANDLE){rtvs[0].ptr + (SIZE_T)i * DESCRIPTOR_STRIDE} : rtvs[i];
    const SoftDesc *d = view_at(h);
    const int rtv = d && d->kind == DK_RTV;
    l->rtv_layers[l->rtv_count][0] = rtv ? d->slice : 0;
    l->rtv_layers[l->rtv_count][1] = rtv && d->stride ? d->stride : 1;
    l->rtv_layers[l->rtv_count][2] = rtv ? d->mip : 0;
    l->rtv[l->rtv_count++] = rtv ? d->alloc : 0;
  }
  l->dsv = 0;
  l->dsv_layers[0] = 0, l->dsv_layers[1] = 1, l->dsv_layers[2] = 0;
  if (dsv && dsv->ptr) {
    const SoftDesc *d = view_at(*dsv);
    if (d && d->kind == DK_DSV) {
      l->dsv = d->alloc;
      l->dsv_layers[0] = d->slice, l->dsv_layers[1] = d->stride ? d->stride : 1, l->dsv_layers[2] = d->mip;
      l->dsv_width = (UINT)d->size;
      l->dsv_height = (UINT)(d->size >> 32);
      l->dsv_samples = (UINT)d->offset;
    }
  }
}

static void STDMETHODCALLTYPE list_SetDescriptorHeaps(CmdList *l, UINT count, ID3D12DescriptorHeap *const *heaps) { if (l->bundle) { for (UINT k = 0; k < count; ++k) keep(l, (IUnknown *)heaps[k]); } BUNDLE_LOG(l, B_HEAPS, count, count > 0 ? (UINT64)(UINT_PTR)heaps[0] : 0, count > 1 ? (UINT64)(UINT_PTR)heaps[1] : 0);
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
static void STDMETHODCALLTYPE list_SetGraphicsRootSignature(CmdList *l, ID3D12RootSignature *rs) { keep(l, (IUnknown *)rs); BUNDLE_LOG(l, B_GRAPHICS_SIGNATURE, (UINT64)(UINT_PTR)rs); set_signature(l, &l->gfx, rs, &l->dirty_gfx); }
static void STDMETHODCALLTYPE list_SetComputeRootSignature(CmdList *l, ID3D12RootSignature *rs) { keep(l, (IUnknown *)rs); BUNDLE_LOG(l, B_COMPUTE_SIGNATURE, (UINT64)(UINT_PTR)rs); set_signature(l, &l->cs, rs, &l->dirty_cs); }

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
static void STDMETHODCALLTYPE list_SetGraphicsRoot32BitConstant(CmdList *l, UINT i, UINT v, UINT dst) { BUNDLE_LOG_DATA(l, B_GRAPHICS_CONSTANTS, &v, 1, i, dst); set_constants(l, &l->gfx, &l->dirty_gfx, i, 1, &v, dst); }
static void STDMETHODCALLTYPE list_SetComputeRoot32BitConstant(CmdList *l, UINT i, UINT v, UINT dst) { BUNDLE_LOG_DATA(l, B_COMPUTE_CONSTANTS, &v, 1, i, dst); set_constants(l, &l->cs, &l->dirty_cs, i, 1, &v, dst); }
static void STDMETHODCALLTYPE list_SetGraphicsRoot32BitConstants(CmdList *l, UINT i, UINT n, const void *d, UINT dst) { BUNDLE_LOG_DATA(l, B_GRAPHICS_CONSTANTS, d, n, i, dst); set_constants(l, &l->gfx, &l->dirty_gfx, i, n, d, dst); }
static void STDMETHODCALLTYPE list_SetComputeRoot32BitConstants(CmdList *l, UINT i, UINT n, const void *d, UINT dst) { BUNDLE_LOG_DATA(l, B_COMPUTE_CONSTANTS, d, n, i, dst); set_constants(l, &l->cs, &l->dirty_cs, i, n, d, dst); }
static void STDMETHODCALLTYPE list_SetGraphicsRootConstantBufferView(CmdList *l, UINT i, UINT64 a) { BUNDLE_LOG(l, B_GRAPHICS_CBV, i, a); set_view(&l->gfx, &l->dirty_gfx, i, 2, a); }
static void STDMETHODCALLTYPE list_SetComputeRootConstantBufferView(CmdList *l, UINT i, UINT64 a) { BUNDLE_LOG(l, B_COMPUTE_CBV, i, a); set_view(&l->cs, &l->dirty_cs, i, 2, a); }
static void STDMETHODCALLTYPE list_SetGraphicsRootShaderResourceView(CmdList *l, UINT i, UINT64 a) { BUNDLE_LOG(l, B_GRAPHICS_SRV, i, a); set_view(&l->gfx, &l->dirty_gfx, i, 3, a); }
static void STDMETHODCALLTYPE list_SetComputeRootShaderResourceView(CmdList *l, UINT i, UINT64 a) { BUNDLE_LOG(l, B_COMPUTE_SRV, i, a); set_view(&l->cs, &l->dirty_cs, i, 3, a); }
static void STDMETHODCALLTYPE list_SetGraphicsRootUnorderedAccessView(CmdList *l, UINT i, UINT64 a) { BUNDLE_LOG(l, B_GRAPHICS_UAV, i, a); set_view(&l->gfx, &l->dirty_gfx, i, 4, a); }
static void STDMETHODCALLTYPE list_SetComputeRootUnorderedAccessView(CmdList *l, UINT i, UINT64 a) { BUNDLE_LOG(l, B_COMPUTE_UAV, i, a); set_view(&l->cs, &l->dirty_cs, i, 4, a); }
static void STDMETHODCALLTYPE list_SetGraphicsRootDescriptorTable(CmdList *l, UINT i, D3D12_GPU_DESCRIPTOR_HANDLE h) { BUNDLE_LOG(l, B_GRAPHICS_TABLE, i, h.ptr); set_table(&l->gfx, &l->dirty_gfx, i, h.ptr); }
static void STDMETHODCALLTYPE list_SetComputeRootDescriptorTable(CmdList *l, UINT i, D3D12_GPU_DESCRIPTOR_HANDLE h) { BUNDLE_LOG(l, B_COMPUTE_TABLE, i, h.ptr); set_table(&l->cs, &l->dirty_cs, i, h.ptr); }

/* ---- clears -------------------------------------------------------------------------------------------------------------------------------------------------------------------- */

static void STDMETHODCALLTYPE list_ClearRenderTargetView(CmdList *l, D3D12_CPU_DESCRIPTOR_HANDLE rtv, const FLOAT color[4], UINT rect_count, const D3D12_RECT *rects) {
  const SoftDesc *d = view_at(rtv);
  if (!d || d->kind != DK_RTV || !d->alloc) return;
  end_pass(l);
  ensure_room(l, ROOM_BYTES, ROOM_ALLOCS);
  const UINT saved = l->rtv_count;
  l->rtv_count = 1;
  /* (a narrow view clears its channels: the kernel's image keeps the others as zeros and the maximum alpha; an integer view of a plane - a mirror of it, whose red, or red
   * and green, go back to the plane - clears to the integers the values are, as float bits: the kernel's clear colour of an integer image) */
  const UINT mask = format_write_mask(d->format);
  const float narrow[4] = {color[0], (mask & 2) ? color[1] : 0.0f, 0.0f, 1.0f};
  float integers[4] = {0, 0, 0, 0};
  const int integer = d->format == DXGI_FORMAT_R8_UINT || d->format == DXGI_FORMAT_R8G8_UINT || d->format == DXGI_FORMAT_R16_UINT || d->format == DXGI_FORMAT_R16G16_UINT;
  if (integer) {
    const float limit = d->format == DXGI_FORMAT_R8_UINT || d->format == DXGI_FORMAT_R8G8_UINT ? 255.0f : 65535.0f;
    for (int c = 0; c < 4; ++c) {
      const float v = color[c] != color[c] || color[c] < 0.0f ? 0.0f : color[c] > limit ? limit : color[c];
      const UINT u = (UINT)v;
      memcpy(&integers[c], &u, 4);
    }
  }
  const UINT layers[1][3] = {{d->slice, d->stride ? d->stride : 1, d->mip}};      /* (the view's layers of its level) */
  begin_pass_packet_layers(l, d->alloc, 0, 1 /* clear colour */, integer ? integers : mask == 0xF ? color : narrow, 1.0f, 0, layers, NULL);
  l->rtv_count = saved;
  packet(l, OP_END_PASS, NULL, 0);
}
static void STDMETHODCALLTYPE list_ClearDepthStencilView(CmdList *l, D3D12_CPU_DESCRIPTOR_HANDLE dsv, D3D12_CLEAR_FLAGS flags, FLOAT depth, UINT8 stencil, UINT rect_count, const D3D12_RECT *rects) {
  const SoftDesc *d = view_at(dsv);
  if (!d || d->kind != DK_DSV || !d->alloc) return;
  if (l->pass_open && l->dsv == d->alloc) end_pass(l);
  if (l->pending_clear && (l->pending_alloc != d->alloc || l->pending_layers[0] != d->slice || l->pending_layers[1] != (d->stride ? d->stride : 1) || l->pending_layers[2] != d->mip)) flush_pending_clear(l);
  /* (kept until the next pass that uses the buffer, which clears as it begins; a pass of its own if none does: flush_pending_clear) */
  l->pending_clear = 1;
  l->pending_alloc = d->alloc;
  l->pending_layers[0] = d->slice, l->pending_layers[1] = d->stride ? d->stride : 1, l->pending_layers[2] = d->mip;
  l->pending_width = (UINT)d->size;
  l->pending_height = (UINT)(d->size >> 32);
  l->pending_samples = (UINT)d->offset;
  l->pending_flags = ((flags & D3D12_CLEAR_FLAG_DEPTH) ? 2u : 0u) | ((flags & D3D12_CLEAR_FLAG_STENCIL) ? 4u : 0u);
  l->pending_depth = depth < 0.0f ? 0.0f : depth > 1.0f ? 1.0f : depth;
  l->pending_stencil = stencil;
}
static void uav_clear_list(CmdList *l, D3D12_CPU_DESCRIPTOR_HANDLE cpu, ID3D12Resource *resource, const UINT u[4], const float f[4], int floats, UINT rect_count, const D3D12_RECT *rects);
static void STDMETHODCALLTYPE list_ClearUnorderedAccessViewUint(CmdList *l, D3D12_GPU_DESCRIPTOR_HANDLE g, D3D12_CPU_DESCRIPTOR_HANDLE c, ID3D12Resource *r, const UINT v[4], UINT n, const D3D12_RECT *rects) {
  const float none[4] = {0, 0, 0, 0};
  if (v) uav_clear_list(l, c, r, v, none, 0, n, rects);
}
static void STDMETHODCALLTYPE list_ClearUnorderedAccessViewFloat(CmdList *l, D3D12_GPU_DESCRIPTOR_HANDLE g, D3D12_CPU_DESCRIPTOR_HANDLE c, ID3D12Resource *r, const float v[4], UINT n, const D3D12_RECT *rects) {
  const UINT none[4] = {0, 0, 0, 0};
  if (v) uav_clear_list(l, c, r, none, v, 1, n, rects);
}
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
/* What a subresource is in the kernel: the allocation, its level and layer, and the format of its texels (a plane's for a planar texture, whose every plane and slice is a 2D
 * allocation of one level). 0 for a subresource the resource does not have. */
static UINT sub_target(const Resource *r, UINT sub, UINT *level, UINT *layer, const FormatInfo **fi) {
  if (sub >= res_subresources(r)) return 0;
  if (r->planes) {
    *level = *layer = 0;
    *fi = plane_format(r->fmt, sub / r->layers);
    return r->plane_alloc[sub];      /* (plane * layers + slice: the subresource index itself) */
  }
  /* (a depth buffer's plane: its subresources after the depth's are the stencil's, whose footprints are bytes) */
  const UINT per_plane = r->mips * (r->kind == 1 ? 1 : r->layers);
  const int depth = r->fmt && r->fmt->depth;
  sub_split(r, depth ? sub % per_plane : sub, level, layer);
  *fi = depth ? depth_plane_format(r->desc.Format, sub / per_plane) : r->fmt;
  return r->alloc;
}
/* The plane a subresource of a depth buffer is of (0 the depth, 1 the stencil); 0 for the other textures' (planar textures are allocations of a plane each). */
static UINT sub_depth_plane(const Resource *r, UINT sub) { return r->fmt && r->fmt->depth && !r->planes ? sub / (r->mips * (r->kind == 1 ? 1 : r->layers)) : 0; }
/* Whether the kernel keeps no memory of the texture's contents - a depth buffer, a multisampled image -, which Direct3D 12 copies only as whole subresources: a copy of one has
 * no box or one of the whole subresource, and goes to (0, 0, 0) (the debug layer's error on Windows otherwise; the copy is not made). */
static int whole_copies_only(const Resource *r) { return (r->fmt && r->fmt->depth) || r->desc.SampleDesc.Count > 1; }
static int whole_box(const D3D12_BOX *box, UINT w, UINT h, UINT d) {
  return !box || (box->left == 0 && box->top == 0 && box->front == 0 && box->right == w && box->bottom == h && box->back == d);
}
/* A CopyBufferImage packet: with the narrow layout of the texture's format as its twelfth word when it has one. */
static void copy_buffer_image_packet(CmdList *l, UINT direction, UINT image, UINT level, UINT layer, UINT x, UINT y, UINT w, UINT h, UINT buffer, UINT offset, UINT pitch,
                                     const FormatInfo *fi) {
  UINT payload[12] = {direction, image, level, layer, x, y, w, h, buffer, offset, pitch, fi ? fi->narrow : 0};
  packet(l, OP_COPY_BUFFER_IMAGE, payload, fi && fi->narrow ? 12 : 11);
}

/* CopyTextureRegion of a depth buffer's plane, or of a multisampled image (whole_copies_only): between a subresource and a footprint (not of a multisampled image), the plane in the
 * kernel's depth layout of the resource's format (depth_layout), or between two subresources of one format, sample count and size, the same plane of each. */
static void copy_whole_region(CmdList *l, const D3D12_TEXTURE_COPY_LOCATION *dst, UINT x, UINT y, UINT z, const D3D12_TEXTURE_COPY_LOCATION *src, const D3D12_BOX *box) {
  Resource *d = (Resource *)dst->pResource, *s = (Resource *)src->pResource;
  if (x || y || z) return;
  if (l->pending_clear) flush_pending_clear(l);      /* (a depth clear recorded before comes first) */
  if (dst->Type == D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX && src->Type == D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX) {
    UINT dl, dy, sl, sy;
    const FormatInfo *dfi = NULL, *sfi = NULL;
    const UINT di = sub_target(d, dst->SubresourceIndex, &dl, &dy, &dfi), si = sub_target(s, src->SubresourceIndex, &sl, &sy, &sfi);
    if (!di || !si || !whole_copies_only(d) || !whole_copies_only(s) || !d->fmt || !s->fmt || d->fmt->kernel != s->fmt->kernel || !d->fmt->depth != !s->fmt->depth ||
        d->desc.SampleDesc.Count != s->desc.SampleDesc.Count || d->kind == 1 || s->kind == 1)
      return;
    const UINT plane = sub_depth_plane(s, src->SubresourceIndex);
    const UINT w = mip_extent((UINT)s->desc.Width, sl), h = mip_extent(s->desc.Height, sl);
    if (plane != sub_depth_plane(d, dst->SubresourceIndex) || w != mip_extent((UINT)d->desc.Width, dl) || h != mip_extent(d->desc.Height, dl) || !whole_box(box, w, h, 1)) return;
    end_pass(l);
    ensure_room(l, 256, 4);
    UINT payload[14] = {alloc_index(l, di), 0, 0, alloc_index(l, si), 0, 0, w, h, dl, dy, sl, sy, 1, d->fmt->depth ? 1u << plane : 0u};
    packet(l, OP_COPY_REGION, payload, 14);
    return;
  }
  const int to_texture = dst->Type == D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  const D3D12_TEXTURE_COPY_LOCATION *texture = to_texture ? dst : src, *footprint = to_texture ? src : dst;
  Resource *t = (Resource *)texture->pResource, *b = (Resource *)footprint->pResource;
  if (footprint->Type != D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT || t->desc.SampleDesc.Count > 1 || !t->fmt || !t->fmt->depth || b->desc.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER) return;
  UINT level, layer;
  const FormatInfo *fi = NULL;
  const UINT image = sub_target(t, texture->SubresourceIndex, &level, &layer, &fi);
  const D3D12_SUBRESOURCE_FOOTPRINT *fp = &footprint->PlacedFootprint.Footprint;
  const UINT w = mip_extent((UINT)t->desc.Width, level), h = mip_extent(t->desc.Height, level);
  /* (the whole subresource: the footprint of its size - into the texture the box, if any, is of the whole footprint -, a pitch it fits in) */
  if (!image || !fi || fp->Width != w || fp->Height != h || fp->Depth != 1 || !whole_box(box, w, h, 1) || fp->RowPitch < (UINT64)w * fi->bytes) return;
  if (footprint->PlacedFootprint.Offset > 0xFFFFFFFFu || footprint->PlacedFootprint.Offset + (UINT64)fp->RowPitch * (h - 1) + (UINT64)w * fi->bytes > b->desc.Width) return;
  end_pass(l);
  ensure_room(l, 256, 4);
  UINT payload[12] = {to_texture ? 0u : 1u, alloc_index(l, image), level, layer, 0, 0, w, h, alloc_index(l, b->alloc), (UINT)footprint->PlacedFootprint.Offset, fp->RowPitch,
                      depth_layout(t->desc.Format, sub_depth_plane(t, texture->SubresourceIndex))};
  packet(l, OP_COPY_BUFFER_IMAGE, payload, 12);
}

static void STDMETHODCALLTYPE list_CopyTextureRegion(CmdList *l, const D3D12_TEXTURE_COPY_LOCATION *dst, UINT x, UINT y, UINT z, const D3D12_TEXTURE_COPY_LOCATION *src, const D3D12_BOX *box) {
  Resource *d = (Resource *)dst->pResource, *s = (Resource *)src->pResource;
  if (!d || !s) return;
  /* (depth buffers and multisampled images: whole subresources) */
  if ((dst->Type == D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX && whole_copies_only(d)) || (src->Type == D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX && whole_copies_only(s))) {
    copy_whole_region(l, dst, x, y, z, src, box);
    return;
  }
  end_pass(l);
  ensure_room(l, 1024, 8);
  if (dst->Type == D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX && src->Type == D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT) {
    /* buffer to texture */
    const D3D12_SUBRESOURCE_FOOTPRINT *fp = &src->PlacedFootprint.Footprint;
    UINT level, layer;
    const FormatInfo *fi = NULL;
    const UINT image = sub_target(d, dst->SubresourceIndex, &level, &layer, &fi);
    if (!image) return;
    const UINT left = box ? box->left : 0, top = box ? box->top : 0, front = box ? box->front : 0;
    const UINT w = box ? box->right - box->left : fp->Width, h = box ? box->bottom - box->top : fp->Height, depth = box ? box->back - box->front : fp->Depth;
    for (UINT k = 0; k < depth; ++k) {
      /* (a compressed format's rows are rows of blocks: the box is on whole blocks) */
      UINT64 offset = src->PlacedFootprint.Offset + (UINT64)(front + k) * fp->RowPitch * format_rows(fi, fp->Height) + (UINT64)format_rows(fi, top) * fp->RowPitch + format_row_bytes(fi, left);
      UINT image_layer = d->kind == 1 ? z + k : layer;
      copy_buffer_image_packet(l, 0, alloc_index(l, image), level, image_layer, format_kx(fi, x), y, format_kw(fi, w), h, alloc_index(l, s->alloc), (UINT)offset, fp->RowPitch, fi);
    }
  } else if (dst->Type == D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT && src->Type == D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX) {
    /* texture to buffer */
    const D3D12_SUBRESOURCE_FOOTPRINT *fp = &dst->PlacedFootprint.Footprint;
    UINT level, layer;
    const FormatInfo *fi = NULL;
    const UINT image = sub_target(s, src->SubresourceIndex, &level, &layer, &fi);
    if (!image) return;
    const UINT left = box ? box->left : 0, top = box ? box->top : 0, front = box ? box->front : 0;
    const UINT w = box ? box->right - box->left : fp->Width, h = box ? box->bottom - box->top : fp->Height, depth = box ? box->back - box->front : fp->Depth;
    for (UINT k = 0; k < depth; ++k) {
      UINT64 offset = dst->PlacedFootprint.Offset + (UINT64)(z + k) * fp->RowPitch * format_rows(fi, fp->Height) + (UINT64)format_rows(fi, y) * fp->RowPitch + format_row_bytes(fi, x);
      UINT image_layer = s->kind == 1 ? front + k : layer;
      copy_buffer_image_packet(l, 1, alloc_index(l, image), level, image_layer, format_kx(fi, left), top, format_kw(fi, w), h, alloc_index(l, d->alloc), (UINT)offset, fp->RowPitch, fi);
    }
  } else if (dst->Type == D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX && src->Type == D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX) {
    /* (a subresource into a subresource: a level of a layer - a volume's level, its slices from front - into a level of a layer at (x, y, z); a planar texture's plane of a slice
     * is a 2D allocation of one level) */
    UINT dl, dy, sl, sy;
    const FormatInfo *dfi = NULL, *sfi = NULL;
    const UINT di = sub_target(d, dst->SubresourceIndex, &dl, &dy, &dfi), si = sub_target(s, src->SubresourceIndex, &sl, &sy, &sfi);
    if (!di || !si) return;
    UINT sw = mip_extent((UINT)s->desc.Width, sl), sh = mip_extent(s->desc.Height, sl);
    if (s->planes) { sw = (UINT)s->desc.Width, sh = s->desc.Height; plane_extent(s->fmt, src->SubresourceIndex / s->layers, &sw, &sh); }
    const UINT sd = s->kind == 1 ? mip_extent(s->layers, sl) : 1;
    const UINT left = box ? box->left : 0, top = box ? box->top : 0, front = box ? box->front : 0;
    const UINT w = box ? box->right - box->left : sw, h = box ? box->bottom - box->top : sh, depth = box ? box->back - box->front : sd;
    if (!w || !h || !depth) return;
    const UINT dst_layer = d->kind == 1 ? z : dy, src_layer = s->kind == 1 ? front : sy;
    /* (a packed 4:2:2 format's columns are pairs in the kernel's image) */
    UINT payload[13] = {alloc_index(l, di), format_kx(dfi, x), y, alloc_index(l, si), format_kx(sfi, left), top, format_kw(sfi, w), h, dl, dst_layer, sl, src_layer, depth};
    const int plain = dl == 0 && sl == 0 && dst_layer == 0 && src_layer == 0 && depth == 1 && !s->planes ? (s->mips == 1 && d->mips == 1 && s->kind == 0 && d->kind == 0) : s->planes && d->planes;
    packet(l, OP_COPY_REGION, payload, plain ? 8 : 13);
  }
}

static void STDMETHODCALLTYPE list_CopyResource(CmdList *l, ID3D12Resource *dst, ID3D12Resource *src) {
  Resource *d = (Resource *)dst, *s = (Resource *)src;
  if (!d || !s) return;
  if (d->desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER && s->desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {
    list_CopyBufferRegion(l, dst, 0, src, 0, s->desc.Width < d->desc.Width ? s->desc.Width : d->desc.Width);
  } else if (d->planes && s->planes && d->planes == s->planes && d->layers == s->layers) {
    /* (every plane of every slice) */
    end_pass(l);
    for (UINT i = 0; i < s->planes * s->layers; ++i) {
      ensure_room(l, 256, 4);
      UINT w = (UINT)s->desc.Width, h = s->desc.Height;
      plane_extent(s->fmt, i / s->layers, &w, &h);
      UINT payload[8] = {alloc_index(l, d->plane_alloc[i]), 0, 0, alloc_index(l, s->plane_alloc[i]), 0, 0, w, h};
      packet(l, OP_COPY_REGION, payload, 8);
    }
  } else if (d->fmt && s->fmt && !d->planes && !s->planes && d->mips == s->mips && d->kind == s->kind && d->layers == s->layers) {
    /* (every level with all its layers - a volume's level its slices there; a 2D image of one level is the one packet of eight words every device takes. Depth buffers - both planes - and
     * multisampled images, which the device copies, of one format, sample count and size) */
    if ((whole_copies_only(d) || whole_copies_only(s)) && (d->fmt->kernel != s->fmt->kernel || !d->fmt->depth != !s->fmt->depth || d->desc.SampleDesc.Count != s->desc.SampleDesc.Count ||
                                                         d->desc.Width != s->desc.Width || d->desc.Height != s->desc.Height))
      return;
    if (l->pending_clear) flush_pending_clear(l);      /* (a depth clear recorded before comes first) */
    end_pass(l);
    for (UINT level = 0; level < s->mips; ++level) {
      ensure_room(l, 256, 4);
      const UINT layers = s->kind == 1 ? mip_extent(s->layers, level) : s->kind == 0 ? 1 : s->layers;
      UINT payload[13] = {alloc_index(l, d->alloc), 0, 0, alloc_index(l, s->alloc), 0, 0, format_kw(s->fmt, mip_extent((UINT)s->desc.Width, level)), mip_extent(s->desc.Height, level),
                          level, 0, level, 0, layers};
      packet(l, OP_COPY_REGION, payload, s->mips == 1 && s->kind == 0 ? 8 : 13);
    }
  }
}
/* The samples of a multisampled image into an ordinary one of the same format and size: a colour image's are averaged, a depth buffer's first sample is taken (the kernel's
 * ResolveRegion; it refuses formats or sizes that differ). */
static void STDMETHODCALLTYPE list_ResolveSubresource(CmdList *l, ID3D12Resource *dst, UINT ds, ID3D12Resource *src, UINT ss, DXGI_FORMAT f) {
  Resource *d = (Resource *)dst, *s = (Resource *)src;
  if (!d || !s || !d->alloc || !s->alloc || ds != 0 || ss != 0 || l->bundle) return;
  if (s->desc.SampleDesc.Count < 2 || d->desc.SampleDesc.Count > 1) return;
  end_pass(l);
  ensure_room(l, 64, 4);
  const int depth = (s->desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) != 0;
  UINT payload[3] = {alloc_index(l, d->alloc), alloc_index(l, s->alloc), 1 /* graphics::ResolveMode::Sample0 */};
  packet(l, OP_RESOLVE_REGION, payload, depth ? 3 : 2);
}

/* ---- barriers, queries, markers ------------------------------------------------------------------------------------------------------------------------------------ */

static int unpredicated_begin(CmdList *l);
static void unpredicated_end(CmdList *l, int split);
static void STDMETHODCALLTYPE list_ResourceBarrier(CmdList *l, UINT count, const D3D12_RESOURCE_BARRIER *barriers) {
  for (UINT i = 0; i < count; ++i) {
    if (barriers[i].Type != D3D12_RESOURCE_BARRIER_TYPE_ALIASING) continue;
    const Resource *before = (const Resource *)barriers[i].Aliasing.pResourceBefore, *after = (const Resource *)barriers[i].Aliasing.pResourceAfter;
    end_pass(l);
    const int split = unpredicated_begin(l);      /* (a barrier is not predicated) */
    /* (a planar texture is an allocation for each plane of each slice: every one of the outgoing resource's goes out, every one of the incoming's comes in) */
    const UINT outs = before && before->planes ? before->planes * before->layers : 1, ins = after && after->planes ? after->planes * after->layers : 1;
    for (UINT o = 0; o < outs; ++o)
      for (UINT n = 0; n < ins; ++n) {
        ensure_room(l, 256, 4);
        const UINT out_alloc = before ? (before->planes ? before->plane_alloc[o] : before->alloc) : 0, in_alloc = after ? (after->planes ? after->plane_alloc[n] : after->alloc) : 0;
        UINT w[7] = {1, 2, out_alloc ? alloc_index(l, out_alloc) : 0xFFFFFFFFu, in_alloc ? alloc_index(l, in_alloc) : 0xFFFFFFFFu, 0, 0, 0};
        packet(l, OP_RESOURCE_BARRIER, w, 6);
      }
    unpredicated_end(l, split);
  }
}
static void STDMETHODCALLTYPE list_SetMarker(CmdList *l, UINT m, const void *d, UINT s) {}
static void STDMETHODCALLTYPE list_BeginEvent(CmdList *l, UINT m, const void *d, UINT s) {}
static void STDMETHODCALLTYPE list_EndEvent(CmdList *l) {}
