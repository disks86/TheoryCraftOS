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

/* msxml3.dll: the tree of a document and the parser that builds it (included by msxml3.c).
 *
 * A strict, non-validating XML 1.0 parser with namespaces. The input is UTF-16 that the caller does not trust: every index is checked against the
 * length, the parser is a loop (the open elements are the tree itself, bounded by MAX_DEPTH), and what it builds is allocated from the tree's own
 * arena, so that a failed parse - or the last reference to a tree - frees everything at once. A DOCTYPE is skipped: it defines no entities here and
 * nothing external is ever read, so only the five predefined entities and character references exist. A tree does not change once it is built. */

#define MAX_DEPTH 1024            /* open elements */
#define MAX_ATTRIBUTES 8192       /* of one element (duplicates are found by comparing each with each) */
#define MAX_INPUT 0x10000000u     /* UTF-16 units of one document */
#define CHUNK_BYTES 0x10000

typedef struct Chunk { struct Chunk *next; SIZE_T used, size; SIZE_T padding; } Chunk;      /* (its bytes follow) */

struct Document;
typedef struct Node {
  const void *vtbl;                 /* the COM interface of its kind: a node is its own COM object (msxml3.c) */
  struct Tree *tree;
  int type;                         /* DOMNodeType */
  struct Node *parent;              /* (an attribute's: its element) */
  struct Node *first, *last, *previous, *next;      /* children; siblings (attributes are chained through `next` from the element's `attributes`) */
  struct Node *attributes, *last_attribute;
  struct Node *scope;               /* an element: the nearest element, itself or an ancestor, with namespace declarations */
  const WCHAR *name;                /* qualified name (an element, an attribute), target (a processing instruction); NUL-terminated like all strings here */
  const WCHAR *value;               /* an attribute's value, the characters of text, CDATA, a comment, a processing instruction's data */
  const WCHAR *uri;                 /* namespace, NULL if none */
  UINT name_length, prefix_length, value_length, uri_length;      /* (prefix_length 0: no prefix; else the local name starts one after it) */
  UINT order, last_order;           /* position in document order; the last position inside it (itself, for a leaf) */
  UINT attribute_count;
  BOOL declares;                    /* an element with xmlns attributes */
} Node;

typedef struct Tree {
  LONG references;                  /* its document, while it is the document's tree, and each reference to one of its nodes */
  struct Document *document;
  Chunk *chunks;
  Node *root;                       /* the document node; never handed out (the document object stands for it) */
  UINT next_order;
  BOOL preserve;                    /* built with preserveWhiteSpace */
} Tree;

static const void *vtbl_of_type(int type);      /* (msxml3.c) */

static void *arena_alloc(Tree *t, SIZE_T n) {
  Chunk *c = t->chunks;
  if (n > 0x40000000u) return NULL;
  n = (n + 7) & ~(SIZE_T)7;
  if (!c || n > c->size - c->used) {
    SIZE_T size = n > CHUNK_BYTES ? n : CHUNK_BYTES;
    c = (Chunk *)HeapAlloc(GetProcessHeap(), 0, sizeof(Chunk) + size);
    if (!c) return NULL;
    c->next = t->chunks;
    c->used = 0;
    c->size = size;
    t->chunks = c;
  }
  void *p = (BYTE *)(c + 1) + c->used;
  c->used += n;
  return p;
}

static void tree_free(Tree *t) {
  Chunk *c = t->chunks;
  while (c) { Chunk *next = c->next; HeapFree(GetProcessHeap(), 0, c); c = next; }
  HeapFree(GetProcessHeap(), 0, t);
}

static Node *new_node(Tree *t, int type) {
  Node *n = (Node *)arena_alloc(t, sizeof(Node));
  if (!n) return NULL;
  zero(n, sizeof(Node));
  n->vtbl = vtbl_of_type(type);
  n->tree = t;
  n->type = type;
  n->order = n->last_order = t->next_order++;
  return n;
}

/* An empty tree: the document node alone. */
static Tree *new_tree(struct Document *document, BOOL preserve) {
  Tree *t = (Tree *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(Tree));
  if (!t) return NULL;
  t->document = document;
  t->preserve = preserve;
  t->root = new_node(t, NODE_DOCUMENT);
  if (!t->root) { tree_free(t); return NULL; }
  return t;
}

static void append_child(Node *parent, Node *child) {
  child->parent = parent;
  child->previous = parent->last;
  if (parent->last) parent->last->next = child; else parent->first = child;
  parent->last = child;
}

/* ---- the parser --------------------------------------------------------------------------------------------------------------------------------- */

/* MSXML's numbers for what went wrong (IXMLDOMParseError::errorCode). */
#define XML_E_MISSINGEQUALS 0xC00CE501
#define XML_E_MISSINGQUOTE 0xC00CE502
#define XML_E_COMMENTSYNTAX 0xC00CE503
#define XML_E_BADSTARTNAMECHAR 0xC00CE504
#define XML_E_BADNAMECHAR 0xC00CE505
#define XML_E_BADCHARINSTRING 0xC00CE506
#define XML_E_XMLDECLSYNTAX 0xC00CE507
#define XML_E_BADCHARDATA 0xC00CE508
#define XML_E_MISSINGWHITESPACE 0xC00CE509
#define XML_E_EXPECTINGTAGEND 0xC00CE50A
#define XML_E_MISSINGSEMICOLON 0xC00CE50D
#define XML_E_MULTIPLE_COLONS 0xC00CE51C
#define XML_E_INVALID_DECIMAL 0xC00CE51D
#define XML_E_INVALID_HEXIDECIMAL 0xC00CE51E
#define XML_E_INVALID_UNICODE 0xC00CE51F
#define XML_E_UNEXPECTEDENDTAG 0xC00CE552
#define XML_E_UNCLOSEDTAG 0xC00CE553
#define XML_E_DUPLICATEATTRIBUTE 0xC00CE554
#define XML_E_MULTIPLEROOTS 0xC00CE555
#define XML_E_INVALIDATROOTLEVEL 0xC00CE556
#define XML_E_BADXMLDECL 0xC00CE557
#define XML_E_MISSINGROOT 0xC00CE558
#define XML_E_UNEXPECTEDEOF 0xC00CE559
#define XML_E_INVALID_CDATACLOSINGTAG 0xC00CE55C
#define XML_E_UNCLOSEDPI 0xC00CE55D
#define XML_E_UNCLOSEDSTARTTAG 0xC00CE55E
#define XML_E_UNCLOSEDENDTAG 0xC00CE55F
#define XML_E_UNCLOSEDSTRING 0xC00CE560
#define XML_E_UNCLOSEDCOMMENT 0xC00CE561
#define XML_E_UNCLOSEDDECL 0xC00CE562
#define XML_E_UNCLOSEDCDATA 0xC00CE564
#define XML_E_BADDECLNAME 0xC00CE565
#define XML_E_RESERVEDNAMESPACE 0xC00CE568
#define XML_E_ENDTAGMISMATCH 0xC00CE56D
#define XML_E_INVALIDENCODING 0xC00CE56E
#define XML_E_MISSING_ENTITY 0xC00CE57C
#define XML_E_INVALID_VERSION 0xC00CE57F
#define XML_E_UNDECLAREDPREFIX 0xC00CE01D
#define XML_E_TOODEEP 0xC00CE5FE          /* (ours: the limits above) */
#define XML_E_TOOLARGE 0xC00CE5FF

typedef struct Parser {
  Tree *tree;
  const WCHAR *s;
  SIZE_T n, p;                      /* length, position */
  Node *open;                       /* the innermost open element, or the document node */
  UINT depth;
  BOOL seen_root, seen_doctype;
  DWORD code;                       /* 0, or why it failed */
  const WCHAR *reason;
  SIZE_T error_at;
} Parser;

static BOOL fail(Parser *x, DWORD code, const WCHAR *reason, SIZE_T at) {
  if (!x->code) { x->code = code; x->reason = reason; x->error_at = at > x->n ? x->n : at; }
  return FALSE;
}
static BOOL fail_memory(Parser *x) { return fail(x, (DWORD)E_OUTOFMEMORY, L"Out of memory.", x->p); }

static BOOL is_space(WCHAR c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }
static BOOL is_name_start(WCHAR c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == ':' || (c >= 0xC0 && c != 0xD7 && c != 0xF7); }
static BOOL is_name_char(WCHAR c) { return is_name_start(c) || (c >= '0' && c <= '9') || c == '-' || c == '.' || c == 0xB7; }
static BOOL is_xml_char(DWORD c) { return c == 9 || c == 10 || c == 13 || (c >= 0x20 && c <= 0xD7FF) || (c >= 0xE000 && c <= 0xFFFD) || (c >= 0x10000 && c <= 0x10FFFF); }

/* Is the ASCII text `literal` at the position? */
static BOOL at(const Parser *x, const char *literal) {
  SIZE_T i = 0;
  for (; literal[i]; ++i) if (i >= x->n - x->p || x->s[x->p + i] != (WCHAR)(BYTE)literal[i]) return FALSE;
  return TRUE;
}
static BOOL skip_space(Parser *x) {
  SIZE_T from = x->p;
  while (x->p < x->n && is_space(x->s[x->p])) ++x->p;
  return x->p != from;
}
static BOOL same_text(const WCHAR *a, SIZE_T a_length, const WCHAR *b, SIZE_T b_length) {
  if (a_length != b_length) return FALSE;
  for (SIZE_T i = 0; i < a_length; ++i) if (a[i] != b[i]) return FALSE;
  return TRUE;
}
static BOOL same_ascii(const WCHAR *a, SIZE_T a_length, const char *b) {
  SIZE_T i = 0;
  for (; b[i]; ++i) if (i >= a_length || a[i] != (WCHAR)(BYTE)b[i]) return FALSE;
  return i == a_length;
}

/* A name at the position: where it starts, its length and the length of its prefix (0: none). At most one colon, and not at either end. */
static BOOL parse_name(Parser *x, SIZE_T *start, UINT *length, UINT *prefix_length) {
  SIZE_T b = x->p, colon = 0;
  BOOL has_colon = FALSE;
  if (b >= x->n) return fail(x, XML_E_UNEXPECTEDEOF, L"Unexpected end of input where a name was expected.", b);
  if (!is_name_start(x->s[b]) || x->s[b] == ':') return fail(x, XML_E_BADSTARTNAMECHAR, L"A name was started with an invalid character.", b);
  while (x->p < x->n && is_name_char(x->s[x->p])) {
    if (x->s[x->p] == ':') {
      if (has_colon) return fail(x, XML_E_MULTIPLE_COLONS, L"A name contains more than one colon.", x->p);
      has_colon = TRUE;
      colon = x->p - b;
    }
    ++x->p;
  }
  if (has_colon && (colon + 1 == x->p - b || !is_name_start(x->s[b + colon + 1]))) return fail(x, XML_E_BADNAMECHAR, L"A name contains an invalid character.", b + colon);
  *start = b;
  *length = (UINT)(x->p - b);
  *prefix_length = (UINT)colon;
  return TRUE;
}

static const WCHAR *store(Parser *x, const WCHAR *text, SIZE_T length) {
  WCHAR *copy = (WCHAR *)arena_alloc(x->tree, (length + 1) * sizeof(WCHAR));
  if (!copy) { fail_memory(x); return NULL; }
  for (SIZE_T i = 0; i < length; ++i) copy[i] = text[i];
  copy[length] = 0;
  return copy;
}

enum { DECODE_RAW, DECODE_TEXT, DECODE_ATTRIBUTE };
/* The characters of s[from, to) as the document means them: line ends become a line feed; in text and attribute values the references are replaced
 * (&lt; &#65; ...), and white space in an attribute's value becomes a space. What comes out is never longer than what goes in. */
static BOOL decode(Parser *x, SIZE_T from, SIZE_T to, int mode, const WCHAR **out, UINT *out_length) {
  const WCHAR *s = x->s;
  WCHAR *d = (WCHAR *)arena_alloc(x->tree, (to - from + 1) * sizeof(WCHAR));
  SIZE_T n = 0;
  if (!d) return fail_memory(x);
  for (SIZE_T i = from; i < to;) {
    WCHAR c = s[i];
    if (c == '\r') {
      d[n++] = mode == DECODE_ATTRIBUTE ? ' ' : '\n';
      ++i;
      if (i < to && s[i] == '\n') ++i;
    } else if (mode == DECODE_ATTRIBUTE && (c == '\n' || c == '\t')) {
      d[n++] = ' ';
      ++i;
    } else if (mode == DECODE_ATTRIBUTE && c == '<') {
      return fail(x, XML_E_BADCHARINSTRING, L"A string literal contains '<'.", i);
    } else if (mode == DECODE_TEXT && c == ']' && to - i >= 3 && s[i + 1] == ']' && s[i + 2] == '>') {
      return fail(x, XML_E_INVALID_CDATACLOSINGTAG, L"']]>' is not allowed in text.", i);
    } else if (mode != DECODE_RAW && c == '&') {
      SIZE_T j = i + 1;
      if (j < to && s[j] == '#') {
        DWORD value = 0;
        BOOL hex = FALSE, digits = FALSE;
        ++j;
        if (j < to && s[j] == 'x') { hex = TRUE; ++j; }
        for (; j < to && s[j] != ';'; ++j) {
          WCHAR h = s[j];
          DWORD digit;
          if (h >= '0' && h <= '9') digit = h - '0';
          else if (hex && h >= 'a' && h <= 'f') digit = h - 'a' + 10;
          else if (hex && h >= 'A' && h <= 'F') digit = h - 'A' + 10;
          else return fail(x, hex ? XML_E_INVALID_HEXIDECIMAL : XML_E_INVALID_DECIMAL, L"A character reference contains an invalid digit.", j);
          value = value * (hex ? 16 : 10) + digit;
          if (value > 0x10FFFF) return fail(x, XML_E_INVALID_UNICODE, L"A character reference is not a character of XML.", j);
          digits = TRUE;
        }
        if (j >= to) return fail(x, XML_E_MISSINGSEMICOLON, L"A character reference has no semicolon.", i);
        if (!digits) return fail(x, hex ? XML_E_INVALID_HEXIDECIMAL : XML_E_INVALID_DECIMAL, L"A character reference has no digits.", j);
        if (!is_xml_char(value)) return fail(x, XML_E_INVALID_UNICODE, L"A character reference is not a character of XML.", i);
        if (value >= 0x10000) {
          value -= 0x10000;
          d[n++] = (WCHAR)(0xD800 + (value >> 10));
          d[n++] = (WCHAR)(0xDC00 + (value & 0x3FF));
        } else d[n++] = (WCHAR)value;
        i = j + 1;
      } else {
        while (j < to && s[j] != ';' && j - i <= 8) ++j;
        if (j >= to || s[j] != ';') return fail(x, XML_E_MISSINGSEMICOLON, L"An entity reference has no semicolon.", i);
        const WCHAR *name = s + i + 1;
        SIZE_T length = j - i - 1;
        if (same_ascii(name, length, "lt")) d[n++] = '<';
        else if (same_ascii(name, length, "gt")) d[n++] = '>';
        else if (same_ascii(name, length, "amp")) d[n++] = '&';
        else if (same_ascii(name, length, "apos")) d[n++] = '\'';
        else if (same_ascii(name, length, "quot")) d[n++] = '"';
        else return fail(x, XML_E_MISSING_ENTITY, L"Reference to an undefined entity.", i);
        i = j + 1;
      }
    } else {
      d[n++] = c;
      ++i;
    }
  }
  d[n] = 0;
  *out = d;
  *out_length = (UINT)n;
  return TRUE;
}

/* The position of the ASCII text `literal` at or after the position, or the length of the input if it is not there. */
static SIZE_T find(const Parser *x, const char *literal) {
  SIZE_T length = 0;
  while (literal[length]) ++length;
  for (SIZE_T i = x->p; x->n - i >= length; ++i) {
    SIZE_T k = 0;
    while (k < length && x->s[i + k] == (WCHAR)(BYTE)literal[k]) ++k;
    if (k == length) return i;
  }
  return x->n;
}

static Node *add_node(Parser *x, int type) {
  Node *n = new_node(x->tree, type);
  if (!n) { fail_memory(x); return NULL; }
  append_child(x->open, n);
  return n;
}

/* The xmlns attribute that binds `prefix` (length 0: the default namespace) for the element `e`, or NULL. */
static const Node *find_namespace(const Node *e, const WCHAR *prefix, UINT prefix_length) {
  for (const Node *scope = e->scope; scope; scope = scope->parent && scope->parent->type == NODE_ELEMENT ? scope->parent->scope : NULL) {
    for (const Node *a = scope->attributes; a; a = a->next) {
      if (prefix_length == 0 ? same_ascii(a->name, a->name_length, "xmlns")
                             : a->prefix_length == 5 && same_ascii(a->name, 5, "xmlns") && same_text(a->name + 6, a->name_length - 6, prefix, prefix_length)) return a;
    }
  }
  return NULL;
}

static const WCHAR kXmlNamespace[] = L"http://www.w3.org/XML/1998/namespace";
static const WCHAR kXmlnsNamespace[] = L"http://www.w3.org/2000/xmlns/";

/* The namespace of the element or attribute `n` of the element `e`, from its prefix. */
static BOOL resolve_namespace(Parser *x, Node *e, Node *n, SIZE_T name_at) {
  const BOOL attribute = n->type == NODE_ATTRIBUTE;
  if (attribute && (same_ascii(n->name, n->name_length, "xmlns") || (n->prefix_length == 5 && same_ascii(n->name, 5, "xmlns")))) {
    n->uri = kXmlnsNamespace;
    n->uri_length = sizeof(kXmlnsNamespace) / sizeof(WCHAR) - 1;
    return TRUE;
  }
  if (n->prefix_length == 0 && attribute) return TRUE;      /* (the default namespace is not for attributes) */
  if (n->prefix_length == 3 && same_ascii(n->name, 3, "xml")) {
    n->uri = kXmlNamespace;
    n->uri_length = sizeof(kXmlNamespace) / sizeof(WCHAR) - 1;
    return TRUE;
  }
  if (n->prefix_length == 5 && same_ascii(n->name, 5, "xmlns")) return fail(x, XML_E_RESERVEDNAMESPACE, L"The prefix 'xmlns' is reserved.", name_at);
  const Node *binding = find_namespace(e, n->name, n->prefix_length);
  if (!binding) {
    if (n->prefix_length) return fail(x, XML_E_UNDECLAREDPREFIX, L"Reference to undeclared namespace prefix.", name_at);
    return TRUE;
  }
  if (binding->value_length) { n->uri = binding->value; n->uri_length = binding->value_length; }
  return TRUE;
}

/* "<name attributes> or />", the position after the '<'. */
static BOOL parse_start_tag(Parser *x) {
  const SIZE_T tag_at = x->p;
  SIZE_T start;
  UINT length, prefix_length;
  if (!parse_name(x, &start, &length, &prefix_length)) return FALSE;
  if (x->open->type == NODE_DOCUMENT) {
    if (x->seen_root) return fail(x, XML_E_MULTIPLEROOTS, L"Only one top level element is allowed in an XML document.", tag_at);
    x->seen_root = TRUE;
  }
  if (x->depth >= MAX_DEPTH) return fail(x, XML_E_TOODEEP, L"The elements are nested too deeply.", tag_at);
  Node *e = add_node(x, NODE_ELEMENT);
  if (!e) return FALSE;
  if (!(e->name = store(x, x->s + start, length))) return FALSE;
  e->name_length = length;
  e->prefix_length = prefix_length;
  BOOL open = FALSE;
  for (;;) {
    const BOOL spaced = skip_space(x);
    if (x->p >= x->n) return fail(x, XML_E_UNCLOSEDSTARTTAG, L"A start tag was not closed.", x->p);
    const WCHAR c = x->s[x->p];
    if (c == '>') { ++x->p; open = TRUE; break; }
    if (c == '/') {
      if (x->n - x->p < 2 || x->s[x->p + 1] != '>') return fail(x, XML_E_EXPECTINGTAGEND, L"Expecting '>' after '/'.", x->p);
      x->p += 2;
      break;
    }
    if (!spaced) return fail(x, XML_E_MISSINGWHITESPACE, L"Required white space was missing.", x->p);
    const SIZE_T name_at = x->p;
    if (!parse_name(x, &start, &length, &prefix_length)) return FALSE;
    skip_space(x);
    if (x->p >= x->n || x->s[x->p] != '=') return fail(x, XML_E_MISSINGEQUALS, L"Missing equals sign between attribute and attribute value.", x->p);
    ++x->p;
    skip_space(x);
    if (x->p >= x->n || (x->s[x->p] != '"' && x->s[x->p] != '\'')) return fail(x, XML_E_MISSINGQUOTE, L"A string literal was expected, but no opening quote character was found.", x->p);
    const WCHAR quote = x->s[x->p++];
    const SIZE_T value_at = x->p;
    while (x->p < x->n && x->s[x->p] != quote) ++x->p;
    if (x->p >= x->n) return fail(x, XML_E_UNCLOSEDSTRING, L"A string literal was not closed.", value_at - 1);
    for (const Node *a = e->attributes; a; a = a->next)
      if (same_text(a->name, a->name_length, x->s + start, length)) return fail(x, XML_E_DUPLICATEATTRIBUTE, L"Duplicate attribute.", name_at);
    if (e->attribute_count >= MAX_ATTRIBUTES) return fail(x, XML_E_TOOLARGE, L"An element has too many attributes.", name_at);
    Node *a = new_node(x->tree, NODE_ATTRIBUTE);
    if (!a) return fail_memory(x);
    if (!(a->name = store(x, x->s + start, length))) return FALSE;
    a->name_length = length;
    a->prefix_length = prefix_length;
    if (!decode(x, value_at, x->p, DECODE_ATTRIBUTE, &a->value, &a->value_length)) return FALSE;
    ++x->p;      /* (the closing quote) */
    a->parent = e;
    a->previous = e->last_attribute;
    if (e->last_attribute) e->last_attribute->next = a; else e->attributes = a;
    e->last_attribute = a;
    ++e->attribute_count;
    if (same_ascii(a->name, a->name_length, "xmlns")) e->declares = TRUE;
    else if (prefix_length == 5 && same_ascii(a->name, 5, "xmlns")) {
      if (a->value_length == 0) return fail(x, XML_E_RESERVEDNAMESPACE, L"A namespace prefix cannot be bound to an empty namespace.", name_at);
      if (same_ascii(a->name + 6, length - 6, "xmlns")) return fail(x, XML_E_RESERVEDNAMESPACE, L"The prefix 'xmlns' is reserved.", name_at);
      e->declares = TRUE;
    }
  }
  e->scope = e->declares ? e : x->open->type == NODE_ELEMENT ? x->open->scope : NULL;
  if (!resolve_namespace(x, e, e, tag_at)) return FALSE;
  for (Node *a = e->attributes; a; a = a->next) if (!resolve_namespace(x, e, a, tag_at)) return FALSE;
  e->last_order = x->tree->next_order - 1;
  if (open) { x->open = e; ++x->depth; }
  return TRUE;
}

/* "</name>", the position after the "</". */
static BOOL parse_end_tag(Parser *x) {
  const SIZE_T tag_at = x->p;
  SIZE_T start;
  UINT length, prefix_length;
  Node *e = x->open;
  if (!parse_name(x, &start, &length, &prefix_length)) return FALSE;
  if (e->type != NODE_ELEMENT) return fail(x, XML_E_UNEXPECTEDENDTAG, L"End tag was not expected at this location.", tag_at);
  if (!same_text(e->name, e->name_length, x->s + start, length)) return fail(x, XML_E_ENDTAGMISMATCH, L"End tag does not match the start tag.", tag_at);
  skip_space(x);
  if (x->p >= x->n || x->s[x->p] != '>') return fail(x, XML_E_UNCLOSEDENDTAG, L"An end tag was not closed.", x->p);
  ++x->p;
  e->last_order = x->tree->next_order - 1;
  x->open = e->parent;
  --x->depth;
  return TRUE;
}

/* "<!-- ... -->", the position at the "<!--". */
static BOOL parse_comment(Parser *x) {
  const SIZE_T comment_at = x->p;
  x->p += 4;
  const SIZE_T end = find(x, "--");
  if (end >= x->n) return fail(x, XML_E_UNCLOSEDCOMMENT, L"A comment was not closed.", comment_at);
  if (x->n - end < 3 || x->s[end + 2] != '>') return fail(x, XML_E_COMMENTSYNTAX, L"Incorrect syntax was used in a comment.", end);
  Node *n = add_node(x, NODE_COMMENT);
  if (!n || !decode(x, x->p, end, DECODE_RAW, &n->value, &n->value_length)) return FALSE;
  x->p = end + 3;
  return TRUE;
}

/* "<?target data?>", the position at the "<?". */
static BOOL parse_instruction(Parser *x) {
  const SIZE_T instruction_at = x->p;
  SIZE_T start;
  UINT length, prefix_length;
  x->p += 2;
  if (!parse_name(x, &start, &length, &prefix_length)) return FALSE;
  if (length == 3 && (x->s[start] | 0x20) == 'x' && (x->s[start + 1] | 0x20) == 'm' && (x->s[start + 2] | 0x20) == 'l')
    return fail(x, XML_E_BADXMLDECL, L"The XML declaration must be the first thing in the document.", instruction_at);
  if (!at(x, "?>") && !skip_space(x)) return fail(x, XML_E_MISSINGWHITESPACE, L"Required white space was missing.", x->p);
  const SIZE_T end = find(x, "?>");
  if (end >= x->n) return fail(x, XML_E_UNCLOSEDPI, L"A processing instruction was not closed.", instruction_at);
  Node *n = add_node(x, NODE_PROCESSING_INSTRUCTION);
  if (!n || !(n->name = store(x, x->s + start, length))) return FALSE;
  n->name_length = length;
  if (!decode(x, x->p, end, DECODE_RAW, &n->value, &n->value_length)) return FALSE;
  x->p = end + 2;
  return TRUE;
}

/* name="value" inside the XML declaration: is it `name`? Then the value's place. */
static BOOL declaration_attribute(Parser *x, const char *name, SIZE_T *value_at, SIZE_T *value_length) {
  const SIZE_T saved = x->p;
  if (!skip_space(x) || !at(x, name)) { x->p = saved; return FALSE; }
  while (*name) { ++name; ++x->p; }
  skip_space(x);
  if (x->p >= x->n || x->s[x->p] != '=') { x->p = saved; return FALSE; }
  ++x->p;
  skip_space(x);
  if (x->p >= x->n || (x->s[x->p] != '"' && x->s[x->p] != '\'')) { x->p = saved; return FALSE; }
  const WCHAR quote = x->s[x->p++];
  *value_at = x->p;
  while (x->p < x->n && x->s[x->p] != quote) ++x->p;
  if (x->p >= x->n) { x->p = saved; return FALSE; }
  *value_length = x->p - *value_at;
  ++x->p;
  return TRUE;
}

/* "<?xml version="1.0" encoding="..." standalone="..."?>" at the very start. It becomes a processing instruction node called xml, as in MSXML. */
static BOOL parse_declaration(Parser *x) {
  const SIZE_T declaration_at = x->p;
  SIZE_T value_at, value_length;
  x->p += 5;
  const SIZE_T data_at = x->p + 1;
  if (!declaration_attribute(x, "version", &value_at, &value_length)) return fail(x, XML_E_XMLDECLSYNTAX, L"Invalid syntax for an xml declaration.", x->p);
  if (value_length != 3 || x->s[value_at] != '1' || x->s[value_at + 1] != '.' || x->s[value_at + 2] < '0' || x->s[value_at + 2] > '9')
    return fail(x, XML_E_INVALID_VERSION, L"Invalid version number.", value_at);
  if (declaration_attribute(x, "encoding", &value_at, &value_length)) {
    if (value_length == 0) return fail(x, XML_E_INVALIDENCODING, L"Invalid encoding name.", value_at);
    for (SIZE_T i = 0; i < value_length; ++i) {
      const WCHAR c = x->s[value_at + i];
      if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (i > 0 && ((c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.'))))
        return fail(x, XML_E_INVALIDENCODING, L"Invalid encoding name.", value_at + i);
    }
  }
  if (declaration_attribute(x, "standalone", &value_at, &value_length)) {
    if (!same_ascii(x->s + value_at, value_length, "yes") && !same_ascii(x->s + value_at, value_length, "no"))
      return fail(x, XML_E_XMLDECLSYNTAX, L"Invalid syntax for an xml declaration.", value_at);
  }
  const SIZE_T data_end = x->p;
  skip_space(x);
  if (!at(x, "?>")) return fail(x, XML_E_XMLDECLSYNTAX, L"Invalid syntax for an xml declaration.", x->p >= x->n ? declaration_at : x->p);
  x->p += 2;
  Node *n = add_node(x, NODE_PROCESSING_INSTRUCTION);
  if (!n || !(n->name = store(x, L"xml", 3))) return FALSE;
  n->name_length = 3;
  return decode(x, data_at, data_end, DECODE_RAW, &n->value, &n->value_length);
}

/* "<!DOCTYPE ...>", with or without an internal subset: skipped. What it declares is not used (no entities, no defaults) and nothing it names is read. */
static BOOL skip_doctype(Parser *x) {
  const SIZE_T doctype_at = x->p;
  BOOL subset = FALSE;
  UINT nested = 0;
  if (x->open->type != NODE_DOCUMENT || x->seen_root || x->seen_doctype) return fail(x, XML_E_INVALIDATROOTLEVEL, L"A DOCTYPE is not allowed here.", doctype_at);
  x->seen_doctype = TRUE;
  x->p += 9;
  if (!skip_space(x)) return fail(x, XML_E_MISSINGWHITESPACE, L"Required white space was missing.", x->p);
  while (x->p < x->n) {
    const WCHAR c = x->s[x->p];
    if (c == '"' || c == '\'') {
      ++x->p;
      while (x->p < x->n && x->s[x->p] != c) ++x->p;
      if (x->p >= x->n) break;
      ++x->p;
    } else if (subset && at(x, "<!--")) {
      x->p += 4;
      x->p = find(x, "-->");
      if (x->p >= x->n) break;
      x->p += 3;
    } else if (subset && at(x, "<?")) {
      x->p = find(x, "?>");
      if (x->p >= x->n) break;
      x->p += 2;
    } else if (c == '<') {
      if (!subset) return fail(x, XML_E_BADDECLNAME, L"Invalid character in a DOCTYPE.", x->p);
      ++nested;
      ++x->p;
    } else if (c == '[' && !subset && !nested) {
      subset = TRUE;
      ++x->p;
    } else if (c == ']' && subset && !nested) {
      subset = FALSE;
      ++x->p;
    } else if (c == '>') {
      ++x->p;
      if (nested) --nested;
      else if (!subset) return TRUE;
      else return fail(x, XML_E_BADDECLNAME, L"Invalid character in a DOCTYPE.", x->p - 1);
    } else ++x->p;
  }
  return fail(x, XML_E_UNCLOSEDDECL, L"A DOCTYPE was not closed.", doctype_at);
}

/* "<![CDATA[ ... ]]>", the position at the "<![CDATA[". */
static BOOL parse_cdata(Parser *x) {
  const SIZE_T cdata_at = x->p;
  if (x->open->type != NODE_ELEMENT) return fail(x, XML_E_INVALIDATROOTLEVEL, L"Invalid at the top level of the document.", cdata_at);
  x->p += 9;
  const SIZE_T end = find(x, "]]>");
  if (end >= x->n) return fail(x, XML_E_UNCLOSEDCDATA, L"A CDATA section was not closed.", cdata_at);
  Node *n = add_node(x, NODE_CDATA_SECTION);
  if (!n || !decode(x, x->p, end, DECODE_RAW, &n->value, &n->value_length)) return FALSE;
  x->p = end + 3;
  return TRUE;
}

/* Characters up to the next '<'. */
static BOOL parse_text(Parser *x) {
  const SIZE_T text_at = x->p;
  BOOL blank = TRUE;
  while (x->p < x->n && x->s[x->p] != '<') { if (!is_space(x->s[x->p])) blank = FALSE; ++x->p; }
  if (x->open->type != NODE_ELEMENT) {
    if (blank) return TRUE;
    SIZE_T i = text_at;
    while (is_space(x->s[i])) ++i;
    return fail(x, XML_E_INVALIDATROOTLEVEL, L"Invalid at the top level of the document.", i);
  }
  if (blank && !x->tree->preserve) return TRUE;      /* (white space between elements) */
  Node *n = add_node(x, NODE_TEXT);
  return n && decode(x, text_at, x->p, DECODE_TEXT, &n->value, &n->value_length);
}

/* Every unit is a character XML allows, and the surrogates come in pairs. */
static BOOL check_characters(Parser *x) {
  for (SIZE_T i = 0; i < x->n; ++i) {
    const WCHAR c = x->s[i];
    if (c >= 0xD800 && c <= 0xDBFF) {
      if (x->n - i < 2 || x->s[i + 1] < 0xDC00 || x->s[i + 1] > 0xDFFF) return fail(x, XML_E_BADCHARDATA, L"An invalid character was found in text content.", i);
      ++i;
    } else if ((c >= 0xDC00 && c <= 0xDFFF) || !is_xml_char(c)) return fail(x, XML_E_BADCHARDATA, L"An invalid character was found in text content.", i);
  }
  return TRUE;
}

/* Builds the tree of the document s[0, n) in `x->tree` (an empty tree). FALSE: x->code, x->reason and x->error_at say why, and the tree is to be thrown away. */
static BOOL parse_document(Parser *x) {
  if (x->n > MAX_INPUT) return fail(x, XML_E_TOOLARGE, L"The document is too large.", 0);
  if (!check_characters(x)) return FALSE;
  x->open = x->tree->root;
  if (x->n && x->s[0] == 0xFEFF) x->p = 1;
  if (at(x, "<?xml") && x->n - x->p > 5 && is_space(x->s[x->p + 5]) && !parse_declaration(x)) return FALSE;
  while (x->p < x->n) {
    if (x->s[x->p] != '<') { if (!parse_text(x)) return FALSE; continue; }
    if (at(x, "<!--")) { if (!parse_comment(x)) return FALSE; }
    else if (at(x, "<![CDATA[")) { if (!parse_cdata(x)) return FALSE; }
    else if (at(x, "<!DOCTYPE")) { if (!skip_doctype(x)) return FALSE; }
    else if (at(x, "<!")) return fail(x, XML_E_BADDECLNAME, L"Invalid declaration.", x->p);
    else if (at(x, "<?")) { if (!parse_instruction(x)) return FALSE; }
    else if (at(x, "</")) { x->p += 2; if (!parse_end_tag(x)) return FALSE; }
    else { ++x->p; if (!parse_start_tag(x)) return FALSE; }
  }
  if (x->open->type != NODE_DOCUMENT) return fail(x, XML_E_UNCLOSEDTAG, L"An element was not closed.", x->n);
  if (!x->seen_root) return fail(x, XML_E_MISSINGROOT, L"XML document must have a top level element.", x->n);
  x->tree->root->last_order = x->tree->next_order - 1;
  return TRUE;
}
