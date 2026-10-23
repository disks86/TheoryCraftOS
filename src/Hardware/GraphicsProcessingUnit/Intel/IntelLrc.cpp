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

#include "Hardware/GraphicsProcessingUnit/Intel/IntelLrc.hpp"

namespace hardware {
namespace intel_gt {

namespace {
constexpr uint32_t kMiLoadRegisterImmBase = (0x22u << 23);
constexpr uint32_t kMiLriForcePosted = 1u << 12;
constexpr uint32_t kCtxCtrlRestoreInhibit = 1u << 0, kCtxCtrlRsCtxEnable = 1u << 1, kCtxCtrlSaveInhibit = 1u << 2, kCtxCtrlInhibitSynCtxSwitch = 1u << 3;
constexpr uint32_t kStopRing = 1u << 8;
constexpr uint32_t kRingValid = 1;

constexpr uint32_t masked_enable(uint32_t bits) { return (bits << 16) | bits; }
constexpr uint32_t masked_disable(uint32_t bits) { return bits << 16; }

// The layout of the register state is a sequence of runs: some skipped dwords, then an MI_LOAD_REGISTER_IMM header followed by (register offset, value)
// pairs. This table lists the runs for a Gen9 video engine, as the i915 driver's gen9_xcs_offsets does: a skip, then the register offsets (relative to the
// engine's base) of a load. Encoded here as plain data instead of a byte stream.
struct Run {
  uint8_t skip;           // dwords to leave alone before the load header
  uint8_t count;          // registers loaded
  uint16_t first, last;   // index range into kOffsets
};

constexpr uint16_t kOffsets[] = {
    // run 0: 14 registers
    0x244, 0x034, 0x030, 0x038, 0x03c, 0x168, 0x140, 0x110, 0x11c, 0x114, 0x118, 0x1c0, 0x1c4, 0x1c8,
    // run 1: 9 registers
    0x3a8, 0x28c, 0x288, 0x284, 0x280, 0x27c, 0x278, 0x274, 0x270,
    // run 2: 1 register
    0x200,
    // run 3: 44 registers
    0x028, 0x09c, 0x0c0, 0x178, 0x17c, 0x358, 0x170, 0x150, 0x154, 0x158, 0x41c,
    0x600, 0x604, 0x608, 0x60c, 0x610, 0x614, 0x618, 0x61c, 0x620, 0x624, 0x628, 0x62c, 0x630, 0x634, 0x638, 0x63c,
    0x640, 0x644, 0x648, 0x64c, 0x650, 0x654, 0x658, 0x65c, 0x660, 0x664, 0x668, 0x66c, 0x670, 0x674, 0x678, 0x67c,
    0x068,
};
constexpr Run kRuns[] = {
    {1, 14, 0, 14},
    {3, 9, 14, 23},
    {13, 1, 23, 24},
    {13, 44, 24, 68},
};
static_assert(sizeof(kOffsets) / sizeof(kOffsets[0]) == 68, "gen9 video engine register list");
}  // namespace

bool lrc_init_gen9_xcs(ArrayView<uint32_t> state, uint32_t mmio_base, bool inhibit_restore) {
  if (state.size() != kLrcStateDwords) return false;
  for (uint32_t& d : state) d = 0;
  uint64_t at = 0;
  for (const Run& run : kRuns) {
    at += run.skip;
    // header + 2 dwords per register must fit in the page
    if (at + 1 + uint64_t{run.count} * 2 > state.size()) return false;
    state[at++] = kMiLoadRegisterImmBase | (2u * run.count - 1u) | kMiLriForcePosted;
    for (uint16_t i = run.first; i < run.last; ++i) {
      state[at] = mmio_base + kOffsets[i];
      at += 2;
    }
  }
  // Context control, as the driver sets it for Gen9: no synchronous context switch inhibit, and no save inhibit or render-state restore. A context with no saved
  // state yet (the first start) has the restore of the saved engine state inhibited; the registers listed above are loaded either way.
  state[kCtxContextControl] = masked_enable(kCtxCtrlInhibitSynCtxSwitch) | masked_disable(kCtxCtrlRestoreInhibit) | (inhibit_restore ? kCtxCtrlRestoreInhibit : 0u) |
                              masked_disable(kCtxCtrlSaveInhibit | kCtxCtrlRsCtxEnable);
  // The ring is not stopped when the context starts.
  state[kCtxRingMiMode] = masked_disable(kStopRing);
  return true;
}

bool lrc_set_ring(ArrayView<uint32_t> state, uint32_t ring_ggtt_address, uint32_t ring_bytes, uint32_t head, uint32_t tail) {
  if (state.size() != kLrcStateDwords || ring_bytes < 4096 || (ring_bytes & 4095) || (ring_ggtt_address & 4095)) return false;
  if (head >= ring_bytes || tail >= ring_bytes || (head & 7) || (tail & 7)) return false;
  state[kCtxRingStart] = ring_ggtt_address;
  state[kCtxRingHead] = head;
  state[kCtxRingTail] = tail;
  state[kCtxRingCtl] = (ring_bytes - 4096) | kRingValid;
  return true;
}

bool lrc_set_tail(ArrayView<uint32_t> state, uint32_t tail) {
  if (state.size() != kLrcStateDwords || (tail & 7)) return false;
  state[kCtxRingTail] = tail;
  return true;
}

bool lrc_set_page_directories(ArrayView<uint32_t> state, const uint64_t directories[4]) {
  if (state.size() != kLrcStateDwords) return false;
  const uint32_t udw[4] = {kCtxPdp0Udw, kCtxPdp1Udw, kCtxPdp2Udw, kCtxPdp3Udw};
  const uint32_t ldw[4] = {kCtxPdp0Ldw, kCtxPdp1Ldw, kCtxPdp2Ldw, kCtxPdp3Ldw};
  for (uint32_t i = 0; i < 4; ++i) {
    state[udw[i]] = static_cast<uint32_t>(directories[i] >> 32);
    state[ldw[i]] = static_cast<uint32_t>(directories[i]);
  }
  return true;
}

}  // namespace intel_gt
}  // namespace hardware
