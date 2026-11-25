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

/* Part of d3d12.c: what the command list interfaces after ID3D12GraphicsCommandList add (ID3D12GraphicsCommandList1 .. 7), and the clears of unordered access views.
 *
 * Done: atomic buffer copies (the queue runs a list's commands one after the other, so a copy of 4 or 8 bytes is atomic), regions resolved with a mode,
 * WriteBufferImmediate, render passes (tier 0: the targets bound, the beginning accesses' clears, the ending accesses' resolves), the enhanced barriers (the
 * kernel's allocations are not state tracked: as ResourceBarrier, nothing to do). The calls of the features CheckFeatureSupport reports at their "not supported"
 * tier - depth bounds, programmable sample positions, view instancing, protected sessions, meta commands, raytracing, variable-rate shading, mesh shaders - are
 * calls a program may not make on this device; they are left out, as Direct3D 12 leaves out an invalid call. */

/* ---- ID3D12GraphicsCommandList1 ------------------------------------------------------------------------------------------------------------------------------- */

static void STDMETHODCALLTYPE list_AtomicCopyBufferUINT(CmdList *l, ID3D12Resource *dst, UINT64 dst_offset, ID3D12Resource *src, UINT64 src_offset, UINT dependents,
                                                        ID3D12Resource *const *resources, const D3D12_SUBRESOURCE_RANGE_UINT64 *ranges) {
  if (dst_offset % 4 || src_offset % 4) return;
  keep(l, (IUnknown *)dst);
  keep(l, (IUnknown *)src);
  list_CopyBufferRegion(l, dst, dst_offset, src, src_offset, 4);
}
static void STDMETHODCALLTYPE list_AtomicCopyBufferUINT64(CmdList *l, ID3D12Resource *dst, UINT64 dst_offset, ID3D12Resource *src, UINT64 src_offset, UINT dependents,
                                                          ID3D12Resource *const *resources, const D3D12_SUBRESOURCE_RANGE_UINT64 *ranges) {
  if (dst_offset % 8 || src_offset % 8) return;
  keep(l, (IUnknown *)dst);
  keep(l, (IUnknown *)src);
  list_CopyBufferRegion(l, dst, dst_offset, src, src_offset, 8);
}
/* (D3D12_FEATURE_DATA_D3D12_OPTIONS2.DepthBoundsTestSupported is FALSE) */
static void STDMETHODCALLTYPE list_OMSetDepthBounds(CmdList *l, FLOAT min, FLOAT max) { (void)l; (void)min; (void)max; }
/* (ProgrammableSamplePositionsTier: not supported) */
static void STDMETHODCALLTYPE list_SetSamplePositions(CmdList *l, UINT samples, UINT pixels, D3D12_SAMPLE_POSITION *positions) { (void)l; (void)samples; (void)pixels; (void)positions; }
/* (ViewInstancingTier: not supported; a mask of the one view is all there is) */
static void STDMETHODCALLTYPE list_SetViewInstanceMask(CmdList *l, UINT mask) { (void)l; (void)mask; }

/* A region of a multisampled texture's samples into a subresource of a texture, at (x, y): averaged, or the least or greatest of each pixel's (the kernel's resolve
 * modes). The whole of it at (0, 0) of a plain texture of its size (one level, one layer) is resolved in place. Anything else is resolved into a texture of the
 * source's size first and the region copied from there: straight into a plain texture, or through a buffer of the list's into a level or layer of a texture with
 * several (the kernel copies buffers to any level and layer). DECOMPRESS resolves in place, and nothing is compressed here, so there is nothing to do; the sampler
 * feedback modes need SamplerFeedbackTier, which is NOT_SUPPORTED. A call outside the rules (a subresource that is not there, a region outside the source or the
 * destination, a mode the adapter has not) is left out, as Direct3D 12 leaves out an invalid call. */
static void STDMETHODCALLTYPE list_ResolveSubresourceRegion(CmdList *l, ID3D12Resource *dst, UINT dst_sub, UINT x, UINT y, ID3D12Resource *src, UINT src_sub, D3D12_RECT *rect,
                                                            DXGI_FORMAT format, D3D12_RESOLVE_MODE mode) {
  Resource *d = (Resource *)dst, *s = (Resource *)src;
  (void)format;      /* (the resources' own formats: a typeless resource is not made here) */
  if (!d || !s || l->bundle) return;
  if (mode == D3D12_RESOLVE_MODE_DECOMPRESS) return;
  if (mode != D3D12_RESOLVE_MODE_AVERAGE && mode != D3D12_RESOLVE_MODE_MIN && mode != D3D12_RESOLVE_MODE_MAX) return;
  if (s->desc.SampleDesc.Count < 2 || d->desc.SampleDesc.Count > 1 || d->planes || d->desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D) return;
  if (src_sub >= res_subresources(s) || dst_sub >= res_subresources(d)) return;      /* (a multisampled texture has one subresource here) */
  const UINT kmode = mode == D3D12_RESOLVE_MODE_AVERAGE ? 0 : mode == D3D12_RESOLVE_MODE_MIN ? 2 : 3;
  const int depth = (s->desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) != 0;
  /* (a mode the adapter does not have - a colour image's least or greatest samples, any of a depth image's three - is a call this device does not take: the kernel's
   * resolves without kFeatureResolveModes are a colour image's average and a depth image's sample 0) */
  const UINT features = l->d.dev->caps.features;
  const BOOL colour_extremes = (features & DEV_FEATURE_RESOLVE_MODES) || (l->d.dev->caps.features2 & DEV_FEATURE2_COLOR_RESOLVE_MODES);
  if (depth ? !(features & DEV_FEATURE_RESOLVE_MODES) : kmode != 0 && !colour_extremes) return;
  if (depth && !(features & DEV_FEATURE_DEPTH_RESOLVE)) return;
  const LONG w = (LONG)s->desc.Width, h = (LONG)s->desc.Height;
  const D3D12_RECT whole_rect = {0, 0, w, h};
  const D3D12_RECT r = rect ? *rect : whole_rect;
  if (r.left < 0 || r.top < 0 || r.right > w || r.bottom > h || r.left >= r.right || r.top >= r.bottom) return;
  UINT level, layer;
  sub_split(d, dst_sub, &level, &layer);
  const UINT dw = (UINT)(d->desc.Width >> level) ? (UINT)(d->desc.Width >> level) : 1, dh = (d->desc.Height >> level) ? (d->desc.Height >> level) : 1;
  const UINT rw = (UINT)(r.right - r.left), rh = (UINT)(r.bottom - r.top);
  if (x > dw || rw > dw - x || y > dh || rh > dh - y) return;
  const BOOL plain = d->mips == 1 && d->kind == 0;
  const BOOL direct = plain && x == 0 && y == 0 && r.left == 0 && r.top == 0 && r.right == w && r.bottom == h && d->desc.Width == s->desc.Width && d->desc.Height == s->desc.Height;
  keep(l, (IUnknown *)dst);
  keep(l, (IUnknown *)src);
  ID3D12Resource *target = dst;
  Resource *temp = NULL;
  if (!direct) {
    D3D12_RESOURCE_DESC td = s->desc;
    td.SampleDesc.Count = 1;
    td.SampleDesc.Quality = 0;
    td.Flags &= ~(D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    const D3D12_HEAP_PROPERTIES props = {D3D12_HEAP_TYPE_DEFAULT, D3D12_CPU_PAGE_PROPERTY_UNKNOWN, D3D12_MEMORY_POOL_UNKNOWN, 1, 1};
    if (FAILED(resource_make(l->d.dev, &props, D3D12_HEAP_FLAG_NONE, &td, NULL, 0, 0, &temp))) return;
    const int kept_ = keep(l, (IUnknown *)temp);
    dx_Release(OBJ(temp));      /* (the list's reference keeps it until the list is reset) */
    if (!kept_) return;
    target = (ID3D12Resource *)temp;
  }
  Resource *t = (Resource *)target;
  end_pass(l);
  ensure_room(l, 64, 4);
  UINT payload[3] = {alloc_index(l, t->alloc), alloc_index(l, s->alloc), kmode};
  packet(l, OP_RESOLVE_REGION, payload, 3);
  if (direct) return;
  const D3D12_BOX box = {(UINT)r.left, (UINT)r.top, 0, (UINT)r.right, (UINT)r.bottom, 1};
  D3D12_TEXTURE_COPY_LOCATION from = {target, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {.SubresourceIndex = 0}};
  if (plain) {
    D3D12_TEXTURE_COPY_LOCATION to = {dst, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {.SubresourceIndex = dst_sub}};
    list_CopyTextureRegion(l, &to, x, y, 0, &from, &box);
    return;
  }
  /* (a level or layer: the resolved texture into a buffer, and the region from there into the subresource) */
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp;
  UINT64 total = 0;
  get_footprints(&t->desc, 0, 1, 0, &fp, NULL, NULL, &total);
  if (!total || total == ~(UINT64)0) return;
  const D3D12_RESOURCE_DESC bd = {D3D12_RESOURCE_DIMENSION_BUFFER, 0, KALIGN(total, 256), 1, 1, 1, DXGI_FORMAT_UNKNOWN, {1, 0}, D3D12_TEXTURE_LAYOUT_ROW_MAJOR, D3D12_RESOURCE_FLAG_NONE};
  const D3D12_HEAP_PROPERTIES bprops = {D3D12_HEAP_TYPE_DEFAULT, D3D12_CPU_PAGE_PROPERTY_UNKNOWN, D3D12_MEMORY_POOL_UNKNOWN, 1, 1};
  Resource *staging = NULL;
  if (FAILED(resource_make(l->d.dev, &bprops, D3D12_HEAP_FLAG_NONE, &bd, NULL, 0, 0, &staging))) return;
  const int kept_staging = keep(l, (IUnknown *)staging);
  dx_Release(OBJ(staging));
  if (!kept_staging) return;
  D3D12_TEXTURE_COPY_LOCATION buffer = {(ID3D12Resource *)staging, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {.PlacedFootprint = fp}};
  list_CopyTextureRegion(l, &buffer, 0, 0, 0, &from, NULL);
  D3D12_TEXTURE_COPY_LOCATION to = {dst, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {.SubresourceIndex = dst_sub}};
  list_CopyTextureRegion(l, &to, x, y, 0, &buffer, &box);
}

/* ---- ID3D12GraphicsCommandList2: WriteBufferImmediate ------------------------------------------------------------------------------------------------------------ */
/* Each 32-bit value goes through an upload buffer of the list's and a copy, in its place among the commands (the markers' modes are the same here: the queue runs
 * one command after the other). */
static void STDMETHODCALLTYPE list_WriteBufferImmediate(CmdList *l, UINT count, const D3D12_WRITEBUFFERIMMEDIATE_PARAMETER *params, const D3D12_WRITEBUFFERIMMEDIATE_MODE *modes) {
  if (!params || l->bundle) return;
  for (UINT i = 0; i < count; ++i) {
    if (modes && modes[i] > D3D12_WRITEBUFFERIMMEDIATE_MODE_MARKER_OUT) continue;
    UINT64 offset = 0;
    Resource *dst = va_find(params[i].Dest, &offset);
    if (!dst || params[i].Dest % 4) continue;
    if (!l->immediate || l->immediate_used + 4 > 4096) {
      const D3D12_HEAP_PROPERTIES props = {D3D12_HEAP_TYPE_UPLOAD, D3D12_CPU_PAGE_PROPERTY_UNKNOWN, D3D12_MEMORY_POOL_UNKNOWN, 1, 1};
      const D3D12_RESOURCE_DESC bd = {D3D12_RESOURCE_DIMENSION_BUFFER, 0, 4096, 1, 1, 1, DXGI_FORMAT_UNKNOWN, {1, 0}, D3D12_TEXTURE_LAYOUT_ROW_MAJOR, D3D12_RESOURCE_FLAG_NONE};
      Resource *r = NULL;
      if (FAILED(resource_make(l->d.dev, &props, D3D12_HEAP_FLAG_NONE, &bd, NULL, 0, 0, &r))) return;
      const int kept_ = keep(l, (IUnknown *)r);
      dx_Release(OBJ(r));
      if (!kept_) return;
      l->immediate = r;
      l->immediate_used = 0;
    }
    memcpy(l->immediate->shadow + l->immediate_used, &params[i].Value, 4);
    l->immediate->dirty = 1;
    keep(l, (IUnknown *)dst);
    list_CopyBufferRegion(l, (ID3D12Resource *)dst, offset, (ID3D12Resource *)l->immediate, l->immediate_used, 4);
    l->immediate_used += 4;
  }
}

/* ---- ID3D12GraphicsCommandList3, 4 ------------------------------------------------------------------------------------------------------------------------------------- */

/* (no protected resource sessions: NULL is the only one there is) */
static void STDMETHODCALLTYPE list_SetProtectedResourceSession(CmdList *l, ID3D12ProtectedResourceSession *session) { (void)l; (void)session; }

/* A render pass (D3D12_RENDER_PASS_TIER_0: done with the list's own calls): the targets bound, the ones whose beginning access says so cleared. EndRenderPass
 * does the ending accesses' resolves. Preserving and discarding change nothing here (what a discard leaves is undefined: the contents stay). A pass that resumes a
 * suspended one (D3D12_RENDER_PASS_FLAG_RESUMING_PASS) skips its beginning accesses, and one that is suspended (SUSPENDING_PASS) its ending ones: the pass that
 * resumes it last ends it. The resolve parameters are copied (the program's array need not outlive the call). */
static UINT rp_param_count(const D3D12_RENDER_PASS_ENDING_ACCESS *end) {
  return end->Type == D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_RESOLVE && end->Resolve.pSubresourceParameters ? end->Resolve.SubresourceCount : 0;
}
/* Points `end` at its copy of the parameters at *at. */
static void rp_param_copy(CmdList *l, D3D12_RENDER_PASS_ENDING_ACCESS *end, UINT *at) {
  const UINT n = rp_param_count(end);
  if (!n) { end->Resolve.pSubresourceParameters = NULL; end->Resolve.SubresourceCount = 0; return; }
  memcpy(&l->rp_params[*at], end->Resolve.pSubresourceParameters, (SIZE_T)n * sizeof l->rp_params[0]);
  end->Resolve.pSubresourceParameters = &l->rp_params[*at];
  *at += n;
}
static void STDMETHODCALLTYPE list_BeginRenderPass(CmdList *l, UINT count, const D3D12_RENDER_PASS_RENDER_TARGET_DESC *targets, const D3D12_RENDER_PASS_DEPTH_STENCIL_DESC *depth,
                                                   D3D12_RENDER_PASS_FLAGS flags) {
  if (count > 8 || (count && !targets) || l->in_render_pass) return;
  /* (room for the resolve parameters first: without it the pass is not begun) */
  UINT64 params = 0;
  if (!(flags & D3D12_RENDER_PASS_FLAG_SUSPENDING_PASS)) {
    for (UINT i = 0; i < count; ++i) params += rp_param_count(&targets[i].EndingAccess);
    if (depth) params += (UINT64)rp_param_count(&depth->DepthEndingAccess) + rp_param_count(&depth->StencilEndingAccess);
  }
  if (params > (1u << 16)) return;
  if (params > l->rp_params_cap) {
    void *grown = mem_grow(l->rp_params, (SIZE_T)params * sizeof l->rp_params[0]);
    if (!grown) return;
    l->rp_params = grown;
    l->rp_params_cap = (UINT)params;
  }
  const int resuming = (flags & D3D12_RENDER_PASS_FLAG_RESUMING_PASS) != 0;
  D3D12_CPU_DESCRIPTOR_HANDLE rtvs[8];
  for (UINT i = 0; i < count; ++i) rtvs[i] = targets[i].cpuDescriptor;
  list_OMSetRenderTargets(l, count, rtvs, FALSE, depth ? &depth->cpuDescriptor : NULL);
  for (UINT i = 0; i < count; ++i)
    if (!resuming && targets[i].BeginningAccess.Type == D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_CLEAR)
      list_ClearRenderTargetView(l, targets[i].cpuDescriptor, targets[i].BeginningAccess.Clear.ClearValue.Color, 0, NULL);
  if (depth && !resuming) {
    UINT clear = 0;
    if (depth->DepthBeginningAccess.Type == D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_CLEAR) clear |= D3D12_CLEAR_FLAG_DEPTH;
    if (depth->StencilBeginningAccess.Type == D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_CLEAR) clear |= D3D12_CLEAR_FLAG_STENCIL;
    if (clear)
      list_ClearDepthStencilView(l, depth->cpuDescriptor, (D3D12_CLEAR_FLAGS)clear, depth->DepthBeginningAccess.Clear.ClearValue.DepthStencil.Depth,
                                 depth->StencilBeginningAccess.Clear.ClearValue.DepthStencil.Stencil, 0, NULL);
  }
  l->in_render_pass = 1;
  l->rp_suspending = (flags & D3D12_RENDER_PASS_FLAG_SUSPENDING_PASS) != 0;
  l->rp_count = count;
  UINT at = 0;
  for (UINT i = 0; i < count; ++i) {
    l->rp_rts[i] = targets[i];
    if (l->rp_suspending) l->rp_rts[i].EndingAccess.Type = D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_PRESERVE;
    rp_param_copy(l, &l->rp_rts[i].EndingAccess, &at);
  }
  l->rp_has_ds = depth != NULL;
  if (depth) {
    l->rp_ds = *depth;
    if (l->rp_suspending) l->rp_ds.DepthEndingAccess.Type = l->rp_ds.StencilEndingAccess.Type = D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_PRESERVE;
    rp_param_copy(l, &l->rp_ds.DepthEndingAccess, &at);
    rp_param_copy(l, &l->rp_ds.StencilEndingAccess, &at);
  }
}
static void rp_resolve(CmdList *l, const D3D12_RENDER_PASS_ENDING_ACCESS *end) {
  if (end->Type != D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_RESOLVE || !end->Resolve.pSubresourceParameters) return;
  for (UINT k = 0; k < end->Resolve.SubresourceCount; ++k) {
    const D3D12_RENDER_PASS_ENDING_ACCESS_RESOLVE_SUBRESOURCE_PARAMETERS *p = &end->Resolve.pSubresourceParameters[k];
    D3D12_RECT rect = p->SrcRect;
    list_ResolveSubresourceRegion(l, end->Resolve.pDstResource, p->DstSubresource, p->DstX, p->DstY, end->Resolve.pSrcResource, p->SrcSubresource, &rect, end->Resolve.Format,
                                  end->Resolve.ResolveMode);
  }
}
static void STDMETHODCALLTYPE list_EndRenderPass(CmdList *l) {
  if (!l->in_render_pass) return;
  for (UINT i = 0; i < l->rp_count; ++i) rp_resolve(l, &l->rp_rts[i].EndingAccess);
  if (l->rp_has_ds) { rp_resolve(l, &l->rp_ds.DepthEndingAccess); rp_resolve(l, &l->rp_ds.StencilEndingAccess); }
  l->in_render_pass = 0;
}
/* (no meta commands are made, no raytracing, no state objects: D3D12_RAYTRACING_TIER_NOT_SUPPORTED) */
static void STDMETHODCALLTYPE list_InitializeMetaCommand(CmdList *l, ID3D12MetaCommand *command, const void *data, SIZE_T size) { (void)l; (void)command; (void)data; (void)size; }
static void STDMETHODCALLTYPE list_ExecuteMetaCommand(CmdList *l, ID3D12MetaCommand *command, const void *data, SIZE_T size) { (void)l; (void)command; (void)data; (void)size; }
static void STDMETHODCALLTYPE list_BuildRaytracingAccelerationStructure(CmdList *l, const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC *desc, UINT count,
                                                                        const D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_DESC *infos) { (void)l; (void)desc; (void)count; (void)infos; }
static void STDMETHODCALLTYPE list_EmitRaytracingAccelerationStructurePostbuildInfo(CmdList *l, const D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_DESC *desc, UINT count,
                                                                                    const D3D12_GPU_VIRTUAL_ADDRESS *structures) { (void)l; (void)desc; (void)count; (void)structures; }
static void STDMETHODCALLTYPE list_CopyRaytracingAccelerationStructure(CmdList *l, D3D12_GPU_VIRTUAL_ADDRESS dst, D3D12_GPU_VIRTUAL_ADDRESS src,
                                                                       D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE mode) { (void)l; (void)dst; (void)src; (void)mode; }
static void STDMETHODCALLTYPE list_SetPipelineState1(CmdList *l, ID3D12StateObject *state) { (void)l; (void)state; }
static void STDMETHODCALLTYPE list_DispatchRays(CmdList *l, const D3D12_DISPATCH_RAYS_DESC *desc) { (void)l; (void)desc; }

/* ---- ID3D12GraphicsCommandList5, 6, 7 -------------------------------------------------------------------------------------------------------------------------------- */

/* (VariableShadingRateTier: not supported - every pixel is shaded once, which is the 1x1 rate) */
static void STDMETHODCALLTYPE list_RSSetShadingRate(CmdList *l, D3D12_SHADING_RATE rate, const D3D12_SHADING_RATE_COMBINER *combiners) { (void)l; (void)rate; (void)combiners; }
static void STDMETHODCALLTYPE list_RSSetShadingRateImage(CmdList *l, ID3D12Resource *image) { (void)l; (void)image; }
/* (MeshShaderTier: not supported) */
static void STDMETHODCALLTYPE list_DispatchMesh(CmdList *l, UINT x, UINT y, UINT z) { (void)l; (void)x; (void)y; (void)z; }
/* The enhanced barriers. The kernel's allocations are not state tracked (as with ResourceBarrier), so layouts and accesses need no transition, and a layout of
 * UNDEFINED discards (the contents are not kept, which is what the kernel does anyway). What does need doing is aliasing: a placed resource whose access after the
 * barrier is NO_ACCESS goes out of use, which Direct3D 12's enhanced barriers say instead of an aliasing barrier - the kernel's aliasing barrier with that resource
 * going out and none named coming in (ResourceBarrier's: what the GPU holds of it is written to the heap's memory and every allocation sharing those bytes is
 * marked to take them up on its next use). A resource coming into use (access before NO_ACCESS) needs nothing more: the outgoing one's barrier did the work. */
static void barrier_alias_out(CmdList *l, ID3D12Resource *resource) {
  Resource *r = (Resource *)resource;
  if (!r || !r->heap) return;      /* (a committed resource shares its bytes with nothing) */
  D3D12_RESOURCE_BARRIER b;
  memset(&b, 0, sizeof b);
  b.Type = D3D12_RESOURCE_BARRIER_TYPE_ALIASING;
  b.Aliasing.pResourceBefore = resource;
  b.Aliasing.pResourceAfter = NULL;
  list_ResourceBarrier(l, 1, &b);
}
static void STDMETHODCALLTYPE list_Barrier(CmdList *l, UINT32 count, const D3D12_BARRIER_GROUP *groups) {
  if (!groups) return;
  for (UINT32 g = 0; g < count; ++g) {
    const D3D12_BARRIER_GROUP *group = &groups[g];
    if (group->Type == D3D12_BARRIER_TYPE_TEXTURE && group->pTextureBarriers) {
      for (UINT32 i = 0; i < group->NumBarriers; ++i)
        if (group->pTextureBarriers[i].AccessAfter == D3D12_BARRIER_ACCESS_NO_ACCESS) barrier_alias_out(l, group->pTextureBarriers[i].pResource);
    } else if (group->Type == D3D12_BARRIER_TYPE_BUFFER && group->pBufferBarriers) {
      for (UINT32 i = 0; i < group->NumBarriers; ++i)
        if (group->pBufferBarriers[i].AccessAfter == D3D12_BARRIER_ACCESS_NO_ACCESS) barrier_alias_out(l, group->pBufferBarriers[i].pResource);
    }
  }
}

/* ---- clearing unordered access views ------------------------------------------------------------------------------------------------------------------------------ */
/* A buffer's range (the view's elements), filled with the value: Uint writes each 32-bit word (raw and structured views) or each element of a typed view's format;
 * Float the float's bits, or the element converted to the view's format. The pattern goes through an upload buffer of the list's and a copy. A texture's view the same
 * way, into each of its layers (uav_clear_texture). */
/* A format's channels: count, bits of each, and what they hold (0 float, 1 unorm, 2 snorm, 3 uint, 4 sint). R10G10B10A2 is the one with uneven channels. */
typedef struct UavFormat { DXGI_FORMAT f; BYTE channels, bits, kind; } UavFormat;
static const UavFormat g_uav_formats[] = {
    {DXGI_FORMAT_R32G32B32A32_FLOAT, 4, 32, 0}, {DXGI_FORMAT_R32G32B32A32_UINT, 4, 32, 3}, {DXGI_FORMAT_R32G32B32A32_SINT, 4, 32, 4},
    {DXGI_FORMAT_R32G32B32_FLOAT, 3, 32, 0}, {DXGI_FORMAT_R32G32B32_UINT, 3, 32, 3}, {DXGI_FORMAT_R32G32B32_SINT, 3, 32, 4},
    {DXGI_FORMAT_R32G32_FLOAT, 2, 32, 0}, {DXGI_FORMAT_R32G32_UINT, 2, 32, 3}, {DXGI_FORMAT_R32G32_SINT, 2, 32, 4},
    {DXGI_FORMAT_R32_FLOAT, 1, 32, 0}, {DXGI_FORMAT_R32_UINT, 1, 32, 3}, {DXGI_FORMAT_R32_SINT, 1, 32, 4},
    {DXGI_FORMAT_R16G16B16A16_FLOAT, 4, 16, 0}, {DXGI_FORMAT_R16G16B16A16_UNORM, 4, 16, 1}, {DXGI_FORMAT_R16G16B16A16_SNORM, 4, 16, 2},
    {DXGI_FORMAT_R16G16B16A16_UINT, 4, 16, 3}, {DXGI_FORMAT_R16G16B16A16_SINT, 4, 16, 4},
    {DXGI_FORMAT_R16G16_FLOAT, 2, 16, 0}, {DXGI_FORMAT_R16G16_UNORM, 2, 16, 1}, {DXGI_FORMAT_R16G16_SNORM, 2, 16, 2}, {DXGI_FORMAT_R16G16_UINT, 2, 16, 3},
    {DXGI_FORMAT_R16G16_SINT, 2, 16, 4}, {DXGI_FORMAT_R16_FLOAT, 1, 16, 0}, {DXGI_FORMAT_R16_UNORM, 1, 16, 1}, {DXGI_FORMAT_R16_SNORM, 1, 16, 2},
    {DXGI_FORMAT_R16_UINT, 1, 16, 3}, {DXGI_FORMAT_R16_SINT, 1, 16, 4},
    {DXGI_FORMAT_R8G8B8A8_UNORM, 4, 8, 1}, {DXGI_FORMAT_R8G8B8A8_SNORM, 4, 8, 2}, {DXGI_FORMAT_R8G8B8A8_UINT, 4, 8, 3}, {DXGI_FORMAT_R8G8B8A8_SINT, 4, 8, 4},
    {DXGI_FORMAT_B8G8R8A8_UNORM, 4, 8, 1}, {DXGI_FORMAT_R8G8_UNORM, 2, 8, 1}, {DXGI_FORMAT_R8G8_SNORM, 2, 8, 2}, {DXGI_FORMAT_R8G8_UINT, 2, 8, 3},
    {DXGI_FORMAT_R8G8_SINT, 2, 8, 4}, {DXGI_FORMAT_R8_UNORM, 1, 8, 1}, {DXGI_FORMAT_R8_SNORM, 1, 8, 2}, {DXGI_FORMAT_R8_UINT, 1, 8, 3}, {DXGI_FORMAT_R8_SINT, 1, 8, 4},
    {DXGI_FORMAT_A8_UNORM, 1, 8, 1}, {DXGI_FORMAT_R10G10B10A2_UNORM, 4, 10, 1}, {DXGI_FORMAT_R10G10B10A2_UINT, 4, 10, 3},
};
static UINT16 float_to_half(float x) {
  UINT32 b;
  memcpy(&b, &x, 4);
  const UINT32 sign = (b >> 16) & 0x8000u, mant = b & 0x7FFFFFu;
  const INT e = (INT)((b >> 23) & 255u);
  if (e == 255) return (UINT16)(sign | 0x7C00u | (mant ? 0x200u : 0));      /* (infinity, NaN) */
  const INT he = e - 127 + 15;
  if (he >= 31) return (UINT16)(sign | 0x7C00u);
  if (he <= 0) {
    if (he < -10) return (UINT16)sign;
    const UINT32 m = mant | 0x800000u;
    return (UINT16)(sign | (m >> (14 - he)));
  }
  return (UINT16)(sign | ((UINT32)he << 10) | (mant >> 13));
}
/* One channel's bits: Uint writes the value's low bits (as Direct3D 12 does, also into a float or normalized format); Float converts to what the channel holds. */
static UINT32 uav_channel(const UavFormat *u, UINT c, UINT uv, float fv, int floats) {
  const UINT bits = u->f == DXGI_FORMAT_R10G10B10A2_UNORM || u->f == DXGI_FORMAT_R10G10B10A2_UINT ? (c == 3 ? 2 : 10) : u->bits;
  const UINT32 mask = bits == 32 ? 0xFFFFFFFFu : ((1u << bits) - 1);
  if (!floats) return uv & mask;
  if (fv != fv) fv = 0;      /* (NaN: 0 in the normalized and integer formats) */
  switch (u->kind) {
    case 0: {
      if (bits == 16) return float_to_half(fv);
      UINT32 b;
      memcpy(&b, &fv, 4);
      return b;
    }
    case 1: { const float x = fv < 0 ? 0 : fv > 1 ? 1 : fv; return (UINT32)(x * (float)mask + 0.5f); }
    case 2: {
      const float x = fv < -1 ? -1 : fv > 1 ? 1 : fv;
      const float m = (float)(mask >> 1);
      const INT32 v = (INT32)(x * m + (x < 0 ? -0.5f : 0.5f));
      return (UINT32)v & mask;
    }
    case 3: return fv <= 0 ? 0 : fv >= (float)mask ? mask : (UINT32)fv;
    default: {
      const float hi = (float)(mask >> 1), lo = -hi - 1;
      const INT32 v = fv <= lo ? (INT32)lo : fv >= hi ? (INT32)hi : (INT32)fv;
      return (UINT32)v & mask;
    }
  }
}
/* The element's bytes; 0 for a format a view cannot be cleared in. A raw or structured view (no format) is cleared a 32-bit word at a time, with the first value. */
static UINT uav_clear_element(DXGI_FORMAT format, const UINT u[4], const float f[4], int floats, BYTE out[16]) {
  if (format == DXGI_FORMAT_UNKNOWN || format == DXGI_FORMAT_R32_TYPELESS) {
    memcpy(out, floats ? (const void *)&f[0] : (const void *)&u[0], 4);
    return 4;
  }
  const UavFormat *uf = NULL;
  for (UINT i = 0; i < sizeof g_uav_formats / sizeof g_uav_formats[0]; ++i)
    if (g_uav_formats[i].f == format) uf = &g_uav_formats[i];
  if (!uf) return 0;
  if (uf->bits == 10) {
    UINT32 w = 0;
    for (UINT c = 0; c < 4; ++c) w |= uav_channel(uf, c, u[c], f[c], floats) << (c * 10);
    memcpy(out, &w, 4);
    return 4;
  }
  const UINT size = uf->bits / 8;
  for (UINT c = 0; c < uf->channels; ++c) {
    /* (B8G8R8A8: the first value is red, which is the third byte) */
    const UINT src = uf->f == DXGI_FORMAT_B8G8R8A8_UNORM ? (c == 0 ? 2 : c == 2 ? 0 : c) : c;
    const UINT32 v = uav_channel(uf, src, u[src], f[src], floats);
    memcpy(out + c * size, &v, size);      /* (little-endian: the low bytes) */
  }
  return size * uf->channels;
}
/* A texture's view (DK_UAV_TEXTURE): the rectangles (all of the level when there are none) of its level in each of its layers, filled with the value in the view's format - the
 * element goes through an upload buffer of the list's and a copy into the image of each layer. A view through a mirror (a plane, a narrow texture) clears the planes or the
 * texture it shows - each slice's plane allocation - with the narrow copy that keeps the widened image's other channels; the mirror takes it from them when it is next used. */
static void uav_clear_texture(CmdList *l, const SoftDesc *d, Resource *r, const UINT u[4], const float f[4], int floats, UINT rect_count, const D3D12_RECT *rects) {
  BYTE element[16];
  const UINT bytes = uav_clear_element(d->format, u, f, floats, element);
  if (!bytes || !r->fmt) return;
  const int mirrored = (d->offset & SRV_MIRRORS) != 0;
  const UINT narrow = (UINT)(d->size >> 32);
  static const DXGI_FORMAT narrow_formats[5] = {DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_R8_TYPELESS, DXGI_FORMAT_R8G8_TYPELESS, DXGI_FORMAT_R16_TYPELESS, DXGI_FORMAT_R16G16_TYPELESS};
  const FormatInfo *copy_format = narrow && narrow < 5 ? format_info(narrow_formats[narrow]) : NULL;
  const UINT plane = mirrored ? (d->pad & 0xFF) : 0, first = mirrored ? d->pad >> 8 : d->slice;
  UINT width = mip_extent(format_kw(r->fmt, (UINT)r->desc.Width), d->mip), height = mip_extent(r->desc.Height, d->mip);
  if (r->planes) {
    UINT w = (UINT)r->desc.Width, h = r->desc.Height;
    plane_extent(r->fmt, plane, &w, &h);
    width = w, height = h;
  }
  for (UINT k = 0; k <= (rect_count ? rect_count - 1 : 0); ++k) {
    UINT x0 = 0, y0 = 0, x1 = width, y1 = height;
    if (rect_count) {
      const D3D12_RECT *rc = &rects[k];
      if (rc->right <= rc->left || rc->bottom <= rc->top || rc->right <= 0 || rc->bottom <= 0) continue;
      x0 = rc->left < 0 ? 0 : (UINT)rc->left, y0 = rc->top < 0 ? 0 : (UINT)rc->top;
      x1 = (UINT)rc->right > width ? width : (UINT)rc->right, y1 = (UINT)rc->bottom > height ? height : (UINT)rc->bottom;
      if (x0 >= x1 || y0 >= y1) continue;
    }
    const UINT64 pitch = (UINT64)(x1 - x0) * bytes, span = pitch * (y1 - y0);
    if (span > (64u << 20)) continue;
    const D3D12_HEAP_PROPERTIES props = {D3D12_HEAP_TYPE_UPLOAD, D3D12_CPU_PAGE_PROPERTY_UNKNOWN, D3D12_MEMORY_POOL_UNKNOWN, 1, 1};
    const D3D12_RESOURCE_DESC bd = {D3D12_RESOURCE_DIMENSION_BUFFER, 0, span, 1, 1, 1, DXGI_FORMAT_UNKNOWN, {1, 0}, D3D12_TEXTURE_LAYOUT_ROW_MAJOR, D3D12_RESOURCE_FLAG_NONE};
    Resource *fill = NULL;
    if (FAILED(resource_make(l->d.dev, &props, D3D12_HEAP_FLAG_NONE, &bd, NULL, 0, 0, &fill))) return;
    for (UINT64 at = 0; at < span; at += bytes) memcpy(fill->shadow + at, element, bytes);
    fill->dirty = 1;
    const int kept_ = keep(l, (IUnknown *)fill);
    dx_Release(OBJ(fill));      /* (the list's reference keeps it until the list is reset) */
    if (!kept_) return;
    keep(l, (IUnknown *)r);
    end_pass(l);
    for (UINT layer = 0; layer < d->stride; ++layer) {
      /* (the image of the layer: a plane's slice of its own, the texture's one image, or the view's layer of the resource's image) */
      const UINT image = r->planes ? (first + layer < r->layers ? r->plane_alloc[plane * r->layers + first + layer] : 0) : mirrored ? r->alloc : (UINT)d->size;
      if (!image) continue;
      ensure_room(l, 256, 4);
      copy_buffer_image_packet(l, 0, alloc_index(l, image), mirrored ? 0 : d->mip, r->planes || mirrored ? 0 : first + layer, x0, y0, x1 - x0, y1 - y0, alloc_index(l, fill->alloc), 0, (UINT)pitch, copy_format);
    }
  }
}

static void uav_clear_list(CmdList *l, D3D12_CPU_DESCRIPTOR_HANDLE cpu, ID3D12Resource *resource, const UINT u[4], const float f[4], int floats, UINT rect_count, const D3D12_RECT *rects) {
  const SoftDesc *d = desc_at(cpu);
  Resource *r = (Resource *)resource;
  if (d && d->kind == DK_UAV_TEXTURE && d->alloc && r && r->desc.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER && !l->bundle) {
    uav_clear_texture(l, d, r, u, f, floats, rect_count, rects);
    return;
  }
  if (!d || d->kind != DK_UAV || !d->alloc || !r || r->desc.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER || l->bundle) return;
  BYTE element[16];
  const UINT bytes = uav_clear_element(d->format, u, f, floats, element);
  if (!bytes || !d->size) return;
  /* (rectangles of a buffer view: elements left .. right of row 0, elements of the view's stride) */
  const UINT64 element_bytes = d->stride ? d->stride : bytes;
  UINT64 from = 0, to = d->size;
  for (UINT k = 0; k <= (rect_count ? rect_count - 1 : 0); ++k) {
    if (rect_count) {
      if (rects[k].top > 0 || rects[k].left < 0 || rects[k].right <= rects[k].left) continue;
      from = (UINT64)rects[k].left * element_bytes;
      to = (UINT64)rects[k].right * element_bytes;
      if (to > d->size) to = d->size;
      if (from >= to) continue;
    }
    const UINT64 span = to - from;
    if (span > (64u << 20)) continue;
    const D3D12_HEAP_PROPERTIES props = {D3D12_HEAP_TYPE_UPLOAD, D3D12_CPU_PAGE_PROPERTY_UNKNOWN, D3D12_MEMORY_POOL_UNKNOWN, 1, 1};
    const D3D12_RESOURCE_DESC bd = {D3D12_RESOURCE_DIMENSION_BUFFER, 0, span, 1, 1, 1, DXGI_FORMAT_UNKNOWN, {1, 0}, D3D12_TEXTURE_LAYOUT_ROW_MAJOR, D3D12_RESOURCE_FLAG_NONE};
    Resource *fill = NULL;
    if (FAILED(resource_make(l->d.dev, &props, D3D12_HEAP_FLAG_NONE, &bd, NULL, 0, 0, &fill))) return;
    for (UINT64 at = 0; at < span; at += bytes) memcpy(fill->shadow + at, element, span - at < bytes ? (SIZE_T)(span - at) : bytes);
    fill->dirty = 1;
    const int kept_ = keep(l, (IUnknown *)fill);
    dx_Release(OBJ(fill));      /* (the list's reference keeps it until the list is reset) */
    if (!kept_) return;
    keep(l, (IUnknown *)r);
    end_pass(l);
    ensure_room(l, 256, 4);
    UINT w[5] = {alloc_index(l, d->alloc), (UINT)(d->offset + from), alloc_index(l, fill->alloc), 0, (UINT)span};
    packet(l, OP_COPY_BUFFER, w, 5);
  }
}

/* ---- ID3D12GraphicsCommandList8 .. 10 ------------------------------------------------------------------------------------------------------------------------------- */

/* OMSetFrontAndBackStencilRef: one reference for both faces here (IndependentFrontAndBackStencilRefMaskSupported is FALSE, OPTIONS14: a program gives the two the same
 * value), so the front one is the list's stencil reference. */
static void STDMETHODCALLTYPE list_OMSetFrontAndBackStencilRef(CmdList *l, UINT front, UINT back) {
  (void)back;
  list_OMSetStencilRef(l, front);
}
/* RSSetDepthBias / IASetIndexBufferStripCutValue: the state of a pipeline made with D3D12_PIPELINE_STATE_FLAG_DYNAMIC_DEPTH_BIAS / _DYNAMIC_INDEX_BUFFER_STRIP_CUT, which
 * this device refuses (DynamicDepthBiasSupported, DynamicIndexBufferStripCutSupported are FALSE, OPTIONS16 / 15): calls a program may not make here, left out. */
static void STDMETHODCALLTYPE list_RSSetDepthBias(CmdList *l, FLOAT bias, FLOAT clamp, FLOAT slope) { (void)l; (void)bias; (void)clamp; (void)slope; }
static void STDMETHODCALLTYPE list_IASetIndexBufferStripCutValue(CmdList *l, D3D12_INDEX_BUFFER_STRIP_CUT_VALUE value) { (void)l; (void)value; }
/* SetProgram / DispatchGraph: work graphs (WorkGraphsTier is NOT_SUPPORTED, OPTIONS21: no program can be made to set) - left out. */
static void STDMETHODCALLTYPE list_SetProgram(CmdList *l, const D3D12_SET_PROGRAM_DESC *desc) { (void)l; (void)desc; }
static void STDMETHODCALLTYPE list_DispatchGraph(CmdList *l, const D3D12_DISPATCH_GRAPH_DESC *desc) { (void)l; (void)desc; }
