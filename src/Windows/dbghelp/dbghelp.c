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

/* dbghelp.dll for TheoryCraftOS: the image helpers (ImageNtHeader ...), the symbol handler, the x64 stack walker and minidumps. Everything is done in user mode over the
 * services that exist (ReadProcessMemory, VirtualQueryEx, the thread context calls, the process snapshot); no service of its own is needed.
 *
 * The symbol handler keeps, per process handle (SymInitialize), a list of modules. A module is loaded from the memory of the process (SymLoadModuleEx with a base address, or
 * SymInitialize with fInvadeProcess) or from its file. The symbols are the PE export table and the COFF symbol table of the file (what mingw's linker leaves in an image that
 * is not stripped); the unwind tables (.pdata) are kept for SymFunctionTableAccess64 and StackWalk64. There is no PDB, DWARF or CodeView reader, so there are no line
 * numbers, types, locals or source files: those calls fail the way they do for a module without debug information (ERROR_MOD_NOT_FOUND / ERROR_NOT_SUPPORTED).
 * The 32-bit variants of the functions are the same calls with 32-bit addresses; only the ones that need no new structure layout are here. Not here either: symbol servers
 * (SymSrv*, except SymSrvGetFileIndexInfo), the Windows debugger extension exports, MiniDumpWriteDump's callbacks and the full C++ name decoder of UnDecorateSymbolName (the common signatures only).
 * Built freestanding over kernel32.dll and ntdll.dll with the OS's mingw compiler. */
#define WIN32_LEAN_AND_MEAN
#define _IMAGEHLP_SOURCE_
#pragma GCC diagnostic ignored "-Wpacked-not-aligned"
#include <stddef.h>
#include <windows.h>
#include <winternl.h>
#include <tlhelp32.h>
#include <psapi.h>
#include "dbghelp.h"

#define EXPORT __declspec(dllexport)

__asm__(".text\n.globl ___chkstk_ms\n___chkstk_ms:\n ret\n");

#define ERR_MOD_NOT_FOUND ERROR_MOD_NOT_FOUND
#define SymTagFunction 5      /* (SymTagEnum of the DIA SDK) */
#define SymTagData 7
#define SymTagPublicSymbol 10
#define SYM_NAME_MAX 4096      /* longest symbol name taken from a file */
#define IMAGE_LIMIT (256u << 20)      /* largest image the symbol handler reads into memory */

/* ---- small helpers (the library has no C runtime) ---- */
static void *xalloc(SIZE_T n) { return HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, n ? n : 1); }
static void *xrealloc(void *p, SIZE_T n) { return p ? HeapReAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, p, n) : xalloc(n); }
static void xfree(void *p) { if (p) HeapFree(GetProcessHeap(), 0, p); }
static void mcopy(void *d, const void *s, SIZE_T n) { BYTE *a = d; const BYTE *b = s; while (n--) *a++ = *b++; }
static void mzero(void *d, SIZE_T n) { BYTE *a = d; while (n--) *a++ = 0; }
static SIZE_T slen(const char *s) { SIZE_T n = 0; while (s && s[n]) n++; return n; }
static SIZE_T wlen(const WCHAR *s) { SIZE_T n = 0; while (s && s[n]) n++; return n; }
static char lc(char c) { return c >= 'A' && c <= 'Z' ? (char)(c + 32) : c; }
static int sicmp(const char *a, const char *b) { while (*a && lc(*a) == lc(*b)) { a++; b++; } return (unsigned char)lc(*a) - (unsigned char)lc(*b); }
static int scmp(const char *a, const char *b) { while (*a && *a == *b) { a++; b++; } return (unsigned char)*a - (unsigned char)*b; }
static char *sdup(const char *s) { SIZE_T n = slen(s); char *r = xalloc(n + 1); if (r) mcopy(r, s, n); return r; }
/* Copies at most capacity - 1 characters and always terminates; returns the length of the source. */
static SIZE_T scopy(char *d, SIZE_T capacity, const char *s) {
  const SIZE_T n = slen(s);
  if (capacity) { const SIZE_T k = n < capacity - 1 ? n : capacity - 1; mcopy(d, s, k); d[k] = 0; }
  return n;
}
static BOOL is_sep(char c) { return c == '\\' || c == '/'; }
static const char *base_name(const char *path) {
  const char *r = path;
  for (const char *p = path; p && *p; p++) if (is_sep(*p) || *p == ':') r = p + 1;
  return r;
}

static char *to_ansi(const WCHAR *w) {
  if (!w) return NULL;
  const int n = WideCharToMultiByte(CP_ACP, 0, w, -1, NULL, 0, NULL, NULL);
  char *r = xalloc((SIZE_T)(n > 0 ? n : 1));
  if (r && n > 0) WideCharToMultiByte(CP_ACP, 0, w, -1, r, n, NULL, NULL);
  return r;
}
static WCHAR *to_wide(const char *a) {
  if (!a) return NULL;
  const int n = MultiByteToWideChar(CP_ACP, 0, a, -1, NULL, 0);
  WCHAR *r = xalloc((SIZE_T)(n > 0 ? n : 1) * sizeof(WCHAR));
  if (r && n > 0) MultiByteToWideChar(CP_ACP, 0, a, -1, r, n);
  return r;
}
/* Copies an ANSI string into a wide buffer of capacity characters (terminated); returns the length it would have. */
static SIZE_T widen_into(WCHAR *d, SIZE_T capacity, const char *s) {
  WCHAR *w = to_wide(s);
  SIZE_T n = wlen(w);
  if (capacity && d) { const SIZE_T k = n < capacity - 1 ? n : capacity - 1; if (w) mcopy(d, w, k * sizeof(WCHAR)); d[k] = 0; }
  xfree(w);
  return n;
}

/* A wildcard match (* and ?), case-insensitive: SymMatchString and the symbol enumerations. */
static BOOL match_pattern(const char *pattern, const char *text, BOOL case_sensitive) {
  while (*pattern) {
    if (*pattern == '*') {
      while (*pattern == '*') pattern++;
      if (!*pattern) return TRUE;
      for (; *text; text++) if (match_pattern(pattern, text, case_sensitive)) return TRUE;
      return FALSE;
    }
    if (!*text) return FALSE;
    if (*pattern != '?') {
      const char a = case_sensitive ? *pattern : lc(*pattern), b = case_sensitive ? *text : lc(*text);
      if (a != b) return FALSE;
    }
    pattern++;
    text++;
  }
  return !*text;
}

/* ---- the data of the symbol handler ---- */
#define SYMKIND_EXPORT 1
#define SYMKIND_COFF 2
#define SYMKIND_USER 3

typedef struct { DWORD64 address; DWORD size; DWORD kind; DWORD name; DWORD tag; } SYM;      /* name: offset in the module's pool */

typedef struct MODULE {
  struct MODULE *next;
  DWORD64 base;
  DWORD size, timestamp, checksum;
  WORD machine;
  SYM_TYPE type;
  BOOL is_virtual;
  char name[64], image[MAX_PATH], loaded[MAX_PATH];
  SYM *syms;      /* sorted by address */
  DWORD count, capacity;
  char *pool;
  DWORD pool_length, pool_capacity;
  RUNTIME_FUNCTION *pdata;      /* a copy of the exception directory (x64) */
  DWORD pdata_count;
} MODULE;

typedef struct SYMPROC {
  struct SYMPROC *next;
  HANDLE handle;
  MODULE *modules;
  char *search_path;
  PSYMBOL_REGISTERED_CALLBACK64 callback;
  ULONG64 callback_context;
  BOOL callback_wide;
  IMAGEHLP_STACK_FRAME frame;
} SYMPROC;

static CRITICAL_SECTION g_lock;
static SYMPROC *g_processes;
static DWORD g_options = SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS;
static char *g_home;

static SYMPROC *find_process(HANDLE h) {
  for (SYMPROC *p = g_processes; p; p = p->next) if (p->handle == h) return p;
  return NULL;
}
static void module_free(MODULE *m) {
  xfree(m->syms);
  xfree(m->pool);
  xfree(m->pdata);
  xfree(m);
}
static MODULE *module_containing(SYMPROC *p, DWORD64 address) {
  for (MODULE *m = p->modules; m; m = m->next) if (address >= m->base && address - m->base < (m->size ? m->size : 1)) return m;
  return NULL;
}
static MODULE *module_at_base(SYMPROC *p, DWORD64 base) {
  for (MODULE *m = p->modules; m; m = m->next) if (m->base == base) return m;
  return NULL;
}

static BOOL target_read(HANDLE h, DWORD64 address, void *buffer, SIZE_T n) {
  SIZE_T got = 0;
  return ReadProcessMemory(h, (LPCVOID)(ULONG_PTR)address, buffer, n, &got) && got == n;
}

/* ---- the symbols of a module ---- */
static const char *sym_name(const MODULE *m, const SYM *s) { return m->pool + s->name; }

static BOOL module_add(MODULE *m, DWORD64 address, DWORD size, DWORD kind, const char *name, SIZE_T length, DWORD tag) {
  if (length == 0 || length >= SYM_NAME_MAX) return FALSE;
  if (m->count == m->capacity) {
    const DWORD cap = m->capacity ? m->capacity * 2 : 256;
    SYM *s = xrealloc(m->syms, (SIZE_T)cap * sizeof(SYM));
    if (!s) return FALSE;
    m->syms = s;
    m->capacity = cap;
  }
  if (length + 1 > (SIZE_T)(0xFFFFFFFFu - m->pool_length)) return FALSE;
  if (m->pool_length + length + 1 > m->pool_capacity) {
    DWORD cap = m->pool_capacity ? m->pool_capacity : 4096;
    while (cap < m->pool_length + length + 1) cap *= 2;
    char *p = xrealloc(m->pool, cap);
    if (!p) return FALSE;
    m->pool = p;
    m->pool_capacity = cap;
  }
  mcopy(m->pool + m->pool_length, name, length);
  m->pool[m->pool_length + length] = 0;
  SYM *s = &m->syms[m->count++];
  s->address = address;
  s->size = size;
  s->kind = kind;
  s->name = m->pool_length;
  s->tag = tag;
  m->pool_length += (DWORD)length + 1;
  return TRUE;
}

static int sym_less(const MODULE *m, const SYM *a, const SYM *b) {
  if (a->address != b->address) return a->address < b->address;
  if (a->kind != b->kind) return a->kind > b->kind;      /* user and COFF symbols before the exports at the same address */
  return scmp(sym_name(m, a), sym_name(m, b)) < 0;
}
static void module_sort(MODULE *m) {
  /* shell sort: no recursion and no allocation */
  DWORD gap = 1;
  while (gap < m->count / 3) gap = gap * 3 + 1;
  for (; gap > 0; gap = (gap - 1) / 3) {
    for (DWORD i = gap; i < m->count; i++) {
      const SYM t = m->syms[i];
      DWORD j = i;
      while (j >= gap && sym_less(m, &t, &m->syms[j - gap])) { m->syms[j] = m->syms[j - gap]; j -= gap; }
      m->syms[j] = t;
    }
    if (gap == 1) break;
  }
}
/* Sizes of the COFF symbols: up to the next symbol, if that is not further away than the end of its section. */
static void module_fix_sizes(MODULE *m) {
  for (DWORD i = 0; i + 1 < m->count; i++) {
    SYM *s = &m->syms[i];
    if (s->kind == SYMKIND_COFF && s->size == 0) {
      DWORD j = i + 1;
      while (j < m->count && m->syms[j].address == s->address) j++;
      if (j < m->count && m->syms[j].address - s->address < 0x100000) s->size = (DWORD)(m->syms[j].address - s->address);
    }
  }
}

/* ---- reading PE images ---- */
typedef struct {
  BOOL ok, is64;
  WORD machine, nsections;
  DWORD64 image_base;
  DWORD size_of_image, size_of_headers, checksum, timestamp, ndirs, symtab, nsyms;
  const IMAGE_SECTION_HEADER *sections;
  const IMAGE_DATA_DIRECTORY *dirs;
} PEINFO;

static BOOL pe_parse(const BYTE *d, SIZE_T n, PEINFO *p) {
  mzero(p, sizeof(*p));
  if (n < sizeof(IMAGE_DOS_HEADER) || d[0] != 'M' || d[1] != 'Z') return FALSE;
  const DWORD e_lfanew = *(const DWORD *)(d + 0x3c);
  if (e_lfanew > n || n - e_lfanew < 4 + sizeof(IMAGE_FILE_HEADER) + 2 || *(const DWORD *)(d + e_lfanew) != 0x00004550) return FALSE;
  const IMAGE_FILE_HEADER *fh = (const IMAGE_FILE_HEADER *)(d + e_lfanew + 4);
  const BYTE *opt = (const BYTE *)(fh + 1);
  const SIZE_T opt_have = n - (SIZE_T)(opt - d);
  const WORD magic = *(const WORD *)opt;
  p->machine = fh->Machine;
  p->nsections = fh->NumberOfSections;
  p->timestamp = fh->TimeDateStamp;
  p->symtab = fh->PointerToSymbolTable;
  p->nsyms = fh->NumberOfSymbols;
  if (magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
    if (opt_have < sizeof(IMAGE_OPTIONAL_HEADER64) - sizeof(IMAGE_DATA_DIRECTORY) * IMAGE_NUMBEROF_DIRECTORY_ENTRIES) return FALSE;
    const IMAGE_OPTIONAL_HEADER64 *o = (const IMAGE_OPTIONAL_HEADER64 *)opt;
    p->is64 = TRUE;
    p->image_base = o->ImageBase;
    p->size_of_image = o->SizeOfImage;
    p->size_of_headers = o->SizeOfHeaders;
    p->checksum = o->CheckSum;
    p->ndirs = o->NumberOfRvaAndSizes;
    p->dirs = o->DataDirectory;
  } else if (magic == IMAGE_NT_OPTIONAL_HDR32_MAGIC) {
    if (opt_have < sizeof(IMAGE_OPTIONAL_HEADER32) - sizeof(IMAGE_DATA_DIRECTORY) * IMAGE_NUMBEROF_DIRECTORY_ENTRIES) return FALSE;
    const IMAGE_OPTIONAL_HEADER32 *o = (const IMAGE_OPTIONAL_HEADER32 *)opt;
    p->image_base = o->ImageBase;
    p->size_of_image = o->SizeOfImage;
    p->size_of_headers = o->SizeOfHeaders;
    p->checksum = o->CheckSum;
    p->ndirs = o->NumberOfRvaAndSizes;
    p->dirs = o->DataDirectory;
  } else return FALSE;
  if (p->ndirs > IMAGE_NUMBEROF_DIRECTORY_ENTRIES) p->ndirs = IMAGE_NUMBEROF_DIRECTORY_ENTRIES;
  const SIZE_T dirs_offset = (SIZE_T)((const BYTE *)p->dirs - d);
  if (dirs_offset > n || (n - dirs_offset) / sizeof(IMAGE_DATA_DIRECTORY) < p->ndirs) return FALSE;
  const SIZE_T sections_offset = (SIZE_T)(opt - d) + fh->SizeOfOptionalHeader;
  if (sections_offset > n || (n - sections_offset) / sizeof(IMAGE_SECTION_HEADER) < p->nsections) return FALSE;
  p->sections = (const IMAGE_SECTION_HEADER *)(d + sections_offset);
  p->ok = TRUE;
  return TRUE;
}

/* The bytes of the image at an RVA, if the whole range is inside the image laid out in memory. */
static const BYTE *img_at(const BYTE *image, DWORD size, DWORD rva, DWORD length) {
  if (rva > size || length > size - rva) return NULL;
  return image + rva;
}
static BOOL img_string(const BYTE *image, DWORD size, DWORD rva, const char **out, SIZE_T *length) {
  if (rva >= size) return FALSE;
  SIZE_T n = 0;
  while (rva + n < size && image[rva + n]) n++;
  if (rva + n >= size) return FALSE;
  *out = (const char *)image + rva;
  *length = n;
  return TRUE;
}

static void load_exports(MODULE *m, const BYTE *image, DWORD size, const PEINFO *pe) {
  if (pe->ndirs <= IMAGE_DIRECTORY_ENTRY_EXPORT) return;
  const IMAGE_DATA_DIRECTORY dir = pe->dirs[IMAGE_DIRECTORY_ENTRY_EXPORT];
  const IMAGE_EXPORT_DIRECTORY *ex = (const IMAGE_EXPORT_DIRECTORY *)img_at(image, size, dir.VirtualAddress, sizeof(IMAGE_EXPORT_DIRECTORY));
  if (!dir.VirtualAddress || !ex) return;
  if (ex->NumberOfNames > 0x100000 || ex->NumberOfFunctions > 0x100000) return;
  const DWORD *names = (const DWORD *)img_at(image, size, ex->AddressOfNames, ex->NumberOfNames * 4);
  const WORD *ordinals = (const WORD *)img_at(image, size, ex->AddressOfNameOrdinals, ex->NumberOfNames * 2);
  const DWORD *functions = (const DWORD *)img_at(image, size, ex->AddressOfFunctions, ex->NumberOfFunctions * 4);
  if (!functions) return;
  if (names && ordinals) {
    for (DWORD i = 0; i < ex->NumberOfNames; i++) {
      if (ordinals[i] >= ex->NumberOfFunctions) continue;
      const DWORD rva = functions[ordinals[i]];
      if (!rva || (rva >= dir.VirtualAddress && rva - dir.VirtualAddress < dir.Size)) continue;      /* (a forwarder has no address here) */
      const char *name;
      SIZE_T length;
      if (!img_string(image, size, names[i], &name, &length)) continue;
      module_add(m, m->base + rva, 0, SYMKIND_EXPORT, name, length, SymTagPublicSymbol);
    }
  }
}

/* The COFF symbol table of a file: functions and data with a name, at the address of their section plus the value. */
static void load_coff(MODULE *m, HANDLE file, const PEINFO *pe, const IMAGE_SECTION_HEADER *sections) {
  if (!pe->symtab || !pe->nsyms || pe->nsyms > 4000000) return;
  const SIZE_T bytes = (SIZE_T)pe->nsyms * 18;
  BYTE *table = xalloc(bytes);
  if (!table) return;
  LARGE_INTEGER pos;
  DWORD got = 0;
  pos.QuadPart = pe->symtab;
  if (!SetFilePointerEx(file, pos, NULL, FILE_BEGIN) || !ReadFile(file, table, (DWORD)bytes, &got, NULL) || got != bytes) { xfree(table); return; }
  DWORD strings_size = 0;
  char *strings = NULL;
  if (ReadFile(file, &strings_size, 4, &got, NULL) && got == 4 && strings_size > 4 && strings_size < (64u << 20)) {
    strings = xalloc(strings_size + 1);
    if (strings) {
      mcopy(strings, &strings_size, 4);
      if (!ReadFile(file, strings + 4, strings_size - 4, &got, NULL) || got != strings_size - 4) { xfree(strings); strings = NULL; strings_size = 0; }
    }
  } else strings_size = 0;
  for (DWORD i = 0; i < pe->nsyms; i++) {
    const BYTE *e = table + (SIZE_T)i * 18;
    const WORD section = *(const WORD *)(e + 12), type = *(const WORD *)(e + 14);
    const BYTE storage = e[16], aux = e[17];
    const DWORD value = *(const DWORD *)(e + 8);
    if ((storage == IMAGE_SYM_CLASS_EXTERNAL || storage == IMAGE_SYM_CLASS_STATIC) && section > 0 && section <= pe->nsections) {
      const IMAGE_SECTION_HEADER *s = &sections[section - 1];
      char short_name[9];
      const char *name = NULL;
      SIZE_T length = 0;
      if (*(const DWORD *)e == 0) {
        const DWORD off = *(const DWORD *)(e + 4);
        if (strings && off < strings_size) { name = strings + off; while (off + length < strings_size && name[length]) length++; }
      } else {
        mcopy(short_name, e, 8);
        short_name[8] = 0;
        name = short_name;
        length = slen(short_name);
      }
      /* section names ('.text') and compiler labels are not symbols; a static symbol is the name of the section when its value is 0 and it has an auxiliary entry */
      const BOOL function = (type & 0xF0) == 0x20;
      if (name && length && name[0] != '.' && !(storage == IMAGE_SYM_CLASS_STATIC && !function && aux))
        module_add(m, m->base + s->VirtualAddress + value,
                   0, SYMKIND_COFF, name, length, function ? SymTagFunction : SymTagData);
    }
    i += aux;
  }
  xfree(strings);
  xfree(table);
}

/* Lays a file out the way the loader does, to read its sections by RVA. Returns the image (free with xfree), its size and its headers. */
static BYTE *file_to_image(HANDLE file, DWORD *size_out, PEINFO *pe_out, BYTE **headers_out) {
  BYTE *headers = xalloc(0x1000);
  DWORD got = 0;
  if (!headers) return NULL;
  LARGE_INTEGER zero;
  zero.QuadPart = 0;
  if (!SetFilePointerEx(file, zero, NULL, FILE_BEGIN) || !ReadFile(file, headers, 0x1000, &got, NULL) || got < 64) { xfree(headers); return NULL; }
  PEINFO pe;
  if (!pe_parse(headers, got, &pe) || pe.size_of_image == 0 || pe.size_of_image > IMAGE_LIMIT) {
    /* the headers may be larger than a page */
    xfree(headers);
    return NULL;
  }
  BYTE *image = xalloc(pe.size_of_image);
  if (!image) { xfree(headers); return NULL; }
  const DWORD header_bytes = pe.size_of_headers < pe.size_of_image ? pe.size_of_headers : pe.size_of_image;
  zero.QuadPart = 0;
  SetFilePointerEx(file, zero, NULL, FILE_BEGIN);
  ReadFile(file, image, header_bytes, &got, NULL);
  if (!pe_parse(image, header_bytes, &pe)) { xfree(image); xfree(headers); return NULL; }
  for (WORD i = 0; i < pe.nsections; i++) {
    const IMAGE_SECTION_HEADER *s = &pe.sections[i];
    DWORD length = s->SizeOfRawData < s->Misc.VirtualSize || !s->Misc.VirtualSize ? s->SizeOfRawData : s->Misc.VirtualSize;
    if (!length || s->VirtualAddress >= pe.size_of_image) continue;
    if (length > pe.size_of_image - s->VirtualAddress) length = pe.size_of_image - s->VirtualAddress;
    LARGE_INTEGER at;
    at.QuadPart = s->PointerToRawData;
    if (SetFilePointerEx(file, at, NULL, FILE_BEGIN)) ReadFile(file, image + s->VirtualAddress, length, &got, NULL);
  }
  *size_out = pe.size_of_image;
  *pe_out = pe;
  if (headers_out) *headers_out = headers; else xfree(headers);
  return image;
}

/* Reads a loaded image out of the memory of a process (unreadable pages stay zero). */
static BYTE *memory_to_image(HANDLE process, DWORD64 base, DWORD *size_out, PEINFO *pe_out) {
  BYTE head[0x1000];
  PEINFO pe;
  if (!target_read(process, base, head, sizeof(head))) {
    /* a module whose headers are shorter than a page from the end of its mapping */
    SIZE_T got = 0;
    if (!ReadProcessMemory(process, (LPCVOID)(ULONG_PTR)base, head, sizeof(head), &got) || got < 64) return NULL;
    mzero(head + got, sizeof(head) - got);
  }
  if (!pe_parse(head, sizeof(head), &pe) || pe.size_of_image == 0 || pe.size_of_image > IMAGE_LIMIT) return NULL;
  BYTE *image = xalloc(pe.size_of_image);
  if (!image) return NULL;
  if (!target_read(process, base, image, pe.size_of_image)) {
    for (DWORD off = 0; off < pe.size_of_image; off += 0x1000) {
      const DWORD n = pe.size_of_image - off < 0x1000 ? pe.size_of_image - off : 0x1000;
      if (!target_read(process, base + off, image + off, n)) mzero(image + off, n);
    }
  }
  *size_out = pe.size_of_image;
  *pe_out = pe;
  return image;
}

static void module_set_name(MODULE *m, const char *module_name, const char *image_name) {
  if (image_name) { scopy(m->image, sizeof(m->image), image_name); scopy(m->loaded, sizeof(m->loaded), image_name); }
  if (module_name && *module_name) scopy(m->name, sizeof(m->name), module_name);
  else {
    scopy(m->name, sizeof(m->name), base_name(m->image));
    char *dot = NULL;
    for (char *p = m->name; *p; p++) if (*p == '.') dot = p;
    if (dot) *dot = 0;
  }
}

static void module_take_pdata(MODULE *m, const BYTE *image, DWORD size, const PEINFO *pe) {
  if (!pe->is64 || pe->machine != IMAGE_FILE_MACHINE_AMD64 || pe->ndirs <= IMAGE_DIRECTORY_ENTRY_EXCEPTION) return;
  const IMAGE_DATA_DIRECTORY d = pe->dirs[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
  const BYTE *p = d.VirtualAddress ? img_at(image, size, d.VirtualAddress, d.Size) : NULL;
  if (!p || d.Size < sizeof(RUNTIME_FUNCTION)) return;
  m->pdata = xalloc(d.Size);
  if (!m->pdata) return;
  mcopy(m->pdata, p, d.Size);
  m->pdata_count = d.Size / sizeof(RUNTIME_FUNCTION);
}

/* Makes a module of a process from the memory of the process and/or the file (either may be missing). Returns NULL with the last error set when there is neither. The module is not
 * in the process's list yet (module_link). */
static MODULE *module_load(SYMPROC *proc, DWORD64 base, DWORD size_hint, const char *image_name, const char *module_name, BOOL virtual_only) {
  MODULE *m = xalloc(sizeof(MODULE));
  if (!m) { SetLastError(ERROR_OUTOFMEMORY); return NULL; }
  module_set_name(m, module_name, image_name);
  m->base = base;
  m->size = size_hint;
  m->type = SymNone;
  m->is_virtual = virtual_only;
  BYTE *image = NULL;
  DWORD image_size = 0;
  PEINFO pe;
  BYTE *headers = NULL;
  HANDLE file = INVALID_HANDLE_VALUE;
  if (!virtual_only) {
    if (base) image = memory_to_image(proc->handle, base, &image_size, &pe);
    if (image_name && *image_name) {
      file = CreateFileA(image_name, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
      if (file != INVALID_HANDLE_VALUE && !image) {
        image = file_to_image(file, &image_size, &pe, &headers);
        if (image && !base) { m->base = pe.image_base; base = pe.image_base; }
      }
    }
  }
  if (!image && !virtual_only && (!size_hint || !base)) {
    if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    xfree(headers);
    module_free(m);
    SetLastError(file == INVALID_HANDLE_VALUE && image_name ? (base ? ERROR_FILE_NOT_FOUND : ERROR_NO_MORE_FILES) : ERROR_MOD_NOT_FOUND);
    return NULL;
  }
  if (image) {
    if (!m->size) m->size = image_size;      /* (a size the caller gives is the one that counts) */
    m->timestamp = pe.timestamp;
    m->checksum = pe.checksum;
    m->machine = pe.machine;
    load_exports(m, image, image_size, &pe);
    if (m->count) m->type = SymExport;
    module_take_pdata(m, image, image_size, &pe);
    if (file != INVALID_HANDLE_VALUE) {
      /* the section headers of the memory image are the ones the file has, so the same symbol table applies */
      PEINFO filepe;
      BYTE fh[0x1000];
      DWORD got = 0;
      LARGE_INTEGER zero;
      zero.QuadPart = 0;
      if (SetFilePointerEx(file, zero, NULL, FILE_BEGIN) && ReadFile(file, fh, sizeof(fh), &got, NULL) && pe_parse(fh, got, &filepe) && filepe.symtab && filepe.nsections == pe.nsections) {
        const DWORD before = m->count;
        load_coff(m, file, &filepe, filepe.sections);
        if (m->count > before) m->type = SymCoff;
      }
    }
  }
  if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
  xfree(image);
  xfree(headers);
  module_sort(m);
  module_fix_sizes(m);
  return m;
}

/* Puts a module in the list of a process. A module whose base lies inside an older one replaces it; one that begins below an older module stays beside it. Fails (the module is freed,
 * the last error set) if a module has that base already. */
static BOOL module_link(SYMPROC *proc, MODULE *m) {
  if (module_at_base(proc, m->base)) { module_free(m); SetLastError(ERROR_INVALID_ADDRESS); return FALSE; }
  for (MODULE **pp = &proc->modules; *pp;) {
    MODULE *e = *pp;
    if (m->base > e->base && m->base - e->base < e->size) { *pp = e->next; module_free(e); } else pp = &e->next;
  }
  m->next = proc->modules;
  proc->modules = m;
  return TRUE;
}

/* Finds the module of a process that holds an address and loads it from the process if the symbol handler does not know it yet. */
static MODULE *module_for_address(SYMPROC *proc, DWORD64 address, BOOL autoload) {
  MODULE *m = module_containing(proc, address);
  if (m || !autoload) return m;
  MEMORY_BASIC_INFORMATION mbi;
  if (!VirtualQueryEx(proc->handle, (LPCVOID)(ULONG_PTR)address, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT || mbi.Type != MEM_IMAGE) return NULL;
  const DWORD64 base = (DWORD64)(ULONG_PTR)mbi.AllocationBase;
  if (module_at_base(proc, base)) return NULL;
  WCHAR wpath[MAX_PATH];
  char path[MAX_PATH];
  path[0] = 0;
  if (K32GetModuleFileNameExW(proc->handle, (HMODULE)(ULONG_PTR)base, wpath, MAX_PATH)) {
    char *a = to_ansi(wpath);
    if (a) { scopy(path, sizeof(path), a); xfree(a); }
  }
  MODULE *loaded = module_load(proc, base, 0, path[0] ? path : NULL, NULL, FALSE);
  return loaded && module_link(proc, loaded) ? module_at_base(proc, base) : NULL;
}

/* Loads every module that the process has (SymInitialize with fInvadeProcess, SymRefreshModuleList). */
static BOOL load_process_modules(SYMPROC *proc) {
  HMODULE modules[1024];
  DWORD needed = 0;
  if (!K32EnumProcessModules(proc->handle, modules, sizeof(modules), &needed)) return FALSE;
  const DWORD count = needed / sizeof(HMODULE) < 1024 ? needed / sizeof(HMODULE) : 1024;
  for (DWORD i = 0; i < count; i++) {
    const DWORD64 base = (DWORD64)(ULONG_PTR)modules[i];
    if (module_at_base(proc, base)) continue;
    WCHAR wpath[MAX_PATH];
    char *path = NULL;
    if (K32GetModuleFileNameExW(proc->handle, modules[i], wpath, MAX_PATH)) path = to_ansi(wpath);
    MODULE *loaded = module_load(proc, base, 0, path, NULL, FALSE);
    if (loaded) module_link(proc, loaded);
    xfree(path);
  }
  return TRUE;
}

/* ---- DLL entry ---- */
BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved) {
  if (reason == DLL_PROCESS_ATTACH) InitializeCriticalSection(&g_lock);
  return TRUE;
}

#include "image.inc"
#include "symbols.inc"
#include "lookup.inc"
#include "stack.inc"
#include "dump.inc"
#include "undname.inc"
#include "paths.inc"
