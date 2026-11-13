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

/* What the Windows audio libraries share (winmm.dll's waveOut and waveIn, dsound.dll, mmdevapi.dll's WASAPI): the call into NtTcosAudio, the kernel's way into
 * its PipeWire graph (src/Process/WindowsAudio.hpp has the operations), the wave formats the graph takes, and the bit of maths the libraries need
 * without a C runtime (decibels, square roots, sines). Include it after <windows.h>; the libraries are linked against ntdll.dll. */
#pragma once

__declspec(dllimport) LONG NTAPI NtTcosAudio(ULONG64 *arguments);

enum { TCOS_AUDIO_OPEN = 1, TCOS_AUDIO_CLOSE, TCOS_AUDIO_WRITE, TCOS_AUDIO_STATUS, TCOS_AUDIO_FLUSH, TCOS_AUDIO_PAUSE, TCOS_AUDIO_SET_VOLUME,
       TCOS_AUDIO_GET_VOLUME, TCOS_AUDIO_DEVICES, TCOS_AUDIO_SET_MUTE, TCOS_AUDIO_OPEN_CAPTURE, TCOS_AUDIO_READ, TCOS_AUDIO_VIRTUAL_SINKS };
#define TCOS_AUDIO_SOURCE_STREAM 0xFFFFFFFFull      /* (the volume calls' stream number for the microphone's master gain; 0 is the master volume) */
#define TCOS_AUDIO_VIRTUAL_STREAM 0x80000000ull     /* (| a virtual sink's node: the volume calls' stream number for its master volume) */
#define TCOS_AUDIO_MAX_VIRTUAL 16                   /* (virtual sinks the graph has at most) */
#define TCOS_STATUS_INVALID_PARAMETER ((LONG)0xC000000D)

/* One call: [0] operation, [1] stream, [2..6] parameters; *out5, *out6, *out7 (each may be NULL) get what the kernel wrote back. */
static inline LONG audio_call(ULONG64 op, ULONG64 stream, ULONG64 a, ULONG64 b, ULONG64 c, ULONG64 d, ULONG64 e, ULONG64 *out5, ULONG64 *out6, ULONG64 *out7) {
  ULONG64 w[8] = {op, stream, a, b, c, d, e, 0};
  LONG st = NtTcosAudio(w);
  if (out5) *out5 = w[5];
  if (out6) *out6 = w[6];
  if (out7) *out7 = w[7];
  return st;
}

/* Where a stream is, in bytes of its format: what the mixer has not taken yet (*waiting), what has not been played (*unplayed: those and the device's
 * queue), what was written and what was played. Any pointer may be NULL. */
static inline LONG tcos_audio_status(ULONG64 stream, ULONG64 *waiting, ULONG64 *unplayed, ULONG64 *written, ULONG64 *played) {
  ULONG64 w[8] = {TCOS_AUDIO_STATUS, stream, 0, 0, 0, 0, 0, 0};
  LONG st = NtTcosAudio(w);
  if (st != 0) w[4] = w[5] = w[6] = w[7] = 0;
  if (waiting) *waiting = w[4];
  if (unplayed) *unplayed = w[5];
  if (written) *written = w[6];
  if (played) *played = w[7];
  return st;
}

/* The number of output devices (0 or 1). */
static inline ULONG64 tcos_audio_devices(void) {
  ULONG64 n = 0;
  return audio_call(TCOS_AUDIO_DEVICES, 0, 0, 0, 0, 0, 0, NULL, NULL, &n) == 0 ? n : 0;
}

/* The number of input devices (0 or 1: a microphone is there). */
static inline ULONG64 tcos_audio_input_devices(void) {
  ULONG64 n = 0;
  return audio_call(TCOS_AUDIO_DEVICES, 0, 0, 0, 0, 0, 0, NULL, &n, NULL) == 0 ? n : 0;
}

/* (mmreg.h's WAVEFORMATEXTENSIBLE, without pulling in ksmedia.h) */
#define TCOS_WAVE_FORMAT_EXTENSIBLE 0xFFFE
#define TCOS_WAVE_FORMAT_IEEE_FLOAT 3
typedef struct {
  WAVEFORMATEX Format;
  WORD wValidBitsPerSample;
  DWORD dwChannelMask;
  GUID SubFormat;
} TcosWaveFormatExtensible;

/* The plain tag of a format (WAVE_FORMAT_PCM or IEEE float), looking through WAVE_FORMAT_EXTENSIBLE; 0 for anything else. */
static inline WORD wave_format_tag(const WAVEFORMATEX *f) {
  WORD tag = f->wFormatTag;
  if (tag == TCOS_WAVE_FORMAT_EXTENSIBLE) {
    if (f->cbSize < 22) return 0;
    const TcosWaveFormatExtensible *x = (const TcosWaveFormatExtensible *)f;
    /* KSDATAFORMAT_SUBTYPE_PCM / _IEEE_FLOAT are {00000001/3-0000-0010-8000-00aa00389b71} */
    if (x->SubFormat.Data2 != 0 || x->SubFormat.Data3 != 0x10 || x->SubFormat.Data4[0] != 0x80 || x->SubFormat.Data4[7] != 0x71) return 0;
    tag = (WORD)x->SubFormat.Data1;
  }
  return tag == 1 || tag == TCOS_WAVE_FORMAT_IEEE_FLOAT ? tag : 0;
}

/* The code NtTcosAudio's OPEN takes for a format (tag | bits << 16), or 0 when the graph cannot play it: PCM 8/16/24/32-bit, float 32/64-bit, 1 to
 * 32 channels, 4 to 192 kHz, whole samples. */
static inline ULONG64 wave_format_code(const WAVEFORMATEX *f) {
  const WORD tag = wave_format_tag(f);
  if (!tag) return 0;
  const WORD bits = f->wBitsPerSample;
  if (tag == 1 && bits != 8 && bits != 16 && bits != 24 && bits != 32) return 0;
  if (tag == TCOS_WAVE_FORMAT_IEEE_FLOAT && bits != 32 && bits != 64) return 0;
  if (f->nChannels < 1 || f->nChannels > 32 || f->nSamplesPerSec < 4000 || f->nSamplesPerSec > 192000) return 0;
  if (f->nBlockAlign != f->nChannels * (bits / 8)) return 0;
  return (ULONG64)tag | (ULONG64)bits << 16;
}

/* The program's file name (no folder), UTF-8 as far as ASCII goes: the name its streams have in the graph. */
static inline void program_name(char *out, int size) {
  char path[MAX_PATH];
  const DWORD n = GetModuleFileNameA(NULL, path, sizeof path);
  int start = 0;
  for (DWORD i = 0; i < n; ++i) if (path[i] == '\\' || path[i] == '/') start = (int)i + 1;
  int k = 0;
  for (DWORD i = (DWORD)start; i < n && k + 1 < size; ++i) out[k++] = path[i];
  out[k] = 0;
}

/* The virtual sink number `which` (0, 1, ...): its node (0 past the last), its description in `text` (UTF-8, NUL-ended), and how many there are. */
static inline ULONG64 tcos_audio_virtual_sink(ULONG64 which, char *text, int size, ULONG64 *count) {
  ULONG64 node = 0, n = 0, length = 0;
  if (audio_call(TCOS_AUDIO_VIRTUAL_SINKS, 0, which, (ULONG64)(ULONG_PTR)text, size > 1 ? (ULONG64)(size - 1) : 0, 0, 0, &node, &n, &length) != 0) node = n = length = 0;
  if (text && size > 0) text[length < (ULONG64)size ? length : (ULONG64)size - 1] = 0;
  if (count) *count = n;
  return node;
}

/* A virtual sink's device GUID (DirectSound's, and the last part of its WASAPI endpoint id): the speakers' {5af3a3c2-6e1b-4b4e-8d2f-...} with the
 * sink's node in its last six bytes. tcos_audio_virtual_node gives the node back (0 when the GUID is not one of these). */
static inline void tcos_audio_virtual_guid(ULONG64 node, GUID *out) {
  const GUID base = {0x5AF3A3C2, 0x6E1B, 0x4B4E, {0x8D, 0x2F, 0, 0, 0, 0, 0, 0}};
  *out = base;
  for (int k = 0; k < 6; ++k) out->Data4[2 + k] = (BYTE)(node >> (8 * (5 - k)));
}
static inline ULONG64 tcos_audio_virtual_node(const GUID *g) {
  if (!g || g->Data1 != 0x5AF3A3C2 || g->Data2 != 0x6E1B || g->Data3 != 0x4B4E || g->Data4[0] != 0x8D || g->Data4[1] != 0x2F) return 0;
  ULONG64 node = 0;
  for (int k = 0; k < 6; ++k) node = node << 8 | g->Data4[2 + k];
  /* (only a virtual sink there is now: the speakers' own GUID has other bytes there) */
  for (ULONG64 i = 0; i < TCOS_AUDIO_MAX_VIRTUAL; ++i) {
    ULONG64 count = 0;
    const ULONG64 n = tcos_audio_virtual_sink(i, NULL, 0, &count);
    if (!n) break;
    if (n == node) return node;
  }
  return 0;
}

/* Opens a stream of the graph for `format` (see wave_format_code), a playback stream or (`capture`) a record stream of the microphone or (`monitor`)
 * of what the mixer plays; with `target` (a virtual sink's node), into that sink or (capture) of its monitor. 0 when it cannot be opened. */
static inline ULONG64 tcos_audio_open_to(const WAVEFORMATEX *format, BOOL capture, BOOL monitor, ULONG64 target, LONG *status) {
  char name[128];
  program_name(name, sizeof name);
  ULONG64 stream = 0;
  const ULONG64 code = wave_format_code(format);
  const ULONG64 which = target ? target : capture && monitor ? 1 : 0;
  LONG st = code ? audio_call(capture ? TCOS_AUDIO_OPEN_CAPTURE : TCOS_AUDIO_OPEN, which, code, format->nSamplesPerSec, format->nChannels, (ULONG64)(ULONG_PTR)name, lstrlenA(name), NULL, NULL, &stream)
                 : TCOS_STATUS_INVALID_PARAMETER;
  if (status) *status = st;
  return st == 0 ? stream : 0;
}
static inline ULONG64 tcos_audio_open_from(const WAVEFORMATEX *format, BOOL capture, BOOL monitor, LONG *status) {
  return tcos_audio_open_to(format, capture, monitor, 0, status);
}
static inline ULONG64 tcos_audio_open_kind(const WAVEFORMATEX *format, BOOL capture, LONG *status) { return tcos_audio_open_from(format, capture, FALSE, status); }
static inline ULONG64 tcos_audio_open(const WAVEFORMATEX *format, LONG *status) { return tcos_audio_open_from(format, FALSE, FALSE, status); }

/* ---- maths without a C runtime ------------------------------------------------------------------------------------------------------------------ */

static inline double tcos_sqrt(double x) {
  if (!(x > 0)) return 0;
  double r;
  __asm__("sqrtsd %1, %0" : "=x"(r) : "x"(x));
  return r;
}

/* 2^x, good to about 1e-7 relative over the range audio needs (|x| < 60). */
static inline double tcos_exp2(double x) {
  if (x < -1000) return 0;
  if (x > 1000) x = 1000;
  double whole = (double)(LONG64)x;
  if (whole > x) whole -= 1;      /* floor */
  const double f = x - whole;      /* 0 .. 1 */
  /* 2^f = e^(f ln 2) by its series */
  const double y = f * 0.69314718055994530942;
  double term = 1, sum = 1;
  for (int n = 1; n < 14; ++n) {
    term *= y / n;
    sum += term;
  }
  LONG64 e = (LONG64)whole;
  double scale = 1;
  const double base = e < 0 ? 0.5 : 2.0;
  for (LONG64 k = e < 0 ? -e : e; k > 0; --k) scale *= base;
  return sum * scale;
}

/* log2(x) for x > 0 (-1000 for 0 and below). */
static inline double tcos_log2(double x) {
  if (!(x > 0)) return -1000;
  double whole = 0;
  while (x >= 2) { x *= 0.5; whole += 1; }
  while (x < 1) { x *= 2; whole -= 1; }
  /* ln(x) = 2 atanh((x - 1) / (x + 1)) for x in [1, 2) */
  const double t = (x - 1) / (x + 1), t2 = t * t;
  double term = t, sum = 0;
  for (int n = 1; n < 40; n += 2) {
    sum += term / n;
    term *= t2;
  }
  return whole + 2 * sum * 1.44269504088896340736;
}

/* Decibels to an amplitude and back. */
static inline double tcos_db_to_gain(double db) { return tcos_exp2(db * (3.32192809488736234787 / 20.0)); }
static inline double tcos_gain_to_db(double gain) { return 20.0 / 3.32192809488736234787 * tcos_log2(gain); }

static inline double tcos_sin(double x) {
  const double pi = 3.14159265358979323846;
  if (x > 1e6 || x < -1e6) return 0;
  while (x > pi) x -= 2 * pi;
  while (x < -pi) x += 2 * pi;
  double term = x, sum = x;
  for (int n = 1; n < 12; ++n) {
    term *= -x * x / ((2.0 * n) * (2.0 * n + 1.0));
    sum += term;
  }
  return sum;
}
static inline double tcos_cos(double x) { return tcos_sin(x + 1.57079632679489661923); }
