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

/* tlbdump: prints everything the ITypeLib2 / ITypeInfo2 interfaces tell about a type library, in a text form that can be diffed between two oleaut32 implementations
 * (Wine's, which is the reference, and src/Windows/oleaut32).   tlbdump <file> [more...]   Build: x86_64-w64-mingw32-gcc -o tlbdump.exe tlbdump.c -loleaut32 -lole32 */
#define COBJMACROS
#define INITGUID
#include <windows.h>
#include <oleauto.h>
#include <stdio.h>

static void pr_bstr(const char *what, BSTR s) {
  if (!s) { printf("%s=<null>", what); return; }
  printf("%s=\"", what);
  for (; *s; s++) putchar(*s < 128 && *s >= 32 ? (char)*s : '?');
  printf("\"");
}
static void pr_guid(const GUID *g) {
  printf("{%08lx-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x}", g->Data1, g->Data2, g->Data3, g->Data4[0], g->Data4[1], g->Data4[2], g->Data4[3], g->Data4[4], g->Data4[5], g->Data4[6], g->Data4[7]);
}
static void pr_tdesc(const TYPEDESC *t) {
  switch (t->vt) {
  case VT_PTR: printf("PTR("); pr_tdesc(t->lptdesc); printf(")"); break;
  case VT_SAFEARRAY: printf("SAFEARRAY("); pr_tdesc(t->lptdesc); printf(")"); break;
  case VT_CARRAY: {
    printf("CARRAY(");
    pr_tdesc(&t->lpadesc->tdescElem);
    for (UINT i = 0; i < t->lpadesc->cDims; i++) printf(",[%lu,%ld]", t->lpadesc->rgbounds[i].cElements, t->lpadesc->rgbounds[i].lLbound);
    printf(")");
    break;
  }
  case VT_USERDEFINED: printf("USER(%#lx)", t->hreftype); break;
  default: printf("vt%d", t->vt);
  }
}
static void pr_variant(const VARIANT *v) {
  printf("<vt%d", V_VT(v));
  switch (V_VT(v)) {
  case VT_I2: case VT_BOOL: printf(" %d", V_I2(v)); break;
  case VT_I4: case VT_INT: case VT_UI4: case VT_UINT: case VT_ERROR: case VT_HRESULT: printf(" %ld", V_I4(v)); break;
  case VT_UI1: case VT_I1: printf(" %d", V_UI1(v)); break;
  case VT_R4: printf(" %g", V_R4(v)); break;
  case VT_R8: case VT_DATE: printf(" %g", V_R8(v)); break;
  case VT_BSTR: printf(" "); pr_bstr("s", V_BSTR(v)); break;
  default: break;
  }
  printf(">");
}
static void pr_elem(const ELEMDESC *e) {
  pr_tdesc(&e->tdesc);
  printf(" flags=%#x", e->paramdesc.wParamFlags);
  if (e->paramdesc.wParamFlags & PARAMFLAG_FHASDEFAULT) {
    printf(" def="); if (e->paramdesc.pparamdescex) { printf("(%lu)", e->paramdesc.pparamdescex->cBytes); pr_variant(&e->paramdesc.pparamdescex->varDefaultValue); } else printf("NULL");
  }
}
static void pr_cust(ITypeInfo2 *ti2, ITypeLib2 *tl2) {
  CUSTDATA cd;
  HRESULT hr = ti2 ? ITypeInfo2_GetAllCustData(ti2, &cd) : ITypeLib2_GetAllCustData(tl2, &cd);
  if (FAILED(hr)) { printf(" custall hr=%#lx", hr); return; }
  for (DWORD i = 0; i < cd.cCustData; i++) { printf(" cust:"); pr_guid(&cd.prgCustData[i].guid); pr_variant(&cd.prgCustData[i].varValue); }
  ClearCustData(&cd);
}
static void pr_doc(ITypeInfo *ti, MEMBERID id) {
  BSTR n = NULL, d = NULL, f = NULL; DWORD hc = 0;
  HRESULT hr = ITypeInfo_GetDocumentation(ti, id, &n, &d, &hc, &f);
  printf(" doc(hr=%#lx ", hr); pr_bstr("name", n); printf(" "); pr_bstr("doc", d); printf(" hc=%lu ", hc); pr_bstr("file", f); printf(")");
  SysFreeString(n); SysFreeString(d); SysFreeString(f);
}

static void dump_type(ITypeLib *tl, UINT idx) {
  ITypeInfo *ti; TYPEATTR *ta;
  HRESULT hr = ITypeLib_GetTypeInfo(tl, idx, &ti);
  printf("TYPE %u hr=%#lx\n", idx, hr);
  if (FAILED(hr)) return;
  TYPEKIND tk = (TYPEKIND)-1;
  ITypeLib_GetTypeInfoType(tl, idx, &tk);
  hr = ITypeInfo_GetTypeAttr(ti, &ta);
  if (FAILED(hr)) { printf("  GetTypeAttr hr=%#lx\n", hr); return; }
  printf("  kind=%d(%d) guid=", ta->typekind, tk); pr_guid(&ta->guid);
  printf(" lcid=%#lx ctor=%#lx dtor=%#lx size=%lu cFuncs=%u cVars=%u cImpl=%u vft=%u align=%u flags=%#x ver=%u.%u\n", ta->lcid, ta->memidConstructor, ta->memidDestructor,
         ta->cbSizeInstance, ta->cFuncs, ta->cVars, ta->cImplTypes, ta->cbSizeVft, ta->cbAlignment, ta->wTypeFlags, ta->wMajorVerNum, ta->wMinorVerNum);
  printf("  idl: flags=%#x ", ta->idldescType.wIDLFlags); pr_tdesc(&ta->tdescAlias); printf("\n");
  printf(" "); pr_doc(ti, MEMBERID_NIL);
  ITypeInfo2 *ti2;
  if (SUCCEEDED(ITypeInfo_QueryInterface(ti, &IID_ITypeInfo2, (void **)&ti2))) { pr_cust(ti2, NULL); ITypeInfo2_Release(ti2); }
  printf("\n");
  for (UINT i = 0; i < ta->cImplTypes; i++) {
    HREFTYPE href = 0; INT fl = 0; ITypeInfo *rt = NULL;
    hr = ITypeInfo_GetRefTypeOfImplType(ti, i, &href);
    printf("  impl %u hr=%#lx href=%#lx", i, hr, href);
    hr = ITypeInfo_GetImplTypeFlags(ti, i, &fl);
    printf(" flags=%d(hr=%#lx)", fl, hr);
    hr = ITypeInfo_GetRefTypeInfo(ti, href, &rt);
    printf(" refinfo hr=%#lx", hr);
    if (SUCCEEDED(hr)) { pr_doc(rt, MEMBERID_NIL); ITypeInfo_Release(rt); }
    printf("\n");
  }
  if (ta->typekind == TKIND_DISPATCH && (ta->wTypeFlags & TYPEFLAG_FDUAL)) {
    ITypeInfo *rt = NULL; HREFTYPE href = 0;
    hr = ITypeInfo_GetRefTypeOfImplType(ti, (UINT)-1, &href);
    printf("  dual impl -1 hr=%#lx href=%#lx", hr, href);
    hr = ITypeInfo_GetRefTypeInfo(ti, href, &rt);
    printf(" refinfo hr=%#lx", hr);
    if (SUCCEEDED(hr)) {
      TYPEATTR *ra; if (SUCCEEDED(ITypeInfo_GetTypeAttr(rt, &ra))) { printf(" kind=%d cFuncs=%u vft=%u flags=%#x", ra->typekind, ra->cFuncs, ra->cbSizeVft, ra->wTypeFlags); ITypeInfo_ReleaseTypeAttr(rt, ra); }
      ITypeInfo_Release(rt);
    }
    printf("\n");
  }
  for (UINT i = 0; i < ta->cFuncs; i++) {
    FUNCDESC *fd;
    hr = ITypeInfo_GetFuncDesc(ti, i, &fd);
    if (FAILED(hr)) { printf("  func %u hr=%#lx\n", i, hr); continue; }
    printf("  func %u id=%#lx kind=%d inv=%d cc=%d cParams=%d opt=%d oVft=%d cScodes=%d flags=%#x ret=", i, fd->memid, fd->funckind, fd->invkind, fd->callconv, fd->cParams, fd->cParamsOpt, fd->oVft,
           fd->cScodes, fd->wFuncFlags);
    pr_elem(&fd->elemdescFunc);
    printf("\n");
    BSTR names[40]; UINT cn = 0;
    hr = ITypeInfo_GetNames(ti, fd->memid, names, 40, &cn);
    printf("    names hr=%#lx n=%u:", hr, cn);
    for (UINT k = 0; k < cn; k++) { printf(" "); pr_bstr("", names[k]); SysFreeString(names[k]); }
    printf("\n   "); pr_doc(ti, fd->memid);
    if (SUCCEEDED(ITypeInfo_QueryInterface(ti, &IID_ITypeInfo2, (void **)&ti2))) {
      DWORD hsc = 0; HRESULT h2 = ITypeInfo2_GetDocumentation2(ti2, fd->memid, 0x409, NULL, &hsc, NULL);
      printf(" hsc(hr=%#lx)=%lu", h2, hsc);
      ITypeInfo2_Release(ti2);
    }
    printf("\n");
    for (SHORT p = 0; p < fd->cParams; p++) {
      printf("    param %d: ", p); pr_elem(&fd->lprgelemdescParam[p]);
      const TYPEDESC *td = &fd->lprgelemdescParam[p].tdesc;
      while (td->vt == VT_PTR && td->lptdesc) td = td->lptdesc;
      if (td->vt == VT_USERDEFINED) { ITypeInfo *rt = NULL; HRESULT rh = ITypeInfo_GetRefTypeInfo(ti, td->hreftype, &rt); printf(" ref hr=%#lx", rh); if (rt) ITypeInfo_Release(rt); }
      printf("\n");
    }
    if (ta->typekind == TKIND_MODULE || fd->funckind == FUNC_STATIC) {
      BSTR dll = NULL, ent = NULL; WORD ord = 0;
      hr = ITypeInfo_GetDllEntry(ti, fd->memid, fd->invkind, &dll, &ent, &ord);
      printf("    dllentry hr=%#lx ", hr); pr_bstr("dll", dll); printf(" "); pr_bstr("entry", ent); printf(" ord=%u\n", ord);
      SysFreeString(dll); SysFreeString(ent);
    }
    ITypeInfo_ReleaseFuncDesc(ti, fd);
  }
  for (UINT i = 0; i < ta->cVars; i++) {
    VARDESC *vd;
    hr = ITypeInfo_GetVarDesc(ti, i, &vd);
    if (FAILED(hr)) { printf("  var %u hr=%#lx\n", i, hr); continue; }
    printf("  var %u id=%#lx kind=%d flags=%#x type=", i, vd->memid, vd->varkind, vd->wVarFlags);
    pr_elem(&vd->elemdescVar);
    if (vd->varkind == VAR_CONST) { printf(" value="); pr_variant(vd->lpvarValue); } else printf(" oInst=%lu", vd->oInst);
    printf(" schema=%p", vd->lpstrSchema);
    BSTR names[4]; UINT cn = 0;
    hr = ITypeInfo_GetNames(ti, vd->memid, names, 4, &cn);
    printf(" names hr=%#lx n=%u:", hr, cn);
    for (UINT k = 0; k < cn; k++) { printf(" "); pr_bstr("", names[k]); SysFreeString(names[k]); }
    pr_doc(ti, vd->memid);
    printf("\n");
    ITypeInfo_ReleaseVarDesc(ti, vd);
  }
  if (ta->typekind == TKIND_MODULE) {
    /* the DLL name of a module */
  }
  ITypeInfo_ReleaseTypeAttr(ti, ta);
  ITypeInfo_Release(ti);
}

int main(int argc, char **argv) {
  for (int a = 1; a < argc; a++) {
    WCHAR path[MAX_PATH]; ITypeLib *tl = NULL;
    MultiByteToWideChar(CP_ACP, 0, argv[a], -1, path, MAX_PATH);
    HRESULT hr = LoadTypeLibEx(path, REGKIND_NONE, &tl);
    printf("LIB %s hr=%#lx\n", argv[a], hr);
    if (FAILED(hr)) continue;
    TLIBATTR *la;
    if (SUCCEEDED(ITypeLib_GetLibAttr(tl, &la))) {
      printf("  libattr "); pr_guid(&la->guid); printf(" lcid=%#lx sys=%d ver=%u.%u flags=%#x\n", la->lcid, la->syskind, la->wMajorVerNum, la->wMinorVerNum, la->wLibFlags);
      ITypeLib_ReleaseTLibAttr(tl, la);
    }
    BSTR n = NULL, d = NULL, f = NULL; DWORD hc = 0;
    hr = ITypeLib_GetDocumentation(tl, -1, &n, &d, &hc, &f);
    printf("  libdoc hr=%#lx ", hr); pr_bstr("name", n); printf(" "); pr_bstr("doc", d); printf(" hc=%lu ", hc); pr_bstr("file", f);
    SysFreeString(n); SysFreeString(d); SysFreeString(f);
    ITypeLib2 *tl2;
    if (SUCCEEDED(ITypeLib_QueryInterface(tl, &IID_ITypeLib2, (void **)&tl2))) {
      pr_cust(NULL, tl2);
      ITypeLib2_Release(tl2);
    }
    printf("\n");
    UINT count = ITypeLib_GetTypeInfoCount(tl);
    printf("  count=%u\n", count);
    for (UINT i = 0; i < count; i++) dump_type(tl, i);
    /* name lookups */
    for (UINT i = 0; i < count; i++) {
      BSTR nm = NULL;
      if (SUCCEEDED(ITypeLib_GetDocumentation(tl, i, &nm, NULL, NULL, NULL)) && nm) {
        BOOL is = FALSE; ITypeLib_IsName(tl, nm, 0, &is);
        printf("  isname "); pr_bstr("n", nm); printf(" %d", is);
        ITypeInfo *found[4]; MEMBERID ids[4]; USHORT c = 4; WCHAR copy[128]; lstrcpynW(copy, nm, 128);
        hr = ITypeLib_FindName(tl, copy, 0, found, ids, &c);
        printf(" findname hr=%#lx c=%u\n", hr, c);
        for (USHORT k = 0; k < c; k++) { BSTR fn = NULL; ITypeInfo_GetDocumentation(found[k], MEMBERID_NIL, &fn, NULL, NULL, NULL); printf("    found "); pr_bstr("type", fn); printf(" id=%#lx\n", ids[k]); SysFreeString(fn); ITypeInfo_Release(found[k]); }
        SysFreeString(nm);
      }
    }
    ITypeLib_Release(tl);
  }
  return 0;
}
