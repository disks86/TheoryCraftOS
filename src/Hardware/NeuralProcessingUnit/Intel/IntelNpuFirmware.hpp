/*
 * TheoryCraftOS - a minimal UEFI x86_64 operating system.
 * Copyright (C) 2026 Christopher Schaefer
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published
 * by the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */


#pragma once
#include <stdint.h>
#include "Hardware/NeuralProcessingUnit/Intel/IntelNpuHw.hpp"
#include "Utility/ArrayView.hpp"

namespace hardware {

// Firmware images for Intel NPUs ("VPU") and everything the host hands the firmware before it starts, as the Linux ivpu driver does it (ivpu_fw.c,
// vpu_boot_api.h): parsing and checking the image file, the NPU-side memory map the checks are made against, loading the image into the runtime
// memory, filling the 4 KiB boot parameter page, and the load-power-up-boot sequence. All of it works on views the caller supplies; the NPU's own
// MMU, which has to map those pages at the addresses below before the NPU's CPU runs, is not here yet, nor is the IPC channel the firmware reports
// ready on (see IntelNpuHw).

// A range of the NPU's own 64-bit address space, [start, end).
struct IntelNpuAddressRange {
  uint64_t start = 0, end = 0;
  // ivpu_is_within_range(): addr + size does not wrap and lies inside.
  constexpr bool contains(uint64_t addr, uint64_t size) const {
    return size <= UINT64_MAX - addr && addr >= start && addr + size <= end;
  }
};

// The NPU address space layout of a hardware generation (ivpu_hw.c: memory_ranges_init).
struct IntelNpuMemoryMap {
  IntelNpuAddressRange runtime, global, shave, user, dma;
  static IntelNpuMemoryMap for_ip(IntelNpuIp ip);
};

enum class IntelNpuFirmwareError : uint8_t {
  None,
  TooSmall,           // not even a header and a version page
  HeaderVersion,
  BootParamsAddress,
  VersionSize,
  VersionAddress,
  RuntimeSize,        // runtime_size smaller than the boot-params and version pages, or not page aligned
  RuntimeAddress,
  ImageSize,          // the image does not fit in the file or in the runtime memory
  ImageAddress,
  EntryPoint,
  ShaveSize,
  ReadOnlySection,
  BootApiVersion,
  JsmApiVersion,
};
const char* intel_npu_firmware_error_name(IntelNpuFirmwareError error);

// How the NPU schedules jobs: its firmware ("OS") or the hardware scheduler ("HW"). The values are the boot parameter's.
inline constexpr uint32_t kIntelNpuSchedOs = 0;
inline constexpr uint32_t kIntelNpuSchedHw = 1;
inline constexpr uint32_t kIntelNpuSchedAuto = 0xFFFFFFFFu;  // decide from the firmware's JSM API version, as ivpu does

// The result of reading a firmware file. Everything in it is checked against the memory map; `image` and `version` point into the file.
struct IntelNpuFirmware {
  static constexpr uint32_t kHeaderSize = 4096;        // VPU_FW_HEADER_SIZE
  static constexpr uint32_t kVersionHeaderSize = 4096; // FW_VERSION_HEADER_SIZE: the version string's page
  static constexpr uint32_t kFileImageOffset = kHeaderSize + kVersionHeaderSize;
  static constexpr uint32_t kHeaderVersion = 1;
  static constexpr uint32_t kApiVersions = 16;
  static constexpr uint32_t kBootApiIndex = 0, kJsmApiIndex = 4;
  static constexpr uint32_t kPageSize = 4096;
  static constexpr uint32_t kShaveNnMaxSize = 2u << 20;
  static constexpr uint32_t kPreemptMinSize = 4096, kPreemptMaxSize = 32u << 20;
  static constexpr uint32_t kMinApiMajor = 3;          // both the boot and the JSM API

  IntelNpuFirmwareError error = IntelNpuFirmwareError::TooSmall;
  bool valid() const { return error == IntelNpuFirmwareError::None; }

  ConstByteView image;    // the bytes to copy into runtime memory
  ConstByteView version;  // the firmware's version string (no terminator); empty if the file has none
  uint64_t boot_params_addr = 0, boot_params_size = 0;
  uint64_t fw_version_addr = 0, fw_version_size = 0;
  uint64_t runtime_addr = 0, runtime_size = 0;  // the image's own memory, without the two pages above
  uint64_t image_load_offset = 0;
  uint32_t image_size = 0;
  uint32_t shave_nn_size = 0;                   // page aligned
  uint64_t cold_boot_entry_point = 0;
  uint32_t api_version[kApiVersions] = {};      // (major << 16) | minor
  uint32_t sched_mode = kIntelNpuSchedOs;
  uint32_t primary_preempt_buf_size = 0, secondary_preempt_buf_size = 0;  // 0: mid-inference preemption not supported
  uint64_t read_only_addr = 0;
  uint32_t read_only_size = 0;

  static uint32_t api_major(uint32_t v) { return v >> 16; }
  static uint32_t api_minor(uint32_t v) { return v & 0xFFFF; }

  // ivpu_fw_parse(). `sched_request` is kIntelNpuSchedOs/Hw to force a mode, or kIntelNpuSchedAuto. Never reads outside `file`, whatever its header says.
  static IntelNpuFirmware parse(ConstByteView file, IntelNpuIp ip, uint32_t sched_request = kIntelNpuSchedAuto);
};

// What the host knows that goes into the boot parameter page (ivpu_fw_boot_params_setup). Addresses are NPU addresses of buffers the host has set up.
struct IntelNpuBootInputs {
  uint32_t pci_bus = 0;                 // vpu_id
  uint32_t perf_clk_frequency = 0;      // profiling clock, 0 if none
  uint32_t si_stepping = 0;             // PCI revision
  uint64_t device_id = 0;               // PCI device id
  uint64_t tile_fuse = 0;               // feature_exclusion: the fused-off tiles
  uint64_t sku = 0;
  uint32_t min_ratio = 0, pn_ratio = 0, max_ratio = 0;  // from IntelNpuInfo
  uint64_t ipc_rx_addr = 0;             // the IPC receive buffer: first half headers, second half payloads
  uint32_t ipc_rx_size = 0;
  uint64_t shave_nn_addr = 0;           // 0 when the firmware has no SHAVE NN section
  uint64_t crit_log_addr = 0, verbose_log_addr = 0;
  uint32_t crit_log_size = 0, verbose_log_size = 0;
  uint32_t trace_level = 0;
  uint64_t telemetry_base = 0, telemetry_size = 0;
  bool telemetry_enable = false;
  bool uses_ecc_mca_signal = false;
  bool disable_d0i2 = false;
  uint64_t system_time_us = 0;          // wall clock, microseconds since 1970
};

enum class IntelNpuBootParamsError : uint8_t { None, PageSize, IpcBuffer, ShaveAddress, LogBuffer };

class IntelNpuBoot {
 public:
  static constexpr uint32_t kBootParamsSize = 4096;
  static constexpr uint32_t kBootParamsMagic = 0x10000;
  static constexpr uint32_t kBootCold = 0, kBootWarm = 1;
  static constexpr uint32_t kWatchdogIrqMss = 32, kWatchdogIrqNce = 33;
  static constexpr uint32_t kFocusPresentTimerMs = 1000;
  static constexpr uint32_t kTraceDestinationVerbose = 2;  // VPU_TRACE_DESTINATION_VERBOSE_TRACING
  static constexpr uint32_t kTraceFormatString = 0;
  static constexpr uint32_t kMcaEccBoth = 3;
  // Byte offsets of the fields in the page (vpu_boot_params with the API's 4-byte packing); used by tests and by whoever reads the page back.
  enum Offset : uint32_t {
    kMagic = 0, kVpuId = 4, kPerfClk = 84, kIpcHeaderStart = 256, kIpcHeaderSize = 264, kSharedBase = 268, kSharedSize = 276, kIpcPayloadStart = 280,
    kIpcPayloadSize = 288, kPioBase = 292, kPioSize = 300, kAutoconfig = 304, kCacheDefaults = 308, kShaveNnBase = 324, kSaveRestoreRet = 332,
    kWatchdogMss = 512, kWatchdogNce = 516, kSiStepping = 772, kDeviceId = 776, kFeatureExclusion = 784, kSku = 792, kMinRatio = 800, kMaxRatio = 804,
    kTraceLevel = 808, kBootType = 812, kTelemetryBase = 816, kTelemetrySize = 824, kTelemetryEnable = 832, kCritBufAddr = 836, kCritBufSize = 844,
    kVerboseBufAddr = 848, kVerboseBufSize = 856, kTraceDestination = 868, kTraceHwComponents = 872, kTraceFormats = 880, kPnRatio = 908, kDvfsMode = 912,
    kD0i3DelayedEntry = 924, kD0i3Residency = 928, kD0i3EntryTs = 936, kSystemTime = 944, kSchedulingMode = 1136, kFocusPresentTimer = 1140,
    kEccMcaSignal = 1144, kPowerProfile = 1148,
  };

  // Cold-boot parameters: clears `page` (exactly kBootParamsSize bytes) and fills it in.
  static IntelNpuBootParamsError build_cold(ByteView page, const IntelNpuFirmware& fw, IntelNpuIp ip, const IntelNpuMemoryMap& map,
                                            const IntelNpuBootInputs& in);
  // Warm boot (back from D0i3): only the varying fields change.
  static IntelNpuBootParamsError update_warm(ByteView page, uint64_t residency_us, uint64_t entry_vpu_ts, uint64_t system_time_us);

  // ivpu_fw_load(): `runtime` is the host's view of the firmware's runtime memory (fw.runtime_size bytes, mapped at fw.runtime_addr). Zeroes what is
  // before the image, copies the image and zeroes the rest.
  static bool load_image(ByteView runtime, const IntelNpuFirmware& fw);
};

// The memory the host gives the firmware, as host views of what the NPU's MMU maps at the firmware's addresses.
struct IntelNpuBootMemory {
  ByteView boot_params;  // kBootParamsSize bytes
  ByteView runtime;      // fw.runtime_size bytes
};

enum class IntelNpuStartStage : uint8_t { Done, Memory, Load, BootParams, PowerUp, CpuBoot };
struct IntelNpuStartResult {
  bool ok = false;
  IntelNpuStartStage stage = IntelNpuStartStage::Memory;  // where it stopped; Done if ok
  IntelNpuStep hw_step = IntelNpuStep::None;              // for PowerUp and CpuBoot: the hardware step that failed
  IntelNpuBootParamsError params_error = IntelNpuBootParamsError::None;
};

// Loads the image and the cold-boot parameters, powers the NPU up and starts its CPU at the firmware's entry point. `hw` must have had read_info() called.
// The caller has made `memory` reachable from the NPU (its MMU) before; this returns when the CPU has been released, not when the firmware answers.
IntelNpuStartResult intel_npu_start(IntelNpuHw& hw, const IntelNpuFirmware& fw, IntelNpuIp ip, const IntelNpuMemoryMap& map, const IntelNpuBootInputs& inputs,
                                    const IntelNpuBootMemory& memory);

}  // namespace hardware
