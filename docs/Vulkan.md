# Vulkan for Windows and Linux programs (vulkan-1.dll, libvulkan)

One driver, two user-mode front doors. The driver is `src/Vulkan/` (`vk_driver.inc` includes the parts; `spirv.inc` is the shader translator, freestanding C with its own
interface in `spirv.h`; `khronos/` are the unchanged Khronos headers, Apache-2.0 OR MIT). What it needs from the system is the small interface of `src/Vulkan/vk_os.h`, and each
userland has a file that provides it and includes the driver:

| | Windows: `vulkan-1.dll` | Linux: `libvulkan.so.1` and `libvulkan.a` |
|---|---|---|
| Source | `src/Windows/vulkan-1/vulkan-1.c` | `src/Linux/libvulkan/libvulkan.c` |
| Built by | the Windows DLL build (CMake `windows_dlls`, mingw) | `src/Linux/libvulkan/build.sh <out> [x86_64 aarch64 riscv64 loongarch64]` (clang, freestanding): `<out>/<cpu>/libvulkan.so.1` and `libvulkan.a` |
| D3DKMT services (`kmt()`) | gdi32.dll's `D3DKMT*` thunks: the `NtGdiDdDDI*` system calls | `ioctl(TCOS_DXG_IOCTL_CALL)` on `/dev/dxg` (`src/Process/DxgDevice.h`) |
| Memory | the process heap | `mmap` (size classes); memcpy/memset/memmove/memcmp: the C library's in `libvulkan.so.1`, weak byte loops in `libvulkan.a` |
| Diagnostics (`VULKAN_1_DEBUG`) | `OutputDebugStringA`, and standard error when set | standard error when set (read from `environ` if the program has a C library) |
| Clock | `QueryPerformanceCounter` | `clock_gettime(CLOCK_MONOTONIC)` |
| Surface | `VK_KHR_win32_surface` (a window's client area) | `VK_KHR_wayland_surface` (a Wayland window: the images go to the compositor as dmabufs, `src/Linux/libvulkan/wayland.inc`); `VK_EXT_headless_surface` (no window: the swapchain flips onto the display) |
| Images as descriptors | - | `VK_KHR_external_memory`, `VK_KHR_external_memory_fd`, `VK_EXT_external_memory_dma_buf`: dmabufs of `/dev/dxg` (`vk_external.inc`) |

Everything else - the instance, the physical devices, memory, buffers and images, the formats, pipelines and the SPIR-V to DXBC translation, command buffers, compute, queries,
swapchains and presents - is the same code. The D3DKMT parameter blocks have Windows' x64 layout (`d3dkmthk.h`, `DxgLayouts.hpp`) on both systems and on every CPU (the driver
writes them byte by byte), so the kernel reads a Linux process's calls exactly as a Windows process's: `/dev/dxg` hands the call to the same win32k handler the system call has
(`src/Process/Win32k/Dxg/Win32kDxg.cpp`, `linux_service`), with the same per-process records and limits. Only the D3DKMT services can be reached that way (every number that
`dxg::register_service` registered); the service numbers are the `NtGdiDdDDI*` system call numbers, which `DxgDevice.h` repeats and `Win32kDxg.cpp` checks with `static_assert`.

A new platform (another Linux surface, a different transport) is a new file next to these two: define `VK_OS_NAME`, `VK_OS_EXPORT`, `VK_OS_INSTANCE_EXTENSIONS` and `VK_OS_PROCS`,
include `vk_driver.inc`, then define `kmt`, `vk_alloc`, `vk_free`, `vk_os_trace`, `vk_os_now_ns`, `vk_os_surface_extent` and the surface functions (each makes its `Surface` with
`make_surface(window, kind, out)`). Keep platform code out of `src/Vulkan`: no Windows or Linux types or calls there.

A surface that a compositor shows (a platform turns them on with `VK_OS_COMPOSITOR_SURFACES` and `vk_os_swapchain_made` / `vk_os_swapchain_gone` / `vk_os_acquire` /
`vk_os_present`, `src/Vulkan/vk_wsi.inc`) has the platform's state in `Surface.os`. Its swapchain images are shareable allocations, made as an image with
`VkExternalMemoryImageCreateInfo` is (not flipped, so not `A_PRESENT`: virgl refuses an image that is both shown and sampled), and the platform hands them over. The surface
reports an extent of 0xFFFFFFFF (the swapchain picks the window's size), B8G8R8A8_UNORM only, and opaque or premultiplied alpha.

`VK_KHR_wayland_surface` (`src/Linux/libvulkan/wayland.inc`). The protocol goes through the program's own libwayland-client: libvulkan finds its functions with `dlopen`
(`RTLD_NOLOAD` first: the copy the program loaded, whose `wl_display` it was given) and `dlsym`, so it does not link against it and needs it only when a Wayland surface is made
(libwayland 1.20 or later: `wl_proxy_marshal_flags`). A swapchain binds `zwp_linux_dmabuf_v1` through a registry of its own on an event queue of its own, so the program's
dispatching never runs the swapchain's handlers. Each image is exported once (`TCOS_DXG_IOCTL_EXPORT`) and made a `wl_buffer` with `create_immed` (plane 0, offset 0, the
allocation's row pitch, the linear modifier, XRGB8888 or ARGB8888 for premultiplied alpha); a roundtrip then catches a compositor that refused them (`VK_ERROR_SURFACE_LOST_KHR`). A
present attaches the image's buffer, damages all of it and commits (every submission has run when `vkQueueSubmit` returns, so the image holds the frame: no sync object is needed);
FIFO asks for a frame callback and waits for the previous one first, MAILBOX and IMMEDIATE do not wait. An acquire hands out the oldest image whose buffer the compositor released
and the program does not hold, waiting on the swapchain's queue for a release; with a timeout of 0 it answers `VK_NOT_READY`.

`VK_KHR_xcb_surface` and `VK_KHR_xlib_surface` (`src/Linux/libvulkan/x11.inc`), for X11 programs (the kernel's X server, [X11.md](X11.md), or any other). They are surfaces
of the same kind as Wayland's (the platform's state on `Surface.os`; `libvulkan.c` sends the swapchain hooks to `x11.inc` or `wayland.inc` by the surface's kind). The
protocol goes through the program's own libxcb, found with `dlopen` / `dlsym` as libwayland is; an Xlib surface takes its `Display`'s connection with
`XGetXCBConnection` (libX11-xcb). The surface's current extent is the window's size (GetGeometry); a swapchain needs a window of depth 24 or 32. A present maps the
image for the CPU (`D3DKMTLock`, read-only) and sends its rows with `xcb_put_image` (ZPixmap, B G R X bytes) in bands that fit the server's largest request
(BIG-REQUESTS when there is one; padded rows are packed into a band buffer first). The server never holds an image, so an acquire hands out any image the program
does not hold (no timeout to wait on: `VK_NOT_READY` or `VK_TIMEOUT` when it holds them all). FIFO waits for a round trip (GetInputFocus) after each present, MAILBOX
and IMMEDIATE do not. Each frame is one read of the image and one copy over the X connection: no MIT-SHM, DRI3 or Present extension (the kernel's X server has none).

Images as dmabufs (Linux; a platform turns them on with `VK_OS_EXTERNAL_MEMORY_FD` and `vk_os_export_fd` / `vk_os_import_fd` / `vk_os_close_fd`, `src/Vulkan/vk_external.inc`). An
image's pixels are its allocation, not its memory, so what is exported is the allocation of the image the memory belongs to: the one `VkMemoryDedicatedAllocateInfo` names, else
the first image bound to it. An image made with `VkExternalMemoryImageCreateInfo` has a shareable allocation that is also a texture (so that the compositor samples it);
`vkGetMemoryFdKHR` exports it (`TCOS_DXG_IOCTL_EXPORT`), and `vkAllocateMemory` with `VkImportMemoryFdInfoKHR` and a dedicated image gives that image the descriptor's allocation
in place of its own (same size and format, else `VK_ERROR_INVALID_EXTERNAL_HANDLE`; the descriptor is then the driver's and closed). Only 2D colour images of one level and one
sample, 8 bits a channel; `vkGetPhysicalDeviceExternal*Properties` and `VK_KHR_get_physical_device_properties2` are not there. This is how a Wayland client on this system
renders on the GPU and hands its frames to the compositor (`zwp_linux_dmabuf_v1`, [Wayland.md](Wayland.md)).

Linux programs of a C library load `libvulkan.so.1` (installed as `/usr/lib/libvulkan.so.1` of the Linux tree, `docs/LinuxDynamicPrograms.md`). It takes the place of the
Khronos loader rather than being one of its drivers: a program built against the loader (`-lvulkan`, or `dlopen("libvulkan.so.1")` and `vkGetInstanceProcAddr`) calls the driver
directly. It exports only the `vk*` functions (`-fvisibility=hidden`) and takes `dlopen`, `dlsym`, `environ` and the memcpy family from the program's C library. Static programs
without a C library link `libvulkan.a` (no Wayland surface there: no `dlopen`).

The same driver is also built as an installable client driver of the Khronos loader (`-DVK_ICD`: `<out>/<cpu>/libvulkan_tcos.so` with the manifest
`tcos_icd.<cpu>.json`), for a system whose `/usr/lib/libvulkan.so.1` is the real loader (a program that brings its own, a Flatpak-style runtime, layers): installed as
`/usr/lib/libvulkan_tcos.so` with the manifest at `/usr/share/vulkan/icd.d/`, which the loader searches by itself. It exports only
`vk_icdNegotiateLoaderICDInterfaceVersion` (interface version 5: the driver makes the surfaces, answers unknown physical-device functions), `vk_icdGetInstanceProcAddr`
and `vk_icdGetPhysicalDeviceProcAddr` (`vkGetPhysicalDevice*` only); every `vk*` function is internal and the library is linked `-Bsymbolic`, so the driver's
own references never bind to the loader's functions of the same names. Each dispatchable object (instance, physical device, device, queue, command buffer) starts
with `ICD_LOADER_MAGIC`, which the loader replaces with its dispatch table (`Hdr`, `vk_base.inc`; nothing in the driver reads that word).

Tests: `tests/VulkanShaderTest.cpp` (the translator, host, ASan + UBSan: `tests/run_host_tests.sh VulkanShaderTest`); `tests/windows/vulkantest.c` (the real DLL on every adapter of
the machine, in QEMU); `tests/linux_vulkan.sh` (`tests/linux/vulkantest.c` over `libvulkan.a` as an x86-64, AArch64 and RISC-V Linux program on the kernel's CPU emulator, with
win32k's D3DKMT services and the software renderer behind `/dev/dxg`: the extensions, a triangle with push constants, a compute dispatch, a headless swapchain);
`tests/wayland_dmabuf_emulated.sh` (`tests/linux/waylandvktest.c`: an exported image rendered frame after frame and shown by the Wayland compositor, an import of it, the ioctls'
refusals, on the three CPUs); `tests/linux_vulkan_wayland_emulated.sh` (`tests/linux/vkswapchain.c`: a glibc program with libwayland-client and an xdg-shell toplevel over
`libvulkan.so.1` with `VK_KHR_wayland_surface`: the surface's answers, FIFO and MAILBOX swapchains, a swapchain remade from another, `VK_NOT_READY`, the program's own queue beside
the swapchain's, the window on the stand-in desktop with the last frame, drawn on the device and on the CPU; x86-64 on the CPU emulator; it also passed on the real kernel in
QEMU, 41 checks, on standard VGA (the desktop drew the buffers from the CPU) and on virgl (all drawn in place on the GPU); the script then runs the same program
through the development machine's Khronos loader (1.4.363) with `libvulkan_tcos.so` and its manifest at the system's paths, which must pass too and draw its window);
`tests/linux_vulkan_x11_emulated.sh` (`tests/linux/vkx11.c`: a glibc program with libxcb, libX11 and libX11-xcb, an xcb window and an Xlib window over
`VK_KHR_xcb_surface` / `VK_KHR_xlib_surface`: the extensions, presentation support, the window's size as the extent, FIFO and MAILBOX frames, `VK_NOT_READY` with every
image held; the kernel's X server on the CPU emulator, whose stand-in desktop must show both windows with their last frames; then again through the Khronos loader);
`ProcessFilesTest` (`test_dxg_device`: `/dev/dxg`'s answers to bad requests, pointers and services). Lists regenerated by `scripts/gen-vulkan-exports.py` after a function is added
(`src/Vulkan/vk_exports.inc` for the shared functions; a platform lists its own in `VK_OS_PROCS`).

## One driver for every GPU

A real `vulkan-1.dll` is a loader that forwards to the installed drivers of each GPU. This OS has one graphics architecture: the kernel-mode Direct3D layer
(`src/Kernel/Dxgkrnl`, [Direct3DKernelMode.md](Direct3DKernelMode.md)) owns the adapters, video memory and queues and replays a device-independent command stream on whatever
`graphics::IGraphicsDevice` is behind an adapter: the CPU renderer, virgl, SVGA3D, Intel Gen9, Raspberry Pi V3D, Radeon Vega. So the Vulkan driver is written once, over the
D3DKMT services of `gdi32.dll` (`NtGdiDdDDI*` in win32u, [win32k/Dxg.md](win32k/Dxg.md)), and every adapter that the kernel layer drives is a `VkPhysicalDevice`.

What differs between GPUs is what the adapter reports, and the driver exposes exactly that and nothing more:

* `QueryAdapterInfo` type 0 (the adapter capabilities blob) gives the texture units, vertex attributes, varyings, push constant bytes, vertex stride, the `DeviceFeature` bits,
  the multisample counts, the kind of device, a vendor and a device number (the last four are new for Vulkan: `AdapterCaps::sample_counts / device_kind / vendor_id / device_id`,
  appended to the blob after the name; a caller that asks for the old 96 bytes still gets them).
* Limits (`maxPushConstantsSize`, `maxPerStageDescriptorSamplers`, `maxVertexInputAttributes`, `maxFragmentOutputAttachments`, ...), the sample counts, the features
  (`independentBlend` only with `kFeatureIndependentBlend`) and the formats (sRGB, float, integer, 16 bit unorm / snorm and 10-10-10-2 only with their feature bits) follow the adapter.
  virgl gives 16 texture units, 16 vertex attributes (`maxVertexInputAttributes`, the most vulkan-1 takes) and 15 varyings (14 on a GL 3.3 host); a GPU with less is a smaller
  Vulkan device, and the CPU renderer is a big one. virgl 3D commands wait up to 20 s for the host (a cold shader compile can be slow), display commands 250 ms.
* A pipeline the adapter cannot run is refused when it is created (`VK_ERROR_FEATURE_NOT_PRESENT`, with the reason written to the debugger when `VULKAN_1_DEBUG` is set), never
  when it is drawn with.

## How a program's calls become work

* **Shaders**: SPIR-V is translated to Shader Model 4.0 DXBC (`spirv.inc`: structured control flow, phi nodes, inlined functions, std140 uniform blocks, sampled images, derivatives,
  `GLSL.std.450`) and given to the kernel as a `ShaderKind::Dxbc` shader through its private escape, which every adapter takes (interpreter, TGSI for virgl, D3D9 tokens for SVGA3D, ...).
  Vulkan's Y-down clip space is turned into the Y-up the kernel's Direct3D shaders have by negating the position's Y when it is written.
* **Resource numbers** belong to a pipeline: uniform blocks (by set and binding) and the push constant block are constant buffers `cb0..` that the kernel lays end to end into its push
  constant block (so every stage declares every slot); sampled images are texture units `t0..`/`s0..`; storage buffers are the kernel's storage slots: a buffer some stage of the
  pipeline writes is a raw unordered access view `u0..u3`, one that is only read (`NonWritable`) a raw read-only view `t0..t3` (the kernel's slots 4..7). That is 4 + 4 storage buffers a
  pipeline (`maxPerStageDescriptorStorageBuffers` 8; a pipeline that writes a fifth is refused when it is created).
* **Size**: a shader may use up to 256 temporary registers at once and 60000 instructions (the kernel takes 65536 instructions and 256 temporaries of a program made from DXBC; a
  Direct3D 9 program keeps 32). The DXBC goes to the kernel in one escape of at most 4 MiB, and on virgl its TGSI text goes in pieces (the host's continuation packets). A big shader
  is slow to compile on the virgl host the first time (the host's GL driver compiles what virglrenderer makes of the TGSI; its shader cache keeps it after that).
* **Cube maps** (also cube map arrays: `imageCubeArray`) are 2D arrays of six layers a cube to the kernel. A cube compatible image of 6·n layers is one `Texture2DArray`, and the
  translated shader works out the face (Vulkan's table: the major axis and its sign), the point on it and the layer `face + 6 · round(cube)`, then samples the array. Filtering does not
  cross faces. Not taken: depth comparisons and gradients with a cube map.
* **Textures in vertex and compute shaders**: lookups at a level (`textureLod`; `texture` there is one at level 0, as in GLSL), texel fetches, sizes, gathers and depth comparisons
  at level 0 - what needs no derivatives. A pipeline's units are numbered for all its stages; virgl binds them to the vertex and pixel stage of a draw and to the compute stage of a
  dispatch, the CPU renderer hands its texture set to the vertex and compute shaders through their contexts.
* **Dynamic indices into registers** (a local array `a[i]`, a vector's `v[i]`, nested and struct-member arrays): a load is a chain of comparisons and `movc` over the places the
  indices may pick (at most 256), a store writes each of them under its comparison.
* **Memory in shaders** (storage buffers, `Workgroup` variables) is addressed by bytes: an access chain is a constant offset plus an integer for its dynamic indices, a load `ld_raw`, a
  store `store_raw`, atomics `imm_atomic_*` (add, sub, and, or, xor, min, max, exchange, increment, decrement; not compare-exchange, which the kernel has not). The layout is the module's
  (`Offset`, `ArrayStride`, `MatrixStride`: std430 or std140); `Workgroup` variables have none, so they are laid out one after the other in the group-shared memory `g0` (32 KiB).
  A boolean in memory is a 32-bit word. `barrier()` is `sync_g_t` (the kernel then runs the group a whole group at a time, at most 256 threads).
* **Compute pipelines** (`vkCreateComputePipelines`, the adapter's `kFeatureCompute`): the shader is a `cs_5_0` DXBC compute shader with `dcl_thread_group` from `LocalSize`,
  `LocalSizeId` or the specialised `WorkgroupSize`; `gl_GlobalInvocationID`, `gl_WorkGroupID`, `gl_LocalInvocationID` and `gl_LocalInvocationIndex` are the system values
  `vThreadID`, `vThreadGroupID`, `vThreadIDInGroup`, `vThreadIDInGroupFlattened`. Queue family 0 has `VK_QUEUE_COMPUTE_BIT` on an adapter with compute. Graphics and compute have
  their own bound pipeline and descriptor sets, as Vulkan's bind points do; push constants are shared.
* **Memory** is host memory. A `VkBuffer` or `VkImage` bound to it gets a kernel allocation when the GPU needs it; the contents are copied to it when the program has written the
  memory since. A mapped memory counts as written once in each `vkQueueSubmit` (Vulkan makes host writes visible at the submission, not while it runs), so a buffer that many draws
  of a submission use is brought up once; and a buffer in mapped memory keeps a copy of what it last sent, so one found unchanged (static geometry an engine keeps mapped) is not
  copied at all. Uniform buffers never reach the GPU as buffers: their bytes are read at draw time into the constant block.
* **Command buffers** are recorded as a list and run at `vkQueueSubmit`: a render pass is one command stream (`CommandStream.hpp`; a pass that does not fit the 64 KiB buffer of
  the context is run in parts), copies, fills, updates and clears that the CPU can do are done in order with it. A clear of an image that can be drawn into (one level, one layer,
  one sample, not linear) is a pass that only clears it, in the stream, done by the GPU. A vertex buffer is used directly when it is one binding of 32-bit
  floats with the stride the pipeline expects; otherwise (normalised or integer formats, several bindings, instancing, other strides) the vertices are gathered into a temporary
  buffer of floats. Every submission has run when `vkQueueSubmit` returns, so fences, semaphores and events only keep a state.
* **Dispatches** (`vkCmdDispatch`, `vkCmdDispatchIndirect`) are a stream of their own: the pipeline, the constants, the storage buffers (`BindUav`), `Dispatch`. When it has run, what
  the GPU wrote into the written storage buffers (the bound ranges) is copied back into their host memory (`buf_readback`), so the copies the CPU does, the uniform data, an
  indirect dispatch's group counts and a mapped memory all see it. Storage buffers in draws are bound the same way (read-only ones need the adapter's `kFeatureStorageInDraws`, written
  ones `kFeatureVertexStores` / `kFeatureFragmentStores`, which are `vertexPipelineStoresAndAtomics` / `fragmentStoresAndAtomics`) and copied back when the pass's stream has run.
* **Presentation**: `VK_KHR_win32_surface` + `VK_KHR_swapchain`; a swapchain image is a kernel allocation that `D3DKMTPresent` flips onto the display. A swapchain image is a render target and a present image, and a texture only when the program asks to sample it: virgl cannot show an image that is also
  sampled (making it resident for the present fails, and a present that fails loses the context).

## Speed

`vulkantest.c` ends with `test_timing`, which prints `timing:` lines for each adapter: an empty submission, a 64 x 64 frame read back, 500 draws in a pass, one draw a frame from a
mapped 4 MiB vertex buffer (unchanged, then with 60 bytes written), and a clear and present to the display and into a 640 x 480 window. On virgl in QEMU (2026-11-07): a draw 1 us
(was 140 us), the mapped buffer 0.4 ms a frame unchanged and 3.6 ms written (it was copied and uploaded whole on every draw), a 640 x 480 clear 0.07 ms (was 2.1 ms on the CPU),
a present 0.5 ms (was 1.6 ms). What it took: mapped memory counted as written once a submission, unchanged buffers not sent, clears on the GPU, virgl command streams and uploads
sent without a fence (the host answers when it has the commands, not when its GPU is done: `VirtioGpu::execute`), and memory copies, fills and compares that are no longer a byte at
a time (ntdll / msvcrt `libc_string.inc`, the kernel's `runtime.cpp`). Left: a written buffer is still sent whole (no range), and a present into a window reads the picture back
from the host and the desktop composites it on the CPU.

## The 3D Test window

The desktop's 3D Test can draw its cube through the Vulkan path (SPIR-V translated by the driver's translator, built into the kernel, then the escape), next to Direct3D; see [Direct3DKernelMode.md](Direct3DKernelMode.md#the-3d-test-window).

## Not done

Geometry and tessellation shaders, storage images, texel buffers, occlusion and pipeline statistics queries (timestamps are done: the CPU's clock when the commands before them
have run), indirect draws, sparse resources, input attachments, a second queue family, depth/stencil
images that are also sampled (the kernel's depth images have no CPU view), blend constants, wireframe, depth clamp, anisotropy, specialization of `OpSpecConstantOp`, row-major
matrices in shaders, `OpArrayLength` (the length of a runtime array),
`gl_NumWorkGroups`, compare-exchange atomics, more than 4 written and 4 read-only storage buffers a pipeline, arrays of storage buffers indexed dynamically, Vulkan 1.1 and later
(`vkEnumerateInstanceVersion` says 1.0). A draw that writes a storage buffer another draw of the same pass reads from mapped memory may see the old contents (the copy back is when
the pass's stream has run). Kernel limits Godot's renderers run into next: arrays and cubes
have one mip level, an 8-bit colour format and cannot be drawn into, and uniform data is the push constant block (`maxUniformBufferRange` 4096, below Vulkan's 16384). A window is not composed: a swapchain flips the whole display. The exports of the core that are not implemented are linked in as functions
that answer `VK_ERROR_FEATURE_NOT_PRESENT` (`src/Windows/stubs/vulkan-1.inc`). Not validated on real GPUs: only the CPU renderer and what QEMU gives (virgl, standard VGA) run it.

Linux: X surfaces copy each frame through the X connection (no MIT-SHM / DRI3 / Present, no `VK_SUBOPTIMAL_KHR` when the window is resized; the image is drawn at the
window's top left at its own size). The installed `libvulkan.so.1` is still the driver itself (no layers); the ICD build is there for the real
loader, which has been tried on the emulator only (not in QEMU), and the image does not install the loader. Wayland: a finite acquire timeout is not kept (it waits
until an image is released), no explicit sync (not needed while submissions run to the end in `vkQueueSubmit`), no `VK_KHR_present_wait` / `present_id`, no
`VK_EXT_swapchain_maintenance1`, only B8G8R8A8_UNORM (no sRGB or 10-bit formats), no `VK_SUBOPTIMAL_KHR` on a window resize (a Wayland window takes its buffers' size anyway).
`libvulkan.so.1` with Wayland has been run for x86-64 only (the development machine has no other C library), on the emulator and natively in QEMU.
