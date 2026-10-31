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
`BindIndexBuffer`, `PushConstants`, `BindTexture`, `SetViewport`, `SetScissor`, `Draw`, `DrawIndexed`, `CopyRegion`, `GenerateMipmaps`, `BeginQuery`, `EndQuery`, `BindUav`, `Dispatch`, `ResolveRegion`. Allocations are named by index into
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

## Direct3D 10 and 11

The kernel-mode driver is the D3DKMT layer, which Direct3D 9, 10, 11 and 12 all sit on; nothing in the services (adapters, allocations, residency, fences, contexts, queues, `render`,
`present`, `escape`) is specific to a Direct3D version. What decides how much of Direct3D 10 a program gets is the shader front end and the command stream.

- **Shaders:** DXBC (the Direct3D 10 / 11 shader container, Shader Model 4.0 - 5.1) is the format of `ShaderKind::Dxbc`: vertex and pixel shaders, float and integer arithmetic and
  bitwise instructions (integers live in the register's 32 bits, comparisons give all ones or zero, negate / abs of an integer source are integer ones), `movc`, `if` / `else`,
  `loop` / `endloop` with `break` / `breakc` / `continue` / `continuec` (a run stops after 2^20 instructions, so a loop with no exit cannot hang the machine), `discard`, `sample`,
  conversions (`ftoi`, `ftou`, `itof`, `utof`, saturating), `sincos`, the rounding and reciprocal family, `imul` / `umul` low words and `udiv`.
- **Constant buffers:** `cb0..cb13`, any size up to 256 vec4s in all. The push-constant block is the declared buffers laid end to end in slot order (the user-mode driver sees the
  same `dcl_constant_buffer`s, so it builds the block), `cb[n][r1.x + 3]` indexes by an integer register (past the end of the buffer reads zero), and the immediate constant buffer
  (`icb[...]`, up to 512 vec4s) is part of the program. A device says how many bytes of constants it takes (`DeviceInfo::max_push_constant_bytes`: 4096 for the software device, 128
  for the others); a shader that reads more is refused on a device that takes less (`kNotSupported`).
- **Topologies:** `SetTopology` in the command stream selects triangle list, strip or fan (restart index 0xFFFF / 0xFFFFFFFF in strips); a strip or fan is expanded to a list of
  triangles by the kernel with an index buffer made for the draw, so every device draws them.
- **Formats:** R8G8B8A8 / B8G8R8A8 colour (`A8B8G8R8` / `A8R8G8B8`), D32 and D24S8 depth - what the D3D10 swap chain and back buffers use.
- **Where the interpreter is the only back end:** integer and bitwise instructions, loops with integer tests, indexing constant buffers by a register and constant blocks above 128 bytes
  do not become TGSI text, Direct3D 9 tokens or a fixed kernel (they are refused there with `kNotSupported` - the adapter keeps the function for the software device). Loops over floats
  do become TGSI (`BGNLOOP` / `BRK`).
- **The device interface (`DeviceInfo::features`):** the base `IGraphicsDevice` stays small; a device lists the extras it has as feature bits and the adapter copies them to
  `AdapterCaps::features`. The kernel refuses (`kNotSupported`, or an invalid stream) state, shaders and draws that need a feature the device lacks, instead of drawing something else.
  Only the software device has them so far:
  - `kFeatureBlendState`: `GraphicsPipelineDesc::blend` (factors, ops and write mask for the colour and the alpha on their own, `custom` selects it over `alpha_blend`) and
    `depth_compare` (any of the eight comparisons).
  - `kFeatureTexelAccess`: `ITextureSet::fetch` (Load), `sample_lod` (SampleLevel), `size` (GetDimensions); DXBC `ld`, `sample_l`, `resinfo`. For the images of `kFeatureTextureKinds` (one level) there are `fetch_volume` (x, y and the slice or layer; a cube has no Load), `sample_lod_volume` and `size_volume` (the depth or layer count next to the width and height); TGSI `TXF` / `TXL` / `TXQ` with the `3D`, `2D_ARRAY` and `CUBE` targets. All three kinds match the real GPU.
  - `kFeatureSystemInputs`: `VertexShaderFnEx` / `FragmentShaderFnEx` carry `SV_VertexID`, `SV_InstanceID`, `SV_Position` (pixel centre, depth, 1/w), `SV_IsFrontFace` and
    `SV_PrimitiveID`; the DXBC front end reads them from the declarations (`dcl_input_sgv` / `siv`), and a program that uses them needs the feature.
  - `kFeatureInstancing`: `draw(..., instance_count, first_instance)` and `VertexAttribute::per_instance` (the element numbered by the instance, in the same buffer at the same stride);
    the command stream has `DrawInstanced` / `DrawIndexedInstanced`.
  - `kFeatureLines`: `set_primitive_topology` for line lists and point lists, one pixel wide (a step per pixel along the longer axis, perspective-correct varyings); the stream's
    `Topology::LineList`, `LineStrip` (expanded to a list) and `PointList`.
  The escape ABI's `CreatePipeline` takes the blend state, the depth comparison and the per-instance bits as optional trailing words.
- **Checked against a real compiler:** `ps_texel` (Load, SampleLevel, GetDimensions, SV_Position, SV_IsFrontFace, SV_PrimitiveID) and `vs_ids` (SV_VertexID, SV_InstanceID with a
  per-instance input) from Wine's HLSL compiler run through the kernel on the software device with the expected pictures.
- **Not done for Direct3D 10:** stream output and geometry shader features beyond the basics (see Geometry shaders below), `sample_b` / `sample_c` / derivatives, more than four vertex attributes, more than four varyings (and semantics
  beyond COLOR0/1 and TEXCOORD0/1), more than one vertex buffer slot, multisampling, predication and queries other than occlusion counts (`D3D10_QUERY_OCCLUSION`; `OCCLUSION_PREDICATE` and `SO_STATISTICS` etc. are not done), depth bias and wireframe fill, texture arrays
  with mipmaps, mipmapped / filtered-across-faces cube and 3D textures, cube arrays, packed formats (float, half-float, sRGB, 16-bit unorm and R32G32B32A32_UINT colour formats are done; the other integer widths, SNORM and 10-bit formats are not), UAV textures, typed UAV loads and stores, append / consume buffers, group-shared memory and barriers, indirect dispatch (Direct3D 11 compute without those). The other devices (virgl, SVGA3D, Intel Gen9, V3D) have none of the features above, so
  they run the Direct3D 9 level only.

## Conformance on a real GPU

`Conformance.cpp` (`dxgk::run_conformance`) draws 41 small scenes through the services on one adapter and on the CPU renderer and compares the 32 x 32 pictures: Direct3D 9 vertex colours, Shader
Model 1 and 3 bytecode, depth, alpha blend, indexed draws, viewport and scissor, culling and winding (into a target, a texture and a back buffer), copies; Direct3D 10 constant buffers
(also 1 KiB, indexed by an integer register), point and linear textures, pixel shader arithmetic, `Load` / `SampleLevel` / `GetDimensions` with `SV_Position` and `SV_IsFrontFace` (into a
texture and into a back buffer, whose rows are stored the other way up), strips, blend state, depth comparison, stencil, lines and points, per-instance attributes, `SV_InstanceID` /
`SV_VertexID`, two render targets (`SV_Target0` / `SV_Target1`, the render pass takes up to four colour images: `kFeatureMultipleTargets`, `CommandWriter::begin_pass_targets`, `PipelineDesc::color_target_count`); half-float and float render targets that are then sampled (`A16B16G16R16F` / `A32B32G32R32F`, additive blending that exceeds 1.0; `kFeatureFloatFormats` on the CPU renderer and virgl, one mip level, no present; the memory manager converts the backing to and from `Vec4`); occlusion queries (a near quad, then three queried quads: in the open, half behind it, wholly behind it - the counts 256 / 128 / 0 are drawn as the colour of three stripes); sRGB (two triangles blended on linear values into an sRGB target, shown as stored and as sampled), 16-bit normalised (twenty additions of 1/10000 survive in 16 bits and vanish in 8) and integer (written as integers, read back with `Load`) targets; tessellation (`hs_tri` / `ds_dent`: a three-point patch with all factors 2, the domain shader bending the edges by the products of the coordinates); a geometry shader (`gs_copy`: each triangle comes out as itself and as a half-size copy with red and blue swapped); 4x multisampling (a far and a near rectangle with fractional edges, depth tested per sample, resolved into a plain image and into a texture-capable one); a compute shader with unordered access views (`cs_fill`: 4 x 8 groups of 8 x 4 threads fill a 32 x 32 picture in a structured buffer from `SV_DispatchThreadID` / `SV_GroupID` / `SV_GroupIndex`, with atomic counters that must reach 1024); independent blend per render target (target 0 adds, target 1 does not blend; `kFeatureIndependentBlend`, `PipelineDesc::independent_blend` / `target_blend`, the escape ABI's trailing words); a volume, an array and a cube texture sampled by `Texture3D` / `Texture2DArray` / `TextureCube` pixel shaders (`kFeatureTextureKinds`); Direct3D 11 render to texture. A scene is *Same*, *Differs*, *Refused* (the device says `kNotSupported`: a program can fall back) or *Broken* (a step failed or the context was
lost); a scene in which the reference draws nothing is Broken too. The system runs it at boot on each accelerated GPU and logs one line per scene (`Direct3D: ...`); the host test runs it on
two software adapters (all the same) and on a device that is told it has no Direct3D 10 features (those scenes are refused). The DXBC fixtures are compiled by Wine's d3dcompiler
(`tests/data/dxbc/*.hlsl`, `scripts/gen_conformance_blobs.py` writes `ConformanceBlobs.hpp`).

What running it on virglrenderer taught (and is now fixed): a render target must be creatable together with sampling (virgl images are stored top row first when they are sampled; the viewport
is not flipped for them), a stencil plane must be `Z32_FLOAT_S8X24_UINT` (format number 19, the one the name `Z24_UNORM_S8_UINT` suggests, makes the host lose the whole context), the host's GLSL
has no `gl_PrimitiveID` in a pixel shader (`SV_PrimitiveID` is refused for virgl, not sent), the TGSI text parser takes no hexadecimal integer literals, and a push-constant packet could not carry
more than 248 bytes although the adapters advertise 4096.

## The GPUs

Every 3D device (`graphics::IGraphicsDevice`) is a miniport. `register_dxg_adapters` (SystemDxg.cpp) makes one adapter per GPU of the graphics pool from the pool's device, and an
accelerated one first draws the Direct3D self-test (`SelfTest.cpp`: the vs_2_0 / ps_2_0 first triangle, through the same services an application uses) on a throw-away kernel; a device that
fails it is replaced by the CPU renderer for Direct3D and boot says so. What each device takes (`Adapter::create_shader` offers every form in the module description, a device uses what it
understands):

| Device | Shaders | Notes |
|---|---|---|
| Software (CPU) | the interpreter's functions | everything: textures, mipmaps, stencil, any program, and all the Direct3D 10 features above. |
| virgl (QEMU) | TGSI text | from D3D9 / SM1 pixel / SM2-5.1; run against a real virglrenderer (QEMU `virtio-gpu-gl`, host GL 4.6) by the conformance scenes below: all of them draw what the CPU renderer draws. Blend state, depth comparison, stencil (Z32F + S8 image), lines and points, instancing (per-instance attributes get a vertex-buffer binding of their own, the host's divisor belongs to the binding), integer and bitwise ops, loops, `SV_VertexID` / `SV_InstanceID` / `SV_Position` / `SV_IsFrontFace`, `Load` / `SampleLevel` / `GetDimensions`, constant buffers up to 4 KiB with integer indexing, render targets that are textures. |
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

## Occlusion queries

`BeginQuery allocation, offset` / `EndQuery` inside a pass count the fragments that pass the stencil and depth tests (and that the shader did not discard) between them; once the stream has
run, the count is a little-endian 64-bit number at `offset` of the allocation. The allocation is a `kAllocQueryResult` buffer: system memory only, never resident, read with
`lock()` like any other (so after the stream has run, as `lock` waits for queued work). One query is open at a time, it ends before the pass does, and a stream has at most
`kMaxOcclusionQueries` (64). The adapter needs `kFeatureOcclusionQueries` (the CPU renderer counts in `write_fragment`; virgl uses the host's `OCCLUSION_COUNTER`); without it
creating the buffer returns `kNotSupported`. virgl notes: the host takes no offset into a shared result buffer (two queries on one buffer overwrite each other), so each query
has a 16-byte buffer of its own created with `VIRGL_BIND_CUSTOM` (host system memory); the host writes `{state, size, result}` straight into the buffer's guest memory when its GL
query is complete - which it notices while commands with fences are arriving, so `submit` sends empty commands until every state reads DONE - and a transfer from the host would
overwrite it with its stale copy, so none is made.

## Volume, array and cube textures

An allocation has a `kind` (`graphics::TextureKind`: `Texture2D`, `Texture3D`, `Texture2DArray`, `TextureCube`) and `layers` (the depth of a volume, the size of an array, 6 for a cube whose
width equals its height, at most `kMaxTextureLayers` = 256). They are sampled-only (`kAllocTexture`, optionally `kAllocShared`), one mip level, 8-bit colour, and need the adapter's
`kFeatureTextureKinds` (`kNotSupported` otherwise); the lock shows all layers one after the other (`pitch` per row, `pitch * height` per layer). A DXBC pixel shader declares the kind with
`dcl_resource_texture3d / texture2darray / texturecube`; `D3d9Program::texture_kind(unit)` records it, the TGSI text says `3D` / `2D_ARRAY` / `CUBE`, `ld` / `sample_l` / `resinfo` stay 2D-only,
and `draw` refuses a texture whose kind is not the one the pixel shader declared. Sampling (`ITextureSet::sample_volume`, third coordinate `w`): a volume's `w` is 0..1 over the slices (blended
by a linear filter, clamped), an array's is the layer (rounded, clamped), a cube's coordinate is a direction (+X, -X, +Y, -Y, +Z, -Z faces; no filtering across faces). virgl: resource targets 3D / 2D_ARRAY / CUBE,
layers staged one at a time through a one-layer backing (transfer box `z` = the layer), the sampler's wrap R is clamped, the view's last layer is `layers - 1` for arrays and cubes and 0 for a volume. The 33 scenes
pass on virglrenderer; pixel centres on exact texel boundaries are avoided in the array scene (devices round ties either way).

## Compute shaders and unordered access views

`cs_5_0` DXBC compute shaders run through `ShaderKind::Dxbc` with `ShaderStage::Compute` (the escape ABI's `CreateShader` stage 2). A compute pipeline has no state: `PipelineDesc::compute_shader`
(or `EscapeOp::CreateComputePipeline`) with the shader's handle. Buffers are `kAllocUnorderedAccess` allocations (vertex / index flags may be added; the adapter needs `kFeatureCompute`).
A stream binds them outside a pass with `BindUav slot, allocation, offset, size` (slots below `kMaxStorageBuffers` = 4, offset and size multiples of 4), binds the pipeline with `BindPipeline`
(a compute pipeline leaves the graphics one alone), sets constants with `PushConstants` (cb0) and runs `Dispatch x, y, z` (each at most 65535, at most 2^24 threads in all; every slot the shader
declared must be bound). The GPU's writes come back to the CPU like a render target's: `lock()` reads them. Taken: `dcl_thread_group`, `dcl_uav_raw` / `dcl_uav_structured`, `SV_DispatchThreadID`,
`SV_GroupID`, `SV_GroupThreadID`, `SV_GroupIndex`, `ld_raw` / `store_raw` / `ld_structured` / `store_structured` (a write outside the buffer is dropped, a read there is zero) and the atomics (add, and,
or, xor, min, max, exchange, with and without the old value) on either kind of buffer, plus all the integer and float arithmetic, loops and branches. The software device runs the groups one after the
other and the threads of a group in order (`ComputeContext`, `ComputeShaderFn`), so atomics are trivially atomic. virgl gets TGSI (`COMP`, `PROPERTY CS_FIXED_BLOCK_*`, `DCL SV BLOCK_ID /
THREAD_ID`, `DCL BUFFER[n]`, `LOAD` / `STORE` / `ATOM*` with byte addresses; the dispatch number and group index are worked out in a prologue; the system values must be read with three-component swizzles
because the host's are `uvec3`s), `SET_SHADER_BUFFERS`, `LAUNCH_GRID` and a memory barrier; storage buffers are created with `VIRGL_BIND_SHADER_BUFFER` (1 << 14), and `IBuffer::read_back()`
transfers what the shader wrote. The device claims `kFeatureCompute` only when the host's capability set says GLSL 4.30 or later (`VirtioGpu::virgl_has_compute()`). The HLSL compiler at hand
(Wine's) has no `ByteAddressBuffer` methods, so the raw-buffer path is tested with a shader written token by token (`test_compute`); the conformance shader uses structured buffers.
The conformance run on virglrenderer is the only check of the TGSI path against a real host.

## Multisampling

An allocation with `samples` 2, 4 or 8 (the adapter's `kFeatureMultisample`; `DeviceInfo::sample_counts` says which) is a render target of an 8-bit colour format or a depth buffer, nothing
else: no system-memory backing (so no lock, copy, texture or present), one level. Every image of a pass has the same count. `ResolveRegion destination, source` (outside a pass) averages the
samples of a multisampled colour target into an ordinary image of the same format and size - a render target, a texture, a back buffer. Coverage is per sample when the pipeline says
`multisample` (D3D10 `MultisampleEnable`): a triangle covers the samples whose standard Direct3D positions (2x: (4,4) (-4,-4); 4x: (-2,-6) (6,-2) (-6,2) (2,6); 8x as in `SoftwareDevice.cpp`, in
sixteenths of a pixel) it contains, depth and stencil are tested for each, the shader runs once per pixel at its centre, and the colour goes to the covered samples that pass; without
`multisample` the pixel is all or nothing by its centre. Lines and points cover every sample. Occlusion queries count samples. The contents of a multisampled image do not survive being
evicted from video memory (there is nothing to read them back into). virgl: resources with `nr_samples`, the rasterizer's multisample bit, and `VIRGL_CCMD_BLIT` for the resolve (the host
keeps a multisampled image the GL way up whatever flag it was made with, so the blit turns the picture over only for a destination kept upright); 2 and 4 samples, as far as the host's
`max_samples` goes. The conformance scene uses only vertical and horizontal edges, which every device with the standard patterns covers the same whatever the order of the samples.

## Geometry shaders

A `gs_4_0` / `gs_5_0` DXBC shader (`ShaderStage::Geometry`, escape `CreateShader` stage 3) goes into a pipeline with `PipelineDesc::geometry_shader` (the escape ABI's last optional trailing word); the
adapter needs `kFeatureGeometryShaders`. It runs once per list primitive - the kernel expands strips and fans first - between the vertex shader and the rasterizer, and a draw must make what it
takes (`dcl_inputprimitive` point, line or triangle; no adjacency): `draw` refuses a topology that does not match. It emits up to `dcl_maxvertexcount` (at most `kMaxGeometryVertices` = 64) vertices with `emit`
and ends a strip with `cut`, into a point list, line strip or triangle strip. Inputs are `v[vertex][register]` with immediate indices (what the compiler writes once a loop over the vertices is unrolled);
outputs are the vertex shader's: SV_Position, COLOR0/1, TEXCOORD0/1. Not taken: relative vertex indices. The
vertex shader's positions come with +Y down (the devices' convention) and a geometry shader computes in Direct3D's clip space, so they are turned over on the way in and out (the CPU interpreter in
`run_geometry`, the TGSI in its prologue and its `EMIT`s). virgl gets `GEOM` TGSI (`PROPERTY GS_INPUT_PRIMITIVE / GS_OUTPUT_PRIMITIVE / GS_MAX_OUTPUT_VERTICES`, `DCL IN[][n]`, `EMIT` / `ENDPRIM`), a
shader of type 2 and a `BIND_SHADER` for it on every pipeline bind (handle 0 for a pipeline without one: the host keeps the last geometry shader, across submits too); it claims the feature when the host's GLSL
level is 1.50 or later.

## sRGB, 16-bit normalised and integer formats

`A8R8G8B8Srgb` / `A8B8G8R8Srgb` (`DXGI_FORMAT_*_UNORM_SRGB`), `A16B16G16R16` (four 16-bit normalised channels) and `A32B32G32R32Uint` (four unsigned integers) are allocation formats, each behind a
feature of the adapter (`kFeatureSrgbFormats`, `kFeatureUnorm16Formats`, `kFeatureIntegerFormats`; `kNotSupported` without it, except for staging memory). They are render targets and textures, not
presentable, and the 16-bit and integer ones (like the floats) have one level and no multisampling; an sRGB image may have mip levels (the chain is made from the stored codes, not from linear values) and
may be multisampled (a resolve averages linear values). **sRGB:** a texture is decoded to linear values when it is read (before filtering), a render target takes linear values from the shader,
blends on linear values and stores the encoded result (a clear colour is linear too); alpha is linear. The CPU renderer builds the decode table without a maths library and encodes by searching
it. **16-bit:** the CPU renderer keeps Vec4s rounded to 1/65535 (clamped to 0..1); the memory manager converts the backing to and from them. **Integer:** the Vec4s hold the integers as float bits - a pixel
shader writes them as bits (`SV_Target` of `uint4`), `Load` returns the bits, the clear colour of a pass is the integers as float bits (not checked for finiteness), a pipeline that blends cannot draw
into one (refused) and sampling with a filter is meaningless (use `ld`; `Sample` of an integer texture returns garbage). virgl does sRGB (`B8G8R8A8_SRGB`, 100) and 16-bit normalised
(`R16G16B16A16_UNORM`, 51) and the integer format (`R32G32B32A32_UINT`, 196; the ids come from virglrenderer's `virgl_hw.h`), not `R8G8B8A8` sRGB; all the ids were right on the host.

## Tessellation

`hs_5_0` / `ds_5_0` DXBC shaders (`ShaderStage::Hull` and `Domain`, escape stages 4 and 5) go into a pipeline with `PipelineDesc::hull_shader` / `domain_shader` (both or neither; the escape
ABI's last two optional trailing words); the adapter needs `kFeatureTessellation`. The draws are `PatchList` topology (`SetTopology` with `kPatchListBase` + n, `patch_list(n)`, n = 1 .. 32 control
points), and the pipeline's hull shader must take that many; patches go with tessellation pipelines and lists with the others (`draw` refuses a mismatch, and a geometry shader after the tessellator).
The vertex shader runs on each control point, the hull shader's control point phase makes the output control points and its fork and join phases (one instance each) the patch constant registers
(the tessellation factors are the ones declared with `SV_TessFactor` / `SV_InsideTessFactor` names; user constants reach the domain shader as `vpc[n]` on the CPU renderer), the tessellator
makes the domain points and the triangles (or lines, or points) joining them, the domain shader the vertices. Taken: the triangle, quad and isoline domains, integer, power-of-two and
fractional (odd and even) partitioning, `triangle_cw` / `triangle_ccw` / `point` / `line` output, immediate or `vOutputControlPointID` control point indices. **Not made:** adjacency.
The tessellator is `Graphics/Graphics3D/Software/Tessellator.hpp`, after the published Direct3D 11 reference one: the outer ring has as many segments as the edge factor, the inner rings two
segments less each (a triangle's concentric about the centre, so their points are not on the lattice; a quad's the rectangles of the lattice), an inside factor of 1 is taken as 2, all factors 1 is
the patch itself, a factor not above 0 (or NaN) on an edge culls the patch, factors are clamped to 1..64; the first inner ring is joined to the outer one in the order of the "ruler function", the
others by a fixed pattern, and an odd smaller factor leaves a strip of quads in a quad's middle. Points and triangles agree pixel for pixel with a real GPU (virglrenderer) for triangles and quads
of factors 1 to 8 with edges that differ from the inside, isolines and point output. **Winding:** with the usual domain shaders (V up on the screen) `triangle_cw` patches are back faces under
Direct3D's default front face on the real GPU, and the CPU renderer orders the indices so as to agree. virgl takes the output primitive (winding, point mode) from the hull shader the domain
shader is paired with (and its spacing, `TES_SPACING` 0 / 1 / 2): `VirglPipeline` makes a copy of the evaluation shader with those properties patched. Fractional partitioning matches the real GPU too. The constants (push constants) are set for the geometry, hull and domain stages
of the host too, which they were not before.
The vertex shader's positions come with Y down and the hull and domain shaders work in Direct3D's clip space, so the CPU renderer turns them over on the way in (`run_hull`) and out
(`run_domain`). virgl: `TESS_CTRL` TGSI (the control point phase written once for each output control point behind a test of `INVOCATIONID`, since the per-vertex arrays are then written with
constant indices; the factors in the `TESSOUTER` / `TESSINNER` outputs), `TESS_EVAL` TGSI (`TES_PRIM_MODE 4`, `TES_SPACING 2`, the values of these properties are numbers), shader types 3 and 4,
a `BIND_SHADER` of both on every pipeline bind, and `DRAW_VBO` of `PIPE_PRIM_PATCHES` with the two extra words (control points per patch, draw id); the feature is claimed when the host reports GLSL 4.00.

## Not done

- **The user-mode half and the system-call glue**: no `NtGdiDdDDI*` services in `src/Process/Windows*.cpp` / `win32u`, no d3d9 / dxgi / d3d11 DLLs. `lock()` returns a view of
  kernel memory; mapping it into the process (a section) is the system-call layer's job.
- **Shaders**: SM2 / SM3 / SM4 / SM5.0 / SM5.1 vertex and pixel shaders (no geometry / tessellation / compute shaders, no 5.1 register spaces or arrays of resources). The pixel stage of Intel Gen9 is two fixed kernels, so a general pixel
  shader runs there only on the software, virgl (as unvalidated TGSI) and SVGA3D (unvalidated) paths. No loops, calls, predication, ps_1_x, constants above c7 (push constants are
  128 bytes), depth output, fog / point size, cube and volume samplers. Pipeline state is limited to what `IGraphicsDevice` offers (one colour target, 2 textures, alpha blend on/off, depth and stencil).
- No UAV textures, no group-shared memory or barriers (a compute shader that needs `sync` or `dcl_tgsm` is refused), no multisampled shader resources (`Texture2DMS`, `ld2dms`), no depth resolve, no alpha-to-coverage or sample mask, no per-sample shading, no centroid interpolation, no mipmapped / render-target / float cube, volume or array textures, no integer formats but R32G32B32A32_UINT, no signed or 10-bit formats.
- Sync objects cannot be shared between processes. No timeout detection and recovery (a hung device cannot be reset), no vblank / scan line, gamma, overlays or VidPN topology
  (multi-monitor); display mode listing and switching go straight to the GPU driver.
- Video memory is accounted per adapter but there is no per-process budget or priority-based paging between processes.
- Run through the kernel: the software rasterizer (everything) and the simulated Intel Gen9 engine (the self-test picture matches the software device). virgl was run in QEMU against a real virglrenderer (the conformance scenes above); SVGA3D and the V3D hardware were not run here (V3D has no host simulation of its rasterizer; its matrix handling is checked numerically). One lock covers the kernel.


### More colour formats

`A32B32G32R32Sint`, `A16B16G16R16Uint` / `Sint`, `A8B8G8R8Uint` (the integer formats, narrow ones saturate), `A16B16G16R16Snorm` and `A2B10G10R10` (10-bit colour with two bits of alpha) join the wide formats: pixels travel as `Vec4`s, laid out and quantised by `Graphics/WidePixels.hpp` (one place for the software device, the memory manager and virgl). Features: `kFeatureIntegerFormats`, `kFeatureSnormFormats`. virgl ids (from virglrenderer's `virgl_hw.h`): R32G32B32A32_SINT 200, R16G16B16A16_UINT 188, _SINT 192, R8G8B8A8_UINT 180, R16G16B16A16_SNORM 59, R10G10B10A2_UNORM 8. Two conformance scenes match the real GPU. `SV_PrimitiveID` is a `PRIMID` input on virgl and matches too.

### More queries

`BeginQuery` takes an optional third word, a `graphics::QueryKind`: 0 occlusion count, 1 occlusion predicate (0 or 1), 2 IA vertices, 3 IA primitives, 4 VS invocations, 5 PS invocations (`kFeatureStatisticsQueries`; the result is still a 64-bit number in the `kAllocQueryResult` buffer). The CPU renderer counts them exactly (VS invocations once for every vertex it reads, including repeats of an index); a GPU may run the vertex shader fewer times and shade helper pixels, so the scene compares the predicates and the IA counts exactly and the shader runs only against "more than none". virgl: `PIPE_QUERY_OCCLUSION_PREDICATE` (1) and `PIPE_QUERY_PIPELINE_STATISTICS` (10 in virglrenderer's `p_defines.h`, which is not the numbering older Gallium used) with the statistic in the upper half of the type word. Not done: timestamps, time elapsed, stream output statistics.

### Alpha to coverage

`PipelineDesc::alpha_to_coverage` (with `multisample`; bit 1 of the multisample word of the escape, `kFeatureMultisample`): the samples a pixel's colour reaches are the first round(alpha * samples) of the covered ones, alpha being target 0's. virgl sets the blend state's `ALPHA_TO_COVERAGE` bit. On the real GPU tried, a fraction of an alpha gives a number of samples that varies from pixel to pixel (dither), so the conformance scene uses alphas of 0 and 1 and the rounding of the CPU renderer is a unit test (`test_alpha_to_coverage`).

### Geometry shaders: adjacency and instances

`Topology::LineListAdjacency` (0x100) and `TriangleListAdjacency` (0x101) draw lists of 4 and 6 vertices a primitive (the vertices in the order D3D says: a triangle's are v0, v1 (beside edge 0-2), v2, v3, v4, v5), only with a geometry shader whose `dcl_input_primitive` is `lineadj` / `triangleadj` (`GeometryPrimitive::LineAdjacency` / `TriangleAdjacency`, `PrimitiveTopology::*Adjacency`, `kFeatureGeometryShaders`); virgl `PIPE_PRIM_LINES_ADJACENCY` 10 and `TRIANGLES_ADJACENCY` 12. `dcl_gs_instance_count n` (n up to 32) runs the shader n times for each primitive with `SV_GSInstanceID` (`GeometryInputData::instance_id`, `ShaderModuleDesc::geometry_instances`); TGSI `PROPERTY GS_INVOCATIONS n` and an `INVOCATIONID` system value. The Wine HLSL compiler cannot build `[instance(n)]`, so `gs_copy_inst2.dxbc` is `gs_copy.dxbc` patched by `scripts/patch_gs_instances.py` to declare 2 instances (the picture is the same triangles drawn twice; the use of `SV_GSInstanceID` itself is not exercised by a conformance scene). Both match the real GPU. Several output streams and stream output are below.

### Compute: group-shared memory and barriers

`dcl_tgsm_raw` / `dcl_tgsm_structured` (g0 .. g3, 32 KiB together) and `sync_g...` (a barrier when its flags include "threads in group"; a memory-only sync is a no-op for the CPU, which runs one thread at a time, and a `MEMBAR` for the GPU) are taken. A program that uses either is run a whole group at a time (`ShaderModuleDesc::compute_group_function`, `D3d9Program::run_compute_group`): every thread has registers of its own (so at most 256 threads per group, `kMaxBarrierThreads`), runs up to its next barrier, and when all have stopped they go on; the barrier may be inside a loop, a thread that has finished does not wait. The loads, stores and atomics of the shared memories are the buffer instructions with the slot after the u# ones. virgl: TGSI `DCL MEMORY[0], SHARED` (only one shared region), `LOAD` / `STORE` / `ATOM*` on `MEMORY[0]`, `MEMBAR` + `BARRIER`, and the size in bytes goes in the shader-creation command (the field that holds the stream-output count for other stages). Two things the host's TGSI parser is picky about: the memory type follows a comma, and the `MEMBAR` operand needs a four-letter swizzle (`.xxxx`; `.x` is "Expected register swizzle"). `cs_shared.hlsl` (neighbour read across a barrier, a reduction with a barrier in its unrolled loop) matches the real GPU.

### Multisampled images as shader resources, and depth resolve

A multisampled colour allocation may also be a texture (`kAllocRenderTarget | kAllocTexture`, 8-bit colour). A pixel shader reads it with `Texture2DMS`: `dcl_resource_texture2dms` (the unit's `TextureKind::Texture2DMS`, which a draw checks against the bound allocation having several samples), `ld_ms` (`ITextureSet::fetch_sample`) and `sampleinfo` (the count the texture was declared with, a constant); TGSI `2D_MSAA` `SVIEW` and `TXF` with the sample in the fourth component. **The host's samples are in another order than Direct3D's** (found by drawing edges through a pixel and seeing which samples they covered): its sample 1 is Direct3D's 3 and the other way round for 4x, so the TGSI for `ld_ms` flips the second bit of an odd index (2x and 8x are not remapped, nor checked). `ps_ms.hlsl` and a scene that reads (x + y) mod 4 of each pixel match the real GPU.

`ResolveRegion` takes an optional third word, a `graphics::ResolveMode` (0 average, 1 sample 0, 2 least, 3 greatest), and now also resolves a multisampled **depth** buffer into a single-sample one of the same size (`kFeatureDepthResolve`, with sample 0; the other modes, and the sample 0 of a colour image, `kFeatureResolveModes`: the CPU renderer only). virgl blits depth with the mask `PIPE_MASK_Z` (0x10 in virglrenderer's `p_defines.h`; 0x1 is a colour channel). The scenes read the resolved depth by drawing over it with the depth test; the first-sample one matches the real GPU, the least and greatest are refused there.

### Several geometry-shader output streams, and stream output

`dcl_stream` and `emit_stream` / `cut_stream` / `emitthencut_stream` (gs_5_0; at most `kMaxGeometryStreams` = 4 streams, and D3D's rule that more than one stream means every one is a point list) write each vertex to the stream it names; the shader's outputs belong to the stream whose emit follows their writes (the `OSG5` signature names the stream of each). `PipelineDesc::stream_output_count` entries (`StreamOutputEntry{stream, output, start_component, component_count, buffer}`, at most 16, `output` 0 the position as the shader wrote it, +Y up, and n the vertex shader's varying n - 1), `stream_output_stride[4]` (bytes) and `rasterized_stream` (the stream that is drawn, -1 for none) say what is captured; entries of one buffer must be of one stream. The buffers are `kAllocStreamOutput` allocations (`kBufferStreamOutput`) bound with `CommandWriter::set_stream_output(slot, allocation, offset)`; a draw refuses when a buffer its pipeline writes is unbound. Records are appended from the bound offset on; a primitive that does not fit is dropped whole and its stream stops (strips are written as lists of triangles, with the rasterizer's swap of every second one). The escape ABI has trailing words after the hull / domain ones: the entry count and, when it is not 0, the rasterized stream, four strides and, for each entry, `stream | output << 8 | start << 16 | count << 24` and the buffer. `kFeatureStreamOutput` (the CPU renderer and virgl).

**Later additions.** *Stages*: stream output also takes the outputs of a vertex shader (no geometry or tessellation: a primitive of the draw's list topology) or of a domain shader (the tessellator's primitives); only stream 0 exists there, and `rasterized_stream` is -1 or 0. *Drawing another stream*: `rasterized_stream` may name any stream of a geometry shader (its position and varyings go through the pipeline as stream 0's would). *Appending*: `set_stream_output(slot, allocation, kStreamOutputAppendOffset)` (`graphics::kStreamOutputAppend` on the device interface) starts where the last stream output into that buffer ended; the device keeps that in `IBuffer::stream_output_filled`, VidMm keeps it across a device object being dropped. *Queries*: `QueryKind::Timestamp` (`kFeatureTimestampQueries`; the CPU renderer's clock counts 1000 ns for every timestamp unless `SoftwareDevice::set_clock` gives it a real one; the value is taken when the query ends) and `SoPrimitivesWritten` / `SoPrimitivesNeeded` (the kind word's bits 8-9 are the output stream); a primitive that does not fit counts as needed, not as written, and the stream stays closed.

virgl, in more detail than above: the declaration goes with the last vertex-processing stage (geometry, else domain, else vertex shader), which is why the vertex and domain shaders also write the unflipped position to `OUT[5]` (a geometry shader has `OUT[5 + stream]`, one for each stream, so that several streams can capture it). The host rasterizes stream 0 only, so for a pipeline that draws stream *n* the numbers of stream 0 and *n* are exchanged in a copy of the shader: the emits take their stream from an immediate that has `24064 + stream` in its fourth component (found by that marker in the text and patched) and the declaration's stream fields are exchanged too. A shader output that two streams capture cannot be had (the host gives a register one stream): the pipeline is refused. Timestamp is `PIPE_QUERY_TIMESTAMP` (2); the statistics are `PRIMITIVES_EMITTED` (6, written) and `PRIMITIVES_GENERATED` (5, needed) with the stream as the query index in the upper half. **The host's transform feedback**: it ends only when the target objects are destroyed, it resumes with the program it began with (another program's draw fails while the targets are set, so they are unset for draws without stream output and at the end of a pass), and it keeps its position in the buffer. The device therefore makes the target objects at the first draw with stream output and keeps them to the end of the submit (a pipeline with other shaders that has stream output ends the old ones: the buffers then start again at their bound offsets), and counts the primitives written with queries of its own (`PRIMITIVES_EMITTED`, per stream) to learn how far each buffer got for the next submit's appends. The host allows one query of a kind at a time, so a submit that has its own `SoPrimitivesWritten` query for a stream does not count that stream: the buffers' ends are unknown after it and an append to one is refused. Within a submit an append is only to a buffer already bound in that slot (it goes on); to another one that was written in the same submit it is refused. Rebinding a slot ends the targets of the others too (their positions start again at their bound offsets on virgl, on the CPU renderer they go on).

`gs_streams.hlsl` (two point streams), `gs_streams_draw1.hlsl` (stream 1 is drawn, stream 0 captured) and four conformance scenes (two streams, appending with statistics and timestamps, vertex and domain shader output, the second stream drawn) match the real GPU; a unit test covers the contents, overflow, strips, appends, the statistics, timestamps and the refusals. Not done: stream output statistics for the two-in-one `SO_STATISTICS` kind (ask for written and needed one after the other), `SetStreamOutputTargets` offsets other than "start" and "append" (an arbitrary offset is fine for `set_stream_output`), a shader output captured by two streams on virgl, a real clock for the software rasterizer by default.

**Timestamp frequency and disjoint.** `QueryKind::TimestampFrequency` (result 1000000000: the timestamps are nanoseconds) and `QueryKind::TimestampDisjoint` (1 if the clock was interrupted while the query was open, else 0), kind words 9 and 10 of `BeginQuery`, need `kFeatureTimestampQueries` and no draw. Both devices answer them without asking a clock: the CPU renderer's clock cannot be interrupted, and virglrenderer has no `TIMESTAMP_DISJOINT` query (it rejects the type), so the virgl device answers 1e9 and 0 itself (a GL host clock is in nanoseconds; it cannot tell a disjoint interval on desktop GL). The append conformance scene reads both. **The flag is latched**: `IGraphicsDevice::interrupt_clock()` (`Adapter::interrupt_clock()`; a software device also does it when given a new clock with `set_clock`) says the clock was interrupted - a reset, a power transition, a frequency change - and the next `TimestampDisjoint` query reads 1, once; timestamps from before it do not compare with those after. (Direct3D brackets timestamps with a disjoint query; here a query cannot hold another, so the flag is the interruption since the last time it was asked.) Nothing in the kernel calls it yet: there is no GPU reset or power management in this layer to call it from.

### Direct3D 12: root signatures, descriptor heaps, resource states

`src/Kernel/Dxgkrnl/D3d12.hpp` / `.cpp`. This is the *binding model* of Direct3D 12 on the existing devices; the command-list model, DXIL and resource heaps are not done (below).

- **Root signatures**: `Kernel::create_root_signature(pid, device, blob, &handle)` parses what `D3D12SerializeRootSignature` writes (versions 1.0 and 1.1) or the `RTS0` chunk of a DXBC container: up to 16 parameters (a descriptor table of up to 8 ranges of SRV / UAV / CBV / sampler with offsets, "append" and an unbounded last range; 32-bit root constants; root CBV / SRV / UAV descriptors), 8 static samplers, 64 DWORDs in all. A table is of views or of samplers, never both. The parser is bounds-checked and has truncation and bit-flip tests.
- **Descriptor heaps**: `create_descriptor_heap(type, count <= 4096)` (views or samplers), `write_descriptor` (what `CreateShaderResourceView` and friends say: a texture, a buffer range as unordered access or constant buffer view, a sampler; the allocation is held by the heap), `copy_descriptors` (memmove semantics). A `kAllocConstantBuffer` allocation (16-byte multiples, system memory only, never resident) is what a constant buffer view names: the CPU reads it when a draw needs it.
- **Stream ops** (`CommandStream.hpp`): `SetRootSignature`, `SetDescriptorHeaps` (one of each type), `SetRootConstants`, `SetRootDescriptor` (CBV / UAV; a buffer SRV is refused: no device has buffer shader resources), `SetRootTable`, `ResourceBarrier`. Root signature and heaps are named by handle and collected at queue time like pipelines (`Submission::signatures` / `heaps`).
- **Lowering**: at each draw and dispatch the runner turns the root arguments into the bindings the devices have, through the same code as the plain ops, so the checks are the same: `b0` (root constants, a root CBV, or the CBV at b0 of a table) becomes the push constants (one constant buffer; any other register or a non-zero space is refused), `t<n>` texture unit *n* with the sampler `s<n>` (a sampler table, a static sampler, else `s0`, else the defaults; the pairing of a texture with a sampler is by register number), `u<n>` storage slot *n*. A descriptor that was never written leaves its unit unbound; every allocation a descriptor names has to be in the stream's allocation list (D3D12's residency list), else the draw is refused.
- **Resource states**: an allocation made with `kAllocTrackState` (any other flags as usual) has a state (`AllocationDesc::initial_state`, D3D12's bits: `state::kRenderTarget` ...). `ResourceBarrier` moves it (a transition names the state it expects; the same state twice, a writing state combined with another, and a state that does not exist are refused; split barriers begin/end; unordered access barriers need an allocation in that state; aliasing barriers are no-ops), and a pass target, a vertex / index / constant buffer, a bound texture, an unordered access view, a stream output buffer, a copy and a resolve each need the matching state (the common state is promoted for reads and copies, as D3D12 does). The states a stream leaves are kept for the next stream; a stream that is refused changes nothing. Allocations without the flag are not checked.
- **Conformance** (virgl matches the CPU renderer): the compute scene through root signature with root constants, root UAVs and a table, and the 3D texture scene through a table, root constants and a static sampler, with barriers.

Reserved (tiled) buffers (`kAllocReserved`, `Kernel::update_tile_mappings`): a buffer of whole 64 KiB tiles with its own backing; each tile is mapped to a heap range or to nothing (reads zero, writes dropped) and is gathered from / scattered to the heap's memory on upload, read-back and CPU writes, so placed allocations and the GPU see the same bytes. Reserved textures too (`kAllocReserved` with `kAllocTexture`): a plain texture of 4-byte pixels, one level, whose sides are whole tiles of 128 x 128 pixels (64 KiB); the tiles are numbered row by row and `update_tile_mappings` takes those numbers; a tile in a heap is its rows one after the other (the layout is ours, not a GPU's swizzle); the conformance scene draws one mapped to two placed textures. Not done: tiled render targets, depth and mipped textures (D3D12's packed mips), command lists / bundles / `ExecuteIndirect` as such (a stream is the command list), a root signature checked against the pipeline, buffer shader resource views, more than one constant buffer, register spaces other than 0, descriptor heaps with copy-on-use semantics for `D3D12_DESCRIPTOR_RANGE_FLAG_*`, the escape ABI for the new objects (they are `Kernel` methods; the user-mode DLLs do not exist yet).

### Direct3D 12: resource heaps (placed resources)

A heap is an allocation made with `kAllocHeap` alone (a multiple of 256 bytes; system memory only, never resident). An allocation made with `AllocationDesc::heap` (the heap's handle, same device) and `heap_offset` (a multiple of 16) is **placed**: its system-memory backing is that part of the heap's memory, not a copy and not charged against the system limit. Placed allocations may overlap, so a lock through any of them sees what was written through another. Buffers and colour images of one sample can be placed (depth and multisampled images have no backing to share: `kNotSupported`; `kAllocShared` is refused). The heap's memory stays while any allocation placed in it exists, whatever happens to the heap's handle.

The device objects are still one for each allocation, so the GPU's view has to be carried across by hand, as Direct3D 12 does with **aliasing barriers**: `ResourceBarrier` of type 2 names the allocation going out of use (`allocation_index`) and, in the "before" word, the one coming into use (either `kNoAllocation`: nothing). In the runner it runs what was recorded so far, writes what the GPU holds for the outgoing allocation to the heap's memory (`VidMm::switch_alias`: `read_back`, with the same format conversions as a lock), and marks every allocation that shares bytes with it stale so that its next use uploads from the memory (reinterpreting the bytes, as aliasing does). The CPU writing through a lock does the same marking without a barrier. Not inside a pass. Without the barrier the incoming allocation keeps what its device object had. Placed render targets and textures, buffers and storage buffers all go through the same code; the conformance scene draws a triangle into a placed render target, passes the use to a placed texture at the same offset and samples it (matches on virgl). Not done: depth and multisampled placed images, a check that only one of the aliases is in use at a time, heap types (all heaps are CPU-visible), writing an aliased region from two device objects without a barrier is undefined.

### Direct3D 12: DXIL (Shader Model 6)

`ShaderKind::Dxbc` takes a DXIL container too (`compile_dxbc` hands it to `D3d9Program::compile_dxil`, `DxilTranslate.cpp`; the reader of LLVM bitcode is `DxilBitcode.cpp`). The container is a DXBC one with a `DXIL` part (a program header and LLVM 3.7 bitcode) beside the signature parts `ISG1` / `OSG1`. The bitcode is read (bitstream with abbreviations and BLOCKINFO; types, constants, global variables, the entry function's instructions, the metadata that names the entry point, its resources and its thread group) and **translated to the Shader Model 5 tokens of the same shader**, which `compile_dxbc` then takes: so the interpreter, the TGSI emitter, the D3D9 token writer and every device path built for DXBC run DXIL shaders unchanged.

- **Values**: every scalar of the function is a component of a temporary register (a linear scan reuses them after the last use; a structured walk numbers the "events" twice, once to find the last use of every value and once to emit); inputs are read where they are (`v#.c`), constants are immediates, constant buffer elements are read as `cb#[r].c`, a `ResRet` (sample, buffer load) is a whole register whose components `extractvalue` names.
- **Control flow** is made structured again from the acyclic CFG dxc writes: a branch is `if_nz` / `else` / `endif` around the arms up to their join (the lowest block both reach), a `phi` is a register the arms write on the edge to it (a swap of two phis uses scratch registers), `discard` is `discard_nz`. Loops (a back edge from the last block to the first of a run) are `loop` / `endloop` with `break`: the rotated form (the latch branches back or out), a conditional break anywhere in the body, an unconditional one; what is needed from before the loop lives to its end. Not taken: a `continue` that is not the latch's back edge, a return inside a loop, a break that does not go to one block, nesting deeper than four loops or 24 regions, `switch`.
- **Instructions** (`dx.op.*` by opcode): `loadInput` / `storeOutput` (by the signature element and row / column), the unary math of the interpreter (`FAbs`, `Saturate`, `Sin`, `Cos`, `Exp`, `Frc`, `Log`, `Sqrt`, `Rsqrt`, the four roundings), min / max (float, signed, unsigned), `FMad` / `IMad` / `UMad`, `Dot2` / `Dot3` / `Dot4` (a multiplication and multiply-adds), `createHandle` (resource and sampler registers; no arrays indexed by a variable), `cbufferLoadLegacy`, `sample`, `sampleLevel`, `textureLoad` (offsets must be zero), `discard`; and the LLVM ones: float and 32-bit integer arithmetic, shifts, bitwise, comparisons (floats as ordered / unordered alike), `select`, the casts between int and float and the free ones. Compute shaders (`cs_6_0`): `threadId`, `groupId`, `threadIdInGroup`, `flattenedThreadIdInGroup`, `bufferLoad` / `bufferStore` of raw and structured UAV buffers, `atomicBinOp`, `barrier`, and group-shared memory (a global variable in address space 3 with `getelementptr`, `load` and `store`, also as a constant expression), declared as `dcl_tgsm_raw`.
- **More stages and instructions**: geometry shaders (one stream; the vertex index of an input must be constant, `[unroll]`), hull shaders (the control-point and the fork phase, tessellation factors) and domain shaders translate to `gs_*` / `hs_*` / `ds_*` tokens; `switch` (as a chain of tests), an early `ret` outside a loop, `tan` and the hyperbolic functions, `IsNaN` / `IsInf` / `IsFinite`, signed division and remainder. Conformance scenes draw DXIL geometry, and hull plus domain shaders, on virgl.
- **Also**: `atan` / `asin` / `acos` (polynomials, within 1e-4), gather4 on 2D textures (`kTexGather`: the four texels of the footprint as filtered lookups at their centres on the CPU, TGSI `TG4`; DXIL `textureGather`), derivatives (`ddx` / `ddy`, fine and coarse: the rasterizer gives each pixel a function for the varyings and position of the corners of its 2 x 2 quad, `FragmentContext::quad_fn`, and the interpreter runs the shader up to the derivative for the other corners of the pair; TGSI `DDX_FINE` / `DDY_FINE` / `DDX` / `DDY`, which match the GPU for the fine ones), buffer shader resource views (`t#` of a raw or structured buffer are read-only storage slots after the u# ones: `kMaxUavBuffers` + n; root SRV and descriptor table SRV of a buffer lower to them; compute shaders only), `sample_d` and `sample_b` of 2D textures (and CalculateLevelOfDetail / the lod instruction: x clamped, y not; TGSI `LODQ`, with the host only taking a two-component destination) (the level is log2 of the longer gradient in texels plus the bias, then a lookup at that level; sample_b takes the gradients from the quad; TGSI `TXD` / `TXB`), texture arrays indexed by a variable (up to the device's texture units, 2: the lookup is repeated for each element behind a test of the index), `sample_c` / `sample_c_lz` (`SampleCmp`) of 2D textures: the comparison is always `reference <= texel` on the red channel with the bilinear filter (the sampler's comparison function and filter are not read), done as a gather, four compares and a hand-made blend, so the texture is any colour texture whose red is the depth and the host needs no depth texture or comparison sampler, double precision (SM5 `dadd` / `dmul` / `ddiv` / `dfma` / `drcp` / `dmin` / `dmax` / `dmov` / `dmovc`, the comparisons, `dtof` / `ftod` / `dtoi` / `dtou` / `itod` / `utod`: a double is a pair of components, the low word first, so the interpreter keeps a double in two of a register's floats' bits; the translator allocates aligned pairs for doubles; fma is not fused on the CPU; TGSI `DADD` and the rest on the GPU; DXIL: fadd / fsub / fmul / fdiv / fcmp / select / phi / fpext / fptrunc / the integer conversions on `double`, `Fma`, `MakeDouble`, `SplitDouble`, doubles from a constant buffer), 64-bit integers (a pair of words like a double; add, sub and the comparisons from carries and borrows made with unsigned compares, mul from the high word of `umul` (the instruction now gives it) and cross products, shifts by two word shifts and a select, unsigned and signed division by a 64-step shift-and-subtract loop, widening and the conversions to and from float / double), and geometry shaders with several output streams (`dcl_stream`, `emit_stream`, an OSG5 signature).
- **Not taken** (kNotSupported, or kInvalidParameter for bitcode that is not well formed): mesh and library targets, 16-bit arithmetic, buffer views in stages other than compute, gather of volumes, cubes and arrays (and gather with offsets or a comparison), wave operations, comparison sampling of anything but 2D textures with the one assumed comparison (below), alloca / memory other than group-shared, a `ret` inside a loop, a program that needs more than 32 temporaries or 1024 instructions in its translation.
- **Tests**: dxc (the library of an installed game, driven by `scripts/dxc_compile.c`; `-Vd`, since its validator rejects the bitcode this compiler writes for typed texture loads) and the Wine d3dcompiler make the same HLSL (`dxil_*.hlsl`) into `.dxil` and `.dxbc`; the unit test runs both programs on 200 random inputs each (pixel shaders: the colour and whether it was killed; the vertex shader: position and varyings; the compute shaders: the buffers after the whole dispatch) and compares; the containers are truncated and have every bit flipped under the sanitizers (`DXIL_FUZZ_STRIDE=1` flips every bit; the default is every 37th). The conformance scenes draw DXIL pixel shaders (arithmetic, branches and a discard, loops) and run the two DXIL compute shaders on the real virglrenderer and match the CPU renderer.
