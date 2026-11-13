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

/* mmdevapi.dll for TheoryCraftOS: the Core Audio API (MMDevice and WASAPI) over the kernel's PipeWire graph (NtTcosAudio, src/Process/WindowsAudio.hpp,
 * tcos_audio.h). In Windows the audio client lives in audioses.dll; programs only reach it through IMMDevice::Activate, so it is here.
 *
 *  - CLSID_MMDeviceEnumerator (DllGetClassObject; ole32 finds this library for it) enumerates one render endpoint, the graph's sink, while the kernel
 *    has an audio device, one more render endpoint for each of the graph's virtual sinks (its null sinks: their clients play into them and their
 *    loopback clients record them; their volume is the sink's own), and one capture endpoint, the graph's source, while there is a microphone. Their
 *    property stores have the names, form factor, speakers and the engine's format (32-bit float stereo at 48 kHz).
 *  - IAudioClient/2/3 in shared mode: every initialized client is a stream of the graph in the client's own format (PCM 8/16/24/32-bit or float, any
 *    rate the graph takes: the kernel converts, as AUTOCONVERTPCM would), mixed there with the native, Linux and other Windows programs' sound. The
 *    client's buffer is a ring in this process; what the program releases goes to the kernel as fast as its stream takes it, the padding is what the
 *    graph's mixer has not read yet (as in Windows: not what the device has still to play), the clock is what the device has played, and a thread
 *    signals the event every period (10 ms) while the stream runs. A client of the capture endpoint is a record stream of the graph: the thread reads
 *    what the kernel recorded into the ring, IAudioCaptureClient hands it out a packet (10 ms) at a time with its device position, the padding is what
 *    waits to be read, and audio lost because the program read too slowly flags the next packet AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY. Exclusive mode
 *    is refused (AUDCLNT_E_EXCLUSIVE_MODE_NOT_ALLOWED). A render endpoint's client made with AUDCLNT_STREAMFLAGS_LOOPBACK records what the mixer
 *    plays (the graph's monitor) as a capture client does, except that it hears nothing while nothing plays, as in Windows.
 *  - IAudioRenderClient, IAudioClock (bytes per second), IAudioStreamVolume and IChannelAudioVolume (the first two channels set the stream's volume
 *    and balance), and the process's one audio session (ISimpleAudioVolume, IAudioSessionControl2, IAudioSessionManager2): the session's volume and
 *    mute apply to every stream of the process.
 *  - IAudioEndpointVolume is the graph's master volume on the render endpoint and the microphone's master gain on the capture endpoint (what pactl and
 *    wpctl show as the sink's and the source's: scalar = cube root of the amplitude, so 50% here is 50% there), with change notifications (for changes made
 *    in this process at once; for changes from outside it, pactl or the desktop, within 100 ms through a watcher thread); IAudioMeterInformation reads silence. The session's volume applies to the process's render streams; its events
 *    (IAudioSessionEvents) tell of volume and mute changes and of the session becoming active or inactive. IMMNotificationClient hears of endpoints
 *    coming and going (a microphone plugged in or pulled out).
 *  - ActivateAudioInterfaceAsync activates on the default endpoints (DEVINTERFACE_AUDIO_RENDER, DEVINTERFACE_AUDIO_CAPTURE or an endpoint's id) and
 *    calls the handler on a thread of its own.
 * Built freestanding over kernel32.dll, ntdll.dll and ole32.dll (CoTaskMemAlloc). */
#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#include <windows.h>
#include <mmsystem.h>
#include <initguid.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <audiopolicy.h>
#include <endpointvolume.h>
#include <functiondiscoverykeys_devpkey.h>
#include "../tcos_audio.h"

#define EXPORT __declspec(dllexport)

__asm__(".text\n.globl ___chkstk_ms\n___chkstk_ms:\n ret\n");

/* (GUIDs the headers declare without defining them: this library is not linked with uuid) */
static const GUID kIidUnknown = {0x00000000, 0x0000, 0x0000, {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
static const GUID kIidClassFactory = {0x00000001, 0x0000, 0x0000, {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
static const GUID kIidAgileObject = {0x94EA2B94, 0xE9CC, 0x49E0, {0xC0, 0xFF, 0xEE, 0x64, 0xCA, 0x8F, 0x5B, 0x90}};
static const GUID kIidDirectSound = {0x279AFA83, 0x4981, 0x11CE, {0xA5, 0x21, 0x00, 0x20, 0xAF, 0x0B, 0xE5, 0x60}};
static const GUID kIidDirectSound8 = {0xC50A7E93, 0xF395, 0x4834, {0x9E, 0xF6, 0x7F, 0xA9, 0x9D, 0xE5, 0x09, 0x66}};
static const GUID kEndpointGuid = {0x5AF3A3C2, 0x6E1B, 0x4B4E, {0x8D, 0x2F, 0x1C, 0x0A, 0x7E, 0x5D, 0x9B, 0x30}};      /* (dsound.dll's device GUID too) */
static const GUID kSubtypeFloat = {0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71}};

#define E_NOTFOUND_ HRESULT_FROM_WIN32(ERROR_NOT_FOUND)
#define MIX_RATE 48000
#define DEFAULT_PERIOD 100000      /* 10 ms in 100 ns units */
#define MINIMUM_PERIOD 30000       /* 3 ms */
#define MINIMUM_BUFFER 300000      /* 30 ms */
#define MIN_DB (-96.0f)

static const WCHAR kEndpointId[] = L"{0.0.0.00000000}.{5af3a3c2-6e1b-4b4e-8d2f-1c0a7e5d9b30}";
static const WCHAR kEndpointGuidString[] = L"{5af3a3c2-6e1b-4b4e-8d2f-1c0a7e5d9b30}";
static const WCHAR kRenderInterface[] = L"{E6327CAD-DCEC-4949-AE8A-991E976A79D2}";      /* DEVINTERFACE_AUDIO_RENDER */
static const WCHAR kFriendlyName[] = L"Speakers (TheoryCraftOS)";
static const WCHAR kDeviceDescription[] = L"Speakers";
static const WCHAR kInterfaceName[] = L"TheoryCraftOS Audio";
/* the capture endpoint: the graph's source (dsound.dll's capture device GUID too) */
static const GUID kCaptureEndpointGuid = {0x5AF3A3C3, 0x6E1B, 0x4B4E, {0x8D, 0x2F, 0x1C, 0x0A, 0x7E, 0x5D, 0x9B, 0x31}};
static const GUID kIidDirectSoundCapture = {0xB0210781, 0x89CD, 0x11D0, {0xAF, 0x08, 0x00, 0xA0, 0xC9, 0x25, 0xCD, 0x16}};
static const WCHAR kCaptureEndpointId[] = L"{0.0.1.00000000}.{5af3a3c3-6e1b-4b4e-8d2f-1c0a7e5d9b31}";
static const WCHAR kCaptureGuidString[] = L"{5af3a3c3-6e1b-4b4e-8d2f-1c0a7e5d9b31}";
static const WCHAR kCaptureInterface[] = L"{2EEF81BE-33FA-4800-9670-1CD474972C3F}";      /* DEVINTERFACE_AUDIO_CAPTURE */
static const WCHAR kCaptureFriendlyName[] = L"Microphone (TheoryCraftOS)";
static const WCHAR kCaptureDescription[] = L"Microphone";
#define RENDER 0
#define CAPTURE 1
#define FIRST_VIRTUAL 2
#define ENDPOINTS (FIRST_VIRTUAL + TCOS_AUDIO_MAX_VIRTUAL)

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
static SIZE_T wide_length(const WCHAR *s) {
  SIZE_T n = 0;
  while (s[n]) ++n;
  return n;
}
static BOOL wide_equal_nocase(const WCHAR *a, const WCHAR *b) {
  for (;; ++a, ++b) {
    WCHAR x = *a, y = *b;
    if (x >= 'a' && x <= 'z') x = (WCHAR)(x - 32);
    if (y >= 'a' && y <= 'z') y = (WCHAR)(y - 32);
    if (x != y) return FALSE;
    if (!x) return TRUE;
  }
}
/* A copy in CoTaskMemAlloc memory (the caller frees it with CoTaskMemFree). */
static WCHAR *task_string(const WCHAR *s) {
  const SIZE_T n = wide_length(s) + 1;
  WCHAR *r = (WCHAR *)CoTaskMemAlloc(n * sizeof(WCHAR));
  if (r) copy(r, s, n * sizeof(WCHAR));
  return r;
}
static BOOL has_device(void) { return tcos_audio_devices() != 0; }

/* The graph's virtual sinks (its null sinks: pw-cli create-node adapter ..., or PulseAudio's) as more render endpoints, FIRST_VIRTUAL onwards. A sink
 * keeps its slot while it is there (refresh_virtual, when the endpoints are listed or looked up); a slot whose sink went is empty (node 0). */
typedef struct {
  ULONG64 node;
  WCHAR name[96];
  WCHAR id[72];
  WCHAR guid[40];
} VirtualSink;
static VirtualSink g_virtual[TCOS_AUDIO_MAX_VIRTUAL];
static SRWLOCK g_virtual_lock = SRWLOCK_INIT;
static BOOL is_virtual(int which) { return which >= FIRST_VIRTUAL && which < ENDPOINTS; }
static ULONG64 virtual_node(int which) {
  if (!is_virtual(which)) return 0;
  AcquireSRWLockShared(&g_virtual_lock);
  const ULONG64 node = g_virtual[which - FIRST_VIRTUAL].node;
  ReleaseSRWLockShared(&g_virtual_lock);
  return node;
}
static void hex_tail(WCHAR *out, ULONG64 v, int digits) {
  for (int k = digits - 1; k >= 0; --k, v >>= 4) out[k] = (WCHAR)("0123456789abcdef"[v & 0xF]);
}
static void refresh_virtual(void) {
  ULONG64 nodes[TCOS_AUDIO_MAX_VIRTUAL] = {0};
  char names[TCOS_AUDIO_MAX_VIRTUAL][96];
  ULONG64 count = 0;
  int found = 0;
  for (ULONG64 k = 0; k < TCOS_AUDIO_MAX_VIRTUAL; ++k) {
    names[found][0] = 0;
    const ULONG64 node = tcos_audio_virtual_sink(k, names[found], sizeof names[found], &count);
    if (!node) break;
    nodes[found++] = node;
  }
  AcquireSRWLockExclusive(&g_virtual_lock);
  /* (the slots of sinks that went are emptied; the sinks that came take free slots) */
  for (int slot = 0; slot < TCOS_AUDIO_MAX_VIRTUAL; ++slot) {
    BOOL still = FALSE;
    for (int f = 0; f < found; ++f) still |= nodes[f] == g_virtual[slot].node;
    if (!still) g_virtual[slot].node = 0;
  }
  for (int f = 0; f < found; ++f) {
    int slot = -1;
    for (int k = 0; k < TCOS_AUDIO_MAX_VIRTUAL && slot < 0; ++k) if (g_virtual[k].node == nodes[f]) slot = k;
    for (int k = 0; k < TCOS_AUDIO_MAX_VIRTUAL && slot < 0; ++k) if (g_virtual[k].node == 0) slot = k;
    if (slot < 0) continue;
    VirtualSink *v = &g_virtual[slot];
    v->node = nodes[f];
    int n = 0;
    for (const char *c = names[f]; *c && n < 95; ++c) v->name[n++] = (WCHAR)(unsigned char)*c;      /* (ASCII as it comes; more is shown as it is) */
    v->name[n] = 0;
    /* its id: the render endpoint's, with the node in the last part of the GUID */
    const WCHAR guid[] = L"{5af3a3c2-6e1b-4b4e-8d2f-000000000000}";
    copy(v->guid, guid, sizeof guid);
    hex_tail(v->guid + 25, nodes[f], 12);
    const WCHAR prefix[] = L"{0.0.0.00000000}.";
    copy(v->id, prefix, sizeof prefix - sizeof(WCHAR));
    copy(v->id + (sizeof prefix / sizeof(WCHAR) - 1), v->guid, sizeof v->guid);
  }
  ReleaseSRWLockExclusive(&g_virtual_lock);
}
static BOOL has_endpoint(int which) {
  if (is_virtual(which)) return virtual_node(which) != 0 && has_device();
  return which == CAPTURE ? tcos_audio_input_devices() != 0 : has_device();
}

/* The engine's format: 32-bit float stereo at 48 kHz. */
static void mix_format(WAVEFORMATEXTENSIBLE *f) {
  zero(f, sizeof *f);
  f->Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
  f->Format.nChannels = 2;
  f->Format.nSamplesPerSec = MIX_RATE;
  f->Format.wBitsPerSample = 32;
  f->Format.nBlockAlign = 8;
  f->Format.nAvgBytesPerSec = MIX_RATE * 8;
  f->Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
  f->Samples.wValidBitsPerSample = 32;
  f->dwChannelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
  f->SubFormat = kSubtypeFloat;
}
static WAVEFORMATEX *task_mix_format(void) {
  WAVEFORMATEXTENSIBLE *f = (WAVEFORMATEXTENSIBLE *)CoTaskMemAlloc(sizeof *f);
  if (f) mix_format(f);
  return (WAVEFORMATEX *)f;
}

/* ---- the process's audio session ---------------------------------------------------------------------------------------------------------------- */

typedef struct Client Client;
static SRWLOCK g_lock = SRWLOCK_INIT;      /* the session's values and the list of clients */
static float g_session_volume = 1.0f;
static BOOL g_session_mute;
static WCHAR g_display_name[260], g_icon_path[260];
static GUID g_grouping;
static Client *g_clients;

static void apply_session_to_all(void);
static BOOL any_started(void);

/* A list of the program's callback objects (session events, notification clients, endpoint volume callbacks). They are called with no lock held:
 * listeners_copy takes a reference to each, the caller calls them and listeners_drop lets go. */
#define MAX_LISTENERS 16
typedef struct {
  SRWLOCK lock;
  IUnknown *items[MAX_LISTENERS];
} Listeners;
static HRESULT listeners_add(Listeners *l, IUnknown *item) {
  if (!item) return E_POINTER;
  HRESULT hr = E_OUTOFMEMORY;
  AcquireSRWLockExclusive(&l->lock);
  for (int k = 0; k < MAX_LISTENERS; ++k)
    if (!l->items[k]) {
      l->items[k] = item;
      IUnknown_AddRef(item);
      hr = S_OK;
      break;
    }
  ReleaseSRWLockExclusive(&l->lock);
  return hr;
}
static HRESULT listeners_remove(Listeners *l, IUnknown *item) {
  if (!item) return E_POINTER;
  IUnknown *found = NULL;
  AcquireSRWLockExclusive(&l->lock);
  for (int k = 0; k < MAX_LISTENERS; ++k)
    if (l->items[k] == item) {
      found = item;
      l->items[k] = NULL;
      break;
    }
  ReleaseSRWLockExclusive(&l->lock);
  if (!found) return E_NOTFOUND_;
  IUnknown_Release(found);
  return S_OK;
}
static int listeners_copy(Listeners *l, IUnknown **out) {
  int n = 0;
  AcquireSRWLockShared(&l->lock);
  for (int k = 0; k < MAX_LISTENERS; ++k)
    if (l->items[k]) {
      out[n++] = l->items[k];
      IUnknown_AddRef(l->items[k]);
    }
  ReleaseSRWLockShared(&l->lock);
  return n;
}
static void listeners_drop(IUnknown **items, int n) {
  for (int k = 0; k < n; ++k) IUnknown_Release(items[k]);
}

static Listeners g_session_events = {SRWLOCK_INIT, {0}};
static void notify_session_volume(LPCGUID context) {
  AcquireSRWLockShared(&g_lock);
  const float level = g_session_volume;
  const BOOL mute = g_session_mute;
  ReleaseSRWLockShared(&g_lock);
  IUnknown *items[MAX_LISTENERS];
  const int n = listeners_copy(&g_session_events, items);
  for (int k = 0; k < n; ++k) IAudioSessionEvents_OnSimpleVolumeChanged((IAudioSessionEvents *)items[k], level, mute, context);
  listeners_drop(items, n);
}
/* The session became active (a stream started) or inactive (the last one stopped). */
static void notify_session_state(BOOL active) {
  IUnknown *items[MAX_LISTENERS];
  const int n = listeners_copy(&g_session_events, items);
  for (int k = 0; k < n; ++k) IAudioSessionEvents_OnStateChanged((IAudioSessionEvents *)items[k], active ? AudioSessionStateActive : AudioSessionStateInactive);
  listeners_drop(items, n);
}

static HRESULT session_query(REFIID riid, void **out);

static HRESULT WINAPI sv_query(ISimpleAudioVolume *i, REFIID riid, void **out) { (void)i; return session_query(riid, out); }
static ULONG WINAPI sv_add_ref(ISimpleAudioVolume *i) { (void)i; return 2; }
static ULONG WINAPI sv_release(ISimpleAudioVolume *i) { (void)i; return 1; }
static HRESULT WINAPI sv_set_master(ISimpleAudioVolume *i, float level, LPCGUID context) {
  (void)i;
  if (!(level >= 0.0f && level <= 1.0f)) return E_INVALIDARG;
  AcquireSRWLockExclusive(&g_lock);
  const BOOL changed = g_session_volume != level;
  g_session_volume = level;
  ReleaseSRWLockExclusive(&g_lock);
  apply_session_to_all();
  if (changed) notify_session_volume(context);
  return S_OK;
}
static HRESULT WINAPI sv_get_master(ISimpleAudioVolume *i, float *level) {
  (void)i;
  if (!level) return E_POINTER;
  AcquireSRWLockShared(&g_lock);
  *level = g_session_volume;
  ReleaseSRWLockShared(&g_lock);
  return S_OK;
}
static HRESULT WINAPI sv_set_mute(ISimpleAudioVolume *i, WINBOOL mute, LPCGUID context) {
  (void)i;
  AcquireSRWLockExclusive(&g_lock);
  const BOOL changed = g_session_mute != (mute ? TRUE : FALSE);
  g_session_mute = mute ? TRUE : FALSE;
  ReleaseSRWLockExclusive(&g_lock);
  apply_session_to_all();
  if (changed) notify_session_volume(context);
  return S_OK;
}
static HRESULT WINAPI sv_get_mute(ISimpleAudioVolume *i, WINBOOL *mute) {
  (void)i;
  if (!mute) return E_POINTER;
  AcquireSRWLockShared(&g_lock);
  *mute = g_session_mute;
  ReleaseSRWLockShared(&g_lock);
  return S_OK;
}
static const ISimpleAudioVolumeVtbl kSimpleVolumeVtbl = {sv_query, sv_add_ref, sv_release, sv_set_master, sv_get_master, sv_set_mute, sv_get_mute};
static ISimpleAudioVolume g_simple_volume = {(ISimpleAudioVolumeVtbl *)&kSimpleVolumeVtbl};

static HRESULT task_copy(const WCHAR *s, LPWSTR *out) {
  if (!out) return E_POINTER;
  *out = task_string(s);
  return *out ? S_OK : E_OUTOFMEMORY;
}

static HRESULT WINAPI sc_query(IAudioSessionControl2 *i, REFIID riid, void **out) { (void)i; return session_query(riid, out); }
static ULONG WINAPI sc_add_ref(IAudioSessionControl2 *i) { (void)i; return 2; }
static ULONG WINAPI sc_release(IAudioSessionControl2 *i) { (void)i; return 1; }
static HRESULT WINAPI sc_get_state(IAudioSessionControl2 *i, AudioSessionState *state) {
  (void)i;
  if (!state) return E_POINTER;
  *state = any_started() ? AudioSessionStateActive : AudioSessionStateInactive;
  return S_OK;
}
static HRESULT WINAPI sc_get_display_name(IAudioSessionControl2 *i, LPWSTR *name) {
  (void)i;
  AcquireSRWLockShared(&g_lock);
  const HRESULT hr = task_copy(g_display_name, name);
  ReleaseSRWLockShared(&g_lock);
  return hr;
}
static void set_string(WCHAR *to, LPCWSTR from) {
  SIZE_T k = 0;
  for (; from && from[k] && k + 1 < 260; ++k) to[k] = from[k];
  to[k] = 0;
}
static HRESULT WINAPI sc_set_display_name(IAudioSessionControl2 *i, LPCWSTR name, LPCGUID context) {
  (void)i; (void)context;
  if (!name) return E_POINTER;
  AcquireSRWLockExclusive(&g_lock);
  set_string(g_display_name, name);
  ReleaseSRWLockExclusive(&g_lock);
  return S_OK;
}
static HRESULT WINAPI sc_get_icon_path(IAudioSessionControl2 *i, LPWSTR *path) {
  (void)i;
  AcquireSRWLockShared(&g_lock);
  const HRESULT hr = task_copy(g_icon_path, path);
  ReleaseSRWLockShared(&g_lock);
  return hr;
}
static HRESULT WINAPI sc_set_icon_path(IAudioSessionControl2 *i, LPCWSTR path, LPCGUID context) {
  (void)i; (void)context;
  if (!path) return E_POINTER;
  AcquireSRWLockExclusive(&g_lock);
  set_string(g_icon_path, path);
  ReleaseSRWLockExclusive(&g_lock);
  return S_OK;
}
static HRESULT WINAPI sc_get_grouping(IAudioSessionControl2 *i, GUID *grouping) {
  (void)i;
  if (!grouping) return E_POINTER;
  AcquireSRWLockShared(&g_lock);
  *grouping = g_grouping;
  ReleaseSRWLockShared(&g_lock);
  return S_OK;
}
static HRESULT WINAPI sc_set_grouping(IAudioSessionControl2 *i, LPCGUID grouping, LPCGUID context) {
  (void)i; (void)context;
  if (!grouping) return E_POINTER;
  AcquireSRWLockExclusive(&g_lock);
  g_grouping = *grouping;
  ReleaseSRWLockExclusive(&g_lock);
  return S_OK;
}
/* (no session events are ever sent: the session does not change from outside the process) */
static HRESULT WINAPI sc_register(IAudioSessionControl2 *i, IAudioSessionEvents *events) { (void)i; return listeners_add(&g_session_events, (IUnknown *)events); }
static HRESULT WINAPI sc_unregister(IAudioSessionControl2 *i, IAudioSessionEvents *events) { (void)i; return listeners_remove(&g_session_events, (IUnknown *)events); }
static HRESULT WINAPI sc_get_identifier(IAudioSessionControl2 *i, LPWSTR *id) {
  (void)i;
  WCHAR s[160];
  SIZE_T k = 0;
  for (const WCHAR *p = kEndpointId; *p && k < 100; ++p) s[k++] = *p;
  const WCHAR tail[] = L"|#%b{00000000-0000-0000-0000-000000000000}";
  for (const WCHAR *p = tail; *p && k < 159; ++p) s[k++] = *p;
  s[k] = 0;
  return task_copy(s, id);
}
static HRESULT WINAPI sc_get_instance_identifier(IAudioSessionControl2 *i, LPWSTR *id) {
  (void)i;
  WCHAR s[160];
  SIZE_T k = 0;
  for (const WCHAR *p = kEndpointId; *p && k < 100; ++p) s[k++] = *p;
  const WCHAR tail[] = L"|#%b{00000000-0000-0000-0000-000000000000}|";
  for (const WCHAR *p = tail; *p && k < 150; ++p) s[k++] = *p;
  DWORD pid = GetCurrentProcessId();
  WCHAR digits[12];
  int n = 0;
  do { digits[n++] = (WCHAR)('0' + pid % 10); pid /= 10; } while (pid && n < 11);
  while (n > 0 && k < 159) s[k++] = digits[--n];
  s[k] = 0;
  return task_copy(s, id);
}
static HRESULT WINAPI sc_get_process_id(IAudioSessionControl2 *i, DWORD *pid) {
  (void)i;
  if (!pid) return E_POINTER;
  *pid = GetCurrentProcessId();
  return S_OK;
}
static HRESULT WINAPI sc_is_system_sounds(IAudioSessionControl2 *i) { (void)i; return S_FALSE; }
static HRESULT WINAPI sc_set_ducking(IAudioSessionControl2 *i, WINBOOL opt_out) { (void)i; (void)opt_out; return S_OK; }
static const IAudioSessionControl2Vtbl kSessionControlVtbl = {
  sc_query, sc_add_ref, sc_release, sc_get_state, sc_get_display_name, sc_set_display_name, sc_get_icon_path, sc_set_icon_path, sc_get_grouping,
  sc_set_grouping, sc_register, sc_unregister, sc_get_identifier, sc_get_instance_identifier, sc_get_process_id, sc_is_system_sounds, sc_set_ducking};
static IAudioSessionControl2 g_session_control = {(IAudioSessionControl2Vtbl *)&kSessionControlVtbl};

static HRESULT session_query(REFIID riid, void **out) {
  if (!out) return E_POINTER;
  *out = NULL;
  if (same_guid(riid, &kIidUnknown) || same_guid(riid, &IID_IAudioSessionControl) || same_guid(riid, &IID_IAudioSessionControl2)) *out = &g_session_control;
  else if (same_guid(riid, &IID_ISimpleAudioVolume)) *out = &g_simple_volume;
  else return E_NOINTERFACE;
  return S_OK;
}

/* The session list: this process's one session. */
static HRESULT WINAPI se_query(IAudioSessionEnumerator *i, REFIID riid, void **out) {
  if (!out) return E_POINTER;
  *out = NULL;
  if (!same_guid(riid, &kIidUnknown) && !same_guid(riid, &IID_IAudioSessionEnumerator)) return E_NOINTERFACE;
  *out = i;
  return S_OK;
}
static ULONG WINAPI se_add_ref(IAudioSessionEnumerator *i) { (void)i; return 2; }
static ULONG WINAPI se_release(IAudioSessionEnumerator *i) { (void)i; return 1; }
static HRESULT WINAPI se_get_count(IAudioSessionEnumerator *i, int *count) {
  (void)i;
  if (!count) return E_POINTER;
  *count = 1;
  return S_OK;
}
static HRESULT WINAPI se_get_session(IAudioSessionEnumerator *i, int index, IAudioSessionControl **out) {
  (void)i;
  if (!out) return E_POINTER;
  *out = NULL;
  if (index != 0) return E_INVALIDARG;
  *out = (IAudioSessionControl *)&g_session_control;
  return S_OK;
}
static const IAudioSessionEnumeratorVtbl kSessionEnumVtbl = {se_query, se_add_ref, se_release, se_get_count, se_get_session};
static IAudioSessionEnumerator g_session_enum = {(IAudioSessionEnumeratorVtbl *)&kSessionEnumVtbl};

static HRESULT WINAPI sm_query(IAudioSessionManager2 *i, REFIID riid, void **out) {
  if (!out) return E_POINTER;
  *out = NULL;
  if (!same_guid(riid, &kIidUnknown) && !same_guid(riid, &IID_IAudioSessionManager) && !same_guid(riid, &IID_IAudioSessionManager2)) return E_NOINTERFACE;
  *out = i;
  return S_OK;
}
static ULONG WINAPI sm_add_ref(IAudioSessionManager2 *i) { (void)i; return 2; }
static ULONG WINAPI sm_release(IAudioSessionManager2 *i) { (void)i; return 1; }
static HRESULT WINAPI sm_get_control(IAudioSessionManager2 *i, LPCGUID session, DWORD flags, IAudioSessionControl **out) {
  (void)i; (void)session; (void)flags;
  if (!out) return E_POINTER;
  *out = (IAudioSessionControl *)&g_session_control;
  return S_OK;
}
static HRESULT WINAPI sm_get_volume(IAudioSessionManager2 *i, LPCGUID session, DWORD flags, ISimpleAudioVolume **out) {
  (void)i; (void)session; (void)flags;
  if (!out) return E_POINTER;
  *out = &g_simple_volume;
  return S_OK;
}
static HRESULT WINAPI sm_get_enumerator(IAudioSessionManager2 *i, IAudioSessionEnumerator **out) {
  (void)i;
  if (!out) return E_POINTER;
  *out = &g_session_enum;
  return S_OK;
}
static HRESULT WINAPI sm_register(IAudioSessionManager2 *i, IAudioSessionNotification *n) { (void)i; return n ? S_OK : E_POINTER; }
static HRESULT WINAPI sm_unregister(IAudioSessionManager2 *i, IAudioSessionNotification *n) { (void)i; return n ? S_OK : E_POINTER; }
static HRESULT WINAPI sm_register_duck(IAudioSessionManager2 *i, LPCWSTR id, IAudioVolumeDuckNotification *n) { (void)i; (void)id; return n ? S_OK : E_POINTER; }
static HRESULT WINAPI sm_unregister_duck(IAudioSessionManager2 *i, IAudioVolumeDuckNotification *n) { (void)i; return n ? S_OK : E_POINTER; }
static const IAudioSessionManager2Vtbl kSessionManagerVtbl = {sm_query, sm_add_ref, sm_release, sm_get_control, sm_get_volume, sm_get_enumerator,
                                                               sm_register, sm_unregister, sm_register_duck, sm_unregister_duck};
static IAudioSessionManager2 g_session_manager = {(IAudioSessionManager2Vtbl *)&kSessionManagerVtbl};

/* ---- the audio client --------------------------------------------------------------------------------------------------------------------------- */

#define MAX_CHANNELS 32

struct Client {
  Client *next;      /* (first: apply_session_to_all walks the list through it) */
  IAudioClient3 client;
  IAudioRenderClient render;
  IAudioClock clock;
  IAudioStreamVolume stream_volume;
  IChannelAudioVolume channel_volume;
  IAudioCaptureClient capture_client;
  LONG refs;
  CRITICAL_SECTION lock;
  BOOL capture;             /* a client of the capture endpoint, or a loopback client of the render endpoint: it records */
  BOOL loopback;            /* recording what plays (AUDCLNT_STREAMFLAGS_LOOPBACK on the render endpoint) */
  ULONG64 target;           /* a virtual sink's node (its endpoint's client: it plays into the sink, or its loopback records the sink's monitor); 0 the speakers */
  BOOL initialized, started, in_get;
  DWORD flags;
  WAVEFORMATEX format;      /* (the plain part; the graph needs no more) */
  UINT32 frame, buffer_frames, got;
  REFERENCE_TIME period;
  ULONG64 stream;
  BYTE *staging;            /* what GetBuffer hands out */
  BYTE *ring;               /* released, not yet taken by the kernel (capture: recorded, not yet handed out): buffer_frames frames */
  UINT32 ring_head, ring_count;
  ULONG64 written;          /* frames the program released since the last Reset */
  UINT32 packet;            /* capture: the frames of a packet (a period) */
  ULONG64 captured;         /* capture: frames read from the kernel since the last Reset (the device position after the ring) */
  ULONG64 lost_seen;        /* capture: the kernel's lost bytes when last looked at */
  BOOL discontinuity;       /* capture: audio was lost before the next packet */
  ULONG64 base;             /* the kernel's written bytes at the last Reset */
  HANDLE event, thread, wake;
  volatile LONG stop;
  float channels[MAX_CHANNELS];
};

static Client *from_client(IAudioClient3 *i) { return CONTAINING_RECORD(i, Client, client); }
static Client *from_render(IAudioRenderClient *i) { return CONTAINING_RECORD(i, Client, render); }
static Client *from_clock(IAudioClock *i) { return CONTAINING_RECORD(i, Client, clock); }
static Client *from_stream_volume(IAudioStreamVolume *i) { return CONTAINING_RECORD(i, Client, stream_volume); }
static Client *from_channel_volume(IChannelAudioVolume *i) { return CONTAINING_RECORD(i, Client, channel_volume); }
static Client *from_capture_client(IAudioCaptureClient *i) { return CONTAINING_RECORD(i, Client, capture_client); }

static BOOL any_started(void) {
  BOOL any = FALSE;
  AcquireSRWLockShared(&g_lock);
  for (Client *c = g_clients; c; c = c->next) any |= c->started;
  ReleaseSRWLockShared(&g_lock);
  return any;
}

/* The stream's volume in the graph: the session's times the first two channels' (left, right). */
static void apply_volume_with(Client *c, float session, BOOL mute) {
  EnterCriticalSection(&c->lock);
  if (c->capture) {      /* (the session is the process's playback; a microphone stream has its own channels' volume only) */
    session = 1.0f;
    mute = FALSE;
  }
  if (c->stream) {
    const float l = c->channels[0] * session, r = (c->format.nChannels > 1 ? c->channels[1] : c->channels[0]) * session;
    audio_call(TCOS_AUDIO_SET_VOLUME, c->stream, (ULONG64)(l * 65535.0f + 0.5f), (ULONG64)(r * 65535.0f + 0.5f), 0, 0, 0, NULL, NULL, NULL);
    audio_call(TCOS_AUDIO_SET_MUTE, c->stream, mute ? 1 : 0, 0, 0, 0, 0, NULL, NULL, NULL);
  }
  LeaveCriticalSection(&c->lock);
}
static void apply_volume(Client *c) {
  AcquireSRWLockShared(&g_lock);
  const float session = g_session_volume;
  const BOOL mute = g_session_mute;
  ReleaseSRWLockShared(&g_lock);
  apply_volume_with(c, session, mute);
}
static void apply_session_to_all(void) {
  AcquireSRWLockShared(&g_lock);
  for (Client *c = g_clients; c; c = c->next) apply_volume_with(c, g_session_volume, g_session_mute);
  ReleaseSRWLockShared(&g_lock);
}

/* Frames released that the audio engine has not read yet: in the ring here, and in the kernel's stream (as in Windows, what the engine has taken
 * and the device still has to play does not count). */
static UINT32 padding_locked(Client *c) {
  if (!c->stream) return 0;
  ULONG64 waiting = 0;      /* (capture: what the kernel recorded that is not in the ring yet; with the ring, everything waiting to be read) */
  tcos_audio_status(c->stream, &waiting, NULL, NULL, NULL);
  const ULONG64 pad = c->ring_count + waiting / c->frame;
  return pad > c->buffer_frames ? c->buffer_frames : (UINT32)pad;
}
static ULONG64 played_bytes_locked(Client *c) {
  if (!c->stream) return 0;
  if (c->capture) return c->captured * c->frame;      /* (the device position: what was recorded) */
  ULONG64 played = 0;
  tcos_audio_status(c->stream, NULL, NULL, NULL, &played);
  ULONG64 bytes = played > c->base ? played - c->base : 0;
  const ULONG64 most = c->written * c->frame;
  return bytes > most ? most : bytes;
}

/* Gives the kernel what it takes of the ring. */
static void push_locked(Client *c) {
  while (c->ring_count > 0) {
    UINT32 n = c->buffer_frames - c->ring_head;
    if (n > c->ring_count) n = c->ring_count;
    ULONG64 taken = 0;
    if (audio_call(TCOS_AUDIO_WRITE, c->stream, (ULONG64)(ULONG_PTR)(c->ring + (SIZE_T)c->ring_head * c->frame), (ULONG64)n * c->frame, 0, 0, 0, NULL, NULL,
                   &taken) != 0)
      return;
    const UINT32 frames = (UINT32)(taken / c->frame);
    c->ring_head = (c->ring_head + frames) % c->buffer_frames;
    c->ring_count -= frames;
    if (frames < n) return;
  }
}

/* Capture: reads what the kernel recorded into the ring after what is there, and notes audio the kernel lost meanwhile. */
static void pull_locked(Client *c) {
  if (!c->started || !c->stream) return;
  while (c->ring_count < c->buffer_frames) {
    const UINT32 tail = (c->ring_head + c->ring_count) % c->buffer_frames;
    UINT32 n = c->buffer_frames - c->ring_count;
    if (n > c->buffer_frames - tail) n = c->buffer_frames - tail;
    ULONG64 got = 0;
    if (audio_call(TCOS_AUDIO_READ, c->stream, (ULONG64)(ULONG_PTR)(c->ring + (SIZE_T)tail * c->frame), (ULONG64)n * c->frame, 0, 0, 0, NULL, NULL, &got) != 0)
      return;
    const UINT32 frames = (UINT32)(got / c->frame);
    c->ring_count += frames;
    c->captured += frames;
    if (frames < n) break;
  }
  ULONG64 lost = 0;
  tcos_audio_status(c->stream, NULL, &lost, NULL, NULL);
  if (lost > c->lost_seen) {
    c->discontinuity = TRUE;
    c->lost_seen = lost;
  }
}
/* Capture: the frames of the next packet (a whole period while recording; what is left once stopped), 0 when there is none. */
static UINT32 next_packet_locked(Client *c) {
  if (c->ring_count >= c->packet) return c->packet;
  return c->started ? 0 : c->ring_count;
}

static DWORD WINAPI client_thread(LPVOID param) {
  Client *c = (Client *)param;
  DWORD period_ms = (DWORD)(c->period / 10000);
  if (period_ms < 1) period_ms = 1;
  while (!c->stop) {
    WaitForSingleObject(c->wake, period_ms);
    EnterCriticalSection(&c->lock);
    if (c->capture) pull_locked(c);
    else push_locked(c);
    if (c->started && c->event) SetEvent(c->event);
    LeaveCriticalSection(&c->lock);
  }
  return 0;
}

static HRESULT client_query(Client *c, REFIID riid, void **out) {
  if (!out) return E_POINTER;
  *out = NULL;
  if (same_guid(riid, &kIidUnknown) || same_guid(riid, &IID_IAudioClient) || same_guid(riid, &IID_IAudioClient2) || same_guid(riid, &IID_IAudioClient3))
    *out = &c->client;
  else return E_NOINTERFACE;
  InterlockedIncrement(&c->refs);
  return S_OK;
}
static ULONG client_add_ref(Client *c) { return (ULONG)InterlockedIncrement(&c->refs); }
static ULONG client_release(Client *c) {
  const LONG left = InterlockedDecrement(&c->refs);
  if (left != 0) return (ULONG)left;
  AcquireSRWLockExclusive(&g_lock);
  for (Client **p = &g_clients; *p; p = &(*p)->next)
    if (*p == c) {
      *p = c->next;
      break;
    }
  ReleaseSRWLockExclusive(&g_lock);
  if (c->thread) {
    c->stop = 1;
    SetEvent(c->wake);
    WaitForSingleObject(c->thread, INFINITE);
    CloseHandle(c->thread);
  }
  if (c->stream) audio_call(TCOS_AUDIO_CLOSE, c->stream, 0, 0, 0, 0, 0, NULL, NULL, NULL);
  if (c->wake) CloseHandle(c->wake);
  DeleteCriticalSection(&c->lock);
  release_memory(c->staging);
  release_memory(c->ring);
  release_memory(c);
  return 0;
}

static HRESULT WINAPI ac_query(IAudioClient3 *i, REFIID riid, void **out) { return client_query(from_client(i), riid, out); }
static ULONG WINAPI ac_add_ref(IAudioClient3 *i) { return client_add_ref(from_client(i)); }
static ULONG WINAPI ac_release(IAudioClient3 *i) { return client_release(from_client(i)); }

#define VALID_STREAM_FLAGS (AUDCLNT_STREAMFLAGS_CROSSPROCESS | AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_NOPERSIST | \
                            AUDCLNT_STREAMFLAGS_RATEADJUST | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | \
                            AUDCLNT_SESSIONFLAGS_EXPIREWHENUNOWNED | AUDCLNT_SESSIONFLAGS_DISPLAY_HIDE | AUDCLNT_SESSIONFLAGS_DISPLAY_HIDEWHENEXPIRED)

static HRESULT WINAPI ac_initialize(IAudioClient3 *i, AUDCLNT_SHAREMODE mode, DWORD flags, REFERENCE_TIME duration, REFERENCE_TIME periodicity,
                                    const WAVEFORMATEX *format, LPCGUID session) {
  Client *c = from_client(i);
  (void)session;
  if (!format) return E_POINTER;
  if (mode != AUDCLNT_SHAREMODE_SHARED && mode != AUDCLNT_SHAREMODE_EXCLUSIVE) return E_INVALIDARG;
  if (flags & ~(DWORD)VALID_STREAM_FLAGS) return E_INVALIDARG;
  if (duration < 0 || periodicity < 0) return E_INVALIDARG;
  EnterCriticalSection(&c->lock);
  HRESULT hr = S_OK;
  if (c->initialized) hr = AUDCLNT_E_ALREADY_INITIALIZED;
  else if (mode == AUDCLNT_SHAREMODE_EXCLUSIVE) hr = AUDCLNT_E_EXCLUSIVE_MODE_NOT_ALLOWED;
  else if ((flags & AUDCLNT_STREAMFLAGS_LOOPBACK) && c->capture) hr = AUDCLNT_E_WRONG_ENDPOINT_TYPE;      /* (loopback is the render endpoint's) */
  else if (!wave_format_code(format)) hr = AUDCLNT_E_UNSUPPORTED_FORMAT;
  if (FAILED(hr)) {
    LeaveCriticalSection(&c->lock);
    return hr;
  }
  if (duration < MINIMUM_BUFFER) duration = MINIMUM_BUFFER;
  if (duration > 20000000) duration = 20000000;      /* (2 s at most) */
  c->period = DEFAULT_PERIOD;
  c->format = *format;
  c->format.cbSize = 0;
  if (c->format.wFormatTag == WAVE_FORMAT_EXTENSIBLE) c->format.wFormatTag = wave_format_tag(format);
  c->frame = format->nBlockAlign;
  c->buffer_frames = (UINT32)((duration * (ULONG64)format->nSamplesPerSec + 9999999) / 10000000);
  c->staging = (BYTE *)alloc((SIZE_T)c->buffer_frames * c->frame);
  c->ring = (BYTE *)alloc((SIZE_T)c->buffer_frames * c->frame);
  LONG st = 0;
  const BOOL have_memory = c->staging && c->ring;
  if (flags & AUDCLNT_STREAMFLAGS_LOOPBACK) c->capture = c->loopback = TRUE;      /* (a render endpoint's loopback client records what plays: the monitor) */
  if (have_memory) c->stream = tcos_audio_open_to(format, c->capture, c->loopback, c->target, &st);
  c->packet = format->nSamplesPerSec / 100;
  if (c->packet < 1) c->packet = 1;
  if (c->packet > c->buffer_frames) c->packet = c->buffer_frames;
  if (!c->stream) {
    release_memory(c->staging);
    release_memory(c->ring);
    c->staging = c->ring = NULL;
    LeaveCriticalSection(&c->lock);
    return !have_memory ? E_OUTOFMEMORY : st == TCOS_STATUS_INVALID_PARAMETER ? AUDCLNT_E_UNSUPPORTED_FORMAT : AUDCLNT_E_DEVICE_INVALIDATED;
  }
  audio_call(TCOS_AUDIO_PAUSE, c->stream, 1, 0, 0, 0, 0, NULL, NULL, NULL);      /* (until Start) */
  c->flags = flags;
  c->initialized = TRUE;
  c->wake = CreateEventA(NULL, FALSE, FALSE, NULL);
  c->thread = CreateThread(NULL, 0, client_thread, c, 0, NULL);
  LeaveCriticalSection(&c->lock);
  apply_volume(c);
  return S_OK;
}
static HRESULT WINAPI ac_get_buffer_size(IAudioClient3 *i, UINT32 *frames) {
  Client *c = from_client(i);
  if (!frames) return E_POINTER;
  if (!c->initialized) return AUDCLNT_E_NOT_INITIALIZED;
  *frames = c->buffer_frames;
  return S_OK;
}
static HRESULT WINAPI ac_get_stream_latency(IAudioClient3 *i, REFERENCE_TIME *latency) {
  Client *c = from_client(i);
  if (!latency) return E_POINTER;
  if (!c->initialized) return AUDCLNT_E_NOT_INITIALIZED;
  *latency = 2 * DEFAULT_PERIOD;
  return S_OK;
}
static HRESULT WINAPI ac_get_current_padding(IAudioClient3 *i, UINT32 *padding) {
  Client *c = from_client(i);
  if (!padding) return E_POINTER;
  if (!c->initialized) return AUDCLNT_E_NOT_INITIALIZED;
  EnterCriticalSection(&c->lock);
  *padding = padding_locked(c);
  LeaveCriticalSection(&c->lock);
  return S_OK;
}
static HRESULT WINAPI ac_is_format_supported(IAudioClient3 *i, AUDCLNT_SHAREMODE mode, const WAVEFORMATEX *format, WAVEFORMATEX **closest) {
  (void)i;
  if (closest) *closest = NULL;
  if (!format) return E_POINTER;
  if (mode == AUDCLNT_SHAREMODE_EXCLUSIVE) return AUDCLNT_E_UNSUPPORTED_FORMAT;
  if (mode != AUDCLNT_SHAREMODE_SHARED) return E_INVALIDARG;
  if (!closest) return E_POINTER;
  if (wave_format_code(format)) return S_OK;
  if (format->wFormatTag != WAVE_FORMAT_PCM && format->wFormatTag != WAVE_FORMAT_IEEE_FLOAT && format->wFormatTag != WAVE_FORMAT_EXTENSIBLE) return AUDCLNT_E_UNSUPPORTED_FORMAT;
  *closest = task_mix_format();
  return *closest ? S_FALSE : E_OUTOFMEMORY;
}
static HRESULT WINAPI ac_get_mix_format(IAudioClient3 *i, WAVEFORMATEX **format) {
  (void)i;
  if (!format) return E_POINTER;
  *format = task_mix_format();
  return *format ? S_OK : E_OUTOFMEMORY;
}
static HRESULT WINAPI ac_get_device_period(IAudioClient3 *i, REFERENCE_TIME *default_period, REFERENCE_TIME *minimum) {
  (void)i;
  if (!default_period && !minimum) return E_POINTER;
  if (default_period) *default_period = DEFAULT_PERIOD;
  if (minimum) *minimum = MINIMUM_PERIOD;
  return S_OK;
}
static HRESULT WINAPI ac_start(IAudioClient3 *i) {
  Client *c = from_client(i);
  if (!c->initialized) return AUDCLNT_E_NOT_INITIALIZED;
  const BOOL was_active = any_started();
  EnterCriticalSection(&c->lock);
  HRESULT hr = S_OK;
  if (c->started) hr = AUDCLNT_E_NOT_STOPPED;
  else if ((c->flags & AUDCLNT_STREAMFLAGS_EVENTCALLBACK) && !c->event) hr = AUDCLNT_E_EVENTHANDLE_NOT_SET;
  else {
    c->started = TRUE;
    if (!c->capture) push_locked(c);
    audio_call(TCOS_AUDIO_PAUSE, c->stream, 0, 0, 0, 0, 0, NULL, NULL, NULL);      /* (capture: the kernel records from now) */
  }
  LeaveCriticalSection(&c->lock);
  if (SUCCEEDED(hr)) SetEvent(c->wake);
  if (SUCCEEDED(hr) && !was_active) notify_session_state(TRUE);
  return hr;
}
static HRESULT WINAPI ac_stop(IAudioClient3 *i) {
  Client *c = from_client(i);
  if (!c->initialized) return AUDCLNT_E_NOT_INITIALIZED;
  EnterCriticalSection(&c->lock);
  const BOOL was = c->started;
  if (was && c->capture) pull_locked(c);      /* (what was recorded up to now can still be read) */
  if (was) audio_call(TCOS_AUDIO_PAUSE, c->stream, 1, 0, 0, 0, 0, NULL, NULL, NULL);
  c->started = FALSE;
  LeaveCriticalSection(&c->lock);
  if (was && !any_started()) notify_session_state(FALSE);
  return was ? S_OK : S_FALSE;
}
static HRESULT WINAPI ac_reset(IAudioClient3 *i) {
  Client *c = from_client(i);
  if (!c->initialized) return AUDCLNT_E_NOT_INITIALIZED;
  EnterCriticalSection(&c->lock);
  HRESULT hr = S_OK;
  if (c->started) hr = AUDCLNT_E_NOT_STOPPED;
  else if (c->in_get) hr = AUDCLNT_E_BUFFER_OPERATION_PENDING;
  else {
    audio_call(TCOS_AUDIO_FLUSH, c->stream, 0, 0, 0, 0, 0, NULL, NULL, NULL);
    ULONG64 lost = 0, written = 0;
    tcos_audio_status(c->stream, NULL, &lost, &written, NULL);
    c->base = written;
    c->written = 0;
    c->ring_head = c->ring_count = 0;
    c->captured = 0;
    if (c->capture) c->lost_seen = lost;
    c->discontinuity = FALSE;
  }
  LeaveCriticalSection(&c->lock);
  return hr;
}
static HRESULT WINAPI ac_set_event_handle(IAudioClient3 *i, HANDLE event) {
  Client *c = from_client(i);
  if (!event) return E_INVALIDARG;
  if (!c->initialized) return AUDCLNT_E_NOT_INITIALIZED;
  if (!(c->flags & AUDCLNT_STREAMFLAGS_EVENTCALLBACK)) return AUDCLNT_E_EVENTHANDLE_NOT_EXPECTED;
  EnterCriticalSection(&c->lock);
  c->event = event;
  LeaveCriticalSection(&c->lock);
  return S_OK;
}
static HRESULT WINAPI ac_get_service(IAudioClient3 *i, REFIID riid, void **out) {
  Client *c = from_client(i);
  if (!out) return E_POINTER;
  *out = NULL;
  if (!c->initialized) return AUDCLNT_E_NOT_INITIALIZED;
  if (same_guid(riid, &IID_IAudioRenderClient)) {
    if (c->capture) return AUDCLNT_E_WRONG_ENDPOINT_TYPE;
    *out = &c->render;
  } else if (same_guid(riid, &IID_IAudioCaptureClient)) {
    if (!c->capture) return AUDCLNT_E_WRONG_ENDPOINT_TYPE;
    *out = &c->capture_client;
  }
  else if (same_guid(riid, &IID_IAudioClock)) *out = &c->clock;
  else if (same_guid(riid, &IID_IAudioStreamVolume)) *out = &c->stream_volume;
  else if (same_guid(riid, &IID_IChannelAudioVolume)) *out = &c->channel_volume;
  else if (same_guid(riid, &IID_ISimpleAudioVolume)) {
    *out = &g_simple_volume;
    return S_OK;
  } else if (same_guid(riid, &IID_IAudioSessionControl) || same_guid(riid, &IID_IAudioSessionControl2)) {
    *out = &g_session_control;
    return S_OK;
  } else return E_NOINTERFACE;
  InterlockedIncrement(&c->refs);
  return S_OK;
}
static HRESULT WINAPI ac_is_offload_capable(IAudioClient3 *i, AUDIO_STREAM_CATEGORY category, WINBOOL *capable) {
  (void)i; (void)category;
  if (!capable) return E_INVALIDARG;
  *capable = FALSE;
  return S_OK;
}
static HRESULT WINAPI ac_set_client_properties(IAudioClient3 *i, const AudioClientProperties *properties) {
  Client *c = from_client(i);
  if (!properties) return E_POINTER;
  if (properties->cbSize < sizeof(UINT32) * 2 + sizeof(AUDIO_STREAM_CATEGORY)) return E_INVALIDARG;
  if (c->initialized) return AUDCLNT_E_ALREADY_INITIALIZED;
  return properties->bIsOffload ? AUDCLNT_E_ENDPOINT_OFFLOAD_NOT_CAPABLE : S_OK;
}
static HRESULT WINAPI ac_get_buffer_size_limits(IAudioClient3 *i, const WAVEFORMATEX *format, WINBOOL event, REFERENCE_TIME *minimum, REFERENCE_TIME *maximum) {
  (void)i; (void)format; (void)event; (void)minimum; (void)maximum;
  return E_NOTIMPL;      /* (for offloaded streams only) */
}
static HRESULT WINAPI ac_get_shared_mode_engine_period(IAudioClient3 *i, const WAVEFORMATEX *format, UINT32 *default_frames, UINT32 *fundamental, UINT32 *minimum,
                                                       UINT32 *maximum) {
  (void)i;
  if (!format || !default_frames || !fundamental || !minimum || !maximum) return E_POINTER;
  if (!wave_format_code(format)) return AUDCLNT_E_UNSUPPORTED_FORMAT;
  *default_frames = *fundamental = *minimum = *maximum = format->nSamplesPerSec / 100;
  return S_OK;
}
static HRESULT WINAPI ac_get_current_shared_mode_engine_period(IAudioClient3 *i, WAVEFORMATEX **format, UINT32 *frames) {
  (void)i;
  if (!format || !frames) return E_POINTER;
  *format = task_mix_format();
  if (!*format) return E_OUTOFMEMORY;
  *frames = MIX_RATE / 100;
  return S_OK;
}
static HRESULT WINAPI ac_initialize_shared(IAudioClient3 *i, DWORD flags, UINT32 period_frames, const WAVEFORMATEX *format, LPCGUID session) {
  if (!format) return E_POINTER;
  if (period_frames == 0) return E_INVALIDARG;
  const REFERENCE_TIME duration = (REFERENCE_TIME)((ULONG64)period_frames * 10000000 * 2 / (format->nSamplesPerSec ? format->nSamplesPerSec : 1));
  return ac_initialize(i, AUDCLNT_SHAREMODE_SHARED, flags, duration, 0, format, session);
}
static const IAudioClient3Vtbl kClientVtbl = {
  ac_query, ac_add_ref, ac_release, ac_initialize, ac_get_buffer_size, ac_get_stream_latency, ac_get_current_padding, ac_is_format_supported,
  ac_get_mix_format, ac_get_device_period, ac_start, ac_stop, ac_reset, ac_set_event_handle, ac_get_service, ac_is_offload_capable,
  ac_set_client_properties, ac_get_buffer_size_limits, ac_get_shared_mode_engine_period, ac_get_current_shared_mode_engine_period, ac_initialize_shared};

/* the render client */
static HRESULT WINAPI rc_query(IAudioRenderClient *i, REFIID riid, void **out) {
  if (!out) return E_POINTER;
  *out = NULL;
  if (!same_guid(riid, &kIidUnknown) && !same_guid(riid, &IID_IAudioRenderClient)) return E_NOINTERFACE;
  *out = i;
  client_add_ref(from_render(i));
  return S_OK;
}
static ULONG WINAPI rc_add_ref(IAudioRenderClient *i) { return client_add_ref(from_render(i)); }
static ULONG WINAPI rc_release(IAudioRenderClient *i) { return client_release(from_render(i)); }
static HRESULT WINAPI rc_get_buffer(IAudioRenderClient *i, UINT32 frames, BYTE **data) {
  Client *c = from_render(i);
  if (!data) return E_POINTER;
  *data = NULL;
  EnterCriticalSection(&c->lock);
  HRESULT hr = S_OK;
  if (c->in_get) hr = AUDCLNT_E_OUT_OF_ORDER;
  else if (frames > c->buffer_frames - padding_locked(c) || frames > c->buffer_frames - c->ring_count) hr = AUDCLNT_E_BUFFER_TOO_LARGE;
  else {
    c->in_get = TRUE;
    c->got = frames;
    *data = frames ? c->staging : NULL;
  }
  LeaveCriticalSection(&c->lock);
  return hr;
}
static HRESULT WINAPI rc_release_buffer(IAudioRenderClient *i, UINT32 frames, DWORD flags) {
  Client *c = from_render(i);
  EnterCriticalSection(&c->lock);
  HRESULT hr = S_OK;
  if (!c->in_get) hr = frames ? AUDCLNT_E_OUT_OF_ORDER : S_OK;
  else if (frames > c->got) hr = AUDCLNT_E_INVALID_SIZE;
  else if (flags & ~(DWORD)AUDCLNT_BUFFERFLAGS_SILENT) hr = E_INVALIDARG;
  else {
    const BYTE silence = c->format.wFormatTag == WAVE_FORMAT_PCM && c->format.wBitsPerSample == 8 ? 0x80 : 0;
    /* into the ring after what is there (it has room: the program could only ask for what is free) */
    UINT32 at = (c->ring_head + c->ring_count) % c->buffer_frames;
    for (UINT32 f = 0; f < frames; ++f) {
      BYTE *to = c->ring + (SIZE_T)at * c->frame;
      const BYTE *from = c->staging + (SIZE_T)f * c->frame;
      for (UINT32 k = 0; k < c->frame; ++k) to[k] = (flags & AUDCLNT_BUFFERFLAGS_SILENT) ? silence : from[k];
      at = at + 1 == c->buffer_frames ? 0 : at + 1;
    }
    c->ring_count += frames;
    c->written += frames;
    c->in_get = FALSE;
    c->got = 0;
    push_locked(c);
  }
  LeaveCriticalSection(&c->lock);
  return hr;
}
static const IAudioRenderClientVtbl kRenderVtbl = {rc_query, rc_add_ref, rc_release, rc_get_buffer, rc_release_buffer};

/* the capture client */
static HRESULT WINAPI cc_query(IAudioCaptureClient *i, REFIID riid, void **out) {
  if (!out) return E_POINTER;
  *out = NULL;
  if (!same_guid(riid, &kIidUnknown) && !same_guid(riid, &IID_IAudioCaptureClient)) return E_NOINTERFACE;
  *out = i;
  client_add_ref(from_capture_client(i));
  return S_OK;
}
static ULONG WINAPI cc_add_ref(IAudioCaptureClient *i) { return client_add_ref(from_capture_client(i)); }
static ULONG WINAPI cc_release(IAudioCaptureClient *i) { return client_release(from_capture_client(i)); }
static UINT64 qpc_100ns(void) {
  LARGE_INTEGER now, frequency;
  QueryPerformanceCounter(&now);
  QueryPerformanceFrequency(&frequency);
  return frequency.QuadPart ? (UINT64)((double)now.QuadPart * 10000000.0 / (double)frequency.QuadPart) : 0;
}
static HRESULT WINAPI cc_get_buffer(IAudioCaptureClient *i, BYTE **data, UINT32 *frames, DWORD *flags, UINT64 *device_position, UINT64 *qpc_position) {
  Client *c = from_capture_client(i);
  if (!data || !frames || !flags) return E_POINTER;
  *data = NULL;
  *frames = 0;
  *flags = 0;
  EnterCriticalSection(&c->lock);
  HRESULT hr = S_OK;
  if (c->in_get) {
    hr = AUDCLNT_E_OUT_OF_ORDER;
  } else {
    pull_locked(c);
    const UINT32 n = next_packet_locked(c);
    if (!n) {
      hr = AUDCLNT_S_BUFFER_EMPTY;
    } else {
      if (c->ring_head + n <= c->buffer_frames) {
        *data = c->ring + (SIZE_T)c->ring_head * c->frame;
      } else {      /* (around the end of the ring: one piece in the staging buffer) */
        const UINT32 first = c->buffer_frames - c->ring_head;
        copy(c->staging, c->ring + (SIZE_T)c->ring_head * c->frame, (SIZE_T)first * c->frame);
        copy(c->staging + (SIZE_T)first * c->frame, c->ring, (SIZE_T)(n - first) * c->frame);
        *data = c->staging;
      }
      *frames = n;
      *flags = c->discontinuity ? AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY : 0;
      c->discontinuity = FALSE;
      if (device_position) *device_position = c->captured - c->ring_count;
      if (qpc_position) *qpc_position = qpc_100ns();
      c->in_get = TRUE;
      c->got = n;
    }
  }
  LeaveCriticalSection(&c->lock);
  return hr;
}
static HRESULT WINAPI cc_release_buffer(IAudioCaptureClient *i, UINT32 frames) {
  Client *c = from_capture_client(i);
  EnterCriticalSection(&c->lock);
  HRESULT hr = S_OK;
  if (!c->in_get) hr = frames ? AUDCLNT_E_OUT_OF_ORDER : S_OK;
  else if (frames != 0 && frames != c->got) hr = AUDCLNT_E_INVALID_SIZE;      /* (a packet is read whole or not at all) */
  else {
    c->ring_head = (c->ring_head + frames) % c->buffer_frames;
    c->ring_count -= frames;
    c->in_get = FALSE;
    c->got = 0;
  }
  LeaveCriticalSection(&c->lock);
  return hr;
}
static HRESULT WINAPI cc_get_next_packet_size(IAudioCaptureClient *i, UINT32 *frames) {
  Client *c = from_capture_client(i);
  if (!frames) return E_POINTER;
  EnterCriticalSection(&c->lock);
  pull_locked(c);
  *frames = c->in_get ? 0 : next_packet_locked(c);
  LeaveCriticalSection(&c->lock);
  return S_OK;
}
static const IAudioCaptureClientVtbl kCaptureClientVtbl = {cc_query, cc_add_ref, cc_release, cc_get_buffer, cc_release_buffer, cc_get_next_packet_size};

/* the clock: bytes of the client's format */
static HRESULT WINAPI ck_query(IAudioClock *i, REFIID riid, void **out) {
  if (!out) return E_POINTER;
  *out = NULL;
  if (!same_guid(riid, &kIidUnknown) && !same_guid(riid, &IID_IAudioClock)) return E_NOINTERFACE;
  *out = i;
  client_add_ref(from_clock(i));
  return S_OK;
}
static ULONG WINAPI ck_add_ref(IAudioClock *i) { return client_add_ref(from_clock(i)); }
static ULONG WINAPI ck_release(IAudioClock *i) { return client_release(from_clock(i)); }
static HRESULT WINAPI ck_get_frequency(IAudioClock *i, UINT64 *frequency) {
  Client *c = from_clock(i);
  if (!frequency) return E_POINTER;
  *frequency = (UINT64)c->format.nSamplesPerSec * c->frame;
  return S_OK;
}
static HRESULT WINAPI ck_get_position(IAudioClock *i, UINT64 *position, UINT64 *qpc) {
  Client *c = from_clock(i);
  if (!position) return E_POINTER;
  EnterCriticalSection(&c->lock);
  *position = played_bytes_locked(c);
  LeaveCriticalSection(&c->lock);
  if (qpc) {
    LARGE_INTEGER now, frequency;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&frequency);
    *qpc = frequency.QuadPart ? (UINT64)((double)now.QuadPart * 10000000.0 / (double)frequency.QuadPart) : 0;
  }
  return S_OK;
}
static HRESULT WINAPI ck_get_characteristics(IAudioClock *i, DWORD *characteristics) {
  (void)i;
  if (!characteristics) return E_POINTER;
  *characteristics = 0;
  return S_OK;
}
static const IAudioClockVtbl kClockVtbl = {ck_query, ck_add_ref, ck_release, ck_get_frequency, ck_get_position, ck_get_characteristics};

/* per-channel volumes (IAudioStreamVolume, and IChannelAudioVolume with the same values) */
static HRESULT set_channel(Client *c, UINT32 index, float level) {
  if (index >= c->format.nChannels || index >= MAX_CHANNELS || !(level >= 0.0f && level <= 1.0f)) return E_INVALIDARG;
  EnterCriticalSection(&c->lock);
  c->channels[index] = level;
  LeaveCriticalSection(&c->lock);
  apply_volume(c);
  return S_OK;
}
static HRESULT get_channel(Client *c, UINT32 index, float *level) {
  if (!level) return E_POINTER;
  if (index >= c->format.nChannels || index >= MAX_CHANNELS) return E_INVALIDARG;
  *level = c->channels[index];
  return S_OK;
}
static HRESULT set_all_channels(Client *c, UINT32 count, const float *levels) {
  if (!levels) return E_POINTER;
  if (count != c->format.nChannels || count > MAX_CHANNELS) return E_INVALIDARG;
  for (UINT32 k = 0; k < count; ++k) if (!(levels[k] >= 0.0f && levels[k] <= 1.0f)) return E_INVALIDARG;
  EnterCriticalSection(&c->lock);
  for (UINT32 k = 0; k < count; ++k) c->channels[k] = levels[k];
  LeaveCriticalSection(&c->lock);
  apply_volume(c);
  return S_OK;
}
static HRESULT get_all_channels(Client *c, UINT32 count, float *levels) {
  if (!levels) return E_POINTER;
  if (count != c->format.nChannels || count > MAX_CHANNELS) return E_INVALIDARG;
  for (UINT32 k = 0; k < count; ++k) levels[k] = c->channels[k];
  return S_OK;
}
static HRESULT WINAPI asv_query(IAudioStreamVolume *i, REFIID riid, void **out) {
  if (!out) return E_POINTER;
  *out = NULL;
  if (!same_guid(riid, &kIidUnknown) && !same_guid(riid, &IID_IAudioStreamVolume)) return E_NOINTERFACE;
  *out = i;
  client_add_ref(from_stream_volume(i));
  return S_OK;
}
static ULONG WINAPI asv_add_ref(IAudioStreamVolume *i) { return client_add_ref(from_stream_volume(i)); }
static ULONG WINAPI asv_release(IAudioStreamVolume *i) { return client_release(from_stream_volume(i)); }
static HRESULT WINAPI asv_get_count(IAudioStreamVolume *i, UINT32 *count) {
  if (!count) return E_POINTER;
  *count = from_stream_volume(i)->format.nChannels;
  return S_OK;
}
static HRESULT WINAPI asv_set(IAudioStreamVolume *i, UINT32 index, const float level) { return set_channel(from_stream_volume(i), index, level); }
static HRESULT WINAPI asv_get(IAudioStreamVolume *i, UINT32 index, float *level) { return get_channel(from_stream_volume(i), index, level); }
static HRESULT WINAPI asv_set_all(IAudioStreamVolume *i, UINT32 count, const float *levels) { return set_all_channels(from_stream_volume(i), count, levels); }
static HRESULT WINAPI asv_get_all(IAudioStreamVolume *i, UINT32 count, float *levels) { return get_all_channels(from_stream_volume(i), count, levels); }
static const IAudioStreamVolumeVtbl kStreamVolumeVtbl = {asv_query, asv_add_ref, asv_release, asv_get_count, asv_set, asv_get, asv_set_all, asv_get_all};

static HRESULT WINAPI cav_query(IChannelAudioVolume *i, REFIID riid, void **out) {
  if (!out) return E_POINTER;
  *out = NULL;
  if (!same_guid(riid, &kIidUnknown) && !same_guid(riid, &IID_IChannelAudioVolume)) return E_NOINTERFACE;
  *out = i;
  client_add_ref(from_channel_volume(i));
  return S_OK;
}
static ULONG WINAPI cav_add_ref(IChannelAudioVolume *i) { return client_add_ref(from_channel_volume(i)); }
static ULONG WINAPI cav_release(IChannelAudioVolume *i) { return client_release(from_channel_volume(i)); }
static HRESULT WINAPI cav_get_count(IChannelAudioVolume *i, UINT32 *count) {
  if (!count) return E_POINTER;
  *count = from_channel_volume(i)->format.nChannels;
  return S_OK;
}
static HRESULT WINAPI cav_set(IChannelAudioVolume *i, UINT32 index, const float level, LPCGUID context) {
  (void)context;
  return set_channel(from_channel_volume(i), index, level);
}
static HRESULT WINAPI cav_get(IChannelAudioVolume *i, UINT32 index, float *level) { return get_channel(from_channel_volume(i), index, level); }
static HRESULT WINAPI cav_set_all(IChannelAudioVolume *i, UINT32 count, const float *levels, LPCGUID context) {
  (void)context;
  return set_all_channels(from_channel_volume(i), count, levels);
}
static HRESULT WINAPI cav_get_all(IChannelAudioVolume *i, UINT32 count, float *levels) { return get_all_channels(from_channel_volume(i), count, levels); }
static const IChannelAudioVolumeVtbl kChannelVolumeVtbl = {cav_query, cav_add_ref, cav_release, cav_get_count, cav_set, cav_get, cav_set_all, cav_get_all};

static HRESULT new_client(void **out, int which) {
  Client *c = (Client *)alloc(sizeof *c);
  if (!c) return E_OUTOFMEMORY;
  c->capture = which == CAPTURE;
  c->target = virtual_node(which);
  c->capture_client.lpVtbl = (IAudioCaptureClientVtbl *)&kCaptureClientVtbl;
  c->client.lpVtbl = (IAudioClient3Vtbl *)&kClientVtbl;
  c->render.lpVtbl = (IAudioRenderClientVtbl *)&kRenderVtbl;
  c->clock.lpVtbl = (IAudioClockVtbl *)&kClockVtbl;
  c->stream_volume.lpVtbl = (IAudioStreamVolumeVtbl *)&kStreamVolumeVtbl;
  c->channel_volume.lpVtbl = (IChannelAudioVolumeVtbl *)&kChannelVolumeVtbl;
  c->refs = 1;
  for (int k = 0; k < MAX_CHANNELS; ++k) c->channels[k] = 1.0f;
  InitializeCriticalSection(&c->lock);
  AcquireSRWLockExclusive(&g_lock);
  c->next = g_clients;
  g_clients = c;
  ReleaseSRWLockExclusive(&g_lock);
  *out = &c->client;
  return S_OK;
}

/* ---- the endpoints' volume (the graph's master volume, the microphone's master gain) and meter ------------------------------------------------- */

static Listeners g_volume_callbacks[ENDPOINTS] = {[0 ... ENDPOINTS - 1] = {SRWLOCK_INIT, {0}}};
static void start_watcher(void);
/* What the endpoints' master volumes were when last told (the watcher tells the callbacks when someone outside the process changes them). */
static SRWLOCK g_seen_lock = SRWLOCK_INIT;
static float g_seen_scalar[ENDPOINTS] = {[0 ... ENDPOINTS - 1] = -1};
static BOOL g_seen_mute[ENDPOINTS];
static IAudioEndpointVolume g_endpoint_volume[ENDPOINTS];
static int volume_endpoint(IAudioEndpointVolume *i) {
  for (int k = 0; k < ENDPOINTS; ++k) if (i == &g_endpoint_volume[k]) return k;
  return RENDER;
}
/* The stream number NtTcosAudio's volume calls take for an endpoint's master volume (a virtual sink's: its node with TCOS_AUDIO_VIRTUAL_STREAM). */
static ULONG64 master_stream(int which) {
  if (is_virtual(which)) return TCOS_AUDIO_VIRTUAL_STREAM | virtual_node(which);
  return which == CAPTURE ? TCOS_AUDIO_SOURCE_STREAM : 0;
}

/* The master volume as the scalar programs and the volume sliders use: the cube root of the amplitude (as PulseAudio and PipeWire show it). */
static float cube_root(float x) {
  if (!(x > 0)) return 0;
  double r = x > 1 ? x : 1;
  for (int k = 0; k < 40; ++k) r = (2 * r + x / (r * r)) / 3;
  return (float)r;
}
static BOOL read_master(int which, float *scalar, BOOL *mute) {
  ULONG64 v = 0, muted = 0;
  if (audio_call(TCOS_AUDIO_GET_VOLUME, master_stream(which), 0, 0, 0, 0, 0, NULL, &muted, &v) != 0) return FALSE;
  const DWORD left = (DWORD)(v & 0xFFFF), right = (DWORD)(v >> 16 & 0xFFFF);
  if (scalar) *scalar = cube_root((float)(left > right ? left : right) / 65535.0f);
  if (mute) *mute = muted != 0;
  return TRUE;
}
static void notify_volume(int which, LPCGUID context) {
  float scalar = 0;
  BOOL mute = FALSE;
  if (!read_master(which, &scalar, &mute)) return;
  BYTE raw[sizeof(AUDIO_VOLUME_NOTIFICATION_DATA) + sizeof(float)];
  AUDIO_VOLUME_NOTIFICATION_DATA *data = (AUDIO_VOLUME_NOTIFICATION_DATA *)raw;
  zero(raw, sizeof raw);
  if (context) data->guidEventContext = *context;
  data->bMuted = mute;
  data->fMasterVolume = scalar;
  data->nChannels = 2;
  data->afChannelVolumes[0] = scalar;
  ((float *)(data->afChannelVolumes))[1] = scalar;
  AcquireSRWLockExclusive(&g_seen_lock);
  g_seen_scalar[which] = scalar;
  g_seen_mute[which] = mute;
  ReleaseSRWLockExclusive(&g_seen_lock);
  IUnknown *items[MAX_LISTENERS];
  const int n = listeners_copy(&g_volume_callbacks[which], items);
  for (int k = 0; k < n; ++k) IAudioEndpointVolumeCallback_OnNotify((IAudioEndpointVolumeCallback *)items[k], data);
  listeners_drop(items, n);
}
static HRESULT set_master(int which, float scalar, LPCGUID context) {
  if (!(scalar >= 0.0f && scalar <= 1.0f)) return E_INVALIDARG;
  const float amplitude = scalar * scalar * scalar;
  const ULONG64 v = (ULONG64)(amplitude * 65535.0f + 0.5f);
  if (audio_call(TCOS_AUDIO_SET_VOLUME, master_stream(which), v, v, 0, 0, 0, NULL, NULL, NULL) != 0) return AUDCLNT_E_DEVICE_INVALIDATED;
  notify_volume(which, context);
  return S_OK;
}
static float db_to_scalar(float db) { return cube_root((float)tcos_db_to_gain(db)); }
static float scalar_to_db(float scalar) {
  if (!(scalar > 0)) return MIN_DB;
  const float db = (float)tcos_gain_to_db((double)scalar * scalar * scalar);
  return db < MIN_DB ? MIN_DB : db;
}

static HRESULT WINAPI ev_query(IAudioEndpointVolume *i, REFIID riid, void **out) {
  if (!out) return E_POINTER;
  *out = NULL;
  if (!same_guid(riid, &kIidUnknown) && !same_guid(riid, &IID_IAudioEndpointVolume)) return E_NOINTERFACE;
  *out = i;
  return S_OK;
}
static ULONG WINAPI ev_add_ref(IAudioEndpointVolume *i) { (void)i; return 2; }
static ULONG WINAPI ev_release(IAudioEndpointVolume *i) { (void)i; return 1; }
static HRESULT WINAPI ev_register(IAudioEndpointVolume *i, IAudioEndpointVolumeCallback *callback) {
  const int which = volume_endpoint(i);
  const HRESULT hr = listeners_add(&g_volume_callbacks[which], (IUnknown *)callback);
  if (SUCCEEDED(hr)) start_watcher();
  return hr;
}
static HRESULT WINAPI ev_unregister(IAudioEndpointVolume *i, IAudioEndpointVolumeCallback *callback) {
  return listeners_remove(&g_volume_callbacks[volume_endpoint(i)], (IUnknown *)callback);
}
static HRESULT WINAPI ev_get_channel_count(IAudioEndpointVolume *i, UINT *count) {
  (void)i;
  if (!count) return E_POINTER;
  *count = 2;
  return S_OK;
}
static HRESULT WINAPI ev_set_level(IAudioEndpointVolume *i, float db, LPCGUID context) {
  if (!(db >= MIN_DB && db <= 0.0f)) return E_INVALIDARG;
  return set_master(volume_endpoint(i), db_to_scalar(db), context);
}
static HRESULT WINAPI ev_set_scalar(IAudioEndpointVolume *i, float level, LPCGUID context) { return set_master(volume_endpoint(i), level, context); }
static HRESULT WINAPI ev_get_level(IAudioEndpointVolume *i, float *db) {
  if (!db) return E_POINTER;
  float s = 0;
  if (!read_master(volume_endpoint(i), &s, NULL)) return AUDCLNT_E_DEVICE_INVALIDATED;
  *db = scalar_to_db(s);
  return S_OK;
}
static HRESULT WINAPI ev_get_scalar(IAudioEndpointVolume *i, float *level) {
  if (!level) return E_POINTER;
  return read_master(volume_endpoint(i), level, NULL) ? S_OK : AUDCLNT_E_DEVICE_INVALIDATED;
}
/* (the master volumes have no balance: a channel's level is the whole volume) */
static HRESULT WINAPI ev_set_channel_level(IAudioEndpointVolume *i, UINT channel, float db, LPCGUID context) {
  if (channel >= 2) return E_INVALIDARG;
  return ev_set_level(i, db, context);
}
static HRESULT WINAPI ev_set_channel_scalar(IAudioEndpointVolume *i, UINT channel, float level, LPCGUID context) {
  if (channel >= 2) return E_INVALIDARG;
  return set_master(volume_endpoint(i), level, context);
}
static HRESULT WINAPI ev_get_channel_level(IAudioEndpointVolume *i, UINT channel, float *db) {
  if (channel >= 2) return E_INVALIDARG;
  return ev_get_level(i, db);
}
static HRESULT WINAPI ev_get_channel_scalar(IAudioEndpointVolume *i, UINT channel, float *level) {
  if (channel >= 2) return E_INVALIDARG;
  return ev_get_scalar(i, level);
}
static HRESULT WINAPI ev_set_mute(IAudioEndpointVolume *i, WINBOOL mute, LPCGUID context) {
  const int which = volume_endpoint(i);
  BOOL was = FALSE;
  if (!read_master(which, NULL, &was)) return AUDCLNT_E_DEVICE_INVALIDATED;
  if (audio_call(TCOS_AUDIO_SET_MUTE, master_stream(which), mute ? 1 : 0, 0, 0, 0, 0, NULL, NULL, NULL) != 0) return AUDCLNT_E_DEVICE_INVALIDATED;
  if ((was != 0) == (mute != 0)) return S_FALSE;
  notify_volume(which, context);
  return S_OK;
}
static HRESULT WINAPI ev_get_mute(IAudioEndpointVolume *i, WINBOOL *mute) {
  if (!mute) return E_POINTER;
  BOOL m = FALSE;
  if (!read_master(volume_endpoint(i), NULL, &m)) return AUDCLNT_E_DEVICE_INVALIDATED;
  *mute = m;
  return S_OK;
}
#define VOLUME_STEPS 101
static HRESULT WINAPI ev_get_step_info(IAudioEndpointVolume *i, UINT *step, UINT *count) {
  if (!step && !count) return E_POINTER;
  float s = 0;
  if (!read_master(volume_endpoint(i), &s, NULL)) return AUDCLNT_E_DEVICE_INVALIDATED;
  if (step) *step = (UINT)(s * (VOLUME_STEPS - 1) + 0.5f);
  if (count) *count = VOLUME_STEPS;
  return S_OK;
}
static HRESULT step_by(int which, int delta, LPCGUID context) {
  float s = 0;
  if (!read_master(which, &s, NULL)) return AUDCLNT_E_DEVICE_INVALIDATED;
  int step = (int)(s * (VOLUME_STEPS - 1) + 0.5f) + delta;
  if (step < 0) step = 0;
  if (step > VOLUME_STEPS - 1) step = VOLUME_STEPS - 1;
  return set_master(which, (float)step / (VOLUME_STEPS - 1), context);
}
static HRESULT WINAPI ev_step_up(IAudioEndpointVolume *i, LPCGUID context) { return step_by(volume_endpoint(i), 1, context); }
static HRESULT WINAPI ev_step_down(IAudioEndpointVolume *i, LPCGUID context) { return step_by(volume_endpoint(i), -1, context); }
static HRESULT WINAPI ev_query_hardware(IAudioEndpointVolume *i, DWORD *mask) {
  (void)i;
  if (!mask) return E_POINTER;
  *mask = 0;
  return S_OK;
}
static HRESULT WINAPI ev_get_range(IAudioEndpointVolume *i, float *min, float *max, float *step) {
  (void)i;
  if (!min || !max || !step) return E_POINTER;
  *min = MIN_DB;
  *max = 0.0f;
  *step = 0.03125f;
  return S_OK;
}
static const IAudioEndpointVolumeVtbl kEndpointVolumeVtbl = {
  ev_query, ev_add_ref, ev_release, ev_register, ev_unregister, ev_get_channel_count, ev_set_level, ev_set_scalar, ev_get_level, ev_get_scalar,
  ev_set_channel_level, ev_set_channel_scalar, ev_get_channel_level, ev_get_channel_scalar, ev_set_mute, ev_get_mute, ev_get_step_info, ev_step_up,
  ev_step_down, ev_query_hardware, ev_get_range};
static IAudioEndpointVolume g_endpoint_volume[ENDPOINTS] = {[0 ... ENDPOINTS - 1] = {(IAudioEndpointVolumeVtbl *)&kEndpointVolumeVtbl}};

/* (IAudioMeterInformation: mingw's endpointvolume.h only declares it) */
static const GUID kIidMeter = {0xC02216F6, 0x8C67, 0x4B5B, {0x9D, 0x00, 0xD0, 0x08, 0xE7, 0x3E, 0x00, 0x64}};
typedef struct Meter Meter;
typedef struct {
  HRESULT(WINAPI *QueryInterface)(Meter *, REFIID, void **);
  ULONG(WINAPI *AddRef)(Meter *);
  ULONG(WINAPI *Release)(Meter *);
  HRESULT(WINAPI *GetPeakValue)(Meter *, float *);
  HRESULT(WINAPI *GetMeteringChannelCount)(Meter *, UINT *);
  HRESULT(WINAPI *GetChannelsPeakValues)(Meter *, UINT32, float *);
  HRESULT(WINAPI *QueryHardwareSupport)(Meter *, DWORD *);
} MeterVtbl;
struct Meter {
  const MeterVtbl *lpVtbl;
};
static HRESULT WINAPI mi_query(Meter *i, REFIID riid, void **out) {
  if (!out) return E_POINTER;
  *out = NULL;
  if (!same_guid(riid, &kIidUnknown) && !same_guid(riid, &kIidMeter)) return E_NOINTERFACE;
  *out = i;
  return S_OK;
}
static ULONG WINAPI mi_add_ref(Meter *i) { (void)i; return 2; }
static ULONG WINAPI mi_release(Meter *i) { (void)i; return 1; }
static HRESULT WINAPI mi_get_peak(Meter *i, float *peak) {
  (void)i;
  if (!peak) return E_POINTER;
  *peak = 0;
  return S_OK;
}
static HRESULT WINAPI mi_get_count(Meter *i, UINT *count) {
  (void)i;
  if (!count) return E_POINTER;
  *count = 2;
  return S_OK;
}
static HRESULT WINAPI mi_get_peaks(Meter *i, UINT32 count, float *peaks) {
  (void)i;
  if (!peaks) return E_POINTER;
  if (count != 2) return E_INVALIDARG;
  peaks[0] = peaks[1] = 0;
  return S_OK;
}
static HRESULT WINAPI mi_query_hardware(Meter *i, DWORD *mask) {
  (void)i;
  if (!mask) return E_POINTER;
  *mask = 0;
  return S_OK;
}
static const MeterVtbl kMeterVtbl = {mi_query, mi_add_ref, mi_release, mi_get_peak, mi_get_count, mi_get_peaks, mi_query_hardware};
static Meter g_meter = {&kMeterVtbl};

/* ---- the endpoints and their properties --------------------------------------------------------------------------------------------------------- */

static HRESULT activate(int which, REFIID riid, DWORD context, PROPVARIANT *params, void **out) {
  (void)context; (void)params;
  if (!out) return E_POINTER;
  *out = NULL;
  if (!riid) return E_POINTER;
  if (!has_endpoint(which)) return AUDCLNT_E_DEVICE_INVALIDATED;
  if (same_guid(riid, &IID_IAudioClient) || same_guid(riid, &IID_IAudioClient2) || same_guid(riid, &IID_IAudioClient3)) return new_client(out, which);
  if (same_guid(riid, &IID_IAudioEndpointVolume)) { *out = &g_endpoint_volume[which]; return S_OK; }
  if (same_guid(riid, &IID_IAudioSessionManager) || same_guid(riid, &IID_IAudioSessionManager2)) { *out = &g_session_manager; return S_OK; }
  if (same_guid(riid, &kIidMeter)) { *out = &g_meter; return S_OK; }
  typedef HRESULT (WINAPI *CreateFn)(LPCGUID, void **, IUnknown *);
  if (which == RENDER && (same_guid(riid, &kIidDirectSound) || same_guid(riid, &kIidDirectSound8))) {
    /* (dsound.dll's object on this endpoint) */
    HMODULE dsound = LoadLibraryA("dsound.dll");
    CreateFn create = dsound ? (CreateFn)(void *)GetProcAddress(dsound, "DirectSoundCreate8") : NULL;
    return create ? create(&kEndpointGuid, out, NULL) : E_NOINTERFACE;
  }
  if (which == CAPTURE && same_guid(riid, &kIidDirectSoundCapture)) {      /* (IID_IDirectSoundCapture8 is the same) */
    HMODULE dsound = LoadLibraryA("dsound.dll");
    CreateFn create = dsound ? (CreateFn)(void *)GetProcAddress(dsound, "DirectSoundCaptureCreate8") : NULL;
    return create ? create(&kCaptureEndpointGuid, out, NULL) : E_NOINTERFACE;
  }
  return E_NOINTERFACE;
}

static IPropertyStore g_properties[ENDPOINTS];
static int property_endpoint(IPropertyStore *i) {
  for (int k = 0; k < ENDPOINTS; ++k) if (i == &g_properties[k]) return k;
  return RENDER;
}
static HRESULT WINAPI ps_query(IPropertyStore *i, REFIID riid, void **out) {
  if (!out) return E_POINTER;
  *out = NULL;
  if (!same_guid(riid, &kIidUnknown) && !same_guid(riid, &IID_IPropertyStore)) return E_NOINTERFACE;
  *out = i;
  return S_OK;
}
static ULONG WINAPI ps_add_ref(IPropertyStore *i) { (void)i; return 2; }
static ULONG WINAPI ps_release(IPropertyStore *i) { (void)i; return 1; }
static const PROPERTYKEY *const kKeys[] = {&PKEY_Device_FriendlyName, &PKEY_Device_DeviceDesc, &PKEY_DeviceInterface_FriendlyName, &PKEY_AudioEndpoint_FormFactor,
                                           &PKEY_AudioEndpoint_PhysicalSpeakers, &PKEY_AudioEndpoint_GUID, &PKEY_AudioEngine_DeviceFormat};
#define KEY_COUNT (sizeof kKeys / sizeof kKeys[0])
static BOOL same_key(const PROPERTYKEY *a, const PROPERTYKEY *b) { return same_guid(&a->fmtid, &b->fmtid) && a->pid == b->pid; }
static HRESULT WINAPI ps_get_count(IPropertyStore *i, DWORD *count) {
  (void)i;
  if (!count) return E_POINTER;
  *count = KEY_COUNT;
  return S_OK;
}
static HRESULT WINAPI ps_get_at(IPropertyStore *i, DWORD index, PROPERTYKEY *key) {
  (void)i;
  if (!key) return E_POINTER;
  if (index >= KEY_COUNT) return E_INVALIDARG;
  *key = *kKeys[index];
  return S_OK;
}
static HRESULT WINAPI ps_get_value(IPropertyStore *i, REFPROPERTYKEY key, PROPVARIANT *value) {
  if (!key || !value) return E_POINTER;
  const int which = property_endpoint(i);
  const BOOL capture = which == CAPTURE;
  zero(value, sizeof *value);
  const WCHAR *text = NULL;
  WCHAR virtual_text[96];
  if (is_virtual(which)) {
    /* a virtual sink: its description, and its own GUID */
    AcquireSRWLockShared(&g_virtual_lock);
    const VirtualSink *v = &g_virtual[which - FIRST_VIRTUAL];
    const WCHAR *from = same_key(key, &PKEY_AudioEndpoint_GUID) ? v->guid : v->name;
    SIZE_T n = 0;
    while (from[n] && n < 95) { virtual_text[n] = from[n]; ++n; }
    virtual_text[n] = 0;
    ReleaseSRWLockShared(&g_virtual_lock);
    if (same_key(key, &PKEY_Device_FriendlyName) || same_key(key, &PKEY_AudioEndpoint_GUID)) text = virtual_text;
    else if (same_key(key, &PKEY_Device_DeviceDesc)) text = L"Virtual sink";
    else if (same_key(key, &PKEY_DeviceInterface_FriendlyName)) text = L"PipeWire null sink";
  } else if (same_key(key, &PKEY_Device_FriendlyName)) text = capture ? kCaptureFriendlyName : kFriendlyName;
  else if (same_key(key, &PKEY_Device_DeviceDesc)) text = capture ? kCaptureDescription : kDeviceDescription;
  else if (same_key(key, &PKEY_DeviceInterface_FriendlyName)) text = kInterfaceName;
  else if (same_key(key, &PKEY_AudioEndpoint_GUID)) text = capture ? kCaptureGuidString : kEndpointGuidString;
  if (text) {
    value->pwszVal = task_string(text);
    if (!value->pwszVal) return E_OUTOFMEMORY;
    value->vt = VT_LPWSTR;
    return S_OK;
  }
  if (same_key(key, &PKEY_AudioEndpoint_FormFactor)) {
    value->vt = VT_UI4;
    value->ulVal = capture ? Microphone : Speakers;
  } else if (same_key(key, &PKEY_AudioEndpoint_PhysicalSpeakers)) {
    value->vt = VT_UI4;
    value->ulVal = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
  } else if (same_key(key, &PKEY_AudioEngine_DeviceFormat)) {
    WAVEFORMATEXTENSIBLE *f = (WAVEFORMATEXTENSIBLE *)CoTaskMemAlloc(sizeof *f);
    if (!f) return E_OUTOFMEMORY;
    mix_format(f);
    value->vt = VT_BLOB;
    value->blob.cbSize = sizeof *f;
    value->blob.pBlobData = (BYTE *)f;
  }
  return S_OK;      /* (anything else: VT_EMPTY) */
}
static HRESULT WINAPI ps_set_value(IPropertyStore *i, REFPROPERTYKEY key, REFPROPVARIANT value) { (void)i; (void)key; (void)value; return STG_E_ACCESSDENIED; }
static HRESULT WINAPI ps_commit(IPropertyStore *i) { (void)i; return S_OK; }
static const IPropertyStoreVtbl kPropertyStoreVtbl = {ps_query, ps_add_ref, ps_release, ps_get_count, ps_get_at, ps_get_value, ps_set_value, ps_commit};
static IPropertyStore g_properties[ENDPOINTS] = {[0 ... ENDPOINTS - 1] = {(IPropertyStoreVtbl *)&kPropertyStoreVtbl}};

typedef struct {
  IMMDevice device;
  IMMEndpoint endpoint;
} Endpoint;
static Endpoint g_endpoints[ENDPOINTS];
static int device_endpoint(IMMDevice *i) {
  for (int k = 0; k < ENDPOINTS; ++k) if (i == &g_endpoints[k].device) return k;
  return RENDER;
}
/* (a virtual sink's id is copied while its slot is looked at: the caller's buffer of 72) */
static const WCHAR *endpoint_id_into(int which, WCHAR *buffer) {
  if (!is_virtual(which)) return which == CAPTURE ? kCaptureEndpointId : kEndpointId;
  AcquireSRWLockShared(&g_virtual_lock);
  copy(buffer, g_virtual[which - FIRST_VIRTUAL].id, sizeof g_virtual[0].id);
  ReleaseSRWLockShared(&g_virtual_lock);
  return buffer;
}

static HRESULT WINAPI dv_query(IMMDevice *i, REFIID riid, void **out) {
  if (!out) return E_POINTER;
  *out = NULL;
  Endpoint *e = &g_endpoints[device_endpoint(i)];
  if (same_guid(riid, &kIidUnknown) || same_guid(riid, &IID_IMMDevice)) *out = &e->device;
  else if (same_guid(riid, &IID_IMMEndpoint)) *out = &e->endpoint;
  else return E_NOINTERFACE;
  return S_OK;
}
static ULONG WINAPI dv_add_ref(IMMDevice *i) { (void)i; return 2; }
static ULONG WINAPI dv_release(IMMDevice *i) { (void)i; return 1; }
static HRESULT WINAPI dv_activate(IMMDevice *i, REFIID riid, DWORD context, PROPVARIANT *params, void **out) {
  return activate(device_endpoint(i), riid, context, params, out);
}
static HRESULT WINAPI dv_open_property_store(IMMDevice *i, DWORD access, IPropertyStore **out) {
  if (!out) return E_POINTER;
  *out = NULL;
  if (access != STGM_READ && access != STGM_WRITE && access != STGM_READWRITE) return E_INVALIDARG;
  *out = &g_properties[device_endpoint(i)];
  return S_OK;
}
static HRESULT WINAPI dv_get_id(IMMDevice *i, LPWSTR *id) {
  if (!id) return E_POINTER;
  WCHAR buffer[72];
  *id = task_string(endpoint_id_into(device_endpoint(i), buffer));
  return *id ? S_OK : E_OUTOFMEMORY;
}
static HRESULT WINAPI dv_get_state(IMMDevice *i, DWORD *state) {
  if (!state) return E_POINTER;
  *state = has_endpoint(device_endpoint(i)) ? DEVICE_STATE_ACTIVE : DEVICE_STATE_UNPLUGGED;
  return S_OK;
}
static const IMMDeviceVtbl kDeviceVtbl = {dv_query, dv_add_ref, dv_release, dv_activate, dv_open_property_store, dv_get_id, dv_get_state};

static int endpoint_of(IMMEndpoint *i) {
  for (int k = 0; k < ENDPOINTS; ++k) if (i == &g_endpoints[k].endpoint) return k;
  return RENDER;
}
static HRESULT WINAPI ep_query(IMMEndpoint *i, REFIID riid, void **out) { return dv_query(&g_endpoints[endpoint_of(i)].device, riid, out); }
static ULONG WINAPI ep_add_ref(IMMEndpoint *i) { (void)i; return 2; }
static ULONG WINAPI ep_release(IMMEndpoint *i) { (void)i; return 1; }
static HRESULT WINAPI ep_get_data_flow(IMMEndpoint *i, EDataFlow *flow) {
  if (!flow) return E_POINTER;
  *flow = endpoint_of(i) == CAPTURE ? eCapture : eRender;
  return S_OK;
}
static const IMMEndpointVtbl kEndpointVtbl = {ep_query, ep_add_ref, ep_release, ep_get_data_flow};
static Endpoint g_endpoints[ENDPOINTS] = {[0 ... ENDPOINTS - 1] = {{(IMMDeviceVtbl *)&kDeviceVtbl}, {(IMMEndpointVtbl *)&kEndpointVtbl}}};

/* ---- the enumerator ----------------------------------------------------------------------------------------------------------------------------- */

typedef struct {
  IMMDeviceCollection iface;
  LONG refs;
  UINT count;
  int items[ENDPOINTS];
} Collection;

static HRESULT WINAPI co_query(IMMDeviceCollection *i, REFIID riid, void **out) {
  if (!out) return E_POINTER;
  *out = NULL;
  if (!same_guid(riid, &kIidUnknown) && !same_guid(riid, &IID_IMMDeviceCollection)) return E_NOINTERFACE;
  *out = i;
  InterlockedIncrement(&((Collection *)i)->refs);
  return S_OK;
}
static ULONG WINAPI co_add_ref(IMMDeviceCollection *i) { return (ULONG)InterlockedIncrement(&((Collection *)i)->refs); }
static ULONG WINAPI co_release(IMMDeviceCollection *i) {
  const LONG left = InterlockedDecrement(&((Collection *)i)->refs);
  if (left == 0) release_memory(i);
  return (ULONG)left;
}
static HRESULT WINAPI co_get_count(IMMDeviceCollection *i, UINT *count) {
  if (!count) return E_POINTER;
  *count = ((Collection *)i)->count;
  return S_OK;
}
static HRESULT WINAPI co_item(IMMDeviceCollection *i, UINT index, IMMDevice **out) {
  if (!out) return E_POINTER;
  *out = NULL;
  Collection *c = (Collection *)i;
  if (index >= c->count) return E_INVALIDARG;
  *out = &g_endpoints[c->items[index]].device;
  return S_OK;
}
static const IMMDeviceCollectionVtbl kCollectionVtbl = {co_query, co_add_ref, co_release, co_get_count, co_item};

static HRESULT WINAPI en_query(IMMDeviceEnumerator *i, REFIID riid, void **out) {
  if (!out) return E_POINTER;
  *out = NULL;
  if (!same_guid(riid, &kIidUnknown) && !same_guid(riid, &IID_IMMDeviceEnumerator)) return E_NOINTERFACE;
  *out = i;
  return S_OK;
}
static ULONG WINAPI en_add_ref(IMMDeviceEnumerator *i) { (void)i; return 2; }
static ULONG WINAPI en_release(IMMDeviceEnumerator *i) { (void)i; return 1; }
static HRESULT WINAPI en_enum(IMMDeviceEnumerator *i, EDataFlow flow, DWORD mask, IMMDeviceCollection **out) {
  (void)i;
  if (!out) return E_POINTER;
  *out = NULL;
  if (flow != eRender && flow != eCapture && flow != eAll) return E_INVALIDARG;
  if (mask & ~(DWORD)DEVICE_STATEMASK_ALL) return E_INVALIDARG;
  Collection *c = (Collection *)alloc(sizeof *c);
  if (!c) return E_OUTOFMEMORY;
  c->iface.lpVtbl = (IMMDeviceCollectionVtbl *)&kCollectionVtbl;
  c->refs = 1;
  refresh_virtual();
  /* the speakers, the virtual sinks (render endpoints too), the microphone */
  static const int kOrder[] = {RENDER, -1, CAPTURE};
  for (int o = 0; o < 3; ++o) {
    const int first = kOrder[o] >= 0 ? kOrder[o] : FIRST_VIRTUAL, last = kOrder[o] >= 0 ? kOrder[o] : ENDPOINTS - 1;
    for (int which = first; which <= last; ++which) {
      if (flow != eAll && flow != (which == CAPTURE ? eCapture : eRender)) continue;
      if (is_virtual(which) && !virtual_node(which)) continue;      /* (an empty slot) */
      const DWORD state = has_endpoint(which) ? DEVICE_STATE_ACTIVE : DEVICE_STATE_UNPLUGGED;
      if ((mask & state) && c->count < ENDPOINTS) c->items[c->count++] = which;
    }
  }
  *out = &c->iface;
  return S_OK;
}
static HRESULT WINAPI en_get_default(IMMDeviceEnumerator *i, EDataFlow flow, ERole role, IMMDevice **out) {
  (void)i;
  if (!out) return E_POINTER;
  *out = NULL;
  if ((flow != eRender && flow != eCapture) || role < eConsole || role >= ERole_enum_count) return E_INVALIDARG;
  const int which = flow == eCapture ? CAPTURE : RENDER;
  if (!has_endpoint(which)) return E_NOTFOUND_;
  *out = &g_endpoints[which].device;
  return S_OK;
}
static HRESULT WINAPI en_get_device(IMMDeviceEnumerator *i, LPCWSTR id, IMMDevice **out) {
  (void)i;
  if (!out || !id) return E_POINTER;
  *out = NULL;
  refresh_virtual();
  for (int which = RENDER; which < ENDPOINTS; ++which) {
    if (is_virtual(which) && !virtual_node(which)) continue;
    WCHAR buffer[72];
    if (wide_equal_nocase(id, endpoint_id_into(which, buffer))) {
      *out = &g_endpoints[which].device;
      return S_OK;
    }
  }
  return E_NOTFOUND_;
}
/* ---- the watcher: what changes from outside the process ----------------------------------------------------------------------------------------- *
 * While the program listens (endpoint volume callbacks, notification clients), a thread looks every 100 ms at the endpoints' master volumes (pactl,
 * wpctl or the desktop may change them) and at whether the microphone and the speakers are there (a USB headset plugged in or pulled out), and tells
 * the callbacks what changed. */
static Listeners g_notification_clients = {SRWLOCK_INIT, {0}};
static volatile LONG g_watcher_started;
static BOOL g_seen_present[ENDPOINTS];

static void notify_device_change(int which, BOOL present) {
  IUnknown *items[MAX_LISTENERS];
  const int n = listeners_copy(&g_notification_clients, items);
  WCHAR buffer[72];
  const WCHAR *id = endpoint_id_into(which, buffer);
  const EDataFlow flow = which == CAPTURE ? eCapture : eRender;
  for (int k = 0; k < n; ++k) {
    IMMNotificationClient *c = (IMMNotificationClient *)items[k];
    IMMNotificationClient_OnDeviceStateChanged(c, id, present ? DEVICE_STATE_ACTIVE : DEVICE_STATE_UNPLUGGED);
    for (int role = eConsole; role < ERole_enum_count; ++role) IMMNotificationClient_OnDefaultDeviceChanged(c, flow, (ERole)role, present ? id : NULL);
  }
  listeners_drop(items, n);
}

static DWORD WINAPI watcher_thread(LPVOID param) {
  (void)param;
  for (int which = RENDER; which <= CAPTURE; ++which) g_seen_present[which] = has_endpoint(which);
  for (;;) {
    Sleep(100);
    for (int which = RENDER; which <= CAPTURE; ++which) {
      const BOOL present = has_endpoint(which);
      if (present != g_seen_present[which]) {
        g_seen_present[which] = present;
        notify_device_change(which, present);
      }
      float scalar = 0;
      BOOL mute = FALSE;
      if (!present || !read_master(which, &scalar, &mute)) continue;
      AcquireSRWLockShared(&g_seen_lock);
      const BOOL changed = g_seen_scalar[which] >= 0 && (scalar != g_seen_scalar[which] || mute != g_seen_mute[which]);
      const BOOL first = g_seen_scalar[which] < 0;
      ReleaseSRWLockShared(&g_seen_lock);
      if (changed) {
        notify_volume(which, NULL);
      } else if (first) {
        AcquireSRWLockExclusive(&g_seen_lock);
        g_seen_scalar[which] = scalar;
        g_seen_mute[which] = mute;
        ReleaseSRWLockExclusive(&g_seen_lock);
      }
    }
  }
  return 0;
}

static void start_watcher(void) {
  if (InterlockedCompareExchange(&g_watcher_started, 1, 0) != 0) return;
  HANDLE thread = CreateThread(NULL, 0, watcher_thread, NULL, 0, NULL);
  if (thread) CloseHandle(thread);
  else InterlockedExchange(&g_watcher_started, 0);
}

static HRESULT WINAPI en_register(IMMDeviceEnumerator *i, IMMNotificationClient *client) {
  (void)i;
  const HRESULT hr = listeners_add(&g_notification_clients, (IUnknown *)client);
  if (SUCCEEDED(hr)) start_watcher();
  return hr;
}
static HRESULT WINAPI en_unregister(IMMDeviceEnumerator *i, IMMNotificationClient *client) { (void)i; return listeners_remove(&g_notification_clients, (IUnknown *)client); }
static const IMMDeviceEnumeratorVtbl kEnumeratorVtbl = {en_query, en_add_ref, en_release, en_enum, en_get_default, en_get_device, en_register, en_unregister};
static IMMDeviceEnumerator g_enumerator = {(IMMDeviceEnumeratorVtbl *)&kEnumeratorVtbl};

/* ---- ActivateAudioInterfaceAsync ---------------------------------------------------------------------------------------------------------------- */

typedef struct {
  IActivateAudioInterfaceAsyncOperation iface;
  LONG refs;
  HRESULT result;
  IUnknown *object;
  IActivateAudioInterfaceCompletionHandler *handler;
} Operation;

static HRESULT WINAPI op_query(IActivateAudioInterfaceAsyncOperation *i, REFIID riid, void **out) {
  if (!out) return E_POINTER;
  *out = NULL;
  if (!same_guid(riid, &kIidUnknown) && !same_guid(riid, &IID_IActivateAudioInterfaceAsyncOperation) && !same_guid(riid, &kIidAgileObject)) return E_NOINTERFACE;
  *out = i;
  InterlockedIncrement(&((Operation *)i)->refs);
  return S_OK;
}
static ULONG WINAPI op_add_ref(IActivateAudioInterfaceAsyncOperation *i) { return (ULONG)InterlockedIncrement(&((Operation *)i)->refs); }
static ULONG WINAPI op_release(IActivateAudioInterfaceAsyncOperation *i) {
  Operation *o = (Operation *)i;
  const LONG left = InterlockedDecrement(&o->refs);
  if (left == 0) {
    if (o->object) IUnknown_Release(o->object);
    release_memory(o);
  }
  return (ULONG)left;
}
static HRESULT WINAPI op_get_result(IActivateAudioInterfaceAsyncOperation *i, HRESULT *result, IUnknown **object) {
  Operation *o = (Operation *)i;
  if (!result || !object) return E_POINTER;
  *result = o->result;
  *object = o->object;
  if (o->object) IUnknown_AddRef(o->object);
  return S_OK;
}
static const IActivateAudioInterfaceAsyncOperationVtbl kOperationVtbl = {op_query, op_add_ref, op_release, op_get_result};

static DWORD WINAPI complete_activation(LPVOID param) {
  Operation *o = (Operation *)param;
  IActivateAudioInterfaceCompletionHandler_ActivateCompleted(o->handler, &o->iface);
  IActivateAudioInterfaceCompletionHandler_Release(o->handler);
  op_release(&o->iface);
  return 0;
}

EXPORT HRESULT WINAPI ActivateAudioInterfaceAsync(LPCWSTR device, REFIID iid, PROPVARIANT *parameters, IActivateAudioInterfaceCompletionHandler *handler,
                                                  IActivateAudioInterfaceAsyncOperation **operation) {
  if (operation) *operation = NULL;
  if (!device || !iid || !handler || !operation) return E_INVALIDARG;
  Operation *o = (Operation *)alloc(sizeof *o);
  if (!o) return E_OUTOFMEMORY;
  o->iface.lpVtbl = (IActivateAudioInterfaceAsyncOperationVtbl *)&kOperationVtbl;
  o->refs = 2;      /* the caller's and the completion's */
  int which = wide_equal_nocase(device, kRenderInterface) || wide_equal_nocase(device, kEndpointId) ? RENDER
              : wide_equal_nocase(device, kCaptureInterface) || wide_equal_nocase(device, kCaptureEndpointId) ? CAPTURE : -1;
  if (which < 0) {      /* (a virtual sink's endpoint id) */
    refresh_virtual();
    for (int k = FIRST_VIRTUAL; k < ENDPOINTS && which < 0; ++k) {
      WCHAR buffer[72];
      if (virtual_node(k) && wide_equal_nocase(device, endpoint_id_into(k, buffer))) which = k;
    }
  }
  if (which >= 0) {
    void *object = NULL;
    o->result = activate(which, iid, CLSCTX_ALL, parameters, &object);
    o->object = (IUnknown *)object;
  } else {
    o->result = E_NOTFOUND_;      /* (process loopback, an endpoint that is not here) */
  }
  o->handler = handler;
  IActivateAudioInterfaceCompletionHandler_AddRef(handler);
  HANDLE thread = CreateThread(NULL, 0, complete_activation, o, 0, NULL);
  if (!thread) complete_activation(o);
  else CloseHandle(thread);
  *operation = &o->iface;
  return S_OK;
}

/* ---- the class factory -------------------------------------------------------------------------------------------------------------------------- */

static HRESULT WINAPI f_query(IClassFactory *i, REFIID riid, void **out) {
  if (!out) return E_POINTER;
  *out = NULL;
  if (!same_guid(riid, &kIidUnknown) && !same_guid(riid, &kIidClassFactory)) return E_NOINTERFACE;
  *out = i;
  return S_OK;
}
static ULONG WINAPI f_add_ref(IClassFactory *i) { (void)i; return 2; }
static ULONG WINAPI f_release(IClassFactory *i) { (void)i; return 1; }
static HRESULT WINAPI f_create_instance(IClassFactory *i, IUnknown *outer, REFIID riid, void **out) {
  (void)i;
  if (!out) return E_POINTER;
  *out = NULL;
  if (outer) return CLASS_E_NOAGGREGATION;
  return en_query(&g_enumerator, riid, out);
}
static HRESULT WINAPI f_lock_server(IClassFactory *i, BOOL lock) { (void)i; (void)lock; return S_OK; }
static const IClassFactoryVtbl kFactoryVtbl = {f_query, f_add_ref, f_release, f_create_instance, f_lock_server};
static IClassFactory g_factory = {(IClassFactoryVtbl *)&kFactoryVtbl};

EXPORT HRESULT WINAPI DllGetClassObject(REFCLSID clsid, REFIID riid, LPVOID *out) {
  if (!out) return E_POINTER;
  *out = NULL;
  if (!same_guid(clsid, &CLSID_MMDeviceEnumerator)) return CLASS_E_CLASSNOTAVAILABLE;
  return f_query(&g_factory, riid, out);
}
EXPORT HRESULT WINAPI DllCanUnloadNow(void) { return S_FALSE; }

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved) {
  (void)instance;
  (void)reason;
  (void)reserved;
  return TRUE;
}
