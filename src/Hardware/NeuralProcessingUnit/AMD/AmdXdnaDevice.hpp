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
#include "Hardware/NeuralProcessingUnit/AMD/AmdXdnaHw.hpp"
#include "Hardware/NeuralProcessingUnit/AMD/AmdXdnaMailbox.hpp"
#include "Hardware/NeuralProcessingUnit/AMD/AmdXdnaMessages.hpp"

namespace hardware {

// The supported features of a firmware whose management protocol is major.minor, from the model's table (aie_check_protocol). False when no row matches.
bool amd_xdna_check_protocol(const AmdXdnaLayout& layout, uint32_t major, uint32_t minor, uint64_t* features);

// Where a start stopped.
enum class AmdXdnaStage : uint8_t { None, Firmware, Smu, Psp, Alive, Channel, Protocol, RuntimeConfig, Pasid, TimeQuota, Reset, Dpm, ClockGating, Query };
const char* amd_xdna_stage_name(AmdXdnaStage stage);

// An AMD NPU brought up the way aie2_hw_start() / aie4_pf_hw_start() do it: SMU power and clocks, the firmware validated and started by the PSP, the
// management mailbox the firmware publishes, the runtime configuration, the DPM level, and the queries that say what the device is. Polled: no interrupts.
// Not thread safe: the owner serializes (the driver holds dev_lock).
class AmdXdnaDevice {
 public:
  static constexpr uint32_t kDefaultTimeQuantumUs = 30000;  // DEFAULT_TIME_QUANTUM

  AmdXdnaDevice(const AmdXdnaLayout& layout, const AmdXdnaBars& bars, const IClock* clock);
  ~AmdXdnaDevice();
  AmdXdnaDevice(const AmdXdnaDevice&) = delete;
  AmdXdnaDevice& operator=(const AmdXdnaDevice&) = delete;

  // Copies the firmware image(s) into PSP-readable memory. AIE4 parts need the certificate image as well; a virtual function loads nothing.
  AmdXdnaError load_firmware(allocators::IAllocator& allocator, ConstByteView firmware, ConstByteView cert_firmware);
  // Brings the device up. On failure everything done so far is undone (powered off) and failed_stage() says where it stopped.
  AmdXdnaError start();
  // aie2_hw_stop(): clock gating back to the defaults, firmware suspended, mailbox closed, PSP released, lowest DPM level, power off. Every step is tried;
  // the first error is returned.
  AmdXdnaError stop();
  bool running() const { return running_; }
  AmdXdnaStage failed_stage() const { return failed_stage_; }
  // AIE2 (npu1, npu4..6): the management messages of AmdXdnaMessages apply. AIE4 parts speak another message set (not implemented): only the channel comes up.
  bool aie2() const;

  // aie2_runtime_cfg(): sends the model's runtime configuration rows of `category`, with `value` instead of each row's own when not null.
  AmdXdnaError runtime_config(AmdXdnaRtCategory category, const uint32_t* value);
  bool has_feature(AmdXdnaFeature feature) const { return (features_ & amd_xdna_feature_bit(feature)) != 0; }
  uint64_t features() const { return features_; }
  uint32_t protocol_major() const { return protocol_major_; }
  uint32_t protocol_minor() const { return protocol_minor_; }
  const AmdXdnaFirmwareVersion& firmware_version() const { return firmware_version_; }
  uint16_t aie_major() const { return aie_major_; }
  uint16_t aie_minor() const { return aie_minor_; }
  const AmdXdnaTileInfo& tile_info() const { return tile_info_; }
  uint32_t total_columns() const { return tile_info_.cols; }

  AmdXdnaHw& hw() { return hw_; }
  AmdXdnaMailbox& mailbox() { return mailbox_; }
  AmdXdnaMessages& messages() { return messages_; }

  // Reads the channel description the firmware published (AIE2: the address at the alive word, then mgmt_mbox_chann_info; AIE4: mailbox_info), checks it
  // and turns device addresses into BAR offsets. Waits up to a second for the firmware. AIE2 clears the alive word afterwards, as the driver does.
  AmdXdnaError read_channel(AmdXdnaChannelConfig* out);

 private:
  AmdXdnaError fail(AmdXdnaStage stage, AmdXdnaError error);
  AmdXdnaError start_aie2();
  AmdXdnaError start_aie4();
  void power_down(bool psp_started);
  AmdXdnaError read_channel_aie2(AmdXdnaChannelConfig* out);
  AmdXdnaError read_channel_aie4(AmdXdnaChannelConfig* out);

  const AmdXdnaLayout& layout_;
  AmdXdnaHw hw_;
  AmdXdnaMailbox mailbox_;
  AmdXdnaMessages messages_;
  bool running_ = false;
  AmdXdnaStage failed_stage_ = AmdXdnaStage::None;
  uint64_t features_ = 0;
  uint32_t protocol_major_ = 0, protocol_minor_ = 0;
  AmdXdnaFirmwareVersion firmware_version_;
  uint16_t aie_major_ = 0, aie_minor_ = 0;
  AmdXdnaTileInfo tile_info_;
};

}  // namespace hardware
