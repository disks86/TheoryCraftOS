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

- **Polaris bring-up**: the SMC microcode load and the GFX 8 command processor start are written and so are the memory controller and GART (see "Radeon RX Vega M: graphics engine start-up" and "... memory controller and GART" below, simulation only). Still missing: DCE 11.2 mode setting (only the surface address is written, see below), the rest of GFX 8. The interrupt handler ring is written ("... interrupt handler" below).
  None of it can be checked without the hardware.
- **UVD 6.3 video decode** for the Vega M beyond H.264, H.265 (8 and 10 bit), MPEG-2, VC-1 (interlaced pictures untested against real data, see below) and MPEG-4 (the MPEG-4 tools listed below; the chip has no VP9), and any run of it on the card.
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
`Repeat` or `ClampToEdge` addressing per axis; coordinates are normalised with (0, 0) at the top left corner of the top left texel and v down; mipmaps are described below. A pipeline says how many units it samples
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

## Colour tiling and mipmaps

`ImageDesc::mip_levels` and `IImage::write_mip_pixels` / `generate_mipmaps` (the 2 x 2 box filter of `Mipmaps.hpp`, which also has the level sizes and the logarithm); `SamplerDesc::mip_filter`
(`None`: level 0 only, as before; `Nearest`; `Linear`), `lod_bias`, `min_lod`, `max_lod`. The level of detail is the one OpenGL's formula gives from the derivatives of the coordinates in texels of level 0,
taken over the 2 x 2 pixel quad a pixel is in (right minus left, lower minus upper, the same for the whole quad - what the sampling hardware does): rho is the longer of the two vectors,
lambda = log2(rho) + bias clamped to the sampler's bounds and the image's levels; at 0 or less the lookup is magnified and reads level 0; Nearest reads level floor(lambda + 0.5), Linear
blends floor(lambda) and the next by the fraction. The software device does it by running the textured shader three more times for the quad's upper left, right and lower pixels with the lookups only recorded
(only for draws that bind a mipmapped image with a mip filter). The Intel Gen9 device does it in hardware; V3d, VMware SVGA and virgl refuse an image with more than one level.

On the Gen9 device **every colour image is now Y-tiled** (tile mode 3 in the surface state; the pitch a multiple of 128 bytes, the base a whole page, the memory a multiple of 32 rows), like the depth buffer already was.
The old note that the CPU cannot address tiled colour memory because of the memory controller's bit 6 swizzling does not apply to Gen9: the i915 driver programs `I915_BIT_6_SWIZZLE_NONE` for Gen8 and
later ("on BDW+, swizzling is not used"), so `write_pixels`, `read_pixels` and `present` (which gathers the rows into a linear copy for the display) address the memory as laid out: a tile is 128 bytes by 32 rows in 4 KiB,
tiles row by row, and inside a tile 16-byte columns run down the 32 rows, side by side (`y_tile_offset`; the layout of `isl_tiled_memcpy.c`). A texture may have a mip chain (a sampled-only image: a render target
or present image with levels is refused): `mip_layout` lays out the 2D surface as the Skylake PRM and Mesa's isl do for 32-bit texels without an auxiliary surface - every level's size rounded up to
4 texels each way, level 0 on top, level 1 under it, level 2 right of level 1 and the others under level 2 - and gives the pitch, the rows, and the rows the levels take (the surface state's QPitch,
in units of 4 rows). The surface state carries the tile mode, the QPitch, and (dword 5, bits 3:0) the number of levels less one; the sampler state the mip mode filter (bits 21:20: 0 none, 1 nearest, 3 linear),
the LOD bias (13:1, signed 4.8), and the maximum (19:8) and minimum (31:20) LOD of its second dword (unsigned 4.8, kept within the levels the image has; zero when there is no mip filter, which keeps a one-level image at level 0).
The pixel kernel is still the copy kernel with its sampler message, which takes the level from the quad's derivatives itself.

`tests/FakeIntelRender.hpp` models the memory (a second, bit-formulated version of the tiling), the mip layout (written from the PRM's description, not from `mip_layout`), the sampler's mip mode, bias and bounds, and
the quad derivatives (the coordinates extended beyond the triangle, as the hardware does for the pixels of a quad it covers in part); `tests/IntelRenderTest.cpp` checks the tile offsets and the layouts against
numbers worked out by hand (64 x 64 with 7 levels: pitch 256, 100 rows in the levels, 128 rows of memory, level 2 at (32, 64); 5 x 3, 100 x 4 and the refusals) and the surface and sampler state dword by dword;
`tests/Gen9GraphicsDeviceTest.cpp` draws solid-level chains (which level shows), generated chains, odd sizes and a receding floor on the Gen9 and software devices and compares the pictures, round-trips images of
33 sizes through the tiling, and checks the refusals and that the memory comes back; `tests/GraphicsTest.cpp` covers the software device's level selection (nearest, linear, bias, bounds, magnification, anisotropy, odd sizes,
generation, both byte orders). Every colour scene of the earlier tests passes unchanged on tiled images.

What is not verified: nothing has run on a GPU. That the sampling message takes the level from the quad's coarse derivatives the way the model and the software device do (and rounds Nearest levels the same way, and treats
Nearest and Linear mip filters with the same LOD bounds) is the thing to look at first; the mip layout and QPitch are from the PRM's text as Mesa implements it, and the tests check the code against my reading of them.
Render targets stay single-level (a chain on a drawn-into image is refused), and mipmaps are not used by the WebGL/WebGPU paths (their textures are level 0 only).

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

Not done: interrupts (the driver polls), a render node for a non-display Intel GPU, the other engines' workarounds, and every check
on real hardware. The device is not used by the 2D renderer. Treat the first run on a 9575 as a bring-up, not as a working driver.

## Radeon RX Vega M: graphics engine start-up

Code: `src/Hardware/GraphicsProcessingUnit/AMD/AmdGfx8Firmware.*`, `AmdSmu7Ucode.*`, `AmdGfx8.*`, `AmdGfx8Compute.*` (+ generated `AmdGfx8Registers.inc`), tests: `tests/AmdGfx8Test.cpp` with the simulated card `tests/FakeAmdGfx8.hpp`
(`tests/run_host_tests.sh AmdGfx8Test`), compute queues: `tests/AmdGfx8ComputeTest.cpp` (the simulated microcode in `FakeAmdGfx8.hpp` polls doorbells, runs the KIQ's packets, maps queues from the descriptors it reads and checks them), checker: `scripts/check_gfx8_port.py <gfx_v8_0.c>`.

| File | Job |
| --- | --- |
| `AmdGfx8Firmware` | Takes apart the microcode files (`amdgpu_ucode.h`: the common header and the gfx/rlc/sdma additions): sizes checked against the file, the ucode array, the MEC's jump table. The names are those of linux-firmware (`vegam_pfp.bin`, `_me`, `_ce`, `_mec`, `_mec2`, `_rlc`, `_sdma`, `_sdma1`; all in `/firmware`, SHA-256 in `firmware/README.md`). |
| `AmdSmu7Ucode` | On this chip the **SMC loads the engines' microcode**, not the driver (`smu7_request_smu_load_fw`): a table of contents (`SMU_DRAMData_TOC`: id, version, address, size, flags per image) and the images go to video memory, the SMC is told where (`PPSMC_MSG_SMU_DRAM_ADDR_HI/LO`, `DRV_DRAM_ADDR_HI/LO`), asked to load (`LoadUcodes`, mask `0x47E`: RLC_G, SDMA0/1, CE, ME, PFP, MEC) and `UcodeLoadStatus` (SMC memory, soft registers address at header +48, word at +108 - `offsetof` on the real `smu75.h`) is polled for the same bits. The MEC's slot excludes its jump table, which has slots of its own (JT1 from the MEC file, JT2 from `vegam_mec2.bin` if given). |
| `AmdGfx8Compute` | The compute queues (`gfx_v8_0.c` KIQ / KCQ): a ring, an end-of-pipe buffer, a write-back page and a memory queue descriptor (MQD, `struct vi_mqd`, field positions generated by `scripts/gen_gfx8_mqd.py`, which also checks that the descriptor's register image has the registers' order) per queue. The **KIQ** (MEC 1, pipe 3, queue 0, doorbell 0) is programmed by the driver straight into its hardware queue registers (`mqd_init` + `mqd_commit` under an `SRBM_GFX_CNTL` selection, `RLC_CP_SCHEDULERS` told which queue it is); the eight **KCQs** (queues 0-7 of pipe 0, doorbells `0x10`-`0x17`) are then mapped by `SET_RESOURCES` and `MAP_QUEUES` packets on the KIQ ring, which the compute microcode answers by reading their descriptors. Rings are driven by doorbells (BAR 2). `stop()` sends `UNMAP_QUEUES` (RESET_QUEUES) for each queue and tests the KIQ ring before halting the engines. `AmdGfx8::attach_compute` puts it at the driver's places: KIQ before the graphics ring, KCQs after it, ring tests of all together. |
| `AmdGfx8` | `gfx_v8_0_hw_init` without `init_pg` beyond the clear-state buffer: the VEGAM golden registers (`golden_settings_vegam_a11`, `vegam_golden_common_all`, compared value for value by the checker), `constants_init` (address configuration, the 32 tile modes and 15 macrotile modes, the render back-end set-up with the harvested raster configuration read from `CC_/GC_USER_RB_BACKEND_DISABLE`, shader memory apertures of all 16 VMIDs, GDS, FIFO sizes), RLC stop / reset / clear-state buffer / start, CP ring 0 (8 KiB, `CP_RB0_*`, write-back read pointer), the clear state (`vi_cs_data`: 885 context register defaults, sent through the ring and kept in the RLC's buffer; generated by `scripts/gen_gfx8_tables.py` from `clearstate_vi.h` and the driver's tile mode expressions), the raster configuration and `SET_BASE` for the CE partitions, and the driver's ring test (`SET_UCONFIG_REG` of `SCRATCH_REG0`). `stop()` is `gfx_v8_0_hw_fini` (graphics and compute CPs halted, RLC stopped). |

What the test shows: the real firmware files parse to the values worked out from their headers (and every truncated or bit-flipped header is refused or yields views inside the file, under the sanitizers); the table the
SMC reads has the right ids, versions, sizes, flags and GPU addresses (including addresses above 4 GiB), and each image is in video memory byte for byte; the messages go in the driver's order and a flush of the host data
path precedes them; every failure (no SMC, no firmware header, a refused load, a load that does not finish, memory that is too small or misaligned) is reported and leaves video memory untouched where it should; the
register writes of the start are in the driver's order with the driver's pauses; and the simulated command processor only runs the ring once the SMC has set the CE, PFP and ME bits and the halt bits are clear, so the ring test
fails without the load and passes with it. The simulated card is written from the driver's description; it does **not** show that the real SMC accepts this table or that the real command processor runs these packets.

Not done and assumed:
- Left out: `init_pg` beyond the clear-state buffer (save/restore list, power gating), `get_cu_info`, the graphics ring's doorbell (its write pointer goes through `CP_RB0_WPTR`), compute queue priorities, anything that submits work (fences, indirect buffers) and interrupts, the RLC safe mode and idle waits of `hw_fini`. Whether the engine draws is untested.
- `shader_engines` / `sh_per_se` / `max_backends_per_se` (2 / 1 / 2 by default) come from the video BIOS in the driver (`amdgpu_atombios_get_gfx_info`); they decide the RLC serdes wait and the render back-end set-up (a part with back-ends fused off gets the driver's harvested raster configuration, per shader engine). The wrong numbers here would write a wrong raster configuration on a real card.
- The SMC is assumed to be running already (the card's firmware started it; `vegam_start_smu` only starts it if it is not): a card where it is not gets `NotAnswering` and nothing is written to its memory. The SMC firmware (`vegam_smc.bin`) is not loaded.
- A `NOP` of count `0x3FFF` is a one-dword filler in the ring (that is how the driver pads), and the simulated card encodes that assumption.
- The memory controller must let the SMC and the CP reach the video memory addresses used (VMID 0, inside the system aperture the card's firmware set), as for the video engine.
- Nothing here was run on a real 9575.

## Radeon RX Vega M: memory controller and GART

Code: `src/Hardware/GraphicsProcessingUnit/AMD/AmdGmc8.*` (+ generated `AmdGmc8Registers.inc`), tests: `tests/AmdGmc8Test.cpp` with `tests/FakeAmdGmc8.hpp` (`tests/run_host_tests.sh AmdGmc8Test`), checker:
`scripts/check_gmc8_port.py <gmc_v8_0.c>`. Ported from `gmc_v8_0_hw_init` (golden registers, `mc_program`, `gart_enable`), `gart_disable`, the TLB flush, and `amdgpu_gmc_gart_location`.

`amd_plan_gmc8_gart` puts a 256 MiB GART in the GPU's 40-bit address space the way the driver does (best fit, 4 GiB aligned, cut down if it does not fit; for the Vega M's 4 GiB at address 0 it lands at
`0xFF00000000`). `AmdGmc8::start` clears the 512 KiB page table (8 bytes per 4 KiB page, in video memory), programs the system aperture (video memory's range, the unmapped-access page), the AGP window (none), the
host data path, the L1 TLB / L2 cache, VM context 0 (the GART, one-level page table) and contexts 1-15 (empty, pointing at the GART table, faults redirected to a dummy page). `map()` writes page table
entries (`kAmdGmc8PteSystemPage` for ordinary RAM: valid, system, snooped, read, write, exec; `kAmdGmc8PteVideoPage` for video memory), flushes the host data path and invalidates the TLBs; `unmap()` clears them.
That is what lets an engine reach a buffer outside the CPU-visible part of video memory, or in RAM.

The test's memory controller model reads the entries from the table and caches translations until `VM_INVALIDATE_REQUEST`, so a `map()` that forgot the invalidation would show stale pages. Register values were worked out
from the bit positions in AMD's headers. Not done and assumed: VM faults (no interrupt handler; faults go to the dummy page), the VGA lockout (display block registers; the driver only does it with a display driver),
the memory controller microcode (the driver does not load it for the VEGAM), per-process page tables (contexts 1-15 stay empty), and the assumption that the card's firmware left the controller in a state this can be
layered on (it rewrites the system aperture to the same range the controller reports). Nothing here was run on a real 9575; a wrong aperture on a real card would hang it.

## Radeon RX Vega M: interrupt handler

Code: `src/Hardware/GraphicsProcessingUnit/AMD/AmdIh3.*` (+ generated `AmdIh3Registers.inc`), tests: `tests/AmdIh3Test.cpp` with `tests/FakeAmdIh3.hpp` (`tests/run_host_tests.sh AmdIh3Test`), checker:
`scripts/check_ih3_port.py <tonga_ih.c>`. Ported from `tonga_ih.c` (the IH 3.0 the Polaris family and the VEGAM use): the 64 KiB ring of 16-byte vectors, its write pointer (register and
a write-back word after the ring), the read pointer (the doorbell BAR, or `IH_RB_RPTR`), `tonga_ih_irq_init` / `_irq_disable` / `_get_wptr` (with the driver's ring overflow recovery: the read pointer
moves to the write pointer + 16 and `WPTR_OVERFLOW_CLEAR` is pulsed) / `_decode_iv` (source id, 28-bit source data, ring id, VMID, PASID) / `_set_rptr`.

`AmdIh3::drain()` returns the vectors that arrived; nothing dispatches them (the OS polls, and the card's interrupt line goes nowhere). Every block's own interrupt enable (`CP_INT_CNTL_RING0`, the compute
pipes' EOP enables, the memory controller's fault interrupts) is a separate thing and is not switched on here. Not done and assumed: bus mastering (the PCI layer must set it before the ring is enabled),
MSI (`Config::msi` only sets `RPTR_REARM`), the software ring (`ih_soft`), the ring in video memory (it expects memory the CPU can read and the card can write, e.g. system RAM mapped through the GART; the
non-snooped bit is left clear as in the driver for system memory), and every check on a real card.

## Radeon RX Vega M: SDMA engines

Code: `src/Hardware/GraphicsProcessingUnit/AMD/AmdSdma3.*` (+ generated `AmdSdma3Registers.inc`), tests: `tests/AmdSdma3Test.cpp` with `tests/FakeAmdSdma3.hpp` (`tests/run_host_tests.sh AmdSdma3Test`), checker:
`scripts/check_sdma3_port.py <sdma_v3_0.c>`. Ported from `sdma_v3_0.c`: the golden registers (`golden_settings_polaris11_a11`, compared by value), both engines halted, each engine's ring (8 KiB: size, base, read pointer
write-back, doorbell `0x1E0` / `0x1E1`, no write-pointer polling), un-halt, context switching, and the driver's ring test (a WRITE packet to a word of memory). Their microcode is loaded by the SMC
(`AmdSmu7Ucode`). `copy()` and `fill()` build the driver's COPY_LINEAR / CONST_FILL packets (chunks of at most `0x3fffe0` bytes), then the fence write and a trap (`sdma_v3_0_ring_emit_fence`), and wait for the
fence; an engine that does not answer is wedged until it is started again. Padding to 16 dwords uses a burst NOP when `Config::burst_nop` is set (the driver sets it for a firmware feature version of at
least 20: `AmdGfx8Image::feature_version`, 31 for `vegam_sdma.bin`, 0 for `vegam_sdma1.bin`).

With the GART (`AmdGmc8`) this moves data between video memory and system RAM without the CPU. Not done and assumed: interrupts (the trap packet is sent; see `AmdIh3`), indirect buffers, the VM flush and
page-table update packets (the rings use VMID 0), the RLC queues, tiled copies, clock gating, soft reset and the SR-IOV poll-memory mode. The test's engines are written from the driver's description of the
packets; a real engine's handling of a burst NOP, an unaligned destination or a copy that crosses the end of a GART mapping has not been seen. Nothing here was run on a real 9575.

## Radeon RX Vega M: DCE 11.2 display

Code: `src/Hardware/GraphicsProcessingUnit/AMD/AmdDce11.*` (+ generated `AmdDce11Registers.inc`), tests: `tests/AmdDce11Test.cpp` (`tests/run_host_tests.sh AmdDce11Test`), checker:
`scripts/check_dce11_port.py <dce_v11_0.c>` (the file is from kernel 6.12; later kernels dropped the legacy display code for the display core). The VEGAM uses the DCE 11.2 block
(`dce_v11_0.c`, register headers `dce_11_2_*`). This is the dGPU counterpart of `amd_dcn1::Plane`: it **does not set a mode**. The card's video BIOS lit the screen at POST; `amd_dce11::Plane::probe()` finds the
CRTC (of six) whose graphics surface is the firmware's framebuffer (same GPU address, same pitch, ARGB8888, linear, no byte swapping, CRTC and surface on) and `set_surface()` points it at another
address inside the memory controller's system aperture the way `dce_v11_0_page_flip` does (immediate flip, pitch, high half, low half; the firmware's flip policy is put back), waits until the update is no
longer pending and the address is the new one, and puts the old address back if it is not taken. `restore()` returns to the firmware's picture.

Why no mode setting: the driver programs the PLLs, the DIG encoders and the transmitters (and trains DisplayPort links) through the video BIOS's AtomBIOS command tables, which need an interpreter this OS does not
have, and the panel of the 9575 is wired to the Intel GPU anyway: the Vega M's outputs are the external ports. Not done: wiring it into `AmdGpu` / the scanout (the Raven path's `Scanout` plans a framebuffer in
the carve-out and copies the firmware's picture; the Vega M would put its framebuffer in the part of video memory the video engine does not use and map it through BAR 0), hotplug, cursors, the colour
pipeline, vblank counters, underflow detection and every check on a real card. Whether the display reads the new address through the system aperture unchanged after `AmdGmc8` rewrote the aperture is the
driver's behaviour and has not been seen on the card.

## Radeon RX Vega M: bringing it up together (`TCOS_AMD_VEGAM`)

Code: `src/Hardware/GraphicsProcessingUnit/AMD/AmdVegaM.*`, test: `tests/AmdVegaMTest.cpp` (`tests/run_host_tests.sh AmdVegaMTest`), switch: `cmake -DTCOS_AMD_VEGAM=ON` (off by default, like `TCOS_AMD_UVD`: none of it has run on a real
card, and a wrong register on a real GPU can hang the machine). `main.cpp` runs it once the root volume is mounted (it reads `/firmware`) and before the video engines start (the memory controller comes first, as in the driver):
bus mastering is switched on, BAR 5 (registers), BAR 2 (doorbells) and BAR 0 (video memory) are mapped, and `AmdVegaM::bring_up` does, in the Linux driver's order: read the memory controller (`AmdGmc8Memory`), read and
parse the eight microcode files, lay out one block of video memory (below), `AmdGmc8::start` (GART), `AmdIh3::start`, `AmdSmu7Ucode::load`, `AmdGfx8::start` with `AmdGfx8Compute` (eight queues), `AmdSdma3::start`. Each
log line says how far it got (`Vega M: started: GART 256 MiB, ...` or `not started - <step>: <reason>`); a step that fails stops the earlier ones again in reverse.

The block is at 48 MiB of video memory (above the firmware's framebuffer, below the video engine, which sits in the last half of the 256 MiB the memory BAR shows; the whole block ends by 128 MiB): dummy and scratch pages,
the GART's 512 KiB page table, the microcode images and the SMC's scratch area (1.3 MiB), the graphics ring with its write-back page and clear-state buffer, the compute queues (184 KiB), the SDMA rings, the interrupt ring
(told it is not snooped: `AmdIh3::Config::ring_in_video_memory`) and the OS's framebuffer (the largest mode, whole MiB). Everything is reached by the CPU through the memory BAR and by the card at the memory controller's
base plus the offset.

The display part: if the active display GPU (`AmdGpu`) is this card, `AmdGpu::set_native_display` takes the CRTC that shows the firmware's framebuffer over for a framebuffer in the block (`amd_dce11::Display`), hands it to
the renderer, and gives the firmware's back before a mode change and at shutdown. On the 9575 the boot display is the Intel GPU, so the Vega M normally does not drive it and the log says so; the hook is not covered by a host
test (no test builds `AmdGpu`), only by the pieces' own.

Not done: the GFX configuration numbers that the driver takes from the video BIOS are defaults (`AmdGfx8::Config`: 2 shader engines, 1 array, 2 back-ends: wrong values would write a wrong raster configuration),
the clock and power requests the video engines make are separate, nothing uses the rings yet (no command submission, no use of SDMA by the OS, interrupts are not dispatched), and the UVD engine's memory plan does not know about
this block (it relies on the 128 MiB limit). Treat the first run on a 9575 as a bring-up.

### Command buffers: indirect buffers and fences

`AmdGfx8::submit_ib`, `AmdGfx8Compute::submit_ib` and `AmdSdma3::submit_ib` run an indirect buffer (a command buffer already in memory the engine reads) and wait for a fence, each ported from the driver's
emit functions: the graphics ring gets `INDIRECT_BUFFER` (`gfx_v8_0_ring_emit_ib_gfx`) and the fence of `gfx_v8_0_ring_emit_fence_gfx` (two `EVENT_WRITE_EOP`, the first a workaround that writes the number below,
the second the number; word 12 of the write-back page); a compute queue gets `INDIRECT_BUFFER` with the valid bit (`..._emit_ib_compute`) and `RELEASE_MEM` (`..._emit_fence_compute`; word 16 of the queue's
write-back page); an SDMA engine gets the `INDIRECT` packet placed to end on an 8-dword boundary of the ring (`sdma_v3_0_ring_emit_ib`) and the fence/trap of the copy path. Buffers are padded as the driver pads them
(`AmdGfx8::pad_ib`: NOPs to a multiple of 256 dwords; `AmdSdma3::pad_ib`: to 8, with a burst NOP where the engine has it). A fence that never comes wedges that ring until the engine is started again.
`AmdVegaM::submit_graphics` / `submit_compute` / `submit_sdma` copy a command buffer into the block's 8 KiB staging area, pad it and run it, one at a time.

The simulated engines run the buffers' packets (register writes, memory writes, copies, fences) and object to anything else (a buffer that is not packets, a compute buffer without the valid bit, a VMID, an
IB that does not end on an 8-dword boundary), so the packets' framing is checked; what the real microcode does with these framings, and with caches (the driver's fence packets flush the GPU's caches before the write;
the simulated ones do not model caches), has not been seen. No shaders are run (there is no compiler for the GCN4 instruction set), no draws are issued, VMIDs other than 0 and the hardware-queue
priorities are not used, and nothing in the OS calls these yet.

### Copies between system memory and video memory

`AmdVegaM::upload(source, offset)` and `download(destination, offset)` copy between a buffer in system RAM and the block's framebuffer area (the part of the block meant for data; `offset` is in it). The
system pages are mapped into the first `kTransferPages` (512, 2 MiB) pages of the GART with `kAmdGmc8PteSystemPage` (valid, system, snooped, readable, writeable, executable: what `amdgpu_ttm_gart_bind` writes
for ordinary memory), the SDMA engine (`instance`, default 0) copies between that window and video memory with `AmdSdma3::copy`, a window at a time, and the window is unmapped again. A buffer that
starts inside a page is handled (the page offset is added to the window address). The card is given a buffer's address as it is (the OS keeps its memory mapped at its own addresses), or whatever
`Inputs::system_addresses` (`ISystemAddresses`) returns; addresses beyond the card's 40 bits are refused. A call returns when the copy is done; one at a time. If the engine does not finish it is wedged, the
window stays mapped (the engine may still use the pages, so the caller must not free the buffer) and the engine is given nothing more until it is started again. A copy the engine never took is unmapped.

The simulated SDMA engine walks the GART table the driver wrote (a system page is a page of the test's own memory, a video page is video memory) and faults on an invalid entry, so the table's contents and
flags are checked, as well as the data. Nothing in the OS calls these yet, and the real engine's behaviour with snooped system pages (cache coherency, the IOMMU if one is on) has not been seen.

### The OS's picture goes through SDMA

When the Vega M's display is engaged, `AmdGpu` no longer draws into the uncached window of video memory (slow to write a pixel at a time and very slow to read for `copy_rect`). It draws in a back buffer
in system memory (`amd_dce11::Presenter`, started from what the display shows) and, after each drawing call, sends the rows the dirty rectangles cover (one run, from the first dirty row to the last) to video
memory with `INativeDisplay::upload_picture`, which `AmdVegaM` implements with `upload()`. If the engine cannot (not up, wedged, address beyond 40 bits, copy refused) the CPU copies the same rows into the
window, so the picture is always right. Before the display goes back to the firmware's framebuffer (mode change, shutdown) the whole back buffer is presented first.

The Presenter is host-tested with the real `AmdVegaM` and the simulated SDMA engine (data in video memory, one copy per present, the CPU fallback, a wedged engine, rows outside the picture, the final present
before disengaging). `AmdGpu`'s own use of it (the back buffer swap and the flush) is not host-tested: that needs the UEFI graphics classes. Decoded video frames are not routed this way yet: the video
player draws through the same GPU `blit`, so they take this path only as far as the player calls the GPU's drawing functions.

### Interrupts: dispatch by source

`AmdIrq` (`AmdIrq.*`) is the dispatch half of `amdgpu_irq_dispatch` for the sources of the blocks `AmdVegaM` brings up. `AmdVegaM::service_interrupts()` drains the IH ring (`AmdIh3::drain`), classifies each
vector by source id and ring id, and passes every event to a sink. Sources, from the blocks' `sw_init` (`amdgpu_irq_add_id`): `gfx_v8_0` 181 end of pipe (a graphics fence for ME 0, a compute fence for ME 1 and 2;
ring id bits 3:2 ME, 1:0 pipe, 6:4 queue), 184 privileged register, 185 privileged instruction, 197 ECC error; `sdma_v3_0` 224 trap (engine in ring id bits 1:0, a fence only for queue 0), 241 and 247 illegal
instruction; `gmc_v8_0` 146 and 147 VM faults, for which `VM_CONTEXT1_PROTECTION_FAULT_ADDR/STATUS/MCCLIENT` are read, decoded (vmid, protections, client, read/write) and cleared (`VM_CONTEXT1_CNTL2` bit 0). A
vector with both fault registers zero is dropped, as in the driver. Everything else is counted by source id. `scripts/check_irq_port.py` checks the ids, registers, masks and ring id decoding against the kernel
sources (v6.12).

What a handler does: the driver passes fences to its scheduler and faults to its timeout handling (`drm_sched_fault`). Here the engines' `submit_*` functions poll their fence words, so a fence is only counted and
shown to the sink, and a fault wedges the ring it came from (`AmdGfx8::fault`, `AmdGfx8Compute::fault(me, pipe, queue)`, `AmdSdma3::fault`) until the engine is started again. The desktop loop calls
`service_interrupts()` (under `TCOS_AMD_VEGAM`); with MSI (below) only when an interrupt came.

The sources are turned on at the end of `AmdVegaM::bring_up` (after the SDMA ring tests) and off at the start of `shutdown`, each a read-modify-write like the driver's `amdgpu_irq_get`/`put` state functions:
`AmdGfx8::set_interrupts` (`CP_INT_CNTL_RING0` TIME_STAMP, PRIV_REG, PRIV_INSTR: `gfx_v8_0_set_gfx_eop_interrupt_state`, `_set_priv_reg_fault_state`, `_set_priv_inst_fault_state`),
`AmdGfx8Compute::set_eop_interrupt` (TIME_STAMP of `CP_ME1_PIPEn_INT_CNTL` for the queues' pipe; MEC 1 only, as amdgpu leaves MEC 2 to amdkfd), `AmdSdma3::set_trap_interrupts` (`SDMA0_CNTL.TRAP_ENABLE` of both
engines) and `AmdGmc8::set_fault_interrupts` (the seven `*_PROTECTION_FAULT_ENABLE_INTERRUPT` bits of `VM_CONTEXT0_CNTL` and `VM_CONTEXT1_CNTL`). `scripts/check_irq_port.py` checks the registers and fields against the
kernel's state functions. The illegal-instruction sources have no enable in the driver (always on).

Not done: the CP ECC error (`gfx_v8_0_set_cp_ecc_int_state`, 13 registers) and SQ interrupts are not enabled (source 197 is classified but a real card will not post it); the display's interrupts (vblank, page flip, hot plug) and the UVD/VCE ones are not dispatched; no ring reset or recovery after a fault (the engine has to be restarted).

### MSI: interrupts wake the CPU

`PciMsi` (`src/Hardware/Bus/Pci/PciMsi.*`) programs the MSI capability (id 0x05) of a PCI function as PCI Local Bus Specification 3.0 section 6.8.1 describes: message control cleared of enable and
multiple-message-enable, the address (and its upper half for a 64-bit function), the data word, the per-vector mask bits cleared, then enable, checked by reading the enable bit back (a function that does not take
it is left off). The command register's INTx disable bit is set while MSI is on and cleared again after, written without touching the status bits (they clear when written). The message is the caller's: `main.cpp` builds
the x86 one, `0xFEE00000 | apic_id << 12` with vector `kVectorAmdVegaM` (0x51), for the boot core.

With `TCOS_AMD_VEGAM`, `main.cpp` installs the handler (EOI and a counter; no register is touched in the interrupt), enables MSI **before** `AmdVegaM::bring_up` (the driver's order; `Inputs::msi` makes the interrupt
ring re-arm the message, `IH_RB_CNTL.RPTR_REARM`), then tries it out as virtio-gpu does: an SDMA copy ends with a trap, which must arrive as an interrupt within 5 ms. If it does, the desktop loop reads the ring only
when the counter moved (and once a second in case a message was lost); if not, MSI is switched off again and the ring is polled every pass as before.

Host-tested: the capability programming (`PciMsiTest`: 32/64-bit, with and without per-vector masking, refusals, status bits never written, undone by the destructor) and that `Inputs::msi` changes only the ring's re-arm
bit. Not tested: the handler and the loop (they need the kernel), and nothing has run on a real machine: whether the Kaby Lake-G's PCH/root port delivers the message, the APIC id format beyond 255 cores (refused),
and the card's re-arm behaviour are unseen.

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
  of tolerance). That the firmware really picks the same anchors as the CPU decoder is what the self-test is for; nothing here has seen the firmware. Not done: intensity compensation beyond what the
  firmware does itself, and the interlaced pictures' own verification (below).
- **Interlaced VC-1: partly done in simulation.** Advanced profile only (the sequence flag exists nowhere else). The Core (`vc1::Core`) now takes interlaced sequences: every frame starts with its
  coding mode (FCM: progressive, frame interlace, field interlace), followed - after the picture type, `TFCNTR` and the pulldown bits (`TFF` and `RFF` here) - by `RNDCTRL` and `UVSAMP`, and
  `DMVRANGE` follows `MVRANGE` in a P picture when the entry point has `EXTENDED_DMV`. A *progressive* frame in an interlaced sequence is decoded on the CPU as before, bit for bit like FFmpeg's decoder
  (three new generator streams, `vc1_adv_il_*`: I P B BI and skipped pictures, pulldown bits, post processing, extended DMV range). A frame or field coded picture has no pixel path on the CPU - it is
  refused unless an engine takes the sequence - and the Core reads only as far as the picture class (`FPTYPE` for field coding: a frame whose fields are B or BI is a B frame, one with an intra first field an I
  frame, else P), hands the engine all the frame's units in order (the field start code unit `0x0C` after the frame unit) and manages anchors, references and the output order as for progressive frames.
  `AmdUvd6Vc1Decoder::allow_interlace` lets a session give such a sequence to the engine; `AmdUvd6VideoEngine::enable_vc1_interlaced_decode` sets it after a test of its own: the Advanced profile
  pattern in an interlaced sequence (progressive frames, so the CPU decoder is the reference) must come out right. The message is the progressive one with the `interlace` flag (bit 5 of the sps
  flags, as in Mesa's `get_vc1_msg`); `pic_structure` stays 0 and the firmware is left to read the frame coding mode and the field headers from the frame data, as Mesa does (it never sets the
  `frame_coding_mode` and `picture_type` fields its own `#if 0` lists). What is NOT verified: any frame or field coded picture - there is no VC-1 encoder or reference decoder that makes
  them here (FFmpeg decodes but cannot encode VC-1, and the generator writes only progressive macroblock syntax), so the engine test's stand-in only checks that such a picture arrives whole and in
  order with the flag set (hand-built headers, `tests/Vc1InterlacedFrames.hpp`), and the device assumes a firmware that passes the progressive-frame test decodes them. Whether the surface the firmware
  writes for a field coded frame is the two fields interleaved, as read back here, is a guess. Not done: progressive segmented frames (`PSF`; FFmpeg has none to check against), pulldown (the repeat
  flags are read and ignored), and the other engines (VCN and Intel still refuse interlaced sequences, which the CPU then decodes frame by frame where it can).
- **MPEG-4 Part 2: done in simulation.** `VideoCodec::Mpeg4` (Simple and Advanced Simple profile, as DivX and Xvid make it; the AVI reader maps the `xvid`, `divx`, `dx50`, `fmp4` and `mp4v` tags to it). Unlike every
  other codec here there is no CPU decoder for it, so the engine is the only decoder and this OS reads the headers itself: `Mpeg4Headers` parses the video object layer (size, time base, interlacing, quarter-sample
  motion, quantiser type and matrices, resync marker, data partitioning, low delay) in the order of the standard - checked against FFmpeg's `decode_vol_header` and against streams from FFmpeg's encoder
  (`scripts/gen_mpeg4_test_streams.sh`, `tests/Mpeg4HeadersTest.cpp`, which also cuts and bit-flips headers under the sanitizers) - and each plane's coding type and "coded" flag. It refuses what it does not
  understand, so a stream is taken whole or not at all: sprites and GMC, non-rectangular shapes, more than 8 bits, complexity estimation, new prediction, reduced resolution, scalability, studio profiles, and
  the H.263 short header. `AmdUvd6Messages` builds Mesa's `get_mpeg4_msg` (`struct ruvd_mpeg4`; profile/level 0xF0 and `verid` 5 as Mesa has them, the layer's own size and time base, the matrices in scan order, the
  flags - the ones Mesa's comments list but leaves unset because VA-API does not give them (OBMC disable, data partitioning, reversible VLC, layer identifier, fixed rate) are set from the layer header, which assumes
  the bit positions in those comments are the firmware's) and sizes the picture buffer as `calc_dpb_size` does (three pictures, the 64 and 32 bytes a macroblock of coding-mode and intra-transform surfaces, at
  least 30 MiB). The firmware reads the plane header itself, so the message has no coding type or time stamps. References are job numbers as for MPEG-2: an intra plane names the latest job, a P plane the newest
  anchor, a B plane the older anchor and the newest. `AmdUvd6Mpeg4Decoder` is the session: it keeps the anchors' job numbers and the output order (an anchor is held until the next anchor is decoded, a B plane goes
  out at once, a stream with `low_delay` does not wait) and reads the picture back into I420. Not taken: a packed bitstream (two planes in a packet), a plane that is not coded (the DivX "repeat" marker), a size change
  without `reset()`, five B planes in a row (a reference more than six jobs back). `enable_mpeg4_decode` runs the self-test - 8 pictures of 32 x 32 from FFmpeg's encoder with B planes, held to FFmpeg's decode of them
  within a level or two (`scripts/gen_mpeg4_selftest.sh`); passing is what puts MPEG-4 in `info().decode_codecs`. The engine test's stand-in checks the message by hand-computed offsets and values, the job numbers against
  the planes' types, and writes FFmpeg's pictures in the display order the plane headers' time stamps give (independent of the session's hold logic). Not verified: that the firmware wants these flags and `verid`, and
  how it treats B planes (Mesa never sets the fields that would tell it more) - nothing has run on a card.
- **The video device: done in simulation, opt-in.** `AmdUvd6VideoEngine` (a `SoftwareVideoCodec`, like `AmdVcnVideoEngine`) loads `vegam_uvd.bin`, reads the memory controller and the graphics block's `GB_ADDR_CONFIG`
  (it refuses to start if that reads zero or all ones), plans and maps the memory, starts `AmdUvd6` (three boot tries), runs the decode ring's own test and the same H.264 self-test the VCN engine runs
  (`AmdVcnSelfTest::run_h264_session`: this OS's own 64 x 64 stream, decoded by the engine and the CPU, byte for byte). Only then does `create_decoder` hand H.264 sessions to the engine (one at a time; H.265 the same way after its own test); any failure stops
  the engine and H.264 stays on the CPU, and every other codec is the CPU's. `SystemVideoCodecs::add_amd_uvd6` finds BAR 0 (with its size) and BAR 5 for device IDs 694C/E/F; it is switched on by
  `-DTCOS_AMD_UVD=ON` (off by default, since the start has never touched a real card). `tests/AmdUvd6VideoEngineTest.cpp` runs the whole path against one simulated card (memory controller, graphics block, engine boot,
  ring controller, a stand-in firmware that decodes with the CPU decoder) and ten cases: success (sessions served by the engine, second session on the CPU, shutdown), no firmware, no registers, too small an aperture,
  a mapper that refuses, an engine that never answers, a graphics block that reports nothing, a ring that does not run, an engine that writes wrong pictures, and one that never finishes a picture.
- Still open on the video side: checking interlaced VC-1 pictures against real data, the MPEG-4 tools above, reading the feedback area back, and a run on a real card (a card whose engine stays unpowered makes the start time out; it is stopped and reported).
- Encode rings, medium-grain clock gating, and the 3D side of the card (CP/RLC/SDMA microcode, memory controller and GART, display) are all untouched.
- Not run on a Vega M.
