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

/* d3d11.dll for TheoryCraftOS: Direct3D 11 as a layer on this system's Direct3D 12 (d3d12.dll, which is the user-mode half of the kernel's driver) -
 * what Windows calls "11 on 12". A device here is a Direct3D 12 device, one command queue and one command list; the immediate context records into
 * the list and sends it when something has to be seen: Flush, a Map that reads, a Present, a CPU write into a buffer a recorded draw uses.
 *
 * What maps to what:
 *   buffers            a Direct3D 12 buffer in an upload heap, mapped for good: Map hands that memory out, a draw uses its GPU address.
 *   constant buffers   Direct3D 12 here has one block of constants for all stages (b0). The block of a draw is put together from the buffers bound to
 *                      the vertex shader's slots, in slot order, and then the pixel shader's (d3d12.dll is told where those begin:
 *                      TcosD3D12PipelineOptions), and goes into a ring of upload memory - so a constant buffer may change between draws freely.
 *   textures           a Direct3D 12 texture in a default heap. The CPU reaches it through copies (UpdateSubresource, Map of a dynamic or staging
 *                      texture: a copy in memory, sent or fetched). R8 and A8 textures are kept as R8G8B8A8.
 *   vertex layouts     the kernel takes 32-bit floats only: a layout with R8G8B8A8 / B8G8R8A8 colours gets its vertex buffers converted (a second
 *                      buffer, made again when the first changes).
 *   states             a Direct3D 12 pipeline state for each combination of shaders, layout, blend, depth and rasterizer state that is drawn with,
 *                      kept in a cache.
 *   textures, samplers a table of 8 of each for the pixel shader (t0 .. t7, s0 .. s7; the sampler of t<n> is s<n>, as the kernel pairs them).
 *   swap chains        dxgi.dll asks the device for its back buffers and their pixels (ITcosPresenter, src/Windows/tcosdx.h).
 *
 * Not there: deferred contexts, geometry / hull / domain / compute shaders, textures and constant buffers of the vertex shader beyond what is said
 * above, 1D / 3D / cube textures, multisampling, queries, mipmaps of render targets (GenerateMips does nothing), buffers as shader resources, blend
 * factors, depth-only passes (a draw needs a render target), stream output. The methods for them answer E_NOTIMPL or do nothing. */
#define INITGUID
#define COBJMACROS
#define CONST_VTABLE
#define WIDL_C_INLINE_WRAPPERS 1
#define WIN32_LEAN_AND_MEAN
#include "../dxcommon.h"
#include <stddef.h>
#include <d3d11.h>
#include <d3d12.h>
#include <dxgi1_3.h>
#include <dxgi1_2.h>
#include "../tcosdx.h"

#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wincompatible-pointer-types"
#pragma GCC diagnostic ignored "-Wcast-function-type"

__asm__(".text\n.globl ___chkstk_ms\n___chkstk_ms:\n ret\n");

#define OBJ(x) ((DxObj *)(x))
#define TEXTURE_SLOTS 8u              /* t0 .. t7 and s0 .. s7 of the pixel shader */
#define CONSTANT_SLOTS 14u            /* b0 .. b13 a stage */
#define CONSTANT_BLOCK 4096u          /* the bytes of constants a draw has in all (the kernel's limit) */
#define RING_BYTES (1u << 20)         /* the upload memory the constant blocks of one command list go into */
#define VIEW_HEAP 4096u               /* descriptors of the heap the textures of the draws go into */
#define RETIRED_MAX 256u              /* upload buffers a list being recorded may still read after the program has moved on from them */
#define RETIRED_MAX_BYTES (32u << 20) /* ... and their bytes: beyond that the list is sent first, as without renaming (memory for textures must stay) */
#define SPARES_MAX 64u                /* upload buffers kept for the next buffer of their size */
#define SPARES_MAX_BYTES (16u << 20)  /* ... in all */
#define RTV_SLOTS 1024u
#define DSV_SLOTS 256u
#define PIPELINES 96u
#define VERTEX_SLOTS 8u

typedef struct Dev Dev;
typedef struct Tex Tex;
typedef struct Buf Buf;
/* A D3D12 upload buffer and where it is mapped (NULL: not mapped). */
typedef struct Spare {
  ID3D12Resource *res;
  BYTE *data;
  UINT64 bytes;
} Spare;

/* ---- the objects ----------------------------------------------------------------------------------------------------------------------------------- */

/* Every object of a device: the head, the device (a reference) and a number no other object of the process has (what the pipeline cache knows a
 * state or shader by: an address may be given to another object later). */
typedef struct Child {
  DxObj o;
  Dev *dev;
  UINT id;
} Child;
static LONG g_next_id;

struct Buf {
  Child c;
  D3D11_BUFFER_DESC desc;
  ID3D12Resource *res;
  BYTE *data;                  /* the buffer's memory: d3d12.dll's CPU copy of `res` (its Map hands out the same memory every time, and it stays there while
                                * not mapped). `res` is mapped only while the program has the buffer mapped: d3d12.dll hashes every mapped buffer at each
                                * submission; one this runtime writes itself is marked changed with buf_touched() */
  int mapped;                  /* the program has it mapped (Map without Unmap yet) */
  UINT64 bytes;                /* the size of `res` (a spare may be bigger than the buffer) */
  UINT used;                   /* the command list (Dev.epoch) that has a draw reading `res` */
  UINT converted_used;         /* ... reading `converted` (the two are apart: `res` may be renamed while a draw reads the copy made from the old) */
  UINT64 converted_bytes;      /* the size of `converted` */
  UINT version;                /* counts the writes: what a converted copy is compared with */
  /* the copy with float colours, for a layout that needs one */
  ID3D12Resource *converted;
  UINT converted_layout, converted_version, converted_stride, converted_offset, converted_vertices, converted_out_stride;
};

struct Tex {
  Child c;
  D3D11_TEXTURE2D_DESC desc;   /* (a volume: its width, height and levels, with one layer; its depth is `slices`) */
  UINT kind;                   /* the kernel's TextureKind: 0 2D, 1 3D (a volume), 2 2D array, 3 cube */
  UINT slices;                 /* a volume's depth (1 for the others) */
  UINT kmips;                  /* the levels the Direct3D 12 texture has: the kernel keeps one of a volume, an array or a cube, whose other levels are dropped */
  D3D11_TEXTURE3D_DESC desc3;  /* a volume: as the program made it */
  DXGI_FORMAT stored;          /* the format of the Direct3D 12 texture */
  UINT bpp, stored_bpp;        /* bytes a pixel: as the program has it, as it is kept */
  int expand;                  /* 0: kept as it is; 1: R8 kept as RGBA (r, 0, 0, 1); 2: A8 kept as RGBA (0, 0, 0, a); 3: RG8 kept as RGBA (r, g, 0, 1) */
  int depth;                   /* a depth buffer: the CPU does not reach it */
  UINT block;                  /* a block-compressed format (BC1 .. BC3): the bytes of a 4 x 4 block (bpp is the same); its rows are rows of blocks */
  ID3D12Resource *res;
  UINT used;
  UINT sampled;                /* the command list that has a draw sampling it */
  UINT written;                /* the command list that draws into it, clears it or copies into it */
  BYTE **shadow;               /* by subresource: the copy a Map hands out (NULL until one is mapped) */
  D3D11_MAP *mapped;           /* by subresource: how it is mapped now (0: not) */
  IDXGIResource1 dxgi_res;     /* the same object, for DXGI (dxgi_resource.inc) */
  IDXGISurface2 dxgi_surf;
};

typedef struct View {          /* a render target, depth-stencil or shader resource view: the texture, and its descriptor for the first two */
  Child c;
  Tex *tex;
  UINT slot;
  D3D12_CPU_DESCRIPTOR_HANDLE handle;
  union { D3D11_RENDER_TARGET_VIEW_DESC rtv; D3D11_DEPTH_STENCIL_VIEW_DESC dsv; D3D11_SHADER_RESOURCE_VIEW_DESC srv; } desc;
} View;

typedef struct Shader {
  Child c;
  BYTE *code;
  SIZE_T bytes;
  UINT cb_registers[CONSTANT_SLOTS];      /* the size each constant buffer is declared with, in 16-byte registers (0: not used) */
  UINT registers;                         /* ... and all of them together */
  BYTE tex_kind[TEXTURE_SLOTS];           /* what each t<n> is declared as (Tex.kind; 0xFF: not declared) */
} Shader;

typedef struct Layout {
  Child c;
  UINT count;
  D3D12_INPUT_ELEMENT_DESC elements[16];  /* as the pipeline gets them (the formats and offsets after the conversion) */
  char names[16][32];
  UINT source_offset[16];
  DXGI_FORMAT source_format[16];
  UINT slot_converted[VERTEX_SLOTS];      /* the stride of the converted buffer of a slot, 0: used as it is */
} Layout;

typedef struct Blend { Child c; D3D11_BLEND_DESC desc; } Blend;
typedef struct Depth { Child c; D3D11_DEPTH_STENCIL_DESC desc; } Depth;
typedef struct Raster { Child c; D3D11_RASTERIZER_DESC desc; } Raster;
typedef struct Sampler { Child c; D3D11_SAMPLER_DESC desc; } Sampler;
typedef struct Query { Child c; D3D11_QUERY_DESC desc; BOOL begun, ended; UINT64 start, stop; } Query;      /* stop/start: QueryPerformanceCounter */

typedef struct Pipeline {
  UINT vs, ps, layout;
  UINT gs, hs, ds;
  D3D11_BLEND_DESC blend;
  D3D11_DEPTH_STENCIL_DESC depth;
  D3D11_RASTERIZER_DESC raster;
  UINT targets, has_depth;
  UINT samples;                       /* the targets' sample count */
  DXGI_FORMAT formats[8], depth_format;      /* (a pipeline is made for its targets' formats) */
  ID3D12PipelineState *state;
  UINT last_used;
} Pipeline;

/* The immediate context: what is bound. Every pointer is a reference. */
typedef struct Ctx {
  DxObj o;
  Dev *dev;
  Shader *vs, *ps;
  Layout *layout;
  Buf *vb[VERTEX_SLOTS];
  UINT vb_stride[VERTEX_SLOTS], vb_offset[VERTEX_SLOTS];
  Buf *ib;
  DXGI_FORMAT ib_format;
  UINT ib_offset;
  D3D11_PRIMITIVE_TOPOLOGY topology;
  Buf *vs_cb[CONSTANT_SLOTS], *ps_cb[CONSTANT_SLOTS];
  View *srv[TEXTURE_SLOTS];
  Sampler *sampler[TEXTURE_SLOTS];
  /* The geometry, hull and domain stages (STAGE_GS ...), and the textures and samplers of the stages but the pixel shader's (STAGE_VS ..., which share the table:
   * a slot the pixel shader has nothing bound to takes the first of the others that has). */
  Shader *gs, *hs, *ds;
  Buf *stage_cb[3][CONSTANT_SLOTS];
  View *stage_srv[4][TEXTURE_SLOTS];
  Sampler *stage_sampler[4][TEXTURE_SLOTS];
  View *rtv[8];
  UINT rtv_count;
  View *dsv;
  Blend *blend;
  FLOAT blend_factor[4];
  UINT sample_mask;
  Depth *depth;
  UINT stencil_ref;
  Raster *raster;
  D3D11_VIEWPORT viewport;
  int viewport_set;
  D3D11_RECT scissor;
  int scissor_set;
} Ctx;

/* (the stages in Ctx.stage_cb: geometry, hull, domain; in Ctx.stage_srv and stage_sampler the vertex shader's first) */
#define STAGE_GS 0u
#define STAGE_HS 1u
#define STAGE_DS 2u
#define SRV_VS 0u
#define SRV_GS 1u
#define SRV_HS 2u
#define SRV_DS 3u

typedef struct PendingClear { View *dsv; UINT flags; FLOAT depth; UINT8 stencil; } PendingClear;

struct Dev {
  DxObj o;
  IDXGIDevice1 dxgi;                       /* the same object, for dxgi's interfaces */
  ITcosPresenter presenter;                /* ... and for dxgi.dll's swap chains */
  CRITICAL_SECTION lock;
  IDXGIAdapter *adapter;
  IDXGIFactory1 *factory;                  /* the factory the adapter was taken from, when the program gave none */
  D3D_FEATURE_LEVEL level;
  UINT flags;
  ID3D12Device *d12;
  ID3D12CommandQueue *queue;
  ID3D12CommandAllocator *allocator;
  ID3D12GraphicsCommandList *list;
  ID3D12RootSignature *root;
  ID3D12DescriptorHeap *rtv_heap, *dsv_heap, *view_heap, *sampler_heap;
  UINT rtv_free[RTV_SLOTS], rtv_free_count, dsv_free[DSV_SLOTS], dsv_free_count;
  ID3D12Resource *ring;                    /* the constant blocks of the list's draws */
  Spare retired[RETIRED_MAX];              /* upload buffers the list being recorded still reads (a renamed buffer's old memory): kept until it has run */
  UINT retired_count;
  UINT64 retired_bytes;
  Spare spares[SPARES_MAX];                /* upload buffers of lists that have run, not mapped (d3d12.dll looks at every mapped one at each submission) */
  UINT spare_count;
  UINT64 spare_bytes;
  BYTE *ring_data;
  UINT ring_used, ring_last, ring_last_bytes;
  UINT view_used, sampler_used;            /* descriptors of the two heaps the list's draws have taken */
  UINT64 table_views, table_samplers;      /* the GPU handles of the tables the last draw used ... */
  UINT table_view_ids[TEXTURE_SLOTS], table_sampler_ids[TEXTURE_SLOTS];      /* ... and what was in them */
  int tables_valid;
  Tex *blank;                              /* a white pixel: what a texture slot with nothing bound samples */
  Tex *blank_kind[4];                      /* ... a white volume, array and cube (by Tex.kind, made when a shader that declares one has nothing bound to it) */
  UINT epoch;                              /* counts the command lists */
  int recorded;                            /* the list has something in it */
  UINT dirty;                              /* what the next draw has to set again (DIRTY_*) */
  Pipeline pipelines[PIPELINES];
  UINT pipeline_count, pipeline_clock;
  PendingClear clears[8];                  /* depth clears a draw has not followed yet (Direct3D 12 here does them when the next pass begins) */
  UINT clear_count;
  Ctx *ctx;
};
#define DIRTY_TARGETS 1u
#define DIRTY_VIEWPORT 2u
#define DIRTY_INPUT 4u
#define DIRTY_PIPELINE 8u
#define DIRTY_ROOT 16u
#define DIRTY_ALL 0xFFu

static Dev *dev_of_dxgi(IDXGIDevice1 *p) { return (Dev *)((BYTE *)p - offsetof(Dev, dxgi)); }
static Dev *dev_of_presenter(ITcosPresenter *p) { return (Dev *)((BYTE *)p - offsetof(Dev, presenter)); }

#define obj_QueryInterface dx_QueryInterface
#define obj_AddRef dx_AddRef
#define obj_Release dx_Release
#define obj_SetPrivateData dx_SetPrivateData
#define obj_SetPrivateDataInterface dx_SetPrivateDataInterface
#define obj_GetPrivateData dx_GetPrivateData

static void *child_new(SIZE_T size, Dev *dev, const void *vtbl, const GUID *const *iids, void (*destroy)(DxObj *)) {
  Child *c = mem_alloc(size);
  if (!c) return NULL;
  dx_init(&c->o, vtbl, iids, destroy);
  c->dev = dev;
  c->id = (UINT)InterlockedIncrement(&g_next_id);
  dx_AddRef(OBJ(dev));
  return c;
}
static void child_destroy(DxObj *o) { dx_Release(OBJ(((Child *)o)->dev)); }
static void STDMETHODCALLTYPE child_GetDevice(Child *c, ID3D11Device **out) {
  if (!out) return;
  dx_AddRef(OBJ(c->dev));
  *out = (ID3D11Device *)c->dev;
}
/* (the creation's own reference goes: the output has its own, or there is none to give) */
static HRESULT give(void *object, void **out) {
  if (!out) { dx_Release(OBJ(object)); return S_FALSE; }
  *out = object;
  return S_OK;
}
/* Puts `object` (or NULL) where `*slot` is, with the references moved. */
static void bind(void *slot_address, void *object) {
  DxObj **slot = slot_address;
  if (*slot == object) return;
  if (object) dx_AddRef(OBJ(object));
  DxObj *old = *slot;
  *slot = object;
  if (old) dx_Release(old);
}

extern const ID3D11DeviceVtbl dev_vtbl;
extern const ID3D11DeviceContextVtbl ctx_vtbl;
extern const ID3D11BufferVtbl buf_vtbl;
extern const ID3D11Texture2DVtbl tex_vtbl;
extern const ID3D11RenderTargetViewVtbl rtv_vtbl;
extern const ID3D11DepthStencilViewVtbl dsv_vtbl;
extern const ID3D11ShaderResourceViewVtbl srv_vtbl;
extern const ID3D11VertexShaderVtbl vs_vtbl;
extern const ID3D11PixelShaderVtbl ps_vtbl;
extern const ID3D11GeometryShaderVtbl gs_vtbl;
extern const ID3D11HullShaderVtbl hs_vtbl;
extern const ID3D11DomainShaderVtbl ds_vtbl;
extern const ID3D11InputLayoutVtbl layout_vtbl;
extern const ID3D11BlendStateVtbl blend_vtbl;
extern const ID3D11DepthStencilStateVtbl depth_vtbl;
extern const ID3D11RasterizerStateVtbl raster_vtbl;
extern const ID3D11SamplerStateVtbl sampler_vtbl;
extern const ID3D11QueryVtbl query_vtbl;

static const GUID *const g_dev_iids[] = {&IID_ID3D11Device, NULL};
static const GUID *const g_ctx_iids[] = {&IID_ID3D11DeviceChild, &IID_ID3D11DeviceContext, NULL};
static const GUID *const g_buf_iids[] = {&IID_ID3D11DeviceChild, &IID_ID3D11Resource, &IID_ID3D11Buffer, NULL};
static const GUID *const g_tex_iids[] = {&IID_ID3D11DeviceChild, &IID_ID3D11Resource, &IID_ID3D11Texture2D, NULL};
static const GUID *const g_tex3_iids[] = {&IID_ID3D11DeviceChild, &IID_ID3D11Resource, &IID_ID3D11Texture3D, NULL};
static const GUID *const g_rtv_iids[] = {&IID_ID3D11DeviceChild, &IID_ID3D11View, &IID_ID3D11RenderTargetView, NULL};
static const GUID *const g_dsv_iids[] = {&IID_ID3D11DeviceChild, &IID_ID3D11View, &IID_ID3D11DepthStencilView, NULL};
static const GUID *const g_srv_iids[] = {&IID_ID3D11DeviceChild, &IID_ID3D11View, &IID_ID3D11ShaderResourceView, NULL};
static const GUID *const g_vs_iids[] = {&IID_ID3D11DeviceChild, &IID_ID3D11VertexShader, NULL};
static const GUID *const g_ps_iids[] = {&IID_ID3D11DeviceChild, &IID_ID3D11PixelShader, NULL};
static const GUID *const g_gs_iids[] = {&IID_ID3D11DeviceChild, &IID_ID3D11GeometryShader, NULL};
static const GUID *const g_hs_iids[] = {&IID_ID3D11DeviceChild, &IID_ID3D11HullShader, NULL};
static const GUID *const g_ds_iids[] = {&IID_ID3D11DeviceChild, &IID_ID3D11DomainShader, NULL};
static const GUID *const g_layout_iids[] = {&IID_ID3D11DeviceChild, &IID_ID3D11InputLayout, NULL};
static const GUID *const g_blend_iids[] = {&IID_ID3D11DeviceChild, &IID_ID3D11BlendState, NULL};
static const GUID *const g_depth_iids[] = {&IID_ID3D11DeviceChild, &IID_ID3D11DepthStencilState, NULL};
static const GUID *const g_raster_iids[] = {&IID_ID3D11DeviceChild, &IID_ID3D11RasterizerState, NULL};
static const GUID *const g_sampler_iids[] = {&IID_ID3D11DeviceChild, &IID_ID3D11SamplerState, NULL};
static const GUID *const g_query_iids[] = {&IID_ID3D11DeviceChild, &IID_ID3D11Asynchronous, &IID_ID3D11Query, NULL};

/* ---- the command list ------------------------------------------------------------------------------------------------------------------------------- */

static void record_pending_clears(Dev *d);

/* ---- upload buffers that outlive the program's use of them ------------------------------------------------------------------------------------- */

/* Sending the list is a whole trip to the GPU and back, and d3d12.dll sends an upload buffer as it is at that moment (its CPU copy, at ExecuteCommandLists).
 * A program that writes a buffer a recorded draw still reads (Map with WRITE_DISCARD, as ANGLE does for each batch of Skia's rectangles) had to have the list
 * sent first, once for every write: about 300 ms a frame in Chrome. Instead such a buffer gets other memory, and the old is retired: kept, as it is, until
 * the list has run; then it is a spare for the next buffer of its size. */
static void spare_keep(Dev *d, Spare s) {
  if (s.data) { ID3D12Resource_Unmap(s.res, 0, NULL); s.data = NULL; }      /* (d3d12.dll hashes every mapped upload buffer at each submission) */
  if (d->spare_count < SPARES_MAX && d->spare_bytes + s.bytes <= SPARES_MAX_BYTES && s.bytes) { d->spares[d->spare_count++] = s; d->spare_bytes += s.bytes; return; }
  ID3D12Resource_Release(s.res);
}
static void spares_take_retired(Dev *d) {
  for (UINT i = 0; i < d->retired_count; ++i) spare_keep(d, d->retired[i]);
  d->retired_count = 0;
  d->retired_bytes = 0;
}
/* Whether `bytes` more may wait for the list (else the caller sends the list: memory kept back for every draw of a long list left Chrome none for its textures). */
static BOOL retire_room(const Dev *d, UINT64 bytes) { return d->retired_count < RETIRED_MAX && d->retired_bytes + bytes <= RETIRED_MAX_BYTES; }
static void spares_release(Dev *d) {
  spares_take_retired(d);
  for (UINT i = 0; i < d->spare_count; ++i) ID3D12Resource_Release(d->spares[i].res);
  d->spare_count = 0;
  d->spare_bytes = 0;
}
static ID3D12Resource *make_buffer(Dev *d, D3D12_HEAP_TYPE heap, UINT64 bytes);
static void flush(Dev *d);
/* An upload buffer of at least `bytes`, mapped: a spare (at most twice the size) or a new one. NULL if there is none. */
static ID3D12Resource *take_upload(Dev *d, UINT64 bytes, BYTE **data, UINT64 *size) {
  UINT best = d->spare_count;
  for (UINT i = 0; i < d->spare_count; ++i)
    if (d->spares[i].bytes >= bytes && d->spares[i].bytes <= bytes * 2 && (best == d->spare_count || d->spares[i].bytes < d->spares[best].bytes)) best = i;
  ID3D12Resource *res;
  if (best < d->spare_count) {
    res = d->spares[best].res;
    *size = d->spares[best].bytes;
    d->spare_bytes -= *size;
    d->spares[best] = d->spares[--d->spare_count];
  } else {
    res = make_buffer(d, D3D12_HEAP_TYPE_UPLOAD, bytes);
    *size = bytes;
    if (!res) return NULL;
  }
  *data = NULL;
  if (FAILED(ID3D12Resource_Map(res, 0, NULL, (void **)data)) || !*data) { ID3D12Resource_Release(res); return NULL; }
  return res;
}
/* `res` is read by the list being recorded: kept until the list has run (sent first when too much waits). The reference passes. */
static void retire(Dev *d, ID3D12Resource *res, BYTE *data, UINT64 bytes) {
  const Spare s = {res, data, bytes};
  if (d->retired_count == RETIRED_MAX) flush(d);      /* (after which nothing reads it any more) */
  if (d->retired_count == RETIRED_MAX) { spare_keep(d, s); return; }
  d->retired[d->retired_count++] = s;
  d->retired_bytes += bytes;
}

/* Sends what was recorded (the queue runs it before it returns) and begins a new list. */
static void flush(Dev *d) {
  if (!d->recorded) return;
  if (d->ring_used) { void *p = NULL; if (SUCCEEDED(ID3D12Resource_Map(d->ring, 0, NULL, &p))) ID3D12Resource_Unmap(d->ring, 0, NULL); }      /* (the constants go with the list) */
  ID3D12GraphicsCommandList_Close(d->list);
  ID3D12CommandList *lists[1] = {(ID3D12CommandList *)d->list};
  ID3D12CommandQueue_ExecuteCommandLists(d->queue, 1, lists);
  ID3D12CommandAllocator_Reset(d->allocator);
  ID3D12GraphicsCommandList_Reset(d->list, d->allocator, NULL);
  spares_take_retired(d);      /* (the list has run: what it read may be used again) */
  d->recorded = 0;
  d->dirty = DIRTY_ALL;
  d->ring_used = d->ring_last_bytes = 0;
  d->view_used = d->sampler_used = 0;
  d->tables_valid = 0;
  ++d->epoch;
  record_pending_clears(d);
}
/* Before the CPU writes into (or the last reference leaves) something a recorded command uses. */
static void settle(Dev *d, UINT used) { if (used == d->epoch) flush(d); }

static ID3D12Resource *make_buffer(Dev *d, D3D12_HEAP_TYPE heap, UINT64 bytes) {
  D3D12_HEAP_PROPERTIES props = {heap, D3D12_CPU_PAGE_PROPERTY_UNKNOWN, D3D12_MEMORY_POOL_UNKNOWN, 1, 1};
  D3D12_RESOURCE_DESC desc = {D3D12_RESOURCE_DIMENSION_BUFFER, 0, bytes < 16 ? 16 : bytes, 1, 1, 1, DXGI_FORMAT_UNKNOWN, {1, 0}, D3D12_TEXTURE_LAYOUT_ROW_MAJOR, D3D12_RESOURCE_FLAG_NONE};
  ID3D12Resource *res = NULL;
  ID3D12Device_CreateCommittedResource(d->d12, &props, D3D12_HEAP_FLAG_NONE, &desc,
                                       heap == D3D12_HEAP_TYPE_READBACK ? D3D12_RESOURCE_STATE_COPY_DEST : D3D12_RESOURCE_STATE_GENERIC_READ, NULL, &IID_ID3D12Resource,
                                       (void **)&res);
  return res;
}

/* ---- formats ----------------------------------------------------------------------------------------------------------------------------------------- */

/* What a texture of `format` is kept as; FALSE for one this runtime has not. */
static BOOL format_of(DXGI_FORMAT format, UINT bind, DXGI_FORMAT *stored, UINT *bpp, UINT *stored_bpp, int *expand, int *depth) {
  *expand = 0;
  *depth = (bind & D3D11_BIND_DEPTH_STENCIL) != 0;
  *stored = format;
  switch (format) {
    case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: case DXGI_FORMAT_R8G8B8A8_TYPELESS: case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: case DXGI_FORMAT_B8G8R8A8_TYPELESS: case DXGI_FORMAT_B8G8R8X8_UNORM: case DXGI_FORMAT_B8G8R8X8_TYPELESS:
    case DXGI_FORMAT_R10G10B10A2_UNORM:
      *bpp = *stored_bpp = 4;
      return !*depth;
    case DXGI_FORMAT_R16G16B16A16_FLOAT: case DXGI_FORMAT_R16G16B16A16_UNORM:
      *bpp = *stored_bpp = 8;
      return !*depth;
    case DXGI_FORMAT_R32G32B32A32_FLOAT:
      *bpp = *stored_bpp = 16;
      return !*depth;
    /* One- and two-channel formats are kept as RGBA8 (the kernel's images have four channels): sampled they read as Direct3D reads them, and a draw into
     * one writes only its channels (pipeline_get masks the others), so the rest stay 0 and 1. Chrome keeps the planes of video frames in R8 and RG8. */
    case DXGI_FORMAT_R8_UNORM: case DXGI_FORMAT_R8_TYPELESS: case DXGI_FORMAT_A8_UNORM:
      *expand = format == DXGI_FORMAT_A8_UNORM ? 2 : 1;
      *stored = DXGI_FORMAT_R8G8B8A8_UNORM;
      *bpp = 1;
      *stored_bpp = 4;
      return !*depth && !(format == DXGI_FORMAT_A8_UNORM && (bind & D3D11_BIND_RENDER_TARGET));
    case DXGI_FORMAT_R8G8_UNORM: case DXGI_FORMAT_R8G8_TYPELESS:
      *expand = 3;
      *stored = DXGI_FORMAT_R8G8B8A8_UNORM;
      *bpp = 2;
      *stored_bpp = 4;
      return !*depth;
    case DXGI_FORMAT_D32_FLOAT: case DXGI_FORMAT_D24_UNORM_S8_UINT: case DXGI_FORMAT_R32_TYPELESS: case DXGI_FORMAT_R24G8_TYPELESS:
      *bpp = *stored_bpp = 4;
      return *depth;
    /* Block-compressed: kept as they are (the kernel decodes them where the device does not sample them so); sampled only. */
    case DXGI_FORMAT_BC1_TYPELESS: case DXGI_FORMAT_BC1_UNORM: case DXGI_FORMAT_BC1_UNORM_SRGB:
      *bpp = *stored_bpp = 8;
      return !*depth && !(bind & (D3D11_BIND_RENDER_TARGET | D3D11_BIND_UNORDERED_ACCESS));
    case DXGI_FORMAT_BC2_TYPELESS: case DXGI_FORMAT_BC2_UNORM: case DXGI_FORMAT_BC2_UNORM_SRGB: case DXGI_FORMAT_BC3_TYPELESS: case DXGI_FORMAT_BC3_UNORM: case DXGI_FORMAT_BC3_UNORM_SRGB:
      *bpp = *stored_bpp = 16;
      return !*depth && !(bind & (D3D11_BIND_RENDER_TARGET | D3D11_BIND_UNORDERED_ACCESS));
    case DXGI_FORMAT_D16_UNORM: case DXGI_FORMAT_R16_TYPELESS:
      *stored = DXGI_FORMAT_D32_FLOAT;
      *bpp = 2;
      *stored_bpp = 4;
      return *depth;
    default:
      return FALSE;
  }
}

/* ---- buffers ----------------------------------------------------------------------------------------------------------------------------------------- */

static void buf_destroy(DxObj *o) {
  Buf *b = (Buf *)o;
  Dev *d = b->c.dev;
  if (b->converted) { const Spare s = {b->converted, NULL, b->converted_bytes}; if (b->converted_used == d->epoch) retire(d, s.res, NULL, s.bytes); else spare_keep(d, s); }
  if (b->res) { const Spare s = {b->res, b->mapped ? b->data : NULL, b->bytes}; if (b->used == d->epoch) retire(d, s.res, s.data, s.bytes); else spare_keep(d, s); }
  child_destroy(o);
}
static void STDMETHODCALLTYPE buf_GetType(Buf *b, D3D11_RESOURCE_DIMENSION *type) { (void)b; if (type) *type = D3D11_RESOURCE_DIMENSION_BUFFER; }
static void STDMETHODCALLTYPE buf_SetEvictionPriority(Buf *b, UINT priority) { (void)b; (void)priority; }
static UINT STDMETHODCALLTYPE buf_GetEvictionPriority(Buf *b) { (void)b; return 0; }
static void STDMETHODCALLTYPE buf_GetDesc(Buf *b, D3D11_BUFFER_DESC *desc) { if (desc) *desc = b->desc; }

static HRESULT STDMETHODCALLTYPE dev_CreateBuffer(Dev *d, const D3D11_BUFFER_DESC *desc, const D3D11_SUBRESOURCE_DATA *initial, ID3D11Buffer **out) {
  if (out) *out = NULL;
  if (!desc || desc->ByteWidth == 0 || desc->ByteWidth > (256u << 20)) return E_INVALIDARG;
  if (desc->Usage == D3D11_USAGE_IMMUTABLE && (!initial || !initial->pSysMem)) return E_INVALIDARG;
  if ((desc->BindFlags & D3D11_BIND_CONSTANT_BUFFER) && desc->ByteWidth > CONSTANT_BLOCK) return E_INVALIDARG;
  Buf *b = child_new(sizeof *b, d, &buf_vtbl, g_buf_iids, buf_destroy);
  if (!b) return E_OUTOFMEMORY;
  b->desc = *desc;
  b->res = take_upload(d, desc->ByteWidth, &b->data, &b->bytes);
  if (!b->res) { dx_Release(OBJ(b)); return E_OUTOFMEMORY; }
  if (initial && initial->pSysMem) memcpy(b->data, initial->pSysMem, desc->ByteWidth);
  else memset(b->data, 0, desc->ByteWidth);
  ID3D12Resource_Unmap(b->res, 0, NULL);      /* (marks it changed; see Buf.data) */
  b->version = 1;
  return give(b, (void **)out);
}

/* ---- textures ---------------------------------------------------------------------------------------------------------------------------------------- */

static UINT tex_subresources(const Tex *t) { return t->desc.MipLevels * t->desc.ArraySize; }
static UINT mip_size(UINT size, UINT level) { size >>= level; return size ? size : 1; }
/* The subresource of the Direct3D 12 texture that subresource `sub` of the program's is; FALSE for a level the kernel does not keep (see Tex.kmips). */
static BOOL tex_d12_sub(const Tex *t, UINT sub, UINT *out) {
  const UINT level = sub % t->desc.MipLevels, layer = sub / t->desc.MipLevels;
  if (level >= t->kmips) return FALSE;
  *out = level + layer * t->kmips;
  return TRUE;
}
/* The bytes of a row of a level `w` pixels wide as the program has it (a row of blocks of a compressed format), and the rows of a level `h` high. */
static UINT tex_pitch(const Tex *t, UINT w) { return t->block ? (w + 3) / 4 * t->block : w * t->bpp; }
static UINT tex_rows(const Tex *t, UINT h) { return t->block ? (h + 3) / 4 : h; }
/* The slices of a level of a volume (1 for the other kinds). */
static UINT tex_slices(const Tex *t, UINT sub) { return t->kind == 1 ? mip_size(t->slices, sub % t->desc.MipLevels) : 1; }

/* Sends rows of pixels (as the program has them, `pitch` bytes apart) into the rectangle (x, y, w, h) of a subresource. The list is sent with it. */
static HRESULT tex_write(Dev *d, Tex *t, UINT sub, UINT x, UINT y, UINT z, UINT w, UINT h, const BYTE *rows, UINT pitch) {
  if (t->depth || w == 0 || h == 0) return E_INVALIDARG;
  UINT d12_sub;
  if (!tex_d12_sub(t, sub, &d12_sub)) return S_OK;      /* (a level the kernel does not keep) */
  const UINT row_bytes = t->block ? tex_pitch(t, w) : w * t->stored_bpp, row_pitch = (row_bytes + 255u) & ~255u, rows_of = tex_rows(t, h);
  /* The copy goes into the list in order with the draws: those recorded before it still see the old pixels, and a draw that samples the texture later sends
   * the list first (`written`, in draw_prepare). As with a render target, a texture the list has sampled already is not copied into in the same list. */
  settle(d, t->sampled);
  const UINT64 upload_bytes = (UINT64)row_pitch * rows_of;
  if (!retire_room(d, upload_bytes)) flush(d);
  BYTE *memory = NULL;
  UINT64 upload_size = 0;
  ID3D12Resource *upload = take_upload(d, upload_bytes, &memory, &upload_size);
  if (!upload) return E_OUTOFMEMORY;
  for (UINT row = 0; row < rows_of; ++row) {
    const BYTE *in = rows + (SIZE_T)row * pitch;
    BYTE *to = memory + (SIZE_T)row * row_pitch;
    if (!t->expand) memcpy(to, in, row_bytes);
    else for (UINT i = 0; i < w; ++i) {
      const BYTE *p = in + (SIZE_T)i * t->bpp;
      to[4 * i] = t->expand == 2 ? 0 : p[0];
      to[4 * i + 1] = t->expand == 3 ? p[1] : 0;
      to[4 * i + 2] = 0;
      to[4 * i + 3] = t->expand == 2 ? p[0] : 255;
    }
  }
  ID3D12Resource_Unmap(upload, 0, NULL);
  D3D12_TEXTURE_COPY_LOCATION to, from;
  memset(&to, 0, sizeof to);
  memset(&from, 0, sizeof from);
  to.pResource = t->res;
  to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  to.SubresourceIndex = d12_sub;
  from.pResource = upload;
  from.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
  from.PlacedFootprint.Footprint.Format = t->stored;
  from.PlacedFootprint.Footprint.Width = w;
  from.PlacedFootprint.Footprint.Height = h;
  from.PlacedFootprint.Footprint.Depth = 1;
  from.PlacedFootprint.Footprint.RowPitch = row_pitch;
  ID3D12GraphicsCommandList_CopyTextureRegion(d->list, &to, x, y, t->kind == 1 ? z : 0, &from, NULL);
  d->recorded = 1;
  t->used = t->written = d->epoch;
  retire(d, upload, NULL, upload_size);      /* (unmapped above: kept until the list has run, then a spare) */
  return S_OK;
}

/* Fetches a whole subresource (slice `z` of a volume's) as rows of `w * bpp` bytes, as the program has them. */
static HRESULT tex_read(Dev *d, Tex *t, UINT sub, UINT z, BYTE *rows) {
  if (t->depth) return E_INVALIDARG;
  const UINT level = sub % t->desc.MipLevels, w = mip_size(t->desc.Width, level), h = mip_size(t->desc.Height, level);
  UINT d12_sub;
  if (!tex_d12_sub(t, sub, &d12_sub)) { memset(rows, 0, (SIZE_T)tex_pitch(t, w) * tex_rows(t, h)); return S_OK; }      /* (a level the kernel does not keep) */
  const UINT row_pitch = ((t->block ? tex_pitch(t, w) : w * t->stored_bpp) + 255u) & ~255u, rows_of = tex_rows(t, h);
  ID3D12Resource *readback = make_buffer(d, D3D12_HEAP_TYPE_READBACK, (UINT64)row_pitch * rows_of);
  if (!readback) return E_OUTOFMEMORY;
  D3D12_TEXTURE_COPY_LOCATION to, from;
  memset(&to, 0, sizeof to);
  memset(&from, 0, sizeof from);
  to.pResource = readback;
  to.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
  to.PlacedFootprint.Footprint.Format = t->stored;
  to.PlacedFootprint.Footprint.Width = w;
  to.PlacedFootprint.Footprint.Height = h;
  to.PlacedFootprint.Footprint.Depth = 1;
  to.PlacedFootprint.Footprint.RowPitch = row_pitch;
  from.pResource = t->res;
  from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  from.SubresourceIndex = d12_sub;
  const D3D12_BOX slice = {0, 0, z, w, h, z + 1};
  ID3D12GraphicsCommandList_CopyTextureRegion(d->list, &to, 0, 0, 0, &from, t->kind == 1 ? &slice : NULL);
  d->recorded = 1;
  flush(d);
  const BYTE *memory = NULL;
  HRESULT hr = ID3D12Resource_Map(readback, 0, NULL, (void **)&memory);
  if (SUCCEEDED(hr) && memory) {
    for (UINT row = 0; row < rows_of; ++row) {
      const BYTE *in = memory + (SIZE_T)row * row_pitch;
      BYTE *to_row = rows + (SIZE_T)row * tex_pitch(t, w);
      if (!t->expand) memcpy(to_row, in, tex_pitch(t, w));
      else for (UINT i = 0; i < w; ++i) {
        if (t->expand == 3) { to_row[2 * i] = in[4 * i]; to_row[2 * i + 1] = in[4 * i + 1]; }
        else to_row[i] = in[4 * i + (t->expand == 1 ? 0 : 3)];
      }
    }
    ID3D12Resource_Unmap(readback, 0, NULL);
  } else if (SUCCEEDED(hr)) hr = E_FAIL;
  ID3D12Resource_Release(readback);
  return hr;
}

static void tex_destroy(DxObj *o) {
  Tex *t = (Tex *)o;
  settle(t->c.dev, t->used);
  if (t->shadow) for (UINT i = 0; i < tex_subresources(t); ++i) mem_free(t->shadow[i]);
  mem_free(t->shadow);
  mem_free(t->mapped);
  if (t->res) ID3D12Resource_Release(t->res);
  child_destroy(o);
}
static void STDMETHODCALLTYPE tex_GetType(Tex *t, D3D11_RESOURCE_DIMENSION *type) { (void)t; if (type) *type = D3D11_RESOURCE_DIMENSION_TEXTURE2D; }
static void STDMETHODCALLTYPE tex_SetEvictionPriority(Tex *t, UINT priority) { (void)t; (void)priority; }
static UINT STDMETHODCALLTYPE tex_GetEvictionPriority(Tex *t) { (void)t; return 0; }
static void STDMETHODCALLTYPE tex_GetDesc(Tex *t, D3D11_TEXTURE2D_DESC *desc) { if (desc) *desc = t->desc; }
static void STDMETHODCALLTYPE tex3_GetType(Tex *t, D3D11_RESOURCE_DIMENSION *type) { (void)t; if (type) *type = D3D11_RESOURCE_DIMENSION_TEXTURE3D; }
static void STDMETHODCALLTYPE tex3_GetDesc(Tex *t, D3D11_TEXTURE3D_DESC *desc) { if (desc) *desc = t->desc3; }

static void tex_dxgi_init(Tex *t);      /* (dxgi_resource.inc) */

/* A texture this library cannot make, said once for each format (what a program wanted that is missing). */
static void tex_refused(const D3D11_TEXTURE2D_DESC *desc, const char *why) {
  static volatile LONG said[256];
  const UINT f = (UINT)desc->Format < 256 ? (UINT)desc->Format : 255;
  if (InterlockedExchange((LONG *)&said[f], 1)) return;
  char text[240];
  SIZE_T n = 0;
  const char *parts[8] = {"dx: refused: ID3D11Device::CreateTexture2D format ", " bind ", " misc ", " size ", "x", " layers ", " samples ", " levels "};
  const UINT values[8] = {(UINT)desc->Format, desc->BindFlags, desc->MiscFlags, desc->Width, desc->Height, desc->ArraySize, desc->SampleDesc.Count, desc->MipLevels};
  for (UINT i = 0; i < 8; ++i) {
    for (const char *c = parts[i]; *c && n + 1 < sizeof text; ++c) text[n++] = *c;
    char digits[12];
    UINT k = 0, v = values[i];
    do { digits[k++] = (char)('0' + v % 10); v /= 10; } while (v && k < sizeof digits);
    while (k && n + 1 < sizeof text) text[n++] = digits[--k];
  }
  if (n + 3 < sizeof text) { text[n++] = ':'; text[n++] = ' '; }
  for (const char *c = why; *c && n + 1 < sizeof text; ++c) text[n++] = *c;
  text[n] = 0;
  dx_say(text);
}

/* A texture object around `res` (a reference is taken), or around a new Direct3D 12 texture when it is NULL. `slices`: 0 for a 2D texture, array or cube, else the
 * depth of a volume (`in` then has one layer). */
static HRESULT tex_make(Dev *d, const D3D11_TEXTURE2D_DESC *in, UINT slices, ID3D12Resource *res, Tex **out) {
  D3D11_TEXTURE2D_DESC desc = *in;
  *out = NULL;
  if (desc.Width == 0 || desc.Height == 0 || desc.ArraySize == 0) return E_INVALIDARG;
  if (desc.Width > 4096 || desc.Height > 4096 || slices > 256) { tex_refused(&desc, "larger than 4096 (or a volume deeper than 256)"); return E_INVALIDARG; }
  const BOOL cube = (desc.MiscFlags & D3D11_RESOURCE_MISC_TEXTURECUBE) != 0;
  if (cube && (desc.ArraySize % 6 || desc.Width != desc.Height)) return E_INVALIDARG;
  if (cube && desc.ArraySize != 6) { tex_refused(&desc, "an array of cubes"); return DXGI_ERROR_UNSUPPORTED; }
  if (desc.ArraySize > 256) { tex_refused(&desc, "an array of more than 256"); return E_INVALIDARG; }
  const BOOL layered = cube || slices || desc.ArraySize > 1;
  /* (the kernel samples volumes, arrays and cubes; it does not draw into them) */
  if (layered && (desc.BindFlags & (D3D11_BIND_RENDER_TARGET | D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_UNORDERED_ACCESS))) {
    tex_refused(&desc, slices ? "a volume to draw into" : cube ? "a cube to draw into" : "an array to draw into");
    return DXGI_ERROR_UNSUPPORTED;
  }
  const BOOL target = (desc.BindFlags & (D3D11_BIND_RENDER_TARGET | D3D11_BIND_DEPTH_STENCIL)) != 0;
  /* (a multisampled texture: a render target or depth buffer of one level and one layer, drawn into and resolved; the adapter says which counts it has) */
  if (desc.SampleDesc.Count == 0) return E_INVALIDARG;
  if (desc.SampleDesc.Count > 1 && (!target || desc.ArraySize != 1 || desc.CPUAccessFlags || desc.Usage != D3D11_USAGE_DEFAULT)) { tex_refused(&desc, "multisampled"); return E_INVALIDARG; }
  UINT full = 1;
  UINT largest = desc.Width > desc.Height ? desc.Width : desc.Height;
  if (slices > largest) largest = slices;
  for (UINT size = largest; size > 1; size >>= 1) ++full;
  /* (a render target has one level here: the kernel draws into those only) */
  if (target || desc.SampleDesc.Count > 1) desc.MipLevels = 1;
  else if (desc.MipLevels == 0 || desc.MipLevels > full) desc.MipLevels = full;
  Tex *t = child_new(sizeof *t, d, &tex_vtbl, g_tex_iids, tex_destroy);
  if (!t) return E_OUTOFMEMORY;
  tex_dxgi_init(t);
  t->desc = desc;
  t->kind = slices ? 1 : cube ? 3 : desc.ArraySize > 1 ? 2 : 0;
  t->block = (desc.Format >= DXGI_FORMAT_BC1_TYPELESS && desc.Format <= DXGI_FORMAT_BC3_UNORM_SRGB) ? t->bpp : 0;
  t->slices = slices ? slices : 1;
  t->kmips = layered ? 1 : desc.MipLevels;      /* (the kernel's volumes, arrays and cubes have one level: the program's others are dropped) */
  if (!format_of(desc.Format, desc.BindFlags, &t->stored, &t->bpp, &t->stored_bpp, &t->expand, &t->depth)) { tex_refused(&desc, "the format, for that use"); dx_Release(OBJ(t)); return E_INVALIDARG; }
  t->shadow = mem_alloc(sizeof(BYTE *) * tex_subresources(t));
  t->mapped = mem_alloc(sizeof(D3D11_MAP) * tex_subresources(t));
  if (!t->shadow || !t->mapped) { dx_Release(OBJ(t)); return E_OUTOFMEMORY; }
  if (res) {
    ID3D12Resource_AddRef(res);
    t->res = res;
  } else {
    D3D12_HEAP_PROPERTIES props = {D3D12_HEAP_TYPE_DEFAULT, D3D12_CPU_PAGE_PROPERTY_UNKNOWN, D3D12_MEMORY_POOL_UNKNOWN, 1, 1};
    D3D12_RESOURCE_DESC rd = {slices ? D3D12_RESOURCE_DIMENSION_TEXTURE3D : D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, desc.Width, desc.Height,
                              (UINT16)(slices ? slices : desc.ArraySize), (UINT16)t->kmips, t->stored, {desc.SampleDesc.Count, 0}, D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_NONE};
    if (desc.BindFlags & D3D11_BIND_RENDER_TARGET) rd.Flags |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    if (t->depth) rd.Flags |= D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    if (cube) TcosD3D12ResourceOptions(d->d12, TCOS_RESOURCE_CUBE);      /* (Direct3D 12 knows a cube only by its views; the kernel has to know it now) */
    const HRESULT hr = ID3D12Device_CreateCommittedResource(d->d12, &props, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COMMON, NULL, &IID_ID3D12Resource, (void **)&t->res);
    if (FAILED(hr) || !t->res) { tex_refused(&desc, "Direct3D 12 below refused it"); dx_Release(OBJ(t)); return FAILED(hr) ? hr : E_FAIL; }
  }
  *out = t;
  return S_OK;
}

/* The contents a texture is made with: every subresource (each slice of a volume's levels), in the program's order. */
static HRESULT tex_initial(Dev *d, Tex *t, const D3D11_SUBRESOURCE_DATA *initial) {
  HRESULT hr = S_OK;
  for (UINT sub = 0; SUCCEEDED(hr) && sub < tex_subresources(t); ++sub) {
    if (!initial[sub].pSysMem) continue;
    const UINT level = sub % t->desc.MipLevels, w = mip_size(t->desc.Width, level), h = mip_size(t->desc.Height, level);
    const UINT pitch = initial[sub].SysMemPitch ? initial[sub].SysMemPitch : tex_pitch(t, w);
    const UINT slice_pitch = initial[sub].SysMemSlicePitch ? initial[sub].SysMemSlicePitch : pitch * tex_rows(t, h);
    for (UINT z = 0; SUCCEEDED(hr) && z < tex_slices(t, sub); ++z)
      hr = tex_write(d, t, sub, 0, 0, z, w, h, (const BYTE *)initial[sub].pSysMem + (SIZE_T)z * slice_pitch, pitch);
  }
  return hr;
}

static HRESULT STDMETHODCALLTYPE dev_CreateTexture2D(Dev *d, const D3D11_TEXTURE2D_DESC *desc, const D3D11_SUBRESOURCE_DATA *initial, ID3D11Texture2D **out) {
  if (out) *out = NULL;
  if (!desc) return E_INVALIDARG;
  Tex *t = NULL;
  EnterCriticalSection(&d->lock);
  HRESULT hr = tex_make(d, desc, 0, NULL, &t);
  if (SUCCEEDED(hr) && initial && !t->depth) hr = tex_initial(d, t, initial);
  LeaveCriticalSection(&d->lock);
  if (FAILED(hr)) { if (t) dx_Release(OBJ(t)); return hr; }
  return give(t, (void **)out);
}

static HRESULT STDMETHODCALLTYPE dev_CreateTexture3D(Dev *d, const D3D11_TEXTURE3D_DESC *desc, const D3D11_SUBRESOURCE_DATA *initial, ID3D11Texture3D **out) {
  extern const ID3D11Texture3DVtbl tex3_vtbl;
  if (out) *out = NULL;
  if (!desc || desc->Depth == 0) return E_INVALIDARG;
  const D3D11_TEXTURE2D_DESC as2d = {desc->Width, desc->Height, desc->MipLevels, 1, desc->Format, {1, 0}, desc->Usage, desc->BindFlags, desc->CPUAccessFlags,
                                     desc->MiscFlags & ~(UINT)D3D11_RESOURCE_MISC_TEXTURECUBE};
  Tex *t = NULL;
  EnterCriticalSection(&d->lock);
  HRESULT hr = tex_make(d, &as2d, desc->Depth, NULL, &t);
  if (SUCCEEDED(hr)) {
    t->c.o.lpVtbl = (const void *)&tex3_vtbl;
    t->c.o.iids = g_tex3_iids;
    t->desc3 = *desc;
    t->desc3.MipLevels = t->desc.MipLevels;
    if (initial && !t->depth) hr = tex_initial(d, t, initial);
  }
  LeaveCriticalSection(&d->lock);
  if (FAILED(hr)) { if (t) dx_Release(OBJ(t)); return hr; }
  return give(t, (void **)out);
}

/* ---- views --------------------------------------------------------------------------------------------------------------------------------------------- */

static Tex *tex_of_resource(ID3D11Resource *resource) {
  extern const ID3D11Texture3DVtbl tex3_vtbl;
  if (!resource || (((DxObj *)resource)->lpVtbl != (const void *)&tex_vtbl && ((DxObj *)resource)->lpVtbl != (const void *)&tex3_vtbl)) return NULL;
  return (Tex *)resource;
}
static void STDMETHODCALLTYPE view_GetResource(View *v, ID3D11Resource **out) {
  if (!out) return;
  dx_AddRef(OBJ(v->tex));
  *out = (ID3D11Resource *)v->tex;
}
static void rtv_destroy(DxObj *o) {
  View *v = (View *)o;
  Dev *d = v->c.dev;
  settle(d, v->tex->used);
  if (d->rtv_free_count < RTV_SLOTS) d->rtv_free[d->rtv_free_count++] = v->slot;
  dx_Release(OBJ(v->tex));
  child_destroy(o);
}
static void dsv_destroy(DxObj *o) {
  View *v = (View *)o;
  Dev *d = v->c.dev;
  settle(d, v->tex->used);
  for (UINT i = 0; i < d->clear_count;) {      /* (a clear nothing drew after: it goes with the view) */
    if (d->clears[i].dsv == v) d->clears[i] = d->clears[--d->clear_count]; else ++i;
  }
  if (d->dsv_free_count < DSV_SLOTS) d->dsv_free[d->dsv_free_count++] = v->slot;
  dx_Release(OBJ(v->tex));
  child_destroy(o);
}
static void srv_destroy(DxObj *o) {
  View *v = (View *)o;
  settle(v->c.dev, v->tex->used);
  dx_Release(OBJ(v->tex));
  child_destroy(o);
}
static void STDMETHODCALLTYPE rtv_GetDesc(View *v, D3D11_RENDER_TARGET_VIEW_DESC *desc) { if (desc) *desc = v->desc.rtv; }
static void STDMETHODCALLTYPE dsv_GetDesc(View *v, D3D11_DEPTH_STENCIL_VIEW_DESC *desc) { if (desc) *desc = v->desc.dsv; }
static void STDMETHODCALLTYPE srv_GetDesc(View *v, D3D11_SHADER_RESOURCE_VIEW_DESC *desc) { if (desc) *desc = v->desc.srv; }

static HRESULT STDMETHODCALLTYPE dev_CreateRenderTargetView(Dev *d, ID3D11Resource *resource, const D3D11_RENDER_TARGET_VIEW_DESC *desc, ID3D11RenderTargetView **out) {
  if (out) *out = NULL;
  Tex *t = tex_of_resource(resource);
  if (!t || !(t->desc.BindFlags & D3D11_BIND_RENDER_TARGET)) return E_INVALIDARG;
  if (desc && desc->ViewDimension == D3D11_RTV_DIMENSION_TEXTURE2D && desc->Texture2D.MipSlice != 0) return E_INVALIDARG;
  EnterCriticalSection(&d->lock);
  if (d->rtv_free_count == 0) { LeaveCriticalSection(&d->lock); return E_OUTOFMEMORY; }
  View *v = child_new(sizeof *v, d, &rtv_vtbl, g_rtv_iids, rtv_destroy);
  if (!v) { LeaveCriticalSection(&d->lock); return E_OUTOFMEMORY; }
  v->slot = d->rtv_free[--d->rtv_free_count];
  LeaveCriticalSection(&d->lock);
  dx_AddRef(OBJ(t));
  v->tex = t;
  if (desc) v->desc.rtv = *desc;
  else { v->desc.rtv.Format = t->desc.Format; v->desc.rtv.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D; }
  v->handle = ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(d->rtv_heap);
  v->handle.ptr += (SIZE_T)v->slot * ID3D12Device_GetDescriptorHandleIncrementSize(d->d12, D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
  if (t->desc.SampleDesc.Count > 1) {
    v->desc.rtv.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DMS;
    D3D12_RENDER_TARGET_VIEW_DESC ms = {t->stored, D3D12_RTV_DIMENSION_TEXTURE2DMS, {{0}}};
    ID3D12Device_CreateRenderTargetView(d->d12, t->res, &ms, v->handle);
  } else ID3D12Device_CreateRenderTargetView(d->d12, t->res, NULL, v->handle);
  return give(v, (void **)out);
}
static HRESULT STDMETHODCALLTYPE dev_CreateDepthStencilView(Dev *d, ID3D11Resource *resource, const D3D11_DEPTH_STENCIL_VIEW_DESC *desc, ID3D11DepthStencilView **out) {
  if (out) *out = NULL;
  Tex *t = tex_of_resource(resource);
  if (!t || !t->depth) return E_INVALIDARG;
  EnterCriticalSection(&d->lock);
  if (d->dsv_free_count == 0) { LeaveCriticalSection(&d->lock); return E_OUTOFMEMORY; }
  View *v = child_new(sizeof *v, d, &dsv_vtbl, g_dsv_iids, dsv_destroy);
  if (!v) { LeaveCriticalSection(&d->lock); return E_OUTOFMEMORY; }
  v->slot = d->dsv_free[--d->dsv_free_count];
  LeaveCriticalSection(&d->lock);
  dx_AddRef(OBJ(t));
  v->tex = t;
  if (desc) v->desc.dsv = *desc;
  else { v->desc.dsv.Format = t->desc.Format; v->desc.dsv.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D; }
  v->handle = ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(d->dsv_heap);
  v->handle.ptr += (SIZE_T)v->slot * ID3D12Device_GetDescriptorHandleIncrementSize(d->d12, D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
  if (t->desc.SampleDesc.Count > 1) v->desc.dsv.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2DMS;
  ID3D12Device_CreateDepthStencilView(d->d12, t->res, NULL, v->handle);      /* (the view follows the resource: a multisampled one is seen as such) */
  return give(v, (void **)out);
}
static HRESULT STDMETHODCALLTYPE dev_CreateShaderResourceView(Dev *d, ID3D11Resource *resource, const D3D11_SHADER_RESOURCE_VIEW_DESC *desc, ID3D11ShaderResourceView **out) {
  if (out) *out = NULL;
  Tex *t = tex_of_resource(resource);
  if (!t) return resource ? E_NOTIMPL : E_INVALIDARG;      /* (a buffer as a shader resource) */
  if (!(t->desc.BindFlags & D3D11_BIND_SHADER_RESOURCE)) return E_INVALIDARG;      /* (a depth buffer made to be read too has one: ANGLE makes them for its depth renderbuffers) */
  View *v = child_new(sizeof *v, d, &srv_vtbl, g_srv_iids, srv_destroy);
  if (!v) return E_OUTOFMEMORY;
  dx_AddRef(OBJ(t));
  v->tex = t;
  if (desc) v->desc.srv = *desc;
  else {
    /* (the whole texture, as what it is) */
    v->desc.srv.Format = t->desc.Format;
    switch (t->kind) {
      case 1: v->desc.srv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE3D; v->desc.srv.Texture3D.MipLevels = t->desc.MipLevels; break;
      case 2: v->desc.srv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY; v->desc.srv.Texture2DArray.MipLevels = t->desc.MipLevels; v->desc.srv.Texture2DArray.ArraySize = t->desc.ArraySize; break;
      case 3: v->desc.srv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURECUBE; v->desc.srv.TextureCube.MipLevels = t->desc.MipLevels; break;
      default: v->desc.srv.ViewDimension = t->desc.SampleDesc.Count > 1 ? D3D11_SRV_DIMENSION_TEXTURE2DMS : D3D11_SRV_DIMENSION_TEXTURE2D; v->desc.srv.Texture2D.MipLevels = t->desc.MipLevels; break;
    }
  }
  return give(v, (void **)out);
}

/* ---- shaders, layouts and states ------------------------------------------------------------------------------------------------------------------- */

static void shader_destroy(DxObj *o) {
  mem_free(((Shader *)o)->code);
  child_destroy(o);
}

/* The sizes a shader declares its constant buffers with: the dcl_constantbuffer instructions of its SHDR or SHEX part. */
static void shader_constants(Shader *s) {
  const BYTE *b = s->code;
  if (s->bytes < 32 || memcmp(b, "DXBC", 4) != 0) return;
  UINT parts;
  memcpy(&parts, b + 28, 4);
  for (UINT i = 0; i < parts && i < 32 && 32 + 4 * (SIZE_T)i + 4 <= s->bytes; ++i) {
    UINT at, size;
    memcpy(&at, b + 32 + 4 * i, 4);
    if (at > s->bytes || s->bytes - at < 16) continue;
    if (memcmp(b + at, "SHDR", 4) != 0 && memcmp(b + at, "SHEX", 4) != 0) continue;
    memcpy(&size, b + at + 4, 4);
    if (size > s->bytes - at - 8 || size < 8) return;
    const UINT words = size / 4;
    const BYTE *code = b + at + 8;
    for (UINT w = 2; w < words;) {
      UINT token, length;
      memcpy(&token, code + 4 * w, 4);
      const UINT op = token & 0x7FF;
      length = (token >> 24) & 0x7F;
      if (op == 53 /* customdata */) { if (w + 1 >= words) return; memcpy(&length, code + 4 * (w + 1), 4); }
      if (length == 0 || length > words - w) return;
      if (op == 88 /* dcl_resource: the dimension in the token, then the operand and its register */ && length >= 3) {
        UINT operand, slot;
        const UINT dimension = (token >> 11) & 0x1F;
        memcpy(&operand, code + 4 * (w + 1), 4);
        const UINT k = w + ((operand >> 31) ? 3 : 2);
        if (k < w + length && ((operand >> 20) & 3) == 1) {      /* (one immediate index: SM 4 / 5.0) */
          memcpy(&slot, code + 4 * k, 4);
          if (slot < TEXTURE_SLOTS) s->tex_kind[slot] = dimension == 5 ? 1 : dimension == 8 ? 2 : dimension == 6 ? 3 : 0;
        }
      }
      if (op == 89 /* dcl_constantbuffer: the operand, then the slot and the size in registers */ && length >= 4) {
        UINT operand, slot, registers, k = w + 1;
        memcpy(&operand, code + 4 * k, 4);
        k += (operand >> 31) ? 2 : 1;
        if (k + 1 < w + length) {
          memcpy(&slot, code + 4 * k, 4);
          memcpy(&registers, code + 4 * (k + 1), 4);
          if (slot < CONSTANT_SLOTS && registers <= CONSTANT_BLOCK / 16) s->cb_registers[slot] = registers;
        }
      }
      w += length;
    }
    break;
  }
  for (UINT slot = 0; slot < CONSTANT_SLOTS; ++slot) s->registers += s->cb_registers[slot];
}

static HRESULT shader_make(Dev *d, const void *code, SIZE_T bytes, const void *vtbl, const GUID *const *iids, void **out) {
  if (out) *out = NULL;
  if (!code || bytes < 32 || bytes > 60000) return E_INVALIDARG;
  Shader *s = child_new(sizeof *s, d, vtbl, iids, shader_destroy);
  if (!s) return E_OUTOFMEMORY;
  s->code = mem_alloc(bytes);
  if (!s->code) { dx_Release(OBJ(s)); return E_OUTOFMEMORY; }
  memcpy(s->code, code, bytes);
  s->bytes = bytes;
  memset(s->tex_kind, 0xFF, sizeof s->tex_kind);
  shader_constants(s);
  if (s->registers * 16 > CONSTANT_BLOCK) { dx_Release(OBJ(s)); return E_INVALIDARG; }
  return give(s, out);
}
static HRESULT STDMETHODCALLTYPE dev_CreateVertexShader(Dev *d, const void *code, SIZE_T bytes, ID3D11ClassLinkage *linkage, ID3D11VertexShader **out) {
  (void)linkage;
  return shader_make(d, code, bytes, &vs_vtbl, g_vs_iids, (void **)out);
}
static HRESULT STDMETHODCALLTYPE dev_CreatePixelShader(Dev *d, const void *code, SIZE_T bytes, ID3D11ClassLinkage *linkage, ID3D11PixelShader **out) {
  (void)linkage;
  return shader_make(d, code, bytes, &ps_vtbl, g_ps_iids, (void **)out);
}
/* (geometry, hull and domain shaders go into the pipeline as they are: the kernel takes gs_4_0 / gs_5_0, hs_5_0 and ds_5_0; see docs/Direct3DKernelMode.md) */
static HRESULT STDMETHODCALLTYPE dev_CreateGeometryShader(Dev *d, const void *code, SIZE_T bytes, ID3D11ClassLinkage *linkage, ID3D11GeometryShader **out) {
  (void)linkage;
  return shader_make(d, code, bytes, &gs_vtbl, g_gs_iids, (void **)out);
}
static HRESULT STDMETHODCALLTYPE dev_CreateHullShader(Dev *d, const void *code, SIZE_T bytes, ID3D11ClassLinkage *linkage, ID3D11HullShader **out) {
  (void)linkage;
  return shader_make(d, code, bytes, &hs_vtbl, g_hs_iids, (void **)out);
}
static HRESULT STDMETHODCALLTYPE dev_CreateDomainShader(Dev *d, const void *code, SIZE_T bytes, ID3D11ClassLinkage *linkage, ID3D11DomainShader **out) {
  (void)linkage;
  return shader_make(d, code, bytes, &ds_vtbl, g_ds_iids, (void **)out);
}

/* The floats a vertex element is for the pipeline (1 .. 4), and whether it has to be converted to them; 0 for a format that cannot be had. */
static UINT element_floats(DXGI_FORMAT format, int *convert) {
  *convert = 0;
  switch (format) {
    case DXGI_FORMAT_R32_FLOAT: return 1;
    case DXGI_FORMAT_R32G32_FLOAT: return 2;
    case DXGI_FORMAT_R32G32B32_FLOAT: return 3;
    case DXGI_FORMAT_R32G32B32A32_FLOAT: return 4;
    case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_B8G8R8A8_UNORM: *convert = 1; return 4;
    default: return 0;
  }
}
static UINT element_bytes(DXGI_FORMAT format) {
  int convert;
  const UINT floats = element_floats(format, &convert);
  return convert ? 4 : floats * 4;
}
static HRESULT STDMETHODCALLTYPE dev_CreateInputLayout(Dev *d, const D3D11_INPUT_ELEMENT_DESC *elements, UINT count, const void *code, SIZE_T bytes, ID3D11InputLayout **out) {
  (void)code; (void)bytes;
  static const DXGI_FORMAT float_formats[5] = {0, DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R32G32_FLOAT, DXGI_FORMAT_R32G32B32_FLOAT, DXGI_FORMAT_R32G32B32A32_FLOAT};
  if (out) *out = NULL;
  if ((!elements && count) || count > 16) return E_INVALIDARG;
  Layout *l = child_new(sizeof *l, d, &layout_vtbl, g_layout_iids, child_destroy);
  if (!l) return E_OUTOFMEMORY;
  UINT next[VERTEX_SLOTS] = {0}, slot_needs[VERTEX_SLOTS] = {0};
  l->count = count;
  for (UINT i = 0; i < count; ++i) {
    int convert;
    const UINT slot = elements[i].InputSlot;
    if (slot >= VERTEX_SLOTS || !elements[i].SemanticName || !element_floats(elements[i].Format, &convert)) { dx_Release(OBJ(l)); return E_INVALIDARG; }
    SIZE_T n = 0;
    while (elements[i].SemanticName[n] && n + 1 < sizeof l->names[i]) { l->names[i][n] = elements[i].SemanticName[n]; ++n; }
    memcpy(&l->elements[i], &elements[i], sizeof l->elements[i]);      /* (the two structures are the same) */
    l->elements[i].SemanticName = l->names[i];
    l->source_format[i] = elements[i].Format;
    l->source_offset[i] = elements[i].AlignedByteOffset == D3D11_APPEND_ALIGNED_ELEMENT ? (next[slot] + 3u) & ~3u : elements[i].AlignedByteOffset;
    next[slot] = l->source_offset[i] + element_bytes(elements[i].Format);
    l->elements[i].AlignedByteOffset = l->source_offset[i];
    if (convert) slot_needs[slot] = 1;
  }
  /* A slot with an element that is not floats is repacked: its elements one after the other, each as floats. */
  for (UINT slot = 0; slot < VERTEX_SLOTS; ++slot) {
    if (!slot_needs[slot]) continue;
    UINT offset = 0;
    for (UINT i = 0; i < count; ++i) {
      int convert;
      if (l->elements[i].InputSlot != slot) continue;
      const UINT floats = element_floats(l->source_format[i], &convert);
      l->elements[i].Format = float_formats[floats];
      l->elements[i].AlignedByteOffset = offset;
      offset += floats * 4;
    }
    l->slot_converted[slot] = offset;
  }
  return give(l, (void **)out);
}

static void STDMETHODCALLTYPE blend_GetDesc(Blend *s, D3D11_BLEND_DESC *desc) { if (desc) *desc = s->desc; }
static void STDMETHODCALLTYPE depth_GetDesc(Depth *s, D3D11_DEPTH_STENCIL_DESC *desc) { if (desc) *desc = s->desc; }
static void STDMETHODCALLTYPE raster_GetDesc(Raster *s, D3D11_RASTERIZER_DESC *desc) { if (desc) *desc = s->desc; }
static void STDMETHODCALLTYPE sampler_GetDesc(Sampler *s, D3D11_SAMPLER_DESC *desc) { if (desc) *desc = s->desc; }

static HRESULT STDMETHODCALLTYPE dev_CreateBlendState(Dev *d, const D3D11_BLEND_DESC *desc, ID3D11BlendState **out) {
  if (out) *out = NULL;
  if (!desc) return E_INVALIDARG;
  Blend *s = child_new(sizeof *s, d, &blend_vtbl, g_blend_iids, child_destroy);
  if (!s) return E_OUTOFMEMORY;
  s->desc = *desc;
  return give(s, (void **)out);
}
static HRESULT STDMETHODCALLTYPE dev_CreateDepthStencilState(Dev *d, const D3D11_DEPTH_STENCIL_DESC *desc, ID3D11DepthStencilState **out) {
  if (out) *out = NULL;
  if (!desc) return E_INVALIDARG;
  Depth *s = child_new(sizeof *s, d, &depth_vtbl, g_depth_iids, child_destroy);
  if (!s) return E_OUTOFMEMORY;
  s->desc = *desc;
  return give(s, (void **)out);
}
static HRESULT STDMETHODCALLTYPE dev_CreateRasterizerState(Dev *d, const D3D11_RASTERIZER_DESC *desc, ID3D11RasterizerState **out) {
  if (out) *out = NULL;
  if (!desc) return E_INVALIDARG;
  Raster *s = child_new(sizeof *s, d, &raster_vtbl, g_raster_iids, child_destroy);
  if (!s) return E_OUTOFMEMORY;
  s->desc = *desc;
  return give(s, (void **)out);
}
static HRESULT STDMETHODCALLTYPE dev_CreateSamplerState(Dev *d, const D3D11_SAMPLER_DESC *desc, ID3D11SamplerState **out) {
  if (out) *out = NULL;
  if (!desc) return E_INVALIDARG;
  Sampler *s = child_new(sizeof *s, d, &sampler_vtbl, g_sampler_iids, child_destroy);
  if (!s) return E_OUTOFMEMORY;
  s->desc = *desc;
  return give(s, (void **)out);
}

/* ---- queries ---------------------------------------------------------------------------------------------------------------------------------------- */
/* The queue runs each command list before it returns, so a query is answered once the work recorded before its End is sent: an event is then done, a
 * timestamp is the time of the End. Occlusion is not counted: a query says some samples passed (what a renderer may assume when it cannot know). */

static UINT query_data_size(D3D11_QUERY q) {
  switch (q) {
    case D3D11_QUERY_EVENT: return sizeof(BOOL);
    case D3D11_QUERY_OCCLUSION: case D3D11_QUERY_TIMESTAMP: return sizeof(UINT64);
    case D3D11_QUERY_TIMESTAMP_DISJOINT: return sizeof(D3D11_QUERY_DATA_TIMESTAMP_DISJOINT);
    case D3D11_QUERY_OCCLUSION_PREDICATE: return sizeof(BOOL);
    default: return 0;
  }
}
static HRESULT STDMETHODCALLTYPE dev_CreateQuery(Dev *d, const D3D11_QUERY_DESC *desc, ID3D11Query **out) {
  if (out) *out = NULL;
  if (!desc || desc->MiscFlags & ~(UINT)D3D11_QUERY_MISC_PREDICATEHINT) return E_INVALIDARG;
  if (query_data_size(desc->Query) == 0 || desc->Query == D3D11_QUERY_OCCLUSION_PREDICATE) return E_INVALIDARG;      /* (a predicate is made with CreatePredicate) */
  if (!out) return S_FALSE;
  Query *q = child_new(sizeof *q, d, &query_vtbl, g_query_iids, child_destroy);
  if (!q) return E_OUTOFMEMORY;
  q->desc = *desc;
  q->begun = q->ended = FALSE;
  q->start = q->stop = 0;
  return give(q, (void **)out);
}
static void STDMETHODCALLTYPE query_GetDesc(Query *q, D3D11_QUERY_DESC *desc) { if (desc) *desc = q->desc; }
static UINT STDMETHODCALLTYPE query_GetDataSize(Query *q) { return query_data_size(q->desc.Query); }
static UINT64 query_now(void) {
  LARGE_INTEGER t;
  QueryPerformanceCounter(&t);
  return (UINT64)t.QuadPart;
}

#include "context.inc"
#include "device.inc"
#include "dxgi_resource.inc"
#include "vtbl.inc"
