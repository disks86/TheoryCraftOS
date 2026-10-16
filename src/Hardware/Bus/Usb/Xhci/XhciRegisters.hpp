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

// xHCI register layout, TRB formats and MMIO helpers (xHCI spec rev 1.2).
// Private to Usb/Xhci/.
#pragma once
#include <stdint.h>

namespace hardware::xhci {

// --- MMIO. Registers must be touched with exact-width volatile accesses.
inline uint32_t read32(uint64_t address) { return *reinterpret_cast<volatile uint32_t*>(address); }
inline void write32(uint64_t address, uint32_t value) {
  *reinterpret_cast<volatile uint32_t*>(address) = value;
}
// 64-bit registers are written as two dwords, low first.
inline void write64(uint64_t address, uint64_t value) {
  write32(address, static_cast<uint32_t>(value));
  write32(address + 4, static_cast<uint32_t>(value >> 32));
}

// --- Capability registers (at the BAR).
constexpr uint32_t kCapLength = 0x00;       // u8
constexpr uint32_t kCapVersion = 0x02;      // u16
constexpr uint32_t kCapHcsParams1 = 0x04;   // slots [7:0], ports [31:24]
constexpr uint32_t kCapHcsParams2 = 0x08;   // scratchpad count hi [25:21], lo [31:27]
constexpr uint32_t kCapHccParams1 = 0x10;   // AC64 b0, CSZ b2, xECP [31:16]
constexpr uint32_t kCapDoorbellOffset = 0x14;
constexpr uint32_t kCapRuntimeOffset = 0x18;
constexpr uint32_t kHccAc64 = 1u << 0;
constexpr uint32_t kHccContextSize64 = 1u << 2;

// --- Operational registers (at BAR + CAPLENGTH).
constexpr uint32_t kOpUsbCmd = 0x00;
constexpr uint32_t kOpUsbSts = 0x04;
constexpr uint32_t kOpCrcr = 0x18;
constexpr uint32_t kOpDcbaap = 0x30;
constexpr uint32_t kOpConfig = 0x38;
constexpr uint32_t kOpPortBase = 0x400;  // PORTSC at +0x10 * port
constexpr uint32_t kPortStride = 0x10;
constexpr uint32_t kCmdRun = 1u << 0;
constexpr uint32_t kCmdReset = 1u << 1;
constexpr uint32_t kStsHalted = 1u << 0;
constexpr uint32_t kStsNotReady = 1u << 11;

// --- Port status and control. Writing 1 to the change bits (17-23) and to
// PED (bit 1) clears or disables, so writes start from the "neutral" subset.
constexpr uint32_t kPortConnected = 1u << 0;
constexpr uint32_t kPortEnabled = 1u << 1;
constexpr uint32_t kPortReset = 1u << 4;
constexpr uint32_t kPortResetChange = 1u << 21;
constexpr uint32_t kPortChangeBits = 0x00FE0000;  // bits 17-23, RW1C
// Fields that are safe to write back unchanged: PLS [8:5], PP [9], wake [27:25].
constexpr uint32_t kPortPreserveMask = (0xFu << 5) | (1u << 9) | (7u << 25);
inline uint32_t port_speed_id(uint32_t portsc) { return (portsc >> 10) & 0xF; }

// --- Runtime registers (at BAR + RTSOFF); interrupter 0.
constexpr uint32_t kRtInterrupter0 = 0x20;
constexpr uint32_t kIrErstSize = 0x08;
constexpr uint32_t kIrErstBase = 0x10;
constexpr uint32_t kIrDequeue = 0x18;
constexpr uint64_t kIrDequeueBusy = 1u << 3;  // EHB: write 1 to clear

// --- Extended capabilities.
constexpr uint32_t kExtCapLegacySupport = 1;
constexpr uint32_t kLegacyBiosOwned = 1u << 16;
constexpr uint32_t kLegacyOsOwned = 1u << 24;

// --- Transfer request blocks.
struct Trb {
  uint64_t parameter;
  uint32_t status;
  uint32_t control;
};
static_assert(sizeof(Trb) == 16);

enum TrbType : uint32_t {
  kTrbNormal = 1,
  kTrbSetupStage = 2,
  kTrbDataStage = 3,
  kTrbStatusStage = 4,
  kTrbLink = 6,
  kTrbEnableSlot = 9,
  kTrbDisableSlot = 10,
  kTrbAddressDevice = 11,
  kTrbConfigureEndpoint = 12,
  kTrbEvaluateContext = 13,
  kTrbResetEndpoint = 14,
  kTrbSetTrDequeuePointer = 16,
  kTrbNoop = 23,
  kTrbTransferEvent = 32,
  kTrbCommandCompletion = 33,
  kTrbPortStatusChange = 34,
};

constexpr uint32_t kTrbCycle = 1u << 0;
constexpr uint32_t kTrbToggleCycle = 1u << 1;  // on Link TRBs
constexpr uint32_t kTrbIoc = 1u << 5;
constexpr uint32_t kTrbInterruptOnShortPacket = 1u << 2;
constexpr uint32_t kTrbImmediateData = 1u << 6;
constexpr uint32_t kTrbDirectionIn = 1u << 16;  // data/status stage direction

inline uint32_t trb_type(uint32_t control) { return (control >> 10) & 0x3F; }
inline uint32_t completion_code(uint32_t status) { return status >> 24; }
constexpr uint32_t kCompletionSuccess = 1;
constexpr uint32_t kCompletionShortPacket = 13;

}  // namespace hardware::xhci
