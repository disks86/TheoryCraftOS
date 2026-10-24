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
#include "Allocators/IAllocator.hpp"
#include "Hardware/VideoCodec/Software/Av1Types.hpp"
#include "Utility/ArrayView.hpp"

// Reference motion vectors of the AV1 decoder: a port of dav1d's src/refmvs.c (BSD-2-Clause, Copyright (c) VideoLAN and dav1d authors).
// Single threaded: dav1d's tile-thread/frame-thread logic is gone, so there is exactly one row-buffer set and one tile-row slot.
//
// ---- Lifetime -------------------------------------------------------------------------------------------------------------
//   MotionVectorStore  one per decoded inter (or intrabc-less inter) picture: the 8x8 "saved" motion vectors that later frames
//                      project (temporal MV prediction) plus the order hints of the picture and of its own 7 references. The
//                      picture's reference-slot entries share it (wrap it in a SharedPointer); it is read-only once the frame ended.
//   RefmvsFrame        the per-frame decoder state: sign-bias tables, the row buffers (35 rows of 4x4 RefmvsBlock) and the projected
//                      temporal MVs (16 rows of 8x8 RefmvsTemporalBlock). It can be reused for every frame (buffers are re-used when the
//                      frame width is the same); it borrows the frame header, the current store and the 7 reference stores, which
//                      must stay alive and unchanged until the frame is finished.
//   RefmvsTile         pointers (row indices) of the current tile, set by RefmvsFrame::tile_sbrow_init for every (tile, sb row).
//
// ---- Call sequence (single threaded, per decode_frame in dav1d's decode.c) ---------------------------------------------------
//   Only for frames with (frame_type is INTER or SWITCH) or allow_intrabc:
//   0. MotionVectorStore cur(allocator); cur.allocate(frm_hdr.width[0], frm_hdr.height)      -- only if INTER/SWITCH or intrabc
//   1. rf.init_frame(seq_hdr, frm_hdr, ref_poc, &cur, refs)                                 -- once per frame
//        ref_poc[i]  = frame_offset (order hint) of reference i (RefFrame LAST+i), or all 0 for intrabc frames;
//        refs[i]     = store of the picture in slot frm_hdr.refidx[i], or null if that picture has none (intra frame, intrabc frame,
//                      different size). init_frame itself drops references whose 8x8 size differs from this frame's.
//   For each superblock row sby (by = sby << (4 + sb128) in 4x4 units; by_end = (by + sb_step) >> 1 in 8x8 units):
//   2. if frm_hdr.use_ref_frame_mvs:  rf.load_tmvs(0, iw4 >> 1, by >> 1, by_end)      (iw4 = ((width[0] + 7) >> 3) << 1 = f->bw)
//   3. for each tile (column) of the row, with a RefmvsTile rt that lives across tiles/sb rows:
//        rf.tile_sbrow_init(rt, col_start4, col_end4, row_start4, row_end4, sby)
//        then for every block (decode_b):
//          - neighbours for the contexts:  rf.row(rt, (by & 31) + 5 + dy)[bx + dx]      (dy = -1 is the row above, dx = -1 the left)
//          - inter blocks: rf.find(rt, mvstack, cnt, ctx, ref, bs, top_has_right, by, bx)
//          - after the block is decoded:   rf.splat_mv(rt, block, by, bx, bw4, bh4)       (every block, including intra: mv = INVALID_MV)
//   4. if INTER/SWITCH (not for intrabc):  rf.save_tmvs(rt, 0, iw4 >> 1, by >> 1, by_end)    -- after all tiles of the row
//   After the frame: cur.frame_offset / cur.ref_poc are already set by init_frame; keep `cur` with the picture iff it is INTER/SWITCH
//   (dav1d keeps it for every slot the frame refreshes; for intrabc frames it is NOT stored: refs[...] must be null for those slots).
//
// Data the block decoder provides to splat_mv (dav1d decode.c splat_*_mv):
//   intra / intrabc-less intra block: mv[0].n = INVALID_MV, ref = {0, -1}, bs, mf = 0
//   intrabc:                          mv[0] = the vector, ref = {0, -1}, bs, mf = 0
//   single ref:                       mv[0], ref = {ref0 + 1, interintra ? 0 : -1}, mf = (GLOBALMV && min(bw4, bh4) >= 2) | (NEWMV * 2)
//   compound:                         mv[0..1], ref = {ref0 + 1, ref1 + 1}, mf = (GLOBALMV_GLOBALMV && min(bw4, bh4) >= 2) | (any NEWMV * 2)
// `ref` passed to find: {0, -1} for intrabc, {ref0 + 1, -1} for single, {ref0 + 1, ref1 + 1} for compound.

namespace hardware {
namespace av1 {

constexpr uint32_t INVALID_MV = 0x80008000u;
constexpr int INVALID_REF2CUR = -32;

// One 8x8 cell of the saved motion vectors. `ref` = 0 means "no vector".
struct RefmvsTemporalBlock {
  Mv mv;
  uint8_t ref;
};

struct RefmvsRefPair {
  int8_t ref[2];  // [0] = 0: intra, [1] = -1: not compound (0: interintra)
  bool operator==(const RefmvsRefPair& other) const { return ref[0] == other.ref[0] && ref[1] == other.ref[1]; }
};

struct RefmvsMvPair {
  Mv mv[2];
  bool operator==(const RefmvsMvPair& other) const { return mv[0].n == other.mv[0].n && mv[1].n == other.mv[1].n; }
};

// One 4x4 cell of the current frame (dav1d refmvs_block).
struct RefmvsBlock {
  RefmvsMvPair mv;
  RefmvsRefPair ref;
  uint8_t bs;  // BlockSize
  uint8_t mf;  // 1 = globalmv + affine, 2 = newmv
};

struct RefmvsCandidate {
  RefmvsMvPair mv;
  int weight;
};

// The saved motion vectors of one picture, read by the frames that reference it.
class MotionVectorStore {
 public:
  explicit MotionVectorStore(allocators::IAllocator& allocator) : allocator_(allocator) {}
  ~MotionVectorStore() { release(); }
  MotionVectorStore(const MotionVectorStore&) = delete;
  MotionVectorStore& operator=(const MotionVectorStore&) = delete;
  MotionVectorStore(MotionVectorStore&& other);
  MotionVectorStore& operator=(MotionVectorStore&& other);

  // Allocates (zeroed) storage for a frame of the given coded width (frame_hdr.width[0]) and height. False if out of memory.
  bool allocate(int width, int height);
  void release();
  bool valid() const { return !blocks_.empty(); }
  // init_frame sets these for the current frame; tests (and callers that restore a store) can set them directly.
  void set_order_hints(uint8_t frame_offset, ArrayView<const uint8_t> ref_poc);

  int iw8() const { return iw8_; }
  int ih8() const { return ih8_; }
  int stride() const { return stride_; }
  uint8_t frame_offset() const { return frame_offset_; }
  uint8_t ref_poc(int i) const { return ArrayView<const uint8_t>(ref_poc_)[static_cast<uint64_t>(i)]; }
  ArrayView<RefmvsTemporalBlock> blocks() const { return blocks_; }

 private:
  friend class RefmvsFrame;
  allocators::IAllocator& allocator_;
  ArrayView<RefmvsTemporalBlock> blocks_;
  int iw8_ = 0, ih8_ = 0, stride_ = 0;
  uint8_t frame_offset_ = 0;
  uint8_t ref_poc_[7] = {};
};

class RefmvsFrame;

struct RefmvsTile {
  const RefmvsFrame* rf = nullptr;
  RefmvsTile() {
    for (int32_t& row : r) row = -1;
  }
  int32_t r[32 + 5];  // slot -> row of RefmvsFrame's block buffer, -1 for the unused slots
  struct {
    int start, end;
  } tile_col, tile_row;
};

class RefmvsFrame {
 public:
  explicit RefmvsFrame(allocators::IAllocator& allocator) : allocator_(allocator) {}
  ~RefmvsFrame() { release(); }
  RefmvsFrame(const RefmvsFrame&) = delete;
  RefmvsFrame& operator=(const RefmvsFrame&) = delete;

  // dav1d_refmvs_init_frame. `ref_poc` is the 7 order hints of the references, `cur` the (allocated) store of this frame (its
  // frame_offset/ref_poc are filled in here), `refs` the 7 reference stores (null entries allowed). False if out of memory.
  bool init_frame(const SequenceHeader& seq_hdr, const FrameHeader& frm_hdr, ArrayView<const uint8_t> ref_poc, MotionVectorStore* cur,
                  ArrayView<const MotionVectorStore* const> refs);

  // dav1d_refmvs_tile_sbrow_init (tile_row_idx = 0, pass = 0). Tile bounds in 4x4 units.
  void tile_sbrow_init(RefmvsTile& rt, int tile_col_start4, int tile_col_end4, int tile_row_start4, int tile_row_end4, int sby) const;

  // dav1d load_tmvs (the C reference, tile_row_idx = 0). Bounds in 8x8 units; row_end8 - row_start8 <= 16.
  void load_tmvs(int col_start8, int col_end8, int row_start8, int row_end8);

  // dav1d_refmvs_save_tmvs.
  void save_tmvs(const RefmvsTile& rt, int col_start8, int col_end8, int row_start8, int row_end8);

  // dav1d splat_mv: stores `rmv` in the bw4 x bh4 cells at (by4, bx4).
  void splat_mv(const RefmvsTile& rt, const RefmvsBlock& rmv, int by4, int bx4, int bw4, int bh4) const;

  // dav1d_refmvs_find. mvstack must have 8 entries. top_has_right = the EDGE_I444_TOP_HAS_RIGHT bit of the block's edge flags.
  void find(const RefmvsTile& rt, ArrayView<RefmvsCandidate> mvstack, int& cnt, int& ctx, RefmvsRefPair ref, BlockSize bs,
            bool top_has_right, int by4, int bx4) const;

  // The cells of block-buffer slot `slot` (dav1d rt->r[slot], slot = (by & 31) + 5 + row offset), indexed by the 4x4 column.
  ArrayView<RefmvsBlock> row(const RefmvsTile& rt, int slot) const;

  // The projected temporal vectors of the current sb row (16 rows of 8x8 cells); for tests and debugging.
  ArrayView<const RefmvsTemporalBlock> projected() const { return rp_proj_; }

  bool use_ref_frame_mvs() const { return use_ref_frame_mvs_; }
  int iw4() const { return iw4_; }
  int ih4() const { return ih4_; }

 private:
  void release();

  allocators::IAllocator& allocator_;
  const FrameHeader* frm_hdr_ = nullptr;
  int iw4_ = 0, ih4_ = 0, iw8_ = 0, ih8_ = 0;
  int sbsz_ = 0;
  bool use_ref_frame_mvs_ = false;
  uint8_t sign_bias_[7] = {}, mfmv_sign_[7] = {};
  int8_t pocdiff_[7] = {};
  uint8_t mfmv_ref_[3] = {};
  int8_t mfmv_ref2cur_[3] = {};
  uint8_t mfmv_ref2ref_[3][7] = {};
  int n_mfmvs_ = 0;

  int n_blocks_ = 0;
  int64_t rp_stride_ = 0;
  MotionVectorStore* cur_ = nullptr;
  const MotionVectorStore* rp_ref_[7] = {};
  ArrayView<RefmvsTemporalBlock> rp_proj_;  // 16 rows of rp_stride_
  ArrayView<RefmvsBlock> r_;                // 35 rows of 2 * rp_stride_
};

}  // namespace av1
}  // namespace hardware
