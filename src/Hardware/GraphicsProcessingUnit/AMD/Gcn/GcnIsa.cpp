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


#include "Hardware/GraphicsProcessingUnit/AMD/Gcn/GcnIsa.hpp"

namespace gcn {

Src imm_int(int32_t value) {
  if (value >= 0 && value <= 64) return Src{static_cast<uint16_t>(128 + value), false, 0};
  if (value >= -16 && value <= -1) return Src{static_cast<uint16_t>(192 - value), false, 0};
  return Src{kCodeLiteral, true, static_cast<uint32_t>(value)};
}

Src imm_bits(uint32_t bits) {
  switch (bits) {
    case 0x3F000000u: return Src{240, false, 0};       // 0.5
    case 0xBF000000u: return Src{241, false, 0};       // -0.5
    case 0x3F800000u: return Src{242, false, 0};       // 1.0
    case 0xBF800000u: return Src{243, false, 0};       // -1.0
    case 0x40000000u: return Src{244, false, 0};       // 2.0
    case 0xC0000000u: return Src{245, false, 0};       // -2.0
    case 0x40800000u: return Src{246, false, 0};       // 4.0
    case 0xC0800000u: return Src{247, false, 0};       // -4.0
    default: break;
  }
  if (bits <= 64 || bits >= 0xFFFFFFF0u) return imm_int(static_cast<int32_t>(bits));
  return Src{kCodeLiteral, true, bits};
}

Src imm_float(float value) {
  uint32_t bits = 0;
  static_assert(sizeof bits == sizeof value);
  __builtin_memcpy(&bits, &value, sizeof bits);
  return imm_bits(bits);
}

void Code::put(uint32_t word) {
  if (!words_.push_back(word)) ok_ = false;
}

void Code::src_literal(const Src& s) {
  if (s.literal) put(s.value);
}

void Code::sop1(Sop1 op, uint32_t sdst, Src ssrc0) {
  if (sdst > 127 || ssrc0.code > 255) bad();
  put(0xBE800000u | (sdst << 16) | (static_cast<uint32_t>(op) << 8) | (ssrc0.code & 0xFF));
  src_literal(ssrc0);
}

void Code::sop2(Sop2 op, uint32_t sdst, Src ssrc0, Src ssrc1) {
  if (sdst > 127 || ssrc0.code > 255 || ssrc1.code > 255 || (ssrc0.literal && ssrc1.literal && ssrc0.value != ssrc1.value)) bad();
  put(0x80000000u | (static_cast<uint32_t>(op) << 23) | (sdst << 16) | ((ssrc1.code & 0xFF) << 8) | (ssrc0.code & 0xFF));
  src_literal(ssrc0.literal ? ssrc0 : ssrc1);
}

void Code::sopk(Sopk op, uint32_t sdst, int32_t simm16) {
  if (sdst > 127 || simm16 < -32768 || simm16 > 65535) bad();
  put(0xB0000000u | (static_cast<uint32_t>(op) << 23) | (sdst << 16) | (static_cast<uint32_t>(simm16) & 0xFFFF));
}

void Code::sopc(Sopc op, Src ssrc0, Src ssrc1) {
  if (ssrc0.code > 255 || ssrc1.code > 255 || (ssrc0.literal && ssrc1.literal && ssrc0.value != ssrc1.value)) bad();
  put(0xBF000000u | (static_cast<uint32_t>(op) << 16) | ((ssrc1.code & 0xFF) << 8) | (ssrc0.code & 0xFF));
  src_literal(ssrc0.literal ? ssrc0 : ssrc1);
}

void Code::sopp(Sopp op, int32_t simm16) {
  if (simm16 < -32768 || simm16 > 65535) bad();
  put(0xBF800000u | (static_cast<uint32_t>(op) << 16) | (static_cast<uint32_t>(simm16) & 0xFFFF));
}

void Code::waitcnt(uint32_t vmcnt, uint32_t expcnt, uint32_t lgkmcnt) {
  if (vmcnt > 15 || expcnt > 7 || lgkmcnt > 15) bad();       // (GFX8: four bits of vm count)
  sopp(Sopp::s_waitcnt, static_cast<int32_t>((vmcnt & 0xF) | ((expcnt & 7) << 4) | ((lgkmcnt & 0xF) << 8)));
}

uint32_t Code::branch(Sopp op) {
  const uint32_t at = size();
  sopp(op, 0);
  return at;
}

void Code::patch_branch(uint32_t at, uint32_t target) {
  if (at >= words_.count()) {
    bad();
    return;
  }
  const int64_t offset = static_cast<int64_t>(target) - (static_cast<int64_t>(at) + 1);
  if (offset < -32768 || offset > 32767) {
    bad();
    return;
  }
  words_[at] = (words_[at] & 0xFFFF0000u) | (static_cast<uint32_t>(offset) & 0xFFFF);
}

void Code::smem(Smem op, uint32_t sdata, uint32_t sbase, uint32_t offset_bytes) {
  if (sdata > 127 || sbase > 127 || (sbase & 1) || offset_bytes >= (1u << 20)) bad();
  put(0xC0000000u | (static_cast<uint32_t>(op) << 18) | (1u << 17) | (sdata << 6) | ((sbase >> 1) & 0x3F));
  put(offset_bytes);
}

void Code::vop1(Vop1 op, uint32_t vdst, Src src0) {
  if (vdst > 255 || src0.code > 511) bad();
  put(0x7E000000u | (vdst << 17) | (static_cast<uint32_t>(op) << 9) | (src0.code & 0x1FF));
  src_literal(src0);
}

void Code::vop2(Vop2 op, uint32_t vdst, Src src0, uint32_t vsrc1) {
  if (vdst > 255 || vsrc1 > 255 || src0.code > 511) bad();
  put((static_cast<uint32_t>(op) << 25) | (vdst << 17) | (vsrc1 << 9) | (src0.code & 0x1FF));
  src_literal(src0);
}

void Code::vop2_literal(Vop2 op, uint32_t vdst, Src src0, uint32_t vsrc1, uint32_t literal) {
  if (vdst > 255 || vsrc1 > 255 || src0.code > 511 || src0.literal) bad();
  put((static_cast<uint32_t>(op) << 25) | (vdst << 17) | (vsrc1 << 9) | (src0.code & 0x1FF));
  put(literal);
}

void Code::vopc(Vopc op, Src src0, uint32_t vsrc1) {
  if (vsrc1 > 255 || src0.code > 511) bad();
  put(0x7C000000u | (static_cast<uint32_t>(op) << 17) | (vsrc1 << 9) | (src0.code & 0x1FF));
  src_literal(src0);
}

void Code::vop3(Vop3 op, uint32_t vdst, Src src0, Src src1, Src src2, Mods mods) {
  if (vdst > 255 || src0.literal || src1.literal || src2.literal || src0.code > 511 || src1.code > 511 || src2.code > 511 || mods.neg > 7 || mods.abs > 7 || mods.omod > 3) bad();
  put(0xD0000000u | (static_cast<uint32_t>(op) << 16) | (mods.clamp ? 1u << 15 : 0u) | (static_cast<uint32_t>(mods.abs & 7) << 8) | vdst);
  put((src0.code & 0x1FF) | ((src1.code & 0x1FFu) << 9) | ((src2.code & 0x1FFu) << 18) | (static_cast<uint32_t>(mods.omod & 3) << 27) | (static_cast<uint32_t>(mods.neg & 7) << 29));
}

namespace {
// the VOP3 opcode of a VOP1 / VOP2 / VOPC instruction
constexpr uint32_t kVop3OfVop2 = 0x100, kVop3OfVop1 = 0x140;
}  // namespace

void Code::vop3_of(Vop1 op, uint32_t vdst, Src src0, Mods mods) {
  vop3(static_cast<Vop3>(kVop3OfVop1 + static_cast<uint32_t>(op)), vdst, src0, Src{}, Src{}, mods);
}

void Code::vop3_of(Vop2 op, uint32_t vdst, Src src0, Src src1, Mods mods) {
  vop3(static_cast<Vop3>(kVop3OfVop2 + static_cast<uint32_t>(op)), vdst, src0, src1, Src{}, mods);
}

void Code::vop3_of(Vop2 op, uint32_t vdst, Src src0, Src src1, Src carry_or_select, Mods mods) {
  vop3(static_cast<Vop3>(kVop3OfVop2 + static_cast<uint32_t>(op)), vdst, src0, src1, carry_or_select, mods);
}

void Code::vop3_of(Vopc op, uint32_t sdst, Src src0, Src src1, Mods mods) {
  vop3(static_cast<Vop3>(static_cast<uint32_t>(op)), sdst, src0, src1, Src{}, mods);
}

void Code::vintrp(Vintrp op, uint32_t vdst, uint32_t vsrc, uint32_t attr, uint32_t chan) {
  if (vdst > 255 || vsrc > 255 || attr > 63 || chan > 3) bad();
  put(0xD4000000u | (vdst << 18) | (static_cast<uint32_t>(op) << 16) | (attr << 10) | (chan << 8) | vsrc);
}

void Code::exp(uint32_t target, uint32_t enable, uint32_t vsrc0, uint32_t vsrc1, uint32_t vsrc2, uint32_t vsrc3, bool done, bool valid_mask, bool compressed) {
  if (target > 63 || enable > 15 || vsrc0 > 255 || vsrc1 > 255 || vsrc2 > 255 || vsrc3 > 255) bad();
  put(0xC4000000u | (valid_mask ? 1u << 12 : 0u) | (done ? 1u << 11 : 0u) | (compressed ? 1u << 10 : 0u) | (target << 4) | enable);
  put(vsrc0 | (vsrc1 << 8) | (vsrc2 << 16) | (vsrc3 << 24));
}

void Code::mubuf(Mubuf op, uint32_t vdata, uint32_t vaddr, uint32_t srsrc, Src soffset, BufferMode mode) {
  if (vdata > 255 || vaddr > 255 || (srsrc & 3) || srsrc > 100 || soffset.literal || soffset.code > 255 || mode.offset > 4095) bad();
  put(0xE0000000u | (static_cast<uint32_t>(op) << 18) | (mode.slc ? 1u << 17 : 0u) | (mode.glc ? 1u << 14 : 0u) | (mode.idxen ? 1u << 13 : 0u) | (mode.offen ? 1u << 12 : 0u) | mode.offset);
  put(vaddr | (vdata << 8) | ((srsrc >> 2) << 16) | ((soffset.code & 0xFFu) << 24));
}

void Code::mimg(Mimg op, uint32_t vdata, uint32_t vaddr, uint32_t srsrc, uint32_t ssamp, uint32_t dmask, bool unorm) {
  if (vdata > 255 || vaddr > 255 || (srsrc & 3) || (ssamp & 3) || srsrc > 96 || ssamp > 100 || dmask > 15) bad();
  put(0xF0000000u | (static_cast<uint32_t>(op) << 18) | (unorm ? 1u << 12 : 0u) | (dmask << 8));
  put(vaddr | (vdata << 8) | ((srsrc >> 2) << 16) | ((ssamp >> 2) << 21));
}

uint32_t instruction_length(ArrayView<const uint32_t> words) {
  if (words.size() == 0) return 0;
  const uint32_t w = words[0];
  const auto literal = [&](uint32_t src, uint32_t base) { return src == kCodeLiteral ? base + 1 : base; };
  if ((w & 0xFF800000u) == 0xBE800000u) return literal(w & 0xFF, 1);                 // SOP1
  if ((w & 0xFF800000u) == 0xBF000000u) return ((w & 0xFF) == kCodeLiteral || ((w >> 8) & 0xFF) == kCodeLiteral) ? 2 : 1;       // SOPC
  if ((w & 0xFF800000u) == 0xBF800000u) return 1;                                      // SOPP
  if ((w & 0xF0000000u) == 0xB0000000u) return 1;                                      // SOPK
  if ((w & 0xC0000000u) == 0x80000000u) return ((w & 0xFF) == kCodeLiteral || ((w >> 8) & 0xFF) == kCodeLiteral) ? 2 : 1;       // SOP2
  if ((w & 0xFC000000u) == 0xC0000000u) return 2;                                      // SMEM
  if ((w & 0xFC000000u) == 0xC4000000u) return 2;                                      // EXP
  if ((w & 0xFC000000u) == 0xD4000000u) return 1;                                      // VINTRP
  if ((w & 0xFC000000u) == 0xD0000000u) return 2;                                      // VOP3
  if ((w & 0xFC000000u) == 0xE0000000u) return 2;                                      // MUBUF
  if ((w & 0xFC000000u) == 0xF0000000u) return 2;                                      // MIMG
  if ((w & 0xFE000000u) == 0x7E000000u) return (w & 0x1FF) == kCodeLiteral ? 2 : 1;   // VOP1
  if ((w & 0xFE000000u) == 0x7C000000u) return (w & 0x1FF) == kCodeLiteral ? 2 : 1;   // VOPC
  if ((w & 0x80000000u) == 0) {                                                        // VOP2 (madmk / madak carry a literal)
    const uint32_t op = (w >> 25) & 0x3F;
    const bool lit = (w & 0x1FF) == kCodeLiteral || op == static_cast<uint32_t>(Vop2::v_madmk_f32) || op == static_cast<uint32_t>(Vop2::v_madak_f32);
    return lit ? 2 : 1;
  }
  return 0;
}

}  // namespace gcn
