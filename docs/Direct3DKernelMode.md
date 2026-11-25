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

**DXBC** (`ShaderKind::Dxbc`, `D3d9Dxbc.cpp`): the container the Direct3D 10/11 runtimes pass a driver, Shader Model 4.0 / 4.1 / 5.0 / 5.1 vertex and pixel shaders (5.1 declares constant buffers, textures and samplers as ranges with a space: `cb[ID][index][element]`, `t[ID][index]`; only space 0, single-register ranges, `b0` as the push constants and `t0`/`t1` are taken). The container is A program that declares textures and reads none takes no texture units (a pipeline gets the units its shaders read: d3d11.dll's specialisation for class instances leaves the declarations of the textures other classes would sample). Interfaces and `fcall` are not taken here: d3d11.dll specialises such a shader first (docs/Direct3D11.md, class linkage). Subroutines and switches are: see "Subroutines and switches in DXBC" below.
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
  - `kFeatureTexelAccess`: `ITextureSet::fetch` (Load), `sample_lod` (SampleLevel), `size` (GetDimensions); DXBC `ld`, `sample_l`, `resinfo`. For the images of `kFeatureTextureKinds` there are `fetch_volume` (x, y and the slice or layer; a cube has no Load), `sample_lod_volume` and `size_volume` (the depth or layer count next to the width and height; with `kFeature2MipmappedLayers` at any level: a volume's depth at that level), and for cube arrays `sample_cube_array` / `sample_lod_cube_array` (a direction and the cube; `size_volume`'s depth the number of cubes); TGSI `TXF` / `TXL` / `TXQ` with the `3D`, `2D_ARRAY`, `CUBE` and `CUBEARRAY` targets (`TXL2` for a cube array: the level in the second source). All the kinds match the real GPU.
  - `kFeatureSystemInputs`: `VertexShaderFnEx` / `FragmentShaderFnEx` carry `SV_VertexID`, `SV_InstanceID`, `SV_Position` (pixel centre, depth, 1/w), `SV_IsFrontFace` and
    `SV_PrimitiveID`; the DXBC front end reads them from the declarations (`dcl_input_sgv` / `siv`), and a program that uses them needs the feature.
  - `kFeatureInstancing`: `draw(..., instance_count, first_instance)` and `VertexAttribute::per_instance` (the element numbered by the instance, in the same buffer at the same stride);
    `VertexAttribute::step_rate` (Direct3D's `InstanceDataStepRate`, 1 when not said): instance n of a draw reads element `first_instance + n / step_rate`, with 0 always
    `first_instance` (`instance_element`); a draw is refused when the last element an instance reads (the slot's smallest rate decides) is past its buffer.
    The command stream has `DrawInstanced` / `DrawIndexedInstanced`.
  - `kFeatureLines`: `set_primitive_topology` for line lists and point lists, one pixel wide (a step per pixel along the longer axis, perspective-correct varyings); the stream's
    `Topology::LineList`, `LineStrip` (expanded to a list) and `PointList`.
  The escape ABI's `CreatePipeline` takes the blend state, the depth comparison and the per-instance bits as optional trailing words. After the last of them (the four
  floats of the constant blend colour) come tagged parts, each a tag, its word count and the words, in any order and each at most once (`DxgTypes.hpp`): the step rates are
  `kPipelineStepRateTag` ('STP1') with n words (n at most the attribute count and 16), word k the step rate of attribute k (a per-instance one's: any number, 0 never stepping;
  a per-vertex one's must be 0, else `kInvalidParameter`); the rasterizer state is `kPipelineRasterTag` ('RST1', below). Attributes past n step by 1, and a request without
  the part is what it was. d3d12.dll sends it (with the blend colour before it, whatever it is) when a per-instance element's rate is not 1.
- **Checked against a real compiler:** `ps_texel` (Load, SampleLevel, GetDimensions, SV_Position, SV_IsFrontFace, SV_PrimitiveID) and `vs_ids` (SV_VertexID, SV_InstanceID with a
  per-instance input) from Wine's HLSL compiler run through the kernel on the software device with the expected pictures.
- **Not done for Direct3D 10:** stream output and geometry shader features beyond the basics (see Geometry shaders below), `sample_b` / derivatives, more than four vertex attributes, more than four varyings (and semantics
  beyond COLOR0/1 and TEXCOORD0/1), more than one vertex buffer slot, multisampling, predication and queries other than occlusion counts (`D3D10_QUERY_OCCLUSION`; `OCCLUSION_PREDICATE` and `SO_STATISTICS` etc. are not done), depth bias and wireframe fill, texture arrays
  with mipmaps, mipmapped / filtered-across-faces cube and 3D textures, cube arrays, packed formats (float, half-float, sRGB, 16-bit unorm and R32G32B32A32_UINT colour formats are done; the other integer widths, SNORM and 10-bit formats and the rest are in "More texture formats"), UAV textures, typed UAV loads and stores, append / consume buffers, group-shared memory and barriers, indirect dispatch (Direct3D 11 compute without those). The other devices (virgl, SVGA3D, Intel Gen9, V3D) have none of the features above, so
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
| Venus (QEMU, the host's Vulkan) | TGSI text, compiled to SPIR-V (`src/Graphics/Graphics3D/Venus/TgsiSpirv.hpp`) | the kernel's own Vulkan on the host's driver through virtio-gpu's Venus (docs/Venus.md, "The kernel's 3D device on Venus"): every feature and limit virgl has, plus conservative rasterization, depth and stencil read in one draw, a layer or viewport from any stage, anisotropy, and stream output from a second pass of a submit. The device QEMU's virtio-gpu gets whenever it offers Venus (`TCOS_GPU_BACKEND`). The conformance scenes match the CPU renderer on the host's Vulkan (`VenusHostTest`) and in the guest. `DeviceInfo::d3d_sample_order` (no 4x sample remap in the TGSI). |
| VMware SVGA3D | Direct3D 9 tokens (D3D9 rewritten, DXBC written out first) | textures, mipmaps, stencil written from memory of the protocol; not run on a host. |
| Intel Gen9 | a vertex function on the CPU + its two fixed pixel kernels | pixel shaders must be exactly a colour or one texture sample (`classify_fragment`); simulated engine in the host tests, picture matches the software device. |
| Raspberry Pi V3D | its fixed vertex and pixel programs | a vertex shader that is exactly the matrix transform (`classify_vertex`, tested by running it on vectors against the built-in shader; rows or columns, Y flipped for Direct3D) and a `mov oC0, v0` pixel shader; no textures or stencil. The device is not yet in the render nodes (only the Pi session's tests make one), so a Pi has no accelerated Direct3D until it is. |
| Radeon RX Vega M | TGSI text, compiled to GCN machine code (`src/Hardware/GraphicsProcessingUnit/AMD/Gcn`) | both stages run on the card. `TCOS_AMD_VEGAM` (off by default; never run on a card: the host tests run the compiled code on a GCN emulator, `AmdVegaMTest`, `GcnCompilerTest`). Eight colour targets (one blend state for all); 16 vertex inputs and textures; any vertex format in 8 vertex buffer slots (the vertices are repacked for the card); float and integer shader instructions, `IF` / `UIF`; no loops, texel fetches, system values, instancing, lines or mipmaps yet. Every draw uploads its targets and reads them back, so it proves the card draws rather than being fast. |

`AdapterCaps` says what the adapter found by trying (`stencil`, `mipmaps`, `textures`, `programmable_vertex`, `programmable_pixel`), so a user-mode driver can pick shaders and formats the
device takes. **A new device, the AMD one included, is done when**: `create_image` takes a colour target (B8G8R8A8) and a D32Float depth image and `read_pixels` works; it takes the
built-in shader pair or any of the module forms of `ShaderModuleDesc` (function, `tgsi`, `d3d9_tokens`, or a fixed-kernel name plus `eu_varying` / `qpu_*`) for the Direct3D 9 pair
of the self-test; `create_graphics_pipeline` accepts position float3 at location 0 and colour float4 at location 1; and the host suite (`tests/DxgkrnlDeviceSuite.hpp`:
`dxgk_suite::compare(allocator, device, display, name, tolerance, allowed_bad)`) draws the same picture as the software device. Nothing in the kernel is vendor specific: once the
device is in the render node, Direct3D has it (and the self-test falls back to the CPU renderer until it draws correctly).

## Occlusion queries

`BeginQuery allocation, offset [, kind]` / `EndQuery [allocation, offset]` count the fragments that pass the stencil and depth tests (and that the shader did not discard) between them -
or another kind's counts (below); once the stream has run, the count is a little-endian 64-bit number at `offset` of the allocation (several, for the kinds with several values). The
allocation is a `kAllocQueryResult` buffer (at most 16 MiB): system memory only, never resident, read with `lock()` like any other (so after the stream has run, as `lock` waits for
queued work). **Any number of queries may be open at once**, of one kind or of several, nested or overlapping, and they begin and end inside passes or outside them: a query counts
the draws and dispatches of every pass between its `BeginQuery` and its `EndQuery`, and nothing else (clears, copies, resolves and mipmaps count nothing). `EndQuery` with the two
words ends the open query that writes there; without them, the one begun last that is still open. Two open queries may not write the same bytes, every query begun is ended in the
stream, and a stream's queries take at most `kMaxOcclusionQueries` (256) device queries (a `kQueryKindSoStatistics` query takes two, the others one); the validator refuses the
stream otherwise. A query that goes on from an earlier stream is begun with `kQueryAccumulate` (bit 16 of the kind word): its counts are added to what the place holds (a predicate
or the disjoint flag: 1 if either is not 0); timestamps and the frequency cannot be. Across a flush in the middle of a stream (a copy, a mirror's read-back) an open query goes on:
each command buffer it is open in has a segment of it - its device queries - and the kernel adds the segments up (`StreamQuery`, Adapter.cpp). The adapter needs
`kFeatureOcclusionQueries`; without it creating the buffer returns `kNotSupported`.

The devices (`ICommandBuffer::begin_query` / `end_query(slot)` / `query_value(slot, index)`): any number open, nested, inside passes or out. The CPU renderer counts everything a
submit does from its start (`QueryCounters`: samples, the eleven pipeline statistics, each stream's primitives written and needed) and a query is the difference between its end
and its begin. virgl: the host runs one query of a *target* at a time (GL's rule: an occlusion count, each pipeline statistic, the primitives a stream wrote, those it made), so a
target's host queries are *segments*: one ends and the next begins whenever a query of ours that needs the target begins or ends (and around a blit, which the host may draw),
and each of ours adds up the segments of each of its targets between its begin and its end; an occlusion predicate is its count's segments added up, not 0. The targets of
the primitives written and needed run only while a transform feedback is alive (from the first draw with stream output to its end: a pipeline without stream output writes and
needs none, and the host rasterizes wrongly with a `PRIMITIVES_GENERATED` query running and no transform feedback); their segments begin before the stream output targets are
set, as the host counts them then. **Open (virgl, not of the queries)**: with a transform feedback alive, a draw in a pass after another pass of the same submit (a clear
between two draws of a stream output pipeline) rasterizes degenerate (one pixel) - it happens without any query; the tests keep stream output draws in one pass. The host takes no
offset into a shared result buffer (two queries on one buffer overwrite each other), so each host query has a 16-byte buffer of its own created with `VIRGL_BIND_CUSTOM` (host
system memory); the host writes `{state, size, result}` straight into the buffer's guest memory when its GL query is complete - which it notices while commands with fences are
arriving, so `submit` sends empty commands until every state reads DONE - and a transfer from the host would overwrite it with its stale copy, so none is made.

## Volume, array and cube textures

(What follows was written for one level; see "Levels of every image" below for what `kFeature2MipmappedLayers` adds: levels of all of these, cube arrays, views, passes into a level.)

An allocation has a `kind` (`graphics::TextureKind`: `Texture2D`, `Texture3D`, `Texture2DArray`, `TextureCube`) and `layers` (the depth of a volume, the size of an array, 6 for a cube whose
width equals its height, at most `kMaxTextureLayers` = 256). They are sampled-only (`kAllocTexture`, optionally `kAllocShared`), one mip level, 8-bit colour, and need the adapter's
`kFeatureTextureKinds` (`kNotSupported` otherwise); the lock shows all layers one after the other (`pitch` per row, `pitch * height` per layer). A DXBC pixel shader declares the kind with
`dcl_resource_texture3d / texture2darray / texturecube`; `D3d9Program::texture_kind(unit)` records it, the TGSI text says `3D` / `2D_ARRAY` / `CUBE`, `ld` / `sample_l` / `resinfo` stay 2D-only,
and `draw` refuses a texture whose kind is not the one the pixel shader declared. Sampling (`ITextureSet::sample_volume`, third coordinate `w`): a volume's `w` is 0..1 over the slices (blended
by a linear filter, clamped), an array's is the layer (rounded, clamped), a cube's coordinate is a direction (+X, -X, +Y, -Y, +Z, -Z faces; no filtering across faces). virgl: resource targets 3D / 2D_ARRAY / CUBE,
layers staged one at a time through a one-layer backing (transfer box `z` = the layer), the sampler's wrap R is clamped, the view's last layer is `layers - 1` for arrays and cubes and 0 for a volume. The 33 scenes
pass on virglrenderer; pixel centres on exact texel boundaries are avoided in the array scene (devices round ties either way).

`d3d12.dll` makes a `TEXTURE3D` resource a volume and a 2D one of `DepthOrArraySize` > 1 an array; a cube only when the runtime above says the next resource is one
(`TcosD3D12ResourceOptions(device, TCOS_RESOURCE_CUBE)`, src/Windows/tcosdx.h: six square layers), since Direct3D 12 tells a cube from an array of six only by its views and
the kernel's allocation has to know when it is made. `d3d11.dll` (`D3D11_RESOURCE_MISC_TEXTURECUBE`) and `d3d9.dll` (`CreateCubeTexture`) say it; a plain Direct3D 12
program's cube is an array - which a TEXTURECUBE or TEXTURECUBEARRAY view now shows as one (below). The same header's `TcosD3D12StageConstantBases` places the geometry, hull and domain shaders' constant buffers in the block of constants.

## Levels of every image (kFeature2MipmappedLayers)

On an adapter with `kFeature2MipmappedLayers` (features2 bit 10; the CPU renderer, and virgl when the host's GLSL is 4.30 or later - its sampler views of another target or of a
range of levels are the host's texture views) every image of one sample may have levels: arrays, cubes and volumes (a volume's level `l` has `max(1, layers >> l)` slices:
`graphics::level_layer_count`, `Allocation::level_layers`), render targets and depth buffers, storage images, the wide formats, staging memory. And:

- **Cube arrays**: `TextureKind::TextureCubeArray` (5), `layers` a multiple of six (cube n is layers 6n .. 6n + 5), square. DXBC `dcl_resource_texturecubearray` (dimension
  10, SM 4.1): `sample` takes (x, y, z, cube), `sample_l` the same and the level (TGSI `TXL2`), `resinfo` the number of cubes; it has no `ld` and no gather here. A storage image
  of a cube or a cube array is an array of its faces to the shaders (virgl makes such an image a `2D_ARRAY` resource with sampler views of the cube target).
- **The memory**: the backing has the levels one after the other, each level its layers (`Allocation::level_range` / `subresource_range` / `subresource_bytes`); a lock of level `l`
  is all its layers. VidMm uploads and reads back each layer of each level (`IImage::write_subresource` / `read_subresource` and the `_float` forms; on a device without the
  feature the defaults take what the older forms take).
- **Views** (`BindTexture`'s four optional words after its tenth, the `texture_view_word()` - the 11th to 14th -: first level, count of levels, first layer, count of layers - a count of 0 to the last; a Direct3D 12 texture descriptor carries
  them in its offset words: level, count, layer, count, 16 bits each): the unit sees those levels and layers as the whole image (`ITextureSet` lookups, `GetDimensions` and `Load`
  are of the view), of the kind the shader declares - `graphics::texture_view_fits`: a slice of an array as a 2D texture, six layers of an array as a cube and a multiple of six
  as a cube array, a cube or a cube array as an array, a 2D image as an array of one; a volume's view has all its slices. The stream runner checks the view against each
  pipeline's declared kinds at the draw or dispatch (`fit_textures`) and binds the unit again with the declared kind when it differs (`ICommandBuffer::bind_texture_view`). The
  sampler's minimum and maximum LOD count from the view's first level, as Direct3D's do.
- **Passes into a level**: `BeginPass` with `kPassLevels` (flag 16) ends with the level of each colour target and of the depth buffer; every target's level is of one size, the
  pass's, and the layers of `kPassLayers` are that level's (a volume's level has fewer slices). A unit may sample levels or layers of a target the pass does not draw into
  (render into level n while sampling level n - 1 of the same texture); one whose view shows what the pass draws into is refused, as before. Software device:
  `RenderPassBegin::level`; virgl: surfaces of (level, first layer, count).
- **Copies**: `CopyRegion`'s five optional words (destination level and layer, source level and layer, number of layers - a volume's slices) copy between any subresources of
  images whose pixels (or 4 x 4 blocks: block-compressed images, whole blocks as `CopyBufferImage` takes them) are as many bytes; `CopyBufferImage` takes any layer of any level.
  On the CPU, from the backings (the GPU's newer contents read back first).
- **GenerateMipmaps** of arrays and cubes (each layer its own chain), volumes (2 x 2 x 2 boxes of the slices above) and the wide formats (by their values); on the CPU from the
  backing, after the GPU's contents are read back (a level 0 drawn into); block-compressed and integer images are refused.
- **Storage images** of any level of a layered image (`BindUavImage`'s layers are the level's).
- **Mirrors of subresources**: `SetMirror`'s optional words name the subresource (level | layer << 16) each layer of the mirror shows - what d3d12.dll makes a narrow texture's
  unordered access view of a level, or of layers of an array, of (`narrow_mirror`: an R8 / R8G8 / R16 / R16G16 view of the widened image, whose channels go back). A mirror may
  be a volume of one level too, each of its slices a source's subresource (subresource words required): what an unordered access view of slices of a level of a narrow volume
  writes through (`RWTexture3D` of an R8 / R16G16 ... volume).
- **The DLLs**: d3d12.dll makes resources of any levels and kinds (cube arrays: `TcosD3D12ResourceOptions(TCOS_RESOURCE_CUBE)` of a multiple of six; a plain Direct3D 12
  program's arrays of six or twelve are cubes and cube arrays through their views), honours a shader resource view's `MostDetailedMip` / `MipLevels` and slices
  (`srv_view`), render target and depth stencil views' `MipSlice` (the level and its size into the pass; a depth-only pass's companion target is of the level's size),
  unordered access views of a level of arrays, volumes and cubes, `CopyTextureRegion` between any subresources and `CopyResource` of every level and layer; packed YUV
  arrays of levels are one array of the kernel's (the video processor reads their slices by way of a copy), Y416 has levels. d3d11.dll (`dev_all_levels`) keeps every
  level of every texture, draws into render targets and depth buffers of levels directly (no rtshadow.inc targets), makes cube arrays, turns its views into Direct3D 12's
  (`srv_desc12`), generates the levels of a view's levels and layers (a volume's by 2 x 2 x 2) and copies any subresources through Direct3D 12.
- **Tests**: `DxgkrnlTest` `test_mipmapped_layers` (allocations and their memory, sampling at levels, views of every kind, cube arrays, derivatives on arrays, passes into levels,
  copies, GenerateMipmaps, storage images of cubes and levels, mirrors of subresources, what a device without the feature refuses); the conformance scenes of levels, views,
  passes into levels, cube arrays and unordered access to a cube's faces match the CPU renderer on the real virglrenderer; `d3d12test` `test_mip_layers` and `d3d11test`
  `test_mip_layers` through the DLLs.

## Depth buffers read as textures, comparisons, texture and sampler pairs

A depth buffer is a texture too when it is made `kAllocDepthStencil | kAllocTexture` (one sample; one level, or any on a device with `kFeature2MipmappedLayers`; a 2D image, an array, a cube or a cube array - not a volume): the adapter's
`graphics::kFeature2DepthTextures` (bit 16 of `DeviceInfo::features2`; the CPU renderer and virgl), `kNotSupported` without it. Its device image is `kImageDepthAttachment |
kImageSampled`. A unit bound to it reads the depth as (depth, 0, 0, 1) - every depth is a float here, so a D16 or D24 buffer's is not rounded to its bits -, or, with
`SamplerDesc::stencil`, the stencil plane of a D24S8 buffer as the integers (0, stencil, 0, 1) (Direct3D's X24_TYPELESS_G8_UINT view, read with `Load`). A unit's view of it
(`BindTexture`'s view words, a descriptor's view) is of any of its levels and layers, of another kind than the image's as for colour textures (a slice of an array as a 2D texture,
six layers as a cube, an array of whole cubes as a cube array): the CPU renderer reads the view's level and layer planes, virgl's sampler views show those levels and layers of the
host's depth texture. Drawing into a level and layer of it while a unit reads that one is refused (as for any target); other levels and layers of it may be read meanwhile.

**Copies** (`kFeature2DepthCopies`: the CPU renderer and virgl). The system keeps no memory of a depth buffer or of a multisampled image, so the device copies them:
`IImage::copy_subresource` copies a whole subresource into another image of the same format, sample count and size there (a depth buffer's planes asked for), and
`IImage::read_depth_subresource` / `write_depth_subresource` give the CPU a depth buffer's depth plane as floats and its stencil plane as bytes. `CopyRegion` of two depth buffers or two
multisampled images is such a copy of whole subresources (the rectangle all of the destination's level at 0, 0; anything else is refused), with an optional fourteenth word, the planes
(bit 0 the depth, bit 1 the stencil; 0 every plane the buffers have). `CopyBufferImage` of a depth buffer needs one of the depth layouts in its twelfth word - `NarrowLayout::DepthFloat32`
(5: the depth as 32-bit floats), `DepthUnorm16` (6: 16-bit codes), `DepthUnorm24` (7: 24-bit codes in the low bits of a word whose high byte is 0) or `Stencil8` (8: a D24S8 buffer's stencil as
bytes) - and copies a rectangle of a level's layer of that plane into the buffer or out of it (the rest of the plane and the other plane keep what they had); into the buffer a depth is
rounded to the nearest code, into the image a code becomes the float it stands for. On virgl the planes go through transfers of the host's Z32_FLOAT / Z32_FLOAT_S8X24_UINT resource
(a float, then a word with the stencil in its low byte), the subresource copies are blits of the planes asked for (masks Z and S; the rows turned over between images kept in the two
orders - a multisampled image never is). Direct3D 12's rules come first in d3d12.dll: a depth buffer or a multisampled image is copied whole, a subresource of a D24S8 / D32S8 buffer is
of one plane (the stencil's subresources after the depth's, `D3D12_FEATURE_FORMAT_INFO`'s `PlaneCount` 2 also for the typeless forms; the footprints of the depth plane are
`R32_TYPELESS` words and of the stencil plane `R8_TYPELESS` bytes), and a copy against the rules is not made (the debug layer's error on Windows).

**The least level of detail of a view** (Direct3D 12's `ResourceMinLODClamp`, Direct3D 11's `SetResourceMinLOD`): a texture descriptor's `min_lod` word (`EscapeOp::WriteDescriptors`; a
float, 0 none, at most 1000) is the most detailed level its lookups take, counted in the image's levels. At each draw the binding raises the sampler's `min_lod` to the clamp less the
view's first level (and its `max_lod` with it: Direct3D clamps the sampler's level first, then takes the greater of it and the clamp), and gives a sampler without levels the nearest
level; a clamp past the view's last level reads that last level. Loads are not clamped. Every device has it (it is the sampler's state).

**Gathers of cube arrays.** gather4 and gather4_c (Gather, GatherRed / Green / Blue / Alpha, GatherCmp*) of a `TextureCubeArray` take the direction and the cube from the coordinate's
four components (no offsets: Direct3D has none for cubes): the CPU renderer looks the taps up on the face of the cube the direction names (`sample_lod_cube_array`, as a cube's with
the neighbouring face for a tap off the face), TGSI's `TG4` has the coordinate's four components on `CUBEARRAY` (DXIL's cube arrays are SM5's resource dimension 10 now, their
`textureGather` the same). The component a gather takes is the one the four-component sampler operand selects (fxc's and Wine's `s0.z` of GatherBlue), through the resource operand's
swizzle (the DXIL translator writes it on the resource, with a sampler of no components).

**Comparisons.** `SamplerDesc::compare` (a `graphics::CompareOp`, never .. always; Never when the sampler does not compare) is what `sample_c` / `sample_c_lz` (SampleCmp,
SampleCmpLevelZero) and `gather4_c` / `gather4_po_c` (GatherCmp) compare with: a texel passes when reference <op> texel. `ITextureSet::sample_compare` filters the passes as the
sampler filters (point: the texel the point lies in; linear: the four around it, bilinearly), at the level the derivatives give or level 0, on 2D textures, arrays (the layer) and
cubes (the face the direction points at, no filtering across faces); a static texel offset (aoffimmi) moves the point by whole texels of the base level. GatherCmp compares each of
the four texels by `ITextureSet::compare_op`. Any texture whose red is the depth can be compared (R32_FLOAT and R16_UNORM textures too). The TGSI text compares by hand - a TG4 of
the red channels, the passes, the weights from the place in the footprint (on a cube from the face's place, worked out from the direction) -, reading each unit's comparison from the
constants: `CONST[kSamplerCompareRegister + unit]` (register 256, after the most a program may have) holds (less passes, equal passes, greater passes, linear filter) as 1 or 0
(`graphics::sampler_compare_constant`), which virgl puts after the push constants for a pipeline whose modules say `ShaderModuleDesc::sampler_constants`. The level is the view's
first one there (SampleCmpLevelZero's; a SampleCmp of a mipmapped texture compares that level on virgl).

**In the stream and the descriptors.** `BindTexture` takes an optional tenth word, `texture_view_word()` of DxgTypes.hpp: the comparison in bits 0-3 and the stencil plane in bit 8
(refused for an image that is not a D24S8 depth buffer). Direct3D 12's descriptors (`EscapeOp::WriteDescriptors`): a sampler's offset low word is its comparison (`CompareOp`, 0
never .. 7 always; the high word 0), an image view's lod-bias word 1 says the stencil plane (`DescriptorDesc::stencil`; its offset words are its levels and layers); a static sampler's `ComparisonFunc` is taken for a
comparison filter (reduction bits 7-8 = 1).

**Texture and sampler pairs.** A DXBC shader may read one texture with several samplers. `D3d9Dxbc.cpp` gives each pair the shader uses a unit: t<n>'s own unit n with the
sampler of its first lookup (most textures are read with one sampler, often s0 for all of them), and every other pair of t<n> the first unit no t# is declared at;
`D3d9Program::texture_register(u)` / `sampler_register(u)` say what unit u is (the others are their own pair), `sampled_units()` which units a lookup with a sampler reads. SM 5.1's
`dcl_sampler s[ID][lower:upper]` ranges are taken. The binding of Direct3D 12 (`apply_root_arguments`) puts t<reg> on every unit whose texture register is reg, each with the sampler
of its pair (a table's or a static one, else s0, else the defaults). A pipeline's units are its stages': a vertex and a pixel shader that read one unit as different pairs (each
picks the units of its own pairs) are refused (`kNotSupported`); a unit only loaded from (ld, resinfo) takes whichever sampler. The pipeline has the paired units whatever its
description counts. On virgl each unit is its own sampler view and sampler state, as TGSI's SVIEW / SAMP have them.

**virgl.** A depth buffer that is a texture is a resource of the depth format with a sampler view (`Z32_FLOAT` or `Z32_FLOAT_S8X24_UINT`, swizzle (X, 0, 0, 1)) and, with stencil, a
second view of `X32_S8X24_UINT` (138; the host reads it with `GL_STENCIL_INDEX` texturing, swizzle (0, X, 0, 1)). Textures are kept row 0 first and images that are only drawn into
and shown with Y_0_TOP, the other way round; a depth buffer drawn in a pass orients its rows as the pass's colour target does. So a depth buffer that is a texture is kept as
textures are (no Y_0_TOP), and a pass that draws into it with a colour target kept the other way (a swap chain's back buffer) draws into an upright stand-in for the target
(`VirglImage::stand_in`, one per image, made on first use): the stand-in is filled from the target by a blit before the pass unless the pass clears it, and blitted back into the
target when the pass ends (the host's blits turn the rows over between the two kinds of image, as they do for every copy between them).

## Block-compressed textures

`Format::Bc1`, `Bc2`, `Bc3` and their sRGB forms (BC1 = DXT1 with its one-bit alpha, BC2 = DXT3, BC3 = DXT5) are allocation formats of sampled textures only (`kAllocTexture`,
optionally `kAllocShared`, or staging memory; one sample): plain textures with mip levels, or volumes, arrays and cubes (of levels too, with `kFeature2MipmappedLayers`). The backing is rows of 4 x 4 blocks of 8 or 16
bytes (`image_row_bytes` / `image_rows` / `image_level_bytes` in DxgTypes.hpp; a lock's pitch is a row of blocks), and `CopyBufferImage` copies whole blocks: the rectangle starts on a
block and ends on one or at the level's edge, the buffer's rows are rows of blocks. `GenerateMipmaps` refuses them; `CopyRegion` copies whole blocks between their subresources with `kFeature2MipmappedLayers` (refused without). A device with `kFeatureCompressedFormats` samples
them as they are: VidMm gives it the blocks with `IImage::write_compressed` (virgl: the host's `DXT1_RGBA` 106, `DXT3_RGBA` 107, `DXT5_RGBA` 108 and the `SRGBA` forms 110 - 112 of
virglrenderer's `virgl_hw.h`, bound as sampler views only, each level and layer transferred with a stride of a row of blocks); for the others (the CPU renderer) the device image is
B8G8R8A8 and VidMm decodes the blocks when it uploads them (`Graphics/BlockCompression.hpp`, the decoding the Direct3D 10 specification gives; `BlockCompressionTest` checks it
with values worked out by hand, shortened views and every bit turned). BC4 - BC7 are formats of their own (Bc4Unorm .. Bc7Srgb, below: "More texture formats").

## Compute shaders and unordered access views

`cs_5_0` DXBC compute shaders run through `ShaderKind::Dxbc` with `ShaderStage::Compute` (the escape ABI's `CreateShader` stage 2). A compute pipeline has no state: `PipelineDesc::compute_shader`
(or `EscapeOp::CreateComputePipeline`) with the shader's handle. Buffers are `kAllocUnorderedAccess` allocations (vertex / index flags may be added; the adapter needs `kFeatureCompute`).
A stream binds them outside a pass with `BindUav slot, allocation, offset, size` (slots below `kMaxStorageBuffers`: u0 .. u63, then t0 .. t127 of buffers, then the 64 counters - see below; offset and size multiples of 4), binds the pipeline with `BindPipeline`
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

### Storage images (unordered access views of textures)

A `kAllocStorageImage` texture (with `kAllocTexture`, optionally `kAllocRenderTarget`; a colour format that is not sRGB or block-compressed, one sample; a cube or a cube array, whose faces are an array to the shaders, only with `kFeature2MipmappedLayers`; the adapter's
`kFeature2StorageImages`, a second word of device features `DeviceInfo::features2` / `AdapterCaps::features2`) is read and written texel by texel by compute shaders, and by pixel
shaders in passes (`kFeature2StorageImagesInDraws`). A stream binds a level and a run of layers of one to storage slot u<n> with `BindUavImage slot, allocation, level,
first_layer, layer_count [, view format]` (op 37; slots below `kMaxStorageImages` = 8, a register being a buffer or an image by the shader's declaration); Direct3D 12 binds
them through descriptors of `DescriptorKind::UavTexture` (5; WriteDescriptors' offset words the level and first layer, size words the count and the view's format), which
the stream runner lowers at each dispatch and draw where the pipeline's shader declares the slot. The view's format may be another wide format of the same pixel size than the
allocation's (a typeless resource's views: R32_UINT of R32_FLOAT), or A8B8G8R8Uint of A8B8G8R8 and back. A dispatch or draw is refused when a slot the shader declares is
unbound, an integer image is given to a float slot or the other way round, a 2D slot gets several layers or a volume, an array slot a volume. What the shaders write is in the
image after the dispatch or draw (VidMm reads layered and mipmapped storage images back by layer and level); a mirror that writes back gives its channels to its source then.
- DXBC: `dcl_uav_typed_texture1d / 1darray / 2d / 2darray / 3d` (return types float, unorm, snorm: floats; uint, sint: integers), `ld_uav_typed`, `store_uav_typed`,
  `resinfo` of a u#, and the atomics `atomic_*` / `imm_atomic_*` on typed views (add, and, or, xor, min / max signed and unsigned, exchange, compare-exchange; the last also on
  buffers now: `atomic_cmp_store`, `imm_atomic_cmp_exch`) on R32Uint / R32Sint images - `Format::R32Uint`, `R32Sint`, `R32Float` (25 .. 27) are new one-channel 32-bit wide
  formats (`kFeature2SingleChannelFormats`), render targets, textures and storage images. A one-dimensional image is a two-dimensional one a texel high (its array an array of
  those). Typed views of buffers and storage images in the other stages: below. Not taken: SM 5.1 UAV ranges. (DXIL texture UAVs and typed buffer views: "Shader Model 6
  wave operations" below.)
- Software device: `IStorageImages` (load / store / atomic / size) in `ComputeContext::images` and `FragmentContext::images`; values travel as textures' do (floats, or the
  integers' bits).
- virgl: `DCL IMAGE[n], <target>, IMGFMT<n>, WR` and `LOAD` / `STORE` / `ATOM*` / `ATOMCAS` / `RESQ` on `IMAGE[n]`; `IMGFMT<n>` is a placeholder the device replaces with
  the Gallium name of the format of what is bound to slot n (`VirglShaderModule::variant`, up to four host shaders per module), bound with `SET_SHADER_IMAGES` (35: format,
  access, `first | last << 16` layers, level, resource) before each dispatch and draw, and a memory barrier after. An 8-bit storage image is an RGBA8 resource on the host
  (the image units have no BGRA format; its 0xAARRGGBB pixels are swapped on the way in and out).

### Buffers: typed views, counters, the slots, every stage

- **Slots.** `graphics::kMaxUavBuffers` = 64 (u0 .. u63, Direct3D 11.1's), `kMaxSrvBuffers` = 128 (t0 .. t127 of raw and structured buffers, storage slot `kMaxUavBuffers + n`),
  `kMaxStorageImages` = 64; `kMaxStorageBuffers` = 256 with the counters. Masks of these spaces are `graphics::SlotMask<N>` (the shaders' and pipelines' `storage_mask()` /
  `image_mask()`). What a runtime offers is its feature level's: d3d11.dll and d3d12.dll are at 11_0 (eight UAVs, `ResourceBindingTier` 1), d3d11.dll's shader resources go to t127
  (a second, wide root signature when a shader declares one above t7).
- **Typed views of buffers** (`Buffer<>`, `RWBuffer<>`; `kFeature2TexelBuffers`, `graphics::TexelFormat` in `Graphics3D/TexelBuffer.hpp`: DXGI's numbers, with the decoding and
  encoding of each element). They are storage images of a buffer: u# ones in image slots 0 .. 63, t# ones (read only) in slots `kTexelBufferSlotBase + n` (64 .. 191,
  `kMaxImageSlots`); `bind_storage_texel_buffer` on the devices, op **`BindTexelBuffer` (38)** `slot, allocation, first_element, element_count, format` in the stream, and in
  Direct3D 12 descriptors a buffer `Srv` / `Uav` with `DescriptorDesc::texel_format` (WriteDescriptors' bias word; lowered to `BindTexelBuffer` where a shader declares the
  slot a typed buffer, to `BindUav` where it declares a raw or structured one). The elements lie in the buffer from a whole element; an unordered access view takes the formats
  that can be stored (`texel_format_storable`: not the 96-bit, B8G8R8, 16-bit packed and A8 ones), atomics the 32-bit integers. DXBC: `dcl_resource_buffer`, `ld` on it,
  `dcl_uav_typed_buffer`, `ld_uav_typed` / `store_uav_typed` and the atomics on it, `bufinfo` (`kBufInfo`: the bytes of a raw view, the structures of a structured one, the
  elements of a typed one). Out of the view a load is zero and a store is dropped.
- **Counters** (append, consume and counter views: `imm_atomic_alloc` / `imm_atomic_consume`, `kBufCounter`). The hidden count of u<n> is a word of a buffer bound to storage slot
  `kCounterSlotBase + n` (192 + n) with `BindUav`; in Direct3D 12 a buffer `Uav` descriptor's `counter` / `counter_offset` (WriteDescriptors' min and max LOD words), lowered
  where a shader counts with the view. alloc gives the count and adds one, consume takes one and gives the count after.
- **Every stage** (`kFeature2StorageImagesAllStages`): storage images and typed views in vertex, geometry, hull and domain shaders too (the contexts carry `images`); a vertex
  shader that writes them is a `vertex_stores` pipeline. A slot two stages of a pipeline declare must be the same kind to both.
- **Draws with no target** (UAV-only rendering): the kernel's passes still need a colour target, so d3d12.dll gives such a pass one of its own of the viewport's size and of the
  pipeline's samples (`ForcedSampleCount` 4, 8 or 16 where the device has the count: the pixel shader still runs once a pixel), kept until the device goes; d3d11.dll draws when
  its pixel shader declares UAVs with no render target or depth buffer bound.
- **virgl**: a shader's TGSI numbers its storage densely - `BUFFER[k]`, `IMAGE[k]` and buffer views `SVIEW[16 + k]` / `SAMP[16 + k]` - and says which slot each is
  (`graphics::TgsiBindings` in the ShaderModuleDesc; at most 32 buffers, 32 images and 16 buffer views a stage); each stage of a draw (and a dispatch) gets
  `SET_SHADER_BUFFERS` / `SET_SHADER_IMAGES` / `SET_SAMPLER_VIEWS` of its own from those tables. A typed u# view is an `IMAGE` of `BUFFER` target over the whole buffer (the host's
  image of a buffer ignores the offset), a t# view a host sampler view of the buffer's elements up to the view's last, read with `TXF`; the view's first element and count are
  marks of the text (`IMGOFS<k>` / `IMGLEN<k>`, `TBOFS<k>` / `TBLEN<k>`) the device writes into the host shader made for what is bound (variants, eight a module, in every
  stage now), with an index past the count made one outside the image. Counters are `ATOMUADD` of 1 / -1 on their buffer. The host's buffer textures take OpenGL's buffer
  texture formats only, so a typed view of a snorm, R10G10B10A2, R11G11B10, A8 or B-first packed format is refused on virgl (the submit fails; the CPU renderer has them all). A domain shader with images goes only with a hull
  shader of its winding and partitioning, and a stream output pipeline's last vertex stage has none (the host's copies of those shaders are not made per binding).
- Tests: `tests/data/dxbc/cs_typed_buffer`, `cs_append` and `cs_append_u3` (assembled by `make_asm_fixtures.py`: Wine's compiler has no append buffers and no `GetDimensions` of
  a buffer), `vs_uav`, `ps_uav_only`; `DxgkrnlTest` test_texel_buffers_kernel / test_uav_counters_kernel / test_vertex_uav_kernel (also the element codecs against values worked
  out by hand), d3d12test test_typed_buffers / test_counters / test_uav_only_draws, d3d11test test_typed_buffers / test_append_consume / test_uav_only_draw.

### Layered render targets (SV_RenderTargetArrayIndex)

Arrays, cubes and volumes may be render targets, and arrays and cubes depth buffers, on an adapter with `kFeature2LayeredTargets` (of several levels, and drawn into at a level - `kPassLevels` -, with `kFeature2MipmappedLayers`). `BeginPass` with
`kPassLayers` (flag 8) ends with a first layer and a count for each colour target and then the depth buffer (`RenderPassBegin::first_layer` / `layer_count`): a clear
clears those layers, and a primitive goes to the layer its geometry shader's `SV_RenderTargetArrayIndex` names (the leading vertex's: `ShaderIo::layer`), the target's first
when that is past its count. Only a geometry shader writes it (`VPAndRTArrayIndexFromAnyShaderFeedingRasterizerSupportedWithoutGSEmulation` is FALSE); `SV_ViewportArrayIndex`
is not taken (one viewport). DXBC: the output's `render_target_array_index` (OSGN names up to 31 characters now) and `vPrim` in geometry shaders. virgl: `DCL OUT[n], LAYER`
(after the stream positions), `vPrim` as the input `DCL IN[n], PRIM_ID` (the host makes a geometry shader's PRIMID system value the output `gl_PrimitiveID`, its input `gl_PrimitiveIDIn`), and surfaces of a layer range (`first | last << 16`); layered resources never take Y_0_TOP (the host refuses it on anything but 2D). d3d12.dll:
render target and depth views of `TEXTURE2DARRAY` / `TEXTURE1DARRAY` / `TEXTURE3D` slices (`SoftDesc::slice` / `stride`), planar views of several slices through an array
mirror that writes back. d3d11.dll: arrays, cubes and volumes are drawn into directly, of any level on an adapter with `kFeature2MipmappedLayers` (the render target shadows of rtshadow.inc stay for textures with levels on the others).

## Multisampling

An allocation with `samples` 2, 4 or 8 (the adapter's `kFeatureMultisample`; `DeviceInfo::sample_counts` says which) is a render target of an 8-bit colour format or a depth buffer, nothing
else: no system-memory backing (so no lock, copy, texture or present), one level. Every image of a pass has the same count. `ResolveRegion destination, source` (outside a pass) averages the
samples of a multisampled colour target into an ordinary image of the same format and size - a render target, a texture, a back buffer. Coverage is per sample when the pipeline says
`multisample` (D3D10 `MultisampleEnable`): a triangle covers the samples whose standard Direct3D positions (2x: (4,4) (-4,-4); 4x: (-2,-6) (6,-2) (-6,2) (2,6); 8x as in `SoftwareDevice.cpp`, in
sixteenths of a pixel) it contains, depth and stencil are tested for each, the shader runs once per pixel at its centre, and the colour goes to the covered samples that pass; without
`multisample` the pixel is all or nothing by its centre. Lines and points cover every sample. Occlusion queries count samples. The contents of a multisampled image do not survive being
evicted from video memory (there is nothing to read them back into). virgl: resources with `nr_samples`, the rasterizer's multisample bit, and `VIRGL_CCMD_BLIT` for the resolve (the host
keeps a multisampled image the GL way up whatever flag it was made with, so the blit turns the picture over only for a destination kept upright); 2 and 4 samples, as far as the host's
`max_samples` goes. The conformance scene uses only vertical and horizontal edges, which every device with the standard patterns covers the same whatever the order of the samples. The least and greatest
samples of a colour image (`ResolveRegion`'s mode 2 / 3) are `kFeatureResolveModes` on the CPU renderer and, without the depth modes, `kFeature2ColorResolveModes`
(1 << 14 of `features2`) on virgl, where the blit only averages: `extreme_resolve` draws a triangle over the destination with a pixel shader that fetches each sample of
its pixel (TXF on the source's 2D_MSAA view, one made for it when the image is not sampled) and folds them with MIN or MAX; its shaders and states are made for the
resolve and destroyed after it in the stream, and the next pass and draw set the state again.

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
presentable, and the 16-bit and integer ones (like the floats) have no multisampling, and one level but on an adapter with `kFeature2MipmappedLayers`; an sRGB image may have mip levels (the chain is made from the stored codes, not from linear values) and
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

## The 3D Test window

The desktop's 3D Test (Start menu, `src/Desktop/Graphics3DTest.cpp`) draws its spinning cube through any of three APIs, chosen with the radio buttons at the bottom, so
the kernel side of each can be tried and timed on the machine at hand:

- **Direct3D**: the kernel-mode Direct3D driver with its built-in shaders and the typed calls (`desktop::CubeRenderer`).
- **Vulkan**: the same driver the way vulkan-1.dll drives it: SPIR-V (`src/Desktop/CubeShaders`, compiled by `scripts/gen-cube-spirv.py`) translated to DXBC by the DLL's
  own translator (built into the kernel as `src/Desktop/CubeSpirvTranslator.c`), shaders and pipelines made through `Kernel::escape`.
- **OpenGL**: the kernel's GL (`desktop::GlCubeRenderer`): the GL command stream opengl32.dll writes, run by `opengl::GlService` in a context and window surface of the
  window's own process, the frame shown with a swap and read with `read_front`. It always draws on the CPU.

`tests/run_host_tests.sh CubeRendererTest` checks that the three give the same picture on the software device. Every few seconds the window writes a `3D Test:` line
(frames per second, average frame time, the device and the API) to the debug console.

## Video: decoding and processing

Direct3D 11's and 12's video interfaces run on the kernel's codecs, on the CPU, whatever the adapter.

- **Decoding** (DXVA). A program parses its stream and hands each picture's DXVA buffers (picture parameters, inverse quantization matrices, slice or tile controls,
  compressed data). `src/Hardware/VideoCodec/Dxva` decodes them (`IDxvaDecoder`, DxvaDecoder.hpp): H.264 VLD (short slices; progressive and interlaced - MBAFF frames
  and field pictures, whose references are the fields UsedForReferenceFlags marks - monochrome as NV12 with grey chroma, POC types 0-2), MPEG-2 and
  MPEG-1, VP9 profile 0 and 10-bit profile 2, HEVC Main (NV12) and Main 10 (P010: 8 to 10 bits for luma and chroma each, so Main and the Still Picture
  profiles too) and the HEVC range extension profiles (DXVA_PicParams_HEVC_RangeExt; DxvaProfile 17-26: Main12 into P016, Main10_422 into Y210 / YUY2,
  Main12_422 into Y216, Main_444 into AYUV, Main10_Ext into P010, Main10_444 into Y410, Main12_444 and Main16 into Y416, Monochrome into NV12 and
  Monochrome10 into P010 with neutral chroma - each profile's chroma format and bit depths and the smaller ones, into a surface of the picture's chroma format),
  AV1 profile 0 (8 and 10 bits, monochrome, film grain), VC-1 (VC1_VLD and VC1_D2010: Simple, Main and
  Advanced profile, progressive and interlaced pictures: frame and field pictures, range reduction and range mapping) and MPEG-4 Part 2 (MPEG4PT2_VLD_SIMPLE, _ADVSIMPLE_NOGMC, _ADVSIMPLE_GMC: B planes, quarter samples, interlace, GMC, the
  MPEG quantiser, video packets, data partitioning and RVLC, the short video header). H.264, MPEG-2 and VP9 rebuild what the software decoders read from the parameters;
  HEVC and AV1 fill the decoders' parameter sets and frame headers from them and take the reference sets DXVA gives. The CPU H.265 decoder
  has every sample path at any depth up to 16 bits (templated on the sample type: 8-bit pictures take the 8-bit code as before, deeper ones 16-bit planes with
  QpBdOffset, the interpolation shifts, weighted prediction offsets, bdShift, PCM at its own depth, tC / beta scaled), the chroma formats 4:0:0, 4:2:2 (the
  Table 8-3 mode mapping, two square chroma blocks a transform unit) and 4:4:4 (a chroma mode a part, chroma filtering of the intra references), and the range
  extension tools: transform skip rotation and contexts, implicit and explicit RDPCM, transform skip up to 32x32, extended precision, intra smoothing disabled,
  high precision weighted prediction offsets, persistent Rice adaptation (synchronised with the contexts for wavefront rows and dependent slices), CABAC bypass
  alignment, cross-component prediction, the chroma QP offset lists and log2_sao_offset_scale. VC-1 (DxvaVc1.cpp) makes the sequence and entry
  point layers from DXVA_PictureParameters and hands the frame (with its start codes) to the CPU decoder's external mode (`vc1::Core::decode_external`: the caller's
  pictures, the rounding control bRcontrol, the intensity compensation tables kept with the surfaces as the reference decoder keeps them - what a P picture did to its
  forward reference, what a second field did to its first -; a field interlace frame as two field pictures, bPicStructure 1 / 2 and bSecondField, the second behind its
  0C start code, each written into its field's lines (DxvaDecodedPicture::field), a B field pair's REFDIST from bMV_RPS; range reduction (RANGEREDFRM: the reduced
  reconstruction kept as the reference, references scaled by the difference) and range mapping (bPicOBMC) applied to the output only; the type, frame coding mode,
  field order, RANGEREDFRM and the signalled intensity compensation checked against the bitstream); MPEG-4
  (DxvaMpeg4.cpp) makes the video object layer from DXVA_PicParams_MPEG4_PART2 and DXVA_QmatrixData and takes the plane header from the bitstream (checked against
  the parameters) or, without a start code, from the parameters with wMBbitOffset; direct mode uses TRB / TRD. Refused as Unsupported: VC-1 pan-scan and progressive
  segmented frames, post-processing into wDeblockedPictureIndex (a display hint, POSTPROC), multiresolution coding (RESPIC, refused as Invalid), MPEG-4 static sprites and reduced resolution planes. Each picture comes back at
  once, in decoding order, kept under a key (the kernel's allocation of its luma plane), so the reference list may change from picture to picture. The kernel's decoder is
  a `VideoDecoderObject` (escapes CreateVideoDecoder / DestroyVideoDecoder / QueryVideoDecodeSupport); the stream's `DecodeFrame` decodes into the output planes and writes a
  `VideoDecodeStatus` (Direct3D 12's decode statistics). A picture that cannot be decoded leaves the planes alone and says so in the status: the stream goes on. An H.264
  field picture writes only its field's lines of the planes (`DxvaDecodedPicture::field`); the second field of a pair goes into the same surface beside the first, and the
  decoder decodes it into the frame it keeps for that surface. Direct3D 12 reports `D3D12_VIDEO_FRAME_CODED_INTERLACE_TYPE_FIELD_BASED` for H.264 (with
  `HEIGHT_ALIGNMENT_MULTIPLE_32_REQUIRED`), d3d11.dll's H.264 decoders are field-based.
- **H.264 gaps in frame_num** (8.2.5.2): the program infers the "non-existing" frames (`h264::Dpb::fill_frame_num_gap`, which the CPU decoder runs itself) and names
  them in RefFrameList with their own FrameNumList entries. ffmpeg's dxva2_h264.c / d3d12va_h264.c give such a frame the surface of the short-term reference before it
  and leave NonExistingFrameFlags 0: the decoder then keeps a copy of that surface's picture under the second frame number (`Picture::alias`). An entry
  NonExistingFrameFlags marks is a mid-grey frame whose surface is not read. Either way it takes its place in the P lists by FrameNumWrap and, with
  pic_order_cnt_type 0, stays out of a B slice's lists (8.2.4.2.3).
- **AV1 large-scale tiles** (7.3, a light field's tile lists): DXVA has only `DXVA_Tile_AV1.anchor_frame`. A call whose first tile control names an anchor frame is
  one tile list: the picture parameters are the camera frame header, control i is tile_list_entry() i (row / column the anchor tile, the anchor a surface index - a
  frame decoded into it before), and the output frame goes into CurrPicTextureIndex. Neither DXVA_PicParams_AV1 nor D3D12 (its decode arguments are the same DXVA
  structures) can say output_frame_width_in_tiles_minus_1 / _height_: the output frame is the decoder's size in whole tiles (SampleWidth / TileWidth columns),
  entry i at (i % columns, i / columns), samples no tile wrote 0 (as libaom), and a list that does not fit is refused. `av1::Core::decode_tile_list_external`
  decodes it; the CPU decoder takes tile list OBUs itself (`Av1Decoder::set_large_scale_tile`, anchors by `keep_anchor_frame`). Bit for bit with libaom's
  lightfield_tile_list_decoder (tests/data/av1_lst.ivf, make_av1_large_scale_tile.sh).
- **Processing.** The stream's `VideoProcess` (Hardware/VideoCodec/VideoProcess.hpp): up to 16 streams converted between NV12 / P010 / P016 / BGRA8 / BGRX8 / RGBA8 /
  RGB10A2 / RGBA16F and the SDR colour spaces (BT.601 / 709 / 2020, studio and full range, gamma 2.2, 2.4 and linear, chroma siting), scaled bilinearly, oriented, blended,
  composed over a background in a target rectangle; PQ and HLG (BT.2100) in and out, converted in absolute light (SDR white = BT.2408's 203 cd/m2) and tone mapped by
  BT.2390's EETF on the brightest channel with the content's and the display's HDR10 light levels. Before sampling, a stream may be made a scratch picture of its own
  (VideoProcessStages.inc, plane by plane in the source's layout, so it is format-independent): the view of a stereo picture (side by side, top and bottom, rows,
  columns, checkerboard, mono offset, flipped frames), deinterlaced (bob, or motion adaptive: yadif's algorithm with a past and a future frame, the same as ffmpeg's yadif
  sample for sample), noise reduced (3x3 sigma filter) and edge enhanced (3x3 unsharp mask). ProcAmp (DXVA's formulas on Y'CbCr: an affine map folded into the
  conversion) and the luma key (D3D11's truncated code range) are applied per sample, anamorphic scaling in the destination mapping. `VideoProcess` takes these as an
  optional part after the streams (`kVideoProcessExtension`, CommandStream.hpp: the display's light levels, then 25 words per stream, with the reference frames' planes).
- **Planes.** The kernel has no one- or two-channel formats: NV12 / P010 planes, and R8 / R8G8 / R16 / R16G16 textures, are kept widened (`(r, g or 0, 0, maximum)` in an
  A8B8G8R8 or A16B16G16R16 image), so a view of the narrow format reads the right values; `CopyBufferImage` takes a `NarrowLayout` to copy the narrow rows, and render
  targets of those formats write their channels only. d3d12.dll makes every plane of every array slice of a planar texture an allocation of its own
  (`Resource.plane_alloc`), which keeps P010 arrays and planar render targets within what the kernel's images can be. A shared planar texture is one win32k resource of
  all those allocations (an opener gets every one); a placed one has them one after the other in the heap (`planar_layout`, 4 KiB apart; `GetResourceAllocationInfo`
  counts the widened images); aliasing barriers name each. A planar texture may be in a custom heap the CPU sees: each plane of each slice is a subresource of
  `WriteToSubresource` / `ReadFromSubresource` in its plane's format (widened and narrowed on the way, as every narrow format's texels are). Reserved planar and packed YUV
  textures are refused (`E_INVALIDARG`, and no `D3D12_FORMAT_SUPPORT2_TILED`): Direct3D defines no tiles for video formats, and DXGI's format tables have "Tiled Resource"
  disallowed for every one of them.
- **Mirrors** (`VidMm::set_mirror`, escape `SetMirror`): what a view the kernel's images cannot be is made of. A mirror is a texture (2D, or a 2D array - also of the wide
  formats now: the software and virgl devices take layers of Vec4s, `IImage::write_layer_pixels_float`) whose layers are copies of other 2D images of the same size and
  channel width; `Allocation::version()` counts each image's changes and a stream copies a source again when its version moved: when a root table that reaches the
  mirror is set outside a pass (where what the GPU drew can be read back first; d3d12.dll sends such a binding before the pass, after the pipeline, so only the image the
  pipeline declares is copied), and from the CPU's copy when a draw binds it. `kMirrorInteger` gives an integer image of the same bits (alpha 1): Direct3D's
  `R8_UINT` / `R8G8_UINT` / `R16_UINT` / `R16G16_UINT` views of a plane. `kMirrorWriteBack` makes a render target or a storage image whose channels go back to the plane
  when the pass that drew into it ends, or after the dispatch that wrote it (an integer render target view; an unordered access view of a plane, integer or not, and of a
  narrow non-planar texture's one image - so a store leaves the widened image's other channels as zeros and the maximum); an array mirror that writes back gives each
  layer to its own source (a render target view of several slices of a plane, drawn into by layer). A Texture2DArray view of planes has two images: the plane of its first slice (a shader that declares a 2D
  texture) and an array mirror of its slices (`Descriptor::alternate`, chosen by the unit's declared kind: one that declares an array sees every slice).
- **4:2:2 and 4:4:4.** AV1 profiles 1 and 2 (`D3D12_VIDEO_DECODE_PROFILE_AV1_PROFILE1`, `_PROFILE2`, `_12BIT_PROFILE2`, `_12BIT_PROFILE2_420`; DXVA_ModeAV1_VLD_*) decode
  into the formats Windows' decoders use: AYUV / Y410 (8- and 10-bit 4:4:4), YUY2 / Y210 (4:2:2) and NV12 / P010 (monochrome), Y216 / Y416 (12-bit 4:2:2 / 4:4:4),
  P016 (12-bit 4:2:0 and monochrome; 12 bits in the top of each word). `VideoDecoderDesc::surface_format` (hardware::DxvaSurfaceFormat; 0 and 1 are NV12 and P010 as
  the `ten_bit` word was) names it; the packed ones are one image of their views' pixels (AYUV and YUY2 A8B8G8R8, Y410 A2B10G10R10, Y416 / Y210 / Y216 A16B16G16R16;
  the 4:2:2 ones half as wide, a pixel a pair), into which `DecodeFrame` copies the rows the decoder packed. d3d12.dll has the packed formats (views of their pixels'
  format only, render targets of AYUV only, mip levels of AYUV and Y416 - DXGI's tables give them to those two; Y416's need `kFeature2MipmappedLayers`, as do arrays of levels, which are one array allocation -, arrays of one level one allocation a slice, committed, placed - `planar_layout` -, shared or in a CPU-visible custom heap as a planar texture's planes are; a render
  target view of a slice, a Texture2DArray view of several slices through an array mirror) and Direct3D 12's planar P208 / V208 / V408 (two or three planes); the processor reads and
  writes AYUV, Y410, Y416, YUY2, Y210, Y216 and P208 (4:2:2 chroma sited as the colour space says, horizontally).
- d3d12.dll: `ID3D12VideoDevice` .. 3, decoders and heaps (a heap is the kernel's decoder), processors, the VIDEO_DECODE / VIDEO_PROCESS queues and lists, decode
  statistics queries (video.inc; video_types.h has what mingw's d3d12video.h lacks). d3d11.dll: `ID3D11VideoDevice` .. 2, `ID3D11VideoContext` .. 3 on those
  (docs/Direct3D11.md).
- Encoding and motion estimation (Direct3D 12 only; `src/Kernel/Dxgkrnl/VideoEncode.{hpp,cpp}`, stream ops EstimateMotion / ResolveMotionVectors / EncodeFrame /
  ResolveEncoderMetadata, states kVideoEncodeRead / Write; d3d12.dll `encode.inc`): the motion estimator (`Software/MotionEstimator`: 8x8 / 16x16 blocks, SAD, whole-sample
  search +-8 around the predictors, then half and quarter refinement with H.264's interpolation; resolved into an R16G16_SINT texture - an R16G16Sint image of its own, or (x, y, 0, 1) in an A16B16G16R16Sint one)
  and an H.264 encoder driven like a D3D12 driver is (`Software/H264PictureEncoder` + `H264PictureEncoderSyntax`: the program keeps the DPB, names lists / marking /
  modifications and writes SPS and PPS; the slices are Main or High, CAVLC or CABAC, Intra16x16 and Intra8x8 (all 9 modes), 16x16 P / B partitions with 4x4 or adaptive 8x8
  transforms, P_Skip, B_Skip / B_Direct_16x16 by spatial or temporal direct prediction (direct_8x8_inference 1), quarter-sample vectors, filter modes 0-2, row-based intra
  refresh (the refreshed rows predict only from refreshed rows), slices by macroblocks / rows / count / bytes (a slice taken back to the macroblock that does not fit), a QP
  per macroblock, cabac_zero_words where 9.3.4.6 needs them), picture-level rate control `Software/VideoRateControl` (CQP, CBR, VBR, QVBR, absolute QP map; state in the
  encoder heap, re-encode on overflow). Any even size 16 .. 4096: coded in whole macroblocks, the program's SPS crops; the encoder heap (`video_encode_heap_bytes`) keeps,
  after the rate control state, a reference store of 17 slots - each reconstructed reference's macroblock motion (the co-located picture of direct prediction) and the samples
  of its coded size its texture has no room for - found by the texture's allocation id and POC (a reference not found: predictions keep inside its texture, no direct from
  it). ID3D12VideoDevice4 / ID3D12VideoEncoderHeap1 / ID3D12VideoEncodeCommandList4: EncodeFrame1 with CPU input maps (dirty rectangles -> unchanged macroblocks copying the
  reference, move regions -> per-macroblock search hints in the three search modes, the QP map of the picture control) and frame / subregion PSNR metadata through
  ResolveEncoderOutputMetadata1 (ResolveEncoderMetadata's packet takes the two PSNR buffers). Not done, and the queries say so: input maps from GPU textures (so
  ResolveInputParamLayout has nothing to resolve), the per-block QP / SATD / bit-allocation metadata maps (their R8_SINT / R32_UINT textures do not exist in d3d12.dll),
  frame analysis (two-pass), subregion notification, intra constrained slices, deblocking modes 3-6 (SVC's). Tests:
  `tests/run_host_tests.sh VideoEncodeTest`, `tests/h264_encode_ffmpeg.sh` (ffmpeg's decode = the reconstruction, 14 streams),
  `tests/emulated_dx.sh tests/windows/d3d12encodetest.c`, `tests/d3d12_qemu.sh d3d12encodetest`, `tests/wow64_qemu.sh d3d12encodetest`.
- HEVC and AV1 encoding (`src/Kernel/Dxgkrnl/VideoEncodeCodecs.hpp` - argument layouts and metadata -, `VideoEncodeHevc.cpp`, `VideoEncodeAv1.cpp`, `VideoEncodePlanes.cpp`;
  d3d12.dll `encode_codecs.{h,inc}`, `video_encode_types.h`): EncodeFrame's word 0 names the codec and each has its own arguments; NV12 and P010 inputs (P010 widened as
  A16B16G16R16, which the EncodeFrame packet takes for these two). `Software/H265PictureEncoder`: HEVC Main / Main 10 driven as D3D12 drives it (the program keeps the DPB -
  every descriptor, used or not, short or long term - and writes VPS / SPS / PPS as the header says; the slices carry the reference picture set explicitly, list
  modifications given or derived), CTB 16 / 32 / 64, coding quadtree by cost, 2Nx2N intra (35 modes) and inter (merge / skip / AMVP, one or two lists, MotionEstimator
  candidates refined with HEVC's interpolation), CABAC, cu_qp_delta per CTB for QP maps, SAO chosen per CTB (coded once without, decoded, chosen, coded again), slices by
  CTBs / rows / count; the reconstruction is the in-tree decoder's (`h265::Core::external_*`) of the slices, so deblocking and SAO are the decoder's. `Software/Av1PictureEncoder`
  (+ `Av1EncBitstream`: the OBU bit writer, the od_ec symbol coder): AV1 Main 8 / 10 bits on the decoder's own block and context structures (contexts, ref MV stacks and
  predictions are the decoder's), primary_ref_frame always none, single reference, 13 intra modes, NEAREST / NEAR / GLOBAL / NEWMV, every interpolation filter
  (switchable too), TX_MODE_LARGEST / SELECT, delta q per superblock, cdef_idx chosen per 64x64; the program writes every OBU from the post-encode values, which
  ResolveEncoderOutputMetadata gives after a record a tile (the tile layout, then D3D12_VIDEO_ENCODER_AV1_POST_ENCODE_VALUES); the reconstruction is
  `av1::Core::decode_frame_external`'s of the tiles. Rate control as H.264's (AV1's quantizer indices put on its QP scale by the 8-bit AC quantizer). The queries
  (`encode_codecs.inc`) say exactly this, including D3D12_FEATURE_VIDEO_ENCODER_FRAME_SUBREGION_LAYOUT_CONFIG for AV1 tiles. Not done (refused / not reported):
  HEVC intra refresh, slices by bytes, intra constrained slices, NxN / AMP / rectangular partitions (AMP is allowed in the SPS, 2Nx2N coded), 4x4 luma transforms,
  temporal MVP, the range extension values of PICTURE_CONTROL_CODEC_DATA_HEVC1; AV1 compound prediction and skip mode, palette, intra block copy, filter intra, CfL,
  warped motion and OBMC, super-resolution, loop restoration, segmentation, delta LF, quantizer matrices, 128x128 superblocks, layers, CDF inheritance from references;
  both: EncodeFrame1's input maps but the QP map, PSNR metadata, frame analysis. Tests: `tests/run_host_tests.sh VideoEncodeHevcTest VideoEncodeAv1Test` (every stream decoded
  by the in-tree decoders = the reconstruction, the encoders' own pre-filter pictures = the decoders' (no drift), the kernel's EncodeFrame against the encoders driven
  directly), `tests/hevc_encode_ffmpeg.sh` and `tests/av1_encode_ffmpeg.sh` (ffmpeg's HEVC decoder, libdav1d and libaom: their decode = the reconstruction, byte for
  byte), `tests/emulated_dx.sh tests/windows/d3d12encodecodectest.c` (the queries, HEVC Main / Main 10 and AV1 8 / 10-bit encodes decoded again through
  ID3D12VideoDecoder with DXVA parameters from the program's settings and the post-encode values), `tests/d3d12_qemu.sh d3d12encodecodectest`,
  `tests/wow64_qemu.sh d3d12encodecodectest`.
- Decode conversion and histograms (`DecodeFrame`'s optional parts, CommandStream.hpp `kDecodeConversion` / `kDecodeHistogram`): the picture is decoded into its
  surface - Direct3D 12's reference texture - and then converted by the video processor into another surface (any format and colour space the processor writes, from the
  decode colour space, scaled to any size: `D3D12_VIDEO_DECODE_CONVERSION_ARGUMENTS` / `ARGUMENTS1`, D3D11 decoder downsampling); histograms of the decoded picture
  (`hardware::vp_histogram`: Y, U, V and the alpha of AYUV / Y410 / Y416, 64 bins of the codes, 32-bit counters, at 256-aligned offsets) go to the program's buffers.
  d3d12.dll reports both truthfully (`DECODE_CONVERSION_SUPPORT`: supported for every pair the processor converts, sizes 1 .. 16384; `DECODE_HISTOGRAM`).
- Not there: HEVC 16-bit samples through DXVA (DXVA_PicParams_HEVC's bit depths are three bits: the Main16 profile takes 15 at most), HEVC separate colour planes (no
  profile has them), extended precision and CABAC bypass alignment checked against a reference (no decoder at hand has them: implemented from the standard, not
  verified), AV1 tile lists whose output frame is not the decoder's size (DXVA cannot say), VC-1 multiresolution coding (RESPIC: no reference decoder to check it against),
  MPEG-4 low-latency sprites (sprite pieces and updates; static sprites are refused through DXVA, which has no fields for them), protected
  content (crypto sessions and authenticated channels are refused).
- Tests: `tests/run_host_tests.sh DxvaDecoderTest VideoProcessTest DxgkrnlTest` (every codec bit for bit with ffmpeg; the DXVA buffers filled as ffmpeg's dxva2_*.c
  fill them; interlaced H.264 from x264 MBAFF streams and cuts of the JVT conformance streams, tests/data/make_h264_interlaced_fixtures.py; VC-1 and MPEG-4 Part 2, which ffmpeg has no DXVA path for, from the software decoder's view of the stream (VC-1 field pairs as two pictures, as dxva2_vc1.c sends
  them; range reduction against ffmpeg with scripts/ffmpeg-vc1-range-reduction.patch, which makes it follow SMPTE 421M); HEVC Main 10 over tests/data/hevc10_*.h265, scripts/gen_hevc10_test_streams.sh, with the PCM stream of
  scripts/gen_hevc10_pcm_stream.py; the HEVC range extensions over tests/data/hevcrext_*.h265 - x265's 4:0:0 / 4:2:0 / 4:2:2 / 4:4:4 streams of 8 to 12 bits,
  scripts/gen_hevc_rext_test_streams.sh - and hevcrexttool_*.h265, the range extension tools from the random-syntax encoder scripts/gen_hevc_rext_tool_streams.py,
  every picture checked against the MD5 of each plane it carries: FFmpeg's decode, libde265's where FFmpeg decodes a tool otherwise than the standard (the script
  lists where FFmpeg n9.0.2 and libde265 1.1.3 differ)), `tests/emulated_dx.sh
  tests/windows/d3d12videotest.c` and `d3d11videotest.c` (H.264 - progressive and an MBAFF + field picture stream -, HEVC Main 10 - tests/windows/hevc10_dxva.h, made by `DXVA_HEVC10_DUMP=... DxvaDecoderTest` -, HEVC range extensions - 4:2:2 10-bit into Y210, 4:4:4 with the
  range extension tools into AYUV, 4:4:4 12-bit into Y416: tests/windows/hevcrext_dxva.h, `DXVA_HEVCREXT_DUMP=... DxvaDecoderTest` -, VC-1 (progressive; and interlaced frame and field pictures with slices, range mapping), MPEG-4 Part 2 and AV1 profiles 1 and 2 decoded through both APIs, bit for bit with ffmpeg -
  the AV1 streams' DXVA buffers dumped by DxvaDecoderTest with DXVA_AV1_DUMP, tests/data/make_av1_dxva_data.py; the packed and planar YUV textures; the processor
  against BT.709; motion adaptive deinterlacing against ffmpeg's yadif output, bob, ProcAmp, luma key, stereo, HDR10 into SDR), `tests/d3d12_qemu.sh d3d12videotest`,
  `tests/wow64_qemu.sh d3d12videotest d3d11videotest` (32-bit). VideoProcessTest's references: tests/data/videoprocess/make_refs.sh (zscale, yadif, geq).
- Temporal processing (`VideoProcessTemporal.inc`; VideoProcess's second optional part, `kVideoProcessExtension2`): frame rate conversion (an output between two
  progressive frames blends them by its time, or repeats the earlier; d3d12.dll reads the input over output frame rate and `RateInfo` as DXVA-HD's stream data are read -
  `InputFrameOrField` the input frame, `OutputIndex` the outputs made from it -, video.inc VideoRate); inverse telecine of 3:2, 2:3:3:2, 2:2:2:4 (a cycle of five
  frames to four: 30i to 24p) and 2:2 pulldown - ffmpeg `fieldmatch` (c / p / n matches, the least combing) then `decimate` (the frame most like its predecessor of
  each cycle dropped), a frame still combed (fieldmatch's comb test) deinterlaced instead; a clean telecine gives the film frames bit for bit, as ffmpeg's
  `fieldmatch,decimate` does (tests/data/videoprocess/ivtc_ffmpeg.sh); motion compensated deinterlacing - bwdif with the other fields' samples taken along each 8x8 block's
  motion (MotionEstimator between the missing field's neighbours, bilateral candidate check, Catmull-Rom field interpolation): on the test's moving scenes 41.8 / 41.0 dB
  luma PSNR against the progressive truth where ffmpeg's yadif gives 34.1 / 33.6, bwdif 41.1 / 38.3, mcdeint 40.5 / 39.5 (tests/data/videoprocess/mcdeint_ffmpeg.sh).
  D3D12 has no MC flag (its CUSTOM deinterlacing stays motion adaptive): d3d11.dll's rate conversion 2 asks for it through video_extra.h.
- The stereo adjustment filter (the views moved apart: left left, right right, -128..127 pixels; D3D12 filter 7), automatic processing (`VideoProcessAuto.inc`; D3D12
  `AutoProcessingSupport` / D3D11 `AutoStreamCaps` denoise, deringing, edge enhancement, colour correction: each when the picture calls for it - Immerkaer's noise
  measure, ripples beside strong edges, enlargement of 1.5 and more, a luma range of a tenth to six tenths - so a clean picture comes out as it went in), HDR10+
  dynamic metadata of a stream (`vp_parse_hdr10plus`: the ST 2094-40 T.35 payload as DXGI_HDR_METADATA_HDR10PLUS holds it; tone mapping along its basis OOTF - the
  knee and Bezier curve - from the scene peak to the targeted display, adapted to the actual display, its colour saturation weight; window 0 only, as HDR10+'s
  profiles have).
- Not in the processor: inverse telecine of other cadences (3:2:3:2:2, 5:5, 6:4, 8:7, 2:2:2:2:2:2:2:2:2:2:2:3: not reported), frame rate conversion of interlaced
  frames other than deinterlacing and inverse telecine (VIDEO_PROCESS_SUPPORT says unsupported), HDR10+ windows 1 and 2 (read, not applied), the auto processing kinds
  flesh tone mapping, image stabilization, super resolution and anamorphic scaling (not reported), scRGB (G10) input brighter than white is clipped, not tone mapped.

## Not done

- **The user-mode half and the system-call glue**: no `NtGdiDdDDI*` services in `src/Process/Windows*.cpp` / `win32u`, no d3d9 / dxgi / d3d11 DLLs. `lock()` returns a view of
  kernel memory; mapping it into the process (a section) is the system-call layer's job.
- **Shaders**: SM2 / SM3 / SM4 / SM5.0 / SM5.1 vertex and pixel shaders (no geometry / tessellation / compute shaders, no 5.1 register spaces or arrays of resources). The pixel stage of Intel Gen9 is two fixed kernels, so a general pixel
  shader runs there only on the software, virgl (as unvalidated TGSI) and SVGA3D (unvalidated) paths. No loops, calls, predication, ps_1_x, constants above c7 (push constants are
  128 bytes), depth output, fog / point size, cube and volume samplers. Pipeline state is limited to what `IGraphicsDevice` offers (one colour target, 2 textures, alpha blend on/off, depth and stencil).
- No group-shared memory or barriers (a compute shader that needs `sync` or `dcl_tgsm` is refused), no multisampled shader resources (`Texture2DMS`, `ld2dms`), no depth resolve, no alpha-to-coverage or sample mask, no per-sample shading, no centroid interpolation, no mipmapped / render-target / float cube, volume or array textures (`d3d11.dll` and `d3d9.dll` keep the first level of the ones a program makes with more), and of the colour formats only what "More colour formats" and "More texture formats" below list.
- Sync objects cannot be shared between processes. No timeout detection and recovery (a hung device cannot be reset), no vblank / scan line, gamma, overlays or VidPN topology
  (multi-monitor); display mode listing and switching go straight to the GPU driver.
- Video memory is accounted per adapter but there is no per-process budget or priority-based paging between processes.
- Run through the kernel: the software rasterizer (everything) and the simulated Intel Gen9 engine (the self-test picture matches the software device). virgl was run in QEMU against a real virglrenderer (the conformance scenes above); SVGA3D and the V3D hardware were not run here (V3D has no host simulation of its rasterizer; its matrix handling is checked numerically). One lock covers the kernel.


### More colour formats

`A32B32G32R32Sint`, `A16B16G16R16Uint` / `Sint`, `A8B8G8R8Uint` (the integer formats, narrow ones saturate), `A16B16G16R16Snorm` and `A2B10G10R10` (10-bit colour with two bits of alpha) join the wide formats: pixels travel as `Vec4`s, laid out and quantised by `Graphics/WidePixels.hpp` (one place for the software device, the memory manager and virgl). Features: `kFeatureIntegerFormats`, `kFeatureSnormFormats`. virgl ids (from virglrenderer's `virgl_hw.h`): R32G32B32A32_SINT 200, R16G16B16A16_UINT 188, _SINT 192, R8G8B8A8_UINT 180, R16G16B16A16_SNORM 59, R10G10B10A2_UNORM 8. Two conformance scenes match the real GPU. `SV_PrimitiveID` is a `PRIMID` input on virgl and matches too.

### More texture formats

The rest of Direct3D 11 / 12's colour formats are allocation formats, numbered from 64 in `dxgk::Format` as in `graphics::Format` (the same number is the same format, so other
changes can append to the first run below 64): `R8Unorm` .. `B4G4R4A4Unorm` (64 .. 69: R8, R8G8, A8, B5G6R5, B5G5R5A1, B4G4R4A4), `R8Snorm` .. `R10G10B10A2Uint` (70 .. 96:
R8 / R8G8 snorm, uint, sint; R8G8B8A8 snorm and sint; R16 and R16G16 unorm, snorm, uint, sint, float; R32G32 and R32G32B32 uint, sint, float; R11G11B10_FLOAT; R9G9B9E5;
R10G10B10A2_UINT) and `Bc4Unorm` .. `Bc7Srgb` (97 .. 104: BC4 and BC5 unorm and snorm, BC6H unsigned and signed, BC7 and its sRGB form). Each is in memory as Direct3D lays it
out (`pixel_bytes`: 1, 2, 4, 8 or 12 bytes; BC4 blocks are 8 bytes, the others 16), so locks, `CopyBufferImage`, `CopyRegion` and the reserved textures' tiles move its own bytes.
`is_known_format` is what a format number from user mode may be.

- **The packed 32-bit ones** (64 .. 69) are kept by the devices as 0xAARRGGBB words with what the format holds of a colour (`quantize_packed32`: the channels it has rounded to
  their bits, the others 0, alpha 255) - so they have mip levels, multisampling aside, as the 8-bit formats do; VidMm turns the backing's texels into words and back
  (`packed32_to_argb` / `packed32_from_argb`), and the software device samples the 5-, 6- and 4-bit codes as their exact values (`packed32_to_floats`).
- **The wide ones** (70 .. 96) travel as `Vec4`s (`Graphics/WidePixels.hpp`: every layout from one table - count, bits and kind of the channels -, R11G11B10's unsigned
  small floats rounded to nearest even with infinity and NaN kept and negative values 0, RGB9E5's shared exponent as `EXT_texture_shared_exponent` and Direct3D compute it);
  the channels a format does not have read as 0 and alpha as 1 (an integer 1). They have one level, as the other wide formats.
- **BC4 - BC7**: decoded by `Graphics/BlockCompression.hpp` (all BC6H and BC7 modes, partitions, anchors and p-bits; BC4 / BC5 snorm with -128 as -127; reserved BC6H modes and
  a BC7 block without a mode bit decode to zero) where the device does not sample them as blocks: BC4 / BC5 unorm and BC7 into 8-bit pixels (with levels), BC4 / BC5 snorm into
  R32G32B32A32Float and BC6H into R16G16B16A16Float (one level). `BlockCompressionTest` compares the decoder with Mesa's (softpipe, through `scripts/bc-reference.c`; the
  reference is `tests/data/bc/reference.bin`): BC6H and BC7 bit for bit on blocks of every mode, BC4 / BC5 within a code; plus blocks worked out by hand, every bit of a block
  turned, shortened views.
- **What a device does with each** is `IGraphicsDevice::format_caps` (`graphics::FormatCap`: sampled, filtered, render target, blendable, storage image, mip levels,
  multisample, native blocks), and what allocations of it may be is `VidMm::format_caps` (a block-compressed format the device does not sample is what its decoded image can
  be). `VidMm::create` refuses what the device cannot do with `kNotSupported`; the adapter hands the table to user mode at the end of the capabilities blob (`AdapterCaps::format_caps`:
  the first format, the count, a word each), which d3d12.dll's `CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT)` and resource creation follow. The CPU renderer has all of them
  (no render targets of R9G9B9E5, no storage images of the 96-bit formats, of A8 or of the 16-bit packed ones - Direct3D has none -, the integer ones not filtered or blended).
  virgl reads the host's sampler and render masks (`virgl_caps_v1`) and gives each format the host format of the same layout (`virgl::extra_format`, from virglrenderer's
  `virgl_hw.h`: R8 64, R8G8 65, A8 10, B5G6R5 7, B5G5R5A1 5, B4G4R4A4 6, R8 snorm / uint / sint 74 / 177 / 181, R8G8 75 / 178 / 182, R8G8B8A8 snorm / sint 77 / 184, R16 unorm
  / snorm / uint / sint / float 48 / 56 / 185 / 189 / 91, R16G16 49 / 57 / 186 / 190 / 92, R32G32 uint / sint / float 194 / 198 / 29, R32G32B32 195 / 199 / 30, R11G11B10 124,
  R9G9B9E5 125, R10G10B10A2_UINT 253, RGTC1 113 / 114, RGTC2 115 / 116, BPTC_RGB_FLOAT 257 (BC6H signed), BPTC_RGB_UFLOAT 258, BPTC_RGBA_UNORM 255, BPTC_SRGBA 256); a format
  the host does not draw into is a sampler view alone, the packed 32-bit ones are transferred in their own bytes, and an integer texture is bound with a nearest sampler
  (GL takes an integer texture with a linear filter as incomplete and reads zeros, also in texel fetches; Direct3D's `Load` ignores the sampler). `LockedDevice` passes both
  questions on to the device it wraps. The other devices have none of them.
- **Sizes**: `kMaxTextureDimension` is 16384 (Direct3D 11's); `IGraphicsDevice::max_image_dimension` says what a device has (4096 unless it says more: the CPU renderer 16384,
  virgl the host's `max_texture_2d_size` from `virgl_caps_v2`, read with version 2 of the second capability set), and `AdapterCaps::max_texture_dimension` is that (VidMm
  refuses larger textures with `kNotSupported`). Video surfaces stay at most 4096 (`kMinTextureDimension`).
- d3d12.dll keeps R8 / R8G8 / R16 / R16G16 unorm and typeless as the kernel's own narrow formats where the adapter has them for the texture's use, widened into four-channel
  images elsewhere and for the planes of the video formats (see *Strip cuts, filters, samplers of Direct3D 12, line modes, narrow formats* below); every other format above is a
  format of its own there (`KF_R8` .. `KF_BC7_SRGB`), and d3d11.dll keeps every format as it is.
  `TextureFormatsTest` checks the conversions (every code of the packed and the 8- and 16-bit formats, the small floats, the shared exponent); d3d12test's `test_formats`
  checks on each adapter that the support answers are what can be made, that a shader's `Load` reads what was written in each format, copies keep the bytes, render targets of
  each kind keep what a pixel shader wrote in their encoding, BC4 - BC7 blocks are read as their decoded texels (BC7's sRGB form linearised and filtered on linear values), a
  typed clear of R11G11B10 and 16384-wide textures.

### Subroutines and switches in DXBC

`D3d9Dxbc.cpp` (`Flattener`) rewrites a program's words before they are compiled, for every stage and every device (the interpreter and TGSI for virgl run the same
flow control):
- `call l#` is the subroutine (from `label l#` to the next label or the end) inlined; `callc_z` / `callc_nz` is that inside `if_z` / `if_nz`. Calls nest 32 deep (Direct3D's
  limit); deeper can only be recursion, which Direct3D does not have: kInvalidParameter. A subroutine that returns before its end is a loop run once, its returns breaks; from
  inside its own loops and switches a return sets a flag first and each loop and switch out to the subroutine's breaks on when it ends with the flag (cleared after it).
  A label of a function body (an interface's) is kNotSupported: d3d11.dll specialises those shaders, inlining their subroutines itself so that each has its body's `this`.
- `retc_z` / `retc_nz` of the main program is `if`, `ret`, `endif` (a return inside an if ends the program: `kRetEarly`); in a hull shader, where `ret` ends a phase, it is
  kNotSupported.
- A switch whose cases each end in a break at their top level (or a return or continue), with no other break of the switch, is a chain of `if` / `else` on the selector
  (copied first into a temporary of its own); the default case is the last else, case labels together are one case. Any other switch (a break inside an if of a case,
  `breakc`, a case that falls into the next, a default between cases) is a loop run once around one `if` per case, taken when the selector matched that case or one before
  it, or when it is none of the cases for the default (computed before the loop); the breaks of the cases break the loop. A `continue` of a loop outside such a switch sets a
  flag and breaks; after the switch it continues (or breaks on to the next such switch out).
- The temporaries: one for the flags and one for each level of switches inside switches, after the program's own (every `dcl_temps` is made larger; more than 253 in all
  is kNotSupported). A program without these instructions is left as it is. Loops and ifs nest 64 deep in the instruction form (was 8 and 24).
- A hull shader that moves `vOutputControlPointID` into a temporary and indexes the control points with it (`v[r1.x + 0][0]`, what Microsoft's compiler makes in a
  subroutine) reads the control point being made, as `vicp[vOutputControlPointID]`: the temporary is followed through the instructions without flow control after the
  move and until it is written. A geometry shader's inputs indexed by a register are still not taken (`[unroll]` the loop).
- Fixed on the way: a geometry shader's `dcl_input v[N][r]` before its `dcl_inputprimitive` (the order Microsoft's compiler writes) was refused.
- Tests: `tests/run_host_tests.sh DxbcFlowTest` - Microsoft's compiler's `[call] switch` and switch shaders (`tests/data/dxbc/*call*.hlsl`, `ps_switch.hlsl`, every stage)
  run on the interpreter and as TGSI on the test's TGSI machine against what the HLSL says, hand-made programs (returns out of loops of nested subroutines, continue from a
  switch, a falling case, callc, retc, the nesting limit, recursion), truncations and every bit turned under the sanitizers; d3d11test / d3d12test draw with them.

### More queries

`BeginQuery` takes an optional third word, a `graphics::QueryKind`: 0 occlusion count, 1 occlusion predicate (0 or 1), 2 IA vertices, 3 IA primitives, 4 VS invocations, 5 PS invocations (`kFeatureStatisticsQueries`; the result is still a 64-bit number in the `kAllocQueryResult` buffer); 11 pipeline statistics (`QueryKind::PipelineStatistics`: eleven 64-bit numbers in `D3D11_QUERY_DATA_PIPELINE_STATISTICS`'s order, `graphics::PipelineStatistic`), 12 `kQueryKindPipelineStatistics1` (D3D12_QUERY_DATA_PIPELINE_STATISTICS1: those and the amplification and mesh shader invocations and mesh primitives, 0 as there are no such stages) and 13 `kQueryKindSoStatistics` (D3D12_QUERY_DATA_SO_STATISTICS of the stream in bits 8-9: written, then needed; made of a written and a needed device query). The kernel draws strips and fans as lists (`draw_expanded`), so it corrects the IA vertices and VS invocations of the queries open over such a draw to the strip's own vertices (a four-vertex quad: 4 and 4, not 6). The statistics are: GS invocations once for every primitive and instance a geometry shader takes, GS primitives what its output makes on all streams, C invocations every primitive given to the rasterizer's clipper, C primitives those not wholly outside one plane of the view volume, HS invocations one a patch, DS invocations every domain shader run, CS invocations every compute thread. virgl: each statistic is `PIPE_QUERY_PIPELINE_STATISTICS` with its index (0 .. 10) in the upper half of the type word (virglrenderer maps them to `GL_VERTICES_SUBMITTED` .. `GL_COMPUTE_SHADER_INVOCATIONS`; HS invocations are GL's tessellation control patches). The CPU renderer counts them exactly (VS invocations once for every vertex it reads, including repeats of an index); a GPU may run the vertex shader fewer times and shade helper pixels, so the scene compares the predicates and the IA counts exactly and the shader runs only against "more than none". virgl: `PIPE_QUERY_OCCLUSION_PREDICATE` (1) and `PIPE_QUERY_PIPELINE_STATISTICS` (10 in virglrenderer's `p_defines.h`, which is not the numbering older Gallium used) with the statistic in the upper half of the type word. Not done: timestamps, time elapsed, stream output statistics.

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

(`SetStreamOutput` takes a fourth word, optional: `end`, the byte the binding's writes stop at - a primitive that does not fit before it is not written, nor any after it,
and it counts as needed and not written, as at the buffer's end; `kStreamOutputNoEnd` or left out: the buffer's end; a multiple of 4 within the allocation and not before
the offset. `ICommandBuffer::bind_stream_output` takes it (the CPU renderer cuts its view of the buffer there, virgl makes the host's target that size). d3d12.dll binds
a view with BufferLocation + SizeInBytes as the end, its start read from BufferFilledSizeLocation when the chunk runs.)

(A pipeline whose fragment shader stands in for none - d3d12.dll's null pixel shader for a pipeline state without one - has `kPipelineFlagStandInFragment` (8) in the
CreatePipeline escape's flags word, `GraphicsPipelineDesc::stand_in_fragment`: its runs are not counted as PSInvocations - the CPU renderer does not count them, virgl
ends the pixel shader invocations' host query segment around the draw.)

`dcl_stream` and `emit_stream` / `cut_stream` / `emitthencut_stream` (gs_5_0; at most `kMaxGeometryStreams` = 4 streams, and D3D's rule that more than one stream means every one is a point list) write each vertex to the stream it names; the shader's outputs belong to the stream whose emit follows their writes (the `OSG5` signature names the stream of each). `PipelineDesc::stream_output_count` entries (`StreamOutputEntry{stream, output, start_component, component_count, buffer}`, at most 16, `output` 0 the position as the shader wrote it, +Y up, and n the vertex shader's varying n - 1), `stream_output_stride[4]` (bytes) and `rasterized_stream` (the stream that is drawn, -1 for none) say what is captured; entries of one buffer must be of one stream. The buffers are `kAllocStreamOutput` allocations (`kBufferStreamOutput`) bound with `CommandWriter::set_stream_output(slot, allocation, offset)`; a draw refuses when a buffer its pipeline writes is unbound. Records are appended from the bound offset on; a primitive that does not fit is dropped whole and its stream stops (strips are written as lists of triangles, with the rasterizer's swap of every second one). The escape ABI has trailing words after the hull / domain ones: the entry count and, when it is not 0, the rasterized stream, four strides and, for each entry, `stream | output << 8 | start << 16 | count << 24` and the buffer. `kFeatureStreamOutput` (the CPU renderer and virgl).

**Later additions.** *Stages*: stream output also takes the outputs of a vertex shader (no geometry or tessellation: a primitive of the draw's list topology) or of a domain shader (the tessellator's primitives); only stream 0 exists there, and `rasterized_stream` is -1 or 0. *Drawing another stream*: `rasterized_stream` may name any stream of a geometry shader (its position and varyings go through the pipeline as stream 0's would). *Appending*: `set_stream_output(slot, allocation, kStreamOutputAppendOffset)` (`graphics::kStreamOutputAppend` on the device interface) starts where the last stream output into that buffer ended; the device keeps that in `IBuffer::stream_output_filled`, VidMm keeps it across a device object being dropped. *Queries*: `QueryKind::Timestamp` (`kFeatureTimestampQueries`; the CPU renderer's clock counts 1000 ns for every timestamp unless `SoftwareDevice::set_clock` gives it a real one; the value is taken when the query ends) and `SoPrimitivesWritten` / `SoPrimitivesNeeded` (the kind word's bits 8-9 are the output stream); a primitive that does not fit counts as needed, not as written, and the stream stays closed.

virgl, in more detail than above: the declaration goes with the last vertex-processing stage (geometry, else domain, else vertex shader), which is why the vertex and domain shaders also write the unflipped position to `OUT[5]` (a geometry shader has `OUT[5 + stream]`, one for each stream, so that several streams can capture it). The host rasterizes stream 0 only, so for a pipeline that draws stream *n* the numbers of stream 0 and *n* are exchanged in a copy of the shader: the emits take their stream from an immediate that has `24064 + stream` in its fourth component (found by that marker in the text and patched) and the declaration's stream fields are exchanged too. A shader output that two streams capture cannot be had (the host gives a register one stream): the pipeline is refused. Timestamp is `PIPE_QUERY_TIMESTAMP` (2); the statistics are `PRIMITIVES_EMITTED` (6, written) and `PRIMITIVES_GENERATED` (5, needed) with the stream as the query index in the upper half. **The host's transform feedback**: it ends only when the target objects are destroyed, it resumes with the program it began with (another program's draw fails while the targets are set, so they are unset for draws without stream output and at the end of a pass), and it keeps its position in the buffer. The device therefore makes the target objects at the first draw with stream output and keeps them to the end of the submit (a pipeline with other shaders that has stream output ends the old ones: the buffers then start again at their bound offsets), and counts the primitives written with queries of its own (`PRIMITIVES_EMITTED`, per stream) to learn how far each buffer got for the next submit's appends; they are queries of ours like the submit's own (slots from `kMaxOcclusionQueries` on), so they share the host's target with a submit's `SoPrimitivesWritten` queries through segments. An append to a buffer that is full makes its target the buffer's last 4 bytes (GL refuses a range of none; no record of more than 4 bytes fits, so the primitives are needed and not written; records of 4 bytes or less are refused). Within a submit an append is only to a buffer already bound in that slot (it goes on); to another one that was written in the same submit it is refused. Rebinding a slot ends the targets of the others too (their positions start again at their bound offsets on virgl, on the CPU renderer they go on).

`gs_streams.hlsl` (two point streams), `gs_streams_draw1.hlsl` (stream 1 is drawn, stream 0 captured) and four conformance scenes (two streams, appending with statistics and timestamps, vertex and domain shader output, the second stream drawn) match the real GPU; a unit test covers the contents, overflow, strips, appends, the statistics, timestamps and the refusals. Not done: stream output statistics for the two-in-one `SO_STATISTICS` kind (ask for written and needed one after the other), `SetStreamOutputTargets` offsets other than "start" and "append" (an arbitrary offset is fine for `set_stream_output`), a shader output captured by two streams on virgl, a real clock for the software rasterizer by default.

**Timestamp frequency and disjoint.** `QueryKind::TimestampFrequency` (result 1000000000: the timestamps are nanoseconds) and `QueryKind::TimestampDisjoint` (1 if the clock was interrupted while the query was open, else 0), kind words 9 and 10 of `BeginQuery`, need `kFeatureTimestampQueries` and no draw. Both devices answer them without asking a clock: the CPU renderer's clock cannot be interrupted, and virglrenderer has no `TIMESTAMP_DISJOINT` query (it rejects the type), so the virgl device answers 1e9 and 0 itself (a GL host clock is in nanoseconds; it cannot tell a disjoint interval on desktop GL). The append conformance scene reads both. **The flag is latched**: `IGraphicsDevice::interrupt_clock()` (`Adapter::interrupt_clock()`; a software device also does it when given a new clock with `set_clock`) says the clock was interrupted - a reset, a power transition, a frequency change - and the next `TimestampDisjoint` query reads 1, once; timestamps from before it do not compare with those after. (Direct3D brackets timestamps with a disjoint query; the flag is the interruption since the last time it was asked - a disjoint query open over a stream with timestamps says whether the clock was interrupted before or during that stream.) Nothing in the kernel calls it yet: there is no GPU reset or power management in this layer to call it from.

### Direct3D 12: root signatures, descriptor heaps, resource states

`src/Kernel/Dxgkrnl/D3d12.hpp` / `.cpp`. This is the *binding model* of Direct3D 12 on the existing devices; the command-list model, DXIL and resource heaps are not done (below).

- **Root signatures**: `Kernel::create_root_signature(pid, device, blob, &handle)` parses what `D3D12SerializeRootSignature` writes (versions 1.0, 1.1 and 1.2) or the `RTS0` chunk of a DXBC container: up to 16 parameters (a descriptor table of up to 8 ranges of SRV / UAV / CBV / sampler with offsets, "append" and an unbounded last range; 32-bit root constants; root CBV / SRV / UAV descriptors), 8 static samplers, 64 DWORDs in all. A table is of views or of samplers, never both. The parser is bounds-checked and has truncation and bit-flip tests.
- **Descriptor heaps**: `create_descriptor_heap(type, count <= 4096)` (views or samplers), `write_descriptor` (what `CreateShaderResourceView` and friends say: a texture, a buffer range as unordered access or constant buffer view, a sampler; the allocation is held by the heap), `copy_descriptors` (memmove semantics). A `kAllocConstantBuffer` allocation (16-byte multiples, system memory only, never resident) is what a constant buffer view names: the CPU reads it when a draw needs it.
- **Stream ops** (`CommandStream.hpp`): `SetRootSignature`, `SetDescriptorHeaps` (one of each type), `SetRootConstants`, `SetRootDescriptor` (CBV / UAV; a buffer SRV is refused: no device has buffer shader resources), `SetRootTable`, `ResourceBarrier`. Root signature and heaps are named by handle and collected at queue time like pipelines (`Submission::signatures` / `heaps`).
- **Lowering**: at each draw and dispatch the runner turns the root arguments into the bindings the devices have, through the same code as the plain ops, so the checks are the same: `b0` (root constants, a root CBV, or the CBV at b0 of a table) becomes the push constants (one constant buffer; any other register or a non-zero space is refused), `t<n>` every texture unit the shaders read it through, each with the sampler of its pair (a sampler table, a static sampler, else `s0`, else the defaults: see "Depth buffers read as textures, comparisons, texture and sampler pairs"), `u<n>` storage slot *n*. A descriptor that was never written leaves its unit unbound; every allocation a descriptor names has to be in the stream's allocation list (D3D12's residency list), else the draw is refused.
- **Resource states**: an allocation made with `kAllocTrackState` (any other flags as usual) has a state (`AllocationDesc::initial_state`, D3D12's bits: `state::kRenderTarget` ...). `ResourceBarrier` moves it (a transition names the state it expects; the same state twice, a writing state combined with another, and a state that does not exist are refused; split barriers begin/end; unordered access barriers need an allocation in that state; aliasing barriers are no-ops), and a pass target, a vertex / index / constant buffer, a bound texture, an unordered access view, a stream output buffer, a copy and a resolve each need the matching state (the common state is promoted for reads and copies, as D3D12 does). The states a stream leaves are kept for the next stream; a stream that is refused changes nothing. Allocations without the flag are not checked.
- **Conformance** (virgl matches the CPU renderer): the compute scene through root signature with root constants, root UAVs and a table, and the 3D texture scene through a table, root constants and a static sampler, with barriers.

Reserved (tiled) buffers (`kAllocReserved`, `Kernel::update_tile_mappings`): a buffer of whole 64 KiB tiles with its own backing; each tile is mapped to a heap range or to nothing (reads zero, writes dropped) and is gathered from / scattered to the heap's memory on upload, read-back and CPU writes, so placed allocations and the GPU see the same bytes. Reserved textures too (`kAllocReserved` with `kAllocTexture`, and `kAllocRenderTarget` or `kAllocStorageImage`): a 2D texture, array, cube or volume of any colour or block-compressed format (sampled only), any size and levels, one sample, on Direct3D's standard 64 KiB tile shapes for the bytes of its elements - texels, or 4 x 4 blocks - in the heaps (`tile_shape`: 8 bits 256 x 256, 16 bits 256 x 128, 32 bits 128 x 128, 64 bits 128 x 64, 128 bits 64 x 64; a volume 64 x 32 x 32, 32 x 32 x 32, 32 x 32 x 16, 32 x 16 x 16, 16 x 16 x 16). No reserved depth buffer or multisampled image: those have no backing the tiles could be gathered into. `AllocationDesc::tile_narrow` (a `NarrowLayout`, the optional private-data word `kAwTileNarrow`) says the heaps hold Direct3D's R8 / R8G8 / R16 / R16G16 texels, which the image keeps widened as every narrow format: gathering widens them to (r, g or 0, 0, maximum), scattering keeps the first channels. `ReservedLayout` (VidMm.hpp) is the numbering, Direct3D's: each slice has the tiles of its standard levels (at least a tile in every direction, in texels) level after level, a level's tiles in x, then y, then z order, then its packed levels as one run of elements (a volume's level slice after slice) over as many tiles as they fill; the slices follow one another; `update_tile_mappings` takes those numbers; a tile in a heap is its rows one after the other (the layout is ours, not a GPU's swizzle), or - `kTileStandardSwizzle` (bit 8 of `tile_narrow`: Direct3D's `64KB_STANDARD_SWIZZLE`) - its elements where Direct3D's standard swizzle puts them (`ReservedLayout::swizzle_x/y/z`, the patterns of src/Windows/tcos_swizzle.h; the packed levels stay rows); the conformance scene draws one mapped to two placed textures. Only the tiles the device or the CPU changed are scattered back (`TileBinding::changed`, found against the heaps
before any is written), so tiles that alias one place of a heap (Direct3D's `REUSE_SINGLE_TILE`, `CopyTileMappings`) show a write through any of them. A heap knows the reserved allocations with tiles in it (`tile_users_`): a write into it makes them stale, and their GPU writes come back before another one gathers from it. What the GPU draws or stores into a reserved texture goes to its mapped tiles when it is read back (`settle_tiles`); what fell on unmapped tiles is dropped - the backing is gathered again and the image uploaded before its next use, so it reads as zero (Direct3D's tier 2). A remapping reads the GPU's writes back first. The device image is told which tiles are mapped (`graphics::ImageResidency`, `IImage::set_residency`: one unit per tile of the standard levels, one per slice for the packed ones); on a device with `kFeature2TileResidency` (the CPU renderer) the shaders' residency feedback counts the texels each lookup reads in unmapped units. The stream op `CopyTiles` (48: direction, reserved, first tile, count, buffer, offset) moves the 64 KiB a tile holds in its heap to or from a buffer (an unmapped tile gives zeros and drops what it is given) - Direct3D's `CopyTiles`.

Residency feedback in shaders (Direct3D's tiled resources tier 2): the DXBC decoder takes the `_s` forms of the lookups - gather4_s 0xDB, gather4_c_s 0xDC, gather4_po_s 0xDD, gather4_po_c_s 0xDE, ld_s 0xDF, ld2dms_s 0xE0, sample_l_s 0xE4, sample_c_lz_s 0xE5, sample_cl_s 0xE6, sample_b_cl_s 0xE7, sample_d_cl_s 0xE8, sample_c_cl_s 0xE9 (the status destination after the first operand, the clamp - the finest level of detail - last for the `_cl_` ones) - as the plain lookup with `Instruction::feedback`, `status` and `clamp`; the interpreter brackets the lookup with `ITextureSet::begin_feedback(clamp)` / `end_feedback()` and writes the status (1: every texel read was mapped). `check_access_fully_mapped` (0xEA) is an integer compare with 0. The DXIL translator writes the same forms for `Sample` / `SampleBias` / `SampleGrad` / `SampleCmp` with a clamp argument and for any lookup whose ResRet status (element 4) is read, and `CheckAccessFullyMapped` (dx.op 71). A program with them needs `kFeature2TileResidency` (`required_features2`); the TGSI and Direct3D 9 token emitters refuse them, so virgl has none (Direct3D 12 tier 1 there). `ld_uav_typed_s` (0xE1), `ld_raw_s` (0xE2) and `ld_structured_s` (0xE3) are refused: the storage buffers and images have no residency here. Not done: tiled depth and multisampled textures, command lists / bundles / `ExecuteIndirect` as such (a stream is the command list), a root signature checked against the pipeline, more than one constant buffer, register spaces other than 0, descriptor heaps with copy-on-use semantics for `D3D12_DESCRIPTOR_RANGE_FLAG_*`, the escape ABI for the new objects (they are `Kernel` methods; the user-mode DLLs do not exist yet).

### Direct3D 12: resource heaps (placed resources)

A heap is an allocation made with `kAllocHeap` alone (a multiple of 256 bytes; system memory only, never resident). An allocation made with `AllocationDesc::heap` (the heap's handle, same device) and `heap_offset` (a multiple of 16) is **placed**: its system-memory backing is that part of the heap's memory, not a copy and not charged against the system limit. Placed allocations may overlap, so a lock through any of them sees what was written through another. Buffers and colour images of one sample can be placed (depth and multisampled images have no backing to share: `kNotSupported`; `kAllocShared` is refused). The heap's memory stays while any allocation placed in it exists, whatever happens to the heap's handle.

The device objects are still one for each allocation, so the GPU's view has to be carried across by hand, as Direct3D 12 does with **aliasing barriers**: `ResourceBarrier` of type 2 names the allocation going out of use (`allocation_index`) and, in the "before" word, the one coming into use (either `kNoAllocation`: nothing). In the runner it runs what was recorded so far, writes what the GPU holds for the outgoing allocation to the heap's memory (`VidMm::switch_alias`: `read_back`, with the same format conversions as a lock), and marks every allocation that shares bytes with it stale so that its next use uploads from the memory (reinterpreting the bytes, as aliasing does). The CPU writing through a lock does the same marking without a barrier. Not inside a pass. Without the barrier the incoming allocation keeps what its device object had. Placed render targets and textures, buffers and storage buffers all go through the same code; the conformance scene draws a triangle into a placed render target, passes the use to a placed texture at the same offset and samples it (matches on virgl). Not done: depth and multisampled placed images, a check that only one of the aliases is in use at a time, heap types (all heaps are CPU-visible), writing an aliased region from two device objects without a barrier is undefined.

### Direct3D 12: queries and predication (d3d12.dll extras.inc)

Query heaps are `kAllocQueryResult` buffers of `Count` results: `OCCLUSION` (8 bytes; `OCCLUSION` and `BINARY_OCCLUSION`, kinds 0 and 1), `TIMESTAMP` and `COPY_QUEUE_TIMESTAMP`
(8; kind 6, `EndQuery` only; `CopyQueueTimestampQueriesSupported`), `PIPELINE_STATISTICS` (88; kind 11), `PIPELINE_STATISTICS1` (112; kind 12: `MeshShaderPipelineStatsSupported` is
FALSE, there are no mesh shaders, and their three counts are 0), `SO_STATISTICS` (16; `SO_STATISTICS_STREAM0` .. 3, kind 13 with the stream) and `VIDEO_DECODE_STATISTICS`
(video.inc). A heap of a kind the adapter lacks (`kFeatureTimestampQueries`, `kFeatureStatisticsQueries`, `kFeatureStreamOutput`) is `DXGI_ERROR_UNSUPPORTED`, an unknown
type or no queries `E_INVALIDARG`. As the runtime does, `BeginQuery` / `EndQuery` ignore a type the heap does not hold, an index past it, a list type that does not run the
query (a copy list runs only copy-queue timestamps, a compute list timestamps and pipeline statistics), a query begun twice, an `End` of one not begun (or of another type)
and a begun timestamp. Up to 64 queries are open at once in a list, across passes, clears, copies, chunks and `ExecuteIndirect`: a list's stream is cut into chunks, and a
query open at the end of one is ended there and begun again with `kQueryAccumulate` in the next (`queries_suspend` / `queries_resume`); `Close` ends what is still open.
`ResolveQueryData` is a `CopyBuffer` of the heap's results: its type must be the heap's, the queries inside it, the offset a multiple of 8 and the range inside the buffer
(else nothing is done). **Predication** (`SetPredication`: a 64-bit value at a multiple of 8 in a buffer, `EQUAL_ZERO` / `NOT_EQUAL_ZERO`; another offset or operation is
ignored) skips whole chunks, read when the chunk runs; what Direct3D 12 does not predicate - `BeginQuery`, `EndQuery`, `ResolveQueryData`, aliasing barriers and
`WriteBufferImmediate` - goes into a chunk of its own without the predicate while predication is in force, so a query's counts and its resolve happen while the draws
around it are skipped.

### Direct3D 12: DXIL (Shader Model 6)

`ShaderKind::Dxbc` takes a DXIL container too (`compile_dxbc` hands it to `D3d9Program::compile_dxil`, `DxilTranslate.cpp`; the reader of LLVM bitcode is `DxilBitcode.cpp`). The container is a DXBC one with a `DXIL` part (a program header and LLVM 3.7 bitcode) beside the signature parts `ISG1` / `OSG1`. The bitcode is read (bitstream with abbreviations and BLOCKINFO; types, constants, global variables, the entry function's instructions, the metadata that names the entry point, its resources and its thread group) and **translated to the Shader Model 5 tokens of the same shader**, which `compile_dxbc` then takes: so the interpreter, the TGSI emitter, the D3D9 token writer and every device path built for DXBC run DXIL shaders unchanged.

- **Values**: every scalar of the function is a component of a temporary register (a linear scan reuses them after the last use; a structured walk numbers the "events" twice, once to find the last use of every value and once to emit); inputs are read where they are (`v#.c`), constants are immediates, constant buffer elements are read as `cb#[r].c`, a `ResRet` (sample, buffer load) is a whole register whose components `extractvalue` names.
- **Control flow** is made structured again from the acyclic CFG dxc writes: a branch is `if_nz` / `else` / `endif` around the arms up to their join (the lowest block both reach), a `phi` is a register the arms write on the edge to it (a swap of two phis uses scratch registers), `discard` is `discard_nz`. Loops (a back edge from the last block to the first of a run) are `loop` / `endloop` with `break`: the rotated form (the latch branches back or out), a conditional break anywhere in the body, an unconditional one; what is needed from before the loop lives to its end. Not taken: a `continue` that is not the latch's back edge, a return inside a loop, a break that does not go to one block, nesting deeper than four loops or 24 regions, `switch`.
- **Instructions** (`dx.op.*` by opcode): `loadInput` / `storeOutput` (by the signature element and row / column), the unary math of the interpreter (`FAbs`, `Saturate`, `Sin`, `Cos`, `Exp`, `Frc`, `Log`, `Sqrt`, `Rsqrt`, the four roundings), min / max (float, signed, unsigned), `FMad` / `IMad` / `UMad`, `Dot2` / `Dot3` / `Dot4` (a multiplication and multiply-adds), `createHandle` (resource and sampler registers; no arrays indexed by a variable), `cbufferLoadLegacy`, `sample`, `sampleLevel`, `textureLoad` (offsets must be zero), `discard`; and the LLVM ones: float and 32-bit integer arithmetic, shifts, bitwise, comparisons (floats as ordered / unordered alike), `select`, the casts between int and float and the free ones. Compute shaders (`cs_6_0`): `threadId`, `groupId`, `threadIdInGroup`, `flattenedThreadIdInGroup`, `bufferLoad` / `bufferStore` of raw and structured UAV buffers, `atomicBinOp`, `barrier`, and group-shared memory (a global variable in address space 3 with `getelementptr`, `load` and `store`, also as a constant expression), declared as `dcl_tgsm_raw`.
- **More stages and instructions**: geometry shaders (one stream; the vertex index of an input must be constant, `[unroll]`), hull shaders (the control-point and the fork phase, tessellation factors) and domain shaders translate to `gs_*` / `hs_*` / `ds_*` tokens; `switch` (as a chain of tests), an early `ret` outside a loop, `tan` and the hyperbolic functions, `IsNaN` / `IsInf` / `IsFinite`, signed division and remainder. Conformance scenes draw DXIL geometry, and hull plus domain shaders, on virgl.
- **Also**: `atan` / `asin` / `acos` (polynomials, within 1e-4), gather4 on 2D, array and cube textures (a cube tap off the face is the texel of the neighbouring face that the extended plane leads to, as with seamless cube maps; the footprint at a corner, with both taps off, is not defined and not tested; virgl's samplers set the seamless bit) (`kTexGather`: the four texels of the footprint as filtered lookups at their centres on the CPU, TGSI `TG4`; DXIL `textureGather`), derivatives (`ddx` / `ddy`, fine and coarse: the rasterizer gives each pixel a function for the varyings and position of the corners of its 2 x 2 quad, `FragmentContext::quad_fn`, and the interpreter runs the shader up to the derivative for the other corners of the pair; TGSI `DDX_FINE` / `DDY_FINE` / `DDX` / `DDY`, which match the GPU for the fine ones), buffer shader resource views (`t#` of a raw or structured buffer are read-only storage slots after the u# ones: `kMaxUavBuffers` + n; root SRV and descriptor table SRV of a buffer lower to them; compute shaders only), `sample_d` and `sample_b` of 2D textures (and CalculateLevelOfDetail / the lod instruction: x clamped, y not; TGSI `LODQ`, with the host only taking a two-component destination) (the level is log2 of the longer gradient in texels plus the bias, then a lookup at that level; sample_b takes the gradients from the quad; TGSI `TXD` / `TXB`), texture arrays indexed by a variable (up to the device's texture units, 2: the lookup is repeated for each element behind a test of the index), `sample_c` / `sample_c_lz` (`SampleCmp`) of 2D textures, arrays and cubes, with static offsets: the sampler's comparison on the red channel, filtered as its filter says (above, "Depth buffers read as textures"), done in TGSI as a gather, the passes and a hand-made blend, so the texture is any texture whose red is the depth and the host needs no comparison sampler, double precision (SM5 `dadd` / `dmul` / `ddiv` / `dfma` / `drcp` / `dmin` / `dmax` / `dmov` / `dmovc`, the comparisons, `dtof` / `ftod` / `dtoi` / `dtou` / `itod` / `utod`: a double is a pair of components, the low word first, so the interpreter keeps a double in two of a register's floats' bits; the translator allocates aligned pairs for doubles; fma is not fused on the CPU; TGSI `DADD` and the rest on the GPU; DXIL: fadd / fsub / fmul / fdiv / fcmp / select / phi / fpext / fptrunc / the integer conversions on `double`, `Fma`, `MakeDouble`, `SplitDouble`, doubles from a constant buffer), 64-bit integers (a pair of words like a double; add, sub and the comparisons from carries and borrows made with unsigned compares, mul from the high word of `umul` (the instruction now gives it) and cross products, shifts by two word shifts and a select, unsigned and signed division by a 64-step shift-and-subtract loop, widening and the conversions to and from float / double), early returns, also from inside a loop (a `ret` in the middle of the program is a real instruction now: `kRetEarly`, `ret` in the DXBC front end, TGSI `RET` after the outputs are written; the translator inlines the block that returns - dxc merges the returns into one block, sometimes numbered before the loop - at each jump to it, with the moves into its phis), half floats and 16-bit integers (done in 32 bits: a half is rounded to a half after each operation with `f32tof16` / `f16tof32`, which the TGSI makes of integer instructions because the host's PK2H / UP2H do nothing; a 16-bit integer is kept zero-extended and wrapped after each operation, sign-extended for the signed operations and tests; 16-bit elements of a constant buffer are cut out of their words; fpext / fptrunc, the integer conversions and the bitcasts between a half and its bits), buffer views in vertex, geometry, hull, domain and pixel shaders too (`kFeatureStorageInDraws`: the read-only slots after the u# ones may be bound inside a pass and between draws; the draw is refused when a view its shaders read is not bound; the contexts of the shader functions carry the views; on virgl the TGSI of such a shader numbers its `BUFFER`s from 0 - the host takes the ones of a compute shader's slots otherwise - and the views are bound to the stages the pipeline has before the draw; the hull shader's declarations carry them for both its functions), pixel shaders that write buffers (`kFeatureFragmentStores`: `RWStructuredBuffer` / `RWByteAddressBuffer` stores and atomics in a pixel shader; u0 .. u3 may be bound between draws; the shader runs for every fragment the rasterizer makes, also those the depth test then fails, and not for the helper pixels of the level of detail and derivatives; the draw leaves the buffers marked as written by the device; on virgl the TGSI numbers the views from 0 and the unordered access buffers after them, declares every number up to the last, and a memory barrier follows the draw), gather4 with constant offsets (-8 .. 7 texels, on 2D textures and arrays: in the extended opcode token; TGSI's texture offset operand) gather4_po (the offset in a register; the host's texture offsets are literals, so TGSI moves the coordinate to the middle of the footprint it wants - (floor(u * size - 0.5) + offset + 1) / size - and gathers there; the shader of the scene is assembled by the host test, which also writes `tests/data/dxbc/gather_po.dxbc` when `GEN_GATHER_PO` names a path) and gather4_c (`GatherCmp`: the sampler's comparison for each of the four, made in TGSI from the gather and the comparison constants), and vertex shaders that write buffers too (`kFeatureVertexStores`: the same bindings, a barrier after the draw; a vertex shader runs once for each vertex a draw fetches - a vertex an index repeats runs again on the CPU renderer, and the hardware may reuse its result - so the writes of a vertex shader belong at places its vertex number names; geometry shaders write too (`kFeatureGeometryStores`; a geometry shader runs once for each primitive and instance, the same on the CPU renderer and the hardware); hull and domain shaders write too (`kFeatureTessellationStores`: the control point function runs once per output control point and the patch constant function once per patch, as on the hardware; a domain shader that writes runs once per tessellator point on the CPU renderer - its vertices are kept for the triangles that share the point - the way hardware shades each point once)), and geometry shaders with several output streams (`dcl_stream`, `emit_stream`, an OSG5 signature).
- **Not taken** (kNotSupported, or kInvalidParameter for bitcode that is not well formed): mesh and library targets, a cube gather whose footprint leaves the face (the taps are kept on it; the hardware goes on to the next face), wave operations on a device without the lane model (below, "Shader Model 6 wave operations"), comparison sampling of anything but 2D textures with the one assumed comparison (below), alloca / memory other than group-shared, a program that needs more than 256 temporaries or 65536 instructions in its translation.
- **Tests**: dxc (the library of an installed game, driven by `scripts/dxc_compile.c`; `-Vd`, since its validator rejects the bitcode this compiler writes for typed texture loads) and the Wine d3dcompiler make the same HLSL (`dxil_*.hlsl`) into `.dxil` and `.dxbc`; the unit test runs both programs on 200 random inputs each (pixel shaders: the colour and whether it was killed; the vertex shader: position and varyings; the compute shaders: the buffers after the whole dispatch) and compares; the containers are truncated and have every bit flipped under the sanitizers (`DXIL_FUZZ_STRIDE=1` flips every bit; the default is every 37th). The conformance scenes draw DXIL pixel shaders (arithmetic, branches and a discard, loops) and run the two DXIL compute shaders on the real virglrenderer and match the CPU renderer.

### Direct3D 12 on the 3D hardware

`d3d12.dll` (`src/Windows/d3d12`) draws on whatever device the adapter has, and offers feature level 11_0 when the device takes sixteen vertex inputs and texture units and
eight render targets. The CPU renderer always did. virgl does now: eight colour targets, eight vertex buffer slots, 8-bit images in either byte order, mipmapped textures, and
the vertex formats and address modes below. `tests/d3d12_qemu.sh` boots the kernel in QEMU with `virtio-gpu-gl` (no window: `egl-headless`), starts
`tests/windows/d3d12test.c` and passes when every check does and the program says it drew on the hardware; `GPU=std` runs the same boot on the CPU renderer, and
`tests/emulated_dx.sh` the same program on the host emulator. The other GPU backends (SVGA3D, Intel Gen9, Vega, V3D) still report the base limits, so Direct3D 12 programs
that ask for 11_0 get no device on them.

What the device interface gained for it (each a `DeviceFeature`, so a device without it is refused or given the nearest thing, never handed something it would misread):

- **Vertex formats** (`kFeatureVertexFormats`, `graphics::VertexFormat`): normalised and integer bytes and 16-bit numbers, half floats, 32-bit integers (0 .. 47: type * 4 +
  components - 1), `Bgra8Unorm` = 48, `Rgb10A2Unorm` = 49, `Rgb10A2Uint` = 50 (the four fields as integers), `Rg11B10Float` = 51 (unsigned 11/11/10-bit floats, w 1) and
  `Bgrx8Unorm` = 52 (the fourth byte not read, w 1); `kVertexFormatCount` = 53. Normalised values reach the shader as floats (the most negative signed value -1), integers as
  integers (their bits in the register), missing components as 0, 0, 0, 1 (the integer 1). The CPU renderer and the Vega device convert as they read a vertex
  (`read_vertex_attribute`, VertexFetch.hpp); virgl names the host's vertex element format, except for what OpenGL cannot fetch as Direct3D reads it: `Rgb10A2Uint` goes to the
  host as the R32_UINT word and `Bgrx8Unorm` as B8G8R8A8, and the pipeline binds a copy of the vertex shader's TGSI that unpacks the fields or sets w to 1 in a temporary before
  its first instruction (VirglVertexPatch.hpp). The pipeline escape's format word is the `VertexFormat`.
- **Address modes** (`kFeatureAddressModes`): mirror, border and mirror once beside wrap and clamp, with one of three border colours (transparent black, opaque black,
  opaque white: what a static sampler can have). In the stream and in a descriptor the mode is the low byte of the address word and the border colour bits 8-9 of the
  word of U (`address_word`, DxgTypes.hpp). A device without the feature gets wrap for the mirrored mode and clamp for the others (`base_address_mode`).
- **Vertex buffer slots and targets on virgl**: the host keeps the instance divisor with a buffer binding, and sets binding n's divisor from vertex element n
  (`glVertexBindingDivisor(n, ...)` for every element n), so element n reads binding m, the first element with the same slot and divisor, whose divisor element m set; binding
  n is bound to element n's slot whether it is read or not. The elements are laid out by the shader's input number. The divisor is the step rate; a step rate of 0 (every
  instance reads the first instance's element) is the largest divisor, as OpenGL's divisor 0 is per vertex.

Things the host taught: a buffer may have only one bind flag when it is made, though the buffer object can then be bound as anything (a buffer of several uses is made
as a vertex buffer); an indexed draw's first index is ignored (the host draws from the index buffer's offset, so the first index is part of the offset); mip levels are
transferred one at a time with the level in the transfer, and the sampler view names the last level.

In the DLL alone: passes with a depth buffer and no render target (the kernel's passes have a colour target, so the depth buffer gets a colour companion of its size that
nothing reads; a device whose passes need none would save that memory), a depth clear that no pass takes (a pass of its own when the list is closed), multisampled render
targets and depth buffers with `ResolveSubresource`.

**Shared handles** (`device.inc`, `resource.inc` ShareDesc, win32k `DxgShares.cpp`): a committed resource or heap made `D3D12_HEAP_FLAG_SHARED` (GPU memory) or a fence
made `D3D12_FENCE_FLAG_SHARED` gets NT handles from `CreateSharedHandle` (`GENERIC_ALL`, a name in the session's `\BaseNamedObjects` as kernel32 names objects,
`DXGI_ERROR_NAME_ALREADY_EXISTS` for a taken one); `OpenSharedHandle` makes a resource (its description, heap properties and flags travel in the share's runtime data,
with up to 64 bytes for the runtime above: d3d11.dll's texture description through `TcosD3D12ShareExtra` / `TcosD3D12SharedExtra`), a heap (placed resources in it on
both sides alias the same memory) or a fence on any device of the adapter, in this process or another; `OpenSharedHandleByName` finds the handle. d3d11.dll shares
`D3D11_RESOURCE_MISC_SHARED_NTHANDLE` textures with `IDXGIResource1::CreateSharedHandle` and opens them, and Direct3D 12's, with `OpenSharedResource1` /
`OpenSharedResourceByName`. `OpenSharedHandle` also opens the legacy (global share) handles d3d11.dll's `GetSharedHandle` gives (resources made with the private
option `TCOS_RESOURCE_GLOBAL_SHARE`; `TcosD3D12OpenShared` takes one kind only). d3d11.dll's `ID3D11Fence` is a Direct3D 12 fence of its device, so its handles and
Direct3D 12's open in either runtime (`ID3D11Device5::OpenSharedFence`). Tested by tests/windows/d3dsharetest.c (two devices on the emulator; `ARGS=processes tests/d3d12_qemu.sh d3dsharetest` also a second process
that inherits the handles).

**The later interfaces** (`ID3D12Device1` .. `ID3D12Device14`, `ID3D12GraphicsCommandList1` .. `10`, `ID3D12Fence1`, `ID3D12Resource2`, `ID3D12Heap1`; every vtable slot
has its method, `scripts/gen-dx-vtables.py` names the latest interface of each object; what mingw's headers lack - root signature 1.2, `D3D12_SAMPLER_DESC2`, the
later stream subobjects, `OPTIONS19` .. `21`, the GPU upload heap, `ID3D12Device11` .. `14` and `ID3D12GraphicsCommandList8` .. `10` - is in
`src/Windows/d3d12/d3d12_types.h`, laid out as Microsoft's DirectX-Headers have it, which the generator and the test programs read too). What each part does:

- `device1.inc`: the `*1`/`*2`/`*3` resource and heap creation (`D3D12_RESOURCE_DESC1`, initial layouts as states; protected sessions and castable formats refused with
  `E_INVALIDARG`), `GetResourceAllocationInfo1/2`, `GetCopyableFootprints1`, `CreateCommandList1` (a closed list), `CreateCommandQueue1`, residency
  (`SetResidencyPriority`, `EnqueueMakeResident` signals its fence), `SetEventOnMultipleFenceCompletion` (any or all, on a thread of its own), `RemoveDevice` (every fence
  of the device signalled with `UINT64_MAX`), lifetime trackers, pipeline state streams (`CreatePipelineState`, `stream_parse`: the CD3DX12 defaults, one subobject of
  each kind - `DEPTH_STENCIL`/`1`/`2`, the three rasterizers, `ROOT_SIGNATURE` and `SERIALIZED_ROOT_SIGNATURE` are each one kind -; `DEPTH_STENCIL2`'s front and back
  masks are the pipeline's two pairs where the device has `kFeature2RasterizerState` (`IndependentFrontAndBackStencilRefMaskSupported`), and must match with the stencil test
  on where it has not; `RASTERIZER1/2`'s float depth bias is the pipeline's (a float, not rounded; `PsoExtras` carries both past the description); `RASTERIZER2`'s line mode is the multisample / antialiased-line pair it replaces, narrow quadrilateral lines `E_INVALIDARG`
  (`NarrowQuadrilateralLinesSupported` FALSE); `SERIALIZED_ROOT_SIGNATURE` makes the root signature, which the pipeline keeps; `PSDB` and later types `E_INVALIDARG`;
  `D3D12_PIPELINE_STATE_FLAG_DYNAMIC_DEPTH_BIAS` a bias the list sets (`RSSetDepthBias`) where the device has the rasterizer state, `E_INVALIDARG` elsewhere and
  `_DYNAMIC_INDEX_BUFFER_STRIP_CUT` everywhere, `OPTIONS16` / `15` saying so),
  `CheckDriverMatchingIdentifier`, and the later fence/resource/heap methods. `ID3D12Device11::CreateSampler2` (an integer border colour is the nearest of the three
  colours, as a float one; coordinates in texels - `NonNormalizedCoordinateSamplersSupported` FALSE - and unknown flags leave the call out),
  `ID3D12Device12::GetResourceAllocationInfo3` (castable formats: a size of ~0, `RelaxedFormatCastingSupported` FALSE), `ID3D12Device13::OpenExistingHeapFromAddress1`
  (`DXGI_ERROR_UNSUPPORTED`, as the other existing-heap calls), `ID3D12Device14::CreateRootSignatureFromSubobjectInLibrary`. The `*3`/`*2` creation calls take initial
  layouts as states (as `Barrier` needs none). GPU upload heaps (`D3D12_HEAP_TYPE_GPU_UPLOAD`, `OPTIONS16.GPUUploadHeapSupported`): buffers and textures the CPU maps and
  the GPU writes, kept as a custom heap the CPU writes back is (the CPU's writes go up before a list runs, the GPU's come back after); `GetCustomHeapProperties` describes the
  type as Windows does (write-combined, L1). What the device does not have answers what Direct3D 12 answers for it: no meta commands are enumerated, state objects and
  protected sessions are `DXGI_ERROR_UNSUPPORTED`, the raytracing prebuild info is zeros.
- `library.inc`: pipeline libraries (`ID3D12PipelineLibrary1`, a `TCOSPLB1` blob of names, kinds and FNV fingerprints of the descriptions: a load with another description or
  a name not stored is `E_INVALIDARG`, a foreign blob `D3D12_ERROR_DRIVER_VERSION_MISMATCH`) and shader cache sessions (`ID3D12ShaderCacheSession`, in memory or in a
  `TCOSSCC1` file under `%LOCALAPPDATA%\D3DSCache`, `ShaderCacheControl` clearing the application's). A session is a view of a store of the process: sessions of one
  identifier, mode and file are one store (`CacheStore`), so what one stores the others find and a disk store's file has all of it; a second
  `CreateShaderCacheSession` with the description of a session open on the same device gives that session, on another device a session of the same store, and with
  another description (version, limits, flags) `DXGI_ERROR_ALREADY_EXISTS`. The store goes with its last session (and its file, when one asked with
  `SetDeleteOnDestroy`).
- `rootsig.inc`: the serializer (`D3D12SerializeRootSignature`, `D3D12SerializeVersionedRootSignature`) and deserializers (`D3D12CreateRootSignatureDeserializer`,
  `D3D12CreateVersionedRootSignatureDeserializer`, `D3D12CreateVersionedRootSignatureDeserializerFromSubobjectInLibrary`) of versions 1.0, 1.1 and 1.2 (version word 3:
  1.1 with static samplers of 56 bytes, `D3D12_SAMPLER_FLAGS` last). Raw blobs, DXBC `RTS0` parts, and the `GlobalRootSignature` / `LocalRootSignature` subobjects of a DXIL
  library's `RDAT` part (version 0x10: the string buffer, raw bytes and subobject table parts; `tests/data/dxbc/rootsig_lib.dxbc` is dxc's `lib_6_3` of
  `rootsig_lib.hlsl`), everything bounds-checked (truncation and bit-flip cases in d3d12test and DxgkrnlTest). Every version is read into the 1.2 form and each version's
  description made from it: 1.0 to 1.1 / 1.2 makes ranges and root descriptors volatile as Windows does, to 1.0 the flags are dropped, a 1.2 static sampler with flags has
  no 1.0 / 1.1 description (`E_INVALIDARG`). The serializer refuses a border colour outside the five, the integer ones before 1.2 or without
  `D3D12_SAMPLER_FLAG_UINT_BORDER_COLOR` (and the flag without them), unknown flags. The kernel (`D3d12.cpp parse_root_signature`) takes 1.2: the integer opaque black and
  white are the float ones' values; a sampler of coordinates in texels is refused (`NonNormalizedCoordinateSamplersSupported` is FALSE), so `CreateRootSignature` is
  `E_INVALIDARG` for it. `D3D12_FEATURE_ROOT_SIGNATURE` answers 1.2.
- `cmdlist1.inc`: atomic buffer copies, `WriteBufferImmediate` (through an upload buffer of the list's), `ResolveSubresourceRegion` (average, least, greatest into any
  subresource, with a rectangle and an offset: the whole image into a plain texture of its size in place; anything else through a texture of the source's size, copied
  straight into a plain texture or through a buffer of the list's into a level or layer; `DECOMPRESS` has nothing to do, the sampler feedback modes are left out -
  `SamplerFeedbackTier` NOT_SUPPORTED -, as is a region outside the source or the destination), render passes (tier 0: targets bound, beginning-access clears,
  ending-access resolves), the enhanced barriers (`ID3D12GraphicsCommandList7::Barrier`, `OPTIONS12.EnhancedBarriersSupported`: layouts and accesses need no transition;
  a placed resource whose access after is `NO_ACCESS` goes out of use as with an aliasing barrier - its bytes written to the heap's memory and the allocations sharing
  them marked to take them up), `ID3D12GraphicsCommandList8::OMSetFrontAndBackStencilRef` (one reference for both faces), `List9::RSSetDepthBias` /
  `IASetIndexBufferStripCutValue` and `List10::SetProgram` / `DispatchGraph` (features at "not supported": left out), and `ClearUnorderedAccessViewUint/Float` of buffer views (raw, structured and typed, each format's conversion). Depth bounds, sample positions,
  view instancing and variable-rate shading are below ("Direct3D 12's optional rasterizer features"); the calls of the features the device reports at the "not supported" tier
  (meta commands, raytracing, mesh shaders; and those four where the adapter lacks them: virgl) are calls a program may not make here.
- `resource.inc`: textures in custom heaps the CPU sees (`D3D12_CPU_PAGE_PROPERTY_WRITE_BACK` / `WRITE_COMBINE`, the driver's layout), `Map` with no pointer and
  `WriteToSubresource` / `ReadFromSubresource`, which lock the kernel's image of the subresource (a plane of a planar texture's slice, a slice of a packed YUV array; the
  texels of the narrow formats and planes widened into the kernel's pixels and narrowed back). `D3D12_TEXTURE_LAYOUT_64KB_STANDARD_SWIZZLE` (`StandardSwizzle64KBSupported`):
  2D and 3D textures of one sample, not depth, video or R32G32B32 formats, elements of 1 to 16 bytes; the kernel's image is what the GPU uses, and a CPU-visible one is
  mapped with a pointer in Direct3D's standard swizzle (src/Windows/tcos_swizzle.h, shared with d3d11.dll: 64 KiB tiles in rows of tiles, the bit patterns of
  `D3D12_TEXTURE_LAYOUT`'s documentation, every level in tiles of its own) - a copy made from the image at the first `Map` and put back at each `Unmap` that wrote.
- Conservative rasterization (`D3D12_CONSERVATIVE_RASTERIZATION_MODE_ON`): tier 1 where the kernel's device has `graphics::kFeature2ConservativeRaster` (the CPU renderer:
  every pixel whose square a triangle's area overlaps, by the separating-axis test, all its samples, attributes and depth of the plane at the centre, degenerate triangles
  culled), passed as the `CreatePipeline` cull word's `kCullConservative` bit (0x100); virgl has none (the pipeline state is `E_INVALIDARG` there, as the tier says).
- `OMSetBlendFactor` and the factors `BLEND_FACTOR`, `INV_BLEND_FACTOR`, `ALPHA_FACTOR`, `INV_ALPHA_FACTOR`: the kernel's `BlendFactor::Constant*` (the CPU renderer and
  virgl, `DeviceInfo::blend_constant`); a pipeline is made for each factor a draw uses, as for each stencil reference. d3d11 and d3d9 (`D3DRS_BLENDFACTOR`,
  `D3DBLEND_BOTHSRCALPHA`) pass theirs on.

### The fixed state of Direct3D 10 and 11: rasterizer, depth-stencil, blend, samplers, viewports

What `D3D11_RASTERIZER_DESC`, `D3D11_DEPTH_STENCIL_DESC`, `D3D11_BLEND_DESC1`, `D3D11_SAMPLER_DESC` and `RSSetViewports` (and their Direct3D 12 forms) say, as state of the
kernel's pipelines, samplers and streams. `graphics::DeviceFeature2` bits say which device has what (the CPU renderer all; virgl as noted):

- **`kFeature2RasterizerState`** (`GraphicsPipelineDesc::fill_mode`, `depth_bias`, `depth_clip`, `separate_back_stencil` / `back_stencil`):
  - *Depth bias* as Direct3D defines it: `bias = DepthBias * r + SlopeScaledDepthBias * max(|dz/dx|, |dz/dy|)`, held to `DepthBiasClamp` (above 0 at most, below 0 at
    least), the biased depth clamped to the viewport's range. `r` is the depth format's unit (`DepthBias::unorm_bits`): `1 / (2^n - 1)` for D16 and D24, and for the float
    formats `2^(e - 23)`, `e` the exponent of the triangle's largest depth (`depth_bias_unit`). d3d12.dll takes `n` from the PSO's `DSVFormat`; d3d11.dll passes the
    program's own depth format (its D16 is kept in a float image, its D32_FLOAT_S8X24 in D24S8: the unit is the format the program made). Triangles are biased (a wireframe
    triangle's edges too), lines and points not - as OpenGL's polygon offset. On virgl the host's depth buffers are 32-bit floats and its polygon offset counts in their
    unit, which is Direct3D's float unit: the float formats are `offset_units` / `offset_scale` / `offset_clamp` (`glPolygonOffsetClamp`); a normalised format's bias
    (whose constant part GL cannot express on a float buffer) is added by the pipeline's own fragment shader (`depth_bias_fragment_text`: the slope from `DDX` / `DDY` of the
    window depth at the start of the shader, `MOV_SAT` of the biased depth to `POSITION.z` wherever it ends). A pipeline whose fragment shader has storage images (a shader
    made for each draw's images) and a normalised bias is not made on virgl.
  - *Depth clip*: with `DepthClipEnable` primitives are clipped against `0 <= z <= w` (the CPU renderer now clips the far plane too, as GPUs do); without it they are not and
    every fragment's depth is clamped to the viewport's range (virgl `depth_clip` 0: `GL_DEPTH_CLAMP`). The CPU renderer clips against `w > 0` then.
  - *Wireframe*: a triangle that is not culled is drawn as its edges, lines one pixel wide with the triangle's depth bias and facing (virgl `fill_front` / `fill_back`
    `PIPE_POLYGON_MODE_LINE`). The edges a near-plane cut makes are drawn; the inner edges of the fan a cut polygon is split into are not.
  - *Two-sided stencil*: back faces use `back_stencil`'s comparison, operations and masks (`enable` and the reference are both faces'; lines and points are front faces).
    That is also what D3D12's `D3D12_DEPTH_STENCIL_DESC2` front and back masks need: d3d12.dll's `Pso::back_read_mask` / `back_write_mask`, which a pipeline stream's
    `DEPTH_STENCIL2` subobject sets (`device1.inc` `stream_parse`; `IndependentFrontAndBackStencilRefMaskSupported`, OPTIONS14, says so where the device has the bit, and masks
    that differ are refused - `E_INVALIDARG` - on one that has not). The reference is one for both faces (`OMSetFrontAndBackStencilRef` with two values is the debug layer's
    error there: the front one is used). Where the device has the bit the back faces' reference is their own too (`OMSetFrontAndBackStencilRef`: the stencil state's
    `reference` of `back_stencil`, the 16th word of the tagged part below; d3d12.dll makes a kernel pipeline for each pair of references, as it does for each one). virgl:
    the DSA object's second stencil state, and the second reference of `SET_STENCIL_REF`.
  - *Dynamic depth bias* (Direct3D 12's `D3D12_PIPELINE_STATE_FLAG_DYNAMIC_DEPTH_BIAS`, `RSSetDepthBias`; `DynamicDepthBiasSupported`, OPTIONS16, where the device has the bit):
    the command list's bias, clamp and slope in place of the pipeline's; d3d12.dll makes a kernel pipeline of each set of values a draw uses (`pso_pipeline`), as for the
    blend factor. The bias of a `RASTERIZER1/2` subobject is the float itself (`DepthBias::constant` is a float).
- **`kFeature2DualSourceBlend`**: `BlendFactor::Src1Color`, `InvSrc1Color`, `Src1Alpha`, `InvSrc1Alpha` (15 .. 18): the pixel shader's `SV_Target1` as a factor of target 0's blend,
  in a pipeline of one colour target (more is `kInvalidParameter`; D3D12 `E_INVALIDARG`). virgl when the host blends a target so (`max_dual_source_render_targets`).
- **`kFeature2LogicOps`** (`logic_op_enable`, `logic_op`, numbered as `D3D11_LOGIC_OP`): in place of blending, for every target, on the bits of the stored channels
  (`R8G8B8A8_UINT` and the other integer formats, which Direct3D defines it for, the 8-bit normalised ones' bytes and R16G16B16A16_UNORM's 16 bits as OpenGL does; float, sRGB,
  snorm and 10-bit targets are written as without one). `D3D11_FEATURE_DATA_D3D11_OPTIONS::OutputMergerLogicOp` and D3D12's say it. d3d11's `CreateBlendState1` refuses a
  logic operation with blending or with `IndependentBlendEnable` (the runtime's rules, which Wine's `test_logic_op` checks); d3d12 one whose targets differ in it.
- **`kFeature2SamplerState`** (`SamplerDesc::max_anisotropy`, `custom_border` / `border_rgba`): anisotropic filtering by the reference method of
  `EXT_texture_filter_anisotropic` (`N = min(ceil(Pmax / Pmin), MaxAnisotropy)` lookups along the longer axis of the footprint at the level of `Pmax / N`; the CPU renderer's axis is
  the change of the coordinate its lookups take - (u, v) of a 2D texture or an array, (u, v, w) of a volume, the direction of a cube or a cube array, whose footprint is measured on the
  face); border colours of any
  value (an integer texture's as its integers' bits: Direct3D 12's static `OPAQUE_BLACK_UINT` / `OPAQUE_WHITE_UINT`). The stream's `BindTexture` (its 15th to 20th words, after the view word and
  the view) and the descriptor escape's sampler entries (`kDescriptorSamplerExtras` in the kind word; six words after the entry, the comparison staying in the offset's low word)
  carry them; a device without the bit draws a descriptor's sampler without anisotropy and with the nearest of the three
  border colours every device has (a stream that names them is refused there). The levels of detail (`MipLODBias`, `MinLOD`, `MaxLOD`) go to virgl as they are (they were
  rounded to sixteenths). virgl's border colour is set as the bits it is (`glSamplerParameterIuiv`); **virglrenderer 1.3.0 leaves anisotropy out**: it sets
  `GL_TEXTURE_MAX_ANISOTROPY` on the texture, which the sampler objects it binds override, so virgl filters trilinearly whatever is asked (the quality of anisotropic
  filtering is the implementation's; the state is sent as it is).
- **`kFeature2Viewports`**: up to `kMaxViewports` (16) viewports and scissor rectangles (the stream's `SetViewport` / `SetScissor` packets take several, six and four words
  each; `ICommandBuffer::set_viewports` / `set_scissors`), a primitive drawn with the pair its first vertex's `SV_ViewportArrayIndex` (`ShaderIo::viewport`) names - the first
  when that is past the ones set. Geometry shaders write it (TGSI `VIEWPORT_INDEX`); virgl sets all sixteen of the host's (those past the ones given to the first, so that an
  index past them is drawn as on the CPU renderer) when the host has 16 (`max_viewports`) and geometry shaders.
- **`kFeature2LayerFromAnyStage`**: `SV_RenderTargetArrayIndex` and `SV_ViewportArrayIndex` written by the vertex or domain shader that feeds the rasterizer (Direct3D 11.3's
  `VPAndRTArrayIndexFromAnyShaderFeedingRasterizer`, D3D12's `...SupportedWithoutGSEmulation`): the CPU renderer. virgl has not got it - virglrenderer takes a vertex shader's
  layer only with `GL_ARB_shader_viewport_layer_array` and an evaluation shader's not at all - so such a shader is refused there (`kNotSupported`) and the options say FALSE.

### Strip cuts, filters, samplers of Direct3D 12, line modes, narrow formats (gap pass 4)

- **Strip cuts** (`SetTopology`'s optional second word, `StripCut`: 0 none, 1 0xFFFF, 2 0xFFFFFFFF - Direct3D 12's `D3D12_INDEX_BUFFER_STRIP_CUT_VALUE` -, 3 or no word the index
  buffer's largest index, as Direct3D 10 and 11 cut). The kernel expands strips and fans itself (`draw_expanded`), so every device draws them; an index that cuts names no
  vertex (the vertex-range check skips it). A fan is cut as a strip is (a new fan from the next index). d3d12.dll sends the pipeline's `IBStripCutValue` (also of a pipeline made
  with `D3D12_PIPELINE_STATE_FLAG_DYNAMIC_INDEX_BUFFER_STRIP_CUT`), which `SetPipelineState` sets and `IASetIndexBufferStripCutValue` replaces (bundles record it); OPTIONS15
  says `DynamicIndexBufferStripCutSupported` and `TriangleFanSupported` (`D3D_PRIMITIVE_TOPOLOGY_TRIANGLEFAN`, 6) on every device. d3d11.dll's pipelines are dynamic and it sets
  the cut of its index buffer's format at each indexed draw; d3d9.dll's are `DISABLED` (Direct3D 9 has no restart index).
- **`kFeature2SeparateFilters`** (`SamplerDesc::separate_mag` / `mag_filter`; the filter word's bits 1 and 2, `filter_word`): a magnification filter of its own - a lookup
  minifies where its level of detail, with the bias and the `MinLOD` / `MaxLOD` clamps, is above 0, else it magnifies - Direct3D's `MIN_POINT_MAG_LINEAR`,
  `MIN_LINEAR_MAG_POINT` .. . CPU renderer (`TextureSet::at_lod`; the derivatives are taken when the two filters differ even without levels) and virgl (the sampler state's
  `mag_img_filter`). A descriptor's filters on a device without the bit are the one filter, linear where either is (`lower_texture`); a stream that names them there is refused.
- **`kFeature2SamplerReductions`** (`SamplerDesc::reduction`, bits 4-5): Direct3D's `MINIMUM_*` / `MAXIMUM_*` filters - each channel's least or greatest value among the texels
  the filter gives a weight above 0, also across a linear mip filter's two levels, a volume's two slices and an anisotropic filter's lookups. The CPU renderer; virglrenderer's
  sampler state has no reduction mode, so virgl refuses them (`kNotSupported`) - d3d11's `MinMaxFiltering` (`D3D11_FEATURE_DATA_D3D11_OPTIONS1`) says so and its
  `CreateSamplerState` refuses those filters there with `E_INVALIDARG`; Direct3D 12 has no option for them (a draw with one is refused on virgl).
- **`kFeature2UnnormalizedCoordinates`** (`SamplerDesc::unnormalized`, bit 8): the coordinates of a 2D image (or array) are in texels of the view's first level, which every
  lookup reads - `D3D12_SAMPLER_FLAG_NON_NORMALIZED_COORDINATES` (OPTIONS17 `NonNormalizedCoordinateSamplersSupported`), with Direct3D 12's rules (min and mag filter alike, a
  point mip filter, no anisotropy or comparison, clamp or border in U and V, MinLOD and MaxLOD 0: d3d12.dll's `CreateSampler2` writes a null descriptor and the root
  signature parser refuses a static sampler that breaks them). The CPU renderer; virgl's sampler state has no `normalized_coords` (`kNotSupported`, the option FALSE).
- **`D3D12_SAMPLER_DESC2`** (`CreateSampler2`): every filter of `D3D12_FILTER`, also the anisotropic ones with a point mip filter (0x54, 0xD4, 0x154, 0x1D4:
  `AnisoFilterWithPointMipSupported`, OPTIONS19 - on a device without anisotropy they filter as `MIN_MAG_LINEAR_MIP_POINT`, as the others filter trilinearly there), a border
  colour of integers (`D3D12_SAMPLER_FLAG_UINT_BORDER_COLOR`: the four words as the bits of `border_rgba`, which an integer texture's channels are) and coordinates in texels.
  A value of `D3D12_FILTER` that is none, or flags that do not exist, make a null descriptor (the call is void). Static samplers of root signature 1.2 the same
  (`d3d12_filter`, `D3D12_STATIC_SAMPLER_DESC1::Flags`).
- **`kFeature2LineModes`** (`GraphicsPipelineDesc::line_mode`, `graphics::LineMode`; the escape's tagged part `kPipelineLineTag` 'LIN1', one word): aliased (the device's line,
  as before), alpha-antialiased (a rectangle one pixel wide between the two ends, not lengthened; each pixel it touches is shaded once at its centre with the attributes of the
  nearest point of the line, and the first target's alpha is multiplied by the part of the pixel the rectangle covers - measured on 8 x 8 points - before blending: Direct3D's
  `AntialiasedLineEnable`, which needs blending to show), quadrilateral 1.4 pixels wide (`MultisampleEnable`, `QUADRILATERAL_WIDE`) and 1.0 wide (`QUADRILATERAL_NARROW`,
  `NarrowQuadrilateralLinesSupported`): the rectangle as two triangles, its samples or pixel centres by the triangle rules. Wireframe triangles' edges are drawn in the
  pipeline's line mode. d3d12.dll takes the mode from `RASTERIZER2`'s `LineRasterizationMode`, or `MultisampleEnable` before `AntialiasedLineEnable` (D3D10 - 12's choice),
  d3d11.dll forwards its rasterizer state's two flags. CPU renderer; virgl: smooth lines (`line_smooth`, the host's `GL_LINE_SMOOTH`: its coverage, not this rectangle's, in the
  alpha) and multisampled lines of the width 1.4 or 1.0 - which the host draws as rectangles on a multisampled target; on a target of one sample it draws its own aliased line.
  A device without the bit makes no pipeline of another mode (`kNotSupported`); d3d12.dll draws lines aliased there and refuses narrow quadrilateral ones (`E_INVALIDARG`).
- **Narrow formats kept as they are**: d3d12.dll makes R8 / R8G8 / R16 / R16G16 textures - unorm and typeless - of the kernel's own `R8Unorm`, `R8G8Unorm`, `R16Unorm` and
  `R16G16Unorm` where the adapter has that format for all the texture is made for (sampled and filtered; drawn into and blended; storage; levels; samples: `g_native_narrow`),
  and widened into four channels as before elsewhere (and always for the planes of the video formats). A typeless one's views of the other kinds of its family (R8 / R8G8: snorm,
  uint, sint; R16 / R16G16 also float) are **mirrors** in the view's format: VidMm's mirrors now take the one- and two-channel 8- and 16-bit families (a mirror and its sources of
  one family: the same bytes) and several levels (`set_mirror` with a source for each subresource, levels first). A shader resource view is a mirror of its levels and layers
  (`typed_mirror`; at most 256 subresources), a render target view and an unordered access view a write-back mirror of its level and layers (the view's channels given back
  after each pass or dispatch). A view of another family, or of a typeless format, is a null descriptor. d3d11.dll passes the view's format for such textures (`view_format12`)
  and keeps an `R16_TYPELESS` colour texture typeless (it was R16_UNORM).
- **`R1_UNORM`**: a bit a pixel, the first pixel of a row the most significant bit of its first byte (as DirectXTex reads it), rows of `(width + 7) / 8` bytes in footprints. The
  kernel keeps it as bytes of 0 or 255 in red of an `R8Unorm` image (or widened into `A8B8G8R8`), and `CopyBufferImage` with `NarrowLayout::R1` (9) packs and unpacks the bits (a
  red of 128 or more is a 1; bits of the last byte past the width are left as they are). Direct3D 12: 1D and 2D textures, arrays and levels, sampled (as 0 and 1 in red) and
  loaded - no render target, unordered access, volume, cube, tiles, multisampling or CPU-visible memory (`E_INVALIDARG`; the format support says the same); a footprint box
  or place that does not start on a byte is not copied. Direct3D 11 the same (initial data, `UpdateSubresource`, copies; no staging or dynamic texture). Direct3D 10's
  `D3D10_FILTER_TEXT_1BIT` has no Direct3D 11 / 12 form, so nothing else filters it.

Tests: `DxgkrnlTest`'s `test_fixed_state_gaps` (the filter word and `d3d12_filter`, the strip cut word, the three line widths and an alpha-antialiased line, the 'LIN1' part and
a device without the bits, R1 copies into R8Unorm and A8B8G8R8 and back, mirrors of the narrow families and of two levels) and the conformance scenes "separate minification and
magnification filters", "minimum and maximum filters", "non-normalized sampler coordinates" and "index buffer strip cut" (pixels checked in `test_conformance`); d3d12test's
`test_fixed_state_gaps` (strip cuts of the pipeline, of the list, from a bundle, of 32-bit indices, a fan; the separate filters, reductions, anisotropy with a point mip filter,
an integer border colour and coordinates in texels through `CreateSampler2` in a compute shader, `cs_smp_lod.dxbc`; the three line modes and `RASTERIZER2`'s narrow lines; every
view of the four typeless families read with `Load`, typeless R8 / R16 drawn into as `R8_UINT` / `R16_UINT`; R1_UNORM footprints, copies, `Load` and `Sample`) and d3d11test's
`test_fixed_state_gaps` (strip cuts, separate filters, `MinMaxFiltering`, line modes, an R16_TYPELESS texture as R16_FLOAT, R1_UNORM).

The escape `CreatePipeline` carries the state in a tagged part after the constant blend colour (beside the step rates' 'STP1' and the line mode's 'LIN1', in any order): `kPipelineRasterTag` ('RST1'), `kPipelineRasterWords` (16), then fill mode,
depth clip, depth bias (constant, slope and clamp as float bits - `DepthBias::constant` is a float, so that `RASTERIZER1/2`'s float `DepthBias` goes as it is -, the format's bits), the back faces' stencil (separate, compare, fail, depth fail, pass, compare mask, write
mask), the logic operation (enable, op) and the back faces' stencil reference (with separate: Direct3D 12's `OMSetFrontAndBackStencilRef`). Direct3D 9 (d3d9.dll) uses it too: `D3DRS_FILLMODE` wireframe (`D3DFILL_POINT` is drawn solid: Direct3D 12 below has no point
fill), `D3DRS_DEPTHBIAS` / `D3DRS_SLOPESCALEDEPTHBIAS` (the bias a depth: in units of the device's D24 buffer), `D3DRS_TWOSIDEDSTENCILMODE` with the `D3DRS_CCW_STENCIL*`
operations (the counter-clockwise faces are the back ones), and the samplers' `D3DSAMP_MAXANISOTROPY`, `D3DSAMP_BORDERCOLOR`, `D3DSAMP_MIPMAPLODBIAS` and
`D3DSAMP_MAXMIPLEVEL`; its caps say `D3DPRASTERCAPS_DEPTHBIAS`, `SLOPESCALEDEPTHBIAS`, `ANISOTROPY`, `D3DSTENCILCAPS_TWOSIDED`, `MaxAnisotropy` 16 and the border and mirror
address modes.

Tests: the conformance scenes "depth bias, float format", "depth bias, normalised formats", "two-sided stencil", "wireframe", "depth clip and clamp", "dual-source blending",
"logic operations", "border colours" and "several viewports and SV_ViewportArrayIndex" (each with its pixels checked in `DxgkrnlTest`'s `test_conformance`, and all the same on
real virglrenderer 1.3.0); `test_rasterizer_state` (the refusals, a device without the bits, the escape's tagged part); `test_viewports_from_vertex_shader`
(`vs_viewport.dxbc`: the viewport and the layer from a vertex shader); d3d12test's `test_fixed_state`, `test_stencil_refs_and_dynamic_bias` (front and back references, the list's depth bias) and the pipeline-stream variants (`DEPTH_STENCIL2` masks, `RASTERIZER1/2`), and d3d11test's `test_fixed_state`, `test_viewports` and
`test_sampler_state` (Wine's `test_logic_op` table on an `R8G8B8A8_UINT` target, the depth formats' units, `MaxLOD` / `MinLOD` / `MipLODBias` of 0.3, anisotropy of 2 and 4
on the CPU renderer).

Not done, in the order a program is likely to meet them: views of a typeless R8 / R8G8 / R16 / R16G16 texture in another kind on a device without that narrow format (the texture is
kept widened there, its other views not made), typed views of such a volume's slices or of more than 256 subresources (a mirror is 2D or an array of at most that many); a depth buffer read
by shaders while it is also the pass's (read-only) depth buffer, depth buffers of several samples read as textures, SampleCmp of the levels of a mipmapped texture on virgl
(the view's first level is compared), a vertex and a pixel shader that read one unit as different texture and sampler pairs; CPU access to the samples of a multisampled image
(`read_depth_subresource` and the depth layouts are of one sample; multisampled images are copied whole, image to image, only); the layer and viewport from a vertex or domain shader on virgl,
anisotropic filtering on virgl (virglrenderer 1.3.0 drops it; the CPU renderer filters 2D textures, arrays, cubes, cube arrays and volumes anisotropically), the minimum and maximum filters and coordinates in texels on virgl (its sampler state has neither), virgl's
quadrilateral lines on a target of one sample (the host's aliased line) and its smooth lines' coverage (the host's); more than four unordered access buffers; heaps and textures shared across adapters (`D3D12_HEAP_FLAG_SHARED_CROSS_ADAPTER`: `DXGI_ERROR_UNSUPPORTED`,
`CrossAdapterRowMajorTextureSupported` FALSE: an allocation is of one adapter's memory manager, and sharing one needs a backing both adapters' memory managers keep coherent
at the fences - read back after one adapter's work, uploaded before the other's -, which `VidMm` does not have; fences shared across adapters are done, the kernel's
synchronization objects being of no adapter); the blend factor on Vega and the other fixed backends; ray tracing, mesh shaders, depth bounds, sample positions, variable rate
shading, view instancing and barycentrics on virgl (below), sampler feedback, work graphs, protected sessions and meta commands (reported unsupported - see below); a depth image's average, least and greatest resolves on virgl
(the host's blit takes sample 0, and its multisampled depth buffers are attachments a shader does not read); typed UAV clears of formats outside `g_uav_formats`
(R11G11B10, B5G6R5 ...); and all of it on the GPU backends other than virgl.

What the Shader Model 6 features would take (the DXIL front end, `DxilTranslate.cpp`, translates one entry function of vertex, pixel, geometry, hull, domain and compute
shaders into Shader Model 5 tokens): **ray tracing** - DXIL libraries of many entry functions and their `RDAT` (the root signature subobjects are read already),
state objects and shader tables, acceleration structures built on the device (a BVH in a buffer), and `TraceRay` / `ReportHit` / `CallShader` lowered into a compute
program with a traversal loop and the hit groups called as functions (inline ray queries, `RayQuery`, are the smaller first step); **mesh and amplification shaders** -
the two stages translated and run as compute dispatches whose output primitives feed an indirect draw; **sampler feedback** - `WriteSamplerFeedback` writing MinMip and
MipRegionUsed maps (opaque formats) from the sampler's footprint; **work graphs** - node shaders and a scheduler of records between dispatches.

### Direct3D 12's optional rasterizer features: depth bounds, sample positions, view instancing, variable-rate shading, barycentrics

What `OPTIONS2`, `OPTIONS3`, `OPTIONS6` and `OPTIONS10` report above their "not supported" tiers, done on the CPU renderer. Each is a `graphics::DeviceFeature2` bit of the device
(high bits, apart from the run the other features fill): `kFeature2SamplePositions` (1 << 25), `kFeature2ViewInstancing` (26), `kFeature2Barycentrics` (28),
`kFeature2SampleCopies` (29), `kFeature2DepthBounds` (30), `kFeature2ShadingRate` (31); d3d12.dll mirrors them as `DEV_FEATURE2_*` and reports a feature only where the adapter has it. virgl has
none of them: virglrenderer's protocol carries no depth bounds, sample locations or shading rate, the TGSI emitted here has no view id, barycentric or per-vertex pixel inputs,
and a copy of samples would be a host blit - its tiers stay "not supported" (with the QEMU backend moving to Venus, Vulkan's `depthBounds`, `VK_EXT_sample_locations`,
`VK_KHR_fragment_shading_rate`, multiview and `VK_KHR_fragment_shader_barycentric` are the way there).

The state is the command stream's, not the pipeline's: four ops numbered apart from the others (`Op::SetDepthBounds` 56, `SetSamplePositions` 57, `SetViewInstance` 58,
`SetShadingRate` 59; `CommandStream.hpp` has their words), valid inside and outside passes, kept for the stream across passes and command buffers (`StreamRunner::RasterState`,
given to the device's command buffer before the next draw, `apply_raster`); a stream starts with the defaults. The device interface has the matching `ICommandBuffer` calls
(`set_depth_bounds`, `set_sample_positions`, `set_view_instance`, `set_shading_rate` / `set_shading_rate_image`, `copy_samples`), recorded by `CommandRecorder`.

- **Depth bounds** (`OMSetDepthBounds`, `DEPTH_STENCIL1/2.DepthBoundsTestEnable`): a sample whose depth buffer value, as it was before the draw, is outside `min .. max`
  (inclusive) is not drawn - no shader result, depth or stencil write, no occlusion count; triangles, lines and points; a pass without a depth buffer passes everything. The
  enable is the PSO's, sent with the list's range before each draw that changes them (`send_raster_state`); a range outside 0 .. 1 or with `min > max` is a call left out.
- **Programmable sample positions** (`SetSamplePositions`, tier 2): 1, 2, 4, 8 or 16 positions in sixteenths of a pixel (-8 .. 7) for one pixel, or for each pixel of a 2 x 2
  quad aligned to even coordinates (at most 16 in all) - the triangles' coverage and the depth of each sample come from them; a single-sample pass covers a pixel by its one
  moved sample; positions of another count than the pass's are not used; `(0, 0, NULL)` the standard ones again. Not in bundles (they inherit the caller's). The CPU renderer
  keeps every sample's depth, so depth is never "compressed": the rules of the interplay with clears, copies and resolves hold by themselves. `ResolveSubresourceRegion` with
  `D3D12_RESOLVE_MODE_DECOMPRESS` is a copy of every sample of the region (colour, depth and stencil) into another resource of the format and sample count (`ResolveRegion` of
  mode `kResolveCopySamples` 16 and six words of region, `kFeature2SampleCopies`; one sample: an ordinary copy); in place it does nothing. Centroid interpolation is not done,
  so the positions' order (the centroid priority) has no effect.
- **View instancing** (`VIEW_INSTANCING` subobject, `SetViewInstanceMask`, `SV_ViewID`; tier 2): draw-call looping in d3d12.dll (`draw_views`): each draw is made once for each
  view of the PSO - with `ENABLE_VIEW_INSTANCE_MASKING` those of the list's mask (0 when reset; a bundle starts with 0 and its last mask carries back) -, each after a
  `SetViewInstance` of the view and its location; the kernel's device gives the view to every stage (`VertexContext` / `GeometryInputData` / `HullInputData` / `HullOutputData` /
  `FragmentContext::view_id`) and adds the location's viewport and render target array index to the primitive's own (32-bit wrapping; out of range: the first, as for the
  shaders' own). Every stage runs for every view and counts in the pipeline statistics, which tier 2 allows and tier 3 does not. At most 4 views; a location of viewport 16+
  or layer 2048+, more than 4 views, no locations or another flag is `E_INVALIDARG`. ExecuteIndirect's draws loop the same way.
- **Variable-rate shading** (tier 2, `AdditionalShadingRatesSupported`, 16 x 16 tiles, the SUM combiner): `RSSetShadingRate` (the rate and the two combiners),
  `RSSetShadingRateImage` (an R8_UINT texture of one level and sample, a rate a tile; past its edge 1x1), `SV_ShadingRate` written by a vertex or geometry shader (the
  primitive's first vertex's) and read by a pixel shader. The rate of a tile is `combiner1(combiner0(rate, primitive's), image's)` per axis (`combine_shading_rate`), made what the
  pass's samples allow (`sanitize_shading_rate`: with 2+ samples an axis at most 2x but 2x4 with 2, never more than 16 samples a coarse pixel, 4x1 / 1x4 the finer 2x1 / 1x2). The
  CPU renderer then shades each coarse pixel (on a grid from the target's origin, inside the tile) once at its centre - varyings, `SV_Position` and depth there, derivatives and
  the level of detail over the 2 x 2 quad of coarse pixels - and writes the colour to every covered sample of its pixels, depth and stencil tested per sample at full
  resolution; a discard drops the whole coarse pixel. Lines and points are shaded per pixel. The image is read when the list runs (`D3D12_RESOURCE_STATE_SHADING_RATE_SOURCE`
  is `state::kShadingRateSource`).
- **Barycentrics and the other SM 6.1+ pixel values** (the DXIL translator, `DxilTranslate.cpp`, with operand types no DXBC shader has, `0xF0` .. `0xF3`, read by `D3d9Dxbc.cpp`):
  `SV_ViewID` (`dx.op.viewID`, any graphics stage), `SV_Barycentrics` (a system value with no register, perspective-correct or - `noperspective` - linear in the screen: the
  rasterizer carries each vertex's weights through clipping and interpolates them like a varying, so a clipped triangle keeps the weights of the whole one; derivatives of them
  through `FragmentContext::quad_bary_fn`), `GetAttributeAtVertex` (`dx.op.attributeAtVertex`: a pixel shader's input as vertex 0, 1 or 2 of the primitive made it, in the draw's
  vertex order: `FragmentContext::vertex_varyings`), `SV_ShadingRate` (D3D_NAME 24 in and out), `IsHelperLane` (SM 6.6). Barycentrics, attributes at vertices and shading rates make the program
  need their feature bit, so a device without it is not given the shader (`kNotSupported`), and the TGSI emitter refuses them; `SV_ViewID` needs none - a device without view
  instancing draws every draw as view 0, as Direct3D 12 has a driver without it do (TGSI: the immediate 0), so such shaders run on virgl too.
- **Tests**: `DxgkrnlTest` (`test_depth_bounds_kernel`, `test_sample_positions_kernel` with the copies of samples, `test_view_instance_kernel`, `test_shading_rate_kernel`,
  `test_dxil_sm61_values`, `test_barycentrics_kernel`: pixels through the kernel's stream on the CPU renderer, refusals, a device without each feature; the DXIL fixtures
  `dxil_vs_view`, `dxil_ps_view`, `dxil_ps_bary`, `dxil_ps_attr`, `dxil_ps_rate`, `dxil_vs_rate`, `dxil_ps_helper` made with dxc, truncated and bit-flipped); `d3d12test`'s
  `test_raster_features` (the caps, and each feature drawn through d3d12.dll and read back: bands of depth, a resolved edge under three sample patterns and a DECOMPRESS, two
  layers of views with masks and a bundle, three rate sources, barycentric weights and vertex attributes). The emulator's fixture header now has the `.dxil` blobs too
  (`dx_<name>_dxil`, as `d3d12_qemu.sh` had).
- **Not done**: centroid and sample interpolation (and so `EvaluateAttributeSnapped` and per-sample shading at all), `SV_Coverage` in and out, tier 3 view instancing (the
  stages before the first that reads `SV_ViewID` are run for each view). The shader model the device reports: below.

### Shader Model 6 wave operations

What `HighestShaderModel` above 5.1 needed: the wave and quad intrinsics of Shader Model 6.0, on a device that runs the lanes of a wave together. Done on the CPU renderer
(`graphics::kFeature3WaveOps`, bit 16 of `DeviceFeature3`, and `kFeature3WaveMatch`, bit 17, for 6.5's `WaveMatch` / `WaveMultiPrefix*` and 6.7's `QuadAny` / `QuadAll`).

- **Translation** (`DxilTranslate.cpp` `Translator::wave`): `dx.op` 110 - 123, 135, 136 (6.0: `WaveIsFirstLane`, `WaveGetLaneIndex`, `WaveGetLaneCount`, `WaveActiveAnyTrue` /
  `AllTrue` / `Ballot` / `AllEqual`, `WaveReadLaneAt` / `ReadLaneFirst`, `WaveActiveSum` / `Product` / `Min` / `Max` / `BitAnd` / `BitOr` / `BitXor` / `CountBits`,
  `WavePrefixSum` / `Product` / `CountBits`, `QuadReadLaneAt`, `QuadReadAcrossX` / `Y` / `Diagonal`), 165 - 167 (6.5) and 222 (6.7), every overload the front end has the type
  of: 32-bit floats and integers (signed and unsigned by the operation's `sop`), doubles and 64-bit integers (a pair, as elsewhere), halves (done as the float they are kept in,
  rounded after) and 16-bit integers (in their 32 bits; sign-extended for a signed minimum or maximum, wrapped after). Each becomes a private DXBC opcode, `0x700` + its place
  in `D3d9Decoded.hpp`'s list (`kWaveIsFirstLane` ..), the type and the operation in the control bits, the destination, the sources and a last immediate, the site (the DXIL
  instruction's number); `D3d9Dxbc.cpp` makes the IR instructions of them, sets `required_features3` and marks the program `uses_wave()`. A return block the translator puts into
  each branch that returns (`inline_return`) is bracketed by `0x7F0 block` / `0x7F1` (`Instruction::tail`, `tail_at`), so the lane model takes its copies for one block.
  Also new for these fixtures: `rawBufferLoad` / `rawBufferStore` of 32-bit elements (6.2's form of the buffer accesses), the legacy `f32tof16` / `f16tof32`, 64-bit `imin` /
  `imax` / `umin` / `umax`.
- **The rest of 6.0 the DXBC front end has** (so that the model reported is the DXIL of everything the kernel takes as DXBC): typed unordered access views of textures (1D,
  2D, 3D, 1D and 2D arrays) and buffers (`RWTexture*`, `RWBuffer`: `dcl_uav_typed` with the return type of the view's element - the struct the resource variable points to,
  whose name gives an integer's sign; dxc's element-type tag says U32 for every typed view, so it is not used -, `TextureLoad` / `BufferLoad` as `ld_uav_typed`, `TextureStore`
  / `BufferStore` as `store_uav_typed`, `AtomicBinOp` with the coordinate), `Buffer<>` views (`dcl_resource_buffer`, `ld`), `Texture1D`, `Texture1DArray` and `Texture2DMS`
  views (`ld_ms` for the last), `GetDimensions` (`bufinfo`; `resinfo_uint`, and `sampleinfo_uint` for a multisampled texture's count), `AtomicCompareExchange` (raw,
  structured and typed: `imm_atomic_cmp_exch`), `BufferUpdateCounter` (`imm_atomic_alloc` / `imm_atomic_consume`), and `atomicrmw` / `cmpxchg` on group-shared memory (LLVM
  instructions the bitcode reader takes now: `Op::AtomicRmw`, `Op::CmpXchg`; sub as an add of the negated value; nand refused). An `if` that joins at the block that ends the
  function is written as `if` .. `endif` and the block once (it was a copy of the block in each way, so a compute shader had a `ret` in the middle, which TGSI has not: such
  DXIL compute shaders now run on virgl too). `d3d12test`'s `test_dxil_resources` runs `dxil_cs_typed` as DXIL (dxc) and as DXBC (Microsoft's compiler) on the same resources
  and compares every word they write.
- **The lane model** (`D3d9Shader.cpp`, "Waves (WaveRunner)"): a program with wave operations runs the lanes of a wave together. Each lane runs until it stops before a wave
  operation, a derivative (or a lookup whose level comes from them) or a loop's end; then the stopped lane with the lowest instruction goes on with every lane stopped at that
  instruction (in a copy of a return block: at that instruction of any copy, after every lane outside the copies). The flow control is structured, so this is the order a SIMD
  machine with a mask of the lanes taking each branch comes to the instructions, and lanes that leave a loop wait after it. Lanes that end or return are inactive; a discarded
  pixel goes on as a helper (`IsHelperLane` true). The active lanes of an operation are the lanes at it but the helpers; the quad operations and derivatives read helpers too; a
  lane read that is not there is 0. Reductions are taken in lane order, so the results do not depend on the machine.
  - A **pixel shader**'s wave is the 2 x 2 quad of its pixel (4 lanes, lane = corner): `run_fragment_wave` runs the four corners (`FragmentContext::quad_fn`, `quad_bary_fn`),
    the pixel's own writing and the others with no stores; a corner the primitive does not cover (`FragmentContext::quad_covered_fn`: a sample of the corner's pixel, inside the
    bounds and the depth bounds) is a helper. Derivatives of such a program are the quad's differences at the instruction (no reruns). Without a quad (points, lines) the pixel
    is the one active lane of a wave of four. Each covered pixel runs its whole quad, so a pixel shader with wave operations costs four runs a pixel.
  - A **compute shader**'s wave is 32 threads of its group in `SV_GroupIndex` order; such a program runs a group at a time (`needs_group`): a wave at a time when it has no
    barriers or shared memory (any group size), else every thread kept (`keeps_group`, at most `kMaxBarrierThreads`) and each wave run together up to the barrier.
  - The **other stages** run one invocation at a time: a wave of four lanes with the one active (lane 0).
- **What the devices report** (`d3d12.dll` `device.inc`): the CPU renderer `HighestShaderModel` 6.1 (6.1 adds only `SV_ViewID`, `SV_Barycentrics` and `GetAttributeAtVertex`,
  which it has; 6.2 asks for fp32 denormals flushed or kept as the shader says, which the interpreter does not do), `OPTIONS1` `WaveOps` TRUE, `WaveLaneCountMin` 4,
  `WaveLaneCountMax` 32, `TotalLaneCount` 32, `Int64ShaderOps` TRUE; virgl and Venus 5.1 and no wave operations - TGSI has no reductions, prefixes or quad reads (only
  ballot / vote / read-invocation, which would not make the set), so the shaders are refused there (`kFeature3WaveOps` missing: `Adapter::create_shader` gives
  `kNotSupported`, `CreateComputePipelineState` / `CreateGraphicsPipelineState` fail). Venus could carry them as SPIR-V `GroupNonUniform*` but its shaders come from TGSI text
  (`TgsiSpirv.cpp`), which has no words for them: that needs a TGSI dialect of its own (or SPIR-V made from the IR), the host's subgroup sizes as the lane counts, and helpers
  masked out of the reductions (`OpIsHelperInvocationEXT`).
- **Tests**: `WaveOpsTest` (host, `tests/run_host_tests.sh WaveOpsTest`: the fixtures `dxil_cs_wave` (32-bit integers, divergent branches, a loop the lanes leave at different
  rounds, an early return), `dxil_cs_wave_types` (floats, doubles, 64-bit integers), `dxil_cs_wave16` (halves, 16-bit integers), `dxil_cs_wave_match` (6.5, a barrier and shared
  memory between waves) and `dxil_ps_wave` (quads with uncovered and discarded corners) made with dxc, against `tests/windows/wave_model.h`, which works the results out from the
  model; the containers truncated and bit-flipped); `d3d12test`'s `test_wave_ops` / `test_wave_pixels` (the caps, the same fixtures through d3d12.dll, the refusal elsewhere).
- **Not done**: wave operations on the GPUs (above), `WaveSize` (6.6) and the other 6.2+ requirements, what neither front end has (`countbits`, `firstbithigh` / `low`,
  `reversebits`, `msad4`, the bit-field instructions, `EvaluateAttribute*`, `SV_SampleIndex`, `SV_Coverage`, sample offsets outside gathers and comparisons, multisampled
  array views), 16- and 64-bit elements of raw buffers (6.2), quad operations in compute shaders as 6.6's derivatives (the quads are
  lanes 4k .. 4k + 3, not the 2 x 2 layout of the thread ids), a pixel wave of more than one quad, the level-of-detail runs of a pixel (the device's helper runs for implicit
  derivatives) see their pixel as the only lane, and the lanes a pixel runs for its neighbours do not write, so an atomic's returned value in them is not the one their own run gets.

### The other GPU backends and feature level 11_0

Where each stands, and what it would take:

- **Radeon RX Vega M** (`Graphics3D/Vega`, the GCN compiler): the one with a real shader compiler and an emulator that runs what it compiles, so its work is checked on the host.
  It takes 16 vertex inputs, 16 textures (a shader with more than four shares one set of descriptor registers, loaded before each lookup; more than eight vertex inputs
  likewise), 8 vertex buffer slots and every `VertexFormat` (repacked on the CPU: each input the shader reads becomes four 32-bit values), the address modes, and shaders
  with integers and masks (`FSLT` / `USEQ` ..., `UADD` / `UMUL` / `UMAD`, the bitwise operations and shifts, `F2I` / `I2F` ..., `UCMP`, `UIF`, integer immediates).
  Eight colour targets: the pixel shader's `COLOR[n]` goes out as export `mrt n` (with one target and dual source factors `COLOR[1]` is the second source instead), the
  targets are `CB_COLOR0` .. 7 with one blend state and write mask, and each has its own room in the draw area; `AmdVegaMTest` draws into eight on the simulated card and
  each comes back holding its own colour. 24 of the 99 conformance scenes match the CPU renderer (21 before this work); the rest are refused. Still between it and
  11_0: a blend state for each target, loops and `UDIV`, texel fetches and sizes (`TXF`, `TXQ`), the fragment position and face, vertex and instance
  numbers, instancing, lines and points, mipmapped and non-8-bit images. The limit of 12 interpolants it reports is what the tables hold and has no scene of its own yet.
- **VMware SVGA3D**: its commands are the Direct3D 9 generation (`SVGA_3D_CMD_SHADER_DEFINE` with vs / ps 2.0 and 3.0 tokens), which ends at Shader Model 3: about ten
  interpolants, no integer instructions, no texel fetches. More render targets, samplers and vertex streams are possible there, feature level 11_0 is not: that needs the
  DX10 command set (`SVGA_3D_CMD_DX_*`, which takes DXBC as it is, with guest-backed objects). There is no simulator of the device here, so nothing about it can be checked
  on the host.
- **Intel Gen9**: the vertex stage runs on the CPU and the pixel stage is one of three fixed EU kernels. Programmable shaders need an EU compiler and a simulator that runs
  EU code; raising the limits without them would promise what cannot be drawn.
- **Raspberry Pi V3D**: fixed programs and a small straight-line compiler; the device is not in the render nodes, so nothing reaches it.
