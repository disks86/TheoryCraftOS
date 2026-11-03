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

/* The standard font object (OleCreateFontIndirect): IFont and its dispinterface IFontDisp, persistence to a stream and a property bag, change notifications, and the shared
 * cache of the GDI fonts that the objects realize. The properties are Name, Size (a currency value: points * 10000), Bold, Italic, Underline, Strikethrough, Weight, Charset. */

/* ---- the HFONT cache --------------------------------------------------------------------------------------------------------------------------------------- */
/* Fonts with the same description share one HFONT. An item counts the references held by IFont objects (int_refs: the object's current font) and the total, which the
 * AddRefHfont / ReleaseHfont calls change too; the HFONT goes away when the total reaches zero as an object lets go of it, and whatever is left when the last IFont is
 * gone (an item whose total went negative: more releases than references) is dropped with it. */
typedef struct so_hfont_item {
  struct so_hfont_item *next;
  HFONT hfont;
  LONG int_refs, total_refs;
  LOGFONTW lf;
} so_hfont_item;

static so_hfont_item *so_hfonts;
static LONG so_font_objects;

static so_hfont_item *so_hfont_find(HFONT hfont) {
  for (so_hfont_item *i = so_hfonts; i; i = i->next) if (i->hfont == hfont) return i;
  return NULL;
}
static void so_hfont_remove(so_hfont_item *item) {
  for (so_hfont_item **p = &so_hfonts; *p; p = &(*p)->next) {
    if (*p != item) continue;
    *p = item->next;
    break;
  }
  so_DeleteObject(item->hfont);
  oa_free(item);
}
/* The font for a description: an existing item gets another internal reference, otherwise a font is made. */
static HFONT so_hfont_acquire(const LOGFONTW *lf) {
  so_lock();
  for (so_hfont_item *i = so_hfonts; i; i = i->next) {
    if (memcmp(&i->lf, lf, sizeof(*lf)) != 0) continue;
    i->int_refs++; i->total_refs++;
    HFONT hfont = i->hfont;
    so_unlock();
    return hfont;
  }
  so_unlock();
  HFONT hfont = so_CreateFontIndirectW(lf);
  if (!hfont) return NULL;
  so_hfont_item *item = oa_calloc(sizeof(*item));
  if (!item) { so_DeleteObject(hfont); return NULL; }
  item->hfont = hfont; item->int_refs = 1; item->total_refs = 1; item->lf = *lf;
  so_lock();
  item->next = so_hfonts;
  so_hfonts = item;
  so_unlock();
  return hfont;
}
static void so_hfont_drop(HFONT hfont) {
  so_lock();
  so_hfont_item *item = so_hfont_find(hfont);
  if (item) {
    item->int_refs--;
    if (--item->total_refs == 0) so_hfont_remove(item);
  }
  so_unlock();
}
static HRESULT so_hfont_ext_add(HFONT hfont) {
  HRESULT hr = S_FALSE;
  so_lock();
  so_hfont_item *item = so_hfont_find(hfont);
  if (item) { item->total_refs++; hr = S_OK; }
  so_unlock();
  return hr;
}
static HRESULT so_hfont_ext_release(HFONT hfont) {
  HRESULT hr = S_FALSE;
  so_lock();
  so_hfont_item *item = so_hfont_find(hfont);
  if (item && item->total_refs > 0) { item->total_refs--; hr = S_OK; }
  so_unlock();
  return hr;
}
static void so_hfont_clear(void) {
  so_lock();
  while (so_hfonts) so_hfont_remove(so_hfonts);
  so_unlock();
}

/* ---- the object -------------------------------------------------------------------------------------------------------------------------------------------- */
typedef struct so_font {
  IFont ifont;
  IDispatch idisp;
  IPersistStream ipstream;
  IConnectionPointContainer icpc;
  IPersistPropertyBag ipbag;
  LONG ref;
  FONTDESC desc;                 /* lpstrName is a copy of the object's */
  HFONT gdi;                     /* the realized font (one internal reference), NULL until needed or after a change */
  BOOL dirty;
  LONG cy_logical, cy_himetric;  /* the ratio between logical units and HIMETRIC (screen dpi and 2540 by default) */
  ITypeInfo *typeinfo;
  so_cp cp_notify, cp_events;
} so_font;

static const IFontVtbl so_font_vtbl;
static const IDispatchVtbl so_font_disp_vtbl;
static const IPersistStreamVtbl so_font_persist_vtbl;
static const IConnectionPointContainerVtbl so_font_cpc_vtbl;
static const IPersistPropertyBagVtbl so_font_pbag_vtbl;

static so_font *so_font_from(IFont *iface) { return CONTAINING_RECORD(iface, so_font, ifont); }
static so_font *so_font_from_disp(IDispatch *iface) { return CONTAINING_RECORD(iface, so_font, idisp); }
static so_font *so_font_from_persist(IPersistStream *iface) { return CONTAINING_RECORD(iface, so_font, ipstream); }
static so_font *so_font_from_cpc(IConnectionPointContainer *iface) { return CONTAINING_RECORD(iface, so_font, icpc); }
static so_font *so_font_from_pbag(IPersistPropertyBag *iface) { return CONTAINING_RECORD(iface, so_font, ipbag); }

static LONG so_screen_dpi_y(void) {
  HDC dc = so_GetDC(NULL);
  LONG dpi = dc ? so_GetDeviceCaps(dc, LOGPIXELSY) : 0;
  if (dc) so_ReleaseDC(NULL, dc);
  return dpi > 0 ? dpi : 96;
}

static const WCHAR so_default_font_name[] = {'M','S',' ','S','a','n','s',' ','S','e','r','i','f',0};

static HRESULT so_font_create(const FONTDESC *desc, REFIID riid, void **ppv);

static so_font *so_font_new(const FONTDESC *desc) {
  so_font *f = oa_calloc(sizeof(*f));
  if (!f) return NULL;
  f->ifont.lpVtbl = (void *)&so_font_vtbl;
  f->idisp.lpVtbl = (void *)&so_font_disp_vtbl;
  f->ipstream.lpVtbl = (void *)&so_font_persist_vtbl;
  f->icpc.lpVtbl = (void *)&so_font_cpc_vtbl;
  f->ipbag.lpVtbl = (void *)&so_font_pbag_vtbl;
  f->ref = 1;
  f->desc.cbSizeofstruct = sizeof(FONTDESC);
  if (desc) {
    f->desc = *desc;
    f->desc.cbSizeofstruct = sizeof(FONTDESC);
    f->desc.lpstrName = so_wcsdup(desc->lpstrName);
  } else {
    f->desc.lpstrName = so_wcsdup(so_default_font_name);
    f->desc.cySize.int64 = 80000;           /* 8 points */
    f->desc.sWeight = FW_NORMAL;
  }
  if (!f->desc.lpstrName) { oa_free(f); return NULL; }
  f->dirty = TRUE;
  f->cy_logical = so_screen_dpi_y();
  f->cy_himetric = 2540;
  so_cp_init(&f->cp_notify, (IUnknown *)&f->ifont, &IID_IPropertyNotifySink, FALSE);
  so_cp_init(&f->cp_events, (IUnknown *)&f->ifont, &IID_IFontEventsDisp, TRUE);
  __sync_add_and_fetch(&so_objects, 1);
  __sync_add_and_fetch(&so_font_objects, 1);
  return f;
}

static void so_font_destroy(so_font *f) {
  if (f->gdi) so_hfont_drop(f->gdi);
  if (f->typeinfo) ITypeInfo_Release(f->typeinfo);
  so_cp_free(&f->cp_notify);
  so_cp_free(&f->cp_events);
  oa_free(f->desc.lpstrName);
  oa_free(f);
  if (__sync_sub_and_fetch(&so_font_objects, 1) == 0) so_hfont_clear();
  __sync_sub_and_fetch(&so_objects, 1);
}

/* ---- change notification ----------------------------------------------------------------------------------------------------------------------------------- */
static void so_font_notify(so_font *f, DISPID dispid, const WCHAR *name) {
  UINT count;
  so_notify_changed(&f->cp_notify, dispid);
  IUnknown **sinks = so_cp_snapshot(&f->cp_events, &count);
  for (UINT i = 0; i < count; i++) {
    VARIANT arg;
    DISPPARAMS params;
    VariantInit(&arg);
    V_VT(&arg) = VT_BSTR;
    V_BSTR(&arg) = SysAllocString(name);
    params.rgvarg = &arg;
    params.rgdispidNamedArgs = NULL;
    params.cArgs = 1;
    params.cNamedArgs = 0;
    IDispatch_Invoke((IDispatch *)sinks[i], DISPID_FONT_CHANGED, &so_null_iid, 0, DISPATCH_METHOD, &params, NULL, NULL, NULL);
    SysFreeString(V_BSTR(&arg));
  }
  so_cp_release_snapshot(sinks, count);
}

/* ---- realization ------------------------------------------------------------------------------------------------------------------------------------------- */
static LONG so_font_height(const so_font *f) {
  LONG logical = f->cy_logical, himetric = f->cy_himetric;
  if (logical == 1 && himetric == 1) { logical = so_screen_dpi_y(); himetric = 2540; }     /* (1:1 means "not set") */
  /* points * 10000 -> HIMETRIC (2540 per inch, 72 points per inch) -> logical units */
  double h = (double)f->desc.cySize.int64 * 2540.0 * (double)logical / (720000.0 * (double)himetric);
  if (h > 32767.0) h = 32767.0;
  if (h < -32767.0) h = -32767.0;
  return (LONG)(h >= 0 ? oa_floor(h + 0.5) : oa_ceil(h - 0.5));
}

/* The GDI font for the description; a changed description makes a new one (and lets go of the old one). */
static HFONT so_font_realize(so_font *f) {
  if (!f->dirty && f->gdi) return f->gdi;
  LOGFONTW lf;
  memset(&lf, 0, sizeof(lf));
  for (int i = 0; i < LF_FACESIZE - 1 && f->desc.lpstrName[i]; i++) lf.lfFaceName[i] = f->desc.lpstrName[i];
  lf.lfHeight = -so_font_height(f);
  lf.lfWeight = f->desc.sWeight;
  lf.lfItalic = f->desc.fItalic ? TRUE : FALSE;
  lf.lfUnderline = f->desc.fUnderline ? TRUE : FALSE;
  lf.lfStrikeOut = f->desc.fStrikethrough ? TRUE : FALSE;
  lf.lfCharSet = (BYTE)f->desc.sCharset;
  HFONT hfont = so_hfont_acquire(&lf);
  if (f->gdi) so_hfont_drop(f->gdi);
  f->gdi = hfont;
  f->dirty = FALSE;
  if (hfont) {
    /* the charset of the font the mapper chose is the one the object reports */
    HDC dc = so_CreateCompatibleDC(NULL);
    if (dc) {
      TEXTMETRICW tm;
      HGDIOBJ old = so_SelectObject(dc, hfont);
      memset(&tm, 0, sizeof(tm));
      if (so_GetTextMetricsW(dc, &tm) && tm.tmHeight != 0) f->desc.sCharset = tm.tmCharSet;
      if (old) so_SelectObject(dc, old);
      so_DeleteDC(dc);
    }
  }
  return hfont;
}

/* ---- IFont ------------------------------------------------------------------------------------------------------------------------------------------------- */
static HRESULT WINAPI so_font_QueryInterface(IFont *iface, REFIID riid, void **ppv) {
  so_font *f = so_font_from(iface);
  if (!ppv) return E_POINTER;
  *ppv = NULL;
  if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IFont)) *ppv = &f->ifont;
  else if (IsEqualIID(riid, &IID_IDispatch) || IsEqualIID(riid, &IID_IFontDisp)) *ppv = &f->idisp;
  else if (IsEqualIID(riid, &IID_IPersist) || IsEqualIID(riid, &IID_IPersistStream)) *ppv = &f->ipstream;
  else if (IsEqualIID(riid, &IID_IConnectionPointContainer)) *ppv = &f->icpc;
  else if (IsEqualIID(riid, &IID_IPersistPropertyBag)) *ppv = &f->ipbag;
  if (!*ppv) return E_NOINTERFACE;
  IFont_AddRef(iface);
  return S_OK;
}
static ULONG WINAPI so_font_AddRef(IFont *iface) { return (ULONG)__sync_add_and_fetch(&so_font_from(iface)->ref, 1); }
static ULONG WINAPI so_font_Release(IFont *iface) {
  so_font *f = so_font_from(iface);
  LONG ref = __sync_sub_and_fetch(&f->ref, 1);
  if (ref == 0) so_font_destroy(f);
  return (ULONG)ref;
}
static HRESULT WINAPI so_font_get_Name(IFont *iface, BSTR *name) {
  so_font *f = so_font_from(iface);
  if (!name) return E_POINTER;
  *name = SysAllocString(f->desc.lpstrName);
  return *name ? S_OK : E_OUTOFMEMORY;
}
static HRESULT WINAPI so_font_put_Name(IFont *iface, BSTR name) {
  so_font *f = so_font_from(iface);
  if (!name) return CTL_E_INVALIDPROPERTYVALUE;
  WCHAR *copy = so_wcsdup(name);
  if (!copy) return E_OUTOFMEMORY;
  oa_free(f->desc.lpstrName);
  f->desc.lpstrName = copy;
  f->desc.sCharset = DEFAULT_CHARSET;       /* a new name is mapped with whatever charset it has */
  f->dirty = TRUE;
  so_font_notify(f, DISPID_FONT_NAME, L"Name");
  return S_OK;
}
static HRESULT WINAPI so_font_get_Size(IFont *iface, CY *size) {
  if (!size) return E_POINTER;
  *size = so_font_from(iface)->desc.cySize;
  return S_OK;
}
static HRESULT WINAPI so_font_put_Size(IFont *iface, CY size) {
  so_font *f = so_font_from(iface);
  f->desc.cySize = size;
  f->dirty = TRUE;
  so_font_notify(f, DISPID_FONT_SIZE, L"Size");
  return S_OK;
}
static HRESULT WINAPI so_font_get_Bold(IFont *iface, BOOL *value) {
  if (!value) return E_POINTER;
  *value = so_font_from(iface)->desc.sWeight > FW_MEDIUM;
  return S_OK;
}
static HRESULT WINAPI so_font_put_Bold(IFont *iface, BOOL value) {
  so_font *f = so_font_from(iface);
  f->desc.sWeight = value ? FW_BOLD : FW_NORMAL;
  f->dirty = TRUE;
  so_font_notify(f, DISPID_FONT_BOLD, L"Bold");
  return S_OK;
}
static HRESULT WINAPI so_font_get_Italic(IFont *iface, BOOL *value) {
  if (!value) return E_POINTER;
  *value = so_font_from(iface)->desc.fItalic;
  return S_OK;
}
static HRESULT WINAPI so_font_put_Italic(IFont *iface, BOOL value) {
  so_font *f = so_font_from(iface);
  f->desc.fItalic = value;
  f->dirty = TRUE;
  so_font_notify(f, DISPID_FONT_ITALIC, L"Italic");
  return S_OK;
}
static HRESULT WINAPI so_font_get_Underline(IFont *iface, BOOL *value) {
  if (!value) return E_POINTER;
  *value = so_font_from(iface)->desc.fUnderline;
  return S_OK;
}
static HRESULT WINAPI so_font_put_Underline(IFont *iface, BOOL value) {
  so_font *f = so_font_from(iface);
  f->desc.fUnderline = value;
  f->dirty = TRUE;
  so_font_notify(f, DISPID_FONT_UNDER, L"Underline");
  return S_OK;
}
static HRESULT WINAPI so_font_get_Strikethrough(IFont *iface, BOOL *value) {
  if (!value) return E_POINTER;
  *value = so_font_from(iface)->desc.fStrikethrough;
  return S_OK;
}
static HRESULT WINAPI so_font_put_Strikethrough(IFont *iface, BOOL value) {
  so_font *f = so_font_from(iface);
  f->desc.fStrikethrough = value;
  f->dirty = TRUE;
  so_font_notify(f, DISPID_FONT_STRIKE, L"Strikethrough");
  return S_OK;
}
static HRESULT WINAPI so_font_get_Weight(IFont *iface, SHORT *value) {
  if (!value) return E_POINTER;
  *value = so_font_from(iface)->desc.sWeight;
  return S_OK;
}
static HRESULT WINAPI so_font_put_Weight(IFont *iface, SHORT value) {
  so_font *f = so_font_from(iface);
  f->desc.sWeight = value;
  f->dirty = TRUE;
  so_font_notify(f, DISPID_FONT_WEIGHT, L"Weight");
  return S_OK;
}
static HRESULT WINAPI so_font_get_Charset(IFont *iface, SHORT *value) {
  so_font *f = so_font_from(iface);
  if (!value) return E_POINTER;
  if (f->dirty) so_font_realize(f);          /* (the charset reported is the one of the font that was really chosen) */
  *value = f->desc.sCharset;
  return S_OK;
}
static HRESULT WINAPI so_font_put_Charset(IFont *iface, SHORT value) {
  so_font *f = so_font_from(iface);
  f->desc.sCharset = value;
  f->dirty = TRUE;
  so_font_notify(f, DISPID_FONT_CHARSET, L"Charset");
  return S_OK;
}
static HRESULT WINAPI so_font_get_hFont(IFont *iface, HFONT *hfont) {
  if (!hfont) return E_POINTER;
  *hfont = so_font_realize(so_font_from(iface));
  return S_OK;
}
static HRESULT WINAPI so_font_Clone(IFont *iface, IFont **out) {
  so_font *f = so_font_from(iface);
  if (!out) return E_POINTER;
  *out = NULL;
  so_font *copy = so_font_new(&f->desc);
  if (!copy) return E_OUTOFMEMORY;
  copy->cy_logical = f->cy_logical;
  copy->cy_himetric = f->cy_himetric;
  *out = &copy->ifont;
  return S_OK;
}
static HRESULT WINAPI so_font_IsEqual(IFont *iface, IFont *other) {
  so_font *f = so_font_from(iface);
  BSTR name = NULL;
  CY size;
  BOOL italic, underline, strike;
  SHORT weight, charset;
  if (!other) return E_POINTER;
  if (FAILED(IFont_get_Name(other, &name))) return S_FALSE;
  BOOL same = name && so_wcsicmp(name, f->desc.lpstrName) == 0;
  SysFreeString(name);
  if (!same) return S_FALSE;
  if (FAILED(IFont_get_Size(other, &size)) || size.int64 != f->desc.cySize.int64) return S_FALSE;
  if (FAILED(IFont_get_Weight(other, &weight)) || weight != f->desc.sWeight) return S_FALSE;
  if (FAILED(IFont_get_Italic(other, &italic)) || !italic != !f->desc.fItalic) return S_FALSE;
  if (FAILED(IFont_get_Underline(other, &underline)) || !underline != !f->desc.fUnderline) return S_FALSE;
  if (FAILED(IFont_get_Strikethrough(other, &strike)) || !strike != !f->desc.fStrikethrough) return S_FALSE;
  /* (the stored charsets, not the realized ones) */
  so_font *o = other->lpVtbl == (const IFontVtbl *)&so_font_vtbl ? so_font_from(other) : NULL;
  if (o) charset = o->desc.sCharset;
  else if (FAILED(IFont_get_Charset(other, &charset))) return S_FALSE;
  return charset == f->desc.sCharset ? S_OK : S_FALSE;
}
static HRESULT WINAPI so_font_SetRatio(IFont *iface, LONG logical, LONG himetric) {
  so_font *f = so_font_from(iface);
  if (logical == 0 || himetric == 0) return E_FAIL;
  f->cy_logical = logical;
  f->cy_himetric = himetric;
  f->dirty = TRUE;
  return S_OK;
}
static HRESULT WINAPI so_font_QueryTextMetrics(IFont *iface, TEXTMETRICOLE *metrics) {
  so_font *f = so_font_from(iface);
  if (!metrics) return E_POINTER;
  memset(metrics, 0, sizeof(*metrics));
  HFONT hfont = so_font_realize(f);
  HDC dc = so_CreateCompatibleDC(NULL);
  if (!dc) return E_FAIL;
  HGDIOBJ old = hfont ? so_SelectObject(dc, hfont) : NULL;
  BOOL ok = so_GetTextMetricsW(dc, metrics);
  if (old) so_SelectObject(dc, old);
  so_DeleteDC(dc);
  return ok ? S_OK : E_FAIL;
}
static HRESULT WINAPI so_font_AddRefHfont(IFont *iface, HFONT hfont) {
  (void)iface;
  if (!hfont) return E_INVALIDARG;
  return so_hfont_ext_add(hfont);
}
static HRESULT WINAPI so_font_ReleaseHfont(IFont *iface, HFONT hfont) {
  (void)iface;
  if (!hfont) return E_INVALIDARG;
  return so_hfont_ext_release(hfont);
}
static HRESULT WINAPI so_font_SetHdc(IFont *iface, HDC dc) { (void)iface; (void)dc; return S_OK; }

static const IFontVtbl so_font_vtbl = {
  so_font_QueryInterface, so_font_AddRef, so_font_Release, so_font_get_Name, so_font_put_Name, so_font_get_Size, so_font_put_Size, so_font_get_Bold, so_font_put_Bold,
  so_font_get_Italic, so_font_put_Italic, so_font_get_Underline, so_font_put_Underline, so_font_get_Strikethrough, so_font_put_Strikethrough, so_font_get_Weight,
  so_font_put_Weight, so_font_get_Charset, so_font_put_Charset, so_font_get_hFont, so_font_Clone, so_font_IsEqual, so_font_SetRatio, so_font_QueryTextMetrics,
  so_font_AddRefHfont, so_font_ReleaseHfont, so_font_SetHdc
};

/* ---- IFontDisp: the properties by dispatch id, with its own type information ---------------------------------------------------------------------------------- */
typedef struct { DISPID id; const WCHAR *name; VARTYPE vt; BOOL writable; } so_font_prop;
static const so_font_prop so_font_props[] = {
  {DISPID_FONT_NAME, L"Name", VT_BSTR, TRUE}, {DISPID_FONT_SIZE, L"Size", VT_CY, TRUE}, {DISPID_FONT_BOLD, L"Bold", VT_BOOL, TRUE}, {DISPID_FONT_ITALIC, L"Italic", VT_BOOL, TRUE},
  {DISPID_FONT_UNDER, L"Underline", VT_BOOL, TRUE}, {DISPID_FONT_STRIKE, L"Strikethrough", VT_BOOL, TRUE}, {DISPID_FONT_WEIGHT, L"Weight", VT_I2, TRUE}, {DISPID_FONT_CHARSET, L"Charset", VT_I2, TRUE}
};

static HRESULT WINAPI so_font_disp_QueryInterface(IDispatch *iface, REFIID riid, void **ppv) { return IFont_QueryInterface(&so_font_from_disp(iface)->ifont, riid, ppv); }
static ULONG WINAPI so_font_disp_AddRef(IDispatch *iface) { return IFont_AddRef(&so_font_from_disp(iface)->ifont); }
static ULONG WINAPI so_font_disp_Release(IDispatch *iface) { return IFont_Release(&so_font_from_disp(iface)->ifont); }
static HRESULT WINAPI so_font_disp_GetTypeInfoCount(IDispatch *iface, UINT *count) {
  (void)iface;
  if (!count) return E_POINTER;
  *count = 1;
  return S_OK;
}

/* The type information is built from a table (CreateDispTypeInfo): a getter and a setter for each property, in the order of the table. */
static HRESULT so_build_typeinfo(const so_font_prop *props, UINT n, LCID lcid, ITypeInfo **out) {
  static const WCHAR value_name[] = {'V','a','l','u','e',0};
  METHODDATA *methods = oa_calloc(sizeof(METHODDATA) * n * 2);
  PARAMDATA *params = oa_calloc(sizeof(PARAMDATA) * n);
  if (!methods || !params) { oa_free(methods); oa_free(params); return E_OUTOFMEMORY; }
  UINT count = 0;
  for (UINT i = 0; i < n; i++) {
    params[i].szName = (OLECHAR *)value_name;
    params[i].vt = props[i].vt;
    methods[count].szName = (OLECHAR *)props[i].name;
    methods[count].ppdata = NULL;
    methods[count].dispid = props[i].id;
    methods[count].iMeth = count;
    methods[count].cc = CC_STDCALL;
    methods[count].cArgs = 0;
    methods[count].wFlags = DISPATCH_PROPERTYGET;
    methods[count].vtReturn = props[i].vt;
    count++;
  }
  for (UINT i = 0; i < n; i++) {
    if (!props[i].writable) continue;
    methods[count] = methods[i];
    methods[count].iMeth = count;
    methods[count].ppdata = &params[i];
    methods[count].cArgs = 1;
    methods[count].wFlags = DISPATCH_PROPERTYPUT;
    methods[count].vtReturn = VT_EMPTY;
    count++;
  }
  INTERFACEDATA data;
  data.pmethdata = methods;
  data.cMembers = count;
  *out = NULL;
  HRESULT hr = CreateDispTypeInfo(&data, lcid, out);
  oa_free(methods);
  oa_free(params);
  if (SUCCEEDED(hr) && !*out) hr = E_NOTIMPL;
  return hr;
}
static HRESULT WINAPI so_font_disp_GetTypeInfo(IDispatch *iface, UINT index, LCID lcid, ITypeInfo **out) {
  so_font *f = so_font_from_disp(iface);
  if (!out) return E_POINTER;
  *out = NULL;
  if (index != 0) return DISP_E_BADINDEX;
  if (!f->typeinfo) {
    ITypeInfo *info;
    HRESULT hr = so_build_typeinfo(so_font_props, OA_ARRAY_SIZE(so_font_props), lcid, &info);
    if (FAILED(hr)) return hr;
    if (__sync_val_compare_and_swap(&f->typeinfo, NULL, info) != NULL) ITypeInfo_Release(info);
  }
  *out = f->typeinfo;
  ITypeInfo_AddRef(*out);
  return S_OK;
}
static HRESULT WINAPI so_font_disp_GetIDsOfNames(IDispatch *iface, REFIID riid, LPOLESTR *names, UINT count, LCID lcid, DISPID *ids) {
  (void)iface; (void)lcid;
  if (!IsEqualIID(riid, &so_null_iid)) return DISP_E_UNKNOWNINTERFACE;
  if (!names || !ids || count == 0) return E_INVALIDARG;
  HRESULT hr = DISP_E_UNKNOWNNAME;
  ids[0] = DISPID_UNKNOWN;
  for (UINT i = 0; i < OA_ARRAY_SIZE(so_font_props); i++) {
    if (so_wcsicmp(names[0], so_font_props[i].name) != 0) continue;
    ids[0] = so_font_props[i].id;
    hr = S_OK;
    break;
  }
  for (UINT i = 1; i < count; i++) ids[i] = DISPID_UNKNOWN;       /* (parameter names are not known to this object) */
  return hr;
}

/* A CY from the usual number types, points being the unit of a plain number. */
static HRESULT so_variant_to_cy(const VARIANT *in, CY *out) {
  VARIANT v;
  BOOL owned;
  HRESULT hr;
  switch (V_VT(in)) {
  case VT_CY: *out = V_CY(in); return S_OK;
  case VT_I2: out->int64 = (LONGLONG)V_I2(in) * 10000; return S_OK;
  case VT_I4: out->int64 = (LONGLONG)V_I4(in) * 10000; return S_OK;
  case VT_R4: out->int64 = (LONGLONG)(V_R4(in) * 10000.0f + (V_R4(in) < 0 ? -0.5f : 0.5f)); return S_OK;
  case VT_R8: out->int64 = (LONGLONG)(V_R8(in) * 10000.0 + (V_R8(in) < 0 ? -0.5 : 0.5)); return S_OK;
  default: break;
  }
  hr = so_variant_as(&v, in, VT_CY, &owned);
  if (FAILED(hr)) return hr;
  *out = V_CY(&v);
  if (owned) VariantClear(&v);
  return S_OK;
}

static HRESULT WINAPI so_font_disp_Invoke(IDispatch *iface, DISPID dispid, REFIID riid, LCID lcid, WORD flags, DISPPARAMS *params, VARIANT *result, EXCEPINFO *excep, UINT *arg_err) {
  so_font *f = so_font_from_disp(iface);
  IFont *font = &f->ifont;
  const so_font_prop *prop = NULL;
  VARIANT v;
  BOOL owned = FALSE;
  HRESULT hr;
  (void)lcid; (void)excep; (void)arg_err;
  if (!IsEqualIID(riid, &so_null_iid)) return DISP_E_UNKNOWNINTERFACE;
  for (UINT i = 0; i < OA_ARRAY_SIZE(so_font_props); i++) if (so_font_props[i].id == dispid) prop = &so_font_props[i];
  if (!prop) return DISP_E_MEMBERNOTFOUND;
  if (flags & DISPATCH_PROPERTYGET) {
    if (!result) return DISP_E_PARAMNOTOPTIONAL;
    VariantInit(result);
    V_VT(result) = prop->vt;
    switch (dispid) {
    case DISPID_FONT_NAME: hr = IFont_get_Name(font, &V_BSTR(result)); break;
    case DISPID_FONT_SIZE: hr = IFont_get_Size(font, &V_CY(result)); break;
    case DISPID_FONT_WEIGHT: hr = IFont_get_Weight(font, &V_I2(result)); break;
    case DISPID_FONT_CHARSET: hr = IFont_get_Charset(font, &V_I2(result)); break;
    default: {
      BOOL value = FALSE;
      hr = dispid == DISPID_FONT_BOLD ? IFont_get_Bold(font, &value) : dispid == DISPID_FONT_ITALIC ? IFont_get_Italic(font, &value)
         : dispid == DISPID_FONT_UNDER ? IFont_get_Underline(font, &value) : IFont_get_Strikethrough(font, &value);
      V_BOOL(result) = value ? VARIANT_TRUE : VARIANT_FALSE;
      break;
    }
    }
    if (FAILED(hr)) V_VT(result) = VT_EMPTY;
    return hr;
  }
  if (flags & DISPATCH_PROPERTYPUT) {
    if (!params) return DISP_E_PARAMNOTOPTIONAL;
    if (params->cArgs != 1) return DISP_E_BADPARAMCOUNT;
    const VARIANT *arg = &params->rgvarg[0];
    switch (dispid) {
    case DISPID_FONT_NAME:
      hr = so_variant_as(&v, arg, VT_BSTR, &owned);
      if (SUCCEEDED(hr)) hr = IFont_put_Name(font, V_BSTR(&v));
      break;
    case DISPID_FONT_SIZE: {
      CY size;
      hr = so_variant_to_cy(arg, &size);
      if (SUCCEEDED(hr)) hr = IFont_put_Size(font, size);
      break;
    }
    case DISPID_FONT_WEIGHT:
    case DISPID_FONT_CHARSET:
      hr = so_variant_as(&v, arg, VT_I2, &owned);
      if (SUCCEEDED(hr)) hr = dispid == DISPID_FONT_WEIGHT ? IFont_put_Weight(font, V_I2(&v)) : IFont_put_Charset(font, V_I2(&v));
      break;
    default:
      hr = so_variant_as(&v, arg, VT_BOOL, &owned);
      if (SUCCEEDED(hr)) {
        BOOL value = V_BOOL(&v) != VARIANT_FALSE;
        hr = dispid == DISPID_FONT_BOLD ? IFont_put_Bold(font, value) : dispid == DISPID_FONT_ITALIC ? IFont_put_Italic(font, value)
           : dispid == DISPID_FONT_UNDER ? IFont_put_Underline(font, value) : IFont_put_Strikethrough(font, value);
      }
      break;
    }
    if (owned) VariantClear(&v);
    return hr;
  }
  return DISP_E_MEMBERNOTFOUND;
}
static const IDispatchVtbl so_font_disp_vtbl = {
  so_font_disp_QueryInterface, so_font_disp_AddRef, so_font_disp_Release, so_font_disp_GetTypeInfoCount, so_font_disp_GetTypeInfo, so_font_disp_GetIDsOfNames, so_font_disp_Invoke
};

/* ---- IPersistStream: version 1, charset, attributes, weight, size (CY.Lo), a counted ANSI name ---------------------------------------------------------------- */
#define SO_FONT_PERSIST_ITALIC 0x02
#define SO_FONT_PERSIST_UNDERLINE 0x04
#define SO_FONT_PERSIST_STRIKE 0x08

static HRESULT WINAPI so_font_ps_QueryInterface(IPersistStream *iface, REFIID riid, void **ppv) { return IFont_QueryInterface(&so_font_from_persist(iface)->ifont, riid, ppv); }
static ULONG WINAPI so_font_ps_AddRef(IPersistStream *iface) { return IFont_AddRef(&so_font_from_persist(iface)->ifont); }
static ULONG WINAPI so_font_ps_Release(IPersistStream *iface) { return IFont_Release(&so_font_from_persist(iface)->ifont); }
static HRESULT WINAPI so_font_ps_GetClassID(IPersistStream *iface, CLSID *clsid) {
  (void)iface;
  if (!clsid) return E_POINTER;
  *clsid = so_CLSID_StdFont;
  return S_OK;
}
static HRESULT WINAPI so_font_ps_IsDirty(IPersistStream *iface) { (void)iface; return S_OK; }
static HRESULT WINAPI so_font_ps_Load(IPersistStream *iface, IStream *stream) {
  so_font *f = so_font_from_persist(iface);
  BYTE version, charset, attributes, name_length;
  WORD weight;
  DWORD size;
  char ansi[256];
  HRESULT hr;
  if (!stream) return E_POINTER;
  if (FAILED(hr = so_read_exact(stream, &version, 1))) return hr;
  if (version != 0x01) return E_FAIL;
  if (FAILED(hr = so_read_exact(stream, &charset, 1)) || FAILED(hr = so_read_exact(stream, &attributes, 1)) || FAILED(hr = so_read_exact(stream, &weight, 2))
      || FAILED(hr = so_read_exact(stream, &size, 4)) || FAILED(hr = so_read_exact(stream, &name_length, 1))) return hr;
  if (name_length && FAILED(hr = so_read_exact(stream, ansi, name_length))) return hr;
  WCHAR *name = oa_alloc(((SIZE_T)name_length + 1) * sizeof(WCHAR));
  if (!name) return E_OUTOFMEMORY;
  int n = name_length ? MultiByteToWideChar(CP_ACP, 0, ansi, name_length, name, name_length) : 0;
  name[n > 0 ? n : 0] = 0;
  oa_free(f->desc.lpstrName);
  f->desc.lpstrName = name;
  f->desc.sCharset = charset;
  f->desc.sWeight = (SHORT)weight;
  f->desc.fItalic = (attributes & SO_FONT_PERSIST_ITALIC) != 0;
  f->desc.fUnderline = (attributes & SO_FONT_PERSIST_UNDERLINE) != 0;
  f->desc.fStrikethrough = (attributes & SO_FONT_PERSIST_STRIKE) != 0;
  f->desc.cySize.Lo = size;
  f->desc.cySize.Hi = 0;
  f->dirty = TRUE;
  return S_OK;
}
static HRESULT WINAPI so_font_ps_Save(IPersistStream *iface, IStream *stream, BOOL clear_dirty) {
  so_font *f = so_font_from_persist(iface);
  BYTE header[10];
  char ansi[256];
  (void)clear_dirty;
  if (!stream) return E_POINTER;
  int n = WideCharToMultiByte(CP_ACP, 0, f->desc.lpstrName, -1, ansi, sizeof(ansi), NULL, NULL);
  n = n > 0 ? n - 1 : 0;                      /* (without the terminator; a name too long for the count byte is cut) */
  if (n > 255) n = 255;
  header[0] = 0x01;
  header[1] = (BYTE)f->desc.sCharset;
  header[2] = (BYTE)((f->desc.fItalic ? SO_FONT_PERSIST_ITALIC : 0) | (f->desc.fUnderline ? SO_FONT_PERSIST_UNDERLINE : 0) | (f->desc.fStrikethrough ? SO_FONT_PERSIST_STRIKE : 0));
  header[3] = (BYTE)f->desc.sWeight; header[4] = (BYTE)((WORD)f->desc.sWeight >> 8);
  header[5] = (BYTE)f->desc.cySize.Lo; header[6] = (BYTE)(f->desc.cySize.Lo >> 8); header[7] = (BYTE)(f->desc.cySize.Lo >> 16); header[8] = (BYTE)(f->desc.cySize.Lo >> 24);
  header[9] = (BYTE)n;
  HRESULT hr = so_write_all(stream, header, sizeof(header));
  if (SUCCEEDED(hr) && n) hr = so_write_all(stream, ansi, (ULONG)n);
  return hr;
}
static HRESULT WINAPI so_font_ps_GetSizeMax(IPersistStream *iface, ULARGE_INTEGER *size) {
  so_font *f = so_font_from_persist(iface);
  if (!size) return E_INVALIDARG;
  int n = WideCharToMultiByte(CP_ACP, 0, f->desc.lpstrName, -1, NULL, 0, NULL, NULL);
  n = n > 0 ? n - 1 : 0;
  if (n > 255) n = 255;
  size->QuadPart = 10 + (ULONGLONG)n;
  return S_OK;
}
static const IPersistStreamVtbl so_font_persist_vtbl = {
  so_font_ps_QueryInterface, so_font_ps_AddRef, so_font_ps_Release, so_font_ps_GetClassID, so_font_ps_IsDirty, so_font_ps_Load, so_font_ps_Save, so_font_ps_GetSizeMax
};

/* ---- IConnectionPointContainer ------------------------------------------------------------------------------------------------------------------------------- */
static HRESULT WINAPI so_font_cpc_QueryInterface(IConnectionPointContainer *iface, REFIID riid, void **ppv) { return IFont_QueryInterface(&so_font_from_cpc(iface)->ifont, riid, ppv); }
static ULONG WINAPI so_font_cpc_AddRef(IConnectionPointContainer *iface) { return IFont_AddRef(&so_font_from_cpc(iface)->ifont); }
static ULONG WINAPI so_font_cpc_Release(IConnectionPointContainer *iface) { return IFont_Release(&so_font_from_cpc(iface)->ifont); }
static HRESULT WINAPI so_font_cpc_EnumConnectionPoints(IConnectionPointContainer *iface, IEnumConnectionPoints **out) {
  (void)iface;
  if (!out) return E_POINTER;
  *out = NULL;
  return E_NOTIMPL;
}
static HRESULT WINAPI so_font_cpc_FindConnectionPoint(IConnectionPointContainer *iface, REFIID riid, IConnectionPoint **out) {
  so_font *f = so_font_from_cpc(iface);
  so_cp *points[2] = {&f->cp_notify, &f->cp_events};
  return so_cpc_find(points, 2, riid, out);
}
static const IConnectionPointContainerVtbl so_font_cpc_vtbl = {
  so_font_cpc_QueryInterface, so_font_cpc_AddRef, so_font_cpc_Release, so_font_cpc_EnumConnectionPoints, so_font_cpc_FindConnectionPoint
};

/* ---- IPersistPropertyBag --------------------------------------------------------------------------------------------------------------------------------------- */
static HRESULT WINAPI so_font_pb_QueryInterface(IPersistPropertyBag *iface, REFIID riid, void **ppv) { return IFont_QueryInterface(&so_font_from_pbag(iface)->ifont, riid, ppv); }
static ULONG WINAPI so_font_pb_AddRef(IPersistPropertyBag *iface) { return IFont_AddRef(&so_font_from_pbag(iface)->ifont); }
static ULONG WINAPI so_font_pb_Release(IPersistPropertyBag *iface) { return IFont_Release(&so_font_from_pbag(iface)->ifont); }
static HRESULT WINAPI so_font_pb_GetClassID(IPersistPropertyBag *iface, CLSID *clsid) {
  (void)iface;
  if (!clsid) return E_POINTER;
  *clsid = so_CLSID_StdFont;
  return S_OK;
}
static HRESULT WINAPI so_font_pb_InitNew(IPersistPropertyBag *iface) { (void)iface; return S_OK; }
static HRESULT WINAPI so_font_pb_Load(IPersistPropertyBag *iface, IPropertyBag *bag, IErrorLog *log) {
  so_font *f = so_font_from_pbag(iface);
  VARIANT v;
  if (!bag) return E_POINTER;
  for (UINT i = 0; i < OA_ARRAY_SIZE(so_font_props); i++) {
    const so_font_prop *prop = &so_font_props[i];
    VariantInit(&v);
    V_VT(&v) = prop->vt == VT_CY ? VT_R8 : prop->vt;         /* (a size is written as a number of points) */
    if (IPropertyBag_Read(bag, prop->name, &v, log) != S_OK) { VariantClear(&v); continue; }
    switch (prop->id) {
    case DISPID_FONT_NAME:
      if (V_VT(&v) == VT_BSTR && V_BSTR(&v)) { WCHAR *copy = so_wcsdup(V_BSTR(&v)); if (copy) { oa_free(f->desc.lpstrName); f->desc.lpstrName = copy; } }
      break;
    case DISPID_FONT_SIZE: {
      CY size;
      if (SUCCEEDED(so_variant_to_cy(&v, &size))) f->desc.cySize = size;
      break;
    }
    case DISPID_FONT_WEIGHT: if (V_VT(&v) == VT_I2) f->desc.sWeight = V_I2(&v); break;
    case DISPID_FONT_CHARSET: if (V_VT(&v) == VT_I2) f->desc.sCharset = V_I2(&v); break;
    default: {
      BOOL value = V_VT(&v) == VT_BOOL && V_BOOL(&v) != VARIANT_FALSE;
      if (prop->id == DISPID_FONT_BOLD) f->desc.sWeight = value ? FW_BOLD : FW_NORMAL;
      else if (prop->id == DISPID_FONT_ITALIC) f->desc.fItalic = value;
      else if (prop->id == DISPID_FONT_UNDER) f->desc.fUnderline = value;
      else f->desc.fStrikethrough = value;
      break;
    }
    }
    VariantClear(&v);
  }
  f->dirty = TRUE;
  return S_OK;
}
static HRESULT WINAPI so_font_pb_Save(IPersistPropertyBag *iface, IPropertyBag *bag, BOOL clear_dirty, BOOL save_all) {
  so_font *f = so_font_from_pbag(iface);
  HRESULT hr = S_OK;
  (void)clear_dirty; (void)save_all;
  if (!bag) return E_POINTER;
  for (UINT i = 0; i < OA_ARRAY_SIZE(so_font_props) && SUCCEEDED(hr); i++) {
    const so_font_prop *prop = &so_font_props[i];
    VARIANT v;
    VariantInit(&v);
    switch (prop->id) {
    case DISPID_FONT_NAME: V_VT(&v) = VT_BSTR; V_BSTR(&v) = SysAllocString(f->desc.lpstrName); break;
    case DISPID_FONT_SIZE: V_VT(&v) = VT_R8; V_R8(&v) = (double)f->desc.cySize.int64 / 10000.0; break;
    case DISPID_FONT_WEIGHT: V_VT(&v) = VT_I2; V_I2(&v) = f->desc.sWeight; break;
    case DISPID_FONT_CHARSET: V_VT(&v) = VT_I2; V_I2(&v) = f->desc.sCharset; break;
    default: {
      BOOL value = prop->id == DISPID_FONT_BOLD ? f->desc.sWeight > FW_MEDIUM : prop->id == DISPID_FONT_ITALIC ? f->desc.fItalic
                 : prop->id == DISPID_FONT_UNDER ? f->desc.fUnderline : f->desc.fStrikethrough;
      V_VT(&v) = VT_BOOL; V_BOOL(&v) = value ? VARIANT_TRUE : VARIANT_FALSE;
      break;
    }
    }
    hr = IPropertyBag_Write(bag, prop->name, &v);
    VariantClear(&v);
  }
  return hr;
}
static const IPersistPropertyBagVtbl so_font_pbag_vtbl = {
  so_font_pb_QueryInterface, so_font_pb_AddRef, so_font_pb_Release, so_font_pb_GetClassID, so_font_pb_InitNew, so_font_pb_Load, so_font_pb_Save
};

/* ---- the exports ---------------------------------------------------------------------------------------------------------------------------------------------- */
static HRESULT so_font_create(const FONTDESC *desc, REFIID riid, void **ppv) {
  if (!ppv) return E_POINTER;
  *ppv = NULL;
  if (desc && !desc->lpstrName) return CTL_E_INVALIDPROPERTYVALUE;
  so_font *f = so_font_new(desc);
  if (!f) return E_OUTOFMEMORY;
  HRESULT hr = IFont_QueryInterface(&f->ifont, riid, ppv);
  IFont_Release(&f->ifont);
  return hr;
}

EXPORT HRESULT WINAPI OleCreateFontIndirect(LPFONTDESC desc, REFIID riid, LPVOID *ppv) { return so_font_create(desc, riid, ppv); }
