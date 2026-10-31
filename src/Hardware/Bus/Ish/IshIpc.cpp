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

#include "Hardware/Bus/Ish/IshIpc.hpp"
#include "Kernel/Delay.hpp"

namespace hardware {

namespace {
constexpr uint32_t kReadyPolls = 20000, kPollUs = 500;           // up to 10 s, as the kernel driver waits for the reset acknowledgement
constexpr uint32_t kInputPolls = 4000, kInputUs = 50;
uint32_t build_header(uint32_t length, uint32_t protocol, uint32_t command) { return (length & 0x3FF) | (protocol << 10) | (command << 16) | ish_reg::kDoorbellBusy; }
}  // namespace

void IshIpc::set_host_ready() {
  registers_.write32(ish_reg::kHostComm, registers_.read32(ish_reg::kHostComm) | ish_reg::kHostCommReady);
  uint32_t mask = registers_.read32(ish_reg::kPimr);
  mask |= ish_reg::kPimrInterruptEnable;
  mask &= ~ish_reg::kPimrHostToIshBusyClear;
  registers_.write32(ish_reg::kPimr, mask);
}

bool IshIpc::firmware_ready() {
  const uint32_t status = registers_.read32(ish_reg::kFwStatus);
  return (status & ish_reg::kFwIlup) && (status & ish_reg::kFwIshtpReady);
}

bool IshIpc::wait_input_ready() {
  for (uint32_t i = 0; i < kInputPolls; ++i) {
    if (!(registers_.read32(ish_reg::kHostToIshDoorbell) & ish_reg::kDoorbellBusy)) return true;
    delay_us(kInputUs);
  }
  return false;
}

// The message registers are written as 32 bit words (the last one padded), the doorbell last.
bool IshIpc::write_message(uint32_t doorbell, ConstByteView first, ConstByteView second) {
  const uint64_t total = first.size() + second.size();
  if (total > ish_reg::kMaxMessage || !wait_input_ready()) return false;
  uint32_t word = 0, filled = 0, at = 0;
  auto put = [&](ConstByteView bytes) {
    for (uint64_t i = 0; i < bytes.size(); ++i) {
      word |= static_cast<uint32_t>(bytes[i]) << (8 * filled);
      if (++filled == 4) {
        registers_.write32(ish_reg::kHostToIshMessage + at, word);
        at += 4;
        word = 0;
        filled = 0;
      }
    }
  };
  put(first);
  put(second);
  if (filled) registers_.write32(ish_reg::kHostToIshMessage + at, word);
  registers_.write32(ish_reg::kHostToIshDoorbell, doorbell);
  registers_.read32(ish_reg::kFwStatus);                          // (flushes the posted writes)
  return true;
}

bool IshIpc::send_ishtp(uint8_t fw_addr, uint8_t host_addr, bool complete, ConstByteView payload) {
  if (payload.size() > ish_reg::kMaxMessage - 4) return false;
  const uint32_t header = fw_addr | (uint32_t{host_addr} << 8) | (static_cast<uint32_t>(payload.size()) << 16) | (complete ? (1u << 31) : 0u);
  const uint8_t bytes[4] = {static_cast<uint8_t>(header), static_cast<uint8_t>(header >> 8), static_cast<uint8_t>(header >> 16), static_cast<uint8_t>(header >> 24)};
  return write_message(build_header(static_cast<uint32_t>(payload.size()) + 4, ish_reg::kProtocolIshtp, 0), ConstByteView(bytes, 4), payload);
}

bool IshIpc::send_management(uint32_t command, ConstByteView payload) {
  return write_message(build_header(static_cast<uint32_t>(payload.size()), ish_reg::kProtocolManagement, command), payload, ConstByteView());
}

bool IshIpc::receive(IshMessage* out) {
  for (;;) {
    const uint32_t doorbell = registers_.read32(ish_reg::kIshToHostDoorbell);
    if (!(doorbell & ish_reg::kDoorbellBusy)) return false;
    registers_.write32(ish_reg::kPisr, registers_.read32(ish_reg::kPisr));          // (acknowledges the interrupt that is not enabled in the CPU: write one to clear)
    IshMessage m;
    m.length = doorbell & 0x3FF;
    m.protocol = (doorbell >> 10) & 0xF;
    m.management_command = (doorbell >> 16) & 0xF;
    const bool fits = m.length <= ish_reg::kMaxMessage;
    if (fits) {
      for (uint32_t at = 0; at < m.length; at += 4) {
        const uint32_t word = registers_.read32(ish_reg::kIshToHostMessage + at);
        for (uint32_t b = 0; b < 4 && at + b < m.length; ++b) m.data[at + b] = static_cast<uint8_t>(word >> (8 * b));
      }
    }
    registers_.write32(ish_reg::kIshToHostDoorbell, 0);
    registers_.read32(ish_reg::kFwStatus);
    if (!fits) continue;                                                           // (too long to be real: dropped)
    if (m.protocol == ish_reg::kProtocolManagement) {
      if (m.management_command == ish_reg::kMngResetNotify) {
        // the firmware reset: the host has to be ready before the acknowledgement, which names the reset
        set_host_ready();
        const uint32_t id = m.length >= 2 ? (m.data[0] | (m.data[1] << 8)) : 0;
        const uint8_t payload[4] = {static_cast<uint8_t>(id), static_cast<uint8_t>(id >> 8), 0, 0};
        send_management(ish_reg::kMngResetNotifyAck, ConstByteView(payload, 4));
        reset_seen_ = true;
        reset_acked_ = true;
      } else if (m.management_command == ish_reg::kMngResetNotifyAck) {
        reset_acked_ = true;
      }
      continue;                                                                    // (the rest of the management traffic is not needed)
    }
    if (m.protocol == ish_reg::kProtocolIshtp && out) {
      *out = m;
      return true;
    }
  }
}

bool IshIpc::take_reset_seen() {
  const bool seen = reset_seen_;
  reset_seen_ = false;
  return seen;
}

bool IshIpc::start() {
  set_host_ready();
  registers_.write32(ish_reg::kRemap2, 1);                          // the DMA window of the firmware (the host never uses DMA transfers)
  // the IPC reset: ready again, the doorbell of the firmware cleared, a reset notify (reset id 1) and the acknowledgement
  set_host_ready();
  registers_.write32(ish_reg::kIshToHostDoorbell, 0);
  reset_acked_ = false;
  const uint8_t payload[8] = {1, 0, 0, 0, 0, 0, 0, 0};
  if (!send_management(ish_reg::kMngResetNotify, ConstByteView(payload, 8))) return false;
  for (uint32_t i = 0; i < kReadyPolls && !reset_acked_; ++i) {
    IshMessage m;
    if (receive(&m)) continue;                                      // (an ISHTP message this early: nobody to give it to)
    if (reset_acked_) break;
    delay_us(kPollUs);
  }
  if (!reset_acked_) return false;
  for (uint32_t i = 0; i < kReadyPolls / 10; ++i) {
    if (firmware_ready()) {
      reset_seen_ = false;                                          // (the reset that was ours)
      return true;
    }
    delay_us(kPollUs);
  }
  return false;
}

}  // namespace hardware
