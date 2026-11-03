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

/* The MSFT type library format (what MIDL and MKTYPLIB write; typelib_create.inc writes it, too): a header, a directory of typeinfo offsets, a directory of 15 segments
 * (typeinfo records, imported types and files, references, GUID and name tables with their hash tables, strings, type descriptions, array descriptions, custom data)
 * and per typeinfo a block of function and variable records. Every offset of the file is checked against the file's size before it is used, every count against what
 * the file could hold, and the chains (names, custom data, references) are bounded by the number of their entries. */

#define MSFT_MAGIC1 0x5446534Du
#define MSFT_MAGIC2 0x00010002u
#define MSFT_HEADER_SIZE 0x54u
#define MSFT_SEGMENTS 15
enum { MS_TYPEINFO, MS_IMPINFO, MS_IMPFILES, MS_REFTAB, MS_GUIDHASH, MS_GUIDTAB, MS_NAMEHASH, MS_NAMETAB, MS_STRINGS, MS_TYPEDESC, MS_ARRAYDESC, MS_CUSTDATA, MS_CDGUIDS };

typedef struct {
  const BYTE *d;
  SIZE_T n;
  INT seg_off[MSFT_SEGMENTS], seg_len[MSFT_SEGMENTS];
  UINT cp;                            /* code page of the strings */
  TlLib *lib;
  DWORD *impfile_off; UINT nimpfile_off;
  INT dispatchpos;
} Msft;

static BOOL ms_u32(const Msft *m, INT off, DWORD *v) {
  *v = 0;
  if (off < 0 || (SIZE_T)off > m->n || m->n - (SIZE_T)off < 4) return FALSE;
  const BYTE *p = m->d + off;
  *v = (DWORD)p[0] | (DWORD)p[1] << 8 | (DWORD)p[2] << 16 | (DWORD)p[3] << 24;
  return TRUE;
}
static BOOL ms_u16(const Msft *m, INT off, WORD *v) {
  *v = 0;
  if (off < 0 || (SIZE_T)off > m->n || m->n - (SIZE_T)off < 2) return FALSE;
  *v = (WORD)(m->d[off] | m->d[off + 1] << 8);
  return TRUE;
}
/* The file offset of byte `rel` of a segment, or -1 if the segment is absent or the offset is outside the file. */
static INT ms_seg(const Msft *m, int seg, INT rel) {
  if (m->seg_off[seg] < 0 || rel < 0) return -1;
  const SIZE_T at = (SIZE_T)m->seg_off[seg] + (SIZE_T)rel;
  return at < m->n ? (INT)at : -1;
}
static UINT ms_codepage(LCID lcid) {
  DWORD cp = 0;
  if (lcid && GetLocaleInfoW(lcid, LOCALE_IDEFAULTANSICODEPAGE | LOCALE_RETURN_NUMBER, (LPWSTR)&cp, sizeof(cp) / sizeof(WCHAR)) && cp != CP_UTF8) return cp;
  return CP_ACP;
}
/* ANSI bytes to a heap string */
static WCHAR *ms_ansi(const Msft *m, const BYTE *s, UINT len) {
  WCHAR *w = oa_alloc(((SIZE_T)len + 1) * sizeof(WCHAR));
  if (!w) return NULL;
  UINT n = len ? (UINT)MultiByteToWideChar(m->cp, 0, (LPCSTR)s, (int)len, w, (int)len) : 0;
  if (len && !n) { for (UINT i = 0; i < len; i++) w[i] = s[i]; n = len; }
  w[n] = 0;
  return w;
}
/* A string of the string table (a 16 bit length and the characters) */
static WCHAR *ms_string(const Msft *m, INT off) {
  if (off < 0) return NULL;
  const INT at = ms_seg(m, MS_STRINGS, off);
  WORD len;
  if (at < 0 || !ms_u16(m, at, &len)) return NULL;
  if (len == 0xffff || (SIZE_T)at + 2 + len > m->n) return NULL;
  return ms_ansi(m, m->d + at + 2, len);
}
static WCHAR *ms_name(const Msft *m, INT off) {
  if (off < 0) return NULL;
  const INT at = ms_seg(m, MS_NAMETAB, off);
  DWORD lenhash;
  if (at < 0 || !ms_u32(m, at + 8, &lenhash)) return NULL;
  const UINT len = lenhash & 0xff;
  if ((SIZE_T)at + 12 + len > m->n) return NULL;
  return ms_ansi(m, m->d + at + 12, len);
}
static BOOL ms_guid(const Msft *m, INT off, GUID *g) {
  const INT at = off < 0 ? -1 : ms_seg(m, MS_GUIDTAB, off);
  if (at < 0 || (SIZE_T)at + 16 > m->n) { memset(g, 0, sizeof(*g)); return FALSE; }
  DWORD a; WORD b, c;
  ms_u32(m, at, &a); ms_u16(m, at + 4, &b); ms_u16(m, at + 6, &c);
  g->Data1 = a; g->Data2 = b; g->Data3 = c;
  memcpy(g->Data4, m->d + at + 8, 8);
  return TRUE;
}

/* A typed value of the custom data segment, or an immediate one (the high bit set: the type in bits 26..30, the value in the 26 bits below). */
static HRESULT ms_value(const Msft *m, INT off, VARIANT *v) {
  VariantInit(v);
  if (off < 0) {
    V_VT(v) = (VARTYPE)(((DWORD)off & 0x7c000000u) >> 26);
    V_I4(v) = (LONG)((DWORD)off & 0x3ffffffu);
    if (V_VT(v) == VT_BSTR || V_VT(v) == VT_DISPATCH || V_VT(v) == VT_UNKNOWN || V_VT(v) == VT_VARIANT || V_VT(v) > VT_CLSID) { V_VT(v) = VT_EMPTY; return E_FAIL; }
    return S_OK;
  }
  const INT at = ms_seg(m, MS_CUSTDATA, off);
  WORD vt;
  if (at < 0 || !ms_u16(m, at, &vt)) return E_FAIL;
  const INT data = at + 2;
  switch (vt) {
    case VT_EMPTY: case VT_NULL: return S_OK;
    case VT_I2: case VT_I4: case VT_R4: case VT_ERROR: case VT_BOOL: case VT_I1: case VT_UI1: case VT_UI2: case VT_UI4: case VT_INT: case VT_UINT: case VT_HRESULT: {
      DWORD x;
      if (!ms_u32(m, data, &x)) return E_FAIL;
      V_VT(v) = vt;
      V_I4(v) = (LONG)x;
      if (vt == VT_BOOL) V_BOOL(v) = (VARIANT_BOOL)x;
      return S_OK;
    }
    case VT_R8: case VT_CY: case VT_DATE: case VT_I8: case VT_UI8: case VT_FILETIME: {
      DWORD lo, hi;
      if (!ms_u32(m, data, &lo) || !ms_u32(m, data + 4, &hi)) return E_FAIL;
      V_VT(v) = vt;
      V_UI8(v) = (ULONGLONG)lo | (ULONGLONG)hi << 32;
      return S_OK;
    }
    case VT_BSTR: case VT_LPSTR: {
      DWORD len;
      if (!ms_u32(m, data, &len)) return E_FAIL;
      len &= 0xffff;
      if ((SIZE_T)data + 4 + len > m->n) return E_FAIL;
      WCHAR *w = ms_ansi(m, m->d + data + 4, len);
      if (!w) return E_OUTOFMEMORY;
      V_BSTR(v) = SysAllocStringLen(w, (UINT)oa_wcslen(w));
      oa_free(w);
      if (!V_BSTR(v)) return E_OUTOFMEMORY;
      V_VT(v) = VT_BSTR;
      return S_OK;
    }
    default: return E_FAIL;
  }
}

static HRESULT ms_custdata(const Msft *m, INT off, TlCustList *list) {
  if (off < 0 || m->seg_off[MS_CDGUIDS] < 0) return S_OK;
  const UINT limit = (UINT)m->seg_len[MS_CDGUIDS] / 12 + 1;
  INT chain[64];
  UINT nchain = 0;
  for (UINT i = 0; off >= 0 && i < limit && nchain < 64; i++) {
    chain[nchain++] = off;
    const INT at = ms_seg(m, MS_CDGUIDS, off);
    DWORD next;
    if (at < 0 || !ms_u32(m, at + 8, &next)) break;
    off = (INT)next;
  }
  /* (the entries of a chain are listed from the last: that is the order the reference implementation shows) */
  for (UINT i = nchain; i-- > 0;) {
    off = chain[i];
    const INT at = ms_seg(m, MS_CDGUIDS, off);
    DWORD goff, doff, next;
    if (at < 0 || !ms_u32(m, at, &goff) || !ms_u32(m, at + 4, &doff) || !ms_u32(m, at + 8, &next)) return S_OK;
    GUID g;
    VARIANT v;
    if (ms_guid(m, (INT)goff, &g) && SUCCEEDED(ms_value(m, (INT)doff, &v))) {
      HRESULT hr = tl_cust_set(list, &g, &v);
      VariantClear(&v);
      if (hr == E_OUTOFMEMORY) return hr;
    }
  }
  return S_OK;
}

/* A type description: an inline basic type (the sign bit set, the VT in the low bits) or an entry of the type description table. */
static HRESULT ms_typedesc(const Msft *m, INT t, TYPEDESC *td, UINT depth) {
  memset(td, 0, sizeof(*td));
  if (t < 0) { td->vt = (VARTYPE)(t & VT_TYPEMASK); return S_OK; }
  if (depth > 32) return E_FAIL;
  const INT at = ms_seg(m, MS_TYPEDESC, t);
  DWORD a, b;
  if (at < 0 || !ms_u32(m, at, &a) || !ms_u32(m, at + 4, &b)) return E_FAIL;
  td->vt = (VARTYPE)(a & VT_TYPEMASK);
  HRESULT hr = S_OK;
  switch (td->vt) {
    case VT_PTR: case VT_SAFEARRAY:
      td->lptdesc = oa_calloc(sizeof(TYPEDESC));
      if (!td->lptdesc) return E_OUTOFMEMORY;
      hr = ms_typedesc(m, (INT)b, td->lptdesc, depth + 1);
      if (FAILED(hr)) { oa_free(td->lptdesc); memset(td, 0, sizeof(*td)); }
      break;
    case VT_CARRAY: {
      const INT ad = ms_seg(m, MS_ARRAYDESC, (INT)b);
      DWORD elem, dims;
      if (ad < 0 || !ms_u32(m, ad, &elem) || !ms_u32(m, ad + 4, &dims)) { memset(td, 0, sizeof(*td)); return E_FAIL; }
      const UINT cdims = dims & 0xffff;
      if (cdims > 64 || (SIZE_T)ad + 8 + (SIZE_T)cdims * 8 > m->n) { memset(td, 0, sizeof(*td)); return E_FAIL; }
      td->lpadesc = oa_calloc(sizeof(ARRAYDESC) + (cdims ? cdims - 1 : 0) * sizeof(SAFEARRAYBOUND));
      if (!td->lpadesc) { memset(td, 0, sizeof(*td)); return E_OUTOFMEMORY; }
      td->lpadesc->cDims = (USHORT)cdims;
      for (UINT i = 0; i < cdims; i++) {
        DWORD ce, lb;
        ms_u32(m, ad + 8 + (INT)(i * 8), &ce);
        ms_u32(m, ad + 12 + (INT)(i * 8), &lb);
        td->lpadesc->rgbounds[i].cElements = ce;
        td->lpadesc->rgbounds[i].lLbound = (LONG)lb;
      }
      hr = ms_typedesc(m, (INT)elem, &td->lpadesc->tdescElem, depth + 1);
      if (FAILED(hr)) { oa_free(td->lpadesc); memset(td, 0, sizeof(*td)); }
      break;
    }
    case VT_USERDEFINED: td->hreftype = b; break;
    default: break;
  }
  return hr;
}

/* The function records of a type (ptr points at the block: a length, the records, the member ids and the name offsets). */
static HRESULT ms_funcs(const Msft *m, TlType *t, UINT count, INT block, DWORD infolen, INT ids, INT names) {
  INT rec = block + 4;
  const INT end = block + 4 + (INT)infolen;
  for (UINT fi = 0; fi < count; fi++) {
    DWORD w[8];
    if (rec < 0 || rec + 24 > end || !ms_u32(m, rec, &w[0])) return E_FAIL;
    const UINT recsize = w[0] & 0xffff;
    if (recsize < 24 || recsize % 4 || (SIZE_T)rec + recsize > (SIZE_T)end) return E_FAIL;
    DWORD dt, flags, vtbl, kind, nargs_opt;
    ms_u32(m, rec + 4, &dt); ms_u32(m, rec + 8, &flags); ms_u32(m, rec + 12, &vtbl); ms_u32(m, rec + 16, &kind); ms_u32(m, rec + 20, &nargs_opt);
    const UINT nargs = nargs_opt & 0xffff;
    const SHORT nopt = (SHORT)(nargs_opt >> 16);
    UINT words = recsize / 4 - 6;
    if (nargs > words / 3) return E_FAIL;
    words -= nargs * 3;
    const INT params = rec + (INT)recsize - (INT)(nargs * 12);
    INT defaults = -1, custs = -1;
    if (kind & 0x1000) { if (words < nargs) return E_FAIL; words -= nargs; defaults = params - (INT)(nargs * 4); }
    if (kind & 0x80) { if (words < nargs + 1) return E_FAIL; words -= nargs + 1; custs = rec + 24 + (INT)(words * 4); }
    TlFunc *f = tl_type_add_func(t);
    if (!f) return E_OUTOFMEMORY;
    f->fd.funckind = (FUNCKIND)(kind & 7);
    f->fd.invkind = (INVOKEKIND)((kind >> 3) & 0xf);
    f->fd.callconv = (CALLCONV)((kind >> 8) & 0xf);
    f->fd.cParams = (SHORT)nargs;
    f->fd.cParamsOpt = nopt;
    f->fd.oVft = (SHORT)(vtbl & 0xffff);
    if (sizeof(void *) == 8 && m->lib->syskind != SYS_WIN64) f->fd.oVft = (SHORT)((unsigned)(vtbl & 0xffff) * 8 / 4);    /* (offsets of this process) */
    f->fd.wFuncFlags = (WORD)(flags & 0xffff);
    DWORD id = 0, nameoff = 0xffffffffu;
    ms_u32(m, ids + (INT)fi * 4, &id);
    ms_u32(m, names + (INT)fi * 4, &nameoff);
    f->fd.memid = (MEMBERID)id;
    f->name = ms_name(m, (INT)nameoff);
    HRESULT hr = ms_typedesc(m, (INT)dt, &f->fd.elemdescFunc.tdesc, 0);
    if (FAILED(hr)) return hr;
    /* the optional fields: help context, help string, entry point, two reserved, help string context */
    DWORD opt[6] = { 0, 0xffffffffu, 0xffffffffu, 0xffffffffu, 0xffffffffu, 0 };
    for (UINT k = 0; k < words && k < 6; k++) ms_u32(m, rec + 24 + (INT)k * 4, &opt[k]);
    f->help_context = opt[0];
    if (words > 1) f->doc = ms_string(m, (INT)opt[1]);
    if (words > 2 && opt[2] != 0xffffffffu) {
      if ((opt[2] >> 16) != 0xffff) f->dll_entry = ms_string(m, (INT)opt[2]);
      if (!f->dll_entry) { f->has_ordinal = TRUE; f->ordinal = (WORD)(opt[2] & 0xffff); }
    }
    f->help_string_context = opt[5];
    if (custs >= 0) {
      DWORD co;
      if (ms_u32(m, custs, &co)) { hr = ms_custdata(m, (INT)co, &f->cust); if (FAILED(hr)) return hr; }
    }
    if (nargs) {
      f->fd.lprgelemdescParam = oa_calloc((SIZE_T)nargs * sizeof(ELEMDESC));
      f->pnames = oa_calloc((SIZE_T)nargs * sizeof(WCHAR *));
      if (!f->fd.lprgelemdescParam || !f->pnames) return E_OUTOFMEMORY;
      for (UINT p = 0; p < nargs; p++) {
        DWORD pdt, pname, pflags;
        ms_u32(m, params + (INT)p * 12, &pdt); ms_u32(m, params + (INT)p * 12 + 4, &pname); ms_u32(m, params + (INT)p * 12 + 8, &pflags);
        ELEMDESC *e = &f->fd.lprgelemdescParam[p];
        hr = ms_typedesc(m, (INT)pdt, &e->tdesc, 0);
        if (FAILED(hr)) return hr;
        e->paramdesc.wParamFlags = (USHORT)(pflags & 0xffff);
        f->pnames[p] = ms_name(m, (INT)pname);
        if (e->paramdesc.wParamFlags & PARAMFLAG_FHASDEFAULT) {
          PARAMDESCEX *ex = oa_calloc(sizeof(PARAMDESCEX));
          if (!ex) return E_OUTOFMEMORY;
          ex->cBytes = sizeof(PARAMDESCEX);
          e->paramdesc.pparamdescex = ex;
          DWORD dv;
          if (defaults >= 0 && ms_u32(m, defaults + (INT)p * 4, &dv) && dv != 0xffffffffu) {
            if (FAILED(ms_value(m, (INT)dv, &ex->varDefaultValue))) VariantInit(&ex->varDefaultValue);
          }
        }
        if (custs >= 0) {
          DWORD co;
          if (ms_u32(m, custs + 4 + (INT)p * 4, &co) && co != 0xffffffffu) {
            if (!f->pcust) { f->pcust = oa_calloc((SIZE_T)nargs * sizeof(TlCustList)); if (!f->pcust) return E_OUTOFMEMORY; }
            hr = ms_custdata(m, (INT)co, &f->pcust[p]);
            if (FAILED(hr)) return hr;
          }
        }
      }
    }
    rec += (INT)recsize;
  }
  return S_OK;
}

static HRESULT ms_vars(const Msft *m, TlType *t, UINT nf, UINT count, INT rec, INT end, INT ids, INT names) {
  for (UINT vi = 0; vi < count; vi++) {
    DWORD w0;
    if (rec < 0 || rec + 20 > end || !ms_u32(m, rec, &w0)) return E_FAIL;
    const UINT recsize = w0 & 0xffff;
    if (recsize < 20 || recsize % 4 || (SIZE_T)rec + recsize > (SIZE_T)end) return E_FAIL;
    DWORD dt, flags, kind, value;
    ms_u32(m, rec + 4, &dt); ms_u32(m, rec + 8, &flags); ms_u32(m, rec + 12, &kind); ms_u32(m, rec + 16, &value);
    TlVar *v = tl_type_add_var(t);
    if (!v) return E_OUTOFMEMORY;
    v->vd.wVarFlags = (WORD)(flags & 0xffff);
    v->vd.varkind = (VARKIND)(kind & 0xffff);
    DWORD id = 0, nameoff = 0xffffffffu;
    ms_u32(m, ids + (INT)(nf + vi) * 4, &id);
    ms_u32(m, names + (INT)(nf + vi) * 4, &nameoff);
    v->vd.memid = (MEMBERID)id;
    v->name = ms_name(m, (INT)nameoff);
    HRESULT hr = ms_typedesc(m, (INT)dt, &v->vd.elemdescVar.tdesc, 0);
    if (FAILED(hr)) return hr;
    if (v->vd.varkind == VAR_CONST) {
      v->vd.lpvarValue = oa_calloc(sizeof(VARIANT));
      if (!v->vd.lpvarValue) return E_OUTOFMEMORY;
      if (FAILED(ms_value(m, (INT)value, v->vd.lpvarValue))) VariantInit(v->vd.lpvarValue);
    } else {
      v->vd.oInst = value;
    }
    const UINT words = recsize / 4 - 5;
    DWORD opt[5] = { 0, 0xffffffffu, 0, 0xffffffffu, 0 };
    for (UINT k = 0; k < words && k < 5; k++) ms_u32(m, rec + 20 + (INT)k * 4, &opt[k]);
    v->help_context = opt[0];
    if (words > 1) v->doc = ms_string(m, (INT)opt[1]);
    if (words > 3) { hr = ms_custdata(m, (INT)opt[3], &v->cust); if (FAILED(hr)) return hr; }
    v->help_string_context = opt[4];
    rec += (INT)recsize;
  }
  return S_OK;
}

static HRESULT ms_type(Msft *m, TlType *t, INT base, DWORD diroff) {
  DWORD ti[25];
  for (UINT k = 0; k < 25; k++) if (!ms_u32(m, base + (INT)k * 4, &ti[k])) return E_FAIL;
  t->file_offset = diroff;
  t->attr.typekind = (TYPEKIND)(ti[0] & 0xf);
  if (t->attr.typekind >= TKIND_MAX) return E_FAIL;
  t->attr.cbAlignment = (WORD)((ti[0] >> 11) & 0x1f);
  t->attr.wTypeFlags = (WORD)ti[12];
  t->attr.cbSizeInstance = (LONG)ti[20];
  t->attr.cFuncs = (WORD)(ti[6] & 0xffff);
  t->attr.cVars = (WORD)(ti[6] >> 16);
  t->attr.cImplTypes = (WORD)(ti[19] & 0xffff);
  t->attr.cbSizeVft = (WORD)(ti[19] >> 16);
  if (sizeof(void *) == 8 && m->lib->syskind != SYS_WIN64) t->attr.cbSizeVft = (WORD)((unsigned)t->attr.cbSizeVft * 8 / 4);
  t->attr.wMajorVerNum = (WORD)(ti[14] & 0xffff);
  t->attr.wMinorVerNum = (WORD)(ti[14] >> 16);
  t->attr.lcid = m->lib->lcid;
  ms_guid(m, (INT)ti[11], &t->attr.guid);
  t->name = ms_name(m, (INT)ti[13]);
  t->doc = ms_string(m, (INT)ti[15]);
  t->help_string_context = ti[16];
  t->help_context = ti[17];
  HRESULT hr = ms_custdata(m, (INT)ti[18], &t->cust);
  if (FAILED(hr)) return hr;
  if (t->attr.typekind == TKIND_INTERFACE || t->attr.typekind == TKIND_DISPATCH || t->attr.typekind == TKIND_COCLASS) {
    /* (a pointer sized object: of this process, whatever the library was made for) */
    t->attr.cbSizeInstance = sizeof(void *);
    if ((sizeof(void *) == 8) != (m->lib->syskind == SYS_WIN64)) t->attr.cbAlignment = sizeof(void *);
  }
  switch (t->attr.typekind) {
    case TKIND_ALIAS: {
      hr = ms_typedesc(m, (INT)ti[21], &t->attr.tdescAlias, 0);
      if (FAILED(hr)) return hr;
      /* the size of an alias in a library of the other kind of system is that of this process */
      if (t->attr.tdescAlias.vt != VT_USERDEFINED && ((sizeof(void *) == 8) != (m->lib->syskind == SYS_WIN64))) {
        ULONG size; WORD align;
        if (SUCCEEDED(tl_td_size(m->lib, &t->attr.tdescAlias, sizeof(void *), TRUE, &size, &align, 0))) { t->attr.cbSizeInstance = (LONG)size; t->attr.cbAlignment = align; }
      }
      break;
    }
    case TKIND_MODULE: t->dllname = ms_string(m, (INT)ti[21]); break;
    case TKIND_COCLASS: {
      INT ref = (INT)ti[21];
      const UINT limit = (UINT)(m->seg_len[MS_REFTAB] > 0 ? m->seg_len[MS_REFTAB] : 0) / 16 + 1, nimpl = t->attr.cImplTypes;
      for (UINT k = 0; k < nimpl && ref >= 0 && k < limit; k++) {
        const INT at = ms_seg(m, MS_REFTAB, ref);
        DWORD r[4];
        if (at < 0 || !ms_u32(m, at, &r[0]) || !ms_u32(m, at + 4, &r[1]) || !ms_u32(m, at + 8, &r[2]) || !ms_u32(m, at + 12, &r[3])) return E_FAIL;
        TlImpl *im = tl_type_add_impl(t);
        if (!im) return E_OUTOFMEMORY;
        im->href = r[0];
        im->flags = (INT)r[1];
        hr = ms_custdata(m, (INT)r[2], &im->cust);
        if (FAILED(hr)) return hr;
        ref = (INT)r[3];
      }
      t->attr.cImplTypes = (WORD)t->nimpls;
      break;
    }
    case TKIND_INTERFACE: case TKIND_DISPATCH:
      if (t->attr.cImplTypes && (INT)ti[21] != -1) {
        TlImpl *im = tl_type_add_impl(t);
        if (!im) return E_OUTOFMEMORY;
        im->href = ti[21];
      }
      break;
    default: break;
  }
  if (t->attr.cFuncs + t->attr.cVars) {
    const INT block = (INT)ti[1];
    DWORD infolen;
    if (!ms_u32(m, block, &infolen) || infolen > m->n) return E_FAIL;
    const UINT total = (UINT)t->attr.cFuncs + t->attr.cVars;
    const INT ids = block + 4 + (INT)infolen, names = ids + (INT)total * 4;
    if ((SIZE_T)ids + (SIZE_T)total * 8 > m->n) return E_FAIL;
    const UINT nfuncs = t->attr.cFuncs, nvars = t->attr.cVars;
    hr = ms_funcs(m, t, nfuncs, block, infolen, ids, names);
    if (FAILED(hr)) return hr;
    /* the variables follow the function records */
    INT rec = block + 4;
    for (UINT i = 0; i < nfuncs; i++) { DWORD w; ms_u32(m, rec, &w); rec += (INT)(w & 0xffff); }
    hr = ms_vars(m, t, nfuncs, nvars, rec, block + 4 + (INT)infolen, ids, names);
    if (FAILED(hr)) return hr;
  }
  return S_OK;
}

static HRESULT msft_load(const BYTE *data, SIZE_T size, TlLib **out) {
  Msft m;
  memset(&m, 0, sizeof(m));
  m.d = data;
  m.n = size;
  if (size < MSFT_HEADER_SIZE) return TYPE_E_CANTLOADLIBRARY;
  DWORD h[21];
  for (UINT i = 0; i < 21; i++) ms_u32(&m, (INT)i * 4, &h[i]);
  if (h[0] != MSFT_MAGIC1 || h[1] != MSFT_MAGIC2) return TYPE_E_CANTLOADLIBRARY;
  const DWORD varflags = h[5], ntypes = h[8];
  if (ntypes > TL_MAX_ITEMS || ntypes > size / 4) return TYPE_E_CANTLOADLIBRARY;
  INT dir = MSFT_HEADER_SIZE + (varflags & 0x100 ? 4 : 0);
  INT segdir = dir + (INT)ntypes * 4;
  if ((SIZE_T)segdir + MSFT_SEGMENTS * 16 > size) return TYPE_E_CANTLOADLIBRARY;
  for (int i = 0; i < MSFT_SEGMENTS; i++) {
    DWORD off, len;
    ms_u32(&m, segdir + i * 16, &off);
    ms_u32(&m, segdir + i * 16 + 4, &len);
    m.seg_off[i] = (INT)off >= 0 && off <= size ? (INT)off : -1;
    m.seg_len[i] = (INT)len >= 0 ? (INT)len : 0;
    if (m.seg_off[i] >= 0 && (SIZE_T)m.seg_off[i] + (SIZE_T)m.seg_len[i] > size) m.seg_len[i] = (INT)(size - (SIZE_T)m.seg_off[i]);
  }
  TlLib *lib = tl_lib_new();
  if (!lib) return E_OUTOFMEMORY;
  m.lib = lib;
  HRESULT hr = S_OK;
  lib->file_lcid = (LCID)h[3];
  lib->lcid = (LCID)h[4];
  lib->syskind = (SYSKIND)(varflags & 0xf);
  if (lib->syskind > SYS_WIN64) lib->syskind = SYS_WIN32;
  lib->major = (WORD)(h[6] & 0xffff);
  lib->minor = (WORD)(h[6] >> 16);
  lib->flags = (WORD)(h[7] | LIBFLAG_FHASDISKIMAGE);
  lib->help_string_context = h[10];
  lib->help_context = h[11];
  m.cp = ms_codepage(lib->file_lcid);
  ms_guid(&m, (INT)h[2], &lib->guid);
  lib->name = ms_name(&m, (INT)h[14]);
  lib->doc = ms_string(&m, (INT)h[9]);
  lib->helpfile = ms_string(&m, (INT)h[15]);
  if (varflags & 0x100) { DWORD hd; if (ms_u32(&m, MSFT_HEADER_SIZE, &hd)) lib->helpstringdll = ms_string(&m, (INT)hd); }
  lib->dispatch_href = h[19];
  hr = ms_custdata(&m, (INT)h[16], &lib->cust);
  /* imported files (variable size entries) and imported types */
  if (SUCCEEDED(hr) && m.seg_off[MS_IMPFILES] >= 0) {
    INT off = 0;
    while (off + 14 <= m.seg_len[MS_IMPFILES]) {
      const INT at = m.seg_off[MS_IMPFILES] + off;
      DWORD goff, lcid, ver;
      WORD sz;
      ms_u32(&m, at, &goff); ms_u32(&m, at + 4, &lcid); ms_u32(&m, at + 8, &ver); ms_u16(&m, at + 12, &sz);
      const UINT len = sz >> 2;
      if ((SIZE_T)at + 14 + len > size) { hr = E_FAIL; break; }
      TlImpFile *f = TL_GROW(lib->impfiles, lib->nimpfiles, lib->impfiles_cap);
      DWORD *offs = f ? oa_realloc(m.impfile_off, ((SIZE_T)m.nimpfile_off + 1) * sizeof(DWORD)) : NULL;
      if (!f || !offs) { hr = E_OUTOFMEMORY; break; }
      m.impfile_off = offs;
      m.impfile_off[m.nimpfile_off++] = (DWORD)off;
      ms_guid(&m, (INT)goff, &f->guid);
      f->lcid = lcid;
      f->major = (WORD)(ver & 0xffff);
      f->minor = (WORD)(ver >> 16);
      f->name = ms_ansi(&m, m.d + at + 14, len);
      off += 12 + (INT)((2 + len + 3) & ~3u);
    }
  }
  if (SUCCEEDED(hr) && m.seg_off[MS_IMPINFO] >= 0) {
    const UINT n = (UINT)m.seg_len[MS_IMPINFO] / TL_IMPINFO_RECORD;
    for (UINT i = 0; i < n && SUCCEEDED(hr); i++) {
      DWORD flags, ofile, oguid;
      const INT at = m.seg_off[MS_IMPINFO] + (INT)(i * TL_IMPINFO_RECORD);
      ms_u32(&m, at, &flags); ms_u32(&m, at + 4, &ofile); ms_u32(&m, at + 8, &oguid);
      TlImpInfo *ii = TL_GROW(lib->impinfos, lib->nimpinfos, lib->impinfos_cap);
      if (!ii) { hr = E_OUTOFMEMORY; break; }
      ii->tkind = (INT)(flags >> 24);
      ii->file = (UINT)-1;
      for (UINT k = 0; k < m.nimpfile_off; k++) if (m.impfile_off[k] == ofile) ii->file = k;
      ii->by_guid = (flags & 0x10000u) != 0;
      if (ii->by_guid) ms_guid(&m, (INT)oguid, &ii->guid);
      ii->offset = ii->by_guid ? flags & 0xffff : oguid / TL_TYPEINFO_RECORD;      /* (the index of the type in its library) */
    }
  }
  oa_free(m.impfile_off);
  m.impfile_off = NULL;
  for (UINT i = 0; SUCCEEDED(hr) && i < ntypes; i++) {
    DWORD off;
    ms_u32(&m, dir + (INT)i * 4, &off);
    const INT base = ms_seg(&m, MS_TYPEINFO, (INT)off);
    if (base < 0 || (SIZE_T)base + TL_TYPEINFO_RECORD > size) { hr = E_FAIL; break; }
    TlType *t = tl_lib_add_type(lib, TKIND_ALIAS);
    if (!t) { hr = E_OUTOFMEMORY; break; }
    hr = ms_type(&m, t, base, off);
  }
  if (SUCCEEDED(hr) && (sizeof(void *) == 8) != (lib->syskind == SYS_WIN64)) {
    /* an alias of a type of the library: the size and alignment of that type */
    for (UINT i = 0; i < lib->ntypes; i++) {
      TlType *t = lib->types[i];
      ULONG size; WORD align;
      if (t->attr.typekind == TKIND_ALIAS && t->attr.tdescAlias.vt == VT_USERDEFINED && SUCCEEDED(tl_td_size(lib, &t->attr.tdescAlias, sizeof(void *), TRUE, &size, &align, 0))) {
        t->attr.cbSizeInstance = (LONG)size;
        t->attr.cbAlignment = align;
      }
    }
  }
  if (FAILED(hr)) {
    tl_lib_free(lib);
    return hr == E_OUTOFMEMORY ? hr : TYPE_E_CANTLOADLIBRARY;
  }
  *out = lib;
  return S_OK;
}
