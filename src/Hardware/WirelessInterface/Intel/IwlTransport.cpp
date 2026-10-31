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


#include "Hardware/WirelessInterface/Intel/IwlTransport.hpp"
#include "Kernel/Delay.hpp"
#include "Utility/ByteStream.hpp"

namespace hardware {

using namespace iwl_csr;

void IwlTransport::configure_nic(uint32_t phy_config) {
  // iwl_mvm_nic_config: the MAC's step and dash from the hardware revision, the radio's type, step and dash from the firmware's PHY SKU
  const uint32_t type = phy_config & 0x3, step = (phy_config >> 2) & 0x3, dash = (phy_config >> 4) & 0x3;
  uint32_t value = device_.hw_rev() & kHwIfMacStepDashMask;
  value |= type << kHwIfPhyTypePos;
  value |= step << kHwIfPhyStepPos;
  value |= dash << kHwIfPhyDashPos;
  const uint32_t mask = kHwIfMacStepDashMask | kHwIfPhyTypeMask | kHwIfPhyStepMask | kHwIfPhyDashMask | kHwIfRadioSi | kHwIfMacSi;
  device_.write32(kHwIfConfig, (device_.read32(kHwIfConfig) & ~mask) | (value & mask));
}

bool IwlTransport::start(const IwlFirmware& firmware, IwlImage image, uint32_t timeout_us, const char** error) {
  const char* local = nullptr;
  if (!error) error = &local;
  stop();
  alive_ = IwlAliveInfo();
  failed_ = false;
  failure_text_[0] = 0;
  error_log_ = IwlErrorLog();
  if (!device_.present()) {
    *error = "the card does not answer";
    return false;
  }
  if (!device_.prepare_card_hw()) {
    *error = "the hardware is not ready (another owner?)";
    return false;
  }
  device_.disable_interrupts();
  if (device_.hw_rfkill()) {
    *error = "the radio is switched off";
    return false;
  }
  device_.write32(kUcodeDrvGp1Clr, kUcodeSwBitRfKill);
  device_.write32(kUcodeDrvGp1Clr, kUcodeBitCmdBlocked);
  device_.write32(kInt, 0xFFFFFFFFu);
  // iwl_pcie_nic_init
  if (!device_.apm_init()) {
    *error = "the adapter would not start";
    return false;
  }
  configure_nic(firmware.phy_config());
  if (!rx_.init(integrated_, error) || !tx_.init(error)) {
    stop();
    return false;
  }
  device_.set_bit(kMacShadowRegCtrl, kMacShadowRegCtrlEnable);
  device_.write32(kUcodeDrvGp1Clr, kUcodeSwBitRfKill);
  if (!load_.load(firmware, image, error)) {
    stop();
    return false;
  }
  if (!load_.wait_alive(timeout_us, error)) {
    stop();
    return false;
  }
  // the notification arrives as a frame
  uint32_t waited = 0;
  while (!alive_.valid) {
    poll(nullptr);
    if (alive_.valid) break;
    if (waited >= timeout_us) {
      *error = "the firmware did not send its ALIVE notification";
      stop();
      return false;
    }
    delay_us(100);
    waited += 100;
  }
  if (alive_.status != iwl_cmd::kAliveStatusOk) {
    *error = "the firmware reported a failed start";
    stop();
    return false;
  }
  if (!tx_.start(error)) {
    stop();
    return false;
  }
  running_ = true;
  return true;
}

void IwlTransport::stop() {
  running_ = false;
  interrupts_on_ = false;
  waiting_ = false;
  rx_.release();
  tx_.release();
  if (device_.present()) {
    device_.disable_interrupts();
    device_.apm_stop();
  }
}

void IwlTransport::use_interrupts(bool on) {
  interrupts_on_ = on && running_;
  __atomic_store_n(&irq_pending_, 0u, __ATOMIC_RELEASE);
  device_.write32(kIntMask, interrupts_on_ ? kIniSetMask : 0);
}

// Interrupt context: the registers only. The card is masked until the thread has looked at what it said; the cause bits stay set for it.
void IwlTransport::on_interrupt() {
  if (!interrupts_on_) return;
  const uint32_t cause = device_.read32(kInt);
  if (cause == 0 || cause == 0xFFFFFFFFu) return;                        // not ours, or the card is gone
  device_.write32(kIntMask, 0);
  __atomic_store_n(&irq_pending_, 1u, __ATOMIC_RELEASE);
  __atomic_fetch_add(&irq_count_, 1u, __ATOMIC_ACQ_REL);
}

void IwlTransport::service_interrupt() {
  __atomic_store_n(&irq_pending_, 0u, __ATOMIC_RELEASE);
  const uint32_t cause = device_.read32(kInt), fh = device_.read32(kFhIntStatus);
  if (cause != 0xFFFFFFFFu && (cause & (kIntBitSwError | kIntBitHwError))) capture_failure();   // (before the bits are cleared: check_firmware() looks at them too)
  device_.write32(kInt, cause);                                           // acknowledged
  device_.write32(kFhIntStatus, fh);
  if (interrupts_on_ && !failed_) device_.write32(kIntMask, kIniSetMask);  // and the card may speak again
}

void IwlTransport::capture_failure() {
  if (failed_) return;
  failed_ = true;
  if (!iwl_read_error_log(device_, alive_.error_event_table, &error_log_)) error_log_ = IwlErrorLog();
  iwl_format_error_log(error_log_, failure_text_, sizeof failure_text_);
}

bool IwlTransport::check_firmware() {
  if (failed_) return true;
  if (running_ && (device_.read32(kInt) & (kIntBitSwError | kIntBitHwError))) capture_failure();
  return failed_;
}

void IwlTransport::on_packet(const IwlRxPacket& packet) {
  if (packet.group == 0 && packet.command == iwl_cmd::kReplyError && !packet.is_response()) {
    ++command_errors_;
    return;
  }
  if (packet.group == 0 && packet.command == iwl_cmd::kAliveNotification && !packet.is_response()) {
    ByteReader r(packet.payload);
    alive_.status = r.le16();
    alive_.flags = r.le16();
    alive_.ucode_major = r.le32();
    alive_.ucode_minor = r.le32();
    r.skip(8);                                           // subtype, type, mac, opt, timestamp
    alive_.error_event_table = r.le32();                 // dbg_ptrs.error_event_table_ptr
    r.skip(16);                                          // log table, cpu register, dbgm config, alive counter
    alive_.scd_base = r.le32();
    alive_.valid = r.ok();
    return;
  }
  if (packet.is_response()) {
    if (packet.queue() == iwl_tx::kCommandQueue) tx_.reclaim(packet.index());
    if (waiting_ && !answered_ && ((packet.queue() << 8) | packet.index()) == waiting_sequence_) {
      const uint64_t n = packet.payload.size() < response_.size() ? packet.payload.size() : response_.size();
      if (n) response_.subview(0, n).copy_from(packet.payload.subview(0, n));
      response_length_ = static_cast<uint32_t>(packet.payload.size());
      answered_ = true;
    }
    return;
  }
  if (handler_) handler_->on_notification(packet);
}

uint32_t IwlTransport::poll(IIwlNotificationHandler* handler) {
  if (interrupts_on_ && interrupt_pending()) service_interrupt();
  IIwlNotificationHandler* saved = handler_;
  handler_ = handler ? handler : default_handler_;
  const uint32_t frames = rx_.poll(*this);
  frames_ += frames;
  handler_ = saved;
  if (running_) check_firmware();
  return frames;
}

int32_t IwlTransport::send_sync(const IwlHostCommand& command, ByteView response, uint32_t timeout_us, const char** error) {
  const char* local = nullptr;
  if (!error) error = &local;
  if (failed_) {
    *error = failure_text_[0] ? failure_text_ : "the firmware has failed";
    return -1;
  }
  if (!running_ || waiting_) {
    *error = running_ ? "a command is already being waited for" : "the firmware is not running";
    return -1;
  }
  const int32_t sequence = tx_.enqueue(command, error);
  if (sequence < 0) return -1;
  waiting_ = true;
  answered_ = false;
  waiting_sequence_ = static_cast<uint16_t>(sequence);
  response_ = response;
  response_length_ = 0;
  uint32_t waited = 0;
  while (!answered_) {
    poll(handler_);
    if (answered_) break;
    if (check_firmware()) {
      *error = failure_text_[0] ? failure_text_ : "the firmware failed while a command was pending";
      waiting_ = false;
      return -1;
    }
    if (waited >= timeout_us) {
      *error = "the firmware did not answer the command";
      waiting_ = false;
      if (device_.present()) capture_failure();           // (a silent firmware may have stopped without the interrupt: its log says)
      return -1;
    }
    delay_us(50);
    waited += 50;
  }
  waiting_ = false;
  return static_cast<int32_t>(response_length_);
}

}  // namespace hardware
