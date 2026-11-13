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

/* winmm's wave input over the kernel's PipeWire graph: every HWAVEIN is a capture stream of the graph (NtTcosAudio's OPEN_CAPTURE and READ), which hears
 * the microphone along with every other program that records. Included by winmm.c after waveout.inc. The devices: 0 the microphone (when there is
 * one), then the graph's virtual sources (module-remap-source, -virtual-source, echo-cancel-source, ...), a device each; the mapper is device 0.
 *
 * A device has a worker thread: while recording (waveInStart until waveInStop or waveInReset) it reads what the kernel has into the queued WAVEHDRs in order
 * and marks a header done (dwBytesRecorded, WHDR_DONE, WIM_DATA through the program's callback, event, window or thread) once it is full. waveInStop returns
 * the header being filled with what it holds; waveInReset returns every header and starts the position at 0 again. Formats as for wave output. */

#define TCOS_PID_WAVEIN 101
#define MAX_WAVEINS 16

typedef struct WaveIn {
  ULONG64 stream;
  UINT device;               /* (its device number) */
  HWAVEIN self;
  WAVEFORMATEX format;
  DWORD frame;
  DWORD_PTR callback, instance;
  DWORD callback_kind;
  CRITICAL_SECTION lock;
  WAVEHDR *head, *tail;      /* queued, in order; the head is the one being filled */
  BOOL recording;
  ULONG64 recorded;          /* bytes put into headers since the last reset */
  volatile LONG stop;
  HANDLE thread, wake;
} WaveIn;

static WaveIn *g_waveins[MAX_WAVEINS];
static SRWLOCK g_waveins_lock = SRWLOCK_INIT;

/* The microphone's devices: 1 when there is one, else 0 (the virtual sources then start at 0). */
static UINT wavein_microphones(void) {
  ULONG64 inputs = 0, outputs = 0;
  return audio_call(TCOS_AUDIO_DEVICES, 0, 0, 0, 0, 0, 0, NULL, &inputs, &outputs) == 0 && inputs ? WAVE_DEVICES : 0;
}
static UINT wavein_devices(void) {
  ULONG64 virtuals = 0;
  tcos_audio_virtual_source(0, NULL, 0, &virtuals);
  return wavein_microphones() + (UINT)(virtuals < TCOS_AUDIO_MAX_VIRTUAL ? virtuals : TCOS_AUDIO_MAX_VIRTUAL);
}
/* A device number's virtual source (its node; 0: the microphone), and its name; FALSE for a number with no device. */
static BOOL wavein_device_node(UINT id, ULONG64 *node, char *name, int size) {
  *node = 0;
  const UINT microphones = wavein_microphones();
  if (id == WAVE_MAPPER) id = 0;
  if (id < microphones) return TRUE;
  ULONG64 count = 0;
  *node = tcos_audio_virtual_source(id - microphones, name, size, &count);
  return *node != 0;
}

static WaveIn *wavein_lookup(HWAVEIN h) {
  WaveIn *found = NULL;
  AcquireSRWLockShared(&g_waveins_lock);
  for (int i = 0; i < MAX_WAVEINS; ++i)
    if (g_waveins[i] && g_waveins[i]->self == h) found = g_waveins[i];
  ReleaseSRWLockShared(&g_waveins_lock);
  return found;
}

static void wavein_notify(WaveIn *w, UINT message, DWORD_PTR param) {
  switch (w->callback_kind) {
    case CALLBACK_FUNCTION:
      if (w->callback) ((LPDRVCALLBACK)w->callback)((HDRVR)w->self, message, w->instance, param, 0);
      break;
    case CALLBACK_EVENT:
      if (w->callback) SetEvent((HANDLE)w->callback);
      break;
    case CALLBACK_WINDOW: case CALLBACK_THREAD: {
      HMODULE user32 = GetModuleHandleA("user32.dll");
      if (!user32 || !w->callback) break;
      const UINT mm = message == WIM_OPEN ? MM_WIM_OPEN : message == WIM_CLOSE ? MM_WIM_CLOSE : MM_WIM_DATA;
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

/* Takes the head header out of the queue as done (the lock held), appending it to *done_tail. */
static void wavein_retire_head_locked(WaveIn *w, WAVEHDR ***done_tail) {
  WAVEHDR *h = w->head;
  w->head = h->lpNext;
  if (!w->head) w->tail = NULL;
  h->dwFlags = (h->dwFlags & ~WHDR_INQUEUE) | WHDR_DONE;
  h->lpNext = NULL;
  **done_tail = h;
  *done_tail = &h->lpNext;
}

/* Reads what the kernel has into the queued headers; the full ones are returned as a list (lpNext) for the callbacks. */
static WAVEHDR *wavein_step(WaveIn *w) {
  WAVEHDR *done = NULL, **done_tail = &done;
  EnterCriticalSection(&w->lock);
  while (w->recording && w->head) {
    WAVEHDR *h = w->head;
    const DWORD room = h->dwBufferLength - h->dwBytesRecorded;
    ULONG64 got = 0;
    if (room && audio_call(TCOS_AUDIO_READ, w->stream, (ULONG64)(ULONG_PTR)(h->lpData + h->dwBytesRecorded), room, 0, 0, 0, NULL, NULL, &got) != 0) break;
    h->dwBytesRecorded += (DWORD)got;
    w->recorded += got;
    if (h->dwBytesRecorded < h->dwBufferLength) break;      /* nothing more yet: the rest next time */
    wavein_retire_head_locked(w, &done_tail);
  }
  LeaveCriticalSection(&w->lock);
  return done;
}

static void wavein_deliver(WaveIn *w, WAVEHDR *done) {
  while (done) {
    WAVEHDR *next = done->lpNext;
    done->lpNext = NULL;
    wavein_notify(w, WIM_DATA, (DWORD_PTR)done);
    done = next;
  }
}

static DWORD WINAPI wavein_worker(LPVOID context) {
  WaveIn *w = (WaveIn *)context;
  while (!w->stop) {
    wavein_deliver(w, wavein_step(w));
    WaitForSingleObject(w->wake, WORKER_PERIOD_MS);
  }
  return 0;
}

EXPORT MMRESULT WINAPI waveInOpen(LPHWAVEIN out, UINT id, LPCWAVEFORMATEX format, DWORD_PTR callback, DWORD_PTR instance, DWORD flags) {
  if (out) *out = NULL;
  if (!format) return MMSYSERR_INVALPARAM;
  if (!wavein_devices()) return id == WAVE_MAPPER ? MMSYSERR_NODRIVER : MMSYSERR_BADDEVICEID;
  /* (WAVE_MAPPED: the device the id names, through the mapper - which changes nothing here) */
  ULONG64 node = 0;
  if (!wavein_device_node(id, &node, NULL, 0)) return MMSYSERR_BADDEVICEID;
  const ULONG64 code = wave_format_code(format);
  if (!code) return WAVERR_BADFORMAT;
  if (flags & WAVE_FORMAT_QUERY) return MMSYSERR_NOERROR;
  if (!out) return MMSYSERR_INVALPARAM;
  WaveIn *w = (WaveIn *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof *w);
  if (!w) return MMSYSERR_NOMEM;
  char name[128];
  program_name(name, sizeof name);
  ULONG64 stream = 0;
  /* (OPEN_CAPTURE [1]: 0 the microphone, a virtual source's node) */
  LONG st = audio_call(TCOS_AUDIO_OPEN_CAPTURE, node, code, format->nSamplesPerSec, format->nChannels, (ULONG64)(ULONG_PTR)name, lstrlenA(name), NULL, NULL, &stream);
  if (st != 0) {
    HeapFree(GetProcessHeap(), 0, w);
    return st == TCOS_STATUS_INVALID_PARAMETER ? WAVERR_BADFORMAT : MMSYSERR_ALLOCATED;
  }
  audio_call(TCOS_AUDIO_PAUSE, stream, 1, 0, 0, 0, 0, NULL, NULL, NULL);      /* (it records from waveInStart) */
  w->stream = stream;
  w->device = id == WAVE_MAPPER ? 0 : id;
  w->format = *format;
  w->format.cbSize = 0;
  w->frame = format->nBlockAlign;
  w->callback = callback;
  w->instance = instance;
  w->callback_kind = flags & CALLBACK_TYPEMASK;
  InitializeCriticalSection(&w->lock);
  w->wake = CreateEventA(NULL, FALSE, FALSE, NULL);
  int slot = -1;
  AcquireSRWLockExclusive(&g_waveins_lock);
  for (int i = 0; i < MAX_WAVEINS && slot < 0; ++i) if (!g_waveins[i]) slot = i;
  if (slot >= 0) {
    static LONG counter;
    w->self = (HWAVEIN)(ULONG_PTR)(0x7B000000u + (ULONG)InterlockedIncrement(&counter) * 0x10u + (ULONG)slot);
    g_waveins[slot] = w;
  }
  ReleaseSRWLockExclusive(&g_waveins_lock);
  if (slot < 0 || !w->wake) {
    if (slot >= 0) { AcquireSRWLockExclusive(&g_waveins_lock); g_waveins[slot] = NULL; ReleaseSRWLockExclusive(&g_waveins_lock); }
    audio_call(TCOS_AUDIO_CLOSE, stream, 0, 0, 0, 0, 0, NULL, NULL, NULL);
    if (w->wake) CloseHandle(w->wake);
    DeleteCriticalSection(&w->lock);
    HeapFree(GetProcessHeap(), 0, w);
    return MMSYSERR_ALLOCATED;
  }
  w->thread = CreateThread(NULL, 0, wavein_worker, w, 0, NULL);
  if (w->thread) SetThreadPriority(w->thread, THREAD_PRIORITY_TIME_CRITICAL);
  *out = w->self;
  wavein_notify(w, WIM_OPEN, 0);
  return MMSYSERR_NOERROR;
}

EXPORT MMRESULT WINAPI waveInClose(HWAVEIN h) {
  WaveIn *w = wavein_lookup(h);
  if (!w) return MMSYSERR_INVALHANDLE;
  EnterCriticalSection(&w->lock);
  const BOOL busy = w->head != NULL;
  LeaveCriticalSection(&w->lock);
  if (busy) return WAVERR_STILLPLAYING;      /* (headers still queued: waveInReset first) */
  AcquireSRWLockExclusive(&g_waveins_lock);
  for (int i = 0; i < MAX_WAVEINS; ++i) if (g_waveins[i] == w) g_waveins[i] = NULL;
  ReleaseSRWLockExclusive(&g_waveins_lock);
  InterlockedExchange(&w->stop, 1);
  SetEvent(w->wake);
  if (w->thread) {
    if (GetThreadId(w->thread) != GetCurrentThreadId()) WaitForSingleObject(w->thread, INFINITE);
    CloseHandle(w->thread);
  }
  audio_call(TCOS_AUDIO_CLOSE, w->stream, 0, 0, 0, 0, 0, NULL, NULL, NULL);
  wavein_notify(w, WIM_CLOSE, 0);
  CloseHandle(w->wake);
  DeleteCriticalSection(&w->lock);
  HeapFree(GetProcessHeap(), 0, w);
  return MMSYSERR_NOERROR;
}

EXPORT MMRESULT WINAPI waveInPrepareHeader(HWAVEIN h, LPWAVEHDR header, UINT size) {
  if (!wavein_lookup(h)) return MMSYSERR_INVALHANDLE;
  if (!header || size < sizeof(WAVEHDR)) return MMSYSERR_INVALPARAM;
  if (header->dwFlags & WHDR_INQUEUE) return WAVERR_STILLPLAYING;
  header->dwFlags |= WHDR_PREPARED;
  header->dwFlags &= ~WHDR_DONE;
  return MMSYSERR_NOERROR;
}

EXPORT MMRESULT WINAPI waveInUnprepareHeader(HWAVEIN h, LPWAVEHDR header, UINT size) {
  if (!wavein_lookup(h)) return MMSYSERR_INVALHANDLE;
  if (!header || size < sizeof(WAVEHDR)) return MMSYSERR_INVALPARAM;
  if (header->dwFlags & WHDR_INQUEUE) return WAVERR_STILLPLAYING;
  header->dwFlags &= ~WHDR_PREPARED;
  header->dwFlags |= WHDR_DONE;
  return MMSYSERR_NOERROR;
}

EXPORT MMRESULT WINAPI waveInAddBuffer(HWAVEIN h, LPWAVEHDR header, UINT size) {
  WaveIn *w = wavein_lookup(h);
  if (!w) return MMSYSERR_INVALHANDLE;
  if (!header || size < sizeof(WAVEHDR) || !header->lpData || !header->dwBufferLength) return MMSYSERR_INVALPARAM;
  if (!(header->dwFlags & WHDR_PREPARED)) return WAVERR_UNPREPARED;
  if (header->dwFlags & WHDR_INQUEUE) return WAVERR_STILLPLAYING;
  header->dwBufferLength -= header->dwBufferLength % (w->frame ? w->frame : 1);      /* (whole frames only) */
  if (!header->dwBufferLength) return MMSYSERR_INVALPARAM;
  EnterCriticalSection(&w->lock);
  header->dwFlags = (header->dwFlags & ~WHDR_DONE) | WHDR_INQUEUE;
  header->dwBytesRecorded = 0;
  header->lpNext = NULL;
  if (w->tail) w->tail->lpNext = header;
  else w->head = header;
  w->tail = header;
  LeaveCriticalSection(&w->lock);
  SetEvent(w->wake);
  return MMSYSERR_NOERROR;
}

EXPORT MMRESULT WINAPI waveInStart(HWAVEIN h) {
  WaveIn *w = wavein_lookup(h);
  if (!w) return MMSYSERR_INVALHANDLE;
  EnterCriticalSection(&w->lock);
  if (!w->recording) {
    audio_call(TCOS_AUDIO_PAUSE, w->stream, 0, 0, 0, 0, 0, NULL, NULL, NULL);      /* (the kernel starts with sound from now) */
    w->recording = TRUE;
  }
  LeaveCriticalSection(&w->lock);
  SetEvent(w->wake);
  return MMSYSERR_NOERROR;
}

/* Stops recording (`reset`: and returns every queued header, the position starting at 0 again). The header being filled is returned with what it holds. */
static MMRESULT wavein_stop(HWAVEIN h, BOOL reset) {
  WaveIn *w = wavein_lookup(h);
  if (!w) return MMSYSERR_INVALHANDLE;
  /* what was recorded up to now goes into the headers first */
  WAVEHDR *done = wavein_step(w), **done_tail = &done;
  while (*done_tail) done_tail = &(*done_tail)->lpNext;
  EnterCriticalSection(&w->lock);
  if (w->recording) {
    audio_call(TCOS_AUDIO_PAUSE, w->stream, 1, 0, 0, 0, 0, NULL, NULL, NULL);
    w->recording = FALSE;
  }
  if (w->head && (reset || w->head->dwBytesRecorded)) wavein_retire_head_locked(w, &done_tail);
  while (reset && w->head) wavein_retire_head_locked(w, &done_tail);
  if (reset) {
    audio_call(TCOS_AUDIO_FLUSH, w->stream, 0, 0, 0, 0, 0, NULL, NULL, NULL);
    w->recorded = 0;
  }
  LeaveCriticalSection(&w->lock);
  wavein_deliver(w, done);
  return MMSYSERR_NOERROR;
}

EXPORT MMRESULT WINAPI waveInStop(HWAVEIN h) { return wavein_stop(h, FALSE); }
EXPORT MMRESULT WINAPI waveInReset(HWAVEIN h) { return wavein_stop(h, TRUE); }

EXPORT MMRESULT WINAPI waveInGetPosition(HWAVEIN h, LPMMTIME time, UINT size) {
  WaveIn *w = wavein_lookup(h);
  if (!w) return MMSYSERR_INVALHANDLE;
  if (!time || size < sizeof(MMTIME)) return MMSYSERR_INVALPARAM;
  EnterCriticalSection(&w->lock);
  const ULONG64 bytes = w->recorded;
  LeaveCriticalSection(&w->lock);
  const ULONG64 samples = w->frame ? bytes / w->frame : 0;
  switch (time->wType) {
    case TIME_SAMPLES: time->u.sample = (DWORD)samples; break;
    case TIME_MS: time->u.ms = (DWORD)(samples * 1000 / w->format.nSamplesPerSec); break;
    default: time->wType = TIME_BYTES; time->u.cb = (DWORD)bytes; break;
  }
  return MMSYSERR_NOERROR;
}

EXPORT MMRESULT WINAPI waveInGetID(HWAVEIN h, LPUINT id) {
  WaveIn *w = wavein_lookup(h);
  if (!w) return MMSYSERR_INVALHANDLE;
  if (!id) return MMSYSERR_INVALPARAM;
  *id = w->device;
  return MMSYSERR_NOERROR;
}

EXPORT MMRESULT WINAPI waveInMessage(HWAVEIN h, UINT message, DWORD_PTR a, DWORD_PTR b) { (void)h; (void)message; (void)a; (void)b; return MMSYSERR_NOTSUPPORTED; }

static void fill_input_caps_name(WCHAR *w, CHAR *a, UINT id) {
  char described[MAXPNAMELEN];
  ULONG64 node = 0;
  const char *name = id == WAVE_MAPPER ? "Microsoft Sound Mapper" : "TheoryCraftOS Microphone";
  if (id != WAVE_MAPPER && wavein_device_node(id, &node, described, sizeof described) && node) name = described;      /* (a virtual source: its description) */
  int i = 0;
  for (; name[i] && i < MAXPNAMELEN - 1; ++i) { if (w) w[i] = (WCHAR)name[i]; if (a) a[i] = name[i]; }
  if (w) w[i] = 0;
  if (a) a[i] = 0;
}

/* The device number a capabilities query names (a handle stands for its device); FALSE when there is no such device. */
static BOOL wavein_caps_id(UINT_PTR *id) {
  const UINT devices = wavein_devices();
  if (!devices) return FALSE;
  if ((UINT)*id == WAVE_MAPPER || *id < devices) return TRUE;
  WaveIn *w = wavein_lookup((HWAVEIN)*id);
  if (!w) return FALSE;
  *id = w->device;
  return TRUE;
}

EXPORT MMRESULT WINAPI waveInGetDevCapsA(UINT_PTR id, LPWAVEINCAPSA caps, UINT size) {
  if (!caps || size < sizeof *caps) return MMSYSERR_INVALPARAM;
  if (!wavein_caps_id(&id)) return (UINT)id == WAVE_MAPPER ? MMSYSERR_NODRIVER : MMSYSERR_BADDEVICEID;
  caps->wMid = MM_MICROSOFT;
  caps->wPid = (UINT)id == WAVE_MAPPER ? TCOS_PID_MAPPER : TCOS_PID_WAVEIN;
  caps->vDriverVersion = 0x0100;
  fill_input_caps_name(NULL, caps->szPname, (UINT)id);
  caps->dwFormats = WAVE_ALL_FORMATS;
  caps->wChannels = 2;
  caps->wReserved1 = 0;
  return MMSYSERR_NOERROR;
}

EXPORT MMRESULT WINAPI waveInGetDevCapsW(UINT_PTR id, LPWAVEINCAPSW caps, UINT size) {
  if (!caps || size < sizeof *caps) return MMSYSERR_INVALPARAM;
  if (!wavein_caps_id(&id)) return (UINT)id == WAVE_MAPPER ? MMSYSERR_NODRIVER : MMSYSERR_BADDEVICEID;
  caps->wMid = MM_MICROSOFT;
  caps->wPid = (UINT)id == WAVE_MAPPER ? TCOS_PID_MAPPER : TCOS_PID_WAVEIN;
  caps->vDriverVersion = 0x0100;
  fill_input_caps_name(caps->szPname, NULL, (UINT)id);
  caps->dwFormats = WAVE_ALL_FORMATS;
  caps->wChannels = 2;
  caps->wReserved1 = 0;
  return MMSYSERR_NOERROR;
}

EXPORT UINT WINAPI waveInGetNumDevs(void) { return wavein_devices(); }

EXPORT MMRESULT WINAPI waveInGetErrorTextA(MMRESULT error, LPSTR text, UINT size) { return waveOutGetErrorTextA(error, text, size); }
EXPORT MMRESULT WINAPI waveInGetErrorTextW(MMRESULT error, LPWSTR text, UINT size) { return waveOutGetErrorTextW(error, text, size); }
