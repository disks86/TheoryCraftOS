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
#include "Emulation/RiscV64Crypto.hpp"
#include "Emulation/RiscV64VectorUnit.hpp"

// The vector cryptography extensions (opcode OP-VE): Zvkned (AES), Zvknha / Zvknhb (SHA-2), Zvksed (SM4), Zvksh (SM3) and Zvkg (GHASH). They work on element groups (four elements of
// 32 or 64 bits), so vl and vstart have to be multiples of the group size, and none of them is masked.
namespace emulation {

RiscV64Cpu::Step VectorUnit::crypto() {
  if (funct3 != 2 || !vm) return undefined();
  const bool sha2 = funct6 >= 45 && funct6 <= 47;
  if (sew != 32 && !(sha2 && sew == 64)) return undefined();
  if (vl % 4 != 0 || s.vstart % 4 != 0) return undefined();
  const uint32_t regs = regs_of(lmul_log);
  const uint64_t groups = vl / 4;
  auto group_bytes = [&](uint32_t reg, uint64_t g) { return s.v + reg * kVlenBytes + 16 * g; };
  if ((funct6 == 40 && vs1 <= 3) || (funct6 == 41 && (vs1 <= 3 || vs1 == 7))) {      // vaes*, vaesz
    const bool scalar_key = funct6 == 41;
    if (!group_ok(vd, regs) || (!scalar_key && !group_ok(vs2, regs))) return undefined();
    if (scalar_key && vs2 >= 32) return undefined();
    if (vd >= vs2 && vd < vs2 + regs && !scalar_key && vd != vs2) return undefined();
    if (vs1 == 7 && scalar_key) {      // vaesz.vs: the first round key is added
      for (uint64_t g = s.vstart / 4; g < groups; ++g)
        for (uint32_t i = 0; i < 16; ++i) group_bytes(vd, g)[i] ^= s.v[vs2 * kVlenBytes + i];
      tail(vd, regs, 4, vl);
      return ok();
    }
    if (vs1 <= 3) {
      for (uint64_t g = s.vstart / 4; g < groups; ++g) {
        uint8_t key[16];
        const uint8_t* source = scalar_key ? s.v + vs2 * kVlenBytes : group_bytes(vs2, g);
        for (uint32_t i = 0; i < 16; ++i) key[i] = source[i];
        rvcrypto::aes_round(group_bytes(vd, g), key, vs1);
      }
      tail(vd, regs, 4, vl);
      return ok();
    }
    return undefined();
  }
  if (funct6 == 34 || funct6 == 42) {      // vaeskf1.vi, vaeskf2.vi: the round number is in the vs1 field
    if (!group_ok(vd, regs) || !group_ok(vs2, regs)) return undefined();
    for (uint64_t g = s.vstart / 4; g < groups; ++g) {
      uint8_t out[16], second[16];
      for (uint32_t i = 0; i < 16; ++i) second[i] = group_bytes(vs2, g)[i];
      if (funct6 == 34) rvcrypto::aes_key_128(second, vs1, out);
      else rvcrypto::aes_key_256(group_bytes(vd, g), second, vs1, out);
      for (uint32_t i = 0; i < 16; ++i) group_bytes(vd, g)[i] = out[i];
    }
    tail(vd, regs, 4, vl);
    return ok();
  }
  if (funct6 == 33 || (funct6 == 40 && vs1 == 16) || (funct6 == 41 && vs1 == 16)) {      // vsm4k.vi, vsm4r.vv, vsm4r.vs
    const bool vs_form = funct6 == 41;
    if (!group_ok(vd, regs) || (!vs_form && !group_ok(vs2, regs))) return undefined();
    for (uint64_t g = s.vstart / 4; g < groups; ++g) {
      uint32_t a[4], b[4], out[4];
      for (uint32_t i = 0; i < 4; ++i) {
        a[i] = static_cast<uint32_t>(get(funct6 == 33 ? vs2 : vd, 4, 4 * (funct6 == 33 ? g : g) + i));
        b[i] = static_cast<uint32_t>(vs_form ? get(vs2, 4, i) : get(vs2, 4, 4 * g + i));
      }
      if (funct6 == 33) rvcrypto::sm4_keys(a, vs1, out);
      else rvcrypto::sm4_rounds(a, b, out);
      for (uint32_t i = 0; i < 4; ++i) put(vd, 4, 4 * g + i, out[i]);
    }
    tail(vd, regs, 4, vl);
    return ok();
  }
  if (funct6 == 32 || funct6 == 43) {      // vsm3me.vv, vsm3c.vi (element groups of eight)
    if (vl % 8 != 0 || s.vstart % 8 != 0) return undefined();
    if (!group_ok(vd, regs) || !group_ok(vs2, regs) || (funct6 == 32 && !group_ok(vs1, regs))) return undefined();
    for (uint64_t g = s.vstart / 8; g < vl / 8; ++g) {
      uint32_t d[8], a[8], b[8], out[8];
      for (uint32_t i = 0; i < 8; ++i) { d[i] = static_cast<uint32_t>(get(vd, 4, 8 * g + i)); a[i] = static_cast<uint32_t>(get(vs2, 4, 8 * g + i)); b[i] = funct6 == 32 ? static_cast<uint32_t>(get(vs1, 4, 8 * g + i)) : 0; }
      if (funct6 == 32) rvcrypto::sm3_expand(b, a, out);
      else rvcrypto::sm3_compress(d, a, vs1, out);
      for (uint32_t i = 0; i < 8; ++i) put(vd, 4, 8 * g + i, out[i]);
    }
    tail(vd, regs, 4, vl);
    return ok();
  }
  if (funct6 == 44 || (funct6 == 40 && vs1 == 17)) {      // vghsh.vv, vgmul.vv
    if (!group_ok(vd, regs) || !group_ok(vs2, regs) || (funct6 == 44 && !group_ok(vs1, regs))) return undefined();
    for (uint64_t g = s.vstart / 4; g < groups; ++g) {
      uint8_t x[16];
      for (uint32_t i = 0; i < 16; ++i) x[i] = group_bytes(vd, g)[i] ^ (funct6 == 44 ? group_bytes(vs1, g)[i] : 0);
      uint8_t hh[16], out[16];
      for (uint32_t i = 0; i < 16; ++i) hh[i] = group_bytes(vs2, g)[i];
      rvcrypto::ghash_multiply(x, hh, out);
      for (uint32_t i = 0; i < 16; ++i) group_bytes(vd, g)[i] = out[i];
    }
    tail(vd, regs, 4, vl);
    return ok();
  }
  if (sha2) {
    if (!group_ok(vd, regs) || !group_ok(vs2, regs) || !group_ok(vs1, regs) || vd == vs1 || vd == vs2) return undefined();
    for (uint64_t g = s.vstart / 4; g < groups; ++g) {
      uint64_t d[4], b2[4], b1[4];
      for (uint32_t i = 0; i < 4; ++i) { d[i] = get(vd, sew_bytes, 4 * g + i); b2[i] = get(vs2, sew_bytes, 4 * g + i); b1[i] = get(vs1, sew_bytes, 4 * g + i); }
      if (funct6 == 45) rvcrypto::sha2_message(d, b2, b1, sew);
      else rvcrypto::sha2_rounds(d, b2, b1, sew, funct6 == 46);
      for (uint32_t i = 0; i < 4; ++i) put(vd, sew_bytes, 4 * g + i, d[i]);
    }
    tail(vd, regs, sew_bytes, vl);
    return ok();
  }
  return undefined();
}

}  // namespace emulation
