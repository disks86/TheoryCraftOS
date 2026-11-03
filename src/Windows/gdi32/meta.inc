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
/* gdi32.dll: metafiles. A metafile DC is a client handle (NtGdiCreateClientObj) whose records live in user memory; a hidden "shadow" display DC in the kernel holds its graphics
 * state, so the state calls and queries (SetMapMode, GetViewportOrgEx, GetTextExtentPoint32, ...) simply run on the shadow (RD(hdc)) while the drawing calls are recorded
 * (EMF: enhmfdrv, WMF: mfdrv of Wine). The finished metafile objects (HENHMETAFILE / HMETAFILE) are client handles too; their bytes are in `meta_files`.
 * Included by gdi32.c after the wrappers. */

#define META_LIST_LIMIT 0x7fffffffu

typedef struct MetaDC {
  struct MetaDC *next;
  HDC handle, shadow;
  int enh;
  BYTE *buf;
  DWORD len, cap;
  HANDLE file;                  /* the disk file (INVALID_HANDLE_VALUE: memory) */
  DWORD nrecords, max_record;   /* WMF: the largest record in WORDs */
  HGDIOBJ *objects;             /* index -> object (EMF: 0 is the metafile itself) */
  DWORD nobjects;
  RECTL bounds;
  int bounds_set;
  RECTL frame;
  int frame_given;
  int saved;
  int path_open;
  SIZE dev, mm;                 /* the reference device */
  DWORD desc_chars, header_size;
  DWORD palette_entries;
  HPALETTE palette;
} MetaDC;

typedef struct MetaFile { struct MetaFile *next; HANDLE handle; int enh; BYTE *data; DWORD size; } MetaFile;
static MetaDC *meta_dcs;
static MetaFile *meta_files;

static MetaDC *meta_get(HDC hdc) {
  MetaDC *m;
  table_lock();
  for (m = meta_dcs; m; m = m->next) if (m->handle == (HANDLE)hdc) break;
  table_unlock();
  return m;
}
static HDC meta_real(HDC hdc) {
  if (!is_meta_dc(hdc)) return hdc;
  MetaDC *m = meta_get(hdc);
  return m ? m->shadow : hdc;
}
static MetaFile *metafile_get(HANDLE h) {
  MetaFile *f;
  table_lock();
  for (f = meta_files; f; f = f->next) if (f->handle == h) break;
  table_unlock();
  return f;
}
static HANDLE metafile_add(int enh, BYTE *data, DWORD size) {
  MetaFile *f = mem_alloc(sizeof(*f));
  HANDLE h;
  if (!f) { mem_free(data); return NULL; }
  h = NtGdiCreateClientObj((enh ? NTGDI_OBJ_ENHMETAFILE : NTGDI_OBJ_METAFILE) >> 16);
  if (!h) { mem_free(f); mem_free(data); return NULL; }
  f->handle = h; f->enh = enh; f->data = data; f->size = size;
  table_lock();
  f->next = meta_files; meta_files = f;
  table_unlock();
  return h;
}
static BOOL metafile_delete(HANDLE h, int enh) {
  MetaFile **link, *f = NULL;
  table_lock();
  for (link = &meta_files; *link; link = &(*link)->next) if ((*link)->handle == h && (*link)->enh == enh) { f = *link; *link = f->next; break; }
  table_unlock();
  if (!f) { SetLastError(ERROR_INVALID_HANDLE); return FALSE; }
  mem_free(f->data); mem_free(f);
  NtGdiDeleteClientObj(h);
  return TRUE;
}

static int meta_put(MetaDC *m, const void *data, DWORD n) {
  if (n > META_LIST_LIMIT - m->len) return 0;
  if (m->len + n > m->cap) {
    DWORD cap = m->cap ? m->cap : 4096;
    while (cap < m->len + n) { if (cap > 0x3fffffff) return 0; cap *= 2; }
    BYTE *p = mem_alloc(cap);
    if (!p) return 0;
    if (m->buf) memcpy(p, m->buf, m->len);
    mem_free(m->buf);
    m->buf = p; m->cap = cap;
  }
  if (data) memcpy(m->buf + m->len, data, n); else memset(m->buf + m->len, 0, n);
  m->len += n;
  return 1;
}

/* ---- creating a metafile DC -------------------------------------------------------------------------------------------------------------------------------- */

static MetaDC *meta_create(int enh, HDC ref, LPCWSTR filename) {
  MetaDC *m = mem_alloc(sizeof(*m));
  if (!m) return NULL;
  m->file = INVALID_HANDLE_VALUE;
  m->shadow = open_dc(L"DISPLAY", NULL, NULL, NULL, 1);
  if (!m->shadow) { mem_free(m); return NULL; }
  m->handle = NtGdiCreateClientObj((enh ? NTGDI_OBJ_ENHMETADC : NTGDI_OBJ_METADC) >> 16);
  if (!m->handle) { NtGdiDeleteObjectApp(m->shadow); mem_free(m); return NULL; }
  m->enh = enh;
  if (filename) {
    m->file = CreateFileW(filename, GENERIC_WRITE | GENERIC_READ, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (m->file == INVALID_HANDLE_VALUE) { NtGdiDeleteClientObj(m->handle); NtGdiDeleteObjectApp(m->shadow); mem_free(m); return NULL; }
  }
  {
    const HDC d = ref && !is_meta_dc(ref) ? ref : m->shadow;
    m->dev.cx = NtGdiGetDeviceCaps(d, HORZRES); m->dev.cy = NtGdiGetDeviceCaps(d, VERTRES);
    m->mm.cx = NtGdiGetDeviceCaps(d, HORZSIZE); m->mm.cy = NtGdiGetDeviceCaps(d, VERTSIZE);
    if (m->dev.cx <= 0) m->dev.cx = 1024;
    if (m->dev.cy <= 0) m->dev.cy = 768;
    if (m->mm.cx <= 0) m->mm.cx = 320;
    if (m->mm.cy <= 0) m->mm.cy = 240;
  }
  m->bounds.left = m->bounds.top = 0; m->bounds.right = m->bounds.bottom = -1;
  table_lock();
  m->next = meta_dcs; meta_dcs = m;
  table_unlock();
  return m;
}
static void meta_destroy(MetaDC *m) {
  MetaDC **link;
  table_lock();
  for (link = &meta_dcs; *link; link = &(*link)->next) if (*link == m) { *link = m->next; break; }
  table_unlock();
  NtGdiDeleteObjectApp(m->shadow);
  NtGdiDeleteClientObj(m->handle);
  if (m->file != INVALID_HANDLE_VALUE) CloseHandle(m->file);
  mem_free(m->buf); mem_free(m->objects); mem_free(m);
}
static BOOL meta_write_file(MetaDC *m) {
  DWORD done = 0;
  if (m->file == INVALID_HANDLE_VALUE) return TRUE;
  if (SetFilePointer(m->file, 0, NULL, FILE_BEGIN) == INVALID_SET_FILE_POINTER && GetLastError()) return FALSE;
  return WriteFile(m->file, m->buf, m->len, &done, NULL) && done == m->len;
}


/* ---- bounds -------------------------------------------------------------------------------------------------------------------------------------------------- */

static void meta_add_bounds(MetaDC *m, const RECTL *r) {
  if (!m->bounds_set) { m->bounds = *r; m->bounds_set = 1; return; }
  if (r->left < m->bounds.left) m->bounds.left = r->left;
  if (r->top < m->bounds.top) m->bounds.top = r->top;
  if (r->right > m->bounds.right) m->bounds.right = r->right;
  if (r->bottom > m->bounds.bottom) m->bounds.bottom = r->bottom;
}
/* The device-space bounds (inclusive) of logical points; merged into the metafile's bounds unless a path is being built. */
static void meta_bound_points(MetaDC *m, const POINT *pts, int n, RECTL *out) {
  POINT dp[2];
  RECTL r;
  int first = 1;
  r.left = r.top = r.right = r.bottom = 0;
  for (int i = 0; i < n; ++i) {
    POINT p = pts[i];
    NtGdiTransformPoints(m->shadow, &p, &dp[0], 1, NtGdiLPtoDP);
    if (first) { r.left = r.right = dp[0].x; r.top = r.bottom = dp[0].y; first = 0; }
    else { if (dp[0].x < r.left) r.left = dp[0].x; if (dp[0].x > r.right) r.right = dp[0].x; if (dp[0].y < r.top) r.top = dp[0].y; if (dp[0].y > r.bottom) r.bottom = dp[0].y; }
  }
  *out = r;
  if (!m->path_open && n) meta_add_bounds(m, &r);
}
static void meta_bound_rect(MetaDC *m, int l, int t, int r, int b, RECTL *out) {
  POINT p[2];
  p[0].x = l < r ? l : r; p[0].y = t < b ? t : b; p[1].x = l < r ? r : l; p[1].y = t < b ? b : t;
  if (get_dc_dword(m->shadow, NtGdiGetGraphicsMode, GM_COMPATIBLE) == GM_COMPATIBLE) { --p[1].x; --p[1].y; }
  meta_bound_points(m, p, 2, out);
}

/* ---- EMF records ----------------------------------------------------------------------------------------------------------------------------------------------- */

static int emf_record(MetaDC *m, DWORD type, const void *payload, DWORD size) {
  const DWORD padded = (size + 3) & ~3u, total = 8 + padded;
  DWORD head[2];
  head[0] = type; head[1] = total;
  if (!meta_put(m, head, 8)) return 0;
  if (size && !meta_put(m, payload, size)) return 0;
  if (padded > size && !meta_put(m, NULL, padded - size)) return 0;
  ++m->nrecords;
  return 1;
}
static int emf_ints(MetaDC *m, DWORD type, int n, const int *v) { return emf_record(m, type, v, n * 4); }

/* A record made of a fixed part and tail blocks: payload = head, then each block padded to 4. */
typedef struct EmfBuild { BYTE *p; DWORD len, cap; } EmfBuild;
static int eb_put(EmfBuild *b, const void *d, DWORD n) {
  if (b->len + n > b->cap) {
    DWORD cap = b->cap ? b->cap : 256;
    while (cap < b->len + n) cap *= 2;
    BYTE *p = mem_alloc(cap);
    if (!p) return 0;
    if (b->p) memcpy(p, b->p, b->len);
    mem_free(b->p);
    b->p = p; b->cap = cap;
  }
  if (d) memcpy(b->p + b->len, d, n); else memset(b->p + b->len, 0, n);
  b->len += n;
  return 1;
}
static int eb_align(EmfBuild *b) { return (b->len & 3) ? eb_put(b, NULL, 4 - (b->len & 3)) : 1; }

/* ---- EMF objects ---------------------------------------------------------------------------------------------------------------------------------------------- */

static DWORD emf_alloc_index(MetaDC *m, HGDIOBJ obj) {
  DWORD i;
  for (i = 1; i < m->nobjects; ++i) if (!m->objects[i]) { m->objects[i] = obj; return i; }
  if (!m->nobjects) m->nobjects = 1;
  HGDIOBJ *p = mem_alloc_n(m->nobjects + 8, sizeof(HGDIOBJ));
  if (!p) return 0;
  if (m->objects) memcpy(p, m->objects, m->nobjects * sizeof(HGDIOBJ));
  mem_free(m->objects);
  m->objects = p;
  i = m->nobjects;
  m->nobjects += 8;
  m->objects[i] = obj;
  return i;
}
static DWORD emf_max_handle(const MetaDC *m) { DWORD n = 1; for (DWORD i = 1; i < m->nobjects; ++i) if (m->objects[i] || 1) { if (m->objects[i]) n = i + 1; } return n; }

static int emf_dib_of_bitmap(HBITMAP bm, BITMAPINFO **bmi_out, void **bits_out, DWORD *bmi_size, DWORD *bits_size) {
  BITMAP b;
  HDC dc;
  BITMAPINFO *bi;
  void *bits;
  DWORD stride, size;
  if (!NtGdiExtGetObjectW(bm, sizeof(b), &b)) return 0;
  stride = ((DWORD)b.bmWidth * 24 + 31) / 32 * 4;
  size = stride * (DWORD)b.bmHeight;
  bi = mem_alloc(sizeof(BITMAPINFOHEADER) + 12);
  bits = mem_alloc(size);
  dc = NtGdiCreateCompatibleDC(NULL);
  if (!bi || !bits || !dc) { mem_free(bi); mem_free(bits); if (dc) NtGdiDeleteObjectApp(dc); return 0; }
  bi->bmiHeader.biSize = sizeof(BITMAPINFOHEADER); bi->bmiHeader.biWidth = b.bmWidth; bi->bmiHeader.biHeight = b.bmHeight; bi->bmiHeader.biPlanes = 1;
  bi->bmiHeader.biBitCount = 24; bi->bmiHeader.biCompression = BI_RGB;
  if (!NtGdiGetDIBitsInternal(dc, bm, 0, b.bmHeight, bits, bi, DIB_RGB_COLORS, DIB_UNKNOWN_SIZE, DIB_UNKNOWN_SIZE)) { NtGdiDeleteObjectApp(dc); mem_free(bi); mem_free(bits); return 0; }
  NtGdiDeleteObjectApp(dc);
  bi->bmiHeader.biSizeImage = size;
  *bmi_out = bi; *bits_out = bits; *bmi_size = sizeof(BITMAPINFOHEADER); *bits_size = size;
  return 1;
}

/* Gives `obj` an index in the metafile (recording its creation) and returns it; stock objects are named by ENHMETA_STOCK_OBJECT | index. 0xffffffff on failure. */
static DWORD emf_object_index(MetaDC *m, HGDIOBJ obj) {
  DWORD i;
  if (((UINT_PTR)obj & NTGDI_HANDLE_STOCK_OBJECT) && gdi_handle_type(obj) != NTGDI_OBJ_PAL) return ENHMETA_STOCK_OBJECT | (DWORD)(((UINT_PTR)obj & 0xffff) - 1);
  if (((UINT_PTR)obj & NTGDI_HANDLE_STOCK_OBJECT)) return ENHMETA_STOCK_OBJECT | DEFAULT_PALETTE;
  for (i = 1; i < m->nobjects; ++i) if (m->objects[i] == obj) return i;
  switch (gdi_handle_type(obj)) {
    case NTGDI_OBJ_PEN: {
      LOGPEN lp;
      struct { DWORD ih; LOGPEN lp; } r;
      if (!NtGdiExtGetObjectW(obj, sizeof(lp), &lp)) return 0xffffffff;
      if (!(i = emf_alloc_index(m, obj))) return 0xffffffff;
      r.ih = i; r.lp = lp;
      emf_record(m, EMR_CREATEPEN, &r, sizeof(r));
      return i;
    }
    case NTGDI_OBJ_EXTPEN: {
      BYTE buf[sizeof(EXTLOGPEN) + 16 * 4];
      EXTLOGPEN *elp = (EXTLOGPEN *)buf;
      EmfBuild b = {0};
      DWORD n = NtGdiExtGetObjectW(obj, sizeof(buf), buf), head[5] = {0, 0, 0, 0, 0};
      if (!n) return 0xffffffff;
      if (!(i = emf_alloc_index(m, obj))) return 0xffffffff;
      head[0] = i;
      eb_put(&b, head, 20);
      eb_put(&b, elp, n);
      if (n <= FIELD_OFFSET(EXTLOGPEN, elpStyleEntry)) eb_put(&b, NULL, 4);      /* (the record always has one style entry) */
      emf_record(m, EMR_EXTCREATEPEN, b.p, b.len);
      mem_free(b.p);
      return i;
    }
    case NTGDI_OBJ_BRUSH: {
      LOGBRUSH lb;
      if (!NtGdiExtGetObjectW(obj, sizeof(lb), &lb)) return 0xffffffff;
      if (lb.lbStyle == BS_PATTERN || lb.lbStyle == BS_PATTERN8X8 || lb.lbStyle == BS_DIBPATTERN || lb.lbStyle == BS_DIBPATTERNPT || lb.lbStyle == BS_DIBPATTERN8X8) {
        BITMAPINFO *bi = NULL; void *bits = NULL; DWORD bs = 0, ds = 0;
        EmfBuild b = {0};
        DWORD head[6];
        if (!emf_dib_of_bitmap((HBITMAP)lb.lbHatch, &bi, &bits, &bs, &ds)) return 0xffffffff;
        if (!(i = emf_alloc_index(m, obj))) { mem_free(bi); mem_free(bits); return 0xffffffff; }
        head[0] = i; head[1] = DIB_RGB_COLORS; head[2] = 8 + 24; head[3] = bs; head[4] = 8 + 24 + bs; head[5] = ds;
        eb_put(&b, head, 24); eb_put(&b, bi, bs); eb_put(&b, bits, ds);
        emf_record(m, EMR_CREATEDIBPATTERNBRUSHPT, b.p, b.len);
        mem_free(b.p); mem_free(bi); mem_free(bits);
        return i;
      } else {
        DWORD r[4];
        if (!(i = emf_alloc_index(m, obj))) return 0xffffffff;
        r[0] = i; r[1] = lb.lbStyle; r[2] = lb.lbColor; r[3] = (DWORD)lb.lbHatch;
        emf_record(m, EMR_CREATEBRUSHINDIRECT, r, 16);
        return i;
      }
    }
    case NTGDI_OBJ_FONT: {
      EMREXTCREATEFONTINDIRECTW r;
      LOGFONTW lf;
      if (!NtGdiExtGetObjectW(obj, sizeof(lf), &lf)) return 0xffffffff;
      if (!(i = emf_alloc_index(m, obj))) return 0xffffffff;
      memset(&r, 0, sizeof(r));
      r.ihFont = i;
      memcpy(&r.elfw.elfLogFont, &lf, sizeof(lf));
      emf_record(m, EMR_EXTCREATEFONTINDIRECTW, &r.ihFont, sizeof(r) - 8);
      return i;
    }
    case NTGDI_OBJ_PAL: {
      WORD count = 0;
      EmfBuild b = {0};
      DWORD ih;
      PALETTEENTRY *e;
      if (!NtGdiExtGetObjectW(obj, sizeof(count), &count)) return 0xffffffff;
      e = mem_alloc_n(count ? count : 1, sizeof(PALETTEENTRY));
      if (!e) return 0xffffffff;
      NtGdiDoPalette(obj, 0, count, e, NtGdiGetPaletteEntries, FALSE);
      if (!(i = emf_alloc_index(m, obj))) { mem_free(e); return 0xffffffff; }
      ih = i;
      eb_put(&b, &ih, 4);
      { WORD v[2] = { 0x300, count }; eb_put(&b, v, 4); }
      eb_put(&b, e, count * 4u);
      emf_record(m, EMR_CREATEPALETTE, b.p, b.len);
      mem_free(b.p); mem_free(e);
      return i;
    }
    default: return 0xffffffff;
  }
}

static void emf_delete_object(MetaDC *m, HGDIOBJ obj) {
  for (DWORD i = 1; i < m->nobjects; ++i) if (m->objects[i] == obj) { DWORD ih = i; m->objects[i] = NULL; emf_record(m, EMR_DELETEOBJECT, &ih, 4); return; }
}

/* ---- WMF records --------------------------------------------------------------------------------------------------------------------------------------------- */

static int wmf_record(MetaDC *m, WORD func, const WORD *params, DWORD nwords) {
  const DWORD size = 3 + nwords;
  DWORD s = size;
  if (!meta_put(m, &s, 4) || !meta_put(m, &func, 2)) return 0;
  if (nwords && !meta_put(m, params, nwords * 2)) return 0;
  if (size > m->max_record) m->max_record = size;
  ++m->nrecords;
  return 1;
}

/* A record of plain integers in API order. EMF: DWORDs in that order. WMF: WORDs in reverse order (bit i of `wflags`: argument i is a 32-bit value, stored as low word, high word). */
static BOOL meta_simple(HDC hdc, DWORD emr, int wmf, unsigned wflags, int n, ...) {
  MetaDC *m = meta_get(hdc);
  int v[16];
  __builtin_va_list ap;
  if (!m || n > 16) return FALSE;
  __builtin_va_start(ap, n);
  for (int i = 0; i < n; ++i) v[i] = __builtin_va_arg(ap, int);
  __builtin_va_end(ap);
  if (m->enh) return emr ? emf_ints(m, emr, n, v) : TRUE;
  if (wmf < 0) return TRUE;
  {
    WORD w[32];
    int k = 0;
    for (int i = n - 1; i >= 0; --i) {
      if (wflags & (1u << i)) { w[k++] = (WORD)(v[i] & 0xffff); w[k++] = (WORD)((DWORD)v[i] >> 16); } else w[k++] = (WORD)v[i];
    }
    return wmf_record(m, (WORD)wmf, w, k);
  }
}


/* ---- selecting objects -------------------------------------------------------------------------------------------------------------------------------------- */

static int wmf_object_index(MetaDC *m, HGDIOBJ obj);

static HGDIOBJ meta_select(HDC hdc, HGDIOBJ obj) {
  MetaDC *m = meta_get(hdc);
  HGDIOBJ previous;
  if (!m) return NULL;
  switch (gdi_handle_type(obj)) {
    case NTGDI_OBJ_PEN: case NTGDI_OBJ_EXTPEN: case NTGDI_OBJ_BRUSH: case NTGDI_OBJ_FONT: break;
    case NTGDI_OBJ_REGION: return (HGDIOBJ)(UINT_PTR)(UINT)ExtSelectClipRgn(hdc, obj, RGN_COPY);
    default: SetLastError(ERROR_INVALID_HANDLE); return NULL;
  }
  previous = NULL;
  switch (gdi_handle_type(obj)) {
    case NTGDI_OBJ_BRUSH: previous = NtGdiSelectBrush(m->shadow, obj); break;
    case NTGDI_OBJ_FONT: previous = NtGdiSelectFont(m->shadow, obj); break;
    default: previous = NtGdiSelectPen(m->shadow, obj); break;
  }
  if (m->enh) {
    const DWORD i = emf_object_index(m, obj);
    if (i == 0xffffffff) return previous;
    emf_record(m, EMR_SELECTOBJECT, &i, 4);
  } else {
    const int i = wmf_object_index(m, obj);
    if (i < 0) return previous;
    WORD w = (WORD)i;
    wmf_record(m, 0x012D /* META_SELECTOBJECT */, &w, 1);
  }
  return previous;
}
static HPALETTE meta_select_palette(HDC hdc, HPALETTE pal) {
  MetaDC *m = meta_get(hdc);
  HPALETTE previous = m ? m->palette : NULL;
  if (!m) return NULL;
  if (m->enh) {
    const DWORD i = emf_object_index(m, pal);
    if (i == 0xffffffff) return NULL;
    emf_record(m, EMR_SELECTPALETTE, &i, 4);
  } else {
    const int i = wmf_object_index(m, pal);
    if (i < 0) return NULL;
    WORD w = (WORD)i;
    wmf_record(m, 0x0234, &w, 1);
  }
  m->palette = pal;
  return previous ? previous : GetStockObject(DEFAULT_PALETTE);
}
static void meta_object_deleted(HGDIOBJ obj) {
  MetaDC *m;
  for (m = meta_dcs; m; m = m->next) {
    if (m->enh) emf_delete_object(m, obj);
    else for (DWORD i = 0; i < m->nobjects; ++i) if (m->objects[i] == obj) { WORD w = (WORD)i; m->objects[i] = NULL; wmf_record(m, 0x01F0, &w, 1); break; }
  }
}

/* ---- records with a point list -------------------------------------------------------------------------------------------------------------------------------- */

static int points_fit16(const POINT *p, int n) { for (int i = 0; i < n; ++i) if (p[i].x < -32768 || p[i].x > 32767 || p[i].y < -32768 || p[i].y > 32767) return 0; return 1; }

/* kind: 0 bezier, 1 polygon, 2 polyline, 3 bezierto, 4 polylineto */
static BOOL meta_poly(HDC hdc, int kind, const POINT *pts, int n) {
  static const DWORD emr32[] = { EMR_POLYBEZIER, EMR_POLYGON, EMR_POLYLINE, EMR_POLYBEZIERTO, EMR_POLYLINETO };
  static const DWORD emr16[] = { EMR_POLYBEZIER16, EMR_POLYGON16, EMR_POLYLINE16, EMR_POLYBEZIERTO16, EMR_POLYLINETO16 };
  static const WORD wmfs[] = { 0, 0x0324, 0x0325, 0, 0 };
  MetaDC *m = meta_get(hdc);
  RECTL b;
  if (!m || n < 0 || (!pts && n)) return FALSE;
  if (kind == 3 || kind == 4) {
    POINT all[1];
    meta_bound_points(m, pts, n, &b);
    NtGdiGetDCPoint(m->shadow, NtGdiGetCurrentPosition, &all[0]);
    (void)all;
  } else meta_bound_points(m, pts, n, &b);
  if (m->enh) {
    EmfBuild eb = {0};
    const int small = points_fit16(pts, n);
    DWORD cnt = n;
    eb_put(&eb, &b, 16); eb_put(&eb, &cnt, 4);
    if (small) for (int i = 0; i < n; ++i) { SHORT s[2] = { (SHORT)pts[i].x, (SHORT)pts[i].y }; eb_put(&eb, s, 4); }
    else eb_put(&eb, pts, n * 8u);
    emf_record(m, small ? emr16[kind] : emr32[kind], eb.p, eb.len);
    mem_free(eb.p);
  } else if (wmfs[kind]) {
    WORD *w = mem_alloc_n(1 + 2 * (SIZE_T)n, 2);
    if (!w) return FALSE;
    w[0] = (WORD)n;
    for (int i = 0; i < n; ++i) { w[1 + 2 * i] = (WORD)pts[i].x; w[2 + 2 * i] = (WORD)pts[i].y; }
    wmf_record(m, wmfs[kind], w, 1 + 2 * n);
    mem_free(w);
  }
  if (kind == 3 || kind == 4) { if (n) NtGdiMoveTo(m->shadow, pts[n - 1].x, pts[n - 1].y, NULL); }
  return TRUE;
}
static BOOL meta_polypoly(HDC hdc, int polygon, const POINT *pts, const DWORD *counts, DWORD n) {
  MetaDC *m = meta_get(hdc);
  DWORD total = 0;
  RECTL b;
  if (!m || !pts || !counts) return FALSE;
  for (DWORD i = 0; i < n; ++i) { if (counts[i] > 0x1000000 - total) return FALSE; total += counts[i]; }
  meta_bound_points(m, pts, total, &b);
  if (m->enh) {
    EmfBuild eb = {0};
    const int small = points_fit16(pts, total);
    eb_put(&eb, &b, 16); eb_put(&eb, &n, 4); eb_put(&eb, &total, 4); eb_put(&eb, counts, n * 4);
    if (small) for (DWORD i = 0; i < total; ++i) { SHORT s[2] = { (SHORT)pts[i].x, (SHORT)pts[i].y }; eb_put(&eb, s, 4); }
    else eb_put(&eb, pts, total * 8u);
    emf_record(m, small ? (polygon ? EMR_POLYPOLYGON16 : EMR_POLYPOLYLINE16) : (polygon ? EMR_POLYPOLYGON : EMR_POLYPOLYLINE), eb.p, eb.len);
    mem_free(eb.p);
  } else if (polygon) {
    WORD *w = mem_alloc_n(1 + (SIZE_T)n * 2 + (SIZE_T)total * 2, 2);
    DWORD k = 1;
    if (!w) return FALSE;
    w[0] = (WORD)n;
    for (DWORD i = 0; i < n; ++i) { w[k++] = (WORD)counts[i]; w[k++] = 0; }
    for (DWORD i = 0; i < total; ++i) { w[k++] = (WORD)pts[i].x; w[k++] = (WORD)pts[i].y; }
    wmf_record(m, 0x0538, w, k);
    mem_free(w);
  }
  return TRUE;
}

/* ---- shapes ---------------------------------------------------------------------------------------------------------------------------------------------------- */

static BOOL meta_shape(HDC hdc, DWORD emr, int wmf, int n, const int *v) {
  /* v: left, top, right, bottom [, more]; bounds from the rectangle */
  MetaDC *m = meta_get(hdc);
  RECTL b;
  if (!m) return FALSE;
  meta_bound_rect(m, v[0], v[1], v[2], v[3], &b);
  switch (n) {
    case 4: return meta_simple(hdc, emr, wmf, 0, 4, v[0], v[1], v[2], v[3]);
    case 6: return meta_simple(hdc, emr, wmf, 0, 6, v[0], v[1], v[2], v[3], v[4], v[5]);
    default: return meta_simple(hdc, emr, wmf, 0, 8, v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7]);
  }
}
static BOOL meta_arc(HDC hdc, int kind, int l, int t, int r, int b, int xs, int ys, int xe, int ye) {
  static const DWORD emr[] = { EMR_ARC, EMR_ARCTO, EMR_CHORD, EMR_PIE };
  static const int wmf[] = { 0x0817, -1, 0x0830, 0x081A };
  const int v[8] = { l, t, r, b, xs, ys, xe, ye };
  if (kind == 1) { MetaDC *m = meta_get(hdc); if (m) NtGdiMoveTo(m->shadow, xe, ye, NULL); }
  return meta_shape(hdc, emr[kind], wmf[kind], 8, v);
}

/* ---- text ------------------------------------------------------------------------------------------------------------------------------------------------------ */

static BOOL meta_exttextout(HDC hdc, int x, int y, UINT flags, const RECT *rect, const WCHAR *str, UINT count, const INT *dx) {
  MetaDC *m = meta_get(hdc);
  RECTL b;
  SIZE sz;
  POINT pts[2];
  if (!m) return FALSE;
  if (m->enh) {
    EmfBuild eb = {0};
    EMREXTTEXTOUTW r;
    const DWORD mode = get_dc_dword(m->shadow, NtGdiGetGraphicsMode, GM_COMPATIBLE);
    memset(&r, 0, sizeof(r));
    if (count && NtGdiGetTextExtentExW(m->shadow, str, count, 0, NULL, NULL, &sz, 0)) {
      pts[0].x = x; pts[0].y = y; pts[1].x = x + sz.cx; pts[1].y = y + sz.cy;
      meta_bound_points(m, pts, 2, &b);
    } else { b.left = b.top = 0; b.right = b.bottom = -1; }
    if (rect && (flags & (ETO_OPAQUE | ETO_CLIPPED))) { RECTL rb; meta_bound_rect(m, rect->left, rect->top, rect->right, rect->bottom, &rb); }
    r.rclBounds = b;
    r.iGraphicsMode = mode;
    if (mode == GM_COMPATIBLE) { r.exScale = 100.0f * NtGdiGetDeviceCaps(m->shadow, HORZSIZE) / NtGdiGetDeviceCaps(m->shadow, HORZRES); r.eyScale = 100.0f * NtGdiGetDeviceCaps(m->shadow, VERTSIZE) / NtGdiGetDeviceCaps(m->shadow, VERTRES); }
    r.emrtext.ptlReference.x = x; r.emrtext.ptlReference.y = y; r.emrtext.nChars = count; r.emrtext.fOptions = flags;
    if (rect) { r.emrtext.rcl.left = rect->left; r.emrtext.rcl.top = rect->top; r.emrtext.rcl.right = rect->right; r.emrtext.rcl.bottom = rect->bottom; }
    r.emrtext.offString = 8 + sizeof(r) - 8 + 0;
    r.emrtext.offString = sizeof(EMREXTTEXTOUTW);
    r.emrtext.offDx = dx ? sizeof(EMREXTTEXTOUTW) + ((count * 2 + 3) & ~3u) : 0;
    eb_put(&eb, (BYTE *)&r + 8, sizeof(r) - 8);
    eb_put(&eb, str, count * 2); eb_align(&eb);
    if (dx) eb_put(&eb, dx, count * 4);
    emf_record(m, EMR_EXTTEXTOUTW, eb.p, eb.len);
    mem_free(eb.p);
  } else {
    char *a = mem_alloc((SIZE_T)count * 2 + 4);
    int alen = 0;
    EmfBuild eb = {0};
    WORD w[8];
    if (!a) return FALSE;
    if (count) alen = WideCharToMultiByte(GdiGetCodePage(m->shadow), 0, str, count, a, count * 2, NULL, NULL);
    w[0] = (WORD)y; w[1] = (WORD)x; w[2] = (WORD)alen; w[3] = (WORD)flags;
    eb_put(&eb, w, 8);
    if (rect && (flags & (ETO_OPAQUE | ETO_CLIPPED))) { WORD rc[4] = { (WORD)rect->left, (WORD)rect->top, (WORD)rect->right, (WORD)rect->bottom }; eb_put(&eb, rc, 8); }
    eb_put(&eb, a, alen); if (alen & 1) eb_put(&eb, NULL, 1);
    if (dx) for (int i = 0; i < alen && (UINT)i < count; ++i) { WORD d = (WORD)dx[i]; eb_put(&eb, &d, 2); }
    wmf_record(m, 0x0a32, (WORD *)eb.p, eb.len / 2);
    mem_free(eb.p); mem_free(a);
  }
  return TRUE;
}

/* ---- regions and clipping ------------------------------------------------------------------------------------------------------------------------------- */

static RGNDATA *region_data(HRGN rgn, DWORD *size) {
  DWORD n = NtGdiGetRegionData(rgn, 0, NULL);
  RGNDATA *d;
  if (!n) return NULL;
  d = mem_alloc(n);
  if (!d) return NULL;
  if (!NtGdiGetRegionData(rgn, n, d)) { mem_free(d); return NULL; }
  *size = n;
  return d;
}
static BOOL meta_regionop(HDC hdc, DWORD emr, HRGN rgn, HBRUSH brush, int w, int h, int mode, int has_mode) {
  MetaDC *m = meta_get(hdc);
  EmfBuild eb = {0};
  DWORD size = 0, ih = 0, cb;
  RGNDATA *d = NULL;
  RECTL b;
  RECT rc;
  if (!m || !m->enh) return TRUE;
  if (rgn) { d = region_data(rgn, &size); if (!d) return FALSE; }
  if (emr == EMR_EXTSELECTCLIPRGN) {
    cb = size; { DWORD md = mode; eb_put(&eb, &cb, 4); eb_put(&eb, &md, 4); }
    if (d) eb_put(&eb, d, size);
  } else {
    NtGdiGetRgnBox(rgn, &rc);
    meta_bound_rect(m, rc.left, rc.top, rc.right, rc.bottom, &b);
    cb = size;
    eb_put(&eb, &b, 16); eb_put(&eb, &cb, 4);
    if (emr == EMR_FILLRGN || emr == EMR_FRAMERGN) { ih = emf_object_index(m, brush); eb_put(&eb, &ih, 4); }
    if (emr == EMR_FRAMERGN) { SIZE s = { w, h }; eb_put(&eb, &s, 8); }
    eb_put(&eb, d, size);
  }
  (void)has_mode;
  emf_record(m, emr, eb.p, eb.len);
  mem_free(eb.p); mem_free(d);
  return TRUE;
}

/* ---- blits ------------------------------------------------------------------------------------------------------------------------------------------------------ */

#define ROP_USES_SOURCE(rop) ((((rop) >> 2) & 0x330000) != ((rop) & 0x330000))

/* kind: 0 BitBlt / PatBlt, 1 StretchBlt, 2 AlphaBlend, 3 TransparentBlt. `extra` is the ROP, the blend function or the transparent colour. */
static BOOL meta_blit(HDC hdc, int kind, int xd, int yd, int wd, int hd, HDC src, int xs, int ys, int ws, int hs, DWORD extra) {
  MetaDC *m = meta_get(hdc);
  EmfBuild eb = {0};
  RECTL b;
  XFORM id = { 1, 0, 0, 1, 0, 0 };
  BITMAPINFO *bi = NULL;
  void *bits = NULL;
  DWORD bs = 0, ds = 0, bk = 0, head, f[8];
  const int use_src = kind >= 2 || (src && ROP_USES_SOURCE(extra));
  const DWORD emr = kind == 0 ? EMR_BITBLT : kind == 1 ? EMR_STRETCHBLT : kind == 2 ? EMR_ALPHABLEND : EMR_TRANSPARENTBLT;
  if (!m) return FALSE;
  if (!m->enh) {
    if (kind == 0 && !use_src) (void)meta_simple(hdc, 0, 0x061D, 1 << 4, 5, xd, yd, wd, hd, (int)extra);
    return TRUE;
  }
  meta_bound_rect(m, xd, yd, xd + wd, yd + hd, &b);
  if (use_src) {
    HBITMAP bm = src && !is_meta_dc(src) ? GetCurrentObject(src, OBJ_BITMAP) : NULL;
    if (!bm || !emf_dib_of_bitmap(bm, &bi, &bits, &bs, &ds)) return FALSE;
    bk = get_dc_dword(src, NtGdiGetBkColor, 0);
  }
  head = 8 + 16 + 16 + 4 + 8 + 24 + 4 + 4 + 16 + (kind >= 1 ? 8 : 0);
  eb_put(&eb, &b, 16);
  f[0] = xd; f[1] = yd; f[2] = wd; f[3] = hd; eb_put(&eb, f, 16);
  eb_put(&eb, &extra, 4);
  f[0] = xs; f[1] = ys; eb_put(&eb, f, 8);
  eb_put(&eb, &id, 24);
  eb_put(&eb, &bk, 4);
  f[0] = DIB_RGB_COLORS; f[1] = use_src ? head : 0; f[2] = bs; f[3] = use_src ? head + bs : 0; f[4] = ds;
  eb_put(&eb, f, 20);
  if (kind >= 1) { f[0] = ws; f[1] = hs; eb_put(&eb, f, 8); }
  if (use_src) { eb_put(&eb, bi, bs); eb_put(&eb, bits, ds); }
  emf_record(m, emr, eb.p, eb.len);
  mem_free(eb.p); mem_free(bi); mem_free(bits);
  return TRUE;
}

/* kind 0: SetDIBitsToDevice (xs, ys, ws, hs is the source rectangle, start / lines the scan lines); kind 1: StretchDIBits. */
static BOOL meta_dibs(HDC hdc, int kind, int xd, int yd, int wd, int hd, int xs, int ys, int ws, int hs, UINT start, UINT lines, const void *bits, const BITMAPINFO *bmi, UINT usage, DWORD rop) {
  MetaDC *m = meta_get(hdc);
  EmfBuild eb = {0};
  RECTL b;
  DibHeader h;
  DWORD ds, bs, f[16], head;
  if (!m) return FALSE;
  if (!dib_parse(bmi, usage, &h)) return FALSE;
  ds = dib_image_bytes(&h);
  bs = dib_info_bytes(&h);
  if (ds == (DWORD)-1) return FALSE;
  if (!m->enh) {
    WORD w[12];
    int k = 0;
    if (kind == 0) {
      w[k++] = (WORD)usage; w[k++] = (WORD)lines; w[k++] = (WORD)start; w[k++] = (WORD)ys; w[k++] = (WORD)xs; w[k++] = (WORD)hs; w[k++] = (WORD)ws; w[k++] = (WORD)yd; w[k++] = (WORD)xd;
    } else {
      w[k++] = (WORD)(rop & 0xffff); w[k++] = (WORD)(rop >> 16); w[k++] = (WORD)usage; w[k++] = (WORD)hs; w[k++] = (WORD)ws; w[k++] = (WORD)ys; w[k++] = (WORD)xs;
      w[k++] = (WORD)hd; w[k++] = (WORD)wd; w[k++] = (WORD)yd; w[k++] = (WORD)xd;
    }
    eb_put(&eb, w, k * 2); eb_put(&eb, bmi, bs); eb_put(&eb, bits, ds);
    if (eb.len & 1) eb_put(&eb, NULL, 1);
    wmf_record(m, kind == 0 ? 0x0d33 : 0x0f43, (WORD *)eb.p, eb.len / 2);
    mem_free(eb.p);
    return TRUE;
  }
  if (kind == 0) meta_bound_rect(m, xd, yd, xd + ws, yd + hs, &b); else meta_bound_rect(m, xd, yd, xd + wd, yd + hd, &b);
  eb_put(&eb, &b, 16);
  if (kind == 0) {
    head = 8 + 16 + 8 + 8 + 8 + 16 + 12;
    f[0] = xd; f[1] = yd; f[2] = xs; f[3] = ys; f[4] = ws; f[5] = hs; f[6] = head; f[7] = bs; f[8] = head + bs; f[9] = ds; f[10] = usage; f[11] = start; f[12] = lines;
    eb_put(&eb, f, 13 * 4);
    eb_put(&eb, bmi, bs); eb_put(&eb, bits, ds);
    emf_record(m, EMR_SETDIBITSTODEVICE, eb.p, eb.len);
  } else {
    head = 8 + 16 + 8 + 8 + 8 + 16 + 4 + 4 + 8;
    f[0] = xd; f[1] = yd; f[2] = xs; f[3] = ys; f[4] = ws; f[5] = hs; f[6] = head; f[7] = bs; f[8] = head + bs; f[9] = ds; f[10] = usage; f[11] = rop; f[12] = wd; f[13] = hd;
    eb_put(&eb, f, 14 * 4);
    eb_put(&eb, bmi, bs); eb_put(&eb, bits, ds);
    emf_record(m, EMR_STRETCHDIBITS, eb.p, eb.len);
  }
  mem_free(eb.p);
  return TRUE;
}

/* ---- WMF objects ---------------------------------------------------------------------------------------------------------------------------------------------- */

static int wmf_alloc_index(MetaDC *m, HGDIOBJ obj) {
  DWORD i;
  for (i = 0; i < m->nobjects; ++i) if (!m->objects[i]) { m->objects[i] = obj; return (int)i; }
  {
    HGDIOBJ *p = mem_alloc_n(m->nobjects + 8, sizeof(HGDIOBJ));
    if (!p) return -1;
    if (m->objects) memcpy(p, m->objects, m->nobjects * sizeof(HGDIOBJ));
    mem_free(m->objects);
    m->objects = p;
    i = m->nobjects;
    m->nobjects += 8;
    m->objects[i] = obj;
  }
  return (int)i;
}
static DWORD wmf_object_count(const MetaDC *m) { DWORD n = 0; for (DWORD i = 0; i < m->nobjects; ++i) if (m->objects[i] || (m->palette_entries > i)) n = i + 1; return n; }

static int wmf_object_index(MetaDC *m, HGDIOBJ obj) {
  int i;
  for (i = 0; i < (int)m->nobjects; ++i) if (m->objects[i] == obj) return i;
  switch (gdi_handle_type(obj)) {
    case NTGDI_OBJ_PEN: case NTGDI_OBJ_EXTPEN: {
      LOGPEN lp;
      WORD w[5];
      if (gdi_handle_type(obj) == NTGDI_OBJ_EXTPEN) {
        EXTLOGPEN e;
        if (!NtGdiExtGetObjectW(obj, sizeof(e), &e)) return -1;
        lp.lopnStyle = e.elpPenStyle & PS_STYLE_MASK; lp.lopnWidth.x = e.elpWidth; lp.lopnWidth.y = 0; lp.lopnColor = e.elpColor;
      } else if (!NtGdiExtGetObjectW(obj, sizeof(lp), &lp)) return -1;
      if ((i = wmf_alloc_index(m, obj)) < 0) return -1;
      w[0] = (WORD)lp.lopnStyle; w[1] = (WORD)lp.lopnWidth.x; w[2] = (WORD)lp.lopnWidth.y; w[3] = (WORD)(lp.lopnColor & 0xffff); w[4] = (WORD)(lp.lopnColor >> 16);
      wmf_record(m, 0x02FA, w, 5);
      return i;
    }
    case NTGDI_OBJ_BRUSH: {
      LOGBRUSH lb;
      if (!NtGdiExtGetObjectW(obj, sizeof(lb), &lb)) return -1;
      if (lb.lbStyle == BS_PATTERN || lb.lbStyle == BS_DIBPATTERN || lb.lbStyle == BS_DIBPATTERNPT || lb.lbStyle == BS_PATTERN8X8 || lb.lbStyle == BS_DIBPATTERN8X8) {
        BITMAPINFO *bi = NULL; void *bits = NULL; DWORD bs = 0, ds = 0;
        EmfBuild b = {0};
        WORD head[2];
        if (!emf_dib_of_bitmap((HBITMAP)lb.lbHatch, &bi, &bits, &bs, &ds)) return -1;
        if ((i = wmf_alloc_index(m, obj)) < 0) { mem_free(bi); mem_free(bits); return -1; }
        head[0] = BS_PATTERN; head[1] = DIB_RGB_COLORS;
        eb_put(&b, head, 4); eb_put(&b, bi, bs); eb_put(&b, bits, ds);
        wmf_record(m, 0x0142, (WORD *)b.p, b.len / 2);
        mem_free(b.p); mem_free(bi); mem_free(bits);
        return i;
      } else {
        WORD w[4];
        if ((i = wmf_alloc_index(m, obj)) < 0) return -1;
        w[0] = (WORD)lb.lbStyle; w[1] = (WORD)(lb.lbColor & 0xffff); w[2] = (WORD)(lb.lbColor >> 16); w[3] = (WORD)lb.lbHatch;
        wmf_record(m, 0x02FC, w, 4);
        return i;
      }
    }
    case NTGDI_OBJ_FONT: {
      LOGFONTW lf;
      BYTE rec[50];
      if (!NtGdiExtGetObjectW(obj, sizeof(lf), &lf)) return -1;
      if ((i = wmf_alloc_index(m, obj)) < 0) return -1;
      memset(rec, 0, sizeof(rec));
      { SHORT s[5] = { (SHORT)lf.lfHeight, (SHORT)lf.lfWidth, (SHORT)lf.lfEscapement, (SHORT)lf.lfOrientation, (SHORT)lf.lfWeight }; memcpy(rec, s, 10); }
      rec[10] = lf.lfItalic; rec[11] = lf.lfUnderline; rec[12] = lf.lfStrikeOut; rec[13] = lf.lfCharSet; rec[14] = lf.lfOutPrecision; rec[15] = lf.lfClipPrecision;
      rec[16] = lf.lfQuality; rec[17] = lf.lfPitchAndFamily;
      WideCharToMultiByte(CP_ACP, 0, lf.lfFaceName, -1, (char *)rec + 18, 32, NULL, NULL);
      rec[49] = 0;
      wmf_record(m, 0x02FB, (WORD *)rec, 25);
      return i;
    }
    case NTGDI_OBJ_PAL: {
      WORD count = 0;
      EmfBuild b = {0};
      PALETTEENTRY *e;
      WORD head[2];
      if (!NtGdiExtGetObjectW(obj, sizeof(count), &count)) return -1;
      e = mem_alloc_n(count ? count : 1, 4);
      if (!e) return -1;
      NtGdiDoPalette(obj, 0, count, e, NtGdiGetPaletteEntries, FALSE);
      if ((i = wmf_alloc_index(m, obj)) < 0) { mem_free(e); return -1; }
      head[0] = 0x300; head[1] = count;
      eb_put(&b, head, 4); eb_put(&b, e, count * 4u);
      wmf_record(m, 0x00F7, (WORD *)b.p, b.len / 2);
      mem_free(b.p); mem_free(e);
      return i;
    }
    default: return -1;
  }
}

/* ---- creating and closing --------------------------------------------------------------------------------------------------------------------------------------- */

EXPORT HDC WINAPI CreateEnhMetaFileW(HDC ref, LPCWSTR filename, const RECT *rect, LPCWSTR description) {
  MetaDC *m = meta_create(1, ref, filename);
  DWORD chars = 0;
  ENHMETAHEADER h;
  if (!m) return NULL;
  if (description) { while (description[chars] || description[chars + 1]) ++chars; chars += 2; }
  m->desc_chars = chars;
  m->header_size = sizeof(ENHMETAHEADER) + ((chars * 2 + 3) & ~3u);
  memset(&h, 0, sizeof(h));
  meta_put(m, &h, sizeof(h));
  if (chars) { meta_put(m, description, chars * 2); if ((chars * 2) & 3) meta_put(m, NULL, 4 - ((chars * 2) & 3)); }
  m->nrecords = 1;
  if (rect) { m->frame.left = rect->left; m->frame.top = rect->top; m->frame.right = rect->right; m->frame.bottom = rect->bottom; m->frame_given = 1; }
  return m->handle;
}
EXPORT HDC WINAPI CreateEnhMetaFileA(HDC ref, LPCSTR filename, const RECT *rect, LPCSTR description) {
  WCHAR *f = dup_AtoW(filename, CP_ACP), *d = NULL;
  DWORD n = 0;
  HDC r;
  if (description) {
    while (description[n] || description[n + 1]) ++n;
    n += 2;
    d = mem_alloc_n(n + 1, sizeof(WCHAR));
    if (d) MultiByteToWideChar(CP_ACP, 0, description, n, d, n);
  }
  r = CreateEnhMetaFileW(ref, f, rect, d);
  mem_free(f); mem_free(d);
  return r;
}
EXPORT HENHMETAFILE WINAPI CloseEnhMetaFile(HDC hdc) {
  MetaDC *m = is_meta_dc(hdc) ? meta_get(hdc) : NULL;
  ENHMETAHEADER *h;
  DWORD eof[3];
  HANDLE result;
  if (!m || !m->enh) { SetLastError(ERROR_INVALID_HANDLE); return NULL; }
  eof[0] = 0; eof[1] = 16; eof[2] = 20;      /* nPalEntries, offPalEntries (EMREOF size), nSizeLast */
  eof[1] = 16;
  emf_record(m, EMR_EOF, eof, 12);
  h = (ENHMETAHEADER *)m->buf;
  h->iType = EMR_HEADER;
  h->nSize = m->header_size;
  h->dSignature = ENHMETA_SIGNATURE;
  h->nVersion = 0x10000;
  h->nBytes = m->len;
  h->nRecords = m->nrecords;
  h->nHandles = emf_max_handle(m);
  h->nDescription = m->desc_chars;
  h->offDescription = m->desc_chars ? sizeof(ENHMETAHEADER) : 0;
  h->szlDevice = m->dev;
  h->szlMillimeters = m->mm;
  h->szlMicrometers.cx = m->mm.cx * 1000; h->szlMicrometers.cy = m->mm.cy * 1000;
  if (m->bounds_set) h->rclBounds = m->bounds; else { h->rclBounds.left = h->rclBounds.top = 0; h->rclBounds.right = h->rclBounds.bottom = -1; }
  if (m->frame_given) h->rclFrame = m->frame;
  else if (m->bounds_set) {
    h->rclFrame.left = (LONG)((LONGLONG)m->bounds.left * m->mm.cx * 100 / m->dev.cx);
    h->rclFrame.top = (LONG)((LONGLONG)m->bounds.top * m->mm.cy * 100 / m->dev.cy);
    h->rclFrame.right = (LONG)((LONGLONG)(m->bounds.right + 1) * m->mm.cx * 100 / m->dev.cx) - 1;
    h->rclFrame.bottom = (LONG)((LONGLONG)(m->bounds.bottom + 1) * m->mm.cy * 100 / m->dev.cy) - 1;
  } else { h->rclFrame.left = h->rclFrame.top = 0; h->rclFrame.right = h->rclFrame.bottom = -1; }
  if (!meta_write_file(m)) { meta_destroy(m); return NULL; }
  {
    BYTE *data = m->buf;
    DWORD size = m->len;
    m->buf = NULL;
    result = metafile_add(1, data, size);
  }
  meta_destroy(m);
  return result;
}

EXPORT HDC WINAPI CreateMetaFileW(LPCWSTR filename) {
  MetaDC *m = meta_create(0, NULL, filename);
  METAHEADER h;
  if (!m) return NULL;
  memset(&h, 0, sizeof(h));
  meta_put(m, &h, sizeof(h));
  return m->handle;
}
EXPORT HDC WINAPI CreateMetaFileA(LPCSTR filename) {
  WCHAR *f = dup_AtoW(filename, CP_ACP);
  HDC r = CreateMetaFileW(f);
  mem_free(f);
  return r;
}
EXPORT HMETAFILE WINAPI CloseMetaFile(HDC hdc) {
  MetaDC *m = is_meta_dc(hdc) ? meta_get(hdc) : NULL;
  METAHEADER *h;
  HANDLE result;
  if (!m || m->enh) { SetLastError(ERROR_INVALID_HANDLE); return NULL; }
  wmf_record(m, 0, NULL, 0);
  h = (METAHEADER *)m->buf;
  h->mtType = 1; h->mtHeaderSize = 9; h->mtVersion = 0x300;
  h->mtSize = m->len / 2; h->mtNoObjects = (WORD)wmf_object_count(m); h->mtMaxRecord = m->max_record; h->mtNoParameters = 0;
  if (!meta_write_file(m)) { meta_destroy(m); return NULL; }
  {
    BYTE *data = m->buf;
    DWORD size = m->len;
    m->buf = NULL;
    result = metafile_add(0, data, size);
  }
  meta_destroy(m);
  return result;
}

/* ---- metafile objects --------------------------------------------------------------------------------------------------------------------------------------- */

EXPORT BOOL WINAPI DeleteEnhMetaFile(HENHMETAFILE h) { return metafile_delete(h, 1); }
EXPORT BOOL WINAPI DeleteMetaFile(HMETAFILE h) { return metafile_delete(h, 0); }

static BYTE *dup_bytes(const void *p, DWORD n) { BYTE *d = mem_alloc(n); if (d) memcpy(d, p, n); return d; }
static BOOL emf_valid_header(const BYTE *p, DWORD size) {
  const ENHMETAHEADER *h = (const ENHMETAHEADER *)p;
  return size >= 88 && h->iType == EMR_HEADER && h->dSignature == ENHMETA_SIGNATURE && h->nBytes <= size;
}
EXPORT HENHMETAFILE WINAPI SetEnhMetaFileBits(UINT size, const BYTE *bits) {
  if (!bits || !emf_valid_header(bits, size)) { SetLastError(ERROR_INVALID_DATA); return NULL; }
  return metafile_add(1, dup_bytes(bits, size), size);
}
EXPORT UINT WINAPI GetEnhMetaFileBits(HENHMETAFILE h, UINT size, LPBYTE buffer) {
  MetaFile *f = metafile_get(h);
  if (!f || !f->enh) { SetLastError(ERROR_INVALID_HANDLE); return 0; }
  if (buffer) { if (size < f->size) return 0; memcpy(buffer, f->data, f->size); }
  return f->size;
}
EXPORT UINT WINAPI GetEnhMetaFileHeader(HENHMETAFILE h, UINT size, LPENHMETAHEADER header) {
  MetaFile *f = metafile_get(h);
  const ENHMETAHEADER *src;
  if (!f || !f->enh) { SetLastError(ERROR_INVALID_HANDLE); return 0; }
  src = (const ENHMETAHEADER *)f->data;
  if (!header) return src->nSize;
  if (size < 8) { SetLastError(ERROR_BUFFER_OVERFLOW); return 0; }
  if (size > src->nSize) size = src->nSize;
  memcpy(header, f->data, size);
  return size;
}
EXPORT UINT WINAPI GetEnhMetaFileDescriptionW(HENHMETAFILE h, UINT count, LPWSTR buffer) {
  MetaFile *f = metafile_get(h);
  const ENHMETAHEADER *src;
  if (!f || !f->enh) { SetLastError(ERROR_INVALID_HANDLE); return GDI_ERROR; }
  src = (const ENHMETAHEADER *)f->data;
  if (!src->nDescription) return 0;
  if (!buffer) return src->nDescription;
  if (count > src->nDescription) count = src->nDescription;
  memcpy(buffer, f->data + src->offDescription, count * 2);
  return count;
}
EXPORT UINT WINAPI GetEnhMetaFileDescriptionA(HENHMETAFILE h, UINT count, LPSTR buffer) {
  const UINT n = GetEnhMetaFileDescriptionW(h, 0, NULL);
  WCHAR *w;
  int r;
  if (n == GDI_ERROR || !n) return n;
  if (!buffer) return n;
  w = mem_alloc_n(n, 2);
  if (!w) return GDI_ERROR;
  GetEnhMetaFileDescriptionW(h, n, w);
  r = WideCharToMultiByte(CP_ACP, 0, w, n, buffer, count, NULL, NULL);
  mem_free(w);
  return r ? (UINT)r : GDI_ERROR;
}
EXPORT UINT WINAPI GetEnhMetaFilePaletteEntries(HENHMETAFILE h, UINT count, LPPALETTEENTRY entries) {
  MetaFile *f = metafile_get(h);
  const ENHMETAHEADER *src;
  const BYTE *p, *end;
  if (!f || !f->enh) { SetLastError(ERROR_INVALID_HANDLE); return GDI_ERROR; }
  src = (const ENHMETAHEADER *)f->data;
  if (!entries) return src->nPalEntries;
  p = f->data + src->nSize; end = f->data + f->size;
  while (p + 8 <= end) {
    const DWORD *r = (const DWORD *)p;
    if (r[1] < 8) break;
    if (r[0] == EMR_EOF && r[1] >= 20) {
      const DWORD n = r[2];
      if (n && r[3] + n * 4 <= r[1]) { if (n < count) count = n; memcpy(entries, p + r[3], count * 4); return count; }
      return 0;
    }
    p += r[1];
  }
  return 0;
}
EXPORT UINT WINAPI GetEnhMetaFilePixelFormat(HENHMETAFILE h, UINT size, PIXELFORMATDESCRIPTOR *pfd) {
  MetaFile *f = metafile_get(h);
  const ENHMETAHEADER *src;
  if (!f || !f->enh) { SetLastError(ERROR_INVALID_HANDLE); return 0; }
  src = (const ENHMETAHEADER *)f->data;
  if (!src->cbPixelFormat) return 0;
  if (pfd) { if (size > src->cbPixelFormat) size = src->cbPixelFormat; memcpy(pfd, f->data + src->offPixelFormat, size); return size; }
  return src->cbPixelFormat;
}
EXPORT BOOL WINAPI IsValidEnhMetaRecord(const void *p, const void *q) { const DWORD *r = p; return r && r[1] >= 8 && (r[1] & 3) == 0; }
EXPORT BOOL WINAPI IsValidEnhMetaRecordOffExt(const void *p, const void *q, DWORD off, DWORD ext) { return IsValidEnhMetaRecord(p, q); }

static BYTE *read_whole_file(LPCWSTR name, DWORD *size) {
  HANDLE file = CreateFileW(name, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
  DWORD n, done = 0;
  BYTE *data;
  if (file == INVALID_HANDLE_VALUE) return NULL;
  n = GetFileSize(file, NULL);
  if (n == INVALID_FILE_SIZE || n < 8 || n > 0x3fffffff) { CloseHandle(file); SetLastError(ERROR_INVALID_DATA); return NULL; }
  data = mem_alloc(n);
  if (!data || !ReadFile(file, data, n, &done, NULL) || done != n) { mem_free(data); CloseHandle(file); return NULL; }
  CloseHandle(file);
  *size = n;
  return data;
}
EXPORT HENHMETAFILE WINAPI GetEnhMetaFileW(LPCWSTR name) {
  DWORD size;
  BYTE *data = read_whole_file(name, &size);
  if (!data) return NULL;
  if (!emf_valid_header(data, size)) { mem_free(data); SetLastError(ERROR_INVALID_DATA); return NULL; }
  return metafile_add(1, data, size);
}
EXPORT HENHMETAFILE WINAPI GetEnhMetaFileA(LPCSTR name) { WCHAR *w = dup_AtoW(name, CP_ACP); HENHMETAFILE r = GetEnhMetaFileW(w); mem_free(w); return r; }
static BOOL wmf_valid_header(const BYTE *p, DWORD size) {
  const METAHEADER *h = (const METAHEADER *)p;
  return size >= sizeof(METAHEADER) && h->mtHeaderSize == 9 && (h->mtType == 1 || h->mtType == 2) && (h->mtVersion == 0x300 || h->mtVersion == 0x100);
}
EXPORT HMETAFILE WINAPI GetMetaFileW(LPCWSTR name) {
  DWORD size;
  BYTE *data = read_whole_file(name, &size);
  if (!data) return NULL;
  if (!wmf_valid_header(data, size)) { mem_free(data); SetLastError(ERROR_INVALID_DATA); return NULL; }
  return metafile_add(0, data, size);
}
EXPORT HMETAFILE WINAPI GetMetaFileA(LPCSTR name) { WCHAR *w = dup_AtoW(name, CP_ACP); HMETAFILE r = GetMetaFileW(w); mem_free(w); return r; }
EXPORT HMETAFILE WINAPI SetMetaFileBitsEx(UINT size, const BYTE *bits) {
  if (!bits || !wmf_valid_header(bits, size)) { SetLastError(ERROR_INVALID_DATA); return NULL; }
  return metafile_add(0, dup_bytes(bits, size), size);
}
EXPORT UINT WINAPI GetMetaFileBitsEx(HMETAFILE h, UINT size, LPVOID buffer) {
  MetaFile *f = metafile_get(h);
  if (!f || f->enh) { SetLastError(ERROR_INVALID_HANDLE); return 0; }
  if (!buffer) return f->size;
  if (size < f->size) return 0;
  memcpy(buffer, f->data, f->size);
  return f->size;
}
static BOOL write_whole_file(LPCWSTR name, const void *data, DWORD size) {
  HANDLE file = CreateFileW(name, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
  DWORD done = 0;
  BOOL ok;
  if (file == INVALID_HANDLE_VALUE) return FALSE;
  ok = WriteFile(file, data, size, &done, NULL) && done == size;
  CloseHandle(file);
  return ok;
}
EXPORT HENHMETAFILE WINAPI CopyEnhMetaFileW(HENHMETAFILE h, LPCWSTR name) {
  MetaFile *f = metafile_get(h);
  if (!f || !f->enh) { SetLastError(ERROR_INVALID_HANDLE); return NULL; }
  if (name && !write_whole_file(name, f->data, f->size)) return NULL;
  return metafile_add(1, dup_bytes(f->data, f->size), f->size);
}
EXPORT HENHMETAFILE WINAPI CopyEnhMetaFileA(HENHMETAFILE h, LPCSTR name) { WCHAR *w = dup_AtoW(name, CP_ACP); HENHMETAFILE r = CopyEnhMetaFileW(h, w); mem_free(w); return r; }
EXPORT HMETAFILE WINAPI CopyMetaFileW(HMETAFILE h, LPCWSTR name) {
  MetaFile *f = metafile_get(h);
  if (!f || f->enh) { SetLastError(ERROR_INVALID_HANDLE); return NULL; }
  if (name && !write_whole_file(name, f->data, f->size)) return NULL;
  return metafile_add(0, dup_bytes(f->data, f->size), f->size);
}
EXPORT HMETAFILE WINAPI CopyMetaFileA(HMETAFILE h, LPCSTR name) { WCHAR *w = dup_AtoW(name, CP_ACP); HMETAFILE r = CopyMetaFileW(h, w); mem_free(w); return r; }

/* ---- EMF playback ------------------------------------------------------------------------------------------------------------------------------------------------ */

typedef struct EmfPlay { XFORM init; int have_init; } EmfPlay;
static EmfPlay *current_play;

static HGDIOBJ emf_handle(HANDLETABLE *ht, UINT n, DWORD index) {
  if (index & ENHMETA_STOCK_OBJECT) return GetStockObject(index & 0x7fffffff);
  return index < n ? ht->objectHandle[index] : NULL;
}
static void emf_store(HANDLETABLE *ht, UINT n, DWORD index, HGDIOBJ obj) {
  if (index && index < n) { if (ht->objectHandle[index]) DeleteObject(ht->objectHandle[index]); ht->objectHandle[index] = obj; }
  else if (obj) DeleteObject(obj);
}
static HDC emf_source_dc(HDC hdc, const BYTE *rec, DWORD offBmi, DWORD offBits, DWORD usage, HBITMAP *bm) {
  const BITMAPINFO *bi = (const BITMAPINFO *)(rec + offBmi);
  HDC src = CreateCompatibleDC(hdc);
  if (!src) return NULL;
  *bm = CreateDIBitmap(hdc, &bi->bmiHeader, CBM_INIT, rec + offBits, bi, usage);
  if (!*bm) { DeleteDC(src); return NULL; }
  NtGdiSelectBitmap(src, *bm);
  return src;
}
static POINT *emf_points(const DWORD *d, DWORD count, int small) {
  POINT *p = mem_alloc_n(count ? count : 1, sizeof(POINT));
  if (!p) return NULL;
  if (small) { const SHORT *s = (const SHORT *)d; for (DWORD i = 0; i < count; ++i) { p[i].x = s[2 * i]; p[i].y = s[2 * i + 1]; } }
  else memcpy(p, d, count * sizeof(POINT));
  return p;
}
static BOOL emf_play_poly(HDC hdc, DWORD type, const DWORD *d, DWORD size) {
  int small = type >= EMR_POLYBEZIER16 && type <= EMR_POLYLINETO16;
  const DWORD count = d[4];
  POINT *p;
  BOOL r = FALSE;
  const DWORD limit = (size - 8 - 20) / (small ? 4 : 8);
  if (count > limit) return FALSE;
  p = emf_points(d + 5, count, small);
  if (!p) return FALSE;
  switch (small ? type - EMR_POLYBEZIER16 + EMR_POLYBEZIER : type) {
    case EMR_POLYBEZIER: r = PolyBezier(hdc, p, count); break;
    case EMR_POLYGON: r = Polygon(hdc, p, count); break;
    case EMR_POLYLINE: r = Polyline(hdc, p, count); break;
    case EMR_POLYBEZIERTO: r = PolyBezierTo(hdc, p, count); break;
    case EMR_POLYLINETO: r = PolylineTo(hdc, p, count); break;
  }
  mem_free(p);
  return r;
}
static BOOL emf_play_polypoly(HDC hdc, DWORD type, const DWORD *d, DWORD size) {
  const int small = type == EMR_POLYPOLYLINE16 || type == EMR_POLYPOLYGON16;
  const int gon = type == EMR_POLYPOLYGON || type == EMR_POLYPOLYGON16;
  const DWORD n = d[4], total = d[5];
  POINT *p;
  BOOL r;
  if (n > size / 4 || total > size / 4) return FALSE;
  p = emf_points(d + 6 + n, total, small);
  if (!p) return FALSE;
  r = gon ? PolyPolygon(hdc, p, (const INT *)(d + 6), n) : PolyPolyline(hdc, p, d + 6, n);
  mem_free(p);
  return r;
}

EXPORT BOOL WINAPI PlayEnhMetaFileRecord(HDC hdc, LPHANDLETABLE ht, const ENHMETARECORD *rec, UINT n) {
  const DWORD *d = rec->dParm;
  const BYTE *base = (const BYTE *)rec;
  const DWORD size = rec->nSize;
  BOOL r = TRUE;
  if (size < 8) return FALSE;
  switch (rec->iType) {
    case EMR_HEADER: case EMR_EOF: break;
    case EMR_POLYBEZIER: case EMR_POLYGON: case EMR_POLYLINE: case EMR_POLYBEZIERTO: case EMR_POLYLINETO:
    case EMR_POLYBEZIER16: case EMR_POLYGON16: case EMR_POLYLINE16: case EMR_POLYBEZIERTO16: case EMR_POLYLINETO16: r = emf_play_poly(hdc, rec->iType, d, size); break;
    case EMR_POLYPOLYLINE: case EMR_POLYPOLYGON: case EMR_POLYPOLYLINE16: case EMR_POLYPOLYGON16: r = emf_play_polypoly(hdc, rec->iType, d, size); break;
    case EMR_SETWINDOWEXTEX: r = SetWindowExtEx(hdc, d[0], d[1], NULL); break;
    case EMR_SETWINDOWORGEX: r = SetWindowOrgEx(hdc, d[0], d[1], NULL); break;
    case EMR_SETVIEWPORTEXTEX: r = SetViewportExtEx(hdc, d[0], d[1], NULL); break;
    case EMR_SETVIEWPORTORGEX: r = SetViewportOrgEx(hdc, d[0], d[1], NULL); break;
    case EMR_SETBRUSHORGEX: r = SetBrushOrgEx(hdc, d[0], d[1], NULL); break;
    case EMR_SETPIXELV: r = SetPixelV(hdc, d[0], d[1], d[2]); break;
    case EMR_SETMAPPERFLAGS: SetMapperFlags(hdc, d[0]); break;
    case EMR_SETMAPMODE: SetMapMode(hdc, d[0]); break;
    case EMR_SETBKMODE: SetBkMode(hdc, d[0]); break;
    case EMR_SETPOLYFILLMODE: SetPolyFillMode(hdc, d[0]); break;
    case EMR_SETROP2: SetROP2(hdc, d[0]); break;
    case EMR_SETSTRETCHBLTMODE: SetStretchBltMode(hdc, d[0]); break;
    case EMR_SETTEXTALIGN: SetTextAlign(hdc, d[0]); break;
    case EMR_SETTEXTCOLOR: SetTextColor(hdc, d[0]); break;
    case EMR_SETBKCOLOR: SetBkColor(hdc, d[0]); break;
    case EMR_SETARCDIRECTION: SetArcDirection(hdc, d[0]); break;
    case EMR_SETMITERLIMIT: { FLOAT f; memcpy(&f, d, 4); SetMiterLimit(hdc, f, NULL); break; }
    case EMR_SETLAYOUT: SetLayout(hdc, d[0]); break;
    case EMR_SETTEXTJUSTIFICATION: r = SetTextJustification(hdc, d[0], d[1]); break;
    case EMR_OFFSETCLIPRGN: OffsetClipRgn(hdc, d[0], d[1]); break;
    case EMR_MOVETOEX: r = MoveToEx(hdc, d[0], d[1], NULL); break;
    case EMR_SETMETARGN: SetMetaRgn(hdc); break;
    case EMR_EXCLUDECLIPRECT: ExcludeClipRect(hdc, d[0], d[1], d[2], d[3]); break;
    case EMR_INTERSECTCLIPRECT: IntersectClipRect(hdc, d[0], d[1], d[2], d[3]); break;
    case EMR_SCALEVIEWPORTEXTEX: r = ScaleViewportExtEx(hdc, d[0], d[1], d[2], d[3], NULL); break;
    case EMR_SCALEWINDOWEXTEX: r = ScaleWindowExtEx(hdc, d[0], d[1], d[2], d[3], NULL); break;
    case EMR_SAVEDC: SaveDC(hdc); break;
    case EMR_RESTOREDC: r = RestoreDC(hdc, (INT)d[0]); break;
    case EMR_SETWORLDTRANSFORM: {
      XFORM x;
      memcpy(&x, d, sizeof(x));
      if (current_play && current_play->have_init) CombineTransform(&x, &x, &current_play->init);
      r = SetWorldTransform(hdc, &x);
      break;
    }
    case EMR_MODIFYWORLDTRANSFORM: {
      XFORM x;
      const DWORD mode = d[6];
      memcpy(&x, d, sizeof(x));
      if (mode == MWT_IDENTITY) {
        if (current_play && current_play->have_init) r = SetWorldTransform(hdc, &current_play->init); else r = ModifyWorldTransform(hdc, NULL, MWT_IDENTITY);
      } else if (mode == MWT_SET_ ) {
        if (current_play && current_play->have_init) CombineTransform(&x, &x, &current_play->init);
        r = SetWorldTransform(hdc, &x);
      } else r = ModifyWorldTransform(hdc, &x, mode);
      break;
    }
    case EMR_SELECTOBJECT: {
      const DWORD i = d[0];
      HGDIOBJ o = emf_handle(ht, n, i);
      if (!o) { r = FALSE; break; }
      if (gdi_handle_type(o) == NTGDI_OBJ_PAL) SelectPalette(hdc, o, FALSE); else SelectObject(hdc, o);
      break;
    }
    case EMR_CREATEPEN: emf_store(ht, n, d[0], CreatePenIndirect((const LOGPEN *)(d + 1))); break;
    case EMR_CREATEBRUSHINDIRECT: { LOGBRUSH lb; lb.lbStyle = d[1]; lb.lbColor = d[2]; lb.lbHatch = d[3]; emf_store(ht, n, d[0], CreateBrushIndirect(&lb)); break; }
    case EMR_DELETEOBJECT: if (d[0] < n && ht->objectHandle[d[0]]) { DeleteObject(ht->objectHandle[d[0]]); ht->objectHandle[d[0]] = NULL; } break;
    case EMR_EXTCREATEPEN: {
      const EXTLOGPEN *e = (const EXTLOGPEN *)(d + 5);
      LOGBRUSH lb;
      HPEN pen;
      lb.lbStyle = e->elpBrushStyle; lb.lbColor = e->elpColor; lb.lbHatch = e->elpHatch;
      pen = ExtCreatePen(e->elpPenStyle, e->elpWidth, &lb, e->elpNumEntries, e->elpNumEntries ? e->elpStyleEntry : NULL);
      emf_store(ht, n, d[0], pen);
      break;
    }
    case EMR_CREATEDIBPATTERNBRUSHPT: emf_store(ht, n, d[0], CreateDIBPatternBrushPt(base + d[2], d[1])); break;
    case EMR_EXTCREATEFONTINDIRECTW: emf_store(ht, n, d[0], CreateFontIndirectW((const LOGFONTW *)(d + 1))); break;
    case EMR_CREATEPALETTE: emf_store(ht, n, d[0], CreatePalette((const LOGPALETTE *)(d + 1))); break;
    case EMR_SELECTPALETTE: { HGDIOBJ o = emf_handle(ht, n, d[0]); if (o) SelectPalette(hdc, o, FALSE); break; }
    case EMR_SETPALETTEENTRIES: { HGDIOBJ o = emf_handle(ht, n, d[0]); if (o) SetPaletteEntries(o, d[1], d[2], (const PALETTEENTRY *)(d + 3)); break; }
    case EMR_RESIZEPALETTE: { HGDIOBJ o = emf_handle(ht, n, d[0]); if (o) ResizePalette(o, d[1]); break; }
    case EMR_REALIZEPALETTE: RealizePalette(hdc); break;
    case EMR_ANGLEARC: { FLOAT a, b; memcpy(&a, d + 3, 4); memcpy(&b, d + 4, 4); r = AngleArc(hdc, d[0], d[1], d[2], a, b); break; }
    case EMR_ELLIPSE: r = Ellipse(hdc, d[0], d[1], d[2], d[3]); break;
    case EMR_RECTANGLE: r = Rectangle(hdc, d[0], d[1], d[2], d[3]); break;
    case EMR_ROUNDRECT: r = RoundRect(hdc, d[0], d[1], d[2], d[3], d[4], d[5]); break;
    case EMR_ARC: r = Arc(hdc, d[0], d[1], d[2], d[3], d[4], d[5], d[6], d[7]); break;
    case EMR_CHORD: r = Chord(hdc, d[0], d[1], d[2], d[3], d[4], d[5], d[6], d[7]); break;
    case EMR_PIE: r = Pie(hdc, d[0], d[1], d[2], d[3], d[4], d[5], d[6], d[7]); break;
    case EMR_ARCTO: r = ArcTo(hdc, d[0], d[1], d[2], d[3], d[4], d[5], d[6], d[7]); break;
    case EMR_EXTFLOODFILL: r = ExtFloodFill(hdc, d[0], d[1], d[2], d[3]); break;
    case EMR_LINETO: r = LineTo(hdc, d[0], d[1]); break;
    case EMR_POLYDRAW: { const DWORD c = d[4]; POINT *p = c <= size / 9 ? emf_points(d + 5, c, 0) : NULL; if (p) { r = PolyDraw(hdc, p, (const BYTE *)(d + 5 + 2 * c), c); mem_free(p); } else r = FALSE; break; }
    case EMR_BEGINPATH: r = BeginPath(hdc); break;
    case EMR_ENDPATH: r = EndPath(hdc); break;
    case EMR_CLOSEFIGURE: r = CloseFigure(hdc); break;
    case EMR_FILLPATH: r = FillPath(hdc); break;
    case EMR_STROKEANDFILLPATH: r = StrokeAndFillPath(hdc); break;
    case EMR_STROKEPATH: r = StrokePath(hdc); break;
    case EMR_FLATTENPATH: r = FlattenPath(hdc); break;
    case EMR_WIDENPATH: r = WidenPath(hdc); break;
    case EMR_SELECTCLIPPATH: r = SelectClipPath(hdc, d[0]); break;
    case EMR_ABORTPATH: r = AbortPath(hdc); break;
    case EMR_GDICOMMENT: break;
    case EMR_FILLRGN: case EMR_FRAMERGN: case EMR_INVERTRGN: case EMR_PAINTRGN: {
      const int fill = rec->iType == EMR_FILLRGN, frame = rec->iType == EMR_FRAMERGN;
      const DWORD cb = d[4], off = 5 + (fill || frame ? 1 : 0) + (frame ? 2 : 0);
      HRGN rgn = cb && cb <= size ? ExtCreateRegion(NULL, cb, (const RGNDATA *)(d + off)) : NULL;
      HGDIOBJ brush = fill || frame ? emf_handle(ht, n, d[5]) : NULL;
      if (!rgn) { r = FALSE; break; }
      if (fill) r = FillRgn(hdc, rgn, brush);
      else if (frame) r = FrameRgn(hdc, rgn, brush, d[6], d[7]);
      else if (rec->iType == EMR_INVERTRGN) r = InvertRgn(hdc, rgn);
      else r = PaintRgn(hdc, rgn);
      DeleteObject(rgn);
      break;
    }
    case EMR_EXTSELECTCLIPRGN: {
      const DWORD cb = d[0];
      HRGN rgn = cb && cb <= size ? ExtCreateRegion(NULL, cb, (const RGNDATA *)(d + 2)) : NULL;
      r = ExtSelectClipRgn(hdc, rgn, d[1]) != ERROR;
      if (rgn) DeleteObject(rgn);
      break;
    }
    case EMR_BITBLT: case EMR_STRETCHBLT: case EMR_ALPHABLEND: case EMR_TRANSPARENTBLT: {
      /* d: bounds(0..3) xDest yDest cxDest cyDest(4..7) rop(8) xSrc ySrc(9,10) xform(11..16) bk(17) usage(18) offBmi cbBmi offBits cbBits(19..22) cxSrc cySrc(23,24) */
      const int stretch = rec->iType != EMR_BITBLT;
      HBITMAP bm = NULL;
      HDC src = NULL;
      if (d[20]) {
        if (d[19] > size || d[20] > size - d[19] || d[21] > size || d[22] > size - d[21]) { r = FALSE; break; }
        src = emf_source_dc(hdc, base, d[19], d[21], d[18], &bm);
        if (!src) { r = FALSE; break; }
        { XFORM x; memcpy(&x, d + 11, sizeof(x)); if (x.eM11 != 0 || x.eM22 != 0) { SetGraphicsMode(src, GM_ADVANCED); SetWorldTransform(src, &x); } }
      } else if (rec->iType != EMR_BITBLT) { r = FALSE; break; }
      if (rec->iType == EMR_BITBLT) r = src ? BitBlt(hdc, d[4], d[5], d[6], d[7], src, d[9], d[10], d[8]) : PatBlt(hdc, d[4], d[5], d[6], d[7], d[8]);
      else if (rec->iType == EMR_STRETCHBLT) r = StretchBlt(hdc, d[4], d[5], d[6], d[7], src, d[9], d[10], d[23], d[24], d[8]);
      else if (rec->iType == EMR_ALPHABLEND) { BLENDFUNCTION bf; memcpy(&bf, d + 8, 4); r = GdiAlphaBlend(hdc, d[4], d[5], d[6], d[7], src, d[9], d[10], d[23], d[24], bf); }
      else r = TransparentBlt(hdc, d[4], d[5], d[6], d[7], src, d[9], d[10], d[23], d[24], d[8]);
      (void)stretch;
      if (src) { DeleteDC(src); DeleteObject(bm); }
      break;
    }
    case EMR_STRETCHDIBITS: {
      /* bounds(0..3) xDest yDest xSrc ySrc cxSrc cySrc(4..9) offBmi cbBmi offBits cbBits(10..13) usage(14) rop(15) cxDest cyDest(16,17) */
      if (d[10] > size || d[11] > size - d[10] || d[12] > size || d[13] > size - d[12]) { r = FALSE; break; }
      r = StretchDIBits(hdc, d[4], d[5], d[16], d[17], d[6], d[7], d[8], d[9], base + d[12], (const BITMAPINFO *)(base + d[10]), d[14], d[15]) != (int)GDI_ERROR;
      break;
    }
    case EMR_SETDIBITSTODEVICE: {
      /* bounds(0..3) xDest yDest xSrc ySrc cxSrc cySrc(4..9) offBmi cbBmi offBits cbBits(10..13) usage(14) start(15) scans(16) */
      if (d[10] > size || d[11] > size - d[10] || d[12] > size || d[13] > size - d[12]) { r = FALSE; break; }
      r = SetDIBitsToDevice(hdc, d[4], d[5], d[8], d[9], d[6], d[7], d[15], d[16], base + d[12], (const BITMAPINFO *)(base + d[10]), d[14]) != 0;
      break;
    }
    case EMR_EXTTEXTOUTW: case EMR_EXTTEXTOUTA: {
      const EMREXTTEXTOUTW *t = (const EMREXTTEXTOUTW *)rec;
      const DWORD cnt = t->emrtext.nChars, off = t->emrtext.offString;
      const BOOL wide = rec->iType == EMR_EXTTEXTOUTW;
      RECT rc;
      const INT *dx = t->emrtext.offDx && t->emrtext.offDx < size ? (const INT *)(base + t->emrtext.offDx) : NULL;
      if (off > size || cnt > size || (wide ? cnt * 2 : cnt) > size - off) { r = FALSE; break; }
      rc.left = t->emrtext.rcl.left; rc.top = t->emrtext.rcl.top; rc.right = t->emrtext.rcl.right; rc.bottom = t->emrtext.rcl.bottom;
      if (dx && t->emrtext.offDx + cnt * 4 > size) dx = NULL;
      if (wide) r = ExtTextOutW(hdc, t->emrtext.ptlReference.x, t->emrtext.ptlReference.y, t->emrtext.fOptions, &rc, (const WCHAR *)(base + off), cnt, dx);
      else r = ExtTextOutA(hdc, t->emrtext.ptlReference.x, t->emrtext.ptlReference.y, t->emrtext.fOptions, &rc, (const char *)(base + off), cnt, dx);
      break;
    }
    default: break;      /* records this gdi32 does not play are skipped, as Wine does for the ones it lacks */
  }
  return r;
}

EXPORT BOOL WINAPI EnumEnhMetaFile(HDC hdc, HENHMETAFILE hemf, ENHMFENUMPROC proc, LPVOID param, const RECT *rect) {
  MetaFile *f = metafile_get(hemf);
  const ENHMETAHEADER *h;
  HANDLETABLE *ht;
  EmfPlay play, *saved_play;
  const BYTE *p, *end;
  UINT nh;
  int save_level = 0;
  BOOL ret = TRUE;
  if (!f || !f->enh) { SetLastError(ERROR_INVALID_HANDLE); return FALSE; }
  h = (const ENHMETAHEADER *)f->data;
  if (!emf_valid_header(f->data, f->size)) { SetLastError(ERROR_INVALID_DATA); return FALSE; }
  nh = h->nHandles ? h->nHandles : 1;
  ht = mem_alloc(sizeof(HANDLETABLE) + nh * sizeof(HGDIOBJ));
  if (!ht) return FALSE;
  ht->objectHandle[0] = hemf;
  memset(&play, 0, sizeof(play));
  if (hdc) {
    save_level = SaveDC(hdc);
    if (rect && h->szlDevice.cx && h->szlDevice.cy && h->szlMillimeters.cx && h->szlMillimeters.cy) {
      const double sx = (double)h->szlDevice.cx / ((double)h->szlMillimeters.cx * 100.0), sy = (double)h->szlDevice.cy / ((double)h->szlMillimeters.cy * 100.0);
      const double fw = (double)(h->rclFrame.right - h->rclFrame.left + 1) * sx, fh = (double)(h->rclFrame.bottom - h->rclFrame.top + 1) * sy;
      if (fw != 0 && fh != 0) {
        const double xs = (double)(rect->right - rect->left) / fw, ys = (double)(rect->bottom - rect->top) / fh;
        play.init.eM11 = (FLOAT)xs; play.init.eM12 = 0; play.init.eM21 = 0; play.init.eM22 = (FLOAT)ys;
        play.init.eDx = (FLOAT)(rect->left - h->rclFrame.left * sx * xs);
        play.init.eDy = (FLOAT)(rect->top - h->rclFrame.top * sy * ys);
        play.have_init = 1;
        SetGraphicsMode(hdc, GM_ADVANCED);
        SetWorldTransform(hdc, &play.init);
      }
    }
  }
  saved_play = current_play;
  current_play = &play;
  p = f->data; end = f->data + f->size;
  while (p + 8 <= end) {
    const ENHMETARECORD *rec = (const ENHMETARECORD *)p;
    if (rec->nSize < 8 || (rec->nSize & 3) || rec->nSize > (DWORD)(end - p)) { ret = FALSE; break; }
    if (!proc(hdc, ht, rec, nh, (LPARAM)param)) break;
    if (rec->iType == EMR_EOF) break;
    p += rec->nSize;
  }
  current_play = saved_play;
  for (UINT i = 1; i < nh; ++i) if (ht->objectHandle[i]) DeleteObject(ht->objectHandle[i]);
  if (hdc) RestoreDC(hdc, save_level);
  mem_free(ht);
  return ret;
}
static int CALLBACK emf_play_proc(HDC hdc, HANDLETABLE *ht, const ENHMETARECORD *rec, int n, LPARAM param) { PlayEnhMetaFileRecord(hdc, ht, rec, n); return 1; }
EXPORT BOOL WINAPI PlayEnhMetaFile(HDC hdc, HENHMETAFILE hemf, const RECT *rect) {
  if (!hdc) { SetLastError(ERROR_INVALID_HANDLE); return FALSE; }
  return EnumEnhMetaFile(hdc, hemf, emf_play_proc, NULL, rect);
}

/* ---- WMF playback ------------------------------------------------------------------------------------------------------------------------------------------------ */

#define WP(i) ((SHORT)w[i])
#define WDW(i) ((DWORD)w[i] | ((DWORD)w[(i) + 1] << 16))

static void wmf_store(HANDLETABLE *ht, UINT n, HGDIOBJ obj) {
  UINT i;
  if (!obj) return;
  for (i = 0; i < n; ++i) if (!ht->objectHandle[i]) { ht->objectHandle[i] = obj; return; }
  DeleteObject(obj);
}

static BOOL wmf_dib_blit(HDC hdc, int kind, const WORD *w, DWORD nw) {
  /* kind 0 DIBBITBLT, 1 DIBSTRETCHBLT, 2 STRETCHDIB, 3 SETDIBTODEV: the DIB follows the parameters */
  DWORD head = kind == 0 ? 8 : kind == 1 ? 10 : kind == 2 ? 11 : 9;
  const BITMAPINFO *bi;
  UINT usage = DIB_RGB_COLORS;
  DibHeader h;
  DWORD rop = 0;
  if (kind <= 1) rop = WDW(0);
  if (kind == 2) { rop = WDW(0); usage = w[2]; }
  if (kind == 3) usage = w[0];
  if (nw <= head + 1) {
    /* DIBBITBLT with no bitmap: a PatBlt (one dummy word follows the parameters) */
    if (kind == 0) return PatBlt(hdc, WP(5), WP(7), WP(4), WP(3), rop);
    return FALSE;
  }
  bi = (const BITMAPINFO *)(w + head);
  if (!dib_parse(bi, usage, &h)) return FALSE;
  {
    const BYTE *bits = (const BYTE *)bi + dib_info_bytes(&h);
    if ((const BYTE *)bits > (const BYTE *)(w + nw)) return FALSE;
    switch (kind) {
      case 0: { HBITMAP bm = CreateDIBitmap(hdc, &bi->bmiHeader, CBM_INIT, bits, bi, usage); HDC src = bm ? CreateCompatibleDC(hdc) : NULL; BOOL r = FALSE;
                if (src) { NtGdiSelectBitmap(src, bm); r = BitBlt(hdc, WP(7), WP(6), WP(5), WP(4), src, WP(3), WP(2), rop); DeleteDC(src); } if (bm) DeleteObject(bm); return r; }
      case 1: { HBITMAP bm = CreateDIBitmap(hdc, &bi->bmiHeader, CBM_INIT, bits, bi, usage); HDC src = bm ? CreateCompatibleDC(hdc) : NULL; BOOL r = FALSE;
                if (src) { NtGdiSelectBitmap(src, bm); r = StretchBlt(hdc, WP(9), WP(8), WP(7), WP(6), src, WP(5), WP(4), WP(3), WP(2), rop); DeleteDC(src); } if (bm) DeleteObject(bm); return r; }
      case 2: return StretchDIBits(hdc, WP(10), WP(9), WP(8), WP(7), WP(6), WP(5), WP(4), WP(3), bits, bi, usage, rop) != (int)GDI_ERROR;
      default: return SetDIBitsToDevice(hdc, WP(8), WP(7), WP(6), WP(5), WP(4), WP(3), WP(2), WP(1), bits, bi, usage) != 0;
    }
  }
}

EXPORT BOOL WINAPI PlayMetaFileRecord(HDC hdc, LPHANDLETABLE ht, LPMETARECORD rec, UINT n) {
  const WORD *w = (const WORD *)rec->rdParm;
  const DWORD nw = rec->rdSize >= 3 ? rec->rdSize - 3 : 0;
  BOOL r = TRUE;
  switch (rec->rdFunction) {
    case 0: break;
    case 0x0103: SetMapMode(hdc, WP(0)); break;
    case 0x0102: SetBkMode(hdc, WP(0)); break;
    case 0x0106: SetPolyFillMode(hdc, WP(0)); break;
    case 0x0104: SetROP2(hdc, WP(0)); break;
    case 0x0107: SetStretchBltMode(hdc, WP(0)); break;
    case 0x012E: SetTextAlign(hdc, w[0]); break;
    case 0x0108: SetTextCharacterExtra(hdc, WP(0)); break;
    case 0x0105: SetRelAbs(hdc, WP(0)); break;
    case 0x0201: SetBkColor(hdc, WDW(0)); break;
    case 0x0209: SetTextColor(hdc, WDW(0)); break;
    case 0x0231: SetMapperFlags(hdc, WDW(0)); break;
    case 0x020A: r = SetTextJustification(hdc, WP(1), WP(0)); break;
    case 0x020B: r = SetWindowOrgEx(hdc, WP(1), WP(0), NULL); break;
    case 0x020C: r = SetWindowExtEx(hdc, WP(1), WP(0), NULL); break;
    case 0x020D: r = SetViewportOrgEx(hdc, WP(1), WP(0), NULL); break;
    case 0x020E: r = SetViewportExtEx(hdc, WP(1), WP(0), NULL); break;
    case 0x020F: r = OffsetWindowOrgEx(hdc, WP(1), WP(0), NULL); break;
    case 0x0211: r = OffsetViewportOrgEx(hdc, WP(1), WP(0), NULL); break;
    case 0x0410: r = ScaleWindowExtEx(hdc, WP(3), WP(2), WP(1), WP(0), NULL); break;
    case 0x0412: r = ScaleViewportExtEx(hdc, WP(3), WP(2), WP(1), WP(0), NULL); break;
    case 0x0214: r = MoveToEx(hdc, WP(1), WP(0), NULL); break;
    case 0x0213: r = LineTo(hdc, WP(1), WP(0)); break;
    case 0x041B: r = Rectangle(hdc, WP(3), WP(2), WP(1), WP(0)); break;
    case 0x061C: r = RoundRect(hdc, WP(5), WP(4), WP(3), WP(2), WP(1), WP(0)); break;
    case 0x0418: r = Ellipse(hdc, WP(3), WP(2), WP(1), WP(0)); break;
    case 0x0817: r = Arc(hdc, WP(7), WP(6), WP(5), WP(4), WP(3), WP(2), WP(1), WP(0)); break;
    case 0x081A: r = Pie(hdc, WP(7), WP(6), WP(5), WP(4), WP(3), WP(2), WP(1), WP(0)); break;
    case 0x0830: r = Chord(hdc, WP(7), WP(6), WP(5), WP(4), WP(3), WP(2), WP(1), WP(0)); break;
    case 0x0324: case 0x0325: {
      const DWORD c = w[0];
      POINT *p = c * 2 + 1 <= nw ? mem_alloc_n(c ? c : 1, sizeof(POINT)) : NULL;
      if (!p) { r = FALSE; break; }
      for (DWORD i = 0; i < c; ++i) { p[i].x = WP(1 + 2 * i); p[i].y = WP(2 + 2 * i); }
      r = rec->rdFunction == 0x0324 ? Polygon(hdc, p, c) : Polyline(hdc, p, c);
      mem_free(p);
      break;
    }
    case 0x0538: {
      const DWORD c = w[0];
      DWORD total = 0, *counts;
      POINT *p;
      INT *ic;
      if (1 + c > nw) { r = FALSE; break; }
      counts = mem_alloc_n(c ? c : 1, 4); ic = (INT *)counts;
      for (DWORD i = 0; i < c; ++i) { counts[i] = w[1 + i]; total += w[1 + i]; }
      if (1 + c + total * 2 > nw) { mem_free(counts); r = FALSE; break; }
      p = mem_alloc_n(total ? total : 1, sizeof(POINT));
      if (!p) { mem_free(counts); r = FALSE; break; }
      for (DWORD i = 0; i < total; ++i) { p[i].x = WP(1 + c + 2 * i); p[i].y = WP(2 + c + 2 * i); }
      r = PolyPolygon(hdc, p, ic, c);
      mem_free(p); mem_free(counts);
      break;
    }
    case 0x041F: r = SetPixelV(hdc, WP(3), WP(2), WDW(0)); break;
    case 0x0419: r = ExtFloodFill(hdc, WP(3), WP(2), WDW(0), FLOODFILLBORDER); break;
    case 0x0548: r = ExtFloodFill(hdc, WP(4), WP(3), WDW(1), w[0]); break;
    case 0x0521: {
      const int c = (SHORT)w[0];
      if (c < 0 || (DWORD)((c + 1) / 2 + 3) > nw) { r = FALSE; break; }
      r = TextOutA(hdc, WP(1 + (c + 1) / 2 + 1), WP(1 + (c + 1) / 2), (const char *)(w + 1), c);
      break;
    }
    case 0x0a32: {
      const int c = (SHORT)w[2];
      const UINT opts = w[3];
      DWORD at = 4;
      RECT rc, *prc = NULL;
      const INT *dx = NULL;
      INT *dxs = NULL;
      if (c < 0) { r = FALSE; break; }
      if (opts & (ETO_OPAQUE | ETO_CLIPPED)) { if (nw < 8) { r = FALSE; break; } rc.left = WP(4); rc.top = WP(5); rc.right = WP(6); rc.bottom = WP(7); prc = &rc; at = 8; }
      if (at + (DWORD)(c + 1) / 2 > nw) { r = FALSE; break; }
      if (nw >= at + (DWORD)(c + 1) / 2 + (DWORD)c && c) {
        const WORD *dw = w + at + (c + 1) / 2;
        dxs = mem_alloc_n(c, sizeof(INT));
        if (dxs) { for (int i = 0; i < c; ++i) dxs[i] = (SHORT)dw[i]; dx = dxs; }
      }
      r = ExtTextOutA(hdc, WP(1), WP(0), opts, prc, (const char *)(w + at), c, dx);
      mem_free(dxs);
      break;
    }
    case 0x001E: SaveDC(hdc); break;
    case 0x0127: r = RestoreDC(hdc, WP(0)); break;
    case 0x02FA: { LOGPEN lp; lp.lopnStyle = w[0]; lp.lopnWidth.x = WP(1); lp.lopnWidth.y = WP(2); lp.lopnColor = WDW(3); wmf_store(ht, n, CreatePenIndirect(&lp)); break; }
    case 0x02FC: { LOGBRUSH lb; lb.lbStyle = w[0]; lb.lbColor = WDW(1); lb.lbHatch = w[3]; wmf_store(ht, n, CreateBrushIndirect(&lb)); break; }
    case 0x02FB: {
      LOGFONTW lf;
      const BYTE *b = (const BYTE *)w;
      memset(&lf, 0, sizeof(lf));
      if (nw < 25) { r = FALSE; break; }
      lf.lfHeight = WP(0); lf.lfWidth = WP(1); lf.lfEscapement = WP(2); lf.lfOrientation = WP(3); lf.lfWeight = WP(4);
      lf.lfItalic = b[10]; lf.lfUnderline = b[11]; lf.lfStrikeOut = b[12]; lf.lfCharSet = b[13]; lf.lfOutPrecision = b[14]; lf.lfClipPrecision = b[15]; lf.lfQuality = b[16]; lf.lfPitchAndFamily = b[17];
      MultiByteToWideChar(CP_ACP, 0, (const char *)b + 18, -1, lf.lfFaceName, LF_FACESIZE);
      lf.lfFaceName[LF_FACESIZE - 1] = 0;
      wmf_store(ht, n, CreateFontIndirectW(&lf));
      break;
    }
    case 0x00F7: if (nw >= 2) wmf_store(ht, n, CreatePalette((const LOGPALETTE *)w)); break;
    case 0x0142: {
      const BITMAPINFO *bi = (const BITMAPINFO *)(w + 2);
      if (nw < 4) { r = FALSE; break; }
      wmf_store(ht, n, CreateDIBPatternBrushPt(bi, w[1]));
      break;
    }
    case 0x012D: {
      HGDIOBJ o = w[0] < n ? ht->objectHandle[w[0]] : NULL;
      if (!o) { r = FALSE; break; }
      if (gdi_handle_type(o) == NTGDI_OBJ_PAL) SelectPalette(hdc, o, FALSE); else SelectObject(hdc, o);
      break;
    }
    case 0x01F0: if (w[0] < n && ht->objectHandle[w[0]]) { DeleteObject(ht->objectHandle[w[0]]); ht->objectHandle[w[0]] = NULL; } break;
    case 0x0234: { HGDIOBJ o = w[0] < n ? ht->objectHandle[w[0]] : NULL; if (o) SelectPalette(hdc, o, FALSE); break; }
    case 0x0035: RealizePalette(hdc); break;
    case 0x0415: ExcludeClipRect(hdc, WP(3), WP(2), WP(1), WP(0)); break;
    case 0x0416: IntersectClipRect(hdc, WP(3), WP(2), WP(1), WP(0)); break;
    case 0x0220: OffsetClipRgn(hdc, WP(1), WP(0)); break;
    case 0x061D: r = PatBlt(hdc, WP(5), WP(4), WP(3), WP(2), WDW(0)); break;
    case 0x0940: r = wmf_dib_blit(hdc, 0, w, nw); break;
    case 0x0b41: r = wmf_dib_blit(hdc, 1, w, nw); break;
    case 0x0f43: r = wmf_dib_blit(hdc, 2, w, nw); break;
    case 0x0d33: r = wmf_dib_blit(hdc, 3, w, nw); break;
    default: break;
  }
  return r;
}

static BOOL wmf_walk(HDC hdc, HMETAFILE hmf, MFENUMPROC proc, LPARAM param, int play) {
  MetaFile *f = metafile_get(hmf);
  const METAHEADER *h;
  HANDLETABLE *ht;
  HGDIOBJ pen = NULL, brush = NULL, font = NULL, pal = NULL;
  const BYTE *p, *end;
  UINT nh;
  int save = 0;
  BOOL ret = TRUE;
  if (!f || f->enh) { SetLastError(ERROR_INVALID_HANDLE); return FALSE; }
  if (!wmf_valid_header(f->data, f->size)) { SetLastError(ERROR_INVALID_DATA); return FALSE; }
  h = (const METAHEADER *)f->data;
  nh = h->mtNoObjects ? h->mtNoObjects : 1;
  ht = mem_alloc(sizeof(HANDLETABLE) + nh * sizeof(HGDIOBJ));
  if (!ht) return FALSE;
  if (hdc) {
    save = SaveDC(hdc);
    pen = GetCurrentObject(hdc, OBJ_PEN); brush = GetCurrentObject(hdc, OBJ_BRUSH); font = GetCurrentObject(hdc, OBJ_FONT); pal = GetCurrentObject(hdc, OBJ_PAL);
  }
  p = f->data + h->mtHeaderSize * 2; end = f->data + f->size;
  while (p + 6 <= end) {
    METARECORD *rec = (METARECORD *)p;
    if (rec->rdSize < 3 || rec->rdSize * 2u > (DWORD)(end - p)) { ret = FALSE; break; }
    if (play) PlayMetaFileRecord(hdc, ht, rec, nh);
    else if (!proc(hdc, ht, rec, nh, param)) break;
    if (rec->rdFunction == 0) break;
    p += rec->rdSize * 2;
  }
  if (hdc) {
    if (pen) SelectObject(hdc, pen);
    if (brush) SelectObject(hdc, brush);
    if (font) SelectObject(hdc, font);
    if (pal) SelectPalette(hdc, pal, FALSE);
  }
  for (UINT i = 0; i < nh; ++i) if (ht->objectHandle[i]) DeleteObject(ht->objectHandle[i]);
  if (hdc && save) RestoreDC(hdc, save);
  mem_free(ht);
  return ret;
}
EXPORT BOOL WINAPI EnumMetaFile(HDC hdc, HMETAFILE hmf, MFENUMPROC proc, LPARAM param) { return wmf_walk(hdc, hmf, proc, param, 0); }
EXPORT BOOL WINAPI PlayMetaFile(HDC hdc, HMETAFILE hmf) { return wmf_walk(hdc, hmf, NULL, 0, 1); }

/* ---- converting between the two --------------------------------------------------------------------------------------------------------------------------- */

#pragma pack(push, 2)
typedef struct PlaceableHeader { DWORD key; WORD hmf; SHORT left, top, right, bottom; WORD inch; DWORD reserved; WORD checksum; } PlaceableHeader;
#pragma pack(pop)

EXPORT HENHMETAFILE WINAPI SetWinMetaFileBits(UINT size, const BYTE *bits, HDC ref, const METAFILEPICT *mfp) {
  HMETAFILE wmf;
  HDC emf;
  HENHMETAFILE result;
  RECT frame;
  int mm = MM_ANISOTROPIC, xext = 0, yext = 0;
  const BYTE *data = bits;
  UINT n = size;
  HDC dev = ref;
  int own = 0;
  if (!bits || size < sizeof(METAHEADER)) { SetLastError(ERROR_INVALID_PARAMETER); return NULL; }
  if (size >= sizeof(PlaceableHeader) && ((const PlaceableHeader *)bits)->key == 0x9AC6CDD7u) {
    const PlaceableHeader *ph = (const PlaceableHeader *)bits;
    data += 22; n -= 22;
    xext = (ph->right - ph->left) * 2540 / (ph->inch ? ph->inch : 1440);
    yext = (ph->bottom - ph->top) * 2540 / (ph->inch ? ph->inch : 1440);
  }
  if (mfp) { mm = mfp->mm; if (mfp->xExt > 0) xext = mfp->xExt; if (mfp->yExt > 0) yext = mfp->yExt; }
  wmf = SetMetaFileBitsEx(n, data);
  if (!wmf) return NULL;
  if (!dev) { dev = open_dc(L"DISPLAY", NULL, NULL, NULL, 1); own = 1; }
  if (xext <= 0 || yext <= 0) { xext = NtGdiGetDeviceCaps(dev, HORZSIZE) * 100 / 2; yext = NtGdiGetDeviceCaps(dev, VERTSIZE) * 100 / 2; }
  frame.left = frame.top = 0; frame.right = xext; frame.bottom = yext;
  emf = CreateEnhMetaFileW(dev, NULL, &frame, NULL);
  if (!emf) { DeleteMetaFile(wmf); if (own) DeleteDC(dev); return NULL; }
  SetMapMode(emf, mm == MM_ISOTROPIC ? MM_ISOTROPIC : MM_ANISOTROPIC);
  SetWindowExtEx(emf, xext, yext, NULL);
  SetViewportExtEx(emf, xext * NtGdiGetDeviceCaps(dev, HORZRES) / (NtGdiGetDeviceCaps(dev, HORZSIZE) * 100), yext * NtGdiGetDeviceCaps(dev, VERTRES) / (NtGdiGetDeviceCaps(dev, VERTSIZE) * 100), NULL);
  PlayMetaFile(emf, wmf);
  result = CloseEnhMetaFile(emf);
  DeleteMetaFile(wmf);
  if (own) DeleteDC(dev);
  return result;
}
EXPORT UINT WINAPI GetWinMetaFileBits(HENHMETAFILE hemf, UINT size, LPBYTE buffer, INT mode, HDC ref) {
  MetaFile *f = metafile_get(hemf);
  const ENHMETAHEADER *h;
  HDC wmf, dev = ref;
  HMETAFILE hmf;
  RECT rc;
  int own = 0;
  UINT n = 0;
  if (!f || !f->enh) { SetLastError(ERROR_INVALID_HANDLE); return 0; }
  h = (const ENHMETAHEADER *)f->data;
  if (!dev) { dev = open_dc(L"DISPLAY", NULL, NULL, NULL, 1); own = 1; }
  wmf = CreateMetaFileW(NULL);
  if (!wmf) { if (own) DeleteDC(dev); return 0; }
  rc.left = rc.top = 0;
  rc.right = (h->rclFrame.right - h->rclFrame.left + 1) * NtGdiGetDeviceCaps(dev, HORZRES) / (NtGdiGetDeviceCaps(dev, HORZSIZE) * 100);
  rc.bottom = (h->rclFrame.bottom - h->rclFrame.top + 1) * NtGdiGetDeviceCaps(dev, VERTRES) / (NtGdiGetDeviceCaps(dev, VERTSIZE) * 100);
  SetMapMode(wmf, MM_ANISOTROPIC);
  SetWindowOrgEx(wmf, 0, 0, NULL);
  SetWindowExtEx(wmf, rc.right, rc.bottom, NULL);
  PlayEnhMetaFile(wmf, hemf, &rc);
  hmf = CloseMetaFile(wmf);
  if (own) DeleteDC(dev);
  if (!hmf) return 0;
  n = GetMetaFileBitsEx(hmf, 0, NULL);
  if (buffer && n && size >= n) GetMetaFileBitsEx(hmf, n, buffer); else if (buffer) n = 0;
  DeleteMetaFile(hmf);
  return n;
}


/* ---- the remaining hooks ------------------------------------------------------------------------------------------------------------------------------------ */

/* The kind of a client object that is a metafile (DC): read without the lock (gdi_handle_type is called from inside it); the lists only change at the head. */
static UINT meta_kind(const void *h) {
  for (MetaDC *m = meta_dcs; m; m = m->next) if (m->handle == (HANDLE)h) return m->enh ? NTGDI_OBJ_ENHMETADC : NTGDI_OBJ_METADC;
  for (MetaFile *f = meta_files; f; f = f->next) if (f->handle == (HANDLE)h) return f->enh ? NTGDI_OBJ_ENHMETAFILE : NTGDI_OBJ_METAFILE;
  return 0;
}
static int meta_is_enh(HDC hdc) { MetaDC *m = meta_get(hdc); return m && m->enh; }
static int meta_float_bits(float f) { int v; memcpy(&v, &f, 4); return v; }

static BOOL meta_lineto(HDC hdc, int x, int y) {
  MetaDC *m = meta_get(hdc);
  POINT p[2];
  RECTL b;
  if (!m) return FALSE;
  if (!NtGdiGetDCPoint(m->shadow, NtGdiGetCurrentPosition, &p[0])) p[0].x = p[0].y = 0;
  p[1].x = x; p[1].y = y;
  meta_bound_points(m, p, 2, &b);
  meta_simple(hdc, EMR_LINETO, 0x0213, 0, 2, x, y);
  NtGdiMoveTo(m->shadow, x, y, NULL);
  return TRUE;
}
static BOOL meta_polydraw(HDC hdc, const POINT *pts, const BYTE *types, int count) {
  MetaDC *m = meta_get(hdc);
  EmfBuild eb = {0};
  RECTL b;
  DWORD c = count;
  if (!m || count < 0) return FALSE;
  meta_bound_points(m, pts, count, &b);
  if (!m->enh) return TRUE;
  eb_put(&eb, &b, 16); eb_put(&eb, &c, 4); eb_put(&eb, pts, count * 8u); eb_put(&eb, types, count);
  eb_align(&eb);
  emf_record(m, EMR_POLYDRAW, eb.p, eb.len);
  mem_free(eb.p);
  return TRUE;
}
/* open: 1 BeginPath, 2 EndPath, 0 other */
static BOOL meta_path(HDC hdc, DWORD emr, int open) {
  MetaDC *m = meta_get(hdc);
  if (!m) return FALSE;
  if (open == 1) m->path_open = 1; else if (open == 2) m->path_open = 0;
  return m->enh ? emf_record(m, emr, NULL, 0) : TRUE;
}
static BOOL meta_xform(HDC hdc, const XFORM *x, DWORD mode, int has_mode) {
  MetaDC *m = meta_get(hdc);
  EmfBuild eb = {0};
  if (!m || !m->enh) return FALSE;
  eb_put(&eb, x, sizeof(*x));
  if (has_mode) eb_put(&eb, &mode, 4);
  emf_record(m, has_mode ? EMR_MODIFYWORLDTRANSFORM : EMR_SETWORLDTRANSFORM, eb.p, eb.len);
  mem_free(eb.p);
  return TRUE;
}
static int meta_save(HDC hdc) {
  MetaDC *m = meta_get(hdc);
  if (!m) return 0;
  meta_simple(hdc, EMR_SAVEDC, 0x001E, 0, 0);
  NtGdiSaveDC(m->shadow);
  return ++m->saved;
}
static BOOL meta_restore(HDC hdc, int level) {
  MetaDC *m = meta_get(hdc);
  int rel;
  if (!m) return FALSE;
  rel = level < 0 ? level : level - (m->saved + 1);
  if (rel >= 0 || -rel > m->saved) return FALSE;
  meta_simple(hdc, EMR_RESTOREDC, 0x0127, 0, 1, rel);
  NtGdiRestoreDC(m->shadow, level);
  m->saved += rel;
  return TRUE;
}
static BOOL meta_comment(HDC hdc, UINT size, const BYTE *data) {
  MetaDC *m = meta_get(hdc);
  EmfBuild eb = {0};
  DWORD n = size;
  if (!m || !m->enh) return FALSE;
  eb_put(&eb, &n, 4); eb_put(&eb, data, size);
  eb_align(&eb);
  emf_record(m, EMR_GDICOMMENT, eb.p, eb.len);
  mem_free(eb.p);
  return TRUE;
}
/* OffsetWindowOrgEx / OffsetViewportOrgEx after the shadow applied them: EMF has no offset record, so it stores the resulting origin. */
static void meta_offset_org(HDC hdc, int window, int x, int y) {
  MetaDC *m = meta_get(hdc);
  POINT p;
  if (!m) return;
  if (!m->enh) { meta_simple(hdc, 0, window ? 0x020F : 0x0211, 0, 2, x, y); return; }
  if (NtGdiGetDCPoint(m->shadow, window ? TcosPointWindowOrg : TcosPointViewportOrg, &p))
    meta_simple(hdc, window ? EMR_SETWINDOWORGEX : EMR_SETVIEWPORTORGEX, 0, 0, 2, p.x, p.y);
}
static BOOL meta_delete_dc(HDC hdc) {
  MetaDC *m = meta_get(hdc);
  if (!m) { SetLastError(ERROR_INVALID_HANDLE); return FALSE; }
  meta_destroy(m);
  return TRUE;
}
static int meta_handle_valid(HGDIOBJ h) {
  switch (gdi_handle_type(h)) {
    case NTGDI_OBJ_METADC: case NTGDI_OBJ_ENHMETADC: return meta_get((HDC)h) != NULL;
    case NTGDI_OBJ_METAFILE: case NTGDI_OBJ_ENHMETAFILE: return metafile_get(h) != NULL;
    default: return 0;
  }
}
