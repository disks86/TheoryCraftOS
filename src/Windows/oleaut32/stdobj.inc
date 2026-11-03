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

/* The standard OLE objects: StdFont and StdPicture (with the image decoders behind OleLoadPicture), OleTranslateColor, the property frame, and the DLL's class objects.
 * gdi32 and user32 are loaded when first needed (the BSTR and VARIANT users of this DLL should not pull the graphics libraries into every process), so every call
 * into them goes through the so_* wrappers below. */

#define SO_MAX_BLOB (256u * 1024u * 1024u)     /* the most that is read from a stream or file for one picture */

/* ---- lazily bound gdi32 / user32 calls ------------------------------------------------------------------------------------------------------------------ */
static void *so_import(const char *library, const char *name) {
  HMODULE module = GetModuleHandleA(library);
  if (!module) module = LoadLibraryA(library);
  return module ? (void *)GetProcAddress(module, name) : NULL;
}
#define SO_IMP(library, ret, name, params, args) \
  static ret so_##name params { static ret (WINAPI *fn) params; static LONG tried; if (!tried) { fn = (void *)so_import(library, #name); tried = 1; } return fn ? fn args : (ret)0; }
#define SO_IMPV(library, name, params, args) \
  static void so_##name params { static void (WINAPI *fn) params; static LONG tried; if (!tried) { fn = (void *)so_import(library, #name); tried = 1; } if (fn) fn args; }

SO_IMP("gdi32.dll", HFONT, CreateFontIndirectW, (const LOGFONTW *lf), (lf))
SO_IMP("gdi32.dll", BOOL, DeleteObject, (HGDIOBJ object), (object))
SO_IMP("gdi32.dll", int, GetObjectW, (HANDLE object, int count, void *buffer), (object, count, buffer))
SO_IMP("gdi32.dll", int, GetDeviceCaps, (HDC dc, int index), (dc, index))
SO_IMP("gdi32.dll", HDC, CreateCompatibleDC, (HDC dc), (dc))
SO_IMP("gdi32.dll", BOOL, DeleteDC, (HDC dc), (dc))
SO_IMP("gdi32.dll", HGDIOBJ, SelectObject, (HDC dc, HGDIOBJ object), (dc, object))
SO_IMP("gdi32.dll", BOOL, GetTextMetricsW, (HDC dc, TEXTMETRICW *metrics), (dc, metrics))
SO_IMP("gdi32.dll", HBITMAP, CreateDIBSection, (HDC dc, const BITMAPINFO *info, UINT usage, void **bits, HANDLE section, DWORD offset), (dc, info, usage, bits, section, offset))
SO_IMP("gdi32.dll", int, GetDIBits, (HDC dc, HBITMAP bitmap, UINT start, UINT lines, void *bits, BITMAPINFO *info, UINT usage), (dc, bitmap, start, lines, bits, info, usage))
SO_IMP("gdi32.dll", BOOL, StretchBlt, (HDC dst, int x, int y, int cx, int cy, HDC src, int xs, int ys, int cxs, int cys, DWORD rop), (dst, x, y, cx, cy, src, xs, ys, cxs, cys, rop))
SO_IMP("gdi32.dll", UINT, GetEnhMetaFileHeader, (HENHMETAFILE emf, UINT size, ENHMETAHEADER *header), (emf, size, header))
SO_IMP("gdi32.dll", UINT, GetEnhMetaFileBits, (HENHMETAFILE emf, UINT size, BYTE *bits), (emf, size, bits))
SO_IMP("gdi32.dll", HENHMETAFILE, SetEnhMetaFileBits, (UINT size, const BYTE *bits), (size, bits))
SO_IMP("gdi32.dll", BOOL, DeleteEnhMetaFile, (HENHMETAFILE emf), (emf))
SO_IMP("gdi32.dll", BOOL, PlayEnhMetaFile, (HDC dc, HENHMETAFILE emf, const RECT *rect), (dc, emf, rect))
SO_IMP("gdi32.dll", UINT, GetMetaFileBitsEx, (HMETAFILE wmf, UINT size, void *bits), (wmf, size, bits))
SO_IMP("gdi32.dll", HMETAFILE, SetMetaFileBitsEx, (UINT size, const BYTE *bits), (size, bits))
SO_IMP("gdi32.dll", BOOL, DeleteMetaFile, (HMETAFILE wmf), (wmf))
SO_IMP("gdi32.dll", BOOL, PlayMetaFile, (HDC dc, HMETAFILE wmf), (dc, wmf))
SO_IMP("gdi32.dll", int, SetMapMode, (HDC dc, int mode), (dc, mode))
SO_IMP("gdi32.dll", BOOL, SetWindowOrgEx, (HDC dc, int x, int y, POINT *old), (dc, x, y, old))
SO_IMP("gdi32.dll", BOOL, SetWindowExtEx, (HDC dc, int cx, int cy, SIZE *old), (dc, cx, cy, old))
SO_IMP("gdi32.dll", BOOL, SetViewportOrgEx, (HDC dc, int x, int y, POINT *old), (dc, x, y, old))
SO_IMP("gdi32.dll", BOOL, SetViewportExtEx, (HDC dc, int cx, int cy, SIZE *old), (dc, cx, cy, old))
SO_IMP("gdi32.dll", int, SaveDC, (HDC dc), (dc))
SO_IMP("gdi32.dll", BOOL, RestoreDC, (HDC dc, int saved), (dc, saved))
SO_IMP("gdi32.dll", UINT, GetPaletteEntries, (HPALETTE palette, UINT start, UINT count, PALETTEENTRY *entries), (palette, start, count, entries))
SO_IMP("gdi32.dll", UINT, GetNearestPaletteIndex, (HPALETTE palette, COLORREF color), (palette, color))
SO_IMP("gdi32.dll", HGDIOBJ, GetStockObject, (int object), (object))
SO_IMP("user32.dll", HDC, GetDC, (HWND window), (window))
SO_IMP("user32.dll", int, ReleaseDC, (HWND window, HDC dc), (window, dc))
SO_IMP("user32.dll", BOOL, GetIconInfo, (HICON icon, ICONINFO *info), (icon, info))
SO_IMP("user32.dll", BOOL, DrawIconEx, (HDC dc, int x, int y, HICON icon, int cx, int cy, UINT step, HBRUSH brush, UINT flags), (dc, x, y, icon, cx, cy, step, brush, flags))
SO_IMP("user32.dll", BOOL, DestroyIcon, (HICON icon), (icon))
SO_IMP("user32.dll", HICON, CopyIcon, (HICON icon), (icon))
SO_IMP("user32.dll", HICON, CreateIconFromResourceEx, (PBYTE bits, DWORD size, BOOL icon, DWORD version, int cx, int cy, UINT flags), (bits, size, icon, version, cx, cy, flags))
SO_IMP("user32.dll", DWORD, GetSysColor, (int index), (index))
SO_IMP("user32.dll", int, GetSystemMetrics, (int index), (index))

/* ---- identifiers ----------------------------------------------------------------------------------------------------------------------------------------- */
static const CLSID so_CLSID_StdFont = {0x0be35203, 0x8f91, 0x11ce, {0x9d, 0xe3, 0x00, 0xaa, 0x00, 0x4b, 0xb8, 0x51}};
static const CLSID so_CLSID_StdPicture = {0x0be35204, 0x8f91, 0x11ce, {0x9d, 0xe3, 0x00, 0xaa, 0x00, 0x4b, 0xb8, 0x51}};

static const GUID so_null_iid = {0, 0, 0, {0, 0, 0, 0, 0, 0, 0, 0}};

/* ---- shared state: the objects alive (for DllCanUnloadNow) and one lock for the small global structures --------------------------------------------------- */
static LONG so_objects;
static LONG so_server_locks;
static LONG so_lock_flag;
static void so_lock(void) { while (__sync_lock_test_and_set(&so_lock_flag, 1)) Sleep(0); }
static void so_unlock(void) { __sync_lock_release(&so_lock_flag); }

static WCHAR *so_wcsdup(const WCHAR *s) {
  SIZE_T n = oa_wcslen(s) + 1;
  WCHAR *copy = oa_alloc(n * sizeof(WCHAR));
  if (copy) memcpy(copy, s, n * sizeof(WCHAR));
  return copy;
}
static WCHAR so_wlower(WCHAR c) { return oa_towlower(c); }
static int so_wcsicmp(const WCHAR *a, const WCHAR *b) {
  while (*a && so_wlower(*a) == so_wlower(*b)) { a++; b++; }
  return (int)so_wlower(*a) - (int)so_wlower(*b);
}

/* A stream read that must deliver all of it. */
static HRESULT so_read_exact(IStream *stream, void *buffer, ULONG size) {
  ULONG got = 0;
  HRESULT hr = IStream_Read(stream, buffer, size, &got);
  if (FAILED(hr)) return hr;
  return got == size ? S_OK : E_FAIL;
}
static HRESULT so_write_all(IStream *stream, const void *buffer, ULONG size) {
  ULONG put = 0;
  HRESULT hr = IStream_Write(stream, buffer, size, &put);
  if (FAILED(hr)) return hr;
  return put == size ? S_OK : STG_E_MEDIUMFULL;
}

/* A VARIANT argument as the type wanted: the exact type is taken as it is, anything else goes through VariantChangeType (then *owned says to clear the result). */
static HRESULT so_variant_as(VARIANT *out, const VARIANT *in, VARTYPE vt, BOOL *owned) {
  VariantInit(out);
  *owned = FALSE;
  if (V_VT(in) == vt) {
    memcpy(out, in, sizeof(VARIANT));       /* (a shallow copy: the caller does not clear it) */
    return S_OK;
  }
  HRESULT hr = VariantChangeType(out, (VARIANT *)in, 0, vt);
  if (FAILED(hr)) return hr == DISP_E_BADVARTYPE || hr == E_INVALIDARG ? DISP_E_TYPEMISMATCH : hr;
  *owned = TRUE;
  return S_OK;
}

/* ---- connection points ----------------------------------------------------------------------------------------------------------------------------------- */
/* A connection point lives inside its owner and shares the owner's reference count, so it can never outlive the object it reports about. */
typedef struct so_cp {
  IConnectionPoint iface;
  IUnknown *owner;
  const IID *iid;
  BOOL dispatch_sink;            /* the sink is a dispinterface: IDispatch is good enough when it does not know the IID */
  IUnknown **sinks;
  DWORD *cookies;
  UINT count, capacity;
  DWORD next_cookie;
} so_cp;

typedef struct so_enum_conn {
  IEnumConnections iface;
  LONG ref;
  CONNECTDATA *items;
  ULONG count, position;
} so_enum_conn;

static so_enum_conn *so_enum_conn_from(IEnumConnections *iface) { return CONTAINING_RECORD(iface, so_enum_conn, iface); }
static const IEnumConnectionsVtbl so_enum_conn_vtbl;

static so_enum_conn *so_enum_conn_make(const CONNECTDATA *source, ULONG count, ULONG position) {
  so_enum_conn *e = oa_calloc(sizeof(*e));
  if (!e) return NULL;
  e->iface.lpVtbl = (void *)&so_enum_conn_vtbl;
  e->ref = 1;
  if (count) {
    e->items = oa_alloc((SIZE_T)count * sizeof(CONNECTDATA));
    if (!e->items) { oa_free(e); return NULL; }
    for (ULONG i = 0; i < count; i++) { e->items[i] = source[i]; IUnknown_AddRef(e->items[i].pUnk); }
  }
  e->count = count;
  e->position = position;
  return e;
}
static HRESULT WINAPI so_enum_conn_QueryInterface(IEnumConnections *iface, REFIID riid, void **ppv) {
  if (!ppv) return E_POINTER;
  if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IEnumConnections)) { *ppv = iface; IEnumConnections_AddRef(iface); return S_OK; }
  *ppv = NULL;
  return E_NOINTERFACE;
}
static ULONG WINAPI so_enum_conn_AddRef(IEnumConnections *iface) { return (ULONG)__sync_add_and_fetch(&so_enum_conn_from(iface)->ref, 1); }
static ULONG WINAPI so_enum_conn_Release(IEnumConnections *iface) {
  so_enum_conn *e = so_enum_conn_from(iface);
  LONG ref = __sync_sub_and_fetch(&e->ref, 1);
  if (ref == 0) {
    for (ULONG i = 0; i < e->count; i++) IUnknown_Release(e->items[i].pUnk);
    oa_free(e->items);
    oa_free(e);
  }
  return (ULONG)ref;
}
static HRESULT WINAPI so_enum_conn_Next(IEnumConnections *iface, ULONG count, CONNECTDATA *out, ULONG *fetched) {
  so_enum_conn *e = so_enum_conn_from(iface);
  ULONG n = 0;
  if (!out || (!fetched && count != 1)) return E_POINTER;
  while (n < count && e->position < e->count) {
    out[n] = e->items[e->position++];
    IUnknown_AddRef(out[n].pUnk);
    n++;
  }
  if (fetched) *fetched = n;
  return n == count ? S_OK : S_FALSE;
}
static HRESULT WINAPI so_enum_conn_Skip(IEnumConnections *iface, ULONG count) {
  so_enum_conn *e = so_enum_conn_from(iface);
  if (count > e->count - e->position) { e->position = e->count; return S_FALSE; }
  e->position += count;
  return S_OK;
}
static HRESULT WINAPI so_enum_conn_Reset(IEnumConnections *iface) { so_enum_conn_from(iface)->position = 0; return S_OK; }
static HRESULT WINAPI so_enum_conn_Clone(IEnumConnections *iface, IEnumConnections **out) {
  so_enum_conn *e = so_enum_conn_from(iface), *copy;
  if (!out) return E_POINTER;
  copy = so_enum_conn_make(e->items, e->count, e->position);
  if (!copy) { *out = NULL; return E_OUTOFMEMORY; }
  *out = &copy->iface;
  return S_OK;
}
static const IEnumConnectionsVtbl so_enum_conn_vtbl = {
  so_enum_conn_QueryInterface, so_enum_conn_AddRef, so_enum_conn_Release, so_enum_conn_Next, so_enum_conn_Skip, so_enum_conn_Reset, so_enum_conn_Clone
};

static so_cp *so_cp_from(IConnectionPoint *iface) { return CONTAINING_RECORD(iface, so_cp, iface); }
static HRESULT WINAPI so_cp_QueryInterface(IConnectionPoint *iface, REFIID riid, void **ppv) {
  if (!ppv) return E_POINTER;
  if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IConnectionPoint)) { *ppv = iface; IUnknown_AddRef(so_cp_from(iface)->owner); return S_OK; }
  *ppv = NULL;
  return E_NOINTERFACE;
}
static ULONG WINAPI so_cp_AddRef(IConnectionPoint *iface) { return IUnknown_AddRef(so_cp_from(iface)->owner); }
static ULONG WINAPI so_cp_Release(IConnectionPoint *iface) { return IUnknown_Release(so_cp_from(iface)->owner); }
static HRESULT WINAPI so_cp_GetConnectionInterface(IConnectionPoint *iface, IID *iid) {
  if (!iid) return E_POINTER;
  *iid = *so_cp_from(iface)->iid;
  return S_OK;
}
static HRESULT WINAPI so_cp_GetConnectionPointContainer(IConnectionPoint *iface, IConnectionPointContainer **out) {
  if (!out) return E_POINTER;
  return IUnknown_QueryInterface(so_cp_from(iface)->owner, &IID_IConnectionPointContainer, (void **)out);
}
static HRESULT WINAPI so_cp_Advise(IConnectionPoint *iface, IUnknown *sink_unknown, DWORD *cookie) {
  so_cp *cp = so_cp_from(iface);
  IUnknown *sink = NULL;
  if (!sink_unknown || !cookie) return E_POINTER;
  HRESULT hr = IUnknown_QueryInterface(sink_unknown, cp->iid, (void **)&sink);
  if (FAILED(hr) && cp->dispatch_sink) hr = IUnknown_QueryInterface(sink_unknown, &IID_IDispatch, (void **)&sink);
  if (FAILED(hr) || !sink) return CONNECT_E_CANNOTCONNECT;
  so_lock();
  if (cp->count == cp->capacity) {
    UINT capacity = cp->capacity ? cp->capacity * 2 : 4;
    IUnknown **sinks = oa_realloc(cp->sinks, (SIZE_T)capacity * sizeof(*sinks));
    if (sinks) cp->sinks = sinks;
    DWORD *cookies = oa_realloc(cp->cookies, (SIZE_T)capacity * sizeof(*cookies));
    if (cookies) cp->cookies = cookies;
    if (!sinks || !cookies) { so_unlock(); IUnknown_Release(sink); return E_OUTOFMEMORY; }
    cp->capacity = capacity;
  }
  *cookie = ++cp->next_cookie;
  cp->sinks[cp->count] = sink;
  cp->cookies[cp->count] = *cookie;
  cp->count++;
  so_unlock();
  return S_OK;
}
static HRESULT WINAPI so_cp_Unadvise(IConnectionPoint *iface, DWORD cookie) {
  so_cp *cp = so_cp_from(iface);
  IUnknown *gone = NULL;
  so_lock();
  for (UINT i = 0; i < cp->count; i++) {
    if (cp->cookies[i] != cookie) continue;
    gone = cp->sinks[i];
    for (UINT j = i + 1; j < cp->count; j++) { cp->sinks[j - 1] = cp->sinks[j]; cp->cookies[j - 1] = cp->cookies[j]; }
    cp->count--;
    break;
  }
  so_unlock();
  if (!gone) return CONNECT_E_NOCONNECTION;
  IUnknown_Release(gone);
  return S_OK;
}
static HRESULT WINAPI so_cp_EnumConnections(IConnectionPoint *iface, IEnumConnections **out) {
  so_cp *cp = so_cp_from(iface);
  CONNECTDATA *items = NULL;
  if (!out) return E_POINTER;
  *out = NULL;
  so_lock();
  UINT count = cp->count;
  if (count) {
    items = oa_alloc((SIZE_T)count * sizeof(*items));
    if (!items) { so_unlock(); return E_OUTOFMEMORY; }
    for (UINT i = 0; i < count; i++) { items[i].pUnk = cp->sinks[i]; items[i].dwCookie = cp->cookies[i]; }
  }
  so_unlock();
  if (!count) { *out = NULL; return OLE_E_NOCONNECTION; }
  so_enum_conn *e = so_enum_conn_make(items, count, 0);       /* (the sinks are kept alive by the enumerator before anyone can unadvise them: single owner thread) */
  oa_free(items);
  if (!e) return E_OUTOFMEMORY;
  *out = &e->iface;
  return S_OK;
}
static const IConnectionPointVtbl so_cp_vtbl = {
  so_cp_QueryInterface, so_cp_AddRef, so_cp_Release, so_cp_GetConnectionInterface, so_cp_GetConnectionPointContainer, so_cp_Advise, so_cp_Unadvise, so_cp_EnumConnections
};

static void so_cp_init(so_cp *cp, IUnknown *owner, const IID *iid, BOOL dispatch_sink) {
  cp->iface.lpVtbl = (void *)&so_cp_vtbl;
  cp->owner = owner;
  cp->iid = iid;
  cp->dispatch_sink = dispatch_sink;
}
static void so_cp_free(so_cp *cp) {
  for (UINT i = 0; i < cp->count; i++) IUnknown_Release(cp->sinks[i]);
  oa_free(cp->sinks);
  oa_free(cp->cookies);
  cp->sinks = NULL; cp->cookies = NULL; cp->count = cp->capacity = 0;
}
/* A snapshot of the sinks (each with a reference), for calling out without the lock. The result is freed with so_cp_release_snapshot. */
static IUnknown **so_cp_snapshot(so_cp *cp, UINT *count) {
  IUnknown **copy = NULL;
  so_lock();
  *count = 0;
  if (cp->count && (copy = oa_alloc((SIZE_T)cp->count * sizeof(*copy)))) {
    for (UINT i = 0; i < cp->count; i++) { copy[i] = cp->sinks[i]; IUnknown_AddRef(copy[i]); }
    *count = cp->count;
  }
  so_unlock();
  return copy;
}
static void so_cp_release_snapshot(IUnknown **copy, UINT count) {
  for (UINT i = 0; i < count; i++) IUnknown_Release(copy[i]);
  oa_free(copy);
}
/* The container side: the points an object has, by IID. */
static HRESULT so_cpc_find(so_cp *const *points, UINT n, REFIID riid, IConnectionPoint **out) {
  if (!out) return E_POINTER;
  *out = NULL;
  for (UINT i = 0; i < n; i++) {
    if (!IsEqualIID(riid, points[i]->iid)) continue;
    *out = &points[i]->iface;
    IConnectionPoint_AddRef(*out);
    return S_OK;
  }
  return CONNECT_E_NOCONNECTION;
}

/* IPropertyNotifySink::OnChanged for a point that holds those sinks. */
static void so_notify_changed(so_cp *cp, DISPID dispid) {
  UINT count;
  IUnknown **sinks = so_cp_snapshot(cp, &count);
  for (UINT i = 0; i < count; i++) IPropertyNotifySink_OnChanged((IPropertyNotifySink *)sinks[i], dispid);
  so_cp_release_snapshot(sinks, count);
}

#include "stdfont.inc"
#include "imgdec.inc"
#include "jpeg.inc"
#include "stdpic.inc"
#include "stdmisc.inc"
#include "stdframe.inc"
