# Venus: the host's Vulkan driver for Windows and Linux programs

When the machine is QEMU with `-device virtio-gpu-gl-pci,venus=on,blob=on,hostmem=<size>` (virglrenderer built with Venus, a host Vulkan driver), Vulkan
programs in this OS run on the host's own driver: every call goes to the host as a command of the [Venus protocol](https://gitlab.freedesktop.org/virgl/venus-protocol),
which virglrenderer's `vkr` decodes and calls the host's Vulkan with. There are two ways in, and one kernel object behind both:

| | What runs in the program | How it reaches the kernel |
|---|---|---|
| Our driver's Venus backend | `vulkan-1.dll` (Windows) and `libvulkan.a` / `libvulkan.so.1` / `libvulkan_tcos.so` (Linux), `src/Vulkan` | Windows: `NtTcosVirtGpu` (service 514); Linux: `/dev/dri/renderD128` |
| Mesa's venus driver, unchanged | a Linux program with Mesa's `libvulkan_virtio.so` (and libdrm) | `/dev/dri/renderD128`, Linux's virtio-gpu DRM interface |

## Kernel

* **The device** (`src/Hardware/GraphicsProcessingUnit/Virtio/VirtioGpu.*`): negotiates `VIRTIO_GPU_F_RESOURCE_BLOB` and `VIRTIO_GPU_F_CONTEXT_INIT`, reads every
  capability set (`capsets()`, `capset_info()`, `read_capset()`), finds the host-visible shared memory region (virtio PCI capability type 8, id 1:
  `VirtioPciDevice::shared_memory`), and has contexts of a chosen type (`create_context(capset)`), blob resources (`create_blob`, `map_blob` into the window with
  `Utility/RangeAllocator.hpp` handing out its pages, `unmap_blob`), 3D resources with guest backing (`create_resource_3d`, `transfer_3d`) and context submissions
  (`submit`, or `submit_async` with a fence that `fence_signaled` / `wait_fence` look at: 32 slots, descriptors `2 + 2 * slot`, so a fenced submission does not hold the
  control queue that the display also uses). The boot log says `virtio-gpu capsets 0x16, context types, blobs, host-visible window 1024 MiB: Venus (host Vulkan) available`.
* **The session** (`src/Graphics/VirtGpu/VirtGpuSession.*`): one opening of the render node, what Linux's `virtio_gpu_fpriv` is. Every DRM and virtio-gpu ioctl a
  render node answers, with Linux 6.12's checks and error numbers: `VERSION` (`virtio_gpu`), `GET_CAP`, `GEM_CLOSE`, `PRIME_HANDLE_TO_FD` / `PRIME_FD_TO_HANDLE`,
  `VIRTGPU_GETPARAM`, `GET_CAPS`, `CONTEXT_INIT` (capset, rings, poll mask, debug name), `RESOURCE_CREATE_BLOB` (guest, host 3D and host 3D over guest pages; host
  blobs are mapped into the window at creation unless `DEFER_MAPPING`), `RESOURCE_CREATE`, `RESOURCE_INFO`, `TRANSFER_TO/FROM_HOST`, `WAIT`, `MAP` (mmap offsets from
  4 GiB), `EXECBUFFER` (every submission fenced; the buffers it names remember the fence; in and out fence descriptors). A buffer object (`Buffer`) is reference counted
  and is the `SharedMemory` of its mappings, so it lives as long as a handle, a descriptor or a mapping holds it. The device behind it is an `IVirtGpuHost`
  (`VirtioVirtGpuHost` over `VirtioGpu`; host tests have a pretend one), set at boot when the virtio-gpu has 3D (`set_system_host`), as Linux has a render node only then.
* **Windows** (`src/Process/WindowsVirtGpu.*`): `NtTcosVirtGpu(ULONG64 args[8])` with `OPEN`, `CLOSE`, `IOCTL` (any of the ioctls above; the answer is Linux's errno),
  `MAP` and `UNMAP` (`src/Process/VirtGpuDevice.h` has the layout). Sessions belong to the process and go when it ends. The fence and PRIME descriptors have no Windows
  meaning and are `EOPNOTSUPP` there.
* **Linux** (`src/Process/LinuxRenderNode.*`): `/dev/dri` with `renderD128` (character device 226:128; dup, fork and SCM_RIGHTS share the session), mmap of
  buffer objects with the host's cache mode, read()/poll() of ring fence events; sync_file descriptors (poll, `SYNC_IOC_MERGE`, `SYNC_IOC_FILE_INFO`,
  `SYNC_IOC_SET_DEADLINE`, taken back as in fences); dma-buf descriptors (size by lseek, mmap, poll, `DMA_BUF_IOCTL_SYNC`, `SET_NAME`, export/import of sync
  files); and the part of `/sys` libdrm reads to find a PCI DRM device (the PCI function with its config space, `/sys/dev/char/226:128`, `/sys/class/drm`,
  `/sys/bus/pci/devices`, with Linux's links). The boot code hands it the virtio-gpu's PCI function (`set_render_node_pci`).
* **Caching.** A host blob's memory is mapped the way the host says (`map_info`: cached, write-combined or uncached; `MemoryCache` of
  `AddressSpace::map_shared`, the PAT bits in `UserPageTables`): VRAM that a host GPU exports comes back write-combined, and write-back cache lines over it
  read stale data.

## The driver's Venus backend (`src/Vulkan`)

The driver has two backends and one set of exported entry points:

* `vk_names.h` (generated) renames every function of the Direct3D backend (the existing driver over the kernel's Direct3D layer) and of the platforms to `dx_vk*`.
  The platform includes it before the Khronos headers.
* `vk_venus.inc` is the Venus backend: the protocol's encoder and decoder (`venus-protocol/vn_cs.h`), the ring (`venus-protocol/vn_ring.h` is the interface the generated
  code calls), the render node, the objects, and the commands that need more than forwarding. `vn_commands.inc` (generated) forwards the rest. `venus-protocol/` is
  the generated protocol (venus-protocol v1.1.3, MIT; see its README).
* `vk_dispatch.inc` (generated, included last by the platform) exports every core 1.0 - 1.3, surface and swapchain function: each calls `vn_vk*` when the process runs
  on Venus and `dx_vk*` otherwise. `vn_procaddr.inc` is `vkGetInstanceProcAddr` / `vkGetDeviceProcAddr` for Venus (core functions up to the instance's or device's
  version, extension functions when the extension is offered or enabled).
* `vn_selected()` decides once per process: Venus when the render node opens and its host offers the Venus context type with the wire format this protocol speaks,
  unless `TCOS_VULKAN_BACKEND=dxg` asks for the Direct3D backend.

What the backend does:

* **Transport.** Each instance opens the render node, makes a Venus context (64 rings, as Mesa does), and a ring of 128 KiB in host shared memory (a blob with blob id
  0). Commands are written to the ring; when the host's ring thread has gone idle it is woken with `vkNotifyRingMESA` through `EXECBUFFER`. A command with a reply
  first points the host at the reply stream (`vkSetReplyCommandStreamMESA`, a shared memory blob that grows to the largest reply) and waits until the ring's head has
  passed it; the ring's lock is held until the reply is decoded. A command stream over 8 KiB (a big command buffer, a shader module) goes in shared memory of its own
  and the ring carries `vkExecuteCommandStreamsMESA`. Handles go over the wire as ids the guest chooses: the dispatchable objects carry one after the loader's slot,
  most non-dispatchable handles are the id itself, device memory, command pools and descriptor update templates point at the guest's struct.
* **Instances and devices.** The host's instance has no extensions (surfaces are the driver's) and at least Vulkan 1.1. The backend offers Vulkan 1.3 at most
  (`VN_API_VERSION`; 1.4's host image copy needs host pointers the guest cannot hand over), the host's device extensions that the protocol carries and that need
  nothing the guest cannot do (`kVnDeviceExtensions`, from `DENIED_EXTENSIONS` in `scripts/gen-vulkan-venus.py`: no external handles, host pointers, display or
  present-timing extensions, deferred host operations, ray tracing, descriptor buffers, calibrated timestamps), plus `VK_KHR_swapchain`. `driverID` is
  `VK_DRIVER_ID_MESA_VENUS`, `driverName` "TheoryCraftOS Venus", `driverInfo` names the host's driver, the conformance version is 0. External memory, fence and
  semaphore queries answer "no handle types".
* **Waits** are polled (`vkWaitForFences`, `vkWaitSemaphores`, `vkQueueWaitIdle` with an empty submission and a fence, `vkDeviceWaitIdle`, `vkGetQueryPoolResults`
  with `WAIT`): a blocking call would stop the one ring every thread's commands go through.
* **Memory.** Host-visible allocations are rounded up to pages; on the first map the memory becomes a blob of the context (blob id = its id, mappable and shareable:
  the host exports it and QEMU maps it into the window) and is mapped into the process. It stays mapped until it is freed. Memory the host cannot see does not map
  (`VK_ERROR_MEMORY_MAP_FAILED`).
* **Command buffers** are recorded into a stream of their own and sent at `vkEndCommandBuffer`.
* **Descriptor update templates** are the guest's: `vkUpdateDescriptorSetWithTemplate` and the push-descriptor variants become the writes they stand for.
* **Private data** of objects the host knows goes to the host by id; swapchains keep theirs in the guest.
* **Presentation.** A surface is the platform's (the same object on both backends). A Venus swapchain has the host's images (`TRANSFER_SRC` added) and, behind them, a
  swapchain of the Direct3D backend on the same surface, on a hidden instance and device of that backend. Acquire takes the Direct3D swapchain's next image and
  signals the program's semaphore or fence with an empty submission. Present copies the image on the host into a buffer the guest maps (waiting for the program's
  semaphores), then the guest copies it into the Direct3D swapchain's image and presents that. So every surface kind works (Win32, Wayland, X11, headless) with one
  copy of each frame through guest memory.

## Regenerating

* The protocol: `scripts/gen-venus-protocol.sh <venus-protocol checkout>` (python with mako).
* The command layer: `scripts/gen-vulkan-venus.py <venus-protocol checkout>` (reads its `xmls/vk.xml`), after adding or removing a Direct3D-backend function too (the
  rename header lists them), then `scripts/gen-vulkan-exports.py`.

## Testing

* `tests/venus_qemu.sh [program]` boots the kernel under QEMU with Venus and runs `tests/windows/<program>.c` (default `venustest`: the host's devices, memory, big
  command buffers, waits, compute through a template, images, timestamps, private data, the proc-address rules); run it through a short symlink to the checkout.
* `tests/run_host_tests.sh LinuxRenderNodeTest`: the render node and the session over a pretend device.
* `VENUS=off` runs the same machine without Venus (the Direct3D backend), `LINUX=1` a Linux program (`tests/linux/<program>.c` over `libvulkan.a`).
* Results (QEMU 11.1, virglrenderer 1.3, host RADV on an RX 9070 XT and a Ryzen 7800X3D's iGPU): `venustest` 133/0, `vulkantest` (Windows) 2520/0 on Venus and
  2520/0 with Venus off, the Linux `vulkantest` 143/0 on Venus; `LinuxRenderNodeTest` 5813/0.
* The host's log (virglrenderer's `vkr: ...` lines, which name a command the host could not decode) is QEMU's standard error: `$TMPDIR/tcos-venus-qemu/qemu-err.txt`.

Host facts learned on the way: `vkGetDeviceQueue2` must carry `VkDeviceQueueTimelineInfoMESA` with a ring index of its own (1 .. 63) or the host stops the
context; virglrenderer 1.3's `vkr` has no private data at all (the guest keeps it, as Mesa does); `vkr` adds the export information to host-visible
allocations itself.

## Gaps

* Mesa's own venus driver has not been run in the guest yet (no Mesa build for this OS here): the render node, `/sys` and the descriptors are checked by the host
  test and by our driver's Linux backend, not by Mesa.
* `libvulkan.so.1` / `libvulkan_tcos.so` need lld to link, which this build host does not have now; `libvulkan.a` is what the QEMU test uses.

* No DRM sync objects (`DRM_CAP_SYNCOBJ` is 0; the syncobj ioctls answer `EOPNOTSUPP`, `EXECBUFFER` with sync objects `EINVAL`): Mesa uses its simulated ones over the
  fence descriptors then.
* Linux differences of the render node: `chdir()` into `/sys` or `/dev/dri` is `ENOENT` (open, stat, getdents and `openat` work); `PRIME_FD_TO_HANDLE` of a
  closed descriptor is `EINVAL` (Linux: `EBADF`); a sync_file's `timestamp_ns` is when the guest first saw the fence signaled; a buffer keeps one fence, so
  several pending in fences are waited for on the spot; `/sys` attributes are read-only (no synthetic uevents), the PCI path has no bridge levels; a
  `CONTEXT_INIT` that fails part way keeps none of its parameters.
* No cross-device resource sharing (`RESOURCE_ASSIGN_UUID` is not negotiated: QEMU has no other virtio device to share with).
* The driver's backend: Vulkan 1.4 and the denied extensions above are not offered; one ring per instance (Mesa has one per thread); creation and submission calls wait
  for their reply (Mesa sends many of them without waiting); presentation copies each frame through guest memory instead of handing the host's image to the display.
* The application's allocation callbacks are not used (as in the Direct3D backend).
* The host's memory window limits host-visible memory that is mapped at once (QEMU's `hostmem`).
