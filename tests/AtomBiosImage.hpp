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

// A small assembler for AtomBIOS images, for the interpreter's tests and the differential run against the Linux driver's interpreter (scripts/atom_differential): command tables
// are built from operations with their operands, the image gets a ROM header, the lists of command and data tables, a firmware info table and indirect I/O programs.
#pragma once
#include <stdint.h>
#include <string.h>
#include <vector>

namespace atomimage {

// Operand kinds and alignments, spelled out here (not taken from the code under test).
enum { REG = 0, PS = 1, WS = 2, FB = 3, ID = 4, IMM = 5, PLL = 6, MC = 7 };
enum { DWORD = 0, WORD0 = 1, WORD8 = 2, WORD16 = 3, BYTE0 = 4, BYTE8 = 5, BYTE16 = 6, BYTE24 = 7 };
// The opcode numbers of the driver's table, by operation; add kind() to the first of the six.
enum { MOVE = 1, AND = 7, OR = 13, SHL_OP = 19, SHR_OP = 25, MUL = 31, DIV = 37, ADD = 43, SUB = 49, SETPORT_ATI = 55, SETPORT_PCI = 56, SETPORT_SYSIO = 57, SETREGBLOCK = 58,
       SETFBBASE = 59, COMPARE = 60, SWITCH = 66, JUMP = 67, JUMP_EQUAL = 68, JUMP_BELOW = 69, JUMP_ABOVE = 70, JUMP_BELOWEQ = 71, JUMP_ABOVEEQ = 72, JUMP_NOTEQUAL = 73,
       TEST = 74, DELAY_MS = 80, DELAY_US = 81, CALLTABLE = 82, REPEAT = 83, CLEAR = 84, NOP = 90, EOT = 91, MASK = 92, POSTCARD = 98, BEEP = 99, SETDATABLOCK = 102, XOR = 103,
       SHL = 109, SHR = 115, DEBUG_OP = 121, PROCESSDS = 122, MUL32_PS = 123, MUL32_WS = 124, DIV32_PS = 125, DIV32_WS = 126 };
inline int kind(int arg) { switch (arg) { case REG: return 0; case PS: return 1; case WS: return 2; case FB: return 3; case PLL: return 4; default: return 5; } }

struct Code {
  std::vector<uint8_t> b;
  Code& u8(uint32_t v) { b.push_back(static_cast<uint8_t>(v)); return *this; }
  Code& u16(uint32_t v) { u8(v); u8(v >> 8); return *this; }
  Code& u32(uint32_t v) { u16(v); u16(v >> 16); return *this; }
  size_t pos() const { return b.size(); }
  void patch16(size_t at, uint32_t v) { b[at] = static_cast<uint8_t>(v); b[at + 1] = static_cast<uint8_t>(v >> 8); }
  // an operand: registers and data table offsets are 16 bits, the rest 8; an immediate by its alignment
  Code& operand(int k, uint32_t v, int align = DWORD) {
    if (k == REG || k == ID) return u16(v);
    if (k == IMM) return align == DWORD ? u32(v) : (align <= WORD16 ? u16(v) : u8(v));
    return u8(v);
  }
  // a two-operand operation: opcode, attribute (source kind | source alignment << 3 | destination alignment << 6), destination, source
  Code& op2(int base, int dst_arg, uint32_t dst_index, int src_kind, uint32_t src, int src_align = DWORD, int dst_align = 0) {
    u8(base + kind(dst_arg));
    u8(src_kind | (src_align << 3) | (dst_align << 6));
    operand(dst_arg, dst_index);
    return operand(src_kind, src, src_align);
  }
  Code& move(int dst_arg, uint32_t dst, int src_kind, uint32_t src, int src_align = DWORD, int dst_align = 0) { return op2(MOVE, dst_arg, dst, src_kind, src, src_align, dst_align); }
  Code& eot() { return u8(EOT); }
};

struct Table {
  uint32_t index, ws, ps;
  Code code;
};

struct Iio {
  uint32_t port;
  std::vector<uint8_t> program;   // up to and not including the end
};

// An image: ROM header, the list of command tables, the list of data tables (firmware info, indirect I/O), then the tables.
struct Image {
  std::vector<Table> tables;
  std::vector<Iio> iio;
  uint32_t sclk = 30000, mclk = 40000;
  uint32_t revision = 0x01020304;
  bool with_firmware_info = true;
  uint32_t vram_usage_kb = 0;     // a VRAM_UsageByFirmware data table asking for this many kilobytes (0: none)
  uint32_t vram_usage_flags = 0;
  std::vector<uint8_t> extra_data;   // a data table at offset data_extra(), for ID operands

  static constexpr uint32_t kRom = 0x100, kCmd = 0x200, kData = 0x300, kFwi = 0x400, kIioAt = 0x500, kVram = 0x580, kExtra = 0x5A0, kTables = 0x600;
  static constexpr uint32_t kImageBytes = 0x4000;

  std::vector<uint8_t> build() const {
    std::vector<uint8_t> img(kImageBytes, 0);
    auto w8 = [&](uint32_t at, uint32_t v) { img[at] = static_cast<uint8_t>(v); };
    auto w16 = [&](uint32_t at, uint32_t v) { w8(at, v); w8(at + 1, v >> 8); };
    auto w32 = [&](uint32_t at, uint32_t v) { w16(at, v); w16(at + 2, v >> 16); };
    w16(0, 0xAA55);
    memcpy(&img[0x30], " 761295520", 10);
    memcpy(&img[0x80], "TESTBIOS", 8);   // (a part number: the driver's parser dereferences a null pointer without one)
    w16(0x48, kRom);
    memcpy(&img[kRom + 4], "ATOM", 4);
    w16(kRom + 0x1E, kCmd);
    w16(kRom + 0x20, kData);
    // data tables: index 4 firmware info, 11 VRAM usage, 23 indirect I/O, 40 the extra one
    if (with_firmware_info) w16(kData + 4 + 2 * 4, kFwi);
    if (vram_usage_kb != 0) w16(kData + 4 + 2 * 11, kVram);
    w16(kData + 4 + 2 * 23, kIioAt);
    if (!extra_data.empty()) w16(kData + 4 + 2 * 40, kExtra);
    w16(kFwi, 0x20);
    w32(kFwi + 4, revision);
    w32(kFwi + 8, sclk);
    w32(kFwi + 12, mclk);
    if (vram_usage_kb != 0) {
      w16(kVram, 16);
      w32(kVram + 4, vram_usage_flags);
      w16(kVram + 8, vram_usage_kb);
    }
    for (size_t i = 0; i < extra_data.size(); ++i) w8(kExtra + static_cast<uint32_t>(i), extra_data[i]);
    // indirect I/O programs: START, port, the program, END, then three bytes (the driver skips END's 3)
    uint32_t at = kIioAt + 4;
    for (const Iio& p : iio) {
      w8(at++, 1);
      w8(at++, p.port);
      for (uint8_t byte : p.program) w8(at++, byte);
      w8(at++, 9);
      at += 2;
    }
    // command tables: size u16, frev, crev, ws, ps, code
    uint32_t t = kTables;
    for (const Table& table : tables) {
      w16(kCmd + 4 + 2 * table.index, t);
      w16(t, 6 + static_cast<uint32_t>(table.code.b.size()));
      w8(t + 4, table.ws);
      w8(t + 5, table.ps);
      for (size_t i = 0; i < table.code.b.size(); ++i) w8(t + 6 + static_cast<uint32_t>(i), table.code.b[i]);
      t += 6 + static_cast<uint32_t>(table.code.b.size());
      t = (t + 3) & ~3u;
    }
    return img;
  }
  uint32_t table_at(uint32_t index) const {
    uint32_t t = kTables;
    for (const Table& table : tables) {
      if (table.index == index) return t;
      t += 6 + static_cast<uint32_t>(table.code.b.size());
      t = (t + 3) & ~3u;
    }
    return 0;
  }
};

}  // namespace atomimage
