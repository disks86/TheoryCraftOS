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

/* The path part of UrlCanonicalize, following what Windows does (Wine's tests are the specification, see tests/windows/wine/shlwapi/url.c):
 *   opaque schemes (about, javascript, mailto, shell, vbscript): the rest is copied as it is;
 *   mk: no host, backslashes are separators only in a buggy way, progids ("mk:@name/...") are a root dots do not pass;
 *   the web schemes (ftp http https gopher news nntp snews telnet wais local res): a host after "//", ".." that would pass the host is dropped,
 *     a dot as the first element after the host stops all simplification; ftp, wais, local and res keep backslashes, the others turn them into slashes;
 *   every other scheme, and a URL without one: the "unknown scheme" rules, where ".." that cannot be applied stays in the output.
 * Included by url.inc; written to be tested on a host (tests/UrlCanonTest.cpp) without Windows. */

typedef struct { int start, length; WCHAR sep; } UrlElem;
typedef struct { UrlElem e[URL_MAX + 2]; int count; } UrlStack;
typedef struct { WCHAR *p; int n; } UrlOut;

static void uo_put(UrlOut *o, WCHAR c) { if (o->n < URL_MAX) o->p[o->n++] = c; }
static void uo_range(UrlOut *o, const WCHAR *s, int n) { for (int i = 0; i < n; ++i) uo_put(o, s[i]); }
static void uo_ascii(UrlOut *o, const char *s) { while (*s) uo_put(o, (WCHAR)(unsigned char)*s++); }
static int uo_ends(const UrlOut *o, WCHAR c) { return o->n && o->p[o->n - 1] == c; }
static int ue_is(const WCHAR *s, const UrlElem *e, const char *text) {
  int i = 0;
  for (; text[i]; ++i) if (i >= e->length || s[e->start + i] != (WCHAR)text[i]) return 0;
  return i == e->length;
}
static int ucn_find(const WCHAR *s, int n, const char *set) {
  for (int i = 0; i < n; ++i) for (const char *c = set; *c; ++c) if (s[i] == (WCHAR)*c) return i;
  return -1;
}
static WCHAR ucn_lower(WCHAR c) { return c >= 'A' && c <= 'Z' ? (WCHAR)(c + 32) : c; }

/* Splits s[0..n) at '/' (and at '\\' when `both`) and applies the dots. `drop_underflow`: a ".." with nothing to remove is dropped (web schemes), else it is kept.
 * `*trailing` is set when the last element was a dot that was removed. */
static void ucn_elements(const WCHAR *s, int n, int both, int simplify, int drop_underflow, int keep_dot_trailing, UrlStack *st, int *trailing) {
  st->count = 0;
  *trailing = 0;
  int i = 0;
  for (;;) {
    UrlElem e;
    e.start = i;
    while (i < n && s[i] != '/' && !(both && s[i] == '\\')) ++i;
    e.length = i - e.start;
    e.sep = i < n ? s[i] : 0;
    const int last = i >= n;
    if (simplify && ue_is(s, &e, ".")) { if (last) *trailing = 1; }
    else if (simplify && ue_is(s, &e, "..")) {
      if (st->count && (drop_underflow || !ue_is(s, &st->e[st->count - 1], ".."))) --st->count;
      else if (!drop_underflow) st->e[st->count++] = e;
      if (last) *trailing = 1;
    } else {
      st->e[st->count++] = e;
      if (!simplify && keep_dot_trailing && last && (ue_is(s, &e, ".") || ue_is(s, &e, ".."))) *trailing = 1;
    }
    if (last || st->count >= URL_MAX) break;
    ++i;
  }
}
static void ucn_join(UrlOut *o, const WCHAR *s, const UrlStack *st, int mode /* 0 join with '/', 1 keep separators, 2 separators as '/' */) {
  for (int i = 0; i < st->count; ++i) {
    uo_range(o, s + st->e[i].start, st->e[i].length);
    if (mode == 0) { if (i + 1 < st->count) uo_put(o, '/'); }
    else if (st->e[i].sep) uo_put(o, mode == 1 ? st->e[i].sep : '/');
  }
}
static void ucn_reorder(UrlOut *o, const WCHAR *extra, int n) {
  const int h = ucn_find(extra, n, "#"), q = ucn_find(extra, n, "?");
  if (h >= 0 && q > h) { uo_range(o, extra + q, n - q); uo_range(o, extra + h, q - h); }
  else uo_range(o, extra, n);
}

/* an unknown scheme, or none. `rest` is what follows the scheme and its colon. */
static void ucn_unknown(UrlOut *o, const WCHAR *rest, int n, int simplify, int has_scheme) {
  UrlStack *st = (UrlStack *)HeapAlloc(GetProcessHeap(), 0, sizeof(UrlStack));
  if (!st) { uo_range(o, rest, n); return; }
  int body_end = ucn_find(rest, n, "?#");
  if (body_end < 0) body_end = n;
  const WCHAR *body = rest;
  int trailing;
  if (body_end >= 2 && body[0] == '/' && body[1] == '/') {
    const int host_len = ucn_find(body + 2, body_end - 2, "/") < 0 ? body_end - 2 : ucn_find(body + 2, body_end - 2, "/");
    const int path = 2 + host_len;
    uo_ascii(o, "//");
    uo_range(o, body + 2, host_len);
    uo_put(o, '/');
    ucn_elements(body + path + (path < body_end ? 1 : 0), body_end - path - (path < body_end ? 1 : 0), 0, simplify, 0, 1, st, &trailing);
    ucn_join(o, body + path + (path < body_end ? 1 : 0), st, 0);
    if (trailing && st->count && !uo_ends(o, '/')) uo_put(o, '/');
  } else if (body_end >= 1 && body[0] == '/') {
    int skip = 1;
    if (body_end > 1 && body[1] == '\\') skip = 2;
    uo_put(o, '/');
    ucn_elements(body + skip, body_end - skip, 0, simplify, 0, 1, st, &trailing);
    ucn_join(o, body + skip, st, 0);
    if (trailing && st->count && !uo_ends(o, '/')) uo_put(o, '/');
  } else if (has_scheme) {
    if (!(body_end == 1 && body[0] == '.' && simplify)) uo_range(o, body, body_end);
  } else if (body_end > 0) {
    const int start = o->n;
    ucn_elements(body, body_end, 0, simplify, 0, 1, st, &trailing);
    ucn_join(o, body, st, 0);
    if (trailing && st->count && !uo_ends(o, '/')) uo_put(o, '/');
    if (o->n == start) uo_put(o, '/');
  }
  ucn_reorder(o, rest + body_end, n - body_end);
  HeapFree(GetProcessHeap(), 0, st);
}

/* the web schemes */
static void ucn_web(UrlOut *o, const WCHAR *rest, int n, int simplify, int ftp_like, int lower_host) {
  UrlStack *st = (UrlStack *)HeapAlloc(GetProcessHeap(), 0, sizeof(UrlStack));
  if (!st) { uo_range(o, rest, n); return; }
  int body_end = ucn_find(rest, n, "?#");
  if (body_end < 0) body_end = n;
  const WCHAR *body = rest;
  int trailing;
#define UCN_SEP(c) ((c) == '/' || (!ftp_like && (c) == '\\'))
  if (ftp_like && body_end >= 2 && (body[0] == '/' || body[0] == '\\') && (body[1] == '/' || body[1] == '\\') && !(body[0] == '/' && body[1] == '/')) {
    /* "/\" and "\/" are not a host for these schemes */
    uo_range(o, body, body_end);
    if (body_end >= 2 && body[body_end - 1] == '.' && body[body_end - 2] == '.') uo_put(o, '/');
  } else if (body_end >= 2 && UCN_SEP(body[0]) && UCN_SEP(body[1])) {
    int host_len = body_end - 2;
    for (int i = 0; i < body_end - 2; ++i) if (UCN_SEP(body[2 + i])) { host_len = i; break; }
    const int path = 2 + host_len;
    uo_ascii(o, "//");
    for (int i = 0; i < host_len; ++i) uo_put(o, lower_host ? ucn_lower(body[2 + i]) : body[2 + i]);
    uo_put(o, '/');
    if (path < body_end) {
      const WCHAR *p = body + path + 1;
      const int pn = body_end - path - 1;
      ucn_elements(p, pn, 1, simplify, 1, 0, st, &trailing);
      int first_len = 0;
      while (first_len < pn && p[first_len] != '/' && p[first_len] != '\\') ++first_len;
      if ((first_len == 1 && p[0] == '.') || (first_len == 2 && p[0] == '.' && p[1] == '.')) {
        for (int i = 0; i < pn; ++i) uo_put(o, (!ftp_like && p[i] == '\\') ? '/' : p[i]);
      } else {
        ucn_join(o, p, st, ftp_like ? 1 : 2);
        if (trailing && st->count && !uo_ends(o, '/') && !uo_ends(o, '\\')) uo_put(o, '/');
      }
    }
  } else {
    ucn_elements(body, body_end, 1, simplify, 1, 0, st, &trailing);
    ucn_join(o, body, st, ftp_like ? 1 : 2);
    if (trailing && st->count && !uo_ends(o, '/') && !uo_ends(o, '\\')) uo_put(o, '/');
  }
#undef UCN_SEP
  uo_range(o, rest + body_end, n - body_end);
  HeapFree(GetProcessHeap(), 0, st);
}

/* mk: removes "./" and "../" only at the start of an element and only before a forward slash (or the end), and a ".." goes back to the previous forward slash */
static int ucn_rewind(const WCHAR *out, int n, int floor) {
  if (n <= floor) return n;
  if (out[n - 1] == '/') --n;
  int cut = n;
  while (cut > 0 && out[cut - 1] != '/') --cut;
  return cut < floor ? floor : cut;
}
static void ucn_mk(UrlOut *o, const WCHAR *rest, int n, int simplify) {
  int floor = o->n, i = 0;      /* (a ".." does not go back past the scheme) */
  if (n && rest[0] == '@') {
    int slash = -1;
    for (int k = 0; k < n; ++k) if (rest[k] == '/') { slash = k; break; }
    if (slash < 0) { uo_range(o, rest, n); uo_put(o, '/'); return; }
    uo_range(o, rest, slash + 1);
    floor = o->n;
    i = slash + 1;
    int len = 0;
    while (i + len < n && rest[i + len] == '.' ) ++len;
    if ((len == 1 || len == 2) && (i + len == n || rest[i + len] == '/' || rest[i + len] == '?' || rest[i + len] == '#')) simplify = 0;
  }
  for (; i < n; ++i) {
    const WCHAR c = rest[i];
    if (c == '?' || c == '#') { uo_range(o, rest + i, n - i); return; }
    const int boundary = i == 0 || rest[i - 1] == '/' || rest[i - 1] == '\\';
    if (simplify && boundary && c == '.') {
      const int single_end = i + 1 == n || rest[i + 1] == '?' || rest[i + 1] == '#';
      const int double_end = i + 2 < n ? (rest[i + 1] == '.' && (rest[i + 2] == '?' || rest[i + 2] == '#')) : (i + 2 == n && rest[i + 1] == '.');
      if (i + 1 < n && rest[i + 1] == '/') { i += 1; continue; }
      if (single_end) continue;
      if (i + 2 < n && rest[i + 1] == '.' && rest[i + 2] == '/') { o->n = ucn_rewind(o->p, o->n, floor); i += 2; continue; }
      if (double_end) { o->n = ucn_rewind(o->p, o->n, floor); i += 1; continue; }
    }
    uo_put(o, c);
  }
}


/* ---- file: URLs and DOS / UNC paths ---------------------------------------------------------------------------------------------------------------------------------- */

#define UCN_SEP(c) ((c) == '/' || (c) == '\\')
#define F_WININET 0x80000000u
static int ucn_drive(const WCHAR *s, int n) { return n >= 2 && ((s[0] | 0x20) >= 'a' && (s[0] | 0x20) <= 'z') && (s[1] == ':' || s[1] == '|'); }
static int ucn_ends_html(const WCHAR *s, int n) {
  static const char htm[] = ".htm", html[] = ".html";
  for (int pass = 0; pass < 2; ++pass) {
    const char *t = pass ? html : htm;
    const int len = pass ? 5 : 4;
    if (n < len) continue;
    int ok = 1;
    for (int i = 0; i < len; ++i) if (ucn_lower(s[n - len + i]) != (WCHAR)t[i]) { ok = 0; break; }
    if (ok) return 1;
  }
  return 0;
}

/* "C:\dir" and "\\host\share" are file paths: the URL they stand for, written to out (the path is `in`, with backslashes as slashes). Returns the length, or -1 when `in` is not such a path. */
static int ucn_dos_to_url(const WCHAR *in, int n, WCHAR *out) {
  int m = 0;
  if (url_scheme_length(in)) return -1;
  if (n >= 2 && in[0] == '\\' && in[1] == '\\') { const char *p = "file:"; while (*p) out[m++] = (WCHAR)*p++; }
  else if (ucn_drive(in, n)) { const char *p = "file:///"; while (*p) out[m++] = (WCHAR)*p++; }
  else return -1;
  for (int i = 0; i < n && m < URL_MAX - 3; ++i) {
    WCHAR c = in[i] == '\\' ? (WCHAR)'/' : in[i];
    out[m++] = c;
  }
  out[m] = 0;
  return m;
}

static int ucn_is_localhost(const WCHAR *host, int n) {
  static const char lh[] = "localhost";
  if (n != 9) return 0;
  for (int i = 0; i < 9; ++i) if (ucn_lower(host[i]) != (WCHAR)lh[i]) return 0;
  return 1;
}

/* A file: URL. `rest` follows "file:" (which has been written to `o`). Without file flags the path is the URL form (forward slashes, dots simplified like a web URL, with the
 * quirks of # and ?); with URL_FILE_USE_PATHURL or URL_WININET_COMPATIBILITY it is the path form ("\\host\share" as "file://\\host\share"). */
static void ucn_file(UrlOut *o, const WCHAR *rest, int n, DWORD flags) {
  const int wininet = (flags & F_WININET) != 0, pathurl = (flags & F_FILE_USE_PATHURL) != 0, file_flags = wininet || pathurl;
  const int simplify = !(flags & F_DONT_SIMPLIFY);
  UrlStack *st = (UrlStack *)HeapAlloc(GetProcessHeap(), 0, sizeof(UrlStack));
  if (!st) { uo_range(o, rest, n); return; }
  /* with file flags the rest from the first ? is cut (PATHURL) or kept as it is (WININET) */
  const WCHAR *qtail = rest + n;
  int qn = 0;
  if (file_flags) { const int q = ucn_find(rest, n, "?"); if (q >= 0) { qtail = rest + q; qn = n - q; n = q; } }
  int k = 0;
  while (k < n && UCN_SEP(rest[k])) ++k;
  const WCHAR *host = rest, *path;
  int host_len = 0, has_host = 0, bare = 0, path_len;
  if (k >= 4 || k == 2) {
    const int skip = k >= 4 ? k : 2;
    host = rest + skip;
    while (skip + host_len < n && !UCN_SEP(rest[skip + host_len])) ++host_len;
    has_host = host_len > 0;
    path = rest + skip + host_len;
    path_len = n - skip - host_len;
  } else if (k == 3 || k == 1) { path = rest + (k == 3 ? 2 : 0); path_len = n - (k == 3 ? 2 : 0); }
  else { bare = 1; path = rest; path_len = n; }
  /* a drive: as the host ("file://c:/x"), or as the first element ("file:///c:/x", "file:c:x") */
  const WCHAR *root = NULL, *tail = path;
  int root_len = 0, lead_sep = 0, tail_len = path_len;
  if (has_host && host_len == 2 && ucn_drive(host, 2)) { root = host; root_len = 2; has_host = 0; host_len = 0; }
  else {
    int skip = (!bare && tail_len > 0 && UCN_SEP(tail[0])) ? 1 : 0;
    if (has_host && file_flags && ucn_is_localhost(host, host_len)) { has_host = 0; host_len = 0; if (skip && tail_len > 1 && UCN_SEP(tail[1])) ++skip; }
    if (ucn_drive(tail + skip, tail_len - skip)) { root = tail + skip; root_len = 2; tail = root + 2; tail_len -= skip + 2; }
    else { tail += skip; tail_len -= skip; lead_sep = skip > 0; }
  }
  if (root) { lead_sep = tail_len > 0 && UCN_SEP(tail[0]); if (lead_sep) { ++tail; --tail_len; } }
  /* file flags: a drive path without a slash keeps its first element as part of the root ("c:dir") */
  if (file_flags && root && !lead_sep) {
    int e = 0;
    while (e < tail_len && !UCN_SEP(tail[e])) ++e;
    root_len += e; tail += e; tail_len -= e;
    if (tail_len > 0) { ++tail; --tail_len; lead_sep = 1; }
  }
  const int has_root_only = root && !lead_sep && tail_len == 0;

  /* the elements */
  const WCHAR *verbatim = NULL;
  int verbatim_len = 0, trailing = 0, translate_all = 1;
  st->count = 0;
  if (!file_flags) {
    /* the first element a dot or double dot, or a ? or # right after the host: nothing is simplified */
    int e = 0;
    while (e < tail_len && !UCN_SEP(tail[e])) ++e;
    const int dot = !(bare && !root) && ((e == 1 && tail[0] == '.') || (e == 2 && tail[0] == '.' && tail[1] == '.'));
    const int mark = tail_len > 0 && (tail[0] == '#' || tail[0] == '?');
    const int mark2 = tail_len > 1 && UCN_SEP(tail[0]) && (tail[1] == '#' || tail[1] == '?');
    if (dot || (!root && !bare && (mark || mark2))) { verbatim = tail; verbatim_len = tail_len; tail_len = 0; translate_all = dot; has_root_only ? (void)0 : (void)0; }
  }
  int i = 0;
  while (!verbatim && !has_root_only && i <= tail_len && st->count < URL_MAX) {
    int j = i, hash_cut = -1, glued = 0;
    while (j < tail_len && !UCN_SEP(tail[j])) {
      if (tail[j] == '#' && ucn_ends_html(tail + i, j - i)) { hash_cut = j; break; }
      if (!file_flags && j == i && tail[j] == '.') {
        /* a dot or double dot right before a ? or # is still a dot, and what follows continues the line */
        const int d = (j + 1 < tail_len && tail[j + 1] == '.') ? 2 : 1;
        if (j + d < tail_len && (tail[j + d] == '#' || tail[j + d] == '?')) { j += d; glued = 1; break; }
      }
      ++j;
    }
    if (hash_cut >= 0) {
      /* the element before the # ends in .htm(l): the rest is copied as it is (and left out with PATHURL) */
      UrlElem e;
      e.start = i; e.length = hash_cut - i; e.sep = 0;
      st->e[st->count++] = e;
      verbatim = tail + hash_cut; verbatim_len = pathurl ? 0 : tail_len - hash_cut; translate_all = 0;
      break;
    }
    UrlElem e;
    e.start = i; e.length = j - i; e.sep = (j < tail_len && !glued) ? tail[j] : 0;
    const int is_dot = e.length == 1 && tail[e.start] == '.', is_dot2 = e.length == 2 && tail[e.start] == '.' && tail[e.start + 1] == '.';
    const int last = j >= tail_len;
    if (simplify && is_dot) { if (last) trailing = 1; }
    else if (simplify && is_dot2) {
      if (st->count && (!file_flags || !ue_is(tail, &st->e[st->count - 1], ".."))) --st->count;
      else if (file_flags) st->e[st->count++] = e;
      if (last) trailing = 1;
    } else {
      st->e[st->count++] = e;
      if (!simplify && file_flags && last && (is_dot || is_dot2)) trailing = 1;
    }
    if (last) break;
    i = glued ? j : j + 1;
  }

  if (!file_flags) {
    if (bare && !root) { /* "file:" and the path */ }
    else { uo_ascii(o, "//"); uo_range(o, host, host_len); uo_put(o, '/'); }
    if (root) { uo_range(o, root, root_len); if (lead_sep) uo_put(o, '/'); }
    for (int e = 0; e < st->count; ++e) {
      uo_range(o, tail + st->e[e].start, st->e[e].length);
      if (st->e[e].sep) uo_put(o, '/');
    }
    if (verbatim) {
      for (int e = 0; e < verbatim_len; ++e) uo_put(o, (UCN_SEP(verbatim[e]) && (translate_all || e == 0)) ? (WCHAR)'/' : verbatim[e]);
    }
    HeapFree(GetProcessHeap(), 0, st);
    return;
  }
  /* file flags */
  int seen_hash = 0;
  const int local = !has_host;
  if (bare && !root) { if (!(wininet && !pathurl)) uo_ascii(o, "//"); }
  else {
    uo_ascii(o, "//");
    if (root) { uo_range(o, root, root_len); if (ucn_find(root, root_len, "#") >= 0) seen_hash = 1; }
    else if (!local) { uo_ascii(o, "\\\\"); uo_range(o, host, host_len); if (ucn_find(host, host_len, "#") >= 0) seen_hash = 1; }
  }
  const int single_empty = st->count == 1 && st->e[0].length == 0 && !trailing;
  const int no_path = (st->count == 0 && !trailing) || single_empty;
  if (!(no_path && !root)) {
    for (int e = 0; e < st->count; ++e) {
      if (!(bare && !root && e == 0)) uo_put(o, seen_hash ? (WCHAR)'/' : (WCHAR)'\\');
      uo_range(o, tail + st->e[e].start, st->e[e].length);
      if (ucn_find(tail + st->e[e].start, st->e[e].length, "#") >= 0) seen_hash = 1;
    }
  }
  if (trailing && !(bare && !root && st->count == 0)) uo_put(o, seen_hash ? (WCHAR)'/' : (WCHAR)'\\');
  /* WININET: a path that is only the host (or all empty) ends in a backslash; and the rest after a .html# and the query come last */
  if (wininet && !pathurl && !root && !bare && no_path) uo_put(o, '\\');
  if (wininet && !pathurl) uo_range(o, qtail, qn);
  if (verbatim && verbatim_len) uo_range(o, verbatim, verbatim_len);
  HeapFree(GetProcessHeap(), 0, st);
}
#undef UCN_SEP

/* Canonical path form of `in` (already stripped of surrounding control characters and of tab, newline and return). Returns the length written to `out`. */
static int url_canon_core(const WCHAR *in, int n, DWORD flags, WCHAR *out) {
  UrlOut o = {out, 0};
  const int simplify = !(flags & F_DONT_SIMPLIFY);
  const int sl = url_scheme_length(in);
  if (!sl) { ucn_unknown(&o, in, n, simplify, 0); out[o.n] = 0; return o.n; }
  const UrlScheme *s = url_find_scheme(in, sl);
  for (int i = 0; i < sl; ++i) uo_put(&o, ucn_lower(in[i]));
  uo_put(&o, ':');
  const WCHAR *rest = in + sl + 1;
  const int rn = n - sl - 1;
  const int id = s ? s->scheme : 0;
  if (id == 9) { ucn_file(&o, rest, rn, flags); out[o.n] = 0; return o.n; }
  if (id == 4 || id == 12 || id == 15 || id == 16 || id == 17) uo_range(&o, rest, rn);
  else if (id == 10) ucn_mk(&o, rest, rn, simplify);
  else if (id == 1 || id == 8 || id == 14 || id == 18) ucn_web(&o, rest, rn, simplify, 1, 1);
  else if (id == 2 || id == 3 || id == 5 || id == 6 || id == 7 || id == 11 || id == 13) ucn_web(&o, rest, rn, simplify, 0, 1);
  else ucn_unknown(&o, rest, rn, simplify, 1);
  out[o.n] = 0;
  return o.n;
}
