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
/* Drawing and clearing (d3d.inc): Direct3D 7 through Direct3D 9's fixed function, Direct3D 1-3 transformed, lit and clipped on the CPU
 * (d3d_legacy.h) and drawn as transformed vertices. */

static BOOL primitive_type_ok(DWORD type) { return type >= D3DPT_POINTLIST && type <= D3DPT_TRIANGLEFAN; }

/* The clip status after a draw of Direct3D 2, 3 and 7: the union and intersection of the vertices' clip codes, and (D3DRENDERSTATE_EXTENTS, unless
 * D3DDP_DONOTUPDATEEXTENTS) the rectangle they cover. */
static void clip_status_add(D3dDevice *dev, const DlProcessResult *r, DWORD flags) {
  if (!r->count) return;
  D3DCLIPSTATUS *cs = &dev->clip_status;
  if (!(cs->dwFlags & D3DCLIPSTATUS_STATUS)) { cs->dwFlags |= D3DCLIPSTATUS_STATUS; cs->dwStatus = D3DSTATUS_DEFAULT; }
  cs->dwStatus |= r->clip_union & D3DSTATUS_CLIPUNIONALL;
  cs->dwStatus &= ~(DWORD)D3DSTATUS_CLIPINTERSECTIONALL | (r->clip_intersection << 12);
  if ((flags & D3DDP_DONOTUPDATEEXTENTS) || !dev->st.rs[D3DRENDERSTATE_EXTENTS] || !r->has_extent) return;
  if (!(cs->dwFlags & (D3DCLIPSTATUS_EXTENTS2 | D3DCLIPSTATUS_EXTENTS3))) {
    cs->minx = r->x1; cs->maxx = r->x2; cs->miny = r->y1; cs->maxy = r->y2;
    cs->dwFlags |= D3DCLIPSTATUS_EXTENTS2;
    return;
  }
  if (r->x1 < cs->minx) cs->minx = r->x1;
  if (r->x2 > cs->maxx) cs->maxx = r->x2;
  if (r->y1 < cs->miny) cs->miny = r->y1;
  if (r->y2 > cs->maxy) cs->maxy = r->y2;
}

/* Direct3D 7's clip status of untransformed vertices: the positions through world, view and projection. */
static void clip_status_ff(D3dDevice *dev, DWORD fvf, const BYTE *v, DWORD stride, DWORD count, DWORD flags) {
  if (!dev->st.rs[D3DRENDERSTATE_CLIPPING] || (fvf & D3DFVF_POSITION_MASK) == D3DFVF_XYZRHW) return;
  DlTransform t;
  d3_transform(dev, &t);
  DlProcessResult r;
  dl_result_init(&r);
  for (DWORD i = 0; i < count; ++i) {
    DlClipVertex cv;
    float p[3];
    memcpy(p, v + (SIZE_T)i * stride, sizeof p);
    zero(&cv, sizeof cv);
    dl_transform_point(&t.total, p, cv.position);
    float world[4];
    dl_transform_point(&t.world, p, world);
    for (unsigned k = 0; k < 6; ++k)
      cv.user[k] = t.user_plane[k][0] * world[0] + t.user_plane[k][1] * world[1] + t.user_plane[k][2] * world[2] + t.user_plane[k][3];
    cv.clip = dl_clip_code(cv.position, cv.user, t.user_planes);
    dl_result_add(&r, &t, &cv);
  }
  clip_status_add(dev, &r, flags);
}

/* The vertices an indexed draw reads: the larger of what the program says it has and the largest index + 1 (Windows reads what the indices name). */
static DWORD indexed_vertex_count(const WORD *indices, DWORD index_count, DWORD vertex_count) {
  DWORD top = 0;
  for (DWORD i = 0; i < index_count; ++i) if (indices[i] >= top) top = (DWORD)indices[i] + 1;
  return top > vertex_count ? top : vertex_count;
}

/* Draws vertices as Direct3D 9 takes them (Direct3D 7's vertices, and transformed vertices of every version). */
static HRESULT draw_direct(D3dDevice *dev, DWORD type, DWORD fvf, const BYTE *vertices, DWORD vertex_count, const WORD *indices, DWORD index_count, DWORD flags) {
  if (!primitive_type_ok(type)) return DDERR_INVALIDPARAMS;
  const DWORD stride = dl_fvf_stride(fvf);
  if (!stride || !dl_fvf_valid(fvf, 7)) return DDERR_INVALIDPARAMS;
  const UINT prims = dl_primitive_count(type, indices ? index_count : vertex_count);
  if (!prims) return D3D_OK;
  if (!vertices || (indices == NULL && index_count)) return DDERR_INVALIDPARAMS;
  HRESULT hr = d3_flush(dev);
  if (FAILED(hr)) return hr;
  IDirect3DDevice9 *d9 = dev->dd3->dev;
  IDirect3DVertexDeclaration9 *decl = d3_decl(dev->dd3, fvf);
  if (!decl) return DDERR_INVALIDPARAMS;
  D3D_CALL(d9, SetVertexDeclaration, decl);
  if (dev->st.rs[D3DRENDERSTATE_ZVISIBLE] && dev->occlusion) D3D_CALL(dev->occlusion, Issue, 2 /* D3DISSUE_BEGIN */);
  if (indices) {
    const DWORD n = indexed_vertex_count(indices, index_count, vertex_count);
    hr = D3D_CALL(d9, DrawIndexedPrimitiveUP, type, 0, n, prims, indices, D3DFMT_INDEX16, vertices, stride);
    if (dev->version >= 7) clip_status_ff(dev, fvf, vertices, stride, n, flags);
  } else {
    hr = D3D_CALL(d9, DrawPrimitiveUP, type, prims, vertices, stride);
    if (dev->version >= 7) clip_status_ff(dev, fvf, vertices, stride, vertex_count, flags);
  }
  if (dev->st.rs[D3DRENDERSTATE_ZVISIBLE] && dev->occlusion) D3D_CALL(dev->occlusion, Issue, 1 /* D3DISSUE_END */);
  if (FAILED(hr)) return DDERR_INVALIDPARAMS;
  d3_drawn(dev);
  ++dev->stats.dwVerticesProcessed;
  dev->stats.dwVerticesProcessed += vertex_count - 1;
  if (type >= D3DPT_TRIANGLELIST) dev->stats.dwTrianglesDrawn += prims;
  else if (type >= D3DPT_LINELIST) dev->stats.dwLinesDrawn += prims;
  else dev->stats.dwPointsDrawn += prims;
  return D3D_OK;
}

/* ---- the CPU path of Direct3D 1-3 ------------------------------------------------------------------------------------------------------------------- */

/* Collects primitives after clipping, as transformed vertices of `fvf` (dl_output_fvf), into one list of points, lines or triangles. */
typedef struct Assembly {
  D3dDevice *dev;
  const DlTransform *t;
  DWORD fvf, stride;
  BYTE *out;
  DWORD count, capacity;
  BOOL failed;
} Assembly;

static BYTE *assembly_room(Assembly *a, DWORD n) {
  if (a->failed) return NULL;
  if (n > a->capacity - a->count) {
    DWORD capacity = a->capacity ? a->capacity : 256;
    while (capacity - a->count < n) {
      if (capacity > 0x1000000u) { a->failed = TRUE; return NULL; }
      capacity *= 2;
    }
    BYTE *grown = mem_grow(a->out, (SIZE_T)capacity * a->stride);
    if (!grown) { a->failed = TRUE; return NULL; }
    a->out = grown;
    a->capacity = capacity;
  }
  BYTE *p = a->out + (SIZE_T)a->count * a->stride;
  a->count += n;
  return p;
}
static void assembly_put(Assembly *a, const DlClipVertex *v, DWORD n) {
  BYTE *p = assembly_room(a, n);
  if (p) dl_project_fvf(a->t, a->fvf, v, n, dl_bytes(p, (SIZE_T)n * a->stride));
}
static void assembly_triangle(Assembly *a, const DlClipVertex *v0, const DlClipVertex *v1, const DlClipVertex *v2, uint32_t planes, BOOL clip) {
  if (!clip) {
    const DlClipVertex tri[3] = {*v0, *v1, *v2};
    assembly_put(a, tri, 3);
    return;
  }
  const int cls = dl_triangle_clip_class(v0->clip, v1->clip, v2->clip);
  if (cls == 1) return;
  if (cls == 0) {
    const DlClipVertex tri[3] = {*v0, *v1, *v2};
    assembly_put(a, tri, 3);
    return;
  }
  DlClipVertex poly[DL_CLIP_MAX];
  poly[0] = *v0;
  poly[1] = *v1;
  poly[2] = *v2;
  const unsigned n = dl_clip_polygon(poly, 3, planes);
  for (unsigned i = 1; i + 1 < n; ++i) {
    const DlClipVertex tri[3] = {poly[0], poly[i], poly[i + 1]};
    assembly_put(a, tri, 3);
  }
}

/* Draws processed vertices (`cv`, `count` of them; `indices` or not) as primitives of `type`, clipped unless `noclip`. */
static HRESULT draw_processed(D3dDevice *dev, const DlTransform *t, DWORD type, DWORD out_fvf, const DlClipVertex *cv, DWORD count, const WORD *indices,
                              DWORD index_count, BOOL clip) {
  D3dDD *d = dev->dd3;
  IDirect3DDevice9 *d9 = d->dev;
  const DWORD stride = dl_fvf_stride(out_fvf);
  const DWORD n = indices ? index_count : count;
  const UINT prims = dl_primitive_count(type, n);
  if (!prims) return D3D_OK;
  IDirect3DVertexDeclaration9 *decl = d3_decl(d, out_fvf);
  if (!decl) return DDERR_INVALIDPARAMS;
  D3D_CALL(d9, SetVertexDeclaration, decl);
  HRESULT hr = D3D_OK;
  uint32_t any = 0;
  for (DWORD i = 0; i < count; ++i) any |= cv[i].clip;
  if (dev->st.rs[D3DRENDERSTATE_ZVISIBLE] && dev->occlusion) D3D_CALL(dev->occlusion, Issue, 2);
  if (!clip || !any) {
    /* (all on the screen: the primitives as they are) */
    BYTE *out = d3_scratch(d, 1, (SIZE_T)count * stride);
    if (!out) return DDERR_OUTOFMEMORY;
    dl_project_fvf(t, out_fvf, cv, count, dl_bytes(out, (SIZE_T)count * stride));
    if (indices) hr = D3D_CALL(d9, DrawIndexedPrimitiveUP, type, 0, count, prims, indices, D3DFMT_INDEX16, out, stride);
    else hr = D3D_CALL(d9, DrawPrimitiveUP, type, prims, out, stride);
  } else {
    Assembly a;
    zero(&a, sizeof a);
    a.dev = dev;
    a.t = t;
    a.fvf = out_fvf;
    a.stride = stride;
    const uint32_t planes = dl_clip_planes(t);
    DWORD out_type = D9_PT_TRIANGLELIST;
#define IDX(i) (indices ? (DWORD)indices[i] : (DWORD)(i))
#define VTX(i) (IDX(i) < count ? &cv[IDX(i)] : NULL)
    if (type == D3DPT_POINTLIST) {
      out_type = D9_PT_POINTLIST;
      for (DWORD i = 0; i < n; ++i) { const DlClipVertex *v = VTX(i); if (v && !v->clip) assembly_put(&a, v, 1); }
    } else if (type == D3DPT_LINELIST || type == D3DPT_LINESTRIP) {
      out_type = D9_PT_LINELIST;
      const DWORD step = type == D3DPT_LINELIST ? 2 : 1;
      for (DWORD i = 0; i + 1 < n; i += step) {
        const DlClipVertex *v0 = VTX(i), *v1 = VTX(i + 1);
        if (!v0 || !v1) continue;
        DlClipVertex l[2] = {*v0, *v1};
        if (dl_clip_line(&l[0], &l[1], planes)) assembly_put(&a, l, 2);
      }
    } else {
      for (DWORD p = 0; p < prims; ++p) {
        const DlClipVertex *v0, *v1, *v2;
        if (type == D3DPT_TRIANGLELIST) { v0 = VTX(3 * p); v1 = VTX(3 * p + 1); v2 = VTX(3 * p + 2); }
        else if (type == D3DPT_TRIANGLESTRIP) {
          v0 = VTX(p); v1 = VTX(p + 1); v2 = VTX(p + 2);
          if (p & 1) { const DlClipVertex *s = v0; v0 = v1; v1 = s; }      /* (every other triangle of a strip turns the other way) */
        } else { v0 = VTX(0); v1 = VTX(p + 1); v2 = VTX(p + 2); }
        if (v0 && v1 && v2) assembly_triangle(&a, v0, v1, v2, planes, TRUE);
      }
    }
#undef VTX
#undef IDX
    const UINT out_prims = dl_primitive_count(out_type, a.count);
    if (a.failed) hr = DDERR_OUTOFMEMORY;
    else if (out_prims) hr = D3D_CALL(d9, DrawPrimitiveUP, out_type, out_prims, a.out, stride);
    mem_free(a.out);
  }
  if (dev->st.rs[D3DRENDERSTATE_ZVISIBLE] && dev->occlusion) D3D_CALL(dev->occlusion, Issue, 1);
  if (FAILED(hr)) return hr == DDERR_OUTOFMEMORY ? hr : DDERR_INVALIDPARAMS;
  d3_drawn(dev);
  if (type >= D3DPT_TRIANGLELIST) dev->stats.dwTrianglesDrawn += prims;
  else if (type >= D3DPT_LINELIST) dev->stats.dwLinesDrawn += prims;
  else dev->stats.dwPointsDrawn += prims;
  dev->stats.dwVerticesProcessed += count;
  return D3D_OK;
}

/* DrawPrimitive / DrawIndexedPrimitive of Direct3D 2 and 3 (and their Begin .. End, strided and vertex buffer draws): untransformed vertices are
 * transformed with the current viewport, lit with its lights and the material of the MATERIAL light state, and clipped. */
static HRESULT draw_legacy(D3dDevice *dev, DWORD type, DWORD fvf, const BYTE *vertices, DWORD vertex_count, const WORD *indices, DWORD index_count, DWORD flags) {
  if (!primitive_type_ok(type)) return DDERR_INVALIDPARAMS;
  if (!dl_fvf_valid(fvf, 3) && !dl_fvf_valid(fvf, 7)) return DDERR_INVALIDPARAMS;
  if ((fvf & D3DFVF_POSITION_MASK) == D3DFVF_XYZRHW) return draw_direct(dev, type, fvf, vertices, vertex_count, indices, index_count, flags);
  if (!dl_primitive_count(type, indices ? index_count : vertex_count)) return D3D_OK;
  if (!vertices || (!indices && index_count)) return DDERR_INVALIDPARAMS;
  HRESULT hr = d3_flush(dev);
  if (FAILED(hr)) return hr;
  const DWORD count = indices ? indexed_vertex_count(indices, index_count, vertex_count) : vertex_count;
  const DWORD stride = dl_fvf_stride(fvf);
  DlClipVertex *cv = (DlClipVertex *)d3_scratch(dev->dd3, 0, (SIZE_T)count * sizeof *cv);
  if (!cv) return DDERR_OUTOFMEMORY;
  DlTransform t;
  d3_transform(dev, &t);
  DlLighting l;
  d3_lighting(dev, &l);
  DlProcessResult result;
  dl_result_init(&result);
  if (dl_process_fvf(&t, &l, !(flags & D3DDP_DONOTLIGHT), fvf, dl_const_bytes(vertices, (SIZE_T)count * stride), count, cv, count, &result) != DL_OK)
    return DDERR_INVALIDPARAMS;
  if (dev->st.rs[D3DRENDERSTATE_CLIPPING]) clip_status_add(dev, &result, flags);
  const BOOL clip = !(flags & D3DDP_DONOTCLIP) && dev->st.rs[D3DRENDERSTATE_CLIPPING];
  return draw_processed(dev, &t, type, dl_output_fvf(fvf), cv, count, indices, index_count, clip);
}

/* Any device's draw of vertices of an FVF code. */
static HRESULT d3_draw(D3dDevice *dev, DWORD type, DWORD fvf, const void *vertices, DWORD vertex_count, const WORD *indices, DWORD index_count, DWORD flags) {
  if (dev->version >= 7) return draw_direct(dev, type, fvf, vertices, vertex_count, indices, index_count, flags);
  return draw_legacy(dev, type, fvf, vertices, vertex_count, indices, index_count, flags);
}

/* ---- clearing --------------------------------------------------------------------------------------------------------------------------------------- */

/* Clears rectangles of the target and depth buffer within a viewport (Direct3D 7: the device's; Direct3D 1-3: the viewport's). */
static HRESULT d3_clear(D3dDevice *dev, DWORD count, const D3DRECT *rects, DWORD flags, DWORD color, float z, DWORD stencil, const D3DVIEWPORT7 *vp) {
  if (count && !rects) count = 0;
  if (flags & ~(DWORD)(D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL)) return DDERR_INVALIDPARAMS;
  HRESULT hr = d3_flush(dev);
  if (FAILED(hr)) return hr;
  Surface *zs = attached_depth(dev->target);
  if ((flags & (D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL)) && !zs) return D3DERR_ZBUFFER_NOTPRESENT;
  if ((flags & D3DCLEAR_STENCIL) && !dd_format(zs->fmt)->g) return D3DERR_STENCILBUFFER_NOTPRESENT;
  IDirect3DDevice9 *d9 = dev->dd3->dev;
  D3DVIEWPORT9 v9;
  const UINT tw = dev->target->desc.dwWidth, th = dev->target->desc.dwHeight;
  v9.X = vp->dwX < tw ? vp->dwX : tw;
  v9.Y = vp->dwY < th ? vp->dwY : th;
  v9.Width = vp->dwWidth < tw - v9.X ? vp->dwWidth : tw - v9.X;
  v9.Height = vp->dwHeight < th - v9.Y ? vp->dwHeight : th - v9.Y;
  v9.MinZ = 0.0f;
  v9.MaxZ = 1.0f;
  D3D_CALL(d9, SetViewport, &v9);
  dev->dirty.viewport = TRUE;
  if (z < 0.0f) z = 0.0f;
  if (z > 1.0f) z = 1.0f;
  hr = D3D_CALL(d9, Clear, count, (const D3DRECT *)rects, flags, color, z, stencil);
  if (FAILED(hr)) return DDERR_INVALIDPARAMS;
  if (flags & D3DCLEAR_TARGET) dev->target->store.shadow.state |= DD_SHADOW_GPU_NEWER;
  if (zs && (flags & (D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL))) zs->store.shadow.state |= DD_SHADOW_GPU_NEWER;
  return D3D_OK;
}
