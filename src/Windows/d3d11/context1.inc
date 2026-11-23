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

/* Part of d3d11.c: what Direct3D 11.1 adds to the context and the device (ID3D11DeviceContext1, ID3D11Device1), and the context state objects
 * (ID3DDeviceContextState) of SwapDeviceContextState. The constant buffer windows of *SetConstantBuffers1 are in context.inc (cb_set). */

/* ---- ID3D11DeviceContext1 ------------------------------------------------------------------------------------------------------------------------------------ */

/* (D3D11_COPY_NO_OVERWRITE and D3D11_COPY_DISCARD promise what the copy may overwrite; the copy is the same) */
static void STDMETHODCALLTYPE ctx_CopySubresourceRegion1(Ctx *c, ID3D11Resource *to, UINT to_sub, UINT x, UINT y, UINT z, ID3D11Resource *from, UINT from_sub, const D3D11_BOX *box,
                                                         UINT flags) {
  if (flags & ~(UINT)(D3D11_COPY_NO_OVERWRITE | D3D11_COPY_DISCARD)) return;
  ctx_CopySubresourceRegion(c, to, to_sub, x, y, z, from, from_sub, box);
}
static void STDMETHODCALLTYPE ctx_UpdateSubresource1(Ctx *c, ID3D11Resource *resource, UINT sub, const D3D11_BOX *box, const void *data, UINT row_pitch, UINT depth_pitch, UINT flags) {
  if (flags & ~(UINT)(D3D11_COPY_NO_OVERWRITE | D3D11_COPY_DISCARD)) return;
  ctx_UpdateSubresource(c, resource, sub, box, data, row_pitch, depth_pitch);
}
/* What a discarded resource or view holds is undefined from then on: keeping it as it was is one of the things it may be. */
static void STDMETHODCALLTYPE ctx_DiscardResource(Ctx *c, ID3D11Resource *resource) { (void)c; (void)resource; }
static void STDMETHODCALLTYPE ctx_DiscardView(Ctx *c, ID3D11View *view) { (void)c; (void)view; }
static void STDMETHODCALLTYPE ctx_DiscardView1(Ctx *c, ID3D11View *view, const D3D11_RECT *rects, UINT count) { (void)c; (void)view; (void)rects; (void)count; }

/* One element of `bytes` bytes in the rectangle (x0, y0) .. (x1, y1) of slice `z` of a subresource. */
static void tex_fill_rect(Dev *d, Tex *t, UINT sub, UINT z, UINT x0, UINT y0, UINT x1, UINT y1, const BYTE *element, UINT bytes) {
  const UINT w = x1 - x0, h = y1 - y0;
  if (!w || !h) return;
  BYTE *fill = mem_alloc((SIZE_T)w * h * bytes);
  if (!fill) return;
  for (SIZE_T at = 0; at < (SIZE_T)w * h * bytes; at += bytes) memcpy(fill + at, element, bytes);
  tex_before_write(d, t);
  tex_write(d, t, sub, x0, y0, z, w, h, fill, w * bytes);
  mem_free(fill);
}
/* ClearView: a render target or unordered access view cleared with a colour (converted to the view's format), all of it or the rectangles given (clipped to
 * the view; of a buffer, a rectangle's left and right are elements). */
static void STDMETHODCALLTYPE ctx_ClearView(Ctx *c, ID3D11View *view, const FLOAT colour[4], const D3D11_RECT *rects, UINT count) {
  extern const ID3D11UnorderedAccessViewVtbl uav_vtbl;
  View *v = (View *)view;
  if (!v || !colour || (count && !rects)) return;
  if (c->deferred) { deferred_record(c, &(RecArgs){.op = REC_CLEAR_VIEW, .obj = {view}, .f = {colour[0], colour[1], colour[2], colour[3]}, .rects = rects, .rect_count = count}); return; }
  const BOOL target = ((DxObj *)v)->lpVtbl == (const void *)&rtv_vtbl, unordered = ((DxObj *)v)->lpVtbl == (const void *)&uav_vtbl;
  if (!target && !unordered) return;      /* (a depth-stencil view is cleared with ClearDepthStencilView) */
  if (!count) {
    if (target) ctx_ClearRenderTargetView(c, (ID3D11RenderTargetView *)v, colour);
    else ctx_ClearUnorderedAccessViewFloat(c, (ID3D11UnorderedAccessView *)v, colour);
    return;
  }
  if (predicated_out(c->dev)) return;
  static const UINT none[4] = {0, 0, 0, 0};
  BYTE element[16];
  LOCK(c);
  if (v->buf) {
    const DXGI_FORMAT format = v->format != DXGI_FORMAT_UNKNOWN ? v->format : DXGI_FORMAT_R32_TYPELESS;
    const UINT bytes = clear_element(format, none, colour, TRUE, element), elements = v->stride ? (UINT)(v->size / v->stride) : 0;
    for (UINT r = 0; bytes && r < count; ++r) {
      const UINT left = rects[r].left < 0 ? 0 : (UINT)rects[r].left, right = rects[r].right < 0 ? 0 : (UINT)rects[r].right > elements ? elements : (UINT)rects[r].right;
      if (left >= right) continue;
      const UINT64 span = (UINT64)(right - left) * v->stride;
      BYTE *fill = mem_alloc((SIZE_T)span);
      if (!fill) break;
      for (UINT64 at = 0; at + bytes <= span; at += bytes) memcpy(fill + at, element, bytes);
      buf_write_any(c->dev, v->buf, v->offset + (UINT64)left * v->stride, fill, span);
      mem_free(fill);
    }
  } else {
    Tex *t = v->tex;
    UINT level = 0;
    DXGI_FORMAT format = t->desc.Format;
    if (target) { if (v->desc.rtv.Format != DXGI_FORMAT_UNKNOWN) format = v->desc.rtv.Format; if (v->desc.rtv.ViewDimension == D3D11_RTV_DIMENSION_TEXTURE2D) level = v->desc.rtv.Texture2D.MipSlice; }
    else { if (v->desc.uav.Format != DXGI_FORMAT_UNKNOWN) format = v->desc.uav.Format; if (v->desc.uav.ViewDimension == D3D11_UAV_DIMENSION_TEXTURE2D) level = v->desc.uav.Texture2D.MipSlice; }
    const UINT bytes = clear_element(format, none, colour, TRUE, element);
    const UINT w = mip_size(t->desc.Width, level), h = mip_size(t->desc.Height, level);
    for (UINT r = 0; bytes == t->bpp && !t->depth && r < count; ++r) {
      const LONG x0 = rects[r].left < 0 ? 0 : rects[r].left, y0 = rects[r].top < 0 ? 0 : rects[r].top;
      const LONG x1 = rects[r].right > (LONG)w ? (LONG)w : rects[r].right, y1 = rects[r].bottom > (LONG)h ? (LONG)h : rects[r].bottom;
      if (x0 >= x1 || y0 >= y1) continue;
      tex_fill_rect(c->dev, t, level, 0, (UINT)x0, (UINT)y0, (UINT)x1, (UINT)y1, element, bytes);
    }
  }
  UNLOCK(c);
}

/* ---- context state objects ---------------------------------------------------------------------------------------------------------------------------------------- */

/* A context's bindings kept apart (CreateDeviceContextState), and the device interface it was made to work like. SwapDeviceContextState puts the context's
 * bindings into the state that is active on it and takes on those of the new one. */
typedef struct CtxState {
  Child c;
  StateCopy *state;            /* NULL: the default bindings */
  D3D_FEATURE_LEVEL level;
  GUID emulated;
} CtxState;

static void cstate_destroy(DxObj *o) {
  state_release(((CtxState *)o)->state);
  child_destroy(o);
}
static CtxState *cstate_new(Dev *d, D3D_FEATURE_LEVEL level, const GUID *emulated) {
  extern const ID3DDeviceContextStateVtbl cstate_vtbl;
  static const GUID *const iids[] = {&IID_ID3D11DeviceChild, &IID_ID3DDeviceContextState, NULL};
  CtxState *s = child_new(sizeof *s, d, &cstate_vtbl, iids, cstate_destroy);
  if (!s) return NULL;
  s->level = level;
  s->emulated = *emulated;
  return s;
}
static BOOL emulated_interface_valid(REFIID riid) {
  return guid_eq(riid, &IID_ID3D10Device) || guid_eq(riid, &IID_ID3D10Device1) || guid_eq(riid, &IID_ID3D11Device) || guid_eq(riid, &IID_ID3D11Device1);
}
static HRESULT STDMETHODCALLTYPE dev_CreateDeviceContextState(Dev *d, UINT flags, const D3D_FEATURE_LEVEL *levels, UINT count, UINT sdk, REFIID emulated, D3D_FEATURE_LEVEL *chosen,
                                                              ID3DDeviceContextState **out) {
  (void)sdk;
  if (out) *out = NULL;
  if (chosen) *chosen = 0;
  if ((flags & ~(UINT)D3D11_1_CREATE_DEVICE_CONTEXT_STATE_SINGLETHREADED) || !levels || !count || !emulated || !emulated_interface_valid(emulated)) return E_INVALIDARG;
  UINT i = 0;
  while (i < count && (levels[i] > d->level || levels[i] < D3D_FEATURE_LEVEL_9_1)) ++i;      /* (the first the device has) */
  if (i == count) return E_INVALIDARG;
  if (chosen) *chosen = levels[i];
  if (!out) return S_FALSE;
  CtxState *s = cstate_new(d, levels[i], emulated);
  if (!s) return E_OUTOFMEMORY;
  *out = (ID3DDeviceContextState *)s;
  return S_OK;
}
static CtxState *cstate_of(ID3DDeviceContextState *p) {
  extern const ID3DDeviceContextStateVtbl cstate_vtbl;
  return p && ((DxObj *)p)->lpVtbl == (const void *)&cstate_vtbl ? (CtxState *)p : NULL;
}
static void STDMETHODCALLTYPE ctx_SwapDeviceContextState(Ctx *c, ID3DDeviceContextState *state, ID3DDeviceContextState **previous) {
  CtxState *s = cstate_of(state);
  if (previous) *previous = NULL;
  if (c->deferred) return;      /* (the immediate context's alone) */
  LOCK(c);
  /* (the state active until now: the device's own, the first time) */
  CtxState *old = c->active;
  if (!old) old = cstate_new(c->dev, c->dev->level, &IID_ID3D11Device1);
  else dx_AddRef(OBJ(old));
  if (old) {
    state_release(old->state);
    old->state = state_of(c);
  }
  if (s) {
    if (s->state) state_apply(c, s->state);
    else { ctx_unbind_all(c); memset((BYTE *)c + CTX_VALUES_FIRST, 0, CTX_VALUES_END - CTX_VALUES_FIRST); c->sample_mask = 0xFFFFFFFFu; for (UINT i = 0; i < 4; ++i) c->blend_factor[i] = 1.0f; }
    c->dev->dirty = DIRTY_ALL;
  }
  bind(&c->active, s ? s : old);
  UNLOCK(c);
  if (previous) *previous = (ID3DDeviceContextState *)old;
  else if (old) dx_Release(OBJ(old));
}

/* ---- ID3D11Device1 ------------------------------------------------------------------------------------------------------------------------------------------------ */

static void STDMETHODCALLTYPE dev_GetImmediateContext1(Dev *d, ID3D11DeviceContext1 **out) {
  if (!out) return;
  ctx_AddRef(d->ctx);
  *out = (ID3D11DeviceContext1 *)d->ctx;
}
static HRESULT STDMETHODCALLTYPE dev_CreateDeferredContext1(Dev *d, UINT flags, ID3D11DeviceContext1 **out) { return dev_CreateDeferredContext(d, flags, (ID3D11DeviceContext **)out); }
/* Shared resources: no resource of this device is shared (IDXGIResource::GetSharedHandle answers DXGI_ERROR_UNSUPPORTED), so no handle names one. */
static HRESULT STDMETHODCALLTYPE dev_OpenSharedResource(Dev *d, HANDLE handle, REFIID riid, void **out) {
  (void)d; (void)handle; (void)riid;
  if (out) *out = NULL;
  return E_INVALIDARG;
}
static HRESULT STDMETHODCALLTYPE dev_OpenSharedResource1(Dev *d, HANDLE handle, REFIID riid, void **out) { return dev_OpenSharedResource(d, handle, riid, out); }
static HRESULT STDMETHODCALLTYPE dev_OpenSharedResourceByName(Dev *d, LPCWSTR name, DWORD access, REFIID riid, void **out) {
  (void)d; (void)access; (void)riid;
  if (out) *out = NULL;
  return E_INVALIDARG;
}

/* ---- ID3D11Multithread and ID3DUserDefinedAnnotation -------------------------------------------------------------------------------------------------------------- */

static Ctx *ctx_of_multithread(ID3D11Multithread *p) { return (Ctx *)((BYTE *)p - offsetof(Ctx, multithread)); }
static Ctx *ctx_of_annotation(ID3DUserDefinedAnnotation *p) { return (Ctx *)((BYTE *)p - offsetof(Ctx, annotation)); }
static HRESULT STDMETHODCALLTYPE ctx_QueryInterface(Ctx *c, REFIID riid, void **out);
static HRESULT STDMETHODCALLTYPE mt_QueryInterface(ID3D11Multithread *p, REFIID riid, void **out) { return ctx_QueryInterface(ctx_of_multithread(p), riid, out); }
static ULONG STDMETHODCALLTYPE mt_AddRef(ID3D11Multithread *p) { return ctx_AddRef(ctx_of_multithread(p)); }
static ULONG STDMETHODCALLTYPE mt_Release(ID3D11Multithread *p) { return ctx_Release(ctx_of_multithread(p)); }
/* Enter and Leave: the device's lock, which the context's calls take too. */
static void STDMETHODCALLTYPE mt_Enter(ID3D11Multithread *p) { EnterCriticalSection(&ctx_of_multithread(p)->dev->lock); }
static void STDMETHODCALLTYPE mt_Leave(ID3D11Multithread *p) { LeaveCriticalSection(&ctx_of_multithread(p)->dev->lock); }
static BOOL STDMETHODCALLTYPE mt_SetMultithreadProtected(ID3D11Multithread *p, BOOL on) {
  Ctx *c = ctx_of_multithread(p);
  const BOOL was = c->protected_;
  c->protected_ = !!on;
  return was;
}
static BOOL STDMETHODCALLTYPE mt_GetMultithreadProtected(ID3D11Multithread *p) { return ctx_of_multithread(p)->protected_; }
static const ID3D11MultithreadVtbl g_mt_vtbl = {mt_QueryInterface, mt_AddRef, mt_Release, mt_Enter, mt_Leave, mt_SetMultithreadProtected, mt_GetMultithreadProtected};

/* The events and markers of a profiler: with no profiler attached they are not kept, and BeginEvent and EndEvent answer -1 (as Windows does then). */
static HRESULT STDMETHODCALLTYPE an_QueryInterface(ID3DUserDefinedAnnotation *p, REFIID riid, void **out) { return ctx_QueryInterface(ctx_of_annotation(p), riid, out); }
static ULONG STDMETHODCALLTYPE an_AddRef(ID3DUserDefinedAnnotation *p) { return ctx_AddRef(ctx_of_annotation(p)); }
static ULONG STDMETHODCALLTYPE an_Release(ID3DUserDefinedAnnotation *p) { return ctx_Release(ctx_of_annotation(p)); }
static INT STDMETHODCALLTYPE an_BeginEvent(ID3DUserDefinedAnnotation *p, LPCWSTR name) { (void)p; (void)name; return -1; }
static INT STDMETHODCALLTYPE an_EndEvent(ID3DUserDefinedAnnotation *p) { (void)p; return -1; }
static void STDMETHODCALLTYPE an_SetMarker(ID3DUserDefinedAnnotation *p, LPCWSTR name) { (void)p; (void)name; }
static BOOL STDMETHODCALLTYPE an_GetStatus(ID3DUserDefinedAnnotation *p) { (void)p; return FALSE; }
static const ID3DUserDefinedAnnotationVtbl g_annotation_vtbl = {an_QueryInterface, an_AddRef, an_Release, an_BeginEvent, an_EndEvent, an_SetMarker, an_GetStatus};

static void ctx_init_faces(Ctx *c) {
  c->multithread.lpVtbl = &g_mt_vtbl;
  c->annotation.lpVtbl = &g_annotation_vtbl;
}
