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
#include "Hardware/GraphicsProcessingUnit/Intel/IntelRegisterBank.hpp"

namespace hardware {
namespace intel_gt {
namespace gen9 {

// What a Gen9.5 GPU (Kaby Lake, Coffee Lake, Whiskey Lake, Comet Lake graphics; the HD Graphics 630 of the Kaby Lake-G in a Dell XPS 15 9575) needs set before its render
// engine is trusted with work: the memory object control tables (MOCS), and the hardware workarounds the i915 driver applies. Values, registers and conditions are those
// of i915 (gt/intel_mocs.c, gt/intel_workarounds.c, intel_step.c), taking the Kaby Lake and Coffee Lake branches. The driver has one context, so the context workarounds
// i915 loads into every context with MI_LOAD_REGISTER_IMM are written to the registers directly here (the engine saves them with the context).
//
// Not done: the slice/subslice steering (MCR) selector for reads of slice-specific registers (nothing here reads them), and the workarounds of other engines.

enum class Platform : uint8_t { KabyLake, CoffeeLake };

// Kaby Lake graphics are the device IDs 0x5900-0x593F; every other Gen9.5 ID is Coffee Lake (Whiskey Lake and Comet Lake share its workarounds).
Platform platform_for_device(uint16_t device_id);

struct Identity {
  Platform platform = Platform::KabyLake;
  uint8_t revision = 0;   // the PCI revision ID: Kaby Lake steppings before C0 are revisions 0 and 1
};
Identity identity_for(uint16_t device_id, uint8_t revision);

// --- Memory object control (MOCS) ---

constexpr uint32_t kMocsEntries = 64;
constexpr uint32_t kMocsUncached = 0, kMocsPageTable = 1, kMocsCached = 2, kMocsL3Eviction = 63;
// The first control register of each engine's table, and the L3 cache control registers (two entries each, shared by all engines).
constexpr uint32_t kRenderMocsBase = 0xC800, kVideoMocsBase = 0xC900, kL3ccBase = 0xB020, kL3ccRegisters = (kMocsEntries + 1) / 2;

// The control value (cacheability, target cache, LRU management) and the L3 value of entry `index`; the entries the table does not define are the page-table entry's.
uint32_t mocs_control(uint32_t index);
uint16_t mocs_l3cc(uint32_t index);
// Writes the engine's 64 control registers / the 32 L3 registers.
void program_mocs(IRegisterBank& registers, uint32_t engine_mocs_base);
void program_l3cc(IRegisterBank& registers);

// --- Workarounds ---

// The global GT workarounds (read-modify-write of the registers named in i915's gen9/kbl/cfl_gt_workarounds_init). The caller holds the GT power well.
void apply_gt_workarounds(IRegisterBank& registers, const Identity& identity);
// The render engine's context workarounds (masked writes: gen9_ctx_workarounds_init and the kbl/cfl ones). The caller holds the render power well.
void apply_render_workarounds(IRegisterBank& registers, const Identity& identity);

// For tests and reading: how a masked write is built, and the list of context workarounds for a platform.
constexpr uint32_t masked_enable(uint32_t bits) { return (bits << 16) | bits; }
constexpr uint32_t masked_disable(uint32_t bits) { return bits << 16; }

struct MaskedWrite {
  uint32_t reg;
  uint32_t value;   // the whole dword written, mask in the upper half
};
// Fills `out` (at most `capacity`) with the writes apply_render_workarounds makes, in order; returns how many there are.
uint32_t render_workaround_list(const Identity& identity, MaskedWrite* out, uint32_t capacity);

}  // namespace gen9
}  // namespace intel_gt
}  // namespace hardware
