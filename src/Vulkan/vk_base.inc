/* The Vulkan driver, part 1: strings, the D3DKMT calls, the objects, the formats. (memcpy, memset, memmove and the heap are the platform's: vk_os.h.) */

/* ---- strings ------------------------------------------------------------------------------------------------------------------------------------------------------------ */
static int streq(const char *a, const char *b) {
  while (*a && *a == *b) ++a, ++b;
  return *a == *b;
}
static size_t cstrlen(const char *s) {
  size_t n = 0;
  while (s[n]) ++n;
  return n;
}
static void copy_string(char *dst, size_t cap, const char *src) {
  size_t i = 0;
  for (; i + 1 < cap && src[i]; ++i) dst[i] = src[i];
  dst[i] = 0;
}

static void *vk_realloc_array(void *old, size_t old_count, size_t new_count, size_t elem) {
  if (elem && new_count > SIZE_MAX / elem) return NULL;
  void *n = vk_alloc(new_count * elem);
  if (n && old) memcpy(n, old, old_count * elem);
  if (n) vk_free(old);
  return n;
}
/* Appends to a growing array (the item is copied); false when memory ran out. */
#define LIST_PUSH(list, count, cap, item)                                                       \
  (((count) < (cap) || ((list) = vk_realloc_array((list), (count), (cap) = (cap) ? (cap) * 2 : 8, sizeof((list)[0])))) ? ((list)[(count)++] = (item), 1) : 0)

#define SP_MAX_RES_KEYS 32
#define MAX_ATTRIBUTES 16   /* vertex attributes of a pipeline (graphics::kMaxVertexAttributes) */
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))

/* ---- the D3DKMT services: kmt() of the platform (vk_os.h). Structures are byte blocks with the offsets of Windows' d3dkmthk.h (src/Process/Win32k/Dxg/DxgLayouts.hpp) on
 * every platform: the kernel reads them the same way for a Windows and a Linux process. */
typedef uint32_t KMT_HANDLE;

typedef struct KmtBlock {
  uint8_t b[128];
} KmtBlock;
static void put32(void *p, unsigned off, uint32_t v) { memcpy((uint8_t *)p + off, &v, 4); }
static void put64(void *p, unsigned off, uint64_t v) { memcpy((uint8_t *)p + off, &v, 8); }
static uint32_t get32(const void *p, unsigned off) {
  uint32_t v;
  memcpy(&v, (const uint8_t *)p + off, 4);
  return v;
}
static uint64_t get64(const void *p, unsigned off) {
  uint64_t v;
  memcpy(&v, (const uint8_t *)p + off, 8);
  return v;
}

#define STATUS_OK(s) ((int32_t)(s) >= 0)

/* The adapter's capabilities blob (QueryAdapterInfo type 0, src/Process/Win32k/Dxg/DxgAdapters.cpp). */
typedef struct Caps {
  uint32_t max_texture_dimension, max_vertex_attributes, max_varyings, max_texture_units, max_push_constant_bytes, max_vertex_stride, flags, features;
  char name[64];
  uint32_t sample_counts, device_kind, vendor_id, device_id;
} Caps;

enum { CAP_ACCELERATED = 1, CAP_STENCIL = 2, CAP_MIPMAPS = 4, CAP_TEXTURES = 8, CAP_PROGRAMMABLE_VERTEX = 16, CAP_PROGRAMMABLE_PIXEL = 32 };
/* graphics::DeviceFeature bits (src/Graphics/Graphics3D/IGraphicsDevice.hpp) */
enum {
  FEAT_BLEND_STATE = 1u << 0, FEAT_TEXEL_ACCESS = 1u << 1, FEAT_SYSTEM_INPUTS = 1u << 2, FEAT_INSTANCING = 1u << 3, FEAT_LINES = 1u << 4, FEAT_MULTIPLE_TARGETS = 1u << 5, FEAT_FLOAT_FORMATS = 1u << 6,
  FEAT_TEXTURE_KINDS = 1u << 8, FEAT_INDEPENDENT_BLEND = 1u << 9, FEAT_COMPUTE = 1u << 10, FEAT_MULTISAMPLE = 1u << 11, FEAT_SRGB = 1u << 13, FEAT_UNORM16 = 1u << 14, FEAT_INTEGER = 1u << 15,
  FEAT_SNORM = 1u << 17, FEAT_STORAGE_IN_DRAWS = 1u << 23, FEAT_FRAGMENT_STORES = 1u << 24, FEAT_VERTEX_STORES = 1u << 25
};
/* The kernel's storage buffer slots (graphics::kMaxUavBuffers, kMaxSrvBuffers): u0 .. u3 are read and written, the read-only views t0 .. t3 are the slots after them. */
enum { UAV_SLOTS = 4, SRV_SLOTS = 4 };
/* dxgk::AllocationFlags and dxgk::Format */
enum { A_VERTEX = 1u << 0, A_INDEX = 1u << 1, A_TEXTURE = 1u << 2, A_TARGET = 1u << 3, A_DEPTH = 1u << 4, A_PRESENT = 1u << 5, A_STAGING = 1u << 6, A_SHARED = 1u << 7, A_UAV = 1u << 9 };
enum {
  DF_UNKNOWN = 0, DF_A8R8G8B8, DF_X8R8G8B8, DF_A8B8G8R8, DF_X8B8G8R8, DF_D32F, DF_D24S8, DF_RGBA16F, DF_RGBA32F, DF_A8R8G8B8_SRGB, DF_A8B8G8R8_SRGB, DF_RGBA16, DF_RGBA32UI, DF_RGBA32SI,
  DF_RGBA16UI, DF_RGBA16SI, DF_RGBA8UI, DF_RGBA16SN, DF_A2B10G10R10
};

/* ---- objects -------------------------------------------------------------------------------------------------------------------------------------------------------------- */
#define MAGIC_INSTANCE 0x56494E53u
#define MAGIC_PHYS 0x56504859u
#define MAGIC_DEVICE 0x56444556u
#define MAGIC_QUEUE 0x56515545u
#define MAGIC_CMD 0x56434D44u

typedef struct Hdr {
  void *dispatch;   /* the loader's slot: ICD_LOADER_MAGIC when made, which the Khronos loader replaces with its dispatch table (an installable client driver); nothing here reads it */
  uint32_t magic;
} Hdr;
#define ICD_LOADER_MAGIC ((void *)(uintptr_t)0x01CDC0DEu)

struct Instance;
typedef struct PhysDev {
  Hdr h;
  struct Instance *inst;
  KMT_HANDLE adapter;
  struct { uint32_t LowPart; int32_t HighPart; } luid;   /* (the adapter's LUID, as Windows lays it out) */
  Caps caps;
  uint64_t local_memory;
} PhysDev;

typedef struct Instance {
  Hdr h;
  PhysDev *pd;
  uint32_t npd;
  uint32_t api_version;
} Instance;

struct Queue;
typedef struct Device {
  Hdr h;
  PhysDev *pd;
  KMT_HANDLE dev, ctx;
  uint8_t *dma;               /* the context's command buffer (D3DKMTCreateContext maps it) */
  uint32_t dma_bytes;
  uint8_t *alloc_list;        /* ... its allocation list (8 bytes an entry) */
  uint32_t alloc_entries;
  struct Queue *queue;
} Device;

typedef struct Queue {
  Hdr h;
  Device *dev;
} Queue;

typedef struct Mem {
  Device *dev;
  uint64_t size;
  uint8_t *data;
  uint32_t type;
  uint32_t version;            /* counts the writes the CPU may have made: a buffer or image synchronised with an older one is stale */
  bool mapped;
  uint32_t seen_submission;    /* the submission in which a mapped memory was last counted as written (g_submission) */
  struct Img *image;           /* the image it is dedicated to (VkMemoryDedicatedAllocateInfo), else the first one bound to it: what vkGetMemoryFdKHR exports */
  bool exportable;             /* made with VkExportMemoryAllocateInfo */
} Mem;

typedef struct Buf {
  Device *dev;
  uint64_t size;
  uint32_t usage;
  Mem *mem;
  uint64_t off;
  KMT_HANDLE alloc;        /* made when the GPU first needs the buffer */
  uint32_t synced;
  uint8_t *sent;              /* a buffer in mapped memory: what was last copied to the GPU (size bytes), so that a submission that finds it unchanged copies nothing */
} Buf;

typedef struct Img {
  Device *dev;
  VkImageType type;
  VkFormat format;
  uint32_t dfmt, bpp;
  uint32_t w, h, depth, mips, layers, samples;
  VkImageTiling tiling;
  VkImageUsageFlags usage;
  uint32_t flags;
  Mem *mem;
  uint64_t off;
  KMT_HANDLE alloc;
  uint32_t pitch;
  uint32_t kind;               /* graphics::TextureKind */
  bool gpu_written;
  bool swapchain;
  uint32_t synced;
  bool is_depth;
  bool swap_rb;
  bool external;               /* VkExternalMemoryImageCreateInfo: its allocation is shareable (and sampled, so that a compositor draws it in place) */
} Img;

typedef struct View {
  Img *img;
  VkImageViewType type;
  VkFormat format;
  uint32_t base_mip, nmips, base_layer, nlayers;
} View;

typedef struct Sampler {
  uint32_t filter, mip_filter, addr_u, addr_v;
  float lod_bias, min_lod, max_lod;
} Sampler;

typedef struct Shader {
  uint32_t *words;
  uint32_t nwords;
} Shader;

typedef struct Attachment {
  VkFormat format;
  uint32_t samples;
  VkAttachmentLoadOp load, stencil_load;
  VkAttachmentStoreOp store;
} Attachment;

typedef struct Subpass {
  uint32_t ncolor;
  uint32_t color[4];
  uint32_t depth;              /* attachment index or ~0 */
  uint32_t resolve[4];
  uint32_t nresolve;
} Subpass;

typedef struct RenderPass {
  Device *dev;
  Attachment att[9];
  uint32_t natt;
  Subpass sub[4];
  uint32_t nsub;
} RenderPass;

typedef struct Framebuffer {
  Device *dev;
  RenderPass *pass;
  View *view[9];
  uint32_t nview;
  uint32_t w, h, layers;
} Framebuffer;

typedef struct DescBinding {
  uint32_t binding;
  VkDescriptorType type;
  uint32_t count;
  VkShaderStageFlags stages;
} DescBinding;

typedef struct DSetLayout {
  Device *dev;
  DescBinding *b;
  uint32_t nb;
} DSetLayout;

typedef struct Descr {
  Buf *buf;
  uint64_t off, range;
  View *view;
  Sampler *sampler;
} Descr;

typedef struct DescSet {
  Device *dev;
  DSetLayout *layout;
  uint32_t *first;             /* per binding: the index of its first descriptor in `d` */
  Descr *d;
  uint32_t nd;
} DescSet;

typedef struct PipeLayout {
  Device *dev;
  DSetLayout *sets[8];
  uint32_t nsets;
  uint32_t push_bytes;
} PipeLayout;

/* What a pipeline needs of the descriptors it is drawn with. */
typedef struct CbSlot {
  uint32_t kind;               /* SPV_RES_UNIFORM or SPV_RES_PUSH */
  uint32_t set, binding;
  uint32_t vec4;
} CbSlot;
typedef struct TexUnit {
  uint32_t set, binding, element;
  uint32_t sampler_set, sampler_binding;   /* a separate sampler (0xFFFFFFFF: the descriptor is a combined image sampler) */
} TexUnit;

/* A storage buffer of the pipeline: the descriptor (set, binding, array element) and the kernel's slot it is bound to (below UAV_SLOTS: read and written). */
typedef struct StorageSlot {
  uint32_t set, binding, element;
  uint32_t slot;
} StorageSlot;

typedef struct VtxBinding {
  uint32_t binding, stride;
  bool per_instance;
} VtxBinding;
typedef struct VtxAttr {
  uint32_t location, binding, offset;
  VkFormat format;
} VtxAttr;

typedef struct Pipe {
  Device *dev;
  PipeLayout *layout;
  KMT_HANDLE vs, fs, pipe;
  VtxBinding bindings[8];
  uint32_t nbindings;
  VtxAttr attrs[MAX_ATTRIBUTES];
  uint32_t nattrs;
  uint32_t out_stride;         /* of the gathered vertex buffer: the sum of the attributes' float components */
  uint32_t out_offset[MAX_ATTRIBUTES];
  uint32_t out_format[MAX_ATTRIBUTES];      /* graphics::VertexFormat: Float1 .. Float4 */
  uint32_t topology;
  bool dyn_viewport, dyn_scissor;
  VkViewport viewport;
  VkRect2D scissor;
  CbSlot cb[16];
  uint32_t ncb;
  TexUnit unit[SP_MAX_UNITS];
  uint32_t nunit;
  StorageSlot storage[UAV_SLOTS + SRV_SLOTS];
  uint32_t nstorage;
  bool compute;            /* a compute pipeline: `vs` is its shader */
  uint32_t color_targets;
  uint32_t subpass;
  uint32_t stencil_ref;
} Pipe;

/* ---- formats ------------------------------------------------------------------------------------------------------------------------------------------------------------- */
typedef struct FormatInfo {
  VkFormat vk;
  uint32_t dfmt;
  uint32_t bpp;
  uint32_t need;               /* DeviceFeature bits */
  uint32_t swap_rb;            /* the bytes of Vulkan's format have red and blue the other way round from the allocation's format: the CPU swaps them on the way in and out */
} FormatInfo;

static const FormatInfo kFormats[] = {
    {VK_FORMAT_B8G8R8A8_UNORM, DF_A8R8G8B8, 4, 0},
    /* (the adapters differ in what they take of the byte order RGBA: a texture of it is stored as BGRA, which all of them take) */
    {VK_FORMAT_R8G8B8A8_UNORM, DF_A8R8G8B8, 4, 0, 1},
    {VK_FORMAT_B8G8R8A8_SRGB, DF_A8R8G8B8_SRGB, 4, FEAT_SRGB},
    {VK_FORMAT_R8G8B8A8_SRGB, DF_A8R8G8B8_SRGB, 4, FEAT_SRGB, 1},
    {VK_FORMAT_R8G8B8A8_UINT, DF_RGBA8UI, 4, FEAT_INTEGER},
    {VK_FORMAT_R16G16B16A16_SFLOAT, DF_RGBA16F, 8, FEAT_FLOAT_FORMATS},
    {VK_FORMAT_R32G32B32A32_SFLOAT, DF_RGBA32F, 16, FEAT_FLOAT_FORMATS},
    {VK_FORMAT_R16G16B16A16_UNORM, DF_RGBA16, 8, FEAT_UNORM16},
    {VK_FORMAT_R16G16B16A16_SNORM, DF_RGBA16SN, 8, FEAT_SNORM},
    {VK_FORMAT_R16G16B16A16_UINT, DF_RGBA16UI, 8, FEAT_INTEGER},
    {VK_FORMAT_R16G16B16A16_SINT, DF_RGBA16SI, 8, FEAT_INTEGER},
    {VK_FORMAT_R32G32B32A32_UINT, DF_RGBA32UI, 16, FEAT_INTEGER},
    {VK_FORMAT_R32G32B32A32_SINT, DF_RGBA32SI, 16, FEAT_INTEGER},
    {VK_FORMAT_A2B10G10R10_UNORM_PACK32, DF_A2B10G10R10, 4, FEAT_SNORM},
    {VK_FORMAT_D32_SFLOAT, DF_D32F, 4, 0},
    {VK_FORMAT_D24_UNORM_S8_UINT, DF_D24S8, 4, 0},
};

static const FormatInfo *format_info(const PhysDev *pd, VkFormat f) {
  for (uint32_t i = 0; i < sizeof kFormats / sizeof kFormats[0]; ++i)
    if (kFormats[i].vk == f) return (kFormats[i].need & ~pd->caps.features) ? NULL : &kFormats[i];
  return NULL;
}
static void swap_rb_pixels(uint8_t *p, uint64_t count) {
  for (uint64_t i = 0; i < count; ++i, p += 4) {
    uint8_t t = p[0];
    p[0] = p[2], p[2] = t;
  }
}
static bool format_is_depth(VkFormat f) { return f == VK_FORMAT_D32_SFLOAT || f == VK_FORMAT_D24_UNORM_S8_UINT; }

/* Vertex attribute formats: how many float components the device gets, and a converter from the bytes in the buffer. */
typedef struct VtxFormat {
  VkFormat vk;
  uint32_t bytes, comps;
  uint32_t kind;               /* 0 float32, 1 unorm8, 2 snorm8, 3 uint8, 4 sint8, 5 float16, 6 unorm16, 7 snorm16, 8 uint16, 9 sint16, 10 uint32, 11 sint32 */
} VtxFormat;
static const VtxFormat kVertexFormats[] = {
    {VK_FORMAT_R32_SFLOAT, 4, 1, 0}, {VK_FORMAT_R32G32_SFLOAT, 8, 2, 0}, {VK_FORMAT_R32G32B32_SFLOAT, 12, 3, 0}, {VK_FORMAT_R32G32B32A32_SFLOAT, 16, 4, 0},
    {VK_FORMAT_R8_UNORM, 1, 1, 1}, {VK_FORMAT_R8G8_UNORM, 2, 2, 1}, {VK_FORMAT_R8G8B8A8_UNORM, 4, 4, 1}, {VK_FORMAT_R8G8B8_UNORM, 3, 3, 1},
    {VK_FORMAT_R8_SNORM, 1, 1, 2}, {VK_FORMAT_R8G8_SNORM, 2, 2, 2}, {VK_FORMAT_R8G8B8A8_SNORM, 4, 4, 2},
    {VK_FORMAT_R8_UINT, 1, 1, 3}, {VK_FORMAT_R8G8_UINT, 2, 2, 3}, {VK_FORMAT_R8G8B8A8_UINT, 4, 4, 3},
    {VK_FORMAT_R8_SINT, 1, 1, 4}, {VK_FORMAT_R8G8_SINT, 2, 2, 4}, {VK_FORMAT_R8G8B8A8_SINT, 4, 4, 4},
    {VK_FORMAT_R16_SFLOAT, 2, 1, 5}, {VK_FORMAT_R16G16_SFLOAT, 4, 2, 5}, {VK_FORMAT_R16G16B16A16_SFLOAT, 8, 4, 5},
    {VK_FORMAT_R16_UNORM, 2, 1, 6}, {VK_FORMAT_R16G16_UNORM, 4, 2, 6}, {VK_FORMAT_R16G16B16A16_UNORM, 8, 4, 6},
    {VK_FORMAT_R16_SNORM, 2, 1, 7}, {VK_FORMAT_R16G16_SNORM, 4, 2, 7}, {VK_FORMAT_R16G16B16A16_SNORM, 8, 4, 7},
    {VK_FORMAT_R16_UINT, 2, 1, 8}, {VK_FORMAT_R16G16_UINT, 4, 2, 8}, {VK_FORMAT_R16G16B16A16_UINT, 8, 4, 8},
    {VK_FORMAT_R16_SINT, 2, 1, 9}, {VK_FORMAT_R16G16_SINT, 4, 2, 9}, {VK_FORMAT_R16G16B16A16_SINT, 8, 4, 9},
    {VK_FORMAT_R32_UINT, 4, 1, 10}, {VK_FORMAT_R32G32_UINT, 8, 2, 10}, {VK_FORMAT_R32G32B32_UINT, 12, 3, 10}, {VK_FORMAT_R32G32B32A32_UINT, 16, 4, 10},
    {VK_FORMAT_R32_SINT, 4, 1, 11}, {VK_FORMAT_R32G32_SINT, 8, 2, 11}, {VK_FORMAT_R32G32B32_SINT, 12, 3, 11}, {VK_FORMAT_R32G32B32A32_SINT, 16, 4, 11},
};
static const VtxFormat *vertex_format(VkFormat f) {
  for (uint32_t i = 0; i < sizeof kVertexFormats / sizeof kVertexFormats[0]; ++i)
    if (kVertexFormats[i].vk == f) return &kVertexFormats[i];
  return NULL;
}

static float half_to_float(uint32_t h) {
  uint32_t sign = (h >> 15) & 1, exp = (h >> 10) & 31, man = h & 1023;
  union { float f; uint32_t u; } r;
  if (exp == 0) {
    if (man == 0) {
      r.u = sign << 31;
    } else {   /* a subnormal */
      exp = 1;
      while (!(man & 1024)) man <<= 1, --exp;
      man &= 1023;
      r.u = (sign << 31) | ((exp + 112) << 23) | (man << 13);
    }
  } else if (exp == 31) {
    r.u = (sign << 31) | 0x7F800000u | (man << 13);
  } else {
    r.u = (sign << 31) | ((exp + 112) << 23) | (man << 13);
  }
  return r.f;
}

/* One component of a vertex attribute as a float. */
static float read_component(const VtxFormat *f, const uint8_t *p, uint32_t c) {
  switch (f->kind) {
    case 0: { float v; memcpy(&v, p + 4 * c, 4); return v; }
    case 1: return p[c] / 255.0f;
    case 2: { float v = (signed char)p[c] / 127.0f; return v < -1 ? -1 : v; }
    case 3: return (float)p[c];
    case 4: return (float)(signed char)p[c];
    case 5: { uint16_t h; memcpy(&h, p + 2 * c, 2); return half_to_float(h); }
    case 6: { uint16_t h; memcpy(&h, p + 2 * c, 2); return h / 65535.0f; }
    case 7: { int16_t h; memcpy(&h, p + 2 * c, 2); float v = h / 32767.0f; return v < -1 ? -1 : v; }
    case 8: { uint16_t h; memcpy(&h, p + 2 * c, 2); return (float)h; }
    case 9: { int16_t h; memcpy(&h, p + 2 * c, 2); return (float)h; }
    case 10: { uint32_t v; memcpy(&v, p + 4 * c, 4); return (float)v; }
    default: { int v; memcpy(&v, p + 4 * c, 4); return (float)v; }
  }
}

/* Why a call failed: the platform writes it where a developer looks (vk_os.h: vk_os_trace). */
static void trace_text(const char *text) { vk_os_trace(text); }
static void trace_status(const char *what, uint32_t status) {
  char text[96];
  uint32_t n = 0;
  const char *p = VK_OS_NAME ": ";
  while (*p) text[n++] = *p++;
  for (; *what && n < 70; ++what) text[n++] = *what;
  text[n++] = ' ';
  text[n++] = '0';
  text[n++] = 'x';
  for (int i = 28; i >= 0; i -= 4) text[n++] = "0123456789abcdef"[(status >> i) & 15];
  text[n++] = '\n';
  text[n] = 0;
  trace_text(text);
}
static void trace(const char *what) {
  trace_text(VK_OS_NAME ": ");
  trace_text(what);
  trace_text("\n");
}
