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

/* winmm.dll for TheoryCraftOS: the multimedia timer and the answers of a machine with no wave, MIDI, mixer or joystick device behind winmm.
 *   time*      timeGetTime (milliseconds since boot), timeGetDevCaps (1 .. 65535 ms), timeBegin/EndPeriod (accepted: the scheduler's sleeps are not tick-bound), and
 *              timeSetEvent / timeKillEvent over kernel32's timer queues: at most 16 events at once, ids that are not reused right away, a one-shot event is gone once it fired.
 *   devices    the open functions of wave, MIDI and mixer devices answer that there is none (a stub answered MMSYSERR_NOERROR and left the handle unset); the joystick functions
 *              answer for 16 empty slots (JOYERR_PARMS / JOYERR_UNPLUGGED).
 * Not here (still stubs): PlaySound and the MCI, mmio, ACM and driver functions. Built freestanding over kernel32.dll and ntdll.dll. */
#define WIN32_LEAN_AND_MEAN
#define _WINMM_      /* (the definitions: mmsystem.h declares the functions without dllimport) */
#include <windows.h>
#include <mmsystem.h>

#define EXPORT __declspec(dllexport)

__asm__(".text\n.globl ___chkstk_ms\n___chkstk_ms:\n ret\n");

/* ---- time --------------------------------------------------------------------------------------------------------------------------------------------------------- */

#define PERIOD_MIN 1
#define PERIOD_MAX 65535

EXPORT DWORD WINAPI timeGetTime(void) { return (DWORD)GetTickCount64(); }

EXPORT MMRESULT WINAPI timeGetSystemTime(LPMMTIME time, UINT size) {
  if (!time) return MMSYSERR_INVALPARAM;
  if (size >= sizeof *time) {
    time->wType = TIME_MS;
    time->u.ms = timeGetTime();
  }
  return TIMERR_NOERROR;
}

EXPORT MMRESULT WINAPI timeGetDevCaps(LPTIMECAPS caps, UINT size) {
  if (!caps) return TIMERR_NOCANDO;
  if (size < sizeof *caps) return TIMERR_NOCANDO;
  caps->wPeriodMin = PERIOD_MIN;
  caps->wPeriodMax = PERIOD_MAX;
  return TIMERR_NOERROR;
}

EXPORT MMRESULT WINAPI timeBeginPeriod(UINT period) { return period < PERIOD_MIN || period > PERIOD_MAX ? TIMERR_NOCANDO : TIMERR_NOERROR; }
EXPORT MMRESULT WINAPI timeEndPeriod(UINT period) { return period < PERIOD_MIN || period > PERIOD_MAX ? TIMERR_NOCANDO : TIMERR_NOERROR; }

/* timeSetEvent: a slot per event, each a timer of kernel32's timer queue whose callback calls the program (or sets / pulses its event). The lock covers the table; the
 * program's callback runs outside it, so it may kill its own event (or any other). */
#define MAX_EVENTS 16
#define TIME_EVENT_KIND_ (TIME_CALLBACK_EVENT_SET | TIME_CALLBACK_EVENT_PULSE)
typedef struct {
  UINT id;                 /* 0: free */
  HANDLE timer;
  LPTIMECALLBACK callback;
  DWORD_PTR user;
  UINT flags;
  DWORD running_thread;    /* the thread of a callback that is running, else 0 */
} TimeEvent;
static TimeEvent g_events[MAX_EVENTS];
static SRWLOCK g_events_lock = SRWLOCK_INIT;
static UINT g_next_id = 1;

/* The context of a slot's timer is the slot and the id it was made for: a callback that comes after the event was killed (and the slot taken again) finds another id. */
static PVOID event_context(UINT slot, UINT id) { return (PVOID)((ULONG_PTR)id << 32 | slot); }

static void CALLBACK event_fired(PVOID context, BOOLEAN timer_fired) {
  (void)timer_fired;
  const UINT slot = (UINT)((ULONG_PTR)context & 0xFFFFFFFFu), id = (UINT)((ULONG_PTR)context >> 32);
  if (slot >= MAX_EVENTS) return;
  TimeEvent *e = &g_events[slot];
  AcquireSRWLockExclusive(&g_events_lock);
  if (e->id != id) { ReleaseSRWLockExclusive(&g_events_lock); return; }      /* killed before this callback got the lock */
  /* The timer queue may start the next period's callback while one still runs; winmm calls an event's callbacks one after another, so that tick is dropped (as Windows
   * does), which also keeps running_thread exact for timeKillEvent. */
  if (e->running_thread) { ReleaseSRWLockExclusive(&g_events_lock); return; }
  LPTIMECALLBACK callback = e->callback;
  const DWORD_PTR user = e->user;
  const UINT flags = e->flags;
  if (!(flags & TIME_PERIODIC) && (flags & TIME_EVENT_KIND_)) {      /* a one-shot event that sets an event is over before it is set: the woken thread finds its id gone */
    HANDLE timer = e->timer;
    e->id = 0;
    e->timer = NULL;
    ReleaseSRWLockExclusive(&g_events_lock);
    if (flags & TIME_CALLBACK_EVENT_SET) SetEvent((HANDLE)callback); else PulseEvent((HANDLE)callback);
    DeleteTimerQueueTimer(NULL, timer, NULL);      /* (does not wait: this is the timer's own callback) */
    return;
  }
  e->running_thread = GetCurrentThreadId();
  ReleaseSRWLockExclusive(&g_events_lock);
  /* (Windows runs the callbacks on a time-critical thread; this one is the timer queue's, so its priority is given back afterwards) */
  const int priority = GetThreadPriority(GetCurrentThread());
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
  if (flags & TIME_CALLBACK_EVENT_SET) SetEvent((HANDLE)callback);
  else if (flags & TIME_CALLBACK_EVENT_PULSE) PulseEvent((HANDLE)callback);
  else callback(id, 0, user, 0, 0);
  SetThreadPriority(GetCurrentThread(), priority);
  AcquireSRWLockExclusive(&g_events_lock);
  if (e->id != id) { ReleaseSRWLockExclusive(&g_events_lock); return; }      /* the callback killed its own event */
  e->running_thread = 0;
  if (flags & TIME_PERIODIC) { ReleaseSRWLockExclusive(&g_events_lock); return; }
  HANDLE timer = e->timer;      /* a one-shot event is over: its id is no longer valid */
  e->id = 0;
  e->timer = NULL;
  ReleaseSRWLockExclusive(&g_events_lock);
  DeleteTimerQueueTimer(NULL, timer, NULL);      /* (does not wait: this is the timer's own callback; the timer goes when it returns) */
}

EXPORT MMRESULT WINAPI timeSetEvent(UINT delay, UINT resolution, LPTIMECALLBACK callback, DWORD_PTR user, UINT flags) {
  (void)resolution;
  if (!callback || delay < PERIOD_MIN || delay > PERIOD_MAX) return 0;
  if ((flags & TIME_EVENT_KIND_) == TIME_EVENT_KIND_) return 0;
  AcquireSRWLockExclusive(&g_events_lock);
  UINT slot = 0;
  while (slot < MAX_EVENTS && g_events[slot].id) ++slot;
  if (slot == MAX_EVENTS) { ReleaseSRWLockExclusive(&g_events_lock); return 0; }
  TimeEvent *e = &g_events[slot];
  const UINT id = g_next_id;
  g_next_id = g_next_id == 0xFFFFFFFFu ? 1 : g_next_id + 1;
  e->id = id;
  e->callback = callback;
  e->user = user;
  e->flags = flags;
  e->running_thread = 0;
  /* (the slot is held by the lock until the timer exists: a callback that comes at once waits for it) */
  HANDLE timer = NULL;
  if (!CreateTimerQueueTimer(&timer, NULL, event_fired, event_context(slot, id), delay, (flags & TIME_PERIODIC) ? delay : 0, WT_EXECUTEDEFAULT)) {
    e->id = 0;
    ReleaseSRWLockExclusive(&g_events_lock);
    return 0;
  }
  e->timer = timer;
  ReleaseSRWLockExclusive(&g_events_lock);
  return id;
}

EXPORT MMRESULT WINAPI timeKillEvent(UINT id) {
  if (!id) return TIMERR_NOCANDO;
  AcquireSRWLockExclusive(&g_events_lock);
  TimeEvent *e = NULL;
  for (UINT i = 0; i < MAX_EVENTS; ++i) if (g_events[i].id == id) { e = &g_events[i]; break; }
  if (!e) { ReleaseSRWLockExclusive(&g_events_lock); return TIMERR_NOCANDO; }
  HANDLE timer = e->timer;
  /* TIME_KILL_SYNCHRONOUS: the call returns once no callback of the event runs, except from that callback itself, which cannot wait for its own end */
  const BOOL wait = (e->flags & TIME_KILL_SYNCHRONOUS) && e->running_thread != GetCurrentThreadId();
  e->id = 0;
  e->timer = NULL;
  ReleaseSRWLockExclusive(&g_events_lock);
  if (timer) DeleteTimerQueueTimer(NULL, timer, wait ? INVALID_HANDLE_VALUE : NULL);
  return TIMERR_NOERROR;
}

/* ---- joysticks ------------------------------------------------------------------------------------------------------------------------------------------------------ *
 * joyGetNumDevs answers how many slots there are, not how many joysticks are attached; none is. */
#define JOYSTICK_SLOTS 16
static MMRESULT joystick_slot(UINT id) { return id < JOYSTICK_SLOTS ? JOYERR_PARMS : MMSYSERR_NODRIVER; }
EXPORT UINT WINAPI joyGetNumDevs(void) { return JOYSTICK_SLOTS; }
EXPORT MMRESULT WINAPI joyGetDevCapsA(UINT_PTR id, LPJOYCAPSA caps, UINT size) {
  if (!caps || size < sizeof *caps) return MMSYSERR_INVALPARAM;
  return joystick_slot((UINT)id);
}
EXPORT MMRESULT WINAPI joyGetDevCapsW(UINT_PTR id, LPJOYCAPSW caps, UINT size) {
  if (!caps || size < sizeof *caps) return MMSYSERR_INVALPARAM;
  return joystick_slot((UINT)id);
}
EXPORT MMRESULT WINAPI joyGetPos(UINT id, LPJOYINFO info) {
  if (!info) return MMSYSERR_INVALPARAM;
  return id < JOYSTICK_SLOTS ? JOYERR_UNPLUGGED : JOYERR_PARMS;
}
EXPORT MMRESULT WINAPI joyGetPosEx(UINT id, LPJOYINFOEX info) {
  if (!info) return MMSYSERR_INVALPARAM;
  return id < JOYSTICK_SLOTS ? JOYERR_UNPLUGGED : JOYERR_PARMS;
}
EXPORT MMRESULT WINAPI joyGetThreshold(UINT id, LPUINT threshold) {
  if (!threshold) return MMSYSERR_INVALPARAM;
  return joystick_slot(id);
}
EXPORT MMRESULT WINAPI joySetThreshold(UINT id, UINT threshold) { (void)threshold; return joystick_slot(id); }
EXPORT MMRESULT WINAPI joySetCapture(HWND window, UINT id, UINT period, BOOL changed) {
  (void)period; (void)changed;
  if (!window) return JOYERR_PARMS;
  return joystick_slot(id);
}
EXPORT MMRESULT WINAPI joyReleaseCapture(UINT id) { return joystick_slot(id); }

/* ---- wave, MIDI and mixer devices: there are none ---------------------------------------------------------------------------------------------------------------- *
 * The counts are 0 (the stubs' answer); the open functions clear the handle and answer that the device does not exist (the mapper, that there is no driver). */
#define MAPPER_ID ((UINT)-1)
static MMRESULT no_device(UINT id) { return id == MAPPER_ID ? MMSYSERR_NODRIVER : MMSYSERR_BADDEVICEID; }
EXPORT MMRESULT WINAPI waveOutOpen(LPHWAVEOUT out, UINT id, LPCWAVEFORMATEX format, DWORD_PTR callback, DWORD_PTR instance, DWORD flags) {
  (void)callback; (void)instance; (void)flags;
  if (out) *out = NULL;
  if (!format) return MMSYSERR_INVALPARAM;
  return no_device(id);
}
EXPORT MMRESULT WINAPI waveInOpen(LPHWAVEIN out, UINT id, LPCWAVEFORMATEX format, DWORD_PTR callback, DWORD_PTR instance, DWORD flags) {
  (void)callback; (void)instance; (void)flags;
  if (out) *out = NULL;
  if (!format) return MMSYSERR_INVALPARAM;
  return no_device(id);
}
EXPORT MMRESULT WINAPI midiOutOpen(LPHMIDIOUT out, UINT id, DWORD_PTR callback, DWORD_PTR instance, DWORD flags) {
  (void)callback; (void)instance; (void)flags;
  if (!out) return MMSYSERR_INVALPARAM;
  *out = NULL;
  return no_device(id);
}
EXPORT MMRESULT WINAPI midiInOpen(LPHMIDIIN out, UINT id, DWORD_PTR callback, DWORD_PTR instance, DWORD flags) {
  (void)callback; (void)instance; (void)flags;
  if (!out) return MMSYSERR_INVALPARAM;
  *out = NULL;
  return no_device(id);
}
EXPORT MMRESULT WINAPI midiStreamOpen(LPHMIDISTRM out, LPUINT id, DWORD count, DWORD_PTR callback, DWORD_PTR instance, DWORD flags) {
  (void)count; (void)callback; (void)instance; (void)flags;
  if (!out || !id) return MMSYSERR_INVALPARAM;
  *out = NULL;
  return no_device(*id);
}
EXPORT MMRESULT WINAPI mixerOpen(LPHMIXER out, UINT id, DWORD_PTR callback, DWORD_PTR instance, DWORD flags) {
  (void)callback; (void)instance; (void)flags;
  (void)id;
  if (out) *out = NULL;
  return MMSYSERR_BADDEVICEID;
}
EXPORT MMRESULT WINAPI waveOutGetDevCapsA(UINT_PTR id, LPWAVEOUTCAPSA caps, UINT size) { (void)size; return caps ? no_device((UINT)id) : MMSYSERR_INVALPARAM; }
EXPORT MMRESULT WINAPI waveOutGetDevCapsW(UINT_PTR id, LPWAVEOUTCAPSW caps, UINT size) { (void)size; return caps ? no_device((UINT)id) : MMSYSERR_INVALPARAM; }
EXPORT MMRESULT WINAPI waveInGetDevCapsA(UINT_PTR id, LPWAVEINCAPSA caps, UINT size) { (void)size; return caps ? no_device((UINT)id) : MMSYSERR_INVALPARAM; }
EXPORT MMRESULT WINAPI waveInGetDevCapsW(UINT_PTR id, LPWAVEINCAPSW caps, UINT size) { (void)size; return caps ? no_device((UINT)id) : MMSYSERR_INVALPARAM; }
EXPORT MMRESULT WINAPI midiOutGetDevCapsA(UINT_PTR id, LPMIDIOUTCAPSA caps, UINT size) { (void)size; return caps ? no_device((UINT)id) : MMSYSERR_INVALPARAM; }
EXPORT MMRESULT WINAPI midiOutGetDevCapsW(UINT_PTR id, LPMIDIOUTCAPSW caps, UINT size) { (void)size; return caps ? no_device((UINT)id) : MMSYSERR_INVALPARAM; }
EXPORT MMRESULT WINAPI midiInGetDevCapsA(UINT_PTR id, LPMIDIINCAPSA caps, UINT size) { (void)size; return caps ? no_device((UINT)id) : MMSYSERR_INVALPARAM; }
EXPORT MMRESULT WINAPI midiInGetDevCapsW(UINT_PTR id, LPMIDIINCAPSW caps, UINT size) { (void)size; return caps ? no_device((UINT)id) : MMSYSERR_INVALPARAM; }
EXPORT MMRESULT WINAPI mixerGetDevCapsA(UINT_PTR id, LPMIXERCAPSA caps, UINT size) { (void)id; (void)size; return caps ? MMSYSERR_BADDEVICEID : MMSYSERR_INVALPARAM; }
EXPORT MMRESULT WINAPI mixerGetDevCapsW(UINT_PTR id, LPMIXERCAPSW caps, UINT size) { (void)id; (void)size; return caps ? MMSYSERR_BADDEVICEID : MMSYSERR_INVALPARAM; }

/* The counts: no devices. The functions on an open device get a handle that no open function ever gave out: MMSYSERR_INVALHANDLE (a stub answered MMSYSERR_NOERROR, so
 * a program that wrote a buffer waited for a WOM_DONE that never came). */
EXPORT UINT WINAPI waveOutGetNumDevs(void) { return 0; }
EXPORT UINT WINAPI waveInGetNumDevs(void) { return 0; }
EXPORT UINT WINAPI midiOutGetNumDevs(void) { return 0; }
EXPORT UINT WINAPI midiInGetNumDevs(void) { return 0; }
EXPORT UINT WINAPI mixerGetNumDevs(void) { return 0; }
EXPORT UINT WINAPI auxGetNumDevs(void) { return 0; }
EXPORT MMRESULT WINAPI waveOutClose(HWAVEOUT h) { (void)h; return MMSYSERR_INVALHANDLE; }
EXPORT MMRESULT WINAPI waveOutPrepareHeader(HWAVEOUT h, LPWAVEHDR header, UINT size) { (void)h; (void)header; (void)size; return MMSYSERR_INVALHANDLE; }
EXPORT MMRESULT WINAPI waveOutUnprepareHeader(HWAVEOUT h, LPWAVEHDR header, UINT size) { (void)h; (void)header; (void)size; return MMSYSERR_INVALHANDLE; }
EXPORT MMRESULT WINAPI waveOutWrite(HWAVEOUT h, LPWAVEHDR header, UINT size) { (void)h; (void)header; (void)size; return MMSYSERR_INVALHANDLE; }
EXPORT MMRESULT WINAPI waveOutPause(HWAVEOUT h) { (void)h; return MMSYSERR_INVALHANDLE; }
EXPORT MMRESULT WINAPI waveOutRestart(HWAVEOUT h) { (void)h; return MMSYSERR_INVALHANDLE; }
EXPORT MMRESULT WINAPI waveOutReset(HWAVEOUT h) { (void)h; return MMSYSERR_INVALHANDLE; }
EXPORT MMRESULT WINAPI waveOutBreakLoop(HWAVEOUT h) { (void)h; return MMSYSERR_INVALHANDLE; }
EXPORT MMRESULT WINAPI waveOutGetPosition(HWAVEOUT h, LPMMTIME time, UINT size) { (void)h; (void)time; (void)size; return MMSYSERR_INVALHANDLE; }
EXPORT MMRESULT WINAPI waveInClose(HWAVEIN h) { (void)h; return MMSYSERR_INVALHANDLE; }
EXPORT MMRESULT WINAPI waveInPrepareHeader(HWAVEIN h, LPWAVEHDR header, UINT size) { (void)h; (void)header; (void)size; return MMSYSERR_INVALHANDLE; }
EXPORT MMRESULT WINAPI waveInUnprepareHeader(HWAVEIN h, LPWAVEHDR header, UINT size) { (void)h; (void)header; (void)size; return MMSYSERR_INVALHANDLE; }
EXPORT MMRESULT WINAPI waveInAddBuffer(HWAVEIN h, LPWAVEHDR header, UINT size) { (void)h; (void)header; (void)size; return MMSYSERR_INVALHANDLE; }
EXPORT MMRESULT WINAPI waveInStart(HWAVEIN h) { (void)h; return MMSYSERR_INVALHANDLE; }
EXPORT MMRESULT WINAPI waveInStop(HWAVEIN h) { (void)h; return MMSYSERR_INVALHANDLE; }
EXPORT MMRESULT WINAPI waveInReset(HWAVEIN h) { (void)h; return MMSYSERR_INVALHANDLE; }
EXPORT MMRESULT WINAPI midiOutClose(HMIDIOUT h) { (void)h; return MMSYSERR_INVALHANDLE; }
EXPORT MMRESULT WINAPI midiOutShortMsg(HMIDIOUT h, DWORD message) { (void)h; (void)message; return MMSYSERR_INVALHANDLE; }
EXPORT MMRESULT WINAPI midiOutLongMsg(HMIDIOUT h, LPMIDIHDR header, UINT size) { (void)h; (void)header; (void)size; return MMSYSERR_INVALHANDLE; }
EXPORT MMRESULT WINAPI midiOutPrepareHeader(HMIDIOUT h, LPMIDIHDR header, UINT size) { (void)h; (void)header; (void)size; return MMSYSERR_INVALHANDLE; }
EXPORT MMRESULT WINAPI midiOutUnprepareHeader(HMIDIOUT h, LPMIDIHDR header, UINT size) { (void)h; (void)header; (void)size; return MMSYSERR_INVALHANDLE; }
EXPORT MMRESULT WINAPI midiOutReset(HMIDIOUT h) { (void)h; return MMSYSERR_INVALHANDLE; }
EXPORT MMRESULT WINAPI midiInClose(HMIDIIN h) { (void)h; return MMSYSERR_INVALHANDLE; }
EXPORT MMRESULT WINAPI midiInPrepareHeader(HMIDIIN h, LPMIDIHDR header, UINT size) { (void)h; (void)header; (void)size; return MMSYSERR_INVALHANDLE; }
EXPORT MMRESULT WINAPI midiInUnprepareHeader(HMIDIIN h, LPMIDIHDR header, UINT size) { (void)h; (void)header; (void)size; return MMSYSERR_INVALHANDLE; }
EXPORT MMRESULT WINAPI midiInAddBuffer(HMIDIIN h, LPMIDIHDR header, UINT size) { (void)h; (void)header; (void)size; return MMSYSERR_INVALHANDLE; }
EXPORT MMRESULT WINAPI midiInStart(HMIDIIN h) { (void)h; return MMSYSERR_INVALHANDLE; }
EXPORT MMRESULT WINAPI midiInStop(HMIDIIN h) { (void)h; return MMSYSERR_INVALHANDLE; }
EXPORT MMRESULT WINAPI midiInReset(HMIDIIN h) { (void)h; return MMSYSERR_INVALHANDLE; }
EXPORT MMRESULT WINAPI mixerClose(HMIXER h) { (void)h; return MMSYSERR_INVALHANDLE; }

BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID reserved) {
  (void)module;
  /* events still set when the library is freed (not at process exit, when the threads are gone): their timers must not call into unloaded code */
  if (reason == DLL_PROCESS_DETACH && !reserved)
    for (UINT i = 0; i < MAX_EVENTS; ++i) if (g_events[i].id) timeKillEvent(g_events[i].id);
  return TRUE;
}
