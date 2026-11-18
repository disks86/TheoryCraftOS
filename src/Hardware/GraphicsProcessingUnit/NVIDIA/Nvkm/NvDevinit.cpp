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

// Follows nouveau (MIT license): nvkm/subdev/devinit/gf100.c (gf100_devinit_preinit), tu102.c (tu102_devinit_wait), nv50.c and nv04.c
// (the VGA CRTC heuristics this does not do yet).
#include "Hardware/GraphicsProcessingUnit/NVIDIA/Nvkm/NvDevinit.hpp"

namespace hardware {
namespace nv {

PostStatus NvDevinit::detect(INvRegisters& r, const Chip& chip) {
  PostStatus status;
  if (chip.family == Family::Unknown) {
    status.reason = "unknown chip";
    return status;
  }
  if (chip.tegra) {
    status.state = PostState::Posted;
    status.reason = "a Tegra GPU: the SoC's boot firmware brings it up";
    return status;
  }
  if (chip.card_type < 0xc0) {
    // nv50_devinit_preinit reads VGA CR00/CR1A, nv04_devinit_preinit the CRTC's horizontal total: both need 8-bit writes of the CRTC index
    // port (0x6013d4), which INvRegisters (32-bit only) cannot do without clobbering the data port next to it.
    status.reason = "not detectable yet before Fermi (needs 8-bit VGA CRTC access)";
    return status;
  }
  const uint32_t posted = r.read32(kPostedGf100);
  if (posted == 0xFFFFFFFFu) {
    status.reason = "the GPU does not answer (BAR0 reads all ones)";
    return status;
  }
  status.state = (posted & kPostedBit) ? PostState::Posted : PostState::NotPosted;
  status.reason = status.state == PostState::Posted ? "devinit has run (0x2240c bit 1)"
                                                    : "not POSTed (0x2240c bit 1 clear): no init-script interpreter yet, the GPU cannot be brought up";
  if (chip.card_type >= 0x160) {
    status.devinit_complete_known = true;
    status.devinit_complete = (r.read32(kDevinitStatusTu102) & 1) && (r.read32(kDevinitDoneTu102) & 0xff) == 0xff;
  }
  return status;
}

const char* NvDevinit::state_name(PostState state) {
  switch (state) {
    case PostState::Unknown: return "unknown";
    case PostState::Posted: return "POSTed";
    case PostState::NotPosted: return "not POSTed";
  }
  return "unknown";
}

}  // namespace nv
}  // namespace hardware
