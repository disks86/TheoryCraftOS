/* usrmarshal.inc - the "user marshaling" routines (xxx_UserSize / _UserMarshal / _UserUnmarshal / _UserFree) that MIDL generated proxies and stubs call to put Windows
 * handle types, CLIPFORMAT, SNB, STGMEDIUM and interface pointers on the wire. The wire format is the NDR one that Windows uses (byte exact, checked by Wine's usrmarshal test).
 *
 * Conventions. flags: LOWORD = MSHCTX_*, HIWORD = data representation. The size routines return the offset after the item; the marshal/unmarshal routines align the buffer
 * pointer to 4 and return the pointer after the item. The buffer length is not known to these routines, so they only read what the format describes: every count that comes
 * from the wire is validated (limit 0x7fffffff and exact consistency of the repeated size fields) and a bad value raises RPC_X_BAD_STUB_DATA like Windows. user32 and gdi32
 * are reached through GetProcAddress (the GDI/user functions are optional, a missing one behaves like a failing call), never by import.
 * The "...Ext" and "...64" spellings are the same code on this target (the Ext variants receive the USER_MARSHAL_CB whose first member is the flags word). */

#define UM_MAXLEN 0x7fffffffu
#define UM_OBJ_PAL 5
#define UM_OBJ_BITMAP 7
#define UM_PTR_PREFIX 0x72657355u /* "User", the pointer prefix of HMETAFILEPICT's embedded HMETAFILE */

typedef struct { LONG bmType, bmWidth, bmHeight, bmWidthBytes; WORD bmPlanes, bmBitsPixel; const void *bmBits; } UM_BITMAP; /* the BITMAP structure */
typedef struct { LONG mm, xExt, yExt; HMETAFILE hMF; } UM_METAFILEPICT;                                                  /* METAFILEPICT */
#define UM_BMP_HDR 20u /* FIELD_OFFSET(userBITMAP, cbSize): bmType..bmBitsPixel, the part of BITMAP that goes on the wire */

typedef UINT_PTR(WINAPI *um_fn4)(UINT_PTR, UINT_PTR, UINT_PTR, UINT_PTR);

__attribute__((noreturn)) static void um_raise(DWORD code) {
  RaiseException(code, EXCEPTION_NONCONTINUABLE, 0, NULL);
  for (;;) {}
}

/* Calls an optional function of gdi32 (idx 0) or user32 (idx 1) with up to four integer arguments (extra arguments are ignored by the callee on x64). Result 0 when absent. */
static UINT_PTR um_call(int idx, const char *fn, UINT_PTR a, UINT_PTR b, UINT_PTR c, UINT_PTR d) {
  const WCHAR *name = idx ? W('u', 's', 'e', 'r', '3', '2', '.', 'd', 'l', 'l') : W('g', 'd', 'i', '3', '2', '.', 'd', 'l', 'l');
  HMODULE m = GetModuleHandleW(name);
  um_fn4 f;
  if (!m) m = LoadLibraryW(name);
  if (!m) return 0;
  f = (um_fn4)(void *)GetProcAddress(m, fn);
  return f ? f(a, b, c, d) : 0;
}
#define UM_GDI(fn, a, b, c, d) um_call(0, fn, (UINT_PTR)(a), (UINT_PTR)(b), (UINT_PTR)(c), (UINT_PTR)(d))

/* Checked size arithmetic: offsets and sizes stay below 0x7fffffff. */
static ULONG um_add(ULONG a, SIZE_T b) {
  if (b > UM_MAXLEN || a > UM_MAXLEN - (ULONG)b) um_raise(RPC_X_BAD_STUB_DATA);
  return a + (ULONG)b;
}
static ULONG um_align(ULONG a) { return um_add(a, 3) & ~3u; }

static unsigned char *um_align_ptr(unsigned char *p) { return (unsigned char *)(((ULONG_PTR)p + 3) & ~(ULONG_PTR)3); }
static void um_wr(unsigned char **p, ULONG v) { mem_copy(*p, &v, 4); *p += 4; }
static ULONG um_rd(const unsigned char **p) { ULONG v = 0; mem_copy(&v, *p, 4); *p += 4; return v; }
static ULONG um_ref_id(const void *p) { ULONG v = (ULONG)(ULONG_PTR)p; return p ? (v ? v : 1) : 0; } /* non-zero referent id for a non-NULL pointer */
static HANDLE um_h32(ULONG v) { return (HANDLE)(LONG_PTR)(LONG)v; }                                   /* 32 bit handle value, sign extended */

/* Allocator of the stub message (USER_MARSHAL_CB.Flags is its first member); CoTaskMem when the caller gave no message. */
static const USER_MARSHAL_CB *um_cb(const ULONG *flags) {
  const USER_MARSHAL_CB *cb = (const USER_MARSHAL_CB *)flags; /* flags is the first member of the callback structure MIDL passes, see the Windows DDK */
  return (cb->Signature == 0x55535243u /* USRC */ && cb->pStubMsg) ? cb : NULL;
}
static void *um_alloc(ULONG *flags, SIZE_T n) {
  const USER_MARSHAL_CB *cb = um_cb(flags);
  return (cb && cb->pStubMsg->pfnAllocate) ? cb->pStubMsg->pfnAllocate(n) : CoTaskMemAlloc(n);
}
static void um_free(ULONG *flags, void *p) {
  const USER_MARSHAL_CB *cb = um_cb(flags);
  if (cb && cb->pStubMsg->pfnFree) cb->pStubMsg->pfnFree(p); else CoTaskMemFree(p);
}

/* ---- WIRE_HANDLE: HWND HMENU HACCEL HBRUSH HDC HICON HMONITOR HRGN ---- {long fContext; long handle} */
static ULONG um_hnd_size(ULONG *f, ULONG s, void *v) { (void)f; (void)v; return um_add(um_align(s), 8); }
static unsigned char *um_hnd_marshal(ULONG *f, unsigned char *p, void *v) {
  p = um_align_ptr(p);
  um_wr(&p, LOWORD(*f) == MSHCTX_DIFFERENTMACHINE ? WDT_REMOTE_CALL : WDT_INPROC_CALL);
  um_wr(&p, (ULONG)(ULONG_PTR) * (HANDLE *)v);
  return p;
}
static unsigned char *um_hnd_unmarshal(ULONG *f, unsigned char *p, void *v) {
  ULONG ctx;
  (void)f;
  p = um_align_ptr(p);
  ctx = um_rd((const unsigned char **)&p);
  if (ctx != WDT_INPROC_CALL && ctx != WDT_REMOTE_CALL) um_raise(RPC_S_INVALID_TAG);
  *(HANDLE *)v = um_h32(um_rd((const unsigned char **)&p));
  return p;
}
static void um_hnd_free(ULONG *f, void *v) { (void)f; (void)v; }

/* ---- CLIPFORMAT: {ctx, value} and, for registered formats (>= 0xc000), a conformant varying string of the name ---- */
static int um_cf_name(UINT cf, WCHAR *buf) { /* buf holds 256 WCHAR; returns the name length, 0 when unknown */
  int n = (int)um_call(1, "GetClipboardFormatNameW", cf, (UINT_PTR)buf, 255, 0);
  if (n < 0 || n > 255) n = 0;
  buf[n] = 0;
  return n;
}
static ULONG um_cf_size(ULONG *f, ULONG s, void *v) {
  UINT cf = *(CLIPFORMAT *)v;
  WCHAR name[256];
  (void)f;
  s = um_add(um_align(s), 8);
  if (cf >= 0xc000) s = um_add(um_add(s, 12), ((SIZE_T)um_cf_name(cf, name) + 1) * sizeof(WCHAR));
  return s;
}
static unsigned char *um_cf_marshal(ULONG *f, unsigned char *p, void *v) {
  UINT cf = *(CLIPFORMAT *)v;
  (void)f;
  p = um_align_ptr(p);
  um_wr(&p, WDT_REMOTE_CALL);
  um_wr(&p, cf);
  if (cf >= 0xc000) {
    WCHAR name[256];
    ULONG n = (ULONG)um_cf_name(cf, name) + 1;
    um_wr(&p, n);
    um_wr(&p, 0);
    um_wr(&p, n);
    mem_copy(p, name, n * sizeof(WCHAR));
    p += n * sizeof(WCHAR);
  }
  return p;
}
static unsigned char *um_cf_unmarshal(ULONG *f, unsigned char *p, void *v) {
  ULONG ctx, val;
  (void)f;
  p = um_align_ptr(p);
  ctx = um_rd((const unsigned char **)&p);
  val = um_rd((const unsigned char **)&p);
  if (ctx != WDT_INPROC_CALL && ctx != WDT_REMOTE_CALL) um_raise(RPC_S_INVALID_TAG);
  *(CLIPFORMAT *)v = (CLIPFORMAT)val;
  if (ctx == WDT_REMOTE_CALL && val >= 0xc000) {
    WCHAR name[256];
    ULONG max = um_rd((const unsigned char **)&p), off = um_rd((const unsigned char **)&p), act = um_rd((const unsigned char **)&p), n;
    if (max > UM_MAXLEN / sizeof(WCHAR) || off != 0 || act > max) um_raise(RPC_X_BAD_STUB_DATA);
    n = act > 255 ? 255 : act;
    mem_copy(name, p, n * sizeof(WCHAR));
    name[n] = 0;
    p += (SIZE_T)act * sizeof(WCHAR);
    if (n && name[0]) {
      UINT id = (UINT)um_call(1, "RegisterClipboardFormatW", (UINT_PTR)name, 0, 0, 0);
      if (id) *(CLIPFORMAT *)v = (CLIPFORMAT)id;
    }
  }
  return p;
}
static void um_cf_free(ULONG *f, void *v) { (void)f; (void)v; }

/* ---- HGLOBAL: inproc {WDT_INPROC64_CALL, 64 bit handle}; otherwise {WDT_REMOTE_CALL, handle32} and for a non-NULL handle {size, handle32, size, bytes} ---- */
static ULONG um_hglobal_size(ULONG *f, ULONG s, void *v) {
  HGLOBAL h = *(HGLOBAL *)v;
  s = um_add(um_align(s), 4);
  if (LOWORD(*f) == MSHCTX_INPROC) return um_add(s, sizeof(HGLOBAL));
  s = um_add(s, 4);
  if (h) s = um_add(um_add(s, 12), GlobalSize(h));
  return s;
}
static unsigned char *um_hglobal_marshal(ULONG *f, unsigned char *p, void *v) {
  HGLOBAL h = *(HGLOBAL *)v;
  p = um_align_ptr(p);
  if (LOWORD(*f) == MSHCTX_INPROC) {
    um_wr(&p, WDT_INPROC64_CALL);
    mem_copy(p, &h, sizeof(h));
    return p + sizeof(h);
  }
  um_wr(&p, WDT_REMOTE_CALL);
  um_wr(&p, (ULONG)(ULONG_PTR)h);
  if (h) {
    SIZE_T n = GlobalSize(h);
    const void *mem = GlobalLock(h);
    if (n > UM_MAXLEN) um_raise(RPC_X_BAD_STUB_DATA);
    um_wr(&p, (ULONG)n);
    um_wr(&p, um_ref_id(h));
    um_wr(&p, (ULONG)n);
    if (mem) mem_copy(p, mem, n); else mem_zero(p, n);
    p += n;
    if (mem) GlobalUnlock(h);
  }
  return p;
}
static unsigned char *um_hglobal_unmarshal(ULONG *f, unsigned char *p, void *v) {
  HGLOBAL *ph = v;
  ULONG ctx;
  (void)f;
  p = um_align_ptr(p);
  ctx = um_rd((const unsigned char **)&p);
  if (ctx == WDT_INPROC64_CALL) {
    mem_copy(ph, p, sizeof(*ph));
    return p + sizeof(*ph);
  }
  if (ctx == WDT_INPROC_CALL) { /* a 32 bit client */
    *ph = um_h32(um_rd((const unsigned char **)&p));
    return p;
  }
  if (ctx != WDT_REMOTE_CALL) um_raise(RPC_S_INVALID_TAG);
  if (um_rd((const unsigned char **)&p)) {
    ULONG n = um_rd((const unsigned char **)&p), n2;
    void *mem;
    (void)um_rd((const unsigned char **)&p); /* referent id */
    n2 = um_rd((const unsigned char **)&p);
    if (n != n2 || n > UM_MAXLEN) um_raise(RPC_X_BAD_STUB_DATA);
    if (*ph) {
      HGLOBAL r = GlobalReAlloc(*ph, n, GMEM_MOVEABLE);
      if (r) *ph = r; else { GlobalFree(*ph); *ph = NULL; }
    }
    if (!*ph) *ph = GlobalAlloc(GMEM_MOVEABLE, n);
    if (!*ph) um_raise(RPC_X_NO_MEMORY);
    mem = GlobalLock(*ph);
    if (!mem) um_raise(RPC_X_NO_MEMORY);
    mem_copy(mem, p, n);
    GlobalUnlock(*ph);
    p += n;
  } else {
    *ph = NULL;
  }
  return p;
}
static void um_hglobal_free(ULONG *f, void *v) {
  HGLOBAL *ph = v;
  if (LOWORD(*f) != MSHCTX_INPROC && *ph) {
    GlobalFree(*ph);
    *ph = NULL;
  }
}

/* ---- HMETAFILE (kind 0) and HENHMETAFILE (kind 1): inproc {ctx, handle32}; otherwise {WDT_REMOTE_CALL, handle32} and for a non-NULL handle {size, size, bytes} ---- */
static const char *const um_mf_get[2] = {"GetMetaFileBitsEx", "GetEnhMetaFileBits"};
static const char *const um_mf_set[2] = {"SetMetaFileBitsEx", "SetEnhMetaFileBits"};
static const char *const um_mf_del[2] = {"DeleteMetaFile", "DeleteEnhMetaFile"};

static ULONG um_mf_size(ULONG *f, ULONG s, HANDLE h, int kind) {
  s = um_add(um_align(s), 8);
  if (LOWORD(*f) != MSHCTX_INPROC && h) s = um_add(um_add(s, 8), (UINT)UM_GDI(um_mf_get[kind], h, 0, 0, 0));
  return s;
}
static unsigned char *um_mf_marshal(ULONG *f, unsigned char *p, HANDLE h, int kind) {
  p = um_align_ptr(p);
  if (LOWORD(*f) == MSHCTX_INPROC) {
    um_wr(&p, WDT_INPROC_CALL);
    um_wr(&p, (ULONG)(ULONG_PTR)h);
    return p;
  }
  um_wr(&p, WDT_REMOTE_CALL);
  um_wr(&p, (ULONG)(ULONG_PTR)h);
  if (h) {
    ULONG n = (UINT)UM_GDI(um_mf_get[kind], h, 0, 0, 0);
    if (n > UM_MAXLEN) um_raise(RPC_X_BAD_STUB_DATA);
    um_wr(&p, n);
    um_wr(&p, n);
    if (n) {
      ULONG got = (UINT)UM_GDI(um_mf_get[kind], h, n, p, 0);
      if (got < n) mem_zero(p + got, n - got);
    }
    p += n;
  }
  return p;
}
static unsigned char *um_mf_unmarshal(ULONG *f, unsigned char *p, HANDLE *ph, int kind) {
  ULONG ctx, handle;
  (void)f;
  p = um_align_ptr(p);
  ctx = um_rd((const unsigned char **)&p);
  handle = um_rd((const unsigned char **)&p);
  if (ctx == WDT_INPROC_CALL) {
    *ph = um_h32(handle);
  } else if (ctx == WDT_REMOTE_CALL) {
    if (handle) {
      ULONG n = um_rd((const unsigned char **)&p), n2 = um_rd((const unsigned char **)&p);
      if (n != n2 || n > UM_MAXLEN) um_raise(RPC_X_BAD_STUB_DATA);
      *ph = (HANDLE)UM_GDI(um_mf_set[kind], n, p, 0, 0);
      p += n;
    } else {
      *ph = NULL;
    }
  } else {
    um_raise(RPC_S_INVALID_TAG);
  }
  return p;
}
static void um_mf_free(ULONG *f, HANDLE *ph, int kind) {
  if (LOWORD(*f) != MSHCTX_INPROC && *ph) UM_GDI(um_mf_del[kind], *ph, 0, 0, 0);
}
static ULONG um_hmf_size(ULONG *f, ULONG s, void *v) { return um_mf_size(f, s, *(HANDLE *)v, 0); }
static unsigned char *um_hmf_marshal(ULONG *f, unsigned char *p, void *v) { return um_mf_marshal(f, p, *(HANDLE *)v, 0); }
static unsigned char *um_hmf_unmarshal(ULONG *f, unsigned char *p, void *v) { return um_mf_unmarshal(f, p, v, 0); }
static void um_hmf_free(ULONG *f, void *v) { um_mf_free(f, v, 0); }
static ULONG um_hemf_size(ULONG *f, ULONG s, void *v) { return um_mf_size(f, s, *(HANDLE *)v, 1); }
static unsigned char *um_hemf_marshal(ULONG *f, unsigned char *p, void *v) { return um_mf_marshal(f, p, *(HANDLE *)v, 1); }
static unsigned char *um_hemf_unmarshal(ULONG *f, unsigned char *p, void *v) { return um_mf_unmarshal(f, p, v, 1); }
static void um_hemf_free(ULONG *f, void *v) { um_mf_free(f, v, 1); }

/* ---- HMETAFILEPICT: {ctx, handle32} and for a non-NULL remote one {mm, xExt, yExt, "User", HMETAFILE} ---- */
static ULONG um_hmfp_size(ULONG *f, ULONG s, void *v) {
  HGLOBAL h = *(HGLOBAL *)v;
  s = um_add(um_align(s), 8);
  if (LOWORD(*f) != MSHCTX_INPROC && h) {
    UM_METAFILEPICT *mfp = GlobalLock(h);
    HMETAFILE hmf = mfp ? mfp->hMF : NULL;
    if (mfp) GlobalUnlock(h);
    s = um_hmf_size(f, um_add(s, 16), &hmf);
  }
  return s;
}
static unsigned char *um_hmfp_marshal(ULONG *f, unsigned char *p, void *v) {
  HGLOBAL h = *(HGLOBAL *)v;
  p = um_align_ptr(p);
  um_wr(&p, LOWORD(*f) == MSHCTX_INPROC ? WDT_INPROC_CALL : WDT_REMOTE_CALL);
  um_wr(&p, (ULONG)(ULONG_PTR)h);
  if (LOWORD(*f) != MSHCTX_INPROC && h) {
    UM_METAFILEPICT zero, *mfp = GlobalLock(h);
    mem_zero(&zero, sizeof(zero));
    um_wr(&p, (ULONG)(mfp ? mfp->mm : 0));
    um_wr(&p, (ULONG)(mfp ? mfp->xExt : 0));
    um_wr(&p, (ULONG)(mfp ? mfp->yExt : 0));
    um_wr(&p, UM_PTR_PREFIX);
    p = um_hmf_marshal(f, p, mfp ? &mfp->hMF : &zero.hMF);
    if (mfp) GlobalUnlock(h);
  }
  return p;
}
static unsigned char *um_hmfp_unmarshal(ULONG *f, unsigned char *p, void *v) {
  HGLOBAL *ph = v;
  ULONG ctx, handle;
  p = um_align_ptr(p);
  ctx = um_rd((const unsigned char **)&p);
  handle = um_rd((const unsigned char **)&p);
  if (ctx == WDT_INPROC_CALL) {
    *ph = um_h32(handle);
  } else if (ctx == WDT_REMOTE_CALL) {
    if (handle) {
      UM_METAFILEPICT *mfp;
      LONG mm = (LONG)um_rd((const unsigned char **)&p), x = (LONG)um_rd((const unsigned char **)&p), y = (LONG)um_rd((const unsigned char **)&p);
      HMETAFILE hmf = NULL;
      HGLOBAL g;
      (void)um_rd((const unsigned char **)&p); /* pointer prefix */
      p = um_hmf_unmarshal(f, p, &hmf);
      g = GlobalAlloc(GMEM_MOVEABLE, sizeof(*mfp));
      mfp = g ? GlobalLock(g) : NULL;
      if (!mfp) {
        if (g) GlobalFree(g);
        um_raise(RPC_X_NO_MEMORY);
      }
      mfp->mm = mm;
      mfp->xExt = x;
      mfp->yExt = y;
      mfp->hMF = hmf;
      GlobalUnlock(g);
      *ph = g;
    } else {
      *ph = NULL;
    }
  } else {
    um_raise(RPC_S_INVALID_TAG);
  }
  return p;
}
static void um_hmfp_free(ULONG *f, void *v) {
  HGLOBAL *ph = v;
  if (LOWORD(*f) != MSHCTX_INPROC && *ph) {
    UM_METAFILEPICT *mfp = GlobalLock(*ph);
    if (mfp) {
      um_hmf_free(f, &mfp->hMF);
      GlobalUnlock(*ph);
    }
    GlobalFree(*ph);
    *ph = NULL;
  }
}

/* ---- HBITMAP: inproc {ctx, handle32}; otherwise {WDT_REMOTE_CALL, handle32} and for a non-NULL one {size, 20 byte BITMAP header, bits} ---- */
static ULONG um_bmp_bits(HBITMAP h) {
  int n = (int)UM_GDI("GetBitmapBits", h, 0, 0, 0);
  return n > 0 ? (ULONG)n : 0;
}
static ULONG um_hbitmap_size(ULONG *f, ULONG s, void *v) {
  HBITMAP h = *(HBITMAP *)v;
  s = um_add(um_align(s), 8);
  if (LOWORD(*f) != MSHCTX_INPROC && h) s = um_add(um_add(s, 4 + UM_BMP_HDR), um_bmp_bits(h));
  return s;
}
static unsigned char *um_hbitmap_marshal(ULONG *f, unsigned char *p, void *v) {
  HBITMAP h = *(HBITMAP *)v;
  p = um_align_ptr(p);
  um_wr(&p, LOWORD(*f) == MSHCTX_INPROC ? WDT_INPROC_CALL : WDT_REMOTE_CALL);
  um_wr(&p, (ULONG)(ULONG_PTR)h);
  if (LOWORD(*f) != MSHCTX_INPROC && h) {
    UM_BITMAP bm;
    ULONG n = um_bmp_bits(h);
    mem_zero(&bm, sizeof(bm));
    UM_GDI("GetObjectW", h, sizeof(bm), &bm, 0);
    um_wr(&p, n);
    mem_copy(p, &bm, UM_BMP_HDR);
    p += UM_BMP_HDR;
    if (n) {
      ULONG got = (ULONG)(int)UM_GDI("GetBitmapBits", h, n, p, 0);
      if (got > n) got = 0;
      if (got < n) mem_zero(p + got, n - got);
    }
    p += n;
  }
  return p;
}
static unsigned char *um_hbitmap_unmarshal(ULONG *f, unsigned char *p, void *v) {
  HBITMAP *ph = v;
  ULONG ctx, handle;
  (void)f;
  p = um_align_ptr(p);
  ctx = um_rd((const unsigned char **)&p);
  handle = um_rd((const unsigned char **)&p);
  if (ctx == WDT_INPROC_CALL) {
    *ph = um_h32(handle);
  } else if (ctx == WDT_REMOTE_CALL) {
    if (handle) {
      UM_BITMAP bm;
      ULONG n = um_rd((const unsigned char **)&p);
      UINT64 need;
      mem_zero(&bm, sizeof(bm));
      if (n > UM_MAXLEN - UM_BMP_HDR) um_raise(RPC_X_BAD_STUB_DATA);
      mem_copy(&bm, p, UM_BMP_HDR);
      p += UM_BMP_HDR;
      if (bm.bmWidthBytes < 0 || bm.bmHeight < 0) um_raise(RPC_X_BAD_STUB_DATA);
      need = (UINT64)(ULONG)bm.bmWidthBytes * (ULONG)bm.bmHeight; /* < 2^62 */
      if (need > n / (bm.bmPlanes ? bm.bmPlanes : 1)) um_raise(RPC_X_BAD_STUB_DATA); /* the pixel data must lie inside the n bytes */
      bm.bmBits = p;
      *ph = (HBITMAP)UM_GDI("CreateBitmapIndirect", &bm, 0, 0, 0);
      p += n;
    } else {
      *ph = NULL;
    }
  } else {
    um_raise(RPC_S_INVALID_TAG);
  }
  return p;
}
static void um_hbitmap_free(ULONG *f, void *v) {
  HBITMAP *ph = v;
  if (LOWORD(*f) != MSHCTX_INPROC && *ph) UM_GDI("DeleteObject", *ph, 0, 0, 0);
}

/* ---- HPALETTE: inproc {ctx, handle32}; remote {WDT_REMOTE_CALL, referent id} and, when non-NULL, the LOGPALETTE {entry count, version, count, 4 byte entries} ---- */
static ULONG um_pal_count(HPALETTE h) {
  UINT n = (UINT)UM_GDI("GetPaletteEntries", h, 0, 0, 0);
  return n > 0x10000 ? 0 : n;
}
static ULONG um_hpalette_size(ULONG *f, ULONG s, void *v) {
  HPALETTE h = *(HPALETTE *)v;
  s = um_add(um_align(s), 8);
  if (LOWORD(*f) != MSHCTX_INPROC && h) s = um_add(um_add(s, 8), (SIZE_T)um_pal_count(h) * 4);
  return s;
}
static unsigned char *um_hpalette_marshal(ULONG *f, unsigned char *p, void *v) {
  HPALETTE h = *(HPALETTE *)v;
  p = um_align_ptr(p);
  if (LOWORD(*f) == MSHCTX_INPROC) {
    um_wr(&p, WDT_INPROC_CALL);
    um_wr(&p, (ULONG)(ULONG_PTR)h);
    return p;
  }
  um_wr(&p, WDT_REMOTE_CALL);
  um_wr(&p, um_ref_id(h));
  if (h) {
    ULONG n = um_pal_count(h);
    um_wr(&p, n);
    um_wr(&p, 0x300 | (n << 16)); /* palVersion, palNumEntries */
    if (n) {
      ULONG got = (UINT)UM_GDI("GetPaletteEntries", h, 0, n, p);
      if (got < n) mem_zero(p + got * 4, (SIZE_T)(n - got) * 4);
    }
    p += (SIZE_T)n * 4;
  }
  return p;
}
static unsigned char *um_hpalette_unmarshal(ULONG *f, unsigned char *p, void *v) {
  HPALETTE *ph = v;
  ULONG ctx, ref;
  (void)f;
  p = um_align_ptr(p);
  ctx = um_rd((const unsigned char **)&p);
  ref = um_rd((const unsigned char **)&p);
  if (ctx == WDT_INPROC_CALL) {
    *ph = um_h32(ref);
  } else if (ctx == WDT_REMOTE_CALL) {
    if (ref) {
      ULONG max = um_rd((const unsigned char **)&p), ver = um_rd((const unsigned char **)&p), n = ver >> 16;
      unsigned char *lp;
      if (max != n) um_raise(RPC_X_BAD_STUB_DATA);
      lp = mem_alloc(4 + (SIZE_T)n * 4);
      if (!lp) um_raise(RPC_X_NO_MEMORY);
      mem_copy(lp, &ver, 4); /* palVersion, palNumEntries */
      mem_copy(lp + 4, p, (SIZE_T)n * 4);
      *ph = (HPALETTE)UM_GDI("CreatePalette", lp, 0, 0, 0);
      mem_free(lp);
      p += (SIZE_T)n * 4;
    } else {
      *ph = NULL;
    }
  } else {
    um_raise(RPC_S_INVALID_TAG);
  }
  return p;
}
static void um_hpalette_free(ULONG *f, void *v) {
  HPALETTE *ph = v;
  if (LOWORD(*f) != MSHCTX_INPROC && *ph) UM_GDI("DeleteObject", *ph, 0, 0, 0);
}

/* ---- SNB (string name block): {chars, strings, chars (conformance), the strings with their NULs} ---- */
static void um_snb_count(const SNB snb, ULONG *chars, ULONG *strings) {
  *chars = *strings = 0;
  if (snb) {
    const WCHAR *const *pp;
    for (pp = (const WCHAR *const *)snb; *pp; pp++) {
      *chars = um_add(*chars, wlen(*pp) + 1);
      (*strings)++;
    }
  }
}
static ULONG um_snb_size(ULONG *f, ULONG s, void *v) {
  ULONG chars, strings;
  (void)f;
  um_snb_count(*(SNB *)v, &chars, &strings);
  return um_add(um_add(um_align(s), 12), (SIZE_T)chars * sizeof(WCHAR));
}
static unsigned char *um_snb_marshal(ULONG *f, unsigned char *p, void *v) {
  const WCHAR *const *pp = (const WCHAR *const *)*(SNB *)v;
  ULONG chars, strings;
  (void)f;
  um_snb_count((SNB)pp, &chars, &strings);
  p = um_align_ptr(p);
  um_wr(&p, chars);
  um_wr(&p, strings);
  um_wr(&p, chars);
  for (; pp && *pp; pp++) {
    SIZE_T n = (wlen(*pp) + 1) * sizeof(WCHAR);
    mem_copy(p, *pp, n);
    p += n;
  }
  return p;
}
static unsigned char *um_snb_unmarshal(ULONG *f, unsigned char *p, void *v) {
  SNB *psnb = v;
  ULONG chars, strings, conf, i, start, c;
  WCHAR **tab, *data;
  p = um_align_ptr(p);
  chars = um_rd((const unsigned char **)&p);
  strings = um_rd((const unsigned char **)&p);
  conf = um_rd((const unsigned char **)&p);
  if (chars == 0) {
    if (strings != 0 || conf != 0) um_raise(RPC_X_BAD_STUB_DATA);
    *psnb = NULL;
    return p;
  }
  if (conf != chars || chars > UM_MAXLEN / sizeof(WCHAR) || strings > chars) um_raise(RPC_X_BAD_STUB_DATA);
  tab = um_alloc(f, ((SIZE_T)strings + 1) * sizeof(WCHAR *) + (SIZE_T)chars * sizeof(WCHAR));
  if (!tab) um_raise(RPC_X_NO_MEMORY);
  data = (WCHAR *)(tab + strings + 1);
  mem_copy(data, p, (SIZE_T)chars * sizeof(WCHAR));
  for (i = 0, start = 0, c = 0; c < chars; c++) {
    if (data[c]) continue;
    if (i >= strings) break;
    tab[i++] = data + start;
    start = c + 1;
  }
  if (i != strings || start != chars) { /* count of strings or the final NUL does not match the header */
    um_free(f, tab);
    um_raise(RPC_X_BAD_STUB_DATA);
  }
  tab[strings] = NULL;
  *psnb = (SNB)tab;
  return p + (SIZE_T)chars * sizeof(WCHAR);
}
static void um_snb_free(ULONG *f, void *v) {
  SNB *psnb = v;
  if (*psnb) um_free(f, *psnb);
}

/* ---- interface pointers: {size, size, CoMarshalInterface data} (nothing for a NULL pointer) ---- */
EXPORT ULONG WINAPI WdtpInterfacePointer_UserSize(ULONG *flags, ULONG real_flags, ULONG start, IUnknown *punk, REFIID riid) {
  DWORD n = 0;
  (void)flags;
  if (!punk || FAILED(CoGetMarshalSizeMax(&n, riid, punk, LOWORD(real_flags), NULL, MSHLFLAGS_NORMAL))) return start;
  return um_add(um_add(um_align(start), 8), n);
}
EXPORT unsigned char *WINAPI WdtpInterfacePointer_UserMarshal(ULONG *flags, ULONG real_flags, unsigned char *p, IUnknown *punk, REFIID riid) {
  IStream *stm = NULL;
  HGLOBAL h = NULL;
  HRESULT hr;
  LARGE_INTEGER zero;
  ULARGE_INTEGER pos;
  const void *mem;
  (void)flags;
  if (!punk) return p;
  p = um_align_ptr(p);
  hr = CreateStreamOnHGlobal(NULL, TRUE, &stm);
  if (FAILED(hr)) um_raise(hr);
  hr = CoMarshalInterface(stm, riid, punk, LOWORD(real_flags), NULL, MSHLFLAGS_NORMAL);
  zero.QuadPart = 0;
  pos.QuadPart = 0;
  if (SUCCEEDED(hr)) hr = IStream_Seek(stm, zero, STREAM_SEEK_CUR, &pos);
  if (SUCCEEDED(hr)) hr = GetHGlobalFromStream(stm, &h);
  if (FAILED(hr) || pos.QuadPart > UM_MAXLEN || !(mem = GlobalLock(h))) {
    IStream_Release(stm);
    um_raise(FAILED(hr) ? (DWORD)hr : RPC_X_BAD_STUB_DATA);
  }
  um_wr(&p, (ULONG)pos.QuadPart);
  um_wr(&p, (ULONG)pos.QuadPart);
  mem_copy(p, mem, (SIZE_T)pos.QuadPart);
  p += (SIZE_T)pos.QuadPart;
  GlobalUnlock(h);
  IStream_Release(stm);
  return p;
}
EXPORT unsigned char *WINAPI WdtpInterfacePointer_UserUnmarshal(ULONG *flags, unsigned char *p, IUnknown **ppunk, REFIID riid) {
  ULONG n, n2;
  HGLOBAL h;
  IStream *stm = NULL;
  void *mem, *out = NULL;
  HRESULT hr;
  (void)flags;
  p = um_align_ptr(p);
  n = um_rd((const unsigned char **)&p);
  n2 = um_rd((const unsigned char **)&p);
  if (n != n2 || n > UM_MAXLEN) um_raise(RPC_X_BAD_STUB_DATA);
  h = GlobalAlloc(GMEM_MOVEABLE, n);
  mem = h ? GlobalLock(h) : NULL;
  if (!mem) {
    if (h) GlobalFree(h);
    um_raise(RPC_X_NO_MEMORY);
  }
  mem_copy(mem, p, n);
  GlobalUnlock(h);
  hr = CreateStreamOnHGlobal(h, TRUE, &stm);
  if (FAILED(hr)) {
    GlobalFree(h);
    um_raise(hr);
  }
  hr = CoUnmarshalInterface(stm, riid, &out);
  IStream_Release(stm);
  if (FAILED(hr)) um_raise(hr);
  if (*ppunk) IUnknown_Release(*ppunk);
  *ppunk = out;
  return p + n;
}
EXPORT void WINAPI WdtpInterfacePointer_UserFree(IUnknown *punk) {
  if (punk) IUnknown_Release(punk);
}
EXPORT ULONG WINAPI WdtpInterfacePointer_UserSize64(ULONG *flags, ULONG real_flags, ULONG start, IUnknown *punk, REFIID riid) {
  return WdtpInterfacePointer_UserSize(flags, real_flags, start, punk, riid);
}
EXPORT unsigned char *WINAPI WdtpInterfacePointer_UserMarshal64(ULONG *flags, ULONG real_flags, unsigned char *p, IUnknown *punk, REFIID riid) {
  return WdtpInterfacePointer_UserMarshal(flags, real_flags, p, punk, riid);
}
EXPORT unsigned char *WINAPI WdtpInterfacePointer_UserUnmarshal64(ULONG *flags, unsigned char *p, IUnknown **ppunk, REFIID riid) {
  return WdtpInterfacePointer_UserUnmarshal(flags, p, ppunk, riid);
}
EXPORT void WINAPI WdtpInterfacePointer_UserFree64(IUnknown *punk) { WdtpInterfacePointer_UserFree(punk); }

/* ---- STGMEDIUM: {tymed, [content id if tymed != NULL], pUnkForRelease id, content, [pUnkForRelease interface]} ---- */
static ULONG um_iface_size(ULONG *f, ULONG s, IUnknown *obj, REFIID riid) {
  IUnknown *unk = NULL;
  if (FAILED(IUnknown_QueryInterface(obj, &IID_IUnknown, (void **)&unk)) || !unk) { /* identity; fall back to the pointer itself */
    IUnknown_AddRef(obj);
    unk = obj;
  }
  s = WdtpInterfacePointer_UserSize(f, LOWORD(*f), s, unk, riid);
  IUnknown_Release(unk);
  return s;
}
static unsigned char *um_iface_marshal(ULONG *f, unsigned char *p, IUnknown *obj, REFIID riid) {
  IUnknown *unk = NULL;
  if (FAILED(IUnknown_QueryInterface(obj, &IID_IUnknown, (void **)&unk)) || !unk) {
    IUnknown_AddRef(obj);
    unk = obj;
  }
  p = WdtpInterfacePointer_UserMarshal(f, LOWORD(*f), p, unk, riid);
  IUnknown_Release(unk);
  return p;
}
static DWORD um_gdi_kind(HANDLE h) { /* the wire object type of a GDI handle: palette or (also when gdi32 cannot tell) bitmap */
  DWORD t = (DWORD)UM_GDI("GetObjectType", h, 0, 0, 0);
  if (t == 0 || t == UM_OBJ_BITMAP) return UM_OBJ_BITMAP;
  if (t == UM_OBJ_PAL) return UM_OBJ_PAL;
  um_raise(DV_E_TYMED);
}

static ULONG um_stgm_size(ULONG *f, ULONG s, void *v) {
  STGMEDIUM *m = v;
  s = um_add(um_align(s), m->tymed == TYMED_NULL ? 8 : 12);
  switch (m->tymed) {
    case TYMED_NULL: break;
    case TYMED_HGLOBAL: if (m->hGlobal) s = um_hglobal_size(f, s, &m->hGlobal); break;
    case TYMED_FILE: if (m->lpszFileName) s = um_add(um_add(s, 12), (wlen(m->lpszFileName) + 1) * sizeof(WCHAR)); break;
    case TYMED_ISTREAM: if (m->pstm) s = um_iface_size(f, s, (IUnknown *)m->pstm, &IID_IStream); break;
    case TYMED_ISTORAGE: if (m->pstg) s = um_iface_size(f, s, (IUnknown *)m->pstg, &IID_IStorage); break;
    case TYMED_GDI:
      if (m->hBitmap) s = um_gdi_kind(m->hBitmap) == UM_OBJ_PAL ? um_hpalette_size(f, um_add(s, 4), &m->hBitmap) : um_hbitmap_size(f, um_add(s, 4), &m->hBitmap);
      break;
    case TYMED_MFPICT: if (m->hMetaFilePict) s = um_hmfp_size(f, s, &m->hMetaFilePict); break;
    case TYMED_ENHMF: if (m->hEnhMetaFile) s = um_hemf_size(f, s, &m->hEnhMetaFile); break;
    default: um_raise(DV_E_TYMED);
  }
  if (m->pUnkForRelease) s = WdtpInterfacePointer_UserSize(f, LOWORD(*f), s, m->pUnkForRelease, &IID_IUnknown);
  return s;
}
static unsigned char *um_stgm_marshal(ULONG *f, unsigned char *p, void *v) {
  STGMEDIUM *m = v;
  p = um_align_ptr(p);
  um_wr(&p, m->tymed);
  if (m->tymed != TYMED_NULL) um_wr(&p, um_ref_id(m->pstg));
  um_wr(&p, um_ref_id(m->pUnkForRelease));
  switch (m->tymed) {
    case TYMED_NULL: break;
    case TYMED_HGLOBAL: if (m->hGlobal) p = um_hglobal_marshal(f, p, &m->hGlobal); break;
    case TYMED_FILE:
      if (m->lpszFileName) {
        ULONG n = (ULONG)wlen(m->lpszFileName) + 1;
        um_wr(&p, n);
        um_wr(&p, 0);
        um_wr(&p, n);
        mem_copy(p, m->lpszFileName, (SIZE_T)n * sizeof(WCHAR));
        p += (SIZE_T)n * sizeof(WCHAR);
      }
      break;
    case TYMED_ISTREAM: if (m->pstm) p = um_iface_marshal(f, p, (IUnknown *)m->pstm, &IID_IStream); break;
    case TYMED_ISTORAGE: if (m->pstg) p = um_iface_marshal(f, p, (IUnknown *)m->pstg, &IID_IStorage); break;
    case TYMED_GDI:
      if (m->hBitmap) {
        DWORD kind = um_gdi_kind(m->hBitmap);
        um_wr(&p, kind);
        p = kind == UM_OBJ_PAL ? um_hpalette_marshal(f, p, &m->hBitmap) : um_hbitmap_marshal(f, p, &m->hBitmap);
      }
      break;
    case TYMED_MFPICT: if (m->hMetaFilePict) p = um_hmfp_marshal(f, p, &m->hMetaFilePict); break;
    case TYMED_ENHMF: if (m->hEnhMetaFile) p = um_hemf_marshal(f, p, &m->hEnhMetaFile); break;
    default: um_raise(DV_E_TYMED);
  }
  if (m->pUnkForRelease) p = WdtpInterfacePointer_UserMarshal(f, LOWORD(*f), p, m->pUnkForRelease, &IID_IUnknown);
  return p;
}
static unsigned char *um_stgm_unmarshal(ULONG *f, unsigned char *p, void *v) {
  STGMEDIUM *m = v;
  ULONG tymed, content = 0, release;
  p = um_align_ptr(p);
  tymed = um_rd((const unsigned char **)&p);
  if (tymed != TYMED_NULL) content = um_rd((const unsigned char **)&p);
  release = um_rd((const unsigned char **)&p);
  switch (tymed) {
    case TYMED_NULL: case TYMED_HGLOBAL: case TYMED_FILE: case TYMED_ISTREAM: case TYMED_ISTORAGE: case TYMED_GDI: case TYMED_MFPICT: case TYMED_ENHMF: break;
    default: um_raise(DV_E_TYMED);
  }
  m->tymed = tymed;
  switch (tymed) {
    case TYMED_NULL: break;
    case TYMED_HGLOBAL: if (content) p = um_hglobal_unmarshal(f, p, &m->hGlobal); else m->hGlobal = NULL; break;
    case TYMED_FILE:
      m->lpszFileName = NULL;
      if (content) {
        ULONG max = um_rd((const unsigned char **)&p), off = um_rd((const unsigned char **)&p), act = um_rd((const unsigned char **)&p);
        WCHAR *name;
        if (max > UM_MAXLEN / sizeof(WCHAR) - 1 || off != 0 || act > max) um_raise(RPC_X_BAD_STUB_DATA);
        name = CoTaskMemAlloc(((SIZE_T)act + 1) * sizeof(WCHAR));
        if (!name) um_raise(RPC_X_NO_MEMORY);
        mem_copy(name, p, (SIZE_T)act * sizeof(WCHAR));
        name[act] = 0;
        p += (SIZE_T)act * sizeof(WCHAR);
        m->lpszFileName = name;
      }
      break;
    case TYMED_ISTREAM:
      if (content) p = WdtpInterfacePointer_UserUnmarshal(f, p, (IUnknown **)&m->pstm, &IID_IStream);
      else { if (m->pstm) IStream_Release(m->pstm); m->pstm = NULL; }
      break;
    case TYMED_ISTORAGE:
      if (content) p = WdtpInterfacePointer_UserUnmarshal(f, p, (IUnknown **)&m->pstg, &IID_IStorage);
      else { if (m->pstg) IStorage_Release(m->pstg); m->pstg = NULL; }
      break;
    case TYMED_GDI:
      m->hBitmap = NULL;
      if (content) {
        ULONG kind = um_rd((const unsigned char **)&p);
        if (kind == UM_OBJ_PAL) p = um_hpalette_unmarshal(f, p, &m->hBitmap);
        else if (kind == UM_OBJ_BITMAP) p = um_hbitmap_unmarshal(f, p, &m->hBitmap);
        else um_raise(RPC_X_BAD_STUB_DATA);
      }
      break;
    case TYMED_MFPICT: if (content) p = um_hmfp_unmarshal(f, p, &m->hMetaFilePict); else m->hMetaFilePict = NULL; break;
    case TYMED_ENHMF: if (content) p = um_hemf_unmarshal(f, p, &m->hEnhMetaFile); else m->hEnhMetaFile = NULL; break;
    default: break;
  }
  if (release) p = WdtpInterfacePointer_UserUnmarshal(f, p, &m->pUnkForRelease, &IID_IUnknown);
  return p;
}
static void um_stgm_free(ULONG *f, void *v) { (void)f; ReleaseStgMedium((STGMEDIUM *)v); }

/* ---- the exports: plain, 64 and Ext spellings share one implementation ---- */
#define UM_API(NAME, TYPE, SUF, SZ, MA, UN, FR)                                                                       \
  EXPORT ULONG WINAPI NAME##_UserSize##SUF(ULONG *f, ULONG s, TYPE *v) { return SZ(f, s, v); }                         \
  EXPORT unsigned char *WINAPI NAME##_UserMarshal##SUF(ULONG *f, unsigned char *b, TYPE *v) { return MA(f, b, v); }    \
  EXPORT unsigned char *WINAPI NAME##_UserUnmarshal##SUF(ULONG *f, unsigned char *b, TYPE *v) { return UN(f, b, v); } \
  EXPORT void WINAPI NAME##_UserFree##SUF(ULONG *f, TYPE *v) { FR(f, v); }
#define UM_API2(NAME, TYPE, kind) UM_API(NAME, TYPE, , um_##kind##_size, um_##kind##_marshal, um_##kind##_unmarshal, um_##kind##_free) \
                                  UM_API(NAME, TYPE, 64, um_##kind##_size, um_##kind##_marshal, um_##kind##_unmarshal, um_##kind##_free)
#define UM_API3(NAME, TYPE, kind) UM_API2(NAME, TYPE, kind) \
                                  UM_API(NAME, TYPE, Ext, um_##kind##_size, um_##kind##_marshal, um_##kind##_unmarshal, um_##kind##_free)

UM_API3(CLIPFORMAT, CLIPFORMAT, cf)
UM_API3(HWND, HWND, hnd)
UM_API3(HPALETTE, HPALETTE, hpalette)
UM_API3(STGMEDIUM, STGMEDIUM, stgm)
UM_API2(HACCEL, HACCEL, hnd)
UM_API2(HBITMAP, HBITMAP, hbitmap)
UM_API2(HBRUSH, HBRUSH, hnd)
UM_API2(HDC, HDC, hnd)
UM_API2(HENHMETAFILE, HENHMETAFILE, hemf)
UM_API2(HGLOBAL, HGLOBAL, hglobal)
UM_API2(HICON, HICON, hnd)
UM_API2(HMENU, HMENU, hnd)
UM_API2(HMETAFILEPICT, HMETAFILEPICT, hmfp)
UM_API2(HMETAFILE, HMETAFILE, hmf)
UM_API2(HMONITOR, HMONITOR, hnd)
UM_API2(SNB, SNB, snb)
UM_API(HRGN, HRGN, , um_hnd_size, um_hnd_marshal, um_hnd_unmarshal, um_hnd_free)

/* exports: CLIPFORMAT_UserSize CLIPFORMAT_UserMarshal CLIPFORMAT_UserUnmarshal CLIPFORMAT_UserFree CLIPFORMAT_UserSize64 CLIPFORMAT_UserMarshal64 CLIPFORMAT_UserUnmarshal64 CLIPFORMAT_UserFree64 CLIPFORMAT_UserSizeExt CLIPFORMAT_UserMarshalExt CLIPFORMAT_UserUnmarshalExt CLIPFORMAT_UserFreeExt HWND_UserSize HWND_UserMarshal HWND_UserUnmarshal HWND_UserFree HWND_UserSize64 HWND_UserMarshal64 HWND_UserUnmarshal64 HWND_UserFree64 HWND_UserSizeExt HWND_UserMarshalExt HWND_UserUnmarshalExt HWND_UserFreeExt HPALETTE_UserSize HPALETTE_UserMarshal HPALETTE_UserUnmarshal HPALETTE_UserFree HPALETTE_UserSize64 HPALETTE_UserMarshal64 HPALETTE_UserUnmarshal64 HPALETTE_UserFree64 HPALETTE_UserSizeExt HPALETTE_UserMarshalExt HPALETTE_UserUnmarshalExt HPALETTE_UserFreeExt STGMEDIUM_UserSize STGMEDIUM_UserMarshal STGMEDIUM_UserUnmarshal STGMEDIUM_UserFree STGMEDIUM_UserSize64 STGMEDIUM_UserMarshal64 STGMEDIUM_UserUnmarshal64 STGMEDIUM_UserFree64 STGMEDIUM_UserSizeExt STGMEDIUM_UserMarshalExt STGMEDIUM_UserUnmarshalExt STGMEDIUM_UserFreeExt HACCEL_UserSize HACCEL_UserMarshal HACCEL_UserUnmarshal HACCEL_UserFree HACCEL_UserSize64 HACCEL_UserMarshal64 HACCEL_UserUnmarshal64 HACCEL_UserFree64 HBITMAP_UserSize HBITMAP_UserMarshal HBITMAP_UserUnmarshal HBITMAP_UserFree HBITMAP_UserSize64 HBITMAP_UserMarshal64 HBITMAP_UserUnmarshal64 HBITMAP_UserFree64 HBRUSH_UserSize HBRUSH_UserMarshal HBRUSH_UserUnmarshal HBRUSH_UserFree HBRUSH_UserSize64 HBRUSH_UserMarshal64 HBRUSH_UserUnmarshal64 HBRUSH_UserFree64 HDC_UserSize HDC_UserMarshal HDC_UserUnmarshal HDC_UserFree HDC_UserSize64 HDC_UserMarshal64 HDC_UserUnmarshal64 HDC_UserFree64 HENHMETAFILE_UserSize HENHMETAFILE_UserMarshal HENHMETAFILE_UserUnmarshal HENHMETAFILE_UserFree HENHMETAFILE_UserSize64 HENHMETAFILE_UserMarshal64 HENHMETAFILE_UserUnmarshal64 HENHMETAFILE_UserFree64 HGLOBAL_UserSize HGLOBAL_UserMarshal HGLOBAL_UserUnmarshal HGLOBAL_UserFree HGLOBAL_UserSize64 HGLOBAL_UserMarshal64 HGLOBAL_UserUnmarshal64 HGLOBAL_UserFree64 HICON_UserSize HICON_UserMarshal HICON_UserUnmarshal HICON_UserFree HICON_UserSize64 HICON_UserMarshal64 HICON_UserUnmarshal64 HICON_UserFree64 HMENU_UserSize HMENU_UserMarshal HMENU_UserUnmarshal HMENU_UserFree HMENU_UserSize64 HMENU_UserMarshal64 HMENU_UserUnmarshal64 HMENU_UserFree64 HMETAFILEPICT_UserSize HMETAFILEPICT_UserMarshal HMETAFILEPICT_UserUnmarshal HMETAFILEPICT_UserFree HMETAFILEPICT_UserSize64 HMETAFILEPICT_UserMarshal64 HMETAFILEPICT_UserUnmarshal64 HMETAFILEPICT_UserFree64 HMETAFILE_UserSize HMETAFILE_UserMarshal HMETAFILE_UserUnmarshal HMETAFILE_UserFree HMETAFILE_UserSize64 HMETAFILE_UserMarshal64 HMETAFILE_UserUnmarshal64 HMETAFILE_UserFree64 HMONITOR_UserSize HMONITOR_UserMarshal HMONITOR_UserUnmarshal HMONITOR_UserFree HMONITOR_UserSize64 HMONITOR_UserMarshal64 HMONITOR_UserUnmarshal64 HMONITOR_UserFree64 SNB_UserSize SNB_UserMarshal SNB_UserUnmarshal SNB_UserFree SNB_UserSize64 SNB_UserMarshal64 SNB_UserUnmarshal64 SNB_UserFree64 HRGN_UserSize HRGN_UserMarshal HRGN_UserUnmarshal HRGN_UserFree WdtpInterfacePointer_UserSize WdtpInterfacePointer_UserMarshal WdtpInterfacePointer_UserUnmarshal WdtpInterfacePointer_UserFree WdtpInterfacePointer_UserSize64 WdtpInterfacePointer_UserMarshal64 WdtpInterfacePointer_UserUnmarshal64 WdtpInterfacePointer_UserFree64 */
