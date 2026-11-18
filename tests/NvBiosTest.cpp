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

// The NVIDIA chip identification and video BIOS code (Nvkm/NvChip, NvBios, NvBiosShadow, NvDevinit) against synthesized images: a Kepler-like
// board with DCB 4.0 (DP, HDMI, dual-link DVI with its analog half), an NV4x/early-Tesla-like DCB 3.0 board, an NV1x-like DCB 2.2 board with a
// BMP, and a BMP-only NV04 board. Each image is cut at every length and has many bits flipped (the sanitizers catch any read outside it).
// Every fetch source runs against a register model / fake ACPI method / fake PCI ROM BAR, with broken images falling through to the next.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <map>
#include <vector>
#include "Hardware/GraphicsProcessingUnit/NVIDIA/NvidiaGpuGenerations.hpp"
#include "Hardware/GraphicsProcessingUnit/NVIDIA/Nvkm/NvBios.hpp"
#include "Hardware/GraphicsProcessingUnit/NVIDIA/Nvkm/NvChip.hpp"
#include "Hardware/GraphicsProcessingUnit/NVIDIA/Nvkm/NvDevinit.hpp"

using namespace hardware;
using namespace hardware::nv;

static int g_failed = 0;
#define CHECK(...) do { if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)

struct TestAllocator final : allocators::IAllocator {
  void* allocate(uint64_t size) override { return malloc(size); }
  void* allocate_aligned(uint64_t size, uint64_t alignment) override {
    const uint64_t al = alignment < 16 ? 16 : alignment;
    return aligned_alloc(al, (size + al - 1) / al * al);
  }
  void deallocate(void* p) override { if (p) free(p); }
};

typedef std::vector<uint8_t> Bytes;
static void put8(Bytes& v, size_t at, uint32_t x) { if (v.size() < at + 1) v.resize(at + 1); v[at] = static_cast<uint8_t>(x); }
static void put16(Bytes& v, size_t at, uint32_t x) { put8(v, at, x); put8(v, at + 1, x >> 8); }
static void put32(Bytes& v, size_t at, uint32_t x) { put16(v, at, x); put16(v, at + 2, x >> 16); }
static ConstByteView view(const Bytes& v) { return ConstByteView(v.data(), v.size()); }

static Chip chip_of(uint32_t boot0, uint32_t boot42 = 0) { return identify_chip(boot0, boot42); }

// --- Image building ---------------------------------------------------------------------------------------------------------------------

// The ROM header and PCI data structure of an x86 image of `units` 512-byte blocks at `base`, PCIR at +0x40, optionally an NPDE after it.
static void rom_header(Bytes& v, size_t base, uint32_t units, uint8_t type, bool last, bool npde = false, uint16_t vendor = 0x10de,
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
static void fix_checksum(Bytes& v, size_t base, size_t size) {
  uint8_t sum = 0;
  for (size_t i = base; i < base + size - 1; ++i) sum = static_cast<uint8_t>(sum + v[i]);
  v[base + size - 1] = static_cast<uint8_t>(0x100 - sum);
}

// A BIT table at `at` with an 'i' entry pointing to the version bytes at `info`.
static void bit_table(Bytes& v, size_t at, size_t info, const uint8_t version[5]) {
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

static uint32_t conn_word(uint8_t type, uint8_t i2c, uint8_t heads, uint8_t connector, uint8_t bus, uint8_t location, uint8_t ors) {
  return uint32_t{ors} << 24 | uint32_t{location} << 20 | uint32_t{bus} << 16 | uint32_t{connector} << 12 | uint32_t{heads} << 8 | uint32_t{i2c} << 4 | type;
}

static const uint8_t kKeplerVersion[5] = {0x80, 0x04, 0x31, 0x00, 0x09};   // 80.04.31.00.09

// A Kepler-like (GK104) board: DCB 4.0, DP on SOR0 (HBR2 x4), HDMI on SOR1, dual-link DVI-I on SOR2 with its analog half on DAC0, an EOL.
// I2C 4.1 (PMGR ports), GPIO 4.1, connector table 4.0. One x86 image with an NPDE, followed by an EFI image.
static Bytes kepler_image(bool with_efi = true) {
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

static const uint8_t kNv40Version[5] = {0x05, 0x40, 0x22, 0x01, 0x00};

// An NV4x/early-Tesla-like board: DCB 3.0 (header 0x0c: no connector table), a CRT on DAC0, a TV on DAC1, a TMDS on SOR0. I2C 3.0 with
// NV04 bit-banged ports and an NV4E one; GPIO 3.0 (16-bit entries).
static Bytes dcb30_image() {
  Bytes v;
  const uint32_t units = 0x20;
  rom_header(v, 0, units, 0x00, true);
  bit_table(v, 0x100, 0x180, kNv40Version);
  const size_t dcb = 0x300;
  put16(v, 0x36, dcb);
  put8(v, dcb + 0, 0x30); put8(v, dcb + 1, 0x0c); put8(v, dcb + 2, 4); put8(v, dcb + 3, 8);
  put16(v, dcb + 4, 0x500);
  put32(v, dcb + 6, 0x4edcbdcb);
  put16(v, dcb + 0x0a, 0x580);
  size_t e = dcb + 0x0c;
  put32(v, e, conn_word(0, 0, 0x3, 0, 0, 0, 0x1)); put32(v, e + 4, 0); e += 8;
  put32(v, e, conn_word(1, 1, 0x2, 1, 1, 0, 0x2)); put32(v, e + 4, 0); e += 8;
  put32(v, e, conn_word(15, 0, 0, 0, 0, 0, 0)); put32(v, e + 4, 0); e += 8;   // unused: skipped
  put32(v, e, conn_word(2, 2, 0x3, 2, 2, 0, 0x1)); put32(v, e + 4, 0x00000030);   // (conf not read before 4.0)
  // I2C 3.0: header 5 (defaults 2 / 0), 3 entries
  put8(v, 0x500, 0x30); put8(v, 0x501, 5); put8(v, 0x502, 3); put8(v, 0x503, 4); put8(v, 0x504, 0x02);
  put8(v, 0x505, 0x3e); put8(v, 0x506, 0x3f); put8(v, 0x507, 0); put8(v, 0x508, 0x00);     // NV04 bit: drive 0x3e sense 0x3f
  put8(v, 0x509, 0x36); put8(v, 0x50a, 0x37); put8(v, 0x50b, 0); put8(v, 0x50c, 0x00);
  put8(v, 0x50d, 0x00); put8(v, 0x50e, 0x51); put8(v, 0x50f, 0); put8(v, 0x510, 0x04);     // NV4E bit: drive 0x51
  // GPIO 3.0: header 4, 2 entries of 2 bytes: line 3 func 0x0c (panel power) log 0/1, line 5 func 0x09 param
  put8(v, 0x580, 0x30); put8(v, 0x581, 4); put8(v, 0x582, 2); put8(v, 0x583, 2);
  put16(v, 0x584, 3 | (0x0c << 5) | (0 << 11) | (1 << 13));
  put16(v, 0x586, 5 | (0x09 << 5) | (1 << 11) | (0 << 13) | 0x8000);
  fix_checksum(v, 0, units * 512);
  return v;
}

// An NV1x-like board: a BMP (no BIT), DCB 2.2 with a CRT and a TV and an EOL, the I2C table (16 entries of 4 bytes, no header) right after
// the entries, and a GPIO 2.x table found through the pointer 15 bytes before the DCB (marker byte >= 0x13 just before it).
static Bytes dcb22_image() {
  Bytes v;
  const uint32_t units = 0x10;
  rom_header(v, 0, units, 0x00, true);
  const uint8_t bmp[5] = {0xff, 0x7f, 'N', 'V', 0x00};
  for (int i = 0; i < 5; ++i) put8(v, 0x80 + i, bmp[i]);
  put8(v, 0x85, 5); put8(v, 0x86, 0x10);   // BMP 5.16
  put8(v, 0x80 + 10, 0x07); put8(v, 0x80 + 11, 0x23); put8(v, 0x80 + 12, 0x11); put8(v, 0x80 + 13, 0x03);   // version 03.11.23.07
  const size_t dcb = 0x400;
  put16(v, 0x36, dcb);
  put16(v, dcb - 0x0f, 0x600);   // GPIO pointer
  put8(v, dcb - 1, 0x13);
  put8(v, dcb + 0, 0x22);
  put16(v, dcb + 2, dcb + 8 + 3 * 8);   // the I2C table right after three entries
  put32(v, dcb + 4, 0x4edcbdcb);
  size_t e = dcb + 8;
  put32(v, e, conn_word(0, 0, 0x1, 0, 0, 0, 0x1)); e += 8;
  put32(v, e, conn_word(1, 1, 0x1, 1, 0, 0, 0x2)); e += 8;
  put32(v, e, 0x0000000e); e += 8;
  // I2C 2.x: type in bits 2:0 of byte 3
  const size_t i2c = dcb + 8 + 3 * 8;
  for (int i = 0; i < 16; ++i) put32(v, i2c + i * 4, 0x07000000);   // unused
  put8(v, i2c + 0, 0x3f); put8(v, i2c + 1, 0x3e); put8(v, i2c + 3, 0x00);
  put8(v, i2c + 4, 0x37); put8(v, i2c + 5, 0x36); put8(v, i2c + 7, 0x00);
  // GPIO 2.x: version 0x20, entry length 2, 1 entry
  put8(v, 0x600, 0x20); put8(v, 0x601, 2); put8(v, 0x602, 1);
  put16(v, 0x603, 4 | (0x0c << 5));
  fix_checksum(v, 0, units * 512);
  return v;
}

// An NV04 board: a BMP 3.x and nothing else; the two fixed I2C ports at 0x48 (one left 0: the defaults).
static Bytes nv04_image() {
  Bytes v;
  rom_header(v, 0, 8, 0x00, true);
  const uint8_t bmp[5] = {0xff, 0x7f, 'N', 'V', 0x00};
  for (int i = 0; i < 5; ++i) put8(v, 0x100 + i, bmp[i]);
  put8(v, 0x105, 3); put8(v, 0x106, 0x01);
  put8(v, 0x48 + 4, 0x1f); put8(v, 0x48 + 5, 0x1e); put8(v, 0x48 + 6, 0); put8(v, 0x48 + 7, 0);
  put16(v, 0x36, 0x200);   // (an NV04 has no DCB: the pointer is ignored)
  fix_checksum(v, 0, 8 * 512);
  return v;
}

// --- Chip identification ----------------------------------------------------------------------------------------------------------------

static void test_identify() {
  struct Case { uint32_t boot0, boot42; uint32_t chipset; Family family; const char* name; uint16_t card_type; bool gsp, native, display; };
  const Case cases[] = {
      {0x20004000, 0, 0x04, Family::Nv04, "NV04", 0x04, false, false, true},
      {0x20104000, 0, 0x05, Family::Nv04, "NV05", 0x04, false, false, true},
      {0x010000a1, 0, 0x10, Family::Nv10, "NV10", 0x10, false, false, true},
      {0x011000a1, 0, 0x11, Family::Nv10, "NV11", 0x11, false, false, true},
      {0x01f000a1, 0, 0x1f, Family::Nv10, "nForce2", 0x11, false, false, true},
      {0x025000a2, 0, 0x25, Family::Nv20, "NV25", 0x20, false, false, true},
      {0x034000a1, 0, 0x34, Family::Nv30, "NV34", 0x30, false, false, true},
      {0x049000a1, 0, 0x49, Family::Nv40, "G71", 0x40, false, false, true},
      {0x063000a2, 0, 0x63, Family::Nv40, "C73", 0x40, false, false, true},
      {0x050000a2, 0, 0x50, Family::Tesla, "G80", 0x50, false, true, true},
      {0x092000a2, 0, 0x92, Family::Tesla, "G92", 0x50, false, true, true},
      {0x0ac000a1, 0, 0xac, Family::Tesla, "MCP79/MCP7A", 0x50, false, true, true},
      {0x0c0000a3, 0, 0xc0, Family::Fermi, "GF100", 0xc0, false, true, true},
      {0x0d7000a1, 0, 0xd7, Family::Fermi, "GF117", 0xc0, false, true, true},
      {0x0e4000a2, 0, 0xe4, Family::Kepler, "GK104", 0xe0, false, true, true},
      {0x0ea000a1, 0, 0xea, Family::Kepler, "GK20A", 0xe0, false, false, false},
      {0x108000a1, 0, 0x108, Family::Kepler, "GK208", 0xe0, false, true, true},
      {0x117000a2, 0, 0x117, Family::Maxwell, "GM107", 0x110, false, true, true},
      {0x12b000a1, 0, 0x12b, Family::Maxwell, "GM20B", 0x110, false, false, false},
      {0x134000a1, 0, 0x134, Family::Pascal, "GP104", 0x130, false, true, true},
      {0x140000a1, 0, 0x140, Family::Volta, "GV100", 0x140, false, true, true},
      {0x164000a1, 0, 0x164, Family::Turing, "TU104", 0x160, false, true, true},
      {0x164000a1, 0x168000a1 | (0xa << 16) | (1 << 12), 0x168, Family::Turing, "TU116", 0x160, false, true, true},   // BOOT_42 wins
      {0x170000a1, 0, 0x170, Family::Ampere, "GA100", 0x170, true, false, false},
      {0x172000a1, 0, 0x172, Family::Ampere, "GA102", 0x170, true, false, true},
      {0x180000a1, 0, 0x180, Family::Hopper, "GH100", 0x180, true, false, false},
      {0x194000a1, 0, 0x194, Family::Ada, "AD104", 0x190, true, false, true},
      {0x1b2000a1, 0x1b200000, 0x1b2, Family::Blackwell, "GB202", 0x1a0, true, false, true},
      {0x1a0000a1, 0, 0x1a0, Family::Blackwell, "GB100", 0x1a0, true, false, false},
  };
  for (const Case& c : cases) {
    const Chip chip = identify_chip(c.boot0, c.boot42);
    const bool ok = chip.chipset == c.chipset && chip.family == c.family && strcmp(chip.name, c.name) == 0 && chip.card_type == c.card_type &&
                    chip.needs_gsp == c.gsp && chip.display_native == c.native && chip.has_display == c.display;
    if (!ok) printf("  identify %08x: chipset %x family %s name %s card %x gsp %d native %d disp %d\n", c.boot0, chip.chipset, family_name(chip.family),
                    chip.name, chip.card_type, chip.needs_gsp, chip.display_native, chip.has_display);
    CHECK(ok);
  }
  CHECK(identify_chip(0x0e4000a2, 0).revision == 0xa2);
  CHECK(identify_chip(0x011000a1, 0).revision == 0);   // nouveau drops the NV1x revision
  CHECK(identify_chip(0x0ea000a1, 0).tegra && identify_chip(0x0ea000a1, 0).integrated);
  CHECK(identify_chip(0x04e000a1, 0).integrated && !identify_chip(0x04e000a1, 0).tegra);
  // Edge cases: all ones (powered down), zero, NV01/NV03-era values, unknown chipsets inside and outside known families, a BOOT_42 that reads junk
  CHECK(identify_chip(0xFFFFFFFF, 0).family == Family::Unknown && strcmp(identify_chip(0xFFFFFFFF, 0).name, "unknown") == 0);
  CHECK(identify_chip(0, 0).family == Family::Unknown);
  CHECK(identify_chip(0x00030010, 0).family == Family::Unknown);
  CHECK(identify_chip(0x0e5000a1, 0).family == Family::Unknown && identify_chip(0x0e5000a1, 0).chipset == 0xe5);   // GK10x hole
  CHECK(identify_chip(0x070000a1, 0).family == Family::Unknown);   // no 0x70 family
  CHECK(identify_chip(0x1f0000a1, 0).family == Family::Unknown);
  CHECK(identify_chip(0x164000a1, 0xFFFFFFFF).chipset == 0x164);   // BOOT_42 all ones: BOOT_0 stands
  CHECK(identify_chip(0x164000a1, 0x00000000).chipset == 0x164);
  CHECK(identify_chip(0x0e4000a2, 0x168000a1).chipset == 0xe4);    // BOOT_42 only from Turing on
  CHECK(identify_chip(0x164000a1, 0x1e5000a1).family == Family::Unknown);   // BOOT_42 names a chip nobody knows
  // Consistent with NvidiaGpuGenerations for every Pascal+ chip it names.
  for (uint32_t chipset = 0x130; chipset < 0x1c0; ++chipset) {
    const char* old_name = nvidia_chip_name(chipset);
    if (!old_name) continue;
    const Chip chip = identify_chip(chipset << 20 | 0xa1, 0);
    CHECK(strcmp(chip.name, old_name) == 0);
    const NvidiaGpuInfo info = nvidia_gpu_identify_boot0(chipset << 20 | 0xa1);
    CHECK(info.architecture != NvidiaGpuArchitecture::Unknown && chip.family != Family::Unknown);
  }
  // Every family has a name.
  for (int f = 0; f <= static_cast<int>(Family::Blackwell); ++f) CHECK(strcmp(family_name(static_cast<Family>(f)), "") != 0);
}

// --- Parsing --------------------------------------------------------------------------------------------------------------------------------

static void test_kepler_parse() {
  const Bytes image = kepler_image();
  const NvBios bios(view(image));
  BoardInfo board;
  const BoardParseResult r = parse_board(bios, chip_of(0x0e4000a2), &board);
  CHECK(r.has_bit && !r.has_bmp && r.has_dcb && r.dcb_problem == nullptr && !board.truncated);
  CHECK(board.bios_version[0] == 0x80 && board.bios_version[1] == 0x04 && board.bios_version[2] == 0x31 && board.bios_version[3] == 0x00 &&
        board.bios_version[4] == 0x09);
  CHECK(board.dcb_version == 0x40 && board.output_count == 4);
  const DcbOutput& dp = board.outputs[0];
  CHECK(dp.type == DcbOutputType::Dp && dp.or_mask == 1 && dp.heads == 0xf && dp.connector == 0 && dp.i2c_index == 0);
  CHECK(dp.dp_max_link_rate == 0x14 && dp.dp_max_lanes == 4 && dp.sorconf_link == 1 && dp.location == 0 && dp.extdev == 0);
  CHECK(dp.hasht == 0x0006 && dp.hashm == (0xf << 8 | 1 << 6 | 1));
  const DcbOutput& hdmi = board.outputs[1];
  CHECK(hdmi.type == DcbOutputType::Tmds && hdmi.or_mask == 2 && hdmi.i2c_index == 1 && hdmi.connector == 1 && hdmi.sorconf_link == 1 &&
        hdmi.dp_max_lanes == 0 && hdmi.bus == 1);
  const DcbOutput& dvi = board.outputs[2];
  CHECK(dvi.type == DcbOutputType::Tmds && dvi.or_mask == 4 && dvi.sorconf_link == 3);
  const DcbOutput& crt = board.outputs[3];
  CHECK(crt.type == DcbOutputType::Analog && crt.connector == 2 && crt.i2c_index == 2 && crt.sorconf_link == 0);
  CHECK(board.i2c_count == 4 && board.i2c_default[0] == 0 && board.i2c_default[1] == 1);
  CHECK(board.i2c[0].type == I2cPortType::Pmgr && board.i2c[0].drive == 0xff && board.i2c[0].aux == 0 && board.i2c[0].share == 0);
  CHECK(board.i2c[1].type == I2cPortType::Pmgr && board.i2c[1].drive == 1 && board.i2c[1].aux == 0xff);
  CHECK(board.i2c[2].drive == 2 && board.i2c[3].type == I2cPortType::Unused);
  CHECK(board.gpio_count == 4);
  CHECK(board.gpio[0].line == 8 && board.gpio[0].function == 0x07 && board.gpio[0].log0 == 0 && board.gpio[0].log1 == 1 && !board.gpio[0].param);
  CHECK(board.gpio[3].line == 16 && board.gpio[3].function == 0x09 && board.gpio[3].param);
  CHECK(board.connector_count == 3);
  CHECK(board.connectors[0].type == 0x46 && board.connectors[0].hpd == 0x07 && board.connectors[0].hpd_mask == 1 && board.connectors[0].dp_mask == 1);
  CHECK(board.connectors[1].type == 0x61 && board.connectors[1].hpd == 0x08 && board.connectors[1].location == 1);
  CHECK(board.connectors[2].type == 0x30 && board.connectors[2].hpd == 0x51 && board.connectors[2].index == 2);
  BitEntry e;
  CHECK(bios.bit_entry('d', &e) && e.version == 1 && e.offset == 0x420 && !bios.bit_entry('Z', &e));
}

static void test_dcb30_parse() {
  const Bytes image = dcb30_image();
  BoardInfo board;
  const BoardParseResult r = parse_board(NvBios(view(image)), chip_of(0x046000a1), &board);
  CHECK(r.has_dcb && !board.truncated && board.dcb_version == 0x30);
  CHECK(board.bios_version[0] == 0x05 && board.bios_version[1] == 0x40);
  CHECK(board.output_count == 3);   // the unused entry is skipped
  CHECK(board.outputs[0].type == DcbOutputType::Analog && board.outputs[0].heads == 3);
  CHECK(board.outputs[1].type == DcbOutputType::Tv && board.outputs[1].or_mask == 2 && board.outputs[1].i2c_index == 1);
  CHECK(board.outputs[2].type == DcbOutputType::Tmds && board.outputs[2].sorconf_link == 0 && board.outputs[2].raw[1] == 0x30);
  CHECK(board.i2c_count == 3 && board.i2c_default[0] == 2 && board.i2c_default[1] == 0);
  CHECK(board.i2c[0].type == I2cPortType::Nv04Bit && board.i2c[0].drive == 0x3e && board.i2c[0].sense == 0x3f);
  CHECK(board.i2c[2].type == I2cPortType::Nv4eBit && board.i2c[2].drive == 0x51 && board.i2c[2].sense == 0xff);
  CHECK(board.gpio_count == 2 && board.gpio[0].line == 3 && board.gpio[0].function == 0x0c && board.gpio[0].log1 == 1);
  CHECK(board.gpio[1].line == 5 && board.gpio[1].function == 0x09 && board.gpio[1].log0 == 1 && board.gpio[1].param);
  CHECK(board.connector_count == 0);   // the 3.0 header is too short for a connector table
}

static void test_dcb22_parse() {
  const Bytes image = dcb22_image();
  const NvBios bios(view(image));
  CHECK(bios.bit_offset() == 0 && bios.bmp_offset() == 0x80 && bios.bmp_version() == 0x0510);
  BoardInfo board;
  const BoardParseResult r = parse_board(bios, chip_of(0x017000a1), &board);
  CHECK(r.has_dcb && r.has_bmp && !r.has_bit && board.dcb_version == 0x22 && !board.truncated);
  CHECK(board.bios_version[0] == 0x03 && board.bios_version[1] == 0x11 && board.bios_version[2] == 0x23 && board.bios_version[3] == 0x07);
  CHECK(board.output_count == 2 && board.outputs[0].type == DcbOutputType::Analog && board.outputs[1].type == DcbOutputType::Tv);
  CHECK(board.i2c_count == 16 && board.i2c[0].type == I2cPortType::Nv04Bit && board.i2c[0].drive == 0x3f && board.i2c[1].sense == 0x36 &&
        board.i2c[2].type == I2cPortType::Unused);
  CHECK(board.gpio_count == 1 && board.gpio[0].line == 4 && board.gpio[0].function == 0x0c);
}

static void test_nv04_parse() {
  const Bytes image = nv04_image();
  BoardInfo board;
  const BoardParseResult r = parse_board(NvBios(view(image)), chip_of(0x20004000), &board);
  CHECK(!r.has_dcb && r.has_bmp && r.dcb_problem != nullptr);
  CHECK(board.i2c_count == 2 && board.i2c[0].drive == 0x1f && board.i2c[0].sense == 0x1e && board.i2c[1].drive == 0x37 && board.i2c[1].sense == 0x36);
  // The same image on an NV10 reads the DCB pointer: 0x200 holds nothing valid.
  const BoardParseResult r2 = parse_board(NvBios(view(image)), chip_of(0x010000a1), &board);
  CHECK(!r2.has_dcb && r2.dcb_problem != nullptr);
}

static void test_bad_tables() {
  BoardInfo board;
  const Chip kepler = chip_of(0x0e4000a2);
  Bytes v = kepler_image(false);
  put8(v, 0x400, 0x42);   // unknown DCB version
  CHECK(!parse_board(NvBios(view(v)), kepler, &board).has_dcb);
  v = kepler_image(false);
  put32(v, 0x406, 0x12345678);   // bad signature
  CHECK(!parse_board(NvBios(view(v)), kepler, &board).has_dcb);
  v = kepler_image(false);
  put8(v, 0x402, 0xff);    // 255 entries: they run past... no, the image is 32 KiB; but the count is capped at what fits in BoardInfo
  (void)parse_board(NvBios(view(v)), kepler, &board);
  CHECK(board.output_count <= BoardInfo::kMaxOutputs);
  v = kepler_image(false);
  put8(v, 0x403, 2);       // entries of 2 bytes: unusable
  CHECK(parse_board(NvBios(view(v)), kepler, &board).has_dcb && board.output_count == 0 && board.truncated);
  v = kepler_image(false);
  put16(v, 0x36, 0xfffe);  // pointer past the image
  CHECK(!parse_board(NvBios(view(v)), kepler, &board).has_dcb);
  v = kepler_image(false);
  put16(v, 0x806, 0);      // (connector entries) fine; now a table pointer to the very end
  put16(v, 0x414, static_cast<uint32_t>(v.size() - 2));
  CHECK(parse_board(NvBios(view(v)), kepler, &board).has_dcb && board.connector_count == 0 && board.truncated);
  // the GPIO table with a huge count is cut at the image end and at kMaxGpio
  v = kepler_image(false);
  put8(v, 0x702, 0xff);
  (void)parse_board(NvBios(view(v)), kepler, &board);
  CHECK(board.gpio_count == BoardInfo::kMaxGpio && board.truncated);
  // the BIT table with an entry size below 6 has no entries
  v = kepler_image(false);
  put8(v, 0x109, 4);
  BitEntry e;
  CHECK(!NvBios(view(v)).bit_entry('i', &e));
  // an empty image
  CHECK(!parse_board(NvBios(ConstByteView()), kepler, &board).has_dcb);
}

// Every prefix of the image, and many single and double bit flips: parse must stay inside the image (ASan) and BoardInfo's counts in range.
static void fuzz_image(const Bytes& image, const Chip& chip, uint32_t seed) {
  BoardInfo board;
  for (size_t length = 0; length <= image.size(); length += (length < 0x1000 ? 1 : 61)) {
    // A copy of exactly `length` bytes, so ASan sees a read past it.
    Bytes cut(image.begin(), image.begin() + length);
    (void)parse_board(NvBios(view(cut)), chip, &board);
    CHECK(board.output_count <= BoardInfo::kMaxOutputs && board.i2c_count <= BoardInfo::kMaxI2c && board.gpio_count <= BoardInfo::kMaxGpio &&
          board.connector_count <= BoardInfo::kMaxConnectors);
    NvBiosImageHeader h;
    (void)NvBiosShadow::parse_image_header(view(cut), 0, &h);
  }
  uint32_t state = seed;
  auto next = [&] { state = state * 1103515245u + 12345u; return state >> 8; };
  for (int round = 0; round < 3000; ++round) {
    Bytes v = image;
    const int flips = 1 + (round % 3);
    for (int k = 0; k < flips; ++k) {
      // Mostly in the first 4 KiB, where the tables are.
      const size_t at = (round & 1) ? next() % (v.size() < 0x1000 ? v.size() : 0x1000) : next() % v.size();
      v[at] ^= static_cast<uint8_t>(1u << (next() % 8));
    }
    (void)parse_board(NvBios(view(v)), chip, &board);
    CHECK(board.output_count <= BoardInfo::kMaxOutputs && board.i2c_count <= BoardInfo::kMaxI2c && board.gpio_count <= BoardInfo::kMaxGpio &&
          board.connector_count <= BoardInfo::kMaxConnectors);
    NvBiosImageHeader h;
    (void)NvBiosShadow::parse_image_header(view(v), 0, &h);
  }
}

// --- Fetching -----------------------------------------------------------------------------------------------------------------------------

// BAR0 as the shadow code sees it: plain registers, PRAMIN (the 1 MiB window at 0x700000 onto VRAM at 0x1700 << 16) and PROM (0x300000:
// the ROM while the shadow bit in the PCI config mirror is clear, all ones while it is set).
struct FakeBar0 final : INvRegisters {
  std::map<uint32_t, uint32_t> regs;
  Bytes vram;        // VRAM from 0
  Bytes rom;         // what PROM shows
  uint32_t pci_mirror = 0x088000;
  uint32_t window_writes = 0;
  uint32_t read32(uint32_t offset) override {
    if (offset >= 0x700000 && offset < 0x800000) {
      const uint64_t at = (uint64_t{regs[0x1700]} << 16) + (offset - 0x700000);
      uint32_t v = 0;
      for (int i = 0; i < 4; ++i) v |= uint32_t{at + i < vram.size() ? vram[at + i] : uint8_t{0}} << (8 * i);
      return v;
    }
    if (offset >= 0x300000 && offset < 0x400000) {
      if (regs[pci_mirror + 0x50] & 1) return 0xFFFFFFFF;
      const uint64_t at = offset - 0x300000;
      uint32_t v = 0;
      for (int i = 0; i < 4; ++i) v |= uint32_t{at + i < rom.size() ? rom[at + i] : uint8_t{0xff}} << (8 * i);
      return v;
    }
    auto it = regs.find(offset);
    return it == regs.end() ? 0 : it->second;
  }
  void write32(uint32_t offset, uint32_t value) override {
    if (offset == 0x1700) ++window_writes;
    regs[offset] = value;
  }
};

struct FakeAcpi final : INvAcpiRom {
  Bytes rom;
  bool fast_broken = false;   // fdo#55948: a big request gives only 4 KiB
  uint32_t calls = 0;
  uint32_t read_rom(uint32_t offset, ByteView out) override {
    ++calls;
    uint64_t length = out.size();
    if (fast_broken && length > 0x1000) length = 0x1000;
    uint64_t i = 0;
    for (; i < length && offset + i < rom.size(); ++i) out[i] = rom[offset + i];
    return static_cast<uint32_t>(i);
  }
};

struct RomPci final : IPciBus {
  uint32_t rom_register = 0, size_mask = 0;
  std::vector<uint32_t> writes;
  const char* name() const override { return "pci"; }
  bool initialize() override { return true; }
  void shutdown() override {}
  uint32_t device_count() const override { return 1; }
  bool pci_device(uint32_t, PciDeviceInfo*) const override { return false; }
  uint32_t read32(PciAddress, uint16_t offset) const override { return offset == 0x30 ? rom_register : 0; }
  void write32(PciAddress, uint16_t offset, uint32_t value) const override {
    RomPci& self = const_cast<RomPci&>(*this);
    if (offset != 0x30) return;
    self.writes.push_back(value);
    self.rom_register = value & (size_mask | 1);
  }
  uint64_t memory_bar(PciAddress, uint8_t) const override { return 0; }
  uint16_t find_capability(PciAddress, uint8_t) const override { return 0; }
};

// Maps the expansion ROM window (at the ROM BAR's address) and the legacy shadow (0xC0000) onto test buffers.
struct FakeMapper final : IRegisterMapper {
  uint64_t rom_at = 0xE0000000;
  Bytes rom_window, legacy;
  bool map_registers(uint64_t physical, uint64_t length, MmioRegion* region) override {
    Bytes* memory = physical == rom_at ? &rom_window : physical == 0xC0000 ? &legacy : nullptr;
    if (!memory || memory->size() < length) return false;
    *region = MmioRegion(reinterpret_cast<uint64_t>(memory->data()), length);
    return true;
  }
};

// A Kepler with the PRAMIN window set up the way the x86 VBIOS leaves it: display enabled, window enabled in VRAM at `vram_address`.
static void setup_pramin(FakeBar0& bar0, const Bytes& image, uint64_t vram_address = 0x100000) {
  bar0.regs[0x022500] = 0;
  bar0.regs[0x619f04] = static_cast<uint32_t>((vram_address >> 8) & 0xffffff00) | 0x8 | 0x1;
  bar0.vram.assign(vram_address + image.size(), 0);
  memcpy(bar0.vram.data() + vram_address, image.data(), image.size());
  bar0.regs[0x1700] = 0x1234;
}

static bool same(const ByteBuffer& out, const Bytes& image) { return out.view().equals(view(image)); }

static void test_fetch() {
  TestAllocator alloc;
  const Bytes good = kepler_image();
  const Chip kepler = chip_of(0x0e4000a2);
  ByteBuffer out(alloc);
  NvBiosFetchReport report;

  {
    // PRAMIN first: the window pointed at the image and put back.
    FakeBar0 bar0;
    setup_pramin(bar0, good);
    NvBiosSources s;
    s.registers = &bar0;
    s.chip = kepler;
    s.allocator = &alloc;
    CHECK(NvBiosShadow::fetch(s, &out, &report) == BiosSource::Pramin && same(out, good));
    CHECK(report.chosen == BiosSource::Pramin && report.score == 8 && report.attempts[0].score == 8);   // two images, both good
    CHECK(bar0.regs[0x1700] == 0x1234 && bar0.window_writes == 2);
    CHECK(report.attempts[3].tried == false && report.attempts[4].tried == false);   // ACPI slow and the ROM BAR skipped (score >= 4 / >= 1)
    CHECK(report.attempts[1].tried && report.attempts[1].score == 0);                // PROM tried (nouveau tries all unskipped sources)
    CHECK((bar0.regs[0x088050] & 1) == 1);                                            // and the ROM shadow switched back on
  }
  {
    // A wrong signature, a wrong vendor, a short image, a bad checksum in PRAMIN: PROM's good image wins.
    struct Damage { const char* what; void (*apply)(Bytes&); };
    const Damage damages[] = {
        {"signature", [](Bytes& v) { v[1] = 0xAB; }},
        {"vendor", [](Bytes& v) { put16(v, 0x44, 0x1002); }},
        {"too long", [](Bytes& v) { put16(v, 0x60 + 8, 0x0900); }},   // the NPDE claims 1.125 MiB: more than the window
        {"checksum", [](Bytes& v) { v[0x1000] ^= 0x55; }},
        {"no pcir", [](Bytes& v) { put32(v, 0x40, 0x11223344); }},
        {"zero length", [](Bytes& v) { put16(v, 0x60 + 8, 0); }},
    };
    for (const Damage& d : damages) {
      Bytes bad = good;
      d.apply(bad);
      FakeBar0 bar0;
      setup_pramin(bar0, bad);
      bar0.rom = good;
      NvBiosSources s;
      s.registers = &bar0;
      s.chip = kepler;
      s.allocator = &alloc;
      const BiosSource got = NvBiosShadow::fetch(s, &out, &report);
      if (got != BiosSource::Prom) printf("  damage %s: got %s (%s)\n", d.what, bios_source_name(got), report.attempts[0].reason);
      CHECK(got == BiosSource::Prom && same(out, good));
      CHECK(strcmp(report.attempts[0].reason, "ok") != 0);
      if (strcmp(d.what, "checksum") == 0) CHECK(report.attempts[0].score == 3 + 4);   // kept as a fallback: rw source with a bad checksum (+ the EFI image)
      else CHECK(report.attempts[0].score == 0);
    }
  }
  {
    // A bad checksum everywhere: the best of the bad (PRAMIN, rw: 3 per image) wins over PROM (2).
    Bytes bad = good;
    bad[0x1000] ^= 1;
    FakeBar0 bar0;
    setup_pramin(bar0, bad);
    bar0.rom = bad;
    NvBiosSources s;
    s.registers = &bar0;
    s.chip = kepler;
    s.allocator = &alloc;
    CHECK(NvBiosShadow::fetch(s, &out, &report) == BiosSource::Pramin && report.attempts[1].score == 2 + 4 && report.attempts[0].score == 3 + 4);
  }
  {
    // PRAMIN not usable: before NV50, display fused off, window disabled, window in system memory.
    FakeBar0 bar0;
    setup_pramin(bar0, good);
    NvBiosSources s;
    s.registers = &bar0;
    s.allocator = &alloc;
    s.chip = chip_of(0x046000a1);
    bar0.rom = good;
    CHECK(NvBiosShadow::fetch(s, &out, &report) == BiosSource::Prom && !strstr(report.attempts[0].reason, "ok"));
    CHECK(bar0.regs[0x088050] == 1);
    s.chip = chip_of(0x04e000a1);   // C51: no PROM either
    CHECK(NvBiosShadow::fetch(s, &out, &report) == BiosSource::None && report.attempts[1].score == 0);
    s.chip = kepler;
    bar0.regs[0x022500] = 1;
    bar0.rom.clear();
    CHECK(NvBiosShadow::fetch(s, &out, &report) == BiosSource::None && strstr(report.attempts[0].reason, "fused"));
    bar0.regs[0x022500] = 0;
    bar0.regs[0x619f04] &= ~0x8u;
    CHECK(NvBiosShadow::fetch(s, &out, &report) == BiosSource::None && strstr(report.attempts[0].reason, "not enabled"));
    bar0.regs[0x619f04] |= 0x8u;
    bar0.regs[0x619f04] = (bar0.regs[0x619f04] & ~3u) | 2u;
    CHECK(NvBiosShadow::fetch(s, &out, &report) == BiosSource::None && strstr(report.attempts[0].reason, "not in VRAM"));
    // Maxwell reads the fuse at 0x21c04, Volta the window at 0x625f04; an NV04-era chip has its PCI mirror at 0x1800.
    FakeBar0 gm;
    setup_pramin(gm, good);
    gm.regs[0x021c04] = 1;
    s.registers = &gm;
    s.chip = chip_of(0x124000a1);
    CHECK(NvBiosShadow::fetch(s, &out, &report) == BiosSource::None);
    gm.regs[0x021c04] = 0;
    CHECK(NvBiosShadow::fetch(s, &out, &report) == BiosSource::Pramin);
    s.chip = chip_of(0x140000a1);
    CHECK(NvBiosShadow::fetch(s, &out, &report) == BiosSource::None);   // (0x625f04 not set)
    gm.regs[0x625f04] = gm.regs[0x619f04];
    CHECK(NvBiosShadow::fetch(s, &out, &report) == BiosSource::Pramin);
    s.chip = chip_of(0x170000a1);   // GA100: no display, no PRAMIN copy
    CHECK(NvBiosShadow::fetch(s, &out, &report) == BiosSource::None);
    FakeBar0 nv20;
    nv20.pci_mirror = 0x1800;
    nv20.rom = good;
    nv20.regs[0x1850] = 1;
    s.registers = &nv20;
    s.chip = chip_of(0x025000a2);
    CHECK(NvBiosShadow::fetch(s, &out, &report) == BiosSource::Prom && nv20.regs[0x1850] == 1);
  }
  {
    // ACPI: the fast path (one call); a firmware that returns only 4 KiB per call (the slow path picks it up); junk.
    FakeAcpi acpi;
    acpi.rom = good;
    NvBiosSources s;
    s.chip = kepler;
    s.acpi_rom = &acpi;
    s.allocator = &alloc;
    CHECK(NvBiosShadow::fetch(s, &out, &report) == BiosSource::AcpiFast && same(out, good) && !report.attempts[3].tried);
    acpi.fast_broken = true;
    CHECK(NvBiosShadow::fetch(s, &out, &report) == BiosSource::AcpiSlow && same(out, good) && report.attempts[2].score == 0);
    acpi.fast_broken = false;
    acpi.rom[0x1000] ^= 1;   // a bad checksum: the fast path refuses it (require_checksum), the slow path keeps it at 2 per image
    CHECK(NvBiosShadow::fetch(s, &out, &report) == BiosSource::AcpiSlow && report.attempts[2].score == 0 && report.attempts[3].score == 2 + 4);
    acpi.rom.assign(good.begin(), good.begin() + 0x2000);   // a short image: the method has no more
    CHECK(NvBiosShadow::fetch(s, &out, &report) == BiosSource::None && strstr(report.attempts[3].reason, "shorter"));
    acpi.rom.assign(0x3000, 0xEE);
    CHECK(NvBiosShadow::fetch(s, &out, &report) == BiosSource::None && out.size() == 0);
  }
  {
    // The expansion ROM BAR: tried only when nothing else has an image; the BAR is probed, enabled, and left as it was.
    RomPci pci;
    pci.rom_register = 0xE0000000;
    pci.size_mask = 0xFFFE0000;   // 128 KiB
    FakeMapper mapper;
    mapper.rom_window = good;
    mapper.rom_window.resize(0x20000, 0xff);
    NvBiosSources s;
    s.chip = kepler;
    s.pci = &pci;
    s.mapper = &mapper;
    s.allocator = &alloc;
    CHECK(NvBiosShadow::fetch(s, &out, &report) == BiosSource::PciRom && same(out, good));
    CHECK(pci.rom_register == 0xE0000000 && pci.writes.size() == 4 && (pci.writes[2] & 1) == 1);
    pci.rom_register = 0;   // no ROM base: the legacy shadow, if this is the boot display and the device ID matches
    mapper.legacy = good;
    mapper.legacy.resize(0x20000, 0);
    CHECK(NvBiosShadow::fetch(s, &out, &report) == BiosSource::None && strstr(report.attempts[5].reason, "boot display"));
    s.boot_display = true;
    s.device_id = 0x1180;
    CHECK(NvBiosShadow::fetch(s, &out, &report) == BiosSource::Platform && same(out, good));
    s.device_id = 0x1184;   // another card's image
    CHECK(NvBiosShadow::fetch(s, &out, &report) == BiosSource::None && strstr(report.attempts[5].reason, "another device"));
  }
  {
    // A short-buffer source and an absurd chain: images that claim more than the 1 MiB window, eight chained images without a last one.
    Bytes huge = good;
    put16(huge, 0x60 + 8, 0x0900);   // 0x900 * 512 = 1.125 MiB
    FakeAcpi acpi;
    acpi.rom = huge;
    NvBiosSources s;
    s.chip = kepler;
    s.acpi_rom = &acpi;
    s.allocator = &alloc;
    CHECK(NvBiosShadow::fetch(s, &out, &report) == BiosSource::None);
    Bytes chain;
    for (int i = 0; i < 10; ++i) rom_header(chain, i * 0x200, 1, 0x03, false);   // never the last: stops after kMaxImages
    put16(chain, 0x44, 0x10de);
    acpi.rom = chain;
    CHECK(NvBiosShadow::fetch(s, &out, &report) == BiosSource::AcpiFast && report.score == 4 * NvBiosShadow::kMaxImages);
  }
  // No allocator: nothing.
  NvBiosSources none;
  CHECK(NvBiosShadow::fetch(none, &out, nullptr) == BiosSource::None);
  for (uint32_t i = 0; i <= kBiosSourceCount; ++i) CHECK(bios_source_name(static_cast<BiosSource>(i))[0] != 0);
}

// --- The ACPI _ROM method through the AML interpreter (tables assembled byte by byte, as AmdVbiosTest does) -------------------------------

namespace aml_asm {
static Bytes cat(std::initializer_list<Bytes> parts) { Bytes r; for (const Bytes& p : parts) r.insert(r.end(), p.begin(), p.end()); return r; }
static Bytes b(std::initializer_list<uint8_t> v) { return Bytes(v); }
static Bytes path(const char* s) {
  Bytes r;
  if (*s == '\\') { r.push_back(0x5C); ++s; }
  std::vector<Bytes> segs;
  while (*s) {
    Bytes sg(4, '_');
    int n = 0;
    while (*s && *s != '.') sg[n++] = static_cast<uint8_t>(*s++);
    segs.push_back(sg);
    if (*s == '.') ++s;
  }
  if (segs.size() == 2) r.push_back(0x2E);
  else if (segs.size() > 2) { r.push_back(0x2F); r.push_back(static_cast<uint8_t>(segs.size())); }
  for (const Bytes& sg : segs) r.insert(r.end(), sg.begin(), sg.end());
  return r;
}
static Bytes pkg(const Bytes& body) {
  Bytes r;
  const uint32_t n = static_cast<uint32_t>(body.size());
  if (n + 1 < 64) r.push_back(static_cast<uint8_t>(n + 1));
  else if (n + 2 < 4096) { const uint32_t t = n + 2; r.push_back(static_cast<uint8_t>(0x40 | (t & 0xF))); r.push_back(static_cast<uint8_t>(t >> 4)); }
  else { const uint32_t t = n + 3; r.push_back(static_cast<uint8_t>(0x80 | (t & 0xF))); r.push_back(static_cast<uint8_t>(t >> 4)); r.push_back(static_cast<uint8_t>(t >> 12)); }
  r.insert(r.end(), body.begin(), body.end());
  return r;
}
static Bytes num(uint64_t v) {
  if (v == 0) return b({0x00});
  if (v == 1) return b({0x01});
  if (v <= 0xFF) return b({0x0A, static_cast<uint8_t>(v)});
  if (v <= 0xFFFF) return b({0x0B, static_cast<uint8_t>(v), static_cast<uint8_t>(v >> 8)});
  return b({0x0C, static_cast<uint8_t>(v), static_cast<uint8_t>(v >> 8), static_cast<uint8_t>(v >> 16), static_cast<uint8_t>(v >> 24)});
}
static Bytes name_def(const char* n, const Bytes& value) { return cat({b({0x08}), path(n), value}); }
static Bytes method(const char* n, int argc, const Bytes& body) { return cat({b({0x14}), pkg(cat({path(n), b({static_cast<uint8_t>(argc)}), body}))}); }
static Bytes device(const char* n, const Bytes& body) { return cat({b({0x5B, 0x82}), pkg(cat({path(n), body}))}); }
static Bytes scope(const char* n, const Bytes& body) { return cat({b({0x10}), pkg(cat({path(n), body}))}); }
static Bytes buffer(const Bytes& size, const Bytes& init) { return cat({b({0x11}), pkg(cat({size, init}))}); }
static Bytes ret(const Bytes& v) { return cat({b({0xA4}), v}); }
static Bytes if_(const Bytes& cond, const Bytes& body) { return cat({b({0xA0}), pkg(cat({cond, body}))}); }
static Bytes lequal(const Bytes& a, const Bytes& c) { return cat({b({0x93}), a, c}); }
static Bytes make_table(const char* signature, const Bytes& aml) {
  Bytes t(36, 0);
  memcpy(&t[0], signature, 4);
  const uint32_t length = static_cast<uint32_t>(36 + aml.size());
  for (int i = 0; i < 4; ++i) t[4 + i] = static_cast<uint8_t>(length >> (8 * i));
  t[8] = 2;
  t.insert(t.end(), aml.begin(), aml.end());
  return t;
}
}  // namespace aml_asm

struct NoRegions final : acpi::IAmlHost {
  bool region_read(const acpi::RegionAccess&, uint64_t*) override { return false; }
  bool region_write(const acpi::RegionAccess&, uint64_t) override { return false; }
};

// A device with _ADR `adr` whose _ROM(offset, length) returns `image` for offset 0 (whatever the length) and an empty buffer otherwise.
static Bytes rom_device(const char* name, uint32_t adr, const Bytes& image) {
  using namespace aml_asm;
  return device(name, cat({name_def("_ADR", num(adr)), name_def("P0__", buffer(num(image.size()), image)),
                           method("_ROM", 2, cat({if_(lequal(b({0x68}), num(0)), ret(path("P0__"))), ret(buffer(num(0), Bytes()))}))}));
}

static void test_aml_rom() {
  using namespace aml_asm;
  TestAllocator alloc;
  NoRegions host;
  const Bytes image = nv04_image();   // 4 KiB: one page
  Bytes other = image;
  other[1] = 0;
  // Another device's _ROM (the integrated GPU at 00:02.0) comes first in the namespace; the right one is matched by _ADR.
  const Bytes table = make_table("DSDT", scope("\\_SB", device("PCI0", cat({rom_device("IGPU", 0x20000, other), device("PEG0",
                                                                                rom_device("GFX0", 0x00000, image))}))));
  acpi::AmlInterpreter vm(alloc, host);
  CHECK(vm.load_table(view(table)));
  AmlNvAcpiRom rom;
  CHECK(!rom.find(vm, PciAddress{1, 3, 0}) && !rom.valid());   // nobody has _ADR 0x30000
  CHECK(rom.find(vm, PciAddress{1, 0, 0}) && rom.valid());
  uint8_t page[0x1000] = {};
  CHECK(rom.read_rom(0, ByteView(page, sizeof(page))) == 0x1000 && ConstByteView(page, sizeof(page)).equals(view(image)));
  CHECK(rom.read_rom(0x1000, ByteView(page, sizeof(page))) == 0);
  ByteBuffer out(alloc);
  NvBiosFetchReport report;
  NvBiosSources s;
  s.chip = chip_of(0x20004000);
  s.acpi_rom = &rom;
  s.allocator = &alloc;
  CHECK(NvBiosShadow::fetch(s, &out, &report) == BiosSource::AcpiFast && same(out, image));
  CHECK(rom.find(vm, PciAddress{0, 2, 0}));   // the other one: a broken signature
  CHECK(NvBiosShadow::fetch(s, &out, &report) == BiosSource::None && strstr(report.attempts[2].reason, "signature"));
  AmlNvAcpiRom unset;
  CHECK(unset.read_rom(0, ByteView(page, sizeof(page))) == 0);
}

// Every prefix and bit flips through the whole fetch path (ACPI fake), then parsing what it accepted.
static void fuzz_fetch(const Bytes& image) {
  TestAllocator alloc;
  ByteBuffer out(alloc);
  FakeAcpi acpi;
  NvBiosSources s;
  s.chip = chip_of(0x0e4000a2);
  s.acpi_rom = &acpi;
  s.allocator = &alloc;
  BoardInfo board;
  for (size_t length = 0; length <= image.size(); length += 97) {
    acpi.rom.assign(image.begin(), image.begin() + length);
    if (NvBiosShadow::fetch(s, &out, nullptr) != BiosSource::None) (void)parse_board(NvBios(out.view()), s.chip, &board);
  }
  uint32_t state = 77;
  for (int round = 0; round < 300; ++round) {
    acpi.rom = image;
    state = state * 1103515245u + 12345u;
    acpi.rom[(state >> 8) % 0x100] ^= static_cast<uint8_t>(1u << (state % 8));   // the headers
    if (NvBiosShadow::fetch(s, &out, nullptr) != BiosSource::None) (void)parse_board(NvBios(out.view()), s.chip, &board);
  }
}

// --- Devinit --------------------------------------------------------------------------------------------------------------------------------

static void test_devinit() {
  FakeBar0 bar0;
  CHECK(NvDevinit::detect(bar0, chip_of(0x0e4000a2)).state == PostState::NotPosted);
  bar0.regs[0x2240c] = 2;
  CHECK(NvDevinit::detect(bar0, chip_of(0x0e4000a2)).state == PostState::Posted);
  PostStatus tu = NvDevinit::detect(bar0, chip_of(0x164000a1));
  CHECK(tu.state == PostState::Posted && tu.devinit_complete_known && !tu.devinit_complete);
  bar0.regs[0x118128] = 1;
  bar0.regs[0x118234] = 0xff;
  CHECK(NvDevinit::detect(bar0, chip_of(0x164000a1)).devinit_complete);
  CHECK(NvDevinit::detect(bar0, chip_of(0x092000a2)).state == PostState::Unknown);
  CHECK(NvDevinit::detect(bar0, chip_of(0x12b000a1)).state == PostState::Posted);   // Tegra
  CHECK(NvDevinit::detect(bar0, chip_of(0xFFFFFFFF)).state == PostState::Unknown);
  bar0.regs[0x2240c] = 0xFFFFFFFF;
  CHECK(NvDevinit::detect(bar0, chip_of(0x0e4000a2)).state == PostState::Unknown);
  CHECK(strcmp(NvDevinit::state_name(PostState::Posted), "POSTed") == 0);
}

int main() {
  test_identify();
  test_kepler_parse();
  test_dcb30_parse();
  test_dcb22_parse();
  test_nv04_parse();
  test_bad_tables();
  fuzz_image(kepler_image(), chip_of(0x0e4000a2), 1);
  fuzz_image(dcb30_image(), chip_of(0x046000a1), 2);
  fuzz_image(dcb22_image(), chip_of(0x017000a1), 3);
  fuzz_image(nv04_image(), chip_of(0x20004000), 4);
  test_fetch();
  test_aml_rom();
  fuzz_fetch(kepler_image());
  test_devinit();
  if (g_failed) {
    printf("NvBiosTest: %d FAILED\n", g_failed);
    return 1;
  }
  printf("NvBiosTest: all passed\n");
  return 0;
}
