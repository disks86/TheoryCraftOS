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

/* winmm's wave output over the kernel's PipeWire graph: every HWAVEOUT is a playback stream of the graph (NtTcosAudio, src/Process/WindowsAudio.hpp),
 * mixed with what native and Linux programs play. Included by winmm.c.
 *
 * A device has a worker thread: it hands the queued WAVEHDRs to the kernel in order (the kernel takes what its ring has room for, never blocking), asks
 * how much has been played, and marks a header done (WHDR_DONE, WOM_DONE through the program's callback, event, window or thread) once everything up to
 * its end has played. Formats: PCM 8/16/24/32-bit and IEEE float 32/64-bit, any rate from 4 to 192 kHz, 1 to 32 channels, also as TCOS_WAVE_FORMAT_EXTENSIBLE.
 * Loops (WHDR_BEGINLOOP/ENDLOOP) play once. PlaySound / sndPlaySound play a RIFF WAVE file or image (SND_FILENAME, SND_MEMORY) this way; aliases and
 * resources are not found (FALSE, or silence with SND_NODEFAULT). */

#include "../tcos_audio.h"

#define TCOS_PID_MAPPER 2
#define TCOS_PID_WAVEOUT 100

#define WAVE_DEVICES 1
#define MAX_WAVEOUTS 32
#define WORKER_PERIOD_MS 5

typedef struct WaveOut {
  ULONG64 stream;
  HWAVEOUT self;
  WAVEFORMATEX format;
  DWORD frame;
  UINT device;               /* the device number it was opened on (0: the speakers or the mapper; 1..: a virtual sink) */
  DWORD_PTR callback, instance;
  DWORD callback_kind;
  CRITICAL_SECTION lock;
  WAVEHDR *head, *tail;      /* queued, in order; [head, current) were given to the kernel and wait to be played */
  WAVEHDR *current;          /* the first header not yet handed over whole */
  DWORD current_offset;
  ULONG64 written;           /* bytes handed to the kernel since the last reset */
  ULONG64 played_base;       /* the kernel's played count at the last reset */
  ULONG64 written_base;
  BOOL paused;
  ULONG64 paused_at;         /* the position while paused (it does not move) */
  volatile LONG stop;
  HANDLE thread, wake;
} WaveOut;

static WaveOut *g_waveouts[MAX_WAVEOUTS];
static SRWLOCK g_waveouts_lock = SRWLOCK_INIT;

/* The output devices: 0 the speakers, then the graph's virtual sinks (1 .. their count); none without an audio device. */
static UINT wave_devices(void) {
  ULONG64 n = 0, virtuals = 0;
  if (audio_call(TCOS_AUDIO_DEVICES, 0, 0, 0, 0, 0, 0, NULL, NULL, &n) != 0 || !n) return 0;
  tcos_audio_virtual_sink(0, NULL, 0, &virtuals);
  return WAVE_DEVICES + (UINT)(virtuals < TCOS_AUDIO_MAX_VIRTUAL ? virtuals : TCOS_AUDIO_MAX_VIRTUAL);
}
/* A device number's virtual sink (its node; 0: the speakers), and its name. */
static ULONG64 wave_device_node(UINT id, char *name, int size) {
  if (id == WAVE_MAPPER || id < WAVE_DEVICES) return 0;
  ULONG64 count = 0;
  return tcos_audio_virtual_sink(id - WAVE_DEVICES, name, size, &count);
}
/* (handles are 0x7A000000 and up: anything that low is a device number) */
#define WAVE_HANDLE_BASE 0x10000u

/* The open device behind a handle (NULL for one that is not open). */
static WaveOut *wave_lookup(HWAVEOUT h) {
  WaveOut *found = NULL;
  AcquireSRWLockShared(&g_waveouts_lock);
  for (int i = 0; i < MAX_WAVEOUTS; ++i)
    if (g_waveouts[i] && g_waveouts[i]->self == h) found = g_waveouts[i];
  ReleaseSRWLockShared(&g_waveouts_lock);
  return found;
}

typedef BOOL (WINAPI *PostMessageAFn)(HWND, UINT, WPARAM, LPARAM);
typedef BOOL (WINAPI *PostThreadMessageAFn)(DWORD, UINT, WPARAM, LPARAM);

static void wave_notify(WaveOut *w, UINT message, DWORD_PTR param) {
  switch (w->callback_kind) {
    case CALLBACK_FUNCTION:
      if (w->callback) ((LPDRVCALLBACK)w->callback)((HDRVR)w->self, message, w->instance, param, 0);
      break;
    case CALLBACK_EVENT:
      if (w->callback) SetEvent((HANDLE)w->callback);
      break;
    case CALLBACK_WINDOW: case CALLBACK_THREAD: {
      /* (user32 is not imported: a program that asks for window or thread callbacks has it loaded already) */
      HMODULE user32 = GetModuleHandleA("user32.dll");
      if (!user32 || !w->callback) break;
      const UINT mm = message == WOM_OPEN ? MM_WOM_OPEN : message == WOM_CLOSE ? MM_WOM_CLOSE : MM_WOM_DONE;
      if (w->callback_kind == CALLBACK_WINDOW) {
        PostMessageAFn post = (PostMessageAFn)(void *)GetProcAddress(user32, "PostMessageA");
        if (post) post((HWND)w->callback, mm, (WPARAM)w->self, (LPARAM)param);
      } else {
        PostThreadMessageAFn post = (PostThreadMessageAFn)(void *)GetProcAddress(user32, "PostThreadMessageA");
        if (post) post((DWORD)w->callback, mm, (WPARAM)w->self, (LPARAM)param);
      }
      break;
    }
    default: break;
  }
}

/* Hands over what the kernel takes and collects the headers that have played; the done ones are returned as a list (lpNext) for the callbacks. */
static WAVEHDR *wave_step(WaveOut *w) {
  WAVEHDR *done = NULL, **done_tail = &done;
  EnterCriticalSection(&w->lock);
  while (!w->paused && w->current) {
    WAVEHDR *h = w->current;
    const DWORD left = h->dwBufferLength - w->current_offset;
    ULONG64 taken = 0;
    if (left) {
      if (audio_call(TCOS_AUDIO_WRITE, w->stream, (ULONG64)(ULONG_PTR)(h->lpData + w->current_offset), left, 0, 0, 0, NULL, NULL, &taken) != 0) break;
      w->current_offset += (DWORD)taken;
      w->written += taken;
    }
    if (w->current_offset < h->dwBufferLength) break;      /* the kernel is full: the rest next time */
    h->reserved = (DWORD_PTR)w->written;                  /* where it ends */
    w->current = h->lpNext;
    w->current_offset = 0;
  }
  ULONG64 queued = 0, written = 0, played = 0;
  if (w->head != w->current && audio_call(TCOS_AUDIO_STATUS, w->stream, 0, 0, 0, 0, 0, &queued, &written, &played) == 0) {
    const ULONG64 since = played >= w->played_base ? played - w->played_base : 0;
    while (w->head && w->head != w->current && (ULONG64)w->head->reserved <= since) {
      WAVEHDR *h = w->head;
      w->head = h->lpNext;
      if (!w->head) w->tail = NULL;
      h->dwFlags = (h->dwFlags & ~WHDR_INQUEUE) | WHDR_DONE;
      h->lpNext = NULL;
      *done_tail = h;
      done_tail = &h->lpNext;
    }
  }
  LeaveCriticalSection(&w->lock);
  return done;
}

static void wave_deliver(WaveOut *w, WAVEHDR *done) {
  while (done) {
    WAVEHDR *next = done->lpNext;
    done->lpNext = NULL;
    wave_notify(w, WOM_DONE, (DWORD_PTR)done);
    done = next;
  }
}

static DWORD WINAPI wave_worker(LPVOID context) {
  WaveOut *w = (WaveOut *)context;
  while (!w->stop) {
    wave_deliver(w, wave_step(w));
    WaitForSingleObject(w->wake, WORKER_PERIOD_MS);
  }
  return 0;
}

/* Bytes played since the last reset (the lock held). */
static ULONG64 wave_position_locked(WaveOut *w) {
  if (w->paused) return w->paused_at;
  ULONG64 queued = 0, written = 0, played = 0;
  audio_call(TCOS_AUDIO_STATUS, w->stream, 0, 0, 0, 0, 0, &queued, &written, &played);
  ULONG64 bytes = played >= w->played_base ? played - w->played_base : 0;
  return bytes > w->written ? w->written : bytes;
}

/* The kernel's format code for a WAVEFORMATEX (0 if it is not one the graph plays). */
EXPORT MMRESULT WINAPI waveOutOpen(LPHWAVEOUT out, UINT id, LPCWAVEFORMATEX format, DWORD_PTR callback, DWORD_PTR instance, DWORD flags) {
  if (out) *out = NULL;
  if (!format) return MMSYSERR_INVALPARAM;
  if (!wave_devices()) return id == WAVE_MAPPER ? MMSYSERR_NODRIVER : MMSYSERR_BADDEVICEID;
  if (id != WAVE_MAPPER && id >= wave_devices() && !(flags & WAVE_MAPPED)) return MMSYSERR_BADDEVICEID;
  const ULONG64 target = (flags & WAVE_MAPPED) ? 0 : wave_device_node(id, NULL, 0);      /* (a virtual sink, or the speakers) */
  if (id != WAVE_MAPPER && id >= WAVE_DEVICES && !(flags & WAVE_MAPPED) && !target) return MMSYSERR_BADDEVICEID;      /* (it went meanwhile) */
  const ULONG64 code = wave_format_code(format);
  if (!code) return WAVERR_BADFORMAT;
  if (flags & WAVE_FORMAT_QUERY) return MMSYSERR_NOERROR;
  if (!out) return MMSYSERR_INVALPARAM;
  WaveOut *w = (WaveOut *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof *w);
  if (!w) return MMSYSERR_NOMEM;
  char name[128];
  program_name(name, sizeof name);
  ULONG64 stream = 0;
  LONG st = audio_call(TCOS_AUDIO_OPEN, target, code, format->nSamplesPerSec, format->nChannels, (ULONG64)(ULONG_PTR)name, lstrlenA(name), NULL, NULL, &stream);
  if (st != 0) {
    HeapFree(GetProcessHeap(), 0, w);
    return st == (LONG)0xC000000D ? WAVERR_BADFORMAT : MMSYSERR_ALLOCATED;
  }
  w->stream = stream;
  w->device = id == WAVE_MAPPER || (flags & WAVE_MAPPED) ? 0 : id;
  w->format = *format;
  w->format.cbSize = 0;
  w->frame = format->nBlockAlign;
  w->callback = callback;
  w->instance = instance;
  w->callback_kind = flags & CALLBACK_TYPEMASK;
  InitializeCriticalSection(&w->lock);
  w->wake = CreateEventA(NULL, FALSE, FALSE, NULL);
  int slot = -1;
  AcquireSRWLockExclusive(&g_waveouts_lock);
  for (int i = 0; i < MAX_WAVEOUTS && slot < 0; ++i) if (!g_waveouts[i]) slot = i;
  if (slot >= 0) {
    static LONG counter;
    w->self = (HWAVEOUT)(ULONG_PTR)(0x7A000000u + (ULONG)InterlockedIncrement(&counter) * 0x10u + (ULONG)slot);
    g_waveouts[slot] = w;
  }
  ReleaseSRWLockExclusive(&g_waveouts_lock);
  if (slot < 0 || !w->wake) {
    if (slot >= 0) { AcquireSRWLockExclusive(&g_waveouts_lock); g_waveouts[slot] = NULL; ReleaseSRWLockExclusive(&g_waveouts_lock); }
    audio_call(TCOS_AUDIO_CLOSE, stream, 0, 0, 0, 0, 0, NULL, NULL, NULL);
    if (w->wake) CloseHandle(w->wake);
    DeleteCriticalSection(&w->lock);
    HeapFree(GetProcessHeap(), 0, w);
    return MMSYSERR_ALLOCATED;
  }
  w->thread = CreateThread(NULL, 0, wave_worker, w, 0, NULL);
  if (w->thread) SetThreadPriority(w->thread, THREAD_PRIORITY_TIME_CRITICAL);
  *out = w->self;
  wave_notify(w, WOM_OPEN, 0);
  return MMSYSERR_NOERROR;
}

EXPORT MMRESULT WINAPI waveOutClose(HWAVEOUT h) {
  WaveOut *w = wave_lookup(h);
  if (!w) return MMSYSERR_INVALHANDLE;
  EnterCriticalSection(&w->lock);
  const BOOL busy = w->head != NULL;
  LeaveCriticalSection(&w->lock);
  if (busy) return WAVERR_STILLPLAYING;
  AcquireSRWLockExclusive(&g_waveouts_lock);
  for (int i = 0; i < MAX_WAVEOUTS; ++i) if (g_waveouts[i] == w) g_waveouts[i] = NULL;
  ReleaseSRWLockExclusive(&g_waveouts_lock);
  InterlockedExchange(&w->stop, 1);
  SetEvent(w->wake);
  if (w->thread) {
    /* (from the program's own callback the worker cannot be waited for: it is the caller) */
    if (GetThreadId(w->thread) != GetCurrentThreadId()) WaitForSingleObject(w->thread, INFINITE);
    CloseHandle(w->thread);
  }
  audio_call(TCOS_AUDIO_CLOSE, w->stream, 0, 0, 0, 0, 0, NULL, NULL, NULL);
  wave_notify(w, WOM_CLOSE, 0);
  CloseHandle(w->wake);
  DeleteCriticalSection(&w->lock);
  HeapFree(GetProcessHeap(), 0, w);
  return MMSYSERR_NOERROR;
}

EXPORT MMRESULT WINAPI waveOutPrepareHeader(HWAVEOUT h, LPWAVEHDR header, UINT size) {
  if (!wave_lookup(h)) return MMSYSERR_INVALHANDLE;
  if (!header || size < sizeof(WAVEHDR)) return MMSYSERR_INVALPARAM;
  if (header->dwFlags & WHDR_INQUEUE) return WAVERR_STILLPLAYING;
  header->dwFlags |= WHDR_PREPARED;
  header->dwFlags &= ~WHDR_DONE;
  return MMSYSERR_NOERROR;
}

EXPORT MMRESULT WINAPI waveOutUnprepareHeader(HWAVEOUT h, LPWAVEHDR header, UINT size) {
  if (!wave_lookup(h)) return MMSYSERR_INVALHANDLE;
  if (!header || size < sizeof(WAVEHDR)) return MMSYSERR_INVALPARAM;
  if (header->dwFlags & WHDR_INQUEUE) return WAVERR_STILLPLAYING;
  header->dwFlags &= ~WHDR_PREPARED;
  header->dwFlags |= WHDR_DONE;
  return MMSYSERR_NOERROR;
}

EXPORT MMRESULT WINAPI waveOutWrite(HWAVEOUT h, LPWAVEHDR header, UINT size) {
  WaveOut *w = wave_lookup(h);
  if (!w) return MMSYSERR_INVALHANDLE;
  if (!header || size < sizeof(WAVEHDR) || (!header->lpData && header->dwBufferLength)) return MMSYSERR_INVALPARAM;
  if (!(header->dwFlags & WHDR_PREPARED)) return WAVERR_UNPREPARED;
  if (header->dwFlags & WHDR_INQUEUE) return WAVERR_STILLPLAYING;
  header->dwBufferLength -= header->dwBufferLength % (w->frame ? w->frame : 1);      /* (a partial frame at the end is not played) */
  EnterCriticalSection(&w->lock);
  header->dwFlags = (header->dwFlags & ~WHDR_DONE) | WHDR_INQUEUE;
  header->lpNext = NULL;
  header->reserved = 0;
  if (w->tail) w->tail->lpNext = header;
  else w->head = header;
  w->tail = header;
  if (!w->current) {
    w->current = header;
    w->current_offset = 0;
  }
  LeaveCriticalSection(&w->lock);
  SetEvent(w->wake);
  return MMSYSERR_NOERROR;
}

EXPORT MMRESULT WINAPI waveOutPause(HWAVEOUT h) {
  WaveOut *w = wave_lookup(h);
  if (!w) return MMSYSERR_INVALHANDLE;
  EnterCriticalSection(&w->lock);
  if (!w->paused) {
    audio_call(TCOS_AUDIO_PAUSE, w->stream, 1, 0, 0, 0, 0, NULL, NULL, NULL);
    w->paused_at = wave_position_locked(w);
    w->paused = TRUE;
  }
  LeaveCriticalSection(&w->lock);
  return MMSYSERR_NOERROR;
}

EXPORT MMRESULT WINAPI waveOutRestart(HWAVEOUT h) {
  WaveOut *w = wave_lookup(h);
  if (!w) return MMSYSERR_INVALHANDLE;
  EnterCriticalSection(&w->lock);
  w->paused = FALSE;
  audio_call(TCOS_AUDIO_PAUSE, w->stream, 0, 0, 0, 0, 0, NULL, NULL, NULL);
  LeaveCriticalSection(&w->lock);
  SetEvent(w->wake);
  return MMSYSERR_NOERROR;
}

/* Stops at once: everything queued is returned done, the position starts again at 0. */
EXPORT MMRESULT WINAPI waveOutReset(HWAVEOUT h) {
  WaveOut *w = wave_lookup(h);
  if (!w) return MMSYSERR_INVALHANDLE;
  EnterCriticalSection(&w->lock);
  audio_call(TCOS_AUDIO_FLUSH, w->stream, 0, 0, 0, 0, 0, NULL, NULL, NULL);
  WAVEHDR *done = w->head;
  for (WAVEHDR *x = done; x; x = x->lpNext) x->dwFlags = (x->dwFlags & ~WHDR_INQUEUE) | WHDR_DONE;
  w->head = w->tail = w->current = NULL;
  w->current_offset = 0;
  w->written = 0;
  w->paused_at = 0;
  ULONG64 queued = 0, written = 0, played = 0;
  if (audio_call(TCOS_AUDIO_STATUS, w->stream, 0, 0, 0, 0, 0, &queued, &written, &played) == 0) w->played_base = played;
  w->paused = FALSE;
  audio_call(TCOS_AUDIO_PAUSE, w->stream, 0, 0, 0, 0, 0, NULL, NULL, NULL);
  LeaveCriticalSection(&w->lock);
  wave_deliver(w, done);
  return MMSYSERR_NOERROR;
}

EXPORT MMRESULT WINAPI waveOutBreakLoop(HWAVEOUT h) { return wave_lookup(h) ? MMSYSERR_NOERROR : MMSYSERR_INVALHANDLE; }

EXPORT MMRESULT WINAPI waveOutGetPosition(HWAVEOUT h, LPMMTIME time, UINT size) {
  WaveOut *w = wave_lookup(h);
  if (!w) return MMSYSERR_INVALHANDLE;
  if (!time || size < sizeof(MMTIME)) return MMSYSERR_INVALPARAM;
  EnterCriticalSection(&w->lock);
  const ULONG64 bytes = wave_position_locked(w);
  LeaveCriticalSection(&w->lock);
  const ULONG64 samples = w->frame ? bytes / w->frame : 0;
  switch (time->wType) {
    case TIME_SAMPLES: time->u.sample = (DWORD)samples; break;
    case TIME_MS: time->u.ms = (DWORD)(samples * 1000 / w->format.nSamplesPerSec); break;
    default: time->wType = TIME_BYTES; time->u.cb = (DWORD)bytes; break;
  }
  return MMSYSERR_NOERROR;
}

static MMRESULT wave_volume(HWAVEOUT h, DWORD *volume, BOOL set) {
  ULONG64 stream = 0;
  if ((ULONG_PTR)h >= WAVE_HANDLE_BASE) {      /* a handle, not a device number: that stream's own volume */
    WaveOut *w = wave_lookup(h);
    if (!w) return MMSYSERR_INVALHANDLE;
    stream = w->stream;
  } else if ((ULONG_PTR)h >= wave_devices()) {
    return MMSYSERR_BADDEVICEID;
  } else if ((ULONG_PTR)h >= WAVE_DEVICES) {      /* a virtual sink's own (master) volume */
    const ULONG64 node = wave_device_node((UINT)(ULONG_PTR)h, NULL, 0);
    if (!node) return MMSYSERR_BADDEVICEID;
    stream = TCOS_AUDIO_VIRTUAL_STREAM | node;
  }
  if (set) return audio_call(TCOS_AUDIO_SET_VOLUME, stream, *volume & 0xFFFF, *volume >> 16, 0, 0, 0, NULL, NULL, NULL) == 0 ? MMSYSERR_NOERROR : MMSYSERR_ERROR;
  ULONG64 v = 0;
  if (audio_call(TCOS_AUDIO_GET_VOLUME, stream, 0, 0, 0, 0, 0, NULL, NULL, &v) != 0) return MMSYSERR_ERROR;
  *volume = (DWORD)v;
  return MMSYSERR_NOERROR;
}

EXPORT MMRESULT WINAPI waveOutGetVolume(HWAVEOUT h, LPDWORD volume) { return volume ? wave_volume(h, volume, FALSE) : MMSYSERR_INVALPARAM; }
EXPORT MMRESULT WINAPI waveOutSetVolume(HWAVEOUT h, DWORD volume) { return wave_volume(h, &volume, TRUE); }

EXPORT MMRESULT WINAPI waveOutGetID(HWAVEOUT h, LPUINT id) {
  if (!wave_lookup(h)) return MMSYSERR_INVALHANDLE;
  if (!id) return MMSYSERR_INVALPARAM;
  *id = wave_lookup(h)->device;
  return MMSYSERR_NOERROR;
}

EXPORT MMRESULT WINAPI waveOutGetPitch(HWAVEOUT h, LPDWORD v) { (void)v; return wave_lookup(h) ? MMSYSERR_NOTSUPPORTED : MMSYSERR_INVALHANDLE; }
EXPORT MMRESULT WINAPI waveOutSetPitch(HWAVEOUT h, DWORD v) { (void)v; return wave_lookup(h) ? MMSYSERR_NOTSUPPORTED : MMSYSERR_INVALHANDLE; }
EXPORT MMRESULT WINAPI waveOutGetPlaybackRate(HWAVEOUT h, LPDWORD v) { (void)v; return wave_lookup(h) ? MMSYSERR_NOTSUPPORTED : MMSYSERR_INVALHANDLE; }
EXPORT MMRESULT WINAPI waveOutSetPlaybackRate(HWAVEOUT h, DWORD v) { (void)v; return wave_lookup(h) ? MMSYSERR_NOTSUPPORTED : MMSYSERR_INVALHANDLE; }
EXPORT MMRESULT WINAPI waveOutMessage(HWAVEOUT h, UINT message, DWORD_PTR a, DWORD_PTR b) { (void)h; (void)message; (void)a; (void)b; return MMSYSERR_NOTSUPPORTED; }

static void fill_caps_name(WCHAR *w, CHAR *a, UINT id) {
  char sink[MAXPNAMELEN];
  const char *name = id == WAVE_MAPPER ? "Microsoft Sound Mapper" : "TheoryCraftOS Audio";
  if (id != WAVE_MAPPER && id >= WAVE_DEVICES && wave_device_node(id, sink, sizeof sink)) name = sink;      /* (a virtual sink: its description) */
  int i = 0;
  for (; name[i] && i < MAXPNAMELEN - 1; ++i) { if (w) w[i] = (WCHAR)name[i]; if (a) a[i] = name[i]; }
  if (w) w[i] = 0;
  if (a) a[i] = 0;
}

#define WAVE_ALL_FORMATS 0x000FFFFFu
#define WAVE_CAPS (WAVECAPS_VOLUME | WAVECAPS_LRVOLUME | WAVECAPS_SAMPLEACCURATE)

EXPORT MMRESULT WINAPI waveOutGetDevCapsA(UINT_PTR id, LPWAVEOUTCAPSA caps, UINT size) {
  if (!caps || size < sizeof *caps) return MMSYSERR_INVALPARAM;
  if (!wave_devices()) return (UINT)id == WAVE_MAPPER ? MMSYSERR_NODRIVER : MMSYSERR_BADDEVICEID;
  if ((UINT)id != WAVE_MAPPER && id >= WAVE_HANDLE_BASE) { WaveOut *w = wave_lookup((HWAVEOUT)id); if (!w) return MMSYSERR_BADDEVICEID; id = w->device; }
  else if ((UINT)id != WAVE_MAPPER && id >= wave_devices()) return MMSYSERR_BADDEVICEID;
  caps->wMid = MM_MICROSOFT;
  caps->wPid = (UINT)id == WAVE_MAPPER ? TCOS_PID_MAPPER : TCOS_PID_WAVEOUT;
  caps->vDriverVersion = 0x0100;
  fill_caps_name(NULL, caps->szPname, (UINT)id);
  caps->dwFormats = WAVE_ALL_FORMATS;
  caps->wChannels = 2;
  caps->wReserved1 = 0;
  caps->dwSupport = WAVE_CAPS;
  return MMSYSERR_NOERROR;
}

EXPORT MMRESULT WINAPI waveOutGetDevCapsW(UINT_PTR id, LPWAVEOUTCAPSW caps, UINT size) {
  if (!caps || size < sizeof *caps) return MMSYSERR_INVALPARAM;
  if (!wave_devices()) return (UINT)id == WAVE_MAPPER ? MMSYSERR_NODRIVER : MMSYSERR_BADDEVICEID;
  if ((UINT)id != WAVE_MAPPER && id >= WAVE_HANDLE_BASE) { WaveOut *w = wave_lookup((HWAVEOUT)id); if (!w) return MMSYSERR_BADDEVICEID; id = w->device; }
  else if ((UINT)id != WAVE_MAPPER && id >= wave_devices()) return MMSYSERR_BADDEVICEID;
  caps->wMid = MM_MICROSOFT;
  caps->wPid = (UINT)id == WAVE_MAPPER ? TCOS_PID_MAPPER : TCOS_PID_WAVEOUT;
  caps->vDriverVersion = 0x0100;
  fill_caps_name(caps->szPname, NULL, (UINT)id);
  caps->dwFormats = WAVE_ALL_FORMATS;
  caps->wChannels = 2;
  caps->wReserved1 = 0;
  caps->dwSupport = WAVE_CAPS;
  return MMSYSERR_NOERROR;
}

EXPORT UINT WINAPI waveOutGetNumDevs(void) { return wave_devices(); }

static const char *wave_error_text(MMRESULT e) {
  switch (e) {
    case MMSYSERR_NOERROR: return "The command completed successfully.";
    case MMSYSERR_ERROR: return "An unspecified error occurred.";
    case MMSYSERR_BADDEVICEID: return "The specified device ID is out of range.";
    case MMSYSERR_NOTENABLED: return "The driver failed to enable.";
    case MMSYSERR_ALLOCATED: return "The specified resource is already in use.";
    case MMSYSERR_INVALHANDLE: return "The specified device handle is invalid.";
    case MMSYSERR_NODRIVER: return "There is no driver installed on your system.";
    case MMSYSERR_NOMEM: return "There is not enough memory available for this task.";
    case MMSYSERR_NOTSUPPORTED: return "This function is not supported.";
    case MMSYSERR_INVALFLAG: return "An invalid flag was passed to a system function.";
    case MMSYSERR_INVALPARAM: return "An invalid parameter was passed to a system function.";
    case WAVERR_BADFORMAT: return "The specified format is not supported or cannot be translated.";
    case WAVERR_STILLPLAYING: return "Cannot perform this operation while media data is still playing.";
    case WAVERR_UNPREPARED: return "The wave header was not prepared.";
    case WAVERR_SYNC: return "Cannot open the device synchronously.";
    default: return NULL;
  }
}

EXPORT MMRESULT WINAPI waveOutGetErrorTextA(MMRESULT error, LPSTR text, UINT size) {
  const char *t = wave_error_text(error);
  if (!t) return MMSYSERR_BADERRNUM;
  if (!text || !size) return MMSYSERR_INVALPARAM;
  UINT i = 0;
  for (; t[i] && i + 1 < size; ++i) text[i] = t[i];
  text[i] = 0;
  return MMSYSERR_NOERROR;
}

EXPORT MMRESULT WINAPI waveOutGetErrorTextW(MMRESULT error, LPWSTR text, UINT size) {
  const char *t = wave_error_text(error);
  if (!t) return MMSYSERR_BADERRNUM;
  if (!text || !size) return MMSYSERR_INVALPARAM;
  UINT i = 0;
  for (; t[i] && i + 1 < size; ++i) text[i] = (WCHAR)t[i];
  text[i] = 0;
  return MMSYSERR_NOERROR;
}

/* ---- PlaySound: a RIFF WAVE through a wave output device ------------------------------------------------------------------------------------------- */

typedef struct SoundJob {
  BYTE *image;          /* the whole file (owned) */
  const WAVEFORMATEX *format;
  const BYTE *data;
  DWORD length;
  BOOL loop;
  volatile LONG cancel;
} SoundJob;

static SRWLOCK g_sound_lock = SRWLOCK_INIT;
static SoundJob *g_sound;      /* the asynchronous sound playing now */
static HANDLE g_sound_thread;

/* Finds "fmt " and "data" in a RIFF WAVE image. */
static BOOL parse_wave(const BYTE *image, DWORD size, const WAVEFORMATEX **format, const BYTE **data, DWORD *length) {
  if (size < 12 || memcmp(image, "RIFF", 4) != 0 || memcmp(image + 8, "WAVE", 4) != 0) return FALSE;
  *format = NULL;
  *data = NULL;
  for (DWORD at = 12; size - at >= 8;) {
    const DWORD chunk = *(const DWORD *)(image + at + 4);
    const BYTE *body = image + at + 8;
    const DWORD room = size - at - 8;
    const DWORD n = chunk < room ? chunk : room;
    if (!memcmp(image + at, "fmt ", 4) && n >= 16) *format = (const WAVEFORMATEX *)body;
    else if (!memcmp(image + at, "data", 4)) { *data = body; *length = n; }
    if (chunk > room || room - chunk < (chunk & 1)) break;
    at += 8 + chunk + (chunk & 1);
  }
  return *format && *data;
}

static void play_job(SoundJob *job) {
  WAVEFORMATEX f = *job->format;
  if (f.wFormatTag != TCOS_WAVE_FORMAT_EXTENSIBLE) f.cbSize = 0;
  HANDLE done = CreateEventA(NULL, FALSE, FALSE, NULL);
  HWAVEOUT h = NULL;
  if (!done) return;
  if (waveOutOpen(&h, WAVE_MAPPER, f.wFormatTag == TCOS_WAVE_FORMAT_EXTENSIBLE ? job->format : &f, (DWORD_PTR)done, 0, CALLBACK_EVENT) != MMSYSERR_NOERROR) {
    CloseHandle(done);
    return;
  }
  WAVEHDR header;
  memset(&header, 0, sizeof header);
  do {
    header.lpData = (LPSTR)job->data;
    header.dwBufferLength = job->length;
    header.dwFlags = 0;
    waveOutPrepareHeader(h, &header, sizeof header);
    waveOutWrite(h, &header, sizeof header);
    while (!(header.dwFlags & WHDR_DONE) && !job->cancel) WaitForSingleObject(done, 20);
    if (job->cancel) waveOutReset(h);
    waveOutUnprepareHeader(h, &header, sizeof header);
  } while (job->loop && !job->cancel);
  waveOutClose(h);
  CloseHandle(done);
}

static DWORD WINAPI sound_thread(LPVOID context) {
  SoundJob *job = (SoundJob *)context;
  play_job(job);
  return 0;
}

static void stop_sound(void) {
  AcquireSRWLockExclusive(&g_sound_lock);
  SoundJob *job = g_sound;
  HANDLE thread = g_sound_thread;
  g_sound = NULL;
  g_sound_thread = NULL;
  ReleaseSRWLockExclusive(&g_sound_lock);
  if (!job) return;
  InterlockedExchange(&job->cancel, 1);
  if (thread) {
    WaitForSingleObject(thread, INFINITE);
    CloseHandle(thread);
  }
  HeapFree(GetProcessHeap(), 0, job->image);
  HeapFree(GetProcessHeap(), 0, job);
}

static BYTE *read_whole_file(LPCWSTR path, DWORD *size) {
  HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
  if (f == INVALID_HANDLE_VALUE) return NULL;
  LARGE_INTEGER n;
  BYTE *image = NULL;
  if (GetFileSizeEx(f, &n) && n.QuadPart > 0 && n.QuadPart < (64 << 20)) {
    image = (BYTE *)HeapAlloc(GetProcessHeap(), 0, (SIZE_T)n.QuadPart);
    DWORD got = 0;
    if (image && (!ReadFile(f, image, (DWORD)n.QuadPart, &got, NULL) || got != (DWORD)n.QuadPart)) {
      HeapFree(GetProcessHeap(), 0, image);
      image = NULL;
    }
    *size = (DWORD)n.QuadPart;
  }
  CloseHandle(f);
  return image;
}

static BOOL play_sound(LPCWSTR wide_name, const BYTE *memory, DWORD flags) {
  if (!(flags & SND_NOSTOP) || !memory) stop_sound();
  if (!wide_name && !memory) return TRUE;      /* (NULL: stop what plays) */
  BYTE *image = NULL;
  DWORD size = 0;
  if (flags & SND_MEMORY) {
    if (!memory) return FALSE;
    /* the size is not given: the RIFF header says it */
    size = *(const DWORD *)(memory + 4) + 8;
    image = (BYTE *)HeapAlloc(GetProcessHeap(), 0, size);
    if (!image) return FALSE;
    memcpy(image, memory, size);
  } else if (!(flags & SND_ALIAS) && !(flags & 0x40000 /* SND_RESOURCE's own bit */)) {
    image = read_whole_file(wide_name, &size);
  }
  SoundJob *job = image ? (SoundJob *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof *job) : NULL;
  if (!job || !parse_wave(image, size, &job->format, &job->data, &job->length)) {
    if (image) HeapFree(GetProcessHeap(), 0, image);
    if (job) HeapFree(GetProcessHeap(), 0, job);
    return FALSE;      /* (with or without SND_NODEFAULT: there is no default sound to play instead) */
  }
  job->image = image;
  job->loop = (flags & SND_LOOP) && (flags & SND_ASYNC);
  if (flags & SND_ASYNC) {
    HANDLE thread = CreateThread(NULL, 0, sound_thread, job, 0, NULL);
    if (!thread) {
      HeapFree(GetProcessHeap(), 0, image);
      HeapFree(GetProcessHeap(), 0, job);
      return FALSE;
    }
    AcquireSRWLockExclusive(&g_sound_lock);
    g_sound = job;
    g_sound_thread = thread;
    ReleaseSRWLockExclusive(&g_sound_lock);
    return TRUE;
  }
  play_job(job);
  HeapFree(GetProcessHeap(), 0, image);
  HeapFree(GetProcessHeap(), 0, job);
  return TRUE;
}

EXPORT BOOL WINAPI PlaySoundW(LPCWSTR sound, HMODULE module, DWORD flags) {
  (void)module;
  return play_sound((flags & SND_MEMORY) ? NULL : sound, (flags & SND_MEMORY) ? (const BYTE *)sound : NULL, flags);
}

EXPORT BOOL WINAPI PlaySoundA(LPCSTR sound, HMODULE module, DWORD flags) {
  (void)module;
  if (!sound || (flags & SND_MEMORY)) return play_sound(NULL, (const BYTE *)sound, flags);
  WCHAR wide[MAX_PATH];
  if (!MultiByteToWideChar(CP_ACP, 0, sound, -1, wide, MAX_PATH)) return FALSE;
  return play_sound(wide, NULL, flags);
}

#undef PlaySound
EXPORT BOOL WINAPI PlaySound(LPCSTR sound, HMODULE module, DWORD flags) { return PlaySoundA(sound, module, flags); }
EXPORT BOOL WINAPI sndPlaySoundA(LPCSTR sound, UINT flags) { return PlaySoundA(sound, NULL, (flags & ~SND_RESOURCE) | ((flags & SND_MEMORY) ? 0 : SND_FILENAME)); }
EXPORT BOOL WINAPI sndPlaySoundW(LPCWSTR sound, UINT flags) { return PlaySoundW(sound, NULL, (flags & ~SND_RESOURCE) | ((flags & SND_MEMORY) ? 0 : SND_FILENAME)); }
