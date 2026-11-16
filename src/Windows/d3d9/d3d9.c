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

/* d3d9.dll for TheoryCraftOS: the fixed-function pipeline of Direct3D 9, as a layer on this system's Direct3D 12 (d3d12.dll; docs/Direct3D9.md).
 *
 * A device is a Direct3D 12 device, one command queue and one command list; drawing records into the list, and the list is sent when something has
 * to be seen (Present, a lock that reads, the end of what a ring of memory holds). What the program hands over is kept in plain memory - vertex and
 * index buffers, the levels of textures - and a draw brings it into the one form the two shaders here take:
 *   vertices    whatever the program's layout (an FVF code or a declaration), each vertex a draw reads is decoded into a position, a colour and two
 *               texture coordinates (48 bytes) and written into a ring of upload memory; indices are copied into another, counted from the first
 *               vertex the draw reads, and a triangle fan becomes a list.
 *   shaders     one vertex shader (the world, view and projection matrices as one, or positions that are pixels already) and one pixel shader (two
 *               texture stages as weights, the alpha test), written in HLSL and compiled ahead (ff_vs.hlsl, ff_ps.hlsl -> shaders.inc).
 *   states      a Direct3D 12 pipeline for each combination of the render states that a pipeline holds (culling, depth, blending, stencil, write
 *               mask), in a cache.
 *   textures    a Direct3D 12 texture of B8G8R8A8 whatever the program's format; a level is sent when it is unlocked.
 *   Present     the back buffer's pixels are read and put into the window (SetDIBitsToDevice), as dxgi.dll does for its swap chains.
 *
 * Not there: vertex and pixel shaders of the program (the caps say version 0: a program takes its fixed-function path), lighting (a vertex has
 * its diffuse colour, or white), fog, texture transforms and generated coordinates, more than two texture stages, the stage operations beyond
 * select, modulate and add, cube and volume textures, compressed formats, multisampling, queries, clip planes, point sprites. */
#define INITGUID
#define COBJMACROS
#define CONST_VTABLE
#define WIDL_C_INLINE_WRAPPERS 1
#define WIN32_LEAN_AND_MEAN
#include "../dxcommon.h"
#include <stddef.h>
#include <d3d9.h>
#include <d3d12.h>
#include "../tcosdx.h"

#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wincompatible-pointer-types"
#pragma GCC diagnostic ignored "-Wcast-function-type"

__asm__(".text\n.globl ___chkstk_ms\n___chkstk_ms:\n ret\n");

#include "shaders.inc"

#define OBJ(x) ((DxObj *)(x))
#define STAGES 2u                      /* the texture stages the pixel shader has */
#define TEXTURE_SLOTS 8u               /* the stages a program may set textures and states of */
#define STREAMS 4u
#define VERTEX_BYTES 48u               /* a decoded vertex: position (4 floats), colour (4), two texture coordinates (2 + 2) */
#define VERTEX_RING (1u << 20)       /* (21845 vertices a draw at most. The rings are kept small: each is there three times - here, in the kernel, on the device) */
#define INDEX_RING (256u << 10)
#define CONSTANT_RING (256u << 10)
#define CONSTANT_BYTES 176u            /* the vertex shader's 6 registers, then the pixel shader's 5 */
#define VERTEX_REGISTERS 6u
#define VIEW_HEAP 4096u
#define RTV_SLOTS 256u
#define DSV_SLOTS 64u
#define PIPELINES 64u
#define RENDER_STATES 256u
#define SAMPLER_STATES 16u
#define STAGE_STATES 34u
#define MAX_LEVELS 13u                 /* of a texture of 4096 pixels */
#define MAX_ELEMENTS 32u

typedef struct Dev Dev;
typedef struct Tex Tex;

/* ---- the objects ----------------------------------------------------------------------------------------------------------------------------------- */

/* Every object of a device: the head, the device (a reference) and a number no other object of the process has. */
typedef struct Child {
  DxObj o;
  Dev *dev;
  UINT id;
} Child;
static LONG g_next_id;

typedef struct Buffer {      /* a vertex or an index buffer: plain memory, read when something is drawn from it */
  Child c;
  UINT length;
  DWORD usage, fvf;
  D3DFORMAT format;          /* an index buffer's */
  D3DPOOL pool;
  BYTE *data;
} Buffer;

/* A texture, and what stands behind a surface that is not a level of a program's texture (the back buffer, a depth buffer, an off-screen surface). */
struct Tex {
  Child c;
  UINT width, height, levels;
  D3DFORMAT format;
  DWORD usage;
  D3DPOOL pool;
  int depth;                     /* a depth buffer */
  int target;                    /* something that is drawn into */
  ID3D12Resource *res;           /* NULL: memory only (a system-memory surface) */
  BYTE *shadow[MAX_LEVELS];      /* the pixels of a level as the program has them, once it was locked */
  DWORD locked[MAX_LEVELS];      /* the flags of the lock, | 0x80000000 while it is locked */
  UINT rtv_slot, dsv_slot;
  D3D12_CPU_DESCRIPTOR_HANDLE view;      /* of a target or a depth buffer */
  UINT used;                     /* the command list that has a command using it */
  UINT sampled;                  /* ... a draw sampling it */
};

typedef struct Surface {
  Child c;
  Tex *tex;                      /* a reference */
  UINT level;
  int standalone;                /* not a level of a texture a program made */
} Surface;

/* What a vertex declaration (or an FVF code) says of the four things the shaders take: where each is, and as what; type 255: not there. */
typedef struct Attribute { BYTE stream, type; WORD offset; } Attribute;
typedef struct Layout {
  Attribute position, colour, texture[2];
  int transformed;               /* the positions are pixels (XYZRHW) */
} Layout;
typedef struct Decl {
  Child c;
  D3DVERTEXELEMENT9 elements[MAX_ELEMENTS + 1];
  UINT count;                    /* with the end marker */
  Layout layout;
} Decl;

typedef struct Stream { Buffer *buffer; UINT offset, stride; } Stream;

/* Everything a program sets on the device that a state block records. The pointers are references. */
typedef struct State {
  DWORD rs[RENDER_STATES];
  DWORD sampler[TEXTURE_SLOTS][SAMPLER_STATES];
  DWORD stage[TEXTURE_SLOTS][STAGE_STATES];
  D3DMATRIX world, view, projection, texture[TEXTURE_SLOTS];
  D3DVIEWPORT9 viewport;
  RECT scissor;
  D3DMATERIAL9 material;
  DWORD fvf;
  Decl *decl;
  Stream stream[STREAMS];
  Buffer *indices;
  Tex *tex[TEXTURE_SLOTS];
} State;

typedef struct StateBlock { Child c; State state; } StateBlock;

typedef struct Pipeline {
  DWORD cull, z, zwrite, zfunc, blend, src, dst, op, separate, src_alpha, dst_alpha, op_alpha, write, stencil, sfail, szfail, spass, sfunc, smask, swrite;
  ID3D12PipelineState *state;
  UINT last_used;
} Pipeline;

typedef struct PendingClear { Tex *depth; UINT flags; FLOAT z; UINT8 stencil; } PendingClear;

typedef struct Direct3D { DxObj o; } Direct3D;

struct Dev {
  DxObj o;
  Direct3D *parent;
  CRITICAL_SECTION lock;
  D3DDEVICE_CREATION_PARAMETERS creation;
  D3DPRESENT_PARAMETERS present;
  ID3D12Device *d12;
  ID3D12CommandQueue *queue;
  ID3D12CommandAllocator *allocator;
  ID3D12GraphicsCommandList *list;
  ID3D12RootSignature *root;
  ID3D12DescriptorHeap *rtv_heap, *dsv_heap, *view_heap, *sampler_heap;
  UINT rtv_free[RTV_SLOTS], rtv_free_count, dsv_free[DSV_SLOTS], dsv_free_count;
  ID3D12Resource *vertex_ring, *index_ring, *constant_ring;
  BYTE *vertex_data, *index_data, *constant_data;
  UINT vertex_used, index_used, constant_used, constant_last;
  int constant_valid;
  UINT view_used, sampler_used;
  UINT64 table_views, table_samplers;
  UINT table_ids[STAGES];
  DWORD table_sampler_states[STAGES][4];
  int tables_valid;
  Tex *blank;                    /* a white pixel: what a stage with no texture samples */
  UINT epoch;
  int recorded;
  int targets_dirty;             /* the next draw sets the render target, the viewport and the scissor rectangle again */
  Pipeline pipelines[PIPELINES];
  UINT pipeline_count, pipeline_clock;
  PendingClear clears[4];
  UINT clear_count;
  Tex *back, *auto_depth;        /* the back buffer, and the depth buffer made with the device (or NULL) */
  Surface *back_surface, *depth_surface;
  Surface *target, *depth;       /* what is drawn into now (references) */
  State state;
  Layout fvf_layout;
  int in_scene;
  StateBlock *recording;         /* BeginStateBlock .. EndStateBlock (not recorded apart: the block is of everything) */
};

#define obj_QueryInterface dx_QueryInterface
#define obj_AddRef dx_AddRef
#define obj_Release dx_Release

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
static HRESULT STDMETHODCALLTYPE child_GetDevice(Child *c, IDirect3DDevice9 **out) {
  if (!out) return D3DERR_INVALIDCALL;
  dx_AddRef(OBJ(c->dev));
  *out = (IDirect3DDevice9 *)c->dev;
  return D3D_OK;
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

extern const IDirect3D9Vtbl d3d_vtbl;
extern const IDirect3DDevice9Vtbl dev_vtbl;
extern const IDirect3DVertexBuffer9Vtbl vb_vtbl;
extern const IDirect3DIndexBuffer9Vtbl ib_vtbl;
extern const IDirect3DTexture9Vtbl tex_vtbl;
extern const IDirect3DSurface9Vtbl surf_vtbl;
extern const IDirect3DVertexDeclaration9Vtbl decl_vtbl;
extern const IDirect3DStateBlock9Vtbl sb_vtbl;

static const GUID *const g_d3d_iids[] = {&IID_IDirect3D9, NULL};
static const GUID *const g_dev_iids[] = {&IID_IDirect3DDevice9, NULL};
static const GUID *const g_vb_iids[] = {&IID_IDirect3DResource9, &IID_IDirect3DVertexBuffer9, NULL};
static const GUID *const g_ib_iids[] = {&IID_IDirect3DResource9, &IID_IDirect3DIndexBuffer9, NULL};
static const GUID *const g_tex_iids[] = {&IID_IDirect3DResource9, &IID_IDirect3DBaseTexture9, &IID_IDirect3DTexture9, NULL};
static const GUID *const g_surf_iids[] = {&IID_IDirect3DResource9, &IID_IDirect3DSurface9, NULL};
static const GUID *const g_decl_iids[] = {&IID_IDirect3DVertexDeclaration9, NULL};
static const GUID *const g_sb_iids[] = {&IID_IDirect3DStateBlock9, NULL};

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
  d->targets_dirty = 1;
  d->vertex_used = d->index_used = d->constant_used = 0;
  d->constant_valid = 0;
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

#include "resources.inc"
#include "state.inc"
#include "draw.inc"
#include "device.inc"
#include "vtbl.inc"
