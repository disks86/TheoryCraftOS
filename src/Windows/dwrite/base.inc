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

/* dwrite.dll: memory, strings, the service call, COM boilerplate. Included by dwrite.c. */

void *memcpy(void *dest, const void *source, size_t n) {
  unsigned char *d = dest;
  const unsigned char *s = source;
  while (n--) *d++ = *s++;
  return dest;
}
void *memmove(void *dest, const void *source, size_t n) {
  unsigned char *d = dest;
  const unsigned char *s = source;
  if (d < s) { while (n--) *d++ = *s++; }
  else { d += n; s += n; while (n--) *--d = *--s; }
  return dest;
}
void *memset(void *dest, int value, size_t n) {
  unsigned char *d = dest;
  while (n--) *d++ = (unsigned char)value;
  return dest;
}
int memcmp(const void *a, const void *b, size_t n) {
  const unsigned char *x = a, *y = b;
  for (; n; --n, ++x, ++y)
    if (*x != *y) return *x < *y ? -1 : 1;
  return 0;
}

static CRITICAL_SECTION g_lock;      /* the caches of faces (face.inc) and the system collection (fonts.inc) */

static void *mem_alloc(SIZE_T n) { return HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, n ? n : 1); }
static void mem_free(void *p) { if (p) HeapFree(GetProcessHeap(), 0, p); }
static void *mem_grow(void *p, SIZE_T n) { return p ? HeapReAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, p, n) : HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, n); }
/* n elements of size `size`, NULL when the product overflows. */
static void *mem_array(SIZE_T n, SIZE_T size) { return size && n > (SIZE_T)0x7FFFFFFF / size ? NULL : mem_alloc(n * size); }

static SIZE_T wlen(const WCHAR *s) { SIZE_T n = 0; while (s[n]) ++n; return n; }
static WCHAR wlower(WCHAR c) { return c >= 'A' && c <= 'Z' ? (WCHAR)(c + 32) : c; }
static int wieq(const WCHAR *a, const WCHAR *b) {      /* ASCII case-insensitive */
  for (; *a && *b; ++a, ++b)
    if (wlower(*a) != wlower(*b)) return 0;
  return *a == *b;
}
static WCHAR *wdup(const WCHAR *s) {
  const SIZE_T n = wlen(s);
  WCHAR *r = mem_alloc((n + 1) * sizeof(WCHAR));
  if (r) memcpy(r, s, (n + 1) * sizeof(WCHAR));
  return r;
}
/* UTF-8 (from the kernel: names) to a new wide string. */
static WCHAR *wdup_utf8(const char *s, SIZE_T n) {
  WCHAR *r = mem_alloc((n + 1) * sizeof(WCHAR));
  SIZE_T o = 0;
  for (SIZE_T i = 0; r && i < n;) {
    unsigned c = (unsigned char)s[i++], extra = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
    unsigned cp = extra == 3 ? c & 7 : extra == 2 ? c & 15 : extra == 1 ? c & 31 : c;
    while (extra-- && i < n) cp = (cp << 6) | ((unsigned char)s[i++] & 63);
    if (cp >= 0x10000) { cp -= 0x10000; r[o++] = (WCHAR)(0xD800 + (cp >> 10)); r[o++] = (WCHAR)(0xDC00 + (cp & 0x3FF)); }
    else r[o++] = (WCHAR)cp;
  }
  return r;
}

static int guid_eq(REFIID a, REFIID b) { return memcmp(a, b, sizeof(GUID)) == 0; }

/* ---- floating point without the C library */
static float f_floor(float v) {
  if (v >= 2147483520.0f || v <= -2147483520.0f) return v;
  const float t = (float)(int)v;
  return t > v ? t - 1.0f : t;
}
static float f_ceil(float v) {
  const float f = f_floor(v);
  return f < v ? f + 1.0f : f;
}
static float f_round(float v) { return f_floor(v + 0.5f); }
static float f_abs(float v) { return v < 0 ? -v : v; }
static float f_max(float a, float b) { return a > b ? a : b; }
static float f_min(float a, float b) { return a < b ? a : b; }
static int f_finite(float v) { return v == v && v < 3.0e38f && v > -3.0e38f; }
#define BIG_WIDTH 3.0e38f

/* ---- NtTcosFont (src/Process/WindowsFonts.hpp) */
__declspec(dllimport) NTSTATUS NTAPI NtTcosFont(void *request);

typedef struct TcosFontRequest {
  ULONG size, op, face, x, y, count;
  ULONGLONG in, out;
  ULONG capacity, r0, r1, r2, r3, reserved;
} TcosFontRequest;
typedef char TcosFontRequestSize[sizeof(TcosFontRequest) == 64 ? 1 : -1];

enum { OP_FACE_COUNT = 1, OP_FACE_INFO, OP_FACE_STRING, OP_MAP_CODEPOINTS, OP_GLYPH_BOXES, OP_KERNING, OP_RASTERIZE, OP_OUTLINE, OP_FONT_DATA, OP_ADD_FONT };
#define STATUS_BUFFER_TOO_SMALL_ ((NTSTATUS)0xC0000023)

typedef struct TcosFaceInfo {
  UINT32 units_per_em;
  INT32 ascent, descent, line_gap, x_height, cap_height;
  UINT32 glyph_count;
  UINT16 weight, stretch;
  UINT32 flags;      /* 1 italic, 2 bold, 4 bundled, 8 monospaced */
  BYTE reserved[24];
} TcosFaceInfo;
typedef struct TcosGlyphBox { INT32 advance, x_min, y_min, x_max, y_max; } TcosGlyphBox;

static NTSTATUS font_call(ULONG op, ULONG face, ULONG x, ULONG y, ULONG count, const void *in, void *out, ULONG capacity, TcosFontRequest *result) {
  TcosFontRequest r;
  memset(&r, 0, sizeof(r));
  r.size = sizeof(r);
  r.op = op;
  r.face = face;
  r.x = x;
  r.y = y;
  r.count = count;
  r.in = (ULONGLONG)(ULONG_PTR)in;
  r.out = (ULONGLONG)(ULONG_PTR)out;
  r.capacity = capacity;
  const NTSTATUS status = NtTcosFont(&r);
  if (result) *result = r;
  return status;
}

/* ---- COM boilerplate. Every object starts with its vtable pointer and a reference count. */
#define OBJECT_NEW(type) ((type *)mem_alloc(sizeof(type)))
#define REF_METHODS(prefix, type, iface, destroy)                                                          \
  static ULONG WINAPI prefix##_AddRef(iface *This) { return (ULONG)InterlockedIncrement(&((type *)This)->ref); } \
  static ULONG WINAPI prefix##_Release(iface *This) {                                                      \
    type *o = (type *)This;                                                                                \
    const LONG n = InterlockedDecrement(&o->ref);                                                          \
    if (n == 0) destroy(o);                                                                                \
    return (ULONG)n;                                                                                       \
  }

/* QueryInterface for an object that answers IUnknown and up to three interfaces. */
static HRESULT query3(void *self, REFIID riid, void **out, REFIID a, REFIID b, REFIID c) {
  if (!out) return E_POINTER;
  *out = NULL;
  if (guid_eq(riid, &IID_IUnknown) || (a && guid_eq(riid, a)) || (b && guid_eq(riid, b)) || (c && guid_eq(riid, c))) {
    *out = self;
    ((IUnknown *)self)->lpVtbl->AddRef((IUnknown *)self);
    return S_OK;
  }
  return E_NOINTERFACE;
}

static void add_ref(void *iface) { if (iface) ((IUnknown *)iface)->lpVtbl->AddRef((IUnknown *)iface); }
static void release(void *iface) { if (iface) ((IUnknown *)iface)->lpVtbl->Release((IUnknown *)iface); }

/* Clamp helpers for text ranges */
static int range_ok(DWRITE_TEXT_RANGE r, UINT32 length) { return r.startPosition <= length && r.length <= length - r.startPosition; }
