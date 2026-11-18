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

// Was the GPU POSTed (did the video BIOS's x86 code, or the GPU's own firmware, run the devinit scripts that bring up its memory and
// clocks)? nouveau's devinit preinit checks. Only the detection is here: there is no init-script interpreter yet, so a GPU that was not
// POSTed (normally a secondary card the firmware did not initialise) cannot be brought up, and the driver must leave it alone.
#pragma once
#include <stdint.h>
#include "Hardware/GraphicsProcessingUnit/NVIDIA/Nvkm/NvChip.hpp"
#include "Hardware/GraphicsProcessingUnit/NVIDIA/Nvkm/NvRegisters.hpp"

namespace hardware {
namespace nv {

enum class PostState : uint8_t { Unknown, Posted, NotPosted };

struct PostStatus {
  PostState state = PostState::Unknown;
  const char* reason = "";
  // Turing and later: the FWSEC/devinit completion flags (as tu102_devinit_wait polls them), read once.
  bool devinit_complete_known = false;
  bool devinit_complete = false;
};

class NvDevinit {
 public:
  // gf100_devinit_preinit: bit 1 of 0x2240c is set by devinit and goes back to 0 on suspend.
  static constexpr uint32_t kPostedGf100 = 0x02240c, kPostedBit = 0x2;
  // tu102_devinit_wait: 0x118128 bit 0 and 0x118234 bits 7:0 == 0xff once devinit has finished.
  static constexpr uint32_t kDevinitStatusTu102 = 0x118128, kDevinitDoneTu102 = 0x118234;

  [[nodiscard]] static PostStatus detect(INvRegisters& registers, const Chip& chip);
  static const char* state_name(PostState state);
};

}  // namespace nv
}  // namespace hardware
