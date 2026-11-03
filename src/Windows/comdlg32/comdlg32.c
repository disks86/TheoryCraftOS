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

/* comdlg32.dll for TheoryCraftOS: the common dialogs. Windows draws them as windows of the program; this OS has no window system for programs (user32.dll and gdi32.dll are
 * not implemented), so the dialogs that need a person - Open, Save As and the colour chooser - are shown by the desktop and the program's thread waits for the answer
 * (NtTcosCommonDialog, src/Process/WindowsDialogs.cpp). The rest of comdlg32 is done here exactly as Windows does it: checking the structures (the CDERR_* codes of
 * CommDlgExtendedError, the same for every dialog), the filter lists, the default extension, the file title, and the answer written back into the program's structure in ANSI or
 * Unicode form. Not possible without windows: hook procedures and templates (OFN_ENABLEHOOK, CC_ENABLEHOOK, ... are accepted but the hook is never called), the modeless Find and
 * Replace dialogs (they fail with CDERR_DIALOGFAILURE), the font chooser (there are no fonts: CFERR_NOFONTS), and printing (there is no print spooler for Windows programs:
 * PDERR_NODEFAULTPRN). Built freestanding over kernel32.dll with the OS's mingw compiler. */
#include "../tcos_win.h"

__asm__(".text\n.globl ___chkstk_ms\n___chkstk_ms:\n ret\n");

IMPORT HLOCAL WINAPI LocalAlloc(UINT flags, SIZE_T bytes);
IMPORT HLOCAL WINAPI LocalFree(HLOCAL memory);
IMPORT DWORD WINAPI GetCurrentThreadId(void);
IMPORT void WINAPI SetLastError(DWORD error);
IMPORT DWORD WINAPI GetFileAttributesW(LPCWSTR path);
IMPORT DWORD WINAPI GetFullPathNameW(LPCWSTR name, DWORD capacity, LPWSTR buffer, LPWSTR *file_part);
IMPORT DWORD WINAPI GetCurrentDirectoryW(DWORD capacity, LPWSTR buffer);
IMPORT BOOL WINAPI SetCurrentDirectoryW(LPCWSTR path);
IMPORT int WINAPI MultiByteToWideChar(UINT code_page, DWORD flags, LPCSTR source, int length, LPWSTR dest, int capacity);
IMPORT int WINAPI WideCharToMultiByte(UINT code_page, DWORD flags, LPCWSTR source, int length, LPSTR dest, int capacity, LPCSTR default_char, LPBOOL used_default);
IMPORT NTSTATUS NTAPI NtTcosCommonDialog(void *request);

/* ---- the codes of cderr.h ------------------------------------------------------------------------------------------------------------------------------- */
#define CDERR_DIALOGFAILURE 0xFFFFu
#define CDERR_STRUCTSIZE 0x0001u
#define CDERR_INITIALIZATION 0x0002u
#define CDERR_NOTEMPLATE 0x0003u
#define CDERR_NOHINSTANCE 0x0004u
#define CDERR_FINDRESFAILURE 0x0006u
#define CDERR_MEMALLOCFAILURE 0x0009u
#define CDERR_NOHOOK 0x000Bu
#define PDERR_NODEFAULTPRN 0x1008u
#define CFERR_NOFONTS 0x2001u
#define FNERR_BUFFERTOOSMALL 0x3003u
#define FRERR_BUFFERLENGTHZERO 0x4001u
#define E_INVALIDARG_ ((LONG)0x80070057)
#define E_HANDLE_ ((LONG)0x80070006)
#define E_FAIL_ ((LONG)0x80004005)

/* ---- CommDlgExtendedError: the error of the last dialog call of the thread ----------------------------------------------------------------------------------- */
#define ERROR_SLOTS 32
static struct { DWORD thread; DWORD code; } g_errors[ERROR_SLOTS];
static volatile LONG g_error_lock;
static unsigned g_error_next;

static void lock_errors(void) { while (__atomic_exchange_n(&g_error_lock, 1, __ATOMIC_ACQUIRE)) {} }
static void unlock_errors(void) { __atomic_store_n(&g_error_lock, 0, __ATOMIC_RELEASE); }

static void set_error(DWORD code) {
  const DWORD me = GetCurrentThreadId();
  lock_errors();
  for (unsigned i = 0; i < ERROR_SLOTS; ++i)
    if (g_errors[i].thread == me) { g_errors[i].code = code; unlock_errors(); return; }
  const unsigned slot = g_error_next++ % ERROR_SLOTS;      /* (the oldest thread loses its code when more than ERROR_SLOTS threads use dialogs) */
  g_errors[slot].thread = me;
  g_errors[slot].code = code;
  unlock_errors();
}

EXPORT DWORD WINAPI CommDlgExtendedError(void) {
  const DWORD me = GetCurrentThreadId();
  DWORD code = 0;
  lock_errors();
  for (unsigned i = 0; i < ERROR_SLOTS; ++i) if (g_errors[i].thread == me) code = g_errors[i].code;
  unlock_errors();
  return code;
}

/* ---- strings --------------------------------------------------------------------------------------------------------------------------------------------- */
static int wlen(const WCHAR *s) { int n = 0; while (s[n]) ++n; return n; }
static int alen(const char *s) { int n = 0; while (s[n]) ++n; return n; }
static WCHAR lower(WCHAR c) { return c >= 'A' && c <= 'Z' ? (WCHAR)(c + 32) : c; }
static int is_separator(WCHAR c) { return c == '\\' || c == '/'; }

static void *heap_get(SIZE_T bytes) { return LocalAlloc(0x40 /* LPTR */, bytes ? bytes : 1); }
static void heap_put(void *p) { if (p) LocalFree(p); }

/* A copy of the program's string as wide text (NULL stays NULL). */
static WCHAR *wide_copy(const void *text, int wide) {
  if (!text) return NULL;
  if (wide) {
    const int n = wlen(text);
    WCHAR *r = heap_get((SIZE_T)(n + 1) * 2);
    if (r) for (int i = 0; i <= n; ++i) r[i] = ((const WCHAR *)text)[i];
    return r;
  }
  const int n = alen(text);
  WCHAR *r = heap_get((SIZE_T)(n + 1) * 2);
  if (r && n) MultiByteToWideChar(0, 0, text, n, r, n);
  return r;
}

/* A copy of a list of strings that ends with an empty string ("a\0b\0\0"), as wide text. `*units` is how many characters it has with the terminators. */
static WCHAR *wide_list_copy(const void *list, int wide, int *units) {
  *units = 0;
  if (!list) return NULL;
  int total = 0;
  if (wide) {
    const WCHAR *p = list;
    while (p[total]) total += wlen(p + total) + 1;
    ++total;
  } else {
    const char *p = list;
    while (p[total]) total += alen(p + total) + 1;
    ++total;
  }
  WCHAR *r = heap_get((SIZE_T)(total + 1) * 2);
  if (!r) return NULL;
  if (wide) for (int i = 0; i < total; ++i) r[i] = ((const WCHAR *)list)[i];
  else MultiByteToWideChar(0, 0, list, total, r, total);
  *units = total;
  return r;
}

/* Writes `text` (wide, NUL-terminated) to the program's buffer of `capacity` characters, as wide or ANSI text; false if it does not fit. `*needed` is the size it takes. */
static int store_text(void *destination, DWORD capacity, int wide, const WCHAR *text, DWORD *needed) {
  const int n = wlen(text);
  DWORD size = (DWORD)n + 1;
  if (!wide) {
    const int bytes = WideCharToMultiByte(0, 0, text, n + 1, NULL, 0, NULL, NULL);
    size = bytes > 0 ? (DWORD)bytes : (DWORD)n + 1;
  }
  if (needed) *needed = size;
  if (!destination || size > capacity) return 0;
  if (wide) { WCHAR *d = destination; for (int i = 0; i <= n; ++i) d[i] = text[i]; }
  else WideCharToMultiByte(0, 0, text, n + 1, destination, (int)size, NULL, NULL);
  return 1;
}

/* ---- GetFileTitle ---------------------------------------------------------------------------------------------------------------------------------------- *
 * The name of the file without its folder: 0 and the title in the buffer; the size the buffer needs (with the NUL) if it is too small; -1 for a name that is no file name. */
static short file_title(const WCHAR *path, WCHAR *title, WORD capacity) {
  if (!path) return -1;
  const int n = wlen(path);
  if (n == 0) return -1;
  if (path[n - 1] == '\\' || path[n - 1] == '/' || path[n - 1] == ':') return -1;      /* a folder or a drive */
  for (int i = 0; i < n; ++i) if (path[i] == '*' || path[i] == '[' || path[i] == ']') return -1;
  int start = 0;
  for (int i = 0; i < n; ++i) if (is_separator(path[i]) || path[i] == ':') start = i + 1;
  const int size = n - start + 1;
  if (!title || capacity < size) return (short)size;
  for (int i = 0; i < size; ++i) title[i] = path[start + i];
  return 0;
}

EXPORT short WINAPI GetFileTitleW(LPCWSTR path, LPWSTR title, WORD capacity) { return file_title(path, title, capacity); }
EXPORT short WINAPI GetFileTitleA(LPCSTR path, LPSTR title, WORD capacity) {
  if (!path) return -1;
  WCHAR *wide = wide_copy(path, 0);
  if (!wide) return -1;
  /* (the size is counted in the ANSI characters) */
  short result = -1;
  const int n = wlen(wide);
  WCHAR *out = heap_get((SIZE_T)(n + 1) * 2);
  if (out) {
    result = file_title(wide, out, (WORD)(n + 1));
    if (result == 0) {
      DWORD needed = 0;
      if (!store_text(title, capacity, 0, out, &needed)) result = (short)needed;
    }
  }
  heap_put(out);
  heap_put(wide);
  return result;
}

/* ---- the file dialogs ------------------------------------------------------------------------------------------------------------------------------------- */
typedef struct {
  DWORD lStructSize;
  HANDLE hwndOwner, hInstance;
  const void *lpstrFilter;
  void *lpstrCustomFilter;
  DWORD nMaxCustFilter, nFilterIndex;
  void *lpstrFile;
  DWORD nMaxFile;
  void *lpstrFileTitle;
  DWORD nMaxFileTitle;
  const void *lpstrInitialDir, *lpstrTitle;
  DWORD Flags;
  WORD nFileOffset, nFileExtension;
  const void *lpstrDefExt;
  LPARAM lCustData;
  void *lpfnHook;
  const void *lpTemplateName;
  void *pvReserved;
  DWORD dwReserved, FlagsEx;
} OFN;
#define OFN_SIZE_VERSION_400 136u
#define OFN_SIZE 152u
#define OFN_NOCHANGEDIR 0x8u
#define OFN_ENABLEHOOK 0x20u
#define OFN_EXTENSIONDIFFERENT 0x400u
#define OFN_READONLY 0x1u
#define PATH_UNITS 32768      /* the longest path a dialog hands back */

typedef struct {      /* the same layout as TCOS_COMMON_DIALOG of src/Windows/tcos_syscalls.h and src/Process/WindowsDialogs.hpp */
  DWORD size, kind, flags, filter_index, color, color_out, accepted, reserved;
  const WCHAR *title, *start, *filters;
  WCHAR *result;
  DWORD result_capacity, result_length;
} CommonDialog;
#define DIALOG_OPEN 1
#define DIALOG_SAVE 2
#define DIALOG_COLOR 3

/* What the kernel's file dialog takes of a pattern list ("*.txt;*.md"): the extensions without "*." ("txt;md"), "*" for every file or for a pattern it cannot express. */
static int append_extensions(const WCHAR *patterns, WCHAR *out) {
  int n = 0;
  int any = 0;
  const WCHAR *p = patterns;
  while (*p) {
    while (*p == ' ') ++p;
    const WCHAR *token = p;
    while (*p && *p != ';') ++p;
    const WCHAR *end = p;
    while (end > token && end[-1] == ' ') --end;
    if (*p == ';') ++p;
    const int length = (int)(end - token);
    if (length == 0) continue;
    if ((length == 1 && token[0] == '*') || (length == 3 && token[0] == '*' && token[1] == '.' && token[2] == '*')) { out[0] = '*'; out[1] = 0; return 1; }
    int simple = length > 2 && token[0] == '*' && token[1] == '.';
    for (int i = 2; simple && i < length; ++i) if (token[i] == '*' || token[i] == '?' || token[i] == '.' || token[i] == '\\' || token[i] == '/') simple = 0;
    if (!simple) { out[0] = '*'; out[1] = 0; return 1; }
    if (any) out[n++] = ';';
    for (int i = 2; i < length; ++i) out[n++] = token[i];
    any = 1;
  }
  if (!any) out[n++] = '*';
  out[n] = 0;
  return n;
}

/* The filter list of the program (pairs "name\0patterns\0", ended by an empty name) in the form of NtTcosCommonDialog: the same pairs, with the patterns reduced to extensions. */
static WCHAR *dialog_filters(const WCHAR *list, int units) {
  if (!list || !*list) return NULL;
  WCHAR *out = heap_get((SIZE_T)(units + 4) * 2);
  if (!out) return NULL;
  int o = 0;
  const WCHAR *p = list;
  while (*p) {
    const int name = wlen(p);
    for (int i = 0; i <= name; ++i) out[o++] = p[i];
    p += name + 1;
    o += append_extensions(p, out + o) + 1;
    p += wlen(p) + 1;
  }
  out[o++] = 0;
  return out;
}

/* The patterns of filter number `index` (1-based) of the program's list, or NULL. */
static const WCHAR *filter_patterns(const WCHAR *list, DWORD index) {
  if (!list) return NULL;
  const WCHAR *p = list;
  for (DWORD i = 1; *p; ++i) {
    p += wlen(p) + 1;
    if (i == index) return p;
    p += wlen(p) + 1;
  }
  return NULL;
}

/* The extension a filter stands for: -1 nothing (the pattern ends in a dot), 0 none that is certain (a wildcard), 1 `out` has it (the first pattern's, without the dot). */
static int filter_extension(const WCHAR *patterns, WCHAR *out) {
  out[0] = 0;
  if (!patterns) return 0;
  const WCHAR *p = patterns;
  while (*p == ' ') ++p;
  const WCHAR *end = p;
  while (*end && *end != ';') ++end;
  const WCHAR *dot = NULL;
  for (const WCHAR *c = p; c < end; ++c) if (*c == '.') { dot = c; break; }
  if (!dot) return 0;
  ++dot;
  if (dot == end) return -1;
  int n = 0;
  for (const WCHAR *c = dot; c < end; ++c) {
    if (*c == '*' || *c == '?') { out[0] = 0; return 0; }
    out[n++] = *c;
  }
  out[n] = 0;
  return 1;
}

/* A name with no extension of its own gets the default one: the extension of the chosen filter if that is a plain one (and the program asked for default extensions at all), else
 * the default extension itself. */
static void add_default_extension(WCHAR *path, const WCHAR *filters, DWORD filter, const WCHAR *default_extension) {
  if (!default_extension) return;
  int n = wlen(path);
  int start = 0;
  for (int i = 0; i < n; ++i) if (is_separator(path[i])) start = i + 1;
  for (int i = start; i < n; ++i) if (path[i] == '.') return;
  WCHAR from_filter[64];
  const int kind = filter_extension(filter_patterns(filters, filter), from_filter);
  const WCHAR *extension = kind == 1 ? from_filter : default_extension;
  if (kind == -1 || !*extension) return;
  if (n + 1 + wlen(extension) + 1 > PATH_UNITS) return;
  path[n++] = '.';
  for (int i = 0; extension[i]; ++i) path[n++] = extension[i];
  path[n] = 0;
}

static int is_absolute(const WCHAR *s) { return is_separator(s[0]) || (s[0] && s[1] == ':'); }

/* Where the dialog starts: the initial folder (else the current one) with the file name given, as a full path. */
static WCHAR *start_path(const WCHAR *directory, const WCHAR *name) {
  WCHAR *joined = heap_get((SIZE_T)(wlen(directory ? directory : (const WCHAR *)L"") + wlen(name ? name : (const WCHAR *)L"") + 4) * 2);
  WCHAR *full = heap_get((SIZE_T)PATH_UNITS * 2);
  if (!joined || !full) { heap_put(joined); heap_put(full); return NULL; }
  int n = 0;
  if (name && *name && is_absolute(name)) {
    for (int i = 0; name[i]; ++i) joined[n++] = name[i];
  } else {
    if (directory && *directory) {
      for (int i = 0; directory[i]; ++i) joined[n++] = directory[i];
      if (name && *name && !is_separator(joined[n - 1])) joined[n++] = '\\';
    }
    if (name) for (int i = 0; name[i]; ++i) joined[n++] = name[i];
  }
  joined[n] = 0;
  const DWORD got = n ? GetFullPathNameW(joined, PATH_UNITS, full, NULL) : GetCurrentDirectoryW(PATH_UNITS, full);
  heap_put(joined);
  if (got == 0 || got >= PATH_UNITS) { heap_put(full); return NULL; }
  return full;
}

/* GetOpenFileName and GetSaveFileName for both character sets. */
static BOOL file_dialog(OFN *ofn, int wide, int save) {
  set_error(0);
  if (!ofn) { set_error(CDERR_INITIALIZATION); return FALSE; }
  if (ofn->lStructSize != OFN_SIZE && ofn->lStructSize != OFN_SIZE_VERSION_400) { set_error(CDERR_STRUCTSIZE); return FALSE; }
  if ((ofn->Flags & OFN_ENABLEHOOK) && !ofn->lpfnHook) { set_error(CDERR_NOHOOK); return FALSE; }

  int units = 0;
  WCHAR *filter_list = wide_list_copy(ofn->lpstrFilter, wide, &units);
  WCHAR *kernel_filters = dialog_filters(filter_list, units);
  WCHAR *title = wide_copy(ofn->lpstrTitle, wide);
  WCHAR *directory = wide_copy(ofn->lpstrInitialDir, wide);
  WCHAR *default_extension = wide_copy(ofn->lpstrDefExt, wide);
  WCHAR *file = ofn->lpstrFile ? wide_copy(ofn->lpstrFile, wide) : NULL;      /* (what the program put in the buffer: the name to start with) */
  WCHAR *start = start_path(directory, file);
  WCHAR *path = heap_get((SIZE_T)PATH_UNITS * 2);
  BOOL accepted = FALSE;
  DWORD error = 0;
  if (!path || !start) {
    error = CDERR_MEMALLOCFAILURE;
    goto done;
  }
  CommonDialog request = {0};
  request.size = sizeof(CommonDialog);
  request.kind = save ? DIALOG_SAVE : DIALOG_OPEN;
  request.filter_index = ofn->nFilterIndex > 0 ? ofn->nFilterIndex - 1 : 0;
  request.title = title;
  request.start = start;
  request.filters = kernel_filters;
  request.result = path;
  request.result_capacity = PATH_UNITS;
  const NTSTATUS status = NtTcosCommonDialog(&request);
  if (status < 0 && status != (NTSTATUS)0xC0000023) {      /* (no screen, or another dialog is open; STATUS_BUFFER_TOO_SMALL cannot happen with this buffer) */
    error = CDERR_DIALOGFAILURE;
    goto done;
  }
  if (!request.accepted) goto done;      /* cancelled: FALSE with no error */

  if (save) add_default_extension(path, filter_list, request.filter_index + 1, default_extension);
  DWORD flags = ofn->Flags & ~(DWORD)(OFN_READONLY | OFN_EXTENSIONDIFFERENT);
  if (default_extension && *default_extension) {      /* the chosen name has another extension than the default one */
    int dot = -1, n = wlen(path);
    for (int i = 0; i < n; ++i) { if (is_separator(path[i])) dot = -1; else if (path[i] == '.') dot = i; }
    int same = dot >= 0 && wlen(default_extension) == n - dot - 1;
    for (int i = 0; same && i < n - dot - 1; ++i) if (lower(path[dot + 1 + i]) != lower(default_extension[i])) same = 0;
    if (dot >= 0 && !same) flags |= OFN_EXTENSIONDIFFERENT;
  }

  /* The answer: the whole path in the buffer, where the name and the extension begin, the title, which filter it was. */
  DWORD needed = 0;
  if (ofn->lpstrFile && !store_text(ofn->lpstrFile, ofn->nMaxFile, wide, path, &needed)) {
    if (ofn->nMaxFile >= 2) *(WORD *)ofn->lpstrFile = (WORD)needed;      /* (as Windows: the size that would have been needed) */
    error = FNERR_BUFFERTOOSMALL;
    goto done;
  }
  const WCHAR *shown = path;
  WCHAR *ansi = NULL;
  if (!wide && ofn->lpstrFile) shown = ansi = wide_copy(ofn->lpstrFile, 0);      /* offsets are counted in the program's own characters */
  {
    const int n = wlen(shown);
    int name = 0, extension = 0;
    for (int i = 0; i < n; ++i) if (is_separator(shown[i])) name = i + 1;
    for (int i = name; i < n; ++i) if (shown[i] == '.') extension = i + 1;
    if (!ofn->lpstrFile) {      /* (no buffer: the offsets of the path all the same) */
      const int m = wlen(path);
      name = 0; extension = 0;
      for (int i = 0; i < m; ++i) if (is_separator(path[i])) name = i + 1;
    }
    ofn->nFileOffset = (WORD)name;
    ofn->nFileExtension = (WORD)(extension > 0 && extension < n ? extension : 0);
    if (ofn->lpstrFileTitle) {
      const WCHAR *title_start = path;
      const int m = wlen(path);
      for (int i = 0; i < m; ++i) if (is_separator(path[i])) title_start = path + i + 1;
      store_text(ofn->lpstrFileTitle, ofn->nMaxFileTitle, wide, title_start, NULL);
    }
    heap_put(ansi);
  }
  ofn->nFilterIndex = request.filter_index + 1;
  ofn->Flags = flags;
  if (!(ofn->Flags & OFN_NOCHANGEDIR)) {      /* the dialog leaves the process in the folder that was chosen */
    int n = wlen(path), name = 0;
    for (int i = 0; i < n; ++i) if (is_separator(path[i])) name = i + 1;
    if (name > 0) { const WCHAR saved = path[name > 3 ? name - 1 : name]; path[name > 3 ? name - 1 : name] = 0; SetCurrentDirectoryW(path); path[name > 3 ? name - 1 : name] = saved; }
  }
  accepted = TRUE;
done:
  heap_put(filter_list);
  heap_put(kernel_filters);
  heap_put(title);
  heap_put(directory);
  heap_put(default_extension);
  heap_put(file);
  heap_put(start);
  heap_put(path);
  set_error(error);
  return accepted;
}

EXPORT BOOL WINAPI GetOpenFileNameA(void *ofn) { return file_dialog(ofn, 0, 0); }
EXPORT BOOL WINAPI GetOpenFileNameW(void *ofn) { return file_dialog(ofn, 1, 0); }
EXPORT BOOL WINAPI GetSaveFileNameA(void *ofn) { return file_dialog(ofn, 0, 1); }
EXPORT BOOL WINAPI GetSaveFileNameW(void *ofn) { return file_dialog(ofn, 1, 1); }

/* ---- the colour chooser ----------------------------------------------------------------------------------------------------------------------------------- */
typedef struct {
  DWORD lStructSize;
  HANDLE hwndOwner, hInstance;
  DWORD rgbResult;
  DWORD *lpCustColors;
  DWORD Flags;
  LPARAM lCustData;
  void *lpfnHook;
  const void *lpTemplateName;
} CHOOSECOLOR_;
#define CC_RGBINIT 0x1u
#define CC_ENABLEHOOK 0x10u
#define CC_SIZE 72u

static BOOL choose_color(CHOOSECOLOR_ *cc) {
  set_error(0);
  if (!cc) { set_error(CDERR_INITIALIZATION); return FALSE; }
  if (cc->lStructSize != CC_SIZE) { set_error(CDERR_STRUCTSIZE); return FALSE; }
  if (!cc->lpCustColors) { set_error(CDERR_INITIALIZATION); return FALSE; }
  if ((cc->Flags & CC_ENABLEHOOK) && !cc->lpfnHook) { set_error(CDERR_NOHOOK); return FALSE; }
  CommonDialog request = {0};
  request.size = sizeof(CommonDialog);
  request.kind = DIALOG_COLOR;
  request.color = (cc->Flags & CC_RGBINIT) ? cc->rgbResult & 0xFFFFFFu : 0;
  const NTSTATUS status = NtTcosCommonDialog(&request);
  if (status < 0) { set_error(CDERR_DIALOGFAILURE); return FALSE; }
  if (!request.accepted) return FALSE;
  cc->rgbResult = request.color_out & 0xFFFFFFu;
  return TRUE;
}

EXPORT BOOL WINAPI ChooseColorA(void *cc) { return choose_color(cc); }
EXPORT BOOL WINAPI ChooseColorW(void *cc) { return choose_color(cc); }

/* ---- the font chooser ------------------------------------------------------------------------------------------------------------------------------------- *
 * The checks of Windows, then CFERR_NOFONTS: a font list needs gdi32.dll, which has no fonts to list yet. */
typedef struct {
  DWORD lStructSize;
  HANDLE hwndOwner, hDC;
  void *lpLogFont;
  INT iPointSize;
  DWORD Flags;
  DWORD rgbColors;
  LPARAM lCustData;
  void *lpfnHook;
  const void *lpTemplateName;
  HANDLE hInstance;
  void *lpszStyle;
  WORD nFontType, align;
  INT nSizeMin, nSizeMax;
} CHOOSEFONT_;
#define CF_SCREENFONTS 0x1u
#define CF_PRINTERFONTS 0x2u
#define CF_ENABLEHOOK 0x8u
#define CF_ENABLETEMPLATE 0x10u
#define CF_ENABLETEMPLATEHANDLE 0x20u
#define CF_LIMITSIZE 0x2000u
#define CF_INITTOLOGFONTSTRUCT 0x40u
#define CF_SIZE 104u

static BOOL choose_font(CHOOSEFONT_ *cf) {
  set_error(0);
  if (!cf) { set_error(CDERR_INITIALIZATION); return FALSE; }
  if (cf->lStructSize != CF_SIZE) { set_error(CDERR_STRUCTSIZE); return FALSE; }
  if (!cf->lpLogFont) { set_error(CDERR_INITIALIZATION); return FALSE; }
  if ((cf->Flags & CF_ENABLEHOOK) && !cf->lpfnHook) { set_error(CDERR_NOHOOK); return FALSE; }
  if ((cf->Flags & CF_ENABLETEMPLATE) && !cf->lpTemplateName) { set_error(CDERR_NOTEMPLATE); return FALSE; }
  if ((cf->Flags & CF_ENABLETEMPLATEHANDLE) && !cf->hInstance) { set_error(CDERR_NOHINSTANCE); return FALSE; }
  if (!(cf->Flags & (CF_SCREENFONTS | CF_PRINTERFONTS))) { set_error(CFERR_NOFONTS); return FALSE; }
  if ((cf->Flags & CF_LIMITSIZE) && cf->nSizeMax < cf->nSizeMin) { set_error(0x2002 /* CFERR_MAXLESSTHANMIN */); return FALSE; }
  set_error(CFERR_NOFONTS);
  return FALSE;
}

EXPORT BOOL WINAPI ChooseFontA(void *cf) { return choose_font(cf); }
EXPORT BOOL WINAPI ChooseFontW(void *cf) { return choose_font(cf); }

/* ---- Find and Replace --------------------------------------------------------------------------------------------------------------------------------------- *
 * Modeless dialogs that are windows of the program (and tell it what the user did with window messages): checked as Windows checks them, then CDERR_DIALOGFAILURE. */
typedef struct {
  DWORD lStructSize;
  HANDLE hwndOwner, hInstance;
  DWORD Flags;
  void *lpstrFindWhat, *lpstrReplaceWith;
  WORD wFindWhatLen, wReplaceWithLen;
  LPARAM lCustData;
  void *lpfnHook;
  const void *lpTemplateName;
} FINDREPLACE_;
#define FR_ENABLEHOOK 0x100u
#define FR_ENABLETEMPLATE 0x200u
#define FR_ENABLETEMPLATEHANDLE 0x2000u
#define FR_SIZE 80u

static HANDLE find_replace(FINDREPLACE_ *fr, int replace) {
  set_error(0);
  if (!fr) { set_error(CDERR_INITIALIZATION); return NULL; }
  if (fr->lStructSize != FR_SIZE) { set_error(CDERR_STRUCTSIZE); return NULL; }
  if (!fr->hwndOwner) { set_error(CDERR_DIALOGFAILURE); return NULL; }
  if (fr->wFindWhatLen == 0 || !fr->lpstrFindWhat) { set_error(FRERR_BUFFERLENGTHZERO); return NULL; }
  if (replace && !fr->lpstrReplaceWith) { set_error(FRERR_BUFFERLENGTHZERO); return NULL; }
  if ((fr->Flags & FR_ENABLEHOOK) && !fr->lpfnHook) { set_error(CDERR_NOHOOK); return NULL; }
  if ((fr->Flags & FR_ENABLETEMPLATEHANDLE) && !fr->hInstance) { set_error(CDERR_NOHINSTANCE); return NULL; }
  if (fr->Flags & FR_ENABLETEMPLATE) { set_error(CDERR_FINDRESFAILURE); return NULL; }      /* (a template is a dialog resource of a window; there are no such dialogs) */
  set_error(CDERR_DIALOGFAILURE);
  return NULL;
}

EXPORT HANDLE WINAPI FindTextA(void *fr) { return find_replace(fr, 0); }
EXPORT HANDLE WINAPI FindTextW(void *fr) { return find_replace(fr, 0); }
EXPORT HANDLE WINAPI ReplaceTextA(void *fr) { return find_replace(fr, 1); }
EXPORT HANDLE WINAPI ReplaceTextW(void *fr) { return find_replace(fr, 1); }

/* ---- printing ------------------------------------------------------------------------------------------------------------------------------------------------ *
 * Windows programs have no print spooler here, so there is no printer to choose or to be the default: PDERR_NODEFAULTPRN after the checks of Windows. */
#define PD_ENABLEPRINTHOOK 0x1000u
#define PD_ENABLESETUPHOOK 0x2000u
#define PD_ENABLEPRINTTEMPLATE 0x4000u
#define PD_ENABLESETUPTEMPLATE 0x8000u
#define PD_ENABLEPRINTTEMPLATEHANDLE 0x10000u
#define PD_ENABLESETUPTEMPLATEHANDLE 0x20000u
#define PD_NOPAGENUMS 0x8u
#define PD_RETURNDEFAULT 0x400u
#define PRINTDLG_SIZE 120u
#define PAGESETUPDLG_SIZE 128u
#define PRINTDLGEX_SIZE 136u
#define START_PAGE_GENERAL 0xFFFFFFFFu

static BOOL print_dialog(const BYTE *dlg, DWORD size) {
  set_error(0);
  if (!dlg) { set_error(CDERR_INITIALIZATION); return FALSE; }
  if (*(const DWORD *)dlg != size) { set_error(CDERR_STRUCTSIZE); return FALSE; }
  set_error(PDERR_NODEFAULTPRN);
  return FALSE;
}

EXPORT BOOL WINAPI PrintDlgA(void *pd) { return print_dialog(pd, PRINTDLG_SIZE); }
EXPORT BOOL WINAPI PrintDlgW(void *pd) { return print_dialog(pd, PRINTDLG_SIZE); }
EXPORT BOOL WINAPI PageSetupDlgA(void *psd) { return print_dialog(psd, PAGESETUPDLG_SIZE); }
EXPORT BOOL WINAPI PageSetupDlgW(void *psd) { return print_dialog(psd, PAGESETUPDLG_SIZE); }

/* PrintDlgEx answers with an HRESULT: E_INVALIDARG for a wrong structure, E_HANDLE without an owner window, E_FAIL (nothing was chosen) when there is no printer. */
static LONG print_dialog_ex(const BYTE *dlg) {
  set_error(0);
  if (!dlg) { set_error(CDERR_INITIALIZATION); return E_INVALIDARG_; }
  if (*(const DWORD *)dlg != PRINTDLGEX_SIZE) { set_error(CDERR_STRUCTSIZE); return E_INVALIDARG_; }
  if (!*(void *const *)(dlg + 8)) return E_HANDLE_;
  const DWORD flags = *(const DWORD *)(dlg + 40), max_ranges = *(const DWORD *)(dlg + 56);
  const void *ranges = *(void *const *)(dlg + 64);
  const DWORD start_page = *(const DWORD *)(dlg + 128), property_pages = *(const DWORD *)(dlg + 112);
  if (start_page != START_PAGE_GENERAL && start_page >= property_pages) return E_INVALIDARG_;
  if (!(flags & PD_NOPAGENUMS) && (max_ranges == 0 || !ranges)) return E_INVALIDARG_;
  if ((flags & PD_NOPAGENUMS) && (max_ranges != 0) != (ranges != NULL)) return E_INVALIDARG_;
  set_error(PDERR_NODEFAULTPRN);
  *(DWORD *)(dlg + 132) = 0;      /* dwResultAction = PD_RESULT_CANCEL */
  return E_FAIL_;
}

EXPORT LONG WINAPI PrintDlgExA(void *pd) { return print_dialog_ex(pd); }
EXPORT LONG WINAPI PrintDlgExW(void *pd) { return print_dialog_ex(pd); }

/* ---- the rest of the exports: made for windows -------------------------------------------------------------------------------------------------------------- */
EXPORT HANDLE WINAPI LoadAlterBitmap(UINT id, DWORD replace, DWORD instead) { (void)id; (void)replace; (void)instead; SetLastError(1814 /* ERROR_RESOURCE_NAME_NOT_FOUND */); return NULL; }
EXPORT void WINAPI Ssync_ANSI_UNICODE_Struct_For_WOW(HANDLE window, BOOL to_unicode, DWORD kind) { (void)window; (void)to_unicode; (void)kind; }
EXPORT LONG_PTR WINAPI WantArrows(HANDLE window, UINT message, UINT_PTR wparam, LONG_PTR lparam) { (void)window; (void)wparam; (void)lparam; return message == 0x87 /* WM_GETDLGCODE */ ? 1 /* DLGC_WANTARROWS */ : 0; }
