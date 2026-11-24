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
 *   buffers            a Direct3D 12 buffer in an upload heap, mapped for good: Map hands that memory out, a draw uses its GPU address (one the
 *                      GPU writes - unordered access, stream output - in its memory).
 *   constant buffers   Direct3D 12 here has one block of constants for all stages (b0). The block of a draw is put together from the buffers bound to
 *                      the vertex shader's slots, in slot order, and then the pixel shader's (d3d12.dll is told where those begin:
 *                      TcosD3D12PipelineOptions), and goes into a ring of upload memory - so a constant buffer may change between draws freely.
 *   textures           a Direct3D 12 texture in a default heap. The CPU reaches it through copies (UpdateSubresource, Map of a dynamic or staging
 *                      texture: a copy in memory, sent or fetched). Formats the kernel has no image of are kept wider (formats.inc).
 *   vertex layouts     the kernel takes 32-bit floats only: a layout with R8G8B8A8 / B8G8R8A8 colours gets its vertex buffers converted (a second
 *                      buffer, made again when the first changes).
 *   states             a Direct3D 12 pipeline state for each combination of shaders, layout, blend, depth and rasterizer state that is drawn with,
 *                      kept in a cache.
 *   textures, samplers a table of 8 of each for all the stages (t0 .. t7, s0 .. s7; the sampler of t<n> is s<n>, as the kernel pairs them), and one of the
 *                      unordered access views (u0 .. u7).
 *   swap chains        dxgi.dll asks the device for its back buffers and their pixels (ITcosPresenter, src/Windows/tcosdx.h).
 *
 * The rest is in the other parts: compute, unordered access and buffer views, stream output and the draws whose counts are in buffers (context.inc); queries,
 * predication and counters (query.inc); deferred contexts and command lists (deferred.inc); Direct3D 11.1 (context1.inc); render targets of levels and layers
 * and GenerateMips (rtshadow.inc); the texture formats (formats.inc); 11 on 12 and the Windows Runtime's face (interop.inc). What is not there, and why, is
 * in docs/Direct3D11.md. */
#define INITGUID
#define COBJMACROS
#define CONST_VTABLE
#define WIDL_C_INLINE_WRAPPERS 1
#define WIN32_LEAN_AND_MEAN
#include "../dxcommon.h"
#include <stddef.h>
#include <d3d11.h>
#include <d3d11_1.h>
#include <d3d11_4.h>
#include <d3d11on12.h>
#include <d3d10_1.h>
#include <d3d12.h>
#include <d3d12video.h>
#include "../d3d12/video_types.h"
#include <dxva.h>
#include "video_types.h"
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
#define ACTIVE_QUERIES 32u            /* queries begun and not ended at once */
#define MEASURES 2048u                /* the kernel's measurements one list holds (one a draw) */
#define UAV_SLOTS 8u                  /* u0 .. u7: of the compute shader, and of the pixel shader beside its render targets (D3D11_PS_CS_UAV_REGISTER_COUNT) */
#define KIND_BUFFER 0xFEu             /* Shader.tex_kind: t<n> is a raw or structured buffer */

typedef struct Dev Dev;
typedef struct Tex Tex;
typedef struct Buf Buf;
/* A D3D12 upload buffer and where it is mapped (NULL: not mapped). */
typedef struct Spare {
  ID3D12Resource *res;
  BYTE *data;
  UINT64 bytes;
  int uav;                     /* made with D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS: a buffer shaders may read as a shader resource (kept apart from the others) */
} Spare;

/* ---- the objects ----------------------------------------------------------------------------------------------------------------------------------- */

/* Every object of a device: the head, the device (a reference) and a number no other object of the process has (what the pipeline cache knows a
 * state or shader by: an address may be given to another object later). */
typedef struct Child {
  DxObj o;
  Dev *dev;
  UINT id;
  LONG internal;      /* the references of the runtime itself (bindings, views, recorded commands): not counted in what AddRef and Release answer, as in Windows */
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
  /* A buffer the GPU writes (unordered access, stream output) is in the GPU's memory: `data` is NULL, `res` a default-heap buffer the CPU reaches through copies
   * (buf_upload, buf_download); `written` is the list that wrote it last. A buffer shaders read as a shader resource is an upload buffer made for that (`uav`). */
  int gpu;
  int uav;
  UINT written;
  /* a stream output target: where the records written into it end (bytes from its start), and a buffer d3d12.dll puts that into after a list ran (the
   * BufferFilledSizeLocation of the view: the bytes after the view's offset) */
  UINT64 so_end;
  ID3D12Resource *so_fill;
  BYTE *so_fill_data;
};

struct Tex {
  Child c;
  D3D11_TEXTURE2D_DESC desc;   /* (a volume: its width, height and levels, with one layer; its depth is `slices`) */
  UINT kind;                   /* the kernel's TextureKind: 0 2D, 1 3D (a volume), 2 2D array, 3 cube */
  UINT slices;                 /* a volume's depth (1 for the others) */
  UINT kmips;                  /* the levels the Direct3D 12 texture has: the kernel keeps one of a volume, an array or a cube, whose other levels are dropped */
  D3D11_TEXTURE3D_DESC desc3;  /* a volume: as the program made it */
  D3D11_TEXTURE1D_DESC desc1;  /* a one-dimensional texture (`one_d`): as the program made it; kept as a 2D texture (or array) a texel high */
  BOOL one_d;
  DXGI_FORMAT stored;          /* the format of the Direct3D 12 texture */
  UINT bpp, stored_bpp;        /* bytes a pixel: as the program has it, as it is kept */
  const struct TexFormat *fmt; /* how the program's format is kept (formats.inc) */
  /* A render target with levels or layers is a texture the kernel samples, drawn into through a target of its own for each render target view (rtshadow.inc):
   * `rt_views` are those views; `version` counts the writes into the texture that are not by them (what makes a view's copy stale). */
  BOOL shadowed;
  UINT version;
  struct View *rt_views;
  int depth;                   /* a depth buffer: the CPU does not reach it */
  UINT block;                  /* a block-compressed format (BC1 .. BC3): the bytes of a 4 x 4 block (bpp is the same); its rows are rows of blocks */
  ID3D12Resource *res;
  UINT used;
  UINT sampled;                /* the command list that has a draw sampling it */
  UINT written;                /* the command list that draws into it, clears it or copies into it */
  FLOAT min_lod;               /* SetResourceMinLOD: the most detailed level that is sampled */
  BYTE **shadow;               /* by subresource: the copy a Map hands out (NULL until one is mapped) */
  D3D11_MAP *mapped;           /* by subresource: how it is mapped now (0: not) */
  IDXGIResource1 dxgi_res;     /* the same object, for DXGI (dxgi_resource.inc) */
  IDXGISurface2 dxgi_surf;
  IDXGIKeyedMutex dxgi_mutex;  /* ... and its keyed mutex (a texture made D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX, or opened with one: `keyed_mutex`) */
  UINT keyed_mutex;            /* the D3DKMT keyed mutex (0: none) */
  UINT global_share;           /* shared with a legacy handle (made SHARED / SHARED_KEYEDMUTEX without NTHANDLE, or opened by one): what GetSharedHandle answers */
  /* A planar texture (NV12, P010, P016): the bytes of a luma sample (1 or 2; 0 for the other formats). Direct3D 12 below keeps each plane of each slice as a texture
   * of its own (subresource plane * ArraySize + slice); here, as in Direct3D 11, a subresource is a slice with both planes: the CPU sees the luma rows and after them
   * the rows of U, V pairs, at the same pitch (tex_rows counts both). */
  UINT planar;
};

/* A render target, depth-stencil, shader resource or unordered access view: the texture (or buffer), and the descriptor of the first two. A view of a buffer names
 * bytes of it: `offset` and `size`, with `stride` the bytes of an element (4 for a raw view, the structure's for a structured one, the format's for a typed one). */
typedef struct View {
  Child c;
  Tex *tex;
  Buf *buf;
  UINT slot;
  D3D12_CPU_DESCRIPTOR_HANDLE handle;
  UINT64 offset, size;
  UINT stride;
  DXGI_FORMAT format;          /* a typed buffer view's format (DXGI_FORMAT_UNKNOWN for raw and structured ones) */
  UINT counter;                /* an append or counter view's hidden count (D3D11_BUFFER_UAV_FLAG_APPEND / COUNTER) */
  UINT width, height;          /* a render target view: the size of the level it names */
  /* a render target view of a shadowed texture (rtshadow.inc): the target that is drawn into, the buffer the copies go through, the subresource (level, layer
   * or slice) it is of, whether the target holds what the texture does not have yet, and the texture's version it was last loaded from */
  ID3D12Resource *scratch, *link;
  UINT level, layer, link_pitch, loaded_version;
  BOOL newer;
  struct View *next_rt;
  /* a view of a planar texture: the plane it is of (its format: R8 / R8G8 or R16 / R16G16, or their UINT forms), the slice, and how many slices a 2D array view of it has
   * (0: a 2D view) */
  DXGI_FORMAT plane_format;
  UINT plane, array_slice, array_size;
  union { D3D11_RENDER_TARGET_VIEW_DESC rtv; D3D11_DEPTH_STENCIL_VIEW_DESC dsv; D3D11_SHADER_RESOURCE_VIEW_DESC srv; D3D11_UNORDERED_ACCESS_VIEW_DESC uav; } desc;
} View;

typedef struct Shader {
  Child c;
  BYTE *code;
  SIZE_T bytes;
  UINT cb_registers[CONSTANT_SLOTS];      /* the size each constant buffer is declared with, in 16-byte registers (0: not used) */
  UINT registers;                         /* ... and all of them together */
  BYTE tex_kind[TEXTURE_SLOTS];           /* what each t<n> is declared as (Tex.kind, KIND_BUFFER; 0xFF: not declared) */
  UINT uav_mask;                          /* the u<n> it declares */
  UINT group[3];                          /* a compute shader's threads in a group (dcl_thread_group) */
  UINT gs_instances;                      /* a geometry shader's instances (dcl_gs_instance_count; 0: one) */
  UINT program_type;                      /* the shader's stage by its version token (D3D10_SB_TOKENIZED_PROGRAM_TYPE: 0 pixel, 1 vertex, 2 geometry, 3 hull, 4 domain, 5 compute) */
  UINT out_vertices;                      /* the vertices of a primitive a geometry shader puts out (dcl_output_topology: 1, 2 or 3; 0 not declared) */
  /* CreateGeometryShaderWithStreamOutput: what is captured, the strides of the buffers and the stream that is drawn (D3D11_SO_NO_RASTERIZED_STREAM: none). The
   * shader may be a vertex or domain shader's code, which captures what that stage puts out (there is then no geometry stage: `so_only`). */
  D3D11_SO_DECLARATION_ENTRY *so;
  char (*so_names)[64];
  UINT so_count, so_strides[4], so_stride_count, so_rasterized;
  BOOL so_only;
  ID3D12PipelineState *compute;           /* a compute shader: its Direct3D 12 pipeline, made at its first Dispatch */
  UINT used;                              /* ... and the list that last dispatched it */
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

/* The state objects. Direct3D 11 hands out the same object for the same description: a device keeps those it made in a list a kind (Dev.states, `next`), and the
 * description each is found by (`key`, after it is put in its usual form) is what GetDesc gives back. */
typedef struct StateObj { Child c; struct StateObj *next; UINT kind; } StateObj;
typedef struct Blend { StateObj s; D3D11_BLEND_DESC1 desc1; D3D11_BLEND_DESC desc; } Blend;
typedef struct Depth { StateObj s; D3D11_DEPTH_STENCIL_DESC desc; } Depth;
typedef struct Raster { StateObj s; D3D11_RASTERIZER_DESC1 desc1; D3D11_RASTERIZER_DESC desc; } Raster;
typedef struct Sampler { StateObj s; D3D11_SAMPLER_DESC desc; } Sampler;
#define STATE_BLEND 0u
#define STATE_DEPTH 1u
#define STATE_RASTER 2u
#define STATE_SAMPLER 3u
/* A query or predicate (query.inc). What the kernel measures for it (the samples that passed, the pixel shader's invocations, the stream output's primitives) is
 * added up as each list runs; the pipeline statistics this library counts itself are added at each draw. */
typedef struct Query {
  Child c;
  D3D11_QUERY_DESC desc;
  D3D11_CONTEXT_TYPE context_type;            /* CreateQuery1: the kind of work it is for (what GetDesc1 gives back; every context here is one queue, which does all kinds) */
  BOOL begun, ended, active;
  UINT64 start, stop;                         /* QueryPerformanceCounter at Begin and End */
  UINT64 samples;
  D3D11_QUERY_DATA_PIPELINE_STATISTICS stats;
  UINT64 so_written[4], so_needed[4];
  UINT64 so_filled_at_begin[4];               /* the bytes the stream output targets of each stream held at Begin (what was written is the growth) */
} Query;

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
/* A device context: what is bound. The immediate context draws; a deferred one (CreateDeferredContext) records what it is asked to do into a command list
 * (deferred.inc). The bound objects come first, each a reference, from `vs` to `predicate`; then the plain values, from `vb_stride` to `predicate_value`: a
 * deferred context's commands keep a copy of both (StateCopy). */
typedef struct Ctx {
  DxObj o;
  Dev *dev;
  ID3D11Multithread multithread;              /* the same object as ID3D11Multithread (the immediate context's) and ID3DUserDefinedAnnotation (context1.inc) */
  ID3DUserDefinedAnnotation annotation;
  BOOL protected_;                            /* SetMultithreadProtected (the device's lock is taken by every call that needs it, whatever this says) */
  /* ---- the bound objects (references) */
  Shader *vs, *ps;
  Layout *layout;
  Buf *vb[VERTEX_SLOTS];
  Buf *ib;
  Buf *vs_cb[CONSTANT_SLOTS], *ps_cb[CONSTANT_SLOTS];
  View *srv[TEXTURE_SLOTS];
  Sampler *sampler[TEXTURE_SLOTS];
  /* The geometry, hull and domain stages (STAGE_GS ...), and the textures and samplers of the stages but the pixel shader's (STAGE_VS ..., which share the table:
   * a slot the pixel shader has nothing bound to takes the first of the others that has). */
  Shader *gs, *hs, *ds;
  Buf *stage_cb[3][CONSTANT_SLOTS];
  View *stage_srv[4][TEXTURE_SLOTS];
  Sampler *stage_sampler[4][TEXTURE_SLOTS];
  /* the compute stage, and the unordered access views of the pixel shader (OMSetRenderTargetsAndUnorderedAccessViews: u<n> for n from the first after the targets) */
  Shader *cs;
  Buf *cs_cb[CONSTANT_SLOTS];
  View *cs_srv[TEXTURE_SLOTS];
  Sampler *cs_sampler[TEXTURE_SLOTS];
  View *cs_uav[UAV_SLOTS];
  View *ps_uav[UAV_SLOTS];
  View *rtv[8];
  View *dsv;
  Blend *blend;
  Depth *depth;
  Raster *raster;
  struct ClassInst *instances[6][8];  /* the class instances bound with each stage's shader (vs, ps, gs, hs, ds, cs) */
  struct Query *predicate;            /* SetPredication: the predicate, and the value that leaves the commands out */
  /* ---- the plain values */
  UINT vb_stride[VERTEX_SLOTS], vb_offset[VERTEX_SLOTS];
  DXGI_FORMAT ib_format;
  UINT ib_offset;
  D3D11_PRIMITIVE_TOPOLOGY topology;
  UINT rtv_count;
  FLOAT blend_factor[4];
  UINT sample_mask;
  UINT stencil_ref;
  D3D11_VIEWPORT viewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];      /* (the kernel draws with the first) */
  UINT viewport_count;
  D3D11_RECT scissors[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
  UINT scissor_count;
  BOOL predicate_value;
  UINT instance_count[6];
  UINT cb_first[6][CONSTANT_SLOTS], cb_number[6][CONSTANT_SLOTS];      /* the windows of the constant buffers (by stage: vs, ps, gs, hs, ds, cs; in constants) */
  /* ---- not part of the copies */
  Buf *so_buf[4];                     /* SOSetTargets: the stream output buffers (their records go on from Buf.so_end; a deferred context records the call) */
  BOOL deferred;
  UINT flags;                         /* CreateDeferredContext's ContextFlags */
  struct Deferred *rec;               /* a deferred context: what it has recorded since the last FinishCommandList */
  struct CtxState *active;            /* SwapDeviceContextState: the context state object that is active (a reference; NULL: the device's own, not made yet) */
  BOOL hardware_protection;           /* SetHardwareProtectionState (device5.inc) */
  ID3D11VideoContext3 video;          /* the immediate context's ID3D11VideoContext .. 3 (video.inc; a deferred context has none: NULL table) */
} Ctx;
#define CTX_OBJECTS_FIRST offsetof(Ctx, vs)
#define CTX_OBJECTS_END (offsetof(Ctx, predicate) + sizeof(void *))
#define CTX_VALUES_FIRST offsetof(Ctx, vb_stride)
#define CTX_VALUES_END (offsetof(Ctx, cb_number) + sizeof(((Ctx *)0)->cb_number))

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
  ID3D11On12Device1 on12;                  /* ... and, made by D3D11On12CreateDevice on the program's Direct3D 12 device, ID3D11On12Device1 (interop.inc; NULL table: not one) */
  CRITICAL_SECTION lock;
  IDXGIAdapter *adapter;
  IDXGIFactory1 *factory;                  /* the factory the adapter was taken from, when the program gave none */
  D3D_FEATURE_LEVEL level;
  UINT flags;
  ID3D12Device *d12;
  int layered_targets;         /* Direct3D 12 below draws into arrays, cubes and volumes (render target and depth views of several slices): 0 not asked yet, 1 yes, 2 no */
  UINT next_tex_kind;                      /* what tex_make makes next, for the description a share carries: 0 as its arguments say, 2 one-dimensional (dev_CreateTexture1D) */
  ID3D12CommandQueue *queue;
  ID3D12CommandAllocator *allocator;
  ID3D12GraphicsCommandList *list;
  ID3D12RootSignature *root;
  ID3D12CommandSignature *sig_draw, *sig_draw_indexed, *sig_dispatch;      /* the indirect draws and dispatches (ExecuteIndirect of one command) */
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
  Buf *blank_buffer;                       /* ... and a buffer of zeros, for a buffer slot (t<n> or u<n>) a shader reads with nothing bound (what it reads beyond it is zero too) */
  Buf *blank_uav;                          /* ... and a buffer for an unordered access slot a shader declares with nothing bound (what it writes there is lost to the program) */
  UINT64 table_uavs;                       /* the GPU handle of the unordered access table the last draw used, and what was in it */
  UINT table_uav_ids[UAV_SLOTS];
  FLOAT table_min_lods[TEXTURE_SLOTS];     /* ... and the textures' SetResourceMinLOD clamps the sampler table was made with */
  UINT epoch;                              /* counts the command lists */
  int recorded;                            /* the list has something in it */
  UINT dirty;                              /* what the next draw has to set again (DIRTY_*) */
  Pipeline pipelines[PIPELINES];
  UINT pipeline_count, pipeline_clock;
  /* the queries (query.inc): those begun and not ended; the kernel's measurements of the list's draws (one query of the kernel's at a time), into slots of
   * `measure_heap`, read back through `measure_readback` once the list ran, and the queries each is for (references) */
  struct Query *active[ACTIVE_QUERIES];
  UINT active_count;
  ID3D12QueryHeap *measure_heap;
  ID3D12Resource *measure_readback;
  struct Measure *measures;                /* MEASURES of them */
  UINT measure_count;
  Buf *so_written[16];                     /* the stream output buffers the list has bound (references): where their records end is read when it has run */
  UINT so_written_count;
  PendingClear clears[8];                  /* depth clears a draw has not followed yet (Direct3D 12 here does them when the next pass begins) */
  UINT clear_count;
  Ctx *ctx;
  StateObj *states[4];                     /* the state objects made, by kind (STATE_*): what CreateBlendState ... give again for the same description */
  struct RemovalWatch *removal;            /* RegisterDeviceRemovedEvent (device5.inc): the events to set when Direct3D 12 below loses the device */
  ID3D11VideoDevice2 video;                /* the same object as ID3D11VideoDevice .. 2 (video.inc) */
  struct VideoState *video_state;          /* ... and Direct3D 12's video below it (made when first needed) */
};
#define DIRTY_TARGETS 1u
#define DIRTY_VIEWPORT 2u
#define DIRTY_INPUT 4u
#define DIRTY_PIPELINE 8u
#define DIRTY_ROOT 16u
#define DIRTY_SO 32u
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
/* A reference of the runtime's own to a child (hold) and its end (unhold): the object lives while either kind is there; AddRef and Release answer the program's. */
static void hold(void *object) { if (object) { InterlockedIncrement(&((Child *)object)->internal); dx_AddRef(OBJ(object)); } }
static void unhold(void *object) { if (object) { InterlockedDecrement(&((Child *)object)->internal); dx_Release(OBJ(object)); } }
static ULONG STDMETHODCALLTYPE child_AddRef(Child *c) { return (ULONG)(InterlockedIncrement(&c->o.ref) - c->internal); }
static ULONG STDMETHODCALLTYPE child_Release(Child *c) {
  const LONG internal = c->internal;
  const LONG n = (LONG)dx_Release(OBJ(c));
  return n > internal ? (ULONG)(n - internal) : 0;
}
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
  hold(object);      /* (a binding is the runtime's reference) */
  DxObj *old = *slot;
  *slot = object;
  unhold(old);
}

extern const ID3D11Device5Vtbl dev_vtbl;
extern const ID3D11DeviceContext4Vtbl ctx_vtbl;
extern const ID3D11BufferVtbl buf_vtbl;
extern const ID3D11Texture2D1Vtbl tex_vtbl;
extern const ID3D11RenderTargetView1Vtbl rtv_vtbl;
extern const ID3D11DepthStencilViewVtbl dsv_vtbl;
extern const ID3D11ShaderResourceView1Vtbl srv_vtbl;
extern const ID3D11VertexShaderVtbl vs_vtbl;
extern const ID3D11PixelShaderVtbl ps_vtbl;
extern const ID3D11GeometryShaderVtbl gs_vtbl;
extern const ID3D11HullShaderVtbl hs_vtbl;
extern const ID3D11DomainShaderVtbl ds_vtbl;
extern const ID3D11InputLayoutVtbl layout_vtbl;
extern const ID3D11BlendState1Vtbl blend_vtbl;
extern const ID3D11DepthStencilStateVtbl depth_vtbl;
extern const ID3D11RasterizerState2Vtbl raster_vtbl;
extern const ID3D11SamplerStateVtbl sampler_vtbl;
extern const ID3D11Query1Vtbl query_vtbl;
extern const ID3D11VideoDevice2Vtbl vdev_vtbl;      /* (video.inc) */
static void video_shutdown(Dev *d);
extern const ID3D11VideoContext3Vtbl vctx_vtbl;

static const GUID *const g_dev_iids[] = {&IID_ID3D11Device, NULL};
static const GUID *const g_ctx_iids[] = {&IID_ID3D11DeviceChild, &IID_ID3D11DeviceContext, NULL};
static const GUID *const g_buf_iids[] = {&IID_ID3D11DeviceChild, &IID_ID3D11Resource, &IID_ID3D11Buffer, NULL};
static const GUID *const g_tex_iids[] = {&IID_ID3D11DeviceChild, &IID_ID3D11Resource, &IID_ID3D11Texture2D, &IID_ID3D11Texture2D1, NULL};
static const GUID *const g_tex3_iids[] = {&IID_ID3D11DeviceChild, &IID_ID3D11Resource, &IID_ID3D11Texture3D, &IID_ID3D11Texture3D1, NULL};
static const GUID *const g_rtv_iids[] = {&IID_ID3D11DeviceChild, &IID_ID3D11View, &IID_ID3D11RenderTargetView, &IID_ID3D11RenderTargetView1, NULL};
static const GUID *const g_dsv_iids[] = {&IID_ID3D11DeviceChild, &IID_ID3D11View, &IID_ID3D11DepthStencilView, NULL};
static const GUID *const g_srv_iids[] = {&IID_ID3D11DeviceChild, &IID_ID3D11View, &IID_ID3D11ShaderResourceView, &IID_ID3D11ShaderResourceView1, NULL};
static const GUID *const g_vs_iids[] = {&IID_ID3D11DeviceChild, &IID_ID3D11VertexShader, NULL};
static const GUID *const g_ps_iids[] = {&IID_ID3D11DeviceChild, &IID_ID3D11PixelShader, NULL};
static const GUID *const g_gs_iids[] = {&IID_ID3D11DeviceChild, &IID_ID3D11GeometryShader, NULL};
static const GUID *const g_hs_iids[] = {&IID_ID3D11DeviceChild, &IID_ID3D11HullShader, NULL};
static const GUID *const g_ds_iids[] = {&IID_ID3D11DeviceChild, &IID_ID3D11DomainShader, NULL};
static const GUID *const g_layout_iids[] = {&IID_ID3D11DeviceChild, &IID_ID3D11InputLayout, NULL};
static const GUID *const g_depth_iids[] = {&IID_ID3D11DeviceChild, &IID_ID3D11DepthStencilState, NULL};
static const GUID *const g_sampler_iids[] = {&IID_ID3D11DeviceChild, &IID_ID3D11SamplerState, NULL};
static const GUID *const g_query_iids[] = {&IID_ID3D11DeviceChild, &IID_ID3D11Asynchronous, &IID_ID3D11Query, &IID_ID3D11Query1, NULL};

/* ---- the command list ------------------------------------------------------------------------------------------------------------------------------- */

static void record_pending_clears(Dev *d);

/* ---- upload buffers that outlive the program's use of them ------------------------------------------------------------------------------------- */

/* Sending the list is a whole trip to the GPU and back, and d3d12.dll sends an upload buffer as it is at that moment (its CPU copy, at ExecuteCommandLists).
 * A program that writes a buffer a recorded draw still reads (Map with WRITE_DISCARD, as ANGLE does for each batch of Skia's rectangles) had to have the list
 * sent first, once for every write: about 300 ms a frame in Chrome. Instead such a buffer gets other memory, and the old is retired: kept, as it is, until
 * the list has run; then it is a spare for the next buffer of its size. */
static void spare_keep(Dev *d, Spare s) {
  if (s.uav < 0) { ID3D12Resource_Release(s.res); return; }      /* (a buffer of the GPU's memory: not an upload buffer to use again) */
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
static ID3D12Resource *make_buffer_flags(Dev *d, D3D12_HEAP_TYPE heap, UINT64 bytes, D3D12_RESOURCE_FLAGS flags);
static void flush(Dev *d);
static void removal_shutdown(Dev *d);
/* An upload buffer of at least `bytes`, mapped: a spare (at most twice the size) or a new one. NULL if there is none. `uav`: one shaders may read as a shader resource
 * view (the kernel takes buffer views of unordered-access allocations only). */
static ID3D12Resource *take_upload_of(Dev *d, UINT64 bytes, int uav, BYTE **data, UINT64 *size) {
  UINT best = d->spare_count;
  for (UINT i = 0; i < d->spare_count; ++i)
    if (d->spares[i].uav == uav && d->spares[i].bytes >= bytes && d->spares[i].bytes <= bytes * 2 && (best == d->spare_count || d->spares[i].bytes < d->spares[best].bytes)) best = i;
  ID3D12Resource *res;
  if (best < d->spare_count) {
    res = d->spares[best].res;
    *size = d->spares[best].bytes;
    d->spare_bytes -= *size;
    d->spares[best] = d->spares[--d->spare_count];
  } else {
    res = make_buffer_flags(d, D3D12_HEAP_TYPE_UPLOAD, bytes, uav ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE);
    *size = bytes;
    if (!res) return NULL;
  }
  *data = NULL;
  if (FAILED(ID3D12Resource_Map(res, 0, NULL, (void **)data)) || !*data) { ID3D12Resource_Release(res); return NULL; }
  return res;
}
static ID3D12Resource *take_upload(Dev *d, UINT64 bytes, BYTE **data, UINT64 *size) { return take_upload_of(d, bytes, 0, data, size); }
/* `res` is read by the list being recorded: kept until the list has run (sent first when too much waits). The reference passes. */
static void retire_of(Dev *d, ID3D12Resource *res, BYTE *data, UINT64 bytes, int uav) {
  const Spare s = {res, data, bytes, uav};
  if (d->retired_count == RETIRED_MAX) flush(d);      /* (after which nothing reads it any more) */
  if (d->retired_count == RETIRED_MAX) { spare_keep(d, s); return; }
  d->retired[d->retired_count++] = s;
  d->retired_bytes += bytes;
}
static void retire(Dev *d, ID3D12Resource *res, BYTE *data, UINT64 bytes) { retire_of(d, res, data, bytes, 0); }

/* Sends what was recorded (the queue runs it before it returns) and begins a new list. */
static void measures_resolve(Dev *d);
static void measures_collect(Dev *d);
static void so_collect(Dev *d);
static void flush(Dev *d) {
  if (!d->recorded) return;
  measures_resolve(d);      /* (what the kernel measured for the queries goes into memory the CPU reads after the list ran) */
  if (d->ring_used) { void *p = NULL; if (SUCCEEDED(ID3D12Resource_Map(d->ring, 0, NULL, &p))) ID3D12Resource_Unmap(d->ring, 0, NULL); }      /* (the constants go with the list) */
  ID3D12GraphicsCommandList_Close(d->list);
  ID3D12CommandList *lists[1] = {(ID3D12CommandList *)d->list};
  ID3D12CommandQueue_ExecuteCommandLists(d->queue, 1, lists);
  measures_collect(d);
  so_collect(d);
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

static ID3D12Resource *make_buffer_flags(Dev *d, D3D12_HEAP_TYPE heap, UINT64 bytes, D3D12_RESOURCE_FLAGS flags) {
  D3D12_HEAP_PROPERTIES props = {heap, D3D12_CPU_PAGE_PROPERTY_UNKNOWN, D3D12_MEMORY_POOL_UNKNOWN, 1, 1};
  D3D12_RESOURCE_DESC desc = {D3D12_RESOURCE_DIMENSION_BUFFER, 0, bytes < 16 ? 16 : bytes, 1, 1, 1, DXGI_FORMAT_UNKNOWN, {1, 0}, D3D12_TEXTURE_LAYOUT_ROW_MAJOR, flags};
  ID3D12Resource *res = NULL;
  ID3D12Device_CreateCommittedResource(d->d12, &props, D3D12_HEAP_FLAG_NONE, &desc,
                                       heap == D3D12_HEAP_TYPE_READBACK ? D3D12_RESOURCE_STATE_COPY_DEST
                                       : heap == D3D12_HEAP_TYPE_DEFAULT ? D3D12_RESOURCE_STATE_COMMON : D3D12_RESOURCE_STATE_GENERIC_READ, NULL, &IID_ID3D12Resource,
                                       (void **)&res);
  return res;
}
static ID3D12Resource *make_buffer(Dev *d, D3D12_HEAP_TYPE heap, UINT64 bytes) { return make_buffer_flags(d, heap, bytes, D3D12_RESOURCE_FLAG_NONE); }

/* ---- formats ----------------------------------------------------------------------------------------------------------------------------------------- */

#include "formats.inc"

/* ---- buffers ----------------------------------------------------------------------------------------------------------------------------------------- */

static void buf_destroy(DxObj *o) {
  Buf *b = (Buf *)o;
  Dev *d = b->c.dev;
  if (b->converted) { const Spare s = {b->converted, NULL, b->converted_bytes, 0}; if (b->converted_used == d->epoch) retire(d, s.res, NULL, s.bytes); else spare_keep(d, s); }
  if (b->so_fill) { settle(d, b->used); ID3D12Resource_Release(b->so_fill); }
  if (b->res) {
    const Spare s = {b->res, b->mapped ? b->data : NULL, b->bytes, b->gpu ? -1 : b->uav};
    if (b->used == d->epoch) retire_of(d, s.res, s.data, s.bytes, s.uav); else spare_keep(d, s);
  }
  child_destroy(o);
}
static void STDMETHODCALLTYPE buf_GetType(Buf *b, D3D11_RESOURCE_DIMENSION *type) { (void)b; if (type) *type = D3D11_RESOURCE_DIMENSION_BUFFER; }
static void STDMETHODCALLTYPE buf_SetEvictionPriority(Buf *b, UINT priority) { (void)b; (void)priority; }
static UINT STDMETHODCALLTYPE buf_GetEvictionPriority(Buf *b) { (void)b; return 0; }
static void STDMETHODCALLTYPE buf_GetDesc(Buf *b, D3D11_BUFFER_DESC *desc) { if (desc) *desc = b->desc; }

/* ---- buffers in the GPU's memory -------------------------------------------------------------------------------------------------------------------- */

/* Writes `bytes` at `offset` of a buffer of the GPU's memory: a copy from upload memory, recorded in order with the draws and dispatches. */
static HRESULT buf_gpu_write(Dev *d, Buf *b, UINT64 offset, const void *data, UINT64 bytes) {
  if (!bytes) return S_OK;
  if (!retire_room(d, bytes)) flush(d);
  BYTE *memory = NULL;
  UINT64 size = 0;
  ID3D12Resource *upload = take_upload(d, bytes, &memory, &size);
  if (!upload) return E_OUTOFMEMORY;
  memcpy(memory, data, bytes);
  ID3D12Resource_Unmap(upload, 0, NULL);
  ID3D12GraphicsCommandList_CopyBufferRegion(d->list, b->res, offset, upload, 0, bytes);
  d->recorded = 1;
  b->used = b->written = d->epoch;
  ++b->version;
  retire(d, upload, NULL, size);
  return S_OK;
}
/* A buffer of the GPU's memory as it is made: `data` (ByteWidth bytes) or zeros. */
static HRESULT buf_gpu_fill(Dev *d, Buf *b, const void *data) {
  BYTE *zeros = NULL;
  if (!data) { zeros = mem_alloc(b->desc.ByteWidth); if (!zeros) return E_OUTOFMEMORY; memset(zeros, 0, b->desc.ByteWidth); }
  const HRESULT hr = buf_gpu_write(d, b, 0, data ? data : zeros, b->desc.ByteWidth);
  mem_free(zeros);
  return hr;
}
/* Reads `bytes` at `offset` of a buffer of the GPU's memory, after everything recorded before (the list is sent). */
static HRESULT buf_gpu_read(Dev *d, Buf *b, UINT64 offset, UINT64 bytes, void *out) {
  if (!bytes) return S_OK;
  ID3D12Resource *readback = make_buffer(d, D3D12_HEAP_TYPE_READBACK, bytes);
  if (!readback) return E_OUTOFMEMORY;
  ID3D12GraphicsCommandList_CopyBufferRegion(d->list, readback, 0, b->res, offset, bytes);
  d->recorded = 1;
  flush(d);
  const BYTE *memory = NULL;
  HRESULT hr = ID3D12Resource_Map(readback, 0, NULL, (void **)&memory);
  if (SUCCEEDED(hr) && memory) { memcpy(out, memory, bytes); ID3D12Resource_Unmap(readback, 0, NULL); }
  else if (SUCCEEDED(hr)) hr = E_FAIL;
  ID3D12Resource_Release(readback);
  return hr;
}

/* Whether a buffer description is one Direct3D 11 takes (the rules of ID3D11Device::CreateBuffer at feature level 11_0). */
static BOOL buffer_desc_valid(const D3D11_BUFFER_DESC *desc, const D3D11_SUBRESOURCE_DATA *initial) {
  const UINT bind = desc->BindFlags, misc = desc->MiscFlags;
  const UINT known_bind = D3D11_BIND_VERTEX_BUFFER | D3D11_BIND_INDEX_BUFFER | D3D11_BIND_CONSTANT_BUFFER | D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_STREAM_OUTPUT |
                          D3D11_BIND_UNORDERED_ACCESS;
  if (desc->ByteWidth == 0 || desc->ByteWidth > (256u << 20) || (bind & ~known_bind)) return FALSE;
  if (misc & (D3D11_RESOURCE_MISC_TILED | D3D11_RESOURCE_MISC_TILE_POOL)) return FALSE;      /* (TiledResourcesTier NOT_SUPPORTED) */
  if (desc->Usage > D3D11_USAGE_STAGING || (desc->CPUAccessFlags & ~(UINT)(D3D11_CPU_ACCESS_READ | D3D11_CPU_ACCESS_WRITE))) return FALSE;
  switch (desc->Usage) {
    case D3D11_USAGE_DEFAULT: if (desc->CPUAccessFlags) return FALSE; break;
    case D3D11_USAGE_IMMUTABLE: if (desc->CPUAccessFlags || !initial || !initial->pSysMem || (bind & (D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_STREAM_OUTPUT))) return FALSE; break;
    case D3D11_USAGE_DYNAMIC: if (desc->CPUAccessFlags != D3D11_CPU_ACCESS_WRITE || (bind & (D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_STREAM_OUTPUT))) return FALSE; break;
    case D3D11_USAGE_STAGING: if (bind || !desc->CPUAccessFlags) return FALSE; break;
  }
  /* (a constant buffer: whole registers, at most 4096 of them, and nothing else at feature level 11_0) */
  if ((bind & D3D11_BIND_CONSTANT_BUFFER) && (bind != D3D11_BIND_CONSTANT_BUFFER || desc->ByteWidth % 16 || desc->ByteWidth > 65536)) return FALSE;
  const BOOL structured = (misc & D3D11_RESOURCE_MISC_BUFFER_STRUCTURED) != 0, raw = (misc & D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS) != 0;
  if (structured && raw) return FALSE;
  if (structured && (desc->StructureByteStride == 0 || desc->StructureByteStride % 4 || desc->StructureByteStride > 2048 || desc->ByteWidth % desc->StructureByteStride)) return FALSE;
  if ((structured || raw) && (bind & (D3D11_BIND_VERTEX_BUFFER | D3D11_BIND_INDEX_BUFFER | D3D11_BIND_STREAM_OUTPUT | D3D11_BIND_CONSTANT_BUFFER))) return FALSE;
  if (raw && desc->ByteWidth % 4) return FALSE;
  if ((misc & D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS) && structured) return FALSE;
  return TRUE;
}

static HRESULT STDMETHODCALLTYPE dev_CreateBuffer(Dev *d, const D3D11_BUFFER_DESC *desc, const D3D11_SUBRESOURCE_DATA *initial, ID3D11Buffer **out) {
  if (out) *out = NULL;
  if (!desc || !buffer_desc_valid(desc, initial) || (initial && !initial->pSysMem)) return E_INVALIDARG;
  if (!out) return S_FALSE;
  Buf *b = child_new(sizeof *b, d, &buf_vtbl, g_buf_iids, buf_destroy);
  if (!b) return E_OUTOFMEMORY;
  b->desc = *desc;
  b->version = 1;
  if (desc->Usage == D3D11_USAGE_DEFAULT && (desc->BindFlags & (D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_STREAM_OUTPUT))) {
    /* (the GPU writes it: a buffer of its memory, which unordered access views and stream output may name) */
    b->gpu = 1;
    b->bytes = (desc->ByteWidth + 15u) & ~15u;
    b->res = make_buffer_flags(d, D3D12_HEAP_TYPE_DEFAULT, b->bytes, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    if (!b->res) { dx_Release(OBJ(b)); return E_OUTOFMEMORY; }
    EnterCriticalSection(&d->lock);
    const HRESULT hr = buf_gpu_fill(d, b, initial && initial->pSysMem ? initial->pSysMem : NULL);
    LeaveCriticalSection(&d->lock);
    if (FAILED(hr)) { dx_Release(OBJ(b)); return hr; }
    return give(b, (void **)out);
  }
  b->uav = (desc->BindFlags & D3D11_BIND_SHADER_RESOURCE) != 0;
  b->res = take_upload_of(d, desc->ByteWidth, b->uav, &b->data, &b->bytes);
  if (!b->res) { dx_Release(OBJ(b)); return E_OUTOFMEMORY; }
  if (initial && initial->pSysMem) memcpy(b->data, initial->pSysMem, desc->ByteWidth);
  else memset(b->data, 0, desc->ByteWidth);
  ID3D12Resource_Unmap(b->res, 0, NULL);      /* (marks it changed; see Buf.data) */
  return give(b, (void **)out);
}

/* ---- textures ---------------------------------------------------------------------------------------------------------------------------------------- */

/* Whether Direct3D 12 below makes render targets of several slices (the kernel's layered targets): asked once, with a small array it is given to make. */
static BOOL dev_layered_targets(Dev *d) {
  if (!d->layered_targets) {
    const D3D12_HEAP_PROPERTIES props = {D3D12_HEAP_TYPE_DEFAULT, D3D12_CPU_PAGE_PROPERTY_UNKNOWN, D3D12_MEMORY_POOL_UNKNOWN, 1, 1};
    const D3D12_RESOURCE_DESC rd = {D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, 4, 4, 2, 1, DXGI_FORMAT_R8G8B8A8_UNORM, {1, 0}, D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET};
    ID3D12Resource *probe = NULL;
    const HRESULT hr = ID3D12Device_CreateCommittedResource(d->d12, &props, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COMMON, NULL, &IID_ID3D12Resource, (void **)&probe);
    d->layered_targets = SUCCEEDED(hr) && probe ? 1 : 2;
    if (probe) ID3D12Resource_Release(probe);
  }
  return d->layered_targets == 1;
}

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
static UINT tex_rows(const Tex *t, UINT h) { return t->block ? (h + 3) / 4 : t->planar ? h + (h + 1) / 2 : h; }      /* (a planar texture: the luma's rows and the chroma's) */
/* The slices of a level of a volume (1 for the other kinds). */
static UINT tex_slices(const Tex *t, UINT sub) { return t->kind == 1 ? mip_size(t->slices, sub % t->desc.MipLevels) : 1; }

/* Sends rows of pixels (as the program has them, `pitch` bytes apart) into the rectangle (x, y, w, h) of a subresource. The list is sent with it. */
static void tex_sync(Dev *d, Tex *t);      /* (rtshadow.inc) */
/* ---- planar textures (NV12, P010, P016) ------------------------------------------------------------------------------------------------------------------ */

/* The bytes of a texel of a plane: a luma sample, or a U, V pair. A plane's size: the luma's is the texture's, the chroma's half of it both ways (the sizes are even). */
static UINT plane_texel(const Tex *t, UINT plane) { return plane ? 2 * t->planar : t->planar; }
static UINT plane_width(const Tex *t, UINT plane) { return plane ? t->desc.Width / 2 : t->desc.Width; }
static UINT plane_height(const Tex *t, UINT plane) { return plane ? t->desc.Height / 2 : t->desc.Height; }
/* The Direct3D 12 subresource of a plane of a slice. */
static UINT plane_sub(const Tex *t, UINT plane, UINT slice) { return plane * t->desc.ArraySize + slice; }

/* Rows of a plane's texels (`pitch` bytes apart) into the rectangle (x, y, w, h) of that plane of a slice, in the plane's texels: a copy from upload memory recorded in
 * order with the draws (as tex_write_rows does). The rectangle is inside the plane (the callers clip it). */
static HRESULT tex_plane_write(Dev *d, Tex *t, UINT plane, UINT slice, UINT x, UINT y, UINT w, UINT h, const BYTE *rows, UINT pitch) {
  if (!w || !h) return S_OK;
  const UINT row_bytes = w * plane_texel(t, plane), row_pitch = (row_bytes + 255u) & ~255u;
  settle(d, t->sampled);      /* (a texture the list sampled is not copied into in the same list) */
  const UINT64 upload_bytes = (UINT64)row_pitch * h;
  if (!retire_room(d, upload_bytes)) flush(d);
  BYTE *memory = NULL;
  UINT64 upload_size = 0;
  ID3D12Resource *upload = take_upload(d, upload_bytes, &memory, &upload_size);
  if (!upload) return E_OUTOFMEMORY;
  for (UINT row = 0; row < h; ++row) memcpy(memory + (SIZE_T)row * row_pitch, rows + (SIZE_T)row * pitch, row_bytes);
  ID3D12Resource_Unmap(upload, 0, NULL);
  D3D12_TEXTURE_COPY_LOCATION to, from;
  memset(&to, 0, sizeof to);
  memset(&from, 0, sizeof from);
  to.pResource = t->res;
  to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  to.SubresourceIndex = plane_sub(t, plane, slice);
  from.pResource = upload;
  from.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
  from.PlacedFootprint.Footprint.Format = plane_format_of(t->desc.Format, plane);
  from.PlacedFootprint.Footprint.Width = w;
  from.PlacedFootprint.Footprint.Height = h;
  from.PlacedFootprint.Footprint.Depth = 1;
  from.PlacedFootprint.Footprint.RowPitch = row_pitch;
  ID3D12GraphicsCommandList_CopyTextureRegion(d->list, &to, x, y, 0, &from, NULL);
  d->recorded = 1;
  ++t->version;
  t->used = t->written = d->epoch;
  retire(d, upload, NULL, upload_size);
  return S_OK;
}
/* The rectangle (x, y, w, h) of a slice, in luma pixels on whole 2 x 2 blocks: `rows` has its luma rows and after them (h rows on) its rows of U, V pairs, `pitch` bytes
 * apart. */
static HRESULT tex_planar_write(Dev *d, Tex *t, UINT slice, UINT x, UINT y, UINT w, UINT h, const BYTE *rows, UINT pitch) {
  if (slice >= t->desc.ArraySize || (x | y | w | h) & 1 || x > t->desc.Width || y > t->desc.Height || w > t->desc.Width - x || h > t->desc.Height - y) return E_INVALIDARG;
  HRESULT hr = tex_plane_write(d, t, 0, slice, x, y, w, h, rows, pitch);
  if (SUCCEEDED(hr)) hr = tex_plane_write(d, t, 1, slice, x / 2, y / 2, w / 2, h / 2, rows + (SIZE_T)pitch * h, pitch);
  return hr;
}
/* A whole slice, both planes, as the CPU sees it (the luma's rows, then the chroma's, `pitch` bytes apart), after everything recorded before (the list is sent). */
static HRESULT tex_planar_read(Dev *d, Tex *t, UINT slice, BYTE *rows, UINT pitch) {
  if (slice >= t->desc.ArraySize) return E_INVALIDARG;
  UINT64 offsets[2], total = 0;
  UINT pitches[2];
  for (UINT plane = 0; plane < 2; ++plane) {
    pitches[plane] = (plane_width(t, plane) * plane_texel(t, plane) + 255u) & ~255u;
    offsets[plane] = total;
    total += ((UINT64)pitches[plane] * plane_height(t, plane) + 511u) & ~(UINT64)511u;      /* (a footprint's placement: 512-byte aligned) */
  }
  ID3D12Resource *readback = make_buffer(d, D3D12_HEAP_TYPE_READBACK, total);
  if (!readback) return E_OUTOFMEMORY;
  settle(d, t->written);      /* (... nor read in the list that wrote it) */
  for (UINT plane = 0; plane < 2; ++plane) {
    D3D12_TEXTURE_COPY_LOCATION to, from;
    memset(&to, 0, sizeof to);
    memset(&from, 0, sizeof from);
    to.pResource = readback;
    to.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    to.PlacedFootprint.Offset = offsets[plane];
    to.PlacedFootprint.Footprint.Format = plane_format_of(t->desc.Format, plane);
    to.PlacedFootprint.Footprint.Width = plane_width(t, plane);
    to.PlacedFootprint.Footprint.Height = plane_height(t, plane);
    to.PlacedFootprint.Footprint.Depth = 1;
    to.PlacedFootprint.Footprint.RowPitch = pitches[plane];
    from.pResource = t->res;
    from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    from.SubresourceIndex = plane_sub(t, plane, slice);
    ID3D12GraphicsCommandList_CopyTextureRegion(d->list, &to, 0, 0, 0, &from, NULL);
  }
  d->recorded = 1;
  t->used = d->epoch;
  flush(d);
  const BYTE *memory = NULL;
  HRESULT hr = ID3D12Resource_Map(readback, 0, NULL, (void **)&memory);
  if (SUCCEEDED(hr) && memory) {
    BYTE *at = rows;
    for (UINT plane = 0; plane < 2; ++plane) {
      const UINT row_bytes = plane_width(t, plane) * plane_texel(t, plane);
      for (UINT row = 0; row < plane_height(t, plane); ++row, at += pitch) memcpy(at, memory + offsets[plane] + (SIZE_T)row * pitches[plane], row_bytes);
    }
    ID3D12Resource_Unmap(readback, 0, NULL);
  } else if (SUCCEEDED(hr)) hr = E_FAIL;
  ID3D12Resource_Release(readback);
  return hr;
}
/* One texel value (`bytes` of it) in the rectangle (x0, y0) .. (x1, y1) of a plane of a slice, in the plane's texels (clipped to the plane). */
static void tex_plane_fill(Dev *d, Tex *t, UINT plane, UINT slice, UINT x0, UINT y0, UINT x1, UINT y1, const BYTE *element, UINT bytes) {
  if (x1 > plane_width(t, plane)) x1 = plane_width(t, plane);
  if (y1 > plane_height(t, plane)) y1 = plane_height(t, plane);
  if (bytes != plane_texel(t, plane) || x0 >= x1 || y0 >= y1 || slice >= t->desc.ArraySize) return;
  const UINT w = x1 - x0, h = y1 - y0;
  BYTE *fill = mem_alloc((SIZE_T)w * h * bytes);
  if (!fill) return;
  for (SIZE_T at = 0; at < (SIZE_T)w * h * bytes; at += bytes) memcpy(fill + at, element, bytes);
  tex_plane_write(d, t, plane, slice, x0, y0, w, h, fill, w * bytes);
  mem_free(fill);
}
/* A box of a slice of one planar texture into another of the same format, both planes, as copies the list makes. Direct3D 11 copies nothing when the box or the place is
 * not on whole 2 x 2 blocks of luma (the chroma's samples). */
static void tex_planar_copy(Dev *d, Tex *to, UINT to_slice, UINT x, UINT y, Tex *from, UINT from_slice, const D3D11_BOX *box) {
  const UINT x0 = box ? box->left : 0, y0 = box ? box->top : 0, x1 = box ? box->right : from->desc.Width, y1 = box ? box->bottom : from->desc.Height;
  if (to->desc.Format != from->desc.Format || to_slice >= to->desc.ArraySize || from_slice >= from->desc.ArraySize || (to == from && to_slice == from_slice)) return;
  if (x0 >= x1 || y0 >= y1 || x1 > from->desc.Width || y1 > from->desc.Height || (box && (box->front != 0 || box->back != 1))) return;
  if ((x0 | y0 | x1 | y1 | x | y) & 1) return;
  if (x > to->desc.Width || y > to->desc.Height || x1 - x0 > to->desc.Width - x || y1 - y0 > to->desc.Height - y) return;
  settle(d, to->sampled);
  settle(d, from->written);
  for (UINT plane = 0; plane < 2; ++plane) {
    const UINT s = plane ? 2 : 1;
    D3D12_TEXTURE_COPY_LOCATION dst, src;
    memset(&dst, 0, sizeof dst);
    memset(&src, 0, sizeof src);
    dst.pResource = to->res;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.SubresourceIndex = plane_sub(to, plane, to_slice);
    src.pResource = from->res;
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.SubresourceIndex = plane_sub(from, plane, from_slice);
    const D3D12_BOX region = {x0 / s, y0 / s, 0, x1 / s, y1 / s, 1};
    ID3D12GraphicsCommandList_CopyTextureRegion(d->list, &dst, x / s, y / s, 0, &src, &region);
  }
  ++to->version;
  to->used = from->used = to->written = d->epoch;
  d->recorded = 1;
}

/* `raw`: the rows are in the stored format (GenerateMips), not the program's. */
static HRESULT tex_write_rows(Dev *d, Tex *t, UINT sub, UINT x, UINT y, UINT z, UINT w, UINT h, const BYTE *rows, UINT pitch, BOOL raw);
static HRESULT tex_write(Dev *d, Tex *t, UINT sub, UINT x, UINT y, UINT z, UINT w, UINT h, const BYTE *rows, UINT pitch) { return tex_write_rows(d, t, sub, x, y, z, w, h, rows, pitch, FALSE); }
static HRESULT tex_write_rows(Dev *d, Tex *t, UINT sub, UINT x, UINT y, UINT z, UINT w, UINT h, const BYTE *rows, UINT pitch, BOOL raw) {
  if (t->depth || w == 0 || h == 0) return E_INVALIDARG;
  if (t->planar) return tex_planar_write(d, t, sub, x, y, w, h, rows, pitch);      /* (both planes of the slice: `rows` has the chroma after the luma) */
  if (format_pairs(t->desc.Format) && ((x & 1) || ((w & 1) && x + w != t->desc.Width))) return E_INVALIDARG;      /* (YUY2, Y210, Y216: whole pairs of pixels) */
  tex_sync(d, t);      /* (what views drew into it goes in first: this write comes after) */
  ++t->version;
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
    if (raw) memcpy(to, in, (SIZE_T)w * t->stored_bpp); else row_to_stored(t->fmt, in, to, w, row_bytes);
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
static HRESULT tex_read_rows(Dev *d, Tex *t, UINT sub, UINT z, BYTE *rows, BOOL raw);
static HRESULT tex_read(Dev *d, Tex *t, UINT sub, UINT z, BYTE *rows) { return tex_read_rows(d, t, sub, z, rows, FALSE); }
/* `raw`: rows of `w * stored_bpp` bytes in the stored format. */
static HRESULT tex_read_rows(Dev *d, Tex *t, UINT sub, UINT z, BYTE *rows, BOOL raw) {
  if (t->depth) return E_INVALIDARG;
  if (t->planar) return tex_planar_read(d, t, sub, rows, tex_pitch(t, t->desc.Width));
  tex_sync(d, t);
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
      if (raw) memcpy(rows + (SIZE_T)row * w * t->stored_bpp, in, (SIZE_T)w * t->stored_bpp);
      else row_from_stored(t->fmt, in, to_row, w, tex_pitch(t, w));
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
  if (t->keyed_mutex) kmt_destroy_keyed_mutex(t->keyed_mutex);      /* (held: abandoned, as Windows has it) */
  child_destroy(o);
}
static void STDMETHODCALLTYPE tex_GetType(Tex *t, D3D11_RESOURCE_DIMENSION *type) { (void)t; if (type) *type = D3D11_RESOURCE_DIMENSION_TEXTURE2D; }
static void STDMETHODCALLTYPE tex_SetEvictionPriority(Tex *t, UINT priority) { (void)t; (void)priority; }
static UINT STDMETHODCALLTYPE tex_GetEvictionPriority(Tex *t) { (void)t; return 0; }
static void STDMETHODCALLTYPE tex_GetDesc(Tex *t, D3D11_TEXTURE2D_DESC *desc) { if (desc) *desc = t->desc; }
static void STDMETHODCALLTYPE tex3_GetType(Tex *t, D3D11_RESOURCE_DIMENSION *type) { (void)t; if (type) *type = D3D11_RESOURCE_DIMENSION_TEXTURE3D; }
static void STDMETHODCALLTYPE tex1d_GetType(Tex *t, D3D11_RESOURCE_DIMENSION *type) { (void)t; if (type) *type = D3D11_RESOURCE_DIMENSION_TEXTURE1D; }
static void STDMETHODCALLTYPE tex1d_GetDesc(Tex *t, D3D11_TEXTURE1D_DESC *desc) { if (desc) *desc = t->desc1; }
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

/* What a shared texture carries for the device that opens it (TcosD3D12ShareExtra): "D11T", the kind (0 2D, 1 3D, 2 1D), then the D3D11_TEXTURE2D_DESC as tex_make
 * keeps it, the depth of a volume and the global share handle of its keyed mutex (a texture shared with a legacy handle: its mutex has one too; 0 otherwise), each a
 * 32-bit word. */
#define TEX_SHARE_BYTES 60
static void tex_share_blob(const D3D11_TEXTURE2D_DESC *t, UINT kind, UINT slices, UINT mutex_global, BYTE out[TEX_SHARE_BYTES]) {
  const UINT words[14] = {kind, t->Width, t->Height, t->MipLevels, t->ArraySize, t->Format, t->SampleDesc.Count, t->SampleDesc.Quality, t->Usage, t->BindFlags,
                          t->CPUAccessFlags, t->MiscFlags, slices, mutex_global};
  out[0] = 'D', out[1] = '1', out[2] = '1', out[3] = 'T';
  memcpy(out + 4, words, sizeof words);
}
static int tex_share_read(const BYTE *in, UINT size, D3D11_TEXTURE2D_DESC *t, UINT *kind, UINT *slices, UINT *mutex_global) {
  UINT words[14];
  if (size != TEX_SHARE_BYTES || in[0] != 'D' || in[1] != '1' || in[2] != '1' || in[3] != 'T') return 0;
  memcpy(words, in + 4, sizeof words);
  if (words[0] > 2) return 0;
  *kind = words[0];
  t->Width = words[1], t->Height = words[2], t->MipLevels = words[3], t->ArraySize = words[4], t->Format = (DXGI_FORMAT)words[5];
  t->SampleDesc.Count = words[6], t->SampleDesc.Quality = words[7], t->Usage = (D3D11_USAGE)words[8], t->BindFlags = words[9], t->CPUAccessFlags = words[10];
  t->MiscFlags = words[11];
  *slices = words[12];
  *mutex_global = words[13];
  return 1;
}

/* A texture object around `res` (a reference is taken), or around a new Direct3D 12 texture when it is NULL. `slices`: 0 for a 2D texture, array or cube, else the
 * depth of a volume (`in` then has one layer). */
static UINT dev_ms_levels(Dev *d, DXGI_FORMAT format, UINT samples);
static HRESULT tex_make(Dev *d, const D3D11_TEXTURE2D_DESC *in, UINT slices, ID3D12Resource *res, Tex **out) {
  D3D11_TEXTURE2D_DESC desc = *in;
  *out = NULL;
  if (desc.Width == 0 || desc.Height == 0 || desc.ArraySize == 0) return E_INVALIDARG;
  /* (what a texture may be bound as, and the combinations Direct3D 11 refuses) */
  const UINT bind = desc.BindFlags;
  if ((bind & ~(UINT)(D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET | D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_DECODER | D3D11_BIND_VIDEO_ENCODER)) ||
      ((bind & D3D11_BIND_DEPTH_STENCIL) && (bind & (D3D11_BIND_RENDER_TARGET | D3D11_BIND_UNORDERED_ACCESS))))
    return E_INVALIDARG;
  if ((desc.MiscFlags & D3D11_RESOURCE_MISC_SHARED) && (desc.MiscFlags & D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX)) return E_INVALIDARG;
  /* (what a video encoder reads: there is no encoder - no format has D3D11_FORMAT_SUPPORT_VIDEO_ENCODER -; what a decoder writes: the formats the decoders put out) */
  const TexFormat *made_format = tex_format(desc.Format);
  const BOOL planar = made_format && (made_format->flags & FMT_PLANAR);
  if (bind & D3D11_BIND_VIDEO_ENCODER) return E_INVALIDARG;
  const BOOL yuv = made_format && (made_format->flags & FMT_YUV);
  /* (the decoders' formats: NV12 and P010, P016 for AV1's 12-bit 4:2:0, the packed ones of AV1's profiles 1 and 2) */
  if ((bind & D3D11_BIND_DECODER) && desc.Format != DXGI_FORMAT_NV12 && desc.Format != DXGI_FORMAT_P010 && desc.Format != DXGI_FORMAT_P016 && !yuv) return E_INVALIDARG;
  if (yuv) {
    /* AYUV, Y410, Y416, YUY2, Y210, Y216: a 2D texture or array of one level, not multisampled, a depth buffer or a cube; YUY2, Y210, Y216 of even width */
    if (slices || desc.MipLevels != 1 || desc.SampleDesc.Count != 1 || desc.SampleDesc.Quality || (desc.MiscFlags & D3D11_RESOURCE_MISC_TEXTURECUBE) ||
        (bind & D3D11_BIND_DEPTH_STENCIL) || (format_pairs(desc.Format) && (desc.Width & 1)))
      return E_INVALIDARG;
  }
  if (planar) {
    /* NV12, P010, P016: a 2D texture or array of one level, of even width and height (the chroma is half of both), not multisampled, not a depth buffer or a cube; it
     * may be shared - SHARED, SHARED_NTHANDLE, SHARED_KEYEDMUTEX (D3D11_FEATURE_D3D11_OPTIONS4: ExtendedNV12SharedTextureSupported), every plane of every slice going
     * with it -, nothing else of the misc flags; the CPU reaches one of a single slice (as on Windows, which has no CPU access to arrays of them) */
    const UINT planar_misc = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
    if (slices || (desc.Width & 1) || (desc.Height & 1) || desc.MipLevels != 1 || desc.SampleDesc.Count != 1 || desc.SampleDesc.Quality || (desc.MiscFlags & ~planar_misc) ||
        (bind & D3D11_BIND_DEPTH_STENCIL) || ((desc.Usage == D3D11_USAGE_DYNAMIC || desc.Usage == D3D11_USAGE_STAGING) && desc.ArraySize != 1))
      return E_INVALIDARG;
  }
  if (desc.MiscFlags & (D3D11_RESOURCE_MISC_TILED | D3D11_RESOURCE_MISC_TILE_POOL)) return E_INVALIDARG;      /* (TiledResourcesTier NOT_SUPPORTED) */
  /* (shared through NT handles: with SHARED or SHARED_KEYEDMUTEX, in GPU memory) */
  const BOOL nt_shared = !res && (desc.MiscFlags & D3D11_RESOURCE_MISC_SHARED_NTHANDLE) != 0;
  if (nt_shared && (!(desc.MiscFlags & (D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX)) || desc.Usage != D3D11_USAGE_DEFAULT || desc.CPUAccessFlags))
    return E_INVALIDARG;
  /* (shared with a legacy handle - IDXGIResource::GetSharedHandle's -: SHARED or SHARED_KEYEDMUTEX without NTHANDLE, in GPU memory too) */
  const BOOL global_shared = !res && !nt_shared && (desc.MiscFlags & (D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX)) != 0;
  if (global_shared && (desc.Usage != D3D11_USAGE_DEFAULT || desc.CPUAccessFlags)) return E_INVALIDARG;
  const UINT made_kind = d->next_tex_kind;
  d->next_tex_kind = 0;
  if ((desc.MiscFlags & D3D11_RESOURCE_MISC_GENERATE_MIPS) && (bind & (D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE)) != (D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE)) return E_INVALIDARG;
  if (desc.Usage > D3D11_USAGE_STAGING || (desc.Usage == D3D11_USAGE_STAGING && bind) || (desc.Usage == D3D11_USAGE_DYNAMIC && (desc.CPUAccessFlags != D3D11_CPU_ACCESS_WRITE || (bind & ~(UINT)D3D11_BIND_SHADER_RESOURCE))) ||
      ((desc.Usage == D3D11_USAGE_DEFAULT || desc.Usage == D3D11_USAGE_IMMUTABLE) && desc.CPUAccessFlags))
    return E_INVALIDARG;
  /* (a typed depth format is not read by shaders: a typeless one is made for that) */
  if ((bind & D3D11_BIND_SHADER_RESOURCE) && (desc.Format == DXGI_FORMAT_D16_UNORM || desc.Format == DXGI_FORMAT_D24_UNORM_S8_UINT || desc.Format == DXGI_FORMAT_D32_FLOAT ||
                                               desc.Format == DXGI_FORMAT_D32_FLOAT_S8X24_UINT))
    return E_INVALIDARG;
  if (desc.Width > 4096 || desc.Height > 4096 || slices > 256) { tex_refused(&desc, "larger than 4096 (or a volume deeper than 256)"); return E_INVALIDARG; }
  const BOOL cube = (desc.MiscFlags & D3D11_RESOURCE_MISC_TEXTURECUBE) != 0;
  if (cube && (desc.ArraySize % 6 || desc.Width != desc.Height)) return E_INVALIDARG;
  if (cube && desc.ArraySize != 6) { tex_refused(&desc, "an array of cubes"); return DXGI_ERROR_UNSUPPORTED; }
  if (desc.ArraySize > 256) { tex_refused(&desc, "an array of more than 256"); return E_INVALIDARG; }
  const BOOL layered = cube || slices || desc.ArraySize > 1;
  /* (a render target of levels - or of layers, on a device below that does not draw into them -: the kernel draws into images of one level, so the texture is drawn
   * into through targets of its own) */
  const BOOL shadowed = !res && !planar && !yuv && (desc.BindFlags & D3D11_BIND_RENDER_TARGET) && desc.SampleDesc.Count == 1 &&
                        (desc.MipLevels != 1 || (layered && !dev_layered_targets(d)));
  /* (a format that cannot be had for that use: refused as such, before what the kernel cannot do) */
  if (!tex_format_for(desc.Format, desc.BindFlags) && !(res && tex_format(desc.Format))) { tex_refused(&desc, "the format, for that use"); return E_INVALIDARG; }
  /* (a depth buffer of several slices: an array or a cube - a volume is not one -, on a device below that draws into layers) */
  if (layered && !planar && (desc.BindFlags & D3D11_BIND_DEPTH_STENCIL)) {
    if (slices) return E_INVALIDARG;
    if (!dev_layered_targets(d)) { tex_refused(&desc, cube ? "a cube to draw into" : "an array to draw into"); return DXGI_ERROR_UNSUPPORTED; }
  }
  const BOOL target = (desc.BindFlags & (D3D11_BIND_RENDER_TARGET | D3D11_BIND_DEPTH_STENCIL)) != 0;
  /* (a multisampled texture: a render target or depth buffer of one level and one layer, drawn into and resolved; the adapter says which counts it has) */
  if (desc.SampleDesc.Count == 0) return E_INVALIDARG;
  if (desc.SampleDesc.Count > 1 && (!target || desc.ArraySize != 1 || desc.CPUAccessFlags || desc.Usage != D3D11_USAGE_DEFAULT)) { tex_refused(&desc, "multisampled"); return E_INVALIDARG; }
  /* (a sample count the adapter has, and a quality below the levels it has for it) */
  if (!res && (desc.SampleDesc.Count > 1 || desc.SampleDesc.Quality)) {
    const UINT levels = dev_ms_levels(d, desc.Format, desc.SampleDesc.Count);
    if (!levels || desc.SampleDesc.Quality >= levels) return E_INVALIDARG;
  }
  UINT full = 1;
  UINT largest = desc.Width > desc.Height ? desc.Width : desc.Height;
  if (slices > largest) largest = slices;
  for (UINT size = largest; size > 1; size >>= 1) ++full;
  /* (a render target has one level here: the kernel draws into those only) */
  if ((target && !shadowed) || desc.SampleDesc.Count > 1) desc.MipLevels = 1;
  else if (desc.MipLevels == 0 || desc.MipLevels > full) desc.MipLevels = full;
  if ((desc.BindFlags & D3D11_BIND_DEPTH_STENCIL) && desc.MipLevels != 1) desc.MipLevels = 1;      /* (a depth buffer: one level) */
  Tex *t = child_new(sizeof *t, d, &tex_vtbl, g_tex_iids, tex_destroy);
  if (!t) return E_OUTOFMEMORY;
  tex_dxgi_init(t);
  t->desc = desc;
  t->kind = slices ? 1 : cube ? 3 : desc.ArraySize > 1 && !planar ? 2 : 0;      /* (a planar array: each slice an image of its own below) */
  t->shadowed = shadowed;
  t->version = 1;
  t->slices = slices ? slices : 1;
  t->kmips = layered ? 1 : desc.MipLevels;      /* (the kernel's volumes, arrays and cubes have one level: the program's others are dropped) */
  t->planar = planar ? made_format->bpp : 0;
  t->fmt = tex_format_for(desc.Format, desc.BindFlags);
  if (!t->fmt) { tex_refused(&desc, "the format, for that use"); dx_Release(OBJ(t)); return E_INVALIDARG; }
  t->stored = t->fmt->stored;
  t->bpp = t->fmt->bpp;
  t->stored_bpp = t->fmt->stored_bpp;
  t->depth = (t->fmt->flags & FMT_DEPTH) != 0;
  t->block = (t->fmt->flags & FMT_BLOCK) ? t->bpp : 0;
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
    if ((desc.BindFlags & D3D11_BIND_RENDER_TARGET) && !shadowed) rd.Flags |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    if (t->depth) rd.Flags |= D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    if (desc.BindFlags & D3D11_BIND_UNORDERED_ACCESS) rd.Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;      /* (the kernel's storage images) */
    /* (a keyed mutex that orders its use between devices: free, with key 0, as DXGI makes one; shared with the texture's NT handle, or - a legacy share - with a
     * global share handle of its own, which the texture's description carries) */
    UINT mutex_global = 0;
    if (desc.MiscFlags & D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX) {
      const NTSTATUS st = kmt_create_keyed_mutex_shared(0, &t->keyed_mutex, global_shared ? &mutex_global : NULL);
      if (!KMT_OK(st)) { dx_Release(OBJ(t)); return E_OUTOFMEMORY; }
    }
    /* (Direct3D 12 knows a cube only by its views, so the kernel has to be told now; a legacy share is one Direct3D 12 itself does not make) */
    if (cube || global_shared) TcosD3D12ResourceOptions(d->d12, (cube ? TCOS_RESOURCE_CUBE : 0u) | (global_shared ? TCOS_RESOURCE_GLOBAL_SHARE : 0u));
    if (nt_shared || global_shared) {
      /* (what a device that opens it makes of it: the program's description, which the Direct3D 12 one - formats kept wider, one level of a target - does not say) */
      BYTE blob[TEX_SHARE_BYTES];
      tex_share_blob(&t->desc, slices ? 1 : made_kind == 2 ? 2 : 0, slices, mutex_global, blob);
      TcosD3D12ShareExtra(d->d12, blob, sizeof blob);
    }
    const HRESULT hr = ID3D12Device_CreateCommittedResource(d->d12, &props, nt_shared ? D3D12_HEAP_FLAG_SHARED : D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COMMON, NULL,
                                                            &IID_ID3D12Resource, (void **)&t->res);
    if (FAILED(hr) || !t->res) { tex_refused(&desc, "Direct3D 12 below refused it"); dx_Release(OBJ(t)); return FAILED(hr) ? hr : E_FAIL; }
    if (global_shared) t->global_share = TcosD3D12GlobalShare(t->res);
  }
  *out = t;
  return S_OK;
}

/* The contents a texture is made with: every subresource (each slice of a volume's levels), in the program's order. */
static HRESULT tex_initial(Dev *d, Tex *t, const D3D11_SUBRESOURCE_DATA *initial) {
  HRESULT hr = S_OK;
  for (UINT sub = 0; sub < tex_subresources(t); ++sub) if (!initial[sub].pSysMem) return E_INVALIDARG;      /* (initial data has every subresource) */
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
  extern const ID3D11Texture3D1Vtbl tex3_vtbl;
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

/* A one-dimensional texture (or an array of them): a two-dimensional one a texel high, which is what the kernel's shaders take a texture1d as. */
static HRESULT STDMETHODCALLTYPE dev_CreateTexture1D(Dev *d, const D3D11_TEXTURE1D_DESC *desc, const D3D11_SUBRESOURCE_DATA *initial, ID3D11Texture1D **out) {
  extern const ID3D11Texture1DVtbl tex1d_vtbl;
  static const GUID *const iids[] = {&IID_ID3D11DeviceChild, &IID_ID3D11Resource, &IID_ID3D11Texture1D, NULL};
  if (out) *out = NULL;
  if (!desc || desc->Width == 0 || desc->Width > D3D11_REQ_TEXTURE1D_U_DIMENSION || desc->ArraySize == 0 || desc->ArraySize > D3D11_REQ_TEXTURE1D_ARRAY_AXIS_DIMENSION) return E_INVALIDARG;
  if (desc->MiscFlags & D3D11_RESOURCE_MISC_TEXTURECUBE) return E_INVALIDARG;
  if (initial && !initial->pSysMem) return E_INVALIDARG;
  const D3D11_TEXTURE2D_DESC as2d = {desc->Width, 1, desc->MipLevels, desc->ArraySize, desc->Format, {1, 0}, desc->Usage, desc->BindFlags, desc->CPUAccessFlags, desc->MiscFlags};
  if (!out) return S_FALSE;
  Tex *t = NULL;
  EnterCriticalSection(&d->lock);
  d->next_tex_kind = 2;      /* (for the description a share of it carries) */
  HRESULT hr = tex_make(d, &as2d, 0, NULL, &t);
  if (SUCCEEDED(hr)) {
    t->c.o.lpVtbl = (const void *)&tex1d_vtbl;
    t->c.o.iids = iids;
    t->one_d = TRUE;
    t->desc1 = *desc;
    t->desc1.MipLevels = t->desc.MipLevels;
    if (initial && !t->depth) hr = tex_initial(d, t, initial);
  }
  LeaveCriticalSection(&d->lock);
  if (FAILED(hr)) { if (t) dx_Release(OBJ(t)); return hr; }
  *out = (ID3D11Texture1D *)t;
  return S_OK;
}

/* ---- views --------------------------------------------------------------------------------------------------------------------------------------------- */

static Tex *tex_of_resource(ID3D11Resource *resource) {
  extern const ID3D11Texture3D1Vtbl tex3_vtbl;
  extern const ID3D11Texture1DVtbl tex1d_vtbl;
  if (!resource) return NULL;
  const void *v = ((DxObj *)resource)->lpVtbl;
  if (v != (const void *)&tex_vtbl && v != (const void *)&tex3_vtbl && v != (const void *)&tex1d_vtbl) return NULL;
  return (Tex *)resource;
}
static void STDMETHODCALLTYPE view_GetResource(View *v, ID3D11Resource **out) {
  if (!out) return;
  DxObj *r = v->tex ? OBJ(v->tex) : OBJ(v->buf);
  dx_AddRef(r);
  *out = (ID3D11Resource *)r;
}
/* The list that last used what a view is of. */
static UINT view_used(const View *v) { return v->tex ? v->tex->used : v->buf->used; }
static void view_release_resource(View *v) { unhold(v->tex); unhold(v->buf); }
static void rtv_shadow_destroy(Dev *d, View *v);      /* (rtshadow.inc) */
static void rtv_destroy(DxObj *o) {
  View *v = (View *)o;
  Dev *d = v->c.dev;
  if (v->scratch) { EnterCriticalSection(&d->lock); rtv_shadow_destroy(d, v); LeaveCriticalSection(&d->lock); }
  settle(d, v->tex->used);
  if (d->rtv_free_count < RTV_SLOTS) d->rtv_free[d->rtv_free_count++] = v->slot;
  unhold(v->tex);
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
  unhold(v->tex);
  child_destroy(o);
}
static void srv_destroy(DxObj *o) {
  View *v = (View *)o;
  settle(v->c.dev, view_used(v));
  view_release_resource(v);
  child_destroy(o);
}
static void uav_destroy(DxObj *o) { srv_destroy(o); }
static void STDMETHODCALLTYPE rtv_GetDesc(View *v, D3D11_RENDER_TARGET_VIEW_DESC *desc) { if (desc) *desc = v->desc.rtv; }
static void STDMETHODCALLTYPE dsv_GetDesc(View *v, D3D11_DEPTH_STENCIL_VIEW_DESC *desc) { if (desc) *desc = v->desc.dsv; }
static void STDMETHODCALLTYPE srv_GetDesc(View *v, D3D11_SHADER_RESOURCE_VIEW_DESC *desc) { if (desc) *desc = v->desc.srv; }
static void STDMETHODCALLTYPE uav_GetDesc(View *v, D3D11_UNORDERED_ACCESS_VIEW_DESC *desc) { if (desc) *desc = v->desc.uav; }

/* The bytes of an element of a typed buffer view of `format`; 0 for a format a buffer view cannot have (a block-compressed, depth or video format). */
static UINT buffer_format_bytes(DXGI_FORMAT format) {
  switch (format) {
    case DXGI_FORMAT_R32G32B32A32_TYPELESS: case DXGI_FORMAT_R32G32B32A32_FLOAT: case DXGI_FORMAT_R32G32B32A32_UINT: case DXGI_FORMAT_R32G32B32A32_SINT: return 16;
    case DXGI_FORMAT_R32G32B32_TYPELESS: case DXGI_FORMAT_R32G32B32_FLOAT: case DXGI_FORMAT_R32G32B32_UINT: case DXGI_FORMAT_R32G32B32_SINT: return 12;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: case DXGI_FORMAT_R16G16B16A16_FLOAT: case DXGI_FORMAT_R16G16B16A16_UNORM: case DXGI_FORMAT_R16G16B16A16_UINT:
    case DXGI_FORMAT_R16G16B16A16_SNORM: case DXGI_FORMAT_R16G16B16A16_SINT: case DXGI_FORMAT_R32G32_TYPELESS: case DXGI_FORMAT_R32G32_FLOAT: case DXGI_FORMAT_R32G32_UINT:
    case DXGI_FORMAT_R32G32_SINT: return 8;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS: case DXGI_FORMAT_R10G10B10A2_UNORM: case DXGI_FORMAT_R10G10B10A2_UINT: case DXGI_FORMAT_R11G11B10_FLOAT:
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_UINT: case DXGI_FORMAT_R8G8B8A8_SNORM: case DXGI_FORMAT_R8G8B8A8_SINT:
    case DXGI_FORMAT_R16G16_TYPELESS: case DXGI_FORMAT_R16G16_FLOAT: case DXGI_FORMAT_R16G16_UNORM: case DXGI_FORMAT_R16G16_UINT: case DXGI_FORMAT_R16G16_SNORM:
    case DXGI_FORMAT_R16G16_SINT: case DXGI_FORMAT_R32_TYPELESS: case DXGI_FORMAT_R32_FLOAT: case DXGI_FORMAT_R32_UINT: case DXGI_FORMAT_R32_SINT:
    case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_B8G8R8X8_UNORM: case DXGI_FORMAT_B8G8R8A8_TYPELESS: return 4;
    case DXGI_FORMAT_R8G8_TYPELESS: case DXGI_FORMAT_R8G8_UNORM: case DXGI_FORMAT_R8G8_UINT: case DXGI_FORMAT_R8G8_SNORM: case DXGI_FORMAT_R8G8_SINT:
    case DXGI_FORMAT_R16_TYPELESS: case DXGI_FORMAT_R16_FLOAT: case DXGI_FORMAT_R16_UNORM: case DXGI_FORMAT_R16_UINT: case DXGI_FORMAT_R16_SNORM: case DXGI_FORMAT_R16_SINT:
    case DXGI_FORMAT_B5G6R5_UNORM: case DXGI_FORMAT_B5G5R5A1_UNORM: case DXGI_FORMAT_B4G4R4A4_UNORM: return 2;
    case DXGI_FORMAT_R8_TYPELESS: case DXGI_FORMAT_R8_UNORM: case DXGI_FORMAT_R8_UINT: case DXGI_FORMAT_R8_SNORM: case DXGI_FORMAT_R8_SINT: case DXGI_FORMAT_A8_UNORM: return 1;
    default: return 0;
  }
}
/* The bytes a buffer view names: `first` and `count` elements; the stride by the view's kind (raw: 4, structured: the buffer's structure, typed: the format's).
 * FALSE for a view Direct3D 11 does not make of that buffer. */
static BOOL buffer_view_range(const Buf *b, DXGI_FORMAT format, BOOL raw, UINT first, UINT count, View *v) {
  const BOOL structured = (b->desc.MiscFlags & D3D11_RESOURCE_MISC_BUFFER_STRUCTURED) != 0;
  UINT stride;
  if (raw) {
    if (!(b->desc.MiscFlags & D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS) || format != DXGI_FORMAT_R32_TYPELESS) return FALSE;
    stride = 4;
  } else if (structured) {
    if (format != DXGI_FORMAT_UNKNOWN) return FALSE;
    stride = b->desc.StructureByteStride;
  } else {
    stride = buffer_format_bytes(format);
    if (!stride || format == DXGI_FORMAT_R32G32B32A32_TYPELESS || format == DXGI_FORMAT_R32G32B32_TYPELESS || format == DXGI_FORMAT_R16G16B16A16_TYPELESS ||
        format == DXGI_FORMAT_R32G32_TYPELESS || format == DXGI_FORMAT_R10G10B10A2_TYPELESS || format == DXGI_FORMAT_R8G8B8A8_TYPELESS || format == DXGI_FORMAT_R16G16_TYPELESS ||
        format == DXGI_FORMAT_R32_TYPELESS || format == DXGI_FORMAT_R8G8_TYPELESS || format == DXGI_FORMAT_R16_TYPELESS || format == DXGI_FORMAT_R8_TYPELESS ||
        format == DXGI_FORMAT_B8G8R8A8_TYPELESS) return FALSE;      /* (a typed view has a type) */
  }
  if (count == 0 || first > b->desc.ByteWidth / stride || count > b->desc.ByteWidth / stride - first) return FALSE;
  v->offset = (UINT64)first * stride;
  v->size = (UINT64)count * stride;
  v->stride = stride;
  v->format = raw || structured ? DXGI_FORMAT_UNKNOWN : format;
  return TRUE;
}

/* The plane and slice a view of a planar texture is of. Its format names the plane (R8 / R16 the luma, R8G8 / R16G16 U and V, or their UINT forms; `plane_slice`, a *1
 * description's, must be that one, -1: none given); a 2D view is of the first slice, a 2D array view of one level of its slices from the first. FALSE for any other view - of
 * the planar format itself, of no format, of a level but the first: Direct3D 11 makes a view of a planar texture of one plane. */
static BOOL planar_view(const Tex *t, DXGI_FORMAT format, INT plane_slice, BOOL array, UINT mip, UINT levels, UINT first, UINT size, UINT *plane, UINT *slice) {
  if (!plane_of_view_format(t->desc.Format, format, plane) || (plane_slice >= 0 && (UINT)plane_slice != *plane) || mip != 0 || (levels != 1 && levels != (UINT)-1)) return FALSE;
  *slice = 0;
  if (!array) return TRUE;
  if (first >= t->desc.ArraySize || size == 0 || (size != (UINT)-1 && size > t->desc.ArraySize - first)) return FALSE;
  *slice = first;
  return TRUE;
}

static BOOL rtv_subresource(const Tex *t, const D3D11_RENDER_TARGET_VIEW_DESC *desc, UINT *level, UINT *layer);
static HRESULT rtv_shadow_make(Dev *d, View *v, UINT level, UINT layer);
static HRESULT rtv_create(Dev *d, ID3D11Resource *resource, const D3D11_RENDER_TARGET_VIEW_DESC *desc, INT plane_slice, ID3D11RenderTargetView **out) {
  if (out) *out = NULL;
  Tex *t = tex_of_resource(resource);
  if (!t || !(t->desc.BindFlags & D3D11_BIND_RENDER_TARGET)) return E_INVALIDARG;
  UINT level = 0, layer = 0, plane = 0;
  if (t->planar) {
    if (!desc || (desc->ViewDimension != D3D11_RTV_DIMENSION_TEXTURE2D && desc->ViewDimension != D3D11_RTV_DIMENSION_TEXTURE2DARRAY)) return E_INVALIDARG;
    const BOOL array = desc->ViewDimension == D3D11_RTV_DIMENSION_TEXTURE2DARRAY;
    if (!planar_view(t, desc->Format, plane_slice, array, array ? desc->Texture2DArray.MipSlice : desc->Texture2D.MipSlice, 1, array ? desc->Texture2DArray.FirstArraySlice : 0,
                     array ? desc->Texture2DArray.ArraySize : 1, &plane, &layer))
      return E_INVALIDARG;
  } else {
    if (plane_slice > 0) return E_INVALIDARG;      /* (a format of one plane) */
    if ((t->fmt->flags & FMT_YUV) && (!desc || !yuv_view_ok(t->desc.Format, desc->Format, TRUE))) return E_INVALIDARG;      /* (AYUV: as R8G8B8A8) */
    if (desc && !rtv_subresource(t, desc, &level, &layer)) return E_INVALIDARG;
    if (!t->shadowed && level) return E_INVALIDARG;
    if (!t->shadowed && layer && t->kind == 0) return E_INVALIDARG;
  }
  EnterCriticalSection(&d->lock);
  if (d->rtv_free_count == 0) { LeaveCriticalSection(&d->lock); return E_OUTOFMEMORY; }
  View *v = child_new(sizeof *v, d, &rtv_vtbl, g_rtv_iids, rtv_destroy);
  if (!v) { LeaveCriticalSection(&d->lock); return E_OUTOFMEMORY; }
  v->slot = d->rtv_free[--d->rtv_free_count];
  LeaveCriticalSection(&d->lock);
  hold(t);      /* (a view's reference to its resource is the runtime's) */
  v->tex = t;
  if (desc) { v->desc.rtv = *desc; if (v->desc.rtv.Format == DXGI_FORMAT_UNKNOWN) v->desc.rtv.Format = t->desc.Format; }
  else { v->desc.rtv.Format = t->desc.Format; v->desc.rtv.ViewDimension = t->one_d ? D3D11_RTV_DIMENSION_TEXTURE1D : D3D11_RTV_DIMENSION_TEXTURE2D; }
  v->handle = ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(d->rtv_heap);
  v->handle.ptr += (SIZE_T)v->slot * ID3D12Device_GetDescriptorHandleIncrementSize(d->d12, D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
  v->width = mip_size(t->desc.Width, level);
  v->height = mip_size(t->desc.Height, level);
  if (t->planar) {
    /* (a plane of a slice: an image of its own below, the size of the plane) */
    v->plane_format = desc->Format;
    v->plane = plane;
    v->array_slice = layer;
    v->width = plane_width(t, plane);
    v->height = plane_height(t, plane);
    D3D12_RENDER_TARGET_VIEW_DESC r;
    memset(&r, 0, sizeof r);
    r.Format = desc->Format;
    if (t->desc.ArraySize > 1) {
      /* (a 2D array view of several slices: Direct3D 12 below draws into an array mirror of them, a primitive into the slice its SV_RenderTargetArrayIndex names) */
      r.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
      r.Texture2DArray.FirstArraySlice = layer;
      r.Texture2DArray.ArraySize = desc->ViewDimension == D3D11_RTV_DIMENSION_TEXTURE2DARRAY ? (desc->Texture2DArray.ArraySize == (UINT)-1 ? t->desc.ArraySize - layer : desc->Texture2DArray.ArraySize) : 1;
      r.Texture2DArray.PlaneSlice = plane;
    } else {
      r.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
      r.Texture2D.PlaneSlice = plane;
    }
    ID3D12Device_CreateRenderTargetView(d->d12, t->res, &r, v->handle);
  } else if (t->shadowed) {
    EnterCriticalSection(&d->lock);
    const HRESULT hr = rtv_shadow_make(d, v, level, layer);
    LeaveCriticalSection(&d->lock);
    if (FAILED(hr)) { dx_Release(OBJ(v)); return hr; }
    ID3D12Device_CreateRenderTargetView(d->d12, v->scratch, NULL, v->handle);
  } else if (t->kind != 0 && !(t->fmt->flags & FMT_YUV)) {
    /* (an array, a cube or a volume Direct3D 12 below draws into: a view of the layers the description names - all of them without one) */
    D3D12_RENDER_TARGET_VIEW_DESC r;
    memset(&r, 0, sizeof r);
    r.Format = t->stored;
    UINT first = 0, count = t->kind == 1 ? t->slices : t->desc.ArraySize;
    if (desc && desc->ViewDimension == D3D11_RTV_DIMENSION_TEXTURE2DARRAY) first = desc->Texture2DArray.FirstArraySlice, count = desc->Texture2DArray.ArraySize;
    if (desc && desc->ViewDimension == D3D11_RTV_DIMENSION_TEXTURE1DARRAY) first = desc->Texture1DArray.FirstArraySlice, count = desc->Texture1DArray.ArraySize;
    if (desc && desc->ViewDimension == D3D11_RTV_DIMENSION_TEXTURE3D) first = desc->Texture3D.FirstWSlice, count = desc->Texture3D.WSize;
    if (desc && (desc->ViewDimension == D3D11_RTV_DIMENSION_TEXTURE2D || desc->ViewDimension == D3D11_RTV_DIMENSION_TEXTURE1D)) count = 1;
    if (t->kind == 1) {
      r.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE3D;
      r.Texture3D.FirstWSlice = first;
      r.Texture3D.WSize = count;
    } else {
      r.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
      r.Texture2DArray.FirstArraySlice = first;
      r.Texture2DArray.ArraySize = count;
    }
    if (!desc) {
      v->desc.rtv.ViewDimension = t->kind == 1 ? D3D11_RTV_DIMENSION_TEXTURE3D : t->one_d ? D3D11_RTV_DIMENSION_TEXTURE1DARRAY : D3D11_RTV_DIMENSION_TEXTURE2DARRAY;
      if (t->kind == 1) v->desc.rtv.Texture3D.WSize = count;
      else if (t->one_d) v->desc.rtv.Texture1DArray.ArraySize = count;
      else v->desc.rtv.Texture2DArray.ArraySize = count;
    }
    ID3D12Device_CreateRenderTargetView(d->d12, t->res, &r, v->handle);
  } else if (t->desc.SampleDesc.Count > 1) {
    v->desc.rtv.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DMS;
    D3D12_RENDER_TARGET_VIEW_DESC ms = {t->stored, D3D12_RTV_DIMENSION_TEXTURE2DMS, {{0}}};
    ID3D12Device_CreateRenderTargetView(d->d12, t->res, &ms, v->handle);
  } else if (t->fmt->flags & FMT_YUV) {
    /* (AYUV: a view of its pixels' format - Direct3D 12 below makes no view of the YUV format itself; slice 0, as for the other formats here) */
    D3D12_RENDER_TARGET_VIEW_DESC r;
    memset(&r, 0, sizeof r);
    r.Format = desc->Format;
    r.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    ID3D12Device_CreateRenderTargetView(d->d12, t->res, &r, v->handle);
  } else ID3D12Device_CreateRenderTargetView(d->d12, t->res, NULL, v->handle);
  return give(v, (void **)out);
}
static HRESULT STDMETHODCALLTYPE dev_CreateRenderTargetView(Dev *d, ID3D11Resource *resource, const D3D11_RENDER_TARGET_VIEW_DESC *desc, ID3D11RenderTargetView **out) {
  return rtv_create(d, resource, desc, -1, out);
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
  hold(t);      /* (a view's reference to its resource is the runtime's) */
  v->tex = t;
  if (desc) { v->desc.dsv = *desc; if (v->desc.dsv.Format == DXGI_FORMAT_UNKNOWN) v->desc.dsv.Format = t->desc.Format; }
  else { v->desc.dsv.Format = t->desc.Format; v->desc.dsv.ViewDimension = t->one_d ? D3D11_DSV_DIMENSION_TEXTURE1D : D3D11_DSV_DIMENSION_TEXTURE2D; }
  v->handle = ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(d->dsv_heap);
  v->handle.ptr += (SIZE_T)v->slot * ID3D12Device_GetDescriptorHandleIncrementSize(d->d12, D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
  if (t->desc.SampleDesc.Count > 1) v->desc.dsv.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2DMS;
  if (t->kind == 2 || t->kind == 3) {
    /* (an array or a cube: a view of the slices the description names - all of them without one) */
    D3D12_DEPTH_STENCIL_VIEW_DESC x;
    memset(&x, 0, sizeof x);
    x.Format = t->stored;
    x.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
    x.Texture2DArray.ArraySize = t->desc.ArraySize;
    if (desc && desc->ViewDimension == D3D11_DSV_DIMENSION_TEXTURE2DARRAY) x.Texture2DArray.FirstArraySlice = desc->Texture2DArray.FirstArraySlice, x.Texture2DArray.ArraySize = desc->Texture2DArray.ArraySize;
    if (desc && desc->ViewDimension == D3D11_DSV_DIMENSION_TEXTURE1DARRAY) x.Texture2DArray.FirstArraySlice = desc->Texture1DArray.FirstArraySlice, x.Texture2DArray.ArraySize = desc->Texture1DArray.ArraySize;
    if (desc && (desc->ViewDimension == D3D11_DSV_DIMENSION_TEXTURE2D || desc->ViewDimension == D3D11_DSV_DIMENSION_TEXTURE1D)) x.Texture2DArray.ArraySize = 1;
    if (!desc) { v->desc.dsv.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2DARRAY; v->desc.dsv.Texture2DArray.ArraySize = t->desc.ArraySize; }
    ID3D12Device_CreateDepthStencilView(d->d12, t->res, &x, v->handle);
    return give(v, (void **)out);
  }
  ID3D12Device_CreateDepthStencilView(d->d12, t->res, NULL, v->handle);      /* (the view follows the resource: a multisampled one is seen as such) */
  return give(v, (void **)out);
}
static Buf *buf_of_resource(ID3D11Resource *resource);
static HRESULT srv_of_buffer(Dev *d, Buf *b, const D3D11_SHADER_RESOURCE_VIEW_DESC *desc, ID3D11ShaderResourceView **out) {
  if (!(b->desc.BindFlags & D3D11_BIND_SHADER_RESOURCE)) return E_INVALIDARG;
  D3D11_SHADER_RESOURCE_VIEW_DESC whole;
  if (!desc) {
    /* (without a description: a structured buffer, all of it) */
    if (!(b->desc.MiscFlags & D3D11_RESOURCE_MISC_BUFFER_STRUCTURED)) return E_INVALIDARG;
    memset(&whole, 0, sizeof whole);
    whole.Format = DXGI_FORMAT_UNKNOWN;
    whole.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
    whole.Buffer.FirstElement = 0;
    whole.Buffer.NumElements = b->desc.ByteWidth / b->desc.StructureByteStride;
    desc = &whole;
  }
  View probe;
  memset(&probe, 0, sizeof probe);
  if (desc->ViewDimension == D3D11_SRV_DIMENSION_BUFFER) {
    if (!buffer_view_range(b, desc->Format, FALSE, desc->Buffer.FirstElement, desc->Buffer.NumElements, &probe)) return E_INVALIDARG;
  } else if (desc->ViewDimension == D3D11_SRV_DIMENSION_BUFFEREX) {
    if ((desc->BufferEx.Flags & ~(UINT)D3D11_BUFFEREX_SRV_FLAG_RAW) ||
        !buffer_view_range(b, desc->Format, (desc->BufferEx.Flags & D3D11_BUFFEREX_SRV_FLAG_RAW) != 0, desc->BufferEx.FirstElement, desc->BufferEx.NumElements, &probe))
      return E_INVALIDARG;
  } else return E_INVALIDARG;
  if (!out) return S_FALSE;
  View *v = child_new(sizeof *v, d, &srv_vtbl, g_srv_iids, srv_destroy);
  if (!v) return E_OUTOFMEMORY;
  hold(b);
  v->buf = b;
  v->offset = probe.offset;
  v->size = probe.size;
  v->stride = probe.stride;
  v->format = probe.format;
  v->desc.srv = *desc;
  return give(v, (void **)out);
}

static HRESULT srv_create(Dev *d, ID3D11Resource *resource, const D3D11_SHADER_RESOURCE_VIEW_DESC *desc, INT plane_slice, ID3D11ShaderResourceView **out) {
  if (out) *out = NULL;
  Buf *b = buf_of_resource(resource);
  if (b) return plane_slice > 0 ? E_INVALIDARG : srv_of_buffer(d, b, desc, out);
  Tex *t = tex_of_resource(resource);
  if (!t) return E_INVALIDARG;
  if (!(t->desc.BindFlags & D3D11_BIND_SHADER_RESOURCE)) return E_INVALIDARG;      /* (a depth buffer made to be read too has one: ANGLE makes them for its depth renderbuffers) */
  UINT plane = 0, slice = 0;
  if (t->planar) {
    if (!desc || (desc->ViewDimension != D3D11_SRV_DIMENSION_TEXTURE2D && desc->ViewDimension != D3D11_SRV_DIMENSION_TEXTURE2DARRAY)) return E_INVALIDARG;
    const BOOL array = desc->ViewDimension == D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
    if (!planar_view(t, desc->Format, plane_slice, array, array ? desc->Texture2DArray.MostDetailedMip : desc->Texture2D.MostDetailedMip,
                     array ? desc->Texture2DArray.MipLevels : desc->Texture2D.MipLevels, array ? desc->Texture2DArray.FirstArraySlice : 0, array ? desc->Texture2DArray.ArraySize : 1,
                     &plane, &slice))
      return E_INVALIDARG;
  } else if (plane_slice > 0) return E_INVALIDARG;      /* (a format of one plane) */
  if ((t->fmt->flags & FMT_YUV) && (!desc || !yuv_view_ok(t->desc.Format, desc->Format, FALSE))) return E_INVALIDARG;      /* (packed YUV: a view of its pixels' format) */
  if (!out) return S_FALSE;
  View *v = child_new(sizeof *v, d, &srv_vtbl, g_srv_iids, srv_destroy);
  if (!v) return E_OUTOFMEMORY;
  hold(t);      /* (a view's reference to its resource is the runtime's) */
  v->tex = t;
  if (t->planar) {
    v->plane_format = desc->Format;
    v->plane = plane;
    v->array_slice = slice;
    /* (a 2D array view: its slices, all from the first when the size is -1) */
    if (desc->ViewDimension == D3D11_SRV_DIMENSION_TEXTURE2DARRAY)
      v->array_size = desc->Texture2DArray.ArraySize == (UINT)-1 ? t->desc.ArraySize - slice : desc->Texture2DArray.ArraySize;
  }
  if (desc) v->desc.srv = *desc;
  else {
    /* (the whole texture, as what it is) */
    v->desc.srv.Format = t->desc.Format;
    if (t->one_d) {
      if (t->desc.ArraySize > 1) { v->desc.srv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE1DARRAY; v->desc.srv.Texture1DArray.MipLevels = t->desc.MipLevels; v->desc.srv.Texture1DArray.ArraySize = t->desc.ArraySize; }
      else { v->desc.srv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE1D; v->desc.srv.Texture1D.MipLevels = t->desc.MipLevels; }
    } else switch (t->kind) {
      case 1: v->desc.srv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE3D; v->desc.srv.Texture3D.MipLevels = t->desc.MipLevels; break;
      case 2: v->desc.srv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY; v->desc.srv.Texture2DArray.MipLevels = t->desc.MipLevels; v->desc.srv.Texture2DArray.ArraySize = t->desc.ArraySize; break;
      case 3: v->desc.srv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURECUBE; v->desc.srv.TextureCube.MipLevels = t->desc.MipLevels; break;
      default: v->desc.srv.ViewDimension = t->desc.SampleDesc.Count > 1 ? D3D11_SRV_DIMENSION_TEXTURE2DMS : D3D11_SRV_DIMENSION_TEXTURE2D; v->desc.srv.Texture2D.MipLevels = t->desc.MipLevels; break;
    }
  }
  return give(v, (void **)out);
}
static HRESULT STDMETHODCALLTYPE dev_CreateShaderResourceView(Dev *d, ID3D11Resource *resource, const D3D11_SHADER_RESOURCE_VIEW_DESC *desc, ID3D11ShaderResourceView **out) {
  return srv_create(d, resource, desc, -1, out);
}

/* An unordered access view. Of a buffer: raw (R32_TYPELESS, a buffer that allows raw views), structured (UNKNOWN; an append or counter view has a hidden count), or
 * typed. Of a texture: a level and layers of it (the kernel's storage images: context.inc texture_uav_descriptor), a plane of the slices of a planar one. */
static HRESULT uav_create(Dev *d, ID3D11Resource *resource, const D3D11_UNORDERED_ACCESS_VIEW_DESC *desc, INT plane_slice, ID3D11UnorderedAccessView **out) {
  extern const ID3D11UnorderedAccessView1Vtbl uav_vtbl;
  static const GUID *const iids[] = {&IID_ID3D11DeviceChild, &IID_ID3D11View, &IID_ID3D11UnorderedAccessView, &IID_ID3D11UnorderedAccessView1, NULL};
  if (out) *out = NULL;
  Buf *b = buf_of_resource(resource);
  Tex *t = tex_of_resource(resource);
  if (!b && !t) return E_INVALIDARG;
  View probe;
  memset(&probe, 0, sizeof probe);
  D3D11_UNORDERED_ACCESS_VIEW_DESC whole;
  memset(&whole, 0, sizeof whole);
  if (b) {
    if (!(b->desc.BindFlags & D3D11_BIND_UNORDERED_ACCESS)) return E_INVALIDARG;
    if (!desc) {
      if (!(b->desc.MiscFlags & D3D11_RESOURCE_MISC_BUFFER_STRUCTURED)) return E_INVALIDARG;
      whole.Format = DXGI_FORMAT_UNKNOWN;
      whole.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
      whole.Buffer.NumElements = b->desc.ByteWidth / b->desc.StructureByteStride;
      desc = &whole;
    }
    const UINT flags = desc->Buffer.Flags;
    if (desc->ViewDimension != D3D11_UAV_DIMENSION_BUFFER || (flags & ~(UINT)(D3D11_BUFFER_UAV_FLAG_RAW | D3D11_BUFFER_UAV_FLAG_APPEND | D3D11_BUFFER_UAV_FLAG_COUNTER))) return E_INVALIDARG;
    if ((flags & D3D11_BUFFER_UAV_FLAG_APPEND) && (flags & D3D11_BUFFER_UAV_FLAG_COUNTER)) return E_INVALIDARG;
    if ((flags & (D3D11_BUFFER_UAV_FLAG_APPEND | D3D11_BUFFER_UAV_FLAG_COUNTER)) && !(b->desc.MiscFlags & D3D11_RESOURCE_MISC_BUFFER_STRUCTURED)) return E_INVALIDARG;
    if (!buffer_view_range(b, desc->Format, (flags & D3D11_BUFFER_UAV_FLAG_RAW) != 0, desc->Buffer.FirstElement, desc->Buffer.NumElements, &probe)) return E_INVALIDARG;
  } else if (t->planar) {
    /* (a plane of a slice, as the other views of a planar texture) */
    if (!(t->desc.BindFlags & D3D11_BIND_UNORDERED_ACCESS) || !desc || (desc->ViewDimension != D3D11_UAV_DIMENSION_TEXTURE2D && desc->ViewDimension != D3D11_UAV_DIMENSION_TEXTURE2DARRAY))
      return E_INVALIDARG;
    const BOOL array = desc->ViewDimension == D3D11_UAV_DIMENSION_TEXTURE2DARRAY;
    UINT plane = 0, slice = 0;
    if (!planar_view(t, desc->Format, plane_slice, array, array ? desc->Texture2DArray.MipSlice : desc->Texture2D.MipSlice, 1, array ? desc->Texture2DArray.FirstArraySlice : 0,
                     array ? desc->Texture2DArray.ArraySize : 1, &plane, &slice))
      return E_INVALIDARG;
    /* (written through a mirror of the plane below that gives the view's channels back after each dispatch or pass: d3d12.dll texture_uav) */
    probe.plane_format = desc->Format;
    probe.plane = plane;
    probe.array_slice = slice;
  } else {
    if (plane_slice > 0) return E_INVALIDARG;      /* (a format of one plane) */
    if (!(t->desc.BindFlags & D3D11_BIND_UNORDERED_ACCESS)) return E_INVALIDARG;
    if ((t->fmt->flags & FMT_YUV) && (!desc || !yuv_view_ok(t->desc.Format, desc->Format, FALSE))) return E_INVALIDARG;      /* (packed YUV: a view of its pixels' format) */
    if (!desc) {
      whole.Format = t->desc.Format;
      whole.ViewDimension = t->one_d ? (t->desc.ArraySize > 1 ? D3D11_UAV_DIMENSION_TEXTURE1DARRAY : D3D11_UAV_DIMENSION_TEXTURE1D)
                          : t->kind == 1 ? D3D11_UAV_DIMENSION_TEXTURE3D : t->desc.ArraySize > 1 ? D3D11_UAV_DIMENSION_TEXTURE2DARRAY : D3D11_UAV_DIMENSION_TEXTURE2D;
      if (whole.ViewDimension == D3D11_UAV_DIMENSION_TEXTURE1DARRAY) whole.Texture1DArray.ArraySize = t->desc.ArraySize;
      if (whole.ViewDimension == D3D11_UAV_DIMENSION_TEXTURE2DARRAY) whole.Texture2DArray.ArraySize = t->desc.ArraySize;
      if (whole.ViewDimension == D3D11_UAV_DIMENSION_TEXTURE3D) whole.Texture3D.WSize = t->slices;
      desc = &whole;
    }
    if (desc->ViewDimension == D3D11_UAV_DIMENSION_BUFFER || desc->ViewDimension == D3D11_UAV_DIMENSION_UNKNOWN) return E_INVALIDARG;
    if (desc->ViewDimension == D3D11_UAV_DIMENSION_TEXTURE2D && desc->Texture2D.MipSlice >= t->desc.MipLevels) return E_INVALIDARG;
  }
  if (!out) return S_FALSE;
  View *v = child_new(sizeof *v, d, &uav_vtbl, iids, uav_destroy);
  if (!v) return E_OUTOFMEMORY;
  if (b) { hold(b); v->buf = b; }
  else { hold(t); v->tex = t; }
  v->offset = probe.offset;
  v->size = probe.size;
  v->stride = probe.stride;
  v->format = probe.format;
  v->plane_format = probe.plane_format;
  v->plane = probe.plane;
  v->array_slice = probe.array_slice;
  v->desc.uav = *desc;
  return give(v, (void **)out);
}
static HRESULT STDMETHODCALLTYPE dev_CreateUnorderedAccessView(Dev *d, ID3D11Resource *resource, const D3D11_UNORDERED_ACCESS_VIEW_DESC *desc, ID3D11UnorderedAccessView **out) {
  return uav_create(d, resource, desc, -1, out);
}

/* ---- shaders, layouts and states ------------------------------------------------------------------------------------------------------------------- */

static void shader_destroy(DxObj *o) {
  Shader *s = (Shader *)o;
  if (s->compute) { settle(s->c.dev, s->used); ID3D12PipelineState_Release(s->compute); }
  mem_free(s->so);
  mem_free(s->so_names);
  mem_free(s->code);
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
    UINT version;
    memcpy(&version, code, 4);
    s->program_type = version >> 16;
    for (UINT w = 2; w < words;) {
      UINT token, length;
      memcpy(&token, code + 4 * w, 4);
      const UINT op = token & 0x7FF;
      length = (token >> 24) & 0x7F;
      if (op == 53 /* customdata */) { if (w + 1 >= words) return; memcpy(&length, code + 4 * (w + 1), 4); }
      if (length == 0 || length > words - w) return;
      /* dcl_resource (the dimension in the token), dcl_resource_raw / _structured, dcl_uav_typed / _raw / _structured: the operand, then its register */
      if ((op == 88 || op == 161 || op == 162 || op == 156 || op == 157 || op == 158) && length >= 3) {
        UINT operand, slot;
        const UINT dimension = (token >> 11) & 0x1F;
        memcpy(&operand, code + 4 * (w + 1), 4);
        const UINT k = w + ((operand >> 31) ? 3 : 2);
        if (k < w + length && ((operand >> 20) & 3) == 1) {      /* (one immediate index: SM 4 / 5.0) */
          memcpy(&slot, code + 4 * k, 4);
          if (op >= 156 && op <= 158) { if (slot < UAV_SLOTS) s->uav_mask |= 1u << slot; }
          else if (slot < TEXTURE_SLOTS) s->tex_kind[slot] = op != 88 || dimension == 1 ? KIND_BUFFER : dimension == 5 ? 1 : (dimension == 8 || dimension == 7) ? 2 : dimension == 6 ? 3 : 0;
        }
      }
      if (op == 155 /* dcl_thread_group x, y, z */ && length >= 4) for (UINT k = 0; k < 3; ++k) memcpy(&s->group[k], code + 4 * (w + 1 + k), 4);
      if (op == 206 /* dcl_gs_instance_count n */ && length >= 2) memcpy(&s->gs_instances, code + 4 * (w + 1), 4);
      if (op == 92 /* dcl_output_topology: the topology in the token (1 points, 2 lines, 3 line strip, 4 triangles, 5 triangle strip) */) {
        const UINT t = (token >> 11) & 0x7F;
        s->out_vertices = t == 1 ? 1 : (t == 2 || t == 3) ? 2 : 3;
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
/* A geometry shader whose outputs go into stream output buffers, or the code of a vertex or domain shader whose outputs do (there is then no geometry stage). */
static HRESULT STDMETHODCALLTYPE dev_CreateGeometryShaderWithStreamOutput(Dev *d, const void *code, SIZE_T bytes, const D3D11_SO_DECLARATION_ENTRY *entries, UINT count,
                                                                         const UINT *strides, UINT stride_count, UINT rasterized, ID3D11ClassLinkage *linkage,
                                                                         ID3D11GeometryShader **out) {
  (void)linkage;
  if (out) *out = NULL;
  if (!entries || count == 0 || count > D3D11_SO_STREAM_COUNT * D3D11_SO_OUTPUT_COMPONENT_COUNT || stride_count > D3D11_SO_BUFFER_SLOT_COUNT || (stride_count && !strides)) return E_INVALIDARG;
  if (rasterized != D3D11_SO_NO_RASTERIZED_STREAM && rasterized >= D3D11_SO_STREAM_COUNT) return E_INVALIDARG;
  for (UINT i = 0; i < stride_count; ++i) if (strides[i] > 2048 || strides[i] % 4) return E_INVALIDARG;
  UINT components[4] = {0, 0, 0, 0}, stream_of_slot[4] = {~0u, ~0u, ~0u, ~0u};
  for (UINT i = 0; i < count; ++i) {
    const D3D11_SO_DECLARATION_ENTRY *e = &entries[i];
    if (e->Stream >= D3D11_SO_STREAM_COUNT || e->OutputSlot >= D3D11_SO_BUFFER_SLOT_COUNT || e->ComponentCount == 0 || e->ComponentCount > 4 || e->StartComponent > 3 ||
        e->StartComponent + e->ComponentCount > 4)
      return E_INVALIDARG;
    if (stream_of_slot[e->OutputSlot] != ~0u && stream_of_slot[e->OutputSlot] != e->Stream) return E_INVALIDARG;      /* (a buffer has one stream's records) */
    stream_of_slot[e->OutputSlot] = e->Stream;
    components[e->OutputSlot] += e->ComponentCount;
  }
  for (UINT slot = 0; slot < 4; ++slot)
    if (components[slot] && slot < stride_count && strides[slot] && strides[slot] < components[slot] * 4) return E_INVALIDARG;      /* (a record wider than its stride) */
  Shader *s = NULL;
  HRESULT hr = shader_make(d, code, bytes, &gs_vtbl, g_gs_iids, (void **)&s);
  if (FAILED(hr)) return hr;
  if (s->program_type != 1 && s->program_type != 2 && s->program_type != 4) { dx_Release(OBJ(s)); return E_INVALIDARG; }
  s->so_only = s->program_type != 2;
  if (s->so_only && rasterized != D3D11_SO_NO_RASTERIZED_STREAM && rasterized != 0) { dx_Release(OBJ(s)); return E_INVALIDARG; }      /* (those stages have stream 0 only) */
  s->so = mem_alloc(sizeof *s->so * count);
  s->so_names = mem_alloc(sizeof *s->so_names * count);
  if (!s->so || !s->so_names) { dx_Release(OBJ(s)); return E_OUTOFMEMORY; }
  for (UINT i = 0; i < count; ++i) {
    s->so[i] = entries[i];
    s->so_names[i][0] = 0;
    if (entries[i].SemanticName) {
      SIZE_T n = 0;
      while (entries[i].SemanticName[n] && n + 1 < sizeof s->so_names[i]) { s->so_names[i][n] = entries[i].SemanticName[n]; ++n; }
      s->so_names[i][n] = 0;
      s->so[i].SemanticName = s->so_names[i];
    }
  }
  s->so_count = count;
  s->so_stride_count = stride_count;
  for (UINT i = 0; i < stride_count; ++i) s->so_strides[i] = strides[i];
  for (UINT slot = 0; slot < 4; ++slot) if (components[slot] && (slot >= stride_count || !s->so_strides[slot])) s->so_strides[slot] = components[slot] * 4;      /* (a stride not given: the record) */
  if (stride_count < 4) for (UINT slot = stride_count; slot < 4; ++slot) if (s->so_strides[slot]) s->so_stride_count = slot + 1;
  s->so_rasterized = rasterized;
  return give(s, (void **)out);
}

/* ---- class linkage ----------------------------------------------------------------------------------------------------------------------------------- */
/* Dynamic shader linkage: a linkage object, and the class instances made from it. The kernel has no dynamic linkage (interfaces and fcall), so a shader that
 * declares class instances is not run, and no instance of one is known to GetClassInstance (docs/Direct3D11.md). */
typedef struct Linkage { Child c; } Linkage;
typedef struct ClassInst {
  Child c;
  Linkage *linkage;            /* a reference */
  D3D11_CLASS_INSTANCE_DESC desc;
  char type_name[256];
  char instance_name[256];
} ClassInst;
static HRESULT STDMETHODCALLTYPE dev_CreateClassLinkage(Dev *d, ID3D11ClassLinkage **out) {
  extern const ID3D11ClassLinkageVtbl link_vtbl;
  static const GUID *const iids[] = {&IID_ID3D11DeviceChild, &IID_ID3D11ClassLinkage, NULL};
  if (!out) return E_INVALIDARG;
  *out = NULL;
  Linkage *l = child_new(sizeof *l, d, &link_vtbl, iids, child_destroy);
  if (!l) return E_OUTOFMEMORY;
  *out = (ID3D11ClassLinkage *)l;
  return S_OK;
}
static void inst_destroy(DxObj *o) {
  ClassInst *i = (ClassInst *)o;
  unhold(i->linkage);
  child_destroy(o);
}
/* A string into a buffer of `size` (`*length` in: its room, out: the length with the NUL; no buffer: only the length). */
static void name_out(const char *name, LPSTR buffer, SIZE_T *length) {
  if (!length) return;
  SIZE_T n = 0;
  while (name[n]) ++n;
  if (buffer && *length) {
    const SIZE_T copied = n + 1 <= *length ? n : *length - 1;
    memcpy(buffer, name, copied);
    buffer[copied] = 0;
  }
  *length = n + 1;
}
static void STDMETHODCALLTYPE inst_GetClassLinkage(ClassInst *i, ID3D11ClassLinkage **out) { if (!out) return; dx_AddRef(OBJ(i->linkage)); *out = (ID3D11ClassLinkage *)i->linkage; }
static void STDMETHODCALLTYPE inst_GetDesc(ClassInst *i, D3D11_CLASS_INSTANCE_DESC *desc) { if (desc) *desc = i->desc; }
static void STDMETHODCALLTYPE inst_GetInstanceName(ClassInst *i, LPSTR name, SIZE_T *length) { name_out(i->instance_name, name, length); }
static void STDMETHODCALLTYPE inst_GetTypeName(ClassInst *i, LPSTR name, SIZE_T *length) { name_out(i->type_name, name, length); }
/* An instance that is not a shader's variable: the type, and where its data is (the constant buffer, the offset in it in vectors, the first texture and sampler).
 * Its instance name is empty, and it counts as made (Created). */
static HRESULT STDMETHODCALLTYPE link_CreateClassInstance(Linkage *l, LPCSTR type_name, UINT cb, UINT cb_offset, UINT texture, UINT sampler, ID3D11ClassInstance **out) {
  extern const ID3D11ClassInstanceVtbl inst_vtbl;
  static const GUID *const iids[] = {&IID_ID3D11DeviceChild, &IID_ID3D11ClassInstance, NULL};
  if (out) *out = NULL;
  if (!type_name || !out) return E_INVALIDARG;
  SIZE_T n = 0;
  while (type_name[n]) ++n;
  if (n == 0 || n >= sizeof ((ClassInst *)0)->type_name) return E_INVALIDARG;
  ClassInst *i = child_new(sizeof *i, l->c.dev, &inst_vtbl, iids, inst_destroy);
  if (!i) return E_OUTOFMEMORY;
  hold(l);
  i->linkage = l;
  memcpy(i->type_name, type_name, n + 1);
  i->instance_name[0] = 0;
  i->desc.InstanceId = 0;
  i->desc.InstanceIndex = 0;
  i->desc.TypeId = 0;
  i->desc.ConstantBuffer = cb;
  i->desc.BaseConstantBufferOffset = cb_offset;
  i->desc.BaseTexture = texture;
  i->desc.BaseSampler = sampler;
  i->desc.Created = TRUE;
  *out = (ID3D11ClassInstance *)i;
  return S_OK;
}
static HRESULT STDMETHODCALLTYPE link_GetClassInstance(Linkage *l, LPCSTR name, UINT index, ID3D11ClassInstance **out) {
  (void)l; (void)index;
  if (out) *out = NULL;
  if (!name || !out) return E_INVALIDARG;
  return E_INVALIDARG;      /* (no shader made with the linkage has class instances this device runs: see above) */
}

/* (a compute shader's Direct3D 12 pipeline is made at its first Dispatch: ctx_compute_prepare) */
static HRESULT STDMETHODCALLTYPE dev_CreateComputeShader(Dev *d, const void *code, SIZE_T bytes, ID3D11ClassLinkage *linkage, ID3D11ComputeShader **out) {
  extern const ID3D11ComputeShaderVtbl cs_vtbl;
  static const GUID *const iids[] = {&IID_ID3D11DeviceChild, &IID_ID3D11ComputeShader, NULL};
  (void)linkage;
  return shader_make(d, code, bytes, &cs_vtbl, iids, (void **)out);
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
static void STDMETHODCALLTYPE blend_GetDesc1(Blend *s, D3D11_BLEND_DESC1 *desc) { if (desc) *desc = s->desc1; }
static void STDMETHODCALLTYPE depth_GetDesc(Depth *s, D3D11_DEPTH_STENCIL_DESC *desc) { if (desc) *desc = s->desc; }
static void STDMETHODCALLTYPE raster_GetDesc(Raster *s, D3D11_RASTERIZER_DESC *desc) { if (desc) *desc = s->desc; }
static void STDMETHODCALLTYPE raster_GetDesc1(Raster *s, D3D11_RASTERIZER_DESC1 *desc) { if (desc) *desc = s->desc1; }
static void STDMETHODCALLTYPE sampler_GetDesc(Sampler *s, D3D11_SAMPLER_DESC *desc) { if (desc) *desc = s->desc; }

/* A state object leaves its device's list as it goes. */
static void state_obj_destroy(DxObj *o) {
  StateObj *s = (StateObj *)o;
  Dev *d = s->c.dev;
  EnterCriticalSection(&d->lock);
  for (StateObj **at = &d->states[s->kind]; *at; at = &(*at)->next)
    if (*at == s) { *at = s->next; break; }
  LeaveCriticalSection(&d->lock);
  child_destroy(o);
}
/* The object of `kind` whose description (`bytes` at `offset` of it) is `key`, with a new reference; NULL when there is none (or it is just going away). */
static StateObj *state_obj_find(Dev *d, UINT kind, SIZE_T offset, const void *key, SIZE_T bytes) {
  for (StateObj *s = d->states[kind]; s; s = s->next) {
    if (memcmp((const BYTE *)s + offset, key, bytes) != 0) continue;
    LONG n = s->c.o.ref;
    while (n > 0) {      /* (a reference only while it still has one: its last may be going on another thread) */
      const LONG seen = InterlockedCompareExchange(&s->c.o.ref, n + 1, n);
      if (seen == n) return s;
      n = seen;
    }
  }
  return NULL;
}
static void *state_obj_new(Dev *d, SIZE_T size, UINT kind, const void *vtbl, const GUID *const *iids) {
  StateObj *s = child_new(size, d, vtbl, iids, state_obj_destroy);
  if (!s) return NULL;
  s->kind = kind;
  s->next = d->states[kind];
  d->states[kind] = s;
  return s;
}

static BOOL blend_valid(D3D11_BLEND b, BOOL alpha) {
  if (b < D3D11_BLEND_ZERO || b > D3D11_BLEND_INV_SRC1_ALPHA || b == 12 || b == 13) return FALSE;
  return !alpha || (b != D3D11_BLEND_SRC_COLOR && b != D3D11_BLEND_INV_SRC_COLOR && b != D3D11_BLEND_DEST_COLOR && b != D3D11_BLEND_INV_DEST_COLOR &&
                    b != D3D11_BLEND_SRC1_COLOR && b != D3D11_BLEND_INV_SRC1_COLOR);
}
/* CreateBlendState1: every target as the description says (all as the first without IndependentBlendEnable); a target that does not blend has the default
 * factors. Logic operations need D3D11_FEATURE_DATA_D3D11_OPTIONS.OutputMergerLogicOp, which this device does not have. */
static HRESULT STDMETHODCALLTYPE dev_CreateBlendState1(Dev *d, const D3D11_BLEND_DESC1 *desc, ID3D11BlendState1 **out) {
  extern const ID3D11BlendState1Vtbl blend_vtbl;
  static const GUID *const iids[] = {&IID_ID3D11DeviceChild, &IID_ID3D11BlendState, &IID_ID3D11BlendState1, NULL};
  if (out) *out = NULL;
  if (!desc) return E_INVALIDARG;
  D3D11_BLEND_DESC1 key;
  memset(&key, 0, sizeof key);
  key.AlphaToCoverageEnable = !!desc->AlphaToCoverageEnable;
  key.IndependentBlendEnable = !!desc->IndependentBlendEnable;
  for (UINT i = 0; i < 8; ++i) {
    const D3D11_RENDER_TARGET_BLEND_DESC1 *in = &desc->RenderTarget[desc->IndependentBlendEnable ? i : 0];
    D3D11_RENDER_TARGET_BLEND_DESC1 *t = &key.RenderTarget[i];
    if (in->BlendEnable && in->LogicOpEnable) return E_INVALIDARG;
    if (in->LogicOpEnable) return E_INVALIDARG;      /* (no logic operations: OutputMergerLogicOp is FALSE) */
    if (in->BlendEnable) {
      if (!blend_valid(in->SrcBlend, FALSE) || !blend_valid(in->DestBlend, FALSE) || !blend_valid(in->SrcBlendAlpha, TRUE) || !blend_valid(in->DestBlendAlpha, TRUE) ||
          in->BlendOp < D3D11_BLEND_OP_ADD || in->BlendOp > D3D11_BLEND_OP_MAX || in->BlendOpAlpha < D3D11_BLEND_OP_ADD || in->BlendOpAlpha > D3D11_BLEND_OP_MAX)
        return E_INVALIDARG;
      *t = *in;
      t->BlendEnable = TRUE;
    } else {
      t->SrcBlend = t->SrcBlendAlpha = D3D11_BLEND_ONE;
      t->DestBlend = t->DestBlendAlpha = D3D11_BLEND_ZERO;
      t->BlendOp = t->BlendOpAlpha = D3D11_BLEND_OP_ADD;
    }
    t->LogicOpEnable = FALSE;
    t->LogicOp = D3D11_LOGIC_OP_NOOP;
    if (in->RenderTargetWriteMask > D3D11_COLOR_WRITE_ENABLE_ALL) return E_INVALIDARG;
    t->RenderTargetWriteMask = in->RenderTargetWriteMask;
  }
  if (!out) return S_FALSE;
  EnterCriticalSection(&d->lock);
  Blend *s = (Blend *)state_obj_find(d, STATE_BLEND, offsetof(Blend, desc1), &key, sizeof key);
  if (!s && (s = state_obj_new(d, sizeof *s, STATE_BLEND, &blend_vtbl, iids)) != NULL) {
    s->desc1 = key;
    s->desc.AlphaToCoverageEnable = key.AlphaToCoverageEnable;
    s->desc.IndependentBlendEnable = key.IndependentBlendEnable;
    for (UINT i = 0; i < 8; ++i) {
      const D3D11_RENDER_TARGET_BLEND_DESC1 *t = &key.RenderTarget[i];
      const D3D11_RENDER_TARGET_BLEND_DESC plain = {t->BlendEnable, t->SrcBlend, t->DestBlend, t->BlendOp, t->SrcBlendAlpha, t->DestBlendAlpha, t->BlendOpAlpha, t->RenderTargetWriteMask};
      s->desc.RenderTarget[i] = plain;
    }
  }
  LeaveCriticalSection(&d->lock);
  if (!s) return E_OUTOFMEMORY;
  *out = (ID3D11BlendState1 *)s;
  return S_OK;
}
static HRESULT STDMETHODCALLTYPE dev_CreateBlendState(Dev *d, const D3D11_BLEND_DESC *desc, ID3D11BlendState **out) {
  if (out) *out = NULL;
  if (!desc) return E_INVALIDARG;
  D3D11_BLEND_DESC1 d1;
  memset(&d1, 0, sizeof d1);
  d1.AlphaToCoverageEnable = desc->AlphaToCoverageEnable;
  d1.IndependentBlendEnable = desc->IndependentBlendEnable;
  for (UINT i = 0; i < 8; ++i) {
    const D3D11_RENDER_TARGET_BLEND_DESC *t = &desc->RenderTarget[i];
    const D3D11_RENDER_TARGET_BLEND_DESC1 wide = {t->BlendEnable, FALSE, t->SrcBlend, t->DestBlend, t->BlendOp, t->SrcBlendAlpha, t->DestBlendAlpha, t->BlendOpAlpha, D3D11_LOGIC_OP_NOOP,
                                                  t->RenderTargetWriteMask};
    d1.RenderTarget[i] = wide;
  }
  return dev_CreateBlendState1(d, &d1, (ID3D11BlendState1 **)out);
}
static BOOL comparison_valid(D3D11_COMPARISON_FUNC f) { return f >= D3D11_COMPARISON_NEVER && f <= D3D11_COMPARISON_ALWAYS; }
static BOOL stencil_op_valid(D3D11_STENCIL_OP o) { return o >= D3D11_STENCIL_OP_KEEP && o <= D3D11_STENCIL_OP_DECR; }
static HRESULT STDMETHODCALLTYPE dev_CreateDepthStencilState(Dev *d, const D3D11_DEPTH_STENCIL_DESC *desc, ID3D11DepthStencilState **out) {
  if (out) *out = NULL;
  if (!desc) return E_INVALIDARG;
  D3D11_DEPTH_STENCIL_DESC key = *desc;
  key.DepthEnable = !!key.DepthEnable;
  key.StencilEnable = !!key.StencilEnable;
  /* (only what applies is checked: the depth fields with the depth test, the stencil fields with the stencil test) */
  if (key.DepthEnable && (key.DepthWriteMask > D3D11_DEPTH_WRITE_MASK_ALL || !comparison_valid(key.DepthFunc))) return E_INVALIDARG;
  const D3D11_DEPTH_STENCILOP_DESC *faces[2] = {&key.FrontFace, &key.BackFace};
  for (UINT i = 0; key.StencilEnable && i < 2; ++i)
    if (!stencil_op_valid(faces[i]->StencilFailOp) || !stencil_op_valid(faces[i]->StencilDepthFailOp) || !stencil_op_valid(faces[i]->StencilPassOp) || !comparison_valid(faces[i]->StencilFunc))
      return E_INVALIDARG;
  /* (what does not apply is in its usual form: without the depth test, writing all and LESS; without stencil, the masks 0xFF and every operation KEEP, ALWAYS) */
  if (!key.DepthEnable) { key.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL; key.DepthFunc = D3D11_COMPARISON_LESS; }
  if (!key.StencilEnable) {
    static const D3D11_DEPTH_STENCILOP_DESC keep = {D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP, D3D11_COMPARISON_ALWAYS};
    key.StencilReadMask = key.StencilWriteMask = 0xFF;
    key.FrontFace = key.BackFace = keep;
  }
  if (!out) return S_FALSE;
  extern const ID3D11DepthStencilStateVtbl depth_vtbl;
  EnterCriticalSection(&d->lock);
  Depth *s = (Depth *)state_obj_find(d, STATE_DEPTH, offsetof(Depth, desc), &key, sizeof key);
  if (!s && (s = state_obj_new(d, sizeof *s, STATE_DEPTH, &depth_vtbl, g_depth_iids)) != NULL) s->desc = key;
  LeaveCriticalSection(&d->lock);
  if (!s) return E_OUTOFMEMORY;
  *out = (ID3D11DepthStencilState *)s;
  return S_OK;
}
/* CreateRasterizerState1: ForcedSampleCount is a feature of level 11_1; at 11_0 it must be 0. */
static HRESULT STDMETHODCALLTYPE dev_CreateRasterizerState1(Dev *d, const D3D11_RASTERIZER_DESC1 *desc, ID3D11RasterizerState1 **out) {
  extern const ID3D11RasterizerState2Vtbl raster_vtbl;
  static const GUID *const iids[] = {&IID_ID3D11DeviceChild, &IID_ID3D11RasterizerState, &IID_ID3D11RasterizerState1, &IID_ID3D11RasterizerState2, NULL};
  if (out) *out = NULL;
  if (!desc) return E_INVALIDARG;
  if (desc->FillMode < D3D11_FILL_WIREFRAME || desc->FillMode > D3D11_FILL_SOLID || desc->CullMode < D3D11_CULL_NONE || desc->CullMode > D3D11_CULL_BACK || desc->ForcedSampleCount)
    return E_INVALIDARG;
  D3D11_RASTERIZER_DESC1 key = *desc;
  key.FrontCounterClockwise = !!key.FrontCounterClockwise;
  key.DepthClipEnable = !!key.DepthClipEnable;
  key.ScissorEnable = !!key.ScissorEnable;
  key.MultisampleEnable = !!key.MultisampleEnable;
  key.AntialiasedLineEnable = !!key.AntialiasedLineEnable;
  if (!out) return S_FALSE;
  EnterCriticalSection(&d->lock);
  Raster *s = (Raster *)state_obj_find(d, STATE_RASTER, offsetof(Raster, desc1), &key, sizeof key);
  if (!s && (s = state_obj_new(d, sizeof *s, STATE_RASTER, &raster_vtbl, iids)) != NULL) {
    s->desc1 = key;
    const D3D11_RASTERIZER_DESC plain = {key.FillMode, key.CullMode, key.FrontCounterClockwise, key.DepthBias, key.DepthBiasClamp, key.SlopeScaledDepthBias, key.DepthClipEnable,
                                         key.ScissorEnable, key.MultisampleEnable, key.AntialiasedLineEnable};
    s->desc = plain;
  }
  LeaveCriticalSection(&d->lock);
  if (!s) return E_OUTOFMEMORY;
  *out = (ID3D11RasterizerState1 *)s;
  return S_OK;
}
static HRESULT STDMETHODCALLTYPE dev_CreateRasterizerState(Dev *d, const D3D11_RASTERIZER_DESC *desc, ID3D11RasterizerState **out) {
  if (out) *out = NULL;
  if (!desc) return E_INVALIDARG;
  const D3D11_RASTERIZER_DESC1 d1 = {desc->FillMode, desc->CullMode, desc->FrontCounterClockwise, desc->DepthBias, desc->DepthBiasClamp, desc->SlopeScaledDepthBias,
                                     desc->DepthClipEnable, desc->ScissorEnable, desc->MultisampleEnable, desc->AntialiasedLineEnable, 0};
  return dev_CreateRasterizerState1(d, &d1, (ID3D11RasterizerState1 **)out);
}
static BOOL address_valid(D3D11_TEXTURE_ADDRESS_MODE m) { return m >= D3D11_TEXTURE_ADDRESS_WRAP && m <= D3D11_TEXTURE_ADDRESS_MIRROR_ONCE; }
static BOOL filter_valid(D3D11_FILTER f) {
  const UINT plain = (UINT)f & 0x7Fu, reduction = ((UINT)f >> 7) & 3u;
  if ((UINT)f & ~(0x7Fu | (3u << 7))) return FALSE;
  return plain == 0x55 || (plain & ~0x15u) == 0;      /* (anisotropic, or the three point/linear choices) */
  (void)reduction;
}
static HRESULT STDMETHODCALLTYPE dev_CreateSamplerState(Dev *d, const D3D11_SAMPLER_DESC *desc, ID3D11SamplerState **out) {
  if (out) *out = NULL;
  if (!desc) return E_INVALIDARG;
  if (!filter_valid(desc->Filter) || !address_valid(desc->AddressU) || !address_valid(desc->AddressV) || !address_valid(desc->AddressW) ||
      desc->MaxAnisotropy > D3D11_MAX_MAXANISOTROPY || desc->MinLOD > desc->MaxLOD || desc->MinLOD != desc->MinLOD || desc->MaxLOD != desc->MaxLOD)
    return E_INVALIDARG;
  if (((UINT)desc->Filter >> 7 & 3u) == 1 && !comparison_valid(desc->ComparisonFunc)) return E_INVALIDARG;      /* (a comparison filter needs a comparison) */
  D3D11_SAMPLER_DESC key = *desc;
  if (((UINT)desc->Filter & 0x7Fu) != 0x55) key.MaxAnisotropy = 0;
  if (((UINT)desc->Filter >> 7 & 3u) != 1) key.ComparisonFunc = D3D11_COMPARISON_NEVER;
  if (!out) return S_FALSE;
  extern const ID3D11SamplerStateVtbl sampler_vtbl;
  EnterCriticalSection(&d->lock);
  Sampler *s = (Sampler *)state_obj_find(d, STATE_SAMPLER, offsetof(Sampler, desc), &key, sizeof key);
  if (!s && (s = state_obj_new(d, sizeof *s, STATE_SAMPLER, &sampler_vtbl, g_sampler_iids)) != NULL) s->desc = key;
  LeaveCriticalSection(&d->lock);
  if (!s) return E_OUTOFMEMORY;
  *out = (ID3D11SamplerState *)s;
  return S_OK;
}

/* (deferred.inc: what a deferred context records) */
enum {
  REC_DRAW, REC_DRAW_INDEXED, REC_DRAW_INSTANCED, REC_DRAW_INDEXED_INSTANCED, REC_DRAW_AUTO, REC_DRAW_INDIRECT, REC_DRAW_INDEXED_INDIRECT, REC_DISPATCH,
  REC_DISPATCH_INDIRECT, REC_CLEAR_RTV, REC_CLEAR_DSV, REC_CLEAR_UAV_UINT, REC_CLEAR_UAV_FLOAT, REC_COPY_REGION, REC_COPY_RESOURCE, REC_UPDATE,
  REC_COPY_STRUCTURE_COUNT, REC_RESOLVE, REC_GENERATE_MIPS, REC_BEGIN, REC_END, REC_SO_TARGETS, REC_UAV_COUNTS, REC_MIN_LOD, REC_MAPPED, REC_CLEAR_VIEW,
  REC_DISCARD
};
typedef struct Command {
  UINT op;
  struct StateCopy *state;       /* the bindings it was recorded under (NULL: it does not need them) */
  DxObj *obj[8];                 /* references */
  UINT u[10];
  FLOAT f[4];
  D3D11_BOX box;
  BOOL has_box;
  BYTE *data;                    /* bytes it carries (an update, the writes of a Map) */
  SIZE_T bytes;
  D3D11_RECT *rects;             /* ClearView's rectangles */
  UINT rect_count;
} Command;
/* What a call of a context's method records (deferred_record). */
typedef struct RecArgs {
  UINT op;
  void *obj[8];
  UINT u[10];
  FLOAT f[4];
  const D3D11_BOX *box;
  const void *data;
  SIZE_T bytes;
  const D3D11_RECT *rects;
  UINT rect_count;
} RecArgs;
struct StateCopy;
static void deferred_record(Ctx *c, const RecArgs *a);
static void deferred_update(Ctx *c, ID3D11Resource *resource, UINT sub, const D3D11_BOX *box, const void *data, UINT row_pitch, UINT depth_pitch);
static HRESULT deferred_map(Ctx *c, ID3D11Resource *resource, UINT sub, D3D11_MAP type, D3D11_MAPPED_SUBRESOURCE *out);
static void deferred_unmap(Ctx *c, ID3D11Resource *resource, UINT sub);
static void STDMETHODCALLTYPE ctx_ClearView(Ctx *c, ID3D11View *view, const FLOAT colour[4], const D3D11_RECT *rects, UINT count);

#include "query.inc"
#include "context.inc"
#include "deferred.inc"
#include "context1.inc"
#include "rtshadow.inc"
static const ID3D11On12Device1Vtbl g_on12_vtbl;
#include "device.inc"
#include "interop.inc"
#include "dxgi_resource.inc"
#include "device5.inc"
#include "video.inc"
#include "vtbl.inc"
