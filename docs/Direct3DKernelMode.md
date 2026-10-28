# Direct3D, kernel-mode half

Direct3D on Windows is two halves: a **user-mode driver** (the D3D9/D3D11 runtime plus a vendor UMD; Mesa's gallium-nine is the free equivalent, translating D3D9 calls
into gallium commands) and a **kernel-mode driver** (`dxgkrnl.sys` plus the vendor miniport; under nine that is the DRM driver). This document is the kernel half:
`src/Kernel/Dxgkrnl/`, test `tests/DxgkrnlTest.cpp` (`tests/run_host_tests.sh DxgkrnlTest`, ASan + UBSan). The user-mode half (d3d9.dll, dxgi.dll, gdi32/win32u thunks) is
separate work and reaches this through `dxgk::Kernel`.

The kernel half owns what a process must not touch directly: the adapters, video memory and who may use which of it, the queues commands go to and the order they run in,
fences between them, the display, and the validation of everything a process sends. It is a typed C++ service surface named after D3DKMT (`D3DKMTCreateDevice` ->
`Kernel::create_device`, `D3DKMTLock` -> `Kernel::lock`, ...; the table is at the top of `DxgKernel.hpp`), with the calling process passed explicitly, so the `NtGdiDdDDI*`
system calls are thin.

| File | Job |
| --- | --- |
| `DxgTypes.hpp` | NTSTATUS values, handles, formats, allocation / pipeline / present descriptions. |
| `HandleTable.hpp` | Per-process handle table: slot + 12-bit generation + 4-bit kind, so a closed or wrong-kind handle never names another object. |
| `VidMm` | Allocations and video memory (WDDM's VidMm): system-memory backing, residency, budget, LRU eviction with write-back, locking. |
| `CommandStream.hpp` | The device-independent command stream (the "DMA buffer") and `CommandWriter`, which builds one. |
| `Adapter` | One adapter: its `IGraphicsDevice`, VidMm, contexts, **scheduler** (VidSch) and the code that validates and replays a command stream. |
| `DxgObjects.hpp` | Sync objects, shaders, pipelines, devices, contexts, queue entries. |
| `DxgKernel` | The services: processes, adapters, devices, contexts, allocations, sharing, lock, residency, fences, render, present, escape, display modes. |
| `src/Graphics/SystemDxg` | `register_dxg_adapters()`: one adapter per GPU of the `GraphicsPool`. `main.cpp` builds the kernel and publishes it as `dxgk::system_kernel()`. |

## How it differs from Windows, on purpose

- The "miniport" is `graphics::IGraphicsDevice` (software rasterizer, virgl, SVGA3D, Intel Gen9, ...). So the user-mode driver emits one device-independent command
  stream, and the kernel validates it and replays it on whichever device the adapter has. There are no hardware DMA buffers, patch lists or GPU virtual addresses.
- Nothing blocks by itself. The scheduler runs on the calling thread (every call that can unblock work calls `pump()`; a kernel thread may call it too), and
  `wait_sync_cpu` sleeps only if a wait hook (`IWaitHook`) is set.

## Memory (VidMm)

Every allocation has a system-memory backing, always valid unless the GPU holds newer contents. The device object (`IImage` / `IBuffer`) exists only while the allocation is
**resident**; the manager keeps resident bytes within the adapter's budget by evicting the least recently used allocation that is not pinned (a colour image is read back
first). A command stream pins its allocations while it runs, so a stream whose working set exceeds the budget fails with the device in `ErrorOutOfMemory` and the context
lost. `lock()` reads the GPU's contents back and hands out a view of the backing; `unlock()` marks the device copy stale (uploaded at next use). Depth/stencil images have no
backing: their contents are lost on eviction, and they cannot be locked. Formats are A8R8G8B8, X8R8G8B8, A8B8G8R8, X8B8G8R8, D32F, D24S8; textures may have mip levels
(lockable per level, `GenerateMipmaps` in the stream). Allocations are shared across processes by token (`share_allocation` / `open_shared_allocation`).

## The command stream

Packets of little-endian words, `op | size << 16` then the payload (full layout in `CommandStream.hpp`): `BeginPass`, `EndPass`, `BindPipeline`, `BindVertexBuffer`,
`BindIndexBuffer`, `PushConstants`, `BindTexture`, `SetViewport`, `SetScissor`, `Draw`, `DrawIndexed`, `CopyRegion`, `GenerateMipmaps`. Allocations are named by index into
the allocation list passed with the stream, pipelines by handle. `render()` copies the stream, **validates all of it** (packet sizes, indices, flags, finite floats, pass /
pipeline / buffer / texture state, ranges of draws against buffer sizes, a pass never draws into a bound texture, copies inside the images) and queues it; nothing is queued if
it is wrong. When it runs, index values are checked against the vertex buffer (the buffer can change after the stream was checked); a failure there loses the context.

Shaders and pipelines are device objects made through `create_shader` / `create_pipeline` or the private-data ABI of `escape()` (`EscapeOp`, documented in
`DxgKernel.hpp`). A shader is a built-in program (BuiltinShaders), TGSI text (virgl), Direct3D 9 tokens passed through (SVGA3D), or **Direct3D 9 bytecode** (`ShaderKind::D3d9Bytecode`):
`D3d9Shader` decodes and checks vs_1_1/2_0/3_0 and ps_2_0/3_0 once, then interprets it per vertex / pixel as the native function the software device runs (register /
varying / constant mapping and the supported instruction set are in `D3d9Shader.hpp`). Constants are the 8 push-constant registers plus the shader's own `def`s.

The same shader is also translated to TGSI text (`D3d9Tgsi.cpp`, `D3d9Program::emit_tgsi`), which `create_shader` hands to the device beside the native function; the virgl device
takes the text, the software device the function, and each ignores the other. The text means what the interpreter computes: outputs go through shadow registers that start at
zero and carry the position's Y flip, modifiers TGSI has no syntax for (bias, sign, complement, x2) are worked out into scratch registers, shift and saturate use MUL / MAX / MIN,
and instructions without a TGSI counterpart (lit, dst, sincos, crs, nrm, lrp, dp2add, the matrix instructions) are expanded. A shader the text cannot say (a relative address
together with `def`s, a constant beyond 1e9, a read of a0) still works on the interpreter; the device then has no text. `DxgkrnlTest` runs the emitted text on a small TGSI machine
and compares it with the interpreter on every instruction with varied sources, swizzles, modifiers, masks and result modifiers (about 3,000 programs), plus control flow,
texture sampling, texkill, relative addressing and truncated / bit-flipped streams. The text was **not** run through a real TGSI parser or virglrenderer.

**Shader Model 1 pixel shaders** (`D3d9Ps1.cpp`, ps_1_0 .. ps_1_4) work on every back end through the same instruction form. Their model is the texture stage: the result is r0, `tex t0` turns
the texture register into the sample of stage 0 at coordinate set 0, 1.4 has `texld` / `texcrd` with temporaries. Mapping: t0 / t1 are TEXCOORD0 / 1 until written, then temporaries; cnd
is "0.5 < a" and a select; the source and result modifiers (`_bx2`, `_x2`, `_sat`, ...) and coissue are taken. Not taken: texbem, texreg2*, texm3x*, texdp3*, texdepth, bem, stages 2 and 3
(the devices have two units and two coordinate sets). The fixed-point ranges of the model (registers clamped to [-1, 1]) are not imitated. Checked on hand-written programs for each
instruction group against their definitions, as TGSI text, and with a ps_1_4 shader from Wine's HLSL compiler (it cannot make `tex` for 1.1 - 1.3).

**DXBC** (`ShaderKind::Dxbc`, `D3d9Dxbc.cpp`): the container the Direct3D 10/11 runtimes pass a driver, Shader Model 4.0 / 4.1 / 5.0 / 5.1 vertex and pixel shaders (5.1 declares constant buffers, textures and samplers as ranges with a space: `cb[ID][index][element]`, `t[ID][index]`; only space 0, single-register ranges, `b0` as the push constants and `t0`/`t1` are taken). The container is
checked, the SHDR / SHEX program and the ISGN / OSGN signatures are read, and the program is decoded into the same instruction form, so the interpreter, the TGSI text and the
device forms below all apply. Registers map as in Direct3D 9 (`vN` = attribute location N, outputs and pixel inputs by COLOR0/1 and TEXCOORD0/1, SV_Position, `cb0[0..7]` the push
constants, literals become constants, `t0`/`t1` 2D textures). Taken: the float arithmetic, dot products, comparisons (as 1.0 / 0.0), movc, rounding, sincos / sqrt / rsq / exp / log / div,
sample (with a swizzled resource), if_z / if_nz / else / endif, discard, saturate, negate / abs. Refused: integer and bitwise instructions, loops, switches, calls, ld / sample_l / sample_b /
sample_c / sample_d, derivatives, relative addressing, constant buffers other than cb0, indexable temps, geometry / hull / domain / compute shaders, SV_Position as a pixel input, depth
output. Checked against an oracle for every arithmetic instruction (swizzles, modifiers, masks, saturate), against the TGSI text, and against four blobs from a real HLSL compiler
(Wine's d3dcompiler_47 = vkd3d-shader; `tests/data/dxbc`, sources beside them), compiled for 4.0, 5.0 and 5.1 and required to give identical results; Microsoft's fxc was not available.

**Devices without a shader compiler** (`D3d9Devices.cpp`; `create_shader` offers each what it understands in the module description):
- *VMware SVGA3D* takes the Direct3D 9 token stream itself. `emit_svga_tokens` rewrites a Direct3D 9 vs_2_0 / vs_3_0 / ps_2_0 / ps_3_0 shader for the device: input declarations name
  location 0 the position, 1 the colour and 2.. texture coordinates (what the device's vertex declaration says), and the position (`oPos`, or in Shader Model 3 the output register
  declared as the position) goes through a spare temporary and `mul <position>, rT, c31` (the Y flip constant the device supplies). A program made from DXBC is first written as a
  Direct3D 9 stream (`D3d9Emit.cpp`, `emit_d3d9_tokens`: vs/ps_2_0, or 3.0 when it branches on a register) and then rewritten; instructions Direct3D 9 lacks (div, sqrt, seq / sne,
  floor / ceil / trunc / round, movc, dp2 in vertex shaders, discard, sin / cos) are built from the ones it has, checked by compiling the stream again and running it beside the
  original. Refused: other models than 1.1 (vertex) / 2.0 / 3.0, a vertex shader with no temporary left (vs_2_0 has 12) or its own `def c31`, a program that needs more than 22 constants.
  The device now also sets the push constants for pixel shaders and takes sampled textures (`kImageSampled` images with `write_pixels`, up to two units, nearest / linear, wrap / clamp,
  mip filter, level clamp and bias, mipmapped images with `write_mip_pixels` / `generate_mipmaps` made on the CPU) - written from the protocol as I remember it (texture states 8..12 and 21 / 22, surface hint 1<<1), no header was at hand; none of it ran on a VMware host. Vertex shaders of Shader Model 1.1 go too (the rewrite reads their instruction lengths from the opcodes). Pixel shaders of Shader Model 1 (ps_1_0 .. ps_1_4) are decoded too (`D3d9Ps1.cpp`) and go to the device as they are. The stencil test works too (render states 8, 13..15, 38..41 and the stencil clear; the numbers follow the enumeration whose other members the device already used, still from memory). Still without:
  the R8G8B8A8 format, multiple render targets.
- *Intel Gen9* has no compiler for its execution units. Its vertex stage already runs on the CPU, so it now takes a vertex shader as a function (any attributes up to four, the push
  constants, one varying handed to the pixel kernel); its pixel stage stays one of two fixed kernels, and `classify_fragment` says whether a pixel shader (D3D9 or DXBC) is exactly one:
  `mov oC0, v0` or a texture sample at a varying (unit 0, `texld`/`sample` into a temporary then moved, or straight into the output). Any other pixel shader is refused there. Tested
  on the simulated engine against the software device (`Gen9GraphicsDeviceTest`); not on hardware.

## Scheduling and synchronization

Each context has an in-order queue of render / signal / wait / present entries. `pump()` runs queue heads, highest priority first, contexts of one priority in turn, until
everything is empty or waiting for a fence. Fences are 64-bit monotonic counters, semaphores count signals up to a limit; both can be signalled and waited on from the CPU
or ordered with a context's work (`signal_sync_gpu` / `wait_sync_gpu`). A context that fails is lost (`DeviceState`: `PageFault`, `ErrorOutOfMemory`, `Hung`, with a reset
count), its queue is dropped but the fence signals still happen, and destroying a context does the same, so nobody waits forever. `present()` flips a `kAllocPresent`
allocation to the screen (`IGraphicsDevice::present`) or blits a region into one and shows that.

## The GPUs

Every 3D device (`graphics::IGraphicsDevice`) is a miniport. `register_dxg_adapters` (SystemDxg.cpp) makes one adapter per GPU of the graphics pool from the pool's device, and an
accelerated one first draws the Direct3D self-test (`SelfTest.cpp`: the vs_2_0 / ps_2_0 first triangle, through the same services an application uses) on a throw-away kernel; a device that
fails it is replaced by the CPU renderer for Direct3D and boot says so. What each device takes (`Adapter::create_shader` offers every form in the module description, a device uses what it
understands):

| Device | Shaders | Notes |
|---|---|---|
| Software (CPU) | the interpreter's functions | everything: textures, mipmaps, stencil, any program. |
| virgl (QEMU) | TGSI text | from D3D9 / SM1 pixel / SM2-5.1; never run against a real virglrenderer. |
| VMware SVGA3D | Direct3D 9 tokens (D3D9 rewritten, DXBC written out first) | textures, mipmaps, stencil written from memory of the protocol; not run on a host. |
| Intel Gen9 | a vertex function on the CPU + its two fixed pixel kernels | pixel shaders must be exactly a colour or one texture sample (`classify_fragment`); simulated engine in the host tests, picture matches the software device. |
| Raspberry Pi V3D | its fixed vertex and pixel programs | a vertex shader that is exactly the matrix transform (`classify_vertex`, tested by running it on vectors against the built-in shader; rows or columns, Y flipped for Direct3D) and a `mov oC0, v0` pixel shader; no textures or stencil. The device is not yet in the render nodes (only the Pi session's tests make one), so a Pi has no accelerated Direct3D until it is. |
| AMD (in progress) | - | no `IGraphicsDevice` exists yet; the Vega / Raven work is hardware drivers only. |

`AdapterCaps` says what the adapter found by trying (`stencil`, `mipmaps`, `textures`, `programmable_vertex`, `programmable_pixel`), so a user-mode driver can pick shaders and formats the
device takes. **A new device, the AMD one included, is done when**: `create_image` takes a colour target (B8G8R8A8) and a D32Float depth image and `read_pixels` works; it takes the
built-in shader pair or any of the module forms of `ShaderModuleDesc` (function, `tgsi`, `d3d9_tokens`, or a fixed-kernel name plus `eu_varying` / `qpu_*`) for the Direct3D 9 pair
of the self-test; `create_graphics_pipeline` accepts position float3 at location 0 and colour float4 at location 1; and the host suite (`tests/DxgkrnlDeviceSuite.hpp`:
`dxgk_suite::compare(allocator, device, display, name, tolerance, allowed_bad)`) draws the same picture as the software device. Nothing in the kernel is vendor specific: once the
device is in the render node, Direct3D has it (and the self-test falls back to the CPU renderer until it draws correctly).

## Not done

- **The user-mode half and the system-call glue**: no `NtGdiDdDDI*` services in `src/Process/Windows*.cpp` / `win32u`, no d3d9 / dxgi / d3d11 DLLs. `lock()` returns a view of
  kernel memory; mapping it into the process (a section) is the system-call layer's job.
- **Shaders**: SM2 / SM3 / SM4 / SM5.0 / SM5.1 vertex and pixel shaders (no geometry / tessellation / compute shaders, no 5.1 register spaces or arrays of resources). The pixel stage of Intel Gen9 is two fixed kernels, so a general pixel
  shader runs there only on the software, virgl (as unvalidated TGSI) and SVGA3D (unvalidated) paths. No loops, calls, predication, ps_1_x, constants above c7 (push constants are
  128 bytes), depth output, fog / point size, cube and volume samplers. Pipeline state is limited to what `IGraphicsDevice` offers (one colour target, 2 textures, alpha blend on/off, depth and stencil).
- No GPU writes to buffers (stream-out, UAVs), no compute, no queries / occlusion, no multisampling, no cube / volume textures, no 16-bit or float formats.
- Sync objects cannot be shared between processes. No timeout detection and recovery (a hung device cannot be reset), no vblank / scan line, gamma, overlays or VidPN topology
  (multi-monitor); display mode listing and switching go straight to the GPU driver.
- Video memory is accounted per adapter but there is no per-process budget or priority-based paging between processes.
- Run through the kernel: the software rasterizer (everything) and the simulated Intel Gen9 engine (the self-test picture matches the software device). virgl, SVGA3D and the V3D hardware were not run here (V3D has no host simulation of its rasterizer; its matrix handling is checked numerically). One lock covers the kernel.
