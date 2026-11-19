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

/* Part of d3d11.c: the immediate context. Setting state only remembers it; a draw works out the Direct3D 12 pipeline of what is bound, sets on the
 * command list what changed since the last draw, and records the draw. See d3d11.c for what maps to what. */

/* ---- pipelines ------------------------------------------------------------------------------------------------------------------------------------- */

static const D3D11_RASTERIZER_DESC g_default_raster = {D3D11_FILL_SOLID, D3D11_CULL_BACK, FALSE, 0, 0.0f, 0.0f, TRUE, FALSE, FALSE, FALSE};
static const D3D11_DEPTH_STENCIL_DESC g_default_depth = {TRUE, D3D11_DEPTH_WRITE_MASK_ALL, D3D11_COMPARISON_LESS, FALSE, 0xFF, 0xFF,
                                                         {D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP, D3D11_COMPARISON_ALWAYS},
                                                         {D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP, D3D11_COMPARISON_ALWAYS}};
static D3D11_BLEND_DESC default_blend(void) {
  D3D11_BLEND_DESC b;
  memset(&b, 0, sizeof b);
  for (UINT i = 0; i < 8; ++i) {
    b.RenderTarget[i].SrcBlend = b.RenderTarget[i].SrcBlendAlpha = D3D11_BLEND_ONE;
    b.RenderTarget[i].DestBlend = b.RenderTarget[i].DestBlendAlpha = D3D11_BLEND_ZERO;
    b.RenderTarget[i].BlendOp = b.RenderTarget[i].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    b.RenderTarget[i].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
  }
  return b;
}

/* The Direct3D 12 pipeline state of what the context has bound; NULL when it cannot be made (the draw is then left out). */
static ID3D12PipelineState *pipeline_get(Dev *d, Ctx *c) {
  Pipeline key;
  memset(&key, 0, sizeof key);
  key.vs = c->vs->c.id;
  key.ps = c->ps ? c->ps->c.id : 0;
  key.layout = c->layout ? c->layout->c.id : 0;
  key.blend = c->blend ? c->blend->desc : default_blend();
  key.depth = c->depth ? c->depth->desc : g_default_depth;
  key.raster = c->raster ? c->raster->desc : g_default_raster;
  key.raster.ScissorEnable = 0;      /* (not the pipeline's: the scissor rectangle is set with the viewport) */
  key.targets = c->rtv_count;
  key.has_depth = c->dsv != NULL;
  key.samples = 1;
  for (UINT i = 0; i < c->rtv_count && i < 8; ++i) {
    key.formats[i] = c->rtv[i]->tex->desc.Format;      /* (as the program made it: an R8 target is kept as RGBA8 but masked) */
    if (c->rtv[i]->tex->desc.SampleDesc.Count > 1) key.samples = c->rtv[i]->tex->desc.SampleDesc.Count;
  }
  if (c->dsv) {
    key.depth_format = c->dsv->tex->stored;
    if (c->dsv->tex->desc.SampleDesc.Count > 1) key.samples = c->dsv->tex->desc.SampleDesc.Count;
  }
  if (!c->dsv) { key.depth.DepthEnable = FALSE; key.depth.StencilEnable = FALSE; }
  Pipeline *oldest = &d->pipelines[0];
  for (UINT i = 0; i < d->pipeline_count; ++i) {
    Pipeline *p = &d->pipelines[i];
    if (p->vs == key.vs && p->ps == key.ps && p->layout == key.layout && p->targets == key.targets && p->has_depth == key.has_depth && p->samples == key.samples &&
        memcmp(p->formats, key.formats, sizeof key.formats) == 0 && p->depth_format == key.depth_format &&
        memcmp(&p->blend, &key.blend, sizeof key.blend) == 0 && memcmp(&p->depth, &key.depth, sizeof key.depth) == 0 && memcmp(&p->raster, &key.raster, sizeof key.raster) == 0) {
      p->last_used = ++d->pipeline_clock;
      return p->state;
    }
    if (p->last_used < oldest->last_used) oldest = p;
  }

  D3D12_GRAPHICS_PIPELINE_STATE_DESC pd;
  memset(&pd, 0, sizeof pd);
  pd.pRootSignature = d->root;
  pd.VS.pShaderBytecode = c->vs->code;
  pd.VS.BytecodeLength = c->vs->bytes;
  if (c->ps) { pd.PS.pShaderBytecode = c->ps->code; pd.PS.BytecodeLength = c->ps->bytes; }
  pd.SampleMask = 0xFFFFFFFFu;
  pd.BlendState.AlphaToCoverageEnable = key.blend.AlphaToCoverageEnable;
  pd.BlendState.IndependentBlendEnable = key.blend.IndependentBlendEnable;
  for (UINT i = 0; i < 8; ++i) {
    const D3D11_RENDER_TARGET_BLEND_DESC *in = &key.blend.RenderTarget[i];
    D3D12_RENDER_TARGET_BLEND_DESC *out = &pd.BlendState.RenderTarget[i];
    out->BlendEnable = in->BlendEnable;
    out->SrcBlend = (D3D12_BLEND)in->SrcBlend;
    out->DestBlend = (D3D12_BLEND)in->DestBlend;
    out->BlendOp = (D3D12_BLEND_OP)in->BlendOp;
    out->SrcBlendAlpha = (D3D12_BLEND)in->SrcBlendAlpha;
    out->DestBlendAlpha = (D3D12_BLEND)in->DestBlendAlpha;
    out->BlendOpAlpha = (D3D12_BLEND_OP)in->BlendOpAlpha;
    out->LogicOp = D3D12_LOGIC_OP_NOOP;
    out->RenderTargetWriteMask = in->RenderTargetWriteMask;
  }
  for (UINT i = 0; i < c->rtv_count && i < 8; ++i) {      /* a target kept wider than its format: only its own channels are written */
    const int e = c->rtv[i]->tex->expand;
    if (!e) continue;
    if (!pd.BlendState.IndependentBlendEnable) {
      for (UINT j = 1; j < 8; ++j) pd.BlendState.RenderTarget[j] = pd.BlendState.RenderTarget[0];
      pd.BlendState.IndependentBlendEnable = TRUE;
    }
    pd.BlendState.RenderTarget[i].RenderTargetWriteMask &= (UINT8)(e == 1 ? D3D12_COLOR_WRITE_ENABLE_RED : e == 3 ? (D3D12_COLOR_WRITE_ENABLE_RED | D3D12_COLOR_WRITE_ENABLE_GREEN)
                                                                                             : D3D12_COLOR_WRITE_ENABLE_ALPHA);
  }
  pd.RasterizerState.FillMode = (D3D12_FILL_MODE)key.raster.FillMode;
  pd.RasterizerState.CullMode = (D3D12_CULL_MODE)key.raster.CullMode;
  pd.RasterizerState.FrontCounterClockwise = key.raster.FrontCounterClockwise;
  pd.RasterizerState.DepthBias = key.raster.DepthBias;
  pd.RasterizerState.DepthBiasClamp = key.raster.DepthBiasClamp;
  pd.RasterizerState.SlopeScaledDepthBias = key.raster.SlopeScaledDepthBias;
  pd.RasterizerState.DepthClipEnable = key.raster.DepthClipEnable;
  pd.DepthStencilState.DepthEnable = key.depth.DepthEnable;
  pd.DepthStencilState.DepthWriteMask = (D3D12_DEPTH_WRITE_MASK)key.depth.DepthWriteMask;
  pd.DepthStencilState.DepthFunc = (D3D12_COMPARISON_FUNC)key.depth.DepthFunc;
  pd.DepthStencilState.StencilEnable = key.depth.StencilEnable;
  pd.DepthStencilState.StencilReadMask = key.depth.StencilReadMask;
  pd.DepthStencilState.StencilWriteMask = key.depth.StencilWriteMask;
  memcpy(&pd.DepthStencilState.FrontFace, &key.depth.FrontFace, sizeof pd.DepthStencilState.FrontFace);      /* (the two structures are the same) */
  memcpy(&pd.DepthStencilState.BackFace, &key.depth.BackFace, sizeof pd.DepthStencilState.BackFace);
  if (c->layout) { pd.InputLayout.pInputElementDescs = c->layout->elements; pd.InputLayout.NumElements = c->layout->count; }
  pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  pd.NumRenderTargets = c->rtv_count;
  for (UINT i = 0; i < c->rtv_count; ++i) pd.RTVFormats[i] = c->rtv[i]->tex->stored;
  if (c->dsv) pd.DSVFormat = c->dsv->tex->stored == DXGI_FORMAT_R24G8_TYPELESS ? DXGI_FORMAT_D24_UNORM_S8_UINT
                           : c->dsv->tex->stored == DXGI_FORMAT_R32_TYPELESS ? DXGI_FORMAT_D32_FLOAT : c->dsv->tex->stored;
  pd.SampleDesc.Count = key.samples;
  ID3D12PipelineState *state = NULL;
  TcosD3D12PipelineOptions(d->d12, c->vs->registers, 0);      /* (the pixel shader's constant buffers come after the vertex shader's) */
  if (FAILED(ID3D12Device_CreateGraphicsPipelineState(d->d12, &pd, &IID_ID3D12PipelineState, (void **)&state)) || !state) return NULL;

  Pipeline *slot = d->pipeline_count < PIPELINES ? &d->pipelines[d->pipeline_count++] : oldest;
  if (slot == oldest && slot->state) {
    flush(d);      /* (a recorded draw may use the one that goes) */
    ID3D12PipelineState_Release(slot->state);
  }
  *slot = key;
  slot->state = state;
  slot->last_used = ++d->pipeline_clock;
  return state;
}

/* ---- what a draw needs ------------------------------------------------------------------------------------------------------------------------------ */

/* The buffer of slot `slot` as the pipeline reads it: the buffer itself, or its copy with every element as floats. */
static ID3D12Resource *vertex_source(Dev *d, Ctx *c, UINT slot, UINT *stride, UINT *offset, UINT *bytes) {
  Buf *b = c->vb[slot];
  const UINT out_stride = c->layout ? c->layout->slot_converted[slot] : 0;
  *stride = c->vb_stride[slot];
  *offset = c->vb_offset[slot];
  *bytes = b->desc.ByteWidth > *offset ? b->desc.ByteWidth - *offset : 0;
  if (!out_stride || *stride == 0) return b->res;
  const Layout *l = c->layout;
  const UINT vertices = *bytes / *stride;
  if (!b->converted || b->converted_layout != l->c.id || b->converted_version != b->version || b->converted_stride != *stride || b->converted_offset != *offset) {
    if (b->converted && b->used == d->epoch) flush(d);      /* (a recorded draw reads the copy that goes) */
    if (b->converted && (b->converted_vertices < vertices || b->converted_out_stride != out_stride)) { ID3D12Resource_Release(b->converted); b->converted = NULL; }
    if (!b->converted) b->converted = make_buffer(d, D3D12_HEAP_TYPE_UPLOAD, (UINT64)(vertices ? vertices : 1) * out_stride);
    BYTE *out = NULL;
    if (!b->converted || FAILED(ID3D12Resource_Map(b->converted, 0, NULL, (void **)&out)) || !out) return NULL;
    for (UINT v = 0; v < vertices; ++v) {
      const BYTE *in = b->data + *offset + (SIZE_T)v * *stride;
      for (UINT i = 0; i < l->count; ++i) {
        if (l->elements[i].InputSlot != slot) continue;
        FLOAT *to = (FLOAT *)(out + (SIZE_T)v * out_stride + l->elements[i].AlignedByteOffset);
        const BYTE *from = in + l->source_offset[i];
        int convert;
        const UINT floats = element_floats(l->source_format[i], &convert);
        if (l->source_offset[i] + (convert ? 4 : floats * 4) > *stride) continue;
        if (!convert) memcpy(to, from, floats * 4);
        else if (l->source_format[i] == DXGI_FORMAT_B8G8R8A8_UNORM) { to[0] = from[2] / 255.0f; to[1] = from[1] / 255.0f; to[2] = from[0] / 255.0f; to[3] = from[3] / 255.0f; }
        else for (UINT k = 0; k < 4; ++k) to[k] = from[k] / 255.0f;
      }
    }
    ID3D12Resource_Unmap(b->converted, 0, NULL);
    b->converted_layout = l->c.id;
    b->converted_version = b->version;
    b->converted_stride = *stride;
    b->converted_offset = *offset;
    b->converted_vertices = vertices;
    b->converted_out_stride = out_stride;
  }
  *stride = out_stride;
  *offset = 0;
  *bytes = vertices * out_stride;
  return b->converted;
}

static void record_pending_clears(Dev *d) {
  for (UINT i = 0; i < d->clear_count; ++i) {
    const PendingClear *p = &d->clears[i];
    ID3D12GraphicsCommandList_ClearDepthStencilView(d->list, p->dsv->handle, (D3D12_CLEAR_FLAGS)p->flags, p->depth, p->stencil, 0, NULL);
    d->recorded = 1;
  }
}

/* Everything of a draw but the draw: FALSE when it cannot be drawn (no vertex shader, no render target, a pipeline the kernel does not take). */
static BOOL draw_prepare(Ctx *c, BOOL indexed) {
  Dev *d = c->dev;
  if (!c->vs || c->rtv_count == 0 || (indexed && !c->ib)) return FALSE;

  /* what may have to send the list first */
  ID3D12Resource *sources[VERTEX_SLOTS] = {0};
  UINT strides[VERTEX_SLOTS], offsets[VERTEX_SLOTS], sizes[VERTEX_SLOTS];
  for (UINT slot = 0; slot < VERTEX_SLOTS; ++slot) {
    if (!c->vb[slot]) continue;
    sources[slot] = vertex_source(d, c, slot, &strides[slot], &offsets[slot], &sizes[slot]);
    if (!sources[slot]) return FALSE;
  }
  /* (a texture the list's draws sampled cannot become its target: the kernel keeps it bound as a texture for the whole list) */
  for (UINT i = 0; i < c->rtv_count; ++i) settle(d, c->rtv[i]->tex->sampled);
  if (d->ring_used + CONSTANT_BLOCK > RING_BYTES || d->view_used + TEXTURE_SLOTS > VIEW_HEAP || d->sampler_used + TEXTURE_SLOTS > VIEW_HEAP) flush(d);
  ID3D12PipelineState *pipeline = pipeline_get(d, c);
  if (!pipeline) return FALSE;

  ID3D12GraphicsCommandList *list = d->list;
  if (d->dirty & DIRTY_ROOT) {
    ID3D12DescriptorHeap *heaps[2] = {d->view_heap, d->sampler_heap};
    ID3D12GraphicsCommandList_SetGraphicsRootSignature(list, d->root);
    ID3D12GraphicsCommandList_SetDescriptorHeaps(list, 2, heaps);
  }
  if (d->dirty & DIRTY_TARGETS) {
    D3D12_CPU_DESCRIPTOR_HANDLE targets[8];
    for (UINT i = 0; i < c->rtv_count; ++i) targets[i] = c->rtv[i]->handle;
    ID3D12GraphicsCommandList_OMSetRenderTargets(list, c->rtv_count, targets, FALSE, c->dsv ? &c->dsv->handle : NULL);
  }
  if (d->dirty & (DIRTY_VIEWPORT | DIRTY_TARGETS)) {
    const Tex *target = c->rtv[0]->tex;
    D3D12_VIEWPORT vp = {0, 0, (FLOAT)target->desc.Width, (FLOAT)target->desc.Height, 0, 1};
    D3D12_RECT sc = {0, 0, (LONG)target->desc.Width, (LONG)target->desc.Height};
    if (c->viewport_set) memcpy(&vp, &c->viewport, sizeof vp);      /* (the two structures are the same) */
    if (c->raster && c->raster->desc.ScissorEnable && c->scissor_set) memcpy(&sc, &c->scissor, sizeof sc);
    ID3D12GraphicsCommandList_RSSetViewports(list, 1, &vp);
    ID3D12GraphicsCommandList_RSSetScissorRects(list, 1, &sc);
  }
  ID3D12GraphicsCommandList_SetPipelineState(list, pipeline);
  ID3D12GraphicsCommandList_IASetPrimitiveTopology(list, (D3D12_PRIMITIVE_TOPOLOGY)c->topology);
  ID3D12GraphicsCommandList_OMSetStencilRef(list, c->stencil_ref);
  for (UINT slot = 0; slot < VERTEX_SLOTS; ++slot) {
    if (!sources[slot]) continue;
    D3D12_VERTEX_BUFFER_VIEW view = {ID3D12Resource_GetGPUVirtualAddress(sources[slot]) + offsets[slot], sizes[slot], strides[slot]};
    ID3D12GraphicsCommandList_IASetVertexBuffers(list, slot, 1, &view);
    c->vb[slot]->used = d->epoch;
  }
  if (indexed) {
    const UINT offset = c->ib_offset < c->ib->desc.ByteWidth ? c->ib_offset : 0;
    D3D12_INDEX_BUFFER_VIEW view = {ID3D12Resource_GetGPUVirtualAddress(c->ib->res) + offset, c->ib->desc.ByteWidth - offset,
                                    c->ib_format == DXGI_FORMAT_R32_UINT ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_R16_UINT};
    ID3D12GraphicsCommandList_IASetIndexBuffer(list, &view);
    c->ib->used = d->epoch;
  }

  /* the constants: the vertex shader's buffers in slot order, then the pixel shader's */
  BYTE block[CONSTANT_BLOCK];
  UINT bytes = 0;
  for (UINT stage = 0; stage < 2; ++stage) {
    const Shader *s = stage == 0 ? c->vs : c->ps;
    Buf *const *bound = stage == 0 ? c->vs_cb : c->ps_cb;
    for (UINT slot = 0; s && slot < CONSTANT_SLOTS; ++slot) {
      const UINT size = s->cb_registers[slot] * 16;
      if (!size || bytes + size > CONSTANT_BLOCK) continue;
      const UINT have = bound[slot] ? (bound[slot]->desc.ByteWidth < size ? bound[slot]->desc.ByteWidth : size) : 0;
      if (have) memcpy(block + bytes, bound[slot]->data, have);
      memset(block + bytes + have, 0, size - have);
      bytes += size;
    }
  }
  if (bytes == 0) { memset(block, 0, 16); bytes = 16; }
  if (d->ring_last_bytes != bytes || memcmp(d->ring_data + d->ring_last, block, bytes) != 0) {
    d->ring_last = d->ring_used;
    d->ring_last_bytes = bytes;
    memcpy(d->ring_data + d->ring_used, block, bytes);
    d->ring_used += (bytes + 255u) & ~255u;
  }
  ID3D12GraphicsCommandList_SetGraphicsRootConstantBufferView(list, 0, ID3D12Resource_GetGPUVirtualAddress(d->ring) + d->ring_last);

  /* the textures and the samplers of the pixel shader: a table of each, made again when what is bound changed */
  UINT view_ids[TEXTURE_SLOTS], sampler_ids[TEXTURE_SLOTS];
  for (UINT i = 0; i < TEXTURE_SLOTS; ++i) {
    view_ids[i] = c->srv[i] ? c->srv[i]->tex->c.id : 0;
    sampler_ids[i] = c->sampler[i] ? c->sampler[i]->c.id : 0;
  }
  const UINT step_views = ID3D12Device_GetDescriptorHandleIncrementSize(d->d12, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  const UINT step_samplers = ID3D12Device_GetDescriptorHandleIncrementSize(d->d12, D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
  if (!d->tables_valid || memcmp(view_ids, d->table_view_ids, sizeof view_ids) != 0) {
    D3D12_CPU_DESCRIPTOR_HANDLE at = ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(d->view_heap);
    D3D12_GPU_DESCRIPTOR_HANDLE gpu = ID3D12DescriptorHeap_GetGPUDescriptorHandleForHeapStart(d->view_heap);
    at.ptr += (SIZE_T)d->view_used * step_views;
    d->table_views = gpu.ptr + (UINT64)d->view_used * step_views;
    for (UINT i = 0; i < TEXTURE_SLOTS; ++i) {
      Tex *t = c->srv[i] ? c->srv[i]->tex : d->blank;
      ID3D12Device_CreateShaderResourceView(d->d12, t->res, NULL, at);
      at.ptr += step_views;
    }
    d->view_used += TEXTURE_SLOTS;
    memcpy(d->table_view_ids, view_ids, sizeof view_ids);
  }
  if (!d->tables_valid || memcmp(sampler_ids, d->table_sampler_ids, sizeof sampler_ids) != 0) {
    static const D3D11_SAMPLER_DESC fallback = {D3D11_FILTER_MIN_MAG_MIP_LINEAR, D3D11_TEXTURE_ADDRESS_CLAMP, D3D11_TEXTURE_ADDRESS_CLAMP, D3D11_TEXTURE_ADDRESS_CLAMP, 0.0f, 1,
                                                D3D11_COMPARISON_NEVER, {0, 0, 0, 0}, 0.0f, 3.402823466e+38f};
    D3D12_CPU_DESCRIPTOR_HANDLE at = ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(d->sampler_heap);
    D3D12_GPU_DESCRIPTOR_HANDLE gpu = ID3D12DescriptorHeap_GetGPUDescriptorHandleForHeapStart(d->sampler_heap);
    at.ptr += (SIZE_T)d->sampler_used * step_samplers;
    d->table_samplers = gpu.ptr + (UINT64)d->sampler_used * step_samplers;
    for (UINT i = 0; i < TEXTURE_SLOTS; ++i) {
      ID3D12Device_CreateSampler(d->d12, (const D3D12_SAMPLER_DESC *)(c->sampler[i] ? &c->sampler[i]->desc : &fallback), at);      /* (the two structures are the same) */
      at.ptr += step_samplers;
    }
    d->sampler_used += TEXTURE_SLOTS;
    memcpy(d->table_sampler_ids, sampler_ids, sizeof sampler_ids);
  }
  d->tables_valid = 1;
  D3D12_GPU_DESCRIPTOR_HANDLE views = {d->table_views}, samplers = {d->table_samplers};
  ID3D12GraphicsCommandList_SetGraphicsRootDescriptorTable(list, 1, views);
  ID3D12GraphicsCommandList_SetGraphicsRootDescriptorTable(list, 2, samplers);

  for (UINT i = 0; i < TEXTURE_SLOTS; ++i) if (c->srv[i]) c->srv[i]->tex->used = c->srv[i]->tex->sampled = d->epoch;
  for (UINT i = 0; i < c->rtv_count; ++i) c->rtv[i]->tex->used = d->epoch;
  if (c->dsv) {
    c->dsv->tex->used = d->epoch;
    for (UINT i = 0; i < d->clear_count;) {      /* (its clear is done by the pass this draw begins) */
      if (d->clears[i].dsv == c->dsv) d->clears[i] = d->clears[--d->clear_count]; else ++i;
    }
  }
  d->dirty = 0;
  d->recorded = 1;
  return TRUE;
}

/* ---- the context's own methods --------------------------------------------------------------------------------------------------------------------- */

#define LOCK(c) EnterCriticalSection(&(c)->dev->lock)
#define UNLOCK(c) LeaveCriticalSection(&(c)->dev->lock)

/* The device holds the context (one reference); the first reference a program takes holds the device, the last one it gives back lets it go. */
static ULONG STDMETHODCALLTYPE ctx_AddRef(Ctx *c) {
  const LONG n = InterlockedIncrement(&c->o.ref);
  if (n == 2) dx_AddRef(OBJ(c->dev));
  return (ULONG)(n - 1);
}
static ULONG STDMETHODCALLTYPE ctx_Release(Ctx *c) {
  const LONG n = InterlockedDecrement(&c->o.ref);
  if (n == 1) dx_Release(OBJ(c->dev));      /* (may be the device's last reference: the context goes with it) */
  return (ULONG)(n - 1);
}
static HRESULT STDMETHODCALLTYPE ctx_QueryInterface(Ctx *c, REFIID riid, void **out) {
  if (!out) return E_POINTER;
  *out = NULL;
  if (!guid_eq(riid, &IID_IUnknown) && !guid_eq(riid, &IID_ID3D11DeviceChild) && !guid_eq(riid, &IID_ID3D11DeviceContext)) {
    dx_no_interface("ID3D11DeviceContext", riid);
    return E_NOINTERFACE;
  }
  ctx_AddRef(c);
  *out = c;
  return S_OK;
}
static void STDMETHODCALLTYPE ctx_GetDevice(Ctx *c, ID3D11Device **out) {
  if (!out) return;
  dx_AddRef(OBJ(c->dev));
  *out = (ID3D11Device *)c->dev;
}
static D3D11_DEVICE_CONTEXT_TYPE STDMETHODCALLTYPE ctx_GetType(Ctx *c) { (void)c; return D3D11_DEVICE_CONTEXT_IMMEDIATE; }
static UINT STDMETHODCALLTYPE ctx_GetContextFlags(Ctx *c) { (void)c; return 0; }

static void STDMETHODCALLTYPE ctx_VSSetShader(Ctx *c, ID3D11VertexShader *shader, ID3D11ClassInstance *const *instances, UINT count) {
  (void)instances; (void)count;
  bind(&c->vs, shader);
}
static void STDMETHODCALLTYPE ctx_PSSetShader(Ctx *c, ID3D11PixelShader *shader, ID3D11ClassInstance *const *instances, UINT count) {
  (void)instances; (void)count;
  bind(&c->ps, shader);
}
static void set_range(void **slots, UINT capacity, UINT start, UINT count, void *const *objects) {
  for (UINT i = 0; i < count && start + i < capacity; ++i) bind(&slots[start + i], objects ? objects[i] : NULL);
}
static void get_range(void **slots, UINT capacity, UINT start, UINT count, void **objects) {
  for (UINT i = 0; objects && i < count; ++i) {
    objects[i] = start + i < capacity ? slots[start + i] : NULL;
    if (objects[i]) dx_AddRef(OBJ(objects[i]));
  }
}
static void STDMETHODCALLTYPE ctx_VSSetConstantBuffers(Ctx *c, UINT start, UINT count, ID3D11Buffer *const *buffers) { set_range((void **)c->vs_cb, CONSTANT_SLOTS, start, count, (void *const *)buffers); }
static void STDMETHODCALLTYPE ctx_PSSetConstantBuffers(Ctx *c, UINT start, UINT count, ID3D11Buffer *const *buffers) { set_range((void **)c->ps_cb, CONSTANT_SLOTS, start, count, (void *const *)buffers); }
static void STDMETHODCALLTYPE ctx_PSSetShaderResources(Ctx *c, UINT start, UINT count, ID3D11ShaderResourceView *const *views) { set_range((void **)c->srv, TEXTURE_SLOTS, start, count, (void *const *)views); }
static void STDMETHODCALLTYPE ctx_PSSetSamplers(Ctx *c, UINT start, UINT count, ID3D11SamplerState *const *samplers) { set_range((void **)c->sampler, TEXTURE_SLOTS, start, count, (void *const *)samplers); }
static void STDMETHODCALLTYPE ctx_VSGetConstantBuffers(Ctx *c, UINT start, UINT count, ID3D11Buffer **buffers) { get_range((void **)c->vs_cb, CONSTANT_SLOTS, start, count, (void **)buffers); }
static void STDMETHODCALLTYPE ctx_PSGetConstantBuffers(Ctx *c, UINT start, UINT count, ID3D11Buffer **buffers) { get_range((void **)c->ps_cb, CONSTANT_SLOTS, start, count, (void **)buffers); }
static void STDMETHODCALLTYPE ctx_PSGetShaderResources(Ctx *c, UINT start, UINT count, ID3D11ShaderResourceView **views) { get_range((void **)c->srv, TEXTURE_SLOTS, start, count, (void **)views); }
static void STDMETHODCALLTYPE ctx_PSGetSamplers(Ctx *c, UINT start, UINT count, ID3D11SamplerState **samplers) { get_range((void **)c->sampler, TEXTURE_SLOTS, start, count, (void **)samplers); }
static void get_shader(void *shader, void **out, ID3D11ClassInstance **instances, UINT *count) {
  (void)instances;
  if (count) *count = 0;
  if (!out) return;
  *out = shader;
  if (shader) dx_AddRef(OBJ(shader));
}
static void STDMETHODCALLTYPE ctx_VSGetShader(Ctx *c, ID3D11VertexShader **out, ID3D11ClassInstance **instances, UINT *count) { get_shader(c->vs, (void **)out, instances, count); }
static void STDMETHODCALLTYPE ctx_PSGetShader(Ctx *c, ID3D11PixelShader **out, ID3D11ClassInstance **instances, UINT *count) { get_shader(c->ps, (void **)out, instances, count); }
/* (the stages this runtime has not: nothing is bound to them) */
static void STDMETHODCALLTYPE ctx_GSGetShader(Ctx *c, ID3D11GeometryShader **out, ID3D11ClassInstance **instances, UINT *count) { (void)c; get_shader(NULL, (void **)out, instances, count); }
static void STDMETHODCALLTYPE ctx_HSGetShader(Ctx *c, ID3D11HullShader **out, ID3D11ClassInstance **instances, UINT *count) { (void)c; get_shader(NULL, (void **)out, instances, count); }
static void STDMETHODCALLTYPE ctx_DSGetShader(Ctx *c, ID3D11DomainShader **out, ID3D11ClassInstance **instances, UINT *count) { (void)c; get_shader(NULL, (void **)out, instances, count); }
static void STDMETHODCALLTYPE ctx_CSGetShader(Ctx *c, ID3D11ComputeShader **out, ID3D11ClassInstance **instances, UINT *count) { (void)c; get_shader(NULL, (void **)out, instances, count); }

static void STDMETHODCALLTYPE ctx_IASetInputLayout(Ctx *c, ID3D11InputLayout *layout) { bind(&c->layout, layout); }
static void STDMETHODCALLTYPE ctx_IAGetInputLayout(Ctx *c, ID3D11InputLayout **out) { get_shader(c->layout, (void **)out, NULL, NULL); }
static void STDMETHODCALLTYPE ctx_IASetVertexBuffers(Ctx *c, UINT start, UINT count, ID3D11Buffer *const *buffers, const UINT *strides, const UINT *offsets) {
  for (UINT i = 0; i < count && start + i < VERTEX_SLOTS; ++i) {
    bind(&c->vb[start + i], buffers ? buffers[i] : NULL);
    c->vb_stride[start + i] = strides ? strides[i] : 0;
    c->vb_offset[start + i] = offsets ? offsets[i] : 0;
  }
}
static void STDMETHODCALLTYPE ctx_IAGetVertexBuffers(Ctx *c, UINT start, UINT count, ID3D11Buffer **buffers, UINT *strides, UINT *offsets) {
  get_range((void **)c->vb, VERTEX_SLOTS, start, count, (void **)buffers);
  for (UINT i = 0; i < count; ++i) {
    if (strides) strides[i] = start + i < VERTEX_SLOTS ? c->vb_stride[start + i] : 0;
    if (offsets) offsets[i] = start + i < VERTEX_SLOTS ? c->vb_offset[start + i] : 0;
  }
}
static void STDMETHODCALLTYPE ctx_IASetIndexBuffer(Ctx *c, ID3D11Buffer *buffer, DXGI_FORMAT format, UINT offset) {
  bind(&c->ib, buffer);
  c->ib_format = format;
  c->ib_offset = offset;
}
static void STDMETHODCALLTYPE ctx_IAGetIndexBuffer(Ctx *c, ID3D11Buffer **buffer, DXGI_FORMAT *format, UINT *offset) {
  get_shader(c->ib, (void **)buffer, NULL, NULL);
  if (format) *format = c->ib_format;
  if (offset) *offset = c->ib_offset;
}
static void STDMETHODCALLTYPE ctx_IASetPrimitiveTopology(Ctx *c, D3D11_PRIMITIVE_TOPOLOGY topology) { c->topology = topology; }
static void STDMETHODCALLTYPE ctx_IAGetPrimitiveTopology(Ctx *c, D3D11_PRIMITIVE_TOPOLOGY *topology) { if (topology) *topology = c->topology; }

static void STDMETHODCALLTYPE ctx_OMSetRenderTargets(Ctx *c, UINT count, ID3D11RenderTargetView *const *views, ID3D11DepthStencilView *depth) {
  UINT n = 0;
  while (views && n < count && n < 8 && views[n]) ++n;      /* (up to the first that is not there: the kernel's targets have no gaps) */
  for (UINT i = 0; i < 8; ++i) bind(&c->rtv[i], i < n ? views[i] : NULL);
  c->rtv_count = n;
  bind(&c->dsv, depth);
  c->dev->dirty |= DIRTY_TARGETS | DIRTY_VIEWPORT;
}
static void STDMETHODCALLTYPE ctx_OMGetRenderTargets(Ctx *c, UINT count, ID3D11RenderTargetView **views, ID3D11DepthStencilView **depth) {
  get_range((void **)c->rtv, 8, 0, count, (void **)views);
  get_shader(c->dsv, (void **)depth, NULL, NULL);
}
static void STDMETHODCALLTYPE ctx_OMSetBlendState(Ctx *c, ID3D11BlendState *state, const FLOAT factor[4], UINT sample_mask) {
  bind(&c->blend, state);
  for (UINT i = 0; i < 4; ++i) c->blend_factor[i] = factor ? factor[i] : 1.0f;
  c->sample_mask = sample_mask;
}
static void STDMETHODCALLTYPE ctx_OMGetBlendState(Ctx *c, ID3D11BlendState **state, FLOAT factor[4], UINT *sample_mask) {
  get_shader(c->blend, (void **)state, NULL, NULL);
  if (factor) memcpy(factor, c->blend_factor, sizeof c->blend_factor);
  if (sample_mask) *sample_mask = c->sample_mask;
}
static void STDMETHODCALLTYPE ctx_OMSetDepthStencilState(Ctx *c, ID3D11DepthStencilState *state, UINT stencil_ref) {
  bind(&c->depth, state);
  c->stencil_ref = stencil_ref;
}
static void STDMETHODCALLTYPE ctx_OMGetDepthStencilState(Ctx *c, ID3D11DepthStencilState **state, UINT *stencil_ref) {
  get_shader(c->depth, (void **)state, NULL, NULL);
  if (stencil_ref) *stencil_ref = c->stencil_ref;
}
static void STDMETHODCALLTYPE ctx_RSSetState(Ctx *c, ID3D11RasterizerState *state) {
  bind(&c->raster, state);
  c->dev->dirty |= DIRTY_VIEWPORT;      /* (whether the scissor rectangle is used) */
}
static void STDMETHODCALLTYPE ctx_RSGetState(Ctx *c, ID3D11RasterizerState **state) { get_shader(c->raster, (void **)state, NULL, NULL); }
static void STDMETHODCALLTYPE ctx_RSSetViewports(Ctx *c, UINT count, const D3D11_VIEWPORT *viewports) {
  c->viewport_set = count && viewports;
  if (c->viewport_set) c->viewport = viewports[0];
  c->dev->dirty |= DIRTY_VIEWPORT;
}
static void STDMETHODCALLTYPE ctx_RSGetViewports(Ctx *c, UINT *count, D3D11_VIEWPORT *viewports) {
  if (!count) return;
  if (viewports && *count && c->viewport_set) viewports[0] = c->viewport;
  *count = c->viewport_set ? 1 : 0;
}
static void STDMETHODCALLTYPE ctx_RSSetScissorRects(Ctx *c, UINT count, const D3D11_RECT *rects) {
  c->scissor_set = count && rects;
  if (c->scissor_set) c->scissor = rects[0];
  c->dev->dirty |= DIRTY_VIEWPORT;
}
static void STDMETHODCALLTYPE ctx_RSGetScissorRects(Ctx *c, UINT *count, D3D11_RECT *rects) {
  if (!count) return;
  if (rects && *count && c->scissor_set) rects[0] = c->scissor;
  *count = c->scissor_set ? 1 : 0;
}

static void STDMETHODCALLTYPE ctx_Draw(Ctx *c, UINT count, UINT start) {
  LOCK(c);
  if (draw_prepare(c, FALSE)) ID3D12GraphicsCommandList_DrawInstanced(c->dev->list, count, 1, start, 0);
  UNLOCK(c);
}
static void STDMETHODCALLTYPE ctx_DrawIndexed(Ctx *c, UINT count, UINT start, INT base_vertex) {
  LOCK(c);
  if (draw_prepare(c, TRUE)) ID3D12GraphicsCommandList_DrawIndexedInstanced(c->dev->list, count, 1, start, base_vertex, 0);
  UNLOCK(c);
}
static void STDMETHODCALLTYPE ctx_DrawInstanced(Ctx *c, UINT count, UINT instances, UINT start, UINT start_instance) {
  LOCK(c);
  if (draw_prepare(c, FALSE)) ID3D12GraphicsCommandList_DrawInstanced(c->dev->list, count, instances, start, start_instance);
  UNLOCK(c);
}
static void STDMETHODCALLTYPE ctx_DrawIndexedInstanced(Ctx *c, UINT count, UINT instances, UINT start, INT base_vertex, UINT start_instance) {
  LOCK(c);
  if (draw_prepare(c, TRUE)) ID3D12GraphicsCommandList_DrawIndexedInstanced(c->dev->list, count, instances, start, base_vertex, start_instance);
  UNLOCK(c);
}

static void STDMETHODCALLTYPE ctx_ClearRenderTargetView(Ctx *c, ID3D11RenderTargetView *view, const FLOAT colour[4]) {
  View *v = (View *)view;
  if (!v || !colour) return;
  LOCK(c);
  FLOAT kept[4] = {colour[0], colour[1], colour[2], colour[3]};      /* (a target kept wider than its format: the channels it does not have stay 0 and 1) */
  if (v->tex->expand == 1) kept[1] = kept[2] = 0, kept[3] = 1;
  else if (v->tex->expand == 3) kept[2] = 0, kept[3] = 1;
  else if (v->tex->expand == 2) kept[0] = kept[1] = kept[2] = 0;
  ID3D12GraphicsCommandList_ClearRenderTargetView(c->dev->list, v->handle, kept, 0, NULL);
  v->tex->used = c->dev->epoch;
  c->dev->recorded = 1;
  UNLOCK(c);
}
static void STDMETHODCALLTYPE ctx_ClearDepthStencilView(Ctx *c, ID3D11DepthStencilView *view, UINT flags, FLOAT depth, UINT8 stencil) {
  View *v = (View *)view;
  Dev *d = c->dev;
  if (!v || !(flags & 3)) return;
  LOCK(c);
  /* Direct3D 12 here clears a depth buffer when the next pass with it begins, and forgets the clear with the list: it is kept until a draw has
   * followed it, and recorded again in every list until then. */
  UINT at = d->clear_count;
  for (UINT i = 0; i < d->clear_count; ++i) if (d->clears[i].dsv == v) at = i;
  if (at == d->clear_count && d->clear_count < 8) ++d->clear_count;
  if (at < 8) {
    PendingClear *p = &d->clears[at];
    p->flags = (p->dsv == v ? p->flags : 0) | (flags & 3);
    p->dsv = v;
    if (flags & D3D11_CLEAR_DEPTH) p->depth = depth;
    if (flags & D3D11_CLEAR_STENCIL) p->stencil = stencil;
  }
  ID3D12GraphicsCommandList_ClearDepthStencilView(d->list, v->handle, (D3D12_CLEAR_FLAGS)(flags & 3), depth, stencil, 0, NULL);
  v->tex->used = d->epoch;
  d->recorded = 1;
  UNLOCK(c);
}

/* ---- the CPU's side of resources ----------------------------------------------------------------------------------------------------------------- */

static Buf *buf_of_resource(ID3D11Resource *resource) {
  if (!resource || ((DxObj *)resource)->lpVtbl != (const void *)&buf_vtbl) return NULL;
  return (Buf *)resource;
}

static HRESULT STDMETHODCALLTYPE ctx_Map(Ctx *c, ID3D11Resource *resource, UINT sub, D3D11_MAP type, UINT flags, D3D11_MAPPED_SUBRESOURCE *out) {
  (void)flags;
  Dev *d = c->dev;
  if (out) memset(out, 0, sizeof *out);
  if (!out || type < D3D11_MAP_READ || type > D3D11_MAP_WRITE_NO_OVERWRITE) return E_INVALIDARG;
  Buf *b = buf_of_resource(resource);
  Tex *t = tex_of_resource(resource);
  HRESULT hr = S_OK;
  LOCK(c);
  if (b) {
    if (sub != 0) hr = E_INVALIDARG;
    else {
      if (type != D3D11_MAP_WRITE_NO_OVERWRITE && type != D3D11_MAP_READ) settle(d, b->used);      /* (a recorded draw reads what is there now) */
      out->pData = b->data;
      out->RowPitch = out->DepthPitch = b->desc.ByteWidth;
    }
  } else if (t) {
    if (sub >= tex_subresources(t) || t->depth || t->mapped[sub]) hr = E_INVALIDARG;
    else {
      const UINT level = sub % t->desc.MipLevels, w = mip_size(t->desc.Width, level), h = mip_size(t->desc.Height, level);
      if (!t->shadow[sub]) t->shadow[sub] = mem_alloc((SIZE_T)w * h * t->bpp);
      if (!t->shadow[sub]) hr = E_OUTOFMEMORY;
      else if (type == D3D11_MAP_READ || type == D3D11_MAP_READ_WRITE) hr = tex_read(d, t, sub, t->shadow[sub]);
      if (SUCCEEDED(hr)) {
        t->mapped[sub] = type;
        out->pData = t->shadow[sub];
        out->RowPitch = w * t->bpp;
        out->DepthPitch = w * h * t->bpp;
      }
    }
  } else hr = E_INVALIDARG;
  UNLOCK(c);
  return hr;
}
static void STDMETHODCALLTYPE ctx_Unmap(Ctx *c, ID3D11Resource *resource, UINT sub) {
  Buf *b = buf_of_resource(resource);
  Tex *t = tex_of_resource(resource);
  LOCK(c);
  if (b) ++b->version;
  else if (t && sub < tex_subresources(t) && t->mapped[sub]) {
    const D3D11_MAP type = t->mapped[sub];
    t->mapped[sub] = 0;
    if (type != D3D11_MAP_READ) {
      const UINT level = sub % t->desc.MipLevels, w = mip_size(t->desc.Width, level), h = mip_size(t->desc.Height, level);
      tex_write(c->dev, t, sub, 0, 0, w, h, t->shadow[sub], w * t->bpp);
    }
  }
  UNLOCK(c);
}

static void STDMETHODCALLTYPE ctx_UpdateSubresource(Ctx *c, ID3D11Resource *resource, UINT sub, const D3D11_BOX *box, const void *data, UINT row_pitch, UINT depth_pitch) {
  (void)depth_pitch;
  Buf *b = buf_of_resource(resource);
  Tex *t = tex_of_resource(resource);
  if (!data) return;
  LOCK(c);
  if (b && sub == 0) {
    const UINT left = box ? box->left : 0, right = box ? box->right : b->desc.ByteWidth;
    if (left < right && right <= b->desc.ByteWidth) {
      settle(c->dev, b->used);
      memcpy(b->data + left, data, right - left);
      ++b->version;
    }
  } else if (t && sub < tex_subresources(t) && !t->depth) {
    const UINT level = sub % t->desc.MipLevels, w = mip_size(t->desc.Width, level), h = mip_size(t->desc.Height, level);
    const UINT x = box ? box->left : 0, y = box ? box->top : 0, x1 = box ? box->right : w, y1 = box ? box->bottom : h;
    if (x < x1 && y < y1 && x1 <= w && y1 <= h) tex_write(c->dev, t, sub, x, y, x1 - x, y1 - y, data, row_pitch ? row_pitch : (x1 - x) * t->bpp);
  }
  UNLOCK(c);
}

/* A rectangle of one subresource into another, by way of memory (what Direct3D 12 here does not copy itself). */
static void tex_copy_slow(Dev *d, Tex *to, UINT to_sub, UINT x, UINT y, Tex *from, UINT from_sub, const D3D11_BOX *box) {
  if (to->bpp != from->bpp || to->depth || from->depth) return;
  const UINT level = from_sub % from->desc.MipLevels, w = mip_size(from->desc.Width, level), h = mip_size(from->desc.Height, level);
  const UINT to_level = to_sub % to->desc.MipLevels, to_w = mip_size(to->desc.Width, to_level), to_h = mip_size(to->desc.Height, to_level);
  const UINT x0 = box ? box->left : 0, y0 = box ? box->top : 0, x1 = box ? box->right : w, y1 = box ? box->bottom : h;
  if (x0 >= x1 || y0 >= y1 || x1 > w || y1 > h || x > to_w || y > to_h || x1 - x0 > to_w - x || y1 - y0 > to_h - y) return;
  BYTE *pixels = mem_alloc((SIZE_T)w * h * from->bpp);
  if (pixels && SUCCEEDED(tex_read(d, from, from_sub, pixels)))
    tex_write(d, to, to_sub, x, y, x1 - x0, y1 - y0, pixels + ((SIZE_T)y0 * w + x0) * from->bpp, w * from->bpp);
  mem_free(pixels);
}
static BOOL tex_simple(const Tex *t) { return t->desc.MipLevels == 1 && t->desc.ArraySize == 1 && !t->depth; }

static void STDMETHODCALLTYPE ctx_CopySubresourceRegion(Ctx *c, ID3D11Resource *to, UINT to_sub, UINT x, UINT y, UINT z, ID3D11Resource *from, UINT from_sub, const D3D11_BOX *box) {
  (void)z;
  Dev *d = c->dev;
  Buf *bt = buf_of_resource(to), *bf = buf_of_resource(from);
  Tex *tt = tex_of_resource(to), *tf = tex_of_resource(from);
  LOCK(c);
  if (bt && bf) {
    const UINT left = box ? box->left : 0, right = box ? box->right : bf->desc.ByteWidth;
    if (left < right && right <= bf->desc.ByteWidth && x <= bt->desc.ByteWidth && right - left <= bt->desc.ByteWidth - x) {
      settle(d, bt->used);
      memmove(bt->data + x, bf->data + left, right - left);
      ++bt->version;
    }
  } else if (tt && tf && to_sub < tex_subresources(tt) && from_sub < tex_subresources(tf)) {
    if (tex_simple(tt) && tex_simple(tf) && tt->stored == tf->stored && tt != tf) {
      D3D12_TEXTURE_COPY_LOCATION dst, src;
      memset(&dst, 0, sizeof dst);
      memset(&src, 0, sizeof src);
      dst.pResource = tt->res;
      dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
      src.pResource = tf->res;
      src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
      D3D12_BOX region;
      if (box) { region.left = box->left; region.top = box->top; region.front = 0; region.right = box->right; region.bottom = box->bottom; region.back = 1; }
      ID3D12GraphicsCommandList_CopyTextureRegion(d->list, &dst, x, y, 0, &src, box ? &region : NULL);
      tt->used = tf->used = d->epoch;
      d->recorded = 1;
    } else tex_copy_slow(d, tt, to_sub, x, y, tf, from_sub, box);
  }
  UNLOCK(c);
}
static void STDMETHODCALLTYPE ctx_CopyResource(Ctx *c, ID3D11Resource *to, ID3D11Resource *from) {
  Dev *d = c->dev;
  Buf *bt = buf_of_resource(to), *bf = buf_of_resource(from);
  Tex *tt = tex_of_resource(to), *tf = tex_of_resource(from);
  LOCK(c);
  if (bt && bf && bt != bf) {
    settle(d, bt->used);
    memcpy(bt->data, bf->data, bt->desc.ByteWidth < bf->desc.ByteWidth ? bt->desc.ByteWidth : bf->desc.ByteWidth);
    ++bt->version;
  } else if (tt && tf && tt != tf && tex_subresources(tt) == tex_subresources(tf) && tt->desc.Width == tf->desc.Width && tt->desc.Height == tf->desc.Height) {
    if (tex_simple(tt) && tex_simple(tf) && tt->stored == tf->stored) {
      ID3D12GraphicsCommandList_CopyResource(d->list, tt->res, tf->res);
      tt->used = tf->used = d->epoch;
      d->recorded = 1;
    } else for (UINT sub = 0; sub < tex_subresources(tt); ++sub) tex_copy_slow(d, tt, sub, 0, 0, tf, sub, NULL);
  }
  UNLOCK(c);
}
static void STDMETHODCALLTYPE ctx_GenerateMips(Ctx *c, ID3D11ShaderResourceView *view) { (void)c; (void)view; }
/* A multisampled texture into an ordinary one of its size and format: the samples of each pixel averaged (Direct3D 12's ResolveSubresource below). */
static void STDMETHODCALLTYPE ctx_ResolveSubresource(Ctx *c, ID3D11Resource *to, UINT to_sub, ID3D11Resource *from, UINT from_sub, DXGI_FORMAT format) {
  (void)format;
  Dev *d = c->dev;
  Tex *tt = tex_of_resource(to), *tf = tex_of_resource(from);
  if (!tt || !tf || to_sub != 0 || from_sub != 0 || tf->desc.SampleDesc.Count < 2 || tt->desc.SampleDesc.Count > 1 || tt->desc.Width != tf->desc.Width ||
      tt->desc.Height != tf->desc.Height || tt->stored != tf->stored) return;
  LOCK(c);
  ID3D12GraphicsCommandList_ResolveSubresource(d->list, tt->res, 0, tf->res, 0, tt->stored);      /* (the stored format: a typed view of it asked for is the same bytes) */
  tt->used = tf->used = d->epoch;
  d->recorded = 1;
  UNLOCK(c);
}

static void STDMETHODCALLTYPE ctx_Flush(Ctx *c) {
  LOCK(c);
  flush(c->dev);
  UNLOCK(c);
}
static void ctx_unbind_all(Ctx *c) {
  bind(&c->vs, NULL);
  bind(&c->ps, NULL);
  bind(&c->layout, NULL);
  bind(&c->ib, NULL);
  bind(&c->dsv, NULL);
  bind(&c->blend, NULL);
  bind(&c->depth, NULL);
  bind(&c->raster, NULL);
  for (UINT i = 0; i < VERTEX_SLOTS; ++i) bind(&c->vb[i], NULL);
  for (UINT i = 0; i < CONSTANT_SLOTS; ++i) { bind(&c->vs_cb[i], NULL); bind(&c->ps_cb[i], NULL); }
  for (UINT i = 0; i < TEXTURE_SLOTS; ++i) { bind(&c->srv[i], NULL); bind(&c->sampler[i], NULL); }
  for (UINT i = 0; i < 8; ++i) bind(&c->rtv[i], NULL);
  c->rtv_count = 0;
}
static void STDMETHODCALLTYPE ctx_ClearState(Ctx *c) {
  LOCK(c);
  ctx_unbind_all(c);
  c->viewport_set = c->scissor_set = 0;
  c->topology = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
  c->stencil_ref = 0;
  c->sample_mask = 0xFFFFFFFFu;
  c->dev->dirty = DIRTY_ALL;
  UNLOCK(c);
}

/* ---- queries (see d3d11.c) ------------------------------------------------------------------------------------------------------------------------------ */

static Query *query_of(ID3D11Asynchronous *a) {
  if (!a || ((DxObj *)a)->lpVtbl != (const void *)&query_vtbl) return NULL;      /* (only this library's queries: predicates and counters are not made) */
  return (Query *)a;
}
static void STDMETHODCALLTYPE ctx_Begin(Ctx *c, ID3D11Asynchronous *async) {
  (void)c;
  Query *q = query_of(async);
  if (!q || q->desc.Query == D3D11_QUERY_EVENT || q->desc.Query == D3D11_QUERY_TIMESTAMP) return;      /* (those only End) */
  q->begun = TRUE;
  q->ended = FALSE;
  q->start = query_now();
}
static void STDMETHODCALLTYPE ctx_End(Ctx *c, ID3D11Asynchronous *async) {
  Query *q = query_of(async);
  if (!q) return;
  flush(c->dev);      /* (what came before is done) */
  q->stop = query_now();
  q->ended = TRUE;
}
static HRESULT STDMETHODCALLTYPE ctx_GetData(Ctx *c, ID3D11Asynchronous *async, void *data, UINT size, UINT flags) {
  (void)c; (void)flags;
  Query *q = query_of(async);
  if (!q) return E_INVALIDARG;
  const UINT need = query_data_size(q->desc.Query);
  if (data && size != need && size != 0) return E_INVALIDARG;
  if (!q->ended) return DXGI_ERROR_INVALID_CALL;
  if (!data || size == 0) return S_OK;
  switch (q->desc.Query) {
    case D3D11_QUERY_EVENT: *(BOOL *)data = TRUE; break;
    case D3D11_QUERY_OCCLUSION: *(UINT64 *)data = 1; break;
    case D3D11_QUERY_TIMESTAMP: *(UINT64 *)data = q->stop; break;
    case D3D11_QUERY_TIMESTAMP_DISJOINT: {
      D3D11_QUERY_DATA_TIMESTAMP_DISJOINT *t = data;
      LARGE_INTEGER f;
      QueryPerformanceFrequency(&f);
      t->Frequency = (UINT64)f.QuadPart;
      t->Disjoint = FALSE;
      break;
    }
    default: return E_INVALIDARG;
  }
  return S_OK;
}

