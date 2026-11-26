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

#define LOCK(c) EnterCriticalSection(&(c)->dev->lock)
#define UNLOCK(c) LeaveCriticalSection(&(c)->dev->lock)
static void set_range(void **slots, UINT capacity, UINT start, UINT count, void *const *objects);
static void get_range(void **slots, UINT capacity, UINT start, UINT count, void **objects);
static void unbind_reads_of(Ctx *c, const void *resource, BOOL compute);
static BOOL compute_prepare(Ctx *c);
static void rtv_load(Dev *d, struct View *v, BOOL whole);
static void set_instances(Ctx *c, UINT stage, ID3D11ClassInstance *const *instances, UINT count);

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
/* The format a render target view draws in, as the pipeline has it: a plane's (its plane's format), the view's own of a typeless R8 / R8G8 / R16 / R16G16 texture (Direct3D 12 draws
 * into a mirror of the view's kind: R8_UINT, R16_FLOAT ...), else `texture_format`. */
static DXGI_FORMAT rtv_format(const View *v, DXGI_FORMAT texture_format) {
  if (v->plane_format) return v->plane_format;
  const DXGI_FORMAT stored = v->tex->stored;
  if ((stored == DXGI_FORMAT_R8_TYPELESS || stored == DXGI_FORMAT_R8G8_TYPELESS || stored == DXGI_FORMAT_R16_TYPELESS || stored == DXGI_FORMAT_R16G16_TYPELESS) &&
      v->desc.rtv.Format != DXGI_FORMAT_UNKNOWN)
    return v->desc.rtv.Format;
  return texture_format;
}

static ID3D12PipelineState *pipeline_get(Dev *d, Ctx *c) {
  Pipeline key;
  memset(&key, 0, sizeof key);
  /* the code of each stage: the shader's, or for one with interfaces the shader specialised for the class instances bound with it */
  Shader *const stage_shader[5] = {c->vs, c->ps, c->gs, c->hs, c->ds};
  const BYTE *code[5] = {NULL, NULL, NULL, NULL, NULL};
  SIZE_T code_bytes[5] = {0, 0, 0, 0, 0};
  UINT code_id[5] = {0, 0, 0, 0, 0};
  for (UINT k = 0; k < 5; ++k) {
    Variant *variant;
    if (stage_shader[k] && !shader_code(d, c, k, stage_shader[k], &code[k], &code_bytes[k], &code_id[k], &variant)) return NULL;
  }
  key.vs = code_id[0];
  key.ps = code_id[1];
  key.gs = code_id[2];
  key.hs = code_id[3];
  key.ds = code_id[4];
  key.layout = c->layout ? c->layout->c.id : 0;
  key.blend = c->blend ? c->blend->desc : default_blend();
  key.logic_op = c->blend && c->blend->desc1.RenderTarget[0].LogicOpEnable ? (UINT)c->blend->desc1.RenderTarget[0].LogicOp + 1 : 0;
  key.depth = c->depth ? c->depth->desc : g_default_depth;
  key.raster = c->raster ? c->raster->desc : g_default_raster;
  key.raster.ScissorEnable = 0;      /* (not the pipeline's: the scissor rectangle is set with the viewport) */
  key.conservative = c->raster ? (UINT)c->raster->desc2.ConservativeRaster : 0u;
  key.forced = c->raster ? c->raster->desc2.ForcedSampleCount : 0u;
  key.targets = c->rtv_count;
  key.has_depth = c->dsv != NULL;
  key.samples = 1;
  for (UINT i = 0; i < c->rtv_count && i < 8; ++i) {
    key.formats[i] = rtv_format(c->rtv[i], c->rtv[i]->tex->desc.Format);      /* (as the program made it: a plane's view: its plane; a typeless narrow texture's view: the view's) */
    if (c->rtv[i]->tex->desc.SampleDesc.Count > 1) key.samples = c->rtv[i]->tex->desc.SampleDesc.Count;
  }
  if (c->dsv) {
    key.depth_format = c->dsv->tex->desc.Format;      /* (the program's format: a depth bias counts in its unit, D16 and D32 being kept alike) */
    if (c->dsv->tex->desc.SampleDesc.Count > 1) key.samples = c->dsv->tex->desc.SampleDesc.Count;
  }
  if (!c->dsv) { key.depth.DepthEnable = FALSE; key.depth.StencilEnable = FALSE; }
  Pipeline *oldest = &d->pipelines[0];
  for (UINT i = 0; i < d->pipeline_count; ++i) {
    Pipeline *p = &d->pipelines[i];
    if (p->vs == key.vs && p->ps == key.ps && p->gs == key.gs && p->hs == key.hs && p->ds == key.ds && p->layout == key.layout && p->targets == key.targets && p->has_depth == key.has_depth && p->samples == key.samples &&
        memcmp(p->formats, key.formats, sizeof key.formats) == 0 && p->depth_format == key.depth_format &&
        memcmp(&p->blend, &key.blend, sizeof key.blend) == 0 && memcmp(&p->depth, &key.depth, sizeof key.depth) == 0 && memcmp(&p->raster, &key.raster, sizeof key.raster) == 0 && p->logic_op == key.logic_op && p->conservative == key.conservative &&
        p->forced == key.forced) {
      p->last_used = ++d->pipeline_clock;
      return p->state;
    }
    if (p->last_used < oldest->last_used) oldest = p;
  }

  D3D12_GRAPHICS_PIPELINE_STATE_DESC pd;
  memset(&pd, 0, sizeof pd);
  pd.pRootSignature = d->root;
  pd.VS.pShaderBytecode = code[0];
  pd.VS.BytecodeLength = code_bytes[0];
  if (c->ps) { pd.PS.pShaderBytecode = code[1]; pd.PS.BytecodeLength = code_bytes[1]; }
  if (c->gs && !c->gs->so_only) { pd.GS.pShaderBytecode = code[2]; pd.GS.BytecodeLength = code_bytes[2]; }
  if (c->gs && c->gs->so) {
    /* (stream output: of the geometry shader, or of the stage whose code the "geometry shader" is - which is then the vertex or domain shader bound) */
    pd.StreamOutput.pSODeclaration = (const D3D12_SO_DECLARATION_ENTRY *)c->gs->so;      /* (the two structures are the same) */
    pd.StreamOutput.NumEntries = c->gs->so_count;
    pd.StreamOutput.pBufferStrides = c->gs->so_strides;
    pd.StreamOutput.NumStrides = c->gs->so_stride_count;
    pd.StreamOutput.RasterizedStream = c->gs->so_rasterized;
  }
  if (c->hs) { pd.HS.pShaderBytecode = code[3]; pd.HS.BytecodeLength = code_bytes[3]; }
  if (c->ds) { pd.DS.pShaderBytecode = code[4]; pd.DS.BytecodeLength = code_bytes[4]; }
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
    out->LogicOpEnable = key.logic_op != 0;
    out->LogicOp = key.logic_op ? (D3D12_LOGIC_OP)(key.logic_op - 1) : D3D12_LOGIC_OP_NOOP;      /* (the two enumerations are the same) */
    out->RenderTargetWriteMask = in->RenderTargetWriteMask;
  }
  for (UINT i = 0; i < c->rtv_count && i < 8; ++i) {      /* a target kept wider than its format: only its own channels are written */
    const UINT8 mask = c->rtv[i]->plane_format ? (c->rtv[i]->plane ? 3 : 1) : tex_format_mask(c->rtv[i]->tex->fmt);
    if (mask == 0xF) continue;
    if (!pd.BlendState.IndependentBlendEnable) {
      for (UINT j = 1; j < 8; ++j) pd.BlendState.RenderTarget[j] = pd.BlendState.RenderTarget[0];
      pd.BlendState.IndependentBlendEnable = TRUE;
    }
    pd.BlendState.RenderTarget[i].RenderTargetWriteMask &= mask;
  }
  pd.RasterizerState.FillMode = (D3D12_FILL_MODE)key.raster.FillMode;
  pd.RasterizerState.CullMode = (D3D12_CULL_MODE)key.raster.CullMode;
  pd.RasterizerState.FrontCounterClockwise = key.raster.FrontCounterClockwise;
  pd.RasterizerState.DepthBias = key.raster.DepthBias;
  pd.RasterizerState.DepthBiasClamp = key.raster.DepthBiasClamp;
  pd.RasterizerState.SlopeScaledDepthBias = key.raster.SlopeScaledDepthBias;
  pd.RasterizerState.DepthClipEnable = key.raster.DepthClipEnable;
  /* (the line mode: MultisampleEnable quadrilateral lines, else AntialiasedLineEnable alpha-antialiased ones - Direct3D 12 chooses as Direct3D 11 does) */
  pd.RasterizerState.MultisampleEnable = key.raster.MultisampleEnable;
  pd.RasterizerState.AntialiasedLineEnable = key.raster.AntialiasedLineEnable;
  pd.RasterizerState.ConservativeRaster = key.conservative ? D3D12_CONSERVATIVE_RASTERIZATION_MODE_ON : D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;
  pd.RasterizerState.ForcedSampleCount = key.forced;      /* (Direct3D 12's pipeline checks it: above 1 no multisampled target, no depth or stencil test) */
  pd.DepthStencilState.DepthEnable = key.depth.DepthEnable;
  pd.DepthStencilState.DepthWriteMask = (D3D12_DEPTH_WRITE_MASK)key.depth.DepthWriteMask;
  pd.DepthStencilState.DepthFunc = (D3D12_COMPARISON_FUNC)key.depth.DepthFunc;
  pd.DepthStencilState.StencilEnable = key.depth.StencilEnable;
  pd.DepthStencilState.StencilReadMask = key.depth.StencilReadMask;
  pd.DepthStencilState.StencilWriteMask = key.depth.StencilWriteMask;
  memcpy(&pd.DepthStencilState.FrontFace, &key.depth.FrontFace, sizeof pd.DepthStencilState.FrontFace);      /* (the two structures are the same) */
  memcpy(&pd.DepthStencilState.BackFace, &key.depth.BackFace, sizeof pd.DepthStencilState.BackFace);
  if (c->layout) { pd.InputLayout.pInputElementDescs = c->layout->elements; pd.InputLayout.NumElements = c->layout->count; }
  pd.PrimitiveTopologyType = c->hs ? D3D12_PRIMITIVE_TOPOLOGY_TYPE_PATCH : D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  pd.NumRenderTargets = c->rtv_count;
  for (UINT i = 0; i < c->rtv_count; ++i) pd.RTVFormats[i] = rtv_format(c->rtv[i], c->rtv[i]->tex->stored);
  if (c->dsv) {
    /* (the program's depth format, typeless ones as the depth format they are: what the depth bias's unit is) */
    const DXGI_FORMAT f = c->dsv->tex->desc.Format;
    pd.DSVFormat = f == DXGI_FORMAT_R24G8_TYPELESS ? DXGI_FORMAT_D24_UNORM_S8_UINT : f == DXGI_FORMAT_R32_TYPELESS ? DXGI_FORMAT_D32_FLOAT : f == DXGI_FORMAT_R16_TYPELESS ? DXGI_FORMAT_D16_UNORM
                 : f == DXGI_FORMAT_R32G8X24_TYPELESS ? DXGI_FORMAT_D32_FLOAT_S8X24_UINT : f;
  }
  pd.SampleDesc.Count = key.samples;
  /* (Direct3D 11 ends a strip at the index buffer's largest index, 0xFFFF or 0xFFFFFFFF by its format: the list's strip cut, set with the index buffer at each indexed draw) */
  pd.Flags |= D3D12_PIPELINE_STATE_FLAG_DYNAMIC_INDEX_BUFFER_STRIP_CUT;
  ID3D12PipelineState *state = NULL;
  /* (the pixel shader's constant buffers come after the vertex shader's, then the geometry, hull and domain shaders' in that order: as draw_prepare lays them out) */
  const Shader *gs = c->gs && !c->gs->so_only ? c->gs : NULL;
  const UINT ps_base = c->vs->registers, gs_base = ps_base + (c->ps ? c->ps->registers : 0), hs_base = gs_base + (gs ? gs->registers : 0),
             ds_base = hs_base + (c->hs ? c->hs->registers : 0);
  TcosD3D12PipelineOptions(d->d12, ps_base, 0);
  TcosD3D12StageConstantBases(d->d12, gs_base, hs_base, ds_base);
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
    /* A recorded draw reads the copy: the list is sent first. (Retiring the copy instead, as a renamed buffer's memory is, made Chrome's window stop updating
     * now and then on the 3D hardware - the GPU process kept drawing and presenting, the picture stayed - for a reason not found yet; see docs/Direct3D11.md.) */
    if (b->converted && b->converted_used == d->epoch) flush(d);
    if (b->converted && (b->converted_vertices < vertices || b->converted_out_stride != out_stride)) {
      const Spare s = {b->converted, NULL, b->converted_bytes, 0};
      spare_keep(d, s);
      b->converted = NULL;
    }
    /* (a buffer of the GPU's memory - stream output, unordered access - is read back first) */
    BYTE *source = b->data, *fetched = NULL;
    if (b->gpu) {
      fetched = mem_alloc(b->desc.ByteWidth);
      if (!fetched || FAILED(buf_gpu_read(d, b, 0, b->desc.ByteWidth, fetched))) { mem_free(fetched); return NULL; }
      source = fetched;
    }
    BYTE *out = NULL;
    if (!b->converted) b->converted = take_upload(d, (UINT64)(vertices ? vertices : 1) * out_stride, &out, &b->converted_bytes);
    else if (FAILED(ID3D12Resource_Map(b->converted, 0, NULL, (void **)&out))) out = NULL;
    if (!b->converted || !out) { mem_free(fetched); return NULL; }
    for (UINT v = 0; v < vertices; ++v) {
      const BYTE *in = source + *offset + (SIZE_T)v * *stride;
      for (UINT i = 0; i < l->count; ++i) {
        if (l->elements[i].InputSlot != slot) continue;
        BYTE *to = out + (SIZE_T)v * out_stride + l->elements[i].AlignedByteOffset;
        const BYTE *from = in + l->source_offset[i];
        if (l->source_offset[i] + element_bytes(l->source_format[i]) > *stride) continue;      /* (an element beyond the stride: not read) */
        UINT32 value[4];
        element_decode(l->source_format[i], from, value);
        memcpy(to, value, 16);
      }
    }
    ID3D12Resource_Unmap(b->converted, 0, NULL);
    mem_free(fetched);
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
    p->dsv->tex->used = p->dsv->tex->written = d->epoch;      /* (the list uses the texture now: it is sent before the texture goes) */
    d->recorded = 1;
  }
}

/* The kernel keeps a texture bound the way a list first uses it. So a list does not write a texture (draw into it, clear it, copy into it) after it sampled it,
 * nor read one (sample it, copy from it) after it wrote it: the list is sent first. (When a write to whatever a recorded draw reads sent the list, as it did
 * for every buffer write, these cases hardly came up; with long lists they show stale pixels.) */
static void tex_before_write(Dev *d, const Tex *t) { settle(d, t->sampled); }
static void tex_before_read(Dev *d, const Tex *t) { settle(d, t->written); }

/* What texture slot `i` of the table holds: the pixel shader's, else the first of the other stages' that has one bound (they share the kernel's units). */
static View *slot_view(const Ctx *c, UINT i) {
  if (i >= SRV_SLOTS) return NULL;
  if (c->srv[i]) return c->srv[i];
  for (UINT k = 0; k < 4; ++k) if (c->stage_srv[k][i]) return c->stage_srv[k][i];
  return NULL;
}
static Sampler *slot_sampler(const Ctx *c, UINT i) {
  if (i >= TEXTURE_SLOTS) return NULL;
  if (c->sampler[i]) return c->sampler[i];
  for (UINT k = 0; k < 4; ++k) if (c->stage_sampler[k][i]) return c->stage_sampler[k][i];
  return NULL;
}
/* What fills a slot with nothing bound: a white texture of the kind the shaders declare there (the kernel refuses a draw whose texture is of another kind). */
static Tex *slot_blank(const Dev *d, const Ctx *c, UINT i) {
  const Shader *stages[5] = {c->ps, c->vs, c->gs, c->hs, c->ds};
  for (UINT k = 0; k < 5; ++k) {
    const UINT kind = stages[k] ? stages[k]->tex_kind[i] : 0xFF;
    if (kind >= 1 && kind <= 5 && d->blank_kind[kind]) return d->blank_kind[kind];
  }
  return d->blank;
}

/* The block of constants of a draw or a dispatch: the buffers bound to each stage's slots, stage after stage, each as large as the shader declares it, into the ring
 * (Dev.ring_last is where; a block the same as the last one is used again). */
static Buf **cb_slots(Ctx *c, UINT stage);
static void constants_record(Dev *d, Ctx *c, const Shader *const *stages, const UINT *stage_of, UINT count) {
  BYTE block[CONSTANT_BLOCK];
  UINT bytes = 0;
  for (UINT stage = 0; stage < count; ++stage) {
    const Shader *s = stages[stage];
    const UINT k = stage_of[stage];
    Buf *const *bound = cb_slots(c, k);
    for (UINT slot = 0; s && slot < CONSTANT_SLOTS; ++slot) {
      const UINT size = s->cb_registers[slot] * 16;
      if (!size || bytes + size > CONSTANT_BLOCK) continue;
      const Buf *b = bound[slot];
      /* (the window the stage reads: from its first constant, as many as were given and the buffer has) */
      const UINT64 from = b ? (UINT64)c->cb_first[k][slot] * 16 : 0;
      UINT64 window = b ? (UINT64)c->cb_number[k][slot] * 16 : 0;
      if (b && from >= b->desc.ByteWidth) window = 0;
      else if (b && window > b->desc.ByteWidth - from) window = b->desc.ByteWidth - from;
      const UINT have = (UINT)(window < size ? window : size);
      if (have) memcpy(block + bytes, b->data + from, have);
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
}

/* What the shaders of a draw declare t<n> as (Tex.kind, KIND_BUFFER; 0xFF: none declares it). */
static BYTE slot_declared_kind(const Ctx *c, UINT i) {
  const Shader *stages[5] = {c->ps, c->vs, c->gs, c->hs, c->ds};
  for (UINT k = 0; k < 5; ++k) if (stages[k] && stages[k]->tex_kind[i] != 0xFF) return stages[k]->tex_kind[i];
  return 0xFF;
}

static void buf_write_any(Dev *d, Buf *b, UINT64 offset, const void *data, UINT64 bytes);
static void buf_copy(Dev *d, Buf *to, UINT64 to_offset, Buf *from, UINT64 from_offset, UINT64 bytes);
/* The descriptor of a buffer view: a typed view as Direct3D 12's typed view of the same elements; an append or counter view as a structured one with its counter (written
 * first when it was set since); the others raw words (the kernel takes the stride from the shader's declaration). FALSE for one the kernel cannot bind (not whole words). */
static BOOL buffer_descriptor(Dev *d, View *v, BOOL unordered, D3D12_CPU_DESCRIPTOR_HANDLE at) {
  if (v->format != DXGI_FORMAT_UNKNOWN && v->stride) {
    if (unordered) {
      D3D12_UNORDERED_ACCESS_VIEW_DESC u;
      memset(&u, 0, sizeof u);
      u.Format = v->format;
      u.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
      u.Buffer.FirstElement = v->offset / v->stride;
      u.Buffer.NumElements = (UINT)(v->size / v->stride);
      ID3D12Device_CreateUnorderedAccessView(d->d12, v->buf->res, NULL, &u, at);
    } else {
      D3D12_SHADER_RESOURCE_VIEW_DESC r;
      memset(&r, 0, sizeof r);
      r.Format = v->format;
      r.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
      r.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
      r.Buffer.FirstElement = v->offset / v->stride;
      r.Buffer.NumElements = (UINT)(v->size / v->stride);
      ID3D12Device_CreateShaderResourceView(d->d12, v->buf->res, &r, at);
    }
    return TRUE;
  }
  if (v->offset % 4 || v->size % 4) return FALSE;
  if (unordered && v->counter_buf && v->stride) {
    if (v->counter_dirty) {
      buf_write_any(d, v->counter_buf, 0, &v->counter, 4);
      v->counter_dirty = FALSE;
    }
    D3D12_UNORDERED_ACCESS_VIEW_DESC u;
    memset(&u, 0, sizeof u);
    u.Format = DXGI_FORMAT_UNKNOWN;
    u.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    u.Buffer.FirstElement = v->offset / v->stride;
    u.Buffer.NumElements = (UINT)(v->size / v->stride);
    u.Buffer.StructureByteStride = v->stride;
    ID3D12Device_CreateUnorderedAccessView(d->d12, v->buf->res, v->counter_buf->res, &u, at);
    return TRUE;
  }
  if (unordered) {
    D3D12_UNORDERED_ACCESS_VIEW_DESC u;
    memset(&u, 0, sizeof u);
    u.Format = DXGI_FORMAT_R32_TYPELESS;
    u.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    u.Buffer.FirstElement = v->offset / 4;
    u.Buffer.NumElements = (UINT)(v->size / 4);
    u.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
    ID3D12Device_CreateUnorderedAccessView(d->d12, v->buf->res, NULL, &u, at);
  } else {
    D3D12_SHADER_RESOURCE_VIEW_DESC r;
    memset(&r, 0, sizeof r);
    r.Format = DXGI_FORMAT_R32_TYPELESS;
    r.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    r.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    r.Buffer.FirstElement = v->offset / 4;
    r.Buffer.NumElements = (UINT)(v->size / 4);
    r.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
    ID3D12Device_CreateShaderResourceView(d->d12, v->buf->res, &r, at);
  }
  return TRUE;
}

/* The descriptor of a texture's unordered access view: Direct3D 12's view of the same level and layers (a planar texture's: the plane of the slices, which Direct3D 12 below
 * writes through a mirror of them). */
static void texture_uav_descriptor(Dev *d, const View *v, D3D12_CPU_DESCRIPTOR_HANDLE at) {
  const Tex *t = v->tex;
  const D3D11_UNORDERED_ACCESS_VIEW_DESC *u = &v->desc.uav;
  D3D12_UNORDERED_ACCESS_VIEW_DESC x;
  memset(&x, 0, sizeof x);
  x.Format = t->planar ? v->plane_format : u->Format != DXGI_FORMAT_UNKNOWN ? u->Format : t->desc.Format;
  if (!t->planar && x.Format == t->desc.Format) x.Format = t->stored;      /* (the format Direct3D 12 keeps it in) */
  switch (u->ViewDimension) {
    case D3D11_UAV_DIMENSION_TEXTURE1D: x.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D; x.Texture2D.MipSlice = u->Texture1D.MipSlice; break;
    case D3D11_UAV_DIMENSION_TEXTURE1DARRAY:
      x.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
      x.Texture2DArray.MipSlice = u->Texture1DArray.MipSlice;
      x.Texture2DArray.FirstArraySlice = u->Texture1DArray.FirstArraySlice;
      x.Texture2DArray.ArraySize = u->Texture1DArray.ArraySize;
      break;
    case D3D11_UAV_DIMENSION_TEXTURE2D:
      if (t->planar && t->desc.ArraySize > 1) {      /* (a plane of a slice of an array) */
        x.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
        x.Texture2DArray.FirstArraySlice = v->array_slice;
        x.Texture2DArray.ArraySize = 1;
        x.Texture2DArray.PlaneSlice = v->plane;
      } else {
        x.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        x.Texture2D.MipSlice = u->Texture2D.MipSlice;
        x.Texture2D.PlaneSlice = t->planar ? v->plane : 0;
      }
      break;
    case D3D11_UAV_DIMENSION_TEXTURE2DARRAY:
      x.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
      x.Texture2DArray.MipSlice = u->Texture2DArray.MipSlice;
      x.Texture2DArray.FirstArraySlice = u->Texture2DArray.FirstArraySlice;
      x.Texture2DArray.ArraySize = u->Texture2DArray.ArraySize;
      x.Texture2DArray.PlaneSlice = t->planar ? v->plane : 0;
      break;
    case D3D11_UAV_DIMENSION_TEXTURE3D:
      x.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE3D;
      x.Texture3D.MipSlice = u->Texture3D.MipSlice;
      x.Texture3D.FirstWSlice = u->Texture3D.FirstWSlice;
      x.Texture3D.WSize = u->Texture3D.WSize;
      break;
    default: ID3D12Device_CreateUnorderedAccessView(d->d12, NULL, NULL, NULL, at); return;
  }
  /* (a level the kernel does not keep - the other levels of an array or a volume -: nothing to write) */
  if ((x.ViewDimension == D3D12_UAV_DIMENSION_TEXTURE2D ? x.Texture2D.MipSlice : x.ViewDimension == D3D12_UAV_DIMENSION_TEXTURE3D ? x.Texture3D.MipSlice : x.Texture2DArray.MipSlice) >= t->kmips) {
    ID3D12Device_CreateUnorderedAccessView(d->d12, NULL, NULL, NULL, at);
    return;
  }
  ID3D12Device_CreateUnorderedAccessView(d->d12, t->res, NULL, &x, at);
}

/* The Direct3D 12 view of a shader resource view of a texture (not a planar or packed YUV one): the dimension the program's names, its levels and layers within those Direct3D 12 below
 * keeps (Tex.kmips: a device without levels of layered images keeps level 0 of them), a one-dimensional view as a two-dimensional one. FALSE when the view shows nothing kept. */
/* Makes the Direct3D 12 view `r` of texture t at `at` with t's SetResourceMinLOD clamp as its ResourceMinLODClamp (the most detailed level its lookups take, in the texture's levels;
 * the kernel raises the sampler's MinLOD to it, counted from the view's first level). */
static void srv_create12(Dev *d, Tex *t, D3D12_SHADER_RESOURCE_VIEW_DESC *r, D3D12_CPU_DESCRIPTOR_HANDLE at) {
  const FLOAT clamp = t->min_lod;
  switch (r->ViewDimension) {
    case D3D12_SRV_DIMENSION_TEXTURE1D: r->Texture1D.ResourceMinLODClamp = clamp; break;
    case D3D12_SRV_DIMENSION_TEXTURE1DARRAY: r->Texture1DArray.ResourceMinLODClamp = clamp; break;
    case D3D12_SRV_DIMENSION_TEXTURE2D: r->Texture2D.ResourceMinLODClamp = clamp; break;
    case D3D12_SRV_DIMENSION_TEXTURE2DARRAY: r->Texture2DArray.ResourceMinLODClamp = clamp; break;
    case D3D12_SRV_DIMENSION_TEXTURE3D: r->Texture3D.ResourceMinLODClamp = clamp; break;
    case D3D12_SRV_DIMENSION_TEXTURECUBE: r->TextureCube.ResourceMinLODClamp = clamp; break;
    case D3D12_SRV_DIMENSION_TEXTURECUBEARRAY: r->TextureCubeArray.ResourceMinLODClamp = clamp; break;
    default: break;      /* (multisampled views have one level) */
  }
  ID3D12Device_CreateShaderResourceView(d->d12, t->res, r, at);
}
static BOOL srv_desc12(const View *v, D3D12_SHADER_RESOURCE_VIEW_DESC *r) {
  const Tex *t = v->tex;
  UINT first_level = 0, levels = 0, first = 0, count = 0;
  if (!srv_range(t, &v->desc.srv, &first_level, &levels, &first, &count)) return FALSE;
  if (first_level >= t->kmips) return FALSE;
  if (levels > t->kmips - first_level) levels = t->kmips - first_level;
  memset(r, 0, sizeof *r);
  r->Format = view_format12(t, v->desc.srv.Format);
  r->Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  switch (v->desc.srv.ViewDimension) {
    case D3D11_SRV_DIMENSION_TEXTURE1D: case D3D11_SRV_DIMENSION_TEXTURE2D:
      if (t->kind == 0) {
        r->ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        r->Texture2D.MostDetailedMip = first_level, r->Texture2D.MipLevels = levels;
        return TRUE;
      }
      /* (a slice of an array as a 2D texture: Direct3D 12's 2D array view of one slice, which a shader that declares a 2D texture reads) */
      r->ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
      r->Texture2DArray.MostDetailedMip = first_level, r->Texture2DArray.MipLevels = levels, r->Texture2DArray.FirstArraySlice = first, r->Texture2DArray.ArraySize = 1;
      return TRUE;
    case D3D11_SRV_DIMENSION_TEXTURE1DARRAY: case D3D11_SRV_DIMENSION_TEXTURE2DARRAY:
      r->ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
      r->Texture2DArray.MostDetailedMip = first_level, r->Texture2DArray.MipLevels = levels, r->Texture2DArray.FirstArraySlice = first, r->Texture2DArray.ArraySize = count;
      return TRUE;
    case D3D11_SRV_DIMENSION_TEXTURE2DMS: r->ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DMS; return TRUE;
    case D3D11_SRV_DIMENSION_TEXTURE2DMSARRAY:
      r->ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DMSARRAY;
      r->Texture2DMSArray.FirstArraySlice = first, r->Texture2DMSArray.ArraySize = count;
      return TRUE;
    case D3D11_SRV_DIMENSION_TEXTURE3D:
      r->ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
      r->Texture3D.MostDetailedMip = first_level, r->Texture3D.MipLevels = levels;
      return TRUE;
    case D3D11_SRV_DIMENSION_TEXTURECUBE:
      r->ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
      r->TextureCube.MostDetailedMip = first_level, r->TextureCube.MipLevels = levels;
      if (first == 0) return TRUE;
      /* (the first cube of a cube array is the cube view; another is a cube array view of one) */
      r->ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBEARRAY;
      r->TextureCubeArray.MostDetailedMip = first_level, r->TextureCubeArray.MipLevels = levels, r->TextureCubeArray.First2DArrayFace = first, r->TextureCubeArray.NumCubes = 1;
      return TRUE;
    case D3D11_SRV_DIMENSION_TEXTURECUBEARRAY:
      r->ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBEARRAY;
      r->TextureCubeArray.MostDetailedMip = first_level, r->TextureCubeArray.MipLevels = levels, r->TextureCubeArray.First2DArrayFace = first, r->TextureCubeArray.NumCubes = count / 6;
      return TRUE;
    default: return FALSE;
  }
}

/* The root signature of a draw or dispatch: of the narrow or wide table of shader resources (TEXTURE_SLOTS or SRV_SLOTS), and of the narrow or wide one of unordered access views
 * (UAV_TABLE_NARROW, or UAV_SLOTS where a shader declares one above u7). */
static UINT uav_table_count(UINT64 uav_mask) { return (uav_mask >> UAV_TABLE_NARROW) ? UAV_SLOTS : UAV_TABLE_NARROW; }
static ID3D12RootSignature *root_for(Dev *d, UINT view_count, UINT uav_count) {
  if (uav_count == UAV_SLOTS) return d->root_uavs[view_count == SRV_SLOTS];
  return view_count == SRV_SLOTS ? d->root_wide : d->root;
}
/* The tables of a draw or a dispatch: shader resources t0 .. t<view_count - 1> (TEXTURE_SLOTS, or SRV_SLOTS for the wide root signature; `kinds`: what the shaders declare
 * there, for what fills an empty slot), samplers s0 .. s7, and the unordered access views u0 .. u<uav_count - 1> (UAV_TABLE_NARROW or UAV_SLOTS: uav_table_count) of which the
 * shaders declare `uav_mask`. The handles go into Dev.table_*. `cache`: a table the same as the last one is used again. */
static BOOL tables_record(Dev *d, View *const *srvs, Sampler *const *samplers, const BYTE *kinds, View *const *uavs, UINT64 uav_mask, BOOL cache, UINT view_count) {
  const UINT uav_count = uav_table_count(uav_mask);
  UINT view_ids[SRV_SLOTS], sampler_ids[TEXTURE_SLOTS], uav_ids[UAV_SLOTS];
  FLOAT min_lods[SRV_SLOTS];
  for (UINT i = 0; i < view_count; ++i) {
    view_ids[i] = srvs[i] ? srvs[i]->c.id : kinds[i] == KIND_BUFFER ? d->blank_buffer->c.id : (kinds[i] >= 1 && kinds[i] <= 5 && d->blank_kind[kinds[i]] ? d->blank_kind[kinds[i]] : d->blank)->c.id;
    min_lods[i] = srvs[i] && srvs[i]->tex ? srvs[i]->tex->min_lod : 0.0f;      /* (SetResourceMinLOD: the view's ResourceMinLODClamp) */
  }
  for (UINT i = 0; i < TEXTURE_SLOTS; ++i) sampler_ids[i] = samplers[i] ? samplers[i]->s.c.id : 0;
  /* (an append or counter view whose count was set since: its counter is written now, in order with what the list did before; the table is made again) */
  for (UINT i = 0; i < uav_count; ++i)
    if ((uav_mask >> i & 1) && uavs[i] && uavs[i]->counter_buf && uavs[i]->counter_dirty) cache = FALSE;
  for (UINT i = 0; i < uav_count; ++i) uav_ids[i] = !(uav_mask >> i & 1) ? 0 : uavs[i] ? uavs[i]->c.id : d->blank_uav->c.id;
  const UINT step_views = ID3D12Device_GetDescriptorHandleIncrementSize(d->d12, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  const UINT step_samplers = ID3D12Device_GetDescriptorHandleIncrementSize(d->d12, D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
  const D3D12_CPU_DESCRIPTOR_HANDLE view_start = ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(d->view_heap);
  const D3D12_GPU_DESCRIPTOR_HANDLE view_gpu = ID3D12DescriptorHeap_GetGPUDescriptorHandleForHeapStart(d->view_heap);
  if (!cache || !d->tables_valid || d->table_view_count != view_count || memcmp(view_ids, d->table_view_ids, view_count * sizeof view_ids[0]) != 0 ||
      memcmp(min_lods, d->table_min_lods, view_count * sizeof min_lods[0]) != 0) {
    D3D12_CPU_DESCRIPTOR_HANDLE at = view_start;
    at.ptr += (SIZE_T)d->view_used * step_views;
    d->table_views = view_gpu.ptr + (UINT64)d->view_used * step_views;
    for (UINT i = 0; i < view_count; ++i) {
      View *v = srvs[i];
      if (v && v->buf) { if (!buffer_descriptor(d, v, FALSE, at)) return FALSE; }
      else if (!v && kinds[i] == 0xFF && i >= TEXTURE_SLOTS) ID3D12Device_CreateShaderResourceView(d->d12, NULL, NULL, at);      /* (the wide table past t7: nothing where no shader reads) */
      else if (!v && kinds[i] == KIND_BUFFER) {
        View blank = {.buf = d->blank_buffer, .offset = 0, .size = d->blank_buffer->desc.ByteWidth};
        buffer_descriptor(d, &blank, FALSE, at);
      } else if (v && v->tex->planar) {
        /* (a view of a plane of a slice - or of slices: a 2D array view of them, which Direct3D 12 below gives a shader that declares an array as a mirror of the
         * slices) */
        D3D12_SHADER_RESOURCE_VIEW_DESC r;
        memset(&r, 0, sizeof r);
        r.Format = v->plane_format;
        r.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        if (v->array_size) {
          r.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
          r.Texture2DArray.MipLevels = 1;
          r.Texture2DArray.FirstArraySlice = v->array_slice;
          r.Texture2DArray.ArraySize = v->array_size;
          r.Texture2DArray.PlaneSlice = v->plane;
        } else {
          r.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
          r.Texture2D.MipLevels = 1;
          r.Texture2D.PlaneSlice = v->plane;
        }
        ID3D12Device_CreateShaderResourceView(d->d12, v->tex->res, &r, at);
      } else if (v && v->tex->depth) {
        /* (a depth texture: a view of its depth, or of its stencil plane - PlaneSlice 1 -, in the format the texture below has it in (d3d11.c depth_view_format), of the view's levels
         * and layers) */
        D3D12_SHADER_RESOURCE_VIEW_DESC r;
        BOOL stencil = FALSE;
        if (srv_desc12(v, &r)) {
          depth_view_format(v->tex->desc.Format, v->desc.srv.Format, &r.Format, &stencil);
          if (r.ViewDimension == D3D12_SRV_DIMENSION_TEXTURE2D) r.Texture2D.PlaneSlice = stencil ? 1 : 0;
          if (r.ViewDimension == D3D12_SRV_DIMENSION_TEXTURE2DARRAY) r.Texture2DArray.PlaneSlice = stencil ? 1 : 0;
          srv_create12(d, v->tex, &r, at);
        } else {
          ID3D12Device_CreateShaderResourceView(d->d12, NULL, NULL, at);
        }
      } else if (v && (v->tex->fmt->flags & FMT_YUV)) {
        /* (a packed YUV texture: through a view of its pixels' format; of an array, its slices - each an image of its own below, a shader that declares an array
         * reading a mirror of them -, of a texture of levels, the view's levels) */
        D3D12_SHADER_RESOURCE_VIEW_DESC r;
        memset(&r, 0, sizeof r);
        r.Format = v->desc.srv.Format;
        r.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        const BOOL array = v->desc.srv.ViewDimension == D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
        const UINT slice = array ? v->desc.srv.Texture2DArray.FirstArraySlice : 0;
        UINT count = array ? v->desc.srv.Texture2DArray.ArraySize : 1;
        if (count == (UINT)-1 || count > v->tex->desc.ArraySize - slice) count = v->tex->desc.ArraySize - slice;
        if (v->tex->desc.ArraySize > 1) {
          r.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
          r.Texture2DArray.MipLevels = 1;
          r.Texture2DArray.FirstArraySlice = slice;
          r.Texture2DArray.ArraySize = count;
        } else {
          r.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
          r.Texture2D.MostDetailedMip = array ? v->desc.srv.Texture2DArray.MostDetailedMip : v->desc.srv.Texture2D.MostDetailedMip;
          r.Texture2D.MipLevels = array ? v->desc.srv.Texture2DArray.MipLevels : v->desc.srv.Texture2D.MipLevels;
        }
        srv_create12(d, v->tex, &r, at);
      } else if (v) {
        /* (the view's levels and layers, of the dimension it names: Direct3D 12 below gives the shader what the view shows - levels it does not keep are not there, Tex.kmips) */
        D3D12_SHADER_RESOURCE_VIEW_DESC r;
        if (srv_desc12(v, &r)) srv_create12(d, v->tex, &r, at);
        else ID3D12Device_CreateShaderResourceView(d->d12, v->tex->res, NULL, at);
      } else {
        Tex *t = kinds[i] >= 1 && kinds[i] <= 5 && d->blank_kind[kinds[i]] ? d->blank_kind[kinds[i]] : d->blank;
        ID3D12Device_CreateShaderResourceView(d->d12, t->res, NULL, at);
      }
      at.ptr += step_views;
    }
    d->view_used += view_count;
    memcpy(d->table_view_ids, view_ids, view_count * sizeof view_ids[0]);
    memcpy(d->table_min_lods, min_lods, view_count * sizeof min_lods[0]);
    d->table_view_count = view_count;
  }
  if (!cache || !d->tables_valid || d->table_uav_count != uav_count || memcmp(uav_ids, d->table_uav_ids, uav_count * sizeof uav_ids[0]) != 0) {
    D3D12_CPU_DESCRIPTOR_HANDLE at = view_start;
    at.ptr += (SIZE_T)d->view_used * step_views;
    d->table_uavs = view_gpu.ptr + (UINT64)d->view_used * step_views;
    for (UINT i = 0; i < uav_count; ++i) {
      View *v = !(uav_mask >> i & 1) ? NULL : uavs[i] ? uavs[i] : NULL;
      View blank = {.buf = d->blank_uav, .offset = 0, .size = d->blank_uav->desc.ByteWidth};
      if (v && v->buf) { if (!buffer_descriptor(d, v, TRUE, at)) return FALSE; }
      else if ((uav_mask >> i & 1) && !v) buffer_descriptor(d, &blank, TRUE, at);      /* (D3D11: what is written to nothing is lost; here it goes into a buffer of its own) */
      else if (v && v->tex) texture_uav_descriptor(d, v, at);
      else ID3D12Device_CreateUnorderedAccessView(d->d12, NULL, NULL, NULL, at);      /* (not declared) */
      at.ptr += step_views;
    }
    d->view_used += uav_count;
    memcpy(d->table_uav_ids, uav_ids, uav_count * sizeof uav_ids[0]);
    d->table_uav_count = uav_count;
  }
  if (!cache || !d->tables_valid || memcmp(sampler_ids, d->table_sampler_ids, sizeof sampler_ids) != 0) {
    static const D3D11_SAMPLER_DESC fallback = {D3D11_FILTER_MIN_MAG_MIP_LINEAR, D3D11_TEXTURE_ADDRESS_CLAMP, D3D11_TEXTURE_ADDRESS_CLAMP, D3D11_TEXTURE_ADDRESS_CLAMP, 0.0f, 1,
                                                D3D11_COMPARISON_NEVER, {0, 0, 0, 0}, 0.0f, 3.402823466e+38f};
    D3D12_CPU_DESCRIPTOR_HANDLE at = ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(d->sampler_heap);
    D3D12_GPU_DESCRIPTOR_HANDLE gpu = ID3D12DescriptorHeap_GetGPUDescriptorHandleForHeapStart(d->sampler_heap);
    at.ptr += (SIZE_T)d->sampler_used * step_samplers;
    d->table_samplers = gpu.ptr + (UINT64)d->sampler_used * step_samplers;
    for (UINT i = 0; i < TEXTURE_SLOTS; ++i) {
      const D3D11_SAMPLER_DESC desc = samplers[i] ? samplers[i]->desc : fallback;
      ID3D12Device_CreateSampler(d->d12, (const D3D12_SAMPLER_DESC *)&desc, at);      /* (the two structures are the same) */
      at.ptr += step_samplers;
    }
    d->sampler_used += TEXTURE_SLOTS;
    memcpy(d->table_sampler_ids, sampler_ids, sizeof sampler_ids);
  }
  d->tables_valid = cache;
  return TRUE;
}
/* What the tables name is used by the list: read (shader resources), and written (unordered access views, and the counters of append and counter views). */
static void tables_mark_used(Dev *d, View *const *srvs, View *const *uavs, UINT64 uav_mask) {
  for (UINT i = 0; i < SRV_SLOTS; ++i) {
    if (!srvs[i]) continue;
    if (srvs[i]->tex) srvs[i]->tex->used = srvs[i]->tex->sampled = d->epoch;
    else srvs[i]->buf->used = d->epoch;
  }
  for (UINT i = 0; i < UAV_SLOTS; ++i) {
    if (!(uav_mask >> i & 1) || !uavs[i]) continue;
    if (uavs[i]->tex) {
      /* (a texture the shaders write: what reads it later sends the list first; a render target's views load it again) */
      Tex *t = uavs[i]->tex;
      t->used = t->written = d->epoch;
      ++t->version;
      continue;
    }
    Buf *b = uavs[i]->buf;
    b->used = b->written = d->epoch;
    ++b->version;      /* (what the GPU writes: a converted copy of it is made again) */
    if (uavs[i]->counter_buf) uavs[i]->counter_buf->used = uavs[i]->counter_buf->written = d->epoch;
  }
}
/* Before a draw or dispatch records anything: the textures its unordered access views write are brought up to date (what their render target views drew) and, if the list
 * samples them, the list is sent first (the kernel keeps a texture bound the way a list first uses it). */
static void uav_textures_before(Dev *d, View *const *uavs, UINT64 uav_mask) {
  for (UINT i = 0; i < UAV_SLOTS; ++i) {
    if (!(uav_mask >> i & 1) || !uavs[i] || !uavs[i]->tex) continue;
    tex_sync(d, uavs[i]->tex);
    tex_before_write(d, uavs[i]->tex);
  }
}

/* ---- stream output -------------------------------------------------------------------------------------------------------------------------------------- */

/* The buffer d3d12.dll puts the bytes a stream output target holds into, after a list ran (an upload buffer: its memory is d3d12.dll's copy, which it writes). Each binding
 * of a list has a slot of its own (SO_FILL_SLOTS of 16 bytes: the filled size, where the view began), its filled size 0: Direct3D 12 starts a view's writes after the
 * filled size it has when the draws run, and the slot of an earlier binding of the list gets that binding's after its draws. */
#define SO_FILL_SLOTS 64u
static BOOL so_fill_buffer(Dev *d, Buf *b) {
  if (b->so_fill) return TRUE;
  b->so_fill = make_buffer(d, D3D12_HEAP_TYPE_UPLOAD, 16 * SO_FILL_SLOTS);
  if (!b->so_fill) return FALSE;
  if (FAILED(ID3D12Resource_Map(b->so_fill, 0, NULL, (void **)&b->so_fill_data)) || !b->so_fill_data) { ID3D12Resource_Release(b->so_fill); b->so_fill = NULL; return FALSE; }
  memset(b->so_fill_data, 0, 16 * SO_FILL_SLOTS);
  ID3D12Resource_Unmap(b->so_fill, 0, NULL);
  return TRUE;
}
/* The stream output targets into the list: each goes on from where its records end (Buf.so_end; SOSetTargets set that to the offset it was given). */
static void so_record(Dev *d, Ctx *c) {
  D3D12_STREAM_OUTPUT_BUFFER_VIEW views[4];
  memset(views, 0, sizeof views);
  for (UINT slot = 0; slot < 4; ++slot) {
    Buf *b = c->so_buf[slot];
    if (!b || !so_fill_buffer(d, b)) continue;
    const UINT64 at = b->so_end < b->desc.ByteWidth ? b->so_end : b->desc.ByteWidth;
    b->so_slot = b->written == d->epoch && b->so_slot + 1 < SO_FILL_SLOTS ? b->so_slot + 1 : 0;      /* (SOSetTargets flushes before the slots run out) */
    BYTE *fill = b->so_fill_data + 16 * b->so_slot;
    views[slot].BufferLocation = ID3D12Resource_GetGPUVirtualAddress(b->res) + at;
    views[slot].SizeInBytes = b->desc.ByteWidth - at;
    views[slot].BufferFilledSizeLocation = ID3D12Resource_GetGPUVirtualAddress(b->so_fill) + 16 * b->so_slot;
    memset(fill, 0, 8);      /* (nothing written after the view's start yet) */
    memcpy(fill + 8, &at, 8);      /* (bytes 8 ..: where the view began, for so_collect) */
    b->used = b->written = d->epoch;
    ++b->version;
    UINT k = 0;
    while (k < d->so_written_count && d->so_written[k] != b) ++k;
    if (k == d->so_written_count && k < 16) { hold(b); d->so_written[d->so_written_count++] = b; }
  }
  ID3D12GraphicsCommandList_SOSetTargets(d->list, 0, 4, views);
  d->recorded = 1;
}
/* After a list ran: where the records of each target it wrote end now. */
static void so_collect(Dev *d) {
  for (UINT i = 0; i < d->so_written_count; ++i) {
    Buf *b = d->so_written[i];
    UINT64 filled = 0, began = 0;
    memcpy(&filled, b->so_fill_data + 16 * b->so_slot, 8);      /* (the last binding of the list's) */
    memcpy(&began, b->so_fill_data + 16 * b->so_slot + 8, 8);
    b->so_end = began + filled;
    unhold(b);
  }
  d->so_written_count = 0;
}
/* Before what reads where a target's records end: the list that writes them is sent. */
static void so_settle(Dev *d, const Buf *b) { if (b->so_fill && b->written == d->epoch) flush(d); }

static void STDMETHODCALLTYPE ctx_SOSetTargets(Ctx *c, UINT count, ID3D11Buffer *const *buffers, const UINT *offsets) {
  if (c->deferred) {
    if (count > 4) return;
    RecArgs a = {.op = REC_SO_TARGETS, .u = {count}};
    for (UINT slot = 0; slot < 4; ++slot) {
      Buf *b = slot < count && buffers ? (Buf *)buffers[slot] : NULL;
      if (b && !(b->desc.BindFlags & D3D11_BIND_STREAM_OUTPUT)) b = NULL;
      if (b) {
        for (UINT i = 0; i < VERTEX_SLOTS; ++i) if (c->vb[i] == b) bind(&c->vb[i], NULL);
        if (c->ib == b) bind(&c->ib, NULL);
        unbind_reads_of(c, b, FALSE);
      }
      bind(&c->so_buf[slot], b);
      a.obj[slot] = b;
      a.u[1 + slot] = offsets && slot < count ? offsets[slot] : 0;
    }
    deferred_record(c, &a);
    return;
  }
  Dev *d = c->dev;
  if (count > 4) return;
  LOCK(c);
  for (UINT slot = 0; slot < 4; ++slot) {
    Buf *b = slot < count && buffers ? (Buf *)buffers[slot] : NULL;
    if (b && !(b->desc.BindFlags & D3D11_BIND_STREAM_OUTPUT)) b = NULL;
    if (b) {
      const UINT offset = offsets ? offsets[slot] : 0;
      if (b->so_fill && b->written == d->epoch && b->so_slot + 1 >= SO_FILL_SLOTS) flush(d);      /* (the list has used every slot of its filled sizes) */
      if (offset == (UINT)-1) so_settle(d, b);      /* (-1: on from where its records end) */
      else b->so_end = offset;
      /* (a buffer written to is no input: its vertex, index and shader resource bindings are taken away) */
      for (UINT i = 0; i < VERTEX_SLOTS; ++i) if (c->vb[i] == b) bind(&c->vb[i], NULL);
      if (c->ib == b) bind(&c->ib, NULL);
      unbind_reads_of(c, b, FALSE);
    }
    bind(&c->so_buf[slot], b);
  }
  d->dirty |= DIRTY_SO;
  UNLOCK(c);
}
static void STDMETHODCALLTYPE ctx_SOGetTargets(Ctx *c, UINT count, ID3D11Buffer **buffers) { get_range((void **)c->so_buf, 4, 0, count, (void **)buffers); }

/* Everything of a draw but the draw: FALSE when it cannot be drawn (no vertex shader, no render target, a pipeline the kernel does not take). */
static BOOL draw_prepare(Ctx *c, BOOL indexed) {
  Dev *d = c->dev;
  const BOOL streams = c->gs && c->gs->so;
  /* (d3d12.dll gives a pass without a colour target one: of the depth buffer's size, of the viewport's when the draw writes unordered access views only, a tiny one for stream
   * output alone) */
  const BOOL uav_only = c->rtv_count == 0 && !c->dsv && ((c->ps && c->ps->uav_mask) || (c->vs && c->vs->uav_mask) || (c->gs && c->gs->uav_mask) || (c->hs && c->hs->uav_mask) ||
                                                        (c->ds && c->ds->uav_mask));      /* (UAV-only rendering: the shaders of any stage write unordered access views) */
  if (!c->vs || (c->rtv_count == 0 && !c->dsv && !streams && !uav_only) || (indexed && !c->ib)) return FALSE;
  if (!c->hs != !c->ds) return FALSE;      /* (tessellation needs both) */

  /* what may have to send the list first */
  ID3D12Resource *sources[VERTEX_SLOTS] = {0};
  UINT strides[VERTEX_SLOTS], offsets[VERTEX_SLOTS], sizes[VERTEX_SLOTS];
  for (UINT slot = 0; slot < VERTEX_SLOTS; ++slot) {
    if (!c->vb[slot]) continue;
    sources[slot] = vertex_source(d, c, slot, &strides[slot], &offsets[slot], &sizes[slot]);
    if (!sources[slot]) return FALSE;
  }
  /* (the table of shader resources: the wide one when a stage declares one above t7) */
  const Shader *declaring[5] = {c->vs, c->ps, c->gs, c->hs, c->ds};
  UINT view_count = TEXTURE_SLOTS;
  for (UINT k = 0; k < 5; ++k) if (declaring[k] && declaring[k]->wide) view_count = SRV_SLOTS;
  /* (what views drew into the textures sampled goes into them first: rtshadow.inc) */
  for (UINT i = 0; i < view_count; ++i) if (slot_view(c, i) && slot_view(c, i)->tex) tex_sync(d, slot_view(c, i)->tex);
  /* (a texture the list's draws sampled cannot become its target: the kernel keeps it bound as a texture for the whole list; a view with a target of its own is
   * drawn into there, loaded from the texture when that changed) */
  for (UINT i = 0; i < c->rtv_count; ++i) {
    if (c->rtv[i]->scratch) rtv_load(d, c->rtv[i], FALSE);
    else tex_before_write(d, c->rtv[i]->tex);
  }
  if (c->dsv) tex_before_write(d, c->dsv->tex);
  /* (... nor be sampled after the list has drawn into it, cleared it or copied into it: the draw would see the texture as it was before the list) */
  for (UINT i = 0; i < view_count; ++i) if (slot_view(c, i) && slot_view(c, i)->tex) tex_before_read(d, slot_view(c, i)->tex);
  {
    UINT64 uav_mask = 0;
    const Shader *writers[5] = {c->vs, c->ps, c->gs, c->hs, c->ds};
    for (UINT k = 0; k < 5; ++k) if (writers[k]) uav_mask |= writers[k]->uav_mask;
    uav_textures_before(d, c->ps_uav, uav_mask);
  }
  if (d->ring_used + CONSTANT_BLOCK > RING_BYTES || d->view_used + SRV_SLOTS + UAV_SLOTS > VIEW_HEAP || d->sampler_used + TEXTURE_SLOTS > VIEW_HEAP) flush(d);
  ID3D12PipelineState *pipeline = pipeline_get(d, c);
  if (!pipeline) return FALSE;

  ID3D12GraphicsCommandList *list = d->list;
  UINT64 stage_uavs = 0;
  {
    const Shader *writers[5] = {c->vs, c->ps, c->gs, c->hs, c->ds};
    for (UINT k = 0; k < 5; ++k) if (writers[k]) stage_uavs |= writers[k]->uav_mask;
  }
  ID3D12RootSignature *root = root_for(d, view_count, uav_table_count(stage_uavs));
  if (d->bound_root != root) d->dirty |= DIRTY_ROOT;
  if (d->dirty & DIRTY_ROOT) {
    ID3D12DescriptorHeap *heaps[2] = {d->view_heap, d->sampler_heap};
    ID3D12GraphicsCommandList_SetGraphicsRootSignature(list, root);
    d->bound_root = root;
    ID3D12GraphicsCommandList_SetDescriptorHeaps(list, 2, heaps);
  }
  if (d->dirty & DIRTY_TARGETS) {
    D3D12_CPU_DESCRIPTOR_HANDLE targets[8];
    for (UINT i = 0; i < c->rtv_count; ++i) targets[i] = c->rtv[i]->handle;
    ID3D12GraphicsCommandList_OMSetRenderTargets(list, c->rtv_count, targets, FALSE, c->dsv ? &c->dsv->handle : NULL);
  }
  if (d->dirty & (DIRTY_VIEWPORT | DIRTY_TARGETS)) {
    static const Tex none = {.desc = {1, 1}};
    const Tex *target = c->rtv_count ? c->rtv[0]->tex : c->dsv ? c->dsv->tex : &none;
    const UINT tw = c->rtv_count ? c->rtv[0]->width : target->desc.Width, th = c->rtv_count ? c->rtv[0]->height : target->desc.Height;      /* (the level a view names) */
    /* (the viewports bound - the whole target when none is -, and for each the scissor rectangle of the same number when the rasterizer state has the scissor test, else all
     * of the target; with the test a rectangle not set lets nothing pass. With no target at all the viewport says what is rasterized: its scissor rectangle is all of it) */
    D3D12_VIEWPORT vp[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
    D3D12_RECT sc[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
    const UINT n = c->viewport_count ? c->viewport_count : 1;
    for (UINT i = 0; i < n; ++i) {
      const D3D12_VIEWPORT whole = {0, 0, (FLOAT)tw, (FLOAT)th, 0, 1};
      const D3D12_RECT all = {0, 0, (LONG)tw, (LONG)th}, none = {0, 0, 0, 0};
      vp[i] = whole;
      if (i < c->viewport_count) memcpy(&vp[i], &c->viewports[i], sizeof vp[i]);      /* (the two structures are the same) */
      sc[i] = all;
      if (!c->rtv_count && !c->dsv && i < c->viewport_count) {
        sc[i].right = (LONG)(vp[i].TopLeftX + vp[i].Width + 0.999f);
        sc[i].bottom = (LONG)(vp[i].TopLeftY + vp[i].Height + 0.999f);
      }
      if (c->raster && c->raster->desc.ScissorEnable) {
        if (i < c->scissor_count) memcpy(&sc[i], &c->scissors[i], sizeof sc[i]);
        else sc[i] = none;
      }
    }
    ID3D12GraphicsCommandList_RSSetViewports(list, n, vp);
    ID3D12GraphicsCommandList_RSSetScissorRects(list, n, sc);
  }
  ID3D12GraphicsCommandList_SetPipelineState(list, pipeline);
  if ((d->dirty & DIRTY_SO) && streams) so_record(d, c);
  ID3D12GraphicsCommandList_IASetPrimitiveTopology(list, (D3D12_PRIMITIVE_TOPOLOGY)c->topology);
  ID3D12GraphicsCommandList_OMSetStencilRef(list, c->stencil_ref);
  ID3D12GraphicsCommandList_OMSetBlendFactor(list, c->blend_factor);
  for (UINT slot = 0; slot < VERTEX_SLOTS; ++slot) {
    if (!sources[slot]) {
      /* (no buffer: a slot the layout reads gives 0 - the components its format lacks 0, 0, 0, 1 -, as Direct3D 11's unbound input slot; Direct3D 12's null view, not
       * the buffer an earlier draw of the list bound there) */
      ID3D12GraphicsCommandList_IASetVertexBuffers(list, slot, 1, NULL);
      continue;
    }
    D3D12_VERTEX_BUFFER_VIEW view = {ID3D12Resource_GetGPUVirtualAddress(sources[slot]) + offsets[slot], sizes[slot], strides[slot]};
    ID3D12GraphicsCommandList_IASetVertexBuffers(list, slot, 1, &view);
    if (sources[slot] == c->vb[slot]->converted) c->vb[slot]->converted_used = d->epoch;
    else c->vb[slot]->used = d->epoch;
  }
  if (indexed) {
    const UINT offset = c->ib_offset < c->ib->desc.ByteWidth ? c->ib_offset : 0;
    D3D12_INDEX_BUFFER_VIEW view = {ID3D12Resource_GetGPUVirtualAddress(c->ib->res) + offset, c->ib->desc.ByteWidth - offset,
                                    c->ib_format == DXGI_FORMAT_R32_UINT ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_R16_UINT};
    ID3D12GraphicsCommandList_IASetIndexBuffer(list, &view);
    ID3D12GraphicsCommandList9_IASetIndexBufferStripCutValue((ID3D12GraphicsCommandList9 *)list, view.Format == DXGI_FORMAT_R32_UINT ? D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_0xFFFFFFFF
                                                                                                                                       : D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_0xFFFF);
    c->ib->used = d->epoch;
  }

  /* the constants: the vertex shader's buffers in slot order, then the pixel shader's, the geometry shader's, the hull shader's and the domain shader's */
  const Shader *const stages[5] = {c->vs, c->ps, c->gs && !c->gs->so_only ? c->gs : NULL, c->hs, c->ds};
  static const UINT stage_of[5] = {0, 1, 2, 3, 4};
  constants_record(d, c, stages, stage_of, 5);
  ID3D12GraphicsCommandList_SetGraphicsRootConstantBufferView(list, 0, ID3D12Resource_GetGPUVirtualAddress(d->ring) + d->ring_last);

  /* the textures (and buffer views) and the samplers of the stages, and the pixel shader's unordered access views: a table of each, made again when what is bound
   * changed */
  View *srvs[SRV_SLOTS];
  Sampler *samplers_of[TEXTURE_SLOTS];
  BYTE kinds[SRV_SLOTS];
  for (UINT i = 0; i < view_count; ++i) {
    srvs[i] = slot_view(c, i);
    kinds[i] = slot_declared_kind(c, i);
  }
  for (UINT i = view_count; i < SRV_SLOTS; ++i) srvs[i] = NULL;
  for (UINT i = 0; i < TEXTURE_SLOTS; ++i) samplers_of[i] = slot_sampler(c, i);
  UINT64 uav_mask = 0;
  const Shader *graphics_stages[5] = {c->vs, c->ps, c->gs, c->hs, c->ds};
  for (UINT k = 0; k < 5; ++k) if (graphics_stages[k]) uav_mask |= graphics_stages[k]->uav_mask;
  if (!tables_record(d, srvs, samplers_of, kinds, c->ps_uav, uav_mask, TRUE, view_count)) return FALSE;
  D3D12_GPU_DESCRIPTOR_HANDLE views = {d->table_views}, samplers = {d->table_samplers}, uavs = {d->table_uavs};
  ID3D12GraphicsCommandList_SetGraphicsRootDescriptorTable(list, 1, views);
  ID3D12GraphicsCommandList_SetGraphicsRootDescriptorTable(list, 2, samplers);
  ID3D12GraphicsCommandList_SetGraphicsRootDescriptorTable(list, 3, uavs);
  tables_mark_used(d, srvs, c->ps_uav, uav_mask);

  for (UINT i = 0; i < c->rtv_count; ++i) c->rtv[i]->tex->used = c->rtv[i]->tex->written = d->epoch;
  if (c->dsv) {
    c->dsv->tex->used = c->dsv->tex->written = d->epoch;
    for (UINT i = 0; i < d->clear_count;) {      /* (its clear is done by the pass this draw begins) */
      if (d->clears[i].dsv == c->dsv) d->clears[i] = d->clears[--d->clear_count]; else ++i;
    }
  }
  d->dirty = streams ? 0 : d->dirty & DIRTY_SO;      /* (the targets go into the list with the first draw that writes them) */
  d->recorded = 1;
  return TRUE;
}

/* ---- the context's own methods --------------------------------------------------------------------------------------------------------------------- */

/* The device holds the context (one reference); the first reference a program takes holds the device, the last one it gives back lets it go. */
static ULONG STDMETHODCALLTYPE ctx_AddRef(Ctx *c) {
  if (c->deferred) return dx_AddRef(OBJ(c));      /* (a deferred context is an object of its own, which holds the device) */
  const LONG n = InterlockedIncrement(&c->o.ref);
  if (n == 2) dx_AddRef(OBJ(c->dev));
  return (ULONG)(n - 1);
}
static ULONG STDMETHODCALLTYPE ctx_Release(Ctx *c) {
  if (c->deferred) return dx_Release(OBJ(c));
  const LONG n = InterlockedDecrement(&c->o.ref);
  if (n == 1) dx_Release(OBJ(c->dev));      /* (may be the device's last reference: the context goes with it) */
  return (ULONG)(n - 1);
}
static HRESULT STDMETHODCALLTYPE ctx_QueryInterface(Ctx *c, REFIID riid, void **out) {
  if (!out) return E_POINTER;
  *out = NULL;
  void *face = NULL;
  if (guid_eq(riid, &IID_IUnknown) || guid_eq(riid, &IID_ID3D11DeviceChild) || guid_eq(riid, &IID_ID3D11DeviceContext) || guid_eq(riid, &IID_ID3D11DeviceContext1) ||
      guid_eq(riid, &IID_ID3D11DeviceContext2) || guid_eq(riid, &IID_ID3D11DeviceContext3) || guid_eq(riid, &IID_ID3D11DeviceContext4))
    face = c;
  else if (guid_eq(riid, &IID_ID3D11Multithread) && !c->deferred) face = &c->multithread;      /* (the immediate context's only) */
  else if (guid_eq(riid, &IID_ID3DUserDefinedAnnotation)) face = &c->annotation;
  else if ((guid_eq(riid, &IID_ID3D11VideoContext) || guid_eq(riid, &IID_ID3D11VideoContext1) || guid_eq(riid, &IID_ID3D11VideoContext2) || guid_eq(riid, &IID_ID3D11VideoContext3)) &&
           !c->deferred && c->video.lpVtbl)
    face = &c->video;      /* (video.inc: the immediate context's only, as on Windows) */
  if (!face) {
    dx_no_interface("ID3D11DeviceContext", riid);
    return E_NOINTERFACE;
  }
  ctx_AddRef(c);
  *out = face;
  return S_OK;
}
static void STDMETHODCALLTYPE ctx_GetDevice(Ctx *c, ID3D11Device **out) {
  if (!out) return;
  dx_AddRef(OBJ(c->dev));
  *out = (ID3D11Device *)c->dev;
}
static D3D11_DEVICE_CONTEXT_TYPE STDMETHODCALLTYPE ctx_GetType(Ctx *c) { return c->deferred ? D3D11_DEVICE_CONTEXT_DEFERRED : D3D11_DEVICE_CONTEXT_IMMEDIATE; }
static UINT STDMETHODCALLTYPE ctx_GetContextFlags(Ctx *c) { return c->flags; }

static void STDMETHODCALLTYPE ctx_VSSetShader(Ctx *c, ID3D11VertexShader *shader, ID3D11ClassInstance *const *instances, UINT count) {
  bind(&c->vs, shader);
  set_instances(c, 0, instances, count);
}
static void STDMETHODCALLTYPE ctx_PSSetShader(Ctx *c, ID3D11PixelShader *shader, ID3D11ClassInstance *const *instances, UINT count) {
  bind(&c->ps, shader);
  set_instances(c, 1, instances, count);
}
static void STDMETHODCALLTYPE ctx_GSSetShader(Ctx *c, ID3D11GeometryShader *shader, ID3D11ClassInstance *const *instances, UINT count) {
  bind(&c->gs, shader);
  set_instances(c, 2, instances, count);
}
static void STDMETHODCALLTYPE ctx_HSSetShader(Ctx *c, ID3D11HullShader *shader, ID3D11ClassInstance *const *instances, UINT count) {
  bind(&c->hs, shader);
  set_instances(c, 3, instances, count);
}
static void STDMETHODCALLTYPE ctx_DSSetShader(Ctx *c, ID3D11DomainShader *shader, ID3D11ClassInstance *const *instances, UINT count) {
  bind(&c->ds, shader);
  set_instances(c, 4, instances, count);
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
/* The constant buffers of a stage (0 vertex, 1 pixel, 2 geometry, 3 hull, 4 domain, 5 compute shader), and the window of each a stage reads
 * (*SetConstantBuffers1: from constant `first`, `number` constants, each of 16 bytes; without it all of the buffer). */
static Buf **cb_slots(Ctx *c, UINT stage) {
  return stage == 0 ? c->vs_cb : stage == 1 ? c->ps_cb : stage == 5 ? c->cs_cb : c->stage_cb[stage - 2];
}
static void cb_set(Ctx *c, UINT stage, UINT start, UINT count, ID3D11Buffer *const *buffers, const UINT *first, const UINT *number) {
  if (start >= CONSTANT_SLOTS || count > CONSTANT_SLOTS - start) return;
  if ((first != NULL) != (number != NULL)) return;
  for (UINT i = 0; first && i < count; ++i)
    if (buffers && buffers[i] && (first[i] % 16 || number[i] % 16 || number[i] == 0 || number[i] > 4096)) return;      /* (windows of whole 256 bytes, at most 4096 constants) */
  Buf **slots = cb_slots(c, stage);
  for (UINT i = 0; i < count; ++i) {
    bind(&slots[start + i], buffers ? buffers[i] : NULL);
    c->cb_first[stage][start + i] = first ? first[i] : 0;
    c->cb_number[stage][start + i] = first ? number[i] : 4096;
  }
}
static void cb_get(Ctx *c, UINT stage, UINT start, UINT count, ID3D11Buffer **buffers, UINT *first, UINT *number) {
  Buf **slots = cb_slots(c, stage);
  for (UINT i = 0; i < count; ++i) {
    const BOOL in = start + i < CONSTANT_SLOTS;
    if (buffers) { buffers[i] = in ? (ID3D11Buffer *)slots[start + i] : NULL; if (buffers[i]) dx_AddRef(OBJ(buffers[i])); }
    if (first) first[i] = in && slots[start + i] ? c->cb_first[stage][start + i] : 0;
    if (number) number[i] = in && slots[start + i] ? c->cb_number[stage][start + i] : 0;
  }
}
static void STDMETHODCALLTYPE ctx_VSSetConstantBuffers(Ctx *c, UINT start, UINT count, ID3D11Buffer *const *buffers) { cb_set(c, 0, start, count, buffers, NULL, NULL); }
static void STDMETHODCALLTYPE ctx_VSSetConstantBuffers1(Ctx *c, UINT start, UINT count, ID3D11Buffer *const *buffers, const UINT *first, const UINT *number) { cb_set(c, 0, start, count, buffers, first, number); }
static void STDMETHODCALLTYPE ctx_PSSetConstantBuffers(Ctx *c, UINT start, UINT count, ID3D11Buffer *const *buffers) { cb_set(c, 1, start, count, buffers, NULL, NULL); }
static void STDMETHODCALLTYPE ctx_PSSetConstantBuffers1(Ctx *c, UINT start, UINT count, ID3D11Buffer *const *buffers, const UINT *first, const UINT *number) { cb_set(c, 1, start, count, buffers, first, number); }
static void set_resources(Ctx *c, View **slots, UINT start, UINT count, ID3D11ShaderResourceView *const *views, BOOL compute);
static void STDMETHODCALLTYPE ctx_PSSetShaderResources(Ctx *c, UINT start, UINT count, ID3D11ShaderResourceView *const *views) { set_resources(c, c->srv, start, count, views, FALSE); }
static void STDMETHODCALLTYPE ctx_PSSetSamplers(Ctx *c, UINT start, UINT count, ID3D11SamplerState *const *samplers) { set_range((void **)c->sampler, TEXTURE_SLOTS, start, count, (void *const *)samplers); }
static void STDMETHODCALLTYPE ctx_VSGetConstantBuffers(Ctx *c, UINT start, UINT count, ID3D11Buffer **buffers) { cb_get(c, 0, start, count, buffers, NULL, NULL); }
static void STDMETHODCALLTYPE ctx_VSGetConstantBuffers1(Ctx *c, UINT start, UINT count, ID3D11Buffer **buffers, UINT *first, UINT *number) { cb_get(c, 0, start, count, buffers, first, number); }
static void STDMETHODCALLTYPE ctx_PSGetConstantBuffers(Ctx *c, UINT start, UINT count, ID3D11Buffer **buffers) { cb_get(c, 1, start, count, buffers, NULL, NULL); }
static void STDMETHODCALLTYPE ctx_PSGetConstantBuffers1(Ctx *c, UINT start, UINT count, ID3D11Buffer **buffers, UINT *first, UINT *number) { cb_get(c, 1, start, count, buffers, first, number); }
static void STDMETHODCALLTYPE ctx_PSGetShaderResources(Ctx *c, UINT start, UINT count, ID3D11ShaderResourceView **views) { get_range((void **)c->srv, SRV_SLOTS, start, count, (void **)views); }
static void STDMETHODCALLTYPE ctx_PSGetSamplers(Ctx *c, UINT start, UINT count, ID3D11SamplerState **samplers) { get_range((void **)c->sampler, TEXTURE_SLOTS, start, count, (void **)samplers); }
/* the other stages' constant buffers, textures and samplers (written out: scripts/gen-dx-vtables.py finds the methods by their names) */
static void STDMETHODCALLTYPE ctx_VSSetShaderResources(Ctx *c, UINT start, UINT count, ID3D11ShaderResourceView *const *views) { set_resources(c, c->stage_srv[SRV_VS], start, count, views, FALSE); }
static void STDMETHODCALLTYPE ctx_VSGetShaderResources(Ctx *c, UINT start, UINT count, ID3D11ShaderResourceView **views) { get_range((void **)c->stage_srv[SRV_VS], SRV_SLOTS, start, count, (void **)views); }
static void STDMETHODCALLTYPE ctx_VSSetSamplers(Ctx *c, UINT start, UINT count, ID3D11SamplerState *const *samplers) { set_range((void **)c->stage_sampler[SRV_VS], TEXTURE_SLOTS, start, count, (void *const *)samplers); }
static void STDMETHODCALLTYPE ctx_VSGetSamplers(Ctx *c, UINT start, UINT count, ID3D11SamplerState **samplers) { get_range((void **)c->stage_sampler[SRV_VS], TEXTURE_SLOTS, start, count, (void **)samplers); }
static void STDMETHODCALLTYPE ctx_GSSetShaderResources(Ctx *c, UINT start, UINT count, ID3D11ShaderResourceView *const *views) { set_resources(c, c->stage_srv[SRV_GS], start, count, views, FALSE); }
static void STDMETHODCALLTYPE ctx_GSGetShaderResources(Ctx *c, UINT start, UINT count, ID3D11ShaderResourceView **views) { get_range((void **)c->stage_srv[SRV_GS], SRV_SLOTS, start, count, (void **)views); }
static void STDMETHODCALLTYPE ctx_GSSetSamplers(Ctx *c, UINT start, UINT count, ID3D11SamplerState *const *samplers) { set_range((void **)c->stage_sampler[SRV_GS], TEXTURE_SLOTS, start, count, (void *const *)samplers); }
static void STDMETHODCALLTYPE ctx_GSGetSamplers(Ctx *c, UINT start, UINT count, ID3D11SamplerState **samplers) { get_range((void **)c->stage_sampler[SRV_GS], TEXTURE_SLOTS, start, count, (void **)samplers); }
static void STDMETHODCALLTYPE ctx_HSSetShaderResources(Ctx *c, UINT start, UINT count, ID3D11ShaderResourceView *const *views) { set_resources(c, c->stage_srv[SRV_HS], start, count, views, FALSE); }
static void STDMETHODCALLTYPE ctx_HSGetShaderResources(Ctx *c, UINT start, UINT count, ID3D11ShaderResourceView **views) { get_range((void **)c->stage_srv[SRV_HS], SRV_SLOTS, start, count, (void **)views); }
static void STDMETHODCALLTYPE ctx_HSSetSamplers(Ctx *c, UINT start, UINT count, ID3D11SamplerState *const *samplers) { set_range((void **)c->stage_sampler[SRV_HS], TEXTURE_SLOTS, start, count, (void *const *)samplers); }
static void STDMETHODCALLTYPE ctx_HSGetSamplers(Ctx *c, UINT start, UINT count, ID3D11SamplerState **samplers) { get_range((void **)c->stage_sampler[SRV_HS], TEXTURE_SLOTS, start, count, (void **)samplers); }
static void STDMETHODCALLTYPE ctx_DSSetShaderResources(Ctx *c, UINT start, UINT count, ID3D11ShaderResourceView *const *views) { set_resources(c, c->stage_srv[SRV_DS], start, count, views, FALSE); }
static void STDMETHODCALLTYPE ctx_DSGetShaderResources(Ctx *c, UINT start, UINT count, ID3D11ShaderResourceView **views) { get_range((void **)c->stage_srv[SRV_DS], SRV_SLOTS, start, count, (void **)views); }
static void STDMETHODCALLTYPE ctx_DSSetSamplers(Ctx *c, UINT start, UINT count, ID3D11SamplerState *const *samplers) { set_range((void **)c->stage_sampler[SRV_DS], TEXTURE_SLOTS, start, count, (void *const *)samplers); }
static void STDMETHODCALLTYPE ctx_DSGetSamplers(Ctx *c, UINT start, UINT count, ID3D11SamplerState **samplers) { get_range((void **)c->stage_sampler[SRV_DS], TEXTURE_SLOTS, start, count, (void **)samplers); }
static void STDMETHODCALLTYPE ctx_GSSetConstantBuffers(Ctx *c, UINT start, UINT count, ID3D11Buffer *const *buffers) { cb_set(c, 2, start, count, buffers, NULL, NULL); }
static void STDMETHODCALLTYPE ctx_GSSetConstantBuffers1(Ctx *c, UINT start, UINT count, ID3D11Buffer *const *buffers, const UINT *first, const UINT *number) { cb_set(c, 2, start, count, buffers, first, number); }
static void STDMETHODCALLTYPE ctx_GSGetConstantBuffers(Ctx *c, UINT start, UINT count, ID3D11Buffer **buffers) { cb_get(c, 2, start, count, buffers, NULL, NULL); }
static void STDMETHODCALLTYPE ctx_GSGetConstantBuffers1(Ctx *c, UINT start, UINT count, ID3D11Buffer **buffers, UINT *first, UINT *number) { cb_get(c, 2, start, count, buffers, first, number); }
static void STDMETHODCALLTYPE ctx_HSSetConstantBuffers(Ctx *c, UINT start, UINT count, ID3D11Buffer *const *buffers) { cb_set(c, 3, start, count, buffers, NULL, NULL); }
static void STDMETHODCALLTYPE ctx_HSSetConstantBuffers1(Ctx *c, UINT start, UINT count, ID3D11Buffer *const *buffers, const UINT *first, const UINT *number) { cb_set(c, 3, start, count, buffers, first, number); }
static void STDMETHODCALLTYPE ctx_HSGetConstantBuffers(Ctx *c, UINT start, UINT count, ID3D11Buffer **buffers) { cb_get(c, 3, start, count, buffers, NULL, NULL); }
static void STDMETHODCALLTYPE ctx_HSGetConstantBuffers1(Ctx *c, UINT start, UINT count, ID3D11Buffer **buffers, UINT *first, UINT *number) { cb_get(c, 3, start, count, buffers, first, number); }
static void STDMETHODCALLTYPE ctx_DSSetConstantBuffers(Ctx *c, UINT start, UINT count, ID3D11Buffer *const *buffers) { cb_set(c, 4, start, count, buffers, NULL, NULL); }
static void STDMETHODCALLTYPE ctx_DSSetConstantBuffers1(Ctx *c, UINT start, UINT count, ID3D11Buffer *const *buffers, const UINT *first, const UINT *number) { cb_set(c, 4, start, count, buffers, first, number); }
static void STDMETHODCALLTYPE ctx_DSGetConstantBuffers(Ctx *c, UINT start, UINT count, ID3D11Buffer **buffers) { cb_get(c, 4, start, count, buffers, NULL, NULL); }
static void STDMETHODCALLTYPE ctx_DSGetConstantBuffers1(Ctx *c, UINT start, UINT count, ID3D11Buffer **buffers, UINT *first, UINT *number) { cb_get(c, 4, start, count, buffers, first, number); }
/* The class instances bound with a stage's shader, by interface slot (linkage.inc specialises the shader for them). Only class instances of this
 * library are taken (another object in the array: the slot is empty). */
static void set_instances(Ctx *c, UINT stage, ID3D11ClassInstance *const *instances, UINT count) {
  extern const ID3D11ClassInstanceVtbl inst_vtbl;
  if (!instances || count > D3D11_SHADER_MAX_INTERFACES) count = 0;
  for (UINT i = 0; i < D3D11_SHADER_MAX_INTERFACES; ++i) {
    ID3D11ClassInstance *in = i < count ? instances[i] : NULL;
    if (in && ((DxObj *)in)->lpVtbl != (const void *)&inst_vtbl) in = NULL;
    if (i >= count && i >= c->instance_count[stage] && !c->instances[stage][i]) break;      /* (nothing above: nothing to undo) */
    bind(&c->instances[stage][i], in);
  }
  c->instance_count[stage] = count;
}
/* (*count in: the room in `instances`; out: how many are bound) */
static void get_instances(Ctx *c, UINT stage, ID3D11ClassInstance **instances, UINT *count) {
  if (!count) return;
  const UINT have = c->instance_count[stage];
  for (UINT i = 0; instances && i < *count; ++i) {
    instances[i] = i < have ? (ID3D11ClassInstance *)c->instances[stage][i] : NULL;
    if (instances[i]) dx_AddRef(OBJ(instances[i]));
  }
  *count = have;
}
static void get_shader(void *shader, void **out, ID3D11ClassInstance **instances, UINT *count) {
  (void)instances;
  if (count) *count = 0;
  if (!out) return;
  *out = shader;
  if (shader) dx_AddRef(OBJ(shader));
}
static void STDMETHODCALLTYPE ctx_VSGetShader(Ctx *c, ID3D11VertexShader **out, ID3D11ClassInstance **instances, UINT *count) { get_shader(c->vs, (void **)out, NULL, NULL); get_instances(c, 0, instances, count); }
static void STDMETHODCALLTYPE ctx_PSGetShader(Ctx *c, ID3D11PixelShader **out, ID3D11ClassInstance **instances, UINT *count) { get_shader(c->ps, (void **)out, NULL, NULL); get_instances(c, 1, instances, count); }
static void STDMETHODCALLTYPE ctx_GSGetShader(Ctx *c, ID3D11GeometryShader **out, ID3D11ClassInstance **instances, UINT *count) { get_shader(c->gs, (void **)out, NULL, NULL); get_instances(c, 2, instances, count); }
static void STDMETHODCALLTYPE ctx_HSGetShader(Ctx *c, ID3D11HullShader **out, ID3D11ClassInstance **instances, UINT *count) { get_shader(c->hs, (void **)out, NULL, NULL); get_instances(c, 3, instances, count); }
static void STDMETHODCALLTYPE ctx_DSGetShader(Ctx *c, ID3D11DomainShader **out, ID3D11ClassInstance **instances, UINT *count) { get_shader(c->ds, (void **)out, NULL, NULL); get_instances(c, 4, instances, count); }
static void STDMETHODCALLTYPE ctx_CSGetShader(Ctx *c, ID3D11ComputeShader **out, ID3D11ClassInstance **instances, UINT *count) { get_shader(c->cs, (void **)out, NULL, NULL); get_instances(c, 5, instances, count); }

static void STDMETHODCALLTYPE ctx_IASetInputLayout(Ctx *c, ID3D11InputLayout *layout) { bind(&c->layout, layout); }
static void STDMETHODCALLTYPE ctx_IAGetInputLayout(Ctx *c, ID3D11InputLayout **out) { get_shader(c->layout, (void **)out, NULL, NULL); }
static void STDMETHODCALLTYPE ctx_IASetVertexBuffers(Ctx *c, UINT start, UINT count, ID3D11Buffer *const *buffers, const UINT *strides, const UINT *offsets) {
  for (UINT i = 0; i < count && start + i < VERTEX_SLOTS; ++i) {
    Buf *b = buffers ? (Buf *)buffers[i] : NULL;
    for (UINT k = 0; b && k < 4; ++k) if (c->so_buf[k] == b) b = NULL;      /* (a stream output target is no input) */
    bind(&c->vb[start + i], b);
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
  for (UINT k = 0; buffer && k < 4; ++k) if (c->so_buf[k] == (Buf *)buffer) buffer = NULL;
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

static void unbind_reads_of(Ctx *c, const void *resource, BOOL compute);
static const void *view_resource(const View *v);
static void STDMETHODCALLTYPE ctx_OMSetRenderTargets(Ctx *c, UINT count, ID3D11RenderTargetView *const *views, ID3D11DepthStencilView *depth) {
  UINT n = 0;
  while (views && n < count && n < 8 && views[n]) ++n;      /* (up to the first that is not there: the kernel's targets have no gaps) */
  for (UINT i = 0; i < 8; ++i) bind(&c->rtv[i], i < n ? views[i] : NULL);
  c->rtv_count = n;
  bind(&c->dsv, depth);
  /* (what is drawn into is no shader resource any more) */
  for (UINT i = 0; i < n; ++i) unbind_reads_of(c, view_resource(c->rtv[i]), FALSE);
  if (c->dsv) unbind_reads_of(c, view_resource(c->dsv), FALSE);
  c->dev->dirty |= DIRTY_TARGETS | DIRTY_VIEWPORT;
}
static void STDMETHODCALLTYPE ctx_OMGetRenderTargets(Ctx *c, UINT count, ID3D11RenderTargetView **views, ID3D11DepthStencilView **depth) {
  get_range((void **)c->rtv, 8, 0, count, (void **)views);
  get_shader(c->dsv, (void **)depth, NULL, NULL);
}
/* A deferred context's hidden counts of the append and counter views being bound: records of eight views each, the counts set when the list runs (deferred.inc REC_UAV_COUNTS). */
static void uav_counts_record(Ctx *c, ID3D11UnorderedAccessView *const *views, const UINT *counts, UINT count) {
  for (UINT first = 0; first < count; first += 8) {
    const UINT n = count - first < 8 ? count - first : 8;
    RecArgs a = {.op = REC_UAV_COUNTS, .u = {n}, .data = counts + first, .bytes = 4 * n};
    for (UINT i = 0; i < n; ++i) a.obj[i] = views[first + i];
    deferred_record(c, &a);
  }
}
/* Render targets and the pixel shader's unordered access views, which share the slots: u<n> for n from `uav_start` (at or after the last target) on, the other
 * unordered access slots left empty. D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL and D3D11_KEEP_UNORDERED_ACCESS_VIEWS keep what is bound. */
static void STDMETHODCALLTYPE ctx_OMSetRenderTargetsAndUnorderedAccessViews(Ctx *c, UINT rtv_count, ID3D11RenderTargetView *const *rtvs, ID3D11DepthStencilView *depth,
                                                                            UINT uav_start, UINT uav_count, ID3D11UnorderedAccessView *const *uavs, const UINT *counts) {
  if (rtv_count != D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL) {
    if (rtv_count > 8) return;
    ctx_OMSetRenderTargets(c, rtv_count, rtvs, depth);
  }
  if (uav_count == D3D11_KEEP_UNORDERED_ACCESS_VIEWS) return;
  /* (the slots of the device's feature level: u0 .. u63 from 11_1, u0 .. u7 below - dev_uav_slots) */
  const UINT slots = dev_uav_slots(c->dev);
  if (uav_start > slots || uav_count > slots - uav_start || (rtv_count != D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL && uav_count && uav_start < rtv_count)) return;
  if (c->deferred && counts && uavs) uav_counts_record(c, uavs, counts, uav_count);
  if (c->deferred) counts = NULL;
  for (UINT slot = 0; slot < UAV_SLOTS; ++slot) {
    View *v = slot >= uav_start && slot < uav_start + uav_count && uavs ? (View *)uavs[slot - uav_start] : NULL;
    if (v) {
      unbind_reads_of(c, view_resource(v), FALSE);
      const UINT k = slot - uav_start;
      if (counts && counts[k] != (UINT)-1 && v->counter_buf) v->counter = counts[k], v->counter_dirty = TRUE;
    }
    bind(&c->ps_uav[slot], v);
  }
}
static void STDMETHODCALLTYPE ctx_OMGetRenderTargetsAndUnorderedAccessViews(Ctx *c, UINT rtv_count, ID3D11RenderTargetView **rtvs, ID3D11DepthStencilView **depth,
                                                                            UINT uav_start, UINT uav_count, ID3D11UnorderedAccessView **uavs) {
  if (rtvs || depth) ctx_OMGetRenderTargets(c, rtvs ? rtv_count : 0, rtvs, depth);
  if (uavs) get_range((void **)c->ps_uav, UAV_SLOTS, uav_start, uav_count, (void **)uavs);
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
  if (count > D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE || (count && !viewports)) return;
  for (UINT i = 0; i < count; ++i) c->viewports[i] = viewports[i];
  c->viewport_count = count;
  c->dev->dirty |= DIRTY_VIEWPORT;
}
/* (no array: the number bound; else up to *count of them, and zeros after what is bound) */
static void STDMETHODCALLTYPE ctx_RSGetViewports(Ctx *c, UINT *count, D3D11_VIEWPORT *viewports) {
  if (!count) return;
  if (!viewports) { *count = c->viewport_count; return; }
  for (UINT i = 0; i < *count; ++i) {
    if (i < c->viewport_count) viewports[i] = c->viewports[i];
    else memset(&viewports[i], 0, sizeof viewports[i]);
  }
}
static void STDMETHODCALLTYPE ctx_RSSetScissorRects(Ctx *c, UINT count, const D3D11_RECT *rects) {
  if (count > D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE || (count && !rects)) return;
  for (UINT i = 0; i < count; ++i) c->scissors[i] = rects[i];
  c->scissor_count = count;
  c->dev->dirty |= DIRTY_VIEWPORT;
}
static void STDMETHODCALLTYPE ctx_RSGetScissorRects(Ctx *c, UINT *count, D3D11_RECT *rects) {
  if (!count) return;
  if (!rects) { *count = c->scissor_count; return; }
  for (UINT i = 0; i < *count; ++i) {
    if (i < c->scissor_count) rects[i] = c->scissors[i];
    else memset(&rects[i], 0, sizeof rects[i]);
  }
}

/* A draw: left out while predicated, measured for the active queries. */
static void draw_any(Ctx *c, BOOL indexed, UINT count, UINT instances, UINT start, INT base_vertex, UINT start_instance) {
  Dev *d = c->dev;
  LOCK(c);
  if (!count || !instances || predicated_out(d)) { UNLOCK(c); return; }
  measure_room(d);
  if (!draw_prepare(c, indexed)) { UNLOCK(c); return; }
  measure_draw(d);
  if (indexed) ID3D12GraphicsCommandList_DrawIndexedInstanced(d->list, count, instances, start, base_vertex, start_instance);
  else ID3D12GraphicsCommandList_DrawInstanced(d->list, count, instances, start, start_instance);
  UNLOCK(c);
}
static void STDMETHODCALLTYPE ctx_Draw(Ctx *c, UINT count, UINT start) {
  if (c->deferred) { deferred_record(c, &(RecArgs){.op = REC_DRAW, .u = {count, start}}); return; } draw_any(c, FALSE, count, 1, start, 0, 0); }
static void STDMETHODCALLTYPE ctx_DrawIndexed(Ctx *c, UINT count, UINT start, INT base_vertex) {
  if (c->deferred) { deferred_record(c, &(RecArgs){.op = REC_DRAW_INDEXED, .u = {count, start, (UINT)base_vertex}}); return; } draw_any(c, TRUE, count, 1, start, base_vertex, 0); }
static void STDMETHODCALLTYPE ctx_DrawInstanced(Ctx *c, UINT count, UINT instances, UINT start, UINT start_instance) {
  if (c->deferred) { deferred_record(c, &(RecArgs){.op = REC_DRAW_INSTANCED, .u = {count, instances, start, start_instance}}); return; } draw_any(c, FALSE, count, instances, start, 0, start_instance); }
static void STDMETHODCALLTYPE ctx_DrawIndexedInstanced(Ctx *c, UINT count, UINT instances, UINT start, INT base_vertex, UINT start_instance) {
  if (c->deferred) { deferred_record(c, &(RecArgs){.op = REC_DRAW_INDEXED_INSTANCED, .u = {count, instances, start, (UINT)base_vertex, start_instance}}); return; }
  draw_any(c, TRUE, count, instances, start, base_vertex, start_instance);
}
/* A draw whose arguments are in a buffer (made with D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS): the vertex count, instances, first vertex and first instance (and for an
 * indexed one the index count, instances, first index, base vertex and first instance), as the GPU finds them when the list runs. */
static void draw_indirect(Ctx *c, BOOL indexed, ID3D11Buffer *buffer, UINT offset) {
  Dev *d = c->dev;
  Buf *b = (Buf *)buffer;
  const UINT bytes = indexed ? 20 : 16;
  if (!b || !(b->desc.MiscFlags & D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS) || offset % 4 || offset > b->desc.ByteWidth || b->desc.ByteWidth - offset < bytes) return;
  LOCK(c);
  if (predicated_out(d)) { UNLOCK(c); return; }
  measure_room(d);
  if (!draw_prepare(c, indexed)) { UNLOCK(c); return; }
  measure_draw(d);
  ID3D12GraphicsCommandList_ExecuteIndirect(d->list, indexed ? d->sig_draw_indexed : d->sig_draw, 1, b->res, offset, NULL, 0);
  b->used = d->epoch;
  UNLOCK(c);
}
static void STDMETHODCALLTYPE ctx_DrawInstancedIndirect(Ctx *c, ID3D11Buffer *buffer, UINT offset) {
  if (c->deferred) { deferred_record(c, &(RecArgs){.op = REC_DRAW_INDIRECT, .obj = {buffer}, .u = {offset}}); return; } draw_indirect(c, FALSE, buffer, offset); }
static void STDMETHODCALLTYPE ctx_DrawIndexedInstancedIndirect(Ctx *c, ID3D11Buffer *buffer, UINT offset) {
  if (c->deferred) { deferred_record(c, &(RecArgs){.op = REC_DRAW_INDEXED_INDIRECT, .obj = {buffer}, .u = {offset}}); return; } draw_indirect(c, TRUE, buffer, offset); }
static void STDMETHODCALLTYPE ctx_DispatchIndirect(Ctx *c, ID3D11Buffer *buffer, UINT offset) {
  if (c->deferred) { deferred_record(c, &(RecArgs){.op = REC_DISPATCH_INDIRECT, .obj = {buffer}, .u = {offset}}); return; }
  Dev *d = c->dev;
  Buf *b = (Buf *)buffer;
  if (!b || !(b->desc.MiscFlags & D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS) || offset % 4 || offset > b->desc.ByteWidth || b->desc.ByteWidth - offset < 12) return;
  LOCK(c);
  if (!predicated_out(d)) measure_room(d);
  if (!predicated_out(d) && compute_prepare(c)) {
    measure_draw(d);
    ID3D12GraphicsCommandList_ExecuteIndirect(d->list, d->sig_dispatch, 1, b->res, offset, NULL, 0);
    b->used = d->epoch;
  }
  UNLOCK(c);
}

/* The vertices stream output wrote into the buffer of vertex slot 0, drawn: as many as its records hold after the slot's offset. */
static void STDMETHODCALLTYPE ctx_DrawAuto(Ctx *c) {
  if (c->deferred) { deferred_record(c, &(RecArgs){.op = REC_DRAW_AUTO}); return; }
  Dev *d = c->dev;
  LOCK(c);
  Buf *b = c->vb[0];
  UINT vertices = 0;
  if (b && c->vb_stride[0]) {
    so_settle(d, b);
    const UINT64 end = b->so_end < b->desc.ByteWidth ? b->so_end : b->desc.ByteWidth;
    vertices = end > c->vb_offset[0] ? (UINT)((end - c->vb_offset[0]) / c->vb_stride[0]) : 0;
  }
  UNLOCK(c);
  if (vertices) draw_any(c, FALSE, vertices, 1, 0, 0, 0);
}

/* ---- compute ------------------------------------------------------------------------------------------------------------------------------------------ */

/* The resource a view is of. */
static const void *view_resource(const View *v) { return v->tex ? (const void *)v->tex : (const void *)v->buf; }
/* Direct3D 11 keeps a resource from being read and written at once: binding it for writing takes it out of every shader-resource slot, and a shader-resource binding
 * of what is bound for writing is left empty. These are the views that write: the compute shader's unordered access views (`compute`), or the render targets,
 * the depth buffer and the pixel shader's unordered access views. */
static BOOL bound_for_writing(const Ctx *c, const void *resource, BOOL compute) {
  if (compute) {
    for (UINT i = 0; i < UAV_SLOTS; ++i) if (c->cs_uav[i] && view_resource(c->cs_uav[i]) == resource) return TRUE;
    return FALSE;
  }
  for (UINT i = 0; i < UAV_SLOTS; ++i) if (c->ps_uav[i] && view_resource(c->ps_uav[i]) == resource) return TRUE;
  for (UINT i = 0; i < c->rtv_count; ++i) if (c->rtv[i] && view_resource(c->rtv[i]) == resource) return TRUE;
  return c->dsv && view_resource(c->dsv) == resource;
}
static void unbind_reads_of(Ctx *c, const void *resource, BOOL compute) {
  if (compute) {
    for (UINT i = 0; i < SRV_SLOTS; ++i) if (c->cs_srv[i] && view_resource(c->cs_srv[i]) == resource) bind(&c->cs_srv[i], NULL);
    return;
  }
  for (UINT i = 0; i < SRV_SLOTS; ++i) {
    if (c->srv[i] && view_resource(c->srv[i]) == resource) bind(&c->srv[i], NULL);
    for (UINT k = 0; k < 4; ++k) if (c->stage_srv[k][i] && view_resource(c->stage_srv[k][i]) == resource) bind(&c->stage_srv[k][i], NULL);
  }
}
/* Shader resources into slots, each left empty when what it is of is bound for writing. */
static void set_resources(Ctx *c, View **slots, UINT start, UINT count, ID3D11ShaderResourceView *const *views, BOOL compute) {
  for (UINT i = 0; i < count && start + i < SRV_SLOTS; ++i) {
    View *v = views ? (View *)views[i] : NULL;
    if (v && bound_for_writing(c, view_resource(v), compute)) v = NULL;
    bind(&slots[start + i], v);
  }
}

static void STDMETHODCALLTYPE ctx_CSSetShader(Ctx *c, ID3D11ComputeShader *shader, ID3D11ClassInstance *const *instances, UINT count) {
  bind(&c->cs, shader);
  set_instances(c, 5, instances, count);
}
static void STDMETHODCALLTYPE ctx_CSSetConstantBuffers(Ctx *c, UINT start, UINT count, ID3D11Buffer *const *buffers) { cb_set(c, 5, start, count, buffers, NULL, NULL); }
static void STDMETHODCALLTYPE ctx_CSSetConstantBuffers1(Ctx *c, UINT start, UINT count, ID3D11Buffer *const *buffers, const UINT *first, const UINT *number) { cb_set(c, 5, start, count, buffers, first, number); }
static void STDMETHODCALLTYPE ctx_CSGetConstantBuffers(Ctx *c, UINT start, UINT count, ID3D11Buffer **buffers) { cb_get(c, 5, start, count, buffers, NULL, NULL); }
static void STDMETHODCALLTYPE ctx_CSGetConstantBuffers1(Ctx *c, UINT start, UINT count, ID3D11Buffer **buffers, UINT *first, UINT *number) { cb_get(c, 5, start, count, buffers, first, number); }
static void STDMETHODCALLTYPE ctx_CSSetShaderResources(Ctx *c, UINT start, UINT count, ID3D11ShaderResourceView *const *views) { set_resources(c, c->cs_srv, start, count, views, TRUE); }
static void STDMETHODCALLTYPE ctx_CSGetShaderResources(Ctx *c, UINT start, UINT count, ID3D11ShaderResourceView **views) { get_range((void **)c->cs_srv, SRV_SLOTS, start, count, (void **)views); }
static void STDMETHODCALLTYPE ctx_CSSetSamplers(Ctx *c, UINT start, UINT count, ID3D11SamplerState *const *samplers) { set_range((void **)c->cs_sampler, TEXTURE_SLOTS, start, count, (void *const *)samplers); }
static void STDMETHODCALLTYPE ctx_CSGetSamplers(Ctx *c, UINT start, UINT count, ID3D11SamplerState **samplers) { get_range((void **)c->cs_sampler, TEXTURE_SLOTS, start, count, (void **)samplers); }
/* Unordered access views of the compute shader; `counts`: the hidden count of each append or counter view (-1: kept as it is). */
static void STDMETHODCALLTYPE ctx_CSSetUnorderedAccessViews(Ctx *c, UINT start, UINT count, ID3D11UnorderedAccessView *const *views, const UINT *counts) {
  const UINT slots = dev_uav_slots(c->dev);
  if (start > slots || count > slots - start) return;      /* (past the device's slots: the runtime drops the call) */
  if (c->deferred && counts && views) uav_counts_record(c, views, counts, count);      /* (the counts are set when the list runs) */
  if (c->deferred) counts = NULL;
  for (UINT i = 0; i < count && start + i < UAV_SLOTS; ++i) {
    View *v = views ? (View *)views[i] : NULL;
    if (v) {
      unbind_reads_of(c, view_resource(v), TRUE);
      if (counts && counts[i] != (UINT)-1 && v->counter_buf) v->counter = counts[i], v->counter_dirty = TRUE;
    }
    bind(&c->cs_uav[start + i], v);
  }
}
static void STDMETHODCALLTYPE ctx_CSGetUnorderedAccessViews(Ctx *c, UINT start, UINT count, ID3D11UnorderedAccessView **views) { get_range((void **)c->cs_uav, UAV_SLOTS, start, count, (void **)views); }

/* Everything of a dispatch but the dispatch: FALSE when there is no compute shader or the kernel does not take it. */
static BOOL compute_prepare(Ctx *c) {
  Dev *d = c->dev;
  Shader *cs = c->cs;
  if (!cs) return FALSE;
  const UINT view_count = cs->wide ? SRV_SLOTS : TEXTURE_SLOTS;
  for (UINT i = 0; i < view_count; ++i) if (c->cs_srv[i] && c->cs_srv[i]->tex) tex_sync(d, c->cs_srv[i]->tex);
  for (UINT i = 0; i < view_count; ++i) if (c->cs_srv[i] && c->cs_srv[i]->tex) tex_before_read(d, c->cs_srv[i]->tex);
  uav_textures_before(d, c->cs_uav, cs->uav_mask);
  if (d->ring_used + CONSTANT_BLOCK > RING_BYTES || d->view_used + SRV_SLOTS + UAV_SLOTS > VIEW_HEAP || d->sampler_used + TEXTURE_SLOTS > VIEW_HEAP) flush(d);
  /* (a shader with interfaces: the variant for the instances bound keeps the pipeline) */
  const BYTE *cs_code;
  SIZE_T cs_bytes;
  UINT cs_id;
  Variant *variant;
  if (!shader_code(d, c, 5, cs, &cs_code, &cs_bytes, &cs_id, &variant)) return FALSE;
  ID3D12PipelineState **pipeline = variant ? &variant->compute : &cs->compute;
  if (!*pipeline) {
    D3D12_COMPUTE_PIPELINE_STATE_DESC pd;
    memset(&pd, 0, sizeof pd);
    pd.pRootSignature = root_for(d, view_count, uav_table_count(cs->uav_mask));
    pd.CS.pShaderBytecode = cs_code;
    pd.CS.BytecodeLength = cs_bytes;
    if (FAILED(ID3D12Device_CreateComputePipelineState(d->d12, &pd, &IID_ID3D12PipelineState, (void **)pipeline)) || !*pipeline) {
      *pipeline = NULL;
      static volatile LONG said;
      if (!InterlockedExchange(&said, 1)) dx_say("dx: refused: a compute shader the kernel does not take (see docs/Direct3D11.md)");
      return FALSE;
    }
  }
  ID3D12GraphicsCommandList *list = d->list;
  ID3D12DescriptorHeap *heaps[2] = {d->view_heap, d->sampler_heap};
  ID3D12GraphicsCommandList_SetComputeRootSignature(list, root_for(d, view_count, uav_table_count(cs->uav_mask)));
  ID3D12GraphicsCommandList_SetDescriptorHeaps(list, 2, heaps);
  ID3D12GraphicsCommandList_SetPipelineState(list, *pipeline);
  const Shader *const stages[1] = {cs};
  static const UINT stage_of[1] = {5};
  constants_record(d, c, stages, stage_of, 1);
  ID3D12GraphicsCommandList_SetComputeRootConstantBufferView(list, 0, ID3D12Resource_GetGPUVirtualAddress(d->ring) + d->ring_last);
  if (!tables_record(d, c->cs_srv, c->cs_sampler, cs->tex_kind, c->cs_uav, cs->uav_mask, FALSE, view_count)) return FALSE;
  D3D12_GPU_DESCRIPTOR_HANDLE views = {d->table_views}, samplers = {d->table_samplers}, uavs = {d->table_uavs};
  ID3D12GraphicsCommandList_SetComputeRootDescriptorTable(list, 1, views);
  ID3D12GraphicsCommandList_SetComputeRootDescriptorTable(list, 2, samplers);
  ID3D12GraphicsCommandList_SetComputeRootDescriptorTable(list, 3, uavs);
  tables_mark_used(d, c->cs_srv, c->cs_uav, cs->uav_mask);
  cs->used = d->epoch;
  if (variant) variant->used = d->epoch;
  d->dirty |= DIRTY_ROOT;      /* (the next draw sets the graphics root signature and the heaps again) */
  d->recorded = 1;
  return TRUE;
}
static void STDMETHODCALLTYPE ctx_Dispatch(Ctx *c, UINT x, UINT y, UINT z) {
  if (c->deferred) { deferred_record(c, &(RecArgs){.op = REC_DISPATCH, .u = {x, y, z}}); return; }
  LOCK(c);
  if (!predicated_out(c->dev)) measure_room(c->dev);
  if (!predicated_out(c->dev) && x && y && z && x <= D3D11_CS_DISPATCH_MAX_THREAD_GROUPS_PER_DIMENSION && y <= D3D11_CS_DISPATCH_MAX_THREAD_GROUPS_PER_DIMENSION &&
      z <= D3D11_CS_DISPATCH_MAX_THREAD_GROUPS_PER_DIMENSION && compute_prepare(c)) {
    measure_draw(c->dev);
    ID3D12GraphicsCommandList_Dispatch(c->dev->list, x, y, z);
  }
  UNLOCK(c);
}

static void STDMETHODCALLTYPE ctx_ClearRenderTargetView(Ctx *c, ID3D11RenderTargetView *view, const FLOAT colour[4]) {
  if (c->deferred) { deferred_record(c, &(RecArgs){.op = REC_CLEAR_RTV, .obj = {view}, .f = {colour ? colour[0] : 0, colour ? colour[1] : 0, colour ? colour[2] : 0, colour ? colour[3] : 0}}); return; }
  View *v = (View *)view;
  if (!v || !colour) return;
  if (predicated_out(c->dev)) return;
  LOCK(c);
  FLOAT kept[4] = {colour[0], colour[1], colour[2], colour[3]};      /* (a target kept wider than its format: the channels it does not have stay 0 and 1) */
  const UINT8 mask = v->plane_format ? (v->plane ? 3 : 1) : tex_format_mask(v->tex->fmt);
  for (UINT k = 0; k < 4; ++k) if (!(mask >> k & 1)) kept[k] = k == 3 ? 1.0f : 0.0f;
  if (v->scratch) rtv_load(c->dev, v, TRUE);      /* (all of it: nothing to load) */
  else tex_before_write(c->dev, v->tex);
  ID3D12GraphicsCommandList_ClearRenderTargetView(c->dev->list, v->handle, kept, 0, NULL);
  v->tex->used = v->tex->written = c->dev->epoch;
  c->dev->recorded = 1;
  UNLOCK(c);
}
static void STDMETHODCALLTYPE ctx_ClearDepthStencilView(Ctx *c, ID3D11DepthStencilView *view, UINT flags, FLOAT depth, UINT8 stencil) {
  if (c->deferred) { deferred_record(c, &(RecArgs){.op = REC_CLEAR_DSV, .obj = {view}, .u = {flags, stencil}, .f = {depth}}); return; }
  View *v = (View *)view;
  Dev *d = c->dev;
  if (!v || !(flags & 3)) return;
  if (predicated_out(c->dev)) return;
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
  tex_before_write(d, v->tex);
  ID3D12GraphicsCommandList_ClearDepthStencilView(d->list, v->handle, (D3D12_CLEAR_FLAGS)(flags & 3), depth, stencil, 0, NULL);
  v->tex->used = v->tex->written = d->epoch;
  d->recorded = 1;
  UNLOCK(c);
}

/* ---- the CPU's side of resources ----------------------------------------------------------------------------------------------------------------- */

static Buf *buf_of_resource(ID3D11Resource *resource) {
  if (!resource || ((DxObj *)resource)->lpVtbl != (const void *)&buf_vtbl) return NULL;
  return (Buf *)resource;
}

/* Gives a buffer that a recorded draw reads (`res`) other memory, so the program can write it at once (Map, UpdateSubresource, a copy into it): the old memory
 * is retired until the list has run. `keep`: the new memory starts as a copy of the old (not for WRITE_DISCARD). FALSE without memory: the caller sends the
 * list instead. (A converted copy a draw reads is `converted_used`'s business: it is replaced when it is made again.) */
static BOOL buf_rename(Dev *d, Buf *b, BOOL keep) {
  BYTE *data = NULL;
  UINT64 bytes = 0;
  if (!retire_room(d, b->bytes)) return FALSE;
  ID3D12Resource *fresh = take_upload(d, b->desc.ByteWidth, &data, &bytes);
  if (!fresh) return FALSE;
  if (keep) memcpy(data, b->data, b->desc.ByteWidth);
  ID3D12Resource_Unmap(fresh, 0, NULL);      /* (see Buf.data: the memory stays, and the caller writes it next) */
  retire(d, b->res, b->mapped ? b->data : NULL, b->bytes);      /* (a mapping of the old memory goes with it) */
  b->mapped = 0;
  b->res = fresh;
  b->data = data;
  b->bytes = bytes;
  b->used = d->epoch - 1u;      /* (nothing recorded reads the new memory) */
  return TRUE;
}

/* The runtime wrote the buffer's memory itself (UpdateSubresource, a copy): d3d12.dll sends it with the next list (a Map and Unmap mark it changed). */
static void buf_touched(Buf *b) {
  void *p = NULL;
  if (b->mapped) return;      /* (mapped: d3d12.dll compares it at each submission anyway) */
  if (SUCCEEDED(ID3D12Resource_Map(b->res, 0, NULL, &p))) ID3D12Resource_Unmap(b->res, 0, NULL);
}

/* Bytes of one buffer into another, in their place among the commands. Between buffers of the CPU's memory the copy is done here (a recorded draw keeps the old
 * bytes in the old memory); into one of the GPU's memory it is a copy the list makes; out of one into the CPU's memory the list is sent and the bytes read back. */
static void buf_copy(Dev *d, Buf *to, UINT64 to_offset, Buf *from, UINT64 from_offset, UINT64 bytes) {
  if (!bytes) return;
  if (to->gpu) {
    if (from->gpu && to == from && to_offset < from_offset + bytes && from_offset < to_offset + bytes) {
      /* (an overlapping copy within one buffer: by way of memory) */
      BYTE *t = mem_alloc(bytes);
      if (t && SUCCEEDED(buf_gpu_read(d, from, from_offset, bytes, t))) buf_gpu_write(d, to, to_offset, t, bytes);
      mem_free(t);
      return;
    }
    ID3D12GraphicsCommandList_CopyBufferRegion(d->list, to->res, to_offset, from->res, from_offset, bytes);
    d->recorded = 1;
    to->used = to->written = from->used = d->epoch;
    ++to->version;
    return;
  }
  if (to->used == d->epoch && (to == from || !buf_rename(d, to, to_offset != 0 || bytes != to->desc.ByteWidth))) settle(d, to->used);
  if (from->gpu) {
    if (FAILED(buf_gpu_read(d, from, from_offset, bytes, to->data + to_offset))) return;
  } else memmove(to->data + to_offset, from->data + from_offset, bytes);
  buf_touched(to);
  ++to->version;
}
/* Writes bytes into a buffer wherever its memory is (CopyStructureCount, the clears of unordered access views). */
static void buf_write_any(Dev *d, Buf *b, UINT64 offset, const void *data, UINT64 bytes) {
  if (b->gpu) { buf_gpu_write(d, b, offset, data, bytes); return; }
  if (b->used == d->epoch && !buf_rename(d, b, TRUE)) settle(d, b->used);
  memcpy(b->data + offset, data, bytes);
  buf_touched(b);
  ++b->version;
}

static HRESULT STDMETHODCALLTYPE ctx_Map(Ctx *c, ID3D11Resource *resource, UINT sub, D3D11_MAP type, UINT flags, D3D11_MAPPED_SUBRESOURCE *out) {
  if (c->deferred) { if (out) memset(out, 0, sizeof *out); return out && !flags ? deferred_map(c, resource, sub, type, out) : E_INVALIDARG; }
  (void)flags;
  Dev *d = c->dev;
  if (out) memset(out, 0, sizeof *out);
  Buf *b = buf_of_resource(resource);
  Tex *t = tex_of_resource(resource);
  /* (a default texture the CPU reaches: layout.inc) */
  if (t && t->desc.Usage == D3D11_USAGE_DEFAULT && t->desc.CPUAccessFlags) {
    LOCK(c);
    const HRESULT hr = tex_map_default(d, t, sub, type, out);
    UNLOCK(c);
    return hr;
  }
  if (!out || type < D3D11_MAP_READ || type > D3D11_MAP_WRITE_NO_OVERWRITE) return E_INVALIDARG;
  HRESULT hr = S_OK;
  LOCK(c);
  if (b) {
    if (sub != 0 || b->gpu || b->desc.Usage == D3D11_USAGE_DEFAULT || b->desc.Usage == D3D11_USAGE_IMMUTABLE) hr = E_INVALIDARG;      /* (the CPU maps dynamic and staging buffers only) */
    else {
      /* (a recorded draw reads what is there now: the buffer gets other memory, or failing that the list is sent first) */
      if (type != D3D11_MAP_WRITE_NO_OVERWRITE && type != D3D11_MAP_READ && b->used == d->epoch && !buf_rename(d, b, type != D3D11_MAP_WRITE_DISCARD)) settle(d, b->used);
      void *p = NULL;
      if (type != D3D11_MAP_READ && !b->mapped && SUCCEEDED(ID3D12Resource_Map(b->res, 0, NULL, &p))) b->mapped = 1;      /* (see Buf.data) */
      out->pData = b->data;
      out->RowPitch = out->DepthPitch = b->desc.ByteWidth;
    }
  } else if (t) {
    if (sub >= tex_subresources(t) || (t->depth && t->desc.Usage != D3D11_USAGE_STAGING) || t->mapped[sub]) hr = E_INVALIDARG;      /* (a depth texture the CPU maps is a staging one) */
    else {
      const UINT level = sub % t->desc.MipLevels, w = mip_size(t->desc.Width, level), h = mip_size(t->desc.Height, level), slices = tex_slices(t, sub);
      const SIZE_T slice_bytes = (SIZE_T)tex_pitch(t, w) * tex_rows(t, h);
      if (!t->shadow[sub]) t->shadow[sub] = mem_alloc(slice_bytes * slices);
      if (!t->shadow[sub]) hr = E_OUTOFMEMORY;
      else if (type == D3D11_MAP_READ || type == D3D11_MAP_READ_WRITE)
        for (UINT z = 0; SUCCEEDED(hr) && z < slices; ++z) hr = tex_read(d, t, sub, z, t->shadow[sub] + z * slice_bytes);
      if (SUCCEEDED(hr)) {
        t->mapped[sub] = type;
        out->pData = t->shadow[sub];
        out->RowPitch = tex_pitch(t, w);
        out->DepthPitch = (UINT)slice_bytes;
      }
    }
  } else hr = E_INVALIDARG;
  UNLOCK(c);
  return hr;
}
static void STDMETHODCALLTYPE ctx_Unmap(Ctx *c, ID3D11Resource *resource, UINT sub) {
  if (c->deferred) { deferred_unmap(c, resource, sub); return; }
  Buf *b = buf_of_resource(resource);
  Tex *t = tex_of_resource(resource);
  if (t && t->desc.Usage == D3D11_USAGE_DEFAULT && t->desc.CPUAccessFlags) {
    LOCK(c);
    tex_unmap_default(c->dev, t, sub);
    UNLOCK(c);
    return;
  }
  LOCK(c);
  if (b) {
    ++b->version;
    if (b->mapped) { ID3D12Resource_Unmap(b->res, 0, NULL); b->mapped = 0; }      /* (d3d12.dll sends it with the next list) */
  } else if (t && sub < tex_subresources(t) && t->mapped[sub]) {
    const D3D11_MAP type = t->mapped[sub];
    t->mapped[sub] = 0;
    if (type != D3D11_MAP_READ) {
      const UINT level = sub % t->desc.MipLevels, w = mip_size(t->desc.Width, level), h = mip_size(t->desc.Height, level);
      for (UINT z = 0; z < tex_slices(t, sub); ++z) tex_write(c->dev, t, sub, 0, 0, z, w, h, t->shadow[sub] + (SIZE_T)z * tex_pitch(t, w) * tex_rows(t, h), tex_pitch(t, w));
    }
  }
  UNLOCK(c);
}

static void STDMETHODCALLTYPE ctx_UpdateSubresource(Ctx *c, ID3D11Resource *resource, UINT sub, const D3D11_BOX *box, const void *data, UINT row_pitch, UINT depth_pitch) {
  if (c->deferred) { deferred_update(c, resource, sub, box, data, row_pitch, depth_pitch); return; }
  Buf *b = buf_of_resource(resource);
  Tex *t = tex_of_resource(resource);
  if (!data) return;
  if (predicated_out(c->dev)) return;
  LOCK(c);
  if (b && sub == 0) {
    const UINT left = box ? box->left : 0, right = box ? box->right : b->desc.ByteWidth;
    /* (a part of a constant buffer - ConstantBufferPartialUpdate - begins and ends on a 16-byte constant, else the runtime drops the call) */
    const BOOL constant_part_ok = !(b->desc.BindFlags & D3D11_BIND_CONSTANT_BUFFER) || !box || (left % 16 == 0 && (right % 16 == 0 || right == b->desc.ByteWidth));
    if (left < right && right <= b->desc.ByteWidth && constant_part_ok) {
      if (b->gpu) buf_gpu_write(c->dev, b, left, data, right - left);
      else {
        if (b->used == c->dev->epoch && !buf_rename(c->dev, b, left != 0 || right != b->desc.ByteWidth)) settle(c->dev, b->used);
        memcpy(b->data + left, data, right - left);
        buf_touched(b);
        ++b->version;
      }
    }
  } else if (t && sub < tex_subresources(t) && !t->depth) {
    const UINT level = sub % t->desc.MipLevels, w = mip_size(t->desc.Width, level), h = mip_size(t->desc.Height, level), slices = tex_slices(t, sub);
    const UINT x = box ? box->left : 0, y = box ? box->top : 0, x1 = box ? box->right : w, y1 = box ? box->bottom : h;
    const UINT z0 = box && t->kind == 1 ? box->front : 0, z1 = box && t->kind == 1 ? box->back : slices;
    const UINT pitch = row_pitch ? row_pitch : tex_pitch(t, x1 - x);
    const UINT slice_pitch = depth_pitch ? depth_pitch : pitch * tex_rows(t, y1 - y);
    /* (a planar texture: the box is on whole 2 x 2 blocks of luma, else Direct3D 11 writes nothing; the chroma's rows follow the luma's in `data`) */
    if (x < x1 && y < y1 && x1 <= w && y1 <= h && z0 < z1 && z1 <= slices && !(t->planar && ((x | y | x1 | y1) & 1)))
      for (UINT z = z0; z < z1; ++z) tex_write(c->dev, t, sub, x, y, z, x1 - x, y1 - y, (const BYTE *)data + (SIZE_T)(z - z0) * slice_pitch, pitch);
  }
  UNLOCK(c);
}

/* A rectangle of one subresource into another, by way of memory (what Direct3D 12 here does not copy itself). */
static void tex_copy_slow(Dev *d, Tex *to, UINT to_sub, UINT x, UINT y, Tex *from, UINT from_sub, const D3D11_BOX *box) {
  /* (a volume: each slice of the box - all of the level's without one - into the slice of the other volume at the same place; a volume and another kind are not copied, nor
   * compressed textures) */
  /* (block-compressed ones a block at a time - of the same block size, the box on whole blocks but at the level's edges, the place on a block -; a whole slice is read before
   * any of it is written, so a copy within one subresource whose regions overlap - D3D11_FEATURE_DATA_D3D11_OPTIONS's CopyWithOverlap - copies what was there) */
  if (to->bpp != from->bpp || (to->kind == 1) != (from->kind == 1) || to->block != from->block) return;      /* (a depth texture's texels are the program's: tex_depth_rows) */
  const UINT level = from_sub % from->desc.MipLevels, w = mip_size(from->desc.Width, level), h = mip_size(from->desc.Height, level);
  const UINT to_level = to_sub % to->desc.MipLevels, to_w = mip_size(to->desc.Width, to_level), to_h = mip_size(to->desc.Height, to_level);
  const UINT x0 = box ? box->left : 0, y0 = box ? box->top : 0, x1 = box ? box->right : w, y1 = box ? box->bottom : h;
  const UINT depth = tex_slices(from, from_sub), to_depth = tex_slices(to, to_sub), z0 = box ? box->front : 0, z1 = box ? box->back : depth;
  if (x0 >= x1 || y0 >= y1 || x1 > w || y1 > h || x > to_w || y > to_h || x1 - x0 > to_w - x || y1 - y0 > to_h - y) return;
  if (z0 >= z1 || z1 > depth || z1 - z0 > to_depth) return;
  if (from->block && (((x0 | y0 | x | y) & 3) || ((x1 & 3) && x1 != w) || ((y1 & 3) && y1 != h))) return;
  const SIZE_T pitch = tex_pitch(from, w), offset = from->block ? (SIZE_T)(y0 / 4) * pitch + (SIZE_T)(x0 / 4) * from->block : (SIZE_T)y0 * pitch + (SIZE_T)x0 * from->bpp;
  BYTE *pixels = mem_alloc(pitch * tex_rows(from, h));
  for (UINT z = z0; pixels && z < z1; ++z)
    if (SUCCEEDED(tex_read(d, from, from_sub, z, pixels))) tex_write(d, to, to_sub, x, y, z - z0, x1 - x0, y1 - y0, pixels + offset, (UINT)pitch);
  mem_free(pixels);
}
static BOOL tex_simple(const Tex *t) { return t->desc.MipLevels == 1 && t->desc.ArraySize == 1 && t->kind == 0 && !t->depth; }
/* Whether Direct3D 12 below copies between the two itself: plain 2D textures of one level and format, or - on a device that keeps every level (dev_all_levels) - textures of the same kind
 * of dimension whose texels (or blocks) are as many bytes, not depth buffers or shadowed targets (whose views hold what they drew: tex_sync first). */
static BOOL tex_copies12(Dev *d, const Tex *to, const Tex *from) {
  /* (two depth textures kept in one format below: Direct3D 12 copies their planes; a depth texture and a colour one go through the program's texels, tex_copy_slow) */
  if (to->depth || from->depth)
    return to->depth && from->depth && tex_depth_stencil_below(to) == tex_depth_stencil_below(from) && to->desc.SampleDesc.Count == from->desc.SampleDesc.Count &&
           (to->kind == 1) == (from->kind == 1);
  if (tex_simple(to) && tex_simple(from) && to->stored == from->stored) return TRUE;
  return dev_all_levels(d) && !to->depth && !from->depth && !to->shadowed && !from->shadowed && to->stored_bpp == from->stored_bpp && to->block == from->block &&
         (to->kind == 1) == (from->kind == 1) && !to->planar && !from->planar;
}

static void STDMETHODCALLTYPE ctx_CopySubresourceRegion(Ctx *c, ID3D11Resource *to, UINT to_sub, UINT x, UINT y, UINT z, ID3D11Resource *from, UINT from_sub, const D3D11_BOX *box) {
  if (c->deferred) { deferred_record(c, &(RecArgs){.op = REC_COPY_REGION, .obj = {to, from}, .u = {to_sub, x, y, z, from_sub}, .box = box}); return; }
  Dev *d = c->dev;
  Buf *bt = buf_of_resource(to), *bf = buf_of_resource(from);
  Tex *tt = tex_of_resource(to), *tf = tex_of_resource(from);
  if (predicated_out(c->dev)) return;
  LOCK(c);
  if (bt && bf) {
    const UINT left = box ? box->left : 0, right = box ? box->right : bf->desc.ByteWidth;
    if (left < right && right <= bf->desc.ByteWidth && x <= bt->desc.ByteWidth && right - left <= bt->desc.ByteWidth - x) buf_copy(d, bt, x, bf, left, right - left);
  } else if (tt && tf && (tt->planar || tf->planar)) {
    /* (planar textures: a slice into a slice of one of the same format, both planes) */
    if (tt->planar && tf->planar && to_sub < tex_subresources(tt) && from_sub < tex_subresources(tf)) tex_planar_copy(d, tt, to_sub, x, y, tf, from_sub, box);
  } else if (tt && tf && to_sub < tex_subresources(tt) && from_sub < tex_subresources(tf)) {
    /* (a depth or multisampled texture is copied whole: no box but one of the whole subresource, to (0, 0, 0), into a subresource of the same size - else nothing is copied, as the
     * debug layer's error leaves it) */
    if (tt->depth || tf->depth || tt->desc.SampleDesc.Count > 1 || tf->desc.SampleDesc.Count > 1) {
      const UINT fl = from_sub % tf->desc.MipLevels, tl = to_sub % tt->desc.MipLevels, fw = mip_size(tf->desc.Width, fl), fh = mip_size(tf->desc.Height, fl);
      if (x || y || z || fw != mip_size(tt->desc.Width, tl) || fh != mip_size(tt->desc.Height, tl) ||
          (box && (box->left || box->top || box->front || box->right != fw || box->bottom != fh || box->back != 1))) {
        UNLOCK(c);
        return;
      }
    }
    tex_sync(d, tf);
    tex_sync(d, tt);
    ++tt->version;
    UINT to12 = 0, from12 = 0;
    if (tex_copies12(d, tt, tf) && (tt != tf || to_sub != from_sub) && tex_d12_sub(tt, to_sub, &to12) && tex_d12_sub(tf, from_sub, &from12)) {
      tex_before_write(d, tt);
      tex_before_read(d, tf);
      D3D12_TEXTURE_COPY_LOCATION dst, src;
      memset(&dst, 0, sizeof dst);
      memset(&src, 0, sizeof src);
      dst.pResource = tt->res;
      dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
      dst.SubresourceIndex = to12;
      src.pResource = tf->res;
      src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
      src.SubresourceIndex = from12;
      /* (a box of a level of a layer; of a volume's level its slices from front, into the slice z on) */
      D3D12_BOX region;
      if (box) { region.left = box->left; region.top = box->top; region.front = tf->kind == 1 ? box->front : 0; region.right = box->right; region.bottom = box->bottom; region.back = tf->kind == 1 ? box->back : 1; }
      ID3D12GraphicsCommandList_CopyTextureRegion(d->list, &dst, x, y, tt->kind == 1 ? z : 0, &src, box ? &region : NULL);
      if (tt->depth && tex_depth_stencil_below(tt)) {
        /* (and the stencil: Direct3D 12 copies a plane at a time, the stencil's subresources after the depth's) */
        dst.SubresourceIndex += tt->kmips * (tt->kind == 1 ? 1 : tt->desc.ArraySize);
        src.SubresourceIndex += tf->kmips * (tf->kind == 1 ? 1 : tf->desc.ArraySize);
        ID3D12GraphicsCommandList_CopyTextureRegion(d->list, &dst, 0, 0, 0, &src, box ? &region : NULL);
      }
      tt->used = tf->used = tt->written = d->epoch;
      d->recorded = 1;
    } else tex_copy_slow(d, tt, to_sub, x, y, tf, from_sub, box);
  }
  UNLOCK(c);
}
static void STDMETHODCALLTYPE ctx_CopyResource(Ctx *c, ID3D11Resource *to, ID3D11Resource *from) {
  if (c->deferred) { deferred_record(c, &(RecArgs){.op = REC_COPY_RESOURCE, .obj = {to, from}}); return; }
  Dev *d = c->dev;
  Buf *bt = buf_of_resource(to), *bf = buf_of_resource(from);
  Tex *tt = tex_of_resource(to), *tf = tex_of_resource(from);
  if (predicated_out(c->dev)) return;
  LOCK(c);
  if (bt && bf && bt != bf) {
    if (bt->desc.ByteWidth == bf->desc.ByteWidth) buf_copy(d, bt, 0, bf, 0, bt->desc.ByteWidth);      /* (CopyResource copies resources of one size) */
  } else if (tt && tf && (tt->planar || tf->planar)) {
    /* (planar textures: of one format, size and number of slices; Direct3D 12 below copies every plane of every slice) */
    if (tt != tf && tt->planar && tf->planar && tt->desc.Format == tf->desc.Format && tt->desc.ArraySize == tf->desc.ArraySize && tt->desc.Width == tf->desc.Width &&
        tt->desc.Height == tf->desc.Height) {
      tex_before_write(d, tt);
      tex_before_read(d, tf);
      ID3D12GraphicsCommandList_CopyResource(d->list, tt->res, tf->res);
      ++tt->version;
      tt->used = tf->used = tt->written = d->epoch;
      d->recorded = 1;
    }
  } else if (tt && tf && tt != tf && tex_subresources(tt) == tex_subresources(tf) && tt->desc.Width == tf->desc.Width && tt->desc.Height == tf->desc.Height) {
    tex_sync(d, tf);
    tex_sync(d, tt);
    ++tt->version;
    if (tex_copies12(d, tt, tf) && tt->kind == tf->kind && tt->kmips == tf->kmips && tt->desc.ArraySize == tf->desc.ArraySize && tt->slices == tf->slices) {
      /* (every level of every layer: Direct3D 12 below copies them all) */
      tex_before_write(d, tt);
      tex_before_read(d, tf);
      ID3D12GraphicsCommandList_CopyResource(d->list, tt->res, tf->res);
      tt->used = tf->used = tt->written = d->epoch;
      d->recorded = 1;
    } else for (UINT sub = 0; sub < tex_subresources(tt); ++sub) tex_copy_slow(d, tt, sub, 0, 0, tf, sub, NULL);
  }
  UNLOCK(c);
}
/* ---- clearing unordered access views ------------------------------------------------------------------------------------------------------------------ */

static UINT16 half_of(FLOAT f) {
  UINT32 x;
  memcpy(&x, &f, 4);
  const UINT32 sign = (x >> 16) & 0x8000u, exponent = (x >> 23) & 0xFFu, mantissa = x & 0x7FFFFFu;
  if (exponent == 0xFF) return (UINT16)(sign | 0x7C00u | (mantissa ? 0x200u : 0));      /* (infinity, NaN) */
  const INT e = (INT)exponent - 127 + 15;
  if (e >= 31) return (UINT16)(sign | 0x7C00u);
  if (e <= 0) {
    if (e < -10) return (UINT16)sign;
    const UINT32 m = (mantissa | 0x800000u) >> (1 - e);
    return (UINT16)(sign | ((m + 0x1000u) >> 13));
  }
  UINT32 h = sign | ((UINT32)e << 10) | (mantissa >> 13);
  if (mantissa & 0x1000u) ++h;      /* (rounded to nearest) */
  return (UINT16)h;
}
static UINT32 unorm_of(FLOAT f, UINT bits) {
  const FLOAT top = (FLOAT)((1u << bits) - 1);
  if (!(f > 0.0f)) return 0;
  if (f >= 1.0f) return (UINT32)top;
  return (UINT32)(f * top + 0.5f);
}
static UINT32 snorm_of(FLOAT f, UINT bits) {
  const FLOAT top = (FLOAT)((1u << (bits - 1)) - 1);
  if (!(f > -1.0f)) f = -1.0f;
  if (f > 1.0f) f = 1.0f;
  const INT v = (INT)(f * top + (f >= 0 ? 0.5f : -0.5f));
  return (UINT32)v & ((1u << bits) - 1);
}
/* One element of `format` with the four values of a clear (`floats`: ClearUnorderedAccessViewFloat, converted to the format; else ClearUnorderedAccessViewUint, the
 * low bits of each value). Returns its bytes; 0 for a format that cannot be had. */
static UINT clear_element(DXGI_FORMAT format, const UINT values[4], const FLOAT fvalues[4], BOOL floats, BYTE *out) {
  UINT channels = 0, bits = 0;
  int kind = 0;      /* 0 uint/sint/typeless, 1 float, 2 unorm, 3 snorm */
  BOOL bgra = FALSE;
  switch (format) {
    case DXGI_FORMAT_R32G32B32A32_FLOAT: kind = 1; /* fallthrough */ case DXGI_FORMAT_R32G32B32A32_UINT: case DXGI_FORMAT_R32G32B32A32_SINT: case DXGI_FORMAT_R32G32B32A32_TYPELESS: channels = 4; bits = 32; break;
    case DXGI_FORMAT_R32G32B32_FLOAT: kind = 1; /* fallthrough */ case DXGI_FORMAT_R32G32B32_UINT: case DXGI_FORMAT_R32G32B32_SINT: case DXGI_FORMAT_R32G32B32_TYPELESS: channels = 3; bits = 32; break;
    case DXGI_FORMAT_R32G32_FLOAT: kind = 1; /* fallthrough */ case DXGI_FORMAT_R32G32_UINT: case DXGI_FORMAT_R32G32_SINT: case DXGI_FORMAT_R32G32_TYPELESS: channels = 2; bits = 32; break;
    case DXGI_FORMAT_R32_FLOAT: kind = 1; /* fallthrough */ case DXGI_FORMAT_R32_UINT: case DXGI_FORMAT_R32_SINT: case DXGI_FORMAT_R32_TYPELESS: case DXGI_FORMAT_UNKNOWN: channels = 1; bits = 32; break;
    case DXGI_FORMAT_R16G16B16A16_FLOAT: kind = 1; channels = 4; bits = 16; break;
    case DXGI_FORMAT_R16G16B16A16_UNORM: kind = 2; channels = 4; bits = 16; break;
    case DXGI_FORMAT_R16G16B16A16_SNORM: kind = 3; channels = 4; bits = 16; break;
    case DXGI_FORMAT_R16G16B16A16_UINT: case DXGI_FORMAT_R16G16B16A16_SINT: case DXGI_FORMAT_R16G16B16A16_TYPELESS: channels = 4; bits = 16; break;
    case DXGI_FORMAT_R16G16_FLOAT: kind = 1; channels = 2; bits = 16; break;
    case DXGI_FORMAT_R16G16_UNORM: kind = 2; channels = 2; bits = 16; break;
    case DXGI_FORMAT_R16G16_SNORM: kind = 3; channels = 2; bits = 16; break;
    case DXGI_FORMAT_R16G16_UINT: case DXGI_FORMAT_R16G16_SINT: case DXGI_FORMAT_R16G16_TYPELESS: channels = 2; bits = 16; break;
    case DXGI_FORMAT_R16_FLOAT: kind = 1; channels = 1; bits = 16; break;
    case DXGI_FORMAT_R16_UNORM: kind = 2; channels = 1; bits = 16; break;
    case DXGI_FORMAT_R16_SNORM: kind = 3; channels = 1; bits = 16; break;
    case DXGI_FORMAT_R16_UINT: case DXGI_FORMAT_R16_SINT: case DXGI_FORMAT_R16_TYPELESS: channels = 1; bits = 16; break;
    case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: case DXGI_FORMAT_B8G8R8X8_UNORM: bgra = TRUE; /* fallthrough */
    case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: kind = 2; channels = 4; bits = 8; break;
    case DXGI_FORMAT_R8G8B8A8_SNORM: kind = 3; channels = 4; bits = 8; break;
    case DXGI_FORMAT_R8G8B8A8_UINT: case DXGI_FORMAT_R8G8B8A8_SINT: case DXGI_FORMAT_R8G8B8A8_TYPELESS: case DXGI_FORMAT_B8G8R8A8_TYPELESS: channels = 4; bits = 8; break;
    case DXGI_FORMAT_R8G8_UNORM: kind = 2; channels = 2; bits = 8; break;
    case DXGI_FORMAT_R8G8_SNORM: kind = 3; channels = 2; bits = 8; break;
    case DXGI_FORMAT_R8G8_UINT: case DXGI_FORMAT_R8G8_SINT: case DXGI_FORMAT_R8G8_TYPELESS: channels = 2; bits = 8; break;
    case DXGI_FORMAT_R8_UNORM: case DXGI_FORMAT_A8_UNORM: kind = 2; channels = 1; bits = 8; break;
    case DXGI_FORMAT_R8_SNORM: kind = 3; channels = 1; bits = 8; break;
    case DXGI_FORMAT_R8_UINT: case DXGI_FORMAT_R8_SINT: case DXGI_FORMAT_R8_TYPELESS: channels = 1; bits = 8; break;
    case DXGI_FORMAT_R10G10B10A2_UNORM: case DXGI_FORMAT_R10G10B10A2_UINT: case DXGI_FORMAT_R10G10B10A2_TYPELESS: {
      static const UINT widths[4] = {10, 10, 10, 2};
      UINT32 word = 0, shift = 0;
      for (UINT i = 0; i < 4; ++i) {
        const UINT32 v = floats ? (format == DXGI_FORMAT_R10G10B10A2_UNORM ? unorm_of(fvalues[i], widths[i]) : (UINT32)fvalues[i]) : values[i];
        word |= (v & ((1u << widths[i]) - 1)) << shift;
        shift += widths[i];
      }
      memcpy(out, &word, 4);
      return 4;
    }
    default: return 0;
  }
  if (format == DXGI_FORMAT_A8_UNORM) {      /* (the one channel is alpha) */
    out[0] = (BYTE)(floats ? unorm_of(fvalues[3], 8) : values[3]);
    return 1;
  }
  for (UINT i = 0; i < channels; ++i) {
    const UINT from = bgra && i < 3 ? 2 - i : i;
    UINT32 v;
    if (!floats) v = values[from];
    else if (kind == 1) { if (bits == 32) memcpy(&v, &fvalues[from], 4); else v = half_of(fvalues[from]); }
    else if (kind == 2) v = unorm_of(fvalues[from], bits);
    else if (kind == 3) v = snorm_of(fvalues[from], bits);
    else if (bits == 32 && format != DXGI_FORMAT_R32G32B32A32_UINT && format != DXGI_FORMAT_R32G32B32A32_SINT && format != DXGI_FORMAT_R32G32_UINT &&
             format != DXGI_FORMAT_R32G32_SINT && format != DXGI_FORMAT_R32_UINT && format != DXGI_FORMAT_R32_SINT && format != DXGI_FORMAT_R32G32B32_UINT &&
             format != DXGI_FORMAT_R32G32B32_SINT) memcpy(&v, &fvalues[from], 4);      /* (raw, structured and typeless: the float's bits) */
    else v = (UINT32)(INT)fvalues[from];
    if (bits == 32) memcpy(out + 4 * i, &v, 4);
    else if (bits == 16) { const UINT16 h = (UINT16)v; memcpy(out + 2 * i, &h, 2); }
    else out[i] = (BYTE)v;
  }
  return channels * bits / 8;
}

/* Fills what an unordered access view names with one element made from the clear's values: a buffer's range, or the level and layers of a texture's view. */
static void uav_clear(Ctx *c, View *v, const UINT values[4], const FLOAT fvalues[4], BOOL floats) {
  Dev *d = c->dev;
  BYTE element[16];
  if (v->buf) {
    /* (a raw or structured view is cleared word by word with the first value) */
    const BOOL words = v->format == DXGI_FORMAT_UNKNOWN;
    const UINT bytes = clear_element(words ? DXGI_FORMAT_R32_TYPELESS : v->format, values, fvalues, floats, element);
    if (!bytes || v->size % bytes) return;
    BYTE *fill = mem_alloc((SIZE_T)v->size);
    if (!fill) return;
    for (UINT64 at = 0; at < v->size; at += bytes) memcpy(fill + at, element, bytes);
    buf_write_any(d, v->buf, v->offset, fill, v->size);
    mem_free(fill);
    return;
  }
  Tex *t = v->tex;
  const D3D11_UNORDERED_ACCESS_VIEW_DESC *u = &v->desc.uav;
  UINT level = 0, first = 0, layers = 1;
  switch (u->ViewDimension) {
    case D3D11_UAV_DIMENSION_TEXTURE1D: level = u->Texture1D.MipSlice; break;
    case D3D11_UAV_DIMENSION_TEXTURE1DARRAY: level = u->Texture1DArray.MipSlice; first = u->Texture1DArray.FirstArraySlice; layers = u->Texture1DArray.ArraySize; break;
    case D3D11_UAV_DIMENSION_TEXTURE2D: level = u->Texture2D.MipSlice; break;
    case D3D11_UAV_DIMENSION_TEXTURE2DARRAY: level = u->Texture2DArray.MipSlice; first = u->Texture2DArray.FirstArraySlice; layers = u->Texture2DArray.ArraySize; break;
    case D3D11_UAV_DIMENSION_TEXTURE3D: level = u->Texture3D.MipSlice; first = u->Texture3D.FirstWSlice; layers = u->Texture3D.WSize; break;
    default: return;
  }
  const UINT bytes = clear_element(u->Format != DXGI_FORMAT_UNKNOWN ? u->Format : t->desc.Format, values, fvalues, floats, element);
  if (t->planar) {      /* (a view of a plane: that plane of its slices) */
    if (layers == (UINT)-1 || first + layers > t->desc.ArraySize) layers = first < t->desc.ArraySize ? t->desc.ArraySize - first : 0;
    tex_before_write(d, t);
    for (UINT k = first; k < first + layers; ++k) tex_plane_fill(d, t, v->plane, k, 0, 0, plane_width(t, v->plane), plane_height(t, v->plane), element, bytes);
    return;
  }
  if (!bytes || bytes != t->bpp || level >= t->desc.MipLevels) return;
  const UINT w = mip_size(t->desc.Width, level), h = mip_size(t->desc.Height, level);
  const UINT have = t->kind == 1 ? mip_size(t->slices, level) : t->desc.ArraySize;
  if (layers == (UINT)-1 || first + layers > have) layers = first < have ? have - first : 0;
  BYTE *fill = mem_alloc((SIZE_T)w * h * bytes);
  if (!fill) return;
  for (SIZE_T at = 0; at < (SIZE_T)w * h * bytes; at += bytes) memcpy(fill + at, element, bytes);
  tex_before_write(d, t);
  for (UINT k = first; k < first + layers; ++k) {
    if (t->kind == 1) tex_write(d, t, level, 0, 0, k, w, h, fill, w * bytes);
    else tex_write(d, t, level + k * t->desc.MipLevels, 0, 0, 0, w, h, fill, w * bytes);
  }
  mem_free(fill);
}
static void STDMETHODCALLTYPE ctx_ClearUnorderedAccessViewUint(Ctx *c, ID3D11UnorderedAccessView *view, const UINT values[4]) {
  if (c->deferred && values) { deferred_record(c, &(RecArgs){.op = REC_CLEAR_UAV_UINT, .obj = {view}, .u = {values[0], values[1], values[2], values[3]}}); return; }
  if (!view || !values) return;
  static const FLOAT none[4] = {0, 0, 0, 0};
  if (predicated_out(c->dev)) return;
  LOCK(c);
  uav_clear(c, (View *)view, values, none, FALSE);
  UNLOCK(c);
}
static void STDMETHODCALLTYPE ctx_ClearUnorderedAccessViewFloat(Ctx *c, ID3D11UnorderedAccessView *view, const FLOAT values[4]) {
  if (c->deferred && values) { deferred_record(c, &(RecArgs){.op = REC_CLEAR_UAV_FLOAT, .obj = {view}, .f = {values[0], values[1], values[2], values[3]}}); return; }
  if (!view || !values) return;
  static const UINT none[4] = {0, 0, 0, 0};
  if (predicated_out(c->dev)) return;
  LOCK(c);
  uav_clear(c, (View *)view, none, values, TRUE);
  UNLOCK(c);
}
/* The hidden count of an append or counter view into a buffer (4 bytes at a multiple of 4): the count the program set, if no draw or dispatch took it since, else what the
 * shaders left in the view's counter (copied in order with the commands before). */
static void STDMETHODCALLTYPE ctx_CopyStructureCount(Ctx *c, ID3D11Buffer *buffer, UINT offset, ID3D11UnorderedAccessView *view) {
  if (c->deferred) { deferred_record(c, &(RecArgs){.op = REC_COPY_STRUCTURE_COUNT, .obj = {buffer, view}, .u = {offset}}); return; }
  Buf *b = (Buf *)buffer;
  View *v = (View *)view;
  if (!b || !v || !v->buf || offset % 4 || offset > b->desc.ByteWidth || b->desc.ByteWidth - offset < 4 ||
      !(v->desc.uav.Buffer.Flags & (D3D11_BUFFER_UAV_FLAG_APPEND | D3D11_BUFFER_UAV_FLAG_COUNTER))) return;
  if (predicated_out(c->dev)) return;
  LOCK(c);
  if (v->counter_dirty || !v->counter_buf) buf_write_any(c->dev, b, offset, &v->counter, 4);
  else buf_copy(c->dev, b, offset, v->counter_buf, 0, 4);
  UNLOCK(c);
}

/* A multisampled texture into an ordinary one of its size and format: the samples of each pixel averaged (Direct3D 12's ResolveSubresource below). */
static void STDMETHODCALLTYPE ctx_ResolveSubresource(Ctx *c, ID3D11Resource *to, UINT to_sub, ID3D11Resource *from, UINT from_sub, DXGI_FORMAT format) {
  if (c->deferred) { deferred_record(c, &(RecArgs){.op = REC_RESOLVE, .obj = {to, from}, .u = {to_sub, from_sub, (UINT)format}}); return; }
  (void)format;
  Dev *d = c->dev;
  Tex *tt = tex_of_resource(to), *tf = tex_of_resource(from);
  /* (a slice of a multisampled array - its subresource is the slice: one level - into a subresource of a texture of one sample) */
  if (!tt || !tf || from_sub >= tf->desc.ArraySize || tf->desc.SampleDesc.Count < 2 || tt->desc.SampleDesc.Count > 1 || tt->desc.Width != tf->desc.Width ||
      tt->desc.Height != tf->desc.Height || tt->stored != tf->stored || to_sub >= tt->desc.MipLevels * (tt->kind == 1 ? 1 : tt->desc.ArraySize) || predicated_out(d)) return;
  LOCK(c);
  tex_before_write(d, tt);
  tex_before_read(d, tf);
  tex_sync(d, tt);
  ++tt->version;
  ID3D12GraphicsCommandList_ResolveSubresource(d->list, tt->res, to_sub, tf->res, from_sub, tt->stored);      /* (the stored format: a typed view of it asked for is the same bytes) */
  tt->used = tf->used = tt->written = d->epoch;
  d->recorded = 1;
  UNLOCK(c);
}

static void STDMETHODCALLTYPE ctx_Flush(Ctx *c) {
  if (c->deferred) return;
  LOCK(c);
  flush(c->dev);
  UNLOCK(c);
}
/* The bound objects of a context as an array (the references of Ctx from `vs` to `predicate`). */
static DxObj **ctx_objects(Ctx *c, UINT *count) {
  *count = (UINT)((CTX_OBJECTS_END - CTX_OBJECTS_FIRST) / sizeof(DxObj *));
  return (DxObj **)((BYTE *)c + CTX_OBJECTS_FIRST);
}
static void ctx_unbind_all(Ctx *c) {
  UINT n;
  DxObj **objects = ctx_objects(c, &n);
  for (UINT i = 0; i < n; ++i) bind(&objects[i], NULL);
  for (UINT i = 0; i < 4; ++i) bind(&c->so_buf[i], NULL);
  c->rtv_count = 0;
}
static void ctx_release_all(Ctx *c) {
  ctx_unbind_all(c);
  bind(&c->active, NULL);
}
static void STDMETHODCALLTYPE ctx_ClearState(Ctx *c) {
  LOCK(c);
  ctx_unbind_all(c);
  memset((BYTE *)c + CTX_VALUES_FIRST, 0, CTX_VALUES_END - CTX_VALUES_FIRST);      /* (no viewports or scissor rectangles, the topology UNDEFINED, no predication) */
  c->sample_mask = 0xFFFFFFFFu;
  for (UINT i = 0; i < 4; ++i) c->blend_factor[i] = 1.0f;
  c->dev->dirty = DIRTY_ALL;
  UNLOCK(c);
}

/* SetResourceMinLOD: the most detailed level a texture is sampled at, in its levels (the ResourceMinLODClamp of every Direct3D 12 view of it the draws make: a view whose
 * MostDetailedMip is above 0 counts it from the texture's level 0 all the same; a clamp past a view's last level reads that level). Buffers have no levels; a value below 0 or
 * one that is not a number is not taken. */
static void STDMETHODCALLTYPE ctx_SetResourceMinLOD(Ctx *c, ID3D11Resource *resource, FLOAT lod) {
  if (c->deferred) { deferred_record(c, &(RecArgs){.op = REC_MIN_LOD, .obj = {resource}, .f = {lod}}); return; }
  Tex *t = tex_of_resource(resource);
  if (!t || !(lod >= 0.0f)) return;
  LOCK(c);
  t->min_lod = lod;
  UNLOCK(c);
}
static FLOAT STDMETHODCALLTYPE ctx_GetResourceMinLOD(Ctx *c, ID3D11Resource *resource) {
  (void)c;
  const Tex *t = tex_of_resource(resource);
  return t ? t->min_lod : 0.0f;
}
