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

// Realtek power sequences (after rtw88/mac.c rtw_pwr_seq_parser and struct rtw_pwr_seq_cmd in rtw88/main.h): the chip's power states (card disabled, card
// emulation, active) are left and entered by short programs of byte-register commands. Each command names an offset, the chip cuts and host interfaces it applies to,
// the address space (MAC registers, or the SDIO local registers), and what to do: WRITE (read-modify-write of `mask` bits to `value`), POLL (wait until the masked
// byte equals the masked value, 50 us apart, at most 20000 times; on PCIe once more after toggling BIT_PFM_WOWL), DELAY (`offset` microseconds, or milliseconds
// when `value` is 1), END. A chip's "power on" is a list of such programs run in order (card disabled -> card emulation -> active).
//
// The tables for 8822C, 8822B, 8821C and 8723D were written from memory of rtw8822c.c, rtw8822b.c, rtw8821c.c and rtw8723d.c (trans_carddis_to_cardemu_*,
// trans_cardemu_to_act_*) and are NOT verified against the source or on hardware; 8822C is the one recalled with the most certainty.
#pragma once
#include <stdint.h>
#include "Hardware/NetworkInterface/NicDevice.hpp"
#include "Utility/ArrayView.hpp"

namespace hardware {

struct RtwPowerCommand {
  uint16_t offset;
  uint8_t cut_mask;
  uint8_t interface_mask;
  uint8_t base;          // RTW_PWR_ADDR_*
  uint8_t command;       // RTW_PWR_CMD_*
  uint8_t mask;
  uint8_t value;
};

namespace rtw_power {
constexpr uint8_t kRead = 0, kWrite = 1, kPoll = 2, kDelay = 3, kEnd = 4;
constexpr uint8_t kAddressMac = 0, kAddressUsb = 1, kAddressPcie = 2, kAddressSdio = 3;
constexpr uint8_t kInterfaceSdio = 1 << 0, kInterfaceUsb = 1 << 1, kInterfacePci = 1 << 2, kInterfaceAll = 0x0F;
constexpr uint8_t kCutAll = 0xFF;
constexpr uint8_t kDelayUs = 0, kDelayMs = 1;
constexpr uint32_t kSdioLocalOffset = 0x10250000;
constexpr uint32_t kPollCount = 20000, kPollIntervalUs = 50;
// The cut mask bit of a chip cut (REG_SYS_CFG1 bits 15:12; 0 = A cut): BIT(cut + 1), bit 0 being the test chip.
inline uint8_t cut_mask(uint32_t cut) { return cut < 7 ? static_cast<uint8_t>(1u << (cut + 1)) : 0x80; }
}  // namespace rtw_power

enum class RtwPowerResult : uint8_t { Ok, Timeout, BadCommand };

struct RtwPowerOptions {
  uint8_t interface_mask = rtw_power::kInterfacePci;
  uint8_t cut_mask = rtw_power::kCutAll;
  bool pcie = true;              // retry a failed poll once after toggling BIT_PFM_WOWL (REG_SYS_PW_CTRL 0x04 bit 3), as rtw_pwr_cmd_polling does on PCIe
  bool toggle_8723d = false;     // the 8723D's extra writes around that toggle
};

// Runs one program up to its END command (or the end of the view).
[[nodiscard]] RtwPowerResult rtw_run_power_program(INicRegisters& registers, ArrayView<const RtwPowerCommand> program, const RtwPowerOptions& options);
// Runs a list of programs in order; stops at the first that fails.
[[nodiscard]] RtwPowerResult rtw_run_power_sequence(INicRegisters& registers, ArrayView<const ArrayView<const RtwPowerCommand>> sequence, const RtwPowerOptions& options);

// The card-enable sequences (card disabled -> active). Empty for a chip without tables here.
ArrayView<const ArrayView<const RtwPowerCommand>> rtw8822c_power_on();
ArrayView<const ArrayView<const RtwPowerCommand>> rtw8822b_power_on();
ArrayView<const ArrayView<const RtwPowerCommand>> rtw8821c_power_on();
ArrayView<const ArrayView<const RtwPowerCommand>> rtw8723d_power_on();
// The common core of trans_act_to_cardemu_*: switch the MAC off (REG_APS_FSMCO bit 9, APFM_OFFMAC) and wait for the chip to clear it.
ArrayView<const ArrayView<const RtwPowerCommand>> rtw_power_off_mac();

}  // namespace hardware
