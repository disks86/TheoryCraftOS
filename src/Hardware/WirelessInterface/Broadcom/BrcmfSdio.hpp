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

// The SDIO transport of the Broadcom FullMAC chips (the Raspberry Pi's CYW43455, the 43430 of the Pi 3 and Zero W, the AP62xx modules), after Linux brcmfmac
// bcmsdh.c and sdio.c:
//
//   - Function 1 reaches the backplane through a 32 KiB window: the window's address bits 15..31 go into SBADDRLOW/MID/HIGH (function 1 registers 0x1000A..C,
//     bits 8..15, 16..23, 24..31 of the address; brcmf_sdiod_set_backplane_window), and a CMD53 to (address & 0x7FFF) | 0x8000 (the "2/4 byte access" flag) reads or
//     writes 32 bits there (brcmf_sdiod_readl/writel). RAM transfers are CMD53 blocks split at the window boundary (brcmf_sdiod_ramrw).
//   - Clocks: CHIPCLKCSR (0x1000E) asks for the ALP clock (brcmf_sdio_buscoreprep: request, wait for ALP_AVAIL up to 15 ms, force ALP) before the backplane is touched,
//     and for the HT clock (brcmf_sdio_htclk) before the download.
//   - After the ARM runs, the firmware writes the address of its sdpcm_shared structure into the last RAM word (it replaces the NVRAM's length token); Linux reads it
//     for traps and the console (brcmf_sdio_readshared).
#pragma once
#include <stdint.h>
#include "Hardware/WirelessInterface/Broadcom/BrcmfBackplane.hpp"
#include "Hardware/WirelessInterface/IWifiChip.hpp"

namespace hardware {

namespace brcmf_sdio {
// CCCR (function 0)
constexpr uint32_t kCccrIoEnable = 0x02, kCccrIoReady = 0x03;
constexpr uint8_t kFunction1Bit = 0x02, kFunction2Bit = 0x04;
// function 1 registers
constexpr uint32_t kSbAddrLow = 0x1000a, kSbAddrMid = 0x1000b, kSbAddrHigh = 0x1000c, kChipClkCsr = 0x1000e, kSdioPullUp = 0x1000f;
// CHIPCLKCSR bits
constexpr uint8_t kForceAlp = 0x01, kForceHt = 0x02, kForceIlp = 0x04, kAlpAvailReq = 0x08, kHtAvailReq = 0x10, kForceHwClkReqOff = 0x20, kAlpAvail = 0x40, kHtAvail = 0x80;
constexpr uint8_t kAvailBits = kHtAvail | kAlpAvail;
constexpr uint32_t kPmuMaxTransitionUs = 15000;        // PMU_MAX_TRANSITION_DLY
// the backplane window
constexpr uint32_t kOffsetMask = 0x07fff, kOffsetLimit = 0x08000, kAccess24BFlag = 0x08000, kWindowMask = 0xffff8000;
// SDIO device core registers (struct sdpcmd_regs; offsets after Linux's layout, unverified on hardware)
constexpr uint32_t kCoreIntStatus = 0x20, kCoreToSbMailboxData = 0x48;
constexpr uint32_t kSdpcmProtocolVersion = 4, kSmbDataVersionShift = 16;
// sdpcm_shared
constexpr uint32_t kSharedVersion = 0x0003, kSharedVersionMask = 0x00ff, kSharedAssertBuilt = 0x0100, kSharedAssert = 0x0200, kSharedTrap = 0x0400;
constexpr uint32_t kSharedBytes = 4 * 7 + 32 + 4;      // flags, trap, assert exp/file/line, console, msgtrace, tag[32], brpt
constexpr uint32_t kSharedConsoleAddrOffset = 20;
}  // namespace brcmf_sdio

class BrcmfSdioBackplane final : public IBrcmfBackplane {
 public:
  explicit BrcmfSdioBackplane(IWifiSdio& sdio) : sdio_(sdio) {}
  bool read32(uint32_t address, uint32_t* value) override;
  bool write32(uint32_t address, uint32_t value) override;
  bool write_block(uint32_t address, ConstByteView data) override;
  bool read_block(uint32_t address, ByteView data) override;
  void forget_window() { window_valid_ = false; }

  IWifiSdio& sdio() { return sdio_; }

 private:
  [[nodiscard]] bool set_window(uint32_t address);
  IWifiSdio& sdio_;
  uint32_t window_ = 0;
  bool window_valid_ = false;
};

// Sets the function's enable bit in the CCCR and waits (up to `timeout_us`) for its ready bit.
[[nodiscard]] bool brcmf_sdio_enable_function(IWifiSdio& sdio, uint8_t function_bit, uint32_t timeout_us);
// brcmf_sdio_buscoreprep: the ALP clock, then the pull-ups off. False if CHIPCLKCSR does not take the request or ALP never comes.
[[nodiscard]] bool brcmf_sdio_request_alp(IWifiSdio& sdio);
// The HT clock (brcmf_sdio_htclk(bus, true)): request it and wait for HT_AVAIL.
[[nodiscard]] bool brcmf_sdio_request_ht(IWifiSdio& sdio);

struct BrcmfSdioShared {
  uint32_t address = 0;
  uint32_t flags = 0;
  uint32_t trap_addr = 0;
  uint32_t assert_line = 0;
  uint32_t console_addr = 0;
};
// The last-word value is a pointer and not still the NVRAM's length token ((~x >> 16) == (x & 0xffff)) or 0 (brcmf_sdio_readshared's check).
[[nodiscard]] bool brcmf_sdio_shared_pointer_valid(uint32_t value);
// sdpcm_shared from its bytes; false if short or of a newer version than 3.
[[nodiscard]] bool brcmf_sdio_parse_shared(ConstByteView bytes, uint32_t address, BrcmfSdioShared* out);

}  // namespace hardware
