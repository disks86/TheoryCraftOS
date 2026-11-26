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

/* The preprocessor of HLSL (D3D10PreprocessShader): the C preprocessor as Microsoft's shader compiler has it. Lines continued with a backslash are joined, comments
 * become a space; #define and #undef of object-like and function-like macros (with # and ##, variadic ones with __VA_ARGS__), __LINE__ and __FILE__; #include of
 * "local" and <system> files through the caller's opener (ID3D10Include); #if, #ifdef, #ifndef, #elif, #else and #endif with defined() and integer expressions; #line;
 * #pragma passed through; #error fails with its text. The text keeps its line numbers (a #line directive where a file begins or resumes). Errors go into a second
 * text as "file(line): error: what". Plain C over dxbc_disasm.h's text buffers (DXD_REALLOC / DXD_FREE), for a host test. Nothing is assumed about the input: every
 * read is within its length, recursion and include depth are bounded. */
#ifndef TCOS_HLSL_PP_H
#define TCOS_HLSL_PP_H

#include "dxbc_disasm.h"

typedef struct HppIo {
  void *user;
  /* Opens an include (`system`: <name>); `parent` is the text of the file that includes it (NULL: the top file). Nonzero with the text, or 0. */
  int (*open)(void *user, int system, const char *name, const char *parent, const char **data, size_t *bytes);
  void (*close)(void *user, const char *data);
} HppIo;

typedef struct HppMacro {
  char *name;
  char *body;
  char **params;
  uint32_t param_count;
  int function_like, variadic;
} HppMacro;

#define HPP_MAX_INCLUDE 32
#define HPP_MAX_DEPTH 64
#define HPP_MAX_CONDITIONS 256
typedef struct HppFile { const char *data; size_t size, pos; char *name; uint32_t line; int included; } HppFile;
typedef struct HppCond { int active, taken, seen_else, parent; } HppCond;      /* (a #if: whether its part is in, one was, #else was seen, its parent's part is in) */
typedef struct Hpp {
  HppMacro *macros;
  size_t macro_count, macro_capacity;
  HppFile files[HPP_MAX_INCLUDE + 1];
  int depth;
  HppCond cond[HPP_MAX_CONDITIONS];
  int cond_count[HPP_MAX_INCLUDE + 1];      /* (where each file's conditions begin) */
  int conds;
  const HppIo *io;
  DxdText *out, *errors;
  int failed;
  const char *disabled[HPP_MAX_DEPTH];      /* (the macros being expanded, which are not expanded again) */
  int disabled_count;
  int marked;      /* (the last directive wrote a #line marker, which stands for its line) */
} Hpp;

static char *hpp_dup(const char *s, size_t n) {
  char *d = (char *)DXD_REALLOC(NULL, n + 1);
  if (!d) return NULL;
  for (size_t i = 0; i < n; ++i) d[i] = s[i];
  d[n] = 0;
  return d;
}
static int hpp_id_start(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }
static int hpp_id_char(char c) { return hpp_id_start(c) || (c >= '0' && c <= '9'); }
static int hpp_space(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\f' || c == '\v'; }
static size_t hpp_len(const char *s) { size_t n = 0; while (s[n]) ++n; return n; }
static int hpp_eqn(const char *a, size_t n, const char *b) {
  for (size_t i = 0; i < n; ++i) if (a[i] != b[i] || !b[i]) return 0;
  return b[n] == 0;
}

static void hpp_error(Hpp *p, const char *what, const char *detail, size_t detail_length) {
  const HppFile *f = &p->files[p->depth];
  dxd_lit(p->errors, f->name ? f->name : "memory");
  dxd_lit(p->errors, "(");
  dxd_uint(p->errors, f->line);
  dxd_lit(p->errors, "): error: ");
  dxd_lit(p->errors, what);
  if (detail) { dxd_lit(p->errors, ": "); dxd_raw(p->errors, detail, detail_length); }
  dxd_lit(p->errors, "\n");
  p->failed = 1;
}
static void hpp_line_marker(Hpp *p) {
  const HppFile *f = &p->files[p->depth];
  dxd_lit(p->out, "#line ");
  dxd_uint(p->out, f->line);
  dxd_lit(p->out, " \"");
  for (const char *s = f->name ? f->name : ""; *s; ++s) {
    if (*s == '\\' || *s == '"') dxd_lit(p->out, "\\");
    dxd_raw(p->out, s, 1);
  }
  dxd_lit(p->out, "\"\n");
}

/* ---- macros ---------------------------------------------------------------------------------------------------------------------------------------------------- */

static HppMacro *hpp_find(Hpp *p, const char *name, size_t n) {
  for (size_t i = 0; i < p->macro_count; ++i)
    if (hpp_eqn(name, n, p->macros[i].name)) return &p->macros[i];
  return NULL;
}
static void hpp_macro_free(HppMacro *m) {
  DXD_FREE(m->name);
  DXD_FREE(m->body);
  for (uint32_t i = 0; i < m->param_count; ++i) DXD_FREE(m->params[i]);
  DXD_FREE(m->params);
}
static void hpp_undef(Hpp *p, const char *name, size_t n) {
  HppMacro *m = hpp_find(p, name, n);
  if (!m) return;
  hpp_macro_free(m);
  *m = p->macros[--p->macro_count];
}
/* Adds a macro (replacing one of the name). Takes the strings. */
static int hpp_add(Hpp *p, HppMacro *m) {
  hpp_undef(p, m->name, hpp_len(m->name));
  if (p->macro_count == p->macro_capacity) {
    const size_t want = p->macro_capacity ? p->macro_capacity * 2 : 32;
    if (want > (size_t)-1 / sizeof(HppMacro)) return 0;
    HppMacro *grown = (HppMacro *)DXD_REALLOC(p->macros, want * sizeof(HppMacro));
    if (!grown) return 0;
    p->macros = grown;
    p->macro_capacity = want;
  }
  p->macros[p->macro_count++] = *m;
  return 1;
}
/* "name body" or "name(params) body" of a #define: FALSE (and an error) when it is not one. */
static int hpp_define(Hpp *p, const char *s, size_t n) {
  size_t i = 0;
  while (i < n && hpp_space(s[i])) ++i;
  if (i == n || !hpp_id_start(s[i])) { hpp_error(p, "#define without a macro name", NULL, 0); return 0; }
  const size_t name_at = i;
  while (i < n && hpp_id_char(s[i])) ++i;
  HppMacro m = {0, 0, 0, 0, 0, 0};
  m.name = hpp_dup(s + name_at, i - name_at);
  if (!m.name) { p->failed = 1; return 0; }
  if (i < n && s[i] == '(') {      /* (a parameter list only when the parenthesis touches the name) */
    m.function_like = 1;
    ++i;
    for (;;) {
      while (i < n && hpp_space(s[i])) ++i;
      if (i < n && s[i] == ')') { ++i; break; }
      if (i + 2 < n && s[i] == '.' && s[i + 1] == '.' && s[i + 2] == '.') {
        m.variadic = 1;
        i += 3;
        while (i < n && hpp_space(s[i])) ++i;
        if (i < n && s[i] == ')') { ++i; break; }
        hpp_error(p, "invalid macro parameter list", NULL, 0);
        hpp_macro_free(&m);
        return 0;
      }
      if (i == n || !hpp_id_start(s[i])) { hpp_error(p, "invalid macro parameter list", NULL, 0); hpp_macro_free(&m); return 0; }
      const size_t at = i;
      while (i < n && hpp_id_char(s[i])) ++i;
      char **grown = (char **)DXD_REALLOC(m.params, (m.param_count + 1) * sizeof(char *));
      char *param = hpp_dup(s + at, i - at);
      if (!grown || !param) { if (grown) m.params = grown; DXD_FREE(param); hpp_macro_free(&m); p->failed = 1; return 0; }
      m.params = grown;
      m.params[m.param_count++] = param;
      while (i < n && hpp_space(s[i])) ++i;
      if (i < n && s[i] == ',') { ++i; continue; }
      if (i < n && s[i] == ')') { ++i; break; }
      hpp_error(p, "invalid macro parameter list", NULL, 0);
      hpp_macro_free(&m);
      return 0;
    }
  }
  while (i < n && hpp_space(s[i])) ++i;
  size_t end = n;
  while (end > i && hpp_space(s[end - 1])) --end;
  m.body = hpp_dup(s + i, end - i);
  if (!m.body || !hpp_add(p, &m)) { hpp_macro_free(&m); p->failed = 1; return 0; }
  return 1;
}

static void hpp_expand(Hpp *p, const char *s, size_t n, DxdText *out, int depth);
/* The extent of a string or character literal starting at `i` (its closing quote included; the end of the text if there is none). */
static size_t hpp_literal_end(const char *s, size_t n, size_t i) {
  const char q = s[i++];
  while (i < n && s[i] != q) { if (s[i] == '\\' && i + 1 < n) ++i; ++i; }
  return i < n ? i + 1 : n;
}
/* The arguments of a function-like macro's call: from `*at` (the opening parenthesis), split at the commas outside parentheses; FALSE without the closing one. */
typedef struct HppArgs { size_t start[64], end[64]; uint32_t count; } HppArgs;
static int hpp_args(const char *s, size_t n, size_t *at, HppArgs *a, uint32_t keep_commas_after) {
  size_t i = *at + 1;
  int level = 0;
  a->count = 0;
  size_t start = i;
  while (i < n) {
    const char c = s[i];
    if (c == '"' || c == '\'') { i = hpp_literal_end(s, n, i); continue; }
    if (c == '(') ++level;
    else if (c == ')') {
      if (level == 0) {
        if (a->count >= 64) return 0;
        a->start[a->count] = start; a->end[a->count] = i; ++a->count;
        *at = i + 1;
        return 1;
      }
      --level;
    } else if (c == ',' && level == 0 && a->count < keep_commas_after) {
      if (a->count >= 63) return 0;
      a->start[a->count] = start; a->end[a->count] = i; ++a->count;
      start = i + 1;
    }
    ++i;
  }
  return 0;
}
static void hpp_trim(const char *s, size_t *b, size_t *e) {
  while (*b < *e && (hpp_space(s[*b]) || s[*b] == '\n')) ++*b;
  while (*e > *b && (hpp_space(s[*e - 1]) || s[*e - 1] == '\n')) --*e;
}
static int hpp_param_index(const HppMacro *m, const char *s, size_t n) {
  for (uint32_t k = 0; k < m->param_count; ++k) if (hpp_eqn(s, n, m->params[k])) return (int)k;
  if (m->variadic && hpp_eqn(s, n, "__VA_ARGS__")) return (int)m->param_count;
  return -1;
}
/* The body of a function-like macro with its arguments put in: # makes a string of one (as it was written), ## joins what is either side (the arguments as written),
 * any other use of a parameter is its argument fully expanded. */
static void hpp_substitute(Hpp *p, const HppMacro *m, const char *s, const HppArgs *a, DxdText *out, int depth) {
  const char *b = m->body;
  const size_t n = hpp_len(b);
  size_t i = 0;
  while (i < n) {
    const char c = b[i];
    if (c == '"' || c == '\'') { const size_t e = hpp_literal_end(b, n, i); dxd_raw(out, b + i, e - i); i = e; continue; }
    if (c == '#' && i + 1 < n && b[i + 1] == '#') {      /* (## : the spaces around it go) */
      while (out->size && hpp_space(out->data[out->size - 1])) out->data[--out->size] = 0;
      i += 2;
      while (i < n && hpp_space(b[i])) ++i;
      if (i < n && hpp_id_start(b[i])) {
        size_t e = i;
        while (e < n && hpp_id_char(b[e])) ++e;
        const int k = hpp_param_index(m, b + i, e - i);
        if (k >= 0 && (uint32_t)k < a->count) {
          size_t s0 = a->start[k], s1 = a->end[k];
          hpp_trim(s, &s0, &s1);
          dxd_raw(out, s + s0, s1 - s0);
          i = e;
          continue;
        }
      }
      continue;
    }
    if (c == '#') {      /* # parameter: a string of the argument */
      size_t j = i + 1;
      while (j < n && hpp_space(b[j])) ++j;
      size_t e = j;
      while (e < n && hpp_id_char(b[e])) ++e;
      const int k = e > j ? hpp_param_index(m, b + j, e - j) : -1;
      if (k >= 0 && (uint32_t)k < a->count) {
        size_t s0 = a->start[k], s1 = a->end[k];
        hpp_trim(s, &s0, &s1);
        dxd_lit(out, "\"");
        for (size_t q = s0; q < s1; ++q) {
          if (s[q] == '"' || s[q] == '\\') dxd_lit(out, "\\");
          dxd_raw(out, s + q, 1);
        }
        dxd_lit(out, "\"");
        i = e;
        continue;
      }
    }
    if (hpp_id_start(c)) {
      size_t e = i;
      while (e < n && hpp_id_char(b[e])) ++e;
      const int k = hpp_param_index(m, b + i, e - i);
      if (k >= 0) {
        /* (the argument as written when ## follows) */
        size_t f = e;
        while (f < n && hpp_space(b[f])) ++f;
        const int pasted = f + 1 < n && b[f] == '#' && b[f + 1] == '#';
        if ((uint32_t)k < a->count) {
          size_t s0 = a->start[k], s1 = a->end[k];
          hpp_trim(s, &s0, &s1);
          if (pasted) dxd_raw(out, s + s0, s1 - s0);
          else hpp_expand(p, s + s0, s1 - s0, out, depth + 1);
        }
        i = e;
        continue;
      }
      dxd_raw(out, b + i, e - i);
      i = e;
      continue;
    }
    dxd_raw(out, b + i, 1);
    ++i;
  }
}
static int hpp_is_disabled(const Hpp *p, const char *name) {
  for (int i = 0; i < p->disabled_count; ++i) if (p->disabled[i] == name) return 1;
  return 0;
}
/* Expands the macros of a text into `out`. */
static void hpp_expand(Hpp *p, const char *s, size_t n, DxdText *out, int depth) {
  if (depth > HPP_MAX_DEPTH) { hpp_error(p, "macros nested too deeply", NULL, 0); return; }
  size_t i = 0;
  while (i < n && !p->failed) {
    const char c = s[i];
    if (c == '"' || c == '\'') { const size_t e = hpp_literal_end(s, n, i); dxd_raw(out, s + i, e - i); i = e; continue; }
    if ((c >= '0' && c <= '9') || (c == '.' && i + 1 < n && s[i + 1] >= '0' && s[i + 1] <= '9')) {      /* (a number: not names inside it) */
      size_t e = i + 1;
      while (e < n && (hpp_id_char(s[e]) || s[e] == '.' || ((s[e] == '+' || s[e] == '-') && (s[e - 1] == 'e' || s[e - 1] == 'E')))) ++e;
      dxd_raw(out, s + i, e - i);
      i = e;
      continue;
    }
    if (!hpp_id_start(c)) { dxd_raw(out, s + i, 1); ++i; continue; }
    size_t e = i;
    while (e < n && hpp_id_char(s[e])) ++e;
    if (hpp_eqn(s + i, e - i, "__LINE__")) { dxd_uint(out, p->files[p->depth].line); i = e; continue; }
    if (hpp_eqn(s + i, e - i, "__FILE__")) {
      dxd_lit(out, "\"");
      dxd_lit(out, p->files[p->depth].name ? p->files[p->depth].name : "");
      dxd_lit(out, "\"");
      i = e;
      continue;
    }
    HppMacro *m = hpp_find(p, s + i, e - i);
    if (!m || hpp_is_disabled(p, m->name) || p->disabled_count >= HPP_MAX_DEPTH) { dxd_raw(out, s + i, e - i); i = e; continue; }
    if (!m->function_like) {
      DxdText body = {0, 0, 0, 0, 0};
      p->disabled[p->disabled_count++] = m->name;
      const char *name = m->name;
      hpp_expand(p, m->body, hpp_len(m->body), &body, depth + 1);
      p->disabled_count--;
      (void)name;
      if (body.failed) p->failed = 1;
      if (body.data) { dxd_raw(out, body.data, body.size); DXD_FREE(body.data); }
      i = e;
      continue;
    }
    /* a function-like macro: only with a call after it */
    size_t j = e;
    while (j < n && (hpp_space(s[j]) || s[j] == '\n')) ++j;
    if (j >= n || s[j] != '(') { dxd_raw(out, s + i, e - i); i = e; continue; }
    HppArgs a;
    if (!hpp_args(s, n, &j, &a, m->variadic ? m->param_count : 64)) { hpp_error(p, "unterminated call of macro", m->name, hpp_len(m->name)); return; }
    /* (a call with nothing in its parentheses has no arguments when the macro takes none) */
    if (a.count == 1 && m->param_count == 0 && !m->variadic) {
      size_t b0 = a.start[0], b1 = a.end[0];
      hpp_trim(s, &b0, &b1);
      if (b0 == b1) a.count = 0;
    }
    const uint32_t want = m->param_count + (m->variadic ? 1u : 0u);
    if (a.count != want && !(m->variadic && a.count == m->param_count)) {
      hpp_error(p, "wrong number of macro arguments", m->name, hpp_len(m->name));
      return;
    }
    DxdText body = {0, 0, 0, 0, 0};
    hpp_substitute(p, m, s, &a, &body, depth);
    p->disabled[p->disabled_count++] = m->name;
    DxdText again = {0, 0, 0, 0, 0};
    if (body.data) hpp_expand(p, body.data, body.size, &again, depth + 1);
    p->disabled_count--;
    if (body.failed || again.failed) p->failed = 1;
    if (again.data) { dxd_raw(out, again.data, again.size); DXD_FREE(again.data); }
    DXD_FREE(body.data);
    i = j;
  }
}

/* ---- #if ------------------------------------------------------------------------------------------------------------------------------------------------------- */

typedef struct HppExpr { const char *s; size_t n, i; int bad; } HppExpr;
static void hpp_skip(HppExpr *e) { while (e->i < e->n && (hpp_space(e->s[e->i]) || e->s[e->i] == '\n')) ++e->i; }
static int hpp_peek(HppExpr *e, const char *op) {
  hpp_skip(e);
  const size_t n = hpp_len(op);
  if (e->i + n > e->n) return 0;
  for (size_t k = 0; k < n; ++k) if (e->s[e->i + k] != op[k]) return 0;
  return 1;
}
static int hpp_take(HppExpr *e, const char *op) {
  if (!hpp_peek(e, op)) return 0;
  e->i += hpp_len(op);
  return 1;
}
static int64_t hpp_ternary(HppExpr *e, int depth);
static int64_t hpp_primary(HppExpr *e, int depth) {
  hpp_skip(e);
  if (depth > 200) { e->bad = 1; return 0; }
  if (e->i >= e->n) { e->bad = 1; return 0; }
  const char c = e->s[e->i];
  if (hpp_take(e, "(")) {
    const int64_t v = hpp_ternary(e, depth + 1);
    if (!hpp_take(e, ")")) e->bad = 1;
    return v;
  }
  if (hpp_take(e, "!")) return !hpp_primary(e, depth + 1);
  if (hpp_take(e, "~")) return ~hpp_primary(e, depth + 1);
  if (hpp_take(e, "-")) return (int64_t)(0 - (uint64_t)hpp_primary(e, depth + 1));
  if (hpp_take(e, "+")) return hpp_primary(e, depth + 1);
  if (c >= '0' && c <= '9') {
    uint64_t v = 0;
    unsigned base = 10;
    if (c == '0' && e->i + 1 < e->n && (e->s[e->i + 1] == 'x' || e->s[e->i + 1] == 'X')) { base = 16; e->i += 2; }
    else if (c == '0') base = 8;
    int digits = 0;
    while (e->i < e->n) {
      const char d = e->s[e->i];
      unsigned x;
      if (d >= '0' && d <= '9') x = (unsigned)(d - '0');
      else if (d >= 'a' && d <= 'f') x = (unsigned)(d - 'a' + 10);
      else if (d >= 'A' && d <= 'F') x = (unsigned)(d - 'A' + 10);
      else break;
      if (x >= base) { e->bad = 1; return 0; }
      v = v * base + x;
      ++digits;
      ++e->i;
    }
    if (!digits && base == 16) e->bad = 1;
    while (e->i < e->n && (e->s[e->i] == 'u' || e->s[e->i] == 'U' || e->s[e->i] == 'l' || e->s[e->i] == 'L')) ++e->i;
    return (int64_t)v;
  }
  if (c == '\'') {      /* a character */
    int64_t v = 0;
    ++e->i;
    if (e->i < e->n && e->s[e->i] == '\\' && e->i + 1 < e->n) {
      ++e->i;
      const char x = e->s[e->i];
      v = x == 'n' ? 10 : x == 't' ? 9 : x == 'r' ? 13 : x == '0' ? 0 : x;
    } else if (e->i < e->n) v = (unsigned char)e->s[e->i];
    ++e->i;
    if (e->i >= e->n || e->s[e->i] != '\'') { e->bad = 1; return 0; }
    ++e->i;
    return v;
  }
  if (hpp_id_start(c)) {      /* a name that is no macro: 0 */
    while (e->i < e->n && hpp_id_char(e->s[e->i])) ++e->i;
    return 0;
  }
  e->bad = 1;
  return 0;
}
static int hpp_precedence(HppExpr *e, const char **op) {
  static const char *const ops[] = {"||", "&&", "|", "^", "&", "==", "!=", "<=", ">=", "<<", ">>", "<", ">", "+", "-", "*", "/", "%"};
  static const int prec[] = {1, 2, 3, 4, 5, 6, 6, 7, 7, 8, 8, 7, 7, 9, 9, 10, 10, 10};
  for (int k = 0; k < 18; ++k)
    if (hpp_peek(e, ops[k])) {
      /* (| is not ||, & is not &&) */
      if ((k == 2 && hpp_peek(e, "||")) || (k == 4 && hpp_peek(e, "&&"))) continue;
      *op = ops[k];
      return prec[k];
    }
  return 0;
}
static int64_t hpp_binary(HppExpr *e, int min, int depth) {
  int64_t left = hpp_primary(e, depth);
  for (;;) {
    const char *op;
    const int prec = hpp_precedence(e, &op);
    if (!prec || prec < min || e->bad) return left;
    e->i += hpp_len(op);
    const int64_t right = hpp_binary(e, prec + 1, depth + 1);
    const uint64_t l = (uint64_t)left, r = (uint64_t)right;
    switch (op[0]) {
      case '|': left = op[1] ? (left || right) : (int64_t)(l | r); break;
      case '&': left = op[1] ? (left && right) : (int64_t)(l & r); break;
      case '^': left = (int64_t)(l ^ r); break;
      case '=': left = left == right; break;
      case '!': left = left != right; break;
      case '<': left = op[1] == '=' ? left <= right : op[1] == '<' ? (int64_t)(l << (r & 63)) : left < right; break;
      case '>': left = op[1] == '=' ? left >= right : op[1] == '>' ? left >> (r & 63) : left > right; break;
      case '+': left = (int64_t)(l + r); break;
      case '-': left = (int64_t)(l - r); break;
      case '*': left = (int64_t)(l * r); break;
      case '/': case '%':
        if (!right || (left == INT64_MIN && right == -1)) { e->bad = 2; return 0; }
        left = op[0] == '/' ? left / right : left % right;
        break;
    }
  }
}
static int64_t hpp_ternary(HppExpr *e, int depth) {
  const int64_t c = hpp_binary(e, 1, depth);
  if (!hpp_take(e, "?")) return c;
  const int64_t a = hpp_ternary(e, depth + 1);
  if (!hpp_take(e, ":")) { e->bad = 1; return 0; }
  const int64_t b = hpp_ternary(e, depth + 1);
  return c ? a : b;
}
/* The value of a #if's condition: defined X / defined(X) first, then the macros, then the arithmetic. */
static int hpp_condition(Hpp *p, const char *s, size_t n) {
  DxdText pre = {0, 0, 0, 0, 0};
  size_t i = 0;
  while (i < n) {
    if (hpp_id_start(s[i])) {
      size_t e = i;
      while (e < n && hpp_id_char(s[e])) ++e;
      if (hpp_eqn(s + i, e - i, "defined")) {
        size_t j = e;
        while (j < n && hpp_space(s[j])) ++j;
        const int paren = j < n && s[j] == '(';
        if (paren) { ++j; while (j < n && hpp_space(s[j])) ++j; }
        size_t k = j;
        while (k < n && hpp_id_char(s[k])) ++k;
        if (k == j) { DXD_FREE(pre.data); hpp_error(p, "invalid use of defined", NULL, 0); return 0; }
        const int yes = hpp_find(p, s + j, k - j) != NULL || hpp_eqn(s + j, k - j, "__LINE__") || hpp_eqn(s + j, k - j, "__FILE__");
        if (paren) {
          while (k < n && hpp_space(s[k])) ++k;
          if (k >= n || s[k] != ')') { DXD_FREE(pre.data); hpp_error(p, "invalid use of defined", NULL, 0); return 0; }
          ++k;
        }
        dxd_lit(&pre, yes ? " 1 " : " 0 ");
        i = k;
        continue;
      }
      dxd_raw(&pre, s + i, e - i);
      i = e;
      continue;
    }
    dxd_raw(&pre, s + i, 1);
    ++i;
  }
  DxdText expanded = {0, 0, 0, 0, 0};
  if (pre.data) hpp_expand(p, pre.data, pre.size, &expanded, 0);
  DXD_FREE(pre.data);
  if (expanded.failed) p->failed = 1;
  HppExpr e = {expanded.data ? expanded.data : "", expanded.size, 0, 0};
  const int64_t v = hpp_ternary(&e, 0);
  hpp_skip(&e);
  if (!expanded.size) e.bad = 1;
  if (e.i < e.n && !e.bad) e.bad = 1;
  DXD_FREE(expanded.data);
  if (e.bad) { hpp_error(p, e.bad == 2 ? "division by zero in #if" : "invalid #if expression", NULL, 0); return 0; }
  return v != 0;
}

/* ---- lines and directives -------------------------------------------------------------------------------------------------------------------------------------- */

/* The next logical line of the current file into `line` (continued lines joined, comments one space; a block comment may run over lines): FALSE at its end.
 * `physical` is how many lines of the file it took. */
static int hpp_read_line(Hpp *p, DxdText *line, uint32_t *physical, int *in_comment) {
  HppFile *f = &p->files[p->depth];
  line->size = 0;
  if (line->data) line->data[0] = 0;
  *physical = 0;
  if (f->pos >= f->size) return 0;
  char quote = 0;
  while (f->pos < f->size) {
    const char c = f->data[f->pos];
    if (*in_comment) {
      if (c == '*' && f->pos + 1 < f->size && f->data[f->pos + 1] == '/') { *in_comment = 0; f->pos += 2; dxd_lit(line, " "); continue; }
      if (c == '\n') { ++*physical; ++f->pos; if (line->size) return 1; continue; }      /* (the rest of the comment is on the next line) */
      ++f->pos;
      continue;
    }
    if (c == '\\' && f->pos + 1 < f->size && (f->data[f->pos + 1] == '\n' || (f->data[f->pos + 1] == '\r' && f->pos + 2 < f->size && f->data[f->pos + 2] == '\n'))) {
      f->pos += f->data[f->pos + 1] == '\r' ? 3 : 2;      /* (a continued line) */
      ++*physical;
      continue;
    }
    if (c == '\n') { ++f->pos; ++*physical; return 1; }
    if (quote) {
      dxd_raw(line, &c, 1);
      if (c == '\\' && f->pos + 1 < f->size && f->data[f->pos + 1] != '\n') { dxd_raw(line, &f->data[f->pos + 1], 1); f->pos += 2; continue; }
      if (c == quote) quote = 0;
      ++f->pos;
      continue;
    }
    if (c == '"' || c == '\'') { quote = c; dxd_raw(line, &c, 1); ++f->pos; continue; }
    if (c == '/' && f->pos + 1 < f->size && f->data[f->pos + 1] == '/') {
      while (f->pos < f->size && f->data[f->pos] != '\n') {
        if (f->data[f->pos] == '\\' && f->pos + 1 < f->size && f->data[f->pos + 1] == '\n') { ++*physical; f->pos += 2; continue; }
        ++f->pos;
      }
      continue;
    }
    if (c == '/' && f->pos + 1 < f->size && f->data[f->pos + 1] == '*') { *in_comment = 1; f->pos += 2; continue; }
    if (c != '\r') dxd_raw(line, &c, 1);
    ++f->pos;
  }
  ++*physical;      /* (the last line, without a newline) */
  return 1;
}
static int hpp_active(const Hpp *p) { return p->conds == 0 || p->cond[p->conds - 1].active; }
static int hpp_push_file(Hpp *p, const char *data, size_t size, const char *name, size_t name_length, int included) {
  if (p->depth >= HPP_MAX_INCLUDE) { hpp_error(p, "#include nested too deeply", NULL, 0); return 0; }
  HppFile *f = &p->files[++p->depth];
  f->data = data;
  f->size = size;
  f->pos = 0;
  f->line = 1;
  f->included = included;
  f->name = name ? hpp_dup(name, name_length) : NULL;
  p->cond_count[p->depth] = p->conds;
  hpp_line_marker(p);
  return 1;
}
/* A directive (the text after '#'). */
static void hpp_directive(Hpp *p, const char *s, size_t n) {
  size_t i = 0;
  while (i < n && hpp_space(s[i])) ++i;
  const size_t word = i;
  while (i < n && hpp_id_char(s[i])) ++i;
  const char *w = s + word;
  const size_t wn = i - word;
  while (i < n && hpp_space(s[i])) ++i;
  const char *rest = s + i;
  size_t rn = n - i;
  while (rn && hpp_space(rest[rn - 1])) --rn;
  const int active = hpp_active(p);
  if (hpp_eqn(w, wn, "if") || hpp_eqn(w, wn, "ifdef") || hpp_eqn(w, wn, "ifndef")) {
    if (p->conds >= HPP_MAX_CONDITIONS) { hpp_error(p, "conditions nested too deeply", NULL, 0); return; }
    int value = 0;
    if (active) {
      if (wn == 2) value = hpp_condition(p, rest, rn);
      else {
        size_t k = 0;
        while (k < rn && hpp_id_char(rest[k])) ++k;
        if (!k || !hpp_id_start(rest[0])) { hpp_error(p, "#ifdef without a macro name", NULL, 0); return; }
        value = hpp_find(p, rest, k) != NULL;
        if (wn == 6) value = !value;
      }
    }
    p->cond[p->conds].parent = active;
    p->cond[p->conds].active = active && value;
    p->cond[p->conds].taken = active && value;
    p->cond[p->conds].seen_else = 0;
    ++p->conds;
    return;
  }
  if (hpp_eqn(w, wn, "elif") || hpp_eqn(w, wn, "else") || hpp_eqn(w, wn, "endif")) {
    if (p->conds <= p->cond_count[p->depth]) { hpp_error(p, "unexpected directive", w, wn); return; }
    HppCond *c = &p->cond[p->conds - 1];
    if (wn == 5 && w[1] == 'n') { --p->conds; return; }      /* endif */
    if (c->seen_else) { hpp_error(p, "directive after #else", w, wn); return; }
    if (wn == 4 && w[1] == 'l' && w[2] == 's') {      /* else */
      c->seen_else = 1;
      c->active = c->parent && !c->taken;
      c->taken = c->taken || c->active;
      return;
    }
    /* elif */
    if (c->parent && !c->taken) {
      const int v = hpp_condition(p, rest, rn);
      c->active = v;
      c->taken = v;
    } else c->active = 0;
    return;
  }
  if (!active) return;      /* (the other directives of a part that is left out are not looked at) */
  if (wn == 0) return;      /* (the null directive) */
  if (hpp_eqn(w, wn, "define")) { hpp_define(p, rest, rn); return; }
  if (hpp_eqn(w, wn, "undef")) {
    size_t k = 0;
    while (k < rn && hpp_id_char(rest[k])) ++k;
    if (!k) { hpp_error(p, "#undef without a macro name", NULL, 0); return; }
    hpp_undef(p, rest, k);
    return;
  }
  if (hpp_eqn(w, wn, "error")) { hpp_error(p, "#error", rest, rn); return; }
  if (hpp_eqn(w, wn, "pragma")) { dxd_lit(p->out, "#pragma "); dxd_raw(p->out, rest, rn); dxd_lit(p->out, "\n"); return; }
  if (hpp_eqn(w, wn, "line")) {
    DxdText x = {0, 0, 0, 0, 0};
    hpp_expand(p, rest, rn, &x, 0);
    size_t k = 0;
    uint64_t line = 0;
    const char *t = x.data ? x.data : "";
    while (k < x.size && hpp_space(t[k])) ++k;
    const size_t digits = k;
    while (k < x.size && t[k] >= '0' && t[k] <= '9' && line < 0x7FFFFFFF) line = line * 10 + (uint64_t)(t[k++] - '0');
    if (k == digits) { DXD_FREE(x.data); hpp_error(p, "invalid #line", NULL, 0); return; }
    while (k < x.size && hpp_space(t[k])) ++k;
    HppFile *f = &p->files[p->depth];
    if (k < x.size && t[k] == '"') {
      size_t e = k + 1;
      while (e < x.size && t[e] != '"') ++e;
      char *name = hpp_dup(t + k + 1, e - k - 1);
      if (name) { DXD_FREE(f->name); f->name = name; }
    }
    DXD_FREE(x.data);
    f->line = (uint32_t)line;
    hpp_line_marker(p);
    p->marked = 1;
    f->line = (uint32_t)line - 1;      /* (the line after this one is `line`: the reader counts it in) */
    return;
  }
  if (hpp_eqn(w, wn, "include")) {
    DxdText x = {0, 0, 0, 0, 0};
    const char *t = rest;
    size_t tn = rn;
    if (!rn || (rest[0] != '"' && rest[0] != '<')) { hpp_expand(p, rest, rn, &x, 0); t = x.data ? x.data : ""; tn = x.size; }
    size_t k = 0;
    while (k < tn && hpp_space(t[k])) ++k;
    const char close = k < tn && t[k] == '<' ? '>' : '"';
    if (k >= tn || (t[k] != '"' && t[k] != '<')) { DXD_FREE(x.data); hpp_error(p, "invalid #include", NULL, 0); return; }
    size_t e = k + 1;
    while (e < tn && t[e] != close) ++e;
    if (e >= tn) { DXD_FREE(x.data); hpp_error(p, "invalid #include", NULL, 0); return; }
    char *name = hpp_dup(t + k + 1, e - k - 1);
    DXD_FREE(x.data);
    if (!name) { p->failed = 1; return; }
    const char *data = NULL;
    size_t bytes = 0;
    if (!p->io || !p->io->open || !p->io->open(p->io->user, close == '>', name, p->depth > 0 && p->files[p->depth].included ? p->files[p->depth].data : NULL, &data, &bytes)) {
      hpp_error(p, "failed to open include file", name, hpp_len(name));
      DXD_FREE(name);
      return;
    }
    if (!hpp_push_file(p, data, bytes, name, hpp_len(name), 1)) { if (p->io->close) p->io->close(p->io->user, data); }
    else p->marked = 1;
    DXD_FREE(name);
    return;
  }
  hpp_error(p, "invalid preprocessor directive", w, wn);
}
/* Whether a line calls a function-like macro whose parentheses close on a later line (the call then takes the lines after it). */
static int hpp_open_call(Hpp *p, const char *s, size_t n) {
  int level = 0, after_macro = 0;
  for (size_t i = 0; i < n; ++i) {
    const char c = s[i];
    if (c == '"' || c == '\'') { i = hpp_literal_end(s, n, i) - 1; continue; }
    if (hpp_id_start(c)) {
      size_t e = i;
      while (e < n && hpp_id_char(s[e])) ++e;
      const HppMacro *m = hpp_find(p, s + i, e - i);
      if (m && m->function_like && level == 0) after_macro = 1;
      i = e - 1;
      continue;
    }
    if (c == '(') ++level;
    if (c == ')' && level) --level;
  }
  return after_macro && level > 0;
}

/* The whole text: the top file's `size` bytes, its name, the defines (names and values: a NULL value is 1) and the include opener. 1 when it went without error. */
static int hpp_run(const char *source, size_t size, const char *file, const char *const *names, const char *const *values, size_t defines, const HppIo *io, DxdText *out,
                   DxdText *errors) {
  Hpp *p = (Hpp *)DXD_REALLOC(NULL, sizeof(Hpp));
  if (!p) return 0;
  for (size_t i = 0; i < sizeof(Hpp); ++i) ((char *)p)[i] = 0;
  p->io = io;
  p->out = out;
  p->errors = errors;
  p->depth = -1;
  for (size_t i = 0; i < defines && !p->failed; ++i) {
    if (!names[i]) continue;
    DxdText d = {0, 0, 0, 0, 0};
    dxd_lit(&d, names[i]);
    dxd_lit(&d, " ");
    dxd_lit(&d, values[i] ? values[i] : "1");
    if (!d.failed && d.data) hpp_define(p, d.data, d.size);
    DXD_FREE(d.data);
  }
  hpp_push_file(p, source, size, file, file ? hpp_len(file) : 0, 0);
  DxdText line = {0, 0, 0, 0, 0}, more = {0, 0, 0, 0, 0};
  int in_comment = 0;
  while (p->depth >= 0 && !p->failed) {
    uint32_t physical;
    if (!hpp_read_line(p, &line, &physical, &in_comment)) {
      /* the end of a file: its conditions must be closed */
      if (p->conds > p->cond_count[p->depth]) { hpp_error(p, "unterminated conditional directive", NULL, 0); break; }
      HppFile *f = &p->files[p->depth];
      if (f->included && io && io->close) io->close(io->user, f->data);
      DXD_FREE(f->name);
      f->name = NULL;
      --p->depth;
      if (p->depth >= 0) hpp_line_marker(p);
      in_comment = 0;
      continue;
    }
    HppFile *f = &p->files[p->depth];
    const char *s = line.data ? line.data : "";
    size_t n = line.size, k = 0;
    while (k < n && hpp_space(s[k])) ++k;
    if (k < n && s[k] == '#') {
      const int depth_before = p->depth;
      p->marked = 0;
      hpp_directive(p, s + k + 1, n - k - 1);
      p->files[depth_before].line += physical;
      if (!p->marked) for (uint32_t x = 0; x < physical; ++x) dxd_lit(out, "\n");
      else if (p->depth == depth_before) for (uint32_t x = 1; x < physical; ++x) dxd_lit(out, "\n");
      continue;
    }
    if (!hpp_active(p)) { f->line += physical; for (uint32_t x = 0; x < physical; ++x) dxd_lit(out, "\n"); continue; }
    /* (a macro call that goes on over the next lines: they are read into it) */
    uint32_t extra = 0;
    while (hpp_open_call(p, line.data ? line.data : "", line.size) && f->pos < f->size) {
      uint32_t more_lines;
      const size_t keep = f->pos;
      if (!hpp_read_line(p, &more, &more_lines, &in_comment)) break;
      size_t m = 0;
      while (m < more.size && hpp_space(more.data[m])) ++m;
      if (m < more.size && more.data[m] == '#') { f->pos = keep; break; }      /* (a directive ends it) */
      dxd_lit(&line, "\n");
      if (more.data) dxd_raw(&line, more.data, more.size);
      extra += more_lines;
    }
    hpp_expand(p, line.data ? line.data : "", line.size, out, 0);
    dxd_lit(out, "\n");
    for (uint32_t x = 1; x < physical + extra; ++x) dxd_lit(out, "\n");
    f->line += physical + extra;
  }
  /* (what is left open after an error) */
  while (p->depth >= 0) {
    HppFile *f = &p->files[p->depth];
    if (f->included && io && io->close) io->close(io->user, f->data);
    DXD_FREE(f->name);
    --p->depth;
  }
  if (line.failed || more.failed || out->failed || errors->failed) p->failed = 1;
  DXD_FREE(line.data);
  DXD_FREE(more.data);
  for (size_t i = 0; i < p->macro_count; ++i) hpp_macro_free(&p->macros[i]);
  DXD_FREE(p->macros);
  const int ok = !p->failed;
  DXD_FREE(p);
  return ok;
}

#endif
