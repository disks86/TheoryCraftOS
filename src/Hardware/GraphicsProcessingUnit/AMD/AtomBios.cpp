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

#include "Hardware/GraphicsProcessingUnit/AMD/AtomBios.hpp"

// Follows the Linux amdgpu driver's atom.c (Copyright 2008 Advanced Micro Devices, Inc.; MIT license; also in the radeon driver) and atom.h; scripts/check_atom_port.py checks the
// numbers against them. The opcode table is generated from the driver's (scripts/gen_atom_ops.py).

namespace hardware {

namespace {
constexpr uint32_t kBiosMagic = 0xAA55, kAtiMagicPtr = 0x30, kRomTablePtr = 0x48, kRomMagicPtr = 4, kRomCmdPtr = 0x1E, kRomDataPtr = 0x20;
const char kAtiMagic[] = " 761295520";
const char kRomMagic[] = "ATOM";
constexpr uint32_t kDataFwiPtr = 0xC, kDataIioPtr = 0x32, kDataVramUsageIndex = 11;   // VRAM_UsageByFirmware: the 12th entry of the master list of data tables
constexpr uint32_t kFwiRevisionPtr = 4, kFwiDefSclkPtr = 8, kFwiDefMclkPtr = 0xC;
constexpr uint32_t kCtSizePtr = 0, kCtWsPtr = 4, kCtPsPtr = 5, kCtPsMask = 0x7F, kCtCodePtr = 6;
constexpr uint32_t kOpCnt = 127, kOpEot = 91;
constexpr uint32_t kCaseMagic = 0x63, kCaseEnd = 0x5A5A;
constexpr uint32_t kMaxImageBytes = 16u << 20, kMaxScratchBytes = 4u << 20;

enum : int { kArgReg = 0, kArgPs, kArgWs, kArgFb, kArgId, kArgImm, kArgPll, kArgMc };
enum : int { kSrcDword = 0, kSrcWord0, kSrcWord8, kSrcWord16, kSrcByte0, kSrcByte8, kSrcByte16, kSrcByte24 };
enum : uint32_t { kWsQuotient = 0x40, kWsRemainder, kWsDataPtr, kWsShift, kWsOrMask, kWsAndMask, kWsFbWindow, kWsAttributes, kWsRegPtr };
enum : uint8_t { kIioNop = 0, kIioStart, kIioRead, kIioWrite, kIioClear, kIioSet, kIioMoveIndex, kIioMoveAttr, kIioMoveData, kIioEnd };
enum : int { kIoMm = 0, kIoPci = 1, kIoSysio = 2, kIoIio = 0x80 };
enum : int { kCondAbove = 0, kCondAboveOrEqual, kCondAlways, kCondBelow, kCondBelowOrEqual, kCondEqual, kCondNotEqual };
enum : int { kPortAti = 0, kPortPci, kPortSysio };
enum : int { kUnitMicrosecond = 0, kUnitMillisecond = 1 };

const uint32_t kArgMask[8] = {0xFFFFFFFF, 0xFFFF, 0xFFFF00, 0xFFFF0000, 0xFF, 0xFF00, 0xFF0000, 0xFF000000};
const int kArgShift[8] = {0, 0, 8, 16, 0, 8, 16, 24};
// translate the destination alignment field to the source alignment encoding
const int kDstToSrc[8][4] = {{0, 0, 0, 0}, {1, 2, 3, 0}, {1, 2, 3, 0}, {1, 2, 3, 0}, {4, 5, 6, 7}, {4, 5, 6, 7}, {4, 5, 6, 7}, {4, 5, 6, 7}};
const int kDefDst[8] = {0, 0, 1, 2, 0, 1, 2, 3};
const int kIioLen[10] = {1, 2, 3, 3, 3, 3, 4, 4, 4, 3};

struct OpEntry {
  AtomOp op;
  int arg;
};
const OpEntry kOpcodeTable[kOpCnt] = {
#include "Hardware/GraphicsProcessingUnit/AMD/AtomBiosOps.inc"
};

// C shifts by 32 or more are not defined; the tables can ask for them.
uint32_t shl32(uint32_t v, uint32_t s) { return s >= 32 ? 0 : v << s; }
uint32_t shr32(uint32_t v, uint32_t s) { return s >= 32 ? 0 : v >> s; }
// 0xFFFFFFFF >> (32 - width): a mask of `width` low bits
uint32_t low_bits(uint32_t width) { return width == 0 ? 0 : (width >= 32 ? 0xFFFFFFFFu : 0xFFFFFFFFu >> (32 - width)); }
}  // namespace

// One run of one command table (atom_exec_context).
struct AtomBios::Exec {
  ArrayView<uint32_t> ps;
  uint32_t ws[256] = {};
  uint32_t ws_size = 0;
  uint32_t ps_shift = 0;
  uint32_t start = 0;
  uint32_t last_jump = 0;
  uint64_t last_jump_us = 0;
  uint32_t depth = 0;
  bool abort = false;
  Status cause = Status::Ok;
};

const char* AtomBios::status_name(Status status) {
  switch (status) {
    case Status::Ok: return "ok";
    case Status::BadImage: return "not an AtomBIOS image";
    case Status::NoTable: return "no such command table";
    case Status::BadRead: return "a table reads beyond the image";
    case Status::Stuck: return "stuck in a loop";
    case Status::TooDeep: return "tables call tables too deep";
    case Status::TooLong: return "too many operations";
    case Status::CalledTableFailed: return "a called table failed";
    case Status::NoClocks: return "the firmware info has no default clocks";
  }
  return "unknown";
}

bool AtomBios::in_image(uint32_t index, uint32_t bytes) {
  if (uint64_t{index} + bytes > bios_.size()) {
    if (!failed()) fail_ = Status::BadRead;
    return false;
  }
  return true;
}

uint8_t AtomBios::cu8(uint32_t index) {
  uint8_t v = 0;
  if (!bios_.try_get(index, &v) && !failed()) fail_ = Status::BadRead;
  return v;
}

uint16_t AtomBios::cu16(uint32_t index) {
  if (!in_image(index, 2)) return 0;
  return static_cast<uint16_t>(bios_[index] | (bios_[index + 1] << 8));
}

uint32_t AtomBios::cu32(uint32_t index) {
  if (!in_image(index, 4)) return 0;
  return bios_[index] | (bios_[index + 1] << 8) | (bios_[index + 2] << 16) | (uint32_t{bios_[index + 3]} << 24);
}

uint32_t AtomBios::scratch_read(uint32_t word) {
  if (uint64_t{word} * 4 + 4 > scratch_.size()) return 0;
  const ConstByteView v = scratch_.view();
  return v[word * 4] | (v[word * 4 + 1] << 8) | (v[word * 4 + 2] << 16) | (uint32_t{v[word * 4 + 3]} << 24);
}

bool AtomBios::scratch_write(uint32_t word, uint32_t value) {
  if (uint64_t{word} * 4 + 4 > scratch_.size()) return false;
  ByteView v = scratch_.writable_view();
  v[word * 4] = static_cast<uint8_t>(value);
  v[word * 4 + 1] = static_cast<uint8_t>(value >> 8);
  v[word * 4 + 2] = static_cast<uint8_t>(value >> 16);
  v[word * 4 + 3] = static_cast<uint8_t>(value >> 24);
  return true;
}

uint32_t AtomBios::scratch_word(uint32_t index) const { return const_cast<AtomBios*>(this)->scratch_read(index); }
void AtomBios::set_scratch_word(uint32_t index, uint32_t value) { scratch_write(index, value); }

// atom_iio_execute: an indirect I/O program of the BIOS (a register read or write through an index and data pair), run on a temporary.
uint32_t AtomBios::iio_execute(uint32_t base, uint32_t index, uint32_t data) {
  uint32_t temp = 0xCDCDCDCD;
  for (;;) {
    if (failed()) return 0;
    if (++operations_ > max_operations_) {
      fail_ = Status::TooLong;
      return 0;
    }
    switch (cu8(base)) {
      case kIioNop:
        base++;
        break;
      case kIioRead:
        temp = host_.read_register(cu16(base + 1));
        base += 3;
        break;
      case kIioWrite:
        host_.write_register(cu16(base + 1), temp);
        base += 3;
        break;
      case kIioClear:
        temp &= ~shl32(low_bits(cu8(base + 1)), cu8(base + 2));
        base += 3;
        break;
      case kIioSet:
        temp |= shl32(low_bits(cu8(base + 1)), cu8(base + 2));
        base += 3;
        break;
      case kIioMoveIndex:
        temp &= ~shl32(low_bits(cu8(base + 1)), cu8(base + 3));
        temp |= shl32(shr32(index, cu8(base + 2)) & low_bits(cu8(base + 1)), cu8(base + 3));
        base += 4;
        break;
      case kIioMoveData:
        temp &= ~shl32(low_bits(cu8(base + 1)), cu8(base + 3));
        temp |= shl32(shr32(data, cu8(base + 2)) & low_bits(cu8(base + 1)), cu8(base + 3));
        base += 4;
        break;
      case kIioMoveAttr:
        temp &= ~shl32(low_bits(cu8(base + 1)), cu8(base + 3));
        temp |= shl32(shr32(io_attr_, cu8(base + 2)) & low_bits(cu8(base + 1)), cu8(base + 3));
        base += 4;
        break;
      case kIioEnd:
        return temp;
      default:   // Unknown IIO opcode
        ++warnings_;
        return 0;
    }
  }
}

// atom_index_iio: where each indirect I/O program starts. A program of the BIOS this does not understand makes the image unusable (the driver would read on).
void AtomBios::index_iio(uint32_t base) {
  while (cu8(base) == kIioStart && !failed()) {
    if (base + 2 > 0xFFFF) {
      fail_ = Status::BadImage;
      return;
    }
    iio_[cu8(base + 1)] = static_cast<uint16_t>(base + 2);
    base += 2;
    while (cu8(base) != kIioEnd) {
      const uint8_t op = cu8(base);
      if (op > kIioEnd || failed()) {
        fail_ = Status::BadImage;
        return;
      }
      base += kIioLen[op];
    }
    base += 3;
  }
}

// atom_get_src_int
uint32_t AtomBios::get_src_int(Exec& e, uint8_t attr, uint32_t* ptr, uint32_t* saved) {
  uint32_t idx, val = 0xCDCDCDCD;
  const uint32_t arg = attr & 7, align = (attr >> 3) & 7;
  switch (arg) {
    case kArgReg:
      idx = cu16(*ptr);
      *ptr += 2;
      idx += reg_block_;
      switch (io_mode_) {
        case kIoMm:
          val = host_.read_register(idx);
          break;
        case kIoPci:   // PCI registers are not implemented
        case kIoSysio:   // SYSIO registers are not implemented
          ++warnings_;
          return 0;
        default:
          if (!(io_mode_ & kIoIio)) {   // Bad IO mode
            ++warnings_;
            return 0;
          }
          if (!iio_[io_mode_ & 0x7F]) {   // Undefined indirect IO read method
            ++warnings_;
            return 0;
          }
          val = iio_execute(iio_[io_mode_ & 0x7F], idx, 0);
      }
      break;
    case kArgPs:
      idx = cu8(*ptr);
      (*ptr)++;
      if (idx < e.ps.size()) val = e.ps[idx];
      else ++warnings_;   // PS index out of range
      break;
    case kArgWs:
      idx = cu8(*ptr);
      (*ptr)++;
      switch (idx) {
        case kWsQuotient: val = divmul_[0]; break;
        case kWsRemainder: val = divmul_[1]; break;
        case kWsDataPtr: val = data_block_; break;
        case kWsShift: val = shift_; break;
        case kWsOrMask: val = shl32(1, shift_); break;
        case kWsAndMask: val = ~shl32(1, shift_); break;
        case kWsFbWindow: val = fb_base_; break;
        case kWsAttributes: val = io_attr_; break;
        case kWsRegPtr: val = reg_block_; break;
        default:
          if (idx < e.ws_size) val = e.ws[idx];
          else ++warnings_;   // WS index out of range
      }
      break;
    case kArgId:
      idx = cu16(*ptr);
      *ptr += 2;
      val = cu32(idx + data_block_);
      break;
    case kArgFb:
      idx = cu8(*ptr);
      (*ptr)++;
      if (uint64_t{fb_base_ / 4} + idx + 1 > scratch_.size() / 4) {   // fb read beyond scratch region
        ++warnings_;
        val = 0;
      } else {
        val = scratch_read(fb_base_ / 4 + idx);
      }
      break;
    case kArgImm:
      switch (align) {
        case kSrcDword:
          val = cu32(*ptr);
          *ptr += 4;
          break;
        case kSrcWord0:
        case kSrcWord8:
        case kSrcWord16:
          val = cu16(*ptr);
          *ptr += 2;
          break;
        default:   // the byte alignments
          val = cu8(*ptr);
          (*ptr)++;
          break;
      }
      return val;
    case kArgPll:
      idx = cu8(*ptr);
      (*ptr)++;
      val = host_.read_pll(idx);
      break;
    case kArgMc:
      idx = cu8(*ptr);
      (*ptr)++;
      val = host_.read_mc(idx);
      break;
  }
  if (saved) *saved = val;
  val &= kArgMask[align];
  val >>= kArgShift[align];
  return val;
}

// atom_skip_src_int
void AtomBios::skip_src_int(uint8_t attr, uint32_t* ptr) {
  const uint32_t align = (attr >> 3) & 7, arg = attr & 7;
  switch (arg) {
    case kArgReg:
    case kArgId:
      *ptr += 2;
      break;
    case kArgPll:
    case kArgMc:
    case kArgPs:
    case kArgWs:
    case kArgFb:
      (*ptr)++;
      break;
    case kArgImm:
      switch (align) {
        case kSrcDword: *ptr += 4; return;
        case kSrcWord0:
        case kSrcWord8:
        case kSrcWord16: *ptr += 2; return;
        default: (*ptr)++; return;
      }
  }
}

// atom_get_src_direct
uint32_t AtomBios::get_src_direct(uint8_t align, uint32_t* ptr) {
  uint32_t val = 0xCDCDCDCD;
  switch (align) {
    case kSrcDword:
      val = cu32(*ptr);
      *ptr += 4;
      break;
    case kSrcWord0:
    case kSrcWord8:
    case kSrcWord16:
      val = cu16(*ptr);
      *ptr += 2;
      break;
    default:
      val = cu8(*ptr);
      (*ptr)++;
      break;
  }
  return val;
}

uint32_t AtomBios::get_dst(Exec& e, int arg, uint8_t attr, uint32_t* ptr, uint32_t* saved) {
  return get_src_int(e, static_cast<uint8_t>(arg | (kDstToSrc[(attr >> 3) & 7][(attr >> 6) & 3] << 3)), ptr, saved);
}

void AtomBios::skip_dst(int arg, uint8_t attr, uint32_t* ptr) {
  skip_src_int(static_cast<uint8_t>(arg | (kDstToSrc[(attr >> 3) & 7][(attr >> 6) & 3] << 3)), ptr);
}

// atom_put_dst
void AtomBios::put_dst(Exec& e, int arg, uint8_t attr, uint32_t* ptr, uint32_t val, uint32_t saved) {
  const uint32_t align = kDstToSrc[(attr >> 3) & 7][(attr >> 6) & 3];
  uint32_t idx;
  val <<= kArgShift[align];
  val &= kArgMask[align];
  saved &= ~kArgMask[align];
  val |= saved;
  switch (arg) {
    case kArgReg:
      idx = cu16(*ptr);
      *ptr += 2;
      idx += reg_block_;
      switch (io_mode_) {
        case kIoMm:
          if (idx == 0) host_.write_register(idx, val << 2);
          else host_.write_register(idx, val);
          break;
        case kIoPci:   // PCI registers are not implemented
        case kIoSysio:   // SYSIO registers are not implemented
          ++warnings_;
          return;
        default:
          if (!(io_mode_ & kIoIio)) {   // Bad IO mode
            ++warnings_;
            return;
          }
          // (the driver masks the index with 0xFF here and with 0x7F for a read: ported as it is)
          if (!iio_[io_mode_ & 0xFF]) {   // Undefined indirect IO write method
            ++warnings_;
            return;
          }
          iio_execute(iio_[io_mode_ & 0xFF], idx, val);
      }
      break;
    case kArgPs:
      idx = cu8(*ptr);
      (*ptr)++;
      if (idx >= e.ps.size()) {   // PS index out of range
        ++warnings_;
        return;
      }
      e.ps[idx] = val;
      break;
    case kArgWs:
      idx = cu8(*ptr);
      (*ptr)++;
      switch (idx) {
        case kWsQuotient: divmul_[0] = val; break;
        case kWsRemainder: divmul_[1] = val; break;
        case kWsDataPtr: data_block_ = static_cast<uint16_t>(val); break;
        case kWsShift: shift_ = static_cast<uint8_t>(val); break;
        case kWsOrMask:
        case kWsAndMask: break;
        case kWsFbWindow: fb_base_ = val; break;
        case kWsAttributes: io_attr_ = static_cast<uint16_t>(val); break;
        case kWsRegPtr: reg_block_ = static_cast<uint16_t>(val); break;
        default:
          if (idx >= e.ws_size) {   // WS index out of range
            ++warnings_;
            return;
          }
          e.ws[idx] = val;
      }
      break;
    case kArgFb:
      idx = cu8(*ptr);
      (*ptr)++;
      if (!scratch_write(fb_base_ / 4 + idx, val)) ++warnings_;   // fb write beyond scratch region
      break;
    case kArgPll:
      idx = cu8(*ptr);
      (*ptr)++;
      host_.write_pll(idx, val);
      break;
    case kArgMc:
      idx = cu8(*ptr);
      (*ptr)++;
      host_.write_mc(idx, val);
      return;
  }
}

void AtomBios::run_op(Exec& e, AtomOp op, int arg, uint32_t* ptr) {
  switch (op) {
    case AtomOp::None: break;
    case AtomOp::Add:
    case AtomOp::And:
    case AtomOp::Or:
    case AtomOp::Sub:
    case AtomOp::Xor: {
      const uint8_t attr = cu8((*ptr)++);
      uint32_t saved = 0xCDCDCDCD;
      uint32_t dptr = *ptr;
      uint32_t dst = get_dst(e, arg, attr, ptr, &saved);
      const uint32_t src = get_src(e, attr, ptr);
      switch (op) {
        case AtomOp::Add: dst += src; break;
        case AtomOp::And: dst &= src; break;
        case AtomOp::Or: dst |= src; break;
        case AtomOp::Sub: dst -= src; break;
        default: dst ^= src; break;
      }
      put_dst(e, arg, attr, &dptr, dst, saved);
      break;
    }
    case AtomOp::Beep: break;   // ATOM BIOS beeped!
    case AtomOp::CallTable: {
      const uint32_t idx = cu8((*ptr)++);
      Status r = Status::Ok;
      if (cu16(cmd_table_ + 4 + 2 * idx)) {
        ArrayView<uint32_t> sub;
        if (e.ps_shift <= e.ps.size()) sub = e.ps.subview(e.ps_shift);
        r = execute_locked(idx, sub, e.depth + 1);
      }
      if (r != Status::Ok) {
        e.abort = true;
        e.cause = r;
      }
      break;
    }
    case AtomOp::Clear: {
      uint8_t attr = cu8((*ptr)++);
      uint32_t saved = 0xCDCDCDCD;
      uint32_t dptr = *ptr;
      attr &= 0x38;
      attr |= kDefDst[attr >> 3] << 6;
      get_dst(e, arg, attr, ptr, &saved);
      put_dst(e, arg, attr, &dptr, 0, saved);
      break;
    }
    case AtomOp::Compare: {
      const uint8_t attr = cu8((*ptr)++);
      const uint32_t dst = get_dst(e, arg, attr, ptr, nullptr);
      const uint32_t src = get_src(e, attr, ptr);
      cs_equal_ = (dst == src);
      cs_above_ = (dst > src);
      break;
    }
    case AtomOp::Delay: {
      const uint32_t count = cu8((*ptr)++);
      if (arg == kUnitMicrosecond) host_.delay_us(count);
      else host_.delay_ms(count);
      break;
    }
    case AtomOp::Div: {
      const uint8_t attr = cu8((*ptr)++);
      const uint32_t dst = get_dst(e, arg, attr, ptr, nullptr);
      const uint32_t src = get_src(e, attr, ptr);
      if (src != 0) {
        divmul_[0] = dst / src;
        divmul_[1] = dst % src;
      } else {
        divmul_[0] = 0;
        divmul_[1] = 0;
      }
      break;
    }
    case AtomOp::Div32: {
      const uint8_t attr = cu8((*ptr)++);
      const uint32_t dst = get_dst(e, arg, attr, ptr, nullptr);
      const uint32_t src = get_src(e, attr, ptr);
      if (src != 0) {
        uint64_t val64 = dst;
        val64 |= static_cast<uint64_t>(divmul_[1]) << 32;
        val64 /= src;
        divmul_[0] = static_cast<uint32_t>(val64);
        divmul_[1] = static_cast<uint32_t>(val64 >> 32);
      } else {
        divmul_[0] = 0;
        divmul_[1] = 0;
      }
      break;
    }
    case AtomOp::Eot: break;
    case AtomOp::Jump: {
      uint32_t target = cu16(*ptr);
      (*ptr) += 2;
      bool execute = false;
      switch (arg) {
        case kCondAbove: execute = cs_above_; break;
        case kCondAboveOrEqual: execute = cs_above_ || cs_equal_; break;
        case kCondAlways: execute = true; break;
        case kCondBelow: execute = !(cs_above_ || cs_equal_); break;
        case kCondBelowOrEqual: execute = !cs_above_; break;
        case kCondEqual: execute = cs_equal_; break;
        case kCondNotEqual: execute = !cs_equal_; break;
      }
      if (execute) {
        if (e.last_jump == e.start + target) {
          const uint64_t now = host_.now_us();
          if (now > e.last_jump_us) {
            if (now - e.last_jump_us > kLoopTimeoutUs) {   // atombios stuck in loop for more than 20 secs aborting
              e.abort = true;
              e.cause = Status::Stuck;
            }
          } else {
            e.last_jump_us = now;
          }
        } else {
          e.last_jump = e.start + target;
          e.last_jump_us = host_.now_us();
        }
        *ptr = e.start + target;
      }
      break;
    }
    case AtomOp::Mask: {
      const uint8_t attr = cu8((*ptr)++);
      uint32_t saved = 0xCDCDCDCD;
      uint32_t dptr = *ptr;
      uint32_t dst = get_dst(e, arg, attr, ptr, &saved);
      const uint32_t mask = get_src_direct((attr >> 3) & 7, ptr);
      const uint32_t src = get_src(e, attr, ptr);
      dst &= mask;
      dst |= src;
      put_dst(e, arg, attr, &dptr, dst, saved);
      break;
    }
    case AtomOp::Move: {
      const uint8_t attr = cu8((*ptr)++);
      uint32_t saved = 0xCDCDCDCD;
      uint32_t dptr = *ptr;
      if (((attr >> 3) & 7) != kSrcDword) {
        get_dst(e, arg, attr, ptr, &saved);
      } else {
        skip_dst(arg, attr, ptr);
        saved = 0xCDCDCDCD;
      }
      const uint32_t src = get_src(e, attr, ptr);
      put_dst(e, arg, attr, &dptr, src, saved);
      break;
    }
    case AtomOp::Mul: {
      const uint8_t attr = cu8((*ptr)++);
      const uint32_t dst = get_dst(e, arg, attr, ptr, nullptr);
      const uint32_t src = get_src(e, attr, ptr);
      divmul_[0] = dst * src;
      break;
    }
    case AtomOp::Mul32: {
      const uint8_t attr = cu8((*ptr)++);
      const uint32_t dst = get_dst(e, arg, attr, ptr, nullptr);
      const uint32_t src = get_src(e, attr, ptr);
      const uint64_t val64 = static_cast<uint64_t>(dst) * static_cast<uint64_t>(src);
      divmul_[0] = static_cast<uint32_t>(val64);
      divmul_[1] = static_cast<uint32_t>(val64 >> 32);
      break;
    }
    case AtomOp::Nop: break;
    case AtomOp::PostCard:
    case AtomOp::Debug:
      cu8((*ptr)++);
      break;
    case AtomOp::Repeat:
    case AtomOp::RestoreReg:
    case AtomOp::SaveReg:   // unimplemented, in the driver too
      break;
    case AtomOp::SetDataBlock: {
      const uint32_t idx = cu8(*ptr);
      (*ptr)++;
      if (!idx) data_block_ = 0;
      else if (idx == 255) data_block_ = static_cast<uint16_t>(e.start);
      else data_block_ = cu16(data_table_ + 4 + 2 * idx);
      break;
    }
    case AtomOp::SetFbBase: {
      const uint8_t attr = cu8((*ptr)++);
      fb_base_ = get_src(e, attr, ptr);
      break;
    }
    case AtomOp::SetPort:
      switch (arg) {
        case kPortAti: {
          const uint32_t port = cu16(*ptr);
          if (!port) io_mode_ = kIoMm;
          else io_mode_ = kIoIio | static_cast<int>(port);
          *ptr += 2;
          break;
        }
        case kPortPci:
          io_mode_ = kIoPci;
          (*ptr)++;
          break;
        case kPortSysio:
          io_mode_ = kIoSysio;
          (*ptr)++;
          break;
      }
      break;
    case AtomOp::SetRegBlock:
      reg_block_ = cu16(*ptr);
      *ptr += 2;
      break;
    case AtomOp::ShiftLeft:
    case AtomOp::ShiftRight: {
      uint8_t attr = cu8((*ptr)++);
      uint32_t saved = 0xCDCDCDCD;
      uint32_t dptr = *ptr;
      attr &= 0x38;
      attr |= kDefDst[attr >> 3] << 6;
      uint32_t dst = get_dst(e, arg, attr, ptr, &saved);
      const uint8_t shift = static_cast<uint8_t>(get_src_direct(kSrcByte0, ptr));
      dst = op == AtomOp::ShiftLeft ? shl32(dst, shift) : shr32(dst, shift);
      put_dst(e, arg, attr, &dptr, dst, saved);
      break;
    }
    case AtomOp::Shl:
    case AtomOp::Shr: {
      const uint8_t attr = cu8((*ptr)++);
      uint32_t saved = 0xCDCDCDCD;
      uint32_t dptr = *ptr;
      const uint32_t dst_align = kDstToSrc[(attr >> 3) & 7][(attr >> 6) & 3];
      uint32_t dst = get_dst(e, arg, attr, ptr, &saved);
      dst = saved;
      const uint8_t shift = static_cast<uint8_t>(get_src(e, attr, ptr));
      dst = op == AtomOp::Shl ? shl32(dst, shift) : shr32(dst, shift);
      dst &= kArgMask[dst_align];
      dst >>= kArgShift[dst_align];
      put_dst(e, arg, attr, &dptr, dst, saved);
      break;
    }
    case AtomOp::Switch: {
      const uint8_t attr = cu8((*ptr)++);
      const uint32_t src = get_src(e, attr, ptr);
      while (cu16(*ptr) != kCaseEnd && !failed()) {
        if (cu8(*ptr) == kCaseMagic) {
          (*ptr)++;
          const uint32_t val = get_src(e, (attr & 0x38) | kArgImm, ptr);
          const uint32_t target = cu16(*ptr);
          if (val == src) {
            *ptr = e.start + target;
            return;
          }
          *ptr += 2;
        } else {   // Bad case
          ++warnings_;
          return;
        }
      }
      *ptr += 2;
      break;
    }
    case AtomOp::Test: {
      const uint8_t attr = cu8((*ptr)++);
      const uint32_t dst = get_dst(e, arg, attr, ptr, nullptr);
      const uint32_t src = get_src(e, attr, ptr);
      cs_equal_ = ((dst & src) == 0);
      break;
    }
    case AtomOp::ProcessDs: {
      const uint32_t val = cu16(*ptr);
      *ptr += val + 2;
      break;
    }
  }
}

// amdgpu_atom_execute_table_locked
AtomBios::Status AtomBios::execute_locked(uint32_t index, ArrayView<uint32_t> params, uint32_t depth) {
  if (depth > kMaxDepth) return Status::TooDeep;
  const uint32_t base = cu16(cmd_table_ + 4 + 2 * index);
  if (failed()) return fail_;
  if (!base) return Status::NoTable;
  (void)cu16(base + kCtSizePtr);   // (the length is read and not used)
  const uint32_t ws = cu8(base + kCtWsPtr);
  const uint32_t ps = cu8(base + kCtPsPtr) & kCtPsMask;
  uint32_t ptr = base + kCtCodePtr;
  Exec e;
  e.ps = params;
  e.ps_shift = ps / 4;
  e.start = base;
  e.ws_size = ws;
  e.depth = depth;
  for (;;) {
    const uint8_t op = cu8(ptr++);
    if (e.abort) return e.cause != Status::Ok ? e.cause : Status::CalledTableFailed;   // atombios stuck executing
    if (failed()) return fail_;
    if (op < kOpCnt && op > 0) run_op(e, kOpcodeTable[op].op, kOpcodeTable[op].arg, &ptr);
    else break;
    if (op == kOpEot) break;
    if (++operations_ > max_operations_) {
      fail_ = Status::TooLong;
      return fail_;
    }
  }
  if (e.abort) return e.cause != Status::Ok ? e.cause : Status::CalledTableFailed;
  return failed() ? fail_ : Status::Ok;
}

AtomBios::Status AtomBios::execute_table(uint32_t index, ArrayView<uint32_t> params) {
  fail_ = Status::Ok;
  operations_ = 0;
  data_block_ = 0;
  reg_block_ = 0;
  fb_base_ = 0;
  io_mode_ = kIoMm;
  divmul_[0] = 0;
  divmul_[1] = 0;
  if (!valid_) return Status::BadImage;
  return execute_locked(index, params, 0);
}

AtomBios::Status AtomBios::asic_init() {
  fail_ = Status::Ok;
  if (!valid_) return Status::BadImage;
  const uint32_t hwi = cu16(data_table_ + kDataFwiPtr);
  uint32_t ps[16] = {};
  ps[0] = cu32(hwi + kFwiDefSclkPtr);
  ps[1] = cu32(hwi + kFwiDefMclkPtr);
  if (failed()) return fail_;
  if (!ps[0] || !ps[1]) return Status::NoClocks;
  if (!cu16(cmd_table_ + 4 + 2 * kCmdInit)) return Status::NoTable;
  const Status r = execute_table(kCmdInit, ArrayView<uint32_t>(ps, 16));
  for (uint32_t& word : ps) word = 0;
  return r;
}

bool AtomBios::data_table_header(uint32_t index, uint16_t* size, uint8_t* frev, uint8_t* crev, uint16_t* data_start) {
  fail_ = Status::Ok;
  if (index > 255) return false;
  const uint16_t idx = cu16(data_table_ + index * 2 + 4);
  if (failed() || !idx) return false;
  if (size) *size = cu16(idx);
  if (frev) *frev = cu8(idx + 2);
  if (crev) *crev = cu8(idx + 3);
  if (data_start) *data_start = idx;
  return !failed();
}

bool AtomBios::cmd_table_header(uint32_t index, uint8_t* frev, uint8_t* crev) {
  fail_ = Status::Ok;
  if (index > 255) return false;
  const uint16_t idx = cu16(cmd_table_ + index * 2 + 4);
  if (failed() || !idx) return false;
  if (frev) *frev = cu8(idx + 2);
  if (crev) *crev = cu8(idx + 3);
  return !failed();
}

// amdgpu_atom_parse, and amdgpu_atombios_allocate_fb_scratch for the scratch memory
AtomBios::AtomBios(IAtomHost& host, ConstByteView image, allocators::IAllocator& allocator) : host_(host), bios_(image), scratch_(allocator) {
  if (image.size() < 0x4A || image.size() > kMaxImageBytes) return;
  if (cu16(0) != kBiosMagic) return;   // Invalid BIOS magic
  for (uint32_t i = 0; i + 1 < sizeof kAtiMagic; ++i)
    if (cu8(kAtiMagicPtr + i) != static_cast<uint8_t>(kAtiMagic[i])) return;   // Invalid ATI magic
  const uint32_t base = cu16(kRomTablePtr);
  for (uint32_t i = 0; i + 1 < sizeof kRomMagic; ++i)
    if (cu8(base + kRomMagicPtr + i) != static_cast<uint8_t>(kRomMagic[i])) return;   // Invalid ATOM magic
  cmd_table_ = cu16(base + kRomCmdPtr);
  data_table_ = cu16(base + kRomDataPtr);
  index_iio(cu16(data_table_ + kDataIioPtr) + 4);
  if (failed()) return;
  // The firmware revision (the master data table's FirmwareInfo: ulFirmwareRevision).
  const uint32_t fwi = cu16(data_table_ + kDataFwiPtr);
  if (fwi != 0) version_ = cu32(fwi + kFwiRevisionPtr);
  if (failed()) return;
  // The scratch memory: what the firmware says it needs in VRAM_UsageByFirmware, else 20 KB.
  uint32_t usage_bytes = 0;
  uint16_t data_start = 0;
  if (data_table_header(kDataVramUsageIndex, nullptr, nullptr, nullptr, &data_start)) {
    const uint32_t start_addr = cu32(data_start + 4);
    const uint32_t kb = cu16(data_start + 8);
    const uint32_t kFlagsMask = 0xC0000000u, kFlagsShift = 30, kSriovMsgShare = 0x2;
    if ((start_addr & kFlagsMask) == (kSriovMsgShare << kFlagsShift)) usage_bytes = 0;   // (a reservation for SR-IOV: the default size)
    else usage_bytes = kb * 1024;
  }
  fail_ = Status::Ok;   // (a data table that is not there is not an error)
  if (usage_bytes == 0) usage_bytes = kDefaultScratchBytes;
  if (usage_bytes > kMaxScratchBytes || !scratch_.put_zeros(usage_bytes)) return;
  valid_ = true;
}

}  // namespace hardware
