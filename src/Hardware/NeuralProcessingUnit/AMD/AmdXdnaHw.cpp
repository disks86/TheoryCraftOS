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

#include "Hardware/NeuralProcessingUnit/AMD/AmdXdnaHw.hpp"
#include "Utility/AllocatedView.hpp"

namespace hardware {

namespace {
// Memory from IAllocator::allocate_dma is identity mapped and below 4 GiB, so its address is also the bus address the PSP is given; this is the only place a
// pointer becomes an address. Only for the firmware buffers this file allocated.
uint64_t bus_address_of(const void* dma_memory) { return reinterpret_cast<uint64_t>(dma_memory); }

constexpr uint32_t kAllOnes = 0xFFFFFFFFu;
constexpr uint32_t kPspStartCopyFirmware = 1;   // PSP_START_COPY_FW
constexpr uint32_t kAie4Notify = 0xD007BE11u;   // PSP_NOTIFY_INTR
constexpr uint32_t kWaitModeIdle = 1u << 0;

constexpr AmdXdnaRegister reg(uint8_t bar, uint32_t aperture_address, uint32_t bar_base) { return {bar, aperture_address - bar_base}; }
constexpr uint8_t psp_index(AmdXdnaPspReg r) { return static_cast<uint8_t>(r); }
constexpr uint8_t smu_index(AmdXdnaSmuReg r) { return static_cast<uint8_t>(r); }

constexpr uint64_t f(AmdXdnaFeature x) { return amd_xdna_feature_bit(x); }
constexpr uint64_t kAllFeatures = (uint64_t{1} << static_cast<uint8_t>(AmdXdnaFeature::Count)) - 1;

// npu1_regs.c
constexpr AmdXdnaDpmLevel kNpu1Dpm[] = {{400, 800}, {600, 1024}, {600, 1024}, {600, 1024}, {600, 1024}, {720, 1309}, {720, 1309}, {847, 1600}};
constexpr AmdXdnaProtocolEntry kNpu1Protocol[] = {{5, 7, 0, 0}, {5, 8, 0, f(AmdXdnaFeature::NpuCommand)}};
constexpr AmdXdnaRuntimeConfig kNpu1RuntimeConfig[] = {
    {2, 1, AmdXdnaRtCategory::Init, 0},         // PDI app load mode
    {4, 1, AmdXdnaRtCategory::Init, 0},         // debug buffer
    {1, 1, AmdXdnaRtCategory::ClockGating, 0},  // clock gating on
};
// npu4_regs.c (npu5 and npu6 use the same tables)
constexpr AmdXdnaDpmLevel kNpu4Dpm[] = {{396, 792}, {600, 1056}, {792, 1152}, {975, 1267}, {975, 1267}, {1056, 1408}, {1152, 1584}, {1267, 1800}};
constexpr AmdXdnaProtocolEntry kNpu4Protocol[] = {
    {6, 12, 0, 0},
    {6, 12, 0, f(AmdXdnaFeature::Preempt)},
    {6, 12, 0, f(AmdXdnaFeature::TemporalOnly)},
    {6, 15, 0, f(AmdXdnaFeature::NpuCommand)},
    {6, 15, 0, f(AmdXdnaFeature::UpdateProperty)},
    {6, 18, 0, f(AmdXdnaFeature::AppHealth)},
    {6, 18, 0, f(AmdXdnaFeature::AddHostBuffer)},
    {6, 24, 0, f(AmdXdnaFeature::GetDevRevision)},
    {7, 0, 0, kAllFeatures},
};
constexpr AmdXdnaRuntimeConfig kNpu4RuntimeConfig[] = {
    {5, 1, AmdXdnaRtCategory::Init, 0},                                // PDI app load mode
    {10, 1, AmdXdnaRtCategory::Init, 0},                               // debug buffer
    {14, 0, AmdXdnaRtCategory::Init, f(AmdXdnaFeature::Preempt)},      // frame boundary preemption
    {1, 1, AmdXdnaRtCategory::ClockGating, 0},
    {2, 1, AmdXdnaRtCategory::ClockGating, 0},
    {3, 1, AmdXdnaRtCategory::ClockGating, 0},
    {4, 1, AmdXdnaRtCategory::ClockGating, 0},
    {13, 0, AmdXdnaRtCategory::ForcePreempt, 0},
    {14, 0, AmdXdnaRtCategory::FrameBoundaryPreempt, 0},
};
// npu3_regs.c
constexpr AmdXdnaProtocolEntry kNpu3Protocol[] = {{5, 10, 0, 0}};

constexpr AmdXdnaLayout make_npu1() {
  // BARs: registers, PSP and SMU in BAR 0 (aperture 0 at 0x3000000), SRAM in BAR 2 (aperture 1 at 0x3080000), mailbox registers in BAR 4 (aperture 2 at 0x30C0000).
  constexpr uint32_t kAp0 = 0x3000000, kAp1 = 0x3080000, kAp2 = 0x30C0000;
  AmdXdnaLayout l;
  l.model = AmdXdnaModel::Npu1;
  l.reg_bar = 0; l.mbox_bar = 4; l.sram_bar = 2; l.psp_bar = 0; l.smu_bar = 0;
  l.mbox_dev_addr = kAp2; l.sram_dev_addr = kAp1;
  l.mbox_channel_offset = 0x30A0000 - kAp1;  // MPNPU_SRAM_X2I_MAILBOX_0
  l.fw_alive_offset = 0x30BF000 - kAp1;      // MPNPU_SRAM_I2X_MAILBOX_15
  l.psp[psp_index(AmdXdnaPspReg::Cmd)] = reg(0, 0x30100A0, kAp0);       // MPNPU_PUB_SCRATCH2
  l.psp[psp_index(AmdXdnaPspReg::Arg0)] = reg(0, 0x30100A4, kAp0);      // SCRATCH3
  l.psp[psp_index(AmdXdnaPspReg::Arg1)] = reg(0, 0x30100A8, kAp0);      // SCRATCH4
  l.psp[psp_index(AmdXdnaPspReg::Arg2)] = reg(0, 0x30100BC, kAp0);      // SCRATCH9
  l.psp[psp_index(AmdXdnaPspReg::Intr)] = reg(0, 0x3010090, kAp0);      // MPNPU_PUB_SEC_INTR
  l.psp[psp_index(AmdXdnaPspReg::Status)] = reg(0, 0x30100A0, kAp0);    // SCRATCH2
  l.psp[psp_index(AmdXdnaPspReg::Resp)] = reg(0, 0x30100A4, kAp0);      // SCRATCH3
  l.psp[psp_index(AmdXdnaPspReg::WaitMode)] = reg(0, 0x3010034, kAp0);  // MPNPU_PWAITMODE
  l.smu[smu_index(AmdXdnaSmuReg::Cmd)] = reg(0, 0x30100AC, kAp0);       // SCRATCH5
  l.smu[smu_index(AmdXdnaSmuReg::Arg)] = reg(0, 0x30100B4, kAp0);       // SCRATCH7
  l.smu[smu_index(AmdXdnaSmuReg::Intr)] = reg(0, 0x3010094, kAp0);      // MPNPU_PUB_PWRMGMT_INTR
  l.smu[smu_index(AmdXdnaSmuReg::Resp)] = reg(0, 0x30100B0, kAp0);      // SCRATCH6
  l.smu[smu_index(AmdXdnaSmuReg::Out)] = reg(0, 0x30100B4, kAp0);       // SCRATCH7
  l.psp_arg2_mask = 0x00FFFFFFu;
  l.psp_notify = 1;
  l.firmware_names[0] = "npu_7.sbin";
  l.firmware_names[1] = "npu.sbin";
  l.first_col = 1;
  l.col_opc = 2048;
  l.hwctx_limit = 6;
  l.dpm_by_level = false;
  l.dpm = ArrayView<const AmdXdnaDpmLevel>(kNpu1Dpm);
  l.protocol = ArrayView<const AmdXdnaProtocolEntry>(kNpu1Protocol);
  l.runtime_config = ArrayView<const AmdXdnaRuntimeConfig>(kNpu1RuntimeConfig);
  l.bar_window[0] = 0x20000;   // up to SCRATCH9 at 0x100BC
  l.bar_window[2] = 0x40000;   // aperture 1 up to aperture 2
  l.bar_window[4] = 0x10000;   // mailbox registers: firmware-reported, bounded here
  return l;
}

constexpr AmdXdnaLayout make_npu456(AmdXdnaModel model) {
  // BARs: registers and mailbox registers in BAR 0 (aperture 0 at 0x3000000), SRAM in BAR 2 (aperture 1 at 0x3600000), PSP (MP0) in BAR 4 (aperture 3 at
  // 0x3810000), SMU (MP1) in BAR 5 (aperture 4 at 0x3B10000). npu4_regs.c, npu5_regs.c and npu6_regs.c are identical in this.
  constexpr uint32_t kAp0 = 0x3000000, kAp1 = 0x3600000, kAp3 = 0x3810000, kAp4 = 0x3B10000;
  AmdXdnaLayout l;
  l.model = model;
  l.reg_bar = 0; l.mbox_bar = 0; l.sram_bar = 2; l.psp_bar = 4; l.smu_bar = 5;
  l.mbox_dev_addr = kAp0; l.sram_dev_addr = kAp1;
  l.mbox_channel_offset = 0x3600000 - kAp1;  // MPNPU_SRAM_X2I_MAILBOX_0
  l.fw_alive_offset = 0x361E000 - kAp1;      // MPNPU_SRAM_X2I_MAILBOX_15
  l.psp[psp_index(AmdXdnaPspReg::Cmd)] = reg(4, 0x3810AEC, kAp3);       // MP0_C2PMSG_123
  l.psp[psp_index(AmdXdnaPspReg::Arg0)] = reg(0, 0x3010078, kAp0);      // MPNPU_PUB_SCRATCH3
  l.psp[psp_index(AmdXdnaPspReg::Arg1)] = reg(0, 0x301007C, kAp0);      // SCRATCH4
  l.psp[psp_index(AmdXdnaPspReg::Arg2)] = reg(0, 0x3010090, kAp0);      // SCRATCH9
  l.psp[psp_index(AmdXdnaPspReg::Intr)] = reg(4, 0x3810A24, kAp3);      // MP0_C2PMSG_73
  l.psp[psp_index(AmdXdnaPspReg::Status)] = reg(4, 0x3810AEC, kAp3);    // MP0_C2PMSG_123
  l.psp[psp_index(AmdXdnaPspReg::Resp)] = reg(0, 0x3010078, kAp0);      // SCRATCH3
  l.psp[psp_index(AmdXdnaPspReg::WaitMode)] = reg(0, 0x301003C, kAp0);  // MPNPU_PWAITMODE
  l.smu[smu_index(AmdXdnaSmuReg::Cmd)] = reg(5, 0x3B10900, kAp4);       // MP1_C2PMSG_0
  l.smu[smu_index(AmdXdnaSmuReg::Arg)] = reg(5, 0x3B109F0, kAp4);       // MP1_C2PMSG_60
  l.smu[smu_index(AmdXdnaSmuReg::Intr)] = reg(5, kAp4, kAp4);           // MMNPU_APERTURE4_BASE
  l.smu[smu_index(AmdXdnaSmuReg::Resp)] = reg(5, 0x3B109F4, kAp4);      // MP1_C2PMSG_61
  l.smu[smu_index(AmdXdnaSmuReg::Out)] = reg(5, 0x3B109F0, kAp4);       // MP1_C2PMSG_60
  l.psp_arg2_mask = 0x00FFFFFFu;
  l.psp_notify = 1;
  l.firmware_names[0] = "npu_7.sbin";
  l.firmware_names[1] = "npu.sbin";
  l.first_col = 0;
  l.col_opc = 4096;
  l.hwctx_limit = 16;
  l.dpm_by_level = true;
  l.dpm = ArrayView<const AmdXdnaDpmLevel>(kNpu4Dpm);
  l.protocol = ArrayView<const AmdXdnaProtocolEntry>(kNpu4Protocol);
  l.runtime_config = ArrayView<const AmdXdnaRuntimeConfig>(kNpu4RuntimeConfig);
  l.bar_window[0] = 0x100000;  // public registers at 0x10000..0x100A8; mailbox registers firmware-reported, bounded here
  l.bar_window[2] = 0x40000;   // SRAM mailboxes 0..31 (0x3600000..0x3640000)
  l.bar_window[4] = 0x1000;    // MP0_C2PMSG_123 at 0xAEC
  l.bar_window[5] = 0x1000;    // MP1_C2PMSG_61 at 0x9F4
  return l;
}

constexpr AmdXdnaLayout make_npu3(bool virtual_function) {
  constexpr uint32_t kAp3 = 0x3810000, kAp4 = 0x3B10000;
  AmdXdnaLayout l;
  l.model = virtual_function ? AmdXdnaModel::Npu3Vf : AmdXdnaModel::Npu3Pf;
  l.reg_bar = 0; l.mbox_bar = 0; l.sram_bar = 2;   // NPU3_MBOX_BAR, NPU3_MBOX_BUFFER_BAR
  l.mbox_info_offset = 0;                          // NPU3_MBOX_INFO_OFF
  l.protocol = ArrayView<const AmdXdnaProtocolEntry>(kNpu3Protocol);
  l.bar_window[0] = 0x100000;  // mailbox registers: firmware-reported, bounded here
  l.bar_window[2] = 0x100000;  // mailbox_info and ring buffers: firmware-reported, bounded here
  if (virtual_function) return l;  // a VF has no SMU, no PSP and loads no firmware
  l.psp_bar = 4; l.smu_bar = 5;
  l.psp[psp_index(AmdXdnaPspReg::Cmd)] = reg(4, 0x3810AEC, kAp3);       // MPASP_C2PMSG_123_ALT_1
  l.psp[psp_index(AmdXdnaPspReg::Arg0)] = reg(4, 0x3810B70, kAp3);      // MPASP_C2PMSG_156_ALT_1
  l.psp[psp_index(AmdXdnaPspReg::Arg1)] = reg(4, 0x3810B74, kAp3);      // MPASP_C2PMSG_157_ALT_1
  l.psp[psp_index(AmdXdnaPspReg::Arg2)] = reg(4, 0x3810AEC, kAp3);      // the command register again: ARG2 carries the command in its top byte
  l.psp[psp_index(AmdXdnaPspReg::Intr)] = reg(4, 0x3810A24, kAp3);      // MPASP_C2PMSG_73_ALT_1
  l.psp[psp_index(AmdXdnaPspReg::Status)] = reg(4, 0x3810AEC, kAp3);
  l.psp[psp_index(AmdXdnaPspReg::Resp)] = reg(4, 0x3810B70, kAp3);
  // no PWAITMODE register on npu3
  l.smu[smu_index(AmdXdnaSmuReg::Cmd)] = reg(5, 0x3B109EC, kAp4);       // MP1_C2PMSG_59_ALT_1
  l.smu[smu_index(AmdXdnaSmuReg::Arg)] = reg(5, 0x3B109F4, kAp4);       // MP1_C2PMSG_61_ALT_1
  l.smu[smu_index(AmdXdnaSmuReg::Intr)] = reg(5, kAp4, kAp4);
  l.smu[smu_index(AmdXdnaSmuReg::Resp)] = reg(5, 0x3B109F0, kAp4);      // MP1_C2PMSG_60_ALT_1
  l.smu[smu_index(AmdXdnaSmuReg::Out)] = reg(5, 0x3B109F4, kAp4);
  l.psp_arg2_mask = 0xFFFFFFFFu;
  l.psp_notify = kAie4Notify;
  l.firmware_names[0] = "npu.dev.sbin";
  l.cert_firmware = "cert.dev.sbin";
  l.bar_window[4] = 0x1000;
  l.bar_window[5] = 0x1000;
  return l;
}

constexpr AmdXdnaLayout kNone{};
constexpr AmdXdnaLayout kNpu1 = make_npu1();
constexpr AmdXdnaLayout kNpu4 = make_npu456(AmdXdnaModel::Npu4);
constexpr AmdXdnaLayout kNpu5 = make_npu456(AmdXdnaModel::Npu5);
constexpr AmdXdnaLayout kNpu6 = make_npu456(AmdXdnaModel::Npu6);
constexpr AmdXdnaLayout kNpu3Pf = make_npu3(false);
constexpr AmdXdnaLayout kNpu3Vf = make_npu3(true);
}  // namespace

const AmdXdnaLayout& amd_xdna_layout(AmdXdnaModel model) {
  switch (model) {
    case AmdXdnaModel::Npu1: return kNpu1;
    case AmdXdnaModel::Npu4: return kNpu4;
    case AmdXdnaModel::Npu5: return kNpu5;
    case AmdXdnaModel::Npu6: return kNpu6;
    case AmdXdnaModel::Npu3Pf: return kNpu3Pf;
    case AmdXdnaModel::Npu3Vf: return kNpu3Vf;
    default: return kNone;
  }
}

const char* amd_xdna_error_name(AmdXdnaError error) {
  switch (error) {
    case AmdXdnaError::None: return "none";
    case AmdXdnaError::NoDevice: return "no device";
    case AmdXdnaError::BadState: return "bad state";
    case AmdXdnaError::Registers: return "registers not mapped";
    case AmdXdnaError::Timeout: return "timeout";
    case AmdXdnaError::SmuFailed: return "SMU error";
    case AmdXdnaError::PspFailed: return "PSP error";
    case AmdXdnaError::BadFirmware: return "bad firmware";
    case AmdXdnaError::NoMemory: return "out of memory";
    case AmdXdnaError::NotAlive: return "firmware not alive";
    case AmdXdnaError::BadChannel: return "bad mailbox description";
    case AmdXdnaError::Protocol: return "unsupported protocol";
    case AmdXdnaError::ChannelBroken: return "mailbox broken";
    case AmdXdnaError::NoSlot: return "no message slot";
    case AmdXdnaError::BadMessage: return "bad message";
    case AmdXdnaError::BadResponse: return "bad response";
    case AmdXdnaError::DeviceStatus: return "firmware error status";
    case AmdXdnaError::Unsupported: return "unsupported";
    case AmdXdnaError::BadArgument: return "bad argument";
  }
  return "unknown";
}

bool AmdXdnaBars::contains(uint8_t index, uint64_t offset, uint64_t length) const {
  if (index >= 6 || !bar[index].regs) return false;
  const uint64_t size = bar[index].size;
  return offset <= size && length <= size - offset;
}

bool AmdXdnaBars::read32(uint8_t index, uint64_t offset, uint32_t* value) const {
  if ((offset & 3) != 0 || !contains(index, offset, 4)) return false;
  *value = bar[index].regs->read32(offset);
  return true;
}

bool AmdXdnaBars::write32(uint8_t index, uint64_t offset, uint32_t value) const {
  if ((offset & 3) != 0 || !contains(index, offset, 4)) return false;
  bar[index].regs->write32(offset, value);
  return true;
}

AmdXdnaHw::~AmdXdnaHw() { release_buffers(); }

void AmdXdnaHw::release_buffers() {
  if (!allocator_) return;
  release_view(*allocator_, fw_buffer_);
  release_view(*allocator_, cert_buffer_);
  fw_size_ = cert_size_ = 0;
  fw_bus_ = cert_bus_ = 0;
}

bool AmdXdnaHw::poll(AmdXdnaRegister r, uint32_t mask, uint32_t expected) const {
  AmdXdnaDeadline deadline(clock_, kTimeoutUs);
  uint32_t value = 0;
  while (true) {
    if (!bars_.read32(r, &value)) return false;
    if ((value & mask) == expected) return true;
    if (deadline.expired()) return false;
  }
}

AmdXdnaProbe AmdXdnaHw::probe() const {
  AmdXdnaProbe p;
  if (!layout_.valid()) return p;
  bool any = false, all_ones = true;
  uint32_t v = 0;
  const AmdXdnaRegister status = layout_.psp[psp_index(AmdXdnaPspReg::Status)];
  if (status.present() && bars_.read32(status, &v)) {
    any = true;
    all_ones = all_ones && v == kAllOnes;
    p.psp_status = v;
    p.psp_ready = (v & kPspStatusReady) != 0;
  }
  const AmdXdnaRegister resp = layout_.smu[smu_index(AmdXdnaSmuReg::Resp)];
  if (resp.present() && bars_.read32(resp, &v)) {
    any = true;
    all_ones = all_ones && v == kAllOnes;
    p.smu_response = v;
  }
  const AmdXdnaRegister wait = layout_.psp[psp_index(AmdXdnaPspReg::WaitMode)];
  if (wait.present() && bars_.read32(wait, &v)) {
    any = true;
    all_ones = all_ones && v == kAllOnes;
    p.wait_mode = (v & kWaitModeIdle) != 0;
  }
  const bool aie4 = layout_.model == AmdXdnaModel::Npu3Pf || layout_.model == AmdXdnaModel::Npu3Vf;
  if (bars_.read32(layout_.sram_bar, aie4 ? layout_.mbox_info_offset : layout_.fw_alive_offset, &v)) {
    any = true;
    all_ones = all_ones && v == kAllOnes;
    p.fw_alive_value = v;
    p.firmware_alive = aie4 ? v == 1 : (v != 0 && v != kAllOnes);
  }
  p.valid = any && !all_ones;  // a device that is off the bus (or not decoding memory) reads all-ones everywhere
  if (!p.valid) p = AmdXdnaProbe{};
  return p;
}

AmdXdnaError AmdXdnaHw::smu_exec(AmdXdnaSmuCommand command, uint32_t argument, uint32_t* out) {
  const AmdXdnaRegister cmd = layout_.smu[smu_index(AmdXdnaSmuReg::Cmd)], arg = layout_.smu[smu_index(AmdXdnaSmuReg::Arg)];
  const AmdXdnaRegister intr = layout_.smu[smu_index(AmdXdnaSmuReg::Intr)], resp = layout_.smu[smu_index(AmdXdnaSmuReg::Resp)];
  const AmdXdnaRegister outr = layout_.smu[smu_index(AmdXdnaSmuReg::Out)];
  if (!cmd.present()) return AmdXdnaError::Registers;
  if (!bars_.write32(resp, 0) || !bars_.write32(arg, argument) || !bars_.write32(cmd, static_cast<uint32_t>(command)) || !bars_.write32(intr, 0) ||
      !bars_.write32(intr, 1))
    return AmdXdnaError::Registers;
  AmdXdnaDeadline deadline(clock_, kTimeoutUs);
  uint32_t value = 0;
  while (true) {
    if (!bars_.read32(resp, &value)) return AmdXdnaError::Registers;
    if (value != 0) break;
    if (deadline.expired()) return AmdXdnaError::Timeout;
  }
  last_smu_response_ = value;
  if (value != kSmuResultOk) return AmdXdnaError::SmuFailed;
  if (out && !bars_.read32(outr, out)) return AmdXdnaError::Registers;
  return AmdXdnaError::None;
}

AmdXdnaError AmdXdnaHw::smu_init() {
  const AmdXdnaError off = smu_exec(AmdXdnaSmuCommand::PowerOff, 0, nullptr);
  if (off != AmdXdnaError::None) return off;
  return smu_exec(AmdXdnaSmuCommand::PowerOn, 0, nullptr);
}

AmdXdnaError AmdXdnaHw::smu_power_off() { return smu_exec(AmdXdnaSmuCommand::PowerOff, 0, nullptr); }

AmdXdnaError AmdXdnaHw::smu_set_clocks(uint32_t* npuclk_mhz, uint32_t* hclk_mhz) {
  if (npuclk_mhz) {
    uint32_t actual = 0;
    const AmdXdnaError e = smu_exec(AmdXdnaSmuCommand::SetMpNpuClock, *npuclk_mhz, &actual);
    if (e != AmdXdnaError::None) return e;
    *npuclk_mhz = actual;
  }
  if (hclk_mhz) {
    uint32_t actual = 0;
    const AmdXdnaError e = smu_exec(AmdXdnaSmuCommand::SetHClock, *hclk_mhz, &actual);
    if (e != AmdXdnaError::None) return e;
    *hclk_mhz = actual;
  }
  return AmdXdnaError::None;
}

AmdXdnaError AmdXdnaHw::smu_set_dpm(uint32_t level) {
  const AmdXdnaError hard = smu_exec(AmdXdnaSmuCommand::SetHardDpmLevel, level, nullptr);
  if (hard != AmdXdnaError::None) return hard;
  return smu_exec(AmdXdnaSmuCommand::SetSoftDpmLevel, level, nullptr);
}

AmdXdnaError AmdXdnaHw::set_dpm_level(uint32_t level) {
  AmdXdnaDpmLevel entry;
  if (!layout_.dpm.try_get(level, &entry)) return AmdXdnaError::BadArgument;
  if (layout_.dpm_by_level) {
    const AmdXdnaError e = smu_set_dpm(level);
    if (e != AmdXdnaError::None) return e;
    npuclk_mhz_ = entry.npuclk_mhz;
    hclk_mhz_ = entry.hclk_mhz;
  } else {
    uint32_t npuclk = entry.npuclk_mhz, hclk = entry.hclk_mhz;
    const AmdXdnaError e = smu_set_clocks(&npuclk, &hclk);
    if (e != AmdXdnaError::None) return e;
    npuclk_mhz_ = npuclk;
    hclk_mhz_ = hclk;
  }
  dpm_level_ = level;
  return AmdXdnaError::None;
}

AmdXdnaError AmdXdnaHw::copy_image(allocators::IAllocator& allocator, ConstByteView image, uint32_t align, ArrayView<uint8_t>* buffer, uint32_t* size,
                                   uint64_t* bus) {
  // psp_alloc_fw_buf(): the buffer is the image rounded up to the alignment, at an aligned address. The PSP gets the rounded size in 24 bits.
  if (image.empty() || image.size() > kPspSizeLimit - align) return AmdXdnaError::BadFirmware;
  const uint64_t rounded = (image.size() + align - 1) & ~uint64_t{align - 1};
  if (rounded >= kPspSizeLimit) return AmdXdnaError::BadFirmware;
  ArrayView<uint8_t> memory = allocate_dma_view<uint8_t>(allocator, rounded, align);
  if (memory.empty()) return AmdXdnaError::NoMemory;
  memory.copy_from(image);
  memory.subview(image.size()).fill(0);
  *buffer = memory;
  *size = static_cast<uint32_t>(rounded);
  *bus = bus_address_of(memory.data());
  return AmdXdnaError::None;
}

AmdXdnaError AmdXdnaHw::load_firmware(allocators::IAllocator& allocator, ConstByteView firmware, ConstByteView cert_firmware) {
  if (!layout_.valid() || !layout_.psp[psp_index(AmdXdnaPspReg::Cmd)].present()) return AmdXdnaError::BadState;  // a VF loads no firmware
  if (layout_.cert_firmware && cert_firmware.empty()) return AmdXdnaError::BadFirmware;
  release_buffers();
  allocator_ = &allocator;
  AmdXdnaError e = copy_image(allocator, firmware, kFirmwareAlign, &fw_buffer_, &fw_size_, &fw_bus_);
  if (e == AmdXdnaError::None && layout_.cert_firmware) e = copy_image(allocator, cert_firmware, kCertAlign, &cert_buffer_, &cert_size_, &cert_bus_);
  if (e != AmdXdnaError::None) release_buffers();
  return e;
}

AmdXdnaError AmdXdnaHw::psp_exec(AmdXdnaPspCommand command, uint32_t arg0, uint32_t arg1, uint32_t arg2) {
  const AmdXdnaRegister status = layout_.psp[psp_index(AmdXdnaPspReg::Status)], intr = layout_.psp[psp_index(AmdXdnaPspReg::Intr)];
  const AmdXdnaRegister resp = layout_.psp[psp_index(AmdXdnaPspReg::Resp)];
  if (!status.present()) return AmdXdnaError::Registers;
  if (arg2 >= kPspSizeLimit) return AmdXdnaError::BadArgument;  // it would run into the command byte
  uint32_t value = 0;
  if (!bars_.read32(status, &value)) return AmdXdnaError::Registers;
  if (!poll(status, kPspStatusReady, kPspStatusReady)) return AmdXdnaError::Timeout;
  const uint32_t cmd = static_cast<uint32_t>(command);
  const uint32_t in[4] = {cmd, arg0, arg1, (arg2 | (cmd << 24)) & layout_.psp_arg2_mask};
  for (uint8_t i = 0; i < 4; ++i)
    if (!bars_.write32(layout_.psp[i], in[i])) return AmdXdnaError::Registers;
  if (!bars_.write32(intr, 0) || !bars_.write32(intr, layout_.psp_notify)) return AmdXdnaError::Registers;
  if (!poll(status, kPspStatusReady, kPspStatusReady)) return AmdXdnaError::Timeout;
  if (!bars_.read32(resp, &value)) return AmdXdnaError::Registers;
  last_psp_response_ = value;
  return value == 0 ? AmdXdnaError::None : AmdXdnaError::PspFailed;
}

AmdXdnaError AmdXdnaHw::psp_start() {
  if (fw_buffer_.empty()) return AmdXdnaError::BadState;
  AmdXdnaError e = psp_exec(AmdXdnaPspCommand::Validate, static_cast<uint32_t>(fw_bus_), static_cast<uint32_t>(fw_bus_ >> 32), fw_size_);
  if (e != AmdXdnaError::None) return e;
  if (!cert_buffer_.empty()) {
    e = psp_exec(AmdXdnaPspCommand::ValidateCert, static_cast<uint32_t>(cert_bus_), static_cast<uint32_t>(cert_bus_ >> 32), cert_size_);
    if (e != AmdXdnaError::None) return e;
  }
  return psp_exec(AmdXdnaPspCommand::Start, kPspStartCopyFirmware, 0, 0);
}

AmdXdnaError AmdXdnaHw::psp_stop() { return psp_exec(AmdXdnaPspCommand::ReleaseTmr, 0, 0, 0); }

AmdXdnaError AmdXdnaHw::psp_wait_mode() {
  const AmdXdnaRegister wait = layout_.psp[psp_index(AmdXdnaPspReg::WaitMode)];
  if (!wait.present()) return AmdXdnaError::None;
  return poll(wait, kWaitModeIdle, kWaitModeIdle) ? AmdXdnaError::None : AmdXdnaError::Timeout;
}

}  // namespace hardware
