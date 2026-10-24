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
#include "Hardware/VideoCodec/Software/Av1Cdf.hpp"
#include "Hardware/VideoCodec/Software/Av1Cdef.hpp"
#include "Hardware/VideoCodec/Software/Av1IntraEdge.hpp"
#include "Hardware/VideoCodec/Software/Av1Ipred.hpp"
#include "Hardware/VideoCodec/Software/Av1LoopFilter.hpp"
#include "Hardware/VideoCodec/Software/Av1LoopRestoration.hpp"
#include "Hardware/VideoCodec/Software/Av1Msac.hpp"
#include "Hardware/VideoCodec/Software/Av1Obu.hpp"
#include "Hardware/VideoCodec/Software/Av1Refmvs.hpp"
#include "Hardware/VideoCodec/Software/Av1Types.hpp"
#include "Hardware/VideoCodec/Software/Av1Wedge.hpp"
#include "Utility/AllocatedView.hpp"
#include "Utility/ArrayView.hpp"
#include "Utility/SharedPointer.hpp"
#include "Utility/UniquePointer.hpp"

// The AV1 decoder core: frame, tile and block state (dav1d's Dav1dContext / Dav1dFrameContext / Dav1dTileState / Dav1dTaskContext /
// BlockContext) and the pieces of dav1d's decode.c, lib.c, picture.c and ref.c that drive the module kernels (Av1Msac, Av1Cdf,
// Av1Refmvs, Av1Itx, Av1Ipred, Av1Mc, Av1LoopFilter, Av1Cdef, Av1SuperRes, Av1LoopRestoration, Av1FilmGrain). BSD-2-Clause, Copyright (c)
// VideoLAN and dav1d authors, for the parts that follow dav1d.
//
// Files:  Av1Decode.cpp  block syntax, partitions, tiles            (dav1d decode.c)
//         Av1Frame.cpp   frame set-up, references, post filters, output  (decode.c submit_frame, lib.c, picture.c, ref.c, lf_apply)
//         Av1Recon.cpp   coefficients and block reconstruction            (dav1d recon_tmpl.c; written against this header)
//         Av1.cpp        the Av1Decoder wrapper (OBU loop, 8-bit I420 conversion)
//
// Single threaded and single pass: every block is parsed and reconstructed in decode_b (dav1d with n_fc == 1 and n_tc == 1, pass 0).
// A tile is decoded completely before the next one; the whole frame is deblocked / CDEF-filtered / upscaled / restored afterwards.
//
// ---- Conventions for the reconstruction code (Av1Recon.cpp) ---------------------------------------------------------------------
//  * Samples are 16-bit (Pixel); `bitdepth_max` is f.bitdepth_max. Strides are in samples. Pictures are allocated with the luma plane
//    rounded up to a multiple of 128 in both directions (the chroma planes accordingly), so a block that starts inside the frame can
//    be written completely, as dav1d's padded pictures allow. `Picture::plane(p)` is a PixelView with origin 0.
//  * Pixel positions of the tiles are in 4x4 units (t.bx, t.by are frame absolute, f.bw / f.bh the frame size in 4x4 units rounded up
//    to even). The above contexts are indexed with `bx & 31`, the left contexts with `by & 31`, as in dav1d.
//  * Returned `bool`s are "ok": false means the stream is corrupt, the frame is abandoned (dav1d returns a negative value).
//  * Scratch buffers (TaskContext::scratch) are all separate arrays (dav1d overlays several of them in a union): lap / compinter /
//    seg_mask / emu_edge for inter prediction, levels / ac / txtp_map / pal_idx_y / pal_idx_uv / interintra / edge / pal for intra and
//    coefficients, pal_tmp (the pre-pack palette index buffer: dav1d reuses pal_idx_uv for it; here it is its own array). `cf` holds
//    the coefficients (int32_t, the 16bpc `coef` type) and must be all zero between transform blocks (inv_txfm_add clears what it used).
//  * dav1d's `t->l.tx_lpf_*` and `a->tx_lpf_*` are not here: LoopFilterMasks keeps its own frame wide contexts.
//  * The tile's bitstream position is ts.msac, its CDFs ts.cdf (a private copy that adapts), quantizer tables ts.dq.
namespace hardware {
namespace av1 {

// ---- helpers --------------------------------------------------------------------------------------------------------------------

// An owned, zero initialised array from an allocator (plain data only). reset() replaces the contents.
template <class T>
class OwnedArray {
 public:
  explicit OwnedArray(allocators::IAllocator& allocator) : allocator_(allocator) {}
  ~OwnedArray() { release_view(allocator_, view_); }
  OwnedArray(const OwnedArray&) = delete;
  OwnedArray& operator=(const OwnedArray&) = delete;
  // `count` elements, all T{}. False (and the array is empty) if the memory cannot be had.
  bool reset(uint64_t count, uint64_t alignment = alignof(T)) {
    if (view_.size() != count) {
      release_view(allocator_, view_);
      if (count == 0) return true;
      view_ = allocate_view<T>(allocator_, count, alignment);
      if (view_.empty()) return false;
    }
    view_.fill(T{});
    return true;
  }
  void release() { release_view(allocator_, view_); }
  ArrayView<T> view() const { return view_; }
  T& operator[](uint64_t index) const { return view_[index]; }
  uint64_t size() const { return view_.size(); }
  bool empty() const { return view_.empty(); }

 private:
  allocators::IAllocator& allocator_;
  ArrayView<T> view_;
};

// dav1d's TxfmInfo / dav1d_txfm_dimensions: sizes in 4-sample units (w, h), their log2 (lw, lh), min / max of the log2 sizes, the next
// smaller transform (sub) and the context for the transform size CDFs.
struct TxfmInfo {
  uint8_t w, h, lw, lh, min, max, sub, ctx;
};
inline constexpr TxfmInfo k_txfm_dimensions[N_RECT_TX_SIZES] = {
    /* TX_4X4   */ {1, 1, 0, 0, 0, 0, 0, 0},
    /* TX_8X8   */ {2, 2, 1, 1, 1, 1, TX_4X4, 1},
    /* TX_16X16 */ {4, 4, 2, 2, 2, 2, TX_8X8, 2},
    /* TX_32X32 */ {8, 8, 3, 3, 3, 3, TX_16X16, 3},
    /* TX_64X64 */ {16, 16, 4, 4, 4, 4, TX_32X32, 4},
    /* RTX_4X8  */ {1, 2, 0, 1, 0, 1, TX_4X4, 1},
    /* RTX_8X4  */ {2, 1, 1, 0, 0, 1, TX_4X4, 1},
    /* RTX_8X16 */ {2, 4, 1, 2, 1, 2, TX_8X8, 2},
    /* RTX_16X8 */ {4, 2, 2, 1, 1, 2, TX_8X8, 2},
    /* RTX_16X32*/ {4, 8, 2, 3, 2, 3, TX_16X16, 3},
    /* RTX_32X16*/ {8, 4, 3, 2, 2, 3, TX_16X16, 3},
    /* RTX_32X64*/ {8, 16, 3, 4, 3, 4, TX_32X32, 4},
    /* RTX_64X32*/ {16, 8, 4, 3, 3, 4, TX_32X32, 4},
    /* RTX_4X16 */ {1, 4, 0, 2, 0, 2, RTX_4X8, 1},
    /* RTX_16X4 */ {4, 1, 2, 0, 0, 2, RTX_8X4, 1},
    /* RTX_8X32 */ {2, 8, 1, 3, 1, 3, RTX_8X16, 2},
    /* RTX_32X8 */ {8, 2, 3, 1, 1, 3, RTX_16X8, 2},
    /* RTX_16X64*/ {4, 16, 2, 4, 2, 4, RTX_16X32, 3},
    /* RTX_64X16*/ {16, 4, 4, 2, 2, 4, RTX_32X16, 3},
};

// Bit masks over BlockSize (dav1d: cfl_allowed_mask, wedge_allowed_mask, interintra_allowed_mask in levels.h / recon.h).
inline constexpr unsigned cfl_allowed_mask = (1u << BS_32x32) | (1u << BS_32x16) | (1u << BS_32x8) | (1u << BS_16x32) | (1u << BS_16x16) |
                                             (1u << BS_16x8) | (1u << BS_16x4) | (1u << BS_8x32) | (1u << BS_8x16) | (1u << BS_8x8) |
                                             (1u << BS_8x4) | (1u << BS_4x16) | (1u << BS_4x8) | (1u << BS_4x4);
inline constexpr unsigned wedge_allowed_mask = (1u << BS_32x32) | (1u << BS_32x16) | (1u << BS_32x8) | (1u << BS_16x32) | (1u << BS_16x16) |
                                               (1u << BS_16x8) | (1u << BS_8x32) | (1u << BS_8x16) | (1u << BS_8x8);
inline constexpr unsigned interintra_allowed_mask = (1u << BS_32x32) | (1u << BS_32x16) | (1u << BS_16x32) | (1u << BS_16x16) |
                                                    (1u << BS_16x8) | (1u << BS_8x16) | (1u << BS_8x8);

// ---- pictures and per-reference state -------------------------------------------------------------------------------------------

// A decoded picture (dav1d: Dav1dPicture): up to three planes of 16-bit samples plus the header of the frame it came from.
class Picture {
 public:
  explicit Picture(allocators::IAllocator& allocator) : allocator_(allocator) {}
  ~Picture() { release_view(allocator_, memory_); }
  Picture(const Picture&) = delete;
  Picture& operator=(const Picture&) = delete;

  // Allocates (zero filled) a picture of the visible size width x height. Luma is stored with both dimensions rounded up to a multiple
  // of 128. False if the size is unusable or memory ran out.
  bool allocate(PixelLayout layout, int bpc, int width, int height);
  bool valid() const { return !memory_.empty(); }
  // The whole plane `p` (0 = Y, 1 = U, 2 = V); an empty-plane view for p > 0 of a monochrome picture.
  PixelView plane(int p) const { return PixelView{planes_[p], 0, stride_[p ? 1 : 0]}; }
  int stride(int p) const { return static_cast<int>(stride_[p ? 1 : 0]); }
  // The visible size of plane p in samples.
  int plane_width(int p) const { return p ? (width_ + ss_hor()) >> ss_hor() : width_; }
  int plane_height(int p) const { return p ? (height_ + ss_ver()) >> ss_ver() : height_; }
  int ss_hor() const { return layout_ != PIXEL_LAYOUT_I444; }
  int ss_ver() const { return layout_ == PIXEL_LAYOUT_I420; }
  int width() const { return width_; }
  int height() const { return height_; }
  int bpc() const { return bpc_; }
  PixelLayout layout() const { return layout_; }
  // Takes over the sample storage of `other` (same geometry required, else false); `other` gets this picture's storage.
  bool swap_storage(Picture& other);

  FrameHeader hdr{};            // the header of the frame (film grain parameters, frame_offset, sizes)
  bool matrix_identity = false;  // seq_hdr.mtrx == MC_IDENTITY (film grain needs it)

 private:
  allocators::IAllocator& allocator_;
  ArrayView<Pixel> memory_;
  ArrayView<Pixel> planes_[3];
  int64_t stride_[2] = {0, 0};
  int width_ = 0, height_ = 0, bpc_ = 8;
  PixelLayout layout_ = PIXEL_LAYOUT_I420;
};

// The segmentation map of a frame: one byte per 4x4 block, f.b4_stride bytes per row.
struct SegmentMap {
  explicit SegmentMap(allocators::IAllocator& allocator) : data(allocator) {}
  OwnedArray<uint8_t> data;
};

// What dav1d keeps in Dav1dContext::refs[] and ::cdf[] for one of the eight reference slots.
struct RefSlot {
  SharedPointer<Picture> pic;
  SharedPointer<SegmentMap> segmap;
  SharedPointer<MotionVectorStore> mvs;
  SharedPointer<CdfContext> cdf;
};

// ---- block, tile and frame state ------------------------------------------------------------------------------------------------

// dav1d's BlockContext: the above context of one 128 wide superblock column (a) or the left context of the current superblock (l).
// Entries are per 4x4 unit; ref -1 means intra, filter 3 means unset.
struct BlockContext {
  uint8_t mode[32];
  uint8_t lcoef[32];
  uint8_t ccoef[2][32];
  uint8_t seg_pred[32];
  uint8_t skip[32];
  uint8_t skip_mode[32];
  uint8_t intra[32];
  uint8_t comp_type[32];
  int8_t ref[2][32];
  uint8_t filter[2][32];
  int8_t tx_intra[32];
  int8_t tx[32];
  uint8_t partition[16];
  uint8_t uvmode[32];
  uint8_t pal_sz[32];
};

// dav1d: uint16_t dq[MAX_SEGMENTS][3 /* plane */][2 /* dc, ac */].
struct DequantTables {
  uint16_t q[MAX_SEGMENTS][3][2];
};

struct ScalableMotionParams {
  int scale;  // 0 if the reference has the size of the frame
  int step;
};

// Per tile (dav1d Dav1dTileState).
struct TileState {
  CdfContext cdf;
  Msac msac;
  struct {
    int col_start, col_end, row_start, row_end;  // in 4x4 units
    int col, row;                                // in tile units
  } tiling{};
  DequantTables dqmem{};
  const DequantTables* dq = nullptr;  // f.dq or dqmem: the quantizers of the current superblock
  int last_qidx = 0;
  int8_t last_delta_lf[4] = {};
  RestorationUnit lr_ref[3];  // the reference values of the loop restoration coefficient coding
};

struct FrameContext;

// Per block-decoding "thread" (dav1d Dav1dTaskContext): position, contexts, scratch memory.
struct TaskContext {
  FrameContext* f = nullptr;
  TileState* ts = nullptr;
  int bx = 0, by = 0;  // position of the block in 4x4 units, frame absolute
  BlockContext l{};    // left context
  BlockContext* a = nullptr;  // above context of the superblock column (f.a entry)
  RefmvsTile rt;
  alignas(64) int32_t cf[32 * 32];
  Pixel al_pal[2 /* a, l */][32 /* bx4 / by4 */][3 /* plane */][8 /* palette index */];
  uint8_t pal_sz_uv[2 /* a, l */][32 /* bx4 / by4 */];
  struct {
    Pixel lap[128 * 32];
    int16_t compinter[2][128 * 128];
    uint8_t seg_mask[128 * 128];
    Pixel emu_edge[320 * (256 + 7)];  // stride 192 for ordinary references, 320 for scaled ones
    uint8_t levels[32 * 34];
    int16_t ac[32 * 32];          // intra only (CfL)
    uint8_t txtp_map[32 * 32];    // inter only
    uint8_t pal_idx_y[32 * 64];
    uint8_t pal_idx_uv[64 * 64];
    uint8_t pal_tmp[64 * 64];     // wave-front palette indices before packing (stride bw4 * 4)
    Pixel interintra[64 * 64];
    IntraEdge edge;
    Pixel pal[3 /* plane */][8 /* palette index */];
  } scratch;
  WarpedMotionParams warpmv{};
  int tl_4x4_filter = 0;  // Filter2d of the top left 4x4 block (chroma of sub8x8 inter blocks)
};

// Per frame (dav1d Dav1dFrameContext).
struct FrameContext {
  explicit FrameContext(allocators::IAllocator& allocator);

  SequenceHeader seq_hdr_store{};
  FrameHeader frame_hdr_store{};  // the frame header, completed by the decoder (shear parameters of gmv[])
  const SequenceHeader* seq_hdr = &seq_hdr_store;
  const FrameHeader* frame_hdr = &frame_hdr_store;

  SharedPointer<Picture> cur;     // the picture being decoded (coded width)
  SharedPointer<Picture> sr_cur;  // the final picture (after super-resolution); the same object as cur without super-resolution
  SharedPointer<Picture> refp[7];  // the references (frame_hdr.refidx[i]), inter frames only
  SharedPointer<MotionVectorStore> mvs;       // this frame's saved motion vectors (inter frames and intrabc)
  SharedPointer<SegmentMap> cur_segmap, prev_segmap;
  uint8_t refpoc[7] = {};
  uint8_t gmv_warp_allowed[7] = {};
  ScalableMotionParams svc[7][2 /* x, y */] = {};
  int resize_step[2 /* y, uv */] = {}, resize_start[2] = {};
  SharedPointer<CdfContext> in_cdf, out_cdf;

  int w4 = 0, h4 = 0, bw = 0, bh = 0, sb128w = 0, sb128h = 0, sbh = 0, sb_shift = 0, sb_step = 0, sr_sb128w = 0;
  int b4_stride = 0;  // bytes per row of the segmentation map: (bw + 31) & ~31
  int bitdepth_max = 255;
  PixelLayout layout = PIXEL_LAYOUT_I420;
  int ss_hor = 1, ss_ver = 1;

  DequantTables dq{};
  ArrayView<const uint8_t> qm[N_RECT_TX_SIZES][3 /* plane */];  // empty when the frame has no quantizer matrices
  uint8_t jnt_weights[7][7] = {};
  OwnedArray<BlockContext> a;  // above contexts: sb128w per tile row
  // Pre-loopfilter samples of the bottom row of every superblock row, for the intra edge of the next one: dav1d f->ipred_edge[plane],
  // sbh * sb128w * 128 samples per plane (see Core::backup_ipred_edge).
  OwnedArray<Pixel> ipred_edge[3];

  RefmvsFrame rf;
  LoopFilterMasks lfm;
  int restore_planes = 0;  // bit p: plane p is restored
  // Restoration units per plane (Av1LoopRestoration.hpp: row-major, lr_rows * lr_cols).
  OwnedArray<RestorationUnit> lr_units[3];
  int lr_rows[3] = {}, lr_cols[3] = {};
};

// ---- the quantizer matrices (implemented in Av1Recon.cpp, which includes Av1Qm.inc) -----------------------------------------------
// dav1d_qm_tbl[level][is_uv][tx] for level 0..14 and tx a TxfmSize / RectTxfmSize; empty for level 15 (no matrix).
ArrayView<const uint8_t> qm_table(int level, int is_uv, int tx);

// dav1d's reset_context (Av1Decode.cpp): the initial value of an above or left context at the start of a tile (or superblock row).
void reset_block_context(BlockContext& ctx, bool keyframe);

// dav1d's init_quant_tables: the dequantizers of every segment for the base quantizer index `qidx` (Av1Decode.cpp).
void init_quant_tables(const SequenceHeader& seq_hdr, const FrameHeader& frame_hdr, int qidx, DequantTables& dq);

// ---- the core ---------------------------------------------------------------------------------------------------------------------

// A frame handed to another decoder (a hardware engine) instead of being decoded here: the headers, the tile data and the picture to fill. The Core still parses
// every OBU, validates the headers and keeps the reference slots and the output; what the offload keeps is what the engine needs per slot.
struct OffloadFrame {
  const SequenceHeader* seq = nullptr;
  const FrameHeader* hdr = nullptr;
  ArrayView<const TileData> tiles;  // by tile number (row * cols + col)
  PixelView plane[3];               // the picture to fill: hdr->width[1] x hdr->height luma samples (chroma as the layout says)
  int width = 0, height = 0;
};
class IOffload {
 public:
  // Whether this frame (and so, in practice, its sequence) is one the offload can decode. A frame it refuses is decoded here.
  virtual bool accepts(const SequenceHeader& seq, const FrameHeader& hdr) = 0;
  // Decodes the frame into the planes and updates the offload's own reference slots as hdr->refresh_frame_flags says. False: the planes are left grey.
  virtual bool decode_frame(const OffloadFrame& frame) = 0;
  // A show_existing_frame of slot `idx`; with `refresh_all` (a key frame) every slot now holds that frame.
  virtual void show_existing(unsigned idx, bool refresh_all) = 0;
  // A new sequence began: every slot is empty.
  virtual void new_sequence() = 0;

 protected:
  constexpr IOffload() = default;
  ~IOffload() = default;
};

// For diagnostics and tests: told of every frame the Core decodes itself (hidden frames included), with its headers, its tiles and the picture as it is held.
class IFrameObserver {
 public:
  virtual void frame_decoded(const SequenceHeader& seq, const FrameHeader& hdr, ArrayView<const TileData> tiles, const Picture& picture) = 0;

 protected:
  constexpr IFrameObserver() = default;
  ~IFrameObserver() = default;
};

class Core {
 public:
  explicit Core(allocators::IAllocator& allocator);
  ~Core();
  Core(const Core&) = delete;
  Core& operator=(const Core&) = delete;

  // ---- used by Av1Decoder (Av1.cpp)
  ObuParser& parser() { return parser_; }
  // Forget everything: references, pictures, parser state (after a seek or an error).
  void reset();
  // A sequence header that starts a new sequence arrived: all references are invalid.
  void new_sequence();
  // The frame the parser just completed (ObuEventKind::FrameReady): decodes it, updates the references (and the parser's) and makes it the
  // output if it is shown. False if the data is corrupt (the references the frame would have refreshed are dropped).
  bool decode_frame();
  // ObuEventKind::ShowExistingFrame: outputs the picture of slot `idx`; with `refresh_all` (a key frame) copies the slot to all slots.
  bool show_existing_frame(unsigned idx, bool refresh_all);
  // The last frame that was output since take_output() (null if none). With film grain the grain is applied into a separate picture.
  // Valid until the next call of decode_frame / show_existing_frame / reset.
  const Picture* output();
  void clear_output() { out_.reset(); }
  // Frames are decoded by `offload` from then on, where it accepts them (nullptr: here). Set before the first frame.
  void set_offload(IOffload* offload) { offload_ = offload; }
  void set_frame_observer(IFrameObserver* observer) { observer_ = observer; }

  // ---- reconstruction (Av1Recon.cpp). dav1d's bd_fn.
  // Intra block (dav1d_recon_b_intra): prediction and residual of luma and chroma, reads the coefficients.
  bool recon_b_intra(TaskContext& t, BlockSize bs, int intra_edge_flags, const Av1Block& b);
  // Inter block and intrabc block (dav1d_recon_b_inter). t.warpmv holds the local warp parameters for MM_WARP blocks.
  bool recon_b_inter(TaskContext& t, BlockSize bs, const Av1Block& b);
  // Frame threading pass 1 only; unused here (declared for completeness, no definition is needed).
  void read_coef_blocks(TaskContext& t, BlockSize bs, const Av1Block& b);
  // Palette colours of luma (pl = 0) or of both chroma planes: reads them from t.ts->msac into the block, using the palette cache of the
  // neighbours (t.al_pal, t.a->pal_sz / t.l.pal_sz) and writes t.scratch.pal. dav1d_read_pal_plane / _uv. They set b.pal_sz[].
  void read_pal_plane(TaskContext& t, Av1Block& b, bool pl, int sz_ctx, int bx4, int by4);
  void read_pal_uv(TaskContext& t, Av1Block& b, int sz_ctx, int bx4, int by4);
  // Store the palette of the block in the above / left palette caches (t.al_pal).
  void copy_pal_block_y(TaskContext& t, int bx4, int by4, int bw4, int bh4);
  void copy_pal_block_uv(TaskContext& t, int bx4, int by4, int bw4, int bh4);
  // Saves the bottom row of the superblock row just decoded (before the loop filters) into f.ipred_edge (dav1d_backup_ipred_edge).
  void backup_ipred_edge(TaskContext& t);

  const WedgeMasks& wedge_masks() const { return wedges_; }
  allocators::IAllocator& allocator() const { return allocator_; }

 private:
  // ---- Av1Decode.cpp
  bool setup_tile(TileState& ts, int tile_row, int tile_col, ConstByteView data);
  bool decode_tile(TaskContext& t, int tile_row, int tile_col);
  bool decode_tile_sbrow(TaskContext& t);
  bool decode_sb(TaskContext& t, BlockLevel bl, const EdgeNode& node);
  bool decode_b(TaskContext& t, BlockLevel bl, BlockSize bs, BlockPartition bp, int intra_edge_flags);
  void read_restoration_info(TaskContext& t, RestorationUnit& lr, int p, RestorationType frame_type);

  // ---- Av1Frame.cpp
  bool init_frame(const ObuParser& parser);
  bool decode_tiles(const ObuParser& parser);
  bool post_filter();
  bool apply_film_grain_to_output();
  bool decode_frame_offloaded();
  void finish_frame(bool ok);
  void drop_slot(unsigned i);

  allocators::IAllocator& allocator_;
  ObuParser parser_;
  FrameContext f_;
  UniquePointer<TaskContext> t_;
  UniquePointer<TileState> ts_;
  RefSlot refs_[NUM_REF_FRAMES];
  WedgeMasks wedges_;
  IOffload* offload_ = nullptr;
  IFrameObserver* observer_ = nullptr;
  SharedPointer<Picture> out_;
  SharedPointer<Picture> grain_;  // the output with film grain applied
  bool grain_valid_ = false;
};

}  // namespace av1
}  // namespace hardware
