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

/* dsound.dll for TheoryCraftOS: DirectSound 8 over the kernel's PipeWire graph (NtTcosAudio, src/Process/WindowsAudio.hpp, tcos_audio.h).
 *
 * As in Windows, DirectSound mixes in software: each IDirectSound8 object has one stream of the graph (16-bit stereo at 48 kHz, where the native,
 * Linux and other Windows programs' sound is mixed with it) and a thread that mixes its playing secondary buffers into it every 5 ms, about 30 ms
 * ahead of what the device plays. A buffer is read at its own frequency (linear interpolation, so SetFrequency changes pitch at once), with its
 * volume (hundredths of a dB) and pan, or with its 3D position against the listener: distance with the rolloff factor and min/max distances, the
 * cone, an equal-power pan from the direction and the Doppler shift from both velocities (no HRTF). The primary buffer's volume and pan apply to the whole mix; with
 * DSSCL_WRITEPRIMARY the program writes the primary buffer itself, in its own format.
 *
 * The play cursor is where the device is (what the kernel says it has played), the write cursor where the mixer has read up to; position
 * notifications fire as the play cursor passes them. Buffers made with DuplicateSoundBuffer share their memory. Capture (DirectSoundCapture8, its
 * buffers, full duplex) records from the graph's source: capture.inc. Not done: effects (SetFX: DSERR_FXUNAVAILABLE, no capture effects),
 * property sets. The classes have a factory (DllGetClassObject; ole32 finds this library for CLSID_DirectSound(8), CLSID_DirectSoundCapture(8) and
 * CLSID_DirectSoundFullDuplex). Built freestanding over
 * kernel32.dll and ntdll.dll. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>
#include <initguid.h>
#include <dsound.h>
#include "../tcos_audio.h"

#define EXPORT __declspec(dllexport)

__asm__(".text\n.globl ___chkstk_ms\n___chkstk_ms:\n ret\n");

/* (the GUIDs dsound.h does not define: this library is not linked with uuid) */
static const GUID kIidUnknown = {0x00000000, 0x0000, 0x0000, {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
static const GUID kGuidNull = {0, 0, 0, {0, 0, 0, 0, 0, 0, 0, 0}};
static const GUID kIidClassFactory = {0x00000001, 0x0000, 0x0000, {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
/* The one output device: the graph's sink. */
static const GUID kDeviceGuid = {0x5AF3A3C2, 0x6E1B, 0x4B4E, {0x8D, 0x2F, 0x1C, 0x0A, 0x7E, 0x5D, 0x9B, 0x30}};

static BOOL same_guid(const GUID *a, const GUID *b) {
  const BYTE *x = (const BYTE *)a, *y = (const BYTE *)b;
  for (SIZE_T i = 0; i < sizeof(GUID); ++i) if (x[i] != y[i]) return FALSE;
  return TRUE;
}
static void zero(void *p, SIZE_T n) {
  volatile BYTE *b = (volatile BYTE *)p;
  for (SIZE_T i = 0; i < n; ++i) b[i] = 0;
}
static void copy(void *to, const void *from, SIZE_T n) {
  BYTE *t = (BYTE *)to;
  const BYTE *f = (const BYTE *)from;
  for (SIZE_T i = 0; i < n; ++i) t[i] = f[i];
}
static void *alloc(SIZE_T n) { return HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, n); }
static void release_memory(void *p) { if (p) HeapFree(GetProcessHeap(), 0, p); }

#define OUT_RATE 48000
#define CHUNK_FRAMES 240            /* 5 ms mixed at a time */
#define AHEAD_FRAMES 1440           /* mixed at least this far ahead of what the device has played (30 ms) */
#define MAX_AHEAD_FRAMES 7200       /* and at most this far (150 ms: the kernel's stream holds about 170 ms) */
#define TICK_MS 5
#define PRIMARY_FRAMES 9600         /* the primary buffer's size, 200 ms of its format */

/* ---- the objects ------------------------------------------------------------------------------------------------------------------------------- */

typedef struct Memory {      /* a buffer's samples, shared with its duplicates */
  LONG refs;
  DWORD size;
  BYTE *data;
} Memory;

typedef struct Device Device;

typedef struct Buffer {
  IDirectSoundBuffer8 buffer;      /* (first: the object's pointer is this interface) */
  IDirectSoundNotify notify;
  IDirectSound3DBuffer ds3d;
  IDirectSound3DListener listener;      /* (the primary buffer's) */
  LONG refs;
  Device *device;
  struct Buffer *next;
  BOOL primary;
  DWORD flags;
  TcosWaveFormatExtensible format;      /* (Format.cbSize says how much of it is there) */
  WORD tag;                             /* PCM or float, through WAVE_FORMAT_EXTENSIBLE */
  Memory *memory;
  DWORD frames;                         /* whole frames in the memory */
  /* (the rest under the device's lock) */
  BOOL playing, looping, ending;        /* ending: a non-looping buffer mixed to its end, waiting for the device to play it out */
  ULONG64 position;                     /* the mixer's read position: frames << 32 | fraction */
  ULONG64 started_at;                   /* the output frame the buffer started at (since Play or SetCurrentPosition) */
  ULONG64 ended_at;                     /* the output frame its end was mixed at (ending) */
  DWORD frequency;
  LONG volume, pan;
  DWORD last_play;                      /* the play cursor (bytes) at the last tick, for notifications */
  DSBPOSITIONNOTIFY *notifies;
  DWORD notify_count;
  DS3DBUFFER params, deferred;          /* in effect, and with the deferred changes */
  BOOL deferred_dirty;
} Buffer;

struct Device {
  IDirectSound8 iface;
  LONG refs;                 /* the program's */
  LONG holds;                /* the program's (1 while it has any) and one for every buffer: the memory goes with the last */
  BOOL initialized;
  DWORD level, speaker_config;
  CRITICAL_SECTION lock;
  Buffer *buffers;           /* secondary buffers (and the primary buffer while it plays in DSSCL_WRITEPRIMARY) */
  Buffer *primary;
  WAVEFORMATEX primary_format;
  ULONG64 stream;            /* the graph's stream, opened when something first plays */
  ULONG64 target;            /* a virtual sink it plays into (its node, from the device GUID it was made for); 0: the speakers */
  ULONG64 produced;          /* output frames mixed (given to the kernel, or waiting in pending) */
  ULONG64 sent_bytes;        /* bytes the kernel took */
  ULONG64 played;            /* output frames the device has played, at the last tick */
  short pending[CHUNK_FRAMES * 2];
  DWORD pending_frames, pending_offset;
  DS3DLISTENER listener, listener_deferred;
  BOOL listener_dirty;
  HANDLE thread, wake;
  volatile LONG stop;
  ULONG64 last_tick_us;      /* when the mixer last ran, and how late it tends to be: it mixes ahead by more than that */
  ULONG64 tick_gap_frames;
  LONG mix[CHUNK_FRAMES * 2];      /* (16-bit sample scale, in 32 bits so that buffers can add up past it before the clamp) */
};

static const IDirectSoundBuffer8Vtbl kBufferVtbl;
static const IDirectSoundNotifyVtbl kNotifyVtbl;
static const IDirectSound3DBufferVtbl kBuffer3dVtbl;
static const IDirectSound3DListenerVtbl kListenerVtbl;
static const IDirectSound8Vtbl kDeviceVtbl;

static Buffer *from_buffer(void *i) { return (Buffer *)i; }
static Buffer *from_notify(IDirectSoundNotify *i) { return CONTAINING_RECORD(i, Buffer, notify); }
static Buffer *from_3d(IDirectSound3DBuffer *i) { return CONTAINING_RECORD(i, Buffer, ds3d); }
static Buffer *from_listener(IDirectSound3DListener *i) { return CONTAINING_RECORD(i, Buffer, listener); }
static Device *from_device(void *i) { return (Device *)i; }

static void device_hold(Device *d) { InterlockedIncrement(&d->holds); }
static void device_drop(Device *d) {
  if (InterlockedDecrement(&d->holds) != 0) return;
  DeleteCriticalSection(&d->lock);
  if (d->wake) CloseHandle(d->wake);
  release_memory(d);
}

/* ---- samples ------------------------------------------------------------------------------------------------------------------------------------ */

/* One sample of `b` as -1 .. 1. */
static float sample_at(const Buffer *b, const BYTE *p) {
  const WORD bits = b->format.Format.wBitsPerSample;
  if (b->tag == TCOS_WAVE_FORMAT_IEEE_FLOAT) {
    if (bits == 32) {
      float f;
      copy(&f, p, 4);
      return f;
    }
    double v;
    copy(&v, p, 8);
    return (float)v;
  }
  switch (bits) {
    case 8: return ((int)p[0] - 128) * (1.0f / 128);
    case 16: return (short)(p[0] | p[1] << 8) * (1.0f / 32768);
    case 24: return (float)((int)((ULONG)p[0] << 8 | (ULONG)p[1] << 16 | (ULONG)p[2] << 24) >> 8) * (1.0f / 8388608);
    default: return (float)(int)((ULONG)p[0] | (ULONG)p[1] << 8 | (ULONG)p[2] << 16 | (ULONG)p[3] << 24) * (1.0f / 2147483648.0f);
  }
}

/* Frame `index` of `b` as left and right (a centre channel goes to both, the others are left out). */
static void frame_at(const Buffer *b, DWORD index, float *left, float *right) {
  const WORD channels = b->format.Format.nChannels, step = b->format.Format.wBitsPerSample / 8;
  const BYTE *p = b->memory->data + (SIZE_T)index * b->format.Format.nBlockAlign;
  const float l = sample_at(b, p);
  if (channels == 1) {
    *left = *right = l;
    return;
  }
  float r = sample_at(b, p + step);
  float c = channels >= 3 ? sample_at(b, p + 2 * step) * 0.7071f : 0;
  *left = l + c;
  *right = r + c;
}

/* The memory of a 16-bit buffer as samples. (Safe: the memory comes from HeapAlloc, aligned for any type, and a 16-bit buffer's frames are whole
 * samples; the samples are only read while the buffer holds its reference to the memory.) */
static const short *buffer_samples16(const Buffer *b) { return (const short *)(const void *)b->memory->data; }

/* ---- gains -------------------------------------------------------------------------------------------------------------------------------------- */

typedef struct { double x, y, z; } Vec;
static Vec vec(D3DVECTOR v) { Vec r = {v.x, v.y, v.z}; return r; }
static double dot(Vec a, Vec b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static Vec cross(Vec a, Vec b) { Vec r = {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; return r; }
static Vec sub(Vec a, Vec b) { Vec r = {a.x - b.x, a.y - b.y, a.z - b.z}; return r; }
static double length(Vec a) { return tcos_sqrt(dot(a, a)); }

static double hundredths_db(LONG v) { return v <= DSBVOLUME_MIN ? 0 : tcos_db_to_gain(v / 100.0); }

/* The 3D part of a buffer's sound: its gain from distance and cone, and where it is from left (-1) to right (+1). */
static void gains_3d(const Device *d, const Buffer *b, double *gain, double *side) {
  const DS3DBUFFER *p = &b->params;
  const DS3DLISTENER *l = &d->listener;
  *gain = 1;
  *side = 0;
  if (p->dwMode == DS3DMODE_DISABLE) return;
  Vec rel, right;
  if (p->dwMode == DS3DMODE_HEADRELATIVE) {
    rel = vec(p->vPosition);
    Vec r = {1, 0, 0};
    right = r;
  } else {
    rel = sub(vec(p->vPosition), vec(l->vPosition));
    right = cross(vec(l->vOrientTop), vec(l->vOrientFront));      /* (left-handed: x right, y up, z ahead) */
  }
  const double distance = length(rel), right_length = length(right);
  double min = p->flMinDistance > 0 ? p->flMinDistance : DS3D_DEFAULTMINDISTANCE, max = p->flMaxDistance;
  if (max < min) max = min;
  if (distance > min) {
    const double d_clamped = distance < max ? distance : max;
    *gain = tcos_exp2(l->flRolloffFactor * tcos_log2(min / d_clamped));      /* (min / distance) ^ rolloff: 6 dB a doubling at rolloff 1 */
  }
  if (distance > 1e-6 && right_length > 1e-6) {
    *side = dot(rel, right) / (distance * right_length);
    if (*side > 1) *side = 1;
    if (*side < -1) *side = -1;
  }
  /* the cone: full inside the inside angle, the outside volume outside the outside angle, in between by the angle's cosine */
  if (p->dwInsideConeAngle < 360 || p->dwOutsideConeAngle < 360) {
    const Vec orient = vec(p->vConeOrientation);
    const double ol = length(orient);
    if (ol > 1e-6 && distance > 1e-6) {
      const Vec to_listener = {-rel.x, -rel.y, -rel.z};
      const double c = dot(orient, to_listener) / (ol * distance);
      const double pi = 3.14159265358979323846;
      const double ci = tcos_cos(p->dwInsideConeAngle * pi / 360), co = tcos_cos(p->dwOutsideConeAngle * pi / 360);
      const double outside = hundredths_db(p->lConeOutsideVolume);
      double g = 1;
      if (c >= ci) g = 1;
      else if (c <= co || ci - co < 1e-9) g = outside;
      else g = outside + (1 - outside) * (c - co) / (ci - co);
      *gain *= g;
    }
  }
}

/* The Doppler shift of a 3D buffer: how much faster (above 1) or slower its sound reaches the listener, from both velocities along the line between
 * them, the listener's Doppler factor and the speed of sound in the program's units (343.3 m/s over the distance factor, metres per unit). */
static double doppler_ratio(const Device *d, const Buffer *b) {
  const DS3DBUFFER *p = &b->params;
  const DS3DLISTENER *l = &d->listener;
  if (!(b->flags & DSBCAPS_CTRL3D) || b->primary || p->dwMode == DS3DMODE_DISABLE || !(l->flDopplerFactor > 0)) return 1;
  const BOOL head = p->dwMode == DS3DMODE_HEADRELATIVE;
  const Vec zero_v = {0, 0, 0};
  const Vec rel = head ? vec(p->vPosition) : sub(vec(p->vPosition), vec(l->vPosition));
  const double distance = length(rel);
  if (distance < 1e-6) return 1;
  const Vec u = {rel.x / distance, rel.y / distance, rel.z / distance};      /* from the listener to the source */
  const double c = 343.3 / (l->flDistanceFactor > 0 ? l->flDistanceFactor : 1);
  const double f = l->flDopplerFactor;
  double listener_v = dot(head ? zero_v : vec(l->vVelocity), u) * f;      /* towards the source: higher */
  double source_v = dot(vec(p->vVelocity), u) * f;                         /* away from the listener: lower */
  /* (no faster than sound, either way) */
  const double most = 0.9 * c;
  if (listener_v > most) listener_v = most;
  if (listener_v < -most) listener_v = -most;
  if (source_v > most) source_v = most;
  if (source_v < -most) source_v = -most;
  const double ratio = (c + listener_v) / (c + source_v);
  return ratio < 0.1 ? 0.1 : ratio > 10 ? 10 : ratio;
}

/* What `b` contributes to each side now. */
static void buffer_gains(const Device *d, const Buffer *b, float *left, float *right) {
  double g = hundredths_db(b->volume), l = 1, r = 1;
  if ((b->flags & DSBCAPS_CTRL3D) && !b->primary) {
    double gain3d, side;
    gains_3d(d, b, &gain3d, &side);
    g *= gain3d;
    /* equal power, unity at the centre */
    l = tcos_sqrt(1 - side);
    r = tcos_sqrt(1 + side);
    if (l > 1) l = 1;
    if (r > 1) r = 1;
  } else if (b->pan > 0) {
    l = hundredths_db(-b->pan);
  } else if (b->pan < 0) {
    r = hundredths_db(b->pan);
  }
  *left = (float)(g * l);
  *right = (float)(g * r);
}

/* ---- the mixer ---------------------------------------------------------------------------------------------------------------------------------- */

static DWORD bytes_of(const Buffer *b, DWORD frames) { return frames * b->format.Format.nBlockAlign; }

/* The buffer's play cursor (bytes), from where the device is. */
static DWORD play_cursor_locked(const Device *d, const Buffer *b) {
  if (b->frames == 0) return 0;
  const DWORD write_frame = (DWORD)(b->position >> 32);
  if (!b->playing) return bytes_of(b, write_frame < b->frames ? write_frame : 0);
  const double step = (double)b->frequency / OUT_RATE;
  if (b->ending) {
    const ULONG64 left = b->ended_at > d->played ? b->ended_at - d->played : 0;
    const double back = left * step;
    const DWORD f = back >= b->frames ? 0 : b->frames - 1 - (DWORD)back;
    return bytes_of(b, f);
  }
  ULONG64 lag = d->produced > d->played ? d->produced - d->played : 0;
  const ULONG64 since = d->produced - b->started_at;
  if (lag > since) lag = since;
  ULONG64 back = (ULONG64)(lag * step);
  if (back > write_frame && !b->looping) back = write_frame;
  back %= b->frames;
  const DWORD f = back > write_frame ? b->frames - (DWORD)(back - write_frame) : write_frame - (DWORD)back;
  return bytes_of(b, f % b->frames);
}

static void fire_stop_notifications(Buffer *b) {
  for (DWORD i = 0; i < b->notify_count; ++i)
    if (b->notifies[i].dwOffset == (DWORD)DSBPN_OFFSETSTOP && b->notifies[i].hEventNotify) SetEvent(b->notifies[i].hEventNotify);
}

/* Notifications whose offsets the play cursor passed going from `from` to `to` (bytes; it may have wrapped). */
static void fire_notifications(Buffer *b, DWORD from, DWORD to) {
  if (from == to) return;
  for (DWORD i = 0; i < b->notify_count; ++i) {
    const DWORD at = b->notifies[i].dwOffset;
    if (at == (DWORD)DSBPN_OFFSETSTOP || !b->notifies[i].hEventNotify) continue;
    const BOOL passed = from < to ? (at >= from && at < to) : (at >= from || at < to);
    if (passed) SetEvent(b->notifies[i].hEventNotify);
  }
}

/* A sample as 16-bit (float samples may be past full scale: clamped, which also keeps the fixed-point interpolation from overflowing). */
static LONG to_s16(float v) {
  const float x = v * 32767.0f;
  return x >= 32767.0f ? 32767 : x <= -32768.0f ? -32768 : (LONG)x;
}

static LONG to_q15(float g) {
  if (!(g > 0)) return 0;
  return g >= 1.0f ? 32768 : (LONG)(g * 32768.0f + 0.5f);
}

static ULONG64 step_of(const Device *d, const Buffer *b);

/* Mixes `n` output frames of `b` into d->mix, in fixed point: the read position is frames << 32 (its top 15 fraction bits interpolate), the gains
 * are Q15. */
static void mix_buffer(Device *d, Buffer *b, DWORD n) {
  if (b->frames == 0) {
    b->ending = TRUE;
    b->ended_at = d->produced;
    return;
  }
  float fl, fr;
  buffer_gains(d, b, &fl, &fr);
  const LONG gl = to_q15(fl), gr = to_q15(fr);
  const ULONG64 step = step_of(d, b);
  const ULONG64 end = (ULONG64)b->frames << 32;
  const BOOL fast = b->tag == 1 && b->format.Format.wBitsPerSample == 16 && b->format.Format.nChannels <= 2;
  const short *samples = fast ? buffer_samples16(b) : NULL;
  const DWORD channels = b->format.Format.nChannels, right = channels > 1 ? 1 : 0;
  for (DWORD i = 0; i < n; ++i) {
    if (b->position >= end) {
      if (b->looping) {
        b->position %= end;
      } else {
        b->ending = TRUE;
        b->ended_at = d->produced + i;
        b->position = end;
        return;
      }
    }
    const DWORD at = (DWORD)(b->position >> 32);
    DWORD next = at + 1;
    if (next >= b->frames) next = b->looping ? 0 : at;
    const LONG frac = (LONG)((b->position >> 17) & 0x7FFF);
    LONG l0, r0, l1, r1;
    if (fast) {
      /* (the common case, without the general sample reader: 16-bit mono or stereo) */
      const short *x = samples + (SIZE_T)at * channels, *y = samples + (SIZE_T)next * channels;
      l0 = x[0];
      r0 = x[right];
      l1 = y[0];
      r1 = y[right];
    } else {
      float a, c, e, g;
      frame_at(b, at, &a, &c);
      frame_at(b, next, &e, &g);
      l0 = to_s16(a);
      r0 = to_s16(c);
      l1 = to_s16(e);
      r1 = to_s16(g);
    }
    const LONG l = l0 + (((l1 - l0) * frac) >> 15), r = r0 + (((r1 - r0) * frac) >> 15);
    d->mix[2 * i] += (l * gl) >> 15;
    d->mix[2 * i + 1] += (r * gr) >> 15;
    b->position += step;
  }
}

/* A buffer's reading step now: frames << 32 per output frame, with its frequency and Doppler shift. */
static ULONG64 step_of(const Device *d, const Buffer *b) {
  const double doppler = doppler_ratio(d, b);
  return doppler == 1 ? ((ULONG64)b->frequency << 32) / OUT_RATE : (ULONG64)((double)b->frequency * doppler * 4294967296.0 / OUT_RATE);
}

/* Takes back what was mixed ahead and is not in the device yet (the kernel's stream and what waits here to be sent), so that a buffer that starts or
 * stops is heard doing so at once instead of after the 30 ms or more mixed ahead: every playing buffer steps back as many output frames and they are
 * mixed again at the next tick. (What the device holds, a few milliseconds, still plays, as in Windows.) */
static void rewind_locked(Device *d) {
  if (!d->stream) return;
  ULONG64 waiting = 0;
  if (tcos_audio_status(d->stream, &waiting, NULL, NULL, NULL) != 0) return;
  ULONG64 back = waiting / 4 + (d->pending_frames - d->pending_offset);
  if (back > d->produced) back = d->produced;
  if (back == 0) return;
  audio_call(TCOS_AUDIO_FLUSH, d->stream, 0, 0, 0, 0, 0, NULL, NULL, NULL);
  d->pending_frames = d->pending_offset = 0;
  const ULONG64 now = d->produced - back;
  for (Buffer *b = d->buffers; b; b = b->next) {
    if (!b->playing || b->frames == 0) continue;
    const ULONG64 step = step_of(d, b), end = (ULONG64)b->frames << 32;
    if (b->ending) {
      if (b->ended_at <= now) continue;      /* (its end is in the device already) */
      const ULONG64 frames = b->ended_at - now, delta = frames * step;
      b->ending = FALSE;
      b->position = delta < end ? end - delta : 0;
    } else {
      const ULONG64 since = d->produced > b->started_at ? d->produced - b->started_at : 0;
      ULONG64 delta = (back < since ? back : since) * step;
      if (b->looping) {
        delta %= end;
        b->position = b->position >= delta ? b->position - delta : b->position + end - delta;
      } else {
        b->position = b->position > delta ? b->position - delta : 0;
      }
    }
    if (b->started_at > now) b->started_at = now;
  }
  d->produced = now;
  if (d->played > d->produced) d->played = d->produced;
}

static BOOL open_stream_locked(Device *d) {
  if (d->stream) return TRUE;
  WAVEFORMATEX f = {WAVE_FORMAT_PCM, 2, OUT_RATE, OUT_RATE * 4, 4, 16, 0};
  d->stream = tcos_audio_open_to(&f, FALSE, FALSE, d->target, NULL);
  d->produced = d->sent_bytes = d->played = 0;
  d->pending_frames = d->pending_offset = 0;
  return d->stream != 0;
}

static void send_pending_locked(Device *d) {
  while (d->pending_offset < d->pending_frames) {
    ULONG64 taken = 0;
    const DWORD left = d->pending_frames - d->pending_offset;
    if (audio_call(TCOS_AUDIO_WRITE, d->stream, (ULONG64)(ULONG_PTR)&d->pending[d->pending_offset * 2], left * 4, 0, 0, 0, NULL, NULL, &taken) != 0) taken = 0;
    d->sent_bytes += taken;
    d->pending_offset += (DWORD)(taken / 4);
    if (taken < (ULONG64)left * 4) return;
  }
  d->pending_frames = d->pending_offset = 0;
}

static ULONG64 now_us(void) {
  LARGE_INTEGER t, f;
  QueryPerformanceCounter(&t);
  QueryPerformanceFrequency(&f);
  return f.QuadPart ? (ULONG64)((double)t.QuadPart * 1000000.0 / (double)f.QuadPart) : GetTickCount64() * 1000;
}

/* How far ahead to mix: two and a half times the longest recent gap between ticks (a busy or slow machine wakes the thread late), within limits. */
static ULONG64 ahead_frames_locked(Device *d) {
  const ULONG64 now = now_us();
  if (d->last_tick_us) {
    const ULONG64 gap = (now - d->last_tick_us) * OUT_RATE / 1000000;
    d->tick_gap_frames = gap > d->tick_gap_frames ? gap : d->tick_gap_frames - (d->tick_gap_frames - gap) / 16;
  }
  d->last_tick_us = now;
  ULONG64 ahead = d->tick_gap_frames * 5 / 2 + CHUNK_FRAMES;
  if (ahead < AHEAD_FRAMES) ahead = AHEAD_FRAMES;
  if (ahead > MAX_AHEAD_FRAMES) ahead = MAX_AHEAD_FRAMES;
  return ahead;
}

static void tick_locked(Device *d) {
  const ULONG64 ahead = ahead_frames_locked(d);
  BOOL any = FALSE;
  for (Buffer *b = d->buffers; b; b = b->next) any |= b->playing;
  if (!any && !d->stream) return;
  if (!open_stream_locked(d)) return;
  ULONG64 played = 0, waiting = 0;
  if (tcos_audio_status(d->stream, &waiting, NULL, NULL, &played) == 0) d->played = played / 4;
  if (d->played > d->produced) d->played = d->produced;
  send_pending_locked(d);
  /* mix while the kernel's stream holds less than `ahead` (the device's own queue comes after that, and is not ours to fill) */
  ULONG64 queued = waiting / 4;
  while (d->pending_frames == 0) {
    BOOL mixing = FALSE;
    for (Buffer *b = d->buffers; b; b = b->next) mixing |= b->playing && !b->ending;
    if (!mixing || queued >= ahead) break;
    queued += CHUNK_FRAMES;
    for (DWORD i = 0; i < CHUNK_FRAMES * 2; ++i) d->mix[i] = 0;
    for (Buffer *b = d->buffers; b; b = b->next)
      if (b->playing && !b->ending) mix_buffer(d, b, CHUNK_FRAMES);
    LONG master_l = 32768, master_r = 32768;
    if (d->primary) {
      const double g = hundredths_db(d->primary->volume);
      master_l = to_q15((float)(g * (d->primary->pan > 0 ? hundredths_db(-d->primary->pan) : 1)));
      master_r = to_q15((float)(g * (d->primary->pan < 0 ? hundredths_db(d->primary->pan) : 1)));
    }
    for (DWORD i = 0; i < CHUNK_FRAMES * 2; ++i) {
      const LONG64 v = ((LONG64)d->mix[i] * ((i & 1) ? master_r : master_l)) >> 15;
      d->pending[i] = (short)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
    }
    d->pending_frames = CHUNK_FRAMES;
    d->pending_offset = 0;
    d->produced += CHUNK_FRAMES;
    send_pending_locked(d);
  }
  /* where each buffer is now: notifications, and the end of the buffers that were mixed to their end */
  for (Buffer *b = d->buffers; b; b = b->next) {
    if (!b->playing) continue;
    if (b->ending && d->played >= b->ended_at) {
      fire_notifications(b, b->last_play, bytes_of(b, b->frames));      /* (the rest of the way to its end) */
      b->playing = b->ending = FALSE;
      b->position = 0;
      b->last_play = 0;
      fire_stop_notifications(b);
      continue;
    }
    const DWORD now = play_cursor_locked(d, b);
    fire_notifications(b, b->last_play, now);
    b->last_play = now;
  }
}

static DWORD WINAPI mixer_thread(LPVOID param) {
  Device *d = (Device *)param;
  while (!d->stop) {
    EnterCriticalSection(&d->lock);
    tick_locked(d);
    LeaveCriticalSection(&d->lock);
    WaitForSingleObject(d->wake, TICK_MS);
  }
  return 0;
}

/* ---- buffers ------------------------------------------------------------------------------------------------------------------------------------ */

static void default_3d(DS3DBUFFER *p) {
  zero(p, sizeof *p);
  p->dwSize = sizeof *p;
  p->dwInsideConeAngle = p->dwOutsideConeAngle = DS3D_DEFAULTCONEANGLE;
  p->vConeOrientation.z = 1;
  p->lConeOutsideVolume = DS3D_DEFAULTCONEOUTSIDEVOLUME;
  p->flMinDistance = DS3D_DEFAULTMINDISTANCE;
  p->flMaxDistance = DS3D_DEFAULTMAXDISTANCE;
  p->dwMode = DS3DMODE_NORMAL;
}

static void default_listener(DS3DLISTENER *l) {
  zero(l, sizeof *l);
  l->dwSize = sizeof *l;
  l->vOrientFront.z = 1;
  l->vOrientTop.y = 1;
  l->flDistanceFactor = DS3D_DEFAULTDISTANCEFACTOR;
  l->flRolloffFactor = DS3D_DEFAULTROLLOFFFACTOR;
  l->flDopplerFactor = DS3D_DEFAULTDOPPLERFACTOR;
}

static Buffer *new_buffer(Device *d) {
  Buffer *b = (Buffer *)alloc(sizeof *b);
  if (!b) return NULL;
  b->buffer.lpVtbl = (IDirectSoundBuffer8Vtbl *)&kBufferVtbl;
  b->notify.lpVtbl = (IDirectSoundNotifyVtbl *)&kNotifyVtbl;
  b->ds3d.lpVtbl = (IDirectSound3DBufferVtbl *)&kBuffer3dVtbl;
  b->listener.lpVtbl = (IDirectSound3DListenerVtbl *)&kListenerVtbl;
  b->refs = 1;
  b->device = d;
  default_3d(&b->params);
  b->deferred = b->params;
  device_hold(d);
  return b;
}

static void unlink_locked(Device *d, Buffer *b) {
  for (Buffer **p = &d->buffers; *p; p = &(*p)->next)
    if (*p == b) {
      *p = b->next;
      b->next = NULL;
      return;
    }
}

static void free_buffer(Buffer *b) {
  Device *d = b->device;
  EnterCriticalSection(&d->lock);
  unlink_locked(d, b);
  if (d->primary == b) d->primary = NULL;
  LeaveCriticalSection(&d->lock);
  if (b->memory && InterlockedDecrement(&b->memory->refs) == 0) {
    release_memory(b->memory->data);
    release_memory(b->memory);
  }
  release_memory(b->notifies);
  release_memory(b);
  device_drop(d);
}

static BOOL set_format(Buffer *b, const WAVEFORMATEX *f) {
  if (!wave_format_code(f)) return FALSE;
  const SIZE_T size = f->wFormatTag == TCOS_WAVE_FORMAT_EXTENSIBLE ? sizeof(TcosWaveFormatExtensible) : sizeof(WAVEFORMATEX);
  zero(&b->format, sizeof b->format);
  copy(&b->format, f, size);
  b->format.Format.cbSize = f->wFormatTag == TCOS_WAVE_FORMAT_EXTENSIBLE ? 22 : 0;
  b->format.Format.nAvgBytesPerSec = f->nSamplesPerSec * f->nBlockAlign;
  b->tag = wave_format_tag(f);
  return TRUE;
}

static BOOL give_memory(Buffer *b, DWORD bytes) {
  Memory *m = (Memory *)alloc(sizeof *m);
  if (!m) return FALSE;
  m->data = (BYTE *)HeapAlloc(GetProcessHeap(), 0, bytes ? bytes : 1);
  if (!m->data) {
    release_memory(m);
    return FALSE;
  }
  /* silence: 0x80 for 8-bit samples */
  const BYTE fill = b->tag == 1 && b->format.Format.wBitsPerSample == 8 ? 0x80 : 0;
  for (DWORD i = 0; i < bytes; ++i) m->data[i] = fill;
  m->refs = 1;
  m->size = bytes;
  b->memory = m;
  b->frames = bytes / b->format.Format.nBlockAlign;
  return TRUE;
}

/* The primary buffer in DSSCL_WRITEPRIMARY: memory in its format, mixed like a looping secondary buffer while it plays. */
static BOOL primary_memory_locked(Device *d, Buffer *b) {
  if (b->memory) return TRUE;
  if (!set_format(b, &d->primary_format)) return FALSE;
  b->frequency = d->primary_format.nSamplesPerSec;
  return give_memory(b, PRIMARY_FRAMES * d->primary_format.nBlockAlign);
}

static HRESULT WINAPI b_query(IDirectSoundBuffer8 *i, REFIID riid, void **out) {
  Buffer *b = from_buffer(i);
  if (!out) return E_POINTER;
  *out = NULL;
  if (!riid) return E_POINTER;
  if (same_guid(riid, &kIidUnknown) || same_guid(riid, &IID_IDirectSoundBuffer) || (same_guid(riid, &IID_IDirectSoundBuffer8) && !b->primary)) *out = &b->buffer;
  else if (same_guid(riid, &IID_IDirectSoundNotify) && (b->flags & DSBCAPS_CTRLPOSITIONNOTIFY) && !b->primary) *out = &b->notify;
  else if (same_guid(riid, &IID_IDirectSound3DBuffer) && (b->flags & DSBCAPS_CTRL3D) && !b->primary) *out = &b->ds3d;
  else if (same_guid(riid, &IID_IDirectSound3DListener) && b->primary) *out = &b->listener;
  else return E_NOINTERFACE;
  InterlockedIncrement(&b->refs);
  return S_OK;
}
static ULONG WINAPI b_add_ref(IDirectSoundBuffer8 *i) { return (ULONG)InterlockedIncrement(&from_buffer(i)->refs); }
static ULONG WINAPI b_release(IDirectSoundBuffer8 *i) {
  Buffer *b = from_buffer(i);
  const LONG left = InterlockedDecrement(&b->refs);
  if (left == 0) free_buffer(b);
  return (ULONG)left;
}
static HRESULT WINAPI b_get_caps(IDirectSoundBuffer8 *i, LPDSBCAPS caps) {
  Buffer *b = from_buffer(i);
  if (!caps || caps->dwSize < sizeof(DSBCAPS)) return DSERR_INVALIDPARAM;
  caps->dwFlags = b->flags | DSBCAPS_LOCSOFTWARE;
  caps->dwBufferBytes = b->primary && !b->memory ? PRIMARY_FRAMES * b->device->primary_format.nBlockAlign : (b->memory ? b->memory->size : 0);
  caps->dwUnlockTransferRate = 0;
  caps->dwPlayCpuOverhead = 0;
  return DS_OK;
}
static HRESULT WINAPI b_get_current_position(IDirectSoundBuffer8 *i, LPDWORD play, LPDWORD write) {
  Buffer *b = from_buffer(i);
  Device *d = b->device;
  if (!play && !write) return DSERR_INVALIDPARAM;
  EnterCriticalSection(&d->lock);
  DWORD p, w;
  if (b->primary && !b->memory) {
    /* (the mix as a ring the size of the primary buffer: where the device is, and where the mixer is) */
    const DWORD block = d->primary_format.nBlockAlign, size = PRIMARY_FRAMES;
    p = (DWORD)(d->played % size) * block;
    w = (DWORD)(d->produced % size) * block;
  } else {
    p = play_cursor_locked(d, b);
    const DWORD wf = (DWORD)(b->position >> 32);
    w = b->playing ? bytes_of(b, b->frames ? wf % b->frames : 0) : p;
    if (b->ending) w = 0;
  }
  LeaveCriticalSection(&d->lock);
  if (play) *play = p;
  if (write) *write = w;
  return DS_OK;
}
static HRESULT WINAPI b_get_format(IDirectSoundBuffer8 *i, LPWAVEFORMATEX format, DWORD size, LPDWORD written) {
  Buffer *b = from_buffer(i);
  const WAVEFORMATEX *f = b->primary && !b->memory ? &b->device->primary_format : &b->format.Format;
  const DWORD need = sizeof(WAVEFORMATEX) + f->cbSize;
  if (!format && !written) return DSERR_INVALIDPARAM;
  if (format) {
    if (size < need) return DSERR_INVALIDPARAM;
    copy(format, f, need);
  }
  if (written) *written = need;
  return DS_OK;
}
static HRESULT WINAPI b_get_volume(IDirectSoundBuffer8 *i, LPLONG volume) {
  Buffer *b = from_buffer(i);
  if (!volume) return DSERR_INVALIDPARAM;
  if (!(b->flags & DSBCAPS_CTRLVOLUME) && !b->primary) return DSERR_CONTROLUNAVAIL;
  *volume = b->volume;
  return DS_OK;
}
static HRESULT WINAPI b_get_pan(IDirectSoundBuffer8 *i, LPLONG pan) {
  Buffer *b = from_buffer(i);
  if (!pan) return DSERR_INVALIDPARAM;
  if (!(b->flags & DSBCAPS_CTRLPAN) && !b->primary) return DSERR_CONTROLUNAVAIL;
  *pan = b->pan;
  return DS_OK;
}
static HRESULT WINAPI b_get_frequency(IDirectSoundBuffer8 *i, LPDWORD frequency) {
  Buffer *b = from_buffer(i);
  if (!frequency) return DSERR_INVALIDPARAM;
  if (!(b->flags & DSBCAPS_CTRLFREQUENCY) || b->primary) return DSERR_CONTROLUNAVAIL;
  *frequency = b->frequency;
  return DS_OK;
}
static HRESULT WINAPI b_get_status(IDirectSoundBuffer8 *i, LPDWORD status) {
  Buffer *b = from_buffer(i);
  if (!status) return DSERR_INVALIDPARAM;
  EnterCriticalSection(&b->device->lock);
  DWORD s = 0;
  if (b->playing) s |= DSBSTATUS_PLAYING | (b->looping ? DSBSTATUS_LOOPING : 0);
  if ((b->flags & DSBCAPS_LOCDEFER) && b->playing) s |= DSBSTATUS_LOCSOFTWARE;
  if (b->primary && !b->memory) s |= DSBSTATUS_PLAYING | DSBSTATUS_LOOPING;      /* (the mix always runs) */
  LeaveCriticalSection(&b->device->lock);
  *status = s;
  return DS_OK;
}
static HRESULT WINAPI b_initialize(IDirectSoundBuffer8 *i, LPDIRECTSOUND ds, LPCDSBUFFERDESC desc) {
  (void)i; (void)ds; (void)desc;
  return DSERR_ALREADYINITIALIZED;
}
static HRESULT WINAPI b_lock(IDirectSoundBuffer8 *i, DWORD offset, DWORD bytes, LPVOID *p1, LPDWORD n1, LPVOID *p2, LPDWORD n2, DWORD flags) {
  Buffer *b = from_buffer(i);
  Device *d = b->device;
  if (p1) *p1 = NULL;
  if (n1) *n1 = 0;
  if (p2) *p2 = NULL;
  if (n2) *n2 = 0;
  if (!p1 || !n1) return DSERR_INVALIDPARAM;
  EnterCriticalSection(&d->lock);
  if (b->primary) {
    if (d->level != DSSCL_WRITEPRIMARY || !primary_memory_locked(d, b)) {
      LeaveCriticalSection(&d->lock);
      return DSERR_PRIOLEVELNEEDED;
    }
  }
  const DWORD size = b->memory->size;
  if (flags & DSBLOCK_FROMWRITECURSOR) {
    const DWORD wf = (DWORD)(b->position >> 32);
    offset = b->playing && b->frames ? bytes_of(b, wf % b->frames) : play_cursor_locked(d, b);
  }
  if (flags & DSBLOCK_ENTIREBUFFER) bytes = size;
  LeaveCriticalSection(&d->lock);
  if (offset >= size || bytes == 0 || bytes > size) return DSERR_INVALIDPARAM;
  const DWORD first = bytes < size - offset ? bytes : size - offset;
  *p1 = b->memory->data + offset;
  *n1 = first;
  if (first < bytes) {
    if (p2 && n2) {
      *p2 = b->memory->data;
      *n2 = bytes - first;
    }
  }
  return DS_OK;
}
static HRESULT WINAPI b_play(IDirectSoundBuffer8 *i, DWORD reserved, DWORD priority, DWORD flags) {
  Buffer *b = from_buffer(i);
  Device *d = b->device;
  if (reserved) return DSERR_INVALIDPARAM;
  if (priority && !(b->flags & DSBCAPS_LOCDEFER)) return DSERR_INVALIDPARAM;
  EnterCriticalSection(&d->lock);
  if (b->primary) {
    if (!(flags & DSBPLAY_LOOPING)) {
      LeaveCriticalSection(&d->lock);
      return DSERR_INVALIDPARAM;
    }
    /* with DSSCL_WRITEPRIMARY the program's own data plays; otherwise the mix runs anyway */
    if (d->level == DSSCL_WRITEPRIMARY && primary_memory_locked(d, b) && !b->playing) {
      b->next = d->buffers;
      d->buffers = b;
    } else {
      LeaveCriticalSection(&d->lock);
      return DS_OK;
    }
  }
  const BOOL was = b->playing && !b->ending;
  b->looping = (flags & DSBPLAY_LOOPING) != 0;
  if (!was) {
    rewind_locked(d);      /* (it is heard from now, not after what the others had mixed ahead) */
    if (b->ending) b->position = 0;
    b->playing = TRUE;
    b->ending = FALSE;
    b->started_at = d->produced;
    b->last_play = bytes_of(b, b->frames ? (DWORD)(b->position >> 32) % b->frames : 0);
  }
  LeaveCriticalSection(&d->lock);
  SetEvent(d->wake);
  return DS_OK;
}
static HRESULT WINAPI b_set_current_position(IDirectSoundBuffer8 *i, DWORD position) {
  Buffer *b = from_buffer(i);
  Device *d = b->device;
  if (b->primary) return DSERR_INVALIDCALL;
  if (position >= b->memory->size) return DSERR_INVALIDPARAM;
  EnterCriticalSection(&d->lock);
  b->position = (ULONG64)(position / b->format.Format.nBlockAlign) << 32;
  b->started_at = d->produced;
  b->ending = FALSE;
  b->last_play = position - position % b->format.Format.nBlockAlign;
  LeaveCriticalSection(&d->lock);
  return DS_OK;
}
static HRESULT WINAPI b_set_format(IDirectSoundBuffer8 *i, LPCWAVEFORMATEX format) {
  Buffer *b = from_buffer(i);
  Device *d = b->device;
  if (!b->primary) return DSERR_INVALIDCALL;
  if (!format) return DSERR_INVALIDPARAM;
  if (d->level < DSSCL_PRIORITY) return DSERR_PRIOLEVELNEEDED;
  if (!wave_format_code(format) || format->nChannels > 2) return DSERR_BADFORMAT;
  EnterCriticalSection(&d->lock);
  if (b->playing) {
    LeaveCriticalSection(&d->lock);
    return DSERR_INVALIDCALL;
  }
  zero(&d->primary_format, sizeof d->primary_format);
  copy(&d->primary_format, format, sizeof(WAVEFORMATEX));
  d->primary_format.cbSize = 0;
  if (d->primary_format.wFormatTag == TCOS_WAVE_FORMAT_EXTENSIBLE) d->primary_format.wFormatTag = wave_format_tag(format);
  d->primary_format.nAvgBytesPerSec = format->nSamplesPerSec * format->nBlockAlign;
  if (b->memory) {      /* (the program's primary memory goes; it is made again in the new format) */
    if (InterlockedDecrement(&b->memory->refs) == 0) {
      release_memory(b->memory->data);
      release_memory(b->memory);
    }
    b->memory = NULL;
    b->frames = 0;
  }
  LeaveCriticalSection(&d->lock);
  return DS_OK;
}
static HRESULT WINAPI b_set_volume(IDirectSoundBuffer8 *i, LONG volume) {
  Buffer *b = from_buffer(i);
  if (!(b->flags & DSBCAPS_CTRLVOLUME) && !b->primary) return DSERR_CONTROLUNAVAIL;
  if (volume < DSBVOLUME_MIN || volume > DSBVOLUME_MAX) return DSERR_INVALIDPARAM;
  EnterCriticalSection(&b->device->lock);
  b->volume = volume;
  LeaveCriticalSection(&b->device->lock);
  return DS_OK;
}
static HRESULT WINAPI b_set_pan(IDirectSoundBuffer8 *i, LONG pan) {
  Buffer *b = from_buffer(i);
  if (!(b->flags & DSBCAPS_CTRLPAN) && !b->primary) return DSERR_CONTROLUNAVAIL;
  if (pan < DSBPAN_LEFT || pan > DSBPAN_RIGHT) return DSERR_INVALIDPARAM;
  EnterCriticalSection(&b->device->lock);
  b->pan = pan;
  LeaveCriticalSection(&b->device->lock);
  return DS_OK;
}
static HRESULT WINAPI b_set_frequency(IDirectSoundBuffer8 *i, DWORD frequency) {
  Buffer *b = from_buffer(i);
  if (!(b->flags & DSBCAPS_CTRLFREQUENCY) || b->primary) return DSERR_CONTROLUNAVAIL;
  if (frequency == DSBFREQUENCY_ORIGINAL) frequency = b->format.Format.nSamplesPerSec;
  if (frequency < DSBFREQUENCY_MIN || frequency > DSBFREQUENCY_MAX) return DSERR_INVALIDPARAM;
  EnterCriticalSection(&b->device->lock);
  b->frequency = frequency;
  LeaveCriticalSection(&b->device->lock);
  return DS_OK;
}
static HRESULT WINAPI b_stop(IDirectSoundBuffer8 *i) {
  Buffer *b = from_buffer(i);
  Device *d = b->device;
  EnterCriticalSection(&d->lock);
  if (b->playing) {
    /* it stays where the device is, so that Play goes on from there */
    const DWORD at = b->ending ? 0 : play_cursor_locked(d, b);
    b->position = (ULONG64)(at / b->format.Format.nBlockAlign) << 32;
    b->playing = b->ending = FALSE;
    b->last_play = at;
    if (b->primary) unlink_locked(d, b);
    rewind_locked(d);      /* (what it had mixed ahead goes; the others are mixed again without it) */
    fire_stop_notifications(b);
  }
  LeaveCriticalSection(&d->lock);
  return DS_OK;
}
static HRESULT WINAPI b_unlock(IDirectSoundBuffer8 *i, LPVOID p1, DWORD n1, LPVOID p2, DWORD n2) {
  Buffer *b = from_buffer(i);
  if (!b->memory) return DSERR_INVALIDCALL;
  const BYTE *start = b->memory->data, *end = start + b->memory->size;
  if (p1 && ((const BYTE *)p1 < start || (const BYTE *)p1 + n1 > end)) return DSERR_INVALIDPARAM;
  if (p2 && ((const BYTE *)p2 < start || (const BYTE *)p2 + n2 > end)) return DSERR_INVALIDPARAM;
  return DS_OK;
}
static HRESULT WINAPI b_restore(IDirectSoundBuffer8 *i) { (void)i; return DS_OK; }
static HRESULT WINAPI b_set_fx(IDirectSoundBuffer8 *i, DWORD count, LPDSEFFECTDESC desc, LPDWORD results) {
  Buffer *b = from_buffer(i);
  if (!(b->flags & DSBCAPS_CTRLFX)) return DSERR_CONTROLUNAVAIL;
  if (count && !desc) return DSERR_INVALIDPARAM;
  if (b->playing) return DSERR_INVALIDCALL;
  for (DWORD k = 0; k < count && results; ++k) results[k] = DSFXR_UNKNOWN;
  return count ? DSERR_FXUNAVAILABLE : DS_OK;
}
static HRESULT WINAPI b_acquire_resources(IDirectSoundBuffer8 *i, DWORD flags, DWORD count, LPDWORD results) {
  Buffer *b = from_buffer(i);
  (void)flags;
  if (!(b->flags & DSBCAPS_LOCDEFER)) return DSERR_INVALIDCALL;
  for (DWORD k = 0; k < count && results; ++k) results[k] = DSFXR_LOCSOFTWARE;
  return DS_OK;
}
static HRESULT WINAPI b_get_object_in_path(IDirectSoundBuffer8 *i, REFGUID object, DWORD index, REFGUID iid, LPVOID *out) {
  (void)i; (void)object; (void)index; (void)iid;
  if (!out) return E_INVALIDARG;
  *out = NULL;
  return DSERR_OBJECTNOTFOUND;
}
static const IDirectSoundBuffer8Vtbl kBufferVtbl = {
  b_query, b_add_ref, b_release, b_get_caps, b_get_current_position, b_get_format, b_get_volume, b_get_pan, b_get_frequency, b_get_status, b_initialize,
  b_lock, b_play, b_set_current_position, b_set_format, b_set_volume, b_set_pan, b_set_frequency, b_stop, b_unlock, b_restore, b_set_fx,
  b_acquire_resources, b_get_object_in_path};

/* ---- position notifications --------------------------------------------------------------------------------------------------------------------- */

static HRESULT WINAPI n_query(IDirectSoundNotify *i, REFIID riid, void **out) { return b_query(&from_notify(i)->buffer, riid, out); }
static ULONG WINAPI n_add_ref(IDirectSoundNotify *i) { return b_add_ref(&from_notify(i)->buffer); }
static ULONG WINAPI n_release(IDirectSoundNotify *i) { return b_release(&from_notify(i)->buffer); }
static HRESULT WINAPI n_set_positions(IDirectSoundNotify *i, DWORD count, LPCDSBPOSITIONNOTIFY positions) {
  Buffer *b = from_notify(i);
  if (count > DSBNOTIFICATIONS_MAX || (count && !positions)) return DSERR_INVALIDPARAM;
  for (DWORD k = 0; k < count; ++k)
    if (positions[k].dwOffset != (DWORD)DSBPN_OFFSETSTOP && positions[k].dwOffset >= b->memory->size) return DSERR_INVALIDPARAM;
  DSBPOSITIONNOTIFY *copy_of = NULL;
  if (count) {
    copy_of = (DSBPOSITIONNOTIFY *)alloc(count * sizeof *copy_of);
    if (!copy_of) return DSERR_OUTOFMEMORY;
    copy(copy_of, positions, count * sizeof *copy_of);
  }
  EnterCriticalSection(&b->device->lock);
  if (b->playing) {
    LeaveCriticalSection(&b->device->lock);
    release_memory(copy_of);
    return DSERR_INVALIDCALL;
  }
  DSBPOSITIONNOTIFY *old = b->notifies;
  b->notifies = copy_of;
  b->notify_count = count;
  LeaveCriticalSection(&b->device->lock);
  release_memory(old);
  return DS_OK;
}
static const IDirectSoundNotifyVtbl kNotifyVtbl = {n_query, n_add_ref, n_release, n_set_positions};

/* ---- 3D buffers --------------------------------------------------------------------------------------------------------------------------------- */

/* A change to `b`'s 3D parameters: in effect now (DS3D_IMMEDIATE), or at the listener's CommitDeferredSettings. */
#define CHANGE_3D(b, apply, field, value)                         \
  do {                                                            \
    EnterCriticalSection(&(b)->device->lock);                     \
    (b)->deferred.field = (value);                                \
    if ((apply) == DS3D_IMMEDIATE) (b)->params.field = (value);   \
    else (b)->deferred_dirty = TRUE;                              \
    LeaveCriticalSection(&(b)->device->lock);                     \
  } while (0)

static HRESULT WINAPI t_query(IDirectSound3DBuffer *i, REFIID riid, void **out) { return b_query(&from_3d(i)->buffer, riid, out); }
static ULONG WINAPI t_add_ref(IDirectSound3DBuffer *i) { return b_add_ref(&from_3d(i)->buffer); }
static ULONG WINAPI t_release(IDirectSound3DBuffer *i) { return b_release(&from_3d(i)->buffer); }
static HRESULT WINAPI t_get_all(IDirectSound3DBuffer *i, LPDS3DBUFFER p) {
  if (!p || p->dwSize < sizeof *p) return DSERR_INVALIDPARAM;
  *p = from_3d(i)->params;
  return DS_OK;
}
static HRESULT WINAPI t_get_cone_angles(IDirectSound3DBuffer *i, LPDWORD inside, LPDWORD outside) {
  if (!inside || !outside) return DSERR_INVALIDPARAM;
  *inside = from_3d(i)->params.dwInsideConeAngle;
  *outside = from_3d(i)->params.dwOutsideConeAngle;
  return DS_OK;
}
static HRESULT WINAPI t_get_cone_orientation(IDirectSound3DBuffer *i, LPD3DVECTOR v) {
  if (!v) return DSERR_INVALIDPARAM;
  *v = from_3d(i)->params.vConeOrientation;
  return DS_OK;
}
static HRESULT WINAPI t_get_cone_outside_volume(IDirectSound3DBuffer *i, LPLONG v) {
  if (!v) return DSERR_INVALIDPARAM;
  *v = from_3d(i)->params.lConeOutsideVolume;
  return DS_OK;
}
static HRESULT WINAPI t_get_max_distance(IDirectSound3DBuffer *i, LPD3DVALUE v) {
  if (!v) return DSERR_INVALIDPARAM;
  *v = from_3d(i)->params.flMaxDistance;
  return DS_OK;
}
static HRESULT WINAPI t_get_min_distance(IDirectSound3DBuffer *i, LPD3DVALUE v) {
  if (!v) return DSERR_INVALIDPARAM;
  *v = from_3d(i)->params.flMinDistance;
  return DS_OK;
}
static HRESULT WINAPI t_get_mode(IDirectSound3DBuffer *i, LPDWORD v) {
  if (!v) return DSERR_INVALIDPARAM;
  *v = from_3d(i)->params.dwMode;
  return DS_OK;
}
static HRESULT WINAPI t_get_position(IDirectSound3DBuffer *i, LPD3DVECTOR v) {
  if (!v) return DSERR_INVALIDPARAM;
  *v = from_3d(i)->params.vPosition;
  return DS_OK;
}
static HRESULT WINAPI t_get_velocity(IDirectSound3DBuffer *i, LPD3DVECTOR v) {
  if (!v) return DSERR_INVALIDPARAM;
  *v = from_3d(i)->params.vVelocity;
  return DS_OK;
}
static BOOL valid_3d(const DS3DBUFFER *p) {
  return p->dwInsideConeAngle <= 360 && p->dwOutsideConeAngle <= 360 && p->lConeOutsideVolume >= DSBVOLUME_MIN && p->lConeOutsideVolume <= DSBVOLUME_MAX &&
         p->flMinDistance > 0 && p->flMaxDistance > 0 && p->dwMode <= DS3DMODE_DISABLE;
}
static HRESULT WINAPI t_set_all(IDirectSound3DBuffer *i, LPCDS3DBUFFER p, DWORD apply) {
  Buffer *b = from_3d(i);
  if (!p || p->dwSize < sizeof *p || !valid_3d(p)) return DSERR_INVALIDPARAM;
  EnterCriticalSection(&b->device->lock);
  b->deferred = *p;
  b->deferred.dwSize = sizeof *p;
  if (apply == DS3D_IMMEDIATE) b->params = b->deferred;
  else b->deferred_dirty = TRUE;
  LeaveCriticalSection(&b->device->lock);
  return DS_OK;
}
static HRESULT WINAPI t_set_cone_angles(IDirectSound3DBuffer *i, DWORD inside, DWORD outside, DWORD apply) {
  Buffer *b = from_3d(i);
  if (inside > 360 || outside > 360) return DSERR_INVALIDPARAM;
  CHANGE_3D(b, apply, dwInsideConeAngle, inside);
  CHANGE_3D(b, apply, dwOutsideConeAngle, outside);
  return DS_OK;
}
static HRESULT WINAPI t_set_cone_orientation(IDirectSound3DBuffer *i, D3DVALUE x, D3DVALUE y, D3DVALUE z, DWORD apply) {
  Buffer *b = from_3d(i);
  D3DVECTOR v = {x, y, z};
  CHANGE_3D(b, apply, vConeOrientation, v);
  return DS_OK;
}
static HRESULT WINAPI t_set_cone_outside_volume(IDirectSound3DBuffer *i, LONG volume, DWORD apply) {
  Buffer *b = from_3d(i);
  if (volume < DSBVOLUME_MIN || volume > DSBVOLUME_MAX) return DSERR_INVALIDPARAM;
  CHANGE_3D(b, apply, lConeOutsideVolume, volume);
  return DS_OK;
}
static HRESULT WINAPI t_set_max_distance(IDirectSound3DBuffer *i, D3DVALUE v, DWORD apply) {
  Buffer *b = from_3d(i);
  if (!(v > 0)) return DSERR_INVALIDPARAM;
  CHANGE_3D(b, apply, flMaxDistance, v);
  return DS_OK;
}
static HRESULT WINAPI t_set_min_distance(IDirectSound3DBuffer *i, D3DVALUE v, DWORD apply) {
  Buffer *b = from_3d(i);
  if (!(v > 0)) return DSERR_INVALIDPARAM;
  CHANGE_3D(b, apply, flMinDistance, v);
  return DS_OK;
}
static HRESULT WINAPI t_set_mode(IDirectSound3DBuffer *i, DWORD mode, DWORD apply) {
  Buffer *b = from_3d(i);
  if (mode > DS3DMODE_DISABLE) return DSERR_INVALIDPARAM;
  CHANGE_3D(b, apply, dwMode, mode);
  return DS_OK;
}
static HRESULT WINAPI t_set_position(IDirectSound3DBuffer *i, D3DVALUE x, D3DVALUE y, D3DVALUE z, DWORD apply) {
  Buffer *b = from_3d(i);
  D3DVECTOR v = {x, y, z};
  CHANGE_3D(b, apply, vPosition, v);
  return DS_OK;
}
static HRESULT WINAPI t_set_velocity(IDirectSound3DBuffer *i, D3DVALUE x, D3DVALUE y, D3DVALUE z, DWORD apply) {
  Buffer *b = from_3d(i);
  D3DVECTOR v = {x, y, z};
  CHANGE_3D(b, apply, vVelocity, v);
  return DS_OK;
}
static const IDirectSound3DBufferVtbl kBuffer3dVtbl = {
  t_query, t_add_ref, t_release, t_get_all, t_get_cone_angles, t_get_cone_orientation, t_get_cone_outside_volume, t_get_max_distance, t_get_min_distance,
  t_get_mode, t_get_position, t_get_velocity, t_set_all, t_set_cone_angles, t_set_cone_orientation, t_set_cone_outside_volume, t_set_max_distance,
  t_set_min_distance, t_set_mode, t_set_position, t_set_velocity};

/* ---- the listener (the primary buffer's) -------------------------------------------------------------------------------------------------------- */

#define CHANGE_LISTENER(d, apply, field, value)                   \
  do {                                                            \
    EnterCriticalSection(&(d)->lock);                             \
    (d)->listener_deferred.field = (value);                       \
    if ((apply) == DS3D_IMMEDIATE) (d)->listener.field = (value); \
    else (d)->listener_dirty = TRUE;                              \
    LeaveCriticalSection(&(d)->lock);                             \
  } while (0)

static HRESULT WINAPI l_query(IDirectSound3DListener *i, REFIID riid, void **out) { return b_query(&from_listener(i)->buffer, riid, out); }
static ULONG WINAPI l_add_ref(IDirectSound3DListener *i) { return b_add_ref(&from_listener(i)->buffer); }
static ULONG WINAPI l_release(IDirectSound3DListener *i) { return b_release(&from_listener(i)->buffer); }
static Device *listener_device(IDirectSound3DListener *i) { return from_listener(i)->device; }
static HRESULT WINAPI l_get_all(IDirectSound3DListener *i, LPDS3DLISTENER p) {
  if (!p || p->dwSize < sizeof *p) return DSERR_INVALIDPARAM;
  *p = listener_device(i)->listener;
  return DS_OK;
}
static HRESULT WINAPI l_get_distance_factor(IDirectSound3DListener *i, LPD3DVALUE v) {
  if (!v) return DSERR_INVALIDPARAM;
  *v = listener_device(i)->listener.flDistanceFactor;
  return DS_OK;
}
static HRESULT WINAPI l_get_doppler_factor(IDirectSound3DListener *i, LPD3DVALUE v) {
  if (!v) return DSERR_INVALIDPARAM;
  *v = listener_device(i)->listener.flDopplerFactor;
  return DS_OK;
}
static HRESULT WINAPI l_get_orientation(IDirectSound3DListener *i, LPD3DVECTOR front, LPD3DVECTOR top) {
  if (!front || !top) return DSERR_INVALIDPARAM;
  *front = listener_device(i)->listener.vOrientFront;
  *top = listener_device(i)->listener.vOrientTop;
  return DS_OK;
}
static HRESULT WINAPI l_get_position(IDirectSound3DListener *i, LPD3DVECTOR v) {
  if (!v) return DSERR_INVALIDPARAM;
  *v = listener_device(i)->listener.vPosition;
  return DS_OK;
}
static HRESULT WINAPI l_get_rolloff_factor(IDirectSound3DListener *i, LPD3DVALUE v) {
  if (!v) return DSERR_INVALIDPARAM;
  *v = listener_device(i)->listener.flRolloffFactor;
  return DS_OK;
}
static HRESULT WINAPI l_get_velocity(IDirectSound3DListener *i, LPD3DVECTOR v) {
  if (!v) return DSERR_INVALIDPARAM;
  *v = listener_device(i)->listener.vVelocity;
  return DS_OK;
}
static HRESULT WINAPI l_set_all(IDirectSound3DListener *i, LPCDS3DLISTENER p, DWORD apply) {
  Device *d = listener_device(i);
  if (!p || p->dwSize < sizeof *p || !(p->flRolloffFactor >= DS3D_MINROLLOFFFACTOR && p->flRolloffFactor <= DS3D_MAXROLLOFFFACTOR) ||
      !(p->flDopplerFactor >= DS3D_MINDOPPLERFACTOR && p->flDopplerFactor <= DS3D_MAXDOPPLERFACTOR) || !(p->flDistanceFactor > 0))
    return DSERR_INVALIDPARAM;
  EnterCriticalSection(&d->lock);
  d->listener_deferred = *p;
  d->listener_deferred.dwSize = sizeof *p;
  if (apply == DS3D_IMMEDIATE) d->listener = d->listener_deferred;
  else d->listener_dirty = TRUE;
  LeaveCriticalSection(&d->lock);
  return DS_OK;
}
static HRESULT WINAPI l_set_distance_factor(IDirectSound3DListener *i, D3DVALUE v, DWORD apply) {
  if (!(v > 0)) return DSERR_INVALIDPARAM;
  CHANGE_LISTENER(listener_device(i), apply, flDistanceFactor, v);
  return DS_OK;
}
static HRESULT WINAPI l_set_doppler_factor(IDirectSound3DListener *i, D3DVALUE v, DWORD apply) {
  if (!(v >= DS3D_MINDOPPLERFACTOR && v <= DS3D_MAXDOPPLERFACTOR)) return DSERR_INVALIDPARAM;
  CHANGE_LISTENER(listener_device(i), apply, flDopplerFactor, v);
  return DS_OK;
}
static HRESULT WINAPI l_set_orientation(IDirectSound3DListener *i, D3DVALUE fx, D3DVALUE fy, D3DVALUE fz, D3DVALUE tx, D3DVALUE ty, D3DVALUE tz, DWORD apply) {
  D3DVECTOR front = {fx, fy, fz}, top = {tx, ty, tz};
  if ((fx == 0 && fy == 0 && fz == 0) || (tx == 0 && ty == 0 && tz == 0)) return DSERR_INVALIDPARAM;
  CHANGE_LISTENER(listener_device(i), apply, vOrientFront, front);
  CHANGE_LISTENER(listener_device(i), apply, vOrientTop, top);
  return DS_OK;
}
static HRESULT WINAPI l_set_position(IDirectSound3DListener *i, D3DVALUE x, D3DVALUE y, D3DVALUE z, DWORD apply) {
  D3DVECTOR v = {x, y, z};
  CHANGE_LISTENER(listener_device(i), apply, vPosition, v);
  return DS_OK;
}
static HRESULT WINAPI l_set_rolloff_factor(IDirectSound3DListener *i, D3DVALUE v, DWORD apply) {
  if (!(v >= DS3D_MINROLLOFFFACTOR && v <= DS3D_MAXROLLOFFFACTOR)) return DSERR_INVALIDPARAM;
  CHANGE_LISTENER(listener_device(i), apply, flRolloffFactor, v);
  return DS_OK;
}
static HRESULT WINAPI l_set_velocity(IDirectSound3DListener *i, D3DVALUE x, D3DVALUE y, D3DVALUE z, DWORD apply) {
  D3DVECTOR v = {x, y, z};
  CHANGE_LISTENER(listener_device(i), apply, vVelocity, v);
  return DS_OK;
}
static HRESULT WINAPI l_commit(IDirectSound3DListener *i) {
  Device *d = listener_device(i);
  EnterCriticalSection(&d->lock);
  if (d->listener_dirty) d->listener = d->listener_deferred;
  d->listener_dirty = FALSE;
  for (Buffer *b = d->buffers; b; b = b->next)
    if (b->deferred_dirty) {
      b->params = b->deferred;
      b->deferred_dirty = FALSE;
    }
  LeaveCriticalSection(&d->lock);
  return DS_OK;
}
static const IDirectSound3DListenerVtbl kListenerVtbl = {
  l_query, l_add_ref, l_release, l_get_all, l_get_distance_factor, l_get_doppler_factor, l_get_orientation, l_get_position, l_get_rolloff_factor,
  l_get_velocity, l_set_all, l_set_distance_factor, l_set_doppler_factor, l_set_orientation, l_set_position, l_set_rolloff_factor, l_set_velocity, l_commit};

/* ---- the device --------------------------------------------------------------------------------------------------------------------------------- */

static Device *new_device(void) {
  Device *d = (Device *)alloc(sizeof *d);
  if (!d) return NULL;
  d->iface.lpVtbl = (IDirectSound8Vtbl *)&kDeviceVtbl;
  d->refs = 1;
  d->holds = 1;
  d->speaker_config = DSSPEAKER_COMBINED(DSSPEAKER_STEREO, DSSPEAKER_GEOMETRY_WIDE);
  WAVEFORMATEX f = {WAVE_FORMAT_PCM, 2, 44100, 44100 * 4, 4, 16, 0};      /* (DirectSound's default primary format) */
  d->primary_format = f;
  default_listener(&d->listener);
  d->listener_deferred = d->listener;
  InitializeCriticalSection(&d->lock);
  d->wake = CreateEventA(NULL, FALSE, FALSE, NULL);
  return d;
}

static BOOL playback_guid(const GUID *g) {
  return !g || same_guid(g, &kGuidNull) || same_guid(g, &DSDEVID_DefaultPlayback) || same_guid(g, &DSDEVID_DefaultVoicePlayback) || same_guid(g, &kDeviceGuid);
}

static HRESULT WINAPI d_query(IDirectSound8 *i, REFIID riid, void **out) {
  Device *d = from_device(i);
  if (!out) return E_POINTER;
  *out = NULL;
  if (!riid) return E_POINTER;
  if (same_guid(riid, &kIidUnknown) || same_guid(riid, &IID_IDirectSound) || same_guid(riid, &IID_IDirectSound8)) *out = &d->iface;
  else return E_NOINTERFACE;
  InterlockedIncrement(&d->refs);
  return S_OK;
}
static ULONG WINAPI d_add_ref(IDirectSound8 *i) { return (ULONG)InterlockedIncrement(&from_device(i)->refs); }
static ULONG WINAPI d_release(IDirectSound8 *i) {
  Device *d = from_device(i);
  const LONG left = InterlockedDecrement(&d->refs);
  if (left != 0) return (ULONG)left;
  /* the program is done with it: the mixer stops (the buffers it still holds stay valid, and silent) */
  if (d->thread) {
    d->stop = 1;
    SetEvent(d->wake);
    WaitForSingleObject(d->thread, INFINITE);
    CloseHandle(d->thread);
    d->thread = NULL;
  }
  EnterCriticalSection(&d->lock);
  for (Buffer *b = d->buffers; b; b = b->next) b->playing = b->ending = FALSE;
  if (d->stream) audio_call(TCOS_AUDIO_CLOSE, d->stream, 1, 0, 0, 0, 0, NULL, NULL, NULL);
  d->stream = 0;
  LeaveCriticalSection(&d->lock);
  device_drop(d);
  return 0;
}
static HRESULT create_buffer(Device *d, LPCDSBUFFERDESC desc, LPDIRECTSOUNDBUFFER *out, IUnknown *outer) {
  if (!out) return DSERR_INVALIDPARAM;
  *out = NULL;
  if (outer) return DSERR_NOAGGREGATION;
  if (!desc || (desc->dwSize != sizeof(DSBUFFERDESC) && desc->dwSize != sizeof(DSBUFFERDESC1))) return DSERR_INVALIDPARAM;
  if (!d->initialized) return DSERR_UNINITIALIZED;
  if ((desc->dwFlags & (DSBCAPS_LOCHARDWARE | DSBCAPS_LOCSOFTWARE)) == (DSBCAPS_LOCHARDWARE | DSBCAPS_LOCSOFTWARE)) return DSERR_INVALIDPARAM;
  if (desc->dwFlags & DSBCAPS_LOCHARDWARE) return DSERR_CONTROLUNAVAIL;      /* (everything is mixed in software) */
  if (desc->dwFlags & DSBCAPS_PRIMARYBUFFER) {
    if (desc->dwBufferBytes || desc->lpwfxFormat || (desc->dwFlags & (DSBCAPS_CTRLFREQUENCY | DSBCAPS_CTRLFX | DSBCAPS_CTRLPOSITIONNOTIFY))) return DSERR_INVALIDPARAM;
    EnterCriticalSection(&d->lock);
    Buffer *p = d->primary;
    if (p) {
      InterlockedIncrement(&p->refs);
      p->flags |= desc->dwFlags;
    } else {
      p = new_buffer(d);
      if (p) {
        p->primary = TRUE;
        p->flags = desc->dwFlags;
        d->primary = p;
      }
    }
    LeaveCriticalSection(&d->lock);
    if (!p) return DSERR_OUTOFMEMORY;
    *out = (LPDIRECTSOUNDBUFFER)&p->buffer;
    return DS_OK;
  }
  if (d->level == DSSCL_WRITEPRIMARY) return DSERR_INVALIDCALL;
  if (!desc->lpwfxFormat) return DSERR_INVALIDPARAM;
  if (desc->dwBufferBytes < DSBSIZE_MIN || desc->dwBufferBytes > DSBSIZE_MAX) return DSERR_INVALIDPARAM;
  if ((desc->dwFlags & DSBCAPS_CTRLFX) && desc->dwBufferBytes < (DWORD)((ULONG64)desc->lpwfxFormat->nAvgBytesPerSec * DSBSIZE_FX_MIN / 1000)) return DSERR_BUFFERTOOSMALL;
  Buffer *b = new_buffer(d);
  if (!b) return DSERR_OUTOFMEMORY;
  if (!set_format(b, desc->lpwfxFormat) || b->format.Format.nChannels > 8) {
    free_buffer(b);
    return DSERR_BADFORMAT;
  }
  b->flags = desc->dwFlags;
  b->frequency = b->format.Format.nSamplesPerSec;
  const DWORD bytes = desc->dwBufferBytes - desc->dwBufferBytes % b->format.Format.nBlockAlign;
  if (bytes == 0 || !give_memory(b, bytes)) {
    free_buffer(b);
    return bytes == 0 ? DSERR_INVALIDPARAM : DSERR_OUTOFMEMORY;
  }
  if (desc->dwSize == sizeof(DSBUFFERDESC) && (desc->dwFlags & DSBCAPS_CTRL3D) && desc->guid3DAlgorithm.Data1 == 0) {}      /* (any algorithm: the same one) */
  EnterCriticalSection(&d->lock);
  b->next = d->buffers;
  d->buffers = b;
  LeaveCriticalSection(&d->lock);
  *out = (LPDIRECTSOUNDBUFFER)&b->buffer;
  return DS_OK;
}
static HRESULT WINAPI d_create_sound_buffer(IDirectSound8 *i, LPCDSBUFFERDESC desc, LPLPDIRECTSOUNDBUFFER out, IUnknown *outer) {
  return create_buffer(from_device(i), desc, out, outer);
}
static HRESULT WINAPI d_get_caps(IDirectSound8 *i, LPDSCAPS caps) {
  Device *d = from_device(i);
  if (!caps || caps->dwSize < sizeof(DSCAPS)) return DSERR_INVALIDPARAM;
  if (!d->initialized) return DSERR_UNINITIALIZED;
  const DWORD size = caps->dwSize;
  zero(caps, sizeof(DSCAPS));
  caps->dwSize = size;
  caps->dwFlags = DSCAPS_PRIMARYMONO | DSCAPS_PRIMARYSTEREO | DSCAPS_PRIMARY8BIT | DSCAPS_PRIMARY16BIT | DSCAPS_CONTINUOUSRATE | DSCAPS_CERTIFIED |
                  DSCAPS_SECONDARYMONO | DSCAPS_SECONDARYSTEREO | DSCAPS_SECONDARY8BIT | DSCAPS_SECONDARY16BIT;
  caps->dwMinSecondarySampleRate = DSBFREQUENCY_MIN;
  caps->dwMaxSecondarySampleRate = DSBFREQUENCY_MAX;
  caps->dwPrimaryBuffers = 1;
  caps->dwPlayCpuOverheadSwBuffers = 1;
  return DS_OK;
}
static HRESULT WINAPI d_duplicate(IDirectSound8 *i, LPDIRECTSOUNDBUFFER original, LPLPDIRECTSOUNDBUFFER out) {
  Device *d = from_device(i);
  if (!out) return DSERR_INVALIDPARAM;
  *out = NULL;
  if (!original) return DSERR_INVALIDPARAM;
  Buffer *o = from_buffer(original);
  if (o->primary || o->device != d) return DSERR_INVALIDCALL;
  if (o->flags & DSBCAPS_CTRLFX) return DSERR_INVALIDCALL;
  Buffer *b = new_buffer(d);
  if (!b) return DSERR_OUTOFMEMORY;
  EnterCriticalSection(&d->lock);
  b->flags = o->flags;
  b->format = o->format;
  b->tag = o->tag;
  b->memory = o->memory;
  InterlockedIncrement(&b->memory->refs);
  b->frames = o->frames;
  b->frequency = o->frequency;
  b->volume = o->volume;
  b->pan = o->pan;
  b->params = o->params;
  b->deferred = o->params;
  b->next = d->buffers;
  d->buffers = b;
  LeaveCriticalSection(&d->lock);
  *out = (LPDIRECTSOUNDBUFFER)&b->buffer;
  return DS_OK;
}
static HRESULT WINAPI d_set_cooperative_level(IDirectSound8 *i, HWND window, DWORD level) {
  Device *d = from_device(i);
  (void)window;
  if (level < DSSCL_NORMAL || level > DSSCL_WRITEPRIMARY) return DSERR_INVALIDPARAM;
  if (!d->initialized) return DSERR_UNINITIALIZED;
  EnterCriticalSection(&d->lock);
  if (d->level == DSSCL_WRITEPRIMARY && level != DSSCL_WRITEPRIMARY && d->primary && d->primary->playing) {
    d->primary->playing = FALSE;
    unlink_locked(d, d->primary);
  }
  d->level = level;
  LeaveCriticalSection(&d->lock);
  return DS_OK;
}
static HRESULT WINAPI d_compact(IDirectSound8 *i) {
  Device *d = from_device(i);
  if (!d->initialized) return DSERR_UNINITIALIZED;
  return d->level < DSSCL_PRIORITY ? DSERR_PRIOLEVELNEEDED : DS_OK;
}
static HRESULT WINAPI d_get_speaker_config(IDirectSound8 *i, LPDWORD config) {
  Device *d = from_device(i);
  if (!config) return DSERR_INVALIDPARAM;
  if (!d->initialized) return DSERR_UNINITIALIZED;
  *config = d->speaker_config;
  return DS_OK;
}
static HRESULT WINAPI d_set_speaker_config(IDirectSound8 *i, DWORD config) {
  Device *d = from_device(i);
  if (!d->initialized) return DSERR_UNINITIALIZED;
  d->speaker_config = config;
  return DS_OK;
}
static HRESULT WINAPI d_initialize(IDirectSound8 *i, LPCGUID guid) {
  Device *d = from_device(i);
  if (d->initialized) return DSERR_ALREADYINITIALIZED;
  /* the speakers, or a virtual sink by its GUID (DirectSoundEnumerate lists them) */
  const ULONG64 target = playback_guid(guid) ? 0 : tcos_audio_virtual_node(guid);
  if (!playback_guid(guid) && !target) return DSERR_NODRIVER;
  if (!tcos_audio_devices()) return DSERR_NODRIVER;
  d->target = target;
  d->thread = CreateThread(NULL, 0, mixer_thread, d, 0, NULL);
  if (!d->thread) return DSERR_OUTOFMEMORY;
  d->initialized = TRUE;
  return DS_OK;
}
static HRESULT WINAPI d_verify_certification(IDirectSound8 *i, LPDWORD certified) {
  Device *d = from_device(i);
  if (!certified) return DSERR_INVALIDPARAM;
  if (!d->initialized) return DSERR_UNINITIALIZED;
  *certified = DS_CERTIFIED;
  return DS_OK;
}
static const IDirectSound8Vtbl kDeviceVtbl = {
  d_query, d_add_ref, d_release, d_create_sound_buffer, d_get_caps, d_duplicate, d_set_cooperative_level, d_compact, d_get_speaker_config,
  d_set_speaker_config, d_initialize, d_verify_certification};

/* ---- exports ------------------------------------------------------------------------------------------------------------------------------------ */

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved) {
  (void)instance;
  (void)reason;
  (void)reserved;
  return TRUE;
}

static HRESULT create(const GUID *guid, void **out, IUnknown *outer) {
  if (!out) return DSERR_INVALIDPARAM;
  *out = NULL;
  if (outer) return DSERR_NOAGGREGATION;
  Device *d = new_device();
  if (!d) return DSERR_OUTOFMEMORY;
  const HRESULT hr = d_initialize(&d->iface, guid);
  if (FAILED(hr)) {
    d_release(&d->iface);
    return hr;
  }
  *out = &d->iface;
  return DS_OK;
}

EXPORT HRESULT WINAPI DirectSoundCreate(LPCGUID guid, LPDIRECTSOUND *out, LPUNKNOWN outer) { return create(guid, (void **)out, outer); }
EXPORT HRESULT WINAPI DirectSoundCreate8(LPCGUID guid, LPDIRECTSOUND8 *out, LPUNKNOWN outer) { return create(guid, (void **)out, outer); }

static const char kDescriptionA[] = "Speakers (TheoryCraftOS)";
static const char kPrimaryA[] = "Primary Sound Driver";
static const char kModuleA[] = "{0.0.0.00000000}.{5af3a3c2-6e1b-4b4e-8d2f-1c0a7e5d9b30}";

/* The virtual sink number `which`: its GUID, description and module (its WASAPI endpoint id); FALSE past the last. */
static BOOL virtual_device(ULONG64 which, GUID *guid, char *description, int size, char *module) {
  ULONG64 count = 0;
  const ULONG64 node = tcos_audio_virtual_sink(which, description, size, &count);
  if (!node) return FALSE;
  tcos_audio_virtual_guid(node, guid);
  static const char hex[] = "0123456789abcdef";
  const char prefix[] = "{0.0.0.00000000}.{5af3a3c2-6e1b-4b4e-8d2f-";
  int k = 0;
  for (; prefix[k]; ++k) module[k] = prefix[k];
  for (int d = 11; d >= 0; --d) module[k++] = hex[(node >> (4 * d)) & 0xF];
  module[k++] = '}';
  module[k] = 0;
  return TRUE;
}

EXPORT HRESULT WINAPI DirectSoundEnumerateA(LPDSENUMCALLBACKA callback, LPVOID context) {
  if (!callback) return DSERR_INVALIDPARAM;
  if (!tcos_audio_devices()) return DS_OK;
  if (!callback(NULL, kPrimaryA, "", context)) return DS_OK;
  if (!callback((LPGUID)&kDeviceGuid, kDescriptionA, kModuleA, context)) return DS_OK;
  /* the graph's virtual sinks, a device each */
  for (ULONG64 i = 0; i < TCOS_AUDIO_MAX_VIRTUAL; ++i) {
    GUID guid;
    char description[96], module[80];
    if (!virtual_device(i, &guid, description, sizeof description, module)) break;
    if (!callback(&guid, description, module, context)) break;
  }
  return DS_OK;
}
EXPORT HRESULT WINAPI DirectSoundEnumerateW(LPDSENUMCALLBACKW callback, LPVOID context) {
  if (!callback) return DSERR_INVALIDPARAM;
  if (!tcos_audio_devices()) return DS_OK;
  WCHAR primary[sizeof kPrimaryA], description[sizeof kDescriptionA], module[sizeof kModuleA], empty[1] = {0};
  for (SIZE_T k = 0; k < sizeof kPrimaryA; ++k) primary[k] = (WCHAR)kPrimaryA[k];
  for (SIZE_T k = 0; k < sizeof kDescriptionA; ++k) description[k] = (WCHAR)kDescriptionA[k];
  for (SIZE_T k = 0; k < sizeof kModuleA; ++k) module[k] = (WCHAR)kModuleA[k];
  if (!callback(NULL, primary, empty, context)) return DS_OK;
  if (!callback((LPGUID)&kDeviceGuid, description, module, context)) return DS_OK;
  for (ULONG64 i = 0; i < TCOS_AUDIO_MAX_VIRTUAL; ++i) {
    GUID guid;
    char da[96], ma[80];
    WCHAR dw[96], mw[80];
    if (!virtual_device(i, &guid, da, sizeof da, ma)) break;
    int k = 0;
    for (; da[k] && k < 95; ++k) dw[k] = (WCHAR)(unsigned char)da[k];
    dw[k] = 0;
    for (k = 0; ma[k] && k < 79; ++k) mw[k] = (WCHAR)ma[k];
    mw[k] = 0;
    if (!callback(&guid, dw, mw, context)) break;
  }
  return DS_OK;
}

#include "capture.inc"

EXPORT HRESULT WINAPI GetDeviceID(LPCGUID source, LPGUID dest) {
  if (!source || !dest) return DSERR_INVALIDPARAM;
  if (playback_guid(source) && !same_guid(source, &kGuidNull)) {
    if (!tcos_audio_devices()) return DSERR_NODRIVER;
    *dest = kDeviceGuid;
    return DS_OK;
  }
  if (tcos_audio_virtual_node(source)) {      /* (a virtual sink's GUID is its own id) */
    *dest = *source;
    return DS_OK;
  }
  if (capture_guid(source) && !same_guid(source, &kGuidNull)) {
    if (!tcos_audio_input_devices()) return DSERR_NODRIVER;
    *dest = kCaptureGuid;
    return DS_OK;
  }
  return DSERR_INVALIDPARAM;
}

/* ---- the class factory -------------------------------------------------------------------------------------------------------------------------- */

typedef struct {
  IClassFactory iface;
  int kind;      /* 0 DirectSound, 1 DirectSoundCapture, 2 DirectSoundFullDuplex */
} Factory;

static HRESULT WINAPI f_query(IClassFactory *i, REFIID riid, void **out) {
  if (!out) return E_POINTER;
  *out = NULL;
  if (same_guid(riid, &kIidUnknown) || same_guid(riid, &kIidClassFactory)) {
    *out = i;
    return S_OK;
  }
  return E_NOINTERFACE;
}
static ULONG WINAPI f_add_ref(IClassFactory *i) { (void)i; return 2; }
static ULONG WINAPI f_release(IClassFactory *i) { (void)i; return 1; }
static HRESULT WINAPI f_create_instance(IClassFactory *i, IUnknown *outer, REFIID riid, void **out) {
  if (!out) return E_POINTER;
  *out = NULL;
  if (outer) return CLASS_E_NOAGGREGATION;
  /* (not initialized: the program calls Initialize) */
  const int kind = ((Factory *)i)->kind;
  if (kind == 1) {
    CaptureDevice *c = new_capture_device();
    if (!c) return E_OUTOFMEMORY;
    const HRESULT hr = c_query(&c->iface, riid, out);
    c_release(&c->iface);
    return hr;
  }
  if (kind == 2) {
    FullDuplex *x = new_duplex();
    if (!x) return E_OUTOFMEMORY;
    const HRESULT hr = x_query(&x->iface, riid, out);
    x_release(&x->iface);
    return hr;
  }
  Device *d = new_device();
  if (!d) return E_OUTOFMEMORY;
  const HRESULT hr = d_query(&d->iface, riid, out);
  d_release(&d->iface);
  return hr;
}
static HRESULT WINAPI f_lock_server(IClassFactory *i, BOOL lock) { (void)i; (void)lock; return S_OK; }
static const IClassFactoryVtbl kFactoryVtbl = {f_query, f_add_ref, f_release, f_create_instance, f_lock_server};
static Factory g_factory = {{(IClassFactoryVtbl *)&kFactoryVtbl}, 0};
static Factory g_capture_factory = {{(IClassFactoryVtbl *)&kFactoryVtbl}, 1};
static Factory g_duplex_factory = {{(IClassFactoryVtbl *)&kFactoryVtbl}, 2};

EXPORT HRESULT WINAPI DllGetClassObject(REFCLSID clsid, REFIID riid, LPVOID *out) {
  if (!out) return E_POINTER;
  *out = NULL;
  if (same_guid(clsid, &CLSID_DirectSound) || same_guid(clsid, &CLSID_DirectSound8)) return f_query(&g_factory.iface, riid, out);
  if (same_guid(clsid, &CLSID_DirectSoundCapture) || same_guid(clsid, &CLSID_DirectSoundCapture8)) return f_query(&g_capture_factory.iface, riid, out);
  if (same_guid(clsid, &CLSID_DirectSoundFullDuplex)) return f_query(&g_duplex_factory.iface, riid, out);
  return CLASS_E_CLASSNOTAVAILABLE;
}
EXPORT HRESULT WINAPI DllCanUnloadNow(void) { return S_FALSE; }
