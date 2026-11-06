# AMD Ryzen platforms

Every Ryzen, Ryzen Threadripper and Ryzen AI processor from Zen to Zen 6 is identified, and the blocks its SoC carries (integrated graphics, display,
video engine, NPU) are known by version. What runs on that hardware differs per block; the table at the end says what is driven and what is only
identified.

## Where things are

| Piece | Files |
| --- | --- |
| SoC catalogue (CPU model -> die, core, GC/DCN/VCN versions, iGPU IDs, NPU) | `src/Hardware/Platform/AMD/AmdRyzenPlatform.{hpp,cpp}` |
| CPU generation classes (Zen ... Zen 6) | `src/Hardware/CentralProcessingUnit/AMD/` (`AmdGenerations.cpp`, `AmdZen*Cpu.hpp`) |
| Integrated GPU IDs | `src/Hardware/GraphicsProcessingUnit/AMD/AmdGpuGenerations.cpp` |
| VCN versions (firmware file, codec lists, unified queue) | `src/Hardware/VideoCodec/AMD/AmdVcnGenerations.{hpp,cpp}` |
| Video engine device | `src/Hardware/VideoCodec/AMD/AmdVcnVideoEngine.hpp` |
| NPU (XDNA / XDNA 2) | `src/Hardware/NeuralProcessingUnit/AMD/` |
| Tests | `tests/AmdRyzenPlatformTest.cpp`, `tests/AmdGenerationTest.cpp`, `tests/VideoCodecTest.cpp` (`test_amd_*`), `tests/AmdXdnaHwTest.cpp` |

At boot, an AMD Zen machine gets a `Platform` line in the log after the NPU line, built by `amd_ryzen_identify_machine` (CPU model, then the
integrated GPU and NPU found on the PCI bus) and `amd_ryzen_describe`, for example:

```
Platform  Strix Point (Ryzen AI 300 (HX 370, 365)), Zen 5; Radeon 880M/890M (RDNA 3.5), GC 11.5.0, DCN 3.5.0, VCN 4.0.5 (vcn_4_0_5.bin); XDNA 2 NPU rev 16
```

## The catalogue

Family/model ranges follow the Linux kernel (`arch/x86/kernel/cpu/amd.c` for the Zen class of each model, `drivers/hwmon/k10temp.c` for which die a
model is). Block versions are what amdgpu finds in each ASIC's IP discovery table; the machine's own table (read by `AmdMediaEngineHw`) always wins,
and the catalogue is what to expect before it is read. GPU IDs come from `amdgpu_drv.c` and the PCI ID repository, NPU IDs from
`drivers/accel/amdxdna/amdxdna_pci_drv.c`.

| SoC | Family/models | Core | Graphics (PCI) | GC | DCN | VCN | NPU |
| --- | --- | --- | --- | --- | --- | --- | --- |
| Summit Ridge | 17h 00-07 | Zen | - | | | | |
| Pinnacle Ridge | 17h 08 | Zen+ | - | | | | |
| Raven Ridge | 17h 10-17 | Zen | Vega (15DD) | 9.1.0 | 1.0.0 | 1.0.0 | |
| Picasso | 17h 18 | Zen+ | Vega (15D8) | 9.1.0 | 1.0.0 | 1.0.0 | |
| Dali / Pollock | 17h 20-2F | Zen | Vega, Raven2 (15D8) | 9.2.2 | 1.0.1 | 1.0.1 | |
| Castle Peak | 17h 30-3F | Zen 2 | - | | | | |
| Cyan Skillfish (4700S kit) | 17h 40-4F | Zen 2 | fused off | | | | |
| Renoir | 17h 60-67 | Zen 2 | Vega (1636) | 9.3.0 | 2.1.0 | 2.2.0 | |
| Lucienne | 17h 68-6F | Zen 2 | Vega (164C) | 9.3.0 | 2.1.0 | 2.2.0 | |
| Matisse | 17h 70-7F | Zen 2 | - | | | | |
| Van Gogh (Steam Deck) | 17h 90-91 | Zen 2 | RDNA 2 (163F, 1435) | 10.3.1 | 3.0.1 | 3.0.2 | |
| Mendocino | 17h A0-AF | Zen 2 | RDNA 2 (1506) | 10.3.7 | 3.1.6 | 3.1.2 | |
| Chagall | 19h 08-0F | Zen 3 | - | | | | |
| Storm Peak | 19h 18-1F | Zen 4 | - | | | | |
| Vermeer | 19h 20-2F | Zen 3 | - | | | | |
| Rembrandt | 19h 40-4F | Zen 3 | RDNA 2 (1681) | 10.3.3 | 3.1.2 | 3.1.1 | |
| Cezanne / Barcelo | 19h 50-5F | Zen 3 | Vega (1638, 15E7) | 9.3.0 | 2.1.0 | 2.2.0 | |
| Raphael / Dragon Range | 19h 60-6F | Zen 4 | RDNA 2 (164E) | 10.3.6 | 3.1.5 | 3.1.2 | |
| Phoenix | 19h 70-7F | Zen 4 | RDNA 3 (15BF, 15C8) | 11.0.1 | 3.1.4 | 4.0.2 | XDNA (1502) |
| Hawk Point | 19h 70-7F | Zen 4 | RDNA 3 (1900, 1901) | 11.0.1 | 3.1.4 | 4.0.2 | XDNA (1502) |
| Turin / Shimada Peak | 1Ah 00-1F | Zen 5 | - | | | | |
| Strix Point | 1Ah 20-2F | Zen 5 | RDNA 3.5 (150E) | 11.5.0 | 3.5.0 | 4.0.5 | XDNA 2 (17F0 rev 10) |
| Granite Ridge / Fire Range | 1Ah 40-4F | Zen 5 | RDNA 2 (13C0) | 10.3.6 | 3.1.5 | 3.1.2 | |
| Krackan Point | 1Ah 60-6F | Zen 5 | RDNA 3.5 (1114, 1902) | 11.5.2 | 3.5.1 | 4.0.5 | XDNA 2 (17F0 rev 11) |
| Strix Halo | 1Ah 70-77 | Zen 5 | RDNA 3.5 (1586) | 11.5.1 | 3.5.1 | 4.0.6 x2 | XDNA 2 (17F0 rev 20) |
| Zen 6 | 1Ah 50-5F, 80-AF | Zen 6 | (not known yet) | | | | |

Phoenix and Hawk Point share their CPU models; the integrated GPU's PCI ID tells them apart (`amd_ryzen_identify(family, model, gpu_id)`). 15D8 is both
Picasso and Raven2; the CPU model tells them apart. Phoenix2 / Hawk Point 2 parts have no NPU, which the boot line reports as "not on the bus".
Less certain entries (checked against no hardware here): the DCN of Rembrandt-R and Krackan, the GC of Krackan2, and which Zen 6 models are client parts.

## Video engines

`amd_vcn_info(version)` gives, per VCN IP version, the ip block generation, the firmware file amdgpu loads, whether a JPEG block comes with it, whether
decode and encode share one queue (VCN 4 and 5), and amdgpu's own codec lists with their size limits (`soc15.c`, `nv.c`, `soc21.c`, `soc24.c`). Notable:
VCN 3.1.x (Rembrandt, Mendocino, Raphael, Granite Ridge) drops MPEG-2, MPEG-4 Part 2 and VC-1; VCN 4 adds AV1 encode; Navi 24's VCN 3.0.33 has no
encoder.

`AmdVcnVideoEngine` uses it: the codecs it reports are the version's lists (from the IP discovery table when read, else from the catalogue), and
`load_firmware` now reads and checks the file of every VCN generation, not only VCN 1 (`renoir_vcn.bin`, `green_sardine_vcn.bin`, `vangogh_vcn.bin`,
`yellow_carp_vcn.bin`, `vcn_3_1_2.bin`, `vcn_4_0_2.bin`, `vcn_4_0_5.bin`, `vcn_4_0_6.bin`). Only VCN 1.0 has a start sequence and decode sessions; the
others say "no start sequence for this VCN generation yet" and their sessions run on the CPU. The VCN 2-4 firmware files are in `firmware/` (see `firmware/README.md`), and `VideoCodecTest` checks that every Ryzen APU finds and validates its own.

## What is driven, and what is only identified

| Block | State |
| --- | --- |
| CPU | Zen ... Zen 6 classes from the Linux family/model table (CPU identity only; no per-generation tuning). |
| Integrated graphics | Identified; boot display through UEFI GOP. Native display surface swap only on Raven (DCN 1.0, `AmdDcn1`). No GFX/compute submission on any Ryzen APU. |
| Video engine | Identified with codec lists for every Ryzen APU. VCN 1.0 (Raven, Picasso, Dali) runs decode sessions once its self-test passes on the machine (tested in simulation only); VCN 2-4: firmware read and checked only, decode and encode run on the CPU. |
| NPU | See `src/Hardware/NeuralProcessingUnit/AMD/` and the section below. Computation runs on the CPU (no graph compiler). |

## NPU

Written from the Linux `drivers/accel/amdxdna` driver (register offsets, message layouts, sequences); tested only against a simulated NPU
(`tests/AmdXdnaHwTest.cpp`), never on hardware. The linux-firmware NPU images are in `firmware/amdnpu/<id>_<rev>/` (`npu.sbin`, `npu_7.sbin`) and
pass the PSP load path in the test; nothing loads them at boot yet. linux-firmware has none for the AIE4 parts.

| File | What it does |
| --- | --- |
| `AmdXdnaHw` | `amd_xdna_identify(device, revision)`: 1502/00 npu1 (XDNA), 17F0/10, /11, /20 npu4/5/6 (XDNA 2), 17F2 and 1B0B (PF), 17F3 and 1B0C (VF) rev 10 npu3 (AIE4). Per-model layout (BARs, PSP/SMU registers, mailbox bases, firmware names, DPM tables). Read-only `probe()`, SMU power/clock/DPM commands, PSP firmware load (64 KiB aligned DMA copy) and start/stop. |
| `AmdXdnaMailbox` | The management ring protocol (16-byte headers, tombstone wrap, 8 slots, matching by id), polled. Everything the device writes is bounds-checked; a corrupt ring breaks the channel and nothing outside the rings is read. |
| `AmdXdnaMessages` | Management messages: suspend/resume, PASID, runtime config, firmware/AIE/tile/protocol queries, device revision, time quota, create/destroy context, map host buffer. |
| `AmdXdnaDevice` | The bring-up of `aie2_hw_start` (SMU, PSP, alive word, channel, protocol check, runtime config, DPM, queries) with unwinding on failure, and stop. AIE4 comes up only as far as the channel (its message set is not ported). |
| `AmdXdnaNpu` / `SystemNeuralProcessingUnit` | The boot line names the model by (device, revision); `probe_registers` reads the AMD NPU's status read-only; `start_amd_device` exists but `main.cpp` does not call it (no firmware). |

Not done: interrupts/MSI-X, IOMMU/PASID setup beyond PASID 0, error-event buffers, command submission (execbuf, the column resolver), AIE4 messages,
and any computation on the AI Engine array (no graph compiler): NPU work still runs on the CPU.
