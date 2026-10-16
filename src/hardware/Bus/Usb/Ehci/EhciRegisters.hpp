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

// EHCI (USB 2.0 host controller) registers and data structures, from the
// Enhanced Host Controller Interface specification 1.0.
namespace hardware::ehci {

inline uint32_t read32(uint64_t address) { return *reinterpret_cast<volatile uint32_t*>(address); }
inline void write32(uint64_t address, uint32_t value) { *reinterpret_cast<volatile uint32_t*>(address) = value; }

// Capability registers (at the start of BAR 0).
constexpr uint32_t kCapLength = 0x00;  // byte
constexpr uint32_t kCapVersion = 0x02;  // 16 bits, BCD
constexpr uint32_t kCapHcsParams = 0x04, kCapHccParams = 0x08;
constexpr uint32_t kHcsPortCount = 0xF, kHcsPortPower = 1u << 4;
constexpr uint32_t kHccAddress64 = 1u << 0;

// Operational registers (at CAPLENGTH).
constexpr uint32_t kOpUsbCmd = 0x00, kOpUsbSts = 0x04, kOpUsbIntr = 0x08, kOpFrameIndex = 0x0C;
constexpr uint32_t kOpCtrlDsSegment = 0x10, kOpPeriodicListBase = 0x14, kOpAsyncListAddr = 0x18;
constexpr uint32_t kOpConfigFlag = 0x40, kOpPortSc = 0x44;

constexpr uint32_t kCmdRun = 1u << 0, kCmdReset = 1u << 1, kCmdPeriodicEnable = 1u << 4, kCmdAsyncEnable = 1u << 5;
constexpr uint32_t kCmdInterruptThreshold8 = 8u << 16;
constexpr uint32_t kStsHalted = 1u << 12, kStsPeriodicActive = 1u << 14, kStsAsyncActive = 1u << 15;

// PORTSC bits.
constexpr uint32_t kPortConnected = 1u << 0, kPortConnectChange = 1u << 1, kPortEnabled = 1u << 2,
                   kPortEnableChange = 1u << 3, kPortOverCurrentChange = 1u << 5, kPortReset = 1u << 8,
                   kPortPower = 1u << 12, kPortOwner = 1u << 13;
constexpr uint32_t kPortLineStatusShift = 10, kPortLineStatusMask = 3u << 10;  // 1 = K state: low-speed device
constexpr uint32_t kPortWriteOneToClear = kPortConnectChange | kPortEnableChange | kPortOverCurrentChange;

// Legacy-support extended capability (PCI configuration space).
constexpr uint32_t kLegacyBiosOwned = 1u << 16, kLegacyOsOwned = 1u << 24;

constexpr uint32_t kLinkTerminate = 1u << 0;
constexpr uint32_t kLinkTypeQueueHead = 1u << 1;

// Queue element transfer descriptor token.
constexpr uint32_t kTokenActive = 1u << 7, kTokenHalted = 1u << 6;
constexpr uint32_t kTokenErrors = 0x78;  // data buffer error, babble, transaction error, missed micro-frame
constexpr uint32_t kTokenPidOut = 0u << 8, kTokenPidIn = 1u << 8, kTokenPidSetup = 2u << 8;
constexpr uint32_t kTokenErrorCount3 = 3u << 10;
constexpr uint32_t kTokenInterruptOnComplete = 1u << 15;
constexpr uint32_t kTokenToggle = 1u << 31;

// Queue element transfer descriptor (qTD): up to five pages of data for one
// stage of a transfer. 64-bit capable controllers use the buffer_high words.
struct QtdFields {
  uint32_t next;
  uint32_t alternate;
  uint32_t token;  // status 7:0, PID 9:8, error count 11:10, page 14:12, IOC 15, bytes 30:16, toggle 31
  uint32_t buffer[5];
  uint32_t buffer_high[5];
};
static_assert(sizeof(QtdFields) == 52);

struct alignas(32) Qtd : QtdFields {
  uint32_t padding[3];
};
static_assert(sizeof(Qtd) == 64);

constexpr uint32_t kEndpointHeadOfList = 1u << 15, kEndpointDataToggleFromTd = 1u << 14;
constexpr uint32_t kEndpointSpeedHigh = 2u << 12;

// Queue head: one per endpoint. The overlay holds the qTD being worked on.
struct alignas(32) Qh {
  uint32_t horizontal;
  uint32_t characteristics;  // address 6:0, inactivate 7, endpoint 11:8, speed 13:12, DTC 14, H 15, max packet 26:16, RL 31:28
  uint32_t capabilities;     // S-mask 7:0, C-mask 15:8, hub address, port, mult 31:30
  uint32_t current;
  QtdFields overlay;
  uint32_t padding[15];
};
static_assert(sizeof(Qh) == 128);

}  // namespace hardware::ehci
