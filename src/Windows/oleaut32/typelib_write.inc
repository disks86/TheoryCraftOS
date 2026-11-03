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

/* The MSFT writer: SaveAllChanges. The tables are built in growing byte buffers (names and GUIDs shared, with their hash chains), then the file is assembled in the order
 * MIDL writes it. */

typedef struct { BYTE *d; SIZE_T n, cap; BOOL oom; } TlBuf;
static void tb_put(TlBuf *b, const void *p, SIZE_T n) {
  if (b->oom) return;
  if (n > 0x7fffffffu || b->n + n > 0x7fffffffu) { b->oom = TRUE; return; }
  if (b->n + n > b->cap) {
    SIZE_T nc = b->cap ? b->cap * 2 : 256;
    while (nc < b->n + n) nc *= 2;
    BYTE *nd = oa_realloc(b->d, nc);
    if (!nd) { b->oom = TRUE; return; }
    b->d = nd; b->cap = nc;
  }
  if (p) memcpy(b->d + b->n, p, n); else memset(b->d + b->n, 0, n);
  b->n += n;
}
static void tb_u32(TlBuf *b, DWORD v) { BYTE x[4] = { (BYTE)v, (BYTE)(v >> 8), (BYTE)(v >> 16), (BYTE)(v >> 24) }; tb_put(b, x, 4); }
static void tb_u16(TlBuf *b, WORD v) { BYTE x[2] = { (BYTE)v, (BYTE)(v >> 8) }; tb_put(b, x, 2); }
static void tb_pad4(TlBuf *b) { static const BYTE w[4] = { 'W', 'W', 'W', 'W' }; if (b->n & 3) tb_put(b, w, 4 - (b->n & 3)); }
static void tb_set32(TlBuf *b, SIZE_T at, DWORD v) { if (!b->oom && at + 4 <= b->n) { b->d[at] = (BYTE)v; b->d[at + 1] = (BYTE)(v >> 8); b->d[at + 2] = (BYTE)(v >> 16); b->d[at + 3] = (BYTE)(v >> 24); } }

typedef struct { WCHAR *text; INT off; } TlWName;
typedef struct {
  TlLib *lib;
  UINT cp;
  TlBuf names, strings, guids, typedescs, arrays, custdata, cdguids, refs;
  INT name_hash[128], guid_hash[32];
  TlWName *nlist; UINT nn, ncap;
  WCHAR **slist; INT *soff; UINT ns, scap;
  UINT nnamechars;
  BOOL fail;
} TlW;

static void w_ansi(TlW *w, const WCHAR *s, BYTE **out, UINT *len) {
  const UINT n = (UINT)oa_wcslen(s);
  BYTE *b = oa_alloc((SIZE_T)n * 2 + 1);
  *out = b; *len = 0;
  if (!b) { w->fail = TRUE; return; }
  int r = n ? WideCharToMultiByte(w->cp, 0, s, (int)n, (LPSTR)b, (int)(n * 2), NULL, NULL) : 0;
  if (n && r <= 0) { for (UINT i = 0; i < n; i++) b[i] = s[i] < 256 ? (BYTE)s[i] : '?'; r = (int)n; }
  *len = (UINT)r;
}
static INT w_string(TlW *w, const WCHAR *s) {
  if (!s) return -1;
  for (UINT i = 0; i < w->ns; i++) if (!oa_wcscmp(w->slist[i], s)) return w->soff[i];
  BYTE *a; UINT len;
  w_ansi(w, s, &a, &len);
  if (!a) return -1;
  if (len > 0xfffe) len = 0xfffe;
  const INT off = (INT)w->strings.n;
  tb_u16(&w->strings, (WORD)len);
  tb_put(&w->strings, a, len);
  oa_free(a);
  if ((w->strings.n - (SIZE_T)off) < 8) tb_put(&w->strings, "WWWWWW", 8 - (w->strings.n - (SIZE_T)off));
  tb_pad4(&w->strings);
  WCHAR *copy = tl_wcsdup(s);
  if (!copy || !tl_grow((void **)&w->slist, &w->ns, &w->scap, sizeof(*w->slist))) { oa_free(copy); w->fail = TRUE; return off; }
  INT *so = oa_realloc(w->soff, (SIZE_T)w->scap * sizeof(INT));
  if (!so) { w->fail = TRUE; return off; }
  w->soff = so;
  w->slist[w->ns - 1] = copy;
  w->soff[w->ns - 1] = off;
  return off;
}
/* flags: the byte after the length in a name's header */
static INT w_name(TlW *w, const WCHAR *s, INT owner, BYTE flags) {
  if (!s) return -1;
  for (UINT i = 0; i < w->nn; i++) if (!tl_wcsicmp(w->nlist[i].text, s)) return w->nlist[i].off;
  BYTE *a; UINT len;
  w_ansi(w, s, &a, &len);
  if (!a) return -1;
  if (len > 255) len = 255;
  char *tmp = oa_alloc(len + 1);
  if (!tmp) { oa_free(a); w->fail = TRUE; return -1; }
  memcpy(tmp, a, len); tmp[len] = 0;
  const ULONG hash = LHashValOfNameSysA(w->lib->syskind, w->lib->file_lcid, tmp) & 0xffff;
  oa_free(tmp);
  const INT off = (INT)w->names.n;
  tb_u32(&w->names, (DWORD)owner);
  tb_u32(&w->names, (DWORD)w->name_hash[hash & 0x7f]);
  tb_u32(&w->names, len | (DWORD)flags << 8 | hash << 16);
  tb_put(&w->names, a, len);
  oa_free(a);
  tb_pad4(&w->names);
  w->name_hash[hash & 0x7f] = off;
  w->nnamechars += len;
  WCHAR *copy = tl_wcsdup(s);
  if (!copy || !tl_grow((void **)&w->nlist, &w->nn, &w->ncap, sizeof(*w->nlist))) { oa_free(copy); w->fail = TRUE; return off; }
  w->nlist[w->nn - 1].text = copy;
  w->nlist[w->nn - 1].off = off;
  return off;
}
static INT w_guid(TlW *w, const GUID *g, INT owner) {
  for (SIZE_T o = 0; o + 24 <= w->guids.n; o += 24) {
    GUID e;
    memcpy(&e, w->guids.d + o, 16);
    if (IsEqualGUID(&e, g)) return (INT)o;
  }
  const INT off = (INT)w->guids.n;
  unsigned x = 0;
  for (int i = 0; i < 8; i++) x ^= (unsigned)((const WORD *)g)[i];
  x &= 0x1f;
  tb_u32(&w->guids, g->Data1); tb_u16(&w->guids, g->Data2); tb_u16(&w->guids, g->Data3); tb_put(&w->guids, g->Data4, 8);
  tb_u32(&w->guids, (DWORD)owner);
  tb_u32(&w->guids, (DWORD)w->guid_hash[x]);
  w->guid_hash[x] = off;
  return off;
}
static void w_set_guid_owner(TlW *w, INT off, INT owner) { if (off >= 0) tb_set32(&w->guids, (SIZE_T)off + 16, (DWORD)owner); }

static INT w_typedesc(TlW *w, const TYPEDESC *td, UINT depth) {
  if (depth > 32) { w->fail = TRUE; return -1; }
  const VARTYPE vt = td->vt;
  if (vt != VT_PTR && vt != VT_SAFEARRAY && vt != VT_CARRAY && vt != VT_USERDEFINED) {
    const DWORD store = vt == VT_INT ? 3 : vt == VT_UINT ? 19 : vt == VT_VOID ? 0 : vt;
    return (INT)(0x80000000u | (store << 16) | vt);
  }
  INT next = 0;
  if (vt == VT_PTR || vt == VT_SAFEARRAY) next = td->lptdesc ? w_typedesc(w, td->lptdesc, depth + 1) : (INT)0x80000018;
  else if (vt == VT_USERDEFINED) next = (INT)td->hreftype;
  else {
    const ARRAYDESC *ad = td->lpadesc;
    if (!ad) { w->fail = TRUE; return -1; }
    const INT elem = w_typedesc(w, &ad->tdescElem, depth + 1);
    next = (INT)w->arrays.n;
    tb_u32(&w->arrays, (DWORD)elem);
    tb_u32(&w->arrays, ad->cDims | 0x80000u);
    for (UINT i = 0; i < ad->cDims; i++) { tb_u32(&w->arrays, ad->rgbounds[i].cElements); tb_u32(&w->arrays, (DWORD)ad->rgbounds[i].lLbound); }
  }
  const INT off = (INT)w->typedescs.n;
  tb_u32(&w->typedescs, 0x7fff0000u | vt);
  tb_u32(&w->typedescs, (DWORD)next);
  return off;
}
/* a value: immediate when it fits, else an entry of the custom data segment */
static INT w_value(TlW *w, const VARIANT *v) {
  const VARTYPE vt = V_VT(v);
  switch (vt) {
    case VT_I2: case VT_I4: case VT_INT: case VT_UI2: case VT_UI4: case VT_UINT: case VT_I1: case VT_UI1: case VT_BOOL: case VT_ERROR: case VT_HRESULT: {
      const DWORD x = vt == VT_I4 || vt == VT_INT || vt == VT_UI4 || vt == VT_UINT || vt == VT_ERROR || vt == VT_HRESULT ? (DWORD)V_I4(v) : vt == VT_BOOL || vt == VT_I2 || vt == VT_UI2 ? (DWORD)(WORD)V_I2(v) : (DWORD)V_UI1(v);
      if (x <= 0x3ffffffu && vt != VT_BOOL && vt != VT_I2) return (INT)(0x80000000u | (DWORD)vt << 26 | x);
      const INT off = (INT)w->custdata.n;
      tb_u16(&w->custdata, vt); tb_u32(&w->custdata, vt == VT_I2 ? (DWORD)(INT)V_I2(v) : vt == VT_BOOL ? (DWORD)(INT)V_BOOL(v) : x); tb_pad4(&w->custdata);
      return off;
    }
    case VT_R4: case VT_UI8: case VT_I8: case VT_R8: case VT_CY: case VT_DATE: {
      const INT off = (INT)w->custdata.n;
      tb_u16(&w->custdata, vt);
      if (vt == VT_R4) { DWORD r4; memcpy(&r4, &V_R4(v), 4); tb_u32(&w->custdata, r4); } else { tb_u32(&w->custdata, (DWORD)V_UI8(v)); tb_u32(&w->custdata, (DWORD)(V_UI8(v) >> 32)); }
      tb_pad4(&w->custdata);
      return off;
    }
    case VT_BSTR: {
      BYTE *a; UINT len;
      w_ansi(w, V_BSTR(v) ? V_BSTR(v) : L"", &a, &len);
      if (!a) return -1;
      const INT off = (INT)w->custdata.n;
      tb_u16(&w->custdata, VT_BSTR); tb_u32(&w->custdata, len); tb_put(&w->custdata, a, len); tb_pad4(&w->custdata);
      oa_free(a);
      return off;
    }
    default: {
      const INT off = (INT)w->custdata.n;
      tb_u16(&w->custdata, VT_EMPTY); tb_pad4(&w->custdata);
      return off;
    }
  }
}
static INT w_cust(TlW *w, const TlCustList *l) {
  INT head = -1;
  for (UINT i = 0; i < l->count; i++) {
    const INT g = w_guid(w, &l->items[i].guid, -1), v = w_value(w, &l->items[i].value);
    const INT off = (INT)w->cdguids.n;
    tb_u32(&w->cdguids, (DWORD)g); tb_u32(&w->cdguids, (DWORD)v); tb_u32(&w->cdguids, (DWORD)head);
    head = off;
  }
  return head;
}
/* the memory the FUNCDESC of a function needs in the 32 bit layout, as MIDL records it */
static UINT w_td_extra(const TYPEDESC *td) {
  if ((td->vt == VT_PTR || td->vt == VT_SAFEARRAY) && td->lptdesc) return 8 + w_td_extra(td->lptdesc);
  if (td->vt == VT_CARRAY && td->lpadesc) return 16 + 8 * td->lpadesc->cDims + w_td_extra(&td->lpadesc->tdescElem);
  return 0;
}

static HRESULT tl_write_file(const WCHAR *path, const BYTE *d, SIZE_T n) {
  HANDLE f = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
  if (f == INVALID_HANDLE_VALUE) return TYPE_E_IOERROR;
  HRESULT hr = S_OK;
  for (SIZE_T done = 0; done < n && hr == S_OK;) {
    DWORD put = 0;
    if (!WriteFile(f, d + done, (DWORD)(n - done > 0x100000 ? 0x100000 : n - done), &put, NULL) || !put) hr = TYPE_E_IOERROR; else done += put;
  }
  CloseHandle(f);
  return hr;
}

static HRESULT tl_save(TlLib *lib) {
  TlW w;
  memset(&w, 0, sizeof(w));
  w.lib = lib;
  w.cp = ms_codepage(lib->file_lcid);
  memset(w.name_hash, 0xff, sizeof(w.name_hash));
  memset(w.guid_hash, 0xff, sizeof(w.guid_hash));
  TlBuf typeinfos = { 0 }, impinfo = { 0 }, impfiles = { 0 }, blocks = { 0 };
  const UINT nt = lib->ntypes;
  INT *memoffs = oa_calloc(((SIZE_T)nt + 1) * sizeof(INT));
  if (!memoffs) return E_OUTOFMEMORY;
  /* names and GUIDs in the order the library mentions them */
  const BOOL has_guid = memcmp(&lib->guid, &tl_zero_guid, sizeof(GUID)) != 0;
  if (has_guid) w_guid(&w, &lib->guid, -2);
  const INT libname = w_name(&w, lib->name, -1, 0);
  const INT helpfile = w_string(&w, lib->helpfile), libdoc = w_string(&w, lib->doc), hsdll = w_string(&w, lib->helpstringdll);
  const INT libcust = w_cust(&w, &lib->cust);
  for (UINT i = 0; i < lib->nimpfiles; i++) {
    const TlImpFile *f = &lib->impfiles[i];
    const INT g = w_guid(&w, &f->guid, 2);
    tb_u32(&impfiles, (DWORD)g); tb_u32(&impfiles, f->lcid); tb_u32(&impfiles, f->major | (DWORD)f->minor << 16);
    BYTE *a; UINT len;
    w_ansi(&w, f->name ? f->name : L"", &a, &len);
    if (a) { tb_u16(&impfiles, (WORD)(len << 2 | 1)); tb_put(&impfiles, a, len); tb_pad4(&impfiles); oa_free(a); }
    /* the offset of each file is needed by its imported types: kept in lib order, recomputed below */
  }
  /* offsets of the import files */
  UINT *fileoff = oa_calloc(((SIZE_T)lib->nimpfiles + 1) * sizeof(UINT));
  if (!fileoff) { oa_free(memoffs); return E_OUTOFMEMORY; }
  { SIZE_T o = 0; for (UINT i = 0; i < lib->nimpfiles; i++) { fileoff[i] = (UINT)o; WORD sz = (WORD)(impfiles.d[o + 12] | impfiles.d[o + 13] << 8); o += 12 + ((2u + (sz >> 2) + 3) & ~3u); } }
  for (UINT i = 0; i < lib->nimpinfos; i++) {
    const TlImpInfo *ii = &lib->impinfos[i];
    const INT g = w_guid(&w, &ii->guid, (INT)(i * TL_IMPINFO_RECORD + 1));
    w_set_guid_owner(&w, g, (INT)(i * TL_IMPINFO_RECORD + 1));
    tb_u32(&impinfo, (DWORD)ii->tkind << 24 | 0x10000u | (ii->offset & 0xffff));
    tb_u32(&impinfo, ii->file < lib->nimpfiles ? fileoff[ii->file] : 0);
    tb_u32(&impinfo, (DWORD)g);
  }
  oa_free(fileoff);
  /* each type */
  UINT totalnames = lib->name ? 1 : 0;
  for (UINT ti = 0; ti < nt; ti++) {
    TlType *t = lib->types[ti];
    t->file_offset = ti * TL_TYPEINFO_RECORD;
    const INT owner = (INT)t->file_offset;
    const BYTE tflag = t->attr.typekind == TKIND_MODULE ? 0x28 : 0x38;
    const INT posguid = IsEqualGUID(&t->attr.guid, &tl_zero_guid) ? -1 : w_guid(&w, &t->attr.guid, owner);
    const INT name = w_name(&w, t->name, owner, tflag);
    const INT doc = w_string(&w, t->doc);
    const INT cust = w_cust(&w, &t->cust);
    INT dtype1 = -1;
    DWORD dtype2 = 0;
    if (t->attr.typekind == TKIND_ALIAS) dtype1 = w_typedesc(&w, &t->attr.tdescAlias, 0);
    else if (t->attr.typekind == TKIND_MODULE) dtype1 = w_string(&w, t->dllname);
    else if (t->attr.typekind == TKIND_COCLASS) {
      dtype1 = t->nimpls ? (INT)w.refs.n : -1;
      for (UINT i = 0; i < t->nimpls; i++) {
        const INT c = w_cust(&w, &t->impls[i].cust);
        tb_u32(&w.refs, t->impls[i].href); tb_u32(&w.refs, (DWORD)t->impls[i].flags); tb_u32(&w.refs, (DWORD)c);
        tb_u32(&w.refs, i + 1 < t->nimpls ? (DWORD)(w.refs.n + 4) : 0xffffffffu);
      }
    } else if ((t->attr.typekind == TKIND_INTERFACE || t->attr.typekind == TKIND_DISPATCH) && t->nimpls) {
      dtype1 = (INT)t->impls[0].href;
      ITypeInfo *chain[TL_MAX_BASES];
      const UINT n = tl_ti_chain(&t->ti, chain);
      UINT funcs = 0;
      for (UINT i = 0; i < n; i++) { TYPEATTR *ta; if (SUCCEEDED(ITypeInfo_GetTypeAttr(chain[i], &ta))) { funcs += ta->cFuncs; ITypeInfo_ReleaseTypeAttr(chain[i], ta); } }
      tl_chain_release(chain, n);
      dtype2 = funcs << 16 | n;
    }
    /* members */
    TlBuf recs = { 0 }, ids = { 0 }, nms = { 0 };
    UINT size1 = 0;
    for (UINT i = 0; i < t->nfuncs; i++) {
      TlFunc *f = &t->funcs[i];
      const UINT np = (UINT)f->fd.cParams;
      const INT fname = w_name(&w, f->name, owner, t->attr.typekind == TKIND_MODULE ? 0x30 : 0);
      totalnames++;
      const INT fdoc = w_string(&w, f->doc);
      INT entry = -1;
      if (f->has_ordinal && !f->dll_entry) entry = (INT)(0xffff0000u | f->ordinal);
      else if (f->dll_entry) entry = w_string(&w, f->dll_entry);
      const INT fcust = w_cust(&w, &f->cust);
      INT *pcust = oa_calloc((np + 1) * sizeof(INT));
      INT *defs = oa_calloc((np + 1) * sizeof(INT));
      if (!pcust || !defs) { oa_free(pcust); oa_free(defs); w.fail = TRUE; break; }
      BOOL anycust = fcust >= 0, anydef = FALSE;
      for (UINT p = 0; p < np; p++) {
        pcust[p] = f->pcust ? w_cust(&w, &f->pcust[p]) : -1;
        if (pcust[p] >= 0) anycust = TRUE;
        defs[p] = -1;
        const ELEMDESC *e = &f->fd.lprgelemdescParam[p];
        if ((e->paramdesc.wParamFlags & PARAMFLAG_FHASDEFAULT) && e->paramdesc.pparamdescex) { defs[p] = w_value(&w, &e->paramdesc.pparamdescex->varDefaultValue); anydef = TRUE; }
      }
      UINT fsize = 52 + 16 * np + w_td_extra(&f->fd.elemdescFunc.tdesc);
      BOOL retval = FALSE;
      for (UINT p = 0; p < np; p++) { fsize += w_td_extra(&f->fd.lprgelemdescParam[p].tdesc); if (f->fd.lprgelemdescParam[p].paramdesc.wParamFlags & PARAMFLAG_FRETVAL) retval = TRUE; }
      size1 += fsize;
      const UINT optwords = 6;
      const UINT recsize = 24 + optwords * 4 + (anycust ? (np + 1) * 4 : 0) + (anydef ? np * 4 : 0) + np * 12;
      const SIZE_T start = recs.n;
      tb_u32(&recs, recsize | i << 16);
      tb_u32(&recs, (DWORD)w_typedesc(&w, &f->fd.elemdescFunc.tdesc, 0));
      tb_u32(&recs, f->fd.wFuncFlags);
      tb_u32(&recs, (WORD)f->fd.oVft | fsize << 16);
      tb_u32(&recs, (DWORD)f->fd.funckind | (DWORD)f->fd.invkind << 3 | (DWORD)f->fd.callconv << 8 | (anycust ? 0x80u : 0) | (anydef ? 0x1000u : 0) | (retval ? 0x4000u : 0) | i << 16);
      tb_u32(&recs, np | (DWORD)(WORD)f->fd.cParamsOpt << 16);
      tb_u32(&recs, f->help_context); tb_u32(&recs, (DWORD)fdoc); tb_u32(&recs, (DWORD)entry); tb_u32(&recs, 0xffffffffu); tb_u32(&recs, 0xffffffffu); tb_u32(&recs, f->help_string_context);
      if (anycust) { tb_u32(&recs, (DWORD)fcust); for (UINT p = 0; p < np; p++) tb_u32(&recs, (DWORD)pcust[p]); }
      if (anydef) for (UINT p = 0; p < np; p++) tb_u32(&recs, (DWORD)defs[p]);
      for (UINT p = 0; p < np; p++) {
        const ELEMDESC *e = &f->fd.lprgelemdescParam[p];
        tb_u32(&recs, (DWORD)w_typedesc(&w, &e->tdesc, 0));
        const INT pn = f->pnames && f->pnames[p] ? w_name(&w, f->pnames[p], -1, 0) : -1;
        if (pn >= 0) totalnames++;
        tb_u32(&recs, (DWORD)pn);
        tb_u32(&recs, e->paramdesc.wParamFlags);
      }
      (void)start;
      oa_free(pcust); oa_free(defs);
      tb_u32(&ids, (DWORD)f->fd.memid);
      tb_u32(&nms, (DWORD)fname);
    }
    for (UINT i = 0; i < t->nvars; i++) {
      TlVar *v = &t->vars[i];
      const INT vname = w_name(&w, v->name, owner, t->attr.typekind == TKIND_ENUM || t->attr.typekind == TKIND_MODULE ? 0x30 : 0x10);
      totalnames++;
      const INT vdoc = w_string(&w, v->doc), vcust = w_cust(&w, &v->cust);
      const UINT vsize = 36 + (v->vd.varkind == VAR_CONST ? 16 : 0) + w_td_extra(&v->vd.elemdescVar.tdesc);
      size1 += vsize;
      tb_u32(&recs, 40u | (t->nfuncs + i) << 16);
      tb_u32(&recs, (DWORD)w_typedesc(&w, &v->vd.elemdescVar.tdesc, 0));
      tb_u32(&recs, v->vd.wVarFlags);
      tb_u32(&recs, (WORD)v->vd.varkind | vsize << 16);
      tb_u32(&recs, v->vd.varkind == VAR_CONST ? (DWORD)w_value(&w, v->vd.lpvarValue) : v->vd.oInst);
      tb_u32(&recs, v->help_context); tb_u32(&recs, (DWORD)vdoc); tb_u32(&recs, 0); tb_u32(&recs, (DWORD)vcust); tb_u32(&recs, v->help_string_context);
      tb_u32(&ids, (DWORD)v->vd.memid);
      tb_u32(&nms, (DWORD)vname);
    }
    memoffs[ti] = -1;
    if (t->nfuncs + t->nvars) {
      memoffs[ti] = (INT)blocks.n;                  /* (relative: made absolute below) */
      tb_u32(&blocks, (DWORD)recs.n);
      tb_put(&blocks, recs.d, recs.n);
      tb_put(&blocks, ids.d, ids.n);
      tb_put(&blocks, nms.d, nms.n);
    }
    oa_free(recs.d); oa_free(ids.d); oa_free(nms.d);
    const TYPEKIND wkind = t->attr.typekind == TKIND_INTERFACE && (t->attr.wTypeFlags & TYPEFLAG_FDUAL) ? TKIND_DISPATCH : t->attr.typekind;
    DWORD kind = (DWORD)wkind | (DWORD)(t->attr.cbAlignment & 0x1f) << 11 | (t->attr.cbAlignment >= 8 || t->attr.typekind == TKIND_MODULE ? 0x200u : 0x100u) | 0x20u |
                 (wkind == TKIND_DISPATCH ? 0x10u : 0) | ti << 16;
    tb_u32(&typeinfos, kind);
    tb_u32(&typeinfos, (DWORD)memoffs[ti]);
    tb_u32(&typeinfos, size1 ? size1 : 0); tb_u32(&typeinfos, size1 ? size1 - 0x4c : 0xffffffffu); tb_u32(&typeinfos, 3); tb_u32(&typeinfos, 0);
    tb_u32(&typeinfos, t->nfuncs | (DWORD)t->nvars << 16);
    for (int k = 0; k < 4; k++) tb_u32(&typeinfos, 0);
    tb_u32(&typeinfos, (DWORD)posguid);
    tb_u32(&typeinfos, t->attr.wTypeFlags);
    tb_u32(&typeinfos, (DWORD)name);
    tb_u32(&typeinfos, t->attr.wMajorVerNum | (DWORD)t->attr.wMinorVerNum << 16);
    tb_u32(&typeinfos, (DWORD)doc);
    tb_u32(&typeinfos, t->help_string_context);
    tb_u32(&typeinfos, t->help_context);
    tb_u32(&typeinfos, (DWORD)cust);
    tb_u32(&typeinfos, (t->attr.typekind == TKIND_COCLASS ? t->nimpls : t->attr.cImplTypes) | (DWORD)t->attr.cbSizeVft << 16);
    tb_u32(&typeinfos, (DWORD)t->attr.cbSizeInstance);
    tb_u32(&typeinfos, (DWORD)dtype1);
    tb_u32(&typeinfos, dtype2);
    tb_u32(&typeinfos, 0);
    tb_u32(&typeinfos, 0xffffffffu);
  }
  (void)libname;
  /* the layout of the file */
  const INT hdr_len = MSFT_HEADER_SIZE + (lib->helpstringdll ? 4 : 0);
  const INT segdir = hdr_len + (INT)nt * 4, base = segdir + MSFT_SEGMENTS * 16;
  TlBuf *seg[MSFT_SEGMENTS] = { &typeinfos, &impinfo, &impfiles, &w.refs, NULL, &w.guids, NULL, &w.names, &w.strings, &w.typedescs, &w.arrays, &w.custdata, &w.cdguids, NULL, NULL };
  TlBuf hashg = { 0 }, hashn = { 0 };
  tb_put(&hashg, w.guid_hash, sizeof(w.guid_hash));
  tb_put(&hashn, w.name_hash, sizeof(w.name_hash));
  seg[MS_GUIDHASH] = &hashg;
  seg[MS_NAMEHASH] = &hashn;
  INT segoff[MSFT_SEGMENTS];
  SIZE_T pos = (SIZE_T)base;
  for (int i = 0; i < MSFT_SEGMENTS; i++) { segoff[i] = seg[i] && seg[i]->n ? (INT)pos : (i == MS_GUIDHASH || i == MS_NAMEHASH ? (INT)pos : -1); if (segoff[i] >= 0) pos += seg[i]->n; }
  const SIZE_T blockbase = pos;
  /* the member blocks of types without members point past the file */
  const SIZE_T total = blockbase + blocks.n;
  for (UINT ti = 0; ti < nt; ti++) tb_set32(&typeinfos, (SIZE_T)ti * TL_TYPEINFO_RECORD + 4, memoffs[ti] < 0 ? (DWORD)blockbase : (DWORD)(blockbase + (SIZE_T)memoffs[ti]));
  TlBuf out = { 0 };
  const DWORD varflags = 0x40u | (DWORD)lib->syskind | (lib->helpfile ? 0x10u : 0) | (lib->helpstringdll ? 0x100u : 0);
  tb_u32(&out, MSFT_MAGIC1); tb_u32(&out, MSFT_MAGIC2);
  tb_u32(&out, has_guid ? 0 : 0xffffffffu);        /* (the library's GUID is the first of the table) */
  tb_u32(&out, lib->file_lcid); tb_u32(&out, lib->lcid); tb_u32(&out, varflags);
  tb_u32(&out, lib->major | (DWORD)lib->minor << 16); tb_u32(&out, lib->flags & ~(DWORD)LIBFLAG_FHASDISKIMAGE);
  tb_u32(&out, nt); tb_u32(&out, (DWORD)libdoc); tb_u32(&out, lib->help_string_context); tb_u32(&out, lib->help_context);
  tb_u32(&out, totalnames + nt); tb_u32(&out, w.nnamechars); tb_u32(&out, (DWORD)libname); tb_u32(&out, (DWORD)helpfile); tb_u32(&out, (DWORD)libcust);
  tb_u32(&out, 0x20); tb_u32(&out, 0x80); tb_u32(&out, lib->dispatch_href); tb_u32(&out, lib->nimpinfos);
  if (lib->helpstringdll) tb_u32(&out, (DWORD)hsdll);
  for (UINT i = 0; i < nt; i++) tb_u32(&out, i * TL_TYPEINFO_RECORD);
  for (int i = 0; i < MSFT_SEGMENTS; i++) { tb_u32(&out, (DWORD)segoff[i]); tb_u32(&out, seg[i] ? (DWORD)seg[i]->n : 0); tb_u32(&out, 0xffffffffu); tb_u32(&out, 0xf); }
  for (int i = 0; i < MSFT_SEGMENTS; i++) if (seg[i] && segoff[i] >= 0) tb_put(&out, seg[i]->d, seg[i]->n);
  tb_put(&out, blocks.d, blocks.n);
  HRESULT hr = out.oom || w.fail || out.n != total ? E_OUTOFMEMORY : S_OK;
  if (SUCCEEDED(hr)) hr = tl_write_file(lib->path, out.d, out.n);
  oa_free(out.d); oa_free(typeinfos.d); oa_free(impinfo.d); oa_free(impfiles.d); oa_free(blocks.d); oa_free(hashg.d); oa_free(hashn.d);
  oa_free(w.names.d); oa_free(w.strings.d); oa_free(w.guids.d); oa_free(w.typedescs.d); oa_free(w.arrays.d); oa_free(w.custdata.d); oa_free(w.cdguids.d); oa_free(w.refs.d);
  for (UINT i = 0; i < w.nn; i++) oa_free(w.nlist[i].text);
  oa_free(w.nlist);
  for (UINT i = 0; i < w.ns; i++) oa_free(w.slist[i]);
  oa_free(w.slist); oa_free(w.soff);
  oa_free(memoffs);
  return hr;
}
