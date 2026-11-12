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

/* XAudio2 2.9 (xaudio2_9.dll) over the kernel's PipeWire graph, on the CPU emulator: a 16-bit mono 24 kHz source voice loops 400 Hz into the mastering
 * voice while a 48 kHz float stereo voice plays 600 Hz through a submix voice (resampled to 24 kHz, folded to mono). The mastering voice is mono at
 * 24 kHz: the emulator's floating point is slow, and a quarter of the work keeps the engine up with real time; tests/pipewire_emulated.sh checks both tones in the mix, with the native
 * stream and pulsetest's. Also: voice details and the channel mask, callbacks (passes, buffer start/end, loop end, stream end), loops, operation sets
 * and CommitChanges, output matrices, the frequency ratio, errors, the volume meter on a voice, MS-ADPCM, CreateFX and the reverb, X3DAudio.
 * Prints "xaudio2test: N checks, M failed". */
#define COBJMACROS
#include <windows.h>
#include <mmreg.h>
#include <initguid.h>
#include <xaudio2.h>
#include <xaudio2fx.h>
#include <xapofx.h>
#include <x3daudio.h>
#include <stdio.h>
#include <string.h>

static int checks, failed;
#define CHECK(x) do { ++checks; if (!(x)) { ++failed; printf("FAILED line %d: %s\n", __LINE__, #x); } } while (0)
#define XAUDIO2_E_INVALID_CALL_ ((HRESULT)0x88960001)
__declspec(dllimport) HRESULT WINAPI CreateFX(REFCLSID clsid, IUnknown **effect, const void *init, UINT32 size);      /* (mingw's xapofx.h leaves it out) */
#define END_OF_STREAM_ 0x40
#define USEFILTER_ 0x8

typedef struct {
  IXAudio2VoiceCallback iface;
  volatile LONG passes, starts, ends, loops, stream_ends;
  HANDLE ended;
} VoiceWatch;
static void WINAPI vw_pass_start(IXAudio2VoiceCallback *i, UINT32 bytes) { (void)bytes; InterlockedIncrement(&((VoiceWatch *)i)->passes); }
static void WINAPI vw_pass_end(IXAudio2VoiceCallback *i) { (void)i; }
static void WINAPI vw_stream_end(IXAudio2VoiceCallback *i) {
  VoiceWatch *w = (VoiceWatch *)i;
  InterlockedIncrement(&w->stream_ends);
  if (w->ended) SetEvent(w->ended);
}
static void WINAPI vw_buffer_start(IXAudio2VoiceCallback *i, void *c) { (void)c; InterlockedIncrement(&((VoiceWatch *)i)->starts); }
static void WINAPI vw_buffer_end(IXAudio2VoiceCallback *i, void *c) { (void)c; InterlockedIncrement(&((VoiceWatch *)i)->ends); }
static void WINAPI vw_loop_end(IXAudio2VoiceCallback *i, void *c) { (void)c; InterlockedIncrement(&((VoiceWatch *)i)->loops); }
static void WINAPI vw_error(IXAudio2VoiceCallback *i, void *c, HRESULT e) { (void)i; (void)c; (void)e; }
static IXAudio2VoiceCallbackVtbl kVoiceWatchVtbl = {vw_pass_start, vw_pass_end, vw_stream_end, vw_buffer_start, vw_buffer_end, vw_loop_end, vw_error};

typedef struct {
  IXAudio2EngineCallback iface;
  volatile LONG passes;
} EngineWatch;
static void WINAPI ew_start(IXAudio2EngineCallback *i) { InterlockedIncrement(&((EngineWatch *)i)->passes); }
static void WINAPI ew_end(IXAudio2EngineCallback *i) { (void)i; }
static void WINAPI ew_error(IXAudio2EngineCallback *i, HRESULT e) { (void)i; (void)e; }
static IXAudio2EngineCallbackVtbl kEngineWatchVtbl = {ew_start, ew_end, ew_error};

static const short kSine24k400[60] = {0,836,1663,2472,3253,4000,4702,5353,5945,6472,6928,7307,7608,7826,7961,8000,7961,7826,7608,7307,6928,6472,5945,5353,4702,4000,3253,2472,1663,836,0,-836,-1663,-2472,-3253,-4000,-4702,-5353,-5945,-6472,-6928,-7307,-7608,-7826,-7961,-8000,-7961,-7826,-7608,-7307,-6928,-6472,-5945,-5353,-4702,-4000,-3253,-2472,-1663,-836};
static short g_tone[24000];       /* 1 s at 24 kHz: 400 whole periods */
static float g_tone2[48000 * 2];  /* 1 s at 48 kHz stereo: 600 Hz at 0.25 */
static short g_short[2400];       /* 0.1 s */

static double tsin(double x) {
  const double pi = 3.14159265358979323846;
  while (x > pi) x -= 2 * pi;
  while (x < -pi) x += 2 * pi;
  double term = x, sum = x;
  for (int n = 1; n < 10; ++n) { term *= -x * x / ((2.0 * n) * (2.0 * n + 1.0)); sum += term; }
  return sum;
}

int main(void) {
  for (int i = 0; i < 24000; ++i) g_tone[i] = kSine24k400[i % 60];
  for (int i = 0; i < 48000; ++i) {      /* (a table of one period: 80 samples at 48 kHz) */
    static float period[80];
    if (i < 80) period[i] = (float)(0.25 * tsin(2 * 3.14159265358979323846 * i / 80.0));
    g_tone2[2 * i] = g_tone2[2 * i + 1] = period[i % 80];
  }
  IXAudio2 *xa = NULL;
  const DWORD began = GetTickCount();
  CHECK(XAudio2Create(&xa, 0, XAUDIO2_DEFAULT_PROCESSOR) == S_OK && xa);
  if (!xa) return 1;
  EngineWatch ew = {{&kEngineWatchVtbl}, 0};
  CHECK(IXAudio2_RegisterForCallbacks(xa, &ew.iface) == S_OK);
  IXAudio2MasteringVoice *master = NULL, *second = NULL;
  CHECK(IXAudio2_CreateMasteringVoice(xa, &master, 1, 24000, 0, NULL, NULL, AudioCategory_GameEffects) == S_OK && master);
  if (!master) return 1;
  CHECK(IXAudio2_CreateMasteringVoice(xa, &second, 2, 48000, 0, NULL, NULL, AudioCategory_GameEffects) == XAUDIO2_E_INVALID_CALL_);
  XAUDIO2_VOICE_DETAILS d;
  IXAudio2MasteringVoice_GetVoiceDetails(master, &d);
  CHECK(d.InputChannels == 1 && d.InputSampleRate == 24000);
  DWORD mask = 0;
  CHECK(IXAudio2MasteringVoice_GetChannelMask(master, &mask) == S_OK && mask == SPEAKER_FRONT_CENTER);

  /* the 400 Hz loop: 16-bit mono at 24 kHz, both sides full */
  WAVEFORMATEX mono = {WAVE_FORMAT_PCM, 1, 24000, 48000, 2, 16, 0};
  VoiceWatch w1 = {{&kVoiceWatchVtbl}, 0, 0, 0, 0, 0, NULL};
  IXAudio2SourceVoice *tone = NULL;
  CHECK(IXAudio2_CreateSourceVoice(xa, &tone, &mono, 0, XAUDIO2_DEFAULT_FREQ_RATIO, &w1.iface, NULL, NULL) == S_OK && tone);
  XAUDIO2_BUFFER loop = {0, sizeof g_tone, (const BYTE *)g_tone, 0, 0, 0, 0, XAUDIO2_LOOP_INFINITE, NULL};
  if (tone) {
    CHECK(IXAudio2SourceVoice_SubmitSourceBuffer(tone, &loop, NULL) == S_OK);
    CHECK(IXAudio2SourceVoice_Start(tone, 0, XAUDIO2_COMMIT_NOW) == S_OK);
  }
  /* the 600 Hz voice through a submix */
  IXAudio2SubmixVoice *submix = NULL;
  CHECK(IXAudio2_CreateSubmixVoice(xa, &submix, 1, 24000, 0, 0, NULL, NULL) == S_OK && submix);
  WAVEFORMATEX stereo = {WAVE_FORMAT_IEEE_FLOAT, 2, 48000, 48000 * 8, 8, 32, 0};
  XAUDIO2_SEND_DESCRIPTOR to_submix = {0, (IXAudio2Voice *)submix};
  XAUDIO2_VOICE_SENDS sends = {1, &to_submix};
  IXAudio2SourceVoice *tone2 = NULL;
  CHECK(IXAudio2_CreateSourceVoice(xa, &tone2, &stereo, 0, 2.0f, NULL, &sends, NULL) == S_OK && tone2);
  XAUDIO2_BUFFER loop2 = {0, sizeof g_tone2, (const BYTE *)g_tone2, 0, 0, 0, 0, XAUDIO2_LOOP_INFINITE, NULL};
  if (tone2) {
    CHECK(IXAudio2SourceVoice_SubmitSourceBuffer(tone2, &loop2, NULL) == S_OK);
    CHECK(IXAudio2SourceVoice_Start(tone2, 0, 0) == S_OK);
  }
  /* a send to a voice of another rate is refused (only source voices convert) */
  IXAudio2SubmixVoice *slow = NULL;
  CHECK(IXAudio2_CreateSubmixVoice(xa, &slow, 1, 22050, 0, 0, NULL, NULL) == XAUDIO2_E_INVALID_CALL_);
  /* output matrix round trip, the default matrices (stereo into mono: half of each) */
  float m[2] = {0, 0};
  if (tone && tone2) {
    IXAudio2SourceVoice_GetOutputMatrix(tone2, (IXAudio2Voice *)submix, 2, 1, m);
    CHECK(m[0] == 0.5f && m[1] == 0.5f);
    IXAudio2SourceVoice_GetOutputMatrix(tone, (IXAudio2Voice *)master, 1, 1, m);
    CHECK(m[0] == 1.0f);
    const float half[1] = {0.5f};
    CHECK(IXAudio2SourceVoice_SetOutputMatrix(tone, NULL, 1, 1, half, 0) == S_OK);
    IXAudio2SourceVoice_GetOutputMatrix(tone, (IXAudio2Voice *)master, 1, 1, m);
    CHECK(m[0] == 0.5f);
    const float full[2] = {1.0f, 1.0f};
    CHECK(IXAudio2SourceVoice_SetOutputMatrix(tone, NULL, 1, 1, full, 0) == S_OK);
    CHECK(IXAudio2SourceVoice_SetOutputMatrix(tone, NULL, 1, 2, full, 0) == E_INVALIDARG);
  }
  /* operation sets: nothing changes until CommitChanges */
  float volume = 0;
  if (tone2) {
    CHECK(IXAudio2SourceVoice_SetVolume(tone2, 0.5f, 7) == S_OK);
    IXAudio2SourceVoice_GetVolume(tone2, &volume);
    CHECK(volume == 1.0f);
    CHECK(IXAudio2_CommitChanges(xa, 7) == S_OK);
    IXAudio2SourceVoice_GetVolume(tone2, &volume);
    CHECK(volume == 0.5f);
    CHECK(IXAudio2SourceVoice_SetVolume(tone2, 1.0f, XAUDIO2_COMMIT_NOW) == S_OK);
  }
  /* errors */
  if (tone) {
    XAUDIO2_FILTER_PARAMETERS f = {LowPassFilter, 0.5f, 1.0f};
    CHECK(IXAudio2SourceVoice_SetFilterParameters(tone, &f, 0) == XAUDIO2_E_INVALID_CALL_);      /* (made without XAUDIO2_VOICE_USEFILTER) */
    XAUDIO2_BUFFER bad = loop;
    bad.LoopCount = 0;
    bad.PlayBegin = 30000;
    CHECK(IXAudio2SourceVoice_SubmitSourceBuffer(tone, &bad, NULL) == XAUDIO2_E_INVALID_CALL_);
  }
  WAVEFORMATEX wma = {0x0161, 2, 44100, 16000, 2230, 16, 0};
  IXAudio2SourceVoice *nope = NULL;
  CHECK(IXAudio2_CreateSourceVoice(xa, &nope, &wma, 0, 2.0f, NULL, NULL, NULL) == XAUDIO2_E_INVALID_CALL_ && nope == NULL);

  /* a one-shot buffer and a loop of two: callbacks, END_OF_STREAM, the samples played */
  for (int i = 0; i < 2400; ++i) g_short[i] = (short)(i % 2 ? 100 : -100);
  VoiceWatch w2 = {{&kVoiceWatchVtbl}, 0, 0, 0, 0, 0, CreateEventA(NULL, FALSE, FALSE, NULL)};
  IXAudio2SourceVoice *shot = NULL;
  CHECK(IXAudio2_CreateSourceVoice(xa, &shot, &mono, USEFILTER_, 2.0f, &w2.iface, NULL, NULL) == S_OK && shot);
  if (shot) {
    XAUDIO2_FILTER_PARAMETERS f = {LowPassFilter, 0.3f, 1.0f}, got;
    CHECK(IXAudio2SourceVoice_SetFilterParameters(shot, &f, 0) == S_OK);
    IXAudio2SourceVoice_GetFilterParameters(shot, &got);
    CHECK(got.Frequency == 0.3f);
    IXAudio2SourceVoice_SetVolume(shot, 0.0f, 0);
    XAUDIO2_BUFFER twice = {0, sizeof g_short, (const BYTE *)g_short, 0, 0, 0, 1200, 2, (void *)1};
    XAUDIO2_BUFFER last = {END_OF_STREAM_, sizeof g_short, (const BYTE *)g_short, 0, 0, 0, 0, 0, (void *)2};
    CHECK(IXAudio2SourceVoice_SubmitSourceBuffer(shot, &twice, NULL) == S_OK && IXAudio2SourceVoice_SubmitSourceBuffer(shot, &last, NULL) == S_OK);
    XAUDIO2_VOICE_STATE st;
    IXAudio2SourceVoice_GetState(shot, &st, 0);
    CHECK(st.BuffersQueued == 2 && st.pCurrentBufferContext == (void *)1 && st.SamplesPlayed == 0);
    CHECK(IXAudio2SourceVoice_Start(shot, 0, 0) == S_OK);
    CHECK(WaitForSingleObject(w2.ended, 5000) == WAIT_OBJECT_0);
    IXAudio2SourceVoice_GetState(shot, &st, 0);
    /* the loop region (the first 1200 frames) twice more, then the rest; then the second buffer */
    CHECK(st.BuffersQueued == 0 && st.SamplesPlayed == 2400 + 2 * 1200 + 2400);
    CHECK(w2.starts == 2 && w2.ends == 2 && w2.loops == 2 && w2.stream_ends == 1 && w2.passes > 0);
    IXAudio2SourceVoice_DestroyVoice(shot);
  }

  /* the frequency ratio: two silent voices on the same loop, one at twice the ratio */
  IXAudio2SourceVoice *a = NULL, *b = NULL;
  CHECK(IXAudio2_CreateSourceVoice(xa, &a, &mono, 0, 4.0f, NULL, NULL, NULL) == S_OK && IXAudio2_CreateSourceVoice(xa, &b, &mono, 0, 4.0f, NULL, NULL, NULL) == S_OK);
  if (a && b) {
    IXAudio2SourceVoice_SetVolume(a, 0, 0);
    IXAudio2SourceVoice_SetVolume(b, 0, 0);
    CHECK(IXAudio2SourceVoice_SetFrequencyRatio(b, 2.0f, 0) == S_OK);
    float ratio = 0;
    IXAudio2SourceVoice_GetFrequencyRatio(b, &ratio);
    CHECK(ratio == 2.0f);
    IXAudio2SourceVoice_SubmitSourceBuffer(a, &loop, NULL);
    IXAudio2SourceVoice_SubmitSourceBuffer(b, &loop, NULL);
    /* (started in one operation set, so that they start in the same pass) */
    IXAudio2SourceVoice_Start(a, 0, 9);
    IXAudio2SourceVoice_Start(b, 0, 9);
    IXAudio2_CommitChanges(xa, 9);
    Sleep(300);
    XAUDIO2_VOICE_STATE sa, sb;
    IXAudio2SourceVoice_GetState(a, &sa, 0);
    IXAudio2SourceVoice_GetState(b, &sb, 0);
    const double r = sa.SamplesPlayed ? (double)sb.SamplesPlayed / (double)sa.SamplesPlayed : 0;
    printf("frequency ratio 2: read %.3f times as fast\n", r);
    CHECK(r > 1.95 && r < 2.05);
    IXAudio2SourceVoice_DestroyVoice(a);
    IXAudio2SourceVoice_DestroyVoice(b);
  }

  /* MS-ADPCM with a volume meter on the voice: blocks of zero codes from 1000, 1000 decode to 1000 throughout */
  {
    BYTE raw[sizeof(ADPCMWAVEFORMAT) + 7 * sizeof(ADPCMCOEFSET)];
    memset(raw, 0, sizeof raw);
    ADPCMWAVEFORMAT *af = (ADPCMWAVEFORMAT *)raw;
    static const short kCoef[7][2] = {{256, 0}, {512, -256}, {0, 0}, {192, 64}, {240, 0}, {460, -208}, {392, -232}};
    af->wfx.wFormatTag = WAVE_FORMAT_ADPCM;
    af->wfx.nChannels = 1;
    af->wfx.nSamplesPerSec = 24000;
    af->wfx.nBlockAlign = 256;
    af->wfx.wBitsPerSample = 4;
    af->wfx.cbSize = 4 + 7 * 4;
    af->wSamplesPerBlock = 500;      /* 2 + (256 - 7) * 2 */
    af->wNumCoef = 7;
    for (int k = 0; k < 7; ++k) { af->aCoef[k].iCoef1 = kCoef[k][0]; af->aCoef[k].iCoef2 = kCoef[k][1]; }
    af->wfx.nAvgBytesPerSec = 24000 * 256 / 500;
    static BYTE blocks[256 * 48];      /* 48 blocks: 1 s */
    for (int k = 0; k < 48; ++k) {
      BYTE *p = blocks + 256 * k;
      p[0] = 0;                         /* predictor 0 */
      p[1] = 16; p[2] = 0;              /* delta */
      p[3] = 1000 & 0xFF; p[4] = 1000 >> 8;      /* sample 1 */
      p[5] = 1000 & 0xFF; p[6] = 1000 >> 8;      /* sample 2 */
    }
    IUnknown *meter = NULL;
    CHECK(CreateAudioVolumeMeter(&meter) == S_OK && meter);
    XAUDIO2_EFFECT_DESCRIPTOR fx = {meter, TRUE, 1};
    XAUDIO2_EFFECT_CHAIN chain = {1, &fx};
    IXAudio2SourceVoice *adpcm = NULL;
    CHECK(IXAudio2_CreateSourceVoice(xa, &adpcm, &af->wfx, 0, 2.0f, NULL, NULL, &chain) == S_OK && adpcm);
    if (meter) IUnknown_Release(meter);      /* (the voice holds it) */
    if (adpcm) {
      IXAudio2SourceVoice_SetVolume(adpcm, 0, 0);
      XAUDIO2_BUFFER ab = {0, sizeof blocks, blocks, 0, 0, 0, 0, XAUDIO2_LOOP_INFINITE, NULL};
      CHECK(IXAudio2SourceVoice_SubmitSourceBuffer(adpcm, &ab, NULL) == S_OK);
      CHECK(IXAudio2SourceVoice_Start(adpcm, 0, 0) == S_OK);
      Sleep(200);
      float peak = 0, rms = 0;
      struct { float *peaks; float *rmss; UINT32 count; } levels = {&peak, &rms, 1};
      CHECK(IXAudio2SourceVoice_GetEffectParameters(adpcm, 0, &levels, sizeof levels) == S_OK);
      printf("ADPCM through the meter: peak %.4f, rms %.4f\n", peak, rms);
      CHECK(peak > 0.029f && peak < 0.032f && rms > 0.029f && rms < 0.032f);
      IXAudio2SourceVoice_DestroyVoice(adpcm);
    }
  }

  /* effects on the submix: CreateFX echo and the reverb, an EQ; disabled and enabled */
  if (submix) {
    IUnknown *echo = NULL, *eq = NULL;
    CHECK(CreateFX(&CLSID_FXEcho, &echo, NULL, 0) == S_OK && echo && CreateFX(&CLSID_FXEQ, &eq, NULL, 0) == S_OK && eq);
    IUnknown *none = (IUnknown *)1;
    CHECK(CreateFX(&IID_IUnknown, &none, NULL, 0) == CLASS_E_CLASSNOTAVAILABLE && none == NULL);
    if (echo && eq) {
      XAUDIO2_EFFECT_DESCRIPTOR fx[2] = {{echo, FALSE, 1}, {eq, TRUE, 1}};
      XAUDIO2_EFFECT_CHAIN chain = {2, fx};
      CHECK(IXAudio2SubmixVoice_SetEffectChain(submix, &chain) == S_OK);
      WINBOOL on = TRUE;
      IXAudio2SubmixVoice_GetEffectState(submix, 0, &on);
      CHECK(!on);
      IXAudio2SubmixVoice_GetEffectState(submix, 1, &on);
      CHECK(on);
      CHECK(IXAudio2SubmixVoice_EnableEffect(submix, 0, 0) == S_OK && IXAudio2SubmixVoice_DisableEffect(submix, 0, 0) == S_OK);
      CHECK(IXAudio2SubmixVoice_EnableEffect(submix, 5, 0) == E_INVALIDARG);
      IUnknown_Release(echo);
      IUnknown_Release(eq);
    }
    IUnknown *reverb = NULL;
    CHECK(CreateAudioReverb(&reverb) == S_OK && reverb);
    if (reverb) IUnknown_Release(reverb);
  }

  /* X3DAudio: an emitter to the right is louder on the right; twice the scaler away it is at half; coming closer it is higher */
  X3DAUDIO_HANDLE x3d;
  CHECK(X3DAudioInitialize(SPEAKER_STEREO, X3DAUDIO_SPEED_OF_SOUND, x3d) == S_OK);
  X3DAUDIO_LISTENER listener;
  memset(&listener, 0, sizeof listener);
  listener.OrientFront.z = 1;
  listener.OrientTop.y = 1;
  X3DAUDIO_EMITTER emitter;
  memset(&emitter, 0, sizeof emitter);
  emitter.OrientFront.z = 1;
  emitter.OrientTop.y = 1;
  emitter.ChannelCount = 1;
  emitter.CurveDistanceScaler = 1;
  emitter.DopplerScaler = 1;
  emitter.Position.x = 2;      /* right */
  emitter.Velocity.x = -34.35f;      /* coming at the listener */
  float matrix[2] = {0, 0};
  X3DAUDIO_DSP_SETTINGS dsp;
  memset(&dsp, 0, sizeof dsp);
  dsp.SrcChannelCount = 1;
  dsp.DstChannelCount = 2;
  dsp.pMatrixCoefficients = matrix;
  X3DAudioCalculate(x3d, &listener, &emitter, X3DAUDIO_CALCULATE_MATRIX | X3DAUDIO_CALCULATE_DOPPLER, &dsp);
  printf("X3DAudio: left %.3f right %.3f, distance %.2f, Doppler %.3f\n", matrix[0], matrix[1], dsp.EmitterToListenerDistance, dsp.DopplerFactor);
  CHECK(matrix[1] > 0.45f && matrix[0] < 0.05f && dsp.EmitterToListenerDistance > 1.99f && dsp.EmitterToListenerDistance < 2.01f);
  CHECK(dsp.DopplerFactor > 1.10f && dsp.DopplerFactor < 1.12f);
  CHECK(matrix[0] * matrix[0] + matrix[1] * matrix[1] > 0.24f && matrix[0] * matrix[0] + matrix[1] * matrix[1] < 0.26f);      /* (1/2 of full power-wise) */

  /* the tones play on (the script checks them in the mix) */
  XAUDIO2_PERFORMANCE_DATA early;
  IXAudio2_GetPerformanceData(xa, &early);      /* (from here on: the cycles of the last 2.5 s) */
  const LONG passes_before = ew.passes;
  const DWORD slept_at = GetTickCount();
  Sleep(2500);
  printf("xaudio2: %ld passes in the last %lu ms\n", ew.passes - passes_before, GetTickCount() - slept_at);
  XAUDIO2_PERFORMANCE_DATA perf;
  IXAudio2_GetPerformanceData(xa, &perf);
  CHECK(perf.ActiveSourceVoiceCount == 2 && perf.ActiveSubmixVoiceCount == 1 && perf.ActiveResamplerCount == 1);
  CHECK(w1.passes > 50 && w1.starts == 1 && w1.ends == 0 && ew.passes > 50);
  printf("xaudio2: passes took %llu of %llu us (%u .. %u a pass)\n", (unsigned long long)perf.AudioCyclesSinceLastQuery, (unsigned long long)perf.TotalCyclesSinceLastQuery,
         perf.MinimumCyclesPerQuantum, perf.MaximumCyclesPerQuantum);
  printf("xaudio2: %ld engine passes in %lu ms, %u glitches, latency %u samples\n", ew.passes, GetTickCount() - began, perf.GlitchesSinceEngineStarted, perf.CurrentLatencyInSamples);
  IXAudio2_UnregisterForCallbacks(xa, &ew.iface);
  if (tone) IXAudio2SourceVoice_DestroyVoice(tone);
  if (tone2) IXAudio2SourceVoice_DestroyVoice(tone2);
  if (submix) IXAudio2SubmixVoice_DestroyVoice(submix);
  IXAudio2MasteringVoice_DestroyVoice(master);
  CHECK(IXAudio2_Release(xa) == 0);
  printf("xaudio2test: %d checks, %d failed\n", checks, failed);
  return failed ? 1 : 0;
}
