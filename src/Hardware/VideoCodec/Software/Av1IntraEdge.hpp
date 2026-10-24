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

#pragma once
#include <stdint.h>
#include "Hardware/VideoCodec/Software/Av1Types.hpp"

// Intra edge availability tree of the AV1 decoder (port of dav1d's intra_edge.c, BSD-2-Clause, Copyright (c) VideoLAN and dav1d authors).
// While the decoder walks the partition tree of a superblock it walks this tree in step: every node says which neighbours (top right, bottom
// left) of the blocks at that position are already decoded, as EdgeFlags that are handed to the block reconstruction.
//
//   const EdgeNode& root = k_intra_edge_tree.root(sb128 ? BL_128X128 : BL_64X64);
//   decode_b(..., PARTITION_NONE, root.o);   PARTITION_H -> root.h[0], root.h[1];  PARTITION_V -> root.v[0], root.v[1];
//   PARTITION_SPLIT -> recurse into k_intra_edge_tree.split(node, 0..3) (not at BL_8X8: there the tip's split[0..2] are the flags of the
//   three sub8x8 blocks); PARTITION_T_* use h[0|1] / v[0|1] as in dav1d's decode_sb; PARTITION_H4 uses h[0], then h4 (three times) or at
//   BL_8X8 h[1]; PARTITION_V4 likewise with v[0], v4, v[1].
// This is dav1d's `dav1d_intra_edge_tree[bl]` / `INTRA_EDGE_SPLIT` / `EdgeBranch::h4,v4` / `EdgeTip::split` with indices instead of pointers.
// The tree is built at compile time (k_intra_edge_tree), so there is nothing to initialise.

namespace hardware {
namespace av1 {

enum EdgeFlags {
    EDGE_I444_TOP_HAS_RIGHT   = 1 << 0,
    EDGE_I422_TOP_HAS_RIGHT   = 1 << 1,
    EDGE_I420_TOP_HAS_RIGHT   = 1 << 2,
    EDGE_I444_LEFT_HAS_BOTTOM = 1 << 3,
    EDGE_I422_LEFT_HAS_BOTTOM = 1 << 4,
    EDGE_I420_LEFT_HAS_BOTTOM = 1 << 5,
    EDGE_ALL_TOP_HAS_RIGHT    = EDGE_I444_TOP_HAS_RIGHT | EDGE_I422_TOP_HAS_RIGHT | EDGE_I420_TOP_HAS_RIGHT,
    EDGE_ALL_LEFT_HAS_BOTTOM  = EDGE_I444_LEFT_HAS_BOTTOM | EDGE_I422_LEFT_HAS_BOTTOM | EDGE_I420_LEFT_HAS_BOTTOM,
    EDGE_ALL_TR_AND_BL        = EDGE_ALL_TOP_HAS_RIGHT | EDGE_ALL_LEFT_HAS_BOTTOM,
};

// One node of the tree. `o`, `h`, `v` are valid for every node. `h4`/`v4` are valid for branches (every level above BL_8X8), `split` for tips
// (BL_8X8 nodes). `child` are node indices (see IntraEdgeTree::split), valid for branches only.
struct EdgeNode {
    uint8_t o = 0;
    uint8_t h[2] = {0, 0};
    uint8_t v[2] = {0, 0};
    uint8_t h4 = 0, v4 = 0;
    uint8_t split[3] = {0, 0, 0};
    int16_t child[4] = {-1, -1, -1, -1};
};

class IntraEdgeTree {
 public:
    constexpr IntraEdgeTree() {
        // sb128: 1 + 4 + 16 + 64 branches and 256 tips; sb64: 1 + 4 + 16 branches and 64 tips.
        int next = 0;
        roots_[0] = static_cast<int16_t>(build(BL_128X128, true, false, next));
        roots_[1] = static_cast<int16_t>(build(BL_64X64, true, false, next));
    }

    // The root of the tree of a 128x128 (BL_128X128) or 64x64 (BL_64X64) superblock.
    constexpr const EdgeNode& root(BlockLevel bl) const { return nodes_[bl == BL_128X128 ? roots_[0] : roots_[1]]; }
    // INTRA_EDGE_SPLIT(node, i): the node of quadrant i of a branch.
    constexpr const EdgeNode& split(const EdgeNode& node, int i) const { return nodes_[static_cast<unsigned>(node.child[i & 3])]; }

 private:
    static constexpr int kNodes = (1 + 4 + 16 + 64 + 256) + (1 + 4 + 16 + 64);
    EdgeNode nodes_[kNodes];
    int16_t roots_[2] = {0, 0};

    // dav1d's init_edges
    constexpr void init_edges(EdgeNode& node, BlockLevel bl, int edge_flags) {
        node.o = static_cast<uint8_t>(edge_flags);
        node.h[0] = static_cast<uint8_t>(edge_flags | EDGE_ALL_LEFT_HAS_BOTTOM);
        node.v[0] = static_cast<uint8_t>(edge_flags | EDGE_ALL_TOP_HAS_RIGHT);
        if (bl == BL_8X8) {
            node.h[1] = static_cast<uint8_t>(edge_flags & (EDGE_ALL_LEFT_HAS_BOTTOM | EDGE_I420_TOP_HAS_RIGHT));
            node.v[1] = static_cast<uint8_t>(edge_flags & (EDGE_ALL_TOP_HAS_RIGHT | EDGE_I420_LEFT_HAS_BOTTOM | EDGE_I422_LEFT_HAS_BOTTOM));
            node.split[0] = static_cast<uint8_t>((edge_flags & EDGE_ALL_TOP_HAS_RIGHT) | EDGE_I422_LEFT_HAS_BOTTOM);
            node.split[1] = static_cast<uint8_t>(edge_flags | EDGE_I444_TOP_HAS_RIGHT);
            node.split[2] = static_cast<uint8_t>(edge_flags & (EDGE_I420_TOP_HAS_RIGHT | EDGE_I420_LEFT_HAS_BOTTOM | EDGE_I422_LEFT_HAS_BOTTOM));
        } else {
            node.h[1] = static_cast<uint8_t>(edge_flags & EDGE_ALL_LEFT_HAS_BOTTOM);
            node.v[1] = static_cast<uint8_t>(edge_flags & EDGE_ALL_TOP_HAS_RIGHT);
            int h4 = EDGE_ALL_LEFT_HAS_BOTTOM;
            int v4 = EDGE_ALL_TOP_HAS_RIGHT;
            if (bl == BL_16X16) {
                h4 |= edge_flags & EDGE_I420_TOP_HAS_RIGHT;
                v4 |= edge_flags & (EDGE_I420_LEFT_HAS_BOTTOM | EDGE_I422_LEFT_HAS_BOTTOM);
            }
            node.h4 = static_cast<uint8_t>(h4);
            node.v4 = static_cast<uint8_t>(v4);
        }
    }

    // dav1d's init_mode_node, with the node storage handed out in depth-first order. Returns the index of the node.
    constexpr int build(BlockLevel bl, bool top_has_right, bool left_has_bottom, int& next) {
        const int index = next++;
        init_edges(nodes_[index], bl, (top_has_right ? EDGE_ALL_TOP_HAS_RIGHT : 0) | (left_has_bottom ? EDGE_ALL_LEFT_HAS_BOTTOM : 0));
        for (int n = 0; n < 4; n++) {
            if (bl == BL_16X16) {
                const int tip = next++;
                init_edges(nodes_[tip], BL_8X8,
                           ((n == 3 || (n == 1 && !top_has_right)) ? 0 : EDGE_ALL_TOP_HAS_RIGHT) |
                               (!(n == 0 || (n == 2 && left_has_bottom)) ? 0 : EDGE_ALL_LEFT_HAS_BOTTOM));
                nodes_[index].child[n] = static_cast<int16_t>(tip);
            } else {
                const int child = build(static_cast<BlockLevel>(bl + 1), !(n == 3 || (n == 1 && !top_has_right)),
                                        n == 0 || (n == 2 && left_has_bottom), next);
                nodes_[index].child[n] = static_cast<int16_t>(child);
            }
        }
        return index;
    }
};

inline constexpr IntraEdgeTree k_intra_edge_tree{};

}  // namespace av1
}  // namespace hardware
