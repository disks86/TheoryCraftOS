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

/* ICreateTypeLib2 and ICreateTypeInfo2: building a library in the model, and CreateTypeLib2. SaveAllChanges (typelib_write.inc) writes the model as an MSFT file. */

static UINT tl_ptr_size(const TlLib *lib) { return lib->syskind == SYS_WIN64 ? 8 : 4; }

/* ---- ICreateTypeLib2 ----------------------------------------------------------------------------------------------------------------------------------- */
static HRESULT WINAPI tl_clib_QueryInterface(ICreateTypeLib2 *iface, REFIID riid, void **out) { return ITypeLib2_QueryInterface(&tl_lib_from_clib(iface)->ITypeLib2_iface, riid, out); }
static ULONG WINAPI tl_clib_AddRef(ICreateTypeLib2 *iface) { return ITypeLib2_AddRef(&tl_lib_from_clib(iface)->ITypeLib2_iface); }
static ULONG WINAPI tl_clib_Release(ICreateTypeLib2 *iface) { return ITypeLib2_Release(&tl_lib_from_clib(iface)->ITypeLib2_iface); }

static HRESULT WINAPI tl_clib_CreateTypeInfo(ICreateTypeLib2 *iface, LPOLESTR name, TYPEKIND kind, ICreateTypeInfo **out) {
  TlLib *lib = tl_lib_from_clib(iface);
  if (!name || !out) return E_INVALIDARG;
  if ((UINT)kind >= TKIND_MAX) return E_INVALIDARG;
  for (UINT i = 0; i < lib->ntypes; i++) if (lib->types[i]->name && !tl_wcsicmp(lib->types[i]->name, name)) return TYPE_E_NAMECONFLICT;
  TlType *t = tl_lib_add_type(lib, kind);
  if (!t) return E_OUTOFMEMORY;
  t->name = tl_wcsdup(name);
  if (!t->name) return E_OUTOFMEMORY;
  const UINT ptr = tl_ptr_size(lib);
  t->attr.lcid = lib->lcid;
  switch (kind) {
    case TKIND_ENUM: t->attr.cbSizeInstance = 4; t->attr.cbAlignment = 4; break;
    case TKIND_RECORD: case TKIND_UNION: t->attr.cbSizeInstance = 0; t->attr.cbAlignment = 4; break;
    case TKIND_MODULE: t->attr.cbSizeInstance = 2; t->attr.cbAlignment = 1; break;
    case TKIND_INTERFACE: t->attr.cbSizeInstance = ptr; t->attr.cbAlignment = 4; break;
    case TKIND_DISPATCH: t->attr.cbSizeInstance = ptr; t->attr.cbAlignment = 4; t->attr.cbSizeVft = (WORD)(7 * ptr); break;
    case TKIND_COCLASS: t->attr.cbSizeInstance = ptr; t->attr.cbAlignment = 4; break;
    case TKIND_ALIAS: t->attr.cbSizeInstance = ptr; t->attr.cbAlignment = 4; t->attr.tdescAlias.vt = VT_EMPTY; break;
    default: break;
  }
  ITypeInfo2_AddRef(&t->ti.ITypeInfo2_iface);
  *out = (ICreateTypeInfo *)&t->ti.ICreateTypeInfo2_iface;
  ICreateTypeInfo2_AddRef(&t->ti.ICreateTypeInfo2_iface);
  ITypeInfo2_Release(&t->ti.ITypeInfo2_iface);
  return S_OK;
}
static HRESULT WINAPI tl_clib_SetName(ICreateTypeLib2 *iface, LPOLESTR name) {
  TlLib *lib = tl_lib_from_clib(iface);
  if (!name) return E_INVALIDARG;
  HRESULT hr = S_OK;
  tl_set_str(&lib->name, name, &hr);
  return hr;
}
static HRESULT WINAPI tl_clib_SetVersion(ICreateTypeLib2 *iface, WORD major, WORD minor) { TlLib *lib = tl_lib_from_clib(iface); lib->major = major; lib->minor = minor; return S_OK; }
static HRESULT WINAPI tl_clib_SetGuid(ICreateTypeLib2 *iface, REFGUID guid) { if (!guid) return E_INVALIDARG; tl_lib_from_clib(iface)->guid = *guid; return S_OK; }
static HRESULT WINAPI tl_clib_SetDocString(ICreateTypeLib2 *iface, LPOLESTR doc) {
  HRESULT hr = S_OK;
  if (!doc) return E_INVALIDARG;
  tl_set_str(&tl_lib_from_clib(iface)->doc, doc, &hr);
  return hr;
}
static HRESULT WINAPI tl_clib_SetHelpFileName(ICreateTypeLib2 *iface, LPOLESTR file) {
  HRESULT hr = S_OK;
  if (!file) return E_INVALIDARG;
  tl_set_str(&tl_lib_from_clib(iface)->helpfile, file, &hr);
  return hr;
}
static HRESULT WINAPI tl_clib_SetHelpContext(ICreateTypeLib2 *iface, DWORD context) { tl_lib_from_clib(iface)->help_context = context; return S_OK; }
static HRESULT WINAPI tl_clib_SetLcid(ICreateTypeLib2 *iface, LCID lcid) {
  TlLib *lib = tl_lib_from_clib(iface);
  if (lcid != LOCALE_NEUTRAL && lcid != LOCALE_USER_DEFAULT && lcid != LOCALE_SYSTEM_DEFAULT && !IsValidLocale(lcid, LCID_INSTALLED)) return E_INVALIDARG;
  lib->lcid = lcid;
  lib->file_lcid = lcid ? lcid : 0x409;
  for (UINT i = 0; i < lib->ntypes; i++) lib->types[i]->attr.lcid = lcid;
  return S_OK;
}
static HRESULT WINAPI tl_clib_SetLibFlags(ICreateTypeLib2 *iface, UINT flags) { tl_lib_from_clib(iface)->flags = (WORD)flags; return S_OK; }
static HRESULT tl_save(TlLib *lib);
static HRESULT WINAPI tl_clib_SaveAllChanges(ICreateTypeLib2 *iface) { return tl_save(tl_lib_from_clib(iface)); }
static HRESULT WINAPI tl_clib_DeleteTypeInfo(ICreateTypeLib2 *iface, LPOLESTR name) {
  TlLib *lib = tl_lib_from_clib(iface);
  if (!name) return E_INVALIDARG;
  for (UINT i = 0; i < lib->ntypes; i++)
    if (lib->types[i]->name && !tl_wcsicmp(lib->types[i]->name, name)) {
      TlType *t = lib->types[i];
      if (t->ti.ref > 0) return TYPE_E_ELEMENTNOTFOUND;
      tl_type_free_contents(t);
      oa_free(t);
      for (UINT k = i; k + 1 < lib->ntypes; k++) { lib->types[k] = lib->types[k + 1]; lib->types[k]->index = k; lib->types[k]->file_offset = k * TL_TYPEINFO_RECORD; }
      lib->ntypes--;
      return S_OK;
    }
  return TYPE_E_ELEMENTNOTFOUND;
}
static BOOL tl_custom_value_ok(const VARIANT *v) {
  switch (V_VT(v)) {
    case VT_I2: case VT_I4: case VT_R4: case VT_R8: case VT_CY: case VT_DATE: case VT_BSTR: case VT_ERROR: case VT_BOOL: case VT_I1: case VT_UI1: case VT_UI2: case VT_UI4:
    case VT_I8: case VT_UI8: case VT_INT: case VT_UINT: case VT_HRESULT: case VT_LPSTR: return TRUE;
    default: return FALSE;
  }
}
static HRESULT tl_set_cust(TlCustList *l, const GUID *guid, const VARIANT *v) {
  if (!guid || !v) return E_INVALIDARG;
  if (!tl_custom_value_ok(v)) return DISP_E_BADVARTYPE;
  return tl_cust_set(l, guid, v);
}
static HRESULT WINAPI tl_clib_SetCustData(ICreateTypeLib2 *iface, REFGUID guid, VARIANT *value) { return tl_set_cust(&tl_lib_from_clib(iface)->cust, guid, value); }
static HRESULT WINAPI tl_clib_SetHelpStringContext(ICreateTypeLib2 *iface, ULONG context) { tl_lib_from_clib(iface)->help_string_context = context; return S_OK; }
static HRESULT WINAPI tl_clib_SetHelpStringDll(ICreateTypeLib2 *iface, LPOLESTR file) {
  HRESULT hr = S_OK;
  if (!file) return E_INVALIDARG;
  tl_set_str(&tl_lib_from_clib(iface)->helpstringdll, file, &hr);
  return hr;
}
static ICreateTypeLib2Vtbl tl_clib_vtbl = {
  tl_clib_QueryInterface, tl_clib_AddRef, tl_clib_Release, tl_clib_CreateTypeInfo, tl_clib_SetName, tl_clib_SetVersion, tl_clib_SetGuid, tl_clib_SetDocString,
  tl_clib_SetHelpFileName, tl_clib_SetHelpContext, tl_clib_SetLcid, tl_clib_SetLibFlags, tl_clib_SaveAllChanges, tl_clib_DeleteTypeInfo, tl_clib_SetCustData,
  tl_clib_SetHelpStringContext, tl_clib_SetHelpStringDll,
};

/* ---- ICreateTypeInfo2 -------------------------------------------------------------------------------------------------------------------------------- */
static HRESULT WINAPI tl_cti_QueryInterface(ICreateTypeInfo2 *iface, REFIID riid, void **out) { return ITypeInfo2_QueryInterface(&tl_ti_from_cti(iface)->ITypeInfo2_iface, riid, out); }
static ULONG WINAPI tl_cti_AddRef(ICreateTypeInfo2 *iface) { return ITypeInfo2_AddRef(&tl_ti_from_cti(iface)->ITypeInfo2_iface); }
static ULONG WINAPI tl_cti_Release(ICreateTypeInfo2 *iface) { return ITypeInfo2_Release(&tl_ti_from_cti(iface)->ITypeInfo2_iface); }

static HRESULT WINAPI tl_cti_SetGuid(ICreateTypeInfo2 *iface, REFGUID guid) { if (!guid) return E_INVALIDARG; tl_ti_from_cti(iface)->t->attr.guid = *guid; return S_OK; }
static HRESULT WINAPI tl_cti_AddRefTypeInfo(ICreateTypeInfo2 *iface, ITypeInfo *info, HREFTYPE *href);
static HRESULT WINAPI tl_cti_SetTypeFlags(ICreateTypeInfo2 *iface, UINT flags) {
  TlType *t = tl_ti_from_cti(iface)->t;
  if (flags & TYPEFLAG_FDUAL) {
    /* a dual interface refers to IDispatch (the library needs a reference for it) */
    ITypeInfo *dispatch = tl_std_type(4);
    HREFTYPE href;
    if (!dispatch) return E_OUTOFMEMORY;
    HRESULT hr = tl_cti_AddRefTypeInfo(iface, dispatch, &href);
    ITypeInfo_Release(dispatch);
    if (FAILED(hr)) return hr;
  }
  t->attr.wTypeFlags = (WORD)(flags | ((flags & TYPEFLAG_FDUAL) ? TYPEFLAG_FDISPATCHABLE : 0));
  return S_OK;
}
static HRESULT WINAPI tl_cti_SetDocString(ICreateTypeInfo2 *iface, LPOLESTR doc) {
  HRESULT hr = S_OK;
  if (!doc) return E_INVALIDARG;
  tl_set_str(&tl_ti_from_cti(iface)->t->doc, doc, &hr);
  return hr;
}
static HRESULT WINAPI tl_cti_SetHelpContext(ICreateTypeInfo2 *iface, DWORD context) { tl_ti_from_cti(iface)->t->help_context = context; return S_OK; }
static HRESULT WINAPI tl_cti_SetVersion(ICreateTypeInfo2 *iface, WORD major, WORD minor) {
  TlType *t = tl_ti_from_cti(iface)->t;
  t->attr.wMajorVerNum = major;
  t->attr.wMinorVerNum = minor;
  return S_OK;
}

/* Is the library the type belongs to the library of `info`? Then the reference is internal: the offset of the type. */
static HRESULT WINAPI tl_cti_AddRefTypeInfo(ICreateTypeInfo2 *iface, ITypeInfo *info, HREFTYPE *href) {
  TlLib *lib = tl_ti_from_cti(iface)->t->lib;
  if (!info || !href) return E_INVALIDARG;
  ITypeLib *other = NULL;
  UINT index = 0;
  HRESULT hr = ITypeInfo_GetContainingTypeLib(info, &other, &index);
  if (FAILED(hr)) return hr;
  if (other == (ITypeLib *)&lib->ITypeLib2_iface) {
    ITypeLib_Release(other);
    *href = index * TL_TYPEINFO_RECORD;
    return S_OK;
  }
  TLIBATTR *la = NULL;
  hr = ITypeLib_GetLibAttr(other, &la);
  if (FAILED(hr)) { ITypeLib_Release(other); return hr; }
  TYPEATTR *ta = NULL;
  hr = ITypeInfo_GetTypeAttr(info, &ta);
  if (FAILED(hr)) { ITypeLib_ReleaseTLibAttr(other, la); ITypeLib_Release(other); return hr; }
  /* the imported library */
  UINT file = (UINT)-1;
  for (UINT i = 0; i < lib->nimpfiles; i++) if (IsEqualGUID(&lib->impfiles[i].guid, &la->guid)) file = i;
  if (file == (UINT)-1) {
    TlImpFile *f = TL_GROW(lib->impfiles, lib->nimpfiles, lib->impfiles_cap);
    if (!f) hr = E_OUTOFMEMORY;
    else {
      file = lib->nimpfiles - 1;
      f->guid = la->guid; f->lcid = la->lcid; f->major = la->wMajorVerNum; f->minor = la->wMinorVerNum;
      const WCHAR *path = ((((ITypeLib2 *)other)->lpVtbl == &tl_lib_vtbl) ? tl_lib_from_lib((ITypeLib2 *)other)->path : NULL);
      BSTR name = NULL;
      const WCHAR *base = path;
      if (path) { for (const WCHAR *p = path; *p; p++) if (*p == '\\' || *p == '/') base = p + 1; }
      else { ITypeLib_GetDocumentation(other, -1, &name, NULL, NULL, NULL); base = name; }
      f->name = tl_wcsdup(base);
      if (!path && f->name) {                      /* (no file name: the library's name and an extension) */
        WCHAR *n = oa_alloc((oa_wcslen(base) + 5) * sizeof(WCHAR));
        if (n) { memcpy(n, base, oa_wcslen(base) * sizeof(WCHAR)); memcpy(n + oa_wcslen(base), L".tlb", 5 * sizeof(WCHAR)); oa_free(f->name); f->name = n; }
      }
      SysFreeString(name);
      if (!f->name) hr = E_OUTOFMEMORY;
    }
  }
  if (SUCCEEDED(hr)) {
    UINT ii = (UINT)-1;
    for (UINT i = 0; i < lib->nimpinfos; i++) if (lib->impinfos[i].file == file && lib->impinfos[i].by_guid && IsEqualGUID(&lib->impinfos[i].guid, &ta->guid)) ii = i;
    if (ii == (UINT)-1) {
      TlImpInfo *im = TL_GROW(lib->impinfos, lib->nimpinfos, lib->impinfos_cap);
      if (!im) hr = E_OUTOFMEMORY;
      else {
        ii = lib->nimpinfos - 1;
        im->file = file; im->by_guid = TRUE; im->guid = ta->guid; im->tkind = ta->typekind; im->offset = index;
      }
    }
    if (SUCCEEDED(hr)) {
      *href = ii * TL_IMPINFO_RECORD + 1;
      if (IsEqualGUID(&ta->guid, &IID_IDispatch) && (INT)lib->dispatch_href == -1) lib->dispatch_href = *href;
    }
  }
  ITypeInfo_ReleaseTypeAttr(info, ta);
  ITypeLib_ReleaseTLibAttr(other, la);
  ITypeLib_Release(other);
  return hr;
}

/* The number of virtual table slots of the base interfaces and their count (the level of the member ids). */
static void tl_base_info(TlType *t, UINT *slots, UINT *levels) {
  *slots = 0; *levels = 0;
  ITypeInfo *chain[TL_MAX_BASES];
  const UINT n = tl_ti_chain(&t->ti, chain);
  if (n) {
    TYPEATTR *ta;
    if (SUCCEEDED(ITypeInfo_GetTypeAttr(chain[n - 1], &ta))) { *slots = ta->cbSizeVft / sizeof(void *);       /* (a loaded library shows the sizes of this process) */ ITypeInfo_ReleaseTypeAttr(chain[n - 1], ta); }
  }
  *levels = n;
  tl_chain_release(chain, n);
}
static void tl_vft_update(TlType *t) {
  if (t->attr.typekind != TKIND_INTERFACE) return;
  UINT slots, levels;
  tl_base_info(t, &slots, &levels);
  const UINT ptr = tl_ptr_size(t->lib);
  UINT size = slots * ptr;
  for (UINT i = 0; i < t->nfuncs; i++) {
    TlFunc *f = &t->funcs[i];
    if (!f->vft_explicit) f->fd.oVft = (SHORT)((slots + i) * ptr);
    const UINT end = (UINT)(unsigned short)f->fd.oVft + ptr;
    if (end > size) size = end;
  }
  t->attr.cbSizeVft = (WORD)size;
}
static BOOL tl_memid_in_use(const TlType *t, MEMBERID id) {
  for (UINT i = 0; i < t->nfuncs; i++) if (t->funcs[i].fd.memid == id) return TRUE;
  return FALSE;
}

static HRESULT WINAPI tl_cti_AddFuncDesc(ICreateTypeInfo2 *iface, UINT index, FUNCDESC *fd) {
  TlType *t = tl_ti_from_cti(iface)->t;
  if (!fd) return E_INVALIDARG;
  if (index > t->nfuncs) return TYPE_E_ELEMENTNOTFOUND;
  if (fd->cParams < 0 || (fd->cParams > 0 && !fd->lprgelemdescParam)) return E_INVALIDARG;
  if ((fd->invkind & (INVOKE_PROPERTYPUT | INVOKE_PROPERTYPUTREF)) && fd->cParams == 0) return TYPE_E_INCONSISTENTPROPFUNCS;
  const UINT ptr = tl_ptr_size(t->lib);
  if (fd->oVft && fd->oVft % ptr) return E_INVALIDARG;
  TlFunc fn;
  memset(&fn, 0, sizeof(fn));
  HRESULT hr = S_OK;
  FUNCDESC *copy = tl_funcdesc_clone(fd, &hr);
  if (!copy) return hr;
  fn.fd = *copy;
  oa_free(copy);
  fn.fd.lprgscode = NULL;
  fn.fd.cScodes = 0;
  /* ELEMDESCs hold IDL flags in the union with the parameter flags: a default value only goes with PARAMFLAG_FHASDEFAULT */
  for (SHORT i = 0; i < fn.fd.cParams; i++) {
    ELEMDESC *e = &fn.fd.lprgelemdescParam[i];
    if ((e->paramdesc.wParamFlags & PARAMFLAG_FHASDEFAULT) && e->paramdesc.pparamdescex) {
      VARIANT *def = &e->paramdesc.pparamdescex->varDefaultValue;
      if (e->tdesc.vt == VT_INT && V_VT(def) == VT_INT) V_VT(def) = VT_I4;
      else if (e->tdesc.vt == VT_UINT && V_VT(def) == VT_UINT) V_VT(def) = VT_UI4;
    }
  }
  UINT slots, levels;
  tl_base_info(t, &slots, &levels);
  if (fn.fd.memid == MEMBERID_NIL) {
    const MEMBERID base = (MEMBERID)(0x60000000u + (levels << 16));
    MEMBERID id = base + (MEMBERID)index;
    if (!fd->oVft && tl_memid_in_use(t, id)) {
      id = base + (MEMBERID)(t->nfuncs + 1);
      while (tl_memid_in_use(t, id)) id++;
    }
    fn.fd.memid = id;
  }
  fn.vft_explicit = fn.fd.oVft != 0;
  if (!fn.fd.oVft) fn.fd.oVft = (SHORT)((slots + index) * ptr);
  fn.fd.callconv = fn.fd.callconv ? fn.fd.callconv : CC_STDCALL;
  if (fd->invkind == INVOKE_FUNC || fd->funckind == FUNC_DISPATCH) { /* (the kinds are kept as given) */ }
  TlFunc *slot = tl_type_add_func(t);
  if (!slot) { tl_func_free(&fn); return E_OUTOFMEMORY; }
  memmove(slot + 0 == &t->funcs[t->nfuncs - 1] ? &t->funcs[index + 1] : slot, &t->funcs[index], (SIZE_T)(t->nfuncs - 1 - index) * sizeof(TlFunc));
  t->funcs[index] = fn;
  tl_vft_update(t);
  return S_OK;
}

static HRESULT WINAPI tl_cti_AddImplType(ICreateTypeInfo2 *iface, UINT index, HREFTYPE href) {
  TlType *t = tl_ti_from_cti(iface)->t;
  if (index != t->nimpls) return TYPE_E_ELEMENTNOTFOUND;
  if (t->attr.typekind == TKIND_INTERFACE || t->attr.typekind == TKIND_DISPATCH) {
    if (t->nimpls >= 1) return TYPE_E_ELEMENTNOTFOUND;
  } else if (t->attr.typekind != TKIND_COCLASS) return TYPE_E_BADMODULEKIND;
  TlImpl *im = tl_type_add_impl(t);
  if (!im) return E_OUTOFMEMORY;
  im->href = href;
  if (t->attr.typekind == TKIND_INTERFACE) {
    tl_vft_update(t);
    /* an interface derived from IDispatch is dispatchable */
    ITypeInfo *chain[TL_MAX_BASES];
    const UINT n = tl_ti_chain(&t->ti, chain);
    for (UINT i = 0; i < n; i++) {
      TYPEATTR *ta;
      if (SUCCEEDED(ITypeInfo_GetTypeAttr(chain[i], &ta))) {
        if (IsEqualGUID(&ta->guid, &IID_IDispatch)) t->attr.wTypeFlags |= TYPEFLAG_FDISPATCHABLE;
        ITypeInfo_ReleaseTypeAttr(chain[i], ta);
      }
    }
    tl_chain_release(chain, n);
  }
  return S_OK;
}
static HRESULT WINAPI tl_cti_SetImplTypeFlags(ICreateTypeInfo2 *iface, UINT index, INT flags) {
  TlType *t = tl_ti_from_cti(iface)->t;
  if (t->attr.typekind != TKIND_COCLASS && t->attr.typekind != TKIND_DISPATCH && t->attr.typekind != TKIND_INTERFACE) return TYPE_E_BADMODULEKIND;
  if (index >= t->nimpls) return TYPE_E_ELEMENTNOTFOUND;
  if (t->attr.typekind != TKIND_COCLASS) return TYPE_E_BADMODULEKIND;
  t->impls[index].flags = flags;
  return S_OK;
}
static HRESULT WINAPI tl_cti_SetAlignment(ICreateTypeInfo2 *iface, WORD align) { tl_ti_from_cti(iface)->t->attr.cbAlignment = align; return S_OK; }
static HRESULT WINAPI tl_cti_SetSchema(ICreateTypeInfo2 *iface, LPOLESTR schema) {
  HRESULT hr = S_OK;
  if (!schema) return E_INVALIDARG;
  tl_set_str(&tl_ti_from_cti(iface)->t->schema, schema, &hr);
  return hr;
}
static HRESULT WINAPI tl_cti_AddVarDesc(ICreateTypeInfo2 *iface, UINT index, VARDESC *vd) {
  TlType *t = tl_ti_from_cti(iface)->t;
  if (!vd) return E_INVALIDARG;
  if (index > t->nvars) return TYPE_E_ELEMENTNOTFOUND;
  HRESULT hr;
  VARDESC *copy = tl_vardesc_clone(vd, &hr);
  if (!copy) return hr;
  TlVar v;
  memset(&v, 0, sizeof(v));
  v.vd = *copy;
  oa_free(copy);
  if (v.vd.memid == MEMBERID_NIL) {
    MEMBERID id = 0x40000000 + (MEMBERID)index;
    for (BOOL again = TRUE; again;) {
      again = FALSE;
      for (UINT i = 0; i < t->nvars; i++) if (t->vars[i].vd.memid == id) { id++; again = TRUE; }
    }
    v.vd.memid = id;
  }
  TlVar *slot = tl_type_add_var(t);
  if (!slot) { tl_var_free(&v); return E_OUTOFMEMORY; }
  memmove(&t->vars[index + 1], &t->vars[index], (SIZE_T)(t->nvars - 1 - index) * sizeof(TlVar));
  t->vars[index] = v;
  return S_OK;
}
static HRESULT WINAPI tl_cti_SetFuncAndParamNames(ICreateTypeInfo2 *iface, UINT index, LPOLESTR *names, UINT count) {
  TlType *t = tl_ti_from_cti(iface)->t;
  if (!names) return E_INVALIDARG;
  if (index >= t->nfuncs || !count) return TYPE_E_ELEMENTNOTFOUND;
  TlFunc *f = &t->funcs[index];
  if (count > (UINT)f->fd.cParams + ((f->fd.invkind & (INVOKE_PROPERTYPUT | INVOKE_PROPERTYPUTREF)) ? 0 : 1)) return TYPE_E_ELEMENTNOTFOUND;
  if (!names[0]) return E_INVALIDARG;
  /* a function name may be used again only by the other kinds of accessor of its property; the parameter names must differ */
  for (UINT i = 0; i < t->nfuncs; i++)
    if (i != index && t->funcs[i].name && !tl_wcsicmp(t->funcs[i].name, names[0]) &&
        (t->funcs[i].fd.memid != f->fd.memid || t->funcs[i].fd.invkind == f->fd.invkind)) return TYPE_E_AMBIGUOUSNAME;
  for (UINT i = 1; i < count; i++) {
    if (!names[i]) continue;
    if (!tl_wcsicmp(names[i], names[0])) return TYPE_E_AMBIGUOUSNAME;
    for (UINT k = 1; k < i; k++) if (names[k] && !tl_wcsicmp(names[i], names[k])) return TYPE_E_AMBIGUOUSNAME;
  }
  if (!f->pnames && f->fd.cParams) { f->pnames = oa_calloc((SIZE_T)f->fd.cParams * sizeof(WCHAR *)); if (!f->pnames) return E_OUTOFMEMORY; }
  HRESULT hr = S_OK;
  tl_set_str(&f->name, names[0], &hr);
  for (UINT i = 1; SUCCEEDED(hr) && i < count; i++) if (names[i]) tl_set_str(&f->pnames[i - 1], names[i], &hr);
  return hr;
}
static HRESULT WINAPI tl_cti_SetVarName(ICreateTypeInfo2 *iface, UINT index, LPOLESTR name) {
  TlType *t = tl_ti_from_cti(iface)->t;
  if (!name) return E_INVALIDARG;
  if (index >= t->nvars) return TYPE_E_ELEMENTNOTFOUND;
  for (UINT i = 0; i < t->nvars; i++) if (i != index && t->vars[i].name && !tl_wcsicmp(t->vars[i].name, name)) return TYPE_E_AMBIGUOUSNAME;
  HRESULT hr = S_OK;
  tl_set_str(&t->vars[index].name, name, &hr);
  return hr;
}
static HRESULT WINAPI tl_cti_SetTypeDescAlias(ICreateTypeInfo2 *iface, TYPEDESC *alias) {
  TlType *t = tl_ti_from_cti(iface)->t;
  if (!alias) return E_INVALIDARG;
  if (t->attr.typekind != TKIND_ALIAS) return TYPE_E_BADMODULEKIND;
  TYPEDESC copy;
  HRESULT hr = tl_td_copy(&copy, alias, 0);
  if (FAILED(hr)) return hr;
  tl_td_free(&t->attr.tdescAlias);
  t->attr.tdescAlias = copy;
  ULONG size; WORD align;
  if (SUCCEEDED(tl_td_size(t->lib, &copy, tl_ptr_size(t->lib), FALSE, &size, &align, 0))) { t->attr.cbSizeInstance = size; t->attr.cbAlignment = align; }
  return S_OK;
}
static HRESULT WINAPI tl_cti_DefineFuncAsDllEntry(ICreateTypeInfo2 *iface, UINT index, LPOLESTR dll, LPOLESTR proc) {
  TlType *t = tl_ti_from_cti(iface)->t;
  if (!dll || !proc) return E_INVALIDARG;
  if (index >= t->nfuncs) return TYPE_E_ELEMENTNOTFOUND;
  if (t->attr.typekind != TKIND_MODULE) return TYPE_E_BADMODULEKIND;
  TlFunc *f = &t->funcs[index];
  HRESULT hr = S_OK;
  tl_set_str(&t->dllname, dll, &hr);
  if (FAILED(hr)) return hr;
  oa_free(f->dll_entry);
  f->dll_entry = NULL;
  f->has_ordinal = FALSE;
  if (proc[0] == '#') {
    DWORD v = 0;
    for (const WCHAR *p = proc + 1; *p; p++) { if (*p < '0' || *p > '9' || v > 0xffff) return E_INVALIDARG; v = v * 10 + (*p - '0'); }
    f->has_ordinal = TRUE;
    f->ordinal = (WORD)v;
  } else {
    f->dll_entry = tl_wcsdup(proc);
    if (!f->dll_entry) return E_OUTOFMEMORY;
  }
  f->fd.funckind = FUNC_STATIC;
  return S_OK;
}
static HRESULT WINAPI tl_cti_SetFuncDocString(ICreateTypeInfo2 *iface, UINT index, LPOLESTR doc) {
  TlType *t = tl_ti_from_cti(iface)->t;
  if (!doc) return E_INVALIDARG;
  if (index >= t->nfuncs) return TYPE_E_ELEMENTNOTFOUND;
  HRESULT hr = S_OK;
  tl_set_str(&t->funcs[index].doc, doc, &hr);
  return hr;
}
static HRESULT WINAPI tl_cti_SetVarDocString(ICreateTypeInfo2 *iface, UINT index, LPOLESTR doc) {
  TlType *t = tl_ti_from_cti(iface)->t;
  if (!doc) return E_INVALIDARG;
  if (index >= t->nvars) return TYPE_E_ELEMENTNOTFOUND;
  HRESULT hr = S_OK;
  tl_set_str(&t->vars[index].doc, doc, &hr);
  return hr;
}
static HRESULT WINAPI tl_cti_SetFuncHelpContext(ICreateTypeInfo2 *iface, UINT index, DWORD context) {
  TlType *t = tl_ti_from_cti(iface)->t;
  if (index >= t->nfuncs) return TYPE_E_ELEMENTNOTFOUND;
  t->funcs[index].help_context = context;
  return S_OK;
}
static HRESULT WINAPI tl_cti_SetVarHelpContext(ICreateTypeInfo2 *iface, UINT index, DWORD context) {
  TlType *t = tl_ti_from_cti(iface)->t;
  if (index >= t->nvars) return TYPE_E_ELEMENTNOTFOUND;
  t->vars[index].help_context = context;
  return S_OK;
}
static HRESULT WINAPI tl_cti_SetMops(ICreateTypeInfo2 *iface, UINT index, BSTR mops) { (void)iface; (void)index; (void)mops; return E_NOTIMPL; }
static HRESULT WINAPI tl_cti_SetTypeIdldesc(ICreateTypeInfo2 *iface, IDLDESC *idl) {
  if (!idl) return E_INVALIDARG;
  tl_ti_from_cti(iface)->t->attr.idldescType = *idl;
  return S_OK;
}
static HRESULT tl_layout(TlType *t);
static HRESULT WINAPI tl_cti_LayOut(ICreateTypeInfo2 *iface) { return tl_layout(tl_ti_from_cti(iface)->t); }

static void tl_func_remove(TlType *t, UINT index) {
  tl_func_free(&t->funcs[index]);
  memmove(&t->funcs[index], &t->funcs[index + 1], (SIZE_T)(t->nfuncs - 1 - index) * sizeof(TlFunc));
  t->nfuncs--;
  t->attr.cFuncs = (WORD)t->nfuncs;
  tl_vft_update(t);
}
static HRESULT WINAPI tl_cti_DeleteFuncDesc(ICreateTypeInfo2 *iface, UINT index) {
  TlType *t = tl_ti_from_cti(iface)->t;
  if (index >= t->nfuncs) return TYPE_E_ELEMENTNOTFOUND;
  tl_func_remove(t, index);
  return S_OK;
}
static HRESULT WINAPI tl_cti_DeleteFuncDescByMemId(ICreateTypeInfo2 *iface, MEMBERID id, INVOKEKIND kind) {
  TlType *t = tl_ti_from_cti(iface)->t;
  for (UINT i = 0; i < t->nfuncs; i++)
    if (t->funcs[i].fd.memid == id && (t->funcs[i].fd.invkind & kind)) { tl_func_remove(t, i); return S_OK; }
  return TYPE_E_ELEMENTNOTFOUND;
}
static HRESULT tl_var_remove(TlType *t, UINT index) {
  tl_var_free(&t->vars[index]);
  memmove(&t->vars[index], &t->vars[index + 1], (SIZE_T)(t->nvars - 1 - index) * sizeof(TlVar));
  t->nvars--;
  t->attr.cVars = (WORD)t->nvars;
  return S_OK;
}
static HRESULT WINAPI tl_cti_DeleteVarDesc(ICreateTypeInfo2 *iface, UINT index) {
  TlType *t = tl_ti_from_cti(iface)->t;
  if (index >= t->nvars) return TYPE_E_ELEMENTNOTFOUND;
  return tl_var_remove(t, index);
}
static HRESULT WINAPI tl_cti_DeleteVarDescByMemId(ICreateTypeInfo2 *iface, MEMBERID id) {
  TlType *t = tl_ti_from_cti(iface)->t;
  for (UINT i = 0; i < t->nvars; i++) if (t->vars[i].vd.memid == id) return tl_var_remove(t, i);
  return TYPE_E_ELEMENTNOTFOUND;
}
static HRESULT WINAPI tl_cti_DeleteImplType(ICreateTypeInfo2 *iface, UINT index) {
  TlType *t = tl_ti_from_cti(iface)->t;
  if (index >= t->nimpls) return TYPE_E_ELEMENTNOTFOUND;
  tl_cust_free(&t->impls[index].cust);
  memmove(&t->impls[index], &t->impls[index + 1], (SIZE_T)(t->nimpls - 1 - index) * sizeof(TlImpl));
  t->nimpls--;
  t->attr.cImplTypes = (WORD)t->nimpls;
  return S_OK;
}
static HRESULT WINAPI tl_cti_SetCustData(ICreateTypeInfo2 *iface, REFGUID guid, VARIANT *value) { return tl_set_cust(&tl_ti_from_cti(iface)->t->cust, guid, value); }
static HRESULT WINAPI tl_cti_SetFuncCustData(ICreateTypeInfo2 *iface, UINT index, REFGUID guid, VARIANT *value) {
  TlType *t = tl_ti_from_cti(iface)->t;
  if (index >= t->nfuncs) return TYPE_E_ELEMENTNOTFOUND;
  return tl_set_cust(&t->funcs[index].cust, guid, value);
}
static HRESULT WINAPI tl_cti_SetParamCustData(ICreateTypeInfo2 *iface, UINT index, UINT param, REFGUID guid, VARIANT *value) {
  TlType *t = tl_ti_from_cti(iface)->t;
  if (index >= t->nfuncs) return TYPE_E_ELEMENTNOTFOUND;
  TlFunc *f = &t->funcs[index];
  if (param >= (UINT)f->fd.cParams) return TYPE_E_ELEMENTNOTFOUND;
  if (!f->pcust) { f->pcust = oa_calloc((SIZE_T)f->fd.cParams * sizeof(TlCustList)); if (!f->pcust) return E_OUTOFMEMORY; }
  return tl_set_cust(&f->pcust[param], guid, value);
}
static HRESULT WINAPI tl_cti_SetVarCustData(ICreateTypeInfo2 *iface, UINT index, REFGUID guid, VARIANT *value) {
  TlType *t = tl_ti_from_cti(iface)->t;
  if (index >= t->nvars) return TYPE_E_ELEMENTNOTFOUND;
  return tl_set_cust(&t->vars[index].cust, guid, value);
}
static HRESULT WINAPI tl_cti_SetImplTypeCustData(ICreateTypeInfo2 *iface, UINT index, REFGUID guid, VARIANT *value) {
  TlType *t = tl_ti_from_cti(iface)->t;
  if (index >= t->nimpls) return TYPE_E_ELEMENTNOTFOUND;
  return tl_set_cust(&t->impls[index].cust, guid, value);
}
static HRESULT WINAPI tl_cti_SetHelpStringContext(ICreateTypeInfo2 *iface, ULONG context) { tl_ti_from_cti(iface)->t->help_string_context = context; return S_OK; }
static HRESULT WINAPI tl_cti_SetFuncHelpStringContext(ICreateTypeInfo2 *iface, UINT index, ULONG context) {
  TlType *t = tl_ti_from_cti(iface)->t;
  if (index >= t->nfuncs) return TYPE_E_ELEMENTNOTFOUND;
  t->funcs[index].help_string_context = context;
  return S_OK;
}
static HRESULT WINAPI tl_cti_SetVarHelpStringContext(ICreateTypeInfo2 *iface, UINT index, ULONG context) {
  TlType *t = tl_ti_from_cti(iface)->t;
  if (index >= t->nvars) return TYPE_E_ELEMENTNOTFOUND;
  t->vars[index].help_string_context = context;
  return S_OK;
}
static HRESULT WINAPI tl_cti_Invalidate(ICreateTypeInfo2 *iface) { (void)iface; return S_OK; }
static HRESULT WINAPI tl_cti_SetName(ICreateTypeInfo2 *iface, LPOLESTR name) {
  TlType *t = tl_ti_from_cti(iface)->t;
  if (!name) return E_INVALIDARG;
  HRESULT hr = S_OK;
  tl_set_str(&t->name, name, &hr);
  return hr;
}
static ICreateTypeInfo2Vtbl tl_cti_vtbl = {
  tl_cti_QueryInterface, tl_cti_AddRef, tl_cti_Release, tl_cti_SetGuid, tl_cti_SetTypeFlags, tl_cti_SetDocString, tl_cti_SetHelpContext, tl_cti_SetVersion,
  tl_cti_AddRefTypeInfo, tl_cti_AddFuncDesc, tl_cti_AddImplType, tl_cti_SetImplTypeFlags, tl_cti_SetAlignment, tl_cti_SetSchema, tl_cti_AddVarDesc,
  tl_cti_SetFuncAndParamNames, tl_cti_SetVarName, tl_cti_SetTypeDescAlias, tl_cti_DefineFuncAsDllEntry, tl_cti_SetFuncDocString, tl_cti_SetVarDocString,
  tl_cti_SetFuncHelpContext, tl_cti_SetVarHelpContext, tl_cti_SetMops, tl_cti_SetTypeIdldesc, tl_cti_LayOut, tl_cti_DeleteFuncDesc, tl_cti_DeleteFuncDescByMemId,
  tl_cti_DeleteVarDesc, tl_cti_DeleteVarDescByMemId, tl_cti_DeleteImplType, tl_cti_SetCustData, tl_cti_SetFuncCustData, tl_cti_SetParamCustData,
  tl_cti_SetVarCustData, tl_cti_SetImplTypeCustData, tl_cti_SetHelpStringContext, tl_cti_SetFuncHelpStringContext, tl_cti_SetVarHelpStringContext, tl_cti_Invalidate,
  tl_cti_SetName,
};

/* LayOut: the sizes and offsets of a type. */
static HRESULT tl_layout(TlType *t) {
  const UINT ptr = tl_ptr_size(t->lib);
  if (t->attr.typekind == TKIND_INTERFACE || t->attr.typekind == TKIND_DISPATCH) {
    UINT slots, levels;
    tl_base_info(t, &slots, &levels);
    UINT size = slots * ptr;
    for (UINT i = 0; i < t->nfuncs; i++) {
      const UINT end = (UINT)(unsigned short)t->funcs[i].fd.oVft + ptr;
      if (end > size) size = end;
    }
    if (t->attr.typekind == TKIND_INTERFACE) t->attr.cbSizeVft = (WORD)size;
  }
  t->laid_out = TRUE;
  return S_OK;
}

/* ---- CreateTypeLib2 ------------------------------------------------------------------------------------------------------------------------------------- */
EXPORT HRESULT WINAPI CreateTypeLib2(SYSKIND syskind, LPCOLESTR file, ICreateTypeLib2 **out) {
  if (!out) return E_INVALIDARG;
  if (!file) return E_INVALIDARG;
  if (syskind != SYS_WIN16 && syskind != SYS_WIN32 && syskind != SYS_WIN64 && syskind != SYS_MAC) return E_INVALIDARG;
  TlLib *lib = tl_lib_new();
  if (!lib) return E_OUTOFMEMORY;
  lib->created = TRUE;
  lib->syskind = syskind;
  lib->major = 0;
  lib->flags = 0;
  lib->file_lcid = 0x409;
  lib->lcid = LOCALE_NEUTRAL;
  lib->path = tl_wcsdup(file);
  if (!lib->path) { tl_lib_free(lib); return E_OUTOFMEMORY; }
  *out = &lib->ICreateTypeLib2_iface;
  return S_OK;
}
EXPORT HRESULT WINAPI OACreateTypeLib2(SYSKIND syskind, LPCOLESTR file, ICreateTypeLib2 **out) { return CreateTypeLib2(syskind, file, out); }
EXPORT HRESULT WINAPI CreateTypeLib(SYSKIND syskind, LPCOLESTR file, ICreateTypeLib **out) { return CreateTypeLib2(syskind, file, (ICreateTypeLib2 **)out); }

#include "typelib_write.inc"
