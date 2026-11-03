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

/* ITypeInfo::Invoke: calls a member of an object through its virtual table (interfaces and the interface half of dual interfaces; the arguments come from a DISPPARAMS and
 * are converted to the parameter types; DispCallFunc makes the call) or, for a dispinterface, through IDispatch::Invoke. */

#define TL_MAX_INVOKE_PARAMS 64

/* The VARTYPE a parameter of type `td` is passed as, resolving aliases and enumerations; *pointee is set when the parameter is a pointer to a value of the type
 * returned. */
static HRESULT tl_resolve_type(ITypeInfo *ti, const TYPEDESC *td, VARTYPE *vt, BOOL *by_pointer, UINT depth) {
  if (depth > 16) return DISP_E_BADVARTYPE;
  switch (td->vt) {
    case VT_PTR: {
      if (!td->lptdesc) return DISP_E_BADVARTYPE;
      BOOL inner_ptr = FALSE;
      HRESULT hr = tl_resolve_type(ti, td->lptdesc, vt, &inner_ptr, depth + 1);
      if (FAILED(hr)) return hr;
      if (inner_ptr || *vt == VT_VARIANT || (*vt & VT_BYREF)) { *by_pointer = TRUE; return S_OK; }
      *by_pointer = TRUE;
      return S_OK;
    }
    case VT_USERDEFINED: {
      ITypeInfo *ref;
      TYPEATTR *ta;
      HRESULT hr = ITypeInfo_GetRefTypeInfo(ti, td->hreftype, &ref);
      if (FAILED(hr)) return hr;
      hr = ITypeInfo_GetTypeAttr(ref, &ta);
      if (SUCCEEDED(hr)) {
        switch (ta->typekind) {
          case TKIND_ALIAS: hr = tl_resolve_type(ref, &ta->tdescAlias, vt, by_pointer, depth + 1); break;
          case TKIND_ENUM: *vt = VT_I4; *by_pointer = FALSE; break;
          case TKIND_INTERFACE: case TKIND_COCLASS: *vt = VT_UNKNOWN; *by_pointer = FALSE; break;
          case TKIND_DISPATCH: *vt = VT_DISPATCH; *by_pointer = FALSE; break;
          default: hr = DISP_E_BADVARTYPE; break;
        }
        ITypeInfo_ReleaseTypeAttr(ref, ta);
      }
      ITypeInfo_Release(ref);
      return hr;
    }
    case VT_SAFEARRAY: {
      VARTYPE inner;
      BOOL p = FALSE;
      if (!td->lptdesc) return DISP_E_BADVARTYPE;
      HRESULT hr = tl_resolve_type(ti, td->lptdesc, &inner, &p, depth + 1);
      if (FAILED(hr)) return hr;
      *vt = VT_ARRAY | inner;
      *by_pointer = FALSE;
      return S_OK;
    }
    case VT_CARRAY: return DISP_E_BADVARTYPE;
    case VT_INT: *vt = VT_I4; *by_pointer = FALSE; return S_OK;
    case VT_UINT: *vt = VT_UI4; *by_pointer = FALSE; return S_OK;
    case VT_HRESULT: *vt = VT_ERROR; *by_pointer = FALSE; return S_OK;
    case VT_LPSTR: case VT_LPWSTR: *vt = (VARTYPE)td->vt; *by_pointer = FALSE; return S_OK;
    default: *vt = td->vt; *by_pointer = FALSE; return S_OK;
  }
}
/* Pointer depth: how many levels of VT_PTR lie in front of the type */
static const TYPEDESC *tl_strip_ptr(const TYPEDESC *td, UINT *levels) {
  *levels = 0;
  while (td->vt == VT_PTR && td->lptdesc) { td = td->lptdesc; (*levels)++; }
  return td;
}
/* Size of storage for a value of the resolved type */
static BOOL tl_is_pointer_vt(VARTYPE vt) { return vt == VT_BSTR || vt == VT_UNKNOWN || vt == VT_DISPATCH || vt == VT_LPSTR || vt == VT_LPWSTR || (vt & VT_ARRAY) || vt == VT_PTR || vt == VT_BYREF; }

typedef struct {
  VARIANT arg;                 /* the value passed (converted) */
  VARIANT store;               /* storage behind a pointer parameter */
  VARTYPE pass_vt;             /* type for DispCallFunc */
  VARTYPE store_vt;            /* type of `store` when the parameter is a pointer */
  BOOL indirect;               /* a pointer to `store` (or to the caller's data) is passed */
  BOOL copy_back;
  VARIANT *dest;               /* the caller's argument to copy an [out] value back to */
  BOOL supplied;
} TlInvArg;

static HRESULT tl_funcs_find_invoke(TlTi *ti, MEMBERID id, WORD flags, TlFunc **out) {
  *out = NULL;
  TlFunc *any = NULL;
  for (UINT i = 0; i < ti->t->nfuncs; i++) {
    TlFunc *f = &ti->t->funcs[i];
    if (f->fd.memid != id) continue;
    any = f;
    if ((flags & DISPATCH_METHOD) && f->fd.invkind == INVOKE_FUNC) { *out = f; return S_OK; }
    if ((flags & DISPATCH_PROPERTYGET) && f->fd.invkind == INVOKE_PROPERTYGET) { *out = f; return S_OK; }
    if ((flags & DISPATCH_PROPERTYPUT) && f->fd.invkind == INVOKE_PROPERTYPUT) { *out = f; return S_OK; }
    if ((flags & DISPATCH_PROPERTYPUTREF) && f->fd.invkind == INVOKE_PROPERTYPUTREF) { *out = f; return S_OK; }
  }
  return any ? DISP_E_MEMBERNOTFOUND : TYPE_E_ELEMENTNOTFOUND;
}

static HRESULT WINAPI tl_ti_Invoke(ITypeInfo2 *iface, PVOID instance, MEMBERID id, WORD flags, DISPPARAMS *params, VARIANT *result, EXCEPINFO *excep, UINT *arg_err) {
  TlTi *self = tl_ti_from_ti(iface);
  UINT dummy_err;
  if (!arg_err) arg_err = &dummy_err;
  if (flags & ~(DISPATCH_METHOD | DISPATCH_PROPERTYGET | DISPATCH_PROPERTYPUT | DISPATCH_PROPERTYPUTREF)) return DISP_E_BADPARAMCOUNT;
  if (excep) memset(excep, 0, sizeof(*excep));
  TlTi *call = tl_ti_is_dual(self) && self->t->attr.typekind == TKIND_DISPATCH ? &self->t->alt_ti : self;
  for (UINT i = 0; i < call->t->nfuncs; i++)
    if (call->t->funcs[i].fd.memid == id && (call->t->funcs[i].fd.wFuncFlags & FUNCFLAG_FRESTRICTED)) return DISP_E_MEMBERNOTFOUND;
  if (!params) return E_INVALIDARG;
  if (params->cNamedArgs > params->cArgs) return E_INVALIDARG;
  if (!instance) return E_INVALIDARG;
  if (call->t->attr.typekind == TKIND_DISPATCH && !call->alt) {
    /* a dispinterface: the object's own IDispatch */
    IDispatch *disp = instance;
    return IDispatch_Invoke(disp, id, &tl_zero_guid, LOCALE_USER_DEFAULT, flags, params, result, excep, arg_err);
  }
  if (call->t->attr.typekind != TKIND_INTERFACE && !call->alt) return TYPE_E_WRONGTYPEKIND;
  TlFunc *f;
  HRESULT hr = tl_funcs_find_invoke(call, id, flags, &f);
  if (hr == DISP_E_MEMBERNOTFOUND) {
    /* the member exists with another kind: the argument count is judged against the first function with the id before the kind is */
    for (UINT i = 0; i < call->t->nfuncs; i++) {
      const FUNCDESC *g = &call->t->funcs[i].fd;
      if (g->memid != id) continue;
      UINT m = 0;
      BOOL rv = FALSE;
      for (SHORT p = 0; p < g->cParams; p++) {
        const USHORT pf = g->lprgelemdescParam[p].paramdesc.wParamFlags;
        if (pf & PARAMFLAG_FRETVAL) rv = TRUE; else if (!(pf & PARAMFLAG_FLCID)) m++;
      }
      const UINT c = params->cArgs;
      (void)rv;
      if (c == m + 1 || m == 0) return DISP_E_BADPARAMCOUNT;
      break;
    }
    return DISP_E_MEMBERNOTFOUND;
  }
  if (hr == TYPE_E_ELEMENTNOTFOUND) {
    ITypeInfo *base = tl_ti_base(call);
    if (!base) return DISP_E_MEMBERNOTFOUND;
    hr = ITypeInfo_Invoke(base, instance, id, flags, params, result, excep, arg_err);
    ITypeInfo_Release(base);
    return hr;
  }
  if (FAILED(hr)) return hr;
  const FUNCDESC *fd = &f->fd;
  if (fd->funckind != FUNC_PUREVIRTUAL && fd->funckind != FUNC_VIRTUAL) return E_NOTIMPL;
  const UINT np = (UINT)fd->cParams;
  if (np > TL_MAX_INVOKE_PARAMS) return DISP_E_BADPARAMCOUNT;
  /* which parameter each supplied argument is for */
  TlInvArg *a = oa_calloc((np + 1) * sizeof(TlInvArg));
  VARIANT *argv[TL_MAX_INVOKE_PARAMS + 2];
  VARTYPE types[TL_MAX_INVOKE_PARAMS + 2];
  VARIANT retstore;
  VariantInit(&retstore);
  if (!a) return E_OUTOFMEMORY;
  INT retval_index = -1;
  UINT consumable = 0;
  for (UINT p = 0; p < np; p++) {
    const USHORT pf = fd->lprgelemdescParam[p].paramdesc.wParamFlags;
    if (pf & PARAMFLAG_FRETVAL) retval_index = (INT)p;
    else if (!(pf & PARAMFLAG_FLCID)) consumable++;
  }
  /* the arguments: the named ones first (rgvarg[0..cNamedArgs)), then the rest in reverse order */
  UINT first_unnamed = 0;
  VARIANT *positional[TL_MAX_INVOKE_PARAMS + 2];
  INT named_param[TL_MAX_INVOKE_PARAMS + 2];
  UINT named = 0;
  if (params->cNamedArgs > params->cArgs || (params->cArgs && !params->rgvarg) || params->cArgs > TL_MAX_INVOKE_PARAMS) { oa_free(a); return DISP_E_BADPARAMCOUNT; }
  UINT counted = params->cArgs;
  for (UINT k = 0; k < params->cNamedArgs; k++) {
    const DISPID d = params->rgdispidNamedArgs[k];
    if (d == DISPID_THIS) { oa_free(a); return DISP_E_BADPARAMCOUNT; }
    if (d == DISPID_PROPERTYPUT) {
      /* the value of a property put: the last parameter */
      INT target = -1;
      for (INT p = (INT)np - 1; p >= 0; p--) if (!(fd->lprgelemdescParam[p].paramdesc.wParamFlags & (PARAMFLAG_FRETVAL | PARAMFLAG_FLCID))) { target = p; break; }
      if (target < 0) { oa_free(a); return DISP_E_BADPARAMCOUNT; }
      named_param[named] = target;
    } else {
      if (d < 0 || (UINT)d >= np) { oa_free(a); *arg_err = k; return DISP_E_MEMBERNOTFOUND; }
      named_param[named] = d;
    }
    positional[named] = &params->rgvarg[k];
    named++;
  }
  (void)first_unnamed;
  /* more arguments than parameters are for the default member of the object a property get returns (obj.prop(7) is obj.prop.Item(7)) */
  UINT extra = counted > consumable ? counted - consumable : 0;
  if (extra) {
    BOOL object = FALSE;
    VARTYPE ovt = VT_EMPTY;
    BOOL op = FALSE;
    if (retval_index >= 0) {
      UINT lv;
      const TYPEDESC *b = tl_strip_ptr(&fd->lprgelemdescParam[retval_index].tdesc, &lv);
      object = lv == 2 && SUCCEEDED(tl_resolve_type((ITypeInfo *)&call->ITypeInfo2_iface, b, &ovt, &op, 0)) && (ovt == VT_UNKNOWN || ovt == VT_DISPATCH);
    } else if (fd->elemdescFunc.tdesc.vt != VT_VOID && fd->elemdescFunc.tdesc.vt != VT_HRESULT) {
      UINT lv;
      const TYPEDESC *b = tl_strip_ptr(&fd->elemdescFunc.tdesc, &lv);
      object = lv == 0 && SUCCEEDED(tl_resolve_type((ITypeInfo *)&call->ITypeInfo2_iface, b, &ovt, &op, 0)) && (ovt == VT_UNKNOWN || ovt == VT_DISPATCH);
    }
    if (!object || !(flags & DISPATCH_PROPERTYGET) || params->cNamedArgs) { oa_free(a); return DISP_E_BADPARAMCOUNT; }
  }
  const UINT unnamed_count = counted - named - extra;
  const UINT required = fd->cParamsOpt == -1 ? 0 : (consumable > (UINT)fd->cParamsOpt ? consumable - (UINT)fd->cParamsOpt : 0);
  if (counted - extra < required) { oa_free(a); return DISP_E_BADPARAMCOUNT; }
  /* assign: named arguments to their parameters, the unnamed ones to the free parameters in order (the last of rgvarg is the first parameter) */
  VARIANT *assigned[TL_MAX_INVOKE_PARAMS + 2];
  memset(assigned, 0, sizeof(assigned));
  for (UINT k = 0; k < named; k++) assigned[named_param[k]] = positional[k];
  {
    UINT next = 0;
    for (UINT u = 0; u < unnamed_count; u++) {
      VARIANT *arg = &params->rgvarg[params->cArgs - 1 - u];
      while (next < np && (assigned[next] || (fd->lprgelemdescParam[next].paramdesc.wParamFlags & (PARAMFLAG_FRETVAL | PARAMFLAG_FLCID)))) next++;
      if (next >= np) { oa_free(a); return DISP_E_BADPARAMCOUNT; }
      assigned[next++] = arg;
    }
  }
  /* build the call */
  for (UINT p = 0; p < np && SUCCEEDED(hr); p++) {
    const ELEMDESC *e = &fd->lprgelemdescParam[p];
    TlInvArg *ia = &a[p];
    VariantInit(&ia->arg);
    VariantInit(&ia->store);
    UINT levels;
    const TYPEDESC *base = tl_strip_ptr(&e->tdesc, &levels);
    VARTYPE vt = VT_EMPTY;
    BOOL ptr = FALSE;
    hr = tl_resolve_type((ITypeInfo *)&call->ITypeInfo2_iface, base, &vt, &ptr, 0);
    if (FAILED(hr)) break;
    ia->store_vt = vt;
    const USHORT pf = e->paramdesc.wParamFlags;
    VARIANT *src = assigned[p];
    if (pf & PARAMFLAG_FLCID) {
      V_UI8(&ia->arg) = GetUserDefaultLCID();
      ia->pass_vt = VT_I4;
      continue;
    }
    if (pf & PARAMFLAG_FRETVAL) src = NULL;
    ia->supplied = src != NULL;
    if (levels == 0) {
      ia->pass_vt = vt == VT_VARIANT ? VT_VARIANT : vt;
      if (src) {
        if (vt == VT_VARIANT) { hr = VariantCopyInd(&ia->arg, src); if (hr == DISP_E_BADVARTYPE && (V_VT(src) & VT_BYREF)) hr = VariantCopy(&ia->arg, src); }
        else if (V_VT(src) == vt) { hr = VariantCopy(&ia->arg, src); }
        else {
          VARIANT tmp;
          VariantInit(&tmp);
          if (vt == VT_UNKNOWN && V_VT(src) == VT_DISPATCH) { V_VT(&tmp) = VT_UNKNOWN; V_UNKNOWN(&tmp) = (IUnknown *)V_DISPATCH(src); if (V_UNKNOWN(&tmp)) IUnknown_AddRef(V_UNKNOWN(&tmp)); hr = S_OK; }
          else hr = VariantChangeType(&tmp, src, 0, vt);
          if (SUCCEEDED(hr)) ia->arg = tmp;
        }
        if (FAILED(hr)) *arg_err = (UINT)(src - params->rgvarg);
      } else if (pf & PARAMFLAG_FHASDEFAULT && e->paramdesc.pparamdescex) {
        hr = VariantCopy(&ia->arg, &e->paramdesc.pparamdescex->varDefaultValue);
        if (SUCCEEDED(hr) && vt != VT_VARIANT && V_VT(&ia->arg) != vt) hr = VariantChangeType(&ia->arg, &ia->arg, 0, vt);
      } else if (vt == VT_VARIANT) {
        V_VT(&ia->arg) = VT_ERROR;
        V_ERROR(&ia->arg) = DISP_E_PARAMNOTFOUND;
      } else {
        V_VT(&ia->arg) = vt;
      }
      if (SUCCEEDED(hr) && vt != VT_VARIANT && !(vt & VT_ARRAY) && V_VT(&ia->arg) != vt && V_VT(&ia->arg) != VT_EMPTY) hr = DISP_E_TYPEMISMATCH;
      continue;
    }
    /* a pointer: to the caller's data when the argument is a reference of the right type, else to a copy */
    ia->indirect = TRUE;
    ia->pass_vt = VT_PTR;
    ia->copy_back = (pf & PARAMFLAG_FOUT) != 0;
    ia->dest = src;
    if (levels > 1) {
      /* pointer to pointer: the storage holds the (converted) value, e.g. an out interface pointer */
      if (src && (V_VT(src) & VT_BYREF)) { V_UI8(&ia->arg) = (ULONGLONG)(ULONG_PTR)V_BYREF(src); ia->copy_back = FALSE; ia->indirect = FALSE; }
      else { V_UI8(&ia->arg) = (ULONGLONG)(ULONG_PTR)&ia->store; V_UI8(&ia->store) = 0; }
      continue;
    }
    if (src && (V_VT(src) & VT_BYREF) && (V_VT(src) & VT_TYPEMASK) == (vt & VT_TYPEMASK) && !(V_VT(src) & VT_ARRAY) == !(vt & VT_ARRAY)) {
      V_UI8(&ia->arg) = (ULONGLONG)(ULONG_PTR)V_BYREF(src);      /* (the caller's own storage) */
      ia->indirect = FALSE;
      ia->copy_back = FALSE;
      continue;
    }
    if (vt == VT_VARIANT) {
      if (src && (V_VT(src) & VT_BYREF) && (V_VT(src) & VT_TYPEMASK) == VT_VARIANT) { V_UI8(&ia->arg) = (ULONGLONG)(ULONG_PTR)V_VARIANTREF(src); ia->indirect = FALSE; ia->copy_back = FALSE; continue; }
      if (src) hr = VariantCopy(&ia->store, src);
      V_UI8(&ia->arg) = (ULONGLONG)(ULONG_PTR)&ia->store;
      continue;
    }
    if (src && (pf & PARAMFLAG_FIN)) {
      VARIANT tmp;
      VariantInit(&tmp);
      hr = (V_VT(src) == vt) ? VariantCopy(&tmp, src) : VariantChangeType(&tmp, src, 0, vt);
      if (FAILED(hr)) { *arg_err = (UINT)(src - params->rgvarg); break; }
      ia->store = tmp;
    } else {
      V_VT(&ia->store) = vt & ~VT_BYREF;
      V_UI8(&ia->store) = 0;
    }
    V_UI8(&ia->arg) = (ULONGLONG)(ULONG_PTR)&V_UI8(&ia->store);
    if (vt == VT_DECIMAL || vt == VT_CY) { /* (stored in the variant's own space) */ }
  }
  VARIANT callres;
  VariantInit(&callres);
  VARTYPE ret_vt = VT_EMPTY;
  BOOL have_ret = FALSE, ret_is_hresult = fd->elemdescFunc.tdesc.vt == VT_HRESULT;
  if (SUCCEEDED(hr)) {
    UINT nargs = 0;
    for (UINT p = 0; p < np; p++) {
      const USHORT pf = fd->lprgelemdescParam[p].paramdesc.wParamFlags;
      TlInvArg *ia = &a[p];
      if (pf & PARAMFLAG_FRETVAL) {
        /* the return value: storage for the pointee */
        UINT levels;
        const TYPEDESC *base = tl_strip_ptr(&fd->lprgelemdescParam[p].tdesc, &levels);
        VARTYPE vt; BOOL ptr;
        if (FAILED(tl_resolve_type((ITypeInfo *)&call->ITypeInfo2_iface, base, &vt, &ptr, 0))) vt = VT_EMPTY;
        memset(&retstore, 0, sizeof(retstore));
        if (levels == 1 && vt == VT_VARIANT) VariantInit(&retstore);
        V_UI8(&ia->arg) = (ULONGLONG)(ULONG_PTR)(levels == 1 && vt == VT_VARIANT ? &retstore : (VARIANT *)&V_UI8(&retstore));
        ia->pass_vt = VT_PTR;
        ia->store_vt = levels == 1 ? vt : VT_UNKNOWN;
      }
      types[nargs] = ia->pass_vt ? ia->pass_vt : VT_I4;
      argv[nargs] = &ia->arg;
      nargs++;
    }
    const TYPEDESC *rt = &fd->elemdescFunc.tdesc;
    if (rt->vt == VT_VOID || ret_is_hresult) ret_vt = ret_is_hresult ? VT_ERROR : VT_EMPTY;
    else { BOOL p; if (FAILED(tl_resolve_type((ITypeInfo *)&call->ITypeInfo2_iface, rt, &ret_vt, &p, 0))) ret_vt = VT_I4; if (p) ret_vt = VT_PTR; have_ret = TRUE; }
    hr = DispCallFunc(instance, (ULONG_PTR)(USHORT)fd->oVft, fd->callconv, ret_vt, nargs, types, argv, &callres);
    if (SUCCEEDED(hr) && ret_is_hresult) { hr = (HRESULT)V_I4(&callres); if (SUCCEEDED(hr)) hr = S_OK; }
  }
  /* results and out values */
  if (SUCCEEDED(hr)) {
    if (extra) {
      IUnknown *obj = (IUnknown *)(ULONG_PTR)(retval_index >= 0 ? V_UI8(&retstore) : V_UI8(&callres));
      IDispatch *disp = NULL;
      if (result) VariantInit(result);
      if (!obj) hr = DISP_E_BADCALLEE;
      else {
        hr = IUnknown_QueryInterface(obj, &IID_IDispatch, (void **)&disp);
        if (SUCCEEDED(hr)) {
          DISPPARAMS dp = { params->rgvarg, NULL, extra, 0 };
          hr = IDispatch_Invoke(disp, DISPID_VALUE, &tl_zero_guid, LOCALE_USER_DEFAULT, DISPATCH_METHOD | DISPATCH_PROPERTYGET, &dp, result, excep, arg_err);
          IDispatch_Release(disp);
        }
        IUnknown_Release(obj);
        memset(&V_UI8(&retstore), 0, 8);
      }
    } else if (result) {
      VariantInit(result);
      if (retval_index >= 0) {
        UINT levels;
        const TYPEDESC *base = tl_strip_ptr(&fd->lprgelemdescParam[retval_index].tdesc, &levels);
        VARTYPE vt; BOOL ptr;
        if (levels == 1 && SUCCEEDED(tl_resolve_type((ITypeInfo *)&call->ITypeInfo2_iface, base, &vt, &ptr, 0))) {
          if (vt == VT_VARIANT) { *result = retstore; VariantInit(&retstore); }
          else { V_VT(result) = vt; memcpy(&V_UI8(result), &V_UI8(&retstore), 8); }
        }
      } else if (have_ret && ret_vt != VT_PTR) {
        V_VT(result) = ret_vt;
        memcpy(&V_UI8(result), &V_UI8(&callres), 8);
      }
    } else if (retval_index >= 0 && V_VT(&retstore) == VT_BSTR) {
      /* (nobody wants the value: free it) */
    }
    for (UINT p = 0; p < np; p++) {
      TlInvArg *ia = &a[p];
      if (!ia->indirect || !ia->copy_back || !ia->dest || !(V_VT(ia->dest) & VT_BYREF)) continue;
      if (ia->store_vt == VT_VARIANT) { VariantClear(V_VARIANTREF(ia->dest)); *V_VARIANTREF(ia->dest) = ia->store; VariantInit(&ia->store); }
      else if (V_BYREF(ia->dest)) { memcpy(V_BYREF(ia->dest), &V_UI8(&ia->store), (ia->store_vt == VT_I2 || ia->store_vt == VT_BOOL || ia->store_vt == VT_UI2) ? 2 : (ia->store_vt == VT_I1 || ia->store_vt == VT_UI1) ? 1 : (ia->store_vt == VT_I4 || ia->store_vt == VT_UI4 || ia->store_vt == VT_R4 || ia->store_vt == VT_ERROR) ? 4 : 8); V_VT(&ia->store) = VT_EMPTY; }
    }
  }
  for (UINT p = 0; p < np; p++) {
    TlInvArg *ia = &a[p];
    if (ia->indirect || !ia->pass_vt) { /* (converted copies are released below) */ }
    if (ia->indirect) { if (V_VT(&ia->store) != VT_EMPTY && (V_VT(&ia->store) & VT_TYPEMASK) != VT_UNKNOWN && (V_VT(&ia->store) & VT_TYPEMASK) != VT_DISPATCH) VariantClear(&ia->store); }
    else if (ia->supplied && V_VT(&ia->arg) != VT_EMPTY) VariantClear(&ia->arg);
  }
  VariantClear(&retstore);
  oa_free(a);
  return hr;
}
