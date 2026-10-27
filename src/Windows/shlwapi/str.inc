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

/* shlwapi: the part that is written once for CHAR and WCHAR. Included by shlwapi.c with SUF (A or W), CHAR_T, UPC (upper-case a character), UV (the character as an
 * unsigned number), LEN and TXT (a character or string literal of the width) defined. */
#define CAT_(a, b) a##b
#define CAT(a, b) CAT_(a, b)
#define FN(name) CAT(name, SUF)
#define SEP(c) ((c) == '\\' || (c) == '/')

/* ---- comparing and searching ----------------------------------------------------------------------------------------------------------------------------- */

EXPORT int WINAPI FN(StrCmp)(const CHAR_T *a, const CHAR_T *b) {
  while (*a && *a == *b) { ++a; ++b; }
  return (int)UV(*a) - (int)UV(*b);
}
EXPORT int WINAPI FN(StrCmpI)(const CHAR_T *a, const CHAR_T *b) {
  while (*a && UPC(*a) == UPC(*b)) { ++a; ++b; }
  return (int)UV(UPC(*a)) - (int)UV(UPC(*b));
}
EXPORT int WINAPI FN(StrCmpN)(const CHAR_T *a, const CHAR_T *b, int n) {
  if (n <= 0) return 0;
  while (--n && *a && *a == *b) { ++a; ++b; }
  return (int)UV(*a) - (int)UV(*b);
}
EXPORT int WINAPI FN(StrCmpNI)(const CHAR_T *a, const CHAR_T *b, int n) {
  if (n <= 0) return 0;
  while (--n && *a && UPC(*a) == UPC(*b)) { ++a; ++b; }
  return (int)UV(UPC(*a)) - (int)UV(UPC(*b));
}
EXPORT BOOL WINAPI FN(IntlStrEqWorker)(BOOL case_sensitive, const CHAR_T *a, const CHAR_T *b, int n) {
  return case_sensitive ? FN(StrCmpN)(a, b, n) == 0 : FN(StrCmpNI)(a, b, n) == 0;
}
EXPORT BOOL WINAPI FN(StrIsIntlEqual)(BOOL case_sensitive, const CHAR_T *a, const CHAR_T *b, int n) { return FN(IntlStrEqWorker)(case_sensitive, a, b, n); }

EXPORT CHAR_T *WINAPI FN(StrChr)(const CHAR_T *s, CHAR_T c) {
  if (!s) return NULL;
  for (; *s; ++s) if (*s == c) return (CHAR_T *)s;
  return NULL;
}
EXPORT CHAR_T *WINAPI FN(StrChrI)(const CHAR_T *s, CHAR_T c) {
  if (!s) return NULL;
  for (; *s; ++s) if (UPC(*s) == UPC(c)) return (CHAR_T *)s;
  return NULL;
}
EXPORT CHAR_T *WINAPI FN(StrRChr)(const CHAR_T *s, const CHAR_T *end, CHAR_T c) {
  CHAR_T *found = NULL;
  if (!s) return NULL;
  if (!end) end = s + LEN(s);
  for (; *s && s <= end; ++s) if (*s == c) found = (CHAR_T *)s;
  return found;
}
EXPORT CHAR_T *WINAPI FN(StrRChrI)(const CHAR_T *s, const CHAR_T *end, CHAR_T c) {
  CHAR_T *found = NULL;
  if (!s) return NULL;
  if (!end) end = s + LEN(s);
  for (; *s && s <= end; ++s) if (UPC(*s) == UPC(c)) found = (CHAR_T *)s;
  return found;
}
EXPORT CHAR_T *WINAPI FN(StrStr)(const CHAR_T *s, const CHAR_T *find) {
  if (!s || !find || !*find) return NULL;
  for (; *s; ++s) {
    const CHAR_T *a = s, *b = find;
    while (*a && *b && *a == *b) { ++a; ++b; }
    if (!*b) return (CHAR_T *)s;
  }
  return NULL;
}
EXPORT CHAR_T *WINAPI FN(StrStrI)(const CHAR_T *s, const CHAR_T *find) {
  if (!s || !find || !*find) return NULL;
  for (; *s; ++s) {
    const CHAR_T *a = s, *b = find;
    while (*a && *b && UPC(*a) == UPC(*b)) { ++a; ++b; }
    if (!*b) return (CHAR_T *)s;
  }
  return NULL;
}
EXPORT CHAR_T *WINAPI FN(StrRStrI)(const CHAR_T *s, const CHAR_T *end, const CHAR_T *find) {
  CHAR_T *found = NULL;
  if (!s || !find || !*find) return NULL;
  const size_t_ n = LEN(find);
  if (!end) end = s + LEN(s);
  for (; s < end + n && *s; ++s) {
    size_t_ k = 0;
    while (k < n && s[k] && UPC(s[k]) == UPC(find[k])) ++k;
    if (k == n && s <= end) found = (CHAR_T *)s;
  }
  return found;
}
EXPORT CHAR_T *WINAPI FN(StrPBrk)(const CHAR_T *s, const CHAR_T *set) {
  if (!s || !set) return NULL;
  for (; *s; ++s) for (const CHAR_T *p = set; *p; ++p) if (*s == *p) return (CHAR_T *)s;
  return NULL;
}
EXPORT int WINAPI FN(StrCSpn)(const CHAR_T *s, const CHAR_T *set) {
  if (!s || !set) return 0;
  int n = 0;
  for (; s[n]; ++n) for (const CHAR_T *p = set; *p; ++p) if (s[n] == *p) return n;
  return n;
}
EXPORT int WINAPI FN(StrCSpnI)(const CHAR_T *s, const CHAR_T *set) {
  if (!s || !set) return 0;
  int n = 0;
  for (; s[n]; ++n) for (const CHAR_T *p = set; *p; ++p) if (UPC(s[n]) == UPC(*p)) return n;
  return n;
}
EXPORT int WINAPI FN(StrSpn)(const CHAR_T *s, const CHAR_T *set) {
  if (!s || !set) return 0;
  int n = 0;
  for (; s[n]; ++n) {
    int in = 0;
    for (const CHAR_T *p = set; *p; ++p) if (s[n] == *p) { in = 1; break; }
    if (!in) break;
  }
  return n;
}

/* ---- numbers --------------------------------------------------------------------------------------------------------------------------------------------- */

EXPORT int WINAPI FN(StrToInt)(const CHAR_T *s) {
  if (!s) return 0;
  int neg = 0;
  if (*s == '-') { neg = 1; ++s; }      /* (a '+' is not a sign here) */
  unsigned v = 0;
  while (*s >= '0' && *s <= '9') v = v * 10 + (unsigned)(*s++ - '0');
  return (int)(neg ? 0 - v : v);
}
#define STIF_SUPPORT_HEX_ 1
EXPORT BOOL WINAPI FN(StrToInt64Ex)(const CHAR_T *s, DWORD flags, INT64_ *out) {
  if (!s || !out) return FALSE;
  while (*s == ' ' || (*s >= 9 && *s <= 13)) ++s;
  int neg = 0;
  if (*s == '-') { neg = 1; ++s; } else if (*s == '+') ++s;
  ULONGLONG v = 0;
  if ((flags & STIF_SUPPORT_HEX_) && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
    s += 2;
    int any = 0;
    for (;; ++s) {
      const unsigned c = UV(*s);
      const int d = c >= '0' && c <= '9' ? (int)(c - '0') : c >= 'a' && c <= 'f' ? (int)(c - 'a' + 10) : c >= 'A' && c <= 'F' ? (int)(c - 'A' + 10) : -1;
      if (d < 0) break;
      v = v * 16 + (ULONGLONG)d;
      any = 1;
    }
    if (!any) return FALSE;
    *out = (INT64_)(neg ? 0 - v : v);
    return TRUE;
  }
  if (!(*s >= '0' && *s <= '9')) return FALSE;
  while (*s >= '0' && *s <= '9') v = v * 10 + (ULONGLONG)(*s++ - '0');
  *out = (INT64_)(neg ? 0 - v : v);
  return TRUE;
}
EXPORT BOOL WINAPI FN(StrToIntEx)(const CHAR_T *s, DWORD flags, int *out) {
  INT64_ v;
  if (!out || !FN(StrToInt64Ex)(s, flags, &v)) return FALSE;
  *out = (int)v;
  return TRUE;
}

/* ---- copying and concatenating --------------------------------------------------------------------------------------------------------------------------- */

EXPORT CHAR_T *WINAPI FN(StrDup)(const CHAR_T *s) {
  const size_t_ n = s ? LEN(s) : 0;
  CHAR_T *copy = LocalAlloc(0, (n + 1) * sizeof(CHAR_T));
  if (!copy) return NULL;
  if (n) memcpy(copy, s, n * sizeof(CHAR_T));
  copy[n] = 0;
  return copy;
}
EXPORT CHAR_T *WINAPI FN(StrCpyN)(CHAR_T *dst, const CHAR_T *src, int n) {
  if (!dst || n <= 0) return dst;
  int i = 0;
  if (src) for (; i < n - 1 && src[i]; ++i) dst[i] = src[i];
  dst[i] = 0;
  return dst;
}
EXPORT CHAR_T *WINAPI FN(StrCpyNX)(CHAR_T *dst, const CHAR_T *src, int n) {
  if (!dst || n <= 0) return dst;
  int i = 0;
  if (src) for (; i < n - 1 && src[i]; ++i) dst[i] = src[i];
  dst[i] = 0;
  return dst + i;
}
EXPORT CHAR_T *WINAPI FN(StrCat)(CHAR_T *dst, const CHAR_T *src) {
  CHAR_T *p = dst + LEN(dst);
  while ((*p++ = *src++)) {}
  return dst;
}
EXPORT CHAR_T *WINAPI FN(StrNCat)(CHAR_T *dst, const CHAR_T *src, int n) {
  CHAR_T *p = dst + LEN(dst);
  while (n-- > 0 && *src) *p++ = *src++;
  *p = 0;
  return dst;
}
EXPORT CHAR_T *WINAPI FN(StrCatBuff)(CHAR_T *dst, const CHAR_T *src, int size) {
  if (!dst || size <= 0) return dst;
  int n = (int)LEN(dst);
  while (n < size - 1 && *src) dst[n++] = *src++;
  dst[n] = 0;
  return dst;
}
EXPORT BOOL WINAPI FN(StrTrim)(CHAR_T *s, const CHAR_T *trim) {
  if (!s || !*s) return FALSE;
  CHAR_T *start = s;
  while (*start && FN(StrChr)(trim, *start)) ++start;
  CHAR_T *end = start + LEN(start);
  while (end > start && FN(StrChr)(trim, end[-1])) --end;
  const int changed = start != s || *end;
  if (start != s) memmove(s, start, (size_t_)(end - start) * sizeof(CHAR_T));
  s[end - start] = 0;
  return changed;
}

/* ---- paths ------------------------------------------------------------------------------------------------------------------------------------------------- */

EXPORT CHAR_T *WINAPI FN(PathFindExtension)(const CHAR_T *path) {
  const CHAR_T *dot = NULL;
  if (!path) return NULL;
  for (; *path; ++path) {
    if (*path == '.') dot = path;
    else if (*path == '\\' || *path == '/' || *path == ' ') dot = NULL;
  }
  return (CHAR_T *)(dot ? dot : path);
}
EXPORT CHAR_T *WINAPI FN(PathFindFileName)(const CHAR_T *path) {
  const CHAR_T *last = path;
  if (!path) return NULL;
  for (; *path; ++path) if ((*path == '\\' || *path == '/' || *path == ':') && path[1] && path[1] != '\\' && path[1] != '/') last = path + 1;
  return (CHAR_T *)last;
}
EXPORT CHAR_T *WINAPI FN(PathFindNextComponent)(const CHAR_T *path) {
  if (!path || !*path) return NULL;
  if (path[0] == '\\' && path[1] == '\\') path += 2;
  else if (path[0] == '\\') ++path;
  while (*path && *path != '\\') ++path;
  if (!*path) return (CHAR_T *)path;
  return (CHAR_T *)(path + 1);
}
EXPORT CHAR_T *WINAPI FN(PathGetArgs)(const CHAR_T *path) {
  int quoted = 0;
  if (!path) return NULL;
  for (; *path; ++path) {
    if (*path == '"') quoted = !quoted;
    else if (*path == ' ' && !quoted) { while (path[1] == ' ') ++path; return (CHAR_T *)(path + 1); }
  }
  return (CHAR_T *)path;
}
EXPORT int WINAPI FN(PathGetDriveNumber)(const CHAR_T *path) {
  if (path && path[0] && path[1] == ':') {
    const unsigned c = UV(UPC(path[0]));
    if (c >= 'A' && c <= 'Z') return (int)(c - 'A');
  }
  return -1;
}
EXPORT BOOL WINAPI FN(PathIsUNC)(const CHAR_T *p) { return p && p[0] == '\\' && p[1] == '\\'; }
EXPORT BOOL WINAPI FN(PathIsUNCServer)(const CHAR_T *p) {
  if (!p || p[0] != '\\' || p[1] != '\\' || !p[2]) return FALSE;
  for (p += 2; *p; ++p) if (*p == '\\') return FALSE;
  return TRUE;
}
EXPORT BOOL WINAPI FN(PathIsUNCServerShare)(const CHAR_T *p) {
  if (!p || p[0] != '\\' || p[1] != '\\' || !p[2]) return FALSE;
  int slashes = 0;
  for (p += 2; *p; ++p) if (*p == '\\' && ++slashes > 1) return FALSE;
  return slashes == 1;
}
EXPORT BOOL WINAPI FN(PathIsRelative)(const CHAR_T *p) {
  if (!p || !*p) return TRUE;
  if (*p == '\\' || *p == '/') return FALSE;
  if (p[1] == ':' && (p[2] == '\\' || p[2] == '/')) return FALSE;
  return TRUE;
}
EXPORT BOOL WINAPI FN(PathIsRoot)(const CHAR_T *p) {
  if (!p || !*p) return FALSE;
  if (p[1] == ':' && p[2] == '\\' && !p[3]) return TRUE;
  if (p[0] == '\\') {
    if (!p[1]) return TRUE;
    if (p[1] == '\\') {      /* \\server or \\server\share */
      int slashes = 0;
      for (const CHAR_T *q = p + 2; *q; ++q) if (*q == '\\') { if (++slashes > 1) return FALSE; if (!q[1]) return FALSE; }
      return slashes <= 1;
    }
  }
  return FALSE;
}
EXPORT BOOL WINAPI FN(PathIsFileSpec)(const CHAR_T *p) {
  if (!p) return FALSE;
  for (; *p; ++p) if (*p == '\\' || *p == ':') return FALSE;
  return TRUE;
}
EXPORT BOOL WINAPI FN(PathIsPrefix)(const CHAR_T *prefix, const CHAR_T *path) {
  const size_t_ n = LEN(prefix);
  if (!n || FN(StrCmpNI)(prefix, path, (int)n)) return FALSE;
  return TRUE;
}
EXPORT CHAR_T *WINAPI FN(PathSkipRoot)(const CHAR_T *p) {
  if (!p) return NULL;
  if (p[0] == '\\' && p[1] == '\\') {
    p += 2;
    while (*p && *p != '\\') ++p;
    if (*p) ++p;
    while (*p && *p != '\\') ++p;
    if (*p) ++p;
    return (CHAR_T *)p;
  }
  if (p[0] && p[1] == ':' && p[2] == '\\') return (CHAR_T *)(p + 3);
  if (p[0] == '\\') return (CHAR_T *)(p + 1);
  return NULL;
}
EXPORT void WINAPI FN(PathStripPath)(CHAR_T *p) {
  if (!p) return;
  CHAR_T *name = FN(PathFindFileName)(p);
  if (name != p) memmove(p, name, (LEN(name) + 1) * sizeof(CHAR_T));
}
EXPORT BOOL WINAPI FN(PathRemoveFileSpec)(CHAR_T *path);
EXPORT BOOL WINAPI FN(PathStripToRoot)(CHAR_T *p) {
  if (!p) return FALSE;
  while (!FN(PathIsRoot)(p)) if (!FN(PathRemoveFileSpec)(p)) return FALSE;
  return TRUE;
}
EXPORT BOOL WINAPI FN(PathRemoveFileSpec)(CHAR_T *path) {
  if (!path) return FALSE;
  CHAR_T *file = FN(PathFindFileName)(path);
  if (file == path && !path[0]) return FALSE;
  if (file == path) {      /* only a file name, or a root */
    if (path[0] == '\\' && path[1] == 0) return FALSE;
    *path = 0;
    return TRUE;
  }
  if (file[0]) {
    CHAR_T *cut = file - 1;
    if (cut == path + 1 && path[1] == ':') ++cut;      /* "C:\file" keeps the root's backslash */
    if (cut == path && *cut == '\\') ++cut;
    *cut = 0;
    return TRUE;
  }
  /* ends in a backslash: remove it unless it is the root's */
  CHAR_T *end = path + LEN(path) - 1;
  if (end > path && !(end == path + 2 && path[1] == ':') && *end == '\\') { *end = 0; return TRUE; }
  return FALSE;
}
EXPORT CHAR_T *WINAPI FN(PathRemoveBackslash)(CHAR_T *path) {
  if (!path) return NULL;
  const size_t_ n = LEN(path);
  if (!n) return path;
  CHAR_T *end = path + n - 1;
  if (*end == '\\' && !(end == path + 2 && path[1] == ':') && end != path) *end = 0;
  return *end ? end + 1 : end;
}
EXPORT CHAR_T *WINAPI FN(PathAddBackslash)(CHAR_T *path) {
  if (!path) return NULL;
  const size_t_ n = LEN(path);
  if (n >= MAX_PATH_ - 1) return NULL;
  if (!n) return path;
  if (path[n - 1] != '\\') { path[n] = '\\'; path[n + 1] = 0; return path + n + 1; }
  return path + n;
}
EXPORT void WINAPI FN(PathRemoveExtension)(CHAR_T *path) {
  if (!path) return;
  CHAR_T *ext = FN(PathFindExtension)(path);
  if (*ext) *ext = 0;
}
EXPORT BOOL WINAPI FN(PathRenameExtension)(CHAR_T *path, const CHAR_T *ext) {
  if (!path || !ext) return FALSE;
  CHAR_T *dot = FN(PathFindExtension)(path);
  if ((size_t_)(dot - path) + LEN(ext) >= MAX_PATH_) return FALSE;
  memcpy(dot, ext, (LEN(ext) + 1) * sizeof(CHAR_T));
  return TRUE;
}
EXPORT BOOL WINAPI FN(PathAddExtension)(CHAR_T *path, const CHAR_T *ext) {
  if (!path) return FALSE;
  if (*FN(PathFindExtension)(path)) return TRUE;
  static const CHAR_T exe[] = {'.', 'e', 'x', 'e', 0};
  if (!ext) ext = exe;
  if (LEN(path) + LEN(ext) >= MAX_PATH_) return FALSE;
  memcpy(path + LEN(path), ext, (LEN(ext) + 1) * sizeof(CHAR_T));
  return TRUE;
}
EXPORT void WINAPI FN(PathRemoveArgs)(CHAR_T *path) {
  if (!path) return;
  CHAR_T *args = FN(PathGetArgs)(path);
  if (*args) { if (args > path && args[-1] == ' ') --args; *args = 0; }
}
EXPORT void WINAPI FN(PathRemoveBlanks)(CHAR_T *path) {
  if (!path || !*path) return;
  CHAR_T *start = path;
  while (*start == ' ') ++start;
  CHAR_T *end = start + LEN(start);
  while (end > start && end[-1] == ' ') --end;
  if (start != path) memmove(path, start, (size_t_)(end - start) * sizeof(CHAR_T));
  path[end - start] = 0;
}
EXPORT void WINAPI FN(PathQuoteSpaces)(CHAR_T *path) {
  if (!path) return;
  const size_t_ n = LEN(path);
  int space = 0;
  for (size_t_ i = 0; i < n; ++i) if (path[i] == ' ') space = 1;
  if (!space || n + 2 >= MAX_PATH_) return;
  memmove(path + 1, path, n * sizeof(CHAR_T));
  path[0] = '"';
  path[n + 1] = '"';
  path[n + 2] = 0;
}
EXPORT void WINAPI FN(PathUnquoteSpaces)(CHAR_T *path) {
  if (!path) return;
  const size_t_ n = LEN(path);
  if (n >= 2 && path[0] == '"' && path[n - 1] == '"') {
    memmove(path, path + 1, (n - 2) * sizeof(CHAR_T));
    path[n - 2] = 0;
  }
}
EXPORT BOOL WINAPI FN(PathBuildRoot)(CHAR_T *root, int drive) {
  if (!root || drive < 0 || drive > 25) return FALSE;
  root[0] = (CHAR_T)('A' + drive); root[1] = ':'; root[2] = '\\'; root[3] = 0;
  return TRUE;
}
EXPORT BOOL WINAPI FN(PathMakePretty)(CHAR_T *path) {
  if (!path || !*path) return FALSE;
  for (CHAR_T *p = path; *p; ++p) if (UV(*p) >= 'a' && UV(*p) <= 'z') return FALSE;
  for (CHAR_T *p = path; *p; ++p) if (UV(*p) >= 'A' && UV(*p) <= 'Z') *p = (CHAR_T)(UV(*p) + 32);
  return TRUE;
}
EXPORT BOOL WINAPI FN(PathFileExists)(const CHAR_T *path) {
  if (!path || !*path) return FALSE;
  return CAT(GetFileAttributes, SUF)(path) != 0xFFFFFFFFu;
}
EXPORT BOOL WINAPI FN(PathIsDirectory)(const CHAR_T *path) {
  if (!path || !*path) return FALSE;
  const DWORD a = CAT(GetFileAttributes, SUF)(path);
  return a != 0xFFFFFFFFu && (a & FILE_ATTRIBUTE_DIRECTORY_);
}
EXPORT BOOL WINAPI FN(PathIsSameRoot)(const CHAR_T *a, const CHAR_T *b) {
  if (!a || !b) return FALSE;
  const CHAR_T *ra = FN(PathSkipRoot)(a), *rb = FN(PathSkipRoot)(b);
  if (!ra || !rb) return FALSE;
  return ra - a == rb - b && FN(StrCmpNI)(a, b, (int)(ra - a)) == 0;
}

/* Collapses "." and ".." and repeated separators; the root (a drive, a server and share, or a backslash) is kept. Returns the length, or -1 when it does not fit. */
static int CAT(canonical_, SUF)(const CHAR_T *src, CHAR_T *out, int capacity) {
  CHAR_T work[MAX_PATH_ + 4];
  int n = 0, root = 0;
  if (!*src) { work[n++] = '\\'; root = 1; }
  else if (SEP(src[0]) && SEP(src[1])) { work[n++] = '\\'; work[n++] = '\\'; src += 2; root = 2; }
  else if (SEP(src[0])) { work[n++] = '\\'; ++src; root = 1; }
  else if (src[0] && src[1] == ':') {
    work[n++] = src[0]; work[n++] = ':'; src += 2;
    if (SEP(*src)) { work[n++] = '\\'; ++src; }
    root = n;
  }
  int trailing_sep = 0;
  while (*src) {
    if (SEP(*src)) { trailing_sep = 1; ++src; continue; }
    trailing_sep = 0;
    const CHAR_T *start = src;
    while (*src && !SEP(*src)) ++src;
    const int len = (int)(src - start);
    if (len == 1 && start[0] == '.') { trailing_sep = SEP(*src) || !*src ? 0 : 0; continue; }
    if (len == 2 && start[0] == '.' && start[1] == '.') {
      if (n > root) {
        if (work[n - 1] == '\\') --n;
        while (n > root && work[n - 1] != '\\') --n;
      }
      continue;
    }
    if (n > root && work[n - 1] != '\\' ) work[n++] = '\\';
    else if (n > root && n && work[n - 1] == '\\' && n == root) {}
    if (n + len >= MAX_PATH_) return -1;
    for (int k = 0; k < len; ++k) work[n++] = start[k];
    if (SEP(*src)) trailing_sep = 1;
  }
  if (trailing_sep && n && work[n - 1] != '\\' && n + 1 < MAX_PATH_) work[n++] = '\\';
  if (n >= capacity) return -1;
  work[n] = 0;
  memcpy(out, work, ((size_t_)n + 1) * sizeof(CHAR_T));
  return n;
}
EXPORT BOOL WINAPI FN(PathCanonicalize)(CHAR_T *dst, const CHAR_T *src) {
  if (!dst) return FALSE;
  *dst = 0;
  if (!src) { SetLastError(ERROR_INVALID_PARAMETER_); return FALSE; }
  return CAT(canonical_, SUF)(src, dst, MAX_PATH_) >= 0;
}
EXPORT CHAR_T *WINAPI FN(PathCombine)(CHAR_T *dst, const CHAR_T *dir, const CHAR_T *file) {
  CHAR_T work[MAX_PATH_ * 2];
  if (!dst) return NULL;
  *dst = 0;
  if ((!dir || !*dir) && (!file || !*file)) return NULL;
  const CHAR_T *use_file = file && *file ? file : NULL;
  if (!use_file) {      /* only a directory */
    return CAT(canonical_, SUF)(dir, dst, MAX_PATH_) >= 0 ? dst : (*dst = 0, (CHAR_T *)NULL);
  }
  if (!dir || !*dir || !FN(PathIsRelative)(use_file) || (SEP(use_file[0]) && SEP(use_file[1]))) {
    if (dir && *dir && FN(PathIsRelative)(use_file) == 0 && 0) {}
    /* a rooted file ("\\b") takes the drive of the directory */
    if (dir && *dir && use_file[0] == '\\' && use_file[1] != '\\' && dir[1] == ':') {
      work[0] = dir[0]; work[1] = ':';
      size_t_ k = 0;
      while (use_file[k] && k + 2 < MAX_PATH_ * 2 - 1) { work[2 + k] = use_file[k]; ++k; }
      work[2 + k] = 0;
      return CAT(canonical_, SUF)(work, dst, MAX_PATH_) >= 0 ? dst : (*dst = 0, (CHAR_T *)NULL);
    }
    return CAT(canonical_, SUF)(use_file, dst, MAX_PATH_) >= 0 ? dst : (*dst = 0, (CHAR_T *)NULL);
  }
  size_t_ n = 0;
  for (const CHAR_T *p = dir; *p && n < MAX_PATH_ * 2 - 3; ++p) work[n++] = *p;
  if (n && work[n - 1] != '\\' && work[n - 1] != '/') work[n++] = '\\';
  for (const CHAR_T *p = use_file; *p && n < MAX_PATH_ * 2 - 1; ++p) work[n++] = *p;
  work[n] = 0;
  return CAT(canonical_, SUF)(work, dst, MAX_PATH_) >= 0 ? dst : (*dst = 0, (CHAR_T *)NULL);
}
EXPORT BOOL WINAPI FN(PathAppend)(CHAR_T *path, const CHAR_T *more) {
  if (!path || !more) return FALSE;
  while (*more == '\\') ++more;      /* (a leading backslash would make "more" absolute) */
  CHAR_T work[MAX_PATH_];
  if (!FN(PathCombine)(work, path, more)) return FALSE;
  memcpy(path, work, (LEN(work) + 1) * sizeof(CHAR_T));
  return TRUE;
}
static BOOL CAT(wild_, SUF)(const CHAR_T *p, const CHAR_T *s) {
  while (*p) {
    if (*p == '*') {
      while (*p == '*') ++p;
      if (!*p) return TRUE;
      for (; *s; ++s) if (CAT(wild_, SUF)(p, s)) return TRUE;
      return CAT(wild_, SUF)(p, s);
    }
    if (!*s) return FALSE;
    if (*p != '?' && UPC(*p) != UPC(*s)) return FALSE;
    ++p; ++s;
  }
  return !*s;
}
EXPORT BOOL WINAPI FN(PathMatchSpec)(const CHAR_T *path, const CHAR_T *spec) {
  if (!path || !spec) return FALSE;
  if (!*spec) return TRUE;
  CHAR_T pattern[MAX_PATH_];
  while (*spec) {
    while (*spec == ' ') ++spec;
    size_t_ n = 0;
    while (*spec && *spec != ';' && n + 1 < MAX_PATH_) pattern[n++] = *spec++;
    pattern[n] = 0;
    if (*spec == ';') ++spec;
    if (n && CAT(wild_, SUF)(pattern, path)) return TRUE;
  }
  return FALSE;
}
