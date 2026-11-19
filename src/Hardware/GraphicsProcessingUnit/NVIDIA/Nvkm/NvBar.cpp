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

#include "Hardware/GraphicsProcessingUnit/NVIDIA/Nvkm/NvBar.hpp"

namespace hardware {
namespace nv {

namespace {

// ---- Registers (BAR0), from memory of nouveau's bar/gf100.c, gm107.c, tu102.c and g84.c. ----
//   Fermi..Volta   0x001704 BAR1 instance: bit 31 valid, bits 29:28 target (VRAM 0), 27:0 address >> 12            confident
//                  0x001714 BAR2 instance, same layout (bit 30: "bar2_halve", not used)                              confident
//                  0x001710 bind status: bits 1:0 BAR1 busy, 3:2 BAR2 busy (waited on from GM107)                    fairly sure
//   Turing, GA10x  0xb80f40 BAR1 instance, 0xb80f48 BAR2 instance, 0xb80f50 bind status (same bits; ga102 = tu102_bar) fairly sure
//   all (G84+)     0x070000 = 1 flushes the BAR write path; bit 1 reads 1 while it runs                                confident
constexpr uint32_t kGfBar1Inst = 0x001704, kGfBar2Inst = 0x001714, kGfBarStatus = 0x001710;
constexpr uint32_t kTuBar1Inst = 0xb80f40, kTuBar2Inst = 0xb80f48, kTuBarStatus = 0xb80f50;
constexpr uint32_t kBarFlush = 0x070000;

}  // namespace

NvBar::NvBar(INvRegisters& regs, const Chip& chip, INvMemory& tables, NvTlb& tlb, INvLock& bar1_lock, INvLock& bar2_lock, NvTimeouts timeouts)
    : regs_(regs), chip_(chip), tables_(tables), timeouts_(timeouts), bar1_(tables, chip, bar1_lock, &tlb), bar2_(tables, chip, bar2_lock, &tlb) {}

NvBar::~NvBar() {
  tables_.release(bar1_inst_);
  tables_.release(bar2_inst_);
}

bool NvBar::poll_clear(uint32_t offset, uint32_t mask) {
  for (uint32_t i = 0; i <= timeouts_.register_us; ++i) {
    if (!(regs_.read32(offset) & mask)) return true;
    regs_.delay_us(1);
  }
  return false;
}

BarStatus NvBar::init(uint64_t bar1_bytes, uint64_t bar2_bytes, uint64_t keep_linear_bytes) {
  if (chip_.family < Family::Fermi || chip_.family > Family::Ampere) return BarStatus::Unsupported;
  if (ready_ || bar1_bytes < (1u << 20) || bar2_bytes < (1u << 20) || (bar1_bytes & (bar1_bytes - 1)) || (bar2_bytes & (bar2_bytes - 1)))
    return BarStatus::BadSize;
  keep_linear_bytes = (keep_linear_bytes + 0xFFFF) & ~uint64_t{0xFFFF};
  if (keep_linear_bytes >= bar1_bytes) return BarStatus::BadSize;

  bar2_inst_ = tables_.allocate(MemoryTarget::Vram, 0x1000, 0x1000);
  bar1_inst_ = tables_.allocate(MemoryTarget::Vram, 0x1000, 0x1000);
  if (!bar1_inst_.valid() || !bar2_inst_.valid()) return BarStatus::NoMemory;
  if (!bar2_.init(0, bar2_bytes, true) || !bar1_.init(keep_linear_bytes ? keep_linear_bytes : 0, bar1_bytes, true)) return BarStatus::NoMemory;
  if (keep_linear_bytes) {
    GpuMemory linear;
    linear.gpu_address = 0;
    linear.bytes = keep_linear_bytes;
    linear.target = MemoryTarget::Vram;
    if (!bar1_.map(0, linear, 0, keep_linear_bytes)) return BarStatus::NoMemory;
  }
  if (!bar2_.join(bar2_inst_) || !bar1_.join(bar1_inst_)) return BarStatus::NoMemory;
  tables_.flush(bar2_inst_);

  const bool turing = chip_.family >= Family::Turing;
  const bool wait = chip_.family >= Family::Maxwell;   // gm107_bar_bar*_wait
  const uint32_t status = turing ? kTuBarStatus : kGfBarStatus;
  regs_.write32(turing ? kTuBar2Inst : kGfBar2Inst, 0x80000000u | static_cast<uint32_t>(bar2_inst_.gpu_address >> 12));
  if (wait && !poll_clear(status, 0x0000000C)) return BarStatus::Timeout;
  regs_.write32(turing ? kTuBar1Inst : kGfBar1Inst, 0x80000000u | static_cast<uint32_t>(bar1_inst_.gpu_address >> 12));
  if (wait && !poll_clear(status, 0x00000003)) return BarStatus::Timeout;
  ready_ = true;
  return BarStatus::Ok;
}

void NvBar::fini() {
  if (!ready_) return;
  const bool turing = chip_.family >= Family::Turing;
  regs_.mask(turing ? kTuBar1Inst : kGfBar1Inst, 0x80000000u, 0);
  regs_.mask(turing ? kTuBar2Inst : kGfBar2Inst, 0x80000000u, 0);
  ready_ = false;
}

bool NvBar::flush_writes() {
  regs_.write32(kBarFlush, 1);
  return poll_clear(kBarFlush, 2);
}

}  // namespace nv
}  // namespace hardware
