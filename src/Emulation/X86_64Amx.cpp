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

// Intel AMX: the tile configuration (LDTILECFG, STTILECFG, TILERELEASE), tile moves (TILEZERO, TILELOADD, TILELOADDT1, TILESTORED) and the tile matrix multiplies (TDPBSSD / SUD / USD / UUD,
// TDPBF16PS, TDPFP16PS, TCMMIMFP16PS, TCMMRLFP16PS). Only palette 1 exists: eight tiles of up to 16 rows of 64 bytes.
// A tile with zero rows is "not configured"; using one is #UD. A configuration whose row count and column width are not both zero or both non-zero is #GP.
#include "Emulation/HalfFloat.hpp"
#include "Emulation/SoftFloat.hpp"
#include "Emulation/X86_64Cpu.hpp"

namespace emulation {

namespace {

constexpr uint32_t kTiles = 8, kMaxRows = 16, kRowBytes = 64;

uint32_t get32(ByteView row, uint32_t at) {
  return static_cast<uint32_t>(row[at]) | (static_cast<uint32_t>(row[at + 1]) << 8) | (static_cast<uint32_t>(row[at + 2]) << 16) | (static_cast<uint32_t>(row[at + 3]) << 24);
}

void put32(ByteView row, uint32_t at, uint32_t v) {
  for (uint32_t b = 0; b < 4; ++b) row[at + b] = static_cast<uint8_t>(v >> (8 * b));
}

uint32_t get16(ByteView row, uint32_t at) { return static_cast<uint32_t>(row[at]) | (static_cast<uint32_t>(row[at + 1]) << 8); }

// AMX treats a bfloat16 denormal as a zero of its sign (and a binary32 one too).
uint32_t bf16_to_single(uint32_t h) {
  uint32_t v = (h & 0xFFFF) << 16;
  if ((v & 0x7F800000u) == 0) v &= 0x80000000u;
  return v;
}

}  // namespace

bool X86_64Cpu::tile_config_valid(ConstByteView c) {
  if (c[0] > 1) return false;
  if (c[0] == 0) return true;      // (the same as TILERELEASE)
  for (uint32_t i = 2; i < 16; ++i)
    if (c[i] != 0) return false;
  for (uint32_t t = 0; t < 16; ++t) {
    const uint32_t colsb = c[16 + 2 * t] | (uint32_t{c[17 + 2 * t]} << 8), rows = c[48 + t];
    if (t >= kTiles) {
      if (colsb != 0 || rows != 0) return false;
    } else if (colsb > kRowBytes || rows > kMaxRows || (colsb == 0) != (rows == 0)) {
      return false;
    }
  }
  return true;
}

void X86_64Cpu::tile_release() {
  ByteView(tiles_.config, sizeof(tiles_.config)).fill(0);
  for (auto& tile : tiles_.data) ByteView(tile, sizeof(tile)).fill(0);
}

void X86_64Cpu::tile_config_load(ConstByteView c) {
  tile_release();
  if (c[0] == 0) return;
  ByteView(tiles_.config, sizeof(tiles_.config)).copy_from(c.first(64));
  tiles_.config[1] = 0;      // (start_row)
}

X86_64Cpu::Step X86_64Cpu::amx_instruction(GuestMemory& m, uint8_t op, uint32_t vvvv) {
  auto bad_decode = [&]() { return fail(d_.available < 15 ? 14 : 6, s_.rip + d_.available); };
  const uint32_t pp = d_.operand16 ? 1 : d_.rep ? 2 : d_.repne ? 3 : 0;
  if (!decode_modrm() || !finish_decode()) return bad_decode();
  if (vex_.wide || d_.rex_w) return fail(6);      // VEX.L and VEX.W must be zero
  auto done = [&]() {
    s_.rip += d_.position;
    return Step::Ok;
  };
  const uint32_t modrm_raw = d_.bytes[d_.position - 1];      // (the last byte decoded is only the modrm when there is no SIB or displacement; used for the register forms only)
  if (op == 0x49) {
    if (vvvv != 0) return fail(6);
    if (pp == 0 && d_.mod == 3) {      // tilerelease
      if (modrm_raw != 0xC0) return fail(6);
      tile_release();
      return done();
    }
    if (pp == 3) {      // tilezero tmm
      if (d_.mod != 3 || d_.reg >= kTiles || !tiles_configured()) return fail(6);
      ByteView(tiles_.data[d_.reg], 1024).fill(0);
      tiles_.config[1] = 0;
      return done();
    }
    if ((pp != 0 && pp != 1) || d_.mod == 3 || (d_.reg & 7) != 0 || d_.reg >= 8) return fail(6);
    const uint64_t address = memory_address();
    if (pp == 0) {      // ldtilecfg [mem]
      uint8_t raw[64] = {};
      if (!m.read(address, ByteView(raw, sizeof(raw)))) return fail(14, address, 0);
      if (!tile_config_valid(ConstByteView(raw, sizeof(raw)))) return fail(13);
      tile_config_load(ConstByteView(raw, sizeof(raw)));
    } else {      // sttilecfg [mem]
      if (!m.write(address, ConstByteView(tiles_.config, sizeof(tiles_.config)))) return fail(14, address, 2);
    }
    return done();
  }
  if (op == 0x4B) {
    if (vvvv != 0 || d_.mod == 3 || !d_.sib_index || d_.reg >= kTiles || pp == 0) return fail(6);
    const uint32_t t = d_.reg;
    if (!tiles_configured() || tile_rows(t) == 0) return fail(6);
    const uint64_t base = memory_address() - d_.index_offset;
    const uint32_t rows = tile_rows(t), colsb = tile_colsb(t);
    if (tiles_.config[1] >= rows) return fail(13);
    for (uint32_t r = tiles_.config[1]; r < rows; ++r) {
      const uint64_t address = base + r * d_.index_offset;
      ByteView row(tiles_.data[t] + kRowBytes * r, kRowBytes);
      if (pp == 2) {      // tilestored
        if (!m.write(address, ConstByteView(row.data(), colsb))) {
          tiles_.config[1] = static_cast<uint8_t>(r);
          return fail(14, address, 2);
        }
      } else {      // tileloadd, tileloaddt1
        uint8_t raw[kRowBytes] = {};
        if (!m.read(address, ByteView(raw, colsb))) {
          tiles_.config[1] = static_cast<uint8_t>(r);
          return fail(14, address, 0);
        }
        row.fill(0);
        row.copy_from(ConstByteView(raw, colsb));
      }
    }
    if (pp != 2) for (uint32_t r = rows; r < kMaxRows; ++r) ByteView(tiles_.data[t] + kRowBytes * r, kRowBytes).fill(0);
    tiles_.config[1] = 0;
    return done();
  }
  // the matrix multiplies: tmm1 (reg) += tmm2 (rm) x tmm3 (vvvv)
  uint32_t kind;
  if (op == 0x5E) kind = pp == 3 ? 0 : pp == 2 ? 1 : pp == 1 ? 2 : 3;
  else if (op == 0x5C && pp == 2) kind = 4;
  else if (op == 0x5C && pp == 3) kind = 5;
  else if (op == 0x6C && pp == 1) kind = 6;
  else if (op == 0x6C && pp == 0) kind = 7;
  else return fail(6);
  if (d_.mod != 3 || d_.reg >= kTiles || d_.rm >= kTiles || vvvv >= kTiles) return fail(6);
  if (d_.reg == d_.rm || d_.reg == vvvv || d_.rm == vvvv) return fail(6);
  const Step r = amx_dot(kind, d_.reg, d_.rm, vvvv);
  if (r == Step::Ok) return done();
  return r;
}

X86_64Cpu::Step X86_64Cpu::amx_dot(uint32_t kind, uint32_t dst, uint32_t src1, uint32_t src2) {
  if (!tiles_configured() || tile_rows(dst) == 0 || tile_rows(src1) == 0 || tile_rows(src2) == 0) return fail(6);
  // C (dst) is M x N dwords, A (src1) is M x K groups of four bytes, B (src2) is K rows of N groups
  if (tile_colsb(dst) != tile_colsb(src2) || tile_colsb(src1) != 4 * tile_rows(src2) || tile_rows(dst) != tile_rows(src1)) return fail(6);
  const uint32_t rows = tile_rows(dst), n_count = tile_colsb(dst) / 4, k_count = tile_colsb(src1) / 4;
  softfloat::Env env;      // round to nearest even, denormals flushed, MXCSR ignored, no exceptions
  env.daz = env.ftz = true;
  for (uint32_t r = 0; r < rows; ++r) {
    ByteView c(tiles_.data[dst] + kRowBytes * r, kRowBytes), a(tiles_.data[src1] + kRowBytes * r, kRowBytes);
    for (uint32_t n = 0; n < n_count; ++n) {
      uint32_t acc = get32(c, 4 * n);
      for (uint32_t k = 0; k < k_count; ++k) {
        ByteView b(tiles_.data[src2] + kRowBytes * k, kRowBytes);
        if (kind < 4) {      // 4 byte products; src1 is signed for SSD / SUD, src2 for SSD / USD
          const bool a_signed = kind <= 1, b_signed = kind == 0 || kind == 2;
          for (uint32_t j = 0; j < 4; ++j) {
            const int32_t x = a_signed ? static_cast<int8_t>(a[4 * k + j]) : static_cast<int32_t>(a[4 * k + j]);
            const int32_t y = b_signed ? static_cast<int8_t>(b[4 * n + j]) : static_cast<int32_t>(b[4 * n + j]);
            acc += static_cast<uint32_t>(x * y);
          }
          continue;
        }
        // pairs of 16-bit floats: bf16 or fp16 pairs of numbers, or fp16 complex numbers (real, imaginary)
        auto element = [&](ByteView row, uint32_t at) -> uint32_t {
          const uint32_t h = get16(row, at);
          return kind == 4 ? bf16_to_single(h) : half::half_to_single(static_cast<uint16_t>(h));
        };
        const uint32_t a0 = element(a, 4 * k), a1 = element(a, 4 * k + 2), b0 = element(b, 4 * n), b1 = element(b, 4 * n + 2);
        auto fma = [&](uint32_t x, uint32_t y, bool negate) {
          if ((acc & 0x7F800000u) == 0) acc &= 0x80000000u;
          acc = static_cast<uint32_t>(softfloat::fused_multiply_add(softfloat::Format::F32, negate ? (x ^ 0x80000000u) : x, y, acc, env));
          if ((acc & 0x7F800000u) == 0) acc &= 0x80000000u;
        };
        switch (kind) {
          case 4: case 5: fma(a0, b0, false); fma(a1, b1, false); break;      // (the products are exact, so the fused add rounds once, as a multiply and an add would)
          case 6: fma(a0, b1, false); fma(a1, b0, false); break;      // the imaginary part: re(a) im(b) + im(a) re(b)
          default: fma(a0, b0, false); fma(a1, b1, true); break;      // the real part: re(a) re(b) - im(a) im(b)
        }
      }
      put32(c, 4 * n, acc);
    }
    for (uint32_t i = 4 * n_count; i < kRowBytes; ++i) c[i] = 0;
  }
  for (uint32_t r = rows; r < kMaxRows; ++r) ByteView(tiles_.data[dst] + kRowBytes * r, kRowBytes).fill(0);
  tiles_.config[1] = 0;
  return Step::Ok;
}

}  // namespace emulation
