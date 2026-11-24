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

/* d3d12.dll: the types the parts share, the formats, the GPU virtual addresses, the ID3D12Object / ID3D12DeviceChild methods. Included by d3d12.c. */

typedef struct Dev Dev;
typedef struct Resource Resource;
typedef struct DescHeap DescHeap;
typedef struct RootSig RootSig;
typedef struct Pso Pso;

#define KALIGN(value, alignment) (((value) + (alignment) - 1) / (alignment) * (alignment))

/* ---- the kernel's allocation flags and formats (src/Kernel/Dxgkrnl/DxgTypes.hpp) ------------------------------------------------------------------------------------------- */

enum {
  KA_VERTEX = 1u << 0, KA_INDEX = 1u << 1, KA_TEXTURE = 1u << 2, KA_RENDER_TARGET = 1u << 3, KA_DEPTH = 1u << 4, KA_PRESENT = 1u << 5, KA_STAGING = 1u << 6, KA_QUERY = 1u << 8, KA_UAV = 1u << 9, KA_STREAM_OUTPUT = 1u << 10,
  KA_HEAP = 1u << 13, KA_RESERVED = 1u << 14,
};
enum {
  KF_UNKNOWN, KF_A8R8G8B8, KF_X8R8G8B8, KF_A8B8G8R8, KF_X8B8G8R8, KF_D32F, KF_D24S8, KF_RGBA16F, KF_RGBA32F, KF_BGRA8_SRGB, KF_RGBA8_SRGB, KF_RGBA16, KF_RGBA32UINT, KF_RGBA32SINT,
  KF_RGBA16UINT, KF_RGBA16SINT, KF_RGBA8UINT, KF_RGBA16SNORM, KF_RGB10A2, KF_BC1, KF_BC2, KF_BC3, KF_BC1_SRGB, KF_BC2_SRGB, KF_BC3_SRGB,
};
#define KMAX_TEXTURE_DIMENSION 4096
#define KTILE_BYTES 65536

typedef struct FormatInfo {
  DXGI_FORMAT dxgi;
  UINT kernel;         /* the kernel's format (0: not one a texture can have) */
  UINT bytes;          /* of a pixel; of a 4 x 4 block of a compressed format */
  UINT depth;          /* depth/stencil format */
  UINT compressed;     /* block-compressed (BC1 .. BC3): the rows of a footprint are rows of blocks */
  UINT narrow;         /* NARROW_*: one or two 8- or 16-bit channels the kernel keeps widened (its CopyBufferImage NarrowLayout); `bytes` is the footprint's pixel */
  UINT planar;         /* PLANAR_*: NV12 (8 bits), P010 / P016 (16 bits) - a luma plane and a half-size plane of U, V: each plane and array slice an allocation of its own;
                        * P208 (U, V half as wide), V208 / V408 (three planes of R8: U and V half as high, or full size) */
  UINT pair;           /* a packed 4:2:2 format (YUY2, Y210, Y216): a pixel of the kernel's image - and of a view, R8G8B8A8 / R16G16B16A16 - is a pair of the texture's (Y0, U, Y1,
                        * V); the image is half as wide, `bytes` is the pair's */
} FormatInfo;
/* The kernel's NarrowLayout (src/Kernel/Dxgkrnl/CommandStream.hpp): an image of four channels the program sees as one or two. Copied into it a pixel becomes (r, g or 0, 0,
 * the maximum), so an R8 / R8G8 / R16 / R16G16 view of it samples what Direct3D says it does; render targets of these formats write their channels only (pso.inc). */
enum { NARROW_NONE, NARROW_R8, NARROW_R8G8, NARROW_R16, NARROW_R16G16 };
enum { PLANAR_NONE, PLANAR_8, PLANAR_16, PLANAR_P208, PLANAR_V208, PLANAR_V408 };

static const FormatInfo g_formats[] = {
    {DXGI_FORMAT_R32G32B32A32_TYPELESS, KF_RGBA32F, 16, 0}, {DXGI_FORMAT_R32G32B32A32_FLOAT, KF_RGBA32F, 16, 0}, {DXGI_FORMAT_R32G32B32A32_UINT, KF_RGBA32UINT, 16, 0},
    {DXGI_FORMAT_R32G32B32A32_SINT, KF_RGBA32SINT, 16, 0}, {DXGI_FORMAT_R16G16B16A16_TYPELESS, KF_RGBA16F, 8, 0}, {DXGI_FORMAT_R16G16B16A16_FLOAT, KF_RGBA16F, 8, 0},
    {DXGI_FORMAT_R16G16B16A16_UNORM, KF_RGBA16, 8, 0}, {DXGI_FORMAT_R16G16B16A16_UINT, KF_RGBA16UINT, 8, 0}, {DXGI_FORMAT_R16G16B16A16_SNORM, KF_RGBA16SNORM, 8, 0},
    {DXGI_FORMAT_R16G16B16A16_SINT, KF_RGBA16SINT, 8, 0}, {DXGI_FORMAT_R10G10B10A2_TYPELESS, KF_RGB10A2, 4, 0}, {DXGI_FORMAT_R10G10B10A2_UNORM, KF_RGB10A2, 4, 0},
    {DXGI_FORMAT_R8G8B8A8_TYPELESS, KF_A8B8G8R8, 4, 0}, {DXGI_FORMAT_R8G8B8A8_UNORM, KF_A8B8G8R8, 4, 0}, {DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, KF_RGBA8_SRGB, 4, 0},
    {DXGI_FORMAT_R8G8B8A8_UINT, KF_RGBA8UINT, 4, 0}, {DXGI_FORMAT_B8G8R8A8_UNORM, KF_A8R8G8B8, 4, 0}, {DXGI_FORMAT_B8G8R8X8_UNORM, KF_X8R8G8B8, 4, 0},
    {DXGI_FORMAT_B8G8R8A8_TYPELESS, KF_A8R8G8B8, 4, 0}, {DXGI_FORMAT_B8G8R8A8_UNORM_SRGB, KF_BGRA8_SRGB, 4, 0}, {DXGI_FORMAT_B8G8R8X8_TYPELESS, KF_X8R8G8B8, 4, 0},
    {DXGI_FORMAT_D32_FLOAT, KF_D32F, 4, 1}, {DXGI_FORMAT_D24_UNORM_S8_UINT, KF_D24S8, 4, 1},
    {DXGI_FORMAT_BC1_TYPELESS, KF_BC1, 8, 0, 1}, {DXGI_FORMAT_BC1_UNORM, KF_BC1, 8, 0, 1}, {DXGI_FORMAT_BC1_UNORM_SRGB, KF_BC1_SRGB, 8, 0, 1},
    {DXGI_FORMAT_BC2_TYPELESS, KF_BC2, 16, 0, 1}, {DXGI_FORMAT_BC2_UNORM, KF_BC2, 16, 0, 1}, {DXGI_FORMAT_BC2_UNORM_SRGB, KF_BC2_SRGB, 16, 0, 1},
    {DXGI_FORMAT_BC3_TYPELESS, KF_BC3, 16, 0, 1}, {DXGI_FORMAT_BC3_UNORM, KF_BC3, 16, 0, 1}, {DXGI_FORMAT_BC3_UNORM_SRGB, KF_BC3_SRGB, 16, 0, 1},
    {DXGI_FORMAT_R8_TYPELESS, KF_A8B8G8R8, 1, 0, 0, NARROW_R8}, {DXGI_FORMAT_R8_UNORM, KF_A8B8G8R8, 1, 0, 0, NARROW_R8},
    {DXGI_FORMAT_R8G8_TYPELESS, KF_A8B8G8R8, 2, 0, 0, NARROW_R8G8}, {DXGI_FORMAT_R8G8_UNORM, KF_A8B8G8R8, 2, 0, 0, NARROW_R8G8},
    {DXGI_FORMAT_R16_TYPELESS, KF_RGBA16, 2, 0, 0, NARROW_R16}, {DXGI_FORMAT_R16_UNORM, KF_RGBA16, 2, 0, 0, NARROW_R16},
    {DXGI_FORMAT_R16G16_TYPELESS, KF_RGBA16, 4, 0, 0, NARROW_R16G16}, {DXGI_FORMAT_R16G16_UNORM, KF_RGBA16, 4, 0, 0, NARROW_R16G16},
    {DXGI_FORMAT_NV12, KF_A8B8G8R8, 1, 0, 0, NARROW_R8, PLANAR_8}, {DXGI_FORMAT_P010, KF_RGBA16, 2, 0, 0, NARROW_R16, PLANAR_16}, {DXGI_FORMAT_P016, KF_RGBA16, 2, 0, 0, NARROW_R16, PLANAR_16},
    /* the packed YUV formats, kept as the RGB format of their views (DXGI_FORMAT: AYUV is R8G8B8A8 - V, U, Y, A -, Y410 R10G10B10A2 - U, Y, V, A -, Y416 R16G16B16A16 - U, Y, V,
     * A -, YUY2 R8G8B8A8 and Y210 / Y216 R16G16B16A16 of half the width - Y0, U, Y1, V), and the planar 4:2:2 / 4:4:0 / 4:4:4 ones of Direct3D 12 (P208, V208, V408) */
    {DXGI_FORMAT_AYUV, KF_A8B8G8R8, 4, 0}, {DXGI_FORMAT_Y410, KF_RGB10A2, 4, 0}, {DXGI_FORMAT_Y416, KF_RGBA16, 8, 0},
    {DXGI_FORMAT_YUY2, KF_A8B8G8R8, 4, 0, 0, 0, 0, 1}, {DXGI_FORMAT_Y210, KF_RGBA16, 8, 0, 0, 0, 0, 1}, {DXGI_FORMAT_Y216, KF_RGBA16, 8, 0, 0, 0, 0, 1},
    {DXGI_FORMAT_P208, KF_A8B8G8R8, 1, 0, 0, NARROW_R8, PLANAR_P208}, {DXGI_FORMAT_V208, KF_A8B8G8R8, 1, 0, 0, NARROW_R8, PLANAR_V208},
    {DXGI_FORMAT_V408, KF_A8B8G8R8, 1, 0, 0, NARROW_R8, PLANAR_V408},
};
/* The bytes of a row of a level `w` pixels wide (of blocks for a compressed format), and the rows of a level `h` high. */
static UINT64 format_row_bytes(const FormatInfo *fi, UINT w) {
  return fi && fi->compressed ? (UINT64)((w + 3) / 4) * fi->bytes : fi && fi->pair ? (UINT64)((w + 1) / 2) * fi->bytes : (UINT64)w * (fi ? fi->bytes : 4);
}
/* A column and a width of a texture in pixels of the kernel's image (a packed 4:2:2 format's are pairs). */
static UINT format_kx(const FormatInfo *fi, UINT x) { return fi && fi->pair ? x / 2 : x; }
static UINT format_kw(const FormatInfo *fi, UINT w) { return fi && fi->pair ? (w + 1) / 2 : w; }
/* A packed YUV format (AYUV, Y410, Y416, YUY2, Y210, Y216): a view of it has the format of its pixels (format_info's kernel format), never the YUV one. */
static int format_packed_yuv(DXGI_FORMAT f) {
  return f == DXGI_FORMAT_AYUV || f == DXGI_FORMAT_Y410 || f == DXGI_FORMAT_Y416 || f == DXGI_FORMAT_YUY2 || f == DXGI_FORMAT_Y210 || f == DXGI_FORMAT_Y216;
}
/* Whether `view` is a format a view of a texture of the packed YUV format `res` may have (DXGI_FORMAT: R8G8B8A8_UNORM / _UINT for AYUV and YUY2, R10G10B10A2_UNORM /
 * _UINT for Y410, R16G16B16A16_UNORM / _UINT for Y416, Y210, Y216), and as a render target (AYUV's views alone: the others are SRV and UAV formats). The R32_UINT UAV view
 * and YUY2's R8G8_B8G8_UNORM view are not offered: the kernel has no 32-bit integer view of a four-channel image and no hardware RGB reconstruction. */
static int packed_yuv_view_ok(DXGI_FORMAT res, DXGI_FORMAT view, int render_target) {
  switch (res) {
    case DXGI_FORMAT_AYUV: return view == DXGI_FORMAT_R8G8B8A8_UNORM || view == DXGI_FORMAT_R8G8B8A8_UINT;
    case DXGI_FORMAT_YUY2: return !render_target && (view == DXGI_FORMAT_R8G8B8A8_UNORM || view == DXGI_FORMAT_R8G8B8A8_UINT);
    case DXGI_FORMAT_Y410: return !render_target && (view == DXGI_FORMAT_R10G10B10A2_UNORM || view == DXGI_FORMAT_R10G10B10A2_UINT);
    case DXGI_FORMAT_Y416: case DXGI_FORMAT_Y210: case DXGI_FORMAT_Y216: return !render_target && (view == DXGI_FORMAT_R16G16B16A16_UNORM || view == DXGI_FORMAT_R16G16B16A16_UINT);
    default: return 1;
  }
}
static UINT format_rows(const FormatInfo *fi, UINT h) { return fi && fi->compressed ? (h + 3) / 4 : h; }

static const FormatInfo *format_info(DXGI_FORMAT f) {
  for (UINT i = 0; i < sizeof g_formats / sizeof g_formats[0]; ++i)
    if (g_formats[i].dxgi == f) return &g_formats[i];
  return NULL;
}
/* The planes of a planar format (0 for another). */
static UINT format_planes(const FormatInfo *fi) { return !fi || !fi->planar ? 0 : fi->planar == PLANAR_V208 || fi->planar == PLANAR_V408 ? 3 : 2; }
/* The size of plane `plane` of a level `*w` x `*h` of a planar format: the chroma of NV12 / P010 / P016 half both ways, of P208 half as wide, of V208 half as high. */
static void plane_extent(const FormatInfo *fi, UINT plane, UINT *w, UINT *h) {
  if (!fi || !fi->planar || plane == 0 || fi->planar == PLANAR_V408) return;
  if (fi->planar != PLANAR_V208) *w = (*w + 1) / 2;
  if (fi->planar != PLANAR_P208) *h = (*h + 1) / 2;
}
/* A plane of a planar format, as the format of its footprints and views (R8 / R8G8, or R16 / R16G16): plane 0 the luma, plane 1 the chroma; V208 / V408: three R8 planes. */
static const FormatInfo *plane_format(const FormatInfo *fi, UINT plane) {
  if (!fi || !fi->planar) return fi;
  if (fi->planar == PLANAR_V208 || fi->planar == PLANAR_V408) return format_info(DXGI_FORMAT_R8_TYPELESS);
  const int sixteen = fi->planar == PLANAR_16;
  return format_info(plane == 0 ? (sixteen ? DXGI_FORMAT_R16_TYPELESS : DXGI_FORMAT_R8_TYPELESS) : (sixteen ? DXGI_FORMAT_R16G16_TYPELESS : DXGI_FORMAT_R8G8_TYPELESS));
}
/* The channels a render target of `f` has, as a write mask (an R8 target of the kernel's four-channel image writes red alone). */
static UINT format_write_mask(DXGI_FORMAT f) {
  const FormatInfo *fi = format_info(f);
  if (!fi || !fi->narrow) return 0xF;
  return fi->narrow == NARROW_R8 || fi->narrow == NARROW_R16 ? 0x1 : 0x3;
}
/* The bytes of a pixel of any format a buffer view or a vertex attribute may name (not only the ones a texture can have). */
static UINT format_bytes(DXGI_FORMAT f) {
  const FormatInfo *i = format_info(f);
  if (i) return i->bytes;
  switch (f) {
    case DXGI_FORMAT_R32G32B32_FLOAT: case DXGI_FORMAT_R32G32B32_UINT: case DXGI_FORMAT_R32G32B32_SINT: return 12;
    case DXGI_FORMAT_R32G32_FLOAT: case DXGI_FORMAT_R32G32_UINT: case DXGI_FORMAT_R32G32_SINT: case DXGI_FORMAT_R16G16B16A16_FLOAT: return 8;
    case DXGI_FORMAT_R32_FLOAT: case DXGI_FORMAT_R32_UINT: case DXGI_FORMAT_R32_SINT: case DXGI_FORMAT_R32_TYPELESS: case DXGI_FORMAT_R16G16_FLOAT: case DXGI_FORMAT_R16G16_UNORM: return 4;
    case DXGI_FORMAT_R16_FLOAT: case DXGI_FORMAT_R16_UNORM: case DXGI_FORMAT_R16_UINT: case DXGI_FORMAT_R16_SINT: return 2;
    case DXGI_FORMAT_R8_UNORM: case DXGI_FORMAT_R8_UINT: case DXGI_FORMAT_A8_UNORM: return 1;
    default: return 0;
  }
}

/* ---- the objects' common part -------------------------------------------------------------------------------------------------------------------------------------------- */

/* Every object of the device: the head, then the device (a reference: a child keeps its device alive). */
typedef struct DObj {
  DxObj o;
  Dev *dev;
} DObj;

#define DOBJ(x) ((DObj *)(x))
#define OBJ(x) ((DxObj *)(x))

static CRITICAL_SECTION g_lock;      /* the table of GPU addresses and the resources that need their CPU copy sent to the kernel */
static LONG g_desc_epoch = 1;        /* bumped by every descriptor written: a shader-visible heap whose copy in the kernel is older is sent again */

#define obj_QueryInterface dx_QueryInterface
#define obj_AddRef dx_AddRef
#define obj_Release dx_Release
#define obj_SetPrivateData dx_SetPrivateData
#define obj_SetPrivateDataInterface dx_SetPrivateDataInterface
#define obj_GetPrivateData dx_GetPrivateData

static HRESULT STDMETHODCALLTYPE dobj_SetName(DxObj *o, LPCWSTR name) { (void)o; (void)name; return S_OK; }

/* A new object of `size` bytes with a reference on the device. */
static void dev_addref(Dev *d);
static void dev_release(Dev *d);

static void dobj_child_destroy(DxObj *o) {
  DObj *d = (DObj *)o;
  if (d->dev) dev_release(d->dev);
}

/* ---- GPU virtual addresses --------------------------------------------------------------------------------------------------------------------------------------------- */

/* A buffer's address is made up (the kernel has none): a range of 64 KiB aligned addresses per buffer, found again by a binary search. */
typedef struct VaEntry { UINT64 va, size; Resource *res; } VaEntry;
static VaEntry *g_va;
static UINT g_va_count, g_va_cap;
static UINT64 g_next_va = (UINT64)1 << 32;

static UINT64 va_register(Resource *r, UINT64 size) {
  EnterCriticalSection(&g_lock);
  if (g_va_count == g_va_cap) {
    const UINT cap = g_va_cap ? g_va_cap * 2 : 64;
    VaEntry *n = mem_grow(g_va, cap * sizeof *n);
    if (!n) { LeaveCriticalSection(&g_lock); return 0; }
    g_va = n;
    g_va_cap = cap;
  }
  const UINT64 va = g_next_va;
  g_next_va += KALIGN(size ? size : 1, KTILE_BYTES) + KTILE_BYTES;
  g_va[g_va_count].va = va;
  g_va[g_va_count].size = size;
  g_va[g_va_count].res = r;
  g_va_count++;
  LeaveCriticalSection(&g_lock);
  return va;
}
static void va_unregister(UINT64 va) {
  EnterCriticalSection(&g_lock);
  for (UINT i = 0; i < g_va_count; ++i)
    if (g_va[i].va == va) { memmove(&g_va[i], &g_va[i + 1], (g_va_count - i - 1) * sizeof g_va[0]); g_va_count--; break; }
  LeaveCriticalSection(&g_lock);
}
/* The buffer an address is in, and the offset in it. */
static Resource *va_find(UINT64 va, UINT64 *offset) {
  Resource *found = NULL;
  EnterCriticalSection(&g_lock);
  UINT lo = 0, hi = g_va_count;
  while (lo < hi) {
    const UINT mid = (lo + hi) / 2;
    if (g_va[mid].va <= va) lo = mid + 1;
    else hi = mid;
  }
  if (lo > 0 && va - g_va[lo - 1].va < g_va[lo - 1].size + 1) {
    found = g_va[lo - 1].res;
    *offset = va - g_va[lo - 1].va;
  }
  LeaveCriticalSection(&g_lock);
  return found;
}

/* ---- escape requests ---------------------------------------------------------------------------------------------------------------------------------------------------- */

enum {
  ESC_CREATE_SHADER = 1, ESC_CREATE_PIPELINE = 2, ESC_DESTROY_SHADER = 3, ESC_DESTROY_PIPELINE = 4, ESC_CREATE_COMPUTE_PIPELINE = 5, ESC_CREATE_ROOT_SIGNATURE = 6,
  ESC_DESTROY_ROOT_SIGNATURE = 7, ESC_CREATE_DESCRIPTOR_HEAP = 8, ESC_DESTROY_DESCRIPTOR_HEAP = 9, ESC_WRITE_DESCRIPTORS = 10, ESC_COPY_DESCRIPTORS = 11, ESC_UPDATE_TILE_MAPPINGS = 12,
  ESC_QUERY_SYNC = 13, ESC_QUERY_STREAM_OUTPUT = 14, ESC_SET_MIRROR = 18,
};

static UINT bits_of(float f) { UINT u; memcpy(&u, &f, 4); return u; }

/* ---- the device, as far as the other parts need it ---------------------------------------------------------------------------------------------------------------------- */

struct Dev {
  DxObj o;
  LUID luid;
  UINT adapter;            /* the kernel's handle of the adapter */
  UINT kdev;               /* ... and of the device made on it */
  KmtCaps caps;
  UINT64 segments[3];
  int software;
  LONG removed;
  UINT null_target;        /* a 1 x 1 render target: the pass of a draw that has no target (stream output only) */
  /* The colour targets of passes that have a depth buffer and no render target (cmdlist.inc depth_companion): one for each depth buffer drawn into that way, until it is destroyed. */
  struct Companion { UINT depth, color; } *companions;
  UINT companion_count, companion_capacity;
  SRWLOCK companion_lock;
  struct SharedShader *shared;      /* the kernel's shaders the pipelines have, each once (pso.inc) */
  UINT shared_count, shared_capacity;
  UINT next_pixel_base;    /* TcosD3D12PipelineOptions: for the next graphics pipeline, where the pixel shader's constant buffers begin (16-byte registers) */
  UINT next_shader_kind;   /* ... and the kernel's kind of its shaders (0: DXBC / DXIL) */
  UINT next_stage_base[3]; /* TcosD3D12StageConstantBases: ... where the geometry, hull and domain shaders' constant buffers begin */
  UINT next_resource_flags;/* TcosD3D12ResourceOptions: for the next resource made (TCOS_RESOURCE_CUBE) */
  BYTE next_share_extra[64];      /* TcosD3D12ShareExtra: what the next shared resource carries for the runtime above (d3d11.dll's description) */
  UINT next_share_extra_size;
  struct Fence *fences;    /* the device's fences (RemoveDevice signals them all) */
  SRWLOCK fence_lock;
  struct ShaderCache *caches;      /* the shader cache sessions open (library.inc), for ShaderCacheControl */
  SRWLOCK cache_lock;
  ID3D12VideoDevice3 video;        /* the same object as ID3D12VideoDevice .. 3 (video.inc) */
};
#define DEV_FEATURE_STREAM_OUTPUT (1u << 21)      /* graphics::kFeatureStreamOutput in KmtCaps.features */
#define DEV_FEATURE_DEPTH_RESOLVE (1u << 19)      /* graphics::kFeatureDepthResolve */
#define DEV_FEATURE_RESOLVE_MODES (1u << 20)      /* graphics::kFeatureResolveModes: resolves of the least, greatest (and a depth image's mean) samples */
#define DEV_FEATURE_VERTEX_FORMATS (1u << 29)     /* graphics::kFeatureVertexFormats: vertex elements other than 32-bit floats */

static const GUID *const g_device_iids[] = {&IID_ID3D12Object, &IID_ID3D12Device, &IID_ID3D12Device1, &IID_ID3D12Device2, &IID_ID3D12Device3, &IID_ID3D12Device4,
                                            &IID_ID3D12Device5, &IID_ID3D12Device6, &IID_ID3D12Device7, &IID_ID3D12Device8, &IID_ID3D12Device9, &IID_ID3D12Device10, NULL};

static void dev_addref(Dev *d) { dx_AddRef(OBJ(d)); }
static void dev_release(Dev *d) { dx_Release(OBJ(d)); }

static void *dobj_new(SIZE_T size, Dev *dev, const void *vtbl, const GUID *const *iids, void (*destroy)(DxObj *)) {
  DObj *o = mem_alloc(size);
  if (!o) return NULL;
  dx_init(&o->o, vtbl, iids, destroy);
  o->dev = dev;
  dev_addref(dev);
  return o;
}

static HRESULT STDMETHODCALLTYPE dobj_GetDevice(DObj *o, REFIID riid, void **out) {
  if (!out) return E_POINTER;
  return dx_QueryInterface(OBJ(o->dev), riid, out);
}

/* ---- escapes ----------------------------------------------------------------------------------------------------------------------------------------------------------------- */

/* Sends `in_words` words of request to the kernel's private escape, and takes `out_words` words of reply from the same buffer. */
static NTSTATUS dev_escape(Dev *d, const UINT *in, UINT in_words, UINT *out, UINT out_words) {
  const UINT words = in_words > out_words ? in_words : out_words;
  UINT *buffer = mem_alloc((SIZE_T)(words ? words : 1) * 4 + 16);
  if (!buffer) return (NTSTATUS)0xC0000017;
  memcpy(buffer, in, (SIZE_T)in_words * 4);
  const NTSTATUS st = kmt_escape(d->adapter, d->kdev, buffer, words * 4);
  if (KMT_OK(st) && out) memcpy(out, buffer, (SIZE_T)out_words * 4);
  mem_free(buffer);
  return st;
}

static HRESULT hr_from(NTSTATUS st) {
  if (KMT_OK(st)) return S_OK;
  switch ((UINT)st) {
    case 0xC0000017: case 0xC01E0100: case 0xC000009A: return E_OUTOFMEMORY;
    case 0xC00000BB: return DXGI_ERROR_UNSUPPORTED;
    case 0xC000000D: case 0xC0000008: return E_INVALIDARG;
    default: return E_FAIL;
  }
}

/* ---- the factory functions ---------------------------------------------------------------------------------------------------------------------------------------------- */

static HRESULT query_out(IUnknown *object, REFIID riid, void **out) {
  const HRESULT hr = out ? object->lpVtbl->QueryInterface(object, riid, out) : S_FALSE;
  object->lpVtbl->Release(object);      /* (the creation's own reference: the output has its own, or there is none to give) */
  return hr;
}

