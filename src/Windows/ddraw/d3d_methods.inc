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
/* The methods of IDirect3DDevice7, IDirect3DDevice3, IDirect3DDevice2 and IDirect3DDevice (d3d_device.inc has what they share). Every method runs
 * under DirectDraw's lock; Direct3D 7's set the FPU up for the call when the device was made with DDSCL_FPUPRESERVE (as Windows; Wine keeps two
 * vtables for it). */

static HRESULT d3_execute(D3dDevice *dev, D3dExecuteBuffer *eb);
static D3dVertexBuffer *vb_from(const void *iface);
static HRESULT texture_load(Surface *dst, Surface *src);

#define D7_BEGIN(i)                                                     \
  D3dDevice *dev = DEV7(i);                                             \
  dd_lock();                                                            \
  const BOOL fpu_keep_ = dev->fpu_preserve;                             \
  const WORD fpu_cw_ = fpu_keep_ ? fpu_setup() : 0;                     \
  HRESULT hr; (void)dev
#define D7_END()                                                        \
  if (fpu_keep_) fpu_set(fpu_cw_);                                      \
  dd_unlock();                                                          \
  return hr
#define DL_BEGIN(D, i) D3dDevice *dev = D(i); dd_lock(); HRESULT hr; (void)dev
#define DL_END() dd_unlock(); return hr

/* ---- IUnknown ------------------------------------------------------------------------------------------------------------------------------------- */

static HRESULT STDMETHODCALLTYPE dev7_QueryInterface(IDirect3DDevice7 *i, REFIID riid, void **out) { return IUnknown_QueryInterface(DEV7(i)->outer, riid, out); }
static ULONG STDMETHODCALLTYPE dev7_AddRef(IDirect3DDevice7 *i) { return IUnknown_AddRef(DEV7(i)->outer); }
static ULONG STDMETHODCALLTYPE dev7_Release(IDirect3DDevice7 *i) { return IUnknown_Release(DEV7(i)->outer); }
static HRESULT STDMETHODCALLTYPE dev3_QueryInterface(IDirect3DDevice3 *i, REFIID riid, void **out) { return IUnknown_QueryInterface(DEV3(i)->outer, riid, out); }
static ULONG STDMETHODCALLTYPE dev3_AddRef(IDirect3DDevice3 *i) { return IUnknown_AddRef(DEV3(i)->outer); }
static ULONG STDMETHODCALLTYPE dev3_Release(IDirect3DDevice3 *i) { return IUnknown_Release(DEV3(i)->outer); }
static HRESULT STDMETHODCALLTYPE dev2_QueryInterface(IDirect3DDevice2 *i, REFIID riid, void **out) { return IUnknown_QueryInterface(DEV2(i)->outer, riid, out); }
static ULONG STDMETHODCALLTYPE dev2_AddRef(IDirect3DDevice2 *i) { return IUnknown_AddRef(DEV2(i)->outer); }
static ULONG STDMETHODCALLTYPE dev2_Release(IDirect3DDevice2 *i) { return IUnknown_Release(DEV2(i)->outer); }
static HRESULT STDMETHODCALLTYPE dev1_QueryInterface(IDirect3DDevice *i, REFIID riid, void **out) { return IUnknown_QueryInterface(DEV1(i)->outer, riid, out); }
static ULONG STDMETHODCALLTYPE dev1_AddRef(IDirect3DDevice *i) { return IUnknown_AddRef(DEV1(i)->outer); }
static ULONG STDMETHODCALLTYPE dev1_Release(IDirect3DDevice *i) { return IUnknown_Release(DEV1(i)->outer); }

/* ---- IDirect3DDevice7 ------------------------------------------------------------------------------------------------------------------------------ */

static HRESULT STDMETHODCALLTYPE dev7_GetCaps(IDirect3DDevice7 *i, D3DDEVICEDESC7 *desc) {
  D7_BEGIN(i);
  if (!desc) hr = DDERR_INVALIDPARAMS;
  else if (!dev->dd3) hr = DDERR_GENERIC;
  else { dl_device_desc7(&dev->dd3->caps, dev->kind, (DlDeviceDesc7 *)desc); hr = D3D_OK; }
  D7_END();
}
static HRESULT STDMETHODCALLTYPE dev7_EnumTextureFormats(IDirect3DDevice7 *i, LPD3DENUMPIXELFORMATSCALLBACK cb, void *ctx) {
  D7_BEGIN(i);
  hr = dv_enum_texture_formats(dev, (void *)cb, ctx, FALSE);
  D7_END();
}
static HRESULT STDMETHODCALLTYPE dev7_BeginScene(IDirect3DDevice7 *i) { D7_BEGIN(i); hr = dv_begin_scene(dev); D7_END(); }
static HRESULT STDMETHODCALLTYPE dev7_EndScene(IDirect3DDevice7 *i) { D7_BEGIN(i); hr = dv_end_scene(dev); D7_END(); }
static HRESULT STDMETHODCALLTYPE dev7_GetDirect3D(IDirect3DDevice7 *i, IDirect3D7 **out) { D7_BEGIN(i); hr = dv_get_direct3d(dev, 7, (void **)out); D7_END(); }
static HRESULT STDMETHODCALLTYPE dev7_SetRenderTarget(IDirect3DDevice7 *i, IDirectDrawSurface7 *target, DWORD flags) {
  (void)flags;
  D7_BEGIN(i);
  hr = dv_set_render_target(dev, surface_from_any(target), (IUnknown *)target, 7);
  D7_END();
}
static HRESULT STDMETHODCALLTYPE dev7_GetRenderTarget(IDirect3DDevice7 *i, IDirectDrawSurface7 **out) {
  D7_BEGIN(i);
  hr = dv_get_render_target(dev, &IID_IDirectDrawSurface7, (void **)out);
  D7_END();
}
static HRESULT STDMETHODCALLTYPE dev7_Clear(IDirect3DDevice7 *i, DWORD count, D3DRECT *rects, DWORD flags, D3DCOLOR color, D3DVALUE z, DWORD stencil) {
  D7_BEGIN(i);
  if (!dev->dd3) hr = DDERR_GENERIC;
  else hr = d3_clear(dev, count, rects, flags, color, z, stencil, &dev->st.viewport);
  D7_END();
}
static HRESULT STDMETHODCALLTYPE dev7_SetTransform(IDirect3DDevice7 *i, D3DTRANSFORMSTATETYPE state, D3DMATRIX *m) { D7_BEGIN(i); hr = dv_set_transform(dev, state, m); D7_END(); }
static HRESULT STDMETHODCALLTYPE dev7_GetTransform(IDirect3DDevice7 *i, D3DTRANSFORMSTATETYPE state, D3DMATRIX *m) { D7_BEGIN(i); hr = dv_get_transform(dev, state, m); D7_END(); }
static HRESULT STDMETHODCALLTYPE dev7_MultiplyTransform(IDirect3DDevice7 *i, D3DTRANSFORMSTATETYPE state, D3DMATRIX *m) { D7_BEGIN(i); hr = dv_multiply_transform(dev, state, m); D7_END(); }
static HRESULT STDMETHODCALLTYPE dev7_SetViewport(IDirect3DDevice7 *i, D3DVIEWPORT7 *vp) {
  D7_BEGIN(i);
  if (!vp) hr = DDERR_INVALIDPARAMS;
  else if (!dev->target) hr = DDERR_INVALIDCAPS;
  else if (vp->dwX > dev->target->desc.dwWidth || vp->dwWidth > dev->target->desc.dwWidth - vp->dwX || vp->dwY > dev->target->desc.dwHeight ||
           vp->dwHeight > dev->target->desc.dwHeight - vp->dwY)
    hr = E_INVALIDARG;
  else { d3_set_viewport(dev, vp); hr = D3D_OK; }
  D7_END();
}
static HRESULT STDMETHODCALLTYPE dev7_GetViewport(IDirect3DDevice7 *i, D3DVIEWPORT7 *vp) {
  D7_BEGIN(i);
  if (!vp) hr = DDERR_INVALIDPARAMS;
  else { *vp = dev->st.viewport; hr = D3D_OK; }
  D7_END();
}
static HRESULT STDMETHODCALLTYPE dev7_SetMaterial(IDirect3DDevice7 *i, D3DMATERIAL7 *m) {
  D7_BEGIN(i);
  if (!m) hr = DDERR_INVALIDPARAMS;
  else { d3_set_material(dev, m); hr = D3D_OK; }
  D7_END();
}
static HRESULT STDMETHODCALLTYPE dev7_GetMaterial(IDirect3DDevice7 *i, D3DMATERIAL7 *m) {
  D7_BEGIN(i);
  if (!m) hr = DDERR_INVALIDPARAMS;
  else { *m = dev->st.material; hr = D3D_OK; }
  D7_END();
}
static HRESULT STDMETHODCALLTYPE dev7_SetLight(IDirect3DDevice7 *i, DWORD index, D3DLIGHT7 *light) {
  D7_BEGIN(i);
  if (!light) hr = DDERR_INVALIDPARAMS;
  else if (light->dltType < D3DLIGHT_POINT || light->dltType > D3DLIGHT_DIRECTIONAL) hr = DDERR_INVALIDPARAMS;
  else if (light->dvRange < 0.0f) hr = DDERR_INVALIDPARAMS;
  /* (a negative attenuation of a light that has a position: Wine's d3d test records DDERR_INVALIDPARAMS) */
  else if (light->dltType != D3DLIGHT_DIRECTIONAL && (light->dvAttenuation0 < 0.0f || light->dvAttenuation1 < 0.0f || light->dvAttenuation2 < 0.0f))
    hr = DDERR_INVALIDPARAMS;
  else hr = d3_light(dev, index, light, -1);
  D7_END();
}
static HRESULT STDMETHODCALLTYPE dev7_GetLight(IDirect3DDevice7 *i, DWORD index, D3DLIGHT7 *light) {
  D7_BEGIN(i);
  if (!light) hr = DDERR_INVALIDPARAMS;
  else if (index >= dev->st.light_count || !dev->st.lights[index].set) hr = DDERR_INVALIDPARAMS;
  else { *light = dev->st.lights[index].light; hr = D3D_OK; }
  D7_END();
}
static HRESULT STDMETHODCALLTYPE dev7_SetRenderState(IDirect3DDevice7 *i, D3DRENDERSTATETYPE state, DWORD value) {
  D7_BEGIN(i);
  if (state >= D3DRENDERSTATE_STIPPLEPATTERN00 && state < D3DRENDERSTATE_STIPPLEPATTERN00 + 32) { d3_set_rs(dev, state, value); hr = D3D_OK; }
  else hr = dv_set_render_state(dev, state, value);
  D7_END();
}
static HRESULT STDMETHODCALLTYPE dev7_GetRenderState(IDirect3DDevice7 *i, D3DRENDERSTATETYPE state, DWORD *value) { D7_BEGIN(i); hr = dv_get_render_state(dev, state, value); D7_END(); }
static HRESULT STDMETHODCALLTYPE dev7_BeginStateBlock(IDirect3DDevice7 *i) {
  D7_BEGIN(i);
  if (dev->recording) hr = D3DERR_INBEGINSTATEBLOCK;
  else if (!(dev->recording = block_new())) hr = DDERR_OUTOFMEMORY;
  else hr = D3D_OK;
  D7_END();
}
static HRESULT STDMETHODCALLTYPE dev7_EndStateBlock(IDirect3DDevice7 *i, DWORD *out) {
  D7_BEGIN(i);
  if (!out) hr = DDERR_INVALIDPARAMS;
  else if (!dev->recording) hr = D3DERR_NOTINBEGINSTATEBLOCK;
  else {
    D3dStateBlock *b = dev->recording;
    dev->recording = NULL;
    const DWORD h = handle_alloc(&dev->blocks, H_STATEBLOCK, b, dev);
    if (!h) { block_free(b); *out = 0; hr = DDERR_OUTOFMEMORY; }
    else { *out = h; hr = D3D_OK; }
  }
  D7_END();
}
static HRESULT STDMETHODCALLTYPE dev7_PreLoad(IDirect3DDevice7 *i, IDirectDrawSurface7 *texture) {
  D7_BEGIN(i);
  Surface *s = surface_from_any(texture);
  if (!s) hr = DDERR_INVALIDPARAMS;
  else if (!(s->desc.ddsCaps.dwCaps2 & (DDSCAPS2_TEXTUREMANAGE | DDSCAPS2_D3DTEXTUREMANAGE))) hr = DDERR_INVALIDPARAMS;
  else {
    /* (a managed texture's copy is made and sent now) */
    hr = D3D_OK;
    if (dev->dd3 && dev->dd3->dev) texture_res(dev->dd3, texture_root(s), dev->st.rs[D3DRENDERSTATE_COLORKEYENABLE] != 0);
  }
  D7_END();
}
static HRESULT STDMETHODCALLTYPE dev7_DrawPrimitive(IDirect3DDevice7 *i, D3DPRIMITIVETYPE type, DWORD fvf, void *v, DWORD count, DWORD flags) {
  D7_BEGIN(i);
  if (!dev->dd3) hr = DDERR_GENERIC;
  else hr = d3_draw(dev, type, fvf, v, count, NULL, 0, flags);
  D7_END();
}
static HRESULT STDMETHODCALLTYPE dev7_DrawIndexedPrimitive(IDirect3DDevice7 *i, D3DPRIMITIVETYPE type, DWORD fvf, void *v, DWORD count, WORD *idx, DWORD icount, DWORD flags) {
  D7_BEGIN(i);
  if (!dev->dd3) hr = DDERR_GENERIC;
  else if (!count || !icount) hr = D3D_OK;
  else if (!idx) hr = DDERR_INVALIDPARAMS;
  else hr = d3_draw(dev, type, fvf, v, count, idx, icount, flags);
  D7_END();
}
static HRESULT STDMETHODCALLTYPE dev7_SetClipStatus(IDirect3DDevice7 *i, D3DCLIPSTATUS *cs) { D7_BEGIN(i); hr = dv_set_clip_status(dev, cs); D7_END(); }
static HRESULT STDMETHODCALLTYPE dev7_GetClipStatus(IDirect3DDevice7 *i, D3DCLIPSTATUS *cs) { D7_BEGIN(i); hr = dv_get_clip_status(dev, cs); D7_END(); }
static HRESULT STDMETHODCALLTYPE dev7_DrawPrimitiveStrided(IDirect3DDevice7 *i, D3DPRIMITIVETYPE type, DWORD fvf, D3DDRAWPRIMITIVESTRIDEDDATA *data, DWORD count, DWORD flags) {
  D7_BEGIN(i);
  hr = dv_draw_strided(dev, type, fvf, data, count, NULL, 0, flags);
  D7_END();
}
static HRESULT STDMETHODCALLTYPE dev7_DrawIndexedPrimitiveStrided(IDirect3DDevice7 *i, D3DPRIMITIVETYPE type, DWORD fvf, D3DDRAWPRIMITIVESTRIDEDDATA *data, DWORD count,
                                                                  WORD *idx, DWORD icount, DWORD flags) {
  D7_BEGIN(i);
  if (!count || !icount) hr = D3D_OK;
  else if (!idx) hr = DDERR_INVALIDPARAMS;
  else hr = dv_draw_strided(dev, type, fvf, data, count, idx, icount, flags);
  D7_END();
}
static HRESULT STDMETHODCALLTYPE dev7_DrawPrimitiveVB(IDirect3DDevice7 *i, D3DPRIMITIVETYPE type, IDirect3DVertexBuffer7 *vb, DWORD start, DWORD count, DWORD flags) {
  D7_BEGIN(i);
  hr = dv_draw_vb(dev, type, vb_from(vb), start, count, NULL, 0, flags);
  D7_END();
}
static HRESULT STDMETHODCALLTYPE dev7_DrawIndexedPrimitiveVB(IDirect3DDevice7 *i, D3DPRIMITIVETYPE type, IDirect3DVertexBuffer7 *vb, DWORD start, DWORD count, WORD *idx,
                                                             DWORD icount, DWORD flags) {
  D7_BEGIN(i);
  if (!icount) hr = D3D_OK;
  else if (!idx) hr = DDERR_INVALIDPARAMS;
  else hr = dv_draw_vb(dev, type, vb_from(vb), start, count, idx, icount, flags);
  D7_END();
}
static HRESULT STDMETHODCALLTYPE dev7_ComputeSphereVisibility(IDirect3DDevice7 *i, D3DVECTOR *centers, D3DVALUE *radii, DWORD count, DWORD flags, DWORD *out) {
  D7_BEGIN(i);
  hr = dv_sphere_visibility(dev, centers, radii, count, flags, out);
  D7_END();
}
static HRESULT STDMETHODCALLTYPE dev7_GetTexture(IDirect3DDevice7 *i, DWORD stage, IDirectDrawSurface7 **out) {
  D7_BEGIN(i);
  if (!out) hr = DDERR_INVALIDPARAMS;
  else {
    hr = D3D_OK;
    *out = NULL;
    Surface *s = stage < D3D_STAGES ? dev->st.texture[stage] : NULL;
    if (s) { s7_AddRef(IFACE7(s)); *out = IFACE7(s); }
  }
  D7_END();
}
static HRESULT STDMETHODCALLTYPE dev7_SetTexture(IDirect3DDevice7 *i, DWORD stage, IDirectDrawSurface7 *texture) {
  D7_BEGIN(i);
  Surface *s = surface_from_any(texture);
  if (texture && !s) hr = DDERR_INVALIDPARAMS;
  else hr = dv_set_texture(dev, stage, s);
  D7_END();
}
static HRESULT STDMETHODCALLTYPE dev7_GetTextureStageState(IDirect3DDevice7 *i, DWORD stage, D3DTEXTURESTAGESTATETYPE state, DWORD *value) {
  D7_BEGIN(i);
  hr = dv_get_tss(dev, stage, state, value);
  D7_END();
}
static HRESULT STDMETHODCALLTYPE dev7_SetTextureStageState(IDirect3DDevice7 *i, DWORD stage, D3DTEXTURESTAGESTATETYPE state, DWORD value) {
  D7_BEGIN(i);
  hr = dv_set_tss(dev, stage, state, value);
  D7_END();
}
static HRESULT STDMETHODCALLTYPE dev7_ValidateDevice(IDirect3DDevice7 *i, DWORD *passes) { D7_BEGIN(i); hr = dv_validate(dev, passes); D7_END(); }
static HRESULT STDMETHODCALLTYPE dev7_ApplyStateBlock(IDirect3DDevice7 *i, DWORD handle) {
  D7_BEGIN(i);
  D3dStateBlock *b = handle_get(&dev->blocks, handle, H_STATEBLOCK);
  if (dev->recording) hr = D3DERR_INBEGINSTATEBLOCK;
  else if (!b) hr = D3DERR_INVALIDSTATEBLOCK;
  else { block_apply(b, dev); hr = D3D_OK; }
  D7_END();
}
static HRESULT STDMETHODCALLTYPE dev7_CaptureStateBlock(IDirect3DDevice7 *i, DWORD handle) {
  D7_BEGIN(i);
  D3dStateBlock *b = handle_get(&dev->blocks, handle, H_STATEBLOCK);
  if (dev->recording) hr = D3DERR_INBEGINSTATEBLOCK;
  else if (!b) hr = D3DERR_INVALIDSTATEBLOCK;
  else { block_capture(b, dev); hr = D3D_OK; }
  D7_END();
}
static HRESULT STDMETHODCALLTYPE dev7_DeleteStateBlock(IDirect3DDevice7 *i, DWORD handle) {
  D7_BEGIN(i);
  if (dev->recording) hr = D3DERR_INBEGINSTATEBLOCK;
  else {
    D3dStateBlock *b = handle_free(&dev->blocks, handle, H_STATEBLOCK);
    if (!b) hr = D3DERR_INVALIDSTATEBLOCK;
    else { block_free(b); hr = D3D_OK; }
  }
  D7_END();
}
static HRESULT STDMETHODCALLTYPE dev7_CreateStateBlock(IDirect3DDevice7 *i, D3DSTATEBLOCKTYPE type, DWORD *out) {
  D7_BEGIN(i);
  if (!out) hr = DDERR_INVALIDPARAMS;
  else if (dev->recording) hr = D3DERR_INBEGINSTATEBLOCK;
  else if (type != D3DSBT_ALL && type != D3DSBT_PIXELSTATE && type != D3DSBT_VERTEXSTATE) hr = DDERR_INVALIDPARAMS;
  else {
    D3dStateBlock *b = block_new();
    if (!b) hr = DDERR_OUTOFMEMORY;
    else {
      block_mask_type(&b->mask, type, dev);
      block_capture(b, dev);
      const DWORD h = handle_alloc(&dev->blocks, H_STATEBLOCK, b, dev);
      if (!h) { block_free(b); hr = DDERR_OUTOFMEMORY; }
      else { *out = h; hr = D3D_OK; }
    }
  }
  D7_END();
}

/* IDirect3DDevice7::Load: a rectangle of a texture (every level of the destination that has a level of the same size in the source, the faces
 * `flags` names of a cube map) into another, with its palette's entries and its colour keys (Wine's checks, which its tests hold against Windows). */
static Surface *next_level(Surface *s) {
  Surface *root = texture_root(s);
  SurfaceGroup *g = s->group;
  for (UINT i = 0; g && i < g->count; ++i)
    if (g->members[i]->face == s->face && g->members[i]->level == s->level + 1 && g->members[i] != root) return g->members[i];
  return NULL;
}
static Surface *cube_face(Surface *root, UINT face) {
  SurfaceGroup *g = root->group;
  for (UINT i = 0; g && i < g->count; ++i)
    if (g->members[i]->face == face && g->members[i]->level == root->level) return g->members[i];
  return NULL;
}
static BOOL level_subset(Surface *dst, Surface *src) {
  BOOL found = FALSE;
  Surface *s = src, *d = dst;
  while (s && d) {
    if (s->desc.dwWidth == d->desc.dwWidth && s->desc.dwHeight == d->desc.dwHeight) { found = TRUE; d = next_level(d); }
    s = next_level(s);
  }
  return !d && found;
}
static void load_chain(Surface *dst, Surface *src, POINT at, RECT sr) {
  if (src->palette && dst->palette) {
    memcpy(dst->palette->entries, src->palette->entries, sizeof dst->palette->entries);
    surfaces_palette_changed(dst->palette);
  }
  /* (IDirect3DDevice7::Load takes the source's colour keys: one the source has not is deleted from the destination - Wine's test_texture_load_ckey;
   * IDirect3DTexture::Load keeps it) */
  if (src->desc.dwFlags & DDSD_CKSRCBLT) { dst->desc.dwFlags |= DDSD_CKSRCBLT; dst->desc.ddckCKSrcBlt = src->desc.ddckCKSrcBlt; }
  else dst->desc.dwFlags &= ~(DWORD)DDSD_CKSRCBLT;
  if (src->desc.dwFlags & DDSD_CKDESTBLT) { dst->desc.dwFlags |= DDSD_CKDESTBLT; dst->desc.ddckCKDestBlt = src->desc.ddckCKDestBlt; }
  else dst->desc.dwFlags &= ~(DWORD)DDSD_CKDESTBLT;
  Surface *s = src, *d = dst;
  while (s && d) {
    if (s->desc.dwWidth == d->desc.dwWidth && s->desc.dwHeight == d->desc.dwHeight) {
      const RECT dr = {at.x, at.y, at.x + (sr.right - sr.left), at.y + (sr.bottom - sr.top)};
      blit_rect(d, &dr, s, &sr, NULL);
      d = next_level(d);
    }
    s = next_level(s);
    at.x /= 2;
    at.y /= 2;
    sr.left /= 2;
    sr.top /= 2;
    sr.right = (sr.right + 1) / 2;
    sr.bottom = (sr.bottom + 1) / 2;
  }
}
static HRESULT STDMETHODCALLTYPE dev7_Load(IDirect3DDevice7 *i, IDirectDrawSurface7 *dst_texture, POINT *dst_pos, IDirectDrawSurface7 *src_texture, RECT *src_rect,
                                           DWORD flags) {
  D7_BEGIN(i);
  Surface *dst = surface_from_any(dst_texture), *src = surface_from_any(src_texture);
  if (!dst || !src) { hr = DDERR_INVALIDPARAMS; D7_END(); }
  RECT sr = {0, 0, (LONG)src->desc.dwWidth, (LONG)src->desc.dwHeight};
  if (src_rect) sr = *src_rect;
  POINT at = {0, 0};
  if (dst_pos) at = *dst_pos;
  if (rect_empty(&sr) || sr.left < 0 || sr.top < 0 || sr.right > (LONG)src->desc.dwWidth || sr.bottom > (LONG)src->desc.dwHeight || at.x < 0 || at.y < 0 ||
      at.x + sr.right - sr.left > (LONG)src->desc.dwWidth || at.y + sr.bottom - sr.top > (LONG)src->desc.dwHeight || dst->desc.dwWidth > src->desc.dwWidth ||
      dst->desc.dwHeight > src->desc.dwHeight || (src->desc.ddsCaps.dwCaps2 & DDSCAPS2_MIPMAPSUBLEVEL) || (dst->desc.ddsCaps.dwCaps2 & DDSCAPS2_MIPMAPSUBLEVEL)) {
    hr = DDERR_INVALIDPARAMS;
    D7_END();
  }
  if (src->desc.ddsCaps.dwCaps2 & DDSCAPS2_CUBEMAP) {
    if (!(dst->desc.ddsCaps.dwCaps2 & DDSCAPS2_CUBEMAP)) { hr = DDERR_INVALIDPARAMS; D7_END(); }
    /* (every face of the source must be one of the destination's, with its levels a subset of the source's) */
    for (UINT f = 0; f < 6; ++f) {
      Surface *sf = cube_face(src, f), *df = cube_face(dst, f);
      if (sf && !df) { hr = DDERR_INVALIDPARAMS; D7_END(); }
      if (sf && df && !level_subset(df, sf)) { hr = DDERR_INVALIDPARAMS; D7_END(); }
    }
    for (UINT f = 0; f < 6; ++f) {
      Surface *sf = cube_face(src, f), *df = cube_face(dst, f);
      if (sf && df && (flags & (DDSCAPS2_CUBEMAP_POSITIVEX << f))) load_chain(df, sf, at, sr);
    }
    hr = D3D_OK;
    D7_END();
  }
  if (dst->desc.ddsCaps.dwCaps2 & DDSCAPS2_CUBEMAP) { hr = DDERR_INVALIDPARAMS; D7_END(); }
  if (!level_subset(dst, src)) { hr = DDERR_INVALIDPARAMS; D7_END(); }
  load_chain(dst, src, at, sr);
  hr = D3D_OK;
  D7_END();
}
static HRESULT STDMETHODCALLTYPE dev7_LightEnable(IDirect3DDevice7 *i, DWORD index, BOOL enable) { D7_BEGIN(i); hr = d3_light(dev, index, NULL, enable ? 1 : 0); D7_END(); }
static HRESULT STDMETHODCALLTYPE dev7_GetLightEnable(IDirect3DDevice7 *i, DWORD index, BOOL *enable) {
  D7_BEGIN(i);
  if (!enable) hr = DDERR_INVALIDPARAMS;
  else if (index >= dev->st.light_count || !dev->st.lights[index].set) hr = DDERR_INVALIDPARAMS;
  else { *enable = dev->st.lights[index].enabled ? 128 : 0; hr = D3D_OK; }      /* (Windows reports 128 for an enabled light, as d3d9 does) */
  D7_END();
}
static UINT plane_limit(const D3dDevice *dev) {
  const UINT n = dev->dd3 ? dev->dd3->caps9.MaxUserClipPlanes : 0;
  return n < D3D_PLANES ? n : D3D_PLANES;
}
static HRESULT STDMETHODCALLTYPE dev7_SetClipPlane(IDirect3DDevice7 *i, DWORD index, D3DVALUE *plane) {
  D7_BEGIN(i);
  if (!plane) hr = DDERR_INVALIDPARAMS;
  else if (index >= plane_limit(dev)) hr = DDERR_INVALIDPARAMS;
  else { d3_set_plane(dev, index, plane); hr = D3D_OK; }
  D7_END();
}
static HRESULT STDMETHODCALLTYPE dev7_GetClipPlane(IDirect3DDevice7 *i, DWORD index, D3DVALUE *plane) {
  D7_BEGIN(i);
  if (!plane) hr = DDERR_INVALIDPARAMS;
  else if (index >= plane_limit(dev)) hr = DDERR_INVALIDPARAMS;
  else { for (UINT k = 0; k < 4; ++k) plane[k] = dev->st.plane[index][k]; hr = D3D_OK; }
  D7_END();
}
/* GetInfo: the retail runtime has no statistics to give (S_FALSE, as Microsoft documents for a non-debug driver); other ids are refused. */
static HRESULT STDMETHODCALLTYPE dev7_GetInfo(IDirect3DDevice7 *i, DWORD id, void *info, DWORD size) {
  (void)info; (void)size;
  D7_BEGIN(i);
  hr = (id == D3DDEVINFOID_TEXTUREMANAGER || id == D3DDEVINFOID_D3DTEXTUREMANAGER || id == D3DDEVINFOID_TEXTURING) ? S_FALSE : DDERR_INVALIDPARAMS;
  D7_END();
}

/* ---- IDirect3DDevice3 ------------------------------------------------------------------------------------------------------------------------------ */

static HRESULT STDMETHODCALLTYPE dev3_GetCaps(IDirect3DDevice3 *i, D3DDEVICEDESC *hal, D3DDEVICEDESC *hel) { DL_BEGIN(DEV3, i); hr = dv_get_caps_legacy(dev, hal, hel); DL_END(); }
static HRESULT STDMETHODCALLTYPE dev3_GetStats(IDirect3DDevice3 *i, D3DSTATS *stats) { DL_BEGIN(DEV3, i); hr = dv_get_stats(dev, stats); DL_END(); }
static HRESULT STDMETHODCALLTYPE dev3_AddViewport(IDirect3DDevice3 *i, IDirect3DViewport3 *vp) { DL_BEGIN(DEV3, i); hr = dv_add_viewport(dev, viewport_from(vp)); DL_END(); }
static HRESULT STDMETHODCALLTYPE dev3_DeleteViewport(IDirect3DDevice3 *i, IDirect3DViewport3 *vp) { DL_BEGIN(DEV3, i); hr = dv_delete_viewport(dev, viewport_from(vp)); DL_END(); }
static HRESULT STDMETHODCALLTYPE dev3_NextViewport(IDirect3DDevice3 *i, IDirect3DViewport3 *ref, IDirect3DViewport3 **out, DWORD flags) {
  DL_BEGIN(DEV3, i);
  hr = dv_next_viewport(dev, viewport_from(ref), (D3dViewport **)out, flags);
  DL_END();
}
static HRESULT STDMETHODCALLTYPE dev3_EnumTextureFormats(IDirect3DDevice3 *i, LPD3DENUMPIXELFORMATSCALLBACK cb, void *ctx) {
  DL_BEGIN(DEV3, i);
  hr = dv_enum_texture_formats(dev, (void *)cb, ctx, FALSE);
  DL_END();
}
static HRESULT STDMETHODCALLTYPE dev3_BeginScene(IDirect3DDevice3 *i) { DL_BEGIN(DEV3, i); hr = dv_begin_scene(dev); DL_END(); }
static HRESULT STDMETHODCALLTYPE dev3_EndScene(IDirect3DDevice3 *i) { DL_BEGIN(DEV3, i); hr = dv_end_scene(dev); DL_END(); }
static HRESULT STDMETHODCALLTYPE dev3_GetDirect3D(IDirect3DDevice3 *i, IDirect3D3 **out) { DL_BEGIN(DEV3, i); hr = dv_get_direct3d(dev, 3, (void **)out); DL_END(); }
static HRESULT STDMETHODCALLTYPE dev3_SetCurrentViewport(IDirect3DDevice3 *i, IDirect3DViewport3 *vp) { DL_BEGIN(DEV3, i); hr = dv_set_current_viewport(dev, viewport_from(vp)); DL_END(); }
static HRESULT STDMETHODCALLTYPE dev3_GetCurrentViewport(IDirect3DDevice3 *i, IDirect3DViewport3 **out) {
  DL_BEGIN(DEV3, i);
  hr = dv_get_current_viewport(dev, (D3dViewport **)out);
  DL_END();
}
static HRESULT STDMETHODCALLTYPE dev3_SetRenderTarget(IDirect3DDevice3 *i, IDirectDrawSurface4 *target, DWORD flags) {
  (void)flags;
  DL_BEGIN(DEV3, i);
  hr = dv_set_render_target(dev, surface_from_any(target), (IUnknown *)target, 3);
  DL_END();
}
static HRESULT STDMETHODCALLTYPE dev3_GetRenderTarget(IDirect3DDevice3 *i, IDirectDrawSurface4 **out) {
  DL_BEGIN(DEV3, i);
  hr = dv_get_render_target(dev, &IID_IDirectDrawSurface4, (void **)out);
  DL_END();
}
static HRESULT STDMETHODCALLTYPE dev3_Begin(IDirect3DDevice3 *i, D3DPRIMITIVETYPE type, DWORD fvf, DWORD flags) { DL_BEGIN(DEV3, i); hr = dv_begin(dev, type, fvf, NULL, 0, flags, FALSE); DL_END(); }
static HRESULT STDMETHODCALLTYPE dev3_BeginIndexed(IDirect3DDevice3 *i, D3DPRIMITIVETYPE type, DWORD fvf, void *v, DWORD count, DWORD flags) {
  DL_BEGIN(DEV3, i);
  hr = dv_begin(dev, type, fvf, v, count, flags, TRUE);
  DL_END();
}
static HRESULT STDMETHODCALLTYPE dev3_Vertex(IDirect3DDevice3 *i, void *v) { DL_BEGIN(DEV3, i); hr = dv_vertex(dev, v); DL_END(); }
static HRESULT STDMETHODCALLTYPE dev3_Index(IDirect3DDevice3 *i, WORD index) { DL_BEGIN(DEV3, i); hr = dv_index(dev, index); DL_END(); }
static HRESULT STDMETHODCALLTYPE dev3_End(IDirect3DDevice3 *i, DWORD flags) { DL_BEGIN(DEV3, i); hr = dv_end(dev, flags); DL_END(); }
static HRESULT STDMETHODCALLTYPE dev3_GetRenderState(IDirect3DDevice3 *i, D3DRENDERSTATETYPE state, DWORD *value) { DL_BEGIN(DEV3, i); hr = dv_get_render_state(dev, state, value); DL_END(); }
static HRESULT STDMETHODCALLTYPE dev3_SetRenderState(IDirect3DDevice3 *i, D3DRENDERSTATETYPE state, DWORD value) { DL_BEGIN(DEV3, i); hr = dv_set_render_state(dev, state, value); DL_END(); }
static HRESULT STDMETHODCALLTYPE dev3_GetLightState(IDirect3DDevice3 *i, D3DLIGHTSTATETYPE state, DWORD *value) { DL_BEGIN(DEV3, i); hr = dv_get_light_state(dev, state, value); DL_END(); }
static HRESULT STDMETHODCALLTYPE dev3_SetLightState(IDirect3DDevice3 *i, D3DLIGHTSTATETYPE state, DWORD value) { DL_BEGIN(DEV3, i); hr = dv_set_light_state(dev, state, value); DL_END(); }
static HRESULT STDMETHODCALLTYPE dev3_SetTransform(IDirect3DDevice3 *i, D3DTRANSFORMSTATETYPE state, D3DMATRIX *m) { DL_BEGIN(DEV3, i); hr = dv_set_transform(dev, state, m); DL_END(); }
static HRESULT STDMETHODCALLTYPE dev3_GetTransform(IDirect3DDevice3 *i, D3DTRANSFORMSTATETYPE state, D3DMATRIX *m) { DL_BEGIN(DEV3, i); hr = dv_get_transform(dev, state, m); DL_END(); }
static HRESULT STDMETHODCALLTYPE dev3_MultiplyTransform(IDirect3DDevice3 *i, D3DTRANSFORMSTATETYPE state, D3DMATRIX *m) { DL_BEGIN(DEV3, i); hr = dv_multiply_transform(dev, state, m); DL_END(); }
static HRESULT STDMETHODCALLTYPE dev3_DrawPrimitive(IDirect3DDevice3 *i, D3DPRIMITIVETYPE type, DWORD fvf, void *v, DWORD count, DWORD flags) {
  DL_BEGIN(DEV3, i);
  if (!dev->dd3) hr = DDERR_GENERIC;
  else hr = d3_draw(dev, type, fvf, v, count, NULL, 0, flags);
  DL_END();
}
static HRESULT STDMETHODCALLTYPE dev3_DrawIndexedPrimitive(IDirect3DDevice3 *i, D3DPRIMITIVETYPE type, DWORD fvf, void *v, DWORD count, WORD *idx, DWORD icount, DWORD flags) {
  DL_BEGIN(DEV3, i);
  if (!dev->dd3) hr = DDERR_GENERIC;
  else if (!count || !icount) hr = D3D_OK;
  else if (!idx) hr = DDERR_INVALIDPARAMS;
  else hr = d3_draw(dev, type, fvf, v, count, idx, icount, flags);
  DL_END();
}
static HRESULT STDMETHODCALLTYPE dev3_SetClipStatus(IDirect3DDevice3 *i, D3DCLIPSTATUS *cs) { DL_BEGIN(DEV3, i); hr = dv_set_clip_status(dev, cs); DL_END(); }
static HRESULT STDMETHODCALLTYPE dev3_GetClipStatus(IDirect3DDevice3 *i, D3DCLIPSTATUS *cs) { DL_BEGIN(DEV3, i); hr = dv_get_clip_status(dev, cs); DL_END(); }
static HRESULT STDMETHODCALLTYPE dev3_DrawPrimitiveStrided(IDirect3DDevice3 *i, D3DPRIMITIVETYPE type, DWORD fvf, D3DDRAWPRIMITIVESTRIDEDDATA *data, DWORD count, DWORD flags) {
  DL_BEGIN(DEV3, i);
  hr = dv_draw_strided(dev, type, fvf, data, count, NULL, 0, flags);
  DL_END();
}
static HRESULT STDMETHODCALLTYPE dev3_DrawIndexedPrimitiveStrided(IDirect3DDevice3 *i, D3DPRIMITIVETYPE type, DWORD fvf, D3DDRAWPRIMITIVESTRIDEDDATA *data, DWORD count,
                                                                  WORD *idx, DWORD icount, DWORD flags) {
  DL_BEGIN(DEV3, i);
  if (!count || !icount) hr = D3D_OK;
  else if (!idx) hr = DDERR_INVALIDPARAMS;
  else hr = dv_draw_strided(dev, type, fvf, data, count, idx, icount, flags);
  DL_END();
}
static HRESULT STDMETHODCALLTYPE dev3_DrawPrimitiveVB(IDirect3DDevice3 *i, D3DPRIMITIVETYPE type, IDirect3DVertexBuffer *vb, DWORD start, DWORD count, DWORD flags) {
  DL_BEGIN(DEV3, i);
  hr = dv_draw_vb(dev, type, vb_from(vb), start, count, NULL, 0, flags);
  DL_END();
}
static HRESULT STDMETHODCALLTYPE dev3_DrawIndexedPrimitiveVB(IDirect3DDevice3 *i, D3DPRIMITIVETYPE type, IDirect3DVertexBuffer *vb, WORD *idx, DWORD icount, DWORD flags) {
  DL_BEGIN(DEV3, i);
  D3dVertexBuffer *b = vb_from(vb);
  if (!icount) hr = D3D_OK;
  else if (!idx || !b) hr = DDERR_INVALIDPARAMS;
  else hr = dv_draw_vb(dev, type, b, 0, b->desc.dwNumVertices, idx, icount, flags);
  DL_END();
}
static HRESULT STDMETHODCALLTYPE dev3_ComputeSphereVisibility(IDirect3DDevice3 *i, D3DVECTOR *centers, D3DVALUE *radii, DWORD count, DWORD flags, DWORD *out) {
  DL_BEGIN(DEV3, i);
  hr = dv_sphere_visibility(dev, centers, radii, count, flags, out);
  DL_END();
}
static HRESULT STDMETHODCALLTYPE dev3_GetTexture(IDirect3DDevice3 *i, DWORD stage, IDirect3DTexture2 **out) {
  DL_BEGIN(DEV3, i);
  if (!out) hr = DDERR_INVALIDPARAMS;
  else {
    hr = D3D_OK;
    *out = NULL;
    Surface *s = stage < D3D_STAGES ? dev->st.texture[stage] : NULL;
    if (s && s->vt_tex2) { IUnknown_AddRef(s->texture_outer); *out = (IDirect3DTexture2 *)&s->vt_tex2; }
  }
  DL_END();
}
static Surface *surface_of_texture2(const void *iface);
static HRESULT STDMETHODCALLTYPE dev3_SetTexture(IDirect3DDevice3 *i, DWORD stage, IDirect3DTexture2 *texture) {
  DL_BEGIN(DEV3, i);
  Surface *s = surface_of_texture2(texture);
  if (texture && !s) hr = DDERR_INVALIDPARAMS;
  else hr = dv_set_texture(dev, stage, s);
  DL_END();
}
static HRESULT STDMETHODCALLTYPE dev3_GetTextureStageState(IDirect3DDevice3 *i, DWORD stage, D3DTEXTURESTAGESTATETYPE state, DWORD *value) {
  DL_BEGIN(DEV3, i);
  hr = dv_get_tss(dev, stage, state, value);
  DL_END();
}
static HRESULT STDMETHODCALLTYPE dev3_SetTextureStageState(IDirect3DDevice3 *i, DWORD stage, D3DTEXTURESTAGESTATETYPE state, DWORD value) {
  DL_BEGIN(DEV3, i);
  hr = dv_set_tss(dev, stage, state, value);
  DL_END();
}
static HRESULT STDMETHODCALLTYPE dev3_ValidateDevice(IDirect3DDevice3 *i, DWORD *passes) { DL_BEGIN(DEV3, i); hr = dv_validate(dev, passes); DL_END(); }

/* ---- IDirect3DDevice2 ------------------------------------------------------------------------------------------------------------------------------ */

static Surface *surface_of_texture1(const void *iface);
/* SwapTextureHandles of Direct3D 1 and 2: the two textures exchange their handles (a bound handle follows: TEXTUREHANDLE is set again). */
static HRESULT dv_swap_handles(D3dDevice *dev, Surface *a, Surface *b) {
  if (!a || !b) return DDERR_INVALIDPARAMS;
  D3dSurf *da = a->d3d, *db = b->d3d;
  if (!da || !db || !da->handle || !db->handle) return E_INVALIDARG;
  DWORD bound = 0;
  dv_get_render_state(dev, D3DRENDERSTATE_TEXTUREHANDLE, &bound);
  const DWORD ha = da->handle, hb = db->handle;
  g_handles.entries[ha - 1].object = b;
  g_handles.entries[hb - 1].object = a;
  da->handle = hb;
  db->handle = ha;
  if (bound == ha || bound == hb) dv_set_render_state(dev, D3DRENDERSTATE_TEXTUREHANDLE, bound);
  return D3D_OK;
}
static HRESULT STDMETHODCALLTYPE dev2_GetCaps(IDirect3DDevice2 *i, D3DDEVICEDESC *hal, D3DDEVICEDESC *hel) { DL_BEGIN(DEV2, i); hr = dv_get_caps_legacy(dev, hal, hel); DL_END(); }
static HRESULT STDMETHODCALLTYPE dev2_SwapTextureHandles(IDirect3DDevice2 *i, IDirect3DTexture2 *a, IDirect3DTexture2 *b) {
  DL_BEGIN(DEV2, i);
  hr = dv_swap_handles(dev, surface_of_texture2(a), surface_of_texture2(b));
  DL_END();
}
static HRESULT STDMETHODCALLTYPE dev2_GetStats(IDirect3DDevice2 *i, D3DSTATS *stats) { DL_BEGIN(DEV2, i); hr = dv_get_stats(dev, stats); DL_END(); }
static HRESULT STDMETHODCALLTYPE dev2_AddViewport(IDirect3DDevice2 *i, IDirect3DViewport2 *vp) { DL_BEGIN(DEV2, i); hr = dv_add_viewport(dev, viewport_from(vp)); DL_END(); }
static HRESULT STDMETHODCALLTYPE dev2_DeleteViewport(IDirect3DDevice2 *i, IDirect3DViewport2 *vp) { DL_BEGIN(DEV2, i); hr = dv_delete_viewport(dev, viewport_from(vp)); DL_END(); }
static HRESULT STDMETHODCALLTYPE dev2_NextViewport(IDirect3DDevice2 *i, IDirect3DViewport2 *ref, IDirect3DViewport2 **out, DWORD flags) {
  DL_BEGIN(DEV2, i);
  hr = dv_next_viewport(dev, viewport_from(ref), (D3dViewport **)out, flags);
  DL_END();
}
static HRESULT STDMETHODCALLTYPE dev2_EnumTextureFormats(IDirect3DDevice2 *i, LPD3DENUMTEXTUREFORMATSCALLBACK cb, void *ctx) {
  DL_BEGIN(DEV2, i);
  hr = dv_enum_texture_formats(dev, (void *)cb, ctx, TRUE);
  DL_END();
}
static HRESULT STDMETHODCALLTYPE dev2_BeginScene(IDirect3DDevice2 *i) { DL_BEGIN(DEV2, i); hr = dv_begin_scene(dev); DL_END(); }
static HRESULT STDMETHODCALLTYPE dev2_EndScene(IDirect3DDevice2 *i) { DL_BEGIN(DEV2, i); hr = dv_end_scene(dev); DL_END(); }
static HRESULT STDMETHODCALLTYPE dev2_GetDirect3D(IDirect3DDevice2 *i, IDirect3D2 **out) { DL_BEGIN(DEV2, i); hr = dv_get_direct3d(dev, 2, (void **)out); DL_END(); }
static HRESULT STDMETHODCALLTYPE dev2_SetCurrentViewport(IDirect3DDevice2 *i, IDirect3DViewport2 *vp) { DL_BEGIN(DEV2, i); hr = dv_set_current_viewport(dev, viewport_from(vp)); DL_END(); }
static HRESULT STDMETHODCALLTYPE dev2_GetCurrentViewport(IDirect3DDevice2 *i, IDirect3DViewport2 **out) {
  DL_BEGIN(DEV2, i);
  hr = dv_get_current_viewport(dev, (D3dViewport **)out);
  DL_END();
}
static HRESULT STDMETHODCALLTYPE dev2_SetRenderTarget(IDirect3DDevice2 *i, IDirectDrawSurface *target, DWORD flags) {
  (void)flags;
  DL_BEGIN(DEV2, i);
  hr = dv_set_render_target(dev, surface_from_any(target), (IUnknown *)target, 2);
  DL_END();
}
static HRESULT STDMETHODCALLTYPE dev2_GetRenderTarget(IDirect3DDevice2 *i, IDirectDrawSurface **out) {
  DL_BEGIN(DEV2, i);
  hr = dv_get_render_target(dev, &IID_IDirectDrawSurface, (void **)out);
  DL_END();
}
static HRESULT STDMETHODCALLTYPE dev2_Begin(IDirect3DDevice2 *i, D3DPRIMITIVETYPE type, D3DVERTEXTYPE vt, DWORD flags) {
  DL_BEGIN(DEV2, i);
  const DWORD fvf = dl_fvf_of_vertex_type(vt);
  hr = fvf ? dv_begin(dev, type, fvf, NULL, 0, flags, FALSE) : DDERR_INVALIDPARAMS;
  DL_END();
}
static HRESULT STDMETHODCALLTYPE dev2_BeginIndexed(IDirect3DDevice2 *i, D3DPRIMITIVETYPE type, D3DVERTEXTYPE vt, void *v, DWORD count, DWORD flags) {
  DL_BEGIN(DEV2, i);
  const DWORD fvf = dl_fvf_of_vertex_type(vt);
  hr = fvf ? dv_begin(dev, type, fvf, v, count, flags, TRUE) : DDERR_INVALIDPARAMS;
  DL_END();
}
static HRESULT STDMETHODCALLTYPE dev2_Vertex(IDirect3DDevice2 *i, void *v) { DL_BEGIN(DEV2, i); hr = dv_vertex(dev, v); DL_END(); }
static HRESULT STDMETHODCALLTYPE dev2_Index(IDirect3DDevice2 *i, WORD index) { DL_BEGIN(DEV2, i); hr = dv_index(dev, index); DL_END(); }
static HRESULT STDMETHODCALLTYPE dev2_End(IDirect3DDevice2 *i, DWORD flags) { DL_BEGIN(DEV2, i); hr = dv_end(dev, flags); DL_END(); }
static HRESULT STDMETHODCALLTYPE dev2_GetRenderState(IDirect3DDevice2 *i, D3DRENDERSTATETYPE state, DWORD *value) { DL_BEGIN(DEV2, i); hr = dv_get_render_state(dev, state, value); DL_END(); }
static HRESULT STDMETHODCALLTYPE dev2_SetRenderState(IDirect3DDevice2 *i, D3DRENDERSTATETYPE state, DWORD value) { DL_BEGIN(DEV2, i); hr = dv_set_render_state(dev, state, value); DL_END(); }
static HRESULT STDMETHODCALLTYPE dev2_GetLightState(IDirect3DDevice2 *i, D3DLIGHTSTATETYPE state, DWORD *value) { DL_BEGIN(DEV2, i); hr = dv_get_light_state(dev, state, value); DL_END(); }
static HRESULT STDMETHODCALLTYPE dev2_SetLightState(IDirect3DDevice2 *i, D3DLIGHTSTATETYPE state, DWORD value) { DL_BEGIN(DEV2, i); hr = dv_set_light_state(dev, state, value); DL_END(); }
static HRESULT STDMETHODCALLTYPE dev2_SetTransform(IDirect3DDevice2 *i, D3DTRANSFORMSTATETYPE state, D3DMATRIX *m) { DL_BEGIN(DEV2, i); hr = dv_set_transform(dev, state, m); DL_END(); }
static HRESULT STDMETHODCALLTYPE dev2_GetTransform(IDirect3DDevice2 *i, D3DTRANSFORMSTATETYPE state, D3DMATRIX *m) { DL_BEGIN(DEV2, i); hr = dv_get_transform(dev, state, m); DL_END(); }
static HRESULT STDMETHODCALLTYPE dev2_MultiplyTransform(IDirect3DDevice2 *i, D3DTRANSFORMSTATETYPE state, D3DMATRIX *m) { DL_BEGIN(DEV2, i); hr = dv_multiply_transform(dev, state, m); DL_END(); }
static HRESULT STDMETHODCALLTYPE dev2_DrawPrimitive(IDirect3DDevice2 *i, D3DPRIMITIVETYPE type, D3DVERTEXTYPE vt, void *v, DWORD count, DWORD flags) {
  DL_BEGIN(DEV2, i);
  const DWORD fvf = dl_fvf_of_vertex_type(vt);
  if (!fvf) hr = DDERR_INVALIDPARAMS;
  else if (!dev->dd3) hr = DDERR_GENERIC;
  else hr = d3_draw(dev, type, fvf, v, count, NULL, 0, flags);
  DL_END();
}
static HRESULT STDMETHODCALLTYPE dev2_DrawIndexedPrimitive(IDirect3DDevice2 *i, D3DPRIMITIVETYPE type, D3DVERTEXTYPE vt, void *v, DWORD count, WORD *idx, DWORD icount, DWORD flags) {
  DL_BEGIN(DEV2, i);
  const DWORD fvf = dl_fvf_of_vertex_type(vt);
  if (!fvf) hr = DDERR_INVALIDPARAMS;
  else if (!dev->dd3) hr = DDERR_GENERIC;
  else if (!count || !icount) hr = D3D_OK;
  else if (!idx) hr = DDERR_INVALIDPARAMS;
  else hr = d3_draw(dev, type, fvf, v, count, idx, icount, flags);
  DL_END();
}
static HRESULT STDMETHODCALLTYPE dev2_SetClipStatus(IDirect3DDevice2 *i, D3DCLIPSTATUS *cs) { DL_BEGIN(DEV2, i); hr = dv_set_clip_status(dev, cs); DL_END(); }
static HRESULT STDMETHODCALLTYPE dev2_GetClipStatus(IDirect3DDevice2 *i, D3DCLIPSTATUS *cs) { DL_BEGIN(DEV2, i); hr = dv_get_clip_status(dev, cs); DL_END(); }

/* ---- IDirect3DDevice ------------------------------------------------------------------------------------------------------------------------------- */

static HRESULT STDMETHODCALLTYPE dev1_Initialize(IDirect3DDevice *i, IDirect3D *d3d, GUID *guid, D3DDEVICEDESC *desc) {
  (void)i; (void)d3d; (void)guid; (void)desc;
  return DDERR_ALREADYINITIALIZED;      /* (the device was made initialized by QueryInterface) */
}
static HRESULT STDMETHODCALLTYPE dev1_GetCaps(IDirect3DDevice *i, D3DDEVICEDESC *hal, D3DDEVICEDESC *hel) { DL_BEGIN(DEV1, i); hr = dv_get_caps_legacy(dev, hal, hel); DL_END(); }
static HRESULT STDMETHODCALLTYPE dev1_SwapTextureHandles(IDirect3DDevice *i, IDirect3DTexture *a, IDirect3DTexture *b) {
  DL_BEGIN(DEV1, i);
  hr = dv_swap_handles(dev, surface_of_texture1(a), surface_of_texture1(b));
  DL_END();
}
static HRESULT d3_execute_buffer_create(D3dDevice *dev, D3DEXECUTEBUFFERDESC *desc, IDirect3DExecuteBuffer **out);
static HRESULT STDMETHODCALLTYPE dev1_CreateExecuteBuffer(IDirect3DDevice *i, D3DEXECUTEBUFFERDESC *desc, IDirect3DExecuteBuffer **out, IUnknown *outer) {
  DL_BEGIN(DEV1, i);
  if (outer) hr = CLASS_E_NOAGGREGATION;
  else hr = d3_execute_buffer_create(dev, desc, out);
  DL_END();
}
static HRESULT STDMETHODCALLTYPE dev1_GetStats(IDirect3DDevice *i, D3DSTATS *stats) { DL_BEGIN(DEV1, i); hr = dv_get_stats(dev, stats); DL_END(); }
static D3dExecuteBuffer *execute_buffer_from(const void *iface);
static HRESULT STDMETHODCALLTYPE dev1_Execute(IDirect3DDevice *i, IDirect3DExecuteBuffer *buffer, IDirect3DViewport *viewport, DWORD flags) {
  (void)flags;
  DL_BEGIN(DEV1, i);
  D3dExecuteBuffer *eb = execute_buffer_from(buffer);
  D3dViewport *vp = viewport_from(viewport);
  if (!eb) hr = DDERR_INVALIDPARAMS;
  else if (FAILED(hr = dv_set_current_viewport(dev, vp))) {}
  else if (eb->locked) hr = D3DERR_EXECUTE_LOCKED;
  else if (!dev->dd3) hr = DDERR_GENERIC;
  else hr = d3_execute(dev, eb);
  DL_END();
}
static HRESULT STDMETHODCALLTYPE dev1_AddViewport(IDirect3DDevice *i, IDirect3DViewport *vp) { DL_BEGIN(DEV1, i); hr = dv_add_viewport(dev, viewport_from(vp)); DL_END(); }
static HRESULT STDMETHODCALLTYPE dev1_DeleteViewport(IDirect3DDevice *i, IDirect3DViewport *vp) { DL_BEGIN(DEV1, i); hr = dv_delete_viewport(dev, viewport_from(vp)); DL_END(); }
static HRESULT STDMETHODCALLTYPE dev1_NextViewport(IDirect3DDevice *i, IDirect3DViewport *ref, IDirect3DViewport **out, DWORD flags) {
  DL_BEGIN(DEV1, i);
  hr = dv_next_viewport(dev, viewport_from(ref), (D3dViewport **)out, flags);
  DL_END();
}
/* Pick: the triangles of an execute buffer under a point of the viewport (their records into the pick records). */
static HRESULT d3_pick(D3dDevice *dev, D3dExecuteBuffer *eb, D3dViewport *vp, const D3DRECT *rect);
static HRESULT STDMETHODCALLTYPE dev1_Pick(IDirect3DDevice *i, IDirect3DExecuteBuffer *buffer, IDirect3DViewport *viewport, DWORD flags, D3DRECT *rect) {
  (void)flags;
  DL_BEGIN(DEV1, i);
  D3dExecuteBuffer *eb = execute_buffer_from(buffer);
  D3dViewport *vp = viewport_from(viewport);
  if (!eb || !vp || !rect) hr = DDERR_INVALIDPARAMS;
  else if (eb->locked) hr = D3DERR_EXECUTE_LOCKED;
  else hr = d3_pick(dev, eb, vp, rect);
  DL_END();
}
static HRESULT STDMETHODCALLTYPE dev1_GetPickRecords(IDirect3DDevice *i, DWORD *count, D3DPICKRECORD *records) {
  DL_BEGIN(DEV1, i);
  if (!count) hr = DDERR_INVALIDPARAMS;
  else {
    if (records) {
      const DWORD n = *count < dev->pick_count ? *count : dev->pick_count;
      for (DWORD k = 0; k < n; ++k) records[k] = dev->picks[k];
    }
    *count = dev->pick_count;
    hr = D3D_OK;
  }
  DL_END();
}
static HRESULT STDMETHODCALLTYPE dev1_EnumTextureFormats(IDirect3DDevice *i, LPD3DENUMTEXTUREFORMATSCALLBACK cb, void *ctx) {
  DL_BEGIN(DEV1, i);
  hr = dv_enum_texture_formats(dev, (void *)cb, ctx, TRUE);
  DL_END();
}
static HRESULT STDMETHODCALLTYPE dev1_CreateMatrix(IDirect3DDevice *i, D3DMATRIXHANDLE *out) {
  DL_BEGIN(DEV1, i);
  if (!out) hr = DDERR_INVALIDPARAMS;
  else {
    DlMatrix *m = mem_alloc(sizeof *m);
    const DWORD h = m ? handle_alloc(&g_handles, H_MATRIX, m, dev) : 0;
    if (!h) { mem_free(m); hr = DDERR_OUTOFMEMORY; }
    else { *out = h; hr = D3D_OK; }
  }
  DL_END();
}
static HRESULT STDMETHODCALLTYPE dev1_SetMatrix(IDirect3DDevice *i, D3DMATRIXHANDLE handle, D3DMATRIX *matrix) {
  DL_BEGIN(DEV1, i);
  DlMatrix *m = handle_get(&g_handles, handle, H_MATRIX);
  if (!matrix || !m) hr = DDERR_INVALIDPARAMS;
  else {
    memcpy(m, matrix, sizeof *m);
    /* (a matrix that is a transform of the device by STATETRANSFORM changes it) */
    for (UINT k = 1; k <= 3; ++k)
      if (dev->matrix_handle[k] == handle) d3_set_xf(dev, k, m);
    hr = D3D_OK;
  }
  DL_END();
}
static HRESULT STDMETHODCALLTYPE dev1_GetMatrix(IDirect3DDevice *i, D3DMATRIXHANDLE handle, D3DMATRIX *matrix) {
  DL_BEGIN(DEV1, i);
  const DlMatrix *m = handle_get(&g_handles, handle, H_MATRIX);
  if (!matrix || !m) hr = DDERR_INVALIDPARAMS;
  else { memcpy(matrix, m, sizeof *matrix); hr = D3D_OK; }
  DL_END();
}
static HRESULT STDMETHODCALLTYPE dev1_DeleteMatrix(IDirect3DDevice *i, D3DMATRIXHANDLE handle) {
  DL_BEGIN(DEV1, i);
  DlMatrix *m = handle_free(&g_handles, handle, H_MATRIX);
  if (!m) hr = DDERR_INVALIDPARAMS;
  else {
    mem_free(m);
    for (UINT k = 1; k <= 3; ++k) if (dev->matrix_handle[k] == handle) dev->matrix_handle[k] = 0;
    hr = D3D_OK;
  }
  DL_END();
}
static HRESULT STDMETHODCALLTYPE dev1_BeginScene(IDirect3DDevice *i) { DL_BEGIN(DEV1, i); hr = dv_begin_scene(dev); DL_END(); }
static HRESULT STDMETHODCALLTYPE dev1_EndScene(IDirect3DDevice *i) { DL_BEGIN(DEV1, i); hr = dv_end_scene(dev); DL_END(); }
static HRESULT STDMETHODCALLTYPE dev1_GetDirect3D(IDirect3DDevice *i, IDirect3D **out) { DL_BEGIN(DEV1, i); hr = dv_get_direct3d(dev, 1, (void **)out); DL_END(); }
