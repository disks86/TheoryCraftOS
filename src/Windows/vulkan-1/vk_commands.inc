/* vulkan-1.c, part 5: command buffers. vkCmd* record into a list; vkQueueSubmit runs the list: the draws become the kernel's command stream (CommandStream.hpp), the copies and clears
 * that the CPU can do are done in order with it. */

enum {
  C_BEGIN_PASS = 1, C_NEXT_SUBPASS, C_END_PASS, C_BIND_PIPE, C_BIND_VB, C_BIND_IB, C_BIND_SETS, C_PUSH, C_VIEWPORT, C_SCISSOR, C_DRAW, C_DRAW_INDEXED, C_COPY_BUF, C_COPY_BUF2IMG,
  C_COPY_IMG2BUF, C_COPY_IMG, C_BLIT, C_CLEAR_COLOR, C_CLEAR_DS, C_CLEAR_ATT, C_FILL, C_UPDATE, C_EXEC, C_RESOLVE
};

typedef struct Cmd {
  UINT op, n;
  ULONGLONG a, b, c, d, e;
  void *p;
} Cmd;

typedef struct CmdBuf {
  Hdr h;
  Device *dev;
  Cmd *cmds;
  UINT n, cap;
  BOOL failed;
  struct CmdPool *pool;
} CmdBuf;

typedef struct CmdPool {
  Device *dev;
  CmdBuf **bufs;
  UINT n, cap;
} CmdPool;

static void cmdbuf_reset(CmdBuf *cb) {
  for (UINT i = 0; i < cb->n; ++i) vk_free(cb->cmds[i].p);
  cb->n = 0;
  cb->failed = FALSE;
}

static Cmd *record(VkCommandBuffer buffer, UINT op) {
  CmdBuf *cb = (CmdBuf *)buffer;
  Cmd c;
  memset(&c, 0, sizeof c);
  c.op = op;
  if (!LIST_PUSH(cb->cmds, cb->n, cb->cap, c)) {
    cb->failed = TRUE;
    static Cmd dummy;
    return &dummy;
  }
  return &cb->cmds[cb->n - 1];
}
/* A copy of an array for the command to read at submit time. */
static void *keep(VkCommandBuffer buffer, const void *src, size_t bytes) {
  void *p = vk_alloc(bytes ? bytes : 1);
  if (!p) {
    ((CmdBuf *)buffer)->failed = TRUE;
    return NULL;
  }
  if (src) memcpy(p, src, bytes);
  return p;
}

VKAPI VkResult VKAPI_CALL vkCreateCommandPool(VkDevice device, const VkCommandPoolCreateInfo *ci, const VkAllocationCallbacks *alloc, VkCommandPool *out) {
  (void)ci, (void)alloc;
  CmdPool *p = vk_alloc(sizeof(CmdPool));
  if (!p) return VK_ERROR_OUT_OF_HOST_MEMORY;
  p->dev = (Device *)device;
  *out = (VkCommandPool)p;
  return VK_SUCCESS;
}
static void free_cmdbuf(CmdBuf *cb) {
  cmdbuf_reset(cb);
  vk_free(cb->cmds);
  vk_free(cb);
}
VKAPI void VKAPI_CALL vkDestroyCommandPool(VkDevice device, VkCommandPool pool, const VkAllocationCallbacks *alloc) {
  CmdPool *p = (CmdPool *)pool;
  (void)device, (void)alloc;
  if (!p) return;
  for (UINT i = 0; i < p->n; ++i) free_cmdbuf(p->bufs[i]);
  vk_free(p->bufs);
  vk_free(p);
}
VKAPI VkResult VKAPI_CALL vkResetCommandPool(VkDevice device, VkCommandPool pool, VkCommandPoolResetFlags flags) {
  CmdPool *p = (CmdPool *)pool;
  (void)device, (void)flags;
  for (UINT i = 0; i < p->n; ++i) cmdbuf_reset(p->bufs[i]);
  return VK_SUCCESS;
}
VKAPI void VKAPI_CALL vkTrimCommandPool(VkDevice device, VkCommandPool pool, VkCommandPoolTrimFlags flags) {
  (void)device, (void)pool, (void)flags;
}
VKAPI VkResult VKAPI_CALL vkAllocateCommandBuffers(VkDevice device, const VkCommandBufferAllocateInfo *ai, VkCommandBuffer *out) {
  CmdPool *p = (CmdPool *)ai->commandPool;
  for (UINT i = 0; i < ai->commandBufferCount; ++i) {
    CmdBuf *cb = vk_alloc(sizeof(CmdBuf));
    if (!cb || !LIST_PUSH(p->bufs, p->n, p->cap, cb)) {
      vk_free(cb);
      for (UINT k = 0; k < i; ++k) out[k] = NULL;
      return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    cb->h.magic = MAGIC_CMD;
    cb->dev = (Device *)device;
    cb->pool = p;
    out[i] = (VkCommandBuffer)cb;
  }
  return VK_SUCCESS;
}
VKAPI void VKAPI_CALL vkFreeCommandBuffers(VkDevice device, VkCommandPool pool, uint32_t count, const VkCommandBuffer *bufs) {
  CmdPool *p = (CmdPool *)pool;
  (void)device;
  for (UINT i = 0; i < count; ++i)
    for (UINT k = 0; k < p->n; ++k)
      if (p->bufs[k] == (CmdBuf *)bufs[i]) {
        free_cmdbuf(p->bufs[k]);
        p->bufs[k] = p->bufs[--p->n];
        break;
      }
}
VKAPI VkResult VKAPI_CALL vkBeginCommandBuffer(VkCommandBuffer buffer, const VkCommandBufferBeginInfo *bi) {
  (void)bi;
  cmdbuf_reset((CmdBuf *)buffer);
  return VK_SUCCESS;
}
VKAPI VkResult VKAPI_CALL vkEndCommandBuffer(VkCommandBuffer buffer) { return ((CmdBuf *)buffer)->failed ? VK_ERROR_OUT_OF_HOST_MEMORY : VK_SUCCESS; }
VKAPI VkResult VKAPI_CALL vkResetCommandBuffer(VkCommandBuffer buffer, VkCommandBufferResetFlags flags) {
  (void)flags;
  cmdbuf_reset((CmdBuf *)buffer);
  return VK_SUCCESS;
}

/* ---- recording ------------------------------------------------------------------------------------------------------------------------------------------------------------- */
VKAPI void VKAPI_CALL vkCmdBeginRenderPass(VkCommandBuffer b, const VkRenderPassBeginInfo *bi, VkSubpassContents contents) {
  (void)contents;
  Cmd *c = record(b, C_BEGIN_PASS);
  c->a = (ULONGLONG)(ULONG_PTR)bi->renderPass, c->b = (ULONGLONG)(ULONG_PTR)bi->framebuffer;
  c->c = ((ULONGLONG)(UINT)bi->renderArea.offset.x & 0xFFFFFFFFull) | ((ULONGLONG)(UINT)bi->renderArea.offset.y << 32);
  c->d = (ULONGLONG)bi->renderArea.extent.width | ((ULONGLONG)bi->renderArea.extent.height << 32);
  c->n = bi->clearValueCount;
  c->p = keep(b, bi->pClearValues, sizeof(VkClearValue) * bi->clearValueCount);
}
VKAPI void VKAPI_CALL vkCmdNextSubpass(VkCommandBuffer b, VkSubpassContents contents) {
  (void)contents;
  record(b, C_NEXT_SUBPASS);
}
VKAPI void VKAPI_CALL vkCmdEndRenderPass(VkCommandBuffer b) { record(b, C_END_PASS); }
VKAPI void VKAPI_CALL vkCmdBindPipeline(VkCommandBuffer b, VkPipelineBindPoint point, VkPipeline pipeline) {
  if (point != VK_PIPELINE_BIND_POINT_GRAPHICS) return;
  record(b, C_BIND_PIPE)->a = (ULONGLONG)(ULONG_PTR)pipeline;
}
VKAPI void VKAPI_CALL vkCmdBindVertexBuffers(VkCommandBuffer b, uint32_t first, uint32_t count, const VkBuffer *bufs, const VkDeviceSize *offsets) {
  for (UINT i = 0; i < count; ++i) {
    Cmd *c = record(b, C_BIND_VB);
    c->a = first + i, c->b = (ULONGLONG)(ULONG_PTR)bufs[i], c->c = offsets[i];
  }
}
VKAPI void VKAPI_CALL vkCmdBindIndexBuffer(VkCommandBuffer b, VkBuffer buffer, VkDeviceSize offset, VkIndexType type) {
  Cmd *c = record(b, C_BIND_IB);
  c->a = (ULONGLONG)(ULONG_PTR)buffer, c->b = offset, c->c = type;
}
VKAPI void VKAPI_CALL vkCmdBindDescriptorSets(VkCommandBuffer b, VkPipelineBindPoint point, VkPipelineLayout layout, uint32_t first, uint32_t count, const VkDescriptorSet *sets, uint32_t ndyn,
                                              const uint32_t *dyn) {
  (void)layout;
  if (point != VK_PIPELINE_BIND_POINT_GRAPHICS) return;
  Cmd *c = record(b, C_BIND_SETS);
  c->a = first, c->n = count, c->b = ndyn;
  c->p = keep(b, NULL, sizeof(void *) * count + sizeof(UINT) * ndyn);
  if (!c->p) return;
  memcpy(c->p, sets, sizeof(void *) * count);
  memcpy((BYTE *)c->p + sizeof(void *) * count, dyn, sizeof(UINT) * ndyn);
}
VKAPI void VKAPI_CALL vkCmdPushConstants(VkCommandBuffer b, VkPipelineLayout layout, VkShaderStageFlags stages, uint32_t offset, uint32_t size, const void *values) {
  (void)layout, (void)stages;
  Cmd *c = record(b, C_PUSH);
  c->a = offset, c->n = size;
  c->p = keep(b, values, size);
}
VKAPI void VKAPI_CALL vkCmdSetViewport(VkCommandBuffer b, uint32_t first, uint32_t count, const VkViewport *v) {
  if (first != 0 || count < 1) return;
  Cmd *c = record(b, C_VIEWPORT);
  c->p = keep(b, v, sizeof(VkViewport));
}
VKAPI void VKAPI_CALL vkCmdSetScissor(VkCommandBuffer b, uint32_t first, uint32_t count, const VkRect2D *r) {
  if (first != 0 || count < 1) return;
  Cmd *c = record(b, C_SCISSOR);
  c->p = keep(b, r, sizeof(VkRect2D));
}
VKAPI void VKAPI_CALL vkCmdSetLineWidth(VkCommandBuffer b, float w) { (void)b, (void)w; }
VKAPI void VKAPI_CALL vkCmdSetDepthBias(VkCommandBuffer b, float a, float c, float d) { (void)b, (void)a, (void)c, (void)d; }
VKAPI void VKAPI_CALL vkCmdSetBlendConstants(VkCommandBuffer b, const float c[4]) { (void)b, (void)c; }
VKAPI void VKAPI_CALL vkCmdSetDepthBounds(VkCommandBuffer b, float lo, float hi) { (void)b, (void)lo, (void)hi; }
VKAPI void VKAPI_CALL vkCmdSetStencilCompareMask(VkCommandBuffer b, VkStencilFaceFlags f, uint32_t m) { (void)b, (void)f, (void)m; }
VKAPI void VKAPI_CALL vkCmdSetStencilWriteMask(VkCommandBuffer b, VkStencilFaceFlags f, uint32_t m) { (void)b, (void)f, (void)m; }
VKAPI void VKAPI_CALL vkCmdSetStencilReference(VkCommandBuffer b, VkStencilFaceFlags f, uint32_t m) { (void)b, (void)f, (void)m; }
VKAPI void VKAPI_CALL vkCmdDraw(VkCommandBuffer b, uint32_t vertices, uint32_t instances, uint32_t first_vertex, uint32_t first_instance) {
  Cmd *c = record(b, C_DRAW);
  c->a = vertices, c->b = instances, c->c = first_vertex, c->d = first_instance;
}
VKAPI void VKAPI_CALL vkCmdDrawIndexed(VkCommandBuffer b, uint32_t indices, uint32_t instances, uint32_t first_index, int32_t vertex_offset, uint32_t first_instance) {
  Cmd *c = record(b, C_DRAW_INDEXED);
  c->a = indices, c->b = instances, c->c = first_index, c->d = (ULONGLONG)(UINT)vertex_offset, c->e = first_instance;
}
VKAPI void VKAPI_CALL vkCmdDrawIndirect(VkCommandBuffer b, VkBuffer buffer, VkDeviceSize offset, uint32_t count, uint32_t stride) {
  (void)buffer, (void)offset, (void)count, (void)stride;
  ((CmdBuf *)b)->failed = TRUE;
}
VKAPI void VKAPI_CALL vkCmdDrawIndexedIndirect(VkCommandBuffer b, VkBuffer buffer, VkDeviceSize offset, uint32_t count, uint32_t stride) {
  (void)buffer, (void)offset, (void)count, (void)stride;
  ((CmdBuf *)b)->failed = TRUE;
}
VKAPI void VKAPI_CALL vkCmdDispatch(VkCommandBuffer b, uint32_t x, uint32_t y, uint32_t z) { (void)x, (void)y, (void)z, ((CmdBuf *)b)->failed = TRUE; }
VKAPI void VKAPI_CALL vkCmdDispatchIndirect(VkCommandBuffer b, VkBuffer buffer, VkDeviceSize offset) { (void)buffer, (void)offset, ((CmdBuf *)b)->failed = TRUE; }

VKAPI void VKAPI_CALL vkCmdCopyBuffer(VkCommandBuffer b, VkBuffer src, VkBuffer dst, uint32_t count, const VkBufferCopy *regions) {
  Cmd *c = record(b, C_COPY_BUF);
  c->a = (ULONGLONG)(ULONG_PTR)src, c->b = (ULONGLONG)(ULONG_PTR)dst, c->n = count;
  c->p = keep(b, regions, sizeof(VkBufferCopy) * count);
}
VKAPI void VKAPI_CALL vkCmdCopyBufferToImage(VkCommandBuffer b, VkBuffer src, VkImage dst, VkImageLayout layout, uint32_t count, const VkBufferImageCopy *regions) {
  (void)layout;
  Cmd *c = record(b, C_COPY_BUF2IMG);
  c->a = (ULONGLONG)(ULONG_PTR)src, c->b = (ULONGLONG)(ULONG_PTR)dst, c->n = count;
  c->p = keep(b, regions, sizeof(VkBufferImageCopy) * count);
}
VKAPI void VKAPI_CALL vkCmdCopyImageToBuffer(VkCommandBuffer b, VkImage src, VkImageLayout layout, VkBuffer dst, uint32_t count, const VkBufferImageCopy *regions) {
  (void)layout;
  Cmd *c = record(b, C_COPY_IMG2BUF);
  c->a = (ULONGLONG)(ULONG_PTR)src, c->b = (ULONGLONG)(ULONG_PTR)dst, c->n = count;
  c->p = keep(b, regions, sizeof(VkBufferImageCopy) * count);
}
VKAPI void VKAPI_CALL vkCmdCopyImage(VkCommandBuffer b, VkImage src, VkImageLayout sl, VkImage dst, VkImageLayout dl, uint32_t count, const VkImageCopy *regions) {
  (void)sl, (void)dl;
  Cmd *c = record(b, C_COPY_IMG);
  c->a = (ULONGLONG)(ULONG_PTR)src, c->b = (ULONGLONG)(ULONG_PTR)dst, c->n = count;
  c->p = keep(b, regions, sizeof(VkImageCopy) * count);
}
VKAPI void VKAPI_CALL vkCmdBlitImage(VkCommandBuffer b, VkImage src, VkImageLayout sl, VkImage dst, VkImageLayout dl, uint32_t count, const VkImageBlit *regions, VkFilter filter) {
  (void)sl, (void)dl, (void)filter;
  Cmd *c = record(b, C_BLIT);
  c->a = (ULONGLONG)(ULONG_PTR)src, c->b = (ULONGLONG)(ULONG_PTR)dst, c->n = count;
  c->p = keep(b, regions, sizeof(VkImageBlit) * count);
}
VKAPI void VKAPI_CALL vkCmdResolveImage(VkCommandBuffer b, VkImage src, VkImageLayout sl, VkImage dst, VkImageLayout dl, uint32_t count, const VkImageResolve *regions) {
  (void)sl, (void)dl, (void)count, (void)regions;
  Cmd *c = record(b, C_RESOLVE);
  c->a = (ULONGLONG)(ULONG_PTR)src, c->b = (ULONGLONG)(ULONG_PTR)dst;
}
VKAPI void VKAPI_CALL vkCmdClearColorImage(VkCommandBuffer b, VkImage image, VkImageLayout layout, const VkClearColorValue *color, uint32_t count, const VkImageSubresourceRange *ranges) {
  (void)layout;
  Cmd *c = record(b, C_CLEAR_COLOR);
  c->a = (ULONGLONG)(ULONG_PTR)image, c->n = count;
  c->p = keep(b, NULL, sizeof(VkClearColorValue) + sizeof(VkImageSubresourceRange) * count);
  if (!c->p) return;
  memcpy(c->p, color, sizeof(VkClearColorValue));
  memcpy((BYTE *)c->p + sizeof(VkClearColorValue), ranges, sizeof(VkImageSubresourceRange) * count);
}
VKAPI void VKAPI_CALL vkCmdClearDepthStencilImage(VkCommandBuffer b, VkImage image, VkImageLayout layout, const VkClearDepthStencilValue *value, uint32_t count, const VkImageSubresourceRange *ranges) {
  (void)layout, (void)count, (void)ranges;
  Cmd *c = record(b, C_CLEAR_DS);
  c->a = (ULONGLONG)(ULONG_PTR)image;
  c->p = keep(b, value, sizeof *value);
}
VKAPI void VKAPI_CALL vkCmdClearAttachments(VkCommandBuffer b, uint32_t count, const VkClearAttachment *atts, uint32_t nrects, const VkClearRect *rects) {
  (void)nrects, (void)rects;
  Cmd *c = record(b, C_CLEAR_ATT);
  c->n = count;
  c->p = keep(b, atts, sizeof(VkClearAttachment) * count);
}
VKAPI void VKAPI_CALL vkCmdFillBuffer(VkCommandBuffer b, VkBuffer dst, VkDeviceSize offset, VkDeviceSize size, uint32_t data) {
  Cmd *c = record(b, C_FILL);
  c->a = (ULONGLONG)(ULONG_PTR)dst, c->b = offset, c->c = size, c->d = data;
}
VKAPI void VKAPI_CALL vkCmdUpdateBuffer(VkCommandBuffer b, VkBuffer dst, VkDeviceSize offset, VkDeviceSize size, const void *data) {
  Cmd *c = record(b, C_UPDATE);
  c->a = (ULONGLONG)(ULONG_PTR)dst, c->b = offset, c->c = size;
  c->p = keep(b, data, (size_t)size);
}
VKAPI void VKAPI_CALL vkCmdExecuteCommands(VkCommandBuffer b, uint32_t count, const VkCommandBuffer *bufs) {
  for (UINT i = 0; i < count; ++i) record(b, C_EXEC)->a = (ULONGLONG)(ULONG_PTR)bufs[i];
}
VKAPI void VKAPI_CALL vkCmdPipelineBarrier(VkCommandBuffer b, VkPipelineStageFlags s, VkPipelineStageFlags d, VkDependencyFlags f, uint32_t nm, const VkMemoryBarrier *m, uint32_t nb,
                                           const VkBufferMemoryBarrier *bb, uint32_t ni, const VkImageMemoryBarrier *ib) {
  (void)b, (void)s, (void)d, (void)f, (void)nm, (void)m, (void)nb, (void)bb, (void)ni, (void)ib;   /* (every command has run before the next starts) */
}
VKAPI void VKAPI_CALL vkCmdSetEvent(VkCommandBuffer b, VkEvent e, VkPipelineStageFlags s) { (void)b, (void)e, (void)s; }
VKAPI void VKAPI_CALL vkCmdResetEvent(VkCommandBuffer b, VkEvent e, VkPipelineStageFlags s) { (void)b, (void)e, (void)s; }
VKAPI void VKAPI_CALL vkCmdWaitEvents(VkCommandBuffer b, uint32_t n, const VkEvent *e, VkPipelineStageFlags s, VkPipelineStageFlags d, uint32_t nm, const VkMemoryBarrier *m, uint32_t nb,
                                      const VkBufferMemoryBarrier *bb, uint32_t ni, const VkImageMemoryBarrier *ib) {
  (void)b, (void)n, (void)e, (void)s, (void)d, (void)nm, (void)m, (void)nb, (void)bb, (void)ni, (void)ib;
}
VKAPI void VKAPI_CALL vkCmdBeginQuery(VkCommandBuffer b, VkQueryPool p, uint32_t q, VkQueryControlFlags f) { (void)b, (void)p, (void)q, (void)f; }
VKAPI void VKAPI_CALL vkCmdEndQuery(VkCommandBuffer b, VkQueryPool p, uint32_t q) { (void)b, (void)p, (void)q; }
VKAPI void VKAPI_CALL vkCmdResetQueryPool(VkCommandBuffer b, VkQueryPool p, uint32_t f, uint32_t c) { (void)b, (void)p, (void)f, (void)c; }
VKAPI void VKAPI_CALL vkCmdWriteTimestamp(VkCommandBuffer b, VkPipelineStageFlagBits s, VkQueryPool p, uint32_t q) { (void)b, (void)s, (void)p, (void)q; }
VKAPI void VKAPI_CALL vkCmdCopyQueryPoolResults(VkCommandBuffer b, VkQueryPool p, uint32_t f, uint32_t c, VkBuffer d, VkDeviceSize o, VkDeviceSize s, VkQueryResultFlags fl) {
  (void)b, (void)p, (void)f, (void)c, (void)d, (void)o, (void)s, (void)fl;
}

/* ---- running a command buffer ---------------------------------------------------------------------------------------------------------------------------------------- */
typedef struct Exec {
  Device *d;
  const Caps *caps;
  UINT bytes;                  /* of the stream so far */
  UINT nalloc;
  KMT_HANDLE temps[256];
  UINT ntemps;
  BOOL stream_open;            /* a pass is open in the stream */
  RenderPass *rp;
  Framebuffer *fb;
  UINT subpass;
  const VkClearValue *clears;
  UINT nclears;
  BOOL attachment_used[9];
  Pipe *pipe;
  DescSet *sets[8];
  UINT dyn[8][8];
  BYTE push[256];
  struct {
    Buf *b;
    ULONGLONG off;
  } vb[8];
  Buf *ib;
  ULONGLONG ib_off;
  UINT ib_type;
  VkViewport vp;
  VkRect2D sc;
  BOOL vp_set, sc_set;
  BOOL dirty_pipe, dirty_state, dirty_vb, dirty_ib;
  UINT topology_sent;
  BOOL failed;
} Exec;

static UINT *stream_words(Exec *x, UINT n) {
  if (x->bytes + n * 4 > x->d->dma_bytes) return NULL;
  UINT *w = (UINT *)(x->d->dma + x->bytes);
  x->bytes += n * 4;
  return w;
}
/* One packet: the op and the payload words. */
static BOOL packet(Exec *x, UINT op, const UINT *payload, UINT n) {
  UINT *w = stream_words(x, n + 1);
  if (!w) return FALSE;
  w[0] = op | ((n + 1) << 16);
  for (UINT i = 0; i < n; ++i) w[i + 1] = payload[i];
  return TRUE;
}
static UINT fbits(float f) {
  union { float f; UINT u; } v;
  v.f = f;
  return v.u;
}
enum { OP_BEGIN_PASS = 1, OP_END_PASS, OP_BIND_PIPELINE, OP_BIND_VB, OP_BIND_IB, OP_PUSH, OP_BIND_TEXTURE, OP_VIEWPORT, OP_SCISSOR, OP_DRAW, OP_DRAW_INDEXED, OP_COPY, OP_MIPMAPS, OP_TOPOLOGY,
       OP_DRAW_INSTANCED, OP_DRAW_INDEXED_INSTANCED, OP_BEGIN_QUERY, OP_END_QUERY, OP_BIND_UAV, OP_DISPATCH, OP_RESOLVE };
#define NO_ALLOC 0xFFFFFFFFu

/* The index of an allocation in the stream's allocation list. */
static int alloc_index(Exec *x, KMT_HANDLE a) {
  for (UINT i = 0; i < x->nalloc; ++i)
    if (get32(x->d->alloc_list, i * 8) == a) return (int)i;
  if (x->nalloc >= x->d->alloc_entries) return -1;
  put32(x->d->alloc_list, x->nalloc * 8, a);
  put32(x->d->alloc_list, x->nalloc * 8 + 4, 0);
  return (int)x->nalloc++;
}

/* Runs the stream written so far. */
static void flush(Exec *x) {
  if (x->bytes == 0) return;
  BYTE args[128];
  memset(args, 0, sizeof args);
  put32(args, 0, x->d->ctx);
  put32(args, 4, 0);
  put32(args, 8, x->bytes);
  put32(args, 12, x->nalloc);
  NTSTATUS st = D3DKMTRender(args);
  if (!STATUS_OK(st)) {
    x->failed = TRUE;
    trace_status("D3DKMTRender", (UINT)st);
  } else {
    /* The stream has run (or been refused) when Render returns; a context that lost a submission says so in the device state (a lost device). */
    BYTE state[16];
    memset(state, 0, sizeof state);
    put32(state, 0, x->d->dev);
    put32(state, 4, 3);   /* TdrReset */
    if (STATUS_OK(D3DKMTGetDeviceState(state)) && get32(state, 8)) {
      x->failed = TRUE;
      trace("the adapter refused a command stream: the device is lost");
    }
  }
  x->bytes = 0;
  x->nalloc = 0;
  x->stream_open = FALSE;
  x->dirty_pipe = x->dirty_state = x->dirty_vb = x->dirty_ib = TRUE;
  x->topology_sent = 0xFFFFFFFFu;
}

static BOOL ensure_room(Exec *x, UINT words) {
  if (x->bytes + words * 4 + 256 <= x->d->dma_bytes && x->nalloc + 8 <= x->d->alloc_entries) return TRUE;
  if (!x->stream_open) {
    flush(x);
    return TRUE;
  }
  return FALSE;
}

/* Starts the pass of the current subpass in the stream. */
static void begin_stream_pass(Exec *x) {
  const Subpass *sp = &x->rp->sub[x->subpass];
  UINT color[4] = {NO_ALLOC, NO_ALLOC, NO_ALLOC, NO_ALLOC};
  UINT flags = 0;
  float clear_color[4] = {0, 0, 0, 0}, clear_depth = 1.0f;
  UINT clear_stencil = 0;
  UINT depth = NO_ALLOC;
  for (UINT c = 0; c < sp->ncolor; ++c) {
    UINT att = sp->color[c];
    if (att >= x->fb->nview) continue;
    Img *img = x->fb->view[att]->img;
    int idx = alloc_index(x, img->alloc);
    color[c] = idx < 0 ? NO_ALLOC : (UINT)idx;
    img->gpu_written = TRUE;
    if (x->rp->att[att].load == VK_ATTACHMENT_LOAD_OP_CLEAR && !x->attachment_used[att] && att < x->nclears) {
      flags |= 1;
      for (int k = 0; k < 4; ++k) clear_color[k] = x->clears[att].color.float32[k];
    }
    x->attachment_used[att] = TRUE;
  }
  if (sp->depth != 0xFFFFFFFFu && sp->depth < x->fb->nview) {
    UINT att = sp->depth;
    Img *img = x->fb->view[att]->img;
    int idx = alloc_index(x, img->alloc);
    depth = idx < 0 ? NO_ALLOC : (UINT)idx;
    img->gpu_written = TRUE;
    if (!x->attachment_used[att] && att < x->nclears) {
      if (x->rp->att[att].load == VK_ATTACHMENT_LOAD_OP_CLEAR) flags |= 2, clear_depth = x->clears[att].depthStencil.depth;
      if (x->rp->att[att].stencil_load == VK_ATTACHMENT_LOAD_OP_CLEAR) flags |= 4, clear_stencil = x->clears[att].depthStencil.stencil;
    }
    x->attachment_used[att] = TRUE;
  }
  UINT nextra = 0;
  for (UINT c = 1; c < 4; ++c)
    if (color[c] != NO_ALLOC) nextra = c;
  UINT payload[12];
  payload[0] = color[0], payload[1] = depth, payload[2] = flags;
  for (int k = 0; k < 4; ++k) payload[3 + k] = fbits(clear_color[k]);
  payload[7] = fbits(clear_depth), payload[8] = clear_stencil;
  for (UINT c = 1; c <= nextra; ++c) payload[8 + c] = color[c];
  if (!packet(x, OP_BEGIN_PASS, payload, 9 + nextra)) x->failed = TRUE;
  x->stream_open = TRUE;
  x->dirty_pipe = x->dirty_state = x->dirty_vb = x->dirty_ib = TRUE;
  x->topology_sent = 0xFFFFFFFFu;
}

static void end_stream_pass(Exec *x) {
  if (!x->stream_open) return;
  packet(x, OP_END_PASS, NULL, 0);
  x->stream_open = FALSE;
}

/* After a subpass: the multisample resolve attachments. */
static void resolve_subpass(Exec *x) {
  const Subpass *sp = &x->rp->sub[x->subpass];
  if (!sp->nresolve) return;
  for (UINT c = 0; c < sp->ncolor && c < sp->nresolve; ++c) {
    UINT from = sp->color[c], to = sp->resolve[c];
    if (from >= x->fb->nview || to >= x->fb->nview) continue;
    int di = alloc_index(x, x->fb->view[to]->img->alloc), si = alloc_index(x, x->fb->view[from]->img->alloc);
    if (di < 0 || si < 0) continue;
    UINT payload[2] = {(UINT)di, (UINT)si};
    packet(x, OP_RESOLVE, payload, 2);
    x->fb->view[to]->img->gpu_written = TRUE;
  }
}

/* The constant data of the pipeline's constant buffer slots, in the order the shaders declared them. Returns the number of bytes. */
static UINT gather_constants(Exec *x, BYTE *out, UINT cap) {
  Pipe *p = x->pipe;
  UINT total = 0;
  for (UINT i = 0; i < p->ncb; ++i) {
    UINT bytes = p->cb[i].vec4 * 16;
    if (total + bytes > cap) break;
    BYTE *dst = out + total;
    memset(dst, 0, bytes);
    if (p->cb[i].kind == SPV_RES_PUSH) {
      memcpy(dst, x->push, MIN(bytes, (UINT)sizeof x->push));
    } else if (p->cb[i].set < 8 && x->sets[p->cb[i].set]) {
      DescSet *s = x->sets[p->cb[i].set];
      int bi = layout_binding_index(s->layout, p->cb[i].binding);
      if (bi >= 0 && s->layout->b[bi].count > 0 && s->d[s->first[bi]].buf) {
        Descr *d = &s->d[s->first[bi]];
        ULONGLONG off = d->off;
        if (s->layout->b[bi].type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC) {
          UINT di = 0;
          for (UINT k = 0; k < s->layout->nb; ++k)
            if (s->layout->b[k].type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC && s->layout->b[k].binding < p->cb[i].binding) di += s->layout->b[k].count;
          off += x->dyn[p->cb[i].set][di & 7];
        }
        Buf *buf = d->buf;
        if (buf->mem && off < buf->size) {
          ULONGLONG n = MIN((ULONGLONG)bytes, MIN(d->range, buf->size - off));
          memcpy(dst, buf->mem->data + buf->off + off, (size_t)n);
        }
      }
    }
    total += bytes;
  }
  return total;
}

static void bind_textures(Exec *x) {
  Pipe *p = x->pipe;
  for (UINT u = 0; u < p->nunit; ++u) {
    TexUnit *tu = &p->unit[u];
    if (tu->set >= 8 || !x->sets[tu->set]) continue;
    DescSet *s = x->sets[tu->set];
    int bi = layout_binding_index(s->layout, tu->binding);
    if (bi < 0) continue;
    UINT index = s->first[bi] + tu->element;
    if (index >= s->nd) continue;
    Descr *d = &s->d[index];
    Sampler *smp = d->sampler;
    if (tu->sampler_set < 8 && x->sets[tu->sampler_set]) {
      DescSet *ss = x->sets[tu->sampler_set];
      int sb = layout_binding_index(ss->layout, tu->sampler_binding);
      if (sb >= 0 && ss->first[sb] < ss->nd) smp = ss->d[ss->first[sb]].sampler;
    }
    if (!d->view || !smp) continue;
    Img *img = d->view->img;
    image_sync_up(img);
    int idx = alloc_index(x, img->alloc);
    if (idx < 0) continue;
    UINT payload[9] = {u, (UINT)idx, smp->filter, smp->addr_u, smp->addr_v, smp->mip_filter, fbits(smp->lod_bias), fbits(smp->min_lod), fbits(smp->max_lod)};
    packet(x, OP_BIND_TEXTURE, payload, 9);
  }
}

static BOOL vertex_direct(Pipe *p) {
  if (p->nattrs == 0) return TRUE;
  UINT binding = p->attrs[0].binding;
  for (UINT i = 0; i < p->nattrs; ++i) {
    const VtxFormat *vf = vertex_format(p->attrs[i].format);
    if (!vf || vf->kind != 0 || p->attrs[i].binding != binding || p->attrs[i].offset != p->out_offset[i]) return FALSE;
  }
  for (UINT b = 0; b < p->nbindings; ++b)
    if (p->bindings[b].binding == binding) return !p->bindings[b].per_instance && p->bindings[b].stride == p->out_stride;
  return FALSE;
}

static const VtxBinding *find_vtx_binding(Pipe *p, UINT binding) {
  for (UINT b = 0; b < p->nbindings; ++b)
    if (p->bindings[b].binding == binding) return &p->bindings[b];
  return NULL;
}

/* Vertices lo .. lo + count - 1 of the bound buffers as one buffer of the layout the pipeline was made with. Returns the allocation (temporary) or 0. */
static KMT_HANDLE gather_vertices(Exec *x, UINT lo, UINT count, UINT first_instance, UINT instances) {
  Pipe *p = x->pipe;
  UINT elements = MAX(count, first_instance + instances);
  if (elements == 0 || p->out_stride == 0) return 0;
  ULONGLONG bytes = (ULONGLONG)elements * p->out_stride;
  if (bytes > (64u << 20)) return 0;
  BYTE *tmp = vk_alloc((size_t)bytes);
  if (!tmp) return 0;
  for (UINT e = 0; e < elements; ++e) {
    for (UINT a = 0; a < p->nattrs; ++a) {
      const VtxBinding *vb = find_vtx_binding(p, p->attrs[a].binding);
      const VtxFormat *vf = vertex_format(p->attrs[a].format);
      if (!vb || !vf) continue;
      UINT src_element = vb->per_instance ? e : lo + e;
      if (vb->per_instance ? e >= first_instance + instances : e >= count) continue;
      Buf *buf = p->attrs[a].binding < 8 ? x->vb[p->attrs[a].binding].b : NULL;
      if (!buf || !buf->mem) continue;
      ULONGLONG at = x->vb[p->attrs[a].binding].off + (ULONGLONG)src_element * vb->stride + p->attrs[a].offset;
      if (at + vf->bytes > buf->size) continue;
      const BYTE *src = buf->mem->data + buf->off + at;
      float *dst = (float *)(tmp + (ULONGLONG)e * p->out_stride + p->out_offset[a]);
      for (UINT c = 0; c < vf->comps; ++c) dst[c] = read_component(vf, src, c);
    }
  }
  KMT_HANDLE a = 0;
  ULONGLONG alloc_bytes = (bytes + 15) & ~15ull;
  if (kmt_make_buffer(x->d, alloc_bytes, A_VERTEX, &a)) {
    BYTE *dst = kmt_lock(x->d, a, 0, LOCK_DISCARD);
    if (dst) {
      memcpy(dst, tmp, (size_t)bytes);
      kmt_unlock(x->d, a);
      if (x->ntemps < 256) x->temps[x->ntemps++] = a;
      else a = 0;
    } else {
      kmt_free(x->d, a);
      a = 0;
    }
  }
  vk_free(tmp);
  return a;
}

static UINT map_topology(UINT t) {
  switch (t) {
    case VK_PRIMITIVE_TOPOLOGY_POINT_LIST: return 5;
    case VK_PRIMITIVE_TOPOLOGY_LINE_LIST: return 3;
    case VK_PRIMITIVE_TOPOLOGY_LINE_STRIP: return 4;
    case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP: return 1;
    case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN: return 2;
    default: return 0;
  }
}

static void do_draw(Exec *x, UINT count, UINT instances, UINT first, int vertex_offset, UINT first_instance, BOOL indexed) {
  Pipe *p = x->pipe;
  if (!p || !x->rp || count == 0 || instances == 0) return;
  if (!ensure_room(x, 64 + p->nunit * 10)) {
    /* The stream is full in the middle of a pass: run it and start the pass again where it left off (without clearing). */
    end_stream_pass(x);
    flush(x);
    begin_stream_pass(x);
  }
  BOOL direct = vertex_direct(p) && p->nattrs > 0 && p->attrs[0].binding < 8 && x->vb[p->attrs[0].binding].b && instances == 1 && first_instance == 0;
  KMT_HANDLE gathered = 0;
  UINT draw_first = first;
  int draw_offset = vertex_offset;
  if (!direct && p->nattrs > 0) {
    UINT lo = 0, hi = 0;
    if (indexed) {
      Buf *ib = x->ib;
      if (!ib || !ib->mem) return;
      UINT isz = x->ib_type == VK_INDEX_TYPE_UINT32 ? 4 : 2;
      ULONGLONG at = x->ib_off + (ULONGLONG)first * isz;
      if (at + (ULONGLONG)count * isz > ib->size) return;
      const BYTE *src = ib->mem->data + ib->off + at;
      UINT mn = 0xFFFFFFFFu, mx = 0;
      for (UINT i = 0; i < count; ++i) {
        UINT v = isz == 4 ? ((const UINT *)src)[i] : ((const USHORT *)src)[i];
        if (v == (isz == 4 ? 0xFFFFFFFFu : 0xFFFFu)) continue;
        mn = MIN(mn, v), mx = MAX(mx, v);
      }
      if (mn > mx) return;
      int base = vertex_offset;
      lo = (UINT)((int)mn + base);
      hi = (UINT)((int)mx + base);
      draw_offset = base - (int)lo;
    } else {
      lo = first;
      hi = first + count - 1;
      draw_first = 0;
    }
    gathered = gather_vertices(x, lo, hi - lo + 1, first_instance, instances);
    if (!gathered) return;
  }
  /* state */
  if (x->dirty_pipe) {
    UINT h = p->pipe;
    packet(x, OP_BIND_PIPELINE, &h, 1);
    x->dirty_pipe = FALSE;
    x->dirty_state = TRUE;
  }
  if (x->dirty_state) {
    BYTE constants[4096];
    UINT bytes = gather_constants(x, constants, MIN((UINT)sizeof constants, x->caps->max_push_constant_bytes));
    if (bytes) {
      UINT words = bytes / 4;
      UINT *w = stream_words(x, 2 + words);
      if (w) {
        w[0] = OP_PUSH | ((2 + words) << 16);
        w[1] = bytes;
        memcpy(w + 2, constants, bytes);
      }
    }
    bind_textures(x);
    x->dirty_state = FALSE;
  }
  {
    VkViewport vp = p->dyn_viewport || x->vp_set ? x->vp : p->viewport;
    if (!x->vp_set && !p->dyn_viewport) vp = p->viewport;
    if (vp.width == 0 && vp.height == 0 && x->fb) vp.x = 0, vp.y = 0, vp.width = (float)x->fb->w, vp.height = (float)x->fb->h, vp.minDepth = 0, vp.maxDepth = 1;
    UINT payload[6] = {fbits(vp.x), fbits(vp.y), fbits(vp.width), fbits(vp.height), fbits(vp.minDepth), fbits(vp.maxDepth)};
    packet(x, OP_VIEWPORT, payload, 6);
    VkRect2D sc = (p->dyn_scissor || x->sc_set) ? x->sc : p->scissor;
    if (!x->sc_set && !p->dyn_scissor) sc = p->scissor;
    if (sc.extent.width == 0 && sc.extent.height == 0 && x->fb) sc.offset.x = sc.offset.y = 0, sc.extent.width = x->fb->w, sc.extent.height = x->fb->h;
    UINT sp[4] = {(UINT)sc.offset.x, (UINT)sc.offset.y, sc.extent.width, sc.extent.height};
    packet(x, OP_SCISSOR, sp, 4);
  }
  UINT topo = map_topology(p->topology);
  if (topo != x->topology_sent) {
    packet(x, OP_TOPOLOGY, &topo, 1);
    x->topology_sent = topo;
  }
  if (p->nattrs > 0) {
    UINT alloc, off = 0;
    int idx;
    if (direct) {
      if (!buf_sync(x->vb[p->attrs[0].binding].b, &alloc)) return;
      idx = alloc_index(x, alloc);
      off = (UINT)x->vb[p->attrs[0].binding].off;
    } else {
      idx = alloc_index(x, gathered);
    }
    if (idx < 0) return;
    UINT payload[2] = {(UINT)idx, off};
    packet(x, OP_BIND_VB, payload, 2);
  }
  if (indexed) {
    Buf *ib = x->ib;
    UINT alloc;
    if (!ib || !buf_sync(ib, &alloc)) return;
    int idx = alloc_index(x, alloc);
    if (idx < 0) return;
    UINT payload[3] = {(UINT)idx, x->ib_type == VK_INDEX_TYPE_UINT32 ? 1u : 0u, (UINT)x->ib_off};
    packet(x, OP_BIND_IB, payload, 3);
  }
  if (indexed) {
    if (instances == 1 && first_instance == 0) {
      UINT payload[3] = {count, first, (UINT)draw_offset};
      packet(x, OP_DRAW_INDEXED, payload, 3);
    } else {
      UINT payload[5] = {count, first, (UINT)draw_offset, instances, first_instance};
      packet(x, OP_DRAW_INDEXED_INSTANCED, payload, 5);
    }
  } else if (instances == 1 && first_instance == 0) {
    UINT payload[2] = {count, direct ? first : draw_first};
    packet(x, OP_DRAW, payload, 2);
  } else {
    UINT payload[4] = {count, direct ? first : draw_first, instances, first_instance};
    packet(x, OP_DRAW_INSTANCED, payload, 4);
  }
}

/* ---- the copies and clears the CPU does ---------------------------------------------------------------------------------------------------------------------------- */
static BYTE *level_pointer(Img *img, UINT level, BOOL write, UINT *pitch_out) {
  BYTE *p = kmt_lock(img->dev, img->alloc, level, write ? 0 : LOCK_READ_ONLY);
  *pitch_out = level_extent(img->w, level) * img->bpp;
  return p;
}

static void copy_buffer_image(Exec *x, const VkBufferImageCopy *r, Buf *buf, Img *img, BOOL to_image) {
  if (!buf->mem || img->is_depth) return;
  UINT layers_in_alloc = img->kind == 1 ? img->depth : img->kind == 3 ? 6 : img->kind == 2 ? img->layers : 1;
  UINT pitch;
  BYTE *level = level_pointer(img, r->imageSubresource.mipLevel, to_image, &pitch);
  if (!level) return;
  UINT lw = level_extent(img->w, r->imageSubresource.mipLevel), lh = level_extent(img->h, r->imageSubresource.mipLevel);
  UINT row_len = r->bufferRowLength ? r->bufferRowLength : r->imageExtent.width;
  UINT img_h = r->bufferImageHeight ? r->bufferImageHeight : r->imageExtent.height;
  UINT slices = MAX(r->imageExtent.depth, r->imageSubresource.layerCount);
  for (UINT z = 0; z < slices; ++z) {
    UINT slice = (img->kind == 1 ? (UINT)r->imageOffset.z : r->imageSubresource.baseArrayLayer) + z;
    if (slice >= layers_in_alloc) break;
    for (UINT y = 0; y < r->imageExtent.height; ++y) {
      int iy = r->imageOffset.y + (int)y, ix = r->imageOffset.x;
      if (iy < 0 || (UINT)iy >= lh || ix < 0 || (UINT)ix + r->imageExtent.width > lw) continue;
      ULONGLONG boff = buf->off + r->bufferOffset + ((ULONGLONG)z * img_h + y) * row_len * img->bpp;
      ULONGLONG ioff = (ULONGLONG)slice * lh * pitch + (ULONGLONG)iy * pitch + (ULONGLONG)ix * img->bpp;
      ULONGLONG n = (ULONGLONG)r->imageExtent.width * img->bpp;
      if (boff + n > buf->mem->size) continue;
      if (to_image) {
        memcpy(level + ioff, buf->mem->data + boff, (size_t)n);
        if (img->swap_rb) swap_rb_pixels(level + ioff, r->imageExtent.width);
      } else {
        memcpy(buf->mem->data + boff, level + ioff, (size_t)n);
        if (img->swap_rb) swap_rb_pixels(buf->mem->data + boff, r->imageExtent.width);
      }
    }
  }
  kmt_unlock(img->dev, img->alloc);
  if (!to_image) ++buf->mem->version;
}

static void exec_copy_image(const VkImageCopy *r, Img *src, Img *dst) {
  if (src->is_depth || dst->is_depth || src->bpp != dst->bpp) return;
  UINT sp, dp;
  BYTE *s = level_pointer(src, r->srcSubresource.mipLevel, FALSE, &sp);
  if (!s) return;
  BYTE *d = src == dst && r->srcSubresource.mipLevel == r->dstSubresource.mipLevel ? s : level_pointer(dst, r->dstSubresource.mipLevel, TRUE, &dp);
  if (!d) {
    kmt_unlock(src->dev, src->alloc);
    return;
  }
  dp = level_extent(dst->w, r->dstSubresource.mipLevel) * dst->bpp;
  UINT sh = level_extent(src->h, r->srcSubresource.mipLevel), dh = level_extent(dst->h, r->dstSubresource.mipLevel);
  for (UINT y = 0; y < r->extent.height; ++y) {
    int sy = r->srcOffset.y + (int)y, dy = r->dstOffset.y + (int)y;
    if (sy < 0 || dy < 0 || (UINT)sy >= sh || (UINT)dy >= dh) continue;
    BYTE *row = d + (ULONGLONG)dy * dp + (ULONGLONG)r->dstOffset.x * dst->bpp;
    memmove(row, s + (ULONGLONG)sy * sp + (ULONGLONG)r->srcOffset.x * src->bpp, (size_t)r->extent.width * src->bpp);
    if (src->swap_rb != dst->swap_rb && src->bpp == 4) swap_rb_pixels(row, r->extent.width);
  }
  if (d != s) kmt_unlock(dst->dev, dst->alloc);
  kmt_unlock(src->dev, src->alloc);
}

static void exec_blit(const VkImageBlit *r, Img *src, Img *dst) {
  if (src->is_depth || dst->is_depth || src->bpp != dst->bpp || src == dst) return;
  UINT sp, dp;
  BYTE *s = level_pointer(src, r->srcSubresource.mipLevel, FALSE, &sp);
  if (!s) return;
  BYTE *d = level_pointer(dst, r->dstSubresource.mipLevel, TRUE, &dp);
  if (!d) {
    kmt_unlock(src->dev, src->alloc);
    return;
  }
  int sx0 = r->srcOffsets[0].x, sy0 = r->srcOffsets[0].y, sx1 = r->srcOffsets[1].x, sy1 = r->srcOffsets[1].y;
  int dx0 = r->dstOffsets[0].x, dy0 = r->dstOffsets[0].y, dx1 = r->dstOffsets[1].x, dy1 = r->dstOffsets[1].y;
  int dw = dx1 - dx0, dh = dy1 - dy0, sw = sx1 - sx0, sh = sy1 - sy0;
  UINT lw = level_extent(dst->w, r->dstSubresource.mipLevel), lh = level_extent(dst->h, r->dstSubresource.mipLevel);
  UINT slw = level_extent(src->w, r->srcSubresource.mipLevel), slh = level_extent(src->h, r->srcSubresource.mipLevel);
  for (int y = 0; y < (dh < 0 ? -dh : dh); ++y)
    for (int xx = 0; xx < (dw < 0 ? -dw : dw); ++xx) {
      int dxp = dw < 0 ? dx0 - 1 - xx : dx0 + xx, dyp = dh < 0 ? dy0 - 1 - y : dy0 + y;
      int fx = (int)(((long long)(2 * xx + 1) * (sw < 0 ? -sw : sw)) / (2 * (dw < 0 ? -dw : dw))), fy = (int)(((long long)(2 * y + 1) * (sh < 0 ? -sh : sh)) / (2 * (dh < 0 ? -dh : dh)));
      int sxp = sw < 0 ? sx0 - 1 - fx : sx0 + fx, syp = sh < 0 ? sy0 - 1 - fy : sy0 + fy;
      if (dxp < 0 || dyp < 0 || (UINT)dxp >= lw || (UINT)dyp >= lh || sxp < 0 || syp < 0 || (UINT)sxp >= slw || (UINT)syp >= slh) continue;
      BYTE *px = d + (ULONGLONG)dyp * dp + (ULONGLONG)dxp * dst->bpp;
      memcpy(px, s + (ULONGLONG)syp * sp + (ULONGLONG)sxp * src->bpp, src->bpp);
      if (src->swap_rb != dst->swap_rb && src->bpp == 4) swap_rb_pixels(px, 1);
    }
  kmt_unlock(dst->dev, dst->alloc);
  kmt_unlock(src->dev, src->alloc);
}

static UINT float_to_half(float f) {
  union { float f; UINT u; } v;
  v.f = f;
  UINT sign = (v.u >> 16) & 0x8000, exp = (v.u >> 23) & 0xFF, man = v.u & 0x7FFFFF;
  if (exp == 0xFF) return sign | 0x7C00 | (man ? 0x200 : 0);
  int e = (int)exp - 127 + 15;
  if (e >= 31) return sign | 0x7C00;
  if (e <= 0) return e < -10 ? sign : sign | ((man | 0x800000) >> (14 - e));
  return sign | ((UINT)e << 10) | (man >> 13);
}
static BYTE unorm8(float f) { return (BYTE)(f <= 0 ? 0 : f >= 1 ? 255 : (int)(f * 255.0f + 0.5f)); }

/* The bytes of a pixel of a format with a clear colour. */
static void clear_pixel(const Img *img, const VkClearColorValue *c, BYTE *out) {
  switch (img->dfmt) {
    case DF_A8R8G8B8: case DF_A8R8G8B8_SRGB: out[0] = unorm8(c->float32[2]), out[1] = unorm8(c->float32[1]), out[2] = unorm8(c->float32[0]), out[3] = unorm8(c->float32[3]); break;
    case DF_A8B8G8R8: case DF_A8B8G8R8_SRGB: out[0] = unorm8(c->float32[0]), out[1] = unorm8(c->float32[1]), out[2] = unorm8(c->float32[2]), out[3] = unorm8(c->float32[3]); break;
    case DF_RGBA8UI: for (int i = 0; i < 4; ++i) out[i] = (BYTE)c->uint32[i]; break;
    case DF_RGBA16F: for (int i = 0; i < 4; ++i) { USHORT h = (USHORT)float_to_half(c->float32[i]); memcpy(out + 2 * i, &h, 2); } break;
    case DF_RGBA32F: case DF_RGBA32UI: case DF_RGBA32SI: memcpy(out, c, 16); break;
    case DF_RGBA16: for (int i = 0; i < 4; ++i) { USHORT h = (USHORT)(c->float32[i] <= 0 ? 0 : c->float32[i] >= 1 ? 65535 : (int)(c->float32[i] * 65535.0f + 0.5f)); memcpy(out + 2 * i, &h, 2); } break;
    case DF_RGBA16UI: case DF_RGBA16SI: for (int i = 0; i < 4; ++i) { USHORT h = (USHORT)c->uint32[i]; memcpy(out + 2 * i, &h, 2); } break;
    default: memset(out, 0, img->bpp); break;
  }
}

static void exec_clear_color(const Img *img_c, const void *payload, UINT nranges) {
  Img *img = (Img *)img_c;
  const VkClearColorValue *color = payload;
  const VkImageSubresourceRange *ranges = (const VkImageSubresourceRange *)((const BYTE *)payload + sizeof(VkClearColorValue));
  BYTE pixel[16];
  clear_pixel(img, color, pixel);
  for (UINT r = 0; r < nranges; ++r) {
    UINT levels = ranges[r].levelCount == VK_REMAINING_MIP_LEVELS ? img->mips - ranges[r].baseMipLevel : ranges[r].levelCount;
    for (UINT l = ranges[r].baseMipLevel; l < ranges[r].baseMipLevel + levels && l < img->mips; ++l) {
      BYTE *p = kmt_lock(img->dev, img->alloc, l, LOCK_DISCARD);
      if (!p) continue;
      UINT layers = img->kind == 1 ? img->depth : img->kind == 3 ? 6 : img->kind == 2 ? img->layers : 1;
      ULONGLONG n = (ULONGLONG)level_extent(img->w, l) * level_extent(img->h, l) * layers;
      for (ULONGLONG i = 0; i < n; ++i) memcpy(p + i * img->bpp, pixel, img->bpp);
      kmt_unlock(img->dev, img->alloc);
    }
  }
}

static void run_cmds(Exec *x, CmdBuf *cb);

static void exec_one(Exec *x, const Cmd *c) {
  Device *d = x->d;
  switch (c->op) {
    case C_BEGIN_PASS: {
      x->rp = (RenderPass *)(ULONG_PTR)c->a;
      x->fb = (Framebuffer *)(ULONG_PTR)c->b;
      x->clears = c->p;
      x->nclears = c->n;
      x->subpass = 0;
      memset(x->attachment_used, 0, sizeof x->attachment_used);
      x->vp_set = x->sc_set = FALSE;
      if (!x->rp || !x->fb || x->rp->nsub == 0) break;
      for (UINT i = 0; i < x->fb->nview; ++i) image_sync_up(x->fb->view[i]->img);
      begin_stream_pass(x);
      break;
    }
    case C_NEXT_SUBPASS:
      if (!x->rp) break;
      end_stream_pass(x);
      resolve_subpass(x);
      if (x->subpass + 1 < x->rp->nsub) ++x->subpass;
      begin_stream_pass(x);
      break;
    case C_END_PASS:
      if (!x->rp) break;
      end_stream_pass(x);
      resolve_subpass(x);
      flush(x);
      x->rp = NULL;
      x->fb = NULL;
      break;
    case C_BIND_PIPE:
      x->pipe = (Pipe *)(ULONG_PTR)c->a;
      x->dirty_pipe = TRUE;
      break;
    case C_BIND_VB:
      if (c->a < 8) x->vb[c->a].b = (Buf *)(ULONG_PTR)c->b, x->vb[c->a].off = c->c;
      x->dirty_vb = TRUE;
      break;
    case C_BIND_IB:
      x->ib = (Buf *)(ULONG_PTR)c->a, x->ib_off = c->b, x->ib_type = (UINT)c->c;
      x->dirty_ib = TRUE;
      break;
    case C_BIND_SETS: {
      DescSet **sets = c->p;
      const UINT *dyn = (const UINT *)((BYTE *)c->p + sizeof(void *) * c->n);
      UINT used = 0;
      for (UINT i = 0; i < c->n && c->a + i < 8; ++i) {
        x->sets[c->a + i] = sets[i];
        UINT count = 0;
        for (UINT k = 0; sets[i] && k < sets[i]->layout->nb; ++k)
          if (sets[i]->layout->b[k].type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC) count += sets[i]->layout->b[k].count;
        for (UINT k = 0; k < count && k < 8 && used < c->b; ++k) x->dyn[c->a + i][k] = dyn[used++];
      }
      x->dirty_state = TRUE;
      break;
    }
    case C_PUSH:
      if (c->a + c->n <= sizeof x->push) memcpy(x->push + c->a, c->p, c->n);
      x->dirty_state = TRUE;
      break;
    case C_VIEWPORT:
      x->vp = *(const VkViewport *)c->p, x->vp_set = TRUE;
      break;
    case C_SCISSOR:
      x->sc = *(const VkRect2D *)c->p, x->sc_set = TRUE;
      break;
    case C_DRAW:
      do_draw(x, (UINT)c->a, (UINT)c->b, (UINT)c->c, 0, (UINT)c->d, FALSE);
      break;
    case C_DRAW_INDEXED:
      do_draw(x, (UINT)c->a, (UINT)c->b, (UINT)c->c, (int)(UINT)c->d, (UINT)c->e, TRUE);
      break;
    case C_COPY_BUF: {
      flush(x);
      Buf *src = (Buf *)(ULONG_PTR)c->a, *dst = (Buf *)(ULONG_PTR)c->b;
      const VkBufferCopy *r = c->p;
      if (!src->mem || !dst->mem) break;
      for (UINT i = 0; i < c->n; ++i)
        if (r[i].srcOffset + r[i].size <= src->size && r[i].dstOffset + r[i].size <= dst->size) memmove(dst->mem->data + dst->off + r[i].dstOffset, src->mem->data + src->off + r[i].srcOffset, (size_t)r[i].size);
      ++dst->mem->version;
      break;
    }
    case C_COPY_BUF2IMG: case C_COPY_IMG2BUF: {
      flush(x);
      BOOL to_image = c->op == C_COPY_BUF2IMG;
      Buf *buf = (Buf *)(ULONG_PTR)(to_image ? c->a : c->b);
      Img *img = (Img *)(ULONG_PTR)(to_image ? c->b : c->a);
      for (UINT i = 0; i < c->n; ++i) copy_buffer_image(x, &((const VkBufferImageCopy *)c->p)[i], buf, img, to_image);
      if (to_image) image_sync_down(img);
      break;
    }
    case C_COPY_IMG: {
      flush(x);
      for (UINT i = 0; i < c->n; ++i) exec_copy_image(&((const VkImageCopy *)c->p)[i], (Img *)(ULONG_PTR)c->a, (Img *)(ULONG_PTR)c->b);
      break;
    }
    case C_BLIT: {
      flush(x);
      for (UINT i = 0; i < c->n; ++i) exec_blit(&((const VkImageBlit *)c->p)[i], (Img *)(ULONG_PTR)c->a, (Img *)(ULONG_PTR)c->b);
      break;
    }
    case C_RESOLVE: {
      Img *src = (Img *)(ULONG_PTR)c->a, *dst = (Img *)(ULONG_PTR)c->b;
      int di = alloc_index(x, dst->alloc), si = alloc_index(x, src->alloc);
      if (di >= 0 && si >= 0) {
        UINT payload[2] = {(UINT)di, (UINT)si};
        packet(x, OP_RESOLVE, payload, 2);
        flush(x);
      }
      break;
    }
    case C_CLEAR_COLOR:
      flush(x);
      exec_clear_color((Img *)(ULONG_PTR)c->a, c->p, c->n);
      break;
    case C_CLEAR_DS: {
      /* A depth image has no CPU view: a pass that only clears it. */
      Img *img = (Img *)(ULONG_PTR)c->a;
      const VkClearDepthStencilValue *v = c->p;
      int idx = alloc_index(x, img->alloc);
      if (idx < 0) break;
      UINT payload[9] = {NO_ALLOC, (UINT)idx, 6, 0, 0, 0, 0, fbits(v->depth), v->stencil};
      packet(x, OP_BEGIN_PASS, payload, 9);
      packet(x, OP_END_PASS, NULL, 0);
      img->gpu_written = TRUE;
      flush(x);
      break;
    }
    case C_CLEAR_ATT: {
      if (!x->rp || !x->fb) break;
      const VkClearAttachment *a = c->p;
      const Subpass *sp = &x->rp->sub[x->subpass];
      UINT flags = 0;
      float color[4] = {0, 0, 0, 0}, depth = 1.0f;
      UINT stencil = 0;
      for (UINT i = 0; i < c->n; ++i) {
        if (a[i].aspectMask & VK_IMAGE_ASPECT_COLOR_BIT) {
          flags |= 1;
          for (int k = 0; k < 4; ++k) color[k] = a[i].clearValue.color.float32[k];
        }
        if (a[i].aspectMask & VK_IMAGE_ASPECT_DEPTH_BIT) flags |= 2, depth = a[i].clearValue.depthStencil.depth;
        if (a[i].aspectMask & VK_IMAGE_ASPECT_STENCIL_BIT) flags |= 4, stencil = a[i].clearValue.depthStencil.stencil;
      }
      /* The whole render area: the pass begins again, clearing, over what is in the images. */
      end_stream_pass(x);
      UINT colors[4] = {NO_ALLOC, NO_ALLOC, NO_ALLOC, NO_ALLOC}, depth_idx = NO_ALLOC, nextra = 0;
      for (UINT k = 0; k < sp->ncolor; ++k) {
        int idx = sp->color[k] < x->fb->nview ? alloc_index(x, x->fb->view[sp->color[k]]->img->alloc) : -1;
        colors[k] = idx < 0 ? NO_ALLOC : (UINT)idx;
        if (colors[k] != NO_ALLOC && k > 0) nextra = k;
      }
      if (sp->depth != 0xFFFFFFFFu && sp->depth < x->fb->nview) {
        int idx = alloc_index(x, x->fb->view[sp->depth]->img->alloc);
        depth_idx = idx < 0 ? NO_ALLOC : (UINT)idx;
      }
      UINT payload[12] = {colors[0], depth_idx, flags, fbits(color[0]), fbits(color[1]), fbits(color[2]), fbits(color[3]), fbits(depth), stencil, colors[1], colors[2], colors[3]};
      packet(x, OP_BEGIN_PASS, payload, 9 + nextra);
      x->stream_open = TRUE;
      x->dirty_pipe = x->dirty_state = x->dirty_vb = x->dirty_ib = TRUE;
      x->topology_sent = 0xFFFFFFFFu;
      break;
    }
    case C_FILL: {
      Buf *b = (Buf *)(ULONG_PTR)c->a;
      if (!b->mem || c->b >= b->size) break;
      ULONGLONG n = c->c == VK_WHOLE_SIZE ? b->size - c->b : MIN(c->c, b->size - c->b);
      UINT v = (UINT)c->d;
      for (ULONGLONG i = 0; i + 4 <= n; i += 4) memcpy(b->mem->data + b->off + c->b + i, &v, 4);
      ++b->mem->version;
      break;
    }
    case C_UPDATE: {
      Buf *b = (Buf *)(ULONG_PTR)c->a;
      if (!b->mem || c->b + c->c > b->size) break;
      memcpy(b->mem->data + b->off + c->b, c->p, (size_t)c->c);
      ++b->mem->version;
      break;
    }
    case C_EXEC:
      run_cmds(x, (CmdBuf *)(ULONG_PTR)c->a);
      break;
    default: break;
  }
  (void)d;
}

static void run_cmds(Exec *x, CmdBuf *cb) {
  for (UINT i = 0; i < cb->n; ++i) exec_one(x, &cb->cmds[i]);
}

static VkResult submit_cmdbuf(Device *d, CmdBuf *cb) {
  Exec *x = vk_alloc(sizeof(Exec));
  if (!x) return VK_ERROR_OUT_OF_HOST_MEMORY;
  x->d = d, x->caps = &d->pd->caps;
  x->dirty_pipe = x->dirty_state = TRUE;
  x->topology_sent = 0xFFFFFFFFu;
  run_cmds(x, cb);
  end_stream_pass(x);
  flush(x);
  for (UINT i = 0; i < x->ntemps; ++i) kmt_free(d, x->temps[i]);
  BOOL failed = x->failed || cb->failed;
  vk_free(x);
  return failed ? VK_ERROR_DEVICE_LOST : VK_SUCCESS;
}

VKAPI VkResult VKAPI_CALL vkQueueSubmit(VkQueue queue, uint32_t count, const VkSubmitInfo *submits, VkFence fence) {
  Queue *q = (Queue *)queue;
  VkResult result = VK_SUCCESS;
  for (UINT i = 0; i < count && result == VK_SUCCESS; ++i)
    for (UINT k = 0; k < submits[i].commandBufferCount && result == VK_SUCCESS; ++k) result = submit_cmdbuf(q->dev, (CmdBuf *)submits[i].pCommandBuffers[k]);
  if (fence) ((Fence *)fence)->signaled = TRUE;
  return result;
}
