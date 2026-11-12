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

/* XAudio2's own effects, as XAPOs any voice's effect chain can hold (included by xaudio2_9.c):
 *   CreateAudioVolumeMeter   peak and RMS levels per channel of each pass (GetEffectParameters with XAUDIO2FX_VOLUMEMETER_LEVELS);
 *   CreateAudioReverb        a Schroeder reverb (four combs and two all-passes a channel) driven by XAUDIO2FX_REVERB_PARAMETERS' wet/dry mix, decay
 *                            time and room size (the 2.8 and 2.9 layouts); mono or stereo in, the same or more channels out;
 *   CreateFX                 FXEQ (four peaking bands, the audio EQ cookbook's biquads), FXMasteringLimiter (make-up gain from the loudness, a peak
 *                            limiter with the release time), FXReverb (the same reverb from diffusion and room size), FXEcho (a delay line with
 *                            wet/dry mix and feedback).
 * The reverb's early reflections, its EQ and room filters and the position parameters are not modelled. */

#define XAPO_KIND_METER 0
#define XAPO_KIND_REVERB 1
#define XAPO_KIND_EQ 2
#define XAPO_KIND_LIMITER 3
#define XAPO_KIND_FXREVERB 4
#define XAPO_KIND_ECHO 5
#define XAPO_E_FORMAT_UNSUPPORTED_ ((HRESULT)0x88970001)

/* (the parameter blocks: mingw's headers leave them out) */
typedef struct {
  float *pPeakLevels;
  float *pRMSLevels;
  UINT32 ChannelCount;
} MeterLevels;
typedef struct {
  float FrequencyCenter0, Gain0, Bandwidth0, FrequencyCenter1, Gain1, Bandwidth1, FrequencyCenter2, Gain2, Bandwidth2, FrequencyCenter3, Gain3, Bandwidth3;
} EqParameters;
typedef struct {
  UINT32 Release, Loudness;
} LimiterParameters;
typedef struct {
  float Diffusion, RoomSize;
} FxReverbParameters;
typedef struct {
  float WetDryMix, Feedback, Delay;
} EchoParameters;
#define REVERB_PARAMETERS_28 52
#define REVERB_PARAMETERS_29 57

#define COMBS 4
#define ALLPASSES 2
static const UINT32 kCombLengths[COMBS] = {1116, 1188, 1277, 1356}, kAllpassLengths[ALLPASSES] = {556, 441};      /* (at 44.1 kHz, Freeverb's) */

typedef struct {
  float *line;
  UINT32 length, at;
  float filter;
} Delay;

typedef struct {
  IXAPO xapo;
  IXAPOParameters parameters;
  LONG refs;
  int kind;
  CLSID clsid;
  BOOL locked;
  UINT32 in_channels, out_channels, rate, max_frames;
  BYTE params[64];
  UINT32 params_size;
  /* state */
  float peak[MAX_CHANNELS], rms[MAX_CHANNELS];
  Delay combs[2][COMBS], allpasses[2][ALLPASSES], echo[MAX_CHANNELS];
  float biquad[MAX_CHANNELS][4][4];      /* per channel and band: x1 x2 y1 y2 */
  float coef[4][5];                       /* b0 b1 b2 a1 a2 per band */
  float limiter_gain;
} Xapo;

static Xapo *from_xapo(IXAPO *i) { return (Xapo *)i; }
static Xapo *from_xapo_parameters(IXAPOParameters *i) { return CONTAINING_RECORD(i, Xapo, parameters); }

static double x_exp(double x) { return tcos_exp2(x * 1.44269504088896340736); }

static void free_delay(Delay *d) {
  release_memory(d->line);
  zero(d, sizeof *d);
}
static BOOL make_delay(Delay *d, UINT32 length) {
  free_delay(d);
  if (length < 1) length = 1;
  d->line = (float *)alloc((SIZE_T)length * sizeof(float));
  d->length = d->line ? length : 0;
  return d->line != NULL;
}
static void free_state(Xapo *x) {
  for (int c = 0; c < 2; ++c) {
    for (int k = 0; k < COMBS; ++k) free_delay(&x->combs[c][k]);
    for (int k = 0; k < ALLPASSES; ++k) free_delay(&x->allpasses[c][k]);
  }
  for (int c = 0; c < MAX_CHANNELS; ++c) free_delay(&x->echo[c]);
}

/* ---- the parameters each kind reads ------------------------------------------------------------------------------------------------------------- */

static float read_float_at(const Xapo *x, UINT32 at, float fallback) {
  if (at + 4 > x->params_size) return fallback;
  float f;
  copy(&f, x->params + at, 4);
  return f == f ? f : fallback;
}
/* The reverb's wet share (0 .. 1), decay time (s) and room size (0 .. 1) from either kind of reverb's parameters. */
static void reverb_settings(const Xapo *x, float *wet, float *decay, float *room) {
  if (x->kind == XAPO_KIND_FXREVERB) {
    const float diffusion = read_float_at(x, 0, 0.9f), size = read_float_at(x, 4, 0.6f);
    *wet = 0.25f + 0.25f * diffusion;
    *decay = 0.3f + 4.0f * size;
    *room = size;
    return;
  }
  const BOOL v29 = x->params_size >= REVERB_PARAMETERS_29;
  *wet = read_float_at(x, 0, 100.0f) / 100.0f;
  *decay = read_float_at(x, v29 ? 41 : 40, 1.0f);
  const float room_feet = read_float_at(x, v29 ? 49 : 48, 100.0f);      /* (1 .. 100 feet) */
  *room = room_feet / 100.0f;
  if (*wet < 0) *wet = 0;
  if (*wet > 1) *wet = 1;
  if (!(*decay > 0.1f)) *decay = 0.1f;
  if (*room < 0.05f) *room = 0.05f;
  if (*room > 1) *room = 1;
}

static void compute_eq(Xapo *x) {
  EqParameters p = {100, 1, 1, 800, 1, 1, 2000, 1, 1, 10000, 1, 1};
  if (x->params_size >= sizeof p) copy(&p, x->params, sizeof p);
  const float centers[4] = {p.FrequencyCenter0, p.FrequencyCenter1, p.FrequencyCenter2, p.FrequencyCenter3};
  const float gains[4] = {p.Gain0, p.Gain1, p.Gain2, p.Gain3}, widths[4] = {p.Bandwidth0, p.Bandwidth1, p.Bandwidth2, p.Bandwidth3};
  const double pi = 3.14159265358979323846;
  for (int b = 0; b < 4; ++b) {
    double f = centers[b], g = gains[b], bw = widths[b];
    if (!(f > 20)) f = 20;
    if (f > x->rate * 0.45) f = x->rate * 0.45;
    if (!(g > 0.126)) g = 0.126;
    if (g > 7.94) g = 7.94;
    if (!(bw > 0.1)) bw = 0.1;
    if (bw > 2) bw = 2;
    const double A = tcos_sqrt(g), w0 = 2 * pi * f / x->rate, sn = tcos_sin(w0), cs = tcos_cos(w0);
    const double y = 0.69314718055994530942 / 2 * bw * w0 / sn;
    const double alpha = sn * (x_exp(y) - x_exp(-y)) / 2;
    const double a0 = 1 + alpha / A;
    x->coef[b][0] = (float)((1 + alpha * A) / a0);
    x->coef[b][1] = (float)((-2 * cs) / a0);
    x->coef[b][2] = (float)((1 - alpha * A) / a0);
    x->coef[b][3] = (float)((-2 * cs) / a0);
    x->coef[b][4] = (float)((1 - alpha / A) / a0);
  }
}

static BOOL prepare_kind(Xapo *x) {
  free_state(x);
  switch (x->kind) {
    case XAPO_KIND_REVERB: case XAPO_KIND_FXREVERB: {
      float wet, decay, room;
      reverb_settings(x, &wet, &decay, &room);
      const double scale = x->rate / 44100.0 * (0.5 + room);
      for (int c = 0; c < 2; ++c) {
        for (int k = 0; k < COMBS; ++k) if (!make_delay(&x->combs[c][k], (UINT32)(kCombLengths[k] * scale) + c * 23)) return FALSE;
        for (int k = 0; k < ALLPASSES; ++k) if (!make_delay(&x->allpasses[c][k], (UINT32)(kAllpassLengths[k] * x->rate / 44100.0) + c * 23)) return FALSE;
      }
      return TRUE;
    }
    case XAPO_KIND_ECHO: {
      const UINT32 longest = x->rate * 2 + 1;      /* (2 s: the longest delay) */
      for (UINT32 c = 0; c < x->in_channels; ++c) if (!make_delay(&x->echo[c], longest)) return FALSE;
      return TRUE;
    }
    case XAPO_KIND_EQ: compute_eq(x); return TRUE;
    case XAPO_KIND_LIMITER: x->limiter_gain = 1.0f; return TRUE;
    default: return TRUE;
  }
}

/* ---- processing --------------------------------------------------------------------------------------------------------------------------------- */

static float reverb_sample(Xapo *x, int side, float in, float feedback) {
  float out = 0;
  for (int k = 0; k < COMBS; ++k) {
    Delay *d = &x->combs[side][k];
    const float y = d->line[d->at];
    d->filter = y * 0.8f + d->filter * 0.2f;      /* (damping) */
    d->line[d->at] = in + d->filter * feedback;
    d->at = d->at + 1 == d->length ? 0 : d->at + 1;
    out += y;
  }
  for (int k = 0; k < ALLPASSES; ++k) {
    Delay *d = &x->allpasses[side][k];
    const float b = d->line[d->at];
    d->line[d->at] = out + b * 0.5f;
    d->at = d->at + 1 == d->length ? 0 : d->at + 1;
    out = b - out;
  }
  return out * 0.25f;
}

static void process_kind(Xapo *x, const float *in, float *out, UINT32 frames, BOOL enabled) {
  const UINT32 ic = x->in_channels, oc = x->out_channels;
  if (x->kind == XAPO_KIND_METER) {
    for (UINT32 c = 0; c < ic; ++c) {
      float peak = 0, sum = 0;
      for (UINT32 i = 0; i < frames; ++i) {
        const float v = in[(SIZE_T)i * ic + c];
        const float a = v < 0 ? -v : v;
        if (a > peak) peak = a;
        sum += v * v;
      }
      x->peak[c] = peak;
      x->rms[c] = frames ? (float)tcos_sqrt(sum / frames) : 0;
    }
    if (out != in) copy(out, in, (SIZE_T)frames * ic * sizeof(float));
    return;
  }
  if (!enabled) {
    /* (a disabled effect passes its input on, the channels it has in common with its output) */
    if (out == in) return;
    for (UINT32 i = 0; i < frames; ++i)
      for (UINT32 c = 0; c < oc; ++c) out[(SIZE_T)i * oc + c] = c < ic ? in[(SIZE_T)i * ic + c] : (ic == 1 ? in[i] : 0);
    return;
  }
  switch (x->kind) {
    case XAPO_KIND_REVERB: case XAPO_KIND_FXREVERB: {
      float wet, decay, room;
      reverb_settings(x, &wet, &decay, &room);
      const double average_delay = 1250.0 / 44100.0 * (0.5 + room);
      const float feedback = (float)tcos_exp2(-3.0 * 3.32192809488736234787 * average_delay / decay);      /* -60 dB over the decay time */
      for (UINT32 i = 0; i < frames; ++i) {
        const float *a = &in[(SIZE_T)i * ic];
        const float l = a[0], r = ic > 1 ? a[1] : a[0];
        const float wl = reverb_sample(x, 0, (l + r) * 0.5f, feedback), wr = reverb_sample(x, 1, (l + r) * 0.5f, feedback);
        float *o = &out[(SIZE_T)i * oc];
        for (UINT32 c = 0; c < oc; ++c) {
          const float dry = c < ic ? a[c] : (ic == 1 ? a[0] : 0);
          const float w = c % 2 == 0 ? wl : wr;
          o[c] = dry * (1 - wet) + w * wet;
        }
      }
      return;
    }
    case XAPO_KIND_ECHO: {
      EchoParameters p = {0.5f, 0.5f, 500.0f};
      if (x->params_size >= sizeof p) copy(&p, x->params, sizeof p);
      if (!(p.Delay >= 1)) p.Delay = 1;
      if (p.Delay > 2000) p.Delay = 2000;
      const UINT32 lag = (UINT32)(p.Delay * x->rate / 1000);
      for (UINT32 c = 0; c < ic && c < oc; ++c) {
        Delay *d = &x->echo[c];
        if (!d->line) continue;
        for (UINT32 i = 0; i < frames; ++i) {
          const float v = in[(SIZE_T)i * ic + c];
          const UINT32 back = (d->at + d->length - (lag < d->length ? lag : d->length - 1)) % d->length;
          const float echoed = d->line[back];
          d->line[d->at] = v + echoed * p.Feedback;
          d->at = d->at + 1 == d->length ? 0 : d->at + 1;
          out[(SIZE_T)i * oc + c] = v * (1 - p.WetDryMix) + echoed * p.WetDryMix;
        }
      }
      return;
    }
    case XAPO_KIND_EQ:
      for (UINT32 c = 0; c < ic && c < oc && c < MAX_CHANNELS; ++c)
        for (UINT32 i = 0; i < frames; ++i) {
          float v = in[(SIZE_T)i * ic + c];
          for (int b = 0; b < 4; ++b) {
            float *s = x->biquad[c][b];
            const float *k = x->coef[b];
            const float y = k[0] * v + k[1] * s[0] + k[2] * s[1] - k[3] * s[2] - k[4] * s[3];
            s[1] = s[0];
            s[0] = v;
            s[3] = s[2];
            s[2] = y;
            v = y;
          }
          out[(SIZE_T)i * oc + c] = v;
        }
      return;
    case XAPO_KIND_LIMITER: {
      LimiterParameters p = {6, 1000};
      if (x->params_size >= sizeof p) copy(&p, x->params, sizeof p);
      const float makeup = (float)p.Loudness / 1000.0f, ceiling = 0.99f;
      const float release = 1.0f - 1.0f / ((float)(p.Release ? p.Release : 6) * x->rate / 1000.0f);
      for (UINT32 i = 0; i < frames; ++i) {
        float peak = 0;
        for (UINT32 c = 0; c < ic; ++c) {
          const float v = in[(SIZE_T)i * ic + c] * makeup;
          const float a = v < 0 ? -v : v;
          if (a > peak) peak = a;
        }
        const float wanted = peak * x->limiter_gain > ceiling ? ceiling / peak : 1.0f;
        x->limiter_gain = wanted < x->limiter_gain ? wanted : x->limiter_gain + (1.0f - x->limiter_gain) * (1.0f - release);
        if (x->limiter_gain > 1) x->limiter_gain = 1;
        for (UINT32 c = 0; c < ic && c < oc; ++c) out[(SIZE_T)i * oc + c] = in[(SIZE_T)i * ic + c] * makeup * x->limiter_gain;
      }
      return;
    }
    default: return;
  }
}

/* ---- IXAPO and IXAPOParameters ------------------------------------------------------------------------------------------------------------------ */

static HRESULT WINAPI xp_query(IXAPO *i, REFIID riid, void **out) {
  Xapo *x = from_xapo(i);
  if (!out) return E_POINTER;
  *out = NULL;
  if (same_guid(riid, &kIidUnknown) || same_guid(riid, &IID_IXAPO)) *out = &x->xapo;
  else if (same_guid(riid, &IID_IXAPOParameters)) *out = &x->parameters;
  else return E_NOINTERFACE;
  InterlockedIncrement(&x->refs);
  return S_OK;
}
static ULONG WINAPI xp_add_ref(IXAPO *i) { return (ULONG)InterlockedIncrement(&from_xapo(i)->refs); }
static ULONG WINAPI xp_release(IXAPO *i) {
  Xapo *x = from_xapo(i);
  const LONG left = InterlockedDecrement(&x->refs);
  if (left == 0) {
    free_state(x);
    release_memory(x);
  }
  return (ULONG)left;
}
static HRESULT WINAPI xp_registration(IXAPO *i, XAPO_REGISTRATION_PROPERTIES **props) {
  Xapo *x = from_xapo(i);
  if (!props) return E_POINTER;
  XAPO_REGISTRATION_PROPERTIES *p = (XAPO_REGISTRATION_PROPERTIES *)CoTaskMemAlloc(sizeof *p);
  if (!p) return E_OUTOFMEMORY;
  zero(p, sizeof *p);
  p->clsid = x->clsid;
  static const WCHAR kName[] = L"TheoryCraftOS XAPO", kCopyright[] = L"TheoryCraftOS";
  copy(p->FriendlyName, kName, sizeof kName);
  copy(p->CopyrightInfo, kCopyright, sizeof kCopyright);
  p->MajorVersion = 1;
  p->Flags = XAPO_FLAG_FRAMERATE_MUST_MATCH | XAPO_FLAG_BITSPERSAMPLE_MUST_MATCH | XAPO_FLAG_BUFFERCOUNT_MUST_MATCH | XAPO_FLAG_INPLACE_SUPPORTED |
             (x->kind == XAPO_KIND_REVERB ? 0 : XAPO_FLAG_CHANNELS_MUST_MATCH);
  p->MinInputBufferCount = p->MaxInputBufferCount = p->MinOutputBufferCount = p->MaxOutputBufferCount = 1;
  *props = p;
  return S_OK;
}
static BOOL float_format(const WAVEFORMATEX *f) {
  return f && (f->wFormatTag == WAVE_FORMAT_IEEE_FLOAT || wave_format_tag(f) == WAVE_FORMAT_IEEE_FLOAT) && f->wBitsPerSample == 32 && f->nChannels >= 1 &&
         f->nChannels <= MAX_CHANNELS;
}
static HRESULT format_answer(const WAVEFORMATEX *want, const WAVEFORMATEX *other, WAVEFORMATEX **supported, BOOL ok) {
  if (ok) {
    if (supported) *supported = NULL;
    return S_OK;
  }
  if (supported) {
    WAVEFORMATEX *f = (WAVEFORMATEX *)CoTaskMemAlloc(sizeof *f);
    if (f) {
      zero(f, sizeof *f);
      f->wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
      f->nChannels = other ? other->nChannels : (want ? want->nChannels : 2);
      f->nSamplesPerSec = other ? other->nSamplesPerSec : DEFAULT_RATE;
      f->wBitsPerSample = 32;
      f->nBlockAlign = (WORD)(4 * f->nChannels);
      f->nAvgBytesPerSec = f->nSamplesPerSec * f->nBlockAlign;
    }
    *supported = f;
  }
  return XAPO_E_FORMAT_UNSUPPORTED_;
}
static HRESULT WINAPI xp_input_supported(IXAPO *i, const WAVEFORMATEX *out_f, const WAVEFORMATEX *in_f, WAVEFORMATEX **supported) {
  Xapo *x = from_xapo(i);
  const BOOL ok = float_format(in_f) && (!out_f || in_f->nSamplesPerSec == out_f->nSamplesPerSec) &&
                  (x->kind == XAPO_KIND_REVERB || !out_f || in_f->nChannels == out_f->nChannels) && (x->kind != XAPO_KIND_REVERB || in_f->nChannels <= 2);
  return format_answer(in_f, out_f, supported, ok);
}
static HRESULT WINAPI xp_output_supported(IXAPO *i, const WAVEFORMATEX *in_f, const WAVEFORMATEX *out_f, WAVEFORMATEX **supported) {
  Xapo *x = from_xapo(i);
  const BOOL ok = float_format(out_f) && (!in_f || in_f->nSamplesPerSec == out_f->nSamplesPerSec) && (x->kind == XAPO_KIND_REVERB || !in_f || in_f->nChannels == out_f->nChannels);
  return format_answer(out_f, in_f, supported, ok);
}
static HRESULT WINAPI xp_initialize(IXAPO *i, const void *data, UINT32 size) {
  Xapo *x = from_xapo(i);
  if (data && size) {
    if (size > sizeof x->params) return E_INVALIDARG;
    copy(x->params, data, size);
    x->params_size = size;
  }
  return S_OK;
}
static void WINAPI xp_reset(IXAPO *i) {
  Xapo *x = from_xapo(i);
  zero(x->biquad, sizeof x->biquad);
  x->limiter_gain = 1.0f;
  if (x->locked) prepare_kind(x);
}
static HRESULT WINAPI xp_lock(IXAPO *i, UINT32 in_count, const XAPO_LOCKFORPROCESS_BUFFER_PARAMETERS *in, UINT32 out_count, const XAPO_LOCKFORPROCESS_BUFFER_PARAMETERS *out) {
  Xapo *x = from_xapo(i);
  if (x->locked) return XAUDIO2_E_INVALID_CALL;
  if (in_count != 1 || out_count != 1 || !in || !out || !float_format(in->pFormat) || !float_format(out->pFormat)) return XAPO_E_FORMAT_UNSUPPORTED_;
  if (in->pFormat->nSamplesPerSec != out->pFormat->nSamplesPerSec) return XAPO_E_FORMAT_UNSUPPORTED_;
  if (x->kind != XAPO_KIND_REVERB && in->pFormat->nChannels != out->pFormat->nChannels) return XAPO_E_FORMAT_UNSUPPORTED_;
  if (x->kind == XAPO_KIND_REVERB && in->pFormat->nChannels > 2) return XAPO_E_FORMAT_UNSUPPORTED_;
  x->in_channels = in->pFormat->nChannels;
  x->out_channels = out->pFormat->nChannels;
  x->rate = in->pFormat->nSamplesPerSec;
  x->max_frames = in->MaxFrameCount;
  if (!prepare_kind(x)) return E_OUTOFMEMORY;
  x->locked = TRUE;
  return S_OK;
}
static void WINAPI xp_unlock(IXAPO *i) {
  Xapo *x = from_xapo(i);
  x->locked = FALSE;
  free_state(x);
}
static void WINAPI xp_process(IXAPO *i, UINT32 in_count, const XAPO_PROCESS_BUFFER_PARAMETERS *in, UINT32 out_count, XAPO_PROCESS_BUFFER_PARAMETERS *out, WINBOOL enabled) {
  Xapo *x = from_xapo(i);
  if (!x->locked || in_count != 1 || out_count != 1 || !in || !out || !in->pBuffer || !out->pBuffer) return;
  UINT32 frames = in->ValidFrameCount;
  if (x->max_frames && frames > x->max_frames) frames = x->max_frames;
  if (in->BufferFlags == XAPO_BUFFER_SILENT && in->pBuffer != out->pBuffer) zero(in->pBuffer, (SIZE_T)frames * x->in_channels * sizeof(float));
  process_kind(x, (const float *)in->pBuffer, (float *)out->pBuffer, frames, enabled);
  out->ValidFrameCount = frames;
  out->BufferFlags = XAPO_BUFFER_VALID;
}
static UINT32 WINAPI xp_calc_input(IXAPO *i, UINT32 frames) { (void)i; return frames; }
static UINT32 WINAPI xp_calc_output(IXAPO *i, UINT32 frames) { (void)i; return frames; }
static const IXAPOVtbl kXapoVtbl = {xp_query, xp_add_ref, xp_release, xp_registration, xp_input_supported, xp_output_supported, xp_initialize, xp_reset,
                                    xp_lock, xp_unlock, xp_process, xp_calc_input, xp_calc_output};

static HRESULT WINAPI xpp_query(IXAPOParameters *i, REFIID riid, void **out) { return xp_query(&from_xapo_parameters(i)->xapo, riid, out); }
static ULONG WINAPI xpp_add_ref(IXAPOParameters *i) { return xp_add_ref(&from_xapo_parameters(i)->xapo); }
static ULONG WINAPI xpp_release(IXAPOParameters *i) { return xp_release(&from_xapo_parameters(i)->xapo); }
static void WINAPI xpp_set(IXAPOParameters *i, const void *p, UINT32 size) {
  Xapo *x = from_xapo_parameters(i);
  if (!p || size > sizeof x->params) return;
  copy(x->params, p, size);
  x->params_size = size;
  if (x->kind == XAPO_KIND_EQ && x->locked) compute_eq(x);
}
static void WINAPI xpp_get(IXAPOParameters *i, void *p, UINT32 size) {
  Xapo *x = from_xapo_parameters(i);
  if (!p) return;
  if (x->kind == XAPO_KIND_METER) {
    if (size < sizeof(MeterLevels)) return;
    MeterLevels *levels = (MeterLevels *)p;
    for (UINT32 c = 0; c < levels->ChannelCount && c < MAX_CHANNELS; ++c) {
      if (levels->pPeakLevels) levels->pPeakLevels[c] = c < x->in_channels ? x->peak[c] : 0;
      if (levels->pRMSLevels) levels->pRMSLevels[c] = c < x->in_channels ? x->rms[c] : 0;
    }
    return;
  }
  copy(p, x->params, size < x->params_size ? size : x->params_size);
}
static const IXAPOParametersVtbl kXapoParametersVtbl = {xpp_query, xpp_add_ref, xpp_release, xpp_set, xpp_get};

static HRESULT new_xapo(int kind, const CLSID *clsid, IUnknown **out, const void *init, UINT32 init_size) {
  if (!out) return E_POINTER;
  *out = NULL;
  Xapo *x = (Xapo *)alloc(sizeof *x);
  if (!x) return E_OUTOFMEMORY;
  x->xapo.lpVtbl = (IXAPOVtbl *)&kXapoVtbl;
  x->parameters.lpVtbl = (IXAPOParametersVtbl *)&kXapoParametersVtbl;
  x->refs = 1;
  x->kind = kind;
  x->clsid = *clsid;
  x->limiter_gain = 1.0f;
  if (init && init_size) {
    if (init_size > sizeof x->params) {
      release_memory(x);
      return E_INVALIDARG;
    }
    copy(x->params, init, init_size);
    x->params_size = init_size;
  }
  *out = (IUnknown *)&x->xapo;
  return S_OK;
}

static const CLSID kClsidVolumeMeter = {0x4fc3b166, 0x972a, 0x40cf, {0xbc, 0x37, 0x7d, 0xb0, 0x3d, 0xb2, 0xfb, 0xa3}};
static const CLSID kClsidReverb = {0xc2633b16, 0x471b, 0x4498, {0xb8, 0xc5, 0x4f, 0x09, 0x59, 0xe2, 0xec, 0x09}};

EXPORT HRESULT WINAPI CreateAudioVolumeMeter(IUnknown **out) { return new_xapo(XAPO_KIND_METER, &kClsidVolumeMeter, out, NULL, 0); }
EXPORT HRESULT WINAPI CreateAudioReverb(IUnknown **out) { return new_xapo(XAPO_KIND_REVERB, &kClsidReverb, out, NULL, 0); }
EXPORT HRESULT WINAPI CreateFX(REFCLSID clsid, IUnknown **out, const void *init, UINT32 init_size) {
  if (!clsid) return E_POINTER;
  if (same_guid(clsid, &CLSID_FXEQ) || same_guid(clsid, &CLSID_FXEQ27)) return new_xapo(XAPO_KIND_EQ, clsid, out, init, init_size);
  if (same_guid(clsid, &CLSID_FXMasteringLimiter) || same_guid(clsid, &CLSID_FXMasteringLimiter27)) return new_xapo(XAPO_KIND_LIMITER, clsid, out, init, init_size);
  if (same_guid(clsid, &CLSID_FXReverb) || same_guid(clsid, &CLSID_FXReverb27)) return new_xapo(XAPO_KIND_FXREVERB, clsid, out, init, init_size);
  if (same_guid(clsid, &CLSID_FXEcho) || same_guid(clsid, &CLSID_FXEcho27)) return new_xapo(XAPO_KIND_ECHO, clsid, out, init, init_size);
  if (out) *out = NULL;
  return CLASS_E_CLASSNOTAVAILABLE;
}
