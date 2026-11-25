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

## The kernel's 3D device on Venus

Everything in the kernel that draws goes through one `graphics::IGraphicsDevice` per GPU (the render node, `src/Graphics/RenderNode.hpp`): the Direct3D layer
(`src/Kernel/Dxgkrnl`, so Direct3D 9 / 10 / 11 / 12 and DXGI), OpenGL and WebGL / WebGPU (`WebGlAccelerator`, the GL translator's TGSI), the desktop's 2D renderer and
compositor, the windows of Wayland and X11 clients, and the Vulkan driver's Direct3D backend. On QEMU's virtio-gpu that device is now **`VenusDevice`**
(`src/Graphics/Graphics3D/Venus`) when the device offers Venus, and `VirglDevice` (virglrenderer's OpenGL, TGSI) when it does not.

* **The kernel's own Vulkan** (`KernelVulkan.h`, `KernelVulkan.c`, `KernelVulkanOs.cpp`): the driver's Venus backend, `vk_venus.inc`, compiled into the kernel without
  presentation (`VN_NO_PRESENT`), over a render-node session of the kernel's own (`graphics::virtgpu::Session`, the same object `/dev/dri/renderD128` hands a program; its
  ioctls take kernel pointers, its host blobs are where the kernel already maps the host-visible window). The kernel calls it through one table (`tcos_kernel_vulkan()`,
  the list is `TCOS_KVK_PROCS`). `VenusContext` makes the instance, picks the first Vulkan 1.3 device with a graphics and compute queue, and enables what the host has of
  Vulkan 1.0 - 1.3 and of transform feedback, conditional rendering, custom border colours, conservative rasterization, depth clip, 8-bit indices, line rasterization,
  vertex attribute divisors, the provoking vertex, robustness2 (null descriptors), depth bias control and sample locations.
* **Shaders**: TGSI is the language every hardware path of the kernel writes (the Direct3D layer's `D3d9Tgsi.cpp`, the GL translator, the built-in shaders), so the device
  compiles TGSI to SPIR-V 1.5 (`TgsiText.*` reads it, `TgsiSpirv.*` with `Spirv.*` writes the module). Registers are untyped vec4s of 32-bit words bit-cast per opcode;
  immediates keep their bits; comparisons give TGSI's ~0 / 0 or 1.0 / 0.0; structured control flow maps onto SPIR-V's; `KILL` is a demote (derivatives after it work).
  One descriptor set: the constant buffer at binding 0, texture units as sampled images and samplers, and each stage's storage buffers, images and typed buffer views at
  bindings of its own (the Direct3D layer numbers them per shader). The things virgl's GL host does by itself are told explicitly: `ShaderModuleDesc::tgsi_gl` (the GL
  translator's images are GL's rows bottom first: DDY runs up, flat varyings take the last vertex) and `DeviceInfo::d3d_sample_order` (Vulkan's standard sample positions
  are Direct3D's, so the Direct3D layer's TGSI does not turn 4x sample indexes around as it does for virgl).
* **Pipelines** keep a variant per binding shape (`VenusVariantKey`: the value types of the bound textures and typed views - a SPIR-V sampled type must match the format -,
  the storage images' formats, the attachments' formats and samples, the topology, the patch size). The Vulkan state is VirglDevice's semantics (`alpha_blend` is
  SrcAlpha / InvSrcAlpha with alpha One / InvSrcAlpha, independent blend falls back to no blending, a depth write without a test passes everything, the hull's tessellator
  settings rule the domain's, the tessellation domain origin is GL's lower left, all sixteen viewports are set with the ones past those given repeating the first).
  A pipeline without multisampling on a multisampled target puts every sample at the pixel centre (VK_EXT_sample_locations): "all or nothing by its centre".
  A normalised depth format's bias on the D32F buffer is a float bias of 1 / (2^n - 1) (VK_EXT_depth_bias_control).
* **Resources**: buffers are host-visible coherent memory the kernel maps (IBuffer::map is that memory); images are device-local, optimal and always in the GENERAL layout;
  CPU transfers go through a staging buffer, 0xAARRGGBB words or WidePixels' Vec4s, rows top first (Vulkan's own order: there are no flips anywhere). R8G8B8A8 images and
  8-bit storage images are RGBA with red and blue turned around on the CPU, as on virgl.
* **submit()** replays the `CommandRecorder` list (VirglDevice's refusals; nothing outlives a submit; a pass resets the pipeline) into a Vulkan command buffer, submits it
  and waits for its fence, so it is as synchronous as the interface says. Each pass, dispatch and storing draw is fenced off by a full memory barrier (what GL gives virgl
  implicitly). Constants go into a per-submit ring bound with a dynamic offset (4096 bytes of push block plus the samplers' comparison rows at 256 are more than any push
  constant range); a submit that fills the ring or the descriptor pool is split. Queries are segmented per kind as on virgl (one query of a kind at a time), timestamps are
  host ticks times `timestampPeriod`; stream output is transform feedback whose counter is seeded with the binding's offset, so appends across draws and pipelines work;
  resolves are vkCmdResolveImage (through a scratch image and a blit when the formats differ), a rendering resolve from sample 0 for depth, and an internal TGSI pipeline
  for the least or greatest sample.
* **Gap pass 4** (Direct3D's later state): the planes of depth images and whole subresources of depth and multisampled images (`kFeature2DepthCopies`: copies of
  each aspect through the staging buffer, `vkCmdCopyImage`), copies of samples (`kFeature2SampleCopies`), a magnification filter of its own
  (`kFeature2SeparateFilters`), min / max reductions (`kFeature2SamplerReductions`, samplerFilterMinmax), Direct3D's line modes (`kFeature2LineModes`:
  VK_EXT_line_rasterization's smooth lines for alpha-antialiased ones, rectangular lines 1.4 and 1.0 pixels wide for the quadrilateral ones), the depth bounds test
  (`kFeature2DepthBounds`, dynamic state), a stream output binding's end (the transform feedback binding's size), and pipeline statistics that leave out a
  stand-in pixel shader (`stand_in_fragment`: the draw has a statistics segment of its own whose PsInvocations are not added).
* **present()** copies the image into the virtio-gpu's 2D screen (`blit` + `present` of the display driver), one copy through guest memory per frame.
* **Which device** (`RenderNode::build_venus`): with `TCOS_GPU_BACKEND=AUTO` (the default) the Venus device is used when it opens, draws the self-test picture and offers
  every feature and limit that a virgl device on the same host would (a virgl device is made for the comparison and given back) and the host has every extension the
  device needs to do those features as virgl does (`VenusDevice::parity_gaps`: robustness2 null descriptors, the last-vertex provoking mode, depth bias control's float
  representation, sample locations for every offered sample count, custom border colours, vertex attribute divisors including zero, depth clamp); the boot log says
  which and why ("chosen (it does everything virgl does here)", or "virgl kept (Venus lacks features ... / ..., extensions ...)").
  `TCOS_GPU_BACKEND=VENUS` takes Venus whenever it works, `VIRGL` never. The scripts take `GPU_BACKEND=` (`tests/venus_qemu.sh`) or `GPU=venus` (`tests/d3d12_qemu.sh`).

What stays on virgl, and why:

* **Everything, on a virtio-gpu without the Venus capset** (QEMU without `venus=on`, or a virglrenderer built without Venus): `VirglDevice` is the only 3D device there.
* **Everything, when AUTO finds Venus short of virgl** on that host (a missing feature, limit or one of the extensions above; the boot log names it), or with
  `TCOS_GPU_BACKEND=VIRGL`.
* With Venus chosen, no path draws through virgl: Direct3D 9 - 12, OpenGL, WebGL / WebGPU, the desktop and compositor, Wayland and X11 windows and the Vulkan driver's
  Direct3D backend all use `VenusDevice`, and user-mode Vulkan programs use Venus directly through the driver's Venus backend, as before. Two things remain of the virgl
  side: the AUTO comparison makes a `VirglDevice` for a moment (no context, nothing drawn) to read its `info()`, and the screen is still the virtio-gpu's 2D scanout
  resource that `present()` copies into (2D virtio-gpu commands, not virgl).

Testing:
* `tests/run_host_tests.sh TgsiSpirvTest`: every TGSI text of the built-in shaders, the GL conformance scenes and the Direct3D conformance scenes is read and compiled, and
  each module passes `spirv-val --target-env vulkan1.3`; damaged texts are refused without reading out of bounds.
* `tests/run_host_tests.sh VenusHostTest`: the Venus device on the **build host's** Vulkan (the table filled from the host's loader, `tests/VenusHostVulkan.inc` from
  `tests/gen_venus_host_vulkan.py`) runs the Direct3D and GL conformance scenes against the CPU renderer, in seconds and without QEMU (`VENUS_SCENE=`, `VENUS_DUMP=1`).
* `tests/venus_qemu.sh` and `GPU=venus tests/d3d12_qemu.sh <program>` run in the guest, where the boot's conformance runs on the device as well.
  (`GPU_BACKEND=AUTO` tests the choice; `GPU=virgl` the same programs on virgl.)
* Results (QEMU 11.1, virglrenderer 1.3, host RADV on an RX 9070 XT): on the Venus device d3d12test 4816/0, d3d11test 465/465, d3d9test 166/166, d3dsharetest
  across processes 594/0, opengltest 51/0, vulkantest 2520/0, venustest 133/0, the Linux vulkantest 143/0, and the desktop draws; on virgl with the same tree
  d3d12test 4831/0, d3d11test 462/462, d3d9test 166/166, opengltest 51/0 (the counts differ where a test checks what only one device offers). The emulator with
  `VENUS=1 tests/emulated_dx.sh` runs the Direct3D video, encode and sharing tests on the device: all pass.

## Regenerating

* The protocol: `scripts/gen-venus-protocol.sh <venus-protocol checkout>` (python with mako).
* The command layer: `scripts/gen-vulkan-venus.py <venus-protocol checkout>` (reads its `xmls/vk.xml`), after adding or removing a Direct3D-backend function too (the
  rename header lists them), then `scripts/gen-vulkan-exports.py`.

## Testing

* `tests/venus_qemu.sh [program]` boots the kernel under QEMU with Venus and runs `tests/windows/<program>.c` (default `venustest`: the host's devices, memory, big
  command buffers, waits, compute through a template, images, timestamps, private data, the proc-address rules); run it through a short symlink to the checkout.
* `tests/run_host_tests.sh LinuxRenderNodeTest`: the render node and the session over a pretend device.
* `VENUS=off` runs the same machine without Venus (the Direct3D backend), `LINUX=1` a Linux program (`tests/linux/<program>.c` over `libvulkan.a`).
* Results (QEMU 11.1, virglrenderer 1.3, host RADV on an RX 9070 XT and a Ryzen 7800X3D's iGPU): `venustest` 133/0, `vulkantest` (Windows) 2520/0 on Venus,
  the Linux `vulkantest` 143/0 on Venus; `LinuxRenderNodeTest` 5813/0. With Venus off `vulkantest` is 2520/6 since the Direct3D gap-pass merges (the `cs_add`
  dispatch's submit fails on the virgl and the software adapter alike; it was 2520/0 before them and is not a Venus matter).
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

Gaps of the kernel's Venus device:

* Every submit waits for its fence, and every creation call (images, pipelines, descriptor sets) is a round trip to the host: correct and simple, not fast. A pipeline's
  first draw with a new binding shape compiles a variant on the host.
* present() copies the frame through guest memory into the virtio-gpu's 2D screen; the host's image is not scanned out directly (a blob scanout would need the host
  image exported as a dma-buf).
* Images are always in the GENERAL layout (no compression a driver keeps for attachment layouts); barriers are full memory barriers at every pass, dispatch and storing
  draw, not the narrow ones Vulkan allows.
* As on virgl: depth resolves to the least or greatest sample are not offered (`kFeatureResolveModes`), nor coordinates in texels
  (`kFeature2UnnormalizedCoordinates`: Vulkan's unnormalized samplers take no levels, borders or offsets, which Direct3D's do).
* Not offered yet, though the host's Vulkan has them (each needs the TGSI text to carry something it does not say, or more state through the replay): programmable
  sample positions (`kFeature2SamplePositions`, VK_EXT_sample_locations' dynamic locations and the depth buffer's own), view instancing (`kFeature2ViewInstancing`,
  SV_ViewID / multiview), coarse shading (`kFeature2ShadingRate`, VK_KHR_fragment_shading_rate), barycentrics (`kFeature2Barycentrics`,
  VK_KHR_fragment_shader_barycentric) and residency feedback (`kFeature2TileResidency`, sparse residency). The device does not offer them, so the Direct3D layer
  reports the lower tiers, and a command for one of them fails the submit.
* What the host's Vulkan lacks is not offered (each feature follows `VenusCaps`); a host without `VK_EXT_sample_locations` refuses pipelines that turn multisampling off on
  a multisampled target, one without `VK_EXT_depth_bias_control`'s float representation the depth bias of normalised depth formats, one without
  `VK_EXT_provoking_vertex` the GL translator's flat varyings. AUTO keeps virgl on such a host (`parity_gaps`); `TCOS_GPU_BACKEND=VENUS` takes Venus anyway.
