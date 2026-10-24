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

#include "Hardware/GraphicsProcessingUnit/Intel/IntelForcewake.hpp"

namespace hardware {
namespace intel_gt {

namespace {
constexpr uint32_t kKernelBit = 1;
constexpr uint32_t kMaskedEnable = (kKernelBit << 16) | kKernelBit;
constexpr uint32_t kMaskedDisable = kKernelBit << 16;
}  // namespace

uint32_t Forcewake::request_register(ForcewakeDomain domain) {
  switch (domain) {
    case ForcewakeDomain::Gt: return 0xA188;      // FORCEWAKE_GT_GEN9
    case ForcewakeDomain::Render: return 0xA278;  // FORCEWAKE_RENDER_GEN9
    case ForcewakeDomain::Media: return 0xA270;   // FORCEWAKE_MEDIA_GEN9
  }
  return 0;
}

uint32_t Forcewake::ack_register(ForcewakeDomain domain) {
  switch (domain) {
    case ForcewakeDomain::Gt: return 0x130044;    // FORCEWAKE_ACK_GT_GEN9
    case ForcewakeDomain::Render: return 0x0D84;  // FORCEWAKE_ACK_RENDER_GEN9
    case ForcewakeDomain::Media: return 0x0D88;   // FORCEWAKE_ACK_MEDIA_GEN9
  }
  return 0;
}

bool Forcewake::acquire(IRegisterBank& registers, ForcewakeDomain domain, uint32_t attempts) {
  const uint32_t request = request_register(domain), ack = ack_register(domain);
  registers.write32(request, kMaskedEnable);
  for (uint32_t i = 0; i < attempts; ++i) {
    const uint32_t v = registers.read32(ack);
    if (v != 0xFFFFFFFFu && (v & kKernelBit)) return true;  // all ones means the register is not reachable at all
  }
  registers.write32(request, kMaskedDisable);
  return false;
}

void Forcewake::release(IRegisterBank& registers, ForcewakeDomain domain) { registers.write32(request_register(domain), kMaskedDisable); }

}  // namespace intel_gt
}  // namespace hardware
