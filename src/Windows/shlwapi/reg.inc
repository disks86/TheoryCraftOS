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

/* The rest of the registry helpers: SHRegGetValue, the paths in the registry, SHCopyKey, the enumeration wrappers and the orphan-key deleters. Included by shlwapi.c after shell.inc. */

IMPORT LSTATUS_ WINAPI RegEnumValueW(HKEY key, DWORD index, LPWSTR name, LPDWORD name_size, LPDWORD reserved, LPDWORD type, LPBYTE data, LPDWORD size);
IMPORT LSTATUS_ WINAPI RegQueryInfoKeyW(HKEY key, LPWSTR cls, LPDWORD cls_size, LPDWORD reserved, LPDWORD subkeys, LPDWORD max_subkey, LPDWORD max_class, LPDWORD values, LPDWORD max_name,
                                        LPDWORD max_data, LPDWORD security, LPFILETIME time);

#define SRRF_NONE_ 0x1
#define SRRF_SZ_ 0x2
#define SRRF_EXPAND_SZ_ 0x4
#define SRRF_BINARY_ 0x8
#define SRRF_DWORD_ 0x10
#define SRRF_MULTI_SZ_ 0x20
#define SRRF_QWORD_ 0x40
#define SRRF_NOEXPAND_ 0x10000000
#define SRRF_ZEROONFAILURE_ 0x20000000

static BOOL reg_type_is_string(DWORD t) { return t == REG_SZ_ || t == REG_EXPAND_SZ_ || t == 7; }

/* The type of a value as the SRRF_RT_* flag that allows it (0 when no flag does). */
static DWORD srrf_for_type(DWORD t, DWORD flags) {
  switch (t) {
    case 0: return SRRF_NONE_;
    case REG_SZ_: return SRRF_SZ_;
    case REG_EXPAND_SZ_: return (flags & SRRF_NOEXPAND_) ? SRRF_EXPAND_SZ_ : SRRF_SZ_;
    case 3: return SRRF_BINARY_;
    case 4: return SRRF_DWORD_;
    case 7: return SRRF_MULTI_SZ_;
    case 11: return SRRF_QWORD_;
    default: return 0;
  }
}

static LSTATUS_ reg_get_value(HKEY root, LPCWSTR subkey, LPCWSTR value, DWORD flags, LPDWORD type_out, LPVOID data, LPDWORD size, BOOL ansi) {
  HKEY key = root;
  BOOL opened = FALSE;
  LSTATUS_ r;
  DWORD t = 0, need = 0;
  BYTE *raw = NULL, *expanded = NULL, *converted = NULL;
  const BYTE *out = NULL;
  DWORD out_size = 0;
  const DWORD room = (data && size) ? *size : 0;
  if (((flags & SRRF_EXPAND_SZ_) && !(flags & SRRF_NOEXPAND_)) || (data && !size)) return ERROR_INVALID_PARAMETER;
  if (subkey && *subkey) {
    r = RegOpenKeyExW(root, subkey, 0, KEY_READ_, &key);
    if (r) goto fail;
    opened = TRUE;
  }
  r = RegQueryValueExW(key, value, NULL, &t, NULL, &need);
  if (r) goto fail;
  if (!(flags & srrf_for_type(t, flags))) { r = ERROR_UNSUPPORTED_TYPE; goto fail; }
  raw = task_alloc(need + 4);
  if (!raw) { r = ERROR_OUTOFMEMORY; goto fail; }
  DWORD got = need;
  r = RegQueryValueExW(key, value, NULL, NULL, raw, &got);
  if (r) goto fail;
  if (reg_type_is_string(t)) { raw[got] = raw[got + 1] = 0; }
  out = raw; out_size = got;
  if (t == REG_EXPAND_SZ_ && !(flags & SRRF_NOEXPAND_)) {
    const DWORD n = ExpandEnvironmentStringsW((LPCWSTR)raw, NULL, 0);
    expanded = task_alloc((n + 1) * sizeof(WCHAR));
    if (!expanded) { r = ERROR_OUTOFMEMORY; goto fail; }
    ExpandEnvironmentStringsW((LPCWSTR)raw, (LPWSTR)expanded, n + 1);
    out = expanded; out_size = n * sizeof(WCHAR); t = REG_SZ_;
  }
  if (ansi && reg_type_is_string(t)) {
    const int wide_chars = (int)(out_size / sizeof(WCHAR));
    const int n = wide_chars ? WideCharToMultiByte(CP_ACP, 0, (LPCWSTR)out, wide_chars, NULL, 0, NULL, NULL) : 0;
    converted = task_alloc((size_t_)n + 1);
    if (!converted) { r = ERROR_OUTOFMEMORY; goto fail; }
    if (n) WideCharToMultiByte(CP_ACP, 0, (LPCWSTR)out, wide_chars, (LPSTR)converted, n, NULL, NULL);
    out = converted; out_size = (DWORD)n;
  }
  if (type_out) *type_out = t;
  if (data) {
    if (room < out_size) { r = ERROR_MORE_DATA; if (size) *size = out_size; goto fail; }
    memcpy(data, out, out_size);
  }
  if (size) *size = out_size;
  r = 0;
fail:
  if (r && (flags & SRRF_ZEROONFAILURE_) && data && room) memset(data, 0, room);
  if (opened) RegCloseKey(key);
  task_free(raw); task_free(expanded); task_free(converted);
  return r;
}
EXPORT LSTATUS_ WINAPI SHRegGetValueW(HKEY root, LPCWSTR subkey, LPCWSTR value, DWORD flags, LPDWORD type, LPVOID data, LPDWORD size) {
  return reg_get_value(root, subkey, value, flags, type, data, size, FALSE);
}
EXPORT LSTATUS_ WINAPI SHRegGetValueA(HKEY root, LPCSTR subkey, LPCSTR value, DWORD flags, LPDWORD type, LPVOID data, LPDWORD size) {
  WCHAR *k = to_wide_dup(subkey), *v = to_wide_dup(value);
  LSTATUS_ r = ERROR_OUTOFMEMORY;
  if ((!subkey || k) && (!value || v)) r = reg_get_value(root, k, v, flags, type, data, size, TRUE);
  task_free(k); task_free(v);
  return r;
}

/* ---- paths ------------------------------------------------------------------------------------------------------------------------------------------------------- */

EXPORT LSTATUS_ WINAPI SHRegGetPathA(HKEY root, LPCSTR subkey, LPCSTR value, LPSTR path, DWORD flags) {
  WCHAR wide[MAX_PATH_];
  WCHAR *k = to_wide_dup(subkey), *v = to_wide_dup(value);
  LSTATUS_ r = ERROR_OUTOFMEMORY;
  if ((!subkey || k) && (!value || v)) {
    r = SHRegGetPathW(root, k, v, wide, flags);
    if (!r && !WideCharToMultiByte(CP_ACP, 0, wide, -1, path, MAX_PATH_, NULL, NULL)) r = ERROR_MORE_DATA;
  }
  task_free(k); task_free(v);
  return r;
}
EXPORT LSTATUS_ WINAPI SHRegSetPathW(HKEY root, LPCWSTR subkey, LPCWSTR value, LPCWSTR path, DWORD flags) {
  WCHAR unexpanded[MAX_PATH_];
  (void)flags;
  const BOOL expandable = unexpand_env(path, unexpanded, MAX_PATH_);
  const WCHAR *text = expandable ? unexpanded : path;
  return SHSetValueW(root, subkey, value, expandable ? REG_EXPAND_SZ_ : REG_SZ_, text, (DWORD)((wcslen(text) + 1) * sizeof(WCHAR)));
}
EXPORT LSTATUS_ WINAPI SHRegSetPathA(HKEY root, LPCSTR subkey, LPCSTR value, LPCSTR path, DWORD flags) {
  WCHAR *k = to_wide_dup(subkey), *v = to_wide_dup(value), *p = to_wide_dup(path);
  LSTATUS_ r = ERROR_OUTOFMEMORY;
  if ((!subkey || k) && (!value || v) && p) r = SHRegSetPathW(root, k, v, p, flags);
  task_free(k); task_free(v); task_free(p);
  return r;
}
EXPORT int WINAPI SHRegGetIntW(HKEY key, LPCWSTR value, int fallback) {
  WCHAR text[32];
  DWORD size = sizeof(text), type = 0;
  if (SHQueryValueExW(key, value, NULL, &type, text, &size) || type != REG_SZ_) return fallback;
  text[31] = 0;
  return StrToIntW(text);
}

/* ---- copying, enumerating, deleting --------------------------------------------------------------------------------------------------------------------------------- */

static LSTATUS_ copy_key_tree(HKEY src, HKEY dst) {
  DWORD subkeys, max_subkey, values, max_name, max_data;
  LSTATUS_ r = RegQueryInfoKeyW(src, NULL, NULL, NULL, &subkeys, &max_subkey, NULL, &values, &max_name, &max_data, NULL, NULL);
  if (r) return r;
  const DWORD name_capacity = (max_name > max_subkey ? max_name : max_subkey) + 2;
  WCHAR *name = task_alloc((size_t_)name_capacity * sizeof(WCHAR));
  BYTE *data = task_alloc((size_t_)max_data + 4);
  if (!name || !data) { task_free(name); task_free(data); return ERROR_OUTOFMEMORY; }
  for (DWORD i = 0; i < values && !r; ++i) {
    DWORD name_size = name_capacity, size = max_data, type = 0;
    r = RegEnumValueW(src, i, name, &name_size, NULL, &type, data, &size);
    if (!r) r = RegSetValueExW(dst, name, 0, type, data, size);
  }
  for (DWORD i = 0; i < subkeys && !r; ++i) {
    DWORD name_size = name_capacity;
    HKEY child_src, child_dst;
    r = RegEnumKeyExW(src, i, name, &name_size, NULL, NULL, NULL, NULL);
    if (r) break;
    r = RegOpenKeyExW(src, name, 0, KEY_READ_, &child_src);
    if (r) break;
    r = RegCreateKeyExW(dst, name, 0, NULL, 0, KEY_ALL_ACCESS_, NULL, &child_dst, NULL);
    if (!r) { r = copy_key_tree(child_src, child_dst); RegCloseKey(child_dst); }
    RegCloseKey(child_src);
  }
  task_free(name); task_free(data);
  return r;
}
EXPORT LSTATUS_ WINAPI SHCopyKeyW(HKEY src_root, LPCWSTR src_subkey, HKEY dst, DWORD reserved) {
  HKEY src = src_root;
  LSTATUS_ r = 0;
  (void)reserved;
  if (src_subkey && *src_subkey) r = RegOpenKeyExW(src_root, src_subkey, 0, KEY_READ_, &src);
  if (r) return r;
  r = copy_key_tree(src, dst);
  if (src != src_root) RegCloseKey(src);
  return r;
}
EXPORT LSTATUS_ WINAPI SHCopyKeyA(HKEY src_root, LPCSTR src_subkey, HKEY dst, DWORD reserved) {
  WCHAR *k = to_wide_dup(src_subkey);
  const LSTATUS_ r = (k || !src_subkey) ? SHCopyKeyW(src_root, k, dst, reserved) : ERROR_OUTOFMEMORY;
  task_free(k);
  return r;
}
EXPORT LSTATUS_ WINAPI SHEnumKeyExW(HKEY key, DWORD index, LPWSTR name, LPDWORD size) { return RegEnumKeyExW(key, index, name, size, NULL, NULL, NULL, NULL); }
EXPORT LSTATUS_ WINAPI SHEnumKeyExA(HKEY key, DWORD index, LPSTR name, LPDWORD size) {
  WCHAR wide[MAX_PATH_ + 1];
  DWORD n = MAX_PATH_;
  LSTATUS_ r = RegEnumKeyExW(key, index, wide, &n, NULL, NULL, NULL, NULL);
  if (r) return r;
  const int bytes = WideCharToMultiByte(CP_ACP, 0, wide, -1, name, (int)*size, NULL, NULL);
  if (!bytes) return ERROR_MORE_DATA;
  *size = (DWORD)bytes - 1;
  return 0;
}
EXPORT LSTATUS_ WINAPI SHEnumValueW(HKEY key, DWORD index, LPWSTR name, LPDWORD name_size, LPDWORD type, LPVOID data, LPDWORD size) {
  return RegEnumValueW(key, index, name, name_size, NULL, type, data, size);
}
EXPORT LSTATUS_ WINAPI SHEnumValueA(HKEY key, DWORD index, LPSTR name, LPDWORD name_size, LPDWORD type, LPVOID data, LPDWORD size) {
  WCHAR wide[MAX_PATH_ + 1];
  DWORD n = MAX_PATH_;
  LSTATUS_ r = RegEnumValueW(key, index, wide, &n, NULL, type, data, size);
  if (r) return r;
  const int bytes = name_size ? WideCharToMultiByte(CP_ACP, 0, wide, -1, name, (int)*name_size, NULL, NULL) : 0;
  if (!bytes) return ERROR_MORE_DATA;
  *name_size = (DWORD)bytes - 1;
  return 0;
}
EXPORT LSTATUS_ WINAPI SHQueryInfoKeyW(HKEY key, LPDWORD subkeys, LPDWORD max_subkey, LPDWORD values, LPDWORD max_value) {
  return RegQueryInfoKeyW(key, NULL, NULL, NULL, subkeys, max_subkey, NULL, values, max_value, NULL, NULL, NULL);
}
EXPORT LSTATUS_ WINAPI SHQueryInfoKeyA(HKEY key, LPDWORD subkeys, LPDWORD max_subkey, LPDWORD values, LPDWORD max_value) { return SHQueryInfoKeyW(key, subkeys, max_subkey, values, max_value); }
EXPORT LSTATUS_ WINAPI SHDeleteValueA(HKEY root, LPCSTR subkey, LPCSTR value) {
  WCHAR *k = to_wide_dup(subkey), *v = to_wide_dup(value);
  LSTATUS_ r = ERROR_OUTOFMEMORY;
  if ((!subkey || k) && (!value || v)) r = SHDeleteValueW(root, k, v);
  task_free(k); task_free(v);
  return r;
}
EXPORT LSTATUS_ WINAPI SHDeleteEmptyKeyA(HKEY root, LPCSTR subkey) {
  WCHAR *k = to_wide_dup(subkey);
  const LSTATUS_ r = (k || !subkey) ? SHDeleteEmptyKeyW(root, k) : ERROR_OUTOFMEMORY;
  task_free(k);
  return r;
}
EXPORT LSTATUS_ WINAPI SHDeleteOrphanKeyW(HKEY root, LPCWSTR subkey) {
  HKEY key;
  DWORD subkeys = 0, values = 0;
  LSTATUS_ r = RegOpenKeyExW(root, subkey, 0, KEY_READ_, &key);
  if (r) return r;
  r = RegQueryInfoKeyW(key, NULL, NULL, NULL, &subkeys, NULL, NULL, &values, NULL, NULL, NULL, NULL);
  RegCloseKey(key);
  if (r) return r;
  return (subkeys || values) ? ERROR_KEY_HAS_CHILDREN : RegDeleteKeyW(root, subkey);
}
EXPORT LSTATUS_ WINAPI SHDeleteOrphanKeyA(HKEY root, LPCSTR subkey) {
  WCHAR *k = to_wide_dup(subkey);
  const LSTATUS_ r = (k || !subkey) ? SHDeleteOrphanKeyW(root, k) : ERROR_OUTOFMEMORY;
  task_free(k);
  return r;
}

/* ---- user-specific keys (HUSKEY): a key that lives under both HKEY_CURRENT_USER and HKEY_LOCAL_MACHINE ---------------------------------------------------------- */

#define SHREGSET_HKCU_ 1
#define SHREGSET_FORCE_HKCU_ 2
#define SHREGSET_HKLM_ 4
#define SHREGSET_FORCE_HKLM_ 8
typedef struct { HKEY cu, lm; WCHAR path[MAX_PATH_]; } USKey;

static BOOL us_join(WCHAR *out, const USKey *relative, LPCWSTR path) {
  size_t_ n = 0;
  if (relative && relative->path[0]) { n = wcslen(relative->path); if (n + 2 > MAX_PATH_) return FALSE; memcpy(out, relative->path, n * sizeof(WCHAR)); if (path && *path) out[n++] = '\\'; }
  const size_t_ m = path ? wcslen(path) : 0;
  if (n + m + 1 > MAX_PATH_) return FALSE;
  if (m) memcpy(out + n, path, m * sizeof(WCHAR));
  out[n + m] = 0;
  return TRUE;
}
EXPORT LSTATUS_ WINAPI SHRegCloseUSKey(USKey *key) {
  if (!key) return ERROR_INVALID_PARAMETER;
  if (key->cu) RegCloseKey(key->cu);
  if (key->lm) RegCloseKey(key->lm);
  task_free(key);
  return 0;
}
EXPORT LSTATUS_ WINAPI SHRegOpenUSKeyW(LPCWSTR path, REGSAM_ access, USKey *relative, USKey **out, BOOL ignore_hkcu) {
  if (!out) return ERROR_INVALID_PARAMETER;
  *out = NULL;
  USKey *k = task_alloc(sizeof(USKey));
  if (!k) return ERROR_OUTOFMEMORY;
  memset(k, 0, sizeof(*k));
  if (!us_join(k->path, relative, path)) { task_free(k); return ERROR_INVALID_PARAMETER; }
  LSTATUS_ cu = ERROR_FILE_NOT_FOUND, lm;
  if (!ignore_hkcu) cu = RegOpenKeyExW(HKEY_CURRENT_USER_, k->path, 0, access, &k->cu);
  lm = RegOpenKeyExW(HKEY_LOCAL_MACHINE_, k->path, 0, access & KEY_READ_, &k->lm);
  if (cu) k->cu = NULL;
  if (lm) k->lm = NULL;
  if (cu && lm) { task_free(k); return lm; }
  *out = k;
  return 0;
}
EXPORT LSTATUS_ WINAPI SHRegOpenUSKeyA(LPCSTR path, REGSAM_ access, USKey *relative, USKey **out, BOOL ignore_hkcu) {
  WCHAR *p = to_wide_dup(path);
  const LSTATUS_ r = (p || !path) ? SHRegOpenUSKeyW(p, access, relative, out, ignore_hkcu) : ERROR_OUTOFMEMORY;
  task_free(p);
  return r;
}
EXPORT LSTATUS_ WINAPI SHRegCreateUSKeyW(LPCWSTR path, REGSAM_ access, USKey *relative, USKey **out, DWORD flags) {
  if (!out || !path) return ERROR_INVALID_PARAMETER;
  *out = NULL;
  USKey *k = task_alloc(sizeof(USKey));
  if (!k) return ERROR_OUTOFMEMORY;
  memset(k, 0, sizeof(*k));
  if (!us_join(k->path, relative, path)) { task_free(k); return ERROR_INVALID_PARAMETER; }
  if (!(flags & (SHREGSET_HKCU_ | SHREGSET_FORCE_HKCU_ | SHREGSET_HKLM_ | SHREGSET_FORCE_HKLM_))) flags = SHREGSET_FORCE_HKCU_;
  LSTATUS_ r = 0;
  if (flags & (SHREGSET_HKCU_ | SHREGSET_FORCE_HKCU_)) r = RegCreateKeyExW(HKEY_CURRENT_USER_, k->path, 0, NULL, 0, access, NULL, &k->cu, NULL);
  if (!r && (flags & (SHREGSET_HKLM_ | SHREGSET_FORCE_HKLM_))) r = RegCreateKeyExW(HKEY_LOCAL_MACHINE_, k->path, 0, NULL, 0, access, NULL, &k->lm, NULL);
  if (r) { SHRegCloseUSKey(k); return r; }
  *out = k;
  return 0;
}
EXPORT LSTATUS_ WINAPI SHRegCreateUSKeyA(LPCSTR path, REGSAM_ access, USKey *relative, USKey **out, DWORD flags) {
  WCHAR *p = to_wide_dup(path);
  const LSTATUS_ r = p ? SHRegCreateUSKeyW(p, access, relative, out, flags) : (path ? ERROR_OUTOFMEMORY : ERROR_INVALID_PARAMETER);
  task_free(p);
  return r;
}
EXPORT HKEY WINAPI SHRegDuplicateHKey(HKEY key) {
  HKEY copy = NULL;
  return RegOpenKeyExW(key, NULL, 0, KEY_ALL_ACCESS_, &copy) ? NULL : copy;
}

static LSTATUS_ us_query(USKey *key, LPCWSTR value, LPDWORD type, LPVOID data, LPDWORD size, BOOL ignore_hkcu, LPVOID fallback, DWORD fallback_size) {
  LSTATUS_ r = ERROR_FILE_NOT_FOUND;
  if (!key) return ERROR_INVALID_PARAMETER;
  if (!ignore_hkcu && key->cu) r = RegQueryValueExW(key->cu, value, NULL, type, data, size);
  if (r && key->lm) r = RegQueryValueExW(key->lm, value, NULL, type, data, size);
  if (r == ERROR_FILE_NOT_FOUND && fallback && fallback_size) {
    if (size && data && *size < fallback_size) { *size = fallback_size; return ERROR_MORE_DATA; }
    if (data) memcpy(data, fallback, fallback_size);
    if (size) *size = fallback_size;
    return 0;
  }
  return r;
}
EXPORT LSTATUS_ WINAPI SHRegQueryUSValueW(USKey *key, LPCWSTR value, LPDWORD type, LPVOID data, LPDWORD size, BOOL ignore_hkcu, LPVOID fallback, DWORD fallback_size) {
  return us_query(key, value, type, data, size, ignore_hkcu, fallback, fallback_size);
}
EXPORT LSTATUS_ WINAPI SHRegQueryUSValueA(USKey *key, LPCSTR value, LPDWORD type, LPVOID data, LPDWORD size, BOOL ignore_hkcu, LPVOID fallback, DWORD fallback_size) {
  WCHAR *v = to_wide_dup(value);
  LSTATUS_ r = ERROR_OUTOFMEMORY;
  if (v || !value) r = us_query(key, v, type, data, size, ignore_hkcu, fallback, fallback_size);
  task_free(v);
  return r;
}
EXPORT LSTATUS_ WINAPI SHRegGetUSValueW(LPCWSTR subkey, LPCWSTR value, LPDWORD type, LPVOID data, LPDWORD size, BOOL ignore_hkcu, LPVOID fallback, DWORD fallback_size) {
  USKey *key;
  LSTATUS_ r = SHRegOpenUSKeyW(subkey, KEY_READ_, NULL, &key, ignore_hkcu);
  if (r) {
    if (r == ERROR_FILE_NOT_FOUND && fallback && fallback_size && data && size && *size >= fallback_size) { memcpy(data, fallback, fallback_size); *size = fallback_size; return 0; }
    return r;
  }
  r = us_query(key, value, type, data, size, ignore_hkcu, fallback, fallback_size);
  SHRegCloseUSKey(key);
  return r;
}
EXPORT LSTATUS_ WINAPI SHRegGetUSValueA(LPCSTR subkey, LPCSTR value, LPDWORD type, LPVOID data, LPDWORD size, BOOL ignore_hkcu, LPVOID fallback, DWORD fallback_size) {
  WCHAR *k = to_wide_dup(subkey), *v = to_wide_dup(value);
  LSTATUS_ r = ERROR_OUTOFMEMORY;
  if ((k || !subkey) && (v || !value)) r = SHRegGetUSValueW(k, v, type, data, size, ignore_hkcu, fallback, fallback_size);
  task_free(k); task_free(v);
  return r;
}
EXPORT LSTATUS_ WINAPI SHRegWriteUSValueW(USKey *key, LPCWSTR value, DWORD type, LPCVOID data, DWORD size, DWORD flags) {
  LSTATUS_ r = 0;
  if (!key || !data) return ERROR_INVALID_PARAMETER;
  if (!flags) flags = SHREGSET_HKCU_ | SHREGSET_HKLM_;
  if (flags & (SHREGSET_HKCU_ | SHREGSET_FORCE_HKCU_)) {
    if (!key->cu) r = RegCreateKeyExW(HKEY_CURRENT_USER_, key->path, 0, NULL, 0, KEY_ALL_ACCESS_, NULL, &key->cu, NULL);
    if (!r) r = RegSetValueExW(key->cu, value, 0, type, data, size);
  }
  if (!r && (flags & (SHREGSET_HKLM_ | SHREGSET_FORCE_HKLM_))) {
    if (!key->lm) r = RegCreateKeyExW(HKEY_LOCAL_MACHINE_, key->path, 0, NULL, 0, KEY_ALL_ACCESS_, NULL, &key->lm, NULL);
    if (!r) r = RegSetValueExW(key->lm, value, 0, type, data, size);
  }
  return r;
}
EXPORT LSTATUS_ WINAPI SHRegWriteUSValueA(USKey *key, LPCSTR value, DWORD type, LPCVOID data, DWORD size, DWORD flags) {
  WCHAR *v = to_wide_dup(value), *d = NULL;
  LSTATUS_ r = ERROR_OUTOFMEMORY;
  if (v || !value) {
    if (type == REG_SZ_ || type == REG_EXPAND_SZ_) { d = to_wide_dup((LPCSTR)data); if (d) r = SHRegWriteUSValueW(key, v, type, d, (DWORD)((wcslen(d) + 1) * sizeof(WCHAR)), flags); }
    else r = SHRegWriteUSValueW(key, v, type, data, size, flags);
  }
  task_free(v); task_free(d);
  return r;
}
EXPORT LSTATUS_ WINAPI SHRegSetUSValueW(LPCWSTR subkey, LPCWSTR value, DWORD type, LPCVOID data, DWORD size, DWORD flags) {
  USKey *key;
  LSTATUS_ r = SHRegCreateUSKeyW(subkey, KEY_ALL_ACCESS_, NULL, &key, flags);
  if (r) return r;
  r = SHRegWriteUSValueW(key, value, type, data, size, flags);
  SHRegCloseUSKey(key);
  return r;
}
EXPORT LSTATUS_ WINAPI SHRegSetUSValueA(LPCSTR subkey, LPCSTR value, DWORD type, LPCVOID data, DWORD size, DWORD flags) {
  WCHAR *k = to_wide_dup(subkey);
  USKey *key;
  LSTATUS_ r = k ? SHRegCreateUSKeyW(k, KEY_ALL_ACCESS_, NULL, &key, flags) : ERROR_OUTOFMEMORY;
  task_free(k);
  if (r) return r;
  r = SHRegWriteUSValueA(key, value, type, data, size, flags);
  SHRegCloseUSKey(key);
  return r;
}
EXPORT LSTATUS_ WINAPI SHRegDeleteUSValueW(USKey *key, LPCWSTR value, DWORD which) {
  LSTATUS_ r = ERROR_FILE_NOT_FOUND;
  if (!key) return ERROR_INVALID_PARAMETER;
  if (key->cu && (which == 0 || which == 1)) r = RegDeleteValueW(key->cu, value);
  if (key->lm && which == 2) r = RegDeleteValueW(key->lm, value);
  return r;
}
EXPORT LSTATUS_ WINAPI SHRegDeleteUSValueA(USKey *key, LPCSTR value, DWORD which) {
  WCHAR *v = to_wide_dup(value);
  const LSTATUS_ r = v ? SHRegDeleteUSValueW(key, v, which) : ERROR_OUTOFMEMORY;
  task_free(v);
  return r;
}
EXPORT LSTATUS_ WINAPI SHRegDeleteEmptyUSKeyW(USKey *key, LPCWSTR subkey, DWORD which) {
  if (!key) return ERROR_INVALID_PARAMETER;
  const HKEY parent = (which == 2) ? key->lm : key->cu;
  return parent ? SHDeleteEmptyKeyW(parent, subkey) : ERROR_FILE_NOT_FOUND;
}
EXPORT LSTATUS_ WINAPI SHRegDeleteEmptyUSKeyA(USKey *key, LPCSTR subkey, DWORD which) {
  WCHAR *k = to_wide_dup(subkey);
  const LSTATUS_ r = k ? SHRegDeleteEmptyUSKeyW(key, k, which) : ERROR_OUTOFMEMORY;
  task_free(k);
  return r;
}
EXPORT LSTATUS_ WINAPI SHRegEnumUSKeyW(USKey *key, DWORD index, LPWSTR name, LPDWORD size, DWORD which) {
  if (!key) return ERROR_INVALID_PARAMETER;
  const HKEY k = (which == 2 && key->lm) ? key->lm : key->cu;
  return k ? RegEnumKeyExW(k, index, name, size, NULL, NULL, NULL, NULL) : ERROR_NO_MORE_ITEMS;
}
EXPORT LSTATUS_ WINAPI SHRegEnumUSKeyA(USKey *key, DWORD index, LPSTR name, LPDWORD size, DWORD which) {
  WCHAR wide[MAX_PATH_ + 1];
  DWORD n = MAX_PATH_;
  LSTATUS_ r = SHRegEnumUSKeyW(key, index, wide, &n, which);
  if (r) return r;
  const int bytes = WideCharToMultiByte(CP_ACP, 0, wide, -1, name, (int)*size, NULL, NULL);
  if (!bytes) return ERROR_MORE_DATA;
  *size = (DWORD)bytes - 1;
  return 0;
}
EXPORT LSTATUS_ WINAPI SHRegEnumUSValueW(USKey *key, DWORD index, LPWSTR name, LPDWORD name_size, LPDWORD type, LPVOID data, LPDWORD size, DWORD which) {
  if (!key) return ERROR_INVALID_PARAMETER;
  const HKEY k = (which == 2 && key->lm) ? key->lm : key->cu;
  return k ? RegEnumValueW(k, index, name, name_size, NULL, type, data, size) : ERROR_NO_MORE_ITEMS;
}
EXPORT LSTATUS_ WINAPI SHRegEnumUSValueA(USKey *key, DWORD index, LPSTR name, LPDWORD name_size, LPDWORD type, LPVOID data, LPDWORD size, DWORD which) {
  WCHAR wide[MAX_PATH_ + 1];
  DWORD n = MAX_PATH_;
  LSTATUS_ r = SHRegEnumUSValueW(key, index, wide, &n, type, data, size, which);
  if (r) return r;
  const int bytes = WideCharToMultiByte(CP_ACP, 0, wide, -1, name, (int)*name_size, NULL, NULL);
  if (!bytes) return ERROR_MORE_DATA;
  *name_size = (DWORD)bytes - 1;
  return 0;
}
EXPORT LSTATUS_ WINAPI SHRegQueryInfoUSKeyW(USKey *key, LPDWORD subkeys, LPDWORD max_subkey, LPDWORD values, LPDWORD max_value, DWORD which) {
  if (!key) return ERROR_INVALID_PARAMETER;
  const HKEY k = (which == 2 && key->lm) ? key->lm : key->cu;
  return k ? SHQueryInfoKeyW(k, subkeys, max_subkey, values, max_value) : ERROR_INVALID_PARAMETER;
}
EXPORT LSTATUS_ WINAPI SHRegQueryInfoUSKeyA(USKey *key, LPDWORD subkeys, LPDWORD max_subkey, LPDWORD values, LPDWORD max_value, DWORD which) {
  return SHRegQueryInfoUSKeyW(key, subkeys, max_subkey, values, max_value, which);
}

/* A boolean the way settings spell it: "YES"/"TRUE"/"1" and "NO"/"FALSE"/"0" (case is ignored); anything else is the fallback. */
static BOOL us_bool_text(const WCHAR *s, BOOL fallback) {
  static const WCHAR *const yes[] = {L"YES", L"TRUE", L"1"}, *const no[] = {L"NO", L"FALSE", L"0"};
  for (int i = 0; i < 3; ++i) {
    if (!StrCmpIW(s, yes[i])) return TRUE;
    if (!StrCmpIW(s, no[i])) return FALSE;
  }
  return fallback;
}
EXPORT BOOL WINAPI SHRegGetBoolUSValueW(LPCWSTR subkey, LPCWSTR value, BOOL ignore_hkcu, BOOL fallback) {
  WCHAR text[MAX_PATH_];
  DWORD size = sizeof(text), type = 0;
  if (SHRegGetUSValueW(subkey, value, &type, text, &size, ignore_hkcu, NULL, 0) || type != REG_SZ_) return fallback;
  return us_bool_text(text, fallback);
}
EXPORT BOOL WINAPI SHRegGetBoolUSValueA(LPCSTR subkey, LPCSTR value, BOOL ignore_hkcu, BOOL fallback) {
  WCHAR *k = to_wide_dup(subkey), *v = to_wide_dup(value);
  BOOL r = fallback;
  if ((k || !subkey) && (v || !value)) r = SHRegGetBoolUSValueW(k, v, ignore_hkcu, fallback);
  task_free(k); task_free(v);
  return r;
}
EXPORT BOOL WINAPI SHRegGetBoolValueFromHKCUHKLM(LPCWSTR subkey, LPCWSTR value, BOOL fallback) { return SHRegGetBoolUSValueW(subkey, value, FALSE, fallback); }
