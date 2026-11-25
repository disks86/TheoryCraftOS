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

/* Part of d3d12.c: what the device interfaces after ID3D12Device add (ID3D12Device1 .. ID3D12Device10).
 *
 * The features the device has not - raytracing, mesh shaders, variable-rate shading, sampler feedback, view instancing, protected sessions, meta commands,
 * existing heaps, background processing - CheckFeatureSupport reports at their "not supported" tier, and what makes their objects answers as Direct3D 12 does
 * on such a device (DXGI_ERROR_UNSUPPORTED or E_INVALIDARG). The rest is here: pipeline state streams, pipeline libraries, shader cache sessions, the resource,
 * heap, list and queue creation of the later interfaces (descriptions of D3D12_RESOURCE_DESC1, layouts of the enhanced barriers), allocation information with
 * each resource's place, residency (everything is resident), waits on several fences, lifetime trackers, RemoveDevice. */

/* ---- resources of the later interfaces --------------------------------------------------------------------------------------------------------------------------- */

/* A D3D12_RESOURCE_DESC1 as a D3D12_RESOURCE_DESC: FALSE for one with a sampler feedback region (the device has no sampler feedback). */
static BOOL desc_of_desc1(const D3D12_RESOURCE_DESC1 *in, D3D12_RESOURCE_DESC *out) {
  if (!in) return FALSE;
  if (in->SamplerFeedbackMipRegion.Width || in->SamplerFeedbackMipRegion.Height || in->SamplerFeedbackMipRegion.Depth) return FALSE;
  out->Dimension = in->Dimension;
  out->Alignment = in->Alignment;
  out->Width = in->Width;
  out->Height = in->Height;
  out->DepthOrArraySize = in->DepthOrArraySize;
  out->MipLevels = in->MipLevels;
  out->Format = in->Format;
  out->SampleDesc = in->SampleDesc;
  out->Layout = in->Layout;
  out->Flags = in->Flags;
  return TRUE;
}
/* The resource state of an enhanced-barrier layout a resource is made in (the kernel's allocations are not state tracked, so this is only what the program said). */
static D3D12_RESOURCE_STATES state_of_layout(D3D12_BARRIER_LAYOUT layout) {
  switch (layout) {
    case D3D12_BARRIER_LAYOUT_RENDER_TARGET: return D3D12_RESOURCE_STATE_RENDER_TARGET;
    case D3D12_BARRIER_LAYOUT_DEPTH_STENCIL_WRITE: return D3D12_RESOURCE_STATE_DEPTH_WRITE;
    case D3D12_BARRIER_LAYOUT_DEPTH_STENCIL_READ: return D3D12_RESOURCE_STATE_DEPTH_READ;
    case D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS: return D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    case D3D12_BARRIER_LAYOUT_COPY_SOURCE: return D3D12_RESOURCE_STATE_COPY_SOURCE;
    case D3D12_BARRIER_LAYOUT_COPY_DEST: return D3D12_RESOURCE_STATE_COPY_DEST;
    default: return D3D12_RESOURCE_STATE_COMMON;
  }
}
/* (no protected resource sessions are made, so any one given is not one of this device's) */
static HRESULT STDMETHODCALLTYPE dev_CreateCommittedResource1(Dev *This, const D3D12_HEAP_PROPERTIES *props, D3D12_HEAP_FLAGS flags, const D3D12_RESOURCE_DESC *desc,
                                                               D3D12_RESOURCE_STATES state, const D3D12_CLEAR_VALUE *clear, ID3D12ProtectedResourceSession *session, REFIID riid,
                                                               void **out) {
  if (out) *out = NULL;
  if (session) return E_INVALIDARG;
  return dev_CreateCommittedResource(This, props, flags, desc, state, clear, riid, out);
}
static HRESULT STDMETHODCALLTYPE dev_CreateCommittedResource2(Dev *This, const D3D12_HEAP_PROPERTIES *props, D3D12_HEAP_FLAGS flags, const D3D12_RESOURCE_DESC1 *desc,
                                                               D3D12_RESOURCE_STATES state, const D3D12_CLEAR_VALUE *clear, ID3D12ProtectedResourceSession *session, REFIID riid,
                                                               void **out) {
  D3D12_RESOURCE_DESC d;
  if (out) *out = NULL;
  if (session || !desc_of_desc1(desc, &d)) return E_INVALIDARG;
  return dev_CreateCommittedResource(This, props, flags, &d, state, clear, riid, out);
}
/* (castable formats need RelaxedFormatCastingSupported, which this device has not: a view may still name any format of the resource's family) */
static HRESULT STDMETHODCALLTYPE dev_CreateCommittedResource3(Dev *This, const D3D12_HEAP_PROPERTIES *props, D3D12_HEAP_FLAGS flags, const D3D12_RESOURCE_DESC1 *desc,
                                                               D3D12_BARRIER_LAYOUT layout, const D3D12_CLEAR_VALUE *clear, ID3D12ProtectedResourceSession *session,
                                                               UINT32 castable_count, DXGI_FORMAT *castable, REFIID riid, void **out) {
  D3D12_RESOURCE_DESC d;
  if (out) *out = NULL;
  if (session || castable_count || !desc_of_desc1(desc, &d)) return E_INVALIDARG;
  if (d.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER && layout != D3D12_BARRIER_LAYOUT_UNDEFINED) return E_INVALIDARG;      /* (buffers have no layout) */
  return dev_CreateCommittedResource(This, props, flags, &d, state_of_layout(layout), clear, riid, out);
}
static HRESULT STDMETHODCALLTYPE dev_CreatePlacedResource1(Dev *This, ID3D12Heap *heap, UINT64 offset, const D3D12_RESOURCE_DESC1 *desc, D3D12_RESOURCE_STATES state,
                                                            const D3D12_CLEAR_VALUE *clear, REFIID riid, void **out) {
  D3D12_RESOURCE_DESC d;
  if (out) *out = NULL;
  if (!desc_of_desc1(desc, &d)) return E_INVALIDARG;
  return dev_CreatePlacedResource(This, heap, offset, &d, state, clear, riid, out);
}
static HRESULT STDMETHODCALLTYPE dev_CreatePlacedResource2(Dev *This, ID3D12Heap *heap, UINT64 offset, const D3D12_RESOURCE_DESC1 *desc, D3D12_BARRIER_LAYOUT layout,
                                                            const D3D12_CLEAR_VALUE *clear, UINT32 castable_count, DXGI_FORMAT *castable, REFIID riid, void **out) {
  D3D12_RESOURCE_DESC d;
  if (out) *out = NULL;
  if (castable_count || !desc_of_desc1(desc, &d)) return E_INVALIDARG;
  return dev_CreatePlacedResource(This, heap, offset, &d, state_of_layout(layout), clear, riid, out);
}
static HRESULT STDMETHODCALLTYPE dev_CreateReservedResource1(Dev *This, const D3D12_RESOURCE_DESC *desc, D3D12_RESOURCE_STATES state, const D3D12_CLEAR_VALUE *clear,
                                                              ID3D12ProtectedResourceSession *session, REFIID riid, void **out) {
  if (out) *out = NULL;
  if (session) return E_INVALIDARG;
  return dev_CreateReservedResource(This, desc, state, clear, riid, out);
}
static HRESULT STDMETHODCALLTYPE dev_CreateReservedResource2(Dev *This, const D3D12_RESOURCE_DESC *desc, D3D12_BARRIER_LAYOUT layout, const D3D12_CLEAR_VALUE *clear,
                                                              ID3D12ProtectedResourceSession *session, UINT32 castable_count, DXGI_FORMAT *castable, REFIID riid, void **out) {
  if (out) *out = NULL;
  if (session || castable_count) return E_INVALIDARG;
  return dev_CreateReservedResource(This, desc, state_of_layout(layout), clear, riid, out);
}
static HRESULT STDMETHODCALLTYPE dev_CreateHeap1(Dev *This, const D3D12_HEAP_DESC *desc, ID3D12ProtectedResourceSession *session, REFIID riid, void **out) {
  if (out) *out = NULL;
  if (session) return E_INVALIDARG;
  return dev_CreateHeap(This, desc, riid, out);
}
/* The size and alignment of a heap for the resources, and where each goes in it (one after the other, each at its alignment). */
static void allocation_infos(Dev *This, UINT count, const D3D12_RESOURCE_DESC *descs, const D3D12_RESOURCE_DESC1 *descs1, D3D12_RESOURCE_ALLOCATION_INFO *ret,
                             D3D12_RESOURCE_ALLOCATION_INFO1 *each) {
  UINT64 offset = 0;
  BOOL bad = FALSE;
  for (UINT i = 0; i < count; ++i) {
    D3D12_RESOURCE_DESC d;
    if (descs) d = descs[i];
    else if (!desc_of_desc1(&descs1[i], &d)) { bad = TRUE; continue; }
    D3D12_RESOURCE_ALLOCATION_INFO one;
    dev_GetResourceAllocationInfo(This, &one, 0, 1, &d);
    offset = KALIGN(offset, one.Alignment);
    if (each) { each[i].Offset = offset; each[i].Alignment = one.Alignment; each[i].SizeInBytes = one.SizeInBytes; }
    offset += one.SizeInBytes;
  }
  ret->SizeInBytes = bad ? (~(UINT64)0) : offset;      /* (a description that cannot be had: (~(UINT64)0), as Direct3D 12 answers) */
  ret->Alignment = KTILE_BYTES;
}
static D3D12_RESOURCE_ALLOCATION_INFO *STDMETHODCALLTYPE dev_GetResourceAllocationInfo1(Dev *This, D3D12_RESOURCE_ALLOCATION_INFO *ret, UINT mask, UINT count,
                                                                                          const D3D12_RESOURCE_DESC *descs, D3D12_RESOURCE_ALLOCATION_INFO1 *each) {
  allocation_infos(This, count, descs, NULL, ret, each);
  return ret;
}
static D3D12_RESOURCE_ALLOCATION_INFO *STDMETHODCALLTYPE dev_GetResourceAllocationInfo2(Dev *This, D3D12_RESOURCE_ALLOCATION_INFO *ret, UINT mask, UINT count,
                                                                                          const D3D12_RESOURCE_DESC1 *descs, D3D12_RESOURCE_ALLOCATION_INFO1 *each) {
  allocation_infos(This, count, NULL, descs, ret, each);
  return ret;
}
static void STDMETHODCALLTYPE dev_GetCopyableFootprints1(Dev *This, const D3D12_RESOURCE_DESC1 *desc, UINT first, UINT count, UINT64 base, D3D12_PLACED_SUBRESOURCE_FOOTPRINT *layouts,
                                                         UINT *rows, UINT64 *row_sizes, UINT64 *total) {
  D3D12_RESOURCE_DESC d;
  if (!desc_of_desc1(desc, &d)) { if (total) *total = (~(UINT64)0); return; }
  get_footprints(&d, first, count, base, layouts, rows, row_sizes, total);
}

/* ---- lists and queues of the later interfaces ------------------------------------------------------------------------------------------------------------------ */

/* A list made closed, with no allocator: Reset gives it one. */
static HRESULT STDMETHODCALLTYPE dev_CreateCommandList1(Dev *This, UINT mask, D3D12_COMMAND_LIST_TYPE type, D3D12_COMMAND_LIST_FLAGS flags, REFIID riid, void **out) {
  if (out) *out = NULL;
  if (flags != D3D12_COMMAND_LIST_FLAG_NONE || mask > 1) return E_INVALIDARG;
  if (type == D3D12_COMMAND_LIST_TYPE_VIDEO_DECODE || type == D3D12_COMMAND_LIST_TYPE_VIDEO_PROCESS || type == D3D12_COMMAND_LIST_TYPE_VIDEO_ENCODE) {
    const HRESULT vh = video_list_make(This, type, riid, out);
    if (SUCCEEDED(vh)) ((CmdList *)*out)->closed = 1;      /* (the interface is the list itself) */
    return vh;
  }
  ID3D12CommandAllocator *allocator = NULL;
  HRESULT hr = dev_CreateCommandAllocator(This, type, &IID_ID3D12CommandAllocator, (void **)&allocator);
  if (FAILED(hr)) return hr;
  ID3D12GraphicsCommandList *list = NULL;
  hr = dev_CreateCommandList(This, mask, type, allocator, NULL, &IID_ID3D12GraphicsCommandList, (void **)&list);
  ID3D12CommandAllocator_Release(allocator);
  if (FAILED(hr)) return hr;
  ID3D12GraphicsCommandList_Close(list);
  hr = ID3D12GraphicsCommandList_QueryInterface(list, riid, out);
  ID3D12GraphicsCommandList_Release(list);
  return hr;
}
/* (the creator's identifier names who made the queue, for tools: it changes nothing here) */
static HRESULT STDMETHODCALLTYPE dev_CreateCommandQueue1(Dev *This, const D3D12_COMMAND_QUEUE_DESC *desc, REFIID creator, REFIID riid, void **out) {
  if (out) *out = NULL;
  if (!creator) return E_INVALIDARG;
  return dev_CreateCommandQueue(This, desc, riid, out);
}

/* ---- residency, fences ---------------------------------------------------------------------------------------------------------------------------------------------- */

/* Every allocation is resident (the kernel does not evict): priorities change nothing, and making things resident is done at once. */
static HRESULT STDMETHODCALLTYPE dev_SetResidencyPriority(Dev *This, UINT count, ID3D12Pageable *const *objects, const D3D12_RESIDENCY_PRIORITY *priorities) {
  return count && (!objects || !priorities) ? E_INVALIDARG : S_OK;
}
static HRESULT STDMETHODCALLTYPE dev_EnqueueMakeResident(Dev *This, D3D12_RESIDENCY_FLAGS flags, UINT count, ID3D12Pageable *const *objects, ID3D12Fence *fence, UINT64 value) {
  if ((count && !objects) || !fence || (flags & ~(UINT)D3D12_RESIDENCY_FLAG_DENY_OVERBUDGET)) return E_INVALIDARG;
  return ID3D12Fence_Signal(fence, value);
}

/* A wait on several fences: all of them reaching their values, or any one. With no event, here; else a thread sets the event. */
typedef struct MultiWait {
  UINT count;
  BOOL any;
  HANDLE event;
  Fence **fences;
  UINT64 *values;
} MultiWait;
static BOOL multi_done(const MultiWait *w) {
  for (UINT i = 0; i < w->count; ++i) {
    const BOOL reached = fence_value(w->fences[i]) >= w->values[i];
    if (w->any && reached) return TRUE;
    if (!w->any && !reached) return FALSE;
  }
  return !w->any || w->count == 0;
}
static void multi_wait(MultiWait *w) {
  if (!w->any) {
    for (UINT i = 0; i < w->count; ++i) if (fence_value(w->fences[i]) < w->values[i]) kmt_wait_cpu(w->fences[i]->d.dev->kdev, w->fences[i]->sync, w->values[i]);
    return;
  }
  while (!multi_done(w)) Sleep(1);      /* (the kernel waits on one synchronization object at a time) */
}
static void multi_free(MultiWait *w) {
  for (UINT i = 0; i < w->count; ++i) dx_Release(OBJ(w->fences[i]));
  mem_free(w->fences);
  mem_free(w->values);
  mem_free(w);
}
static DWORD WINAPI multi_thread(LPVOID param) {
  MultiWait *w = param;
  multi_wait(w);
  SetEvent(w->event);
  multi_free(w);
  return 0;
}
static HRESULT STDMETHODCALLTYPE dev_SetEventOnMultipleFenceCompletion(Dev *This, ID3D12Fence *const *fences, const UINT64 *values, UINT count, D3D12_MULTIPLE_FENCE_WAIT_FLAGS flags,
                                                                       HANDLE event) {
  if ((count && (!fences || !values)) || flags > D3D12_MULTIPLE_FENCE_WAIT_FLAG_ANY || count > (1u << 16)) return E_INVALIDARG;
  MultiWait *w = mem_alloc(sizeof *w);
  if (!w) return E_OUTOFMEMORY;
  memset(w, 0, sizeof *w);
  w->any = flags == D3D12_MULTIPLE_FENCE_WAIT_FLAG_ANY;
  w->event = event;
  w->fences = mem_alloc(sizeof *w->fences * ((SIZE_T)count + 1));
  w->values = mem_alloc(sizeof *w->values * ((SIZE_T)count + 1));
  if (!w->fences || !w->values) { multi_free(w); return E_OUTOFMEMORY; }
  for (UINT i = 0; i < count; ++i) {
    if (!fences[i]) { multi_free(w); return E_INVALIDARG; }
    w->fences[i] = (Fence *)fences[i];
    dx_AddRef(OBJ(fences[i]));
    w->values[i] = values[i];
    ++w->count;
  }
  if (multi_done(w)) { if (event) SetEvent(event); multi_free(w); return S_OK; }
  if (!event) { multi_wait(w); multi_free(w); return S_OK; }
  HANDLE t = CreateThread(NULL, 0, multi_thread, w, 0, NULL);
  if (!t) { multi_free(w); return E_OUTOFMEMORY; }
  CloseHandle(t);
  return S_OK;
}

/* RemoveDevice: the device is lost on purpose. GetDeviceRemovedReason answers DXGI_ERROR_DEVICE_REMOVED from then on, and the device's fences are signalled with
 * (~(UINT64)0), so whatever waits on them goes on. */
static void STDMETHODCALLTYPE dev_RemoveDevice(Dev *This) {
  InterlockedExchange(&This->removed, 1);
  AcquireSRWLockExclusive(&This->fence_lock);
  for (Fence *f = This->fences; f; f = f->next) kmt_signal_cpu(This->kdev, f->sync, (~(UINT64)0));
  ReleaseSRWLockExclusive(&This->fence_lock);
}

/* ---- lifetime trackers ------------------------------------------------------------------------------------------------------------------------------------------- */
/* A tracker tells its owner when an object it was given goes: DestroyOwnedObject lets the object go (its last reference, the program's) and the owner is told it
 * is no longer in use. */
typedef struct Tracker { DObj d; ID3D12LifetimeOwner *owner; } Tracker;
static const GUID *const g_tracker_iids[] = {&IID_ID3D12Object, &IID_ID3D12DeviceChild, &IID_ID3D12LifetimeTracker, NULL};
static void tracker_destroy(DxObj *o) {
  Tracker *t = (Tracker *)o;
  if (t->owner) ID3D12LifetimeOwner_Release(t->owner);
  dobj_child_destroy(o);
}
static HRESULT STDMETHODCALLTYPE dev_CreateLifetimeTracker(Dev *This, ID3D12LifetimeOwner *owner, REFIID riid, void **out) {
  extern const ID3D12LifetimeTrackerVtbl ltrack_vtbl;
  if (out) *out = NULL;
  if (!owner) return E_INVALIDARG;
  Tracker *t = dobj_new(sizeof *t, This, &ltrack_vtbl, g_tracker_iids, tracker_destroy);
  if (!t) return E_OUTOFMEMORY;
  ID3D12LifetimeOwner_AddRef(owner);
  t->owner = owner;
  return query_out((IUnknown *)t, riid, out);
}
static HRESULT STDMETHODCALLTYPE ltrack_DestroyOwnedObject(Tracker *t, ID3D12DeviceChild *object) {
  if (!object) return E_INVALIDARG;
  ID3D12DeviceChild_Release(object);
  ID3D12LifetimeOwner_LifetimeStateUpdated(t->owner, D3D12_LIFETIME_STATE_NOT_IN_USE);
  return S_OK;
}

/* ---- what the device has not ------------------------------------------------------------------------------------------------------------------------------------------ */

static HRESULT STDMETHODCALLTYPE dev_OpenExistingHeapFromAddress(Dev *This, const void *address, REFIID riid, void **out) {
  if (out) *out = NULL;
  return address ? DXGI_ERROR_UNSUPPORTED : E_INVALIDARG;      /* (D3D12_FEATURE_EXISTING_HEAPS: not supported) */
}
static HRESULT STDMETHODCALLTYPE dev_OpenExistingHeapFromFileMapping(Dev *This, HANDLE mapping, REFIID riid, void **out) {
  if (out) *out = NULL;
  return mapping ? DXGI_ERROR_UNSUPPORTED : E_INVALIDARG;
}
static HRESULT STDMETHODCALLTYPE dev_CreateProtectedResourceSession(Dev *This, const D3D12_PROTECTED_RESOURCE_SESSION_DESC *desc, REFIID riid, void **out) {
  if (out) *out = NULL;
  return desc ? DXGI_ERROR_UNSUPPORTED : E_INVALIDARG;      /* (D3D12_FEATURE_PROTECTED_RESOURCE_SESSION_SUPPORT: none) */
}
static HRESULT STDMETHODCALLTYPE dev_CreateProtectedResourceSession1(Dev *This, const D3D12_PROTECTED_RESOURCE_SESSION_DESC1 *desc, REFIID riid, void **out) {
  if (out) *out = NULL;
  return desc ? DXGI_ERROR_UNSUPPORTED : E_INVALIDARG;
}
/* No meta commands: none to list, none to make. */
static HRESULT STDMETHODCALLTYPE dev_EnumerateMetaCommands(Dev *This, UINT *count, D3D12_META_COMMAND_DESC *descs) {
  if (!count) return E_INVALIDARG;
  *count = 0;
  return S_OK;
}
static HRESULT STDMETHODCALLTYPE dev_EnumerateMetaCommandParameters(Dev *This, REFGUID id, D3D12_META_COMMAND_PARAMETER_STAGE stage, UINT *total, UINT *count,
                                                                    D3D12_META_COMMAND_PARAMETER_DESC *descs) {
  if (total) *total = 0;
  if (count) *count = 0;
  return E_INVALIDARG;      /* (no meta command has the identifier) */
}
static HRESULT STDMETHODCALLTYPE dev_CreateMetaCommand(Dev *This, REFGUID id, UINT mask, const void *data, SIZE_T size, REFIID riid, void **out) {
  if (out) *out = NULL;
  return E_INVALIDARG;
}
/* (state objects are raytracing pipelines and collections: D3D12_RAYTRACING_TIER_NOT_SUPPORTED) */
static HRESULT STDMETHODCALLTYPE dev_CreateStateObject(Dev *This, const D3D12_STATE_OBJECT_DESC *desc, REFIID riid, void **out) {
  if (out) *out = NULL;
  return desc ? DXGI_ERROR_UNSUPPORTED : E_INVALIDARG;
}
static HRESULT STDMETHODCALLTYPE dev_AddToStateObject(Dev *This, const D3D12_STATE_OBJECT_DESC *addition, ID3D12StateObject *from, REFIID riid, void **out) {
  if (out) *out = NULL;
  return E_INVALIDARG;      /* (there is no state object to grow from) */
}
static void STDMETHODCALLTYPE dev_GetRaytracingAccelerationStructurePrebuildInfo(Dev *This, const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS *desc,
                                                                                 D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO *info) {
  if (info) memset(info, 0, sizeof *info);      /* (no raytracing: nothing is built) */
}
/* What this device would write into serialized data (it serializes none: acceleration structures need raytracing). */
DEFINE_GUID(g_tcos_driver_guid, 0x7ce3a1d2, 0x43b8, 0x4d5c, 0x9a, 0x1e, 0x54, 0x43, 0x4f, 0x53, 0x44, 0x31);
static D3D12_DRIVER_MATCHING_IDENTIFIER_STATUS STDMETHODCALLTYPE dev_CheckDriverMatchingIdentifier(Dev *This, D3D12_SERIALIZED_DATA_TYPE type,
                                                                                                   const D3D12_SERIALIZED_DATA_DRIVER_MATCHING_IDENTIFIER *id) {
  if (!id || type != D3D12_SERIALIZED_DATA_RAYTRACING_ACCELERATION_STRUCTURE) return D3D12_DRIVER_MATCHING_IDENTIFIER_UNSUPPORTED_TYPE;
  return guid_eq(&id->DriverOpaqueGUID, &g_tcos_driver_guid) ? D3D12_DRIVER_MATCHING_IDENTIFIER_COMPATIBLE_WITH_DEVICE : D3D12_DRIVER_MATCHING_IDENTIFIER_UNRECOGNIZED;
}
/* No background processing to steer (D3D12_FEATURE_DATA_D3D12_OPTIONS6.BackgroundProcessingSupported is FALSE): measurements are done as soon as asked for. */
static HRESULT STDMETHODCALLTYPE dev_SetBackgroundProcessingMode(Dev *This, D3D12_BACKGROUND_PROCESSING_MODE mode, D3D12_MEASUREMENTS_ACTION action, HANDLE event, WINBOOL *more) {
  if (mode > D3D12_BACKGROUND_PROCESSING_MODE_DISABLE_PROFILING_BY_SYSTEM || action > D3D12_MEASUREMENTS_ACTION_DISCARD_PREVIOUS) return E_INVALIDARG;
  if (more) *more = FALSE;
  if (event) SetEvent(event);
  return S_OK;
}
/* (no sampler feedback: the descriptor is left empty) */
static void STDMETHODCALLTYPE dev_CreateSamplerFeedbackUnorderedAccessView(Dev *This, ID3D12Resource *targeted, ID3D12Resource *feedback, D3D12_CPU_DESCRIPTOR_HANDLE handle) {
  SoftDesc d;
  memset(&d, 0, sizeof d);
  desc_write(handle, &d);
}

/* ---- pipeline state streams -------------------------------------------------------------------------------------------------------------------------------------- */
/* A stream is subobjects one after the other, each its type (a UINT) and then its contents at their alignment, the whole aligned to a pointer
 * (CD3DX12_PIPELINE_STATE_STREAM_SUBOBJECT). It is made into a graphics or a compute pipeline description. */

/* A float depth bias (RASTERIZER1 / 2) as the integer of D3D12_RASTERIZER_DESC: the nearest, within an INT (what the description says; the pipeline draws with the float itself,
 * PsoExtras). */
static INT depth_bias_int(FLOAT bias) {
  if (!(bias == bias)) return 0;
  if (bias >= 2147483520.0f) return 0x7FFFFFFF;
  if (bias <= -2147483648.0f) return (INT)0x80000000u;
  return (INT)(bias < 0 ? bias - 0.5f : bias + 0.5f);
}
static const D3D12_BLEND_DESC g_default_blend12 = {FALSE, FALSE, {{FALSE, FALSE, D3D12_BLEND_ONE, D3D12_BLEND_ZERO, D3D12_BLEND_OP_ADD, D3D12_BLEND_ONE, D3D12_BLEND_ZERO, D3D12_BLEND_OP_ADD,
                                                                   D3D12_LOGIC_OP_NOOP, D3D12_COLOR_WRITE_ENABLE_ALL}}};
/* The stream as a graphics or a compute description (`*is_compute`); the descriptions point into the stream. A SERIALIZED_ROOT_SIGNATURE subobject is made into a root
 * signature of `dev`'s, returned in `*made` (NULL when there is none), which the caller releases once the pipeline is made (the pipeline holds its own reference).
 *
 * The subobjects of the later Agility SDKs map to the state the kernel's pipelines have (what the description cannot say goes in `*x`, PsoExtras):
 *   - DEPTH_STENCIL2: front and back faces with their own stencil masks - the kernel pipeline's back_stencil masks, where the device has DEV_FEATURE2_RASTERIZER_STATE
 *     (IndependentFrontAndBackStencilRefMaskSupported, OPTIONS14); on a device without it masks that differ with the stencil test on are E_INVALIDARG, as Direct3D 12
 *     refuses them on such a device.
 *   - RASTERIZER1 / RASTERIZER2: the depth bias as a float, which the kernel pipeline's bias is (D3D12_RASTERIZER_DESC::DepthBias has it rounded); the clamp and the
 *     slope go as they are. RASTERIZER2's line mode is the pair of MultisampleEnable / AntialiasedLineEnable it replaces (aliased, alpha-antialiased, wide quadrilateral);
 *     narrow quadrilateral lines are E_INVALIDARG (NarrowQuadrilateralLinesSupported is FALSE, OPTIONS19).
 *   - one subobject of each kind: DEPTH_STENCIL, DEPTH_STENCIL1 and DEPTH_STENCIL2 are one kind, the three rasterizers another, ROOT_SIGNATURE and SERIALIZED_ROOT_SIGNATURE
 *     a third; a second of a kind is E_INVALIDARG.
 *   - PSDB and the subobject types after it: E_INVALIDARG (a type this runtime does not know). */
static HRESULT stream_parse(Dev *dev, const D3D12_PIPELINE_STATE_STREAM_DESC *stream, D3D12_GRAPHICS_PIPELINE_STATE_DESC *gp, D3D12_COMPUTE_PIPELINE_STATE_DESC *cp, BOOL *is_compute,
                            PsoExtras *x, ID3D12RootSignature **made) {
  *made = NULL;
  memset(x, 0, sizeof *x);
  if (!stream || !stream->pPipelineStateSubobjectStream || stream->SizeInBytes < sizeof(UINT)) return E_INVALIDARG;
  D3D12_GRAPHICS_PIPELINE_STATE_DESC g;
  D3D12_COMPUTE_PIPELINE_STATE_DESC c;
  memset(&g, 0, sizeof g);
  memset(&c, 0, sizeof c);
  /* (the defaults of the subobjects a stream leaves out: CD3DX12's D3D12_DEFAULT ones, a mask of all samples, one sample) */
  g.BlendState = g_default_blend12;
  for (UINT i = 1; i < 8; ++i) g.BlendState.RenderTarget[i] = g.BlendState.RenderTarget[0];
  g.SampleMask = UINT_MAX;
  g.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
  g.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;
  g.RasterizerState.DepthClipEnable = TRUE;
  g.DepthStencilState.DepthEnable = TRUE;
  g.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
  g.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
  g.DepthStencilState.StencilReadMask = g.DepthStencilState.StencilWriteMask = 0xFF;
  const D3D12_DEPTH_STENCILOP_DESC keep = {D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP, D3D12_COMPARISON_FUNC_ALWAYS};
  g.DepthStencilState.FrontFace = g.DepthStencilState.BackFace = keep;
  g.SampleDesc.Count = 1;
  const BYTE *base = stream->pPipelineStateSubobjectStream;
  const SIZE_T size = stream->SizeInBytes;
  UINT seen = 0;
  BOOL compute = FALSE, graphics = FALSE;
  SIZE_T at = 0;
  const UINT depth_kinds = 1u << D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL | 1u << D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL1 |
                           1u << D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL2;
  const UINT raster_kinds = 1u << D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER | 1u << D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER1 |
                            1u << D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER2;
  const UINT root_kinds = 1u << D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE | 1u << D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SERIALIZED_ROOT_SIGNATURE;
  HRESULT hr = S_OK;
#define STREAM_FAIL(code) do { hr = (code); goto fail; } while (0)
  while (at + sizeof(UINT) <= size) {
    UINT type;
    memcpy(&type, base + at, 4);
    if (type >= 32 || (seen >> type & 1)) STREAM_FAIL(E_INVALIDARG);      /* (each subobject at most once) */
    const UINT kinds = (depth_kinds >> type & 1) ? depth_kinds : (raster_kinds >> type & 1) ? raster_kinds : (root_kinds >> type & 1) ? root_kinds : 1u << type;
    if (seen & kinds) STREAM_FAIL(E_INVALIDARG);      /* (one of each kind) */
    seen |= 1u << type;
    SIZE_T inner = 0, align = 4;
    switch (type) {
      case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE: inner = sizeof(void *); align = sizeof(void *); break;
      case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS: case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS: case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DS: case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_HS:
      case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_GS: case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CS: case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_AS: case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS:
        inner = sizeof(D3D12_SHADER_BYTECODE); align = sizeof(void *); break;
      case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_STREAM_OUTPUT: inner = sizeof(D3D12_STREAM_OUTPUT_DESC); align = sizeof(void *); break;
      case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_BLEND: inner = sizeof(D3D12_BLEND_DESC); break;
      case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_MASK: case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_NODE_MASK: case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_IB_STRIP_CUT_VALUE:
      case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PRIMITIVE_TOPOLOGY: case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL_FORMAT: case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_FLAGS:
        inner = 4; break;
      case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER: inner = sizeof(D3D12_RASTERIZER_DESC); break;
      case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL: inner = sizeof(D3D12_DEPTH_STENCIL_DESC); break;
      case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL1: inner = sizeof(D3D12_DEPTH_STENCIL_DESC1); break;
      case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_INPUT_LAYOUT: inner = sizeof(D3D12_INPUT_LAYOUT_DESC); align = sizeof(void *); break;
      case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS: inner = sizeof(struct D3D12_RT_FORMAT_ARRAY); break;
      case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC: inner = sizeof(DXGI_SAMPLE_DESC); break;
      case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CACHED_PSO: inner = sizeof(D3D12_CACHED_PIPELINE_STATE); align = sizeof(void *); break;
      case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VIEW_INSTANCING: inner = sizeof(D3D12_VIEW_INSTANCING_DESC); align = sizeof(void *); break;
      case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL2: inner = sizeof(D3D12_DEPTH_STENCIL_DESC2); break;
      case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER1: inner = sizeof(D3D12_RASTERIZER_DESC1); break;
      case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER2: inner = sizeof(D3D12_RASTERIZER_DESC2); break;
      case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SERIALIZED_ROOT_SIGNATURE: inner = sizeof(D3D12_SERIALIZED_ROOT_SIGNATURE_DESC); align = sizeof(void *); break;
      default: STREAM_FAIL(E_INVALIDARG);
    }
    const SIZE_T data_at = (at + 4 + align - 1) & ~(align - 1), next = (data_at + inner + sizeof(void *) - 1) & ~(sizeof(void *) - 1);
    if (data_at > size || inner > size - data_at) STREAM_FAIL(E_INVALIDARG);
    const void *p = base + data_at;
    switch (type) {
      case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE: memcpy(&g.pRootSignature, p, sizeof(void *)); c.pRootSignature = g.pRootSignature; break;
      case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS: memcpy(&g.VS, p, inner); graphics = TRUE; break;
      case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS: memcpy(&g.PS, p, inner); graphics = TRUE; break;
      case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DS: memcpy(&g.DS, p, inner); graphics = TRUE; break;
      case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_HS: memcpy(&g.HS, p, inner); graphics = TRUE; break;
      case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_GS: memcpy(&g.GS, p, inner); graphics = TRUE; break;
      case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CS: memcpy(&c.CS, p, inner); compute = TRUE; break;
      case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_AS: case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS: {
        D3D12_SHADER_BYTECODE s;
        memcpy(&s, p, inner);
        if (s.pShaderBytecode && s.BytecodeLength) STREAM_FAIL(E_INVALIDARG);      /* (no mesh shaders: D3D12_MESH_SHADER_TIER_NOT_SUPPORTED) */
        break;
      }
      case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_STREAM_OUTPUT: memcpy(&g.StreamOutput, p, inner); break;
      case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_BLEND: memcpy(&g.BlendState, p, inner); break;
      case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_MASK: memcpy(&g.SampleMask, p, 4); break;
      case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER: memcpy(&g.RasterizerState, p, inner); break;
      case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL: memcpy(&g.DepthStencilState, p, inner); break;
      case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL1: {
        D3D12_DEPTH_STENCIL_DESC1 d1;
        memcpy(&d1, p, inner);
        if (d1.DepthBoundsTestEnable) STREAM_FAIL(E_INVALIDARG);      /* (DepthBoundsTestSupported is FALSE) */
        g.DepthStencilState.DepthEnable = d1.DepthEnable;
        g.DepthStencilState.DepthWriteMask = d1.DepthWriteMask;
        g.DepthStencilState.DepthFunc = d1.DepthFunc;
        g.DepthStencilState.StencilEnable = d1.StencilEnable;
        g.DepthStencilState.StencilReadMask = d1.StencilReadMask;
        g.DepthStencilState.StencilWriteMask = d1.StencilWriteMask;
        g.DepthStencilState.FrontFace = d1.FrontFace;
        g.DepthStencilState.BackFace = d1.BackFace;
        break;
      }
      case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_INPUT_LAYOUT: memcpy(&g.InputLayout, p, inner); break;
      case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_IB_STRIP_CUT_VALUE: memcpy(&g.IBStripCutValue, p, 4); break;
      case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PRIMITIVE_TOPOLOGY: memcpy(&g.PrimitiveTopologyType, p, 4); break;
      case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS: {
        struct D3D12_RT_FORMAT_ARRAY f;
        memcpy(&f, p, inner);
        if (f.NumRenderTargets > 8) STREAM_FAIL(E_INVALIDARG);
        g.NumRenderTargets = f.NumRenderTargets;
        memcpy(g.RTVFormats, f.RTFormats, sizeof g.RTVFormats);
        break;
      }
      case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL_FORMAT: memcpy(&g.DSVFormat, p, 4); break;
      case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC: memcpy(&g.SampleDesc, p, inner); break;
      case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_NODE_MASK: memcpy(&g.NodeMask, p, 4); c.NodeMask = g.NodeMask; break;
      case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CACHED_PSO: memcpy(&g.CachedPSO, p, inner); c.CachedPSO = g.CachedPSO; break;
      case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_FLAGS: memcpy(&g.Flags, p, 4); c.Flags = g.Flags; break;
      case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VIEW_INSTANCING: {
        D3D12_VIEW_INSTANCING_DESC v;
        memcpy(&v, p, inner);
        if (v.ViewInstanceCount > 1) STREAM_FAIL(E_INVALIDARG);      /* (D3D12_VIEW_INSTANCING_TIER_NOT_SUPPORTED) */
        break;
      }
      case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL2: {
        D3D12_DEPTH_STENCIL_DESC2 d2;
        memcpy(&d2, p, inner);
        if (d2.DepthBoundsTestEnable) STREAM_FAIL(E_INVALIDARG);      /* (DepthBoundsTestSupported is FALSE) */
        if (d2.StencilEnable && (d2.FrontFace.StencilReadMask != d2.BackFace.StencilReadMask || d2.FrontFace.StencilWriteMask != d2.BackFace.StencilWriteMask) &&
            !(dev->caps.features2 & DEV_FEATURE2_RASTERIZER_STATE))
          STREAM_FAIL(E_INVALIDARG);      /* (IndependentFrontAndBackStencilRefMaskSupported is FALSE there) */
        x->has_back_masks = 1;
        x->back_read_mask = d2.BackFace.StencilReadMask;
        x->back_write_mask = d2.BackFace.StencilWriteMask;
        g.DepthStencilState.DepthEnable = d2.DepthEnable;
        g.DepthStencilState.DepthWriteMask = d2.DepthWriteMask;
        g.DepthStencilState.DepthFunc = d2.DepthFunc;
        g.DepthStencilState.StencilEnable = d2.StencilEnable;
        g.DepthStencilState.StencilReadMask = d2.FrontFace.StencilReadMask;
        g.DepthStencilState.StencilWriteMask = d2.FrontFace.StencilWriteMask;
        const D3D12_DEPTH_STENCILOP_DESC1 *faces[2] = {&d2.FrontFace, &d2.BackFace};
        D3D12_DEPTH_STENCILOP_DESC *to[2] = {&g.DepthStencilState.FrontFace, &g.DepthStencilState.BackFace};
        for (UINT f = 0; f < 2; ++f) {
          to[f]->StencilFailOp = faces[f]->StencilFailOp;
          to[f]->StencilDepthFailOp = faces[f]->StencilDepthFailOp;
          to[f]->StencilPassOp = faces[f]->StencilPassOp;
          to[f]->StencilFunc = faces[f]->StencilFunc;
        }
        break;
      }
      case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER1: {
        D3D12_RASTERIZER_DESC1 r1;
        memcpy(&r1, p, inner);
        D3D12_RASTERIZER_DESC *r = &g.RasterizerState;
        r->FillMode = r1.FillMode; r->CullMode = r1.CullMode; r->FrontCounterClockwise = r1.FrontCounterClockwise;
        r->DepthBias = depth_bias_int(r1.DepthBias); r->DepthBiasClamp = r1.DepthBiasClamp; r->SlopeScaledDepthBias = r1.SlopeScaledDepthBias;
        x->has_bias = 1;
        x->depth_bias = r1.DepthBias;
        r->DepthClipEnable = r1.DepthClipEnable; r->MultisampleEnable = r1.MultisampleEnable; r->AntialiasedLineEnable = r1.AntialiasedLineEnable;
        r->ForcedSampleCount = r1.ForcedSampleCount; r->ConservativeRaster = r1.ConservativeRaster;
        break;
      }
      case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER2: {
        D3D12_RASTERIZER_DESC2 r2;
        memcpy(&r2, p, inner);
        if ((UINT)r2.LineRasterizationMode > D3D12_LINE_RASTERIZATION_MODE_QUADRILATERAL_NARROW) STREAM_FAIL(E_INVALIDARG);
        if (r2.LineRasterizationMode == D3D12_LINE_RASTERIZATION_MODE_QUADRILATERAL_NARROW) STREAM_FAIL(E_INVALIDARG);      /* (NarrowQuadrilateralLinesSupported is FALSE) */
        D3D12_RASTERIZER_DESC *r = &g.RasterizerState;
        r->FillMode = r2.FillMode; r->CullMode = r2.CullMode; r->FrontCounterClockwise = r2.FrontCounterClockwise;
        r->DepthBias = depth_bias_int(r2.DepthBias); r->DepthBiasClamp = r2.DepthBiasClamp; r->SlopeScaledDepthBias = r2.SlopeScaledDepthBias;
        x->has_bias = 1;
        x->depth_bias = r2.DepthBias;
        r->DepthClipEnable = r2.DepthClipEnable;
        r->MultisampleEnable = r2.LineRasterizationMode == D3D12_LINE_RASTERIZATION_MODE_QUADRILATERAL_WIDE;
        r->AntialiasedLineEnable = r2.LineRasterizationMode == D3D12_LINE_RASTERIZATION_MODE_ALPHA_ANTIALIASED;
        r->ForcedSampleCount = r2.ForcedSampleCount; r->ConservativeRaster = r2.ConservativeRaster;
        break;
      }
      case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SERIALIZED_ROOT_SIGNATURE: {
        D3D12_SERIALIZED_ROOT_SIGNATURE_DESC sr;
        memcpy(&sr, p, inner);
        if (!sr.pSerializedBlob || !sr.SerializedBlobSizeInBytes) STREAM_FAIL(E_INVALIDARG);
        hr = dev_CreateRootSignature(dev, 0, sr.pSerializedBlob, sr.SerializedBlobSizeInBytes, &IID_ID3D12RootSignature, (void **)made);
        if (FAILED(hr)) goto fail;
        g.pRootSignature = c.pRootSignature = *made;
        break;
      }
    }
    at = next;
  }
  if (compute == graphics) STREAM_FAIL(E_INVALIDARG);      /* (a compute shader alone, or the stages of a graphics pipeline) */
  *gp = g;
  *cp = c;
  *is_compute = compute;
  return S_OK;
fail:
  if (*made) { ID3D12RootSignature_Release(*made); *made = NULL; }
  return hr;
#undef STREAM_FAIL
}
static HRESULT pipeline_stream(Dev *This, const D3D12_PIPELINE_STATE_STREAM_DESC *stream, REFIID riid, void **out) {
  D3D12_GRAPHICS_PIPELINE_STATE_DESC g;
  D3D12_COMPUTE_PIPELINE_STATE_DESC c;
  BOOL compute;
  PsoExtras x;
  ID3D12RootSignature *made;
  const HRESULT hr = stream_parse(This, stream, &g, &c, &compute, &x, &made);
  if (FAILED(hr)) return hr;
  const HRESULT created = compute ? dev_CreateComputePipelineState(This, &c, riid, out) : pso_create_graphics(This, &g, &x, riid, out);
  if (made) ID3D12RootSignature_Release(made);
  return created;
}
static HRESULT STDMETHODCALLTYPE dev_CreatePipelineState(Dev *This, const D3D12_PIPELINE_STATE_STREAM_DESC *desc, REFIID riid, void **out) {
  if (out) *out = NULL;
  return pipeline_stream(This, desc, riid, out);
}

/* ---- the later versions of the fence, resource and heap -------------------------------------------------------------------------------------------------------------- */

static D3D12_FENCE_FLAGS STDMETHODCALLTYPE fence_GetCreationFlags(Fence *This) { return This->flags; }
/* (no protected resource sessions: a resource and a heap have none, which Direct3D 12 answers with DXGI_ERROR_NOT_FOUND and NULL) */
static HRESULT STDMETHODCALLTYPE res_GetProtectedResourceSession(Resource *This, REFIID riid, void **out) {
  if (!out) return E_INVALIDARG;
  *out = NULL;
  return DXGI_ERROR_NOT_FOUND;
}
static HRESULT STDMETHODCALLTYPE heap_GetProtectedResourceSession(HeapObj *This, REFIID riid, void **out) {
  if (!out) return E_INVALIDARG;
  *out = NULL;
  return DXGI_ERROR_NOT_FOUND;
}
/* GetDesc1: the description with the fields D3D12_RESOURCE_DESC1 adds - no sampler feedback mip region (sampler feedback is not supported). */
static D3D12_RESOURCE_DESC1 *STDMETHODCALLTYPE res_GetDesc1(Resource *This, D3D12_RESOURCE_DESC1 *out) {
  memset(out, 0, sizeof *out);
  out->Dimension = This->desc.Dimension;
  out->Alignment = This->desc.Alignment;
  out->Width = This->desc.Width;
  out->Height = This->desc.Height;
  out->DepthOrArraySize = This->desc.DepthOrArraySize;
  out->MipLevels = This->desc.MipLevels;
  out->Format = This->desc.Format;
  out->SampleDesc = This->desc.SampleDesc;
  out->Layout = This->desc.Layout;
  out->Flags = This->desc.Flags;
  return out;
}

/* ---- ID3D12Device11 .. 14 ------------------------------------------------------------------------------------------------------------------------------------------------ */

/* CreateSampler2: the sampler of D3D12_SAMPLER_DESC2 - D3D12_SAMPLER_DESC with an integer border colour (D3D12_SAMPLER_FLAG_UINT_BORDER_COLOR: the colour is taken
 * as transparent black, opaque black or opaque white by the same rule as a float one, a channel that is not 0 counting as 1) or coordinates in texels, which no
 * device here samples with (NonNormalizedCoordinateSamplersSupported is FALSE, OPTIONS17): such a call, and one with flags that do not exist, is left out, as an
 * invalid call is. */
static void STDMETHODCALLTYPE dev_CreateSampler2(Dev *This, const D3D12_SAMPLER_DESC2 *desc, D3D12_CPU_DESCRIPTOR_HANDLE handle) {
  if (!desc) { dev_CreateSampler(This, NULL, handle); return; }
  if ((desc->Flags & ~(UINT)D3D12_SAMPLER_FLAG_UINT_BORDER_COLOR)) return;
  D3D12_SAMPLER_DESC d;
  d.Filter = desc->Filter;
  d.AddressU = desc->AddressU;
  d.AddressV = desc->AddressV;
  d.AddressW = desc->AddressW;
  d.MipLODBias = desc->MipLODBias;
  d.MaxAnisotropy = desc->MaxAnisotropy;
  d.ComparisonFunc = desc->ComparisonFunc;
  for (UINT c = 0; c < 4; ++c)
    d.BorderColor[c] = (desc->Flags & D3D12_SAMPLER_FLAG_UINT_BORDER_COLOR) ? (desc->UintBorderColor[c] ? 1.0f : 0.0f) : desc->FloatBorderColor[c];
  d.MinLOD = desc->MinLOD;
  d.MaxLOD = desc->MaxLOD;
  dev_CreateSampler(This, &d, handle);
}
/* GetResourceAllocationInfo3: GetResourceAllocationInfo2 with castable formats - which need RelaxedFormatCastingSupported (FALSE): a description with any is one that
 * cannot be had (a size of ~0). */
static D3D12_RESOURCE_ALLOCATION_INFO *STDMETHODCALLTYPE dev_GetResourceAllocationInfo3(Dev *This, D3D12_RESOURCE_ALLOCATION_INFO *ret, UINT mask, UINT count,
                                                                                          const D3D12_RESOURCE_DESC1 *descs, const UINT32 *castable_counts,
                                                                                          const DXGI_FORMAT *const *castable, D3D12_RESOURCE_ALLOCATION_INFO1 *each) {
  allocation_infos(This, count, NULL, descs, ret, each);
  for (UINT i = 0; castable_counts && i < count; ++i)
    if (castable_counts[i]) { ret->SizeInBytes = ~(UINT64)0; break; }
  return ret;
}
/* OpenExistingHeapFromAddress1: as OpenExistingHeapFromAddress, of `size` bytes (D3D12_FEATURE_EXISTING_HEAPS: not supported). */
static HRESULT STDMETHODCALLTYPE dev_OpenExistingHeapFromAddress1(Dev *This, const void *address, SIZE_T size, REFIID riid, void **out) {
  if (out) *out = NULL;
  return address && size ? DXGI_ERROR_UNSUPPORTED : E_INVALIDARG;
}
/* CreateRootSignatureFromSubobjectInLibrary: the root signature of a DXIL library's GlobalRootSignature / LocalRootSignature subobject of that name (rootsig.inc). */
static HRESULT STDMETHODCALLTYPE dev_CreateRootSignatureFromSubobjectInLibrary(Dev *This, UINT mask, const void *library, SIZE_T size, LPCWSTR name, REFIID riid, void **out) {
  const BYTE *bytes;
  SIZE_T n;
  if (out) *out = NULL;
  if (!rs_library_subobject(library, size, name, &bytes, &n)) return E_INVALIDARG;
  return dev_CreateRootSignature(This, mask, bytes, n, riid, out);
}
