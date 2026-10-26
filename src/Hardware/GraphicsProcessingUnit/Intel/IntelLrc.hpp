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
#include "Utility/ArrayView.hpp"

namespace hardware {
namespace intel_gt {

// The logical ring context ("LRC") of Gen8 to Gen10 video engines: the image the GPU loads its engine registers from when it starts a context and saves them to when
// it stops. A context is two pages: the per-process hardware status page (page 0) and the register state (page 1). Layouts follow the i915 driver
// (gt/intel_lrc.c, gt/intel_lrc_reg.h, gt/intel_lrc.h).

constexpr uint32_t kLrcPages = 2;              // video engines of Gen8+ (GEN8_LR_CONTEXT_OTHER_SIZE)
constexpr uint32_t kLrcRenderPages = 22;       // the render engine of Gen9 (GEN9_LR_CONTEXT_RENDER_SIZE): the engine saves its whole 3D state there
constexpr uint32_t kLrcStatePage = 1;          // the register state starts one page in
constexpr uint32_t kLrcStateDwords = 1024;     // one page

// Indices (in dwords, within the register state page) of the values the driver sets: the register's value follows its offset.
constexpr uint32_t kCtxContextControl = 0x02 + 1;
constexpr uint32_t kCtxRingHead = 0x04 + 1;
constexpr uint32_t kCtxRingTail = 0x06 + 1;
constexpr uint32_t kCtxRingStart = 0x08 + 1;
constexpr uint32_t kCtxRingCtl = 0x0a + 1;
constexpr uint32_t kCtxPdp3Udw = 0x24 + 1, kCtxPdp3Ldw = 0x26 + 1;
constexpr uint32_t kCtxPdp2Udw = 0x28 + 1, kCtxPdp2Ldw = 0x2a + 1;
constexpr uint32_t kCtxPdp1Udw = 0x2c + 1, kCtxPdp1Ldw = 0x2e + 1;
constexpr uint32_t kCtxPdp0Udw = 0x30 + 1, kCtxPdp0Ldw = 0x32 + 1;
constexpr uint32_t kCtxRingMiMode = 0x54 + 1;  // GEN9_CTX_RING_MI_MODE

// MMIO offset of the first video engine (VCS0) on Gen8 to Gen10, and of the first video box (VD0) on Gen11 and later.
constexpr uint32_t kVcs0MmioBase = 0x12000;
constexpr uint32_t kVd0MmioBase = 0x1C0000;
// MMIO offset of the render engine (RCS0) on Gen8 to Gen11.
constexpr uint32_t kRcs0MmioBase = 0x2000;

// Which generation's layouts a context follows.
enum class Generation : uint8_t { Gen9, Gen12 };

// Fills the register state page for a Gen9 video engine (the context starts with every register at zero except the ones listed in the layout table), then sets
// the context control and the engine's MI mode (ring not stopped). `mmio_base` is the engine's register base; `inhibit_restore` is true for a context that has
// never run (there is no saved engine state to restore). `state` must be exactly kLrcStateDwords long.
bool lrc_init_gen9_xcs(ArrayView<uint32_t> state, uint32_t mmio_base, bool inhibit_restore);
// The same for the Gen9 render engine (gen9_rcs_offsets): the register list is the video engine's, but the third load (the render power/clock state register,
// not the video engine's) is not posted.
bool lrc_init_gen9_rcs(ArrayView<uint32_t> state, uint32_t mmio_base, bool inhibit_restore);

// The same for a Gen12 (Xe-LP) video engine: the shorter register list of gen12_xcs_offsets (the load commands also set the "from the command streamer's MMIO"
// bit), the ring MI mode and batch buffer offset at the places Gen12 has them. As the i915 driver does (gt/intel_lrc.c).
bool lrc_init_gen12_xcs(ArrayView<uint32_t> state, uint32_t mmio_base, bool inhibit_restore);
// The top of a four-level address space (its page-map level 4 page) goes where Gen9 has the first page directory pointer.
bool lrc_set_pml4(ArrayView<uint32_t> state, uint64_t pml4);

// The ring the context executes from: its graphics address (a GGTT address), its size in bytes (a multiple of 4096) and the head and tail offsets.
bool lrc_set_ring(ArrayView<uint32_t> state, uint32_t ring_ggtt_address, uint32_t ring_bytes, uint32_t head, uint32_t tail);
// Moves only the tail (new commands were written into the ring).
bool lrc_set_tail(ArrayView<uint32_t> state, uint32_t tail);
// The four page directories of the context's 32-bit address space.
bool lrc_set_page_directories(ArrayView<uint32_t> state, const uint64_t directories[4]);

// The 64-bit context descriptor that goes to the execlist port: the graphics address of the context's first page, the "legacy 32-bit addressing" mode, valid and
// privileged, and the context ID in the upper half.
constexpr uint64_t lrc_descriptor(uint32_t context_ggtt_address, uint32_t context_id) {
  constexpr uint64_t kValid = 1u << 0, kPrivilege = 1u << 8, kAddressingShift = 3, kLegacy32 = 1;
  return (uint64_t{context_ggtt_address} & 0xFFFFF000u) | (kLegacy32 << kAddressingShift) | kValid | kPrivilege | (uint64_t{context_id} << 32);
}
constexpr uint64_t kDescriptorForceRestore = 1u << 2;

// Gen12: the addressing mode is the 64-bit (four-level) legacy mode, and the upper half holds the context ID (11 bits at bit 37), the engine instance (bit 48) and the
// engine class (bit 61; 1 is video decode). Context ID 0x7FF means "idle" to the hardware.
constexpr uint64_t lrc_descriptor_gen12(uint32_t context_ggtt_address, uint32_t context_id, uint32_t engine_instance, uint32_t engine_class) {
  constexpr uint64_t kValid = 1u << 0, kPrivilege = 1u << 8, kAddressingShift = 3, kLegacy64 = 3;
  return (uint64_t{context_ggtt_address} & 0xFFFFF000u) | (kLegacy64 << kAddressingShift) | kValid | kPrivilege | (uint64_t{context_id & 0x7FF} << 37) |
         (uint64_t{engine_instance & 0x3F} << 48) | (uint64_t{engine_class & 7} << 61);
}
constexpr uint32_t kVideoDecodeClass = 1;

}  // namespace intel_gt
}  // namespace hardware
