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

/* d3d12.dll: command queues (ID3D12CommandQueue), fences (ID3D12Fence) and the presenter interface dxgi.dll's swap chains use. Included by d3d12.c.
 *
 * A queue is a context of the kernel (D3DKMTCreateContext); ExecuteCommandLists sends each list's chunks to it with D3DKMTRender, which runs them (on the calling thread) before it
 * returns unless a fence they wait for is not there yet. Signal and Wait are the context's fence operations, ordered with the lists. */

typedef struct Queue {
  DObj d;
  ITcosPresenter presenter;
  D3D12_COMMAND_QUEUE_DESC desc;
  KmtContext ctx;
  CRITICAL_SECTION lock;
} Queue;

typedef struct Fence {
  DObj d;
  UINT sync;
  D3D12_FENCE_FLAGS flags;
  struct Fence *next, *prev;      /* Dev.fences */
} Fence;

static const GUID *const g_queue_iids[] = {&IID_ID3D12Object, &IID_ID3D12DeviceChild, &IID_ID3D12Pageable, &IID_ID3D12CommandQueue, NULL};
static const GUID *const g_fence_iids[] = {&IID_ID3D12Object, &IID_ID3D12DeviceChild, &IID_ID3D12Pageable, &IID_ID3D12Fence, &IID_ID3D12Fence1, NULL};

/* ---- fences ---------------------------------------------------------------------------------------------------------------------------------------------------------------------- */

static void fence_destroy(DxObj *o) {
  Fence *f = (Fence *)o;
  Dev *d = f->d.dev;
  AcquireSRWLockExclusive(&d->fence_lock);
  if (f->prev) f->prev->next = f->next;
  else if (d->fences == f) d->fences = f->next;
  if (f->next) f->next->prev = f->prev;
  ReleaseSRWLockExclusive(&d->fence_lock);
  if (f->sync) kmt_destroy_sync(f->sync);
  dobj_child_destroy(o);
}

/* A fence of the device: a new synchronization object (opened 0), or one opened from a shared fence's NT handle (device1.inc's OpenSharedHandle), which is then the
 * fence's. D3D12_FENCE_FLAG_SHARED makes it one CreateSharedHandle shares; D3D12_FENCE_FLAG_SHARED_CROSS_ADAPTER one that devices of other adapters open too (the
 * kernel's synchronization objects are of no adapter: a context of any adapter signals and waits for the same one), which is shared as well;
 * D3D12_FENCE_FLAG_NON_MONITORED changes nothing (no fence here keeps its value in memory a process could map). */
static HRESULT fence_make(Dev *This, UINT64 initial, D3D12_FENCE_FLAGS flags, UINT opened, Fence **out) {
  extern const ID3D12Fence1Vtbl fence_vtbl;
  *out = NULL;
  if (flags & ~(D3D12_FENCE_FLAG_SHARED | D3D12_FENCE_FLAG_SHARED_CROSS_ADAPTER | D3D12_FENCE_FLAG_NON_MONITORED)) return E_INVALIDARG;
  Fence *f = dobj_new(sizeof *f, This, &fence_vtbl, g_fence_iids, fence_destroy);
  if (!f) return E_OUTOFMEMORY;
  if (opened) f->sync = opened;
  else {
    const NTSTATUS st = (flags & (D3D12_FENCE_FLAG_SHARED | D3D12_FENCE_FLAG_SHARED_CROSS_ADAPTER)) ? kmt_create_shared_fence(This->kdev, initial, &f->sync)
                                                                                                   : kmt_create_fence(This->kdev, initial, &f->sync);
    if (!KMT_OK(st)) { dx_Release(OBJ(f)); return hr_from(st); }
  }
  f->flags = flags;
  AcquireSRWLockExclusive(&This->fence_lock);
  f->next = This->fences;
  if (f->next) f->next->prev = f;
  This->fences = f;
  ReleaseSRWLockExclusive(&This->fence_lock);
  if (InterlockedCompareExchange(&This->removed, 0, 0)) kmt_signal_cpu(This->kdev, f->sync, (~(UINT64)0));
  *out = f;
  return S_OK;
}
static HRESULT STDMETHODCALLTYPE dev_CreateFence(Dev *This, UINT64 initial, D3D12_FENCE_FLAGS flags, REFIID riid, void **out) {
  Fence *f = NULL;
  const HRESULT hr = fence_make(This, initial, flags, 0, &f);
  if (FAILED(hr)) return hr;
  return query_out((IUnknown *)f, riid, out);
}

static UINT64 fence_value(Fence *f) {
  UINT request[2] = {ESC_QUERY_SYNC, f->sync};
  UINT reply[2] = {0, 0};
  if (!KMT_OK(dev_escape(f->d.dev, request, 2, reply, 2))) return 0;
  return reply[0] | ((UINT64)reply[1] << 32);
}
static UINT64 STDMETHODCALLTYPE fence_GetCompletedValue(Fence *This) { return fence_value(This); }

typedef struct Waiter { Fence *fence; UINT64 value; HANDLE event; } Waiter;
static DWORD WINAPI waiter_thread(LPVOID param) {
  Waiter *w = param;
  kmt_wait_cpu(w->fence->d.dev->kdev, w->fence->sync, w->value);
  if (w->event) SetEvent(w->event);
  dx_Release(OBJ(w->fence));
  mem_free(w);
  return 0;
}
static HRESULT STDMETHODCALLTYPE fence_SetEventOnCompletion(Fence *This, UINT64 value, HANDLE event) {
  if (fence_value(This) >= value) {
    if (event) SetEvent(event);
    return S_OK;
  }
  if (!event) {
    kmt_wait_cpu(This->d.dev->kdev, This->sync, value);      /* (NULL: wait here) */
    return S_OK;
  }
  Waiter *w = mem_alloc(sizeof *w);
  if (!w) return E_OUTOFMEMORY;
  w->fence = This;
  w->value = value;
  w->event = event;
  dx_AddRef(OBJ(This));
  HANDLE t = CreateThread(NULL, 0, waiter_thread, w, 0, NULL);
  if (!t) { dx_Release(OBJ(This)); mem_free(w); return E_OUTOFMEMORY; }
  CloseHandle(t);
  return S_OK;
}
static HRESULT STDMETHODCALLTYPE fence_Signal(Fence *This, UINT64 value) {
  return hr_from(kmt_signal_cpu(This->d.dev->kdev, This->sync, value));
}

/* ---- queues ---------------------------------------------------------------------------------------------------------------------------------------------------------------------- */

static Queue *queue_of_presenter(ITcosPresenter *p) { return (Queue *)((BYTE *)p - offsetof(Queue, presenter)); }

static void queue_destroy(DxObj *o) {
  Queue *q = (Queue *)o;
  if (q->ctx.handle) kmt_destroy_context(q->ctx.handle);
  DeleteCriticalSection(&q->lock);
  dobj_child_destroy(o);
}

extern const ITcosPresenterVtbl pres_vtbl;

static HRESULT STDMETHODCALLTYPE dev_CreateCommandQueue(Dev *This, const D3D12_COMMAND_QUEUE_DESC *desc, REFIID riid, void **out) {
  extern const ID3D12CommandQueueVtbl queue_vtbl;
  /* (the video queues: decode, process and encode) */
  if (!desc || (desc->Type != D3D12_COMMAND_LIST_TYPE_DIRECT && desc->Type != D3D12_COMMAND_LIST_TYPE_COMPUTE && desc->Type != D3D12_COMMAND_LIST_TYPE_COPY &&
                desc->Type != D3D12_COMMAND_LIST_TYPE_VIDEO_DECODE && desc->Type != D3D12_COMMAND_LIST_TYPE_VIDEO_PROCESS && desc->Type != D3D12_COMMAND_LIST_TYPE_VIDEO_ENCODE))
    return E_INVALIDARG;
  Queue *q = dobj_new(sizeof *q, This, &queue_vtbl, g_queue_iids, queue_destroy);
  if (!q) return E_OUTOFMEMORY;
  InitializeCriticalSection(&q->lock);
  q->presenter.lpVtbl = &pres_vtbl;
  q->desc = *desc;
  const NTSTATUS st = kmt_create_context(This->kdev, &q->ctx);
  if (!KMT_OK(st) || !q->ctx.commands) { dx_Release(OBJ(q)); return KMT_OK(st) ? E_OUTOFMEMORY : hr_from(st); }
  return query_out((IUnknown *)q, riid, out);
}

/* The presenter is the same object as the queue: QueryInterface hands out the second interface. */
static HRESULT STDMETHODCALLTYPE queue_QueryInterface(DxObj *o, REFIID riid, void **out) {
  if (out && riid && guid_eq(riid, &IID_ITcosPresenter)) {
    Queue *q = (Queue *)o;
    dx_AddRef(o);
    *out = &q->presenter;
    return S_OK;
  }
  return dx_QueryInterface(o, riid, out);
}
static HRESULT STDMETHODCALLTYPE queue_GetDevice(Queue *This, REFIID riid, void **out) { return dobj_GetDevice(&This->d, riid, out); }

static void STDMETHODCALLTYPE queue_SetMarker(Queue *This, UINT m, const void *d, UINT s) {}
static void STDMETHODCALLTYPE queue_BeginEvent(Queue *This, UINT m, const void *d, UINT s) {}
static void STDMETHODCALLTYPE queue_EndEvent(Queue *This) {}

/* After a chunk ran: each fan's counts (the device query its open queries of one kind shared in the chunk) added to each member's result - an occlusion predicate and a
 * disjoint flag are or-ed, the timestamp frequency is set (CommandStream.hpp's kinds: 1 predicate, 9 frequency, 10 disjoint, 11 pipeline statistics (11 values), 12 the
 * 14 of PIPELINE_STATISTICS1, 13 stream output statistics (2), the others one). */
static void chunk_fan_out(Queue *q, const Chunk *c) {
  if (!c->nfans || !c->fan_scratch) return;
  const UINT kdev = q->d.dev->kdev;
  void *scratch = NULL;
  if (!KMT_OK(kmt_lock(kdev, c->fan_scratch, 0, KMT_LOCK_READ_ONLY, &scratch))) return;
  UINT64 values[16];
  for (UINT f = 0; f < c->nfans; ++f) {
    const QueryFan *fan = &c->fans[f];
    const UINT kind = fan->kind & 0xFF, n = kind == 11 ? 11 : kind == 12 ? 14 : kind == 13 ? 2 : 1;
    memcpy(values, (const BYTE *)scratch + (SIZE_T)fan->slot * QUERY_SCRATCH_STRIDE, n * 8);
    for (UINT m = 0; m < fan->nmembers; ++m) {
      void *data = NULL;
      if (!KMT_OK(kmt_lock(kdev, fan->members[m].alloc, 0, 0, &data))) continue;
      UINT64 *to = (UINT64 *)((BYTE *)data + fan->members[m].offset);
      for (UINT k = 0; k < n; ++k) {
        UINT64 v;
        memcpy(&v, &to[k], 8);
        v = kind == 1 || kind == 10 ? (v || values[k] ? 1 : 0) : kind == 9 ? values[k] : v + values[k];
        memcpy(&to[k], &v, 8);
      }
      kmt_unlock(kdev, fan->members[m].alloc);
    }
  }
  kmt_unlock(kdev, c->fan_scratch);
}

static void run_chunk(Queue *q, const Chunk *c) {
  if (!c->size) return;
  if (c->size > q->ctx.command_bytes || c->nalloc > q->ctx.allocation_entries) return;
  memcpy(q->ctx.commands, c->bytes, c->size);
  for (UINT i = 0; i < c->nso_patches; ++i) {
    /* (a stream output binding that starts after the view's filled size as it is now) */
    const SoPatch *p = &c->so_patches[i];
    UINT64 filled = 0;
    if (!res_read(p->fill, p->fill_offset, 8, &filled)) filled = 0;
    if (filled > p->size) filled = p->size;
    const UINT start = (UINT)(p->offset + (filled & ~(UINT64)3));
    if (p->at + 4 <= c->size) memcpy((BYTE *)q->ctx.commands + p->at, &start, 4);
  }
  for (UINT i = 0; i < c->nalloc; ++i) {
    q->ctx.allocation_list[i * 2] = c->allocs[i];
    q->ctx.allocation_list[i * 2 + 1] = 0;
  }
  kmt_render(&q->ctx, c->size, c->nalloc);      /* (a stream the kernel refuses is dropped: its work is not done) */
  chunk_fan_out(q, c);
}

/* (the queue's methods take the lists, fences and buffers of this library: a proxy's wrapper of one is seen through, tcos_object.h; a foreign object is not run) */
extern const ID3D12GraphicsCommandList10Vtbl list_vtbl;
extern const ID3D12Fence1Vtbl fence_vtbl;
extern const ID3D12Resource2Vtbl res_vtbl;
extern const ID3D12VideoDecodeCommandList3Vtbl vdlist_vtbl;
extern const ID3D12VideoProcessCommandList3Vtbl vplist_vtbl;
extern const ID3D12VideoEncodeCommandList4Vtbl velist_vtbl;
static void STDMETHODCALLTYPE queue_ExecuteCommandLists(Queue *This, UINT count, ID3D12CommandList *const *lists) {
  if (count && !lists) return;
  EnterCriticalSection(&This->lock);
  res_push_all();
  for (UINT i = 0; i < count; ++i) {
    static const void *const kinds[] = {&list_vtbl, &vdlist_vtbl, &vplist_vtbl, &velist_vtbl};
    CmdList *l = (CmdList *)tcos_own_any(lists[i], kinds, 4);
    if (!l || !l->closed) continue;
    for (UINT k = 0; k < l->nheaps_used; ++k) dheap_sync(l->heaps_used[k]);
    for (UINT k = 0; k < l->nchunks; ++k) {
      if (chunk_skipped(l->chunks[k])) continue;
      if (l->chunks[k]->indirect) {
        CmdList *t = indirect_expand(l->chunks[k]->indirect);
        if (t) {
          for (UINT m = 0; m < t->nchunks; ++m) run_chunk(This, t->chunks[m]);
          list_free_recording(t);
          mem_free(t->chunks);
          mem_free(t);
        }
      } else {
        run_chunk(This, l->chunks[k]);
      }
      list_apply_fills(l, k);      /* (the filled sizes the draws so far reached, for what reads them next) */
    }
  }
  LeaveCriticalSection(&This->lock);
}

static HRESULT STDMETHODCALLTYPE queue_Signal(Queue *This, ID3D12Fence *fence, UINT64 value) {
  const Fence *f = (const Fence *)tcos_own(fence, &fence_vtbl);
  if (!f) return E_INVALIDARG;
  return hr_from(kmt_signal_gpu(This->ctx.handle, f->sync, value));
}
static HRESULT STDMETHODCALLTYPE queue_Wait(Queue *This, ID3D12Fence *fence, UINT64 value) {
  const Fence *f = (const Fence *)tcos_own(fence, &fence_vtbl);
  if (!f) return E_INVALIDARG;
  return hr_from(kmt_wait_gpu(This->ctx.handle, f->sync, value));
}
static HRESULT STDMETHODCALLTYPE queue_GetTimestampFrequency(Queue *This, UINT64 *frequency) {
  if (!frequency) return E_INVALIDARG;
  *frequency = 1000000000ull;
  return S_OK;
}
static HRESULT STDMETHODCALLTYPE queue_GetClockCalibration(Queue *This, UINT64 *gpu, UINT64 *cpu) {
  LARGE_INTEGER n;
  if (!gpu || !cpu) return E_INVALIDARG;
  QueryPerformanceCounter(&n);
  *gpu = *cpu = (UINT64)n.QuadPart;
  return S_OK;
}
static D3D12_COMMAND_QUEUE_DESC *STDMETHODCALLTYPE queue_GetDesc(Queue *This, D3D12_COMMAND_QUEUE_DESC *ret) { *ret = This->desc; return ret; }


/* ---- the presenter ---------------------------------------------------------------------------------------------------------------------------------------------------------- */

static HRESULT STDMETHODCALLTYPE pres_QueryInterface(ITcosPresenter *This, REFIID riid, void **out) { return queue_QueryInterface(OBJ(queue_of_presenter(This)), riid, out); }
static ULONG STDMETHODCALLTYPE pres_AddRef(ITcosPresenter *This) { return dx_AddRef(OBJ(queue_of_presenter(This))); }
static ULONG STDMETHODCALLTYPE pres_Release(ITcosPresenter *This) { return dx_Release(OBJ(queue_of_presenter(This))); }

static HRESULT STDMETHODCALLTYPE pres_CreateBackBuffers(ITcosPresenter *This, UINT count, UINT width, UINT height, DXGI_FORMAT format, UINT usage, IUnknown **out) {
  Queue *q = queue_of_presenter(This);
  D3D12_RESOURCE_DESC desc;
  D3D12_HEAP_PROPERTIES props;
  memset(&desc, 0, sizeof desc);
  memset(&props, 0, sizeof props);
  props.Type = D3D12_HEAP_TYPE_DEFAULT;
  props.CreationNodeMask = props.VisibleNodeMask = 1;
  desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  desc.Width = width;
  desc.Height = height;
  desc.DepthOrArraySize = 1;
  desc.MipLevels = 1;
  desc.SampleDesc.Count = 1;
  desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
  /* (the sRGB flavours are made as the plain ones: the pixels are shown as they are) */
  desc.Format = format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB ? DXGI_FORMAT_B8G8R8A8_UNORM : format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB ? DXGI_FORMAT_R8G8B8A8_UNORM : format;
  for (UINT i = 0; i < count; ++i) {
    Resource *r = NULL;
    const HRESULT hr = resource_make(q->d.dev, &props, D3D12_HEAP_FLAG_NONE, &desc, NULL, 0, 0, &r);
    if (FAILED(hr)) {
      for (UINT k = 0; k < i; ++k) { out[k]->lpVtbl->Release(out[k]); out[k] = NULL; }
      return hr;
    }
    out[i] = (IUnknown *)r;
  }
  return S_OK;
}

static UINT half_to_byte(UINT h) {
  const UINT e = (h >> 10) & 31, m = h & 1023;
  if (h & 0x8000) return 0;
  if (e == 0) return 0;
  if (e >= 15) return 255;
  const UINT v = ((1024 + m) * 255) >> (10 + 15 - e);      /* value = (1 + m / 1024) * 2^(e - 15) */
  return v > 255 ? 255 : v;
}

static HRESULT STDMETHODCALLTYPE pres_ReadBackBuffer(ITcosPresenter *This, IUnknown *buffer, void *dst, UINT pitch) {
  Queue *q = queue_of_presenter(This);
  const Resource *r = (const Resource *)tcos_own(buffer, &res_vtbl);
  void *data = NULL;
  if (!r || !dst || !r->fmt || !KMT_OK(kmt_lock(q->d.dev->kdev, r->alloc, 0, KMT_LOCK_READ_ONLY, &data))) return E_FAIL;
  const UINT w = (UINT)r->desc.Width, h = r->desc.Height;
  for (UINT y = 0; y < h; ++y) {
    BYTE *out = (BYTE *)dst + (SIZE_T)y * pitch;
    switch (r->fmt->kernel) {
      case KF_A8R8G8B8: case KF_X8R8G8B8: case KF_BGRA8_SRGB: memcpy(out, (BYTE *)data + (SIZE_T)y * w * 4, (SIZE_T)w * 4); break;
      case KF_A8B8G8R8: case KF_X8B8G8R8: case KF_RGBA8_SRGB:
        for (UINT x = 0; x < w; ++x) {
          const BYTE *p = (BYTE *)data + ((SIZE_T)y * w + x) * 4;
          out[x * 4 + 0] = p[2]; out[x * 4 + 1] = p[1]; out[x * 4 + 2] = p[0]; out[x * 4 + 3] = 255;
        }
        break;
      case KF_RGBA16F:
        for (UINT x = 0; x < w; ++x) {
          const UINT16 *p = (const UINT16 *)((BYTE *)data + ((SIZE_T)y * w + x) * 8);
          out[x * 4 + 0] = (BYTE)half_to_byte(p[2]); out[x * 4 + 1] = (BYTE)half_to_byte(p[1]); out[x * 4 + 2] = (BYTE)half_to_byte(p[0]); out[x * 4 + 3] = 255;
        }
        break;
      default: memset(out, 0x80, (SIZE_T)w * 4); break;
    }
  }
  kmt_unlock(q->d.dev->kdev, r->alloc);
  return S_OK;
}

const ITcosPresenterVtbl pres_vtbl = {pres_QueryInterface, pres_AddRef, pres_Release, pres_CreateBackBuffers, pres_ReadBackBuffer};
