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

/* DirectSound capture over the kernel's PipeWire graph: every IDirectSoundCaptureBuffer8 is a capture stream of the graph (NtTcosAudio's OPEN_CAPTURE and
 * READ), hearing the microphone with every other program that records. Included by dsound.c.
 *
 * A buffer is a ring in the program's format. While it captures (Start until Stop, or until the end without DSCBSTART_LOOPING) a thread reads what the
 * kernel has into the ring every 5 ms at the capture cursor and signals the position notifications it passes (DSBPN_OFFSETSTOP on Stop). The capture
 * and read cursors are the same: everything before them is there to Lock. The devices are the graph's source (the microphone) and its virtual
 * sources (module-remap-source, -virtual-source, echo-cancel-source: a GUID each, see tcos_audio_virtual_source_guid), several buffers at once
 * (DSCCAPS_MULTIPLECAPTURE); capture effects (DSCBCAPS_CTRLFX) are not there. IDirectSoundFullDuplex opens a capture buffer and a playback buffer
 * together. */

/* The one input device: the graph's source. */
static const GUID kCaptureGuid = {0x5AF3A3C3, 0x6E1B, 0x4B4E, {0x8D, 0x2F, 0x1C, 0x0A, 0x7E, 0x5D, 0x9B, 0x31}};

static BOOL capture_guid(const GUID *g) {
  return !g || same_guid(g, &kGuidNull) || same_guid(g, &DSDEVID_DefaultCapture) || same_guid(g, &DSDEVID_DefaultVoiceCapture) || same_guid(g, &kCaptureGuid);
}

typedef struct CaptureDevice {
  IDirectSoundCapture iface;
  LONG refs;
  BOOL initialized;
  ULONG64 node;      /* (a virtual source's node; 0: the microphone) */
} CaptureDevice;

typedef struct CaptureBuffer {
  IDirectSoundCaptureBuffer8 buffer;      /* (first: the object's pointer is this interface) */
  IDirectSoundNotify notify;
  LONG refs;
  CaptureDevice *device;                  /* (held: the buffer keeps its device) */
  DWORD flags;
  TcosWaveFormatExtensible format;
  DWORD size;                             /* bytes, whole frames */
  BYTE *data;
  ULONG64 stream;
  CRITICAL_SECTION lock;
  /* (the rest under the lock) */
  BOOL capturing, looping;
  DWORD position;                         /* the capture cursor (bytes) */
  DSBPOSITIONNOTIFY *notifies;
  DWORD notify_count;
  HANDLE thread, wake;
  volatile LONG stop;
} CaptureBuffer;

static const IDirectSoundCaptureVtbl kCaptureVtbl;
static const IDirectSoundCaptureBuffer8Vtbl kCaptureBufferVtbl;
static const IDirectSoundNotifyVtbl kCaptureNotifyVtbl;
static CaptureDevice *from_capture(void *i) { return (CaptureDevice *)i; }
static CaptureBuffer *from_capture_buffer(void *i) { return (CaptureBuffer *)i; }
static CaptureBuffer *from_capture_notify(IDirectSoundNotify *i) { return CONTAINING_RECORD(i, CaptureBuffer, notify); }

/* Signals the notifications between `from` and `to` (bytes, `to` exclusive, around the end of the ring when to < from). The lock held. */
static void capture_notify_locked(CaptureBuffer *b, DWORD from, DWORD to, BOOL wrapped) {
  for (DWORD k = 0; k < b->notify_count; ++k) {
    const DWORD at = b->notifies[k].dwOffset;
    if (at == (DWORD)DSBPN_OFFSETSTOP) continue;
    const BOOL passed = wrapped ? (at >= from || at < to) : (at >= from && at < to);
    if (passed && b->notifies[k].hEventNotify) SetEvent(b->notifies[k].hEventNotify);
  }
}

static void capture_notify_stop_locked(CaptureBuffer *b) {
  for (DWORD k = 0; k < b->notify_count; ++k)
    if (b->notifies[k].dwOffset == (DWORD)DSBPN_OFFSETSTOP && b->notifies[k].hEventNotify) SetEvent(b->notifies[k].hEventNotify);
}

/* Reads what the kernel has into the ring at the capture cursor. The lock held. */
static void capture_step_locked(CaptureBuffer *b) {
  while (b->capturing) {
    const DWORD start = b->position, room = b->size - start;
    ULONG64 got = 0;
    if (audio_call(TCOS_AUDIO_READ, b->stream, (ULONG64)(ULONG_PTR)(b->data + start), room, 0, 0, 0, NULL, NULL, &got) != 0 || got == 0) return;
    DWORD end = start + (DWORD)got;
    if (end >= b->size) {
      end = 0;
      capture_notify_locked(b, start, end, TRUE);
      if (!b->looping) {      /* a full buffer that does not loop stops */
        b->position = 0;
        b->capturing = FALSE;
        audio_call(TCOS_AUDIO_PAUSE, b->stream, 1, 0, 0, 0, 0, NULL, NULL, NULL);
        capture_notify_stop_locked(b);
        return;
      }
    } else {
      capture_notify_locked(b, start, end, FALSE);
    }
    b->position = end;
    if (got < room) return;
  }
}

static DWORD WINAPI capture_thread(LPVOID context) {
  CaptureBuffer *b = (CaptureBuffer *)context;
  while (!b->stop) {
    EnterCriticalSection(&b->lock);
    capture_step_locked(b);
    LeaveCriticalSection(&b->lock);
    WaitForSingleObject(b->wake, TICK_MS);
  }
  return 0;
}

/* ---- IDirectSoundCaptureBuffer8 ----------------------------------------------------------------------------------------------------------------- */

static HRESULT WINAPI cb_query(IDirectSoundCaptureBuffer8 *i, REFIID riid, void **out) {
  CaptureBuffer *b = from_capture_buffer(i);
  if (!out) return E_POINTER;
  *out = NULL;
  if (!riid) return E_POINTER;
  if (same_guid(riid, &kIidUnknown) || same_guid(riid, &IID_IDirectSoundCaptureBuffer) || same_guid(riid, &IID_IDirectSoundCaptureBuffer8)) *out = &b->buffer;
  else if (same_guid(riid, &IID_IDirectSoundNotify)) *out = &b->notify;
  else return E_NOINTERFACE;
  InterlockedIncrement(&b->refs);
  return S_OK;
}
static ULONG WINAPI cb_add_ref(IDirectSoundCaptureBuffer8 *i) { return (ULONG)InterlockedIncrement(&from_capture_buffer(i)->refs); }
static ULONG WINAPI c_release(IDirectSoundCapture *i);
static ULONG WINAPI cb_release(IDirectSoundCaptureBuffer8 *i) {
  CaptureBuffer *b = from_capture_buffer(i);
  const LONG left = InterlockedDecrement(&b->refs);
  if (left != 0) return (ULONG)left;
  if (b->thread) {
    b->stop = 1;
    SetEvent(b->wake);
    WaitForSingleObject(b->thread, INFINITE);
    CloseHandle(b->thread);
  }
  if (b->stream) audio_call(TCOS_AUDIO_CLOSE, b->stream, 0, 0, 0, 0, 0, NULL, NULL, NULL);
  if (b->wake) CloseHandle(b->wake);
  DeleteCriticalSection(&b->lock);
  CaptureDevice *d = b->device;
  release_memory(b->notifies);
  release_memory(b->data);
  release_memory(b);
  if (d) c_release(&d->iface);
  return 0;
}
static HRESULT WINAPI cb_get_caps(IDirectSoundCaptureBuffer8 *i, LPDSCBCAPS caps) {
  CaptureBuffer *b = from_capture_buffer(i);
  if (!caps || caps->dwSize < sizeof *caps) return DSERR_INVALIDPARAM;
  caps->dwFlags = b->flags;
  caps->dwBufferBytes = b->size;
  caps->dwReserved = 0;
  return DS_OK;
}
static HRESULT WINAPI cb_get_current_position(IDirectSoundCaptureBuffer8 *i, LPDWORD capture, LPDWORD read) {
  CaptureBuffer *b = from_capture_buffer(i);
  if (!capture && !read) return DSERR_INVALIDPARAM;
  EnterCriticalSection(&b->lock);
  capture_step_locked(b);      /* (as up to date as the kernel is) */
  const DWORD at = b->position;
  LeaveCriticalSection(&b->lock);
  if (capture) *capture = at;
  if (read) *read = at;
  return DS_OK;
}
static HRESULT WINAPI cb_get_format(IDirectSoundCaptureBuffer8 *i, LPWAVEFORMATEX format, DWORD allocated, LPDWORD written) {
  CaptureBuffer *b = from_capture_buffer(i);
  const DWORD need = (DWORD)sizeof(WAVEFORMATEX) + b->format.Format.cbSize;
  if (!format) {
    if (!written) return DSERR_INVALIDPARAM;
    *written = need;
    return DS_OK;
  }
  if (allocated < need) return DSERR_INVALIDPARAM;
  copy(format, &b->format, need);
  if (written) *written = need;
  return DS_OK;
}
static HRESULT WINAPI cb_get_status(IDirectSoundCaptureBuffer8 *i, LPDWORD status) {
  CaptureBuffer *b = from_capture_buffer(i);
  if (!status) return DSERR_INVALIDPARAM;
  EnterCriticalSection(&b->lock);
  capture_step_locked(b);
  *status = (b->capturing ? DSCBSTATUS_CAPTURING : 0) | (b->capturing && b->looping ? DSCBSTATUS_LOOPING : 0);
  LeaveCriticalSection(&b->lock);
  return DS_OK;
}
static HRESULT WINAPI cb_initialize(IDirectSoundCaptureBuffer8 *i, LPDIRECTSOUNDCAPTURE device, LPCDSCBUFFERDESC desc) {
  (void)i; (void)device; (void)desc;
  return DSERR_ALREADYINITIALIZED;
}
static HRESULT WINAPI cb_lock(IDirectSoundCaptureBuffer8 *i, DWORD offset, DWORD bytes, LPVOID *p1, LPDWORD b1, LPVOID *p2, LPDWORD b2, DWORD flags) {
  CaptureBuffer *b = from_capture_buffer(i);
  if (!p1 || !b1) return DSERR_INVALIDPARAM;
  *p1 = NULL;
  *b1 = 0;
  if (p2) *p2 = NULL;
  if (b2) *b2 = 0;
  if (flags & DSCBLOCK_ENTIREBUFFER) bytes = b->size;
  if (offset >= b->size || bytes == 0 || bytes > b->size) return DSERR_INVALIDPARAM;
  const DWORD first = b->size - offset < bytes ? b->size - offset : bytes;
  *p1 = b->data + offset;
  *b1 = first;
  if (first < bytes) {
    if (!p2 || !b2) return DSERR_INVALIDPARAM;
    *p2 = b->data;
    *b2 = bytes - first;
  }
  return DS_OK;
}
static HRESULT WINAPI cb_start(IDirectSoundCaptureBuffer8 *i, DWORD flags) {
  CaptureBuffer *b = from_capture_buffer(i);
  EnterCriticalSection(&b->lock);
  b->looping = (flags & DSCBSTART_LOOPING) != 0;
  if (!b->capturing) {
    audio_call(TCOS_AUDIO_PAUSE, b->stream, 0, 0, 0, 0, 0, NULL, NULL, NULL);      /* (the kernel starts with sound from now) */
    b->capturing = TRUE;
  }
  LeaveCriticalSection(&b->lock);
  SetEvent(b->wake);
  return DS_OK;
}
static HRESULT WINAPI cb_stop(IDirectSoundCaptureBuffer8 *i) {
  CaptureBuffer *b = from_capture_buffer(i);
  EnterCriticalSection(&b->lock);
  if (b->capturing) {
    capture_step_locked(b);      /* what was recorded up to now is kept */
    if (b->capturing) {
      b->capturing = FALSE;
      audio_call(TCOS_AUDIO_PAUSE, b->stream, 1, 0, 0, 0, 0, NULL, NULL, NULL);
      capture_notify_stop_locked(b);
    }
  }
  LeaveCriticalSection(&b->lock);
  return DS_OK;
}
static HRESULT WINAPI cb_unlock(IDirectSoundCaptureBuffer8 *i, LPVOID p1, DWORD b1, LPVOID p2, DWORD b2) {
  CaptureBuffer *b = from_capture_buffer(i);
  (void)b1; (void)b2;
  if (!p1) return DSERR_INVALIDPARAM;
  const BYTE *x = (const BYTE *)p1;
  if (x < b->data || x >= b->data + b->size) return DSERR_INVALIDPARAM;
  if (p2 && (BYTE *)p2 != b->data) return DSERR_INVALIDPARAM;
  return DS_OK;
}
static HRESULT WINAPI cb_get_object_in_path(IDirectSoundCaptureBuffer8 *i, REFGUID object, DWORD index, REFGUID iid, LPVOID *out) {
  (void)i; (void)object; (void)index; (void)iid;
  if (!out) return DSERR_INVALIDPARAM;
  *out = NULL;
  return DSERR_CONTROLUNAVAIL;      /* (no capture effects) */
}
static HRESULT WINAPI cb_get_fx_status(IDirectSoundCaptureBuffer8 *i, DWORD count, LPDWORD status) {
  (void)i; (void)count; (void)status;
  return DSERR_CONTROLUNAVAIL;
}
static const IDirectSoundCaptureBuffer8Vtbl kCaptureBufferVtbl = {
  cb_query, cb_add_ref, cb_release, cb_get_caps, cb_get_current_position, cb_get_format, cb_get_status, cb_initialize, cb_lock, cb_start, cb_stop,
  cb_unlock, cb_get_object_in_path, cb_get_fx_status};

static HRESULT WINAPI cn_query(IDirectSoundNotify *i, REFIID riid, void **out) { return cb_query(&from_capture_notify(i)->buffer, riid, out); }
static ULONG WINAPI cn_add_ref(IDirectSoundNotify *i) { return cb_add_ref(&from_capture_notify(i)->buffer); }
static ULONG WINAPI cn_release(IDirectSoundNotify *i) { return cb_release(&from_capture_notify(i)->buffer); }
static HRESULT WINAPI cn_set_positions(IDirectSoundNotify *i, DWORD count, LPCDSBPOSITIONNOTIFY positions) {
  CaptureBuffer *b = from_capture_notify(i);
  if (count > DSBNOTIFICATIONS_MAX || (count && !positions)) return DSERR_INVALIDPARAM;
  for (DWORD k = 0; k < count; ++k)
    if (positions[k].dwOffset != (DWORD)DSBPN_OFFSETSTOP && positions[k].dwOffset >= b->size) return DSERR_INVALIDPARAM;
  DSBPOSITIONNOTIFY *copy_of = NULL;
  if (count) {
    copy_of = (DSBPOSITIONNOTIFY *)alloc(count * sizeof *copy_of);
    if (!copy_of) return DSERR_OUTOFMEMORY;
    copy(copy_of, positions, count * sizeof *copy_of);
  }
  EnterCriticalSection(&b->lock);
  if (b->capturing) {
    LeaveCriticalSection(&b->lock);
    release_memory(copy_of);
    return DSERR_INVALIDCALL;
  }
  DSBPOSITIONNOTIFY *old = b->notifies;
  b->notifies = copy_of;
  b->notify_count = count;
  LeaveCriticalSection(&b->lock);
  release_memory(old);
  return DS_OK;
}
static const IDirectSoundNotifyVtbl kCaptureNotifyVtbl = {cn_query, cn_add_ref, cn_release, cn_set_positions};

/* ---- IDirectSoundCapture8 ----------------------------------------------------------------------------------------------------------------------- */

static CaptureDevice *new_capture_device(void) {
  CaptureDevice *d = (CaptureDevice *)alloc(sizeof *d);
  if (!d) return NULL;
  d->iface.lpVtbl = (IDirectSoundCaptureVtbl *)&kCaptureVtbl;
  d->refs = 1;
  return d;
}

static HRESULT WINAPI c_query(IDirectSoundCapture *i, REFIID riid, void **out) {
  if (!out) return E_POINTER;
  *out = NULL;
  if (!riid) return E_POINTER;
  if (!same_guid(riid, &kIidUnknown) && !same_guid(riid, &IID_IDirectSoundCapture)) return E_NOINTERFACE;      /* (IID_IDirectSoundCapture8 is the same) */
  InterlockedIncrement(&from_capture(i)->refs);
  *out = i;
  return S_OK;
}
static ULONG WINAPI c_add_ref(IDirectSoundCapture *i) { return (ULONG)InterlockedIncrement(&from_capture(i)->refs); }
static ULONG WINAPI c_release(IDirectSoundCapture *i) {
  CaptureDevice *d = from_capture(i);
  const LONG left = InterlockedDecrement(&d->refs);
  if (left == 0) release_memory(d);
  return (ULONG)left;
}
static HRESULT WINAPI c_create_capture_buffer(IDirectSoundCapture *i, LPCDSCBUFFERDESC desc, LPDIRECTSOUNDCAPTUREBUFFER *out, LPUNKNOWN outer) {
  CaptureDevice *d = from_capture(i);
  if (!out) return DSERR_INVALIDPARAM;
  *out = NULL;
  if (outer) return DSERR_NOAGGREGATION;
  if (!d->initialized) return DSERR_UNINITIALIZED;
  if (!desc || (desc->dwSize != sizeof(DSCBUFFERDESC) && desc->dwSize != sizeof(DSCBUFFERDESC1)) || !desc->lpwfxFormat) return DSERR_INVALIDPARAM;
  if (desc->dwFlags & DSCBCAPS_CTRLFX) return DSERR_INVALIDPARAM;      /* (no capture effects) */
  if (desc->dwSize == sizeof(DSCBUFFERDESC) && desc->dwFXCount) return DSERR_INVALIDPARAM;
  const WAVEFORMATEX *f = desc->lpwfxFormat;
  if (!wave_format_code(f)) return DSERR_BADFORMAT;
  const DWORD size = desc->dwBufferBytes - desc->dwBufferBytes % f->nBlockAlign;
  if (size < f->nBlockAlign || size > DSBSIZE_MAX) return DSERR_INVALIDPARAM;
  CaptureBuffer *b = (CaptureBuffer *)alloc(sizeof *b);
  if (!b) return DSERR_OUTOFMEMORY;
  b->buffer.lpVtbl = (IDirectSoundCaptureBuffer8Vtbl *)&kCaptureBufferVtbl;
  b->notify.lpVtbl = (IDirectSoundNotifyVtbl *)&kCaptureNotifyVtbl;
  b->refs = 1;
  b->flags = desc->dwFlags & DSCBCAPS_WAVEMAPPED;
  const DWORD extra = f->wFormatTag == TCOS_WAVE_FORMAT_EXTENSIBLE ? 22 : 0;
  copy(&b->format, f, sizeof(WAVEFORMATEX) + extra);
  b->format.Format.cbSize = (WORD)extra;
  b->size = size;
  b->data = (BYTE *)alloc(size);
  InitializeCriticalSection(&b->lock);
  b->wake = CreateEventA(NULL, FALSE, FALSE, NULL);
  LONG st = 0;
  b->stream = b->data && b->wake ? tcos_audio_open_to(f, TRUE, FALSE, d->node, &st) : 0;
  if (b->stream) audio_call(TCOS_AUDIO_PAUSE, b->stream, 1, 0, 0, 0, 0, NULL, NULL, NULL);      /* (it records from Start) */
  b->thread = b->stream ? CreateThread(NULL, 0, capture_thread, b, 0, NULL) : NULL;
  if (b->thread) SetThreadPriority(b->thread, THREAD_PRIORITY_TIME_CRITICAL);
  InterlockedIncrement(&d->refs);
  b->device = d;
  if (!b->thread) {
    cb_release(&b->buffer);
    return st == TCOS_STATUS_INVALID_PARAMETER ? DSERR_BADFORMAT : !st ? DSERR_OUTOFMEMORY : DSERR_NODRIVER;
  }
  *out = (LPDIRECTSOUNDCAPTUREBUFFER)&b->buffer;
  return DS_OK;
}
static HRESULT WINAPI c_get_caps(IDirectSoundCapture *i, LPDSCCAPS caps) {
  CaptureDevice *d = from_capture(i);
  if (!caps || caps->dwSize < sizeof *caps) return DSERR_INVALIDPARAM;
  if (!d->initialized) return DSERR_UNINITIALIZED;
  caps->dwFlags = DSCCAPS_MULTIPLECAPTURE | DSCCAPS_CERTIFIED;
  caps->dwFormats = WAVE_FORMAT_1M08 | WAVE_FORMAT_1S08 | WAVE_FORMAT_1M16 | WAVE_FORMAT_1S16 | WAVE_FORMAT_2M08 | WAVE_FORMAT_2S08 | WAVE_FORMAT_2M16 |
                    WAVE_FORMAT_2S16 | WAVE_FORMAT_4M08 | WAVE_FORMAT_4S08 | WAVE_FORMAT_4M16 | WAVE_FORMAT_4S16 | WAVE_FORMAT_48M08 | WAVE_FORMAT_48S08 |
                    WAVE_FORMAT_48M16 | WAVE_FORMAT_48S16 | WAVE_FORMAT_96M08 | WAVE_FORMAT_96S08 | WAVE_FORMAT_96M16 | WAVE_FORMAT_96S16;
  caps->dwChannels = 2;
  return DS_OK;
}
static HRESULT WINAPI c_initialize(IDirectSoundCapture *i, LPCGUID guid) {
  CaptureDevice *d = from_capture(i);
  if (d->initialized) return DSERR_ALREADYINITIALIZED;
  /* the microphone, or one of the graph's virtual sources by its GUID */
  const ULONG64 node = guid && !capture_guid(guid) ? tcos_audio_virtual_source_node(guid) : 0;
  if (!node && !capture_guid(guid)) return DSERR_NODRIVER;
  if (!node && !tcos_audio_input_devices()) return DSERR_NODRIVER;
  d->node = node;
  d->initialized = TRUE;
  return DS_OK;
}
static const IDirectSoundCaptureVtbl kCaptureVtbl = {c_query, c_add_ref, c_release, c_create_capture_buffer, c_get_caps, c_initialize};

static HRESULT create_capture(const GUID *guid, void **out, IUnknown *outer) {
  if (!out) return DSERR_INVALIDPARAM;
  *out = NULL;
  if (outer) return DSERR_NOAGGREGATION;
  CaptureDevice *d = new_capture_device();
  if (!d) return DSERR_OUTOFMEMORY;
  const HRESULT hr = c_initialize(&d->iface, guid);
  if (FAILED(hr)) {
    c_release(&d->iface);
    return hr;
  }
  *out = &d->iface;
  return DS_OK;
}

/* ---- IDirectSoundFullDuplex --------------------------------------------------------------------------------------------------------------------- */

typedef struct FullDuplex {
  IDirectSoundFullDuplex iface;
  LONG refs;
  IDirectSoundCapture *capture;      /* (held once Initialize made them) */
  IDirectSound8 *render;
} FullDuplex;

static FullDuplex *from_duplex(void *i) { return (FullDuplex *)i; }

static HRESULT WINAPI x_query(IDirectSoundFullDuplex *i, REFIID riid, void **out) {
  if (!out) return E_POINTER;
  *out = NULL;
  if (!riid) return E_POINTER;
  if (!same_guid(riid, &kIidUnknown) && !same_guid(riid, &IID_IDirectSoundFullDuplex)) return E_NOINTERFACE;
  InterlockedIncrement(&from_duplex(i)->refs);
  *out = i;
  return S_OK;
}
static ULONG WINAPI x_add_ref(IDirectSoundFullDuplex *i) { return (ULONG)InterlockedIncrement(&from_duplex(i)->refs); }
static ULONG WINAPI x_release(IDirectSoundFullDuplex *i) {
  FullDuplex *x = from_duplex(i);
  const LONG left = InterlockedDecrement(&x->refs);
  if (left != 0) return (ULONG)left;
  if (x->capture) c_release(x->capture);
  if (x->render) x->render->lpVtbl->Release(x->render);
  release_memory(x);
  return 0;
}
static HRESULT WINAPI x_initialize(IDirectSoundFullDuplex *i, LPCGUID capture, LPCGUID render, LPCDSCBUFFERDESC cdesc, LPCDSBUFFERDESC rdesc, HWND window,
                                   DWORD level, LPLPDIRECTSOUNDCAPTUREBUFFER8 cbuf, LPLPDIRECTSOUNDBUFFER8 rbuf) {
  FullDuplex *x = from_duplex(i);
  if (!cbuf || !rbuf) return DSERR_INVALIDPARAM;
  *cbuf = NULL;
  *rbuf = NULL;
  if (x->capture || x->render) return DSERR_ALREADYINITIALIZED;
  if (!cdesc || !rdesc) return DSERR_INVALIDPARAM;
  IDirectSoundCapture *c = NULL;
  IDirectSound8 *r = NULL;
  IDirectSoundCaptureBuffer *cb = NULL;
  IDirectSoundBuffer *rb = NULL;
  HRESULT hr = create_capture(capture, (void **)&c, NULL);
  if (SUCCEEDED(hr)) hr = create(render, (void **)&r, NULL);
  if (SUCCEEDED(hr)) hr = r->lpVtbl->SetCooperativeLevel(r, window, level);
  if (SUCCEEDED(hr)) hr = c->lpVtbl->CreateCaptureBuffer(c, cdesc, &cb, NULL);
  if (SUCCEEDED(hr)) hr = r->lpVtbl->CreateSoundBuffer(r, rdesc, &rb, NULL);
  if (SUCCEEDED(hr)) hr = cb->lpVtbl->QueryInterface(cb, &IID_IDirectSoundCaptureBuffer8, (void **)cbuf);
  if (SUCCEEDED(hr)) hr = rb->lpVtbl->QueryInterface(rb, &IID_IDirectSoundBuffer8, (void **)rbuf);
  if (cb) cb->lpVtbl->Release(cb);
  if (rb) rb->lpVtbl->Release(rb);
  if (FAILED(hr)) {
    if (*cbuf) (*cbuf)->lpVtbl->Release(*cbuf);
    if (*rbuf) (*rbuf)->lpVtbl->Release(*rbuf);
    *cbuf = NULL;
    *rbuf = NULL;
    if (c) c_release(c);
    if (r) r->lpVtbl->Release(r);
    return hr;
  }
  x->capture = c;
  x->render = r;
  return DS_OK;
}
static const IDirectSoundFullDuplexVtbl kDuplexVtbl = {x_query, x_add_ref, x_release, x_initialize};

static FullDuplex *new_duplex(void) {
  FullDuplex *x = (FullDuplex *)alloc(sizeof *x);
  if (!x) return NULL;
  x->iface.lpVtbl = (IDirectSoundFullDuplexVtbl *)&kDuplexVtbl;
  x->refs = 1;
  return x;
}

/* ---- exports ------------------------------------------------------------------------------------------------------------------------------------ */

EXPORT HRESULT WINAPI DirectSoundCaptureCreate(LPCGUID guid, LPDIRECTSOUNDCAPTURE *out, LPUNKNOWN outer) { return create_capture(guid, (void **)out, outer); }
EXPORT HRESULT WINAPI DirectSoundCaptureCreate8(LPCGUID guid, LPDIRECTSOUNDCAPTURE8 *out, LPUNKNOWN outer) { return create_capture(guid, (void **)out, outer); }

static const char kCaptureDescriptionA[] = "Microphone (TheoryCraftOS)";
static const char kCapturePrimaryA[] = "Primary Sound Capture Driver";
static const char kCaptureModuleA[] = "{0.0.1.00000000}.{5af3a3c3-6e1b-4b4e-8d2f-1c0a7e5d9b31}";

/* The virtual source number `which`: its GUID, description and module (its WASAPI endpoint id); FALSE past the last. */
static BOOL virtual_capture_device(ULONG64 which, GUID *guid, char *description, int size, char *module) {
  ULONG64 count = 0;
  const ULONG64 node = tcos_audio_virtual_source(which, description, size, &count);
  if (!node) return FALSE;
  tcos_audio_virtual_source_guid(node, guid);
  static const char hex[] = "0123456789abcdef";
  const char prefix[] = "{0.0.1.00000000}.{5af3a3c3-6e1b-4b4e-8d2f-";
  int k = 0;
  for (; prefix[k]; ++k) module[k] = prefix[k];
  for (int d = 11; d >= 0; --d) module[k++] = hex[(node >> (4 * d)) & 0xF];
  module[k++] = '}';
  module[k] = 0;
  return TRUE;
}

/* The primary driver and the microphone when there is one, then the graph's virtual sources, a device each. */
EXPORT HRESULT WINAPI DirectSoundCaptureEnumerateA(LPDSENUMCALLBACKA callback, LPVOID context) {
  if (!callback) return DSERR_INVALIDPARAM;
  if (tcos_audio_input_devices()) {
    if (!callback(NULL, kCapturePrimaryA, "", context)) return DS_OK;
    if (!callback((LPGUID)&kCaptureGuid, kCaptureDescriptionA, kCaptureModuleA, context)) return DS_OK;
  }
  for (ULONG64 i = 0; i < TCOS_AUDIO_MAX_VIRTUAL; ++i) {
    GUID guid;
    char description[96], module[80];
    if (!virtual_capture_device(i, &guid, description, sizeof description, module)) break;
    if (!callback(&guid, description, module, context)) break;
  }
  return DS_OK;
}
EXPORT HRESULT WINAPI DirectSoundCaptureEnumerateW(LPDSENUMCALLBACKW callback, LPVOID context) {
  if (!callback) return DSERR_INVALIDPARAM;
  if (tcos_audio_input_devices()) {
    WCHAR primary[sizeof kCapturePrimaryA], description[sizeof kCaptureDescriptionA], module[sizeof kCaptureModuleA], empty[1] = {0};
    for (SIZE_T k = 0; k < sizeof kCapturePrimaryA; ++k) primary[k] = (WCHAR)kCapturePrimaryA[k];
    for (SIZE_T k = 0; k < sizeof kCaptureDescriptionA; ++k) description[k] = (WCHAR)kCaptureDescriptionA[k];
    for (SIZE_T k = 0; k < sizeof kCaptureModuleA; ++k) module[k] = (WCHAR)kCaptureModuleA[k];
    if (!callback(NULL, primary, empty, context)) return DS_OK;
    if (!callback((LPGUID)&kCaptureGuid, description, module, context)) return DS_OK;
  }
  for (ULONG64 i = 0; i < TCOS_AUDIO_MAX_VIRTUAL; ++i) {
    GUID guid;
    char da[96], ma[80];
    WCHAR dw[96], mw[80];
    if (!virtual_capture_device(i, &guid, da, sizeof da, ma)) break;
    int k = 0;
    for (; da[k] && k < 95; ++k) dw[k] = (WCHAR)(unsigned char)da[k];
    dw[k] = 0;
    for (k = 0; ma[k] && k < 79; ++k) mw[k] = (WCHAR)ma[k];
    mw[k] = 0;
    if (!callback(&guid, dw, mw, context)) break;
  }
  return DS_OK;
}

EXPORT HRESULT WINAPI DirectSoundFullDuplexCreate(LPCGUID capture, LPCGUID render, LPCDSCBUFFERDESC cdesc, LPCDSBUFFERDESC rdesc, HWND window, DWORD level,
                                                  LPDIRECTSOUNDFULLDUPLEX *out, LPDIRECTSOUNDCAPTUREBUFFER8 *cbuf, LPDIRECTSOUNDBUFFER8 *rbuf, LPUNKNOWN outer) {
  if (out) *out = NULL;
  if (cbuf) *cbuf = NULL;
  if (rbuf) *rbuf = NULL;
  if (!out || !cbuf || !rbuf) return DSERR_INVALIDPARAM;
  if (outer) return DSERR_NOAGGREGATION;
  FullDuplex *x = new_duplex();
  if (!x) return DSERR_OUTOFMEMORY;
  const HRESULT hr = x_initialize(&x->iface, capture, render, cdesc, rdesc, window, level, cbuf, rbuf);
  if (FAILED(hr)) {
    x_release(&x->iface);
    return hr;
  }
  *out = &x->iface;
  return DS_OK;
}
