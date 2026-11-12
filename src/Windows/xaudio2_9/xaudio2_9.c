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

/* xaudio2_9.dll for TheoryCraftOS: XAudio2 2.9 (and, built from the same source, 2.8) over the kernel's PipeWire graph (NtTcosAudio,
 * src/Process/WindowsAudio.hpp, tcos_audio.h). Most games since 2008 play through it.
 *
 * Each IXAudio2 has one stream of the graph in its mastering voice's format (32-bit float, the mastering voice's channels and rate; 2 channels at
 * 48 kHz by default), where it is mixed with every other program's sound. An engine thread runs the voice graph a quantum (10 ms) at a time, keeping
 * the kernel's stream 20 to 100 ms ahead of what has been mixed (further when it is woken late):
 *   - source voices read their queued buffers (PCM 8/16/24/32-bit, 32-bit float, MS-ADPCM decoded when submitted; loops, PlayBegin/Length,
 *     LoopBegin/Length/Count, ExitLoop, END_OF_STREAM) at their sample rate times the frequency ratio (linear interpolation), then the voice's
 *     filter (state-variable low/band/high pass and notch, one-pole low and high pass), its effect chain (XAPOs, the program's own or the ones
 *     CreateAudioVolumeMeter/CreateAudioReverb/CreateFX make), its volume and channel volumes, and each send's output matrix (and its filter)
 *     into the destination voices' inputs;
 *   - submix voices do the same with what was mixed into them, in the order of their processing stage;
 *   - the mastering voice's effects and volume, then the stream.
 * The callbacks (IXAudio2EngineCallback, IXAudio2VoiceCallback: pass start/end, buffer start/end, loop end, stream end) are called on the engine
 * thread; the engine's lock is re-entrant, so a callback may submit buffers or change voices. Operation sets defer Start, Stop, the volumes, matrices,
 * filters, frequency ratio, ExitLoop and effect changes until CommitChanges. X3DAudio (x3daudio.inc) is exported here too, as in Windows.
 * Not done: xWMA and XMA sources (refused), PLAY_TAILS (a stopped voice stops at once), the XAudio2 2.7 COM classes. Built freestanding over kernel32.dll
 * and ntdll.dll. */
#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#include <windows.h>
#include <mmsystem.h>
#include <mmreg.h>
#include <initguid.h>
#include <xaudio2.h>
#include <xaudio2fx.h>
#include <xapo.h>
#include <xapofx.h>
#include <x3daudio.h>
#include <ksmedia.h>
#include "../tcos_audio.h"

#define EXPORT __declspec(dllexport)

__asm__(".text\n.globl ___chkstk_ms\n___chkstk_ms:\n ret\n");

/* (what mingw's xaudio2.h leaves out) */
#ifndef XAUDIO2_E_INVALID_CALL
#define XAUDIO2_E_INVALID_CALL ((HRESULT)0x88960001)
#define XAUDIO2_E_XMA_DECODER_ERROR ((HRESULT)0x88960002)
#define XAUDIO2_E_XAPO_CREATION_FAILED ((HRESULT)0x88960003)
#define XAUDIO2_E_DEVICE_INVALIDATED ((HRESULT)0x88960004)
#endif
#define VOICE_NOPITCH 0x2
#define VOICE_NOSRC 0x4
#define VOICE_USEFILTER 0x8
#define PLAY_TAILS 0x20
#define END_OF_STREAM 0x40
#define SEND_USEFILTER 0x80
#define VOICE_NOSAMPLESPLAYED 0x100
#define FILTER_LOWPASS_ONEPOLE 4
#define FILTER_HIGHPASS_ONEPOLE 5
#define MAX_CHANNELS 64
#define MAX_SENDS 16
#define MAX_EFFECTS 8
#define MAX_QUEUED 64
#define LOOP_INFINITE 255
#define DEFAULT_RATE 48000
#define AHEAD_MIN_MS 20
#define AHEAD_MAX_MS 100

static const GUID kIidUnknown = {0x00000000, 0x0000, 0x0000, {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
static const GUID kIidXAudio2_8 = {0x60d8dac8, 0x5aa1, 0x4e8e, {0xb5, 0x97, 0x2f, 0x5e, 0x28, 0x83, 0xd4, 0x84}};

static BOOL same_guid(const GUID *a, const GUID *b) {
  const BYTE *x = (const BYTE *)a, *y = (const BYTE *)b;
  for (SIZE_T i = 0; i < sizeof(GUID); ++i) if (x[i] != y[i]) return FALSE;
  return TRUE;
}
/* (eight bytes at a time where both ends allow it: the engine clears and copies a few kilobytes every pass. The attribute keeps gcc from turning the
 * loops into memset/memcpy calls, which this library has no C runtime for.) */
__attribute__((optimize("no-tree-loop-distribute-patterns"))) static void zero(void *p, SIZE_T n) {
  BYTE *b = (BYTE *)p;
  SIZE_T i = 0;
  if (((ULONG_PTR)b & 7) == 0)
    for (; i + 8 <= n; i += 8) *(UINT64 *)(b + i) = 0;
  for (; i < n; ++i) b[i] = 0;
}
__attribute__((optimize("no-tree-loop-distribute-patterns"))) static void copy(void *to, const void *from, SIZE_T n) {
  BYTE *t = (BYTE *)to;
  const BYTE *f = (const BYTE *)from;
  SIZE_T i = 0;
  if ((((ULONG_PTR)t | (ULONG_PTR)f) & 7) == 0)
    for (; i + 8 <= n; i += 8) *(UINT64 *)(t + i) = *(const UINT64 *)(f + i);
  for (; i < n; ++i) t[i] = f[i];
}
static void *alloc(SIZE_T n) { return HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, n ? n : 1); }
static void release_memory(void *p) { if (p) HeapFree(GetProcessHeap(), 0, p); }

/* ---- the objects ------------------------------------------------------------------------------------------------------------------------------- */

typedef enum { KIND_SOURCE, KIND_SUBMIX, KIND_MASTER } Kind;
typedef struct Engine Engine;
typedef struct Voice Voice;

typedef struct {
  Voice *to;
  UINT32 flags;
  float *matrix;                       /* out_channels x the destination's channels, row = destination channel */
  XAUDIO2_FILTER_PARAMETERS filter;
  float low[MAX_CHANNELS], band[MAX_CHANNELS];      /* the send filter's state, per destination channel */
} Send;

typedef struct {
  IUnknown *object;
  IXAPO *xapo;
  IXAPOParameters *parameters;
  BOOL enabled, locked;
  UINT32 in_channels, out_channels;
} Effect;

typedef struct {
  XAUDIO2_BUFFER b;
  BYTE *decoded;                       /* (MS-ADPCM: the whole buffer as 16-bit PCM) */
  UINT32 frames;                       /* frames in the buffer */
  UINT32 at;                           /* the next frame to read */
  UINT32 end;                          /* where reading stops (PlayBegin + PlayLength), or the loop's end while it loops */
  UINT32 loops_left;
  BOOL started;
} Queued;

struct Voice {
  union {
    IXAudio2SourceVoice source;
    IXAudio2SubmixVoice submix;
    IXAudio2MasteringVoice master;
    IXAudio2Voice base;
  } iface;                             /* (first: a voice's pointer is its interface) */
  Engine *engine;
  Kind kind;
  Voice *next;
  BOOL dead;                           /* destroyed from a callback: freed after the pass */
  UINT32 flags, channels, rate, out_channels, stage;
  float volume;
  float channel_volumes[MAX_CHANNELS];
  Send sends[MAX_SENDS];
  UINT32 send_count;
  Effect effects[MAX_EFFECTS];
  UINT32 effect_count;
  XAUDIO2_FILTER_PARAMETERS filter;
  float low[MAX_CHANNELS], band[MAX_CHANNELS];
  float *input;                        /* submix and mastering voices: what was mixed into them this pass */
  float *work, *work2;                 /* a pass's frames as they go through the voice */
  UINT32 work_frames;
  /* a source voice */
  WORD tag, bits, block;
  UINT32 src_channels, src_rate;
  WORD adpcm_samples_per_block, adpcm_coefficients;
  short adpcm_coef[2 * 32];
  float max_ratio, ratio;
  IXAudio2VoiceCallback *callback;
  Queued queue[MAX_QUEUED];
  UINT32 head, count;
  BOOL started;
  float s0[MAX_CHANNELS], s1[MAX_CHANNELS];      /* the two source frames being interpolated between */
  UINT64 frac;                          /* where between s0 and s1: a fraction of 2^32 */
  float *window;                        /* a pass's source frames (produce) */
  UINT32 window_frames;
  BOOL primed, starved;
  UINT64 samples_played;
};

typedef struct Op Op;
struct Op {
  Op *next;
  UINT32 set;
  Voice *voice;
  int kind;
  UINT32 a, b;
  float f;
  Voice *to;
  void *data;
  UINT32 size;
};
enum { OP_START, OP_STOP, OP_VOLUME, OP_CHANNEL_VOLUMES, OP_MATRIX, OP_FILTER, OP_OUTPUT_FILTER, OP_RATIO, OP_EXIT_LOOP, OP_ENABLE, OP_DISABLE,
       OP_EFFECT_PARAMETERS };

struct Engine {
  IXAudio2 iface;
  LONG refs;
  CRITICAL_SECTION lock;
  Voice *voices;                       /* every voice; the mastering voice is `master` */
  Voice *master;
  IXAudio2EngineCallback *callbacks[16];
  UINT32 callback_count;
  Op *ops;
  ULONG64 stream;
  UINT32 rate, channels;
  float *output;                       /* a pass of the master's output, interleaved */
  BOOL running;
  HANDLE thread, wake;
  DWORD thread_id;
  volatile LONG stop;
  ULONG64 last_tick_us, gap_frames;
  UINT32 glitches;
  ULONG64 passes;
  ULONG64 audio_us, queried_us;        /* time spent in passes, and when GetPerformanceData last asked (its cycles are microseconds here) */
  UINT32 min_pass_us, max_pass_us;
};

static const IXAudio2Vtbl kEngineVtbl;

static Voice *from_voice(void *i) { return (Voice *)i; }
static Engine *from_engine(IXAudio2 *i) { return (Engine *)i; }
static UINT32 quantum_of(UINT32 rate) { UINT32 q = rate / 100; return q ? q : 1; }

/* ---- source formats ---------------------------------------------------------------------------------------------------------------------------- */

static float read_sample(const Voice *v, const BYTE *p) {
  if (v->tag == WAVE_FORMAT_IEEE_FLOAT) {
    float f;
    copy(&f, p, 4);
    return f == f ? f : 0.0f;      /* (NaN: silence) */
  }
  switch (v->bits) {
    case 8: return ((int)p[0] - 128) / 128.0f;
    case 16: return (short)(p[0] | p[1] << 8) / 32768.0f;
    case 24: return (float)((int)((UINT32)p[0] << 8 | (UINT32)p[1] << 16 | (UINT32)p[2] << 24) >> 8) / 8388608.0f;
    default: return (float)(int)((UINT32)p[0] | (UINT32)p[1] << 8 | (UINT32)p[2] << 16 | (UINT32)p[3] << 24) / 2147483648.0f;
  }
}

/* MS-ADPCM (WAVE_FORMAT_ADPCM): `in` (whole blocks) into 16-bit PCM; the frames made. */
static const int kAdaptation[16] = {230, 230, 230, 230, 307, 409, 512, 614, 768, 614, 512, 409, 307, 230, 230, 230};
static UINT32 decode_adpcm(const Voice *v, const BYTE *in, UINT32 bytes, short *out) {
  const UINT32 ch = v->src_channels, per_block = v->adpcm_samples_per_block, block = v->block;
  UINT32 frames = 0;
  for (UINT32 off = 0; off + block <= bytes; off += block) {
    const BYTE *b = in + off;
    int predictor[2], delta[2], s1[2], s2[2];
    UINT32 at = 0;
    for (UINT32 c = 0; c < ch; ++c) predictor[c] = b[at++] % v->adpcm_coefficients;
    for (UINT32 c = 0; c < ch; ++c) { delta[c] = (short)(b[at] | b[at + 1] << 8); at += 2; }
    for (UINT32 c = 0; c < ch; ++c) { s1[c] = (short)(b[at] | b[at + 1] << 8); at += 2; }
    for (UINT32 c = 0; c < ch; ++c) { s2[c] = (short)(b[at] | b[at + 1] << 8); at += 2; }
    short *o = out + (SIZE_T)frames * ch;
    for (UINT32 c = 0; c < ch; ++c) o[c] = (short)s2[c];
    for (UINT32 c = 0; c < ch; ++c) o[ch + c] = (short)s1[c];
    UINT32 made = 2, nibble = 0;
    while (made < per_block && at < block) {
      for (UINT32 c = 0; c < ch && at < block; ++c) {
        const int code = nibble ? b[at] & 0x0F : b[at] >> 4;
        if (nibble) ++at;
        nibble ^= 1;
        const int signed_code = code & 0x8 ? code - 16 : code;
        const int p = predictor[c];
        int sample = (s1[c] * v->adpcm_coef[2 * p] + s2[c] * v->adpcm_coef[2 * p + 1]) / 256 + signed_code * delta[c];
        sample = sample > 32767 ? 32767 : sample < -32768 ? -32768 : sample;
        s2[c] = s1[c];
        s1[c] = sample;
        delta[c] = delta[c] * kAdaptation[code] / 256;
        if (delta[c] < 16) delta[c] = 16;
        o[(SIZE_T)made * ch + c] = (short)sample;
      }
      ++made;
    }
    frames += made;
  }
  return frames;
}

/* ---- default matrices ------------------------------------------------------------------------------------------------------------------------- */

/* XAudio2's default output matrix from `from` to `to` channels: the same channels where both have them, mono to both front channels, more
 * channels folded into fewer (front centre at 0.707 to both sides, the rear and side channels to their side, LFE dropped), anything into mono averaged. */
static void default_matrix(float *m, UINT32 from, UINT32 to) {
  for (UINT32 i = 0; i < from * to; ++i) m[i] = 0;
  if (to == 1) {
    for (UINT32 s = 0; s < from; ++s) m[s] = 1.0f / (float)from;
    return;
  }
  if (from == 1) {
    m[0 * from] = 1.0f;
    m[1 * from] = 1.0f;
    return;
  }
  if (to == 2 && from > 2) {
    /* (the usual order: FL FR FC LFE BL BR SL SR) */
    for (UINT32 s = 0; s < from; ++s) {
      float l = 0, r = 0;
      switch (s) {
        case 0: l = 1; break;
        case 1: r = 1; break;
        case 2: l = r = 0.7071f; break;
        case 3: break;
        default: if (s % 2 == 0) l = 0.7071f; else r = 0.7071f; break;
      }
      m[0 * from + s] = l;
      m[1 * from + s] = r;
    }
    return;
  }
  for (UINT32 c = 0; c < from && c < to; ++c) m[c * from + c] = 1.0f;
}

/* ---- the filter: XAudio2's state-variable filter (Frequency = 2 sin(pi f / rate)) and the one-pole ones ---------------------------------------- */

static BOOL filter_bypassed(const XAUDIO2_FILTER_PARAMETERS *f) {
  return (f->Type == LowPassFilter && f->Frequency >= 1.0f) || (f->Type == FILTER_LOWPASS_ONEPOLE && f->Frequency >= 1.0f) ||
         ((f->Type == HighPassFilter || f->Type == FILTER_HIGHPASS_ONEPOLE) && f->Frequency <= 0.0f);
}
static void run_filter(const XAUDIO2_FILTER_PARAMETERS *f, float *low, float *band, float *samples, UINT32 frames, UINT32 channels) {
  if (filter_bypassed(f)) return;
  const float F = f->Frequency, Q = f->OneOverQ;
  for (UINT32 c = 0; c < channels; ++c) {
    float l = low[c], b = band[c];
    for (UINT32 i = 0; i < frames; ++i) {
      float *x = &samples[(SIZE_T)i * channels + c];
      if (f->Type == FILTER_LOWPASS_ONEPOLE || f->Type == FILTER_HIGHPASS_ONEPOLE) {
        l += F * (*x - l);
        *x = f->Type == FILTER_LOWPASS_ONEPOLE ? l : *x - l;
        continue;
      }
      l += F * b;
      const float h = *x - l - Q * b;
      b += F * h;
      switch ((int)f->Type) {
        case LowPassFilter: *x = l; break;
        case BandPassFilter: *x = b; break;
        case HighPassFilter: *x = h; break;
        default: *x = l + h; break;      /* notch */
      }
    }
    /* (a filter that blew up, from parameters at the edge of what it takes: start again silent rather than spread NaNs) */
    if (!(l == l) || !(b == b)) l = b = 0;
    low[c] = l;
    band[c] = b;
  }
}

/* ---- voices: common parts --------------------------------------------------------------------------------------------------------------------- */

static Engine *engine_of(Voice *v) { return v->engine; }

static BOOL ensure_work(Voice *v, UINT32 frames) {
  if (v->work_frames >= frames) return TRUE;
  release_memory(v->work);
  release_memory(v->work2);
  v->work = (float *)alloc((SIZE_T)frames * MAX_CHANNELS * sizeof(float));
  v->work2 = (float *)alloc((SIZE_T)frames * MAX_CHANNELS * sizeof(float));
  v->work_frames = v->work && v->work2 ? frames : 0;
  return v->work_frames != 0;
}

/* The channels a voice sends out: its last effect's, or its input's. */
static UINT32 output_channels(const Voice *v) {
  UINT32 ch = v->channels;
  for (UINT32 e = 0; e < v->effect_count; ++e) ch = v->effects[e].out_channels;
  return ch;
}

static void free_sends(Voice *v) {
  for (UINT32 s = 0; s < v->send_count; ++s) release_memory(v->sends[s].matrix);
  v->send_count = 0;
}

/* Sends to `list` (NULL: the mastering voice). The destinations must take the voice's output rate, come later in the processing order, and not be
 * the voice itself. */
static HRESULT set_sends_locked(Voice *v, const XAUDIO2_VOICE_SENDS *list) {
  Engine *e = engine_of(v);
  if (v->kind == KIND_MASTER) return list && list->SendCount ? XAUDIO2_E_INVALID_CALL : S_OK;
  Send sends[MAX_SENDS];
  UINT32 count = 0;
  if (!list) {
    if (!e->master) return XAUDIO2_E_INVALID_CALL;
    zero(&sends[0], sizeof sends[0]);
    sends[0].to = e->master;
    count = 1;
  } else {
    if (list->SendCount > MAX_SENDS || (list->SendCount && !list->pSends)) return E_INVALIDARG;
    for (UINT32 s = 0; s < list->SendCount; ++s) {
      Voice *to = from_voice(list->pSends[s].pOutputVoice);
      if (!to || to == v || to->engine != e || to->kind == KIND_SOURCE) return E_INVALIDARG;
      if (to->kind == KIND_SUBMIX && v->kind == KIND_SUBMIX && to->stage <= v->stage) return XAUDIO2_E_INVALID_CALL;
      for (UINT32 k = 0; k < count; ++k) if (sends[k].to == to) return E_INVALIDARG;
      zero(&sends[count], sizeof sends[count]);
      sends[count].to = to;
      sends[count].flags = list->pSends[s].Flags;
      ++count;
    }
  }
  const UINT32 out_rate = v->kind == KIND_SOURCE ? 0 : v->rate;
  for (UINT32 s = 0; s < count; ++s) {
    if (s > 0 && v->kind == KIND_SOURCE && sends[s].to->rate != sends[0].to->rate) return XAUDIO2_E_INVALID_CALL;      /* (one output rate) */
    if (out_rate && sends[s].to->rate != out_rate) return XAUDIO2_E_INVALID_CALL;      /* (only source voices convert rates) */
    if ((v->flags & VOICE_NOSRC) && sends[s].to->rate != v->src_rate) return XAUDIO2_E_INVALID_CALL;
  }
  const UINT32 out = output_channels(v);
  for (UINT32 s = 0; s < count; ++s) {
    sends[s].matrix = (float *)alloc((SIZE_T)out * sends[s].to->channels * sizeof(float));
    if (!sends[s].matrix) {
      for (UINT32 k = 0; k < s; ++k) release_memory(sends[k].matrix);
      return E_OUTOFMEMORY;
    }
    default_matrix(sends[s].matrix, out, sends[s].to->channels);
    sends[s].filter.Type = LowPassFilter;
    sends[s].filter.Frequency = 1.0f;
    sends[s].filter.OneOverQ = 1.0f;
  }
  free_sends(v);
  for (UINT32 s = 0; s < count; ++s) v->sends[s] = sends[s];
  v->send_count = count;
  v->out_channels = out;
  return S_OK;
}

static Send *send_to(Voice *v, Voice *to) {
  if (!to && v->send_count == 1) return &v->sends[0];
  for (UINT32 s = 0; s < v->send_count; ++s) if (v->sends[s].to == to) return &v->sends[s];
  return NULL;
}

/* ---- effect chains ---------------------------------------------------------------------------------------------------------------------------- */

static void release_effect(Effect *f) {
  if (f->locked) IXAPO_UnlockForProcess(f->xapo);
  if (f->parameters) IXAPOParameters_Release(f->parameters);
  if (f->xapo) IXAPO_Release(f->xapo);
  if (f->object) IUnknown_Release(f->object);
  zero(f, sizeof *f);
}

/* Takes up one effect of a chain: its XAPO locked for 32-bit float at `rate`, `channels` in. */
static HRESULT take_effect(Effect *f, const XAUDIO2_EFFECT_DESCRIPTOR *d, UINT32 channels, UINT32 rate) {
  zero(f, sizeof *f);
  if (!d->pEffect || d->OutputChannels == 0 || d->OutputChannels > MAX_CHANNELS) return E_INVALIDARG;
  if (FAILED(IUnknown_QueryInterface(d->pEffect, &IID_IXAPO, (void **)&f->xapo))) {
    f->xapo = NULL;
    return XAUDIO2_E_XAPO_CREATION_FAILED;
  }
  if (FAILED(IUnknown_QueryInterface(d->pEffect, &IID_IXAPOParameters, (void **)&f->parameters))) f->parameters = NULL;
  f->object = d->pEffect;
  IUnknown_AddRef(f->object);
  f->enabled = d->InitialState;
  f->in_channels = channels;
  f->out_channels = d->OutputChannels;
  WAVEFORMATEX in_f, out_f;
  zero(&in_f, sizeof in_f);
  in_f.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
  in_f.nChannels = (WORD)channels;
  in_f.nSamplesPerSec = rate;
  in_f.wBitsPerSample = 32;
  in_f.nBlockAlign = (WORD)(4 * channels);
  in_f.nAvgBytesPerSec = rate * 4 * channels;
  out_f = in_f;
  out_f.nChannels = (WORD)d->OutputChannels;
  out_f.nBlockAlign = (WORD)(4 * d->OutputChannels);
  out_f.nAvgBytesPerSec = rate * 4 * d->OutputChannels;
  const XAPO_LOCKFORPROCESS_BUFFER_PARAMETERS lin = {&in_f, quantum_of(rate)}, lout = {&out_f, quantum_of(rate)};
  if (FAILED(IXAPO_LockForProcess(f->xapo, 1, &lin, 1, &lout))) {
    release_effect(f);
    return XAUDIO2_E_XAPO_CREATION_FAILED;
  }
  f->locked = TRUE;
  return S_OK;
}

static void free_effects(Voice *v) {
  for (UINT32 k = 0; k < v->effect_count; ++k) release_effect(&v->effects[k]);
  v->effect_count = 0;
}

static HRESULT set_effects_locked(Voice *v, const XAUDIO2_EFFECT_CHAIN *chain) {
  Effect effects[MAX_EFFECTS];
  UINT32 count = 0, ch = v->channels;
  const UINT32 rate = v->kind == KIND_SOURCE ? (v->send_count ? v->sends[0].to->rate : engine_of(v)->rate) : v->rate;
  if (chain) {
    if (chain->EffectCount > MAX_EFFECTS || (chain->EffectCount && !chain->pEffectDescriptors)) return E_INVALIDARG;
    for (; count < chain->EffectCount; ++count) {
      const HRESULT hr = take_effect(&effects[count], &chain->pEffectDescriptors[count], ch, rate);
      if (FAILED(hr)) {
        for (UINT32 j = 0; j < count; ++j) release_effect(&effects[j]);
        return hr;
      }
      ch = effects[count].out_channels;
    }
  }
  free_effects(v);
  for (UINT32 k = 0; k < count; ++k) v->effects[k] = effects[k];
  v->effect_count = count;
  /* the sends' matrices follow the new output channels */
  if (v->kind != KIND_MASTER && output_channels(v) != v->out_channels && v->send_count) {
    XAUDIO2_SEND_DESCRIPTOR d[MAX_SENDS];
    for (UINT32 s = 0; s < v->send_count; ++s) {
      d[s].Flags = v->sends[s].flags;
      d[s].pOutputVoice = &v->sends[s].to->iface.base;
    }
    XAUDIO2_VOICE_SENDS list = {v->send_count, d};
    return set_sends_locked(v, &list);
  }
  v->out_channels = output_channels(v);
  return S_OK;
}

/* Runs the chain over `frames` frames in v->work (v->channels wide); the result is in v->work, v->out_channels wide. */
static void run_effects(Voice *v, UINT32 frames) {
  UINT32 ch = v->channels;
  for (UINT32 k = 0; k < v->effect_count; ++k) {
    Effect *f = &v->effects[k];
    XAPO_PROCESS_BUFFER_PARAMETERS in = {v->work, XAPO_BUFFER_VALID, frames};
    if (f->out_channels == ch) {
      IXAPO_Process(f->xapo, 1, &in, 1, &in, f->enabled);
    } else {
      XAPO_PROCESS_BUFFER_PARAMETERS out = {v->work2, XAPO_BUFFER_VALID, frames};
      IXAPO_Process(f->xapo, 1, &in, 1, &out, f->enabled);
      copy(v->work, v->work2, (SIZE_T)frames * f->out_channels * sizeof(float));
      ch = f->out_channels;
    }
  }
}

/* ---- source voices: the buffer queue ----------------------------------------------------------------------------------------------------------- */

static UINT32 frame_bytes(const Voice *v) { return v->tag == WAVE_FORMAT_ADPCM ? 2 * v->src_channels : v->block; }


static void pop_buffer(Voice *v, BOOL report) {
  Queued *q = &v->queue[v->head];
  void *context = q->b.pContext;
  const BOOL eos = (q->b.Flags & END_OF_STREAM) != 0;
  release_memory(q->decoded);
  zero(q, sizeof *q);
  v->head = (v->head + 1) % MAX_QUEUED;
  --v->count;
  if (report && v->callback) {
    IXAudio2VoiceCallback_OnBufferEnd(v->callback, context);
    if (eos) IXAudio2VoiceCallback_OnStreamEnd(v->callback);
  }
}

/* The next source frame (src_channels samples, -1 .. 1) into `out`; FALSE when the queue is empty. */
static BOOL next_frame(Voice *v, float *out) {
  while (v->count) {
    Queued *q = &v->queue[v->head];
    if (!q->started) {
      q->started = TRUE;
      if (v->callback) IXAudio2VoiceCallback_OnBufferStart(v->callback, q->b.pContext);
    }
    if (q->at < q->end) {
      if (!q->decoded && v->tag == WAVE_FORMAT_IEEE_FLOAT) {
        const float *x = (const float *)q->b.pAudioData + (SIZE_T)q->at * v->src_channels;
        for (UINT32 c = 0; c < v->src_channels; ++c) out[c] = x[c] == x[c] ? x[c] : 0.0f;
        ++q->at;
        ++v->samples_played;
        return TRUE;
      }
      if (!q->decoded && v->tag == WAVE_FORMAT_PCM && v->bits == 16) {
        /* (the common case on its own: 16-bit PCM) */
        const short *x = (const short *)q->b.pAudioData + (SIZE_T)q->at * v->src_channels;
        for (UINT32 c = 0; c < v->src_channels; ++c) out[c] = x[c] * (1.0f / 32768.0f);
        ++q->at;
        ++v->samples_played;
        return TRUE;
      }
      const BYTE *data = q->decoded ? q->decoded : q->b.pAudioData;
      const BYTE *p = data + (SIZE_T)q->at * frame_bytes(v);
      if (q->decoded) {
        for (UINT32 c = 0; c < v->src_channels; ++c) out[c] = (short)(p[2 * c] | p[2 * c + 1] << 8) / 32768.0f;
      } else {
        const UINT32 sample = v->bits / 8;
        for (UINT32 c = 0; c < v->src_channels; ++c) out[c] = read_sample(v, p + (SIZE_T)c * sample);
      }
      ++q->at;
      ++v->samples_played;
      return TRUE;
    }
    /* the end of the loop region or of the buffer */
    const UINT32 play_end = q->b.PlayBegin + (q->b.PlayLength ? q->b.PlayLength : q->frames - q->b.PlayBegin);
    if (q->loops_left > 0 && q->b.LoopCount > 0) {
      if (q->loops_left != LOOP_INFINITE) --q->loops_left;
      if (v->callback) IXAudio2VoiceCallback_OnLoopEnd(v->callback, q->b.pContext);
      q->at = q->b.LoopBegin;
      if (q->loops_left == 0) q->end = play_end;
      continue;
    }
    if (q->end < play_end) {      /* (no more loops, ExitLoop: on to the end of the play region) */
      q->end = play_end;
      continue;
    }
    pop_buffer(v, TRUE);
  }
  return FALSE;
}

/* Up to `n` source frames (src_channels floats each) into `out`, a run of a buffer at a time; returns the frames read (fewer: the queue ran dry). */
static UINT32 read_frames(Voice *v, float *out, UINT32 n) {
  const UINT32 ch = v->src_channels;
  UINT32 done = 0;
  while (done < n && v->count) {
    Queued *q = &v->queue[v->head];
    if (q->at >= q->end || !q->started) {
      /* (a buffer's start, a loop's end or a buffer's end: the one-frame path deals with those) */
      if (!next_frame(v, out + (SIZE_T)done * ch)) break;
      ++done;
      continue;
    }
    UINT32 run = q->end - q->at;
    if (run > n - done) run = n - done;
    float *o = out + (SIZE_T)done * ch;
    const UINT32 samples = run * ch;
    if (q->decoded || (v->tag == WAVE_FORMAT_PCM && v->bits == 16)) {
      const short *x = (const short *)(q->decoded ? q->decoded : q->b.pAudioData) + (SIZE_T)q->at * ch;
      for (UINT32 k = 0; k < samples; ++k) o[k] = x[k] * (1.0f / 32768.0f);
    } else if (v->tag == WAVE_FORMAT_IEEE_FLOAT) {
      const float *x = (const float *)q->b.pAudioData + (SIZE_T)q->at * ch;
      for (UINT32 k = 0; k < samples; ++k) o[k] = x[k] == x[k] ? x[k] : 0.0f;
    } else {
      const BYTE *x = q->b.pAudioData + (SIZE_T)q->at * v->block;
      const UINT32 size = v->bits / 8;
      for (UINT32 k = 0; k < samples; ++k) o[k] = read_sample(v, x + (SIZE_T)k * size);
    }
    q->at += run;
    v->samples_played += run;
    done += run;
  }
  return done;
}

static BOOL ensure_source_window(Voice *v, UINT32 frames) {
  if (v->window_frames >= frames) return TRUE;
  release_memory(v->window);
  v->window = (float *)alloc((SIZE_T)frames * v->src_channels * sizeof(float));
  v->window_frames = v->window ? frames : 0;
  return v->window != NULL;
}

/* `frames` output frames of the source at `out_rate` (its rate times the frequency ratio, linear interpolation) into v->work; with `silent` it only
 * moves on (a voice nobody can hear: its position, callbacks and SamplesPlayed still go on). The source frames come in one batch: window[0] and [1]
 * are the two being interpolated between when the pass starts (s0, s1), the rest is read for this pass. */
static void produce(Voice *v, UINT32 frames, UINT32 out_rate, BOOL silent) {
  const UINT32 ch = v->src_channels;
  double ratio = (double)v->src_rate * (v->flags & VOICE_NOPITCH ? 1.0f : v->ratio) / (double)out_rate;
  if (v->flags & VOICE_NOSRC) ratio = 1.0;
  const UINT64 step = (UINT64)(ratio * 4294967296.0);
  if (!v->primed) {
    for (UINT32 c = 0; c < ch; ++c) v->s0[c] = v->s1[c] = 0;
    if (next_frame(v, v->s0)) next_frame(v, v->s1);
    v->frac = 0;
    v->primed = TRUE;
  }
  const UINT64 end = v->frac + step * frames;
  const UINT32 consumed = (UINT32)(end >> 32);      /* source frames passed this pass */
  if (!ensure_source_window(v, consumed + 2)) {
    if (!silent) zero(v->work, (SIZE_T)frames * ch * sizeof(float));      /* (out of memory: a pass of silence) */
    return;
  }
  float *w = v->window;
  for (UINT32 c = 0; c < ch; ++c) {
    w[c] = v->s0[c];
    w[ch + c] = v->s1[c];
  }
  if (consumed > 0) {
    const UINT32 got = read_frames(v, w + 2 * ch, consumed);
    if (got < consumed) {
      zero(w + (SIZE_T)(2 + got) * ch, (SIZE_T)(consumed - got) * ch * sizeof(float));
      v->starved = TRUE;
    }
  }
  if (!silent) {
    UINT64 pos = v->frac;
    float *o = v->work;
    if (ch == 1) {
      for (UINT32 i = 0; i < frames; ++i, pos += step) {
        const float *x = w + (pos >> 32);
        o[i] = x[0] + (x[1] - x[0]) * ((float)(UINT32)(pos >> 8 & 0xFFFFFF) * (1.0f / 16777216.0f));
      }
    } else if (ch == 2) {
      for (UINT32 i = 0; i < frames; ++i, pos += step) {
        const float *x = w + 2 * (pos >> 32);
        const float t = (float)(UINT32)(pos >> 8 & 0xFFFFFF) * (1.0f / 16777216.0f);
        o[2 * i] = x[0] + (x[2] - x[0]) * t;
        o[2 * i + 1] = x[1] + (x[3] - x[1]) * t;
      }
    } else {
      for (UINT32 i = 0; i < frames; ++i, pos += step) {
        const float *x = w + (SIZE_T)ch * (pos >> 32);
        const float t = (float)(UINT32)(pos >> 8 & 0xFFFFFF) * (1.0f / 16777216.0f);
        for (UINT32 c = 0; c < ch; ++c) o[(SIZE_T)i * ch + c] = x[c] + (x[ch + c] - x[c]) * t;
      }
    }
  }
  for (UINT32 c = 0; c < ch; ++c) {
    v->s0[c] = w[(SIZE_T)consumed * ch + c];
    v->s1[c] = w[(SIZE_T)(consumed + 1) * ch + c];
  }
  v->frac = end & 0xFFFFFFFFull;
}

/* The bytes the voice must have queued beyond what it has to get through this pass (OnVoiceProcessingPassStart). */
static UINT32 bytes_required(const Voice *v, UINT32 frames, UINT32 out_rate) {
  const double step = (double)v->src_rate * v->ratio / (double)out_rate;
  const UINT64 need = (UINT64)(frames * step) + 2;
  UINT64 have = 0;
  for (UINT32 k = 0; k < v->count; ++k) {
    const Queued *q = &v->queue[(v->head + k) % MAX_QUEUED];
    const UINT32 play_end = q->b.PlayBegin + (q->b.PlayLength ? q->b.PlayLength : q->frames - q->b.PlayBegin);
    if (q->b.LoopCount == LOOP_INFINITE) return 0;
    have += play_end > q->at ? play_end - q->at : 0;
  }
  return need > have ? (UINT32)((need - have) * (v->tag == WAVE_FORMAT_ADPCM ? v->block / (v->adpcm_samples_per_block ? v->adpcm_samples_per_block : 1) + 1 : v->block)) : 0;
}

/* ---- the processing pass ----------------------------------------------------------------------------------------------------------------------- */

/* Volume, channel volumes and the sends of a voice whose `frames` frames are in v->work (v->out_channels wide). */
static void output_voice(Voice *v, UINT32 frames) {
  const UINT32 ch = v->out_channels;
  BOOL unity = v->volume == 1.0f;
  for (UINT32 c = 0; c < ch && unity; ++c) unity = v->channel_volumes[c] == 1.0f;
  if (!unity)
    for (UINT32 i = 0; i < frames; ++i)
      for (UINT32 c = 0; c < ch; ++c) v->work[(SIZE_T)i * ch + c] *= v->volume * v->channel_volumes[c];
  for (UINT32 s = 0; s < v->send_count; ++s) {
    Send *send = &v->sends[s];
    Voice *to = send->to;
    if (to->dead || !to->input) continue;
    const UINT32 dch = to->channels;
    const BOOL filtered = (send->flags & SEND_USEFILTER) && !filter_bypassed(&send->filter);
    float *target = filtered ? v->work2 : to->input;
    if (filtered) zero(v->work2, (SIZE_T)frames * dch * sizeof(float));
    const float *mx = send->matrix;
    if (ch == 1 && dch == 1) {
      const float a = mx[0];
      for (UINT32 i = 0; i < frames; ++i) target[i] += a * v->work[i];
    } else if (ch == 2 && dch == 1) {
      const float a = mx[0], b = mx[1];
      for (UINT32 i = 0; i < frames; ++i) target[i] += a * v->work[2 * i] + b * v->work[2 * i + 1];
    } else if (ch == 1 && dch == 2) {
      /* (the common cases on their own: mono and stereo into stereo) */
      const float a = mx[0], b = mx[1];
      for (UINT32 i = 0; i < frames; ++i) {
        const float x = v->work[i];
        target[2 * i] += a * x;
        target[2 * i + 1] += b * x;
      }
    } else if (ch == 2 && dch == 2) {
      const float a = mx[0], b = mx[1], c2 = mx[2], d2 = mx[3];
      for (UINT32 i = 0; i < frames; ++i) {
        const float l = v->work[2 * i], r = v->work[2 * i + 1];
        target[2 * i] += a * l + b * r;
        target[2 * i + 1] += c2 * l + d2 * r;
      }
    } else for (UINT32 i = 0; i < frames; ++i) {
      const float *x = &v->work[(SIZE_T)i * ch];
      float *y = &target[(SIZE_T)i * dch];
      for (UINT32 d = 0; d < dch; ++d) {
        const float *row = &send->matrix[(SIZE_T)d * ch];
        float sum = 0;
        for (UINT32 c = 0; c < ch; ++c) sum += row[c] * x[c];
        y[d] += sum;
      }
    }
    if (filtered) {
      run_filter(&send->filter, send->low, send->band, v->work2, frames, dch);
      for (SIZE_T k = 0; k < (SIZE_T)frames * dch; ++k) to->input[k] += v->work2[k];
    }
  }
}

static void run_source(Engine *e, Voice *v) {
  if (!v->started || v->dead) return;
  const UINT32 out_rate = v->send_count ? v->sends[0].to->rate : e->rate;
  const UINT32 frames = quantum_of(out_rate);
  if (!ensure_work(v, frames)) return;
  if (v->callback) IXAudio2VoiceCallback_OnVoiceProcessingPassStart(v->callback, bytes_required(v, frames, out_rate));
  const BOOL silent = v->volume == 0.0f && v->effect_count == 0;      /* (nothing of it reaches anyone) */
  produce(v, frames, out_rate, silent);
  if (!silent) {
    if (v->flags & VOICE_USEFILTER) run_filter(&v->filter, v->low, v->band, v->work, frames, v->src_channels);
    run_effects(v, frames);
    output_voice(v, frames);
  }
  if (v->callback) IXAudio2VoiceCallback_OnVoiceProcessingPassEnd(v->callback);
}

static void run_mix(Voice *v) {
  if (v->dead || !v->input) return;
  const UINT32 frames = quantum_of(v->rate);
  if (!ensure_work(v, frames)) return;
  copy(v->work, v->input, (SIZE_T)frames * v->channels * sizeof(float));
  if (v->flags & VOICE_USEFILTER) run_filter(&v->filter, v->low, v->band, v->work, frames, v->channels);
  run_effects(v, frames);
  if (v->kind == KIND_SUBMIX) output_voice(v, frames);
}

static void free_voice(Voice *v);

/* One quantum of the whole graph into e->output (the master's frames, interleaved). */
static void run_pass(Engine *e) {
  Voice *m = e->master;
  for (Voice *v = e->voices; v; v = v->next)
    if (v->input) zero(v->input, (SIZE_T)quantum_of(v->rate) * v->channels * sizeof(float));
  for (UINT32 k = 0; k < e->callback_count; ++k) IXAudio2EngineCallback_OnProcessingPassStart(e->callbacks[k]);
  /* (the list may grow while callbacks run: new voices are put first, so the walk goes on safely; destroyed ones are only marked) */
  for (Voice *v = e->voices; v; v = v->next)
    if (v->kind == KIND_SOURCE) run_source(e, v);
  /* submix voices by processing stage */
  Voice *order[256];
  UINT32 n = 0;
  for (Voice *v = e->voices; v && n < 256; v = v->next)
    if (v->kind == KIND_SUBMIX && !v->dead) {
      UINT32 at = n++;
      while (at > 0 && order[at - 1]->stage > v->stage) {
        order[at] = order[at - 1];
        --at;
      }
      order[at] = v;
    }
  for (UINT32 k = 0; k < n; ++k) run_mix(order[k]);
  if (m && !m->dead) {
    const UINT32 frames = quantum_of(m->rate);
    run_mix(m);
    const UINT32 ch = m->out_channels;
    BOOL unity = m->volume == 1.0f;
    for (UINT32 c = 0; c < ch && unity; ++c) unity = m->channel_volumes[c] == 1.0f;
    if (unity) copy(e->output, m->work, (SIZE_T)frames * ch * sizeof(float));
    else
      for (UINT32 i = 0; i < frames; ++i)
        for (UINT32 c = 0; c < ch; ++c) e->output[(SIZE_T)i * ch + c] = m->work[(SIZE_T)i * ch + c] * m->volume * m->channel_volumes[c];
  }
  for (UINT32 k = 0; k < e->callback_count; ++k) IXAudio2EngineCallback_OnProcessingPassEnd(e->callbacks[k]);
  ++e->passes;
  /* voices destroyed from callbacks */
  for (Voice **p = &e->voices; *p;) {
    Voice *v = *p;
    if (v->dead) {
      *p = v->next;
      free_voice(v);
    } else {
      p = &v->next;
    }
  }
}

/* ---- the engine thread ------------------------------------------------------------------------------------------------------------------------- */

static ULONG64 now_us(void) {
  LARGE_INTEGER t, f;
  QueryPerformanceCounter(&t);
  QueryPerformanceFrequency(&f);
  return f.QuadPart ? (ULONG64)((double)t.QuadPart * 1000000.0 / (double)f.QuadPart) : GetTickCount64() * 1000;
}

static void engine_tick_locked(Engine *e) {
  if (!e->running || !e->master || !e->stream || !e->output) return;
  const UINT32 rate = e->rate, frames = quantum_of(rate), frame = 4 * e->channels;
  const ULONG64 now = now_us();
  if (e->last_tick_us) {
    const ULONG64 gap = (now - e->last_tick_us) * rate / 1000000;
    e->gap_frames = gap > e->gap_frames ? gap : e->gap_frames - (e->gap_frames - gap) / 16;
  }
  e->last_tick_us = now;
  ULONG64 ahead = e->gap_frames * 5 / 2 + frames;
  if (ahead < (ULONG64)rate * AHEAD_MIN_MS / 1000) ahead = (ULONG64)rate * AHEAD_MIN_MS / 1000;
  if (ahead > (ULONG64)rate * AHEAD_MAX_MS / 1000) ahead = (ULONG64)rate * AHEAD_MAX_MS / 1000;
  ULONG64 waiting = 0;
  if (tcos_audio_status(e->stream, &waiting, NULL, NULL, NULL) != 0) return;
  ULONG64 queued = waiting / frame;
  if (queued == 0 && e->passes > 0) ++e->glitches;
  for (int guard = 0; queued < ahead && guard < 32; ++guard) {
    const ULONG64 began = now_us();
    run_pass(e);
    const UINT32 took = (UINT32)(now_us() - began);
    e->audio_us += took;
    if (!e->min_pass_us || took < e->min_pass_us) e->min_pass_us = took;
    if (took > e->max_pass_us) e->max_pass_us = took;
    if (!e->master || !e->stream) return;      /* (a callback destroyed the mastering voice) */
    ULONG64 taken = 0;
    audio_call(TCOS_AUDIO_WRITE, e->stream, (ULONG64)(ULONG_PTR)e->output, (ULONG64)frames * frame, 0, 0, 0, NULL, NULL, &taken);
    queued += frames;
    if (taken < (ULONG64)frames * frame) break;      /* (the kernel is full; what it did not take is dropped: rare, and only a few ms) */
  }
}

static DWORD WINAPI engine_thread(LPVOID param) {
  Engine *e = (Engine *)param;
  while (!e->stop) {
    EnterCriticalSection(&e->lock);
    engine_tick_locked(e);
    LeaveCriticalSection(&e->lock);
    WaitForSingleObject(e->wake, 5);
  }
  return 0;
}

/* ---- operation sets ---------------------------------------------------------------------------------------------------------------------------- */

static void start_voice(Voice *v) {
  v->started = TRUE;
}
static void stop_voice(Voice *v) { v->started = FALSE; }
static void exit_loop(Voice *v) {
  if (!v->count) return;
  Queued *q = &v->queue[v->head];
  q->loops_left = 0;
  const UINT32 play_end = q->b.PlayBegin + (q->b.PlayLength ? q->b.PlayLength : q->frames - q->b.PlayBegin);
  if (q->end < play_end && q->at <= q->end) q->end = play_end;
}

static void apply_op(Op *op) {
  Voice *v = op->voice;
  switch (op->kind) {
    case OP_START: start_voice(v); break;
    case OP_STOP: stop_voice(v); break;
    case OP_VOLUME: v->volume = op->f; break;
    case OP_CHANNEL_VOLUMES: copy(v->channel_volumes, op->data, op->size); break;
    case OP_MATRIX: {
      Send *s = send_to(v, op->to);
      if (s && op->size == (SIZE_T)v->out_channels * s->to->channels * sizeof(float)) copy(s->matrix, op->data, op->size);
      break;
    }
    case OP_FILTER: copy(&v->filter, op->data, sizeof v->filter); break;
    case OP_OUTPUT_FILTER: {
      Send *s = send_to(v, op->to);
      if (s) copy(&s->filter, op->data, sizeof s->filter);
      break;
    }
    case OP_RATIO: v->ratio = op->f; break;
    case OP_EXIT_LOOP: exit_loop(v); break;
    case OP_ENABLE: if (op->a < v->effect_count) v->effects[op->a].enabled = TRUE; break;
    case OP_DISABLE: if (op->a < v->effect_count) v->effects[op->a].enabled = FALSE; break;
    case OP_EFFECT_PARAMETERS:
      if (op->a < v->effect_count && v->effects[op->a].parameters) IXAPOParameters_SetParameters(v->effects[op->a].parameters, op->data, op->size);
      break;
    default: break;
  }
}

/* Runs `kind` on `v` now (OperationSet XAUDIO2_COMMIT_NOW) or at CommitChanges(set). `data` (copied) holds the arguments that do not fit. */
static HRESULT queue_op(Voice *v, UINT32 set, int kind, UINT32 a, float f, Voice *to, const void *data, UINT32 size) {
  Engine *e = engine_of(v);
  if (set == XAUDIO2_INVALID_OPSET) return E_INVALIDARG;
  Op op = {NULL, set, v, kind, a, 0, f, to, (void *)data, size};
  EnterCriticalSection(&e->lock);
  if (set == XAUDIO2_COMMIT_NOW) {
    apply_op(&op);
    LeaveCriticalSection(&e->lock);
    return S_OK;
  }
  Op *q = (Op *)alloc(sizeof *q + size);
  if (!q) {
    LeaveCriticalSection(&e->lock);
    return E_OUTOFMEMORY;
  }
  *q = op;
  q->data = size ? (void *)(q + 1) : NULL;
  if (size) copy(q->data, data, size);
  Op **tail = &e->ops;
  while (*tail) tail = &(*tail)->next;
  *tail = q;
  LeaveCriticalSection(&e->lock);
  return S_OK;
}

/* Ops of a voice that goes (the lock held). */
static void drop_ops_locked(Engine *e, Voice *v) {
  for (Op **p = &e->ops; *p;) {
    Op *op = *p;
    if (op->voice == v || op->to == v) {
      *p = op->next;
      release_memory(op);
    } else {
      p = &op->next;
    }
  }
}

/* ---- IXAudio2Voice (shared by the three kinds) ------------------------------------------------------------------------------------------------- */

static void free_voice(Voice *v) {
  while (v->count) pop_buffer(v, FALSE);
  free_effects(v);
  free_sends(v);
  release_memory(v->input);
  release_memory(v->work);
  release_memory(v->work2);
  release_memory(v->window);
  release_memory(v);
}

static void WINAPI v_get_details(IXAudio2Voice *i, XAUDIO2_VOICE_DETAILS *d) {
  Voice *v = from_voice(i);
  if (!d) return;
  d->CreationFlags = v->flags;
  d->ActiveFlags = v->flags & (VOICE_USEFILTER);
  d->InputChannels = v->channels;
  d->InputSampleRate = v->rate;
}
static HRESULT WINAPI v_set_output_voices(IXAudio2Voice *i, const XAUDIO2_VOICE_SENDS *list) {
  Voice *v = from_voice(i);
  EnterCriticalSection(&v->engine->lock);
  const HRESULT hr = set_sends_locked(v, list);
  LeaveCriticalSection(&v->engine->lock);
  return hr;
}
static HRESULT WINAPI v_set_effect_chain(IXAudio2Voice *i, const XAUDIO2_EFFECT_CHAIN *chain) {
  Voice *v = from_voice(i);
  EnterCriticalSection(&v->engine->lock);
  const HRESULT hr = set_effects_locked(v, chain);
  LeaveCriticalSection(&v->engine->lock);
  return hr;
}
static HRESULT WINAPI v_enable_effect(IXAudio2Voice *i, UINT32 index, UINT32 set) {
  Voice *v = from_voice(i);
  if (index >= v->effect_count) return E_INVALIDARG;
  return queue_op(v, set, OP_ENABLE, index, 0, NULL, NULL, 0);
}
static HRESULT WINAPI v_disable_effect(IXAudio2Voice *i, UINT32 index, UINT32 set) {
  Voice *v = from_voice(i);
  if (index >= v->effect_count) return E_INVALIDARG;
  return queue_op(v, set, OP_DISABLE, index, 0, NULL, NULL, 0);
}
static void WINAPI v_get_effect_state(IXAudio2Voice *i, UINT32 index, WINBOOL *enabled) {
  Voice *v = from_voice(i);
  if (enabled) *enabled = index < v->effect_count ? v->effects[index].enabled : FALSE;
}
static HRESULT WINAPI v_set_effect_parameters(IXAudio2Voice *i, UINT32 index, const void *p, UINT32 size, UINT32 set) {
  Voice *v = from_voice(i);
  if (index >= v->effect_count || !v->effects[index].parameters) return E_INVALIDARG;
  if (!p && size) return E_INVALIDARG;
  return queue_op(v, set, OP_EFFECT_PARAMETERS, index, 0, NULL, p, size);
}
static HRESULT WINAPI v_get_effect_parameters(IXAudio2Voice *i, UINT32 index, void *p, UINT32 size) {
  Voice *v = from_voice(i);
  if (index >= v->effect_count || !v->effects[index].parameters) return E_INVALIDARG;
  EnterCriticalSection(&v->engine->lock);
  IXAPOParameters_GetParameters(v->effects[index].parameters, p, size);
  LeaveCriticalSection(&v->engine->lock);
  return S_OK;
}
static BOOL valid_filter(const XAUDIO2_FILTER_PARAMETERS *f) {
  return f && (UINT32)f->Type <= FILTER_HIGHPASS_ONEPOLE && f->Frequency >= 0.0f && f->Frequency <= 1.0f && f->OneOverQ > 0.0f && f->OneOverQ <= 1.5f;
}
static HRESULT WINAPI v_set_filter(IXAudio2Voice *i, const XAUDIO2_FILTER_PARAMETERS *f, UINT32 set) {
  Voice *v = from_voice(i);
  if (!(v->flags & VOICE_USEFILTER)) return XAUDIO2_E_INVALID_CALL;
  if (!valid_filter(f)) return E_INVALIDARG;
  return queue_op(v, set, OP_FILTER, 0, 0, NULL, f, sizeof *f);
}
static void WINAPI v_get_filter(IXAudio2Voice *i, XAUDIO2_FILTER_PARAMETERS *f) {
  Voice *v = from_voice(i);
  if (f) *f = v->filter;
}
static HRESULT WINAPI v_set_output_filter(IXAudio2Voice *i, IXAudio2Voice *to, const XAUDIO2_FILTER_PARAMETERS *f, UINT32 set) {
  Voice *v = from_voice(i);
  Send *s = send_to(v, from_voice(to));
  if (!s) return E_INVALIDARG;
  if (!(s->flags & SEND_USEFILTER)) return XAUDIO2_E_INVALID_CALL;
  if (!valid_filter(f)) return E_INVALIDARG;
  return queue_op(v, set, OP_OUTPUT_FILTER, 0, 0, from_voice(to), f, sizeof *f);
}
static void WINAPI v_get_output_filter(IXAudio2Voice *i, IXAudio2Voice *to, XAUDIO2_FILTER_PARAMETERS *f) {
  Voice *v = from_voice(i);
  Send *s = send_to(v, from_voice(to));
  if (f && s) *f = s->filter;
}
static HRESULT WINAPI v_set_volume(IXAudio2Voice *i, float volume, UINT32 set) {
  Voice *v = from_voice(i);
  if (!(volume >= -XAUDIO2_MAX_VOLUME_LEVEL && volume <= XAUDIO2_MAX_VOLUME_LEVEL)) return E_INVALIDARG;
  return queue_op(v, set, OP_VOLUME, 0, volume, NULL, NULL, 0);
}
static void WINAPI v_get_volume(IXAudio2Voice *i, float *volume) {
  if (volume) *volume = from_voice(i)->volume;
}
static HRESULT WINAPI v_set_channel_volumes(IXAudio2Voice *i, UINT32 channels, const float *volumes, UINT32 set) {
  Voice *v = from_voice(i);
  if (!volumes || channels != v->out_channels || channels > MAX_CHANNELS) return E_INVALIDARG;
  for (UINT32 c = 0; c < channels; ++c)
    if (!(volumes[c] >= -XAUDIO2_MAX_VOLUME_LEVEL && volumes[c] <= XAUDIO2_MAX_VOLUME_LEVEL)) return E_INVALIDARG;
  return queue_op(v, set, OP_CHANNEL_VOLUMES, 0, 0, NULL, volumes, channels * sizeof(float));
}
static void WINAPI v_get_channel_volumes(IXAudio2Voice *i, UINT32 channels, float *volumes) {
  Voice *v = from_voice(i);
  if (!volumes) return;
  for (UINT32 c = 0; c < channels && c < MAX_CHANNELS; ++c) volumes[c] = c < v->out_channels ? v->channel_volumes[c] : 0;
}
static HRESULT WINAPI v_set_output_matrix(IXAudio2Voice *i, IXAudio2Voice *to, UINT32 from_ch, UINT32 to_ch, const float *matrix, UINT32 set) {
  Voice *v = from_voice(i);
  EnterCriticalSection(&v->engine->lock);
  Send *s = send_to(v, from_voice(to));
  const BOOL ok = s && from_ch == v->out_channels && to_ch == s->to->channels;
  Voice *dest = s ? s->to : NULL;
  LeaveCriticalSection(&v->engine->lock);
  if (!ok || !matrix) return E_INVALIDARG;
  return queue_op(v, set, OP_MATRIX, 0, 0, dest, matrix, from_ch * to_ch * sizeof(float));
}
static void WINAPI v_get_output_matrix(IXAudio2Voice *i, IXAudio2Voice *to, UINT32 from_ch, UINT32 to_ch, float *matrix) {
  Voice *v = from_voice(i);
  if (!matrix) return;
  EnterCriticalSection(&v->engine->lock);
  Send *s = send_to(v, from_voice(to));
  if (s && from_ch == v->out_channels && to_ch == s->to->channels) copy(matrix, s->matrix, from_ch * to_ch * sizeof(float));
  LeaveCriticalSection(&v->engine->lock);
}
static void WINAPI v_destroy(IXAudio2Voice *i) {
  Voice *v = from_voice(i);
  Engine *e = v->engine;
  EnterCriticalSection(&e->lock);
  /* nobody sends to it any more */
  for (Voice *o = e->voices; o; o = o->next)
    for (UINT32 s = 0; s < o->send_count;) {
      if (o->sends[s].to == v) {
        release_memory(o->sends[s].matrix);
        for (UINT32 k = s; k + 1 < o->send_count; ++k) o->sends[k] = o->sends[k + 1];
        --o->send_count;
      } else {
        ++s;
      }
    }
  drop_ops_locked(e, v);
  if (v == e->master) {
    e->master = NULL;
    if (e->stream) audio_call(TCOS_AUDIO_CLOSE, e->stream, 0, 0, 0, 0, 0, NULL, NULL, NULL);
    e->stream = 0;
  }
  if (GetCurrentThreadId() == e->thread_id) {
    v->dead = TRUE;      /* (from a callback, during a pass: freed when the pass is over) */
  } else {
    for (Voice **p = &e->voices; *p; p = &(*p)->next)
      if (*p == v) {
        *p = v->next;
        break;
      }
    free_voice(v);
  }
  LeaveCriticalSection(&e->lock);
}

/* ---- IXAudio2SourceVoice ----------------------------------------------------------------------------------------------------------------------- */

static HRESULT WINAPI s_start(IXAudio2SourceVoice *i, UINT32 flags, UINT32 set) {
  (void)flags;
  return queue_op(from_voice(i), set, OP_START, 0, 0, NULL, NULL, 0);
}
static HRESULT WINAPI s_stop(IXAudio2SourceVoice *i, UINT32 flags, UINT32 set) {
  if (flags & ~(UINT32)PLAY_TAILS) return E_INVALIDARG;
  return queue_op(from_voice(i), set, OP_STOP, 0, 0, NULL, NULL, 0);
}
static HRESULT WINAPI s_submit(IXAudio2SourceVoice *i, const XAUDIO2_BUFFER *b, const XAUDIO2_BUFFER_WMA *wma) {
  Voice *v = from_voice(i);
  if (!b || wma) return wma ? XAUDIO2_E_INVALID_CALL : E_POINTER;
  if (!b->pAudioData || b->AudioBytes == 0 || b->AudioBytes > XAUDIO2_MAX_BUFFER_BYTES) return XAUDIO2_E_INVALID_CALL;
  if (b->Flags & ~(UINT32)END_OF_STREAM) return XAUDIO2_E_INVALID_CALL;
  /* frames in it */
  UINT32 frames = 0;
  BYTE *decoded = NULL;
  if (v->tag == WAVE_FORMAT_ADPCM) {
    if (b->AudioBytes % v->block) return XAUDIO2_E_INVALID_CALL;
    const UINT32 blocks = b->AudioBytes / v->block;
    if (blocks > 0xFFFFFFFFu / v->adpcm_samples_per_block) return XAUDIO2_E_INVALID_CALL;
    decoded = (BYTE *)alloc((SIZE_T)blocks * v->adpcm_samples_per_block * 2 * v->src_channels);
    if (!decoded) return E_OUTOFMEMORY;
    frames = decode_adpcm(v, b->pAudioData, b->AudioBytes, (short *)decoded);
  } else {
    frames = b->AudioBytes / v->block;
  }
  if (b->PlayBegin > frames || b->PlayLength > frames - b->PlayBegin || (b->PlayLength == 0 && b->PlayBegin >= frames && frames)) {
    release_memory(decoded);
    return XAUDIO2_E_INVALID_CALL;
  }
  const UINT32 play_end = b->PlayBegin + (b->PlayLength ? b->PlayLength : frames - b->PlayBegin);
  UINT32 loop_end = play_end;
  if (b->LoopCount) {
    if (b->LoopCount > XAUDIO2_MAX_LOOP_COUNT && b->LoopCount != LOOP_INFINITE) { release_memory(decoded); return XAUDIO2_E_INVALID_CALL; }
    loop_end = b->LoopLength ? b->LoopBegin + b->LoopLength : play_end;
    if (b->LoopBegin >= play_end || loop_end > play_end || loop_end <= b->LoopBegin) { release_memory(decoded); return XAUDIO2_E_INVALID_CALL; }
  } else if (b->LoopBegin || b->LoopLength) {
    release_memory(decoded);
    return XAUDIO2_E_INVALID_CALL;
  }
  Engine *e = v->engine;
  EnterCriticalSection(&e->lock);
  if (v->count >= MAX_QUEUED) {
    LeaveCriticalSection(&e->lock);
    release_memory(decoded);
    return XAUDIO2_E_INVALID_CALL;
  }
  Queued *q = &v->queue[(v->head + v->count) % MAX_QUEUED];
  zero(q, sizeof *q);
  q->b = *b;
  q->decoded = decoded;
  q->frames = frames;
  q->at = b->PlayBegin;
  q->end = b->LoopCount ? loop_end : play_end;
  q->loops_left = b->LoopCount;
  ++v->count;
  if (v->starved) {
    /* (it ran dry: start the new audio without a step through zeros) */
    v->primed = FALSE;
    v->starved = FALSE;
  }
  LeaveCriticalSection(&e->lock);
  return S_OK;
}
static HRESULT WINAPI s_flush(IXAudio2SourceVoice *i) {
  Voice *v = from_voice(i);
  Engine *e = v->engine;
  EnterCriticalSection(&e->lock);
  /* (the buffer being played stays while the voice runs) */
  const UINT32 keep = v->started && v->count && v->queue[v->head].started ? 1 : 0;
  while (v->count > keep) {
    const UINT32 last = (v->head + v->count - 1) % MAX_QUEUED;
    Queued *q = &v->queue[last];
    void *context = q->b.pContext;
    release_memory(q->decoded);
    zero(q, sizeof *q);
    --v->count;
    if (v->callback) IXAudio2VoiceCallback_OnBufferEnd(v->callback, context);
  }
  LeaveCriticalSection(&e->lock);
  return S_OK;
}
static HRESULT WINAPI s_discontinuity(IXAudio2SourceVoice *i) { (void)i; return S_OK; }
static HRESULT WINAPI s_exit_loop(IXAudio2SourceVoice *i, UINT32 set) { return queue_op(from_voice(i), set, OP_EXIT_LOOP, 0, 0, NULL, NULL, 0); }
static void WINAPI s_get_state(IXAudio2SourceVoice *i, XAUDIO2_VOICE_STATE *state, UINT32 flags) {
  Voice *v = from_voice(i);
  if (!state) return;
  EnterCriticalSection(&v->engine->lock);
  state->pCurrentBufferContext = v->count ? v->queue[v->head].b.pContext : NULL;
  state->BuffersQueued = v->count;
  state->SamplesPlayed = flags & VOICE_NOSAMPLESPLAYED ? 0 : v->samples_played;
  LeaveCriticalSection(&v->engine->lock);
}
static HRESULT WINAPI s_set_ratio(IXAudio2SourceVoice *i, float ratio, UINT32 set) {
  Voice *v = from_voice(i);
  if (v->flags & VOICE_NOPITCH) return XAUDIO2_E_INVALID_CALL;
  if (!(ratio >= (float)XAUDIO2_MIN_FREQ_RATIO && ratio <= (float)XAUDIO2_MAX_FREQ_RATIO)) return E_INVALIDARG;
  if (ratio > v->max_ratio) ratio = v->max_ratio;
  return queue_op(v, set, OP_RATIO, 0, ratio, NULL, NULL, 0);
}
static void WINAPI s_get_ratio(IXAudio2SourceVoice *i, float *ratio) {
  if (ratio) *ratio = from_voice(i)->ratio;
}
static HRESULT WINAPI s_set_rate(IXAudio2SourceVoice *i, UINT32 rate) {
  Voice *v = from_voice(i);
  if (rate < XAUDIO2_MIN_SAMPLE_RATE || rate > XAUDIO2_MAX_SAMPLE_RATE) return E_INVALIDARG;
  EnterCriticalSection(&v->engine->lock);
  HRESULT hr = S_OK;
  if (v->count) hr = XAUDIO2_E_INVALID_CALL;      /* (only while nothing is queued) */
  else v->src_rate = v->rate = rate;
  LeaveCriticalSection(&v->engine->lock);
  return hr;
}

#define VOICE_METHODS(prefix_type)                                                                                                              \
  (void *)v_get_details, (void *)v_set_output_voices, (void *)v_set_effect_chain, (void *)v_enable_effect, (void *)v_disable_effect,            \
      (void *)v_get_effect_state, (void *)v_set_effect_parameters, (void *)v_get_effect_parameters, (void *)v_set_filter, (void *)v_get_filter, \
      (void *)v_set_output_filter, (void *)v_get_output_filter, (void *)v_set_volume, (void *)v_get_volume, (void *)v_set_channel_volumes,      \
      (void *)v_get_channel_volumes, (void *)v_set_output_matrix, (void *)v_get_output_matrix, (void *)v_destroy

/* (the three vtables begin with the same IXAudio2Voice methods: kept as tables of pointers, cast once here) */
static void *const kSourceTable[] = {VOICE_METHODS(source), (void *)s_start, (void *)s_stop, (void *)s_submit, (void *)s_flush, (void *)s_discontinuity,
                                     (void *)s_exit_loop, (void *)s_get_state, (void *)s_set_ratio, (void *)s_get_ratio, (void *)s_set_rate};
static void *const kSubmixTable[] = {VOICE_METHODS(submix)};
static HRESULT WINAPI m_get_channel_mask(IXAudio2MasteringVoice *i, DWORD *mask) {
  Voice *v = from_voice(i);
  if (!mask) return E_POINTER;
  static const DWORD kMasks[9] = {0, SPEAKER_FRONT_CENTER, KSAUDIO_SPEAKER_STEREO, KSAUDIO_SPEAKER_STEREO | SPEAKER_LOW_FREQUENCY, KSAUDIO_SPEAKER_QUAD,
                                  KSAUDIO_SPEAKER_QUAD | SPEAKER_FRONT_CENTER, KSAUDIO_SPEAKER_5POINT1, KSAUDIO_SPEAKER_5POINT1 | SPEAKER_BACK_CENTER,
                                  KSAUDIO_SPEAKER_7POINT1_SURROUND};
  *mask = v->channels < 9 ? kMasks[v->channels] : 0;
  return S_OK;
}
static void *const kMasterTable[] = {VOICE_METHODS(master), (void *)m_get_channel_mask};

/* ---- IXAudio2 ---------------------------------------------------------------------------------------------------------------------------------- */

static HRESULT WINAPI e_query(IXAudio2 *i, REFIID riid, void **out) {
  if (!out) return E_POINTER;
  *out = NULL;
  if (!same_guid(riid, &kIidUnknown) && !same_guid(riid, &IID_IXAudio2) && !same_guid(riid, &kIidXAudio2_8)) return E_NOINTERFACE;
  InterlockedIncrement(&from_engine(i)->refs);
  *out = i;
  return S_OK;
}
static ULONG WINAPI e_add_ref(IXAudio2 *i) { return (ULONG)InterlockedIncrement(&from_engine(i)->refs); }
static ULONG WINAPI e_release(IXAudio2 *i) {
  Engine *e = from_engine(i);
  const LONG left = InterlockedDecrement(&e->refs);
  if (left != 0) return (ULONG)left;
  if (e->thread) {
    e->stop = 1;
    SetEvent(e->wake);
    WaitForSingleObject(e->thread, INFINITE);
    CloseHandle(e->thread);
  }
  /* the voices go with it (a program should have destroyed them) */
  while (e->voices) {
    Voice *v = e->voices;
    e->voices = v->next;
    free_voice(v);
  }
  while (e->ops) {
    Op *op = e->ops;
    e->ops = op->next;
    release_memory(op);
  }
  if (e->stream) audio_call(TCOS_AUDIO_CLOSE, e->stream, 0, 0, 0, 0, 0, NULL, NULL, NULL);
  if (e->wake) CloseHandle(e->wake);
  DeleteCriticalSection(&e->lock);
  release_memory(e->output);
  release_memory(e);
  return 0;
}
static HRESULT WINAPI e_register(IXAudio2 *i, IXAudio2EngineCallback *callback) {
  Engine *e = from_engine(i);
  if (!callback) return E_INVALIDARG;
  EnterCriticalSection(&e->lock);
  HRESULT hr = S_OK;
  for (UINT32 k = 0; k < e->callback_count; ++k) if (e->callbacks[k] == callback) hr = S_FALSE;
  if (hr == S_OK) {
    if (e->callback_count < 16) e->callbacks[e->callback_count++] = callback;
    else hr = E_OUTOFMEMORY;
  }
  LeaveCriticalSection(&e->lock);
  return hr == S_FALSE ? S_OK : hr;
}
static void WINAPI e_unregister(IXAudio2 *i, IXAudio2EngineCallback *callback) {
  Engine *e = from_engine(i);
  EnterCriticalSection(&e->lock);
  for (UINT32 k = 0; k < e->callback_count; ++k)
    if (e->callbacks[k] == callback) {
      for (UINT32 j = k; j + 1 < e->callback_count; ++j) e->callbacks[j] = e->callbacks[j + 1];
      --e->callback_count;
      break;
    }
  LeaveCriticalSection(&e->lock);
}

static Voice *new_voice(Engine *e, Kind kind, UINT32 channels, UINT32 rate, UINT32 flags) {
  Voice *v = (Voice *)alloc(sizeof *v);
  if (!v) return NULL;
  v->iface.base.lpVtbl = (IXAudio2VoiceVtbl *)(kind == KIND_SOURCE ? (void *)kSourceTable : kind == KIND_SUBMIX ? (void *)kSubmixTable : (void *)kMasterTable);
  v->engine = e;
  v->kind = kind;
  v->channels = v->out_channels = channels;
  v->rate = rate;
  v->flags = flags;
  v->volume = 1.0f;
  for (UINT32 c = 0; c < MAX_CHANNELS; ++c) v->channel_volumes[c] = 1.0f;
  v->filter.Type = LowPassFilter;
  v->filter.Frequency = 1.0f;
  v->filter.OneOverQ = 1.0f;
  v->ratio = 1.0f;
  if (kind != KIND_SOURCE) {
    v->input = (float *)alloc((SIZE_T)quantum_of(rate) * channels * sizeof(float));
    if (!v->input) {
      release_memory(v);
      return NULL;
    }
  }
  if (!ensure_work(v, quantum_of(rate > DEFAULT_RATE ? rate : DEFAULT_RATE) * 2 + 8)) {
    free_voice(v);
    return NULL;
  }
  return v;
}

/* A voice is put first in the list (so that a pass that is walking it from a callback goes on safely). */
static HRESULT finish_voice_locked(Engine *e, Voice *v, const XAUDIO2_VOICE_SENDS *sends, const XAUDIO2_EFFECT_CHAIN *chain) {
  HRESULT hr = S_OK;
  if (v->kind != KIND_MASTER) hr = set_sends_locked(v, sends);
  if (SUCCEEDED(hr) && chain) hr = set_effects_locked(v, chain);
  if (FAILED(hr)) return hr;
  v->next = e->voices;
  e->voices = v;
  return S_OK;
}

static HRESULT WINAPI e_create_source(IXAudio2 *i, IXAudio2SourceVoice **out, const WAVEFORMATEX *f, UINT32 flags, float max_ratio,
                                      IXAudio2VoiceCallback *callback, const XAUDIO2_VOICE_SENDS *sends, const XAUDIO2_EFFECT_CHAIN *chain) {
  Engine *e = from_engine(i);
  if (!out || !f) return E_POINTER;
  *out = NULL;
  if (flags & ~(UINT32)(VOICE_NOPITCH | VOICE_NOSRC | VOICE_USEFILTER)) return E_INVALIDARG;
  if (max_ratio < (float)XAUDIO2_MIN_FREQ_RATIO || max_ratio > (float)XAUDIO2_MAX_FREQ_RATIO) return E_INVALIDARG;
  WORD tag = f->wFormatTag;
  if (tag == TCOS_WAVE_FORMAT_EXTENSIBLE) tag = wave_format_tag(f);
  if (f->nChannels == 0 || f->nChannels > MAX_CHANNELS || f->nSamplesPerSec < XAUDIO2_MIN_SAMPLE_RATE || f->nSamplesPerSec > XAUDIO2_MAX_SAMPLE_RATE)
    return XAUDIO2_E_INVALID_CALL;
  const WORD bits = f->wBitsPerSample;
  if (tag == WAVE_FORMAT_PCM) {
    if ((bits != 8 && bits != 16 && bits != 24 && bits != 32) || f->nBlockAlign != f->nChannels * (bits / 8)) return XAUDIO2_E_INVALID_CALL;
  } else if (tag == WAVE_FORMAT_IEEE_FLOAT) {
    if (bits != 32 || f->nBlockAlign != f->nChannels * 4) return XAUDIO2_E_INVALID_CALL;
  } else if (tag == WAVE_FORMAT_ADPCM) {
    const ADPCMWAVEFORMAT *a = (const ADPCMWAVEFORMAT *)f;
    if (f->nChannels > 2 || bits != 4 || f->cbSize < 4 || a->wSamplesPerBlock < 2 || a->wNumCoef == 0 || a->wNumCoef > 32 ||
        f->cbSize < 4 + 4 * a->wNumCoef || f->nBlockAlign < 7 * f->nChannels)
      return XAUDIO2_E_INVALID_CALL;
  } else {
    return XAUDIO2_E_INVALID_CALL;      /* (xWMA, XMA: not here) */
  }
  if (max_ratio == 0) max_ratio = (float)XAUDIO2_DEFAULT_FREQ_RATIO;
  Voice *v = new_voice(e, KIND_SOURCE, f->nChannels, f->nSamplesPerSec, flags);
  if (!v) return E_OUTOFMEMORY;
  v->tag = tag;
  v->bits = bits;
  v->block = f->nBlockAlign;
  v->src_channels = f->nChannels;
  v->src_rate = f->nSamplesPerSec;
  v->max_ratio = max_ratio;
  v->callback = callback;
  if (tag == WAVE_FORMAT_ADPCM) {
    const ADPCMWAVEFORMAT *a = (const ADPCMWAVEFORMAT *)f;
    v->adpcm_samples_per_block = a->wSamplesPerBlock;
    v->adpcm_coefficients = a->wNumCoef;
    for (UINT32 k = 0; k < a->wNumCoef; ++k) {
      v->adpcm_coef[2 * k] = a->aCoef[k].iCoef1;
      v->adpcm_coef[2 * k + 1] = a->aCoef[k].iCoef2;
    }
  }
  EnterCriticalSection(&e->lock);
  const HRESULT hr = finish_voice_locked(e, v, sends, chain);
  LeaveCriticalSection(&e->lock);
  if (FAILED(hr)) {
    free_voice(v);
    return hr;
  }
  *out = &v->iface.source;
  return S_OK;
}
static HRESULT WINAPI e_create_submix(IXAudio2 *i, IXAudio2SubmixVoice **out, UINT32 channels, UINT32 rate, UINT32 flags, UINT32 stage,
                                      const XAUDIO2_VOICE_SENDS *sends, const XAUDIO2_EFFECT_CHAIN *chain) {
  Engine *e = from_engine(i);
  if (!out) return E_POINTER;
  *out = NULL;
  if (flags & ~(UINT32)VOICE_USEFILTER) return E_INVALIDARG;
  if (channels == 0 || channels > MAX_CHANNELS || rate < XAUDIO2_MIN_SAMPLE_RATE || rate > XAUDIO2_MAX_SAMPLE_RATE) return XAUDIO2_E_INVALID_CALL;
  Voice *v = new_voice(e, KIND_SUBMIX, channels, rate, flags);
  if (!v) return E_OUTOFMEMORY;
  v->stage = stage;
  EnterCriticalSection(&e->lock);
  const HRESULT hr = finish_voice_locked(e, v, sends, chain);
  LeaveCriticalSection(&e->lock);
  if (FAILED(hr)) {
    free_voice(v);
    return hr;
  }
  *out = &v->iface.submix;
  return S_OK;
}
static HRESULT WINAPI e_create_master(IXAudio2 *i, IXAudio2MasteringVoice **out, UINT32 channels, UINT32 rate, UINT32 flags, LPCWSTR device,
                                      const XAUDIO2_EFFECT_CHAIN *chain, AUDIO_STREAM_CATEGORY category) {
  Engine *e = from_engine(i);
  (void)device; (void)category;
  if (!out) return E_POINTER;
  *out = NULL;
  if (flags) return E_INVALIDARG;
  if (channels == XAUDIO2_DEFAULT_CHANNELS) channels = 2;
  if (rate == XAUDIO2_DEFAULT_SAMPLERATE) rate = DEFAULT_RATE;
  if (channels > MAX_CHANNELS || rate < XAUDIO2_MIN_SAMPLE_RATE || rate > XAUDIO2_MAX_SAMPLE_RATE) return XAUDIO2_E_INVALID_CALL;
  if (!tcos_audio_devices()) return XAUDIO2_E_DEVICE_INVALIDATED;
  EnterCriticalSection(&e->lock);
  if (e->master) {
    LeaveCriticalSection(&e->lock);
    return XAUDIO2_E_INVALID_CALL;      /* (one mastering voice) */
  }
  Voice *v = new_voice(e, KIND_MASTER, channels, rate, 0);
  float *output = v ? (float *)alloc((SIZE_T)quantum_of(rate) * MAX_CHANNELS * sizeof(float)) : NULL;
  /* the graph's stream: 32-bit float in the mastering voice's format */
  WAVEFORMATEX f = {WAVE_FORMAT_IEEE_FLOAT, (WORD)channels, rate, rate * 4 * channels, (WORD)(4 * channels), 32, 0};
  LONG st = 0;
  const ULONG64 stream = output ? tcos_audio_open(&f, &st) : 0;
  HRESULT hr = !v || !output ? E_OUTOFMEMORY : !stream ? XAUDIO2_E_DEVICE_INVALIDATED : finish_voice_locked(e, v, NULL, chain);
  if (FAILED(hr)) {
    LeaveCriticalSection(&e->lock);
    if (stream) audio_call(TCOS_AUDIO_CLOSE, stream, 0, 0, 0, 0, 0, NULL, NULL, NULL);
    if (v) free_voice(v);
    release_memory(output);
    return hr;
  }
  release_memory(e->output);
  e->output = output;
  e->stream = stream;
  e->rate = rate;
  e->channels = v->out_channels;
  e->master = v;
  e->passes = 0;
  LeaveCriticalSection(&e->lock);
  *out = &v->iface.master;
  return S_OK;
}
static HRESULT WINAPI e_start(IXAudio2 *i) {
  Engine *e = from_engine(i);
  EnterCriticalSection(&e->lock);
  e->running = TRUE;
  LeaveCriticalSection(&e->lock);
  SetEvent(e->wake);
  return S_OK;
}
static void WINAPI e_stop(IXAudio2 *i) {
  Engine *e = from_engine(i);
  EnterCriticalSection(&e->lock);
  e->running = FALSE;
  LeaveCriticalSection(&e->lock);
}
static HRESULT WINAPI e_commit(IXAudio2 *i, UINT32 set) {
  Engine *e = from_engine(i);
  EnterCriticalSection(&e->lock);
  for (Op **p = &e->ops; *p;) {
    Op *op = *p;
    if (set == XAUDIO2_COMMIT_ALL || op->set == set) {
      *p = op->next;
      apply_op(op);
      release_memory(op);
    } else {
      p = &op->next;
    }
  }
  LeaveCriticalSection(&e->lock);
  return S_OK;
}
static void WINAPI e_get_performance(IXAudio2 *i, XAUDIO2_PERFORMANCE_DATA *d) {
  Engine *e = from_engine(i);
  if (!d) return;
  zero(d, sizeof *d);
  EnterCriticalSection(&e->lock);
  for (Voice *v = e->voices; v; v = v->next) {
    if (v->kind == KIND_SOURCE) {
      ++d->TotalSourceVoiceCount;
      if (v->started) ++d->ActiveSourceVoiceCount;
      if (v->started && v->src_rate != (v->send_count ? v->sends[0].to->rate : e->rate)) ++d->ActiveResamplerCount;
    } else if (v->kind == KIND_SUBMIX) {
      ++d->ActiveSubmixVoiceCount;
    }
    d->MemoryUsageInBytes += sizeof *v + v->work_frames * MAX_CHANNELS * 8;
  }
  d->GlitchesSinceEngineStarted = e->glitches;
  const ULONG64 now = now_us();
  d->AudioCyclesSinceLastQuery = e->audio_us;
  d->TotalCyclesSinceLastQuery = e->queried_us ? now - e->queried_us : now;
  d->MinimumCyclesPerQuantum = e->min_pass_us;
  d->MaximumCyclesPerQuantum = e->max_pass_us;
  e->audio_us = 0;
  e->queried_us = now;
  e->min_pass_us = e->max_pass_us = 0;
  ULONG64 waiting = 0;
  if (e->stream && tcos_audio_status(e->stream, &waiting, NULL, NULL, NULL) == 0 && e->channels)
    d->CurrentLatencyInSamples = (UINT32)(waiting / (4 * e->channels)) + quantum_of(e->rate);
  LeaveCriticalSection(&e->lock);
}
static void WINAPI e_set_debug(IXAudio2 *i, const XAUDIO2_DEBUG_CONFIGURATION *c, void *reserved) { (void)i; (void)c; (void)reserved; }

static const IXAudio2Vtbl kEngineVtbl = {e_query, e_add_ref, e_release, e_register, e_unregister, e_create_source, e_create_submix, e_create_master,
                                         e_start, e_stop, e_commit, e_get_performance, e_set_debug};

static HRESULT create_engine(IXAudio2 **out, UINT32 flags) {
  if (!out) return E_POINTER;
  *out = NULL;
  (void)flags;      /* (the debug engine, 1024-sample quanta and the virtual audio client: the quantum is 10 ms here) */
  Engine *e = (Engine *)alloc(sizeof *e);
  if (!e) return E_OUTOFMEMORY;
  e->iface.lpVtbl = (IXAudio2Vtbl *)&kEngineVtbl;
  e->refs = 1;
  e->rate = DEFAULT_RATE;
  e->channels = 2;
  e->running = TRUE;      /* (the engine starts with the object, as in Windows) */
  InitializeCriticalSection(&e->lock);
  e->wake = CreateEventA(NULL, FALSE, FALSE, NULL);
  e->thread = e->wake ? CreateThread(NULL, 0, engine_thread, e, CREATE_SUSPENDED, &e->thread_id) : NULL;
  if (!e->thread) {
    e_release(&e->iface);
    return E_OUTOFMEMORY;
  }
  SetThreadPriority(e->thread, THREAD_PRIORITY_TIME_CRITICAL);
  ResumeThread(e->thread);
  *out = &e->iface;
  return S_OK;
}

/* ---- exports ------------------------------------------------------------------------------------------------------------------------------------ */

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved) {
  (void)instance; (void)reason; (void)reserved;
  return TRUE;
}

EXPORT HRESULT WINAPI XAudio2Create(IXAudio2 **out, UINT32 flags, XAUDIO2_PROCESSOR processor) {
  (void)processor;
  return create_engine(out, flags);
}
EXPORT HRESULT WINAPI XAudio2CreateWithVersionInfo(IXAudio2 **out, UINT32 flags, XAUDIO2_PROCESSOR processor, DWORD version) {
  (void)processor; (void)version;
  return create_engine(out, flags);
}

#include "xapo.inc"
#include "x3daudio.inc"
