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

#include "Hardware/NeuralProcessingUnit/AMD/AmdXdnaDevice.hpp"

namespace hardware {

namespace {
constexpr uint32_t kAllOnes = 0xFFFFFFFFu;
constexpr uint32_t kClockGatingOn = 1;

// mgmt_mbox_chann_info word indexes (aie2_pci.c)
enum Aie2Info : uint32_t { kX2iTail, kX2iHead, kX2iBuf, kX2iBufSize, kI2xTail, kI2xHead, kI2xBuf, kI2xBufSize, kMagic, kMsiId, kProtMajor, kProtMinor };
// mailbox_info word indexes (aie4_pci.c)
enum Aie4Info : uint32_t {
  k4Valid, k4ProtMajor, k4ProtMinor, k4X2iTail, k4X2iHead, k4X2iBuf, k4X2iBufSize, k4I2xTail, k4I2xHead, k4I2xBuf, k4I2xBufSize, k4MsiIdx
};

// device address -> BAR offset; false if the address lies below the BAR's start.
bool to_offset(uint32_t device_addr, uint32_t base, uint32_t* out) {
  if (device_addr < base) return false;
  *out = device_addr - base;
  return true;
}
}  // namespace

bool amd_xdna_check_protocol(const AmdXdnaLayout& layout, uint32_t major, uint32_t minor, uint64_t* features) {
  bool found = false;
  uint64_t mask = 0;
  for (const AmdXdnaProtocolEntry& e : layout.protocol) {
    if (e.major != major || minor < e.min_minor) continue;
    if (e.max_minor > 0 && minor > e.max_minor) continue;
    mask |= e.features;
    found = true;
  }
  *features = found ? mask : 0;
  return found;
}

const char* amd_xdna_stage_name(AmdXdnaStage stage) {
  switch (stage) {
    case AmdXdnaStage::None: return "none";
    case AmdXdnaStage::Firmware: return "firmware";
    case AmdXdnaStage::Smu: return "SMU power";
    case AmdXdnaStage::Psp: return "PSP start";
    case AmdXdnaStage::Alive: return "firmware alive";
    case AmdXdnaStage::Channel: return "mailbox";
    case AmdXdnaStage::Protocol: return "protocol";
    case AmdXdnaStage::RuntimeConfig: return "runtime config";
    case AmdXdnaStage::Pasid: return "PASID";
    case AmdXdnaStage::TimeQuota: return "time quota";
    case AmdXdnaStage::Reset: return "suspend/resume";
    case AmdXdnaStage::Dpm: return "DPM level";
    case AmdXdnaStage::ClockGating: return "clock gating";
    case AmdXdnaStage::Query: return "query";
  }
  return "unknown";
}

AmdXdnaDevice::AmdXdnaDevice(const AmdXdnaLayout& layout, const AmdXdnaBars& bars, const IClock* clock)
    : layout_(layout), hw_(layout, bars, clock), mailbox_(hw_.bars(), clock), messages_(mailbox_) {}

AmdXdnaDevice::~AmdXdnaDevice() {
  if (running_) stop();
}

bool AmdXdnaDevice::aie2() const {
  return layout_.model == AmdXdnaModel::Npu1 || layout_.model == AmdXdnaModel::Npu4 || layout_.model == AmdXdnaModel::Npu5 ||
         layout_.model == AmdXdnaModel::Npu6;
}

AmdXdnaError AmdXdnaDevice::load_firmware(allocators::IAllocator& allocator, ConstByteView firmware, ConstByteView cert_firmware) {
  if (running_) return AmdXdnaError::BadState;
  return hw_.load_firmware(allocator, firmware, cert_firmware);
}

AmdXdnaError AmdXdnaDevice::fail(AmdXdnaStage stage, AmdXdnaError error) {
  failed_stage_ = stage;
  return error;
}

AmdXdnaError AmdXdnaDevice::read_channel_aie2(AmdXdnaChannelConfig* out) {
  uint32_t addr = 0;
  AmdXdnaDeadline deadline(hw_.clock(), AmdXdnaHw::kTimeoutUs);
  while (true) {
    if (!hw_.bars().read32(layout_.sram_bar, layout_.fw_alive_offset, &addr)) return AmdXdnaError::Registers;
    if (addr != 0 && addr != kAllOnes) break;
    if (deadline.expired()) return AmdXdnaError::NotAlive;
  }
  uint32_t info[AmdXdnaHw::kMgmtInfoWords] = {};
  uint32_t offset = 0;
  bool ok = to_offset(addr, layout_.sram_dev_addr, &offset) && (offset & 3) == 0 &&
            hw_.bars().contains(layout_.sram_bar, offset, AmdXdnaHw::kMgmtInfoWords * 4);
  for (uint32_t i = 0; ok && i < AmdXdnaHw::kMgmtInfoWords; ++i) ok = hw_.bars().read32(layout_.sram_bar, offset + i * 4, &info[i]);
  hw_.bars().write32(layout_.sram_bar, layout_.fw_alive_offset, 0);  // the driver always clears it once it has looked
  if (!ok || info[kMagic] != AmdXdnaHw::kMgmtMagic) return AmdXdnaError::BadChannel;
  AmdXdnaChannelConfig c;
  c.ring_bar = layout_.sram_bar;
  c.reg_bar = layout_.mbox_bar;
  const uint32_t mbox = layout_.mbox_dev_addr, sram = layout_.sram_dev_addr;
  if (!to_offset(info[kX2iHead], mbox, &c.x2i.head_reg) || !to_offset(info[kX2iTail], mbox, &c.x2i.tail_reg) ||
      !to_offset(info[kX2iBuf], sram, &c.x2i.start) || !to_offset(info[kI2xHead], mbox, &c.i2x.head_reg) ||
      !to_offset(info[kI2xTail], mbox, &c.i2x.tail_reg) || !to_offset(info[kI2xBuf], sram, &c.i2x.start))
    return AmdXdnaError::BadChannel;
  c.x2i.size = info[kX2iBufSize];
  c.i2x.size = info[kI2xBufSize];
  if (c.i2x.head_reg > UINT32_MAX - 4) return AmdXdnaError::BadChannel;
  c.has_irq_reg = true;
  c.irq_reg = c.i2x.head_reg + 4;  // the interrupt status register sits after the response ring's head register
  c.msix = info[kMsiId];
  protocol_major_ = info[kProtMajor];
  protocol_minor_ = info[kProtMinor];
  *out = c;
  return AmdXdnaError::None;
}

AmdXdnaError AmdXdnaDevice::read_channel_aie4(AmdXdnaChannelConfig* out) {
  uint32_t valid = 0;
  AmdXdnaDeadline deadline(hw_.clock(), AmdXdnaHw::kTimeoutUs);
  while (true) {
    if (!hw_.bars().read32(layout_.sram_bar, layout_.mbox_info_offset, &valid)) return AmdXdnaError::Registers;
    if (valid == 1) break;
    if (deadline.expired()) return AmdXdnaError::NotAlive;
  }
  uint32_t info[AmdXdnaHw::kAie4InfoWords] = {};
  for (uint32_t i = 0; i < AmdXdnaHw::kAie4InfoWords; ++i)
    if (!hw_.bars().read32(layout_.sram_bar, uint64_t{layout_.mbox_info_offset} + i * 4, &info[i])) return AmdXdnaError::BadChannel;
  AmdXdnaChannelConfig c;
  c.ring_bar = layout_.sram_bar;  // the ring-buffer BAR; the offsets are BAR offsets already
  c.reg_bar = layout_.mbox_bar;
  c.x2i = {info[k4X2iBuf], info[k4X2iBufSize], info[k4X2iHead], info[k4X2iTail]};
  c.i2x = {info[k4I2xBuf], info[k4I2xBufSize], info[k4I2xHead], info[k4I2xTail]};
  c.has_irq_reg = false;  // NO_IOHUB
  c.msix = info[k4MsiIdx];
  protocol_major_ = info[k4ProtMajor];
  protocol_minor_ = info[k4ProtMinor];
  *out = c;
  return AmdXdnaError::None;
}

AmdXdnaError AmdXdnaDevice::read_channel(AmdXdnaChannelConfig* out) {
  if (!layout_.valid()) return AmdXdnaError::NoDevice;
  return aie2() ? read_channel_aie2(out) : read_channel_aie4(out);
}

AmdXdnaError AmdXdnaDevice::runtime_config(AmdXdnaRtCategory category, const uint32_t* value) {
  for (const AmdXdnaRuntimeConfig& row : layout_.runtime_config) {
    if (row.category != category) continue;
    if (row.skip_if_features && (features_ & row.skip_if_features) == row.skip_if_features) continue;
    const AmdXdnaError e = messages_.set_runtime_config(row.type, value ? *value : row.value);
    if (e != AmdXdnaError::None) return e;
  }
  return AmdXdnaError::None;
}

void AmdXdnaDevice::power_down(bool psp_started) {
  mailbox_.stop();
  if (psp_started) hw_.psp_stop();
  if (layout_.smu[static_cast<uint8_t>(AmdXdnaSmuReg::Cmd)].present()) {
    if (!layout_.dpm.empty()) hw_.set_dpm_level(0);  // aie2_smu_fini: lowest level first
    hw_.smu_power_off();
  }
}

AmdXdnaError AmdXdnaDevice::start_aie2() {
  AmdXdnaError e = hw_.smu_init();
  if (e != AmdXdnaError::None) return fail(AmdXdnaStage::Smu, e);  // as the driver: an SMU that failed is not asked again
  e = hw_.psp_start();
  if (e != AmdXdnaError::None) {
    power_down(false);
    return fail(AmdXdnaStage::Psp, e);
  }
  AmdXdnaChannelConfig channel;
  e = read_channel_aie2(&channel);
  if (e != AmdXdnaError::None) {
    power_down(true);
    return fail(e == AmdXdnaError::NotAlive ? AmdXdnaStage::Alive : AmdXdnaStage::Channel, e);
  }
  if (!amd_xdna_check_protocol(layout_, protocol_major_, protocol_minor_, &features_)) {
    power_down(true);
    return fail(AmdXdnaStage::Protocol, AmdXdnaError::Protocol);
  }
  e = mailbox_.start(channel);
  if (e != AmdXdnaError::None) {
    power_down(true);
    return fail(AmdXdnaStage::Channel, e);
  }
  // aie2_mgmt_fw_init()
  AmdXdnaStage stage = AmdXdnaStage::RuntimeConfig;
  e = runtime_config(AmdXdnaRtCategory::Init, nullptr);
  if (e == AmdXdnaError::None) {
    stage = AmdXdnaStage::Pasid;
    e = messages_.assign_mgmt_pasid(0);
  }
  if (e == AmdXdnaError::None && has_feature(AmdXdnaFeature::UpdateProperty)) {  // skipped quietly without the feature, as the driver
    stage = AmdXdnaStage::TimeQuota;
    e = messages_.update_time_quota(kDefaultTimeQuantumUs);
  }
  if (e == AmdXdnaError::None) {  // aie2_xdna_reset(): suspend, wait for the wait mode, resume
    stage = AmdXdnaStage::Reset;
    e = messages_.suspend();
    if (e == AmdXdnaError::None) e = hw_.psp_wait_mode();
    if (e == AmdXdnaError::None) e = messages_.resume();
  }
  // aie2_pm_init(): the highest DPM level, clock gating on.
  if (e == AmdXdnaError::None) {
    stage = AmdXdnaStage::Dpm;
    e = hw_.set_dpm_level(hw_.max_dpm_level());
  }
  if (e == AmdXdnaError::None) {
    stage = AmdXdnaStage::ClockGating;
    const uint32_t on = kClockGatingOn;
    e = runtime_config(AmdXdnaRtCategory::ClockGating, &on);
  }
  // aie2_mgmt_fw_query()
  if (e == AmdXdnaError::None) {
    stage = AmdXdnaStage::Query;
    e = messages_.query_firmware_version(&firmware_version_);
    if (e == AmdXdnaError::None) e = messages_.query_aie_version(&aie_major_, &aie_minor_);
    if (e == AmdXdnaError::None) e = messages_.query_aie_tile_info(&tile_info_);
  }
  if (e != AmdXdnaError::None) {
    if (!mailbox_.broken()) messages_.suspend();
    power_down(true);
    return fail(stage, e);
  }
  return AmdXdnaError::None;
}

AmdXdnaError AmdXdnaDevice::start_aie4() {
  const bool pf = layout_.smu[static_cast<uint8_t>(AmdXdnaSmuReg::Cmd)].present();
  if (pf) {
    AmdXdnaError e = hw_.smu_init();
    if (e != AmdXdnaError::None) return fail(AmdXdnaStage::Smu, e);
    e = hw_.psp_start();
    if (e != AmdXdnaError::None) {
      power_down(false);
      return fail(AmdXdnaStage::Psp, e);
    }
  }
  AmdXdnaChannelConfig channel;
  AmdXdnaError e = read_channel_aie4(&channel);
  if (e == AmdXdnaError::None && !amd_xdna_check_protocol(layout_, protocol_major_, protocol_minor_, &features_)) e = AmdXdnaError::Protocol;
  if (e == AmdXdnaError::None) e = mailbox_.start(channel);
  if (e != AmdXdnaError::None) {
    power_down(pf);
    return fail(e == AmdXdnaError::NotAlive ? AmdXdnaStage::Alive : e == AmdXdnaError::Protocol ? AmdXdnaStage::Protocol : AmdXdnaStage::Channel, e);
  }
  return AmdXdnaError::None;
}

AmdXdnaError AmdXdnaDevice::start() {
  if (!layout_.valid()) return fail(AmdXdnaStage::None, AmdXdnaError::NoDevice);
  if (running_) return AmdXdnaError::BadState;
  failed_stage_ = AmdXdnaStage::None;
  features_ = 0;
  const bool needs_firmware = layout_.psp[static_cast<uint8_t>(AmdXdnaPspReg::Cmd)].present();
  if (needs_firmware && hw_.firmware_buffer_size() == 0) return fail(AmdXdnaStage::Firmware, AmdXdnaError::BadState);
  const AmdXdnaError e = aie2() ? start_aie2() : start_aie4();
  if (e == AmdXdnaError::None) running_ = true;
  return e;
}

AmdXdnaError AmdXdnaDevice::stop() {
  if (!running_) return AmdXdnaError::BadState;
  running_ = false;
  AmdXdnaError first = AmdXdnaError::None;
  auto note = [&first](AmdXdnaError e) {
    if (first == AmdXdnaError::None) first = e;
  };
  if (aie2()) {
    note(runtime_config(AmdXdnaRtCategory::ClockGating, nullptr));
    AmdXdnaError s = messages_.suspend();
    if (s == AmdXdnaError::None) s = hw_.psp_wait_mode();
    note(s);
  }
  mailbox_.stop();
  const bool pf = layout_.psp[static_cast<uint8_t>(AmdXdnaPspReg::Cmd)].present();
  if (pf) note(hw_.psp_stop());
  if (layout_.smu[static_cast<uint8_t>(AmdXdnaSmuReg::Cmd)].present()) {
    if (!layout_.dpm.empty()) note(hw_.set_dpm_level(0));
    note(hw_.smu_power_off());
  }
  return first;
}

}  // namespace hardware
