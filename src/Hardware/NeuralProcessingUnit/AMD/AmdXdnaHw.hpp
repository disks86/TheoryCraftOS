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
#include "Allocators/IAllocator.hpp"
#include "Hardware/Clock/IClock.hpp"
#include "Hardware/NeuralProcessingUnit/Intel/IntelNpuHw.hpp"
#include "Utility/ArrayView.hpp"

namespace hardware {

// AMD's NPUs ("XDNA", PCI vendor 0x1022) as the Linux amdxdna driver (drivers/accel/amdxdna) knows them. The driver keys every part on the PCI device id AND
// the PCI revision (amdxdna_pci_drv.c: amdxdna_ids), so this table does too. The driver's own names are "npu1".."npu6"; the generations are the AI Engine
// ("AIE") array the part carries: AIE2 for XDNA (npu1) and XDNA 2 (npu4/5/6), AIE4 for npu3 (aie4_pci.c).
enum class AmdXdnaGeneration : uint8_t { Unknown = 0, Xdna = 1, Xdna2 = 2, Aie4 = 3 };
enum class AmdXdnaModel : uint8_t { Unknown, Npu1, Npu3Pf, Npu3Vf, Npu4, Npu5, Npu6 };

struct AmdXdnaIdentity {
  AmdXdnaModel model = AmdXdnaModel::Unknown;
  AmdXdnaGeneration generation = AmdXdnaGeneration::Unknown;
  const char* npu = nullptr;           // the driver's name: "npu1", "npu4", ...
  const char* vbnv = nullptr;          // the driver's default_vbnv ("RyzenAI-npu1", ...)
  // Where the parts ship. The driver's regs files name no product for a PCI id: npu1_regs.c names none; npu4_regs.c's revision table (shared by npu4, 5 and 6)
  // names what the FIRMWARE reports (amd_xdna_revision_name). These strings are the commonly used platform names for the PCI id/revision, not driver text.
  const char* platform = nullptr;
  const char* firmware_dir = nullptr;  // the driver's fw_path ("amdnpu/1502_00/"); nullptr for a virtual function, which loads none
  bool virtual_function = false;       // an SR-IOV VF: no SMU, no PSP, no firmware, only a mailbox (npu3 VF)
  constexpr bool valid() const { return model != AmdXdnaModel::Unknown; }
};

constexpr AmdXdnaIdentity amd_xdna_identify(uint16_t device_id, uint8_t revision) {
  switch (device_id) {
    case 0x1502:
      if (revision == 0x00) return {AmdXdnaModel::Npu1, AmdXdnaGeneration::Xdna, "npu1", "RyzenAI-npu1", "Phoenix / Hawk Point (Ryzen 7040/8040)", "amdnpu/1502_00/", false};
      return {};
    case 0x17F0:
      if (revision == 0x10) return {AmdXdnaModel::Npu4, AmdXdnaGeneration::Xdna2, "npu4", "RyzenAI-npu4", "Strix Point (Ryzen AI 300)", "amdnpu/17f0_10/", false};
      if (revision == 0x11) return {AmdXdnaModel::Npu5, AmdXdnaGeneration::Xdna2, "npu5", "RyzenAI-npu5", "Krackan Point (Ryzen AI 300)", "amdnpu/17f0_11/", false};
      // npu6_regs.c gives npu6 the npu4 directory ("amdnpu/17f0_10/"), not a 17f0_20 one: recorded as the driver has it.
      if (revision == 0x20) return {AmdXdnaModel::Npu6, AmdXdnaGeneration::Xdna2, "npu6", "RyzenAI-npu6", "Strix Halo (Ryzen AI Max)", "amdnpu/17f0_10/", false};
      return {};
    // AIE4 parts: the driver builds the directory as "amdnpu/%04x_%02x/" from the device's own id and revision (aie4_request_firmware).
    case 0x17F2:
      if (revision == 0x10) return {AmdXdnaModel::Npu3Pf, AmdXdnaGeneration::Aie4, "npu3", "RyzenAI-npu3-pf", "AIE4 part (physical function)", "amdnpu/17f2_10/", false};
      return {};
    case 0x1B0B:
      if (revision == 0x10) return {AmdXdnaModel::Npu3Pf, AmdXdnaGeneration::Aie4, "npu3", "RyzenAI-npu3-pf", "AIE4 part (physical function)", "amdnpu/1b0b_10/", false};
      return {};
    case 0x17F3:
      if (revision == 0x10) return {AmdXdnaModel::Npu3Vf, AmdXdnaGeneration::Aie4, "npu3", "RyzenAI-npu3-vf", "AIE4 part (virtual function)", nullptr, true};
      return {};
    case 0x1B0C:
      if (revision == 0x10) return {AmdXdnaModel::Npu3Vf, AmdXdnaGeneration::Aie4, "npu3", "RyzenAI-npu3-vf", "AIE4 part (virtual function)", nullptr, true};
      return {};
    default: return {};
  }
}

// The generation from the device id alone (any revision), for code that has no revision at hand. Unknown for an id the driver does not bind.
constexpr AmdXdnaGeneration amd_xdna_generation(uint16_t device_id) {
  switch (device_id) {
    case 0x1502: return AmdXdnaGeneration::Xdna;
    case 0x17F0: return AmdXdnaGeneration::Xdna2;
    case 0x17F2: case 0x17F3: case 0x1B0B: case 0x1B0C: return AmdXdnaGeneration::Aie4;
    default: return AmdXdnaGeneration::Unknown;
  }
}

// What the firmware's GET_DEV_REVISION message names (aie2_msg_priv.h: aie2_dev_revision, npu4_regs.c: npu4_rev_vbnv_tbl, exact strings); nullptr otherwise.
constexpr const char* amd_xdna_revision_name(uint32_t firmware_revision) {
  switch (firmware_revision) {
    case 1: case 2: return "NPU Strix";
    case 3: return "NPU Krackan 1";
    case 4: return "NPU Krackan 2";
    case 5: return "NPU Strix Halo";
    case 6: return "NPU Gorgon Point 1";
    case 7: return "NPU Gorgon Point 2";
    case 8: return "NPU Gorgon Point 3";
    case 9: return "NPU Gorgon Halo";
    default: return nullptr;
  }
}

// A register: which BAR and the byte offset inside it (the driver's DEFINE_BAR_OFFSET: the aperture address minus the BAR's base).
struct AmdXdnaRegister {
  static constexpr uint8_t kNoBar = 0xFF;
  uint8_t bar = kNoBar;
  uint32_t offset = 0;
  constexpr bool present() const { return bar != kNoBar; }
};

// The PSP (platform security processor) registers, in the driver's order (aie.h: psp_reg_idx). The first four are the command and its arguments.
enum class AmdXdnaPspReg : uint8_t { Cmd, Arg0, Arg1, Arg2, Intr, Status, Resp, WaitMode, Count };
// The SMU (system management unit) registers (aie.h: smu_reg_idx).
enum class AmdXdnaSmuReg : uint8_t { Cmd, Arg, Intr, Resp, Out, Count };

// SMU message ids (aie_smu.c).
enum class AmdXdnaSmuCommand : uint32_t { PowerOn = 0x3, PowerOff = 0x4, SetMpNpuClock = 0x5, SetHClock = 0x6, SetSoftDpmLevel = 0x7, SetHardDpmLevel = 0x8 };
// PSP command ids (aie_psp.c).
enum class AmdXdnaPspCommand : uint32_t { Validate = 1, Start = 2, ReleaseTmr = 3, ValidateCert = 4 };

// A DPM (clock) level: MP-NPU clock and H clock in MHz (npu1_regs.c / npu4_regs.c dpm_clk_table).
struct AmdXdnaDpmLevel {
  uint32_t npuclk_mhz = 0, hclk_mhz = 0;
};

// Firmware features the driver keys on the management protocol version (aie2_pci.h: aie2_fw_feature).
enum class AmdXdnaFeature : uint8_t { NpuCommand, Preempt, TemporalOnly, AppHealth, AddHostBuffer, UpdateProperty, GetDevRevision, Count };
constexpr uint64_t amd_xdna_feature_bit(AmdXdnaFeature f) { return uint64_t{1} << static_cast<uint8_t>(f); }

// One row of the protocol table (amdxdna_fw_feature_tbl): a firmware whose protocol major equals `major` and minor lies in [min_minor, max_minor] (0: no
// upper bound) is supported and has `features`.
struct AmdXdnaProtocolEntry {
  uint32_t major = 0, min_minor = 0, max_minor = 0;
  uint64_t features = 0;
};

// Runtime configuration the driver sends at bring-up (aie2_pci.h: rt_config, npu*_default_rt_cfg).
enum class AmdXdnaRtCategory : uint8_t { Init, ClockGating, ForcePreempt, FrameBoundaryPreempt };
struct AmdXdnaRuntimeConfig {
  uint32_t type = 0, value = 0;
  AmdXdnaRtCategory category = AmdXdnaRtCategory::Init;
  uint64_t skip_if_features = 0;  // the driver skips the row when the firmware HAS all of these (aie2_runtime_cfg: bitmap_subset -> continue)
};

// Everything about one model's registers and bring-up, from npu1_regs.c, npu3_regs.c, npu4_regs.c, npu5_regs.c and npu6_regs.c.
struct AmdXdnaLayout {
  AmdXdnaModel model = AmdXdnaModel::Unknown;
  uint8_t reg_bar = AmdXdnaRegister::kNoBar, mbox_bar = AmdXdnaRegister::kNoBar, sram_bar = AmdXdnaRegister::kNoBar;
  uint8_t psp_bar = AmdXdnaRegister::kNoBar, smu_bar = AmdXdnaRegister::kNoBar;
  // AIE2: the device addresses the BARs start at. The firmware reports mailbox registers and ring buffers as device addresses; minus these they are BAR offsets.
  uint32_t mbox_dev_addr = 0, sram_dev_addr = 0;
  // AIE2: offsets in the SRAM BAR of the first management mailbox and of the word the firmware writes the channel description's address to once it runs.
  uint32_t mbox_channel_offset = 0, fw_alive_offset = 0;
  // AIE4: offset in the ring-buffer BAR (sram_bar) of the mailbox_info block the firmware fills in.
  uint32_t mbox_info_offset = 0;
  AmdXdnaRegister psp[static_cast<uint8_t>(AmdXdnaPspReg::Count)];
  AmdXdnaRegister smu[static_cast<uint8_t>(AmdXdnaSmuReg::Count)];
  uint32_t psp_arg2_mask = 0;    // AIE2 GENMASK(23, 0), AIE4 ~0
  uint32_t psp_notify = 0;       // the value written to the PSP interrupt register: 1, or 0xD007BE11 on AIE4
  const char* firmware_names[2] = {nullptr, nullptr};  // tried in order inside firmware_dir (AIE2: "npu_7.sbin", "npu.sbin"; AIE4: "npu.dev.sbin")
  const char* cert_firmware = nullptr;                 // AIE4: "cert.dev.sbin", validated with PSP_VALIDATE_CERT
  uint32_t first_col = 0;        // the first AIE column applications get (npu1 reserves column 0)
  uint32_t col_opc = 0;          // operations per cycle per column
  uint32_t hwctx_limit = 0;
  bool dpm_by_level = false;     // npu4..6 set clocks through SMU DPM levels; npu1 sets the two frequencies
  ArrayView<const AmdXdnaDpmLevel> dpm;
  ArrayView<const AmdXdnaProtocolEntry> protocol;
  ArrayView<const AmdXdnaRuntimeConfig> runtime_config;
  // How much of each BAR the driver touches, for mapping: [bar] bytes, 0 = not mapped. The PSP/SMU/SRAM windows cover the fixed registers; the mailbox
  // windows are a bound for the firmware-reported registers (the driver uses the whole BAR, whose size the PCI layer here does not report).
  uint64_t bar_window[6] = {};
  constexpr bool valid() const { return model != AmdXdnaModel::Unknown; }
};
// The layout of a model; an invalid one (model Unknown) for Unknown.
const AmdXdnaLayout& amd_xdna_layout(AmdXdnaModel model);

// The device's BARs as register windows. A missing BAR has no registers; every access is checked against `size` before it is made.
struct AmdXdnaBar {
  INpuRegisters* regs = nullptr;
  uint64_t size = 0;
};
struct AmdXdnaBars {
  AmdXdnaBar bar[6];
  // False (and *value untouched / nothing written) when the BAR is missing or [offset, offset + 4) is not inside it or not 4-byte aligned.
  bool read32(uint8_t index, uint64_t offset, uint32_t* value) const;
  bool write32(uint8_t index, uint64_t offset, uint32_t value) const;
  bool read32(AmdXdnaRegister r, uint32_t* value) const { return read32(r.bar, r.offset, value); }
  bool write32(AmdXdnaRegister r, uint32_t value) const { return write32(r.bar, r.offset, value); }
  bool contains(uint8_t index, uint64_t offset, uint64_t length) const;
};

enum class AmdXdnaError : uint8_t {
  None,
  NoDevice,        // not a known model, or the registers read all-ones
  BadState,        // called in the wrong order (start twice, stop before start, ...)
  Registers,       // a register the model needs is not mapped
  Timeout,         // the SMU, PSP or firmware did not answer in time
  SmuFailed,       // the SMU answered with an error code
  PspFailed,       // the PSP answered with an error code
  BadFirmware,     // empty, too large for the PSP's size field, or missing the certificate image an AIE4 part needs
  NoMemory,
  NotAlive,        // the firmware never published its management channel
  BadChannel,      // the published channel description is malformed (magic, geometry outside the BARs)
  Protocol,        // the firmware's management protocol version is not one the table supports
  ChannelBroken,   // the device wrote something impossible into a ring; the channel is shut
  NoSlot,          // every message slot is waiting for an answer
  BadMessage,      // the request is too large, misaligned or starts with the tombstone word
  BadResponse,     // the answer has the wrong size or an unknown id
  DeviceStatus,    // the firmware answered with a non-zero status
  Unsupported,     // the firmware does not have the feature the message needs
  BadArgument,
};
const char* amd_xdna_error_name(AmdXdnaError error);

// Polling with a deadline: the clock when there is one, else a number of reads (about a microsecond each over PCI Express). Never spins forever.
class AmdXdnaDeadline {
 public:
  static constexpr uint64_t kMaxPolls = uint64_t{1} << 26;
  AmdXdnaDeadline(const IClock* clock, uint64_t timeout_us)
      : clock_(clock), start_(clock ? clock->now_us() : 0), timeout_us_(timeout_us) {}
  // True once the time is up; call once per poll.
  bool expired() {
    ++polls_;
    if (polls_ > kMaxPolls) return true;
    if (clock_) return clock_->now_us() - start_ > timeout_us_;
    return polls_ > timeout_us_;
  }

 private:
  const IClock* clock_;
  uint64_t start_;
  uint64_t timeout_us_;
  uint64_t polls_ = 0;
};

// What a read-only look at the registers found. Nothing is written.
struct AmdXdnaProbe {
  bool valid = false;            // the registers answered (not all-ones)
  bool psp_ready = false;        // PSP_STATUS bit 31: the PSP accepts a command
  uint32_t psp_status = 0;
  uint32_t smu_response = 0;     // the SMU's last response (1 = OK, 0 = none yet)
  bool firmware_alive = false;   // AIE2: the alive word in SRAM holds a channel address; AIE4: mailbox_info.valid == 1
  uint32_t fw_alive_value = 0;
  bool wait_mode = false;        // AIE2: PWAITMODE bit 0, the firmware is idle/suspended
};

// The host's side of the SMU and PSP (aie_smu.c, aie_psp.c) and the hardware parts of aie2_pci.c / aie4_pci.c: identity, a read-only probe, power and
// clocks through the SMU, firmware validation and start through the PSP, and finding the management mailbox the firmware publishes. Not thread safe: the
// owner serializes (the driver holds dev_lock).
class AmdXdnaHw {
 public:
  static constexpr uint64_t kTimeoutUs = 1000000;     // AIE_TIMEOUT / PSP_POLL_TIMEOUT: one second
  static constexpr uint32_t kSmuResultOk = 1;
  static constexpr uint32_t kPspStatusReady = 1u << 31;
  static constexpr uint32_t kFirmwareAlign = 0x10000; // PSP_FW_ALIGN: the image's buffer is 64 KiB aligned and its size rounded up to 64 KiB
  static constexpr uint32_t kCertAlign = 0x8000;      // PSP_CFW_ALIGN
  static constexpr uint32_t kPspSizeLimit = 1u << 24; // the size travels in the low 24 bits of ARG2 (the command sits in the top 8)
  static constexpr uint32_t kMgmtMagic = 0x55504E5F;  // "_NPU": the management channel description
  static constexpr uint32_t kMgmtInfoWords = 16;      // mgmt_mbox_chann_info
  static constexpr uint32_t kAie4InfoWords = 16;      // aie4 mailbox_info

  AmdXdnaHw(const AmdXdnaLayout& layout, const AmdXdnaBars& bars, const IClock* clock) : layout_(layout), bars_(bars), clock_(clock) {}
  ~AmdXdnaHw();
  AmdXdnaHw(const AmdXdnaHw&) = delete;
  AmdXdnaHw& operator=(const AmdXdnaHw&) = delete;

  const AmdXdnaLayout& layout() const { return layout_; }
  const AmdXdnaBars& bars() const { return bars_; }
  const IClock* clock() const { return clock_; }

  // Reads only.
  AmdXdnaProbe probe() const;

  // aie_smu_exec(): clear RESP, write ARG and CMD, pulse INTR, wait for a response. `out` (may be null) gets the OUT register. The SMU's answer is kept in
  // last_smu_response().
  AmdXdnaError smu_exec(AmdXdnaSmuCommand command, uint32_t argument, uint32_t* out);
  // aie_smu_init(): power off (an unrecoverable fault if even that fails), then on.
  AmdXdnaError smu_init();
  // aie_smu_fini(): power off.
  AmdXdnaError smu_power_off();
  // aie_smu_set_clocks(): either may be null. On success each holds the frequency the SMU actually set.
  AmdXdnaError smu_set_clocks(uint32_t* npuclk_mhz, uint32_t* hclk_mhz);
  // aie_smu_set_dpm(): hard level, then soft level.
  AmdXdnaError smu_set_dpm(uint32_t level);
  // The model's set_dpm hook (npu1_set_dpm / npu4_set_dpm): `level` indexes the DPM table. Remembers the clocks.
  AmdXdnaError set_dpm_level(uint32_t level);
  uint32_t max_dpm_level() const { return layout_.dpm.empty() ? 0 : static_cast<uint32_t>(layout_.dpm.size() - 1); }
  uint32_t dpm_level() const { return dpm_level_; }
  uint32_t npuclk_mhz() const { return npuclk_mhz_; }
  uint32_t hclk_mhz() const { return hclk_mhz_; }
  uint32_t last_smu_response() const { return last_smu_response_; }

  // Copies the firmware (and, for AIE4, the certificate image) into DMA memory the PSP will read: aligned and padded with zeros as aie_psp.c does. Checks
  // the sizes. The memory stays until the destructor (the PSP is given it again on every start).
  AmdXdnaError load_firmware(allocators::IAllocator& allocator, ConstByteView firmware, ConstByteView cert_firmware);
  uint32_t firmware_buffer_size() const { return fw_size_; }
  uint64_t firmware_bus_address() const { return fw_bus_; }

  // psp_exec(): wait until the PSP is ready, write the command and arguments, pulse the interrupt register, wait until ready again, read the response.
  AmdXdnaError psp_exec(AmdXdnaPspCommand command, uint32_t arg0, uint32_t arg1, uint32_t arg2);
  // aie_psp_start(): validate the image (and the certificate), then start it.
  AmdXdnaError psp_start();
  // aie_psp_stop(): release the trusted memory region.
  AmdXdnaError psp_stop();
  // aie_psp_waitmode_poll(): wait for PWAITMODE bit 0 (after a suspend message). Nothing to wait for on a model without the register.
  AmdXdnaError psp_wait_mode();
  uint32_t last_psp_response() const { return last_psp_response_; }

 private:
  bool poll(AmdXdnaRegister r, uint32_t mask, uint32_t expected) const;
  AmdXdnaError copy_image(allocators::IAllocator& allocator, ConstByteView image, uint32_t align, ArrayView<uint8_t>* buffer, uint32_t* size, uint64_t* bus);
  void release_buffers();

  const AmdXdnaLayout& layout_;
  AmdXdnaBars bars_;
  const IClock* clock_;
  allocators::IAllocator* allocator_ = nullptr;
  ArrayView<uint8_t> fw_buffer_, cert_buffer_;
  uint32_t fw_size_ = 0, cert_size_ = 0;
  uint64_t fw_bus_ = 0, cert_bus_ = 0;
  uint32_t dpm_level_ = 0, npuclk_mhz_ = 0, hclk_mhz_ = 0;
  uint32_t last_smu_response_ = 0, last_psp_response_ = 0;
};

}  // namespace hardware
