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

/* A Linux program (no C library; x86-64, AArch64 and RISC-V from the same source) that renders with Vulkan (libvulkan.a over /dev/dxg) and shows its frames
 * through the kernel's Wayland compositor as GPU buffers: the image it renders into is exported as a dmabuf (VK_KHR_external_memory_fd,
 * VK_EXT_external_memory_dma_buf: /dev/dxg's TCOS_DXG_IOCTL_EXPORT), passed with sendmsg(SCM_RIGHTS) in zwp_linux_buffer_params_v1.add and made a wl_buffer
 * (create_immed) that a decorated toplevel shows. Each frame is rendered on the GPU into the same image and committed: no pixel goes through the client's
 * CPU, and the desktop draws it where it is. Also: the dmabuf's size by lseek, an import of it into a second image that sees the same pixels, the ioctls'
 * refusals. Prints "wayland-vulkan: N checks, M failed"; stays a few seconds when given an argument; exits 1 when something failed. tests/wayland_dmabuf_emulated.sh runs it on the CPU emulator. */
#include "../../src/Vulkan/khronos/vulkan/vulkan_core.h"
#include "../../src/Process/DxgDevice.h"
#include "../windows/vulkantest_shaders.h"

#if defined(__x86_64__)
static long sys6(long n, long a, long b, long c, long d, long e, long f) {
  register long r10 __asm__("r10") = d, r8 __asm__("r8") = e, r9 __asm__("r9") = f;
  long r;
  __asm__ volatile("syscall" : "=a"(r) : "a"(n), "D"(a), "S"(b), "d"(c), "r"(r10), "r"(r8), "r"(r9) : "rcx", "r11", "memory");
  return r;
}
enum { NR_READ = 0, NR_WRITE = 1, NR_CLOSE = 3, NR_LSEEK = 8, NR_IOCTL = 16, NR_DUP = 32, NR_SOCKET = 41, NR_CONNECT = 42, NR_SENDMSG = 46, NR_RECVMSG = 47,
       NR_OPENAT = 257, NR_EXIT_GROUP = 231, NR_MEMFD_CREATE = 319, NR_NANOSLEEP = 35, NR_MMAP = 9, NR_MUNMAP = 11, NR_FSTAT = 5 };
__asm__(".globl _start\n_start:\n  xor %ebp, %ebp\n  mov %rsp, %rdi\n  and $-16, %rsp\n  call start_c\n");
#elif defined(__aarch64__)
static long sys6(long n, long a, long b, long c, long d, long e, long f) {
  register long x8 __asm__("x8") = n, x0 __asm__("x0") = a, x1 __asm__("x1") = b, x2 __asm__("x2") = c, x3 __asm__("x3") = d, x4 __asm__("x4") = e,
                x5 __asm__("x5") = f;
  __asm__ volatile("svc #0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x5) : "memory");
  return x0;
}
enum { NR_READ = 63, NR_WRITE = 64, NR_CLOSE = 57, NR_LSEEK = 62, NR_IOCTL = 29, NR_DUP = 23, NR_SOCKET = 198, NR_CONNECT = 203, NR_SENDMSG = 211,
       NR_RECVMSG = 212, NR_OPENAT = 56, NR_EXIT_GROUP = 94, NR_MEMFD_CREATE = 279, NR_NANOSLEEP = 101, NR_MMAP = 222, NR_MUNMAP = 215, NR_FSTAT = 80 };
__asm__(".globl _start\n_start:\n  mov x29, #0\n  mov x0, sp\n  mov x9, sp\n  and x9, x9, #-16\n  mov sp, x9\n  bl start_c\n");
#elif defined(__riscv)
static long sys6(long n, long a, long b, long c, long d, long e, long f) {
  register long a7 __asm__("a7") = n, a0 __asm__("a0") = a, a1 __asm__("a1") = b, a2 __asm__("a2") = c, a3 __asm__("a3") = d, a4 __asm__("a4") = e,
                a5 __asm__("a5") = f;
  __asm__ volatile("ecall" : "+r"(a0) : "r"(a7), "r"(a1), "r"(a2), "r"(a3), "r"(a4), "r"(a5) : "memory");
  return a0;
}
enum { NR_READ = 63, NR_WRITE = 64, NR_CLOSE = 57, NR_LSEEK = 62, NR_IOCTL = 29, NR_DUP = 23, NR_SOCKET = 198, NR_CONNECT = 203, NR_SENDMSG = 211,
       NR_RECVMSG = 212, NR_OPENAT = 56, NR_EXIT_GROUP = 94, NR_MEMFD_CREATE = 279, NR_NANOSLEEP = 101, NR_MMAP = 222, NR_MUNMAP = 215, NR_FSTAT = 80 };
__asm__(".globl _start\n_start:\n  li s0, 0\n  mv a0, sp\n  andi sp, sp, -16\n  call start_c\n");
#else
#error "no runtime for this CPU"
#endif
#define sys3(n, a, b, c) sys6(n, (long)(a), (long)(b), (long)(c), 0, 0, 0)

typedef unsigned char u8;
typedef unsigned int u32;
typedef unsigned long u64;

/* (the compiler may call these; libvulkan's weak ones are byte loops too) */
void *memset(void *d, int c, unsigned long n) {
  volatile u8 *p = d;
  for (unsigned long i = 0; i < n; ++i) p[i] = (u8)c;
  return d;
}

static unsigned checks, failed;
static unsigned long len(const char *s) { unsigned long n = 0; while (s[n]) ++n; return n; }
static void out(const char *s) { sys3(NR_WRITE, 1, s, len(s)); }
static void out_u(unsigned long v) {
  char b[24];
  int i = 23;
  b[i] = 0;
  do b[--i] = (char)('0' + v % 10), v /= 10;
  while (v);
  out(b + i);
}
static void out_x(unsigned long v) {
  char b[24];
  int i = 23;
  b[i] = 0;
  do b[--i] = "0123456789abcdef"[v & 15], v >>= 4;
  while (v);
  out(b + i);
}
#define CHECK(c) do { ++checks; if (!(c)) { ++failed; out("FAIL line "); out_u(__LINE__); out(": " #c "\n"); } } while (0)
static int same(const char *a, const char *b) { while (*a && *a == *b) ++a, ++b; return *a == *b; }
static void copy(void *d, const void *s, unsigned long n) { u8 *q = d; const u8 *p = s; for (unsigned long i = 0; i < n; ++i) q[i] = p[i]; }
static void zero(void *p, unsigned long n) { memset(p, 0, n); }

/* ---- the wire (as tests/linux/waylandtest.c) */
static long sock = -1;
static u8 msg[1024];
static u32 msg_len;
static int pending_fd = -1;
static void begin(u32 object, u32 opcode) { msg_len = 8; copy(msg, &object, 4); copy(msg + 4, &opcode, 4); }
static void put(u32 v) { copy(msg + msg_len, &v, 4); msg_len += 4; }
static void put_s(const char *s) {
  const u32 n = (u32)len(s) + 1;
  put(n);
  copy(msg + msg_len, s, n);
  msg_len += n;
  while (msg_len % 4) msg[msg_len++] = 0;
}
static void end(void) {
  const u32 word = msg_len << 16 | (u32)(msg[4] | msg[5] << 8);
  copy(msg + 4, &word, 4);
  long iov[2] = {(long)msg, (long)msg_len};
  long control[3] = {0, 0, 0};
  long hdr[7] = {0, 0, (long)iov, 1, 0, 0, 0};
  if (pending_fd >= 0) {
    control[0] = 16 + 4;
    control[1] = 1L | (1L << 32);      /* SOL_SOCKET, SCM_RIGHTS */
    copy(&control[2], &pending_fd, 4);
    hdr[4] = (long)control;
    hdr[5] = 24;
  }
  if (sys3(NR_SENDMSG, sock, hdr, 0) != (long)msg_len) { ++failed; out("FAIL: sendmsg\n"); }
  pending_fd = -1;
}
static int table_fd = -1;
static u8 in[65536];
static u32 in_len;
static int fill(void) {
  long iov[2] = {(long)(in + in_len), (long)(sizeof in - in_len)};
  long control[8] = {0};
  long hdr[7] = {0, 0, (long)iov, 1, (long)control, sizeof control, 0};
  const long got = sys3(NR_RECVMSG, sock, hdr, 0);
  if (got <= 0) return 0;
  in_len += (u32)got;
  if ((u64)control[0] > 16 && (control[1] & 0xFFFFFFFF) == 1 && (control[1] >> 32) == 1) {      /* (the feedback's format table; anything else is closed) */
    int fd;
    copy(&fd, &control[2], 4);
    if (table_fd < 0) table_fd = fd;
    else sys3(NR_CLOSE, fd, 0, 0);
  }
  return 1;
}
typedef struct { u32 object, opcode, size; const u8 *args; } Event;
static Event ev;
static u8 ev_copy[4096];
static int next_event(void) {
  while (in_len < 8 || in_len < (u32)(in[6] | in[7] << 8)) if (!fill()) return 0;
  ev.object = (u32)(in[0] | in[1] << 8 | in[2] << 16 | (u32)in[3] << 24);
  ev.opcode = (u32)(in[4] | in[5] << 8);
  ev.size = (u32)(in[6] | in[7] << 8);
  copy(ev_copy, in + 8, ev.size - 8);
  ev.args = ev_copy;
  for (u32 i = ev.size; i < in_len; ++i) in[i - ev.size] = in[i];
  in_len -= ev.size;
  return 1;
}
static u32 arg(u32 word) { u32 v; copy(&v, ev.args + 4 * word, 4); return v; }
static unsigned modifiers_seen, linear_seen, released;
static int wait_event(u32 object, u32 opcode) {
  for (;;) {
    if (!next_event()) return 0;
    if (ev.object == 1 && ev.opcode == 0) { out("FAIL: protocol error "); out_u(arg(1)); out(" on "); out_u(arg(0)); out("\n"); ++failed; return 0; }
    if (ev.object == 16 && ev.opcode == 1) { ++modifiers_seen; linear_seen += arg(1) == 0 && arg(2) == 0; }
    if (ev.object == 40 && ev.opcode == 0) ++released;
    if (ev.object == object && ev.opcode == opcode) return 1;
  }
}
/* A D3DKMT service through /dev/dxg (its NTSTATUS; -1 when the ioctl failed). */
static long dxg_fd = -1;
static int kmt_call(u32 service, void *params) {
  TcosDxgCall call = {service, 0, (unsigned long)params};
  if (sys3(NR_IOCTL, dxg_fd, TCOS_DXG_IOCTL_CALL, &call) != 0) return -1;
  return call.status;
}
static void put32(u8 *b, u32 at, u32 v) { copy(b + at, &v, 4); }
static void put64(u8 *b, u32 at, u64 v) { copy(b + at, &v, 8); }
static u32 get32(const u8 *b, u32 at) { u32 v; copy(&v, b + at, 4); return v; }
static u32 make_fence(u32 device) {      /* D3DKMTCreateSynchronizationObject2, a fence starting at 0 */
  u8 a[96];
  zero(a, sizeof a);
  put32(a, 0, device);
  put32(a, 8, 3);
  return kmt_call(TCOS_DXG_CREATE_SYNCHRONIZATION_OBJECT2, a) == 0 ? get32(a, 88) : 0;
}
static int signal_fence(u32 device, u32 fence, u64 value) {      /* D3DKMTSignalSynchronizationObjectFromCpu */
  u8 a[32];
  zero(a, sizeof a);
  put32(a, 0, device);
  put32(a, 4, 1);
  put64(a, 8, (u64)&fence);
  put64(a, 16, (u64)&value);
  return kmt_call(TCOS_DXG_SIGNAL_SYNCHRONIZATION_OBJECT_FROM_CPU, a);
}
static int wait_fence(u32 device, u32 fence, u64 value) {      /* D3DKMTWaitForSynchronizationObjectFromCpu (no event: blocks) */
  u8 a[48];
  zero(a, sizeof a);
  put32(a, 0, device);
  put32(a, 4, 1);
  put64(a, 8, (u64)&fence);
  put64(a, 16, (u64)&value);
  return kmt_call(TCOS_DXG_WAIT_FOR_SYNCHRONIZATION_OBJECT_FROM_CPU, a);
}
static int export_fence(u32 fence) {
  TcosDxgExport ex = {fence, TCOS_DXG_EXPORT_CLOEXEC, -1, 0};
  return sys3(NR_IOCTL, dxg_fd, TCOS_DXG_IOCTL_EXPORT_SYNC, &ex) == 0 && ex.status == 0 ? ex.fd : -1;
}
static void point(u32 so, u32 opcode, u32 timeline, u64 value) { begin(so, opcode); put(timeline); put((u32)(value >> 32)); put((u32)value); end(); }

static const char *env_value(char **envp, const char *name) {
  for (; *envp; ++envp) {
    const char *e = *envp, *n = name;
    while (*n && *e == *n) ++e, ++n;
    if (!*n && *e == '=') return e + 1;
  }
  return 0;
}

/* ---- Vulkan */
enum { W = 64, H = 48 };
typedef struct {
  VkDevice dev;
  VkQueue queue;
  VkCommandPool pool;
  VkCommandBuffer cb;
  VkFence fence;
  VkPhysicalDeviceMemoryProperties mem;
} Rig;
static Rig r;

static VkDeviceMemory allocate(VkMemoryRequirements req, VkMemoryPropertyFlags want, const void *next) {
  unsigned type = 0;
  for (unsigned i = 0; i < r.mem.memoryTypeCount; ++i)
    if ((req.memoryTypeBits & (1u << i)) && (r.mem.memoryTypes[i].propertyFlags & want) == want) { type = i; break; }
  VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  ai.pNext = next, ai.allocationSize = req.size, ai.memoryTypeIndex = type;
  VkDeviceMemory m = VK_NULL_HANDLE;
  return vkAllocateMemory(r.dev, &ai, 0, &m) == VK_SUCCESS ? m : VK_NULL_HANDLE;
}
/* A 64 x 48 B8G8R8A8 image the GPU draws into, exportable (external) or not; its memory dedicated to it (and exportable, or imported from `import_fd`). */
static VkImage make_image(int external, int import_fd, VkDeviceMemory *memory) {
  VkExternalMemoryImageCreateInfo ext = {VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
  ext.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
  VkImageCreateInfo ic = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  ic.pNext = external ? &ext : 0;
  ic.imageType = VK_IMAGE_TYPE_2D, ic.format = VK_FORMAT_B8G8R8A8_UNORM, ic.extent.width = W, ic.extent.height = H, ic.extent.depth = 1, ic.mipLevels = 1;
  ic.arrayLayers = 1, ic.samples = VK_SAMPLE_COUNT_1_BIT, ic.tiling = VK_IMAGE_TILING_OPTIMAL;
  ic.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
  VkImage image = VK_NULL_HANDLE;
  if (vkCreateImage(r.dev, &ic, 0, &image) != VK_SUCCESS) return VK_NULL_HANDLE;
  VkMemoryRequirements req;
  vkGetImageMemoryRequirements(r.dev, image, &req);
  VkMemoryDedicatedAllocateInfo dedicated = {VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
  dedicated.image = image;
  VkExportMemoryAllocateInfo exported = {VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO};
  exported.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
  VkImportMemoryFdInfoKHR imported = {VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR};
  imported.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, imported.fd = import_fd;
  if (import_fd >= 0) dedicated.pNext = &imported;
  else if (external) dedicated.pNext = &exported;
  *memory = allocate(req, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &dedicated);
  if (!*memory || vkBindImageMemory(r.dev, image, *memory, 0) != VK_SUCCESS) return VK_NULL_HANDLE;
  return image;
}

static VkRenderPass pass;
static VkPipelineLayout layout;
static VkPipeline pipeline;
static VkBuffer vertices, readback;
static VkDeviceMemory vertex_memory, readback_memory;
static void *vertex_map, *readback_map;

static VkBuffer make_buffer(VkDeviceSize size, VkBufferUsageFlags usage, VkDeviceMemory *memory, void **mapped) {
  VkBufferCreateInfo ci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  ci.size = size, ci.usage = usage;
  VkBuffer b = VK_NULL_HANDLE;
  CHECK(vkCreateBuffer(r.dev, &ci, 0, &b) == VK_SUCCESS);
  VkMemoryRequirements req;
  vkGetBufferMemoryRequirements(r.dev, b, &req);
  *memory = allocate(req, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 0);
  CHECK(*memory && vkBindBufferMemory(r.dev, b, *memory, 0) == VK_SUCCESS && vkMapMemory(r.dev, *memory, 0, VK_WHOLE_SIZE, 0, mapped) == VK_SUCCESS);
  return b;
}
static VkShaderModule make_shader(const unsigned *code, unsigned long bytes) {
  VkShaderModuleCreateInfo ci = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
  ci.codeSize = bytes, ci.pCode = code;
  VkShaderModule m = VK_NULL_HANDLE;
  CHECK(vkCreateShaderModule(r.dev, &ci, 0, &m) == VK_SUCCESS);
  return m;
}
/* The render pass and the pipeline of a frame: a clear, then a green triangle moved by a push constant. */
static void make_pipeline(void) {
  VkAttachmentDescription att = {0};
  att.format = VK_FORMAT_B8G8R8A8_UNORM, att.samples = VK_SAMPLE_COUNT_1_BIT, att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE, att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE, att.finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
  VkAttachmentReference ref = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
  VkSubpassDescription sub = {0};
  sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS, sub.colorAttachmentCount = 1, sub.pColorAttachments = &ref;
  VkRenderPassCreateInfo rp = {VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
  rp.attachmentCount = 1, rp.pAttachments = &att, rp.subpassCount = 1, rp.pSubpasses = &sub;
  CHECK(vkCreateRenderPass(r.dev, &rp, 0, &pass) == VK_SUCCESS);
  VkPushConstantRange range = {VK_SHADER_STAGE_VERTEX_BIT, 0, 20};
  VkPipelineLayoutCreateInfo lc = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  lc.pushConstantRangeCount = 1, lc.pPushConstantRanges = &range;
  CHECK(vkCreatePipelineLayout(r.dev, &lc, 0, &layout) == VK_SUCCESS);
  VkShaderModule vs = make_shader(push_vert, sizeof push_vert), fs = make_shader(flat_frag, sizeof flat_frag);
  VkPipelineShaderStageCreateInfo stages[2] = {{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}, {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}};
  stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT, stages[0].module = vs, stages[0].pName = "main";
  stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT, stages[1].module = fs, stages[1].pName = "main";
  VkVertexInputBindingDescription binding = {0, 24, VK_VERTEX_INPUT_RATE_VERTEX};
  VkVertexInputAttributeDescription attrs[2] = {{0, 0, VK_FORMAT_R32G32_SFLOAT, 0}, {1, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 8}};
  VkPipelineVertexInputStateCreateInfo vi = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
  vi.vertexBindingDescriptionCount = 1, vi.pVertexBindingDescriptions = &binding, vi.vertexAttributeDescriptionCount = 2, vi.pVertexAttributeDescriptions = attrs;
  VkPipelineInputAssemblyStateCreateInfo ia = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
  ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
  VkViewport viewport = {0, 0, W, H, 0, 1};
  VkRect2D scissor = {{0, 0}, {W, H}};
  VkPipelineViewportStateCreateInfo vp = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
  vp.viewportCount = 1, vp.pViewports = &viewport, vp.scissorCount = 1, vp.pScissors = &scissor;
  VkPipelineRasterizationStateCreateInfo rs = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
  rs.polygonMode = VK_POLYGON_MODE_FILL, rs.cullMode = VK_CULL_MODE_NONE, rs.lineWidth = 1.0f;
  VkPipelineMultisampleStateCreateInfo ms = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
  ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
  VkPipelineColorBlendAttachmentState ba = {0};
  ba.colorWriteMask = 15;
  VkPipelineColorBlendStateCreateInfo cbs = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
  cbs.attachmentCount = 1, cbs.pAttachments = &ba;
  VkGraphicsPipelineCreateInfo gp = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
  gp.stageCount = 2, gp.pStages = stages, gp.pVertexInputState = &vi, gp.pInputAssemblyState = &ia, gp.pViewportState = &vp, gp.pRasterizationState = &rs, gp.pMultisampleState = &ms;
  gp.pColorBlendState = &cbs, gp.layout = layout, gp.renderPass = pass;
  CHECK(vkCreateGraphicsPipelines(r.dev, VK_NULL_HANDLE, 1, &gp, 0, &pipeline) == VK_SUCCESS);
  static const float tri[3][6] = {{-0.5f, -0.5f, 0, 1, 0, 1}, {0.5f, -0.5f, 0, 1, 0, 1}, {0.0f, 0.5f, 0, 1, 0, 1}};
  vertices = make_buffer(sizeof tri, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, &vertex_memory, &vertex_map);
  if (vertex_map) copy(vertex_map, tri, sizeof tri);
  readback = make_buffer(W * H * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &readback_memory, &readback_map);
}

static void submit_and_wait(void) {
  CHECK(vkEndCommandBuffer(r.cb) == VK_SUCCESS);
  VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
  si.commandBufferCount = 1, si.pCommandBuffers = &r.cb;
  vkResetFences(r.dev, 1, &r.fence);
  CHECK(vkQueueSubmit(r.queue, 1, &si, r.fence) == VK_SUCCESS && vkWaitForFences(r.dev, 1, &r.fence, VK_TRUE, ~0ull) == VK_SUCCESS);
}
static void begin_commands(void) {
  vkResetCommandBuffer(r.cb, 0);
  VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  CHECK(vkBeginCommandBuffer(r.cb, &bi) == VK_SUCCESS);
}
/* One frame into `image` (through `view`'s framebuffer): cleared to (red, green, blue), the triangle moved right by `shift`. */
static void render(VkFramebuffer framebuffer, float red, float green, float blue, float shift) {
  begin_commands();
  VkClearValue clear;
  zero(&clear, sizeof clear);
  clear.color.float32[0] = red, clear.color.float32[1] = green, clear.color.float32[2] = blue, clear.color.float32[3] = 1.0f;
  VkRenderPassBeginInfo rb = {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
  rb.renderPass = pass, rb.framebuffer = framebuffer, rb.renderArea.extent.width = W, rb.renderArea.extent.height = H, rb.clearValueCount = 1, rb.pClearValues = &clear;
  vkCmdBeginRenderPass(r.cb, &rb, VK_SUBPASS_CONTENTS_INLINE);
  vkCmdBindPipeline(r.cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
  VkDeviceSize offset = 0;
  vkCmdBindVertexBuffers(r.cb, 0, 1, &vertices, &offset);
  const float push[5] = {shift, 0.0f, 0.0f, 0.0f, 0.5f};
  vkCmdPushConstants(r.cb, layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof push, push);
  vkCmdDraw(r.cb, 3, 1, 0, 0);
  vkCmdEndRenderPass(r.cb);
  submit_and_wait();
}
/* The pixel at (x, y) of an image, copied out by the GPU (B, G, R, A in memory: 0xAARRGGBB as a word). */
static u32 pixel_of(VkImage image, u32 x, u32 y) {
  begin_commands();
  VkBufferImageCopy c;
  zero(&c, sizeof c);
  c.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, c.imageSubresource.layerCount = 1, c.imageExtent.width = W, c.imageExtent.height = H, c.imageExtent.depth = 1;
  vkCmdCopyImageToBuffer(r.cb, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback, 1, &c);
  submit_and_wait();
  return readback_map ? ((const u32 *)readback_map)[y * W + x] : 0;
}
static VkFramebuffer framebuffer_of(VkImage image, VkImageView *view) {
  VkImageViewCreateInfo vc = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  vc.image = image, vc.viewType = VK_IMAGE_VIEW_TYPE_2D, vc.format = VK_FORMAT_B8G8R8A8_UNORM, vc.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  vc.subresourceRange.levelCount = 1, vc.subresourceRange.layerCount = 1;
  CHECK(vkCreateImageView(r.dev, &vc, 0, view) == VK_SUCCESS);
  VkFramebufferCreateInfo fc = {VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
  fc.renderPass = pass, fc.attachmentCount = 1, fc.pAttachments = view, fc.width = W, fc.height = H, fc.layers = 1;
  VkFramebuffer f = VK_NULL_HANDLE;
  CHECK(vkCreateFramebuffer(r.dev, &fc, 0, &f) == VK_SUCCESS);
  return f;
}
static int near(u32 got, u32 want) {
  for (int i = 0; i < 32; i += 8) {
    const int a = (int)((got >> i) & 255), b = (int)((want >> i) & 255);
    if (a - b > 2 || b - a > 2) return 0;
  }
  return 1;
}

static int setup_vulkan(void) {
  VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO};
  app.apiVersion = VK_API_VERSION_1_0;
  VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
  ici.pApplicationInfo = &app;
  VkInstance instance = VK_NULL_HANDLE;
  CHECK(vkCreateInstance(&ici, 0, &instance) == VK_SUCCESS);
  u32 n = 1;
  VkPhysicalDevice pd = VK_NULL_HANDLE;
  CHECK(vkEnumeratePhysicalDevices(instance, &n, &pd) >= 0 && pd);
  if (!pd) return 0;
  VkExtensionProperties props[16];
  u32 count = 16;
  CHECK(vkEnumerateDeviceExtensionProperties(pd, 0, &count, props) == VK_SUCCESS);
  int fd_ext = 0, dmabuf_ext = 0, memory_ext = 0;
  for (u32 i = 0; i < count; ++i) {
    fd_ext |= same(props[i].extensionName, VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME);
    dmabuf_ext |= same(props[i].extensionName, VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME);
    memory_ext |= same(props[i].extensionName, VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME);
  }
  CHECK(fd_ext && dmabuf_ext && memory_ext);
  vkGetPhysicalDeviceMemoryProperties(pd, &r.mem);
  const float priority = 1.0f;
  VkDeviceQueueCreateInfo q = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
  q.queueCount = 1, q.pQueuePriorities = &priority;
  const char *extensions[3] = {VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME, VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME, VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME};
  VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
  dci.queueCreateInfoCount = 1, dci.pQueueCreateInfos = &q, dci.enabledExtensionCount = 3, dci.ppEnabledExtensionNames = extensions;
  CHECK(vkCreateDevice(pd, &dci, 0, &r.dev) == VK_SUCCESS);
  if (!r.dev) return 0;
  vkGetDeviceQueue(r.dev, 0, 0, &r.queue);
  VkCommandPoolCreateInfo pc = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
  pc.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  CHECK(vkCreateCommandPool(r.dev, &pc, 0, &r.pool) == VK_SUCCESS);
  VkCommandBufferAllocateInfo ca = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
  ca.commandPool = r.pool, ca.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, ca.commandBufferCount = 1;
  CHECK(vkAllocateCommandBuffers(r.dev, &ca, &r.cb) == VK_SUCCESS);
  VkFenceCreateInfo fi = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
  CHECK(vkCreateFence(r.dev, &fi, 0, &r.fence) == VK_SUCCESS);
  return !failed;
}

__attribute__((used)) void start_c(long *stack) {
  const long argc = stack[0];
  char **envp = (char **)(stack + 1 + argc + 1);
  if (!setup_vulkan()) goto done;
  make_pipeline();

  /* the frame's image, exported as a dmabuf */
  VkDeviceMemory memory = VK_NULL_HANDLE;
  VkImage image = make_image(1, -1, &memory);
  CHECK(image != VK_NULL_HANDLE);
  if (!image) goto done;
  VkImageView view;
  VkFramebuffer framebuffer = framebuffer_of(image, &view);
  render(framebuffer, 1.0f, 0.5f, 0.0f, 0.25f);      /* orange, the triangle right of the middle */
  CHECK(near(pixel_of(image, 2, 2), 0xFFFF8000u));
  VkMemoryGetFdInfoKHR gi = {VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR};
  gi.memory = memory, gi.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
  int dmabuf = -1;
  CHECK(vkGetMemoryFdKHR(r.dev, &gi, &dmabuf) == VK_SUCCESS && dmabuf >= 0);
  if (dmabuf < 0) goto done;
  CHECK(sys3(NR_LSEEK, dmabuf, 0, 2) >= (long)(W * H * 4));      /* SEEK_END: its size, as a dmabuf's */
  u8 byte;
  CHECK(sys3(NR_READ, dmabuf, &byte, 1) < 0);                     /* (the pixels are not read through the descriptor) */
  /* the CPU's view: mmap maps a copy of the image; DMA_BUF_IOCTL_SYNC brings the GPU's pixels in (START|READ) and sends the CPU's out (END|WRITE) */
  {
    const unsigned long bytes = (W * H * 4 + 4095) / 4096 * 4096;
    const long m = sys6(NR_MMAP, 0, (long)bytes, 3, 1, dmabuf, 0);      /* PROT_READ | PROT_WRITE, MAP_SHARED */
    CHECK(m > 0 || m < -4096);
    if (m > 0 || m < -4096) {
      u32 *px = (u32 *)m;
      unsigned long flags = 1;      /* DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ */
      CHECK(sys3(NR_IOCTL, dmabuf, 0x40086200, &flags) == 0);
      CHECK(near(px[2 * W + 2], 0xFFFF8000u));
      px[1 * W + 1] = 0xFF0000FFu;
      flags = 4 | 2;                /* DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE */
      CHECK(sys3(NR_IOCTL, dmabuf, 0x40086200, &flags) == 0);
      CHECK(pixel_of(image, 1, 1) == 0xFF0000FFu);      /* (the GPU's image has the CPU's write) */
      flags = 0x10;
      CHECK(sys3(NR_IOCTL, dmabuf, 0x40086200, &flags) == -22);      /* EINVAL */
      CHECK(sys3(NR_MUNMAP, m, bytes, 0) == 0);
    }
  }
  /* an image that is not exportable is refused */
  VkDeviceMemory plain_memory;
  VkImage plain = make_image(0, -1, &plain_memory);
  gi.memory = plain_memory;
  int none = -1;
  CHECK(plain && vkGetMemoryFdKHR(r.dev, &gi, &none) != VK_SUCCESS && none == -1);
  /* an import of the dmabuf into a second image: the same pixels, without a copy */
  const long dup = sys3(NR_DUP, dmabuf, 0, 0);
  VkDeviceMemory imported_memory = VK_NULL_HANDLE;
  VkImage imported = make_image(1, (int)dup, &imported_memory);
  CHECK(imported != VK_NULL_HANDLE);
  if (imported) CHECK(near(pixel_of(imported, 2, 2), 0xFFFF8000u));
  /* /dev/dxg's refusals: a descriptor that is not a GPU image; a handle that is no allocation */
  long dxg = sys6(NR_OPENAT, -100, (long)TCOS_DXG_DEVICE_PATH, 2 | 0x80000, 0, 0, 0);
  CHECK(dxg >= 0);
  const long memfd = sys3(NR_MEMFD_CREATE, "not-gpu", 1, 0);
  TcosDxgImport im;
  zero(&im, sizeof im);
  im.fd = (int)memfd, im.device = 1;
  CHECK(sys3(NR_IOCTL, dxg, TCOS_DXG_IOCTL_IMPORT, &im) == -9);      /* EBADF */
  TcosDxgExport ex = {0x7777, 0, -1, 0};
  CHECK(sys3(NR_IOCTL, dxg, TCOS_DXG_IOCTL_EXPORT, &ex) == 0 && ex.status != 0 && ex.fd == -1);
  ex.flags = 0x80;
  CHECK(sys3(NR_IOCTL, dxg, TCOS_DXG_IOCTL_EXPORT, &ex) == -22);      /* EINVAL: unknown flags */
  CHECK(sys3(NR_IOCTL, memfd, TCOS_DXG_IOCTL_EXPORT, &ex) == -25);    /* ENOTTY: not /dev/dxg */
  sys3(NR_CLOSE, memfd, 0, 0);

  /* the compositor */
  const char *runtime = env_value(envp, "XDG_RUNTIME_DIR"), *display = env_value(envp, "WAYLAND_DISPLAY");
  u8 address[110] = {0};
  address[0] = 1;
  unsigned at = 2;
  for (const char *p = runtime ? runtime : "/run/user/0"; *p; ++p) address[at++] = (u8)*p;
  address[at++] = '/';
  for (const char *p = display ? display : "wayland-0"; *p; ++p) address[at++] = (u8)*p;
  sock = sys3(NR_SOCKET, 1, 1 | 0x80000, 0);
  CHECK(sock >= 0 && sys3(NR_CONNECT, sock, address, at + 1) == 0);
  if (failed) goto done;
  u32 g_compositor = 0, g_wm = 0, g_deco = 0, g_dmabuf = 0, dmabuf_version = 0, g_syncobj = 0;
  begin(1, 1); put(2); end();
  begin(1, 0); put(3); end();
  while (next_event() && !(ev.object == 3 && ev.opcode == 0)) {
    if (ev.object != 2 || ev.opcode != 0) continue;
    const char *name = (const char *)ev.args + 8;
    const u32 global = arg(0);
    if (same(name, "wl_compositor")) g_compositor = global;
    else if (same(name, "xdg_wm_base")) g_wm = global;
    else if (same(name, "zxdg_decoration_manager_v1")) g_deco = global;
    else if (same(name, "zwp_linux_dmabuf_v1")) g_dmabuf = global, dmabuf_version = arg(1 + (arg(1) + 3) / 4 + 1);
    else if (same(name, "wp_linux_drm_syncobj_manager_v1")) g_syncobj = global;
  }
  CHECK(g_compositor && g_wm && g_deco && g_dmabuf && dmabuf_version == 4);
  begin(2, 0); put(g_compositor); put_s("wl_compositor"); put(5); put(10); end();
  begin(2, 0); put(g_wm); put_s("xdg_wm_base"); put(5); put(12); end();
  begin(2, 0); put(g_deco); put_s("zxdg_decoration_manager_v1"); put(1); put(14); end();
  begin(2, 0); put(g_dmabuf); put_s("zwp_linux_dmabuf_v1"); put(4); put(16); end();
  /* version 4: the default feedback - the format table (a sealed file mapped privately, as Mesa does) and the device, which is /dev/dxg */
  begin(16, 2); put(18); end();
  CHECK(wait_event(18, 1));
  const u32 table_size = arg(0);
  CHECK(table_size == 64 && table_fd >= 0);
  if (table_fd >= 0) {
    const long t = sys6(NR_MMAP, 0, 4096, 1, 2, table_fd, 0);      /* PROT_READ, MAP_PRIVATE */
    CHECK(t > 0 || t < -4096);
    if (t > 0 || t < -4096) {
      const u32 *entry = (const u32 *)t;
      CHECK(entry[0] == 0x34325241 && entry[2] == 0 && entry[3] == 0 && entry[12] == 0x34324258);
      sys3(NR_MUNMAP, t, 4096, 0);
    }
    sys3(NR_CLOSE, table_fd, 0, 0);
  }
  CHECK(wait_event(18, 2));
  u64 main_device = 0;
  copy(&main_device, ev.args + 4, 8);
  u8 st[144];
  const long dxg_dev = sys6(NR_OPENAT, -100, (long)TCOS_DXG_DEVICE_PATH, 2 | 0x80000, 0, 0, 0);
  CHECK(dxg_dev >= 0 && sys3(NR_FSTAT, dxg_dev, st, 0) == 0);
  u64 rdev = 0;
  copy(&rdev, st + (sizeof(long) == 8 && NR_FSTAT == 5 ? 40 : 32), 8);      /* (st_rdev: 40 in x86-64's layout, 32 in the generic one) */
  CHECK(main_device == rdev && rdev == TCOS_DXG_DEVICE_NUMBER);
  sys3(NR_CLOSE, dxg_dev, 0, 0);
  CHECK(wait_event(18, 0));      /* done */
  begin(18, 0); end();
  CHECK(modifiers_seen == 0);      /* (version 4 sends no modifier events) */
  /* the buffer: plane 0 is the dmabuf (offset 0, its rows W * 4 bytes apart), linear */
  begin(16, 1); put(39); end();
  begin(39, 1); put(0); put(0); put(W * 4); put(0); put(0); pending_fd = dmabuf; end();
  begin(39, 3); put(40); put(W); put(H); put(0x34325258); put(0); end();      /* create_immed: XRGB8888 */
  begin(39, 0); end();
  sys3(NR_CLOSE, dmabuf, 0, 0);      /* (the compositor has its own) */
  /* a decorated toplevel showing it */
  begin(10, 0); put(30); end();
  begin(12, 2); put(31); put(30); end();
  begin(31, 1); put(32); end();
  begin(14, 1); put(33); put(32); end();
  begin(32, 2); put_s("Linux Vulkan dmabuf"); end();
  begin(30, 6); end();
  CHECK(wait_event(31, 0));
  begin(31, 4); put(arg(0)); end();
  begin(30, 1); put(40); put(0); put(0); end();
  begin(30, 2); put(0); put(0); put(W); put(H); end();
  begin(30, 3); put(41); end();
  begin(30, 6); end();
  CHECK(wait_event(41, 0));      /* drawn by the desktop */
  /* explicit sync: two fences of a device of our own, exported as timelines; a commit waits for its acquire point, the release point comes when its use ends */
  {
    dxg_fd = sys6(NR_OPENAT, -100, (long)TCOS_DXG_DEVICE_PATH, 2 | 0x80000, 0, 0, 0);
    u8 en[16], infos[20 * 4], dv[64];
    zero(en, sizeof en);
    put32(en, 0, 4);
    put64(en, 8, (u64)infos);
    CHECK(kmt_call(TCOS_DXG_ENUM_ADAPTERS2, en) == 0 && get32(en, 0) >= 1);
    zero(dv, sizeof dv);
    put32(dv, 0, get32(infos, 0));
    CHECK(kmt_call(TCOS_DXG_CREATE_DEVICE, dv) == 0);
    const u32 device = get32(dv, 12);
    const u32 fa = make_fence(device), fr = make_fence(device);
    CHECK(device && fa && fr);
    const int ta = export_fence(fa), tr = export_fence(fr);
    CHECK(ta >= 0 && tr >= 0);
    CHECK(export_fence(0x5555) == -1);      /* (no such sync object) */
    TcosDxgImportSync is = {ta, device, 0, 0};
    CHECK(sys3(NR_IOCTL, dxg_fd, TCOS_DXG_IOCTL_IMPORT_SYNC, &is) == 0 && is.status == 0 && is.sync != 0);      /* (the same fence, a second handle) */
    CHECK(g_syncobj != 0);
    begin(2, 0); put(g_syncobj); put_s("wp_linux_drm_syncobj_manager_v1"); put(1); put(19); end();
    begin(19, 2); put(70); pending_fd = ta; end();      /* import_timeline */
    begin(19, 2); put(71); pending_fd = tr; end();
    sys3(NR_CLOSE, ta, 0, 0);
    sys3(NR_CLOSE, tr, 0, 0);
    begin(19, 1); put(72); put(30); end();               /* get_surface */
    /* acquire point 1 not reached yet: the commit waits */
    point(72, 1, 70, 1);
    point(72, 2, 71, 1);
    begin(30, 1); put(40); put(0); put(0); end();
    begin(30, 2); put(0); put(0); put(W); put(H); end();
    begin(30, 6); end();
    begin(1, 0); put(73); end();
    CHECK(wait_event(73, 0));
    CHECK(signal_fence(device, is.sync, 1) == 0);      /* (through the imported handle: the same fence) */
    /* the same buffer again with acquire 2 (reached first): applied at once, and the first commit's release point comes */
    CHECK(signal_fence(device, fa, 2) == 0);
    point(72, 1, 70, 2);
    point(72, 2, 71, 3);
    begin(30, 1); put(40); put(0); put(0); end();      /* (points need a buffer attached in the same commit) */
    begin(30, 2); put(0); put(0); put(W); put(H); end();
    begin(30, 6); end();
    CHECK(wait_fence(device, fr, 1) == 0);      /* (blocks until the compositor signalled it) */
    begin(72, 0); end();      /* back to implicit sync for the frames below */
    begin(1, 0); put(74); end();
    CHECK(wait_event(74, 0));
  }
  /* frames rendered on the GPU into the same image, each committed and drawn */
  for (u32 frame = 0; frame < 6; ++frame) {
    render(framebuffer, 0.0f, 0.25f * (float)(frame % 4), 1.0f, -0.25f + 0.1f * (float)frame);
    begin(30, 2); put(0); put(0); put(W); put(H); end();
    begin(30, 3); put(50 + frame); end();
    begin(30, 6); end();
    CHECK(wait_event(50 + frame, 0));
  }
  render(framebuffer, 0.0f, 0.75f, 0.5f, 0.25f);      /* the last: teal (0xFF00BF80), the desktop's pixel at (2, 2) */
  begin(30, 2); put(0); put(0); put(W); put(H); end();
  begin(30, 3); put(60); end();
  begin(30, 6); end();
  CHECK(wait_event(60, 0));
  CHECK(near(pixel_of(image, 2, 2), 0xFF00BF80u));
  if (imported) CHECK(near(pixel_of(imported, 2, 2), 0xFF00BF80u));      /* (the imported image is the same allocation) */
  begin(1, 0); put(61); end();
  CHECK(wait_event(61, 0));
  CHECK(released == 0);      /* (the one buffer stays attached: never released) */
  out("wayland-vulkan: pixel ");
  out_x(pixel_of(image, 2, 2));
  out("\n");
  if (argc > 1) {      /* stay on screen a while (so the desktop can be looked at) */
    const long ts[2] = {8, 0};
    sys3(NR_NANOSLEEP, ts, 0, 0);
  }
done:
  out("wayland-vulkan: ");
  out_u(checks);
  out(" checks, ");
  out_u(failed);
  out(" failed\n");
  if (sock >= 0) sys3(NR_CLOSE, sock, 0, 0);
  sys3(NR_EXIT_GROUP, failed ? 1 : 0, 0, 0);
  for (;;) {}
}
