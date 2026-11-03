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

/* Registration of type libraries: HKEY_CLASSES_ROOT\TypeLib\{libid}\<major>.<minor> (default value: the library's name; subkeys FLAGS, HELPDIR and <lcid>\<win16|win32|win64>
 * whose default value is the path of the file) and HKEY_CLASSES_ROOT\Interface\{iid} for the dispatch and OLE automation interfaces (the proxy/stub class and a reference
 * back to the library), and the lookups through them. */

typedef struct { WCHAR s[320]; UINT n; BOOL overflow; } TlStr;
static void tls_add(TlStr *t, const WCHAR *w) {
  while (*w) { if (t->n + 1 >= sizeof(t->s) / sizeof(t->s[0])) { t->overflow = TRUE; return; } t->s[t->n++] = *w++; }
  t->s[t->n] = 0;
}
static void tls_hex(TlStr *t, DWORD v, UINT min_digits) {
  WCHAR tmp[9];
  UINT n = 0;
  do { tmp[n++] = (WCHAR)"0123456789abcdef"[v & 15]; v >>= 4; } while (v);
  while (n < min_digits && n < 8) tmp[n++] = '0';
  WCHAR out[9];
  for (UINT i = 0; i < n; i++) out[i] = tmp[n - 1 - i];
  out[n] = 0;
  tls_add(t, out);
}
static void tls_dec(TlStr *t, DWORD v) {
  WCHAR tmp[11], out[11];
  UINT n = 0;
  do { tmp[n++] = (WCHAR)('0' + v % 10); v /= 10; } while (v);
  for (UINT i = 0; i < n; i++) out[i] = tmp[n - 1 - i];
  out[n] = 0;
  tls_add(t, out);
}
/* {XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX} in upper case like StringFromGUID2 */
static void tls_guid(TlStr *t, const GUID *g) {
  static const WCHAR hexu[] = L"0123456789ABCDEF";
  WCHAR w[39];
  UINT n = 0;
  w[n++] = '{';
  for (int i = 7; i >= 0; i--) w[n++] = hexu[(g->Data1 >> (i * 4)) & 15];
  w[n++] = '-';
  for (int i = 3; i >= 0; i--) w[n++] = hexu[(g->Data2 >> (i * 4)) & 15];
  w[n++] = '-';
  for (int i = 3; i >= 0; i--) w[n++] = hexu[(g->Data3 >> (i * 4)) & 15];
  w[n++] = '-';
  for (int i = 0; i < 2; i++) { w[n++] = hexu[g->Data4[i] >> 4]; w[n++] = hexu[g->Data4[i] & 15]; }
  w[n++] = '-';
  for (int i = 2; i < 8; i++) { w[n++] = hexu[g->Data4[i] >> 4]; w[n++] = hexu[g->Data4[i] & 15]; }
  w[n++] = '}';
  w[n] = 0;
  tls_add(t, w);
}
static const WCHAR *tl_syskind_name(SYSKIND k) {
  switch (k) { case SYS_WIN16: return L"win16"; case SYS_MAC: return L"mac"; case SYS_WIN64: return L"win64"; default: return L"win32"; }
}

static HRESULT tl_reg_result(LONG ret) {
  if (ret == ERROR_SUCCESS) return S_OK;
  if (ret == ERROR_ACCESS_DENIED) return TYPE_E_REGISTRYACCESS;
  return HRESULT_FROM_WIN32((DWORD)ret);
}
/* The registrations are made in both views of the registry (a 32 bit and a 64 bit library are visible to both kinds of program). */
static const REGSAM tl_views[2] = { KEY_WOW64_64KEY, KEY_WOW64_32KEY };

/* Creates (or opens) a key below HKEY_CLASSES_ROOT and sets a value (the default one if name is NULL). */
static HRESULT tl_reg_put_value(const WCHAR *key, const WCHAR *name, const WCHAR *value) {
  HRESULT result = S_OK;
  for (UINT v = 0; v < 2; v++) {
    HKEY h;
    LONG ret = RegCreateKeyExW(HKEY_CLASSES_ROOT, key, 0, NULL, 0, KEY_WRITE | tl_views[v], NULL, &h, NULL);
    if (ret != ERROR_SUCCESS) { if (v == 0 || ret == ERROR_ACCESS_DENIED) return tl_reg_result(ret); continue; }
    if (value) ret = RegSetValueExW(h, name, 0, REG_SZ, (const BYTE *)value, (DWORD)((oa_wcslen(value) + 1) * sizeof(WCHAR)));
    RegCloseKey(h);
    if (ret != ERROR_SUCCESS) result = tl_reg_result(ret);
  }
  return result;
}
static HRESULT tl_reg_put(const WCHAR *key, const WCHAR *value) { return tl_reg_put_value(key, NULL, value); }
/* Deletes a key with everything below it from both views. */
static LONG tl_reg_delete_tree(const WCHAR *key, BOOL tree) {
  LONG result = ERROR_FILE_NOT_FOUND;
  for (UINT v = 0; v < 2; v++) {
    HKEY h;
    if (tree && RegOpenKeyExW(HKEY_CLASSES_ROOT, key, 0, KEY_ALL_ACCESS | tl_views[v], &h) == ERROR_SUCCESS) {
      RegDeleteTreeW(h, NULL);
      RegCloseKey(h);
    }
    /* (RegDeleteKeyExW is looked up: the stubs of this library define a function of that name) */
    typedef LONG (WINAPI *DeleteKeyEx)(HKEY, LPCWSTR, REGSAM, DWORD);
    HMODULE adv = GetModuleHandleW(L"advapi32.dll");
    DeleteKeyEx del = adv ? (DeleteKeyEx)(void *)GetProcAddress(adv, "RegDeleteKeyExW") : NULL;
    const LONG ret = del ? del(HKEY_CLASSES_ROOT, key, tl_views[v], 0) : (v == 0 ? RegDeleteKeyW(HKEY_CLASSES_ROOT, key) : ERROR_FILE_NOT_FOUND);
    if (ret == ERROR_SUCCESS) result = ERROR_SUCCESS;
    else if (result != ERROR_SUCCESS && ret != ERROR_FILE_NOT_FOUND) result = ret;
  }
  return result;
}
static void tls_typelib_key(TlStr *t, const GUID *guid, WORD major, WORD minor) {
  tls_add(t, L"TypeLib\\");
  tls_guid(t, guid);
  tls_add(t, L"\\");
  tls_hex(t, major, 1);
  tls_add(t, L".");
  tls_hex(t, minor, 1);
}

/* Interface\{iid} for one type of the library */
static HRESULT tl_register_interface(const TLIBATTR *la, ITypeInfo *ti, const TYPEATTR *ta) {
  TlStr k = { {0}, 0, FALSE };
  tls_add(&k, L"Interface\\");
  tls_guid(&k, &ta->guid);
  BSTR name = NULL;
  ITypeInfo_GetDocumentation(ti, MEMBERID_NIL, &name, NULL, NULL, NULL);
  HRESULT hr = tl_reg_put(k.s, name);
  SysFreeString(name);
  if (FAILED(hr)) return hr;
  const BOOL real_interface = ta->typekind == TKIND_INTERFACE || (ta->wTypeFlags & TYPEFLAG_FDUAL);
  const WCHAR *psclsid = real_interface ? L"{00020424-0000-0000-C000-000000000046}" : L"{00020420-0000-0000-C000-000000000046}";
  TlStr sub = k;
  tls_add(&sub, L"\\ProxyStubClsid");
  hr = tl_reg_put(sub.s, psclsid);
  if (SUCCEEDED(hr)) { sub = k; tls_add(&sub, L"\\ProxyStubClsid32"); hr = tl_reg_put(sub.s, psclsid); }
  if (SUCCEEDED(hr)) {
    sub = k;
    tls_add(&sub, L"\\TypeLib");
    TlStr guid = { {0}, 0, FALSE }, ver = { {0}, 0, FALSE };
    tls_guid(&guid, &la->guid);
    tls_hex(&ver, la->wMajorVerNum, 1);
    tls_add(&ver, L".");
    tls_hex(&ver, la->wMinorVerNum, 1);
    hr = tl_reg_put(sub.s, guid.s);
    if (SUCCEEDED(hr)) hr = tl_reg_put_value(sub.s, L"Version", ver.s);
  }
  return hr;
}

EXPORT HRESULT WINAPI RegisterTypeLib(ITypeLib *lib, LPCOLESTR path, LPCOLESTR helpdir) {
  if (!lib || !path) return E_INVALIDARG;
  TLIBATTR *la;
  if (FAILED(ITypeLib_GetLibAttr(lib, &la))) return E_FAIL;
  TlStr key = { {0}, 0, FALSE };
  tls_typelib_key(&key, &la->guid, la->wMajorVerNum, la->wMinorVerNum);
  BSTR name = NULL, doc = NULL;
  ITypeLib_GetDocumentation(lib, -1, &name, &doc, NULL, NULL);
  HRESULT hr = tl_reg_put(key.s, doc && doc[0] ? doc : name);
  SysFreeString(name);
  SysFreeString(doc);
  if (SUCCEEDED(hr)) {
    TlStr sub = key, value = { {0}, 0, FALSE };
    tls_add(&sub, L"\\FLAGS");
    tls_dec(&value, la->wLibFlags);
    hr = tl_reg_put(sub.s, value.s);
  }
  if (SUCCEEDED(hr)) {
    WCHAR dir[MAX_PATH];
    const WCHAR *help = helpdir;
    if (!help) {
      SIZE_T len = oa_wcslen(path), cut = len;
      while (cut > 0 && path[cut - 1] != '\\' && path[cut - 1] != '/') cut--;
      if (cut > 0) cut--;              /* (without the separator) */
      if (cut >= MAX_PATH) cut = MAX_PATH - 1;
      memcpy(dir, path, cut * sizeof(WCHAR));
      dir[cut] = 0;
      help = dir;
    }
    TlStr sub = key;
    tls_add(&sub, L"\\HELPDIR");
    hr = tl_reg_put(sub.s, help);
  }
  if (SUCCEEDED(hr)) {
    TlStr sub = key;
    tls_add(&sub, L"\\");
    tls_hex(&sub, la->lcid, 1);
    tls_add(&sub, L"\\");
    tls_add(&sub, tl_syskind_name(la->syskind));
    hr = tl_reg_put(sub.s, path);
  }
  const UINT count = ITypeLib_GetTypeInfoCount(lib);
  for (UINT i = 0; SUCCEEDED(hr) && i < count; i++) {
    ITypeInfo *ti;
    TYPEATTR *ta;
    if (FAILED(ITypeLib_GetTypeInfo(lib, i, &ti))) continue;
    if (SUCCEEDED(ITypeInfo_GetTypeAttr(ti, &ta))) {
      if ((ta->typekind == TKIND_INTERFACE && (ta->wTypeFlags & TYPEFLAG_FOLEAUTOMATION)) || ta->typekind == TKIND_DISPATCH) hr = tl_register_interface(la, ti, ta);
      ITypeInfo_ReleaseTypeAttr(ti, ta);
    }
    ITypeInfo_Release(ti);
  }
  ITypeLib_ReleaseTLibAttr(lib, la);
  return hr;
}
EXPORT HRESULT WINAPI RegisterTypeLibForUser(ITypeLib *lib, OLECHAR *path, OLECHAR *helpdir) { return RegisterTypeLib(lib, path, helpdir); }
EXPORT void WINAPI OaEnablePerUserTLibRegistration(void) {}

/* The registered file of a library: the version (the exact one, or with 0xffff.0xffff the highest, or else the highest minor of the major that is not older), then the
 * language (exact, without the sublanguage, neutral), then the kind of system (64 bit, 32 bit, 16 bit). */
static BOOL tl_parse_version(const WCHAR *s, DWORD *major, DWORD *minor) {
  DWORD v[2] = { 0, 0 };
  UINT part = 0, digits = 0;
  for (; *s; s++) {
    DWORD d;
    if (*s >= '0' && *s <= '9') d = (DWORD)(*s - '0');
    else if (*s >= 'a' && *s <= 'f') d = (DWORD)(*s - 'a' + 10);
    else if (*s >= 'A' && *s <= 'F') d = (DWORD)(*s - 'A' + 10);
    else if (*s == '.' && part == 0 && digits) { part = 1; digits = 0; continue; }
    else return FALSE;
    if (++digits > 4) return FALSE;
    v[part] = v[part] * 16 + d;
  }
  if (part != 1 || !digits) return FALSE;
  *major = v[0];
  *minor = v[1];
  return TRUE;
}

static HRESULT tl_find_version(HKEY libkey, WORD major, WORD minor, WCHAR *name /* [32] */) {
  TlStr exact = { {0}, 0, FALSE };
  tls_hex(&exact, major, 1);
  tls_add(&exact, L".");
  tls_hex(&exact, minor, 1);
  HKEY probe;
  const BOOL any = major == 0xffff && minor == 0xffff;
  if (!any && RegOpenKeyExW(libkey, exact.s, 0, KEY_READ, &probe) == ERROR_SUCCESS) {
    RegCloseKey(probe);
    memcpy(name, exact.s, (oa_wcslen(exact.s) + 1) * sizeof(WCHAR));
    return S_OK;
  }
  BOOL found = FALSE;
  DWORD best_major = 0, best_minor = 0;
  for (DWORD i = 0; i < 4096; i++) {
    WCHAR sub[32];
    DWORD len = 32;
    LONG ret = RegEnumKeyExW(libkey, i, sub, &len, NULL, NULL, NULL, NULL);
    if (ret != ERROR_SUCCESS) break;
    DWORD ma, mi;
    if (!tl_parse_version(sub, &ma, &mi)) continue;
    if (!any && (ma != major || mi < minor)) continue;
    if (!found || ma > best_major || (ma == best_major && mi > best_minor)) {
      found = TRUE;
      best_major = ma;
      best_minor = mi;
      memcpy(name, sub, (len + 1) * sizeof(WCHAR));
    }
  }
  return found ? S_OK : TYPE_E_LIBNOTREGISTERED;
}

EXPORT HRESULT WINAPI QueryPathOfRegTypeLib(REFGUID guid, WORD major, WORD minor, LCID lcid, LPBSTR path) {
  if (!guid || !path) return E_INVALIDARG;
  TlStr key = { {0}, 0, FALSE };
  tls_add(&key, L"TypeLib\\");
  tls_guid(&key, guid);
  HKEY libkey;
  if (RegOpenKeyExW(HKEY_CLASSES_ROOT, key.s, 0, KEY_READ, &libkey) != ERROR_SUCCESS) return TYPE_E_LIBNOTREGISTERED;
  WCHAR ver[32];
  HRESULT hr = tl_find_version(libkey, major, minor, ver);
  if (SUCCEEDED(hr)) {
    LCID tries[3] = { lcid, lcid & 0x3ff, 0 };
    hr = TYPE_E_LIBNOTREGISTERED;
    for (UINT t = 0; t < 3 && hr != S_OK; t++) {
      if (t > 0 && tries[t] == tries[t - 1]) continue;
      static const SYSKIND kinds[] = { SYS_WIN64, SYS_WIN32, SYS_WIN16 };
      for (UINT k = 0; k < 3 && hr != S_OK; k++) {
        TlStr sub = { {0}, 0, FALSE };
        tls_add(&sub, ver);
        tls_add(&sub, L"\\");
        tls_hex(&sub, tries[t], 1);
        tls_add(&sub, L"\\");
        tls_add(&sub, tl_syskind_name(kinds[k]));
        HKEY h;
        if (RegOpenKeyExW(libkey, sub.s, 0, KEY_READ, &h) != ERROR_SUCCESS) continue;
        WCHAR value[MAX_PATH + 16];
        DWORD size = sizeof(value) - sizeof(WCHAR), type = 0;
        if (RegQueryValueExW(h, NULL, NULL, &type, (BYTE *)value, &size) == ERROR_SUCCESS && type == REG_SZ && size >= sizeof(WCHAR)) {
          value[size / sizeof(WCHAR)] = 0;
          *path = SysAllocString(value);
          hr = *path ? S_OK : E_OUTOFMEMORY;
        }
        RegCloseKey(h);
      }
    }
  }
  RegCloseKey(libkey);
  return hr;
}

EXPORT HRESULT WINAPI LoadRegTypeLib(REFGUID guid, WORD major, WORD minor, LCID lcid, ITypeLib **out) {
  if (!guid || !out) return E_INVALIDARG;
  BSTR path = NULL;
  HRESULT hr = QueryPathOfRegTypeLib(guid, major, minor, lcid, &path);
  if (FAILED(hr)) return hr;
  hr = LoadTypeLibEx(path, REGKIND_NONE, out);
  SysFreeString(path);
  return hr;
}

EXPORT HRESULT WINAPI UnRegisterTypeLib(REFGUID guid, WORD major, WORD minor, LCID lcid, SYSKIND syskind) {
  if (!guid) return E_INVALIDARG;
  TlStr key = { {0}, 0, FALSE };
  tls_typelib_key(&key, guid, major, minor);
  /* the interfaces of the library are found through the library itself */
  BSTR path = NULL;
  if (SUCCEEDED(QueryPathOfRegTypeLib(guid, major, minor, lcid, &path))) {
    ITypeLib *lib;
    if (SUCCEEDED(LoadTypeLibEx(path, REGKIND_NONE, &lib))) {
      const UINT count = ITypeLib_GetTypeInfoCount(lib);
      for (UINT i = 0; i < count; i++) {
        ITypeInfo *ti;
        TYPEATTR *ta;
        if (FAILED(ITypeLib_GetTypeInfo(lib, i, &ti))) continue;
        if (SUCCEEDED(ITypeInfo_GetTypeAttr(ti, &ta))) {
          if ((ta->typekind == TKIND_INTERFACE && (ta->wTypeFlags & TYPEFLAG_FOLEAUTOMATION)) || ta->typekind == TKIND_DISPATCH) {
            TlStr ik = { {0}, 0, FALSE };
            tls_add(&ik, L"Interface\\");
            tls_guid(&ik, &ta->guid);
            tl_reg_delete_tree(ik.s, TRUE);
          }
          ITypeInfo_ReleaseTypeAttr(ti, ta);
        }
        ITypeInfo_Release(ti);
      }
      ITypeLib_Release(lib);
    }
    SysFreeString(path);
  }
  TlStr sub = key;
  tls_add(&sub, L"\\");
  tls_hex(&sub, lcid, 1);
  tls_add(&sub, L"\\");
  tls_add(&sub, tl_syskind_name(syskind));
  LONG ret = tl_reg_delete_tree(sub.s, TRUE);
  if (ret != ERROR_SUCCESS && ret != ERROR_FILE_NOT_FOUND) return tl_reg_result(ret);
  HRESULT hr = ret == ERROR_FILE_NOT_FOUND ? TYPE_E_REGISTRYACCESS : S_OK;
  /* the language key, then (when no language remains) the rest of the version and the library */
  sub = key;
  tls_add(&sub, L"\\");
  tls_hex(&sub, lcid, 1);
  tl_reg_delete_tree(sub.s, FALSE);
  HKEY vk;
  if (RegOpenKeyExW(HKEY_CLASSES_ROOT, key.s, 0, KEY_READ, &vk) == ERROR_SUCCESS) {
    BOOL only_info = TRUE;
    for (DWORD i = 0; i < 4096; i++) {
      WCHAR n[32];
      DWORD len = 32;
      if (RegEnumKeyExW(vk, i, n, &len, NULL, NULL, NULL, NULL) != ERROR_SUCCESS) break;
      if (tl_wcsicmp(n, L"FLAGS") && tl_wcsicmp(n, L"HELPDIR")) { only_info = FALSE; break; }
    }
    RegCloseKey(vk);
    if (only_info) {
      TlStr d = key;
      tls_add(&d, L"\\FLAGS");
      tl_reg_delete_tree(d.s, FALSE);
      d = key;
      tls_add(&d, L"\\HELPDIR");
      tl_reg_delete_tree(d.s, FALSE);
      tl_reg_delete_tree(key.s, FALSE);
      TlStr g = { {0}, 0, FALSE };
      tls_add(&g, L"TypeLib\\");
      tls_guid(&g, guid);
      tl_reg_delete_tree(g.s, FALSE);
    }
  }
  return hr;
}
EXPORT HRESULT WINAPI UnRegisterTypeLibForUser(REFGUID guid, WORD major, WORD minor, LCID lcid, SYSKIND syskind) { return UnRegisterTypeLib(guid, major, minor, lcid, syskind); }
