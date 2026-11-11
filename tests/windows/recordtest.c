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

/* Recording through the kernel's PipeWire graph from a Windows program: winmm's waveIn, DirectSound capture (and full duplex) and a WASAPI capture
 * client, one after the other, each checking what it heard. The CPU emulator's microphone (tests/EmulationRun.cpp, TCOS_EMU_AUDIO) hears 880 Hz on the
 * left at 8000 and 1320 Hz on the right at 4000; every API converts it to the format asked for (22.05 kHz stereo 16-bit, 16 kHz mono float, 48 kHz
 * stereo 16-bit). Also: device counts and caps, bad formats and handles, waveInStop/Reset returning headers, position notifications and cursors, packet
 * sizes and device positions, the capture endpoint's properties and volume (the microphone's master gain, apart from the speakers'), and WASAPI
 * loopback capture on the render endpoint (what plays: the emulator's native 300 Hz stream). Run by
 * tests/pipewire_emulated.sh next to tests/linux/pulsetest.c, which records the same microphone at the same time. Prints "recordtest: N checks, M failed". */
#define COBJMACROS
#include <windows.h>
#include <mmsystem.h>
#include <initguid.h>
#include <dsound.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <endpointvolume.h>
#include <functiondiscoverykeys_devpkey.h>
#include <stdio.h>
#include <string.h>

static int checks, failed;
#define CHECK(x) do { ++checks; if (!(x)) { ++failed; printf("FAILED line %d: %s\n", __LINE__, #x); } } while (0)

/* (a cosine of its own: the test is about audio, not the C runtime's maths) */
static double tcos(double x) {
  double term = 1, sum = 1;
  for (int n = 1; n < 12; ++n) {
    term *= -x * x / ((2.0 * n - 1.0) * (2.0 * n));
    sum += term;
  }
  return sum;
}
static double tsqrt(double x) {
  if (!(x > 0)) return 0;
  double r = x > 1 ? x : 1;
  for (int k = 0; k < 60; ++k) r = 0.5 * (r + x / r);
  return r;
}
/* The amplitude of `frequency` in `n` samples (every `step`-th of `samples`), by Goertzel. */
static double amplitude(const float *samples, int n, int step, double frequency, double rate) {
  const double coefficient = 2.0 * tcos(2.0 * 3.14159265358979323846 * frequency / rate);
  double s1 = 0, s2 = 0;
  for (int i = 0; i < n; ++i) {
    const double s0 = samples[i * step] + coefficient * s1 - s2;
    s2 = s1;
    s1 = s0;
  }
  return 2.0 * tsqrt(s1 * s1 + s2 * s2 - coefficient * s1 * s2) / n;
}
static int about(double value, double want, double slack) { return value > want - slack && value < want + slack; }
static float g_samples[48000 * 2];      /* what one test recorded, as floats of the 16-bit scale */

static void report_tones(const char *what, double l880, double l1320, double r880, double r1320) {
  printf("%s: left 880 Hz %.0f, 1320 Hz %.0f; right 880 Hz %.0f, 1320 Hz %.0f\n", what, l880, l1320, r880, r1320);
}

/* ---- the capture endpoint's volume (first: the Linux program records later, and must hear the microphone at full gain) ----------------------------- */

static void endpoint_volume(IMMDeviceEnumerator *en) {
  IMMDevice *mic = NULL, *speakers = NULL;
  CHECK(IMMDeviceEnumerator_GetDefaultAudioEndpoint(en, eCapture, eConsole, &mic) == S_OK && mic);
  CHECK(IMMDeviceEnumerator_GetDefaultAudioEndpoint(en, eRender, eConsole, &speakers) == S_OK && speakers);
  if (!mic || !speakers) return;
  IAudioEndpointVolume *mv = NULL, *sv = NULL;
  CHECK(IMMDevice_Activate(mic, &IID_IAudioEndpointVolume, CLSCTX_ALL, NULL, (void **)&mv) == S_OK && mv);
  CHECK(IMMDevice_Activate(speakers, &IID_IAudioEndpointVolume, CLSCTX_ALL, NULL, (void **)&sv) == S_OK && sv);
  if (!mv || !sv) return;
  CHECK(mv != sv);
  float before = -1, level = -1, speaker = -1;
  CHECK(IAudioEndpointVolume_GetMasterVolumeLevelScalar(sv, &before) == S_OK);
  CHECK(IAudioEndpointVolume_SetMasterVolumeLevelScalar(mv, 0.5f, NULL) == S_OK);
  CHECK(IAudioEndpointVolume_GetMasterVolumeLevelScalar(mv, &level) == S_OK && level > 0.47f && level < 0.53f);
  CHECK(IAudioEndpointVolume_GetMasterVolumeLevelScalar(sv, &speaker) == S_OK && speaker == before);      /* (the speakers' is its own) */
  BOOL mute = TRUE;
  CHECK(IAudioEndpointVolume_SetMute(mv, TRUE, NULL) == S_OK && IAudioEndpointVolume_GetMute(mv, &mute) == S_OK && mute);
  CHECK(IAudioEndpointVolume_GetMute(sv, &mute) == S_OK && !mute);
  CHECK(IAudioEndpointVolume_SetMute(mv, FALSE, NULL) == S_OK);
  CHECK(IAudioEndpointVolume_SetMasterVolumeLevelScalar(mv, 1.0f, NULL) == S_OK && IAudioEndpointVolume_GetMasterVolumeLevelScalar(mv, &level) == S_OK && level > 0.99f);
}

/* ---- waveIn ------------------------------------------------------------------------------------------------------------------------------------- */

static volatile LONG g_opened, g_data, g_closed;
static HANDLE g_wave_event;
static void CALLBACK wave_callback(HWAVEIN h, UINT message, DWORD_PTR instance, DWORD_PTR a, DWORD_PTR b) {
  (void)h; (void)instance; (void)a; (void)b;
  if (message == WIM_OPEN) InterlockedIncrement(&g_opened);
  if (message == WIM_DATA) {
    InterlockedIncrement(&g_data);
    SetEvent(g_wave_event);
  }
  if (message == WIM_CLOSE) InterlockedIncrement(&g_closed);
}

#define WAVE_RATE 22050
#define WAVE_FRAMES 2205      /* 100 ms a buffer */
static short g_wave_data[4][WAVE_FRAMES * 2];

static void wave_in(void) {
  CHECK(waveInGetNumDevs() == 1);
  WAVEINCAPSW caps;
  CHECK(waveInGetDevCapsW(0, &caps, sizeof caps) == MMSYSERR_NOERROR && wcsstr(caps.szPname, L"Microphone") && caps.wChannels == 2);
  WAVEINCAPSA caps_a;
  CHECK(waveInGetDevCapsA(WAVE_MAPPER, &caps_a, sizeof caps_a) == MMSYSERR_NOERROR);
  CHECK(waveInGetDevCapsA(5, &caps_a, sizeof caps_a) == MMSYSERR_BADDEVICEID);
  WAVEFORMATEX f = {WAVE_FORMAT_PCM, 2, WAVE_RATE, WAVE_RATE * 4, 4, 16, 0};
  CHECK(waveInOpen(NULL, WAVE_MAPPER, &f, 0, 0, WAVE_FORMAT_QUERY) == MMSYSERR_NOERROR);
  WAVEFORMATEX bad = f;
  bad.nBlockAlign = 3;
  HWAVEIN h = NULL;
  CHECK(waveInOpen(&h, WAVE_MAPPER, &bad, 0, 0, CALLBACK_NULL) == WAVERR_BADFORMAT && h == NULL);
  CHECK(waveInStart((HWAVEIN)(ULONG_PTR)0x1234) == MMSYSERR_INVALHANDLE);
  g_wave_event = CreateEventA(NULL, FALSE, FALSE, NULL);
  CHECK(waveInOpen(&h, WAVE_MAPPER, &f, (DWORD_PTR)wave_callback, 0, CALLBACK_FUNCTION) == MMSYSERR_NOERROR && h);
  if (!h) return;
  CHECK(g_opened == 1);
  WAVEHDR headers[4];
  memset(headers, 0, sizeof headers);
  for (int k = 0; k < 4; ++k) {
    headers[k].lpData = (LPSTR)g_wave_data[k];
    headers[k].dwBufferLength = sizeof g_wave_data[k];
    CHECK(waveInAddBuffer(h, &headers[k], sizeof headers[k]) == WAVERR_UNPREPARED);
    CHECK(waveInPrepareHeader(h, &headers[k], sizeof headers[k]) == MMSYSERR_NOERROR);
    CHECK(waveInAddBuffer(h, &headers[k], sizeof headers[k]) == MMSYSERR_NOERROR);
  }
  CHECK(waveInClose(h) == WAVERR_STILLPLAYING);      /* (headers are queued) */
  CHECK(waveInStart(h) == MMSYSERR_NOERROR);
  /* half a second: five full buffers, each added again when it comes back */
  int frames = 0, rounds = 0;
  while (frames < WAVE_RATE / 2 && rounds < 400) {
    WaitForSingleObject(g_wave_event, 100);
    ++rounds;
    for (int k = 0; k < 4 && frames < WAVE_RATE / 2; ++k) {
      if (!(headers[k].dwFlags & WHDR_DONE) || (headers[k].dwFlags & WHDR_INQUEUE)) continue;
      CHECK(headers[k].dwBytesRecorded == sizeof g_wave_data[k]);
      for (int i = 0; i < WAVE_FRAMES * 2 && frames * 2 + i < 48000 * 2; ++i) g_samples[frames * 2 + i] = g_wave_data[k][i];
      frames += WAVE_FRAMES;
      CHECK(waveInAddBuffer(h, &headers[k], sizeof headers[k]) == MMSYSERR_NOERROR);
    }
  }
  CHECK(frames >= WAVE_RATE / 2);
  MMTIME t;
  memset(&t, 0, sizeof t);
  t.wType = TIME_BYTES;
  CHECK(waveInGetPosition(h, &t, sizeof t) == MMSYSERR_NOERROR && t.wType == TIME_BYTES && t.u.cb >= (DWORD)frames * 4);
  t.wType = TIME_SAMPLES;
  CHECK(waveInGetPosition(h, &t, sizeof t) == MMSYSERR_NOERROR && t.u.sample >= (DWORD)frames);
  /* the last 0.2 s: 176 and 264 whole periods */
  const float *tail = g_samples + (frames - 4410) * 2;
  const double l880 = amplitude(tail, 4410, 2, 880, WAVE_RATE), l1320 = amplitude(tail, 4410, 2, 1320, WAVE_RATE);
  const double r880 = amplitude(tail + 1, 4410, 2, 880, WAVE_RATE), r1320 = amplitude(tail + 1, 4410, 2, 1320, WAVE_RATE);
  report_tones("waveIn", l880, l1320, r880, r1320);
  CHECK(about(l880, 8000, 800) && about(r1320, 4000, 400) && l1320 < 300 && r880 < 300);
  /* Stop: the header being filled comes back with what it holds; Reset: all of them, and the position starts again */
  const LONG before = g_data;
  Sleep(30);
  CHECK(waveInStop(h) == MMSYSERR_NOERROR);
  CHECK(g_data >= before);
  CHECK(waveInReset(h) == MMSYSERR_NOERROR);
  int queued = 0;
  for (int k = 0; k < 4; ++k) {
    if (headers[k].dwFlags & WHDR_INQUEUE) ++queued;
    CHECK(headers[k].dwFlags & WHDR_DONE);
  }
  CHECK(queued == 0);
  t.wType = TIME_BYTES;
  CHECK(waveInGetPosition(h, &t, sizeof t) == MMSYSERR_NOERROR && t.u.cb == 0);
  UINT id = 9;
  CHECK(waveInGetID(h, &id) == MMSYSERR_NOERROR && id == 0);
  for (int k = 0; k < 4; ++k) CHECK(waveInUnprepareHeader(h, &headers[k], sizeof headers[k]) == MMSYSERR_NOERROR);
  CHECK(waveInClose(h) == MMSYSERR_NOERROR && g_closed == 1);
  char text[128];
  CHECK(waveInGetErrorTextA(WAVERR_BADFORMAT, text, sizeof text) == MMSYSERR_NOERROR && text[0]);
  CloseHandle(g_wave_event);
}

/* ---- DirectSound capture ------------------------------------------------------------------------------------------------------------------------ */

static int g_enumerated;
static BOOL CALLBACK on_capture_device(LPGUID guid, LPCSTR description, LPCSTR module, LPVOID context) {
  (void)module; (void)context;
  if (g_enumerated == 0) CHECK(guid == NULL);
  else CHECK(guid != NULL && strstr(description, "Microphone") != NULL);
  ++g_enumerated;
  return TRUE;
}

#define DSC_RATE 16000
static void directsound_capture(void) {
  CHECK(DirectSoundCaptureEnumerateA(on_capture_device, NULL) == DS_OK && g_enumerated == 2);
  GUID id;
  CHECK(GetDeviceID(&DSDEVID_DefaultCapture, &id) == DS_OK && !IsEqualGUID(&id, &GUID_NULL));
  IDirectSoundCapture *dsc = NULL;
  CHECK(DirectSoundCaptureCreate8(&id, &dsc, NULL) == DS_OK && dsc);
  if (!dsc) return;
  DSCCAPS caps = {sizeof caps, 0, 0, 0};
  CHECK(IDirectSoundCapture_GetCaps(dsc, &caps) == DS_OK && caps.dwChannels == 2 && (caps.dwFormats & WAVE_FORMAT_4S16));
  /* one second of 32-bit float mono at 16 kHz: both sides at half (880 Hz at 4000, 1320 Hz at 2000) */
  WAVEFORMATEX f = {WAVE_FORMAT_IEEE_FLOAT, 1, DSC_RATE, DSC_RATE * 4, 4, 32, 0};
  DSCBUFFERDESC desc = {sizeof desc, 0, DSC_RATE * 4, 0, &f, 0, NULL};
  DSCBUFFERDESC with_fx = desc;
  with_fx.dwFlags = DSCBCAPS_CTRLFX;
  IDirectSoundCaptureBuffer *b = NULL;
  CHECK(IDirectSoundCapture_CreateCaptureBuffer(dsc, &with_fx, &b, NULL) == DSERR_INVALIDPARAM && b == NULL);
  CHECK(IDirectSoundCapture_CreateCaptureBuffer(dsc, &desc, &b, NULL) == DS_OK && b);
  if (!b) return;
  IDirectSoundCaptureBuffer8 *b8 = NULL;
  CHECK(IDirectSoundCaptureBuffer_QueryInterface(b, &IID_IDirectSoundCaptureBuffer8, (void **)&b8) == DS_OK && b8);
  DWORD status = 0;
  CHECK(IDirectSoundCaptureBuffer8_GetFXStatus(b8, 0, &status) == DSERR_CONTROLUNAVAIL);
  DSCBCAPS bcaps = {sizeof bcaps, 0, 0, 0};
  CHECK(IDirectSoundCaptureBuffer_GetCaps(b, &bcaps) == DS_OK && bcaps.dwBufferBytes == DSC_RATE * 4);
  WAVEFORMATEX got;
  DWORD written = 0;
  CHECK(IDirectSoundCaptureBuffer_GetFormat(b, &got, sizeof got, &written) == DS_OK && written == sizeof got && got.wFormatTag == WAVE_FORMAT_IEEE_FLOAT);
  IDirectSoundNotify *notify = NULL;
  CHECK(IDirectSoundCaptureBuffer_QueryInterface(b, &IID_IDirectSoundNotify, (void **)&notify) == DS_OK && notify);
  HANDLE half = CreateEventA(NULL, FALSE, FALSE, NULL), end = CreateEventA(NULL, FALSE, FALSE, NULL), stop = CreateEventA(NULL, FALSE, FALSE, NULL);
  DSBPOSITIONNOTIFY points[3] = {{DSC_RATE * 2 - 4, half}, {DSC_RATE * 4 - 4, end}, {DSBPN_OFFSETSTOP, stop}};
  if (notify) {
    DSBPOSITIONNOTIFY beyond = {DSC_RATE * 4, half};
    CHECK(IDirectSoundNotify_SetNotificationPositions(notify, 1, &beyond) == DSERR_INVALIDPARAM);
    CHECK(IDirectSoundNotify_SetNotificationPositions(notify, 3, points) == DS_OK);
    IDirectSoundNotify_Release(notify);
  }
  DWORD capture = 9, read = 9;
  CHECK(IDirectSoundCaptureBuffer_GetCurrentPosition(b, &capture, &read) == DS_OK && capture == 0 && read == 0);
  CHECK(IDirectSoundCaptureBuffer_Start(b, DSCBSTART_LOOPING) == DS_OK);
  CHECK(IDirectSoundCaptureBuffer_GetStatus(b, &status) == DS_OK && status == (DSCBSTATUS_CAPTURING | DSCBSTATUS_LOOPING));
  CHECK(WaitForSingleObject(half, 20000) == WAIT_OBJECT_0);
  CHECK(IDirectSoundCaptureBuffer_GetCurrentPosition(b, &capture, &read) == DS_OK && capture >= DSC_RATE * 2 - 4 && read == capture);
  CHECK(WaitForSingleObject(end, 20000) == WAIT_OBJECT_0);
  /* the first half again, recorded after the wrap: lock it (well behind the cursor) */
  CHECK(WaitForSingleObject(half, 20000) == WAIT_OBJECT_0);
  void *p1 = NULL, *p2 = NULL;
  DWORD n1 = 0, n2 = 0;
  CHECK(IDirectSoundCaptureBuffer_Lock(b, 0, DSC_RATE * 2, &p1, &n1, &p2, &n2, 0) == DS_OK && p1 && n1 == DSC_RATE * 2 && !p2 && n2 == 0);
  if (p1) {
    const float *mono = (const float *)p1;
    for (int i = 0; i < 4000; ++i) g_samples[i] = mono[i + 4000] * 32768.0f;      /* 0.25 s: 220 and 330 whole periods */
    const double m880 = amplitude(g_samples, 4000, 1, 880, DSC_RATE), m1320 = amplitude(g_samples, 4000, 1, 1320, DSC_RATE);
    printf("DirectSound capture: mono 880 Hz %.0f, 1320 Hz %.0f\n", m880, m1320);
    CHECK(about(m880, 4000, 500) && about(m1320, 2000, 300));
    CHECK(IDirectSoundCaptureBuffer_Unlock(b, p1, n1, p2, n2) == DS_OK);
  }
  /* across the end: two pieces */
  CHECK(IDirectSoundCaptureBuffer_Lock(b, DSC_RATE * 4 - 64, 128, &p1, &n1, &p2, &n2, 0) == DS_OK && n1 == 64 && n2 == 64 && p2);
  CHECK(IDirectSoundCaptureBuffer_Lock(b, DSC_RATE * 4, 4, &p1, &n1, &p2, &n2, 0) == DSERR_INVALIDPARAM);
  CHECK(IDirectSoundCaptureBuffer_Stop(b) == DS_OK);
  CHECK(WaitForSingleObject(stop, 2000) == WAIT_OBJECT_0);
  CHECK(IDirectSoundCaptureBuffer_GetStatus(b, &status) == DS_OK && status == 0);
  if (b8) IDirectSoundCaptureBuffer8_Release(b8);
  IDirectSoundCaptureBuffer_Release(b);
  IDirectSoundCapture_Release(dsc);
  CloseHandle(half);
  CloseHandle(end);
  CloseHandle(stop);

  /* through COM, initialized by the program */
  IDirectSoundCapture *com = NULL;
  CHECK(CoCreateInstance(&CLSID_DirectSoundCapture8, NULL, CLSCTX_INPROC_SERVER, &IID_IDirectSoundCapture8, (void **)&com) == S_OK && com);
  if (com) {
    CHECK(IDirectSoundCapture_GetCaps(com, &caps) == DSERR_UNINITIALIZED);
    CHECK(IDirectSoundCapture_Initialize(com, NULL) == DS_OK && IDirectSoundCapture_Initialize(com, NULL) == DSERR_ALREADYINITIALIZED);
    IDirectSoundCapture_Release(com);
  }
  /* full duplex: a capture buffer and a playback buffer at once */
  WAVEFORMATEX pf = {WAVE_FORMAT_PCM, 2, 44100, 44100 * 4, 4, 16, 0};
  DSBUFFERDESC rdesc = {sizeof rdesc, DSBCAPS_GLOBALFOCUS, 44100 * 4, 0, &pf, {0}};
  DSCBUFFERDESC cdesc = {sizeof cdesc, 0, 44100 * 4, 0, &pf, 0, NULL};
  IDirectSoundFullDuplex *duplex = NULL;
  IDirectSoundCaptureBuffer8 *cb = NULL;
  IDirectSoundBuffer8 *rb = NULL;
  CHECK(DirectSoundFullDuplexCreate(NULL, NULL, &cdesc, &rdesc, GetDesktopWindow(), DSSCL_NORMAL, &duplex, &cb, &rb, NULL) == DS_OK && duplex && cb && rb);
  if (cb) IDirectSoundCaptureBuffer8_Release(cb);
  if (rb) IDirectSoundBuffer8_Release(rb);
  if (duplex) IDirectSoundFullDuplex_Release(duplex);
}

/* ---- WASAPI capture ----------------------------------------------------------------------------------------------------------------------------- */

static void wasapi_capture(IMMDeviceEnumerator *en) {
  IMMDevice *dev = NULL, *speakers = NULL;
  CHECK(IMMDeviceEnumerator_GetDefaultAudioEndpoint(en, eCapture, eCommunications, &dev) == S_OK && dev);
  CHECK(IMMDeviceEnumerator_GetDefaultAudioEndpoint(en, eRender, eConsole, &speakers) == S_OK && speakers);
  if (!dev || !speakers) return;
  LPWSTR id = NULL, render_id = NULL;
  CHECK(IMMDevice_GetId(dev, &id) == S_OK && IMMDevice_GetId(speakers, &render_id) == S_OK && id && render_id && wcscmp(id, render_id) != 0);
  IMMDevice *again = NULL;
  CHECK(IMMDeviceEnumerator_GetDevice(en, id, &again) == S_OK && again == dev);
  CoTaskMemFree(id);
  CoTaskMemFree(render_id);
  IMMEndpoint *endpoint = NULL;
  EDataFlow flow = eRender;
  CHECK(IMMDevice_QueryInterface(dev, &IID_IMMEndpoint, (void **)&endpoint) == S_OK && endpoint && IMMEndpoint_GetDataFlow(endpoint, &flow) == S_OK && flow == eCapture);
  IPropertyStore *props = NULL;
  CHECK(IMMDevice_OpenPropertyStore(dev, STGM_READ, &props) == S_OK && props);
  if (props) {
    PROPVARIANT v;
    PropVariantInit(&v);
    CHECK(IPropertyStore_GetValue(props, &PKEY_Device_FriendlyName, &v) == S_OK && v.vt == VT_LPWSTR && wcsstr(v.pwszVal, L"Microphone"));
    PropVariantClear(&v);
    CHECK(IPropertyStore_GetValue(props, &PKEY_AudioEndpoint_FormFactor, &v) == S_OK && v.vt == VT_UI4 && v.ulVal == Microphone);
  }
  IAudioClient *client = NULL;
  CHECK(IMMDevice_Activate(dev, &IID_IAudioClient, CLSCTX_ALL, NULL, (void **)&client) == S_OK && client);
  if (!client) return;
  WAVEFORMATEX f = {WAVE_FORMAT_PCM, 2, 48000, 48000 * 4, 4, 16, 0};
  CHECK(IAudioClient_Initialize(client, AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, 1000000, 0, &f, NULL) == S_OK);
  HANDLE event = CreateEventA(NULL, FALSE, FALSE, NULL);
  CHECK(IAudioClient_SetEventHandle(client, event) == S_OK);
  IAudioRenderClient *render = NULL;
  CHECK(IAudioClient_GetService(client, &IID_IAudioRenderClient, (void **)&render) == AUDCLNT_E_WRONG_ENDPOINT_TYPE && render == NULL);
  IAudioCaptureClient *cap = NULL;
  CHECK(IAudioClient_GetService(client, &IID_IAudioCaptureClient, (void **)&cap) == S_OK && cap);
  if (!cap) return;
  UINT32 size = 0, packet = 9;
  CHECK(IAudioClient_GetBufferSize(client, &size) == S_OK && size >= 4800);
  BYTE *data = NULL;
  UINT32 frames = 0;
  DWORD flags = 0;
  UINT64 position = 0, qpc = 0;
  CHECK(IAudioCaptureClient_GetBuffer(cap, &data, &frames, &flags, &position, &qpc) == AUDCLNT_S_BUFFER_EMPTY && frames == 0);      /* (not started) */
  CHECK(IAudioCaptureClient_GetNextPacketSize(cap, &packet) == S_OK && packet == 0);
  CHECK(IAudioClient_Start(client) == S_OK);
  /* half a second in 10 ms packets, each at the device position where the last one ended */
  int got = 0, rounds = 0, packets = 0, continuous = 1, odd_sizes = 0, tried_wrong_size = 0;
  UINT64 next = 0;
  while (got < 24000 && rounds < 2000) {
    WaitForSingleObject(event, 200);
    ++rounds;
    while (got < 24000 && IAudioCaptureClient_GetNextPacketSize(cap, &packet) == S_OK && packet > 0) {
      if (IAudioCaptureClient_GetBuffer(cap, &data, &frames, &flags, &position, &qpc) != S_OK) break;
      if (!tried_wrong_size) {
        CHECK(IAudioCaptureClient_ReleaseBuffer(cap, 1) == AUDCLNT_E_INVALID_SIZE);      /* (a packet goes whole) */
        BYTE *d2 = NULL;
        UINT32 f2 = 0;
        DWORD fl2 = 0;
        CHECK(IAudioCaptureClient_GetBuffer(cap, &d2, &f2, &fl2, NULL, NULL) == AUDCLNT_E_OUT_OF_ORDER && f2 == 0);      /* (the packet is still out) */
        tried_wrong_size = 1;
      }
      if (frames != 480) ++odd_sizes;
      if (packets > 0 && position != next && !(flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY)) continuous = 0;
      next = position + frames;
      const short *s = (const short *)data;
      for (UINT32 i = 0; i < frames * 2 && got * 2 + (int)i < 48000 * 2; ++i) g_samples[got * 2 + i] = s[i];
      got += (int)frames;
      ++packets;
      CHECK(IAudioCaptureClient_ReleaseBuffer(cap, frames) == S_OK);
    }
  }
  if (got < 24000 || odd_sizes || !continuous) printf("WASAPI capture: %d frames in %d packets, %d not 480 frames, continuous %d, rounds %d\n", got, packets, odd_sizes, continuous, rounds);
  CHECK(got >= 24000 && odd_sizes == 0 && continuous);
  UINT32 padding = 0;
  CHECK(IAudioClient_GetCurrentPadding(client, &padding) == S_OK && padding <= size);
  IAudioClock *clock = NULL;
  UINT64 clock_position = 0, frequency = 0;
  CHECK(IAudioClient_GetService(client, &IID_IAudioClock, (void **)&clock) == S_OK && clock);
  if (clock) {
    CHECK(IAudioClock_GetFrequency(clock, &frequency) == S_OK && frequency == 48000 * 4);
    CHECK(IAudioClock_GetPosition(clock, &clock_position, NULL) == S_OK && clock_position >= (UINT64)got * 4);
    IAudioClock_Release(clock);
  }
  /* the last 0.25 s: 220 and 330 whole periods */
  const float *tail = g_samples + (got - 12000) * 2;
  const double l880 = amplitude(tail, 12000, 2, 880, 48000), l1320 = amplitude(tail, 12000, 2, 1320, 48000);
  const double r880 = amplitude(tail + 1, 12000, 2, 880, 48000), r1320 = amplitude(tail + 1, 12000, 2, 1320, 48000);
  report_tones("WASAPI capture", l880, l1320, r880, r1320);
  CHECK(about(l880, 8000, 800) && about(r1320, 4000, 400) && l1320 < 300 && r880 < 300);
  CHECK(IAudioClient_Stop(client) == S_OK && IAudioClient_Stop(client) == S_FALSE);
  CHECK(IAudioClient_Reset(client) == S_OK);
  CHECK(IAudioClient_GetCurrentPadding(client, &padding) == S_OK && padding == 0);
  IAudioCaptureClient_Release(cap);
  IAudioClient_Release(client);
  CloseHandle(event);
}

/* ---- WASAPI loopback: what plays (the emulator's native 300 Hz stream, 5000 on both sides, and pulsetest's 500 Hz) --------------------------------- */

static void wasapi_loopback(IMMDeviceEnumerator *en) {
  IMMDevice *speakers = NULL, *mic = NULL;
  CHECK(IMMDeviceEnumerator_GetDefaultAudioEndpoint(en, eRender, eConsole, &speakers) == S_OK && speakers);
  CHECK(IMMDeviceEnumerator_GetDefaultAudioEndpoint(en, eCapture, eConsole, &mic) == S_OK && mic);
  if (!speakers || !mic) return;
  IAudioClient *client = NULL, *wrong = NULL;
  WAVEFORMATEX *mix = NULL;
  CHECK(IMMDevice_Activate(mic, &IID_IAudioClient, CLSCTX_ALL, NULL, (void **)&wrong) == S_OK && wrong);
  CHECK(IMMDevice_Activate(speakers, &IID_IAudioClient, CLSCTX_ALL, NULL, (void **)&client) == S_OK && client);
  if (!client || !wrong) return;
  CHECK(IAudioClient_GetMixFormat(client, &mix) == S_OK && mix && mix->nChannels == 2 && mix->nSamplesPerSec == 48000 && mix->wBitsPerSample == 32);
  if (!mix) return;
  CHECK(IAudioClient_Initialize(wrong, AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK, 1000000, 0, mix, NULL) == AUDCLNT_E_WRONG_ENDPOINT_TYPE);
  IAudioClient_Release(wrong);
  CHECK(IAudioClient_Initialize(client, AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK, 2000000, 0, mix, NULL) == S_OK);
  IAudioCaptureClient *cap = NULL;
  IAudioRenderClient *render = NULL;
  CHECK(IAudioClient_GetService(client, &IID_IAudioRenderClient, (void **)&render) == AUDCLNT_E_WRONG_ENDPOINT_TYPE);      /* (a loopback client records) */
  CHECK(IAudioClient_GetService(client, &IID_IAudioCaptureClient, (void **)&cap) == S_OK && cap);
  if (!cap) return;
  CHECK(IAudioClient_Start(client) == S_OK);
  int got = 0, rounds = 0;
  while (got < 9600 && rounds < 1000) {      /* 0.2 s: 60 whole periods of 300 Hz */
    Sleep(10);
    ++rounds;
    UINT32 packet = 0;
    while (got < 9600 && IAudioCaptureClient_GetNextPacketSize(cap, &packet) == S_OK && packet > 0) {
      BYTE *data = NULL;
      UINT32 frames = 0;
      DWORD flags = 0;
      if (IAudioCaptureClient_GetBuffer(cap, &data, &frames, &flags, NULL, NULL) != S_OK) break;
      const float *f = (const float *)data;
      for (UINT32 i = 0; i < frames * 2 && got * 2 + (int)i < 9600 * 2; ++i) g_samples[got * 2 + i] = f[i] * 32768.0f;
      got += (int)frames;
      IAudioCaptureClient_ReleaseBuffer(cap, frames);
    }
  }
  CHECK(got >= 9600);
  const double l300 = amplitude(g_samples, 9600, 2, 300, 48000), r300 = amplitude(g_samples + 1, 9600, 2, 300, 48000);
  printf("WASAPI loopback: 300 Hz left %.0f, right %.0f\n", l300, r300);
  CHECK(about(l300, 5000, 600) && about(r300, 5000, 600));
  CHECK(IAudioClient_Stop(client) == S_OK);
  IAudioCaptureClient_Release(cap);
  IAudioClient_Release(client);
  CoTaskMemFree(mix);
}

int main(void) {
  CHECK(SUCCEEDED(CoInitializeEx(NULL, COINIT_MULTITHREADED)));
  IMMDeviceEnumerator *en = NULL;
  CHECK(CoCreateInstance(&CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL, &IID_IMMDeviceEnumerator, (void **)&en) == S_OK && en);
  if (en) endpoint_volume(en);
  if (en) wasapi_loopback(en);      /* (early: the native stream plays for the emulator's first 6 s) */
  wave_in();
  directsound_capture();
  if (en) wasapi_capture(en);
  printf("recordtest: %d checks, %d failed\n", checks, failed);
  return failed ? 1 : 0;
}
