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

// VP9 constants, small structures and the normative tables (generated from libvpx by scripts/gen_vp9_tables.py).
#pragma once
#include <stdint.h>

namespace hardware {
namespace vp9 {

// ---- enumerations, under libvpx's names (the generated tables use them) ---------------------------------------------------------
constexpr int BLOCK_4X4 = 0, BLOCK_4X8 = 1, BLOCK_8X4 = 2, BLOCK_8X8 = 3, BLOCK_8X16 = 4, BLOCK_16X8 = 5, BLOCK_16X16 = 6, BLOCK_16X32 = 7,
              BLOCK_32X16 = 8, BLOCK_32X32 = 9, BLOCK_32X64 = 10, BLOCK_64X32 = 11, BLOCK_64X64 = 12, BLOCK_SIZES = 13, BLOCK_INVALID = 13;
constexpr int PARTITION_NONE = 0, PARTITION_HORZ = 1, PARTITION_VERT = 2, PARTITION_SPLIT = 3, PARTITION_TYPES = 4;
constexpr int PARTITION_PLOFFSET = 4, PARTITION_CONTEXTS = 16;
constexpr int TX_4X4 = 0, TX_8X8 = 1, TX_16X16 = 2, TX_32X32 = 3, TX_SIZES = 4;
constexpr int ONLY_4X4 = 0, ALLOW_8X8 = 1, ALLOW_16X16 = 2, ALLOW_32X32 = 3, TX_MODE_SELECT = 4, TX_MODES = 5;
constexpr int DCT_DCT = 0, ADST_DCT = 1, DCT_ADST = 2, ADST_ADST = 3, TX_TYPES = 4;
constexpr int DC_PRED = 0, V_PRED = 1, H_PRED = 2, D45_PRED = 3, D135_PRED = 4, D117_PRED = 5, D153_PRED = 6, D207_PRED = 7, D63_PRED = 8,
              TM_PRED = 9, NEARESTMV = 10, NEARMV = 11, ZEROMV = 12, NEWMV = 13, MB_MODE_COUNT = 14;
constexpr int INTRA_MODES = 10, INTER_MODES = 4;
constexpr int INTRA_FRAME = 0, LAST_FRAME = 1, GOLDEN_FRAME = 2, ALTREF_FRAME = 3, MAX_REF_FRAMES = 4, NO_REF_FRAME = -1;
constexpr int SKIP_CONTEXTS = 3, INTER_MODE_CONTEXTS = 7, INTRA_INTER_CONTEXTS = 4, COMP_INTER_CONTEXTS = 5, REF_CONTEXTS = 5;
constexpr int BLOCK_SIZE_GROUPS = 4, TX_SIZE_CONTEXTS = 2, SWITCHABLE_FILTERS = 3, SWITCHABLE_FILTER_CONTEXTS = 4;
constexpr int EIGHTTAP = 0, EIGHTTAP_SMOOTH = 1, EIGHTTAP_SHARP = 2, BILINEAR = 3, SWITCHABLE = 4;
constexpr int PLANE_TYPES = 2, REF_TYPES = 2, COEF_BANDS = 6, COEFF_CONTEXTS = 6, UNCONSTRAINED_NODES = 3, ENTROPY_TOKENS = 12, MODEL_NODES = 8,
              COEFF_PROB_MODELS = 255;
constexpr int MV_JOINTS = 4, MV_CLASSES = 11, CLASS0_BITS = 1, CLASS0_SIZE = 2, MV_OFFSET_BITS = 10, MV_FP_SIZE = 4;
constexpr int MVREF_NEIGHBOURS = 8, MAX_SEGMENTS = 8, SEG_TREE_PROBS = 7, PREDICTION_PROBS = 3, QINDEX_RANGE = 256;
constexpr int SEG_LVL_ALT_Q = 0, SEG_LVL_ALT_LF = 1, SEG_LVL_REF_FRAME = 2, SEG_LVL_SKIP = 3, SEG_LVL_MAX = 4;
constexpr int MAXQ = 255, MAX_LOOP_FILTER = 63, MAX_PROB = 255;
constexpr int REF_FRAMES = 8, REFS_PER_FRAME = 3, FRAME_CONTEXTS = 4;
constexpr int MV_JOINT_ZERO = 0, MV_JOINT_HNZVZ = 1, MV_JOINT_HZVNZ = 2, MV_JOINT_HNZVNZ = 3;
constexpr int MV_CLASS_0 = 0, MV_CLASS_1 = 1, MV_CLASS_2 = 2, MV_CLASS_3 = 3, MV_CLASS_4 = 4, MV_CLASS_5 = 5, MV_CLASS_6 = 6, MV_CLASS_7 = 7,
              MV_CLASS_8 = 8, MV_CLASS_9 = 9, MV_CLASS_10 = 10;
constexpr int MAX_NEIGHBORS = 2, MODE_MV_COUNT_SAT = 20;
constexpr int BOTH_ZERO = 0, ZERO_PLUS_PREDICTED = 1, BOTH_PREDICTED = 2, NEW_PLUS_NON_INTRA = 3, BOTH_NEW = 4, INTRA_PLUS_NON_INTRA = 5, BOTH_INTRA = 6,
              INVALID_CASE = 9;
constexpr int TREE_SIZE(int leaves) { return 2 * leaves - 2; }
constexpr int INTER_OFFSET(int mode) { return mode - NEARESTMV; }

struct Position {
  int row, col;
};
struct PartitionContext {
  int8_t above, left;
};

struct Mv {
  int16_t row = 0, col = 0;
  bool operator==(const Mv& o) const { return row == o.row && col == o.col; }
  bool operator!=(const Mv& o) const { return !(*this == o); }
};

// ---- probability tables as libvpx lays them out --------------------------------------------------------------------------------
struct TxProbs {
  uint8_t p32x32[TX_SIZE_CONTEXTS][TX_SIZES - 1];
  uint8_t p16x16[TX_SIZE_CONTEXTS][TX_SIZES - 2];
  uint8_t p8x8[TX_SIZE_CONTEXTS][TX_SIZES - 3];
};
struct NmvComponent {
  uint8_t sign;
  uint8_t classes[MV_CLASSES - 1];
  uint8_t class0[CLASS0_SIZE - 1];
  uint8_t bits[MV_OFFSET_BITS];
  uint8_t class0_fp[CLASS0_SIZE][MV_FP_SIZE - 1];
  uint8_t fp[MV_FP_SIZE - 1];
  uint8_t class0_hp;
  uint8_t hp;
};
struct NmvContext {
  uint8_t joints[MV_JOINTS - 1];
  NmvComponent comps[2];
};

using TranLow = int32_t;
using TranHigh = int64_t;

#include "Hardware/VideoCodec/Software/Vp9Tables.inc"

}  // namespace vp9
}  // namespace hardware
