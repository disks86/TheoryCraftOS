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
#define RTV_SLOTS 1024u
#define DSV_SLOTS 256u
#define PIPELINES 96u
#define VERTEX_SLOTS 8u

typedef struct Dev Dev;
typedef struct Tex Tex;
typedef struct Buf Buf;

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
  BYTE *data;                  /* the buffer's memory, mapped for good */
  UINT used;                   /* the command list (Dev.epoch) that has a draw reading it */
  UINT version;                /* counts the writes: what a converted copy is compared with */
  /* the copy with float colours, for a layout that needs one */
  ID3D12Resource *converted;
  UINT converted_layout, converted_version, converted_stride, converted_offset, converted_vertices, converted_out_stride;
};

struct Tex {
  Child c;
  D3D11_TEXTURE2D_DESC desc;
  DXGI_FORMAT stored;          /* the format of the Direct3D 12 texture */
  UINT bpp, stored_bpp;        /* bytes a pixel: as the program has it, as it is kept */
  int expand;                  /* 0: kept as it is; 1: R8 kept as RGBA; 2: A8 kept as RGBA */
  int depth;                   /* a depth buffer: the CPU does not reach it */
  ID3D12Resource *res;
  UINT used;
  UINT sampled;                /* the command list that has a draw sampling it */
  BYTE **shadow;               /* by subresource: the copy a Map hands out (NULL until one is mapped) */
  D3D11_MAP *mapped;           /* by subresource: how it is mapped now (0: not) */
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
  D3D11_BLEND_DESC blend;
  D3D11_DEPTH_STENCIL_DESC depth;
  D3D11_RASTERIZER_DESC raster;
  UINT targets, has_depth;
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
  BYTE *ring_data;
  UINT ring_used, ring_last, ring_last_bytes;
  UINT view_used, sampler_used;            /* descriptors of the two heaps the list's draws have taken */
  UINT64 table_views, table_samplers;      /* the GPU handles of the tables the last draw used ... */
  UINT table_view_ids[TEXTURE_SLOTS], table_sampler_ids[TEXTURE_SLOTS];      /* ... and what was in them */
  int tables_valid;
  Tex *blank;                              /* a white pixel: what a texture slot with nothing bound samples */
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
static const GUID *const g_rtv_iids[] = {&IID_ID3D11DeviceChild, &IID_ID3D11View, &IID_ID3D11RenderTargetView, NULL};
static const GUID *const g_dsv_iids[] = {&IID_ID3D11DeviceChild, &IID_ID3D11View, &IID_ID3D11DepthStencilView, NULL};
static const GUID *const g_srv_iids[] = {&IID_ID3D11DeviceChild, &IID_ID3D11View, &IID_ID3D11ShaderResourceView, NULL};
static const GUID *const g_vs_iids[] = {&IID_ID3D11DeviceChild, &IID_ID3D11VertexShader, NULL};
static const GUID *const g_ps_iids[] = {&IID_ID3D11DeviceChild, &IID_ID3D11PixelShader, NULL};
static const GUID *const g_layout_iids[] = {&IID_ID3D11DeviceChild, &IID_ID3D11InputLayout, NULL};
static const GUID *const g_blend_iids[] = {&IID_ID3D11DeviceChild, &IID_ID3D11BlendState, NULL};
static const GUID *const g_depth_iids[] = {&IID_ID3D11DeviceChild, &IID_ID3D11DepthStencilState, NULL};
static const GUID *const g_raster_iids[] = {&IID_ID3D11DeviceChild, &IID_ID3D11RasterizerState, NULL};
static const GUID *const g_sampler_iids[] = {&IID_ID3D11DeviceChild, &IID_ID3D11SamplerState, NULL};
static const GUID *const g_query_iids[] = {&IID_ID3D11DeviceChild, &IID_ID3D11Asynchronous, &IID_ID3D11Query, NULL};

/* ---- the command list ------------------------------------------------------------------------------------------------------------------------------- */

static void record_pending_clears(Dev *d);

/* Sends what was recorded (the queue runs it before it returns) and begins a new list. */
static void flush(Dev *d) {
  if (!d->recorded) return;
  ID3D12GraphicsCommandList_Close(d->list);
  ID3D12CommandList *lists[1] = {(ID3D12CommandList *)d->list};
  ID3D12CommandQueue_ExecuteCommandLists(d->queue, 1, lists);
  ID3D12CommandAllocator_Reset(d->allocator);
  ID3D12GraphicsCommandList_Reset(d->list, d->allocator, NULL);
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
    case DXGI_FORMAT_R8_UNORM: case DXGI_FORMAT_A8_UNORM:
      *expand = format == DXGI_FORMAT_R8_UNORM ? 1 : 2;
      *stored = DXGI_FORMAT_R8G8B8A8_UNORM;
      *bpp = 1;
      *stored_bpp = 4;
      return !*depth && !(bind & D3D11_BIND_RENDER_TARGET);
    case DXGI_FORMAT_D32_FLOAT: case DXGI_FORMAT_D24_UNORM_S8_UINT: case DXGI_FORMAT_R32_TYPELESS: case DXGI_FORMAT_R24G8_TYPELESS:
      *bpp = *stored_bpp = 4;
      return *depth;
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
  settle(b->c.dev, b->used);
  if (b->converted) ID3D12Resource_Release(b->converted);
  if (b->res) ID3D12Resource_Release(b->res);
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
  b->res = make_buffer(d, D3D12_HEAP_TYPE_UPLOAD, desc->ByteWidth);
  if (!b->res || FAILED(ID3D12Resource_Map(b->res, 0, NULL, (void **)&b->data)) || !b->data) { dx_Release(OBJ(b)); return E_OUTOFMEMORY; }
  if (initial && initial->pSysMem) memcpy(b->data, initial->pSysMem, desc->ByteWidth);
  else memset(b->data, 0, desc->ByteWidth);
  b->version = 1;
  return give(b, (void **)out);
}

/* ---- textures ---------------------------------------------------------------------------------------------------------------------------------------- */

static UINT tex_subresources(const Tex *t) { return t->desc.MipLevels * t->desc.ArraySize; }
static UINT mip_size(UINT size, UINT level) { size >>= level; return size ? size : 1; }

/* Sends rows of pixels (as the program has them, `pitch` bytes apart) into the rectangle (x, y, w, h) of a subresource. The list is sent with it. */
static HRESULT tex_write(Dev *d, Tex *t, UINT sub, UINT x, UINT y, UINT w, UINT h, const BYTE *rows, UINT pitch) {
  if (t->depth || w == 0 || h == 0) return E_INVALIDARG;
  const UINT row_bytes = w * t->stored_bpp, row_pitch = (row_bytes + 255u) & ~255u;
  ID3D12Resource *upload = make_buffer(d, D3D12_HEAP_TYPE_UPLOAD, (UINT64)row_pitch * h);
  BYTE *memory = NULL;
  if (!upload || FAILED(ID3D12Resource_Map(upload, 0, NULL, (void **)&memory)) || !memory) { if (upload) ID3D12Resource_Release(upload); return E_OUTOFMEMORY; }
  for (UINT row = 0; row < h; ++row) {
    const BYTE *in = rows + (SIZE_T)row * pitch;
    BYTE *to = memory + (SIZE_T)row * row_pitch;
    if (!t->expand) memcpy(to, in, row_bytes);
    else for (UINT i = 0; i < w; ++i) {
      to[4 * i] = t->expand == 1 ? in[i] : 0;
      to[4 * i + 1] = to[4 * i + 2] = 0;
      to[4 * i + 3] = t->expand == 1 ? 255 : in[i];
    }
  }
  ID3D12Resource_Unmap(upload, 0, NULL);
  D3D12_TEXTURE_COPY_LOCATION to, from;
  memset(&to, 0, sizeof to);
  memset(&from, 0, sizeof from);
  to.pResource = t->res;
  to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  to.SubresourceIndex = sub;
  from.pResource = upload;
  from.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
  from.PlacedFootprint.Footprint.Format = t->stored;
  from.PlacedFootprint.Footprint.Width = w;
  from.PlacedFootprint.Footprint.Height = h;
  from.PlacedFootprint.Footprint.Depth = 1;
  from.PlacedFootprint.Footprint.RowPitch = row_pitch;
  ID3D12GraphicsCommandList_CopyTextureRegion(d->list, &to, x, y, 0, &from, NULL);
  d->recorded = 1;
  flush(d);
  ID3D12Resource_Release(upload);
  return S_OK;
}

/* Fetches a whole subresource as rows of `w * bpp` bytes, as the program has them. */
static HRESULT tex_read(Dev *d, Tex *t, UINT sub, BYTE *rows) {
  if (t->depth) return E_INVALIDARG;
  const UINT level = sub % t->desc.MipLevels, w = mip_size(t->desc.Width, level), h = mip_size(t->desc.Height, level);
  const UINT row_pitch = (w * t->stored_bpp + 255u) & ~255u;
  ID3D12Resource *readback = make_buffer(d, D3D12_HEAP_TYPE_READBACK, (UINT64)row_pitch * h);
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
  from.SubresourceIndex = sub;
  ID3D12GraphicsCommandList_CopyTextureRegion(d->list, &to, 0, 0, 0, &from, NULL);
  d->recorded = 1;
  flush(d);
  const BYTE *memory = NULL;
  HRESULT hr = ID3D12Resource_Map(readback, 0, NULL, (void **)&memory);
  if (SUCCEEDED(hr) && memory) {
    for (UINT row = 0; row < h; ++row) {
      const BYTE *in = memory + (SIZE_T)row * row_pitch;
      BYTE *to_row = rows + (SIZE_T)row * w * t->bpp;
      if (!t->expand) memcpy(to_row, in, (SIZE_T)w * t->bpp);
      else for (UINT i = 0; i < w; ++i) to_row[i] = in[4 * i + (t->expand == 1 ? 0 : 3)];
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

/* A texture object around `res` (a reference is taken), or around a new Direct3D 12 texture when it is NULL. */
static HRESULT tex_make(Dev *d, const D3D11_TEXTURE2D_DESC *in, ID3D12Resource *res, Tex **out) {
  D3D11_TEXTURE2D_DESC desc = *in;
  *out = NULL;
  if (desc.Width == 0 || desc.Height == 0 || desc.Width > 4096 || desc.Height > 4096 || desc.ArraySize == 0) return E_INVALIDARG;
  if (desc.SampleDesc.Count > 1) return DXGI_ERROR_UNSUPPORTED;
  if (desc.MiscFlags & D3D11_RESOURCE_MISC_TEXTURECUBE) return DXGI_ERROR_UNSUPPORTED;
  const BOOL target = (desc.BindFlags & (D3D11_BIND_RENDER_TARGET | D3D11_BIND_DEPTH_STENCIL)) != 0;
  UINT full = 1;
  for (UINT size = desc.Width > desc.Height ? desc.Width : desc.Height; size > 1; size >>= 1) ++full;
  /* (a render target has one level here: the kernel draws into those only) */
  if (target || desc.ArraySize > 1) desc.MipLevels = 1;
  else if (desc.MipLevels == 0 || desc.MipLevels > full) desc.MipLevels = full;
  Tex *t = child_new(sizeof *t, d, &tex_vtbl, g_tex_iids, tex_destroy);
  if (!t) return E_OUTOFMEMORY;
  t->desc = desc;
  if (!format_of(desc.Format, desc.BindFlags, &t->stored, &t->bpp, &t->stored_bpp, &t->expand, &t->depth)) { dx_Release(OBJ(t)); return E_INVALIDARG; }
  t->shadow = mem_alloc(sizeof(BYTE *) * tex_subresources(t));
  t->mapped = mem_alloc(sizeof(D3D11_MAP) * tex_subresources(t));
  if (!t->shadow || !t->mapped) { dx_Release(OBJ(t)); return E_OUTOFMEMORY; }
  if (res) {
    ID3D12Resource_AddRef(res);
    t->res = res;
  } else {
    D3D12_HEAP_PROPERTIES props = {D3D12_HEAP_TYPE_DEFAULT, D3D12_CPU_PAGE_PROPERTY_UNKNOWN, D3D12_MEMORY_POOL_UNKNOWN, 1, 1};
    D3D12_RESOURCE_DESC rd = {D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, desc.Width, desc.Height, (UINT16)desc.ArraySize, (UINT16)desc.MipLevels, t->stored, {1, 0},
                              D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_NONE};
    if (desc.BindFlags & D3D11_BIND_RENDER_TARGET) rd.Flags |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    if (t->depth) rd.Flags |= D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    const HRESULT hr = ID3D12Device_CreateCommittedResource(d->d12, &props, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COMMON, NULL, &IID_ID3D12Resource, (void **)&t->res);
    if (FAILED(hr) || !t->res) { dx_Release(OBJ(t)); return FAILED(hr) ? hr : E_FAIL; }
  }
  *out = t;
  return S_OK;
}

static HRESULT STDMETHODCALLTYPE dev_CreateTexture2D(Dev *d, const D3D11_TEXTURE2D_DESC *desc, const D3D11_SUBRESOURCE_DATA *initial, ID3D11Texture2D **out) {
  if (out) *out = NULL;
  if (!desc) return E_INVALIDARG;
  Tex *t = NULL;
  EnterCriticalSection(&d->lock);
  HRESULT hr = tex_make(d, desc, NULL, &t);
  if (SUCCEEDED(hr) && initial && !t->depth) {
    for (UINT sub = 0; SUCCEEDED(hr) && sub < tex_subresources(t); ++sub) {
      if (!initial[sub].pSysMem) continue;
      const UINT level = sub % t->desc.MipLevels, w = mip_size(t->desc.Width, level), h = mip_size(t->desc.Height, level);
      hr = tex_write(d, t, sub, 0, 0, w, h, initial[sub].pSysMem, initial[sub].SysMemPitch ? initial[sub].SysMemPitch : w * t->bpp);
    }
  }
  LeaveCriticalSection(&d->lock);
  if (FAILED(hr)) { if (t) dx_Release(OBJ(t)); return hr; }
  return give(t, (void **)out);
}

/* ---- views --------------------------------------------------------------------------------------------------------------------------------------------- */

static Tex *tex_of_resource(ID3D11Resource *resource) {
  if (!resource || ((DxObj *)resource)->lpVtbl != (const void *)&tex_vtbl) return NULL;
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
  ID3D12Device_CreateRenderTargetView(d->d12, t->res, NULL, v->handle);
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
  ID3D12Device_CreateDepthStencilView(d->d12, t->res, NULL, v->handle);
  return give(v, (void **)out);
}
static HRESULT STDMETHODCALLTYPE dev_CreateShaderResourceView(Dev *d, ID3D11Resource *resource, const D3D11_SHADER_RESOURCE_VIEW_DESC *desc, ID3D11ShaderResourceView **out) {
  if (out) *out = NULL;
  Tex *t = tex_of_resource(resource);
  if (!t) return resource ? E_NOTIMPL : E_INVALIDARG;      /* (a buffer as a shader resource) */
  if (t->depth || !(t->desc.BindFlags & D3D11_BIND_SHADER_RESOURCE)) return E_INVALIDARG;
  View *v = child_new(sizeof *v, d, &srv_vtbl, g_srv_iids, srv_destroy);
  if (!v) return E_OUTOFMEMORY;
  dx_AddRef(OBJ(t));
  v->tex = t;
  if (desc) v->desc.srv = *desc;
  else {
    v->desc.srv.Format = t->desc.Format;
    v->desc.srv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    v->desc.srv.Texture2D.MipLevels = t->desc.MipLevels;
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
#include "vtbl.inc"
