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


#include "Hardware/NeuralProcessingUnit/Intel/IntelNpuFirmware.hpp"
#include "Utility/ByteStream.hpp"

namespace hardware {

namespace {
constexpr uint64_t kMiB = 1ull << 20, kGiB = 1ull << 30;

constexpr IntelNpuAddressRange range(uint64_t start, uint64_t size) { return {start, start + size}; }
constexpr bool page_aligned(uint64_t v) { return (v & (IntelNpuFirmware::kPageSize - 1)) == 0; }
constexpr uint64_t page_align_up(uint64_t v) { return (v + (IntelNpuFirmware::kPageSize - 1)) & ~uint64_t{IntelNpuFirmware::kPageSize - 1}; }

// Little-endian stores into the boot parameter page. Offsets are the constants of IntelNpuBoot::Offset, all inside the page; the checked subview stops
// the OS if one ever is not.
void put32(ByteView page, uint32_t offset, uint32_t v) { ByteWriter w(page.subview(offset, 4)); w.le32(v); }
void put64(ByteView page, uint32_t offset, uint64_t v) { ByteWriter w(page.subview(offset, 8)); w.le64(v); }
void put8(ByteView page, uint32_t offset, uint8_t v) { ByteWriter w(page.subview(offset, 1)); w.u8(v); }

// The preemption buffer sizes of ivpu_preemption_config_parse(); zero when the firmware's numbers are out of range or the scheduler is not the hardware one.
void parse_preemption(IntelNpuFirmware& fw, uint32_t size1, uint32_t max1, uint32_t size2, uint32_t max2) {
  const uint32_t primary = max1 ? max1 : size1;
  const uint32_t secondary = max2 ? max2 : size2;
  if (primary < IntelNpuFirmware::kPreemptMinSize || secondary < IntelNpuFirmware::kPreemptMinSize) return;
  if (primary > IntelNpuFirmware::kPreemptMaxSize || secondary > IntelNpuFirmware::kPreemptMaxSize) return;
  if (fw.sched_mode != kIntelNpuSchedHw) return;
  fw.primary_preempt_buf_size = static_cast<uint32_t>(page_align_up(primary));
  fw.secondary_preempt_buf_size = static_cast<uint32_t>(page_align_up(secondary));
}
}  // namespace

IntelNpuMemoryMap IntelNpuMemoryMap::for_ip(IntelNpuIp ip) {
  IntelNpuMemoryMap m;
  if (ip == IntelNpuIp::Ip37xx) {
    m.runtime = range(0x84800000, 64 * kMiB);
    m.global = range(0x90000000, 256 * kMiB);
    m.user = range(0xa0000000, 511 * kMiB);
    m.shave = range(0x180000000, 2 * kGiB);
    m.dma = range(0x200000000, 128 * kGiB);
  } else {
    m.runtime = range(0x80000000, 64 * kMiB);
    m.global = range(0x90000000, 256 * kMiB);
    m.shave = range(0x80000000, 2 * kGiB);
    m.user = range(0x100000000, 256 * kGiB);
    m.dma = m.user;
  }
  return m;
}

const char* intel_npu_firmware_error_name(IntelNpuFirmwareError error) {
  switch (error) {
    case IntelNpuFirmwareError::None: return "none";
    case IntelNpuFirmwareError::TooSmall: return "file too small";
    case IntelNpuFirmwareError::HeaderVersion: return "unknown header version";
    case IntelNpuFirmwareError::BootParamsAddress: return "boot parameters outside runtime memory";
    case IntelNpuFirmwareError::VersionSize: return "bad version page size";
    case IntelNpuFirmwareError::VersionAddress: return "version page outside runtime memory";
    case IntelNpuFirmwareError::RuntimeSize: return "bad runtime size";
    case IntelNpuFirmwareError::RuntimeAddress: return "runtime outside runtime memory";
    case IntelNpuFirmwareError::ImageSize: return "bad image size";
    case IntelNpuFirmwareError::ImageAddress: return "image outside runtime memory";
    case IntelNpuFirmwareError::EntryPoint: return "entry point outside the image";
    case IntelNpuFirmwareError::ShaveSize: return "SHAVE NN firmware too big";
    case IntelNpuFirmwareError::ReadOnlySection: return "read-only section outside the image";
    case IntelNpuFirmwareError::BootApiVersion: return "incompatible boot API";
    case IntelNpuFirmwareError::JsmApiVersion: return "incompatible JSM API";
  }
  return "?";
}

IntelNpuFirmware IntelNpuFirmware::parse(ConstByteView file, IntelNpuIp ip, uint32_t sched_request) {
  IntelNpuFirmware fw;
  auto fail = [&fw](IntelNpuFirmwareError e) { fw.error = e; return fw; };
  if (file.size() <= kFileImageOffset) return fail(IntelNpuFirmwareError::TooSmall);

  // struct vpu_firmware_header, packed to 4 bytes: the fields are read in order, skipping the ones ivpu does not use.
  ByteReader in(file);
  const uint32_t header_version = in.le32();
  in.skip(4);  // image_format
  const uint64_t image_load_address = in.le64();
  const uint32_t image_size = in.le32();
  const uint64_t entry_point = in.le64();
  in.skip(32 + 4);  // vpu_version, compression_type
  const uint64_t version_addr = in.le64();
  const uint32_t version_size = in.le32();
  const uint64_t boot_params_addr = in.le64();
  uint32_t api[kApiVersions];
  for (uint32_t& v : api) v = in.le32();
  const uint32_t runtime_size_field = in.le32();
  const uint32_t shave_nn_size = in.le32();
  const uint32_t preempt1 = in.le32(), preempt2 = in.le32(), preempt1_max = in.le32(), preempt2_max = in.le32();
  in.skip(16);  // preemption_reserved
  const uint64_t ro_start = in.le64();
  const uint32_t ro_size = in.le32();
  if (!in.ok()) return fail(IntelNpuFirmwareError::TooSmall);

  if (header_version != kHeaderVersion) return fail(IntelNpuFirmwareError::HeaderVersion);
  const IntelNpuMemoryMap map = IntelNpuMemoryMap::for_ip(ip);

  const uint64_t bp_size = kPageSize;
  if (!map.runtime.contains(boot_params_addr, bp_size)) return fail(IntelNpuFirmwareError::BootParamsAddress);

  const uint64_t ver_size = page_align_up(version_size);
  if (ver_size != kPageSize) return fail(IntelNpuFirmwareError::VersionSize);
  if (!map.runtime.contains(version_addr, ver_size)) return fail(IntelNpuFirmwareError::VersionAddress);

  // ivpu computes this in 64 bits from unsigned 32-bit fields and would wrap; a runtime too small for the two pages is an error here.
  if (runtime_size_field < bp_size + ver_size) return fail(IntelNpuFirmwareError::RuntimeSize);
  const uint64_t runtime_addr = image_load_address;
  const uint64_t runtime_size = runtime_size_field - bp_size - ver_size;
  if (!map.runtime.contains(runtime_addr, runtime_size)) return fail(IntelNpuFirmwareError::RuntimeAddress);
  if (kFileImageOffset + uint64_t{image_size} > file.size()) return fail(IntelNpuFirmwareError::ImageSize);
  if (!page_aligned(runtime_addr) || !page_aligned(runtime_size)) return fail(IntelNpuFirmwareError::RuntimeSize);
  if (runtime_size < image_size) return fail(IntelNpuFirmwareError::ImageSize);
  if (!map.runtime.contains(image_load_address, image_size)) return fail(IntelNpuFirmwareError::ImageAddress);

  const IntelNpuAddressRange image_range = {image_load_address, image_load_address + image_size};
  if (!image_range.contains(entry_point, kPageSize)) return fail(IntelNpuFirmwareError::EntryPoint);
  if (shave_nn_size > kShaveNnMaxSize) return fail(IntelNpuFirmwareError::ShaveSize);

  fw.version = file.subview(kHeaderSize, kVersionHeaderSize).first(255);
  for (uint64_t i = 0; i < fw.version.size(); ++i)
    if (fw.version[i] == 0) { fw.version = fw.version.first(i); break; }

  for (uint32_t i = 0; i < kApiVersions; ++i) fw.api_version[i] = api[i];
  const uint32_t boot_api = fw.api_version[kBootApiIndex], jsm_api = fw.api_version[kJsmApiIndex];
  if (api_major(boot_api) < kMinApiMajor) return fail(IntelNpuFirmwareError::BootApiVersion);
  if (api_major(jsm_api) < kMinApiMajor) return fail(IntelNpuFirmwareError::JsmApiVersion);

  fw.boot_params_addr = boot_params_addr;
  fw.boot_params_size = bp_size;
  fw.fw_version_addr = version_addr;
  fw.fw_version_size = ver_size;
  fw.runtime_addr = runtime_addr;
  fw.runtime_size = runtime_size;
  fw.image_load_offset = image_load_address - runtime_addr;
  fw.image_size = image_size;
  fw.shave_nn_size = static_cast<uint32_t>(page_align_up(shave_nn_size));
  fw.cold_boot_entry_point = entry_point;
  fw.image = file.subview(kFileImageOffset, image_size);

  // ivpu_fw_sched_mode_select(): the OS scheduler does not exist on 60xx; otherwise a forced mode, else the JSM API decides (HW from 3.24).
  if (ip >= IntelNpuIp::Ip60xx && sched_request == kIntelNpuSchedOs) fw.sched_mode = kIntelNpuSchedHw;
  else if (sched_request != kIntelNpuSchedAuto) fw.sched_mode = sched_request == kIntelNpuSchedOs ? kIntelNpuSchedOs : kIntelNpuSchedHw;
  else if (api_major(jsm_api) < 3 || (api_major(jsm_api) == 3 && api_minor(jsm_api) < 24)) fw.sched_mode = kIntelNpuSchedOs;
  else fw.sched_mode = kIntelNpuSchedHw;
  parse_preemption(fw, preempt1, preempt1_max, preempt2, preempt2_max);

  if (ro_start && !image_range.contains(ro_start, ro_size)) return fail(IntelNpuFirmwareError::ReadOnlySection);
  fw.read_only_addr = ro_start;
  fw.read_only_size = ro_size;
  fw.error = IntelNpuFirmwareError::None;
  return fw;
}

IntelNpuBootParamsError IntelNpuBoot::build_cold(ByteView page, const IntelNpuFirmware& fw, IntelNpuIp ip, const IntelNpuMemoryMap& map,
                                                 const IntelNpuBootInputs& in) {
  using E = IntelNpuBootParamsError;
  if (page.size() != kBootParamsSize) return E::PageSize;
  // The IPC buffer is split in two equal halves; both must lie in the global (uncached, shared) range.
  if (in.ipc_rx_size == 0 || (in.ipc_rx_size & 1) || !map.global.contains(in.ipc_rx_addr, in.ipc_rx_size)) return E::IpcBuffer;
  if (in.crit_log_size == 0 || !map.global.contains(in.crit_log_addr, in.crit_log_size) || in.verbose_log_size == 0 ||
      !map.global.contains(in.verbose_log_addr, in.verbose_log_size))
    return E::LogBuffer;
  if (fw.shave_nn_size != 0 && !map.shave.contains(in.shave_nn_addr, fw.shave_nn_size)) return E::ShaveAddress;

  page.fill(0);
  put32(page, kBootType, kBootCold);
  put32(page, kMagic, kBootParamsMagic);
  put32(page, kVpuId, in.pci_bus);
  put32(page, kPerfClk, in.perf_clk_frequency);

  put64(page, kSharedBase, map.global.start);
  put32(page, kSharedSize, static_cast<uint32_t>(map.global.end - map.global.start));
  const uint32_t half = in.ipc_rx_size / 2;
  put64(page, kIpcHeaderStart, in.ipc_rx_addr);
  put32(page, kIpcHeaderSize, half);
  put64(page, kIpcPayloadStart, in.ipc_rx_addr + half);
  put32(page, kIpcPayloadSize, half);
  if (ip == IntelNpuIp::Ip37xx) {
    put64(page, kPioBase, map.user.start);
    put32(page, kPioSize, static_cast<uint32_t>(map.user.end - map.user.start));
  }
  put32(page, kAutoconfig, 1);
  // L2 cache for the first 2 GiB of high memory: cache_defaults[NN] = {use 1, cfg = shave.start >> 31}.
  put8(page, kCacheDefaults + 2, 1);
  put8(page, kCacheDefaults + 3, static_cast<uint8_t>(map.shave.start >> 31));
  if (fw.shave_nn_size != 0) put64(page, kShaveNnBase, in.shave_nn_addr);

  put32(page, kWatchdogMss, kWatchdogIrqMss);
  put32(page, kWatchdogNce, kWatchdogIrqNce);
  put32(page, kSiStepping, in.si_stepping);
  put64(page, kDeviceId, in.device_id);
  put64(page, kFeatureExclusion, in.tile_fuse);
  put64(page, kSku, in.sku);
  put32(page, kMinRatio, in.min_ratio);
  put32(page, kPnRatio, in.pn_ratio);
  put32(page, kMaxRatio, in.max_ratio);

  put32(page, kTraceLevel, in.trace_level);
  put64(page, kTraceFormats, 1ull << kTraceFormatString);
  put32(page, kTraceDestination, kTraceDestinationVerbose);
  put64(page, kTraceHwComponents, ~0ull);
  put64(page, kCritBufAddr, in.crit_log_addr);
  put32(page, kCritBufSize, in.crit_log_size);
  put64(page, kVerboseBufAddr, in.verbose_log_addr);
  put32(page, kVerboseBufSize, in.verbose_log_size);

  put64(page, kTelemetryBase, in.telemetry_base);
  put64(page, kTelemetrySize, in.telemetry_size);
  put32(page, kTelemetryEnable, in.telemetry_enable ? 1 : 0);
  put32(page, kSchedulingMode, fw.sched_mode);
  if (fw.sched_mode == kIntelNpuSchedHw) put32(page, kFocusPresentTimer, kFocusPresentTimerMs);
  put32(page, kDvfsMode, 0);
  put32(page, kD0i3DelayedEntry, 1);
  if (in.disable_d0i2) put32(page, kPowerProfile, 1u << 1);
  put32(page, kEccMcaSignal, in.uses_ecc_mca_signal ? kMcaEccBoth : 0);
  put64(page, kSystemTime, in.system_time_us);
  return E::None;
}

IntelNpuBootParamsError IntelNpuBoot::update_warm(ByteView page, uint64_t residency_us, uint64_t entry_vpu_ts, uint64_t system_time_us) {
  if (page.size() != kBootParamsSize) return IntelNpuBootParamsError::PageSize;
  put64(page, kD0i3Residency, residency_us);
  put64(page, kD0i3EntryTs, entry_vpu_ts);
  put64(page, kSystemTime, system_time_us);
  put64(page, kSaveRestoreRet, 0);
  put32(page, kBootType, kBootWarm);
  return IntelNpuBootParamsError::None;
}

bool IntelNpuBoot::load_image(ByteView runtime, const IntelNpuFirmware& fw) {
  if (!fw.valid() || runtime.size() != fw.runtime_size || fw.image.size() != fw.image_size) return false;
  if (fw.image_load_offset > runtime.size() || fw.image_size > runtime.size() - fw.image_load_offset) return false;
  runtime.fill(0);
  return runtime.subview(fw.image_load_offset, fw.image_size).copy_from(fw.image);
}

IntelNpuStartResult intel_npu_start(IntelNpuHw& hw, const IntelNpuFirmware& fw, IntelNpuIp ip, const IntelNpuMemoryMap& map, const IntelNpuBootInputs& inputs,
                                    const IntelNpuBootMemory& memory) {
  IntelNpuStartResult r;
  if (!fw.valid() || memory.boot_params.size() != IntelNpuBoot::kBootParamsSize || memory.runtime.size() != fw.runtime_size) return r;  // stage Memory
  if (!IntelNpuBoot::load_image(memory.runtime, fw)) { r.stage = IntelNpuStartStage::Load; return r; }
  // The limits and the fused-off tiles come from the hardware itself, whatever the caller put in the inputs.
  IntelNpuBootInputs in = inputs;
  in.min_ratio = hw.info().min_ratio;
  in.pn_ratio = hw.info().pn_ratio;
  in.max_ratio = hw.info().max_ratio;
  in.tile_fuse = hw.info().tile_disable_mask;
  r.params_error = IntelNpuBoot::build_cold(memory.boot_params, fw, ip, map, in);
  if (r.params_error != IntelNpuBootParamsError::None) { r.stage = IntelNpuStartStage::BootParams; return r; }
  const IntelNpuResult up = hw.power_up();
  if (!up.ok) { r.stage = IntelNpuStartStage::PowerUp; r.hw_step = up.failed; return r; }
  const IntelNpuResult boot = hw.boot(fw.cold_boot_entry_point);
  if (!boot.ok) { r.stage = IntelNpuStartStage::CpuBoot; r.hw_step = boot.failed; return r; }
  r.ok = true;
  r.stage = IntelNpuStartStage::Done;
  return r;
}

}  // namespace hardware
