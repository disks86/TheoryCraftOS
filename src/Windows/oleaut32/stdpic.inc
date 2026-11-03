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

/* The standard picture object (OleCreatePictureIndirect, OleLoadPicture and friends): IPicture and its dispinterface, persistence in the OLE picture stream format
 * ("lt\0\0", a length, the file), rendering of bitmaps, icons and metafiles, and saving as BMP / ICO / EMF / WMF files. */

/* ---- a stream in memory (for saving through IStream-based calls) ---------------------------------------------------------------------------------------------- */
typedef struct so_memstream {
  IStream iface;
  LONG ref;
  BYTE *data;
  SIZE_T size, capacity, pos;
} so_memstream;

static so_memstream *so_memstream_from(IStream *iface) { return CONTAINING_RECORD(iface, so_memstream, iface); }
static const IStreamVtbl so_memstream_vtbl;

static HRESULT WINAPI so_ms_QueryInterface(IStream *iface, REFIID riid, void **ppv) {
  if (!ppv) return E_POINTER;
  if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_ISequentialStream) || IsEqualIID(riid, &IID_IStream)) { *ppv = iface; IStream_AddRef(iface); return S_OK; }
  *ppv = NULL;
  return E_NOINTERFACE;
}
static ULONG WINAPI so_ms_AddRef(IStream *iface) { return (ULONG)__sync_add_and_fetch(&so_memstream_from(iface)->ref, 1); }
static ULONG WINAPI so_ms_Release(IStream *iface) {
  so_memstream *m = so_memstream_from(iface);
  LONG ref = __sync_sub_and_fetch(&m->ref, 1);
  if (ref == 0) { oa_free(m->data); oa_free(m); }
  return (ULONG)ref;
}
static HRESULT WINAPI so_ms_Read(IStream *iface, void *buffer, ULONG count, ULONG *read) {
  so_memstream *m = so_memstream_from(iface);
  SIZE_T n = m->pos < m->size ? m->size - m->pos : 0;
  if (n > count) n = count;
  if (n) memcpy(buffer, m->data + m->pos, n);
  m->pos += n;
  if (read) *read = (ULONG)n;
  return n == count ? S_OK : S_FALSE;
}
static HRESULT so_ms_reserve(so_memstream *m, SIZE_T needed) {
  if (needed <= m->capacity) return S_OK;
  if (needed > SO_MAX_BLOB) return STG_E_MEDIUMFULL;
  SIZE_T capacity = m->capacity ? m->capacity : 256;
  while (capacity < needed) capacity *= 2;
  BYTE *grown = oa_realloc(m->data, capacity);
  if (!grown) return STG_E_MEDIUMFULL;
  m->data = grown; m->capacity = capacity;
  return S_OK;
}
static HRESULT WINAPI so_ms_Write(IStream *iface, const void *buffer, ULONG count, ULONG *written) {
  so_memstream *m = so_memstream_from(iface);
  if (written) *written = 0;
  if (count > SO_MAX_BLOB || m->pos > SO_MAX_BLOB - count) return STG_E_MEDIUMFULL;
  HRESULT hr = so_ms_reserve(m, m->pos + count);
  if (FAILED(hr)) return hr;
  if (m->pos > m->size) memset(m->data + m->size, 0, m->pos - m->size);
  memcpy(m->data + m->pos, buffer, count);
  m->pos += count;
  if (m->pos > m->size) m->size = m->pos;
  if (written) *written = count;
  return S_OK;
}
static HRESULT WINAPI so_ms_Seek(IStream *iface, LARGE_INTEGER move, DWORD origin, ULARGE_INTEGER *new_pos) {
  so_memstream *m = so_memstream_from(iface);
  LONGLONG base;
  if (origin == STREAM_SEEK_SET) base = 0;
  else if (origin == STREAM_SEEK_CUR) base = (LONGLONG)m->pos;
  else if (origin == STREAM_SEEK_END) base = (LONGLONG)m->size;
  else return STG_E_INVALIDFUNCTION;
  if ((move.QuadPart < 0 && base + move.QuadPart < 0) || move.QuadPart > (LONGLONG)SO_MAX_BLOB) return STG_E_INVALIDFUNCTION;
  m->pos = (SIZE_T)(base + move.QuadPart);
  if (new_pos) new_pos->QuadPart = m->pos;
  return S_OK;
}
static HRESULT WINAPI so_ms_SetSize(IStream *iface, ULARGE_INTEGER size) {
  so_memstream *m = so_memstream_from(iface);
  if (size.QuadPart > SO_MAX_BLOB) return STG_E_MEDIUMFULL;
  HRESULT hr = so_ms_reserve(m, (SIZE_T)size.QuadPart);
  if (FAILED(hr)) return hr;
  if (size.QuadPart > m->size) memset(m->data + m->size, 0, (SIZE_T)size.QuadPart - m->size);
  m->size = (SIZE_T)size.QuadPart;
  return S_OK;
}
static HRESULT WINAPI so_ms_CopyTo(IStream *iface, IStream *dest, ULARGE_INTEGER count, ULARGE_INTEGER *read, ULARGE_INTEGER *written) {
  so_memstream *m = so_memstream_from(iface);
  SIZE_T n = m->pos < m->size ? m->size - m->pos : 0;
  ULONG put = 0;
  if (!dest) return STG_E_INVALIDPOINTER;
  if (n > count.QuadPart) n = (SIZE_T)count.QuadPart;
  if (n > 0xFFFFFFFFu) n = 0xFFFFFFFFu;
  HRESULT hr = n ? IStream_Write(dest, m->data + m->pos, (ULONG)n, &put) : S_OK;
  m->pos += put;
  if (read) read->QuadPart = n;
  if (written) written->QuadPart = put;
  return hr;
}
static HRESULT WINAPI so_ms_Commit(IStream *iface, DWORD flags) { (void)iface; (void)flags; return S_OK; }
static HRESULT WINAPI so_ms_Revert(IStream *iface) { (void)iface; return S_OK; }
static HRESULT WINAPI so_ms_Lock(IStream *iface, ULARGE_INTEGER offset, ULARGE_INTEGER count, DWORD type) { (void)iface; (void)offset; (void)count; (void)type; return STG_E_INVALIDFUNCTION; }
static HRESULT WINAPI so_ms_Stat(IStream *iface, STATSTG *stat, DWORD flags) {
  so_memstream *m = so_memstream_from(iface);
  (void)flags;
  if (!stat) return STG_E_INVALIDPOINTER;
  memset(stat, 0, sizeof(*stat));
  stat->type = STGTY_STREAM;
  stat->cbSize.QuadPart = m->size;
  return S_OK;
}
static HRESULT WINAPI so_ms_Clone(IStream *iface, IStream **out) { (void)iface; if (out) *out = NULL; return E_NOTIMPL; }
static const IStreamVtbl so_memstream_vtbl = {
  so_ms_QueryInterface, so_ms_AddRef, so_ms_Release, so_ms_Read, so_ms_Write, so_ms_Seek, so_ms_SetSize, so_ms_CopyTo, so_ms_Commit, so_ms_Revert, so_ms_Lock, so_ms_Lock,
  so_ms_Stat, so_ms_Clone
};
static so_memstream *so_memstream_new(void) {
  so_memstream *m = oa_calloc(sizeof(*m));
  if (!m) return NULL;
  m->iface.lpVtbl = (void *)&so_memstream_vtbl;
  m->ref = 1;
  return m;
}

/* ---- screen resolution ------------------------------------------------------------------------------------------------------------------------------------- */
static LONG so_screen_dpi_x(void) {
  HDC dc = so_GetDC(NULL);
  LONG dpi = dc ? so_GetDeviceCaps(dc, LOGPIXELSX) : 0;
  if (dc) so_ReleaseDC(NULL, dc);
  return dpi > 0 ? dpi : 96;
}
static LONG so_px_to_himetric_x(LONG px) { return MulDiv(px, 2540, so_screen_dpi_x()); }
static LONG so_px_to_himetric_y(LONG px) { return MulDiv(px, 2540, so_screen_dpi_y()); }

/* ---- the object ------------------------------------------------------------------------------------------------------------------------------------------------ */
typedef struct so_pic {
  IPicture ipic;
  IDispatch idisp;
  IPersistStream ips;
  IPersistStreamInit ipsi;
  IConnectionPointContainer icpc;
  LONG ref;
  BOOL own;                       /* the object deletes its picture handle */
  PICTDESC desc;
  LONG himetric_w, himetric_h;
  BOOL keep_original;
  BYTE *original;                 /* the file the picture was loaded from */
  SIZE_T original_len;
  HDC cur_dc;
  ITypeInfo *typeinfo;
  so_cp cp_notify;
} so_pic;

static const IPictureVtbl so_pic_vtbl;
static const IDispatchVtbl so_pic_disp_vtbl;
static const IPersistStreamVtbl so_pic_ps_vtbl;
static const IPersistStreamInitVtbl so_pic_psi_vtbl;
static const IConnectionPointContainerVtbl so_pic_cpc_vtbl;

static so_pic *so_pic_from(IPicture *iface) { return CONTAINING_RECORD(iface, so_pic, ipic); }
static so_pic *so_pic_from_disp(IDispatch *iface) { return CONTAINING_RECORD(iface, so_pic, idisp); }
static so_pic *so_pic_from_ps(IPersistStream *iface) { return CONTAINING_RECORD(iface, so_pic, ips); }
static so_pic *so_pic_from_psi(IPersistStreamInit *iface) { return CONTAINING_RECORD(iface, so_pic, ipsi); }
static so_pic *so_pic_from_cpc(IConnectionPointContainer *iface) { return CONTAINING_RECORD(iface, so_pic, icpc); }

static void so_pic_release_handle(so_pic *p) {
  if (p->own) {
    switch (p->desc.picType) {
    case PICTYPE_BITMAP: if (p->desc.bmp.hbitmap) so_DeleteObject(p->desc.bmp.hbitmap); if (p->desc.bmp.hpal) so_DeleteObject(p->desc.bmp.hpal); break;
    case PICTYPE_ICON: if (p->desc.icon.hicon) so_DestroyIcon(p->desc.icon.hicon); break;
    case PICTYPE_METAFILE: if (p->desc.wmf.hmeta) so_DeleteMetaFile(p->desc.wmf.hmeta); break;
    case PICTYPE_ENHMETAFILE: if (p->desc.emf.hemf) so_DeleteEnhMetaFile(p->desc.emf.hemf); break;
    default: break;
    }
  }
  memset(&p->desc, 0, sizeof(p->desc));
  p->desc.cbSizeofstruct = sizeof(PICTDESC);
  p->desc.picType = PICTYPE_UNINITIALIZED;
  p->himetric_w = p->himetric_h = 0;
}

/* The size of the picture in HIMETRIC, from the handle that it holds. */
static void so_pic_measure(so_pic *p) {
  p->himetric_w = p->himetric_h = 0;
  switch (p->desc.picType) {
  case PICTYPE_BITMAP: {
    BITMAP bm;
    memset(&bm, 0, sizeof(bm));
    if (p->desc.bmp.hbitmap && so_GetObjectW(p->desc.bmp.hbitmap, sizeof(bm), &bm) > 0) {
      p->himetric_w = so_px_to_himetric_x(bm.bmWidth);
      p->himetric_h = so_px_to_himetric_y(bm.bmHeight);
    }
    break;
  }
  case PICTYPE_ICON: {
    ICONINFO ii;
    BITMAP bm;
    memset(&ii, 0, sizeof(ii));
    memset(&bm, 0, sizeof(bm));
    if (p->desc.icon.hicon && so_GetIconInfo(p->desc.icon.hicon, &ii)) {
      HBITMAP source = ii.hbmColor ? ii.hbmColor : ii.hbmMask;
      if (source && so_GetObjectW(source, sizeof(bm), &bm) > 0) {
        p->himetric_w = so_px_to_himetric_x(bm.bmWidth);
        p->himetric_h = so_px_to_himetric_y(ii.hbmColor ? bm.bmHeight : bm.bmHeight / 2);
      }
      if (ii.hbmColor) so_DeleteObject(ii.hbmColor);
      if (ii.hbmMask) so_DeleteObject(ii.hbmMask);
    }
    break;
  }
  case PICTYPE_METAFILE:
    p->himetric_w = p->desc.wmf.xExt;
    p->himetric_h = p->desc.wmf.yExt;
    break;
  case PICTYPE_ENHMETAFILE: {
    ENHMETAHEADER header;
    memset(&header, 0, sizeof(header));
    if (p->desc.emf.hemf && so_GetEnhMetaFileHeader(p->desc.emf.hemf, sizeof(header), &header)) {
      p->himetric_w = header.rclFrame.right - header.rclFrame.left;
      p->himetric_h = header.rclFrame.bottom - header.rclFrame.top;
    }
    break;
  }
  default: break;
  }
}

static so_pic *so_pic_new(void) {
  so_pic *p = oa_calloc(sizeof(*p));
  if (!p) return NULL;
  p->ipic.lpVtbl = (void *)&so_pic_vtbl;
  p->idisp.lpVtbl = (void *)&so_pic_disp_vtbl;
  p->ips.lpVtbl = (void *)&so_pic_ps_vtbl;
  p->ipsi.lpVtbl = (void *)&so_pic_psi_vtbl;
  p->icpc.lpVtbl = (void *)&so_pic_cpc_vtbl;
  p->ref = 1;
  p->own = TRUE;
  p->desc.cbSizeofstruct = sizeof(PICTDESC);
  p->desc.picType = PICTYPE_UNINITIALIZED;
  so_cp_init(&p->cp_notify, (IUnknown *)&p->ipic, &IID_IPropertyNotifySink, FALSE);
  __sync_add_and_fetch(&so_objects, 1);
  return p;
}

/* Replace the picture handle (the old one is released). */
static void so_pic_set(so_pic *p, const PICTDESC *desc, BOOL own) {
  so_pic_release_handle(p);
  p->desc = *desc;
  p->own = own;
  so_pic_measure(p);
}

/* ---- serialization ------------------------------------------------------------------------------------------------------------------------------------------- */
static void so_put16(BYTE *p, DWORD v) { p[0] = (BYTE)v; p[1] = (BYTE)(v >> 8); }
static void so_put32(BYTE *p, DWORD v) { p[0] = (BYTE)v; p[1] = (BYTE)(v >> 8); p[2] = (BYTE)(v >> 16); p[3] = (BYTE)(v >> 24); }

/* A bitmap as a BMP file: indexed bitmaps keep their depth, everything else is 24 bits. */
static BOOL so_serialize_bitmap(HBITMAP bitmap, BYTE **out, SIZE_T *out_len) {
  BITMAP bm;
  memset(&bm, 0, sizeof(bm));
  if (!bitmap || so_GetObjectW(bitmap, sizeof(bm), &bm) <= 0 || bm.bmWidth <= 0 || bm.bmHeight <= 0) return FALSE;
  WORD bpp = bm.bmBitsPixel <= 1 ? 1 : bm.bmBitsPixel <= 4 ? 4 : bm.bmBitsPixel <= 8 ? 8 : 24;
  ULONGLONG stride = (((ULONGLONG)bm.bmWidth * bpp + 31) / 32) * 4;
  ULONGLONG image = stride * (ULONGLONG)bm.bmHeight;
  SIZE_T palette = bpp <= 8 ? ((SIZE_T)1 << bpp) * 4 : 0;
  if (image > SO_MAX_BLOB) return FALSE;
  SIZE_T total = 14 + sizeof(BITMAPINFOHEADER) + palette + (SIZE_T)image;
  BYTE *file = oa_calloc(total);
  BITMAPINFO *info = oa_calloc(sizeof(BITMAPINFOHEADER) + 256 * sizeof(RGBQUAD));
  HDC dc = so_CreateCompatibleDC(NULL);
  if (!file || !info || !dc) { oa_free(file); oa_free(info); if (dc) so_DeleteDC(dc); return FALSE; }
  info->bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  info->bmiHeader.biWidth = bm.bmWidth; info->bmiHeader.biHeight = bm.bmHeight;
  info->bmiHeader.biPlanes = 1; info->bmiHeader.biBitCount = bpp; info->bmiHeader.biCompression = BI_RGB;
  int lines = so_GetDIBits(dc, bitmap, 0, (UINT)bm.bmHeight, file + 14 + sizeof(BITMAPINFOHEADER) + palette, info, DIB_RGB_COLORS);
  so_DeleteDC(dc);
  if (lines <= 0) { oa_free(file); oa_free(info); return FALSE; }
  file[0] = 'B'; file[1] = 'M';
  so_put32(file + 2, (DWORD)total);
  so_put32(file + 10, (DWORD)(14 + sizeof(BITMAPINFOHEADER) + palette));
  memcpy(file + 14, &info->bmiHeader, sizeof(BITMAPINFOHEADER));
  so_put32(file + 14 + 20, (DWORD)image);                       /* biSizeImage */
  so_put32(file + 14 + 32, 0);                                  /* biClrUsed: all of the palette */
  if (palette) memcpy(file + 14 + sizeof(BITMAPINFOHEADER), info->bmiColors, palette);
  oa_free(info);
  *out = file; *out_len = total;
  return TRUE;
}

/* An icon as an ICO file with one image. */
static BOOL so_serialize_icon(HICON icon, BYTE **out, SIZE_T *out_len) {
  ICONINFO ii;
  BITMAP bm;
  memset(&ii, 0, sizeof(ii));
  memset(&bm, 0, sizeof(bm));
  if (!icon || !so_GetIconInfo(icon, &ii)) return FALSE;
  BOOL ok = FALSE;
  BYTE *file = NULL;
  BITMAPINFO *info = oa_calloc(sizeof(BITMAPINFOHEADER) + 256 * sizeof(RGBQUAD));
  HDC dc = so_CreateCompatibleDC(NULL);
  HBITMAP shape = ii.hbmColor ? ii.hbmColor : ii.hbmMask;
  if (!info || !dc || !shape || so_GetObjectW(shape, sizeof(bm), &bm) <= 0) goto done;
  {
    LONG width = bm.bmWidth, height = ii.hbmColor ? bm.bmHeight : bm.bmHeight / 2;
    if (width <= 0 || height <= 0 || width > 256 || height > 256) goto done;
    WORD bpp = !ii.hbmColor ? 1 : bm.bmBitsPixel <= 1 ? 1 : bm.bmBitsPixel <= 4 ? 4 : bm.bmBitsPixel <= 8 ? 8 : bm.bmBitsPixel == 32 ? 32 : 24;
    SIZE_T palette = bpp <= 8 ? ((SIZE_T)1 << bpp) * 4 : 0;
    SIZE_T xor_stride = (((SIZE_T)width * bpp + 31) / 32) * 4, and_stride = (((SIZE_T)width + 31) / 32) * 4;
    SIZE_T image = sizeof(BITMAPINFOHEADER) + palette + xor_stride * (SIZE_T)height + and_stride * (SIZE_T)height;
    SIZE_T total = 22 + image;
    file = oa_calloc(total);
    if (!file) goto done;
    BYTE *header = file + 22, *pixels = header + sizeof(BITMAPINFOHEADER) + palette;
    info->bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info->bmiHeader.biWidth = width; info->bmiHeader.biPlanes = 1; info->bmiHeader.biBitCount = bpp; info->bmiHeader.biCompression = BI_RGB;
    if (ii.hbmColor) {
      info->bmiHeader.biHeight = height;
      if (so_GetDIBits(dc, ii.hbmColor, 0, (UINT)height, pixels, info, DIB_RGB_COLORS) <= 0) goto done;
      if (palette) memcpy(header + sizeof(BITMAPINFOHEADER), info->bmiColors, palette);
      info->bmiHeader.biBitCount = 1; info->bmiHeader.biHeight = height;
      if (so_GetDIBits(dc, ii.hbmMask, 0, (UINT)height, pixels + xor_stride * (SIZE_T)height, info, DIB_RGB_COLORS) <= 0) goto done;
    } else {
      /* a monochrome icon: one bitmap with the AND mask above the XOR image, which is the order of an ICO image read bottom-up */
      info->bmiHeader.biHeight = height * 2;
      if (so_GetDIBits(dc, ii.hbmMask, 0, (UINT)(height * 2), pixels, info, DIB_RGB_COLORS) <= 0) goto done;
      memcpy(header + sizeof(BITMAPINFOHEADER), info->bmiColors, palette);
    }
    BYTE *entry = file + 6;
    so_put16(file + 2, 1); so_put16(file + 4, 1);
    entry[0] = (BYTE)(width == 256 ? 0 : width); entry[1] = (BYTE)(height == 256 ? 0 : height); entry[2] = bpp >= 8 ? 0 : (BYTE)(1u << bpp);
    so_put16(entry + 4, 1); so_put16(entry + 6, bpp);
    so_put32(entry + 8, (DWORD)image); so_put32(entry + 12, 22);
    memcpy(header, &info->bmiHeader, sizeof(BITMAPINFOHEADER));
    so_put32(header + 0, sizeof(BITMAPINFOHEADER));
    so_put32(header + 4, (DWORD)width); so_put32(header + 8, (DWORD)(height * 2));
    so_put16(header + 12, 1); so_put16(header + 14, bpp); so_put32(header + 16, BI_RGB); so_put32(header + 20, 0);
    *out = file; *out_len = total;
    file = NULL;
    ok = TRUE;
  }
done:
  oa_free(file); oa_free(info);
  if (dc) so_DeleteDC(dc);
  if (ii.hbmColor) so_DeleteObject(ii.hbmColor);
  if (ii.hbmMask) so_DeleteObject(ii.hbmMask);
  return ok;
}

static BOOL so_serialize_emf(HENHMETAFILE emf, BYTE **out, SIZE_T *out_len) {
  UINT size = emf ? so_GetEnhMetaFileBits(emf, 0, NULL) : 0;
  if (!size || size > SO_MAX_BLOB) return FALSE;
  BYTE *bits = oa_alloc(size);
  if (!bits) return FALSE;
  if (so_GetEnhMetaFileBits(emf, size, bits) != size) { oa_free(bits); return FALSE; }
  *out = bits; *out_len = size;
  return TRUE;
}

/* A (placeable) Windows metafile: the header with the bounding box (in 1440ths of an inch) and the records. */
static BOOL so_serialize_wmf(HMETAFILE wmf, int x_ext, int y_ext, BYTE **out, SIZE_T *out_len) {
  UINT size = wmf ? so_GetMetaFileBitsEx(wmf, 0, NULL) : 0;
  if (!size || size > SO_MAX_BLOB) return FALSE;
  BYTE *file = oa_calloc((SIZE_T)size + 22);
  if (!file) return FALSE;
  if (so_GetMetaFileBitsEx(wmf, size, file + 22) != size) { oa_free(file); return FALSE; }
  so_put32(file, 0x9AC6CDD7);
  so_put16(file + 10, (DWORD)MulDiv(x_ext, 1440, 2540));
  so_put16(file + 12, (DWORD)MulDiv(y_ext, 1440, 2540));
  so_put16(file + 14, 1440);
  WORD checksum = 0;
  for (int i = 0; i < 10; i++) checksum ^= so_u16(file + 2 * i);
  so_put16(file + 20, checksum);
  *out = file; *out_len = (SIZE_T)size + 22;
  return TRUE;
}

/* The bytes that stand for the picture in a file (what SaveAsFile writes with the "memory copy" flag). */
static BOOL so_pic_serialize(const so_pic *p, BYTE **out, SIZE_T *len) {
  switch (p->desc.picType) {
  case PICTYPE_BITMAP: return so_serialize_bitmap(p->desc.bmp.hbitmap, out, len);
  case PICTYPE_ICON: return so_serialize_icon(p->desc.icon.hicon, out, len);
  case PICTYPE_METAFILE: return so_serialize_wmf(p->desc.wmf.hmeta, p->desc.wmf.xExt, p->desc.wmf.yExt, out, len);
  case PICTYPE_ENHMETAFILE: return so_serialize_emf(p->desc.emf.hemf, out, len);
  default: return FALSE;
  }
}

/* ---- loading ----------------------------------------------------------------------------------------------------------------------------------------------- */
static HBITMAP so_dib_to_bitmap(const so_dib *d) {
  BITMAPINFO *info = oa_calloc(sizeof(BITMAPINFOHEADER) + 256 * sizeof(RGBQUAD));
  void *bits = NULL;
  if (!info) return NULL;
  info->bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  info->bmiHeader.biWidth = d->width; info->bmiHeader.biHeight = d->height;
  info->bmiHeader.biPlanes = 1; info->bmiHeader.biBitCount = d->bpp; info->bmiHeader.biCompression = BI_RGB;
  info->bmiHeader.biSizeImage = (DWORD)d->size;
  info->bmiHeader.biClrUsed = d->bpp <= 8 ? 1u << d->bpp : 0; info->bmiHeader.biClrImportant = info->bmiHeader.biClrUsed;      /* (the whole palette: GDI reports it that way) */
  info->bmiHeader.biXPelsPerMeter = d->x_ppm; info->bmiHeader.biYPelsPerMeter = d->y_ppm;
  memcpy(info->bmiColors, d->palette, sizeof(d->palette));
  HBITMAP bitmap = so_CreateDIBSection(NULL, info, DIB_RGB_COLORS, &bits, NULL, 0);
  oa_free(info);
  if (bitmap && bits) memcpy(bits, d->bits, d->size);
  return bitmap;
}

static HRESULT so_pic_set_dib(so_pic *p, const so_dib *d) {
  PICTDESC desc;
  HBITMAP bitmap = so_dib_to_bitmap(d);
  if (!bitmap) return E_FAIL;
  memset(&desc, 0, sizeof(desc));
  desc.cbSizeofstruct = sizeof(desc);
  desc.picType = PICTYPE_BITMAP;
  desc.bmp.hbitmap = bitmap;
  so_pic_set(p, &desc, TRUE);
  /* the size is the one the file states (a resolution) or else the screen's */
  if (d->x_ppm > 0) p->himetric_w = MulDiv(d->width, 100000, d->x_ppm);
  if (d->y_ppm > 0) p->himetric_h = MulDiv(d->height, 100000, d->y_ppm);
  return S_OK;
}

/* A picture file's bytes; the picture keeps a copy (for saving it in its original format). */
static HRESULT so_pic_set_from_data(so_pic *p, const BYTE *d, SIZE_T len) {
  HRESULT hr = E_FAIL;
  so_dib dib;
  PICTDESC desc;
  memset(&desc, 0, sizeof(desc));
  desc.cbSizeofstruct = sizeof(desc);
  if (len >= 2 && d[0] == 'B' && d[1] == 'M') {
    if (so_decode_bmp(d, len, TRUE, &dib)) { hr = so_pic_set_dib(p, &dib); so_dib_free(&dib); }
  } else if (len >= 4 && d[0] == 'G' && d[1] == 'I' && d[2] == 'F' && d[3] == '8') {
    if (so_decode_gif(d, len, &dib)) { hr = so_pic_set_dib(p, &dib); so_dib_free(&dib); }
  } else if (len >= 3 && d[0] == 0xFF && d[1] == 0xD8 && d[2] == 0xFF) {
    if (so_decode_jpeg(d, len, &dib)) { hr = so_pic_set_dib(p, &dib); so_dib_free(&dib); }
  } else if (len >= 6 && d[0] == 0 && d[1] == 0 && (d[2] == 1 || d[2] == 2) && d[3] == 0) {
    SIZE_T offset, size;
    BOOL is_icon;
    if (so_ico_pick(d, len, 32, &offset, &size, &is_icon) && size <= 0xFFFFFFFFu) {
      HICON icon = so_CreateIconFromResourceEx((PBYTE)(d + offset), (DWORD)size, is_icon, 0x00030000, 0, 0, 0);
      if (icon) { desc.picType = PICTYPE_ICON; desc.icon.hicon = icon; so_pic_set(p, &desc, TRUE); hr = S_OK; }
    }
  } else if (len >= 22 + 18 && so_u32(d) == 0x9AC6CDD7) {
    /* a placeable metafile: the box is in units of 1/inch; the picture size is in HIMETRIC */
    LONG left = (short)so_u16(d + 6), top = (short)so_u16(d + 8), right = (short)so_u16(d + 10), bottom = (short)so_u16(d + 12);
    DWORD inch = so_u16(d + 14);
    const BYTE *wmf = d + 22;
    SIZE_T wmf_len = len - 22;
    DWORD type = so_u16(wmf), header_size = so_u16(wmf + 2);
    SIZE_T declared = (SIZE_T)so_u32(wmf + 6) * 2;
    if ((type == 1 || type == 2) && header_size == 9 && declared >= 18 && declared <= wmf_len) {
      HMETAFILE hmeta = so_SetMetaFileBitsEx((UINT)declared, wmf);
      if (hmeta) {
        if (!inch) inch = 1440;
        desc.picType = PICTYPE_METAFILE;
        desc.wmf.hmeta = hmeta;
        desc.wmf.xExt = MulDiv(right - left < 0 ? left - right : right - left, 2540, (int)inch);
        desc.wmf.yExt = MulDiv(bottom - top < 0 ? top - bottom : bottom - top, 2540, (int)inch);
        so_pic_set(p, &desc, TRUE);
        hr = S_OK;
      }
    }
  } else if (len >= 88 && so_u32(d) == 1 && so_u32(d + 40) == 0x464D4520 && len <= 0xFFFFFFFFu) {
    DWORD bytes = so_u32(d + 48);
    if (bytes >= 88 && bytes <= len) {
      HENHMETAFILE hemf = so_SetEnhMetaFileBits((UINT)bytes, d);
      if (hemf) { desc.picType = PICTYPE_ENHMETAFILE; desc.emf.hemf = hemf; so_pic_set(p, &desc, TRUE); hr = S_OK; }
    }
  }
  if (hr == S_OK) {
    oa_free(p->original);
    p->original = oa_alloc(len);
    p->original_len = 0;
    if (p->original) { memcpy(p->original, d, len); p->original_len = len; }
  }
  return hr;
}

/* Reads up to `limit` bytes (0 for as many as the stream has, at most SO_MAX_BLOB) into a new buffer that starts with `prefix`. The buffer grows as data really arrives, so a
 * stream that claims to be large costs nothing until it delivers. */
static HRESULT so_read_stream(IStream *stream, SIZE_T prefix_len, const BYTE *prefix, SIZE_T limit, BYTE **out, SIZE_T *out_len) {
  SIZE_T bound = limit ? limit : SO_MAX_BLOB;
  SIZE_T capacity = 65536, len = 0;
  if (bound > SO_MAX_BLOB || prefix_len > bound) return E_FAIL;
  if (capacity > bound) capacity = bound;
  if (capacity < prefix_len) capacity = prefix_len;
  BYTE *buf = oa_alloc(capacity ? capacity : 1);
  if (!buf) return E_OUTOFMEMORY;
  if (prefix_len) { memcpy(buf, prefix, prefix_len); len = prefix_len; }
  for (;;) {
    if (len >= bound) break;
    if (len == capacity) {
      SIZE_T grown = capacity > bound / 2 ? bound : capacity * 2;
      BYTE *bigger = oa_realloc(buf, grown);
      if (!bigger) { oa_free(buf); return E_OUTOFMEMORY; }
      buf = bigger; capacity = grown;
    }
    SIZE_T room = capacity - len;
    ULONG want = (ULONG)(room > 0x100000 ? 0x100000 : room), got = 0;
    HRESULT hr = IStream_Read(stream, buf + len, want, &got);
    if (FAILED(hr)) { oa_free(buf); return hr; }
    len += got;
    if (got == 0) break;
  }
  *out = buf; *out_len = len;
  return S_OK;
}

static BOOL so_is_lt_header(const BYTE *d) { return d[0] == 'l' && d[1] == 't' && d[2] == 0 && d[3] == 0; }

/* IPersistStream::Load / OleLoadPicture: an OLE picture stream ("lt\0\0" + size + file) or a bare picture file. */
static HRESULT so_pic_load(so_pic *p, IStream *stream, LONG size) {
  BYTE head[8];
  ULONG got = 0;
  BYTE *buf = NULL;
  SIZE_T len = 0;
  HRESULT hr;
  if (!stream) return E_INVALIDARG;
  hr = IStream_Read(stream, head, 8, &got);
  if (FAILED(hr)) return hr;
  if (got == 8 && so_is_lt_header(head)) {
    DWORD declared = so_u32(head + 4);
    if (declared == 0) {                                /* an empty picture */
      PICTDESC desc;
      memset(&desc, 0, sizeof(desc));
      desc.cbSizeofstruct = sizeof(desc);
      desc.picType = PICTYPE_NONE;
      so_pic_set(p, &desc, TRUE);
      return S_OK;
    }
    if (declared > SO_MAX_BLOB) return E_FAIL;
    hr = so_read_stream(stream, 0, NULL, declared, &buf, &len);
    if (FAILED(hr)) return hr;
    /* repeated headers are skipped; each one takes 8 more bytes of the picture from the stream */
    while (len >= 8 && so_is_lt_header(buf)) {
      memmove(buf, buf + 8, len - 8);
      len -= 8;
      BYTE *more = NULL;
      SIZE_T more_len = 0;
      if (FAILED(so_read_stream(stream, 0, NULL, 8, &more, &more_len))) break;
      if (more_len) memcpy(buf + len, more, more_len);
      len += more_len;
      oa_free(more);
    }
  } else {
    SIZE_T limit = 0;
    if (size > 0) limit = (SIZE_T)size;
    if (limit && limit < got) limit = got;
    hr = so_read_stream(stream, got, head, limit, &buf, &len);
    if (FAILED(hr)) return hr;
  }
  hr = len ? so_pic_set_from_data(p, buf, len) : E_FAIL;
  oa_free(buf);
  return hr;
}

/* ---- IPicture ----------------------------------------------------------------------------------------------------------------------------------------------- */
static HRESULT WINAPI so_pic_QueryInterface(IPicture *iface, REFIID riid, void **ppv) {
  so_pic *p = so_pic_from(iface);
  if (!ppv) return E_POINTER;
  *ppv = NULL;
  if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IPicture)) *ppv = &p->ipic;
  else if (IsEqualIID(riid, &IID_IDispatch) || IsEqualIID(riid, &IID_IPictureDisp)) *ppv = &p->idisp;
  else if (IsEqualIID(riid, &IID_IPersist) || IsEqualIID(riid, &IID_IPersistStream)) *ppv = &p->ips;
  else if (IsEqualIID(riid, &IID_IPersistStreamInit)) *ppv = &p->ipsi;
  else if (IsEqualIID(riid, &IID_IConnectionPointContainer)) *ppv = &p->icpc;
  if (!*ppv) return E_NOINTERFACE;
  IPicture_AddRef(iface);
  return S_OK;
}
static ULONG WINAPI so_pic_AddRef(IPicture *iface) { return (ULONG)__sync_add_and_fetch(&so_pic_from(iface)->ref, 1); }
static ULONG WINAPI so_pic_Release(IPicture *iface) {
  so_pic *p = so_pic_from(iface);
  LONG ref = __sync_sub_and_fetch(&p->ref, 1);
  if (ref == 0) {
    so_pic_release_handle(p);
    if (p->typeinfo) ITypeInfo_Release(p->typeinfo);
    so_cp_free(&p->cp_notify);
    oa_free(p->original);
    oa_free(p);
    __sync_sub_and_fetch(&so_objects, 1);
  }
  return (ULONG)ref;
}
static HRESULT WINAPI so_pic_get_Handle(IPicture *iface, OLE_HANDLE *handle) {
  so_pic *p = so_pic_from(iface);
  if (!handle) return E_POINTER;
  switch (p->desc.picType) {
  case PICTYPE_BITMAP: *handle = (OLE_HANDLE)(ULONG_PTR)p->desc.bmp.hbitmap; break;
  case PICTYPE_ICON: *handle = (OLE_HANDLE)(ULONG_PTR)p->desc.icon.hicon; break;
  case PICTYPE_METAFILE: *handle = (OLE_HANDLE)(ULONG_PTR)p->desc.wmf.hmeta; break;
  case PICTYPE_ENHMETAFILE: *handle = (OLE_HANDLE)(ULONG_PTR)p->desc.emf.hemf; break;
  default: *handle = 0; break;
  }
  return S_OK;
}
static HRESULT WINAPI so_pic_get_hPal(IPicture *iface, OLE_HANDLE *handle) {
  so_pic *p = so_pic_from(iface);
  if (!handle) return E_POINTER;
  if (p->desc.picType != PICTYPE_BITMAP) return E_FAIL;
  *handle = (OLE_HANDLE)(ULONG_PTR)p->desc.bmp.hpal;
  return S_OK;
}
static HRESULT WINAPI so_pic_get_Type(IPicture *iface, SHORT *type) {
  if (!type) return E_POINTER;
  *type = (SHORT)so_pic_from(iface)->desc.picType;
  return S_OK;
}
static HRESULT WINAPI so_pic_get_Width(IPicture *iface, OLE_XSIZE_HIMETRIC *width) {
  if (!width) return E_POINTER;
  *width = so_pic_from(iface)->himetric_w;
  return S_OK;
}
static HRESULT WINAPI so_pic_get_Height(IPicture *iface, OLE_YSIZE_HIMETRIC *height) {
  if (!height) return E_POINTER;
  *height = so_pic_from(iface)->himetric_h;
  return S_OK;
}
static HRESULT WINAPI so_pic_Render(IPicture *iface, HDC dc, LONG x, LONG y, LONG cx, LONG cy, OLE_XPOS_HIMETRIC xs, OLE_YPOS_HIMETRIC ys, OLE_XSIZE_HIMETRIC cxs,
                                    OLE_YSIZE_HIMETRIC cys, LPCRECT bounds) {
  so_pic *p = so_pic_from(iface);
  (void)bounds;
  if (!cx || !cy || !cxs || !cys) return CTL_E_INVALIDPROPERTYVALUE;
  switch (p->desc.picType) {
  case PICTYPE_BITMAP: {
    BITMAP bm;
    memset(&bm, 0, sizeof(bm));
    if (!p->desc.bmp.hbitmap || so_GetObjectW(p->desc.bmp.hbitmap, sizeof(bm), &bm) <= 0) return E_FAIL;
    if (p->himetric_w <= 0 || p->himetric_h <= 0) return S_OK;
    HDC mem = so_CreateCompatibleDC(dc);
    if (!mem) return E_FAIL;
    HGDIOBJ old = so_SelectObject(mem, p->desc.bmp.hbitmap);
    /* the source box is in HIMETRIC with the origin at the bottom left; the bitmap's rows run from the top */
    LONG sx = MulDiv(xs, bm.bmWidth, p->himetric_w), sw = MulDiv(cxs, bm.bmWidth, p->himetric_w);
    LONG sy = MulDiv(p->himetric_h - ys, bm.bmHeight, p->himetric_h), sh = MulDiv(-cys, bm.bmHeight, p->himetric_h);
    if (sw == 0) sw = cxs < 0 ? -1 : 1;
    if (sh == 0) sh = cys > 0 ? -1 : 1;
    so_StretchBlt(dc, x, y, cx, cy, mem, sx, sy, sw, sh, SRCCOPY);
    if (old) so_SelectObject(mem, old);
    so_DeleteDC(mem);
    return S_OK;
  }
  case PICTYPE_ICON:
    if (p->desc.icon.hicon) so_DrawIconEx(dc, x, y, p->desc.icon.hicon, cx < 0 ? -cx : cx, cy < 0 ? -cy : cy, 0, NULL, DI_NORMAL);
    return S_OK;
  case PICTYPE_METAFILE: {
    if (!p->desc.wmf.hmeta) return S_OK;
    int saved = so_SaveDC(dc);
    so_SetMapMode(dc, MM_ANISOTROPIC);
    so_SetWindowOrgEx(dc, xs, p->himetric_h - ys, NULL);
    so_SetWindowExtEx(dc, cxs, -cys, NULL);
    so_SetViewportOrgEx(dc, x, y, NULL);
    so_SetViewportExtEx(dc, cx, cy, NULL);
    so_PlayMetaFile(dc, p->desc.wmf.hmeta);
    if (saved) so_RestoreDC(dc, saved);
    return S_OK;
  }
  case PICTYPE_ENHMETAFILE: {
    RECT r;
    if (!p->desc.emf.hemf) return S_OK;
    r.left = x; r.top = y; r.right = x + cx; r.bottom = y + cy;
    so_PlayEnhMetaFile(dc, p->desc.emf.hemf, &r);
    return S_OK;
  }
  default: return S_OK;
  }
}
static HRESULT WINAPI so_pic_set_hPal(IPicture *iface, OLE_HANDLE handle) {
  so_pic *p = so_pic_from(iface);
  if (p->desc.picType != PICTYPE_BITMAP) return E_FAIL;
  p->desc.bmp.hpal = (HPALETTE)(ULONG_PTR)handle;
  so_notify_changed(&p->cp_notify, DISPID_PICT_HPAL);
  return S_OK;
}
static HRESULT WINAPI so_pic_get_CurDC(IPicture *iface, HDC *dc) {
  if (!dc) return E_POINTER;
  *dc = so_pic_from(iface)->cur_dc;
  return S_OK;
}
static HRESULT WINAPI so_pic_SelectPicture(IPicture *iface, HDC dc_in, HDC *dc_out, OLE_HANDLE *bitmap_out) {
  so_pic *p = so_pic_from(iface);
  if (p->desc.picType != PICTYPE_BITMAP) return E_FAIL;
  so_SelectObject(dc_in, p->desc.bmp.hbitmap);
  if (dc_out) *dc_out = p->cur_dc;
  p->cur_dc = dc_in;
  if (bitmap_out) *bitmap_out = (OLE_HANDLE)(ULONG_PTR)p->desc.bmp.hbitmap;
  return S_OK;
}
static HRESULT WINAPI so_pic_get_KeepOriginalFormat(IPicture *iface, BOOL *keep) {
  if (!keep) return E_POINTER;
  *keep = so_pic_from(iface)->keep_original;
  return S_OK;
}
static HRESULT WINAPI so_pic_put_KeepOriginalFormat(IPicture *iface, BOOL keep) {
  so_pic_from(iface)->keep_original = keep;
  return S_OK;
}
static HRESULT WINAPI so_pic_PictureChanged(IPicture *iface) {
  so_notify_changed(&so_pic_from(iface)->cp_notify, DISPID_PICT_HANDLE);
  return S_OK;
}
static HRESULT WINAPI so_pic_SaveAsFile(IPicture *iface, IStream *stream, BOOL mem_copy, LONG *written) {
  so_pic *p = so_pic_from(iface);
  BYTE *buf = NULL;
  SIZE_T len = 0;
  BOOL owned = FALSE;
  HRESULT hr;
  if (!stream) return E_POINTER;
  if (p->desc.picType == PICTYPE_NONE || (int)p->desc.picType == PICTYPE_UNINITIALIZED) return S_OK;
  if (!mem_copy) {
    if (!p->original) return E_FAIL;                   /* nothing but the handle: no original format to write */
    buf = p->original; len = p->original_len;
  } else {
    if (!so_pic_serialize(p, &buf, &len)) return E_FAIL;
    owned = TRUE;
  }
  if (len > 0x7FFFFFFF) hr = E_FAIL;
  else hr = so_write_all(stream, buf, (ULONG)len);
  if (owned) oa_free(buf);
  if (SUCCEEDED(hr) && written) *written = (LONG)len;
  return hr;
}
static HRESULT WINAPI so_pic_get_Attributes(IPicture *iface, DWORD *attributes) {
  so_pic *p = so_pic_from(iface);
  if (!attributes) return E_POINTER;
  *attributes = 0;
  if (p->desc.picType == PICTYPE_METAFILE || p->desc.picType == PICTYPE_ENHMETAFILE) *attributes |= PICTURE_SCALABLE;
  if (p->desc.picType == PICTYPE_ICON) *attributes |= PICTURE_TRANSPARENT;
  return S_OK;
}
static const IPictureVtbl so_pic_vtbl = {
  so_pic_QueryInterface, so_pic_AddRef, so_pic_Release, so_pic_get_Handle, so_pic_get_hPal, so_pic_get_Type, so_pic_get_Width, so_pic_get_Height, so_pic_Render,
  so_pic_set_hPal, so_pic_get_CurDC, so_pic_SelectPicture, so_pic_get_KeepOriginalFormat, so_pic_put_KeepOriginalFormat, so_pic_PictureChanged, so_pic_SaveAsFile,
  so_pic_get_Attributes
};

/* ---- IPictureDisp ------------------------------------------------------------------------------------------------------------------------------------------ */
static const so_font_prop so_pic_props[] = {
  {DISPID_PICT_HANDLE, L"Handle", VT_I4, FALSE}, {DISPID_PICT_HPAL, L"hPal", VT_I4, TRUE}, {DISPID_PICT_TYPE, L"Type", VT_I2, FALSE},
  {DISPID_PICT_WIDTH, L"Width", VT_I4, FALSE}, {DISPID_PICT_HEIGHT, L"Height", VT_I4, FALSE}
};

static HRESULT WINAPI so_pic_disp_QueryInterface(IDispatch *iface, REFIID riid, void **ppv) { return IPicture_QueryInterface(&so_pic_from_disp(iface)->ipic, riid, ppv); }
static ULONG WINAPI so_pic_disp_AddRef(IDispatch *iface) { return IPicture_AddRef(&so_pic_from_disp(iface)->ipic); }
static ULONG WINAPI so_pic_disp_Release(IDispatch *iface) { return IPicture_Release(&so_pic_from_disp(iface)->ipic); }
static HRESULT WINAPI so_pic_disp_GetTypeInfoCount(IDispatch *iface, UINT *count) {
  (void)iface;
  if (!count) return E_POINTER;
  *count = 1;
  return S_OK;
}
static HRESULT WINAPI so_pic_disp_GetTypeInfo(IDispatch *iface, UINT index, LCID lcid, ITypeInfo **out) {
  so_pic *p = so_pic_from_disp(iface);
  if (!out) return E_POINTER;
  *out = NULL;
  if (index != 0) return DISP_E_BADINDEX;
  if (!p->typeinfo) {
    ITypeInfo *info;
    HRESULT hr = so_build_typeinfo(so_pic_props, OA_ARRAY_SIZE(so_pic_props), lcid, &info);
    if (FAILED(hr)) return hr;
    if (__sync_val_compare_and_swap(&p->typeinfo, NULL, info) != NULL) ITypeInfo_Release(info);
  }
  *out = p->typeinfo;
  ITypeInfo_AddRef(*out);
  return S_OK;
}
static HRESULT WINAPI so_pic_disp_GetIDsOfNames(IDispatch *iface, REFIID riid, LPOLESTR *names, UINT count, LCID lcid, DISPID *ids) {
  static const WCHAR render_name[] = {'R','e','n','d','e','r',0};
  (void)iface; (void)lcid;
  if (!IsEqualIID(riid, &so_null_iid)) return DISP_E_UNKNOWNINTERFACE;
  if (!names || !ids || count == 0) return E_INVALIDARG;
  HRESULT hr = DISP_E_UNKNOWNNAME;
  ids[0] = DISPID_UNKNOWN;
  for (UINT i = 0; i < OA_ARRAY_SIZE(so_pic_props); i++) {
    if (so_wcsicmp(names[0], so_pic_props[i].name) != 0) continue;
    ids[0] = so_pic_props[i].id;
    hr = S_OK;
  }
  if (so_wcsicmp(names[0], render_name) == 0) { ids[0] = DISPID_PICT_RENDER; hr = S_OK; }
  for (UINT i = 1; i < count; i++) ids[i] = DISPID_UNKNOWN;
  return hr;
}
static HRESULT WINAPI so_pic_disp_Invoke(IDispatch *iface, DISPID dispid, REFIID riid, LCID lcid, WORD flags, DISPPARAMS *params, VARIANT *result, EXCEPINFO *excep, UINT *arg_err) {
  so_pic *p = so_pic_from_disp(iface);
  IPicture *pic = &p->ipic;
  (void)lcid; (void)excep; (void)arg_err;
  if (!IsEqualIID(riid, &so_null_iid)) return DISP_E_UNKNOWNNAME;
  if (dispid == DISPID_PICT_RENDER) {
    if (!(flags & DISPATCH_METHOD)) return DISP_E_MEMBERNOTFOUND;
    if (!params || params->cArgs != 10) return DISP_E_BADPARAMCOUNT;
    for (UINT i = 0; i < 10; i++) if (V_VT(&params->rgvarg[i]) != VT_I4) return DISP_E_TYPEMISMATCH;
    #define SO_ARG(n) V_I4(&params->rgvarg[9 - (n)])
    return IPicture_Render(pic, (HDC)(LONG_PTR)SO_ARG(0), SO_ARG(1), SO_ARG(2), SO_ARG(3), SO_ARG(4), SO_ARG(5), SO_ARG(6), SO_ARG(7), SO_ARG(8),
                           (const RECT *)(ULONG_PTR)V_INT_PTR(&params->rgvarg[0]));
    #undef SO_ARG
  }
  const so_font_prop *prop = NULL;
  for (UINT i = 0; i < OA_ARRAY_SIZE(so_pic_props); i++) if (so_pic_props[i].id == dispid) prop = &so_pic_props[i];
  if (!prop) return DISP_E_MEMBERNOTFOUND;
  if (flags & DISPATCH_PROPERTYGET) {
    if (!params) return DISP_E_PARAMNOTOPTIONAL;
    if (params->cArgs != 0) return DISP_E_BADPARAMCOUNT;
    if (!result) return DISP_E_PARAMNOTOPTIONAL;
    VariantInit(result);
    V_VT(result) = prop->vt;
    HRESULT hr;
    switch (dispid) {
    case DISPID_PICT_HANDLE: hr = IPicture_get_Handle(pic, (OLE_HANDLE *)&V_I4(result)); break;
    case DISPID_PICT_HPAL: hr = IPicture_get_hPal(pic, (OLE_HANDLE *)&V_I4(result)); break;
    case DISPID_PICT_TYPE: hr = IPicture_get_Type(pic, &V_I2(result)); break;
    case DISPID_PICT_WIDTH: hr = IPicture_get_Width(pic, &V_I4(result)); break;
    default: hr = IPicture_get_Height(pic, &V_I4(result)); break;
    }
    if (FAILED(hr)) V_VT(result) = VT_EMPTY;
    return hr;
  }
  if ((flags & DISPATCH_PROPERTYPUT) && prop->writable) {
    if (!params) return DISP_E_PARAMNOTOPTIONAL;
    if (params->cArgs != 1) return DISP_E_BADPARAMCOUNT;
    VARIANT v;
    BOOL owned;
    HRESULT hr = so_variant_as(&v, &params->rgvarg[0], VT_I4, &owned);
    if (FAILED(hr)) return hr;
    hr = IPicture_set_hPal(pic, (OLE_HANDLE)V_I4(&v));
    if (owned) VariantClear(&v);
    return hr;
  }
  return DISP_E_MEMBERNOTFOUND;
}
static const IDispatchVtbl so_pic_disp_vtbl = {
  so_pic_disp_QueryInterface, so_pic_disp_AddRef, so_pic_disp_Release, so_pic_disp_GetTypeInfoCount, so_pic_disp_GetTypeInfo, so_pic_disp_GetIDsOfNames, so_pic_disp_Invoke
};

/* ---- IPersistStream and IPersistStreamInit ---------------------------------------------------------------------------------------------------------------------- */
static const CLSID *so_pic_clsid(void) { return &so_CLSID_StdPicture; }

static HRESULT so_pic_save_stream(so_pic *p, IStream *stream) {
  BYTE head[8] = {'l', 't', 0, 0, 0, 0, 0, 0};
  BYTE *buf = NULL;
  SIZE_T len = 0;
  BOOL owned = FALSE;
  HRESULT hr;
  if (!stream) return E_POINTER;
  if (p->desc.picType != PICTYPE_NONE && (int)p->desc.picType != PICTYPE_UNINITIALIZED) {
    if (p->keep_original && p->original) { buf = p->original; len = p->original_len; }
    else if (so_pic_serialize(p, &buf, &len)) owned = TRUE;
    else return E_FAIL;
  }
  if (len > 0x7FFFFFF0u) { if (owned) oa_free(buf); return E_FAIL; }
  so_put32(head + 4, (DWORD)len);
  hr = so_write_all(stream, head, 8);
  if (SUCCEEDED(hr) && len) hr = so_write_all(stream, buf, (ULONG)len);
  if (owned) oa_free(buf);
  return hr;
}
static HRESULT so_pic_size_max(so_pic *p, ULARGE_INTEGER *size) {
  BYTE *buf = NULL;
  SIZE_T len = 0;
  if (!size) return E_INVALIDARG;
  if (p->desc.picType == PICTYPE_NONE || (int)p->desc.picType == PICTYPE_UNINITIALIZED) { size->QuadPart = 8; return S_OK; }
  if (p->keep_original && p->original) len = p->original_len;
  else if (so_pic_serialize(p, &buf, &len)) oa_free(buf);
  else return E_FAIL;
  size->QuadPart = 8 + (ULONGLONG)len;
  return S_OK;
}

static HRESULT WINAPI so_pic_ps_QueryInterface(IPersistStream *iface, REFIID riid, void **ppv) { return IPicture_QueryInterface(&so_pic_from_ps(iface)->ipic, riid, ppv); }
static ULONG WINAPI so_pic_ps_AddRef(IPersistStream *iface) { return IPicture_AddRef(&so_pic_from_ps(iface)->ipic); }
static ULONG WINAPI so_pic_ps_Release(IPersistStream *iface) { return IPicture_Release(&so_pic_from_ps(iface)->ipic); }
static HRESULT WINAPI so_pic_ps_GetClassID(IPersistStream *iface, CLSID *clsid) {
  (void)iface;
  if (!clsid) return E_POINTER;
  *clsid = *so_pic_clsid();
  return S_OK;
}
static HRESULT WINAPI so_pic_ps_IsDirty(IPersistStream *iface) { (void)iface; return S_OK; }
static HRESULT WINAPI so_pic_ps_Load(IPersistStream *iface, IStream *stream) { return so_pic_load(so_pic_from_ps(iface), stream, 0); }
static HRESULT WINAPI so_pic_ps_Save(IPersistStream *iface, IStream *stream, BOOL clear_dirty) { (void)clear_dirty; return so_pic_save_stream(so_pic_from_ps(iface), stream); }
static HRESULT WINAPI so_pic_ps_GetSizeMax(IPersistStream *iface, ULARGE_INTEGER *size) { return so_pic_size_max(so_pic_from_ps(iface), size); }
static const IPersistStreamVtbl so_pic_ps_vtbl = {
  so_pic_ps_QueryInterface, so_pic_ps_AddRef, so_pic_ps_Release, so_pic_ps_GetClassID, so_pic_ps_IsDirty, so_pic_ps_Load, so_pic_ps_Save, so_pic_ps_GetSizeMax
};

static HRESULT WINAPI so_pic_psi_QueryInterface(IPersistStreamInit *iface, REFIID riid, void **ppv) { return IPicture_QueryInterface(&so_pic_from_psi(iface)->ipic, riid, ppv); }
static ULONG WINAPI so_pic_psi_AddRef(IPersistStreamInit *iface) { return IPicture_AddRef(&so_pic_from_psi(iface)->ipic); }
static ULONG WINAPI so_pic_psi_Release(IPersistStreamInit *iface) { return IPicture_Release(&so_pic_from_psi(iface)->ipic); }
static HRESULT WINAPI so_pic_psi_GetClassID(IPersistStreamInit *iface, CLSID *clsid) {
  (void)iface;
  if (!clsid) return E_POINTER;
  *clsid = *so_pic_clsid();
  return S_OK;
}
static HRESULT WINAPI so_pic_psi_IsDirty(IPersistStreamInit *iface) { (void)iface; return S_OK; }
static HRESULT WINAPI so_pic_psi_Load(IPersistStreamInit *iface, IStream *stream) { return so_pic_load(so_pic_from_psi(iface), stream, 0); }
static HRESULT WINAPI so_pic_psi_Save(IPersistStreamInit *iface, IStream *stream, BOOL clear_dirty) { (void)clear_dirty; return so_pic_save_stream(so_pic_from_psi(iface), stream); }
static HRESULT WINAPI so_pic_psi_GetSizeMax(IPersistStreamInit *iface, ULARGE_INTEGER *size) { return so_pic_size_max(so_pic_from_psi(iface), size); }
static HRESULT WINAPI so_pic_psi_InitNew(IPersistStreamInit *iface) { (void)iface; return S_OK; }
static const IPersistStreamInitVtbl so_pic_psi_vtbl = {
  so_pic_psi_QueryInterface, so_pic_psi_AddRef, so_pic_psi_Release, so_pic_psi_GetClassID, so_pic_psi_IsDirty, so_pic_psi_Load, so_pic_psi_Save, so_pic_psi_GetSizeMax,
  so_pic_psi_InitNew
};

/* ---- IConnectionPointContainer ------------------------------------------------------------------------------------------------------------------------------- */
static HRESULT WINAPI so_pic_cpc_QueryInterface(IConnectionPointContainer *iface, REFIID riid, void **ppv) { return IPicture_QueryInterface(&so_pic_from_cpc(iface)->ipic, riid, ppv); }
static ULONG WINAPI so_pic_cpc_AddRef(IConnectionPointContainer *iface) { return IPicture_AddRef(&so_pic_from_cpc(iface)->ipic); }
static ULONG WINAPI so_pic_cpc_Release(IConnectionPointContainer *iface) { return IPicture_Release(&so_pic_from_cpc(iface)->ipic); }
static HRESULT WINAPI so_pic_cpc_EnumConnectionPoints(IConnectionPointContainer *iface, IEnumConnectionPoints **out) {
  (void)iface;
  if (!out) return E_POINTER;
  *out = NULL;
  return E_NOTIMPL;
}
static HRESULT WINAPI so_pic_cpc_FindConnectionPoint(IConnectionPointContainer *iface, REFIID riid, IConnectionPoint **out) {
  so_pic *p = so_pic_from_cpc(iface);
  so_cp *points[1] = {&p->cp_notify};
  return so_cpc_find(points, 1, riid, out);
}
static const IConnectionPointContainerVtbl so_pic_cpc_vtbl = {
  so_pic_cpc_QueryInterface, so_pic_cpc_AddRef, so_pic_cpc_Release, so_pic_cpc_EnumConnectionPoints, so_pic_cpc_FindConnectionPoint
};

/* ---- creation and the exports --------------------------------------------------------------------------------------------------------------------------------- */
static HRESULT so_pic_create(const PICTDESC *desc, BOOL own, REFIID riid, void **ppv) {
  if (!ppv) return E_POINTER;
  *ppv = NULL;
  if (desc && (int)desc->picType == PICTYPE_UNINITIALIZED) return E_UNEXPECTED;
  if (desc && ((int)desc->picType > PICTYPE_ENHMETAFILE || (int)desc->picType < PICTYPE_UNINITIALIZED)) return E_UNEXPECTED;
  so_pic *p = so_pic_new();
  if (!p) return E_OUTOFMEMORY;
  if (desc) so_pic_set(p, desc, own);
  HRESULT hr = IPicture_QueryInterface(&p->ipic, riid, ppv);
  IPicture_Release(&p->ipic);
  return hr;
}

EXPORT HRESULT WINAPI OleCreatePictureIndirect(LPPICTDESC desc, REFIID riid, BOOL own, LPVOID *ppv) { return so_pic_create(desc, own, riid, ppv); }

static HRESULT so_load_picture_stream(IStream *stream, LONG size, REFIID riid, void **ppv) {
  if (!ppv) return E_POINTER;
  *ppv = NULL;
  if (!stream) return E_INVALIDARG;
  so_pic *p = so_pic_new();
  if (!p) return E_OUTOFMEMORY;
  HRESULT hr = so_pic_load(p, stream, size);
  if (SUCCEEDED(hr)) hr = IPicture_QueryInterface(&p->ipic, riid, ppv);
  IPicture_Release(&p->ipic);
  return hr;
}

EXPORT HRESULT WINAPI OleLoadPicture(LPSTREAM stream, LONG size, BOOL run_mode, REFIID riid, LPVOID *ppv) {
  (void)run_mode;
  return so_load_picture_stream(stream, size, riid, ppv);
}
EXPORT HRESULT WINAPI OleLoadPictureEx(LPSTREAM stream, LONG size, BOOL run_mode, REFIID riid, DWORD cx, DWORD cy, DWORD flags, LPVOID *ppv) {
  (void)run_mode; (void)cx; (void)cy; (void)flags;
  return so_load_picture_stream(stream, size, riid, ppv);
}

/* ---- files ------------------------------------------------------------------------------------------------------------------------------------------------------ */
static HRESULT so_read_file(HANDLE file, BYTE **out, SIZE_T *out_len) {
  SIZE_T capacity = 65536, len = 0;
  BYTE *buf = oa_alloc(capacity);
  if (!buf) return E_OUTOFMEMORY;
  for (;;) {
    if (len == capacity) {
      if (capacity >= SO_MAX_BLOB) { oa_free(buf); return E_FAIL; }
      BYTE *bigger = oa_realloc(buf, capacity * 2);
      if (!bigger) { oa_free(buf); return E_OUTOFMEMORY; }
      buf = bigger; capacity *= 2;
    }
    DWORD got = 0;
    if (!ReadFile(file, buf + len, (DWORD)(capacity - len > 0x1000000 ? 0x1000000 : capacity - len), &got, NULL)) { oa_free(buf); return E_FAIL; }
    if (got == 0) break;
    len += got;
  }
  *out = buf; *out_len = len;
  return S_OK;
}

static HRESULT so_picture_from_bytes(const BYTE *d, SIZE_T len, REFIID riid, void **ppv) {
  so_pic *p = so_pic_new();
  if (!p) return E_OUTOFMEMORY;
  HRESULT hr;
  if (len >= 8 && so_is_lt_header(d)) {
    so_memstream *m = so_memstream_new();
    hr = m ? so_ms_Write(&m->iface, d, (ULONG)len, NULL) : E_OUTOFMEMORY;
    if (SUCCEEDED(hr)) { m->pos = 0; hr = so_pic_load(p, &m->iface, 0); }
    if (m) IStream_Release(&m->iface);
  } else hr = so_pic_set_from_data(p, d, len);
  if (SUCCEEDED(hr)) hr = IPicture_QueryInterface(&p->ipic, riid, ppv);
  IPicture_Release(&p->ipic);
  return hr;
}

/* "file:" URLs and plain paths; the file: form has its slashes turned around and %XX escapes decoded. Returns an allocated path or NULL for other schemes. */
static int so_hex_digit(WCHAR c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; }
static WCHAR *so_url_to_path(const WCHAR *url, HRESULT *error) {
  static const WCHAR file_scheme[] = {'f','i','l','e',':',0};
  SIZE_T n = oa_wcslen(url);
  *error = S_OK;
  if (n == 0) { *error = INET_E_UNKNOWN_PROTOCOL; return NULL; }
  const WCHAR *p = url;
  BOOL is_file_url = FALSE;
  int slashes = 0;
  if (n >= 5) {
    is_file_url = TRUE;
    for (int i = 0; i < 5; i++) if (oa_towlower(url[i]) != file_scheme[i]) is_file_url = FALSE;
  }
  if (is_file_url) {
    p = url + 5;
    while (slashes < 3 && (*p == '/' || *p == '\\')) { p++; slashes++; }
  } else {
    /* a scheme (two or more characters before a colon) other than file: is not something that can be fetched here */
    SIZE_T k = 0;
    while (url[k] && ((url[k] >= 'a' && url[k] <= 'z') || (url[k] >= 'A' && url[k] <= 'Z') || (url[k] >= '0' && url[k] <= '9') || url[k] == '+' || url[k] == '-' || url[k] == '.')) k++;
    if (url[k] == ':' && k >= 2) { *error = INET_E_UNKNOWN_PROTOCOL; return NULL; }
  }
  SIZE_T m = oa_wcslen(p);
  WCHAR *path = oa_alloc((m + 3) * sizeof(WCHAR));
  if (!path) { *error = E_OUTOFMEMORY; return NULL; }
  SIZE_T o = 0;
  BOOL drive = p[0] && p[1] == ':';
  if (is_file_url && slashes == 2) { path[o++] = '\\'; path[o++] = '\\'; }
  else if (is_file_url && slashes >= 3 && !drive) path[o++] = '\\';
  for (SIZE_T i = 0; i < m; i++) {
    WCHAR c = p[i];
    if (is_file_url && c == '%' && i + 2 < m) {
      int hi = so_hex_digit(p[i + 1]), lo = so_hex_digit(p[i + 2]);
      if (hi >= 0 && lo >= 0) { path[o++] = (WCHAR)(hi * 16 + lo); i += 2; continue; }
    }
    path[o++] = (is_file_url && c == '/') ? '\\' : c;
  }
  path[o] = 0;
  return path;
}

EXPORT HRESULT WINAPI OleLoadPicturePath(LPOLESTR url_or_path, LPUNKNOWN outer, DWORD reserved, OLE_COLOR color, REFIID riid, LPVOID *ppv) {
  HRESULT hr;
  (void)outer; (void)reserved; (void)color;
  if (!url_or_path || !ppv) return E_INVALIDARG;
  *ppv = NULL;
  WCHAR *path = so_url_to_path(url_or_path, &hr);
  if (!path) return hr;
  HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
  oa_free(path);
  if (file == INVALID_HANDLE_VALUE) return INET_E_RESOURCE_NOT_FOUND;
  BYTE *data = NULL;
  SIZE_T len = 0;
  hr = so_read_file(file, &data, &len);
  CloseHandle(file);
  if (FAILED(hr)) return hr;
  hr = so_picture_from_bytes(data, len, riid ? riid : &IID_IPicture, ppv);
  oa_free(data);
  return hr;
}

EXPORT HRESULT WINAPI OleLoadPictureFileEx(VARIANT file_name, DWORD x_size, DWORD y_size, DWORD flags, IDispatch **picture) {
  HANDLE file;
  BOOL close_it = FALSE;
  (void)x_size; (void)y_size; (void)flags;
  if (!picture) return E_POINTER;
  *picture = NULL;
  if (V_VT(&file_name) == VT_BSTR) {
    const WCHAR *name = V_BSTR(&file_name);
    if (!name || !*name) return CTL_E_FILENOTFOUND;
    if (oa_wcslen(name) >= 5 && oa_towlower(name[0]) == 'f' && oa_towlower(name[1]) == 'i' && oa_towlower(name[2]) == 'l' && oa_towlower(name[3]) == 'e' && name[4] == ':') return CTL_E_PATHFILEACCESSERROR;
    file = CreateFileW(name, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) {
      DWORD error = GetLastError();
      return error == ERROR_FILE_NOT_FOUND ? CTL_E_FILENOTFOUND : CTL_E_PATHFILEACCESSERROR;
    }
    close_it = TRUE;
  } else {
    /* anything else is taken for an open file handle */
    VARIANT v;
    BOOL owned;
    if (FAILED(so_variant_as(&v, &file_name, VT_I4, &owned))) return CTL_E_FILENOTFOUND;
    file = (HANDLE)(LONG_PTR)V_I4(&v);
    if (owned) VariantClear(&v);
    if (GetFileType(file) == FILE_TYPE_UNKNOWN) return CTL_E_FILENOTFOUND;
  }
  BYTE *data = NULL;
  SIZE_T len = 0;
  HRESULT hr = so_read_file(file, &data, &len);
  if (close_it) CloseHandle(file);
  if (FAILED(hr)) return hr;
  hr = so_picture_from_bytes(data, len, &IID_IPictureDisp, (void **)picture);
  oa_free(data);
  return hr;
}
EXPORT HRESULT WINAPI OleLoadPictureFile(VARIANT file_name, IDispatch **picture) { return OleLoadPictureFileEx(file_name, 0, 0, 0, picture); }

EXPORT HRESULT WINAPI OleSavePictureFile(IDispatch *picture, BSTR file_name) {
  IPicture *pic;
  if (!picture || !file_name) return CTL_E_ILLEGALFUNCTIONCALL;
  HRESULT hr = IDispatch_QueryInterface(picture, &IID_IPicture, (void **)&pic);
  if (FAILED(hr)) return CTL_E_ILLEGALFUNCTIONCALL;
  so_memstream *m = so_memstream_new();
  if (!m) { IPicture_Release(pic); return E_OUTOFMEMORY; }
  LONG written = 0;
  hr = IPicture_SaveAsFile(pic, &m->iface, TRUE, &written);
  IPicture_Release(pic);
  if (SUCCEEDED(hr)) {
    HANDLE file = CreateFileW(file_name, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) hr = CTL_E_PATHFILEACCESSERROR;
    else {
      SIZE_T done = 0;
      while (done < m->size) {
        DWORD put = 0;
        if (!WriteFile(file, m->data + done, (DWORD)(m->size - done > 0x1000000 ? 0x1000000 : m->size - done), &put, NULL) || put == 0) { hr = CTL_E_PATHFILEACCESSERROR; break; }
        done += put;
      }
      CloseHandle(file);
    }
  }
  IStream_Release(&m->iface);
  return hr;
}

EXPORT HCURSOR WINAPI OleIconToCursor(HINSTANCE instance, HICON icon) {
  (void)instance;
  return icon ? (HCURSOR)so_CopyIcon(icon) : NULL;
}
