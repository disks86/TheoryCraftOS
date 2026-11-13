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

/* A PipeWire virtual source from a Windows program (the emulator makes "emu-vsrc" with TCOS_EMU_NULL_SOURCE: module-remap-source of its microphone,
 * which says 880 Hz on the left and 1320 Hz on the right, with the sides swapped). Through WASAPI it is a capture endpoint of its own next to the
 * microphone, found by its name, whose client records 1320 Hz on the left and 880 Hz on the right (the microphone's own endpoint the other way
 * round); its IAudioEndpointVolume is its own. Through winmm it is wave input device 1, named after it; through DirectSound capture a device
 * DirectSoundCaptureEnumerate lists, recorded from a capture buffer of a device made for its GUID. Run by tests/pipewire_emulated.sh (nullsource).
 * 24 kHz stereo 16-bit. Prints "virtualsourcetest: N checks, M failed". */
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
#define FRAMES (RATE / 2)

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
/* The level of `frequency` on one side (0 left, 1 right) of interleaved stereo. */
static double amplitude(const short *frames, int n, int side, double frequency) {
  const double coefficient = 2.0 * tcos(2.0 * 3.14159265358979323846 * frequency / RATE);
  double s1 = 0, s2 = 0;
  for (int i = 0; i < n; ++i) {
    const double s0 = frames[2 * i + side] + coefficient * s1 - s2;
    s2 = s1;
    s1 = s0;
  }
  return 2.0 * tsqrt(s1 * s1 + s2 * s2 - coefficient * s1 * s2) / n;
}
/* The virtual source's sides: 1320 Hz on the left, 880 Hz on the right (the microphone's swapped). */
static int swapped(const short *frames, int n, const char *what) {
  const double l1320 = amplitude(frames, n, 0, 1320), l880 = amplitude(frames, n, 0, 880), r880 = amplitude(frames, n, 1, 880), r1320 = amplitude(frames, n, 1, 1320);
  printf("virtualsourcetest: %s: %d frames, left 1320 Hz %.0f 880 Hz %.0f, right 880 Hz %.0f 1320 Hz %.0f\n", what, n, l1320, l880, r880, r1320);
  return n >= FRAMES && l1320 > 2500 && r880 > 5000 && l880 < 500 && r1320 < 500;
}

static short g_recorded[FRAMES * 2];

/* The endpoint of `flow` whose friendly name is `name` (NULL when there is none); *count the endpoints of that flow. */
static IMMDevice *find_endpoint(IMMDeviceEnumerator *en, EDataFlow flow, const WCHAR *name, UINT *count) {
  IMMDeviceCollection *all = NULL;
  IMMDevice *found = NULL;
  *count = 0;
  if (IMMDeviceEnumerator_EnumAudioEndpoints(en, flow, DEVICE_STATE_ACTIVE, &all) != S_OK || !all) return NULL;
  IMMDeviceCollection_GetCount(all, count);
  for (UINT i = 0; i < *count; ++i) {
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

/* FRAMES frames recorded by a client of `device` (after skipping the first `skip`). */
static int record_endpoint(IMMDevice *device, int skip) {
  IAudioClient *rec = NULL;
  IAudioCaptureClient *cap = NULL;
  WAVEFORMATEX f = {WAVE_FORMAT_PCM, 2, RATE, RATE * 4, 4, 16, 0};
  if (IMMDevice_Activate(device, &IID_IAudioClient, CLSCTX_ALL, NULL, (void **)&rec) != S_OK || !rec) return 0;
  int got = 0, seen = 0, rounds = 0;
  if (IAudioClient_Initialize(rec, AUDCLNT_SHAREMODE_SHARED, 0, 2000000, 0, &f, NULL) == S_OK &&
      IAudioClient_GetService(rec, &IID_IAudioCaptureClient, (void **)&cap) == S_OK && cap && IAudioClient_Start(rec) == S_OK) {
    while (got < FRAMES && rounds++ < 3000) {
      UINT32 packet = 0;
      while (got < FRAMES && IAudioCaptureClient_GetNextPacketSize(cap, &packet) == S_OK && packet > 0) {
        BYTE *data = NULL;
        UINT32 n = 0;
        DWORD flags = 0;
        if (IAudioCaptureClient_GetBuffer(cap, &data, &n, &flags, NULL, NULL) != S_OK) break;
        for (UINT32 i = 0; i < n; ++i, ++seen)
          if (seen >= skip && got < FRAMES) {
            g_recorded[2 * got] = (flags & AUDCLNT_BUFFERFLAGS_SILENT) ? 0 : ((const short *)data)[2 * i];
            g_recorded[2 * got + 1] = (flags & AUDCLNT_BUFFERFLAGS_SILENT) ? 0 : ((const short *)data)[2 * i + 1];
            ++got;
          }
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

/* (DirectSoundCaptureEnumerate's callback: the device whose description is the virtual source's) */
static GUID g_source_guid;
static int g_source_found, g_capture_listed;
static BOOL CALLBACK find_capture_device(LPGUID guid, LPCSTR description, LPCSTR module, LPVOID context) {
  (void)module; (void)context;
  ++g_capture_listed;
  if (guid && strcmp(description, "Emulated virtual source") == 0) {
    g_source_guid = *guid;
    g_source_found = 1;
  }
  return TRUE;
}

static short g_wave[RATE * 2];      /* (a second of stereo) */

int main(void) {
  CHECK(SUCCEEDED(CoInitializeEx(NULL, COINIT_MULTITHREADED)));
  IMMDeviceEnumerator *en = NULL;
  CHECK(CoCreateInstance(&CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL, &IID_IMMDeviceEnumerator, (void **)&en) == S_OK && en);
  if (!en) goto done;
  UINT count = 0, render_count = 0;
  IMMDevice *source = find_endpoint(en, eCapture, L"Emulated virtual source", &count), *microphone = NULL;
  CHECK(count >= 2 && source);      /* (the microphone and the virtual source) */
  IMMDevice *as_render = find_endpoint(en, eRender, L"Emulated virtual source", &render_count);
  CHECK(!as_render);      /* (it is not a render endpoint) */
  CHECK(IMMDeviceEnumerator_GetDefaultAudioEndpoint(en, eCapture, eConsole, &microphone) == S_OK && microphone && microphone != source);
  if (!source || !microphone) goto done;
  LPWSTR id = NULL, mic_id = NULL;
  CHECK(IMMDevice_GetId(source, &id) == S_OK && IMMDevice_GetId(microphone, &mic_id) == S_OK && id && mic_id && wcscmp(id, mic_id) != 0);
  CHECK(id && wcsncmp(id, L"{0.0.1.", 7) == 0);      /* (a capture endpoint's id) */
  IMMDevice *again = NULL;
  CHECK(id && IMMDeviceEnumerator_GetDevice(en, id, &again) == S_OK && again == source);
  IMMEndpoint *endpoint = NULL;
  EDataFlow flow = eRender;
  CHECK(IMMDevice_QueryInterface(source, &IID_IMMEndpoint, (void **)&endpoint) == S_OK && endpoint && IMMEndpoint_GetDataFlow(endpoint, &flow) == S_OK && flow == eCapture);
  CoTaskMemFree(id);
  CoTaskMemFree(mic_id);

  /* ---- WASAPI: its client records the swapped sides; the microphone's the other way round */
  {
    int n = record_endpoint(source, RATE * 3 / 10);
    CHECK(swapped(g_recorded, n, "WASAPI"));
    n = record_endpoint(microphone, RATE * 3 / 10);
    const double l880 = amplitude(g_recorded, n, 0, 880), r1320 = amplitude(g_recorded, n, 1, 1320);
    printf("virtualsourcetest: the microphone: left 880 Hz %.0f, right 1320 Hz %.0f\n", l880, r1320);
    CHECK(n >= FRAMES && l880 > 5000 && r1320 > 2500);
    /* its volume is its own: a loopback client of it is refused (it is a capture endpoint) */
    IAudioEndpointVolume *volume = NULL;
    CHECK(IMMDevice_Activate(source, &IID_IAudioEndpointVolume, CLSCTX_ALL, NULL, (void **)&volume) == S_OK && volume);
    if (volume) {
      float level = 0;
      CHECK(IAudioEndpointVolume_SetMasterVolumeLevelScalar(volume, 0.5f, NULL) == S_OK);
      CHECK(IAudioEndpointVolume_GetMasterVolumeLevelScalar(volume, &level) == S_OK && level > 0.48f && level < 0.52f);
      n = record_endpoint(source, RATE * 3 / 10);
      const double half = amplitude(g_recorded, n, 1, 880);
      printf("virtualsourcetest: at half its volume: right 880 Hz %.0f\n", half);
      CHECK(n >= FRAMES && half > 200 && half < 5000);      /* (quieter: the source's volume scales what records it) */
      CHECK(IAudioEndpointVolume_SetMasterVolumeLevelScalar(volume, 1.0f, NULL) == S_OK);
    }
    IAudioClient *loop = NULL;
    WAVEFORMATEX f = {WAVE_FORMAT_PCM, 2, RATE, RATE * 4, 4, 16, 0};
    CHECK(IMMDevice_Activate(source, &IID_IAudioClient, CLSCTX_ALL, NULL, (void **)&loop) == S_OK && loop &&
          IAudioClient_Initialize(loop, AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK, 2000000, 0, &f, NULL) == AUDCLNT_E_WRONG_ENDPOINT_TYPE);
    if (loop) IAudioClient_Release(loop);
  }

  /* ---- winmm: the virtual source is wave input device 1 */
  {
    CHECK(waveInGetNumDevs() >= 2);
    WAVEINCAPSA caps;
    CHECK(waveInGetDevCapsA(1, &caps, sizeof caps) == MMSYSERR_NOERROR && strcmp(caps.szPname, "Emulated virtual source") == 0);
    CHECK(waveInGetDevCapsA(0, &caps, sizeof caps) == MMSYSERR_NOERROR && strcmp(caps.szPname, "Emulated virtual source") != 0);
    WAVEFORMATEX f = {WAVE_FORMAT_PCM, 2, RATE, RATE * 4, 4, 16, 0};
    HWAVEIN h = NULL;
    CHECK(waveInOpen(&h, 1, &f, 0, 0, CALLBACK_NULL) == MMSYSERR_NOERROR && h);
    UINT device = 99;
    CHECK(h && waveInGetID(h, &device) == MMSYSERR_NOERROR && device == 1);
    if (h) {
      WAVEHDR hdr = {(LPSTR)g_wave, sizeof g_wave, 0, 0, 0, 0, NULL, 0};      /* (a second of stereo) */
      CHECK(waveInPrepareHeader(h, &hdr, sizeof hdr) == MMSYSERR_NOERROR && waveInAddBuffer(h, &hdr, sizeof hdr) == MMSYSERR_NOERROR);
      CHECK(waveInStart(h) == MMSYSERR_NOERROR);
      for (int k = 0; k < 600 && !(hdr.dwFlags & WHDR_DONE); ++k) Sleep(10);
      CHECK(hdr.dwFlags & WHDR_DONE);
      /* (the first 0.1 s skipped: the stream starting) */
      const int frames = (int)(hdr.dwBytesRecorded / 4);
      CHECK(frames == RATE);      /* (a second, the header whole) */
      if (frames == RATE) {
        memcpy(g_recorded, g_wave + 2 * (RATE / 10), FRAMES * 4);
        CHECK(swapped(g_recorded, FRAMES, "winmm device 1"));
      }
      waveInReset(h);
      waveInUnprepareHeader(h, &hdr, sizeof hdr);
      CHECK(waveInClose(h) == MMSYSERR_NOERROR);
    }
  }

  /* ---- DirectSound capture: a device DirectSoundCaptureEnumerate lists */
  {
    CHECK(DirectSoundCaptureEnumerateA(find_capture_device, NULL) == DS_OK && g_capture_listed >= 3 && g_source_found);
    IDirectSoundCapture8 *dsc = NULL;
    CHECK(g_source_found && DirectSoundCaptureCreate8(&g_source_guid, &dsc, NULL) == DS_OK && dsc);
    GUID back;
    CHECK(GetDeviceID(&g_source_guid, &back) == DS_OK && memcmp(&back, &g_source_guid, sizeof back) == 0);
    if (dsc) {
      WAVEFORMATEX f = {WAVE_FORMAT_PCM, 2, RATE, RATE * 4, 4, 16, 0};
      DSCBUFFERDESC desc = {sizeof desc, 0, RATE * 4, 0, &f, 0, NULL};      /* (a second, not looping) */
      IDirectSoundCaptureBuffer *buffer = NULL;
      CHECK(IDirectSoundCapture_CreateCaptureBuffer(dsc, &desc, &buffer, NULL) == DS_OK && buffer);
      if (buffer) {
        CHECK(IDirectSoundCaptureBuffer_Start(buffer, 0) == DS_OK);
        DWORD status = DSCBSTATUS_CAPTURING;
        for (int k = 0; k < 600 && (status & DSCBSTATUS_CAPTURING); ++k) {
          Sleep(10);
          IDirectSoundCaptureBuffer_GetStatus(buffer, &status);
        }
        CHECK(!(status & DSCBSTATUS_CAPTURING));
        void *p1 = NULL, *p2 = NULL;
        DWORD n1 = 0, n2 = 0;
        if (IDirectSoundCaptureBuffer_Lock(buffer, 0, RATE * 4, &p1, &n1, &p2, &n2, 0) == DS_OK) {
          /* (the second half of the second: well after the start) */
          memcpy(g_recorded, (const BYTE *)p1 + FRAMES * 4, FRAMES * 4);
          IDirectSoundCaptureBuffer_Unlock(buffer, p1, n1, p2, n2);
          CHECK(n1 == RATE * 4 && swapped(g_recorded, FRAMES, "DirectSound capture"));
        } else {
          CHECK(!"the capture buffer could not be locked");
        }
        IDirectSoundCaptureBuffer_Release(buffer);
      }
      IDirectSoundCapture_Release(dsc);
    }
  }
done:
  printf("virtualsourcetest: %d checks, %d failed\n", checks, failed);
  return failed ? 1 : 0;
}
