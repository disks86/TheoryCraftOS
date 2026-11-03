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
#include "Jit/CodeMemory.hpp"

namespace jit {

// IInstructionCacheSync through the scheduler's run_on_every_core: an inter-processor interrupt to every other core, each of which runs the architecture's
// instruction-stream synchronisation (FENCE.I on RISC-V, ISB on AArch64, a serialising CPUID on x86), and the calling core does the same. Thread context
// only; before the scheduler runs it synchronises just this core. The equivalent of the SBI remote FENCE.I (RFENCE extension) on RISC-V, done with the
// kernel's own cross-core call so that it needs neither firmware support nor a mapping from cores to hart ids.
class CrossCoreInstructionSync final : public IInstructionCacheSync {
 public:
  bool sync(ConstByteView code) override;
};

}  // namespace jit
