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

/* ITypeLib2 and ITypeInfo2 over the model (typelib.inc): everything a reader gets from a loaded or created library. */

static const GUID TL_IID_STDOLE = { 0x00020430, 0x0000, 0x0000, { 0xC0, 0, 0, 0, 0, 0, 0, 0x46 } };


static HRESULT tl_lib_ref_info(TlLib *lib, HREFTYPE href, ITypeInfo **out);

/* ---- sizes of types ------------------------------------------------------------------------------------------------------------------------------- */
/* Size and alignment of a type description for a library of pointer size `ptr`; `natural` gives the alignment the compiler would (a loaded library of the other
 * kind of system), otherwise it is capped at 4 as the library writer does. */
static HRESULT tl_td_size(TlLib *lib, const TYPEDESC *td, UINT ptr, BOOL natural, ULONG *size, WORD *align, UINT depth) {
  ULONG sz;
  if (depth > 16) return E_FAIL;
  switch (td->vt) {
    case VT_I1: case VT_UI1: sz = 1; break;
    case VT_I2: case VT_UI2: case VT_BOOL: sz = 2; break;
    case VT_I4: case VT_UI4: case VT_R4: case VT_ERROR: case VT_INT: case VT_UINT: case VT_HRESULT: sz = 4; break;
    case VT_I8: case VT_UI8: case VT_R8: case VT_CY: case VT_DATE: sz = 8; break;
    case VT_DECIMAL: sz = 16; break;
    case VT_VARIANT: sz = ptr == 8 ? 24 : 16; break;
    case VT_VOID: sz = 0; break;
    case VT_CARRAY: {
      ULONG es = 0, n = 1;
      WORD ea = 0;
      if (!td->lpadesc) return E_FAIL;
      HRESULT hr = tl_td_size(lib, &td->lpadesc->tdescElem, ptr, natural, &es, &ea, depth + 1);
      if (FAILED(hr)) return hr;
      for (UINT i = 0; i < td->lpadesc->cDims; i++) n *= td->lpadesc->rgbounds[i].cElements;
      *size = es * n;
      *align = ea;
      return S_OK;
    }
    case VT_USERDEFINED: {
      ITypeInfo *ref;
      HRESULT hr = tl_lib_ref_info(lib, td->hreftype, &ref);
      if (FAILED(hr)) return hr;
      TYPEATTR *ta;
      hr = ITypeInfo_GetTypeAttr(ref, &ta);
      if (SUCCEEDED(hr)) {
        if (ta->typekind == TKIND_ALIAS) hr = tl_td_size(lib, &ta->tdescAlias, ptr, natural, size, align, depth + 1);
        else { *size = ta->cbSizeInstance; *align = ta->cbAlignment; }
        ITypeInfo_ReleaseTypeAttr(ref, ta);
      }
      ITypeInfo_Release(ref);
      return hr;
    }
    default: sz = ptr; break;                    /* BSTR, pointers, interfaces, SAFEARRAY, strings */
  }
  *size = sz;
  *align = (WORD)(sz > (natural ? 8u : 4u) ? (natural ? 8u : 4u) : sz);
  if (!natural && td->vt != VT_VOID && sz >= 4) *align = 4;
  return S_OK;
}

/* ---- the standard library: IUnknown and IDispatch for the dispatch half of dual interfaces and for dispinterfaces -------------------------------------------- */
static TlLib *tl_stdole_lib;

static TlFunc *tl_std_func(TlType *t, const WCHAR *name, MEMBERID id, VARTYPE ret, SHORT nparams, short vft) {
  TlFunc *f = tl_type_add_func(t);
  if (!f) return NULL;
  f->name = tl_wcsdup(name);
  f->fd.memid = id;
  f->fd.funckind = FUNC_PUREVIRTUAL;
  f->fd.invkind = INVOKE_FUNC;
  f->fd.callconv = CC_STDCALL;
  f->fd.wFuncFlags = FUNCFLAG_FRESTRICTED;
  f->fd.oVft = vft;
  f->fd.elemdescFunc.tdesc.vt = ret;
  f->fd.cParams = nparams;
  f->fd.lprgelemdescParam = oa_calloc((SIZE_T)(nparams ? nparams : 1) * sizeof(ELEMDESC));
  f->pnames = oa_calloc((SIZE_T)(nparams ? nparams : 1) * sizeof(WCHAR *));
  return f->name && f->fd.lprgelemdescParam && f->pnames ? f : NULL;
}
/* Parameter i of f: vt, with `ptr` levels of pointer in front of it, `user` (an internal HREFTYPE) for VT_USERDEFINED, and flags. */
static BOOL tl_std_param(TlFunc *f, int i, const WCHAR *name, VARTYPE vt, int ptr, HREFTYPE user, USHORT flags) {
  ELEMDESC *e = &f->fd.lprgelemdescParam[i];
  f->pnames[i] = tl_wcsdup(name);
  e->paramdesc.wParamFlags = flags;
  TYPEDESC *td = &e->tdesc;
  for (int k = 0; k < ptr; k++) {
    td->vt = VT_PTR;
    td->lptdesc = oa_calloc(sizeof(TYPEDESC));
    if (!td->lptdesc) return FALSE;
    td = td->lptdesc;
  }
  td->vt = vt;
  if (vt == VT_USERDEFINED) td->hreftype = user;
  return f->pnames[i] != NULL;
}

/* IUnknown at index 3 and IDispatch at index 4 of a library of GUID, DISPPARAMS and EXCEPINFO records (the layout of stdole2.tlb, whose internal HREFTYPEs the
 * parameters of the functions use). */
static BOOL tl_std_record(TlLib *lib, const WCHAR *name, UINT nfields, const WCHAR *const *fields, const VARTYPE *vts, const int *ptrs, UINT size) {
  TlType *t = tl_lib_add_type(lib, TKIND_RECORD);
  if (!t) return FALSE;
  t->name = tl_wcsdup(name);
  t->attr.cbSizeInstance = size;
  t->attr.cbAlignment = 8;
  UINT off = 0;
  for (UINT i = 0; i < nfields; i++) {
    TlVar *v = tl_type_add_var(t);
    if (!v) return FALSE;
    v->name = tl_wcsdup(fields[i]);
    v->vd.memid = 0x40000000 + (MEMBERID)i;
    v->vd.varkind = VAR_PERINSTANCE;
    TYPEDESC *td = &v->vd.elemdescVar.tdesc;
    for (int k = 0; k < ptrs[i]; k++) { td->vt = VT_PTR; td->lptdesc = oa_calloc(sizeof(TYPEDESC)); if (!td->lptdesc) return FALSE; td = td->lptdesc; }
    td->vt = vts[i];
    const UINT sz = ptrs[i] ? 8 : (vts[i] == VT_BSTR ? 8 : (vts[i] == VT_UI2 ? 2 : 4));
    off = (off + sz - 1) / sz * sz;
    v->vd.oInst = off;
    off += sz;
  }
  return TRUE;
}
static ITypeInfo *tl_std_type(UINT index) {
  if (!tl_stdole_lib) {
    TlLib *lib = tl_lib_new();
    if (!lib) return NULL;
    lib->guid = TL_IID_STDOLE;
    lib->name = tl_wcsdup(L"stdole");
    lib->major = 2;
    lib->lcid = 0;
    lib->flags = LIBFLAG_FHASDISKIMAGE;
    lib->ref = 1;                                   /* (the library lives as long as the process) */
    static const WCHAR *const gf[] = { L"Data1", L"Data2", L"Data3", L"Data4" };
    static const VARTYPE gv[] = { VT_UI4, VT_UI2, VT_UI2, VT_UI1 };
    static const int gp[] = { 0, 0, 0, 0 };
    BOOL ok = tl_std_record(lib, L"GUID", 3, gf, gv, gp, 16);
    TlType *g = ok ? lib->types[0] : NULL;
    if (g) {                                         /* Data4 is an array of 8 bytes */
      TlVar *v = tl_type_add_var(g);
      ok = v != NULL;
      if (ok) {
        v->name = tl_wcsdup(L"Data4");
        v->vd.memid = 0x40000003;
        v->vd.oInst = 8;
        v->vd.elemdescVar.tdesc.vt = VT_CARRAY;
        ARRAYDESC *ad = oa_calloc(sizeof(ARRAYDESC));
        ok = ad != NULL;
        if (ad) { ad->tdescElem.vt = VT_UI1; ad->cDims = 1; ad->rgbounds[0].cElements = 8; v->vd.elemdescVar.tdesc.lpadesc = ad; }
      }
      g->attr.cbAlignment = 4;
      g->attr.guid = (GUID){ 0xde77ba65, 0x517c, 0x11d1, { 0xa2, 0xda, 0, 0, 0xf8, 0x77, 0x3c, 0xe9 } };
    }
    static const WCHAR *const df[] = { L"rgvarg", L"rgdispidNamedArgs", L"cArgs", L"cNamedArgs" };
    static const VARTYPE dv[] = { VT_VARIANT, VT_I4, VT_UINT, VT_UINT };
    static const int dp[] = { 1, 1, 0, 0 };
    ok = ok && tl_std_record(lib, L"DISPPARAMS", 4, df, dv, dp, 24);
    if (ok) lib->types[1]->attr.guid = (GUID){ 0xde77ba63, 0x517c, 0x11d1, { 0xa2, 0xda, 0, 0, 0xf8, 0x77, 0x3c, 0xe9 } };
    static const WCHAR *const ef[] = { L"wCode", L"wReserved", L"bstrSource", L"bstrDescription", L"bstrHelpFile", L"dwHelpContext", L"pvReserved", L"pfnDeferredFillIn", L"scode" };
    static const VARTYPE ev[] = { VT_UI2, VT_UI2, VT_BSTR, VT_BSTR, VT_BSTR, VT_UI4, VT_VOID, VT_VOID, VT_I4 };
    static const int ep[] = { 0, 0, 0, 0, 0, 0, 1, 1, 0 };
    ok = ok && tl_std_record(lib, L"EXCEPINFO", 9, ef, ev, ep, 64);
    if (ok) lib->types[2]->attr.guid = (GUID){ 0xde77ba64, 0x517c, 0x11d1, { 0xa2, 0xda, 0, 0, 0xf8, 0x77, 0x3c, 0xe9 } };
    TlType *unk = ok ? tl_lib_add_type(lib, TKIND_INTERFACE) : NULL, *disp = unk ? tl_lib_add_type(lib, TKIND_INTERFACE) : NULL;
    if (disp) {
      unk->name = tl_wcsdup(L"IUnknown");
      unk->attr.guid = IID_IUnknown;
      unk->attr.cbSizeInstance = 8;
      unk->attr.cbAlignment = 8;
      unk->attr.cbSizeVft = 24;
      unk->attr.wTypeFlags = TYPEFLAG_FRESTRICTED;
      TlFunc *f = tl_std_func(unk, L"QueryInterface", 0x60000000, VT_VOID, 2, 0);
      ok = f && tl_std_param(f, 0, L"riid", VT_USERDEFINED, 1, 0, PARAMFLAG_FIN) && tl_std_param(f, 1, L"ppvObj", VT_VOID, 2, 0, PARAMFLAG_FOUT);
      f = ok ? tl_std_func(unk, L"AddRef", 0x60000001, VT_UI4, 0, 8) : NULL;
      ok = f != NULL;
      f = ok ? tl_std_func(unk, L"Release", 0x60000002, VT_UI4, 0, 16) : NULL;
      ok = f != NULL;
      disp->name = tl_wcsdup(L"IDispatch");
      disp->attr.guid = IID_IDispatch;
      disp->attr.cbSizeInstance = 8;
      disp->attr.cbAlignment = 8;
      disp->attr.cbSizeVft = 56;
      disp->attr.wTypeFlags = TYPEFLAG_FRESTRICTED;
      TlImpl *im = ok ? tl_type_add_impl(disp) : NULL;
      ok = ok && im != NULL;
      if (ok) im->href = unk->file_offset;
      f = ok ? tl_std_func(disp, L"GetTypeInfoCount", 0x60010000, VT_VOID, 1, 24) : NULL;
      ok = f && tl_std_param(f, 0, L"pctinfo", VT_UINT, 1, 0, PARAMFLAG_FOUT);
      f = ok ? tl_std_func(disp, L"GetTypeInfo", 0x60010001, VT_VOID, 3, 32) : NULL;
      ok = f && tl_std_param(f, 0, L"itinfo", VT_UINT, 0, 0, PARAMFLAG_FIN) && tl_std_param(f, 1, L"lcid", VT_UI4, 0, 0, PARAMFLAG_FIN) &&
           tl_std_param(f, 2, L"pptinfo", VT_VOID, 2, 0, PARAMFLAG_FOUT);
      f = ok ? tl_std_func(disp, L"GetIDsOfNames", 0x60010002, VT_VOID, 5, 40) : NULL;
      ok = f && tl_std_param(f, 0, L"riid", VT_USERDEFINED, 1, 0, PARAMFLAG_FIN) && tl_std_param(f, 1, L"rgszNames", VT_LPWSTR, 2, 0, PARAMFLAG_FIN) &&
           tl_std_param(f, 2, L"cNames", VT_UINT, 0, 0, PARAMFLAG_FIN) && tl_std_param(f, 3, L"lcid", VT_UI4, 0, 0, PARAMFLAG_FIN) &&
           tl_std_param(f, 4, L"rgdispid", VT_I4, 1, 0, PARAMFLAG_FOUT);
      f = ok ? tl_std_func(disp, L"Invoke", 0x60010003, VT_VOID, 8, 48) : NULL;
      ok = f && tl_std_param(f, 0, L"dispidMember", VT_I4, 0, 0, PARAMFLAG_FIN) && tl_std_param(f, 1, L"riid", VT_USERDEFINED, 1, 0, PARAMFLAG_FIN) &&
           tl_std_param(f, 2, L"lcid", VT_UI4, 0, 0, PARAMFLAG_FIN) && tl_std_param(f, 3, L"wFlags", VT_UI2, 0, 0, PARAMFLAG_FIN) &&
           tl_std_param(f, 4, L"pdispparams", VT_USERDEFINED, 1, TL_TYPEINFO_RECORD, PARAMFLAG_FIN) &&
           tl_std_param(f, 5, L"pvarResult", VT_VARIANT, 1, 0, PARAMFLAG_FOUT) &&
           tl_std_param(f, 6, L"pexcepinfo", VT_USERDEFINED, 1, 2 * TL_TYPEINFO_RECORD, PARAMFLAG_FOUT) && tl_std_param(f, 7, L"puArgErr", VT_UINT, 1, 0, PARAMFLAG_FOUT);
    } else {
      ok = FALSE;
    }
    if (!ok) { tl_lib_free(lib); return NULL; }
    tl_stdole_lib = lib;
  }
  if (index >= tl_stdole_lib->ntypes) return NULL;
  ITypeInfo2_AddRef(&tl_stdole_lib->types[index]->ti.ITypeInfo2_iface);
  return (ITypeInfo *)&tl_stdole_lib->types[index]->ti.ITypeInfo2_iface;
}

/* ---- HREFTYPE resolution --------------------------------------------------------------------------------------------------------------------------------- */
static BOOL tl_cache_find_guid(const GUID *guid, WORD major, WORD minor, ITypeLib **out);
static HRESULT tl_load_import(TlLib *lib, UINT idx, ITypeLib **out) {
  TlImpFile *f = &lib->impfiles[idx];
  if (!f->lib) {
    ITypeLib *loaded = NULL;
    HRESULT hr = tl_cache_find_guid(&f->guid, f->major, f->minor, &loaded) ? S_OK : E_FAIL;       /* (a library that is open) */
    if (FAILED(hr)) hr = LoadRegTypeLib(&f->guid, f->major, f->minor, f->lcid, &loaded);
    if (FAILED(hr) && f->name) {
      /* the file name in the import table: next to the library first (a relative name), then wherever LoadTypeLib looks */
      hr = LoadTypeLibEx(f->name, REGKIND_NONE, &loaded);
    }
    if (FAILED(hr) && IsEqualGUID(&f->guid, &TL_IID_STDOLE)) {
      /* stdole2 itself: the library over the built-in IUnknown and IDispatch */
      { ITypeInfo *probe = tl_std_type(0); if (probe) ITypeInfo_Release(probe); }
      if (tl_stdole_lib) { ITypeLib2_AddRef(&tl_stdole_lib->ITypeLib2_iface); loaded = (ITypeLib *)&tl_stdole_lib->ITypeLib2_iface; hr = S_OK; }
    }
    if (FAILED(hr)) return TYPE_E_CANTLOADLIBRARY;
    f->lib = loaded;
  }
  ITypeLib_AddRef(f->lib);
  *out = f->lib;
  return S_OK;
}

/* The typeinfo of an HREFTYPE as written in `lib`: bit 0 set is an import (the offset into the import table in the upper bits), otherwise the type at offset
 * 100 * index (the bit 1 that interface references carry is ignored). */
static HRESULT tl_lib_ref_info(TlLib *lib, HREFTYPE href, ITypeInfo **out) {
  *out = NULL;
  if (!(href & 1)) {
    if ((INT)href < 0) return TYPE_E_ELEMENTNOTFOUND;
    const UINT index = href / TL_TYPEINFO_RECORD;
    if (index >= lib->ntypes) return TYPE_E_ELEMENTNOTFOUND;
    ITypeInfo2_AddRef(&lib->types[index]->ti.ITypeInfo2_iface);
    *out = (ITypeInfo *)&lib->types[index]->ti.ITypeInfo2_iface;
    return S_OK;
  }
  if ((INT)href > 0) {
    const DWORD off = href & ~3u;
    if (off % TL_IMPINFO_RECORD || off / TL_IMPINFO_RECORD >= lib->nimpinfos) return TYPE_E_ELEMENTNOTFOUND;
    const TlImpInfo *ii = &lib->impinfos[off / TL_IMPINFO_RECORD];
    if (ii->file >= lib->nimpfiles) return TYPE_E_ELEMENTNOTFOUND;
    ITypeLib *other;
    HRESULT hr = tl_load_import(lib, ii->file, &other);
    if (FAILED(hr)) return hr;
    if (ii->by_guid) hr = ITypeLib_GetTypeInfoOfGuid(other, &ii->guid, out);
    else hr = ITypeLib_GetTypeInfo(other, ii->offset, out);
    ITypeLib_Release(other);
    return hr;
  }
  return TYPE_E_ELEMENTNOTFOUND;
}

/* ---- ITypeLib2 ---------------------------------------------------------------------------------------------------------------------------------------- */
static HRESULT WINAPI tl_lib_QueryInterface(ITypeLib2 *iface, REFIID riid, void **out) {
  TlLib *lib = tl_lib_from_lib(iface);
  if (!out) return E_POINTER;
  *out = NULL;
  if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_ITypeLib) || IsEqualIID(riid, &IID_ITypeLib2)) *out = &lib->ITypeLib2_iface;
  else if (IsEqualIID(riid, &IID_ITypeComp)) *out = &lib->ITypeComp_iface;
  else if (lib->created && (IsEqualIID(riid, &IID_ICreateTypeLib) || IsEqualIID(riid, &IID_ICreateTypeLib2))) *out = &lib->ICreateTypeLib2_iface;
  else return E_NOINTERFACE;
  ITypeLib2_AddRef(iface);
  return S_OK;
}
static ULONG WINAPI tl_lib_AddRef(ITypeLib2 *iface) { return (ULONG)InterlockedIncrement(&tl_lib_from_lib(iface)->ref); }
static void tl_cache_remove(TlLib *lib);
static ULONG WINAPI tl_lib_Release(ITypeLib2 *iface) {
  TlLib *lib = tl_lib_from_lib(iface);
  const LONG ref = InterlockedDecrement(&lib->ref);
  if (ref == 0) {
    if (lib->in_cache) tl_cache_remove(lib);
    if (lib->ref == 0) tl_lib_free(lib);
  }
  return (ULONG)ref;
}
static UINT WINAPI tl_lib_GetTypeInfoCount(ITypeLib2 *iface) { return tl_lib_from_lib(iface)->ntypes; }
static HRESULT WINAPI tl_lib_GetTypeInfo(ITypeLib2 *iface, UINT index, ITypeInfo **out) {
  TlLib *lib = tl_lib_from_lib(iface);
  if (!out) return E_INVALIDARG;
  *out = NULL;
  if (index >= lib->ntypes) return TYPE_E_ELEMENTNOTFOUND;
  ITypeInfo2_AddRef(&lib->types[index]->ti.ITypeInfo2_iface);
  *out = (ITypeInfo *)&lib->types[index]->ti.ITypeInfo2_iface;
  return S_OK;
}
static HRESULT WINAPI tl_lib_GetTypeInfoType(ITypeLib2 *iface, UINT index, TYPEKIND *kind) {
  TlLib *lib = tl_lib_from_lib(iface);
  if (!kind) return E_INVALIDARG;
  if (index >= lib->ntypes) return TYPE_E_ELEMENTNOTFOUND;
  *kind = lib->types[index]->attr.typekind;
  return S_OK;
}
static HRESULT WINAPI tl_lib_GetTypeInfoOfGuid(ITypeLib2 *iface, REFGUID guid, ITypeInfo **out) {
  TlLib *lib = tl_lib_from_lib(iface);
  if (!out) return E_INVALIDARG;
  *out = NULL;
  if (!guid) return E_INVALIDARG;
  for (UINT i = 0; i < lib->ntypes; i++)
    if (IsEqualGUID(&lib->types[i]->attr.guid, guid)) {
      ITypeInfo2_AddRef(&lib->types[i]->ti.ITypeInfo2_iface);
      *out = (ITypeInfo *)&lib->types[i]->ti.ITypeInfo2_iface;
      return S_OK;
    }
  return TYPE_E_ELEMENTNOTFOUND;
}
static HRESULT WINAPI tl_lib_GetLibAttr(ITypeLib2 *iface, TLIBATTR **out) {
  TlLib *lib = tl_lib_from_lib(iface);
  if (!out) return E_INVALIDARG;
  TLIBATTR *a = oa_calloc(sizeof(TLIBATTR));
  if (!a) return E_OUTOFMEMORY;
  a->guid = lib->guid;
  a->lcid = lib->lcid;
  a->syskind = lib->syskind;
  a->wMajorVerNum = lib->major;
  a->wMinorVerNum = lib->minor;
  a->wLibFlags = lib->flags;
  *out = a;
  return S_OK;
}
static HRESULT WINAPI tl_lib_GetTypeComp(ITypeLib2 *iface, ITypeComp **out) {
  if (!out) return E_INVALIDARG;
  *out = &tl_lib_from_lib(iface)->ITypeComp_iface;
  ITypeLib2_AddRef(iface);
  return S_OK;
}
static HRESULT WINAPI tl_lib_GetDocumentation(ITypeLib2 *iface, INT index, BSTR *name, BSTR *doc, DWORD *help_context, BSTR *help_file) {
  TlLib *lib = tl_lib_from_lib(iface);
  if (index < -1) return TYPE_E_ELEMENTNOTFOUND;
  if (index >= 0) {
    if ((UINT)index >= lib->ntypes) return TYPE_E_ELEMENTNOTFOUND;
    return ITypeInfo2_GetDocumentation(&lib->types[index]->ti.ITypeInfo2_iface, MEMBERID_NIL, name, doc, help_context, help_file);
  }
  if (name) { *name = tl_bstr(lib->name); if (lib->name && !*name) return E_OUTOFMEMORY; }
  if (doc) { *doc = tl_bstr(lib->doc); if (lib->doc && !*doc) { if (name) { SysFreeString(*name); *name = NULL; } return E_OUTOFMEMORY; } }
  if (help_context) *help_context = lib->help_context;
  if (help_file) { *help_file = tl_bstr(lib->helpfile); }
  return S_OK;
}

/* Does `name` (ignoring case) name a type, a member or a parameter of the library? On a match the case of the library's name is copied over `name` (Windows does). */
static BOOL tl_func_has_name(const TlFunc *f, WCHAR *name, BOOL fix) {
  if (f->name && !tl_wcsicmp(f->name, name)) { if (fix) memcpy(name, f->name, (oa_wcslen(f->name) + 1) * sizeof(WCHAR)); return TRUE; }
  return FALSE;
}
static BOOL tl_type_has_name(const TlType *t, WCHAR *name, BOOL fix) {
  if (t->name && !tl_wcsicmp(t->name, name)) { if (fix) memcpy(name, t->name, (oa_wcslen(t->name) + 1) * sizeof(WCHAR)); return TRUE; }
  for (UINT i = 0; i < t->nfuncs; i++) {
    const TlFunc *f = &t->funcs[i];
    if (tl_func_has_name(f, name, fix)) return TRUE;
    if (f->pnames)
      for (SHORT k = 0; k < f->fd.cParams; k++)
        if (f->pnames[k] && !tl_wcsicmp(f->pnames[k], name)) { if (fix) memcpy(name, f->pnames[k], (oa_wcslen(f->pnames[k]) + 1) * sizeof(WCHAR)); return TRUE; }
  }
  for (UINT i = 0; i < t->nvars; i++)
    if (t->vars[i].name && !tl_wcsicmp(t->vars[i].name, name)) { if (fix) memcpy(name, t->vars[i].name, (oa_wcslen(t->vars[i].name) + 1) * sizeof(WCHAR)); return TRUE; }
  return FALSE;
}
static HRESULT WINAPI tl_lib_IsName(ITypeLib2 *iface, LPOLESTR name, ULONG hash, BOOL *found) {
  TlLib *lib = tl_lib_from_lib(iface);
  (void)hash;
  if (!name || !found) return E_INVALIDARG;
  *found = FALSE;
  if (lib->name && !tl_wcsicmp(lib->name, name)) { *found = TRUE; return S_OK; }   /* (the case is not fixed for the library's own name) */
  for (UINT i = 0; i < lib->ntypes; i++)
    if (tl_type_has_name(lib->types[i], name, TRUE)) { *found = TRUE; return S_OK; }
  return S_OK;
}
static HRESULT WINAPI tl_lib_FindName(ITypeLib2 *iface, LPOLESTR name, ULONG hash, ITypeInfo **infos, MEMBERID *ids, USHORT *count) {
  TlLib *lib = tl_lib_from_lib(iface);
  (void)hash;
  if (!name || !infos || !ids || !count || !*count) return E_INVALIDARG;
  const USHORT max = *count;
  USHORT n = 0;
  for (UINT i = 0; i < lib->ntypes && n < max; i++) {
    const TlType *t = lib->types[i];
    MEMBERID id = MEMBERID_NIL;
    BOOL found = FALSE;
    if (t->name && !tl_wcsicmp(t->name, name)) found = TRUE;
    for (UINT k = 0; !found && k < t->nfuncs; k++) {
      const TlFunc *f = &t->funcs[k];
      if (tl_func_has_name(f, name, FALSE)) { found = TRUE; id = f->fd.memid; break; }
    }
    for (UINT k = 0; !found && k < t->nvars; k++)
      if (t->vars[k].name && !tl_wcsicmp(t->vars[k].name, name)) { found = TRUE; id = t->vars[k].vd.memid; }
    if (!found) continue;
    ITypeInfo2_AddRef(&lib->types[i]->ti.ITypeInfo2_iface);
    infos[n] = (ITypeInfo *)&lib->types[i]->ti.ITypeInfo2_iface;
    if (ids) ids[n] = id;
    n++;
  }
  *count = n;
  return S_OK;
}
static void WINAPI tl_lib_ReleaseTLibAttr(ITypeLib2 *iface, TLIBATTR *attr) { (void)iface; oa_free(attr); }
static HRESULT WINAPI tl_lib_GetCustData(ITypeLib2 *iface, REFGUID guid, VARIANT *out) { return tl_cust_get(&tl_lib_from_lib(iface)->cust, guid, out); }
static HRESULT WINAPI tl_lib_GetLibStatistics(ITypeLib2 *iface, ULONG *unique_names, ULONG *chars) {
  (void)iface;
  if (unique_names) *unique_names = 1;
  if (chars) *chars = 1;
  return E_NOTIMPL;
}
static HRESULT WINAPI tl_lib_GetDocumentation2(ITypeLib2 *iface, INT index, LCID lcid, BSTR *help_string, DWORD *help_string_context, BSTR *help_string_dll) {
  TlLib *lib = tl_lib_from_lib(iface);
  if (index < -1) return E_INVALIDARG;
  if (index >= 0) {
    if ((UINT)index >= lib->ntypes) return TYPE_E_ELEMENTNOTFOUND;
    return ITypeInfo2_GetDocumentation2(&lib->types[index]->ti.ITypeInfo2_iface, MEMBERID_NIL, lcid, help_string, help_string_context, help_string_dll);
  }
  if (help_string) *help_string = tl_bstr(lib->doc);
  if (help_string_context) *help_string_context = lib->help_string_context;
  if (help_string_dll) *help_string_dll = tl_bstr(lib->helpstringdll);
  return S_OK;
}
static HRESULT WINAPI tl_lib_GetAllCustData(ITypeLib2 *iface, CUSTDATA *out) { return tl_cust_all(&tl_lib_from_lib(iface)->cust, out); }

static ITypeLib2Vtbl tl_lib_vtbl = {
  tl_lib_QueryInterface, tl_lib_AddRef, tl_lib_Release, tl_lib_GetTypeInfoCount, tl_lib_GetTypeInfo, tl_lib_GetTypeInfoType, tl_lib_GetTypeInfoOfGuid,
  tl_lib_GetLibAttr, tl_lib_GetTypeComp, tl_lib_GetDocumentation, tl_lib_IsName, tl_lib_FindName, tl_lib_ReleaseTLibAttr, tl_lib_GetCustData,
  tl_lib_GetLibStatistics, tl_lib_GetDocumentation2, tl_lib_GetAllCustData,
};

/* ---- ITypeInfo2: the views --------------------------------------------------------------------------------------------------------------------------- */
/* A TKIND_DISPATCH typeinfo whose file names a base interface shows the functions of the whole chain of bases (the root first, IUnknown) in front of its own, all as
 * dispatch functions; a dual interface also has another half (what GetRefTypeInfo(-2) gives), the plain TKIND_INTERFACE with the functions as they are in the file. */
static TYPEKIND tl_ti_view_kind(const TlTi *ti) {
  const TYPEKIND k = ti->t->attr.typekind;
  if (!ti->alt) return k;
  return k == TKIND_DISPATCH ? TKIND_INTERFACE : k == TKIND_INTERFACE ? TKIND_DISPATCH : k;
}
static BOOL tl_ti_is_dual(const TlTi *ti) {
  const TYPEKIND k = ti->t->attr.typekind;
  return !ti->alt && (k == TKIND_DISPATCH || k == TKIND_INTERFACE) && (ti->t->attr.wTypeFlags & TYPEFLAG_FDUAL);
}
static BOOL tl_ti_is_dual_dispatch(const TlTi *ti) { return tl_ti_is_dual(ti); }
static BOOL tl_ti_is_dispatch_view(const TlTi *ti) { return tl_ti_view_kind(ti) == TKIND_DISPATCH; }
static BOOL tl_ti_inherits(const TlTi *ti) { return tl_ti_is_dispatch_view(ti) && ti->t->nimpls > 0; }

/* The base of a typeinfo (the base interface of its file, or the IDispatch of a dispinterface that names none) or NULL. */
static ITypeInfo *tl_ti_base(const TlTi *ti) {
  const TlType *t = ti->t;
  ITypeInfo *base = NULL;
  if (t->attr.typekind == TKIND_INTERFACE || t->attr.typekind == TKIND_DISPATCH) {
    if (t->nimpls) {
      if ((INT)t->impls[0].href == -1) return t->attr.typekind == TKIND_DISPATCH ? tl_std_type(4) : NULL;
      if (FAILED(tl_lib_ref_info(t->lib, t->impls[0].href, &base))) return NULL;
    } else if (t->attr.typekind == TKIND_DISPATCH && t->attr.cImplTypes) {
      if ((INT)t->lib->dispatch_href == -1 || FAILED(tl_lib_ref_info(t->lib, t->lib->dispatch_href, &base))) return tl_std_type(4);
    }
  }
  return base;
}
/* The interface half of a typeinfo of ours that is a dual dispatch interface; other typeinfos unchanged. */
static ITypeInfo *tl_interface_half(ITypeInfo *ti) {
  if (((ITypeInfo2 *)ti)->lpVtbl == &tl_ti_vtbl) {
    TlTi *own = tl_ti_from_ti((ITypeInfo2 *)ti);
    if (tl_ti_is_dual(own) && own->t->attr.typekind == TKIND_DISPATCH) {
      ITypeInfo_Release(ti);
      ITypeInfo2_AddRef(&own->t->alt_ti.ITypeInfo2_iface);
      return (ITypeInfo *)&own->t->alt_ti.ITypeInfo2_iface;
    }
  }
  return ti;
}

/* The chain of bases of a dispatch typeinfo, the root first. Returns the count; the entries are referenced. */
#define TL_MAX_BASES 16
static UINT tl_ti_chain(const TlTi *ti, ITypeInfo **chain) {
  ITypeInfo *rev[TL_MAX_BASES];
  UINT n = 0;
  ITypeInfo *cur = tl_ti_base(ti);
  while (cur && n < TL_MAX_BASES) {
    cur = tl_interface_half(cur);
    rev[n++] = cur;
    TYPEATTR *ta = NULL;
    ITypeInfo *next = NULL;
    if (SUCCEEDED(ITypeInfo_GetTypeAttr(cur, &ta))) {
      if (ta->cImplTypes && ta->typekind == TKIND_INTERFACE) {
        HREFTYPE h;
        if (SUCCEEDED(ITypeInfo_GetRefTypeOfImplType(cur, 0, &h))) ITypeInfo_GetRefTypeInfo(cur, h, &next);
      }
      ITypeInfo_ReleaseTypeAttr(cur, ta);
    }
    cur = next;
  }
  if (cur) ITypeInfo_Release(cur);
  for (UINT i = 0; i < n; i++) chain[i] = rev[n - 1 - i];
  return n;
}
static void tl_chain_release(ITypeInfo **chain, UINT n) { for (UINT i = 0; i < n; i++) ITypeInfo_Release(chain[i]); }

static UINT tl_ti_inherited_funcs(const TlTi *ti) {
  if (!tl_ti_inherits(ti)) return 0;
  ITypeInfo *chain[TL_MAX_BASES];
  const UINT n = tl_ti_chain(ti, chain);
  UINT total = 0;
  for (UINT i = 0; i < n; i++) {
    TYPEATTR *ta;
    if (SUCCEEDED(ITypeInfo_GetTypeAttr(chain[i], &ta))) { total += ta->cFuncs; ITypeInfo_ReleaseTypeAttr(chain[i], ta); }
  }
  tl_chain_release(chain, n);
  return total;
}

/* Adds `offset` to every user-defined reference in a type description. */
static void tl_td_add_href(TYPEDESC *td, DWORD offset, UINT depth) {
  if (depth > 64) return;
  if (td->vt == VT_USERDEFINED) td->hreftype += offset;
  else if ((td->vt == VT_PTR || td->vt == VT_SAFEARRAY) && td->lptdesc) tl_td_add_href(td->lptdesc, offset, depth + 1);
  else if (td->vt == VT_CARRAY && td->lpadesc) tl_td_add_href(&td->lpadesc->tdescElem, offset, depth + 1);
}

/* The number of parameters a virtual function shows as a dispatch function: a trailing [retval] parameter becomes the return value, and trailing [lcid] parameters go. */
static SHORT tl_dispatch_param_count(const FUNCDESC *fd) {
  SHORT n = fd->cParams;
  if (fd->funckind == FUNC_DISPATCH) return n;
  if (n > 0 && (fd->lprgelemdescParam[n - 1].paramdesc.wParamFlags & PARAMFLAG_FRETVAL) && fd->lprgelemdescParam[n - 1].tdesc.vt == VT_PTR) n--;
  while (n > 0 && (fd->lprgelemdescParam[n - 1].paramdesc.wParamFlags & PARAMFLAG_FLCID)) n--;
  return n;
}
/* The dispatch form of a function that is declared as a virtual one: HRESULT becomes VT_VOID, a trailing [retval] parameter becomes the return value. */
static void tl_funcdesc_to_dispatch(FUNCDESC *fd) {
  if (fd->funckind == FUNC_DISPATCH) return;
  const SHORT keep = tl_dispatch_param_count(fd);
  BOOL had_retval = FALSE;
  if (fd->cParams > 0 && (fd->lprgelemdescParam[fd->cParams - 1].paramdesc.wParamFlags & PARAMFLAG_FRETVAL) && fd->lprgelemdescParam[fd->cParams - 1].tdesc.vt == VT_PTR &&
      fd->lprgelemdescParam[fd->cParams - 1].tdesc.lptdesc) {
    ELEMDESC *last = &fd->lprgelemdescParam[fd->cParams - 1];
    tl_elem_free(&fd->elemdescFunc);
    fd->elemdescFunc.tdesc = *last->tdesc.lptdesc;          /* (the pointee moves; its block is freed, the type description it holds stays) */
    oa_free(last->tdesc.lptdesc);
    last->tdesc.lptdesc = NULL;
    last->tdesc.vt = VT_EMPTY;
    had_retval = TRUE;
  }
  for (SHORT i = keep; i < fd->cParams; i++) tl_elem_free(&fd->lprgelemdescParam[i]);
  fd->cParams = keep;
  fd->funckind = FUNC_DISPATCH;
  if (!had_retval && fd->elemdescFunc.tdesc.vt == VT_HRESULT) fd->elemdescFunc.tdesc.vt = VT_VOID;
}

/* The function `index` as the typeinfo shows it: an owned FUNCDESC copy. */
static HRESULT tl_ti_func_desc(TlTi *ti, UINT index, FUNCDESC **out) {
  const TlType *t = ti->t;
  const UINT inherited = tl_ti_inherited_funcs(ti);
  if (index >= inherited + t->nfuncs) return TYPE_E_ELEMENTNOTFOUND;
  HRESULT hr = S_OK;
  if (index < inherited) {
    ITypeInfo *chain[TL_MAX_BASES];
    const UINT n = tl_ti_chain(ti, chain);
    UINT rest = index;
    hr = TYPE_E_ELEMENTNOTFOUND;
    for (UINT i = 0; i < n; i++) {
      TYPEATTR *ta;
      if (FAILED(ITypeInfo_GetTypeAttr(chain[i], &ta))) continue;
      const UINT count = ta->cFuncs;
      ITypeInfo_ReleaseTypeAttr(chain[i], ta);
      if (rest >= count) { rest -= count; continue; }
      FUNCDESC *fd;
      hr = ITypeInfo_GetFuncDesc(chain[i], rest, &fd);
      if (SUCCEEDED(hr)) {
        FUNCDESC *copy = tl_funcdesc_clone(fd, &hr);
        ITypeInfo_ReleaseFuncDesc(chain[i], fd);
        if (copy) {
          for (SHORT p = 0; p < copy->cParams; p++) tl_td_add_href(&copy->lprgelemdescParam[p].tdesc, (i + 1) << 24, 0);
          tl_td_add_href(&copy->elemdescFunc.tdesc, (i + 1) << 24, 0);
          tl_funcdesc_to_dispatch(copy);
          *out = copy;
        }
      }
      break;
    }
    tl_chain_release(chain, n);
    return hr;
  }
  FUNCDESC *fd = tl_funcdesc_clone(&t->funcs[index - inherited].fd, &hr);
  if (!fd) return hr;
  if (tl_ti_is_dispatch_view(ti)) {
    if (fd->funckind == FUNC_DISPATCH) fd->oVft = 0;        /* (a dispatch function has no place in a virtual table) */
    tl_funcdesc_to_dispatch(fd);
  }
  *out = fd;
  return S_OK;
}

/* The own function with a member id (and optionally an invoke kind), or NULL. */
static TlFunc *tl_ti_find_func(const TlTi *ti, MEMBERID id, INT invkind) {
  for (UINT i = 0; i < ti->t->nfuncs; i++) {
    TlFunc *f = &ti->t->funcs[i];
    if (f->fd.memid == id && (invkind < 0 || (f->fd.invkind & invkind))) return f;
  }
  return NULL;
}
static TlVar *tl_ti_find_var(const TlTi *ti, MEMBERID id) {
  for (UINT i = 0; i < ti->t->nvars; i++) if (ti->t->vars[i].vd.memid == id) return &ti->t->vars[i];
  return NULL;
}

/* ---- ITypeInfo2 methods ------------------------------------------------------------------------------------------------------------------------------- */
static HRESULT WINAPI tl_ti_QueryInterface(ITypeInfo2 *iface, REFIID riid, void **out) {
  TlTi *ti = tl_ti_from_ti(iface);
  if (!out) return E_POINTER;
  *out = NULL;
  if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_ITypeInfo) || IsEqualIID(riid, &IID_ITypeInfo2)) *out = &ti->ITypeInfo2_iface;
  else if (IsEqualIID(riid, &IID_ITypeComp)) *out = &ti->ITypeComp_iface;
  else if (ti->t->lib->created && !ti->alt && (IsEqualIID(riid, &IID_ICreateTypeInfo) || IsEqualIID(riid, &IID_ICreateTypeInfo2))) *out = &ti->ICreateTypeInfo2_iface;
  else return E_NOINTERFACE;
  ITypeInfo2_AddRef(iface);
  return S_OK;
}
/* (a typeinfo that is referenced keeps its library alive) */
static ULONG WINAPI tl_ti_AddRef(ITypeInfo2 *iface) {
  TlTi *ti = tl_ti_from_ti(iface);
  const LONG ref = InterlockedIncrement(&ti->ref);
  if (ref == 1) ITypeLib2_AddRef(&ti->t->lib->ITypeLib2_iface);
  return (ULONG)ref;
}
static ULONG WINAPI tl_ti_Release(ITypeInfo2 *iface) {
  TlTi *ti = tl_ti_from_ti(iface);
  TlLib *lib = ti->t->lib;
  const LONG ref = InterlockedDecrement(&ti->ref);
  if (ref == 0) ITypeLib2_Release(&lib->ITypeLib2_iface);
  return (ULONG)ref;
}

/* The attributes as shown, in a new TYPEATTR. */
static HRESULT tl_ti_make_attr(TlTi *ti, TYPEATTR **out) {
  TYPEATTR *a = oa_calloc(sizeof(TYPEATTR));
  if (!a) return E_OUTOFMEMORY;
  *a = ti->t->attr;
  a->lcid = ti->t->lib->lcid;
  HRESULT hr = tl_td_copy(&a->tdescAlias, &ti->t->attr.tdescAlias, 0);
  if (FAILED(hr)) { oa_free(a); return hr; }
  a->typekind = tl_ti_view_kind(ti);
  if (a->typekind == TKIND_DISPATCH) {
    a->cbSizeVft = 7 * sizeof(void *);
    a->cFuncs = (WORD)(a->cFuncs + tl_ti_inherited_funcs(ti));
    if (ti->t->attr.wTypeFlags & TYPEFLAG_FDUAL) {
      a->wTypeFlags &= (WORD)~TYPEFLAG_FOLEAUTOMATION;
    }
  }
  *out = a;
  return S_OK;
}
static HRESULT WINAPI tl_ti_GetTypeAttr(ITypeInfo2 *iface, TYPEATTR **out) {
  if (!out) return E_INVALIDARG;
  return tl_ti_make_attr(tl_ti_from_ti(iface), out);
}
static HRESULT WINAPI tl_ti_GetTypeComp(ITypeInfo2 *iface, ITypeComp **out) {
  if (!out) return E_INVALIDARG;
  *out = &tl_ti_from_ti(iface)->ITypeComp_iface;
  ITypeInfo2_AddRef(iface);
  return S_OK;
}
static HRESULT WINAPI tl_ti_GetFuncDesc(ITypeInfo2 *iface, UINT index, FUNCDESC **out) {
  if (!out) return E_INVALIDARG;
  *out = NULL;
  return tl_ti_func_desc(tl_ti_from_ti(iface), index, out);
}
static HRESULT WINAPI tl_ti_GetVarDesc(ITypeInfo2 *iface, UINT index, VARDESC **out) {
  TlTi *ti = tl_ti_from_ti(iface);
  if (!out) return E_INVALIDARG;
  *out = NULL;
  if (index >= ti->t->nvars) return TYPE_E_ELEMENTNOTFOUND;
  HRESULT hr;
  *out = tl_vardesc_clone(&ti->t->vars[index].vd, &hr);
  return hr;
}

/* GetNames for a member of this type or of a base. */
static HRESULT WINAPI tl_ti_GetNames(ITypeInfo2 *iface, MEMBERID id, BSTR *names, UINT max, UINT *count) {
  TlTi *ti = tl_ti_from_ti(iface);
  if (!names || !count) return E_INVALIDARG;
  *count = 0;
  if (!max) return S_OK;
  const TlFunc *f = tl_ti_find_func(ti, id, -1);
  if (f) {
    UINT n = 0;
    names[n] = tl_bstr(f->name);
    if (f->name && !names[n]) return E_OUTOFMEMORY;
    n++;
    SHORT np = f->fd.cParams;
    if (tl_ti_is_dispatch_view(ti)) np = tl_dispatch_param_count(&f->fd);
    for (SHORT i = 0; i < np && n < max; i++, n++) {
      if (!f->pnames || !f->pnames[i]) break;                 /* (the names stop at the first parameter that has none) */
      names[n] = tl_bstr(f->pnames[i]);
      if (!names[n]) { for (UINT k = 0; k < n; k++) SysFreeString(names[k]); return E_OUTOFMEMORY; }
    }
    *count = n;
    return S_OK;
  }
  const TlVar *v = tl_ti_find_var(ti, id);
  if (v) {
    names[0] = tl_bstr(v->name);
    if (v->name && !names[0]) return E_OUTOFMEMORY;
    *count = 1;
    return S_OK;
  }
  if (tl_ti_inherits(ti)) {
    /* a function of a base interface, as a dispatch function (the parameters that became the return value or went are not named) */
    ITypeInfo *chain[TL_MAX_BASES];
    const UINT nchain = tl_ti_chain(ti, chain);
    HRESULT hr = TYPE_E_ELEMENTNOTFOUND;
    for (UINT k = 0; k < nchain && hr == TYPE_E_ELEMENTNOTFOUND; k++) {
      TYPEATTR *ta;
      if (FAILED(ITypeInfo_GetTypeAttr(chain[k], &ta))) continue;
      const UINT nf = ta->cFuncs;
      ITypeInfo_ReleaseTypeAttr(chain[k], ta);
      for (UINT i = 0; i < nf; i++) {
        FUNCDESC *fd;
        if (FAILED(ITypeInfo_GetFuncDesc(chain[k], i, &fd))) continue;
        if (fd->memid == id) {
          const UINT np = (UINT)tl_dispatch_param_count(fd);
          UINT limit = max < np + 1 ? max : np + 1, got = 0;
          hr = ITypeInfo_GetNames(chain[k], id, names, limit, &got);
          *count = got;
        }
        ITypeInfo_ReleaseFuncDesc(chain[k], fd);
        if (hr != TYPE_E_ELEMENTNOTFOUND) break;
      }
    }
    tl_chain_release(chain, nchain);
    return hr;
  }
  ITypeInfo *base = tl_ti_base(ti);
  if (base) {
    HRESULT hr = ITypeInfo_GetNames(base, id, names, max, count);
    ITypeInfo_Release(base);
    return hr;
  }
  return TYPE_E_ELEMENTNOTFOUND;
}
static HRESULT WINAPI tl_ti_GetRefTypeOfImplType(ITypeInfo2 *iface, UINT index, HREFTYPE *out) {
  TlTi *ti = tl_ti_from_ti(iface);
  const TlType *t = ti->t;
  if (!out) return E_INVALIDARG;
  if (index == (UINT)-1) {
    if (tl_ti_is_dual_dispatch(ti)) { *out = (HREFTYPE)-2; return S_OK; }
    return TYPE_E_ELEMENTNOTFOUND;
  }
  if (index >= t->attr.cImplTypes) return TYPE_E_ELEMENTNOTFOUND;
  if (tl_ti_is_dispatch_view(ti)) {
    /* a dispinterface always derives from IDispatch, whatever the file names as its base */
    *out = (INT)t->lib->dispatch_href == -1 ? (HREFTYPE)-1 : t->lib->dispatch_href;
    return S_OK;
  }
  if (index >= t->nimpls) return TYPE_E_ELEMENTNOTFOUND;
  *out = t->impls[index].href;
  if (t->attr.typekind == TKIND_INTERFACE || t->attr.typekind == TKIND_DISPATCH) *out |= 2;
  return S_OK;
}
static HRESULT WINAPI tl_ti_GetImplTypeFlags(ITypeInfo2 *iface, UINT index, INT *flags) {
  TlTi *ti = tl_ti_from_ti(iface);
  const TlType *t = ti->t;
  if (!flags) return E_INVALIDARG;
  if (t->attr.typekind == TKIND_DISPATCH && index == 0) { *flags = 0; return S_OK; }
  if (index >= t->nimpls) return TYPE_E_ELEMENTNOTFOUND;
  *flags = t->impls[index].flags;
  return S_OK;
}

/* Name lookup for GetIDsOfNames. The member ids of the functions and variables (the first name), and for a function the index of its parameters (the others). */
static HRESULT WINAPI tl_ti_GetIDsOfNames(ITypeInfo2 *iface, LPOLESTR *names, UINT count, MEMBERID *ids) {
  TlTi *ti = tl_ti_from_ti(iface);
  const TlType *t = ti->t;
  if (!names || !ids) return E_INVALIDARG;
  if (!count) return S_OK;
  if (t->attr.typekind == TKIND_COCLASS) return TYPE_E_WRONGTYPEKIND;
  for (UINT i = 0; i < count; i++) ids[i] = DISPID_UNKNOWN;
  const TlFunc *found = NULL;
  for (UINT i = 0; i < t->nfuncs && !found; i++) {
    const TlFunc *f = &t->funcs[i];
    if (f->name && !tl_wcsicmp(f->name, names[0])) found = f;
  }
  HRESULT hr = S_OK;
  if (found) {
    ids[0] = found->fd.memid;
    for (UINT n = 1; n < count; n++) {
      BOOL ok = FALSE;
      for (SHORT p = 0; found->pnames && p < found->fd.cParams; p++)
        if (found->pnames[p] && !tl_wcsicmp(found->pnames[p], names[n])) { ids[n] = (MEMBERID)p; ok = TRUE; break; }
      if (!ok) hr = DISP_E_UNKNOWNNAME;
    }
    return hr;
  }
  for (UINT i = 0; i < t->nvars; i++) {
    const TlVar *v = &t->vars[i];
    if (v->name && !tl_wcsicmp(v->name, names[0])) {
      ids[0] = v->vd.memid;
      return count > 1 ? DISP_E_UNKNOWNNAME : S_OK;
    }
  }
  ITypeInfo *base = tl_ti_base(ti);
  if (base) {
    hr = ITypeInfo_GetIDsOfNames(base, names, count, ids);
    ITypeInfo_Release(base);
    return hr;
  }
  return DISP_E_UNKNOWNNAME;
}

static HRESULT WINAPI tl_ti_GetDocumentation(ITypeInfo2 *iface, MEMBERID id, BSTR *name, BSTR *doc, DWORD *help_context, BSTR *help_file) {
  TlTi *ti = tl_ti_from_ti(iface);
  const TlType *t = ti->t;
  const WCHAR *n, *d;
  DWORD hc;
  if (id == MEMBERID_NIL) { n = t->name; d = t->doc; hc = t->help_context; }
  else {
    const TlFunc *f = tl_ti_find_func(ti, id, -1);
    const TlVar *v = f ? NULL : tl_ti_find_var(ti, id);
    if (f) { n = f->name; d = f->doc; hc = f->help_context; }
    else if (v) { n = v->name; d = v->doc; hc = v->help_context; }
    else {
      ITypeInfo *base = tl_ti_base(ti);
      if (!base) return TYPE_E_ELEMENTNOTFOUND;
      HRESULT hr = ITypeInfo_GetDocumentation(base, id, name, doc, help_context, help_file);
      ITypeInfo_Release(base);
      return hr;
    }
  }
  BSTR bn = NULL, bd = NULL, bf = NULL;
  if (name) { bn = tl_bstr(n); if (n && !bn) return E_OUTOFMEMORY; }
  if (doc) { bd = tl_bstr(d); if (d && !bd) { SysFreeString(bn); return E_OUTOFMEMORY; } }
  if (help_file) { bf = tl_bstr(t->lib->helpfile); if (t->lib->helpfile && !bf) { SysFreeString(bn); SysFreeString(bd); return E_OUTOFMEMORY; } }
  if (name) *name = bn;
  if (doc) *doc = bd;
  if (help_context) *help_context = hc;
  if (help_file) *help_file = bf;
  return S_OK;
}
static HRESULT WINAPI tl_ti_GetDllEntry(ITypeInfo2 *iface, MEMBERID id, INVOKEKIND kind, BSTR *dll, BSTR *name, WORD *ordinal) {
  TlTi *ti = tl_ti_from_ti(iface);
  if (dll) *dll = NULL;
  if (name) *name = NULL;
  if (ordinal) *ordinal = 0;
  if (ti->t->attr.typekind != TKIND_MODULE) return TYPE_E_BADMODULEKIND;
  const TlFunc *f = tl_ti_find_func(ti, id, (INT)kind);
  if (!f) return TYPE_E_ELEMENTNOTFOUND;
  if (dll) { *dll = tl_bstr(ti->t->dllname); if (ti->t->dllname && !*dll) return E_OUTOFMEMORY; }
  if (f->has_ordinal && !f->dll_entry) {
    if (ordinal) *ordinal = f->ordinal;
  } else {
    if (name) { *name = tl_bstr(f->dll_entry ? f->dll_entry : f->name); if (!*name) { if (dll) { SysFreeString(*dll); *dll = NULL; } return E_OUTOFMEMORY; } }
    if (ordinal) *ordinal = 0xffff;
  }
  return S_OK;
}

static HRESULT WINAPI tl_ti_GetRefTypeInfo(ITypeInfo2 *iface, HREFTYPE href, ITypeInfo **out) {
  TlTi *ti = tl_ti_from_ti(iface);
  if (!out) return E_INVALIDARG;
  *out = NULL;
  if (href == (HREFTYPE)-2 && tl_ti_is_dual(ti)) {
    *out = (ITypeInfo *)&ti->t->alt_ti.ITypeInfo2_iface;
    ITypeInfo2_AddRef(&ti->t->alt_ti.ITypeInfo2_iface);
    return S_OK;
  }
  if (href == (HREFTYPE)-1 && tl_ti_is_dispatch_view(ti)) { ITypeInfo *d = tl_std_type(4); *out = d; return d ? S_OK : E_OUTOFMEMORY; }
  if (href & 0xff000000 && href != (HREFTYPE)-1 && href != (HREFTYPE)-2) {
    /* a reference that came from a base interface through the dispatch view: (depth) in the upper byte, the base's own reference below */
    ITypeInfo *chain[TL_MAX_BASES];
    const UINT n = tl_ti_chain(ti, chain);
    const UINT k = (href >> 24) - 1;
    HRESULT hr = TYPE_E_ELEMENTNOTFOUND;
    if (k < n) hr = ITypeInfo_GetRefTypeInfo(chain[k], href & 0xffffff, out);
    tl_chain_release(chain, n);
    return hr;
  }
  HRESULT hr = tl_lib_ref_info(ti->t->lib, href, out);
  /* an interface refers to the interface half of a dual interface it derives from */
  if (SUCCEEDED(hr) && tl_ti_view_kind(ti) == TKIND_INTERFACE) *out = tl_interface_half(*out);
  return hr;
}
static HRESULT WINAPI tl_ti_AddressOfMember(ITypeInfo2 *iface, MEMBERID id, INVOKEKIND kind, PVOID *addr) {
  TlTi *ti = tl_ti_from_ti(iface);
  if (!addr) return E_INVALIDARG;
  *addr = NULL;
  if (ti->t->attr.typekind != TKIND_MODULE) return TYPE_E_WRONGTYPEKIND;
  BSTR dll = NULL, entry = NULL;
  WORD ord = 0;
  HRESULT hr = tl_ti_GetDllEntry(iface, id, kind, &dll, &entry, &ord);
  if (FAILED(hr)) return hr;
  HMODULE mod = dll ? LoadLibraryW(dll) : NULL;
  if (!mod) { SysFreeString(dll); SysFreeString(entry); return STG_E_FILENOTFOUND; }
  if (entry) {
    char narrow[256];
    UINT i = 0;
    for (; entry[i] && i + 1 < sizeof(narrow); i++) narrow[i] = (char)entry[i];
    narrow[i] = 0;
    *addr = (PVOID)GetProcAddress(mod, narrow);
  } else {
    *addr = (PVOID)GetProcAddress(mod, (LPCSTR)(ULONG_PTR)ord);
  }
  SysFreeString(dll);
  SysFreeString(entry);
  return *addr ? S_OK : TYPE_E_ELEMENTNOTFOUND;
}
static HRESULT WINAPI tl_ti_CreateInstance(ITypeInfo2 *iface, IUnknown *outer, REFIID riid, PVOID *out) {
  TlTi *ti = tl_ti_from_ti(iface);
  if (!out) return E_INVALIDARG;
  *out = NULL;
  if (ti->t->attr.typekind != TKIND_COCLASS) return TYPE_E_WRONGTYPEKIND;
  /* CoCreateInstance, found at run time: this library does not link ole32 */
  typedef HRESULT (WINAPI *CoCreateInstanceFn)(REFCLSID, LPUNKNOWN, DWORD, REFIID, LPVOID *);
  HMODULE ole = LoadLibraryW(L"ole32.dll");
  CoCreateInstanceFn create = ole ? (CoCreateInstanceFn)(void *)GetProcAddress(ole, "CoCreateInstance") : NULL;
  if (!create) return REGDB_E_CLASSNOTREG;
  HRESULT hr = create(&ti->t->attr.guid, outer, CLSCTX_INPROC_SERVER, riid, out);
  if (SUCCEEDED(hr) && !*out) hr = E_NOINTERFACE;
  return hr;
}
static HRESULT WINAPI tl_ti_GetMops(ITypeInfo2 *iface, MEMBERID id, BSTR *mops) {
  (void)iface; (void)id;
  if (!mops) return E_INVALIDARG;
  *mops = NULL;
  return S_OK;
}
static HRESULT WINAPI tl_ti_GetContainingTypeLib(ITypeInfo2 *iface, ITypeLib **lib, UINT *index) {
  TlTi *ti = tl_ti_from_ti(iface);
  if (index) *index = ti->t->index;
  if (lib) {
    *lib = (ITypeLib *)&ti->t->lib->ITypeLib2_iface;
    ITypeLib2_AddRef(&ti->t->lib->ITypeLib2_iface);
  }
  return S_OK;
}
static void WINAPI tl_ti_ReleaseTypeAttr(ITypeInfo2 *iface, TYPEATTR *attr) {
  (void)iface;
  if (!attr) return;
  tl_td_free(&attr->tdescAlias);
  oa_free(attr);
}
static void WINAPI tl_ti_ReleaseFuncDesc(ITypeInfo2 *iface, FUNCDESC *fd) { (void)iface; tl_funcdesc_release(fd); }
static void WINAPI tl_ti_ReleaseVarDesc(ITypeInfo2 *iface, VARDESC *vd) { (void)iface; tl_vardesc_release(vd); }

static HRESULT WINAPI tl_ti_GetTypeKind(ITypeInfo2 *iface, TYPEKIND *kind) {
  TlTi *ti = tl_ti_from_ti(iface);
  if (!kind) return E_INVALIDARG;
  *kind = tl_ti_view_kind(ti);
  return S_OK;
}
static HRESULT WINAPI tl_ti_GetTypeFlags(ITypeInfo2 *iface, ULONG *flags) {
  TlTi *ti = tl_ti_from_ti(iface);
  if (!flags) return E_INVALIDARG;
  *flags = ti->t->attr.wTypeFlags;
  if (tl_ti_is_dispatch_view(ti) && (ti->t->attr.wTypeFlags & TYPEFLAG_FDUAL)) *flags &= ~(ULONG)TYPEFLAG_FOLEAUTOMATION;
  return S_OK;
}
static HRESULT WINAPI tl_ti_GetFuncIndexOfMemId(ITypeInfo2 *iface, MEMBERID id, INVOKEKIND kind, UINT *index) {
  TlTi *ti = tl_ti_from_ti(iface);
  if (!index) return E_INVALIDARG;
  const UINT inherited = tl_ti_inherited_funcs(ti);
  for (UINT i = 0; i < ti->t->nfuncs; i++)
    if (ti->t->funcs[i].fd.memid == id && (ti->t->funcs[i].fd.invkind & kind)) { *index = inherited + i; return S_OK; }
  return TYPE_E_ELEMENTNOTFOUND;
}
static HRESULT WINAPI tl_ti_GetVarIndexOfMemId(ITypeInfo2 *iface, MEMBERID id, UINT *index) {
  TlTi *ti = tl_ti_from_ti(iface);
  if (!index) return E_INVALIDARG;
  for (UINT i = 0; i < ti->t->nvars; i++) if (ti->t->vars[i].vd.memid == id) { *index = i; return S_OK; }
  return TYPE_E_ELEMENTNOTFOUND;
}
static HRESULT WINAPI tl_ti_GetCustData(ITypeInfo2 *iface, REFGUID guid, VARIANT *out) { return tl_cust_get(&tl_ti_from_ti(iface)->t->cust, guid, out); }
static HRESULT WINAPI tl_ti_GetFuncCustData(ITypeInfo2 *iface, UINT index, REFGUID guid, VARIANT *out) {
  TlTi *ti = tl_ti_from_ti(iface);
  const UINT inherited = tl_ti_inherited_funcs(ti);
  if (index >= inherited + ti->t->nfuncs) return TYPE_E_ELEMENTNOTFOUND;
  static const TlCustList none;
  return tl_cust_get(index < inherited ? &none : &ti->t->funcs[index - inherited].cust, guid, out);
}
static HRESULT WINAPI tl_ti_GetParamCustData(ITypeInfo2 *iface, UINT index, UINT param, REFGUID guid, VARIANT *out) {
  TlTi *ti = tl_ti_from_ti(iface);
  const UINT inherited = tl_ti_inherited_funcs(ti);
  if (index >= inherited + ti->t->nfuncs) return TYPE_E_ELEMENTNOTFOUND;
  static const TlCustList empty;
  if (index < inherited) return tl_cust_get(&empty, guid, out);
  const TlFunc *f = &ti->t->funcs[index - inherited];
  if (param >= (UINT)f->fd.cParams) return TYPE_E_ELEMENTNOTFOUND;
  return tl_cust_get(f->pcust ? &f->pcust[param] : &empty, guid, out);
}
static HRESULT WINAPI tl_ti_GetVarCustData(ITypeInfo2 *iface, UINT index, REFGUID guid, VARIANT *out) {
  TlTi *ti = tl_ti_from_ti(iface);
  if (index >= ti->t->nvars) return TYPE_E_ELEMENTNOTFOUND;
  return tl_cust_get(&ti->t->vars[index].cust, guid, out);
}
static HRESULT WINAPI tl_ti_GetImplTypeCustData(ITypeInfo2 *iface, UINT index, REFGUID guid, VARIANT *out) {
  TlTi *ti = tl_ti_from_ti(iface);
  if (index >= ti->t->nimpls) return TYPE_E_ELEMENTNOTFOUND;
  return tl_cust_get(&ti->t->impls[index].cust, guid, out);
}
static HRESULT WINAPI tl_ti_GetDocumentation2(ITypeInfo2 *iface, MEMBERID id, LCID lcid, BSTR *help_string, DWORD *help_string_context, BSTR *help_string_dll) {
  TlTi *ti = tl_ti_from_ti(iface);
  const TlType *t = ti->t;
  const WCHAR *d;
  DWORD hc;
  (void)lcid;
  if (id == MEMBERID_NIL) { d = t->doc; hc = t->help_string_context; }
  else {
    const TlFunc *f = tl_ti_find_func(ti, id, -1);
    const TlVar *v = f ? NULL : tl_ti_find_var(ti, id);
    if (f) { d = f->doc; hc = f->help_string_context; }
    else if (v) { d = v->doc; hc = v->help_string_context; }
    else return TYPE_E_ELEMENTNOTFOUND;
  }
  if (help_string) { *help_string = tl_bstr(d); if (d && !*help_string) return E_OUTOFMEMORY; }
  if (help_string_context) *help_string_context = hc;
  if (help_string_dll) *help_string_dll = tl_bstr(t->lib->helpstringdll);
  return S_OK;
}
static HRESULT WINAPI tl_ti_GetAllCustData(ITypeInfo2 *iface, CUSTDATA *out) { return tl_cust_all(&tl_ti_from_ti(iface)->t->cust, out); }
static HRESULT WINAPI tl_ti_GetAllFuncCustData(ITypeInfo2 *iface, UINT index, CUSTDATA *out) {
  TlTi *ti = tl_ti_from_ti(iface);
  const UINT inherited = tl_ti_inherited_funcs(ti);
  if (index >= inherited + ti->t->nfuncs) return TYPE_E_ELEMENTNOTFOUND;
  static const TlCustList none;
  return tl_cust_all(index < inherited ? &none : &ti->t->funcs[index - inherited].cust, out);
}
static HRESULT WINAPI tl_ti_GetAllParamCustData(ITypeInfo2 *iface, UINT index, UINT param, CUSTDATA *out) {
  TlTi *ti = tl_ti_from_ti(iface);
  const UINT inherited = tl_ti_inherited_funcs(ti);
  if (index >= inherited + ti->t->nfuncs) return TYPE_E_ELEMENTNOTFOUND;
  static const TlCustList empty;
  if (index < inherited) return tl_cust_all(&empty, out);
  const TlFunc *f = &ti->t->funcs[index - inherited];
  if (param >= (UINT)f->fd.cParams) return TYPE_E_ELEMENTNOTFOUND;
  return tl_cust_all(f->pcust ? &f->pcust[param] : &empty, out);
}
static HRESULT WINAPI tl_ti_GetAllVarCustData(ITypeInfo2 *iface, UINT index, CUSTDATA *out) {
  TlTi *ti = tl_ti_from_ti(iface);
  if (index >= ti->t->nvars) return TYPE_E_ELEMENTNOTFOUND;
  return tl_cust_all(&ti->t->vars[index].cust, out);
}
static HRESULT WINAPI tl_ti_GetAllImplTypeCustData(ITypeInfo2 *iface, UINT index, CUSTDATA *out) {
  TlTi *ti = tl_ti_from_ti(iface);
  if (index >= ti->t->nimpls) return TYPE_E_ELEMENTNOTFOUND;
  return tl_cust_all(&ti->t->impls[index].cust, out);
}

static HRESULT WINAPI tl_ti_Invoke(ITypeInfo2 *iface, PVOID instance, MEMBERID id, WORD flags, DISPPARAMS *params, VARIANT *result, EXCEPINFO *excep, UINT *arg_err);

static ITypeInfo2Vtbl tl_ti_vtbl = {
  tl_ti_QueryInterface, tl_ti_AddRef, tl_ti_Release, tl_ti_GetTypeAttr, tl_ti_GetTypeComp, tl_ti_GetFuncDesc, tl_ti_GetVarDesc, tl_ti_GetNames,
  tl_ti_GetRefTypeOfImplType, tl_ti_GetImplTypeFlags, tl_ti_GetIDsOfNames, tl_ti_Invoke, tl_ti_GetDocumentation, tl_ti_GetDllEntry, tl_ti_GetRefTypeInfo,
  tl_ti_AddressOfMember, tl_ti_CreateInstance, tl_ti_GetMops, tl_ti_GetContainingTypeLib, tl_ti_ReleaseTypeAttr, tl_ti_ReleaseFuncDesc, tl_ti_ReleaseVarDesc,
  tl_ti_GetTypeKind, tl_ti_GetTypeFlags, tl_ti_GetFuncIndexOfMemId, tl_ti_GetVarIndexOfMemId, tl_ti_GetCustData, tl_ti_GetFuncCustData, tl_ti_GetParamCustData,
  tl_ti_GetVarCustData, tl_ti_GetImplTypeCustData, tl_ti_GetDocumentation2, tl_ti_GetAllCustData, tl_ti_GetAllFuncCustData, tl_ti_GetAllParamCustData,
  tl_ti_GetAllVarCustData, tl_ti_GetAllImplTypeCustData,
};
