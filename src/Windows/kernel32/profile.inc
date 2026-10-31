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

/* kernel32.dll: initialization (.ini) files: GetPrivateProfile*, WritePrivateProfile* and the win.ini forms. Ported from Wine's profile.c (Copyright 1993 Miguel de Icaza,
 * 1996 Alexandre Julliard; LGPL 2.1 or later). A file is parsed into sections and keys kept in a small most-recently-used cache, changed in memory and written back (in
 * the encoding it was read in), and a file named in HKLM\Software\Microsoft\Windows NT\CurrentVersion\IniFileMapping lives in the registry instead (the registry calls
 * are those of advapi32.dll, found when the first mapping is looked for). Included by kernel32.c. */

#define IS_TEXT_UNICODE_SIGNATURE_ 0x0008
#define IS_TEXT_UNICODE_REVERSE_SIGNATURE_ 0x0080
#define IS_TEXT_UNICODE_ODD_LENGTH_ 0x0200
#ifndef ERROR_CALL_NOT_IMPLEMENTED
#define ERROR_CALL_NOT_IMPLEMENTED 120
#endif

WCHAR *wcschr(const WCHAR *s, WCHAR c);
int wcscmp(const WCHAR *a, const WCHAR *b);
int wcsncmp(const WCHAR *a, const WCHAR *b, size_t_ n);
int _wcsicmp(const WCHAR *a, const WCHAR *b);
int _wcsnicmp(const WCHAR *a, const WCHAR *b, size_t_ n);
BOOLEAN NTAPI RtlIsTextUnicode(PVOID buffer, ULONG size, PULONG flags);
ULONG NTAPI RtlDetermineDosPathNameType_U(PCWSTR path);
VOID NTAPI RtlInitUnicodeString(PUNICODE_STRING dst, PCWSTR src);
NTSTATUS NTAPI RtlUnicodeStringToInteger(const UNICODE_STRING *s, ULONG base, PULONG value);

typedef enum { ENCODING_ANSI = 1, ENCODING_UTF8, ENCODING_UTF16LE, ENCODING_UTF16BE } ProfEncoding;
/* A line may hold NULs (they are kept when the file is written back), so the names and values have their lengths beside them. The lines before the first [header]
 * are a section with no name (implicit): not the same as the one called "". */
typedef struct ProfKey { WCHAR *value; struct ProfKey *next; int name_len, value_len; WCHAR name[1]; } ProfKey;
typedef struct ProfSection { ProfKey *key; struct ProfSection *next; BOOL implicit; WCHAR name[1]; } ProfSection;
typedef struct { BOOL changed; ProfSection *section; WCHAR *filename; FILETIME last_write; ProfEncoding encoding; } Profile;

#define PROF_CACHED 10
static Profile *g_prof_mru[PROF_CACHED];
#define PROF_CUR (g_prof_mru[0])
static CRITICAL_SECTION g_prof_lock;
static volatile LONG g_prof_ready;
static void prof_enter(void) {
  if (g_prof_ready != 2) {
    if (InterlockedCompareExchange(&g_prof_ready, 1, 0) == 0) { RtlInitializeCriticalSection(&g_prof_lock); g_prof_ready = 2; }
    else while (g_prof_ready != 2) SwitchToThread();
  }
  EnterCriticalSection(&g_prof_lock);
}
static void prof_leave(void) { LeaveCriticalSection(&g_prof_lock); }

static void *prof_alloc(size_t_ bytes) { return HeapAlloc(GetProcessHeap(), 0, bytes); }
static void prof_free(void *p) { if (p) HeapFree(GetProcessHeap(), 0, p); }
static int prof_len(const WCHAR *s) { return (int)wcslen(s); }
static void prof_set_str(WCHAR *d, const WCHAR *s) { memcpy(d, s, ((size_t_)wcslen(s) + 1) * sizeof(WCHAR)); }
static BOOL prof_isspace(WCHAR c) { return (c >= 0x09 && c <= 0x0d) || c == 0x1a || c == 0x20; }      /* (^Z, the DOS end of file, is a space too) */
static int prof_icmp(const WCHAR *a, const WCHAR *b) { return _wcsicmp(a, b); }
static int prof_nicmp(const WCHAR *a, const WCHAR *b, size_t_ n) { return _wcsnicmp(a, b, n); }

/* Copies the value of an entry into a buffer, without the quotes around it. */
static void prof_copy_entry(LPWSTR buffer, LPCWSTR value, int len) {
  WCHAR quote = 0;
  if (!buffer) return;
  if (*value == '\'' || *value == '\"') { if (value[1] && value[prof_len(value) - 1] == *value) quote = *value++; }
  lstrcpynW(buffer, value, len);
  if (quote && len >= prof_len(value)) buffer[prof_len(buffer) - 1] = 0;
}

static void prof_swap(WCHAR *buffer, size_t_ count) { for (size_t_ i = 0; i < count; i++) buffer[i] = (WCHAR)((buffer[i] << 8) | (buffer[i] >> 8)); }

static void prof_write_marker(HANDLE file, ProfEncoding encoding) {
  static const BYTE utf8[3] = {0xEF, 0xBB, 0xBF};
  DWORD written;
  WCHAR bom;
  switch (encoding) {
    case ENCODING_UTF8: WriteFile(file, utf8, sizeof(utf8), &written, NULL); break;
    case ENCODING_UTF16LE: bom = 0xFEFF; WriteFile(file, &bom, sizeof(bom), &written, NULL); break;
    case ENCODING_UTF16BE: bom = 0xFFFE; WriteFile(file, &bom, sizeof(bom), &written, NULL); break;
    default: break;
  }
}

static void prof_write_line(HANDLE file, WCHAR *line, int len, ProfEncoding encoding) {
  DWORD written;
  if (encoding == ENCODING_ANSI || encoding == ENCODING_UTF8) {
    const UINT cp = encoding == ENCODING_ANSI ? CP_ACP : CP_UTF8;
    const int bytes = WideCharToMultiByte(cp, 0, line, len, NULL, 0, NULL, NULL);
    char *out = bytes > 0 ? prof_alloc((size_t_)bytes) : NULL;
    if (!out) return;
    const int n = WideCharToMultiByte(cp, 0, line, len, out, bytes, NULL, NULL);
    WriteFile(file, out, (DWORD)n, &written, NULL);
    prof_free(out);
  } else {
    if (encoding == ENCODING_UTF16BE) prof_swap(line, (size_t_)len);
    WriteFile(file, line, (DWORD)len * sizeof(WCHAR), &written, NULL);
  }
}

static void prof_save(HANDLE file, const ProfSection *section, ProfEncoding encoding) {
  prof_write_marker(file, encoding);
  for (; section; section = section->next) {
    size_t_ len = 0;
    if (!section->implicit) len += (size_t_)prof_len(section->name) + 4;
    for (const ProfKey *key = section->key; key; key = key->next) {
      len += (size_t_)key->name_len + 2;
      if (key->value) len += (size_t_)key->value_len + 1;
    }
    WCHAR *buffer = prof_alloc((len + 1) * sizeof(WCHAR));
    if (!buffer) return;
    WCHAR *p = buffer;
    if (!section->implicit) { *p++ = '['; prof_set_str(p, section->name); p += prof_len(p); *p++ = ']'; *p++ = '\r'; *p++ = '\n'; }
    for (const ProfKey *key = section->key; key; key = key->next) {
      memcpy(p, key->name, (size_t_)key->name_len * sizeof(WCHAR)); p += key->name_len;
      if (key->value) { *p++ = '='; memcpy(p, key->value, (size_t_)key->value_len * sizeof(WCHAR)); p += key->value_len; }
      *p++ = '\r'; *p++ = '\n';
    }
    prof_write_line(file, buffer, (int)(p - buffer), encoding);
    prof_free(buffer);
  }
}

static void prof_free_tree(ProfSection *section) {
  ProfSection *next_section;
  for (; section; section = next_section) {
    ProfKey *next_key;
    for (ProfKey *key = section->key; key; key = next_key) { next_key = key->next; prof_free(key->value); prof_free(key); }
    next_section = section->next;
    prof_free(section);
  }
}

static ProfEncoding prof_detect(const void *buffer, int *len) {
  ULONG flags = IS_TEXT_UNICODE_SIGNATURE_ | IS_TEXT_UNICODE_REVERSE_SIGNATURE_ | IS_TEXT_UNICODE_ODD_LENGTH_;
  const BYTE *b = buffer;
  if (*len >= 3 && b[0] == 0xEF && b[1] == 0xBB && b[2] == 0xBF) { *len = 3; return ENCODING_UTF8; }
  RtlIsTextUnicode((PVOID)buffer, (ULONG)*len, &flags);
  if (flags & IS_TEXT_UNICODE_SIGNATURE_) { *len = 2; return ENCODING_UTF16LE; }
  if (flags & IS_TEXT_UNICODE_REVERSE_SIGNATURE_) { *len = 2; return ENCODING_UTF16BE; }
  *len = 0;
  return ENCODING_ANSI;
}

static void prof_trim(WCHAR **start, WCHAR **end) {
  WCHAR *s = *start, *e = *end;
  while (s < e && prof_isspace(*s)) s++;
  while (e > s && prof_isspace(e[-1])) e--;
  *start = s; *end = e;
}

static ProfSection *prof_load(HANDLE file, ProfEncoding *encoding) {
  const DWORD size_in = GetFileSize(file, NULL);
  if (size_in == INVALID_FILE_SIZE || size_in == 0) return NULL;
  void *base = prof_alloc(size_in);
  if (!base) return NULL;
  DWORD size = size_in;
  if (!ReadFile(file, base, size, &size, NULL)) { prof_free(base); return NULL; }
  int len = (int)size;
  *encoding = prof_detect(base, &len);
  char *buffer = (char *)base + len;      /* past the marker */
  size -= (DWORD)len;
  WCHAR *text, *end;
  BOOL owned = FALSE;
  switch (*encoding) {
    case ENCODING_ANSI: case ENCODING_UTF8: {
      const UINT cp = *encoding == ENCODING_ANSI ? CP_ACP : CP_UTF8;
      len = MultiByteToWideChar(cp, 0, buffer, (int)size, NULL, 0);
      text = prof_alloc(((size_t_)len + 1) * sizeof(WCHAR));
      if (!text) { prof_free(base); return NULL; }
      MultiByteToWideChar(cp, 0, buffer, (int)size, text, len);
      end = text + len;
      owned = TRUE;
      break;
    }
    default:
      text = (WCHAR *)buffer;      /* (the allocation is word aligned; the marker is two bytes) */
      end = (WCHAR *)(buffer + (size & ~1u));
      if (*encoding == ENCODING_UTF16BE) prof_swap(text, size / sizeof(WCHAR));
      break;
  }
  ProfSection *first = prof_alloc(sizeof(ProfSection));
  if (!first) { if (owned) prof_free(text); prof_free(base); return NULL; }
  first->name[0] = 0; first->key = NULL; first->next = NULL; first->implicit = TRUE;
  ProfSection **next_section = &first->next;
  ProfKey **next_key = &first->key;
  ProfKey *prev_key = NULL;
  WCHAR *next_line = text;
  while (next_line < end) {
    WCHAR *line_start = next_line, *line_end;
    while (next_line < end && *next_line != '\n' && *next_line != '\r') next_line++;
    while (next_line < end && (*next_line == '\n' || *next_line == '\r')) next_line++;
    line_end = next_line;
    prof_trim(&line_start, &line_end);
    if (line_start >= line_end) continue;
    if (*line_start == '[') {
      for (len = (int)(line_end - line_start); len > 0; len--) if (line_start[len - 1] == ']') break;
      if (len) {
        line_start++;
        len -= 2;
        line_end = line_start + len;
        prof_trim(&line_start, &line_end);
        len = (int)(line_end - line_start);
        ProfSection *section = prof_alloc(sizeof(*section) + (size_t_)len * sizeof(WCHAR));
        if (!section) break;
        memcpy(section->name, line_start, (size_t_)len * sizeof(WCHAR));
        section->name[len] = 0;
        section->key = NULL; section->next = NULL; section->implicit = FALSE;
        *next_section = section;
        next_section = &section->next;
        next_key = &section->key;
        prev_key = NULL;
        continue;
      }
    }
    len = (int)(line_end - line_start);
    const WCHAR *value_start;
    for (value_start = line_start; value_start < line_end; value_start++) if (*value_start == '=') break;
    if (value_start < line_end) {
      const WCHAR *name_end = value_start;
      while (name_end > line_start && prof_isspace(name_end[-1])) name_end--;
      len = (int)(name_end - line_start);
      value_start++;
      while (value_start < line_end && prof_isspace(*value_start)) value_start++;
    } else value_start = NULL;
    if (len || !prev_key || *prev_key->name) {
      ProfKey *key = prof_alloc(sizeof(*key) + (size_t_)len * sizeof(WCHAR));
      if (!key) break;
      memcpy(key->name, line_start, (size_t_)len * sizeof(WCHAR));
      key->name[len] = 0;
      key->name_len = len;
      key->value_len = 0;
      if (value_start) {
        const int vlen = (int)(line_end - value_start);
        key->value = prof_alloc(((size_t_)vlen + 1) * sizeof(WCHAR));
        if (key->value) { memcpy(key->value, value_start, (size_t_)vlen * sizeof(WCHAR)); key->value[vlen] = 0; key->value_len = vlen; }
      } else key->value = NULL;
      key->next = NULL;
      *next_key = key;
      next_key = &key->next;
      prev_key = key;
    }
  }
  if (owned) prof_free(text);
  prof_free(base);
  return first;
}

static BOOL prof_delete_key(ProfSection **section, LPCWSTR section_name, LPCWSTR key_name) {
  while (*section) {
    if (!(*section)->implicit && !prof_icmp((*section)->name, section_name)) {
      ProfKey **key = &(*section)->key;
      while (*key) {
        if (!prof_icmp((*key)->name, key_name)) {
          ProfKey *to_del = *key;
          *key = to_del->next;
          prof_free(to_del->value);
          prof_free(to_del);
          return TRUE;
        }
        key = &(*key)->next;
      }
    }
    section = &(*section)->next;
  }
  return FALSE;
}

static void prof_delete_all_keys(LPCWSTR section_name) {
  ProfSection **section = &PROF_CUR->section;
  while (*section) {
    if (!(*section)->implicit && !prof_icmp((*section)->name, section_name)) {
      ProfKey **key = &(*section)->key;
      while (*key) {
        ProfKey *to_del = *key;
        *key = to_del->next;
        prof_free(to_del->value);
        prof_free(to_del);
        PROF_CUR->changed = TRUE;
      }
    }
    section = &(*section)->next;
  }
}

/* Finds a key in the tree, optionally creating it (create_always: even if there is one of that name). */
static ProfKey *prof_find(ProfSection **section, LPCWSTR section_name, LPCWSTR key_name, BOOL create, BOOL create_always) {
  const WCHAR *p;
  int seclen = 0, keylen = 0;
  while (prof_isspace(*section_name)) section_name++;
  if (*section_name) { p = section_name + prof_len(section_name) - 1; while (p > section_name && prof_isspace(*p)) p--; seclen = (int)(p - section_name) + 1; }
  while (prof_isspace(*key_name)) key_name++;
  if (*key_name) { p = key_name + prof_len(key_name) - 1; while (p > key_name && prof_isspace(*p)) p--; keylen = (int)(p - key_name) + 1; }
  while (*section) {
    if (!(*section)->implicit && !prof_nicmp((*section)->name, section_name, (size_t_)seclen) && (*section)->name[seclen] == 0) {
      ProfKey **key = &(*section)->key;
      while (*key) {
        if (!create_always && !prof_nicmp((*key)->name, key_name, (size_t_)keylen) && (*key)->name[keylen] == 0) return *key;
        key = &(*key)->next;
      }
      if (!create) return NULL;
      if (!(*key = prof_alloc(sizeof(ProfKey) + (size_t_)prof_len(key_name) * sizeof(WCHAR)))) return NULL;
      prof_set_str((*key)->name, key_name);
      (*key)->name_len = prof_len(key_name); (*key)->value_len = 0;
      (*key)->value = NULL; (*key)->next = NULL;
      return *key;
    }
    section = &(*section)->next;
  }
  if (!create) return NULL;
  *section = prof_alloc(sizeof(ProfSection) + (size_t_)prof_len(section_name) * sizeof(WCHAR));
  if (!*section) return NULL;
  prof_set_str((*section)->name, section_name);
  (*section)->next = NULL;
  (*section)->implicit = FALSE;
  if (!((*section)->key = prof_alloc(sizeof(ProfKey) + (size_t_)prof_len(key_name) * sizeof(WCHAR)))) { prof_free(*section); *section = NULL; return NULL; }
  prof_set_str((*section)->key->name, key_name);
  (*section)->key->name_len = prof_len(key_name); (*section)->key->value_len = 0;
  (*section)->key->value = NULL; (*section)->key->next = NULL;
  return (*section)->key;
}

/* Writes the current profile back if it changed. */
static BOOL prof_flush(void) {
  if (!PROF_CUR) return FALSE;
  if (!PROF_CUR->changed) return TRUE;
  HANDLE file = CreateFileW(PROF_CUR->filename, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
  if (file == INVALID_HANDLE_VALUE) return FALSE;
  prof_save(file, PROF_CUR->section, PROF_CUR->encoding);
  FILETIME t;
  if (GetFileTime(file, NULL, NULL, &t)) PROF_CUR->last_write = t;
  CloseHandle(file);
  PROF_CUR->changed = FALSE;
  return TRUE;
}

static void prof_release(void) {
  prof_flush();
  prof_free_tree(PROF_CUR->section);
  prof_free(PROF_CUR->filename);
  PROF_CUR->changed = FALSE;
  PROF_CUR->section = NULL;
  PROF_CUR->filename = NULL;
  PROF_CUR->encoding = ENCODING_ANSI;
  memset(&PROF_CUR->last_write, 0, sizeof(PROF_CUR->last_write));
}

/* A file time at least 2.1 seconds old (FAT keeps the time of two seconds, so a younger file may change without its time changing: it is not cached). */
static BOOL prof_is_not_current(const FILETIME *ft) {
  LARGE_INTEGER now;
  NtQuerySystemTime(&now);
  const LONGLONG t = ((LONGLONG)ft->dwHighDateTime << 32) + ft->dwLowDateTime;
  return t + 21000000 < now.QuadPart;
}

/* Opens a profile file, from the cache if it is there. */
/* write_access: opened for writing; create: a file that is not there is made (when flushed) rather than left alone. */
static BOOL prof_open(LPCWSTR filename, BOOL write_access, BOOL create) {
  static const WCHAR win_ini[] = {'w', 'i', 'n', '.', 'i', 'n', 'i', 0};
  WCHAR buffer[MAX_PATH];
  FILETIME last_write;
  int i, j;
  memset(&last_write, 0, sizeof(last_write));
  if (!PROF_CUR) {
    for (i = 0; i < PROF_CACHED; i++) {
      g_prof_mru[i] = prof_alloc(sizeof(Profile));
      if (!g_prof_mru[i]) { while (i--) { prof_free(g_prof_mru[i]); g_prof_mru[i] = NULL; } SetLastError(ERROR_NOT_ENOUGH_MEMORY); return FALSE; }
      memset(g_prof_mru[i], 0, sizeof(Profile));
      g_prof_mru[i]->encoding = ENCODING_ANSI;
    }
  }
  if (!filename) filename = win_ini;
  if (!filename[0]) { SetLastError(ERROR_ACCESS_DENIED); return FALSE; }
  if (RtlDetermineDosPathNameType_U(filename) == 5 /* RtlPathTypeRelative */ && !wcschr(filename, '\\') && !wcschr(filename, '/')) {
    if (!filename[0]) { SetLastError(ERROR_ACCESS_DENIED); return FALSE; }
    WCHAR windir[MAX_PATH];
    GetWindowsDirectoryW(windir, MAX_PATH);
    if ((size_t_)prof_len(windir) + 1 + (size_t_)prof_len(filename) >= MAX_PATH) { SetLastError(ERROR_FILENAME_EXCED_RANGE); return FALSE; }
    prof_set_str(buffer, windir);
    buffer[prof_len(buffer) + 1] = 0;
    buffer[prof_len(buffer)] = '\\';
    prof_set_str(buffer + prof_len(buffer), filename);
  } else {
    LPWSTR dummy;
    GetFullPathNameW(filename, MAX_PATH, buffer, &dummy);
  }
  HANDLE file = CreateFileW(buffer, GENERIC_READ | (write_access ? GENERIC_WRITE : 0), FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
                            FILE_ATTRIBUTE_NORMAL, NULL);
  if (file == INVALID_HANDLE_VALUE && GetLastError() != ERROR_FILE_NOT_FOUND) return FALSE;
  for (i = 0; i < PROF_CACHED; i++) {
    if (g_prof_mru[i]->filename && !prof_icmp(buffer, g_prof_mru[i]->filename)) {
      if (i) {
        prof_flush();
        Profile *t = g_prof_mru[i];
        for (j = i; j > 0; j--) g_prof_mru[j] = g_prof_mru[j - 1];
        PROF_CUR = t;
      }
      if (file != INVALID_HANDLE_VALUE) {
        GetFileTime(file, NULL, NULL, &last_write);
        if (!memcmp(&PROF_CUR->last_write, &last_write, sizeof(FILETIME)) && prof_is_not_current(&last_write)) {}      /* still good */
        else {
          prof_free_tree(PROF_CUR->section);
          PROF_CUR->section = prof_load(file, &PROF_CUR->encoding);
          PROF_CUR->last_write = last_write;
        }
        CloseHandle(file);
        return TRUE;
      }
    }
  }
  prof_flush();      /* the old current profile */
  if (i == PROF_CACHED) {      /* the oldest becomes the current one, to be got rid of */
    Profile *t = g_prof_mru[PROF_CACHED - 1];
    for (i = PROF_CACHED - 1; i > 0; i--) g_prof_mru[i] = g_prof_mru[i - 1];
    PROF_CUR = t;
  }
  if (PROF_CUR->filename) prof_release();
  PROF_CUR->filename = prof_alloc(((size_t_)prof_len(buffer) + 1) * sizeof(WCHAR));
  if (!PROF_CUR->filename) { if (file != INVALID_HANDLE_VALUE) CloseHandle(file); SetLastError(ERROR_NOT_ENOUGH_MEMORY); return FALSE; }
  prof_set_str(PROF_CUR->filename, buffer);
  if (file != INVALID_HANDLE_VALUE) {
    PROF_CUR->section = prof_load(file, &PROF_CUR->encoding);
    GetFileTime(file, NULL, NULL, &PROF_CUR->last_write);
    CloseHandle(file);
  } else if (write_access && create) PROF_CUR->changed = TRUE;      /* a file that is not there yet is created (empty if nothing is put into it) when it is flushed */
  return TRUE;
}

/* The keys of a section, with their values if return_values. */
static INT prof_get_section(const WCHAR *filename, LPCWSTR section_name, LPWSTR buffer, UINT len, BOOL return_values) {
  if (!buffer) return 0;
  prof_enter();
  if (!prof_open(filename, FALSE, FALSE)) { prof_leave(); buffer[0] = 0; return 0; }
  for (ProfSection *section = PROF_CUR->section; section; section = section->next) {
    if (!section->implicit && !prof_icmp(section->name, section_name)) {
      SetLastError(ERROR_SUCCESS);
      const UINT old_len = len;
      for (ProfKey *key = section->key; key; key = key->next) {
        if (len <= 2) break;
        if (!*key->name && !key->value) continue;
        if (key->name[0] == ';') continue;
        if (!return_values && !key->value) continue;
        lstrcpynW(buffer, key->name, (int)len - 1);
        len -= (UINT)prof_len(buffer) + 1;
        buffer += prof_len(buffer) + 1;
        if (len < 2) break;
        if (return_values && key->value) {
          buffer[-1] = '=';
          lstrcpynW(buffer, key->value, (int)len - 1);
          len -= (UINT)prof_len(buffer) + 1;
          buffer += prof_len(buffer) + 1;
        }
      }
      *buffer = 0;
      prof_leave();
      if (len <= 1) { buffer[-1] = 0; return (INT)old_len - 2; }      /* too small: the last string is cut and followed by two NULs */
      return (INT)(old_len - len);
    }
  }
  buffer[0] = buffer[1] = 0;
  prof_leave();
  SetLastError(ERROR_FILE_NOT_FOUND);
  return 0;
}

static BOOL prof_delete_section(const WCHAR *filename, const WCHAR *name, BOOL create) {
  prof_enter();
  if (!prof_open(filename, TRUE, create)) { prof_leave(); return FALSE; }
  for (ProfSection **section = &PROF_CUR->section; *section; section = &(*section)->next) {
    if (!(*section)->implicit && !prof_icmp((*section)->name, name)) {
      ProfSection *to_del = *section;
      *section = to_del->next;
      to_del->next = NULL;
      prof_free_tree(to_del);
      PROF_CUR->changed = TRUE;
      break;
    }
  }
  prof_flush();      /* (also creates a file that was not there) */
  prof_leave();
  return TRUE;
}

static INT prof_get_section_names(LPWSTR buffer, UINT len) {
  if (!buffer || !len) return 0;
  if (len == 1) { *buffer = 0; return 0; }
  UINT buflen = len - 1;
  LPWSTR buf = buffer;
  for (ProfSection *section = PROF_CUR->section; section; section = section->next) {
    if (section->name[0]) {
      const UINT tmplen = (UINT)prof_len(section->name) + 1;
      if (tmplen >= buflen) {
        if (buflen > 0) { memcpy(buf, section->name, (buflen - 1) * sizeof(WCHAR)); buf += buflen - 1; *buf++ = 0; }
        *buf = 0;
        return (INT)len - 2;
      }
      memcpy(buf, section->name, tmplen * sizeof(WCHAR));
      buf += tmplen;
      buflen -= tmplen;
    }
  }
  *buf = 0;
  return (INT)(buf - buffer);
}

static BOOL prof_set_string(LPCWSTR section_name, LPCWSTR key_name, LPCWSTR value, BOOL create_always) {
  if (!value) { PROF_CUR->changed |= prof_delete_key(&PROF_CUR->section, section_name, key_name); return TRUE; }
  ProfKey *key = prof_find(&PROF_CUR->section, section_name, key_name, TRUE, create_always);
  if (!key) return FALSE;
  while (prof_isspace(*value)) value++;
  if (key->value) {
    if (!wcscmp(key->value, value)) return TRUE;
    prof_free(key->value);
  }
  key->value = prof_alloc(((size_t_)prof_len(value) + 1) * sizeof(WCHAR));
  if (!key->value) return FALSE;
  prof_set_str(key->value, value);
  key->value_len = prof_len(value);
  PROF_CUR->changed = TRUE;
  return TRUE;
}

/* ---- the registry mapping of a file ---- */

typedef LONG (WINAPI *ProfRegOpen)(HANDLE, LPCWSTR, DWORD, DWORD, HANDLE *);
typedef LONG (WINAPI *ProfRegCreate)(HANDLE, LPCWSTR, DWORD, LPWSTR, DWORD, DWORD, PVOID, HANDLE *, LPDWORD);
typedef LONG (WINAPI *ProfRegClose)(HANDLE);
typedef LONG (WINAPI *ProfRegEnumValue)(HANDLE, DWORD, LPWSTR, LPDWORD, LPDWORD, LPDWORD, BYTE *, LPDWORD);
typedef LONG (WINAPI *ProfRegGetValue)(HANDLE, LPCWSTR, LPCWSTR, DWORD, LPDWORD, PVOID, LPDWORD);
typedef LONG (WINAPI *ProfRegSetValue)(HANDLE, LPCWSTR, DWORD, DWORD, const BYTE *, DWORD);
typedef LONG (WINAPI *ProfRegDeleteValue)(HANDLE, LPCWSTR);
static struct { ProfRegOpen open; ProfRegCreate create; ProfRegClose close; ProfRegEnumValue enum_value; ProfRegGetValue get_value; ProfRegSetValue set_value;
                ProfRegDeleteValue delete_value; BOOL tried, ok; } g_prof_reg;
#define PROF_HKLM ((HANDLE)(ULONG_PTR)(LONG)0x80000002)
#define PROF_HKCU ((HANDLE)(ULONG_PTR)(LONG)0x80000001)

static BOOL prof_reg_load(void) {
  prof_enter();
  if (!g_prof_reg.tried) {
    static const WCHAR name[] = {'a', 'd', 'v', 'a', 'p', 'i', '3', '2', '.', 'd', 'l', 'l', 0};
    g_prof_reg.tried = TRUE;
    HMODULE m = LoadLibraryW(name);
    if (m) {
      g_prof_reg.open = (ProfRegOpen)GetProcAddress(m, "RegOpenKeyExW");
      g_prof_reg.create = (ProfRegCreate)GetProcAddress(m, "RegCreateKeyExW");
      g_prof_reg.close = (ProfRegClose)GetProcAddress(m, "RegCloseKey");
      g_prof_reg.enum_value = (ProfRegEnumValue)GetProcAddress(m, "RegEnumValueW");
      g_prof_reg.get_value = (ProfRegGetValue)GetProcAddress(m, "RegGetValueW");
      g_prof_reg.set_value = (ProfRegSetValue)GetProcAddress(m, "RegSetValueExW");
      g_prof_reg.delete_value = (ProfRegDeleteValue)GetProcAddress(m, "RegDeleteValueW");
      g_prof_reg.ok = g_prof_reg.open && g_prof_reg.create && g_prof_reg.close && g_prof_reg.enum_value && g_prof_reg.get_value && g_prof_reg.set_value && g_prof_reg.delete_value;
    }
  }
  const BOOL ok = g_prof_reg.ok;
  prof_leave();
  return ok;
}

static const WCHAR *prof_file_name_part(const WCHAR *path) {
  const WCHAR *name = path;
  for (const WCHAR *p = path; *p; p++) if ((*p == '\\' || *p == '/' || *p == ':') && p[1]) name = p + 1;
  return name;
}

static HANDLE prof_open_file_mapping_key(const WCHAR *filename) {
  static HANDLE mapping_key;
  HANDLE key;
  if (!filename || !prof_reg_load()) return NULL;
  prof_enter();
  if (!mapping_key && g_prof_reg.open(PROF_HKLM, L"Software\\Microsoft\\Windows NT\\CurrentVersion\\IniFileMapping", 0, 0x0008 | 0x0100, &mapping_key)) mapping_key = NULL;
  prof_leave();
  if (mapping_key && !g_prof_reg.open(mapping_key, prof_file_name_part(filename), 0, 0x20019, &key)) return key;
  return NULL;
}

static WCHAR *prof_enum_key(HANDLE key, DWORD i) {
  DWORD max = 256, len;
  LONG res;
  WCHAR *value = prof_alloc(max * sizeof(WCHAR)), *grown;
  if (!value) return NULL;
  len = max;
  while ((res = g_prof_reg.enum_value(key, i, value, &len, NULL, NULL, NULL, NULL)) == ERROR_MORE_DATA) {
    if (max > 0x7fffffffu / 2) { prof_free(value); return NULL; }
    max *= 2;
    if (!(grown = HeapReAlloc(GetProcessHeap(), 0, value, max * sizeof(WCHAR)))) { prof_free(value); return NULL; }
    value = grown;
    len = max;
  }
  if (!res) return value;
  prof_free(value);
  return NULL;
}

static WCHAR *prof_get_key_value(HANDLE key, const WCHAR *value) {
  DWORD size = 0;
  if (g_prof_reg.get_value(key, NULL, value, 0x2 /* RRF_RT_REG_SZ */ | 0x10000000 /* RRF_NOEXPAND */, NULL, NULL, &size)) return NULL;
  WCHAR *data = prof_alloc(size + sizeof(WCHAR));
  if (!data) return NULL;
  if (!g_prof_reg.get_value(key, NULL, value, 0x2 | 0x10000000, NULL, data, &size)) return data;
  prof_free(data);
  return NULL;
}

static HANDLE prof_open_mapped_key(const WCHAR *path, BOOL write) {
  static const WCHAR usr[] = {'U', 'S', 'R', ':'}, sys[] = {'S', 'Y', 'S', ':'};
  static const WCHAR software[] = {'S', 'o', 'f', 't', 'w', 'a', 'r', 'e', '\\', 0};
  const WCHAR *p = path;
  HANDLE key;
  LONG res;
  while (*p == '!' || *p == '#' || *p == '@') p++;      /* (the modifiers are ignored) */
  if (!wcsncmp(p, usr, 4)) {
    if (write) res = g_prof_reg.create(PROF_HKCU, p + 4, 0, NULL, 0, 0x20019 | 0x20006, NULL, &key, NULL);
    else res = g_prof_reg.open(PROF_HKCU, p + 4, 0, 0x20019, &key);
    return res ? NULL : key;
  }
  if (!wcsncmp(p, sys, 4)) {
    p += 4;
    WCHAR *combined = prof_alloc(((size_t_)9 + (size_t_)prof_len(p) + 1) * sizeof(WCHAR));
    if (!combined) return NULL;
    prof_set_str(combined, software);
    prof_set_str(combined + 9, p);
    if (write) res = g_prof_reg.create(PROF_HKLM, combined, 0, NULL, 0, 0x20019 | 0x20006, NULL, &key, NULL);
    else res = g_prof_reg.open(PROF_HKLM, combined, 0, 0x20019, &key);
    prof_free(combined);
    return res ? NULL : key;
  }
  return NULL;
}

/* TRUE if the given section and name are mapped. */
static BOOL prof_get_mapped_section_key(const WCHAR *filename, const WCHAR *section, const WCHAR *name, BOOL write, HANDLE *ret_key) {
  WCHAR *path = NULL;
  HANDLE key, subkey = NULL;
  *ret_key = NULL;
  if (!(key = prof_open_file_mapping_key(filename))) return FALSE;
  if (!g_prof_reg.open(key, section, 0, 0x20019, &subkey)) {
    if (!(path = prof_get_key_value(subkey, name))) path = prof_get_key_value(subkey, NULL);
    g_prof_reg.close(subkey);
    g_prof_reg.close(key);
    if (!path) return FALSE;
  } else {
    if (!(path = prof_get_key_value(key, section))) {
      if ((path = prof_get_key_value(key, NULL))) {
        WCHAR *combined = prof_alloc(((size_t_)prof_len(path) + (size_t_)prof_len(section) + 2) * sizeof(WCHAR));
        if (combined) {
          prof_set_str(combined, path);
          combined[prof_len(combined) + 1] = 0;
          combined[prof_len(combined)] = '\\';
          prof_set_str(combined + prof_len(combined), section);
        }
        prof_free(path);
        path = combined;
      }
    }
    g_prof_reg.close(key);
    if (!path) return FALSE;
  }
  *ret_key = prof_open_mapped_key(path, write);
  prof_free(path);
  return TRUE;
}

static DWORD prof_min(DWORD a, DWORD b) { return a < b ? a : b; }

static DWORD prof_get_mapped_section(HANDLE key, WCHAR *buffer, DWORD size, BOOL return_values) {
  WCHAR *entry, *value;
  DWORD i, ret = 0;
  for (i = 0; (entry = prof_enum_key(key, i)); ++i) {
    lstrcpynW(buffer + ret, entry, (int)(size - ret - 1));
    ret = prof_min(ret + (DWORD)prof_len(entry) + 1, size - 1);
    if (return_values && ret < size - 1 && (value = prof_get_key_value(key, entry))) {
      buffer[ret - 1] = '=';
      lstrcpynW(buffer + ret, value, (int)(size - ret - 1));
      ret = prof_min(ret + (DWORD)prof_len(value) + 1, size - 1);
      prof_free(value);
    }
    prof_free(entry);
  }
  return ret;
}

static DWORD prof_section(const WCHAR *filename, const WCHAR *section, WCHAR *buffer, DWORD size, BOOL return_values) {
  HANDLE key, subkey, section_key;
  BOOL use_ini = TRUE;
  DWORD ret = 0;
  if ((key = prof_open_file_mapping_key(filename))) {
    if (!g_prof_reg.open(key, section, 0, 0x20019, &subkey)) {
      WCHAR *entry, *value, *path;
      HANDLE entry_key;
      DWORD i;
      for (i = 0; (entry = prof_enum_key(subkey, i)); ++i) {
        if (!(path = prof_get_key_value(subkey, entry))) { prof_free(entry); continue; }
        entry_key = prof_open_mapped_key(path, FALSE);
        prof_free(path);
        if (!entry_key) { prof_free(entry); continue; }
        if (entry[0]) {
          if ((value = prof_get_key_value(entry_key, entry))) {
            lstrcpynW(buffer + ret, entry, (int)(size - ret - 1));
            ret = prof_min(ret + (DWORD)prof_len(entry) + 1, size - 1);
            if (return_values && ret < size - 1) {
              buffer[ret - 1] = '=';
              lstrcpynW(buffer + ret, value, (int)(size - ret - 1));
              ret = prof_min(ret + (DWORD)prof_len(value) + 1, size - 1);
            }
            prof_free(value);
          }
        } else {
          ret += prof_get_mapped_section(entry_key, buffer + ret, size - ret, return_values);
          use_ini = FALSE;
        }
        prof_free(entry);
        g_prof_reg.close(entry_key);
      }
      g_prof_reg.close(subkey);
    } else if (prof_get_mapped_section_key(filename, section, NULL, FALSE, &section_key)) {
      if (section_key) { ret = prof_get_mapped_section(section_key, buffer, size, return_values); g_prof_reg.close(section_key); }
      use_ini = FALSE;
    }
    g_prof_reg.close(key);
  }
  if (use_ini) ret += (DWORD)prof_get_section(filename, section, buffer + ret, size - ret, return_values);
  return ret;
}

static void prof_delete_key_values(HANDLE key) {
  WCHAR *entry;
  while ((entry = prof_enum_key(key, 0))) {
    g_prof_reg.delete_value(key, entry);
    prof_free(entry);
  }
}

static BOOL prof_remove_section(const WCHAR *filename, const WCHAR *section) {
  HANDLE key, subkey, section_key;
  BOOL mapped = FALSE;      /* (a file that has a mapping is not made when its section is deleted) */
  if ((key = prof_open_file_mapping_key(filename))) {
    mapped = TRUE;
    if (!g_prof_reg.open(key, section, 0, 0x20019, &subkey)) {
      WCHAR *entry, *path;
      HANDLE entry_key;
      DWORD i;
      for (i = 0; (entry = prof_enum_key(subkey, i)); ++i) {
        if (!(path = prof_get_key_value(subkey, entry))) { prof_free(entry); continue; }
        entry_key = prof_open_mapped_key(path, TRUE);
        prof_free(path);
        if (!entry_key) { prof_free(entry); continue; }
        if (entry[0]) g_prof_reg.delete_value(entry_key, entry);
        else prof_delete_key_values(entry_key);
        prof_free(entry);
        g_prof_reg.close(entry_key);
      }
      g_prof_reg.close(subkey);
    } else if (prof_get_mapped_section_key(filename, section, NULL, TRUE, &section_key)) {
      if (section_key) { prof_delete_key_values(section_key); g_prof_reg.close(section_key); }
    }
    g_prof_reg.close(key);
  }
  return prof_delete_section(filename, section, !mapped);
}

/* ---- the API ---- */

EXPORT DWORD WINAPI GetPrivateProfileSectionNamesW(LPWSTR buffer, DWORD size, LPCWSTR filename);

EXPORT UINT WINAPI GetPrivateProfileIntW(LPCWSTR section, LPCWSTR entry, INT def_val, LPCWSTR filename);

static DWORD prof_get_string(LPCWSTR section, LPCWSTR entry, LPCWSTR def_val, LPWSTR buffer, DWORD len, LPCWSTR filename);
EXPORT DWORD WINAPI GetPrivateProfileStringW(LPCWSTR section, LPCWSTR entry, LPCWSTR def_val, LPWSTR buffer, DWORD len, LPCWSTR filename) {
  const DWORD error = GetLastError();      /* (looking a string up leaves the last error alone) */
  const DWORD r = prof_get_string(section, entry, def_val, buffer, len, filename);
  SetLastError(error);
  return r;
}
static DWORD prof_get_string(LPCWSTR section, LPCWSTR entry, LPCWSTR def_val, LPWSTR buffer, DWORD len, LPCWSTR filename) {
  static const WCHAR empty[] = {0}, win_ini[] = {'w', 'i', 'n', '.', 'i', 'n', 'i', 0};
  (void)win_ini;
  int ret;
  WCHAR *defval_tmp = NULL;
  HANDLE key;
  if (!buffer || !len) return 0;
  if (!def_val) def_val = empty;
  if (!section) return GetPrivateProfileSectionNamesW(buffer, len, filename);
  if (!entry) {
    ret = (int)prof_section(filename, section, buffer, len, FALSE);
    if (!buffer[0]) { prof_copy_entry(buffer, def_val, (int)len); ret = prof_len(buffer); }
    return (DWORD)ret;
  }
  const WCHAR *p = def_val + prof_len(def_val) - 1;      /* trailing blanks of the default go */
  while (p > def_val && *p == ' ') p--;
  if (p >= def_val) {
    const int vlen = (int)(p - def_val) + 1;
    defval_tmp = prof_alloc(((size_t_)vlen + 1) * sizeof(WCHAR));
    if (defval_tmp) { memcpy(defval_tmp, def_val, (size_t_)vlen * sizeof(WCHAR)); defval_tmp[vlen] = 0; def_val = defval_tmp; }
  }
  if (prof_get_mapped_section_key(filename, section, entry, FALSE, &key)) {
    if (key) {
      WCHAR *value = prof_get_key_value(key, entry);
      if (value) { lstrcpynW(buffer, value, (int)len); prof_free(value); }
      else lstrcpynW(buffer, def_val, (int)len);
      g_prof_reg.close(key);
    } else lstrcpynW(buffer, def_val, (int)len);
    ret = prof_len(buffer);
  } else {
    prof_enter();
    if (prof_open(filename, FALSE, FALSE)) {
      ProfKey *k = prof_find(&PROF_CUR->section, section, entry, FALSE, FALSE);
      prof_copy_entry(buffer, (k && k->value) ? k->value : def_val, (int)len);
      ret = prof_len(buffer);
    } else {
      lstrcpynW(buffer, def_val, (int)len);
      ret = prof_len(buffer);
    }
    prof_leave();
  }
  prof_free(defval_tmp);
  return (DWORD)ret;
}

EXPORT DWORD WINAPI GetPrivateProfileStringA(LPCSTR section, LPCSTR entry, LPCSTR def_val, LPSTR buffer, DWORD len, LPCSTR filename) {
  WCHAR *sectionW = to_wide(section), *entryW = to_wide(entry), *defW = to_wide(def_val), *fileW = to_wide(filename);
  WCHAR *bufferW = buffer ? prof_alloc((size_t_)len * sizeof(WCHAR)) : NULL;
  int ret = 0;
  const int retW = (int)GetPrivateProfileStringW(sectionW, entryW, defW, bufferW, len, fileW);
  if (len && buffer) {
    if (retW) {
      ret = WideCharToMultiByte(CP_ACP, 0, bufferW, retW, buffer, (int)len - 1, NULL, NULL);
      if (!ret) ret = (int)len - 1;
    }
    buffer[ret] = 0;
    if ((!section || !entry) && (DWORD)ret + 1 < len) buffer[ret + 1] = 0;      /* (a list of names ends with two NULs) */
  }
  free_wide(sectionW); free_wide(entryW); free_wide(defW); free_wide(fileW);
  prof_free(bufferW);
  return (DWORD)ret;
}

EXPORT DWORD WINAPI GetProfileStringA(LPCSTR section, LPCSTR entry, LPCSTR def_val, LPSTR buffer, DWORD len) { return GetPrivateProfileStringA(section, entry, def_val, buffer, len, "win.ini"); }
EXPORT DWORD WINAPI GetProfileStringW(LPCWSTR section, LPCWSTR entry, LPCWSTR def_val, LPWSTR buffer, DWORD len) {
  static const WCHAR win_ini[] = {'w', 'i', 'n', '.', 'i', 'n', 'i', 0};
  return GetPrivateProfileStringW(section, entry, def_val, buffer, len, win_ini);
}

EXPORT UINT WINAPI GetPrivateProfileIntW(LPCWSTR section, LPCWSTR entry, INT def_val, LPCWSTR filename) {
  static const WCHAR empty[] = {0};
  WCHAR buffer[30];
  UNICODE_STRING text;
  ULONG result = 0;
  if (GetPrivateProfileStringW(section, entry, empty, buffer, 30, filename) == 0) return (UINT)def_val;
  if (!buffer[0]) return (UINT)def_val;
  RtlInitUnicodeString(&text, buffer);
  RtlUnicodeStringToInteger(&text, 0, &result);
  return result;
}
EXPORT UINT WINAPI GetPrivateProfileIntA(LPCSTR section, LPCSTR entry, INT def_val, LPCSTR filename) {
  WCHAR *s = to_wide(section), *e = to_wide(entry), *f = to_wide(filename);
  const UINT r = GetPrivateProfileIntW(s, e, def_val, f);
  free_wide(s); free_wide(e); free_wide(f);
  return r;
}
EXPORT UINT WINAPI GetProfileIntA(LPCSTR section, LPCSTR entry, INT def_val) { return GetPrivateProfileIntA(section, entry, def_val, "win.ini"); }
EXPORT UINT WINAPI GetProfileIntW(LPCWSTR section, LPCWSTR entry, INT def_val) {
  static const WCHAR win_ini[] = {'w', 'i', 'n', '.', 'i', 'n', 'i', 0};
  return GetPrivateProfileIntW(section, entry, def_val, win_ini);
}

EXPORT DWORD WINAPI GetPrivateProfileSectionW(LPCWSTR section, LPWSTR buffer, DWORD len, LPCWSTR filename) {
  if (!section || !buffer) { SetLastError(ERROR_INVALID_PARAMETER); return 0; }
  return prof_section(filename, section, buffer, len, TRUE);
}

EXPORT DWORD WINAPI GetPrivateProfileSectionA(LPCSTR section, LPSTR buffer, DWORD len, LPCSTR filename) {
  int ret = 0;
  if (!section || !buffer) { SetLastError(ERROR_INVALID_PARAMETER); return 0; }
  if (len > 0x7fffffffu / 2 / sizeof(WCHAR)) len = 0x7fffffffu / 2 / sizeof(WCHAR);
  WCHAR *bufferW = prof_alloc((size_t_)len * 2 * sizeof(WCHAR)), *sectionW = to_wide(section), *fileW = to_wide(filename);
  const int retW = bufferW ? (int)GetPrivateProfileSectionW(sectionW, bufferW, len * 2, fileW) : 0;
  const DWORD error = GetLastError();
  if (retW) {
    int n = retW;
    if ((DWORD)n == len * 2 - 2) n++;      /* overflow */
    ret = WideCharToMultiByte(CP_ACP, 0, bufferW, n + 1, buffer, (int)len, NULL, NULL);
    if (!ret || (DWORD)ret == len) { ret = (int)len - 2; buffer[len - 2] = 0; buffer[len - 1] = 0; }
    else ret--;
  } else if (len >= 2) { buffer[0] = 0; buffer[1] = 0; }
  free_wide(sectionW); free_wide(fileW);
  prof_free(bufferW);
  SetLastError(error);
  return (DWORD)ret;
}
EXPORT DWORD WINAPI GetProfileSectionA(LPCSTR section, LPSTR buffer, DWORD len) { return GetPrivateProfileSectionA(section, buffer, len, "win.ini"); }
EXPORT DWORD WINAPI GetProfileSectionW(LPCWSTR section, LPWSTR buffer, DWORD len) {
  static const WCHAR win_ini[] = {'w', 'i', 'n', '.', 'i', 'n', 'i', 0};
  return GetPrivateProfileSectionW(section, buffer, len, win_ini);
}

EXPORT BOOL WINAPI WritePrivateProfileStringW(LPCWSTR section, LPCWSTR entry, LPCWSTR string, LPCWSTR filename) {
  BOOL ret = FALSE;
  HANDLE key;
  if (!section && !entry && !string) {      /* the documented "flush the file" case */
    prof_enter();
    if (!filename || prof_open(filename, TRUE, FALSE)) { if (PROF_CUR) prof_release(); }
    prof_leave();
    return TRUE;
  }
  if (!entry) return prof_remove_section(filename, section);
  if (prof_get_mapped_section_key(filename, section, entry, TRUE, &key)) {
    LONG res = ERROR_FILE_NOT_FOUND;
    if (key) {
      if (string) res = g_prof_reg.set_value(key, entry, 0, 1 /* REG_SZ */, (const BYTE *)string, ((DWORD)prof_len(string) + 1) * sizeof(WCHAR));
      else res = g_prof_reg.delete_value(key, entry);
      g_prof_reg.close(key);
    }
    if (res) SetLastError((DWORD)res);
    return !res;
  }
  prof_enter();
  if (prof_open(filename, TRUE, TRUE)) {
    if (!section) SetLastError(ERROR_FILE_NOT_FOUND);
    else ret = prof_set_string(section, entry, string, FALSE);
    if (ret) ret = prof_flush();
  }
  prof_leave();
  return ret;
}

EXPORT BOOL WINAPI WritePrivateProfileStringA(LPCSTR section, LPCSTR entry, LPCSTR string, LPCSTR filename) {
  WCHAR *s = to_wide(section), *e = to_wide(entry), *v = to_wide(string), *f = to_wide(filename);
  const BOOL r = WritePrivateProfileStringW(s, e, v, f);
  free_wide(s); free_wide(e); free_wide(v); free_wide(f);
  return r;
}
EXPORT BOOL WINAPI WriteProfileStringA(LPCSTR section, LPCSTR entry, LPCSTR string) { return WritePrivateProfileStringA(section, entry, string, "win.ini"); }
EXPORT BOOL WINAPI WriteProfileStringW(LPCWSTR section, LPCWSTR entry, LPCWSTR string) {
  static const WCHAR win_ini[] = {'w', 'i', 'n', '.', 'i', 'n', 'i', 0};
  return WritePrivateProfileStringW(section, entry, string, win_ini);
}

EXPORT BOOL WINAPI WritePrivateProfileSectionW(LPCWSTR section, LPCWSTR string, LPCWSTR filename) {
  BOOL ret = FALSE;
  HANDLE key, section_key;
  if (!section && !string) {
    prof_enter();
    if (!filename || prof_open(filename, TRUE, FALSE)) { if (PROF_CUR) prof_release(); }
    prof_leave();
    return FALSE;
  }
  if (!string) return prof_remove_section(filename, section);
  if ((key = prof_open_file_mapping_key(filename))) {
    /* existing entries are replaced, but only the mapped ones, and none is deleted */
    for (const WCHAR *entry = string; *entry; entry += prof_len(entry) + 1) {
      const WCHAR *p = wcschr(entry, '=');
      if (!p) continue;
      p++;
      WCHAR *copy = prof_alloc((size_t_)(p - entry) * sizeof(WCHAR));
      if (!copy) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); g_prof_reg.close(key); return FALSE; }
      lstrcpynW(copy, entry, (int)(p - entry));
      if (prof_get_mapped_section_key(filename, section, copy, TRUE, &section_key) && section_key) {
        const LONG res = g_prof_reg.set_value(section_key, copy, 0, 1, (const BYTE *)p, ((DWORD)prof_len(p) + 1) * sizeof(WCHAR));
        g_prof_reg.close(section_key);
        if (res) { prof_free(copy); SetLastError((DWORD)res); g_prof_reg.close(key); return FALSE; }
      }
      prof_free(copy);
    }
    g_prof_reg.close(key);
    return TRUE;
  }
  prof_enter();
  if (prof_open(filename, TRUE, TRUE)) {
    prof_delete_all_keys(section);
    ret = TRUE;
    while (*string && ret) {
      WCHAR *buf = prof_alloc(((size_t_)prof_len(string) + 1) * sizeof(WCHAR));
      if (!buf) { ret = FALSE; break; }
      prof_set_str(buf, string);
      WCHAR *p = wcschr(buf, '=');
      if (p) { *p = 0; ret = prof_set_string(section, buf, p + 1, TRUE); }
      prof_free(buf);
      string += prof_len(string) + 1;
    }
    if (ret) ret = prof_flush();
  }
  prof_leave();
  return ret;
}

EXPORT BOOL WINAPI WritePrivateProfileSectionA(LPCSTR section, LPCSTR string, LPCSTR filename) {
  WCHAR *stringW = NULL;
  if (string) {
    const char *p = string;
    while (*p) p += strlen(p) + 1;
    const int lenA = (int)(p - string) + 1;
    const int lenW = MultiByteToWideChar(CP_ACP, 0, string, lenA, NULL, 0);
    if (lenW > 0 && (stringW = prof_alloc((size_t_)lenW * sizeof(WCHAR)))) MultiByteToWideChar(CP_ACP, 0, string, lenA, stringW, lenW);
  }
  WCHAR *s = to_wide(section), *f = to_wide(filename);
  const BOOL r = WritePrivateProfileSectionW(s, stringW, f);
  prof_free(stringW); free_wide(s); free_wide(f);
  return r;
}
EXPORT BOOL WINAPI WriteProfileSectionA(LPCSTR section, LPCSTR keys_n_values) { return WritePrivateProfileSectionA(section, keys_n_values, "win.ini"); }
EXPORT BOOL WINAPI WriteProfileSectionW(LPCWSTR section, LPCWSTR keys_n_values) {
  static const WCHAR win_ini[] = {'w', 'i', 'n', '.', 'i', 'n', 'i', 0};
  return WritePrivateProfileSectionW(section, keys_n_values, win_ini);
}

EXPORT DWORD WINAPI GetPrivateProfileSectionNamesW(LPWSTR buffer, DWORD size, LPCWSTR filename) {
  DWORD ret = 0;
  HANDLE key;
  if ((key = prof_open_file_mapping_key(filename))) {
    WCHAR *section;
    for (DWORD i = 0; (section = prof_enum_key(key, i)); ++i) {
      lstrcpynW(buffer + ret, section, (int)(size - ret - 1));
      ret = prof_min(ret + (DWORD)prof_len(section) + 1, size - 1);
      prof_free(section);
    }
    g_prof_reg.close(key);
  }
  prof_enter();
  if (prof_open(filename, FALSE, FALSE)) ret += (DWORD)prof_get_section_names(buffer + ret, size - ret);
  prof_leave();
  return ret;
}

EXPORT DWORD WINAPI GetPrivateProfileSectionNamesA(LPSTR buffer, DWORD size, LPCSTR filename) {
  int ret = 0;
  WCHAR *f = to_wide(filename);
  WCHAR *bufferW = buffer ? prof_alloc((size_t_)size * sizeof(WCHAR)) : NULL;
  const int retW = (int)GetPrivateProfileSectionNamesW(bufferW, size, f);
  if (retW && size) {
    ret = WideCharToMultiByte(CP_ACP, 0, bufferW, retW + 1, buffer, (int)size - 1, NULL, NULL);
    if (!ret) { ret = (int)size - 2; buffer[size - 1] = 0; }
    else ret--;
    if (ret >= 0 && (DWORD)ret < size) buffer[ret] = 0;      /* the list ends with two NULs even when it fills the buffer */
    if (ret >= 0 && (DWORD)ret + 1 < size) buffer[ret + 1] = 0;
  } else if (size) buffer[0] = 0;
  free_wide(f);
  prof_free(bufferW);
  return (DWORD)ret;
}

static int prof_hex_digit(WCHAR c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'A' && c <= 'Z') return c - 'A' + 10;
  if (c >= 'a' && c <= 'z') return c - 'a' + 10;
  return -1;
}
static int prof_hex_byte(const WCHAR *p) {
  const int hi = prof_hex_digit(p[0]);
  if (hi < 0) return -1;
  const int lo = prof_hex_digit(p[1]);
  if (lo < 0) return -1;
  return (hi << 4) + lo;
}

EXPORT BOOL WINAPI GetPrivateProfileStructW(LPCWSTR section, LPCWSTR key, LPVOID buf, UINT len, LPCWSTR filename) {
  BOOL ret = FALSE;
  BYTE *data = buf, chksum = 0;
  int val;
  if (len > (0x7fffffffu - 4) / 2) return FALSE;
  WCHAR *buffer = prof_alloc((2 * (size_t_)len + 4) * sizeof(WCHAR)), *p;
  if (!buffer) return FALSE;
  if (GetPrivateProfileStringW(section, key, NULL, buffer, 2 * len + 4, filename) != 2 * len + 2) { SetLastError(ERROR_BAD_LENGTH); goto done; }
  for (p = buffer; len; p += 2, len--) {
    if ((val = prof_hex_byte(p)) == -1) { SetLastError(ERROR_INVALID_DATA); goto done; }
    *data++ = (BYTE)val;
    chksum = (BYTE)(chksum + val);
  }
  if ((val = prof_hex_byte(p)) == -1) { SetLastError(ERROR_INVALID_DATA); goto done; }
  ret = (BYTE)val == chksum;
  if (!ret) SetLastError(ERROR_INVALID_DATA);
done:
  prof_free(buffer);
  return ret;
}
EXPORT BOOL WINAPI GetPrivateProfileStructA(LPCSTR section, LPCSTR key, LPVOID buffer, UINT len, LPCSTR filename) {
  WCHAR *s = to_wide(section), *k = to_wide(key), *f = to_wide(filename);
  const BOOL r = GetPrivateProfileStructW(s, k, buffer, len, f);
  free_wide(s); free_wide(k); free_wide(f);
  return r;
}

EXPORT BOOL WINAPI WritePrivateProfileStructW(LPCWSTR section, LPCWSTR key, LPVOID buf, UINT bufsize, LPCWSTR filename) {
  static const char hex[] = "0123456789ABCDEF";
  if (!section && !key && !buf) return WritePrivateProfileStringW(NULL, NULL, NULL, filename);      /* flush the cache */
  if (!buf) return WritePrivateProfileStringW(section, key, NULL, filename);
  if (bufsize > (0x7fffffffu - 3) / 2) return FALSE;
  WCHAR *out = prof_alloc(((size_t_)bufsize * 2 + 3) * sizeof(WCHAR)), *p = out;
  if (!out) return FALSE;
  DWORD sum = 0;
  for (const BYTE *b = buf; b < (const BYTE *)buf + bufsize; b++) { *p++ = (WCHAR)hex[*b >> 4]; *p++ = (WCHAR)hex[*b & 0xf]; sum += *b; }
  *p++ = (WCHAR)hex[(sum & 0xf0) >> 4];
  *p++ = (WCHAR)hex[sum & 0xf];
  *p++ = 0;
  const BOOL ret = WritePrivateProfileStringW(section, key, out, filename);
  prof_free(out);
  return ret;
}
EXPORT BOOL WINAPI WritePrivateProfileStructA(LPCSTR section, LPCSTR key, LPVOID buf, UINT bufsize, LPCSTR filename) {
  WCHAR *s = to_wide(section), *k = to_wide(key), *f = to_wide(filename);
  const BOOL r = WritePrivateProfileStructW(s, k, buf, bufsize, f);
  free_wide(s); free_wide(k); free_wide(f);
  return r;
}

EXPORT BOOL WINAPI OpenProfileUserMapping(void) { SetLastError(ERROR_CALL_NOT_IMPLEMENTED); return FALSE; }
EXPORT BOOL WINAPI CloseProfileUserMapping(void) { SetLastError(ERROR_CALL_NOT_IMPLEMENTED); return FALSE; }
