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

// The copy engine (nouveau's nvkm/engine/ce; the class methods from NVIDIA's open-gpu-doc cla0b5.h .. clc7b5.h): a DMA engine that
// copies and fills GPU memory and releases a semaphore when it is done. Driven through a channel on the copy engine's runlist; it needs
// no context and no firmware on Kepler..Turing.
//
// Classes: KEPLER_DMA_COPY_A 0xa0b5, MAXWELL_DMA_COPY_A 0xb0b5, PASCAL_DMA_COPY_A 0xc0b5 (GP100), PASCAL_DMA_COPY_B 0xc1b5 (GP10x),
// VOLTA_DMA_COPY_A 0xc3b5, TURING_DMA_COPY_A 0xc5b5; AMPERE_DMA_COPY_A 0xc6b5 (GA100) and AMPERE_DMA_COPY_B 0xc7b5 (GA10x, Ada) are
// named so the chip is recognized, and refused (the channel itself needs GSP there).
#pragma once
#include <stdint.h>
#include "Hardware/GraphicsProcessingUnit/NVIDIA/Nvkm/NvFifo.hpp"

namespace hardware {
namespace nv {

constexpr uint32_t kKeplerDmaCopyA = 0xa0b5, kMaxwellDmaCopyA = 0xb0b5, kPascalDmaCopyA = 0xc0b5, kPascalDmaCopyB = 0xc1b5,
                   kVoltaDmaCopyA = 0xc3b5, kTuringDmaCopyA = 0xc5b5, kAmpereDmaCopyA = 0xc6b5, kAmpereDmaCopyB = 0xc7b5;

// The copy class of `chip` (0 if it has none here).
uint32_t copy_engine_class(const Chip& chip);

// ---- Methods (byte offsets) and LAUNCH_DMA fields; the same in every class listed above. ----
namespace ce {
constexpr uint32_t kSetObject = 0x0000;                 // host method: the class to bind to the subchannel (Kepler+)
constexpr uint32_t kSemaphoreA = 0x0240, kSemaphoreB = 0x0244, kSemaphorePayload = 0x0248;   // address 39:32 (A), 31:0 (B)
constexpr uint32_t kLaunchDma = 0x0300;
constexpr uint32_t kOffsetInUpper = 0x0400, kOffsetInLower = 0x0404, kOffsetOutUpper = 0x0408, kOffsetOutLower = 0x040c;
constexpr uint32_t kPitchIn = 0x0410, kPitchOut = 0x0414, kLineLengthIn = 0x0418, kLineCount = 0x041c;
constexpr uint32_t kRemapConstA = 0x0700, kRemapConstB = 0x0704, kRemapComponents = 0x0708;
// LAUNCH_DMA
constexpr uint32_t kTransferNonPipelined = 2, kFlushEnable = 1u << 2, kSemaphoreReleaseOneWord = 1u << 3;
constexpr uint32_t kSrcPitch = 1u << 7, kDstPitch = 1u << 8, kMultiLine = 1u << 9, kRemapEnable = 1u << 10;
// SET_REMAP_COMPONENTS: DST_X in bits 2:0 (4 = CONST_A), component size 17:16 (3 = four bytes), source / destination component
// counts minus one in 21:20 and 25:24.
constexpr uint32_t kRemapDstXConstA = 4, kRemapComponentFourBytes = 3u << 16;
}  // namespace ce

enum class CeStatus : uint8_t { Ok, Unsupported, NeedsGsp, BadArgument, SubmitFailed, Busy, Timeout };
const char* ce_status_name(CeStatus status);

class NvCopyEngine {
 public:
  static constexpr uint32_t kSubchannel = 4;
  static constexpr uint64_t kMaxLine = 1ull << 30;   // bytes per launch (LINE_LENGTH_IN is 32 bits; keep well inside it)

  NvCopyEngine(NvChannel& channel, INvMemory& memory, const Chip& chip, NvTimeouts timeouts = {})
      : channel_(channel), memory_(memory), class_(copy_engine_class(chip)), needs_gsp_(chip.needs_gsp || chip.family >= Family::Ampere),
        timeouts_(timeouts) {}

  // Binds the class to the subchannel (SET_OBJECT). Needed once per channel before copy/fill.
  [[nodiscard]] CeStatus bind();
  // Copies `bytes` from `src_va` to `dst_va` (virtual addresses in the channel's VMM), then writes `payload` to the 32-bit semaphore
  // at `semaphore_va`.
  [[nodiscard]] CeStatus copy(uint64_t dst_va, uint64_t src_va, uint64_t bytes, uint64_t semaphore_va, uint32_t payload);
  // Fills `bytes` (a multiple of 4, 4-aligned) at `dst_va` with `value`, then releases the semaphore.
  [[nodiscard]] CeStatus fill(uint64_t dst_va, uint64_t bytes, uint32_t value, uint64_t semaphore_va, uint32_t payload);
  // Waits until the semaphore word (`offset` into `semaphore`, as the CPU sees it) holds `payload`; Timeout if the engine never gets
  // there (a hung or missing engine), never an endless loop.
  [[nodiscard]] CeStatus wait(const GpuMemory& semaphore, uint64_t offset, uint32_t payload);

  uint32_t class_id() const { return class_; }

 private:
  CeStatus check() const;
  CeStatus launch(uint64_t dst_va, uint64_t src_va, uint64_t bytes, bool fill, uint32_t value, uint64_t semaphore_va, uint32_t payload);
  NvChannel& channel_;
  INvMemory& memory_;
  uint32_t class_;
  bool needs_gsp_;
  NvTimeouts timeouts_;
  bool bound_ = false;
};

}  // namespace nv
}  // namespace hardware
