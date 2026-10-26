# Dell XPS 15 2-in-1 9575 (Core i7-8705G / i5-8305G, Kaby Lake-G)

Two GPUs on one package: Intel HD Graphics 630 (Gen9.5, PCI `0x591B`) and a Radeon RX Vega M GL (Polaris 22 with HBM2, PCI `0x694E`; GH is `0x694C`).
The internal panel is wired to the Intel GPU, so the Intel GPU is the boot display and the Vega M is a discrete adapter that `GpuManager` puts to sleep.

| Piece | Intel HD 630 | Radeon RX Vega M |
| --- | --- | --- |
| Identified (`*_gpu_identify`) | yes | yes |
| Boot display / mode setting | `IntelGpu` native path (Gen9.5, unvalidated on hardware) | UEFI GOP only (no Polaris display driver) |
| Video decode | `VideoCodec/Intel` (AVC, HEVC, VP9, MPEG-2, VC-1 via the VCS ring, simulated tests only) | none: needs a UVD 6.3 driver (the AMD code is VCN, Raven) |
| 3D acceleration | `Gen9GraphicsDevice` on the render engine, opt-in (`TCOS_INTEL_RENDER`), tested in simulation, **not run on hardware** (below) | none (CPU renderer) |

## Not done

- **Polaris bring-up**: SMU7 firmware load, GFX8 CP/RLC/SDMA microcode, memory controller + GART for the HBM2, interrupt handler, DCE 11.2 display.
  None of it exists and none can be checked without the hardware and the firmware blobs.
- **UVD 6.3 video decode** for the Vega M (H.264, HEVC; no VP9).
- **Hardware 3D** on the Vega M: a GCN4 PM4 path needing a shader compiler to the native ISA. On the Intel GPU see below for what is and is not there.
- Nothing here was run on a real 9575.

## Intel Gen9.5 render engine (HD Graphics 630)

Code: `src/Hardware/GraphicsProcessingUnit/Intel/`, test: `tests/IntelRenderTest.cpp` (`tests/run_host_tests.sh IntelRenderTest`).

| File | Job |
| --- | --- |
| `IntelLrc` | `lrc_init_gen9_rcs`: the render engine's logical ring context (`gen9_rcs_offsets` of the i915 driver; RCS0 at MMIO `0x2000`, 22 pages). |
| `IntelVcsDriver` | `Config::engine = Engine::Render` drives RCS0 through the same execlist code as the video engine: render power well, 28 global GTT pages, PIPE_CONTROL ring job (invalidate, batch, flush, marker write). |
| `IntelRenderCommands` | The 3D command set a batch may hold and the walker that checks a batch against it (no register writes, no global-GTT `PIPE_CONTROL`, no unknown command). |
| `IntelRenderBatch` | Builds one job's batch image: `fill_rect`, `triangles` (flat or interpolated colour, source-over blend), `copy` (nearest-neighbour textured rectangle). The command order and the copy kernel are those of the intel-gpu-tools Gen9 render copy, which runs on hardware; packet fields were checked against Mesa's genxml (`gen8.xml`, `gen9.xml`). |

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

Not done: stencil, textured triangles, tiling, interrupts (the driver polls), a render node for a non-display Intel GPU, the other engines' workarounds, and every check
on real hardware. The device is not used by the 2D renderer. Treat the first run on a 9575 as a bring-up, not as a working driver.
