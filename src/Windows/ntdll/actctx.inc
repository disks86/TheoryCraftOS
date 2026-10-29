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

/* ntdll.dll: activation contexts. A context is made from an application or assembly manifest (XML): its assemblies (the root manifest, then the ones it depends on, found next
 * to it as <name>.manifest), their files, window classes, COM classes and the rest. Each thread has a stack of frames (TEB + 0x2c8 points to it; the cookie that activation
 * returns is the address of the frame); the frame on top is the active context. Lookups (RtlFindActivationContextSection*) read "sections" - blocks in the binary layout
 * Windows uses (a header, an index, names, data) - that are built from the parsed manifests when the context is made. A context handle is the address of the ActCtx. */

#define ACT_MAGIC 0xC07E3E11u
#define ACT_MAX_DEPTH 64
#define ACT_MAX_ASSEMBLIES 128
#define ACT_MAX_SIZE 0x10000000u

#define SXS_SECTION_NOT_FOUND 0xC0150001u
#define SXS_CANT_GEN_ACTCTX 0xC0150002u
#define SXS_ASSEMBLY_NOT_FOUND 0xC0150004u
#define SXS_KEY_NOT_FOUND 0xC0150008u
#define SXS_EARLY_DEACTIVATION 0xC015000Fu
#define SXS_INVALID_DEACTIVATION 0xC0150010u
#define SXS_MULTIPLE_DEACTIVATION 0xC0150011u
#define SXS_CORRUPT_ACTIVATION_STACK 0xC0150014u

#define ACT_SECTION_ASSEMBLY_INFORMATION 1
#define ACT_SECTION_DLL_REDIRECTION 2
#define ACT_SECTION_WINDOW_CLASS 3
#define ACT_SECTION_COM_SERVER 4
#define ACT_SECTION_COM_INTERFACE 5
#define ACT_SECTION_COM_TYPELIB 6
#define ACT_SECTION_COM_PROGID 7
#define ACT_SECTION_CLR_SURROGATES 9
#define ACT_SECTION_APPLICATION_SETTINGS 10
#define ACT_SECTION_COMPATIBILITY 11
#define ACT_SECTION_WINRT_CLASSES 12
#define ACT_SECTION_COUNT 13

#define ACT_FRAME_HEAP_ALLOCATED 8
#define TEB_ACTIVATION_STACK(teb) (*(ActStack **)TEB_BYTE(teb, 0x2c8))

typedef struct ActGuid { ULONG d1; USHORT d2, d3; BYTE d4[8]; } ActGuid;
typedef struct ActFrame { struct ActFrame *previous; struct ActCtx *context; ULONG flags; } ActFrame;
typedef struct ActStack { ActFrame *active; LIST_ENTRY cache; ULONG flags, next_cookie, stack_id; } ActStack;
typedef struct ActFrameExtended { SIZE_T size; ULONG format; ActFrame frame; PVOID extra[3]; } ActFrameExtended;

static ULONG rd32(const BYTE *p) { ULONG v; memcpy(&v, p, 4); return v; }
static struct ActCtx *g_process_ctx;      /* the context of the program's own manifest (the process default) */

static const BYTE *act_image_manifest(const BYTE *base, SIZE_T size, int flat, ULONG only_id, SIZE_T *out_size);

/* ---- small helpers ------------------------------------------------------------------------------------------------------------------------------------- */

static void *act_alloc(SIZE_T n) { return RtlAllocateHeap(process_heap(), HEAP_ZERO_MEMORY, n ? n : 1); }
static void act_free(void *p) { if (p) RtlFreeHeap(process_heap(), 0, p); }

/* A copy of n characters, terminated. */
static WCHAR *act_wdup(const WCHAR *s, SIZE_T n) {
  if (n > ACT_MAX_SIZE / 2) return NULL;
  WCHAR *r = act_alloc((n + 1) * 2);
  if (!r) return NULL;
  memcpy(r, s, n * 2);
  return r;
}
static WCHAR *act_wstrdup(const WCHAR *s) { return s ? act_wdup(s, wcslen(s)) : NULL; }

static int act_ieq(const WCHAR *a, const WCHAR *b) {
  if (!a || !b) return a == b;
  return RtlCompareUnicodeStrings(a, wcslen(a), b, wcslen(b), TRUE) == 0;
}
static int act_ieq_n(const WCHAR *a, SIZE_T an, const WCHAR *b, SIZE_T bn) { return an == bn && RtlCompareUnicodeStrings(a, an, b, bn, TRUE) == 0; }
static int act_eq_ascii(const WCHAR *w, const char *a, int ci) {
  if (!w) return 0;
  for (;; ++w, ++a) {
    WCHAR x = *w, y = (BYTE)*a;
    if (ci) { if (x >= 'A' && x <= 'Z') x += 32; if (y >= 'A' && y <= 'Z') y += 32; }
    if (x != y) return 0;
    if (!x) return 1;
  }
}

/* A growing array of fixed-size elements (the elements are numbered: a pointer to one is good only until the next add). */
typedef struct ActVec { BYTE *data; ULONG count, cap, elem; } ActVec;
static void *act_vec_add(ActVec *v) {
  if (v->count == v->cap) {
    const ULONG cap = v->cap ? v->cap * 2 : 4;
    if (cap > ACT_MAX_SIZE / v->elem) return NULL;
    BYTE *n = act_alloc((SIZE_T)cap * v->elem);
    if (!n) return NULL;
    if (v->data) { memcpy(n, v->data, (SIZE_T)v->count * v->elem); act_free(v->data); }
    v->data = n;
    v->cap = cap;
  }
  return v->data + (SIZE_T)v->count++ * v->elem;
}
static void *act_vec_at(const ActVec *v, ULONG i) { return i < v->count ? v->data + (SIZE_T)i * v->elem : NULL; }

/* A byte block that sections are built in. */
typedef struct ActBlob { BYTE *p; SIZE_T len, cap; int failed; } ActBlob;
static SIZE_T blob_add(ActBlob *b, const void *src, SIZE_T n) {
  const SIZE_T at = b->len;
  if (b->failed) return at;
  if (n > ACT_MAX_SIZE - b->len) { b->failed = 1; return at; }
  if (b->len + n > b->cap) {
    SIZE_T cap = b->cap ? b->cap : 256;
    while (cap < b->len + n) cap *= 2;
    BYTE *np = act_alloc(cap);
    if (!np) { b->failed = 1; return at; }
    if (b->p) { memcpy(np, b->p, b->len); act_free(b->p); }
    b->p = np;
    b->cap = cap;
  }
  if (src) memcpy(b->p + b->len, src, n);
  b->len += n;
  return at;
}
static void blob_align4(ActBlob *b) { while (b->len & 3) blob_add(b, NULL, 1); }
static SIZE_T blob_wstr(ActBlob *b, const WCHAR *s, SIZE_T *bytes) {
  const SIZE_T n = s ? wcslen(s) : 0;
  if (bytes) *bytes = n * 2;
  const SIZE_T at = blob_add(b, s, n * 2);
  WCHAR z = 0;
  blob_add(b, &z, 2);
  return at;
}
static void blob_put32(ActBlob *b, SIZE_T at, ULONG v) { if (!b->failed && at + 4 <= b->len) memcpy(b->p + at, &v, 4); }

/* ---- XML ---------------------------------------------------------------------------------------------------------------------------------------------------- */

typedef struct XAttr { WCHAR *qname, *local, *prefix, *value, *ns; } XAttr;
typedef struct XNode {
  WCHAR *qname, *local, *ns;
  XAttr *attrs;
  ULONG nattrs;
  struct XNode *first, *last, *next;
  WCHAR *text;
  SIZE_T text_len, text_cap;
} XNode;

static void xml_free(XNode *n) {
  while (n) {
    XNode *next = n->next;
    xml_free(n->first);
    for (ULONG i = 0; i < n->nattrs; ++i) { act_free(n->attrs[i].qname); act_free(n->attrs[i].value); }
    act_free(n->attrs);
    act_free(n->qname);
    act_free(n->text);
    act_free(n);
    n = next;
  }
}

typedef struct XP { const WCHAR *p, *end; int failed; int depth; } XP;
static int xp_name_start(WCHAR c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == ':' || c >= 0x80; }
static int xp_name_char(WCHAR c) { return xp_name_start(c) || (c >= '0' && c <= '9') || c == '.' || c == '-'; }
static int xp_space(WCHAR c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }
static void xp_skip_ws(XP *x) { while (x->p < x->end && xp_space(*x->p)) ++x->p; }
static int xp_looking_at(XP *x, const char *s) {
  const WCHAR *p = x->p;
  for (; *s; ++s, ++p) if (p >= x->end || *p != (BYTE)*s) return 0;
  return 1;
}
/* A name at the current position (a copy), or NULL (and the parse failed). */
static WCHAR *xp_name(XP *x) {
  const WCHAR *s = x->p;
  if (s >= x->end || !xp_name_start(*s)) { x->failed = 1; return NULL; }
  while (x->p < x->end && xp_name_char(*x->p)) ++x->p;
  WCHAR *r = act_wdup(s, (SIZE_T)(x->p - s));
  if (!r) x->failed = 1;
  return r;
}
static int xp_char_ok(ULONG c) { return c == 9 || c == 10 || c == 13 || (c >= 0x20 && c <= 0xD7FF) || (c >= 0xE000 && c <= 0xFFFD) || (c >= 0x10000 && c <= 0x10FFFF); }
static int xp_put_char(WCHAR *out, SIZE_T *n, ULONG c) {
  if (!xp_char_ok(c)) return 0;
  if (c >= 0x10000) { c -= 0x10000; out[(*n)++] = (WCHAR)(0xD800 + (c >> 10)); out[(*n)++] = (WCHAR)(0xDC00 + (c & 0x3FF)); }
  else out[(*n)++] = (WCHAR)c;
  return 1;
}
/* The text of s[0..n) with the references (&lt; &#65; ...) replaced; NULL for a malformed one (or, in an attribute value, a '<'). */
static WCHAR *xp_decode(const WCHAR *s, SIZE_T n, int attribute) {
  if (n > ACT_MAX_SIZE / 4) return NULL;
  WCHAR *r = act_alloc((n + 1) * 2);
  if (!r) return NULL;
  SIZE_T o = 0;
  for (SIZE_T i = 0; i < n;) {
    const WCHAR c = s[i];
    if (c == '<' && attribute) goto bad;
    if (c != '&') {
      if (c >= 0xD800 && c <= 0xDBFF && i + 1 < n && s[i + 1] >= 0xDC00 && s[i + 1] <= 0xDFFF) { r[o++] = c; r[o++] = s[i + 1]; i += 2; continue; }
      if (!xp_char_ok(c)) goto bad;
      r[o++] = (attribute && (c == '\t' || c == '\n' || c == '\r')) ? ' ' : c;
      ++i;
      continue;
    }
    SIZE_T j = i + 1;
    while (j < n && s[j] != ';' && j - i < 12) ++j;
    if (j >= n || s[j] != ';') goto bad;
    const WCHAR *e = s + i + 1;
    const SIZE_T el = j - i - 1;
    ULONG v = 0;
    if (el == 2 && e[0] == 'l' && e[1] == 't') v = '<';
    else if (el == 2 && e[0] == 'g' && e[1] == 't') v = '>';
    else if (el == 3 && e[0] == 'a' && e[1] == 'm' && e[2] == 'p') v = '&';
    else if (el == 4 && e[0] == 'q' && e[1] == 'u' && e[2] == 'o' && e[3] == 't') v = '"';
    else if (el == 4 && e[0] == 'a' && e[1] == 'p' && e[2] == 'o' && e[3] == 's') v = '\'';
    else if (el >= 2 && e[0] == '#') {
      int base = 10;
      SIZE_T k = 1;
      if (e[1] == 'x') { base = 16; k = 2; }
      if (k >= el) goto bad;
      for (; k < el; ++k) {
        const WCHAR d = e[k];
        int dv = d >= '0' && d <= '9' ? d - '0' : (base == 16 && d >= 'a' && d <= 'f') ? d - 'a' + 10 : (base == 16 && d >= 'A' && d <= 'F') ? d - 'A' + 10 : -1;
        if (dv < 0) goto bad;
        v = v * (ULONG)base + (ULONG)dv;
        if (v > 0x10FFFF) goto bad;
      }
    } else goto bad;
    if (!xp_put_char(r, &o, v)) goto bad;
    i = j + 1;
  }
  r[o] = 0;
  return r;
bad:
  act_free(r);
  return NULL;
}

static int xp_text_append(XNode *n, const WCHAR *s, SIZE_T len) {
  if (!len) return 1;
  if (len > ACT_MAX_SIZE - n->text_len - 2) return 0;
  if (n->text_len + len + 1 > n->text_cap) {
    SIZE_T cap = n->text_cap ? n->text_cap : 32;
    while (cap < n->text_len + len + 1) cap *= 2;
    WCHAR *t = act_alloc(cap * 2);
    if (!t) return 0;
    if (n->text) { memcpy(t, n->text, n->text_len * 2); act_free(n->text); }
    n->text = t;
    n->text_cap = cap;
  }
  memcpy(n->text + n->text_len, s, len * 2);
  n->text_len += len;
  n->text[n->text_len] = 0;
  return 1;
}

static void xp_skip_misc(XP *x) {      /* whitespace, comments and processing instructions */
  for (;;) {
    xp_skip_ws(x);
    if (xp_looking_at(x, "<!--")) {
      const WCHAR *p = x->p + 4;
      while (p + 2 < x->end && !(p[0] == '-' && p[1] == '-' && p[2] == '>')) { if (p[0] == '-' && p[1] == '-') { x->failed = 1; return; } ++p; }
      if (p + 2 >= x->end) { x->failed = 1; return; }
      x->p = p + 3;
    } else if (xp_looking_at(x, "<?")) {
      const WCHAR *p = x->p + 2;
      while (p + 1 < x->end && !(p[0] == '?' && p[1] == '>')) ++p;
      if (p + 1 >= x->end) { x->failed = 1; return; }
      x->p = p + 2;
    } else return;
  }
}

static XNode *xp_element(XP *x);
static XNode *xp_element(XP *x) {
  if (x->failed || x->depth >= ACT_MAX_DEPTH || x->p >= x->end || *x->p != '<') { x->failed = 1; return NULL; }
  ++x->p;
  XNode *n = act_alloc(sizeof(XNode));
  if (!n) { x->failed = 1; return NULL; }
  x->depth++;
  n->qname = xp_name(x);
  if (!n->qname) goto fail;
  ActVec attrs = { 0, 0, 0, sizeof(XAttr) };
  for (;;) {
    const WCHAR *before = x->p;
    xp_skip_ws(x);
    if (x->p >= x->end) goto bad_attrs;
    if (*x->p == '>' || *x->p == '/') break;
    if (x->p == before) goto bad_attrs;      /* (attributes are separated by white space) */
    WCHAR *an = xp_name(x);
    if (!an) goto bad_attrs;
    xp_skip_ws(x);
    if (x->p >= x->end || *x->p != '=') { act_free(an); goto bad_attrs; }
    ++x->p;
    xp_skip_ws(x);
    if (x->p >= x->end || (*x->p != '"' && *x->p != '\'')) { act_free(an); goto bad_attrs; }
    const WCHAR q = *x->p++;
    const WCHAR *vs = x->p;
    while (x->p < x->end && *x->p != q) ++x->p;
    if (x->p >= x->end) { act_free(an); goto bad_attrs; }
    WCHAR *val = xp_decode(vs, (SIZE_T)(x->p - vs), 1);
    ++x->p;
    XAttr *slot = val ? act_vec_add(&attrs) : NULL;
    if (!slot) { act_free(an); act_free(val); goto bad_attrs; }
    slot->qname = an;
    slot->value = val;
    for (ULONG i = 0; i + 1 < attrs.count; ++i) if (wcscmp(((XAttr *)attrs.data)[i].qname, an) == 0) goto bad_attrs;
  }
  n->attrs = (XAttr *)attrs.data;
  n->nattrs = attrs.count;
  if (*x->p == '/') {
    if (x->p + 1 >= x->end || x->p[1] != '>') goto fail;
    x->p += 2;
    x->depth--;
    return n;
  }
  ++x->p;      /* '>' */
  for (;;) {
    const WCHAR *ts = x->p;
    while (x->p < x->end && *x->p != '<') ++x->p;
    if (x->p > ts) {
      WCHAR *t = xp_decode(ts, (SIZE_T)(x->p - ts), 0);
      if (!t || !xp_text_append(n, t, wcslen(t))) { act_free(t); goto fail; }
      act_free(t);
    }
    if (x->p >= x->end) goto fail;
    if (xp_looking_at(x, "</")) {
      x->p += 2;
      WCHAR *en = xp_name(x);
      if (!en) goto fail;
      const int same = wcscmp(en, n->qname) == 0;
      act_free(en);
      xp_skip_ws(x);
      if (!same || x->p >= x->end || *x->p != '>') goto fail;
      ++x->p;
      x->depth--;
      return n;
    }
    if (xp_looking_at(x, "<!--") || xp_looking_at(x, "<?")) { xp_skip_misc(x); if (x->failed) goto fail; continue; }
    if (xp_looking_at(x, "<![CDATA[")) {
      const WCHAR *p = x->p + 9, *s = p;
      while (p + 2 < x->end && !(p[0] == ']' && p[1] == ']' && p[2] == '>')) ++p;
      if (p + 2 >= x->end || !xp_text_append(n, s, (SIZE_T)(p - s))) goto fail;
      x->p = p + 3;
      continue;
    }
    if (xp_looking_at(x, "<!")) goto fail;
    XNode *c = xp_element(x);
    if (!c) goto fail;
    if (n->last) n->last->next = c; else n->first = c;
    n->last = c;
  }
bad_attrs:
  for (ULONG i = 0; i < attrs.count; ++i) { act_free(((XAttr *)attrs.data)[i].qname); act_free(((XAttr *)attrs.data)[i].value); }
  act_free(attrs.data);
fail:
  x->failed = 1;
  x->depth--;
  xml_free(n);
  return NULL;
}

/* The namespaces of a parsed tree: each element's and attribute's local name and namespace (an unprefixed attribute has none). */
typedef struct XScope { const WCHAR *prefix; const WCHAR *uri; const struct XScope *outer; } XScope;
static const WCHAR *xml_lookup(const XScope *s, const WCHAR *prefix, SIZE_T plen) {
  for (; s; s = s->outer) {
    const SIZE_T l = s->prefix ? wcslen(s->prefix) : 0;
    if (l == plen && (plen == 0 || memcmp(s->prefix, prefix, plen * 2) == 0)) return s->uri;
  }
  return NULL;
}
static int xml_resolve(XNode *n, const XScope *outer, int depth) {
  if (depth > ACT_MAX_DEPTH) return 0;
  for (; n; n = n->next) {
    XScope scopes[16];
    ULONG used = 0;
    const XScope *scope = outer;
    for (ULONG i = 0; i < n->nattrs; ++i) {
      XAttr *a = &n->attrs[i];
      if (act_eq_ascii(a->qname, "xmlns", 0)) {
        if (used >= 16) return 0;
        scopes[used] = (XScope){ NULL, a->value, scope };
        scope = &scopes[used++];
      } else if (wcsncmp(a->qname, L"xmlns:", 6) == 0) {
        if (used >= 16 || !a->qname[6]) return 0;
        scopes[used] = (XScope){ a->qname + 6, a->value, scope };
        scope = &scopes[used++];
      }
    }
    const WCHAR *colon = wcschr(n->qname, ':');
    const SIZE_T plen = colon ? (SIZE_T)(colon - n->qname) : 0;
    n->local = colon ? (WCHAR *)colon + 1 : n->qname;
    n->ns = (WCHAR *)xml_lookup(scope, n->qname, plen);
    if (colon && !n->ns) n->ns = NULL;      /* (a prefix nobody declared: the element has no namespace) */
    for (ULONG i = 0; i < n->nattrs; ++i) {
      XAttr *a = &n->attrs[i];
      const WCHAR *c = wcschr(a->qname, ':');
      a->local = c ? (WCHAR *)c + 1 : a->qname;
      a->prefix = c ? a->qname : NULL;
      a->ns = c ? (WCHAR *)xml_lookup(scope, a->qname, (SIZE_T)(c - a->qname)) : NULL;
    }
    if (!xml_resolve(n->first, scope, depth + 1)) return 0;
  }
  return 1;
}
static int xml_is_xmlns(const XAttr *a) { return act_eq_ascii(a->qname, "xmlns", 0) || wcsncmp(a->qname, L"xmlns:", 6) == 0; }

/* The text as a UTF-16 string: UTF-8 (with or without the byte order mark) or UTF-16 of either byte order (with it; without it the zero bytes are not text). */
static WCHAR *act_decode_text(const BYTE *b, SIZE_T n, SIZE_T *chars) {
  if (n > ACT_MAX_SIZE / 2) return NULL;
  WCHAR *r = act_alloc((n + 2) * 2);
  if (!r) return NULL;
  SIZE_T o = 0;
  if (n >= 2 && ((b[0] == 0xFF && b[1] == 0xFE) || (b[0] == 0xFE && b[1] == 0xFF))) {
    const int big = b[0] == 0xFE;
    for (SIZE_T i = 2; i + 1 < n; i += 2) r[o++] = big ? (WCHAR)((b[i] << 8) | b[i + 1]) : (WCHAR)(b[i] | (b[i + 1] << 8));
    if (n & 1) { act_free(r); return NULL; }
  } else {
    SIZE_T i = (n >= 3 && b[0] == 0xEF && b[1] == 0xBB && b[2] == 0xBF) ? 3 : 0;
    while (i < n) {
      const BYTE c = b[i];
      ULONG cp;
      int extra;
      if (c < 0x80) { cp = c; extra = 0; }
      else if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; extra = 1; }
      else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; extra = 2; }
      else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; extra = 3; }
      else { act_free(r); return NULL; }
      if (i + (SIZE_T)extra >= n + (extra ? 0 : 1) && extra) { act_free(r); return NULL; }
      for (int k = 1; k <= extra; ++k) {
        if (i + (SIZE_T)k >= n || (b[i + k] & 0xC0) != 0x80) { act_free(r); return NULL; }
        cp = (cp << 6) | (b[i + k] & 0x3F);
      }
      i += (SIZE_T)extra + 1;
      if (cp >= 0x10000) { cp -= 0x10000; r[o++] = (WCHAR)(0xD800 + (cp >> 10)); r[o++] = (WCHAR)(0xDC00 + (cp & 0x3FF)); }
      else r[o++] = (WCHAR)cp;
    }
  }
  r[o] = 0;
  *chars = o;
  return r;
}

/* A whole document: the prolog, one root element, then nothing but comments and white space. */
static XNode *xml_parse(const BYTE *bytes, SIZE_T n) {
  SIZE_T chars = 0;
  WCHAR *text = act_decode_text(bytes, n, &chars);
  if (!text) return NULL;
  XP x = { text, text + chars, 0, 0 };
  if (xp_looking_at(&x, "<?xml") && x.p + 5 < x.end && xp_space(x.p[5])) {
    while (x.p + 1 < x.end && !(x.p[0] == '?' && x.p[1] == '>')) ++x.p;
    if (x.p + 1 >= x.end) { act_free(text); return NULL; }
    x.p += 2;
  }
  xp_skip_misc(&x);
  XNode *root = x.failed ? NULL : xp_element(&x);
  if (root) {
    xp_skip_misc(&x);
    if (x.failed || x.p != x.end || !xml_resolve(root, NULL, 0)) { xml_free(root); root = NULL; }
  }
  act_free(text);
  return root;
}

static const XAttr *xml_attr(const XNode *n, const char *name) {
  for (ULONG i = 0; i < n->nattrs; ++i) if (!xml_is_xmlns(&n->attrs[i]) && !n->attrs[i].ns && act_eq_ascii(n->attrs[i].local, name, 0)) return &n->attrs[i];
  return NULL;
}
static const WCHAR *xml_attr_value(const XNode *n, const char *name) { const XAttr *a = xml_attr(n, name); return a ? a->value : NULL; }
static int xml_is(const XNode *n, const char *name) { return act_eq_ascii(n->local, name, 0); }
static int xml_ns_is(const XNode *n, const char *uri) { return n->ns && act_eq_ascii(n->ns, uri, 0); }
#define NS_ASM1 "urn:schemas-microsoft-com:asm.v1"
#define NS_ASM2 "urn:schemas-microsoft-com:asm.v2"
#define NS_ASM3 "urn:schemas-microsoft-com:asm.v3"
#define NS_COMPAT "urn:schemas-microsoft-com:compatibility.v1"
#define NS_WINRT "urn:schemas-microsoft-com:winrt.v1"
static int xml_ns_asm(const XNode *n) { return xml_ns_is(n, NS_ASM1) || xml_ns_is(n, NS_ASM2) || xml_ns_is(n, NS_ASM3); }
/* The text of an element without the white space around it. */
static WCHAR *xml_text(const XNode *n) {
  const WCHAR *s = n->text ? n->text : L"";
  SIZE_T len = n->text ? n->text_len : 0;
  while (len && xp_space(*s)) { ++s; --len; }
  while (len && xp_space(s[len - 1])) --len;
  return act_wdup(s, len);
}

/* ---- the model --------------------------------------------------------------------------------------------------------------------------------------------- */

typedef struct ActIdentity { WCHAR *name, *type, *version, *arch, *token, *language; WORD ver[4]; } ActIdentity;
typedef struct ActFile { WCHAR *name; WCHAR *hash; WCHAR *hash_alg; } ActFile;
typedef struct ActWndClass { WCHAR *name; int versioned; ULONG file; } ActWndClass;
typedef struct ActComClass {
  ActGuid clsid, tlbid;
  int has_tlbid, is_clr;
  WCHAR *description, *progid, *clr_name, *clr_version;
  ActVec progids;      /* the <progid> children (WCHAR *) */
  ULONG model, misc_mask, misc[5];      /* the ThreadingModel_ number; which of the five miscStatus values were given, and the values (status, icon, content, thumbnail, docprint) */
  ULONG file;
} ActComClass;
typedef struct ActIfacePS { ActGuid iid, tlbid, ps32, base; ULONG num_methods, mask, file; int external, has_ps32; WCHAR *name; } ActIfacePS;
typedef struct ActTypelib { ActGuid tlbid; WORD major, minor, langid, flags; WCHAR *helpdir; ULONG file; } ActTypelib;
typedef struct ActSurrogate { ActGuid clsid; WCHAR *name, *version; } ActSurrogate;
typedef struct ActWinrt { WCHAR *name; ULONG model, file; } ActWinrt;
typedef struct ActAssembly {
  ULONG flags;
  WCHAR *manifest_path, *dir_name, *encoded;
  ActIdentity id;
  int has_id;
  ULONG manifest_major, manifest_minor;
  ActVec files, wndclasses, comclasses, ifaceps, typelibs, surrogates, winrts;
} ActAssembly;

typedef struct ActSetting { WCHAR *ns, *name, *value; } ActSetting;
typedef struct ActCompat { ActGuid id; ULONG type; ULONGLONG max_version; } ActCompat;
typedef struct ActCtx {
  ULONG magic;
  volatile LONG refs;
  ULONG flags;
  WCHAR *root_manifest, *app_dir;
  ULONG run_level, ui_access;
  int seen_exec_level, seen_settings;
  ActVec settings, compat;
  ActVec assemblies;
  ActBlob sections[ACT_SECTION_COUNT];
  ULONG section_count[ACT_SECTION_COUNT];
} ActCtx;

static void act_identity_free(ActIdentity *i) { act_free(i->name); act_free(i->type); act_free(i->version); act_free(i->arch); act_free(i->token); act_free(i->language); }

static void act_ctx_destroy(ActCtx *c) {
  for (ULONG i = 0; i < c->assemblies.count; ++i) {
    ActAssembly *a = act_vec_at(&c->assemblies, i);
    act_free(a->manifest_path); act_free(a->dir_name); act_free(a->encoded);
    act_identity_free(&a->id);
    for (ULONG k = 0; k < a->files.count; ++k) { ActFile *f = act_vec_at(&a->files, k); act_free(f->name); act_free(f->hash); act_free(f->hash_alg); }
    act_free(a->files.data);
    for (ULONG k = 0; k < a->wndclasses.count; ++k) act_free(((ActWndClass *)act_vec_at(&a->wndclasses, k))->name);
    act_free(a->wndclasses.data);
    for (ULONG k = 0; k < a->comclasses.count; ++k) {
      ActComClass *cc = act_vec_at(&a->comclasses, k);
      act_free(cc->description); act_free(cc->progid); act_free(cc->clr_name); act_free(cc->clr_version);
      for (ULONG m = 0; m < cc->progids.count; ++m) act_free(*(WCHAR **)act_vec_at(&cc->progids, m));
      act_free(cc->progids.data);
    }
    act_free(a->comclasses.data);
    for (ULONG k = 0; k < a->ifaceps.count; ++k) act_free(((ActIfacePS *)act_vec_at(&a->ifaceps, k))->name);
    act_free(a->ifaceps.data);
    for (ULONG k = 0; k < a->typelibs.count; ++k) act_free(((ActTypelib *)act_vec_at(&a->typelibs, k))->helpdir);
    act_free(a->typelibs.data);
    for (ULONG k = 0; k < a->surrogates.count; ++k) { ActSurrogate *sg = act_vec_at(&a->surrogates, k); act_free(sg->name); act_free(sg->version); }
    act_free(a->surrogates.data);
    for (ULONG k = 0; k < a->winrts.count; ++k) act_free(((ActWinrt *)act_vec_at(&a->winrts, k))->name);
    act_free(a->winrts.data);
  }
  act_free(c->assemblies.data);
  for (ULONG i = 0; i < c->settings.count; ++i) { ActSetting *st = act_vec_at(&c->settings, i); act_free(st->ns); act_free(st->name); act_free(st->value); }
  act_free(c->settings.data);
  act_free(c->compat.data);
  for (int s = 0; s < ACT_SECTION_COUNT; ++s) act_free(c->sections[s].p);
  act_free(c->root_manifest);
  act_free(c->app_dir);
  c->magic = 0;
  act_free(c);
}

/* "1.2.3.4": four numbers of at most 65535. */
static int act_parse_version(const WCHAR *s, WORD out[4]) {
  for (int k = 0; k < 4; ++k) {
    ULONG v = 0;
    int digits = 0;
    while (*s >= '0' && *s <= '9') { v = v * 10 + (ULONG)(*s++ - '0'); if (++digits > 5 || v > 65535) return 0; }
    if (!digits) return 0;
    out[k] = (WORD)v;
    if (k < 3) { if (*s++ != '.') return 0; }
  }
  return *s == 0;
}

static int act_parse_identity(const XNode *n, ActIdentity *id) {
  memset(id, 0, sizeof(*id));
  for (ULONG i = 0; i < n->nattrs; ++i) {
    const XAttr *a = &n->attrs[i];
    if (xml_is_xmlns(a)) continue;
    WCHAR **dst = act_eq_ascii(a->local, "name", 0) ? &id->name : act_eq_ascii(a->local, "type", 0) ? &id->type : act_eq_ascii(a->local, "version", 0) ? &id->version :
                  act_eq_ascii(a->local, "processorArchitecture", 0) ? &id->arch : act_eq_ascii(a->local, "publicKeyToken", 0) ? &id->token :
                  (act_eq_ascii(a->local, "language", 0) || act_eq_ascii(a->local, "culture", 0)) ? &id->language : NULL;
    if (!dst) continue;
    act_free(*dst);
    *dst = act_wstrdup(a->value);
    if (!*dst) return 0;
  }
  if (!id->name || !id->name[0]) return 0;
  if (id->version && !act_parse_version(id->version, id->ver)) return 0;
  if (id->token && id->token[0]) {      /* (16 hex digits) */
    SIZE_T l = wcslen(id->token);
    if (l != 16) return 0;
    for (SIZE_T k = 0; k < l; ++k) { const WCHAR c = id->token[k]; if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) return 0; }
  }
  return 1;
}

/* name,language="..",processorArchitecture="..",publicKeyToken="..",type="..",version=".." - the attributes that are there, in this order. */
static WCHAR *act_encode_identity(const ActIdentity *id) {
  const struct { const char *name; const WCHAR *value; } parts[5] = {
    { "language", id->language }, { "processorArchitecture", id->arch }, { "publicKeyToken", id->token }, { "type", id->type }, { "version", id->version } };
  SIZE_T n = wcslen(id->name) + 1;
  for (int i = 0; i < 5; ++i) if (parts[i].value) n += 1 + strlen(parts[i].name) + 2 + wcslen(parts[i].value) + 1;
  WCHAR *r = act_alloc((n + 1) * 2), *o = r;
  if (!r) return NULL;
  wcscpy(o, id->name);
  o += wcslen(o);
  for (int i = 0; i < 5; ++i) {
    if (!parts[i].value) continue;
    *o++ = ',';
    for (const char *c = parts[i].name; *c; ++c) *o++ = (BYTE)*c;
    *o++ = '='; *o++ = '"';
    wcscpy(o, parts[i].value);
    o += wcslen(o);
    *o++ = '"';
  }
  *o = 0;
  return r;
}

static ULONG act_hash_name(const WCHAR *s, SIZE_T n) { ULONG h = 0; while (n--) h = h * 65599 + (ULONG)RtlUpcaseUnicodeChar(*s++); return h; }

/* The directory name Windows gives an assembly in its store: arch_name_token_version_language_hash. */
static WCHAR *act_assembly_dir_name(const ActIdentity *id, const WCHAR *encoded) {
  WCHAR hash[17];
  const ULONG h = act_hash_name(encoded, wcslen(encoded));
  for (int i = 0; i < 16; ++i) hash[i] = (WCHAR)"0123456789abcdef"[i < 8 ? 0 : (h >> ((15 - i) * 4)) & 15];
  hash[16] = 0;
  const WCHAR *parts[5] = { id->arch ? id->arch : L"none", id->name, id->token ? id->token : L"none", id->version ? id->version : L"0.0.0.0", id->language ? id->language : L"none" };
  SIZE_T n = 0;
  for (int i = 0; i < 5; ++i) n += wcslen(parts[i]) + 1;
  WCHAR *r = act_alloc((n + 17) * 2), *o = r;
  if (!r) return NULL;
  for (int i = 0; i < 5; ++i) { wcscpy(o, parts[i]); o += wcslen(o); *o++ = '_'; }
  wcscpy(o, hash);
  for (WCHAR *c = r; *c; ++c) if (*c >= 'A' && *c <= 'Z') *c += 32;
  return r;
}

/* Does the manifest's identity satisfy the one asked for? Name, type and version must be the same; the architecture, language and key token must be too unless one side
 * leaves it open ("*" or missing). */
static int act_identity_matches(const ActIdentity *want, const ActIdentity *have) {
  if (!act_ieq(want->name, have->name)) return 0;
  if (want->type && have->type && !act_ieq(want->type, have->type)) return 0;
  if (want->version && have->version && memcmp(want->ver, have->ver, sizeof(want->ver)) != 0) return 0;
  if (want->arch && have->arch && want->arch[0] != '*' && have->arch[0] != '*' && !act_ieq(want->arch, have->arch)) return 0;
  if (want->token && have->token && want->token[0] != '*' && have->token[0] != '*' && !act_ieq(want->token, have->token)) return 0;
  if (want->language && have->language && want->language[0] != '*' && have->language[0] != '*' && !act_ieq(want->language, have->language)) return 0;
  return 1;
}

typedef struct ActBuild {
  ActCtx *ctx;
  const WCHAR *search_dir, *app_dir;      /* where assemblies are looked for: next to the manifest (or the directory the caller named), then in the application's */
  int depth;
} ActBuild;

static NTSTATUS act_load_dependencies(ActBuild *b, ULONG assembly_index, const XNode *root);

static int act_hex_ok(const WCHAR *s, SIZE_T want) {
  if (wcslen(s) != want) return 0;
  for (; *s; ++s) if (!((*s >= '0' && *s <= '9') || (*s >= 'a' && *s <= 'f') || (*s >= 'A' && *s <= 'F'))) return 0;
  return 1;
}


static int act_parse_guid(const WCHAR *s, ActGuid *g) {
  if (!s || wcslen(s) != 38 || s[0] != '{' || s[37] != '}' || s[9] != '-' || s[14] != '-' || s[19] != '-' || s[24] != '-') return 0;
  const int pos[11] = { 1, 10, 15, 20, 22, 25, 27, 29, 31, 33, 35 }, len[11] = { 8, 4, 4, 2, 2, 2, 2, 2, 2, 2, 2 };
  ULONG v[11];
  for (int k = 0; k < 11; ++k) {
    ULONG x = 0;
    for (int i = 0; i < len[k]; ++i) {
      const WCHAR c = s[pos[k] + i];
      const int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
      if (d < 0) return 0;
      x = x * 16 + (ULONG)d;
    }
    v[k] = x;
  }
  g->d1 = v[0]; g->d2 = (USHORT)v[1]; g->d3 = (USHORT)v[2];
  for (int i = 0; i < 8; ++i) g->d4[i] = (BYTE)v[3 + i];
  return 1;
}

/* A word list like "cantlinkinside,recomposeonresize": the OLEMISC bits (0 and a failure for a word that is not one). */
static int act_parse_misc(const WCHAR *s, ULONG *out) {
  static const struct { const char *name; ULONG bit; } words[] = {
    { "recomposeonresize", 0x1 }, { "onlyiconic", 0x2 }, { "insertnotreplace", 0x4 }, { "static", 0x8 }, { "cantlinkinside", 0x10 }, { "canlinkbyole1", 0x20 },
    { "islinkobject", 0x40 }, { "insideout", 0x80 }, { "activatewhenvisible", 0x100 }, { "renderingisdeviceindependent", 0x200 }, { "invisibleatruntime", 0x400 },
    { "alwaysrun", 0x800 }, { "actslikebutton", 0x1000 }, { "actslikelabel", 0x2000 }, { "nouiactivate", 0x4000 }, { "alignable", 0x8000 }, { "simpleframe", 0x10000 },
    { "setclientsitefirst", 0x20000 }, { "imemode", 0x40000 }, { "ignoreactivatewhenvisible", 0x80000 }, { "wantstomenumerge", 0x100000 },
    { "supportsmultilevelundo", 0x200000 } };
  ULONG bits = 0;
  while (*s) {
    while (*s == ' ' || *s == ',') ++s;
    const WCHAR *e = s;
    while (*e && *e != ',' && *e != ' ') ++e;
    if (e == s) break;
    int found = 0;
    for (ULONG i = 0; i < sizeof(words) / sizeof(words[0]) && !found; ++i) {
      const char *w = words[i].name;
      SIZE_T k = 0;
      while (w[k] && s + k < e && (s[k] | 0x20) == (WCHAR)(BYTE)w[k]) ++k;
      if (!w[k] && s + k == e) { bits |= words[i].bit; found = 1; }
    }
    if (!found) return 0;
    s = e;
  }
  *out = bits;
  return 1;
}

static ULONG act_threading_model(const WCHAR *s) {
  if (!s) return 3;      /* ThreadingModel_No */
  if (act_eq_ascii(s, "Apartment", 1)) return 1;
  if (act_eq_ascii(s, "Free", 1)) return 2;
  if (act_eq_ascii(s, "Both", 1)) return 4;
  if (act_eq_ascii(s, "Neutral", 1)) return 5;
  return 3;
}

static int act_add_progid(ActComClass *cc, const WCHAR *text) {
  if (!text || !text[0]) return 0;
  WCHAR **slot = act_vec_add(&cc->progids);
  if (!slot) return 0;
  *slot = act_wstrdup(text);
  return *slot != NULL;
}

/* <comClass> (in a file) and <clrClass> (in the assembly). */
static NTSTATUS act_parse_comclass(ActCtx *ctx, ULONG ai, const XNode *n, ULONG file, int is_clr) {
  ActAssembly *a = act_vec_at(&ctx->assemblies, ai);
  ActComClass cc;
  memset(&cc, 0, sizeof(cc));
  cc.progids.elem = sizeof(WCHAR *);
  cc.file = file;
  cc.is_clr = is_clr;
  NTSTATUS status = SXS_CANT_GEN_ACTCTX;
  if (!act_parse_guid(xml_attr_value(n, "clsid"), &cc.clsid)) goto done;
  const WCHAR *tlb = xml_attr_value(n, "tlbid");
  if (tlb) { if (!act_parse_guid(tlb, &cc.tlbid)) goto done; cc.has_tlbid = 1; }
  cc.model = act_threading_model(xml_attr_value(n, "threadingModel"));
  cc.description = act_wstrdup(xml_attr_value(n, "description"));
  const WCHAR *pid = xml_attr_value(n, "progid");
  if (pid && pid[0]) { cc.progid = act_wstrdup(pid); if (!cc.progid) goto done; }
  if (is_clr) {
    const WCHAR *name = xml_attr_value(n, "name"), *ver = xml_attr_value(n, "runtimeVersion");
    if (!name) goto done;
    cc.clr_name = act_wstrdup(name);
    cc.clr_version = act_wstrdup(ver ? ver : L"");
    if (!cc.clr_name || !cc.clr_version) goto done;
  } else {
    static const char *const attrs[5] = { "miscStatus", "miscStatusIcon", "miscStatusContent", "miscStatusThumbnail", "miscStatusDocPrint" };
    for (int i = 0; i < 5; ++i) {
      const WCHAR *v = xml_attr_value(n, attrs[i]);
      if (!v) continue;
      ULONG bits;
      if (!act_parse_misc(v, &bits)) goto done;
      cc.misc[i] = bits;
      cc.misc_mask |= 1u << i;
    }
  }
  for (const XNode *c = n->first; c; c = c->next) {
    if (xml_ns_asm(c) && xml_is(c, "progid")) {
      WCHAR *t = xml_text(c);
      const int ok = act_add_progid(&cc, t);
      act_free(t);
      if (!ok) goto done;
    }
  }
  {
    ActComClass *slot = act_vec_add(&a->comclasses);
    if (!slot) goto done;
    *slot = cc;
    memset(&cc, 0, sizeof(cc));
    status = STATUS_SUCCESS;
  }
done:
  if (cc.progids.data || cc.description || cc.progid || cc.clr_name || cc.clr_version) {
    act_free(cc.description); act_free(cc.progid); act_free(cc.clr_name); act_free(cc.clr_version);
    for (ULONG m = 0; m < cc.progids.count; ++m) act_free(*(WCHAR **)act_vec_at(&cc.progids, m));
    act_free(cc.progids.data);
  }
  return status;
}

static NTSTATUS act_parse_ifaceps(ActCtx *ctx, ULONG ai, const XNode *n, ULONG file, int external) {
  ActAssembly *a = act_vec_at(&ctx->assemblies, ai);
  ActIfacePS ip;
  memset(&ip, 0, sizeof(ip));
  ip.external = external;
  ip.file = file;
  const WCHAR *name = xml_attr_value(n, "name");
  if (!name || !act_parse_guid(xml_attr_value(n, "iid"), &ip.iid)) return SXS_CANT_GEN_ACTCTX;
  const WCHAR *tlb = xml_attr_value(n, "tlbid"), *ps = xml_attr_value(n, "proxyStubClsid32"), *base = xml_attr_value(n, "baseInterface"), *nm = xml_attr_value(n, "numMethods");
  if (tlb && !act_parse_guid(tlb, &ip.tlbid)) return SXS_CANT_GEN_ACTCTX;
  if (ps) { if (!act_parse_guid(ps, &ip.ps32)) return SXS_CANT_GEN_ACTCTX; ip.has_ps32 = 1; }
  if (base) { if (!act_parse_guid(base, &ip.base)) return SXS_CANT_GEN_ACTCTX; ip.mask |= 2; }
  if (nm) {
    ULONG v = 0;
    if (!*nm) return SXS_CANT_GEN_ACTCTX;
    for (const WCHAR *c = nm; *c; ++c) { if (*c < '0' || *c > '9' || v > 100000000) return SXS_CANT_GEN_ACTCTX; v = v * 10 + (ULONG)(*c - '0'); }
    ip.num_methods = v;
    ip.mask |= 1;
  }
  ip.name = act_wstrdup(name);
  ActIfacePS *slot = ip.name ? act_vec_add(&a->ifaceps) : NULL;
  if (!slot) { act_free(ip.name); return SXS_CANT_GEN_ACTCTX; }
  *slot = ip;
  return STATUS_SUCCESS;
}

static NTSTATUS act_parse_typelib(ActCtx *ctx, ULONG ai, const XNode *n, ULONG file) {
  ActAssembly *a = act_vec_at(&ctx->assemblies, ai);
  ActTypelib t;
  memset(&t, 0, sizeof(t));
  t.file = file;
  if (!act_parse_guid(xml_attr_value(n, "tlbid"), &t.tlbid)) return SXS_CANT_GEN_ACTCTX;
  const WCHAR *ver = xml_attr_value(n, "version"), *help = xml_attr_value(n, "helpdir"), *res = xml_attr_value(n, "resourceid"), *flags = xml_attr_value(n, "flags");
  if (!ver) return SXS_CANT_GEN_ACTCTX;
  {
    ULONG v[2] = { 0, 0 };
    int part = 0, digits = 0;
    for (const WCHAR *c = ver;; ++c) {
      if (*c >= '0' && *c <= '9') { v[part] = v[part] * 10 + (ULONG)(*c - '0'); if (++digits > 5 || v[part] > 65535) return SXS_CANT_GEN_ACTCTX; }
      else if (*c == '.' && part == 0 && digits) { part = 1; digits = 0; }
      else if (*c == 0 && part == 1 && digits) break;
      else return SXS_CANT_GEN_ACTCTX;
    }
    t.major = (WORD)v[0];
    t.minor = (WORD)v[1];
  }
  if (res) {      /* a language id in hexadecimal */
    ULONG v = 0;
    for (const WCHAR *c = res; *c; ++c) {
      const int d = *c >= '0' && *c <= '9' ? *c - '0' : *c >= 'a' && *c <= 'f' ? *c - 'a' + 10 : *c >= 'A' && *c <= 'F' ? *c - 'A' + 10 : -1;
      if (d < 0 || v > 0xFFF) return SXS_CANT_GEN_ACTCTX;
      v = v * 16 + (ULONG)d;
    }
    t.langid = (WORD)v;
  }
  if (flags) {
    static const struct { const char *name; WORD bit; } words[] = { { "restricted", 1 }, { "control", 2 }, { "hidden", 4 }, { "hasdiskimage", 8 } };
    const WCHAR *s = flags;
    while (*s) {
      while (*s == ' ' || *s == ',') ++s;
      const WCHAR *e = s;
      while (*e && *e != ',' && *e != ' ') ++e;
      if (e == s) break;
      int found = 0;
      for (ULONG i = 0; i < 4 && !found; ++i) {
        const char *w = words[i].name;
        SIZE_T k = 0;
        while (w[k] && s + k < e && (s[k] | 0x20) == (WCHAR)(BYTE)w[k]) ++k;
        if (!w[k] && s + k == e) { t.flags |= words[i].bit; found = 1; }
      }
      if (!found) return SXS_CANT_GEN_ACTCTX;
      s = e;
    }
  }
  t.helpdir = act_wstrdup(help ? help : L"");
  ActTypelib *slot = t.helpdir ? act_vec_add(&a->typelibs) : NULL;
  if (!slot) { act_free(t.helpdir); return SXS_CANT_GEN_ACTCTX; }
  *slot = t;
  return STATUS_SUCCESS;
}

static NTSTATUS act_parse_surrogate(ActCtx *ctx, ULONG ai, const XNode *n) {
  ActAssembly *a = act_vec_at(&ctx->assemblies, ai);
  ActSurrogate sg;
  memset(&sg, 0, sizeof(sg));
  const WCHAR *name = xml_attr_value(n, "name"), *ver = xml_attr_value(n, "runtimeVersion");
  if (!name || !act_parse_guid(xml_attr_value(n, "clsid"), &sg.clsid)) return SXS_CANT_GEN_ACTCTX;
  sg.name = act_wstrdup(name);
  sg.version = act_wstrdup(ver ? ver : L"");
  ActSurrogate *slot = sg.name && sg.version ? act_vec_add(&a->surrogates) : NULL;
  if (!slot) { act_free(sg.name); act_free(sg.version); return SXS_CANT_GEN_ACTCTX; }
  *slot = sg;
  return STATUS_SUCCESS;
}

static NTSTATUS act_parse_winrt(ActCtx *ctx, ULONG ai, const XNode *n, ULONG file) {
  ActAssembly *a = act_vec_at(&ctx->assemblies, ai);
  const WCHAR *name = xml_attr_value(n, "name"), *tm = xml_attr_value(n, "threadingModel");
  if (!name || !name[0] || !tm) return SXS_CANT_GEN_ACTCTX;
  ActWinrt w;
  memset(&w, 0, sizeof(w));
  w.file = file;
  if (act_eq_ascii(tm, "both", 1)) w.model = 0;
  else if (act_eq_ascii(tm, "sta", 1)) w.model = 1;
  else if (act_eq_ascii(tm, "mta", 1)) w.model = 2;
  else return SXS_CANT_GEN_ACTCTX;
  w.name = act_wstrdup(name);
  ActWinrt *slot = w.name ? act_vec_add(&a->winrts) : NULL;
  if (!slot) { act_free(w.name); return SXS_CANT_GEN_ACTCTX; }
  *slot = w;
  return STATUS_SUCCESS;
}

/* One <file>: its name, hash and the classes it registers. */
static NTSTATUS act_parse_file(ActBuild *b, ActAssembly **pa, ULONG ai, const XNode *fn) {
  ActAssembly *a = act_vec_at(&b->ctx->assemblies, ai);
  const WCHAR *name = xml_attr_value(fn, "name");
  if (!name || !name[0]) return SXS_CANT_GEN_ACTCTX;
  const WCHAR *hash = xml_attr_value(fn, "hash"), *alg = xml_attr_value(fn, "hashalg");
  if (hash) {
    const SIZE_T want = !alg || act_eq_ascii(alg, "SHA1", 1) ? 40 : act_eq_ascii(alg, "SHA256", 1) ? 64 : act_eq_ascii(alg, "SHA384", 1) ? 96 : act_eq_ascii(alg, "SHA512", 1) ? 128 :
                        act_eq_ascii(alg, "MD5", 1) ? 32 : 0;
    if (want && !act_hex_ok(hash, want)) return SXS_CANT_GEN_ACTCTX;
  }
  ActFile *f = act_vec_add(&a->files);
  if (!f) return SXS_CANT_GEN_ACTCTX;
  f->name = act_wstrdup(name);
  f->hash = act_wstrdup(hash);
  f->hash_alg = act_wstrdup(alg);
  if (!f->name) return SXS_CANT_GEN_ACTCTX;
  const ULONG file_index = a->files.count - 1;
  for (const XNode *c = fn->first; c; c = c->next) {
    if (xml_ns_is(c, NS_ASM1) || xml_ns_is(c, NS_ASM3)) {
      if (xml_is(c, "windowClass")) {
        WCHAR *text = xml_text(c);
        if (!text || !text[0]) { act_free(text); return SXS_CANT_GEN_ACTCTX; }
        const WCHAR *versioned = xml_attr_value(c, "versioned");
        a = act_vec_at(&b->ctx->assemblies, ai);
        ActWndClass *w = act_vec_add(&a->wndclasses);
        if (!w) { act_free(text); return SXS_CANT_GEN_ACTCTX; }
        w->name = text;
        w->versioned = !(versioned && (act_eq_ascii(versioned, "no", 1) || act_eq_ascii(versioned, "false", 1)));
        w->file = file_index;
      } else if (xml_is(c, "comClass")) {
        const NTSTATUS s = act_parse_comclass(b->ctx, ai, c, file_index, 0);
        if (s) return s;
      } else if (xml_is(c, "comInterfaceProxyStub")) {
        const NTSTATUS s = act_parse_ifaceps(b->ctx, ai, c, file_index, 0);
        if (s) return s;
      } else if (xml_is(c, "typelib")) {
        const NTSTATUS s = act_parse_typelib(b->ctx, ai, c, file_index);
        if (s) return s;
      } else return SXS_CANT_GEN_ACTCTX;
    } else if (xml_ns_is(c, NS_WINRT)) {
      if (!xml_is(c, "activatableClass")) return SXS_CANT_GEN_ACTCTX;
      const NTSTATUS s = act_parse_winrt(b->ctx, ai, c, file_index);
      if (s) return s;
    } else if (xml_is(c, "activatableClass")) return SXS_CANT_GEN_ACTCTX;
  }
  (void)pa;
  return STATUS_SUCCESS;
}


/* "10.0.18358" as 16-bit fields in a 64-bit number, most significant first. */
static int act_parse_packed_version(const WCHAR *s, ULONGLONG *out) {
  ULONGLONG v = 0;
  int parts = 0;
  for (;;) {
    ULONG x = 0;
    int digits = 0;
    while (*s >= '0' && *s <= '9') { x = x * 10 + (ULONG)(*s++ - '0'); if (++digits > 5 || x > 65535) return 0; }
    if (!digits || parts == 4) return 0;
    v |= (ULONGLONG)x << (48 - 16 * parts);
    ++parts;
    if (*s == '.') { ++s; continue; }
    if (*s) return 0;
    break;
  }
  *out = v;
  return 1;
}

/* <trustInfo><security><requestedPrivileges><requestedExecutionLevel level= uiAccess=/>: only one level may be asked for. */
static NTSTATUS act_parse_trust(ActCtx *ctx, const XNode *n) {
  for (const XNode *sec = n->first; sec; sec = sec->next) {
    if (!xml_ns_asm(sec) || !xml_is(sec, "security")) continue;
    for (const XNode *rp = sec->first; rp; rp = rp->next) {
      if (!xml_ns_asm(rp) || !xml_is(rp, "requestedPrivileges")) continue;
      for (const XNode *el = rp->first; el; el = el->next) {
        if (!xml_ns_asm(el) || !xml_is(el, "requestedExecutionLevel")) continue;
        if (ctx->seen_exec_level) return SXS_CANT_GEN_ACTCTX;
        ctx->seen_exec_level = 1;
        const WCHAR *level = xml_attr_value(el, "level"), *ui = xml_attr_value(el, "uiAccess");
        if (level) ctx->run_level = act_eq_ascii(level, "asInvoker", 1) ? 1 : act_eq_ascii(level, "highestAvailable", 1) ? 2 : act_eq_ascii(level, "requireAdministrator", 1) ? 3 : 0;
        ctx->ui_access = ui && act_eq_ascii(ui, "true", 1);
      }
    }
  }
  return STATUS_SUCCESS;
}

/* <application><windowsSettings><name xmlns="...">value</name>...: the first application element that has settings is the one that counts. */
static NTSTATUS act_parse_application(ActCtx *ctx, const XNode *n) {
  if (ctx->seen_settings) return STATUS_SUCCESS;
  for (const XNode *ws = n->first; ws; ws = ws->next) {
    if (!xml_ns_asm(ws) || !xml_is(ws, "windowsSettings")) continue;
    for (const XNode *st = ws->first; st; st = st->next) {
      ActSetting *slot = act_vec_add(&ctx->settings);
      if (!slot) return SXS_CANT_GEN_ACTCTX;
      slot->ns = act_wstrdup(st->ns ? st->ns : L"");
      slot->name = act_wstrdup(st->local);
      slot->value = xml_text(st);
      if (!slot->ns || !slot->name || !slot->value) return SXS_CANT_GEN_ACTCTX;
      ctx->seen_settings = 1;
    }
  }
  return STATUS_SUCCESS;
}

/* <compatibility xmlns="urn:schemas-microsoft-com:compatibility.v1"><application><supportedOS Id="{guid}"/><maxversiontested Id="10.0.18358"/>. */
static NTSTATUS act_parse_compat(ActCtx *ctx, const XNode *n) {
  for (const XNode *app = n->first; app; app = app->next) {
    if (!xml_ns_is(app, NS_COMPAT) || !xml_is(app, "application")) continue;
    for (const XNode *el = app->first; el; el = el->next) {
      if (!xml_ns_is(el, NS_COMPAT)) continue;
      ActCompat c;
      memset(&c, 0, sizeof(c));
      const WCHAR *id = xml_attr_value(el, "Id");
      if (xml_is(el, "supportedOS")) {
        if (!act_parse_guid(id, &c.id)) return SXS_CANT_GEN_ACTCTX;
        c.type = 1;
      } else if (xml_is(el, "maxversiontested")) {
        if (!id || !act_parse_packed_version(id, &c.max_version)) return SXS_CANT_GEN_ACTCTX;
        c.type = 3;
      } else continue;
      ActCompat *slot = act_vec_add(&ctx->compat);
      if (!slot) return SXS_CANT_GEN_ACTCTX;
      *slot = c;
    }
  }
  return STATUS_SUCCESS;
}

/* A parsed manifest becomes the next assembly of the context (and the assemblies it depends on follow it). */
static NTSTATUS act_add_manifest(ActBuild *b, const XNode *root, const WCHAR *path, int is_root, const ActIdentity *wanted) {
  ActCtx *ctx = b->ctx;
  if (!xml_is(root, "assembly") || !(xml_ns_is(root, NS_ASM1) || xml_ns_is(root, NS_ASM3))) return SXS_CANT_GEN_ACTCTX;
  const WCHAR *mv = NULL;
  for (ULONG i = 0; i < root->nattrs; ++i) {
    const XAttr *a = &root->attrs[i];
    if (xml_is_xmlns(a)) continue;
    if (!a->ns && act_eq_ascii(a->local, "manifestVersion", 0)) mv = a->value;
    else return SXS_CANT_GEN_ACTCTX;
  }
  ULONG major = 0, minor = 0;
  WORD ver[4];
  if (!mv) return SXS_CANT_GEN_ACTCTX;
  {
    WCHAR tmp[32];
    if (wcslen(mv) > 8) return SXS_CANT_GEN_ACTCTX;
    wcscpy(tmp, mv);
    wcscat(tmp, L".0.0");
    if (!act_parse_version(tmp, ver)) return SXS_CANT_GEN_ACTCTX;
    major = ver[0]; minor = ver[1];
  }
  if (ctx->assemblies.count >= ACT_MAX_ASSEMBLIES) return SXS_CANT_GEN_ACTCTX;
  ActAssembly *a = act_vec_add(&ctx->assemblies);
  if (!a) return SXS_CANT_GEN_ACTCTX;
  const ULONG ai = ctx->assemblies.count - 1;
  a->flags = is_root ? 1 : 0x10;
  a->files.elem = sizeof(ActFile);
  a->wndclasses.elem = sizeof(ActWndClass);
  a->comclasses.elem = sizeof(ActComClass);
  a->ifaceps.elem = sizeof(ActIfacePS);
  a->typelibs.elem = sizeof(ActTypelib);
  a->surrogates.elem = sizeof(ActSurrogate);
  a->winrts.elem = sizeof(ActWinrt);
  a->manifest_path = act_wstrdup(path);
  a->manifest_major = major;
  a->manifest_minor = minor;
  int seen_identity = 0;
  for (const XNode *c = root->first; c; c = c->next) {
    if (xml_ns_is(c, NS_COMPAT) && xml_is(c, "compatibility")) {
      if (is_root) { const NTSTATUS s = act_parse_compat(ctx, c); if (s) return s; }
      continue;
    }
    if (!xml_ns_asm(c)) continue;      /* (an element of another vocabulary is not ours) */
    if (xml_is(c, "assemblyIdentity")) {
      if (seen_identity) return SXS_CANT_GEN_ACTCTX;
      seen_identity = 1;
      a = act_vec_at(&ctx->assemblies, ai);
      if (!act_parse_identity(c, &a->id)) return SXS_CANT_GEN_ACTCTX;
      a->has_id = 1;
      a->encoded = act_encode_identity(&a->id);
      if (!a->encoded) return SXS_CANT_GEN_ACTCTX;
      if (!is_root) a->dir_name = act_assembly_dir_name(&a->id, a->encoded);
    } else if (xml_is(c, "file")) {
      a = act_vec_at(&ctx->assemblies, ai);
      const NTSTATUS s = act_parse_file(b, &a, ai, c);
      if (s) return s;
    } else if (xml_is(c, "comInterfaceExternalProxyStub")) {
      const NTSTATUS s = act_parse_ifaceps(ctx, ai, c, 0xFFFFFFFFu, 1);
      if (s) return s;
    } else if (xml_is(c, "clrSurrogate")) {
      const NTSTATUS s = act_parse_surrogate(ctx, ai, c);
      if (s) return s;
    } else if (xml_is(c, "clrClass")) {
      const NTSTATUS s = act_parse_comclass(ctx, ai, c, 0xFFFFFFFFu, 1);
      if (s) return s;
    } else if (xml_is(c, "trustInfo")) {
      if (is_root) { const NTSTATUS s = act_parse_trust(ctx, c); if (s) return s; }
    } else if (xml_is(c, "application")) {
      if (is_root) { const NTSTATUS s = act_parse_application(ctx, c); if (s) return s; }
    } else if (xml_is(c, "dependency") || xml_is(c, "description") || xml_is(c, "noInherit") || xml_is(c, "noInheritable") ||
               xml_is(c, "compatibility") || xml_is(c, "windowsSettings")) {
    } else return SXS_CANT_GEN_ACTCTX;
  }
  a = act_vec_at(&ctx->assemblies, ai);
  if (!is_root && !a->has_id) return SXS_CANT_GEN_ACTCTX;
  if (wanted && !act_identity_matches(wanted, &a->id)) return SXS_CANT_GEN_ACTCTX;
  return act_load_dependencies(b, ai, root);
}

/* ---- files ----------------------------------------------------------------------------------------------------------------------------------------------------- */

/* The bytes of a file (NULL if it is not there). */
static BYTE *act_read_file(const WCHAR *dos_path, SIZE_T *size) {
  UNICODE_STRING nt = { 0 };
  if (!RtlDosPathNameToNtPathName_U(dos_path, &nt, NULL, NULL)) return NULL;
  OBJECT_ATTRIBUTES oa = { sizeof(oa), NULL, &nt, 0x40 /* OBJ_CASE_INSENSITIVE */, NULL, NULL };
  IO_STATUS_BLOCK iosb;
  HANDLE h = NULL;
  const NTSTATUS s = NtCreateFile(&h, 0x80100000u, &oa, &iosb, NULL, 0, 7, 1, 0x60 /* FILE_NON_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT */, NULL, 0);
  RtlFreeHeap(process_heap(), 0, nt.Buffer);
  if (!NT_SUCCESS(s)) return NULL;
  BYTE info[24];
  BYTE *data = NULL;
  if (NT_SUCCESS(NtQueryInformationFile(h, &iosb, info, sizeof(info), 5 /* FileStandardInformation */))) {
    ULONGLONG len;
    memcpy(&len, info + 8, 8);
    if (len <= ACT_MAX_SIZE && (data = act_alloc((SIZE_T)len + 2)) != NULL) {
      SIZE_T got = 0;
      while (got < len) {
        const NTSTATUS rs = NtReadFile(h, NULL, NULL, NULL, &iosb, data + got, (ULONG)(len - got), NULL, NULL);
        if (!NT_SUCCESS(rs) || !iosb.Information) break;
        got += iosb.Information;
      }
      if (got != len) { act_free(data); data = NULL; } else *size = (SIZE_T)len;
    }
  }
  NtClose(h);
  return data;
}


/* The assemblies the system itself provides (Windows keeps them in WinSxS): the manifests are part of this library. A request is met by the one with the same name and
 * key whose version has the same major and minor numbers and is not older. */
#define ASM_HEAD "<assembly xmlns=\"urn:schemas-microsoft-com:asm.v1\" manifestVersion=\"1.0\"><assemblyIdentity type=\"win32\" name=\""
#define WC(n) "<windowClass>" n "</windowClass>"
#define VWC(n) "<windowClass versioned=\"yes\">" n "</windowClass>"
static const char *const g_builtin_assemblies[] = {
  ASM_HEAD "Microsoft.Windows.Common-Controls\" version=\"6.0.10586.0\" processorArchitecture=\"amd64\" publicKeyToken=\"6595b64144ccf1df\"/>"
  "<file name=\"comctl32.dll\">"
  VWC("ReBarWindow32") VWC("ComboBoxEx32") VWC("tooltips_class32") VWC("msctls_trackbar32") VWC("msctls_updown32") VWC("msctls_progress32") VWC("msctls_statusbar32")
  VWC("msctls_hotkey32") VWC("SysTreeView32") VWC("SysListView32") VWC("SysTabControl32") VWC("SysAnimate32") VWC("SysHeader32") VWC("SysIPAddress32") VWC("SysLink")
  VWC("SysPager") VWC("NativeFontCtl") VWC("Button") VWC("Edit") VWC("ListBox") VWC("ComboBox") VWC("ScrollBar") VWC("Static") VWC("ComboLBox") VWC("DropDown")
  "</file></assembly>",
  ASM_HEAD "Microsoft.Windows.GdiPlus\" version=\"1.0.10586.0\" processorArchitecture=\"amd64\" publicKeyToken=\"6595b64144ccf1df\"/><file name=\"gdiplus.dll\"/></assembly>",
  ASM_HEAD "Microsoft.Windows.GdiPlus\" version=\"1.1.10586.0\" processorArchitecture=\"amd64\" publicKeyToken=\"6595b64144ccf1df\"/><file name=\"gdiplus.dll\"/></assembly>",
  ASM_HEAD "Microsoft.VC80.CRT\" version=\"8.0.50727.9999\" processorArchitecture=\"amd64\" publicKeyToken=\"1fc8b3b9a1e18e3b\"/><file name=\"msvcr80.dll\"/><file name=\"msvcp80.dll\"/><file name=\"msvcm80.dll\"/></assembly>",
  ASM_HEAD "Microsoft.VC90.CRT\" version=\"9.0.30729.9999\" processorArchitecture=\"amd64\" publicKeyToken=\"1fc8b3b9a1e18e3b\"/><file name=\"msvcr90.dll\"/><file name=\"msvcp90.dll\"/><file name=\"msvcm90.dll\"/></assembly>",
};

static NTSTATUS act_load_builtin(ActBuild *b, const ActIdentity *want) {
  for (ULONG i = 0; i < sizeof(g_builtin_assemblies) / sizeof(g_builtin_assemblies[0]); ++i) {
    const char *xml = g_builtin_assemblies[i];
    XNode *root = xml_parse((const BYTE *)xml, strlen(xml));
    if (!root) continue;
    ActIdentity have;
    int match = 0;
    for (const XNode *c = root->first; c && !match; c = c->next) {
      if (!xml_is(c, "assemblyIdentity")) continue;
      if (!act_parse_identity(c, &have)) continue;
      ActIdentity relaxed = have;
      if (want->version && have.version && have.ver[0] == want->ver[0] && have.ver[1] == want->ver[1] &&
          (have.ver[2] > want->ver[2] || (have.ver[2] == want->ver[2] && have.ver[3] >= want->ver[3]))) memcpy(relaxed.ver, want->ver, sizeof(want->ver));
      match = act_identity_matches(want, &relaxed);
      act_identity_free(&have);
    }
    NTSTATUS status = SXS_CANT_GEN_ACTCTX;
    if (match) {
      ActIdentity id;
      for (const XNode *c = root->first; c; c = c->next) if (xml_is(c, "assemblyIdentity") && act_parse_identity(c, &id)) {
        WCHAR *enc = act_encode_identity(&id), *dir = enc ? act_assembly_dir_name(&id, enc) : NULL;
        WCHAR path[260];
        if (dir && wcslen(dir) < 200) {
          wcscpy(path, L"C:\\Windows\\WinSxS\\manifests\\");
          wcscat(path, dir);
          wcscat(path, L".manifest");
          b->depth++;
          status = act_add_manifest(b, root, path, 0, NULL);
          b->depth--;
        }
        act_free(enc); act_free(dir);
        act_identity_free(&id);
        break;
      }
    }
    xml_free(root);
    if (match) return status;
  }
  return SXS_CANT_GEN_ACTCTX;
}


/* The file at path as a source of the assembly: a manifest file, or a library with a manifest resource. SXS_ASSEMBLY_NOT_FOUND means there is nothing usable there; any
 * other failure (a manifest that is wrong, or for another assembly) ends the search. */
static NTSTATUS act_try_candidate(ActBuild *b, const WCHAR *path, const ActIdentity *want, int is_library) {
  SIZE_T size = 0;
  BYTE *data = act_read_file(path, &size);
  if (!data) return SXS_ASSEMBLY_NOT_FOUND;
  const BYTE *xml = data;
  SIZE_T xml_size = size;
  if (is_library) {
    xml = act_image_manifest(data, size, 1, 0, &xml_size);
    if (!xml) { act_free(data); return SXS_ASSEMBLY_NOT_FOUND; }
  }
  XNode *root = xml_parse(xml, xml_size);
  act_free(data);
  if (!root) return SXS_CANT_GEN_ACTCTX;
  b->depth++;
  const NTSTATUS s = act_add_manifest(b, root, path, 0, want);
  b->depth--;
  xml_free(root);
  return s;
}

/* Private assemblies are looked for under a directory (the one given for the context, then the application's) as the library or manifest named like the assembly, in the
 * directory itself or in one with the assembly's name; an assembly with a language is looked for only in the folder of that language. */
static NTSTATUS act_probe_assembly(ActBuild *b, const ActIdentity *want) {
  const WCHAR *dirs[2] = { b->search_dir, b->app_dir };
  const SIZE_T name_len = wcslen(want->name);
  const int by_language = want->language && want->language[0] && want->language[0] != '*';
  const SIZE_T lang_len = by_language ? wcslen(want->language) : 0;
  for (int d = 0; d < 2; ++d) {
    if (!dirs[d] || (d == 1 && b->search_dir && act_ieq(dirs[0], dirs[1]))) continue;
    const SIZE_T dir_len = wcslen(dirs[d]);
    WCHAR *path = act_alloc((dir_len + lang_len + 2 * name_len + 32) * 2);
    if (!path) return SXS_CANT_GEN_ACTCTX;
    for (int variant = 0; variant < 4; ++variant) {      /* name.dll, name.manifest, name\name.dll, name\name.manifest */
      SIZE_T o = dir_len;
      memcpy(path, dirs[d], dir_len * 2);
      if (o && path[o - 1] != '\\' && path[o - 1] != '/') path[o++] = '\\';
      if (by_language) { memcpy(path + o, want->language, lang_len * 2); o += lang_len; path[o++] = '\\'; }
      if (variant >= 2) { memcpy(path + o, want->name, name_len * 2); o += name_len; path[o++] = '\\'; }
      memcpy(path + o, want->name, name_len * 2);
      o += name_len;
      wcscpy(path + o, (variant & 1) ? L".manifest" : L".dll");
      const NTSTATUS s = act_try_candidate(b, path, want, !(variant & 1));
      if (s != SXS_ASSEMBLY_NOT_FOUND) { act_free(path); return s; }
    }
    act_free(path);
  }
  return SXS_ASSEMBLY_NOT_FOUND;
}

/* The assembly dependencies: <dependency><dependentAssembly><assemblyIdentity/></dependentAssembly></dependency>. Each is looked for next to the application (the
 * application directory, then a folder with its name) as a manifest file; one that cannot be found fails the context unless the dependency may be bound later. */
static NTSTATUS act_load_dependencies(ActBuild *b, ULONG assembly_index, const XNode *root) {
  (void)assembly_index;
  for (const XNode *dep = root->first; dep; dep = dep->next) {
    if (!xml_ns_asm(dep) || !xml_is(dep, "dependency")) continue;
    for (const XNode *da = dep->first; da; da = da->next) {
      if (!xml_ns_asm(da) || !xml_is(da, "dependentAssembly")) continue;
      const XNode *idn = NULL;
      for (const XNode *c = da->first; c; c = c->next) if (xml_ns_asm(c) && xml_is(c, "assemblyIdentity")) { if (idn) return SXS_CANT_GEN_ACTCTX; idn = c; }
      if (!idn) return SXS_CANT_GEN_ACTCTX;
      ActIdentity want;
      if (!act_parse_identity(idn, &want)) { act_identity_free(&want); return SXS_CANT_GEN_ACTCTX; }
      const WCHAR *delayed = xml_attr_value(da, "allowDelayedBinding");
      const int may_bind_later = delayed && act_eq_ascii(delayed, "true", 1);
      /* already part of the context (the same assembly named twice)? */
      int have = 0;
      for (ULONG i = 0; i < b->ctx->assemblies.count && !have; ++i) {
        const ActAssembly *a = act_vec_at(&b->ctx->assemblies, i);
        if (a->has_id && act_identity_matches(&want, &a->id) && act_ieq(want.name, a->id.name)) have = 1;
      }
      NTSTATUS status = have ? STATUS_SUCCESS : SXS_CANT_GEN_ACTCTX;
      if (!have && b->depth < 8) status = act_probe_assembly(b, &want);
      if (status && !have) status = act_load_builtin(b, &want);
      act_identity_free(&want);
      if (status && !may_bind_later) return status;
    }
  }
  return STATUS_SUCCESS;
}

/* ---- sections ------------------------------------------------------------------------------------------------------------------------------------------------ */

#define STR_HEADER_SIZE 44
#define STR_INDEX_SIZE 24
#define GUID_HEADER_SIZE 40
#define GUID_INDEX_SIZE 28

typedef struct ActEntry { const WCHAR *key; ULONG roster; ULONG assembly, item; } ActEntry;
typedef SIZE_T (*ActWriter)(ActCtx *ctx, ActBlob *b, const ActEntry *e, ULONG *data_len);

static NTSTATUS act_build_string_section(ActCtx *ctx, int kind, ActVec *entries, ActWriter writer) {
  ActBlob *b = &ctx->sections[kind];
  const ULONG n = entries->count;
  blob_add(b, NULL, STR_HEADER_SIZE + (SIZE_T)n * STR_INDEX_SIZE);
  ULONG *meta = act_alloc(((SIZE_T)n * 5 + 1) * 4);
  if (!meta) return SXS_CANT_GEN_ACTCTX;
  for (ULONG i = 0; i < n; ++i) {
    const ActEntry *e = act_vec_at(entries, i);
    SIZE_T bytes = 0;
    blob_align4(b);
    const SIZE_T name_off = blob_wstr(b, e->key, &bytes);
    blob_align4(b);
    ULONG data_len = 0;
    const SIZE_T data_off = writer(ctx, b, e, &data_len);
    meta[i * 5 + 0] = act_hash_name(e->key, bytes / 2);
    meta[i * 5 + 1] = (ULONG)name_off;
    meta[i * 5 + 2] = (ULONG)bytes;
    meta[i * 5 + 3] = (ULONG)data_off;
    meta[i * 5 + 4] = data_len;
  }
  if (!b->failed) {
    blob_put32(b, 0, 0x64487353);
    blob_put32(b, 4, STR_HEADER_SIZE);
    blob_put32(b, 20, n);
    blob_put32(b, 24, STR_HEADER_SIZE);
    for (ULONG i = 0; i < n; ++i) {
      const ActEntry *e = act_vec_at(entries, i);
      const SIZE_T at = STR_HEADER_SIZE + (SIZE_T)i * STR_INDEX_SIZE;
      for (int k = 0; k < 5; ++k) blob_put32(b, at + (SIZE_T)k * 4, meta[i * 5 + (ULONG)k]);
      blob_put32(b, at + 20, e->roster);
    }
  }
  act_free(meta);
  ctx->section_count[kind] = n;
  return b->failed ? SXS_CANT_GEN_ACTCTX : STATUS_SUCCESS;
}

static SIZE_T write_dll_redirect(ActCtx *ctx, ActBlob *b, const ActEntry *e, ULONG *len) {
  (void)ctx; (void)e;
  const ULONG rec[5] = { 20, 2 /* DLL_REDIRECT_PATH_OMITS_ASSEMBLY_ROOT */, 0, 0, 0 };
  *len = 20;
  return blob_add(b, rec, 20);
}

/* The assembly section: a record per assembly, keyed by its encoded identity (the manifest's path and the flags follow the header). */
static SIZE_T write_assembly(ActCtx *ctx, ActBlob *b, const ActEntry *e, ULONG *len) {
  const ActAssembly *a = act_vec_at(&ctx->assemblies, e->assembly);
  const SIZE_T at = blob_add(b, NULL, 20);
  SIZE_T pb = 0;
  const SIZE_T path_at = blob_wstr(b, a->manifest_path, &pb);
  blob_put32(b, at, 20);
  blob_put32(b, at + 4, a->flags);
  blob_put32(b, at + 8, (ULONG)pb);
  blob_put32(b, at + 12, (ULONG)(path_at - at));
  *len = (ULONG)(b->len - at);
  return at;
}

static SIZE_T write_wndclass(ActCtx *ctx, ActBlob *b, const ActEntry *e, ULONG *len) {
  const ActAssembly *a = act_vec_at(&ctx->assemblies, e->assembly);
  const ActWndClass *w = act_vec_at(&a->wndclasses, e->item);
  const ActFile *f = act_vec_at(&a->files, w->file);
  const SIZE_T at = blob_add(b, NULL, 24);
  WCHAR *versioned = NULL;
  if (w->versioned) {
    const SIZE_T vl = a->id.version ? wcslen(a->id.version) : 0, nl = wcslen(w->name);
    versioned = act_alloc((vl + nl + 3) * 2);
    if (versioned) { if (vl) memcpy(versioned, a->id.version, vl * 2); versioned[vl] = '!'; memcpy(versioned + vl + 1, w->name, nl * 2); }
  }
  SIZE_T name_bytes = 0, module_bytes = 0;
  const SIZE_T name_at = blob_wstr(b, versioned ? versioned : w->name, &name_bytes);
  const SIZE_T module_at = blob_wstr(b, f->name, &module_bytes);
  act_free(versioned);
  blob_put32(b, at, 24);
  blob_put32(b, at + 8, (ULONG)name_bytes);
  blob_put32(b, at + 12, (ULONG)(name_at - at));
  blob_put32(b, at + 16, (ULONG)module_bytes);
  blob_put32(b, at + 20, (ULONG)module_at);
  *len = (ULONG)(b->len - at);
  return at;
}

/* A section keyed by GUID: header, index, (the module names), then the records. An entry with dup_of set is another key for the record of an earlier one. */
typedef struct ActGEntry { ActGuid guid; ULONG roster, assembly, item; LONG dup_of; const WCHAR *name; SIZE_T name_off; } ActGEntry;
typedef SIZE_T (*ActGWriter)(ActCtx *ctx, ActBlob *b, const ActGEntry *e, ULONG *data_len);
static SIZE_T g_clr_module_off;      /* (the lower case "mscoree.dll" in the names of the section being built) */
static SIZE_T g_clr_upper_off;

static NTSTATUS act_build_guid_section(ActCtx *ctx, int kind, ActVec *entries, ActGWriter writer, int with_names, int with_clr_names) {
  ActBlob *b = &ctx->sections[kind];
  const ULONG n = entries->count;
  blob_add(b, NULL, GUID_HEADER_SIZE + (SIZE_T)n * GUID_INDEX_SIZE);
  SIZE_T names_off = 0, names_len = 0;
  if (with_names) {
    blob_align4(b);
    names_off = b->len;
    for (ULONG i = 0; i < n; ++i) {
      ActGEntry *e = act_vec_at(entries, i);
      ActGEntry *same = NULL;
      for (ULONG k = 0; k < i && !same; ++k) { ActGEntry *o = act_vec_at(entries, k); if (o->name && e->name && wcscmp(o->name, e->name) == 0) same = o; }
      if (same) e->name_off = same->name_off; else if (e->name) e->name_off = blob_wstr(b, e->name, NULL);
    }
    if (with_clr_names) { g_clr_upper_off = blob_wstr(b, L"MSCOREE.DLL", NULL); g_clr_module_off = blob_wstr(b, L"mscoree.dll", NULL); }
    names_len = b->len - names_off;
  }
  ULONG *meta = act_alloc(((SIZE_T)n * 2 + 1) * 4);
  if (!meta) return SXS_CANT_GEN_ACTCTX;
  for (ULONG i = 0; i < n; ++i) {
    const ActGEntry *e = act_vec_at(entries, i);
    if (e->dup_of >= 0) { meta[i * 2] = meta[(ULONG)e->dup_of * 2]; meta[i * 2 + 1] = meta[(ULONG)e->dup_of * 2 + 1]; continue; }
    blob_align4(b);
    ULONG len = 0;
    meta[i * 2] = (ULONG)writer(ctx, b, e, &len);
    meta[i * 2 + 1] = len;
  }
  if (!b->failed) {
    blob_put32(b, 0, 0x64487347);
    blob_put32(b, 4, GUID_HEADER_SIZE);
    blob_put32(b, 20, n);
    blob_put32(b, 24, GUID_HEADER_SIZE);
    blob_put32(b, 32, (ULONG)names_off);
    blob_put32(b, 36, (ULONG)names_len);
    for (ULONG i = 0; i < n; ++i) {
      const ActGEntry *e = act_vec_at(entries, i);
      const SIZE_T at = GUID_HEADER_SIZE + (SIZE_T)i * GUID_INDEX_SIZE;
      memcpy(b->p + at, &e->guid, 16);
      blob_put32(b, at + 16, meta[i * 2]);
      blob_put32(b, at + 20, meta[i * 2 + 1]);
      blob_put32(b, at + 24, e->roster);
    }
  }
  act_free(meta);
  ctx->section_count[kind] = n;
  return b->failed ? SXS_CANT_GEN_ACTCTX : STATUS_SUCCESS;
}

static ActGuid act_alias_guid(ULONG n) { ActGuid g = { 0x5ca11a5e, 0xa11a, 0x4a11, { 0xa1, 0x1a, 0x5c, 0x0b, 0x00, 0x00, 0x00, 0x00 } }; memcpy(&g.d4[4], &n, 4); return g; }

static const ActFile *act_entry_file(ActCtx *ctx, ULONG ai, ULONG file) {
  const ActAssembly *a = act_vec_at(&ctx->assemblies, ai);
  return file == 0xFFFFFFFFu ? NULL : act_vec_at(&a->files, file);
}

static SIZE_T write_comclass(ActCtx *ctx, ActBlob *b, const ActGEntry *e, ULONG *len) {
  const ActAssembly *a = act_vec_at(&ctx->assemblies, e->assembly);
  const ActComClass *cc = act_vec_at(&a->comclasses, e->item);
  const SIZE_T at = blob_add(b, NULL, 120);
  SIZE_T clr_len = 0;
  ULONG flags = 0;
  for (int i = 0; i < 5; ++i) if (cc->misc_mask & (1u << i)) flags |= 0x100u << i;
  /* a record's alias is the key it was given as the second */
  const ActGuid alias = act_alias_guid(e->roster * 1024 + e->item);
  blob_put32(b, at, 120);
  blob_put32(b, at + 4, flags);
  blob_put32(b, at + 8, cc->model);
  memcpy(b->p + at + 12, &cc->clsid, 16);
  memcpy(b->p + at + 28, &alias, 16);
  memcpy(b->p + at + 44, &cc->clsid, 16);
  if (cc->has_tlbid) memcpy(b->p + at + 60, &cc->tlbid, 16);
  const SIZE_T nbytes = (cc->is_clr ? 11 : wcslen(act_entry_file(ctx, e->assembly, cc->file)->name)) * 2;
  blob_put32(b, at + 76, (ULONG)nbytes);
  blob_put32(b, at + 80, (ULONG)e->name_off);
  if (cc->is_clr) {
    const SIZE_T clr = blob_add(b, NULL, 44);
    SIZE_T nb = 0, vb = 0;
    const SIZE_T name_at = blob_wstr(b, cc->clr_name, &nb);
    const SIZE_T ver_at = blob_wstr(b, cc->clr_version, &vb);
    blob_put32(b, clr, 44);
    blob_put32(b, clr + 8, 2);
    blob_put32(b, clr + 12, 22);
    blob_put32(b, clr + 16, (ULONG)g_clr_module_off);
    blob_put32(b, clr + 20, (ULONG)nb);
    blob_put32(b, clr + 24, (ULONG)(name_at - clr));
    blob_put32(b, clr + 28, (ULONG)vb);
    blob_put32(b, clr + 32, (ULONG)(ver_at - clr));
    clr_len = b->len - clr;
    blob_put32(b, at + 92, (ULONG)clr_len);
    blob_put32(b, at + 96, (ULONG)(clr - at));
  }
  if (cc->progid) {
    SIZE_T pb = 0;
    const SIZE_T pat = blob_wstr(b, cc->progid, &pb);
    blob_put32(b, at + 84, (ULONG)pb);
    blob_put32(b, at + 88, (ULONG)(pat - at));
  }
  blob_put32(b, at + 100, cc->misc[0]);
  blob_put32(b, at + 104, cc->misc[2]);
  blob_put32(b, at + 108, cc->misc[3]);
  blob_put32(b, at + 112, cc->misc[1]);
  blob_put32(b, at + 116, cc->misc[4]);
  *len = (ULONG)(b->len - at);
  return at;
}

static SIZE_T write_ifaceps(ActCtx *ctx, ActBlob *b, const ActGEntry *e, ULONG *len) {
  const ActAssembly *a = act_vec_at(&ctx->assemblies, e->assembly);
  const ActIfacePS *ip = act_vec_at(&a->ifaceps, e->item);
  const SIZE_T at = blob_add(b, NULL, 68);
  blob_put32(b, at, 68);
  blob_put32(b, at + 4, ip->mask);
  memcpy(b->p + at + 8, ip->external && ip->has_ps32 ? &ip->ps32 : &ip->iid, 16);
  blob_put32(b, at + 24, ip->num_methods);
  memcpy(b->p + at + 28, &ip->tlbid, 16);
  memcpy(b->p + at + 44, &ip->base, 16);
  SIZE_T nb = 0;
  const SIZE_T name_at = blob_wstr(b, ip->name, &nb);
  blob_put32(b, at + 60, (ULONG)nb);
  blob_put32(b, at + 64, (ULONG)(name_at - at));
  *len = (ULONG)(b->len - at);
  return at;
}

static SIZE_T write_typelib(ActCtx *ctx, ActBlob *b, const ActGEntry *e, ULONG *len) {
  const ActAssembly *a = act_vec_at(&ctx->assemblies, e->assembly);
  const ActTypelib *t = act_vec_at(&a->typelibs, e->item);
  const SIZE_T at = blob_add(b, NULL, 32);
  blob_put32(b, at, 32);
  const ActFile *f = act_entry_file(ctx, e->assembly, t->file);
  blob_put32(b, at + 8, f ? (ULONG)(wcslen(f->name) * 2) : 0);
  blob_put32(b, at + 12, (ULONG)e->name_off);
  const ULONG lf = (ULONG)t->langid | ((ULONG)t->flags << 16);
  blob_put32(b, at + 16, lf);
  SIZE_T hb = 0;
  if (t->helpdir[0]) {
    const SIZE_T help_at = blob_wstr(b, t->helpdir, &hb);
    blob_put32(b, at + 20, (ULONG)hb);
    blob_put32(b, at + 24, (ULONG)(help_at - at));
  }
  blob_put32(b, at + 28, (ULONG)t->major | ((ULONG)t->minor << 16));
  *len = (ULONG)(b->len - at);
  return at;
}

static SIZE_T write_surrogate(ActCtx *ctx, ActBlob *b, const ActGEntry *e, ULONG *len) {
  const ActAssembly *a = act_vec_at(&ctx->assemblies, e->assembly);
  const ActSurrogate *sg = act_vec_at(&a->surrogates, e->item);
  const SIZE_T at = blob_add(b, NULL, 40);
  blob_put32(b, at, 40);
  memcpy(b->p + at + 8, &sg->clsid, 16);
  SIZE_T vb = 0, nb = 0;
  const SIZE_T ver_at = blob_wstr(b, sg->version, &vb);
  const SIZE_T name_at = blob_wstr(b, sg->name, &nb);
  blob_put32(b, at + 24, (ULONG)(ver_at - at));
  blob_put32(b, at + 28, (ULONG)vb);
  blob_put32(b, at + 32, (ULONG)(name_at - at));
  blob_put32(b, at + 36, (ULONG)nb);
  *len = (ULONG)(b->len - at);
  return at;
}

/* The progid section keeps the alias GUID of each class in its global data; a record points to it. */
typedef struct ActPEntry { ActEntry e; ActGuid alias; } ActPEntry;
static SIZE_T write_progid(ActCtx *ctx, ActBlob *b, const ActEntry *e, ULONG *len) {
  (void)ctx;
  const SIZE_T at = blob_add(b, NULL, 12);
  blob_put32(b, at, 12);
  blob_put32(b, at + 8, (ULONG)e->item);      /* (the offset of the alias in the global data, worked out by the builder) */
  *len = 12;
  return at;
}

static SIZE_T write_winrt(ActCtx *ctx, ActBlob *b, const ActEntry *e, ULONG *len) {
  const ActAssembly *a = act_vec_at(&ctx->assemblies, e->assembly);
  const ActWinrt *w = act_vec_at(&a->winrts, e->item);
  const ActFile *f = act_vec_at(&a->files, w->file);
  const SIZE_T at = blob_add(b, NULL, 20);
  SIZE_T mb = 0;
  const SIZE_T mod_at = blob_wstr(b, f->name, &mb);
  blob_put32(b, at, 20);
  blob_put32(b, at + 8, (ULONG)mb);
  blob_put32(b, at + 12, (ULONG)mod_at);
  blob_put32(b, at + 16, w->model);
  *len = 20;
  return at;
}

/* A string section with global data (the winrt and progid sections): the entries' records are written after it. */
static NTSTATUS act_build_string_section_g(ActCtx *ctx, int kind, ActVec *entries, ActWriter writer, const void *global, SIZE_T global_len, int progid) {
  ActBlob *b = &ctx->sections[kind];
  const ULONG n = entries->count;
  blob_add(b, NULL, STR_HEADER_SIZE + (SIZE_T)n * STR_INDEX_SIZE);
  blob_align4(b);
  const SIZE_T global_off = blob_add(b, global, global_len);
  ULONG *meta = act_alloc(((SIZE_T)n * 5 + 1) * 4);
  if (!meta) return SXS_CANT_GEN_ACTCTX;
  for (ULONG i = 0; i < n; ++i) {
    ActEntry *e = act_vec_at(entries, i);
    SIZE_T bytes = 0;
    blob_align4(b);
    const SIZE_T name_off = blob_wstr(b, e->key, &bytes);
    blob_align4(b);
    ULONG data_len = 0;
    if (progid) e->item = (ULONG)(global_off + (SIZE_T)i * 16);
    const SIZE_T data_off = writer(ctx, b, e, &data_len);
    meta[i * 5 + 0] = act_hash_name(e->key, bytes / 2);
    meta[i * 5 + 1] = (ULONG)name_off;
    meta[i * 5 + 2] = (ULONG)bytes;
    meta[i * 5 + 3] = (ULONG)data_off;
    meta[i * 5 + 4] = data_len;
  }
  if (!b->failed) {
    blob_put32(b, 0, 0x64487353);
    blob_put32(b, 4, STR_HEADER_SIZE);
    blob_put32(b, 20, n);
    blob_put32(b, 24, STR_HEADER_SIZE);
    blob_put32(b, 36, (ULONG)global_off);
    blob_put32(b, 40, (ULONG)global_len);
    for (ULONG i = 0; i < n; ++i) {
      const ActEntry *e = act_vec_at(entries, i);
      const SIZE_T at = STR_HEADER_SIZE + (SIZE_T)i * STR_INDEX_SIZE;
      for (int k = 0; k < 5; ++k) blob_put32(b, at + (SIZE_T)k * 4, meta[i * 5 + (ULONG)k]);
      blob_put32(b, at + 20, e->roster);
    }
  }
  act_free(meta);
  ctx->section_count[kind] = n;
  return b->failed ? SXS_CANT_GEN_ACTCTX : STATUS_SUCCESS;
}

static NTSTATUS act_build_sections(ActCtx *ctx) {
  ActVec asm_info = { 0, 0, 0, sizeof(ActEntry) };
  ActVec dll = { 0, 0, 0, sizeof(ActEntry) }, wnd = { 0, 0, 0, sizeof(ActEntry) }, win = { 0, 0, 0, sizeof(ActEntry) }, prog = { 0, 0, 0, sizeof(ActEntry) };
  ActVec com = { 0, 0, 0, sizeof(ActGEntry) }, ips = { 0, 0, 0, sizeof(ActGEntry) }, tlb = { 0, 0, 0, sizeof(ActGEntry) }, sur = { 0, 0, 0, sizeof(ActGEntry) };
  ActVec aliases = { 0, 0, 0, sizeof(ActGuid) };
  NTSTATUS status = STATUS_SUCCESS;
  int have_clr = 0;
#define ADD(vec, type, ...) do { type *e_ = act_vec_add(&vec); if (!e_) { status = SXS_CANT_GEN_ACTCTX; goto out; } *e_ = (type){ __VA_ARGS__ }; } while (0)
  for (ULONG i = 0; i < ctx->assemblies.count; ++i) {
    const ActAssembly *a = act_vec_at(&ctx->assemblies, i);
    if (a->encoded) ADD(asm_info, ActEntry, a->encoded, i + 1, i, 0);
    for (ULONG k = 0; k < a->files.count; ++k) ADD(dll, ActEntry, ((ActFile *)act_vec_at(&a->files, k))->name, i + 1, i, k);
    for (ULONG k = 0; k < a->wndclasses.count; ++k) ADD(wnd, ActEntry, ((ActWndClass *)act_vec_at(&a->wndclasses, k))->name, i + 1, i, k);
    for (ULONG k = 0; k < a->winrts.count; ++k) ADD(win, ActEntry, ((ActWinrt *)act_vec_at(&a->winrts, k))->name, i + 1, i, k);
    for (ULONG k = 0; k < a->comclasses.count; ++k) {
      const ActComClass *cc = act_vec_at(&a->comclasses, k);
      const ActFile *f = act_entry_file(ctx, i, cc->file);
      const WCHAR *module = cc->is_clr ? L"MSCOREE.DLL" : f ? f->name : L"";
      have_clr |= cc->is_clr;
      const LONG first = (LONG)com.count;
      ADD(com, ActGEntry, cc->clsid, i + 1, i, k, -1, module, 0);
      ADD(com, ActGEntry, act_alias_guid((i + 1) * 1024 + k), i + 1, i, k, first, module, 0);
      const ActGuid alias = act_alias_guid((i + 1) * 1024 + k);
      if (cc->progid) { ADD(prog, ActEntry, cc->progid, i + 1, i, k); { ActGuid *ag = act_vec_add(&aliases); if (!ag) { status = SXS_CANT_GEN_ACTCTX; goto out; } *ag = alias; } }
      for (ULONG m = 0; m < cc->progids.count; ++m) {
        ADD(prog, ActEntry, *(WCHAR **)act_vec_at(&cc->progids, m), i + 1, i, k);
        { ActGuid *ag = act_vec_add(&aliases); if (!ag) { status = SXS_CANT_GEN_ACTCTX; goto out; } *ag = alias; }
      }
    }
    for (ULONG k = 0; k < a->ifaceps.count; ++k) ADD(ips, ActGEntry, ((ActIfacePS *)act_vec_at(&a->ifaceps, k))->iid, i + 1, i, k, -1, NULL, 0);
    for (ULONG k = 0; k < a->typelibs.count; ++k) {
      const ActTypelib *t = act_vec_at(&a->typelibs, k);
      const ActFile *f = act_entry_file(ctx, i, t->file);
      ADD(tlb, ActGEntry, t->tlbid, i + 1, i, k, -1, f ? f->name : L"", 0);
    }
    for (ULONG k = 0; k < a->surrogates.count; ++k) ADD(sur, ActGEntry, ((ActSurrogate *)act_vec_at(&a->surrogates, k))->clsid, i + 1, i, k, -1, NULL, 0);
  }
#undef ADD
  if (asm_info.count) status = act_build_string_section(ctx, ACT_SECTION_ASSEMBLY_INFORMATION, &asm_info, write_assembly);
  if (!status && dll.count) status = act_build_string_section(ctx, ACT_SECTION_DLL_REDIRECTION, &dll, write_dll_redirect);
  if (!status && wnd.count) status = act_build_string_section(ctx, ACT_SECTION_WINDOW_CLASS, &wnd, write_wndclass);
  if (!status && win.count) { static const BYTE zero[8]; status = act_build_string_section_g(ctx, ACT_SECTION_WINRT_CLASSES, &win, write_winrt, zero, sizeof(zero), 0); }
  if (!status && prog.count) status = act_build_string_section_g(ctx, ACT_SECTION_COM_PROGID, &prog, write_progid, aliases.data, (SIZE_T)aliases.count * 16, 1);
  if (!status && com.count) status = act_build_guid_section(ctx, ACT_SECTION_COM_SERVER, &com, write_comclass, 1, have_clr);
  if (!status && ips.count) status = act_build_guid_section(ctx, ACT_SECTION_COM_INTERFACE, &ips, write_ifaceps, 0, 0);
  if (!status && tlb.count) status = act_build_guid_section(ctx, ACT_SECTION_COM_TYPELIB, &tlb, write_typelib, 1, 0);
  if (!status && sur.count) status = act_build_guid_section(ctx, ACT_SECTION_CLR_SURROGATES, &sur, write_surrogate, 0, 0);
out:
  act_free(asm_info.data); act_free(dll.data); act_free(wnd.data); act_free(win.data); act_free(prog.data);
  act_free(com.data); act_free(ips.data); act_free(tlb.data); act_free(sur.data); act_free(aliases.data);
  return status;
}

/* ---- creating a context ------------------------------------------------------------------------------------------------------------------------------ */

/* The context of the manifest in buf (the file at path, if it is one; dir is where the assemblies it needs are looked for). */
static NTSTATUS act_create(const BYTE *buf, SIZE_T size, const WCHAR *path, const WCHAR *search_dir, const WCHAR *dir, ULONG flags, ActCtx **out) {
  *out = NULL;
  XNode *root = xml_parse(buf, size);
  if (!root) return SXS_CANT_GEN_ACTCTX;
  ActCtx *ctx = act_alloc(sizeof(ActCtx));
  if (!ctx) { xml_free(root); return SXS_CANT_GEN_ACTCTX; }
  ctx->magic = ACT_MAGIC;
  ctx->refs = 1;
  ctx->flags = flags;
  ctx->assemblies.elem = sizeof(ActAssembly);
  ctx->settings.elem = sizeof(ActSetting);
  ctx->compat.elem = sizeof(ActCompat);
  ctx->root_manifest = act_wstrdup(path);
  ctx->app_dir = act_wstrdup(dir);
  ActBuild b = { ctx, search_dir, dir, 0 };
  NTSTATUS status = act_add_manifest(&b, root, path, 1, NULL);
  xml_free(root);
  if (!status) status = act_build_sections(ctx);
  if (status) { act_ctx_destroy(ctx); return status; }
  *out = ctx;
  return STATUS_SUCCESS;
}

/* One call for all: kernel32 has read the manifest. */
EXPORT NTSTATUS NTAPI TcosCreateActivationContext(const BYTE *buf, SIZE_T size, PCWSTR path, PCWSTR search_dir, PCWSTR dir, ULONG flags, PVOID *handle) {
  if (!buf || !handle) return STATUS_INVALID_PARAMETER_;
  ActCtx *ctx;
  const NTSTATUS s = act_create(buf, size, path, search_dir, dir, flags, &ctx);
  *handle = ctx;
  return s;
}


/* The manifest of a PE image: the RT_MANIFEST resource with the smallest numeric id (names are not looked at), or only the id `only_id` (when it is not 0); of the
 * languages that resource has, the first of neutral, the system language, its primary language, American English and language 1, else the first there is. The image is a
 * mapped one (relative addresses are addresses) or the file as it is (they are turned into file offsets through the section table). */
static const BYTE *act_resource_address(const BYTE *base, SIZE_T size, int flat, ULONG rva) {
  if (!flat) return base + rva;
  const ULONG pe = rd32(base + 0x3C);
  if (pe > size || size - pe < 24) return NULL;
  const ULONG sections = *(const USHORT *)(base + pe + 6), optional = *(const USHORT *)(base + pe + 20);
  const SIZE_T table = (SIZE_T)pe + 24 + optional;
  if (sections > 96 || table > size || size - table < (SIZE_T)sections * 40) return NULL;
  for (ULONG i = 0; i < sections; ++i) {
    const BYTE *h = base + table + (SIZE_T)i * 40;
    const ULONG vsize = rd32(h + 8), va = rd32(h + 12), raw = rd32(h + 16), ptr = rd32(h + 20);
    const ULONG span = vsize > raw ? vsize : raw;
    if (rva >= va && rva - va < span && rva - va < raw) {
      const SIZE_T at = (SIZE_T)ptr + (rva - va);
      return at < size ? base + at : NULL;
    }
  }
  return NULL;
}

static const BYTE *act_image_manifest(const BYTE *base, SIZE_T size, int flat, ULONG only_id, SIZE_T *out_size) {
  if (!flat) size = ACT_MAX_SIZE;
  if (size < 0x100 || base[0] != 'M' || base[1] != 'Z') return NULL;
  const ULONG pe = rd32(base + 0x3C);
  if (pe > size || size - pe < 0x90 || memcmp(base + pe, "PE\0\0", 4) != 0) return NULL;
  const WORD magic = *(const WORD *)(base + pe + 24);
  const SIZE_T dirs = (SIZE_T)pe + 24 + (magic == 0x20b ? 112 : 96);
  if (dirs + 24 > size) return NULL;
  const ULONG rva = rd32(base + dirs + 16);
  if (!rva) return NULL;
  const BYTE *root = act_resource_address(base, size, flat, rva);
  if (!root) return NULL;
  const SIZE_T room = flat ? (size - (SIZE_T)(root - base)) : ACT_MAX_SIZE;
  #define DIR_OK(d) ((SIZE_T)((d) - root) + 16 <= room)
  #define ENTRIES(d) (((ULONG)*(const USHORT *)((d) + 12)) + *(const USHORT *)((d) + 14))
  if (!DIR_OK(root)) return NULL;
  /* the type RT_MANIFEST (24) */
  const BYTE *dir = NULL;
  for (ULONG i = 0, n = ENTRIES(root); i < n; ++i) {
    const BYTE *e = root + 16 + (SIZE_T)i * 8;
    if ((SIZE_T)(e - root) + 8 > room) return NULL;
    if (!(rd32(e) & 0x80000000u) && rd32(e) == 24 && (rd32(e + 4) & 0x80000000u)) { dir = root + (rd32(e + 4) & 0x7FFFFFFF); break; }
  }
  if (!dir || !DIR_OK(dir)) return NULL;
  /* the smallest numeric id (or the one asked for) */
  const BYTE *lang_dir = NULL;
  ULONG best = 0xFFFFFFFFu;
  for (ULONG i = 0, n = ENTRIES(dir); i < n; ++i) {
    const BYTE *e = dir + 16 + (SIZE_T)i * 8;
    if ((SIZE_T)(e - root) + 8 > room) return NULL;
    const ULONG id = rd32(e);
    if ((id & 0x80000000u) || !(rd32(e + 4) & 0x80000000u) || id >= best || (only_id && id != only_id)) continue;
    best = id;
    lang_dir = root + (rd32(e + 4) & 0x7FFFFFFF);
  }
  if (!lang_dir || !DIR_OK(lang_dir)) return NULL;
  static const ULONG precedence[] = { 0, 0x409, 0x09, 0x01 };
  const BYTE *leaf = NULL;
  const ULONG n = ENTRIES(lang_dir);
  for (ULONG k = 0; k < sizeof(precedence) / sizeof(precedence[0]) && !leaf; ++k)
    for (ULONG i = 0; i < n && !leaf; ++i) {
      const BYTE *e = lang_dir + 16 + (SIZE_T)i * 8;
      if ((SIZE_T)(e - root) + 8 > room) return NULL;
      if (!(rd32(e) & 0x80000000u) && rd32(e) == precedence[k]) leaf = e;
    }
  if (!leaf && n) leaf = lang_dir + 16;
  if (!leaf || (SIZE_T)(leaf - root) + 8 > room || (rd32(leaf + 4) & 0x80000000u)) return NULL;
  const BYTE *data_entry = root + rd32(leaf + 4);
  if ((SIZE_T)(data_entry - root) + 16 > room) return NULL;
  const ULONG data_size = rd32(data_entry + 4);
  const BYTE *data = act_resource_address(base, size, flat, rd32(data_entry));
  if (!data || (flat && (SIZE_T)(data - base) > size - 0 - 0) || (flat && data_size > size - (SIZE_T)(data - base))) return NULL;
  #undef DIR_OK
  #undef ENTRIES
  *out_size = data_size;
  return data;
}

static void act_init_process(void) {
  PEB *peb = NtCurrentPeb();
  const BYTE *params = *(const BYTE **)((const BYTE *)peb + 0x20);
  if (!params || g_process_ctx) return;
  const UNICODE_STRING *image = (const UNICODE_STRING *)(params + 0x60);
  if (!image->Buffer || image->Length < 2 || image->Length > 2 * 520) return;
  WCHAR path[540], dir[540];
  const SIZE_T n = image->Length / 2;
  memcpy(path, image->Buffer, n * 2);
  path[n] = 0;
  SIZE_T dl = n;
  while (dl && path[dl - 1] != '\\' && path[dl - 1] != '/') --dl;
  memcpy(dir, path, dl * 2);
  dir[dl] = 0;
  wcscat(path, L".manifest");
  SIZE_T size = 0;
  BYTE *data = act_read_file(path, &size);
  ActCtx *ctx = NULL;
  if (data) {
    act_create(data, size, path, dir, dir, 0, &ctx);
    act_free(data);
  } else {
    const BYTE *res = act_image_manifest((const BYTE *)peb->ImageBaseAddress, 0, 0, 1, &size);
    path[n] = 0;
    if (res) act_create(res, size, path, dir, dir, 0, &ctx);
  }
  g_process_ctx = ctx;
}

/* ---- the stack ---------------------------------------------------------------------------------------------------------------------------------------------- */

static int act_valid(const ActCtx *c) { return c && (ULONG_PTR)c != (ULONG_PTR)-1 && c->magic == ACT_MAGIC; }
EXPORT void NTAPI RtlAddRefActivationContext(PVOID h) { ActCtx *c = h; if (act_valid(c)) __sync_fetch_and_add(&c->refs, 1); }
EXPORT void NTAPI RtlReleaseActivationContext(PVOID h) {
  ActCtx *c = h;
  if (!act_valid(c)) return;
  if (__sync_sub_and_fetch(&c->refs, 1) == 0) act_ctx_destroy(c);
}
EXPORT NTSTATUS NTAPI RtlZombifyActivationContext(PVOID h) { return act_valid(h) ? STATUS_SUCCESS : STATUS_INVALID_PARAMETER_; }

EXPORT NTSTATUS NTAPI RtlAllocateActivationContextStack(PVOID *out) {
  ActStack *s = act_alloc(sizeof(ActStack));
  if (!s) return STATUS_NO_MEMORY;
  s->cache.Flink = s->cache.Blink = &s->cache;
  *out = s;
  return STATUS_SUCCESS;
}

static ActStack *act_thread_stack(TEB *teb, int create) {
  ActStack *s = TEB_ACTIVATION_STACK(teb);
  if (!s && create) { PVOID p = NULL; if (NT_SUCCESS(RtlAllocateActivationContextStack(&p))) { s = p; TEB_ACTIVATION_STACK(teb) = s; } }
  return s;
}

/* Frames that were made on the heap go with their context. */
static void act_pop_frame(ActStack *s) {
  ActFrame *f = s->active;
  s->active = f->previous;
  RtlReleaseActivationContext(f->context);
  if (f->flags & ACT_FRAME_HEAP_ALLOCATED) act_free(f);
}

EXPORT void NTAPI RtlFreeActivationContextStack(PVOID stack) {
  ActStack *s = stack;
  if (!s) return;
  while (s->active) act_pop_frame(s);
  act_free(s);
}
EXPORT void NTAPI RtlFreeThreadActivationContextStack(void) {
  TEB *teb = NtCurrentTeb();
  ActStack *s = TEB_ACTIVATION_STACK(teb);
  if (!s) return;
  TEB_ACTIVATION_STACK(teb) = NULL;
  RtlFreeActivationContextStack(s);
}

EXPORT NTSTATUS NTAPI RtlActivateActivationContextEx(ULONG flags, TEB *teb, PVOID h, ULONG_PTR *cookie) {
  (void)flags;
  ActStack *s = act_thread_stack(teb, 1);
  if (!s) return STATUS_NO_MEMORY;
  ActFrame *f = act_alloc(sizeof(ActFrame));
  if (!f) return STATUS_NO_MEMORY;
  f->previous = s->active;
  f->context = h;
  f->flags = ACT_FRAME_HEAP_ALLOCATED;
  RtlAddRefActivationContext(h);
  s->active = f;
  *cookie = (ULONG_PTR)f;
  return STATUS_SUCCESS;
}
EXPORT NTSTATUS NTAPI RtlActivateActivationContext(ULONG flags, PVOID h, ULONG_PTR *cookie) { return RtlActivateActivationContextEx(flags, NtCurrentTeb(), h, cookie); }

EXPORT ActFrame *NTAPI RtlActivateActivationContextUnsafeFast(ActFrameExtended *ext, PVOID h) {
  ActStack *s = act_thread_stack(NtCurrentTeb(), 1);
  if (!s) return NULL;
  ActFrame *f = &ext->frame;
  f->previous = s->active;
  f->context = h;
  f->flags = 0;
  RtlAddRefActivationContext(h);
  s->active = f;
  return f;
}
EXPORT ActFrame *NTAPI RtlDeactivateActivationContextUnsafeFast(ActFrameExtended *ext) {
  ActStack *s = act_thread_stack(NtCurrentTeb(), 0);
  if (!s || s->active != &ext->frame) return NULL;
  act_pop_frame(s);
  return s->active;
}

/* The cookie is the address of a frame on this thread's stack; the frames above it go too. */
EXPORT void NTAPI RtlDeactivateActivationContext(ULONG flags, ULONG_PTR cookie) {
  (void)flags;
  ActStack *s = act_thread_stack(NtCurrentTeb(), 0);
  ActFrame *f = s ? s->active : NULL;
  while (f && (ULONG_PTR)f != cookie) f = f->previous;
  if (!f) {
    EXCEPTION_RECORD rec = { 0 };
    rec.ExceptionCode = SXS_INVALID_DEACTIVATION;
    rec.ExceptionFlags = 1;
    RtlRaiseException(&rec);
    return;
  }
  while (s->active != f) act_pop_frame(s);
  act_pop_frame(s);
}

EXPORT NTSTATUS NTAPI RtlGetActiveActivationContext(PVOID *out) {
  ActStack *s = act_thread_stack(NtCurrentTeb(), 0);
  ActCtx *c = s && s->active ? s->active->context : NULL;
  if (c) RtlAddRefActivationContext(c);
  *out = c;
  return STATUS_SUCCESS;
}
EXPORT BOOLEAN NTAPI RtlIsActivationContextActive(PVOID h) {
  ActStack *s = act_thread_stack(NtCurrentTeb(), 0);
  for (ActFrame *f = s ? s->active : NULL; f; f = f->previous) if (f->context == h) return TRUE;
  return FALSE;
}

/* ---- lookups ------------------------------------------------------------------------------------------------------------------------------------------------- */

typedef struct ActKeyedData {
  ULONG cbSize, ulDataFormatVersion;
  PVOID lpData;
  ULONG ulLength;
  PVOID lpSectionGlobalData;
  ULONG ulSectionGlobalDataLength;
  PVOID lpSectionBase;
  ULONG ulSectionTotalLength;
  PVOID hActCtx;
  ULONG ulAssemblyRosterIndex;
  ULONG ulFlags;
  struct { PVOID lpInformation, lpSectionBase; ULONG ulSectionLength; PVOID lpSectionGlobalDataBase; ULONG ulSectionGlobalDataLength; } AssemblyMetadata;
} ActKeyedData;
#define ACT_KEYED_MIN 0x48      /* (through ulAssemblyRosterIndex) */

static ActCtx *act_lookup_context(void) {
  ActStack *s = act_thread_stack(NtCurrentTeb(), 0);
  return s && s->active ? s->active->context : g_process_ctx;
}

static NTSTATUS act_fill_keyed(ActCtx *ctx, ActKeyedData *d, ULONG flags, int kind, ULONG data_off, ULONG data_len, ULONG roster, ULONG global_off, ULONG global_len) {
  const ActBlob *b = &ctx->sections[kind];
  if (!d) return STATUS_SUCCESS;
  d->ulDataFormatVersion = 1;
  d->lpData = b->p + data_off;
  d->ulLength = data_len;
  d->lpSectionGlobalData = global_len ? b->p + global_off : NULL;
  d->ulSectionGlobalDataLength = global_len;
  d->lpSectionBase = b->p;
  d->ulSectionTotalLength = (ULONG)b->len;
  d->hActCtx = NULL;
  if (flags & 1 /* FIND_ACTCTX_SECTION_KEY_RETURN_HACTCTX */) { d->hActCtx = ctx; RtlAddRefActivationContext(ctx); }
  d->ulAssemblyRosterIndex = roster;
  return STATUS_SUCCESS;
}


EXPORT NTSTATUS NTAPI RtlFindActivationContextSectionString(ULONG flags, const ActGuid *ext, ULONG kind, PUNICODE_STRING name, PVOID pdata) {
  ActKeyedData *d = pdata;
  if (ext || (flags & ~7u) || !name || !name->Buffer || (d && d->cbSize < ACT_KEYED_MIN)) return STATUS_INVALID_PARAMETER_;
  ActCtx *ctx = act_lookup_context();
  if (!ctx || kind >= ACT_SECTION_COUNT || !ctx->sections[kind].len) return SXS_SECTION_NOT_FOUND;
  const ActBlob *b = &ctx->sections[kind];
  const ULONG count = rd32(b->p + 20), index = rd32(b->p + 24);
  const ULONG hash = act_hash_name(name->Buffer, name->Length / 2);
  for (ULONG i = 0; i < count; ++i) {
    const BYTE *e = b->p + index + (SIZE_T)i * STR_INDEX_SIZE;
    if (rd32(e) != hash || rd32(e + 8) != name->Length) continue;
    if (!act_ieq_n((const WCHAR *)(b->p + rd32(e + 4)), rd32(e + 8) / 2, name->Buffer, name->Length / 2)) continue;
    return act_fill_keyed(ctx, d, flags, (int)kind, rd32(e + 12), rd32(e + 16), rd32(e + 20), rd32(b->p + 36), rd32(b->p + 40));
  }
  return SXS_KEY_NOT_FOUND;
}

EXPORT NTSTATUS NTAPI RtlFindActivationContextSectionGuid(ULONG flags, const ActGuid *ext, ULONG kind, const ActGuid *guid, PVOID pdata) {
  ActKeyedData *d = pdata;
  if (ext || (flags & ~7u) || !guid || (d && d->cbSize < ACT_KEYED_MIN)) return STATUS_INVALID_PARAMETER_;
  ActCtx *ctx = act_lookup_context();
  if (!ctx || kind >= ACT_SECTION_COUNT || !ctx->sections[kind].len) return SXS_SECTION_NOT_FOUND;
  const ActBlob *b = &ctx->sections[kind];
  const ULONG count = rd32(b->p + 20), index = rd32(b->p + 24);
  for (ULONG i = 0; i < count; ++i) {
    const BYTE *e = b->p + index + (SIZE_T)i * GUID_INDEX_SIZE;
    if (memcmp(e, guid, 16) != 0) continue;
    return act_fill_keyed(ctx, d, flags, (int)kind, rd32(e + 16), rd32(e + 20), rd32(e + 24), rd32(b->p + 32), rd32(b->p + 36));
  }
  return SXS_KEY_NOT_FOUND;
}

/* ---- queries --------------------------------------------------------------------------------------------------------------------------------------------------- */

typedef struct ActBasicInfo { PVOID context; ULONG flags; } ActBasicInfo;
typedef struct ActDetailedInfo {
  ULONG flags, format_version, assembly_count, manifest_type, manifest_chars, config_type, config_chars, app_dir_type, app_dir_chars;
  PWSTR manifest, config, app_dir;
} ActDetailedInfo;
typedef struct ActAssemblyInfo {
  ULONG flags, identity_bytes, manifest_type, manifest_bytes;
  LONGLONG manifest_time;
  ULONG policy_type, policy_bytes;
  LONGLONG policy_time;
  ULONG satellite_roster, manifest_major, manifest_minor, policy_major, policy_minor, dir_name_bytes;
  PWSTR identity, manifest, policy, dir_name;
  ULONG file_count;
} ActAssemblyInfo;
typedef struct ActFileInfo { ULONG flags, name_bytes, path_bytes; PWSTR name, path; } ActFileInfo;

#define QUERY_USE_ACTIVE 4
#define QUERY_NO_ADDREF 0x80000000u

EXPORT NTSTATUS NTAPI RtlQueryInformationActivationContext(ULONG flags, PVOID handle, PVOID subinstance, ULONG class, PVOID buffer, SIZE_T length, SIZE_T *retlen) {
  ActCtx *ctx = handle;
  if (retlen) *retlen = 0;
  if (flags & QUERY_USE_ACTIVE) {
    if (handle) return STATUS_INVALID_PARAMETER_;
    ctx = act_lookup_context();
  } else if (flags & 0x18) return STATUS_NOT_IMPLEMENTED;
  else if (ctx && !act_valid(ctx)) return STATUS_INVALID_PARAMETER_;
  else if (!ctx) ctx = g_process_ctx;      /* (no handle: the process default) */
  if (!buffer && length) return STATUS_INVALID_PARAMETER_;
  switch (class) {
    case 1: {      /* ActivationContextBasicInformation */
      if (length < sizeof(ActBasicInfo)) return STATUS_BUFFER_TOO_SMALL_;
      ActBasicInfo *i = buffer;
      i->context = ctx == g_process_ctx ? NULL : ctx;      /* (the process default has no handle) */
      i->flags = 0;
      if (i->context && !(flags & QUERY_NO_ADDREF)) RtlAddRefActivationContext(ctx);
      if (retlen) *retlen = sizeof(*i);
      return STATUS_SUCCESS;
    }
    case 2: {      /* ActivationContextDetailedInformation */
      const SIZE_T mp = ctx && ctx->root_manifest ? (wcslen(ctx->root_manifest) + 1) * 2 : 0, ap = ctx && ctx->app_dir ? (wcslen(ctx->app_dir) + 1) * 2 : 0;
      const SIZE_T need = sizeof(ActDetailedInfo) + mp + ap;
      if (retlen) *retlen = need;
      if (length < need) return STATUS_BUFFER_TOO_SMALL_;
      ActDetailedInfo *i = buffer;
      memset(i, 0, sizeof(*i));
      if (ctx) {
        i->format_version = 1;
        i->config_type = 1;
        i->assembly_count = ctx->assemblies.count;
        BYTE *o = (BYTE *)(i + 1);
        if (mp) { i->manifest_type = 2; i->manifest_chars = (ULONG)(mp / 2 - 1); i->manifest = (PWSTR)o; memcpy(o, ctx->root_manifest, mp); o += mp; }
        if (ap) { i->app_dir_type = 2; i->app_dir_chars = (ULONG)(ap / 2 - 1); i->app_dir = (PWSTR)o; memcpy(o, ctx->app_dir, ap); }
      }
      return STATUS_SUCCESS;
    }
    case 3: {      /* AssemblyDetailedInformationInActivationContext */
      if (!ctx || !subinstance) return STATUS_INVALID_PARAMETER_;
      const ULONG id = *(const ULONG *)subinstance;
      const ActAssembly *a = id ? act_vec_at(&ctx->assemblies, id - 1) : NULL;
      if (!a) return STATUS_INVALID_PARAMETER_;
      const SIZE_T ib = a->encoded ? (wcslen(a->encoded) + 1) * 2 : 0, mb = a->manifest_path ? (wcslen(a->manifest_path) + 1) * 2 : 0, db = a->dir_name ? (wcslen(a->dir_name) + 1) * 2 : 0;
      const SIZE_T need = sizeof(ActAssemblyInfo) + ib + mb + db;
      if (retlen) *retlen = need;
      if (length < need) return STATUS_BUFFER_TOO_SMALL_;
      ActAssemblyInfo *i = buffer;
      memset(i, 0, sizeof(*i));
      i->flags = a->flags;
      i->manifest_type = mb ? 2 : 0;
      i->policy_type = 1;
      i->manifest_major = a->manifest_major;
      i->manifest_minor = a->manifest_minor;
      i->file_count = a->files.count;
      BYTE *o = (BYTE *)(i + 1);
      if (ib) { i->identity_bytes = (ULONG)(ib - 2); i->identity = (PWSTR)o; memcpy(o, a->encoded, ib); o += ib; }
      if (mb) { i->manifest_bytes = (ULONG)(mb - 2); i->manifest = (PWSTR)o; memcpy(o, a->manifest_path, mb); o += mb; }
      if (db) { i->dir_name_bytes = (ULONG)(db - 2); i->dir_name = (PWSTR)o; memcpy(o, a->dir_name, db); }
      return STATUS_SUCCESS;
    }
    case 4: {      /* FileInformationInAssemblyOfAssemblyInActivationContext */
      if (!ctx || !subinstance) return STATUS_INVALID_PARAMETER_;
      const ULONG *q = subinstance;
      const ActAssembly *a = act_vec_at(&ctx->assemblies, q[0]);
      const ActFile *f = a ? act_vec_at(&a->files, q[1]) : NULL;
      if (!f) return STATUS_INVALID_PARAMETER_;
      const SIZE_T nb = (wcslen(f->name) + 1) * 2, need = sizeof(ActFileInfo) + nb;
      if (length < need) { if (retlen) *retlen = need; return STATUS_BUFFER_TOO_SMALL_; }
      ActFileInfo *i = buffer;
      memset(i, 0, sizeof(*i));
      i->flags = 2;
      i->name_bytes = (ULONG)(nb - 2);
      i->name = (PWSTR)(i + 1);
      memcpy(i->name, f->name, nb);
      return STATUS_SUCCESS;
    }
    case 5: {      /* RunlevelInformationInActivationContext */
      if (length < 12) return STATUS_BUFFER_TOO_SMALL_;
      ULONG *i = buffer;
      i[0] = 0;
      i[1] = ctx ? ctx->run_level : 0;
      i[2] = ctx ? ctx->ui_access : 0;
      if (retlen) *retlen = 12;
      return STATUS_SUCCESS;
    }
    case 6: {      /* CompatibilityInformationInActivationContext: a count, then 32-byte elements (id, type, the highest version tested) */
      const ULONG n = ctx ? ctx->compat.count : 0;
      const SIZE_T need = 8 + (SIZE_T)n * 32;
      if (retlen) *retlen = need;
      if (length < need) return STATUS_BUFFER_TOO_SMALL_;
      BYTE *o = buffer;
      memset(o, 0, need);
      memcpy(o, &n, 4);
      for (ULONG k = 0; k < n; ++k) {
        const ActCompat *c = act_vec_at(&ctx->compat, k);
        BYTE *e = o + 8 + (SIZE_T)k * 32;
        memcpy(e, &c->id, 16);
        memcpy(e + 16, &c->type, 4);
        memcpy(e + 24, &c->max_version, 8);
      }
      return STATUS_SUCCESS;
    }
    default: return STATUS_INVALID_PARAMETER_;
  }
}

EXPORT NTSTATUS NTAPI RtlQueryInformationActiveActivationContext(ULONG class, PVOID buffer, SIZE_T length, SIZE_T *retlen) {
  return RtlQueryInformationActivationContext(QUERY_USE_ACTIVE, NULL, NULL, class, buffer, length, retlen);
}

/* A setting of the application: the namespace is one of the known settings namespaces (none means the 2005 one). The value is copied with its end mark; there may be
 * room for the characters only - Windows writes the end mark anyway, and so do we. */
EXPORT NTSTATUS NTAPI RtlQueryActivationContextApplicationSettings(ULONG flags, PVOID handle, PCWSTR ns, PCWSTR name, PWSTR buffer, SIZE_T chars, SIZE_T *written) {
  static const char *const known[] = { "http://schemas.microsoft.com/SMI/2005/WindowsSettings", "http://schemas.microsoft.com/SMI/2011/WindowsSettings",
                                       "http://schemas.microsoft.com/SMI/2016/WindowsSettings", "http://schemas.microsoft.com/SMI/2017/WindowsSettings",
                                       "http://schemas.microsoft.com/SMI/2019/WindowsSettings", "http://schemas.microsoft.com/SMI/2020/WindowsSettings" };
  if (flags || !name) return STATUS_INVALID_PARAMETER_;
  if (ns) {
    int ok = 0;
    for (ULONG i = 0; i < sizeof(known) / sizeof(known[0]); ++i) ok |= act_eq_ascii(ns, known[i], 0);
    if (!ok) return STATUS_INVALID_PARAMETER_;
  }
  ActCtx *ctx = handle;
  if (ctx && !act_valid(ctx)) return STATUS_INVALID_PARAMETER_;
  if (!ctx) ctx = act_lookup_context();
  if (!ctx) return SXS_KEY_NOT_FOUND;
  for (ULONG i = 0; i < ctx->settings.count; ++i) {
    const ActSetting *st = act_vec_at(&ctx->settings, i);
    if (!(ns ? wcscmp(st->ns, ns) == 0 : act_eq_ascii(st->ns, known[0], 0)) || wcscmp(st->name, name) != 0) continue;
    const SIZE_T len = wcslen(st->value);
    if (written) *written = len + 1;
    if (!buffer || chars < len) return STATUS_BUFFER_TOO_SMALL_;
    memcpy(buffer, st->value, (len + 1) * 2);
    return STATUS_SUCCESS;
  }
  return SXS_KEY_NOT_FOUND;
}
