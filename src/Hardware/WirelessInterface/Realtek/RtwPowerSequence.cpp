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

#include "Hardware/WirelessInterface/Realtek/RtwPowerSequence.hpp"
#include "Kernel/Delay.hpp"

namespace hardware {

using namespace rtw_power;

namespace {

constexpr uint32_t kRegSysPwCtrl = 0x0004;
constexpr uint8_t kPfmWowl = 1u << 3;
constexpr uint8_t B0 = 1 << 0, B1 = 1 << 1, B2 = 1 << 2, B3 = 1 << 3, B4 = 1 << 4, B5 = 1 << 5, B6 = 1 << 6, B7 = 1 << 7;
constexpr uint8_t ALL = kInterfaceAll, PCI = kInterfacePci, USB = kInterfaceUsb, SDIO = kInterfaceSdio;
constexpr uint8_t MAC = kAddressMac, SDIO_BASE = kAddressSdio;

constexpr RtwPowerCommand W(uint16_t offset, uint8_t interfaces, uint8_t mask, uint8_t value, uint8_t base = MAC) { return {offset, kCutAll, interfaces, base, kWrite, mask, value}; }
constexpr RtwPowerCommand P(uint16_t offset, uint8_t interfaces, uint8_t mask, uint8_t value, uint8_t base = MAC) { return {offset, kCutAll, interfaces, base, kPoll, mask, value}; }
constexpr RtwPowerCommand D(uint16_t amount, uint8_t interfaces, uint8_t unit) { return {amount, kCutAll, interfaces, MAC, kDelay, 0, unit}; }
constexpr RtwPowerCommand END = {0xFFFF, kCutAll, ALL, 0, kEnd, 0, 0};

// ---- 8822C (rtw8822c.c)
constexpr RtwPowerCommand k8822cCardDisToCardEmu[] = {
    W(0x002E, ALL, B2, B2),
    W(0x002D, ALL, B0, 0),
    W(0x007F, ALL, B7, 0),
    W(0x004A, USB, B0, 0),
    W(0x0005, ALL, B3 | B4 | B7, 0),
    END,
};
constexpr RtwPowerCommand k8822cCardEmuToAct[] = {
    W(0x0000, USB | SDIO, B5, 0),
    W(0x0005, ALL, B4 | B3 | B2, 0),
    W(0x0075, PCI, B0, B0),
    P(0x0006, ALL, B1, B1),
    W(0x0075, PCI, B0, 0),
    W(0xFF1A, USB, 0xFF, 0),
    W(0x002E, ALL, B3, 0),
    W(0x0006, ALL, B0, B0),
    W(0x0005, ALL, B4 | B3, 0),
    W(0x1018, ALL, B2, B2),
    W(0x0005, ALL, B0, B0),
    P(0x0005, ALL, B0, 0),
    W(0x0074, PCI, B5, B5),
    W(0x0071, PCI, B4, 0),
    W(0x0062, PCI, B7 | B6 | B5, B7 | B6 | B5),
    W(0x0061, PCI, B7 | B6 | B5, 0),
    W(0x001F, ALL, B7 | B6 | B5, B7),
    W(0x00EF, ALL, B7 | B6 | B5, B7),
    W(0x1045, ALL, B4, B4),
    W(0x0010, ALL, B2, B2),
    W(0x1064, ALL, B1, B1),
    END,
};
constexpr ArrayView<const RtwPowerCommand> k8822cPowerOn[] = {k8822cCardDisToCardEmu, k8822cCardEmuToAct};

// ---- 8822B (rtw8822b.c)
constexpr RtwPowerCommand k8822bCardDisToCardEmu[] = {
    W(0x0086, SDIO, B0, 0, SDIO_BASE),
    P(0x0086, SDIO, B1, B1, SDIO_BASE),
    W(0x004A, USB, B0, 0),
    W(0x0005, ALL, B3 | B4 | B7, 0),
    W(0x0300, PCI, 0xFF, 0),
    W(0x0301, PCI, 0xFF, 0),
    END,
};
constexpr RtwPowerCommand k8822bCardEmuToAct[] = {
    W(0x0012, ALL, B1, 0),
    W(0x0012, ALL, B0, B0),
    W(0x0020, ALL, B0, B0),
    D(1, ALL, kDelayMs),
    W(0x0000, ALL, B5, 0),
    W(0x0005, ALL, B4 | B3 | B2, 0),
    W(0x0075, PCI, B0, B0),
    P(0x0006, ALL, B1, B1),
    W(0x0075, PCI, B0, 0),
    W(0xFF1A, USB, 0xFF, 0),
    W(0x0006, ALL, B0, B0),
    W(0x0005, ALL, B7, 0),
    W(0x0005, ALL, B4 | B3, 0),
    W(0x10C3, USB, B0, B0),
    W(0x0005, ALL, B0, B0),
    P(0x0005, ALL, B0, 0),
    W(0x0020, ALL, B3, B3),
    W(0x10A8, ALL, 0xFF, 0),
    W(0x10A9, ALL, 0xFF, 0xEF),
    W(0x10AA, ALL, 0xFF, 0x0C),
    W(0x0068, SDIO, B4, B4),
    W(0x0029, ALL, 0xFF, 0xF9),
    W(0x0024, ALL, B2, 0),
    W(0x0074, PCI, B5, B5),
    W(0x00AF, ALL, B5, B5),
    END,
};
constexpr ArrayView<const RtwPowerCommand> k8822bPowerOn[] = {k8822bCardDisToCardEmu, k8822bCardEmuToAct};

// ---- 8821C (rtw8821c.c)
constexpr RtwPowerCommand k8821cCardDisToCardEmu[] = {
    W(0x0086, SDIO, B0, 0, SDIO_BASE),
    P(0x0086, SDIO, B1, B1, SDIO_BASE),
    W(0x004A, USB, B0, 0),
    W(0x0005, ALL, B3 | B4 | B7, 0),
    W(0x0300, PCI, 0xFF, 0),
    W(0x0301, PCI, 0xFF, 0),
    END,
};
constexpr RtwPowerCommand k8821cCardEmuToAct[] = {
    W(0x0020, USB | SDIO, B0, B0),
    D(1, USB | SDIO, kDelayMs),
    W(0x0000, USB | SDIO, B5, 0),
    W(0x0005, ALL, B4 | B3 | B2, 0),
    W(0x0075, PCI, B0, B0),
    P(0x0006, ALL, B1, B1),
    W(0x0075, PCI, B0, 0),
    W(0x0006, ALL, B0, B0),
    W(0x0005, ALL, B7, 0),
    W(0x0005, ALL, B4 | B3, 0),
    W(0x10C3, USB, B0, B0),
    W(0x0005, ALL, B0, B0),
    P(0x0005, ALL, B0, 0),
    W(0x0020, ALL, B3, B3),
    W(0x0074, PCI, B5, B5),
    END,
};
constexpr ArrayView<const RtwPowerCommand> k8821cPowerOn[] = {k8821cCardDisToCardEmu, k8821cCardEmuToAct};

// ---- 8723D (rtw8723d.c; the least certain of the four)
constexpr RtwPowerCommand k8723dCardDisToCardEmu[] = {
    W(0x0005, ALL, B3 | B7, 0),
    W(0x0086, SDIO, B0, 0, SDIO_BASE),
    P(0x0086, SDIO, B1, B1, SDIO_BASE),
    W(0x004A, USB, B0, 0),
    W(0x0005, ALL, B3 | B4, 0),
    W(0x0023, SDIO, B4, 0),
    W(0x0301, PCI, 0xFF, 0),
    END,
};
constexpr RtwPowerCommand k8723dCardEmuToAct[] = {
    W(0x0020, USB | SDIO, B0, B0),
    D(1, USB | SDIO, kDelayMs),
    W(0x0000, USB | SDIO, B5, 0),
    W(0x0005, ALL, B4 | B3 | B2, 0),
    W(0x0075, PCI, B0, B0),
    P(0x0006, ALL, B1, B1),
    W(0x0075, PCI, B0, 0),
    W(0x0006, ALL, B0, B0),
    W(0x0005, ALL, B7, 0),
    W(0x0005, ALL, B4 | B3, 0),
    W(0x0005, ALL, B0, B0),
    P(0x0005, ALL, B0, 0),
    W(0x0010, ALL, B6, B6),
    W(0x0049, ALL, B1, B1),
    W(0x0063, ALL, B1, B1),
    W(0x0062, ALL, B1, 0),
    W(0x0058, ALL, B0, B0),
    W(0x005A, ALL, B1, B1),
    W(0x0068, ALL, B3, B3),
    W(0x0069, ALL, B6, B6),
    END,
};
constexpr ArrayView<const RtwPowerCommand> k8723dPowerOn[] = {k8723dCardDisToCardEmu, k8723dCardEmuToAct};

constexpr RtwPowerCommand kOffMac[] = {
    W(0x0005, ALL, B1, B1),
    P(0x0005, ALL, B1, 0),
    END,
};
constexpr ArrayView<const RtwPowerCommand> kPowerOffMac[] = {kOffMac};

uint32_t command_offset(const RtwPowerCommand& command) {
  return command.base == kAddressSdio ? (command.offset | kSdioLocalOffset) : command.offset;
}

bool poll_once(INicRegisters& registers, uint32_t offset, uint8_t mask, uint8_t value) {
  for (uint32_t i = 0; i < kPollCount; ++i) {
    if ((registers.read8(offset) & mask) == (value & mask)) return true;
    delay_us(kPollIntervalUs);
  }
  return false;
}

}  // namespace

RtwPowerResult rtw_run_power_program(INicRegisters& registers, ArrayView<const RtwPowerCommand> program, const RtwPowerOptions& options) {
  for (const RtwPowerCommand& command : program) {
    if (command.command == kEnd) break;
    if (!(command.interface_mask & options.interface_mask) || !(command.cut_mask & options.cut_mask)) continue;
    const uint32_t offset = command_offset(command);
    switch (command.command) {
      case kWrite: {
        const uint8_t old = registers.read8(offset);
        registers.write8(offset, static_cast<uint8_t>((old & ~command.mask) | (command.value & command.mask)));
        break;
      }
      case kPoll: {
        if (poll_once(registers, offset, command.mask, command.value)) break;
        if (!options.pcie) return RtwPowerResult::Timeout;
        const uint8_t value = registers.read8(kRegSysPwCtrl);
        if (options.toggle_8723d) registers.write8(kRegSysPwCtrl, static_cast<uint8_t>(value & ~kPfmWowl));
        registers.write8(kRegSysPwCtrl, static_cast<uint8_t>(value | kPfmWowl));
        registers.write8(kRegSysPwCtrl, static_cast<uint8_t>(value & ~kPfmWowl));
        if (options.toggle_8723d) registers.write8(kRegSysPwCtrl, static_cast<uint8_t>(value | kPfmWowl));
        if (!poll_once(registers, offset, command.mask, command.value)) return RtwPowerResult::Timeout;
        break;
      }
      case kDelay:
        delay_us(command.value == kDelayUs ? command.offset : uint64_t{command.offset} * 1000);
        break;
      case kRead:
        break;
      default:
        return RtwPowerResult::BadCommand;
    }
  }
  return RtwPowerResult::Ok;
}

RtwPowerResult rtw_run_power_sequence(INicRegisters& registers, ArrayView<const ArrayView<const RtwPowerCommand>> sequence, const RtwPowerOptions& options) {
  for (const ArrayView<const RtwPowerCommand>& program : sequence) {
    const RtwPowerResult result = rtw_run_power_program(registers, program, options);
    if (result != RtwPowerResult::Ok) return result;
  }
  return RtwPowerResult::Ok;
}

ArrayView<const ArrayView<const RtwPowerCommand>> rtw8822c_power_on() { return ArrayView<const ArrayView<const RtwPowerCommand>>(k8822cPowerOn); }
ArrayView<const ArrayView<const RtwPowerCommand>> rtw8822b_power_on() { return ArrayView<const ArrayView<const RtwPowerCommand>>(k8822bPowerOn); }
ArrayView<const ArrayView<const RtwPowerCommand>> rtw8821c_power_on() { return ArrayView<const ArrayView<const RtwPowerCommand>>(k8821cPowerOn); }
ArrayView<const ArrayView<const RtwPowerCommand>> rtw8723d_power_on() { return ArrayView<const ArrayView<const RtwPowerCommand>>(k8723dPowerOn); }
ArrayView<const ArrayView<const RtwPowerCommand>> rtw_power_off_mac() { return ArrayView<const ArrayView<const RtwPowerCommand>>(kPowerOffMac); }

}  // namespace hardware
