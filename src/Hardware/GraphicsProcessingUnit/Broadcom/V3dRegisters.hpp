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

namespace hardware::v3d {

// Register offsets and bits of the V3D 4.x graphics core (the VideoCore VI GPU of the BCM2711 in the Pi 4 and Pi 400): the "hub" block (identification, the
// MMU, the texture formatting unit) and core 0. Offsets are from the start of each block; the layout follows the Linux v3d driver's register header.
constexpr uint32_t bit(uint32_t n) { return 1u << n; }
constexpr uint32_t mask(uint32_t high, uint32_t low) { return (0xFFFFFFFFu >> (31 - high)) & ~((1u << low) - 1); }
constexpr uint32_t get_field(uint32_t value, uint32_t high, uint32_t low) { return (value & mask(high, low)) >> low; }

// --- Hub
constexpr uint64_t kHubAxiCfg = 0x00000;
constexpr uint64_t kHubIdent0 = 0x00008;  // "V3D" and the version
constexpr uint64_t kHubIdent1 = 0x0000C;  // feature bits, cores, revision, technology version
constexpr uint64_t kHubIdent2 = 0x00010;
constexpr uint64_t kHubIdent3 = 0x00014;
constexpr uint64_t kHubIntSts = 0x00050, kHubIntSet = 0x00054, kHubIntClr = 0x00058, kHubIntMskSts = 0x0005C, kHubIntMskSet = 0x00060, kHubIntMskClr = 0x00064;
constexpr uint32_t kHubIdent1WithMso = bit(19), kHubIdent1WithTsy = bit(18), kHubIdent1WithTfu = bit(17), kHubIdent1WithL3c = bit(16);
constexpr uint32_t kHubIdent2WithMmu = bit(8);
constexpr uint32_t kHubIntMmuWriteViolation = bit(5), kHubIntMmuPageInvalid = bit(4), kHubIntMmuCapExceeded = bit(3), kHubIntMso = bit(2), kHubIntTfuComplete = bit(1), kHubIntTfuFifo = bit(0);

// --- MMU (in the hub)
constexpr uint64_t kMmuCtl = 0x01200, kMmuPtPaBase = 0x01204, kMmuViolationId = 0x0122C, kMmuIllegalAddr = 0x01230, kMmuViolationAddr = 0x01234;
constexpr uint32_t kMmuCtlEnable = bit(0), kMmuCtlTlbClear = bit(2), kMmuCtlTlbClearing = bit(7);
constexpr uint32_t kMmuCtlWriteViolation = bit(12), kMmuCtlWriteViolationAbort = bit(11), kMmuCtlWriteViolationInt = bit(10);
constexpr uint32_t kMmuCtlPtInvalid = bit(20), kMmuCtlPtInvalidAbort = bit(19), kMmuCtlPtInvalidInt = bit(18), kMmuCtlPtInvalidEnable = bit(16);
constexpr uint32_t kMmuCtlCapExceeded = bit(27), kMmuCtlCapExceededAbort = bit(26), kMmuCtlCapExceededInt = bit(25);
constexpr uint32_t kMmuIllegalAddrEnable = bit(31);
constexpr uint32_t kMmuPageShift = 12;
// Page table entries: bits 27:0 hold the page number of the bus address, plus these flags.
constexpr uint32_t kPteSuperpage = bit(31), kPteWriteable = bit(29), kPteValid = bit(28);
constexpr uint32_t kPtePageMask = mask(27, 0);

// --- Core
constexpr uint64_t kCtlIdent0 = 0x00000, kCtlIdent1 = 0x00004, kCtlIdent2 = 0x00008;
constexpr uint64_t kCtlMiscCfg = 0x00018;
constexpr uint64_t kCtlL2CaCtl = 0x00020, kCtlSlCaCtl = 0x00024, kCtlL2TCaCtl = 0x00030, kCtlL2TFlSta = 0x00034, kCtlL2TFlEnd = 0x00038;
constexpr uint64_t kCtlIntSts = 0x00050, kCtlIntSet = 0x00054, kCtlIntClr = 0x00058, kCtlIntMskSts = 0x0005C, kCtlIntMskSet = 0x00060, kCtlIntMskClr = 0x00064;
constexpr uint32_t kL2CaCtlClear = bit(2), kL2CaCtlDisable = bit(1), kL2CaCtlEnable = bit(0);
constexpr uint32_t kL2TCaCtlFlush = bit(0), kL2TCaCtlTmuWriteCombineFlush = bit(8);
constexpr uint32_t kL2TCaCtlModeFlush = 0u << 1, kL2TCaCtlModeClear = 1u << 1, kL2TCaCtlModeClean = 2u << 1;
constexpr uint32_t kSlCaCtlAll = 0x0F0F0F0F;  // clear every slice cache: texture, uniform, instruction, vertex cache... at once

// Control list executers: CT0 is the binner, CT1 the renderer. Writing the end address (CTnQEA) starts a list at CTnQBA.
constexpr uint64_t kCleCt0Qba = 0x00160, kCleCt1Qba = 0x00164, kCleCt0Qea = 0x00168, kCleCt1Qea = 0x0016C;
constexpr uint64_t kCleCt0Cs = 0x00100, kCleCt1Cs = 0x00104, kCleCt0Ca = 0x00110, kCleCt1Ca = 0x00114;  // control status and current address
constexpr uint32_t kCleCt0QtsEnable = bit(1);
constexpr uint64_t kCleCt0Qts = 0x0015C, kCleCt0Qma = 0x00170, kCleCt0Qms = 0x00174;
constexpr uint64_t kCleBfc = 0x00134, kCleRfc = 0x00138;  // count of binning and rendering jobs flushed
constexpr uint64_t kPtbBpca = 0x00300, kPtbBpcs = 0x00304, kPtbBpoa = 0x00308, kPtbBpos = 0x0030C;  // binning memory in use, and the overflow block to give it

// Interrupts of the core.
constexpr uint32_t kIntFrDone = bit(0), kIntFlDone = bit(1), kIntOutOfMemory = bit(2), kIntSpillUse = bit(3), kIntTransformFeedback = bit(4);
constexpr uint32_t kIntViolation = bit(5);  // GMP violation
constexpr uint32_t kIntQpuMask = mask(27, 16);

// Errors the core reports (kErrStat).
constexpr uint64_t kErrStat = 0x00F20;
constexpr uint32_t kErrAnyMask = mask(15, 0);

}  // namespace hardware::v3d
