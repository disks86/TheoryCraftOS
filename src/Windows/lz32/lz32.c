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

/* lz32.dll for TheoryCraftOS: reading files compressed with the LZ (SZDD) format of Microsoft's COMPRESS.EXE, the way setup programs expand "FOO.EX_" to "FOO.EXE".
 * An LZ handle is a number from 0x400 up; a handle of a file that is not compressed is the file's own handle, and the functions then read it as it is. Checked with
 * Wine's tests (tests/windows/wine/lz32). */
#include "../tcos_win.h"

__asm__(".text\n.globl ___chkstk_ms\n___chkstk_ms:\n ret\n");

typedef int HFILE;
typedef struct _OFSTRUCT { BYTE cBytes, fFixedDisk; WORD nErrCode, Reserved1, Reserved2; CHAR szPathName[128]; } OFSTRUCT, *LPOFSTRUCT;
typedef short INT_, *LPINT_;

IMPORT int WINAPI _lopen(LPCSTR path, int mode);
IMPORT UINT WINAPI _lread(int file, LPVOID buffer, UINT bytes);
IMPORT UINT WINAPI _lwrite(int file, LPCSTR buffer, UINT bytes);
IMPORT LONG WINAPI _llseek(int file, LONG offset, int origin);
IMPORT int WINAPI _lclose(int file);
IMPORT HFILE WINAPI OpenFile(LPCSTR name, LPOFSTRUCT of, UINT style);
IMPORT int WINAPI MultiByteToWideChar(UINT code_page, DWORD flags, LPCSTR source, int length, LPWSTR dest, int capacity);
IMPORT int WINAPI WideCharToMultiByte(UINT code_page, DWORD flags, LPCWSTR source, int length, LPSTR dest, int capacity, LPCSTR default_char, LPBOOL used_default);
IMPORT void *memcpy(void *, const void *, size_t_);
IMPORT void *memset(void *, int, size_t_);
IMPORT size_t_ strlen(const char *);

#define LZERROR_BADINHANDLE (-1)
#define LZERROR_BADOUTHANDLE (-2)
#define LZERROR_READ (-3)
#define LZERROR_WRITE (-4)
#define LZERROR_GLOBALLOC (-5)
#define LZERROR_BADVALUE (-7)
#define LZ_BASE 0x400
#define LZ_MAX 16
#define LZ_HEADER 14

typedef struct {
  int in_use, file;
  BYTE missing;
  DWORD size;      /* the uncompressed size */
  DWORD position;      /* how much has been read out */
  BYTE window[4096];
  unsigned window_at;
  BYTE buffer[512];
  unsigned buffer_length, buffer_at;
  unsigned control, bits;
  unsigned match_at, match_length;
} LZ;
static LZ g_lz[LZ_MAX];

static LZ *lz_of(int handle) { return handle >= LZ_BASE && handle < LZ_BASE + LZ_MAX && g_lz[handle - LZ_BASE].in_use ? &g_lz[handle - LZ_BASE] : NULL; }
static int in_byte(LZ *z) {
  if (z->buffer_at == z->buffer_length) {
    const UINT n = _lread(z->file, z->buffer, sizeof(z->buffer));
    if (n == 0 || n == (UINT)-1) return -1;
    z->buffer_length = n;
    z->buffer_at = 0;
  }
  return z->buffer[z->buffer_at++];
}
static void lz_restart(LZ *z) {
  memset(z->window, ' ', sizeof(z->window));
  z->window_at = 4096 - 16;
  z->bits = z->control = z->match_length = 0;
  z->buffer_at = z->buffer_length = 0;
  z->position = 0;
}
/* The next byte of the uncompressed data, or -1 at the end. */
static int out_byte(LZ *z) {
  if (z->position >= z->size) return -1;
  int b;
  if (z->match_length) {
    b = z->window[z->match_at++ & 4095];
    --z->match_length;
  } else {
    if (!z->bits) {
      const int c = in_byte(z);
      if (c < 0) return -1;
      z->control = (unsigned)c;
      z->bits = 8;
    }
    const unsigned bit = z->control & 1;
    z->control >>= 1;
    --z->bits;
    if (bit) {
      b = in_byte(z);
      if (b < 0) return -1;
    } else {
      const int b1 = in_byte(z), b2 = in_byte(z);
      if (b1 < 0 || b2 < 0) return -1;
      z->match_at = (unsigned)b1 | ((unsigned)(b2 & 0xF0) << 4);
      z->match_length = (unsigned)(b2 & 0x0F) + 3;
      b = z->window[z->match_at++ & 4095];
      --z->match_length;
    }
  }
  z->window[z->window_at++ & 4095] = (BYTE)b;
  ++z->position;
  return b;
}

/* Reads the header of a file; TRUE if it is a compressed file (and the size and missing letter). The file position is left after the header, or at the start. */
static int read_header(int file, DWORD *size, BYTE *missing) {
  BYTE h[LZ_HEADER];
  const LONG start = _llseek(file, 0, 1);
  if (start < 0) return LZERROR_BADINHANDLE;
  _llseek(file, 0, 0);
  const UINT n = _lread(file, h, sizeof(h));
  static const BYTE magic[8] = {'S', 'Z', 'D', 'D', 0x88, 0xF0, 0x27, 0x33};
  if (n == (UINT)-1) { _llseek(file, start, 0); return LZERROR_READ; }
  if (n < LZ_HEADER) { _llseek(file, 0, 0); return 0; }
  for (int i = 0; i < 8; ++i) if (h[i] != magic[i]) { _llseek(file, 0, 0); return 0; }
  if (h[8] != 'A') { _llseek(file, 0, 0); return LZERROR_BADVALUE; }
  *missing = h[9];
  *size = (DWORD)h[10] | ((DWORD)h[11] << 8) | ((DWORD)h[12] << 16) | ((DWORD)h[13] << 24);
  return 1;
}

EXPORT INT WINAPI LZStart(void) { return 1; }
EXPORT void WINAPI LZDone(void) {}
EXPORT INT WINAPI LZInit(INT file) {
  DWORD size = 0;
  BYTE missing = 0;
  const int r = read_header(file, &size, &missing);
  if (r < 0) return r;
  if (r == 0) return file;      /* not compressed: the file's own handle serves */
  for (int i = 0; i < LZ_MAX; ++i) {
    if (g_lz[i].in_use) continue;
    LZ *z = &g_lz[i];
    z->in_use = 1;
    z->file = file;
    z->missing = missing;
    z->size = size;
    lz_restart(z);
    return LZ_BASE + i;
  }
  return LZERROR_GLOBALLOC;
}
EXPORT void WINAPI LZClose(INT handle) {
  LZ *z = lz_of(handle);
  if (z) { _lclose(z->file); z->in_use = 0; }
  else _lclose(handle);
}
EXPORT INT WINAPI LZRead(INT handle, CHAR *buffer, INT length) {
  LZ *z = lz_of(handle);
  if (!z) return (INT)_lread(handle, buffer, (UINT)length);
  INT n = 0;
  while (n < length) {
    const int b = out_byte(z);
    if (b < 0) break;
    buffer[n++] = (CHAR)b;
  }
  return n;
}
EXPORT LONG WINAPI LZSeek(INT handle, LONG offset, INT origin) {
  LZ *z = lz_of(handle);
  if (!z) return _llseek(handle, offset, origin);
  LONG target = origin == 0 ? offset : origin == 1 ? (LONG)z->position + offset : (LONG)z->size + offset;
  if (origin < 0 || origin > 2 || target < 0) return LZERROR_BADVALUE;
  if ((DWORD)target > z->size) target = (LONG)z->size;
  if ((DWORD)target < z->position) {
    _llseek(z->file, LZ_HEADER, 0);
    lz_restart(z);
  }
  while (z->position < (DWORD)target) if (out_byte(z) < 0) break;
  return (LONG)z->position;
}
EXPORT LONG WINAPI LZCopy(INT source, INT dest) {
  LONG total = 0;
  CHAR chunk[1024];
  for (;;) {
    const INT n = LZRead(source, chunk, sizeof(chunk));
    if (n < 0) return n;
    if (n == 0) break;
    if (_lwrite(dest, chunk, (UINT)n) != (UINT)n) return LZERROR_WRITE;
    total += n;
  }
  return total;
}
EXPORT LONG WINAPI CopyLZFile(INT source, INT dest) { return LZCopy(source, dest); }

/* "FOO.XXX" is stored as "FOO.XX_", "FOO" as "FOO._", "FOO." as "FOO._": the name of the compressed file that holds the file the program asks for. */
static int compressed_name(const char *name, char *out) {
  size_t_ n = strlen(name);
  if (n + 3 >= 260) return 0;
  memcpy(out, name, n + 1);
  const char *base = name;
  for (const char *p = name; *p; ++p) if (*p == '\\' || *p == '/' || *p == ':') base = p + 1;
  const char *dot = NULL;
  for (const char *p = base; *p; ++p) if (*p == '.') dot = p;
  if (!dot) { out[n++] = '.'; out[n++] = '_'; }
  else if (!dot[1]) out[n++] = '_';
  else if (out[n - 1] == '_') return 0;
  else out[n - 1] = '_';
  out[n] = 0;
  return 1;
}
EXPORT INT WINAPI LZOpenFileA(LPSTR name, LPOFSTRUCT of, WORD style) {
  HFILE file = OpenFile(name, of, style);
  if (file == (HFILE)-1 && !(style & 0x100)) {      /* not there: the compressed file may be */
    char other[260];
    OFSTRUCT again;
    if (compressed_name(name, other) && OpenFile(other, &again, style) != (HFILE)-1) {
      *of = again;
      file = OpenFile(other, of, style);
    }
  }
  if (file == (HFILE)-1 || (style & (0x4000 | 0x100 | 0x200 | 0x1000))) return (INT)file;      /* (a test of existence, a parse, a delete or a create has no LZ file) */
  const INT lz = LZInit((INT)file);
  if (lz < 0) { _lclose((int)file); return lz; }
  return lz;
}
EXPORT INT WINAPI LZOpenFileW(LPWSTR name, LPOFSTRUCT of, WORD style) {
  char narrow[260];
  if (!WideCharToMultiByte(65001, 0, name, -1, narrow, sizeof(narrow), NULL, NULL)) return LZERROR_BADVALUE;
  return LZOpenFileA(narrow, of, style);
}

/* GetExpandedName: the name a compressed file has once expanded: its last character is replaced by the letter the compressor left out ("FOO.EX_" + 'E' -> "FOO.EXE"). */
EXPORT INT WINAPI GetExpandedNameA(LPSTR in, LPSTR out) {
  const int file = _lopen(in, 0);
  if (file < 0) return LZERROR_BADINHANDLE;
  DWORD size;
  BYTE missing;
  const int r = read_header(file, &size, &missing);
  _lclose(file);
  size_t_ n = strlen(in);
  if (n >= 260) return LZERROR_BADVALUE;
  memcpy(out, in, n + 1);
  if (r < 0) return r;
  if (r == 1 && n && out[n - 1] == '_') {
    if (missing) out[n - 1] = (CHAR)missing;
    else out[n - 1] = 0;
  }
  return 1;
}
EXPORT INT WINAPI GetExpandedNameW(LPWSTR in, LPWSTR out) {
  char narrow_in[260], narrow_out[260];
  if (!WideCharToMultiByte(65001, 0, in, -1, narrow_in, sizeof(narrow_in), NULL, NULL)) return LZERROR_BADVALUE;
  const INT r = GetExpandedNameA(narrow_in, narrow_out);
  if (r < 0) return r;
  MultiByteToWideChar(65001, 0, narrow_out, -1, out, 260);
  return r;
}
