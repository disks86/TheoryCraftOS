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

namespace hardware::bcm2711 {

// The BCM2711 SoC of the Raspberry Pi 4 and Pi 400, as the ARM cores see it with the firmware's default "low peripheral" layout (what the Raspberry Pi
// UEFI firmware leaves): the peripherals the VideoCore calls 0x7Exxxxxx are at 0xFExxxxxx. Interrupt numbers are GIC SPI numbers (the GIC interrupt ID is
// 32 more). The addresses come from the Linux device tree of the board (bcm2711.dtsi) and the datasheet; nothing here has been run on a real board.
constexpr uint64_t kPeripheralBase = 0xFE000000;
constexpr uint64_t kVideoCoreBusPeripheral = 0x7E000000;  // the same window in the VideoCore's address map

constexpr uint64_t kMailboxBase = kPeripheralBase + 0xB880;  // VideoCore mailbox 0 (read) and 1 (write)
constexpr uint64_t kMailboxLength = 0x40;

constexpr uint64_t kV3dHubBase = kPeripheralBase + 0xC00000;   // V3D 4.2 hub registers
constexpr uint64_t kV3dCoreBase = kPeripheralBase + 0xC04000;  // core 0 registers
constexpr uint64_t kV3dWindow = 0x4000;
constexpr uint32_t kV3dIrq = 106;  // SPI; hub and core share it

constexpr uint64_t kHevcBase = kPeripheralBase + 0xB00000;      // the HEVC decoder block ("hevc" registers of the rpivid driver)
constexpr uint64_t kHevcWindow = 0x10000;
constexpr uint64_t kHevcIntcBase = kPeripheralBase + 0xB10000;  // its interrupt controller ("intc")
constexpr uint64_t kHevcIntcWindow = 0x1000;
constexpr uint32_t kHevcIrq = 98;  // SPI

// The VideoCore sees ARM memory through bus aliases: the 0xC0000000 alias is uncached (L2 bypass) and is what mailbox messages and GPU buffers use.
constexpr uint32_t kBusAliasUncached = 0xC0000000;
constexpr uint32_t kBusAliasMask = 0x3FFFFFFF;
constexpr uint32_t arm_to_bus(uint64_t address) { return static_cast<uint32_t>(address & kBusAliasMask) | kBusAliasUncached; }
constexpr uint64_t bus_to_arm(uint32_t bus_address) { return bus_address & kBusAliasMask; }

// Part number of the Cortex-A72 cores (MIDR_EL1 bits 15:4).
constexpr uint32_t kCortexA72Part = 0xD08;

}  // namespace hardware::bcm2711
