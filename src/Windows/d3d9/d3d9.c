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

/* d3d9.dll for TheoryCraftOS: Direct3D 9 - its fixed-function pipeline and a program's own vertex and pixel shaders - as a layer on this system's
 * Direct3D 12 (d3d12.dll; docs/Direct3D9.md).
 *
 * A device is a Direct3D 12 device, one command queue and one command list; drawing records into the list, and the list is sent when something has
 * to be seen (Present, a lock that reads, the end of what a ring of memory holds). What the program hands over is kept in plain memory - vertex and
 * index buffers, the levels of textures - and a draw brings it into the one form the two shaders here take:
 *   vertices    whatever the program's layout (an FVF code or a declaration), each vertex a draw reads is decoded into a position, a colour (lit
 *               here, where there is lighting), a fog factor and specular colour and the texture coordinates of the stages (made here: the
 *               texture matrices, generated coordinates) and written into a ring of upload memory; indices are copied into another, counted from
 *               the first vertex the draw reads, and a triangle fan becomes a list.
 *   shaders     one vertex shader (the world, view and projection matrices as one, or positions that are pixels already; fog by the vertex),
 *               written in HLSL and compiled ahead (ff_vs.hlsl -> shaders.inc), and for each set-up of the texture stages a draw comes with a
 *               pixel shader 2.0 written out when it is first met (ff_stages.h: eight stages, every operation but the bump environment ones)
 *               and translated like a program's own.
 *   a program's a vertex or pixel shader of the program's is translated to Shader Model 4 when it is made (sm_to_dxbc.h). A draw with a vertex
 *   shaders     shader decodes the vertices into one register of four floats for each input the shader declares; the constants the program set
 *               are the draw's block (the vertex shader's registers, then the pixel shader's); either stage may be the fixed-function one.
 *               A translated pixel shader ends with the alpha test and fog, which a draw steers by registers after the shader's constants.
 *   states      a Direct3D 12 pipeline for each combination of the render states that a pipeline holds (culling, depth, blending, stencil, write
 *               mask), in a cache.
 *   textures    a Direct3D 12 texture of B8G8R8A8 whatever the program's format; a level is sent when it is unlocked.
 *   Present     the back buffer's pixels are read and put into the window (SetDIBitsToDevice), as dxgi.dll does for its swap chains.
 *
 * Not there: what the translator refuses of a program's shaders (loops, cube and volume samplers, more: docs/Direct3D9.md), bump environment
 * mapping, cube and volume textures, compressed formats, multisampling, queries, clip planes, point sprites. */
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
#include <stdint.h>
#include "sm_to_dxbc.h"
#include "ff_lighting.h"
#include "ff_stages.h"

#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wincompatible-pointer-types"
#pragma GCC diagnostic ignored "-Wcast-function-type"

__asm__(".text\n.globl ___chkstk_ms\n___chkstk_ms:\n ret\n");

#include "shaders.inc"

#define OBJ(x) ((DxObj *)(x))
#define STAGES FFS_STAGES              /* the texture stages of the fixed-function pipeline */
#define TEXTURE_SLOTS 16u              /* the stages a program may set textures and states of: the samplers of a pixel shader */
#define ROOTS 4u                       /* root signatures: for 2, 4, 8 and 16 textures */
#define STREAMS 4u
#define VERTEX_BYTES(sets) (48u + 16u * (sets))      /* a decoded vertex: position (4 floats), colour (4), the fog factor it brings and its specular colour (1 + 3), and so many sets of texture coordinates (4 each) */
#define TEXTURE_SETS 8u                /* the sets of texture coordinates a vertex can have */
#define LIGHTS 32u                     /* the lights a program can set, by their numbers; FFL_LIGHTS of them shine at once */
#define VERTEX_RING (1u << 20)       /* (21845 vertices a draw at most. The rings are kept small: each is there three times - here, in the kernel, on the device) */
#define INDEX_RING (256u << 10)
#define CONSTANT_RING (256u << 10)
#define VERTEX_REGISTERS 11u           /* the constants of the fixed-function vertex shader, in registers of four floats */
#define FIXED_SHADERS 48u              /* the pixel shaders of texture stage set-ups that are kept */
#define CONSTANT_REGISTERS 256u        /* the block of a draw, at most */
#define VS_FLOATS 256u                 /* the float constants a program sets for its vertex shaders, */
#define PS_FLOATS 224u                 /* ... and for its pixel shaders */
#define SHADER_INPUTS 16u
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
  UINT kind;                     /* the kernel's TextureKind: 0 2D, 1 a volume, 3 a cube (IDirect3DCubeTexture9, IDirect3DVolumeTexture9) */
  UINT slices;                   /* a volume's depth (1 for the others) */
  UINT kmips;                    /* the levels the Direct3D 12 texture has: the kernel keeps one of a volume or a cube, whose other levels are dropped */
  D3DFORMAT format;
  DWORD usage;
  D3DPOOL pool;
  int depth;                     /* a depth buffer */
  int target;                    /* something that is drawn into */
  ID3D12Resource *res;           /* NULL: memory only (a system-memory surface) */
  /* By subresource, face * levels + level (a cube's six faces; one for the others): */
  BYTE *shadow[MAX_LEVELS * 6];  /* the pixels of a level as the program has them, once it was locked (a volume's: its slices one after the other) */
  DWORD locked[MAX_LEVELS * 6];  /* the flags of the lock, | 0x80000000 while it is locked */
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
  Attribute position, colour, texture[TEXTURE_SETS];
  Attribute specular;            /* the second colour: its alpha is the fog factor the vertex brings */
  Attribute normal;              /* for lighting */
  int transformed;               /* the positions are pixels (XYZRHW) */
} Layout;
typedef struct Decl {
  Child c;
  D3DVERTEXELEMENT9 elements[MAX_ELEMENTS + 1];
  UINT count;                    /* with the end marker */
  Layout layout;
} Decl;

typedef struct Stream { Buffer *buffer; UINT offset, stride; } Stream;

/* A vertex or pixel shader of the program's: its tokens (GetFunction gives them back) and what the translator made of them. */
typedef struct Shader {
  Child c;
  int vertex;
  DWORD *code;
  UINT code_bytes;
  BYTE *dxbc;
  UINT dxbc_bytes;
  UINT floats;                   /* the float constants it reads: c0 .. c<floats - 1> */
  int bools;                     /* it reads bool constants: 16 registers after the floats */
  UINT registers;                /* all it takes of a draw's block */
  UINT major;                    /* its shader model */
  UINT state;                    /* a pixel shader: the first of its state registers (the alpha test and fog: sm_to_dxbc.h) */
  UINT sampler_count;            /* a pixel shader: the highest sampler it reads, and one */
  BYTE sampler_kind[16];         /* ... and what each is declared as (SM_SAMPLER_*: Tex.kind) */
  UINT coordinates;              /* ... and the highest set of texture coordinates */
  SmInput inputs[SHADER_INPUTS]; /* a vertex shader: what each input register is */
  UINT input_count;
} Shader;

/* Everything a program sets on the device that a state block records. The pointers are references. */
typedef struct State {
  DWORD rs[RENDER_STATES];
  DWORD sampler[TEXTURE_SLOTS][SAMPLER_STATES];
  DWORD stage[TEXTURE_SLOTS][STAGE_STATES];
  D3DMATRIX world, view, projection, texture[TEXTURE_SLOTS];
  D3DVIEWPORT9 viewport;
  RECT scissor;
  D3DMATERIAL9 material;
  D3DLIGHT9 light[LIGHTS];
  DWORD light_set, light_on;     /* a bit for each light that was set, and that shines */
  DWORD fvf;
  Decl *decl;
  Stream stream[STREAMS];
  Buffer *indices;
  Tex *tex[TEXTURE_SLOTS];
  Shader *vs, *ps;               /* NULL: the fixed-function stage */
  float vs_float[VS_FLOATS][4], ps_float[PS_FLOATS][4];
  int vs_int[16][4], ps_int[16][4];
  DWORD vs_bool, ps_bool;        /* bit n: b<n> */
} State;

typedef struct StateBlock { Child c; State state; } StateBlock;

typedef struct Pipeline {
  DWORD vs, ps;                  /* the shaders' numbers (Child.id); 0: the fixed-function vertex shader */
  DWORD sets;                    /* ... which has so many sets of texture coordinates in its vertices */
  DWORD cull, z, zwrite, zfunc, blend, src, dst, op, separate, src_alpha, dst_alpha, op_alpha, write, stencil, sfail, szfail, spass, sfunc, smask, swrite;
  DWORD wireframe, bias, slope;   /* D3DRS_FILLMODE, D3DRS_DEPTHBIAS (in the depth buffer's units) and D3DRS_SLOPESCALEDEPTHBIAS (float bits) */
  DWORD twosided, ccw_fail, ccw_zfail, ccw_pass, ccw_func;   /* D3DRS_TWOSIDEDSTENCILMODE and the counter-clockwise faces' operations */
  ID3D12PipelineState *state;
  UINT last_used;
} Pipeline;

/* The pixel shader of a set-up of the texture stages (ff_stages.h), made when a draw first came with it. */
typedef struct FixedShader { FfsKey key; Shader *shader; UINT last_used; } FixedShader;

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
  ID3D12RootSignature *roots[ROOTS];      /* each: the block of constants (b0), a table of textures (t0 ..) and one of their samplers (s0 ..) */
  ID3D12RootSignature *root_set;          /* the one the list has now */
  ID3D12DescriptorHeap *rtv_heap, *dsv_heap, *view_heap, *sampler_heap;
  UINT rtv_free[RTV_SLOTS], rtv_free_count, dsv_free[DSV_SLOTS], dsv_free_count;
  ID3D12Resource *vertex_ring, *index_ring, *constant_ring;
  BYTE *vertex_data, *index_data, *constant_data;
  UINT vertex_used, index_used, constant_used, constant_last, constant_last_bytes;
  int constant_valid;
  UINT view_used, sampler_used;
  UINT64 table_views, table_samplers;
  UINT table_ids[TEXTURE_SLOTS];
  DWORD table_sampler_states[TEXTURE_SLOTS][8];
  UINT table_count;
  int tables_valid;
  Tex *blank;                    /* a white pixel: what a stage with no texture samples */
  Tex *blank_kind[4];            /* ... a white volume and cube (by Tex.kind): what a sampler the pixel shader declares as one samples when nothing of that kind is set */
  UINT epoch;
  int recorded;
  int targets_dirty;             /* the next draw sets the render target, the viewport and the scissor rectangle again */
  Pipeline pipelines[PIPELINES];
  UINT pipeline_count, pipeline_clock;
  FixedShader fixed[FIXED_SHADERS];      /* (the shaders hold no reference of the device) */
  UINT fixed_count, fixed_clock;
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
extern const IDirect3DCubeTexture9Vtbl cube_vtbl;
extern const IDirect3DVolumeTexture9Vtbl vol_vtbl;
extern const IDirect3DSurface9Vtbl surf_vtbl;
extern const IDirect3DVertexDeclaration9Vtbl decl_vtbl;
extern const IDirect3DStateBlock9Vtbl sb_vtbl;
extern const IDirect3DVertexShader9Vtbl vs_vtbl;
extern const IDirect3DPixelShader9Vtbl ps_vtbl;

static const GUID *const g_d3d_iids[] = {&IID_IDirect3D9, NULL};
static const GUID *const g_dev_iids[] = {&IID_IDirect3DDevice9, NULL};
static const GUID *const g_vb_iids[] = {&IID_IDirect3DResource9, &IID_IDirect3DVertexBuffer9, NULL};
static const GUID *const g_ib_iids[] = {&IID_IDirect3DResource9, &IID_IDirect3DIndexBuffer9, NULL};
static const GUID *const g_tex_iids[] = {&IID_IDirect3DResource9, &IID_IDirect3DBaseTexture9, &IID_IDirect3DTexture9, NULL};
static const GUID *const g_cube_iids[] = {&IID_IDirect3DResource9, &IID_IDirect3DBaseTexture9, &IID_IDirect3DCubeTexture9, NULL};
static const GUID *const g_vol_iids[] = {&IID_IDirect3DResource9, &IID_IDirect3DBaseTexture9, &IID_IDirect3DVolumeTexture9, NULL};
static const GUID *const g_surf_iids[] = {&IID_IDirect3DResource9, &IID_IDirect3DSurface9, NULL};
static const GUID *const g_decl_iids[] = {&IID_IDirect3DVertexDeclaration9, NULL};
static const GUID *const g_sb_iids[] = {&IID_IDirect3DStateBlock9, NULL};
static const GUID *const g_vs_iids[] = {&IID_IDirect3DVertexShader9, NULL};
static const GUID *const g_ps_iids[] = {&IID_IDirect3DPixelShader9, NULL};

/* ---- the command list ------------------------------------------------------------------------------------------------------------------------------- */

static void record_pending_clears(Dev *d);

/* Sends what was recorded (the queue runs it before it returns) and begins a new list. */
/* Marks a ring the list wrote, so that d3d12.dll sends it with the list (see make_ring). */
static void ring_touch(ID3D12Resource *ring, UINT used) {
  void *p = NULL;
  if (used && SUCCEEDED(ID3D12Resource_Map(ring, 0, NULL, &p))) ID3D12Resource_Unmap(ring, 0, NULL);
}
static void flush(Dev *d) {
  if (!d->recorded) return;
  ring_touch(d->vertex_ring, d->vertex_used);
  ring_touch(d->index_ring, d->index_used);
  ring_touch(d->constant_ring, d->constant_used);
  ID3D12GraphicsCommandList_Close(d->list);
  ID3D12CommandList *lists[1] = {(ID3D12CommandList *)d->list};
  ID3D12CommandQueue_ExecuteCommandLists(d->queue, 1, lists);
  ID3D12CommandAllocator_Reset(d->allocator);
  ID3D12GraphicsCommandList_Reset(d->list, d->allocator, NULL);
  d->recorded = 0;
  d->targets_dirty = 1;
  d->root_set = NULL;
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
