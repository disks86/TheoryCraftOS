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

/* Part of d3d9.c: clears and copies - Clear (of rectangles, in the viewport and the scissor rectangle), ColorFill, StretchRect, UpdateSurface, UpdateTexture,
 * GetRenderTargetData.
 *
 * What Direct3D 12 below cannot do itself - clear a part of a target, scale - is a draw of the device's own: a rectangle of vertices that are pixels, with
 * the device's state put aside for it (own_draw_begin .. own_draw_end), its colour (and depth and stencil) written where the rectangle is. Copies of one size
 * are Direct3D 12 copies, and copies to and from memory go through the CPU. */

static Surface *surface_of(IDirect3DSurface9 *surface) {
  return (Surface *)tcos_own(surface, &surf_vtbl);      /* (this library's, or a proxy's wrapper of one: tcos_object.h) */
}

/* ---- the device's own draws ------------------------------------------------------------------------------------------------------------------------- */

typedef struct OwnDraw {
  State *saved;
  Surface *target, *depth, *extra[TARGETS - 1];
} OwnDraw;
/* Puts the device's state aside (it is restored as it was: nothing the program set changes) and draws into `target` (and `depth`) from now on, in the
 * viewport `viewport` with the scissor rectangle `scissor` (NULL: none), with the fixed-function stages and none of the program's shaders. */
static HRESULT own_draw_begin(Dev *d, OwnDraw *o, Surface *target, Surface *depth, const D3DVIEWPORT9 *viewport, const RECT *scissor) {
  Decl *decl = fvf_decl(d, D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1);
  o->saved = decl ? mem_alloc(sizeof(State)) : NULL;
  if (!o->saved) return E_OUTOFMEMORY;
  memcpy(o->saved, &d->state, sizeof(State));
  o->target = d->target[0];
  o->depth = d->depth;
  State *s = &d->state;
  /* (raw pointers: they are put back before anyone sees them, so the counts of what is set do not change; the device's own draws write one target) */
  d->target[0] = target;
  d->depth = depth;
  for (UINT i = 1; i < TARGETS; ++i) {
    o->extra[i - 1] = d->target[i];
    d->target[i] = NULL;
  }
  d->targets_dirty = 1;
  s->vs = s->ps = NULL;
  s->decl = decl;
  s->fvf = D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1;
  for (UINT i = 0; i < SAMPLERS; ++i) s->tex[i] = NULL;
  DWORD *rs = s->rs;
  rs[D3DRS_ZENABLE] = D3DZB_FALSE;
  rs[D3DRS_ZWRITEENABLE] = FALSE;
  rs[D3DRS_ZFUNC] = D3DCMP_ALWAYS;
  rs[D3DRS_STENCILENABLE] = FALSE;
  rs[D3DRS_TWOSIDEDSTENCILMODE] = FALSE;
  rs[D3DRS_ALPHABLENDENABLE] = rs[D3DRS_ALPHATESTENABLE] = rs[D3DRS_FOGENABLE] = rs[D3DRS_SPECULARENABLE] = rs[D3DRS_LIGHTING] = FALSE;
  rs[D3DRS_CULLMODE] = D3DCULL_NONE;
  rs[D3DRS_FILLMODE] = D3DFILL_SOLID;
  rs[D3DRS_DEPTHBIAS] = rs[D3DRS_SLOPESCALEDEPTHBIAS] = 0;
  rs[D3DRS_COLORWRITEENABLE] = 0xF;
  rs[D3DRS_CLIPPLANEENABLE] = 0;
  rs[D3DRS_SRGBWRITEENABLE] = FALSE;
  rs[D3DRS_SCISSORTESTENABLE] = scissor != NULL;
  if (scissor) s->scissor = *scissor;
  s->viewport = *viewport;
  for (UINT i = 0; i < STAGES; ++i) {
    s->stage[i][D3DTSS_COLOROP] = s->stage[i][D3DTSS_ALPHAOP] = i == 0 ? D3DTOP_SELECTARG1 : D3DTOP_DISABLE;
    s->stage[i][D3DTSS_COLORARG1] = s->stage[i][D3DTSS_ALPHAARG1] = D3DTA_DIFFUSE;
    s->stage[i][D3DTSS_RESULTARG] = D3DTA_CURRENT;
    s->stage[i][D3DTSS_TEXCOORDINDEX] = i;
    s->stage[i][D3DTSS_TEXTURETRANSFORMFLAGS] = D3DTTFF_DISABLE;
  }
  d->targets_dirty = 1;
  return D3D_OK;
}
static void own_draw_end(Dev *d, OwnDraw *o) {
  memcpy(&d->state, o->saved, sizeof(State));
  d->target[0] = o->target;
  d->depth = o->depth;
  for (UINT i = 1; i < TARGETS; ++i) d->target[i] = o->extra[i - 1];
  d->targets_dirty = 1;
  mem_free(o->saved);
}
/* A rectangle of vertices that are pixels from (x0, y0) to (x1, y1) - the edges of the pixels: Direct3D 9 has their centres at whole numbers - at depth z, in a
 * colour, with texture coordinates u0 v0 .. u1 v1. */
typedef struct OwnVertex { float x, y, z, rhw; DWORD colour; float u, v; } OwnVertex;
static HRESULT own_quad(Dev *d, float x0, float y0, float x1, float y1, float z, D3DCOLOR colour, float u0, float v0, float u1, float v1) {
  const OwnVertex v[4] = {{x0 - 0.5f, y0 - 0.5f, z, 1, colour, u0, v0}, {x1 - 0.5f, y0 - 0.5f, z, 1, colour, u1, v0}, {x0 - 0.5f, y1 - 0.5f, z, 1, colour, u0, v1},
                          {x1 - 0.5f, y1 - 0.5f, z, 1, colour, u1, v1}};
  Source sources[STREAMS];
  memset(sources, 0, sizeof sources);
  sources[0].data = (const BYTE *)v;
  sources[0].bytes = sizeof v;
  sources[0].stride = sizeof v[0];
  return draw(d, D3DPT_TRIANGLESTRIP, 2, sources, 0, 0, NULL, 0, 0);
}

/* ---- Clear ------------------------------------------------------------------------------------------------------------------------------------------ */

static RECT rect_meet(RECT a, const RECT *b) {
  if (b->left > a.left) a.left = b->left;
  if (b->top > a.top) a.top = b->top;
  if (b->right < a.right) a.right = b->right;
  if (b->bottom < a.bottom) a.bottom = b->bottom;
  if (a.right < a.left) a.right = a.left;
  if (a.bottom < a.top) a.bottom = a.top;
  return a;
}
static BOOL rect_empty(const RECT *r) { return r->right <= r->left || r->bottom <= r->top; }
static D3DCOLOR colour_float(const float c[4]) {
  BYTE b[4];
  for (int i = 0; i < 4; ++i) b[i] = (BYTE)(c[i] <= 0.0f ? 0 : c[i] >= 1.0f ? 255 : (int)(c[i] * 255.0f + 0.5f));
  return (D3DCOLOR)b[3] << 24 | (D3DCOLOR)b[0] << 16 | (D3DCOLOR)b[1] << 8 | b[2];
}
/* The whole target in one colour, and the depth buffer (all of it) in one depth and stencil: Direct3D 12's clears. */
static void clear_whole(Dev *d, Surface *target_surface, Tex *depth, DWORD flags, D3DCOLOR colour, float z, DWORD stencil) {
  if ((flags & D3DCLEAR_TARGET) && target_surface) {
    Tex *target = target_surface->tex;
    const float c[4] = {((colour >> 16) & 255) / 255.0f, ((colour >> 8) & 255) / 255.0f, (colour & 255) / 255.0f, (colour >> 24) / 255.0f};
    const D3D12_CPU_DESCRIPTOR_HANDLE view = surface_target_view(d, target_surface);
    if (view.ptr) {
      ID3D12GraphicsCommandList_ClearRenderTargetView(d->list, view, c, 0, NULL);
      target->used = d->epoch;
      d->recorded = 1;
    }
  }
  if ((flags & (D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL)) && depth) {
    /* Direct3D 12 here clears a depth buffer when the next pass with it begins, and forgets the clear with the list: it is kept until a draw has followed it,
     * and recorded again in every list until then. */
    const UINT what = ((flags & D3DCLEAR_ZBUFFER) ? D3D12_CLEAR_FLAG_DEPTH : 0) | ((flags & D3DCLEAR_STENCIL) ? D3D12_CLEAR_FLAG_STENCIL : 0);
    UINT at = d->clear_count;
    for (UINT i = 0; i < d->clear_count; ++i) if (d->clears[i].depth == depth) at = i;
    if (at == d->clear_count && d->clear_count < 4) ++d->clear_count;
    if (at < 4) {
      PendingClear *p = &d->clears[at];
      p->flags = (p->depth == depth ? p->flags : 0) | what;
      p->depth = depth;
      if (flags & D3DCLEAR_ZBUFFER) p->z = z;
      if (flags & D3DCLEAR_STENCIL) p->stencil = (UINT8)stencil;
    }
    ID3D12GraphicsCommandList_ClearDepthStencilView(d->list, depth->view, (D3D12_CLEAR_FLAGS)what, z, (UINT8)stencil, 0, NULL);
    depth->used = d->epoch;
    d->recorded = 1;
  }
}
/* Rectangles of the target (and the depth buffer) in one colour (depth, stencil): draws of the device's own, each its rectangle by the scissor rectangle. */
static HRESULT clear_rects(Dev *d, Surface *target, Surface *depth, const RECT *rects, UINT count, DWORD flags, D3DCOLOR colour, float z, DWORD stencil, const D3DVIEWPORT9 *viewport) {
  OwnDraw o;
  RECT first = rects[0];
  HRESULT hr = own_draw_begin(d, &o, target, depth, viewport, &first);
  if (FAILED(hr)) return hr;
  DWORD *rs = d->state.rs;
  const BOOL zs = (flags & (D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL)) && depth;
  rs[D3DRS_ZENABLE] = zs ? D3DZB_TRUE : D3DZB_FALSE;      /* (a depth buffer is written only with its test on: it passes always) */
  rs[D3DRS_ZWRITEENABLE] = (flags & D3DCLEAR_ZBUFFER) != 0;
  rs[D3DRS_STENCILENABLE] = (flags & D3DCLEAR_STENCIL) != 0 && depth;
  rs[D3DRS_STENCILFUNC] = D3DCMP_ALWAYS;
  rs[D3DRS_STENCILFAIL] = rs[D3DRS_STENCILZFAIL] = rs[D3DRS_STENCILPASS] = D3DSTENCILOP_REPLACE;
  rs[D3DRS_STENCILREF] = stencil & 0xFF;
  rs[D3DRS_STENCILWRITEMASK] = 0xFF;
  rs[D3DRS_COLORWRITEENABLE] = (flags & D3DCLEAR_TARGET) ? 0xF : 0;
  if (!(flags & D3DCLEAR_TARGET)) d->depth = depth;      /* (a pass needs a colour target: the target is there, with nothing written) */
  const float x0 = (float)viewport->X, y0 = (float)viewport->Y, x1 = x0 + (float)viewport->Width, y1 = y0 + (float)viewport->Height;
  const float depth_value = z < 0.0f ? 0.0f : z > 1.0f ? 1.0f : z;
  for (UINT i = 0; i < count && SUCCEEDED(hr); ++i) {
    if (rect_empty(&rects[i])) continue;
    d->state.scissor = rects[i];
    d->targets_dirty = 1;
    hr = own_quad(d, x0, y0, x1, y1, depth_value, colour, 0, 0, 1, 1);
  }
  own_draw_end(d, &o);
  return hr;
}
static HRESULT STDMETHODCALLTYPE dev_Clear(Dev *d, DWORD rect_count, const D3DRECT *rects, DWORD flags, D3DCOLOR colour, float z, DWORD stencil) {
  if (flags & ~(DWORD)(D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL)) return D3DERR_INVALIDCALL;
  Surface *depth = d->depth;
  if ((flags & (D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL)) && !depth) return D3DERR_INVALIDCALL;
  if ((flags & D3DCLEAR_STENCIL) && !format_has_stencil(depth->tex->format)) return D3DERR_INVALIDCALL;
  if (rect_count && !rects) rect_count = 0;      /* (a count with no rectangles: all of the viewport, as Direct3D 9 takes it) */
  else if (!rect_count && rects) return D3D_OK;  /* (rectangles with no count: nothing) */
  if (!flags || !d->target[0]) return D3D_OK;
  EnterCriticalSection(&d->lock);
  Tex *target = d->target[0]->tex;
  /* What is cleared: the viewport (in the target), and of that the rectangles and the scissor rectangle. Rectangles that are upside down are dropped. */
  const D3DVIEWPORT9 *vp = &d->state.viewport;
  RECT area = {(LONG)vp->X, (LONG)vp->Y, (LONG)(vp->X + vp->Width), (LONG)(vp->Y + vp->Height)};
  const RECT whole = {0, 0, (LONG)sub_width(target, d->target[0]->sub), (LONG)sub_height(target, d->target[0]->sub)};
  area = rect_meet(area, &whole);
  if (d->state.rs[D3DRS_SCISSORTESTENABLE]) area = rect_meet(area, &d->state.scissor);
  RECT *list = NULL;
  UINT count = 0;
  HRESULT hr = D3D_OK;
  if (rect_count) {
    list = mem_alloc(sizeof(RECT) * rect_count);
    if (!list) { LeaveCriticalSection(&d->lock); return E_OUTOFMEMORY; }
    for (UINT i = 0; i < rect_count; ++i) {
      if (rects[i].x1 > rects[i].x2 || rects[i].y1 > rects[i].y2) continue;
      const RECT r = {rects[i].x1, rects[i].y1, rects[i].x2, rects[i].y2};
      list[count] = rect_meet(r, &area);
      if (!rect_empty(&list[count])) ++count;
    }
  } else if (!rect_empty(&area)) {
    list = mem_alloc(sizeof(RECT));
    if (!list) { LeaveCriticalSection(&d->lock); return E_OUTOFMEMORY; }
    list[0] = area;
    count = 1;
  }
  /* (one rectangle that is all of the target, and a depth buffer of the target's size: Direct3D 12's clears) */
  const BOOL depth_whole = !(flags & (D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL)) || (depth->tex->width == (UINT)whole.right && depth->tex->height == (UINT)whole.bottom);
  if (count == 1 && EqualRect(&list[0], &whole) && depth_whole) clear_whole(d, d->target[0], depth ? depth->tex : NULL, flags, colour, z, stencil);
  else if (count) {
    const D3DVIEWPORT9 full = {0, 0, (DWORD)whole.right, (DWORD)whole.bottom, 0.0f, 1.0f};
    hr = clear_rects(d, d->target[0], (flags & (D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL)) ? depth : NULL, list, count, flags, colour, z, stencil, &full);
  }
  /* (the other render targets that are set get the colour too, in the same rectangles of theirs) */
  for (UINT i = 1; i < TARGETS && SUCCEEDED(hr) && (flags & D3DCLEAR_TARGET) && count; ++i) {
    Surface *other = d->target[i];
    if (!other) continue;
    const RECT all = {0, 0, (LONG)sub_width(other->tex, other->sub), (LONG)sub_height(other->tex, other->sub)};
    if (count == 1 && EqualRect(&list[0], &all)) clear_whole(d, other, NULL, D3DCLEAR_TARGET, colour, 0, 0);
    else {
      UINT kept = 0;
      for (UINT k = 0; k < count; ++k) {
        list[kept] = rect_meet(list[k], &all);
        if (!rect_empty(&list[kept])) ++kept;
      }
      count = kept;
      const D3DVIEWPORT9 full = {0, 0, (DWORD)all.right, (DWORD)all.bottom, 0.0f, 1.0f};
      if (count) hr = clear_rects(d, other, NULL, list, count, D3DCLEAR_TARGET, colour, 0, 0, &full);
    }
  }
  mem_free(list);
  LeaveCriticalSection(&d->lock);
  return hr;
}

/* ---- pixels of surfaces, wherever they are ---------------------------------------------------------------------------------------------------------- */

/* A rectangle of a surface as B8G8R8A8 rows (`pitch` bytes apart): from the device for what it owns, else from the program's copy. */
static HRESULT surface_read(Dev *d, Surface *s, const RECT *r, BYTE *out, UINT pitch) {
  Tex *t = s->tex;
  const UINT w = sub_width(t, s->sub), h = sub_height(t, s->sub), rw = (UINT)(r->right - r->left);
  if (tex_device_owned(t)) {
    const UINT gb = gpu_bytes(t->format);
    BYTE *all = mem_alloc((SIZE_T)w * h * gb);
    if (!all) return E_OUTOFMEMORY;
    const HRESULT hr = tex_download(d, t, s->sub, all, w * gb);
    if (SUCCEEDED(hr))
      for (LONG y = r->top; y < r->bottom; ++y) {
        BYTE *row = all + ((SIZE_T)y * w + (SIZE_T)r->left) * gb;
        if (format_is_wide(t->format)) {
          row_from_gpu(t->format, row, rw, row);
          row_to_bgra(t->format, row, rw, out + (SIZE_T)(y - r->top) * pitch, NULL);
        } else memcpy(out + (SIZE_T)(y - r->top) * pitch, row, (SIZE_T)rw * 4);
      }
    mem_free(all);
    return hr;
  }
  const BYTE *shadow = tex_shadow(t, s->sub);
  if (!shadow) return E_OUTOFMEMORY;
  const UINT bytes = format_bytes(t->format), row_bytes = format_pitch(t->format, w);
  for (LONG y = r->top; y < r->bottom; ++y)
    row_to_bgra(t->format, shadow + (SIZE_T)y * row_bytes + (SIZE_T)r->left * bytes, rw, out + (SIZE_T)(y - r->top) * pitch, current_palette(d));
  return D3D_OK;
}
/* B8G8R8A8 rows into a rectangle of a surface: to the device for what it owns, else into the program's copy (and the device's copy of it). */
static HRESULT surface_write(Dev *d, Surface *s, const RECT *r, const BYTE *in, UINT pitch) {
  Tex *t = s->tex;
  const UINT w = sub_width(t, s->sub), rw = (UINT)(r->right - r->left);
  if (tex_device_owned(t)) {
    if (!format_is_wide(t->format)) return tex_upload_rows(d, t, s->sub, r, in, pitch, 0);
    const UINT gb = gpu_bytes(t->format), rh = (UINT)(r->bottom - r->top);
    BYTE *rows = mem_alloc((SIZE_T)rw * rh * gb);
    if (!rows) return E_OUTOFMEMORY;
    for (UINT y = 0; y < rh; ++y) {
      row_from_bgra(t->format, in + (SIZE_T)y * pitch, rw, rows + (SIZE_T)y * rw * gb);
      row_to_gpu(t->format, rows + (SIZE_T)y * rw * gb, rw, rows + (SIZE_T)y * rw * gb);
    }
    const HRESULT hr = tex_upload_rows(d, t, s->sub, r, rows, rw * gb, 0);
    mem_free(rows);
    return hr;
  }
  BYTE *shadow = tex_shadow(t, s->sub);
  if (!shadow) return E_OUTOFMEMORY;
  const UINT bytes = format_bytes(t->format), row_bytes = format_pitch(t->format, w);
  for (LONG y = r->top; y < r->bottom; ++y) row_from_bgra(t->format, in + (SIZE_T)(y - r->top) * pitch, rw, shadow + (SIZE_T)y * row_bytes + (SIZE_T)r->left * bytes);
  if (sub_level(t, s->sub) == 0) tex_add_dirty(t, t->kind == 3 ? sub_face(t, s->sub) : 0, 0, r);
  return tex_upload(d, t, s->sub);
}
/* Copies the program's pixels of a rectangle into another surface of the same format, as they are (a compressed format's blocks too): `r` of `from` to `at`. */
static HRESULT surface_copy_raw(Dev *d, Surface *from, const RECT *r, Surface *to, POINT at) {
  Tex *ft = from->tex, *tt = to->tex;
  const BYTE *source = tex_shadow(ft, from->sub);
  BYTE *dest = tex_shadow(tt, to->sub);
  if (!source || !dest) return E_OUTOFMEMORY;
  const UINT block = dxt_block(ft->format), bytes = format_bytes(ft->format);
  const UINT from_pitch = format_pitch(ft->format, sub_width(ft, from->sub)), to_pitch = format_pitch(tt->format, sub_width(tt, to->sub));
  const UINT rows = block ? (UINT)(r->bottom - r->top + 3) / 4 : (UINT)(r->bottom - r->top);
  const UINT row_bytes = block ? (UINT)(r->right - r->left + 3) / 4 * block : (UINT)(r->right - r->left) * bytes;
  const SIZE_T from_at = block ? (SIZE_T)(r->top / 4) * from_pitch + (SIZE_T)(r->left / 4) * block : (SIZE_T)r->top * from_pitch + (SIZE_T)r->left * bytes;
  const SIZE_T to_at = block ? (SIZE_T)(at.y / 4) * to_pitch + (SIZE_T)(at.x / 4) * block : (SIZE_T)at.y * to_pitch + (SIZE_T)at.x * bytes;
  for (UINT row = 0; row < rows; ++row) memcpy(dest + to_at + (SIZE_T)row * to_pitch, source + from_at + (SIZE_T)row * from_pitch, row_bytes);
  if (tex_device_owned(tt)) return tex_upload(d, tt, to->sub);
  const RECT dirty = {at.x, at.y, at.x + (r->right - r->left), at.y + (r->bottom - r->top)};
  if (sub_level(tt, to->sub) == 0) tex_add_dirty(tt, tt->kind == 3 ? sub_face(tt, to->sub) : 0, 0, &dirty);
  return tex_upload(d, tt, to->sub);
}

/* ---- ColorFill -------------------------------------------------------------------------------------------------------------------------------------- */

/* A rectangle of a target or of an off-screen plain surface of the default pool in one colour (not of a texture that is not drawn into). */
static HRESULT STDMETHODCALLTYPE dev_ColorFill(Dev *d, IDirect3DSurface9 *surface, const RECT *rect, D3DCOLOR colour) {
  Surface *s = surface_of(surface);
  if (!s || s->c.dev != d) return D3DERR_INVALIDCALL;
  Tex *t = s->tex;
  if (t->pool != D3DPOOL_DEFAULT || t->depth || (s->kind == SURFACE_LEVEL && !t->target) || dxt_block(t->format) || format_is_palette(t->format)) return D3DERR_INVALIDCALL;
  const UINT w = sub_width(t, s->sub), h = sub_height(t, s->sub);
  RECT r = {0, 0, (LONG)w, (LONG)h};
  if (rect) {
    if (rect->left < 0 || rect->top < 0 || rect->right > (LONG)w || rect->bottom > (LONG)h || rect->left >= rect->right || rect->top >= rect->bottom) return D3DERR_INVALIDCALL;
    r = *rect;
  }
  HRESULT hr = D3D_OK;
  EnterCriticalSection(&d->lock);
  if (t->target && t->res) {
    if (r.left == 0 && r.top == 0 && r.right == (LONG)w && r.bottom == (LONG)h && surface_drawable(s)) clear_whole(d, s, NULL, D3DCLEAR_TARGET, colour, 0, 0);
    else {
      const D3DVIEWPORT9 full = {0, 0, w, h, 0.0f, 1.0f};
      hr = clear_rects(d, s, NULL, &r, 1, D3DCLEAR_TARGET, colour, 0, 0, &full);
    }
  } else {
    /* (memory: the colour as the surface's format, row by row) */
    BYTE pixel[4];
    colour_to_bgra(colour, pixel);
    const UINT rw = (UINT)(r.right - r.left);
    BYTE *row = mem_alloc((SIZE_T)rw * 4);
    if (!row) hr = E_OUTOFMEMORY;
    else {
      for (UINT x = 0; x < rw; ++x) memcpy(row + 4 * x, pixel, 4);
      BYTE *all = mem_alloc((SIZE_T)rw * 4 * (UINT)(r.bottom - r.top));
      if (!all) hr = E_OUTOFMEMORY;
      else {
        for (LONG y = r.top; y < r.bottom; ++y) memcpy(all + (SIZE_T)(y - r.top) * rw * 4, row, (SIZE_T)rw * 4);
        hr = surface_write(d, s, &r, all, rw * 4);
        mem_free(all);
      }
      mem_free(row);
    }
  }
  LeaveCriticalSection(&d->lock);
  return hr;
}

/* ---- StretchRect ------------------------------------------------------------------------------------------------------------------------------------ */

/* Scales a rectangle of B8G8R8A8 pixels: `filter` D3DTEXF_LINEAR between the four nearest (clamped at the edges), the others the nearest one. */
static void scale_pixels(const BYTE *from, UINT fw, UINT fh, UINT from_pitch, BYTE *to, UINT tw, UINT th, UINT to_pitch, D3DTEXTUREFILTERTYPE filter) {
  for (UINT y = 0; y < th; ++y)
    for (UINT x = 0; x < tw; ++x) {
      BYTE *out = to + (SIZE_T)y * to_pitch + (SIZE_T)x * 4;
      if (filter != D3DTEXF_LINEAR) {
        const UINT sx = (UINT)(((UINT64)x * fw * 2 + fw) / (tw * 2)), sy = (UINT)(((UINT64)y * fh * 2 + fh) / (th * 2));
        memcpy(out, from + (SIZE_T)(sy < fh ? sy : fh - 1) * from_pitch + (SIZE_T)(sx < fw ? sx : fw - 1) * 4, 4);
        continue;
      }
      /* (the place of the pixel's centre in the source, less half a pixel: what is between the centres around it) */
      const float fx = ((float)x + 0.5f) * (float)fw / (float)tw - 0.5f, fy = ((float)y + 0.5f) * (float)fh / (float)th - 0.5f;
      int x0 = (int)(fx < 0 ? fx - 1 : fx), y0 = (int)(fy < 0 ? fy - 1 : fy);
      const float ax = fx - (float)x0, ay = fy - (float)y0;
      int x1 = x0 + 1, y1 = y0 + 1;
      if (x0 < 0) x0 = 0;
      if (y0 < 0) y0 = 0;
      if (x1 > (int)fw - 1) x1 = (int)fw - 1;
      if (y1 > (int)fh - 1) y1 = (int)fh - 1;
      if (x0 > (int)fw - 1) x0 = (int)fw - 1;
      if (y0 > (int)fh - 1) y0 = (int)fh - 1;
      for (int c = 0; c < 4; ++c) {
        const float p00 = from[(SIZE_T)y0 * from_pitch + (SIZE_T)x0 * 4 + c], p10 = from[(SIZE_T)y0 * from_pitch + (SIZE_T)x1 * 4 + c];
        const float p01 = from[(SIZE_T)y1 * from_pitch + (SIZE_T)x0 * 4 + c], p11 = from[(SIZE_T)y1 * from_pitch + (SIZE_T)x1 * 4 + c];
        const float v = (p00 * (1 - ax) + p10 * ax) * (1 - ay) + (p01 * (1 - ax) + p11 * ax) * ay;
        out[c] = (BYTE)(v + 0.5f);
      }
    }
}
/* A rectangle checked against a surface's size; NULL is all of it. FALSE: it does not fit. */
static BOOL rect_in_surface(const Surface *s, const RECT *r, RECT *out) {
  const UINT w = sub_width(s->tex, s->sub), h = sub_height(s->tex, s->sub);
  if (!r) { SetRect(out, 0, 0, (int)w, (int)h); return TRUE; }
  if (r->left < 0 || r->top < 0 || r->right > (LONG)w || r->bottom > (LONG)h || r->left >= r->right || r->top >= r->bottom) return FALSE;
  *out = *r;
  return TRUE;
}
/* StretchRect: a rectangle of a target, an off-screen plain surface or a level of a texture of the default pool into a rectangle of a target (or of another
 * off-screen plain surface of the default pool), of another size and format if need be; depth buffers whole, of one size and format, outside a scene. Within
 * the device a copy of one size and format is Direct3D 12's; one that scales or converts, from what the device has, is a draw of the device's own sampling the
 * source (D3DTEXF_POINT or D3DTEXF_LINEAR); from memory it is the CPU's. */
static HRESULT STDMETHODCALLTYPE dev_StretchRect(Dev *d, IDirect3DSurface9 *from, const RECT *from_rect, IDirect3DSurface9 *to, const RECT *to_rect, D3DTEXTUREFILTERTYPE filter) {
  Surface *source = surface_of(from), *dest = surface_of(to);
  if (!source || !dest || source->c.dev != d || dest->c.dev != d) return D3DERR_INVALIDCALL;
  if (filter != D3DTEXF_NONE && filter != D3DTEXF_POINT && filter != D3DTEXF_LINEAR) return D3DERR_INVALIDCALL;
  Tex *st = source->tex, *dt = dest->tex;
  if (st->pool != D3DPOOL_DEFAULT || dt->pool != D3DPOOL_DEFAULT) return D3DERR_INVALIDCALL;
  const BOOL source_plain = source->kind == SURFACE_ALONE && !st->target && !st->depth, dest_plain = dest->kind == SURFACE_ALONE && !dt->target && !dt->depth;
  /* (into a target, or from a plain surface into a plain surface; a texture that is not drawn into is no destination, and no source but into a target) */
  if (!dt->target && !dt->depth && !(dest_plain && source_plain)) return D3DERR_INVALIDCALL;
  if (dest->kind == SURFACE_LEVEL && !dt->target && !dt->depth) return D3DERR_INVALIDCALL;
  if (source->kind == SURFACE_LEVEL && !st->target && !dt->target) return D3DERR_INVALIDCALL;
  if (dt->target && source_plain && st->kind != 0) return D3DERR_INVALIDCALL;
  if (dest_plain && !source_plain) return D3DERR_INVALIDCALL;
  if (dxt_block(st->format) && dt->target) return D3DERR_INVALIDCALL;
  RECT sr, dr;
  if (!rect_in_surface(source, from_rect, &sr) || !rect_in_surface(dest, to_rect, &dr)) return D3DERR_INVALIDCALL;
  const UINT sw = (UINT)(sr.right - sr.left), sh = (UINT)(sr.bottom - sr.top), dw = (UINT)(dr.right - dr.left), dh = (UINT)(dr.bottom - dr.top);
  if (st == dt && source->sub == dest->sub && sr.left < dr.right && dr.left < sr.right && sr.top < dr.bottom && dr.top < sr.bottom) return D3DERR_INVALIDCALL;      /* (overlapping) */
  if (st->depth || dt->depth) {
    /* (depth buffers: whole, one size and format, none of a rectangle, outside BeginScene .. EndScene) */
    if (!st->depth || !dt->depth || from_rect || to_rect || st->format != dt->format || st->width != dt->width || st->height != dt->height || d->in_scene) return D3DERR_INVALIDCALL;
    EnterCriticalSection(&d->lock);
    for (UINT i = 0; i < d->clear_count; ++i) if (d->clears[i].depth == st) flush(d);      /* (its clear comes first) */
    ID3D12GraphicsCommandList_CopyResource(d->list, dt->res, st->res);
    st->used = dt->used = d->epoch;
    d->recorded = 1;
    LeaveCriticalSection(&d->lock);
    return D3D_OK;
  }
  if (format_is_palette(dt->format) || dxt_block(dt->format)) return D3DERR_INVALIDCALL;
  HRESULT hr = D3D_OK;
  EnterCriticalSection(&d->lock);
  if (st->res && dt->res && sw == dw && sh == dh && st->kind == 0 && dt->kind == 0 && !dxt_block(st->format) && gpu_dxgi(st->format) == gpu_dxgi(dt->format)) {
    /* (one size and the same Direct3D 12 format: Direct3D 12's copy) */
    settle(d, dt->sampled);
    D3D12_TEXTURE_COPY_LOCATION a, b;
    memset(&a, 0, sizeof a);
    memset(&b, 0, sizeof b);
    a.pResource = dt->res;
    a.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    a.SubresourceIndex = tex_d12_sub(dt, dest->sub);
    b.pResource = st->res;
    b.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    b.SubresourceIndex = tex_d12_sub(st, source->sub);
    const D3D12_BOX box = {(UINT)sr.left, (UINT)sr.top, 0, (UINT)sr.right, (UINT)sr.bottom, 1};
    ID3D12GraphicsCommandList_CopyTextureRegion(d->list, &a, (UINT)dr.left, (UINT)dr.top, 0, &b, &box);
    st->used = dt->used = d->epoch;
    d->recorded = 1;
  } else if (st->res && dt->target && st->kind == 0 && sub_level(st, source->sub) == 0 && !dxt_block(st->format) && st != dt) {
    /* (scaled, from the device: a rectangle of the destination sampling the source) */
    OwnDraw o;
    const D3DVIEWPORT9 full = {0, 0, sub_width(dt, dest->sub), sub_height(dt, dest->sub), 0.0f, 1.0f};
    hr = own_draw_begin(d, &o, dest, NULL, &full, &dr);
    if (SUCCEEDED(hr)) {
      State *s = &d->state;
      s->tex[0] = st;
      s->stage[0][D3DTSS_COLORARG1] = s->stage[0][D3DTSS_ALPHAARG1] = D3DTA_TEXTURE;
      s->sampler[0][D3DSAMP_ADDRESSU] = s->sampler[0][D3DSAMP_ADDRESSV] = D3DTADDRESS_CLAMP;
      s->sampler[0][D3DSAMP_MAGFILTER] = s->sampler[0][D3DSAMP_MINFILTER] = filter == D3DTEXF_LINEAR ? D3DTEXF_LINEAR : D3DTEXF_POINT;
      s->sampler[0][D3DSAMP_MIPFILTER] = D3DTEXF_NONE;
      s->sampler[0][D3DSAMP_SRGBTEXTURE] = 0;
      const float u0 = (float)sr.left / (float)st->width, v0 = (float)sr.top / (float)st->height, u1 = (float)sr.right / (float)st->width, v1 = (float)sr.bottom / (float)st->height;
      hr = own_quad(d, (float)dr.left, (float)dr.top, (float)dr.right, (float)dr.bottom, 0.0f, 0xFFFFFFFFu, u0, v0, u1, v1);
      own_draw_end(d, &o);
    }
  } else if (sw == dw && sh == dh && st->format == dt->format && !tex_device_owned(st) && !tex_device_owned(dt)) {
    const POINT at = {dr.left, dr.top};
    hr = surface_copy_raw(d, source, &sr, dest, at);
  } else {
    /* (through the CPU: the source's pixels, scaled, into the destination) */
    BYTE *pixels = mem_alloc((SIZE_T)sw * sh * 4), *scaled = (sw == dw && sh == dh) ? NULL : mem_alloc((SIZE_T)dw * dh * 4);
    if (!pixels || (!scaled && !(sw == dw && sh == dh))) hr = E_OUTOFMEMORY;
    if (SUCCEEDED(hr)) hr = surface_read(d, source, &sr, pixels, sw * 4);
    if (SUCCEEDED(hr) && scaled) scale_pixels(pixels, sw, sh, sw * 4, scaled, dw, dh, dw * 4, filter);
    if (SUCCEEDED(hr)) hr = surface_write(d, dest, &dr, scaled ? scaled : pixels, dw * 4);
    mem_free(pixels);
    mem_free(scaled);
  }
  LeaveCriticalSection(&d->lock);
  return hr;
}

/* ---- UpdateSurface, UpdateTexture, GetRenderTargetData ---------------------------------------------------------------------------------------------- */

/* A rectangle of a surface in system memory to a place of one of the default pool, of the same format; none of them a depth buffer or a target of more than
 * one sample. Compressed formats copy whole blocks. */
static HRESULT STDMETHODCALLTYPE dev_UpdateSurface(Dev *d, IDirect3DSurface9 *from, const RECT *rect, IDirect3DSurface9 *to, const POINT *point) {
  Surface *source = surface_of(from), *dest = surface_of(to);
  if (!source || !dest || source->c.dev != d || dest->c.dev != d) return D3DERR_INVALIDCALL;
  Tex *st = source->tex, *dt = dest->tex;
  if (st->pool != D3DPOOL_SYSTEMMEM || dt->pool != D3DPOOL_DEFAULT || st->format != dt->format || st->depth || dt->depth || st->samples || dt->samples) return D3DERR_INVALIDCALL;
  RECT sr;
  if (!rect_in_surface(source, rect, &sr)) return D3DERR_INVALIDCALL;
  const POINT at = point ? *point : (POINT){0, 0};
  const UINT dw = sub_width(dt, dest->sub), dh = sub_height(dt, dest->sub), block = dxt_block(st->format);
  if (at.x < 0 || at.y < 0 || (UINT)at.x > dw || (UINT)at.y > dh || (UINT)(sr.right - sr.left) > dw - (UINT)at.x || (UINT)(sr.bottom - sr.top) > dh - (UINT)at.y) return D3DERR_INVALIDCALL;
  if (block) {
    const UINT sw = sub_width(st, source->sub), sh = sub_height(st, source->sub);
    if ((sr.left & 3) || (sr.top & 3) || (at.x & 3) || (at.y & 3) || ((sr.right & 3) && (UINT)sr.right != sw) || ((sr.bottom & 3) && (UINT)sr.bottom != sh)) return D3DERR_INVALIDCALL;
  }
  if (!rect && !point && (sub_width(st, source->sub) != dw || sub_height(st, source->sub) != dh)) return D3DERR_INVALIDCALL;
  EnterCriticalSection(&d->lock);
  HRESULT hr;
  if (!tex_device_owned(dt) || block) hr = surface_copy_raw(d, source, &sr, dest, at);
  else {
    const UINT rw = (UINT)(sr.right - sr.left), rh = (UINT)(sr.bottom - sr.top);
    BYTE *pixels = mem_alloc((SIZE_T)rw * rh * 4);
    hr = pixels ? surface_read(d, source, &sr, pixels, rw * 4) : E_OUTOFMEMORY;
    const RECT dr = {at.x, at.y, at.x + (LONG)rw, at.y + (LONG)rh};
    if (SUCCEEDED(hr)) hr = surface_write(d, dest, &dr, pixels, rw * 4);
    mem_free(pixels);
  }
  LeaveCriticalSection(&d->lock);
  return hr;
}
/* UpdateTexture: what changed of a texture in system memory (its dirty region, then all of its lower levels) into one of the default pool of the same kind and
 * format; the destination may have fewer levels, which are the source's smaller ones (its top level the size of one of the source's). A destination that makes
 * its levels (D3DUSAGE_AUTOGENMIPMAP) gets the top level and makes the others. */
/* UpdateTexture of volume textures, which Windows checks less than the others (Wine's test_update_volumetexture holds what it does): levels counts, sizes and formats
 * may differ. Each level of the destination gets the source's level of the same size, if there is one, as it is - formats of the same bytes a pixel (A8R8G8B8 into
 * X8R8G8B8); the destination's other levels stay as they are. Formats of other sizes are refused. */
static HRESULT update_volume(Dev *d, Tex *st, Tex *dt) {
  if (format_bytes(st->format) != format_bytes(dt->format) || !format_bytes(st->format)) return D3DERR_INVALIDCALL;
  HRESULT hr = D3D_OK;
  EnterCriticalSection(&d->lock);
  for (UINT level = 0; level < dt->levels && SUCCEEDED(hr); ++level) {
    const UINT w = level_size(dt->width, level), h = level_size(dt->height, level), depth = level_size(dt->slices, level);
    for (UINT from = 0; from < st->levels; ++from) {
      if (level_size(st->width, from) != w || level_size(st->height, from) != h || level_size(st->slices, from) != depth) continue;
      BYTE *source = tex_shadow(st, from), *dest = tex_shadow(dt, level);
      if (!source || !dest) { hr = E_OUTOFMEMORY; break; }
      memcpy(dest, source, tex_sub_bytes(dt, level));      /* (the same size and bytes a pixel: the same bytes) */
      hr = tex_upload(d, dt, level);
      break;
    }
  }
  st->dirty[0] = 0;
  LeaveCriticalSection(&d->lock);
  return hr;
}
static HRESULT STDMETHODCALLTYPE dev_UpdateTexture(Dev *d, IDirect3DBaseTexture9 *from, IDirect3DBaseTexture9 *to) {
  Tex *st = texture_of(from), *dt = texture_of(to);
  if (!st || !dt || st->c.dev != d || dt->c.dev != d || st->kind != dt->kind || (st->format != dt->format && st->kind != 1)) return D3DERR_INVALIDCALL;
  if (st->pool != D3DPOOL_SYSTEMMEM || dt->pool != D3DPOOL_DEFAULT) return D3DERR_INVALIDCALL;
  if (st->kind == 1) return update_volume(d, st, dt);
  /* (the source level the destination's top level is: of the same size) */
  UINT skip = 0;
  while (skip < st->levels && (level_size(st->width, skip) > dt->width || level_size(st->height, skip) > dt->height)) ++skip;
  if (skip >= st->levels || level_size(st->width, skip) != dt->width || level_size(st->height, skip) != dt->height) return D3DERR_INVALIDCALL;
  if (st->kind == 1 && level_size(st->slices, skip) != dt->slices) return D3DERR_INVALIDCALL;
  const BOOL autogen = (dt->usage & D3DUSAGE_AUTOGENMIPMAP) != 0;
  const UINT levels = autogen ? 1 : dt->levels;
  if (st->levels - skip < levels) return D3DERR_INVALIDCALL;
  const UINT faces = st->kind == 3 ? 6 : 1;
  HRESULT hr = D3D_OK;
  EnterCriticalSection(&d->lock);
  for (UINT face = 0; face < faces && SUCCEEDED(hr); ++face) {
    if (!st->dirty[face]) continue;
    for (UINT level = 0; level < levels && SUCCEEDED(hr); ++level) {
      const UINT from_sub = tex_sub(st, face, level + skip), to_sub = tex_sub(dt, face, level);
      if (st->kind == 1) {
        /* (a volume's level: its slices as they are) */
        BYTE *source = tex_shadow(st, from_sub), *dest = tex_shadow(dt, to_sub);
        if (!source || !dest) { hr = E_OUTOFMEMORY; break; }
        memcpy(dest, source, tex_sub_bytes(dt, to_sub));
        hr = tex_upload(d, dt, to_sub);
        continue;
      }
      Surface *fs = surface_of_level(st, from_sub), *ts = surface_of_level(dt, to_sub);
      if (!fs || !ts) { hr = E_OUTOFMEMORY; break; }
      RECT r = {0, 0, (LONG)sub_width(st, from_sub), (LONG)sub_height(st, from_sub)};
      if (level + skip == 0) {
        r = st->dirty_rect[face];      /* (the top level: what changed; the lower ones are copied whole) */
        if (r.right > (LONG)sub_width(st, from_sub)) r.right = (LONG)sub_width(st, from_sub);
        if (r.bottom > (LONG)sub_height(st, from_sub)) r.bottom = (LONG)sub_height(st, from_sub);
        if (dxt_block(st->format)) { r.left &= ~3; r.top &= ~3; }
      }
      if (rect_empty(&r)) continue;
      const POINT at = {r.left, r.top};
      hr = surface_copy_raw(d, fs, &r, ts, at);
    }
    st->dirty[face] = 0;
  }
  if (SUCCEEDED(hr) && autogen) tex_generate_levels(d, dt);
  LeaveCriticalSection(&d->lock);
  return hr;
}
/* A target's pixels (a level of a target texture, a render target, a back buffer) into a surface in system memory of the same size and format. */
static HRESULT STDMETHODCALLTYPE dev_GetRenderTargetData(Dev *d, IDirect3DSurface9 *from, IDirect3DSurface9 *to) {
  Surface *source = surface_of(from), *dest = surface_of(to);
  if (!source || !dest || source->c.dev != d || dest->c.dev != d) return D3DERR_INVALIDCALL;
  Tex *st = source->tex, *dt = dest->tex;
  if (!st->target || dt->pool != D3DPOOL_SYSTEMMEM || st->format != dt->format || st->samples) return D3DERR_INVALIDCALL;
  const UINT w = sub_width(dt, dest->sub), h = sub_height(dt, dest->sub);
  if (w != sub_width(st, source->sub) || h != sub_height(st, source->sub)) return D3DERR_INVALIDCALL;
  EnterCriticalSection(&d->lock);
  HRESULT hr = tex_fetch(d, st, source->sub);
  if (SUCCEEDED(hr)) {
    const POINT at = {0, 0};
    const RECT r = {0, 0, (LONG)w, (LONG)h};
    hr = surface_copy_raw(d, source, &r, dest, at);
  }
  LeaveCriticalSection(&d->lock);
  return hr;
}
