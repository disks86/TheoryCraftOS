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

#include "Hardware/GraphicsProcessingUnit/NVIDIA/Nvkm/NvCe.hpp"

namespace hardware {
namespace nv {

// Classes per chip (nouveau's engine/device/base.c ce entries and engine/ce/*.c):
//   GK1xx a0b5; GM1xx/GM2xx b0b5; GP100 c0b5, GP10x c1b5; GV100 c3b5; TU1xx c5b5; GA100 c6b5; GA10x and AD10x c7b5.   confident
uint32_t copy_engine_class(const Chip& chip) {
  switch (chip.family) {
    case Family::Kepler: return kKeplerDmaCopyA;
    case Family::Maxwell: return kMaxwellDmaCopyA;
    case Family::Pascal: return chip.chipset == 0x130 ? kPascalDmaCopyA : kPascalDmaCopyB;
    case Family::Volta: return kVoltaDmaCopyA;
    case Family::Turing: return kTuringDmaCopyA;
    case Family::Ampere: return chip.chipset == 0x170 ? kAmpereDmaCopyA : kAmpereDmaCopyB;
    case Family::Ada: return kAmpereDmaCopyB;
    default: return 0;
  }
}

const char* ce_status_name(CeStatus s) {
  switch (s) {
    case CeStatus::Ok: return "ok";
    case CeStatus::Unsupported: return "unsupported";
    case CeStatus::NeedsGsp: return "needs GSP firmware";
    case CeStatus::BadArgument: return "bad argument";
    case CeStatus::SubmitFailed: return "submit failed";
    case CeStatus::Busy: return "busy";
    case CeStatus::Timeout: return "timeout";
  }
  return "?";
}

CeStatus NvCopyEngine::check() const {
  if (needs_gsp_) return CeStatus::NeedsGsp;
  if (class_ == 0) return CeStatus::Unsupported;
  return CeStatus::Ok;
}

static CeStatus from_fifo(FifoStatus s) {
  switch (s) {
    case FifoStatus::Ok: return CeStatus::Ok;
    case FifoStatus::Busy: return CeStatus::Busy;
    case FifoStatus::Timeout: return CeStatus::Timeout;
    default: return CeStatus::SubmitFailed;
  }
}

CeStatus NvCopyEngine::bind() {
  if (const CeStatus s = check(); s != CeStatus::Ok) return s;
  PushBuilder push;
  push.method1(kSubchannel, ce::kSetObject, class_);
  const CeStatus s = from_fifo(channel_.submit(push.words()));
  bound_ = s == CeStatus::Ok;
  return s;
}

CeStatus NvCopyEngine::launch(uint64_t dst_va, uint64_t src_va, uint64_t bytes, bool fill, uint32_t value, uint64_t semaphore_va,
                              uint32_t payload) {
  if (const CeStatus s = check(); s != CeStatus::Ok) return s;
  if (!bound_) return CeStatus::BadArgument;
  if (bytes == 0 || semaphore_va % 4 || dst_va > ~uint64_t{0} - bytes || (!fill && src_va > ~uint64_t{0} - bytes)) return CeStatus::BadArgument;
  if (fill && (bytes % 4 || dst_va % 4)) return CeStatus::BadArgument;
  uint64_t done = 0;
  while (done < bytes) {
    const uint64_t chunk = bytes - done < kMaxLine ? bytes - done : kMaxLine;
    const bool last = done + chunk == bytes;
    const uint64_t src = src_va + done, dst = dst_va + done;
    PushBuilder push;
    uint32_t flags = ce::kTransferNonPipelined | ce::kSrcPitch | ce::kDstPitch;
    uint32_t line = static_cast<uint32_t>(chunk);
    if (fill) {
      const uint32_t remap[1] = {value};
      push.method(kSubchannel, ce::kRemapConstA, ArrayView<const uint32_t>(remap));
      push.method1(kSubchannel, ce::kRemapComponents, ce::kRemapDstXConstA | ce::kRemapComponentFourBytes);  // 1 source, 1 destination component
      flags |= ce::kRemapEnable;
      line = static_cast<uint32_t>(chunk / 4);   // with remapping the line length counts components
    }
    const uint32_t offsets[8] = {
        static_cast<uint32_t>(src >> 32), static_cast<uint32_t>(src), static_cast<uint32_t>(dst >> 32), static_cast<uint32_t>(dst),
        static_cast<uint32_t>(chunk), static_cast<uint32_t>(chunk), line, 1,   // PITCH_IN, PITCH_OUT (bytes), LINE_LENGTH_IN, LINE_COUNT
    };
    push.method(kSubchannel, ce::kOffsetInUpper, ArrayView<const uint32_t>(offsets));
    if (last) {
      const uint32_t sem[3] = {static_cast<uint32_t>(semaphore_va >> 32), static_cast<uint32_t>(semaphore_va), payload};
      push.method(kSubchannel, ce::kSemaphoreA, ArrayView<const uint32_t>(sem));
      flags |= ce::kFlushEnable | ce::kSemaphoreReleaseOneWord;
    }
    push.method1(kSubchannel, ce::kLaunchDma, flags);
    if (push.overflow()) return CeStatus::BadArgument;
    if (const CeStatus s = from_fifo(channel_.submit(push.words())); s != CeStatus::Ok) return s;
    done += chunk;
  }
  return CeStatus::Ok;
}

CeStatus NvCopyEngine::copy(uint64_t dst_va, uint64_t src_va, uint64_t bytes, uint64_t semaphore_va, uint32_t payload) {
  return launch(dst_va, src_va, bytes, false, 0, semaphore_va, payload);
}

CeStatus NvCopyEngine::fill(uint64_t dst_va, uint64_t bytes, uint32_t value, uint64_t semaphore_va, uint32_t payload) {
  return launch(dst_va, 0, bytes, true, value, semaphore_va, payload);
}

CeStatus NvCopyEngine::wait(const GpuMemory& semaphore, uint64_t offset, uint32_t payload) {
  for (uint32_t i = 0; i <= timeouts_.fence_us; ++i) {
    uint32_t v = 0;
    if (!memory_.read32(semaphore, offset, &v)) return CeStatus::BadArgument;
    if (v == payload) return CeStatus::Ok;
    channel_.pause_us(1);
  }
  return CeStatus::Timeout;
}

}  // namespace nv
}  // namespace hardware
