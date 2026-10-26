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

#include "Hardware/GraphicsProcessingUnit/Intel/IntelRenderBatch.hpp"

namespace hardware {
namespace intel_gt {
namespace gen9_3d {

namespace {
constexpr uint32_t kVertexStride = sizeof(Vertex);
static_assert(sizeof(Vertex) == 32, "the vertex elements describe 32 bytes per vertex");
constexpr uint32_t kMaxSurfaceSize = 16384;
constexpr uint32_t kMaxPitch = 256 * 1024;
constexpr float kMaxCoordinate = 16384.0f;
constexpr float kMaxInverseW = 1.0e30f;

// The copy kernel of intel-gpu-tools (lib/i915/shaders/ps/blit.g7a as used by rendercopy_gen9.c). Two plane-equation instructions turn the barycentrics in g2 and the
// coefficients in g6 into the texture coordinates u and v (g122, g124); the sampler message reads the texel (g113..g120); the render target write sends it and ends the
// thread.
constexpr uint32_t kCopyKernel[] = {
    0x0080005a, 0x2f403ae8, 0x3a0000c0, 0x008d0040,
    0x0080005a, 0x2f803ae8, 0x3a0000d0, 0x008d0040,
    0x02800031, 0x2e203a48, 0x0e8d0f40, 0x08840001,
    0x05800031, 0x20003a40, 0x0e8d0e20, 0x90031000,
};

// Instruction fields of the Gen8+ encoding this file edits: the destination register number (dword 1, bits 28:21) of the plane-equation instruction, and the register and
// sub-register of its first source (dword 2, bits 12:0: register * 32 + byte offset).
constexpr uint32_t kPlnDstTemplate = 0x20003ae8;
constexpr uint32_t kPlnDstShift = 21;
constexpr uint32_t kPlnSrc0Template = 0x3a000000;

// Interpolated colour: attribute 0 sits in the setup payload from g6 (x in g6.0, y in g6.4, z in g7.0, w in g7.4, each as the three plane coefficients), the barycentrics
// in g2. Four plane equations (SIMD16: two registers each) fill g112..g119, and the render target write sends them (no header, eight registers).
constexpr uint32_t kGouraudKernel[] = {
    0x0080005a, kPlnDstTemplate | (112u << kPlnDstShift), kPlnSrc0Template | 0xc0, 0x008d0040,
    0x0080005a, kPlnDstTemplate | (114u << kPlnDstShift), kPlnSrc0Template | 0xd0, 0x008d0040,
    0x0080005a, kPlnDstTemplate | (116u << kPlnDstShift), kPlnSrc0Template | 0xe0, 0x008d0040,
    0x0080005a, kPlnDstTemplate | (118u << kPlnDstShift), kPlnSrc0Template | 0xf0, 0x008d0040,
    0x05800031, 0x20003a40, 0x0e8d0e00, 0x90031000,
};

constexpr uint32_t kSurfaceState2d = 1;
constexpr uint32_t kMocsPageTable = 1;   // gen9::kMocsPageTable (the tables are programmed by IntelGen9Setup)
constexpr uint32_t kBlendFactorOne = 0x01, kBlendFactorSrcAlpha = 0x03, kBlendFactorZero = 0x11, kBlendFactorInvSrcAlpha = 0x13;
constexpr uint32_t kCompareLessEqual = 3, kCompareAlways = 7, kDepthFormatD32Float = 1;
constexpr uint32_t kCullNone = 1u << 16, kFrontWindingCcw = 1u << 21;
constexpr uint32_t kVeValid = 1u << 25;
constexpr uint32_t kVfCompStoreSrc = 1, kVfCompStore0 = 2, kVfCompStore1Float = 3;
constexpr uint32_t kFormatR32G32B32A32Float = 0x000, kFormatR32G32Float = 0x085, kFormatR16G16Sscaled = 0x0F6;

bool finite_and_bounded(float v) { return v == v && v > -kMaxCoordinate && v < kMaxCoordinate; }

// Writes the commands (up to kStateOffset) and the state behind them.
class Image {
 public:
  Image(ArrayView<uint32_t> view, uint32_t base) : view_(view), base_(base) {}

  bool valid_buffer() const {
    return view_.size() * 4 >= uint64_t{kStateOffset} * 2 && view_.size() * 4 <= kMaxImageBytes && (view_.size() * 4) % 4096 == 0 && (base_ & 4095) == 0;
  }
  void reset() {
    for (uint32_t& d : view_) d = 0;
    at_ = 0;
    state_at_ = kStateOffset;
    overflow_ = false;
  }
  bool ok() const { return !overflow_; }
  uint32_t bytes() const { return static_cast<uint32_t>(view_.size() * 4); }
  uint32_t base() const { return base_; }
  uint32_t dwords_used() const { return state_at_ / 4; }

  void put(uint32_t value) {
    if (at_ >= kStateOffset / 4) { overflow_ = true; return; }
    view_[at_++] = value;
  }
  void put_zeros(uint32_t count) { for (uint32_t i = 0; i < count; ++i) put(0); }
  // A command whose dword count is `total` (header has the length less two): header, then the caller writes total - 1 more.
  void header(uint32_t opcode, uint32_t total) { put(opcode | (total - 2)); }

  // Reserves state of `bytes` aligned to `align`; the offset from the image start, or 0 when full.
  uint32_t allocate(uint32_t bytes, uint32_t align) {
    const uint64_t start = (uint64_t{state_at_} + align - 1) / align * align;
    if (start + bytes > view_.size() * 4) { overflow_ = true; return 0; }
    state_at_ = static_cast<uint32_t>(start + bytes);
    return static_cast<uint32_t>(start);
  }
  // A dword of state by byte offset.
  void state(uint32_t offset, uint32_t dword_index, uint32_t value) {
    const uint64_t index = uint64_t{offset} / 4 + dword_index;
    if (offset == 0 || index >= view_.size()) { overflow_ = true; return; }
    view_[index] = value;
  }
  void copy_state(uint32_t offset, ArrayView<const uint32_t> data) {
    for (uint64_t i = 0; i < data.size(); ++i) state(offset, static_cast<uint32_t>(i), data[i]);
  }

 private:
  ArrayView<uint32_t> view_;
  uint32_t base_;
  uint32_t at_ = 0;
  uint32_t state_at_ = kStateOffset;
  bool overflow_ = false;
};

uint32_t align_to(uint32_t v, uint32_t a) { return (v + a - 1) / a * a; }
uint32_t minify(uint32_t v, uint32_t level) { return level >= 31 ? 1 : (v >> level ? v >> level : 1); }

uint32_t surface_state(Image& img, const Surface& s) {
  const uint32_t at = img.allocate(64, 64);
  if (!at) return 0;
  // RENDER_SURFACE_STATE of Gen9 (genxml): type, format, 4x4 alignment, render cache read/write; sizes less one; the mip tail start LOD the hardware wants set; the
  // channels selected as they are; the base address.
  // Tile mode (bits 13:12): 0 linear, 3 Y major.
  img.state(at, 0, (kSurfaceState2d << 29) | (static_cast<uint32_t>(s.format) << 18) | (1u << 16) | (1u << 14) | (1u << 8) | (s.tiled_y ? 3u << 12 : 0u));
  // The QPitch (bits 14:0, in units of 4 rows: the rows the levels of the surface take) and the memory object control: the table entry that follows the page table entry (the
  // field is the index shifted left by one).
  const uint32_t qpitch_rows = s.slice_rows ? s.slice_rows : align_to(s.height, 4);
  img.state(at, 1, (kMocsPageTable << 25) | ((qpitch_rows / 4) & 0x7FFF));
  img.state(at, 2, ((s.height - 1) << 16) | (s.width - 1));
  img.state(at, 3, s.pitch - 1);
  img.state(at, 5, (1u << 8) | ((s.levels - 1) & 0xF));   // the mip tail start LOD the hardware wants set; MIP Count / LOD (bits 3:0): the levels less one, for a surface that is sampled
  img.state(at, 7, (7u << 16) | (6u << 19) | (5u << 22) | (4u << 25));
  img.state(at, 8, static_cast<uint32_t>(s.gpu_address));
  img.state(at, 9, static_cast<uint32_t>(s.gpu_address >> 32));
  return at;
}

uint32_t sampler_state(Image& img, const Sampler& sampler) {
  const uint32_t at = img.allocate(16, 64);
  if (!at) return 0;
  // SAMPLER_STATE: the minification filter (bits 16:14) and the magnification one (19:17), 0 nearest and 1 linear; the mip mode filter (21:20: 0 none, 1 nearest, 3 linear) and
  // the LOD bias (13:1, signed 4.8). The second dword holds the maximum (19:8) and minimum (31:20) LOD, unsigned 4.8: both zero keeps a surface without mipmaps at level 0.
  auto fixed = [](float v, float lo, float hi) {
    if (!(v > lo)) v = lo;
    if (v > hi) v = hi;
    return static_cast<int32_t>(v * 256.0f + (v < 0 ? -0.5f : 0.5f));
  };
  const uint32_t mip_mode = sampler.mip == MipMode::Nearest ? 1u : sampler.mip == MipMode::Linear ? 3u : 0u;
  img.state(at, 0, (sampler.linear ? (1u << 14) | (1u << 17) : 0u) | (mip_mode << 20) | ((static_cast<uint32_t>(fixed(sampler.lod_bias, -16.0f, 15.99f)) & 0x1FFF) << 1));
  img.state(at, 1, (sampler.mip == MipMode::None ? 0u : ((static_cast<uint32_t>(fixed(sampler.max_lod, 0.0f, 14.0f)) & 0xFFF) << 8) | ((static_cast<uint32_t>(fixed(sampler.min_lod, 0.0f, 14.0f)) & 0xFFF) << 20)));
  // The address control modes of r, t and s (bits 2:0, 5:3, 8:6): 0 wrap, 2 clamp. (There is no third dimension: r is clamped.)
  img.state(at, 3, (2u << 0) | ((sampler.repeat_v ? 0u : 2u) << 3) | ((sampler.repeat_u ? 0u : 2u) << 6));
  return at;
}

struct BlendFactors {
  uint32_t src, dst, src_alpha, dst_alpha;
  bool enable;
};
BlendFactors blend_factors(Blend blend) {
  if (blend == Blend::SourceOver) return {kBlendFactorSrcAlpha, kBlendFactorInvSrcAlpha, kBlendFactorOne, kBlendFactorInvSrcAlpha, true};
  return {kBlendFactorOne, kBlendFactorZero, kBlendFactorOne, kBlendFactorZero, false};
}

uint32_t blend_state(Image& img, const BlendFactors& f, bool write_color) {
  const uint32_t at = img.allocate(4 + 16 * 8, 64);
  if (!at) return 0;
  if (f.enable) img.state(at, 0, 1u << 30);   // independent alpha blend: the alpha factors below
  for (uint32_t i = 0; i < 16; ++i) {
    // BLEND_STATE_ENTRY: factors and functions in the low dword, the clamps in the high one (pre-blend colour clamp on).
    // (the low four bits disable the writes of blue, green, red and alpha)
    img.state(at, 1 + i * 2, (f.src_alpha << 13) | (f.dst_alpha << 8) | (f.dst << 21) | (f.src << 26) | (f.enable ? 1u << 31 : 0u) | (write_color ? 0u : 0xFu));
    img.state(at, 2 + i * 2, 1u << 1);
  }
  return at;
}

uint32_t cc_state(Image& img) { return img.allocate(6 * 4, 64); }

uint32_t cc_viewport(Image& img) {
  const uint32_t at = img.allocate(8, 32);
  if (!at) return 0;
  img.state(at, 0, float_bits(-1.0e35f));   // minimum and maximum depth wide open
  img.state(at, 1, float_bits(1.0e35f));
  return at;
}

uint32_t sf_clip_viewport(Image& img) {
  const uint32_t at = img.allocate(16 * 4, 64);
  if (!at) return 0;
  img.state(at, 9, float_bits(1.0f));    // the guard band as the render copy test sets it (the clipper is off)
  img.state(at, 11, float_bits(1.0f));
  return at;
}

struct Job {
  uint32_t binding_table = 0, binding_entries = 0;
  uint32_t sampler = 0, sampler_count = 0;
  uint32_t kernel = 0;
  BlendFactors blend;
  Rect drawing;                   // the drawing rectangle, inclusive of x1 - 1
  uint32_t vertex_offset = 0, vertex_bytes = 0;
  uint32_t vertex_count = 0;
  uint32_t topology = 0;
  bool copy_vertices = false;     // 16-bit window position + float texture coordinates, 12 bytes; else a Vertex
  DepthBuffer depth;
  bool depth_test = false, depth_write = false;
  bool depth_always = false;      // a depth clear: every pixel passes
  StencilBuffer stencil;
  StencilTest stencil_test;
};

void emit_null_stage(Image& img, uint32_t constant, uint32_t stage, uint32_t stage_total, uint32_t binding, uint32_t sampler) {
  img.header(constant, 11);
  img.put_zeros(10);
  img.header(stage, stage_total);
  img.put_zeros(stage_total - 1);
  img.header(binding, 2);
  img.put(0);
  if (sampler) {
    img.header(sampler, 2);
    img.put(0);
  }
}

void emit_job(Image& img, const Job& job, uint32_t blend_ptr, uint32_t cc_ptr, uint32_t cc_vp, uint32_t sf_vp, uint32_t scissor) {
  const uint32_t base = img.base();
  const uint32_t pages = img.bytes() / 4096;

  img.put(kPipelineSelect3d);
  img.header(kStateSip, 3);
  img.put_zeros(2);
  const uint32_t push_constant_allocs[] = {kPushConstantAllocVs, kPushConstantAllocHs, kPushConstantAllocDs, kPushConstantAllocGs, kPushConstantAllocPs};
  for (const uint32_t alloc : push_constant_allocs) {
    img.header(alloc, 2);
    img.put(0);
  }

  // STATE_BASE_ADDRESS, 18 dwords as the render copy test has it (the bindless surface state base is not modified) and a no-op in place of the 19th.
  img.header(kStateBaseAddress, 18);
  img.put(1);                      // general state base 0, modify
  img.put(0);
  img.put(1);                      // stateless data port access
  img.put(base | 1);               // surface state base: the image
  img.put(0);
  img.put(base | 1);               // dynamic state base: the image
  img.put(0);
  img.put(0);                      // indirect object base: unchanged
  img.put(0);
  img.put(base | 1);               // instruction base: the image
  img.put(0);
  img.put(0xFFFFF000u | 1);        // general state size: all
  img.put((pages << 12) | 1);      // dynamic state size: the image
  img.put(0xFFFFF000u | 1);        // indirect object size: all
  img.put((pages << 12) | 1);      // instruction size: the image
  img.put(0);
  img.put(0);
  img.put(kMiNoop);

  img.header(kViewportStatePointersCc, 2);
  img.put(cc_vp);
  img.header(kViewportStatePointersSfClip, 2);
  img.put(sf_vp);

  // URB: 64 entries of two 512-bit rows for the vertex shader stage (the minimum the vertex fetch needs), starting at 4; the other stages get none.
  img.header(kUrbVs, 2);
  img.put(64u | (1u << 16) | (4u << 25));
  const uint32_t other_urbs[] = {kUrbGs, kUrbHs, kUrbDs};
  for (const uint32_t urb : other_urbs) {
    img.header(urb, 2);
    img.put(4u << 25);
  }

  img.header(kBlendStatePointers, 2);
  img.put(blend_ptr | 1);
  img.header(kCcStatePointers, 2);
  img.put(cc_ptr | 1);
  img.header(kMultisample, 2);
  img.put(0);
  img.header(kSampleMask, 2);
  img.put(1);

  // Every stage the job does not use, off.
  img.header(kWmHzOp, 5);
  img.put_zeros(4);
  emit_null_stage(img, kConstantHs, kHs, 9, kBindingTablePointersHs, kSamplerStatePointersHs);
  img.header(kTe, 4);
  img.put_zeros(3);
  emit_null_stage(img, kConstantGs, kGs, 10, kBindingTablePointersGs, kSamplerStatePointersGs);
  emit_null_stage(img, kConstantDs, kDs, 11, kBindingTablePointersDs, kSamplerStatePointersDs);
  emit_null_stage(img, kConstantVs, kVs, 9, kBindingTablePointersVs, kSamplerStatePointersVs);
  img.header(kStreamout, 5);
  img.put_zeros(4);
  img.header(kClip, 4);
  img.put_zeros(3);

  // Setup: one attribute (the vertex's slot 2) with all four components, read from the vertex entry from its row 1 on; the rasteriser culls nothing, winding counter-clockwise.
  img.header(kSbe, 6);
  img.put((1u << 22) | (1u << 29) | (1u << 28) | (1u << 11) | (1u << 5));
  img.put(0);
  img.put(0);
  img.put(3);                       // attribute 0: x, y, z and w
  img.put(0);
  img.header(kSbeSwiz, 11);
  img.put_zeros(10);
  img.header(kRaster, 5);
  img.put(kFrontWindingCcw | kCullNone);
  img.put_zeros(3);
  img.header(kSf, 4);
  img.put_zeros(3);

  // Windower and pixel shader: perspective pixel barycentrics, SIMD16, the kernel, blending as chosen.
  img.header(kWm, 2);
  img.put(1u << 11);
  img.header(kConstantPs, 11);
  img.put_zeros(10);
  img.header(kPs, 12);
  img.put(job.kernel);
  img.put(0);
  img.put((job.sampler_count ? 1u << 27 : 0u) | (job.binding_entries << 18));
  img.put(0);
  img.put(0);
  img.put((62u << 23) | (1u << 1));  // 63 threads, 16-pixel dispatch
  img.put(6u << 16);                 // setup data from g6
  img.put_zeros(4);
  img.header(kPsBlend, 2);
  img.put((1u << 30) | (job.blend.enable ? (1u << 29) | (1u << 7) : 0u) | (job.blend.src_alpha << 24) | (job.blend.dst_alpha << 19) | (job.blend.src << 14) |
          (job.blend.dst << 9));
  img.header(kPsExtra, 2);
  img.put((1u << 31) | (1u << 8));

  img.header(kBindingTablePointersPs, 2);
  img.put(job.binding_table);
  img.header(kSamplerStatePointersPs, 2);
  img.put(job.sampler);
  img.header(kScissorStatePointers, 2);
  img.put(scissor);

  // The depth buffer if there is one (its test and write as chosen: less-or-equal, or always for a clear); never a stencil or hierarchical depth buffer.
  const bool write = job.depth.attached() && job.depth_write, test = job.depth.attached() && (job.depth_test || job.depth_always);
  // The stencil test works on a stencil buffer next to the depth buffer; with no test the buffer is not attached.
  const bool stencil_on = job.depth.attached() && job.stencil.attached() && job.stencil_test.enable;
  const StencilTest& st = job.stencil_test;
  const bool stencil_write = stencil_on && st.write_mask != 0;
  img.header(kWmDepthStencil, 4);
  uint32_t dword1 = (write ? 1u : 0u) | (test ? 1u << 1 : 0u) | ((job.depth_always ? kCompareAlways : kCompareLessEqual) << 5);
  uint32_t dword2 = 0, dword3 = 0;
  if (stencil_on) {
    // 3DSTATE_WM_DEPTH_STENCIL: stencil write and test enable, the test function, the three operations (pass, depth fail, fail), the masks and the reference.
    dword1 |= (stencil_write ? 1u << 2 : 0u) | (1u << 3) | (static_cast<uint32_t>(st.func) << 8) | (static_cast<uint32_t>(st.pass) << 23) | (static_cast<uint32_t>(st.depth_fail) << 26) |
              (static_cast<uint32_t>(st.fail) << 29);
    dword2 = (static_cast<uint32_t>(st.write_mask) << 16) | (static_cast<uint32_t>(st.compare_mask) << 24);
    dword3 = static_cast<uint32_t>(st.reference) << 8;
  }
  img.put(dword1);
  img.put(dword2);
  img.put(dword3);
  img.header(kDepthBuffer, 8);
  if (job.depth.attached()) {
    // 3DSTATE_DEPTH_BUFFER: pitch less one, format D32_FLOAT, stencil write and depth write enable, a 2D surface; the base; size less one; memory object control; the QPitch in rows of four.
    img.put((job.depth.pitch() - 1) | (kDepthFormatD32Float << 18) | (write ? 1u << 28 : 0u) | (stencil_write ? 1u << 27 : 0u) | (kSurfaceState2d << 29));
    img.put(static_cast<uint32_t>(job.depth.gpu_address));
    img.put(static_cast<uint32_t>(job.depth.gpu_address >> 32));
    img.put(((job.depth.width - 1) << 4) | ((job.depth.height - 1) << 18));
    img.put(kMocsPageTable << 1);
    img.put(0);
    img.put((job.depth.height + 3) / 4);
  } else {
    img.put_zeros(7);
  }
  img.header(kHierDepthBuffer, 5);
  img.put_zeros(4);
  img.header(kStencilBuffer, 5);
  if (stencil_on) {
    // 3DSTATE_STENCIL_BUFFER: the pitch less one, the memory object control, enable; the base; the QPitch (half the height rounded up to 4).
    img.put((job.stencil.pitch() - 1) | ((kMocsPageTable << 1) << 22) | (1u << 31));
    img.put(static_cast<uint32_t>(job.stencil.gpu_address));
    img.put(static_cast<uint32_t>(job.stencil.gpu_address >> 32));
    img.put(((job.stencil.height + 3) / 4 * 4) >> 1);
  } else {
    img.put_zeros(4);
  }
  img.header(kClearParams, 3);
  img.put(0);
  img.put(1);

  img.header(kDrawingRectangle, 4);
  img.put((static_cast<uint32_t>(job.drawing.y0) << 16) | static_cast<uint32_t>(job.drawing.x0));
  img.put((static_cast<uint32_t>(job.drawing.y1 - 1) << 16) | static_cast<uint32_t>(job.drawing.x1 - 1));
  img.put(0);

  const uint64_t vertex_address = uint64_t{base} + job.vertex_offset;
  img.header(kVertexBuffers, 5);
  img.put((job.copy_vertices ? 12u : kVertexStride) | (1u << 14));   // buffer 0, address modify enable, pitch
  img.put(static_cast<uint32_t>(vertex_address));
  img.put(static_cast<uint32_t>(vertex_address >> 32));
  img.put(job.vertex_bytes);

  // The vertex entry: a row of zeros, then the position, then attribute 0.
  const uint32_t src4 = (kVfCompStoreSrc << 28) | (kVfCompStoreSrc << 24) | (kVfCompStoreSrc << 20) | (kVfCompStoreSrc << 16);
  const uint32_t zeros = (kVfCompStore0 << 28) | (kVfCompStore0 << 24) | (kVfCompStore0 << 20) | (kVfCompStore0 << 16);
  const uint32_t xy01 = (kVfCompStoreSrc << 28) | (kVfCompStoreSrc << 24) | (kVfCompStore0 << 20) | (kVfCompStore1Float << 16);
  img.header(kVertexElements, 7);
  img.put(kVeValid | (kFormatR32G32B32A32Float << 16));
  img.put(zeros);
  if (job.copy_vertices) {
    img.put(kVeValid | (kFormatR16G16Sscaled << 16) | 0);
    img.put(xy01);
    img.put(kVeValid | (kFormatR32G32Float << 16) | 4);
    img.put(xy01);
  } else {
    img.put(kVeValid | (kFormatR32G32B32A32Float << 16) | 0);
    img.put(src4);
    img.put(kVeValid | (kFormatR32G32B32A32Float << 16) | 16);
    img.put(src4);
  }

  img.header(kVfTopology, 2);
  img.put(job.topology);
  img.header(kVf, 2);
  img.put(0);
  img.header(kVfInstancing, 3);
  img.put_zeros(2);
  img.header(k3dPrimitive, 7);
  img.put(0);
  img.put(job.vertex_count);
  img.put(0);
  img.put(1);
  img.put(0);
  img.put(0);

  // Flush the render target cache before the job ends.
  img.header(kPipeControl, 6);
  // (and drop the texture cache: a later job may sample what this one drew, or what the CPU has written since)
  img.put(kPcRenderTargetCacheFlush | kPcDepthCacheFlush | kPcDcFlush | kPcTextureCacheInvalidate | kPcCsStall);
  img.put_zeros(4);
  img.put(kMiBatchBufferEnd);
}

// The depth buffer, if there is one, is valid and the size of the target.
bool depth_matches(const DrawState& draw) {
  // A stencil buffer needs the depth buffer next to it and is the target's size too.
  if (draw.stencil.attached() && (!draw.depth.attached() || !draw.stencil.valid() || draw.stencil.width != draw.target.width || draw.stencil.height != draw.target.height)) return false;
  if (!draw.depth.attached()) return true;   // (a depth test with no buffer passes: the state is left out)
  return draw.depth.valid() && draw.depth.width == draw.target.width && draw.depth.height == draw.target.height;
}

bool clip_to_target(const DrawState& draw, Rect* out) {
  Rect r = draw.clip;
  const Rect whole = {0, 0, static_cast<int32_t>(draw.target.width), static_cast<int32_t>(draw.target.height)};
  if (r.empty()) r = whole;
  if (r.x0 < whole.x0) r.x0 = whole.x0;
  if (r.y0 < whole.y0) r.y0 = whole.y0;
  if (r.x1 > whole.x1) r.x1 = whole.x1;
  if (r.y1 > whole.y1) r.y1 = whole.y1;
  *out = r;
  return !r.empty();
}
}  // namespace

uint32_t float_bits(float value) { return __builtin_bit_cast(uint32_t, value); }

bool mip_layout(uint32_t width, uint32_t height, uint32_t levels, bool tiled, MipLayout* out) {
  if (width == 0 || height == 0 || width > kMaxSurfaceSize || height > kMaxSurfaceSize || levels == 0 || levels > kMaxSurfaceLevels || (levels > 1 && !tiled)) return false;
  // A chain stops at 1 x 1 (the larger side halves to 1).
  uint32_t larger = width > height ? width : height, possible = 1;
  while (larger > 1) {
    larger >>= 1;
    ++possible;
  }
  if (levels > possible) return false;
  MipLayout l;
  l.levels = levels;
  for (uint32_t i = 0; i < levels; ++i) {
    l.w[i] = minify(width, i);
    l.h[i] = minify(height, i);
  }
  if (!tiled) {
    l.pitch = align_to(width * 4, 64);
    l.rows = height;
    l.slice_rows = align_to(height, 4);
    *out = l;
    return true;
  }
  uint32_t top_w = 0, bottom_w = 0, left_h = 0, right_h = 0;
  for (uint32_t i = 0; i < levels; ++i) {
    const uint32_t w = align_to(l.w[i], 4), h = align_to(l.h[i], 4);
    if (i == 0) {
      top_w = w;
      left_h = right_h = h;
    } else if (i == 1) {
      bottom_w = w;
      left_h += h;
      l.y[1] = align_to(l.h[0], 4);
    } else {
      if (i == 2) {
        bottom_w += w;
        l.x[2] = align_to(l.w[1], 4);
        l.y[2] = align_to(l.h[0], 4);
      } else {
        l.x[i] = l.x[2];
        l.y[i] = l.y[i - 1] + align_to(l.h[i - 1], 4);
      }
      right_h += h;
    }
  }
  const uint32_t slice_w = top_w > bottom_w ? top_w : bottom_w;
  l.slice_rows = left_h > right_h ? left_h : right_h;
  l.pitch = align_to(slice_w * 4, 128);
  l.rows = align_to(l.slice_rows, 32);
  *out = l;
  return true;
}

uint64_t y_tile_offset(uint32_t pitch, uint32_t x_byte, uint32_t y) {
  const uint64_t tile = uint64_t{y / 32} * (pitch / 128) + x_byte / 128;
  const uint32_t in_x = x_byte % 128, in_y = y % 32;
  return tile * 4096 + uint64_t{in_x / 16} * 512 + uint64_t{in_y} * 16 + in_x % 16;
}

bool Surface::valid() const {
  if (width == 0 || height == 0 || width > kMaxSurfaceSize || height > kMaxSurfaceSize || pitch < width * 4 || pitch > kMaxPitch || (pitch & 63) != 0 || (gpu_address & 63) != 0 ||
      gpu_address == 0 || (format != SurfaceFormat::B8G8R8A8Unorm && format != SurfaceFormat::R8G8B8A8Unorm))
    return false;
  if (levels == 0 || levels > kMaxSurfaceLevels || (levels > 1 && !tiled_y)) return false;
  if (tiled_y && ((pitch & 127) != 0 || (gpu_address & 4095) != 0)) return false;
  return true;
}

ArrayView<const uint32_t> copy_kernel() { return ArrayView<const uint32_t>(kCopyKernel); }
ArrayView<const uint32_t> gouraud_kernel() { return ArrayView<const uint32_t>(kGouraudKernel); }

bool flat_kernel(ArrayView<uint32_t> out, const float rgba[4]) {
  if (out.size() != kFlatKernelDwords) return false;
  // Eight 8-wide moves of an immediate into g112..g119 (a SIMD16 colour component is two registers), then the render target write.
  for (uint32_t n = 0; n < 8; ++n) {
    out[n * 4 + 0] = 0x00600001;
    out[n * 4 + 1] = 0x2e000608u + n * 0x00200000u;
    out[n * 4 + 2] = 0;
    out[n * 4 + 3] = float_bits(rgba[n / 2]);
  }
  out[32] = 0x05800031;
  out[33] = 0x200022e0;
  out[34] = 0x0e000e00;
  out[35] = 0x90031000;
  return true;
}

uint32_t RenderBatch::max_vertices() const {
  const uint64_t bytes = image_.size() * 4;
  // The state other than the vertices takes well under 2 KiB.
  if (bytes < kStateOffset + 2048) return 0;
  return static_cast<uint32_t>((bytes - kStateOffset - 2048) / kVertexStride);
}

bool StencilBuffer::valid() const {
  return width != 0 && height != 0 && width <= kMaxSurfaceSize && height <= kMaxSurfaceSize && gpu_address != 0 && (gpu_address & 4095) == 0;
}

bool DepthBuffer::valid() const {
  return width != 0 && height != 0 && width <= kMaxSurfaceSize && height <= kMaxSurfaceSize && gpu_address != 0 && (gpu_address & 4095) == 0;
}

bool RenderBatch::clear_stencil(const DrawState& draw, uint8_t value) {
  const float none[4] = {0, 0, 0, 0};
  if (!draw.stencil.attached() || !draw.depth.attached()) return false;
  // A rectangle over everything whose stencil test always passes and replaces the stored value; colours and depths are masked off.
  DrawState clear = draw;
  clear.depth_test = clear.depth_write = false;
  clear.stencil_test = StencilTest();
  clear.stencil_test.enable = true;
  clear.stencil_test.func = CompareFunc::Always;
  clear.stencil_test.fail = clear.stencil_test.depth_fail = clear.stencil_test.pass = StencilAction::Replace;
  clear.stencil_test.reference = value;
  return fill(clear, {0, 0, static_cast<int32_t>(draw.stencil.width), static_cast<int32_t>(draw.stencil.height)}, none, 0.0f, false, true);
}

bool RenderBatch::fill_rect(const DrawState& draw, const Rect& rect, const float rgba[4]) { return fill(draw, rect, rgba, 0.0f, false); }

bool RenderBatch::clear_depth(const DrawState& draw, float value) {
  const float none[4] = {0, 0, 0, 0};
  if (!(value >= 0.0f && value <= 1.0f) || !draw.depth.attached()) return false;
  return fill(draw, {0, 0, static_cast<int32_t>(draw.depth.width), static_cast<int32_t>(draw.depth.height)}, none, value, true);
}

// A rectangle list of one solid colour at depth `z`; with `depth_only` the colour is not written and every pixel passes the depth test (a depth clear).
bool RenderBatch::fill(const DrawState& draw, const Rect& rect, const float rgba[4], float z, bool depth_only, bool mask_color) {
  used_ = 0;
  Image img(image_, base_);
  if (!img.valid_buffer() || !draw.target.valid() || rect.empty() || !depth_matches(draw)) return false;
  Rect drawing;
  if (!clip_to_target(draw, &drawing)) return false;
  // Out of reach of the 16-bit window coordinates, or not a number.
  const int32_t limit = static_cast<int32_t>(kMaxCoordinate);
  if (rect.x0 < -limit || rect.y0 < -limit || rect.x1 > limit || rect.y1 > limit) return false;
  for (int i = 0; i < 4; ++i)
    if (!(rgba[i] == rgba[i])) return false;
  img.reset();

  Job job;
  job.blend = blend_factors(draw.blend);
  job.drawing = drawing;
  job.topology = kTopologyRectList;
  job.vertex_count = 3;
  job.binding_entries = 1;
  job.depth = draw.depth;
  job.depth_test = draw.depth_test;
  job.depth_write = draw.depth_write || depth_only;
  job.depth_always = depth_only;
  job.stencil = draw.stencil;
  job.stencil_test = draw.stencil_test;
  const uint32_t surface = surface_state(img, draw.target);
  job.binding_table = img.allocate(4, 32);
  const uint32_t kernel = img.allocate(kFlatKernelDwords * 4, 64);
  job.vertex_bytes = 3 * kVertexStride;
  job.vertex_offset = img.allocate(job.vertex_bytes, 64);
  const uint32_t blend = blend_state(img, job.blend, !depth_only && !mask_color), cc = cc_state(img), vp_cc = cc_viewport(img), vp_sf = sf_clip_viewport(img), scissor = img.allocate(8, 64);
  if (!img.ok()) return false;
  img.state(job.binding_table, 0, surface);
  uint32_t flat[kFlatKernelDwords];
  flat_kernel(ArrayView<uint32_t>(flat), rgba);
  img.copy_state(kernel, ArrayView<const uint32_t>(flat));
  job.kernel = kernel;
  // A rectangle list: the lower right, lower left and upper left corners; the fourth is implied.
  const float x0 = static_cast<float>(rect.x0), y0 = static_cast<float>(rect.y0), x1 = static_cast<float>(rect.x1), y1 = static_cast<float>(rect.y1);
  const float corners[3][2] = {{x1, y1}, {x0, y1}, {x0, y0}};
  for (uint32_t v = 0; v < 3; ++v) {
    const float values[8] = {corners[v][0], corners[v][1], z, 1.0f, rgba[0], rgba[1], rgba[2], rgba[3]};
    for (uint32_t k = 0; k < 8; ++k) img.state(job.vertex_offset, v * 8 + k, float_bits(values[k]));
  }
  emit_job(img, job, blend, cc, vp_cc, vp_sf, scissor);
  if (!img.ok()) return false;
  used_ = img.dwords_used();
  return true;
}

bool RenderBatch::triangles(const DrawState& draw, ArrayView<const Vertex> vertices, bool gouraud, const float flat_rgba[4]) {
  used_ = 0;
  Image img(image_, base_);
  if (!img.valid_buffer() || !draw.target.valid() || vertices.empty() || vertices.size() % 3 != 0 || vertices.size() > max_vertices() || !depth_matches(draw)) return false;
  Rect drawing;
  if (!clip_to_target(draw, &drawing)) return false;
  for (const Vertex& v : vertices) {
    if (!finite_and_bounded(v.position[0]) || !finite_and_bounded(v.position[1]) || !finite_and_bounded(v.position[2]) || !(v.position[3] > 0.0f && v.position[3] < kMaxInverseW)) return false;
    for (const float c : v.color)
      if (!(c == c)) return false;
  }
  if (!gouraud)
    for (int i = 0; i < 4; ++i)
      if (!(flat_rgba[i] == flat_rgba[i])) return false;
  img.reset();

  Job job;
  job.blend = blend_factors(draw.blend);
  job.drawing = drawing;
  job.topology = kTopologyTriangleList;
  job.vertex_count = static_cast<uint32_t>(vertices.size());
  job.binding_entries = 1;
  job.depth = draw.depth;
  job.depth_test = draw.depth_test;
  job.depth_write = draw.depth_write;
  job.stencil = draw.stencil;
  job.stencil_test = draw.stencil_test;
  const uint32_t surface = surface_state(img, draw.target);
  job.binding_table = img.allocate(4, 32);
  const uint32_t kernel = img.allocate(gouraud ? static_cast<uint32_t>(gouraud_kernel().size() * 4) : kFlatKernelDwords * 4, 64);
  job.vertex_bytes = job.vertex_count * kVertexStride;
  job.vertex_offset = img.allocate(job.vertex_bytes, 64);
  const uint32_t blend = blend_state(img, job.blend, true), cc = cc_state(img), vp_cc = cc_viewport(img), vp_sf = sf_clip_viewport(img), scissor = img.allocate(8, 64);
  if (!img.ok()) return false;
  img.state(job.binding_table, 0, surface);
  if (gouraud) {
    img.copy_state(kernel, gouraud_kernel());
  } else {
    uint32_t flat[kFlatKernelDwords];
    flat_kernel(ArrayView<uint32_t>(flat), flat_rgba);
    img.copy_state(kernel, ArrayView<const uint32_t>(flat));
  }
  job.kernel = kernel;
  for (uint32_t v = 0; v < vertices.size(); ++v) {
    for (uint32_t k = 0; k < 4; ++k) img.state(job.vertex_offset, v * 8 + k, float_bits(vertices[v].position[k]));
    for (uint32_t k = 0; k < 4; ++k) img.state(job.vertex_offset, v * 8 + 4 + k, float_bits(vertices[v].color[k]));
  }
  emit_job(img, job, blend, cc, vp_cc, vp_sf, scissor);
  if (!img.ok()) return false;
  used_ = img.dwords_used();
  return true;
}

bool RenderBatch::textured_triangles(const DrawState& draw, ArrayView<const Vertex> vertices, const Surface& texture, const Sampler& sampler) {
  used_ = 0;
  Image img(image_, base_);
  if (!img.valid_buffer() || !draw.target.valid() || !texture.valid() || vertices.empty() || vertices.size() % 3 != 0 || vertices.size() > max_vertices() || !depth_matches(draw)) return false;
  // Sampling the surface that is being drawn into is not defined.
  if (texture.gpu_address < draw.target.gpu_address + uint64_t{draw.target.pitch} * draw.target.height && draw.target.gpu_address < texture.gpu_address + uint64_t{texture.pitch} * texture.height) return false;
  Rect drawing;
  if (!clip_to_target(draw, &drawing)) return false;
  for (const Vertex& v : vertices) {
    if (!finite_and_bounded(v.position[0]) || !finite_and_bounded(v.position[1]) || !finite_and_bounded(v.position[2]) || !(v.position[3] > 0.0f && v.position[3] < kMaxInverseW)) return false;
    for (const float c : v.color)
      if (!(c == c)) return false;
  }
  img.reset();

  Job job;
  job.blend = blend_factors(draw.blend);
  job.drawing = drawing;
  job.topology = kTopologyTriangleList;
  job.vertex_count = static_cast<uint32_t>(vertices.size());
  job.binding_entries = 2;
  job.sampler_count = 1;
  job.depth = draw.depth;
  job.depth_test = draw.depth_test;
  job.depth_write = draw.depth_write;
  job.stencil = draw.stencil;
  job.stencil_test = draw.stencil_test;
  const uint32_t target_state = surface_state(img, draw.target), texture_state = surface_state(img, texture);
  job.binding_table = img.allocate(8, 32);
  job.sampler = sampler_state(img, sampler);
  const uint32_t kernel = img.allocate(static_cast<uint32_t>(copy_kernel().size() * 4), 64);
  job.vertex_bytes = job.vertex_count * kVertexStride;
  job.vertex_offset = img.allocate(job.vertex_bytes, 64);
  const uint32_t blend = blend_state(img, job.blend, true), cc = cc_state(img), vp_cc = cc_viewport(img), vp_sf = sf_clip_viewport(img), scissor = img.allocate(8, 64);
  if (!img.ok()) return false;
  img.state(job.binding_table, 0, target_state);
  img.state(job.binding_table, 1, texture_state);
  img.copy_state(kernel, copy_kernel());
  job.kernel = kernel;
  // The copy kernel reads attribute 0's x and y as u and v: the vertex's colour slot carries them.
  for (uint32_t v = 0; v < vertices.size(); ++v) {
    for (uint32_t k = 0; k < 4; ++k) img.state(job.vertex_offset, v * 8 + k, float_bits(vertices[v].position[k]));
    for (uint32_t k = 0; k < 4; ++k) img.state(job.vertex_offset, v * 8 + 4 + k, float_bits(vertices[v].color[k]));
  }
  emit_job(img, job, blend, cc, vp_cc, vp_sf, scissor);
  if (!img.ok()) return false;
  used_ = img.dwords_used();
  return true;
}

bool RenderBatch::copy(const Surface& dst, const Surface& src, const Rect& src_rect, int32_t dst_x, int32_t dst_y) {
  used_ = 0;
  Image img(image_, base_);
  if (!img.valid_buffer() || !dst.valid() || !src.valid() || src_rect.empty()) return false;
  // The source rectangle lies inside the source; the destination one is whatever part of it falls inside the destination.
  if (src_rect.x0 < 0 || src_rect.y0 < 0 || src_rect.x1 > static_cast<int32_t>(src.width) || src_rect.y1 > static_cast<int32_t>(src.height)) return false;
  const int32_t w = src_rect.x1 - src_rect.x0, h = src_rect.y1 - src_rect.y0;
  const int64_t dx1 = int64_t{dst_x} + w, dy1 = int64_t{dst_y} + h;
  if (dst_x < -static_cast<int32_t>(kMaxSurfaceSize) || dst_y < -static_cast<int32_t>(kMaxSurfaceSize) || dx1 > kMaxSurfaceSize || dy1 > kMaxSurfaceSize) return false;
  Rect drawing = {dst_x, dst_y, static_cast<int32_t>(dx1), static_cast<int32_t>(dy1)};
  if (drawing.x0 < 0) drawing.x0 = 0;
  if (drawing.y0 < 0) drawing.y0 = 0;
  if (drawing.x1 > static_cast<int32_t>(dst.width)) drawing.x1 = static_cast<int32_t>(dst.width);
  if (drawing.y1 > static_cast<int32_t>(dst.height)) drawing.y1 = static_cast<int32_t>(dst.height);
  if (drawing.empty()) return false;
  img.reset();

  Job job;
  job.blend = blend_factors(Blend::None);
  job.drawing = drawing;
  job.topology = kTopologyRectList;
  job.vertex_count = 3;
  job.binding_entries = 2;
  job.sampler_count = 1;
  const uint32_t dst_state = surface_state(img, dst), src_state = surface_state(img, src);
  job.binding_table = img.allocate(8, 32);
  job.sampler = sampler_state(img, Sampler());
  const uint32_t kernel = img.allocate(static_cast<uint32_t>(copy_kernel().size() * 4), 64);
  job.vertex_bytes = 3 * 12;
  job.vertex_offset = img.allocate(job.vertex_bytes, 64);
  const uint32_t blend = blend_state(img, job.blend, true), cc = cc_state(img), vp_cc = cc_viewport(img), vp_sf = sf_clip_viewport(img), scissor = img.allocate(8, 64);
  if (!img.ok()) return false;
  img.state(job.binding_table, 0, dst_state);
  img.state(job.binding_table, 1, src_state);
  img.copy_state(kernel, copy_kernel());
  job.kernel = kernel;
  // Three vertices of a rectangle list: window position as two signed 16-bit values, texture coordinates as normalised floats. Order: lower right, lower left, upper left.
  const float sw = static_cast<float>(src.width), sh = static_cast<float>(src.height);
  const int32_t px[3] = {dst_x + w, dst_x, dst_x}, py[3] = {dst_y + h, dst_y + h, dst_y};
  const float u[3] = {static_cast<float>(src_rect.x1) / sw, static_cast<float>(src_rect.x0) / sw, static_cast<float>(src_rect.x0) / sw};
  const float v[3] = {static_cast<float>(src_rect.y1) / sh, static_cast<float>(src_rect.y1) / sh, static_cast<float>(src_rect.y0) / sh};
  for (uint32_t i = 0; i < 3; ++i) {
    img.state(job.vertex_offset, i * 3 + 0, (static_cast<uint32_t>(static_cast<uint16_t>(py[i])) << 16) | static_cast<uint32_t>(static_cast<uint16_t>(px[i])));
    img.state(job.vertex_offset, i * 3 + 1, float_bits(u[i]));
    img.state(job.vertex_offset, i * 3 + 2, float_bits(v[i]));
  }
  emit_job(img, job, blend, cc, vp_cc, vp_sf, scissor);
  if (!img.ok()) return false;
  used_ = img.dwords_used();
  return true;
}

}  // namespace gen9_3d
}  // namespace intel_gt
}  // namespace hardware
