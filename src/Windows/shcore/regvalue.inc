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

/* The registry-value helpers written once for narrow and wide characters (SUF = A or W, CH the character type); included twice by shcore.c.
 * SHQueryValueEx reads a value like RegQueryValueEx but returns an expandable string expanded (with the type REG_SZ); the size it reports is the larger of the stored
 * and the expanded size, so that a buffer of that size fits both. */
#define QV_PASTE2(a, b) a##b
#define QV_PASTE(a, b) QV_PASTE2(a, b)
#define QV(name) QV_PASTE(name, SUF)

EXPORT LSTATUS WINAPI QV(SHQueryValueEx)(HKEY key, const CH *value, DWORD *reserved, DWORD *type, void *data, DWORD *size) {
  DWORD t = 0, raw = 0;
  (void)reserved;
  LSTATUS r = QV(RegQueryValueEx)(key, value, NULL, &t, NULL, &raw);
  if (r != ERROR_SUCCESS) return r;
  if (t != REG_EXPAND_SZ) return QV(RegQueryValueEx)(key, value, NULL, type, data, size);
  if (type) *type = REG_SZ;
  if (data && !size) return ERROR_INVALID_PARAMETER;
  if (!data && !size) return ERROR_SUCCESS;
  CH *text = mem_alloc((SIZE_T)raw + 2 * sizeof(CH));
  if (!text) return ERROR_NOT_ENOUGH_MEMORY;
  DWORD got = raw;
  r = QV(RegQueryValueEx)(key, value, NULL, NULL, (BYTE *)text, &got);
  if (r == ERROR_SUCCESS) {
    text[got / sizeof(CH)] = 0;
    const DWORD expanded = QV(ExpandEnvironmentStrings)(text, NULL, 0) * (DWORD)sizeof(CH);
    const DWORD need = expanded > raw ? expanded : raw;
    if (!data) *size = need;
    else if (*size < need) {
      if (*size >= raw && *size >= sizeof(CH)) ((CH *)data)[0] = 0;      /* (the stored text fitted: what was cut is not kept) */
      *size = need;
      r = ERROR_MORE_DATA;
    } else {
      QV(ExpandEnvironmentStrings)(text, data, *size / (DWORD)sizeof(CH));
      *size = expanded;
    }
  }
  mem_free(text);
  return r;
}

EXPORT LSTATUS WINAPI QV(SHGetValue)(HKEY root, const CH *subkey, const CH *value, DWORD *type, void *data, DWORD *size) {
  HKEY key = root;
  LSTATUS r = ERROR_SUCCESS;
  if (subkey && *subkey) r = QV(RegOpenKeyEx)(root, subkey, 0, KEY_QUERY_VALUE, &key);
  if (r != ERROR_SUCCESS) return r;
  r = QV(SHQueryValueEx)(key, value, NULL, type, data, size);
  if (key != root) RegCloseKey(key);
  return r;
}

EXPORT LSTATUS WINAPI QV(SHRegGetValue)(HKEY root, const CH *subkey, const CH *value, DWORD flags, DWORD *type, void *data, DWORD *size) {
  HKEY key = root;
  DWORD t = 0, raw = 0;
  LSTATUS r = ERROR_SUCCESS;
  if (data && !size) return ERROR_INVALID_PARAMETER;
  /* an expandable string can only be asked for as such together with "do not expand" */
  if ((flags & 0xFFFFu) == RRF_RT_REG_EXPAND_SZ && !(flags & RRF_NOEXPAND)) return ERROR_INVALID_PARAMETER;
  if (subkey && *subkey) r = QV(RegOpenKeyEx)(root, subkey, 0, KEY_QUERY_VALUE, &key);
  if (r == ERROR_SUCCESS) {
    r = QV(RegQueryValueEx)(key, value, NULL, &t, NULL, &raw);
    if (r == ERROR_SUCCESS) {
      static const DWORD kBits[] = {RRF_RT_REG_NONE, RRF_RT_REG_SZ, RRF_RT_REG_EXPAND_SZ, RRF_RT_REG_BINARY, RRF_RT_REG_DWORD, 0, 0, 0, RRF_RT_REG_MULTI_SZ, 0, 0, RRF_RT_REG_QWORD};
      const DWORD bit = t < sizeof(kBits) / sizeof(kBits[0]) ? kBits[t] : 0;
      const BOOL expand = t == REG_EXPAND_SZ && !(flags & RRF_NOEXPAND) && (flags & RRF_RT_REG_SZ);
      if (!(flags & bit) && !expand) r = ERROR_UNSUPPORTED_TYPE;
      else if (expand) r = QV(SHQueryValueEx)(key, value, NULL, type, data, size);
      else r = QV(RegQueryValueEx)(key, value, NULL, type, data, size);
    }
    if (key != root) RegCloseKey(key);
  }
  if (r != ERROR_SUCCESS && r != ERROR_MORE_DATA && (flags & RRF_ZEROONFAILURE) && data && size) mem_zero(data, *size);
  return r;
}

EXPORT LSTATUS WINAPI QV(SHRegGetPath)(HKEY root, const CH *subkey, const CH *value, CH *path, DWORD flags) {
  DWORD size = MAX_PATH * (DWORD)sizeof(CH);
  (void)flags;
  return QV(SHGetValue)(root, subkey, value, NULL, path, &size);
}

#undef QV
