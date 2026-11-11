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

/* dsound.dll and WASAPI (mmdevapi.dll) over the kernel's PipeWire graph, at the same time: a DirectSound looping buffer plays 600 Hz (mono, 24 kHz,
 * mixed by dsound's own mixer) while a WASAPI event-driven client plays 700 Hz (the engine's float stereo). Also: enumeration, device ids, caps, the
 * primary buffer and listener, Lock across the end, position notifications, cursors, volume/pan/frequency, duplicates sharing memory, a one-shot
 * buffer's stop notification, deferred 3D settings; the endpoint enumerator, properties, formats, padding, the clock, Stop/Reset, stream, session
 * and endpoint volume with its callback, the session manager and ActivateAudioInterfaceAsync. Run by tests/pipewire_emulated.sh on the CPU emulator
 * with a Linux program and a native stream playing too. Prints "dsoundtest: N checks, M failed". */
#define COBJMACROS
#include <windows.h>
#include <mmsystem.h>
#include <initguid.h>
#include <dsound.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <audiopolicy.h>
#include <endpointvolume.h>
#include <functiondiscoverykeys_devpkey.h>
#include <stdio.h>
#include <string.h>

/* (a sine of its own: the test is about audio, not the C runtime's maths) */
static double tsin(double x) {
  const double pi = 3.14159265358979323846;
  while (x > pi) x -= 2 * pi;
  while (x < -pi) x += 2 * pi;
  double term = x, sum = x;
  for (int n = 1; n < 10; ++n) {
    term *= -x * x / ((2.0 * n) * (2.0 * n + 1.0));
    sum += term;
  }
  return sum;
}

static int checks, failed;
#define CHECK(x) do { ++checks; if (!(x)) { ++failed; printf("FAILED line %d: %s\n", __LINE__, #x); } } while (0)

static int enumerated;
static BOOL CALLBACK on_device(LPGUID guid, LPCSTR description, LPCSTR module, LPVOID context) {
  (void)module;
  (void)context;
  if (enumerated == 0) CHECK(guid == NULL);
  else CHECK(guid != NULL && description && description[0]);
  ++enumerated;
  return TRUE;
}

/* ---- DirectSound ------------------------------------------------------------------------------------------------------------------------------- */

#define DS_RATE 24000
static IDirectSound8 *g_ds;
static IDirectSoundBuffer *g_tone;      /* the 600 Hz loop, playing while WASAPI plays */

static void directsound_start(void) {
  CHECK(DirectSoundEnumerateA(on_device, NULL) == DS_OK && enumerated == 2);
  GUID id;
  CHECK(GetDeviceID(&DSDEVID_DefaultPlayback, &id) == DS_OK && !IsEqualGUID(&id, &GUID_NULL));
  CHECK(GetDeviceID(&DSDEVID_DefaultCapture, &id) == DSERR_NODRIVER);
  IDirectSoundCapture *capture = (IDirectSoundCapture *)1;
  CHECK(DirectSoundCaptureCreate(NULL, &capture, NULL) == DSERR_NODRIVER && capture == NULL);

  CHECK(DirectSoundCreate8(NULL, &g_ds, NULL) == DS_OK && g_ds);
  if (!g_ds) return;
  DSCAPS caps = {sizeof caps};
  CHECK(IDirectSound8_GetCaps(g_ds, &caps) == DS_OK && (caps.dwFlags & DSCAPS_SECONDARY16BIT) && caps.dwMaxSecondarySampleRate >= 96000);
  CHECK(IDirectSound8_SetCooperativeLevel(g_ds, NULL, DSSCL_PRIORITY) == DS_OK);

  /* the primary buffer: its format, and the listener */
  DSBUFFERDESC pd = {sizeof pd, DSBCAPS_PRIMARYBUFFER | DSBCAPS_CTRLVOLUME | DSBCAPS_CTRL3D, 0, 0, NULL, {0}};
  IDirectSoundBuffer *primary = NULL;
  CHECK(IDirectSound8_CreateSoundBuffer(g_ds, &pd, &primary, NULL) == DS_OK && primary);
  WAVEFORMATEX pf = {WAVE_FORMAT_PCM, 2, 48000, 48000 * 4, 4, 16, 0}, got;
  CHECK(IDirectSoundBuffer_SetFormat(primary, &pf) == DS_OK);
  DWORD n = 0;
  CHECK(IDirectSoundBuffer_GetFormat(primary, &got, sizeof got, &n) == DS_OK && got.nSamplesPerSec == 48000 && got.nChannels == 2);
  IDirectSoundBuffer8 *b8 = NULL;
  CHECK(IDirectSoundBuffer_QueryInterface(primary, &IID_IDirectSoundBuffer8, (void **)&b8) == E_NOINTERFACE);
  IDirectSound3DListener *listener = NULL;
  CHECK(IDirectSoundBuffer_QueryInterface(primary, &IID_IDirectSound3DListener, (void **)&listener) == S_OK && listener);
  void *p1, *p2;
  DWORD n1, n2;
  CHECK(IDirectSoundBuffer_Lock(primary, 0, 16, &p1, &n1, &p2, &n2, 0) == DSERR_PRIOLEVELNEEDED);

  /* the 600 Hz loop: one second, 600 whole cycles */
  WAVEFORMATEX mono = {WAVE_FORMAT_PCM, 1, DS_RATE, DS_RATE * 2, 2, 16, 0};
  DSBUFFERDESC d = {sizeof d, DSBCAPS_CTRLVOLUME | DSBCAPS_CTRLPAN | DSBCAPS_CTRLFREQUENCY | DSBCAPS_CTRLPOSITIONNOTIFY | DSBCAPS_GETCURRENTPOSITION2,
                    DS_RATE * 2, 0, &mono, {0}};
  CHECK(IDirectSound8_CreateSoundBuffer(g_ds, &d, &g_tone, NULL) == DS_OK && g_tone);
  if (!g_tone) return;
  DSBCAPS bc = {sizeof bc};
  CHECK(IDirectSoundBuffer_GetCaps(g_tone, &bc) == DS_OK && bc.dwBufferBytes == DS_RATE * 2);
  /* a lock across the end comes in two pieces */
  CHECK(IDirectSoundBuffer_Lock(g_tone, DS_RATE * 2 - 100, 200, &p1, &n1, &p2, &n2, 0) == DS_OK && n1 == 100 && n2 == 100 && p2 != NULL);
  CHECK(IDirectSoundBuffer_Unlock(g_tone, p1, n1, p2, n2) == DS_OK);
  CHECK(IDirectSoundBuffer_Lock(g_tone, DS_RATE * 2, 2, &p1, &n1, &p2, &n2, 0) == DSERR_INVALIDPARAM);
  CHECK(IDirectSoundBuffer_Lock(g_tone, 0, 0, &p1, &n1, &p2, &n2, DSBLOCK_ENTIREBUFFER) == DS_OK && n1 == DS_RATE * 2 && n2 == 0);
  short *s = (short *)p1;
  for (int i = 0; i < DS_RATE; ++i) s[i] = (short)(8000.0 * tsin(2.0 * 3.14159265358979 * (double)((600 * i) % DS_RATE) / DS_RATE));      /* (the angle kept small) */
  CHECK(IDirectSoundBuffer_Unlock(g_tone, p1, n1, NULL, 0) == DS_OK);
  /* notifications at the start and the middle */
  IDirectSoundNotify *notify = NULL;
  CHECK(IDirectSoundBuffer_QueryInterface(g_tone, &IID_IDirectSoundNotify, (void **)&notify) == S_OK && notify);
  HANDLE events[2] = {CreateEventA(NULL, FALSE, FALSE, NULL), CreateEventA(NULL, FALSE, FALSE, NULL)};
  DSBPOSITIONNOTIFY at[2] = {{0, events[0]}, {DS_RATE, events[1]}};
  DSBPOSITIONNOTIFY bad = {DS_RATE * 4, events[0]};
  if (notify) {
    CHECK(IDirectSoundNotify_SetNotificationPositions(notify, 1, &bad) == DSERR_INVALIDPARAM);
    CHECK(IDirectSoundNotify_SetNotificationPositions(notify, 2, at) == DS_OK);
  }
  /* controls */
  LONG v = 1;
  CHECK(IDirectSoundBuffer_SetVolume(g_tone, -600) == DS_OK && IDirectSoundBuffer_GetVolume(g_tone, &v) == DS_OK && v == -600);
  CHECK(IDirectSoundBuffer_SetVolume(g_tone, 1) == DSERR_INVALIDPARAM && IDirectSoundBuffer_SetPan(g_tone, 10001) == DSERR_INVALIDPARAM);
  CHECK(IDirectSoundBuffer_SetVolume(g_tone, 0) == DS_OK && IDirectSoundBuffer_SetPan(g_tone, 0) == DS_OK);
  DWORD f = 0;
  CHECK(IDirectSoundBuffer_SetFrequency(g_tone, 12000) == DS_OK && IDirectSoundBuffer_GetFrequency(g_tone, &f) == DS_OK && f == 12000);
  CHECK(IDirectSoundBuffer_SetFrequency(g_tone, DSBFREQUENCY_ORIGINAL) == DS_OK && IDirectSoundBuffer_GetFrequency(g_tone, &f) == DS_OK && f == DS_RATE);
  CHECK(IDirectSoundBuffer_SetFrequency(g_tone, 50) == DSERR_INVALIDPARAM);

  CHECK(IDirectSoundBuffer_Play(g_tone, 0, 0, DSBPLAY_LOOPING) == DS_OK);
  DWORD status = 0;
  CHECK(IDirectSoundBuffer_GetStatus(g_tone, &status) == DS_OK && (status & DSBSTATUS_PLAYING) && (status & DSBSTATUS_LOOPING));
  if (notify) {
    CHECK(IDirectSoundNotify_SetNotificationPositions(notify, 2, at) == DSERR_INVALIDCALL);      /* (not while it plays) */
    IDirectSoundNotify_Release(notify);
  }
  /* both notifications within the second and a half it takes to come round */
  CHECK(WaitForSingleObject(events[1], 3000) == WAIT_OBJECT_0);
  CHECK(WaitForSingleObject(events[0], 3000) == WAIT_OBJECT_0);
  /* the play cursor moves, the write cursor is ahead of it */
  DWORD play1 = 0, write1 = 0, play2 = 0, write2 = 0;
  CHECK(IDirectSoundBuffer_GetCurrentPosition(g_tone, &play1, &write1) == DS_OK);
  const DWORD t0 = GetTickCount();
  Sleep(200);
  CHECK(IDirectSoundBuffer_GetCurrentPosition(g_tone, &play2, &write2) == DS_OK);
  printf("dsound: slept %lu ms\n", (unsigned long)(GetTickCount() - t0));
  const DWORD moved = (play2 + DS_RATE * 2 - play1) % (DS_RATE * 2), lead = (write2 + DS_RATE * 2 - play2) % (DS_RATE * 2);
  printf("dsound: play cursor moved %lu bytes in 200 ms, write cursor %lu ahead\n", (unsigned long)moved, (unsigned long)lead);
  CHECK(moved > 2000 && moved < 30000);
  CHECK(lead < DS_RATE);

  /* a duplicate shares the samples */
  IDirectSoundBuffer *dup = NULL;
  CHECK(IDirectSound8_DuplicateSoundBuffer(g_ds, g_tone, &dup) == DS_OK && dup);
  if (dup) {
    CHECK(IDirectSoundBuffer_Lock(dup, 0, 4, &p1, &n1, &p2, &n2, 0) == DS_OK && ((short *)p1)[1] == s[1]);
    IDirectSoundBuffer_Unlock(dup, p1, n1, p2, n2);
    CHECK(IDirectSoundBuffer_GetStatus(dup, &status) == DS_OK && status == 0);
    IDirectSoundBuffer_Release(dup);
  }

  /* a one-shot (8-bit, 11 kHz, a quarter of a second) tells when it stops */
  WAVEFORMATEX u8 = {WAVE_FORMAT_PCM, 1, 11025, 11025, 1, 8, 0};
  DSBUFFERDESC od = {sizeof od, DSBCAPS_CTRLPOSITIONNOTIFY | DSBCAPS_CTRLVOLUME, 11025 / 4, 0, &u8, {0}};
  IDirectSoundBuffer *shot = NULL;
  CHECK(IDirectSound8_CreateSoundBuffer(g_ds, &od, &shot, NULL) == DS_OK && shot);
  if (shot) {
    IDirectSoundNotify *sn = NULL;
    HANDLE stopped = CreateEventA(NULL, FALSE, FALSE, NULL);
    DSBPOSITIONNOTIFY stop = {DSBPN_OFFSETSTOP, stopped};
    CHECK(IDirectSoundBuffer_QueryInterface(shot, &IID_IDirectSoundNotify, (void **)&sn) == S_OK && sn);
    if (sn) {
      CHECK(IDirectSoundNotify_SetNotificationPositions(sn, 1, &stop) == DS_OK);
      IDirectSoundNotify_Release(sn);
    }
    CHECK(IDirectSoundBuffer_SetVolume(shot, DSBVOLUME_MIN) == DS_OK);
    CHECK(IDirectSoundBuffer_Play(shot, 0, 0, 0) == DS_OK);
    CHECK(WaitForSingleObject(stopped, 3000) == WAIT_OBJECT_0);
    CHECK(IDirectSoundBuffer_GetStatus(shot, &status) == DS_OK && status == 0);
    IDirectSoundBuffer_Release(shot);
  }

  /* 3D: deferred settings wait for CommitDeferredSettings */
  DSBUFFERDESC td = {sizeof td, DSBCAPS_CTRL3D | DSBCAPS_CTRLVOLUME, DS_RATE / 4, 0, &mono, {0}};
  IDirectSoundBuffer *thing = NULL;
  CHECK(IDirectSound8_CreateSoundBuffer(g_ds, &td, &thing, NULL) == DS_OK && thing);
  IDirectSound3DBuffer *b3 = NULL;
  if (thing) CHECK(IDirectSoundBuffer_QueryInterface(thing, &IID_IDirectSound3DBuffer, (void **)&b3) == S_OK && b3);
  if (b3 && listener) {
    D3DVECTOR pos;
    CHECK(IDirectSound3DBuffer_SetPosition(b3, 5, 0, 0, DS3D_DEFERRED) == DS_OK);
    CHECK(IDirectSound3DBuffer_GetPosition(b3, &pos) == DS_OK && pos.x == 0);
    CHECK(IDirectSound3DListener_CommitDeferredSettings(listener) == DS_OK);
    CHECK(IDirectSound3DBuffer_GetPosition(b3, &pos) == DS_OK && pos.x == 5);
    CHECK(IDirectSound3DBuffer_SetMinDistance(b3, 0, DS3D_IMMEDIATE) == DSERR_INVALIDPARAM);
    DS3DBUFFER all = {sizeof all};
    CHECK(IDirectSound3DBuffer_GetAllParameters(b3, &all) == DS_OK && all.dwMode == DS3DMODE_NORMAL && all.flMinDistance == 1.0f);
    D3DVALUE r = 0;
    CHECK(IDirectSound3DListener_SetRolloffFactor(listener, 2, DS3D_IMMEDIATE) == DS_OK && IDirectSound3DListener_GetRolloffFactor(listener, &r) == DS_OK && r == 2);
    CHECK(IDirectSound3DListener_SetRolloffFactor(listener, 11, DS3D_IMMEDIATE) == DSERR_INVALIDPARAM);
    IDirectSound3DBuffer_Release(b3);
  }
  if (thing) IDirectSoundBuffer_Release(thing);
  if (listener) IDirectSound3DListener_Release(listener);
  IDirectSoundBuffer_Release(primary);
  CloseHandle(events[0]);
  CloseHandle(events[1]);
}

static void directsound_stop(void) {
  if (g_tone) {
    DWORD status = 1;
    CHECK(IDirectSoundBuffer_Stop(g_tone) == DS_OK && IDirectSoundBuffer_GetStatus(g_tone, &status) == DS_OK && status == 0);
    IDirectSoundBuffer_Release(g_tone);
  }
  if (g_ds) CHECK(IDirectSound8_Release(g_ds) == 0);
}

/* ---- WASAPI ------------------------------------------------------------------------------------------------------------------------------------ */

typedef struct {
  IAudioEndpointVolumeCallback iface;
  volatile LONG calls;
  float last;
} VolumeCallback;
static HRESULT WINAPI vc_query(IAudioEndpointVolumeCallback *i, REFIID riid, void **out) {
  if (IsEqualGUID(riid, &IID_IUnknown) || IsEqualGUID(riid, &IID_IAudioEndpointVolumeCallback)) {
    *out = i;
    return S_OK;
  }
  *out = NULL;
  return E_NOINTERFACE;
}
static ULONG WINAPI vc_add_ref(IAudioEndpointVolumeCallback *i) { (void)i; return 2; }
static ULONG WINAPI vc_release(IAudioEndpointVolumeCallback *i) { (void)i; return 1; }
static HRESULT WINAPI vc_notify(IAudioEndpointVolumeCallback *i, PAUDIO_VOLUME_NOTIFICATION_DATA data) {
  VolumeCallback *c = (VolumeCallback *)i;
  c->last = data->fMasterVolume;
  InterlockedIncrement(&c->calls);
  return S_OK;
}
static IAudioEndpointVolumeCallbackVtbl kVcVtbl = {vc_query, vc_add_ref, vc_release, vc_notify};

typedef struct {
  IActivateAudioInterfaceCompletionHandler iface;
  HANDLE done;
  HRESULT result;
  IUnknown *object;
} Completion;
static HRESULT WINAPI cp_query(IActivateAudioInterfaceCompletionHandler *i, REFIID riid, void **out) {
  if (IsEqualGUID(riid, &IID_IUnknown) || IsEqualGUID(riid, &IID_IActivateAudioInterfaceCompletionHandler)) {
    *out = i;
    return S_OK;
  }
  *out = NULL;
  return E_NOINTERFACE;
}
static ULONG WINAPI cp_add_ref(IActivateAudioInterfaceCompletionHandler *i) { (void)i; return 2; }
static ULONG WINAPI cp_release(IActivateAudioInterfaceCompletionHandler *i) { (void)i; return 1; }
static HRESULT WINAPI cp_completed(IActivateAudioInterfaceCompletionHandler *i, IActivateAudioInterfaceAsyncOperation *op) {
  Completion *c = (Completion *)i;
  IActivateAudioInterfaceAsyncOperation_GetActivateResult(op, &c->result, &c->object);
  SetEvent(c->done);
  return S_OK;
}
static IActivateAudioInterfaceCompletionHandlerVtbl kCpVtbl = {cp_query, cp_add_ref, cp_release, cp_completed};

typedef HRESULT (WINAPI *ActivateFn)(LPCWSTR, REFIID, PROPVARIANT *, IActivateAudioInterfaceCompletionHandler *, IActivateAudioInterfaceAsyncOperation **);

static void wasapi(void) {
  CHECK(SUCCEEDED(CoInitializeEx(NULL, COINIT_MULTITHREADED)));
  IMMDeviceEnumerator *en = NULL;
  const HRESULT made = CoCreateInstance(&CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL, &IID_IMMDeviceEnumerator, (void **)&en);
  if (made != S_OK) printf("CoCreateInstance(CLSID_MMDeviceEnumerator): %08lx\n", (unsigned long)made);
  CHECK(made == S_OK && en);
  if (!en) return;
  IMMDeviceCollection *col = NULL;
  UINT count = 9;
  CHECK(IMMDeviceEnumerator_EnumAudioEndpoints(en, eRender, DEVICE_STATE_ACTIVE, &col) == S_OK && IMMDeviceCollection_GetCount(col, &count) == S_OK && count == 1);
  if (col) IMMDeviceCollection_Release(col);
  CHECK(IMMDeviceEnumerator_EnumAudioEndpoints(en, eCapture, DEVICE_STATE_ACTIVE, &col) == S_OK && IMMDeviceCollection_GetCount(col, &count) == S_OK && count == 0);
  if (col) IMMDeviceCollection_Release(col);
  IMMDevice *dev = NULL;
  CHECK(IMMDeviceEnumerator_GetDefaultAudioEndpoint(en, eCapture, eConsole, &dev) == HRESULT_FROM_WIN32(ERROR_NOT_FOUND) && dev == NULL);
  CHECK(IMMDeviceEnumerator_GetDefaultAudioEndpoint(en, eRender, eConsole, &dev) == S_OK && dev);
  if (!dev) return;
  LPWSTR id = NULL;
  CHECK(IMMDevice_GetId(dev, &id) == S_OK && id && id[0] == L'{');
  IMMDevice *again = NULL;
  CHECK(IMMDeviceEnumerator_GetDevice(en, id, &again) == S_OK && again);
  CHECK(IMMDeviceEnumerator_GetDevice(en, L"{0.0.0.00000000}.{nothing}", &again) == HRESULT_FROM_WIN32(ERROR_NOT_FOUND));
  CoTaskMemFree(id);
  DWORD state = 0;
  CHECK(IMMDevice_GetState(dev, &state) == S_OK && state == DEVICE_STATE_ACTIVE);
  IMMEndpoint *ep = NULL;
  EDataFlow flow = eAll;
  CHECK(IMMDevice_QueryInterface(dev, &IID_IMMEndpoint, (void **)&ep) == S_OK && IMMEndpoint_GetDataFlow(ep, &flow) == S_OK && flow == eRender);
  IPropertyStore *props = NULL;
  CHECK(IMMDevice_OpenPropertyStore(dev, STGM_READ, &props) == S_OK && props);
  if (props) {
    PROPVARIANT pv;
    CHECK(IPropertyStore_GetValue(props, &PKEY_Device_FriendlyName, &pv) == S_OK && pv.vt == VT_LPWSTR && pv.pwszVal[0]);
    if (pv.vt == VT_LPWSTR) CoTaskMemFree(pv.pwszVal);
    CHECK(IPropertyStore_GetValue(props, &PKEY_AudioEngine_DeviceFormat, &pv) == S_OK && pv.vt == VT_BLOB && ((WAVEFORMATEX *)pv.blob.pBlobData)->nSamplesPerSec == 48000);
    if (pv.vt == VT_BLOB) CoTaskMemFree(pv.blob.pBlobData);
  }

  IAudioClient *client = NULL;
  CHECK(IMMDevice_Activate(dev, &IID_IAudioClient, CLSCTX_ALL, NULL, (void **)&client) == S_OK && client);
  if (!client) return;
  WAVEFORMATEX *mix = NULL;
  CHECK(IAudioClient_GetMixFormat(client, &mix) == S_OK && mix && mix->nSamplesPerSec == 48000 && mix->nChannels == 2 && mix->wBitsPerSample == 32);
  WAVEFORMATEX *closest = (WAVEFORMATEX *)1;
  CHECK(IAudioClient_IsFormatSupported(client, AUDCLNT_SHAREMODE_SHARED, mix, &closest) == S_OK && closest == NULL);
  CHECK(IAudioClient_IsFormatSupported(client, AUDCLNT_SHAREMODE_EXCLUSIVE, mix, NULL) == AUDCLNT_E_UNSUPPORTED_FORMAT);
  WAVEFORMATEX odd = {WAVE_FORMAT_PCM, 2, 44100, 44100 * 4, 4, 12, 0};
  CHECK(IAudioClient_IsFormatSupported(client, AUDCLNT_SHAREMODE_SHARED, &odd, &closest) == S_FALSE && closest && closest->nSamplesPerSec == 48000);
  if (closest && closest != (WAVEFORMATEX *)1) CoTaskMemFree(closest);
  REFERENCE_TIME period = 0, minimum = 0;
  CHECK(IAudioClient_GetDevicePeriod(client, &period, &minimum) == S_OK && period == 100000 && minimum > 0 && minimum <= period);
  UINT32 frames = 0;
  CHECK(IAudioClient_GetBufferSize(client, &frames) == AUDCLNT_E_NOT_INITIALIZED);
  CHECK(IAudioClient_Initialize(client, AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, 2000000, 0, mix, NULL) == S_OK);
  CHECK(IAudioClient_Initialize(client, AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, 0, 0, mix, NULL) == AUDCLNT_E_ALREADY_INITIALIZED);
  CHECK(IAudioClient_GetBufferSize(client, &frames) == S_OK && frames >= 480);
  CHECK(IAudioClient_Start(client) == AUDCLNT_E_EVENTHANDLE_NOT_SET);
  HANDLE ready = CreateEventA(NULL, FALSE, FALSE, NULL);
  CHECK(IAudioClient_SetEventHandle(client, ready) == S_OK);
  IAudioRenderClient *render = NULL;
  IAudioClock *clock = NULL;
  IAudioStreamVolume *sv = NULL;
  ISimpleAudioVolume *simple = NULL;
  CHECK(IAudioClient_GetService(client, &IID_IAudioRenderClient, (void **)&render) == S_OK && render);
  CHECK(IAudioClient_GetService(client, &IID_IAudioClock, (void **)&clock) == S_OK && clock);
  CHECK(IAudioClient_GetService(client, &IID_IAudioStreamVolume, (void **)&sv) == S_OK && sv);
  CHECK(IAudioClient_GetService(client, &IID_ISimpleAudioVolume, (void **)&simple) == S_OK && simple);
  IAudioCaptureClient *cap = NULL;
  CHECK(IAudioClient_GetService(client, &IID_IAudioCaptureClient, (void **)&cap) == AUDCLNT_E_WRONG_ENDPOINT_TYPE);
  if (!render || !clock) return;

  BYTE *data = NULL;
  CHECK(IAudioRenderClient_GetBuffer(render, frames + 1, &data) == AUDCLNT_E_BUFFER_TOO_LARGE);
  /* 700 Hz for three seconds, refilled on every event (from a table: 480 frames are 7 whole cycles, and the emulator is slow at maths) */
  static float wave700[480];
  for (int i = 0; i < 480; ++i) wave700[i] = (float)(0.25 * tsin(2.0 * 3.14159265358979 * (double)((700 * i) % 48000) / 48000.0));
  long long phase = 0;
  int events = 0;
  UINT32 padding = 0;
  CHECK(IAudioRenderClient_GetBuffer(render, frames, &data) == S_OK && data);
  for (UINT32 i = 0; i < frames; ++i, ++phase) ((float *)data)[2 * i] = ((float *)data)[2 * i + 1] = wave700[phase % 480];
  CHECK(IAudioRenderClient_ReleaseBuffer(render, frames, 0) == S_OK);
  CHECK(IAudioClient_GetCurrentPadding(client, &padding) == S_OK && padding == frames);
  CHECK(IAudioClient_Start(client) == S_OK);
  CHECK(IAudioClient_Start(client) == AUDCLNT_E_NOT_STOPPED);
  const DWORD start = GetTickCount();
  while (GetTickCount() - start < 3000) {
    if (WaitForSingleObject(ready, 500) == WAIT_OBJECT_0) ++events;
    if (IAudioClient_GetCurrentPadding(client, &padding) != S_OK) break;
    const UINT32 room = frames - padding;
    if (room == 0) continue;
    if (IAudioRenderClient_GetBuffer(render, room, &data) != S_OK) break;
    for (UINT32 i = 0; i < room; ++i, ++phase) ((float *)data)[2 * i] = ((float *)data)[2 * i + 1] = wave700[phase % 480];
    IAudioRenderClient_ReleaseBuffer(render, room, 0);
  }
  UINT64 freq = 0, pos = 0;
  CHECK(IAudioClock_GetFrequency(clock, &freq) == S_OK && freq == 48000 * 8);
  CHECK(IAudioClock_GetPosition(clock, &pos, NULL) == S_OK);
  printf("wasapi: %d events in 3 s, %lld frames written, clock at %.2f s\n", events, phase, freq ? (double)pos / (double)freq : 0.0);
  CHECK(events > 20);
  CHECK(pos > freq && pos <= (UINT64)phase * 8);

  /* volumes */
  float level = 0;
  CHECK(IAudioStreamVolume_SetChannelVolume(sv, 1, 0.5f) == S_OK && IAudioStreamVolume_GetChannelVolume(sv, 1, &level) == S_OK && level == 0.5f);
  CHECK(IAudioStreamVolume_SetChannelVolume(sv, 2, 0.5f) == E_INVALIDARG && IAudioStreamVolume_SetChannelVolume(sv, 0, 1.5f) == E_INVALIDARG);
  CHECK(IAudioStreamVolume_SetChannelVolume(sv, 1, 1.0f) == S_OK);
  CHECK(ISimpleAudioVolume_SetMasterVolume(simple, 0.8f, NULL) == S_OK && ISimpleAudioVolume_GetMasterVolume(simple, &level) == S_OK && level == 0.8f);
  CHECK(ISimpleAudioVolume_SetMasterVolume(simple, 1.0f, NULL) == S_OK);

  /* stopped, the padding stays; reset, it is gone and the clock starts again */
  CHECK(IAudioClient_Stop(client) == S_OK && IAudioClient_Stop(client) == S_FALSE);
  UINT32 p1 = 0, p2 = 0;
  IAudioClient_GetCurrentPadding(client, &p1);
  Sleep(150);
  IAudioClient_GetCurrentPadding(client, &p2);
  CHECK(p1 == p2);
  CHECK(IAudioClient_Reset(client) == S_OK && IAudioClient_GetCurrentPadding(client, &p2) == S_OK && p2 == 0);
  CHECK(IAudioClock_GetPosition(clock, &pos, NULL) == S_OK && pos == 0);

  /* the endpoint's volume is the master volume, with a callback */
  IAudioEndpointVolume *ev = NULL;
  CHECK(IMMDevice_Activate(dev, &IID_IAudioEndpointVolume, CLSCTX_ALL, NULL, (void **)&ev) == S_OK && ev);
  if (ev) {
    VolumeCallback vc = {{&kVcVtbl}, 0, 0};
    float before = 0, now = 0;
    CHECK(IAudioEndpointVolume_GetMasterVolumeLevelScalar(ev, &before) == S_OK && before > 0.9f);
    CHECK(IAudioEndpointVolume_RegisterControlChangeNotify(ev, &vc.iface) == S_OK);
    CHECK(IAudioEndpointVolume_SetMasterVolumeLevelScalar(ev, 0.5f, NULL) == S_OK);
    CHECK(IAudioEndpointVolume_GetMasterVolumeLevelScalar(ev, &now) == S_OK && now > 0.48f && now < 0.52f);
    CHECK(vc.calls == 1 && vc.last > 0.48f && vc.last < 0.52f);
    float db = 0;
    CHECK(IAudioEndpointVolume_GetMasterVolumeLevel(ev, &db) == S_OK && db < -17 && db > -19);      /* (0.5 cubed: -18 dB) */
    CHECK(IAudioEndpointVolume_SetMasterVolumeLevelScalar(ev, before, NULL) == S_OK);
    BOOL mute = TRUE;
    CHECK(IAudioEndpointVolume_GetMute(ev, &mute) == S_OK && !mute);
    CHECK(IAudioEndpointVolume_UnregisterControlChangeNotify(ev, &vc.iface) == S_OK);
    CHECK(IAudioEndpointVolume_UnregisterControlChangeNotify(ev, &vc.iface) == HRESULT_FROM_WIN32(ERROR_NOT_FOUND));
    IAudioEndpointVolume_Release(ev);
  }
  IAudioSessionManager2 *sm = NULL;
  CHECK(IMMDevice_Activate(dev, &IID_IAudioSessionManager2, CLSCTX_ALL, NULL, (void **)&sm) == S_OK && sm);
  if (sm) {
    ISimpleAudioVolume *sv2 = NULL;
    CHECK(IAudioSessionManager2_GetSimpleAudioVolume(sm, NULL, 0, &sv2) == S_OK && sv2 && ISimpleAudioVolume_GetMasterVolume(sv2, &level) == S_OK && level == 1.0f);
    IAudioSessionManager2_Release(sm);
  }

  /* ActivateAudioInterfaceAsync, as a UWP-style program does it */
  HMODULE mmdev = LoadLibraryA("mmdevapi.dll");
  ActivateFn activate = mmdev ? (ActivateFn)(void *)GetProcAddress(mmdev, "ActivateAudioInterfaceAsync") : NULL;
  CHECK(activate != NULL);
  if (activate) {
    Completion c = {{&kCpVtbl}, CreateEventA(NULL, TRUE, FALSE, NULL), E_FAIL, NULL};
    IActivateAudioInterfaceAsyncOperation *op = NULL;
    CHECK(activate(L"{E6327CAD-DCEC-4949-AE8A-991E976A79D2}", &IID_IAudioClient, NULL, &c.iface, &op) == S_OK && op);
    CHECK(WaitForSingleObject(c.done, 3000) == WAIT_OBJECT_0 && c.result == S_OK && c.object);
    if (c.object) IUnknown_Release(c.object);
    if (op) IActivateAudioInterfaceAsyncOperation_Release(op);
  }

  CoTaskMemFree(mix);
  IAudioStreamVolume_Release(sv);
  IAudioClock_Release(clock);
  IAudioRenderClient_Release(render);
  CHECK(IAudioClient_Release(client) == 0);
  IMMDevice_Release(dev);
  IMMDeviceEnumerator_Release(en);
  CoUninitialize();
}

int main(void) {
  directsound_start();
  wasapi();
  directsound_stop();
  printf("dsoundtest: %d checks, %d failed\n", checks, failed);
  return failed ? 1 : 0;
}
