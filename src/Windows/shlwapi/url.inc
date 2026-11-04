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
/* The Url* functions of shlwapi.dll (included by shlwapi.c, after the path functions). Everything works on wide strings; the ANSI functions convert in and out.
   Checked with Wine's tests (tests/windows/wine/shlwapi/url.c). */

#define URL_MAX 2084
#define E_FAIL__ ((HRESULT_)0x80004005)
#define URL_E_INVALID_SYNTAX_ ((HRESULT_)0x80041001)

#define F_ESCAPE_PERCENT 0x00001000
#define F_ESCAPE_SEGMENT_ONLY 0x00002000
#define F_ESCAPE_AS_UTF8 0x00040000
#define F_DONT_ESCAPE_EXTRA_INFO 0x02000000
#define F_ESCAPE_SPACES_ONLY 0x04000000
#define F_ESCAPE_UNSAFE 0x20000000
#define F_DONT_SIMPLIFY 0x08000000
#define F_UNESCAPE 0x10000000
#define F_UNESCAPE_INPLACE 0x00100000
#define F_UNESCAPE_AS_UTF8 0x00040000
#define F_FILE_USE_PATHURL 0x00010000

typedef struct { const char *name; int scheme; int opaque; } UrlScheme;
static const UrlScheme g_schemes[] = {
  {"ftp", 1, 0}, {"http", 2, 0}, {"gopher", 3, 0}, {"mailto", 4, 1}, {"news", 5, 0}, {"nntp", 6, 0}, {"telnet", 7, 0}, {"wais", 8, 0}, {"file", 9, 0},
  {"mk", 10, 0}, {"https", 11, 0}, {"shell", 12, 1}, {"snews", 13, 0}, {"local", 14, 0}, {"javascript", 15, 1}, {"vbscript", 16, 1}, {"about", 17, 1}, {"res", 18, 0},
};

/* The length of the scheme at the start of `url` (two or more of letters, digits, + - .) followed by a colon, else 0. */
static int url_scheme_length(const WCHAR *url) {
  int i = 0;
  while (url[i] && (((url[i] | 0x20) >= 'a' && (url[i] | 0x20) <= 'z') || (url[i] >= '0' && url[i] <= '9') || url[i] == '+' || url[i] == '-' || url[i] == '.')) ++i;
  return i >= 2 && url[i] == ':' ? i : 0;
}
static const UrlScheme *url_find_scheme(const WCHAR *url, int length) {
  for (unsigned i = 0; i < sizeof(g_schemes) / sizeof(g_schemes[0]); ++i) {
    const char *n = g_schemes[i].name;
    int k = 0;
    while (k < length && n[k] && up_char(url[k]) == up_char((unsigned char)n[k])) ++k;
    if (k == length && !n[k]) return &g_schemes[i];
  }
  return NULL;
}
EXPORT HRESULT_ WINAPI UrlCreateFromPathW(LPCWSTR path, LPWSTR out, DWORD *length, DWORD reserved);
static int url_is_file(const WCHAR *url) { const int n = url_scheme_length(url); const UrlScheme *s = n ? url_find_scheme(url, n) : NULL; return s && s->scheme == 9; }

/* ---- output --------------------------------------------------------------------------------------------------------------------------------------------------- */

static HRESULT_ url_return(const WCHAR *text, int n, WCHAR *out, DWORD *length) {
  if (!out || !length) return E_INVALIDARG_;
  if ((DWORD)n + 1 > *length) { *length = (DWORD)n + 1; return E_POINTER_; }
  for (int i = 0; i < n; ++i) out[i] = text[i];
  out[n] = 0;
  *length = (DWORD)n;
  return 0;
}
static int url_wide(LPCSTR a, WCHAR *w, int capacity) { return MultiByteToWideChar(CP_ACP, 0, a, -1, w, capacity); }
static HRESULT_ url_return_a(const WCHAR *text, int n, LPSTR out, DWORD *length, HRESULT_ hr) {
  if (!out || !length) return E_INVALIDARG_;
  char narrow[URL_MAX * 3];
  WCHAR copy[URL_MAX + 1];
  if (n > URL_MAX) n = URL_MAX;
  for (int i = 0; i < n; ++i) copy[i] = text[i];
  copy[n] = 0;
  const int bytes = WideCharToMultiByte(CP_ACP, 0, copy, -1, narrow, sizeof(narrow), NULL, NULL);
  if (bytes <= 0) return E_FAIL__;
  if ((DWORD)bytes > *length) { *length = (DWORD)bytes; return E_POINTER_; }
  for (int i = 0; i < bytes; ++i) out[i] = narrow[i];
  *length = (DWORD)bytes - 1;
  return hr;
}

/* ---- escaping --------------------------------------------------------------------------------------------------------------------------------------------- */

static int url_unsafe(unsigned c) { return c <= 0x20 || c == 0x7f || c == '<' || c == '>' || c == '"' || c == '{' || c == '}' || c == '|' || c == '\\' || c == '^' || c == ']' || c == '[' || c == '`' || c == '&'; }
static int url_emit_escaped(WCHAR *out, int n, unsigned byte) {
  static const char hex[] = "0123456789ABCDEF";
  out[n++] = '%'; out[n++] = (WCHAR)hex[byte >> 4]; out[n++] = (WCHAR)hex[byte & 15];
  return n;
}
/* Escapes one character: as UTF-8 bytes (a surrogate pair is one character, a lone surrogate U+FFFD) or as itself when it is not above 0x7f. */
static int url_emit_utf8(WCHAR *out, int n, const WCHAR **text) {
  unsigned c = **text;
  if (c >= 0xD800 && c <= 0xDBFF && (*text)[1] >= 0xDC00 && (*text)[1] <= 0xDFFF) { c = 0x10000 + ((c - 0xD800) << 10) + ((*text)[1] - 0xDC00); ++*text; }
  else if (c >= 0xD800 && c <= 0xDFFF) c = 0xFFFD;
  if (c < 0x80) return url_emit_escaped(out, n, c);
  if (c < 0x800) { n = url_emit_escaped(out, n, 0xC0 | (c >> 6)); return url_emit_escaped(out, n, 0x80 | (c & 63)); }
  if (c < 0x10000) { n = url_emit_escaped(out, n, 0xE0 | (c >> 12)); n = url_emit_escaped(out, n, 0x80 | ((c >> 6) & 63)); return url_emit_escaped(out, n, 0x80 | (c & 63)); }
  n = url_emit_escaped(out, n, 0xF0 | (c >> 18)); n = url_emit_escaped(out, n, 0x80 | ((c >> 12) & 63));
  n = url_emit_escaped(out, n, 0x80 | ((c >> 6) & 63)); return url_emit_escaped(out, n, 0x80 | (c & 63));
}
/* UrlEscape into `out` (capacity URL_MAX + 1 characters); the length, or -1. */
static int url_escape(const WCHAR *url, DWORD flags, WCHAR *out) {
  int n = 0;
  const int limit = URL_MAX - 3;
  if (flags & F_ESCAPE_SPACES_ONLY) {
    for (; *url; ++url) { if (n >= limit) return -1; if (*url == ' ') n = url_emit_escaped(out, n, ' '); else out[n++] = *url; }
    out[n] = 0;
    return n;
  }
  const int segment = (flags & F_ESCAPE_SEGMENT_ONLY) != 0;
  int scheme = 0, hierarchical = 0, is_file = 0, is_web = 0, opaque = 0, extra = 0;
  if (!segment) {
    const int sl = url_scheme_length(url);
    if (sl) {
      const UrlScheme *s = url_find_scheme(url, sl);
      is_file = s && s->scheme == 9;
      is_web = s && (s->scheme == 2 || s->scheme == 11);
      opaque = s && s->opaque;
      for (int i = 0; i < sl; ++i) out[n++] = url[i];
      out[n++] = ':';
      url += sl + 1;
      scheme = 1;
      if (is_file) {
        int slashes = 0;
        while (url[slashes] == '/' || url[slashes] == '\\') ++slashes;
        if (slashes) {
          const WCHAR *after = url + slashes;
          static const WCHAR local[] = {'l', 'o', 'c', 'a', 'l', 'h', 'o', 's', 't', 0};
          int count = slashes >= 4 ? 2 : slashes == 2 ? 2 : 3;
          if (slashes == 2) {
            int k = 0;
            while (local[k] && up_char(after[k]) == up_char(local[k])) ++k;
            if (!local[k] && (after[k] == '/' || after[k] == '\\')) { after += k; count = 3; --after; ++after; }
          }
          for (int i = 0; i < count; ++i) out[n++] = '/';
          url = after;
          if (count == 3 && slashes == 2) { while (*url == '/' || *url == '\\') ++url; }
        }
        hierarchical = 1;
      } else if (opaque) {
        hierarchical = 0;
      } else {
        hierarchical = url[0] == '/' || (is_web && url[0] == '\\');
        if (hierarchical && is_web) { while (*url == '/' || *url == '\\') { out[n++] = '/'; ++url; } }
      }
    }
  }
  if (!scheme && !segment) hierarchical = 1;
  for (; *url; ++url) {
    if (n >= limit - 12) return -1;
    const unsigned c = *url;
    if (!segment && !opaque && !extra && (c == '?' || (c == '#' && !is_file)) && !(scheme && !hierarchical)) { if (flags & F_DONT_ESCAPE_EXTRA_INFO) extra = 1; else extra = 1; }
    if (extra) { out[n++] = (WCHAR)c; continue; }
    if (c == '%') { if (flags & F_ESCAPE_PERCENT) n = url_emit_escaped(out, n, '%'); else out[n++] = '%'; continue; }
    if (c == '/' || c == '\\') {
      if (segment || (scheme && !hierarchical) || opaque) { n = url_emit_escaped(out, n, c); continue; }
      if (c == '\\') { if (is_file || is_web) out[n++] = '/'; else n = url_emit_escaped(out, n, '\\'); }
      else out[n++] = '/';
      continue;
    }
    if (c == '?' || c == '#') { n = url_emit_escaped(out, n, c); continue; }
    if (c == '#') { n = url_emit_escaped(out, n, c); continue; }
    if (url_unsafe(c) || (c > 0x7e && (flags & F_ESCAPE_AS_UTF8))) {
      if (c > 0x7e) n = url_emit_utf8(out, n, &url); else n = url_emit_escaped(out, n, c);
    } else out[n++] = (WCHAR)c;
  }
  out[n] = 0;
  return n;
}
EXPORT HRESULT_ WINAPI UrlEscapeW(LPCWSTR url, LPWSTR out, DWORD *length, DWORD flags) {
  if (!url || !out || !length) return E_INVALIDARG_;
  WCHAR work[URL_MAX + 1];
  const int n = url_escape(url, flags, work);
  if (n < 0) return E_INVALIDARG_;
  return url_return(work, n, out, length);
}
EXPORT HRESULT_ WINAPI UrlEscapeA(LPCSTR url, LPSTR out, DWORD *length, DWORD flags) {
  WCHAR wide[URL_MAX + 1], work[URL_MAX + 1];
  if (!url || !out || !length || !url_wide(url, wide, URL_MAX)) return E_INVALIDARG_;
  const int n = url_escape(wide, flags, work);
  if (n < 0) return E_INVALIDARG_;
  return url_return_a(work, n, out, length, 0);
}

/* ---- unescaping ----------------------------------------------------------------------------------------------------------------------------------------------- */

static int url_hex(WCHAR c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; }
static int url_escaped_byte(const WCHAR *s) { return s[0] == '%' && url_hex(s[1]) >= 0 && url_hex(s[2]) >= 0 ? url_hex(s[1]) * 16 + url_hex(s[2]) : -1; }
static int url_unescape(const WCHAR *url, DWORD flags, WCHAR *out) {
  int n = 0;
  const int utf8 = (flags & F_UNESCAPE_AS_UTF8) != 0;
  while (*url) {
    if (n >= URL_MAX) return -1;
    if (flags & F_DONT_ESCAPE_EXTRA_INFO && (*url == '?' || *url == '#')) { while (*url) { if (n >= URL_MAX) return -1; out[n++] = *url++; } break; }
    int b = url_escaped_byte(url);
    if (b < 0) { out[n++] = *url++; continue; }
    if (!utf8 || b < 0x80) { out[n++] = (WCHAR)b; url += 3; continue; }
    int need = b >= 0xF0 && b < 0xF8 ? 3 : b >= 0xE0 ? 2 : b >= 0xC0 ? 1 : -1;
    url += 3;
    if (need < 0 || b >= 0xF8) { out[n++] = 0xFFFD; continue; }
    unsigned cp = need == 3 ? (unsigned)(b & 7) : need == 2 ? (unsigned)(b & 15) : (unsigned)(b & 31);
    int got = 0;
    while (got < need) {
      const int c = url_escaped_byte(url);
      if (c < 0x80 || c >= 0xC0) break;
      cp = (cp << 6) | (unsigned)(c & 63);
      url += 3;
      ++got;
    }
    if (got < need) { out[n++] = 0xFFFD; continue; }
    if (cp >= 0x10000) { cp -= 0x10000; out[n++] = (WCHAR)(0xD800 + (cp >> 10)); out[n++] = (WCHAR)(0xDC00 + (cp & 0x3FF)); }
    else out[n++] = (WCHAR)cp;
  }
  out[n] = 0;
  return n;
}
EXPORT HRESULT_ WINAPI UrlUnescapeW(LPWSTR url, LPWSTR out, DWORD *length, DWORD flags) {
  if (!url) return E_INVALIDARG_;
  WCHAR work[URL_MAX + 1];
  const int n = url_unescape(url, flags, work);
  if (n < 0) return E_INVALIDARG_;
  if (flags & F_UNESCAPE_INPLACE) { for (int i = 0; i <= n; ++i) url[i] = work[i]; return 0; }
  if (!out || !length) return E_INVALIDARG_;
  return url_return(work, n, out, length);
}
EXPORT HRESULT_ WINAPI UrlUnescapeA(LPSTR url, LPSTR out, DWORD *length, DWORD flags) {
  WCHAR wide[URL_MAX + 1], work[URL_MAX + 1];
  if (!url || !url_wide(url, wide, URL_MAX)) return E_INVALIDARG_;
  const int n = url_unescape(wide, flags, work);
  if (n < 0) return E_INVALIDARG_;
  char narrow[URL_MAX * 3];
  if (flags & F_UNESCAPE_INPLACE) {
    const int bytes = WideCharToMultiByte(CP_ACP, 0, work, -1, narrow, sizeof(narrow), NULL, NULL);
    if (bytes <= 0) return E_FAIL__;
    for (int i = 0; i < bytes; ++i) url[i] = narrow[i];
    return 0;
  }
  if (!out || !length) return E_INVALIDARG_;
  return url_return_a(work, n, out, length, 0);
}

/* ---- classification and parts ------------------------------------------------------------------------------------------------------------------------------------ */

EXPORT BOOL WINAPI UrlIsW(LPCWSTR url, DWORD kind) {
  if (!url) return FALSE;
  const int sl = url_scheme_length(url);
  const UrlScheme *s = sl ? url_find_scheme(url, sl) : NULL;
  switch (kind) {
    case 0: return sl != 0;                                            /* URLIS_URL */
    case 1: return s && s->opaque;                                     /* URLIS_OPAQUE */
    case 2: return s && (s->scheme == 15 || s->scheme == 16 || s->scheme == 17 || s->scheme == 4);      /* URLIS_NOHISTORY */
    case 3: return s && s->scheme == 9;                                /* URLIS_FILEURL */
    case 4: return sl == 0;                                            /* URLIS_APPLIABLE */
    case 5: { size_t_ n = wcslen(url); return n && (url[n - 1] == '/' || url[n - 1] == '\\'); }      /* URLIS_DIRECTORY */
    case 6: for (const WCHAR *p = url; *p; ++p) if (*p == '?') return TRUE; return FALSE;           /* URLIS_HASQUERY */
  }
  return FALSE;
}
EXPORT BOOL WINAPI UrlIsA(LPCSTR url, DWORD kind) {
  WCHAR wide[URL_MAX + 1];
  if (!url || !url_wide(url, wide, URL_MAX)) return FALSE;
  return UrlIsW(wide, kind);
}
EXPORT BOOL WINAPI UrlIsOpaqueW(LPCWSTR url) { return UrlIsW(url, 1); }
EXPORT BOOL WINAPI UrlIsOpaqueA(LPCSTR url) { return UrlIsA(url, 1); }
EXPORT BOOL WINAPI UrlIsNoHistoryW(LPCWSTR url) { return UrlIsW(url, 2); }
EXPORT BOOL WINAPI UrlIsNoHistoryA(LPCSTR url) { return UrlIsA(url, 2); }
EXPORT LPCWSTR WINAPI UrlGetLocationW(LPCWSTR url) {
  if (!url) return NULL;
  const int sl = url_scheme_length(url);
  const UrlScheme *s = sl ? url_find_scheme(url, sl) : NULL;
  if (s && s->opaque) return NULL;
  for (; *url; ++url) if (*url == '#') return url;
  return NULL;
}
EXPORT LPCSTR WINAPI UrlGetLocationA(LPCSTR url) {
  if (!url) return NULL;
  for (; *url; ++url) if (*url == '#') return url;
  return NULL;
}

typedef struct { DWORD size; LPCWSTR protocol; UINT protocol_length; LPCWSTR suffix; UINT suffix_length; UINT scheme; } PARSEDURLW_;
typedef struct { DWORD size; LPCSTR protocol; UINT protocol_length; LPCSTR suffix; UINT suffix_length; UINT scheme; } PARSEDURLA_;
EXPORT HRESULT_ WINAPI ParseURLW(LPCWSTR url, PARSEDURLW_ *parsed) {
  if (!url || !parsed || parsed->size != sizeof(*parsed)) return E_INVALIDARG_;
  parsed->protocol = NULL;
  const int sl = url_scheme_length(url);
  if (!sl) return URL_E_INVALID_SYNTAX_;
  const UrlScheme *s = url_find_scheme(url, sl);
  parsed->protocol = url;
  parsed->protocol_length = (UINT)sl;
  parsed->suffix = url + sl + 1;
  parsed->suffix_length = (UINT)wcslen(url + sl + 1);
  parsed->scheme = s ? (UINT)s->scheme : 0;
  return 0;
}
EXPORT HRESULT_ WINAPI ParseURLA(LPCSTR url, PARSEDURLA_ *parsed) {
  if (!url || !parsed || parsed->size != sizeof(*parsed)) return E_INVALIDARG_;
  WCHAR wide[URL_MAX + 1];
  parsed->protocol = NULL;
  if (!url_wide(url, wide, URL_MAX)) return E_INVALIDARG_;
  const int sl = url_scheme_length(wide);
  if (!sl) return URL_E_INVALID_SYNTAX_;
  const UrlScheme *s = url_find_scheme(wide, sl);
  parsed->protocol = url;
  parsed->protocol_length = (UINT)sl;
  parsed->suffix = url + sl + 1;
  UINT n = 0;
  while (url[sl + 1 + n]) ++n;
  parsed->suffix_length = n;
  parsed->scheme = s ? (UINT)s->scheme : 0;
  return 0;
}

/* UrlGetPart: scheme, host, user, password, port or query. The core answers 0 (the text is in `result`), 1 (the part is empty) or a failure. */
static HRESULT_ url_part(LPCWSTR url, DWORD part, DWORD flags, WCHAR *result, int *length) {
  const int sl = url_scheme_length(url);
  const UrlScheme *scheme = sl ? url_find_scheme(url, sl) : NULL;
  const int is_file = scheme && scheme->scheme == 9;
  const WCHAR *rest = sl ? url + sl + 1 : url;
  int n = 0;
  *length = 0;
  result[0] = 0;
  if (part == 1) {
    if (!sl) return 1;
    for (int i = 0; i < sl; ++i) result[n++] = (WCHAR)(url[i] >= 'A' && url[i] <= 'Z' ? url[i] + 32 : url[i]);
    *length = n; result[n] = 0;
    return 0;
  }
  if (part == 6) {
    const WCHAR *q = rest;
    while (*q && *q != '?' && *q != '#') ++q;
    if (*q != '?') return 1;
    ++q;
    for (; *q && *q != '#' && n < URL_MAX; ++q) result[n++] = *q;
    *length = n; result[n] = 0;
    return n ? 0 : 1;
  }
  if (!sl) return E_FAIL__;
  const int slash1 = rest[0] == '/' || rest[0] == '\\', slash2 = slash1 && (rest[1] == '/' || rest[1] == '\\');
  int hierarchical = scheme && (scheme->scheme == 1 || scheme->scheme == 2 || scheme->scheme == 3 || scheme->scheme == 5 || scheme->scheme == 6 || scheme->scheme == 7 || scheme->scheme == 11 || scheme->scheme == 13);
  if (is_file) {
    if (part != 2) return E_FAIL__;
    if (!slash2) return 1;
    const WCHAR *auth = rest + 2, *end = auth;
    while (*end && *end != '/' && *end != '\\') ++end;
    if (end - auth == 2 && ((auth[0] | 0x20) >= 'a' && (auth[0] | 0x20) <= 'z') && auth[1] == ':') return 1;      /* (a drive, not a host) */
    for (; auth < end && n < URL_MAX; ++auth) result[n++] = *auth;
    *length = n; result[n] = 0;
    return n ? 0 : 1;
  }
  if (!hierarchical || !slash2) return E_FAIL__;
  const WCHAR *auth = rest + 2, *end = auth;
  while (*end && *end != '/' && *end != '\\' && *end != '?' && *end != '#') ++end;
  const WCHAR *at = NULL;
  for (const WCHAR *p = auth; p < end; ++p) if (*p == '@') { at = p; break; }
  const WCHAR *host = at ? at + 1 : auth, *colon = NULL;
  for (const WCHAR *p = host; p < end; ++p) if (*p == ':') { colon = p; break; }
  const WCHAR *from, *to;
  if (part == 3 || part == 4) {
    if (!at) return E_INVALIDARG_;
    const WCHAR *c = NULL;
    for (const WCHAR *p = auth; p < at; ++p) if (*p == ':') { c = p; break; }
    if (part == 3) { from = auth; to = c ? c : at; }
    else { if (!c) return E_INVALIDARG_; from = c + 1; to = at; }
  } else if (part == 2) { from = host; to = colon ? colon : end; }
  else { if (!colon) return E_INVALIDARG_; from = colon + 1; to = end; }
  const int keep = (flags & 1) != 0;
  if (from == to && !(keep && part == 2)) return 1;
  if (keep) { for (int i = 0; i < sl; ++i) result[n++] = url[i]; result[n++] = ':'; }
  for (; from < to && n < URL_MAX; ++from) result[n++] = *from;
  *length = n; result[n] = 0;
  return 0;
}
EXPORT HRESULT_ WINAPI UrlGetPartW(LPCWSTR url, LPWSTR out, DWORD *length, DWORD part, DWORD flags) {
  if (!url || !out || !length || !*length || part < 1 || part > 6) return E_INVALIDARG_;
  WCHAR work[URL_MAX + 1];
  int n;
  const HRESULT_ hr = url_part(url, part, flags, work, &n);
  if (hr < 0) return hr;
  if (hr == 1) { out[0] = 0; *length = 0; return 0; }
  return url_return(work, n, out, length);
}
EXPORT HRESULT_ WINAPI UrlGetPartA(LPCSTR url, LPSTR out, DWORD *length, DWORD part, DWORD flags) {
  WCHAR wide[URL_MAX + 1], work[URL_MAX + 1];
  if (!url || !out || !length || !*length || part < 1 || part > 6 || !url_wide(url, wide, URL_MAX)) return E_INVALIDARG_;
  int n;
  const HRESULT_ hr = url_part(wide, part, flags, work, &n);
  if (hr < 0) return hr;
  if (hr == 1) { out[0] = 0; *length = 0; return 1; }
  return url_return_a(work, n, out, length, 0);
}

/* ---- canonical form, combining, comparing ------------------------------------------------------------------------------------------------------------------------ */

/* Collapses "." and ".." in the path of a hierarchical URL (between the authority and any ? or #). */
static int url_simplify(WCHAR *url, int n) {
  int path = 0;
  const int sl = url_scheme_length(url);
  if (sl) { path = sl + 1; if (url[path] == '/' && url[path + 1] == '/') { path += 2; if (url[path] == '/' && url_is_file(url)) {} else while (path < n && url[path] != '/' && url[path] != '?' && url[path] != '#') ++path; } }
  if (sl && !(url[sl + 1] == '/')) return n;
  int end = path;
  while (end < n && url[end] != '?' && url[end] != '#') ++end;
  /* root of the path: the path of "file:///c:/x" starts after the drive */
  WCHAR work[URL_MAX + 1];
  int w = 0;
  for (int i = 0; i < path; ++i) work[w++] = url[i];
  const int root = w;
  int i = path;
  while (i < end) {
    int s = i;
    while (i < end && url[i] != '/') ++i;
    const int len = i - s;
    const int sep = i < end;
    if (len == 0) { if (w == root || work[w - 1] != '/') work[w++] = '/'; if (sep) ++i; continue; }
    if (len == 1 && url[s] == '.') { if (sep) ++i; else if (w > root && work[w - 1] != '/') {} continue; }
    if (len == 2 && url[s] == '.' && url[s + 1] == '.') {
      if (w > root) { if (work[w - 1] == '/') --w; while (w > root && work[w - 1] != '/') --w; }
      if (!sep && w == root) work[w++] = '/';
      if (sep) ++i;
      continue;
    }
    for (int k = 0; k < len; ++k) work[w++] = url[s + k];
    if (sep) { work[w++] = '/'; ++i; }
  }
  for (int k = end; k < n; ++k) work[w++] = url[k];
  work[w] = 0;
  for (int k = 0; k <= w; ++k) url[k] = work[k];
  return w;
}
static int url_canonicalize(const WCHAR *url, DWORD flags, WCHAR *out) {
  WCHAR work[URL_MAX + 1], escaped[URL_MAX + 1];
  int n = 0;
  const WCHAR *src = url;
  while (*src == ' ') ++src;
  if (flags & F_UNESCAPE) { n = url_unescape(src, flags & F_DONT_ESCAPE_EXTRA_INFO, work); if (n < 0) return -1; src = work; }
  /* a path that is a DOS path becomes a file URL */
  WCHAR dos[URL_MAX + 1];
  if (!url_scheme_length(src) && ((src[0] >= 'a' && src[0] <= 'z') || (src[0] >= 'A' && src[0] <= 'Z')) && src[1] == ':' && (src[2] == '\\' || src[2] == '/')) {
    DWORD l = URL_MAX;
    if (UrlCreateFromPathW(src, dos, &l, 0) < 0) return -1;
    src = dos;
  }
  const DWORD escape_flags = (flags & F_ESCAPE_UNSAFE) ? (flags & (F_DONT_ESCAPE_EXTRA_INFO | F_ESCAPE_PERCENT)) : (F_ESCAPE_SPACES_ONLY);
  n = url_escape(src, escape_flags | ((flags & F_ESCAPE_UNSAFE) ? 0 : 0), escaped);
  if (n < 0) return -1;
  /* the scheme is lower case; a backslash in the path of a web or file URL is a slash */
  const int sl = url_scheme_length(escaped);
  const UrlScheme *s = sl ? url_find_scheme(escaped, sl) : NULL;
  for (int i = 0; i < sl; ++i) if (escaped[i] >= 'A' && escaped[i] <= 'Z') escaped[i] = (WCHAR)(escaped[i] + 32);
  if (s && (s->scheme == 2 || s->scheme == 11 || s->scheme == 9 || s->scheme == 1)) {
    for (int i = sl; escaped[i] && escaped[i] != '?' && escaped[i] != '#'; ++i) if (escaped[i] == '\\') escaped[i] = '/';
  }
  if (!(flags & F_DONT_SIMPLIFY)) n = url_simplify(escaped, n);
  for (int i = 0; i <= n; ++i) out[i] = escaped[i];
  return n;
}
EXPORT HRESULT_ WINAPI UrlCanonicalizeW(LPCWSTR url, LPWSTR out, DWORD *length, DWORD flags) {
  if (!url || !out || !length) return E_INVALIDARG_;
  WCHAR work[URL_MAX + 1];
  if (!*url) { if (*length < 1) { *length = 1; return E_POINTER_; } out[0] = 0; *length = 0; return 0; }
  const int n = url_canonicalize(url, flags, work);
  if (n < 0) return E_INVALIDARG_;
  return url_return(work, n, out, length);
}
EXPORT HRESULT_ WINAPI UrlCanonicalizeA(LPCSTR url, LPSTR out, DWORD *length, DWORD flags) {
  WCHAR wide[URL_MAX + 1], work[URL_MAX + 1];
  if (!url || !out || !length || !url_wide(url, wide, URL_MAX)) return E_INVALIDARG_;
  if (!*url) { if (*length < 1) { *length = 1; return E_POINTER_; } out[0] = 0; *length = 0; return 0; }
  const int n = url_canonicalize(wide, flags, work);
  if (n < 0) return E_INVALIDARG_;
  return url_return_a(work, n, out, length, 0);
}
EXPORT HRESULT_ WINAPI UrlCreateFromPathW(LPCWSTR path, LPWSTR out, DWORD *length, DWORD reserved) {
  (void)reserved;
  if (!path || !out || !length) return E_INVALIDARG_;
  WCHAR work[URL_MAX + 1];
  int n = 0;
  HRESULT_ hr = 0;
  const int sl = url_scheme_length(path);
  if (sl >= 2 && !(path[1] == ':' && sl == 1)) {
    /* already a URL ("file:" or any other scheme): copied as it is */
    n = (int)wcslen(path);
    if (n > URL_MAX) return E_INVALIDARG_;
    for (int i = 0; i <= n; ++i) work[i] = path[i];
    hr = 1;
  } else {
    static const WCHAR file[] = {'f', 'i', 'l', 'e', ':', 0};
    for (int i = 0; file[i]; ++i) work[n++] = file[i];
    if ((path[0] == '\\' && path[1] != '\\') || (((path[0] | 0x20) >= 'a' && (path[0] | 0x20) <= 'z') && path[1] == ':')) { work[n++] = '/'; work[n++] = '/'; work[n++] = '/'; }
    else if (path[0] == '\\' && path[1] == '\\') { work[n++] = '/'; work[n++] = '/'; path += 2; }
    WCHAR tmp[URL_MAX + 1];
    int t = 0;
    for (; *path && t < URL_MAX - 4; ++path) tmp[t++] = *path == '\\' ? '/' : *path;
    tmp[t] = 0;
    WCHAR esc[URL_MAX + 1];
    const int e = url_escape(tmp, F_ESCAPE_SPACES_ONLY, esc);
    if (e < 0) return E_INVALIDARG_;
    for (int i = 0; i < e; ++i) {
      if (esc[i] == '%' && !(esc[i + 1] == '2' && esc[i + 2] == '0')) { if (n + 3 >= URL_MAX) return E_INVALIDARG_; work[n++] = '%'; work[n++] = '2'; work[n++] = '5'; }
      else { if (n + 1 >= URL_MAX) return E_INVALIDARG_; work[n++] = esc[i]; }
    }
    work[n] = 0;
  }
  const HRESULT_ r = url_return(work, n, out, length);
  return r ? r : hr;
}
EXPORT HRESULT_ WINAPI UrlCreateFromPathA(LPCSTR path, LPSTR out, DWORD *length, DWORD reserved) {
  WCHAR wide[URL_MAX + 1], result[URL_MAX + 1];
  if (!path || !out || !length || !url_wide(path, wide, URL_MAX)) return E_INVALIDARG_;
  DWORD n = URL_MAX;
  const HRESULT_ hr = UrlCreateFromPathW(wide, result, &n, reserved);
  if (hr < 0 && hr != E_POINTER_) return hr;
  if (hr == E_POINTER_) { *length = n; return hr; }
  return url_return_a(result, (int)n, out, length, hr);
}

/* UrlApplyScheme: the scheme a typed address is missing (URL_APPLY_DEFAULT 1, GUESSSCHEME 2, GUESSFILE 4, FORCEAPPLY 8). */
EXPORT HRESULT_ WINAPI UrlApplySchemeW(LPCWSTR url, LPWSTR out, DWORD *length, DWORD flags) {
  if (!url || !out || !length) return E_INVALIDARG_;
  WCHAR work[URL_MAX + 1];
  static const WCHAR http[] = {'h', 't', 't', 'p', ':', '/', '/', 0}, ftp[] = {'f', 't', 'p', ':', '/', '/', 0}, file[] = {'f', 'i', 'l', 'e', ':', '/', '/', 0};
  const int has_scheme = url_scheme_length(url) != 0;
  if (has_scheme && !(flags & 8)) return 1;
  const WCHAR *prefix = NULL;
  int unc = 0;
  if (flags & 4) {
    const int drive = ((url[0] | 0x20) >= 'a' && (url[0] | 0x20) <= 'z') && url[1] == ':' && url[2] == '\\';
    if (drive) {
      DWORD l = URL_MAX;
      if (UrlCreateFromPathW(url, work, &l, 0) < 0) return E_FAIL__;
      return url_return(work, (int)l, out, length);
    }
    if (url[0] == '\\' && url[1] == '\\') { unc = 1; prefix = file; }
  }
  if (!prefix && (flags & 2)) {
    if ((url[0] | 0x20) == 'f' && (url[1] | 0x20) == 't' && (url[2] | 0x20) == 'p' && url[3] == '.') prefix = ftp;
    else if ((url[0] | 0x20) == 'w' && (url[1] | 0x20) == 'w' && (url[2] | 0x20) == 'w' && url[3] == '.') prefix = http;
  }
  if (!prefix && (flags & 1)) prefix = http;
  if (!prefix) return 1;
  int n = 0;
  for (int i = 0; prefix[i]; ++i) work[n++] = prefix[i];
  if (unc) url += 2;
  for (; *url && n < URL_MAX - 1; ++url) work[n++] = unc && *url == '\\' ? '/' : *url;
  work[n] = 0;
  return url_return(work, n, out, length);
}
EXPORT HRESULT_ WINAPI UrlApplySchemeA(LPCSTR url, LPSTR out, DWORD *length, DWORD flags) {
  WCHAR wide[URL_MAX + 1], result[URL_MAX + 1];
  if (!url || !out || !length || !url_wide(url, wide, URL_MAX)) return E_INVALIDARG_;
  DWORD n = URL_MAX;
  const HRESULT_ hr = UrlApplySchemeW(wide, result, &n, flags);
  if (hr == 1) return hr;
  if (hr < 0 && hr != E_POINTER_) return hr;
  if (hr == E_POINTER_) { *length = n; return hr; }
  return url_return_a(result, (int)n, out, length, hr);
}

/* UrlCombine: `relative` resolved against `base`. */
EXPORT HRESULT_ WINAPI UrlCombineW(LPCWSTR base, LPCWSTR relative, LPWSTR out, DWORD *length, DWORD flags) {
  if (!base || !relative || !length || (!out && *length)) return E_INVALIDARG_;
  WCHAR work[URL_MAX + 1], b[URL_MAX + 1], r[URL_MAX + 1];
  int n = 0;
  const DWORD canon_flags = flags & ~0u;
  if (url_scheme_length(relative) || (((relative[0] | 0x20) >= 'a' && (relative[0] | 0x20) <= 'z') && relative[1] == ':' && (relative[2] == '\\' || relative[2] == '/'))) {
    n = url_canonicalize(relative, canon_flags, work);
    if (n < 0) return E_INVALIDARG_;
    return url_return(work, n, out, length);
  }
  if (!*base) { n = url_canonicalize(relative, canon_flags, work); if (n < 0) return E_INVALIDARG_; return url_return(work, n, out, length); }
  if (url_canonicalize(base, canon_flags | F_DONT_SIMPLIFY, b) < 0) return E_INVALIDARG_;
  int rn = 0;
  for (; relative[rn] && rn < URL_MAX; ++rn) r[rn] = relative[rn] == '\\' ? '/' : relative[rn];
  r[rn] = 0;
  const int sl = url_scheme_length(b);
  int bn = (int)wcslen(b);
  if (r[0] == '#') {
    int cut = bn;
    for (int i = 0; i < bn; ++i) if (b[i] == '#') { cut = i; break; }
    for (int i = 0; i < cut; ++i) work[n++] = b[i];
    for (int i = 0; i < rn; ++i) work[n++] = r[i];
  } else {
    int cut = bn;
    for (int i = 0; i < bn; ++i) if (b[i] == '?' || b[i] == '#') { cut = i; break; }
    int authority_end = 0;
    if (sl && b[sl + 1] == '/' && b[sl + 2] == '/') { authority_end = sl + 3; while (authority_end < cut && b[authority_end] != '/') ++authority_end; }
    else if (sl) authority_end = sl + 1;
    if (r[0] == '/' && r[1] == '/') {
      for (int i = 0; i < sl + 1; ++i) work[n++] = b[i];
      for (int i = 0; i < rn; ++i) work[n++] = r[i];
    } else if (r[0] == '/') {
      for (int i = 0; i < authority_end; ++i) work[n++] = b[i];
      for (int i = 0; i < rn; ++i) work[n++] = r[i];
    } else {
      int last = authority_end;
      for (int i = authority_end; i < cut; ++i) if (b[i] == '/') last = i + 1;
      for (int i = 0; i < last; ++i) work[n++] = b[i];
      if (last == authority_end && sl && b[sl + 1] == '/' && last > sl + 2 && work[n - 1] != '/') work[n++] = '/';
      for (int i = 0; i < rn; ++i) work[n++] = r[i];
    }
  }
  work[n] = 0;
  WCHAR done[URL_MAX + 1];
  n = url_canonicalize(work, canon_flags, done);
  if (n < 0) return E_INVALIDARG_;
  return url_return(done, n, out, length);
}
EXPORT HRESULT_ WINAPI UrlCombineA(LPCSTR base, LPCSTR relative, LPSTR out, DWORD *length, DWORD flags) {
  WCHAR wb[URL_MAX + 1], wr[URL_MAX + 1], result[URL_MAX + 1];
  if (!base || !relative || !length || !url_wide(base, wb, URL_MAX) || !url_wide(relative, wr, URL_MAX)) return E_INVALIDARG_;
  if (!out && *length) return E_INVALIDARG_;
  DWORD n = URL_MAX;
  const HRESULT_ hr = UrlCombineW(wb, wr, result, &n, flags);
  if (hr < 0 && hr != E_POINTER_) return hr;
  if (hr == E_POINTER_) { *length = n; return hr; }
  return url_return_a(result, (int)n, out, length, hr);
}
EXPORT int WINAPI UrlCompareW(LPCWSTR a, LPCWSTR b, BOOL ignore_slash) {
  if (!a || !b) return a ? 1 : b ? -1 : 0;
  WCHAR x[URL_MAX + 1], y[URL_MAX + 1];
  int nx = (int)wcslen(a), ny = (int)wcslen(b);
  if (nx > URL_MAX) nx = URL_MAX;
  if (ny > URL_MAX) ny = URL_MAX;
  for (int i = 0; i < nx; ++i) x[i] = a[i];
  for (int i = 0; i < ny; ++i) y[i] = b[i];
  if (ignore_slash) { if (nx && x[nx - 1] == '/') --nx; if (ny && y[ny - 1] == '/') --ny; }
  x[nx] = y[ny] = 0;
  return StrCmpW(x, y);
}
EXPORT int WINAPI UrlCompareA(LPCSTR a, LPCSTR b, BOOL ignore_slash) {
  if (!a || !b) return a ? 1 : b ? -1 : 0;
  int nx = 0, ny = 0;
  while (a[nx]) ++nx;
  while (b[ny]) ++ny;
  if (ignore_slash) { if (nx && a[nx - 1] == '/') --nx; if (ny && b[ny - 1] == '/') --ny; }
  for (int i = 0;; ++i) {
    const int ca = i < nx ? (unsigned char)a[i] : 0, cb = i < ny ? (unsigned char)b[i] : 0;
    if (ca != cb) return ca < cb ? -1 : 1;
    if (!ca) return 0;
  }
}
