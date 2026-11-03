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

/* ITypeComp: binding a name to a member (of a typeinfo) or to a type or a global member (of a library). */

static void tl_bind_none(ITypeInfo **info, DESCKIND *kind, BINDPTR *bind) { *info = NULL; *kind = DESCKIND_NONE; bind->lptcomp = NULL; }

/* The name in one typeinfo (and its bases). TYPE_E_TYPEMISMATCH when a function has the name but not the invoke kind. */
static HRESULT tl_ti_bind(TlTi *ti, const WCHAR *name, WORD flags, ITypeInfo **info, DESCKIND *kind, BINDPTR *bind, UINT depth) {
  tl_bind_none(info, kind, bind);
  const TlType *t = ti->t;
  BOOL mismatch = FALSE;
  const UINT inherited = tl_ti_inherited_funcs(ti);
  for (UINT i = 0; i < t->nfuncs; i++) {
    const TlFunc *f = &t->funcs[i];
    if (!f->name || tl_wcsicmp(f->name, name)) continue;
    if (flags && !(f->fd.invkind & flags) && !(f->fd.invkind == INVOKE_FUNC && (flags & INVOKE_FUNC))) { mismatch = TRUE; continue; }
    FUNCDESC *fd;
    HRESULT hr = tl_ti_func_desc(ti, inherited + i, &fd);
    if (FAILED(hr)) return hr;
    bind->lpfuncdesc = fd;
    *kind = DESCKIND_FUNCDESC;
    *info = (ITypeInfo *)&ti->ITypeInfo2_iface;
    ITypeInfo2_AddRef(&ti->ITypeInfo2_iface);
    return S_OK;
  }
  for (UINT i = 0; i < t->nvars; i++) {
    if (!t->vars[i].name || tl_wcsicmp(t->vars[i].name, name)) continue;
    if (flags && !(flags & (INVOKE_PROPERTYGET | INVOKE_PROPERTYPUT | INVOKE_PROPERTYPUTREF))) { mismatch = TRUE; continue; }
    HRESULT hr;
    VARDESC *vd = tl_vardesc_clone(&t->vars[i].vd, &hr);
    if (!vd) return hr;
    bind->lpvardesc = vd;
    *kind = DESCKIND_VARDESC;
    *info = (ITypeInfo *)&ti->ITypeInfo2_iface;
    ITypeInfo2_AddRef(&ti->ITypeInfo2_iface);
    return S_OK;
  }
  if (inherited) {
    /* the functions of the bases, which this typeinfo shows as its own */
    ITypeInfo *chain[TL_MAX_BASES];
    const UINT nchain = tl_ti_chain(ti, chain);
    UINT offset = 0;
    for (UINT k = 0; k < nchain; k++) {
      TYPEATTR *ta;
      if (FAILED(ITypeInfo_GetTypeAttr(chain[k], &ta))) continue;
      const UINT nf = ta->cFuncs;
      ITypeInfo_ReleaseTypeAttr(chain[k], ta);
      for (UINT j = 0; j < nf; j++) {
        FUNCDESC *fd;
        if (FAILED(ITypeInfo_GetFuncDesc(chain[k], j, &fd))) continue;
        BSTR fname = NULL;
        UINT got = 0;
        const MEMBERID id = fd->memid;
        ITypeInfo_ReleaseFuncDesc(chain[k], fd);
        if (SUCCEEDED(ITypeInfo_GetNames(chain[k], id, &fname, 1, &got)) && got && fname && !tl_wcsicmp(fname, name)) {
          SysFreeString(fname);
          tl_chain_release(chain, nchain);
          FUNCDESC *own;
          HRESULT hr = tl_ti_func_desc(ti, offset + j, &own);
          if (FAILED(hr)) return hr;
          if (flags && !(own->invkind & flags)) { tl_funcdesc_release(own); mismatch = TRUE; goto done_chain; }
          bind->lpfuncdesc = own;
          *kind = DESCKIND_FUNCDESC;
          *info = (ITypeInfo *)&ti->ITypeInfo2_iface;
          ITypeInfo2_AddRef(&ti->ITypeInfo2_iface);
          return S_OK;
        }
        SysFreeString(fname);
      }
      offset += nf;
    }
    tl_chain_release(chain, nchain);
  }
done_chain:
  if (!inherited && depth < TL_MAX_BASES && (t->attr.typekind == TKIND_INTERFACE || t->attr.typekind == TKIND_DISPATCH)) {
    ITypeInfo *base = tl_ti_base(ti);
    if (base) {
      ITypeComp *bc = NULL;
      HRESULT hr = ITypeInfo_GetTypeComp(base, &bc);
      if (SUCCEEDED(hr)) {
        hr = ITypeComp_Bind(bc, (LPOLESTR)name, 0, flags, info, kind, bind);
        ITypeComp_Release(bc);
      }
      ITypeInfo_Release(base);
      if (hr == TYPE_E_TYPEMISMATCH) mismatch = TRUE;
      else if (FAILED(hr) || *kind != DESCKIND_NONE) return hr;
    }
  }
  return mismatch ? TYPE_E_TYPEMISMATCH : S_OK;
}

static HRESULT WINAPI tl_ticomp_QueryInterface(ITypeComp *iface, REFIID riid, void **out) { return ITypeInfo2_QueryInterface(&tl_ti_from_comp(iface)->ITypeInfo2_iface, riid, out); }
static ULONG WINAPI tl_ticomp_AddRef(ITypeComp *iface) { return ITypeInfo2_AddRef(&tl_ti_from_comp(iface)->ITypeInfo2_iface); }
static ULONG WINAPI tl_ticomp_Release(ITypeComp *iface) { return ITypeInfo2_Release(&tl_ti_from_comp(iface)->ITypeInfo2_iface); }
static HRESULT WINAPI tl_ticomp_Bind(ITypeComp *iface, LPOLESTR name, ULONG hash, WORD flags, ITypeInfo **info, DESCKIND *kind, BINDPTR *bind) {
  (void)hash;
  if (!name || !info || !kind || !bind) return E_INVALIDARG;
  return tl_ti_bind(tl_ti_from_comp(iface), name, flags, info, kind, bind, 0);
}
static HRESULT WINAPI tl_ticomp_BindType(ITypeComp *iface, LPOLESTR name, ULONG hash, ITypeInfo **info, ITypeComp **comp) {
  (void)iface; (void)hash;
  if (!name || !info || !comp) return E_INVALIDARG;
  *info = NULL;
  *comp = NULL;
  return S_OK;
}
static ITypeCompVtbl tl_ticomp_vtbl = { tl_ticomp_QueryInterface, tl_ticomp_AddRef, tl_ticomp_Release, tl_ticomp_Bind, tl_ticomp_BindType };

static HRESULT WINAPI tl_libcomp_QueryInterface(ITypeComp *iface, REFIID riid, void **out) { return ITypeLib2_QueryInterface(&tl_lib_from_comp(iface)->ITypeLib2_iface, riid, out); }
static ULONG WINAPI tl_libcomp_AddRef(ITypeComp *iface) { return ITypeLib2_AddRef(&tl_lib_from_comp(iface)->ITypeLib2_iface); }
static ULONG WINAPI tl_libcomp_Release(ITypeComp *iface) { return ITypeLib2_Release(&tl_lib_from_comp(iface)->ITypeLib2_iface); }
static HRESULT WINAPI tl_libcomp_Bind(ITypeComp *iface, LPOLESTR name, ULONG hash, WORD flags, ITypeInfo **info, DESCKIND *kind, BINDPTR *bind) {
  TlLib *lib = tl_lib_from_comp(iface);
  (void)hash;
  if (!name || !info || !kind || !bind) return E_INVALIDARG;
  tl_bind_none(info, kind, bind);
  BOOL mismatch = FALSE;
  /* a module or an enumeration of that name is a scope of its own */
  for (UINT i = 0; i < lib->ntypes; i++) {
    TlType *t = lib->types[i];
    if (t->name && !tl_wcsicmp(t->name, name) && (t->attr.typekind == TKIND_MODULE || t->attr.typekind == TKIND_ENUM)) {
      *kind = DESCKIND_TYPECOMP;
      bind->lptcomp = &t->ti.ITypeComp_iface;
      ITypeInfo2_AddRef(&t->ti.ITypeInfo2_iface);
      return S_OK;
    }
  }
  /* then the global members of the modules and enumerations */
  for (UINT i = 0; i < lib->ntypes; i++) {
    TlType *t = lib->types[i];
    if (t->attr.typekind != TKIND_MODULE && t->attr.typekind != TKIND_ENUM) continue;
    HRESULT hr = tl_ti_bind(&t->ti, name, flags, info, kind, bind, 0);
    if (hr == TYPE_E_TYPEMISMATCH) mismatch = TRUE;
    else if (FAILED(hr) || *kind != DESCKIND_NONE) return hr;
  }
  return mismatch ? TYPE_E_TYPEMISMATCH : S_OK;
}
static HRESULT WINAPI tl_libcomp_BindType(ITypeComp *iface, LPOLESTR name, ULONG hash, ITypeInfo **info, ITypeComp **comp) {
  TlLib *lib = tl_lib_from_comp(iface);
  (void)hash;
  if (!name || !info || !comp) return E_INVALIDARG;
  *info = NULL;
  *comp = NULL;
  for (UINT i = 0; i < lib->ntypes; i++)
    if (lib->types[i]->name && !tl_wcsicmp(lib->types[i]->name, name)) {
      ITypeInfo2_AddRef(&lib->types[i]->ti.ITypeInfo2_iface);
      *info = (ITypeInfo *)&lib->types[i]->ti.ITypeInfo2_iface;
      break;
    }
  return S_OK;
}
static ITypeCompVtbl tl_libcomp_vtbl = { tl_libcomp_QueryInterface, tl_libcomp_AddRef, tl_libcomp_Release, tl_libcomp_Bind, tl_libcomp_BindType };
