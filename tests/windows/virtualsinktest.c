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

/* A PipeWire virtual sink (a null sink: the emulator makes "emu-null" with TCOS_EMU_NULL_SINK, as pw-cli create-node adapter would) from a Windows
 * program through WASAPI (mmdevapi.dll): it is a render endpoint of its own next to the speakers, found by its name; a client of it plays 600 Hz into
 * it, a loopback client of the same endpoint records what plays there (the 600 Hz, and not the emulator's native 300 Hz, which plays on the
 * speakers); its IAudioEndpointVolume is the sink's own volume, apart from the speakers'. Then winmm (the sink is wave output device 1, named
 * after it: 400 Hz queued on it) and DirectSound (DirectSoundEnumerate lists it: 500 Hz from a looping buffer of a device made for its GUID), each
 * recorded back by the loopback client. Run by tests/pipewire_emulated.sh (nullsink). 24 kHz mono
 * 16-bit both ways (the emulator is slow; the kernel converts). Prints "virtualsinktest: N checks, M failed". */
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

#define RATE 24000
#define TONE 600.0
#define LEVEL 6000

static double tcos(double x) {
  while (x > 3.14159265358979323846) x -= 2 * 3.14159265358979323846;
  while (x < -3.14159265358979323846) x += 2 * 3.14159265358979323846;
  double term = 1, sum = 1;
  for (int n = 1; n < 14; ++n) {
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
static double amplitude(const short *samples, int n, double frequency) {
  const double coefficient = 2.0 * tcos(2.0 * 3.14159265358979323846 * frequency / RATE);
  double s1 = 0, s2 = 0;
  for (int i = 0; i < n; ++i) {
    const double s0 = samples[i] + coefficient * s1 - s2;
    s2 = s1;
    s1 = s0;
  }
  return 2.0 * tsqrt(s1 * s1 + s2 * s2 - coefficient * s1 * s2) / n;
}

static short g_recorded[RATE];      /* (a second at most) */

/* The render endpoint whose friendly name is `name` (NULL when there is none). */
static IMMDevice *find_endpoint(IMMDeviceEnumerator *en, const WCHAR *name, UINT *render_count) {
  IMMDeviceCollection *all = NULL;
  IMMDevice *found = NULL;
  *render_count = 0;
  if (IMMDeviceEnumerator_EnumAudioEndpoints(en, eRender, DEVICE_STATE_ACTIVE, &all) != S_OK || !all) return NULL;
  IMMDeviceCollection_GetCount(all, render_count);
  for (UINT i = 0; i < *render_count; ++i) {
    IMMDevice *d = NULL;
    IPropertyStore *props = NULL;
    if (IMMDeviceCollection_Item(all, i, &d) != S_OK || !d) continue;
    if (IMMDevice_OpenPropertyStore(d, STGM_READ, &props) == S_OK && props) {
      PROPVARIANT v;
      PropVariantInit(&v);
      if (IPropertyStore_GetValue(props, &PKEY_Device_FriendlyName, &v) == S_OK && v.vt == VT_LPWSTR && wcscmp(v.pwszVal, name) == 0) found = d;
      PropVariantClear(&v);
      IPropertyStore_Release(props);
    }
    if (found != d) IMMDevice_Release(d);
  }
  IMMDeviceCollection_Release(all);
  return found;
}

/* What plays in the sink for `frames` frames (after skipping the first `skip`), by a loopback client of its endpoint. */
static int record_sink(IMMDevice *sink, short *out, int frames, int skip) {
  IAudioClient *rec = NULL;
  IAudioCaptureClient *cap = NULL;
  WAVEFORMATEX f = {WAVE_FORMAT_PCM, 1, RATE, RATE * 2, 2, 16, 0};
  if (IMMDevice_Activate(sink, &IID_IAudioClient, CLSCTX_ALL, NULL, (void **)&rec) != S_OK || !rec) return 0;
  int got = 0, seen = 0, rounds = 0;
  if (IAudioClient_Initialize(rec, AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK, 2000000, 0, &f, NULL) == S_OK &&
      IAudioClient_GetService(rec, &IID_IAudioCaptureClient, (void **)&cap) == S_OK && cap && IAudioClient_Start(rec) == S_OK) {
    while (got < frames && rounds++ < 3000) {
      UINT32 packet = 0;
      while (got < frames && IAudioCaptureClient_GetNextPacketSize(cap, &packet) == S_OK && packet > 0) {
        BYTE *data = NULL;
        UINT32 n = 0;
        DWORD flags = 0;
        if (IAudioCaptureClient_GetBuffer(cap, &data, &n, &flags, NULL, NULL) != S_OK) break;
        for (UINT32 i = 0; i < n; ++i, ++seen)
          if (seen >= skip && got < frames) out[got++] = (flags & AUDCLNT_BUFFERFLAGS_SILENT) ? 0 : ((const short *)data)[i];
        IAudioCaptureClient_ReleaseBuffer(cap, n);
      }
      Sleep(10);
    }
    IAudioClient_Stop(rec);
  }
  if (cap) IAudioCaptureClient_Release(cap);
  IAudioClient_Release(rec);
  return got;
}

/* (DirectSoundEnumerate's callback: the device whose description is the sink's) */
static GUID g_sink_guid;
static int g_sink_found, g_devices_listed;
static BOOL CALLBACK find_ds_device(LPGUID guid, LPCSTR description, LPCSTR module, LPVOID context) {
  (void)module; (void)context;
  ++g_devices_listed;
  if (guid && strcmp(description, "Emulated null sink") == 0) {
    g_sink_guid = *guid;
    g_sink_found = 1;
  }
  return TRUE;
}

static short g_wave[RATE * 2];

int main(void) {
  CHECK(SUCCEEDED(CoInitializeEx(NULL, COINIT_MULTITHREADED)));
  IMMDeviceEnumerator *en = NULL;
  CHECK(CoCreateInstance(&CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL, &IID_IMMDeviceEnumerator, (void **)&en) == S_OK && en);
  if (!en) goto done;
  UINT count = 0;
  IMMDevice *sink = find_endpoint(en, L"Emulated null sink", &count), *speakers = NULL;
  CHECK(count >= 2 && sink);      /* (the speakers and the virtual sink) */
  CHECK(IMMDeviceEnumerator_GetDefaultAudioEndpoint(en, eRender, eConsole, &speakers) == S_OK && speakers && speakers != sink);
  if (!sink || !speakers) goto done;
  /* its id: its own, and it finds it again */
  LPWSTR id = NULL, speakers_id = NULL;
  CHECK(IMMDevice_GetId(sink, &id) == S_OK && IMMDevice_GetId(speakers, &speakers_id) == S_OK && id && speakers_id && wcscmp(id, speakers_id) != 0);
  IMMDevice *again = NULL;
  CHECK(id && IMMDeviceEnumerator_GetDevice(en, id, &again) == S_OK && again == sink);
  IMMEndpoint *endpoint = NULL;
  EDataFlow flow = eCapture;
  CHECK(IMMDevice_QueryInterface(sink, &IID_IMMEndpoint, (void **)&endpoint) == S_OK && endpoint && IMMEndpoint_GetDataFlow(endpoint, &flow) == S_OK && flow == eRender);
  DWORD state = 0;
  CHECK(IMMDevice_GetState(sink, &state) == S_OK && state == DEVICE_STATE_ACTIVE);
  CoTaskMemFree(id);
  CoTaskMemFree(speakers_id);

  /* its volume is its own: set to half, the speakers' stays */
  IAudioEndpointVolume *sink_volume = NULL, *speakers_volume = NULL;
  CHECK(IMMDevice_Activate(sink, &IID_IAudioEndpointVolume, CLSCTX_ALL, NULL, (void **)&sink_volume) == S_OK && sink_volume);
  CHECK(IMMDevice_Activate(speakers, &IID_IAudioEndpointVolume, CLSCTX_ALL, NULL, (void **)&speakers_volume) == S_OK && speakers_volume);
  if (sink_volume && speakers_volume) {
    float before = 0, level = 0;
    CHECK(IAudioEndpointVolume_GetMasterVolumeLevelScalar(speakers_volume, &before) == S_OK);
    CHECK(IAudioEndpointVolume_SetMasterVolumeLevelScalar(sink_volume, 0.5f, NULL) == S_OK);
    CHECK(IAudioEndpointVolume_GetMasterVolumeLevelScalar(sink_volume, &level) == S_OK && level > 0.48f && level < 0.52f);
    CHECK(IAudioEndpointVolume_GetMasterVolumeLevelScalar(speakers_volume, &level) == S_OK && level > before - 0.01f && level < before + 0.01f);
    CHECK(IAudioEndpointVolume_SetMasterVolumeLevelScalar(sink_volume, 1.0f, NULL) == S_OK);
  }

  /* a player into it and a loopback recorder of it */
  IAudioClient *player = NULL, *recorder = NULL;
  CHECK(IMMDevice_Activate(sink, &IID_IAudioClient, CLSCTX_ALL, NULL, (void **)&player) == S_OK && player);
  CHECK(IMMDevice_Activate(sink, &IID_IAudioClient, CLSCTX_ALL, NULL, (void **)&recorder) == S_OK && recorder);
  if (!player || !recorder) goto done;
  WAVEFORMATEX f = {WAVE_FORMAT_PCM, 1, RATE, RATE * 2, 2, 16, 0};
  CHECK(IAudioClient_Initialize(recorder, AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK, 2000000, 0, &f, NULL) == S_OK);
  CHECK(IAudioClient_Initialize(player, AUDCLNT_SHAREMODE_SHARED, 0, 2000000, 0, &f, NULL) == S_OK);
  IAudioRenderClient *render = NULL;
  IAudioCaptureClient *capture = NULL;
  CHECK(IAudioClient_GetService(player, &IID_IAudioRenderClient, (void **)&render) == S_OK && render);
  CHECK(IAudioClient_GetService(recorder, &IID_IAudioCaptureClient, (void **)&capture) == S_OK && capture);
  if (!render || !capture) goto done;
  UINT32 size = 0;
  CHECK(IAudioClient_GetBufferSize(player, &size) == S_OK && size >= RATE / 10);
  CHECK(IAudioClient_Start(recorder) == S_OK);
  CHECK(IAudioClient_Start(player) == S_OK);
  /* a second of 600 Hz, kept topped up; what plays there recorded meanwhile (the first 0.3 s skipped: the streams starting) */
  int played = 0, got = 0, rounds = 0;
  const int skip = RATE * 3 / 10;
  while (got < RATE / 2 && rounds < 3000) {
    ++rounds;
    UINT32 padding = 0;
    if (played < RATE * 2 && IAudioClient_GetCurrentPadding(player, &padding) == S_OK && padding < size) {
      const UINT32 n = size - padding;
      BYTE *data = NULL;
      if (IAudioRenderClient_GetBuffer(render, n, &data) == S_OK && data) {
        short *s = (short *)data;
        for (UINT32 i = 0; i < n; ++i) s[i] = (short)(LEVEL * tcos(2.0 * 3.14159265358979323846 * TONE * (played + (int)i) / RATE));
        IAudioRenderClient_ReleaseBuffer(render, n, 0);
        played += (int)n;
      }
    }
    UINT32 packet = 0;
    while (got < RATE / 2 && IAudioCaptureClient_GetNextPacketSize(capture, &packet) == S_OK && packet > 0) {
      BYTE *data = NULL;
      UINT32 frames = 0;
      DWORD flags = 0;
      if (IAudioCaptureClient_GetBuffer(capture, &data, &frames, &flags, NULL, NULL) != S_OK) break;
      static int seen;
      const short *s = (const short *)data;
      for (UINT32 i = 0; i < frames; ++i, ++seen)
        if (seen >= skip && got < RATE / 2) g_recorded[got++] = (flags & AUDCLNT_BUFFERFLAGS_SILENT) ? 0 : s[i];
      IAudioCaptureClient_ReleaseBuffer(capture, frames);
    }
    Sleep(10);
  }
  const double tone = amplitude(g_recorded, got, TONE), native = amplitude(g_recorded, got, 300.0);
  printf("virtualsinktest: %d endpoints; played %d frames into the null sink, its loopback recorded %d: 600 Hz %.0f, the speakers' 300 Hz %.0f\n", count, played,
         got, tone, native);
  CHECK(got >= RATE / 2 && tone > 3000 && tone < 7000 && native < 200);
  IAudioClient_Stop(player);
  IAudioClient_Stop(recorder);
  IAudioRenderClient_Release(render);
  IAudioCaptureClient_Release(capture);
  IAudioClient_Release(player);
  IAudioClient_Release(recorder);

  /* ---- winmm: the sink is device 1 */
  {
    CHECK(waveOutGetNumDevs() >= 2);
    WAVEOUTCAPSA caps;
    CHECK(waveOutGetDevCapsA(1, &caps, sizeof caps) == MMSYSERR_NOERROR && strcmp(caps.szPname, "Emulated null sink") == 0);
    CHECK(waveOutGetDevCapsA(0, &caps, sizeof caps) == MMSYSERR_NOERROR && strcmp(caps.szPname, "Emulated null sink") != 0);
    WAVEFORMATEX f = {WAVE_FORMAT_PCM, 1, RATE, RATE * 2, 2, 16, 0};
    HWAVEOUT h = NULL;
    CHECK(waveOutOpen(&h, 1, &f, 0, 0, CALLBACK_NULL) == MMSYSERR_NOERROR && h);
    UINT id = 99;
    CHECK(h && waveOutGetID(h, &id) == MMSYSERR_NOERROR && id == 1);
    if (h) {
      for (int i = 0; i < RATE * 2; ++i) g_wave[i] = (short)(LEVEL * tcos(2.0 * 3.14159265358979323846 * 400.0 * i / RATE));
      WAVEHDR hdr = {(LPSTR)g_wave, sizeof g_wave, 0, 0, 0, 0, NULL, 0};
      CHECK(waveOutPrepareHeader(h, &hdr, sizeof hdr) == MMSYSERR_NOERROR && waveOutWrite(h, &hdr, sizeof hdr) == MMSYSERR_NOERROR);
      const int n = record_sink(sink, g_recorded, RATE / 2, RATE * 3 / 10);
      const double a400 = amplitude(g_recorded, n, 400.0);
      printf("virtualsinktest: winmm device 1 into the null sink: 400 Hz %.0f\n", a400);
      CHECK(n >= RATE / 2 && a400 > 3000 && a400 < 7000);
      waveOutReset(h);
      waveOutUnprepareHeader(h, &hdr, sizeof hdr);
      CHECK(waveOutClose(h) == MMSYSERR_NOERROR);
    }
  }

  /* ---- DirectSound: the sink is a device DirectSoundEnumerate lists */
  {
    CHECK(DirectSoundEnumerateA(find_ds_device, NULL) == DS_OK && g_devices_listed >= 3 && g_sink_found);
    IDirectSound8 *ds = NULL;
    CHECK(g_sink_found && DirectSoundCreate8(&g_sink_guid, &ds, NULL) == DS_OK && ds);
    GUID back;
    CHECK(GetDeviceID(&g_sink_guid, &back) == DS_OK && memcmp(&back, &g_sink_guid, sizeof back) == 0);
    if (ds) {
      CHECK(IDirectSound8_SetCooperativeLevel(ds, GetDesktopWindow(), DSSCL_PRIORITY) == DS_OK);
      WAVEFORMATEX f = {WAVE_FORMAT_PCM, 1, RATE, RATE * 2, 2, 16, 0};
      DSBUFFERDESC desc = {sizeof desc, DSBCAPS_GLOBALFOCUS, RATE, 0, &f, {0}};      /* (half a second, looping) */
      IDirectSoundBuffer *buffer = NULL;
      CHECK(IDirectSound8_CreateSoundBuffer(ds, &desc, &buffer, NULL) == DS_OK && buffer);
      if (buffer) {
        void *p1 = NULL, *p2 = NULL;
        DWORD n1 = 0, n2 = 0;
        if (IDirectSoundBuffer_Lock(buffer, 0, RATE, &p1, &n1, &p2, &n2, 0) == DS_OK) {
          for (DWORD i = 0; i < n1 / 2; ++i) ((short *)p1)[i] = (short)(LEVEL * tcos(2.0 * 3.14159265358979323846 * 500.0 * i / RATE));
          IDirectSoundBuffer_Unlock(buffer, p1, n1, p2, n2);
        }
        CHECK(IDirectSoundBuffer_Play(buffer, 0, 0, DSBPLAY_LOOPING) == DS_OK);
        const int n = record_sink(sink, g_recorded, RATE / 2, RATE * 3 / 10);
        const double a500 = amplitude(g_recorded, n, 500.0), a400 = amplitude(g_recorded, n, 400.0);
        printf("virtualsinktest: DirectSound into the null sink: 500 Hz %.0f (400 Hz %.0f)\n", a500, a400);
        CHECK(n >= RATE / 2 && a500 > 3000 && a500 < 7000 && a400 < 200);
        IDirectSoundBuffer_Stop(buffer);
        IDirectSoundBuffer_Release(buffer);
      }
      IDirectSound8_Release(ds);
    }
  }
done:
  printf("virtualsinktest: %d checks, %d failed\n", checks, failed);
  return failed ? 1 : 0;
}
