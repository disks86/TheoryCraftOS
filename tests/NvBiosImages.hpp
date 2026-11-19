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

// Synthesized video BIOS images for the NVIDIA host tests (NvBiosTest, NvidiaNativeTest): the ROM header and PCI data structure, the BIT
// table, and a Kepler-like board's DCB 4.0, I2C 4.1, GPIO 4.1 and connector 4.0 tables.
#pragma once
#include <stdint.h>
#include <vector>
#include "Hardware/GraphicsProcessingUnit/NVIDIA/Nvkm/NvChip.hpp"
#include "Utility/ArrayView.hpp"

using hardware::nv::Chip;
using hardware::nv::identify_chip;

typedef std::vector<uint8_t> Bytes;
static inline void put8(Bytes& v, size_t at, uint32_t x) { if (v.size() < at + 1) v.resize(at + 1); v[at] = static_cast<uint8_t>(x); }
static inline void put16(Bytes& v, size_t at, uint32_t x) { put8(v, at, x); put8(v, at + 1, x >> 8); }
static inline void put32(Bytes& v, size_t at, uint32_t x) { put16(v, at, x); put16(v, at + 2, x >> 16); }
static inline ConstByteView view(const Bytes& v) { return ConstByteView(v.data(), v.size()); }

static inline Chip chip_of(uint32_t boot0, uint32_t boot42 = 0) { return identify_chip(boot0, boot42); }

// --- Image building ---------------------------------------------------------------------------------------------------------------------

// The ROM header and PCI data structure of an x86 image of `units` 512-byte blocks at `base`, PCIR at +0x40, optionally an NPDE after it.
static inline void rom_header(Bytes& v, size_t base, uint32_t units, uint8_t type, bool last, bool npde = false, uint16_t vendor = 0x10de,
                       uint16_t device = 0x1180) {
  if (v.size() < base + units * 512) v.resize(base + units * 512, 0);
  put16(v, base, 0xaa55);
  put8(v, base + 2, units);
  put16(v, base + 0x18, 0x40);
  const size_t p = base + 0x40;
  put32(v, p, 0x52494350);   // "PCIR"
  put16(v, p + 4, vendor);
  put16(v, p + 6, device);
  put16(v, p + 0x0a, 0x18);
  put8(v, p + 0x0c, 0x03);
  put8(v, p + 0x0f, 0x03);
  put16(v, p + 0x10, npde ? 1 : units);   // (with an NPDE, the PCIR length is a decoy: the NPDE's wins)
  put8(v, p + 0x14, type);
  put8(v, p + 0x15, (last && !npde) ? 0x80 : 0x00);
  if (npde) {
    const size_t n = base + 0x60;   // (0x40 + 0x18 + 15) & ~15
    put32(v, n, 0x4544504e);        // "NPDE"
    put16(v, n + 4, 0x0100);
    put16(v, n + 6, 0x0c);
    put16(v, n + 8, units);
    put8(v, n + 0x0a, last ? 0x80 : 0x00);
  }
}

// Makes the bytes of the x86 image at [base, base + size) add up to zero (using its last byte).
static inline void fix_checksum(Bytes& v, size_t base, size_t size) {
  uint8_t sum = 0;
  for (size_t i = base; i < base + size - 1; ++i) sum = static_cast<uint8_t>(sum + v[i]);
  v[base + size - 1] = static_cast<uint8_t>(0x100 - sum);
}

// A BIT table at `at` with an 'i' entry pointing to the version bytes at `info`.
static inline void bit_table(Bytes& v, size_t at, size_t info, const uint8_t version[5]) {
  const uint8_t sig[6] = {0xff, 0xb8, 'B', 'I', 'T', 0x00};
  for (int i = 0; i < 6; ++i) put8(v, at + i, sig[i]);
  put16(v, at + 6, 0x0100);
  put8(v, at + 8, 12);
  put8(v, at + 9, 6);
  put8(v, at + 10, 3);
  // 'I' (init, unused here), 'i' info, 'd' DP
  const size_t e = at + 12;
  put8(v, e + 0, 'I'); put8(v, e + 1, 1); put16(v, e + 2, 0x10); put16(v, e + 4, 0x0400);
  put8(v, e + 6, 'i'); put8(v, e + 7, 2); put16(v, e + 8, 0x20); put16(v, e + 10, static_cast<uint32_t>(info));
  put8(v, e + 12, 'd'); put8(v, e + 13, 1); put16(v, e + 14, 4); put16(v, e + 16, 0x0420);
  // the info entry: micro, minor, chip, major, patch
  put8(v, info + 0, version[3]); put8(v, info + 1, version[2]); put8(v, info + 2, version[1]); put8(v, info + 3, version[0]); put8(v, info + 4, version[4]);
}

// Appends an entry to the BIT table at `bit` (as bit_table builds it: entries of 6 bytes from +12) and raises its count; returns where the
// entry went.
static inline size_t add_bit_entry(Bytes& v, size_t bit, uint8_t id, uint8_t version, uint16_t length, uint16_t offset) {
  const uint8_t count = v[bit + 10];
  const size_t e = bit + 12 + size_t{count} * 6;
  put8(v, e + 0, id); put8(v, e + 1, version); put16(v, e + 2, length); put16(v, e + 4, offset);
  put8(v, bit + 10, count + 1u);
  return e;
}

static inline uint32_t conn_word(uint8_t type, uint8_t i2c, uint8_t heads, uint8_t connector, uint8_t bus, uint8_t location, uint8_t ors) {
  return uint32_t{ors} << 24 | uint32_t{location} << 20 | uint32_t{bus} << 16 | uint32_t{connector} << 12 | uint32_t{heads} << 8 | uint32_t{i2c} << 4 | type;
}

static const uint8_t kKeplerVersion[5] = {0x80, 0x04, 0x31, 0x00, 0x09};   // 80.04.31.00.09

// A Kepler-like (GK104) board: DCB 4.0, DP on SOR0 (HBR2 x4), HDMI on SOR1, dual-link DVI-I on SOR2 with its analog half on DAC0, an EOL.
// I2C 4.1 (PMGR ports), GPIO 4.1, connector table 4.0. One x86 image with an NPDE, followed by an EFI image.
static inline Bytes kepler_image(bool with_efi = true) {
  Bytes v;
  const uint32_t units = 0x40;   // 32 KiB
  rom_header(v, 0, units, 0x00, !with_efi, true);
  bit_table(v, 0x100, 0x180, kKeplerVersion);
  const size_t dcb = 0x400;
  put16(v, 0x36, dcb);
  put8(v, dcb + 0, 0x40);
  put8(v, dcb + 1, 0x17);      // header (>= 0x16: has a connector table)
  put8(v, dcb + 2, 5);         // entries
  put8(v, dcb + 3, 8);
  put16(v, dcb + 4, 0x600);    // I2C
  put32(v, dcb + 6, 0x4edcbdcb);
  put16(v, dcb + 0x0a, 0x700); // GPIO
  put16(v, dcb + 0x14, 0x800); // connectors
  size_t e = dcb + 0x17;
  // DP: i2c/aux 0, heads 0xf, connector 0, SOR0. conf: 5.4 Gb/s, 4 lanes, link A.
  put32(v, e, conn_word(6, 0, 0xf, 0, 0, 0, 0x1)); put32(v, e + 4, 0x0f400010); e += 8;
  // HDMI (TMDS): i2c 1, connector 1, SOR1, link A
  put32(v, e, conn_word(2, 1, 0xf, 1, 1, 0, 0x2)); put32(v, e + 4, 0x00000010); e += 8;
  // DVI-I digital, dual link (A+B), i2c 2, connector 2, SOR2
  put32(v, e, conn_word(2, 2, 0xf, 2, 2, 0, 0x4)); put32(v, e + 4, 0x00000030); e += 8;
  // DVI-I analog, DAC0, same connector and i2c
  put32(v, e, conn_word(0, 2, 0xf, 2, 2, 0, 0x1)); put32(v, e + 4, 0); e += 8;
  // EOL
  put32(v, e, 0x0000000e); put32(v, e + 4, 0);
  // I2C 4.1: header 5 (defaults: primary 0, secondary 1), 4 entries of 4 bytes
  put8(v, 0x600, 0x41); put8(v, 0x601, 5); put8(v, 0x602, 4); put8(v, 0x603, 4); put8(v, 0x604, 0x10);
  put32(v, 0x605 + 0, 0x1f | (0 << 5));      // AUX 0 only (the DP's)
  put32(v, 0x605 + 4, 0x01 | (0x1f << 5));   // I2C port 1 only
  put32(v, 0x605 + 8, 0x02 | (0x1f << 5));   // I2C port 2 only
  put32(v, 0x605 + 12, 0x3ff);               // unused
  // GPIO 4.1: header 6, 4 entries of 5 bytes. HPD 0 (func 0x07) on line 8, HPD 1 (0x08) on line 9, HPD 2 (0x51) on line 10, fan PWM on 16.
  put8(v, 0x700, 0x41); put8(v, 0x701, 6); put8(v, 0x702, 4); put8(v, 0x703, 5);
  const uint8_t gpio[4][2] = {{8, 0x07}, {9, 0x08}, {10, 0x51}, {16, 0x09}};
  for (int i = 0; i < 4; ++i) {
    put32(v, 0x706 + i * 5, gpio[i][0] | (uint32_t{gpio[i][1]} << 8));
    put8(v, 0x706 + i * 5 + 4, i == 3 ? 0x80 | (1 << 5) : (1 << 5));   // log1 = 1; the fan's param bit
  }
  // Connectors 4.0: header 5, 3 entries of 4 bytes: DP (HPD line 0), HDMI (line 1), DVI-I (line 2, in byte 2 bit 0)
  put8(v, 0x800, 0x40); put8(v, 0x801, 5); put8(v, 0x802, 3); put8(v, 0x803, 4);
  put8(v, 0x805 + 0, 0x46); put8(v, 0x805 + 1, 0x10 | 0x40); put8(v, 0x805 + 2, 0); put8(v, 0x805 + 3, 0);
  put8(v, 0x809 + 0, 0x61); put8(v, 0x809 + 1, 0x20 | 0x01); put8(v, 0x809 + 2, 0); put8(v, 0x809 + 3, 0);
  put8(v, 0x80d + 0, 0x30); put8(v, 0x80d + 1, 0x02); put8(v, 0x80d + 2, 0x01); put8(v, 0x80d + 3, 0);
  fix_checksum(v, 0, units * 512);
  if (with_efi) {
    rom_header(v, units * 512, 0x10, 0x03, true, true);   // the EFI image: not checksummed
  }
  return v;
}
