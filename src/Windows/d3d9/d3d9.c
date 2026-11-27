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

/* d3d9.dll for TheoryCraftOS: Direct3D 9 and 9Ex - its fixed-function pipeline and a program's own vertex and pixel shaders - as a layer on this
 * system's Direct3D 12 (d3d12.dll; docs/Direct3D9.md).
 *
 * A device is a Direct3D 12 device, one command queue and one command list; drawing records into the list, and the list is sent when something has
 * to be seen (Present, a lock that reads, a query asked for its result, the end of what a ring of memory holds). What the program hands over is kept
 * in plain memory - vertex and index buffers, the levels of textures - and a draw brings it into the one form the shaders here take:
 *   vertices    whatever the program's layout (an FVF code or a declaration), each vertex a draw reads is decoded into a position, a colour (lit
 *               here, where there is lighting), a fog factor and specular colour and the texture coordinates of the stages (made here: the
 *               texture matrices, generated coordinates) and written into a ring of upload memory; indices are copied into another, counted from
 *               the first vertex the draw reads, and a triangle fan becomes a list.
 *   shaders     one vertex shader (the world, view and projection matrices as one, or positions that are pixels already; fog by the vertex),
 *               written in HLSL and compiled ahead (ff_vs.hlsl -> shaders.inc), and for each set-up of the texture stages a draw comes with a
 *               pixel shader 2.0 written out when it is first met (ff_stages.h) and translated like a program's own.
 *   a program's a vertex or pixel shader of the program's is translated to Shader Model 4 when it is made (sm_to_dxbc.h). A draw with a vertex
 *   shaders     shader decodes the vertices into one register of four floats for each input the shader declares; the constants the program set
 *               are the draw's block (the vertex shader's registers, then the pixel shader's); either stage may be the fixed-function one.
 *               A translated pixel shader ends with the alpha test and fog, which a draw steers by registers after the shader's constants.
 *   states      a Direct3D 12 pipeline for each combination of the render states that a pipeline holds (culling, depth, blending, stencil, write
 *               mask, fill mode, depth bias), in a cache.
 *   textures    a Direct3D 12 texture of B8G8R8A8 whatever the program's format; a level is sent when it is unlocked.
 *   swap chains the back buffers (and a front buffer) are the queue's (ITcosPresenter); Present reads the one shown and puts it into the window
 *               (StretchDIBits), as dxgi.dll does for its swap chains.
 *   copies      clears of parts of a target, ColorFill, StretchRect between sizes and formats: draws of the device's own (blit.inc); copies of
 *               one size: Direct3D 12 copies, or the CPU for surfaces in memory.
 *   queries     Direct3D 12 queries (occlusion, timestamps) and the list's submissions (events): query.inc.
 *
 * The objects follow Direct3D 9's references: a level of a texture (a surface, a volume) counts with its texture; a swap chain's buffers and the
 * device's depth buffer begin at no reference and keep the device while they have one; everything else holds its device. */
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

/* (the flags of PresentEx of the Windows SDK's d3d9.h that mingw's lacks) */
#ifndef D3DPRESENT_DONOTFLIP
#define D3DPRESENT_DONOTFLIP 0x00000004L
#define D3DPRESENT_FLIPRESTART 0x00000008L
#define D3DPRESENT_VIDEO_RESTRICT_TO_MONITOR 0x00000010L
#define D3DPRESENT_UPDATEOVERLAYONLY 0x00000020L
#define D3DPRESENT_HIDEOVERLAY 0x00000040L
#define D3DPRESENT_UPDATECOLORKEY 0x00000080L
#define D3DPRESENT_FORCEIMMEDIATE 0x00000100L
#endif

#define OBJ(x) ((DxObj *)(x))
#define STAGES FFS_STAGES              /* the texture stages of the fixed-function pipeline (and the stages SetTextureStageState takes) */
#define TEXTURE_SLOTS 16u              /* the samplers of a pixel shader */
#define SAMPLERS 21u                   /* the samplers a program sets textures and states of: 0 .. 15, D3DDMAPSAMPLER (16 here), D3DVERTEXTEXTURESAMPLER0 .. 3 (17 .. 20) */
#define ROOTS 4u                       /* root signatures: for 2, 4, 8 and 16 textures */
#define STREAMS 16u
#define VERTEX_BYTES(sets) (48u + 16u * (sets))      /* a decoded vertex: position (4 floats), colour (4), the fog factor it brings and its specular colour (1 + 3), and so many sets of texture coordinates (4 each) */
#define TEXTURE_SETS 8u                /* the sets of texture coordinates a vertex can have */
#define VERTEX_RING (1u << 20)       /* (21845 vertices a draw at most. The rings are kept small: each is there three times - here, in the kernel, on the device) */
#define INDEX_RING (256u << 10)
#define CONSTANT_RING (1024u << 10)
#define VERTEX_REGISTERS 11u           /* the constants of the fixed-function vertex shader, in registers of four floats */
#define FIXED_SHADERS 48u              /* the pixel shaders of texture stage set-ups that are kept */
#define CONSTANT_REGISTERS 1024u       /* the block of a draw, at most (a vertex shader 3.0 has 256 floats, 16 bools and its state; a pixel shader 3.0 224, 16, 5 and 16 fills) */
#define VS_FLOATS 256u                 /* the float constants a program sets for its vertex shaders, */
#define PS_FLOATS 224u                 /* ... and for its pixel shaders */
#define SHADER_INPUTS 16u
#define VIEW_HEAP 4096u
#define RTV_SLOTS 256u
#define DSV_SLOTS 64u
#define PIPELINES 64u
#define RENDER_STATES 256u
#define SAMPLER_STATES 14u             /* D3DSAMP_ADDRESSU (1) .. D3DSAMP_DMAPOFFSET (13) */
#define STAGE_STATES 34u               /* D3DTSS_COLOROP (1) .. D3DTSS_CONSTANT (32), and TCOS_TSS_KEY_KILL */
/* A stage state of this d3d9.dll's own, for ddraw.dll's Direct3D: a texel of alpha 0 of the stage's texture takes the pixel away in the fixed-function
 * pixel stage (a colour-keyed texture: ddraw.dll gives keyed texels alpha 0 and the others at least 1), whatever the stage then does with the alpha.
 * Set only through the private export TcosD3D9SetStageKeyKill (device.inc): IDirect3DDevice9::SetTextureStageState ignores type 33 as Windows ignores a type it
 * does not have, so a program never reaches it. */
#define TCOS_TSS_KEY_KILL 33u
/* A usage of this d3d9.dll's own, for ddraw.dll's Direct3D: a depth buffer the program may lock whatever its format (DirectDraw's depth buffers are memory a
 * program locks; Direct3D 9 locks only the D*_LOCKABLE formats). The layouts of the formats are in tex_depth_copy. Only the private export
 * TcosD3D9CreateLockableDepthStencil takes it: IDirect3DDevice9Ex::CreateDepthStencilSurfaceEx refuses it as Windows refuses a usage it does not have. */
#define TCOS_USAGE_LOCKABLE_DEPTH 0x40000000u
#define MAX_LEVELS 13u                 /* of a texture of 4096 pixels */
#define MAX_SUBS (MAX_LEVELS * 6u)     /* the subresources of a texture: face * levels + level */
#define MAX_ELEMENTS 64u
#define CLIP_PLANES 6u                 /* D3DMAXUSERCLIPPLANES */
#define WORLD_MATRICES 256u            /* D3DTS_WORLDMATRIX(0 .. 255) */
#define TRANSFORMS 512u                /* the numbers of the transforms: D3DTS_VIEW (2), D3DTS_PROJECTION (3), D3DTS_TEXTURE0 .. 7 (16 .. 23), D3DTS_WORLDMATRIX(n) (256 + n) */
#define MAX_BACK_BUFFERS 30u           /* D3DPRESENT_BACK_BUFFERS_MAX_EX (3 for a device that is not Ex) */
#define TARGETS 4u                     /* the render targets a draw writes at once (D3DCAPS9.NumSimultaneousRTs) */
#define MAX_POINT_SIZE 1.0f            /* D3DCAPS9.MaxPointSize: points are one pixel (point sprites are not drawn here yet) */

typedef struct Dev Dev;
typedef struct Tex Tex;
typedef struct Surface Surface;
typedef struct SwapChain SwapChain;
typedef struct Direct3D Direct3D;

/* ---- the objects ----------------------------------------------------------------------------------------------------------------------------------- */

/* Every object of a device: the head, the device and a number no other object of the process has. The references are Direct3D 9's:
 *   - an object holds a reference of its device while the program holds one of it (Child.o.ref above 0);
 *   - what the device (or a state block) has set - a texture, a buffer, a target - is kept alive by a count of its own (`bound`), which the
 *     program does not see: the object goes when both counts are 0;
 *   - a level of a texture (a surface, a volume) has no count of its own: `forward` is the texture, which its AddRef and Release are;
 *   - `kept`: what made it frees it (a swap chain's buffers, the device's own objects), whatever the counts say. */
typedef struct Child {
  DxObj o;
  Dev *dev;
  UINT id;
  LONG bound;
  int kept;
  DxObj *forward;
  DWORD priority;                /* SetPriority (managed resources) */
} Child;
static LONG g_next_id;

typedef struct Buffer {      /* a vertex or an index buffer: plain memory, read when something is drawn from it */
  Child c;
  UINT length;
  DWORD usage, fvf;
  D3DFORMAT format;          /* an index buffer's */
  D3DPOOL pool;
  BYTE *data;
  BOOL locked;               /* (D3DERR_INVALIDCALL for an Unlock with no Lock is not Direct3D 9's: it answers D3D_OK) */
  UINT lock_count;
} Buffer;

/* A level of a volume texture, as its own object (IDirect3DVolume9): it counts with its texture. */
typedef struct Volume {
  Child c;
  Tex *tex;
  UINT level;
} Volume;

/* A texture, and what stands behind a surface that is not a level of a program's texture (a swap chain's buffer, a depth buffer, an off-screen surface). */
struct Tex {
  Child c;
  UINT width, height, levels;
  UINT kind;                     /* the kernel's TextureKind: 0 2D, 1 a volume, 3 a cube (IDirect3DCubeTexture9, IDirect3DVolumeTexture9) */
  UINT slices;                   /* a volume's depth (1 for the others) */
  UINT kmips;                    /* the levels the Direct3D 12 texture has: the kernel keeps one of a volume or a cube, whose other levels are dropped */
  D3DFORMAT format;
  DWORD usage;
  D3DPOOL pool;
  D3DMULTISAMPLE_TYPE samples;
  DWORD quality;
  int depth;                     /* a depth buffer */
  int target;                    /* something that is drawn into */
  int lockable;                  /* a target or depth buffer the program may lock (CreateRenderTarget's Lockable, D3DPRESENTFLAG_LOCKABLE_BACKBUFFER, the D3DFMT_*_LOCKABLE formats) */
  int internal;                  /* the device's own (a white texture to sample, a swap chain's buffers, the device's depth buffer): not one of the program's resources */
  int surface_alone;             /* behind a surface made on its own (CreateRenderTarget, CreateOffscreenPlainSurface, CreateDepthStencilSurface) */
  ID3D12Resource *res;           /* NULL: memory only (a surface in system memory or a scratch one) */
  HANDLE shared;                 /* the handle it was shared with or opened by (pSharedHandle) */
  void *user_memory;             /* (9Ex) a system-memory texture of one level whose pixels are the program's memory (pSharedHandle of D3DPOOL_SYSTEMMEM) */
  /* By subresource, face * levels + level (a cube's six faces; one for the others): */
  BYTE *shadow[MAX_SUBS];        /* the pixels of a level as the program has them, once it was locked (a volume's: its slices one after the other) */
  BYTE *shadow_block;            /* ... all of them in one allocation, face after face and level after level, as Windows lays a texture out (test_lockrect_offset) */
  DWORD locked[MAX_SUBS];        /* the flags of the lock, | 0x80000000 while it is locked */
  Surface *surface[MAX_SUBS];    /* the level as a surface, once a program asked for it (it counts with the texture) */
  Volume *volume[MAX_LEVELS];    /* ... as a volume */
  UINT rtv_slot, dsv_slot;
  D3D12_CPU_DESCRIPTOR_HANDLE view;      /* of a target or a depth buffer */
  UINT used;                     /* the command list that has a command using it */
  UINT sampled;                  /* ... a draw sampling it */
  /* What UpdateTexture copies: the parts of the top level that were written since (AddDirtyRect, Lock), by face; a volume's box. */
  int dirty[6];
  RECT dirty_rect[6];
  D3DBOX dirty_box;
  UINT palette;                  /* P8 and A8P8: the palette and its version the Direct3D 12 copy was made with */
  UINT palette_version;
  DWORD lod;                     /* SetLOD (managed textures) */
  D3DTEXTUREFILTERTYPE autogen_filter;
  int autogen_dirty;             /* the top level changed since the levels were made from it (D3DUSAGE_AUTOGENMIPMAP) */
  UINT dc_count;                 /* the surfaces of it that have a DC out */
  UINT generation;               /* (a swap chain's buffers: the resource behind it changes as it flips) */
  UINT64 video;                  /* the bytes it takes of the adapter's memory (Dev.video_bytes) */
};

enum { SURFACE_LEVEL, SURFACE_ALONE, SURFACE_IMPLICIT };
struct Surface {
  Child c;
  Tex *tex;                      /* a reference, except of a level (the texture holds the surface) */
  UINT sub;                      /* the subresource: face * levels + level */
  int kind;                      /* SURFACE_LEVEL: a level of a texture (it counts with it); SURFACE_ALONE: an object of its own; SURFACE_IMPLICIT: a swap chain's buffer
                                    or the device's depth buffer (it begins at no reference, keeps the device while it has one, and goes with what made it) */
  DxObj *container;              /* GetContainer: the texture, the swap chain, or (NULL) the device */
  HDC dc;                        /* GetDC .. ReleaseDC */
  D3D12_CPU_DESCRIPTOR_HANDLE view;      /* a target that is not level 0 of a 2D texture (another level, a face of a cube): its own render target view */
  UINT view_slot;
  BOOL has_view;
  HBITMAP bitmap;
  HGDIOBJ old_bitmap;
  BYTE *dib;
};

/* What a vertex declaration (or an FVF code) says of the things the fixed-function shaders take: where each is, and as what; type 255: not there. */
typedef struct Attribute { BYTE stream, type; WORD offset; } Attribute;
typedef struct Layout {
  Attribute position, colour, texture[TEXTURE_SETS];
  Attribute specular;            /* the second colour: its alpha is the fog factor the vertex brings */
  Attribute normal;              /* for lighting */
  Attribute weights, indices;    /* vertex blending: the blend weights and (D3DRS_INDEXEDVERTEXBLENDENABLE) the matrices' indices */
  Attribute position1, normal1;  /* tweening: the second position and normal */
  Attribute point_size;
  int transformed;               /* the positions are pixels (XYZRHW) */
} Layout;
typedef struct Decl {
  Child c;
  D3DVERTEXELEMENT9 elements[MAX_ELEMENTS + 1];
  UINT count;                    /* with the end marker */
  DWORD fvf;                     /* the FVF code the declaration is the same as (0: none), as Direct3D 9 works it out */
  Layout layout;
} Decl;

typedef struct Stream { Buffer *buffer; UINT offset, stride, freq; } Stream;      /* (freq: SetStreamSourceFreq's value) */

/* A vertex or pixel shader of the program's: its tokens (GetFunction gives them back) and what the translator made of them. */
#define SHADER_MAGIC 0x39534854u      /* "THS9": Shader.magic */
typedef struct Shader {
  Child c;
  UINT magic;
  int vertex;
  DWORD *code;
  UINT code_bytes;
  BYTE *dxbc;
  UINT dxbc_bytes;
  UINT floats;                   /* the float constants it reads: c0 .. c<floats - 1> */
  int bools;                     /* it reads bool constants: 16 registers after the floats */
  UINT registers;                /* all it takes of a draw's block */
  UINT major;                    /* its shader model */
  UINT state;                    /* the first of its state registers (a pixel shader's alpha test and fog, a vertex shader's half pixel: sm_to_dxbc.h) */
  UINT fill;                     /* a pixel shader: the first of its fill registers, one a sampler up to sampler_count (what a texture's format has not reads 1) */
  UINT sampler_count;            /* a pixel shader: the highest sampler it reads, and one */
  UINT bump, bump_stages;        /* a pixel shader 1.x: its first bump register (texbem, texbeml, bem: two a stage, sm_to_dxbc.h), and the stages they are of */
  BYTE sampler_kind[16];         /* ... and what each is declared as (SM_SAMPLER_*: Tex.kind) */
  UINT coordinates;              /* ... and the highest set of texture coordinates */
  SmInput inputs[SHADER_INPUTS]; /* a vertex shader: what each input register is */
  UINT input_count;
} Shader;

/* A light, by its number: Direct3D 9 takes any number (the device keeps those that were set, in the order of their numbers). */
typedef struct Light { DWORD index; D3DLIGHT9 light; BOOL enabled; } Light;

/* Everything a program sets on the device that a state block records. The pointers are references. */
typedef struct State {
  DWORD rs[RENDER_STATES];
  DWORD sampler[SAMPLERS][SAMPLER_STATES];
  DWORD stage[STAGES][STAGE_STATES];
  D3DMATRIX world[WORLD_MATRICES], view, projection, texture[STAGES];
  D3DVIEWPORT9 viewport;
  RECT scissor;
  D3DMATERIAL9 material;
  Light *lights;                 /* (memory of the state's own: copied, not shared) */
  UINT light_count, light_capacity;
  float clip_plane[CLIP_PLANES][4];
  float npatch;                  /* SetNPatchMode */
  DWORD fvf;
  Decl *decl;
  Stream stream[STREAMS];
  Buffer *indices;
  void *tex[SAMPLERS];           /* Tex (2D, cube, volume texture) */
  Shader *vs, *ps;               /* NULL: the fixed-function stage */
  float vs_float[VS_FLOATS][4], ps_float[PS_FLOATS][4];
  int vs_int[16][4], ps_int[16][4];
  DWORD vs_bool, ps_bool;        /* bit n: b<n> */
} State;

/* Which parts of the state a state block holds: a bit for each render state, sampler and stage state, transform, constant register ... */
typedef struct StateMask {
  DWORD rs[RENDER_STATES / 32];
  DWORD sampler[SAMPLERS];       /* bit n: sampler state n */
  UINT64 stage[STAGES];          /* bit n: stage state n */
  DWORD transform[TRANSFORMS / 32];
  DWORD vs_float[VS_FLOATS / 32], ps_float[PS_FLOATS / 32];
  DWORD vs_int, ps_int, vs_bool, ps_bool;
  DWORD streams, stream_freqs, textures, clip_planes;
  BOOL viewport, scissor, material, fvf, decl, indices, vs, ps, npatch;
  BOOL all_lights;               /* every light there is (D3DSBT_ALL and VERTEXSTATE), else those of `light` */
  DWORD *light;                  /* the numbers of the lights it holds (recorded ones), `light_count` of them */
  UINT light_count, light_capacity;
} StateMask;

typedef struct StateBlock { Child c; State state; StateMask mask; } StateBlock;

typedef struct Pipeline {
  DWORD vs, ps;                  /* the shaders' numbers (Child.id); 0: the fixed-function vertex shader */
  DWORD sets;                    /* ... which has so many sets of texture coordinates in its vertices */
  DWORD cull, z, zwrite, zfunc, blend, src, dst, op, separate, src_alpha, dst_alpha, op_alpha, write, stencil, sfail, szfail, spass, sfunc, smask, swrite;
  DWORD wireframe, bias, slope;   /* D3DRS_FILLMODE, D3DRS_DEPTHBIAS (in the depth buffer's units) and D3DRS_SLOPESCALEDEPTHBIAS (float bits) */
  DWORD twosided, ccw_fail, ccw_zfail, ccw_pass, ccw_func;   /* D3DRS_TWOSIDEDSTENCILMODE and the counter-clockwise faces' operations */
  DWORD topology;                /* D3D12_PRIMITIVE_TOPOLOGY_TYPE */
  DWORD depth_format;            /* the DXGI format of the depth buffer, 0 for none */
  DWORD target_format;           /* ... and of the render target */
  DWORD targets;                 /* the render targets drawn into (1 .. 4) */
  DWORD extra_format[3];         /* the formats of targets 1 .. 3 */
  DWORD extra_write;             /* their D3DRS_COLORWRITEENABLE1 .. 3, four bits each */
  ID3D12PipelineState *state;
  UINT last_used;
} Pipeline;

/* The pixel shader of a set-up of the texture stages (ff_stages.h), made when a draw first came with it. */
typedef struct FixedShader { FfsKey key; Shader *shader; UINT last_used; } FixedShader;

typedef struct PendingClear { Tex *depth; UINT flags; FLOAT z; UINT8 stencil; } PendingClear;

struct Direct3D {
  DxObj o;
  int ex;                        /* made by Direct3DCreate9Ex */
};

/* A swap chain: its back buffers and the front buffer (what was shown last), the queue's (ITcosPresenter); the device's first (implicit) one begins at
 * no reference and goes with the device. */
struct SwapChain {
  Child c;
  int implicit;
  D3DPRESENT_PARAMETERS pp;      /* as the program has them back (the sizes and formats worked out) */
  HWND window;                   /* the window Present shows the picture in, unless told another */
  UINT count;                    /* back buffers */
  Tex *buffer[MAX_BACK_BUFFERS + 1];     /* the back buffers, then the front buffer */
  Surface *surface[MAX_BACK_BUFFERS];    /* ... the back buffers as surfaces */
  D3DGAMMARAMP ramp;
  int ramp_set;                  /* a ramp other than the identity is set */
  UINT presents;                 /* the Presents it showed (9Ex: GetLastPresentCount, GetPresentStats) */
  UINT64 last_present_qpc;
  D3DDISPLAYMODEEX mode;         /* the display mode it has (full screen) or the desktop's */
  BYTE *pixels;                  /* (Present: the picture read back, kept for the next) */
  SIZE_T pixel_bytes;
};

#define QUERY_SLOTS 256u
struct Query;

struct Dev {
  DxObj o;
  Direct3D *parent;
  int ex;                        /* an IDirect3DDevice9Ex */
  CRITICAL_SECTION lock;
  D3DDEVICE_CREATION_PARAMETERS creation;
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
  UINT epoch;                    /* the number of the command list being recorded; those below it were sent and have run */
  int recorded;
  int targets_dirty;             /* the next draw sets the render target, the viewport and the scissor rectangle again */
  Pipeline pipelines[PIPELINES];
  UINT pipeline_count, pipeline_clock;
  FixedShader fixed[FIXED_SHADERS];      /* (the shaders hold no reference of the device) */
  UINT fixed_count, fixed_clock;
  PendingClear clears[4];
  UINT clear_count;
  SwapChain *chain;              /* the implicit swap chain */
  Surface *auto_depth;           /* the depth buffer made with the device (or NULL; its container is the device) */
  Surface *target[TARGETS];      /* what is drawn into now (references) */
  Surface *depth;
  State state;
  StateBlock *recording;         /* BeginStateBlock .. EndStateBlock: what is set goes into it, not into the device */
  int in_scene;
  /* the program's resources in D3DPOOL_DEFAULT (Reset of a device that is not Ex fails while there are any), and its additional swap chains */
  LONG default_resources;
  int lost;                      /* the device is lost (D3DERR_DEVICELOST), and after that not reset (D3DERR_DEVICENOTRESET) until Reset */
  int not_reset;
  DEVMODEW lost_mode;            /* the display mode when it was made or last reset: another one loses it */
  /* palettes (SetPaletteEntries): 256 entries each, by number */
  PALETTEENTRY (*palettes)[256];
  BYTE *palette_set;             /* (a byte for each: it was set) */
  UINT palette_count;            /* the room in `palettes` */
  UINT current_palette;          /* SetCurrentTexturePalette; 0xFFFF before the first */
  UINT palette_version;          /* a number for every change of a palette or of the current one */
  /* the cursor (SetCursorProperties ...) */
  HCURSOR cursor;
  int cursor_shown;
  POINT cursor_position;
  BOOL dialog_box_mode;
  D3DCLIPSTATUS9 clip_status;
  BOOL software_vp;
  /* queries (query.inc) */
  ID3D12QueryHeap *occlusion_heap, *timestamp_heap;
  ID3D12Resource *query_readback;        /* QUERY_SLOTS results of 8 bytes for the occlusion heap's slots, then QUERY_SLOTS for the timestamp heap's */
  BYTE query_slot_used[QUERY_SLOTS], timestamp_slot_used[QUERY_SLOTS];
  struct Query *open_queries;    /* occlusion queries begun and not ended: they go on across a submission (flush ends and begins them again) */
  UINT resolved_epoch;           /* (the list whose results were read) */
  /* the declarations SetFVF made, by their codes (kept by the device; they begin at no reference) */
  struct FvfDecl *fvf_decls;
  UINT fvf_decl_count, fvf_decl_capacity;
  UINT64 video_bytes;            /* what the device's textures and buffers take of the adapter's memory (GetAvailableTextureMem) */
  /* Ex */
  INT gpu_priority;
  UINT max_frame_latency;
  UINT conv_width, conv_height;  /* SetConvolutionMonoKernel */
  float conv_rows[7], conv_columns[7];
};

#define obj_QueryInterface dx_QueryInterface
#define obj_AddRef dx_AddRef
#define obj_Release dx_Release

/* An object of a device, with one reference (and so one of the device). `implicit`: with none (and none of the device), and kept by what made it. */
static void *child_new_ex(SIZE_T size, Dev *dev, const void *vtbl, const GUID *const *iids, void (*destroy)(DxObj *), int implicit) {
  Child *c = mem_alloc(size);
  if (!c) return NULL;
  dx_init(&c->o, vtbl, iids, destroy);
  c->dev = dev;
  c->id = (UINT)InterlockedIncrement(&g_next_id);
  if (implicit) {
    c->o.ref = 0;
    c->kept = 1;
  } else dx_AddRef(OBJ(dev));
  return c;
}
static void *child_new(SIZE_T size, Dev *dev, const void *vtbl, const GUID *const *iids, void (*destroy)(DxObj *)) {
  return child_new_ex(size, dev, vtbl, iids, destroy, 0);
}
static void child_destroy(DxObj *o) { (void)o; }
/* Frees an object whatever its counts (an object that goes with what made it): its private data, what it holds, its memory. */
static void obj_free(DxObj *o) {
  if (!o) return;
  DxPrivate *p = o->priv;
  while (p) {
    DxPrivate *next = p->next;
    if (p->size == 0xFFFFFFFFu) { if (p->data) ((IUnknown *)p->data)->lpVtbl->Release((IUnknown *)p->data); }
    else mem_free(p->data);
    mem_free(p);
    p = next;
  }
  o->priv = NULL;
  if (o->destroy) o->destroy(o);
  mem_free(o);
}
static ULONG STDMETHODCALLTYPE child_AddRef(Child *c) {
  if (c->forward) return ((IUnknown *)c->forward)->lpVtbl->AddRef((IUnknown *)c->forward);
  const LONG n = InterlockedIncrement(&c->o.ref);
  if (n == 1) dx_AddRef(OBJ(c->dev));      /* (one that had none: it holds the device again) */
  return (ULONG)n;
}
static ULONG STDMETHODCALLTYPE child_Release(Child *c) {
  if (c->forward) return ((IUnknown *)c->forward)->lpVtbl->Release((IUnknown *)c->forward);
  if (c->o.ref <= 0) return 0;      /* (an implicit object with no reference: Direct3D 9 answers 0 and nothing happens) */
  const LONG n = InterlockedDecrement(&c->o.ref);
  if (n == 0) {
    Dev *dev = c->dev;
    if (!c->kept && c->bound == 0) obj_free(OBJ(c));
    dx_Release(OBJ(dev));
  }
  return (ULONG)n;
}
static HRESULT STDMETHODCALLTYPE child_QueryInterface(Child *c, REFIID riid, void **out) {
  if (!out) return E_POINTER;
  *out = NULL;
  int found = guid_eq(riid, &IID_IUnknown) || guid_eq(riid, &IID_ITcosObject);      /* (tcos_object.h: the object itself) */
  for (const GUID *const *i = c->o.iids; !found && i && *i; ++i) found = guid_eq(riid, *i);
  if (!found) return E_NOINTERFACE;
  child_AddRef(c);
  *out = c;
  return S_OK;
}
static HRESULT STDMETHODCALLTYPE child_GetDevice(Child *c, IDirect3DDevice9 **out) {
  if (!out) return D3DERR_INVALIDCALL;
  dx_AddRef(OBJ(c->dev));
  *out = (IDirect3DDevice9 *)c->dev;
  return D3D_OK;
}

/* What keeps an object alive when the device (or a state block) has it set: the object itself, or the texture of a level, or the swap chain of a
 * buffer. */
static Child *binding_of(void *object);
/* Puts `object` (or NULL) where `*slot` is, with the counts of what is set moved (Child.bound). */
static void bind(void *slot_address, void *object) {
  Child **slot = slot_address;
  if (*slot == object) return;
  if (object) InterlockedIncrement(&binding_of(object)->bound);
  Child *old = *slot;
  *slot = object;
  if (old) {
    Child *b = binding_of(old);
    if (InterlockedDecrement(&b->bound) == 0 && b->o.ref == 0 && !b->kept) obj_free(OBJ(b));
  }
}

extern const IDirect3D9ExVtbl d3d_vtbl;
extern const IDirect3DDevice9ExVtbl dev_vtbl;
extern const IDirect3DVertexBuffer9Vtbl vb_vtbl;
extern const IDirect3DIndexBuffer9Vtbl ib_vtbl;
extern const IDirect3DTexture9Vtbl tex_vtbl;
extern const IDirect3DCubeTexture9Vtbl cube_vtbl;
extern const IDirect3DVolumeTexture9Vtbl vol_vtbl;
extern const IDirect3DVolume9Vtbl volume_vtbl;
extern const IDirect3DSurface9Vtbl surf_vtbl;
extern const IDirect3DSwapChain9ExVtbl chain_vtbl;
extern const IDirect3DVertexDeclaration9Vtbl decl_vtbl;
extern const IDirect3DStateBlock9Vtbl sb_vtbl;
extern const IDirect3DVertexShader9Vtbl vs_vtbl;
extern const IDirect3DPixelShader9Vtbl ps_vtbl;
extern const IDirect3DQuery9Vtbl query_vtbl;

static const GUID *const g_d3d_iids[] = {&IID_IDirect3D9, NULL};
static const GUID *const g_d3d_ex_iids[] = {&IID_IDirect3D9, &IID_IDirect3D9Ex, NULL};
static const GUID *const g_dev_iids[] = {&IID_IDirect3DDevice9, NULL};
static const GUID *const g_dev_ex_iids[] = {&IID_IDirect3DDevice9, &IID_IDirect3DDevice9Ex, NULL};
static const GUID *const g_vb_iids[] = {&IID_IDirect3DResource9, &IID_IDirect3DVertexBuffer9, NULL};
static const GUID *const g_ib_iids[] = {&IID_IDirect3DResource9, &IID_IDirect3DIndexBuffer9, NULL};
static const GUID *const g_tex_iids[] = {&IID_IDirect3DResource9, &IID_IDirect3DBaseTexture9, &IID_IDirect3DTexture9, NULL};
static const GUID *const g_cube_iids[] = {&IID_IDirect3DResource9, &IID_IDirect3DBaseTexture9, &IID_IDirect3DCubeTexture9, NULL};
static const GUID *const g_vol_iids[] = {&IID_IDirect3DResource9, &IID_IDirect3DBaseTexture9, &IID_IDirect3DVolumeTexture9, NULL};
static const GUID *const g_volume_iids[] = {&IID_IDirect3DVolume9, NULL};
static const GUID *const g_surf_iids[] = {&IID_IDirect3DResource9, &IID_IDirect3DSurface9, NULL};
static const GUID *const g_chain_iids[] = {&IID_IDirect3DSwapChain9, NULL};
static const GUID *const g_chain_ex_iids[] = {&IID_IDirect3DSwapChain9, &IID_IDirect3DSwapChain9Ex, NULL};
static const GUID *const g_decl_iids[] = {&IID_IDirect3DVertexDeclaration9, NULL};
static const GUID *const g_sb_iids[] = {&IID_IDirect3DStateBlock9, NULL};
static const GUID *const g_vs_iids[] = {&IID_IDirect3DVertexShader9, NULL};
static const GUID *const g_ps_iids[] = {&IID_IDirect3DPixelShader9, NULL};
static const GUID *const g_query_iids[] = {&IID_IDirect3DQuery9, NULL};

/* ---- the command list ------------------------------------------------------------------------------------------------------------------------------- */

static void record_pending_clears(Dev *d);
static int adapter_mipmapped_layers(void);
static void queries_before_submit(Dev *d);
static void queries_after_submit(Dev *d);
static void queries_before_draw(Dev *d);
static BOOL queries_need_slots(Dev *d);

/* Marks a ring the list wrote, so that d3d12.dll sends it with the list (see make_ring). */
static void ring_touch(ID3D12Resource *ring, UINT used) {
  void *p = NULL;
  if (used && SUCCEEDED(ID3D12Resource_Map(ring, 0, NULL, &p))) ID3D12Resource_Unmap(ring, 0, NULL);
}
/* Sends what was recorded (the queue runs it before it returns) and begins a new list. Occlusion queries that are open are ended in the list that
 * goes and begun again in the next: what each counted is added up (query.inc). */
static void flush(Dev *d) {
  if (!d->recorded) return;
  queries_before_submit(d);
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
  queries_after_submit(d);
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

#include "formats.inc"
#include "resources.inc"
#include "create.inc"
#include "state.inc"
#include "draw.inc"
#include "blit.inc"
#include "swapchain.inc"
#include "query.inc"
#include "device.inc"
#include "vtbl.inc"
