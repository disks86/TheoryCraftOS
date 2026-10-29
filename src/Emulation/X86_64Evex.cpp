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
#include "Emulation/X86_64EvexInternal.hpp"

namespace emulation {

#define INT_OP(op, sub, w) {1, 1, op, -1, kIntBin, sub, w, (w) >= 32 ? kFull : kFullMem}
const EvexOp kEvexOps[] = {
    // moves
    {1, 0, 0x28, 0, kMove, 0, 32, kFullMem}, {1, 1, 0x28, 1, kMove, 0, 64, kFullMem}, {1, 0, 0x29, 0, kMove, 1, 32, kFullMem}, {1, 1, 0x29, 1, kMove, 1, 64, kFullMem},
    {1, 0, 0x10, 0, kMove, 0, 32, kFullMem}, {1, 1, 0x10, 1, kMove, 0, 64, kFullMem}, {1, 0, 0x11, 0, kMove, 1, 32, kFullMem}, {1, 1, 0x11, 1, kMove, 1, 64, kFullMem},
    {1, 1, 0x6F, 0, kMove, 0, 32, kFullMem}, {1, 1, 0x6F, 1, kMove, 0, 64, kFullMem}, {1, 1, 0x7F, 0, kMove, 1, 32, kFullMem}, {1, 1, 0x7F, 1, kMove, 1, 64, kFullMem},
    {1, 2, 0x6F, 0, kMove, 0, 32, kFullMem}, {1, 2, 0x6F, 1, kMove, 0, 64, kFullMem}, {1, 2, 0x7F, 0, kMove, 1, 32, kFullMem}, {1, 2, 0x7F, 1, kMove, 1, 64, kFullMem},
    {1, 3, 0x6F, 0, kMove, 0, 8, kFullMem}, {1, 3, 0x6F, 1, kMove, 0, 16, kFullMem}, {1, 3, 0x7F, 0, kMove, 1, 8, kFullMem}, {1, 3, 0x7F, 1, kMove, 1, 16, kFullMem},
    {1, 1, 0xE7, 0, kMove, 1, 32, kFullMem}, {1, 0, 0x2B, 0, kMove, 1, 32, kFullMem}, {1, 1, 0x2B, 1, kMove, 1, 64, kFullMem}, {2, 1, 0x2A, 0, kMove, 0, 32, kFullMem},
    {1, 2, 0x10, 0, kMoveScalar, 0, 32, kS4}, {1, 2, 0x11, 0, kMoveScalar, 1, 32, kS4}, {1, 3, 0x10, 1, kMoveScalar, 0, 64, kS8}, {1, 3, 0x11, 1, kMoveScalar, 1, 64, kS8},
    {1, 1, 0x6E, 0, kMoveGpr, 0, 32, kS4}, {1, 1, 0x6E, 1, kMoveGpr, 0, 64, kS8}, {1, 1, 0x7E, 0, kMoveGpr, 1, 32, kS4}, {1, 1, 0x7E, 1, kMoveGpr, 1, 64, kS8},
    {1, 2, 0x7E, 1, kMoveGpr, 2, 64, kS8}, {1, 1, 0xD6, 1, kMoveGpr, 3, 64, kS8}, {1, 2, 0x7E, 0, kMoveGpr, 2, 32, kS4}, {1, 1, 0xD6, 0, kMoveGpr, 3, 32, kS4},
    // broadcasts (sub: source bytes; 0x80: memory source only; 0x40: 512-bit only)
    {2, 1, 0x18, 0, kBroadcast, 4, 32, kS4}, {2, 1, 0x19, 1, kBroadcast, 8, 64, kS8}, {2, 1, 0x19, 0, kBroadcast, 8, 32, kS8},
    {2, 1, 0x1A, 0, kBroadcast, 16 | 0x80, 32, kS16}, {2, 1, 0x1A, 1, kBroadcast, 16 | 0x80, 64, kS16}, {2, 1, 0x1B, 0, kBroadcast, 32 | 0x80 | 0x40, 32, kS32}, {2, 1, 0x1B, 1, kBroadcast, 32 | 0x80, 64, kS32},
    {2, 1, 0x58, 0, kBroadcast, 4, 32, kS4}, {2, 1, 0x59, 1, kBroadcast, 8, 64, kS8}, {2, 1, 0x59, 0, kBroadcast, 8, 32, kS8},
    {2, 1, 0x5A, 0, kBroadcast, 16 | 0x80, 32, kS16}, {2, 1, 0x5A, 1, kBroadcast, 16 | 0x80, 64, kS16}, {2, 1, 0x5B, 0, kBroadcast, 32 | 0x80 | 0x40, 32, kS32}, {2, 1, 0x5B, 1, kBroadcast, 32 | 0x80, 64, kS32},
    {2, 1, 0x78, -1, kBroadcast, 1, 8, kS1}, {2, 1, 0x79, -1, kBroadcast, 2, 16, kS2},
    {2, 1, 0x7A, 0, kBroadcastGpr, 0, 8, kS1}, {2, 1, 0x7B, 0, kBroadcastGpr, 0, 16, kS2}, {2, 1, 0x7C, 0, kBroadcastGpr, 0, 32, kS4}, {2, 1, 0x7C, 1, kBroadcastGpr, 0, 64, kS8},
    // integer arithmetic (element width fixed)
    INT_OP(0xFC, kAdd, 8), INT_OP(0xFD, kAdd, 16), INT_OP(0xFE, kAdd, 32), INT_OP(0xD4, kAdd, 64), INT_OP(0xF8, kSub, 8), INT_OP(0xF9, kSub, 16), INT_OP(0xFA, kSub, 32), INT_OP(0xFB, kSub, 64),
    INT_OP(0xEC, kAdds, 8), INT_OP(0xED, kAdds, 16), INT_OP(0xDC, kAddus, 8), INT_OP(0xDD, kAddus, 16), INT_OP(0xE8, kSubs, 8), INT_OP(0xE9, kSubs, 16), INT_OP(0xD8, kSubus, 8), INT_OP(0xD9, kSubus, 16),
    INT_OP(0xD5, kMullo, 16), INT_OP(0xE5, kMulhs, 16), INT_OP(0xE4, kMulhu, 16), INT_OP(0xE0, kAvg, 8), INT_OP(0xE3, kAvg, 16), INT_OP(0xDA, kMinu, 8), INT_OP(0xDE, kMaxu, 8), INT_OP(0xEA, kMins, 16), INT_OP(0xEE, kMaxs, 16),
    {2, 1, 0x40, 0, kIntBin, kMullo, 32, kFull}, {2, 1, 0x40, 1, kIntBin, kMullo, 64, kFull}, {2, 1, 0x0B, -1, kIntBin, kMulhrs, 16, kFullMem},
    {2, 1, 0x38, -1, kIntBin, kMins, 8, kFullMem}, {2, 1, 0x39, 0, kIntBin, kMins, 32, kFull}, {2, 1, 0x39, 1, kIntBin, kMins, 64, kFull}, {2, 1, 0x3A, -1, kIntBin, kMinu, 16, kFullMem},
    {2, 1, 0x3B, 0, kIntBin, kMinu, 32, kFull}, {2, 1, 0x3B, 1, kIntBin, kMinu, 64, kFull}, {2, 1, 0x3C, -1, kIntBin, kMaxs, 8, kFullMem}, {2, 1, 0x3D, 0, kIntBin, kMaxs, 32, kFull},
    {2, 1, 0x3D, 1, kIntBin, kMaxs, 64, kFull}, {2, 1, 0x3E, -1, kIntBin, kMaxu, 16, kFullMem}, {2, 1, 0x3F, 0, kIntBin, kMaxu, 32, kFull}, {2, 1, 0x3F, 1, kIntBin, kMaxu, 64, kFull},
    {1, 1, 0xDB, 0, kIntBin, kAnd, 32, kFull}, {1, 1, 0xDB, 1, kIntBin, kAnd, 64, kFull}, {1, 1, 0xDF, 0, kIntBin, kAndn, 32, kFull}, {1, 1, 0xDF, 1, kIntBin, kAndn, 64, kFull},
    {1, 1, 0xEB, 0, kIntBin, kOr, 32, kFull}, {1, 1, 0xEB, 1, kIntBin, kOr, 64, kFull}, {1, 1, 0xEF, 0, kIntBin, kXor, 32, kFull}, {1, 1, 0xEF, 1, kIntBin, kXor, 64, kFull},
    {1, 0, 0x54, 0, kIntBin, kAnd, 32, kFull}, {1, 1, 0x54, 1, kIntBin, kAnd, 64, kFull}, {1, 0, 0x55, 0, kIntBin, kAndn, 32, kFull}, {1, 1, 0x55, 1, kIntBin, kAndn, 64, kFull},
    {1, 0, 0x56, 0, kIntBin, kOr, 32, kFull}, {1, 1, 0x56, 1, kIntBin, kOr, 64, kFull}, {1, 0, 0x57, 0, kIntBin, kXor, 32, kFull}, {1, 1, 0x57, 1, kIntBin, kXor, 64, kFull},
    {1, 1, 0xF4, -1, kMulDq, 1, 64, kFull}, {2, 1, 0x28, -1, kMulDq, 0, 64, kFull},
    // in-lane binary operations
    {1, 1, 0x60, -1, kLane, kUnpackLo, 8, kFullMem}, {1, 1, 0x61, -1, kLane, kUnpackLo, 16, kFullMem}, {1, 1, 0x62, 0, kLane, kUnpackLo, 32, kFull}, {1, 1, 0x6C, 1, kLane, kUnpackLo, 64, kFull},
    {1, 1, 0x68, -1, kLane, kUnpackHi, 8, kFullMem}, {1, 1, 0x69, -1, kLane, kUnpackHi, 16, kFullMem}, {1, 1, 0x6A, 0, kLane, kUnpackHi, 32, kFull}, {1, 1, 0x6D, 1, kLane, kUnpackHi, 64, kFull},
    {1, 1, 0x63, -1, kLane, kPacksswb, 8, kFullMem}, {1, 1, 0x67, -1, kLane, kPackuswb, 8, kFullMem}, {1, 1, 0x6B, 0, kLane, kPackssdw, 16, kFull}, {2, 1, 0x2B, 0, kLane, kPackusdw, 16, kFull},
    {2, 1, 0x00, -1, kLane, kPshufb, 8, kFullMem}, {1, 1, 0xF5, -1, kLane, kMaddwd, 32, kFullMem}, {2, 1, 0x04, -1, kLane, kMaddubsw, 16, kFullMem}, {1, 1, 0xF6, -1, kLane, kSadbw, 64, kFullMem},
    {1, 0, 0x14, 0, kLane, kUnpcklps, 32, kFull}, {1, 1, 0x14, 1, kLane, kUnpackLo, 64, kFull}, {1, 0, 0x15, 0, kLane, kUnpckhps, 32, kFull}, {1, 1, 0x15, 1, kLane, kUnpackHi, 64, kFull},
    {1, 0, 0xC6, 0, kLane, kShufps, 32, kFull}, {1, 1, 0xC6, 1, kLane, kShufpd, 64, kFull}, {3, 1, 0x0F, -1, kLane, kPalignr, 8, kFullMem},
    // shifts
    {1, 1, 0xD1, -1, kShiftCount, 1, 16, kMem128}, {1, 1, 0xD2, -1, kShiftCount, 1, 32, kMem128}, {1, 1, 0xD3, -1, kShiftCount, 1, 64, kMem128}, {1, 1, 0xE1, -1, kShiftCount, 2, 16, kMem128},
    {1, 1, 0xE2, 0, kShiftCount, 2, 32, kMem128}, {1, 1, 0xE2, 1, kShiftCount, 2, 64, kMem128}, {1, 1, 0xF1, -1, kShiftCount, 0, 16, kMem128}, {1, 1, 0xF2, -1, kShiftCount, 0, 32, kMem128},
    {1, 1, 0xF3, -1, kShiftCount, 0, 64, kMem128},
    {1, 1, 0x71, -1, kShiftImm, 16, 16, kFullMem}, {1, 1, 0x72, -1, kShiftImm, 32, 0, kFull}, {1, 1, 0x73, -1, kShiftImm, 64, 0, kFull},
    {2, 1, 0x45, 0, kShiftVar, 1, 32, kFull}, {2, 1, 0x45, 1, kShiftVar, 1, 64, kFull}, {2, 1, 0x46, 0, kShiftVar, 2, 32, kFull}, {2, 1, 0x46, 1, kShiftVar, 2, 64, kFull},
    {2, 1, 0x47, 0, kShiftVar, 0, 32, kFull}, {2, 1, 0x47, 1, kShiftVar, 0, 64, kFull}, {2, 1, 0x10, 1, kShiftVar, 1, 16, kFullMem}, {2, 1, 0x11, 1, kShiftVar, 2, 16, kFullMem},
    {2, 1, 0x12, 1, kShiftVar, 0, 16, kFullMem}, {2, 1, 0x14, 0, kRotVar, 1, 32, kFull}, {2, 1, 0x14, 1, kRotVar, 1, 64, kFull}, {2, 1, 0x15, 0, kRotVar, 0, 32, kFull}, {2, 1, 0x15, 1, kRotVar, 0, 64, kFull},
    // compares into mask registers
    {1, 1, 0x74, -1, kCmpEq, 0, 8, kFullMem}, {1, 1, 0x75, -1, kCmpEq, 0, 16, kFullMem}, {1, 1, 0x76, 0, kCmpEq, 0, 32, kFull}, {2, 1, 0x29, 1, kCmpEq, 0, 64, kFull},
    {1, 1, 0x64, -1, kCmpEq, 1, 8, kFullMem}, {1, 1, 0x65, -1, kCmpEq, 1, 16, kFullMem}, {1, 1, 0x66, 0, kCmpEq, 1, 32, kFull}, {2, 1, 0x37, 1, kCmpEq, 1, 64, kFull},
    {3, 1, 0x3F, 0, kCmpImm, 1, 8, kFullMem}, {3, 1, 0x3F, 1, kCmpImm, 1, 16, kFullMem}, {3, 1, 0x3E, 0, kCmpImm, 0, 8, kFullMem}, {3, 1, 0x3E, 1, kCmpImm, 0, 16, kFullMem},
    {3, 1, 0x1F, 0, kCmpImm, 1, 32, kFull}, {3, 1, 0x1F, 1, kCmpImm, 1, 64, kFull}, {3, 1, 0x1E, 0, kCmpImm, 0, 32, kFull}, {3, 1, 0x1E, 1, kCmpImm, 0, 64, kFull},
    {2, 1, 0x26, 0, kTest, 0, 8, kFullMem}, {2, 1, 0x26, 1, kTest, 0, 16, kFullMem}, {2, 1, 0x27, 0, kTest, 0, 32, kFull}, {2, 1, 0x27, 1, kTest, 0, 64, kFull},
    {2, 3, 0x68, -1, kVp2Intersect, 0, 0, kFull},
    {2, 3, 0x9A, 0, kFma4, 0, 32, kTuple4}, {2, 3, 0xAA, 0, kFma4, 2, 32, kTuple4}, {2, 3, 0x9B, 0, kFma4, 1, 32, kTuple4}, {2, 3, 0xAB, 0, kFma4, 3, 32, kTuple4},      // Xeon Phi 4FMAPS
    {2, 3, 0x52, 0, kVnni4, 0, 32, kTuple4}, {2, 3, 0x53, 0, kVnni4, 1, 32, kTuple4},      // Xeon Phi 4VNNIW
    // AVX-512 ER (Xeon Phi): vexp2, vrcp28, vrsqrt28
    {2, 1, 0xC8, 0, kEr, 0, 32, kFull}, {2, 1, 0xC8, 1, kEr, 0, 64, kFull}, {2, 1, 0xCA, 0, kEr, 1, 32, kFull}, {2, 1, 0xCA, 1, kEr, 1, 64, kFull}, {2, 1, 0xCB, 0, kEr, 1, 32, kScalar}, {2, 1, 0xCB, 1, kEr, 1, 64, kScalar},
    {2, 1, 0xCC, 0, kEr, 2, 32, kFull}, {2, 1, 0xCC, 1, kEr, 2, 64, kFull}, {2, 1, 0xCD, 0, kEr, 2, 32, kScalar}, {2, 1, 0xCD, 1, kEr, 2, 64, kScalar},
    // AVX-VNNI-INT8 / INT16 (sub: bit 4 = 16-bit elements, bit 3 = src1 signed, bit 2 = src2 signed, bit 0 = saturate)
    {2, 3, 0x50, 0, kDot, 12, 32, kFull}, {2, 3, 0x51, 0, kDot, 13, 32, kFull}, {2, 2, 0x50, 0, kDot, 8, 32, kFull}, {2, 2, 0x51, 0, kDot, 9, 32, kFull}, {2, 0, 0x50, 0, kDot, 0, 32, kFull}, {2, 0, 0x51, 0, kDot, 1, 32, kFull},
    {2, 2, 0xD2, 0, kDot, 24, 32, kFull}, {2, 2, 0xD3, 0, kDot, 25, 32, kFull}, {2, 1, 0xD2, 0, kDot, 20, 32, kFull}, {2, 1, 0xD3, 0, kDot, 21, 32, kFull}, {2, 0, 0xD2, 0, kDot, 16, 32, kFull}, {2, 0, 0xD3, 0, kDot, 17, 32, kFull},
    {2, 2, 0x26, 0, kTest, 1, 8, kFullMem}, {2, 2, 0x26, 1, kTest, 1, 16, kFullMem}, {2, 2, 0x27, 0, kTest, 1, 32, kFull}, {2, 2, 0x27, 1, kTest, 1, 64, kFull},
    {3, 1, 0x25, 0, kTernlog, 0, 32, kFull}, {3, 1, 0x25, 1, kTernlog, 0, 64, kFull},
    // shuffles and permutes (sub of kShufImm: 0 vpshufd, 1 vpshufhw, 2 vpshuflw, 3 vpermilps, 4 vpermilpd, 5 vpermq/vpermpd by immediate)
    {1, 1, 0x70, 0, kShufImm, 0, 32, kFull}, {1, 2, 0x70, -1, kShufImm, 1, 16, kFullMem}, {1, 3, 0x70, -1, kShufImm, 2, 16, kFullMem}, {3, 1, 0x04, 0, kShufImm, 3, 32, kFull},
    {3, 1, 0x05, 1, kShufImm, 4, 64, kFull}, {3, 1, 0x00, 1, kShufImm, 5, 64, kFull}, {3, 1, 0x01, 1, kShufImm, 5, 64, kFull},
    {2, 1, 0x36, 0, kPermute, 0, 32, kFull}, {2, 1, 0x36, 1, kPermute, 0, 64, kFull}, {2, 1, 0x16, 0, kPermute, 0, 32, kFull}, {2, 1, 0x16, 1, kPermute, 0, 64, kFull},
    {2, 1, 0x0C, 0, kPermute, 1, 32, kFull}, {2, 1, 0x0D, 1, kPermute, 2, 64, kFull}, {2, 1, 0x8D, 0, kPermute, 3, 8, kFullMem}, {2, 1, 0x8D, 1, kPermute, 4, 16, kFullMem},
    {2, 1, 0x75, 0, kPermute2, 0, 8, kFullMem}, {2, 1, 0x75, 1, kPermute2, 0, 16, kFullMem}, {2, 1, 0x76, 0, kPermute2, 0, 32, kFull}, {2, 1, 0x76, 1, kPermute2, 0, 64, kFull},
    {2, 1, 0x77, 0, kPermute2, 0, 32, kFull}, {2, 1, 0x77, 1, kPermute2, 0, 64, kFull},
    {2, 1, 0x7D, 0, kPermute2, 1, 8, kFullMem}, {2, 1, 0x7D, 1, kPermute2, 1, 16, kFullMem}, {2, 1, 0x7E, 0, kPermute2, 1, 32, kFull}, {2, 1, 0x7E, 1, kPermute2, 1, 64, kFull},
    {2, 1, 0x7F, 0, kPermute2, 1, 32, kFull}, {2, 1, 0x7F, 1, kPermute2, 1, 64, kFull},
    {3, 1, 0x18, 0, kInsert, 16, 32, kS16}, {3, 1, 0x18, 1, kInsert, 16, 64, kS16}, {3, 1, 0x38, 0, kInsert, 16, 32, kS16}, {3, 1, 0x38, 1, kInsert, 16, 64, kS16},
    {3, 1, 0x1A, 0, kInsert, 32, 32, kS32}, {3, 1, 0x1A, 1, kInsert, 32, 64, kS32}, {3, 1, 0x3A, 0, kInsert, 32, 32, kS32}, {3, 1, 0x3A, 1, kInsert, 32, 64, kS32},
    {3, 1, 0x19, 0, kExtract, 16, 32, kS16}, {3, 1, 0x19, 1, kExtract, 16, 64, kS16}, {3, 1, 0x39, 0, kExtract, 16, 32, kS16}, {3, 1, 0x39, 1, kExtract, 16, 64, kS16},
    {3, 1, 0x1B, 0, kExtract, 32, 32, kS32}, {3, 1, 0x1B, 1, kExtract, 32, 64, kS32}, {3, 1, 0x3B, 0, kExtract, 32, 32, kS32}, {3, 1, 0x3B, 1, kExtract, 32, 64, kS32},
    {2, 1, 0x64, 0, kBlend, 0, 32, kFull}, {2, 1, 0x64, 1, kBlend, 0, 64, kFull}, {2, 1, 0x65, 0, kBlend, 0, 32, kFull}, {2, 1, 0x65, 1, kBlend, 0, 64, kFull},
    {2, 1, 0x66, 0, kBlend, 0, 8, kFullMem}, {2, 1, 0x66, 1, kBlend, 0, 16, kFullMem},
    {3, 1, 0x03, 0, kAlign, 0, 32, kFull}, {3, 1, 0x03, 1, kAlign, 0, 64, kFull},
    {3, 1, 0x23, 0, kShuf128, 0, 32, kFull}, {3, 1, 0x23, 1, kShuf128, 0, 64, kFull}, {3, 1, 0x43, 0, kShuf128, 0, 32, kFull}, {3, 1, 0x43, 1, kShuf128, 0, 64, kFull},
    // unary integer (sub: 0 abs, 1 lzcnt, 2 popcnt, 3 conflict)
    {2, 1, 0x1C, -1, kIntUnary, 0, 8, kFullMem}, {2, 1, 0x1D, -1, kIntUnary, 0, 16, kFullMem}, {2, 1, 0x1E, 0, kIntUnary, 0, 32, kFull}, {2, 1, 0x1F, 1, kIntUnary, 0, 64, kFull},
    {2, 1, 0x44, 0, kIntUnary, 1, 32, kFull}, {2, 1, 0x44, 1, kIntUnary, 1, 64, kFull}, {2, 1, 0x54, 0, kIntUnary, 2, 8, kFullMem}, {2, 1, 0x54, 1, kIntUnary, 2, 16, kFullMem},
    {2, 1, 0x55, 0, kIntUnary, 2, 32, kFull}, {2, 1, 0x55, 1, kIntUnary, 2, 64, kFull}, {2, 1, 0xC4, 0, kIntUnary, 3, 32, kFull}, {2, 1, 0xC4, 1, kIntUnary, 3, 64, kFull},
    // zero / sign extension (sub: source bytes, 0x80 = signed; elem: destination width) and the down-converting moves (sub: source bytes | mode << 4; elem: destination width)
    {2, 1, 0x30, 0, kExtend, 1, 16, kHalf}, {2, 1, 0x31, 0, kExtend, 1, 32, kQuarter}, {2, 1, 0x32, 0, kExtend, 1, 64, kEighth}, {2, 1, 0x33, 0, kExtend, 2, 32, kHalf},
    {2, 1, 0x34, 0, kExtend, 2, 64, kQuarter}, {2, 1, 0x35, 0, kExtend, 4, 64, kHalf},
    {2, 1, 0x20, 0, kExtend, 0x81, 16, kHalf}, {2, 1, 0x21, 0, kExtend, 0x81, 32, kQuarter}, {2, 1, 0x22, 0, kExtend, 0x81, 64, kEighth}, {2, 1, 0x23, 0, kExtend, 0x82, 32, kHalf},
    {2, 1, 0x24, 0, kExtend, 0x82, 64, kQuarter}, {2, 1, 0x25, 0, kExtend, 0x84, 64, kHalf},
    {2, 2, 0x30, 0, kNarrow, 2, 8, kHalf}, {2, 2, 0x31, 0, kNarrow, 4, 8, kQuarter}, {2, 2, 0x32, 0, kNarrow, 8, 8, kEighth}, {2, 2, 0x33, 0, kNarrow, 4, 16, kHalf},
    {2, 2, 0x34, 0, kNarrow, 8, 16, kQuarter}, {2, 2, 0x35, 0, kNarrow, 8, 32, kHalf},
    {2, 2, 0x20, 0, kNarrow, 2 | 0x10, 8, kHalf}, {2, 2, 0x21, 0, kNarrow, 4 | 0x10, 8, kQuarter}, {2, 2, 0x22, 0, kNarrow, 8 | 0x10, 8, kEighth}, {2, 2, 0x23, 0, kNarrow, 4 | 0x10, 16, kHalf},
    {2, 2, 0x24, 0, kNarrow, 8 | 0x10, 16, kQuarter}, {2, 2, 0x25, 0, kNarrow, 8 | 0x10, 32, kHalf},
    {2, 2, 0x10, 0, kNarrow, 2 | 0x20, 8, kHalf}, {2, 2, 0x11, 0, kNarrow, 4 | 0x20, 8, kQuarter}, {2, 2, 0x12, 0, kNarrow, 8 | 0x20, 8, kEighth}, {2, 2, 0x13, 0, kNarrow, 4 | 0x20, 16, kHalf},
    {2, 2, 0x14, 0, kNarrow, 8 | 0x20, 16, kQuarter}, {2, 2, 0x15, 0, kNarrow, 8 | 0x20, 32, kHalf},
    {2, 2, 0x28, 0, kMaskMove, 0, 8, kFull}, {2, 2, 0x28, 1, kMaskMove, 0, 16, kFull}, {2, 2, 0x38, 0, kMaskMove, 0, 32, kFull}, {2, 2, 0x38, 1, kMaskMove, 0, 64, kFull},
    {2, 2, 0x29, 0, kMaskMove, 1, 8, kFull}, {2, 2, 0x29, 1, kMaskMove, 1, 16, kFull}, {2, 2, 0x39, 0, kMaskMove, 1, 32, kFull}, {2, 2, 0x39, 1, kMaskMove, 1, 64, kFull},
    {2, 1, 0x8A, 0, kCompress, 0, 32, kScalar}, {2, 1, 0x8A, 1, kCompress, 0, 64, kScalar}, {2, 1, 0x8B, 0, kCompress, 0, 32, kScalar}, {2, 1, 0x8B, 1, kCompress, 0, 64, kScalar},
    {2, 1, 0x88, 0, kExpand, 0, 32, kScalar}, {2, 1, 0x88, 1, kExpand, 0, 64, kScalar}, {2, 1, 0x89, 0, kExpand, 0, 32, kScalar}, {2, 1, 0x89, 1, kExpand, 0, 64, kScalar},
    // floating point
    {1, 0, 0x58, 0, kFpBin, 0x58, 32, kFull}, {1, 1, 0x58, 1, kFpBin, 0x58, 64, kFull}, {1, 2, 0x58, 0, kFpScalar, 0x58, 32, kS4}, {1, 3, 0x58, 1, kFpScalar, 0x58, 64, kS8},
    {1, 0, 0x59, 0, kFpBin, 0x59, 32, kFull}, {1, 1, 0x59, 1, kFpBin, 0x59, 64, kFull}, {1, 2, 0x59, 0, kFpScalar, 0x59, 32, kS4}, {1, 3, 0x59, 1, kFpScalar, 0x59, 64, kS8},
    {1, 0, 0x5C, 0, kFpBin, 0x5C, 32, kFull}, {1, 1, 0x5C, 1, kFpBin, 0x5C, 64, kFull}, {1, 2, 0x5C, 0, kFpScalar, 0x5C, 32, kS4}, {1, 3, 0x5C, 1, kFpScalar, 0x5C, 64, kS8},
    {1, 0, 0x5D, 0, kFpBin, 0x5D, 32, kFull}, {1, 1, 0x5D, 1, kFpBin, 0x5D, 64, kFull}, {1, 2, 0x5D, 0, kFpScalar, 0x5D, 32, kS4}, {1, 3, 0x5D, 1, kFpScalar, 0x5D, 64, kS8},
    {1, 0, 0x5E, 0, kFpBin, 0x5E, 32, kFull}, {1, 1, 0x5E, 1, kFpBin, 0x5E, 64, kFull}, {1, 2, 0x5E, 0, kFpScalar, 0x5E, 32, kS4}, {1, 3, 0x5E, 1, kFpScalar, 0x5E, 64, kS8},
    {1, 0, 0x5F, 0, kFpBin, 0x5F, 32, kFull}, {1, 1, 0x5F, 1, kFpBin, 0x5F, 64, kFull}, {1, 2, 0x5F, 0, kFpScalar, 0x5F, 32, kS4}, {1, 3, 0x5F, 1, kFpScalar, 0x5F, 64, kS8},
    {1, 0, 0x51, 0, kFpUnary, 0x51, 32, kFull}, {1, 1, 0x51, 1, kFpUnary, 0x51, 64, kFull}, {1, 2, 0x51, 0, kFpScalar, 0x51, 32, kS4}, {1, 3, 0x51, 1, kFpScalar, 0x51, 64, kS8},
    {2, 1, 0x96, 0, kFma, 0x96, 32, kFull}, {2, 1, 0x96, 1, kFma, 0x96, 64, kFull},
    {2, 1, 0x97, 0, kFma, 0x97, 32, kFull}, {2, 1, 0x97, 1, kFma, 0x97, 64, kFull},
    {2, 1, 0x98, 0, kFma, 0x98, 32, kFull}, {2, 1, 0x98, 1, kFma, 0x98, 64, kFull},
    {2, 1, 0x99, 0, kFma, 0x99, 32, kS4}, {2, 1, 0x99, 1, kFma, 0x99, 64, kS8},
    {2, 1, 0x9A, 0, kFma, 0x9A, 32, kFull}, {2, 1, 0x9A, 1, kFma, 0x9A, 64, kFull},
    {2, 1, 0x9B, 0, kFma, 0x9B, 32, kS4}, {2, 1, 0x9B, 1, kFma, 0x9B, 64, kS8},
    {2, 1, 0x9C, 0, kFma, 0x9C, 32, kFull}, {2, 1, 0x9C, 1, kFma, 0x9C, 64, kFull},
    {2, 1, 0x9D, 0, kFma, 0x9D, 32, kS4}, {2, 1, 0x9D, 1, kFma, 0x9D, 64, kS8},
    {2, 1, 0x9E, 0, kFma, 0x9E, 32, kFull}, {2, 1, 0x9E, 1, kFma, 0x9E, 64, kFull},
    {2, 1, 0x9F, 0, kFma, 0x9F, 32, kS4}, {2, 1, 0x9F, 1, kFma, 0x9F, 64, kS8},
    {2, 1, 0xA6, 0, kFma, 0xA6, 32, kFull}, {2, 1, 0xA6, 1, kFma, 0xA6, 64, kFull},
    {2, 1, 0xA7, 0, kFma, 0xA7, 32, kFull}, {2, 1, 0xA7, 1, kFma, 0xA7, 64, kFull},
    {2, 1, 0xA8, 0, kFma, 0xA8, 32, kFull}, {2, 1, 0xA8, 1, kFma, 0xA8, 64, kFull},
    {2, 1, 0xA9, 0, kFma, 0xA9, 32, kS4}, {2, 1, 0xA9, 1, kFma, 0xA9, 64, kS8},
    {2, 1, 0xAA, 0, kFma, 0xAA, 32, kFull}, {2, 1, 0xAA, 1, kFma, 0xAA, 64, kFull},
    {2, 1, 0xAB, 0, kFma, 0xAB, 32, kS4}, {2, 1, 0xAB, 1, kFma, 0xAB, 64, kS8},
    {2, 1, 0xAC, 0, kFma, 0xAC, 32, kFull}, {2, 1, 0xAC, 1, kFma, 0xAC, 64, kFull},
    {2, 1, 0xAD, 0, kFma, 0xAD, 32, kS4}, {2, 1, 0xAD, 1, kFma, 0xAD, 64, kS8},
    {2, 1, 0xAE, 0, kFma, 0xAE, 32, kFull}, {2, 1, 0xAE, 1, kFma, 0xAE, 64, kFull},
    {2, 1, 0xAF, 0, kFma, 0xAF, 32, kS4}, {2, 1, 0xAF, 1, kFma, 0xAF, 64, kS8},
    {2, 1, 0xB6, 0, kFma, 0xB6, 32, kFull}, {2, 1, 0xB6, 1, kFma, 0xB6, 64, kFull},
    {2, 1, 0xB7, 0, kFma, 0xB7, 32, kFull}, {2, 1, 0xB7, 1, kFma, 0xB7, 64, kFull},
    {2, 1, 0xB8, 0, kFma, 0xB8, 32, kFull}, {2, 1, 0xB8, 1, kFma, 0xB8, 64, kFull},
    {2, 1, 0xB9, 0, kFma, 0xB9, 32, kS4}, {2, 1, 0xB9, 1, kFma, 0xB9, 64, kS8},
    {2, 1, 0xBA, 0, kFma, 0xBA, 32, kFull}, {2, 1, 0xBA, 1, kFma, 0xBA, 64, kFull},
    {2, 1, 0xBB, 0, kFma, 0xBB, 32, kS4}, {2, 1, 0xBB, 1, kFma, 0xBB, 64, kS8},
    {2, 1, 0xBC, 0, kFma, 0xBC, 32, kFull}, {2, 1, 0xBC, 1, kFma, 0xBC, 64, kFull},
    {2, 1, 0xBD, 0, kFma, 0xBD, 32, kS4}, {2, 1, 0xBD, 1, kFma, 0xBD, 64, kS8},
    {2, 1, 0xBE, 0, kFma, 0xBE, 32, kFull}, {2, 1, 0xBE, 1, kFma, 0xBE, 64, kFull},
    {2, 1, 0xBF, 0, kFma, 0xBF, 32, kS4}, {2, 1, 0xBF, 1, kFma, 0xBF, 64, kS8},
    {1, 0, 0xC2, 0, kFpCmp, 0, 32, kFull}, {1, 1, 0xC2, 1, kFpCmp, 0, 64, kFull}, {1, 2, 0xC2, 0, kFpCmp, 1, 32, kS4}, {1, 3, 0xC2, 1, kFpCmp, 1, 64, kS8},
    {3, 1, 0x08, 0, kRound, 0, 32, kFull}, {3, 1, 0x09, 1, kRound, 0, 64, kFull}, {3, 1, 0x0A, 0, kRound, 1, 32, kS4}, {3, 1, 0x0B, 1, kRound, 1, 64, kS8},
    {1, 0, 0x5A, 0, kCvt, 8, 32, kHalf},
    {1, 1, 0x5A, 1, kCvt, 1, 64, kFull},
    {1, 0, 0x5B, 0, kCvt, 2, 32, kFull},
    {1, 0, 0x5B, 1, kCvt, 4, 64, kFull},
    {1, 1, 0x5B, 0, kCvt, 16, 32, kFull},
    {1, 2, 0x5B, 0, kCvt, 80, 32, kFull},
    {1, 2, 0xE6, 0, kCvt, 10, 32, kHalf},
    {1, 2, 0xE6, 1, kCvt, 12, 64, kFull},
    {1, 3, 0xE6, 0, kCvt, 17, 64, kFull},
    {1, 3, 0xE6, 1, kCvt, 17, 64, kFull},
    {1, 1, 0xE6, 1, kCvt, 81, 64, kFull},
    {1, 0, 0x78, 0, kCvt, 88, 32, kFull},
    {1, 0, 0x78, 1, kCvt, 89, 64, kFull},
    {1, 0, 0x79, 0, kCvt, 24, 32, kFull},
    {1, 0, 0x79, 1, kCvt, 25, 64, kFull},
    {1, 1, 0x78, 0, kCvt, 104, 32, kHalf},
    {1, 1, 0x78, 1, kCvt, 105, 64, kFull},
    {1, 1, 0x79, 0, kCvt, 40, 32, kHalf},
    {1, 1, 0x79, 1, kCvt, 41, 64, kFull},
    {1, 1, 0x7A, 0, kCvt, 96, 32, kHalf},
    {1, 1, 0x7A, 1, kCvt, 97, 64, kFull},
    {1, 1, 0x7B, 0, kCvt, 32, 32, kHalf},
    {1, 1, 0x7B, 1, kCvt, 33, 64, kFull},
    {1, 2, 0x7A, 0, kCvt, 11, 32, kHalf},
    {1, 2, 0x7A, 1, kCvt, 13, 64, kFull},
    {1, 3, 0x7A, 0, kCvt, 3, 32, kFull},
    {1, 3, 0x7A, 1, kCvt, 5, 64, kFull},
    {1, 2, 0x5A, 0, kCvtScalar, 8, 32, kS4}, {1, 3, 0x5A, 1, kCvtScalar, 1, 64, kS8},
    {1, 2, 0x2C, -1, kCvtToGpr, 64, 32, kS4}, {1, 2, 0x2D, -1, kCvtToGpr, 0, 32, kS4}, {1, 2, 0x78, -1, kCvtToGpr, 192, 32, kS4}, {1, 2, 0x79, -1, kCvtToGpr, 128, 32, kS4},
    {1, 2, 0x2A, -1, kCvtFromGpr, 0, 0, kScalar}, {1, 2, 0x7B, -1, kCvtFromGpr, 128, 0, kScalar},
    {1, 3, 0x2C, -1, kCvtToGpr, 65, 64, kS8}, {1, 3, 0x2D, -1, kCvtToGpr, 1, 64, kS8}, {1, 3, 0x78, -1, kCvtToGpr, 193, 64, kS8}, {1, 3, 0x79, -1, kCvtToGpr, 129, 64, kS8},
    {1, 3, 0x2A, -1, kCvtFromGpr, 1, 0, kScalar}, {1, 3, 0x7B, -1, kCvtFromGpr, 129, 0, kScalar},
    // more F: duplicates, high/low moves, single elements, half conversions, gathers and scatters
    {1, 3, 0x12, 1, kDup, 6, 64, kFullMem}, {1, 2, 0x12, 0, kDup, 7, 32, kFullMem}, {1, 2, 0x16, 0, kDup, 8, 32, kFullMem},
    {1, 0, 0x12, 0, kMovHL, 0, 32, kS8}, {1, 1, 0x12, 1, kMovHL, 0, 64, kS8}, {1, 0, 0x13, 0, kMovHL, 1, 32, kS8}, {1, 1, 0x13, 1, kMovHL, 1, 64, kS8},
    {1, 0, 0x16, 0, kMovHL, 2, 32, kS8}, {1, 1, 0x16, 1, kMovHL, 2, 64, kS8}, {1, 0, 0x17, 0, kMovHL, 3, 32, kS8}, {1, 1, 0x17, 1, kMovHL, 3, 64, kS8},
    {3, 1, 0x14, -1, kExtractElem, 8, 8, kS1}, {3, 1, 0x15, -1, kExtractElem, 16, 16, kS2}, {3, 1, 0x16, 0, kExtractElem, 32, 32, kS4}, {3, 1, 0x16, 1, kExtractElem, 64, 64, kS8},
    {3, 1, 0x17, 0, kExtractElem, 32, 32, kS4}, {1, 1, 0xC5, -1, kExtractElem, 0x80 | 16, 16, kS2},
    {3, 1, 0x20, -1, kInsertElem, 8, 8, kS1}, {1, 1, 0xC4, -1, kInsertElem, 16, 16, kS2}, {3, 1, 0x22, 0, kInsertElem, 32, 32, kS4}, {3, 1, 0x22, 1, kInsertElem, 64, 64, kS8},
    {3, 1, 0x21, 0, kInsertElem, 0x80 | 32, 32, kS4},
    {2, 1, 0x13, 0, kHalfConv, 0, 32, kHalf}, {3, 1, 0x1D, 0, kHalfConv, 1, 32, kHalf},
    {2, 1, 0x90, 0, kGather, 32, 32, kScalar}, {2, 1, 0x90, 1, kGather, 32, 64, kScalar}, {2, 1, 0x91, 0, kGather, 64, 32, kScalar}, {2, 1, 0x91, 1, kGather, 64, 64, kScalar},
    {2, 1, 0x92, 0, kGather, 32, 32, kScalar}, {2, 1, 0x92, 1, kGather, 32, 64, kScalar}, {2, 1, 0x93, 0, kGather, 64, 32, kScalar}, {2, 1, 0x93, 1, kGather, 64, 64, kScalar},
    {2, 1, 0xA0, 0, kScatter, 32, 32, kScalar}, {2, 1, 0xA0, 1, kScatter, 32, 64, kScalar}, {2, 1, 0xA1, 0, kScatter, 64, 32, kScalar}, {2, 1, 0xA1, 1, kScatter, 64, 64, kScalar},
    {2, 1, 0xA2, 0, kScatter, 32, 32, kScalar}, {2, 1, 0xA2, 1, kScatter, 32, 64, kScalar}, {2, 1, 0xA3, 0, kScatter, 64, 32, kScalar}, {2, 1, 0xA3, 1, kScatter, 64, 64, kScalar},
    // special floating-point operations
    {2, 1, 0x42, 0, kGetExp, 0, 32, kFull}, {2, 1, 0x42, 1, kGetExp, 0, 64, kFull}, {2, 1, 0x43, 0, kGetExp, 1, 32, kS4}, {2, 1, 0x43, 1, kGetExp, 1, 64, kS8},
    {2, 1, 0x2C, 0, kScalef, 0, 32, kFull}, {2, 1, 0x2C, 1, kScalef, 0, 64, kFull}, {2, 1, 0x2D, 0, kScalef, 1, 32, kS4}, {2, 1, 0x2D, 1, kScalef, 1, 64, kS8},
    {3, 1, 0x26, 0, kGetMant, 0, 32, kFull}, {3, 1, 0x26, 1, kGetMant, 0, 64, kFull}, {3, 1, 0x27, 0, kGetMant, 1, 32, kS4}, {3, 1, 0x27, 1, kGetMant, 1, 64, kS8},
    {3, 1, 0x54, 0, kFixup, 0, 32, kFull}, {3, 1, 0x54, 1, kFixup, 0, 64, kFull}, {3, 1, 0x55, 0, kFixup, 1, 32, kS4}, {3, 1, 0x55, 1, kFixup, 1, 64, kS8},
    {3, 1, 0x56, 0, kReduce, 0, 32, kFull}, {3, 1, 0x56, 1, kReduce, 0, 64, kFull}, {3, 1, 0x57, 0, kReduce, 1, 32, kS4}, {3, 1, 0x57, 1, kReduce, 1, 64, kS8},
    {3, 1, 0x50, 0, kRange, 0, 32, kFull}, {3, 1, 0x50, 1, kRange, 0, 64, kFull}, {3, 1, 0x51, 0, kRange, 1, 32, kS4}, {3, 1, 0x51, 1, kRange, 1, 64, kS8},
    {3, 1, 0x66, 0, kFpClass, 0, 32, kFull}, {3, 1, 0x66, 1, kFpClass, 0, 64, kFull}, {3, 1, 0x67, 0, kFpClass, 1, 32, kS4}, {3, 1, 0x67, 1, kFpClass, 1, 64, kS8},
    {2, 1, 0x4C, 0, kRcp14, 0, 32, kFull}, {2, 1, 0x4C, 1, kRcp14, 0, 64, kFull}, {2, 1, 0x4D, 0, kRcp14, 1, 32, kS4}, {2, 1, 0x4D, 1, kRcp14, 1, 64, kS8},
    {2, 1, 0x4E, 0, kRsqrt14, 0, 32, kFull}, {2, 1, 0x4E, 1, kRsqrt14, 0, 64, kFull}, {2, 1, 0x4F, 0, kRsqrt14, 1, 32, kS4}, {2, 1, 0x4F, 1, kRsqrt14, 1, 64, kS8},
    // VNNI, IFMA, VBMI, VBMI2, BITALG, GFNI, BF16, VAES and VPCLMULQDQ
    {2, 1, 0x50, 0, kVnni, 0, 32, kFull}, {2, 1, 0x51, 0, kVnni, 1, 32, kFull}, {2, 1, 0x52, 0, kVnni, 2, 32, kFull}, {2, 1, 0x53, 0, kVnni, 3, 32, kFull},
    {2, 1, 0xB4, 1, kMadd52, 0, 64, kFull}, {2, 1, 0xB5, 1, kMadd52, 1, 64, kFull}, {2, 1, 0x83, 1, kMultishift, 0, 64, kFull}, {2, 1, 0x8F, 0, kShufBit, 0, 8, kFullMem},
    {3, 1, 0x70, 1, kShift2, 0, 16, kFullMem}, {3, 1, 0x71, 0, kShift2, 0, 32, kFull}, {3, 1, 0x71, 1, kShift2, 0, 64, kFull}, {3, 1, 0x72, 1, kShift2, 1, 16, kFullMem},
    {3, 1, 0x73, 0, kShift2, 1, 32, kFull}, {3, 1, 0x73, 1, kShift2, 1, 64, kFull},
    {2, 1, 0x70, 1, kShift2Var, 0, 16, kFullMem}, {2, 1, 0x71, 0, kShift2Var, 0, 32, kFull}, {2, 1, 0x71, 1, kShift2Var, 0, 64, kFull}, {2, 1, 0x72, 1, kShift2Var, 1, 16, kFullMem},
    {2, 1, 0x73, 0, kShift2Var, 1, 32, kFull}, {2, 1, 0x73, 1, kShift2Var, 1, 64, kFull},
    {2, 1, 0x63, 0, kCompress, 0, 8, kS1}, {2, 1, 0x63, 1, kCompress, 0, 16, kS2}, {2, 1, 0x62, 0, kExpand, 0, 8, kS1}, {2, 1, 0x62, 1, kExpand, 0, 16, kS2},
    {3, 1, 0xCE, 1, kGfAffine, 0, 64, kFull}, {3, 1, 0xCF, 1, kGfAffine, 1, 64, kFull}, {2, 1, 0xCF, 0, kGfMul, 0, 8, kFullMem},
    {2, 3, 0x72, 0, kBf16, 0, 32, kFull}, {2, 2, 0x72, 0, kBf16, 1, 32, kFull}, {2, 2, 0x52, 0, kBf16, 2, 32, kFull},
    {2, 1, 0xDC, -1, kAesNi, 0, 32, kFullMem}, {2, 1, 0xDD, -1, kAesNi, 1, 32, kFullMem}, {2, 1, 0xDE, -1, kAesNi, 2, 32, kFullMem}, {2, 1, 0xDF, -1, kAesNi, 3, 32, kFullMem},
    {3, 1, 0x42, 0, kDbpsadbw, 0, 16, kFullMem}, {1, 0, 0x2E, 0, kComi, 0, 32, kS4}, {1, 1, 0x2E, 1, kComi, 0, 64, kS8}, {1, 0, 0x2F, 0, kComi, 1, 32, kS4}, {1, 1, 0x2F, 1, kComi, 1, 64, kS8},
    {3, 1, 0x44, -1, kClmul, 0, 64, kFullMem}, {2, 2, 0x2A, 1, kBroadcastMask, 0, 64, kS1}, {2, 2, 0x3A, 0, kBroadcastMask, 1, 32, kS1},
};
#undef INT_OP
const uint32_t kEvexOpCount = sizeof(kEvexOps) / sizeof(kEvexOps[0]);

uint64_t evex_integer_op(uint32_t sub, uint32_t w, uint64_t a, uint64_t b) {
  const uint64_t mask = width_mask(w);
  const int64_t sa = signed_of(a, w), sb = signed_of(b, w);
  switch (sub) {
    case kAdd: return (a + b) & mask;
    case kSub: return (a - b) & mask;
    case kAdds: {
      const int64_t t = sa + sb, hi = (int64_t{1} << (w - 1)) - 1;
      return static_cast<uint64_t>(t > hi ? hi : t < -hi - 1 ? -hi - 1 : t) & mask;
    }
    case kSubs: {
      const int64_t t = sa - sb, hi = (int64_t{1} << (w - 1)) - 1;
      return static_cast<uint64_t>(t > hi ? hi : t < -hi - 1 ? -hi - 1 : t) & mask;
    }
    case kAddus: return a + b > mask ? mask : a + b;
    case kSubus: return a < b ? 0 : a - b;
    case kMullo: return (a * b) & mask;
    case kMulhs: return static_cast<uint64_t>((sa * sb) >> w) & mask;
    case kMulhu: return ((a * b) >> w) & mask;
    case kMulhrs: return static_cast<uint64_t>((((sa * sb) >> 14) + 1) >> 1) & mask;
    case kAvg: return (a + b + 1) >> 1;
    case kMins: return sa < sb ? a : b;
    case kMinu: return a < b ? a : b;
    case kMaxs: return sa > sb ? a : b;
    case kMaxu: return a > b ? a : b;
    case kAnd: return a & b;
    case kAndn: return ~a & b;
    case kOr: return a | b;
    default: return a ^ b;
  }
}

uint64_t evex_saturate_signed(int64_t v, uint32_t w) {
  const int64_t hi = (int64_t{1} << (w - 1)) - 1;
  return static_cast<uint64_t>(v > hi ? hi : v < -hi - 1 ? -hi - 1 : v) & width_mask(w);
}

uint64_t evex_saturate_unsigned(int64_t v, uint32_t w) {
  const int64_t hi = static_cast<int64_t>(width_mask(w));
  return static_cast<uint64_t>(v < 0 ? 0 : v > hi ? hi : v);
}

X86_64Cpu::Vec512 X86_64Cpu::get_vec(uint32_t r) const {
  Vec512 v;
  const Xmm& lo = r < 16 ? s_.xmm[r] : s_.evex_lo[r - 16];
  const YmmHigh& hi = r < 16 ? s_.ymm_high[r] : s_.evex_hi[r - 16];
  v.q[0] = lo.lo;
  v.q[1] = lo.hi;
  v.q[2] = hi.lo;
  v.q[3] = hi.hi;
  v.q[4] = hi.upper[0].lo;
  v.q[5] = hi.upper[0].hi;
  v.q[6] = hi.upper[1].lo;
  v.q[7] = hi.upper[1].hi;
  return v;
}

void X86_64Cpu::set_vec(uint32_t r, const Vec512& v, uint32_t vl_bits) {
  Xmm& lo = r < 16 ? s_.xmm[r] : s_.evex_lo[r - 16];
  YmmHigh& hi = r < 16 ? s_.ymm_high[r] : s_.evex_hi[r - 16];
  const uint32_t n = vl_bits / 64;
  auto q = [&](uint32_t i) { return i < n ? v.q[i] : 0; };
  lo.lo = q(0);
  lo.hi = q(1);
  hi.lo = q(2);
  hi.hi = q(3);
  hi.upper[0].lo = q(4);
  hi.upper[0].hi = q(5);
  hi.upper[1].lo = q(6);
  hi.upper[1].hi = q(7);
}

// --- the opmask instructions (VEX-encoded) ------------------------------------------------------------------------------------------------------

X86_64Cpu::Step X86_64Cpu::kmask_instruction(GuestMemory& m, uint32_t map, uint8_t op, bool wide_w, uint32_t vvvv) {
  auto bad_decode = [&]() { return fail(d_.available < 15 ? 14 : 6, s_.rip + d_.available); };
  const uint32_t pp = d_.operand16 ? 1 : d_.rep ? 2 : d_.repne ? 3 : 0;
  uint32_t bits;      // the width of the mask operation
  if (pp == 0) bits = wide_w ? 64 : 16;
  else if (pp == 1) bits = wide_w ? 32 : 8;
  else bits = 0;
  const uint64_t mask = bits == 64 ? ~uint64_t{0} : bits ? (uint64_t{1} << bits) - 1 : 0;
  if (map == 3) {      // kshiftr / kshiftl
    if (op < 0x30 || op > 0x33 || pp != 1) return fail(6);
    if (!decode_modrm()) return bad_decode();
    const uint32_t count = static_cast<uint32_t>(next_imm(1, false));
    if (!finish_decode() || d_.mod != 3 || vex_.wide) return d_.truncated ? bad_decode() : fail(6);
    const uint32_t width = op == 0x30 ? (wide_w ? 16 : 8) : op == 0x31 ? (wide_w ? 64 : 32) : op == 0x32 ? (wide_w ? 16 : 8) : (wide_w ? 64 : 32);
    const uint64_t wm = width == 64 ? ~uint64_t{0} : (uint64_t{1} << width) - 1;
    const uint64_t src = s_.k[d_.rm & 7] & wm;
    uint64_t r = 0;
    if (count < width) r = (op <= 0x31 ? src >> count : src << count) & wm;
    s_.k[d_.reg & 7] = r;
    s_.rip += d_.position;
    return Step::Ok;
  }
  if (!decode_modrm() || !finish_decode()) return bad_decode();
  switch (op) {
    case 0x41: case 0x42: case 0x45: case 0x46: case 0x47: case 0x4A: case 0x4B: {      // kand, kandn, kor, kxnor, kxor, kadd, kunpck
      if (d_.mod != 3 || !vex_.wide) return fail(6);
      const uint64_t a = s_.k[vvvv & 7], b = s_.k[d_.rm & 7];
      uint64_t r;
      if (op == 0x4B) {
        const uint32_t half = pp == 1 ? 8 : (wide_w ? 32 : 16);      // kunpckbw (66 W0), kunpckwd (NP W0), kunpckdq (NP W1)
        const uint64_t hm = (uint64_t{1} << half) - 1;
        r = ((a & hm) << half) | (b & hm);
        if (pp == 0 && wide_w) r = ((a & 0xFFFFFFFFu) << 32) | (b & 0xFFFFFFFFu);
      } else if (!bits) {
        return fail(6);
      } else {
        switch (op) {
          case 0x41: r = a & b; break;
          case 0x42: r = ~a & b; break;
          case 0x45: r = a | b; break;
          case 0x46: r = ~(a ^ b); break;
          case 0x47: r = a ^ b; break;
          default: r = a + b; break;
        }
        r &= mask;
      }
      s_.k[d_.reg & 7] = r;
      break;
    }
    case 0x44: {      // knot
      if (d_.mod != 3 || vex_.wide || !bits) return fail(6);
      s_.k[d_.reg & 7] = ~s_.k[d_.rm & 7] & mask;
      break;
    }
    case 0x90: {      // kmov k, k/m
      if (vex_.wide || !bits) return fail(6);
      if (d_.mod == 3) {
        s_.k[d_.reg & 7] = s_.k[d_.rm & 7] & mask;
      } else {
        uint64_t v;
        if (!load(m, memory_address(), bits / 8, &v)) return Step::Fault;
        s_.k[d_.reg & 7] = v;
      }
      break;
    }
    case 0x91: {      // kmov m, k
      if (vex_.wide || !bits || d_.mod == 3) return fail(6);
      if (!store(m, memory_address(), bits / 8, s_.k[d_.reg & 7] & mask)) return Step::Fault;
      break;
    }
    case 0x92: {      // kmov k, r32/r64
      if (vex_.wide || d_.mod != 3) return fail(6);
      uint32_t width;
      if (pp == 0 && !wide_w) width = 16;
      else if (pp == 1 && !wide_w) width = 8;
      else if (pp == 3) width = wide_w ? 64 : 32;
      else return fail(6);
      s_.k[d_.reg & 7] = get_reg(d_.rm, width == 64 ? 8 : 4) & (width == 64 ? ~uint64_t{0} : (uint64_t{1} << width) - 1);
      break;
    }
    case 0x93: {      // kmov r32/r64, k
      if (vex_.wide || d_.mod != 3) return fail(6);
      uint32_t width;
      if (pp == 0 && !wide_w) width = 16;
      else if (pp == 1 && !wide_w) width = 8;
      else if (pp == 3) width = wide_w ? 64 : 32;
      else return fail(6);
      set_reg(d_.reg, width == 64 ? 8 : 4, s_.k[d_.rm & 7] & (width == 64 ? ~uint64_t{0} : (uint64_t{1} << width) - 1));
      break;
    }
    case 0x98: case 0x99: {      // kortest, ktest
      if (vex_.wide || d_.mod != 3 || !bits) return fail(6);
      const uint64_t a = s_.k[d_.reg & 7] & mask, b = s_.k[d_.rm & 7] & mask;
      bool zf, cf;
      if (op == 0x98) {
        zf = (a | b) == 0;
        cf = (a | b) == mask;
      } else {
        zf = (a & b) == 0;
        cf = (~a & b & mask) == 0;
      }
      s_.rflags &= ~(kCarry | kParity | kAdjust | kZero | kSign | kOverflow);
      if (zf) s_.rflags |= kZero;
      if (cf) s_.rflags |= kCarry;
      break;
    }
    default: return fail(6);
  }
  s_.rip += d_.position;
  return Step::Ok;
}


// --- operand helpers --------------------------------------------------------------------------------------------------------------------------

// Reads the r/m operand as a full vector (a register, memory, or a broadcast element). Masked-off elements of a memory operand are not touched when `suppress_faults` is set.
bool X86_64Cpu::ev_read_rm(GuestMemory& m, Vec* out, uint32_t elem_bits, bool allow_broadcast, bool suppress_faults) {
  if (!ev_.memory) {
    *out = get_vec(ev_.rm);
    return true;
  }
  if (ev_.bcst && allow_broadcast) {
    uint64_t v;
    if (!load(m, memory_address(), elem_bits / 8, &v)) return false;
    for (uint32_t i = 0; i < 512 / elem_bits; ++i) set_element(*out, elem_bits, i, v);
    return true;
  }
  const uint32_t bytes_per = elem_bits / 8, vl_bytes = ev_.vl / 8;
  for (uint32_t i = 0; i < vl_bytes / bytes_per; ++i) {
    if (suppress_faults && ev_.aaa != 0 && !ev_active(i)) continue;
    for (uint32_t b = 0; b < bytes_per; b += 8) {
      const uint32_t n = bytes_per - b < 8 ? bytes_per - b : 8;
      uint64_t v;
      if (!load(m, memory_address() + uint64_t{i} * bytes_per + b, n, &v)) return false;
      const uint32_t at = i * bytes_per + b;
      for (uint32_t k = 0; k < n; ++k) out->q[(at + k) / 8] |= ((v >> (8 * k)) & 0xFF) << (8 * ((at + k) & 7));
    }
  }
  return true;
}

// Writes `result` to a destination register, applying the mask (merging or zeroing) per element.
void X86_64Cpu::ev_write_masked(uint32_t dest, const Vec& result, uint32_t elem_bits) {
  Vec out = result;
  if (ev_.aaa != 0) {
    const Vec old = dest == ev_.reg ? ev_.old_dest : get_vec(dest);
    for (uint32_t i = 0; i < ev_.vl / elem_bits; ++i) {
      if (!ev_active(i)) set_element(out, elem_bits, i, ev_.z ? 0 : element(old, elem_bits, i));
    }
  }
  set_vec(dest, out, ev_.vl);
}

void X86_64Cpu::ev_write_k(uint64_t bits, uint32_t count) {
  uint64_t r = bits;
  if (ev_.aaa != 0) r &= ev_.kmask;
  if (count < 64) r &= (uint64_t{1} << count) - 1;
  s_.k[ev_.reg & 7] = r;
}

// A masked store of `bytes` bytes of `v` as elements of the given width.
bool X86_64Cpu::ev_store_vector(GuestMemory& m, const Vec& v, uint32_t elem_bits, uint32_t bytes) {
  const uint32_t bytes_per = elem_bits / 8;
  for (uint32_t i = 0; i < bytes / bytes_per; ++i) {
    if (ev_.aaa != 0 && !ev_active(i)) continue;
    for (uint32_t b = 0; b < bytes_per; b += 8) {
      const uint32_t n = bytes_per - b < 8 ? bytes_per - b : 8;
      uint64_t x = 0;
      for (uint32_t k = 0; k < n; ++k) x |= ((v.q[(i * bytes_per + b + k) / 8] >> (8 * ((i * bytes_per + b + k) & 7))) & 0xFF) << (8 * k);
      if (!store(m, memory_address() + uint64_t{i} * bytes_per + b, n, x)) return false;
    }
  }
  return true;
}

// --- the EVEX prefix and the instructions --------------------------------------------------------------------------------------------------------

X86_64Cpu::Step X86_64Cpu::step_evex(GuestMemory& m) {
  auto bad_decode = [&]() { return fail(d_.available < 15 ? 14 : 6, s_.rip + d_.available); };
  if (d_.operand16 || d_.rep || d_.repne || d_.rex_present) return fail(6);
  const uint8_t p0 = next_u8(), p1 = next_u8(), p2 = next_u8();
  const uint8_t opcode = next_u8();
  if (d_.truncated) return bad_decode();
  if ((p0 & 7) == 4) return apx_map4(m, p0, p1, p2, opcode);      // Intel APX: the promoted legacy instructions
  if ((p0 & 0x08) != 0 || !(p1 & 4)) return fail(6);
  const uint32_t map = p0 & 7;
  if (map == 0) return fail(6);
  const bool w = p1 >> 7;
  const uint32_t pp = p1 & 3, vvvv_low = (~(p1 >> 3)) & 15;
  const bool bcst = (p2 >> 4) & 1;
  uint32_t vl = 128u << ((p2 >> 5) & 3);
  d_.rex_present = true;
  d_.rex_w = w;
  d_.rex_r = !(p0 & 0x80);
  d_.rex_x = !(p0 & 0x40);
  d_.rex_b = !(p0 & 0x20);
  const bool r_high = !(p0 & 0x10);
  const uint32_t vvvv = vvvv_low | (!(p2 & 8) ? 16 : 0);
  d_.operand16 = pp == 1;
  d_.rep = pp == 2;
  d_.repne = pp == 3;
  vex_.active = true;
  vex_.wide = vl >= 256;
  vex_.vvvv = vvvv;

  const EvexOp* def = nullptr;
  for (uint32_t i = 0; i < kEvexOpCount; ++i) {
    const EvexOp& o = kEvexOps[i];
    if (o.map == map && o.pp == pp && o.opcode == opcode && (o.w < 0 || o.w == static_cast<int>(w))) {
      def = &o;
      break;
    }
  }
  for (uint32_t i = 0; !def && i < kEvexFp16OpCount; ++i) {
    const EvexOp& o = kEvexFp16Ops[i];
    if (o.map == map && o.pp == pp && o.opcode == opcode && (o.w < 0 || o.w == static_cast<int>(w))) def = &o;
  }
  for (uint32_t i = 0; !def && i < kEvexBf16OpCount; ++i) {
    const EvexOp& o = kEvexBf16Ops[i];
    if (o.map == map && o.pp == pp && o.opcode == opcode && (o.w < 0 || o.w == static_cast<int>(w))) def = &o;
  }
  if (!def && map == 2 && pp == 1 && (opcode == 0xC6 || opcode == 0xC7)) {      // AVX-512 PF: vgather/vscatterpf0/1 d/q ps/pd: prefetch hints, they do nothing and cannot fault
    if (!decode_modrm()) return bad_decode();
    const uint32_t kind = d_.reg & 7;
    if (!finish_decode()) return bad_decode();
    if (d_.mod == 3 || (kind != 1 && kind != 2 && kind != 5 && kind != 6) || vl != 512) return fail(6);
    s_.rip += d_.position;
    return Step::Ok;
  }
  if (!def) return fail(6);
  const uint32_t elem = def->elem ? def->elem : (w ? 64 : 32);

  if (def->kind == kGather || def->kind == kScatter) {
    ev_ = EvexContext();
    ev_.w = w;
    ev_.z = p2 >> 7;
    ev_.aaa = p2 & 7;
    ev_.vl = vl;
    ev_.vvvv = vvvv;
    ev_.reg = r_high ? 16 : 0;
    return evex_vsib(m, *def);
  }
  if (!decode_modrm()) return bad_decode();
  const bool memory = d_.mod != 3;
  if (((p2 >> 5) & 3) == 3 && (memory || !bcst)) return fail(6);      // (with a register operand, EVEX.b makes L'L a rounding mode)
  ev_ = EvexContext();
  ev_.map = static_cast<uint8_t>(map);
  ev_.pp = static_cast<uint8_t>(pp);
  ev_.opcode = opcode;
  ev_.w = w;
  ev_.memory = memory;
  ev_.bcst = bcst && memory;
  ev_.er = bcst && !memory;
  ev_.rc = (p2 >> 5) & 3;
  ev_.z = p2 >> 7;
  ev_.aaa = p2 & 7;
  ev_.reg = d_.reg | (r_high ? 16 : 0);
  ev_.rm = d_.rm | ((!memory && d_.rex_x) ? 16 : 0);
  ev_.vvvv = vvvv;
  if (ev_.er) vl = 512;
  ev_.vl = vl;
  ev_.kmask = ev_.aaa == 0 ? ~uint64_t{0} : s_.k[ev_.aaa];
  // disp8 * N
  if (memory && d_.mod == 1) {
    uint32_t n;
    const uint32_t vb = vl / 8;
    switch (def->tuple) {
      case kFullMem: n = (def->kind == kDup && def->sub == 6 && vl == 128) ? 8 : vb; break;
      case kFull: n = ev_.bcst ? (def->kind == kCvt || def->map >= 5 || (elem == 16 && def->kind >= kFpBin && def->kind <= kRsqrt14) || def->kind == kBfloat ? elem / 8 : (w ? 8 : 4)) : vb; break;
      case kHalf: n = ev_.bcst ? (def->kind == kCvt || def->map >= 5 || (elem == 16 && def->kind >= kFpBin && def->kind <= kRsqrt14) || def->kind == kBfloat ? elem / 8 : (w ? 8 : 4)) : vb / 2; break;
      case kQuarter: n = ev_.bcst ? elem / 8 : vb / 4; break;
      case kEighth: n = vb / 8; break;
      case kScalar: n = elem / 8; break;
      case kMem128: n = 16; break;
      case kTuple2: n = 2 * elem / 8; break;
      case kTuple4: n = 4 * elem / 8; break;
      case kTuple8: n = 8 * elem / 8; break;
      case kS1: n = 1; break;
      case kS2: n = 2; break;
      case kS4: n = 4; break;
      case kS8: n = 8; break;
      case kS16: n = 16; break;
      case kS32: n = 32; break;
      default: n = vb; break;
    }
    d_.ea += static_cast<uint64_t>(d_.displacement) * (n - 1);
  }
  const bool has_imm = def->map == 3 || def->kind == kShiftImm || def->kind == kFpCmp || (def->kind == kShufImm) || def->kind == kInsertElem || def->kind == kExtractElem ||
                       (def->kind == kLane && (def->sub == kShufps || def->sub == kShufpd));
  if (has_imm) ev_.imm = static_cast<uint32_t>(next_imm(1, false));
  if (!finish_decode()) return bad_decode();
  ev_.old_dest = get_vec(ev_.reg);
  switch (def->kind) {
    case kMove: case kIntBin: case kMulDq: case kLane: case kShiftCount: case kShiftImm: case kShiftVar: case kRotVar: case kCmpEq: case kCmpImm: case kTest: case kTernlog: case kVp2Intersect: case kVnni4:
      return evex_int(m, *def);
    case kComi: case kFpBin: case kFpScalar: case kFpUnary: case kFma: case kFpCmp: case kCvt: case kCvtScalar: case kCvtToGpr: case kCvtFromGpr: case kRound: case kGetExp: case kGetMant:
    case kScalef: case kFixup: case kReduce: case kRange: case kFpClass: case kRcp14: case kRsqrt14:
      return evex_fp(m, *def);
    case kComplex: return evex_fp16(m, *def);
    case kFma4: return evex_fp(m, *def);
    case kEr: return evex_er(m, *def);
    case kBfloat: return evex_bf16(m, *def);
    case kMinMax: case kComx: return evex_minmax(m, *def);
    case kDpPh: case kMpsadbw: case kSatCvt: return evex_avx102(m, *def);
    case kDot: case kVnni: case kMadd52: case kMultishift: case kShufBit: case kShift2: case kShift2Var: case kGfAffine: case kGfMul: case kBf16: case kAesNi: case kClmul: case kBroadcastMask: case kDbpsadbw:
      return evex_ext(m, *def);
    default: return evex_misc(m, *def);
  }
}

X86_64Cpu::Step X86_64Cpu::evex_int(GuestMemory& m, const EvexOp& defn) {
  const EvexOp* def = &defn;
  const uint32_t reg = ev_.reg, vvvv = ev_.vvvv, vl = ev_.vl, aaa = ev_.aaa, imm = ev_.imm;
  const bool memory = ev_.memory, bcst = ev_.bcst, z = ev_.z, w = ev_.w;
  const uint32_t rm_reg = ev_.rm;
  const uint32_t elem = def->elem ? def->elem : (w ? 64 : 32);
  const uint32_t vl_bytes = vl / 8;
  const Vec old_dest = ev_.old_dest;
  auto active = [&](uint32_t i) { return ev_active(i); };
  auto read_rm = [&](Vec* out, uint32_t elem_bits, bool allow_broadcast) {
    return ev_read_rm(m, out, elem_bits, allow_broadcast, def->kind == kIntBin || def->kind == kMove);
  };
  auto write_masked = [&](uint32_t dest, const Vec& result, uint32_t elem_bits) { ev_write_masked(dest, result, elem_bits); };
  auto write_mask_register = [&](uint64_t bits_result, uint32_t count) { ev_write_k(bits_result, count); };
  auto store_vector = [&](const Vec& v, uint32_t elem_bits) { return ev_store_vector(m, v, elem_bits, vl_bytes); };
  auto finish = [&]() {
    s_.rip += d_.position;
    return Step::Ok;
  };
  Vec a = get_vec(vvvv), b, result;
  const uint32_t count = vl / elem;

  switch (def->kind) {
    case kMove: {
      const bool is_store = def->sub == 1;
      if (is_store) {
        Vec src = get_vec(reg);
        if (memory) {
          if (!store_vector(src, elem)) return Step::Fault;
          return finish();
        }
        const Vec old = get_vec(rm_reg);
        for (uint32_t i = 0; i < count; ++i) {
          if (aaa != 0 && !active(i)) set_element(src, elem, i, z ? 0 : element(old, elem, i));
        }
        set_vec(rm_reg, src, vl);
        return finish();
      }
      if (!read_rm(&b, elem, false)) return Step::Fault;
      write_masked(reg, b, elem);
      return finish();
    }
    case kIntBin: {
      if (!read_rm(&b, elem, def->tuple == kFull)) return Step::Fault;
      if (bcst && memory && def->tuple != kFull) return fail(6);
      for (uint32_t i = 0; i < count; ++i) set_element(result, elem, i, evex_integer_op(def->sub, elem, element(a, elem, i), element(b, elem, i)));
      write_masked(reg, result, elem);
      return finish();
    }
    case kMulDq: {      // vpmuludq / vpmuldq: the even dwords make qword products
      if (!read_rm(&b, 64, true)) return Step::Fault;
      for (uint32_t i = 0; i < vl / 64; ++i) {
        const uint64_t x = element(a, 32, 2 * i), y = element(b, 32, 2 * i);
        result.q[i] = def->sub ? x * y : static_cast<uint64_t>(signed_of(x, 32) * signed_of(y, 32));
      }
      write_masked(reg, result, 64);
      return finish();
    }
    case kLane: {
      if (!read_rm(&b, def->tuple == kFull ? (w ? 64 : 32) : elem, def->tuple == kFull)) return Step::Fault;
      const uint32_t lanes = vl / 128;
      uint32_t out_elem = def->elem;
      for (uint32_t l = 0; l < lanes; ++l) {
        uint8_t x[16], y[16], r[16] = {};
        lane_bytes(a, l, x);
        lane_bytes(b, l, y);
        Vec la, lb, lr;
        la.q[0] = a.q[2 * l];
        la.q[1] = a.q[2 * l + 1];
        lb.q[0] = b.q[2 * l];
        lb.q[1] = b.q[2 * l + 1];
        switch (def->sub) {
          case kUnpackLo: case kUnpackHi: {
            const uint32_t w2 = def->elem, n = 128 / w2, base = def->sub == kUnpackLo ? 0 : n / 2;
            for (uint32_t i = 0; i < n / 2; ++i) {
              set_element(lr, w2, 2 * i, element(la, w2, base + i));
              set_element(lr, w2, 2 * i + 1, element(lb, w2, base + i));
            }
            break;
          }
          case kUnpcklps: case kUnpckhps: {
            const uint32_t base = def->sub == kUnpcklps ? 0 : 2;
            for (uint32_t i = 0; i < 2; ++i) {
              set_element(lr, 32, 2 * i, element(la, 32, base + i));
              set_element(lr, 32, 2 * i + 1, element(lb, 32, base + i));
            }
            break;
          }
          case kPacksswb: case kPackuswb: case kPackssdw: case kPackusdw: {
            const uint32_t src_w = (def->sub == kPacksswb || def->sub == kPackuswb) ? 16 : 32, dst_w = src_w / 2, n = 128 / src_w;
            const bool is_unsigned = def->sub == kPackuswb || def->sub == kPackusdw;
            for (uint32_t i = 0; i < n; ++i) {
              const int64_t xv = signed_of(element(la, src_w, i), src_w), yv = signed_of(element(lb, src_w, i), src_w);
              set_element(lr, dst_w, i, is_unsigned ? evex_saturate_unsigned(xv, dst_w) : evex_saturate_signed(xv, dst_w));
              set_element(lr, dst_w, n + i, is_unsigned ? evex_saturate_unsigned(yv, dst_w) : evex_saturate_signed(yv, dst_w));
            }
            break;
          }
          case kShufps:
            set_element(lr, 32, 0, element(la, 32, imm & 3));
            set_element(lr, 32, 1, element(la, 32, (imm >> 2) & 3));
            set_element(lr, 32, 2, element(lb, 32, (imm >> 4) & 3));
            set_element(lr, 32, 3, element(lb, 32, (imm >> 6) & 3));
            break;
          case kShufpd:
            set_element(lr, 64, 0, element(la, 64, (imm >> (2 * l)) & 1));
            set_element(lr, 64, 1, element(lb, 64, (imm >> (2 * l + 1)) & 1));
            break;
          case kPalignr:
            for (uint32_t i = 0; i < 16; ++i) {
              const uint32_t idx = i + (imm & 0xFF);
              r[i] = idx < 16 ? y[idx] : idx < 32 ? x[idx - 16] : 0;
            }
            set_lane_bytes(lr, 0, r);
            break;
          case kPshufb:
            for (uint32_t i = 0; i < 16; ++i) r[i] = (y[i] & 0x80) ? 0 : x[y[i] & 15];
            set_lane_bytes(lr, 0, r);
            break;
          case kMaddwd:
            for (uint32_t i = 0; i < 4; ++i) {
              const int64_t t = signed_of(element(la, 16, 2 * i), 16) * signed_of(element(lb, 16, 2 * i), 16) + signed_of(element(la, 16, 2 * i + 1), 16) * signed_of(element(lb, 16, 2 * i + 1), 16);
              set_element(lr, 32, i, static_cast<uint64_t>(t));
            }
            break;
          case kMaddubsw:
            for (uint32_t i = 0; i < 8; ++i) {
              const int64_t t = static_cast<int64_t>(x[2 * i]) * static_cast<int8_t>(y[2 * i]) + static_cast<int64_t>(x[2 * i + 1]) * static_cast<int8_t>(y[2 * i + 1]);
              set_element(lr, 16, i, evex_saturate_signed(t, 16));
            }
            break;
          default:      // psadbw
            for (uint32_t h = 0; h < 2; ++h) {
              uint64_t sum = 0;
              for (uint32_t i = 0; i < 8; ++i) sum += x[8 * h + i] > y[8 * h + i] ? x[8 * h + i] - y[8 * h + i] : y[8 * h + i] - x[8 * h + i];
              lr.q[h] = sum;
            }
            break;
        }
        result.q[2 * l] = lr.q[0];
        result.q[2 * l + 1] = lr.q[1];
      }
      if (def->sub == kMaddwd) out_elem = 32;
      else if (def->sub == kMaddubsw) out_elem = 16;
      else if (def->sub == kPacksswb || def->sub == kPackuswb) out_elem = 8;
      else if (def->sub == kPackssdw || def->sub == kPackusdw) out_elem = 16;
      write_masked(reg, result, out_elem);
      return finish();
    }
    case kShiftCount: {      // the count is the low qword of an XMM register or 128 bits of memory
      uint64_t cnt;
      if (memory) {
        if (!load(m, memory_address(), 8, &cnt)) return Step::Fault;
        uint64_t hi;
        if (!load(m, memory_address() + 8, 8, &hi)) return Step::Fault;
      } else {
        cnt = get_vec(rm_reg).q[0];
      }
      for (uint32_t i = 0; i < count; ++i) {
        const uint64_t x = element(a, elem, i);
        uint64_t r;
        if (def->sub == 2) r = static_cast<uint64_t>(signed_of(x, elem) >> (cnt >= elem ? elem - 1 : cnt));
        else if (cnt >= elem) r = 0;
        else r = def->sub == 0 ? x << cnt : x >> cnt;
        set_element(result, elem, i, r & width_mask(elem));
      }
      write_masked(reg, result, elem);
      return finish();
    }
    case kShiftImm: {      // group opcodes 71/72/73: the destination is vvvv, the source the r/m operand, the operation the ModRM.reg field
      const uint32_t group = (static_cast<uint32_t>(d_.reg) & 7);
      uint32_t e = def->sub;      // 16, 32, 64
      if (def->opcode == 0x72 || def->opcode == 0x73) e = w ? 64 : 32;
      if (def->opcode == 0x73) e = 64;
      if (def->opcode == 0x72 && w) e = 64;
      const uint32_t dest = vvvv;
      if (!read_rm(&b, e, def->opcode != 0x71)) return Step::Fault;
      const uint32_t n = vl / e;
      bool bytes_shift = false;
      uint32_t op_kind;      // 0 left, 1 right logical, 2 right arithmetic, 3 rotate left, 4 rotate right
      if (def->opcode == 0x71) {
        if (group == 2) op_kind = 1;
        else if (group == 4) op_kind = 2;
        else if (group == 6) op_kind = 0;
        else return fail(6);
      } else if (def->opcode == 0x72) {
        if (group == 2) op_kind = 1;
        else if (group == 4) op_kind = 2;
        else if (group == 6) op_kind = 0;
        else if (group == 0) op_kind = 4;
        else if (group == 1) op_kind = 3;
        else return fail(6);
      } else {
        if (group == 2) op_kind = 1;
        else if (group == 6) op_kind = 0;
        else if (group == 3 || group == 7) {
          op_kind = group == 3 ? 1 : 0;
          bytes_shift = true;
          if (!w && false) return fail(6);
        } else return fail(6);
        if (bytes_shift) {
          if (aaa != 0) return fail(6);
          if (!memory) {}
        }
      }
      if (def->opcode == 0x73 && !bytes_shift && !w) return fail(6);
      if (def->opcode == 0x71 && w) {}
      if (bytes_shift) {
        for (uint32_t l = 0; l < vl / 128; ++l) {
          uint8_t x[16], r[16] = {};
          lane_bytes(b, l, x);
          for (uint32_t i = 0; i < 16; ++i) {
            if (op_kind == 1) r[i] = i + imm < 16 ? x[i + imm] : 0;
            else r[i] = i >= imm ? x[i - imm] : 0;
          }
          set_lane_bytes(result, l, r);
        }
        set_vec(dest, result, vl);
        return finish();
      }
      for (uint32_t i = 0; i < n; ++i) {
        const uint64_t x = element(b, e, i);
        uint64_t r;
        const uint32_t c = imm & 0xFF;
        switch (op_kind) {
          case 0: r = c >= e ? 0 : x << c; break;
          case 1: r = c >= e ? 0 : x >> c; break;
          case 2: r = static_cast<uint64_t>(signed_of(x, e) >> (c >= e ? e - 1 : c)); break;
          case 3: r = (c % e) == 0 ? x : ((x << (c % e)) | (x >> (e - c % e))); break;
          default: r = (c % e) == 0 ? x : ((x >> (c % e)) | (x << (e - c % e))); break;
        }
        set_element(result, e, i, r & width_mask(e));
      }
      // the mask applies per element of the destination (merging into the old destination, which is vvvv)
      {
        Vec out = result;
        if (aaa != 0) {
          const Vec old = get_vec(dest);
          for (uint32_t i = 0; i < n; ++i) {
            if (!active(i)) set_element(out, e, i, z ? 0 : element(old, e, i));
          }
        }
        set_vec(dest, out, vl);
      }
      return finish();
    }
    case kShiftVar: case kRotVar: {
      if (!read_rm(&b, elem, def->tuple == kFull)) return Step::Fault;
      for (uint32_t i = 0; i < count; ++i) {
        const uint64_t x = element(a, elem, i), c = element(b, elem, i);
        uint64_t r;
        if (def->kind == kRotVar) {
          const uint32_t s = static_cast<uint32_t>(c % elem);
          r = s == 0 ? x : (def->sub == 1 ? ((x >> s) | (x << (elem - s))) : ((x << s) | (x >> (elem - s))));
        } else if (def->sub == 2) {
          r = static_cast<uint64_t>(signed_of(x, elem) >> (c >= elem ? elem - 1 : c));
        } else if (c >= elem) {
          r = 0;
        } else {
          r = def->sub == 0 ? x << c : x >> c;
        }
        set_element(result, elem, i, r & width_mask(elem));
      }
      write_masked(reg, result, elem);
      return finish();
    }
    case kCmpEq: case kCmpImm: case kTest: {
      if (!read_rm(&b, elem, def->tuple == kFull)) return Step::Fault;
      uint64_t bits_out = 0;
      const uint32_t predicate = def->kind == kCmpImm ? (imm & 7) : 0;
      if (def->kind == kCmpImm && (imm & 0xF8)) return fail(6);
      for (uint32_t i = 0; i < count; ++i) {
        const uint64_t x = element(a, elem, i), y = element(b, elem, i);
        bool r;
        if (def->kind == kCmpEq) {
          r = def->sub == 0 ? x == y : signed_of(x, elem) > signed_of(y, elem);
        } else if (def->kind == kTest) {
          r = ((x & y) != 0) != (def->sub == 1);
        } else {
          const bool less = def->sub ? signed_of(x, elem) < signed_of(y, elem) : x < y;
          switch (predicate) {
            case 0: r = x == y; break;
            case 1: r = less; break;
            case 2: r = less || x == y; break;
            case 3: r = false; break;
            case 4: r = x != y; break;
            case 5: r = !less; break;
            case 6: r = !less && x != y; break;
            default: r = true; break;
          }
        }
        if (r) bits_out |= uint64_t{1} << i;
      }
      write_mask_register(bits_out, count);
      return finish();
    }
    case kVp2Intersect: {      // vp2intersectd/q k, zmm, zmm/m: the destination is the mask pair (reg & 6, reg | 1); no write mask, no rounding
      if (aaa != 0 || z || ev_.er) return fail(6);
      if (!read_rm(&b, elem, true)) return Step::Fault;
      uint64_t first = 0, second = 0;
      for (uint32_t i = 0; i < count; ++i) {
        for (uint32_t j = 0; j < count; ++j) {
          if (element(a, elem, i) != element(b, elem, j)) continue;
          first |= uint64_t{1} << i;
          second |= uint64_t{1} << j;
        }
      }
      s_.k[d_.reg & 6] = first;
      s_.k[(d_.reg & 6) | 1] = second;
      return finish();
    }
    case kVnni4: {      // Xeon Phi 4VNNIW: vp4dpwssd / vp4dpwssds, four vpdpwssd steps over the register group (vvvv & ~3 ..+3) with the dwords of the m128 operand as the word pairs
      if (!memory || bcst || vl != 512) return fail(6);
      int32_t pairs[4][2];
      for (uint32_t j = 0; j < 4; ++j) {
        uint64_t v;
        if (!load(m, memory_address() + 4 * j, 4, &v)) return Step::Fault;
        pairs[j][0] = static_cast<int16_t>(v);
        pairs[j][1] = static_cast<int16_t>(v >> 16);
      }
      Vec group[4];
      for (uint32_t j = 0; j < 4; ++j) group[j] = get_vec((vvvv & ~3u) + j);
      for (uint32_t i = 0; i < count; ++i) {
        int64_t acc = static_cast<int32_t>(element(old_dest, 32, i));
        for (uint32_t j = 0; j < 4; ++j) {
          const uint64_t x = element(group[j], 32, i);
          acc += int64_t{static_cast<int16_t>(x)} * pairs[j][0] + int64_t{static_cast<int16_t>(x >> 16)} * pairs[j][1];
          if (def->sub) acc = acc > INT32_MAX ? INT32_MAX : acc < INT32_MIN ? INT32_MIN : acc;
          else acc = static_cast<int32_t>(static_cast<uint32_t>(acc));
        }
        set_element(result, 32, i, static_cast<uint32_t>(acc));
      }
      write_masked(reg, result, 32);
      return finish();
    }
    case kTernlog: {
      if (!read_rm(&b, elem, true)) return Step::Fault;
      for (uint32_t i = 0; i < vl / 64; ++i) {
        uint64_t r = 0;
        const uint64_t x = old_dest.q[i], y = a.q[i], zz = b.q[i];
        for (uint32_t bit = 0; bit < 8; ++bit) {
          if (!((imm >> bit) & 1)) continue;
          r |= ((bit & 4) ? x : ~x) & ((bit & 2) ? y : ~y) & ((bit & 1) ? zz : ~zz);
        }
        result.q[i] = r;
      }
      write_masked(reg, result, elem);
      return finish();
    }

    default: return fail(6);
  }
}

}  // namespace emulation
