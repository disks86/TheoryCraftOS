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

#include "Hardware/Tpm/TpmTis.hpp"
#include "Kernel/Delay.hpp"

namespace hardware {

using namespace tis;

namespace {
constexpr uint32_t kStatusPolls = 20000, kPollUs = 100;           // up to 2 s for a state change
constexpr uint32_t kResponsePolls = 600000;                       // up to 60 s for a command (key generation takes long)
constexpr uint32_t kMaxResponse = 4096;
}  // namespace

bool TpmTis::probe() {
  const uint8_t access = registers_.read8(kAccess);
  return access != 0xFF && (access & kAccessValid);
}

bool TpmTis::request_locality() {
  if ((registers_.read8(kAccess) & (kAccessValid | kAccessActiveLocality)) == (kAccessValid | kAccessActiveLocality)) return true;
  registers_.write8(kAccess, kAccessRequestUse);
  for (uint32_t i = 0; i < kStatusPolls; ++i) {
    if ((registers_.read8(kAccess) & (kAccessValid | kAccessActiveLocality)) == (kAccessValid | kAccessActiveLocality)) return true;
    delay_us(kPollUs);
  }
  return false;
}

uint32_t TpmTis::burst_count() {
  for (uint32_t i = 0; i < kStatusPolls; ++i) {
    const uint32_t sts = registers_.read32(kSts);
    const uint32_t burst = (sts >> 8) & 0xFFFF;
    if (burst != 0 && burst != 0xFFFF) return burst;
    delay_us(kPollUs);
  }
  return 0;
}

bool TpmTis::wait_status(uint8_t mask, uint8_t value) {
  for (uint32_t i = 0; i < kStatusPolls; ++i) {
    if ((registers_.read8(kSts) & (mask | kStsValid)) == (value | kStsValid)) return true;
    delay_us(kPollUs);
  }
  return false;
}

bool TpmTis::go_ready() {
  registers_.write8(kSts, kStsCommandReady);
  return wait_status(kStsCommandReady, kStsCommandReady);
}

bool TpmTis::read_bytes(uint8_t* out, uint32_t count) {
  uint32_t done = 0;
  while (done < count) {
    const uint32_t burst = burst_count();
    if (burst == 0) return false;
    for (uint32_t i = 0; i < burst && done < count; ++i) {
      if (!(registers_.read8(kSts) & kStsDataAvail)) return false;     // the TPM has no more to say than it promised
      out[done++] = registers_.read8(kDataFifo);
    }
  }
  return true;
}

bool TpmTis::transmit(ConstByteView command, ByteBuffer* response) {
  if (command.size() < 10 || command.size() > kMaxResponse || !request_locality() || !go_ready()) return false;
  // the command, a burst at a time; dataExpect stays up until the last byte is in
  uint32_t sent = 0;
  const uint32_t total = static_cast<uint32_t>(command.size());
  while (sent < total) {
    const uint32_t burst = burst_count();
    if (burst == 0) return false;
    for (uint32_t i = 0; i < burst && sent < total; ++i) registers_.write8(kDataFifo, command[sent++]);
    if (sent < total && !wait_status(kStsDataExpect, kStsDataExpect)) return false;
  }
  if (!wait_status(kStsDataExpect, 0)) return false;                    // (the TPM thinks the command is complete: it was not cut short or too long)
  registers_.write8(kSts, kStsGo);
  bool available = false;
  for (uint32_t i = 0; i < kResponsePolls && !available; ++i) {
    available = (registers_.read8(kSts) & (kStsValid | kStsDataAvail)) == (kStsValid | kStsDataAvail);
    if (!available) delay_us(kPollUs);
  }
  if (!available) {
    go_ready();
    return false;
  }
  uint8_t header[10];
  bool ok = read_bytes(header, 10);
  const uint32_t length = ok ? (uint32_t{header[2]} << 24) | (uint32_t{header[3]} << 16) | (uint32_t{header[4]} << 8) | header[5] : 0;
  ok = ok && length >= 10 && length <= kMaxResponse;
  response->clear();
  if (ok) ok = response->append(ConstByteView(header, 10));
  if (ok && length > 10) {
    uint8_t body[kMaxResponse];
    ok = read_bytes(body, length - 10) && response->append(ConstByteView(body, length - 10));
  }
  go_ready();                                                           // back to idle (also after a failure)
  return ok;
}

}  // namespace hardware
