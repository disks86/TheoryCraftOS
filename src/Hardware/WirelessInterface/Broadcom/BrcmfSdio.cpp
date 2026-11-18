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

// The SDIO transport after Linux brcmfmac bcmsdh.c and sdio.c; see BrcmfSdio.hpp.
#include "Hardware/WirelessInterface/Broadcom/BrcmfSdio.hpp"
#include "Kernel/Delay.hpp"
#include "Utility/ByteStream.hpp"

namespace hardware {

using namespace brcmf_sdio;

namespace {
constexpr uint8_t kFunction1 = 1;
constexpr uint64_t kMaxTransfer = 2048;     // per CMD53 here (Linux sends up to the window boundary in one buffer)
}  // namespace

bool BrcmfSdioBackplane::set_window(uint32_t address) {
  const uint32_t window = address & kWindowMask;
  if (window_valid_ && window == window_) return true;
  uint32_t value = window >> 8;
  for (uint32_t i = 0; i < 3; ++i, value >>= 8) {
    if (!sdio_.write8(kFunction1, kSbAddrLow + i, static_cast<uint8_t>(value & 0xff))) {
      window_valid_ = false;
      return false;
    }
  }
  window_ = window;
  window_valid_ = true;
  return true;
}

bool BrcmfSdioBackplane::read32(uint32_t address, uint32_t* value) {
  if ((address & 3) || !set_window(address)) return false;
  uint8_t bytes[4] = {};
  if (!sdio_.read_block(kFunction1, (address & kOffsetMask) | kAccess24BFlag, ByteView(bytes))) return false;
  *value = uint32_t{bytes[0]} | uint32_t{bytes[1]} << 8 | uint32_t{bytes[2]} << 16 | uint32_t{bytes[3]} << 24;
  return true;
}

bool BrcmfSdioBackplane::write32(uint32_t address, uint32_t value) {
  if ((address & 3) || !set_window(address)) return false;
  const uint8_t bytes[4] = {static_cast<uint8_t>(value), static_cast<uint8_t>(value >> 8), static_cast<uint8_t>(value >> 16), static_cast<uint8_t>(value >> 24)};
  return sdio_.write_block(kFunction1, (address & kOffsetMask) | kAccess24BFlag, ConstByteView(bytes));
}

bool BrcmfSdioBackplane::write_block(uint32_t address, ConstByteView data) {
  if (data.size() > 0x100000000ull - address) return false;
  uint64_t done = 0;
  while (done < data.size()) {
    const uint32_t at = address + static_cast<uint32_t>(done);
    if (!set_window(at)) return false;
    const uint32_t offset = at & kOffsetMask;
    uint64_t chunk = kOffsetLimit - offset;     // up to the window's end
    if (chunk > kMaxTransfer) chunk = kMaxTransfer;
    if (chunk > data.size() - done) chunk = data.size() - done;
    if (!sdio_.write_block(kFunction1, offset | kAccess24BFlag, data.subview(done, chunk))) return false;
    done += chunk;
  }
  return true;
}

bool BrcmfSdioBackplane::read_block(uint32_t address, ByteView data) {
  if (data.size() > 0x100000000ull - address) return false;
  uint64_t done = 0;
  while (done < data.size()) {
    const uint32_t at = address + static_cast<uint32_t>(done);
    if (!set_window(at)) return false;
    const uint32_t offset = at & kOffsetMask;
    uint64_t chunk = kOffsetLimit - offset;
    if (chunk > kMaxTransfer) chunk = kMaxTransfer;
    if (chunk > data.size() - done) chunk = data.size() - done;
    if (!sdio_.read_block(kFunction1, offset | kAccess24BFlag, data.subview(done, chunk))) return false;
    done += chunk;
  }
  return true;
}

bool brcmf_sdio_enable_function(IWifiSdio& sdio, uint8_t function_bit, uint32_t timeout_us) {
  uint8_t enabled = 0;
  if (!sdio.read8(0, kCccrIoEnable, &enabled) || !sdio.write8(0, kCccrIoEnable, static_cast<uint8_t>(enabled | function_bit))) return false;
  for (uint32_t waited = 0;; waited += 100) {
    uint8_t ready = 0;
    if (!sdio.read8(0, kCccrIoReady, &ready)) return false;
    if (ready & function_bit) return true;
    if (waited >= timeout_us) return false;
    delay_us(100);
  }
}

bool brcmf_sdio_request_alp(IWifiSdio& sdio) {
  // ask for ALP only, hardware clock requests off
  const uint8_t request = kForceHwClkReqOff | kAlpAvailReq;
  if (!sdio.write8(kFunction1, kChipClkCsr, request)) return false;
  uint8_t value = 0;
  if (!sdio.read8(kFunction1, kChipClkCsr, &value)) return false;
  if ((value & ~kAvailBits) != request) return false;     // the register is not there (wrote x, read y)
  for (uint32_t waited = 0; (value & kAlpAvail) == 0; waited += 100) {
    if (waited >= kPmuMaxTransitionUs) return false;
    delay_us(100);
    if (!sdio.read8(kFunction1, kChipClkCsr, &value)) return false;
  }
  if (!sdio.write8(kFunction1, kChipClkCsr, kForceHwClkReqOff | kForceAlp)) return false;
  delay_us(65);
  // the extra SDIO pull-ups off
  return sdio.write8(kFunction1, kSdioPullUp, 0);
}

bool brcmf_sdio_request_ht(IWifiSdio& sdio) {
  if (!sdio.write8(kFunction1, kChipClkCsr, kHtAvailReq)) return false;
  uint8_t value = 0;
  for (uint32_t waited = 0;; waited += 100) {
    if (!sdio.read8(kFunction1, kChipClkCsr, &value)) return false;
    if (value & kHtAvail) return true;
    if (waited >= kPmuMaxTransitionUs) return false;
    delay_us(100);
  }
}

bool brcmf_sdio_shared_pointer_valid(uint32_t value) { return value != 0 && ((~value >> 16) & 0xffff) != (value & 0xffff); }

bool brcmf_sdio_parse_shared(ConstByteView bytes, uint32_t address, BrcmfSdioShared* out) {
  *out = BrcmfSdioShared();
  if (bytes.size() < kSharedBytes) return false;
  ByteReader in(bytes);
  out->address = address;
  out->flags = in.le32();
  out->trap_addr = in.le32();
  (void)in.le32();    // assert_exp_addr
  (void)in.le32();    // assert_file_addr
  out->assert_line = in.le32();
  out->console_addr = in.le32();
  if (!in.ok()) return false;
  return (out->flags & kSharedVersionMask) <= kSharedVersion;
}

}  // namespace hardware
