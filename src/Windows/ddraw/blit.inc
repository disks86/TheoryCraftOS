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
/* Blt, BltFast, BltBatch and Flip (the pixels move in ddraw_pixels.h). Included by ddraw.c.
 *
 * Blt: colour fills (DDBLT_COLORFILL with a raw value of the surface's format), depth fills (DDBLT_DEPTHFILL: the depth bits, the stencil kept), the
 * ROPs Windows' blitter has (SRCCOPY, BLACKNESS, WHITENESS; the others DDERR_NORASTEROPHW), stretching and shrinking, DDBLTFX mirroring and turning
 * (DDBLT_DDFX), source colour keys (the source's or DDBLT_KEYSRCOVERRIDE's), destination colour keys (the destination's own in versions 4 and 7, the source's
 * DDCKEY_DESTBLT in the older versions - as Windows does it - or DDBLT_KEYDESTOVERRIDE's), conversion between the formats, blits of a surface to
 * itself, and clipping by the destination's clipper (each rectangle of its clip list, the source scaled to match). The alpha and z-buffer blits of
 * DDBLT_ALPHA* and DDBLT_ZBUFFER* are what no driver here has: they are left out as Windows' blitter leaves them (the blit is done without them).
 * A blit into the primary surface's front buffer shows it (present.inc). */

/* Copies the source rectangle to memory of its own (for a blit of a surface to an overlapping part of itself). */
static BOOL image_copy_rect(const DdImage *src, const DdRect *r, DdImage *copy, DdRect *copy_rect) {
  *copy = *src;
  /* (block and planar sources are copied decoded, as A8R8G8B8: a rectangle of them need not be whole blocks or planes) */
  if (dd_format_is_blocks(src->fmt) || dd_format(src->fmt)->kind == DD_KIND_YUV_PLANAR) copy->fmt = DD_FMT_A8R8G8B8;
  const uint32_t w = (uint32_t)(r->right - r->left), h = (uint32_t)(r->bottom - r->top);
  const uint32_t pitch = dd_aligned_pitch(copy->fmt, w, 4);
  if (!pitch) return FALSE;
  const uint64_t bytes = dd_image_bytes(copy->fmt, w, h, pitch);
  if (!bytes || bytes > 0x7FFFFFFFull) return FALSE;
  copy->data = mem_alloc((SIZE_T)bytes);
  if (!copy->data) return FALSE;
  copy->size = (SIZE_T)bytes;
  copy->width = w;
  copy->height = h;
  copy->pitch = pitch;
  const DdRect all = {0, 0, (int32_t)w, (int32_t)h};
  *copy_rect = all;
  if (dd_blit(copy, &all, src, r, NULL) != DD_PIX_OK) { mem_free(copy->data); copy->data = NULL; return FALSE; }
  return TRUE;
}

static HRESULT pix_result(int r) {
  switch (r) {
    case DD_PIX_OK: return DD_OK;
    case DD_PIX_BAD_RECT: return DDERR_INVALIDRECT;
    case DD_PIX_UNSUPPORTED: return DDERR_UNSUPPORTED;
    default: return DDERR_INVALIDPARAMS;
  }
}

/* One blit of one rectangle (already clipped and checked). */
static HRESULT blit_rect(Surface *dst, const RECT *dr, Surface *src, const RECT *sr, const DdBlit *b) {
  cpu_access(src, sr);
  cpu_access(dst, dr);
  uint32_t dpal[256], spal[256];
  DdImage di, si;
  surface_image(dst, &di, dpal);
  surface_image(src, &si, spal);
  const DdRect drr = {dr->left, dr->top, dr->right, dr->bottom}, srr = {sr->left, sr->top, sr->right, sr->bottom};
  int r = dd_blit(&di, &drr, &si, &srr, b);
  if (r == DD_PIX_OVERLAP) {
    DdImage copy;
    DdRect cr;
    if (!image_copy_rect(&si, &srr, &copy, &cr)) return DDERR_OUTOFMEMORY;
    r = dd_blit(&di, &drr, &copy, &cr, b);
    mem_free(copy.data);
  }
  if (r == DD_PIX_OK) cpu_wrote(dst, dr);
  return pix_result(r);
}
static HRESULT fill_rect(Surface *dst, const RECT *dr, DWORD value, BOOL depth) {
  cpu_access(dst, dr);
  DdImage di;
  surface_image(dst, &di, NULL);
  const DdRect drr = {dr->left, dr->top, dr->right, dr->bottom};
  int r;
  if (depth) {
    /* (the depth bits only: a depth fill keeps the stencil) */
    if (!dd_image_check(&di)) r = DD_PIX_BAD_IMAGE;
    else if (!dd_rect_inside(&di, &drr)) r = DD_PIX_BAD_RECT;
    else {
      const uint32_t z = dd_format(dst->fmt)->r;
      for (uint32_t y = (uint32_t)dr->top; y < (uint32_t)dr->bottom; ++y)
        for (uint32_t x = (uint32_t)dr->left; x < (uint32_t)dr->right; ++x) dd_put_raw(&di, x, y, (dd_get_raw(&di, x, y) & ~z) | (value & z));
      r = DD_PIX_OK;
    }
  } else {
    r = dd_fill(&di, &drr, value);
  }
  if (r == DD_PIX_OK) cpu_wrote(dst, dr);
  return pix_result(r);
}

/* The clip rectangles of a destination rectangle (the clipper's list met with it); *count 0: nothing to draw. The caller frees the list. */
static HRESULT clip_rects(Clipper *c, const RECT *dst, RECT **out, DWORD *count) {
  *out = NULL;
  *count = 0;
  DWORD size = 0;
  IDirectDrawClipper *ci = (IDirectDrawClipper *)c;
  HRESULT hr = clip_GetClipList(ci, (RECT *)dst, NULL, &size);
  if (FAILED(hr)) return hr;
  RGNDATA *data = mem_alloc(size);
  if (!data) return DDERR_OUTOFMEMORY;
  hr = clip_GetClipList(ci, (RECT *)dst, data, &size);
  if (FAILED(hr)) { mem_free(data); return hr; }
  const DWORD n = data->rdh.nCount;
  RECT *rects = mem_alloc((n ? n : 1) * sizeof(RECT));
  if (!rects) { mem_free(data); return DDERR_OUTOFMEMORY; }
  memcpy(rects, data->Buffer, n * sizeof(RECT));
  mem_free(data);
  *out = rects;
  *count = n;
  return DD_OK;
}

static BOOL surface_busy(const Surface *s) { return s->lock_count || s->dc; }

/* Blt of any version (`version`: the calling interface's, for the destination colour key's source). */
static HRESULT surface_blt(Surface *dst, RECT *dst_rect, Surface *src, RECT *src_rect, DWORD flags, DDBLTFX *fx, UINT version) {
  /* (what no driver here does: alpha and z blits; Windows' own blitter leaves them out as well) */
  flags &= ~(DWORD)(DDBLT_ALPHADEST | DDBLT_ALPHADESTCONSTOVERRIDE | DDBLT_ALPHADESTNEG | DDBLT_ALPHADESTSURFACEOVERRIDE | DDBLT_ALPHAEDGEBLEND |
                    DDBLT_ALPHASRC | DDBLT_ALPHASRCCONSTOVERRIDE | DDBLT_ALPHASRCNEG | DDBLT_ALPHASRCSURFACEOVERRIDE | DDBLT_ZBUFFER |
                    DDBLT_ZBUFFERDESTCONSTOVERRIDE | DDBLT_ZBUFFERDESTOVERRIDE | DDBLT_ZBUFFERSRCCONSTOVERRIDE | DDBLT_ZBUFFERSRCOVERRIDE);
  if ((flags & DDBLT_KEYSRCOVERRIDE) && (!fx || (flags & DDBLT_KEYSRC))) return DDERR_INVALIDPARAMS;
  if ((flags & DDBLT_KEYDESTOVERRIDE) && (!fx || (flags & DDBLT_KEYDEST))) return DDERR_INVALIDPARAMS;
  if (flags & DDBLT_DDROPS) return DDERR_NORASTEROPHW;
  if (flags & (DDBLT_DDFX) && !fx) return DDERR_INVALIDPARAMS;
  dd_lock();
  HRESULT hr = DD_OK;
  DDBLTFX rop_fx;
  if (flags & (DDBLT_COLORFILL | DDBLT_DEPTHFILL)) {
    if (flags & DDBLT_ROP) { hr = DDERR_INVALIDPARAMS; goto done; }
    if (src) { hr = DDERR_INVALIDPARAMS; goto done; }
    if (!fx) { hr = DDERR_INVALIDPARAMS; goto done; }
    if ((flags & (DDBLT_COLORFILL | DDBLT_DEPTHFILL)) == (DDBLT_COLORFILL | DDBLT_DEPTHFILL)) flags &= ~(DWORD)DDBLT_DEPTHFILL;
    if ((dst->desc.ddsCaps.dwCaps & DDSCAPS_ZBUFFER) && (flags & DDBLT_COLORFILL)) { hr = DDERR_INVALIDPARAMS; goto done; }
    if (!(dst->desc.ddsCaps.dwCaps & DDSCAPS_ZBUFFER) && (flags & DDBLT_DEPTHFILL)) { hr = DDERR_INVALIDPARAMS; goto done; }
  }
  if (flags & DDBLT_ROP) {
    if (!fx) { hr = DDERR_INVALIDPARAMS; goto done; }
    if (src && src_rect && ((ULONG)src_rect->left >= (ULONG)src_rect->right || (DWORD)src_rect->right > src->desc.dwWidth ||
                            (ULONG)src_rect->top >= (ULONG)src_rect->bottom || (DWORD)src_rect->bottom > src->desc.dwHeight)) { hr = DDERR_INVALIDRECT; goto done; }
    flags &= ~(DWORD)DDBLT_ROP;
    switch (fx->dwROP) {
      case SRCCOPY: break;
      case WHITENESS:
      case BLACKNESS:
        /* (no ROP fills of FourCC surfaces - YUV, DXT -, where a colour fill of the raw value works: Wine's test_color_fill) */
        if (dd_format_is_yuv(dst->fmt) || dd_format_is_blocks(dst->fmt)) { hr = DDERR_NORASTEROPHW; goto done; }
        rop_fx = *fx;
        rop_fx.dwFillColor = fx->dwROP == WHITENESS ? 0xFFFFFFFFu : 0;
        flags |= (dst->desc.ddsCaps.dwCaps & DDSCAPS_ZBUFFER) ? DDBLT_DEPTHFILL : DDBLT_COLORFILL;
        fx = &rop_fx;
        src = NULL;
        break;
      default: hr = DDERR_NORASTEROPHW; goto done;
    }
  }
  if (!(flags & (DDBLT_COLORFILL | DDBLT_DEPTHFILL)) && !src) { hr = DDERR_INVALIDPARAMS; goto done; }
  if ((flags & DDBLT_KEYSRC) && (!src || !(src->desc.dwFlags & DDSD_CKSRCBLT))) { hr = DDERR_INVALIDPARAMS; goto done; }
  /* (the destination colour key: versions 4 and 7 take the destination's, 1 .. 3 the source's, as Windows) */
  const Surface *dst_key_owner = (version >= 4 || !src) ? dst : src;
  if ((flags & DDBLT_KEYDEST) && !(dst->desc.dwFlags & DDSD_CKDESTBLT) && !(dst_key_owner->desc.dwFlags & DDSD_CKDESTBLT)) { hr = DDERR_INVALIDPARAMS; goto done; }
  if (surface_is_lost(dst) || (src && surface_is_lost(src))) { hr = DDERR_SURFACELOST; goto done; }
  RECT dr, sr;
  if (dst_rect) dr = *dst_rect; else rect_set(&dr, 0, 0, (LONG)dst->desc.dwWidth, (LONG)dst->desc.dwHeight);
  if (rect_empty(&dr)) { hr = DDERR_INVALIDRECT; goto done; }
  if (src) {
    if (src_rect) sr = *src_rect; else rect_set(&sr, 0, 0, (LONG)src->desc.dwWidth, (LONG)src->desc.dwHeight);
    if (rect_empty(&sr)) { hr = DDERR_INVALIDRECT; goto done; }
    if (sr.left < 0 || sr.top < 0 || sr.right > (LONG)src->desc.dwWidth || sr.bottom > (LONG)src->desc.dwHeight) { hr = DDERR_INVALIDRECT; goto done; }
    if (surface_busy(src)) { hr = DDERR_SURFACEBUSY; goto done; }
    if (dd_format_is_depth(src->fmt) != dd_format_is_depth(dst->fmt)) { hr = DDERR_INVALIDPARAMS; goto done; }
    /* (Windows blits between surfaces of different DirectDraw objects only as copies of one format: a conversion between them is E_NOTIMPL -
     * Wine's test_cross_device_blt) */
    if (src->dd != dst->dd && src->dd && dst->dd && src->fmt != dst->fmt) { hr = DDERR_UNSUPPORTED; goto done; }
  } else {
    rect_set(&sr, 0, 0, 0, 0);
  }
  if (surface_busy(dst)) { hr = DDERR_SURFACEBUSY; goto done; }
  DdBlit b;
  zero(&b, sizeof b);
  if (flags & DDBLT_KEYSRC) { b.flags |= DD_BLIT_SRC_KEY; b.src_key_low = src->desc.ddckCKSrcBlt.dwColorSpaceLowValue; b.src_key_high = src->desc.ddckCKSrcBlt.dwColorSpaceHighValue; }
  else if (flags & DDBLT_KEYSRCOVERRIDE) { b.flags |= DD_BLIT_SRC_KEY; b.src_key_low = fx->ddckSrcColorkey.dwColorSpaceLowValue; b.src_key_high = fx->ddckSrcColorkey.dwColorSpaceHighValue; }
  if (flags & DDBLT_KEYDEST) {
    /* (versions 1-3 take the source's key value even after SetColorKey(DDCKEY_DESTBLT, NULL) took it off: "it uses the old key from the src
     * surface", Wine's test_ck_operation) */
    const Surface *k = (version < 4 && src) ? src : (dst_key_owner->desc.dwFlags & DDSD_CKDESTBLT) ? dst_key_owner : dst;
    b.flags |= DD_BLIT_DST_KEY;
    b.dst_key_low = k->desc.ddckCKDestBlt.dwColorSpaceLowValue;
    b.dst_key_high = k->desc.ddckCKDestBlt.dwColorSpaceHighValue;
  } else if (flags & DDBLT_KEYDESTOVERRIDE) {
    b.flags |= DD_BLIT_DST_KEY;
    b.dst_key_low = fx->ddckDestColorkey.dwColorSpaceLowValue;
    b.dst_key_high = fx->ddckDestColorkey.dwColorSpaceHighValue;
  }
  if ((flags & DDBLT_DDFX) && fx) {
    if (fx->dwDDFX & DDBLTFX_MIRRORLEFTRIGHT) b.flags |= DD_BLIT_MIRROR_LR;
    if (fx->dwDDFX & DDBLTFX_MIRRORUPDOWN) b.flags |= DD_BLIT_MIRROR_UD;
    if (fx->dwDDFX & DDBLTFX_ROTATE90) b.flags |= DD_BLIT_ROTATE90;
    if (fx->dwDDFX & DDBLTFX_ROTATE180) b.flags |= DD_BLIT_ROTATE180;
    if (fx->dwDDFX & DDBLTFX_ROTATE270) b.flags |= DD_BLIT_ROTATE270;
  }
  const BOOL fill = (flags & (DDBLT_COLORFILL | DDBLT_DEPTHFILL)) != 0;
  const DWORD fill_value = fill ? fx->dwFillColor : 0;
  RECT whole;
  rect_set(&whole, 0, 0, (LONG)dst->desc.dwWidth, (LONG)dst->desc.dwHeight);
  RECT drawn;
  rect_set(&drawn, 0, 0, 0, 0);
  if (!dst->clipper) {
    if (dr.left < 0 || dr.top < 0 || dr.right > whole.right || dr.bottom > whole.bottom) { hr = DDERR_INVALIDRECT; goto done; }
    hr = fill ? fill_rect(dst, &dr, fill_value, (flags & DDBLT_DEPTHFILL) != 0) : blit_rect(dst, &dr, src, &sr, &b);
    if (SUCCEEDED(hr)) drawn = dr;
  } else {
    if (!clipper_alive(dst->clipper)) { hr = DDERR_INVALIDPARAMS; goto done; }
    RECT *rects;
    DWORD n;
    if (FAILED(hr = clip_rects(dst->clipper, &dr, &rects, &n))) goto done;
    const LONG dw = dr.right - dr.left, dh = dr.bottom - dr.top;
    for (DWORD i = 0; i < n && SUCCEEDED(hr); ++i) {
      RECT c;
      if (!rect_intersect(&c, &rects[i], &whole)) continue;
      if (fill) {
        hr = fill_rect(dst, &c, fill_value, (flags & DDBLT_DEPTHFILL) != 0);
      } else {
        /* (the part of the source that lands in this rectangle) */
        RECT s = sr;
        const LONG sw = sr.right - sr.left, sh = sr.bottom - sr.top;
        s.left += (LONG)(((LONGLONG)(c.left - dr.left) * sw) / dw);
        s.top += (LONG)(((LONGLONG)(c.top - dr.top) * sh) / dh);
        s.right -= (LONG)(((LONGLONG)(dr.right - c.right) * sw) / dw);
        s.bottom -= (LONG)(((LONGLONG)(dr.bottom - c.bottom) * sh) / dh);
        if (rect_empty(&s)) continue;
        hr = blit_rect(dst, &c, src, &s, &b);
      }
      if (SUCCEEDED(hr)) rect_union(&drawn, &c);
    }
    mem_free(rects);
  }
  if (SUCCEEDED(hr) && !rect_empty(&drawn) && surface_is_visible_front(dst)) present_primary(dst, &drawn);
done:
  dd_unlock();
  return hr;
}

static HRESULT surface_blt_fast(Surface *dst, DWORD x, DWORD y, Surface *src, RECT *src_rect, DWORD trans) {
  if (!src) return DDERR_INVALIDPARAMS;
  if (trans & ~(DWORD)(DDBLTFAST_SRCCOLORKEY | DDBLTFAST_DESTCOLORKEY | DDBLTFAST_WAIT | DDBLTFAST_DONOTWAIT)) return DDERR_INVALIDPARAMS;
  RECT s;
  if (!src_rect) { rect_set(&s, 0, 0, (LONG)src->desc.dwWidth, (LONG)src->desc.dwHeight); src_rect = &s; }
  const DWORD dw = dst->desc.dwWidth, dh = dst->desc.dwHeight;
  if (src_rect->left < 0 || src_rect->top < 0 || src_rect->right < src_rect->left || src_rect->bottom < src_rect->top) return DDERR_INVALIDRECT;
  const DWORD sw = (DWORD)(src_rect->right - src_rect->left), sh = (DWORD)(src_rect->bottom - src_rect->top);
  if (sw > dw || x > dw - sw || sh > dh || y > dh - sh) return DDERR_INVALIDRECT;
  if (!sw || !sh) return DDERR_INVALIDRECT;
  if ((DWORD)src_rect->right > src->desc.dwWidth || (DWORD)src_rect->bottom > src->desc.dwHeight) return DDERR_INVALIDRECT;
  RECT dr;
  rect_set(&dr, (LONG)x, (LONG)y, (LONG)(x + sw), (LONG)(y + sh));
  dd_lock();
  HRESULT hr = DD_OK;
  if (dst->clipper) { hr = DDERR_BLTFASTCANTCLIP; goto done; }
  if (surface_is_lost(dst) || surface_is_lost(src)) { hr = DDERR_SURFACELOST; goto done; }
  if (surface_busy(dst) || surface_busy(src)) { hr = DDERR_SURFACEBUSY; goto done; }
  if (dd_format_is_depth(src->fmt) != dd_format_is_depth(dst->fmt)) { hr = DDERR_INVALIDPARAMS; goto done; }
  DdBlit b;
  zero(&b, sizeof b);
  if ((trans & DDBLTFAST_SRCCOLORKEY) && (src->desc.dwFlags & DDSD_CKSRCBLT)) {
    b.flags |= DD_BLIT_SRC_KEY;
    b.src_key_low = src->desc.ddckCKSrcBlt.dwColorSpaceLowValue;
    b.src_key_high = src->desc.ddckCKSrcBlt.dwColorSpaceHighValue;
  }
  if ((trans & DDBLTFAST_DESTCOLORKEY) && (dst->desc.dwFlags & DDSD_CKDESTBLT)) {
    b.flags |= DD_BLIT_DST_KEY;
    b.dst_key_low = dst->desc.ddckCKDestBlt.dwColorSpaceLowValue;
    b.dst_key_high = dst->desc.ddckCKDestBlt.dwColorSpaceHighValue;
  }
  hr = blit_rect(dst, &dr, src, src_rect, &b);
  if (SUCCEEDED(hr) && surface_is_visible_front(dst)) present_primary(dst, &dr);
done:
  dd_unlock();
  return hr;
}

/* BltBatch: Windows' DirectDraw has never implemented it (the documentation says so, and every version answers DDERR_UNSUPPORTED); programs fall
 * back to Blt. The same answer here. */
static HRESULT surface_blt_batch(Surface *s, DDBLTBATCH *batch, DWORD count, DWORD flags) {
  (void)s; (void)batch; (void)count; (void)flags;
  return DDERR_UNSUPPORTED;
}

/* ---- Flip ---------------------------------------------------------------------------------------------------------------------------------------------
 * The surfaces of the chain pass their memory on: the front buffer gets the first back buffer's, each back buffer the next one's, the last the
 * front's (with `target`: the front and that surface exchange theirs). The front buffer of the primary surface is shown, then the flip waits for
 * the vertical blank (DDFLIP_INTERVAL2..4: two to four of them; DDFLIP_NOVSYNC: none). */
static HRESULT surface_flip(Surface *front, Surface *target, DWORD flags, UINT version) {
  (void)version;
  if (target == front || !(front->desc.ddsCaps.dwCaps & (DDSCAPS_FRONTBUFFER | DDSCAPS_OVERLAY)) || !(front->desc.ddsCaps.dwCaps & DDSCAPS_FLIP))
    return DDERR_NOTFLIPPABLE;
  if (surface_is_lost(front)) return DDERR_SURFACELOST;
  if (flags & ~(DWORD)(DDFLIP_WAIT | DDFLIP_EVEN | DDFLIP_ODD | DDFLIP_NOVSYNC | DDFLIP_INTERVAL2 | DDFLIP_INTERVAL3 | DDFLIP_INTERVAL4 | DDFLIP_STEREO | DDFLIP_DONOTWAIT))
    return DDERR_INVALIDPARAMS;
  if ((flags & (DDFLIP_EVEN | DDFLIP_ODD)) && !(front->desc.ddsCaps.dwCaps & DDSCAPS_OVERLAY)) return DDERR_INVALIDPARAMS;
  if (flags & DDFLIP_STEREO) return DDERR_NOSTEREOHARDWARE;
  dd_lock();
  if ((front->desc.ddsCaps.dwCaps & DDSCAPS_PRIMARYSURFACE) && !(front->dd->coop & DDSCL_EXCLUSIVE)) { dd_unlock(); return DDERR_NOEXCLUSIVEMODE; }
  /* the chain from the front buffer on */
  Surface *chain[64];
  UINT n = 0;
  for (Surface *x = front; x && n < 64;) {
    chain[n++] = x;
    Surface *next = x->complex[0];
    if (!next || next == front) break;
    x = next;
  }
  if (n < 2) { dd_unlock(); return DDERR_NOTFLIPPABLE; }
  if (target) {
    BOOL found = FALSE;
    for (UINT i = 1; i < n; ++i) if (chain[i] == target) found = TRUE;
    if (!found) { dd_unlock(); return DDERR_NOTFLIPPABLE; }
  }
  for (UINT i = 0; i < n; ++i) if (surface_busy(chain[i])) { dd_unlock(); return DDERR_SURFACEBUSY; }
  if (target) {
    const SurfaceStore t = front->store;
    front->store = target->store;
    target->store = t;
    chain[1] = target;
    n = 2;
  } else {
    const SurfaceStore first = front->store;
    for (UINT i = 0; i + 1 < n; ++i) chain[i]->store = chain[i + 1]->store;
    chain[n - 1]->store = first;
  }
  for (UINT i = 0; i < n; ++i) {
    chain[i]->uniqueness = g_uniqueness++;
    if (!g_uniqueness) g_uniqueness = 1;
  }
  if (g_d3d.flipped) g_d3d.flipped(chain, n);
  const BOOL primary = (front->desc.ddsCaps.dwCaps & DDSCAPS_PRIMARYSURFACE) != 0;
  if (primary) present_primary(front, NULL);
  dd_unlock();
  if (primary && !(flags & DDFLIP_NOVSYNC)) {
    const UINT interval = (flags & DDFLIP_INTERVAL4) == DDFLIP_INTERVAL4 ? 4 : (flags & DDFLIP_INTERVAL3) == DDFLIP_INTERVAL3 ? 3
                        : (flags & DDFLIP_INTERVAL2) ? 2 : 1;
    for (UINT i = 0; i < interval; ++i) if (!display_wait_vblank()) break;
  }
  return DD_OK;
}

static HRESULT surface_get_blt_status(Surface *s, DWORD flags) {
  if (flags != DDGBS_CANBLT && flags != DDGBS_ISBLTDONE) return DDERR_INVALIDPARAMS;
  if (flags == DDGBS_CANBLT && surface_busy(s)) return DDERR_WASSTILLDRAWING;
  return DD_OK;
}
static HRESULT surface_get_flip_status(Surface *s, DWORD flags) {
  if (flags != DDGFS_CANFLIP && flags != DDGFS_ISFLIPDONE) return DDERR_INVALIDPARAMS;
  (void)s;
  return DD_OK;      /* (a flip is done when Flip returns) */
}
