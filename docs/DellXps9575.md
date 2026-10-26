# Dell XPS 15 2-in-1 9575 (Core i7-8705G / i5-8305G, Kaby Lake-G)

Two GPUs on one package: Intel HD Graphics 630 (Gen9.5, PCI `0x591B`) and a Radeon RX Vega M GL (a Polaris-family chip with HBM2, which amdgpu calls VEGAM; PCI `0x694E`, GH is `0x694C`).
The internal panel is wired to the Intel GPU, so the Intel GPU is the boot display and the Vega M is a discrete adapter that `GpuManager` puts to sleep.

| Piece | Intel HD 630 | Radeon RX Vega M |
| --- | --- | --- |
| Identified (`*_gpu_identify`) | yes | yes |
| Boot display / mode setting | `IntelGpu` native path (Gen9.5, unvalidated on hardware) | UEFI GOP only (no Polaris display driver) |
| Video decode | `VideoCodec/Intel` (AVC, HEVC, VP9, MPEG-2, VC-1 via the VCS ring, simulated tests only) | H.264, H.265, MPEG-2 and VC-1 on `AmdUvd6VideoEngine`, opt-in (`TCOS_AMD_UVD`), tested in simulation, **not run on hardware** (below); the other codecs on the CPU |
| 3D acceleration | `Gen9GraphicsDevice` on the render engine, opt-in (`TCOS_INTEL_RENDER`), tested in simulation, **not run on hardware** (below) | none (CPU renderer) |

## Not done

- **Polaris bring-up**: SMU7 firmware load, GFX8 CP/RLC/SDMA microcode, memory controller + GART for the HBM2, interrupt handler, DCE 11.2 display.
  None of it exists and none can be checked without the hardware and the firmware blobs.
- **UVD 6.3 video decode** for the Vega M beyond H.264, H.265 (8 and 10 bit), MPEG-2 and VC-1 (MPEG-4; interlaced VC-1; the chip has no VP9), and any run of it on the card.
- **VCE 3.4 video encode** on the Vega M beyond the start and stop (see the VCE section), and any run of it on the card.
- **Hardware 3D** on the Vega M: a GCN4 PM4 path needing a shader compiler to the native ISA. On the Intel GPU see below for what is and is not there.
- Nothing here was run on a real 9575.

## Intel Gen9.5 render engine (HD Graphics 630)

Code: `src/Hardware/GraphicsProcessingUnit/Intel/`, test: `tests/IntelRenderTest.cpp` (`tests/run_host_tests.sh IntelRenderTest`).

| File | Job |
| --- | --- |
| `IntelLrc` | `lrc_init_gen9_rcs`: the render engine's logical ring context (`gen9_rcs_offsets` of the i915 driver; RCS0 at MMIO `0x2000`, 22 pages). |
| `IntelVcsDriver` | `Config::engine = Engine::Render` drives RCS0 through the same execlist code as the video engine: render power well, 28 global GTT pages, PIPE_CONTROL ring job (invalidate, batch, flush, marker write). |
| `IntelRenderCommands` | The 3D command set a batch may hold and the walker that checks a batch against it (no register writes, no global-GTT `PIPE_CONTROL`, no unknown command). |
| `IntelRenderBatch` | Builds one job's batch image: `fill_rect`, `triangles` (flat or interpolated colour, source-over blend), `copy` (nearest-neighbour textured rectangle), `textured_triangles` (sampled, nearest or linear, wrap or clamp), a stencil test and `clear_stencil`. The command order and the copy kernel are those of the intel-gpu-tools Gen9 render copy, which runs on hardware; packet fields were checked against Mesa's genxml (`gen8.xml`, `gen9.xml`). |

The pixel shader is one of three kernels: the igt copy kernel word for word, a constant-colour kernel (igt's write-a-constant kernel with its immediates replaced) and an
interpolating one (the copy kernel's plane-equation instruction with other registers). There is no EU compiler, so arbitrary shaders cannot run. The vertex shader, clipper
and viewport transform are off: the caller supplies window-space vertices (x, y in pixels, z the window depth, and 1/w in the fourth component, which is what the clipper would hand the setup
stage). The depth test is fixed function (D32_FLOAT, less-or-equal, or always for a clear), so the kernels need nothing for it.

What the host test shows: the batch decodes the way the hardware reads it (every pointer resolves inside the image, the surface state, binding table, kernel and vertex
buffer agree), a model of the windower fills the render target correctly through the simulated GPU's page tables, and the driver submits, waits, wraps the ring, wedges on a hang
and cleans up. It does **not** show that the hardware accepts the batch.

## The graphics device and the opt-in flag

`src/Graphics/Graphics3D/IntelGen9/`: `Gen9GraphicsDevice` is an `IGraphicsDevice` (modelled on the V3D one) on the batch builder above; `IntelRenderStack` builds what it stands on
for a real GPU (GPU memory, the render-mode `VcsDriver`, the device). Buffers and images are GPU memory mapped into the engine's address space (the mapping is snooped, so the
CPU needs no flushes). A render pass is a solid-rectangle job for the clear and then jobs of window-space triangles: the CPU runs the vertex shader (MVP from the push
constants), clips (near plane, `w > 0`, a guard band), applies the viewport, culls, and the engine interpolates the colour and blends. Same limits as the V3D device: only the
built-in coloured pipeline (shader modules carry an `EuProgram` tag). Unlike the V3D device it also does depth: `D32Float` images are Y-tiled hardware memory (opaque to the CPU, filled with 1.0 at
creation, never read back), a pass clears depth with a job that draws the clear value and passes every pixel with the colour writes masked, and colours are interpolated perspective-correct from the
1/w the device puts in each vertex. That the setup stage reads the fourth component as 1/w, and the exact Y-tiling and QPitch of the depth buffer, are from the documentation and Mesa's behaviour and have not
been seen on a GPU; the host test's model of the pipeline encodes the same assumption, so it cannot check it.

Switch it on with `cmake -DTCOS_INTEL_RENDER=ON` (off by default). `main.cpp` then passes the flag to `SystemGraphics`, `RenderNode` builds the stack for a Gen9.5 display GPU and keeps
it only if the device passes the same create and draw-a-triangle self-tests the other GPU back ends pass; any failure (no spare global GTT pages, the engine not starting, a
hang, a wrong pixel) leaves the CPU renderer in use and `software_reason()` says why. Without the flag the reason reads "the Intel render engine is off".

What the host tests show (`tests/Gen9GraphicsDeviceTest.cpp`): scenes (clear, gouraud triangle in both byte orders, every cull mode and winding, alpha blending, indexed draws with a
viewport and a scissor, near-plane clipping, triangles behind the eye, 800 triangles over several jobs) give the same picture as the software device through the simulated GPU
and the model of the pipeline; mutating the winding test or the Y flip makes it fail. Resources, address reuse, the single engine lease, present, and a hang (everything the
engine might write to is kept) are covered too.

## Start-up state: MOCS, workarounds, shared power wells

| File | Job |
| --- | --- |
| `IntelGen9Setup` | The Gen9.5 memory object control tables (`skl_mocs_table`: the render engine's 64 control registers at `0xC800` and the 32 L3 registers at `0xB020`), the global GT workarounds (read-modify-write, `gen9/kbl/cfl_gt_workarounds_init`) and the render engine's context workarounds (masked writes, `gen9/kbl/cfl_ctx_workarounds_init`, 13 to 14 of them). Register offsets and bits were taken from i915 `intel_gt_regs.h`, the Kaby Lake / Coffee Lake branch chosen by device ID (Kaby Lake is `0x5900`-`0x593F`) and, for the two stepping-dependent ones, by the PCI revision (Kaby Lake C0 is revision 2 and up). |
| `VcsDriver` | In render mode, applies the above after the power wells are up and before the engine starts (`Config::device_id` / `revision`, from `IntelGpu::render_hardware()`). The video engine path is unchanged. |
| `IntelForcewake` | The wells are now reference counted per GPU (`IRegisterBank::forcewake_state()`; `MmioRegisterBank` finds the counts by the register window's address), so the render and video drivers, each with its own bank, no longer let a well sleep under each other. Tested with four threads. |
| `IntelRenderBatch` | Surface states use MOCS entry 1 (the page table's), as the intel-gpu-tools test does. |

The context workarounds are written to the registers directly instead of being loaded into every context with `MI_LOAD_REGISTER_IMM`, which is enough because the driver has one context (the engine saves them with it). The MCR steering
selector (needed for reads of slice-specific registers) is not set; nothing here reads them.

## Radeon RX Vega M: VCE 3.4 (H.264 encode)

`src/Hardware/VideoCodec/AMD/AmdVce3.*` is the start/stop of the Video Coding Engine 3.4, ported from amdgpu (`vce_v3_0.c`: `vce_v3_0_start`, `_stop`, `_mc_resume`, `_firmware_loaded`, the clock gating override and
software clock gating, `_get_harvest_config`; `amdgpu_vce.c`: the memory layout and the microcode copy) the way `AmdUvd6` was ported from `uvd_v6_0.c`: registers numbered by dword in the card's register BAR, constants
generated from AMD's headers (`scripts/gen_vcn1_regs.py --vi` -> `AmdVce3Registers.inc`), and `scripts/check_vce3_port.py <path to vce_v3_0.c>` confirms every ported function touches the same registers in the same order as
the driver (it skips only the branches for chips before Stoney). The firmware is `/firmware/vegam_vce.bin` (linux-firmware, VCE 3.4, encoder firmware 53.21, so three rings), found with `amd_vce3_firmware_name()`.

What is there: the two instances behind `GRBM_GFX_INDEX` (a Polaris-family chip has the second one fused off; the fuse is read through the SMC, `read_harvest_config`), the memory block (a 384 KiB microcode window, then a stack
and a data area for each instance: 1144 KiB), the three rings of 1024 dwords, the memory controller windows, the clock gating, the start (with the driver's retries of the ECPU) and the stop, and a register snapshot of the moment a
start gives up (`failure_snapshot`, as for the UVD). `tests/AmdVce3Test.cpp` runs it against a simulated card whose registers are banked per instance, and covers one instance, both, only the second, none, the layout of each
instance's windows, the rings, the clock gating, retries and failure, and every refused argument.

The ring: `AmdVce3Ring` is the work submission (`vce_v3_0_ring_emit_ib` of the chips from Stoney on, `amdgpu_vce_ring_emit_fence`, `amdgpu_vce_ring_test_ring`, the ring pointers through
`GRBM_GFX_INDEX`, the nop command and the 16-dword alignment, the commands' values from `vid.h`). A job is `IB_VM` (VMID 0, the indirect buffer's address and length), `FENCE` (address, sequence), `TRAP`, `END` and
padding; the indirect buffer holds the firmware's own task commands (the encode messages, next), padded with no-ops to 16 dwords; the CPU polls the fence word, and a job that does not finish wedges the ring until the engine is
restarted. The host data path is flushed before the write pointer is published. `tests/AmdVce3RingTest.cpp` runs it against a simulated ring controller (all three rings, either instance, wrap-around over 200 jobs, the fence,
a stalled engine, a dead one, every bad argument, and the order of the flush and the pointer; moving the fence words, dropping the padding, using the wrong ring's registers or flushing after the pointer each fails it) and
`check_vce3_port.py vce_v3_0.c amdgpu_vce.c` confirms the job's commands, the ring test and the pointer registers against the driver.

The task commands: `AmdVce3Messages` builds what the firmware reads from the indirect buffer (the command set of Mesa's `radeon_vce_52.c`, for firmware 52 and later): blocks of dwords - size in bytes, command id, fields -
for the session, the task info, create, destroy, the configuration (rate control, motion estimation, rdo, picture control, config extension) and, for one picture, the coded picture buffer, the bitstream buffer, the encode block
(98 dwords: input addresses and pitches, picture type, reference lists, the group-of-pictures counters) and the feedback buffer, with 64-bit addresses high dword first and the offset that chains several encode tasks. The values
are those Mesa uses for a card like the Vega M (not "dual pipe", one instance), with a linear NV12 input and a coded picture buffer laid out as `si_vce_frame_offset` does before Vega 10 (`picture_layout`,
`cpb_slot_count`, `cpb_bytes`). Not built: the VUI block, B pictures' own picture-structure variants, intra refresh and adaptive quantisation. `tests/AmdVce3MessagesTest.cpp` has every block's size, id and fields at the dwords they
must be at (counted from Mesa's source, apart from the code), P and B pictures, the chain of tasks, the buffer arithmetic, the feedback and the writer's bounds; swapping the input addresses, inverting the header insertion, or
changing the chain offset or the height in quad words each fails it. `scripts/check_vce3_messages.py radeon_vce.h radeon_vce_52.c` confirms that the five field blocks have Mesa's fields in Mesa's order, both as declared and as
written.

The encode session: `AmdVce3Encoder` is an `IVideoEncoder` (H.264 Baseline, I420/NV12/BGRA input) on top of the ring and the messages. `prepare()` validates the configuration, leases the work memory (one session at a time) and lays out the input picture, the coded picture buffer, the bitstream and feedback buffers; the first picture opens the session (session, create, configuration, feedback). Each picture is converted to limited-range NV12 at the layout's pitch, an IDR at every keyframe interval and a P picture (reference: the previous picture, in the next CPB slot) otherwise, with `frame_num` restarting at each IDR and the order count 2 * `frame_num` (the POC field is made wide, 8 bits, for long groups). One picture is held until `receive()`. A job that never finishes wedges the ring and every later picture is refused; a missing bitstream refuses only that picture. The destructor destroys the engine's session. `tests/AmdVce3EncoderTest.cpp` runs it against a ring controller plus a firmware stand-in that checks the task blocks and encodes with the CPU H.264 encoder; the packets decode with the CPU decoder to within 28 dB of the source. Assumptions not checkable without hardware: the POC field width, the meaning of the intra/P remaining counters, the QP mapping, and that the firmware writes the access unit at offset 0 of the bitstream buffer.

The encode engine: `AmdVce3VideoEngine` brings the encoder up the way `AmdUvd6VideoEngine` brings up the decoder, and is part of that device (`enable_encode()`, `SystemVideoCodecs::set_amd_vce3()`; off by default, and `main.cpp` does not turn it on until it has run on a card). The microcode (`/firmware/vegam_vce.bin`) loads with the decoder's; the block, three rings, the indirect buffer, the fence and 16 MiB of session memory (19 MiB in all) are carved from the end of the decoders' work area, so the two never share memory; the SMC is asked to power the engine (`PPSMC_MSG_VCEPowerON` 0x10f / `OFF` 0x10e, no argument - checked against `smu7_ppsmc.h`; the UVD pair is 0x61 / 0x60); then the start sequence, the ring's own test and a self-test: an IDR and two P pictures through the engine and back through the CPU decoder, which must come back within a luma mean squared error of 160 (about 26 dB). Any failure stops the engine and leaves encoding on the CPU; the decoders do not depend on it. H.264 sessions go to the engine one at a time. The clock: after the power-on the VCE's DPM is switched on (`AmdSmu7::vce_dpm_enable`), in the order of the Linux driver's `smu7_powergate_vce` and the vegam smumgr's `update_vce_smc_table`: the boot level is written into the SMC's DPM table (the table's address is read from the firmware header at SMC memory 0x20000 + 52; the level count is byte 213 and the boot level byte 1649 of the table - offsets taken with `offsetof` from the real `smu75.h` / `smu75_discrete.h` - in the dword that holds them, the SMC being big-endian), `PPSMC_MSG_VCEDPM_SetEnabledMask` (0x12e) names that one level and `PPSMC_MSG_VCEDPM_Enable` (0x15a) switches DPM on; stopping sends `VCEDPM_Disable` (0x15b) before the power-off. The default is the top level (the driver's stable-pstate case, and the level count comes from the SMC's own table, since the powerplay table the driver uses is not read here); `set_clock_level(Lowest)` leaves the SMC to raise the clock from level 0. An SMC that does not answer or has no DPM table leaves the clock as the card had it, and the engine still starts. Open: whether the system firmware leaves the SMC's DPM running (the driver enables it itself at load, which this OS does not do), so the request may be a no-op on a real card; and anything run on a real card. `tests/AmdVce3VideoEngineTest.cpp` runs the engine and the whole device on a simulated card (`tests/FakeAmdVce3.hpp`: ring controller, boot behaviour, SMC mailbox, harvest fuses, firmware stand-in).

Not there, and needed before a Vega M encodes anything: an `IVideoEncoder` session on top (memory for the buffers, the input picture upload, one task per picture through the ring, the feedback read back), a self-test (the
engine's bitstream has to decode with the CPU decoder to something close to the picture that went in), the memory planning and the PCI plumbing (as `AmdGmc8Memory` and `AmdUvd6VideoEngine` do for the UVD), and the SMC's power and
clock requests. Not run on a Vega M.

## Textures in the graphics device API

`IGraphicsDevice` now has textures (`src/Graphics/Graphics3D/IGraphicsDevice.hpp`): an image made with `kImageSampled` is filled by `IImage::write_pixels` (0xAARRGGBB, the device converts to its byte order), bound to a unit
(`kMaxTextureUnits` = 2) by `ICommandBuffer::bind_texture(unit, image, SamplerDesc)`, and read by a fragment shader through `ITextureSet::sample(unit, u, v)`. A sampler has `Nearest` or `Linear` filtering and
`Repeat` or `ClampToEdge` addressing per axis; coordinates are normalised with (0, 0) at the top left corner of the top left texel and v down; there are no mipmaps. A pipeline says how many units it samples
(`GraphicsPipelineDesc::texture_count`) and its fragment shader is a `textured_fragment_function` (a shader is that or a plain `fragment_function`, never both). `BuiltinShaders` has the textured pair
(`textured_vertex_shader`, `textured_fragment_shader`, `describe_textured_pipeline`: a float3 position, a float2 coordinate, the matrix in the push constants, texture unit 0).

The software device, the Intel Gen9 device and the virgl device (QEMU's virtio-gpu) run textures. On virgl a texture is a host resource without the Y_0_TOP flag (so row 0 of `write_pixels` is the row v = 0 reads),
filled through a guest backing and TRANSFER_TO_HOST_3D, with a sampler view per image and a sampler state per filter/address combination (made on first use, kept by the device); the built-in textured pair has TGSI
(`TEX OUT[0], IN[0], SAMP[0], 2D`). virgl refuses an image that is both sampled and drawn into (the two are stored in opposite row order) and, as everywhere, a draw whose texture is unbound or is the render target.
This is **not validated against a running virgl host**: the wire format follows virglrenderer's protocol, and the row order of a texture is the thing to look at first on a real QEMU. The other devices (V3d, VMware SVGA) refuse a pipeline with a texture count (`nullptr`) and fail a submit that binds a texture, so a caller falls back
to the software device the same way it does for any pipeline they cannot take. Both devices refuse a draw whose texture is not bound, is the image being drawn into, or was not made for sampling. `tests/GraphicsTest.cpp`
covers nearest and linear filtering, both address modes, both byte orders, alpha and blending, perspective-correct coordinates (the test fails with affine interpolation) and every refusal.

WebGL uses these textures through `webgl::IAccelerator` (`WebGl/Accelerator.hpp`): `create_texture`/`write_texture`/`destroy_texture` (RGBA8, first row first, no mipmaps), `AccelState::textures`/`samplers`
(at most `kAccelMaxTextures` = 2) and a `textures` count on `create_program`. `TgsiTranslator` turns `texture2D`/`texture` of a `uniform sampler2D` in a fragment shader into `TEX ... SAMP[i], 2D`
(`TgsiProgram::samplers` says which uniform slot holds each sampler's unit); vertex-shader lookups, explicit levels, cube/3D/array/shadow samplers and a third sampler still refuse. `GlContext::hw_texture`
(`GlAccel.cpp`) takes a draw only when each sampled texture is a 2D RGBA8 (non-sRGB) texture up to 1024 x 1024 with level 0 as base, a NEAREST or LINEAR filter (the same for minification and
magnification), and REPEAT or CLAMP_TO_EDGE wrap; the texels go to the device again when a hash of them changed. WebGPU does the same for `textureSample(texture_2d<f32>, sampler, uv)` in a fragment shader (`wgpu::Device::hw_texture`, `GpuAccel.cpp`): the bind group's view must be one mip level of a
single-layer 2D RGBA8 (not sRGB) texture other than the pass's own colour attachment, and the sampler a non-comparison one with no anisotropy, the same nearest/linear filter both ways, and
repeat or clamp addressing. Device copies are cached per texture and mip (`webgl::AccelTextureCache`) and freed with the texture. Not validated on a running virgl host.

On the Gen9 device a textured draw is what `RenderBatch::textured_triangles` builds: the same job as the other triangle draws, but the pixel kernel is the intel-gpu-tools render copy kernel (it interpolates attribute 0's x and y
as u and v and samples the surface bound after the target), the vertex's colour slot carries (u, v), the binding table has the target and the texture, and the sampler state selects nearest or linear filtering (bits 16:14 and
19:17 of its first dword) and wrap or clamp per axis (the address control modes in the fourth). The CPU still transforms and clips the vertices, and clipping interpolates the coordinate like a colour, so the
hardware's perspective-correct interpolation (from the 1/w each vertex carries) applies to it. Every job now ends with a texture cache invalidation next to the render target flush, so a later job sees what the CPU wrote with
`write_pixels` or what an earlier job drew. `tests/FakeIntelRender.hpp` models the sampler (the filter, the address modes, the surface's byte order) and `tests/Gen9GraphicsDeviceTest.cpp` draws the same textured scenes on the
Gen9 device and the software device and compares the pictures: nearest, repeat, clamp, linear (clamped and repeating), R8G8B8A8 textures and targets, perspective, blending and the refusals. Breaking the sampler's
filter bits, swapping its u and v modes or pointing the texture's binding table entry at the target each makes the comparison fail. `tests/IntelRenderTest.cpp` checks the job's structure.

Nothing of this has run on a GPU: the sampler state's bit positions are from Mesa's genxml (`gen8.xml`, `gen9.xml`), the kernel and the surface state are the ones the intel-gpu-tools render copy runs with, and the
hardware's filtering and edge rounding are not modelled exactly (the tests allow a few levels and a few pixels along texel edges).

## Stencil

`IGraphicsDevice` has a stencil test: a D32Float image made with `ImageDesc::stencil` carries an 8-bit stencil plane next to the depth, a pipeline's `StencilState` says how it is tested (compare function, the fail / depth-fail /
pass operations, compare and write masks, reference; one state for both faces), and `RenderPassBegin::clear_stencil` / `clear_stencil_value` set the plane at the start of a pass. The test comes before the depth
test; a fragment a shader discards changes nothing. A stencil test with no stencil plane in the depth image fails the submit. The software device implements it (`tests/GraphicsTest.cpp`: every compare function, every
operation including clamp and wrap, the masks, the three outcomes, the refusals; breaking the invert operation, the depth-fail operation or the write mask each fails it). V3d, VMware SVGA and virgl refuse an image
with a stencil plane and a pipeline with a stencil test.

On the Gen9 device the stencil plane is a second region of GPU memory, W-tiled and opaque like the depth buffer (the hardware has no linear stencil buffer): `StencilBuffer::bytes_for` is the aligned width times the
height aligned to 64, the pitch the hardware is told is twice the aligned width (a tile is 64 x 64 values in 4096 bytes, 128 bytes wide on its physical rows), and the memory starts as zeros, which is a stencil of
zero in any layout. A draw whose test is on attaches it through 3DSTATE_STENCIL_BUFFER and sets the test, the operations (translated to the hardware's numbering; the compare functions are numbered alike), the masks
and the reference in 3DSTATE_WM_DEPTH_STENCIL (on Gen9 the reference is in that packet) and the stencil write bit of 3DSTATE_DEPTH_BUFFER; a pass clears the plane with a job of its own whose test always passes and
replaces, with the colour and depth writes masked off. `tests/FakeIntelRender.hpp` models the test (it keeps the values in rows of the target's width; the W tiling, which the device never looks at, is not modelled) and
`tests/Gen9GraphicsDeviceTest.cpp` runs the same stencil scenes on the Gen9 device and the software device: the pictures agree and show the expected result for every compare function, every operation, masks, the
three outcomes and the refusals, and the memory of a stencil image is returned. Moving the reference field, swapping the pass and depth-fail operations or the two masks, mapping an operation to the wrong
hardware code, or leaving out the clear each makes it fail. `tests/IntelRenderTest.cpp` checks the packets dword by dword.

What is not verified: the W-tile sizes and pitch above are from the documentation and Mesa's surface layout code as I know them, not from a run; the QPitch of the stencil packet is a guess (a single, non-array
surface should ignore it).

Not done: tiling of colour surfaces (reading or writing a tiled image from the CPU needs the memory controller's bit 6 swizzle, which depends on the machine's memory configuration, and linear surfaces cannot have
mipmaps, so mipmaps wait for it too), interrupts (the driver polls), a render node for a non-display Intel GPU, the other engines' workarounds, and every check
on real hardware. The device is not used by the 2D renderer. Treat the first run on a 9575 as a bring-up, not as a working driver.

## Radeon RX Vega M: UVD 6.3

`src/Hardware/VideoCodec/AMD/AmdUvd6.*` is the start/stop of the Unified Video Decoder 6.3, ported from amdgpu (`uvd_v6_0.c`: `mc_resume`, `start`, `stop`, the clock-gating "ungate" path; `amdgpu_uvd.c`: the memory layout
and the microcode copy), in the way `AmdVcn1` was ported from `vcn_v1_0.c`: the registers are the card's register BAR numbered by dword, the constants are generated from AMD's own headers
(`scripts/gen_vcn1_regs.py --vi` -> `AmdUvd6Registers.inc`), and `scripts/check_uvd6_port.py <path to uvd_v6_0.c>` confirms that every ported function touches the same registers in the same order as the driver.
The firmware is `/firmware/vegam_uvd.bin` (linux-firmware, UVD 6.3, version 1.130, 40 sessions), found with `amd_uvd6_firmware_name()`. `tests/AmdUvd6Test.cpp` runs the sequence against a simulated engine
(including the real firmware file) and covers the layout, the register values, the reset order, boot retries and failure, stop and restart, and every refused argument.

What it does not do, and what is needed before a Vega M decodes anything:
- **Power and clock: both requests are made.** `AmdSmu7` is the SMC's message interface ported from `smu7_smumgr.c` (message and result registers, the wait for the previous message, the argument), with the UVD
  messages of `smu7_ppsmc.h` (`PPSMC_MSG_UVDPowerON` 0x61, `PPSMC_MSG_UVDPowerOFF` 0x60; `smu7_powerup_uvd`'s argument). `AmdUvd6VideoEngine` asks it to power the engine before the start and to power it down when the
  engine stops (only if the SMC said yes). An SMC that is not running (the result register stuck at zero, or reading all ones) gets nothing written and the engine is assumed powered; the start itself then shows whether it is.
  `tests/AmdSmu7Test.cpp` runs the mailbox against a simulated SMC. The clock follows the power, the way `smu7_powergate_uvd(false)` does it (`AmdSmu7::uvd_dpm_enable`): the top UVD level becomes the boot level in the SMC's DPM table (the
  table is found through the firmware header at SMC memory 0x20000 + 52; `UvdLevelCount` is byte 212 and `UvdBootLevel` byte 1648 of it - `offsetof` on the real `smu75_discrete.h` - in the big-endian dword that holds
  them), `PPSMC_MSG_UVDDPM_SetEnabledMask` (0x12d) enables that one level only (the stable-pstate case; `set_clock_level(UvdLevel::Default)` leaves every level enabled and sends no mask) and
  `PPSMC_MSG_UVDDPM_Enable` (0x154) switches DPM on; stopping sends `UVDDPM_Disable` (0x155) before the power-off. An SMC with no DPM table (its firmware not running), or one that refuses, leaves the
  clock as the system firmware had it (the status line says "no DPM" / "DPM refused"; its text is short on purpose: the status string is full at 254 characters). The VCE shares the table and the code
  (`AmdSmu7::dpm_enable`). Still open: whether the system firmware leaves the SMC's DPM running (the driver enables it itself at load, which this OS does not do), so the request may be a no-op on a real card,
  and the PLL path (`vi_set_uvd_clocks`, via AtomBIOS) the older cards use does not exist here.
- **Diagnostics for the first run.** When the start gives up, the ring test fails or the H.264 self-test fails, the status line (the `Firmware` / `Video` lines at boot) carries the engine's registers as they were at that
  moment - `st` UVD_STATUS (bit 1: the VCPU answered), `pw` UVD_POWER_STATUS, `rst` UVD_SOFT_RESET, `cn` UVD_VCPU_CNTL, `lmi` UVD_LMI_CTRL2, `rp`/`wp` the ring pointers - taken before the stop sequence changes them
  (`AmdUvd6::failure_snapshot`, `read_diagnostics`). A card whose engine is unpowered shows in `pw`/`st`, a stalled memory path in `lmi`.
- **Memory: planned and wired (opt-in).** `AmdGmc8Memory` reads the memory controller the way amdgpu does (`CONFIG_MEMSIZE`, `MC_VM_FB_LOCATION`; both set by the card's firmware, nothing is written), places the engine's block, ring and
  a work area (96 MiB if there is room, 8 MiB at least) at the end of the part of video memory the CPU can reach through BAR 0 (the first 256 MiB without Resizable BAR), clear of the firmware's start of memory
  (25 MiB) and a 16 MiB tail guard, and maps them (`amd_map_uvd6_plan`). `AmdUvd6::start` flushes the host data path (`HDP_MEM_COHERENCY_FLUSH_CNTL`) after writing the microcode. That the memory controller lets
  the engine reach these addresses unchanged (VMID 0, inside the system aperture the firmware set) is the driver's behaviour and has not been seen on the card.
- **The ring: done in simulation.** `AmdUvd6Ring` ports the ring side (`uvd_v6_0_ring_emit_ib`, `_emit_fence`, `_insert_nop`, `_set_wptr`, `_ring_test_ring`) and builds the indirect buffer Mesa builds
  (`radeon_uvd.c`: each buffer's address through the GPCOM registers and its command, then the engine start); the command numbers are the same as the VCN ones. It flushes the host data path before it
  publishes the write pointer, and wedges on a missed fence. `tests/AmdUvd6RingTest.cpp` runs it against a simulated ring controller (wrap-around, fence, stall, bad arguments, the ring test), and
  `check_uvd6_port.py` confirms the ring test and the ring commands touch the same registers in the same order as the driver.
- **The messages: H.264 done in simulation.** `AmdUvd6Messages` builds the create, destroy and H.264 decode messages the UVD 6 firmware reads (`struct ruvd_msg` of Mesa's `ac_uvd_dec.h`: one fixed 3556-byte structure, no
  index header; offsets measured by `scripts/gen_uvd_message_layout.py` into `AmdUvd6MessageLayout.inc`), the picture-buffer and context sizes (`calc_dpb_size` / `calc_ctx_size_h264_perf` for Polaris and newer: pictures only,
  17 at most), the feedback word and scaling table, and the list of buffers of a job in Mesa's order (session context, message, picture buffer, context, bitstream, target, feedback, scaling table) ready for `AmdUvd6Ring`. It takes the
  same `AvcPictureState` the VCN path makes from the parser, and a linear NV12 target. `tests/AmdUvd6MessagesTest.cpp` checks every field against offsets worked out by hand from the structure definition, and builds a message
  from a real H.264 stream. HEVC is in the next item; the other codecs and reading the feedback area back are not done.
- **The H.264 session: done in simulation.** `AmdUvd6AvcDecoder` is an `IVideoDecoder` (the same session logic as the VCN one: parser, decoded picture buffer rules, output order, avcC and Annex B, grey for a picture the
  engine fails, wedge on a missed fence, one session at a time) over the UVD ring, the messages and the work memory. `tests/AmdUvd6AvcDecoderTest.cpp` runs whole streams through it against a simulated ring controller and a stand-in
  for the firmware that checks the messages and buffers and writes the CPU decoder's pictures into the target; the session must return exactly the CPU decoder's pictures, in order. It also covers a wedged engine, too little
  memory and 24 damaged variants of the stream (under the sanitizers). Making the stand-in's checks wrong (the target address, the pitch) makes the test fail by the hundreds. - **H.265: done in simulation.** `AmdUvd6Messages` also builds the H.265 decode message (`struct ruvd_h265`, `RUVD_CODEC_H265`, offsets measured into `AmdUvd6MessageLayout.inc`; the values follow Mesa's `get_h265_msg`
  and the VCN message of this OS), with the picture buffer's slot numbers (unlike the H.264 performance stream type the engine does not keep that buffer by itself), the reference lists and the scaling table, and sizes the
  buffers as `calc_dpb_size` and `calc_ctx_size_h265_main` do (17 slots, 8 from 4096 x 2000 up). `AmdUvd6HevcDecoder` is the session (the CPU `H265Decoder` reads the headers and orders the pictures; the engine decodes each
  picture into a linear NV12 target that is read back), 8-bit 4:2:0 only. `AmdUvd6VideoEngine::enable_hevc_decode` runs the libx265 self-test (6 pictures, B pictures with reordering) after the H.264 one; a failure
  leaves H.265 on the CPU, a missed fence stops the engine. `tests/AmdUvd6MessagesTest.cpp` checks every field against offsets worked out by hand from the structure (and fails when one is moved);
  `tests/AmdUvd6VideoEngineTest.cpp` covers the stand-in firmware's checks (order count, slots, start code), wrong pictures, a hang, 48 truncated and bit-flipped streams (under the sanitizers) and a clean stream afterwards.
  Not done: tiles-heavy and other streams the self-test does not cover, and `highestTid`/`isNonRef`, which Mesa itself leaves out.
- **H.265 Main 10: done in simulation.** The pixel pipeline has a `PixelFormat::P010` (NV12 with 16-bit little-endian samples, the value in the top ten bits; `ColorConvert` converts to and from it, keeps all
  ten bits P010 to P010, and drops the low two going to 8-bit formats). The H.265 parser takes streams up to ten bits; the CPU decoder (`h265::Core`) still has eight-bit pixel paths only, so a deeper picture is decoded
  by an offload or not at all (no CPU fallback: the stream's pictures simply do not come out). For a deeper picture the Core holds the planes as 16-bit words with the value in the low bits (`Picture::deep`; the form
  of the spec's picture hash, so the MD5 SEI check works unchanged over two bytes per sample) and hands out P010. `AmdUvd6Messages` fills the message the way Mesa's `get_h265_msg` does for
  `PIPE_VIDEO_PROFILE_HEVC_MAIN_10` with a P010 target (`bit_depth_*_minus8`, `p010_mode` and `msb_mode` 1; the 8-bit conversion fields, `luma_10to8` and the like, stay 0 - the layout generator now covers
  them), sizes the slots 9/4 of the luma size and the context as `calc_ctx_size_h265_main10`, and the session puts the target at twice the NV12 pitch and shifts the words down on read-back. Both depths must be ten.
  The gate is `AmdUvd6VideoEngine::enable_hevc10_decode` after the 8-bit H.265 test: a ten-bit libx265 stream (`scripts/gen_hevc_test_streams.sh`, 6 pictures with reordering) is decoded on the engine and every
  picture must agree with the stream's own MD5 hash - there is no CPU reference to compare with. A failure says so on the status line ("Main 10 test failed") and leaves ten-bit streams undecoded; the rest is
  unaffected. The engine test's stand-in writes FFmpeg's pictures (`tests/data/hevc10_selftest.yuv`) and checks the message by hand-computed values (depth fields, P010 mode, the buffer sizes). Not verified: that the
  real firmware wants these exact modes and pitches (the P010 pitch is taken as twice the NV12 one) - nothing has run on a card; a 4:0:0 or 4:2:2 or 12-bit stream is refused. x265's CRC hashes (`hash=2`) did not
  match the spec's CRC of the chroma planes when I checked them against FFmpeg's pictures (8 and 10 bit), so the stream uses MD5.
- **MPEG-2: done in simulation.** `AmdUvd6Messages` builds the MPEG-2 decode message (`struct ruvd_mpeg2`, `RUVD_CODEC_MPEG2`; Mesa's `get_mpeg2_msg`): the picture coding parameters, both quantiser matrices in the picture's scan
  order, and - unlike the H.265 slots and VCN's - job numbers: the engine keeps the last six pictures it decoded and a picture or a reference is named by the number of its decode job (`frame_number`, from 1), so a reference
  more than six jobs back cannot be named (that picture comes out grey). The job has no context buffer and no scaling table. `AmdUvd6Mpeg2Decoder` is the session (the CPU `Mpeg2Decoder` reads the headers and orders
  the pictures; frame and field pictures), and `enable_mpeg2_decode` runs the ffmpeg self-test (8 pictures, B pictures, a tolerance of a level or two since the engine's inverse transform is not the CPU's). Tested like
  H.265, including 48 damaged streams.
- **VC-1: done in simulation.** `struct ruvd_vc1` is only six dwords (profile, level, the sequence and entry point flags, picture structure, chroma format): there are no picture indexes, so the firmware keeps the
  decoded pictures and finds the anchors by decoding order, and parses the picture header, bit planes and macroblocks out of the frame data itself. `AmdUvd6Messages` builds that message (the size in macroblocks for
  Simple and Main, in samples for Advanced; Mesa's `get_vc1_msg`) and sizes the picture buffer as `calc_dpb_size` does (five pictures, the context area and the lines); the job is the MPEG-2 one. `AmdUvd6Vc1Decoder`
  is the session (progressive pictures; a skipped P picture is not sent, the CPU decoder copies it), and `enable_vc1_decode` runs the two-stream self-test (Main and Advanced profile, both must pass, a level or two
  of tolerance). That the firmware really picks the same anchors as the CPU decoder is what the self-test is for; nothing here has seen the firmware. Not done: interlaced pictures, intensity compensation beyond what the
  firmware does itself, and the MPEG-4 message.
- **The video device: done in simulation, opt-in.** `AmdUvd6VideoEngine` (a `SoftwareVideoCodec`, like `AmdVcnVideoEngine`) loads `vegam_uvd.bin`, reads the memory controller and the graphics block's `GB_ADDR_CONFIG`
  (it refuses to start if that reads zero or all ones), plans and maps the memory, starts `AmdUvd6` (three boot tries), runs the decode ring's own test and the same H.264 self-test the VCN engine runs
  (`AmdVcnSelfTest::run_h264_session`: this OS's own 64 x 64 stream, decoded by the engine and the CPU, byte for byte). Only then does `create_decoder` hand H.264 sessions to the engine (one at a time; H.265 the same way after its own test); any failure stops
  the engine and H.264 stays on the CPU, and every other codec is the CPU's. `SystemVideoCodecs::add_amd_uvd6` finds BAR 0 (with its size) and BAR 5 for device IDs 694C/E/F; it is switched on by
  `-DTCOS_AMD_UVD=ON` (off by default, since the start has never touched a real card). `tests/AmdUvd6VideoEngineTest.cpp` runs the whole path against one simulated card (memory controller, graphics block, engine boot,
  ring controller, a stand-in firmware that decodes with the CPU decoder) and ten cases: success (sessions served by the engine, second session on the CPU, shutdown), no firmware, no registers, too small an aperture,
  a mapper that refuses, an engine that never answers, a graphics block that reports nothing, a ring that does not run, an engine that writes wrong pictures, and one that never finishes a picture.
- Still open on the video side: MPEG-4, interlaced VC-1, reading the feedback area back, SMC clock requests (a card whose engine stays unpowered makes the start time out; it is stopped and reported), and a run on a real card.
- Encode rings, medium-grain clock gating, and the 3D side of the card (CP/RLC/SDMA microcode, memory controller and GART, display) are all untouched.
- Not run on a Vega M.
