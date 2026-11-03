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

/* ntdll.dll: more of the Rtl library: the 64-bit bitmaps and some more bit functions, local time, text conversions, wildcard matching, the frames of the TEB and a number of small
 * queries. Included by ntdll.c after etw.inc. */

NTSTATUS NTAPI NtSetSystemInformation(ULONG information_class, PVOID buffer, ULONG length);
NTSTATUS NTAPI NtGetCurrentProcessorNumberEx(PVOID number);
#define STATUS_PRIVILEGE_NOT_HELD_ ((NTSTATUS)0xC0000061)

/* ---- bitmaps with 64-bit sizes (RTL_BITMAP_EX) and the rest of the 32-bit ones ---- */

typedef struct { ULONGLONG SizeOfBitMap; ULONGLONG *Buffer; } RTL_BITMAP_EX_;
static int bmx_get(const RTL_BITMAP_EX_ *b, ULONGLONG i) { return (int)((b->Buffer[i >> 6] >> (i & 63)) & 1); }
static void bmx_put(RTL_BITMAP_EX_ *b, ULONGLONG i, int v) { if (v) b->Buffer[i >> 6] |= 1ull << (i & 63); else b->Buffer[i >> 6] &= ~(1ull << (i & 63)); }
EXPORT void NTAPI RtlInitializeBitMapEx(RTL_BITMAP_EX_ *b, ULONGLONG *buffer, ULONGLONG size) { b->SizeOfBitMap = size; b->Buffer = buffer; }
EXPORT void NTAPI RtlSetBitEx(RTL_BITMAP_EX_ *b, ULONGLONG i) { if (i < b->SizeOfBitMap) bmx_put(b, i, 1); }
EXPORT void NTAPI RtlClearBitEx(RTL_BITMAP_EX_ *b, ULONGLONG i) { if (i < b->SizeOfBitMap) bmx_put(b, i, 0); }
EXPORT BOOLEAN NTAPI RtlTestBitEx(const RTL_BITMAP_EX_ *b, ULONGLONG i) { return i < b->SizeOfBitMap && bmx_get(b, i); }
EXPORT void NTAPI RtlSetAllBitsEx(RTL_BITMAP_EX_ *b) { memset(b->Buffer, 0xFF, (SIZE_T)((b->SizeOfBitMap + 63) / 64) * 8); }
EXPORT void NTAPI RtlClearAllBitsEx(RTL_BITMAP_EX_ *b) { memset(b->Buffer, 0, (SIZE_T)((b->SizeOfBitMap + 63) / 64) * 8); }
EXPORT void NTAPI RtlSetBitsEx(RTL_BITMAP_EX_ *b, ULONGLONG start, ULONGLONG count) { for (ULONGLONG i = 0; i < count && start + i < b->SizeOfBitMap; ++i) bmx_put(b, start + i, 1); }
EXPORT void NTAPI RtlClearBitsEx(RTL_BITMAP_EX_ *b, ULONGLONG start, ULONGLONG count) { for (ULONGLONG i = 0; i < count && start + i < b->SizeOfBitMap; ++i) bmx_put(b, start + i, 0); }
static BOOLEAN bmx_run(const RTL_BITMAP_EX_ *b, ULONGLONG start, ULONGLONG count, int value) {
  if (start > b->SizeOfBitMap || count > b->SizeOfBitMap - start) return FALSE;
  for (ULONGLONG i = 0; i < count; ++i) if (bmx_get(b, start + i) != value) return FALSE;
  return TRUE;
}
EXPORT BOOLEAN NTAPI RtlAreBitsSetEx(const RTL_BITMAP_EX_ *b, ULONGLONG start, ULONGLONG count) { return bmx_run(b, start, count, 1); }
EXPORT BOOLEAN NTAPI RtlAreBitsClearEx(const RTL_BITMAP_EX_ *b, ULONGLONG start, ULONGLONG count) { return bmx_run(b, start, count, 0); }
EXPORT ULONGLONG NTAPI RtlNumberOfSetBitsEx(const RTL_BITMAP_EX_ *b) { ULONGLONG n = 0; for (ULONGLONG i = 0; i < b->SizeOfBitMap; ++i) n += (ULONGLONG)bmx_get(b, i); return n; }
EXPORT ULONGLONG NTAPI RtlNumberOfClearBitsEx(const RTL_BITMAP_EX_ *b) { return b->SizeOfBitMap - RtlNumberOfSetBitsEx(b); }
/* The first run of `count` bits of the value, looking from `hint` to the end and then from the start; ~0 if there is none. */
static ULONGLONG bmx_find(const RTL_BITMAP_EX_ *b, ULONGLONG count, ULONGLONG hint, int value) {
  if (count == 0 || count > b->SizeOfBitMap) return ~0ull;
  if (hint >= b->SizeOfBitMap) hint = 0;
  for (int pass = 0; pass < 2; ++pass) {
    ULONGLONG from = pass == 0 ? hint : 0, to = pass == 0 ? b->SizeOfBitMap : (hint + count - 1 < b->SizeOfBitMap ? hint + count - 1 : b->SizeOfBitMap);
    ULONGLONG run = 0;
    for (ULONGLONG i = from; i < to; ++i) {
      if (bmx_get(b, i) == value) { if (++run == count) return i + 1 - count; } else run = 0;
    }
  }
  return ~0ull;
}
EXPORT ULONGLONG NTAPI RtlFindSetBitsEx(const RTL_BITMAP_EX_ *b, ULONGLONG count, ULONGLONG hint) { return bmx_find(b, count, hint, 1); }
EXPORT ULONGLONG NTAPI RtlFindClearBitsEx(const RTL_BITMAP_EX_ *b, ULONGLONG count, ULONGLONG hint) { return bmx_find(b, count, hint, 0); }
EXPORT ULONGLONG NTAPI RtlFindSetBitsAndClearEx(RTL_BITMAP_EX_ *b, ULONGLONG count, ULONGLONG hint) { ULONGLONG at = bmx_find(b, count, hint, 1); if (at != ~0ull) RtlClearBitsEx(b, at, count); return at; }
EXPORT ULONGLONG NTAPI RtlFindClearBitsAndSetEx(RTL_BITMAP_EX_ *b, ULONGLONG count, ULONGLONG hint) { ULONGLONG at = bmx_find(b, count, hint, 0); if (at != ~0ull) RtlSetBitsEx(b, at, count); return at; }

EXPORT void NTAPI RtlSetBit(RTL_BITMAP *b, ULONG i) { if (i < b->SizeOfBitMap) bm_put(b, i, 1); }
EXPORT void NTAPI RtlClearBit(RTL_BITMAP *b, ULONG i) { if (i < b->SizeOfBitMap) bm_put(b, i, 0); }
EXPORT BOOLEAN NTAPI RtlTestBit(const RTL_BITMAP *b, ULONG i) { return i < b->SizeOfBitMap && bm_get(b, i); }
EXPORT ULONG NTAPI RtlNumberOfSetBitsUlongPtr(ULONG_PTR v) { ULONG n = 0; for (; v; v &= v - 1) ++n; return n; }
EXPORT ULONG NTAPI RtlNumberOfSetBitsInRange(const RTL_BITMAP *b, ULONG start, ULONG count) { ULONG n = 0; for (ULONG i = 0; i < count && start + i < b->SizeOfBitMap; ++i) n += (ULONG)bm_get(b, start + i); return n; }
EXPORT ULONG NTAPI RtlNumberOfClearBitsInRange(const RTL_BITMAP *b, ULONG start, ULONG count) { ULONG n = 0; for (ULONG i = 0; i < count && start + i < b->SizeOfBitMap; ++i) n += (ULONG)!bm_get(b, start + i); return n; }
EXPORT void NTAPI RtlCopyBitMap(const RTL_BITMAP *source, RTL_BITMAP *destination, ULONG to) {
  for (ULONG i = 0; i < source->SizeOfBitMap && to + i < destination->SizeOfBitMap; ++i) bm_put(destination, to + i, bm_get(source, i));
}
EXPORT void NTAPI RtlExtractBitMap(const RTL_BITMAP *source, RTL_BITMAP *destination, ULONG from, ULONG count) {
  for (ULONG i = 0; i < count && from + i < source->SizeOfBitMap && i < destination->SizeOfBitMap; ++i) bm_put(destination, i, bm_get(source, from + i));
}
/* The run of clear bits that ends at `from` (going backward); its length, and where it starts. */
EXPORT ULONG NTAPI RtlFindLastBackwardRunClear(const RTL_BITMAP *b, ULONG from, PULONG start) {
  if (from >= b->SizeOfBitMap) { *start = from; return 0; }
  LONG i = (LONG)from;
  while (i >= 0 && !bm_get(b, (ULONG)i)) --i;
  *start = (ULONG)(i + 1);
  return from - (ULONG)(i + 1) + 1;
}
EXPORT void NTAPI RtlInterlockedSetBitRun(RTL_BITMAP *b, ULONG start, ULONG count) {
  for (ULONG i = start; i < start + count && i < b->SizeOfBitMap; ++i) __sync_fetch_and_or(&b->Buffer[i >> 5], 1u << (i & 31));
}
EXPORT void NTAPI RtlInterlockedClearBitRun(RTL_BITMAP *b, ULONG start, ULONG count) {
  for (ULONG i = start; i < start + count && i < b->SizeOfBitMap; ++i) __sync_fetch_and_and(&b->Buffer[i >> 5], ~(1u << (i & 31)));
}

/* ---- time ---- */

static LONGLONG time_bias_100ns(void) {      /* (the offset of the local time from UTC in the units of the system time) */
  BYTE zone[432];
  memset(zone, 0, sizeof(zone));
  if (!NT_SUCCESS(RtlQueryTimeZoneInformation(zone))) return 0;
  return (LONGLONG)*(const LONG *)zone * 600000000ll;      /* Bias, in minutes, is UTC minus local */
}
EXPORT NTSTATUS NTAPI RtlSystemTimeToLocalTime(const LARGE_INTEGER *system, PLARGE_INTEGER local) { local->QuadPart = system->QuadPart - time_bias_100ns(); return STATUS_SUCCESS; }
EXPORT NTSTATUS NTAPI RtlLocalTimeToSystemTime(const LARGE_INTEGER *local, PLARGE_INTEGER system) { system->QuadPart = local->QuadPart + time_bias_100ns(); return STATUS_SUCCESS; }
EXPORT LONGLONG NTAPI RtlGetSystemTimePrecise(void) { LARGE_INTEGER t; NtQuerySystemTime(&t); return t.QuadPart; }
EXPORT ULONGLONG NTAPI RtlGetInterruptTimePrecise(PLARGE_INTEGER performance) {
  LARGE_INTEGER counter, frequency;
  NtQueryPerformanceCounter(&counter, &frequency);
  if (performance) *performance = counter;
  ULONGLONG time;
  RtlQueryUnbiasedInterruptTime(&time);
  return time;
}
EXPORT void NTAPI RtlGetSystemTimeAndBias(ULONGLONG *time, ULONGLONG *bias, ULONGLONG *update) {
  LARGE_INTEGER now;
  NtQuerySystemTime(&now);
  if (time) *time = (ULONGLONG)now.QuadPart;
  if (bias) *bias = (ULONGLONG)-time_bias_100ns();
  if (update) *update = (ULONGLONG)now.QuadPart;
}
EXPORT BOOLEAN NTAPI RtlTimeToElapsedTimeFields(const LARGE_INTEGER *time, RTL_TIME_FIELDS *f) {
  ULONGLONG t = (ULONGLONG)time->QuadPart / 10000;
  f->Milliseconds = (SHORT)(t % 1000); t /= 1000;
  f->Second = (SHORT)(t % 60); t /= 60;
  f->Minute = (SHORT)(t % 60); t /= 60;
  f->Hour = (SHORT)(t % 24); t /= 24;
  f->Year = 0; f->Month = 0; f->Weekday = 0;
  f->Day = (SHORT)t;
  return TRUE;
}
/* A time zone's date of change: absolute (a Year) or "the n-th weekday of the month" (Year 0; Day 5 is the last one). `current` or the clock gives the year. */
EXPORT BOOLEAN NTAPI RtlCutoverTimeToSystemTime(const RTL_TIME_FIELDS *cutover, PLARGE_INTEGER out, const LARGE_INTEGER *current, BOOLEAN use_current_year) {
  RTL_TIME_FIELDS fields = *cutover, now_fields;
  LARGE_INTEGER now;
  if (current) now = *current; else NtQuerySystemTime(&now);
  RtlTimeToTimeFields(&now.QuadPart, &now_fields);
  if (fields.Year == 0) {
    fields.Year = now_fields.Year;
    const SHORT occurrence = fields.Day, weekday = fields.Weekday;
    fields.Day = 1;
    LONGLONG first;
    if (!RtlTimeFieldsToTime(&fields, &first)) return FALSE;
    RTL_TIME_FIELDS probe;
    RtlTimeToTimeFields(&first, &probe);
    SHORT day = (SHORT)(1 + ((weekday - probe.Weekday + 7) % 7) + (occurrence - 1) * 7);
    for (;;) {
      fields.Day = day;
      if (RtlTimeFieldsToTime(&fields, &first)) { RTL_TIME_FIELDS check; RtlTimeToTimeFields(&first, &check); if (check.Month == fields.Month) break; }
      if (day <= 7) return FALSE;
      day = (SHORT)(day - 7);
    }
    out->QuadPart = first;
    return TRUE;
  }
  if (use_current_year) fields.Year = now_fields.Year;
  LONGLONG t;
  if (!RtlTimeFieldsToTime(&fields, &t)) return FALSE;
  out->QuadPart = t;
  return TRUE;
}
EXPORT NTSTATUS NTAPI RtlSetTimeZoneInformation(PVOID info) { return NtSetSystemInformation(44, info, 172); }
EXPORT NTSTATUS NTAPI RtlSetDynamicTimeZoneInformation(PVOID info) { return NtSetSystemInformation(102, info, 432); }

/* ---- text ---- */

EXPORT BOOLEAN NTAPI RtlCreateUnicodeString(PUNICODE_STRING s, PCWSTR text) {
  SIZE_T n = 0;
  while (text[n]) ++n;
  if (n * 2 + 2 > 0xFFFF) return FALSE;
  WCHAR *buffer = RtlAllocateHeap(process_heap(), 0, n * 2 + 2);
  if (!buffer) return FALSE;
  memcpy(buffer, text, n * 2 + 2);
  s->Buffer = buffer;
  s->Length = (USHORT)(n * 2);
  s->MaximumLength = (USHORT)(n * 2 + 2);
  return TRUE;
}
EXPORT void NTAPI RtlEraseUnicodeString(PUNICODE_STRING s) { if (s->Buffer && s->MaximumLength) memset(s->Buffer, 0, s->MaximumLength); s->Length = 0; }
EXPORT NTSTATUS NTAPI RtlInitStringEx(PANSI_STRING s, PCSTR text) {
  if (text) {
    SIZE_T n = 0;
    while (text[n]) ++n;
    if (n > 0xFFFE) { s->Length = s->MaximumLength = 0; s->Buffer = NULL; return (NTSTATUS)0xC0000106; }      /* STATUS_NAME_TOO_LONG */
    s->Length = (USHORT)n; s->MaximumLength = (USHORT)(n + 1);
  } else s->Length = s->MaximumLength = 0;
  s->Buffer = (char *)text;
  return STATUS_SUCCESS;
}
EXPORT void NTAPI RtlInitUTF8String(PANSI_STRING s, PCSTR text) { RtlInitStringEx(s, text); }
EXPORT NTSTATUS NTAPI RtlInitUTF8StringEx(PANSI_STRING s, PCSTR text) { return RtlInitStringEx(s, text); }
EXPORT void NTAPI RtlFreeUTF8String(PANSI_STRING s) { if (s->Buffer) { RtlFreeHeap(process_heap(), 0, s->Buffer); s->Buffer = NULL; s->Length = s->MaximumLength = 0; } }
EXPORT NTSTATUS NTAPI RtlUTF8StringToUnicodeString(PUNICODE_STRING dst, const ANSI_STRING *src, BOOLEAN alloc) {
  ULONG needed = 0;
  NTSTATUS status = RtlUTF8ToUnicodeN(NULL, 0, &needed, src->Buffer, src->Length);
  if (!NT_SUCCESS(status)) return status;
  if (needed > 0xFFFE) return (NTSTATUS)0xC0000106;
  if (alloc) {
    dst->Buffer = RtlAllocateHeap(process_heap(), 0, needed + 2);
    if (!dst->Buffer) return STATUS_NO_MEMORY;
    dst->MaximumLength = (USHORT)(needed + 2);
  } else if (dst->MaximumLength < needed) return (NTSTATUS)0x80000005;
  status = RtlUTF8ToUnicodeN(dst->Buffer, needed, &needed, src->Buffer, src->Length);
  dst->Length = (USHORT)needed;
  if (dst->MaximumLength >= needed + 2) dst->Buffer[needed / 2] = 0;
  return status;
}
EXPORT NTSTATUS NTAPI RtlUnicodeStringToUTF8String(PANSI_STRING dst, const UNICODE_STRING *src, BOOLEAN alloc) {
  ULONG needed = 0;
  NTSTATUS status = RtlUnicodeToUTF8N(NULL, 0, &needed, src->Buffer, src->Length);
  if (!NT_SUCCESS(status)) return status;
  if (needed > 0xFFFE) return (NTSTATUS)0xC0000106;
  if (alloc) {
    dst->Buffer = RtlAllocateHeap(process_heap(), 0, needed + 1);
    if (!dst->Buffer) return STATUS_NO_MEMORY;
    dst->MaximumLength = (USHORT)(needed + 1);
  } else if (dst->MaximumLength < needed) return (NTSTATUS)0x80000005;
  status = RtlUnicodeToUTF8N(dst->Buffer, needed, &needed, src->Buffer, src->Length);
  dst->Length = (USHORT)needed;
  if (dst->MaximumLength > needed) dst->Buffer[needed] = 0;
  return status;
}
/* (single-byte code pages: a character is a byte) */
EXPORT ULONG NTAPI RtlxAnsiStringToUnicodeSize(const ANSI_STRING *s) { return ((ULONG)s->Length + 1) * 2; }
EXPORT ULONG NTAPI RtlxOemStringToUnicodeSize(const ANSI_STRING *s) { return ((ULONG)s->Length + 1) * 2; }
EXPORT ULONG NTAPI RtlxUnicodeStringToAnsiSize(const UNICODE_STRING *s) { return (ULONG)s->Length / 2 + 1; }
EXPORT ULONG NTAPI RtlxUnicodeStringToOemSize(const UNICODE_STRING *s) { return (ULONG)s->Length / 2 + 1; }
EXPORT ULONG NTAPI RtlAnsiStringToUnicodeSize(const ANSI_STRING *s) { return RtlxAnsiStringToUnicodeSize(s); }
EXPORT ULONG NTAPI RtlOemStringToUnicodeSize(const ANSI_STRING *s) { return RtlxOemStringToUnicodeSize(s); }
EXPORT ULONG NTAPI RtlUnicodeStringToAnsiSize(const UNICODE_STRING *s) { return RtlxUnicodeStringToAnsiSize(s); }
EXPORT ULONG NTAPI RtlUnicodeStringToOemSize(const UNICODE_STRING *s) { return RtlxUnicodeStringToOemSize(s); }
EXPORT NTSTATUS NTAPI RtlOemToUnicodeN(PWSTR dst, ULONG max, PULONG written, PCSTR src, ULONG length) { return RtlMultiByteToUnicodeN(dst, max, written, src, length); }
EXPORT NTSTATUS NTAPI RtlUnicodeToOemN(PSTR dst, ULONG max, PULONG written, PCWSTR src, ULONG length) { return RtlUnicodeToMultiByteN(dst, max, written, src, length); }
EXPORT NTSTATUS NTAPI RtlConsoleMultiByteToUnicodeN(PWSTR dst, ULONG max, PULONG written, PCSTR src, ULONG length, PULONG pdelta) {
  if (pdelta) *pdelta = 0;
  return RtlMultiByteToUnicodeN(dst, max, written, src, length);
}
EXPORT NTSTATUS NTAPI RtlUpcaseUnicodeToMultiByteN(PSTR dst, ULONG max, PULONG written, PCWSTR src, ULONG length) {
  WCHAR chunk[128];
  ULONG total = 0;
  NTSTATUS result = STATUS_SUCCESS;
  for (ULONG done = 0; done < length;) {
    ULONG n = (length - done) / 2;
    if (n > 128) n = 128;
    if (n == 0) break;
    for (ULONG i = 0; i < n; ++i) chunk[i] = RtlUpcaseUnicodeChar(src[done / 2 + i]);
    ULONG got = 0;
    const NTSTATUS status = RtlUnicodeToMultiByteN(dst ? dst + total : NULL, max > total ? max - total : 0, &got, chunk, n * 2);
    total += got;
    if (!NT_SUCCESS(status)) { result = status; break; }
    done += n * 2;
  }
  if (written) *written = total;
  return result;
}
EXPORT NTSTATUS NTAPI RtlUpcaseUnicodeToOemN(PSTR dst, ULONG max, PULONG written, PCWSTR src, ULONG length) { return RtlUpcaseUnicodeToMultiByteN(dst, max, written, src, length); }
static NTSTATUS upcase_to_narrow(PANSI_STRING dst, const UNICODE_STRING *src, BOOLEAN alloc, BOOLEAN counted) {
  const ULONG needed = src->Length / 2;
  if (needed > 0xFFFE) return (NTSTATUS)0xC0000106;
  if (alloc) {
    dst->Buffer = RtlAllocateHeap(process_heap(), 0, needed + 1);
    if (!dst->Buffer) return STATUS_NO_MEMORY;
    dst->MaximumLength = (USHORT)(needed + 1);
  } else if (dst->MaximumLength < needed) return (NTSTATUS)0x80000005;
  ULONG written = 0;
  const NTSTATUS status = RtlUpcaseUnicodeToMultiByteN(dst->Buffer, dst->MaximumLength, &written, src->Buffer, src->Length);
  dst->Length = (USHORT)written;
  if (!counted && dst->MaximumLength > written) dst->Buffer[written] = 0;
  return status;
}
EXPORT NTSTATUS NTAPI RtlUpcaseUnicodeStringToAnsiString(PANSI_STRING dst, const UNICODE_STRING *src, BOOLEAN alloc) { return upcase_to_narrow(dst, src, alloc, FALSE); }
EXPORT NTSTATUS NTAPI RtlUpcaseUnicodeStringToOemString(PANSI_STRING dst, const UNICODE_STRING *src, BOOLEAN alloc) { return upcase_to_narrow(dst, src, alloc, FALSE); }
EXPORT NTSTATUS NTAPI RtlUpcaseUnicodeStringToCountedOemString(PANSI_STRING dst, const UNICODE_STRING *src, BOOLEAN alloc) { return upcase_to_narrow(dst, src, alloc, TRUE); }
EXPORT NTSTATUS NTAPI RtlUnicodeStringToCountedOemString(PANSI_STRING dst, const UNICODE_STRING *src, BOOLEAN alloc) {
  const ULONG needed = src->Length / 2;
  if (alloc) {
    dst->Buffer = RtlAllocateHeap(process_heap(), 0, needed + 1);
    if (!dst->Buffer) return STATUS_NO_MEMORY;
    dst->MaximumLength = (USHORT)(needed + 1);
  } else if (dst->MaximumLength < needed) return (NTSTATUS)0x80000005;
  ULONG written = 0;
  const NTSTATUS status = RtlUnicodeToMultiByteN(dst->Buffer, dst->MaximumLength, &written, src->Buffer, src->Length);
  dst->Length = (USHORT)written;
  return status;
}
EXPORT PWSTR NTAPI RtlFindUnicodeSubstring(const UNICODE_STRING *needle, const UNICODE_STRING *haystack, BOOLEAN case_insensitive) {
  const USHORT n = needle->Length / 2, h = haystack->Length / 2;
  if (n == 0 || n > h) return NULL;
  for (USHORT i = 0; i + n <= h; ++i) {
    USHORT k = 0;
    for (; k < n; ++k) {
      WCHAR a = haystack->Buffer[i + k], b = needle->Buffer[k];
      if (case_insensitive) { a = RtlUpcaseUnicodeChar(a); b = RtlUpcaseUnicodeChar(b); }
      if (a != b) break;
    }
    if (k == n) return haystack->Buffer + i;
  }
  return NULL;
}
EXPORT NTSTATUS NTAPI RtlRunEncodeUnicodeString(PUCHAR seed, PUNICODE_STRING s) {
  LARGE_INTEGER time;
  if (*seed == 0) { NtQuerySystemTime(&time); ULONG r = (ULONG)time.QuadPart; *seed = (UCHAR)(r | 1); }
  BYTE *p = (BYTE *)s->Buffer;
  BYTE key = *seed;
  for (USHORT i = 0; i < s->Length; ++i) { p[i] ^= key; key = (BYTE)(key + p[i]); }
  return STATUS_SUCCESS;
}
EXPORT NTSTATUS NTAPI RtlRunDecodeUnicodeString(UCHAR seed, PUNICODE_STRING s) {
  BYTE *p = (BYTE *)s->Buffer;
  BYTE key = seed;
  for (USHORT i = 0; i < s->Length; ++i) { const BYTE cipher = p[i]; p[i] = (BYTE)(cipher ^ key); key = (BYTE)(key + cipher); }
  return STATUS_SUCCESS;
}

/* Wildcards in names: * any run, ? one character, < zero or more characters up to the last dot, > one character that is not a dot (or nothing at a dot or the end), " a dot or nothing. */
EXPORT BOOLEAN NTAPI RtlDoesNameContainWildCards(const UNICODE_STRING *expression) {
  for (USHORT i = 0; i < expression->Length / 2; ++i) { const WCHAR c = expression->Buffer[i]; if (c == '*' || c == '?' || c == '<' || c == '>' || c == '"') return TRUE; }
  return FALSE;
}
static BOOLEAN wildcard_match(const WCHAR *e, SIZE_T en, const WCHAR *n, SIZE_T nn, BOOLEAN ci) {
  SIZE_T ei = 0, ni = 0, star_e = (SIZE_T)-1, star_n = 0;
  for (;;) {
    if (ei < en) {
      const WCHAR c = e[ei];
      if (c == '*' || c == '<') {
        if (c == '<') {      /* up to the last dot only: a star that cannot cross a dot that has another dot after it */
          SIZE_T dot = (SIZE_T)-1;
          for (SIZE_T k = ni; k < nn; ++k) if (n[k] == '.') dot = k;
          if (dot == (SIZE_T)-1 || dot < ni) { star_e = ei; star_n = ni; ++ei; continue; }
          /* match up to the last dot greedily */
          if (wildcard_match(e + ei + 1, en - ei - 1, n + dot, nn - dot, ci)) return TRUE;
          if (wildcard_match(e + ei + 1, en - ei - 1, n + ni, nn - ni, ci)) return TRUE;
          star_e = ei; star_n = ni; ++ei; continue;
        }
        star_e = ei; star_n = ni; ++ei; continue;
      }
      if (ni < nn) {
        WCHAR a = n[ni], b = c;
        if (ci) { a = RtlUpcaseUnicodeChar(a); b = RtlUpcaseUnicodeChar(b); }
        if (c == '?' || a == b) { ++ei; ++ni; continue; }
        if (c == '>' && n[ni] != '.') { ++ei; ++ni; continue; }
      }
      if (c == '>' && (ni >= nn || n[ni] == '.')) { ++ei; continue; }
      if (c == '"' && ni < nn && n[ni] == '.') { ++ei; ++ni; continue; }
      if (c == '"' && ni >= nn) { ++ei; continue; }
    } else if (ni >= nn) return TRUE;
    if (star_e != (SIZE_T)-1 && star_n < nn) { ++star_n; ni = star_n; ei = star_e + 1; continue; }
    return FALSE;
  }
}
EXPORT BOOLEAN NTAPI RtlIsNameInExpression(const UNICODE_STRING *expression, const UNICODE_STRING *name, BOOLEAN ignore_case, WCHAR *upcase_table) {
  (void)upcase_table;
  return wildcard_match(expression->Buffer, expression->Length / 2, name->Buffer, name->Length / 2, ignore_case);
}
EXPORT BOOLEAN NTAPI RtlIsNameInUnUpcasedExpression(const UNICODE_STRING *expression, const UNICODE_STRING *name, BOOLEAN ignore_case, WCHAR *upcase_table) {
  return RtlIsNameInExpression(expression, name, ignore_case, upcase_table);
}
/* A name that fits the 8.3 scheme: up to 8 characters, a dot, up to 3, nothing that is not allowed in a short name; `spaces` says whether it has any. */
EXPORT BOOLEAN NTAPI RtlIsNameLegalDOS8Dot3(const UNICODE_STRING *name, PANSI_STRING oem, PBOOLEAN spaces) {
  BOOLEAN has_spaces = FALSE;
  USHORT dot = 0xFFFF;
  const USHORT n = name->Length / 2;
  if (n == 0 || n > 12) return FALSE;
  for (USHORT i = 0; i < n; ++i) {
    const WCHAR c = name->Buffer[i];
    if (c >= 0x80 || c < 0x20 || c == '"' || c == '*' || c == '+' || c == ',' || c == '/' || c == ':' || c == ';' || c == '<' || c == '=' || c == '>' || c == '?' || c == '[' || c == '\\' || c == ']' || c == '|') return FALSE;
    if (c == ' ') has_spaces = TRUE;
    if (c == '.') { if (dot != 0xFFFF) return FALSE; dot = i; }
  }
  if (name->Buffer[0] == '.' && !(n == 1 || (n == 2 && name->Buffer[1] == '.'))) return FALSE;
  if (dot == 0xFFFF) { if (n > 8) return FALSE; }
  else if (dot > 8 || n - dot - 1 > 3 || (dot == n - 1 && n > 2)) return FALSE;
  if (spaces) *spaces = has_spaces;
  if (oem && oem->Buffer && oem->MaximumLength >= n) { for (USHORT i = 0; i < n; ++i) oem->Buffer[i] = (char)RtlUpcaseUnicodeChar(name->Buffer[i]); oem->Length = n; }
  return !has_spaces || TRUE;
}

/* ---- the frames of the TEB, the last status, the processor ---- */

typedef struct TEB_FRAME_ { ULONG flags; struct TEB_FRAME_ *previous; const void *context; } TEB_FRAME_;
#define TEB_ACTIVE_FRAME(teb) (*(TEB_FRAME_ **)((BYTE *)(teb) + 0x17C0))
#define TEB_LAST_STATUS(teb) (*(NTSTATUS *)((BYTE *)(teb) + 0x1250))
EXPORT void NTAPI RtlPushFrame(TEB_FRAME_ *frame) { TEB *teb = NtCurrentTeb(); frame->previous = TEB_ACTIVE_FRAME(teb); TEB_ACTIVE_FRAME(teb) = frame; }
EXPORT void NTAPI RtlPopFrame(TEB_FRAME_ *frame) { TEB_ACTIVE_FRAME(NtCurrentTeb()) = frame->previous; }
EXPORT TEB_FRAME_ *NTAPI RtlGetFrame(void) { return TEB_ACTIVE_FRAME(NtCurrentTeb()); }
EXPORT NTSTATUS NTAPI RtlGetLastNtStatus(void) { return TEB_LAST_STATUS(NtCurrentTeb()); }
EXPORT void NTAPI RtlRestoreLastWin32Error(ULONG error) { TEB_LAST_ERROR(NtCurrentTeb()) = error; }
EXPORT void NTAPI RtlSetLastWin32ErrorAndNtStatusFromNtStatus(NTSTATUS status) { TEB_LAST_STATUS(NtCurrentTeb()) = status; TEB_LAST_ERROR(NtCurrentTeb()) = RtlNtStatusToDosError(status); }
EXPORT ULONG NTAPI RtlGetCurrentProcessorNumber(void) { return NtGetCurrentProcessorNumber(); }
EXPORT void NTAPI RtlGetCurrentProcessorNumberEx(PVOID number) { NtGetCurrentProcessorNumberEx(number); }
EXPORT BOOLEAN NTAPI RtlIsCurrentProcess(HANDLE process) {
  if (process == (HANDLE)(LONG_PTR)-1) return TRUE;
  ULONG_PTR info[6];
  if (!NT_SUCCESS(NtQueryInformationProcess(process, 0, info, sizeof(info), NULL))) return FALSE;
  return info[4] == TEB_CLIENT_PROCESS(NtCurrentTeb());
}
EXPORT BOOLEAN NTAPI RtlIsCurrentThread(HANDLE thread) {
  if (thread == (HANDLE)(LONG_PTR)-2) return TRUE;
  ULONG64 info[6];
  if (!NT_SUCCESS(NtQueryInformationThread(thread, 0, info, sizeof(info), NULL))) return FALSE;
  return info[3] == TEB_CLIENT_THREAD(NtCurrentTeb());
}
EXPORT BOOLEAN NTAPI RtlIsCurrentThreadAttachExempt(void) { return FALSE; }
EXPORT BOOLEAN NTAPI RtlIsThreadWithinLoaderCallout(void) { return FALSE; }
EXPORT ULONG NTAPI RtlGetCriticalSectionRecursionCount(PRTL_CRITICAL_SECTION cs) { return cs->OwningThread == (HANDLE)TEB_CLIENT_THREAD(NtCurrentTeb()) ? (ULONG)cs->RecursionCount : 0; }
EXPORT ULONG NTAPI RtlSetCriticalSectionSpinCount(PRTL_CRITICAL_SECTION cs, ULONG spin) { const ULONG old = (ULONG)cs->SpinCount; cs->SpinCount = spin & 0x00FFFFFFu; return old; }
EXPORT void NTAPI RtlCheckForOrphanedCriticalSections(HANDLE thread) { (void)thread; }
EXPORT void NTAPI RtlEnableEarlyCriticalSectionEventCreation(void) {}
EXPORT BOOLEAN NTAPI RtlIsZeroMemory(const void *p, SIZE_T size) { const BYTE *b = p; for (SIZE_T i = 0; i < size; ++i) if (b[i]) return FALSE; return TRUE; }
EXPORT PVOID NTAPI RtlEncodeSystemPointer(PVOID p) { return (PVOID)((ULONG_PTR)p ^ *(volatile ULONG *)(0x7ffe0000ull + 0x330)); }
EXPORT PVOID NTAPI RtlDecodeSystemPointer(PVOID p) { return RtlEncodeSystemPointer(p); }
EXPORT NTSTATUS NTAPI RtlEncodeRemotePointer(HANDLE process, PVOID p, PVOID *out) { (void)process; *out = RtlEncodeSystemPointer(p); return STATUS_SUCCESS; }
EXPORT NTSTATUS NTAPI RtlDecodeRemotePointer(HANDLE process, PVOID p, PVOID *out) { (void)process; *out = RtlEncodeSystemPointer(p); return STATUS_SUCCESS; }
EXPORT BOOLEAN NTAPI RtlGetNtProductType(PULONG type) { *type = *(volatile ULONG *)(0x7ffe0000ull + 0x264); return TRUE; }
EXPORT const WCHAR *NTAPI RtlGetNtSystemRoot(void) { return (const WCHAR *)(0x7ffe0000ull + 0x30); }
EXPORT ULONG NTAPI RtlGetSuiteMask(void) { return *(volatile ULONG *)(0x7ffe0000ull + 0x2d0); }
EXPORT BOOLEAN NTAPI RtlGetProductInfo(ULONG major, ULONG minor, ULONG sp_major, ULONG sp_minor, PULONG type) { (void)major; (void)minor; (void)sp_major; (void)sp_minor; *type = 0x30; return TRUE; }
EXPORT NTSTATUS NTAPI RtlGetNativeSystemInformation(ULONG information_class, PVOID buffer, ULONG length, PULONG returned) { return NtQuerySystemInformation(information_class, buffer, length, returned); }
EXPORT BOOLEAN NTAPI RtlAreLongPathsEnabled(void) { return TRUE; }
EXPORT ULONG NTAPI RtlGetLongestNtPathLength(void) { return 32767; }
EXPORT BOOLEAN NTAPI RtlIsApiSetImplemented(PCSTR name) { (void)name; return FALSE; }
EXPORT BOOLEAN NTAPI RtlIsMultiSessionSku(void) { return FALSE; }
EXPORT BOOLEAN NTAPI RtlIsMultiUsersInSessionSku(void) { return FALSE; }
EXPORT ULONG NTAPI RtlGetActiveConsoleId(void) { return 1; }
EXPORT ULONG NTAPI RtlGetCurrentServiceSessionId(void) { return 0; }
EXPORT ULONG NTAPI RtlGetConsoleSessionForegroundProcessId(void) { return 0; }
EXPORT PVOID NTAPI RtlGetCurrentTransaction(void) { return NULL; }
EXPORT ULONG NTAPI RtlSetCurrentTransaction(PVOID transaction) { return transaction == NULL; }
EXPORT BOOLEAN NTAPI RtlIsStateSeparationEnabled(void) { return FALSE; }
EXPORT void NTAPI RtlNotifyFeatureUsage(void) {}
EXPORT NTSTATUS NTAPI RtlSetProcessIsCritical(BOOLEAN new_value, PBOOLEAN old_value, BOOLEAN need_break) { (void)need_break; if (old_value) *old_value = FALSE; return new_value ? STATUS_PRIVILEGE_NOT_HELD_ : STATUS_SUCCESS; }
EXPORT NTSTATUS NTAPI RtlSetThreadIsCritical(BOOLEAN new_value, PBOOLEAN old_value, BOOLEAN need_break) { return RtlSetProcessIsCritical(new_value, old_value, need_break); }
EXPORT PVOID NTAPI RtlGetCallersAddress(PVOID *caller, PVOID *caller_of_caller) { if (caller) *caller = NULL; if (caller_of_caller) *caller_of_caller = NULL; return NULL; }
EXPORT void NTAPI RtlAssert(PVOID assertion, PVOID file, ULONG line, PCSTR message) { (void)assertion; (void)file; (void)line; (void)message; }
EXPORT void NTAPI RtlApplicationVerifierStop(ULONG code, PCSTR message, ULONG_PTR a, PCSTR am, ULONG_PTR b, PCSTR bm, ULONG_PTR c, PCSTR cm, ULONG_PTR d, PCSTR dm) {
  (void)code; (void)message; (void)a; (void)am; (void)b; (void)bm; (void)c; (void)cm; (void)d; (void)dm;
}
EXPORT BOOLEAN NTAPI RtlIsElevatedRid(PVOID sid_and_attributes) { (void)sid_and_attributes; return FALSE; }
EXPORT BOOLEAN NTAPI RtlIsEcCode(PVOID address) { (void)address; return FALSE; }
EXPORT BOOLEAN NTAPI RtlIsEnclaveFeaturePresent(ULONG feature) { (void)feature; return FALSE; }
EXPORT BOOLEAN NTAPI RtlIsNonEmptyDirectoryReparsePointAllowed(ULONG tag) { (void)tag; return FALSE; }
EXPORT BOOLEAN NTAPI RtlIsCloudFilesPlaceholder(ULONG attributes, ULONG tag) { (void)attributes; (void)tag; return FALSE; }
EXPORT BOOLEAN NTAPI RtlIsPartialPlaceholder(ULONG attributes, ULONG tag) { (void)attributes; (void)tag; return FALSE; }
EXPORT NTSTATUS NTAPI RtlIsPartialPlaceholderFileHandle(HANDLE file, PBOOLEAN partial) { (void)file; *partial = FALSE; return STATUS_SUCCESS; }
EXPORT NTSTATUS NTAPI RtlIsPartialPlaceholderFileInfo(const void *info, ULONG info_class, PBOOLEAN partial) { (void)info; (void)info_class; *partial = FALSE; return STATUS_SUCCESS; }
EXPORT BOOLEAN NTAPI RtlIsUntrustedObject(HANDLE file, PVOID address, PBOOLEAN untrusted) { (void)file; (void)address; if (untrusted) *untrusted = FALSE; return TRUE; }
EXPORT NTSTATUS NTAPI RtlQueryProcessPlaceholderCompatibilityMode(void) { return 0; }
EXPORT NTSTATUS NTAPI RtlQueryThreadPlaceholderCompatibilityMode(void) { return 0; }
EXPORT NTSTATUS NTAPI RtlSetProcessPlaceholderCompatibilityMode(CHAR mode) { return mode; }
EXPORT NTSTATUS NTAPI RtlSetThreadPlaceholderCompatibilityMode(CHAR mode) { return mode; }
