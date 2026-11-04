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

/* Part of dbghelp.c: UnDecorateSymbolName for the names of Microsoft's C++ compiler. The common signatures are decoded (global and member functions, static data, the
 * builtin types, pointers, references, class/struct/union/enum names with their scopes, constructors and destructors); a name with templates, operators, special symbols, arrays
 * or function pointers is not decoded in full and the call fails (the symbol handler then shows the decorated name). */

#define UND_TEXT 1024

typedef struct {
  const char *p;
  DWORD flags;
  char names[10][128];
  int nnames;
  BOOL fail;
} UND;

static void und_cat(char *d, const char *s) {
  SIZE_T n = slen(d);
  while (*s && n < UND_TEXT - 1) d[n++] = *s++;
  d[n] = 0;
}

/* Reads a run of names ending with '@' (the scopes of a qualified name, innermost first) into parts. */
static int und_parts(UND *u, char parts[][128], int max) {
  int n = 0;
  for (;;) {
    const char c = *u->p;
    if (!c) { u->fail = TRUE; return n; }
    if (c == '@') { u->p++; return n; }
    if (n == max) { u->fail = TRUE; return n; }
    if (c >= '0' && c <= '9') {
      const int k = c - '0';
      if (k >= u->nnames) { u->fail = TRUE; return n; }
      scopy(parts[n++], 128, u->names[k]);
      u->p++;
      continue;
    }
    if (c == '?') { u->fail = TRUE; return n; }      /* (templates and the nested special names) */
    SIZE_T len = 0;
    while (u->p[len] && u->p[len] != '@') len++;
    if (!u->p[len]) { u->fail = TRUE; return n; }
    char name[128];
    const SIZE_T k = len < 127 ? len : 127;
    mcopy(name, u->p, k);
    name[k] = 0;
    u->p += len + 1;
    scopy(parts[n++], 128, name);
    if (u->nnames < 10) scopy(u->names[u->nnames++], 128, name);
  }
}

static void und_qualify(char *out, char parts[][128], int n) {
  out[0] = 0;
  for (int i = n - 1; i >= 0; i--) { und_cat(out, parts[i]); if (i) und_cat(out, "::"); }
}

static void und_type(UND *u, char *out) {
  out[0] = 0;
  const char c = *u->p++;
  const char *simple = NULL;
  switch (c) {
    case 'X': simple = "void"; break;
    case 'C': simple = "signed char"; break;
    case 'D': simple = "char"; break;
    case 'E': simple = "unsigned char"; break;
    case 'F': simple = "short"; break;
    case 'G': simple = "unsigned short"; break;
    case 'H': simple = "int"; break;
    case 'I': simple = "unsigned int"; break;
    case 'J': simple = "long"; break;
    case 'K': simple = "unsigned long"; break;
    case 'M': simple = "float"; break;
    case 'N': simple = "double"; break;
    case 'O': simple = "long double"; break;
    case 'Z': simple = "..."; break;
    case '_': {
      const char d = *u->p++;
      switch (d) {
        case 'N': simple = "bool"; break;
        case 'J': simple = "__int64"; break;
        case 'K': simple = "unsigned __int64"; break;
        case 'W': simple = "wchar_t"; break;
        case 'D': simple = "__int8"; break;
        case 'E': simple = "unsigned __int8"; break;
        case 'F': simple = "__int16"; break;
        case 'G': simple = "unsigned __int16"; break;
        case 'H': simple = "__int32"; break;
        case 'I': simple = "unsigned __int32"; break;
        case 'S': simple = "char16_t"; break;
        case 'U': simple = "char32_t"; break;
        default: u->fail = TRUE;
      }
      break;
    }
    case 'P': case 'Q': case 'R': case 'S': case 'A': case 'B': {
      BOOL ptr64 = FALSE;
      if (*u->p == 'E') { ptr64 = TRUE; u->p++; }
      const char cv = *u->p++;
      if (cv < 'A' || cv > 'D') { u->fail = TRUE; return; }
      if (*u->p == '6' || *u->p == '8') { u->fail = TRUE; return; }      /* (function pointers) */
      char pointee[UND_TEXT];
      und_type(u, pointee);
      if (u->fail) return;
      und_cat(out, pointee);
      if (cv == 'B' || cv == 'D') und_cat(out, " const");
      if (cv == 'C' || cv == 'D') und_cat(out, " volatile");
      und_cat(out, c == 'A' || c == 'B' ? " &" : " *");
      if (ptr64) und_cat(out, " __ptr64");
      if (c == 'Q') und_cat(out, " const");
      if (c == 'R') und_cat(out, " volatile");
      if (c == 'S') und_cat(out, " const volatile");
      return;
    }
    case 'U': case 'V': case 'T': case 'W': {
      if (c == 'W') u->p++;      /* (the underlying type of the enum) */
      char parts[16][128];
      const int n = und_parts(u, parts, 16);
      if (u->fail || n == 0) { u->fail = TRUE; return; }
      und_cat(out, c == 'U' ? "struct " : c == 'V' ? "class " : c == 'T' ? "union " : "enum ");
      char q[UND_TEXT];
      und_qualify(q, parts, n);
      und_cat(out, q);
      return;
    }
    default: u->fail = TRUE;
  }
  if (simple) und_cat(out, simple);
}

static BOOL undecorate(const char *mangled, char *out, SIZE_T size, DWORD flags) {
  if (!size) return FALSE;
  if (mangled[0] != '?') {
    const char *s = mangled;
    if ((flags & UNDNAME_NO_LEADING_UNDERSCORES) && *s == '_') s++;
    scopy(out, size, s);
    return TRUE;
  }
  UND u;
  mzero(&u, sizeof(u));
  u.p = mangled + 1;
  u.flags = flags;
  char parts[16][128], name[UND_TEXT], text[UND_TEXT];
  text[0] = 0;
  int special = 0;      /* 1: constructor, 2: destructor */
  if (*u.p == '?') {
    u.p++;
    if (*u.p == '0') special = 1; else if (*u.p == '1') special = 2; else return FALSE;
    u.p++;
  }
  const int n = und_parts(&u, parts, 16);
  if (u.fail || n == 0) return FALSE;
  und_qualify(name, parts, n);
  if (special) {      /* ??0Foo@@: the class is the first part, the member repeats its name */
    und_cat(name, "::");
    if (special == 2) und_cat(name, "~");
    und_cat(name, parts[0]);
  }
  if (flags & UNDNAME_NAME_ONLY) { scopy(out, size, name); return TRUE; }
  const char kind = *u.p;
  if (kind >= '0' && kind <= '5') {
    u.p++;
    char type[UND_TEXT];
    und_type(&u, type);
    if (u.fail) return FALSE;
    const char *access = kind == '0' ? "private: static " : kind == '1' ? "protected: static " : kind == '2' ? "public: static " : "";
    if (flags & UNDNAME_NO_ACCESS_SPECIFIERS) access = kind <= '2' ? "static " : "";
    und_cat(text, access);
    und_cat(text, type);
    und_cat(text, " ");
    und_cat(text, name);
    scopy(out, size, text);
    return TRUE;
  }
  const char *access = "";
  BOOL is_static = FALSE, is_virtual = FALSE, member = FALSE, ptr64 = FALSE;
  const char *cv_suffix = "";
  if (kind == 'Y') {
    u.p++;
  } else if (kind >= 'A' && kind <= 'Z') {
    u.p++;
    member = TRUE;
    const int group = (kind - 'A') / 8, what = (kind - 'A') % 8;
    access = group == 0 ? "private: " : group == 1 ? "protected: " : "public: ";
    if (group > 2) return FALSE;
    if (what >= 6) return FALSE;      /* (thunks) */
    is_static = what == 2 || what == 3;
    is_virtual = what == 4 || what == 5;
    if (!is_static) {
      if (*u.p == 'E') { ptr64 = TRUE; u.p++; }
      const char q = *u.p++;
      if (q < 'A' || q > 'D') return FALSE;
      cv_suffix = q == 'A' ? "" : q == 'B' ? " const" : q == 'C' ? " volatile" : " const volatile";
    }
  } else return FALSE;
  const char conv = *u.p++;
  const char *conv_name;
  switch (conv) {
    case 'A': case 'B': conv_name = "__cdecl"; break;
    case 'C': case 'D': conv_name = "__pascal"; break;
    case 'E': case 'F': conv_name = "__thiscall"; break;
    case 'G': case 'H': conv_name = "__stdcall"; break;
    case 'I': case 'J': conv_name = "__fastcall"; break;
    default: return FALSE;
  }
  char ret[UND_TEXT];
  ret[0] = 0;
  if (special) { if (*u.p != '@') return FALSE; u.p++; }      /* (constructors and destructors have no return type) */
  else {
    if (*u.p == '?') return FALSE;      /* (a return type with a storage class of its own) */
    und_type(&u, ret);
  }
  if (u.fail) return FALSE;
  char args[UND_TEXT];
  args[0] = 0;
  if (*u.p == 'X' && u.p[1] == 'Z') { und_cat(args, "void"); u.p += 2; }
  else {
    BOOL first = TRUE;
    for (;;) {
      if (*u.p == '@') { u.p++; if (*u.p == 'Z') u.p++; break; }
      if (!*u.p) return FALSE;
      char t[UND_TEXT];
      und_type(&u, t);
      if (u.fail) return FALSE;
      if (!first) und_cat(args, ", ");
      first = FALSE;
      und_cat(args, t);
      if (!scmp(t, "...")) { if (*u.p == 'Z') u.p++; break; }
    }
  }
  if (!(flags & UNDNAME_NO_ACCESS_SPECIFIERS) && member) und_cat(text, access);
  if (is_static && member) und_cat(text, "static ");
  if (is_virtual && !(flags & UNDNAME_NO_MEMBER_TYPE)) und_cat(text, "virtual ");
  if (!(flags & UNDNAME_NO_FUNCTION_RETURNS) && ret[0]) { und_cat(text, ret); und_cat(text, " "); }
  if (!(flags & UNDNAME_NO_MS_KEYWORDS)) { und_cat(text, conv_name); und_cat(text, " "); }
  und_cat(text, name);
  if (!(flags & UNDNAME_NO_ARGUMENTS)) { und_cat(text, "("); und_cat(text, args); und_cat(text, ")"); }
  und_cat(text, cv_suffix);
  if (ptr64 && !(flags & UNDNAME_NO_MS_KEYWORDS)) und_cat(text, " __ptr64");
  scopy(out, size, text);
  return TRUE;
}

EXPORT DWORD WINAPI UnDecorateSymbolName(PCSTR name, PSTR out, DWORD max, DWORD flags) {
  if (!name || !out || !max) { SetLastError(ERROR_INVALID_PARAMETER); return 0; }
  char buffer[UND_TEXT];
  if (!undecorate(name, buffer, sizeof(buffer), flags)) {
    /* a name that cannot be decoded in full: the qualified name, as with UNDNAME_NAME_ONLY, if that much is there */
    if (name[0] != '?' || !undecorate(name, buffer, sizeof(buffer), flags | UNDNAME_NAME_ONLY)) { SetLastError(ERROR_INVALID_PARAMETER); return 0; }
  }
  scopy(out, max, buffer);
  return (DWORD)slen(out);
}
EXPORT DWORD WINAPI UnDecorateSymbolNameW(PCWSTR name, PWSTR out, DWORD max, DWORD flags) {
  char *a = to_ansi(name);
  char buffer[UND_TEXT];
  DWORD r = 0;
  if (a && out && max) {
    r = UnDecorateSymbolName(a, buffer, sizeof(buffer), flags);
    if (r) r = (DWORD)widen_into(out, max, buffer);
  } else SetLastError(ERROR_INVALID_PARAMETER);
  xfree(a);
  return r;
}
EXPORT BOOL WINAPI SymUnDName64(PIMAGEHLP_SYMBOL64 sym, PSTR out, DWORD max) {
  return sym && UnDecorateSymbolName(sym->Name, out, max, UNDNAME_COMPLETE) != 0;
}
EXPORT BOOL WINAPI SymUnDName(PIMAGEHLP_SYMBOL sym, PSTR out, DWORD max) {
  return sym && UnDecorateSymbolName(sym->Name, out, max, UNDNAME_COMPLETE) != 0;
}
