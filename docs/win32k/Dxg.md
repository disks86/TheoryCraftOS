# win32u Direct3D services (NtGdiDdDDI*, NtDxgk*)

Code: `src/Process/Win32k/Dxg/` (`Win32kDxg.cpp` installs; `DxgAdapters`, `DxgAllocations`, `DxgSync`, `DxgWork`, `DxgUnsupported`, `DxgCore`; structure
offsets in `DxgLayouts.hpp`). Test: `tests/Win32kDxgTest.cpp` (`tests/run_host_tests.sh Win32kDxgTest`, ASan + UBSan). The services map the D3DKMT thunks of gdi32 /
d3d9 / dxgi onto `dxgk::Kernel` (`dxgk::system_kernel()`; without one every service answers STATUS_NO_SUCH_DEVICE).

Rules: all handlers register with `locked = false` (dxgk has its own lock). One dxgk process per caller (`Call::pid()`), opened on first use, closed by the
process-exit hook. Every structure is read once into a `UserBlock` (Windows x64 layout, offsets in `DxgLayouts.hpp`, checked against the SDK headers in
`tests/windows/wine/include`); pointers and counts inside are bounds-checked (arrays <= 32/256 entries, blobs <= 64 KiB, wrapping address arithmetic refused). A NULL
structure pointer is STATUS_INVALID_PARAMETER, an unreadable one STATUS_ACCESS_VIOLATION, a zero handle STATUS_INVALID_PARAMETER. dxgk statuses pass through.
Blocking waits use neither the system lock nor a dxgk wait hook: they poll `wait_sync_cpu(timeout 0)` and sleep in the host, and end when the process exits.

## Implemented (50)

Adapters and devices: OpenAdapterFromLuid, OpenAdapterFromDeviceName (name of adapter n: `\\?\DISPLAY#TCOSDXG#<n>#{1ca05180-a699-450a-9a0c-de4fbe3ddd89}`),
CloseAdapter, EnumAdapters, EnumAdapters2, NtDxgkEnumAdapters3, QueryAdapterInfo (UMDRIVERPRIVATE = adapter caps blob, GETSEGMENTSIZE, ADAPTERGUID, DRIVERVERSION = WDDM 1.3,
ADAPTERTYPE, PHYSICALADAPTERCOUNT, all-zero capability queries; other types STATUS_INVALID_PARAMETER), QueryVideoMemoryInfo (own process only), CreateDevice (no
device-level DMA buffers), DestroyDevice, GetDeviceState (execution, reset), GetDisplayModeList, SetDisplayMode (the mode with the size of the primary allocation),
WaitForVerticalBlankEvent (fixed 60 Hz), SetQueuedLimit (recorded), WaitForIdle, TrimProcessCommitment, SetVidPnSourceOwner and CheckVidPnExclusiveOwnership (semantics and
statuses as Windows has them, from Wine's d3dkmt test), Evict.

Contexts and work: CreateContext (maps a 96 KiB region: 64 KiB DMA buffer = the command stream of CommandStream.hpp, 1024 allocation-list entries, an unused patch list),
CreateContextVirtual (no buffers, cannot Render), DestroyContext, Get/SetContextSchedulingPriority and the InProcess pair (**recorded only**: dxgk cannot change a context's
priority), Render (copies from the mapped buffer), Present (flip, or blt without stretching; a flip with `hWindow` naming a window the desktop composites goes into that window's
surface at its client area instead of onto the display: `present_to_window`), Escape (DRIVERPRIVATE = the dxgk escape ABI, reply in the same buffer).

Allocations: CreateAllocation(2), DestroyAllocation(2), Lock(2), Unlock(2), QueryAllocationResidency, QueryResourceInfo, OpenResource(2).

Synchronization: CreateSynchronizationObject(2), DestroySynchronizationObject, Signal/WaitForSynchronizationObjectFromCpu, SignalSynchronizationObject,
WaitForSynchronizationObject, Signal/WaitForSynchronizationObjectFromGpu.

### Adapter capabilities (QueryAdapterInfo type 0, UMDRIVERPRIVATE)

112 bytes: 8 words (max texture dimension, vertex attributes, varyings, texture units, push constant bytes, vertex stride, flags, `DeviceFeature` bits), the adapter's
name (64 bytes), then 2 words (colour targets, vertex buffers), then 4 words added for the Vulkan driver (words 26..29): multisample counts (`DeviceInfo::sample_counts`), kind of device (0 CPU renderer, 1 virtual GPU, 2 GPU), PCI vendor
and a device number (`Adapter::Adapter` finds the vendor from the device's name: Intel 8086, AMD 1002, VMware 15AD, virtio 1AF4, Broadcom 14E4, the CPU renderer 10005). A caller
that asks for 96 bytes gets the first 96. [WindowsVulkan.md](../WindowsVulkan.md) is the user of all of it.

gdi32.dll has a `D3DKMT*` thunk for every `NtGdiDdDDI*` service (before the Vulkan driver only 47 existed and `D3DKMTCreateContext`, `Render`, `Lock`, `Present` ... were stubs that
answered success without doing anything).

### Private driver data of an allocation (the miniport ABI)

18 little-endian words (`AllocWord`): version (1), dxgk flags, format (`dxgk::Format`), width, height, mips, size lo/hi, texture kind, samples, layers, initial state,
heap handle, heap offset lo/hi; **out**: pitch, backing bytes lo/hi (CreateAllocation writes them back; OpenResource hands the same data to other processes). Smaller blobs
are STATUS_INVALID_PARAMETER. `Primary` adds kAllocPresent, CreateShared adds kAllocShared.

### Direct3D 12 through Escape and Render

d3d12.dll (`docs/WindowsUserMode.md`) needs no extra service: `NtGdiDdDDIEscape` (driver private data, `DxgKernel.hpp` `EscapeOp`) carries shaders and pipelines (1 .. 5) and the Direct3D 12 objects:
`CreateRootSignature` 6, `DestroyRootSignature` 7, `CreateDescriptorHeap` 8, `DestroyDescriptorHeap` 9, `WriteDescriptors` 10 (up to 4096 descriptors a call, 14 words each), `CopyDescriptors` 11,
`UpdateTileMappings` 12 and `QuerySync` 13 (the value of a fence or the count of a semaphore: the kernel has no mapped fence value). The command stream has two ops beyond the draws:
`CopyBuffer` and `CopyBufferImage`, run on the CPU copies of the allocations after the device work before them (a pass may not be open; the ranges are checked; a heap, reserved or query
buffer is refused).

### What the device takes

`graphics::DeviceInfo::limits` (`DeviceLimits`: vertex attributes, varyings, texture units, colour targets, vertex buffers) is what a graphics device says it can do; the `kMax*` constants of
`IGraphicsDevice.hpp` are only the capacity of the shared structures (16, 16, 16, 8, 8), and `kBase*` (4, 4, 2, 4, 1) what the fixed-pipeline backends were made for. `Adapter` copies the
limits into `AdapterCaps` (the UMDRIVERPRIVATE blob has the colour targets and vertex buffers after the name) and holds pipelines, shaders (the texture units and vertex inputs a DXBC program declares,
its colour outputs) and stream ops (texture units, extra pass targets, vertex buffer slots) to them. The escape `CreatePipeline` has an optional last part for attributes beyond four, vertex
buffer slots and per-slot strides, and the blend state of targets 4 .. 7; `BindVertexBuffer` has an optional third word, the slot; `ICommandBuffer::bind_vertex_buffer_slot` is the device side.
TGSI is emitted with the device's number of interpolants (the vertex and pixel shaders of a pipeline must agree on them).

### Mapping notes

- Lock: dxgk's backing is not in the process, so Lock maps a region of the process's address space with a copy; Unlock copies back unless ReadOnly. `PrivateDriverData`
  is the subresource (mip level). Needs an `AddressSpace` (else STATUS_NOT_SUPPORTED). Destroying the allocation, the device or the process drops the view.
- Resources and global shares live in this layer: `hResource` = 0xE0000000 | n; `hGlobalShare` = a layer id; one dxgk share token per allocation; runtime and driver data
  are kept and returned by QueryResourceInfo / OpenResource. The share ends when the owner destroys the resource or exits.
- Sync: a mutex is a semaphore with limit 1. The legacy Signal/Wait (no value) are for semaphores and mutexes (a fence gives STATUS_INVALID_PARAMETER). Not available
  (STATUS_NOT_SUPPORTED): CPU-notification and monitored fences (need an NT event / a mapped fence value), Shared / NtSecuritySharing / CrossAdapter objects, WaitFromCpu with
  `hAsyncEvent`.

## Answer STATUS_NOT_SUPPORTED (72)

Hardware features the adapters lack (`DxgUnsupported.cpp`): multi-plane overlay and overlays, output duplication, protected sessions, WDDM 2 hardware contexts and queues,
paging queues, GPU virtual addresses, SubmitCommand, doorbells, tracked workloads, native fences, present-to-HW-queue, Miracast (NetDisp*), display mux, DisplayPort, Vail.

## Left unregistered (STATUS_NOT_IMPLEMENTED)

OpenAdapterFromHdc (needs HDC -> display from the GDI module), ShareObjects / OpenResourceFromNtHandle / named NT-handle services / keyed mutexes (no NT-handle object for
dxgk objects), OpenSynchronizationObject (no sync sharing in dxgk), MakeResident and CreatePagingQueue (the paging queue needs a fence address), QueryStatistics,
SignalSynchronizationObjectFromGpu2, services whose layout is not in the headers at hand (Get/SetProperties, IsFeatureEnabled, PinResources, ...), swap chain and DispMgr
services, GetPresentHistory, allocation priorities, and the DirectDraw fullscreen-sprite / QueryVisRgnUniqueness services (this service list has no other legacy DDraw
calls). Missing in `dxgk::Kernel`: a context priority setter, sync-object sharing, a way to map allocation memory into a process.
