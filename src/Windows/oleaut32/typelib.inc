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

/* Type libraries. One in-memory model serves the readers (the MSFT binary format, typelib_msft.inc, and the older SLTG format, typelib_sltg.inc) and the writer
 * (CreateTypeLib2, typelib_create.inc, which saves the model as MSFT): a TlLib holds TlTypes, a TlType holds functions, variables and implemented types.
 * ITypeLib2/ITypeComp/ICreateTypeLib2 are views of a TlLib, ITypeInfo2/ICreateTypeInfo2/ITypeComp views of a TlTi (the typeinfo object of a TlType). Everything the
 * model owns is a heap block of its own, so that the loaders can fail at any point and tl_lib_free() releases what exists. HREFTYPEs are the file's: a multiple of the
 * size of an MSFT typeinfo record (100) is the internal type with that offset, a value with the low bits 01 an offset into the table of imported types. */

#define TL_TYPEINFO_RECORD 100u     /* sizeof(MSFT_TypeInfoBase): also the step of the internal HREFTYPEs */
#define TL_IMPINFO_RECORD 12u
#define TL_MAX_ITEMS 0x10000u       /* more functions, variables or types than this in a library are not believed */

static const GUID tl_zero_guid;
typedef struct TlLib TlLib;
typedef struct TlType TlType;

typedef struct { GUID guid; VARIANT value; } TlCust;
typedef struct { TlCust *items; UINT count; } TlCustList;

typedef struct {
  FUNCDESC fd;                        /* lprgelemdescParam (cParams ELEMDESCs) and every typedesc in it are owned */
  WCHAR *name, *doc;
  DWORD help_context, help_string_context;
  WCHAR *dll_entry;                   /* DLL entry point name, or NULL */
  WORD ordinal;                       /* the entry point's ordinal when dll_entry is NULL and has_ordinal */
  BOOL has_ordinal;
  BOOL vft_explicit;                  /* oVft was given by the caller (created libraries) */
  WCHAR **pnames;                     /* cParams names (entries may be NULL), or NULL */
  TlCustList cust;
  TlCustList *pcust;                  /* cParams lists, or NULL */
} TlFunc;

typedef struct {
  VARDESC vd;                         /* elemdescVar owned; lpvarValue (a VARIANT) owned for VAR_CONST */
  WCHAR *name, *doc;
  DWORD help_context, help_string_context;
  TlCustList cust;
} TlVar;

typedef struct { HREFTYPE href; INT flags; TlCustList cust; } TlImpl;

typedef struct { GUID guid; LCID lcid; WORD major, minor; WCHAR *name; ITypeLib *lib; } TlImpFile;
typedef struct { UINT file; BOOL by_guid; GUID guid; DWORD offset; INT tkind; } TlImpInfo;

typedef struct TlTi {
  ITypeInfo2 ITypeInfo2_iface;
  ICreateTypeInfo2 ICreateTypeInfo2_iface;
  ITypeComp ITypeComp_iface;
  LONG ref;
  TlType *t;
  BOOL alt;                           /* the other half of a dual interface (TKIND_INTERFACE view of a TKIND_DISPATCH) */
} TlTi;

struct TlType {
  TlTi ti;
  TlTi alt_ti;
  TlLib *lib;
  UINT index;
  TYPEATTR attr;                      /* tdescAlias owned; cFuncs, cVars, cImplTypes are kept equal to the counts below */
  WCHAR *name, *doc, *dllname;
  DWORD help_context, help_string_context;
  TlFunc *funcs; UINT nfuncs, funcs_cap;
  TlVar *vars; UINT nvars, vars_cap;
  TlImpl *impls; UINT nimpls, impls_cap;
  TlCustList cust;
  DWORD file_offset;                  /* the offset of the type in the file it was read from: its internal HREFTYPE */
  WCHAR *schema;                      /* ICreateTypeInfo::SetSchema */
  BOOL laid_out;
  ITypeInfo2 *dispatch_info;          /* the IDispatch half of a dual interface, cached */
};

struct TlLib {
  ITypeLib2 ITypeLib2_iface;
  ITypeComp ITypeComp_iface;
  ICreateTypeLib2 ICreateTypeLib2_iface;
  LONG ref;
  GUID guid;
  LCID lcid;                          /* the library's LCID (the one TLIBATTR reports) */
  LCID file_lcid;                     /* the LCID the strings of a file were written in */
  WORD major, minor;
  WORD flags;
  SYSKIND syskind;
  WCHAR *name, *doc, *helpfile, *helpdir, *helpstringdll;
  DWORD help_context, help_string_context;
  TlType **types; UINT ntypes, types_cap;
  TlImpFile *impfiles; UINT nimpfiles, impfiles_cap;
  TlImpInfo *impinfos; UINT nimpinfos, impinfos_cap;
  HREFTYPE dispatch_href;             /* the library's reference to IDispatch (-1 if none) */
  TlCustList cust;
  WCHAR *path;                        /* the file it was loaded from (cache key) */
  BOOL created;                       /* made by CreateTypeLib2 */
  BOOL in_cache;
  ITypeInfo *dispatch_ti;             /* IDispatch of the standard library, found on demand */
  ITypeInfo *unknown_ti;
};

/* ---- small helpers --------------------------------------------------------------------------------------------------------------------------------- */
static WCHAR *tl_wcsdup(const WCHAR *s) {
  if (!s) return NULL;
  const SIZE_T n = oa_wcslen(s);
  WCHAR *d = oa_alloc((n + 1) * sizeof(WCHAR));
  if (d) memcpy(d, s, (n + 1) * sizeof(WCHAR));
  return d;
}
static BSTR tl_bstr(const WCHAR *s) { return s ? SysAllocString(s) : NULL; }
static int tl_wcsicmp(const WCHAR *a, const WCHAR *b) {
  for (;; a++, b++) {
    const WCHAR x = oa_towlower(*a), y = oa_towlower(*b);
    if (x != y) return (int)x - (int)y;
    if (!x) return 0;
  }
}
static void tl_set_str(WCHAR **slot, const WCHAR *s, HRESULT *hr) {
  WCHAR *copy = tl_wcsdup(s);
  if (s && !copy) { *hr = E_OUTOFMEMORY; return; }
  oa_free(*slot);
  *slot = copy;
}

/* A growing array: one function so that the element size and the limit are checked in one place. Returns the new element (zeroed) or NULL. */
static void *tl_grow(void **array, UINT *count, UINT *cap, SIZE_T elem) {
  if (*count >= TL_MAX_ITEMS) return NULL;
  if (*count >= *cap) {
    const UINT ncap = *cap ? *cap * 2 : 8;
    void *p = oa_realloc(*array, (SIZE_T)ncap * elem);
    if (!p) return NULL;
    *array = p;
    *cap = ncap;
  }
  char *slot = (char *)*array + (SIZE_T)*count * elem;
  memset(slot, 0, elem);
  (*count)++;
  return slot;
}
#define TL_GROW(arr, count, cap) tl_grow((void **)&(arr), &(count), &(cap), sizeof(*(arr)))

/* ---- TYPEDESC / ELEMDESC / FUNCDESC / VARDESC / TYPEATTR copies --------------------------------------------------------------------------------------- */
static void tl_td_free(TYPEDESC *td) {
  if (td->vt == VT_PTR || td->vt == VT_SAFEARRAY) {
    if (td->lptdesc) { tl_td_free(td->lptdesc); oa_free(td->lptdesc); }
  } else if (td->vt == VT_CARRAY) {
    if (td->lpadesc) { tl_td_free(&td->lpadesc->tdescElem); oa_free(td->lpadesc); }
  }
  memset(td, 0, sizeof(*td));
}
static HRESULT tl_td_copy(TYPEDESC *dst, const TYPEDESC *src, UINT depth) {
  *dst = *src;
  if (depth > 64) { memset(dst, 0, sizeof(*dst)); return E_INVALIDARG; }
  if (src->vt == VT_PTR || src->vt == VT_SAFEARRAY) {
    if (!src->lptdesc) { dst->lptdesc = NULL; return S_OK; }
    dst->lptdesc = oa_calloc(sizeof(TYPEDESC));
    if (!dst->lptdesc) { memset(dst, 0, sizeof(*dst)); return E_OUTOFMEMORY; }
    HRESULT hr = tl_td_copy(dst->lptdesc, src->lptdesc, depth + 1);
    if (FAILED(hr)) { oa_free(dst->lptdesc); memset(dst, 0, sizeof(*dst)); return hr; }
  } else if (src->vt == VT_CARRAY) {
    if (!src->lpadesc) return S_OK;
    const UINT dims = src->lpadesc->cDims;
    if (dims > 64) { memset(dst, 0, sizeof(*dst)); return E_INVALIDARG; }
    const SIZE_T size = sizeof(ARRAYDESC) + (dims ? (dims - 1) : 0) * sizeof(SAFEARRAYBOUND);
    dst->lpadesc = oa_calloc(size);
    if (!dst->lpadesc) { memset(dst, 0, sizeof(*dst)); return E_OUTOFMEMORY; }
    memcpy(dst->lpadesc, src->lpadesc, size);
    HRESULT hr = tl_td_copy(&dst->lpadesc->tdescElem, &src->lpadesc->tdescElem, depth + 1);
    if (FAILED(hr)) { oa_free(dst->lpadesc); memset(dst, 0, sizeof(*dst)); return hr; }
  }
  return S_OK;
}
static void tl_elem_free(ELEMDESC *e) {
  tl_td_free(&e->tdesc);
  if ((e->paramdesc.wParamFlags & PARAMFLAG_FHASDEFAULT) && e->paramdesc.pparamdescex) {
    VariantClear(&e->paramdesc.pparamdescex->varDefaultValue);
    oa_free(e->paramdesc.pparamdescex);
  }
  memset(e, 0, sizeof(*e));
}
static HRESULT tl_elem_copy(ELEMDESC *dst, const ELEMDESC *src) {
  memset(dst, 0, sizeof(*dst));
  HRESULT hr = tl_td_copy(&dst->tdesc, &src->tdesc, 0);
  if (FAILED(hr)) return hr;
  dst->paramdesc = src->paramdesc;
  dst->paramdesc.pparamdescex = NULL;
  if ((src->paramdesc.wParamFlags & PARAMFLAG_FHASDEFAULT) && src->paramdesc.pparamdescex) {
    PARAMDESCEX *ex = oa_calloc(sizeof(PARAMDESCEX));
    if (!ex) { tl_td_free(&dst->tdesc); return E_OUTOFMEMORY; }
    ex->cBytes = sizeof(PARAMDESCEX);
    hr = VariantCopy(&ex->varDefaultValue, &src->paramdesc.pparamdescex->varDefaultValue);
    if (FAILED(hr)) { oa_free(ex); tl_td_free(&dst->tdesc); return hr; }
    dst->paramdesc.pparamdescex = ex;
  }
  return S_OK;
}

static void tl_funcdesc_release(FUNCDESC *fd) {
  if (!fd) return;
  if (fd->lprgelemdescParam) {
    for (SHORT i = 0; i < fd->cParams; i++) tl_elem_free(&fd->lprgelemdescParam[i]);
    oa_free(fd->lprgelemdescParam);
  }
  oa_free(fd->lprgscode);
  tl_elem_free(&fd->elemdescFunc);
  oa_free(fd);
}
/* A deep copy of a model FUNCDESC (what GetFuncDesc returns; ReleaseFuncDesc frees it). */
static FUNCDESC *tl_funcdesc_clone(const FUNCDESC *src, HRESULT *hr) {
  FUNCDESC *fd = oa_calloc(sizeof(FUNCDESC));
  if (!fd) { *hr = E_OUTOFMEMORY; return NULL; }
  *fd = *src;
  fd->lprgelemdescParam = NULL;
  fd->lprgscode = NULL;
  memset(&fd->elemdescFunc, 0, sizeof(fd->elemdescFunc));
  *hr = tl_elem_copy(&fd->elemdescFunc, &src->elemdescFunc);
  if (SUCCEEDED(*hr) && src->cParams > 0) {
    fd->lprgelemdescParam = oa_calloc((SIZE_T)src->cParams * sizeof(ELEMDESC));
    if (!fd->lprgelemdescParam) *hr = E_OUTOFMEMORY;
    for (SHORT i = 0; SUCCEEDED(*hr) && i < src->cParams; i++) *hr = tl_elem_copy(&fd->lprgelemdescParam[i], &src->lprgelemdescParam[i]);
    if (FAILED(*hr)) { fd->cParams = 0; }
  }
  if (SUCCEEDED(*hr) && src->cScodes > 0 && src->lprgscode) {
    fd->lprgscode = oa_alloc((SIZE_T)src->cScodes * sizeof(SCODE));
    if (!fd->lprgscode) *hr = E_OUTOFMEMORY; else memcpy(fd->lprgscode, src->lprgscode, (SIZE_T)src->cScodes * sizeof(SCODE));
  }
  if (FAILED(*hr)) { fd->cParams = fd->lprgelemdescParam ? src->cParams : 0; tl_funcdesc_release(fd); return NULL; }
  return fd;
}
static void tl_vardesc_release(VARDESC *vd) {
  if (!vd) return;
  tl_elem_free(&vd->elemdescVar);
  if (vd->varkind == VAR_CONST && vd->lpvarValue) { VariantClear(vd->lpvarValue); oa_free(vd->lpvarValue); }
  oa_free(vd->lpstrSchema);
  oa_free(vd);
}
static VARDESC *tl_vardesc_clone(const VARDESC *src, HRESULT *hr) {
  VARDESC *vd = oa_calloc(sizeof(VARDESC));
  if (!vd) { *hr = E_OUTOFMEMORY; return NULL; }
  *vd = *src;
  vd->lpstrSchema = NULL;
  memset(&vd->elemdescVar, 0, sizeof(vd->elemdescVar));
  *hr = tl_elem_copy(&vd->elemdescVar, &src->elemdescVar);
  if (SUCCEEDED(*hr) && src->varkind == VAR_CONST) {
    vd->lpvarValue = oa_calloc(sizeof(VARIANT));
    if (!vd->lpvarValue) *hr = E_OUTOFMEMORY;
    else if (src->lpvarValue) *hr = VariantCopy(vd->lpvarValue, src->lpvarValue);
    if (FAILED(*hr)) { oa_free(vd->lpvarValue); vd->lpvarValue = NULL; }
  }
  if (SUCCEEDED(*hr) && src->lpstrSchema) {
    vd->lpstrSchema = tl_wcsdup(src->lpstrSchema);
    if (!vd->lpstrSchema) *hr = E_OUTOFMEMORY;
  }
  if (FAILED(*hr)) { vd->varkind = VAR_PERINSTANCE; tl_vardesc_release(vd); return NULL; }
  return vd;
}

/* ---- custom data ------------------------------------------------------------------------------------------------------------------------------------ */
static void tl_cust_free(TlCustList *l) {
  for (UINT i = 0; i < l->count; i++) VariantClear(&l->items[i].value);
  oa_free(l->items);
  l->items = NULL;
  l->count = 0;
}
static HRESULT tl_cust_set(TlCustList *l, const GUID *guid, const VARIANT *value) {
  for (UINT i = 0; i < l->count; i++)
    if (IsEqualGUID(&l->items[i].guid, guid)) {
      VARIANT copy;
      VariantInit(&copy);
      HRESULT hr = VariantCopy(&copy, (VARIANT *)value);
      if (FAILED(hr)) return hr;
      VariantClear(&l->items[i].value);
      l->items[i].value = copy;
      return S_OK;
    }
  TlCust *n = oa_realloc(l->items, ((SIZE_T)l->count + 1) * sizeof(TlCust));
  if (!n) return E_OUTOFMEMORY;
  l->items = n;
  VariantInit(&n[l->count].value);
  HRESULT hr = VariantCopy(&n[l->count].value, (VARIANT *)value);
  if (FAILED(hr)) return hr;
  n[l->count].guid = *guid;
  l->count++;
  return S_OK;
}
static HRESULT tl_cust_get(const TlCustList *l, const GUID *guid, VARIANT *out) {
  if (!guid || !out) return E_INVALIDARG;
  VariantInit(out);
  for (UINT i = 0; i < l->count; i++)
    if (IsEqualGUID(&l->items[i].guid, guid)) return VariantCopy(out, (VARIANT *)&l->items[i].value);
  return S_OK;
}
static HRESULT tl_cust_all(const TlCustList *l, CUSTDATA *out) {
  if (!out) return E_INVALIDARG;
  out->cCustData = 0;
  out->prgCustData = NULL;
  if (!l->count) return S_OK;
  CUSTDATAITEM *items = oa_calloc((SIZE_T)l->count * sizeof(CUSTDATAITEM));
  if (!items) return E_OUTOFMEMORY;
  for (UINT i = 0; i < l->count; i++) {
    items[i].guid = l->items[i].guid;
    VariantInit(&items[i].varValue);
    HRESULT hr = VariantCopy(&items[i].varValue, (VARIANT *)&l->items[i].value);
    if (FAILED(hr)) {
      for (UINT k = 0; k < i; k++) VariantClear(&items[k].varValue);
      oa_free(items);
      return hr;
    }
  }
  out->cCustData = l->count;
  out->prgCustData = items;
  return S_OK;
}

/* ---- freeing the model --------------------------------------------------------------------------------------------------------------------------- */
static void tl_func_free(TlFunc *f) {
  if (f->fd.lprgelemdescParam) {
    for (SHORT i = 0; i < f->fd.cParams; i++) tl_elem_free(&f->fd.lprgelemdescParam[i]);
    oa_free(f->fd.lprgelemdescParam);
  }
  tl_elem_free(&f->fd.elemdescFunc);
  oa_free(f->fd.lprgscode);
  oa_free(f->name); oa_free(f->doc); oa_free(f->dll_entry);
  if (f->pnames) { for (SHORT i = 0; i < f->fd.cParams; i++) oa_free(f->pnames[i]); oa_free(f->pnames); }
  if (f->pcust) { for (SHORT i = 0; i < f->fd.cParams; i++) tl_cust_free(&f->pcust[i]); oa_free(f->pcust); }
  tl_cust_free(&f->cust);
  memset(f, 0, sizeof(*f));
}
static void tl_var_free(TlVar *v) {
  tl_elem_free(&v->vd.elemdescVar);
  if (v->vd.varkind == VAR_CONST && v->vd.lpvarValue) { VariantClear(v->vd.lpvarValue); oa_free(v->vd.lpvarValue); }
  oa_free(v->vd.lpstrSchema);
  oa_free(v->name); oa_free(v->doc);
  tl_cust_free(&v->cust);
  memset(v, 0, sizeof(*v));
}
static void tl_type_free_contents(TlType *t) {
  for (UINT i = 0; i < t->nfuncs; i++) tl_func_free(&t->funcs[i]);
  oa_free(t->funcs);
  for (UINT i = 0; i < t->nvars; i++) tl_var_free(&t->vars[i]);
  oa_free(t->vars);
  for (UINT i = 0; i < t->nimpls; i++) tl_cust_free(&t->impls[i].cust);
  oa_free(t->impls);
  tl_td_free(&t->attr.tdescAlias);
  oa_free(t->name); oa_free(t->doc); oa_free(t->dllname); oa_free(t->schema);
  tl_cust_free(&t->cust);
  if (t->dispatch_info) ITypeInfo2_Release(t->dispatch_info);
}
static void tl_lib_free(TlLib *lib) {
  if (!lib) return;
  for (UINT i = 0; i < lib->ntypes; i++) { tl_type_free_contents(lib->types[i]); oa_free(lib->types[i]); }
  oa_free(lib->types);
  for (UINT i = 0; i < lib->nimpfiles; i++) { oa_free(lib->impfiles[i].name); if (lib->impfiles[i].lib) ITypeLib_Release(lib->impfiles[i].lib); }
  oa_free(lib->impfiles);
  oa_free(lib->impinfos);
  oa_free(lib->name); oa_free(lib->doc); oa_free(lib->helpfile); oa_free(lib->helpdir); oa_free(lib->helpstringdll); oa_free(lib->path);
  tl_cust_free(&lib->cust);
  if (lib->dispatch_ti) ITypeInfo_Release(lib->dispatch_ti);
  if (lib->unknown_ti) ITypeInfo_Release(lib->unknown_ti);
  oa_free(lib);
}

/* ---- the interfaces ---------------------------------------------------------------------------------------------------------------------------------- */
static ITypeLib2Vtbl tl_lib_vtbl;
static ITypeCompVtbl tl_libcomp_vtbl;
static ICreateTypeLib2Vtbl tl_clib_vtbl;
static ITypeInfo2Vtbl tl_ti_vtbl;
static ICreateTypeInfo2Vtbl tl_cti_vtbl;
static ITypeCompVtbl tl_ticomp_vtbl;

static TlLib *tl_lib_from_lib(ITypeLib2 *i) { return CONTAINING_RECORD(i, TlLib, ITypeLib2_iface); }
static TlLib *tl_lib_from_comp(ITypeComp *i) { return CONTAINING_RECORD(i, TlLib, ITypeComp_iface); }
static TlLib *tl_lib_from_clib(ICreateTypeLib2 *i) { return CONTAINING_RECORD(i, TlLib, ICreateTypeLib2_iface); }
static TlTi *tl_ti_from_ti(ITypeInfo2 *i) { return CONTAINING_RECORD(i, TlTi, ITypeInfo2_iface); }
static TlTi *tl_ti_from_cti(ICreateTypeInfo2 *i) { return CONTAINING_RECORD(i, TlTi, ICreateTypeInfo2_iface); }
static TlTi *tl_ti_from_comp(ITypeComp *i) { return CONTAINING_RECORD(i, TlTi, ITypeComp_iface); }

static void tl_ti_init(TlTi *ti, TlType *t, BOOL alt) {
  ti->ITypeInfo2_iface.lpVtbl = &tl_ti_vtbl;
  ti->ICreateTypeInfo2_iface.lpVtbl = &tl_cti_vtbl;
  ti->ITypeComp_iface.lpVtbl = &tl_ticomp_vtbl;
  ti->ref = 0;
  ti->t = t;
  ti->alt = alt;
}

static TlLib *tl_lib_new(void) {
  TlLib *lib = oa_calloc(sizeof(TlLib));
  if (!lib) return NULL;
  lib->ITypeLib2_iface.lpVtbl = &tl_lib_vtbl;
  lib->ITypeComp_iface.lpVtbl = &tl_libcomp_vtbl;
  lib->ICreateTypeLib2_iface.lpVtbl = &tl_clib_vtbl;
  lib->ref = 1;
  lib->syskind = SYS_WIN64;
  lib->dispatch_href = (HREFTYPE)-1;
  return lib;
}

/* Adds an empty type to a library. */
static TlType *tl_lib_add_type(TlLib *lib, TYPEKIND kind) {
  TlType **slot = TL_GROW(lib->types, lib->ntypes, lib->types_cap);
  if (!slot) return NULL;
  TlType *t = oa_calloc(sizeof(TlType));
  if (!t) { lib->ntypes--; return NULL; }
  *slot = t;
  t->lib = lib;
  t->index = lib->ntypes - 1;
  t->file_offset = t->index * TL_TYPEINFO_RECORD;
  t->attr.typekind = kind;
  t->attr.memidConstructor = MEMBERID_NIL;
  t->attr.memidDestructor = MEMBERID_NIL;
  t->attr.lcid = lib->lcid;
  t->attr.cbAlignment = 4;
  t->attr.cbSizeInstance = 4;
  t->attr.tdescAlias.vt = VT_EMPTY;
  tl_ti_init(&t->ti, t, FALSE);
  tl_ti_init(&t->alt_ti, t, TRUE);
  return t;
}
static TlFunc *tl_type_add_func(TlType *t) { TlFunc *f = TL_GROW(t->funcs, t->nfuncs, t->funcs_cap); if (f) t->attr.cFuncs = (WORD)t->nfuncs; return f; }
static TlVar *tl_type_add_var(TlType *t) { TlVar *v = TL_GROW(t->vars, t->nvars, t->vars_cap); if (v) t->attr.cVars = (WORD)t->nvars; return v; }
static TlImpl *tl_type_add_impl(TlType *t) { TlImpl *i = TL_GROW(t->impls, t->nimpls, t->impls_cap); if (i) t->attr.cImplTypes = (WORD)t->nimpls; return i; }

#include "typelib_core.inc"
#include "typelib_msft.inc"
#include "typelib_sltg.inc"
#include "typelib_load.inc"
#include "typelib_comp.inc"
#include "typelib_invoke.inc"
#include "typelib_create.inc"
#include "typelib_reg.inc"
#include "typelib_record.inc"
