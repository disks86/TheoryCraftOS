# Dell XPS 15 2-in-1 9575 (Core i7-8705G / i5-8305G, Kaby Lake-G)

Two GPUs on one package: Intel HD Graphics 630 (Gen9.5, PCI `0x591B`) and a Radeon RX Vega M GL (a Polaris-family chip with HBM2, which amdgpu calls VEGAM; PCI `0x694E`, GH is `0x694C`).
The internal panel is wired to the Intel GPU, so the Intel GPU is the boot display and the Vega M is a discrete adapter that `GpuManager` puts to sleep.

| Piece | Intel HD 630 | Radeon RX Vega M |
| --- | --- | --- |
| Identified (`*_gpu_identify`) | yes | yes |
| Boot display / mode setting | `IntelGpu` native path (Gen9.5, unvalidated on hardware) | UEFI GOP only (no Polaris display driver) |
| Video decode | `VideoCodec/Intel` (AVC, HEVC, VP9, MPEG-2, VC-1 via the VCS ring, simulated tests only) | H.264, H.265, MPEG-2 and VC-1 on `AmdUvd6VideoEngine`, opt-in (`TCOS_AMD_UVD`), tested in simulation, **not run on hardware** (below); the other codecs on the CPU |
| 3D acceleration | `Gen9GraphicsDevice` on the render engine, opt-in (`TCOS_INTEL_RENDER`), tested in simulation, **not run on hardware** (below) | none (CPU renderer) |

## Wi-Fi (Intel Wireless-AC 9560, iwlwifi 9000 family)

`src/Hardware/WirelessInterface/Intel/`: **`IwlDevices`** (PCI IDs 0x2526, 0x271B/C, 0x30DC, 0x31DC, 0x9DF0, 0xA370 with their crystal latency / shared clock; the firmware name from the hardware revision and RF id: `iwlwifi-9000-pu-b0-jf-b0-<api>.ucode`, `iwlwifi-9260-th-b0-jf-b0-<api>.ucode`; other MACs, radios and CDB are refused), **`IwlFirmware`** (the `.ucode` TLV file: header, runtime / init / wowlan sections split by the CPU and paging separators, secure sections, flags, API and capability bits, PHY SKU, paging size, firmware version, command versions, image loader; strict length checks) **`IwlFwLoad`** (the 8000/9000-family firmware load: CPU reset released, every LMAC then UMAC section copied by the flow handler's service channel through a DMA bounce buffer in 128 KiB chunks, the extended address space bit for 0x40000-0x57FFF, FH_UCODE_LOAD_STATUS announcements, wait for ALIVE; polled; the 9000 family does **not** use the 22000 series' context-info boot) **`IwlRx`** (the multi-queue receive ring: free / used descriptor rings and status block in DMA memory, buffer ids in the descriptor's low 12 bits, frame walk with 64 byte alignment and the chip's end marker, buffers posted again in groups of eight; frames go to an `IIwlPacketSink`) **`IwlTx`** (the host command queue: 256 TFD ring, first-TB block and command slots, short and wide command headers, doorbell through HBUS_TARG_WRPTR, reclaim of answered commands, scheduler setup after ALIVE) and **`IwlPcie`** (the CSR registers: taking ownership of the card, waking the MAC, APM init / stop, the peripheral and memory windows, interrupts off, the radio switch). Tests: `IwlTest` (synthetic file with every prefix and bit flip, a register model of the chip, and a real firmware file when `IWL_FIRMWARE_FILE` names one).

**Not done:** the bring-up sequence that ties load, queues and the ALIVE notification together, NIC configuration from the PHY SKU, NVM / MAC address, the MVM init sequence, scan, authentication and association, WPA2-PSK (4-way handshake, CCMP), transmit and receive, the `IWirelessInterface` binding. The firmware blob is not in the repository. Nothing has run on hardware.

## Touchpad and touchscreen (HID over I2C)

On this machine neither the touchpad nor the touchscreen is USB or PS/2: both are HID over I2C devices behind the chipset's low power subsystem, described by the ACPI tables as `PNP0C50` / `ACPI0C50`. Without this path the laptop has no pointer.

| File | Job |
| --- | --- |
| `Acpi/AcpiResources` | Parses the resource descriptors of a device's `_CRS` (ACPI 6.4 section 6.4): 32 bit fixed memory, extended interrupt, the GPIO connection (pin, polarity, the controller's path) and the I2C serial bus connection (address, speed, the controller's path); everything else is skipped by its length, every length is checked. |
| `Bus/I2c/DesignWareI2c` | The Synopsys DesignWare I2C master (register map and the clock counts of `i2c-designware-common.c`; polled transfers with the FIFOs, repeated start, abort source). `Bus/I2c/LpssI2c` brings an Intel LPSS function up (PCI 8086:9D60-9D65 of Sunrise Point-LP: D0, memory decoding, the reset release at BAR0 + 0x204, the firmware's `SSCN` / `FMCN` counts and SDA hold). |
| `Bus/I2c/I2cHidDevice` | The HID-over-I2C 1.00 protocol after `i2c-hid-core.c`: the 30 byte descriptor at the register the `_DSM` (UUID `3CDFF6F7-4267-4555-AD05-B30A3D8938DE`, function 1) names, power on, reset (and its zero length answer), the report descriptor, input reports behind a 16 bit length. |
| `Bus/I2c/HidInputMap` | A general HID report descriptor walk (all variable input items with usage, bit position, ranges, units and the finger collection they belong to), bounded everywhere because the descriptor comes from a device. |
| `Bus/I2c/IntelGpio` | The Sunrise Point-LP pads (`INT344B`: communities 0-47, 48-119, 120-151 in the `_CRS` memory ranges, `PADBAR` at +0x0C, two registers a pad, level in bit 1): the device's interrupt pin read as an attention line. |
| `Mouse/I2c/I2cHidPointer` | A Windows Precision Touchpad becomes a relative mouse (16 counts to the millimetre, two finger scroll of a notch per 6 mm, taps: one finger left, two right, three middle; the clickpad button with a right zone; palms ignored by the confidence bit; hybrid frames over several reports), a touchscreen an absolute pointer, a mouse-mode device a mouse. Read when the attention line is asserted. |
| `Mouse/I2c/PlatformI2cHid` | Finds the devices in the AML namespace (`_STA`, `_CRS`, `_DSM`, `_PS0`, `_INI` on the way down), the controller by the path in the I2C connection (`_ADR` of the device: PCI function) and the pin's GPIO controller, and adds each pointer to the system's mouse. `main.cpp` runs it after the other input devices and logs what each device came to. |

`tests/I2cHidTest.cpp` (ASan, UBSan): the controller against a model with FIFOs, aborts and protocol checks (restart bit, overflow); the protocol against a device model (command bytes, reset, stale reports); a Precision Touchpad descriptor walked, cut and bit-flipped; the pointer for motion, scrolling, taps, long and moving touches, the button, a palm, a hybrid frame, the touchscreen, a mouse-mode device, damaged reports and 300 random descriptors; the resource parser; and the whole platform against synthetic AML tables (a controller with `SSCN` / `FMCN`, the GPIO device, two HID devices, a device that is not there, one without a `_DSM`, one on a missing controller, a blacklisted one), a PCI model (function asleep in D3hot) and pads that go low, plus 60 damaged tables.

Not validated on the machine, and not done: the interrupt line is polled (the pin level is read, no GPIO interrupt), power management of the link, sleep and wake of the devices, tap-and-drag and pointer acceleration, pen input, other HID-over-I2C devices (keyboards, sensors), LPSS controllers that the firmware hides from PCI (ACPI-only mode), devices behind other GPIO controllers than `INT344B`. The register offsets of the DesignWare controller are from the databook map (the kernel keeps them in a header I could not fetch); the pad layout and the LPSS reset register are from `pinctrl-intel.c` / `pinctrl-sunrisepoint.c` / `intel-lpss.c`.

## Not done

- **Polaris bring-up**: the SMC microcode load and the GFX 8 command processor start are written and so are the memory controller and GART (see "Radeon RX Vega M: graphics engine start-up" and "... memory controller and GART" below, simulation only). Still missing: DCE 11.2 mode setting (only the surface address is written, see below), the rest of GFX 8. The interrupt handler ring is written ("... interrupt handler" below).
  None of it can be checked without the hardware.
- **UVD 6.3 video decode** for the Vega M beyond H.264, H.265 (8 and 10 bit), MPEG-2, VC-1 (interlaced pictures untested against real data, see below) and MPEG-4 (the MPEG-4 tools listed below; the chip has no VP9), and any run of it on the card.
- **VCE 3.4 video encode** on the Vega M beyond the start and stop (see the VCE section), and any run of it on the card.
- **Hardware 3D** on the Vega M: a GCN4 PM4 path. The shader side exists in `src/Hardware/GraphicsProcessingUnit/AMD/Gcn` (a GCN3 assembler checked against LLVM, a wave emulator, a TGSI parser and TGSI-to-GCN compiler, and `GcnDraw`, which runs compiled shaders on the emulator for a draw call); `GcnState` emits the draw as PM4 packets (shader programs and user data, SPI/CB/PA/VGT state, `DRAW_INDEX_AUTO` / `DRAW_INDEX_2`, cache flush and fence) with register numbers generated from the kernel headers (`scripts/gen_gcn_registers.py`), and `GcnCp` is a model of the command processor that executes such a stream on the emulator. `AmdVegaDraw` puts shaders, constants, descriptors, vertices and the colour buffer in the card's framebuffer area (SDMA upload), submits the stream with `submit_graphics` and copies the picture back; the test runs that whole path against the simulated card, with `GcnCp` standing in for the command processor. The back end is modelled as well: a colour target that is linear or 1D tiled (`GcnTiling`: the GB_TILE_MODE entry names the layout, 8 = ARRAY_LINEAR_ALIGNED, 9 = ARRAY_1D_TILED_THIN1 with display micro tiles), blending (`CB_BLEND0_CONTROL`, constant colour, write mask), a 32-bit float depth plane and an 8-bit stencil plane (`DB_*` registers: tests, three stencil operations, masks), and image / sampler descriptors in the descriptor table (`GcnTexture`: the T# and S# word layouts from the kernel headers, R8 / RGBA8 / RGBA32F 2D images, linear or tiled, wrap / mirror / clamp / border, point and bilinear). Dual-source blending is in: a pixel shader may export `COLOR[1]` as well (export `mrt1`; `SPI_SHADER_COL_FORMAT.COL1` and `CB_SHADER_MASK` 0xFF, `CB_TARGET_MASK` still target 0 only), and the blend factors `SRC1_COLOR / INV_SRC1_COLOR / SRC1_ALPHA / INV_SRC1_ALPHA` (15 to 18) read it; the blend is done after the whole wave so it sees both exports. `AmdVegaDraw` takes these in the request and hands back the colour, depth and stencil planes in their layout. The 2D tiled layouts (ARRAY_2D_TILED_THIN1: macro tiles over the pipes and banks, tile splits, the VEGAM's 16-pipe P16_32x32_16x16 rotation) are implemented in `GcnTiling` from AMD's address library source (egbaddrlib / siaddrlib / ciaddrlib), so the golden table's depth entries (0 to 7) and colour entries 10 and 14 work for colour, depth, stencil and textures (padding to whole macro tiles, base alignment `surface_base_align`, `AmdVegaDraw::area_alignment`); the micro tile pixel orders were checked against `ComputePixelIndexWithinMicroTile`. `scripts/gen_tiling_vectors.cpp` produces reference sizes and addresses from the address library itself for `tests/data/gcn_tiling_vectors.txt` (54 surfaces and 2160 pixel addresses for entries 0-3, 10 and 14 at 8, 32 and 128 bits), which `GcnTextureTest` compares with: sizes, alignments, base alignment and every address agree with the library (the library's output only; the hardware has not been checked). Two sided stencil (front or back by winding and `PA_SU_SC_MODE_CNTL.FACE`) and the depth bounds test (before the stencil test) are in. Images may have mip chains and slices (`GcnTexture`): 3D, 2D array and cube images, a level of detail from `TXL` / `TXB` and the sampler (min / max / bias, none / point / linear mip filter, separate minify and magnify filters, trilinear between 3D slices), the cube lowered by the compiler to `v_cubeid / v_cubesc / v_cubetc / v_cubema` (encodings checked against llvm-mc; the face tables are those of the OpenGL specification), TGSI `SVIEW` targets 2D / 3D / CUBE / 2D_ARRAY. Chains and slices are in all three layouts (linear aligned, 1D tiled, 2D tiled) and for 1D, 1D array, 2D, 2D array, 3D, cube and cube array images (a cube array is a cube with 6 n slices; its third coordinate is the face plus 8 for each layer, as `CUBE_ARRAY` lowers it): the level sizes follow the address library's rules (the base level padded to a power of two when there are several levels, level k from the base pitch shifted down and padded to a power of two, then `surface_pad` for the layout: linear aligned pitch granule max(8, 64 / bytes) widened to a slice of whole 64-pixel units, 1D tiled 8 widened to whole 256 byte slices, 2D tiled whole macro tiles; slice counts up to a power of two), a 2D tiled level smaller than a macro tile is 1D tiled (`level_layout`, the library's ComputeSurfaceMipLevelTileMode), the slices of a 2D tiled level rotate the banks, and a level starts at the base alignment of its layout (Mesa's offset_256B). `GcnTextureTest` compares each level's padded pitch, height, slice count, size, base alignment, layout and the address of pixels in its slices with the library's output in `tests/data/gcn_tiling_vectors.txt`; the offsets between levels are my reading of Mesa. A plain `TEX` has no derivatives in the harness, so it uses level 0 plus the sampler's bias, and a bilinear cube needs DISABLE_CUBE_WRAP (no filtering across faces). Not done: PRT, thick and 3D tiling modes, more than one sample, anisotropy, a bias or lod with a cube array, logic ops (a float target has none), hierarchical Z; none of it has run on the card. On the Intel GPU see below for what is and is not there.
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
the simulated ones do not model caches), has not been seen. No shaders are run on the card (the compiler's output has only run on the emulator), no draws are issued, VMIDs other than 0 and the hardware-queue
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
before disengaging). `AmdGpu`'s own use of it (the back buffer swap, the flush by rows, giving the screen back on a mode change, shutdown or taking the display away) is tested by `AmdGpuTest`, with a simulated UEFI graphics output (two modes) and a simulated display engine that keeps its own picture and takes rows by `upload_picture`; the sequence of drawing calls, the rows each sends (a character at a time for text), the CPU fallback when the upload fails, the whole-picture present before the disengage, a display that refuses or hands out video memory of the wrong size, and swapping one display for another are covered. Decoded video frames are not routed this way yet: the video
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

The CP ECC error (`AmdGfx8::set_ecc_interrupts`: `CP_ECC_ERROR_INT_ENABLE` in the 13 registers of `gfx_v8_0_set_cp_ecc_int_state`) and the SQ messages (`set_sq_interrupts`: `SQ_INTERRUPT_MSG_CTRL.STALL`
cleared) are enabled after the others, in the order of `gfx_v8_0_late_init`, and put back in `gfx_v8_0_hw_fini`'s order. Source 197 is counted; source 239 (`gfx_v8_0_sq_irq`) is decoded: `AmdIrq::SqInfo` has the
encoding (0 general purpose event with its flags, 1 a wave's instruction interrupt, 2 an EDC/ECC error in a wave), the SE, and for waves the SH, CU, SIMD, wave, VM id and privilege bit; for waves the error's source
is read from `SQ_EDC_INFO` of that compute unit (`GRBM_GFX_INDEX` selected for it and put back to broadcast, as the driver's worker does; here it runs in the polling pass, not a work item). Like the driver, nothing
is done about them beyond reporting: an ECC error or SQ report does not wedge a ring.

The display engine's interrupts (`amd_dce11::Interrupts` in `AmdDce11.*`, from `dce_v11_0.c`) are enabled by `bring_up` and dispatched by `AmdIrq`:
- **Page flips** (sources 8, 10, ..., 18, one per controller): `GRPH_INTERRUPT_CONTROL.GRPH_PFLIP_INT_MASK` on for all six controllers (`dce_v11_0_pageflip_interrupt_init`); the handler clears `GRPH_PFLIP_INT_OCCURRED`
  if set, as the driver's does, and counts per controller (`AmdIrq::pageflip_count`). The OS's own flips (at `engage_display` and when the firmware's framebuffer is put back) therefore come back as events.
- **Vertical blank** (sources 1..6, data 0; data 1 is the line interrupt): `LB_INTERRUPT_MASK.VBLANK_INTERRUPT_MASK`. It fires every frame, so it is off until `AmdVegaM::set_vblank_interrupt(true)`, which turns it on for
  the controller that shows the OS's framebuffer, and off again when the display is given back or the card stopped. The handler reads `DISP_INTERRUPT_STATUS*`, acknowledges with `LB_VBLANK_STATUS.VBLANK_ACK` when the bit is
  set and counts either way (`vblank_count`), like `dce_v11_0_crtc_irq`.
- **Hot plug** (source 42, data: the pin): `dce_v11_0_hpd_init` for the pins in `Inputs::hpd_pins` (default all six; which pin a connector uses is in the video BIOS's object table, `AtomConnectors`, but the driver's exclusion of eDP and
  LVDS is not made: every pin in the mask is watched): `DC_HPD_EN`, the toggle filter (50 ms connect, 10 ms disconnect), acknowledge, polarity from `DC_HPD_SENSE`, interrupt on. On an interrupt the pin's status bit must be set (else nothing is done,
  as in `dce_v11_0_hpd_irq`); it is acknowledged, the polarity flipped to report the next change in the other direction (the driver's hotplug work does that when it detects the connector again), and the new state kept
  (`AmdIrq::hpd_connected`, `hotplug_count`). Because a pin that is not what it seems (the internal panel's) could interrupt forever, a pin that interrupts more than `kHotplugStormLimit` (256) times is switched off
  (`hpd_throttled`).
Everything is off again at shutdown (`dce_v11_0_hw_fini`: `hpd_fini`, page flip interrupts off).

`scripts/check_irq_port.py` now also checks these against `dce_v11_0.c` and `vid.h`: the registered source ids, the data decoding, the order of `hpd_init`, the polarity rule, the filter delays, the register block offsets, the
status register table and the acknowledgements.

The video engines' interrupts need no enabling here: the driver turns them on in the engines' own start code, and the ports do the same - `AmdUvd6` start writes `UVD_MASTINT_EN` (`VCPU_EN | SYS_EN`, `uvd_v6_0_start`)
and `AmdVce3::mc_resume` sets `VCE_SYS_INT_EN.VCE_SYS_INT_TRAP_INTERRUPT_EN` (`vce_v3_0_mc_resume`), so the card posts as soon as an engine is started. What was missing was the dispatch (`uvd_v6_0_process_interrupt`,
`vce_v3_0_process_interrupt`): source 124 is the decode ring's fence, 119 and 120 the two encode rings' (`AmdIrq::Kind::UvdFence`, `UvdEncodeFence` with `ring`), and 167 a VCE ring's fence with the ring (0..2) in the data
(`VceFence`; any other data is not a fence). A VCE fence is acknowledged first with `VCE_SYS_INT_STATUS.VCE_SYS_INT_TRAP_INTERRUPT_INT`, as the driver's handler does. The engines' own paths still wait for their fences by
polling, so these are counted (`uvd_encode_fences`, `vce_fences`) and shown to whoever asks: `AmdVegaM::set_event_observer(AmdIrq::ISink*)` passes every event on after the card has dealt with it. Nothing connects the
video engines to the observer yet. `check_irq_port.py` also checks these ids and handlers and that both engines' start code still enables their sources.

The OS reacts to hot plug events like this: `AmdVegaM` keeps the state of the pins it watches (`connected_pins()`: read from `DC_HPD_INT_STATUS.DC_HPD_SENSE` at bring-up, then updated by every event) and queues an event
for each plug and unplug (`take_hotplug()`: the pin, whether a display is connected now, and a flag on the event that switches a bouncing pin off; the queue holds 16 and drops the oldest, counting them). The desktop loop
drains the queue after `service_interrupts()` and, for each event, writes a line to the debug console and shows a toast ("A display was connected to HDMI-A-1 of the Radeon RX Vega M"; "Hot plug detection of DP-1 on the Radeon RX Vega M is switched off: it
kept changing" as a warning); at boot the outputs that already have a display are logged. That is all there is to do about it for now: the card's outputs are not part of the desktop (the OS cannot put a
picture on them: no mode setting without the AtomBIOS command tables for the encoders and PLLs, and no DDC/EDID read of the monitor on the other end), so a plug cannot add a monitor to the desktop's layout. Which connector a pin is
on comes from the video BIOS (next section); without one, or for a pin it does not list, the text says "output 2". Not done: the audio interrupts; recovery from a fault is above.

The vertical line interrupt is enabled like the vertical blank: `AmdVegaM::set_vline_interrupt(true)` sets `LB_INTERRUPT_MASK.VLINE_INTERRUPT_MASK` (bit 4, `dce_v11_0_set_crtc_vline_interrupt_state`; `amd_dce11::Interrupts::set_vline`) of the
controller that shows the OS's framebuffer, and only that one; it is refused without an engaged display, independent of the vblank bit (neither touches the other), and switched off when the display is given back or the card
stopped. The handler acknowledges with `LB_VLINE_STATUS.VLINE_ACK` when `DISP_INTERRUPT_STATUS*` says it is pending, and counts per controller (`AmdIrq::vline_count`). Like the driver, nothing programs which line it comes at.
It has no user in the OS yet (nothing needs a mid-frame callback); a test (`test_vline_interrupt`) covers enable, bits kept, independence from vblank, ack, counting and the off paths.

The DisplayPort short pulse (a sink's status change that is not a plug: link loss, a request to retest) arrives as source 42 with data 6..11 (`VISLANDS30_IV_EXTID_HPD_RX_A..F`, pins 0..5), `AmdIrq::Kind::DisplayHpdRx`.
`AmdVegaM::bring_up` switches it on for the watched pins after `hpd_init` (`DC_HPD_INT_CONTROL.DC_HPD_RX_INT_EN`, a stale pulse acknowledged first) and `hpd_fini` off. A vector counts when `DC_HPD_INT_STATUS.DC_HPD_RX_INT_STATUS`
says it is pending; it is acknowledged (`DC_HPD_RX_INT_ACK`), counted per pin (`AmdIrq::hpd_rx_count`), shown to the event observer, and is not a hot plug (no `HotplugEvent`, `connected_pins` unchanged). A pin that pulses more
than `kHpdRxStormLimit` (256) times has the interrupt switched off (`hpd_rx_throttled`). **Provenance:** the ids come from `ivsrcid_vislands30.h`, the bits from `dce_11_2_sh_mask.h` (both checked by
`check_irq_port.py`), but the kernel's `dce_v11_0.c` never uses the pulse; the way to enable, read and acknowledge it is that of the DC display code (`dce110_irq_service.c`), written here from memory of it, not checked
against its source. **What is not done with it:** a real handler reads the sink's DPCD over the AUX channel (link status, automated test request) and retrains; there is no AUX channel or mode setting here, so the pulse is only
counted.

### Audio endpoints

`amd_dce11::AudioPins` (`dce_v11_0_audio_endpt_rreg`, `dce_v11_0_audio_get_connected_pins`) reads the display engine's eight audio endpoints (CHIP_VEGAM, like Polaris 10): each has an index/data register pair
(`AZALIA_F0_CODEC_ENDPOINT_INDEX`/`DATA`, 0x17a8/0x17a9 plus 4 per endpoint) and, at index 0x56, its default pin configuration, whose port connectivity field (bits 31:30) is 1 when the endpoint has no physical
connection; any other value counts as connected, as in the driver. `AmdVegaM::audio_pins_connected()` is that mask as of bring-up (`refresh_audio_pins()` reads again). **Which output an endpoint serves is not fixed:** the driver
hands an encoder the first connected endpoint (`dce_v11_0_audio_get_pin`), and the encoder-to-endpoint link is made only when a mode is set, which this OS does not do. So the mask says how many displays could have audio
and the connector names come from the other side: `audio_outputs()` lists the connectors that carry audio (HDMI-A, HDMI-B, DisplayPort; `AtomConnectors::carries_audio`) and have a display on their hot plug pin now. The boot
log shows both ("audio: 5 of 8 endpoints with a port", "audio output with a display: HDMI-A-1"). This is a reading of the hardware's state, not an audio path: nothing here drives the endpoints (the card's HD Audio function is
a separate PCI device, polled by the HD Audio driver). `scripts/check_irq_port.py` checks the endpoint count, offsets, register pair order and the connected rule; `test_audio_pins` covers the mask, the names, a display leaving,
refresh, shutdown, no video BIOS and out-of-range endpoints under the sanitizers. Nothing seen on the real machine: the endpoints' default configuration may read as "connected" for all eight until a mode is set.

### Connector names from the video BIOS's object table

`AtomConnectors` (`AtomConnectors.*`) reads what the card's outputs are: a port of the connector part of `amdgpu_atombios_get_connector_info_from_object_table` with `amdgpu_atombios_lookup_gpio` and
`_get_hpd_info_from_gpio`. The Object_Header data table (revision 2 or later) has the display path table, one path for each device the board supports (a bit of the header's device vector), ending in a connector
object; the connector table's object for it has records, the hot plug interrupt record names a GPIO, the GPIO_Pin_LUT data table says which register bit that GPIO is, and a bit of `DC_GPIO_HPD_A` (0x488d: bits 0, 8, 16,
24, 26, 28) is hot plug pin 0 to 5. The connector object id gives the type as `object_connector_convert` does (DVI-I, DVI-D, VGA, Composite, SVIDEO, DIN, HDMI-A, HDMI-B, LVDS, DP, eDP; the others and the TV and
component video devices are skipped). Names are the kernel's: the type and the number among connectors of that type (`HDMI-A-1`, `DP-2`, `eDP-1`). A connector that several paths end in is one connector with the devices
merged, as `amdgpu_connector_add` does.

Differences from the driver, because the image is untrusted bytes: every read is checked and a table that runs past the image ends the walk with what was found kept, the paths and records walked are bounded (255, 64),
at most 16 connectors are kept, and the parts that are not about naming are left out (the encoders' capabilities, routers; the encoder objects of a path are read, see "Encoder objects"). `AmdVegaM` parses the table at bring-up when `Inputs::vbios` is set
(`connectors()`), and each `HotplugEvent` carries its connector (`has_connector`, `connector`); `main.cpp` writes the name in the toast, on the debug console and in the boot log.
`scripts/check_connectors_port.py` checks the master list indexes, the layouts of the object header, path, object, record and GPIO entry, the pin masks and the connector type of each object id against the driver;
`tests/AtomConnectorsTest.cpp` has a hand-assembled XPS-like board (eDP, HDMI, two DisplayPorts), all six pins and the GPIO entries that are not pins, the skipped devices and unnamed objects, records before and after the
hot plug record, merged paths, the capacity, the names, and every truncation, every bit flip and 2000 noisy images under the sanitizers. Not checked against the real thing: `ObjectID.h` (the object id masks and the
numbers are those of the Linux header from memory, checked only through `object_connector_convert`'s order), and no real Vega M video BIOS has been read. Not done: DDC lines and the encoder/router parts, and
Atom firmware-info objects of the newer (Vega and later) `atomfirmware.h` layout, which differs.

### Monitor EDID over the DDC lines

The connector reader now also keeps each connector's DDC line (`Connector::ddc_id`, from the I2C record of its object: record type 1, the access byte at +2; the last record wins as in the driver). `AtomI2c::lookup` finds that
id in the GPIO_I2C_Info data table (index 10; 27-byte entries: the clock's mask, enable, readback and output registers, the data pad's four, the id, the eight bit positions) and makes the bus record
(`amdgpu_atombios_lookup_i2c_gpio`, `_get_bus_rec_for_i2c_gpio`). `AmdDdc` drives the two pads from software through those registers and reads the EDID: the i2c bit-banging algorithm with the 10 us bit time and 2.2 ms
clock stretch limit amdgpu sets, then `drm_do_get_edid`'s sequence - offset 0 written to 0x50, a repeated start, 128 bytes read; the header and checksum checked, four tries per block; the extension blocks the same way (the
segment pointer written to 0x30 from block 2 on), at most three, a block that never checks left out and the base block's count and checksum made true again. The driver's default is software DDC too (its hardware I2C engine
is a module parameter, off). `AmdVegaM::read_edid(connector, &bytes)` does it for a connector; `main.cpp` reads the monitor on each new connection and at boot, parses it with the existing EDID parser, and the toast says
"A display (DELL U2720Q 2560x1440) was connected to HDMI-A-1 of the Radeon RX Vega M". It takes the bus for some tens of milliseconds, so it runs from the desktop loop and not in the interrupt path.

**Not done / not known:** DisplayPort and eDP connectors carry the EDID over the AUX channel, not GPIO pins - see the next section. `amdgpu_i2c.c` (pre_xfer, post_xfer and the pad callbacks) was compared with v6.12 afterwards (outputs cleared, pins made
inputs, pads masked for software with a read-back, a pin "high" when its output is not enabled, released again afterwards, and for a hardware capable line bit 16 of the clock mask register cleared first: added then; udelay 10 and the 2200 us timeout match); `i2c-algo-bit.c` is followed from memory, so `check_i2c_port.py` checks the table and entry layout, which register
is which, the id bits and the lookup against the driver but not the pad callbacks. Tested against a bit-level simulated bus (`tests/FakeI2cMonitor.hpp`: a monitor as an I2C slave, written independently; `AmdDdcTest`): the wire
(addresses, offsets, segment pointers, one stop per transfer), the registers left as found, the pads masked while the pins are driven, pins left pulled low, retries, a damaged block, an absent monitor, clock stretching within and past
the limit, the time taken, and every bit flip of the data; `AtomI2cTest`, `AtomConnectorsTest` and `AmdVegaMTest` (`test_read_edid`) cover the table, the record and the card. The simulated slave is not a real monitor: whether a real
one, through this machine's pads, answers is unseen.

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

### Recovery after a fault

An engine that timed out on a fence or reported a fault is wedged (nothing is queued on it). `AmdVegaM::recover()` resets the ones that are and brings them back with empty rings; `Recovery` says whether graphics
(with the compute queues) and the SDMA engines were reset and whether an engine did not come back. With `Inputs::auto_recover` (the desktop loop sets it) `service_interrupts()` does it by itself, at most
`kMaxRecoveries` (8) times in a life of the card, so a card that keeps hanging is left alone; `recover()` by hand has no limit.
- **Graphics and compute** (`AmdGfx8::recover`, from `gfx_v8_0_check_soft_reset` / `_pre_soft_reset` / `_soft_reset` / `_post_soft_reset`): the status registers decide what is reset (`GRBM_STATUS` busy bits: CP and GFX;
  `GRBM_STATUS2`: the RLC, or CPF/CPC/CPG; `SRBM_STATUS`: pending GRBM requests, SEM); since a fence that never came shows nothing busy, the command processors are added when the status does not name them (the driver
  would reset only what is busy; recovery from a wedged ring has to work whatever the status says). Pre: the RLC stopped, the graphics CP halted, every compute queue's hardware queue dequeued directly through its
  registers (`gfx_v8_0_deactivate_hqd(adev, 2)`: the KIQ is not asked, it may be what hung) and the MEC halted. Reset: the memory controller's graphics clients stalled (`GMCON_DEBUG`), `GRBM_SOFT_RESET` and
  `SRBM_SOFT_RESET` pulsed with the 50 us settling delays, the stall released. Post: the queues dequeued again, the KIQ and compute queues resumed and mapped (`kiq_resume`, `kcq_resume`), the graphics ring
  programmed, all rings tested, the RLC started. The fence numbers start again (the write-back fence words are cleared so an old number cannot look like a new fence), the wedged state is gone, and the interrupt sources
  the reset takes away (the CP's, ECC, SQ, the compute pipe's) are put back.
- **SDMA** (`AmdSdma3::recover`, from `sdma_v3_0_pre_soft_reset` / `_soft_reset` / `_post_soft_reset`): context switching off and the engines halted, `SRBM_SOFT_RESET` for both engines pulsed, then `start()` again on the
  same memory (the driver's `gfx_resume` and `rlc_resume`); both engines are reset together, as in the driver, and the trap interrupts are put back.
- A fault of one compute queue resets all of them with the graphics engine, as the driver's soft reset of the IP does. An engine whose ring test fails after the reset is left stopped (`failed`); it counts as needing
  recovery, so the next `recover()` tries it again.

The simulated engines hang (`hung` for the command processor, `stall` for SDMA) until the reset pulse reaches them, unless told the reset does not work; the reset also takes their interrupt enables, hardware queues and
doorbells away, so the ordering of the steps, the exact reset bits for each status, and what is restored are checked. `scripts/check_reset_port.py` compares the status bits, reset bits, pre/soft/post order, the reset pulses,
the queue dequeue and the SDMA sequence with the kernel sources. Not done: resetting the video engines (UVD/VCE), and the replay of the work that was lost: callers see the failure of their
submission and must resubmit. Whether a real CP comes back from `GRBM_SOFT_RESET` with the microcode the SMC loaded is what the driver relies on and has not been seen here.

### Full reset of the card

When an engine does not come back from its soft reset, `AmdVegaM::recover()` can fall back to resetting the whole card (`Inputs::allow_asic_reset`, with `Inputs::pci` and `pci_address` saying where the card is;
`Recovery::card_reset` and `asic_resets()` say it happened, at most `kMaxAsicResets` (2) times). `AmdAsicReset::pci_config_reset` is `vi_asic_reset` -> `vi_asic_pci_config_reset`: the BIOS scratch register's
"graphics engine hung" flag is set, bus mastering is switched off, the magic value 0x39d5e86b is written to dword 0x7c of the card's PCI configuration space (`amdgpu_device_pci_config_reset`), after 100 us the card has
`usec_timeout` (100 ms) to answer reads of `CONFIG_MEMSIZE` again (all ones while it is in reset), bus mastering is switched back on if it did (not if it did not), and the flag is cleared either way. The VEGAM has
no other method: the driver's BACO (power-off through the SMU) is only for Fiji, Tonga, Polaris 10, 11 and 12 and Topaz. `reset_card()` does it with the rest around it: everything is stopped (the display
gets the firmware's framebuffer back), the card is reset, and `bring_up` is run again with the same inputs; `status()` says what failed ("card reset: ..." if the card did not answer, "after a card reset: ..." if
the bring-up failed afterwards).

**What this does not do, and what that means on a real card.** After a hardware reset the driver marks the card as needing a POST and runs the video BIOS's init tables (the memory controller's training, the
clocks, the display PLLs) through its AtomBIOS interpreter, then initialises every block. This OS now has the interpreter (above) but no way yet to get the video BIOS image, so a card that has been reset comes back quiet - no engine, no DMA, bus mastering on or off as
above - but its video memory is not trained, `bring_up` is expected to fail, and everything in video memory (including the firmware's framebuffer) is gone. The reset is therefore a last resort, off by default, and
`main.cpp` allows it only when the card does not drive the display (a dark screen on top of a dead card is no improvement over a hung engine); the benefit is a card that is known to be quiescent instead of one that
is hung and may still be mastering the bus. Host tests (`AmdAsicResetTest`, `AmdVegaMTest::test_card_reset`) cover the step order and timing, a slow card, a card that never answers, the fallback conditions and the
cap, a card that is brought up again, and one that cannot be (not POSTed); the simulated card has none of the real one's behaviour here. `scripts/check_asic_reset_port.py` checks the steps, the value, the
dword, the scratch register and flag, `CONFIG_MEMSIZE` and that the VEGAM is not a BACO chip against the kernel sources. Not done: finding the video BIOS image (which, with the interpreter, would make the reset recoverable), the PCI-level resets
(function level reset, secondary bus reset) the driver also has for other cases, and BACO.

### AtomBIOS interpreter and the POST after a card reset

`AtomBios` (`AtomBios.*`) is a port of the Linux driver's `atom.c`: the interpreter for the byte code in an AMD card's video BIOS, by which the card is POSTed (the memory controller trained, the clocks and PLLs set
up) and, in the driver, modes are set. The image holds a ROM header, a list of command tables (small programs with a parameter space and a workspace) and a list of data tables. Operations have operands that are
registers (memory-mapped, or reached by an indirect I/O program of the BIOS's own), the table's parameters or workspace, the BIOS's scratch memory, its data tables, immediates, and PLL and memory controller registers, as
whole dwords or a byte or word of one; there are moves, arithmetic, masks, shifts, compares and jumps, a switch, delays and calls of other tables. `IAtomHost` is the driver's `card_info` plus the time: the registers,
the memory controller and PLL (a discrete card has neither through the BIOS; the host returns zero, like `cail_mc_read`), delays and a clock. `asic_init()` is `amdgpu_atom_asic_init`: command table 0 with the firmware
info's default engine and memory clocks as its first two parameters.

The image is untrusted bytes, so where the driver reads through pointers this checks every read (a read beyond the image stops the run), checks the scratch memory exactly (the driver lets a read one dword past its
end through), limits tables calling tables to 16 deep, limits the number of operations (50 million) as well as keeping the driver's rule that a loop which jumps back to the same place for 20 seconds is stuck, avoids C's
undefined shifts by 32 or more, and keeps the workspace on the stack. Where the driver prints and carries on (a parameter or workspace index out of range, PCI or SYSIO register access, an indirect I/O method that does not
exist) so does this, and `warnings()` counts them. One of the driver's oddities is ported as it is: a write through an indirect I/O port looks up the program with `io_mode & 0xFF`, a read with `io_mode & 0x7F`.
Not ported: the name, date, part number and version strings, and the parsing of the other data tables.

How it was checked, in three ways. `tests/AtomBiosTest.cpp` has hand-assembled images (`tests/AtomBiosImage.hpp`) for every operation with results worked out by hand, the alignments, the compare and jump conditions,
switches, calls and parameter passing, the I/O ports, the scratch memory, the limits, the parser's refusals, and truncated and bit-flipped images under the sanitizers. `scripts/atom_differential/run.sh` compiles the
Linux driver's own `atom.c` in user space (with stubs for the kernel headers) and runs random but well formed programs - every operation, operand kind and alignment, indirect I/O programs, switches, calls - through both
interpreters against identical simulated cards, comparing the result, every register, memory controller and PLL access and delay in order, the parameters and the scratch memory: 30,000 programs and a million accesses agree.
It found what the unit tests could not: the driver's negative parameter-space size for a callee of a callee, a null pointer dereference in its part-number parsing, undefined shifts in the indirect I/O code. `scripts/check_atom_port.py`
checks the constants, the alignment tables, the generated opcode table (`scripts/gen_atom_ops.py` makes `AtomBiosOps.inc` from the driver's `opcode_table`) and the layout of the data tables.

`AmdVegaM::post()` runs the POST with `Inputs::vbios`, and `reset_card()` does it between the reset and the bring-up when the image is given. The simulated card here is "trained" by a marker write in the video BIOS's table.
**Where the video BIOS image comes from.** `AmdVbios` (`AmdVbios.*`) finds it, following `amdgpu_bios.c` in the order of `amdgpu_get_bios_dgpu`: (0) the ACPI ATRM method, an AML method (`ATRM(offset, length)`, returning a buffer of the ROM's bytes) of the card's device in the firmware's tables,
called for pages of 4096 bytes from offset 0 until one comes back short, into a 256 KiB image (`read_from_atrm`; see below); (1) the ACPI VFCT table, which holds the firmware's copy of the
video BIOS images of its AMD cards - `find_in_vfct` walks the images (28-byte headers: PCI bus, device, function, vendor and device ID, length) and takes the one for this function, as a view into the table;
(1a) the copy at the start of video memory (`read_from_vram`, `amdgpu_read_bios_from_vram`): a card that has been POSTed has its video BIOS image at the start of the memory BAR, 256 KiB of it (the driver's own guess at
the size). Only tried when the card needs no POST (`amdgpu_device_need_post` for CIK and later is `BIOS_SCRATCH_7` having `ATOM_S7_ASIC_INIT_COMPLETE`, 0x200 of register 0x5d0, which the register header gives) and the BAR is at
least 256 KiB (the driver maps that much whatever the BAR says); (1b) the legacy shadow at 0xC0000 (`read_from_legacy_shadow`, `amdgpu_read_platform_bios`: 128 KiB copied, the driver's `pdev->rom`/`romlen` that the PCI core sets for the boot display's shadowed ROM) - but the driver
knows from the PCI core that the shadow is the card's and this does not, so the image must name the card in its PCI data structure (vendor and device ID, `rom_ids`); the integrated graphics' shadow, which is what a laptop with
two cards normally has there, is refused; (2) the PCI expansion ROM - `read_from_rom_bar` probes the ROM base register for the window size, switches the ROM on at the base the firmware assigned (no base, no ROM), maps the window, copies it,
switches it off, and trims the copy to the chained images (`rom_size` is the PCI core's `pci_get_rom_size`: each image gives its length in 512-byte units and whether it is the last); (3) the card's own ROM through the SMC's
index and data registers (`vi_read_bios_from_rom`: header first, checked for 0xAA55 and the AMD string, then the length it names). Every image passes `check_atom_bios` (0xAA55, a header pointer at 0x48, "ATOM" or "MOTA" at
its offset 4). `main.cpp` looks only when the card may be reset (otherwise nothing would use the image) and `AmdVegaM::bring_up` keeps its own copy, so the buffers need not outlive the call. **The ATRM method** runs on the repository's AML interpreter (`acpi::AmlInterpreter`) with the DSDT and SSDTs loaded; `main.cpp` makes one for the look and drops it afterwards (the machine's
operation regions are reached through `SystemAcpiHost`, as for the battery). The driver takes the first display device on the PCI bus whose ACPI node has an `ATRM`; the namespace here has no PCI enumeration, so
the device is found by walking it: first a device that has an `ATRM` method of its own and whose own `_ADR` is the card's (`device << 16 | function`), then, as the driver would, the first device that has one. The walk is
bounded (4096 nodes, 16 deep). Where the driver copies whatever length the method returns, a page that is not a buffer or is longer than the 4096 asked for ends the reading; the image is checked with `check_atom_bios` as
the driver does. Not done: the `_INI` methods are not run before the call, and the method is not given the PCI bus (the `_ADR` match cannot tell two cards at the same device and function on different buses apart).
**The disabled ROM** is the last resort (`read_from_disabled_rom`, `vi_read_disabled_bios` then `amdgpu_read_bios`): for a card whose firmware switched its expansion ROM off, `BUS_CNTL.BIOS_ROM_DIS` is cleared,
the VGA mode and timing select of the first two display controllers (`D1VGA_CONTROL`, `D2VGA_CONTROL`) and `VGA_RENDER_CONTROL`'s status select are cleared (only if the card has display controllers,
`Sources::display_engine`, true for the Vega M), `ROM_CNTL` (an SMC register, through the index/data pair) gets `SCK_OVERWRITE`, the expansion ROM is read as in (2), and every register is put back in the same order,
found or not. It needs the register BAR, the PCI function (a ROM base must have been assigned) and the mapper. `scripts/check_vbios_port.py` (with the three optional register headers) checks the order of the writes and the
register numbers and masks against `vi.c` and the headers; the test has a card whose ROM answers only once all of it is unlocked (a fake that is stricter than hardware may be, on purpose), checks the exact writes with and
without display controllers, the restore after a failure, and the place in the order. Every source of `amdgpu_get_bios_dgpu` is now there. **Still unseen:** whether any of this machine's sources is the one that works.
Tested with hand-assembled AML for the ATRM method (a short page, a page and a short one, two devices told apart by `_ADR`, a name that is not a method, a method on something that is not a device, failing and wrong-typed methods, an endless supply of pages, every truncation and flipped bit of the table), a simulated VFCT table (several cards, truncations, bit flips in every header byte), an expansion ROM behind a working ROM base register, and the SMC pair (`AmdVbiosTest`), and `scripts/check_vbios_port.py` checks
the signatures, the order, the VFCT layout and the register protocol against the kernel. **Still not seen:** the real machine's VFCT or ROM: whether this machine has a VFCT entry for the Vega M, and whether its table 0 trains the memory.

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

### Monitor EDID of DisplayPort and eDP over the AUX channel

DisplayPort and eDP connectors (most of what a laptop has) have no DDC pins: their EDID is read as I2C over the AUX channel. The AUX transaction on the wire is done by the video BIOS: the AtomBIOS command table
`ProcessAuxChannelTransaction` (index 78 of the master list; `AmdAux::kCommandTable`). `AmdAux` is the port of `amdgpu_atombios_dp_process_aux_ch` and `_aux_transfer`: the request (address, command and length
bytes, then the payload) is written to the interpreter's scratch memory at byte 4, the table is run with the parameters of `PROCESS_AUX_CHANNEL_TRANSACTION_PARAMETERS_V2` (request pointer 4, data-out pointer 20, the
connector's DDC id as the channel, the hot plug pin as `ucHPD_ID`), and the reply status (1 timeout, 2 busy, 3 error, otherwise the DisplayPort reply command in the high nibble) and the data length are read back with
the payload from byte 20. The table takes at most 12 bytes of payload in a write (the request length is four bits) and 16 in a read. On top of it is the DRM helper's I2C-over-AUX adapter (`drm_dp_i2c_xfer`,
`_drain_msg`, `_do_msg`): each message starts with an address-only request with the middle-of-transaction flag, the data goes in requests of at most 16 bytes (a short reply goes on from there), the transaction is closed
with an address-only request without the flag; a native or I2C defer is waited out (500 us) and retried (7 runs, one more for each I2C defer up to 7), busy is retried, a timeout, an error and a NACK end it. It is an
`II2cBus`, so the EDID is read by the same code as over the DDC pins (`i2c_edid::read`, split out of `AmdDdc`). `AmdVegaM::read_edid` uses it for DisplayPort and eDP connectors: it builds an `AtomBios` for the video BIOS
(the interpreter used for the POST, with the same host) and needs the connector's DDC id to be in the GPIO table, as the driver does. `AmdAux::dpcd_read` reads the sink's DPCD with native requests (nothing uses it yet).

**Not done / not known:** `amdgpu_atombios_dp.c` and `drm_dp_helper.c` were not at hand: the table's parameter layout and index are checked against `atombios.h` (`scripts/check_aux_port.py`) but the request
framing (the length nibble, 3 for an address-only request and 4 plus the payload otherwise, 16 as 0), the limits of the table (12 and 16 bytes), the delay (0) and the retry rules are from memory. The panel's power of an
eDP connector is switched on first (`AmdPanelPower`, below). The DPCD is not used (no link training, sink capabilities or power state). Tested with a sink written independently from the AUX wire format (`tests/FakeAuxSink.hpp`, behind a video BIOS whose table 78 passes the request to the simulated
card: `AmdAuxTest`, `AmdVegaMTest::test_read_edid`): the requests on the wire, the table's parameters, native reads and writes, the EDID with its extension blocks and segment pointer, defers, busy, timeouts, errors, NACKs,
short replies, a table that reports more bytes than were asked for, an image without the table, and random replies under the sanitizers. Whether a real sink answers is unseen.

**eDP panel power.** A panel that is off does not answer on the AUX channel. `amdgpu_atombios_encoder_set_edp_panel_power` runs the command table `UNIPHYTransmitterControl` (= `DIG1TransmitterControl`, table 76; `DIG_TRANSMITTER_CONTROL_PARAMETERS`, 8 bytes: `usPixelClock`, `ucConfig`, `ucAction` in
byte 3, four reserved) with `ucAction` = `ATOM_TRANSMITTER_ACTION_POWER_ON` (12) and then polls the connector's hot plug pin once per millisecond for up to 300 ms for the panel to show; on a timeout it logs and goes on.
`AmdPanelPower::power_on` does the same (the pin is sensed through `IHpdSense`, here the display engine's `DC_HPD_INT_STATUS`; a connector without a pin does not wait; a timeout returns false), and
`AmdVegaM::read_edid` runs it for eDP connectors before the first AUX access and tries the AUX channel whatever it returns. The panel is left on (`power_off` exists, nothing calls it: the driver switches the panel
off with the display). The table index, the parameter layout and the two action values are checked against `atombios.h` by `scripts/check_aux_port.py`; `tests/AmdPanelPowerTest.cpp` (the action in the parameters, the wait
to the millisecond and the 300th sense, a pin that never shows the panel, no pin, an image without the table) and `AmdVegaMTest::test_read_edid` (a panel that answers only after the power-up, which only eDP gets) cover it.
The driver source for this function was not at hand: the 300 ms / 1 ms polling and the use of only `ucAction` are from memory. Also unseen on hardware: the time a real panel needs (the T1+T2 power sequencing is the table's).

### Encoder objects: connector to DIG encoder to audio block

The display path table lists, after the connector and GPU object ids of a path, the object ids of the encoders from the GPU to the connector (`usGraphicObjIds`). `AtomConnectors::parse` keeps the first object of the
encoder type (type 2 in bits 14:12 of the id) as `Connector::encoder_object` (the GPU's own encoder, a UNIPHY transmitter) and a second one as `bridge_object` (a bridge chip such as NUTMEG or TRAVIS); at most eight ids of a
path are walked, and a connector of several paths keeps the first path's encoders (a later path fills them in if the first had none). From the encoder object: the id (`encoder_id`, low byte), the enumeration (bits 10:8;
2 means the transmitter's link B, `link_b`, as amdgpu's `dig->linkb`), and from those `dig_encoder()` = `dce_v11_0_pick_dig_encoder` (UNIPHY: 0 / 1 for links A / B, UNIPHY1: 2 / 3, UNIPHY2: 4 / 5, UNIPHY3: 6; `kNoDig`
for any other encoder). `afmt_instance()` is `dce_v11_0_encoder_prepare`'s audio format block, `afmt[dig_encoder]`, only for the digital flat panel devices (DFP1..DFP6: HDMI and DisplayPort, not the internal LCD).
`AmdVegaM::audio_pin_for(connector, &pin)` then gives the endpoint the driver would hand that block: `dce_v11_0_audio_get_pin`, the first connected endpoint (so the same one for every connector: the driver does not
tell two displays apart, and neither does this; `AFMT_AUDIO_SRC_CONTROL` is not written here, there is no audio output to program yet).

Checked by `scripts/check_encoder_port.py` against `dce_v11_0.c` and `atombios.h`: the UNIPHY-to-DIG table, the audio block rule, the DFP device mask, the path layout. **Not checked** (`ObjectID.h` was not at hand, the file
was empty): the encoder object numbers (UNIPHY 0x1E, UNIPHY1 0x20, UNIPHY2 0x21, UNIPHY3 0x25, NUTMEG 0x22, TRAVIS 0x23), the id / enumeration / type masks, the walk of the path's ids in
`amdgpu_atombios_get_connector_info_from_object_table`, and that enumeration 2 is link B. Not done: the encoder object table of the object header (capabilities of the encoders), the DIG's use for mode setting, and the
bridge's own handling (`amdgpu_encoder_get_dp_bridge_encoder_id`). `tests/AtomConnectorsTest.cpp` (every transmitter and link, merged paths, the object id limit, bridges, device masks, the truncation and bit-flip sweeps
over a board with encoders) and `AmdVegaMTest::test_audio_pins` (the XPS-like board's DIGs and the first-connected-endpoint rule as the endpoints change) cover it.

### The sink's DPCD over the AUX channel

A DisplayPort sink describes itself in the DisplayPort Configuration Data (DPCD), a register space read with native AUX requests (`AmdAux::dpcd_read`, up to 16 bytes at a time). `Dpcd` (behind the `IDpcdAux`
interface that `AmdAux` implements) reads and parses two parts of it. **Capabilities** (`Dpcd::read_capabilities`): the receiver field 0x000..0x00F - the DPCD revision (0x10 is 1.0 .. 0x14 is 1.4), the fastest
link rate per lane (`max_link_rate_code` in 0.27 Gbit/s units: 0x06, 0x0A, 0x14, 0x1E are RBR, HBR, HBR2, HBR3; `max_link_rate_kbps()`), the most lanes (1, 2 or 4) with the TPS3 and enhanced framing bits, the
downspread and AUX-handshake bits, the number of receiver ports, the downstream port (a dongle's type, format conversion, port count, OUI support) and the 8b/10b coding bit - and for an eDP connector the panel's DPCD
revision (0x700) and, from eDP 1.4, its own table of up to eight link rates (0x010.., 16-bit, 200 kHz units, ended by a zero). Bytes that are no DPCD (a revision below 1.0, no rate, a lane count other than 1, 2 or 4)
are refused, so a bus that reads zeros or ones is not taken for a sink. **Link status** (`Dpcd::read_link_status`): 0x200..0x207 - the sink count (7 bits, the high one in bit 7), the service interrupt vector
(automated test request, content protection, sink specific), the training status of each lane, the lane alignment and its link-status-updated and downstream-port-changed bits, the sink status and the adjust
requests; `clock_recovery_ok(lanes)` and `channel_eq_ok(lanes)` are `drm_dp_clock_recovery_ok` / `drm_dp_channel_eq_ok`. This is what a driver reads before it trains the link and when the sink pulses its hot plug
pin (the short pulse, see "Hot plug"). `AmdVegaM::read_dpcd(connector, &caps, &status)` reads them for DisplayPort and eDP connectors through the same AtomBIOS AUX session as `read_edid`, with the eDP panel
power-up first (`power_up_panel`, shared).

**Not checked:** all of the register addresses and bit meanings are from memory of the DisplayPort specification and the DRM helper's `drm_dp.h`, neither of which was at hand; there is no port-check script for this
reason. Not done: writing the DPCD (link rate and lane count settings, training patterns, the sink's power state `DP_SET_POWER`), link training itself, the sink's OUI and the branch device's, the MST registers, the eDP backlight
and PSR registers, and acting on the short pulse (reading the link status when it comes). `tests/DpcdTest.cpp` (hand-built images from the specification's register map: an HBR2 4-lane monitor, a dongle, an eDP 1.4 panel
with its rate table, each flag and field alone, sinks that are no DPCD, reads that fail partway, every bit flip of the images and every truncation under the sanitizers) and `AmdVegaMTest::test_read_dpcd` (the XPS-like board's
DisplayPort and eDP connectors through the video BIOS tables, with a panel that answers only after the power-up) cover it.

### The DisplayPort short pulse: link status and test requests

A DisplayPort sink pulses its hot plug pin briefly (the short pulse, source 42 data 6..11, see "Hot plug") to say that something changed that is not a plug: the link lost sync, a stream was lost, or it wants a test.
The interrupt handler only takes note: `AmdVegaM::on_event` sets the pin's bit in `pending_short_pulses()` (a pulse whose status bit is not set never gets that far, `AmdIrq` drops it). Looking at the sink is the AUX
channel's job, which takes some milliseconds, so `service_short_pulses(out, capacity)` does it from the desktop loop. For each pending pin, lowest first, up to `capacity` (the rest stay pending; a pin that pulsed
twice is one entry), it finds the connector of the pin, and for a DisplayPort or eDP connector with an AUX channel reads `DP_LANE_COUNT_SET` (0x101, bits 4:0: the lanes the link was set up for, 0 while it is not
trained) and the link status (`Dpcd::read_link_status`), and fills a `SinkEvent`: `link_lost` when the link was set up for lanes and does not pass `channel_eq_ok(lanes)` (clock recovery, equalization and symbol lock of each lane
in use, and interlane alignment: the test the driver's short pulse handler retrains on), `test_request` (`DP_AUTOMATED_TEST_REQUEST` in the service interrupt vector), `content_protection` and `sink_specific` (the other
two service interrupts), the whole `status` and `lanes`. A sink that does not answer is `read_failed` (it is gone or asleep); a pin that is no DisplayPort connector, or not in the video BIOS's table, is reported
with `checked` false and nothing is asked. The panel's power is not switched on for this (a sink that is off does not pulse). Shutting down drops the pending pulses.

**Acknowledging and answering.** After reading the status, `service_short_pulses` writes back the service interrupts it handles (`Dpcd::acknowledge_service_irq`: the automated test request 0x02, content protection 0x04 and
sink specific 0x40 of `DP_DEVICE_SERVICE_IRQ_VECTOR`, 0x201: the sink clears a bit when a 1 is written to it) - first, as the driver's handler does, so a new request that arrives during the handling is not lost - and
leaves the others (the MCCS bit and the MST message-ready bits) alone for whoever handles them; `SinkEvent::acked` has the bits written, none are written for a vector without such bits. For an automated test request it
reads `DP_TEST_REQUEST` (0x218, `test_kind`) and answers in `DP_TEST_RESPONSE` (0x260, `test_response`): the **EDID read test** (request 0x04) is done - the EDID of the sink is read over the same AUX channel, the checksum
byte of its last 128-byte block goes to `DP_TEST_EDID_CHECKSUM` (0x261) and the response is ACK with the checksum-write bit (0x05) - and every other test (link training 0x01, video pattern 0x02, PHY test pattern 0x08, no
request, several bits, or an EDID that cannot be read) is answered NAK (0x02), because there is no trained link or pattern generator to run them on. A read or write that did not go through is `handling_failed`.
`AmdAux::dpcd_write` (a native write, 12 bytes at a time) is what writes; `IDpcdAux` gained it.

Retraining: `train_link` remembers a link it trained (by hot plug pin, with the pixel clock and bits per colour), and `service_short_pulses` trains it again for the same mode when the check says
the link is lost (`SinkEvent::retrained`, with the new `LinkTraining` in `retrain`). A link `train_link` never trained is only reported (the boot firmware's link is judged, and a card that has not trained
one has `lanes` 0 and cannot lose it). The memory is dropped when the sink stops answering, when a retraining fails, or when a `train_link` fails; at most `kMaxRetrains` (4) retrainings in a row are made
(`retrain_limit`), a healthy check starts the count again. `AmdVegaMTest::test_retrain_on_short_pulse` (a sink that recovers its clock when pattern 1 starts, `FakeAuxSink::after_write`) covers it.

Not done: the other tests (training, video pattern with the test timing
registers 0x219.., PHY patterns), and the MST message and the downstream port changes. Addresses and bit meanings are from memory, like the rest of the DPCD. `AmdVegaMTest::test_short_pulse_status` (healthy link, a lost lane, a lane beyond the ones in use, an untrained link, lost alignment, each
service interrupt, the eDP panel, a sink that times out or ends before the status, HDMI and an unknown pin, capacity and order, the dropped state) and `DpcdTest::test_lane_count` cover it.

### Audio format blocks: selecting the endpoint

DCE 11 has an audio format block (AFMT) for each DIG encoder, at the DIG's register block (`dig_offsets`: DIG0 at +0, DIG1 +0x100, DIG2 +0x200, DIG3 +0x300, DIG4 +0x400, DIG5 +0x500; the Vega M, like Polaris 10, has
6 DIGs, so blocks 0 to 5 - UNIPHY3, DIG 6, has none), and each takes its audio from one of the eight endpoints (`AudioPins`): `AFMT_AUDIO_SRC_CONTROL` (0x4a45 + the DIG's offset), bits 2:0, `AFMT_AUDIO_SRC_SELECT`, is the
endpoint's number. `amd_dce11::AudioFormat::select_pin(block, pin)` is `dce_v11_0_afmt_audio_select_pin`: a read-modify-write of that field alone (the rest of the register is kept), false for a block or endpoint that does not
exist; `selected_pin(block)` reads it back. `AmdVegaM::select_audio_pin(connector)` ties it to the connector: the connector's encoder object gives the DIG (`AtomConnectors::afmt_instance`, HDMI and DisplayPort only), the
endpoints are read again (`refresh_audio_pins`, as `dce_v11_0_audio_get_pin` does every time), `audio_pin_for` picks the first connected endpoint (the driver's rule: the same one for every block, it does not tell two
displays apart), and the field is written; false for a connector with no audio block (the eDP panel, a DIG 6 transmitter, no encoder in the table) or when no endpoint is connected, and then nothing is written.
`select_audio_pins()` does it for every HDMI and DisplayPort connector with a display on its hot plug pin and returns how many. Nothing calls it by itself: a desktop that sets up audio calls it when a display appears.

Not done here (the next section does the endpoint): the audio clock and sample rate (`dce_v11_0_afmt_setmode`'s DTO and ACR), the audio packets and infoframes, and any audio output to play through. `scripts/check_afmt_port.py <dce_v11_0.c> <vid.h> <dce_11_2_d.h> <dce_11_2_sh_mask.h>` checks the
read-modify-write against the driver's function, the register and field numbers (the generated `AmdDce11Registers.inc`), the DIG offsets, `afmt[i]->offset = dig_offsets[i]`, `num_dig` of CHIP_VEGAM and the pin ids. Tested
by `AmdVegaMTest::test_audio_select` (the XPS-like board: HDMI on DIG 2 and DisplayPort on DIG 3 get the first connected endpoint with the other bits and blocks untouched, the eDP panel and a DIG 6 encoder get nothing,
endpoints and displays that come and go, every block and endpoint, out-of-range blocks and endpoints, stopped).

### Enabling the audio endpoint: the "ELD" from the EDID

The DCE 11 hardware builds the ELD that the sound driver reads (HDMI/DP audio, the codec's ELD buffer) from registers of the endpoint, which the display driver fills from the monitor's EDID in `dce_v11_0_afmt_setmode`;
there is no ELD buffer to write. `EdidAudio::parse(edid, &info)` finds in the CTA extension blocks (tag 0x02, revision 3 and up; the data blocks from byte 4 up to the offset in byte 2, 0 meaning 127) the
Short Audio Descriptors of every audio data block (`drm_edid_to_sad`: the format, channels - 1, sample rate bits and byte 2 of each three-byte descriptor, at most 32), the speaker allocation data block (its three-byte
payload; the first one) and, from the HDMI vendor block (OUI 00-0C-03, at least 5 bytes of payload; the first one), the lip sync latency fields (`drm_parse_hdmi_vsdb_audio`: present flags in bit 7 / 6 of byte 8, then video and
audio latency for progressive and for interlaced modes). Everything in the EDID is checked: data blocks that run past the end of the data blocks, bad offsets, short blocks and blocks of other kinds are skipped (every bit flip and
truncation of a rich EDID runs under the sanitizers in `EdidAudioTest`). All the extension blocks are read, as the current kernels do (older ones read the first only).

`AudioPins` writes the endpoint's indexed registers (index at `0x17a8 + 4 * pin`, data next to it): `set_enabled` is `dce_v11_0_audio_enable` (`HOT_PLUG_CONTROL` = `AUDIO_ENABLED` or 0, the whole register), `write_speaker_allocation`
(`CHANNEL_SPEAKER`: DisplayPort bit 0, HDMI bit 1 - for DisplayPort connectors too, as the driver does - and the monitor's allocation or 5, stereo), `write_sads` (the twelve descriptor registers for PCM, AC-3, MPEG-1, MP3, MPEG-2,
AAC LC, DTS, ATRAC, E-AC-3, DTS-HD, MLP and WMA Pro: each holds the descriptor of that format with the most channels, for PCM the sample rates of all of them ORed into the stereo field, and for the other formats only the first
descriptor; nothing at all for a monitor without descriptors) and `write_latency` (`RESPONSE_LIPSYNC`: the progressive or interlaced video and audio latency, 0 if the monitor gives none). `AmdVegaM::enable_audio(connector, edid,
interlaced)` runs the driver's order: the endpoint (the first connected one, `audio_pin_for`) disabled, speaker allocation, the connector's audio format block selects the endpoint, descriptors, latency, endpoint enabled; the variant without
an EDID reads it first (`read_edid`, over DDC or AUX) and fails with it. A monitor without a CTA extension, or an empty EDID, gives stereo speakers, untouched descriptor registers and no latency, as the driver. `disable_audio` is
`dce_v11_0_audio_enable(pin, false)` for the endpoint the connector's block has selected. Nothing is written for a connector without an audio block or when no endpoint is connected. Nothing calls these by itself. (The audio clock and the packets are the next section's: `enable_audio` does them too now.)

**Difference from the driver:** `dce_v11_0_audio_write_latency_fields` computes the video latency and then the audio latency from zero again, so only the audio latency ever reaches the register; `write_latency` puts both in
(the intended value). **Not done:** the audio output to play through; the ELD proper for
a driver that wants the bytes (monitor name, manufacturer, connection type, SAD count). **Not checked:** `drm_edid.c` (the SAD, speaker and vendor block parsing is from memory) and the audio coding type numbers
(`hdmi.h`); `scripts/check_audio_port.py <dce_v11_0.c> <dce_11_2_d.h> <dce_11_2_sh_mask.h>` checks the rest against the driver: the descriptor registers and their coding types, the SAD loop, the speaker allocation, the enable write, the
latency fields, the register and field numbers, and the order of the steps in `dce_v11_0_afmt_setmode` against `enable_audio`. Tested by `EdidAudioTest` and `AmdVegaMTest::test_audio_enable` (a simulated endpoint with indexed registers,
`tests/FakeAzalia.hpp`: the write order and every value of the rich EDID worked out by hand, the DDC-read variant, a monitor without audio data, endpoint changes, the driver's descriptor rules).

### The audio clock, the packets and the AVI InfoFrame

For the endpoint to carry sound the display engine has to send the audio in the blanking of the picture and a clock the sink can regenerate it from. `AmdVegaM::enable_audio(connector, edid, mode)` does all of
`dce_v11_0_afmt_setmode`'s audio parts now, in the driver's order (`scripts/check_hdmi_port.py` compares the register-by-register sequence with the driver's function): the endpoint is disabled, then
`AudioFormat::setup_packets(block, crtc, clock_khz, bpc)` does the audio clock - `dce_v11_0_audio_set_dto`: `DCCG_AUDIO_DTO_SOURCE.DCCG_AUDIO_DTO0_SOURCE_SEL` is the display controller whose timing it follows, `DCCG_AUDIO_DTO0_PHASE`
24000 and `DCCG_AUDIO_DTO0_MODULE` the pixel clock in kHz, i.e. 24 MHz / pixel clock as a ratio - and the packets of the audio format block (null packets; the general control packet every frame, with deep colour for 10 and
12 bits per component, off for 0, 6, 8, 16 and anything else; the audio info frame every frame; the audio packet delay; the 60958 channel status update and the channel numbers 1 to 8; the audio clock regeneration packet
with the software CTS value unless there is deep colour, sent automatically); then the speaker allocation, `enable_channels` (all eight channels), the block's endpoint selection, the descriptors and the lip sync latency;
then `AudioFormat::start`, which writes the AVI InfoFrame into `AFMT_AVI_INFO0..3` and sends it every frame on line 2, sends the audio samples and sets the ramp test registers; and the endpoint is enabled last.
Nothing is written for a controller that does not exist (`mode.crtc` 6 and up) or when the connector has no audio block or no endpoint is connected.

`Hdmi::acr(clock_khz)` is `amdgpu_afmt_acr`: the N and CTS of the audio clock regeneration packet for 32, 44.1 and 48 kHz audio: the driver's predefined rows for 25.175, 25.2, 27, 27.027, 54, 54.054, 74.176,
74.25, 148.352 and 148.5 MHz, otherwise calculated as `amdgpu_afmt_calc_cts` does (the fraction 128 fs / clock in lowest terms, N scaled up to the first value at or above 128 fs / 1000: 4096 / 65000, 7056 / 81250 and
6144 / 65000 for 65 MHz). `Hdmi::vic(mode)` is `drm_match_cea_mode` for the CEA modes that matter (the 640x480 to 3840x2160 modes with their active size, scan, double clock and nominal refresh within half a percent;
4:3 and 16:9 twins by `picture_aspect`, the first otherwise), and `Hdmi::avi_infoframe(mode, out)` the 17 bytes `hdmi_avi_infoframe_pack` makes for what `drm_hdmi_avi_infoframe_from_display_mode` fills in: RGB, no
bars, the active format "as the picture" (A0 and active aspect 8), the picture aspect, the VIC (0 for no CEA mode) and the pixel repeat of a double clocked mode, with the checksum. The mode is the caller's
(`AmdVegaM::AudioMode`: the timing, the controller and the bits per component; the default is 1920x1080 at 60 Hz on controller 0, 8 bits).

**Checked** against `dce_v11_0.c` by `scripts/check_hdmi_port.py`: the order of the register writes, the constants (CRC control 0x1000, the ramp registers, the DTO phase, the info frame line, the audio packets per line,
the channel enable mask and numbers, the deep colour settings, the software CTS rule) and the AVI words. `HdmiInfoframeTest` checks the predefined N/CTS rows against the HDMI formula (CTS = clock N / (128 fs) within 0.02 %),
the calculated values by hand (13.5, 65 MHz, edge cases), the mode matching with the standard timings, its tolerance, and the InfoFrame bytes with their checksum; `AmdVegaMTest::test_audio_packets` checks the registers
of the HDMI-A (DIG 2) and DisplayPort (DIG 3) blocks for 1080p60 and 480i with values worked out by hand from the previous contents of the registers (fields written alone leave the rest). **Not checked:**
`amdgpu_afmt.c` itself (its rows are from memory, but the formula check would catch a wrong one), `drm_edid.c`'s CEA mode table and `hdmi.c`'s packing (the VIC table and the InfoFrame bytes are from memory of
them; the VIC is information for the sink, a wrong one does not stop the audio). **Not done here (the next section does the sound driver's side):** the audio InfoFrame (the hardware builds it), HDMI vendor InfoFrames (4K, 3D), the DisplayPort audio secondary data packets (the driver sets
DisplayPort up as HDMI too, and so does this).

### The sound driver: the graphics card's HD Audio controller

The card's audio function (PCI class 0x0403, the second function of the GPU) is a High Definition Audio controller with one HDMI / DisplayPort codec, and the system's HD Audio driver (`IntelHdaAudioDevice`, which takes any
controller of that class) plays to it: CORB/RIRB command rings, a cyclic buffer of eight 8 KiB entries through a buffer descriptor list, output stream 0, 48 kHz 16-bit stereo. What the codec adds to an analog one, and
what the driver now does about it:

- **Which pin.** The codec's pins are the display engine's endpoints (pin *n* is endpoint *n*): a pin reports *presence detect* (`GET_PIN_SENSE` bit 31) when its endpoint is enabled for a connected display
  (`AmdVegaM::enable_audio`, `HOT_PLUG_CONTROL.AUDIO_ENABLED`) and *ELD valid* (bit 30) once the speaker allocation and descriptors are programmed. The driver prefers, as it always did, a line out, speaker or headphone pin;
  then a digital pin (pin capabilities HDMI bit 7 or DisplayPort bit 24) with presence and a valid ELD, then one with presence only; then any output pin - a digital one with no display, which carries sound as soon as one
  comes (the device exists and plays into the void until then). Pins whose configuration default says "no physical connection" are skipped.
- **What a digital pin needs.** On top of the analog setup (power, connection select, pin control `OUT_EN`, stream tag and format on the converter): the converter's digital output (`SET_DIGI_CONVERT_1`, `DIGEN`), its channel
  count (`SET_CVT_CHAN_COUNT`, 2 - 1), and the audio InfoFrame (type 0x84, version 1, ten data bytes, checksum, channel count - 1 in the first data byte, the rest "refer to stream header") written through the pin's
  DIP buffer (`SET_HDMI_DIP_INDEX` packet 0 byte 0, `SET_HDMI_DIP_DATA` fourteen bytes, `SET_HDMI_DIP_XMIT` stopped while writing and then sent every frame).
- **Following the displays.** `IAudioDevice::retarget()` (default: false; `SystemAudioDevices::retarget()` calls it on every device) makes `IntelHdaAudioDevice` look at the pins again, silence the old path (no InfoFrame, digital
  output off, pin output off, converter off the stream) and set the new one up, with the stream running on. Nothing calls it by itself: a desktop that drives the card's outputs calls `AmdVegaM::enable_audio(connector)`
  when a display with audio appears, `disable_audio` when it goes, and `SystemAudioDevices::retarget()` after either. (The desktop in `main.cpp` does not drive the card's outputs - they show nothing - so it does not do
  this; and an HDMI link carries audio only while it carries video.)
- **The register seam.** The driver reached the controller through volatile casts; it now goes through `IHdaRegisters` (`PhysicalHdaRegisters` is the memory-mapped one, the only place with the casts), so that a simulated
  controller can stand in: `IntelHdaAudioDevice(IHdaRegisters&, allocator)` is the constructor without PCI.

**Tested** by `IntelHdaTest` against `tests/FakeHda.hpp`, a controller simulated at the register level (reset handshakes, the CORB read from the driver's memory and the RIRB written to it, the stream descriptor) with a
graphics card's codec (converters 2, 4, 6, pins 3, 5, 7 each taking any converter, an optional line out), written from the HD Audio specification and independent of the driver: the pin choice over nine combinations of presence,
ELD and connectivity, an analog pin winning, every verb a digital pin gets (and the order of the InfoFrame's), the stream descriptor, the buffer list entries, playing through the ring with the stream position, retargeting
(the old pin silenced, the new one set up, playback going on, a pin that is gone), the failures and a restart; and by `AmdVegaMTest::test_audio_end_to_end`, where the codec's pin sense follows the display side's
endpoint registers (`FakeAzalia`): the sound driver's pin is the endpoint `enable_audio` chose, and follows it between the HDMI and DisplayPort connectors and between endpoints.

**Not checked:** all the HD Audio verbs and parameters are from the High Definition Audio specification and from memory of the Linux `patch_hdmi.c` / `hda_verbs.h` (not at hand): the DIP buffer protocol (index byte, auto-increment
of the data verb, the xmit codes 0x00 and 0xC0), `SET_DIGI_CONVERT_1` and `SET_CVT_CHAN_COUNT` on an HDMI converter, and the pin sense bits. **Not done:** the vendor-specific verbs of the ATI codecs (channel allocation, down-mix
info, HBR control, the multi-channel remap), more than two channels or other formats (the stream is 48 kHz 16-bit stereo), the ELD read back from the pin (`GET_HDMI_ELDD`) to check what the display side programmed,
unsolicited events for display changes (the driver is polled: someone calls `retarget()`), DisplayPort MST (device select), and anything on real hardware.

### DisplayPort link training through the video BIOS

Before a DisplayPort sink shows a picture the source and the sink agree on a link: the lane count and rate, and for each lane the voltage swing and pre-emphasis the sink can still read the clock and the data
at. `DpLinkTrainer` (`DpLinkTrain.*`) is `amdgpu_atombios_dp_link_train`; the hardware side is `AmdDpSource` (`AmdDpSource.*`), the encoder and the transmitter driven through the AtomBIOS command tables, and
`AmdVegaM::train_link(connector, pixel_clock_khz, bpc, &out)` ties them to a connector.

- **Choosing the link.** `DpLinkConfig::choose(caps, pixel_clock_khz, bits_per_pixel)` is `amdgpu_atombios_dp_get_dp_link_config`: rate by rate from 1.62 up to the sink's maximum, the fewest lanes at that rate
  that carry the pixel clock (lanes x rate x 8 / bpp >= clock); if nothing does, no lanes (the driver's -EINVAL; training refuses). (An earlier version of this code, written from memory, had radeon's
  "fewest lanes at 2.7 first" rule, which amdgpu does not have; the source was fetched later and the code corrected.) `bandwidth_code()` is `DP_LINK_BW_SET` (rate / 27 MHz: 6, 10, 20).
- **The training**, in the driver's order: the sink to D0 (`DP_SET_POWER` 0x600 = 1, for DPCD 1.1 and up, 1 ms to wake), downspread (`DP_DOWNSPREAD_CTRL` 0x107 = 0x10 if the sink supports 0.5 %, else 0),
  `DP_LANE_COUNT_SET` 0x101 (the lanes, with bit 7 for enhanced framing if the sink has it) and `DP_LINK_BW_SET` 0x100; the encoder told to start (`ATOM_ENCODER_CMD_DP_LINK_TRAINING_START`) and the pattern off on the sink
  (`DP_TRAINING_PATTERN_SET` 0x102 = 0). **Clock recovery:** pattern 1 on the source (`..._PATTERN1`) and then on the sink, the drive (voltage 0, pre-emphasis 0) on the transmitter (`SETUP_VSEMPH`, all lanes at once from the
  first lane's value) and on the sink (`DP_TRAINING_LANE0_SET` 0x103, one byte per lane), 400 us; then until `drm_dp_clock_recovery_ok` (the clock recovery done bit of every lane in use): wait (the sink's
  `DP_TRAINING_AUX_RD_INTERVAL` 0x00E: 100 us if 0, else 4 ms per unit), read the link status (0x202..0x207), and if the clock is not there give up when every lane is already at the maximum swing (the 'max reached' flag) or when
  the sink has asked for the same voltage five times in a row, else set the drive to the highest voltage swing and pre-emphasis any lane's adjust request (0x206/0x207) asks for, on every lane, with the 'max
  reached' flags (0x04, 0x20) once level 3 is asked. **Channel equalization:** pattern 3 if the sink has it (`TPS3_SUPPORTED`), else 2, the same way; until `drm_dp_channel_eq_ok` (clock recovery, equalization and symbol lock
  of every lane in use, and interlane alignment), with the waits 400 us or the interval, up to six adjustments (seven status reads). **Finish:** pattern off on the sink and `..._COMPLETE` on the encoder. The outcome (`Result`)
  says where it failed, and the finish is done whatever happened (after the driver's 400 us wait). After a failed clock recovery the equalization is not tried, as in the driver; the driver ignores a failed DPCD
  write, here it ends the training with the result that names it. For an eDP panel (or a DP bridge) whose `DP_EDP_CONFIGURATION_CAP` (0x00D) has bit 0 - the internal DP2 panel mode - `DP_EDP_CONFIGURATION_SET`
  (0x10A) is written 1 after the downspread (the alternate scrambler seed); the encoder is told the panel mode (`ATOM_ENCODER_CMD_SETUP_PANEL_MODE`) by the mode set. A sink of DPCD 1.4 gets 100 us in the clock recovery.
- **The tables.** The encoder is the **DIGx encoder control table (index 4)** for every DIG - the DIG number is in the parameters (revisions 1.2 to 1.4: eight bytes, pixel clock in 10 kHz, the configuration with the link rate
  and the DIG in bits 6:4, the action, mode, lanes, bits per colour, in 1.4 the hot plug id; revision 1.5: a union of the command (DIG, action), stream setup, link setup and panel mode parameters); an earlier version
  of this code used DIG1EncoderControl (74) and DIG2EncoderControl (75) by DIG, which the Linux driver does not do for these chips. The transmitter is `UNIPHYTransmitterControl` (76), revision 1.4, 1.5 or 1.6
  (1.5: symbol clock = link rate / 10 in 10 kHz units, the PHY id - the same number as the DIG on this card - the action, lanes, the connector object id, the encoder mode, the configuration with the hot plug select and
  the CRTC's PLL as the PHY's clock source, the DIG front end as a bit per DIG, the lane set). The parameters are built by `AmdModeSet::encoder` / `transmitter` (see "Mode setting"), which `AmdDpSource` calls.
  Other revisions and DIGs that do not exist are refused (`false`), nothing is run. The table indexes, layouts, constants and the driver's conditions are checked by `scripts/check_modeset_port.py` against
  `atombios.h`, `atombios_crtc.c` and `atombios_encoders.c`, and the DPCD addresses and bits and the algorithm's constants by `scripts/check_dplink_port.py` against `drm_dp.h`, `atombios_dp.c` and `drm_dp_helper.c`
  (kernel v6.12; the AMD sources are `drivers/gpu/drm/amd/amdgpu/atombios_*.c`, without the `amdgpu_` prefix).
- **`train_link`** reads the sink's capabilities (the eDP panel is powered up first), chooses the link, and trains; the connector's encoder object gives the DIG (0..5: the Vega M's six) and the link; the hot plug pin, the
  connector object id and the bits per colour (6, 8, 10, 12, 16; anything else 8) go into the tables.

**Tested** by `DpLinkTrainTest` (the algorithm against a sink and source that log everything in order: the exact write sequence of a clean training, the options of the sink, adjusting to levels the sink asks for - the
highest of several lanes, the flags at the top level, lanes not in use ignored - the clock recovery giving up after five repeats or at the maximum swing, the equalization after one to seven reads, pattern 2 or 3, the
delays by the read interval, every write and read and each source step failing in turn, the link configuration over twelve cases), `AmdDpSourceTest` (an AtomBIOS image whose three tables hand their parameter words to the
card: every word for each action, rate, DIG and revision, and the refusals), and `AmdVegaMTest::test_train_link` (the DisplayPort connector on DIG 3 link B and the eDP panel on DIG 0, with the DPCD writes and the table calls
of the whole training for 1080p60 and 4K60, failure, and the refusals).

**Checked** against the driver's sources (v6.12) as above. **Not checked / not done:** which table revisions a real Polaris video BIOS has (1.5 encoder control and 1.5 transmitter control are what I believe; 1.3, 1.4 and 1.6
are handled too), NUTMEG's 2.7-only rule, 8.1 Gbit/s, and everything on real hardware.

**eDP 1.4 rate table and retraining (done after the above):** a panel of eDP revision 1.4 that lists rates (`Capabilities::edp_rates_khz`) is trained by table index: `DpLinkConfig::choose` takes the lowest listed rate (any order) that
the encoder control can be told (1.62, 2.7, 3.24, 5.4 GHz; 2.16 and the like are skipped) with the fewest lanes, writes `DP_LINK_BW_SET` 0 and `DP_LINK_RATE_SET` (0x115) with the index after the lane count. `service_short_pulses`
retrains a lost link that `train_link` trained (see the short pulse section). Tests: `DpLinkTrainTest::test_edp_rate_table`, `AmdVegaMTest::test_train_link`, `test_retrain_on_short_pulse`. `DP_LINK_RATE_SET` (0x115) is in `drm_dp.h` but the amdgpu AtomBIOS path never uses it (it works from `DP_MAX_LINK_RATE`), so this is an addition of ours for eDP 1.4 panels, which has not been seen to work.

### Mode setting through the video BIOS

`AmdModeSet` (`AmdModeSet.*`) calls the BIOS tables that set a mode with the parameters amdgpu gives them (`atombios_crtc.c` and `atombios_encoders.c`, v6.12, which an earlier version of this section had from
memory - the comparison corrected the encoder control table index, the CRTC timing table and the sequence): SetPixelClock 1.7 (the clock in 100 Hz units - on Polaris/VEGAM the card computes the PLL from it - the PLL,
encoder id, mode, CRTC, HDMI deep colour ratio 5:4 / 3:2 / 2:1, dual link DVI above 165 MHz), SetCRTC_UsingDTDTiming (display size and blanking to the total, sync offset and width, polarity / composite / interlace /
double clock flags), SetCRTC_OverScan and EnableScaler (RMX off: all zero, scaler disabled), SelectCRTC_Source 1.2 / 1.3 (the DIG as ASIC_INT_DIGn_ENCODER_ID, mode, panel colour depth), EnableCRTC, BlankCRTC,
UpdateCRTC_DoubleBufferRegisters (the lock), EnableDispPowerGating, the DIGx encoder control (1.2 to 1.5) and the UNIPHY transmitter control (1.4, 1.5, 1.6).

`AmdVegaM::set_mode(connector, crtc, mode, bpc, &result[, pll])` is the DRM modeset of `dce_v11_0.c` in order: **crtc prepare** (power gating off, lock, blank and disable), the DisplayPort sink's capabilities and the lane /
rate choice, **encoder prepare** (eDP panel on, the CRTC's source), **crtc mode set** (pixel clock - DisplayPort on PPLL0, the others on PPLL1 for even CRTCs and PPLL2 for odd unless named - DTD timing, overscan,
scaler), **encoder mode set** (the encoder off: DP video off, the sink to D3, the transmitter disabled, the eDP panel off), **crtc commit** (enable, unblank, unlock), **encoder commit** (encoder setup, the panel mode,
the eDP panel on, the transmitter enabled, for DisplayPort the link trained - sink to D0, ASSR for the internal DP2 panel mode - and the video on). `ModeSetResult::failed` names the step that failed; tables the BIOS
does not have (power gating, lock, overscan, scaler) are skipped as the driver does by running nothing. `disable_mode` is `dce_v11_0_encoder_disable` and `_crtc_disable`: the encoder off, the CRTC blanked and disabled,
power gating on, and the CRTC's PLL off unless another CRTC set up by `set_mode` still uses it. The retraining after a short pulse remembers the PLL.

Tests: `tests/AmdModeSetTest.cpp` (every word of every table for every revision handled, the refusals), `AmdVegaMTest::test_set_mode` (the order and words for DisplayPort, HDMI and the eDP panel, the panel power
sequence, the internal DP2 panel mode with ASSR, each failing stage, switching off with the shared PLL), `scripts/check_modeset_port.py`.

**Scanout:** `amd_dce11::Scanout` is `dce_v11_0_crtc_do_set_base` (VGA mode off, surface update at vertical blank, primary and secondary addresses, the control register for the pixel format - ARGB8888, ABGR8888 (red/blue crossbar), RGB565, ARGB1555, ARGB4444, BGRA5551, ARGB2101010, BGRA1010102 (the 10 bit ones bypass the look-up table), 8 bit indexed - and the tiling (linear, 1D, 2D with banks, tile split, bank size and macro tile aspect; pipe configuration), no swap, offsets and window, pitch, the plane enabled, the line buffer height, the viewport with its x / y position, the master update mode), `load_lut` (`dce_v11_0_crtc_load_lut`: the colour pipeline passed through and the 256 entry table, identity for none), `vga_enable`, `set_interleave` and `program_fmt` (`dce_v11_0_program_fmt`: truncation, or dither, by depth). `scripts/check_scanout_port.py` checks the register addresses, bits, the format table, field positions and the driver's write orders against `dce_v11_0.c` and the headers.

**Watermarks:** `amd_dce11::Watermarks` is `dce_v11_0_bandwidth_update`: the line buffer split by display width (`line_buffer_adjust`, waiting for the DMIF allocation) and the urgency watermarks A (high clocks) and B (low clocks) per CRTC, with the driver's 20.12 fixed point arithmetic and the DRAM channel count from `MC_SHARED_CHMAP`; the clocks come from `Inputs::display_clocks` (the driver's current / DPM sclk and mclk; all zero: not touched). `set_mode` runs it at the end of the base setup, `disable_mode` after the CRTC goes. The checks that only decide a debug message ("force priority to high") are not ported. The arithmetic is tested against values from a separate script.

**PLL, FMT, audio in the modeset:** the PLL is `dce_v11_0_pick_pll` for Polaris/VEGAM: the DP DTO (11) for DisplayPort, a combo PHY PLL (20..25) by UNIPHY and link for the others (an earlier version of this section had PPLL0/1/2, which the driver uses on other chips); a combo PHY PLL is switched off with no CRTC (`ATOM_CRTC_INVALID`), the DP DTO never. The CRTC's dpms follows `dce_v11_0_crtc_dpms` (the VGA mode on around the blank; a CRTC that was never enabled is not blanked), the FMT block is programmed for non-LCD outputs in the encoder prepare, the interleave after the encoder is switched off, and for HDMI with the monitor's EDID given the audio endpoint is set up in the encoder's mode set (`ModeSetStage::Audio`). **Spread spectrum:** nothing to port - on Polaris/VEGAM `dce_v11_0_crtc_mode_set` passes the clock to SetPixelClock with zero dividers ("SetPixelClock calculates the plls and ss values now") and only the older `amdgpu_atombios_crtc_set_pll` path programs it.

**The monitor:** `AmdMonitor.*` has `parse_monitor_info` (the EDID's digital input, depth, HDMI block, deep colour bits and maximum TMDS clock as `update_display_info` / `drm_parse_cea_ext` do, including the 1.4 depth overriding the extension's), `monitor_bpc` (`amdgpu_connector_get_monitor_bpc`: 8 unless the sink says otherwise, the HDMI clamp to 12 and to the TMDS clock, and above 8 only with the module parameter amdgpu.deep_color, which is off by default), `mode_valid` (the DVI/HDMI, DisplayPort and eDP/LVDS `mode_valid` helpers of `amdgpu_connectors.c` and `amdgpu_atombios_dp_mode_valid_helper`) and `edid_detailed_timing` (`drm_mode_detailed`: the timing descriptors as CRTC values). Tests: `AmdMonitorTest`.

**Lighting an output:** `AmdVegaM::light_up(connector, crtc, &result)` reads the EDID, takes the first detailed timing the connector accepts and `Inputs::output_framebuffer_bytes` holds, chooses the depth, fills a test picture (a grey ramp with a white border) in the block's output area and runs `set_mode` with it as the scanout surface (and the EDID for HDMI audio); `picture()` is the CPU's window onto it. `main.cpp` (with `TCOS_AMD_VEGAM`) calls it for every output with a display at start and on every hot plug, switches the output off with `disable_mode` on unplug, and names the result in the toast and the boot log. `AmdVegaMTest::test_light_up` covers HDMI over DDC, DisplayPort over AUX with its link, the CRTC choice, the refusals. The whole OS builds with the option on.

**Still not done:** the desktop does not draw on a lit output (it shows the test picture): the output is not in the desktop's monitor layout, which needs the picture to be the target of a desktop monitor (`Monitor` / `MonitorLayout` and the renderer) rather than a CPU-filled block; a DP bridge chip's DP1 panel mode is read (`panel_mode_of`) but NUTMEG / TRAVIS external encoder control (`setup_external_encoder`) is not done; backlight (the internal panel is on the Intel GPU); the HBR2 capability record of the encoder (`dp12_capable` is assumed in `light_up`); the DPM clocks that `display_clocks` stands for; and everything on real hardware.
