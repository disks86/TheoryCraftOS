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

#include "Hardware/Tpm/TpmCrb.hpp"
#include "Kernel/Delay.hpp"

namespace hardware {

using namespace crb;

namespace {
constexpr uint32_t kStatePolls = 20000, kResponsePolls = 600000, kPollUs = 100;
constexpr uint32_t kMaxResponse = 4096;
}  // namespace

bool TpmCrb::probe() {
  const uint32_t state = registers_.read32(kLocState);
  const uint32_t cmd_size = registers_.read32(kCmdSize);
  return state != 0xFFFFFFFFu && cmd_size != 0 && cmd_size != 0xFFFFFFFFu;
}

bool TpmCrb::wait(uint32_t reg, uint32_t mask, uint32_t value, uint32_t polls) {
  for (uint32_t i = 0; i < polls; ++i) {
    if ((registers_.read32(reg) & mask) == value) return true;
    delay_us(kPollUs);
  }
  return false;
}

bool TpmCrb::request_locality() {
  if ((registers_.read32(kLocSts) & 1) != 0) return true;               // already granted
  registers_.write32(kLocCtrl, 1);                                      // requestAccess
  return wait(kLocSts, 1, 1, kStatePolls);
}

bool TpmCrb::transmit(ConstByteView command, ByteBuffer* response) {
  if (command.size() < 10 || command.size() > kMaxResponse) return false;
  if (registers_.read32(kCtrlSts) & kStsFatal) return false;
  if (!request_locality()) return false;
  const uint32_t cmd_size = registers_.read32(kCmdSize), rsp_size = registers_.read32(kRspSize);
  const uint64_t cmd_pa = registers_.read32(kCmdPaLow) | (uint64_t{registers_.read32(kCmdPaHigh)} << 32), rsp_pa = registers_.read32(kRspPa) | (uint64_t{registers_.read32(kRspPa + 4)} << 32);
  // both buffers must be inside the page the registers cover
  if (cmd_pa < base_ || rsp_pa < base_ || cmd_pa - base_ > 0x1000 || rsp_pa - base_ > 0x1000) return false;
  const uint32_t cmd_at = static_cast<uint32_t>(cmd_pa - base_), rsp_at = static_cast<uint32_t>(rsp_pa - base_);
  if (command.size() > cmd_size || cmd_size > 0x1000 - cmd_at || rsp_size < 10 || rsp_size > 0x1000 - rsp_at) return false;
  // ready: the TPM leaves idle (cmdReady is cleared by the TPM when it has done it)
  registers_.write32(kCtrlReq, kReqCmdReady);
  if (!wait(kCtrlReq, kReqCmdReady, 0, kStatePolls)) return false;
  for (uint32_t i = 0; i < command.size(); ++i) registers_.write8(cmd_at + i, command[i]);
  registers_.write32(kCtrlStart, 1);
  const bool done = wait(kCtrlStart, 1, 0, kResponsePolls);
  bool ok = done && !(registers_.read32(kCtrlSts) & kStsFatal);
  response->clear();
  if (ok) {
    uint8_t header[10];
    for (uint32_t i = 0; i < 10; ++i) header[i] = registers_.read8(rsp_at + i);
    const uint32_t length = (uint32_t{header[2]} << 24) | (uint32_t{header[3]} << 16) | (uint32_t{header[4]} << 8) | header[5];
    ok = length >= 10 && length <= rsp_size && response->append(ConstByteView(header, 10));
    for (uint32_t i = 10; ok && i < length; ++i) ok = response->put8(registers_.read8(rsp_at + i));
  }
  if (!done) {
    registers_.write32(kCtrlCancel, 1);
    wait(kCtrlStart, 1, 0, kStatePolls);
    registers_.write32(kCtrlCancel, 0);
  }
  registers_.write32(kCtrlReq, kReqGoIdle);                              // back to idle (also after a failure)
  wait(kCtrlReq, kReqGoIdle, 0, kStatePolls);
  return ok;
}

}  // namespace hardware
