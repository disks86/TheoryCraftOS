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
    case ForcewakeDomain::MediaVdbox0: return 0xA540;   // FORCEWAKE_MEDIA_VDBOX_GEN11(0)
  }
  return 0;
}

uint32_t Forcewake::ack_register(ForcewakeDomain domain) {
  switch (domain) {
    case ForcewakeDomain::Gt: return 0x130044;    // FORCEWAKE_ACK_GT_GEN9
    case ForcewakeDomain::Render: return 0x0D84;  // FORCEWAKE_ACK_RENDER_GEN9
    case ForcewakeDomain::Media: return 0x0D88;   // FORCEWAKE_ACK_MEDIA_GEN9
    case ForcewakeDomain::MediaVdbox0: return 0x0D50;   // FORCEWAKE_ACK_MEDIA_VDBOX_GEN11(0)
  }
  return 0;
}

namespace {
class StateLock {
 public:
  explicit StateLock(ForcewakeState& s) : state_(s) { while (__atomic_exchange_n(&state_.lock, 1u, __ATOMIC_ACQUIRE)) __builtin_ia32_pause(); }
  ~StateLock() { __atomic_store_n(&state_.lock, 0u, __ATOMIC_RELEASE); }
  StateLock(const StateLock&) = delete;
  StateLock& operator=(const StateLock&) = delete;

 private:
  ForcewakeState& state_;
};
}  // namespace

// Each caller adds one to the well's count; the one that makes it 1 wakes the well and waits (under the lock, so the next caller finds the well awake), and the one that
// brings it back to 0 lets the well sleep.
bool Forcewake::acquire(IRegisterBank& registers, ForcewakeDomain domain, uint32_t attempts) {
  const uint32_t request = request_register(domain), ack = ack_register(domain);
  ForcewakeState& state = registers.forcewake_state();
  StateLock lock(state);
  uint32_t& count = state.counts[static_cast<uint32_t>(domain)];
  if (count != 0) {
    ++count;
    return true;
  }
  registers.write32(request, kMaskedEnable);
  for (uint32_t i = 0; i < attempts; ++i) {
    const uint32_t v = registers.read32(ack);
    if (v != 0xFFFFFFFFu && (v & kKernelBit)) {  // all ones means the register is not reachable at all
      count = 1;
      return true;
    }
  }
  registers.write32(request, kMaskedDisable);
  return false;
}

void Forcewake::release(IRegisterBank& registers, ForcewakeDomain domain) {
  ForcewakeState& state = registers.forcewake_state();
  StateLock lock(state);
  uint32_t& count = state.counts[static_cast<uint32_t>(domain)];
  if (count == 0) return;   // (not held: nothing to give back)
  if (--count == 0) registers.write32(request_register(domain), kMaskedDisable);
}

}  // namespace intel_gt
}  // namespace hardware
