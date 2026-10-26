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

/* ntdll.dll for TheoryCraftOS: the system-call stubs (the numbers are this OS's own), the heap, the Rtl* string and error helpers, critical sections,
 * the C functions a program may take from ntdll, and the process start-up (RtlUserThreadStart). Single-threaded: the process has one thread, so the locks
 * only count. Built freestanding with the OS's mingw compiler; kernel32.dll imports from it.
 *
 * Compile with -ffreestanding -fno-builtin -fno-tree-loop-distribute-patterns: the compiler must not turn the loops of memcpy and memset into calls to themselves. */
#define IMPORT
#include "../tcos_win.h"
#include "../tcos_syscalls.h"

/* ---- code the compiler expects from the platform ------------------------------------------------------------------------------------------------------- */

/* A function with a big frame calls this to touch its pages; the stack here is mapped in full, so there is nothing to do. */
__asm__(".text\n.globl ___chkstk_ms\n___chkstk_ms:\n ret\n");

/* The service stubs: "mov r10, rcx; mov eax, number; syscall; ret". Arguments past the fourth stay on the stack where the caller put them. */
#define NT_SERVICE(name)                                                                                                                                      \
  __asm__(".text\n.globl " #name "\n" #name ":\n mov %rcx, %r10\n mov $" _STR(NT_SERVICE_##name) ", %eax\n syscall\n ret\n.section .drectve\n.ascii \" -export:" #name "\"\n.text\n")
#define _STR(x) _STR2(x)
#define _STR2(x) #x
NT_SERVICE(NtReadFile);
NT_SERVICE(NtWriteFile);
NT_SERVICE(NtClose);
NT_SERVICE(NtTerminateProcess);
NT_SERVICE(NtYieldExecution);
NT_SERVICE(NtDelayExecution);
NT_SERVICE(NtQuerySystemTime);
NT_SERVICE(NtAllocateVirtualMemory);
NT_SERVICE(NtFreeVirtualMemory);
NT_SERVICE(NtProtectVirtualMemory);
NT_SERVICE(NtCreateFile);
NT_SERVICE(NtOpenFile);
NT_SERVICE(NtQueryInformationFile);
NT_SERVICE(NtSetInformationFile);
NT_SERVICE(NtQueryDirectoryFile);
NT_SERVICE(NtQueryAttributesFile);
NT_SERVICE(NtDeleteFile);
NT_SERVICE(NtFlushBuffersFile);
NT_SERVICE(NtQueryVolumeInformationFile);
NT_SERVICE(NtQueryPerformanceCounter);
NT_SERVICE(NtQueryVirtualMemory);
NT_SERVICE(NtQueryInformationProcess);

/* ---- the C library functions ntdll exports (shared with msvcrt.dll) -------------------------------------------------------------------------------------- */

#include "../libc_string.inc"

/* ---- formatting (the narrow printf family) ------------------------------------------------------------------------------------------------------------- */

typedef struct { char *out; size_t_ capacity, length; } Sink;
static void sink_put(Sink *s, char c) {
  if (s->length + 1 < s->capacity) s->out[s->length] = c;
  ++s->length;
}
static void sink_pad(Sink *s, char c, int n) { while (n-- > 0) sink_put(s, c); }

EXPORT int vsnprintf(char *buffer, size_t_ capacity, const char *format, va_list_ args) {
  Sink sink = {buffer, capacity, 0};
  for (const char *f = format; *f; ++f) {
    if (*f != '%') { sink_put(&sink, *f); continue; }
    ++f;
    int left = 0, zero = 0, plus = 0, space = 0, alt = 0, width = 0, precision = -1, size = 0;
    for (;; ++f) {
      if (*f == '-') left = 1; else if (*f == '0') zero = 1; else if (*f == '+') plus = 1; else if (*f == ' ') space = 1; else if (*f == '#') alt = 1; else break;
    }
    if (*f == '*') { width = __builtin_va_arg(args, int); if (width < 0) { left = 1; width = -width; } ++f; }
    else while (isdigit((BYTE)*f)) width = width * 10 + (*f++ - '0');
    if (*f == '.') {
      ++f; precision = 0;
      if (*f == '*') { precision = __builtin_va_arg(args, int); ++f; }
      else while (isdigit((BYTE)*f)) precision = precision * 10 + (*f++ - '0');
    }
    for (;; ++f) {      /* size: 1 = h, 2 = hh, 4 = l (32 bit on Windows), 8 = ll, z, j, t, I64 */
      if (*f == 'h') size = size == 1 ? 2 : 1;
      else if (*f == 'l') size = size == 4 ? 8 : 4;
      else if (*f == 'z' || *f == 'j' || *f == 't') size = 8;
      else if (*f == 'I' && f[1] == '6' && f[2] == '4') { size = 8; f += 2; }
      else if (*f == 'I' && f[1] == '3' && f[2] == '2') { size = 0; f += 2; }
      else break;
    }
    const char conversion = *f;
    if (!conversion) break;
    char text[80];
    const char *body = text;
    int length = 0;
    char sign = 0;
    const char *digits = "0123456789abcdef";
    switch (conversion) {
      case '%': sink_put(&sink, '%'); continue;
      case 'c': text[0] = (char)__builtin_va_arg(args, int); length = 1; break;
      case 's': {
        body = __builtin_va_arg(args, const char *);
        if (!body) body = "(null)";
        while (body[length] && (precision < 0 || length < precision)) ++length;
        break;
      }
      case 'd': case 'i': case 'u': case 'x': case 'X': case 'o': case 'p': {
        unsigned long long value;
        int is_signed = conversion == 'd' || conversion == 'i';
        unsigned base = conversion == 'o' ? 8 : (conversion == 'x' || conversion == 'X' || conversion == 'p') ? 16 : 10;
        if (conversion == 'X') digits = "0123456789ABCDEF";
        if (conversion == 'p') { value = (unsigned long long)__builtin_va_arg(args, void *); is_signed = 0; if (precision < 0) precision = 16; digits = "0123456789ABCDEF"; }
        else if (is_signed) {
          long long v = size == 8 ? __builtin_va_arg(args, long long) : __builtin_va_arg(args, int);
          if (size == 1) v = (short)v; else if (size == 2) v = (signed char)v;
          if (v < 0) { sign = '-'; value = (unsigned long long)-v; } else { value = (unsigned long long)v; if (plus) sign = '+'; else if (space) sign = ' '; }
        } else {
          value = size == 8 ? __builtin_va_arg(args, unsigned long long) : __builtin_va_arg(args, unsigned int);
          if (size == 1) value &= 0xFFFF; else if (size == 2) value &= 0xFF;
        }
        char reversed[72];
        int n = 0;
        do { reversed[n++] = digits[value % base]; value /= base; } while (value);
        while (precision >= 0 && n < precision && n < 70) reversed[n++] = '0';
        if (alt && base == 16 && conversion != 'p') { reversed[n++] = conversion; reversed[n++] = '0'; }
        for (int i = 0; i < n; ++i) text[i] = reversed[n - 1 - i];
        length = n;
        if (precision >= 0 && conversion != 'p') zero = 0;
        break;
      }
      default: sink_put(&sink, '%'); sink_put(&sink, conversion); continue;
    }
    int pad = width - length - (sign ? 1 : 0);
    if (!left && !zero) sink_pad(&sink, ' ', pad);
    if (sign) sink_put(&sink, sign);
    if (!left && zero && conversion != 's' && conversion != 'c') sink_pad(&sink, '0', pad);
    for (int i = 0; i < length; ++i) sink_put(&sink, body[i]);
    if (left) sink_pad(&sink, ' ', pad);
  }
  if (capacity) sink.out[sink.length < capacity ? sink.length : capacity - 1] = 0;
  return (int)sink.length;
}
EXPORT int _vsnprintf(char *b, size_t_ n, const char *f, va_list_ a) { return vsnprintf(b, n, f, a); }
EXPORT int vsprintf(char *b, const char *f, va_list_ a) { return vsnprintf(b, (size_t_)-1 >> 1, f, a); }
EXPORT int snprintf(char *b, size_t_ n, const char *f, ...) { va_list_ a; __builtin_va_start(a, f); int r = vsnprintf(b, n, f, a); __builtin_va_end(a); return r; }
EXPORT int _snprintf(char *b, size_t_ n, const char *f, ...) { va_list_ a; __builtin_va_start(a, f); int r = vsnprintf(b, n, f, a); __builtin_va_end(a); return r; }
EXPORT int sprintf(char *b, const char *f, ...) { va_list_ a; __builtin_va_start(a, f); int r = vsnprintf(b, (size_t_)-1 >> 1, f, a); __builtin_va_end(a); return r; }

/* ---- last error, status codes, the process --------------------------------------------------------------------------------------------------------------- */

EXPORT PEB *NTAPI RtlGetCurrentPeb(void) { return NtCurrentPeb(); }
EXPORT ULONG NTAPI RtlGetLastWin32Error(void) { return TEB_LAST_ERROR(NtCurrentTeb()); }
EXPORT void NTAPI RtlSetLastWin32Error(ULONG error) { TEB_LAST_ERROR(NtCurrentTeb()) = error; }

EXPORT ULONG NTAPI RtlNtStatusToDosError(NTSTATUS status) {
  switch ((ULONG)status) {
    case 0: return ERROR_SUCCESS;
    case 0x80000005u: return ERROR_MORE_DATA;
    case 0x80000006u: return ERROR_NO_MORE_FILES;
    case 0xC0000002u: return ERROR_INVALID_FUNCTION;
    case 0xC0000003u: case 0xC0000004u: return ERROR_INVALID_PARAMETER;
    case 0xC0000005u: return ERROR_NOACCESS;
    case 0xC0000008u: return ERROR_INVALID_HANDLE;
    case 0xC000000Du: return ERROR_INVALID_PARAMETER;
    case 0xC0000010u: return ERROR_INVALID_FUNCTION;
    case 0xC0000011u: return ERROR_HANDLE_EOF;
    case 0xC0000017u: return ERROR_NOT_ENOUGH_MEMORY;
    case 0xC0000018u: return ERROR_INVALID_ADDRESS;
    case 0xC0000022u: return ERROR_ACCESS_DENIED;
    case 0xC0000033u: return ERROR_INVALID_NAME;
    case 0xC0000034u: return ERROR_FILE_NOT_FOUND;
    case 0xC0000035u: return ERROR_ALREADY_EXISTS;
    case 0xC000003Au: return ERROR_PATH_NOT_FOUND;
    case 0xC0000043u: return ERROR_SHARING_VIOLATION;
    case 0xC000007Fu: return ERROR_DISK_FULL;
    case 0xC00000A2u: return ERROR_ACCESS_DENIED;      /* write-protected */
    case 0xC00000BAu: return ERROR_ACCESS_DENIED;      /* a directory opened as a file */
    case 0xC0000101u: return ERROR_DIR_NOT_EMPTY;
    case 0xC0000103u: return ERROR_DIRECTORY;
    case 0xC000011Fu: return ERROR_TOO_MANY_OPEN_FILES;
    case 0xC0000185u: return ERROR_IO_DEVICE;
    default: return (ULONG)status >= 0xC0000000u ? ERROR_MR_MID_NOT_FOUND : ERROR_INVALID_PARAMETER;
  }
}

EXPORT NTSTATUS NTAPI RtlGetVersion(RTL_OSVERSIONINFOW *info) {
  if (!info) return STATUS_INVALID_PARAMETER;
  info->dwMajorVersion = 10;
  info->dwMinorVersion = 0;
  info->dwBuildNumber = 19045;
  info->dwPlatformId = 2;
  info->szCSDVersion[0] = 0;
  return STATUS_SUCCESS;
}

EXPORT void NTAPI RtlExitUserProcess(NTSTATUS status) {
  for (;;) NtTerminateProcess(NtCurrentProcess(), status);
}

EXPORT void NTAPI RtlExitUserThread(NTSTATUS status) { RtlExitUserProcess(status); }      /* one thread: its end is the process's end */

/* ---- strings ------------------------------------------------------------------------------------------------------------------------------------------- */

EXPORT void NTAPI RtlInitUnicodeString(PUNICODE_STRING s, PCWSTR text) {
  s->Buffer = (PWSTR)text;
  s->Length = s->MaximumLength = 0;
  if (text) {
    const size_t_ n = wcslen(text) * 2;
    s->Length = (USHORT)(n > 0xFFFC ? 0xFFFC : n);
    s->MaximumLength = (USHORT)(s->Length + 2);
  }
}
EXPORT void NTAPI RtlInitAnsiString(PANSI_STRING s, PCSTR text) {
  s->Buffer = (PSTR)text;
  s->Length = s->MaximumLength = 0;
  if (text) {
    const size_t_ n = strlen(text);
    s->Length = (USHORT)(n > 0xFFFE ? 0xFFFE : n);
    s->MaximumLength = (USHORT)(s->Length + 1);
  }
}

/* The ANSI code page of this OS is UTF-8. Decodes one code point; a bad sequence is U+FFFD and consumes one byte. */
static ULONG utf8_next(const BYTE *s, ULONG n, ULONG *used) {
  const BYTE b = s[0];
  ULONG cp, extra;
  if (b < 0x80) { *used = 1; return b; }
  if ((b & 0xE0) == 0xC0) cp = b & 0x1F, extra = 1;
  else if ((b & 0xF0) == 0xE0) cp = b & 0x0F, extra = 2;
  else if ((b & 0xF8) == 0xF0) cp = b & 0x07, extra = 3;
  else { *used = 1; return 0xFFFD; }
  if (extra >= n) { *used = 1; return 0xFFFD; }
  for (ULONG k = 1; k <= extra; ++k) {
    if ((s[k] & 0xC0) != 0x80) { *used = 1; return 0xFFFD; }
    cp = (cp << 6) | (s[k] & 0x3F);
  }
  if (cp > 0x10FFFF || (cp >= 0xD800 && cp < 0xE000) || (extra == 1 && cp < 0x80) || (extra == 2 && cp < 0x800) || (extra == 3 && cp < 0x10000)) { *used = 1; return 0xFFFD; }
  *used = extra + 1;
  return cp;
}

EXPORT NTSTATUS NTAPI RtlUTF8ToUnicodeN(PWSTR dest, ULONG dest_bytes, PULONG written, const char *src, ULONG src_bytes) {
  ULONG units = 0;      /* what the whole source needs */
  ULONG out = 0;
  for (ULONG i = 0; i < src_bytes;) {
    ULONG used;
    const ULONG cp = utf8_next((const BYTE *)src + i, src_bytes - i, &used);
    i += used;
    const ULONG count = cp >= 0x10000 ? 2 : 1;
    if (dest && (units + count) * 2 <= dest_bytes) {
      if (cp >= 0x10000) { dest[units] = (WCHAR)(0xD800 + ((cp - 0x10000) >> 10)); dest[units + 1] = (WCHAR)(0xDC00 + ((cp - 0x10000) & 0x3FF)); }
      else dest[units] = (WCHAR)cp;
      out = (units + count) * 2;
    }
    units += count;
  }
  if (written) *written = dest ? out : units * 2;
  return dest && units * 2 > dest_bytes ? STATUS_BUFFER_OVERFLOW : STATUS_SUCCESS;
}
EXPORT NTSTATUS NTAPI RtlMultiByteToUnicodeN(PWSTR d, ULONG db, PULONG w, const char *s, ULONG sb) { return RtlUTF8ToUnicodeN(d, db, w, s, sb); }
EXPORT NTSTATUS NTAPI RtlMultiByteToUnicodeSize(PULONG size, const char *s, ULONG sb) { return RtlUTF8ToUnicodeN(NULL, 0, size, s, sb); }

EXPORT NTSTATUS NTAPI RtlUnicodeToUTF8N(char *dest, ULONG dest_bytes, PULONG written, PCWSTR src, ULONG src_bytes) {
  ULONG needed = 0, out = 0;
  const ULONG count = src_bytes / 2;
  for (ULONG i = 0; i < count; ++i) {
    ULONG cp = src[i];
    if (cp >= 0xD800 && cp < 0xDC00 && i + 1 < count && src[i + 1] >= 0xDC00 && src[i + 1] < 0xE000) { cp = 0x10000 + ((cp - 0xD800) << 10) + (src[i + 1] - 0xDC00); ++i; }
    else if (cp >= 0xD800 && cp < 0xE000) cp = 0xFFFD;
    BYTE encoded[4];
    ULONG n;
    if (cp < 0x80) { encoded[0] = (BYTE)cp; n = 1; }
    else if (cp < 0x800) { encoded[0] = (BYTE)(0xC0 | (cp >> 6)); encoded[1] = (BYTE)(0x80 | (cp & 0x3F)); n = 2; }
    else if (cp < 0x10000) { encoded[0] = (BYTE)(0xE0 | (cp >> 12)); encoded[1] = (BYTE)(0x80 | ((cp >> 6) & 0x3F)); encoded[2] = (BYTE)(0x80 | (cp & 0x3F)); n = 3; }
    else { encoded[0] = (BYTE)(0xF0 | (cp >> 18)); encoded[1] = (BYTE)(0x80 | ((cp >> 12) & 0x3F)); encoded[2] = (BYTE)(0x80 | ((cp >> 6) & 0x3F)); encoded[3] = (BYTE)(0x80 | (cp & 0x3F)); n = 4; }
    if (dest && needed + n <= dest_bytes) { for (ULONG k = 0; k < n; ++k) dest[needed + k] = (char)encoded[k]; out = needed + n; }
    needed += n;
  }
  if (written) *written = dest ? out : needed;
  return dest && needed > dest_bytes ? STATUS_BUFFER_OVERFLOW : STATUS_SUCCESS;
}
EXPORT NTSTATUS NTAPI RtlUnicodeToMultiByteN(char *d, ULONG db, PULONG w, PCWSTR s, ULONG sb) { return RtlUnicodeToUTF8N(d, db, w, s, sb); }
EXPORT NTSTATUS NTAPI RtlUnicodeToMultiByteSize(PULONG size, PCWSTR s, ULONG sb) { return RtlUnicodeToUTF8N(NULL, 0, size, s, sb); }

/* ---- the heap ------------------------------------------------------------------------------------------------------------------------------------------ */

/* A chunk is [prev_size (8)][size and flags (8)][payload...]; the user pointer is the payload, 16-byte aligned. prev_size is only meaningful while the chunk before is
 * free (it then lies in that chunk's last 8 bytes). A free chunk keeps two list links at the start of its payload. A segment ends with a zero-size "in use" chunk. */
#define CHUNK_USED 1ull
#define CHUNK_PREV_USED 2ull
#define CHUNK_SIZE(c) ((c)->size_flags & ~15ull)
typedef struct Chunk { ULONGLONG prev_size, size_flags; struct Chunk *next, *prev; } Chunk;
typedef struct Segment { struct Segment *next; PVOID base; SIZE_T size; } Segment;
typedef struct Heap { ULONGLONG magic; ULONG flags; Segment *segments; Chunk free_list; } Heap;
#define HEAP_MAGIC 0x50414548454B4F54ull
#define MIN_CHUNK 32ull
#define SEGMENT_MIN (1ull << 20)

static Chunk *chunk_next(Chunk *c) { return (Chunk *)((BYTE *)c + CHUNK_SIZE(c)); }
static void free_unlink(Chunk *c) { c->prev->next = c->next; c->next->prev = c->prev; }
static void free_link(Heap *h, Chunk *c) { c->next = h->free_list.next; c->prev = &h->free_list; h->free_list.next->prev = c; h->free_list.next = c; }

/* Makes `c` (size bytes, not in any list) a free chunk and puts the bookkeeping of its neighbour right. */
static void make_free(Heap *h, Chunk *c, ULONGLONG size, int prev_used) {
  c->size_flags = size | (prev_used ? CHUNK_PREV_USED : 0);
  Chunk *after = (Chunk *)((BYTE *)c + size);
  after->prev_size = size;
  after->size_flags &= ~CHUNK_PREV_USED;
  free_link(h, c);
}

static int heap_add_segment(Heap *h, SIZE_T minimum) {
  SIZE_T size = minimum + sizeof(Segment) + 64 + 4096;
  if (size < SEGMENT_MIN) size = SEGMENT_MIN;
  PVOID base = NULL;
  if (!NT_SUCCESS(NtAllocateVirtualMemory(NtCurrentProcess(), &base, 0, &size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE))) return 0;
  Segment *segment = (Segment *)base;
  segment->base = base;
  segment->size = size;
  segment->next = h->segments;
  h->segments = segment;
  BYTE *start = (BYTE *)(((ULONG_PTR)base + sizeof(Segment) + 15 + 8) & ~15ull) - 8;      /* so that start + 16 is 16-byte aligned */
  BYTE *end = (BYTE *)(((ULONG_PTR)base + size) & ~15ull) - 16;
  Chunk *sentinel = (Chunk *)end;
  sentinel->size_flags = CHUNK_USED;      /* size 0 */
  make_free(h, (Chunk *)start, (ULONGLONG)(end - start), 1);
  return 1;
}

static Heap *heap_new(ULONG flags, SIZE_T commit) {
  SIZE_T size = commit < SEGMENT_MIN ? SEGMENT_MIN : commit;
  PVOID base = NULL;
  if (!NT_SUCCESS(NtAllocateVirtualMemory(NtCurrentProcess(), &base, 0, &size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE))) return NULL;
  Heap *h = (Heap *)base;
  h->magic = HEAP_MAGIC;
  h->flags = flags;
  h->free_list.next = h->free_list.prev = &h->free_list;
  Segment *segment = (Segment *)((BYTE *)base + ((sizeof(Heap) + 15) & ~15ull));
  segment->base = base;
  segment->size = size;
  segment->next = NULL;
  h->segments = segment;
  BYTE *start = (BYTE *)(((ULONG_PTR)segment + sizeof(Segment) + 15 + 8) & ~15ull) - 8;
  BYTE *end = (BYTE *)(((ULONG_PTR)base + size) & ~15ull) - 16;
  Chunk *sentinel = (Chunk *)end;
  sentinel->size_flags = CHUNK_USED;
  make_free(h, (Chunk *)start, (ULONGLONG)(end - start), 1);
  return h;
}

static int heap_ok(Heap *h) { return h && h->magic == HEAP_MAGIC; }

EXPORT PVOID NTAPI RtlCreateHeap(ULONG flags, PVOID base, SIZE_T reserve, SIZE_T commit, PVOID lock, PVOID parameters) {
  (void)base; (void)reserve; (void)lock; (void)parameters;
  return heap_new(flags, commit);
}

EXPORT PVOID NTAPI RtlAllocateHeap(PVOID heap, ULONG flags, SIZE_T bytes) {
  Heap *h = heap;
  if (!heap_ok(h)) return NULL;
  if (bytes > (1ull << 46)) return NULL;
  ULONGLONG need = (bytes + 8 + 15) & ~15ull;
  if (need < MIN_CHUNK) need = MIN_CHUNK;
  for (int attempt = 0; attempt < 2; ++attempt) {
    for (Chunk *c = h->free_list.next; c != &h->free_list; c = c->next) {
      const ULONGLONG size = CHUNK_SIZE(c);
      if (size < need) continue;
      free_unlink(c);
      if (size - need >= MIN_CHUNK) {      /* split: the rest stays free */
        c->size_flags = need | CHUNK_USED | (c->size_flags & CHUNK_PREV_USED);
        make_free(h, (Chunk *)((BYTE *)c + need), size - need, 1);
      } else {
        c->size_flags |= CHUNK_USED;
        chunk_next(c)->size_flags |= CHUNK_PREV_USED;
      }
      BYTE *user = (BYTE *)c + 16;
      if (flags & HEAP_ZERO_MEMORY) memset(user, 0, (size_t_)(CHUNK_SIZE(c) - 8));
      return user;
    }
    if (attempt == 0 && !heap_add_segment(h, need)) return NULL;
  }
  return NULL;
}

EXPORT BOOLEAN NTAPI RtlFreeHeap(PVOID heap, ULONG flags, PVOID memory) {
  (void)flags;
  Heap *h = heap;
  if (!heap_ok(h)) return FALSE;
  if (!memory) return TRUE;
  Chunk *c = (Chunk *)((BYTE *)memory - 16);
  if (!(c->size_flags & CHUNK_USED)) return FALSE;      /* freed twice */
  ULONGLONG size = CHUNK_SIZE(c);
  int prev_used = (c->size_flags & CHUNK_PREV_USED) != 0;
  Chunk *after = chunk_next(c);
  if (!(after->size_flags & CHUNK_USED)) {      /* merge with the free chunk after */
    size += CHUNK_SIZE(after);
    free_unlink(after);
  }
  if (!prev_used) {      /* merge with the free chunk before */
    Chunk *before = (Chunk *)((BYTE *)c - c->prev_size);
    size += CHUNK_SIZE(before);
    prev_used = (before->size_flags & CHUNK_PREV_USED) != 0;
    free_unlink(before);
    c = before;
  }
  make_free(h, c, size, prev_used);
  return TRUE;
}

EXPORT SIZE_T NTAPI RtlSizeHeap(PVOID heap, ULONG flags, PVOID memory) {
  (void)flags;
  if (!heap_ok(heap) || !memory) return (SIZE_T)-1;
  Chunk *c = (Chunk *)((BYTE *)memory - 16);
  return (c->size_flags & CHUNK_USED) ? (SIZE_T)(CHUNK_SIZE(c) - 8) : (SIZE_T)-1;
}

EXPORT PVOID NTAPI RtlReAllocateHeap(PVOID heap, ULONG flags, PVOID memory, SIZE_T bytes) {
  if (!memory) return RtlAllocateHeap(heap, flags, bytes);
  const SIZE_T have = RtlSizeHeap(heap, flags, memory);
  if (have == (SIZE_T)-1) return NULL;
  if (bytes <= have) return memory;
  PVOID fresh = RtlAllocateHeap(heap, flags & ~HEAP_ZERO_MEMORY, bytes);
  if (!fresh) return NULL;
  memcpy(fresh, memory, have);
  if (flags & HEAP_ZERO_MEMORY) memset((BYTE *)fresh + have, 0, bytes - have);
  RtlFreeHeap(heap, flags, memory);
  return fresh;
}

EXPORT BOOLEAN NTAPI RtlValidateHeap(PVOID heap, ULONG flags, PVOID memory) {
  (void)flags;
  if (!heap_ok(heap)) return FALSE;
  return memory ? RtlSizeHeap(heap, 0, memory) != (SIZE_T)-1 : TRUE;
}

EXPORT PVOID NTAPI RtlDestroyHeap(PVOID heap) {
  Heap *h = heap;
  if (!heap_ok(h) || h == NtCurrentPeb()->ProcessHeap) return heap;
  Segment *segment = h->segments;
  PVOID first = h;
  h->magic = 0;
  while (segment) {
    Segment *next = segment->next;
    PVOID base = segment->base;
    SIZE_T zero = 0;
    if (base != first) NtFreeVirtualMemory(NtCurrentProcess(), &base, &zero, MEM_RELEASE);
    segment = next;
  }
  SIZE_T zero = 0;
  NtFreeVirtualMemory(NtCurrentProcess(), &first, &zero, MEM_RELEASE);
  return NULL;
}

EXPORT BOOLEAN NTAPI RtlLockHeap(PVOID heap) { (void)heap; return TRUE; }
EXPORT BOOLEAN NTAPI RtlUnlockHeap(PVOID heap) { (void)heap; return TRUE; }
EXPORT ULONG NTAPI RtlCompactHeap(PVOID heap, ULONG flags) { (void)heap; (void)flags; return 0; }
EXPORT NTSTATUS NTAPI RtlSetHeapInformation(PVOID heap, ULONG info_class, PVOID info, SIZE_T length) { (void)heap; (void)info_class; (void)info; (void)length; return STATUS_SUCCESS; }
EXPORT NTSTATUS NTAPI RtlQueryHeapInformation(PVOID heap, ULONG info_class, PVOID info, SIZE_T length, PSIZE_T returned) {
  (void)heap; (void)info_class; (void)info; (void)length; (void)returned;
  return STATUS_NOT_IMPLEMENTED;
}
EXPORT ULONG NTAPI RtlGetProcessHeaps(ULONG count, PVOID *heaps) {
  if (count >= 1 && heaps) heaps[0] = NtCurrentPeb()->ProcessHeap;
  return 1;
}

static PVOID process_heap(void) {
  PEB *peb = NtCurrentPeb();
  if (!peb->ProcessHeap) peb->ProcessHeap = heap_new(0, 0);
  return peb->ProcessHeap;
}
EXPORT PVOID NTAPI RtlProcessHeap(void) { return process_heap(); }

/* ---- critical sections (one thread: they only count) ---------------------------------------------------------------------------------------------------- */

EXPORT NTSTATUS NTAPI RtlInitializeCriticalSection(PRTL_CRITICAL_SECTION cs) {
  cs->DebugInfo = NULL;
  cs->LockCount = -1;
  cs->RecursionCount = 0;
  cs->OwningThread = NULL;
  cs->LockSemaphore = NULL;
  cs->SpinCount = 0;
  return STATUS_SUCCESS;
}
EXPORT NTSTATUS NTAPI RtlInitializeCriticalSectionEx(PRTL_CRITICAL_SECTION cs, ULONG spin, ULONG flags) { (void)spin; (void)flags; return RtlInitializeCriticalSection(cs); }
EXPORT NTSTATUS NTAPI RtlInitializeCriticalSectionAndSpinCount(PRTL_CRITICAL_SECTION cs, ULONG spin) { (void)spin; return RtlInitializeCriticalSection(cs); }
EXPORT NTSTATUS NTAPI RtlEnterCriticalSection(PRTL_CRITICAL_SECTION cs) {
  ++cs->LockCount;
  ++cs->RecursionCount;
  cs->OwningThread = (HANDLE)TEB_CLIENT_THREAD(NtCurrentTeb());
  return STATUS_SUCCESS;
}
EXPORT BOOLEAN NTAPI RtlTryEnterCriticalSection(PRTL_CRITICAL_SECTION cs) { RtlEnterCriticalSection(cs); return TRUE; }
EXPORT NTSTATUS NTAPI RtlLeaveCriticalSection(PRTL_CRITICAL_SECTION cs) {
  if (cs->RecursionCount > 0) --cs->RecursionCount;
  --cs->LockCount;
  if (cs->RecursionCount == 0) cs->OwningThread = NULL;
  return STATUS_SUCCESS;
}
EXPORT NTSTATUS NTAPI RtlDeleteCriticalSection(PRTL_CRITICAL_SECTION cs) { cs->LockCount = -1; cs->RecursionCount = 0; cs->OwningThread = NULL; return STATUS_SUCCESS; }

/* ---- exceptions: unwinding, dispatching, __C_specific_handler ----------------------------------------------------------------------------------------------- */

#include "except.inc"
EXPORT ULONG DbgPrint(const char *format, ...) { (void)format; return 0; }
/* SSE and SSE2 are there (PF_XMMI_INSTRUCTIONS_AVAILABLE = 6, PF_XMMI64_INSTRUCTIONS_AVAILABLE = 10); nothing newer is reported. */
EXPORT BOOLEAN NTAPI RtlIsProcessorFeaturePresent(ULONG feature) { return feature == 6 || feature == 10; }

/* ---- process start-up ------------------------------------------------------------------------------------------------------------------------------- */

typedef BOOL (WINAPI *DllEntry)(HANDLE, DWORD, PVOID);
typedef int (WINAPI *ProgramEntry)(PVOID);

/* The thread of a program starts here: the kernel puts the program's entry point in the first argument and the PEB in the second. The libraries' entry points run first
 * (in the order the kernel listed them: a library after the libraries it imports), then the program's. Its return value ends the process. */
EXPORT void NTAPI RtlUserThreadStart(ProgramEntry entry, PVOID parameter) {
  PEB *peb = NtCurrentPeb();
  PEB_LDR_DATA *ldr = peb->Ldr;
  process_heap();
  if (ldr) {
    LIST_ENTRY *head = &ldr->InInitializationOrderModuleList;
    for (LIST_ENTRY *e = head->Flink; e != head; e = e->Flink) {
      LDR_DATA_TABLE_ENTRY *module = (LDR_DATA_TABLE_ENTRY *)((BYTE *)e - __builtin_offsetof(LDR_DATA_TABLE_ENTRY, InInitializationOrderLinks));
      if (module->EntryPoint) ((DllEntry)module->EntryPoint)(module->DllBase, DLL_PROCESS_ATTACH, (PVOID)1);
    }
  }
  RtlExitUserProcess((NTSTATUS)entry(parameter));
}

/* ntdll's own entry point: nothing to set up (the heap is made when first used). */
EXPORT BOOL WINAPI DllMain(HANDLE module, DWORD reason, PVOID reserved) {
  (void)module; (void)reserved;
  if (reason == DLL_PROCESS_ATTACH) process_heap();
  return TRUE;
}
