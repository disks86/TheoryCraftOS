/* ole32: the OLE object storage formats (\1Ole, OLE streams), OleLoad/OleSave/OleRun, the OleCreate family, the default handler and embedding helper, the registry
 * helpers (user type, misc status, verbs, data formats, auto convert), the OLE 1.0 stream converters and the legacy odds and ends. */

#ifndef EMBDHLP_DELAYCREATE
#define EMBDHLP_DELAYCREATE 0x00010000
#endif

static const GUID g_clsid_default_handler = {0x00020323, 0, 0, {0xC0, 0, 0, 0, 0, 0, 0, 0x46}};
static const GUID g_clsid_std_ole_link = {0x00000303, 0, 0, {0xC0, 0, 0, 0, 0, 0, 0, 0x46}};
static const GUID g_clsid_package = {0x0003000C, 0, 0, {0xC0, 0, 0, 0, 0, 0, 0, 0x46}};
static const GUID g_clsid_picture_metafile = {0x00000315, 0, 0, {0xC0, 0, 0, 0, 0, 0, 0, 0x46}};
static const GUID g_clsid_picture_dib = {0x00000316, 0, 0, {0xC0, 0, 0, 0, 0, 0, 0, 0x46}};
static const GUID g_clsid_picture_enhmetafile = {0x00000319, 0, 0, {0xC0, 0, 0, 0, 0, 0, 0, 0x46}};

/* ---- clipboard format ids ------------------------------------------------------------------------------------------------------------------------------------------ */
/* user32 is not available, so the registered clipboard formats of OLE get ids of their own: 0xC000 and up, handed out by name and never changed. */

#define OLE_CF_BASE 0xC000
#define OLE_CF_DYNAMIC 64
static const char *const g_cf_names[] = {
  "Embedded Object", "Embed Source", "Link Source", "Object Descriptor", "Link Source Descriptor", "FileName", "FileNameW", "OwnerLink", "ObjectLink", "Native",
  "Ole Private Data", "DataObject", "Custom Link Source", "Hyperlink", "Shell IDList Array", "Preferred DropEffect", "Rich Text Format", "HTML Format", NULL};
#define OLE_CF_STATIC_COUNT 18
static WCHAR *g_cf_dynamic[OLE_CF_DYNAMIC];

static BOOL cf_name_eq(const char *a, const WCHAR *w) {
  for (;; a++, w++) {
    WCHAR x = (WCHAR)(BYTE)*a, y = *w;
    if (x >= 'A' && x <= 'Z') x = (WCHAR)(x + 32);
    if (y >= 'A' && y <= 'Z') y = (WCHAR)(y + 32);
    if (x != y) return FALSE;
    if (!x) return TRUE;
  }
}
static UINT ole_clipformat(const WCHAR *name) {
  if (!name || !name[0]) return 0;
  for (UINT i = 0; i < OLE_CF_STATIC_COUNT; i++) if (cf_name_eq(g_cf_names[i], name)) return OLE_CF_BASE + i;
  UINT result = 0;
  lock();
  for (UINT i = 0; i < OLE_CF_DYNAMIC; i++) {
    if (!g_cf_dynamic[i]) {
      if (wlen(name) < 256) { g_cf_dynamic[i] = wdup_heap(name); if (g_cf_dynamic[i]) result = OLE_CF_BASE + OLE_CF_STATIC_COUNT + i; }
      break;
    }
    if (wicmp(g_cf_dynamic[i], name) == 0) { result = OLE_CF_BASE + OLE_CF_STATIC_COUNT + i; break; }
  }
  unlock();
  return result;
}
static UINT ole_clipformat_a(const char *name) {
  WCHAR w[64];
  SIZE_T i = 0;
  for (; name[i] && i < 63; i++) w[i] = (WCHAR)(BYTE)name[i];
  w[i] = 0;
  return ole_clipformat(w);
}
/* the name of a private id (static ones only), or NULL */
static const char *ole_clipformat_static_name(UINT cf) { return cf >= OLE_CF_BASE && cf < OLE_CF_BASE + OLE_CF_STATIC_COUNT ? g_cf_names[cf - OLE_CF_BASE] : NULL; }

static FORMATETC make_fmt(UINT cf, DWORD tymed) {
  FORMATETC f;
  f.cfFormat = (CLIPFORMAT)cf; f.ptd = NULL; f.dwAspect = DVASPECT_CONTENT; f.lindex = -1; f.tymed = tymed;
  return f;
}
static BOOL data_has(IDataObject *d, UINT cf, DWORD tymed) {
  FORMATETC f = make_fmt(cf, tymed);
  return IDataObject_QueryGetData(d, &f) == S_OK;
}

/* ---- small parsing helpers ----------------------------------------------------------------------------------------------------------------------------------------- */

static BOOL parse_long(const WCHAR **p, LONG *v) {
  const WCHAR *s = *p;
  BOOL neg = FALSE;
  LONGLONG n = 0;
  int digits = 0;
  while (*s == ' ' || *s == '\t') s++;
  if (*s == '-') { neg = TRUE; s++; }
  while (*s >= '0' && *s <= '9' && digits < 10) { n = n * 10 + (*s - '0'); s++; digits++; }
  if (!digits) return FALSE;
  while (*s == ' ' || *s == '\t') s++;
  *v = (LONG)(neg ? -n : n);
  *p = s;
  return TRUE;
}
static void put_dec(WCHAR *out, ULONG v) {
  WCHAR tmp[12];
  int n = 0;
  do { tmp[n++] = (WCHAR)('0' + v % 10); v /= 10; } while (v);
  while (n) *out++ = tmp[--n];
  *out = 0;
}
static WCHAR *cotask_wdup(const WCHAR *s) {
  SIZE_T n = (wlen(s) + 1) * sizeof(WCHAR);
  WCHAR *r = CoTaskMemAlloc(n);
  if (r) mem_copy(r, s, n);
  return r;
}

/* ---- the registry: user type, misc status, verbs, data formats ------------------------------------------------------------------------------------------------------ */

static BOOL clsid_registered(REFCLSID clsid) {
  WCHAR path[100];
  HKEY k;
  clsid_key_path(clsid, NULL, path);
  if (RegOpenKeyExW(HKEY_CLASSES_ROOT, path, 0, KEY_READ, &k) != ERROR_SUCCESS) return FALSE;
  RegCloseKey(k);
  return TRUE;
}

EXPORT HRESULT WINAPI OleRegGetUserType(REFCLSID clsid, DWORD form, LPOLESTR *out) {
  WCHAR path[128], sub[32], text[256];
  BOOL got = FALSE;
  if (!out) return E_INVALIDARG;
  *out = NULL;
  if (!clsid_registered(clsid)) return REGDB_E_CLASSNOTREG;
  if (form != USERCLASSTYPE_FULL && form <= 4) {
    const WCHAR *prefix = W('A', 'u', 'x', 'U', 's', 'e', 'r', 'T', 'y', 'p', 'e', '\\');
    SIZE_T n = wlen(prefix);
    mem_copy(sub, prefix, n * sizeof(WCHAR));
    put_dec(sub + n, form);
    clsid_key_path(clsid, sub, path);
    got = reg_class_string(path, NULL, text, 256) == ERROR_SUCCESS && text[0];
  }
  if (!got) {
    clsid_key_path(clsid, NULL, path);
    got = reg_class_string(path, NULL, text, 256) == ERROR_SUCCESS && text[0];
  }
  if (!got) return REGDB_E_READREGDB;
  *out = cotask_wdup(text);
  return *out ? S_OK : E_OUTOFMEMORY;
}

EXPORT HRESULT WINAPI OleRegGetMiscStatus(REFCLSID clsid, DWORD aspect, DWORD *status) {
  WCHAR path[128], sub[32], text[32];
  const WCHAR *prefix = W('M', 'i', 's', 'c', 'S', 't', 'a', 't', 'u', 's');
  const WCHAR *p;
  LONG v = 0;
  if (!status) return E_INVALIDARG;
  *status = 0;
  if (!clsid_registered(clsid)) return REGDB_E_CLASSNOTREG;
  SIZE_T n = wlen(prefix);
  mem_copy(sub, prefix, n * sizeof(WCHAR));
  sub[n] = '\\';
  put_dec(sub + n + 1, aspect);
  clsid_key_path(clsid, sub, path);
  if (reg_class_string(path, NULL, text, 32) != ERROR_SUCCESS) {
    clsid_key_path(clsid, prefix, path);
    if (reg_class_string(path, NULL, text, 32) != ERROR_SUCCESS) return S_OK;
  }
  p = text;
  if (parse_long(&p, &v)) *status = (DWORD)v;
  return S_OK;
}

typedef struct VerbItem { LONG verb; DWORD menu_flags, attribs; WCHAR *name; } VerbItem;
typedef struct EnumVerbObj {
  IEnumOLEVERB iface;
  LONG refs;
  VerbItem *items;
  ULONG count, pos;
} EnumVerbObj;
static const IEnumOLEVERBVtbl g_enumverb_vtbl;

static void enumverb_free_items(VerbItem *items, ULONG count) {
  if (!items) return;
  for (ULONG i = 0; i < count; i++) mem_free(items[i].name);
  mem_free(items);
}
static HRESULT WINAPI enumverb_qi(IEnumOLEVERB *me, REFIID riid, void **out) {
  if (!out) return E_INVALIDARG;
  *out = NULL;
  if (IsEqualGUID(riid, &IID_IUnknown) || IsEqualGUID(riid, &IID_IEnumOLEVERB)) { *out = me; IEnumOLEVERB_AddRef(me); return S_OK; }
  return E_NOINTERFACE;
}
static ULONG WINAPI enumverb_addref(IEnumOLEVERB *me) { return (ULONG)InterlockedIncrement(&((EnumVerbObj *)me)->refs); }
static ULONG WINAPI enumverb_release(IEnumOLEVERB *me) {
  EnumVerbObj *e = (EnumVerbObj *)me;
  LONG r = InterlockedDecrement(&e->refs);
  if (r == 0) { enumverb_free_items(e->items, e->count); mem_free(e); }
  return (ULONG)r;
}
static HRESULT WINAPI enumverb_next(IEnumOLEVERB *me, ULONG celt, LPOLEVERB out, ULONG *fetched) {
  EnumVerbObj *e = (EnumVerbObj *)me;
  ULONG n = 0;
  if (!out && celt) return E_INVALIDARG;
  if (fetched) *fetched = 0;
  while (n < celt && e->pos < e->count) {
    VerbItem *it = &e->items[e->pos];
    out[n].lVerb = it->verb;
    out[n].lpszVerbName = it->name ? cotask_wdup(it->name) : NULL;
    out[n].fuFlags = it->menu_flags;
    out[n].grfAttribs = it->attribs;
    n++; e->pos++;
  }
  if (fetched) *fetched = n;
  return n == celt ? S_OK : S_FALSE;
}
static HRESULT WINAPI enumverb_skip(IEnumOLEVERB *me, ULONG celt) {
  EnumVerbObj *e = (EnumVerbObj *)me;
  if (celt > e->count - e->pos) { e->pos = e->count; return S_FALSE; }
  e->pos += celt;
  return S_OK;
}
static HRESULT WINAPI enumverb_reset(IEnumOLEVERB *me) { ((EnumVerbObj *)me)->pos = 0; return S_OK; }
static HRESULT enumverb_make(VerbItem *items, ULONG count, ULONG pos, IEnumOLEVERB **out) {
  EnumVerbObj *e = mem_alloc(sizeof(*e));
  if (!e) { enumverb_free_items(items, count); return E_OUTOFMEMORY; }
  e->iface.lpVtbl = (IEnumOLEVERBVtbl *)&g_enumverb_vtbl;
  e->refs = 1; e->items = items; e->count = count; e->pos = pos;
  *out = &e->iface;
  return S_OK;
}
static HRESULT WINAPI enumverb_clone(IEnumOLEVERB *me, IEnumOLEVERB **out) {
  EnumVerbObj *e = (EnumVerbObj *)me;
  VerbItem *copy = NULL;
  if (!out) return E_INVALIDARG;
  *out = NULL;
  if (e->count) {
    copy = mem_alloc(e->count * sizeof(VerbItem));
    if (!copy) return E_OUTOFMEMORY;
    for (ULONG i = 0; i < e->count; i++) { copy[i] = e->items[i]; copy[i].name = e->items[i].name ? wdup_heap(e->items[i].name) : NULL; }
  }
  return enumverb_make(copy, e->count, e->pos, out);
}
static const IEnumOLEVERBVtbl g_enumverb_vtbl = {enumverb_qi, enumverb_addref, enumverb_release, enumverb_next, enumverb_skip, enumverb_reset, enumverb_clone};

/* "name,menuflags,verbflags" */
static void parse_verb_value(const WCHAR *text, VerbItem *it) {
  SIZE_T n = 0;
  LONG v;
  const WCHAR *p;
  while (text[n] && text[n] != ',') n++;
  it->name = mem_alloc((n + 1) * sizeof(WCHAR));
  if (it->name) mem_copy(it->name, text, n * sizeof(WCHAR));
  it->menu_flags = 0; it->attribs = 0;
  if (text[n] == ',') {
    p = text + n + 1;
    if (parse_long(&p, &v)) it->menu_flags = (DWORD)v;
    if (*p == ',') { p++; if (parse_long(&p, &v)) it->attribs = (DWORD)v; }
  }
}

EXPORT HRESULT WINAPI OleRegEnumVerbs(REFCLSID clsid, LPENUMOLEVERB *out) {
  WCHAR path[128], keyname[32], text[256];
  HKEY verbs;
  VerbItem *items = NULL;
  ULONG count = 0, cap = 0;
  if (!out) return E_INVALIDARG;
  *out = NULL;
  if (!clsid_registered(clsid)) return REGDB_E_CLASSNOTREG;
  clsid_key_path(clsid, W('v', 'e', 'r', 'b'), path);
  if (RegOpenKeyExW(HKEY_CLASSES_ROOT, path, 0, KEY_READ, &verbs) != ERROR_SUCCESS) return OLEOBJ_E_NOVERBS;
  for (DWORD index = 0; count < 256; index++) {
    DWORD len = 32;
    LONG num;
    const WCHAR *p;
    if (RegEnumKeyExW(verbs, index, keyname, &len, NULL, NULL, NULL, NULL) != ERROR_SUCCESS) break;
    p = keyname;
    if (!parse_long(&p, &num) || *p) continue;
    WCHAR sub[48];
    const WCHAR *pre = W('v', 'e', 'r', 'b', '\\');
    mem_copy(sub, pre, 5 * sizeof(WCHAR));
    wcopy_n(sub + 5, keyname, 32);
    clsid_key_path(clsid, sub, path);
    if (reg_class_string(path, NULL, text, 256) != ERROR_SUCCESS) continue;
    if (count == cap) {
      ULONG ncap = cap ? cap * 2 : 8;
      VerbItem *n2 = mem_realloc(items, ncap * sizeof(VerbItem));
      if (!n2) { enumverb_free_items(items, count); RegCloseKey(verbs); return E_OUTOFMEMORY; }
      items = n2; cap = ncap;
    }
    items[count].verb = num;
    parse_verb_value(text, &items[count]);
    count++;
  }
  RegCloseKey(verbs);
  if (!count) { mem_free(items); return OLEOBJ_E_NOVERBS; }
  for (ULONG i = 1; i < count; i++) {      /* by verb number */
    VerbItem t = items[i];
    ULONG j = i;
    while (j > 0 && items[j - 1].verb > t.verb) { items[j] = items[j - 1]; j--; }
    items[j] = t;
  }
  return enumverb_make(items, count, 0, out);
}
EXPORT HRESULT WINAPI OleReleaseEnumVerbCache(void) { return S_OK; }

typedef struct EnumFmtObj {
  IEnumFORMATETC iface;
  LONG refs;
  FORMATETC *fmts;
  ULONG count, pos;
} EnumFmtObj;
static const IEnumFORMATETCVtbl g_enumfmt_vtbl;
static HRESULT WINAPI enumfmt_qi(IEnumFORMATETC *me, REFIID riid, void **out) {
  if (!out) return E_INVALIDARG;
  *out = NULL;
  if (IsEqualGUID(riid, &IID_IUnknown) || IsEqualGUID(riid, &IID_IEnumFORMATETC)) { *out = me; IEnumFORMATETC_AddRef(me); return S_OK; }
  return E_NOINTERFACE;
}
static ULONG WINAPI enumfmt_addref(IEnumFORMATETC *me) { return (ULONG)InterlockedIncrement(&((EnumFmtObj *)me)->refs); }
static ULONG WINAPI enumfmt_release(IEnumFORMATETC *me) {
  EnumFmtObj *e = (EnumFmtObj *)me;
  LONG r = InterlockedDecrement(&e->refs);
  if (r == 0) { mem_free(e->fmts); mem_free(e); }
  return (ULONG)r;
}
static HRESULT WINAPI enumfmt_next(IEnumFORMATETC *me, ULONG celt, FORMATETC *out, ULONG *fetched) {
  EnumFmtObj *e = (EnumFmtObj *)me;
  ULONG n = 0;
  if (!out && celt) return E_INVALIDARG;
  if (fetched) *fetched = 0;
  while (n < celt && e->pos < e->count) out[n++] = e->fmts[e->pos++];
  if (fetched) *fetched = n;
  return n == celt ? S_OK : S_FALSE;
}
static HRESULT WINAPI enumfmt_skip(IEnumFORMATETC *me, ULONG celt) {
  EnumFmtObj *e = (EnumFmtObj *)me;
  if (celt > e->count - e->pos) { e->pos = e->count; return S_FALSE; }
  e->pos += celt;
  return S_OK;
}
static HRESULT WINAPI enumfmt_reset(IEnumFORMATETC *me) { ((EnumFmtObj *)me)->pos = 0; return S_OK; }
static HRESULT enumfmt_make(FORMATETC *fmts, ULONG count, ULONG pos, IEnumFORMATETC **out) {
  EnumFmtObj *e = mem_alloc(sizeof(*e));
  if (!e) { mem_free(fmts); return E_OUTOFMEMORY; }
  e->iface.lpVtbl = (IEnumFORMATETCVtbl *)&g_enumfmt_vtbl;
  e->refs = 1; e->fmts = fmts; e->count = count; e->pos = pos;
  *out = &e->iface;
  return S_OK;
}
static HRESULT WINAPI enumfmt_clone(IEnumFORMATETC *me, IEnumFORMATETC **out) {
  EnumFmtObj *e = (EnumFmtObj *)me;
  FORMATETC *copy = NULL;
  if (!out) return E_INVALIDARG;
  *out = NULL;
  if (e->count) {
    copy = mem_alloc(e->count * sizeof(FORMATETC));
    if (!copy) return E_OUTOFMEMORY;
    mem_copy(copy, e->fmts, e->count * sizeof(FORMATETC));
  }
  return enumfmt_make(copy, e->count, e->pos, out);
}
static const IEnumFORMATETCVtbl g_enumfmt_vtbl = {enumfmt_qi, enumfmt_addref, enumfmt_release, enumfmt_next, enumfmt_skip, enumfmt_reset, enumfmt_clone};

/* HKCR\CLSID\{..}\DataFormats\GetSet\<n> = "format,aspect,medium,direction" (the format a number or a registered name; direction 1 get, 2 set, 3 both) */
EXPORT HRESULT WINAPI OleRegEnumFormatEtc(REFCLSID clsid, DWORD direction, LPENUMFORMATETC *out) {
  WCHAR path[128], keyname[32], text[160], sub[96];
  HKEY keys;
  FORMATETC *fmts = NULL;
  ULONG count = 0;
  if (!out) return E_INVALIDARG;
  *out = NULL;
  if (direction != DATADIR_GET && direction != DATADIR_SET) return E_INVALIDARG;
  if (!clsid_registered(clsid)) return REGDB_E_CLASSNOTREG;
  clsid_key_path(clsid, W('D', 'a', 't', 'a', 'F', 'o', 'r', 'm', 'a', 't', 's', '\\', 'G', 'e', 't', 'S', 'e', 't'), path);
  if (RegOpenKeyExW(HKEY_CLASSES_ROOT, path, 0, KEY_READ, &keys) != ERROR_SUCCESS) return REGDB_E_KEYMISSING;
  fmts = mem_alloc(64 * sizeof(FORMATETC));
  if (!fmts) { RegCloseKey(keys); return E_OUTOFMEMORY; }
  for (DWORD index = 0; count < 64; index++) {
    DWORD len = 32;
    const WCHAR *p;
    LONG aspect = DVASPECT_CONTENT, tymed = TYMED_HGLOBAL, dir = 1, cfn;
    UINT cf;
    if (RegEnumKeyExW(keys, index, keyname, &len, NULL, NULL, NULL, NULL) != ERROR_SUCCESS) break;
    mem_copy(sub, W('D', 'a', 't', 'a', 'F', 'o', 'r', 'm', 'a', 't', 's', '\\', 'G', 'e', 't', 'S', 'e', 't', '\\'), 19 * sizeof(WCHAR));
    wcopy_n(sub + 19, keyname, 32);
    clsid_key_path(clsid, sub, path);
    if (reg_class_string(path, NULL, text, 160) != ERROR_SUCCESS) continue;
    p = text;
    if (parse_long(&p, &cfn) && (*p == ',' || !*p)) cf = (UINT)cfn;
    else {
      WCHAR name[64];
      SIZE_T n = 0;
      while (text[n] && text[n] != ',' && n < 63) { name[n] = text[n]; n++; }
      name[n] = 0;
      cf = ole_clipformat(name);
      p = text + n;
    }
    if (!cf) continue;
    if (*p == ',') { p++; if (!parse_long(&p, &aspect)) aspect = DVASPECT_CONTENT; }
    if (*p == ',') { p++; if (!parse_long(&p, &tymed)) tymed = TYMED_HGLOBAL; }
    if (*p == ',') { p++; if (!parse_long(&p, &dir)) dir = 1; }
    if (!((DWORD)dir & direction)) continue;
    fmts[count].cfFormat = (CLIPFORMAT)cf; fmts[count].ptd = NULL; fmts[count].dwAspect = (DWORD)aspect; fmts[count].lindex = -1; fmts[count].tymed = (DWORD)tymed;
    count++;
  }
  RegCloseKey(keys);
  return enumfmt_make(fmts, count, 0, out);
}

/* ---- auto convert --------------------------------------------------------------------------------------------------------------------------------------------------- */

EXPORT HRESULT WINAPI OleGetAutoConvert(REFCLSID old_clsid, LPCLSID new_clsid) {
  WCHAR path[128], text[80];
  if (!new_clsid) return E_INVALIDARG;
  *new_clsid = *old_clsid;
  if (!clsid_registered(old_clsid)) return REGDB_E_CLASSNOTREG;
  clsid_key_path(old_clsid, W('A', 'u', 't', 'o', 'C', 'o', 'n', 'v', 'e', 'r', 't', 'T', 'o'), path);
  if (reg_class_string(path, NULL, text, 80) != ERROR_SUCCESS) return REGDB_E_KEYMISSING;
  GUID g;
  if (!guid_from_text(text, &g)) return REGDB_E_KEYMISSING;
  *new_clsid = g;
  return S_OK;
}
EXPORT HRESULT WINAPI OleSetAutoConvert(REFCLSID old_clsid, REFCLSID new_clsid) {
  WCHAR path[100], text[40];
  HKEY key, sub;
  const WCHAR *name = W('A', 'u', 't', 'o', 'C', 'o', 'n', 'v', 'e', 'r', 't', 'T', 'o');
  clsid_key_path(old_clsid, NULL, path);
  if (RegOpenKeyExW(HKEY_CLASSES_ROOT, path, 0, KEY_READ | KEY_WRITE, &key) != ERROR_SUCCESS) return REGDB_E_CLASSNOTREG;
  if (IsEqualGUID(new_clsid, &g_guid_null)) {
    RegDeleteKeyW(key, name);
    RegCloseKey(key);
    return S_OK;
  }
  LONG r = RegCreateKeyExW(key, name, 0, NULL, 0, KEY_WRITE, NULL, &sub, NULL);
  RegCloseKey(key);
  if (r != ERROR_SUCCESS) return REGDB_E_WRITEREGDB;
  guid_to_text(new_clsid, text);
  r = RegSetValueExW(sub, NULL, 0, REG_SZ, (const BYTE *)text, 39 * sizeof(WCHAR));
  RegCloseKey(sub);
  return r == ERROR_SUCCESS ? S_OK : REGDB_E_WRITEREGDB;
}
EXPORT HRESULT WINAPI OleDoAutoConvert(LPSTORAGE stg, LPCLSID new_clsid) {
  STATSTG st;
  CLSID old_clsid;
  CLIPFORMAT cf = 0;
  LPOLESTR user_type = NULL;
  HRESULT hr;
  if (!new_clsid) return E_INVALIDARG;
  if (!stg) { *new_clsid = g_guid_null; return E_INVALIDARG; }
  hr = IStorage_Stat(stg, &st, STATFLAG_NONAME);
  if (FAILED(hr)) { *new_clsid = g_guid_null; return hr; }
  old_clsid = st.clsid;
  *new_clsid = old_clsid;
  hr = OleGetAutoConvert(&old_clsid, new_clsid);
  if (FAILED(hr)) return hr;
  if (FAILED(ReadFmtUserTypeStg(stg, &cf, &user_type))) { cf = 0; user_type = NULL; }
  hr = IStorage_SetClass(stg, new_clsid);
  if (SUCCEEDED(hr)) hr = WriteFmtUserTypeStg(stg, cf, user_type);
  if (user_type) CoTaskMemFree(user_type);
  if (SUCCEEDED(hr)) hr = SetConvertStg(stg, TRUE);
  return hr;
}

/* ---- the \1Ole stream ------------------------------------------------------------------------------------------------------------------------------------------------ */

#define OLE_STREAM_VERSION 0x02000001
#define OLE_STREAM_FLAG_LINK 0x1
#define OLE_STREAM_MAX_MONIKER 0x100000

/* Creates \1Ole: version, flags, link update option, reserved, the size of the reserved moniker stream (0). `ppstm` gets the stream (at its end) when wanted. */
EXPORT HRESULT WINAPI WriteOleStg(LPSTORAGE stg, IOleObject *ole, DWORD reserved, LPSTREAM *ppstm) {
  DWORD header[5] = {OLE_STREAM_VERSION, 0, 0, 0, 0};
  IStream *stm;
  IOleLink *link;
  HRESULT hr;
  (void)reserved;
  if (ppstm) *ppstm = NULL;
  if (!stg) return E_INVALIDARG;
  hr = IStorage_CreateStream(stg, W(1, 'O', 'l', 'e'), STGM_CREATE | STGM_WRITE | STGM_SHARE_EXCLUSIVE, 0, 0, &stm);
  if (FAILED(hr)) return hr;
  if (ole && SUCCEEDED(IOleObject_QueryInterface(ole, &IID_IOleLink, (void **)&link))) {
    DWORD opt = 0;
    header[1] |= OLE_STREAM_FLAG_LINK;
    if (SUCCEEDED(IOleLink_GetUpdateOptions(link, &opt))) header[2] = opt;
    IOleLink_Release(link);
  }
  hr = stream_write_all(stm, header, sizeof(header));
  if (FAILED(hr) || !ppstm) IStream_Release(stm);
  else *ppstm = stm;
  return hr;
}

/* The pieces of \1Ole: the flags, the link update option, the reserved field, the stream (positioned after the 20 byte header) and the reserved moniker stream (a memory stream
 * with the bytes the \1Ole stream holds for it, NULL when it is empty). */
EXPORT HRESULT WINAPI ReadOleStg(LPSTORAGE stg, DWORD *flags, DWORD *opt_update, DWORD *reserved, LPSTREAM *ppstm, LPSTREAM *ppstm_reserved) {
  DWORD header[5];
  IStream *stm;
  IStream *mem = NULL;
  HRESULT hr;
  if (ppstm) *ppstm = NULL;
  if (ppstm_reserved) *ppstm_reserved = NULL;
  if (!stg) return E_INVALIDARG;
  hr = IStorage_OpenStream(stg, W(1, 'O', 'l', 'e'), NULL, STGM_READ | STGM_SHARE_EXCLUSIVE, 0, &stm);
  if (FAILED(hr)) return hr;
  hr = stream_read_all(stm, header, sizeof(header));
  if (FAILED(hr)) { IStream_Release(stm); return hr == STG_E_READFAULT ? STG_E_INVALIDHEADER : hr; }
  if (header[4] > OLE_STREAM_MAX_MONIKER) { IStream_Release(stm); return STG_E_INVALIDHEADER; }
  if (ppstm_reserved && header[4]) {
    BYTE *buf = mem_alloc(header[4]);
    if (!buf) { IStream_Release(stm); return E_OUTOFMEMORY; }
    hr = stream_read_all(stm, buf, header[4]);
    if (SUCCEEDED(hr)) hr = CreateStreamOnHGlobal(NULL, TRUE, &mem);
    if (SUCCEEDED(hr)) hr = stream_write_all(mem, buf, header[4]);
    if (SUCCEEDED(hr)) hr = stream_seek_to(mem, 0);
    mem_free(buf);
    if (FAILED(hr)) { if (mem) IStream_Release(mem); IStream_Release(stm); return hr; }
  }
  if (flags) *flags = header[1];
  if (opt_update) *opt_update = header[2];
  if (reserved) *reserved = header[3];
  if (ppstm_reserved) *ppstm_reserved = mem;
  if (ppstm) *ppstm = stm; else IStream_Release(stm);
  return S_OK;
}

/* ---- loading and saving objects ------------------------------------------------------------------------------------------------------------------------------------- */

EXPORT HRESULT WINAPI OleSaveToStream(LPPERSISTSTREAM ps, LPSTREAM stm) {
  CLSID clsid;
  HRESULT hr;
  if (!ps || !stm) return E_INVALIDARG;
  hr = IPersistStream_GetClassID(ps, &clsid);
  if (FAILED(hr)) return hr;
  hr = WriteClassStm(stm, &clsid);
  if (FAILED(hr)) return hr;
  return IPersistStream_Save(ps, stm, TRUE);
}
EXPORT HRESULT WINAPI OleLoadFromStream(LPSTREAM stm, REFIID riid, LPVOID *out) {
  CLSID clsid;
  IPersistStream *ps;
  HRESULT hr;
  if (!out) return E_INVALIDARG;
  *out = NULL;
  if (!stm) return E_INVALIDARG;
  hr = ReadClassStm(stm, &clsid);
  if (FAILED(hr)) return hr;
  hr = CoCreateInstance(&clsid, NULL, CLSCTX_INPROC_SERVER | CLSCTX_INPROC_HANDLER, &IID_IPersistStream, (void **)&ps);
  if (FAILED(hr)) return hr;
  hr = IPersistStream_Load(ps, stm);
  if (SUCCEEDED(hr)) hr = IPersistStream_QueryInterface(ps, riid, out);
  IPersistStream_Release(ps);
  return hr;
}
EXPORT HRESULT WINAPI OleSave(LPPERSISTSTORAGE ps, LPSTORAGE stg, BOOL same_as_load) {
  CLSID clsid;
  HRESULT hr;
  if (!ps || !stg) return E_INVALIDARG;
  hr = IPersistStorage_GetClassID(ps, &clsid);
  if (FAILED(hr)) return hr;
  hr = WriteClassStg(stg, &clsid);
  if (FAILED(hr)) return hr;
  return IPersistStorage_Save(ps, stg, same_as_load);
}

/* ---- running ----------------------------------------------------------------------------------------------------------------------------------------------------------- */

EXPORT HRESULT WINAPI OleRun(LPUNKNOWN unk) {
  IRunnableObject *run;
  HRESULT hr;
  if (!unk) return E_INVALIDARG;
  if (FAILED(IUnknown_QueryInterface(unk, &IID_IRunnableObject, (void **)&run))) return S_OK;
  hr = IRunnableObject_Run(run, NULL);
  IRunnableObject_Release(run);
  return hr;
}
EXPORT BOOL WINAPI OleIsRunning(LPOLEOBJECT obj) {
  IRunnableObject *run;
  BOOL r;
  if (!obj) return FALSE;
  if (FAILED(IOleObject_QueryInterface(obj, &IID_IRunnableObject, (void **)&run))) return TRUE;
  r = IRunnableObject_IsRunning(run);
  IRunnableObject_Release(run);
  return r;
}
EXPORT HRESULT WINAPI OleLockRunning(LPUNKNOWN unk, BOOL lock_it, BOOL last_unlock_closes) {
  IRunnableObject *run;
  HRESULT hr;
  if (!unk) return E_INVALIDARG;
  if (FAILED(IUnknown_QueryInterface(unk, &IID_IRunnableObject, (void **)&run))) return S_OK;
  hr = IRunnableObject_LockRunning(run, lock_it, last_unlock_closes);
  IRunnableObject_Release(run);
  return hr;
}
EXPORT HRESULT WINAPI OleSetContainedObject(LPUNKNOWN unk, BOOL contained) {
  IRunnableObject *run;
  HRESULT hr;
  if (!unk) return E_INVALIDARG;
  if (FAILED(IUnknown_QueryInterface(unk, &IID_IRunnableObject, (void **)&run))) return S_OK;
  hr = IRunnableObject_SetContainedObject(run, contained);
  IRunnableObject_Release(run);
  return hr;
}

/* ---- what a data object can create ---------------------------------------------------------------------------------------------------------------------------------- */

EXPORT HRESULT WINAPI OleQueryCreateFromData(LPDATAOBJECT data) {
  if (!data) return E_INVALIDARG;
  if (data_has(data, ole_clipformat_a("Embedded Object"), TYMED_ISTORAGE) || data_has(data, ole_clipformat_a("Embed Source"), TYMED_ISTORAGE) ||
      data_has(data, ole_clipformat_a("FileName"), TYMED_HGLOBAL) || data_has(data, ole_clipformat_a("FileNameW"), TYMED_HGLOBAL) ||
      data_has(data, ole_clipformat_a("OwnerLink"), TYMED_HGLOBAL))
    return S_OK;
  if (data_has(data, CF_METAFILEPICT, TYMED_MFPICT) || data_has(data, CF_DIB, TYMED_HGLOBAL) || data_has(data, CF_BITMAP, TYMED_GDI) ||
      data_has(data, CF_ENHMETAFILE, TYMED_ENHMF))
    return OLE_S_STATIC;
  return DV_E_FORMATETC;
}
EXPORT HRESULT WINAPI OleQueryLinkFromData(LPDATAOBJECT data) {
  if (!data) return E_INVALIDARG;
  if (data_has(data, ole_clipformat_a("Link Source"), TYMED_ISTREAM) || data_has(data, ole_clipformat_a("ObjectLink"), TYMED_HGLOBAL) ||
      data_has(data, ole_clipformat_a("FileName"), TYMED_HGLOBAL) || data_has(data, ole_clipformat_a("FileNameW"), TYMED_HGLOBAL))
    return S_OK;
  return S_FALSE;
}

/* ---- the default handler and the embedding helper ---------------------------------------------------------------------------------------------------------------------- */
/* An aggregatable object that stands in for an embedded object. Everything that can be done without the object's server is done here (the presentation cache is a data cache
 * aggregated into the handler); the rest needs the server, which is made from a class factory (the one the helper was given, or the registered one) when the handler is run.
 * The interfaces of the server are looked up on first use. */

typedef struct DefHandler {
  IUnknown inner;
  IOleObject ole;
  IDataObject data;
  IRunnableObject run;
  IPersistStorage persist;
  IExternalConnection ext;
  LONG refs;
  IUnknown *outer;
  CLSID clsid;
  DWORD flags;
  IClassFactory *factory;
  IUnknown *server;
  IOleObject *server_ole;
  IDataObject *server_data;
  BOOL server_ole_tried, server_data_tried;
  IUnknown *cache_unk;
  IOleClientSite *site;
  WCHAR *container_app, *container_obj;
  IOleAdviseHolder *adv;
  IDataAdviseHolder *dadv;
  IStorage *storage;
  BOOL contained;
  LONG connections;
} DefHandler;

#define DH_FROM(field, me) ((DefHandler *)((BYTE *)(me) - offsetof(DefHandler, field)))
static const IUnknownVtbl g_dh_inner_vtbl;
static const IOleObjectVtbl g_dh_ole_vtbl;
static const IDataObjectVtbl g_dh_data_vtbl;
static const IRunnableObjectVtbl g_dh_run_vtbl;
static const IPersistStorageVtbl g_dh_persist_vtbl;
static const IExternalConnectionVtbl g_dh_ext_vtbl;

static HRESULT dh_cache_iface(DefHandler *This, REFIID riid, void **out) {
  *out = NULL;
  return This->cache_unk ? IUnknown_QueryInterface(This->cache_unk, riid, out) : E_NOINTERFACE;
}
static BOOL dh_running(DefHandler *This) { return This->server != NULL; }

static void dh_stop(DefHandler *This) {
  IOleCacheControl *ctl;
  if (!This->server) return;
  if (SUCCEEDED(dh_cache_iface(This, &IID_IOleCacheControl, (void **)&ctl))) { IOleCacheControl_OnStop(ctl); IOleCacheControl_Release(ctl); }
  if (This->server_ole) IOleObject_Release(This->server_ole);
  if (This->server_data) IDataObject_Release(This->server_data);
  IUnknown_Release(This->server);
  This->server = NULL; This->server_ole = NULL; This->server_data = NULL;
  This->server_ole_tried = This->server_data_tried = FALSE;
}

/* the IOleObject of the running server (not owned), after telling it what the handler has been told */
static IOleObject *dh_server_ole(DefHandler *This) {
  if (!This->server) return NULL;
  if (!This->server_ole_tried) {
    This->server_ole_tried = TRUE;
    if (SUCCEEDED(IUnknown_QueryInterface(This->server, &IID_IOleObject, (void **)&This->server_ole))) {
      if (This->site) IOleObject_SetClientSite(This->server_ole, This->site);
      if (This->container_app) IOleObject_SetHostNames(This->server_ole, This->container_app, This->container_obj);
    }
  }
  return This->server_ole;
}
static IDataObject *dh_server_data(DefHandler *This) {
  if (!This->server) return NULL;
  if (!This->server_data_tried) {
    This->server_data_tried = TRUE;
    if (SUCCEEDED(IUnknown_QueryInterface(This->server, &IID_IDataObject, (void **)&This->server_data))) {
      IOleCacheControl *ctl;
      if (SUCCEEDED(dh_cache_iface(This, &IID_IOleCacheControl, (void **)&ctl))) { IOleCacheControl_OnRun(ctl, This->server_data); IOleCacheControl_Release(ctl); }
    }
  }
  return This->server_data;
}

static HRESULT dh_run(DefHandler *This, LPBINDCTX ctx) {
  IClassFactory *cf = This->factory;
  IUnknown *srv = NULL;
  HRESULT hr;
  IPersistStorage *ps;
  (void)ctx;
  if (This->server) return S_OK;
  if (cf) IClassFactory_AddRef(cf);
  else {
    hr = CoGetClassObject(&This->clsid, (This->flags & EMBDHLP_INPROC_SERVER) ? CLSCTX_INPROC_SERVER : CLSCTX_LOCAL_SERVER, NULL, &IID_IClassFactory, (void **)&cf);
    if (FAILED(hr)) return hr;
  }
  hr = IClassFactory_CreateInstance(cf, NULL, &IID_IUnknown, (void **)&srv);
  IClassFactory_Release(cf);
  if (FAILED(hr)) return hr;
  This->server = srv;
  if (This->storage && SUCCEEDED(IUnknown_QueryInterface(srv, &IID_IPersistStorage, (void **)&ps))) {
    IPersistStorage_InitNew(ps, This->storage);
    IPersistStorage_Release(ps);
  }
  return S_OK;
}

static void dh_destroy(DefHandler *This) {
  This->refs = 1;
  dh_stop(This);
  if (This->factory) IClassFactory_Release(This->factory);
  if (This->site) IOleClientSite_Release(This->site);
  if (This->adv) IOleAdviseHolder_Release(This->adv);
  if (This->dadv) IDataAdviseHolder_Release(This->dadv);
  if (This->storage) IStorage_Release(This->storage);
  if (This->cache_unk) IUnknown_Release(This->cache_unk);
  mem_free(This->container_app);
  mem_free(This->container_obj);
  mem_free(This);
}

/* -- the inner IUnknown -- */

static HRESULT WINAPI dh_inner_qi(IUnknown *me, REFIID riid, void **out) {
  DefHandler *This = (DefHandler *)me;
  void *found = NULL;
  if (!out) return E_INVALIDARG;
  *out = NULL;
  if (IsEqualGUID(riid, &IID_IUnknown)) { IUnknown_AddRef(&This->inner); *out = &This->inner; return S_OK; }
  if (IsEqualGUID(riid, &IID_IOleObject)) found = &This->ole;
  else if (IsEqualGUID(riid, &IID_IDataObject)) found = &This->data;
  else if (IsEqualGUID(riid, &IID_IRunnableObject)) found = &This->run;
  else if (IsEqualGUID(riid, &IID_IPersist) || IsEqualGUID(riid, &IID_IPersistStorage)) found = &This->persist;
  else if (IsEqualGUID(riid, &IID_IExternalConnection)) found = &This->ext;
  if (found) { *out = found; IUnknown_AddRef((IUnknown *)found); return S_OK; }
  if (IsEqualGUID(riid, &IID_IOleCache) || IsEqualGUID(riid, &IID_IOleCache2) || IsEqualGUID(riid, &IID_IOleCacheControl) || IsEqualGUID(riid, &IID_IViewObject) ||
      IsEqualGUID(riid, &IID_IViewObject2))
    return dh_cache_iface(This, riid, out);
  if (This->server) return IUnknown_QueryInterface(This->server, riid, out);     /* (the failure of the server is the answer) */
  return E_NOINTERFACE;
}
static ULONG WINAPI dh_inner_addref(IUnknown *me) { return (ULONG)InterlockedIncrement(&((DefHandler *)me)->refs); }
static ULONG WINAPI dh_inner_release(IUnknown *me) {
  DefHandler *This = (DefHandler *)me;
  LONG r = InterlockedDecrement(&This->refs);
  if (r == 0) dh_destroy(This);
  return (ULONG)r;
}
static const IUnknownVtbl g_dh_inner_vtbl = {dh_inner_qi, dh_inner_addref, dh_inner_release};

#define DH_DELEGATE(Type, field, prefix) \
  static HRESULT WINAPI prefix##_qi(Type *me, REFIID riid, void **out) { return IUnknown_QueryInterface(DH_FROM(field, me)->outer, riid, out); } \
  static ULONG WINAPI prefix##_addref(Type *me) { return IUnknown_AddRef(DH_FROM(field, me)->outer); } \
  static ULONG WINAPI prefix##_release(Type *me) { return IUnknown_Release(DH_FROM(field, me)->outer); }
DH_DELEGATE(IOleObject, ole, dho)
DH_DELEGATE(IDataObject, data, dhd)
DH_DELEGATE(IRunnableObject, run, dhr)
DH_DELEGATE(IPersistStorage, persist, dhp)
DH_DELEGATE(IExternalConnection, ext, dhe)

/* -- IOleObject -- */

static HRESULT WINAPI dho_set_client_site(IOleObject *me, IOleClientSite *site) {
  DefHandler *This = DH_FROM(ole, me);
  IOleObject *srv = dh_server_ole(This);
  if (site) IOleClientSite_AddRef(site);
  if (This->site) IOleClientSite_Release(This->site);
  This->site = site;
  if (srv) IOleObject_SetClientSite(srv, site);
  return S_OK;
}
static HRESULT WINAPI dho_get_client_site(IOleObject *me, IOleClientSite **out) {
  DefHandler *This = DH_FROM(ole, me);
  if (!out) return E_INVALIDARG;
  *out = This->site;
  if (This->site) IOleClientSite_AddRef(This->site);
  return S_OK;
}
static HRESULT WINAPI dho_set_host_names(IOleObject *me, LPCOLESTR app, LPCOLESTR obj) {
  DefHandler *This = DH_FROM(ole, me);
  IOleObject *srv;
  WCHAR *a = app ? wdup_heap(app) : NULL, *o = obj ? wdup_heap(obj) : NULL;
  if ((app && !a) || (obj && !o)) { mem_free(a); mem_free(o); return E_OUTOFMEMORY; }
  mem_free(This->container_app);
  mem_free(This->container_obj);
  This->container_app = a; This->container_obj = o;
  srv = dh_server_ole(This);
  if (srv) IOleObject_SetHostNames(srv, app, obj);
  return S_OK;
}
static HRESULT WINAPI dho_close(IOleObject *me, DWORD save) {
  DefHandler *This = DH_FROM(ole, me);
  HRESULT hr = S_OK;
  IOleObject *srv;
  if (!dh_running(This)) return S_OK;
  if (save == OLECLOSE_SAVEIFDIRTY && This->site) {
    IPersistStorage *ps;
    if (SUCCEEDED(IUnknown_QueryInterface(This->server, &IID_IPersistStorage, (void **)&ps))) {
      if (IPersistStorage_IsDirty(ps) == S_OK) IOleClientSite_SaveObject(This->site);
      IPersistStorage_Release(ps);
    }
  }
  srv = dh_server_ole(This);
  if (srv) hr = IOleObject_Close(srv, save);
  dh_stop(This);
  return hr;
}
static HRESULT WINAPI dho_set_moniker(IOleObject *me, DWORD which, IMoniker *mk) {
  DefHandler *This = DH_FROM(ole, me);
  IOleObject *srv = dh_server_ole(This);
  return srv ? IOleObject_SetMoniker(srv, which, mk) : S_OK;
}
static HRESULT WINAPI dho_get_moniker(IOleObject *me, DWORD assign, DWORD which, IMoniker **out) {
  DefHandler *This = DH_FROM(ole, me);
  IOleObject *srv = dh_server_ole(This);
  if (!out) return E_INVALIDARG;
  *out = NULL;
  if (srv) return IOleObject_GetMoniker(srv, assign, which, out);
  if (This->site) return IOleClientSite_GetMoniker(This->site, assign, which, out);
  return E_FAIL;
}
static HRESULT WINAPI dho_init_from_data(IOleObject *me, IDataObject *data, BOOL creation, DWORD reserved) {
  IOleObject *srv = dh_server_ole(DH_FROM(ole, me));
  return srv ? IOleObject_InitFromData(srv, data, creation, reserved) : OLE_E_NOTRUNNING;
}
static HRESULT WINAPI dho_get_clipboard_data(IOleObject *me, DWORD reserved, IDataObject **out) {
  IOleObject *srv = dh_server_ole(DH_FROM(ole, me));
  if (out) *out = NULL;
  return srv ? IOleObject_GetClipboardData(srv, reserved, out) : OLE_E_NOTRUNNING;
}
static HRESULT WINAPI dho_do_verb(IOleObject *me, LONG verb, MSG *msg, IOleClientSite *site, LONG index, HWND parent, LPCRECT rect) {
  DefHandler *This = DH_FROM(ole, me);
  IOleObject *srv;
  HRESULT hr = dh_run(This, NULL);
  if (FAILED(hr)) return hr;
  srv = dh_server_ole(This);
  if (!srv) return OLE_E_NOTRUNNING;
  return IOleObject_DoVerb(srv, verb, msg, site ? site : This->site, index, parent, rect);
}
static HRESULT WINAPI dho_enum_verbs(IOleObject *me, IEnumOLEVERB **out) {
  DefHandler *This = DH_FROM(ole, me);
  IOleObject *srv = dh_server_ole(This);
  if (srv) {
    HRESULT hr = IOleObject_EnumVerbs(srv, out);
    if (SUCCEEDED(hr)) return hr;
  }
  return OleRegEnumVerbs(&This->clsid, out);
}
static HRESULT WINAPI dho_update(IOleObject *me) {
  DefHandler *This = DH_FROM(ole, me);
  IOleObject *srv;
  HRESULT hr = dh_run(This, NULL);
  if (FAILED(hr)) return hr;
  srv = dh_server_ole(This);
  return srv ? IOleObject_Update(srv) : OLE_E_NOTRUNNING;
}
static HRESULT WINAPI dho_is_up_to_date(IOleObject *me) {
  IOleObject *srv = dh_server_ole(DH_FROM(ole, me));
  return srv ? IOleObject_IsUpToDate(srv) : OLE_E_NOTRUNNING;
}
static HRESULT WINAPI dho_get_user_class_id(IOleObject *me, CLSID *clsid) {
  if (!clsid) return E_INVALIDARG;
  *clsid = DH_FROM(ole, me)->clsid;
  return S_OK;
}
static HRESULT WINAPI dho_get_user_type(IOleObject *me, DWORD form, LPOLESTR *out) {
  DefHandler *This = DH_FROM(ole, me);
  IOleObject *srv = dh_server_ole(This);
  HRESULT hr;
  if (!out) return E_INVALIDARG;
  *out = NULL;
  if (srv && SUCCEEDED(hr = IOleObject_GetUserType(srv, form, out))) return hr;
  hr = OleRegGetUserType(&This->clsid, form, out);
  if (SUCCEEDED(hr)) return hr;
  *out = cotask_wdup(W('U', 'n', 'k', 'n', 'o', 'w', 'n'));
  return *out ? S_OK : E_OUTOFMEMORY;
}
static HRESULT WINAPI dho_set_extent(IOleObject *me, DWORD aspect, SIZEL *size) {
  IOleObject *srv = dh_server_ole(DH_FROM(ole, me));
  return srv ? IOleObject_SetExtent(srv, aspect, size) : OLE_E_NOTRUNNING;
}
static HRESULT WINAPI dho_get_extent(IOleObject *me, DWORD aspect, SIZEL *size) {
  DefHandler *This = DH_FROM(ole, me);
  IOleObject *srv = dh_server_ole(This);
  IViewObject2 *view;
  HRESULT hr;
  if (!size) return E_INVALIDARG;
  if (srv && SUCCEEDED(hr = IOleObject_GetExtent(srv, aspect, size))) return hr;
  if (SUCCEEDED(dh_cache_iface(This, &IID_IViewObject2, (void **)&view))) {
    hr = IViewObject2_GetExtent(view, aspect, -1, NULL, size);
    IViewObject2_Release(view);
    if (SUCCEEDED(hr)) return hr;
  }
  return OLE_E_BLANK;
}
static HRESULT dh_adv_holder(DefHandler *This) {
  if (This->adv) return S_OK;
  return CreateOleAdviseHolder(&This->adv);
}
static HRESULT WINAPI dho_advise(IOleObject *me, IAdviseSink *sink, DWORD *conn) {
  DefHandler *This = DH_FROM(ole, me);
  HRESULT hr = dh_adv_holder(This);
  if (FAILED(hr)) return hr;
  return IOleAdviseHolder_Advise(This->adv, sink, conn);
}
static HRESULT WINAPI dho_unadvise(IOleObject *me, DWORD conn) {
  DefHandler *This = DH_FROM(ole, me);
  HRESULT hr = dh_adv_holder(This);
  if (FAILED(hr)) return hr;
  return IOleAdviseHolder_Unadvise(This->adv, conn);
}
static HRESULT WINAPI dho_enum_advise(IOleObject *me, IEnumSTATDATA **out) {
  DefHandler *This = DH_FROM(ole, me);
  HRESULT hr = dh_adv_holder(This);
  if (FAILED(hr)) return hr;
  return IOleAdviseHolder_EnumAdvise(This->adv, out);
}
static HRESULT WINAPI dho_get_misc_status(IOleObject *me, DWORD aspect, DWORD *status) {
  DefHandler *This = DH_FROM(ole, me);
  IOleObject *srv = dh_server_ole(This);
  HRESULT hr;
  if (!status) return E_INVALIDARG;
  if (srv && SUCCEEDED(hr = IOleObject_GetMiscStatus(srv, aspect, status))) return hr;
  hr = OleRegGetMiscStatus(&This->clsid, aspect, status);
  if (FAILED(hr)) *status = 0;
  return hr;
}
static HRESULT WINAPI dho_set_color_scheme(IOleObject *me, LOGPALETTE *palette) {
  IOleObject *srv = dh_server_ole(DH_FROM(ole, me));
  return srv ? IOleObject_SetColorScheme(srv, palette) : OLE_E_NOTRUNNING;
}
static const IOleObjectVtbl g_dh_ole_vtbl = {dho_qi, dho_addref, dho_release, dho_set_client_site, dho_get_client_site, dho_set_host_names, dho_close, dho_set_moniker, dho_get_moniker,
    dho_init_from_data, dho_get_clipboard_data, dho_do_verb, dho_enum_verbs, dho_update, dho_is_up_to_date, dho_get_user_class_id, dho_get_user_type, dho_set_extent, dho_get_extent,
    dho_advise, dho_unadvise, dho_enum_advise, dho_get_misc_status, dho_set_color_scheme};

/* -- IDataObject: the server's when it runs, else the cache's -- */

static HRESULT WINAPI dhd_get_data(IDataObject *me, FORMATETC *fmt, STGMEDIUM *med) {
  DefHandler *This = DH_FROM(data, me);
  IDataObject *srv = dh_server_data(This), *cache;
  HRESULT hr = OLE_E_NOTRUNNING;
  if (srv) { hr = IDataObject_GetData(srv, fmt, med); if (SUCCEEDED(hr)) return hr; }
  if (SUCCEEDED(dh_cache_iface(This, &IID_IDataObject, (void **)&cache))) {
    HRESULT h2 = IDataObject_GetData(cache, fmt, med);
    IDataObject_Release(cache);
    if (SUCCEEDED(h2) || !srv) return h2;
  }
  return hr;
}
static HRESULT WINAPI dhd_get_data_here(IDataObject *me, FORMATETC *fmt, STGMEDIUM *med) {
  IDataObject *srv = dh_server_data(DH_FROM(data, me));
  return srv ? IDataObject_GetDataHere(srv, fmt, med) : OLE_E_NOTRUNNING;
}
static HRESULT WINAPI dhd_query_get_data(IDataObject *me, FORMATETC *fmt) {
  DefHandler *This = DH_FROM(data, me);
  IDataObject *srv = dh_server_data(This), *cache;
  if (srv) return IDataObject_QueryGetData(srv, fmt);
  if (SUCCEEDED(dh_cache_iface(This, &IID_IDataObject, (void **)&cache))) {
    HRESULT hr = IDataObject_QueryGetData(cache, fmt);
    IDataObject_Release(cache);
    if (hr == S_OK) return hr;
  }
  return OLE_E_NOTRUNNING;
}
static HRESULT WINAPI dhd_get_canonical(IDataObject *me, FORMATETC *in, FORMATETC *out) {
  IDataObject *srv = dh_server_data(DH_FROM(data, me));
  return srv ? IDataObject_GetCanonicalFormatEtc(srv, in, out) : OLE_E_NOTRUNNING;
}
static HRESULT WINAPI dhd_set_data(IDataObject *me, FORMATETC *fmt, STGMEDIUM *med, BOOL release) {
  IDataObject *srv = dh_server_data(DH_FROM(data, me));
  return srv ? IDataObject_SetData(srv, fmt, med, release) : OLE_E_NOTRUNNING;
}
static HRESULT WINAPI dhd_enum_format_etc(IDataObject *me, DWORD dir, IEnumFORMATETC **out) {
  DefHandler *This = DH_FROM(data, me);
  IDataObject *srv = dh_server_data(This);
  if (srv) {
    HRESULT hr = IDataObject_EnumFormatEtc(srv, dir, out);
    if (SUCCEEDED(hr)) return hr;
  }
  return OleRegEnumFormatEtc(&This->clsid, dir, out);
}
static HRESULT dh_data_holder(DefHandler *This) {
  if (This->dadv) return S_OK;
  return CreateDataAdviseHolder(&This->dadv);
}
static HRESULT WINAPI dhd_dadvise(IDataObject *me, FORMATETC *fmt, DWORD advf, IAdviseSink *sink, DWORD *conn) {
  DefHandler *This = DH_FROM(data, me);
  HRESULT hr = dh_data_holder(This);
  if (FAILED(hr)) return hr;
  return IDataAdviseHolder_Advise(This->dadv, &This->data, fmt, advf, sink, conn);
}
static HRESULT WINAPI dhd_dunadvise(IDataObject *me, DWORD conn) {
  DefHandler *This = DH_FROM(data, me);
  HRESULT hr = dh_data_holder(This);
  if (FAILED(hr)) return hr;
  return IDataAdviseHolder_Unadvise(This->dadv, conn);
}
static HRESULT WINAPI dhd_enum_dadvise(IDataObject *me, IEnumSTATDATA **out) {
  DefHandler *This = DH_FROM(data, me);
  HRESULT hr = dh_data_holder(This);
  if (FAILED(hr)) return hr;
  return IDataAdviseHolder_EnumAdvise(This->dadv, out);
}
static const IDataObjectVtbl g_dh_data_vtbl = {dhd_qi, dhd_addref, dhd_release, dhd_get_data, dhd_get_data_here, dhd_query_get_data, dhd_get_canonical, dhd_set_data, dhd_enum_format_etc,
    dhd_dadvise, dhd_dunadvise, dhd_enum_dadvise};

/* -- IRunnableObject -- */

static HRESULT WINAPI dhr_get_running_class(IRunnableObject *me, LPCLSID clsid) {
  DefHandler *This = DH_FROM(run, me);
  if (!clsid) return E_INVALIDARG;
  if (!dh_running(This)) return OLE_E_NOTRUNNING;
  *clsid = This->clsid;
  return S_OK;
}
static HRESULT WINAPI dhr_run(IRunnableObject *me, LPBINDCTX ctx) { return dh_run(DH_FROM(run, me), ctx); }
static BOOL WINAPI dhr_is_running(IRunnableObject *me) { return dh_running(DH_FROM(run, me)); }
static HRESULT WINAPI dhr_lock_running(IRunnableObject *me, BOOL lock_it, BOOL last_unlock_closes) { (void)me; (void)lock_it; (void)last_unlock_closes; return S_OK; }
static HRESULT WINAPI dhr_set_contained(IRunnableObject *me, BOOL contained) { DH_FROM(run, me)->contained = contained; return S_OK; }
static const IRunnableObjectVtbl g_dh_run_vtbl = {dhr_qi, dhr_addref, dhr_release, dhr_get_running_class, dhr_run, dhr_is_running, dhr_lock_running, dhr_set_contained};

/* -- IPersistStorage: the cache keeps the presentations; the handler keeps \1Ole and \1CompObj -- */

static HRESULT WINAPI dhp_get_class_id(IPersistStorage *me, CLSID *clsid) {
  if (!clsid) return E_INVALIDARG;
  *clsid = DH_FROM(persist, me)->clsid;
  return S_OK;
}
static HRESULT dh_cache_persist(DefHandler *This, IPersistStorage **ps) { return dh_cache_iface(This, &IID_IPersistStorage, (void **)ps); }
static HRESULT WINAPI dhp_is_dirty(IPersistStorage *me) {
  DefHandler *This = DH_FROM(persist, me);
  IPersistStorage *ps;
  HRESULT hr = S_FALSE;
  if (SUCCEEDED(dh_cache_persist(This, &ps))) { hr = IPersistStorage_IsDirty(ps); IPersistStorage_Release(ps); }
  if (hr != S_OK && This->server && SUCCEEDED(IUnknown_QueryInterface(This->server, &IID_IPersistStorage, (void **)&ps))) {
    hr = IPersistStorage_IsDirty(ps);
    IPersistStorage_Release(ps);
  }
  return hr;
}
/* the class and the user type of a new object storage */
static void dh_write_identity(DefHandler *This, IStorage *stg) {
  LPOLESTR type = NULL;
  WriteClassStg(stg, &This->clsid);
  if (SUCCEEDED(OleRegGetUserType(&This->clsid, USERCLASSTYPE_FULL, &type))) {
    WriteFmtUserTypeStg(stg, 0, type);
    CoTaskMemFree(type);
  }
}
static HRESULT WINAPI dhp_init_new(IPersistStorage *me, IStorage *stg) {
  DefHandler *This = DH_FROM(persist, me);
  IPersistStorage *ps;
  HRESULT hr = S_OK;
  if (!stg) return E_INVALIDARG;
  if (This->storage) return CO_E_ALREADYINITIALIZED;
  if (SUCCEEDED(dh_cache_persist(This, &ps))) { hr = IPersistStorage_InitNew(ps, stg); IPersistStorage_Release(ps); }
  if (FAILED(hr)) return hr;
  hr = WriteOleStg(stg, NULL, 0, NULL);
  if (FAILED(hr)) return hr;
  dh_write_identity(This, stg);
  IStorage_AddRef(stg);
  This->storage = stg;
  if (This->server && SUCCEEDED(IUnknown_QueryInterface(This->server, &IID_IPersistStorage, (void **)&ps))) { IPersistStorage_InitNew(ps, stg); IPersistStorage_Release(ps); }
  return S_OK;
}
static HRESULT WINAPI dhp_load(IPersistStorage *me, IStorage *stg) {
  DefHandler *This = DH_FROM(persist, me);
  IPersistStorage *ps;
  HRESULT hr = S_OK;
  if (!stg) return E_INVALIDARG;
  if (This->storage) return CO_E_ALREADYINITIALIZED;
  if (SUCCEEDED(dh_cache_persist(This, &ps))) { hr = IPersistStorage_Load(ps, stg); IPersistStorage_Release(ps); }
  if (FAILED(hr)) return hr;
  IStorage_AddRef(stg);
  This->storage = stg;
  if (This->server && SUCCEEDED(IUnknown_QueryInterface(This->server, &IID_IPersistStorage, (void **)&ps))) { IPersistStorage_Load(ps, stg); IPersistStorage_Release(ps); }
  return S_OK;
}
static HRESULT WINAPI dhp_save(IPersistStorage *me, IStorage *stg, BOOL same_as_load) {
  DefHandler *This = DH_FROM(persist, me);
  IPersistStorage *ps;
  HRESULT hr = S_OK;
  if (!stg) return E_INVALIDARG;
  if (SUCCEEDED(dh_cache_persist(This, &ps))) { hr = IPersistStorage_Save(ps, stg, same_as_load); IPersistStorage_Release(ps); }
  if (FAILED(hr)) return hr;
  if (!same_as_load) {
    hr = WriteOleStg(stg, NULL, 0, NULL);
    if (FAILED(hr)) return hr;
    dh_write_identity(This, stg);
  }
  if (This->server && SUCCEEDED(IUnknown_QueryInterface(This->server, &IID_IPersistStorage, (void **)&ps))) {
    hr = IPersistStorage_Save(ps, stg, same_as_load);
    IPersistStorage_Release(ps);
  }
  return hr;
}
static HRESULT WINAPI dhp_save_completed(IPersistStorage *me, IStorage *stg) {
  DefHandler *This = DH_FROM(persist, me);
  IPersistStorage *ps;
  HRESULT hr = S_OK;
  if (SUCCEEDED(dh_cache_persist(This, &ps))) { hr = IPersistStorage_SaveCompleted(ps, stg); IPersistStorage_Release(ps); }
  if (FAILED(hr)) return hr;
  if (stg) {
    IStorage_AddRef(stg);
    if (This->storage) IStorage_Release(This->storage);
    This->storage = stg;
  }
  if (This->server && SUCCEEDED(IUnknown_QueryInterface(This->server, &IID_IPersistStorage, (void **)&ps))) { IPersistStorage_SaveCompleted(ps, stg); IPersistStorage_Release(ps); }
  return S_OK;
}
static HRESULT WINAPI dhp_hands_off(IPersistStorage *me) {
  DefHandler *This = DH_FROM(persist, me);
  IPersistStorage *ps;
  HRESULT hr = S_OK;
  if (SUCCEEDED(dh_cache_persist(This, &ps))) { hr = IPersistStorage_HandsOffStorage(ps); IPersistStorage_Release(ps); }
  if (This->server && SUCCEEDED(IUnknown_QueryInterface(This->server, &IID_IPersistStorage, (void **)&ps))) { IPersistStorage_HandsOffStorage(ps); IPersistStorage_Release(ps); }
  if (This->storage) { IStorage_Release(This->storage); This->storage = NULL; }
  return hr;
}
static const IPersistStorageVtbl g_dh_persist_vtbl = {dhp_qi, dhp_addref, dhp_release, dhp_get_class_id, dhp_is_dirty, dhp_init_new, dhp_load, dhp_save, dhp_save_completed, dhp_hands_off};

/* -- IExternalConnection -- */

static DWORD WINAPI dhe_add_connection(IExternalConnection *me, DWORD kind, DWORD reserved) {
  DefHandler *This = DH_FROM(ext, me);
  (void)reserved;
  return (kind & EXTCONN_STRONG) ? (DWORD)InterlockedIncrement(&This->connections) : 0;
}
static DWORD WINAPI dhe_release_connection(IExternalConnection *me, DWORD kind, DWORD reserved, BOOL last_closes) {
  DefHandler *This = DH_FROM(ext, me);
  LONG n;
  (void)reserved;
  if (!(kind & EXTCONN_STRONG)) return 0;
  n = InterlockedDecrement(&This->connections);
  if (n < 0) { InterlockedExchange(&This->connections, 0); return 0; }
  if (n == 0 && last_closes) IOleObject_Close(&This->ole, OLECLOSE_SAVEIFDIRTY);
  return (DWORD)n;
}
static const IExternalConnectionVtbl g_dh_ext_vtbl = {dhe_qi, dhe_addref, dhe_release, dhe_add_connection, dhe_release_connection};

/* -- creation -- */

EXPORT HRESULT WINAPI OleCreateEmbeddingHelper(REFCLSID clsid, LPUNKNOWN outer, DWORD flags, LPCLASSFACTORY cf, REFIID riid, LPVOID *out) {
  DefHandler *h;
  HRESULT hr;
  if (!out) return E_INVALIDARG;
  *out = NULL;
  if (!clsid || !riid) return E_INVALIDARG;
  if (outer && !IsEqualGUID(riid, &IID_IUnknown)) return CLASS_E_NOAGGREGATION;
  h = mem_alloc(sizeof(*h));
  if (!h) return E_OUTOFMEMORY;
  h->inner.lpVtbl = (IUnknownVtbl *)&g_dh_inner_vtbl;
  h->ole.lpVtbl = (IOleObjectVtbl *)&g_dh_ole_vtbl;
  h->data.lpVtbl = (IDataObjectVtbl *)&g_dh_data_vtbl;
  h->run.lpVtbl = (IRunnableObjectVtbl *)&g_dh_run_vtbl;
  h->persist.lpVtbl = (IPersistStorageVtbl *)&g_dh_persist_vtbl;
  h->ext.lpVtbl = (IExternalConnectionVtbl *)&g_dh_ext_vtbl;
  h->refs = 1;
  h->outer = outer ? outer : &h->inner;
  h->clsid = *clsid;
  h->flags = flags;
  if (cf) { h->factory = cf; IClassFactory_AddRef(cf); }
  hr = CreateDataCache(&h->inner, clsid, &IID_IUnknown, (void **)&h->cache_unk);
  if (FAILED(hr)) { dh_destroy(h); return hr; }
  if (cf && (flags & EMBDHLP_INPROC_SERVER) && !(flags & EMBDHLP_DELAYCREATE)) {
    hr = dh_run(h, NULL);
    if (FAILED(hr)) { dh_destroy(h); return hr; }
  }
  hr = IUnknown_QueryInterface(&h->inner, riid, out);
  IUnknown_Release(&h->inner);
  return hr;
}
EXPORT HRESULT WINAPI OleCreateDefaultHandler(REFCLSID clsid, LPUNKNOWN outer, REFIID riid, LPVOID *out) {
  return OleCreateEmbeddingHelper(clsid, outer, EMBDHLP_INPROC_HANDLER | EMBDHLP_CREATENOW, NULL, riid, out);
}
static HRESULT defhandler_create(IUnknown *outer, REFIID riid, void **out) { return OleCreateDefaultHandler(&g_guid_null, outer, riid, out); }

/* ---- OleLoad and the OleCreate family ----------------------------------------------------------------------------------------------------------------------------- */

typedef struct CreateOpts {
  DWORD flags, renderopt;
  ULONG cformats;
  const FORMATETC *formats;
  const DWORD *advf;
  IAdviseSink *sink;
  DWORD *conns;
} CreateOpts;

/* the object of a class: the server in this process, else the handler (the default handler stands in for a server that cannot be reached) */
static HRESULT create_server_object(REFCLSID clsid, IUnknown **unk) {
  HRESULT hr = CoCreateInstance(clsid, NULL, CLSCTX_INPROC_SERVER | CLSCTX_INPROC_HANDLER | CLSCTX_LOCAL_SERVER, &IID_IUnknown, (void **)unk);
  if (SUCCEEDED(hr)) return hr;
  *unk = NULL;
  return OleCreateDefaultHandler(clsid, NULL, &IID_IUnknown, (void **)unk);
}

/* the container site is set before the object is initialized when the object asks for that */
static DWORD site_misc_status(IOleObject *oo) {
  DWORD status = 0;
  if (FAILED(IOleObject_GetMiscStatus(oo, DVASPECT_CONTENT, &status))) status = 0;
  return status;
}
static HRESULT init_stage(IUnknown *unk, IOleClientSite *site, IStorage *stg, BOOL is_new) {
  IOleObject *oo = NULL;
  BOOL site_set = FALSE;
  HRESULT hr = S_OK;
  if (site && SUCCEEDED(IUnknown_QueryInterface(unk, &IID_IOleObject, (void **)&oo))) {
    if (site_misc_status(oo) & OLEMISC_SETCLIENTSITEFIRST) {
      hr = IOleObject_SetClientSite(oo, site);
      site_set = TRUE;
    }
  }
  if (SUCCEEDED(hr)) {
    IPersistStorage *ps;
    hr = IUnknown_QueryInterface(unk, &IID_IPersistStorage, (void **)&ps);
    if (SUCCEEDED(hr)) {
      hr = is_new ? IPersistStorage_InitNew(ps, stg) : IPersistStorage_Load(ps, stg);
      IPersistStorage_Release(ps);
    }
  }
  if (SUCCEEDED(hr) && oo && !site_set) hr = IOleObject_SetClientSite(oo, site);
  if (oo) IOleObject_Release(oo);
  return hr;
}

/* the presentation: run the object and cache what was asked for */
static HRESULT render_stage(IUnknown *unk, const CreateOpts *o) {
  FORMATETC dflt = make_fmt(0, TYMED_NULL);
  const FORMATETC *fmts = o->formats;
  ULONG n = o->cformats;
  IOleCache *cache;
  HRESULT hr;
  DWORD prime = ADVF_PRIMEFIRST;
  if (o->renderopt != OLERENDER_DRAW && o->renderopt != OLERENDER_FORMAT) return S_OK;
  if (o->renderopt == OLERENDER_DRAW && !n) { fmts = &dflt; n = 1; }
  hr = OleRun(unk);
  if (FAILED(hr)) return hr;
  if (FAILED(IUnknown_QueryInterface(unk, &IID_IOleCache, (void **)&cache))) return S_OK;
  for (ULONG i = 0; i < n && SUCCEEDED(hr); i++) {
    FORMATETC f = fmts[i];
    DWORD conn = 0;
    hr = IOleCache_Cache(cache, &f, o->advf ? o->advf[i] : prime, &conn);
    if (SUCCEEDED(hr) && o->conns && !o->sink) o->conns[i] = conn;
  }
  IOleCache_Release(cache);
  if (SUCCEEDED(hr) && o->sink && n) {
    IDataObject *d;
    if (SUCCEEDED(IUnknown_QueryInterface(unk, &IID_IDataObject, (void **)&d))) {
      for (ULONG i = 0; i < n && SUCCEEDED(hr); i++) {
        DWORD conn = 0;
        FORMATETC f = fmts[i];
        hr = IDataObject_DAdvise(d, &f, o->advf ? o->advf[i] : prime, o->sink, &conn);
        if (SUCCEEDED(hr) && o->conns) o->conns[i] = conn;
      }
      IDataObject_Release(d);
    }
  }
  return FAILED(hr) ? hr : S_OK;
}

static HRESULT create_check(const CreateOpts *o, IStorage *stg, void **out) {
  if (!out) return E_INVALIDARG;
  *out = NULL;
  if (!stg) return E_INVALIDARG;
  if (o->renderopt > OLERENDER_ASIS) return E_INVALIDARG;
  if (o->renderopt == OLERENDER_FORMAT && (!o->cformats || !o->formats)) return E_INVALIDARG;
  if (o->cformats && (!o->formats || o->cformats > 0x10000)) return E_INVALIDARG;
  if (o->cformats && o->sink && !o->conns) return E_INVALIDARG;
  return S_OK;
}
static void opts_simple(CreateOpts *o, DWORD renderopt, const FORMATETC *fmt) {
  mem_zero(o, sizeof(*o));
  o->renderopt = renderopt;
  if (renderopt == OLERENDER_FORMAT) { o->formats = fmt; o->cformats = fmt ? 1 : 0; }
}

/* finishes a new object: initialize, (load from a file), present, hand out the interface; takes the reference to `unk` */
static HRESULT create_finish(IUnknown *unk, IOleClientSite *site, IStorage *stg, const CreateOpts *o, LPCOLESTR file, REFIID riid, void **out) {
  HRESULT hr = init_stage(unk, site, stg, TRUE);
  if (SUCCEEDED(hr) && file) {
    IPersistFile *pf;
    hr = IUnknown_QueryInterface(unk, &IID_IPersistFile, (void **)&pf);
    if (SUCCEEDED(hr)) { hr = IPersistFile_Load(pf, file, STGM_READ | STGM_SHARE_DENY_WRITE); IPersistFile_Release(pf); }
  }
  if (SUCCEEDED(hr)) hr = render_stage(unk, o);
  if (SUCCEEDED(hr)) hr = IUnknown_QueryInterface(unk, riid, out);
  IUnknown_Release(unk);
  return hr;
}

EXPORT HRESULT WINAPI OleCreateEx(REFCLSID clsid, REFIID riid, DWORD flags, DWORD renderopt, ULONG cformats, DWORD *advf, LPFORMATETC formats, IAdviseSink *sink, DWORD *conns,
    LPOLECLIENTSITE site, LPSTORAGE stg, LPVOID *out) {
  CreateOpts o = {flags, renderopt, cformats, formats, advf, sink, conns};
  IUnknown *unk;
  HRESULT hr = create_check(&o, stg, out);
  if (FAILED(hr)) return hr;
  if (!clsid || !riid) return E_INVALIDARG;
  hr = create_server_object(clsid, &unk);
  if (FAILED(hr)) return hr;
  return create_finish(unk, site, stg, &o, NULL, riid, out);
}
EXPORT HRESULT WINAPI OleCreate(REFCLSID clsid, REFIID riid, DWORD renderopt, LPFORMATETC fmt, LPOLECLIENTSITE site, LPSTORAGE stg, LPVOID *out) {
  CreateOpts o;
  HRESULT hr;
  IUnknown *unk;
  opts_simple(&o, renderopt, fmt);
  hr = create_check(&o, stg, out);
  if (FAILED(hr)) return hr;
  if (!clsid || !riid) return E_INVALIDARG;
  hr = create_server_object(clsid, &unk);
  if (FAILED(hr)) return hr;
  return create_finish(unk, site, stg, &o, NULL, riid, out);
}

/* loads the object of a storage, for OleLoad and the creation from data; returns it as IUnknown */
static HRESULT load_object(IStorage *stg, IOleClientSite *site, IUnknown **unk) {
  CLSID clsid;
  IUnknown *obj;
  IOleObject *oo = NULL;
  BOOL site_set = FALSE;
  HRESULT hr = ReadClassStg(stg, &clsid);
  *unk = NULL;
  if (FAILED(hr)) return hr;
  hr = create_server_object(&clsid, &obj);
  if (FAILED(hr)) return hr;
  if (site && SUCCEEDED(IUnknown_QueryInterface(obj, &IID_IOleObject, (void **)&oo))) {
    if (site_misc_status(oo) & OLEMISC_SETCLIENTSITEFIRST) { hr = IOleObject_SetClientSite(oo, site); site_set = TRUE; }
  }
  if (SUCCEEDED(hr)) {
    IPersistStorage *ps;
    hr = IUnknown_QueryInterface(obj, &IID_IPersistStorage, (void **)&ps);
    if (SUCCEEDED(hr)) { hr = IPersistStorage_Load(ps, stg); IPersistStorage_Release(ps); }
  }
  if (SUCCEEDED(hr) && oo && !site_set) hr = IOleObject_SetClientSite(oo, site);
  if (SUCCEEDED(hr) && oo && (site_misc_status(oo) & OLEMISC_ALWAYSRUN)) hr = OleRun(obj);
  if (oo) IOleObject_Release(oo);
  if (FAILED(hr)) { IUnknown_Release(obj); return hr; }
  *unk = obj;
  return S_OK;
}
EXPORT HRESULT WINAPI OleLoad(LPSTORAGE stg, REFIID riid, LPOLECLIENTSITE site, LPVOID *out) {
  IUnknown *obj;
  HRESULT hr;
  if (!out) return E_INVALIDARG;
  *out = NULL;
  if (!stg || !riid) return E_INVALIDARG;
  hr = load_object(stg, site, &obj);
  if (FAILED(hr)) return hr;
  hr = IUnknown_QueryInterface(obj, riid, out);
  IUnknown_Release(obj);
  return hr;
}

/* -- from files -- */

EXPORT HRESULT WINAPI OleCreateFromFileEx(REFCLSID rclsid, LPCOLESTR file, REFIID riid, DWORD flags, DWORD renderopt, ULONG cformats, DWORD *advf, LPFORMATETC formats,
    IAdviseSink *sink, DWORD *conns, LPOLECLIENTSITE site, LPSTORAGE stg, LPVOID *out) {
  CreateOpts o = {flags, renderopt, cformats, formats, advf, sink, conns};
  CLSID clsid = rclsid ? *rclsid : g_guid_null;
  IUnknown *unk;
  HRESULT hr = create_check(&o, stg, out);
  if (FAILED(hr)) return hr;
  if (!file || !riid) return E_INVALIDARG;
  if (IsEqualGUID(&clsid, &g_guid_null) && FAILED(GetClassFile(file, &clsid))) clsid = g_clsid_package;
  hr = create_server_object(&clsid, &unk);
  if (FAILED(hr)) return hr;
  return create_finish(unk, site, stg, &o, file, riid, out);
}
EXPORT HRESULT WINAPI OleCreateFromFile(REFCLSID clsid, LPCOLESTR file, REFIID riid, DWORD renderopt, LPFORMATETC fmt, LPOLECLIENTSITE site, LPSTORAGE stg, LPVOID *out) {
  CreateOpts o;
  opts_simple(&o, renderopt, fmt);
  return OleCreateFromFileEx(clsid, file, riid, 0, renderopt, o.cformats, NULL, (LPFORMATETC)o.formats, NULL, NULL, site, stg, out);
}

/* -- links -- */

EXPORT HRESULT WINAPI OleCreateLinkEx(LPMONIKER mk, REFIID riid, DWORD flags, DWORD renderopt, ULONG cformats, DWORD *advf, LPFORMATETC formats, IAdviseSink *sink, DWORD *conns,
    LPOLECLIENTSITE site, LPSTORAGE stg, LPVOID *out) {
  CreateOpts o = {flags, renderopt, cformats, formats, advf, sink, conns};
  IUnknown *unk;
  IOleLink *link;
  HRESULT hr = create_check(&o, stg, out);
  if (FAILED(hr)) return hr;
  if (!mk || !riid) return E_INVALIDARG;
  hr = CoCreateInstance(&g_clsid_std_ole_link, NULL, CLSCTX_INPROC_SERVER | CLSCTX_INPROC_HANDLER, &IID_IUnknown, (void **)&unk);
  if (FAILED(hr)) return hr;
  hr = IUnknown_QueryInterface(unk, &IID_IOleLink, (void **)&link);
  if (SUCCEEDED(hr)) { hr = IOleLink_SetSourceMoniker(link, mk, &g_guid_null); IOleLink_Release(link); }
  if (FAILED(hr)) { IUnknown_Release(unk); return hr; }
  return create_finish(unk, site, stg, &o, NULL, riid, out);
}
EXPORT HRESULT WINAPI OleCreateLink(LPMONIKER mk, REFIID riid, DWORD renderopt, LPFORMATETC fmt, LPOLECLIENTSITE site, LPSTORAGE stg, LPVOID *out) {
  CreateOpts o;
  opts_simple(&o, renderopt, fmt);
  return OleCreateLinkEx(mk, riid, 0, renderopt, o.cformats, NULL, (LPFORMATETC)o.formats, NULL, NULL, site, stg, out);
}
EXPORT HRESULT WINAPI OleCreateLinkToFileEx(LPCOLESTR file, REFIID riid, DWORD flags, DWORD renderopt, ULONG cformats, DWORD *advf, LPFORMATETC formats, IAdviseSink *sink,
    DWORD *conns, LPOLECLIENTSITE site, LPSTORAGE stg, LPVOID *out) {
  IMoniker *mk;
  HRESULT hr;
  if (out) *out = NULL;
  if (!file) return E_INVALIDARG;
  hr = CreateFileMoniker(file, &mk);
  if (FAILED(hr)) return hr;
  hr = OleCreateLinkEx(mk, riid, flags, renderopt, cformats, advf, formats, sink, conns, site, stg, out);
  IMoniker_Release(mk);
  return hr;
}
EXPORT HRESULT WINAPI OleCreateLinkToFile(LPCOLESTR file, REFIID riid, DWORD renderopt, LPFORMATETC fmt, LPOLECLIENTSITE site, LPSTORAGE stg, LPVOID *out) {
  CreateOpts o;
  opts_simple(&o, renderopt, fmt);
  return OleCreateLinkToFileEx(file, riid, 0, renderopt, o.cformats, NULL, (LPFORMATETC)o.formats, NULL, NULL, site, stg, out);
}

/* -- from data objects -- */

/* the text of an HGLOBAL of a FileName / FileNameW format as a new wide string (heap), NULL on failure */
static WCHAR *file_name_from_medium(STGMEDIUM *med, BOOL wide) {
  WCHAR *result = NULL;
  SIZE_T size;
  BYTE *p;
  if (med->tymed != TYMED_HGLOBAL || !med->hGlobal) return NULL;
  size = GlobalSize(med->hGlobal);
  p = GlobalLock(med->hGlobal);
  if (!p) return NULL;
  if (wide) {
    SIZE_T n = size / sizeof(WCHAR), len = 0;
    while (len < n && ((WCHAR *)p)[len]) len++;
    if (len < n && len < 32768) { result = mem_alloc((len + 1) * sizeof(WCHAR)); if (result) mem_copy(result, p, len * sizeof(WCHAR)); }
  } else {
    SIZE_T len = 0;
    while (len < size && p[len]) len++;
    if (len < size && len < 32768) {
      result = mem_alloc((len + 1) * sizeof(WCHAR));
      if (result && len && !MultiByteToWideChar(CP_ACP, 0, (const char *)p, (int)len, result, (int)len)) { mem_free(result); result = NULL; }
    }
  }
  GlobalUnlock(med->hGlobal);
  return result;
}
static WCHAR *data_file_name(IDataObject *d) {
  for (int pass = 0; pass < 2; pass++) {
    FORMATETC f = make_fmt(ole_clipformat_a(pass ? "FileName" : "FileNameW"), TYMED_HGLOBAL);
    STGMEDIUM med;
    if (IDataObject_GetData(d, &f, &med) == S_OK) {
      WCHAR *name = file_name_from_medium(&med, pass == 0);
      ReleaseStgMedium(&med);
      if (name) return name;
    }
  }
  return NULL;
}

EXPORT HRESULT WINAPI OleCreateFromDataEx(LPDATAOBJECT data, REFIID riid, DWORD flags, DWORD renderopt, ULONG cformats, DWORD *advf, LPFORMATETC formats, IAdviseSink *sink,
    DWORD *conns, LPOLECLIENTSITE site, LPSTORAGE stg, LPVOID *out) {
  CreateOpts o = {flags, renderopt, cformats, formats, advf, sink, conns};
  HRESULT hr = create_check(&o, stg, out);
  if (FAILED(hr)) return hr;
  if (!data || !riid) return E_INVALIDARG;
  for (int pass = 0; pass < 2; pass++) {       /* an embedded object, or a copy of an embedded source */
    FORMATETC f = make_fmt(ole_clipformat_a(pass ? "Embed Source" : "Embedded Object"), TYMED_ISTORAGE);
    STGMEDIUM med;
    IUnknown *obj;
    if (!data_has(data, f.cfFormat, TYMED_ISTORAGE)) continue;
    mem_zero(&med, sizeof(med));
    med.tymed = TYMED_ISTORAGE;
    med.pstg = stg;
    hr = IDataObject_GetDataHere(data, &f, &med);
    if (FAILED(hr)) {
      mem_zero(&med, sizeof(med));
      hr = IDataObject_GetData(data, &f, &med);
      if (hr != S_OK) continue;
      if (med.tymed == TYMED_ISTORAGE && med.pstg) hr = IStorage_CopyTo(med.pstg, 0, NULL, NULL, stg);
      else hr = DV_E_TYMED;
      ReleaseStgMedium(&med);
      if (FAILED(hr)) continue;
    }
    hr = load_object(stg, site, &obj);
    if (FAILED(hr)) return hr;
    hr = render_stage(obj, &o);
    if (SUCCEEDED(hr)) hr = IUnknown_QueryInterface(obj, riid, out);
    IUnknown_Release(obj);
    return hr;
  }
  {
    WCHAR *name = data_file_name(data);
    if (name) {
      hr = OleCreateFromFileEx(&g_guid_null, name, riid, flags, renderopt, cformats, advf, formats, sink, conns, site, stg, out);
      mem_free(name);
      return hr;
    }
  }
  return DV_E_FORMATETC;
}
EXPORT HRESULT WINAPI OleCreateFromData(LPDATAOBJECT data, REFIID riid, DWORD renderopt, LPFORMATETC fmt, LPOLECLIENTSITE site, LPSTORAGE stg, LPVOID *out) {
  CreateOpts o;
  opts_simple(&o, renderopt, fmt);
  return OleCreateFromDataEx(data, riid, 0, renderopt, o.cformats, NULL, (LPFORMATETC)o.formats, NULL, NULL, site, stg, out);
}

EXPORT HRESULT WINAPI OleCreateLinkFromDataEx(LPDATAOBJECT data, REFIID riid, DWORD flags, DWORD renderopt, ULONG cformats, DWORD *advf, LPFORMATETC formats, IAdviseSink *sink,
    DWORD *conns, LPOLECLIENTSITE site, LPSTORAGE stg, LPVOID *out) {
  CreateOpts o = {flags, renderopt, cformats, formats, advf, sink, conns};
  FORMATETC f = make_fmt(ole_clipformat_a("Link Source"), TYMED_ISTREAM);
  HRESULT hr = create_check(&o, stg, out);
  if (FAILED(hr)) return hr;
  if (!data || !riid) return E_INVALIDARG;
  if (data_has(data, f.cfFormat, TYMED_ISTREAM)) {
    STGMEDIUM med;
    mem_zero(&med, sizeof(med));
    hr = IDataObject_GetData(data, &f, &med);
    if (hr == S_OK) {
      IMoniker *mk = NULL;
      if (med.tymed == TYMED_ISTREAM && med.pstm) hr = OleLoadFromStream(med.pstm, &IID_IMoniker, (void **)&mk);
      else hr = DV_E_TYMED;
      ReleaseStgMedium(&med);
      if (FAILED(hr)) return hr;
      hr = OleCreateLinkEx(mk, riid, flags, renderopt, cformats, advf, formats, sink, conns, site, stg, out);
      IMoniker_Release(mk);
      return hr;
    }
  }
  {
    WCHAR *name = data_file_name(data);
    if (name) {
      hr = OleCreateLinkToFileEx(name, riid, flags, renderopt, cformats, advf, formats, sink, conns, site, stg, out);
      mem_free(name);
      return hr;
    }
  }
  return DV_E_FORMATETC;
}
EXPORT HRESULT WINAPI OleCreateLinkFromData(LPDATAOBJECT data, REFIID riid, DWORD renderopt, LPFORMATETC fmt, LPOLECLIENTSITE site, LPSTORAGE stg, LPVOID *out) {
  CreateOpts o;
  opts_simple(&o, renderopt, fmt);
  return OleCreateLinkFromDataEx(data, riid, 0, renderopt, o.cformats, NULL, (LPFORMATETC)o.formats, NULL, NULL, site, stg, out);
}

/* a picture object (a bitmap, a metafile): the data goes into the cache of a handler of one of the picture classes */
EXPORT HRESULT WINAPI OleCreateStaticFromData(LPDATAOBJECT data, REFIID riid, DWORD renderopt, LPFORMATETC fmt, LPOLECLIENTSITE site, LPSTORAGE stg, LPVOID *out) {
  FORMATETC f;
  STGMEDIUM med;
  const GUID *clsid;
  const char *type_name;
  IUnknown *unk;
  IPersistStorage *ps;
  IOleCache *cache;
  HRESULT hr;
  if (out) *out = NULL;
  if (!out || !stg || !data || !riid) return E_INVALIDARG;
  if (renderopt > OLERENDER_ASIS) return E_INVALIDARG;
  if (!fmt) {
    static const struct { UINT cf; DWORD tymed; } cands[] = {{CF_DIB, TYMED_HGLOBAL}, {CF_ENHMETAFILE, TYMED_ENHMF}, {CF_METAFILEPICT, TYMED_MFPICT}};
    f = make_fmt(0, TYMED_NULL);
    for (int i = 0; i < 3; i++) { FORMATETC c = make_fmt(cands[i].cf, cands[i].tymed); if (IDataObject_QueryGetData(data, &c) == S_OK) { f = c; break; } }
  } else f = *fmt;
  switch (f.cfFormat) {
    case CF_DIB: clsid = &g_clsid_picture_dib; type_name = "Device Independent Bitmap"; break;
    case CF_ENHMETAFILE: clsid = &g_clsid_picture_enhmetafile; type_name = "Picture (Enhanced Metafile)"; break;
    case CF_METAFILEPICT: clsid = &g_clsid_picture_metafile; type_name = "Picture (Metafile)"; break;
    default: return DV_E_CLIPFORMAT;
  }
  mem_zero(&med, sizeof(med));
  hr = IDataObject_GetData(data, &f, &med);
  if (hr != S_OK) return FAILED(hr) ? hr : DV_E_FORMATETC;
  hr = OleCreateDefaultHandler(clsid, NULL, &IID_IUnknown, (void **)&unk);
  if (FAILED(hr)) { ReleaseStgMedium(&med); return hr; }
  hr = IUnknown_QueryInterface(unk, &IID_IPersistStorage, (void **)&ps);
  if (SUCCEEDED(hr)) {
    hr = IPersistStorage_InitNew(ps, stg);
    if (SUCCEEDED(hr)) {
      WCHAR wname[40];
      ascii_to_wide(wname, type_name);
      WriteFmtUserTypeStg(stg, (CLIPFORMAT)f.cfFormat, wname);
      hr = IUnknown_QueryInterface(unk, &IID_IOleCache, (void **)&cache);
      if (SUCCEEDED(hr)) {
        hr = IOleCache_SetData(cache, &f, &med, TRUE);       /* (takes the medium) */
        IOleCache_Release(cache);
        mem_zero(&med, sizeof(med));
      }
      if (SUCCEEDED(hr)) hr = IPersistStorage_Save(ps, stg, TRUE);
      if (SUCCEEDED(hr)) hr = IPersistStorage_SaveCompleted(ps, NULL);
    }
    IPersistStorage_Release(ps);
  }
  if (med.tymed != TYMED_NULL) ReleaseStgMedium(&med);
  if (SUCCEEDED(hr) && site) {
    IOleObject *oo;
    if (SUCCEEDED(IUnknown_QueryInterface(unk, &IID_IOleObject, (void **)&oo))) { IOleObject_SetClientSite(oo, site); IOleObject_Release(oo); }
  }
  if (SUCCEEDED(hr)) hr = IUnknown_QueryInterface(unk, riid, out);
  IUnknown_Release(unk);
  return hr;
}

/* ---- the OLE 1.0 stream converters ---------------------------------------------------------------------------------------------------------------------------------- */
/* An OLE 1.0 object stream: version 0x501, a format id (1 linked, 2 embedded, 3 static, 0 nothing), then three length-prefixed ANSI strings (class, topic, item); an embedded
 * object continues with the size and the bytes of its native data. After it comes the presentation: version, format id (0 none), the class (the clipboard format name:
 * METAFILEPICT, BITMAP, DIB or a registered name), the topic, the width and height, 8 bytes of attributes, the size and the bytes of the data. The storage of an embedded object
 * holds the native data in \1Ole10Native (a size and the bytes) and the presentation in \2OlePres000. Only embedded objects are converted; a linked or static object returns
 * CONVERT10_E_OLESTREAM_FMT. */

#define OLE1_VERSION 0x501
#define OLE1_MAX_STRING 4096
#define OLE1_MAX_DATA 0x10000000

typedef struct Ole1Pres { BOOL present; UINT cf; DWORD width, height; DWORD size; BYTE *data; } Ole1Pres;

static HRESULT ols_get(LPOLESTREAM s, void *buf, DWORD n) {
  if (!n) return S_OK;
  if (!s || !s->lpstbl || !s->lpstbl->Get) return CONVERT10_E_OLESTREAM_GET;
  return s->lpstbl->Get(s, buf, n) == n ? S_OK : CONVERT10_E_OLESTREAM_GET;
}
static HRESULT ols_put(LPOLESTREAM s, const void *buf, DWORD n) {
  if (!n) return S_OK;
  if (!s || !s->lpstbl || !s->lpstbl->Put) return CONVERT10_E_OLESTREAM_PUT;
  return s->lpstbl->Put(s, buf, n) == n ? S_OK : CONVERT10_E_OLESTREAM_PUT;
}
static HRESULT ols_get_dword(LPOLESTREAM s, DWORD *v) { return ols_get(s, v, 4); }
static HRESULT ols_put_dword(LPOLESTREAM s, DWORD v) { return ols_put(s, &v, 4); }
/* a length-prefixed string; the text (with its end mark) is in *out when `out` is given (heap) */
static HRESULT ols_get_string(LPOLESTREAM s, char **out) {
  DWORD n;
  char *text;
  HRESULT hr = ols_get_dword(s, &n);
  if (out) *out = NULL;
  if (FAILED(hr)) return hr;
  if (n > OLE1_MAX_STRING) return CONVERT10_E_OLESTREAM_FMT;
  if (!n) return S_OK;
  text = mem_alloc(n + 1);
  if (!text) return E_OUTOFMEMORY;
  hr = ols_get(s, text, n);
  if (FAILED(hr)) { mem_free(text); return hr; }
  text[n] = 0;
  if (out) *out = text; else mem_free(text);
  return S_OK;
}
static HRESULT ols_put_string(LPOLESTREAM s, const char *text) {
  DWORD n = 0;
  HRESULT hr;
  if (text) { while (text[n]) n++; n++; }
  hr = ols_put_dword(s, n);
  return SUCCEEDED(hr) ? ols_put(s, text, n) : hr;
}
static BYTE *ols_get_blob(LPOLESTREAM s, DWORD n, HRESULT *hr) {
  BYTE *buf;
  *hr = S_OK;
  if (n > OLE1_MAX_DATA) { *hr = CONVERT10_E_OLESTREAM_FMT; return NULL; }
  buf = mem_alloc(n);
  if (!buf) { *hr = E_OUTOFMEMORY; return NULL; }
  *hr = ols_get(s, buf, n);
  if (FAILED(*hr)) { mem_free(buf); return NULL; }
  return buf;
}
static void ascii_to_wide_n(WCHAR *d, const char *s, SIZE_T cap) { SIZE_T i = 0; for (; i + 1 < cap && s[i]; i++) d[i] = (WCHAR)(BYTE)s[i]; d[i] = 0; }
static UINT ole1_class_to_cf(const char *name) {
  if (!name) return 0;
  {
    WCHAR w[64];
    ascii_to_wide_n(w, name, 64);
    if (cf_name_eq("METAFILEPICT", w)) return CF_METAFILEPICT;
    if (cf_name_eq("BITMAP", w)) return CF_BITMAP;
    if (cf_name_eq("DIB", w)) return CF_DIB;
    return ole_clipformat(w);
  }
}
static const char *ole1_cf_to_class(UINT cf) {
  switch (cf) {
    case CF_METAFILEPICT: return "METAFILEPICT";
    case CF_BITMAP: return "BITMAP";
    case CF_DIB: return "DIB";
    default: return ole_clipformat_static_name(cf);
  }
}

static HRESULT put_storage_stream(IStorage *stg, const WCHAR *name, const void *a, DWORD an, const void *b, DWORD bn) {
  IStream *stm;
  HRESULT hr = IStorage_CreateStream(stg, name, STGM_CREATE | STGM_WRITE | STGM_SHARE_EXCLUSIVE, 0, 0, &stm);
  if (FAILED(hr)) return hr;
  hr = stream_write_all(stm, a, an);
  if (SUCCEEDED(hr)) hr = stream_write_all(stm, b, bn);
  IStream_Release(stm);
  return hr;
}

/* reads the presentation object; its data in pres->data (heap) */
static HRESULT ole1_read_presentation(LPOLESTREAM s, Ole1Pres *pres) {
  DWORD version, fmt, attrs[2];
  char *cls = NULL;
  HRESULT hr;
  mem_zero(pres, sizeof(*pres));
  hr = ols_get_dword(s, &version);
  if (SUCCEEDED(hr)) hr = ols_get_dword(s, &fmt);
  if (FAILED(hr)) return hr;
  if (!fmt) return S_OK;
  hr = ols_get_string(s, &cls);
  if (SUCCEEDED(hr)) { pres->cf = ole1_class_to_cf(cls); hr = ols_get_string(s, NULL); }
  mem_free(cls);
  if (SUCCEEDED(hr)) hr = ols_get_dword(s, &pres->width);
  if (SUCCEEDED(hr)) hr = ols_get_dword(s, &pres->height);
  if (SUCCEEDED(hr)) hr = ols_get(s, attrs, 8);
  if (SUCCEEDED(hr)) hr = ols_get_dword(s, &pres->size);
  if (SUCCEEDED(hr)) pres->data = ols_get_blob(s, pres->size, &hr);
  if (FAILED(hr)) { mem_free(pres->data); pres->data = NULL; return hr; }
  pres->present = TRUE;
  return S_OK;
}

static HRESULT ole1_write_presentation(LPOLESTREAM s, UINT cf, DWORD width, DWORD height, const void *data, DWORD size) {
  const char *cls = ole1_cf_to_class(cf);
  DWORD attrs[2] = {0, 0};
  HRESULT hr;
  if (!cls || !data) {
    hr = ols_put_dword(s, OLE1_VERSION);
    return SUCCEEDED(hr) ? ols_put_dword(s, 0) : hr;
  }
  hr = ols_put_dword(s, OLE1_VERSION);
  if (SUCCEEDED(hr)) hr = ols_put_dword(s, 5);
  if (SUCCEEDED(hr)) hr = ols_put_string(s, cls);
  if (SUCCEEDED(hr)) hr = ols_put_string(s, NULL);
  if (SUCCEEDED(hr)) hr = ols_put_dword(s, width);
  if (SUCCEEDED(hr)) hr = ols_put_dword(s, height);
  if (SUCCEEDED(hr)) hr = ols_put(s, attrs, 8);
  if (SUCCEEDED(hr)) hr = ols_put_dword(s, size);
  if (SUCCEEDED(hr)) hr = ols_put(s, data, size);
  return hr;
}

static HRESULT ole1_to_storage(LPOLESTREAM s, IStorage *stg, Ole1Pres *pres) {
  DWORD version, fmt, native_size;
  char *cls = NULL;
  BYTE *native = NULL;
  CLSID clsid;
  WCHAR wname[128];
  HRESULT hr;
  mem_zero(pres, sizeof(*pres));
  if (!s || !stg) return E_INVALIDARG;
  hr = ols_get_dword(s, &version);
  if (SUCCEEDED(hr)) hr = ols_get_dword(s, &fmt);
  if (FAILED(hr)) return hr;
  if (!fmt) return S_OK;
  if (fmt != 2) return CONVERT10_E_OLESTREAM_FMT;
  hr = ols_get_string(s, &cls);
  if (SUCCEEDED(hr)) hr = ols_get_string(s, NULL);
  if (SUCCEEDED(hr)) hr = ols_get_string(s, NULL);
  if (SUCCEEDED(hr)) hr = ols_get_dword(s, &native_size);
  if (SUCCEEDED(hr)) native = ols_get_blob(s, native_size, &hr);
  if (FAILED(hr)) goto done;
  if (!cls) { hr = CONVERT10_E_OLESTREAM_FMT; goto done; }
  ascii_to_wide_n(wname, cls, 128);
  hr = CLSIDFromOle1Class(wname, &clsid, FALSE);
  if (FAILED(hr)) hr = CLSIDFromProgID(wname, &clsid);
  if (FAILED(hr)) goto done;
  hr = WriteClassStg(stg, &clsid);
  if (SUCCEEDED(hr)) {
    LPOLESTR type = NULL;
    if (FAILED(OleRegGetUserType(&clsid, USERCLASSTYPE_FULL, &type))) type = cotask_wdup(wname);
    hr = WriteFmtUserTypeStg(stg, 0, type);
    if (type) CoTaskMemFree(type);
  }
  if (SUCCEEDED(hr)) hr = WriteOleStg(stg, NULL, 0, NULL);
  if (SUCCEEDED(hr)) hr = put_storage_stream(stg, W(1, 'O', 'l', 'e', '1', '0', 'N', 'a', 't', 'i', 'v', 'e'), &native_size, 4, native, native_size);
  if (SUCCEEDED(hr)) hr = ole1_read_presentation(s, pres);
done:
  mem_free(cls);
  mem_free(native);
  return hr;
}
static HRESULT ole1_write_presentation_stream(IStorage *stg, const Ole1Pres *p) {
  /* \2OlePres000: 0xffffffff, format, size of the target device (4), aspect, index, advise flags, 0, extent x, extent y, data size, data */
  DWORD hdr[11] = {0xffffffff, p->cf, 4, DVASPECT_CONTENT, (DWORD)-1, 0, 0, p->width, p->height, p->size, 0};
  return put_storage_stream(stg, W(2, 'O', 'l', 'e', 'P', 'r', 'e', 's', '0', '0', '0'), hdr, 40, p->data, p->size);
}
static HRESULT convert_ole1_to_storage(LPOLESTREAM s, IStorage *stg, CLIPFORMAT *pcf, LONG *pw, LONG *ph, DWORD *psize, STGMEDIUM *med, BOOL want_medium) {
  Ole1Pres pres;
  HRESULT hr = ole1_to_storage(s, stg, &pres);
  if (FAILED(hr)) return hr;
  if (pres.present) {
    if (pcf) *pcf = (CLIPFORMAT)pres.cf;
    if (pw) *pw = (LONG)pres.width;
    if (ph) *ph = (LONG)pres.height;
    if (psize) *psize = pres.size;
    if (want_medium && med) {
      HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, pres.size ? pres.size : 1);
      BYTE *p = g ? GlobalLock(g) : NULL;
      if (!p) { if (g) GlobalFree(g); hr = E_OUTOFMEMORY; }
      else {
        mem_copy(p, pres.data, pres.size);
        GlobalUnlock(g);
        med->tymed = TYMED_HGLOBAL; med->hGlobal = g; med->pUnkForRelease = NULL;
      }
    } else if (pres.cf) hr = ole1_write_presentation_stream(stg, &pres);
  } else {
    if (pcf) *pcf = 0;
    if (psize) *psize = 0;
  }
  mem_free(pres.data);
  return hr;
}
EXPORT HRESULT WINAPI OleConvertOLESTREAMToIStorage(LPOLESTREAM s, LPSTORAGE stg, const DVTARGETDEVICE *ptd) {
  (void)ptd;
  return convert_ole1_to_storage(s, stg, NULL, NULL, NULL, NULL, NULL, FALSE);
}
EXPORT HRESULT WINAPI OleConvertOLESTREAMToIStorage2(LPOLESTREAM s, LPSTORAGE stg, const DVTARGETDEVICE *ptd, DWORD opt, void *context, void *callback) {
  (void)opt; (void)context; (void)callback;
  return OleConvertOLESTREAMToIStorage(s, stg, ptd);
}
EXPORT HRESULT WINAPI OleConvertOLESTREAMToIStorageEx(LPOLESTREAM s, LPSTORAGE stg, CLIPFORMAT *pcf, LONG *pw, LONG *ph, DWORD *psize, LPSTGMEDIUM med) {
  return convert_ole1_to_storage(s, stg, pcf, pw, ph, psize, med, med != NULL);
}
EXPORT HRESULT WINAPI OleConvertOLESTREAMToIStorageEx2(LPOLESTREAM s, LPSTORAGE stg, CLIPFORMAT *pcf, LONG *pw, LONG *ph, DWORD *psize, LPSTGMEDIUM med, DWORD opt, void *context,
    void *callback) {
  (void)opt; (void)context; (void)callback;
  return OleConvertOLESTREAMToIStorageEx(s, stg, pcf, pw, ph, psize, med);
}

/* the other way: the native data and the presentation of a storage */
static HRESULT read_whole_stream(IStorage *stg, const WCHAR *name, BYTE **data, DWORD *size) {
  IStream *stm;
  STATSTG st;
  HRESULT hr = IStorage_OpenStream(stg, name, NULL, STGM_READ | STGM_SHARE_EXCLUSIVE, 0, &stm);
  *data = NULL; *size = 0;
  if (FAILED(hr)) return hr;
  hr = IStream_Stat(stm, &st, STATFLAG_NONAME);
  if (SUCCEEDED(hr) && (st.cbSize.QuadPart > OLE1_MAX_DATA)) hr = STG_E_INVALIDHEADER;
  if (SUCCEEDED(hr)) {
    BYTE *buf = mem_alloc((SIZE_T)st.cbSize.QuadPart);
    if (!buf) hr = E_OUTOFMEMORY;
    else {
      hr = stream_read_all(stm, buf, (ULONG)st.cbSize.QuadPart);
      if (SUCCEEDED(hr)) { *data = buf; *size = (DWORD)st.cbSize.QuadPart; } else mem_free(buf);
    }
  }
  IStream_Release(stm);
  return hr;
}
static HRESULT convert_storage_to_ole1(IStorage *stg, LPOLESTREAM s, BOOL have_pres, CLIPFORMAT cf, LONG width, LONG height, DWORD size, STGMEDIUM *med) {
  STATSTG st;
  BYTE *native = NULL, *pres = NULL;
  DWORD native_size = 0, pres_size = 0;
  LPOLESTR prog = NULL;
  char name[128];
  HRESULT hr;
  if (!stg || !s) return E_INVALIDARG;
  hr = IStorage_Stat(stg, &st, STATFLAG_NONAME);
  if (FAILED(hr)) return hr;
  hr = read_whole_stream(stg, W(1, 'O', 'l', 'e', '1', '0', 'N', 'a', 't', 'i', 'v', 'e'), &native, &native_size);
  if (FAILED(hr)) return hr == STG_E_FILENOTFOUND ? CONVERT10_E_STG_NO_STD_STREAM : hr;
  if (native_size < 4) { mem_free(native); return CONVERT10_E_STG_FMT; }
  hr = ProgIDFromCLSID(&st.clsid, &prog);
  if (FAILED(hr)) { mem_free(native); return hr; }
  {
    SIZE_T i = 0;
    for (; prog[i] && i < 127; i++) name[i] = prog[i] < 128 ? (char)prog[i] : '?';
    name[i] = 0;
    CoTaskMemFree(prog);
  }
  /* \1Ole10Native: a size, then the bytes */
  {
    DWORD n = *(DWORD *)native;
    if (n > native_size - 4) { mem_free(native); return CONVERT10_E_STG_FMT; }
    hr = ols_put_dword(s, OLE1_VERSION);
    if (SUCCEEDED(hr)) hr = ols_put_dword(s, 2);
    if (SUCCEEDED(hr)) hr = ols_put_string(s, name);
    if (SUCCEEDED(hr)) hr = ols_put_string(s, NULL);
    if (SUCCEEDED(hr)) hr = ols_put_string(s, NULL);
    if (SUCCEEDED(hr)) hr = ols_put_dword(s, n);
    if (SUCCEEDED(hr)) hr = ols_put(s, native + 4, n);
  }
  mem_free(native);
  if (FAILED(hr)) return hr;
  if (have_pres) {
    BYTE *src = NULL;
    BOOL owned = FALSE;
    if (med && med->tymed == TYMED_HGLOBAL && med->hGlobal && size) {
      src = GlobalLock(med->hGlobal);
      if (src && size <= GlobalSize(med->hGlobal)) hr = ole1_write_presentation(s, cf, (DWORD)width, (DWORD)height, src, size);
      else hr = ole1_write_presentation(s, 0, 0, 0, NULL, 0);
      if (src) GlobalUnlock(med->hGlobal);
    } else hr = ole1_write_presentation(s, 0, 0, 0, NULL, 0);
    (void)owned;
    return hr;
  }
  hr = read_whole_stream(stg, W(2, 'O', 'l', 'e', 'P', 'r', 'e', 's', '0', '0', '0'), &pres, &pres_size);
  if (SUCCEEDED(hr) && pres_size >= 40 && ((DWORD *)pres)[0] == 0xffffffff) {
    const DWORD *h = (const DWORD *)pres;
    DWORD data_size = h[9];
    if (data_size > pres_size - 40) hr = CONVERT10_E_STG_FMT;
    else hr = ole1_write_presentation(s, h[1], h[7], h[8], pres + 40, data_size);
  } else hr = ole1_write_presentation(s, 0, 0, 0, NULL, 0);
  mem_free(pres);
  return hr;
}
EXPORT HRESULT WINAPI OleConvertIStorageToOLESTREAM(LPSTORAGE stg, LPOLESTREAM s) { return convert_storage_to_ole1(stg, s, FALSE, 0, 0, 0, 0, NULL); }
EXPORT HRESULT WINAPI OleConvertIStorageToOLESTREAMEx(LPSTORAGE stg, CLIPFORMAT cf, LONG width, LONG height, DWORD size, LPSTGMEDIUM med, LPOLESTREAM s) {
  return convert_storage_to_ole1(stg, s, TRUE, cf, width, height, size, med);
}

/* ---- the progress indicator ---------------------------------------------------------------------------------------------------------------------------------------- */
/* There is no window to show it in: an IBindStatusCallback that passes everything on to the caller's callback. */

typedef struct ProgressObj { IBindStatusCallback iface; LONG refs; IBindStatusCallback *caller; } ProgressObj;
static const IBindStatusCallbackVtbl g_progress_vtbl;
static HRESULT WINAPI prog_qi(IBindStatusCallback *me, REFIID riid, void **out) {
  if (!out) return E_INVALIDARG;
  *out = NULL;
  if (IsEqualGUID(riid, &IID_IUnknown) || IsEqualGUID(riid, &IID_IBindStatusCallback)) { *out = me; IBindStatusCallback_AddRef(me); return S_OK; }
  return E_NOINTERFACE;
}
static ULONG WINAPI prog_addref(IBindStatusCallback *me) { return (ULONG)InterlockedIncrement(&((ProgressObj *)me)->refs); }
static ULONG WINAPI prog_release(IBindStatusCallback *me) {
  ProgressObj *p = (ProgressObj *)me;
  LONG r = InterlockedDecrement(&p->refs);
  if (r == 0) { if (p->caller) IBindStatusCallback_Release(p->caller); mem_free(p); }
  return (ULONG)r;
}
static HRESULT WINAPI prog_on_start(IBindStatusCallback *me, DWORD reserved, IBinding *binding) {
  ProgressObj *p = (ProgressObj *)me;
  return p->caller ? IBindStatusCallback_OnStartBinding(p->caller, reserved, binding) : S_OK;
}
static HRESULT WINAPI prog_get_priority(IBindStatusCallback *me, LONG *priority) {
  ProgressObj *p = (ProgressObj *)me;
  return p->caller ? IBindStatusCallback_GetPriority(p->caller, priority) : E_NOTIMPL;
}
static HRESULT WINAPI prog_on_low_resource(IBindStatusCallback *me, DWORD reserved) {
  ProgressObj *p = (ProgressObj *)me;
  return p->caller ? IBindStatusCallback_OnLowResource(p->caller, reserved) : S_OK;
}
static HRESULT WINAPI prog_on_progress(IBindStatusCallback *me, ULONG progress, ULONG max, ULONG code, LPCWSTR text) {
  ProgressObj *p = (ProgressObj *)me;
  return p->caller ? IBindStatusCallback_OnProgress(p->caller, progress, max, code, text) : S_OK;
}
static HRESULT WINAPI prog_on_stop(IBindStatusCallback *me, HRESULT hr, LPCWSTR error) {
  ProgressObj *p = (ProgressObj *)me;
  return p->caller ? IBindStatusCallback_OnStopBinding(p->caller, hr, error) : S_OK;
}
static HRESULT WINAPI prog_get_bind_info(IBindStatusCallback *me, DWORD *flags, BINDINFO *info) {
  ProgressObj *p = (ProgressObj *)me;
  return p->caller ? IBindStatusCallback_GetBindInfo(p->caller, flags, info) : E_NOTIMPL;
}
static HRESULT WINAPI prog_on_data(IBindStatusCallback *me, DWORD flags, DWORD size, FORMATETC *fmt, STGMEDIUM *med) {
  ProgressObj *p = (ProgressObj *)me;
  return p->caller ? IBindStatusCallback_OnDataAvailable(p->caller, flags, size, fmt, med) : S_OK;
}
static HRESULT WINAPI prog_on_object(IBindStatusCallback *me, REFIID riid, IUnknown *unk) {
  ProgressObj *p = (ProgressObj *)me;
  return p->caller ? IBindStatusCallback_OnObjectAvailable(p->caller, riid, unk) : S_OK;
}
static const IBindStatusCallbackVtbl g_progress_vtbl = {prog_qi, prog_addref, prog_release, prog_on_start, prog_get_priority, prog_on_low_resource, prog_on_progress, prog_on_stop,
    prog_get_bind_info, prog_on_data, prog_on_object};
EXPORT HRESULT WINAPI CreateStdProgressIndicator(HWND parent, LPCOLESTR title, IBindStatusCallback *caller, IBindStatusCallback **out) {
  ProgressObj *p;
  (void)parent; (void)title;
  if (!out) return E_INVALIDARG;
  *out = NULL;
  p = mem_alloc(sizeof(*p));
  if (!p) return E_OUTOFMEMORY;
  p->iface.lpVtbl = (IBindStatusCallbackVtbl *)&g_progress_vtbl;
  p->refs = 1;
  p->caller = caller;
  if (caller) IBindStatusCallback_AddRef(caller);
  *out = &p->iface;
  return S_OK;
}

/* ---- the oleaut32 forwarders ---------------------------------------------------------------------------------------------------------------------------------------- */

static void *oleaut_fn(const char *name) {
  HMODULE m = LoadLibraryW(W('o', 'l', 'e', 'a', 'u', 't', '3', '2', '.', 'd', 'l', 'l'));
  return m ? (void *)GetProcAddress(m, name) : NULL;
}
EXPORT HRESULT WINAPI OleCreateFontIndirectExt(void *desc, REFIID riid, void **out) {
  HRESULT (WINAPI *fn)(void *, REFIID, void **) = oleaut_fn("OleCreateFontIndirect");
  if (out) *out = NULL;
  return fn ? fn(desc, riid, out) : E_NOTIMPL;
}
EXPORT HRESULT WINAPI OleCreatePictureIndirectExt(void *desc, REFIID riid, BOOL own, void **out) {
  HRESULT (WINAPI *fn)(void *, REFIID, BOOL, void **) = oleaut_fn("OleCreatePictureIndirect");
  if (out) *out = NULL;
  return fn ? fn(desc, riid, own, out) : E_NOTIMPL;
}
EXPORT HRESULT WINAPI OleLoadPictureExt(LPSTREAM stm, LONG size, BOOL run_mode, REFIID riid, void **out) {
  HRESULT (WINAPI *fn)(LPSTREAM, LONG, BOOL, REFIID, void **) = oleaut_fn("OleLoadPicture");
  if (out) *out = NULL;
  return fn ? fn(stm, size, run_mode, riid, out) : E_NOTIMPL;
}
EXPORT HRESULT WINAPI OleLoadPictureFileExt(VARIANT file, IDispatch **out) {
  HRESULT (WINAPI *fn)(VARIANT, IDispatch **) = oleaut_fn("OleLoadPictureFile");
  if (out) *out = NULL;
  return fn ? fn(file, out) : E_NOTIMPL;
}
EXPORT HRESULT WINAPI OleLoadPicturePathExt(LPOLESTR path, LPUNKNOWN unk, DWORD reserved, DWORD back_color, REFIID riid, void **out) {
  HRESULT (WINAPI *fn)(LPOLESTR, LPUNKNOWN, DWORD, DWORD, REFIID, void **) = oleaut_fn("OleLoadPicturePath");
  if (out) *out = NULL;
  return fn ? fn(path, unk, reserved, back_color, riid, out) : E_NOTIMPL;
}
EXPORT HRESULT WINAPI OleSavePictureFileExt(IDispatch *picture, BSTR file) {
  HRESULT (WINAPI *fn)(IDispatch *, BSTR) = oleaut_fn("OleSavePictureFile");
  return fn ? fn(picture, file) : E_NOTIMPL;
}
EXPORT HANDLE WINAPI OleIconToCursorExt(HINSTANCE inst, HANDLE icon) {
  HANDLE (WINAPI *fn)(HINSTANCE, HANDLE) = oleaut_fn("OleIconToCursor");
  return fn ? fn(inst, icon) : NULL;
}
EXPORT HRESULT WINAPI OleTranslateColorExt(DWORD color, HANDLE palette, DWORD *colorref) {
  HRESULT (WINAPI *fn)(DWORD, HANDLE, DWORD *) = oleaut_fn("OleTranslateColor");
  return fn ? fn(color, palette, colorref) : E_NOTIMPL;
}
EXPORT HRESULT WINAPI OleCreatePropertyFrameIndirectExt(void *params) {
  HRESULT (WINAPI *fn)(void *) = oleaut_fn("OleCreatePropertyFrameIndirect");
  return fn ? fn(params) : E_NOTIMPL;
}
EXPORT HRESULT WINAPI RegisterActiveObjectExt(IUnknown *unk, REFCLSID clsid, DWORD flags, DWORD *cookie) {
  HRESULT (WINAPI *fn)(IUnknown *, REFCLSID, DWORD, DWORD *) = oleaut_fn("RegisterActiveObject");
  return fn ? fn(unk, clsid, flags, cookie) : E_NOTIMPL;
}
EXPORT HRESULT WINAPI RevokeActiveObjectExt(DWORD cookie, void *reserved) {
  HRESULT (WINAPI *fn)(DWORD, void *) = oleaut_fn("RevokeActiveObject");
  return fn ? fn(cookie, reserved) : E_NOTIMPL;
}
EXPORT HRESULT WINAPI GetActiveObjectExt(REFCLSID clsid, void *reserved, IUnknown **out) {
  HRESULT (WINAPI *fn)(REFCLSID, void *, IUnknown **) = oleaut_fn("GetActiveObject");
  if (out) *out = NULL;
  return fn ? fn(clsid, reserved, out) : E_NOTIMPL;
}

/* ---- legacy exports --------------------------------------------------------------------------------------------------------------------------------------------------- */
/* Hooks, DDE, 16 bit thunks and the NDR proxy helpers: nothing here has anything to do (no hook DLL, no DDE server, no 16 bit code, no RPC runtime). */

EXPORT HRESULT WINAPI UtConvertDvtd16toDvtd32(void *dvtd16, void *info, void *dvtd32) { (void)dvtd16; (void)info; (void)dvtd32; return E_NOTIMPL; }
EXPORT HRESULT WINAPI UtConvertDvtd32toDvtd16(void *dvtd32, void *info, void *dvtd16) { (void)dvtd32; (void)info; (void)dvtd16; return E_NOTIMPL; }
EXPORT HRESULT WINAPI UtGetDvtd16Info(void *dvtd16, void *info) { (void)dvtd16; (void)info; return E_NOTIMPL; }
EXPORT HRESULT WINAPI UtGetDvtd32Info(void *dvtd32, void *info) { (void)dvtd32; (void)info; return E_NOTIMPL; }
EXPORT HRESULT WINAPI HkOleRegisterObject(void *a, void *b, DWORD c, void *d) { (void)a; (void)b; (void)c; (void)d; return E_NOTIMPL; }
EXPORT HRESULT WINAPI DdeBindToObject(void *a, REFIID riid, BOOL b, void **out) { (void)a; (void)riid; (void)b; if (out) *out = NULL; return E_NOTIMPL; }
EXPORT BOOL WINAPI CheckInitDde(BOOL init) { (void)init; return FALSE; }
EXPORT BOOL WINAPI FindExt(const WCHAR *name, void *a, void *b) { (void)name; (void)a; (void)b; return FALSE; }
EXPORT void WINAPI DeletePatternAndExtensionTables(void) {}
EXPORT BOOL WINAPI EnableHookObject(BOOL enable, void *a) { (void)enable; (void)a; return FALSE; }
EXPORT HRESULT WINAPI GetHookInterface(void *a, void **out) { (void)a; if (out) *out = NULL; return E_NOTIMPL; }
EXPORT BOOL WINAPI DllDebugObjectRPCHook(BOOL trace, void *hook) { (void)trace; (void)hook; return FALSE; }
EXPORT HRESULT WINAPI DcomChannelSetHResult(LPVOID a, ULONG *b, HRESULT hr) { (void)a; (void)b; (void)hr; return S_OK; }
EXPORT HRESULT WINAPI IsRoInitializeASTAAllowedInDesktop(void *out) { (void)out; return E_NOTIMPL; }
static HMODULE g_oleaut_module;
EXPORT void WINAPI SetOleautModule(HMODULE m) { g_oleaut_module = m; }
EXPORT void *WINAPI SetWOWThunkGlobalPtr(void *p) { (void)p; return NULL; }
EXPORT HRESULT WINAPI Ole32DllGetClassObject(REFCLSID clsid, REFIID riid, void **out) {
  CreateObjectFn fn;
  if (!out) return E_INVALIDARG;
  *out = NULL;
  fn = find_builtin_class(clsid);
  return fn ? make_factory(fn, riid, out) : CLASS_E_CLASSNOTAVAILABLE;
}
EXPORT HRESULT WINAPI DllGetClassObjectWOW(REFCLSID clsid, REFIID riid, void **out) { return Ole32DllGetClassObject(clsid, riid, out); }
EXPORT HRESULT WINAPI StdTypesGetClassObject(REFCLSID clsid, REFIID riid, void **out) { (void)clsid; (void)riid; if (out) *out = NULL; return CLASS_E_CLASSNOTAVAILABLE; }
EXPORT HRESULT WINAPI StdTypesRegisterServer(void) { return E_NOTIMPL; }
EXPORT void WINAPI ClipboardProcessUninitialize(void) {}
EXPORT HRESULT WINAPI RoGetAgileReference(enum AgileReferenceOptions options, REFIID riid, IUnknown *unk, IAgileReference **out) { (void)options; (void)riid; (void)unk; if (out) *out = NULL; return E_NOTIMPL; }
EXPORT HRESULT WINAPI ComPs_NdrDllCanUnloadNow(void *info) { (void)info; return S_OK; }
EXPORT HRESULT WINAPI ComPs_NdrDllGetClassObject(REFCLSID clsid, REFIID riid, void **out, const void *a, const CLSID *b, void *c) {
  (void)clsid; (void)riid; (void)a; (void)b; (void)c;
  if (out) *out = NULL;
  return CLASS_E_CLASSNOTAVAILABLE;
}
EXPORT HRESULT WINAPI ComPs_NdrDllRegisterProxy(HMODULE m, const void *a, const CLSID *b) { (void)m; (void)a; (void)b; return E_NOTIMPL; }
EXPORT HRESULT WINAPI ComPs_NdrDllUnregisterProxy(HMODULE m, const void *a, const CLSID *b) { (void)m; (void)a; (void)b; return E_NOTIMPL; }
EXPORT void WINAPI NdrOleInitializeExtension(void) {}

#define STUB_NDR(n) \
  EXPORT HRESULT WINAPI NdrProxyForwardingFunction##n(void) { return E_NOTIMPL; } \
  EXPORT HRESULT WINAPI ObjectStublessClient##n(void) { return E_NOTIMPL; }
STUB_NDR(3) STUB_NDR(4) STUB_NDR(5) STUB_NDR(6) STUB_NDR(7) STUB_NDR(8) STUB_NDR(9) STUB_NDR(10)
STUB_NDR(11) STUB_NDR(12) STUB_NDR(13) STUB_NDR(14) STUB_NDR(15) STUB_NDR(16) STUB_NDR(17) STUB_NDR(18)
STUB_NDR(19) STUB_NDR(20) STUB_NDR(21) STUB_NDR(22) STUB_NDR(23) STUB_NDR(24) STUB_NDR(25) STUB_NDR(26)
STUB_NDR(27) STUB_NDR(28) STUB_NDR(29) STUB_NDR(30) STUB_NDR(31) STUB_NDR(32)

#define BUILTIN_CLASSES_OLECREATE(X) X(g_clsid_default_handler, defhandler_create)
