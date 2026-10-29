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

// The PM4 path for the Vega M: draw state emitted as packets (GcnState), checked for the register values the kernel's headers define, and run on a model of the command processor
// (GcnCp) that reads the registers back, fetches the shaders, constants, descriptors and vertices from memory and draws on the wave emulator. The colour buffer in memory must equal what
// GcnDraw gives for the same draw; packets of states the model does not know are refused; every truncation of the stream and every flipped bit must end in a result, not a crash.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <map>
#include "Hardware/GraphicsProcessingUnit/AMD/Gcn/GcnCp.hpp"
#include "Hardware/GraphicsProcessingUnit/AMD/Gcn/GcnState.hpp"
#include "Hardware/GraphicsProcessingUnit/AMD/Gcn/GcnTiling.hpp"

using namespace gcn;

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

struct Memory final : IGpuMemory {
  std::map<uint64_t, uint32_t> words;
  bool read(uint64_t address, ArrayView<uint32_t> out) override {
    if (address & 3) return false;
    for (uint64_t i = 0; i < out.size(); ++i) {
      auto it = words.find(address + 4 * i);
      if (it == words.end()) return false;
      out[i] = it->second;
    }
    return true;
  }
  bool write(uint64_t address, ArrayView<const uint32_t> data) override {
    if (address & 3) return false;
    for (uint64_t i = 0; i < data.size(); ++i) {
      auto it = words.find(address + 4 * i);
      if (it == words.end()) return false;         // only mapped memory is written
      it->second = data[i];
    }
    return true;
  }
  void map(uint64_t address, uint64_t dwords, uint32_t value = 0) { for (uint64_t i = 0; i < dwords; ++i) words[address + 4 * i] = value; }
  void put(uint64_t address, ArrayView<const uint32_t> data) { for (uint64_t i = 0; i < data.size(); ++i) words[address + 4 * i] = data[i]; }
};

static const char* kVertex =
    "VERT\nDCL IN[0]\nDCL IN[1]\nDCL OUT[0], POSITION\nDCL OUT[1], GENERIC[0]\nDCL CONST[0..3]\nDCL TEMP[0]\n"
    "  0: MUL TEMP[0], CONST[0], IN[0].xxxx\n  1: MAD TEMP[0], CONST[1], IN[0].yyyy, TEMP[0]\n  2: MAD TEMP[0], CONST[2], IN[0].zzzz, TEMP[0]\n  3: ADD OUT[0], TEMP[0], CONST[3]\n  4: MOV OUT[1], IN[1]\n  5: END\n";
static const char* kTexFragment = "FRAG\nDCL IN[0], GENERIC[0], PERSPECTIVE\nDCL OUT[0], COLOR\nDCL SAMP[0]\nDCL SVIEW[0], 2D, FLOAT\n  0: TEX OUT[0], IN[0], SAMP[0], 2D\n  1: END\n";
static const char* kFragment = "FRAG\nDCL IN[0], GENERIC[0], PERSPECTIVE\nDCL OUT[0], COLOR\n  0: MOV OUT[0], IN[0]\n  1: END\n";

struct Program {
  List<uint32_t> words;
  ShaderInfo info;
  explicit Program(allocators::IAllocator& a) : words(a) {}
  bool ok = false;
};

static void compile(allocators::IAllocator& alloc, const char* text, Program* out) {
  TgsiShader sh(alloc);
  const char* error = nullptr;
  uint32_t line = 0;
  if (!parse_tgsi(alloc, StringView(text), &sh, &error, &line)) return;
  Code code(alloc);
  if (!compile_tgsi(sh, code, &out->info, &error)) return;
  for (const uint32_t w : code.words()) out->words.push_back(w);
  out->ok = true;
}

constexpr uint64_t kVsAddr = 0x100000, kPsAddr = 0x101000, kVsConst = 0x200000, kPsConst = 0x200400, kTable = 0x300000, kVertices = 0x400000, kIndices = 0x500000, kFence = 0x600000;
constexpr uint64_t kTarget = 0x1000000, kDepthPlane = 0x2000000, kStencilPlane = 0x3000000, kTexture = 0x4000000, kPsAddr2 = 0x102000;
constexpr uint32_t kWidth = 40, kHeight = 30, kPitch = 48, kPaddedHeight = 32;

struct Rig {
  TestAllocator alloc;
  Program vs, ps, tps;
  Memory mem;
  Rig() : vs(alloc), ps(alloc), tps(alloc) {
    compile(alloc, kVertex, &vs);
    compile(alloc, kFragment, &ps);
    compile(alloc, kTexFragment, &tps);
    mem.put(kPsAddr2, ArrayView<const uint32_t>(tps.words.view().data(), tps.words.count()));
    mem.put(kVsAddr, ArrayView<const uint32_t>(vs.words.view().data(), vs.words.count()));
    mem.put(kPsAddr, ArrayView<const uint32_t>(ps.words.view().data(), ps.words.count()));
    // four constant rows only: the model reads as many as are mapped (the identity matrix)
    const float identity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    for (uint32_t i = 0; i < 16; ++i) { uint32_t b; memcpy(&b, &identity[i], 4); mem.words[kVsConst + 4 * i] = b; }
    mem.map(kPsConst, 4);
    mem.map(kTable, abi::kTableBytes / 4);
    mem.map(kFence, 2);
    mem.map(kTarget, static_cast<uint64_t>(kPitch) * kPaddedHeight * 4);
    clear_target(-1.0f);
  }
  void clear_target(float v) {
    uint32_t b;
    memcpy(&b, &v, 4);
    for (uint64_t i = 0; i < static_cast<uint64_t>(kPitch) * kPaddedHeight * 4; ++i) mem.words[kTarget + 4 * i] = b;
  }
  float pixel(uint32_t x, uint32_t y, uint32_t c) {
    float f;
    const uint32_t b = mem.words[kTarget + (static_cast<uint64_t>(y) * kPitch + x) * 16 + 4 * c];
    memcpy(&f, &b, 4);
    return f;
  }
  // vertices (position, colour) at kVertices and their descriptors
  void set_vertices(const float* data, uint32_t count) {
    mem.map(kVertices, count * 7ull);
    for (uint32_t i = 0; i < count * 7; ++i) { uint32_t b; memcpy(&b, &data[i], 4); mem.words[kVertices + 4 * i] = b; }
    uint32_t d[4];
    CHECK(make_vertex_descriptor(ArrayView<uint32_t>(d, 4), kVertices, 28, count, 3));
    mem.put(kTable, ArrayView<const uint32_t>(d, 4));
    CHECK(make_vertex_descriptor(ArrayView<uint32_t>(d, 4), kVertices + 12, 28, count, 4));
    mem.put(kTable + 16, ArrayView<const uint32_t>(d, 4));
  }
  DrawState state(uint32_t index_count) {
    DrawState s;
    s.vertex.address = kVsAddr;
    s.vertex.info = vs.info;
    s.pixel.address = kPsAddr;
    s.pixel.info = ps.info;
    s.vertex_constants = kVsConst;
    s.pixel_constants = kPsConst;
    s.descriptor_table = kTable;
    s.target.address = kTarget;
    s.target.width = kWidth;
    s.target.height = kHeight;
    s.target.pitch_pixels = kPitch;
    s.index_count = index_count;
    return s;
  }
};

static const float kTriangle[21] = {-0.8f, -0.7f, 0.5f, 1, 0, 0, 1, 0.9f, -0.5f, 0.5f, 0, 1, 0, 0.5f, -0.1f, 0.8f, 0.5f, 0, 0, 1, 0.25f};

static uint32_t build(const DrawState& s, uint32_t* out, uint32_t capacity, bool* ok) {
  Pm4Writer w(ArrayView<uint32_t>(out, capacity));
  *ok = emit_draw(w, s) && w.ok();
  return w.count();
}

static bool refused(const DrawState& s) {
  uint32_t b[512];
  bool ok;
  build(s, b, 512, &ok);
  return !ok;
}

static void test_registers(Rig& rig) {
  rig.set_vertices(kTriangle, 3);
  DrawState s = rig.state(3);
  s.fence_address = kFence;
  s.fence_value = 0x1122334455667788ull;
  uint32_t buf[512];
  bool ok;
  const uint32_t n = build(s, buf, 512, &ok);
  CHECK(ok && n > 50);
  CommandProcessorModel cp(rig.alloc, rig.mem);
  { const auto res = cp.execute(ArrayView<const uint32_t>(buf, n)); if (res != CommandProcessorModel::Result::Ok) printf("result: %s\n", CommandProcessorModel::result_name(res)); }
  // the shader programs (SPI_SHADER_PGM_LO_VS 0x2c48, HI 0x2c49, RSRC1 0x2c4a, RSRC2 0x2c4b, USER_DATA 0x2c4c; the pixel shader's 0x2c08 ... 0x2c0c)
  CHECK(cp.sh_reg(0x2c48) == kVsAddr >> 8 && cp.sh_reg(0x2c49) == 0 && cp.sh_reg(0x2c08) == kPsAddr >> 8 && cp.sh_reg(0x2c09) == 0);
  CHECK(cp.sh_reg(0x2c4b) == (4u << 1) && cp.sh_reg(0x2c0b) == (4u << 1));
  const uint32_t r1 = cp.sh_reg(0x2c4a);
  CHECK((r1 & 0x3F) == (rig.vs.info.vgprs - 1) / 4 && ((r1 >> 6) & 0xF) == (rig.vs.info.sgprs + 6 + 7) / 8 - 1 && ((r1 >> 12) & 0xFF) == 0xC0 && ((r1 >> 21) & 1) == 1);
  CHECK(cp.sh_reg(0x2c4c) == kVsConst && cp.sh_reg(0x2c4d) == 0 && cp.sh_reg(0x2c4e) == kTable && cp.sh_reg(0x2c4f) == 0);
  CHECK(cp.sh_reg(0x2c0c) == kPsConst && cp.sh_reg(0x2c0e) == kTable);
  // the interpolation and the exports
  CHECK(cp.context_reg(0xa1c3) == 4 && cp.context_reg(0xa1c4) == 0 && cp.context_reg(0xa1c5) == 9);          // POS0 4 components, no depth export, COL0 32_ABGR
  CHECK(cp.context_reg(0xa1b1) == 0);                                                                         // one parameter: VS_EXPORT_COUNT 0
  CHECK(cp.context_reg(0xa1b3) == 2 && cp.context_reg(0xa1b4) == 2 && cp.context_reg(0xa1b6) == 1 && cp.context_reg(0xa191) == 0);
  // the colour buffer
  CHECK(cp.context_reg(0xa318) == kTarget >> 8 && cp.context_reg(0xa319) == 5 && cp.context_reg(0xa31a) == 23 && cp.context_reg(0xa31b) == 0);
  CHECK(cp.context_reg(0xa31c) == 0x70738 && cp.context_reg(0xa31d) == 8);
  CHECK(cp.context_reg(0xa08e) == 0xF && cp.context_reg(0xa08f) == 0xF && cp.context_reg(0xa202) == ((1u << 4) | (0xCCu << 16)));
  // the viewport and scissors
  CHECK(cp.context_reg(0xa10f) == 0x41A00000 && cp.context_reg(0xa110) == 0x41A00000 && cp.context_reg(0xa111) == 0xC1700000 && cp.context_reg(0xa112) == 0x41700000);          // 20, 20, -15, 15
  CHECK(cp.context_reg(0xa113) == 0x3F000000 && cp.context_reg(0xa114) == 0x3F000000 && cp.context_reg(0xa206) == 0x43F);
  CHECK(cp.context_reg(0xa00d) == ((30u << 16) | 40u) && cp.context_reg(0xa082) == ((30u << 16) | 40u) && cp.context_reg(0xa081) == 0x80000000u && cp.context_reg(0xa00c) == 0);
  CHECK(cp.uconfig_reg(0xc242) == 4);
  // the draw, the fence
  CHECK(cp.draws() == 1 && cp.fence_writes() == 1 && cp.last_draw().triangles == 1 && cp.last_draw().pixels > 100);
  CHECK(rig.mem.words[kFence] == 0x55667788u && rig.mem.words[kFence + 4] == 0x11223344u);
  // the same picture as GcnDraw gives directly
  Framebuffer fb(rig.alloc, kWidth, kHeight);
  fb.clear(-1, -1, -1, -1);
  DrawCall c;
  c.vertex_code = ArrayView<const uint32_t>(rig.vs.words.view().data(), rig.vs.words.count());
  c.pixel_code = ArrayView<const uint32_t>(rig.ps.words.view().data(), rig.ps.words.count());
  c.vertex_info = rig.vs.info;
  c.pixel_info = rig.ps.info;
  const float identity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
  c.vertex_constants = ArrayView<const float>(identity, 16);
  c.vertex_data = ArrayView<const uint32_t>(reinterpret_cast<const uint32_t*>(kTriangle), 21);       // (floats are 32 bit words)
  c.vertex_stride = 28;
  c.attributes[0] = {3, 0};
  c.attributes[1] = {4, 12};
  const uint32_t idx[3] = {0, 1, 2};
  c.indices = ArrayView<const uint32_t>(idx, 3);
  CHECK(draw(rig.alloc, c, fb) == DrawStatus::Ok);
  uint32_t different = 0, covered = 0;
  for (uint32_t y = 0; y < kHeight; ++y)
    for (uint32_t x = 0; x < kWidth; ++x)
      for (uint32_t k = 0; k < 4; ++k) {
        if (rig.pixel(x, y, k) != fb.pixel(x, y)[k]) ++different;
        if (k == 0 && rig.pixel(x, y, 0) != -1.0f) ++covered;
      }
  CHECK(different == 0 && covered > 100);
  // pixels beyond the width of the target but inside its pitch were not touched
  CHECK(rig.pixel(kWidth, 5, 0) == -1.0f && rig.pixel(kPitch - 1, 29, 3) == -1.0f);
}

static void test_indexed_and_scissor(Rig& rig) {
  // a quad as 4 vertices and 6 indices, 32 bit and 16 bit
  const float quad[28] = {-1, -1, 0.5f, 1, 0, 0, 1, 1, -1, 0.5f, 0, 1, 0, 1, 1, 1, 0.5f, 0, 0, 1, 1, -1, 1, 0.5f, 1, 1, 0, 1};
  rig.set_vertices(quad, 4);
  const uint32_t indices32[6] = {0, 1, 2, 0, 2, 3};
  rig.mem.map(kIndices, 6);
  rig.mem.put(kIndices, ArrayView<const uint32_t>(indices32, 6));
  DrawState s = rig.state(6);
  s.index_address = kIndices;
  s.index_max = 6;
  uint32_t buf[512];
  bool ok;
  uint32_t n = build(s, buf, 512, &ok);
  CHECK(ok);
  CommandProcessorModel cp(rig.alloc, rig.mem);
  CHECK(cp.execute(ArrayView<const uint32_t>(buf, n)) == CommandProcessorModel::Result::Ok);
  CHECK(cp.last_draw().pixels == kWidth * kHeight && cp.last_draw().triangles == 2);
  uint32_t uncovered = 0;
  for (uint32_t y = 0; y < kHeight; ++y)
    for (uint32_t x = 0; x < kWidth; ++x)
      if (rig.pixel(x, y, 0) == -1.0f) ++uncovered;
  CHECK(uncovered == 0);
  // 16 bit indices, two to a dword: the same state, then INDEX_TYPE 0 and a DRAW_INDEX_2 over them. The picture must be as complete as before.
  rig.clear_target(-1.0f);
  const uint32_t indices16[3] = {0x00010000u, 0x00000002u, 0x00030002u};      // 0 1 2 0 2 3
  rig.mem.map(kIndices + 0x1000, 3);
  rig.mem.put(kIndices + 0x1000, ArrayView<const uint32_t>(indices16, 3));
  Pm4Writer w(ArrayView<uint32_t>(buf, 512));
  CHECK(emit_draw(w, s));
  w.index_type(0);
  w.draw_index_2(6, kIndices + 0x1000, 6);
  CHECK(w.ok());
  CommandProcessorModel cp2(rig.alloc, rig.mem);
  CHECK(cp2.execute(w.words()) == CommandProcessorModel::Result::Ok && cp2.draws() == 2 && cp2.last_draw().pixels == kWidth * kHeight);
  uncovered = 0;
  for (uint32_t y = 0; y < kHeight; ++y)
    for (uint32_t x = 0; x < kWidth; ++x)
      if (rig.pixel(x, y, 0) == -1.0f) ++uncovered;
  CHECK(uncovered == 0);
  // a scissor from the registers: the left half of the target only
  rig.clear_target(-1.0f);
  Pm4Writer w2(ArrayView<uint32_t>(buf, 512));
  CHECK(emit_draw(w2, s));
  CommandProcessorModel cp3(rig.alloc, rig.mem);
  // (the registers are set, the draw is over; set the scissor and draw again by a second draw packet)
  CHECK(cp3.execute(w2.words()) == CommandProcessorModel::Result::Ok);
  rig.clear_target(-1.0f);
  Pm4Writer w3(ArrayView<uint32_t>(buf, 64));
  w3.set_context_reg(0xa091, (kHeight << 16) | 20u);                         // PA_SC_GENERIC_SCISSOR_BR: x < 20
  w3.draw_index_2(6, kIndices, 6);
  CHECK(w3.ok() && cp3.execute(w3.words()) == CommandProcessorModel::Result::Ok);
  uint32_t left = 0, right = 0;
  for (uint32_t y = 0; y < kHeight; ++y)
    for (uint32_t x = 0; x < kWidth; ++x)
      if (rig.pixel(x, y, 0) != -1.0f) (x < 20 ? left : right)++;
  CHECK(left == 20 * kHeight && right == 0);
  (void)n;
}

static void test_refusals(Rig& rig) {
  rig.set_vertices(kTriangle, 3);
  uint32_t buf[512];
  bool ok;
  const DrawState good = rig.state(3);
  CHECK(build(good, buf, 512, &ok) > 0 && ok);
  DrawState s = good; s.vertex.address += 4;                       CHECK(refused(s));        // misaligned code
  s = good; s.vertex.address = 1ull << 40;                         CHECK(refused(s));        // beyond 40 bits
  s = good; s.vertex.info.fragment = true;                         CHECK(refused(s));        // wrong stage
  s = good; s.pixel.info.fragment = false;                         CHECK(refused(s));
  s = good; s.target.pitch_pixels = 44;                            CHECK(refused(s));        // not a multiple of 8
  s = good; s.target.pitch_pixels = 32;                            CHECK(refused(s));        // narrower than the target
  s = good; s.target.width = 0;                                    CHECK(refused(s));
  s = good; s.target.address += 16;                                CHECK(refused(s));
  s = good; s.index_count = 0;                                     CHECK(refused(s));
  s = good; s.fence_address = kFence + 4;                          CHECK(refused(s));
  s = good; s.pixel.info.ps_inputs = 17;                           CHECK(refused(s));
  s = good; s.index_address = kIndices; s.index_max = 2;           CHECK(refused(s));        // fewer indices than drawn
  // a buffer that is too small
  { Pm4Writer w(ArrayView<uint32_t>(buf, 20)); CHECK(!emit_draw(w, good) || !w.ok()); }
  // writer limits
  { Pm4Writer w(ArrayView<uint32_t>(buf, 64)); w.set_sh_reg(0x2bff, 1); CHECK(!w.ok()); }
  { Pm4Writer w(ArrayView<uint32_t>(buf, 64)); w.set_context_reg(0xa3ff, ArrayView<const uint32_t>(buf, 2)); CHECK(!w.ok()); }          // runs past the end of the range
  { Pm4Writer w(ArrayView<uint32_t>(buf, 64)); w.set_context_reg(0xa3ff, 1); CHECK(w.ok() && w.count() == 3); }
  { Pm4Writer w(ArrayView<uint32_t>(buf, 64)); w.set_uconfig_reg(0xc400, 1); CHECK(!w.ok()); }
  { Pm4Writer w(ArrayView<uint32_t>(buf, 64)); w.event_write_eop(0x28, 0, 4, 0, 2, 0); CHECK(!w.ok()); }          // not 8 byte aligned
  { Pm4Writer w(ArrayView<uint32_t>(buf, 64)); w.draw_index_2(3, 1, 3); CHECK(!w.ok()); }
  { Pm4Writer w(ArrayView<uint32_t>(buf, 2)); w.draw_index_auto(3); CHECK(!w.ok()); }
  // the descriptor
  uint32_t d[4];
  CHECK(make_vertex_descriptor(ArrayView<uint32_t>(d, 4), 0x400000, 28, 3, 3) && d[0] == 0x400000 && d[1] == (28u << 16) && d[2] == 3 && d[3] == ((7u << 12) | (13u << 15)));
  CHECK(make_vertex_descriptor(ArrayView<uint32_t>(d, 4), 0x1234567890ull & ~3ull, 16, 8, 4) && d[0] == 0x34567890u && d[1] == (0x12u | (16u << 16)) && d[3] == ((7u << 12) | (14u << 15)));
  CHECK(!make_vertex_descriptor(ArrayView<uint32_t>(d, 4), 0x400000, 0, 3, 3) && !make_vertex_descriptor(ArrayView<uint32_t>(d, 4), 0x400000, 28, 3, 5) && !make_vertex_descriptor(ArrayView<uint32_t>(d, 3), 0x400000, 28, 3, 3));

  // a state the model does not know is refused, and the picture is not touched
  bool b;
  const uint32_t n = build(good, buf, 512, &b);
  CHECK(b);
  auto run_with_extra = [&](const char* what, auto&& add) {
    uint32_t b2[600];
    memcpy(b2, buf, n * 4);
    Pm4Writer w(ArrayView<uint32_t>(b2 + n, 600 - n));
    add(w);
    // the extra packets go before the draw: find the draw packet (the first DRAW_INDEX_AUTO) and put them in front of it
    uint32_t at = 0;
    Pm4Reader r(ArrayView<const uint32_t>(buf, n));
    Pm4Packet p;
    uint32_t pos = 0;
    while (r.next(&p)) { if (p.opcode == pm4::kPacket3DrawIndexAuto) { at = pos; break; } pos += 1 + static_cast<uint32_t>(p.body.size()); }
    uint32_t b3[600];
    memcpy(b3, buf, at * 4);
    memcpy(b3 + at, b2 + n, w.count() * 4);
    memcpy(b3 + at + w.count(), buf + at, (n - at) * 4);
    rig.clear_target(-1.0f);
    CommandProcessorModel cp(rig.alloc, rig.mem);
    const CommandProcessorModel::Result res = cp.execute(ArrayView<const uint32_t>(b3, n + w.count()));
    if (res != CommandProcessorModel::Result::Unsupported) { printf("FAILED %s: %s\n", what, CommandProcessorModel::result_name(res)); ++g_failed; }
    CHECK(rig.pixel(10, 10, 0) == -1.0f && cp.draws() == 0);
  };
  // (blending is supported now; what is refused is dual source blending, with the blend bypass bit of the target off so that the blend is not bypassed)
  run_with_extra("dual source blending", [&](Pm4Writer& w) { w.set_context_reg(0xa31c, 0x70738 & ~(1u << pm4::CB_COLOR0_INFO__BLEND_BYPASS__SHIFT)); w.set_context_reg(0xa1e0, (1u << 30) | 0xF | (0xFu << 8)); });
  run_with_extra("logic op", [&](Pm4Writer& w) { w.set_context_reg(0xa202, (1u << 4) | (0x88u << 16)); });
  run_with_extra("tile mode 2D", [&](Pm4Writer& w) { w.set_context_reg(0xa31d, 10); });
  run_with_extra("back face stencil", [&](Pm4Writer& w) { w.set_context_reg(0xa200, (1u << 0) | (1u << 7)); });
  run_with_extra("depth bounds", [&](Pm4Writer& w) { w.set_context_reg(0xa200, 1u << 3); });
  run_with_extra("depth clear", [&](Pm4Writer& w) { w.set_context_reg(0xa000, 1u); });
  run_with_extra("depth test", [&](Pm4Writer& w) { w.set_context_reg(0xa200, 1u << 1); });
  run_with_extra("stencil", [&](Pm4Writer& w) { w.set_context_reg(0xa200, 1u << 0); });
  run_with_extra("quad list", [&](Pm4Writer& w) { w.set_uconfig_reg(0xc242, 0x12); });
  run_with_extra("geometry stage", [&](Pm4Writer& w) { w.set_context_reg(0xa2d5, 1u << 5); });
  run_with_extra("instances", [&](Pm4Writer& w) { w.num_instances(2); });
  run_with_extra("other targets", [&](Pm4Writer& w) { w.set_context_reg(0xa08e, 0xF0); });
  run_with_extra("another colour format", [&](Pm4Writer& w) { w.set_context_reg(0xa31c, 0x70738 & ~0x7Cu); });
  run_with_extra("flat shading", [&](Pm4Writer& w) { w.set_context_reg(0xa191, 1u << 10); });
  run_with_extra("no viewport transform", [&](Pm4Writer& w) { w.set_context_reg(0xa206, 0); });
  run_with_extra("perspective centroid", [&](Pm4Writer& w) { w.set_context_reg(0xa1b3, 2 | 0x10); });
  // packets that are not known, or not whole
  const uint32_t unknown[] = {pm4::packet3_header(0x7F, 0), 0};
  { CommandProcessorModel cp(rig.alloc, rig.mem); CHECK(cp.execute(ArrayView<const uint32_t>(unknown, 2)) == CommandProcessorModel::Result::Unsupported); }
  const uint32_t type2[] = {0x80000000u};
  { CommandProcessorModel cp(rig.alloc, rig.mem); CHECK(cp.execute(ArrayView<const uint32_t>(type2, 1)) == CommandProcessorModel::Result::BadPacket); }
  const uint32_t cut[] = {pm4::packet3_header(pm4::kPacket3SetContextReg, 3), 0, 1};
  { CommandProcessorModel cp(rig.alloc, rig.mem); CHECK(cp.execute(ArrayView<const uint32_t>(cut, 3)) == CommandProcessorModel::Result::BadPacket); }
  // shader addresses nothing is mapped at
  { Rig r2; DrawState bad = r2.state(3); bad.pixel.address = 0x7000000; uint32_t b4[512]; bool ok4; const uint32_t m = build(bad, b4, 512, &ok4); r2.set_vertices(kTriangle, 3);
    CommandProcessorModel cp(r2.alloc, r2.mem); CHECK(ok4 && cp.execute(ArrayView<const uint32_t>(b4, m)) == CommandProcessorModel::Result::MemoryFault); }
  // a vertex buffer the descriptors say is larger than the memory
  { Rig r2; r2.set_vertices(kTriangle, 3); uint32_t d[4]; make_vertex_descriptor(ArrayView<uint32_t>(d, 4), kVertices, 28, 100, 3); r2.mem.put(kTable, ArrayView<const uint32_t>(d, 4));
    uint32_t b4[512]; bool ok4; const uint32_t m = build(r2.state(3), b4, 512, &ok4);
    CommandProcessorModel cp(r2.alloc, r2.mem); CHECK(ok4 && cp.execute(ArrayView<const uint32_t>(b4, m)) == CommandProcessorModel::Result::MemoryFault); }
}

// reads a float of the colour target through the layout
static float tiled_pixel(Memory& mem, uint32_t tile_index, uint32_t x, uint32_t y, uint32_t c) {
  TileLayout l;
  resolve_tile_index(tile_index, &l);
  const uint32_t b = mem.words[kTarget + surface_offset(l, 16, kPitch, x, y) + 4 * c];
  float f;
  memcpy(&f, &b, 4);
  return f;
}

// the draw state of the back end test: a 1D tiled colour target, alpha blending, a depth plane (less, write), a stencil plane that counts
static DrawState backend_state(Rig& rig) {
  // 6 vertices: the far layer (z 0.5, red, alpha 0.5) and the near layer (z -0.5, green, alpha 0.5), each a triangle that covers the target
  static const float v[42] = {-1, -1, 0.5f, 1, 0, 0, 0.5f, 3, -1, 0.5f, 1, 0, 0, 0.5f, -1, 3, 0.5f, 1, 0, 0, 0.5f,
                              -1, -1, -0.5f, 0, 1, 0, 0.5f, 3, -1, -0.5f, 0, 1, 0, 0.5f, -1, 3, -0.5f, 0, 1, 0, 0.5f};
  rig.set_vertices(v, 6);
  rig.mem.map(kDepthPlane, kPitch * kPaddedHeight);
  rig.mem.map(kStencilPlane, kPitch * kPaddedHeight / 4);
  DrawState s = rig.state(6);
  s.target.tile_index = 9;
  s.blend.enable = true;
  s.blend.color_src = 4;
  s.blend.color_dst = 5;
  s.depth.z_address = kDepthPlane;
  s.depth.stencil_address = kStencilPlane;
  s.depth.pitch_pixels = kPitch;
  s.depth_stencil.depth_test = true;
  s.depth_stencil.depth_write = true;
  s.depth_stencil.depth_func = 1;
  s.depth_stencil.stencil_test = true;
  s.depth_stencil.stencil_func = 7;
  s.depth_stencil.zpass_op = 5;
  s.depth_stencil.stencil_op_value = 1;
  // the planes hold depth 1.0 and stencil 0 in their layout (1D thin tiles), the colour target (0, 0, 1, 1) in its (1D display tiles)
  TileLayout thin, disp;
  resolve_tile_index(13, &thin);
  resolve_tile_index(9, &disp);
  const float one = 1.0f;
  uint32_t one_bits;
  memcpy(&one_bits, &one, 4);
  for (uint32_t y = 0; y < kPaddedHeight; ++y)
    for (uint32_t x = 0; x < kPitch; ++x) {
      rig.mem.words[kDepthPlane + surface_offset(thin, 4, kPitch, x, y)] = one_bits;
      for (uint32_t c = 0; c < 4; ++c) {
        const float value = c >= 2 ? 1.0f : 0.0f;
        uint32_t b;
        memcpy(&b, &value, 4);
        rig.mem.words[kTarget + surface_offset(disp, 16, kPitch, x, y) + 4 * c] = b;
      }
    }
  for (uint32_t i = 0; i < kPitch * kPaddedHeight / 4; ++i) rig.mem.words[kStencilPlane + 4 * i] = 0;
  return s;
}

// Two layers at different depths drawn through the registers: tiled colour, blending, depth and stencil. The values are worked out by hand.
static void test_backend(Rig& rig) {
  const DrawState s = backend_state(rig);
  uint32_t buf[512];
  bool ok;
  const uint32_t n = build(s, buf, 512, &ok);
  CHECK(ok);
  CommandProcessorModel cp(rig.alloc, rig.mem);
  const auto res = cp.execute(ArrayView<const uint32_t>(buf, n));
  if (res != CommandProcessorModel::Result::Ok) printf("result: %s\n", CommandProcessorModel::result_name(res));
  CHECK(res == CommandProcessorModel::Result::Ok);
  // the registers (kernel offsets): CB_BLEND0_CONTROL 0xa1e0, DB_DEPTH_CONTROL 0xa200, DB_STENCIL_CONTROL 0xa10b, DB_STENCILREFMASK 0xa10c, DB_Z_INFO 0xa010 .. DB_DEPTH_SLICE 0xa017
  CHECK(cp.context_reg(0xa1e0) == ((1u << 30) | 4u | (5u << 8) | (4u << 16) | (5u << 24)));
  CHECK(cp.context_reg(0xa31c) == (0x70738u & ~(1u << pm4::CB_COLOR0_INFO__BLEND_BYPASS__SHIFT)) && cp.context_reg(0xa31d) == 9);
  CHECK(cp.context_reg(0xa200) == ((1u << 0) | (1u << 1) | (1u << 2) | (1u << 4) | (7u << 8) | (7u << 20)));
  CHECK(cp.context_reg(0xa10b) == (5u << 4) && cp.context_reg(0xa10c) == ((0xFFu << 8) | (0xFFu << 16) | (1u << 24)));
  CHECK(cp.context_reg(0xa010) == (3u | (13u << 20)) && cp.context_reg(0xa011) == (1u | (13u << 20) | (1u << 29)));
  CHECK(cp.context_reg(0xa012) == kDepthPlane >> 8 && cp.context_reg(0xa014) == kDepthPlane >> 8 && cp.context_reg(0xa013) == kStencilPlane >> 8 && cp.context_reg(0xa015) == kStencilPlane >> 8);
  CHECK(cp.context_reg(0xa016) == (5u | (3u << 11)) && cp.context_reg(0xa017) == 6 * 4 - 1);
  CHECK(cp.draws() == 1 && cp.last_draw().triangles == 2);
  // by hand: the far layer over blue gives (0.5, 0, 0.5, 0.75), the near one over that (0.25, 0.5, 0.25, 0.625); the depth is 0.25 and the stencil counted twice
  TileLayout thin;
  resolve_tile_index(13, &thin);
  uint32_t wrong = 0;
  const uint32_t points[4][2] = {{0, 0}, {20, 15}, {39, 29}, {7, 22}};
  for (const auto& p : points) {
    const float want[4] = {0.25f, 0.5f, 0.25f, 0.625f};
    for (uint32_t c = 0; c < 4; ++c) if (tiled_pixel(rig.mem, 9, p[0], p[1], c) != want[c]) ++wrong;
    const uint32_t zb = rig.mem.words[kDepthPlane + surface_offset(thin, 4, kPitch, p[0], p[1])];
    float z;
    memcpy(&z, &zb, 4);
    if (z != 0.25f) ++wrong;
    const uint64_t off = surface_offset(thin, 1, kPitch, p[0], p[1]);
    if (((rig.mem.words[kStencilPlane + off / 4 * 4] >> ((off & 3) * 8)) & 0xFF) != 2) ++wrong;
  }
  CHECK(wrong == 0);
  // the pixels outside the target but inside the pitch are as they were: (0, 0, 1, 1), depth 1, stencil 0
  CHECK(tiled_pixel(rig.mem, 9, 44, 3, 0) == 0.0f && tiled_pixel(rig.mem, 9, 44, 3, 2) == 1.0f);
  // the same start and the same stream again: the same result
  const DrawState again = backend_state(rig);
  Pm4Writer w(ArrayView<uint32_t>(buf, 512));
  CHECK(emit_draw(w, again));
  CommandProcessorModel cp2(rig.alloc, rig.mem);
  CHECK(cp2.execute(w.words()) == CommandProcessorModel::Result::Ok);
  CHECK(tiled_pixel(rig.mem, 9, 10, 10, 0) == 0.25f && tiled_pixel(rig.mem, 9, 10, 10, 1) == 0.5f);
}

static void test_texture(Rig& rig) {
  // a 2 by 2 texture (red, green / blue, white) 1D tiled at kTexture; the pixel shader samples it with the T# and S# at the descriptor table's first texture slot
  const uint32_t texels[4] = {0xFF0000FFu, 0xFF00FF00u, 0xFFFF0000u, 0xFFFFFFFFu};
  TileLayout disp;
  CHECK(resolve_tile_index(9, &disp));
  rig.mem.map(kTexture, 64);
  for (uint32_t y = 0; y < 2; ++y)
    for (uint32_t x = 0; x < 2; ++x) rig.mem.words[kTexture + surface_offset(disp, 4, 8, x, y)] = texels[y * 2 + x];
  ImageDescriptor img;
  img.address = kTexture; img.width = 2; img.height = 2; img.pitch_pixels = 8; img.format = TexelFormat::Rgba8Unorm; img.tile_index = 9;
  uint32_t iw[8], sw[4];
  CHECK(make_image_descriptor(ArrayView<uint32_t>(iw, 8), img));
  CHECK(make_sampler_descriptor(ArrayView<uint32_t>(sw, 4), SamplerDescriptor()));
  rig.mem.put(kTable + abi::kImageOffset, ArrayView<const uint32_t>(iw, 8));
  rig.mem.put(kTable + abi::kSamplerOffset, ArrayView<const uint32_t>(sw, 4));
  // one triangle over the target whose colour attribute is (u, v, 0, 1)
  static const float v[21] = {-1, -1, 0, 0, 1, 0, 1, 3, -1, 0, 2, 1, 0, 1, -1, 3, 0, 0, -1, 0, 1};
  rig.set_vertices(v, 3);
  DrawState s = rig.state(3);
  s.pixel.address = kPsAddr2;
  s.pixel.info = rig.tps.info;
  uint32_t buf[512];
  bool ok;
  const uint32_t n = build(s, buf, 512, &ok);
  CHECK(ok);
  rig.clear_target(-1.0f);
  CommandProcessorModel cp(rig.alloc, rig.mem);
  const auto res = cp.execute(ArrayView<const uint32_t>(buf, n));
  if (res != CommandProcessorModel::Result::Ok) printf("result: %s\n", CommandProcessorModel::result_name(res));
  CHECK(res == CommandProcessorModel::Result::Ok);
  auto is = [&](uint32_t x, uint32_t y, float r, float g, float b) { return rig.pixel(x, y, 0) == r && rig.pixel(x, y, 1) == g && rig.pixel(x, y, 2) == b && rig.pixel(x, y, 3) == 1.0f; };
  CHECK(is(5, 5, 1, 0, 0) && is(35, 5, 0, 1, 0) && is(5, 25, 0, 0, 1) && is(35, 25, 1, 1, 1));
  // a descriptor that names memory nothing is mapped at: the draw faults
  img.address = 0x7000000;
  CHECK(make_image_descriptor(ArrayView<uint32_t>(iw, 8), img));
  rig.mem.put(kTable + abi::kImageOffset, ArrayView<const uint32_t>(iw, 8));
  CommandProcessorModel cp2(rig.alloc, rig.mem);
  CHECK(cp2.execute(ArrayView<const uint32_t>(buf, n)) == CommandProcessorModel::Result::ShaderFault);
}

// the same truncations and bit flips for a stream that sets the back end
static void test_damage_backend(Rig& rig) {
  const DrawState s = backend_state(rig);
  uint32_t buf[512];
  bool ok;
  const uint32_t n = build(s, buf, 512, &ok);
  CHECK(ok);
  for (uint32_t cut = 0; cut <= n; ++cut) {
    CommandProcessorModel cp(rig.alloc, rig.mem);
    (void)cp.execute(ArrayView<const uint32_t>(buf, cut));
  }
  uint32_t draws = 0;
  for (uint32_t i = 0; i < n; ++i)
    for (uint32_t bit = 0; bit < 32; ++bit) {
      uint32_t copy[512];
      memcpy(copy, buf, n * 4);
      copy[i] ^= 1u << bit;
      CommandProcessorModel cp(rig.alloc, rig.mem);
      (void)cp.execute(ArrayView<const uint32_t>(copy, n));
      draws += cp.draws();
    }
  CHECK(draws > 0);
}


// Two sided stencil and the depth bounds through the registers
static void test_faces_and_bounds(Rig& rig) {
  for (int variant = 0; variant < 3; ++variant) {
    DrawState s = backend_state(rig);
    s.depth_stencil.zpass_op = 5;
    s.depth_stencil.two_sided = true;
    s.depth_stencil.back.func = 7;
    s.depth_stencil.back.zpass_op = 2;                           // back faces set the stencil to ones
    s.depth_stencil.front_clockwise = variant == 1;              // (the triangles are counter clockwise on the screen)
    if (variant == 2) { s.depth_stencil.depth_bounds = true; s.depth_stencil.bounds_min = 0.0f; s.depth_stencil.bounds_max = 0.5f; }          // the plane holds 1.0: all discarded
    uint32_t buf[512];
    bool ok;
    const uint32_t n = build(s, buf, 512, &ok);
    CHECK(ok);
    CommandProcessorModel cp(rig.alloc, rig.mem);
    const auto res = cp.execute(ArrayView<const uint32_t>(buf, n));
    if (res != CommandProcessorModel::Result::Ok) printf("result: %s\n", CommandProcessorModel::result_name(res));
    CHECK(res == CommandProcessorModel::Result::Ok);
    TileLayout thin;
    resolve_tile_index(13, &thin);
    const uint64_t off = surface_offset(thin, 1, kPitch, 10, 10);
    const uint32_t stencil = (rig.mem.words[kStencilPlane + off / 4 * 4] >> ((off & 3) * 8)) & 0xFF;
    const float blue = tiled_pixel(rig.mem, 9, 10, 10, 2);
    if (variant == 0) CHECK(stencil == 2);                       // front: counted twice
    if (variant == 1) CHECK(stencil == 0xFF);                    // back: ones
    if (variant == 2) CHECK(stencil == 0 && blue == 1.0f);       // discarded by the bounds: stencil untouched, colour still blue
    // the registers: DB_DEPTH_CONTROL bits 7 (back face) and 3 (bounds), the back func at bit 20; PA_SU_SC_MODE_CNTL.FACE bit 2; the back operations at bit 12 ..., DB_STENCILREFMASK_BF
    CHECK((cp.context_reg(0xa200) >> 7 & 1) == 1 && (cp.context_reg(0xa200) >> 20 & 7) == 7 && (cp.context_reg(0xa200) >> 3 & 1) == (variant == 2 ? 1u : 0u));
    CHECK(cp.context_reg(0xa081 + 0) == 0x80000000u && cp.context_reg(0xa205) == (variant == 1 ? 4u : 0u));
    CHECK((cp.context_reg(0xa10b) >> 16 & 0xF) == 2 && cp.context_reg(0xa10d) == ((0xFFu << 8) | (0xFFu << 16) | (1u << 24)));
    CHECK(cp.context_reg(0xa008) == 0 && cp.context_reg(0xa009) == (variant == 2 ? 0x3F000000u : 0x3F800000u));
  }
  // culling and polygon modes are not modelled
  DrawState s = backend_state(rig);
  uint32_t buf[512];
  bool ok;
  const uint32_t n = build(s, buf, 512, &ok);
  Pm4Writer extra(ArrayView<uint32_t>(buf + n, 512 - n));
  extra.set_context_reg(0xa205, 1u << 1);                         // cull back
  extra.draw_index_auto(6);
  CommandProcessorModel cp(rig.alloc, rig.mem);
  CHECK(ok && cp.execute(ArrayView<const uint32_t>(buf, n)) == CommandProcessorModel::Result::Ok);
  CommandProcessorModel cp2(rig.alloc, rig.mem);
  CHECK(cp2.execute(ArrayView<const uint32_t>(buf, n + extra.count())) == CommandProcessorModel::Result::Unsupported);
}

// every prefix of the stream, and every bit of every dword flipped: a result each time, no crash and no sanitizer report
static void test_damage(Rig& rig) {
  rig.set_vertices(kTriangle, 3);
  uint32_t buf[512];
  bool ok;
  DrawState s = rig.state(3);
  s.fence_address = kFence;
  const uint32_t n = build(s, buf, 512, &ok);
  CHECK(ok);
  uint32_t results[6] = {};
  for (uint32_t cut = 0; cut <= n; ++cut) {
    rig.clear_target(-1.0f);
    CommandProcessorModel cp(rig.alloc, rig.mem);
    ++results[static_cast<uint32_t>(cp.execute(ArrayView<const uint32_t>(buf, cut)))];
  }
  CHECK(results[static_cast<uint32_t>(CommandProcessorModel::Result::Ok)] > 1);
  uint32_t draws_after_flips = 0;
  for (uint32_t i = 0; i < n; ++i)
    for (uint32_t bit = 0; bit < 32; ++bit) {
      uint32_t copy[512];
      memcpy(copy, buf, n * 4);
      copy[i] ^= 1u << bit;
      rig.clear_target(-1.0f);
      CommandProcessorModel cp(rig.alloc, rig.mem);
      (void)cp.execute(ArrayView<const uint32_t>(copy, n));
      draws_after_flips += cp.draws();
    }
  CHECK(draws_after_flips > 0);           // many flips only change a value that does not matter
}

int main() {
  Rig rig;
  if (!rig.vs.ok || !rig.ps.ok || !rig.tps.ok) { printf("FAILED: the shaders did not compile\n"); return 1; }
  test_registers(rig);
  { Rig r; test_indexed_and_scissor(r); }
  { Rig r; test_refusals(r); }
  { Rig r; test_damage(r); }
  { Rig r; test_backend(r); }
  { Rig r; test_texture(r); }
  { Rig r; test_faces_and_bounds(r); }
  { Rig r; test_damage_backend(r); }
  printf(g_failed ? "GcnCpTest: %d FAILED\n" : "GcnCpTest: all passed\n", g_failed);
  return g_failed ? 1 : 0;
}
