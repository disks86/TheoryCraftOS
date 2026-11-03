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

/* IDispatch support: calling a function with VARIANT arguments (DispCallFunc), the helpers a dispatch implementation is built from (DispGetParam, DispGetIDsOfNames,
 * DispInvoke), the standard IDispatch (CreateStdDispatch) and a type information made from tables (CreateDispTypeInfo). */

#ifdef __x86_64__
/* oa_dispcall(function, slots, count, xmm0_out): calls `function` with the Windows x64 convention. slots[0..3] go to rcx, rdx, r8, r9 and (the same bits) to xmm0..xmm3, so that
 * integers and floating point values both arrive; the rest is copied to the stack above the 32 bytes of shadow space. The function's rax is returned, xmm0 stored. */
__asm__(
  ".text\n"
  ".globl oa_dispcall\n"
  ".def oa_dispcall; .scl 2; .type 32; .endef\n"
  ".seh_proc oa_dispcall\n"
  "oa_dispcall:\n"
  "  pushq %rbp\n  .seh_pushreg %rbp\n"
  "  pushq %rsi\n  .seh_pushreg %rsi\n"
  "  pushq %rdi\n  .seh_pushreg %rdi\n"
  "  pushq %r12\n  .seh_pushreg %r12\n"
  "  pushq %r13\n  .seh_pushreg %r13\n"
  "  movq %rsp, %rbp\n  .seh_setframe %rbp, 0\n"
  "  .seh_endprologue\n"
  "  movq %rcx, %r10\n"
  "  movq %rdx, %rsi\n"
  "  movq %r8, %r11\n"
  "  movq %r9, %r12\n"
  "  movq %r11, %rax\n"
  "  cmpq $4, %rax\n"
  "  jae 1f\n"
  "  movl $4, %eax\n"
  "1:\n"
  "  leaq 15(,%rax,8), %rax\n"
  "  andq $-16, %rax\n"
  "  subq %rax, %rsp\n"
  "  xorl %eax, %eax\n"
  "  movq %rax, (%rsp)\n  movq %rax, 8(%rsp)\n  movq %rax, 16(%rsp)\n  movq %rax, 24(%rsp)\n"
  "  movq %rsp, %rdi\n"
  "  movq %r11, %rcx\n"
  "  cld\n"
  "  rep movsq\n"
  "  movq (%rsp), %rcx\n  movq 8(%rsp), %rdx\n  movq 16(%rsp), %r8\n  movq 24(%rsp), %r9\n"
  "  movq %rcx, %xmm0\n  movq %rdx, %xmm1\n  movq %r8, %xmm2\n  movq %r9, %xmm3\n"
  "  call *%r10\n"
  "  movsd %xmm0, (%r12)\n"
  "  leaq (%rbp), %rsp\n"
  "  popq %r13\n  popq %r12\n  popq %rdi\n  popq %rsi\n  popq %rbp\n"
  "  ret\n"
  ".seh_endproc\n"
);
unsigned long long oa_dispcall(const void *function, const unsigned long long *slots, unsigned long long count, unsigned long long *xmm0);
#endif

EXPORT HRESULT WINAPI DispCallFunc(void *instance, ULONG_PTR vft_offset, CALLCONV cc, VARTYPE return_type, UINT count, VARTYPE *types, VARIANTARG **args, VARIANT *result) {
#ifdef __x86_64__
  if (cc != CC_STDCALL && cc != CC_CDECL) return DISP_E_BADCALLEE;
  if (return_type == VT_HRESULT) return E_INVALIDARG;
  if (count && (!types || !args)) return E_INVALIDARG;
  const void *function = NULL;
  if (instance) {
    const void *const *vtable = *(const void *const *const *)instance;       /* (the object's first word is its vtable pointer) */
    function = vtable[vft_offset / sizeof(void *)];
  } else {
    function = (const void *)vft_offset;
  }
  /* A VARIANT or DECIMAL argument and a VARIANT result travel as pointers to copies / the result (the Win64 rule for 16 byte values). */
  if (count > 0x10000) return E_INVALIDARG;
  const SIZE_T slot_count = (SIZE_T)count + 2;
  unsigned long long *slots = oa_calloc(slot_count * sizeof(*slots) + (SIZE_T)count * sizeof(VARIANT));
  if (!slots) return E_OUTOFMEMORY;
  VARIANT *copies = (VARIANT *)(slots + slot_count);
  SIZE_T used = 0;
  VARIANT local_result;
  VARIANT *result_target = result;
  if (instance) slots[used++] = (unsigned long long)(ULONG_PTR)instance;
  if (return_type == VT_VARIANT) {
    if (!result_target) { VariantInit(&local_result); result_target = &local_result; }
    slots[used++] = (unsigned long long)(ULONG_PTR)result_target;
  }
  for (UINT i = 0; i < count; i++) {
    VARIANT *arg = args[i];
    if (!arg) { oa_free(slots); return E_INVALIDARG; }
    switch (types[i]) {
      case VT_VARIANT:
      case VT_DECIMAL:
        memcpy(&copies[i], arg, sizeof(VARIANT));
        slots[used++] = (unsigned long long)(ULONG_PTR)&copies[i];
        break;
      default:
        slots[used++] = (unsigned long long)V_UI8(arg);      /* 8 bytes of the value: a smaller type's upper bytes are whatever the variant holds there */
        break;
    }
  }
  unsigned long long xmm0 = 0;
  const unsigned long long rax = oa_dispcall(function, slots, used, &xmm0);
  oa_free(slots);
  if (return_type == VT_VARIANT) {
    if (result_target == &local_result) VariantClear(&local_result);
  } else if (result) {
    V_VT(result) = return_type;
    if (return_type == VT_R4 || return_type == VT_R8) V_UI8(result) = xmm0;
    else V_UI8(result) = rax;
  }
  return S_OK;
#else
  (void)instance; (void)vft_offset; (void)cc; (void)return_type; (void)count; (void)types; (void)args; (void)result;
  return E_NOTIMPL;
#endif
}

/* ---- DispGetParam, DispGetIDsOfNames, DispInvoke ----------------------------------------------------------------------------------------------------- */
EXPORT HRESULT WINAPI DispGetParam(DISPPARAMS *params, UINT position, VARTYPE type, VARIANT *result, UINT *arg_error) {
  UINT index;
  if (position < params->cArgs) {
    index = params->cArgs - position - 1;                 /* positions count from the end of the argument array */
  } else {
    /* a named argument: its DISPID is the position */
    for (index = 0; index < params->cNamedArgs; index++) if ((UINT)params->rgdispidNamedArgs[index] == position) break;
    if (index == params->cNamedArgs) return DISP_E_PARAMNOTFOUND;
  }
  HRESULT hr;
  if (params->cArgs > 0 && !params->rgvarg) hr = E_INVALIDARG;
  else if (!result) hr = E_INVALIDARG;
  else hr = VariantChangeType(result, &params->rgvarg[index], 0, type);
  if (FAILED(hr) && arg_error) *arg_error = index;
  return hr;
}

EXPORT HRESULT WINAPI DispGetIDsOfNames(ITypeInfo *info, LPOLESTR *names, UINT count, DISPID *ids) { return ITypeInfo_GetIDsOfNames(info, names, count, ids); }

EXPORT HRESULT WINAPI DispInvoke(void *object, ITypeInfo *info, DISPID id, WORD flags, DISPPARAMS *params, VARIANT *result, EXCEPINFO *excep, UINT *arg_error) {
  return ITypeInfo_Invoke(info, object, id, flags, params, result, excep, arg_error);
}

static BOOL dsp_is_null_iid(REFIID riid) {
  static const GUID null_guid;
  return IsEqualGUID(riid, &null_guid);
}

/* ---- CreateStdDispatch: an IDispatch over a type information and the object's pointer. The inner IUnknown is the one returned; IDispatch's IUnknown methods go to the outer. */
typedef struct {
  IDispatch dispatch;
  IUnknown inner;
  LONG ref;
  IUnknown *outer;
  void *object;
  ITypeInfo *info;
} StdDispatch;

#define SD_FROM_DISPATCH(p) ((StdDispatch *)((char *)(p) - __builtin_offsetof(StdDispatch, dispatch)))
#define SD_FROM_INNER(p) ((StdDispatch *)((char *)(p) - __builtin_offsetof(StdDispatch, inner)))

static HRESULT WINAPI sd_inner_query(IUnknown *This, REFIID riid, void **out) {
  StdDispatch *sd = SD_FROM_INNER(This);
  if (!out) return E_POINTER;
  *out = NULL;
  if (IsEqualIID(riid, &IID_IUnknown)) *out = &sd->inner;
  else if (IsEqualIID(riid, &IID_IDispatch)) *out = &sd->dispatch;
  else return E_NOINTERFACE;
  IUnknown_AddRef((IUnknown *)*out);       /* (for the dispatch interface this goes to the outer unknown) */
  return S_OK;
}
static ULONG WINAPI sd_inner_addref(IUnknown *This) { return (ULONG)InterlockedIncrement(&SD_FROM_INNER(This)->ref); }
static ULONG WINAPI sd_inner_release(IUnknown *This) {
  StdDispatch *sd = SD_FROM_INNER(This);
  LONG ref = InterlockedDecrement(&sd->ref);
  if (ref == 0) {
    ITypeInfo_Release(sd->info);
    oa_free(sd);
  }
  return (ULONG)ref;
}
static const IUnknownVtbl sd_inner_vtbl = { sd_inner_query, sd_inner_addref, sd_inner_release };

static HRESULT WINAPI sd_query(IDispatch *This, REFIID riid, void **out) { return IUnknown_QueryInterface(SD_FROM_DISPATCH(This)->outer, riid, out); }
static ULONG WINAPI sd_addref(IDispatch *This) { return IUnknown_AddRef(SD_FROM_DISPATCH(This)->outer); }
static ULONG WINAPI sd_release(IDispatch *This) { return IUnknown_Release(SD_FROM_DISPATCH(This)->outer); }
static HRESULT WINAPI sd_get_type_info_count(IDispatch *This, UINT *count) {
  (void)This;
  if (!count) return E_INVALIDARG;
  *count = 1;
  return S_OK;
}
static HRESULT WINAPI sd_get_type_info(IDispatch *This, UINT index, LCID lcid, ITypeInfo **info) {
  StdDispatch *sd = SD_FROM_DISPATCH(This);
  (void)lcid;
  if (!info) return E_INVALIDARG;
  *info = NULL;
  if (index != 0) return DISP_E_BADINDEX;
  *info = sd->info;
  ITypeInfo_AddRef(sd->info);
  return S_OK;
}
static HRESULT WINAPI sd_get_ids_of_names(IDispatch *This, REFIID riid, LPOLESTR *names, UINT count, LCID lcid, DISPID *ids) {
  (void)lcid;
  if (!dsp_is_null_iid(riid)) return DISP_E_UNKNOWNINTERFACE;
  return DispGetIDsOfNames(SD_FROM_DISPATCH(This)->info, names, count, ids);
}
static HRESULT WINAPI sd_invoke(IDispatch *This, DISPID id, REFIID riid, LCID lcid, WORD flags, DISPPARAMS *params, VARIANT *result, EXCEPINFO *excep, UINT *arg_error) {
  StdDispatch *sd = SD_FROM_DISPATCH(This);
  (void)lcid;
  if (!dsp_is_null_iid(riid)) return DISP_E_UNKNOWNINTERFACE;
  return DispInvoke(sd->object, sd->info, id, flags, params, result, excep, arg_error);
}
static const IDispatchVtbl sd_vtbl = {
  sd_query, sd_addref, sd_release, sd_get_type_info_count, sd_get_type_info, sd_get_ids_of_names, sd_invoke,
};

EXPORT HRESULT WINAPI CreateStdDispatch(IUnknown *outer, void *object, ITypeInfo *info, IUnknown **result) {
  if (!result) return E_INVALIDARG;
  *result = NULL;
  if (!object || !info) return E_INVALIDARG;
  StdDispatch *sd = oa_calloc(sizeof(*sd));
  if (!sd) return E_OUTOFMEMORY;
  sd->dispatch.lpVtbl = (IDispatchVtbl *)&sd_vtbl;
  sd->inner.lpVtbl = (IUnknownVtbl *)&sd_inner_vtbl;
  sd->ref = 1;
  sd->outer = outer ? outer : &sd->inner;
  sd->object = object;
  sd->info = info;
  ITypeInfo_AddRef(info);
  *result = &sd->inner;
  return S_OK;
}

/* ---- CreateDispTypeInfo: a coclass type information that implements one interface (an interface type information) made of the METHODDATA entries. The two share the tables. */
typedef struct { BSTR name; VARTYPE type; } DtiParam;
typedef struct {
  BSTR name;
  DISPID id;
  UINT vtable_index;
  CALLCONV cc;
  WORD flags;                 /* INVOKEKIND */
  VARTYPE return_type;
  UINT arg_count;
  DtiParam *params;
} DtiMethod;
typedef struct { LONG ref; LCID lcid; UINT count; DtiMethod *methods; } DtiData;
typedef struct { ITypeInfo iface; LONG ref; DtiData *data; BOOL coclass; } DtiObject;

#define DTI_FROM(p) ((DtiObject *)(p))        /* (the interface is the object's first member) */

static void dti_data_release(DtiData *data) {
  if (InterlockedDecrement(&data->ref) != 0) return;
  for (UINT i = 0; i < data->count; i++) {
    DtiMethod *method = &data->methods[i];
    SysFreeString(method->name);
    if (method->params) for (UINT p = 0; p < method->arg_count; p++) SysFreeString(method->params[p].name);
    oa_free(method->params);
  }
  oa_free(data->methods);
  oa_free(data);
}

static BOOL dti_name_equal(const WCHAR *a, const WCHAR *b) {
  if (!a || !b) return FALSE;
  for (; *a && *b; a++, b++) if (oa_towlower(*a) != oa_towlower(*b)) return FALSE;
  return *a == *b;
}

static const DtiMethod *dti_find_id(const DtiData *data, MEMBERID id) {
  for (UINT i = 0; i < data->count; i++) if (data->methods[i].id == id) return &data->methods[i];
  return NULL;
}

static HRESULT WINAPI dti_query(ITypeInfo *This, REFIID riid, void **out) {
  if (!out) return E_POINTER;
  if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_ITypeInfo)) {
    *out = This;
    InterlockedIncrement(&DTI_FROM(This)->ref);
    return S_OK;
  }
  *out = NULL;
  return E_NOINTERFACE;
}
static ULONG WINAPI dti_addref(ITypeInfo *This) { return (ULONG)InterlockedIncrement(&DTI_FROM(This)->ref); }
static ULONG WINAPI dti_release(ITypeInfo *This) {
  DtiObject *object = DTI_FROM(This);
  LONG ref = InterlockedDecrement(&object->ref);
  if (ref == 0) {
    dti_data_release(object->data);
    oa_free(object);
  }
  return (ULONG)ref;
}

static HRESULT WINAPI dti_get_type_attr(ITypeInfo *This, TYPEATTR **out) {
  DtiObject *object = DTI_FROM(This);
  if (!out) return E_INVALIDARG;
  TYPEATTR *attr = oa_calloc(sizeof(*attr));
  if (!attr) return E_OUTOFMEMORY;
  attr->lcid = object->data->lcid;
  attr->memidConstructor = MEMBERID_NIL;
  attr->memidDestructor = MEMBERID_NIL;
  attr->cbSizeInstance = sizeof(void *);
  attr->cbAlignment = sizeof(void *);
  if (object->coclass) {
    attr->typekind = TKIND_COCLASS;
    attr->cImplTypes = 1;
  } else {
    UINT slots = 0;
    for (UINT i = 0; i < object->data->count; i++) if (object->data->methods[i].vtable_index >= slots) slots = object->data->methods[i].vtable_index + 1;
    attr->typekind = TKIND_INTERFACE;
    attr->cFuncs = (WORD)object->data->count;
    attr->cbSizeVft = (WORD)(slots * sizeof(void *));
  }
  *out = attr;
  return S_OK;
}
static HRESULT WINAPI dti_get_type_comp(ITypeInfo *This, ITypeComp **out) { (void)This; if (out) *out = NULL; return E_NOTIMPL; }

/* A FUNCDESC and its parameter descriptions are one block, which ReleaseFuncDesc frees. */
typedef struct { FUNCDESC desc; ELEMDESC params[1]; } DtiFuncBlock;
static HRESULT WINAPI dti_get_func_desc(ITypeInfo *This, UINT index, FUNCDESC **out) {
  DtiObject *object = DTI_FROM(This);
  if (!out) return E_INVALIDARG;
  *out = NULL;
  if (object->coclass || index >= object->data->count) return TYPE_E_ELEMENTNOTFOUND;
  const DtiMethod *method = &object->data->methods[index];
  const SIZE_T extra = method->arg_count ? (SIZE_T)(method->arg_count - 1) * sizeof(ELEMDESC) : 0;
  DtiFuncBlock *block = oa_calloc(sizeof(*block) + extra);
  if (!block) return E_OUTOFMEMORY;
  FUNCDESC *desc = &block->desc;
  desc->memid = method->id;
  desc->funckind = FUNC_VIRTUAL;
  desc->invkind = (INVOKEKIND)method->flags;
  desc->callconv = method->cc;
  desc->cParams = (SHORT)method->arg_count;
  desc->oVft = (SHORT)(method->vtable_index * sizeof(void *));
  desc->elemdescFunc.tdesc.vt = method->return_type;
  desc->lprgelemdescParam = block->params;
  for (UINT i = 0; i < method->arg_count; i++) {
    block->params[i].tdesc.vt = method->params[i].type;
    block->params[i].paramdesc.wParamFlags = PARAMFLAG_NONE;
  }
  *out = desc;
  return S_OK;
}
static HRESULT WINAPI dti_get_var_desc(ITypeInfo *This, UINT index, VARDESC **out) {
  (void)This; (void)index;
  if (out) *out = NULL;
  return TYPE_E_ELEMENTNOTFOUND;
}

static HRESULT WINAPI dti_get_names(ITypeInfo *This, MEMBERID id, BSTR *names, UINT max, UINT *count) {
  const DtiMethod *method = dti_find_id(DTI_FROM(This)->data, id);
  if (!names || !count) return E_INVALIDARG;
  *count = 0;
  if (!method) return TYPE_E_ELEMENTNOTFOUND;
  UINT n = 0;
  while (n < max && n <= method->arg_count) {
    BSTR text = n == 0 ? method->name : method->params[n - 1].name;
    names[n] = text ? SysAllocStringLen(text, SysStringLen(text)) : NULL;
    if (text && !names[n]) {
      while (n > 0) SysFreeString(names[--n]);
      return E_OUTOFMEMORY;
    }
    n++;
  }
  *count = n;
  return S_OK;
}
static HRESULT WINAPI dti_get_ref_type_of_impl_type(ITypeInfo *This, UINT index, HREFTYPE *ref) {
  if (!ref) return E_INVALIDARG;
  if (!DTI_FROM(This)->coclass || index != 0) return TYPE_E_ELEMENTNOTFOUND;
  *ref = 0;
  return S_OK;
}
static HRESULT WINAPI dti_get_impl_type_flags(ITypeInfo *This, UINT index, INT *flags) {
  if (!flags) return E_INVALIDARG;
  if (!DTI_FROM(This)->coclass || index != 0) return TYPE_E_ELEMENTNOTFOUND;
  *flags = IMPLTYPEFLAG_FDEFAULT;
  return S_OK;
}

/* Names to member IDs: the first name is a member, the following ones its parameters (the id is the parameter's index). */
static HRESULT WINAPI dti_get_ids_of_names(ITypeInfo *This, LPOLESTR *names, UINT count, MEMBERID *ids) {
  const DtiData *data = DTI_FROM(This)->data;
  if (!names || !ids || !count) return E_INVALIDARG;
  for (UINT i = 0; i < count; i++) ids[i] = DISPID_UNKNOWN;
  const DtiMethod *method = NULL;
  for (UINT i = 0; i < data->count && !method; i++) if (dti_name_equal(data->methods[i].name, names[0])) method = &data->methods[i];
  if (!method) return DISP_E_UNKNOWNNAME;
  ids[0] = method->id;
  HRESULT hr = S_OK;
  for (UINT n = 1; n < count; n++) {
    BOOL found = FALSE;
    for (UINT p = 0; p < method->arg_count && !found; p++) if (dti_name_equal(method->params[p].name, names[n])) { ids[n] = (MEMBERID)p; found = TRUE; }
    if (!found) hr = DISP_E_UNKNOWNNAME;
  }
  return hr;
}

static BOOL dti_flags_match(WORD kind, WORD flags) { return (kind & flags) != 0; }

static HRESULT WINAPI dti_invoke(ITypeInfo *This, PVOID object, MEMBERID id, WORD flags, DISPPARAMS *params, VARIANT *result, EXCEPINFO *excep, UINT *arg_error) {
  const DtiData *data = DTI_FROM(This)->data;
  DISPPARAMS none = { NULL, NULL, 0, 0 };
  (void)excep;
  if (!object) return E_INVALIDARG;
  if (!params) params = &none;
  if (arg_error) *arg_error = 0;
  const DtiMethod *method = NULL;
  for (UINT i = 0; i < data->count && !method; i++) if (data->methods[i].id == id && dti_flags_match(data->methods[i].flags, flags)) method = &data->methods[i];
  if (!method) return DISP_E_MEMBERNOTFOUND;
  const UINT n = method->arg_count;
  if (params->cArgs != n || params->cNamedArgs > params->cArgs) return DISP_E_BADPARAMCOUNT;
  if (params->cArgs && !params->rgvarg) return E_INVALIDARG;
  if (params->cNamedArgs && !params->rgdispidNamedArgs) return E_INVALIDARG;

  /* storage for the converted arguments, the pointers handed to the call and their types */
  VARIANT *converted = oa_calloc((SIZE_T)(n ? n : 1) * (sizeof(VARIANT) + sizeof(VARIANT *) + sizeof(VARTYPE) + sizeof(UINT)));
  if (!converted) return E_OUTOFMEMORY;
  VARIANT **pointers = (VARIANT **)(converted + n);
  VARTYPE *types = (VARTYPE *)(pointers + n);
  UINT *origin = (UINT *)(types + n);        /* (index in rgvarg, for the error index) */
  HRESULT hr = S_OK;
  for (UINT k = 0; k < params->cNamedArgs && hr == S_OK; k++) {
    const DISPID named = params->rgdispidNamedArgs[k];
    const UINT slot = named == DISPID_PROPERTYPUT ? n - 1 : (UINT)named;
    if (named < DISPID_PROPERTYPUT || slot >= n || pointers[slot]) { hr = DISP_E_PARAMNOTFOUND; if (arg_error) *arg_error = k; break; }
    pointers[slot] = &params->rgvarg[k];
    origin[slot] = k;
  }
  UINT next = 0;
  for (UINT u = params->cArgs; u > params->cNamedArgs && hr == S_OK; u--) {
    while (next < n && pointers[next]) next++;
    if (next >= n) { hr = DISP_E_BADPARAMCOUNT; break; }
    pointers[next] = &params->rgvarg[u - 1];
    origin[next] = u - 1;
  }
  for (UINT p = 0; p < n && hr == S_OK; p++) {
    const VARTYPE want = method->params[p].type;
    VARIANT *source = pointers[p];
    if (!source) { hr = DISP_E_PARAMNOTFOUND; break; }
    types[p] = want;
    if (V_VT(source) == (VT_VARIANT | VT_BYREF) && want != (VT_VARIANT | VT_BYREF)) source = V_VARIANTREF(source);
    if (want == VT_VARIANT || V_VT(source) == want) { pointers[p] = source; continue; }
    if (want & VT_BYREF) { hr = DISP_E_TYPEMISMATCH; }
    else hr = VariantChangeType(&converted[p], source, 0, want);
    if (hr == S_OK) pointers[p] = &converted[p];
    else if (arg_error) *arg_error = origin[p];
  }
  if (hr == S_OK) {
    if (method->return_type == VT_HRESULT) {
      VARIANT status;
      VariantInit(&status);
      hr = DispCallFunc(object, method->vtable_index * sizeof(void *), method->cc, VT_ERROR, n, types, pointers, &status);
      if (hr == S_OK) hr = V_ERROR(&status);
    } else {
      VARIANT scratch;
      VARIANT *target = result;
      VARTYPE type = method->return_type == VT_VOID ? VT_EMPTY : method->return_type;
      if (!target) { VariantInit(&scratch); target = &scratch; }
      hr = DispCallFunc(object, method->vtable_index * sizeof(void *), method->cc, type, n, types, pointers, target);
      if (target == &scratch) VariantClear(&scratch);
    }
  }
  for (UINT p = 0; p < n; p++) VariantClear(&converted[p]);
  oa_free(converted);
  return hr;
}

static HRESULT WINAPI dti_get_documentation(ITypeInfo *This, MEMBERID id, BSTR *name, BSTR *doc, DWORD *help_context, BSTR *help_file) {
  if (name) *name = NULL;
  if (doc) *doc = NULL;
  if (help_context) *help_context = 0;
  if (help_file) *help_file = NULL;
  if (id == MEMBERID_NIL) return S_OK;
  const DtiMethod *method = dti_find_id(DTI_FROM(This)->data, id);
  if (!method) return TYPE_E_ELEMENTNOTFOUND;
  if (name && method->name) {
    *name = SysAllocStringLen(method->name, SysStringLen(method->name));
    if (!*name) return E_OUTOFMEMORY;
  }
  return S_OK;
}
static HRESULT WINAPI dti_get_dll_entry(ITypeInfo *This, MEMBERID id, INVOKEKIND kind, BSTR *dll, BSTR *name, WORD *ordinal) {
  (void)This; (void)id; (void)kind;
  if (dll) *dll = NULL;
  if (name) *name = NULL;
  if (ordinal) *ordinal = 0;
  return TYPE_E_BADMODULEKIND;
}
static HRESULT WINAPI dti_get_ref_type_info(ITypeInfo *This, HREFTYPE ref, ITypeInfo **out) {
  DtiObject *object = DTI_FROM(This);
  if (!out) return E_INVALIDARG;
  *out = NULL;
  if (!object->coclass || ref != 0) return TYPE_E_ELEMENTNOTFOUND;
  DtiObject *interface_object = oa_calloc(sizeof(*interface_object));
  if (!interface_object) return E_OUTOFMEMORY;
  interface_object->iface.lpVtbl = This->lpVtbl;
  interface_object->ref = 1;
  interface_object->data = object->data;
  InterlockedIncrement(&object->data->ref);
  *out = &interface_object->iface;
  return S_OK;
}
static HRESULT WINAPI dti_address_of_member(ITypeInfo *This, MEMBERID id, INVOKEKIND kind, PVOID *out) {
  (void)This; (void)id; (void)kind;
  if (out) *out = NULL;
  return TYPE_E_BADMODULEKIND;
}
static HRESULT WINAPI dti_create_instance(ITypeInfo *This, IUnknown *outer, REFIID riid, PVOID *out) {
  (void)This; (void)outer; (void)riid;
  if (out) *out = NULL;
  return E_NOTIMPL;
}
static HRESULT WINAPI dti_get_mops(ITypeInfo *This, MEMBERID id, BSTR *mops) {
  (void)This; (void)id;
  if (!mops) return E_INVALIDARG;
  *mops = NULL;
  return S_OK;
}
static HRESULT WINAPI dti_get_containing_type_lib(ITypeInfo *This, ITypeLib **lib, UINT *index) {
  (void)This;
  if (lib) *lib = NULL;
  if (index) *index = 0;
  return E_NOTIMPL;
}
static void WINAPI dti_release_type_attr(ITypeInfo *This, TYPEATTR *attr) { (void)This; oa_free(attr); }
static void WINAPI dti_release_func_desc(ITypeInfo *This, FUNCDESC *desc) { (void)This; oa_free(desc); }       /* (the block starts with the FUNCDESC) */
static void WINAPI dti_release_var_desc(ITypeInfo *This, VARDESC *desc) { (void)This; (void)desc; }

static const ITypeInfoVtbl dti_vtbl = {
  dti_query, dti_addref, dti_release, dti_get_type_attr, dti_get_type_comp, dti_get_func_desc, dti_get_var_desc, dti_get_names, dti_get_ref_type_of_impl_type,
  dti_get_impl_type_flags, dti_get_ids_of_names, dti_invoke, dti_get_documentation, dti_get_dll_entry, dti_get_ref_type_info, dti_address_of_member,
  dti_create_instance, dti_get_mops, dti_get_containing_type_lib, dti_release_type_attr, dti_release_func_desc, dti_release_var_desc,
};

EXPORT HRESULT WINAPI CreateDispTypeInfo(INTERFACEDATA *interface_data, LCID lcid, ITypeInfo **result) {
  if (!interface_data || !result) return E_INVALIDARG;
  *result = NULL;
  if (interface_data->cMembers && !interface_data->pmethdata) return E_INVALIDARG;
  if (interface_data->cMembers > 0xFFFF) return E_INVALIDARG;
  DtiData *data = oa_calloc(sizeof(*data));
  DtiObject *object = oa_calloc(sizeof(*object));
  if (!data || !object) { oa_free(data); oa_free(object); return E_OUTOFMEMORY; }
  data->ref = 1;
  data->lcid = lcid;
  data->methods = oa_calloc((SIZE_T)interface_data->cMembers * sizeof(DtiMethod));
  if (!data->methods) { oa_free(data); oa_free(object); return E_OUTOFMEMORY; }
  object->iface.lpVtbl = (ITypeInfoVtbl *)&dti_vtbl;
  object->ref = 1;
  object->data = data;
  object->coclass = TRUE;
  HRESULT hr = S_OK;
  for (UINT i = 0; i < interface_data->cMembers && hr == S_OK; i++) {
    const METHODDATA *source = &interface_data->pmethdata[i];
    DtiMethod *method = &data->methods[i];
    data->count = i + 1;            /* (so that releasing the data frees what is made so far) */
    if (source->cArgs && !source->ppdata) { hr = E_INVALIDARG; break; }
    if (source->cArgs > 0xFFFF) { hr = E_INVALIDARG; break; }
    method->id = source->dispid;
    method->vtable_index = source->iMeth;
    method->cc = source->cc;
    method->flags = source->wFlags;
    method->return_type = source->vtReturn;
    method->arg_count = source->cArgs;
    if (source->szName) {
      method->name = SysAllocString(source->szName);
      if (!method->name) { hr = E_OUTOFMEMORY; break; }
    }
    if (source->cArgs) {
      method->params = oa_calloc((SIZE_T)source->cArgs * sizeof(DtiParam));
      if (!method->params) { hr = E_OUTOFMEMORY; break; }
      for (UINT p = 0; p < source->cArgs; p++) {
        method->params[p].type = source->ppdata[p].vt;
        if (source->ppdata[p].szName) {
          method->params[p].name = SysAllocString(source->ppdata[p].szName);
          if (!method->params[p].name) { hr = E_OUTOFMEMORY; break; }
        }
      }
    }
  }
  if (hr != S_OK) {
    dti_data_release(data);
    oa_free(object);
    return hr;
  }
  *result = &object->iface;
  return S_OK;
}
