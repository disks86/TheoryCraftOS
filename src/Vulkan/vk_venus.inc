/* The Vulkan driver's Venus backend (docs/Venus.md): every Vulkan call goes to the host's own Vulkan driver through QEMU's virtio-gpu (virtio-gpu-gl with venus=on), in
 * the Venus protocol that virglrenderer's vkr decodes. The process opens the render node (vk_os_virtgpu_*: /dev/dri/renderD128 on Linux, NtTcosVirtGpu on Windows),
 * makes a context of the Venus type, and talks to the host through a ring in shared memory; host-visible memory is the host's own, mapped into the process through the
 * device's host memory window. The encoders and decoders are generated (venus-protocol/, by venus-protocol v1.1.3); the commands that need nothing but forwarding are
 * generated too (vn_commands.inc, scripts/gen-vulkan-venus.py); this file has the transport, the objects and the commands that need more: instances and physical
 * devices (what is offered), devices and queues, waits (polled: the host never blocks), memory and its mappings, command buffers (recorded into a stream of their own
 * and sent when they end), descriptor update templates, private data, and presentation (the host's image is copied into memory the guest sees and shown through the
 * Direct3D backend's swapchain on the same surface, so every surface kind of the platform works).
 *
 * One backend per process (vn_selected): Venus when the system has a render node whose host offers the Venus context type, unless TCOS_VULKAN_BACKEND=dxg asks for
 * the Direct3D backend (src/Vulkan's other files, over the kernel's Direct3D layer). */

#if !VK_OS_VIRTGPU
/* (a platform without the render node: the Venus backend is never selected; its functions exist so that the dispatch layer links) */
static int64_t vk_os_virtgpu_open(void) { return -1; }
static int vk_os_virtgpu_ioctl(int64_t node, uint32_t request, void *argument) { (void)node, (void)request, (void)argument; return 19; }
static void *vk_os_virtgpu_map(int64_t node, uint64_t offset, uint64_t size) { (void)node, (void)offset, (void)size; return NULL; }
static void vk_os_virtgpu_unmap(int64_t node, void *address, uint64_t size) { (void)node, (void)address, (void)size; }
static void vk_os_virtgpu_close(int64_t node) { (void)node; }
#endif

/* ---- what the generated protocol uses ------------------------------------------------------------------------------------------------------------------------------ */
static int vn_strcmp(const char *a, const char *b) {
  while (*a && *a == *b) ++a, ++b;
  return (unsigned char)*a - (unsigned char)*b;
}
static void *vn_bsearch(const void *key, const void *base, size_t count, size_t size, int (*compare)(const void *, const void *)) {
  size_t lo = 0, hi = count;
  while (lo < hi) {
    const size_t mid = lo + (hi - lo) / 2;
    const void *at = (const uint8_t *)base + mid * size;
    const int c = compare(key, at);
    if (c == 0) return (void *)at;
    if (c < 0) hi = mid;
    else lo = mid + 1;
  }
  return NULL;
}
#define assert(x) ((void)0)
#define bsearch(k, b, n, s, f) vn_bsearch((k), (b), (n), (s), (f))
#define malloc(n) vk_alloc(n)
#define free(p) vk_free(p)
#define strlen(s) cstrlen(s)
#define strcmp(a, b) vn_strcmp((a), (b))
#include "venus-protocol/vn_cs.h"
#include "venus-protocol/vn_ring.h"
/* (the generated sizeof functions take the address of a result they only measure: the warnings about it are the generator's, not mistakes) */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpragmas"
#pragma GCC diagnostic ignored "-Wunknown-warning-option"
#pragma GCC diagnostic ignored "-Wuninitialized-const-pointer"
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
#pragma GCC diagnostic ignored "-Wuninitialized"
#include "venus-protocol/vn_protocol_driver.h"
#pragma GCC diagnostic pop
#undef bsearch
#undef malloc
#undef free
#undef strlen
#undef strcmp

/* virglrenderer's capability set of the Venus context type (venus_hw.h, capset 4, version 0) */
typedef struct VnCapset {
  uint32_t wire_format_version, vk_xml_version, vk_ext_command_serialization_spec_version, vk_mesa_venus_protocol_spec_version, supports_blob_id_0;
  uint32_t vk_extension_mask1[32];
  uint32_t allow_vk_wait_syncs, supports_multiple_timelines, use_guest_vram;
} VnCapset;

static uint32_t g_vn_renderer_api_version;     /* what the host's protocol carries (capped by ours) */
static uint32_t g_vn_extension_mask[32];       /* bit n: extension number n is carried by the host's protocol */
static bool g_vn_extension_mask_valid;

static bool vn_cs_renderer_protocol_has_api_version(uint32_t api_version) { return g_vn_renderer_api_version >= api_version; }
static bool vn_cs_renderer_protocol_has_extension(uint32_t number) {
  if (!g_vn_extension_mask_valid) return true;   /* (an older host says nothing: everything the protocol has) */
  return number < 1024 && (g_vn_extension_mask[number / 32] >> (number % 32)) & 1;
}

static bool vn_cs_encoder_grow(struct vn_cs_encoder *enc, size_t size) {
  const size_t used = (size_t)(enc->cur - enc->base), have = (size_t)(enc->end - enc->base);
  if (size > SIZE_MAX / 2 - used) return false;
  size_t want = have ? have : 4096;
  while (want - used < size) want *= 2;
  uint8_t *grown = vk_alloc(want);
  if (!grown) return false;
  if (used) memcpy(grown, enc->base, used);
  vk_free(enc->base);
  enc->base = grown, enc->cur = grown + used, enc->end = grown + want;
  return true;
}

/* ---- locks and waiting ---------------------------------------------------------------------------------------------------------------------------------------- */
typedef struct VnLock {
  int taken;
} VnLock;
static void vn_lock(VnLock *l) {
  for (uint32_t spins = 0; __atomic_exchange_n(&l->taken, 1, __ATOMIC_ACQUIRE); ++spins)
    if (spins > 64) vk_os_sleep_ns(20000);
}
static void vn_unlock(VnLock *l) { __atomic_store_n(&l->taken, 0, __ATOMIC_RELEASE); }

/* Waiting for the host: a few quick looks, then sleeps that grow to a millisecond (the host's ring thread runs on its own CPU; a reply usually takes tens of
 * microseconds). */
typedef struct VnRelax {
  uint32_t step;
} VnRelax;
static void vn_relax(VnRelax *r) {
  if (r->step < 16) {
    ++r->step;
    return;
  }
  uint64_t ns = 10000ull << (r->step - 16 < 7 ? r->step - 16 : 7);
  if (ns > 1000000) ns = 1000000;
  ++r->step;
  vk_os_sleep_ns(ns);
}

/* ---- the render node ----------------------------------------------------------------------------------------------------------------------------------------- */
typedef struct VnShmem {
  uint32_t bo, res;
  uint64_t size;
  uint8_t *map;
} VnShmem;

typedef struct VnRenderer {
  int64_t node;
  VnCapset capset;
} VnRenderer;

static int vn_ioctl(VnRenderer *r, uint32_t request, void *argument) { return vk_os_virtgpu_ioctl(r->node, request, argument); }

static uint64_t vn_getparam(VnRenderer *r, uint64_t param) {
  int32_t value = 0;
  TcosVirtgpuGetparam g = {param, (uint64_t)(uintptr_t)&value};
  return vn_ioctl(r, TCOS_DRM_IOCTL_VIRTGPU_GETPARAM, &g) ? 0 : (uint64_t)(uint32_t)value;
}

static void vn_renderer_close(VnRenderer *r) {
  if (r->node >= 0) vk_os_virtgpu_close(r->node);
  r->node = -1;
}

/* Opens the render node and makes a Venus context on it. False (and nothing open) when the system has none, or its host does not offer Venus. */
static bool vn_renderer_open(VnRenderer *r) {
  memset(r, 0, sizeof *r);
  r->node = vk_os_virtgpu_open();
  if (r->node < 0) return false;
  static const uint64_t required[] = {TCOS_VIRTGPU_PARAM_3D_FEATURES, TCOS_VIRTGPU_PARAM_CAPSET_QUERY_FIX, TCOS_VIRTGPU_PARAM_RESOURCE_BLOB,
                                      TCOS_VIRTGPU_PARAM_CONTEXT_INIT, TCOS_VIRTGPU_PARAM_HOST_VISIBLE};
  for (uint32_t i = 0; i < sizeof required / sizeof required[0]; ++i)
    if (!vn_getparam(r, required[i])) goto fail;
  if (!((vn_getparam(r, TCOS_VIRTGPU_PARAM_SUPPORTED_CAPSET_IDS) >> TCOS_VIRTGPU_CAPSET_VENUS) & 1)) goto fail;
  TcosVirtgpuGetCaps caps = {TCOS_VIRTGPU_CAPSET_VENUS, 0, (uint64_t)(uintptr_t)&r->capset, sizeof r->capset, 0};
  if (vn_ioctl(r, TCOS_DRM_IOCTL_VIRTGPU_GET_CAPS, &caps)) goto fail;
  if (r->capset.wire_format_version != vn_info_wire_format_version() || !r->capset.supports_blob_id_0) {
    vk_os_trace("venus: the host's protocol does not match (wire format or shared memory)\n");
    goto fail;
  }
  TcosVirtgpuContextSetParam params[3] = {{TCOS_VIRTGPU_CONTEXT_PARAM_CAPSET_ID, TCOS_VIRTGPU_CAPSET_VENUS}, {TCOS_VIRTGPU_CONTEXT_PARAM_NUM_RINGS, 64},
                                          {TCOS_VIRTGPU_CONTEXT_PARAM_POLL_RINGS_MASK, 0}};
  TcosVirtgpuContextInit init = {3, 0, (uint64_t)(uintptr_t)params};
  if (vn_ioctl(r, TCOS_DRM_IOCTL_VIRTGPU_CONTEXT_INIT, &init)) goto fail;
  /* (what the host's protocol carries, capped by what ours knows) */
  uint32_t version = r->capset.vk_xml_version;
  if (version > vn_info_vk_xml_version()) version = vn_info_vk_xml_version();
  g_vn_renderer_api_version = version;
  if (r->capset.vk_extension_mask1[0] & 1) {
    memcpy(g_vn_extension_mask, r->capset.vk_extension_mask1, sizeof g_vn_extension_mask);
    g_vn_extension_mask_valid = true;
  }
  return true;
fail:
  vn_renderer_close(r);
  return false;
}

/* Shared memory the host made (a blob of the context with blob id 0), mapped into the process. */
static bool vn_shmem_create(VnRenderer *r, uint64_t size, VnShmem *out) {
  memset(out, 0, sizeof *out);
  size = (size + 4095) & ~(uint64_t)4095;
  TcosVirtgpuResourceCreateBlob blob;
  memset(&blob, 0, sizeof blob);
  blob.blob_mem = TCOS_VIRTGPU_BLOB_MEM_HOST3D, blob.blob_flags = TCOS_VIRTGPU_BLOB_FLAG_USE_MAPPABLE, blob.size = size;
  if (vn_ioctl(r, TCOS_DRM_IOCTL_VIRTGPU_RESOURCE_CREATE_BLOB, &blob)) return false;
  TcosVirtgpuMap map = {0, blob.bo_handle, 0};
  void *address = NULL;
  if (!vn_ioctl(r, TCOS_DRM_IOCTL_VIRTGPU_MAP, &map)) address = vk_os_virtgpu_map(r->node, map.offset, size);
  if (!address) {
    TcosDrmGemClose close = {blob.bo_handle, 0};
    vn_ioctl(r, TCOS_DRM_IOCTL_GEM_CLOSE, &close);
    return false;
  }
  out->bo = blob.bo_handle, out->res = blob.res_handle, out->size = size, out->map = address;
  return true;
}

static void vn_shmem_destroy(VnRenderer *r, VnShmem *s) {
  if (s->map) vk_os_virtgpu_unmap(r->node, s->map, s->size);
  if (s->bo) {
    TcosDrmGemClose close = {s->bo, 0};
    vn_ioctl(r, TCOS_DRM_IOCTL_GEM_CLOSE, &close);
  }
  memset(s, 0, sizeof *s);
}

/* Commands sent straight to the context, not through a ring (creating and waking rings, roundtrips). */
static bool vn_renderer_submit(VnRenderer *r, const void *commands, size_t size) {
  TcosVirtgpuExecbuffer e;
  memset(&e, 0, sizeof e);
  e.size = (uint32_t)size, e.command = (uint64_t)(uintptr_t)commands;
  return vn_ioctl(r, TCOS_DRM_IOCTL_VIRTGPU_EXECBUFFER, &e) == 0;
}

/* ---- the ring -------------------------------------------------------------------------------------------------------------------------------------------------- */
#define VN_RING_BUFFER_BYTES (128u * 1024u)
#define VN_RING_DIRECT_BYTES (VN_RING_BUFFER_BYTES / 16)   /* (bigger commands go in shared memory of their own: vkExecuteCommandStreamsMESA) */
#define VN_RING_IDLE_TIMEOUT_NS 1000000ull
#define VN_REPLY_MIN_BYTES (1u << 20)

struct vn_ring {
  VnRenderer *renderer;
  uint64_t id;
  VnShmem shmem;
  volatile uint32_t *head, *tail, *status;
  uint8_t *buffer;
  uint32_t cur;      /* the bytes written so far (wraps): a command's seqno is cur after it */
  VnLock lock;
  VnShmem reply;     /* where replies go (grown when a reply does not fit) */
  bool fatal;
};

static uint32_t vn_ring_load_head(struct vn_ring *ring) { return __atomic_load_n(ring->head, __ATOMIC_ACQUIRE); }

/* Has the host consumed up to `seqno`? (Mesa's vn_ring_ge_seqno: seqnos are byte counts that wrap.) */
static bool vn_ring_ge_seqno(const struct vn_ring *ring, uint32_t a, uint32_t b) {
  if (a >= b) return ring->cur >= a || ring->cur < b;
  return ring->cur >= a && ring->cur < b;
}

static void vn_ring_wait_seqno(struct vn_ring *ring, uint32_t seqno) {
  VnRelax relax = {0};
  while (!vn_ring_ge_seqno(ring, vn_ring_load_head(ring), seqno)) {
    if (__atomic_load_n(ring->status, __ATOMIC_ACQUIRE) & VK_RING_STATUS_FATAL_BIT_MESA) {
      ring->fatal = true;
      return;
    }
    vn_relax(&relax);
  }
}

static void vn_ring_write(struct vn_ring *ring, const void *data, uint32_t size) {
  const uint32_t offset = ring->cur & (VN_RING_BUFFER_BYTES - 1);
  if (offset + size <= VN_RING_BUFFER_BYTES) {
    memcpy(ring->buffer + offset, data, size);
  } else {
    const uint32_t first = VN_RING_BUFFER_BYTES - offset;
    memcpy(ring->buffer + offset, data, first);
    memcpy(ring->buffer, (const uint8_t *)data + first, size - first);
  }
  ring->cur += size;
}

/* Writes one command stream (lock held). Its seqno: once the host's head reaches it, the host has consumed it. */
static bool vn_ring_submit_locked(struct vn_ring *ring, const void *data, size_t size, uint32_t *seqno);

static bool vn_ring_submit_direct_locked(struct vn_ring *ring, const void *data, uint32_t size, uint32_t *seqno) {
  VnRelax relax = {0};
  while (ring->cur + size - vn_ring_load_head(ring) > VN_RING_BUFFER_BYTES) {   /* (room: wait for the host to consume) */
    if (__atomic_load_n(ring->status, __ATOMIC_ACQUIRE) & VK_RING_STATUS_FATAL_BIT_MESA) {
      ring->fatal = true;
      return false;
    }
    vn_relax(&relax);
  }
  vn_ring_write(ring, data, size);
  __atomic_store_n(ring->tail, ring->cur, __ATOMIC_SEQ_CST);
  const uint32_t status = __atomic_load_n(ring->status, __ATOMIC_SEQ_CST);
  if (status & VK_RING_STATUS_FATAL_BIT_MESA) {
    ring->fatal = true;
    vk_os_trace("venus: the host stopped the ring (a command it could not decode)\n");
    return false;
  }
  if (status & VK_RING_STATUS_IDLE_BIT_MESA) {   /* (the host's ring thread went to sleep: wake it) */
    uint32_t notify[8];
    struct vn_cs_encoder enc = {(uint8_t *)notify, (uint8_t *)notify, (uint8_t *)notify + sizeof notify, false, false};
    vn_encode_vkNotifyRingMESA(&enc, 0, ring->id, ring->cur, 0);
    vn_renderer_submit(ring->renderer, notify, vn_cs_encoder_get_len(&enc));
  }
  if (seqno) *seqno = ring->cur;
  return true;
}

static bool vn_ring_submit_locked(struct vn_ring *ring, const void *data, size_t size, uint32_t *seqno) {
  if (ring->fatal || !size) return false;
  if (size <= VN_RING_DIRECT_BYTES) return vn_ring_submit_direct_locked(ring, data, (uint32_t)size, seqno);
  /* A big command (a large command buffer, a shader, an update): in shared memory of its own, which the ring points the host at. The memory goes once the host
   * has consumed the pointer (it executes the stream right then). */
  VnShmem stream;
  if (!vn_shmem_create(ring->renderer, size, &stream)) return false;
  memcpy(stream.map, data, size);
  const VkCommandStreamDescriptionMESA desc = {stream.res, 0, size};
  uint32_t exec[32];
  struct vn_cs_encoder enc = {(uint8_t *)exec, (uint8_t *)exec, (uint8_t *)exec + sizeof exec, false, false};
  vn_encode_vkExecuteCommandStreamsMESA(&enc, 0, 1, &desc, NULL, 0, NULL, 0);
  uint32_t done = 0;
  const bool ok = !enc.fatal && vn_ring_submit_direct_locked(ring, exec, (uint32_t)vn_cs_encoder_get_len(&enc), &done);
  if (ok) vn_ring_wait_seqno(ring, done);
  vn_shmem_destroy(ring->renderer, &stream);
  if (ok && seqno) *seqno = done;
  return ok;
}

static bool vn_ring_create(struct vn_ring *ring, VnRenderer *renderer, uint64_t id) {
  memset(ring, 0, sizeof *ring);
  ring->renderer = renderer, ring->id = id;
  /* the layout: head, tail and status a cache line each, then the buffer, then 4 bytes of extra (Mesa's) */
  const size_t head = 0, tail = 64, status = 128, buffer = 192, extra = buffer + VN_RING_BUFFER_BYTES, total = extra + 4;
  if (!vn_shmem_create(renderer, total, &ring->shmem)) return false;
  memset(ring->shmem.map, 0, total);
  ring->head = (volatile uint32_t *)(ring->shmem.map + head);
  ring->tail = (volatile uint32_t *)(ring->shmem.map + tail);
  ring->status = (volatile uint32_t *)(ring->shmem.map + status);
  ring->buffer = ring->shmem.map + buffer;
  const VkRingCreateInfoMESA info = {VK_STRUCTURE_TYPE_RING_CREATE_INFO_MESA, NULL, 0, ring->shmem.res, 0, total, VN_RING_IDLE_TIMEOUT_NS, head, tail, status,
                                     buffer, VN_RING_BUFFER_BYTES, extra, 4};
  uint32_t data[64];
  struct vn_cs_encoder enc = {(uint8_t *)data, (uint8_t *)data, (uint8_t *)data + sizeof data, false, false};
  vn_encode_vkCreateRingMESA(&enc, 0, ring->id, &info);
  if (enc.fatal || !vn_renderer_submit(renderer, data, vn_cs_encoder_get_len(&enc))) {
    vn_shmem_destroy(renderer, &ring->shmem);
    return false;
  }
  return true;
}

static void vn_ring_destroy(struct vn_ring *ring) {
  if (!ring->shmem.map) return;
  uint32_t data[8];
  struct vn_cs_encoder enc = {(uint8_t *)data, (uint8_t *)data, (uint8_t *)data + sizeof data, false, false};
  vn_encode_vkDestroyRingMESA(&enc, 0, ring->id);
  vn_renderer_submit(ring->renderer, data, vn_cs_encoder_get_len(&enc));
  /* (the host has destroyed the ring when it answers: execbuffer is fenced; after that the memory may go) */
  vn_shmem_destroy(ring->renderer, &ring->reply);
  vn_shmem_destroy(ring->renderer, &ring->shmem);
}

/* Waits until the host has consumed everything written so far. */
static void vn_ring_roundtrip(struct vn_ring *ring) {
  vn_lock(&ring->lock);
  const uint32_t seqno = ring->cur;
  vn_unlock(&ring->lock);
  vn_ring_wait_seqno(ring, seqno);
}

/* ---- the generated functions' interface (vn_ring.h) ---- */
static struct vn_cs_encoder *vn_ring_submit_command_init(struct vn_ring *ring, struct vn_ring_submit_command *submit, void *cmd_data, size_t cmd_size, size_t reply_size) {
  (void)ring;
  submit->command.base = submit->command.cur = (uint8_t *)cmd_data;
  submit->command.end = (uint8_t *)cmd_data + cmd_size;
  submit->command.growable = false, submit->command.fatal = false;
  submit->reply_size = reply_size;
  submit->has_reply = false;
  return &submit->command;
}

static void vn_ring_submit_command(struct vn_ring *ring, struct vn_ring_submit_command *submit) {
  if (submit->command.fatal || vn_cs_encoder_get_len(&submit->command) == 0) return;
  vn_lock(&ring->lock);
  if (submit->reply_size) {
    if (ring->reply.size < submit->reply_size) {   /* (the old stream is not in use: every reply was waited for under the lock) */
      vn_shmem_destroy(ring->renderer, &ring->reply);
      if (!vn_shmem_create(ring->renderer, submit->reply_size > VN_REPLY_MIN_BYTES ? submit->reply_size : VN_REPLY_MIN_BYTES, &ring->reply)) {
        vn_unlock(&ring->lock);
        return;
      }
    }
    const VkCommandStreamDescriptionMESA stream = {ring->reply.res, 0, submit->reply_size};
    uint32_t data[16];
    struct vn_cs_encoder enc = {(uint8_t *)data, (uint8_t *)data, (uint8_t *)data + sizeof data, false, false};
    vn_encode_vkSetReplyCommandStreamMESA(&enc, 0, &stream);
    if (enc.fatal || !vn_ring_submit_locked(ring, data, vn_cs_encoder_get_len(&enc), NULL)) {
      vn_unlock(&ring->lock);
      return;
    }
  }
  uint32_t seqno = 0;
  const bool sent = vn_ring_submit_locked(ring, submit->command.base, vn_cs_encoder_get_len(&submit->command), &seqno);
  if (!submit->reply_size) {
    vn_unlock(&ring->lock);
    return;
  }
  if (sent) vn_ring_wait_seqno(ring, seqno);
  if (!sent || ring->fatal) {
    vn_unlock(&ring->lock);
    return;
  }
  /* (the lock stays: the reply stream is read before anything else may reply into it) */
  submit->reply.cur = ring->reply.map, submit->reply.end = ring->reply.map + submit->reply_size, submit->reply.fatal = false;
  submit->has_reply = true;
}

static struct vn_cs_decoder *vn_ring_get_command_reply(struct vn_ring *ring, struct vn_ring_submit_command *submit) {
  (void)ring;
  return submit->has_reply ? &submit->reply : NULL;
}

static void vn_ring_free_command_reply(struct vn_ring *ring, struct vn_ring_submit_command *submit) {
  if (submit->has_reply) vn_unlock(&ring->lock);
  submit->has_reply = false;
}

/* ---- objects -------------------------------------------------------------------------------------------------------------------------------------------------- */
/* The dispatchable objects (instance, physical device, device, queue, command buffer) start with VnHdr: the loader's slot, then the id the host knows the object by.
 * The non-dispatchable handles are the ids themselves, except the three kinds the guest keeps state for (device memory, command pools, descriptor update
 * templates), which point at a struct that starts with VnObject. Ids are never reused. */
typedef struct VnHdr {
  void *dispatch;   /* ICD_LOADER_MAGIC (the Khronos loader's slot) */
  uint64_t id;
} VnHdr;
typedef struct VnObject {
  uint64_t id;
} VnObject;

static uint64_t g_vn_next_id = 1;
static uint64_t vn_new_id(void) { return __atomic_fetch_add(&g_vn_next_id, 1, __ATOMIC_RELAXED); }

static bool vn_type_dispatchable(VkObjectType type) {
  return type == VK_OBJECT_TYPE_INSTANCE || type == VK_OBJECT_TYPE_PHYSICAL_DEVICE || type == VK_OBJECT_TYPE_DEVICE || type == VK_OBJECT_TYPE_QUEUE ||
         type == VK_OBJECT_TYPE_COMMAND_BUFFER;
}
static bool vn_type_wrapped(VkObjectType type) {
  return type == VK_OBJECT_TYPE_DEVICE_MEMORY || type == VK_OBJECT_TYPE_COMMAND_POOL || type == VK_OBJECT_TYPE_DESCRIPTOR_UPDATE_TEMPLATE;
}

/* `handle` points at a handle variable (a pointer for the dispatchable kinds, 64 bits for the others on every CPU). */
static uint64_t vn_load_id(const void *handle, VkObjectType type) {
  if (vn_type_dispatchable(type)) {
    const VnHdr *h;
    memcpy(&h, handle, sizeof h);
    return h ? h->id : 0;
  }
  uint64_t v;
  memcpy(&v, handle, sizeof v);
  if (v && vn_type_wrapped(type)) return ((const VnObject *)(uintptr_t)v)->id;
  return v;
}

static void vn_store_id(void *handle, uint64_t id, VkObjectType type) {
  if (vn_type_dispatchable(type)) {   /* (made by the guest before it was sent: the host answers with the same id) */
    VnHdr *h;
    memcpy(&h, handle, sizeof h);
    if (h && id) h->id = id;
    return;
  }
  if (vn_type_wrapped(type)) return;   /* (the same: the guest's struct already has it) */
  memcpy(handle, &id, sizeof id);
}

/* Ids for objects a command is about to create (the generated create functions). */
static void vn_new_handles(uint64_t *handles, uint32_t count) {
  for (uint32_t i = 0; i < count; ++i) handles[i] = vn_new_id();
}
static void vn_clear_handles(uint64_t *handles, uint32_t count) {
  for (uint32_t i = 0; i < count; ++i) handles[i] = 0;
}

typedef struct VnPhysical VnPhysical;
typedef struct VnDevice VnDevice;
typedef struct VnInstance VnInstance;

typedef struct VnHidden {   /* the Direct3D backend's objects presentation runs on (made when a surface is first asked about) */
  VkInstance instance;
  VkPhysicalDevice physical;
  bool tried;
} VnHidden;

struct VnInstance {
  VnHdr hdr;
  VnRenderer renderer;
  struct vn_ring ring;
  uint32_t api_version;          /* what the instance was made for (the application's, at least 1.1 internally) */
  VnPhysical *physicals;
  uint32_t physical_count;
  bool enumerated;
  VnHidden present;
  uint64_t ring_indices;      /* the host's timelines in use (VkDeviceQueueTimelineInfoMESA): bit n for ring n, 1 .. 63, one per queue of the context */
  VnLock ring_index_lock;
};

struct VnPhysical {
  VnHdr hdr;
  VnInstance *instance;
  VkPhysicalDeviceProperties properties;   /* the host's, with this driver's version and name */
  uint32_t api_version;
  VkPhysicalDeviceMemoryProperties memory;
  VkExtensionProperties *extensions;       /* what is offered */
  uint32_t extension_count;
  VkPhysicalDeviceDriverProperties host_driver;
};

typedef struct VnQueue {
  VnHdr hdr;
  VnDevice *device;
  uint32_t family, index;
  VkDeviceQueueCreateFlags flags;
  VkFence idle_fence;   /* for vkQueueWaitIdle (made on first use) */
  uint32_t ring_index;  /* its timeline on the host */
} VnQueue;

typedef struct VnPrivate {   /* a private data value (vkSetPrivateData) */
  VkObjectType type;
  uint64_t object;
  VkPrivateDataSlot slot;
  uint64_t value;
} VnPrivate;

typedef struct VnPresentDevice {   /* the Direct3D backend's device presentation runs on */
  VkDevice device;
  VkQueue queue;
  VkCommandPool pool;
  bool tried;
} VnPresentDevice;

struct VnDevice {
  VnHdr hdr;
  VnPhysical *physical;
  VnInstance *instance;
  VnQueue *queues;
  uint32_t queue_count;
  char (*extensions)[VK_MAX_EXTENSION_NAME_SIZE];   /* the ones enabled */
  uint32_t extension_count;
  uint32_t api_version;
  VnPrivate *privates;
  uint32_t private_count, private_capacity;
  VnLock private_lock;
  VnPresentDevice present;
};

typedef struct VnCommandBuffer VnCommandBuffer;
typedef struct VnCommandPool {
  VnObject obj;
  VnDevice *device;
  VnCommandBuffer *buffers;   /* the pool's command buffers (a list) */
} VnCommandPool;

struct VnCommandBuffer {
  VnHdr hdr;
  VnDevice *device;
  VnCommandPool *pool;
  struct vn_cs_encoder cs;   /* the commands recorded since vkBeginCommandBuffer */
  bool recording;
  VnCommandBuffer *next;
};

typedef struct VnMemory {
  VnObject obj;
  VnDevice *device;
  VkDeviceSize size;        /* as the host allocated it (page rounded when it can be mapped) */
  uint32_t type;
  VkMemoryPropertyFlags flags;
  uint32_t bo, res;         /* its blob, once mapped */
  uint8_t *map;
} VnMemory;

typedef struct VnTemplate {
  VnObject obj;
  VkDescriptorUpdateTemplateType type;
  VkDescriptorUpdateTemplateEntry *entries;
  uint32_t entry_count;
  VkPipelineBindPoint bind_point;
  VkPipelineLayout layout;
  uint32_t set;
} VnTemplate;

#define VN_HANDLE(type, value) ((type)(uintptr_t)(value))
static VnInstance *vn_instance(VkInstance h) { return (VnInstance *)h; }
static VnPhysical *vn_physical(VkPhysicalDevice h) { return (VnPhysical *)h; }
static VnDevice *vn_device(VkDevice h) { return (VnDevice *)h; }
static VnQueue *vn_queue(VkQueue h) { return (VnQueue *)h; }
static VnCommandBuffer *vn_command_buffer(VkCommandBuffer h) { return (VnCommandBuffer *)h; }
static VnMemory *vn_memory(VkDeviceMemory h) { return (VnMemory *)(uintptr_t)h; }
static VnCommandPool *vn_pool(VkCommandPool h) { return (VnCommandPool *)(uintptr_t)h; }
static VnTemplate *vn_template(VkDescriptorUpdateTemplate h) { return (VnTemplate *)(uintptr_t)h; }

static struct vn_ring *vn_ring_of_instance(VkInstance h) { return &vn_instance(h)->ring; }
static struct vn_ring *vn_ring_of_physical(VkPhysicalDevice h) { return &vn_physical(h)->instance->ring; }
static struct vn_ring *vn_ring_of_device(VkDevice h) { return &vn_device(h)->instance->ring; }
static struct vn_ring *vn_ring_of_queue(VkQueue h) { return &vn_queue(h)->device->instance->ring; }
static struct vn_ring *vn_ring_of_command_buffer(VkCommandBuffer h) { return &vn_command_buffer(h)->device->instance->ring; }

/* A recorded command's room in its command buffer's stream (the generated vkCmd* functions). A command buffer that ran out of memory stays fatal until it is reset:
 * vkEndCommandBuffer reports it. */
static bool vn_command_reserve(VnCommandBuffer *cmd, size_t bytes) { return cmd->recording && vn_cs_encoder_reserve(&cmd->cs, bytes); }

/* ---- which backend ------------------------------------------------------------------------------------------------------------------------------------------ */
/* Decided once per process: Venus when TCOS_VULKAN_BACKEND is not "dxg" and the render node opens with the Venus context type (the node is closed again: each
 * instance has a context of its own). */
static int g_vn_selected;   /* 0: not decided, 1: Direct3D, 2: Venus */
static bool vn_selected(void) {
  int s = __atomic_load_n(&g_vn_selected, __ATOMIC_ACQUIRE);
  if (s) return s == 2;
  char want[16] = {0};
  s = 1;
  if (!(vk_os_getenv("TCOS_VULKAN_BACKEND", want, sizeof want) && (streq(want, "dxg") || streq(want, "d3d")))) {
    VnRenderer probe;
    if (vn_renderer_open(&probe)) {
      s = 2;
      vn_renderer_close(&probe);
    }
  }
  int expected = 0;
  __atomic_compare_exchange_n(&g_vn_selected, &expected, s, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
  return __atomic_load_n(&g_vn_selected, __ATOMIC_ACQUIRE) == 2;
}

/* A function of the other backend was called (a program that ignores what the instance and device offer): said in the trace; it fails as an unoffered function does. */
static void vn_not_offered(const char *name) {
  char line[160];
  copy_string(line, sizeof line - 40, name);
  size_t n = cstrlen(line);
  copy_string(line + n, sizeof line - n, vn_selected() ? ": not offered by the Venus backend\n" : ": not offered by the Direct3D backend\n");
  vk_os_trace(line);
}

/* ---- instances --------------------------------------------------------------------------------------------------------------------------------------------- */
#define VN_API_VERSION VK_MAKE_API_VERSION(0, 1, 3, VK_HEADER_VERSION)   /* the most this backend offers (docs/Venus.md: 1.4's host image copy needs host access) */

static const VkExtensionProperties kVnInstanceExtensions[] = {
    {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_SURFACE_SPEC_VERSION},
    {VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME, VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_SPEC_VERSION},
    {VK_KHR_EXTERNAL_MEMORY_CAPABILITIES_EXTENSION_NAME, VK_KHR_EXTERNAL_MEMORY_CAPABILITIES_SPEC_VERSION},
    {VK_KHR_EXTERNAL_SEMAPHORE_CAPABILITIES_EXTENSION_NAME, VK_KHR_EXTERNAL_SEMAPHORE_CAPABILITIES_SPEC_VERSION},
    {VK_KHR_EXTERNAL_FENCE_CAPABILITIES_EXTENSION_NAME, VK_KHR_EXTERNAL_FENCE_CAPABILITIES_SPEC_VERSION},
    VK_OS_INSTANCE_EXTENSIONS
};
#define VN_INSTANCE_EXTENSION_COUNT (sizeof kVnInstanceExtensions / sizeof kVnInstanceExtensions[0])

static VkResult vn_fill_extensions(const VkExtensionProperties *have, uint32_t n, uint32_t *count, VkExtensionProperties *out) {
  if (!out) {
    *count = n;
    return VK_SUCCESS;
  }
  const uint32_t k = *count < n ? *count : n;
  for (uint32_t i = 0; i < k; ++i) out[i] = have[i];
  *count = k;
  return k < n ? VK_INCOMPLETE : VK_SUCCESS;
}

static VkResult VKAPI_CALL vn_vkEnumerateInstanceVersion(uint32_t *version) {
  *version = VN_API_VERSION;
  return VK_SUCCESS;
}

static VkResult VKAPI_CALL vn_vkEnumerateInstanceExtensionProperties(const char *layer, uint32_t *count, VkExtensionProperties *out) {
  if (layer) return VK_ERROR_LAYER_NOT_PRESENT;
  return vn_fill_extensions(kVnInstanceExtensions, VN_INSTANCE_EXTENSION_COUNT, count, out);
}

static VkResult VKAPI_CALL vn_vkEnumerateInstanceLayerProperties(uint32_t *count, VkLayerProperties *out) {
  (void)out;
  *count = 0;
  return VK_SUCCESS;
}

static bool vn_offered(const VkExtensionProperties *have, uint32_t n, const char *name) {
  for (uint32_t i = 0; i < n; ++i)
    if (streq(have[i].extensionName, name)) return true;
  return false;
}

static VkResult VKAPI_CALL vn_vkCreateInstance(const VkInstanceCreateInfo *ci, const VkAllocationCallbacks *alloc, VkInstance *out) {
  (void)alloc;
  for (uint32_t i = 0; i < ci->enabledExtensionCount; ++i)
    if (!vn_offered(kVnInstanceExtensions, VN_INSTANCE_EXTENSION_COUNT, ci->ppEnabledExtensionNames[i])) return VK_ERROR_EXTENSION_NOT_PRESENT;
  if (ci->enabledLayerCount) return VK_ERROR_LAYER_NOT_PRESENT;
  const uint32_t app_version = ci->pApplicationInfo && ci->pApplicationInfo->apiVersion ? ci->pApplicationInfo->apiVersion : VK_API_VERSION_1_0;
  VnInstance *inst = vk_alloc(sizeof *inst);
  if (!inst) return VK_ERROR_OUT_OF_HOST_MEMORY;
  inst->hdr.dispatch = ICD_LOADER_MAGIC, inst->hdr.id = vn_new_id();
  inst->api_version = app_version;
  if (!vn_renderer_open(&inst->renderer)) {
    vk_free(inst);
    return VK_ERROR_INITIALIZATION_FAILED;
  }
  if (!vn_ring_create(&inst->ring, &inst->renderer, vn_new_id())) {
    vn_renderer_close(&inst->renderer);
    vk_free(inst);
    return VK_ERROR_INITIALIZATION_FAILED;
  }
  /* The host's instance: no extensions (the surfaces are this driver's), the API version the program asked for but at least 1.1 (what the protocol needs). */
  uint32_t host_version = 0;
  if (vn_call_vkEnumerateInstanceVersion(&inst->ring, &host_version) != VK_SUCCESS) host_version = VK_API_VERSION_1_0;
  uint32_t want = app_version < VK_API_VERSION_1_1 ? VK_API_VERSION_1_1 : app_version;
  if (want > VN_API_VERSION) want = VK_API_VERSION_1_3;
  VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, NULL, 0, NULL, 0, want};
  if (ci->pApplicationInfo) app = *ci->pApplicationInfo, app.pNext = NULL, app.apiVersion = want;
  VkInstanceCreateInfo host = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &app, 0, NULL, 0, NULL};
  VkInstance handle = (VkInstance)inst;
  const VkResult r = vn_call_vkCreateInstance(&inst->ring, &host, NULL, &handle);
  if (r != VK_SUCCESS) {
    const bool lost = inst->ring.fatal;   /* (no answer at all: the host's protocol is not what this driver speaks) */
    vn_ring_destroy(&inst->ring);
    vn_renderer_close(&inst->renderer);
    vk_free(inst);
    return lost ? VK_ERROR_INITIALIZATION_FAILED : r;
  }
  *out = handle;
  return VK_SUCCESS;
}

static void vn_hidden_destroy(VnInstance *inst);

static void VKAPI_CALL vn_vkDestroyInstance(VkInstance instance, const VkAllocationCallbacks *alloc) {
  (void)alloc;
  if (!instance) return;
  VnInstance *inst = vn_instance(instance);
  vn_hidden_destroy(inst);
  vn_async_vkDestroyInstance(&inst->ring, instance, NULL);
  vn_ring_roundtrip(&inst->ring);   /* (the host is done with the instance before its ring goes) */
  for (uint32_t i = 0; i < inst->physical_count; ++i) vk_free(inst->physicals[i].extensions);
  vk_free(inst->physicals);
  vn_ring_destroy(&inst->ring);
  vn_renderer_close(&inst->renderer);
  vk_free(inst);
}

/* ---- physical devices ---------------------------------------------------------------------------------------------------------------------------------------- */
static bool vn_extension_allowed(const char *name) {
  for (uint32_t i = 0; i < sizeof kVnDeviceExtensions / sizeof kVnDeviceExtensions[0]; ++i)
    if (streq(kVnDeviceExtensions[i].name, name)) return vn_cs_renderer_protocol_has_extension(kVnDeviceExtensions[i].number);
  return false;
}

/* What a physical device offers: the host's extensions this backend can forward, and VK_KHR_swapchain (presentation is this driver's own). */
static bool vn_physical_setup(VnPhysical *pd) {
  VkPhysicalDevice h = (VkPhysicalDevice)pd;
  struct vn_ring *ring = &pd->instance->ring;
  vn_call_vkGetPhysicalDeviceProperties(ring, h, &pd->properties);
  pd->host_driver.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES;
  if (pd->properties.apiVersion >= VK_API_VERSION_1_2) {
    VkPhysicalDeviceProperties2 p2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &pd->host_driver};
    vn_call_vkGetPhysicalDeviceProperties2(ring, h, &p2);
  }
  vn_call_vkGetPhysicalDeviceMemoryProperties(ring, h, &pd->memory);
  uint32_t api = pd->properties.apiVersion;
  if (api > g_vn_renderer_api_version) api = g_vn_renderer_api_version;
  if (VK_API_VERSION_MAJOR(api) > 1 || VK_API_VERSION_MINOR(api) > 3) api = VK_MAKE_API_VERSION(0, 1, 3, VK_API_VERSION_PATCH(api));
  pd->api_version = api;
  uint32_t n = 0;
  if (vn_call_vkEnumerateDeviceExtensionProperties(ring, h, NULL, &n, NULL) != VK_SUCCESS) return false;
  VkExtensionProperties *host = vk_alloc(sizeof *host * (n + 1));
  if (!host && n) return false;
  if (n && vn_call_vkEnumerateDeviceExtensionProperties(ring, h, NULL, &n, host) < 0) {
    vk_free(host);
    return false;
  }
  pd->extensions = vk_alloc(sizeof *pd->extensions * (n + 1));
  if (!pd->extensions) {
    vk_free(host);
    return false;
  }
  uint32_t k = 0;
  for (uint32_t i = 0; i < n; ++i)
    if (vn_extension_allowed(host[i].extensionName)) pd->extensions[k++] = host[i];
#if !VN_NO_PRESENT
  copy_string(pd->extensions[k].extensionName, VK_MAX_EXTENSION_NAME_SIZE, VK_KHR_SWAPCHAIN_EXTENSION_NAME);
  pd->extensions[k++].specVersion = VK_KHR_SWAPCHAIN_SPEC_VERSION;
#endif
  pd->extension_count = k;
  vk_free(host);
  return true;
}

static VkResult vn_enumerate(VnInstance *inst) {
  if (inst->enumerated) return VK_SUCCESS;
  uint32_t n = 0;
  VkResult r = vn_call_vkEnumeratePhysicalDevices(&inst->ring, (VkInstance)inst, &n, NULL);
  if (r < 0) return r;
  VnPhysical *list = n ? vk_alloc(sizeof *list * n) : NULL;
  VkPhysicalDevice *handles = n ? vk_alloc(sizeof *handles * n) : NULL;
  if (n && (!list || !handles)) {
    vk_free(list), vk_free(handles);
    return VK_ERROR_OUT_OF_HOST_MEMORY;
  }
  for (uint32_t i = 0; i < n; ++i) {
    list[i].hdr.dispatch = ICD_LOADER_MAGIC, list[i].hdr.id = vn_new_id(), list[i].instance = inst;
    handles[i] = (VkPhysicalDevice)&list[i];
  }
  if (n) {
    r = vn_call_vkEnumeratePhysicalDevices(&inst->ring, (VkInstance)inst, &n, handles);
    if (r < 0) {
      vk_free(list), vk_free(handles);
      return r;
    }
  }
  vk_free(handles);
  uint32_t kept = 0;
  for (uint32_t i = 0; i < n; ++i)
    if (vn_physical_setup(&list[i])) {
      if (kept != i) list[kept] = list[i];
      ++kept;
    }
  inst->physicals = list, inst->physical_count = kept, inst->enumerated = true;
  return VK_SUCCESS;
}

static VkResult VKAPI_CALL vn_vkEnumeratePhysicalDevices(VkInstance instance, uint32_t *count, VkPhysicalDevice *out) {
  VnInstance *inst = vn_instance(instance);
  const VkResult r = vn_enumerate(inst);
  if (r < 0) return r;
  if (!out) {
    *count = inst->physical_count;
    return VK_SUCCESS;
  }
  const uint32_t k = *count < inst->physical_count ? *count : inst->physical_count;
  for (uint32_t i = 0; i < k; ++i) out[i] = (VkPhysicalDevice)&inst->physicals[i];
  *count = k;
  return k < inst->physical_count ? VK_INCOMPLETE : VK_SUCCESS;
}

/* One group per device: the host's groups would name devices of several adapters this context cannot link (VK_KHR_device_group_creation is not offered). */
static VkResult VKAPI_CALL vn_vkEnumeratePhysicalDeviceGroups(VkInstance instance, uint32_t *count, VkPhysicalDeviceGroupProperties *out) {
  VnInstance *inst = vn_instance(instance);
  const VkResult r = vn_enumerate(inst);
  if (r < 0) return r;
  if (!out) {
    *count = inst->physical_count;
    return VK_SUCCESS;
  }
  const uint32_t k = *count < inst->physical_count ? *count : inst->physical_count;
  for (uint32_t i = 0; i < k; ++i) {
    out[i].physicalDeviceCount = 1;
    memset(out[i].physicalDevices, 0, sizeof out[i].physicalDevices);
    out[i].physicalDevices[0] = (VkPhysicalDevice)&inst->physicals[i];
    out[i].subsetAllocation = VK_FALSE;
  }
  *count = k;
  return k < inst->physical_count ? VK_INCOMPLETE : VK_SUCCESS;
}

/* This driver's name and version in what the host says about its device: the API version it offers, its driver id and name; the host's driver goes in driverInfo. */
static void vn_adjust_properties(const VnPhysical *pd, VkPhysicalDeviceProperties *p) {
  p->apiVersion = pd->api_version;
}

static void vn_adjust_driver(const VnPhysical *pd, VkDriverId *id, char *name, char *info, VkConformanceVersion *conformance) {
  *id = VK_DRIVER_ID_MESA_VENUS;   /* (the protocol's driver id: what the host's tools know a Venus guest by) */
  copy_string(name, VK_MAX_DRIVER_NAME_SIZE, "TheoryCraftOS Venus");
  char text[VK_MAX_DRIVER_INFO_SIZE];
  copy_string(text, sizeof text, "host: ");
  size_t n = cstrlen(text);
  copy_string(text + n, sizeof text - n, pd->host_driver.driverName[0] ? pd->host_driver.driverName : pd->properties.deviceName);
  n = cstrlen(text);
  if (pd->host_driver.driverInfo[0] && n + 2 < sizeof text) {
    copy_string(text + n, sizeof text - n, " ");
    n = cstrlen(text);
    copy_string(text + n, sizeof text - n, pd->host_driver.driverInfo);
  }
  copy_string(info, VK_MAX_DRIVER_INFO_SIZE, text);
  memset(conformance, 0, sizeof *conformance);   /* (this guest driver has not been through the conformance tests) */
}

static void VKAPI_CALL vn_vkGetPhysicalDeviceProperties(VkPhysicalDevice physical, VkPhysicalDeviceProperties *p) {
  VnPhysical *pd = vn_physical(physical);
  *p = pd->properties;
  vn_adjust_properties(pd, p);
}

static void VKAPI_CALL vn_vkGetPhysicalDeviceProperties2(VkPhysicalDevice physical, VkPhysicalDeviceProperties2 *p) {
  VnPhysical *pd = vn_physical(physical);
  vn_call_vkGetPhysicalDeviceProperties2(&pd->instance->ring, physical, p);
  vn_adjust_properties(pd, &p->properties);
  for (VkBaseOutStructure *s = (VkBaseOutStructure *)p->pNext; s; s = s->pNext) {
    if (s->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES) {
      VkPhysicalDeviceDriverProperties *d = (VkPhysicalDeviceDriverProperties *)s;
      vn_adjust_driver(pd, &d->driverID, d->driverName, d->driverInfo, &d->conformanceVersion);
    } else if (s->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES) {
      VkPhysicalDeviceVulkan12Properties *d = (VkPhysicalDeviceVulkan12Properties *)s;
      vn_adjust_driver(pd, &d->driverID, d->driverName, d->driverInfo, &d->conformanceVersion);
    }
  }
}

static void VKAPI_CALL vn_vkGetPhysicalDeviceFeatures(VkPhysicalDevice physical, VkPhysicalDeviceFeatures *f) {
  vn_call_vkGetPhysicalDeviceFeatures(&vn_physical(physical)->instance->ring, physical, f);
}

/* (the host answers for its device; what this backend cannot carry is not offered as an extension, and the core features of 1.0 - 1.3 all go to the host as they are) */
static void VKAPI_CALL vn_vkGetPhysicalDeviceFeatures2(VkPhysicalDevice physical, VkPhysicalDeviceFeatures2 *f) {
  vn_call_vkGetPhysicalDeviceFeatures2(&vn_physical(physical)->instance->ring, physical, f);
}

static VkResult VKAPI_CALL vn_vkEnumerateDeviceExtensionProperties(VkPhysicalDevice physical, const char *layer, uint32_t *count, VkExtensionProperties *out) {
  if (layer) return VK_ERROR_LAYER_NOT_PRESENT;
  VnPhysical *pd = vn_physical(physical);
  return vn_fill_extensions(pd->extensions, pd->extension_count, count, out);
}

static VkResult VKAPI_CALL vn_vkEnumerateDeviceLayerProperties(VkPhysicalDevice physical, uint32_t *count, VkLayerProperties *out) {
  (void)physical, (void)out;
  *count = 0;
  return VK_SUCCESS;
}

/* No external handle types: memory, semaphores and fences of the host cannot be handed to the guest as descriptors or Windows handles here (the fd and win32
 * extensions are not offered). */
static void VKAPI_CALL vn_vkGetPhysicalDeviceExternalBufferProperties(VkPhysicalDevice physical, const VkPhysicalDeviceExternalBufferInfo *info, VkExternalBufferProperties *out) {
  (void)physical, (void)info;
  memset(&out->externalMemoryProperties, 0, sizeof out->externalMemoryProperties);
}
static void VKAPI_CALL vn_vkGetPhysicalDeviceExternalFenceProperties(VkPhysicalDevice physical, const VkPhysicalDeviceExternalFenceInfo *info, VkExternalFenceProperties *out) {
  (void)physical, (void)info;
  out->exportFromImportedHandleTypes = out->compatibleHandleTypes = 0;
  out->externalFenceFeatures = 0;
}
static void VKAPI_CALL vn_vkGetPhysicalDeviceExternalSemaphoreProperties(VkPhysicalDevice physical, const VkPhysicalDeviceExternalSemaphoreInfo *info,
                                                                       VkExternalSemaphoreProperties *out) {
  (void)physical, (void)info;
  out->exportFromImportedHandleTypes = out->compatibleHandleTypes = 0;
  out->externalSemaphoreFeatures = 0;
}

static VkResult VKAPI_CALL vn_vkGetPhysicalDeviceImageFormatProperties2(VkPhysicalDevice physical, const VkPhysicalDeviceImageFormatInfo2 *info,
                                                                      VkImageFormatProperties2 *out) {
  /* An image of an external handle type is one this backend cannot make. */
  for (const VkBaseInStructure *s = (const VkBaseInStructure *)info->pNext; s; s = s->pNext)
    if (s->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO && ((const VkPhysicalDeviceExternalImageFormatInfo *)s)->handleType)
      return VK_ERROR_FORMAT_NOT_SUPPORTED;
  const VkResult r = vn_call_vkGetPhysicalDeviceImageFormatProperties2(&vn_physical(physical)->instance->ring, physical, info, out);
  for (VkBaseOutStructure *s = (VkBaseOutStructure *)out->pNext; s; s = s->pNext)
    if (s->sType == VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES)
      memset(&((VkExternalImageFormatProperties *)s)->externalMemoryProperties, 0, sizeof(VkExternalMemoryProperties));
  return r;
}

static VkResult VKAPI_CALL vn_vkGetPhysicalDeviceToolProperties(VkPhysicalDevice physical, uint32_t *count, VkPhysicalDeviceToolProperties *out) {
  (void)physical, (void)out;
  *count = 0;   /* (no layers or tools are part of this driver) */
  return VK_SUCCESS;
}

/* ---- devices and queues ---------------------------------------------------------------------------------------------------------------------------------- */
static bool vn_device_has(const VnDevice *dev, const char *name) {
  for (uint32_t i = 0; i < dev->extension_count; ++i)
    if (streq(dev->extensions[i], name)) return true;
  return false;
}

static VkResult VKAPI_CALL vn_vkCreateDevice(VkPhysicalDevice physical, const VkDeviceCreateInfo *ci, const VkAllocationCallbacks *alloc, VkDevice *out) {
  (void)alloc;
  VnPhysical *pd = vn_physical(physical);
  for (uint32_t i = 0; i < ci->enabledExtensionCount; ++i)
    if (!vn_offered(pd->extensions, pd->extension_count, ci->ppEnabledExtensionNames[i])) return VK_ERROR_EXTENSION_NOT_PRESENT;
  uint32_t queues = 0;
  for (uint32_t i = 0; i < ci->queueCreateInfoCount; ++i) queues += ci->pQueueCreateInfos[i].queueCount;
  /* (each queue gets a timeline of the context's own: the host has 63 of them, ring 0 being the CPU's) */
  vn_lock(&pd->instance->ring_index_lock);
  uint32_t free_rings = 0;
  for (uint32_t i = 1; i < 64; ++i) free_rings += !((pd->instance->ring_indices >> i) & 1);
  vn_unlock(&pd->instance->ring_index_lock);
  if (queues > free_rings) return VK_ERROR_INITIALIZATION_FAILED;
  VnDevice *dev = vk_alloc(sizeof *dev);
  const char **host_names = vk_alloc(sizeof *host_names * (ci->enabledExtensionCount + 1));
  if (!dev || !host_names) {
    vk_free(dev), vk_free(host_names);
    return VK_ERROR_OUT_OF_HOST_MEMORY;
  }
  dev->hdr.dispatch = ICD_LOADER_MAGIC, dev->hdr.id = vn_new_id();
  dev->physical = pd, dev->instance = pd->instance, dev->api_version = pd->api_version;
  dev->queues = queues ? vk_alloc(sizeof *dev->queues * queues) : NULL;
  dev->extensions = ci->enabledExtensionCount ? vk_alloc(sizeof *dev->extensions * ci->enabledExtensionCount) : NULL;
  if ((queues && !dev->queues) || (ci->enabledExtensionCount && !dev->extensions)) {
    vk_free(dev->queues), vk_free(dev->extensions), vk_free(dev), vk_free(host_names);
    return VK_ERROR_OUT_OF_HOST_MEMORY;
  }
  /* (VK_KHR_swapchain is this driver's: the host's device does not get it) */
  uint32_t host_count = 0;
  for (uint32_t i = 0; i < ci->enabledExtensionCount; ++i) {
    copy_string(dev->extensions[i], VK_MAX_EXTENSION_NAME_SIZE, ci->ppEnabledExtensionNames[i]);
    if (!streq(ci->ppEnabledExtensionNames[i], VK_KHR_SWAPCHAIN_EXTENSION_NAME)) host_names[host_count++] = ci->ppEnabledExtensionNames[i];
  }
  dev->extension_count = ci->enabledExtensionCount;
  VkDeviceCreateInfo host = *ci;
  host.enabledExtensionCount = host_count, host.ppEnabledExtensionNames = host_count ? host_names : NULL;
  host.enabledLayerCount = 0, host.ppEnabledLayerNames = NULL;
  VkDevice handle = (VkDevice)dev;
  const VkResult r = vn_call_vkCreateDevice(&pd->instance->ring, physical, &host, NULL, &handle);
  vk_free(host_names);
  if (r != VK_SUCCESS) {
    vk_free(dev->queues), vk_free(dev->extensions), vk_free(dev);
    return r;
  }
  /* The queues: objects of the guest with ids of their own, which vkGetDeviceQueue2 tells the host about. */
  uint32_t q = 0;
  for (uint32_t i = 0; i < ci->queueCreateInfoCount; ++i)
    for (uint32_t k = 0; k < ci->pQueueCreateInfos[i].queueCount; ++k, ++q) {
      VnQueue *queue = &dev->queues[q];
      queue->hdr.dispatch = ICD_LOADER_MAGIC, queue->hdr.id = vn_new_id(), queue->device = dev;
      queue->family = ci->pQueueCreateInfos[i].queueFamilyIndex, queue->index = k, queue->flags = ci->pQueueCreateInfos[i].flags;
      vn_lock(&pd->instance->ring_index_lock);
      uint32_t ring = 1;
      while (ring < 64 && ((pd->instance->ring_indices >> ring) & 1)) ++ring;
      if (ring < 64) pd->instance->ring_indices |= (uint64_t)1 << ring;
      vn_unlock(&pd->instance->ring_index_lock);
      queue->ring_index = ring;
      const VkDeviceQueueTimelineInfoMESA timeline = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_TIMELINE_INFO_MESA, NULL, ring};
      const VkDeviceQueueInfo2 info = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_INFO_2, &timeline, queue->flags, queue->family, k};
      VkQueue qh = (VkQueue)queue;
      vn_async_vkGetDeviceQueue2(&pd->instance->ring, handle, &info, &qh);
    }
  dev->queue_count = queues;
  *out = handle;
  return VK_SUCCESS;
}

static void vn_present_device_destroy(VnDevice *dev);

static void VKAPI_CALL vn_vkDestroyDevice(VkDevice device, const VkAllocationCallbacks *alloc) {
  (void)alloc;
  if (!device) return;
  VnDevice *dev = vn_device(device);
  vn_present_device_destroy(dev);
  for (uint32_t i = 0; i < dev->queue_count; ++i)
    if (dev->queues[i].idle_fence) vn_async_vkDestroyFence(&dev->instance->ring, device, dev->queues[i].idle_fence, NULL);
  vn_async_vkDestroyDevice(&dev->instance->ring, device, NULL);
  vn_ring_roundtrip(&dev->instance->ring);
  vn_lock(&dev->instance->ring_index_lock);   /* (the host let go of the queues' timelines with the device) */
  for (uint32_t i = 0; i < dev->queue_count; ++i)
    if (dev->queues[i].ring_index < 64) dev->instance->ring_indices &= ~((uint64_t)1 << dev->queues[i].ring_index);
  vn_unlock(&dev->instance->ring_index_lock);
  vk_free(dev->queues), vk_free(dev->extensions), vk_free(dev->privates), vk_free(dev);
}

static VnQueue *vn_find_queue(VnDevice *dev, uint32_t family, uint32_t index, VkDeviceQueueCreateFlags flags) {
  for (uint32_t i = 0; i < dev->queue_count; ++i)
    if (dev->queues[i].family == family && dev->queues[i].index == index && dev->queues[i].flags == flags) return &dev->queues[i];
  return NULL;
}

static void VKAPI_CALL vn_vkGetDeviceQueue(VkDevice device, uint32_t family, uint32_t index, VkQueue *out) {
  VnQueue *q = vn_find_queue(vn_device(device), family, index, 0);
  *out = q ? (VkQueue)q : NULL;
}

static void VKAPI_CALL vn_vkGetDeviceQueue2(VkDevice device, const VkDeviceQueueInfo2 *info, VkQueue *out) {
  VnQueue *q = vn_find_queue(vn_device(device), info->queueFamilyIndex, info->queueIndex, info->flags);
  *out = q ? (VkQueue)q : NULL;
}

/* ---- waits: polled, so that the host's ring thread never blocks (other threads' commands go through the same ring) --------------------------------------------- */
static bool vn_timed_out(uint64_t start, uint64_t timeout) { return timeout != UINT64_MAX && vk_os_now_ns() - start >= timeout; }

static VkResult VKAPI_CALL vn_vkWaitForFences(VkDevice device, uint32_t count, const VkFence *fences, VkBool32 all, uint64_t timeout) {
  struct vn_ring *ring = vn_ring_of_device(device);
  const uint64_t start = vk_os_now_ns();
  VnRelax relax = {0};
  for (;;) {
    uint32_t done = 0;
    for (uint32_t i = 0; i < count; ++i) {
      const VkResult r = vn_call_vkGetFenceStatus(ring, device, fences[i]);
      if (r < 0) return r;
      if (r == VK_SUCCESS) {
        ++done;
        if (!all) return VK_SUCCESS;
      }
    }
    if (done == count) return VK_SUCCESS;
    if (timeout == 0 || vn_timed_out(start, timeout)) return VK_TIMEOUT;
    vn_relax(&relax);
  }
}

static VkResult VKAPI_CALL vn_vkWaitSemaphores(VkDevice device, const VkSemaphoreWaitInfo *info, uint64_t timeout) {
  struct vn_ring *ring = vn_ring_of_device(device);
  const bool any = info->flags & VK_SEMAPHORE_WAIT_ANY_BIT;
  const uint64_t start = vk_os_now_ns();
  VnRelax relax = {0};
  for (;;) {
    uint32_t done = 0;
    for (uint32_t i = 0; i < info->semaphoreCount; ++i) {
      uint64_t value = 0;
      const VkResult r = vn_call_vkGetSemaphoreCounterValue(ring, device, info->pSemaphores[i], &value);
      if (r < 0) return r;
      if (value >= info->pValues[i]) {
        ++done;
        if (any) return VK_SUCCESS;
      }
    }
    if (done == info->semaphoreCount) return VK_SUCCESS;
    if (timeout == 0 || vn_timed_out(start, timeout)) return VK_TIMEOUT;
    vn_relax(&relax);
  }
}

static VkResult VKAPI_CALL vn_vkQueueWaitIdle(VkQueue queue) {
  VnQueue *q = vn_queue(queue);
  VkDevice device = (VkDevice)q->device;
  struct vn_ring *ring = vn_ring_of_queue(queue);
  if (!q->idle_fence) {
    const VkFenceCreateInfo info = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, NULL, 0};
    VkFence f = VN_HANDLE(VkFence, vn_new_id());
    const VkResult r = vn_call_vkCreateFence(ring, device, &info, NULL, &f);
    if (r != VK_SUCCESS) return r;
    q->idle_fence = f;
  }
  /* (an empty submission signals its fence once everything submitted to the queue before it has finished) */
  VkResult r = vn_call_vkQueueSubmit(ring, queue, 0, NULL, q->idle_fence);
  if (r != VK_SUCCESS) return r;
  r = vn_vkWaitForFences(device, 1, &q->idle_fence, VK_TRUE, UINT64_MAX);
  const VkResult reset = vn_call_vkResetFences(ring, device, 1, &q->idle_fence);
  return r != VK_SUCCESS ? r : reset;
}

static VkResult VKAPI_CALL vn_vkDeviceWaitIdle(VkDevice device) {
  VnDevice *dev = vn_device(device);
  for (uint32_t i = 0; i < dev->queue_count; ++i) {
    const VkResult r = vn_vkQueueWaitIdle((VkQueue)&dev->queues[i]);
    if (r != VK_SUCCESS) return r;
  }
  return VK_SUCCESS;
}

static VkResult VKAPI_CALL vn_vkGetQueryPoolResults(VkDevice device, VkQueryPool pool, uint32_t first, uint32_t count, size_t size, void *data, VkDeviceSize stride,
                                                    VkQueryResultFlags flags) {
  struct vn_ring *ring = vn_ring_of_device(device);
  if (!(flags & VK_QUERY_RESULT_WAIT_BIT)) return vn_call_vkGetQueryPoolResults(ring, device, pool, first, count, size, data, stride, flags);
  /* WAIT: asked again until every result is there (VK_NOT_READY says one is not) */
  VnRelax relax = {0};
  for (;;) {
    const VkResult r = vn_call_vkGetQueryPoolResults(ring, device, pool, first, count, size, data, stride, flags & ~VK_QUERY_RESULT_WAIT_BIT);
    if (r != VK_NOT_READY) return r;
    vn_relax(&relax);
  }
}

/* ---- memory ------------------------------------------------------------------------------------------------------------------------------------------------ */
/* Host-visible memory is the host's: on the first map it becomes a blob of the context (blob id = the memory's id; the host exports it to QEMU, which maps it into
 * the device's host memory window) and the window's pages are mapped into the process. Such an allocation is made a whole number of pages (the window maps pages). */
static VkResult VKAPI_CALL vn_vkAllocateMemory(VkDevice device, const VkMemoryAllocateInfo *info, const VkAllocationCallbacks *alloc, VkDeviceMemory *out) {
  (void)alloc;
  VnDevice *dev = vn_device(device);
  if (info->memoryTypeIndex >= dev->physical->memory.memoryTypeCount) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
  VnMemory *mem = vk_alloc(sizeof *mem);
  if (!mem) return VK_ERROR_OUT_OF_HOST_MEMORY;
  mem->obj.id = vn_new_id(), mem->device = dev, mem->type = info->memoryTypeIndex;
  mem->flags = dev->physical->memory.memoryTypes[info->memoryTypeIndex].propertyFlags;
  VkMemoryAllocateInfo host = *info;
  if (mem->flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) {
    if (host.allocationSize > UINT64_MAX - 4095) {
      vk_free(mem);
      return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    }
    host.allocationSize = (host.allocationSize + 4095) & ~(VkDeviceSize)4095;
  }
  mem->size = host.allocationSize;
  VkDeviceMemory handle = VN_HANDLE(VkDeviceMemory, mem);
  const VkResult r = vn_call_vkAllocateMemory(vn_ring_of_device(device), device, &host, NULL, &handle);
  if (r != VK_SUCCESS) {
    vk_free(mem);
    return r;
  }
  *out = handle;
  return VK_SUCCESS;
}

static void vn_memory_unmap_blob(VnMemory *mem) {
  VnRenderer *r = &mem->device->instance->renderer;
  if (mem->map) vk_os_virtgpu_unmap(r->node, mem->map, mem->size);
  if (mem->bo) {
    TcosDrmGemClose close = {mem->bo, 0};
    vn_ioctl(r, TCOS_DRM_IOCTL_GEM_CLOSE, &close);
  }
  mem->map = NULL, mem->bo = mem->res = 0;
}

static void VKAPI_CALL vn_vkFreeMemory(VkDevice device, VkDeviceMemory memory, const VkAllocationCallbacks *alloc) {
  (void)alloc;
  if (!memory) return;
  VnMemory *mem = vn_memory(memory);
  vn_memory_unmap_blob(mem);   /* (the blob goes first: the host's memory is freed after QEMU no longer maps it) */
  vn_async_vkFreeMemory(vn_ring_of_device(device), device, memory, NULL);
  vk_free(mem);
}

static VkResult vn_memory_map_blob(VnMemory *mem) {
  if (mem->map) return VK_SUCCESS;
  if (!(mem->flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) return VK_ERROR_MEMORY_MAP_FAILED;
  VnRenderer *r = &mem->device->instance->renderer;
  /* The host must have made the memory before the blob that names it: everything sent so far is consumed first. */
  vn_ring_roundtrip(&mem->device->instance->ring);
  TcosVirtgpuResourceCreateBlob blob;
  memset(&blob, 0, sizeof blob);
  blob.blob_mem = TCOS_VIRTGPU_BLOB_MEM_HOST3D;
  blob.blob_flags = TCOS_VIRTGPU_BLOB_FLAG_USE_MAPPABLE | TCOS_VIRTGPU_BLOB_FLAG_USE_SHAREABLE;
  blob.size = mem->size, blob.blob_id = mem->obj.id;
  if (vn_ioctl(r, TCOS_DRM_IOCTL_VIRTGPU_RESOURCE_CREATE_BLOB, &blob)) return VK_ERROR_MEMORY_MAP_FAILED;
  mem->bo = blob.bo_handle, mem->res = blob.res_handle;
  TcosVirtgpuMap map = {0, blob.bo_handle, 0};
  if (!vn_ioctl(r, TCOS_DRM_IOCTL_VIRTGPU_MAP, &map)) mem->map = vk_os_virtgpu_map(r->node, map.offset, mem->size);
  if (!mem->map) {
    vn_memory_unmap_blob(mem);
    return VK_ERROR_MEMORY_MAP_FAILED;
  }
  return VK_SUCCESS;
}

static VkResult VKAPI_CALL vn_vkMapMemory(VkDevice device, VkDeviceMemory memory, VkDeviceSize offset, VkDeviceSize size, VkMemoryMapFlags flags, void **data) {
  (void)device, (void)flags;
  VnMemory *mem = vn_memory(memory);
  if (offset >= mem->size || (size != VK_WHOLE_SIZE && size > mem->size - offset)) return VK_ERROR_MEMORY_MAP_FAILED;
  const VkResult r = vn_memory_map_blob(mem);
  if (r != VK_SUCCESS) {
    *data = NULL;
    return r;
  }
  *data = mem->map + offset;
  return VK_SUCCESS;
}

/* (the mapping stays until the memory is freed: a later map finds it again; the pointer the program had is not used after an unmap) */
static void VKAPI_CALL vn_vkUnmapMemory(VkDevice device, VkDeviceMemory memory) { (void)device, (void)memory; }

static VkResult VKAPI_CALL vn_vkMapMemory2(VkDevice device, const VkMemoryMapInfo *info, void **data) {
  for (const VkBaseInStructure *s = (const VkBaseInStructure *)info->pNext; s; s = s->pNext)
    if (s->sType == VK_STRUCTURE_TYPE_MEMORY_MAP_PLACED_INFO_EXT) return VK_ERROR_MEMORY_MAP_FAILED;   /* (VK_EXT_map_memory_placed is not offered) */
  return vn_vkMapMemory(device, info->memory, info->offset, info->size, info->flags, data);
}

static VkResult VKAPI_CALL vn_vkUnmapMemory2(VkDevice device, const VkMemoryUnmapInfo *info) {
  vn_vkUnmapMemory(device, info->memory);
  return VK_SUCCESS;
}

/* ---- command pools and buffers --------------------------------------------------------------------------------------------------------------------------- */
/* A command buffer's commands are encoded into a stream of its own as they are recorded (vkBeginCommandBuffer first) and sent to the host at vkEndCommandBuffer: the
 * host records them into its command buffer, before any later submission that names it. */
static VkResult VKAPI_CALL vn_vkCreateCommandPool(VkDevice device, const VkCommandPoolCreateInfo *info, const VkAllocationCallbacks *alloc, VkCommandPool *out) {
  (void)alloc;
  VnCommandPool *pool = vk_alloc(sizeof *pool);
  if (!pool) return VK_ERROR_OUT_OF_HOST_MEMORY;
  pool->obj.id = vn_new_id(), pool->device = vn_device(device);
  VkCommandPool handle = VN_HANDLE(VkCommandPool, pool);
  const VkResult r = vn_call_vkCreateCommandPool(vn_ring_of_device(device), device, info, NULL, &handle);
  if (r != VK_SUCCESS) {
    vk_free(pool);
    return r;
  }
  *out = handle;
  return VK_SUCCESS;
}

static void vn_command_buffer_free(VnCommandBuffer *cmd) {
  vk_free(cmd->cs.base);
  vk_free(cmd);
}

static void VKAPI_CALL vn_vkDestroyCommandPool(VkDevice device, VkCommandPool pool_handle, const VkAllocationCallbacks *alloc) {
  (void)alloc;
  if (!pool_handle) return;
  VnCommandPool *pool = vn_pool(pool_handle);
  vn_async_vkDestroyCommandPool(vn_ring_of_device(device), device, pool_handle, NULL);
  for (VnCommandBuffer *c = pool->buffers, *next; c; c = next) {
    next = c->next;
    vn_command_buffer_free(c);
  }
  vk_free(pool);
}

static VkResult VKAPI_CALL vn_vkResetCommandPool(VkDevice device, VkCommandPool pool_handle, VkCommandPoolResetFlags flags) {
  VnCommandPool *pool = vn_pool(pool_handle);
  for (VnCommandBuffer *c = pool->buffers; c; c = c->next) c->cs.cur = c->cs.base, c->cs.fatal = false, c->recording = false;
  return vn_call_vkResetCommandPool(vn_ring_of_device(device), device, pool_handle, flags);
}

static VkResult VKAPI_CALL vn_vkAllocateCommandBuffers(VkDevice device, const VkCommandBufferAllocateInfo *info, VkCommandBuffer *out) {
  VnCommandPool *pool = vn_pool(info->commandPool);
  const uint32_t n = info->commandBufferCount;
  for (uint32_t i = 0; i < n; ++i) {
    VnCommandBuffer *cmd = vk_alloc(sizeof *cmd);
    if (!cmd) {
      for (uint32_t k = 0; k < i; ++k) vn_command_buffer_free(vn_command_buffer(out[k]));
      for (uint32_t k = 0; k < n; ++k) out[k] = NULL;
      return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    cmd->hdr.dispatch = ICD_LOADER_MAGIC, cmd->hdr.id = vn_new_id(), cmd->device = vn_device(device), cmd->pool = pool;
    cmd->cs.growable = true;
    out[i] = (VkCommandBuffer)cmd;
  }
  const VkResult r = vn_call_vkAllocateCommandBuffers(vn_ring_of_device(device), device, info, out);
  if (r != VK_SUCCESS) {
    for (uint32_t k = 0; k < n; ++k) {
      vn_command_buffer_free(vn_command_buffer(out[k]));
      out[k] = NULL;
    }
    return r;
  }
  for (uint32_t i = 0; i < n; ++i) {
    VnCommandBuffer *cmd = vn_command_buffer(out[i]);
    cmd->next = pool->buffers;
    pool->buffers = cmd;
  }
  return VK_SUCCESS;
}

static void VKAPI_CALL vn_vkFreeCommandBuffers(VkDevice device, VkCommandPool pool_handle, uint32_t count, const VkCommandBuffer *buffers) {
  VnCommandPool *pool = vn_pool(pool_handle);
  vn_async_vkFreeCommandBuffers(vn_ring_of_device(device), device, pool_handle, count, buffers);
  for (uint32_t i = 0; i < count; ++i) {
    VnCommandBuffer *cmd = vn_command_buffer(buffers[i]);
    if (!cmd) continue;
    for (VnCommandBuffer **link = &pool->buffers; *link; link = &(*link)->next)
      if (*link == cmd) {
        *link = cmd->next;
        break;
      }
    vn_command_buffer_free(cmd);
  }
}

static VkResult VKAPI_CALL vn_vkBeginCommandBuffer(VkCommandBuffer buffer, const VkCommandBufferBeginInfo *info) {
  VnCommandBuffer *cmd = vn_command_buffer(buffer);
  cmd->cs.cur = cmd->cs.base, cmd->cs.fatal = false;   /* (an implicit reset of what was recorded before) */
  cmd->recording = true;
  if (!vn_command_reserve(cmd, vn_sizeof_vkBeginCommandBuffer(buffer, info))) return VK_ERROR_OUT_OF_HOST_MEMORY;
  vn_encode_vkBeginCommandBuffer(&cmd->cs, 0, buffer, info);
  return cmd->cs.fatal ? VK_ERROR_OUT_OF_HOST_MEMORY : VK_SUCCESS;
}

static VkResult VKAPI_CALL vn_vkEndCommandBuffer(VkCommandBuffer buffer) {
  VnCommandBuffer *cmd = vn_command_buffer(buffer);
  if (!vn_command_reserve(cmd, vn_sizeof_vkEndCommandBuffer(buffer))) {
    cmd->recording = false;
    return VK_ERROR_OUT_OF_HOST_MEMORY;
  }
  vn_encode_vkEndCommandBuffer(&cmd->cs, 0, buffer);
  cmd->recording = false;
  if (cmd->cs.fatal) return VK_ERROR_OUT_OF_HOST_MEMORY;
  struct vn_ring *ring = vn_ring_of_command_buffer(buffer);
  vn_lock(&ring->lock);
  const bool sent = vn_ring_submit_locked(ring, cmd->cs.base, vn_cs_encoder_get_len(&cmd->cs), NULL);
  vn_unlock(&ring->lock);
  cmd->cs.cur = cmd->cs.base;
  return sent ? VK_SUCCESS : VK_ERROR_DEVICE_LOST;
}

static VkResult VKAPI_CALL vn_vkResetCommandBuffer(VkCommandBuffer buffer, VkCommandBufferResetFlags flags) {
  VnCommandBuffer *cmd = vn_command_buffer(buffer);
  cmd->cs.cur = cmd->cs.base, cmd->cs.fatal = false, cmd->recording = false;
  return vn_call_vkResetCommandBuffer(vn_ring_of_command_buffer(buffer), buffer, flags);
}

/* ---- descriptor update templates: kept by the guest and turned into the descriptor writes they stand for ------------------------------------------------ */
static VkResult VKAPI_CALL vn_vkCreateDescriptorUpdateTemplate(VkDevice device, const VkDescriptorUpdateTemplateCreateInfo *info, const VkAllocationCallbacks *alloc,
                                                               VkDescriptorUpdateTemplate *out) {
  (void)device, (void)alloc;
  VnTemplate *t = vk_alloc(sizeof *t);
  VkDescriptorUpdateTemplateEntry *entries = info->descriptorUpdateEntryCount ? vk_alloc(sizeof *entries * info->descriptorUpdateEntryCount) : NULL;
  if (!t || (info->descriptorUpdateEntryCount && !entries)) {
    vk_free(t), vk_free(entries);
    return VK_ERROR_OUT_OF_HOST_MEMORY;
  }
  t->obj.id = vn_new_id();
  t->type = info->templateType, t->entries = entries, t->entry_count = info->descriptorUpdateEntryCount;
  for (uint32_t i = 0; i < t->entry_count; ++i) entries[i] = info->pDescriptorUpdateEntries[i];
  t->bind_point = info->pipelineBindPoint, t->layout = info->pipelineLayout, t->set = info->set;
  *out = VN_HANDLE(VkDescriptorUpdateTemplate, t);
  return VK_SUCCESS;
}

static void VKAPI_CALL vn_vkDestroyDescriptorUpdateTemplate(VkDevice device, VkDescriptorUpdateTemplate handle, const VkAllocationCallbacks *alloc) {
  (void)device, (void)alloc;
  if (!handle) return;
  VnTemplate *t = vn_template(handle);
  vk_free(t->entries);
  vk_free(t);
}

/* The writes a template stands for, with `data` as the source (the infos are gathered into one allocation; *storage is freed by the caller). */
typedef struct VnWrites {
  VkWriteDescriptorSet *writes;
  uint32_t count;
  void *storage;
} VnWrites;

static bool vn_template_writes(const VnTemplate *t, VkDescriptorSet set, const void *data, VnWrites *out) {
  size_t bytes = sizeof(VkWriteDescriptorSet) * t->entry_count;
  for (uint32_t i = 0; i < t->entry_count; ++i) {
    const VkDescriptorUpdateTemplateEntry *e = &t->entries[i];
    if (e->descriptorType == VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK) bytes += sizeof(VkWriteDescriptorSetInlineUniformBlock);
    else bytes += (size_t)e->descriptorCount * (sizeof(VkDescriptorImageInfo) > sizeof(VkDescriptorBufferInfo) ? sizeof(VkDescriptorImageInfo) : sizeof(VkDescriptorBufferInfo));
  }
  uint8_t *storage = vk_alloc(bytes ? bytes : 1);
  if (!storage) return false;
  VkWriteDescriptorSet *writes = (VkWriteDescriptorSet *)storage;
  uint8_t *at = storage + sizeof(VkWriteDescriptorSet) * t->entry_count;
  const uint8_t *src = data;
  for (uint32_t i = 0; i < t->entry_count; ++i) {
    const VkDescriptorUpdateTemplateEntry *e = &t->entries[i];
    VkWriteDescriptorSet *w = &writes[i];
    memset(w, 0, sizeof *w);
    w->sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    w->dstSet = set, w->dstBinding = e->dstBinding, w->dstArrayElement = e->dstArrayElement;
    w->descriptorCount = e->descriptorCount, w->descriptorType = e->descriptorType;
    switch (e->descriptorType) {
      case VK_DESCRIPTOR_TYPE_SAMPLER:
      case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
      case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
      case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
      case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT: {
        VkDescriptorImageInfo *infos = (VkDescriptorImageInfo *)at;
        for (uint32_t k = 0; k < e->descriptorCount; ++k) memcpy(&infos[k], src + e->offset + (size_t)k * e->stride, sizeof infos[k]);
        w->pImageInfo = infos;
        at += sizeof *infos * e->descriptorCount;
        break;
      }
      case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
      case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER: {
        VkBufferView *views = (VkBufferView *)at;
        for (uint32_t k = 0; k < e->descriptorCount; ++k) memcpy(&views[k], src + e->offset + (size_t)k * e->stride, sizeof views[k]);
        w->pTexelBufferView = views;
        at += sizeof(VkDescriptorBufferInfo) * e->descriptorCount;   /* (room was counted per descriptor at the larger size) */
        break;
      }
      case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
      case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
      case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC:
      case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC: {
        VkDescriptorBufferInfo *infos = (VkDescriptorBufferInfo *)at;
        for (uint32_t k = 0; k < e->descriptorCount; ++k) memcpy(&infos[k], src + e->offset + (size_t)k * e->stride, sizeof infos[k]);
        w->pBufferInfo = infos;
        at += sizeof(VkDescriptorBufferInfo) * e->descriptorCount;
        break;
      }
      case VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK: {
        /* (descriptorCount is the bytes, at offset: the data is one block) */
        VkWriteDescriptorSetInlineUniformBlock *block = (VkWriteDescriptorSetInlineUniformBlock *)at;
        block->sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_INLINE_UNIFORM_BLOCK, block->pNext = NULL;
        block->dataSize = e->descriptorCount, block->pData = src + e->offset;
        w->pNext = block;
        at += sizeof *block;
        break;
      }
      default:   /* (a type of an extension that is not offered: acceleration structures) */
        vk_free(storage);
        return false;
    }
  }
  out->writes = writes, out->count = t->entry_count, out->storage = storage;
  return true;
}

static void VKAPI_CALL vn_vkUpdateDescriptorSetWithTemplate(VkDevice device, VkDescriptorSet set, VkDescriptorUpdateTemplate handle, const void *data) {
  VnWrites w;
  if (!vn_template_writes(vn_template(handle), set, data, &w)) {
    vk_os_trace("venus: vkUpdateDescriptorSetWithTemplate: out of memory (the set is not updated)\n");
    return;
  }
  vn_async_vkUpdateDescriptorSets(vn_ring_of_device(device), device, w.count, w.writes, 0, NULL);
  vk_free(w.storage);
}

static void vn_push_with_template(VkCommandBuffer buffer, VnTemplate *t, VkPipelineLayout layout, uint32_t set, const void *data) {
  VnCommandBuffer *cmd = vn_command_buffer(buffer);
  VnWrites w;
  if (!vn_template_writes(t, VK_NULL_HANDLE, data, &w)) {
    cmd->cs.fatal = true;   /* (vkEndCommandBuffer reports it) */
    return;
  }
  if (vn_command_reserve(cmd, vn_sizeof_vkCmdPushDescriptorSet(buffer, t->bind_point, layout, set, w.count, w.writes)))
    vn_encode_vkCmdPushDescriptorSet(&cmd->cs, 0, buffer, t->bind_point, layout, set, w.count, w.writes);
  vk_free(w.storage);
}

static void VKAPI_CALL vn_vkCmdPushDescriptorSetWithTemplate(VkCommandBuffer buffer, VkDescriptorUpdateTemplate handle, VkPipelineLayout layout, uint32_t set,
                                                            const void *data) {
  vn_push_with_template(buffer, vn_template(handle), layout, set, data);
}

static void VKAPI_CALL vn_vkCmdPushDescriptorSetWithTemplate2(VkCommandBuffer buffer, const VkPushDescriptorSetWithTemplateInfo *info) {
  vn_push_with_template(buffer, vn_template(info->descriptorUpdateTemplate), info->layout, info->set, info->pData);
}

/* ---- private data: the guest's own (the host's vkr has no private data; Mesa keeps it in the guest too) ---------------------------------------------------- */
/* A slot is an id; the values are kept per device, by object type, object handle and slot (a slot that is destroyed takes its values with it). */
static VkResult VKAPI_CALL vn_vkCreatePrivateDataSlot(VkDevice device, const VkPrivateDataSlotCreateInfo *info, const VkAllocationCallbacks *alloc, VkPrivateDataSlot *out) {
  (void)device, (void)info, (void)alloc;
  *out = VN_HANDLE(VkPrivateDataSlot, vn_new_id());
  return VK_SUCCESS;
}

static void VKAPI_CALL vn_vkDestroyPrivateDataSlot(VkDevice device, VkPrivateDataSlot slot, const VkAllocationCallbacks *alloc) {
  (void)alloc;
  if (!slot) return;
  VnDevice *dev = vn_device(device);
  vn_lock(&dev->private_lock);
  uint32_t kept = 0;
  for (uint32_t i = 0; i < dev->private_count; ++i)
    if (dev->privates[i].slot != slot) dev->privates[kept++] = dev->privates[i];
  dev->private_count = kept;
  vn_unlock(&dev->private_lock);
}

static VkResult VKAPI_CALL vn_vkSetPrivateData(VkDevice device, VkObjectType type, uint64_t object, VkPrivateDataSlot slot, uint64_t value) {
  VnDevice *dev = vn_device(device);
  VkResult r = VK_SUCCESS;
  vn_lock(&dev->private_lock);
  uint32_t i = 0;
  while (i < dev->private_count && !(dev->privates[i].type == type && dev->privates[i].object == object && dev->privates[i].slot == slot)) ++i;
  if (i == dev->private_count) {
    const VnPrivate p = {type, object, slot, value};
    if (!LIST_PUSH(dev->privates, dev->private_count, dev->private_capacity, p)) r = VK_ERROR_OUT_OF_HOST_MEMORY;
  } else {
    dev->privates[i].value = value;
  }
  vn_unlock(&dev->private_lock);
  return r;
}

static void VKAPI_CALL vn_vkGetPrivateData(VkDevice device, VkObjectType type, uint64_t object, VkPrivateDataSlot slot, uint64_t *value) {
  VnDevice *dev = vn_device(device);
  *value = 0;   /* (never set: zero, as the specification says) */
  vn_lock(&dev->private_lock);
  for (uint32_t i = 0; i < dev->private_count; ++i)
    if (dev->privates[i].type == type && dev->privates[i].object == object && dev->privates[i].slot == slot) *value = dev->privates[i].value;
  vn_unlock(&dev->private_lock);
}

/* ---- the forwarded commands (generated) ---------------------------------------------------------------------------------------------------------------- */
#include "vn_commands.inc"

#if VN_NO_PRESENT
/* (built without presentation - the kernel's own Vulkan, src/Graphics/Graphics3D/Venus/KernelVulkan.c, shows its images itself: no instance or device ever has the
 * Direct3D backend's objects, so there is nothing of them to destroy) */
static void vn_hidden_destroy(VnInstance *inst) { (void)inst; }
static void vn_present_device_destroy(VnDevice *dev) { (void)dev; }
#else
/* ---- presentation -------------------------------------------------------------------------------------------------------------------------------------- */
/* A surface is the platform's (made by its vkCreate*SurfaceKHR, the same object for both backends). A swapchain of a Venus device has images of the host (made
 * like the program's own, with TRANSFER_SRC added) and, behind them, a swapchain of the Direct3D backend on the same surface, on a device of its own: at present
 * the host copies the image into a buffer in memory the guest maps, the guest copies that into the Direct3D swapchain's image and presents it there. So every
 * surface kind the platform has (Win32 windows, Wayland, X11, headless) works, at the cost of one copy of the frame through guest memory. */
static void vn_hidden_destroy(VnInstance *inst) {
  if (inst->present.instance) dx_vkDestroyInstance(inst->present.instance, NULL);
  memset(&inst->present, 0, sizeof inst->present);
}

static VkPhysicalDevice vn_hidden_physical(VnInstance *inst) {
  if (inst->present.tried) return inst->present.physical;
  inst->present.tried = true;
  const char *names[sizeof kInstanceExtensions / sizeof kInstanceExtensions[0]];
  for (uint32_t i = 0; i < sizeof names / sizeof names[0]; ++i) names[i] = kInstanceExtensions[i].extensionName;
  const VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "venus presentation", 0, NULL, 0, VK_API_VERSION_1_0};
  const VkInstanceCreateInfo ci = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &app, 0, NULL, (uint32_t)(sizeof names / sizeof names[0]), names};
  if (dx_vkCreateInstance(&ci, NULL, &inst->present.instance) != VK_SUCCESS) {
    vk_os_trace("venus: no Direct3D adapter to present on (surfaces cannot be shown)\n");
    inst->present.instance = VK_NULL_HANDLE;
    return VK_NULL_HANDLE;
  }
  uint32_t n = 1;
  VkPhysicalDevice pd = VK_NULL_HANDLE;
  if (dx_vkEnumeratePhysicalDevices(inst->present.instance, &n, &pd) < 0 || n == 0) {
    dx_vkDestroyInstance(inst->present.instance, NULL);
    inst->present.instance = VK_NULL_HANDLE;
    return VK_NULL_HANDLE;
  }
  inst->present.physical = pd;
  return pd;
}

static bool vn_present_device(VnDevice *dev) {
  VnPresentDevice *p = &dev->present;
  if (p->tried) return p->device != VK_NULL_HANDLE;
  p->tried = true;
  VkPhysicalDevice pd = vn_hidden_physical(dev->instance);
  if (!pd) return false;
  const float priority = 1.0f;
  const VkDeviceQueueCreateInfo q = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, NULL, 0, 0, 1, &priority};
  const char *ext = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
  const VkDeviceCreateInfo ci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, NULL, 0, 1, &q, 0, NULL, 1, &ext, NULL};
  if (dx_vkCreateDevice(pd, &ci, NULL, &p->device) != VK_SUCCESS) {
    p->device = VK_NULL_HANDLE;
    return false;
  }
  dx_vkGetDeviceQueue(p->device, 0, 0, &p->queue);
  const VkCommandPoolCreateInfo pool = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL, VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, 0};
  if (dx_vkCreateCommandPool(p->device, &pool, NULL, &p->pool) != VK_SUCCESS) {
    dx_vkDestroyDevice(p->device, NULL);
    p->device = VK_NULL_HANDLE;
    return false;
  }
  return true;
}

static void vn_present_device_destroy(VnDevice *dev) {
  if (dev->present.device) {
    dx_vkDestroyCommandPool(dev->present.device, dev->present.pool, NULL);
    dx_vkDestroyDevice(dev->present.device, NULL);
  }
  memset(&dev->present, 0, sizeof dev->present);
}

static bool vn_family_can_present(VnPhysical *pd, uint32_t family) {
  uint32_t n = 0;
  vn_call_vkGetPhysicalDeviceQueueFamilyProperties(&pd->instance->ring, (VkPhysicalDevice)pd, &n, NULL);
  if (family >= n || n > 64) return false;
  VkQueueFamilyProperties props[64];
  vn_call_vkGetPhysicalDeviceQueueFamilyProperties(&pd->instance->ring, (VkPhysicalDevice)pd, &n, props);
  return family < n && (props[family].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT));   /* (the copy at present needs one of them) */
}

static VkResult VKAPI_CALL vn_vkGetPhysicalDeviceSurfaceSupportKHR(VkPhysicalDevice physical, uint32_t family, VkSurfaceKHR surface, VkBool32 *supported) {
  VnPhysical *pd = vn_physical(physical);
  VkPhysicalDevice dx = vn_hidden_physical(pd->instance);
  *supported = VK_FALSE;
  if (!dx || !vn_family_can_present(pd, family)) return VK_SUCCESS;
  return dx_vkGetPhysicalDeviceSurfaceSupportKHR(dx, 0, surface, supported);
}

static VkResult VKAPI_CALL vn_vkGetPhysicalDeviceSurfaceCapabilitiesKHR(VkPhysicalDevice physical, VkSurfaceKHR surface, VkSurfaceCapabilitiesKHR *caps) {
  VkPhysicalDevice dx = vn_hidden_physical(vn_physical(physical)->instance);
  return dx ? dx_vkGetPhysicalDeviceSurfaceCapabilitiesKHR(dx, surface, caps) : VK_ERROR_SURFACE_LOST_KHR;
}

static VkResult VKAPI_CALL vn_vkGetPhysicalDeviceSurfaceFormatsKHR(VkPhysicalDevice physical, VkSurfaceKHR surface, uint32_t *count, VkSurfaceFormatKHR *out) {
  VkPhysicalDevice dx = vn_hidden_physical(vn_physical(physical)->instance);
  return dx ? dx_vkGetPhysicalDeviceSurfaceFormatsKHR(dx, surface, count, out) : VK_ERROR_SURFACE_LOST_KHR;
}

static VkResult VKAPI_CALL vn_vkGetPhysicalDeviceSurfacePresentModesKHR(VkPhysicalDevice physical, VkSurfaceKHR surface, uint32_t *count, VkPresentModeKHR *out) {
  VkPhysicalDevice dx = vn_hidden_physical(vn_physical(physical)->instance);
  return dx ? dx_vkGetPhysicalDeviceSurfacePresentModesKHR(dx, surface, count, out) : VK_ERROR_SURFACE_LOST_KHR;
}

static VkResult VKAPI_CALL vn_vkGetPhysicalDevicePresentRectanglesKHR(VkPhysicalDevice physical, VkSurfaceKHR surface, uint32_t *count, VkRect2D *out) {
  VkSurfaceCapabilitiesKHR caps;
  const VkResult r = vn_vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical, surface, &caps);
  if (r != VK_SUCCESS) return r;
  if (!out) {
    *count = 1;
    return VK_SUCCESS;
  }
  if (*count == 0) return VK_INCOMPLETE;
  out[0].offset.x = out[0].offset.y = 0;
  out[0].extent = caps.currentExtent.width == 0xFFFFFFFFu ? caps.minImageExtent : caps.currentExtent;
  *count = 1;
  return VK_SUCCESS;
}

static VkResult VKAPI_CALL vn_vkGetDeviceGroupPresentCapabilitiesKHR(VkDevice device, VkDeviceGroupPresentCapabilitiesKHR *caps) {
  (void)device;
  memset(caps->presentMask, 0, sizeof caps->presentMask);
  caps->presentMask[0] = 1;
  caps->modes = VK_DEVICE_GROUP_PRESENT_MODE_LOCAL_BIT_KHR;
  return VK_SUCCESS;
}

static VkResult VKAPI_CALL vn_vkGetDeviceGroupSurfacePresentModesKHR(VkDevice device, VkSurfaceKHR surface, VkDeviceGroupPresentModeFlagsKHR *modes) {
  (void)device, (void)surface;
  *modes = VK_DEVICE_GROUP_PRESENT_MODE_LOCAL_BIT_KHR;
  return VK_SUCCESS;
}

typedef struct VnSwapImage {
  VkImage image;                 /* the host's: the program draws into it */
  VkDeviceMemory memory;
  VkBuffer buffer;               /* the host copies the frame here: memory the guest maps */
  VkDeviceMemory buffer_memory;
  const uint8_t *pixels;
  VkCommandBuffer copy;          /* image -> buffer, recorded for the pool's queue family */
  VkBuffer dx_buffer;            /* the Direct3D backend's: the frame goes from here into its swapchain image */
  VkDeviceMemory dx_memory;
  uint8_t *dx_pixels;
  VkCommandBuffer dx_copy;
} VnSwapImage;

typedef struct VnSwapchain {
  VnDevice *device;
  VkSwapchainKHR dx;
  uint32_t count;
  VnSwapImage *images;
  VkExtent2D extent;
  VkDeviceSize bytes;
  VkCommandPool pool;            /* (the host's, for the copies; remade for another queue family) */
  uint32_t pool_family;
  VkFence fence;
} VnSwapchain;

static uint32_t vn_memory_type(const VnPhysical *pd, uint32_t bits, VkMemoryPropertyFlags need, VkMemoryPropertyFlags prefer) {
  uint32_t fallback = UINT32_MAX;
  for (uint32_t i = 0; i < pd->memory.memoryTypeCount; ++i) {
    if (!((bits >> i) & 1) || (pd->memory.memoryTypes[i].propertyFlags & need) != need) continue;
    if ((pd->memory.memoryTypes[i].propertyFlags & prefer) == prefer) return i;
    if (fallback == UINT32_MAX) fallback = i;
  }
  return fallback;
}

static void vn_swap_image_destroy(VnSwapchain *sc, VnSwapImage *im) {
  VkDevice device = (VkDevice)sc->device;
  VkDevice dx = sc->device->present.device;
  if (im->dx_copy) dx_vkFreeCommandBuffers(dx, sc->device->present.pool, 1, &im->dx_copy);
  if (im->dx_buffer) dx_vkDestroyBuffer(dx, im->dx_buffer, NULL);
  if (im->dx_memory) dx_vkFreeMemory(dx, im->dx_memory, NULL);
  if (im->buffer) vn_vkDestroyBuffer(device, im->buffer, NULL);
  if (im->buffer_memory) vn_vkFreeMemory(device, im->buffer_memory, NULL);
  if (im->image) vn_vkDestroyImage(device, im->image, NULL);
  if (im->memory) vn_vkFreeMemory(device, im->memory, NULL);
  memset(im, 0, sizeof *im);
}

static void VKAPI_CALL vn_vkDestroySwapchainKHR(VkDevice device, VkSwapchainKHR swapchain, const VkAllocationCallbacks *alloc) {
  (void)alloc;
  if (!swapchain) return;
  VnSwapchain *sc = (VnSwapchain *)(uintptr_t)swapchain;
  vn_vkDeviceWaitIdle(device);   /* (a copy may still be running) */
  for (uint32_t i = 0; i < sc->count; ++i) vn_swap_image_destroy(sc, &sc->images[i]);
  if (sc->pool) vn_vkDestroyCommandPool(device, sc->pool, NULL);
  if (sc->fence) vn_vkDestroyFence(device, sc->fence, NULL);
  if (sc->dx) dx_vkDestroySwapchainKHR(sc->device->present.device, sc->dx, NULL);
  vk_free(sc->images);
  vk_free(sc);
}

/* The host's image, the buffer it is copied into (mapped), and the Direct3D side's buffer and copy. */
static VkResult vn_swap_image_make(VnSwapchain *sc, VnSwapImage *im, const VkSwapchainCreateInfoKHR *ci, VkImage dx_image) {
  VnDevice *dev = sc->device;
  VkDevice device = (VkDevice)dev;
  const VkImageCreateInfo ici = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, NULL, 0, VK_IMAGE_TYPE_2D, ci->imageFormat, {ci->imageExtent.width, ci->imageExtent.height, 1}, 1,
                                 ci->imageArrayLayers ? ci->imageArrayLayers : 1, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_TILING_OPTIMAL,
                                 ci->imageUsage | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, ci->imageSharingMode, ci->queueFamilyIndexCount, ci->pQueueFamilyIndices,
                                 VK_IMAGE_LAYOUT_UNDEFINED};
  VkResult r = vn_vkCreateImage(device, &ici, NULL, &im->image);
  if (r != VK_SUCCESS) return r;
  VkMemoryRequirements req;
  vn_vkGetImageMemoryRequirements(device, im->image, &req);
  uint32_t type = vn_memory_type(dev->physical, req.memoryTypeBits, 0, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  if (type == UINT32_MAX) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
  VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, req.size, type};
  if ((r = vn_vkAllocateMemory(device, &mai, NULL, &im->memory)) != VK_SUCCESS) return r;
  if ((r = vn_vkBindImageMemory(device, im->image, im->memory, 0)) != VK_SUCCESS) return r;
  const VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, sc->bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, NULL};
  if ((r = vn_vkCreateBuffer(device, &bci, NULL, &im->buffer)) != VK_SUCCESS) return r;
  vn_vkGetBufferMemoryRequirements(device, im->buffer, &req);
  type = vn_memory_type(dev->physical, req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
  if (type == UINT32_MAX) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
  mai.allocationSize = req.size, mai.memoryTypeIndex = type;
  if ((r = vn_vkAllocateMemory(device, &mai, NULL, &im->buffer_memory)) != VK_SUCCESS) return r;
  if ((r = vn_vkBindBufferMemory(device, im->buffer, im->buffer_memory, 0)) != VK_SUCCESS) return r;
  void *pixels = NULL;
  if ((r = vn_vkMapMemory(device, im->buffer_memory, 0, VK_WHOLE_SIZE, 0, &pixels)) != VK_SUCCESS) return r;
  im->pixels = pixels;
  /* the Direct3D side: a buffer it can read from the CPU, and a command buffer that copies it into its swapchain image */
  VkDevice dx = dev->present.device;
  const VkBufferCreateInfo dbci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, sc->bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, NULL};
  if ((r = dx_vkCreateBuffer(dx, &dbci, NULL, &im->dx_buffer)) != VK_SUCCESS) return r;
  dx_vkGetBufferMemoryRequirements(dx, im->dx_buffer, &req);
  VkPhysicalDeviceMemoryProperties dx_memory;
  dx_vkGetPhysicalDeviceMemoryProperties(dev->instance->present.physical, &dx_memory);
  type = UINT32_MAX;
  for (uint32_t i = 0; i < dx_memory.memoryTypeCount && type == UINT32_MAX; ++i)
    if (((req.memoryTypeBits >> i) & 1) && (dx_memory.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) type = i;
  if (type == UINT32_MAX) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
  mai.allocationSize = req.size, mai.memoryTypeIndex = type;
  if ((r = dx_vkAllocateMemory(dx, &mai, NULL, &im->dx_memory)) != VK_SUCCESS) return r;
  if ((r = dx_vkBindBufferMemory(dx, im->dx_buffer, im->dx_memory, 0)) != VK_SUCCESS) return r;
  if ((r = dx_vkMapMemory(dx, im->dx_memory, 0, VK_WHOLE_SIZE, 0, &pixels)) != VK_SUCCESS) return r;
  im->dx_pixels = pixels;
  const VkCommandBufferAllocateInfo cai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, dev->present.pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
  if ((r = dx_vkAllocateCommandBuffers(dx, &cai, &im->dx_copy)) != VK_SUCCESS) return r;
  const VkCommandBufferBeginInfo begin = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL, 0, NULL};
  dx_vkBeginCommandBuffer(im->dx_copy, &begin);
  const VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  VkImageMemoryBarrier to_dst = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, NULL, 0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, dx_image, range};
  dx_vkCmdPipelineBarrier(im->dx_copy, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &to_dst);
  const VkBufferImageCopy region = {0, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {sc->extent.width, sc->extent.height, 1}};
  dx_vkCmdCopyBufferToImage(im->dx_copy, im->dx_buffer, dx_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
  VkImageMemoryBarrier to_present = to_dst;
  to_present.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, to_present.dstAccessMask = 0;
  to_present.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, to_present.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
  dx_vkCmdPipelineBarrier(im->dx_copy, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, NULL, 0, NULL, 1, &to_present);
  return dx_vkEndCommandBuffer(im->dx_copy);
}

static VkResult VKAPI_CALL vn_vkCreateSwapchainKHR(VkDevice device, const VkSwapchainCreateInfoKHR *ci, const VkAllocationCallbacks *alloc, VkSwapchainKHR *out) {
  (void)alloc;
  VnDevice *dev = vn_device(device);
  if (!vn_device_has(dev, VK_KHR_SWAPCHAIN_EXTENSION_NAME)) return VK_ERROR_EXTENSION_NOT_PRESENT;
  if (!vn_present_device(dev)) return VK_ERROR_SURFACE_LOST_KHR;
  if (ci->imageFormat != VK_FORMAT_B8G8R8A8_UNORM && ci->imageFormat != VK_FORMAT_R8G8B8A8_UNORM && ci->imageFormat != VK_FORMAT_B8G8R8A8_SRGB &&
      ci->imageFormat != VK_FORMAT_R8G8B8A8_SRGB)
    return VK_ERROR_FORMAT_NOT_SUPPORTED;
  VnSwapchain *sc = vk_alloc(sizeof *sc);
  if (!sc) return VK_ERROR_OUT_OF_HOST_MEMORY;
  sc->device = dev, sc->extent = ci->imageExtent, sc->bytes = (VkDeviceSize)ci->imageExtent.width * ci->imageExtent.height * 4;
  sc->pool_family = UINT32_MAX;
  /* the Direct3D swapchain: same surface, size, mode; the unorm format of the same layout (the bytes are copied as they are) */
  VkSwapchainCreateInfoKHR dci = *ci;
  dci.pNext = NULL;
  dci.imageFormat = (ci->imageFormat == VK_FORMAT_R8G8B8A8_UNORM || ci->imageFormat == VK_FORMAT_R8G8B8A8_SRGB) ? VK_FORMAT_R8G8B8A8_UNORM : VK_FORMAT_B8G8R8A8_UNORM;
  dci.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
  dci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  dci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE, dci.queueFamilyIndexCount = 0, dci.pQueueFamilyIndices = NULL;
  dci.oldSwapchain = ci->oldSwapchain ? ((VnSwapchain *)(uintptr_t)ci->oldSwapchain)->dx : VK_NULL_HANDLE;
  VkResult r = dx_vkCreateSwapchainKHR(dev->present.device, &dci, NULL, &sc->dx);
  if (r != VK_SUCCESS) {
    vk_free(sc);
    return r;
  }
  uint32_t n = 0;
  dx_vkGetSwapchainImagesKHR(dev->present.device, sc->dx, &n, NULL);
  VkImage dx_images[16];
  if (n == 0 || n > 16) r = VK_ERROR_INITIALIZATION_FAILED;
  if (r == VK_SUCCESS) r = dx_vkGetSwapchainImagesKHR(dev->present.device, sc->dx, &n, dx_images);
  sc->images = r == VK_SUCCESS ? vk_alloc(sizeof *sc->images * n) : NULL;
  if (r == VK_SUCCESS && !sc->images) r = VK_ERROR_OUT_OF_HOST_MEMORY;
  if (r == VK_SUCCESS) {
    sc->count = n;
    for (uint32_t i = 0; i < n && r == VK_SUCCESS; ++i) r = vn_swap_image_make(sc, &sc->images[i], ci, dx_images[i]);
  }
  if (r == VK_SUCCESS) {
    const VkFenceCreateInfo fci = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, NULL, 0};
    r = vn_vkCreateFence(device, &fci, NULL, &sc->fence);
  }
  if (r != VK_SUCCESS) {
    vn_vkDestroySwapchainKHR(device, (VkSwapchainKHR)(uintptr_t)sc, NULL);
    return r;
  }
  *out = (VkSwapchainKHR)(uintptr_t)sc;
  return VK_SUCCESS;
}

static VkResult VKAPI_CALL vn_vkGetSwapchainImagesKHR(VkDevice device, VkSwapchainKHR swapchain, uint32_t *count, VkImage *out) {
  (void)device;
  VnSwapchain *sc = (VnSwapchain *)(uintptr_t)swapchain;
  if (!out) {
    *count = sc->count;
    return VK_SUCCESS;
  }
  const uint32_t k = *count < sc->count ? *count : sc->count;
  for (uint32_t i = 0; i < k; ++i) out[i] = sc->images[i].image;
  *count = k;
  return k < sc->count ? VK_INCOMPLETE : VK_SUCCESS;
}

/* Signals a semaphore and/or a fence of the host now (an empty submission on the device's first queue: the host signals them once it runs). */
static VkResult vn_signal_now(VnDevice *dev, VkSemaphore semaphore, VkFence fence) {
  if (!semaphore && !fence) return VK_SUCCESS;
  if (!dev->queue_count) return VK_ERROR_DEVICE_LOST;
  const VkSubmitInfo submit = {VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, 0, NULL, NULL, 0, NULL, semaphore ? 1u : 0u, semaphore ? &semaphore : NULL};
  return vn_call_vkQueueSubmit(&dev->instance->ring, (VkQueue)&dev->queues[0], semaphore ? 1 : 0, semaphore ? &submit : NULL, fence);
}

static VkResult VKAPI_CALL vn_vkAcquireNextImageKHR(VkDevice device, VkSwapchainKHR swapchain, uint64_t timeout, VkSemaphore semaphore, VkFence fence, uint32_t *index) {
  VnDevice *dev = vn_device(device);
  VnSwapchain *sc = (VnSwapchain *)(uintptr_t)swapchain;
  const VkResult r = dx_vkAcquireNextImageKHR(dev->present.device, sc->dx, timeout, VK_NULL_HANDLE, VK_NULL_HANDLE, index);
  if (r < 0 || r == VK_TIMEOUT || r == VK_NOT_READY) return r;
  const VkResult s = vn_signal_now(dev, semaphore, fence);
  return s != VK_SUCCESS ? s : r;
}

static VkResult VKAPI_CALL vn_vkAcquireNextImage2KHR(VkDevice device, const VkAcquireNextImageInfoKHR *info, uint32_t *index) {
  return vn_vkAcquireNextImageKHR(device, info->swapchain, info->timeout, info->semaphore, info->fence, index);
}

/* The copy of image `i` into its buffer, for queue family `family`: from PRESENT_SRC to TRANSFER_SRC and back. */
static VkResult vn_swap_copy_record(VnSwapchain *sc, VnSwapImage *im, uint32_t family) {
  VkDevice device = (VkDevice)sc->device;
  if (sc->pool_family != family) {
    if (sc->pool) {
      vn_vkDestroyCommandPool(device, sc->pool, NULL);
      for (uint32_t i = 0; i < sc->count; ++i) sc->images[i].copy = VK_NULL_HANDLE;
      sc->pool = VK_NULL_HANDLE;
    }
    const VkCommandPoolCreateInfo pci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL, 0, family};
    const VkResult r = vn_vkCreateCommandPool(device, &pci, NULL, &sc->pool);
    if (r != VK_SUCCESS) return r;
    sc->pool_family = family;
  }
  if (im->copy) return VK_SUCCESS;
  const VkCommandBufferAllocateInfo cai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, sc->pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
  VkResult r = vn_vkAllocateCommandBuffers(device, &cai, &im->copy);
  if (r != VK_SUCCESS) return r;
  const VkCommandBufferBeginInfo begin = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL, 0, NULL};
  vn_vkBeginCommandBuffer(im->copy, &begin);
  const VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  VkImageMemoryBarrier to_src = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, NULL, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                                 VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, im->image, range};
  vn_vkCmdPipelineBarrier(im->copy, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &to_src);
  const VkBufferImageCopy region = {0, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {sc->extent.width, sc->extent.height, 1}};
  vn_vkCmdCopyImageToBuffer(im->copy, im->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, im->buffer, 1, &region);
  VkImageMemoryBarrier back = to_src;
  back.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT, back.dstAccessMask = 0;
  back.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, back.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
  const VkBufferMemoryBarrier host = {VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER, NULL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT, VK_QUEUE_FAMILY_IGNORED,
                                      VK_QUEUE_FAMILY_IGNORED, im->buffer, 0, VK_WHOLE_SIZE};
  vn_vkCmdPipelineBarrier(im->copy, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT | VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, NULL, 1, &host, 1, &back);
  return vn_vkEndCommandBuffer(im->copy);
}

static VkResult VKAPI_CALL vn_vkQueuePresentKHR(VkQueue queue, const VkPresentInfoKHR *info) {
  VnQueue *q = vn_queue(queue);
  VkDevice device = (VkDevice)q->device;
  struct vn_ring *ring = vn_ring_of_queue(queue);
  VkResult result = VK_SUCCESS;
  for (uint32_t i = 0; i < info->swapchainCount; ++i) {
    VnSwapchain *sc = (VnSwapchain *)(uintptr_t)info->pSwapchains[i];
    const uint32_t index = info->pImageIndices[i];
    VkResult r = index < sc->count ? vn_swap_copy_record(sc, &sc->images[index], q->family) : VK_ERROR_OUT_OF_DATE_KHR;
    VnSwapImage *im = index < sc->count ? &sc->images[index] : NULL;
    if (r == VK_SUCCESS) {
      /* the host copies the frame once what the program submitted for it is done (the wait semaphores go with the first swapchain's copy) */
      const VkPipelineStageFlags stages[16] = {VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                               VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                               VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                               VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                               VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                               VK_PIPELINE_STAGE_ALL_COMMANDS_BIT};
      const uint32_t waits = i == 0 ? info->waitSemaphoreCount : 0;
      if (waits > 16) r = VK_ERROR_OUT_OF_HOST_MEMORY;
      const VkSubmitInfo submit = {VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, waits, waits ? info->pWaitSemaphores : NULL, waits ? stages : NULL, 1, &im->copy, 0, NULL};
      if (r == VK_SUCCESS) r = vn_call_vkQueueSubmit(ring, queue, 1, &submit, sc->fence);
      if (r == VK_SUCCESS) r = vn_vkWaitForFences(device, 1, &sc->fence, VK_TRUE, UINT64_MAX);
      if (r == VK_SUCCESS) r = vn_call_vkResetFences(ring, device, 1, &sc->fence);
    }
    if (r == VK_SUCCESS) {
      /* through the guest into the Direct3D swapchain's image, and shown there */
      memcpy(im->dx_pixels, im->pixels, (size_t)sc->bytes);
      VnPresentDevice *p = &q->device->present;
      const VkSubmitInfo dsubmit = {VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, 0, NULL, NULL, 1, &im->dx_copy, 0, NULL};
      r = dx_vkQueueSubmit(p->queue, 1, &dsubmit, VK_NULL_HANDLE);
      if (r == VK_SUCCESS) {
        const VkPresentInfoKHR dpresent = {VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, NULL, 0, NULL, 1, &sc->dx, &index, NULL};
        r = dx_vkQueuePresentKHR(p->queue, &dpresent);
      }
    }
    if (info->pResults) info->pResults[i] = r;
    if (r != VK_SUCCESS && (result == VK_SUCCESS || r < 0)) result = r;
  }
  return result;
}

#endif /* VN_NO_PRESENT */

/* ---- the entry points by name ---------------------------------------------------------------------------------------------------------------------------- */
typedef struct VnProc {
  const char *name;
  PFN_vkVoidFunction fn;
  uint32_t version;        /* the core version that has it (major << 8 | minor), 0 for an extension's */
  const char *extension;   /* the extension it needs (NULL: core) */
  uint8_t device_level;    /* its first parameter is a device, queue or command buffer */
} VnProc;
