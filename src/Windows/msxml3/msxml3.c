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

/* msxml3.dll for TheoryCraftOS: the XML document object model of MSXML, as far as a program needs it to read a document.
 *
 * One class, served under every DOMDocument class identifier of MSXML 2.6 to 6.0 (kClasses below; ole32 finds this library for them, and msxml6.dll
 * forwards its DllGetClassObject here): an IXMLDOMDocument3 that parses a string (loadXML) or a local file (load) with the parser of parser.inc and
 * hands out the tree - elements, attributes, text, CDATA sections, comments, processing instructions, node lists, attribute maps, the parse error.
 * Nothing external is ever read (no DTD, no entities, no schemas, no URLs), whatever resolveExternals says.
 *
 * A tree is built once and never changes: the methods that would change one (createElement, appendChild, setAttribute, save, put_text ...) answer
 * E_NOTIMPL, as do the four methods of IDispatch (there is no type library), XSLT and validation. Loading again makes a new tree; the nodes a program
 * still holds keep the old one alive. Every node is its own COM object, allocated in the tree's arena; a reference to a node is a reference to its
 * tree and to the document. selectNodes and selectSingleNode understand simple paths only: steps of name, *, ., .., @name, @*, text() and node(),
 * joined by / and //, with one [@name] or [@name='value'] test per step; anything else is E_NOTIMPL. Names in paths and in getElementsByTagName are
 * compared as written (with their prefix).
 *
 * Not safe against a load on one thread while another reads the same document (the free-threaded classes are the same object). Built freestanding
 * over kernel32.dll and oleaut32.dll (the strings are BSTRs). */
#define COBJMACROS
#include <windows.h>
#include <ole2.h>
#include <msxml2.h>

#define EXPORT __declspec(dllexport)

/* (the compiler may call these for a structure it copies or clears) */
void *memcpy(void *to, const void *from, size_t n) {
  BYTE *t = (BYTE *)to;
  const BYTE *f = (const BYTE *)from;
  for (size_t i = 0; i < n; ++i) t[i] = f[i];
  return to;
}
void *memset(void *to, int value, size_t n) {
  volatile BYTE *t = (volatile BYTE *)to;
  for (size_t i = 0; i < n; ++i) t[i] = (BYTE)value;
  return to;
}
static void zero(void *p, SIZE_T n) { memset(p, 0, n); }
static void *alloc(SIZE_T n) { return HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, n); }
static void release_memory(void *p) { if (p) HeapFree(GetProcessHeap(), 0, p); }

#include "parser.inc"

/* (the identifiers: this library is not linked with uuid) */
#define DOM_IID(low) {0x2933BF00 | (low), 0x7B36, 0x11D2, {0xB2, 0x0E, 0x00, 0xC0, 0x4F, 0x98, 0x3E, 0x60}}
static const GUID kIidUnknown = {0x00000000, 0x0000, 0x0000, {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
static const GUID kIidClassFactory = {0x00000001, 0x0000, 0x0000, {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
static const GUID kIidDispatch = {0x00020400, 0x0000, 0x0000, {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
static const GUID kIidNode = DOM_IID(0x80), kIidDocument = DOM_IID(0x81), kIidNodeList = DOM_IID(0x82), kIidNamedNodeMap = DOM_IID(0x83);
static const GUID kIidCharacterData = DOM_IID(0x84), kIidAttribute = DOM_IID(0x85), kIidElement = DOM_IID(0x86), kIidText = DOM_IID(0x87);
static const GUID kIidComment = DOM_IID(0x88), kIidInstruction = DOM_IID(0x89), kIidCdata = DOM_IID(0x8A), kIidDocument2 = DOM_IID(0x95), kIidDocument3 = DOM_IID(0x96);
static const GUID kIidParseError = {0x3EFAA426, 0x272F, 0x11D2, {0x83, 0x6F, 0x00, 0x00, 0xF8, 0x7A, 0x77, 0x82}};
/* The classes: DOMDocument and FreeThreadedDOMDocument of each version, and the version-independent ones of MSXML 2 and 3. */
static const GUID kClasses[] = {
  {0x2933BF90, 0x7B36, 0x11D2, {0xB2, 0x0E, 0x00, 0xC0, 0x4F, 0x98, 0x3E, 0x60}},      /* DOMDocument */
  {0x2933BF91, 0x7B36, 0x11D2, {0xB2, 0x0E, 0x00, 0xC0, 0x4F, 0x98, 0x3E, 0x60}},      /* FreeThreadedDOMDocument */
  {0xF6D90F11, 0x9C73, 0x11D3, {0xB3, 0x2E, 0x00, 0xC0, 0x4F, 0x99, 0x0B, 0xB4}},      /* DOMDocument (MSXML 2) */
  {0xF6D90F12, 0x9C73, 0x11D3, {0xB3, 0x2E, 0x00, 0xC0, 0x4F, 0x99, 0x0B, 0xB4}},      /* FreeThreadedDOMDocument (MSXML 2) */
  {0xF5078F1B, 0xC551, 0x11D3, {0x89, 0xB9, 0x00, 0x00, 0xF8, 0x1F, 0xE2, 0x21}},      /* DOMDocument26 */
  {0xF5078F1C, 0xC551, 0x11D3, {0x89, 0xB9, 0x00, 0x00, 0xF8, 0x1F, 0xE2, 0x21}},      /* FreeThreadedDOMDocument26 */
  {0xF5078F32, 0xC551, 0x11D3, {0x89, 0xB9, 0x00, 0x00, 0xF8, 0x1F, 0xE2, 0x21}},      /* DOMDocument30 */
  {0xF5078F33, 0xC551, 0x11D3, {0x89, 0xB9, 0x00, 0x00, 0xF8, 0x1F, 0xE2, 0x21}},      /* FreeThreadedDOMDocument30 */
  {0x88D969C0, 0xF192, 0x11D4, {0xA6, 0x5F, 0x00, 0x40, 0x96, 0x32, 0x51, 0xE5}},      /* DOMDocument40 */
  {0x88D969C1, 0xF192, 0x11D4, {0xA6, 0x5F, 0x00, 0x40, 0x96, 0x32, 0x51, 0xE5}},      /* FreeThreadedDOMDocument40 */
  {0x88D96A05, 0xF192, 0x11D4, {0xA6, 0x5F, 0x00, 0x40, 0x96, 0x32, 0x51, 0xE5}},      /* DOMDocument60 */
  {0x88D96A06, 0xF192, 0x11D4, {0xA6, 0x5F, 0x00, 0x40, 0x96, 0x32, 0x51, 0xE5}},      /* FreeThreadedDOMDocument60 */
};

static BOOL same_guid(const GUID *a, const GUID *b) {
  const BYTE *x = (const BYTE *)a, *y = (const BYTE *)b;
  for (SIZE_T i = 0; i < sizeof(GUID); ++i) if (x[i] != y[i]) return FALSE;
  return TRUE;
}

static LONG g_objects;      /* documents, lists, maps and parse errors alive, and the locks on the class factory */

/* ---- the document object and the references ------------------------------------------------------------------------------------------------------ */

typedef struct Document {
  const IXMLDOMDocument3Vtbl *vtbl;
  LONG references;                  /* the program's, and one for each reference to a node, list or map of any of its trees */
  Tree *tree;                       /* what was loaded last (an empty tree before that, and after a failure) */
  VARIANT_BOOL async, validate, resolve, preserve;
  DWORD error_code;                 /* the last load: 0, or why it failed */
  const WCHAR *error_reason;
  LONG error_line, error_column, error_position;
  WCHAR error_source[200];          /* the line with the error (its start) */
  UINT error_source_length;
  WCHAR *url;                       /* the file loaded last, or NULL */
} Document;

static const IXMLDOMDocument3Vtbl document_vtbl;

static void tree_release(Tree *t) { if (InterlockedDecrement(&t->references) == 0) tree_free(t); }
static ULONG document_addref(Document *d) { return (ULONG)InterlockedIncrement(&d->references); }
static ULONG document_release(Document *d) {
  const LONG left = InterlockedDecrement(&d->references);
  if (left == 0) {
    tree_release(d->tree);
    release_memory(d->url);
    release_memory(d);
    InterlockedDecrement(&g_objects);
  }
  return (ULONG)left;
}
/* A reference to a node keeps its tree and the document. */
static ULONG node_addref(Node *n) {
  document_addref(n->tree->document);
  return (ULONG)InterlockedIncrement(&n->tree->references);
}
static ULONG node_release(Node *n) {
  Tree *t = n->tree;
  Document *d = t->document;
  const LONG left = InterlockedDecrement(&t->references);
  if (left == 0) tree_free(t);      /* (a tree the document has replaced) */
  document_release(d);
  return (ULONG)left;
}

/* The node an interface pointer stands for: a node is its own object, and the document object stands for its tree's document node. */
static Node *node_of(void *object) {
  if (*(const void **)object == (const void *)&document_vtbl) return ((Document *)object)->tree->root;
  return (Node *)object;
}
/* Hands out a node (the document object for a document node), or NULL with S_FALSE. */
static HRESULT give(Node *n, void *out) {
  if (!out) return E_INVALIDARG;
  if (!n) { *(void **)out = NULL; return S_FALSE; }
  if (n->type == NODE_DOCUMENT) {
    document_addref(n->tree->document);
    *(void **)out = n->tree->document;
  } else {
    node_addref(n);
    *(void **)out = n;
  }
  return S_OK;
}
static HRESULT give_text(const WCHAR *text, SIZE_T length, BSTR *out) {
  if (!out) return E_INVALIDARG;
  *out = NULL;
  if (length > 0x3FFFFFFF) return E_OUTOFMEMORY;
  *out = SysAllocStringLen(text ? text : L"", (UINT)length);
  return *out ? S_OK : E_OUTOFMEMORY;
}
static HRESULT give_no_text(BSTR *out) {
  if (!out) return E_INVALIDARG;
  *out = NULL;
  return S_FALSE;
}
static HRESULT give_bool(BOOL value, VARIANT_BOOL *out) {
  if (!out) return E_INVALIDARG;
  *out = value ? VARIANT_TRUE : VARIANT_FALSE;
  return S_OK;
}
static HRESULT give_variant_text(const WCHAR *text, SIZE_T length, VARIANT *out) {
  if (!out) return E_INVALIDARG;
  V_VT(out) = VT_NULL;
  V_BSTR(out) = NULL;
  if (!text) return S_FALSE;
  BSTR copy;
  const HRESULT hr = give_text(text, length, &copy);
  if (FAILED(hr)) return hr;
  V_VT(out) = VT_BSTR;
  V_BSTR(out) = copy;
  return S_OK;
}

/* ---- text, serialization -------------------------------------------------------------------------------------------------------------------------- */

typedef struct Buffer { WCHAR *text; SIZE_T length, capacity; BOOL failed; } Buffer;

static void buffer_add(Buffer *b, const WCHAR *text, SIZE_T n) {
  if (b->failed) return;
  if (n > b->capacity - b->length) {
    SIZE_T capacity = b->capacity ? b->capacity : 256;
    while (capacity - b->length < n) {
      if (capacity > 0x20000000) { b->failed = TRUE; return; }
      capacity *= 2;
    }
    WCHAR *grown = b->text ? (WCHAR *)HeapReAlloc(GetProcessHeap(), 0, b->text, capacity * sizeof(WCHAR)) : (WCHAR *)HeapAlloc(GetProcessHeap(), 0, capacity * sizeof(WCHAR));
    if (!grown) { b->failed = TRUE; return; }
    b->text = grown;
    b->capacity = capacity;
  }
  for (SIZE_T i = 0; i < n; ++i) b->text[b->length + i] = text[i];
  b->length += n;
}
static void buffer_add_ascii(Buffer *b, const char *text) {
  for (; *text; ++text) { const WCHAR c = (WCHAR)(BYTE)*text; buffer_add(b, &c, 1); }
}
static HRESULT buffer_give(Buffer *b, BSTR *out) {
  HRESULT hr = b->failed ? E_OUTOFMEMORY : give_text(b->text, b->length, out);
  if (b->failed && out) *out = NULL;
  release_memory(b->text);
  return hr;
}

/* The node after `n` in the subtree of `top`, in document order (children, not attributes); NULL after the last. */
static Node *next_in_subtree(Node *n, const Node *top) {
  if (n->first) return n->first;
  while (n != top) {
    if (n->next) return n->next;
    n = n->parent;
  }
  return NULL;
}

/* The characters of a node as a program reads them: as MSXML does, text is read without the white space at its ends unless the tree was built
 * with preserveWhiteSpace (the tree keeps them, and get_xml writes them). */
static const WCHAR *value_of(const Node *n, UINT *length) {
  const WCHAR *value = n->value ? n->value : L"";
  UINT from = 0, to = n->value_length;
  if (n->type == NODE_TEXT && !n->tree->preserve) {
    while (from < to && is_space(value[from])) ++from;
    while (to > from && is_space(value[to - 1])) --to;
  }
  *length = to - from;
  return value + from;
}

/* The text of a node: its value, or for an element and the document the text and CDATA inside, in order - without the white space at either end
 * unless the tree was built with preserveWhiteSpace. */
static HRESULT text_of(Node *n, BSTR *out) {
  if (!out) return E_INVALIDARG;
  if (n->type != NODE_ELEMENT && n->type != NODE_DOCUMENT) {
    UINT length;
    const WCHAR *value = value_of(n, &length);
    return give_text(value, length, out);
  }
  Buffer b;
  zero(&b, sizeof(b));
  for (Node *d = n->first ? n : NULL; d; d = next_in_subtree(d, n))
    if (d->type == NODE_TEXT || d->type == NODE_CDATA_SECTION) buffer_add(&b, d->value, d->value_length);
  if (b.failed) { release_memory(b.text); *out = NULL; return E_OUTOFMEMORY; }
  SIZE_T from = 0, to = b.length;
  if (!n->tree->preserve) {
    while (from < to && is_space(b.text[from])) ++from;
    while (to > from && is_space(b.text[to - 1])) --to;
  }
  const HRESULT hr = give_text(b.text + from, to - from, out);
  release_memory(b.text);
  return hr;
}

static void add_escaped(Buffer *b, const WCHAR *text, SIZE_T n, BOOL attribute) {
  SIZE_T plain = 0;      /* (where the run of characters that need nothing starts) */
  for (SIZE_T i = 0; i < n; ++i) {
    const char *replacement = NULL;
    switch (text[i]) {
      case '&': replacement = "&amp;"; break;
      case '<': replacement = "&lt;"; break;
      case '>': if (!attribute) replacement = "&gt;"; break;
      case '"': if (attribute) replacement = "&quot;"; break;
      case '\r': replacement = "&#13;"; break;
      case '\n': if (attribute) replacement = "&#10;"; break;
      case '\t': if (attribute) replacement = "&#9;"; break;
    }
    if (!replacement) continue;
    buffer_add(b, text + plain, i - plain);
    buffer_add_ascii(b, replacement);
    plain = i + 1;
  }
  buffer_add(b, text + plain, n - plain);
}
/* What a node starts with in XML; for an element that has children, up to the '>' of its start tag. */
static void add_start(Buffer *b, const Node *n) {
  switch (n->type) {
    case NODE_ELEMENT:
      buffer_add_ascii(b, "<");
      buffer_add(b, n->name, n->name_length);
      for (const Node *a = n->attributes; a; a = a->next) {
        buffer_add_ascii(b, " ");
        buffer_add(b, a->name, a->name_length);
        buffer_add_ascii(b, "=\"");
        add_escaped(b, a->value, a->value_length, TRUE);
        buffer_add_ascii(b, "\"");
      }
      buffer_add_ascii(b, n->first ? ">" : "/>");
      break;
    case NODE_ATTRIBUTE:
      buffer_add(b, n->name, n->name_length);
      buffer_add_ascii(b, "=\"");
      add_escaped(b, n->value, n->value_length, TRUE);
      buffer_add_ascii(b, "\"");
      break;
    case NODE_TEXT: add_escaped(b, n->value, n->value_length, FALSE); break;
    case NODE_CDATA_SECTION:
      buffer_add_ascii(b, "<![CDATA[");
      buffer_add(b, n->value, n->value_length);
      buffer_add_ascii(b, "]]>");
      break;
    case NODE_COMMENT:
      buffer_add_ascii(b, "<!--");
      buffer_add(b, n->value, n->value_length);
      buffer_add_ascii(b, "-->");
      break;
    case NODE_PROCESSING_INSTRUCTION:
      buffer_add_ascii(b, "<?");
      buffer_add(b, n->name, n->name_length);
      if (n->value_length) { buffer_add_ascii(b, " "); buffer_add(b, n->value, n->value_length); }
      buffer_add_ascii(b, "?>");
      break;
    default: break;
  }
}
/* A node as XML. The document's is its children, a line each (as MSXML writes it). */
static HRESULT xml_of(Node *top, BSTR *out) {
  if (!out) return E_INVALIDARG;
  Buffer b;
  zero(&b, sizeof(b));
  Node *n = top;
  for (;;) {
    add_start(&b, n);
    if (n->first) { n = n->first; continue; }
    for (;;) {      /* (the node is finished: on to its sibling, or up, closing the elements that end here) */
      if (n != top && n->parent->type == NODE_DOCUMENT) buffer_add_ascii(&b, "\r\n");
      if (n == top) return buffer_give(&b, out);
      if (n->next) { n = n->next; break; }
      n = n->parent;
      if (n->type == NODE_ELEMENT) { buffer_add_ascii(&b, "</"); buffer_add(&b, n->name, n->name_length); buffer_add_ascii(&b, ">"); }
    }
  }
}

/* ---- sets of nodes, paths ------------------------------------------------------------------------------------------------------------------------- */

typedef struct Set { Node **items; SIZE_T count, capacity; BOOL failed; } Set;

static void set_add(Set *s, Node *n) {
  if (s->failed) return;
  if (s->count == s->capacity) {
    const SIZE_T capacity = s->capacity ? s->capacity * 2 : 16;
    if (capacity > 0x1000000) { s->failed = TRUE; return; }
    Node **grown = s->items ? (Node **)HeapReAlloc(GetProcessHeap(), 0, s->items, capacity * sizeof(Node *)) : (Node **)HeapAlloc(GetProcessHeap(), 0, capacity * sizeof(Node *));
    if (!grown) { s->failed = TRUE; return; }
    s->items = grown;
    s->capacity = capacity;
  }
  s->items[s->count++] = n;
}
static void set_free(Set *s) { release_memory(s->items); zero(s, sizeof(*s)); }

/* Puts a set into document order, each node once (a heap sort: no recursion, no worst case). */
static void set_normalize(Set *s) {
  Node **a = s->items;
  SIZE_T n = s->count, i;
  for (i = 1; i < n; ++i) if (a[i - 1]->order >= a[i]->order) break;
  if (i >= n) return;      /* (it is in order already) */
  for (SIZE_T start = n / 2; start-- > 0;) {
    for (SIZE_T root = start;;) {
      SIZE_T child = 2 * root + 1;
      if (child >= n) break;
      if (child + 1 < n && a[child]->order < a[child + 1]->order) ++child;
      if (a[root]->order >= a[child]->order) break;
      Node *t = a[root]; a[root] = a[child]; a[child] = t;
      root = child;
    }
  }
  for (SIZE_T end = n; end-- > 1;) {
    Node *t = a[0]; a[0] = a[end]; a[end] = t;
    for (SIZE_T root = 0;;) {
      SIZE_T child = 2 * root + 1;
      if (child >= end) break;
      if (child + 1 < end && a[child]->order < a[child + 1]->order) ++child;
      if (a[root]->order >= a[child]->order) break;
      t = a[root]; a[root] = a[child]; a[child] = t;
      root = child;
    }
  }
  SIZE_T kept = 1;
  for (i = 1; i < n; ++i) if (a[i] != a[kept - 1]) a[kept++] = a[i];
  s->count = kept;
}

static Node *find_attribute(const Node *e, const WCHAR *name, SIZE_T length) {
  if (e->type != NODE_ELEMENT) return NULL;
  for (Node *a = e->attributes; a; a = a->next) if (same_text(a->name, a->name_length, name, length)) return a;
  return NULL;
}

enum { STEP_SELF, STEP_PARENT, STEP_CHILD, STEP_ATTRIBUTE, STEP_TEXT, STEP_NODE };
typedef struct Step {
  int kind;
  const WCHAR *name;                /* of the element or attribute; NULL: any */
  SIZE_T name_length;
  int test;                         /* 0: none; 1: [@attribute]; 2: [@attribute='value'] */
  const WCHAR *attribute, *value;
  SIZE_T attribute_length, value_length;
} Step;

static SIZE_T name_at(const WCHAR *e, SIZE_T length, SIZE_T i) {
  SIZE_T n = 0;
  while (i + n < length && is_name_char(e[i + n])) ++n;
  return n;
}
/* One step of a path at e[*at]; FALSE if it is not one this library understands. */
static BOOL parse_step(const WCHAR *e, SIZE_T length, SIZE_T *at, Step *step) {
  SIZE_T i = *at;
  zero(step, sizeof(*step));
  if (i >= length) return FALSE;
  if (e[i] == '.') {
    if (i + 1 < length && e[i + 1] == '.') { step->kind = STEP_PARENT; i += 2; }
    else { step->kind = STEP_SELF; i += 1; }
  } else if (e[i] == '@' || e[i] == '*') {
    step->kind = e[i] == '@' ? STEP_ATTRIBUTE : STEP_CHILD;
    if (e[i] == '@') ++i;
    if (i < length && e[i] == '*') ++i;
    else {
      step->name_length = name_at(e, length, i);
      if (!step->name_length) return FALSE;
      step->name = e + i;
      i += step->name_length;
    }
  } else {
    step->kind = STEP_CHILD;
    step->name_length = name_at(e, length, i);
    if (!step->name_length) return FALSE;
    step->name = e + i;
    i += step->name_length;
    if (length - i >= 2 && e[i] == '(' && e[i + 1] == ')') {
      if (same_ascii(step->name, step->name_length, "text")) step->kind = STEP_TEXT;
      else if (same_ascii(step->name, step->name_length, "node")) step->kind = STEP_NODE;
      else return FALSE;
      i += 2;
    }
  }
  if (i < length && e[i] == '[') {
    if (length - i < 2 || e[i + 1] != '@') return FALSE;
    i += 2;
    step->attribute_length = name_at(e, length, i);
    if (!step->attribute_length) return FALSE;
    step->attribute = e + i;
    i += step->attribute_length;
    step->test = 1;
    if (i < length && e[i] == '=') {
      ++i;
      if (i >= length || (e[i] != '\'' && e[i] != '"')) return FALSE;
      const WCHAR quote = e[i++];
      step->value = e + i;
      while (i < length && e[i] != quote) ++i;
      if (i >= length) return FALSE;
      step->value_length = (SIZE_T)(e + i - step->value);
      ++i;
      step->test = 2;
    }
    if (i >= length || e[i] != ']') return FALSE;
    ++i;
  }
  *at = i;
  return TRUE;
}
static BOOL passes(const Step *step, const Node *n) {
  if (!step->test) return TRUE;
  const Node *a = find_attribute(n, step->attribute, step->attribute_length);
  return a && (step->test == 1 || same_text(a->value, a->value_length, step->value, step->value_length));
}
/* What one step selects from one node. */
static void take_step(const Step *step, Node *from, Set *out) {
  switch (step->kind) {
    case STEP_SELF: if (passes(step, from)) set_add(out, from); break;
    case STEP_PARENT: if (from->parent && passes(step, from->parent)) set_add(out, from->parent); break;
    case STEP_ATTRIBUTE:
      if (from->type != NODE_ELEMENT) break;
      for (Node *a = from->attributes; a; a = a->next) if (!step->name || same_text(a->name, a->name_length, step->name, step->name_length)) set_add(out, a);
      break;
    default:
      if (from->type == NODE_ATTRIBUTE) break;
      for (Node *c = from->first; c; c = c->next) {
        if (step->kind == STEP_CHILD ? c->type == NODE_ELEMENT && (!step->name || same_text(c->name, c->name_length, step->name, step->name_length)) && passes(step, c)
            : step->kind == STEP_TEXT ? (c->type == NODE_TEXT || c->type == NODE_CDATA_SECTION) : TRUE) set_add(out, c);
      }
      break;
  }
}
/* The nodes the path e[0, length) selects from `context`, in document order. E_NOTIMPL for what is not one of the simple paths described at the top. */
static HRESULT select_nodes(Node *context, const WCHAR *e, SIZE_T length, Set *result) {
  Set current;
  SIZE_T i = 0;
  BOOL descendants = FALSE;
  zero(&current, sizeof(current));
  zero(result, sizeof(*result));
  if (length == 0) return E_FAIL;
  if (e[0] == '/') {
    context = context->tree->root;
    i = 1;
    if (i < length && e[i] == '/') { descendants = TRUE; ++i; }
  }
  set_add(&current, context);
  while (i < length) {
    Step step;
    Set next;
    UINT covered = 0;
    BOOL any_covered = FALSE;
    zero(&next, sizeof(next));
    if (!parse_step(e, length, &i, &step)) { set_free(&current); return E_NOTIMPL; }
    for (SIZE_T k = 0; k < current.count; ++k) {
      Node *c = current.items[k];
      if (!descendants || c->type == NODE_ATTRIBUTE) { take_step(&step, c, &next); continue; }
      if (any_covered && c->order <= covered) continue;      /* (inside a node whose descendants have been walked) */
      covered = c->last_order;
      any_covered = TRUE;
      for (Node *d = c; d; d = next_in_subtree(d, c)) take_step(&step, d, &next);
    }
    set_free(&current);
    if (next.failed) { set_free(&next); return E_OUTOFMEMORY; }
    set_normalize(&next);
    current = next;
    if (i == length) break;
    if (e[i] != '/') { set_free(&current); return E_NOTIMPL; }
    ++i;
    descendants = FALSE;
    if (i < length && e[i] == '/') { descendants = TRUE; ++i; }
    if (i == length) { set_free(&current); return E_FAIL; }
  }
  if (current.failed) { set_free(&current); return E_OUTOFMEMORY; }
  *result = current;
  return S_OK;
}

/* ---- IDispatch: not there (no type library) ---------------------------------------------------------------------------------------------------------- */

static HRESULT WINAPI dispatch_GetTypeInfoCount(IDispatch *This, UINT *count) { if (count) *count = 0; return E_NOTIMPL; }
static HRESULT WINAPI dispatch_GetTypeInfo(IDispatch *This, UINT index, LCID locale, ITypeInfo **info) { if (info) *info = NULL; return E_NOTIMPL; }
static HRESULT WINAPI dispatch_GetIDsOfNames(IDispatch *This, REFIID iid, LPOLESTR *names, UINT count, LCID locale, DISPID *ids) { return E_NOTIMPL; }
static HRESULT WINAPI dispatch_Invoke(IDispatch *This, DISPID id, REFIID iid, LCID locale, WORD flags, DISPPARAMS *parameters, VARIANT *result, EXCEPINFO *exception, UINT *bad) { return E_NOTIMPL; }
#define DISPATCH_METHODS \
  .GetTypeInfoCount = (void *)dispatch_GetTypeInfoCount, .GetTypeInfo = (void *)dispatch_GetTypeInfo, .GetIDsOfNames = (void *)dispatch_GetIDsOfNames, .Invoke = (void *)dispatch_Invoke

/* ---- IXMLDOMNodeList ------------------------------------------------------------------------------------------------------------------------------- */

typedef struct List {
  const IXMLDOMNodeListVtbl *vtbl;
  LONG references;
  Node *owner;                      /* a node of the tree, which the list holds a reference to */
  Node **items;
  LONG count, position;
} List;

static HRESULT WINAPI list_QueryInterface(IXMLDOMNodeList *This, REFIID iid, void **out) {
  if (!out) return E_POINTER;
  *out = NULL;
  if (!same_guid(iid, &kIidUnknown) && !same_guid(iid, &kIidDispatch) && !same_guid(iid, &kIidNodeList)) return E_NOINTERFACE;
  InterlockedIncrement(&((List *)This)->references);
  *out = This;
  return S_OK;
}
static ULONG WINAPI list_AddRef(IXMLDOMNodeList *This) { return (ULONG)InterlockedIncrement(&((List *)This)->references); }
static ULONG WINAPI list_Release(IXMLDOMNodeList *This) {
  List *l = (List *)This;
  const LONG left = InterlockedDecrement(&l->references);
  if (left == 0) {
    node_release(l->owner);
    release_memory(l->items);
    release_memory(l);
    InterlockedDecrement(&g_objects);
  }
  return (ULONG)left;
}
static HRESULT WINAPI list_get_item(IXMLDOMNodeList *This, LONG index, IXMLDOMNode **out) {
  List *l = (List *)This;
  return give(index >= 0 && index < l->count ? l->items[index] : NULL, out);
}
static HRESULT WINAPI list_get_length(IXMLDOMNodeList *This, LONG *out) {
  if (!out) return E_INVALIDARG;
  *out = ((List *)This)->count;
  return S_OK;
}
static HRESULT WINAPI list_nextNode(IXMLDOMNodeList *This, IXMLDOMNode **out) {
  List *l = (List *)This;
  if (!out) return E_INVALIDARG;
  return give(l->position < l->count ? l->items[l->position++] : NULL, out);
}
static HRESULT WINAPI list_reset(IXMLDOMNodeList *This) { ((List *)This)->position = 0; return S_OK; }
static HRESULT WINAPI list_get__newEnum(IXMLDOMNodeList *This, IUnknown **out) { if (out) *out = NULL; return E_NOTIMPL; }
static const IXMLDOMNodeListVtbl list_vtbl = {
  .QueryInterface = list_QueryInterface, .AddRef = list_AddRef, .Release = list_Release, DISPATCH_METHODS,
  .get_item = list_get_item, .get_length = list_get_length, .nextNode = list_nextNode, .reset = list_reset, ._newEnum = list_get__newEnum,
};

/* A list of the nodes of `set`, which it takes over (also when it fails). */
static HRESULT new_list(Node *owner, Set *set, IXMLDOMNodeList **out) {
  if (!out) { set_free(set); return E_INVALIDARG; }
  *out = NULL;
  List *l = set->failed ? NULL : (List *)alloc(sizeof(List));
  if (!l) { set_free(set); return E_OUTOFMEMORY; }
  l->vtbl = &list_vtbl;
  l->references = 1;
  l->owner = owner;
  l->items = set->items;
  l->count = (LONG)set->count;
  node_addref(owner);
  InterlockedIncrement(&g_objects);
  *out = (IXMLDOMNodeList *)l;
  return S_OK;
}

/* ---- IXMLDOMNamedNodeMap: the attributes of an element ---------------------------------------------------------------------------------------------- */

typedef struct Map {
  const IXMLDOMNamedNodeMapVtbl *vtbl;
  LONG references;
  Node *element;
  Node *next;                       /* what nextNode gives */
} Map;

static HRESULT WINAPI map_QueryInterface(IXMLDOMNamedNodeMap *This, REFIID iid, void **out) {
  if (!out) return E_POINTER;
  *out = NULL;
  if (!same_guid(iid, &kIidUnknown) && !same_guid(iid, &kIidDispatch) && !same_guid(iid, &kIidNamedNodeMap)) return E_NOINTERFACE;
  InterlockedIncrement(&((Map *)This)->references);
  *out = This;
  return S_OK;
}
static ULONG WINAPI map_AddRef(IXMLDOMNamedNodeMap *This) { return (ULONG)InterlockedIncrement(&((Map *)This)->references); }
static ULONG WINAPI map_Release(IXMLDOMNamedNodeMap *This) {
  Map *m = (Map *)This;
  const LONG left = InterlockedDecrement(&m->references);
  if (left == 0) {
    node_release(m->element);
    release_memory(m);
    InterlockedDecrement(&g_objects);
  }
  return (ULONG)left;
}
static HRESULT WINAPI map_getNamedItem(IXMLDOMNamedNodeMap *This, BSTR name, IXMLDOMNode **out) {
  return give(find_attribute(((Map *)This)->element, name, SysStringLen(name)), out);
}
static HRESULT WINAPI map_setNamedItem(IXMLDOMNamedNodeMap *This, IXMLDOMNode *node, IXMLDOMNode **out) { if (out) *out = NULL; return E_NOTIMPL; }
static HRESULT WINAPI map_removeNamedItem(IXMLDOMNamedNodeMap *This, BSTR name, IXMLDOMNode **out) { if (out) *out = NULL; return E_NOTIMPL; }
static HRESULT WINAPI map_get_item(IXMLDOMNamedNodeMap *This, LONG index, IXMLDOMNode **out) {
  Node *a = index < 0 ? NULL : ((Map *)This)->element->attributes;
  for (; a && index > 0; --index) a = a->next;
  return give(a, out);
}
static HRESULT WINAPI map_get_length(IXMLDOMNamedNodeMap *This, LONG *out) {
  if (!out) return E_INVALIDARG;
  *out = (LONG)((Map *)This)->element->attribute_count;
  return S_OK;
}
/* (by the name without its prefix, and the namespace: none for a NULL or empty one) */
static HRESULT WINAPI map_getQualifiedItem(IXMLDOMNamedNodeMap *This, BSTR name, BSTR uri, IXMLDOMNode **out) {
  const UINT name_length = SysStringLen(name), uri_length = SysStringLen(uri);
  Node *a = ((Map *)This)->element->attributes;
  for (; a; a = a->next) {
    const UINT skip = a->prefix_length ? a->prefix_length + 1 : 0;
    if (same_text(a->name + skip, a->name_length - skip, name, name_length) && same_text(a->uri, a->uri_length, uri, uri_length)) break;
  }
  return give(a, out);
}
static HRESULT WINAPI map_removeQualifiedItem(IXMLDOMNamedNodeMap *This, BSTR name, BSTR uri, IXMLDOMNode **out) { if (out) *out = NULL; return E_NOTIMPL; }
static HRESULT WINAPI map_nextNode(IXMLDOMNamedNodeMap *This, IXMLDOMNode **out) {
  Map *m = (Map *)This;
  Node *a = m->next;
  if (!out) return E_INVALIDARG;
  if (a) m->next = a->next;
  return give(a, out);
}
static HRESULT WINAPI map_reset(IXMLDOMNamedNodeMap *This) { ((Map *)This)->next = ((Map *)This)->element->attributes; return S_OK; }
static HRESULT WINAPI map_get__newEnum(IXMLDOMNamedNodeMap *This, IUnknown **out) { if (out) *out = NULL; return E_NOTIMPL; }
static const IXMLDOMNamedNodeMapVtbl map_vtbl = {
  .QueryInterface = map_QueryInterface, .AddRef = map_AddRef, .Release = map_Release, DISPATCH_METHODS,
  .getNamedItem = map_getNamedItem, .setNamedItem = map_setNamedItem, .removeNamedItem = map_removeNamedItem, .get_item = map_get_item, .get_length = map_get_length,
  .getQualifiedItem = map_getQualifiedItem, .removeQualifiedItem = map_removeQualifiedItem, .nextNode = map_nextNode, .reset = map_reset, ._newEnum = map_get__newEnum,
};

/* ---- IXMLDOMNode: what every node, and the document, has in common ----------------------------------------------------------------------------------- */

static HRESULT WINAPI node_QueryInterface(IXMLDOMNode *This, REFIID iid, void **out) {
  Node *n = (Node *)This;
  if (!out) return E_POINTER;
  *out = NULL;
  BOOL mine = same_guid(iid, &kIidUnknown) || same_guid(iid, &kIidDispatch) || same_guid(iid, &kIidNode);
  switch (n->type) {
    case NODE_ELEMENT: mine = mine || same_guid(iid, &kIidElement); break;
    case NODE_ATTRIBUTE: mine = mine || same_guid(iid, &kIidAttribute); break;
    case NODE_TEXT: mine = mine || same_guid(iid, &kIidCharacterData) || same_guid(iid, &kIidText); break;
    case NODE_CDATA_SECTION: mine = mine || same_guid(iid, &kIidCharacterData) || same_guid(iid, &kIidText) || same_guid(iid, &kIidCdata); break;
    case NODE_COMMENT: mine = mine || same_guid(iid, &kIidCharacterData) || same_guid(iid, &kIidComment); break;
    case NODE_PROCESSING_INSTRUCTION: mine = mine || same_guid(iid, &kIidInstruction); break;
    default: break;
  }
  if (!mine) return E_NOINTERFACE;
  node_addref(n);
  *out = n;
  return S_OK;
}
static ULONG WINAPI node_AddRef(IXMLDOMNode *This) { return node_addref((Node *)This); }
static ULONG WINAPI node_Release(IXMLDOMNode *This) { return node_release((Node *)This); }

static HRESULT WINAPI node_get_nodeName(IXMLDOMNode *This, BSTR *out) {
  const Node *n = node_of(This);
  switch (n->type) {
    case NODE_TEXT: return give_text(L"#text", 5, out);
    case NODE_CDATA_SECTION: return give_text(L"#cdata-section", 14, out);
    case NODE_COMMENT: return give_text(L"#comment", 8, out);
    case NODE_DOCUMENT: return give_text(L"#document", 9, out);
    default: return give_text(n->name, n->name_length, out);
  }
}
static HRESULT WINAPI node_get_nodeValue(IXMLDOMNode *This, VARIANT *out) {
  const Node *n = node_of(This);
  UINT length;
  if (n->type == NODE_ELEMENT || n->type == NODE_DOCUMENT) return give_variant_text(NULL, 0, out);
  const WCHAR *value = value_of(n, &length);
  return give_variant_text(value, length, out);
}
static HRESULT WINAPI node_put_nodeValue(IXMLDOMNode *This, VARIANT value) { return E_NOTIMPL; }
static HRESULT WINAPI node_get_nodeType(IXMLDOMNode *This, DOMNodeType *out) {
  if (!out) return E_INVALIDARG;
  *out = (DOMNodeType)node_of(This)->type;
  return S_OK;
}
static HRESULT WINAPI node_get_parentNode(IXMLDOMNode *This, IXMLDOMNode **out) {
  Node *n = node_of(This);
  return give(n->type == NODE_ATTRIBUTE ? NULL : n->parent, out);      /* (an attribute has no parent in the DOM) */
}
static HRESULT WINAPI node_get_childNodes(IXMLDOMNode *This, IXMLDOMNodeList **out) {
  Node *n = node_of(This);
  Set set;
  zero(&set, sizeof(set));
  for (Node *c = n->first; c; c = c->next) set_add(&set, c);
  return new_list(n, &set, out);
}
static HRESULT WINAPI node_get_firstChild(IXMLDOMNode *This, IXMLDOMNode **out) { return give(node_of(This)->first, out); }
static HRESULT WINAPI node_get_lastChild(IXMLDOMNode *This, IXMLDOMNode **out) { return give(node_of(This)->last, out); }
static HRESULT WINAPI node_get_previousSibling(IXMLDOMNode *This, IXMLDOMNode **out) {
  Node *n = node_of(This);
  return give(n->type == NODE_ATTRIBUTE ? NULL : n->previous, out);
}
static HRESULT WINAPI node_get_nextSibling(IXMLDOMNode *This, IXMLDOMNode **out) {
  Node *n = node_of(This);
  return give(n->type == NODE_ATTRIBUTE ? NULL : n->next, out);
}
static HRESULT WINAPI node_get_attributes(IXMLDOMNode *This, IXMLDOMNamedNodeMap **out) {
  Node *n = node_of(This);
  if (!out) return E_INVALIDARG;
  *out = NULL;
  if (n->type != NODE_ELEMENT) return S_FALSE;
  Map *m = (Map *)alloc(sizeof(Map));
  if (!m) return E_OUTOFMEMORY;
  m->vtbl = &map_vtbl;
  m->references = 1;
  m->element = n;
  m->next = n->attributes;
  node_addref(n);
  InterlockedIncrement(&g_objects);
  *out = (IXMLDOMNamedNodeMap *)m;
  return S_OK;
}
static HRESULT WINAPI node_insertBefore(IXMLDOMNode *This, IXMLDOMNode *node, VARIANT before, IXMLDOMNode **out) { if (out) *out = NULL; return E_NOTIMPL; }
static HRESULT WINAPI node_replaceChild(IXMLDOMNode *This, IXMLDOMNode *node, IXMLDOMNode *old, IXMLDOMNode **out) { if (out) *out = NULL; return E_NOTIMPL; }
static HRESULT WINAPI node_removeChild(IXMLDOMNode *This, IXMLDOMNode *node, IXMLDOMNode **out) { if (out) *out = NULL; return E_NOTIMPL; }
static HRESULT WINAPI node_appendChild(IXMLDOMNode *This, IXMLDOMNode *node, IXMLDOMNode **out) { if (out) *out = NULL; return E_NOTIMPL; }
static HRESULT WINAPI node_hasChildNodes(IXMLDOMNode *This, VARIANT_BOOL *out) {
  const BOOL has = node_of(This)->first != NULL;
  if (!out) return E_INVALIDARG;
  *out = has ? VARIANT_TRUE : VARIANT_FALSE;
  return has ? S_OK : S_FALSE;
}
static HRESULT WINAPI node_get_ownerDocument(IXMLDOMNode *This, IXMLDOMDocument **out) {
  Node *n = node_of(This);
  return give(n->type == NODE_DOCUMENT ? NULL : n->tree->root, out);
}
static HRESULT WINAPI node_cloneNode(IXMLDOMNode *This, VARIANT_BOOL deep, IXMLDOMNode **out) { if (out) *out = NULL; return E_NOTIMPL; }
static HRESULT WINAPI node_get_nodeTypeString(IXMLDOMNode *This, BSTR *out) {
  static const WCHAR *const names[] = {L"", L"element", L"attribute", L"text", L"cdatasection", L"entityreference", L"entity", L"processinginstruction", L"comment", L"document"};
  const int type = node_of(This)->type;
  const WCHAR *name = type >= 1 && type <= 9 ? names[type] : L"";
  SIZE_T length = 0;
  while (name[length]) ++length;
  return give_text(name, length, out);
}
static HRESULT WINAPI node_get_text(IXMLDOMNode *This, BSTR *out) { return text_of(node_of(This), out); }
static HRESULT WINAPI node_put_text(IXMLDOMNode *This, BSTR text) { return E_NOTIMPL; }
static HRESULT WINAPI node_get_specified(IXMLDOMNode *This, VARIANT_BOOL *out) { return give_bool(TRUE, out); }
static HRESULT WINAPI node_get_definition(IXMLDOMNode *This, IXMLDOMNode **out) { return give(NULL, out); }
static HRESULT WINAPI node_get_nodeTypedValue(IXMLDOMNode *This, VARIANT *out) {
  Node *n = node_of(This);
  if (!out) return E_INVALIDARG;
  V_VT(out) = VT_NULL;
  V_BSTR(out) = NULL;
  if (n->type == NODE_DOCUMENT) return S_FALSE;
  BSTR text;
  const HRESULT hr = text_of(n, &text);
  if (FAILED(hr)) return hr;
  V_VT(out) = VT_BSTR;
  V_BSTR(out) = text;
  return S_OK;
}
static HRESULT WINAPI node_put_nodeTypedValue(IXMLDOMNode *This, VARIANT value) { return E_NOTIMPL; }
static HRESULT WINAPI node_get_dataType(IXMLDOMNode *This, VARIANT *out) { return give_variant_text(NULL, 0, out); }
static HRESULT WINAPI node_put_dataType(IXMLDOMNode *This, BSTR type) { return E_NOTIMPL; }
static HRESULT WINAPI node_get_xml(IXMLDOMNode *This, BSTR *out) { return xml_of(node_of(This), out); }
static HRESULT WINAPI node_transformNode(IXMLDOMNode *This, IXMLDOMNode *sheet, BSTR *out) { if (out) *out = NULL; return E_NOTIMPL; }
static HRESULT WINAPI node_selectNodes(IXMLDOMNode *This, BSTR path, IXMLDOMNodeList **out) {
  Node *n = node_of(This);
  Set set;
  if (!out) return E_INVALIDARG;
  *out = NULL;
  const HRESULT hr = select_nodes(n, path, SysStringLen(path), &set);
  if (FAILED(hr)) return hr;
  return new_list(n, &set, out);
}
static HRESULT WINAPI node_selectSingleNode(IXMLDOMNode *This, BSTR path, IXMLDOMNode **out) {
  Set set;
  if (!out) return E_INVALIDARG;
  *out = NULL;
  HRESULT hr = select_nodes(node_of(This), path, SysStringLen(path), &set);
  if (FAILED(hr)) return hr;
  hr = give(set.count ? set.items[0] : NULL, out);
  set_free(&set);
  return hr;
}
static HRESULT WINAPI node_get_parsed(IXMLDOMNode *This, VARIANT_BOOL *out) { return give_bool(TRUE, out); }
static HRESULT WINAPI node_get_namespaceURI(IXMLDOMNode *This, BSTR *out) {
  const Node *n = node_of(This);
  return n->uri ? give_text(n->uri, n->uri_length, out) : give_no_text(out);
}
static HRESULT WINAPI node_get_prefix(IXMLDOMNode *This, BSTR *out) {
  const Node *n = node_of(This);
  return n->prefix_length ? give_text(n->name, n->prefix_length, out) : give_no_text(out);
}
static HRESULT WINAPI node_get_baseName(IXMLDOMNode *This, BSTR *out) {
  const Node *n = node_of(This);
  if (n->type != NODE_ELEMENT && n->type != NODE_ATTRIBUTE && n->type != NODE_PROCESSING_INSTRUCTION) return give_no_text(out);
  const UINT skip = n->prefix_length ? n->prefix_length + 1 : 0;
  return give_text(n->name + skip, n->name_length - skip, out);
}
static HRESULT WINAPI node_transformNodeToObject(IXMLDOMNode *This, IXMLDOMNode *sheet, VARIANT output) { return E_NOTIMPL; }

/* (the methods above in an interface that begins with IXMLDOMNode's; each has IXMLDOMNode's signature but for the type of This) */
#define NODE_METHODS \
  DISPATCH_METHODS, \
  .get_nodeName = (void *)node_get_nodeName, .get_nodeValue = (void *)node_get_nodeValue, .put_nodeValue = (void *)node_put_nodeValue, .get_nodeType = (void *)node_get_nodeType, \
  .get_parentNode = (void *)node_get_parentNode, .get_childNodes = (void *)node_get_childNodes, .get_firstChild = (void *)node_get_firstChild, \
  .get_lastChild = (void *)node_get_lastChild, .get_previousSibling = (void *)node_get_previousSibling, .get_nextSibling = (void *)node_get_nextSibling, \
  .get_attributes = (void *)node_get_attributes, .insertBefore = (void *)node_insertBefore, .replaceChild = (void *)node_replaceChild, .removeChild = (void *)node_removeChild, \
  .appendChild = (void *)node_appendChild, .hasChildNodes = (void *)node_hasChildNodes, .get_ownerDocument = (void *)node_get_ownerDocument, .cloneNode = (void *)node_cloneNode, \
  .get_nodeTypeString = (void *)node_get_nodeTypeString, .get_text = (void *)node_get_text, .put_text = (void *)node_put_text, .get_specified = (void *)node_get_specified, \
  .get_definition = (void *)node_get_definition, .get_nodeTypedValue = (void *)node_get_nodeTypedValue, .put_nodeTypedValue = (void *)node_put_nodeTypedValue, \
  .get_dataType = (void *)node_get_dataType, .put_dataType = (void *)node_put_dataType, .get_xml = (void *)node_get_xml, .transformNode = (void *)node_transformNode, \
  .selectNodes = (void *)node_selectNodes, .selectSingleNode = (void *)node_selectSingleNode, .get_parsed = (void *)node_get_parsed, \
  .get_namespaceURI = (void *)node_get_namespaceURI, .get_prefix = (void *)node_get_prefix, .get_baseName = (void *)node_get_baseName, \
  .transformNodeToObject = (void *)node_transformNodeToObject
#define NODE_UNKNOWN .QueryInterface = (void *)node_QueryInterface, .AddRef = (void *)node_AddRef, .Release = (void *)node_Release

/* (no node is a plain IXMLDOMNode; this table has the compiler check each signature above against the interface, which the casts of NODE_METHODS do not) */
__attribute__((unused)) static const IXMLDOMNodeVtbl node_signature_check = {
  node_QueryInterface, node_AddRef, node_Release, (void *)dispatch_GetTypeInfoCount, (void *)dispatch_GetTypeInfo, (void *)dispatch_GetIDsOfNames, (void *)dispatch_Invoke,
  node_get_nodeName, node_get_nodeValue, node_put_nodeValue, node_get_nodeType, node_get_parentNode, node_get_childNodes, node_get_firstChild, node_get_lastChild,
  node_get_previousSibling, node_get_nextSibling, node_get_attributes, node_insertBefore, node_replaceChild, node_removeChild, node_appendChild, node_hasChildNodes,
  node_get_ownerDocument, node_cloneNode, node_get_nodeTypeString, node_get_text, node_put_text, node_get_specified, node_get_definition, node_get_nodeTypedValue,
  node_put_nodeTypedValue, node_get_dataType, node_put_dataType, node_get_xml, node_transformNode, node_selectNodes, node_selectSingleNode, node_get_parsed,
  node_get_namespaceURI, node_get_prefix, node_get_baseName, node_transformNodeToObject,
};
__attribute__((unused)) static const IDispatchVtbl dispatch_signature_check = {
  NULL, NULL, NULL, dispatch_GetTypeInfoCount, dispatch_GetTypeInfo, dispatch_GetIDsOfNames, dispatch_Invoke,
};

/* The elements below `top` called `name` ("*": all), in document order. */
static HRESULT elements_by_name(Node *top, BSTR name, IXMLDOMNodeList **out) {
  const UINT length = SysStringLen(name);
  const BOOL all = length == 1 && name[0] == '*';
  Set set;
  zero(&set, sizeof(set));
  for (Node *d = top->first ? top : NULL; d; d = next_in_subtree(d, top))
    if (d != top && d->type == NODE_ELEMENT && (all || same_text(d->name, d->name_length, name, length))) set_add(&set, d);
  return new_list(top, &set, out);
}

/* ---- IXMLDOMElement ---------------------------------------------------------------------------------------------------------------------------------- */

static HRESULT WINAPI element_get_tagName(IXMLDOMElement *This, BSTR *out) { return give_text(((Node *)This)->name, ((Node *)This)->name_length, out); }
static HRESULT WINAPI element_getAttribute(IXMLDOMElement *This, BSTR name, VARIANT *out) {
  const Node *a = find_attribute((Node *)This, name, SysStringLen(name));
  return give_variant_text(a ? a->value : NULL, a ? a->value_length : 0, out);
}
static HRESULT WINAPI element_setAttribute(IXMLDOMElement *This, BSTR name, VARIANT value) { return E_NOTIMPL; }
static HRESULT WINAPI element_removeAttribute(IXMLDOMElement *This, BSTR name) { return E_NOTIMPL; }
static HRESULT WINAPI element_getAttributeNode(IXMLDOMElement *This, BSTR name, IXMLDOMAttribute **out) {
  return give(find_attribute((Node *)This, name, SysStringLen(name)), out);
}
static HRESULT WINAPI element_setAttributeNode(IXMLDOMElement *This, IXMLDOMAttribute *attribute, IXMLDOMAttribute **out) { if (out) *out = NULL; return E_NOTIMPL; }
static HRESULT WINAPI element_removeAttributeNode(IXMLDOMElement *This, IXMLDOMAttribute *attribute, IXMLDOMAttribute **out) { if (out) *out = NULL; return E_NOTIMPL; }
static HRESULT WINAPI element_getElementsByTagName(IXMLDOMElement *This, BSTR name, IXMLDOMNodeList **out) { return elements_by_name((Node *)This, name, out); }
static HRESULT WINAPI element_normalize(IXMLDOMElement *This) { return S_OK; }      /* (a parsed tree has no adjacent text nodes) */
static const IXMLDOMElementVtbl element_vtbl = {
  NODE_UNKNOWN, NODE_METHODS,
  .get_tagName = element_get_tagName, .getAttribute = element_getAttribute, .setAttribute = element_setAttribute, .removeAttribute = element_removeAttribute,
  .getAttributeNode = element_getAttributeNode, .setAttributeNode = element_setAttributeNode, .removeAttributeNode = element_removeAttributeNode,
  .getElementsByTagName = element_getElementsByTagName, .normalize = element_normalize,
};

/* ---- IXMLDOMAttribute -------------------------------------------------------------------------------------------------------------------------------- */

static HRESULT WINAPI attribute_get_name(IXMLDOMAttribute *This, BSTR *out) { return give_text(((Node *)This)->name, ((Node *)This)->name_length, out); }
static HRESULT WINAPI attribute_get_value(IXMLDOMAttribute *This, VARIANT *out) { return give_variant_text(((Node *)This)->value, ((Node *)This)->value_length, out); }
static HRESULT WINAPI attribute_put_value(IXMLDOMAttribute *This, VARIANT value) { return E_NOTIMPL; }
static const IXMLDOMAttributeVtbl attribute_vtbl = {
  NODE_UNKNOWN, NODE_METHODS, .get_name = attribute_get_name, .get_value = attribute_get_value, .put_value = attribute_put_value,
};

/* ---- IXMLDOMCharacterData: text, CDATA sections, comments --------------------------------------------------------------------------------------------- */

static HRESULT WINAPI data_get_data(IXMLDOMCharacterData *This, BSTR *out) {
  UINT length;
  const WCHAR *value = value_of((Node *)This, &length);
  return give_text(value, length, out);
}
static HRESULT WINAPI data_put_data(IXMLDOMCharacterData *This, BSTR data) { return E_NOTIMPL; }
static HRESULT WINAPI data_get_length(IXMLDOMCharacterData *This, LONG *out) {
  if (!out) return E_INVALIDARG;
  UINT length;
  value_of((Node *)This, &length);
  *out = (LONG)length;
  return S_OK;
}
static HRESULT WINAPI data_substringData(IXMLDOMCharacterData *This, LONG offset, LONG count, BSTR *out) {
  UINT length;
  const WCHAR *value = value_of((Node *)This, &length);
  if (!out) return E_INVALIDARG;
  *out = NULL;
  if (offset < 0 || count < 0 || (UINT)offset > length) return E_INVALIDARG;
  if (count == 0 || (UINT)offset == length) return S_FALSE;
  if ((UINT)count > length - (UINT)offset) count = (LONG)(length - (UINT)offset);
  return give_text(value + offset, (SIZE_T)count, out);
}
static HRESULT WINAPI data_appendData(IXMLDOMCharacterData *This, BSTR data) { return E_NOTIMPL; }
static HRESULT WINAPI data_insertData(IXMLDOMCharacterData *This, LONG offset, BSTR data) { return E_NOTIMPL; }
static HRESULT WINAPI data_deleteData(IXMLDOMCharacterData *This, LONG offset, LONG count) { return E_NOTIMPL; }
static HRESULT WINAPI data_replaceData(IXMLDOMCharacterData *This, LONG offset, LONG count, BSTR data) { return E_NOTIMPL; }
static HRESULT WINAPI text_splitText(IXMLDOMText *This, LONG offset, IXMLDOMText **out) { if (out) *out = NULL; return E_NOTIMPL; }
#define DATA_METHODS \
  .get_data = (void *)data_get_data, .put_data = (void *)data_put_data, .get_length = (void *)data_get_length, .substringData = (void *)data_substringData, \
  .appendData = (void *)data_appendData, .insertData = (void *)data_insertData, .deleteData = (void *)data_deleteData, .replaceData = (void *)data_replaceData
/* (again for the compiler: the signatures of the eight) */
__attribute__((unused)) static const IXMLDOMCharacterDataVtbl data_signature_check = {
  NODE_UNKNOWN, NODE_METHODS,
  .get_data = data_get_data, .put_data = data_put_data, .get_length = data_get_length, .substringData = data_substringData,
  .appendData = data_appendData, .insertData = data_insertData, .deleteData = data_deleteData, .replaceData = data_replaceData,
};
static const IXMLDOMTextVtbl text_vtbl = { NODE_UNKNOWN, NODE_METHODS, DATA_METHODS, .splitText = text_splitText };
static const IXMLDOMCDATASectionVtbl cdata_vtbl = { NODE_UNKNOWN, NODE_METHODS, DATA_METHODS, .splitText = (void *)text_splitText };
static const IXMLDOMCommentVtbl comment_vtbl = { NODE_UNKNOWN, NODE_METHODS, DATA_METHODS };

/* ---- IXMLDOMProcessingInstruction ---------------------------------------------------------------------------------------------------------------------- */

static HRESULT WINAPI instruction_get_target(IXMLDOMProcessingInstruction *This, BSTR *out) { return give_text(((Node *)This)->name, ((Node *)This)->name_length, out); }
static HRESULT WINAPI instruction_get_data(IXMLDOMProcessingInstruction *This, BSTR *out) { return give_text(((Node *)This)->value, ((Node *)This)->value_length, out); }
static HRESULT WINAPI instruction_put_data(IXMLDOMProcessingInstruction *This, BSTR data) { return E_NOTIMPL; }
static const IXMLDOMProcessingInstructionVtbl instruction_vtbl = {
  NODE_UNKNOWN, NODE_METHODS, .get_target = instruction_get_target, .get_data = instruction_get_data, .put_data = instruction_put_data,
};

static const void *vtbl_of_type(int type) {
  switch (type) {
    case NODE_ELEMENT: return &element_vtbl;
    case NODE_ATTRIBUTE: return &attribute_vtbl;
    case NODE_TEXT: return &text_vtbl;
    case NODE_CDATA_SECTION: return &cdata_vtbl;
    case NODE_COMMENT: return &comment_vtbl;
    case NODE_PROCESSING_INSTRUCTION: return &instruction_vtbl;
    default: return NULL;      /* (the document node, which is never handed out) */
  }
}

/* ---- IXMLDOMParseError: a copy of the document's last error -------------------------------------------------------------------------------------------- */

typedef struct ParseError {
  const IXMLDOMParseErrorVtbl *vtbl;
  LONG references;
  DWORD code;
  BSTR reason, source, url;
  LONG line, column, position;
} ParseError;

static HRESULT WINAPI error_QueryInterface(IXMLDOMParseError *This, REFIID iid, void **out) {
  if (!out) return E_POINTER;
  *out = NULL;
  if (!same_guid(iid, &kIidUnknown) && !same_guid(iid, &kIidDispatch) && !same_guid(iid, &kIidParseError)) return E_NOINTERFACE;
  InterlockedIncrement(&((ParseError *)This)->references);
  *out = This;
  return S_OK;
}
static ULONG WINAPI error_AddRef(IXMLDOMParseError *This) { return (ULONG)InterlockedIncrement(&((ParseError *)This)->references); }
static ULONG WINAPI error_Release(IXMLDOMParseError *This) {
  ParseError *e = (ParseError *)This;
  const LONG left = InterlockedDecrement(&e->references);
  if (left == 0) {
    SysFreeString(e->reason);
    SysFreeString(e->source);
    SysFreeString(e->url);
    release_memory(e);
    InterlockedDecrement(&g_objects);
  }
  return (ULONG)left;
}
static HRESULT give_copy(BSTR text, BSTR *out) {
  if (!out) return E_INVALIDARG;
  *out = NULL;
  if (!text) return S_FALSE;
  return give_text(text, SysStringLen(text), out);
}
static HRESULT give_number(LONG value, LONG *out) {
  if (!out) return E_INVALIDARG;
  *out = value;
  return value ? S_OK : S_FALSE;
}
static HRESULT WINAPI error_get_errorCode(IXMLDOMParseError *This, LONG *out) { return give_number((LONG)((ParseError *)This)->code, out); }
static HRESULT WINAPI error_get_url(IXMLDOMParseError *This, BSTR *out) { return give_copy(((ParseError *)This)->url, out); }
static HRESULT WINAPI error_get_reason(IXMLDOMParseError *This, BSTR *out) { return give_copy(((ParseError *)This)->reason, out); }
static HRESULT WINAPI error_get_srcText(IXMLDOMParseError *This, BSTR *out) { return give_copy(((ParseError *)This)->source, out); }
static HRESULT WINAPI error_get_line(IXMLDOMParseError *This, LONG *out) { return give_number(((ParseError *)This)->line, out); }
static HRESULT WINAPI error_get_linepos(IXMLDOMParseError *This, LONG *out) { return give_number(((ParseError *)This)->column, out); }
static HRESULT WINAPI error_get_filepos(IXMLDOMParseError *This, LONG *out) { return give_number(((ParseError *)This)->position, out); }
static const IXMLDOMParseErrorVtbl error_vtbl = {
  .QueryInterface = error_QueryInterface, .AddRef = error_AddRef, .Release = error_Release, DISPATCH_METHODS,
  .get_errorCode = error_get_errorCode, .get_url = error_get_url, .get_reason = error_get_reason, .get_srcText = error_get_srcText,
  .get_line = error_get_line, .get_linepos = error_get_linepos, .get_filepos = error_get_filepos,
};

/* ---- loading ------------------------------------------------------------------------------------------------------------------------------------------ */

static void set_error(Document *d, DWORD code, const WCHAR *reason) {
  d->error_code = code;
  d->error_reason = reason;
  d->error_line = d->error_column = d->error_position = 0;
  d->error_source_length = 0;
}

/* Makes s[0, n) the document. S_OK, or S_FALSE with the document empty and its parse error set. */
static HRESULT load_text(Document *d, const WCHAR *s, SIZE_T n) {
  Tree *empty = new_tree(d, FALSE), *tree = new_tree(d, d->preserve != VARIANT_FALSE);
  Parser parser;
  if (!empty || !tree) {
    if (empty) tree_free(empty);
    if (tree) tree_free(tree);
    return E_OUTOFMEMORY;
  }
  zero(&parser, sizeof(parser));
  parser.tree = tree;
  parser.s = s;
  parser.n = n;
  Tree *old = d->tree;
  if (parse_document(&parser)) {
    tree_free(empty);
    set_error(d, 0, NULL);
    tree->references = 1;
    d->tree = tree;
    tree_release(old);
    return S_OK;
  }
  tree_free(tree);
  set_error(d, parser.code, parser.reason);
  /* (where: the line, the column in it, both from 1, and the start of that line) */
  SIZE_T line = 1, line_at = 0;
  for (SIZE_T i = 0; i < parser.error_at; ++i) if (s[i] == '\n') { ++line; line_at = i + 1; }
  d->error_line = (LONG)line;
  d->error_column = (LONG)(parser.error_at - line_at + 1);
  d->error_position = (LONG)parser.error_at;
  while (line_at + d->error_source_length < n && d->error_source_length < sizeof(d->error_source) / sizeof(WCHAR)) {
    const WCHAR c = s[line_at + d->error_source_length];
    if (c == '\n' || c == '\r' || c == 0) break;
    d->error_source[d->error_source_length++] = c;
  }
  empty->references = 1;
  d->tree = empty;
  tree_release(old);
  return S_FALSE;
}
/* Leaves the document empty after a load that did not get as far as the parser. */
static HRESULT load_failed(Document *d, DWORD code, const WCHAR *reason) {
  Tree *empty = new_tree(d, FALSE);
  if (!empty) return E_OUTOFMEMORY;
  Tree *old = d->tree;
  empty->references = 1;
  d->tree = empty;
  tree_release(old);
  set_error(d, code, reason);
  return S_FALSE;
}

/* The encoding an XML declaration at the start of the bytes names, in lower case; "" if there is none. */
static void declared_encoding(const BYTE *b, SIZE_T n, char name[32]) {
  static const char key[] = "encoding";
  name[0] = 0;
  if (n < 6 || b[0] != '<' || b[1] != '?' || b[2] != 'x' || b[3] != 'm' || b[4] != 'l') return;
  if (n > 256) n = 256;
  for (SIZE_T i = 5; i + 8 < n && !(b[i] == '?' && b[i + 1] == '>'); ++i) {
    SIZE_T k = 0;
    while (k < 8 && b[i + k] == (BYTE)key[k]) ++k;
    if (k < 8) continue;
    i += 8;
    while (i < n && (b[i] == ' ' || b[i] == '\t' || b[i] == '\r' || b[i] == '\n')) ++i;
    if (i >= n || b[i] != '=') return;
    ++i;
    while (i < n && (b[i] == ' ' || b[i] == '\t' || b[i] == '\r' || b[i] == '\n')) ++i;
    if (i >= n || (b[i] != '"' && b[i] != '\'')) return;
    const BYTE quote = b[i++];
    SIZE_T length = 0;
    for (; i < n && b[i] != quote && length < 31; ++i) name[length++] = (char)(b[i] >= 'A' && b[i] <= 'Z' ? b[i] + 32 : b[i]);
    name[length] = 0;
    return;
  }
}
static BOOL same_name(const char *a, const char *b) {
  while (*a && *a == *b) { ++a; ++b; }
  return *a == *b;
}
/* Strict UTF-8 (no overlong forms, no surrogates, nothing above U+10FFFF) to UTF-16; `out` has room for n units. */
static BOOL utf8_to_utf16(const BYTE *b, SIZE_T n, WCHAR *out, SIZE_T *out_length) {
  SIZE_T o = 0;
  for (SIZE_T i = 0; i < n;) {
    const BYTE c = b[i];
    DWORD value;
    SIZE_T more;
    if (c < 0x80) { out[o++] = c; ++i; continue; }
    if (c >= 0xC2 && c <= 0xDF) { value = c & 0x1F; more = 1; }
    else if (c >= 0xE0 && c <= 0xEF) { value = c & 0x0F; more = 2; }
    else if (c >= 0xF0 && c <= 0xF4) { value = c & 0x07; more = 3; }
    else return FALSE;
    if (n - i <= more) return FALSE;
    for (SIZE_T k = 1; k <= more; ++k) {
      if ((b[i + k] & 0xC0) != 0x80) return FALSE;
      value = (value << 6) | (b[i + k] & 0x3F);
    }
    if ((more == 2 && (value < 0x800 || (value >= 0xD800 && value <= 0xDFFF))) || (more == 3 && (value < 0x10000 || value > 0x10FFFF))) return FALSE;
    if (value >= 0x10000) {
      value -= 0x10000;
      out[o++] = (WCHAR)(0xD800 + (value >> 10));
      out[o++] = (WCHAR)(0xDC00 + (value & 0x3FF));
    } else out[o++] = (WCHAR)value;
    i += more + 1;
  }
  *out_length = o;
  return TRUE;
}

/* The bytes of a file as the document: UTF-16 or UTF-8 by its byte order mark, else what its XML declaration says (UTF-8 without one). */
static HRESULT load_bytes(Document *d, const BYTE *b, SIZE_T n) {
  WCHAR *text = (WCHAR *)HeapAlloc(GetProcessHeap(), 0, (n + 1) * sizeof(WCHAR));
  SIZE_T length = 0;
  BOOL good = TRUE;
  const WCHAR *reason = L"Unsupported encoding.";
  if (!text) return E_OUTOFMEMORY;
  const BOOL little = n >= 2 && ((b[0] == 0xFF && b[1] == 0xFE) || (b[0] == '<' && b[1] == 0));
  const BOOL big = n >= 2 && ((b[0] == 0xFE && b[1] == 0xFF) || (b[0] == 0 && b[1] == '<'));
  if (little || big) {
    if (n & 1) { good = FALSE; reason = L"The document ends inside a character."; }
    for (SIZE_T i = 0; good && i + 1 < n; i += 2) text[length++] = little ? (WCHAR)(b[i] | (b[i + 1] << 8)) : (WCHAR)(b[i + 1] | (b[i] << 8));
  } else {
    char encoding[32];
    const SIZE_T skip = n >= 3 && b[0] == 0xEF && b[1] == 0xBB && b[2] == 0xBF ? 3 : 0;
    declared_encoding(b + skip, n - skip, encoding);
    if (!encoding[0] || same_name(encoding, "utf-8") || same_name(encoding, "utf8") || same_name(encoding, "us-ascii") || same_name(encoding, "ascii")) {
      good = utf8_to_utf16(b + skip, n - skip, text, &length);
      if (!good) reason = L"The document is not valid UTF-8.";
    } else if (!skip && (same_name(encoding, "iso-8859-1") || same_name(encoding, "latin1"))) {
      for (SIZE_T i = 0; i < n; ++i) text[length++] = b[i];
    } else good = FALSE;
  }
  const HRESULT hr = good ? load_text(d, text, length) : load_failed(d, XML_E_INVALIDENCODING, reason);
  HeapFree(GetProcessHeap(), 0, text);
  return hr;
}

static BOOL starts_with(const WCHAR *text, SIZE_T length, const char *prefix) {
  SIZE_T i = 0;
  for (; prefix[i]; ++i) if (i >= length || (text[i] | (text[i] >= 'A' && text[i] <= 'Z' ? 0x20 : 0)) != (WCHAR)(BYTE)prefix[i]) return FALSE;
  return TRUE;
}
/* load(): a local file, by its path or a file: URL. Nothing is ever fetched from a network. */
static HRESULT load_file(Document *d, const WCHAR *name, SIZE_T length) {
  if (length == 0 || length >= 32000) return load_failed(d, (DWORD)E_INVALIDARG, L"Invalid file name.");
  WCHAR *path = (WCHAR *)HeapAlloc(GetProcessHeap(), 0, (length + 1) * sizeof(WCHAR));
  if (!path) return E_OUTOFMEMORY;
  SIZE_T skip = 0;
  if (starts_with(name, length, "file:///")) skip = 8;
  else if (starts_with(name, length, "file://")) skip = 7;
  else if (starts_with(name, length, "http:") || starts_with(name, length, "https:") || starts_with(name, length, "ftp:")) {
    HeapFree(GetProcessHeap(), 0, path);
    return load_failed(d, 0x800C0005 /* INET_E_RESOURCE_NOT_FOUND */, L"The system cannot locate the resource specified.");
  }
  for (SIZE_T i = skip; i < length; ++i) path[i - skip] = skip && name[i] == '/' ? '\\' : name[i];
  path[length - skip] = 0;
  release_memory(d->url);
  d->url = path;
  HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
  if (file == INVALID_HANDLE_VALUE) return load_failed(d, 0x800C0006 /* INET_E_OBJECT_NOT_FOUND */, L"The system cannot locate the object specified.");
  DWORD high = 0;
  const DWORD size = GetFileSize(file, &high);
  if (high || size > MAX_INPUT) { CloseHandle(file); return load_failed(d, XML_E_TOOLARGE, L"The document is too large."); }
  BYTE *bytes = (BYTE *)HeapAlloc(GetProcessHeap(), 0, (SIZE_T)size + 1);
  if (!bytes) { CloseHandle(file); return E_OUTOFMEMORY; }
  DWORD total = 0;
  BOOL good = TRUE;
  while (total < size) {
    DWORD got = 0;
    if (!ReadFile(file, bytes + total, size - total, &got, NULL)) { good = FALSE; break; }
    if (got == 0) break;      /* (the file became shorter) */
    total += got;
  }
  CloseHandle(file);
  const HRESULT hr = good ? load_bytes(d, bytes, total) : load_failed(d, (DWORD)HRESULT_FROM_WIN32(ERROR_READ_FAULT), L"The file could not be read.");
  HeapFree(GetProcessHeap(), 0, bytes);
  return hr;
}

/* ---- IXMLDOMDocument3 ---------------------------------------------------------------------------------------------------------------------------------- */

static HRESULT WINAPI document_QueryInterface(IXMLDOMDocument3 *This, REFIID iid, void **out) {
  if (!out) return E_POINTER;
  *out = NULL;
  if (!same_guid(iid, &kIidUnknown) && !same_guid(iid, &kIidDispatch) && !same_guid(iid, &kIidNode) && !same_guid(iid, &kIidDocument) && !same_guid(iid, &kIidDocument2) &&
      !same_guid(iid, &kIidDocument3)) return E_NOINTERFACE;
  document_addref((Document *)This);
  *out = This;
  return S_OK;
}
static ULONG WINAPI document_AddRef(IXMLDOMDocument3 *This) { return document_addref((Document *)This); }
static ULONG WINAPI document_Release(IXMLDOMDocument3 *This) { return document_release((Document *)This); }

static HRESULT WINAPI document_get_doctype(IXMLDOMDocument3 *This, IXMLDOMDocumentType **out) { return give(NULL, out); }      /* (a DOCTYPE is skipped) */
static HRESULT WINAPI document_get_implementation(IXMLDOMDocument3 *This, IXMLDOMImplementation **out) { if (out) *out = NULL; return E_NOTIMPL; }
static HRESULT WINAPI document_get_documentElement(IXMLDOMDocument3 *This, IXMLDOMElement **out) {
  Node *n = ((Document *)This)->tree->root->first;
  while (n && n->type != NODE_ELEMENT) n = n->next;
  return give(n, out);
}
static HRESULT WINAPI document_putref_documentElement(IXMLDOMDocument3 *This, IXMLDOMElement *element) { return E_NOTIMPL; }
static HRESULT WINAPI document_createElement(IXMLDOMDocument3 *This, BSTR name, IXMLDOMElement **out) { if (out) *out = NULL; return E_NOTIMPL; }
static HRESULT WINAPI document_createDocumentFragment(IXMLDOMDocument3 *This, IXMLDOMDocumentFragment **out) { if (out) *out = NULL; return E_NOTIMPL; }
static HRESULT WINAPI document_createTextNode(IXMLDOMDocument3 *This, BSTR data, IXMLDOMText **out) { if (out) *out = NULL; return E_NOTIMPL; }
static HRESULT WINAPI document_createComment(IXMLDOMDocument3 *This, BSTR data, IXMLDOMComment **out) { if (out) *out = NULL; return E_NOTIMPL; }
static HRESULT WINAPI document_createCDATASection(IXMLDOMDocument3 *This, BSTR data, IXMLDOMCDATASection **out) { if (out) *out = NULL; return E_NOTIMPL; }
static HRESULT WINAPI document_createProcessingInstruction(IXMLDOMDocument3 *This, BSTR target, BSTR data, IXMLDOMProcessingInstruction **out) { if (out) *out = NULL; return E_NOTIMPL; }
static HRESULT WINAPI document_createAttribute(IXMLDOMDocument3 *This, BSTR name, IXMLDOMAttribute **out) { if (out) *out = NULL; return E_NOTIMPL; }
static HRESULT WINAPI document_createEntityReference(IXMLDOMDocument3 *This, BSTR name, IXMLDOMEntityReference **out) { if (out) *out = NULL; return E_NOTIMPL; }
static HRESULT WINAPI document_getElementsByTagName(IXMLDOMDocument3 *This, BSTR name, IXMLDOMNodeList **out) { return elements_by_name(((Document *)This)->tree->root, name, out); }
static HRESULT WINAPI document_createNode(IXMLDOMDocument3 *This, VARIANT type, BSTR name, BSTR uri, IXMLDOMNode **out) { if (out) *out = NULL; return E_NOTIMPL; }
static HRESULT WINAPI document_nodeFromID(IXMLDOMDocument3 *This, BSTR id, IXMLDOMNode **out) { if (out) *out = NULL; return E_NOTIMPL; }
static HRESULT WINAPI document_load(IXMLDOMDocument3 *This, VARIANT source, VARIANT_BOOL *ok) {
  Document *d = (Document *)This;
  BSTR name = NULL;
  if (ok) *ok = VARIANT_FALSE;
  if (V_VT(&source) == VT_BSTR) name = V_BSTR(&source);
  else if (V_VT(&source) == (VT_BSTR | VT_BYREF) && V_BSTRREF(&source)) name = *V_BSTRREF(&source);
  else return E_INVALIDARG;      /* (not done: a stream, an array of bytes, another document) */
  const HRESULT hr = load_file(d, name, SysStringLen(name));
  if (hr == S_OK && ok) *ok = VARIANT_TRUE;
  return hr;
}
static HRESULT WINAPI document_get_readyState(IXMLDOMDocument3 *This, LONG *out) {
  if (!out) return E_INVALIDARG;
  *out = 4;      /* (READYSTATE_COMPLETE: loading is never asynchronous) */
  return S_OK;
}
static HRESULT WINAPI document_get_parseError(IXMLDOMDocument3 *This, IXMLDOMParseError **out) {
  const Document *d = (Document *)This;
  if (!out) return E_INVALIDARG;
  *out = NULL;
  ParseError *e = (ParseError *)alloc(sizeof(ParseError));
  if (!e) return E_OUTOFMEMORY;
  e->vtbl = &error_vtbl;
  e->references = 1;
  e->code = d->error_code;
  e->line = d->error_line;
  e->column = d->error_column;
  e->position = d->error_position;
  if (d->error_code) {
    Buffer reason;
    zero(&reason, sizeof(reason));
    if (d->error_reason) { SIZE_T n = 0; while (d->error_reason[n]) ++n; buffer_add(&reason, d->error_reason, n); }
    buffer_add_ascii(&reason, "\r\n");
    buffer_give(&reason, &e->reason);
    if (d->error_source_length) e->source = SysAllocStringLen(d->error_source, d->error_source_length);
    if (d->url) e->url = SysAllocString(d->url);
  }
  InterlockedIncrement(&g_objects);
  *out = (IXMLDOMParseError *)e;
  return S_OK;
}
static HRESULT WINAPI document_get_url(IXMLDOMDocument3 *This, BSTR *out) {
  const Document *d = (Document *)This;
  if (!d->url) return give_no_text(out);
  SIZE_T n = 0;
  while (d->url[n]) ++n;
  return give_text(d->url, n, out);
}
static HRESULT WINAPI document_get_async(IXMLDOMDocument3 *This, VARIANT_BOOL *out) { return give_bool(((Document *)This)->async, out); }
static HRESULT WINAPI document_put_async(IXMLDOMDocument3 *This, VARIANT_BOOL value) { ((Document *)This)->async = value; return S_OK; }
static HRESULT WINAPI document_abort(IXMLDOMDocument3 *This) { return S_OK; }
static HRESULT WINAPI document_loadXML(IXMLDOMDocument3 *This, BSTR text, VARIANT_BOOL *ok) {
  Document *d = (Document *)This;
  if (ok) *ok = VARIANT_FALSE;
  release_memory(d->url);
  d->url = NULL;
  const HRESULT hr = load_text(d, text, SysStringLen(text));
  if (hr == S_OK && ok) *ok = VARIANT_TRUE;
  return hr;
}
static HRESULT WINAPI document_save(IXMLDOMDocument3 *This, VARIANT destination) { return E_NOTIMPL; }
static HRESULT WINAPI document_get_validateOnParse(IXMLDOMDocument3 *This, VARIANT_BOOL *out) { return give_bool(((Document *)This)->validate, out); }
static HRESULT WINAPI document_put_validateOnParse(IXMLDOMDocument3 *This, VARIANT_BOOL value) { ((Document *)This)->validate = value; return S_OK; }
static HRESULT WINAPI document_get_resolveExternals(IXMLDOMDocument3 *This, VARIANT_BOOL *out) { return give_bool(((Document *)This)->resolve, out); }
static HRESULT WINAPI document_put_resolveExternals(IXMLDOMDocument3 *This, VARIANT_BOOL value) { ((Document *)This)->resolve = value; return S_OK; }
static HRESULT WINAPI document_get_preserveWhiteSpace(IXMLDOMDocument3 *This, VARIANT_BOOL *out) { return give_bool(((Document *)This)->preserve, out); }
static HRESULT WINAPI document_put_preserveWhiteSpace(IXMLDOMDocument3 *This, VARIANT_BOOL value) { ((Document *)This)->preserve = value; return S_OK; }
/* (the event handlers are taken and never called: nothing happens asynchronously) */
static HRESULT WINAPI document_put_onreadystatechange(IXMLDOMDocument3 *This, VARIANT handler) { return S_OK; }
static HRESULT WINAPI document_put_ondataavailable(IXMLDOMDocument3 *This, VARIANT handler) { return S_OK; }
static HRESULT WINAPI document_put_ontransformnode(IXMLDOMDocument3 *This, VARIANT handler) { return S_OK; }
static HRESULT WINAPI document_get_namespaces(IXMLDOMDocument3 *This, IXMLDOMSchemaCollection **out) { if (out) *out = NULL; return E_NOTIMPL; }
static HRESULT WINAPI document_get_schemas(IXMLDOMDocument3 *This, VARIANT *out) { return give_variant_text(NULL, 0, out); }
static HRESULT WINAPI document_putref_schemas(IXMLDOMDocument3 *This, VARIANT schemas) { return E_NOTIMPL; }
static HRESULT WINAPI document_validate(IXMLDOMDocument3 *This, IXMLDOMParseError **out) { if (out) *out = NULL; return E_NOTIMPL; }
/* (the properties - SelectionLanguage, SelectionNamespaces, ProhibitDTD, NewParser ... - are accepted; none changes what this library does) */
static HRESULT WINAPI document_setProperty(IXMLDOMDocument3 *This, BSTR name, VARIANT value) { return S_OK; }
static HRESULT WINAPI document_getProperty(IXMLDOMDocument3 *This, BSTR name, VARIANT *out) {
  if (!out) return E_INVALIDARG;
  V_VT(out) = VT_EMPTY;
  if (same_ascii(name, SysStringLen(name), "SelectionLanguage")) return give_variant_text(L"XPath", 5, out);
  return E_FAIL;
}
static HRESULT WINAPI document_validateNode(IXMLDOMDocument3 *This, IXMLDOMNode *node, IXMLDOMParseError **out) { if (out) *out = NULL; return E_NOTIMPL; }
static HRESULT WINAPI document_importNode(IXMLDOMDocument3 *This, IXMLDOMNode *node, VARIANT_BOOL deep, IXMLDOMNode **out) { if (out) *out = NULL; return E_NOTIMPL; }

static const IXMLDOMDocument3Vtbl document_vtbl = {
  .QueryInterface = document_QueryInterface, .AddRef = document_AddRef, .Release = document_Release, NODE_METHODS,
  .get_doctype = document_get_doctype, .get_implementation = document_get_implementation, .get_documentElement = document_get_documentElement,
  .putref_documentElement = document_putref_documentElement, .createElement = document_createElement, .createDocumentFragment = document_createDocumentFragment,
  .createTextNode = document_createTextNode, .createComment = document_createComment, .createCDATASection = document_createCDATASection,
  .createProcessingInstruction = document_createProcessingInstruction, .createAttribute = document_createAttribute, .createEntityReference = document_createEntityReference,
  .getElementsByTagName = document_getElementsByTagName, .createNode = document_createNode, .nodeFromID = document_nodeFromID, .load = document_load,
  .get_readyState = document_get_readyState, .get_parseError = document_get_parseError, .get_url = document_get_url, .get_async = document_get_async,
  .put_async = document_put_async, .abort = document_abort, .loadXML = document_loadXML, .save = document_save, .get_validateOnParse = document_get_validateOnParse,
  .put_validateOnParse = document_put_validateOnParse, .get_resolveExternals = document_get_resolveExternals, .put_resolveExternals = document_put_resolveExternals,
  .get_preserveWhiteSpace = document_get_preserveWhiteSpace, .put_preserveWhiteSpace = document_put_preserveWhiteSpace,
  .put_onreadystatechange = document_put_onreadystatechange, .put_ondataavailable = document_put_ondataavailable, .put_ontransformnode = document_put_ontransformnode,
  .get_namespaces = document_get_namespaces, .get_schemas = document_get_schemas, .putref_schemas = document_putref_schemas, .validate = document_validate,
  .setProperty = document_setProperty, .getProperty = document_getProperty, .validateNode = document_validateNode, .importNode = document_importNode,
};

static HRESULT new_document(REFIID iid, void **out) {
  Document *d = (Document *)alloc(sizeof(Document));
  if (!d) return E_OUTOFMEMORY;
  d->vtbl = &document_vtbl;
  d->references = 1;
  d->async = VARIANT_TRUE;
  d->validate = VARIANT_TRUE;
  d->resolve = VARIANT_FALSE;
  d->preserve = VARIANT_FALSE;
  d->tree = new_tree(d, FALSE);
  if (!d->tree) { release_memory(d); return E_OUTOFMEMORY; }
  d->tree->references = 1;
  InterlockedIncrement(&g_objects);
  const HRESULT hr = document_QueryInterface((IXMLDOMDocument3 *)d, iid, out);
  document_release(d);
  return hr;
}

/* ---- the class factory, the exports -------------------------------------------------------------------------------------------------------------------- */

static HRESULT WINAPI factory_QueryInterface(IClassFactory *This, REFIID iid, void **out) {
  if (!out) return E_POINTER;
  *out = NULL;
  if (!same_guid(iid, &kIidUnknown) && !same_guid(iid, &kIidClassFactory)) return E_NOINTERFACE;
  *out = This;
  return S_OK;
}
static ULONG WINAPI factory_AddRef(IClassFactory *This) { return 2; }      /* (one object, never freed) */
static ULONG WINAPI factory_Release(IClassFactory *This) { return 1; }
static HRESULT WINAPI factory_CreateInstance(IClassFactory *This, IUnknown *outer, REFIID iid, void **out) {
  if (!out) return E_POINTER;
  *out = NULL;
  if (outer) return CLASS_E_NOAGGREGATION;
  if (!iid) return E_INVALIDARG;
  return new_document(iid, out);
}
static HRESULT WINAPI factory_LockServer(IClassFactory *This, BOOL lock) {
  if (lock) InterlockedIncrement(&g_objects); else InterlockedDecrement(&g_objects);
  return S_OK;
}
static const IClassFactoryVtbl factory_vtbl = { factory_QueryInterface, factory_AddRef, factory_Release, factory_CreateInstance, factory_LockServer };
static IClassFactory g_factory = { (IClassFactoryVtbl *)&factory_vtbl };

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved) {
  (void)instance;
  (void)reason;
  (void)reserved;
  return TRUE;
}

EXPORT HRESULT WINAPI DllGetClassObject(REFCLSID clsid, REFIID iid, LPVOID *out) {
  if (!out) return E_POINTER;
  *out = NULL;
  if (!clsid || !iid) return E_INVALIDARG;
  for (SIZE_T i = 0; i < sizeof(kClasses) / sizeof(kClasses[0]); ++i)
    if (same_guid(clsid, &kClasses[i])) return factory_QueryInterface(&g_factory, iid, out);
  return CLASS_E_CLASSNOTAVAILABLE;
}
EXPORT HRESULT WINAPI DllCanUnloadNow(void) { return g_objects == 0 ? S_OK : S_FALSE; }
/* (ole32 knows the classes without registry entries: src/Windows/ole32/compobj.inc) */
EXPORT HRESULT WINAPI DllRegisterServer(void) { return S_OK; }
EXPORT HRESULT WINAPI DllUnregisterServer(void) { return S_OK; }
