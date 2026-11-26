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

// Host-side tests of src/Windows/ddraw/d3d_legacy.h, the logic of ddraw.dll's Direct3D 1/2/3/7:
//   - execute buffers: every opcode, the execute data checks, SETSTATUS / BRANCHFORWARD / EXIT, the trivial reject of a branch on the clip status,
//     record sizes, range checks, every truncation, random and bit-flipped buffers (the test runs under ASan and UBSan);
//   - the transform (D3DVIEWPORT and D3DVIEWPORT2 semantics), clip codes, extents and status, TransformVertices, the clipper - against
//     computations written out here in double precision;
//   - the lighting of Direct3D 1-3 (legacy attenuation, parallel point lights, spots, specular, NO_SPECULAR, materials from vertex colours) and of 7,
//     and vertex fog;
//   - caps: structure sizes (as d3dcaps.h has them; tests/data/ddraw/d3d_legacy_layout.c checks them against mingw's headers for i386 and x86-64),
//     the descriptions EnumDevices gives for each device and version, FindDevice;
//   - the state mapping tables, defaults and refusals; lights, materials, viewports, FVF codes, strided data, primitive counts, sphere visibility.
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <vector>
#include "../src/Windows/ddraw/d3d_legacy.h"
#include "data/ddraw/wine_scenes.h"

static int g_checks = 0, g_failed = 0;
#define CHECK(x) \
  do { \
    ++g_checks; \
    if (!(x)) { \
      ++g_failed; \
      printf("FAILED %s:%d: %s\n", __FILE__, __LINE__, #x); \
    } \
  } while (0)

static bool near(double a, double b, double tolerance = 1e-4) { return fabs(a - b) <= tolerance * (1.0 + fabs(b)); }

static unsigned g_seed = 31337;
static unsigned random_number() { g_seed = g_seed * 1664525u + 1013904223u; return g_seed >> 8; }
static float random_float(float lo, float hi) { return lo + (hi - lo) * (float)(random_number() % 100000) / 100000.0f; }

// The sizes d3dcaps.h / d3dtypes.h give on x86-64 (the host); i386 is checked by the mingw build of d3d_legacy_layout.c.
static_assert(sizeof(DlExecuteData) == 48, "D3DEXECUTEDATA");
static_assert(sizeof(DlStatus) == 24, "D3DSTATUS");
static_assert(sizeof(DlExecuteBufferDesc) == 24, "D3DEXECUTEBUFFERDESC (x86-64; 20 on i386)");
static_assert(sizeof(DlTlVertex) == 32 && sizeof(DlVertex) == 32 && sizeof(DlLVertex) == 32, "vertices");
static_assert(sizeof(DlViewport1) == 44 && sizeof(DlViewport2) == 44 && sizeof(DlViewport7) == 24, "viewports");
static_assert(sizeof(DlLight2) == 80 && offsetof(DlLight2, dwFlags) == 76 && sizeof(DlLight7) == 104, "lights");
static_assert(sizeof(DlMaterial) == 80 && sizeof(DlMaterial7) == 68, "materials");
static_assert(sizeof(DlPrimCaps) == 56, "D3DPRIMCAPS");
static_assert(sizeof(DlDeviceDesc) == 252 && offsetof(DlDeviceDesc, dwMinTextureWidth) == 172 && offsetof(DlDeviceDesc, dwMaxTextureRepeat) == 204, "D3DDEVICEDESC");
static_assert(sizeof(DlDeviceDesc7) == 236, "D3DDEVICEDESC7");
static_assert(sizeof(DlFindDeviceSearch) == 92, "D3DFINDDEVICESEARCH");
static_assert(sizeof(DlTransformData) == 80, "D3DTRANSFORMDATA (x86-64; 52 on i386)");
static_assert(sizeof(DlStridedData) == 192, "D3DDRAWPRIMITIVESTRIDEDDATA (x86-64; 96 on i386)");

// ---- an execute buffer builder ---------------------------------------------------------------------------------------------------------------------

struct Buffer {
  std::vector<uint8_t> bytes;
  void u8(uint32_t v) { bytes.push_back((uint8_t)v); }
  void u16(uint32_t v) { u8(v & 0xFF); u8(v >> 8); }
  void u32(uint32_t v) { u16(v & 0xFFFF); u16(v >> 16); }
  void f32(float f) { u32(dl_bits_of(f)); }
  void op(uint32_t opcode, uint32_t size, uint32_t count) { u8(opcode); u8(size); u16(count); }
  size_t size() const { return bytes.size(); }
};

static DlExecuteData execute_data(uint32_t vertex_offset, uint32_t vertex_count, uint32_t ins_offset, uint32_t ins_length) {
  DlExecuteData d;
  memset(&d, 0, sizeof d);
  d.dwSize = 48;
  d.dwVertexOffset = vertex_offset;
  d.dwVertexCount = vertex_count;
  d.dwInstructionOffset = ins_offset;
  d.dwInstructionLength = ins_length;
  d.dsStatus.dwStatus = DL_STATUS_DEFAULT;
  return d;
}

// Everything the executor gives for a buffer, as the device would read it.
struct Decoded {
  std::vector<DlCommand> commands;
  std::vector<int> record_results;
  int last = 0;
  DlResult error = 0;
};
static Decoded run(const std::vector<uint8_t>& bytes, const DlExecuteData& data, DlStatus* status) {
  Decoded out;
  DlExecutor x;
  if (dl_execute_begin(&x, dl_const_bytes(bytes.data(), bytes.size()), &data, status) != DL_OK) {
    out.last = -2;
    return out;
  }
  DlCommand c;
  int r, steps = 0;
  while ((r = dl_execute_next(&x, &c)) == DL_NEXT_COMMAND) {
    out.commands.push_back(c);
    for (uint32_t i = 0; i < c.count; ++i) {
      DlPoint p; DlLine l; DlTriangle t; DlMatrixLoad ml; DlMatrixMultiply mm; DlState s; DlProcessVertices pv; DlTextureLoad tl; DlSpan sp;
      int res = DL_BAD;
      switch (c.opcode) {
      case DL_OP_POINT: res = dl_command_point(&x, &c, i, &p); break;
      case DL_OP_LINE: res = dl_command_line(&x, &c, i, &l); break;
      case DL_OP_TRIANGLE: res = dl_command_triangle(&x, &c, i, &t); break;
      case DL_OP_MATRIXLOAD: res = dl_command_matrix_load(&x, &c, i, &ml); break;
      case DL_OP_MATRIXMULTIPLY: res = dl_command_matrix_multiply(&x, &c, i, &mm); break;
      case DL_OP_STATETRANSFORM: case DL_OP_STATELIGHT: case DL_OP_STATERENDER: res = dl_command_state(&x, &c, i, &s); break;
      case DL_OP_PROCESSVERTICES: res = dl_command_process_vertices(&x, &c, i, &pv); break;
      case DL_OP_TEXTURELOAD: res = dl_command_texture_load(&x, &c, i, &tl); break;
      case DL_OP_SPAN: res = dl_command_span(&x, &c, i, &sp); break;
      }
      out.record_results.push_back(res);
    }
    if (++steps > 100000) break;
  }
  CHECK(steps <= 100000);
  out.last = r;
  out.error = x.error;
  return out;
}

static void test_execute_opcodes() {
  // One of every instruction, then EXIT and something after it that must not be seen.
  Buffer b;
  for (int i = 0; i < 4 * 32; ++i) b.u8(0);      // (four vertices' room first)
  const uint32_t ins = (uint32_t)b.size();
  b.op(DL_OP_POINT, 4, 2); b.u16(2); b.u16(0); b.u16(1); b.u16(3);
  b.op(DL_OP_LINE, 4, 1); b.u16(0); b.u16(3);
  b.op(DL_OP_TRIANGLE, 8, 2);
  b.u16(0); b.u16(1); b.u16(2); b.u16(DL_TRIFLAG_START | DL_TRIFLAG_EDGEENABLE1 | DL_TRIFLAG_EDGEENABLE3);
  b.u16(1); b.u16(2); b.u16(3); b.u16(DL_TRIFLAG_ODD | DL_TRIFLAG_EDGEENABLE2);
  b.op(DL_OP_MATRIXLOAD, 8, 1); b.u32(5); b.u32(6);
  b.op(DL_OP_MATRIXMULTIPLY, 12, 1); b.u32(1); b.u32(2); b.u32(3);
  b.op(DL_OP_STATETRANSFORM, 8, 1); b.u32(DL_TRANSFORMSTATE_WORLD); b.u32(1);
  b.op(DL_OP_STATELIGHT, 8, 1); b.u32(DL_LIGHTSTATE_AMBIENT); b.u32(0xFF404040u);
  b.op(DL_OP_STATERENDER, 8, 2); b.u32(DL_RS_TEXTUREMAPBLEND); b.u32(DL_TBLEND_MODULATE); b.u32(DL_RS_ZBIAS); b.u32(3);
  b.op(DL_OP_PROCESSVERTICES, 16, 1); b.u32(DL_PROCESSVERTICES_TRANSFORMLIGHT | DL_PROCESSVERTICES_UPDATEEXTENTS); b.u16(0); b.u16(0); b.u32(4); b.u32(0);
  b.op(DL_OP_TEXTURELOAD, 8, 1); b.u32(7); b.u32(8);
  b.op(DL_OP_SPAN, 4, 1); b.u16(3); b.u16(1);
  b.op(DL_OP_SETSTATUS, 24, 1); b.u32(DL_SETSTATUS_ALL); b.u32(0x1234); b.u32(1); b.u32(2); b.u32(3); b.u32(4);
  b.op(DL_OP_EXIT, 0, 0);
  b.op(DL_OP_POINT, 4, 1); b.u16(1); b.u16(0);      // (after EXIT)
  const DlExecuteData d = execute_data(0, 4, ins, (uint32_t)b.size() - ins);
  DlStatus status = d.dsStatus;
  DlExecutor x;
  CHECK(dl_execute_begin(&x, dl_const_bytes(b.bytes.data(), b.size()), &d, &status) == DL_OK);
  DlCommand c;
  CHECK(dl_execute_next(&x, &c) == DL_NEXT_COMMAND && c.opcode == DL_OP_POINT && c.count == 2 && c.size == 4 && c.header == ins);
  DlPoint p;
  CHECK(dl_command_point(&x, &c, 0, &p) == DL_OK && p.wCount == 2 && p.wFirst == 0);
  CHECK(dl_command_point(&x, &c, 1, &p) == DL_OK && p.wCount == 1 && p.wFirst == 3);
  CHECK(dl_command_point(&x, &c, 2, &p) == DL_BAD);
  DlLine l;
  CHECK(dl_command_line(&x, &c, 0, &l) == DL_BAD);      // (a command of another kind)
  CHECK(dl_execute_next(&x, &c) == DL_NEXT_COMMAND && c.opcode == DL_OP_LINE);
  CHECK(dl_command_line(&x, &c, 0, &l) == DL_OK && l.v1 == 0 && l.v2 == 3);
  CHECK(dl_execute_next(&x, &c) == DL_NEXT_COMMAND && c.opcode == DL_OP_TRIANGLE && c.count == 2);
  DlTriangle t;
  CHECK(dl_command_triangle(&x, &c, 0, &t) == DL_OK && t.v1 == 0 && t.v2 == 1 && t.v3 == 2 && t.wFlags == (DL_TRIFLAG_EDGEENABLE1 | DL_TRIFLAG_EDGEENABLE3));
  uint8_t pairs[6];
  CHECK(dl_triangle_edges(t.wFlags, pairs) == 2 && pairs[0] == 0 && pairs[1] == 1 && pairs[2] == 2 && pairs[3] == 0);
  CHECK(dl_command_triangle(&x, &c, 1, &t) == DL_OK && t.v1 == 1 && t.v3 == 3 && (t.wFlags & 0x1F) == DL_TRIFLAG_ODD);
  CHECK(dl_triangle_edges(t.wFlags, pairs) == 1 && pairs[0] == 1 && pairs[1] == 2);
  CHECK(dl_triangle_edges(0x700, pairs) == 3 && dl_triangle_edges(0, pairs) == 0);
  CHECK(dl_execute_next(&x, &c) == DL_NEXT_COMMAND && c.opcode == DL_OP_MATRIXLOAD);
  DlMatrixLoad ml;
  CHECK(dl_command_matrix_load(&x, &c, 0, &ml) == DL_OK && ml.hDestMatrix == 5 && ml.hSrcMatrix == 6);
  CHECK(dl_execute_next(&x, &c) == DL_NEXT_COMMAND && c.opcode == DL_OP_MATRIXMULTIPLY);
  DlMatrixMultiply mm;
  CHECK(dl_command_matrix_multiply(&x, &c, 0, &mm) == DL_OK && mm.hDestMatrix == 1 && mm.hSrcMatrix1 == 2 && mm.hSrcMatrix2 == 3);
  DlState s;
  CHECK(dl_execute_next(&x, &c) == DL_NEXT_COMMAND && c.opcode == DL_OP_STATETRANSFORM);
  CHECK(dl_command_state(&x, &c, 0, &s) == DL_OK && s.type == DL_TRANSFORMSTATE_WORLD && s.arg == 1);
  CHECK(dl_execute_next(&x, &c) == DL_NEXT_COMMAND && c.opcode == DL_OP_STATELIGHT);
  CHECK(dl_command_state(&x, &c, 0, &s) == DL_OK && s.type == DL_LIGHTSTATE_AMBIENT && s.arg == 0xFF404040u);
  CHECK(dl_execute_next(&x, &c) == DL_NEXT_COMMAND && c.opcode == DL_OP_STATERENDER && c.count == 2);
  CHECK(dl_command_state(&x, &c, 1, &s) == DL_OK && s.type == DL_RS_ZBIAS && s.arg == 3);
  CHECK(dl_execute_next(&x, &c) == DL_NEXT_COMMAND && c.opcode == DL_OP_PROCESSVERTICES);
  DlProcessVertices pv;
  CHECK(dl_command_process_vertices(&x, &c, 0, &pv) == DL_OK && pv.dwCount == 4 && pv.wStart == 0 && pv.wDest == 0 &&
        pv.dwFlags == (DL_PROCESSVERTICES_TRANSFORMLIGHT | DL_PROCESSVERTICES_UPDATEEXTENTS));
  CHECK(dl_execute_next(&x, &c) == DL_NEXT_COMMAND && c.opcode == DL_OP_TEXTURELOAD);
  DlTextureLoad tl;
  CHECK(dl_command_texture_load(&x, &c, 0, &tl) == DL_OK && tl.hDestTexture == 7 && tl.hSrcTexture == 8);
  CHECK(dl_execute_next(&x, &c) == DL_NEXT_COMMAND && c.opcode == DL_OP_SPAN);
  DlSpan sp;
  CHECK(dl_command_span(&x, &c, 0, &sp) == DL_OK && sp.wCount == 3 && sp.wFirst == 1);
  // SETSTATUS is done inside; EXIT ends it and what follows is not seen.
  CHECK(dl_execute_next(&x, &c) == DL_NEXT_END);
  CHECK(status.dwStatus == 0x1234 && status.drExtent.x1 == 1 && status.drExtent.y1 == 2 && status.drExtent.x2 == 3 && status.drExtent.y2 == 4);
  CHECK(dl_execute_next(&x, &c) == DL_NEXT_END && x.error == 0);
}

static void test_execute_status_and_branches() {
  // SETSTATUS with only one of its flags.
  {
    Buffer b;
    b.op(DL_OP_SETSTATUS, 24, 2);
    b.u32(DL_SETSTATUS_EXTENTS); b.u32(0x77); b.u32(10); b.u32(20); b.u32(30); b.u32(40);
    b.u32(DL_SETSTATUS_STATUS); b.u32(0x55); b.u32(1); b.u32(1); b.u32(1); b.u32(1);
    DlExecuteData d = execute_data(0, 0, 0, (uint32_t)b.size());
    DlStatus status = d.dsStatus;
    const Decoded r = run(b.bytes, d, &status);
    CHECK(r.last == DL_NEXT_END && r.commands.empty());
    CHECK(status.dwStatus == 0x55 && status.drExtent.x1 == 10 && status.drExtent.y2 == 40);
  }
  // BRANCHFORWARD: (status & mask) == value branches; negated, it does not; offset 0 ends; the records in turn; past the end is an error.
  auto branch_buffer = [](uint32_t mask, uint32_t value, uint32_t negate, uint32_t offset, int records = 1) {
    Buffer b;
    b.op(DL_OP_BRANCHFORWARD, 16, (uint32_t)records);
    for (int i = 0; i < records - 1; ++i) { b.u32(0xFFFFFFFFu); b.u32(0xDEAD); b.u32(0); b.u32(4); }   // (never taken)
    b.u32(mask); b.u32(value); b.u32(negate); b.u32(offset);
    b.op(DL_OP_POINT, 4, 1); b.u16(1); b.u16(0);          // skipped by a branch of 4 + 16 * records + 8
    b.op(DL_OP_LINE, 4, 1); b.u16(0); b.u16(0);
    return b;
  };
  for (int records = 1; records <= 3; ++records) {
    const uint32_t over_point = 4 + 16 * records + 8;
    {
      Buffer b = branch_buffer(0xF0, 0x30, 0, over_point, records);
      DlExecuteData d = execute_data(0, 1, 0, (uint32_t)b.size());
      DlStatus status = d.dsStatus;
      status.dwStatus = 0x3F;
      const Decoded r = run(b.bytes, d, &status);
      CHECK(r.last == DL_NEXT_END && r.commands.size() == 1 && r.commands[0].opcode == DL_OP_LINE);
    }
    {
      Buffer b = branch_buffer(0xF0, 0x30, 1, over_point, records);      // (negated: not taken)
      DlExecuteData d = execute_data(0, 1, 0, (uint32_t)b.size());
      DlStatus status = d.dsStatus;
      status.dwStatus = 0x3F;
      const Decoded r = run(b.bytes, d, &status);
      CHECK(r.commands.size() == 2 && r.commands[0].opcode == DL_OP_POINT);
    }
    {
      Buffer b = branch_buffer(0xF0, 0x40, 1, over_point, records);      // (not equal, negated: taken)
      DlExecuteData d = execute_data(0, 1, 0, (uint32_t)b.size());
      DlStatus status = d.dsStatus;
      status.dwStatus = 0x3F;
      const Decoded r = run(b.bytes, d, &status);
      CHECK(r.commands.size() == 1 && r.commands[0].opcode == DL_OP_LINE);
    }
    {
      Buffer b = branch_buffer(0, 0, 0, 0, records);      // (taken, offset 0: the end)
      DlExecuteData d = execute_data(0, 1, 0, (uint32_t)b.size());
      DlStatus status = d.dsStatus;
      const Decoded r = run(b.bytes, d, &status);
      CHECK(r.last == DL_NEXT_END && r.commands.empty());
    }
    {
      Buffer b = branch_buffer(0, 0, 0, 4096, records);   // (past the end)
      DlExecuteData d = execute_data(0, 1, 0, (uint32_t)b.size());
      DlStatus status = d.dsStatus;
      const Decoded r = run(b.bytes, d, &status);
      CHECK(r.last == DL_NEXT_ERROR && r.error == DL_D3DERR_EXECUTE_FAILED && r.commands.empty());
    }
    {
      Buffer b = branch_buffer(0, 0, 0, (uint32_t)branch_buffer(0, 0, 0, 0, records).size(), records);   // (exactly to the end: done)
      DlExecuteData d = execute_data(0, 1, 0, (uint32_t)b.size());
      DlStatus status = d.dsStatus;
      const Decoded r = run(b.bytes, d, &status);
      CHECK(r.last == DL_NEXT_END && r.commands.empty());
    }
  }
}

// The classic trivial reject: the status is reset, vertices are processed, and the triangles are skipped if every vertex is off one side.
static void test_execute_trivial_reject() {
  for (int offscreen = 0; offscreen <= 1; ++offscreen) {
    Buffer b;
    const float xs[3] = {-0.5f, 0.5f, 0.0f};
    for (int i = 0; i < 3; ++i) {      // D3DVERTEX: x y z, normal, tu tv
      b.f32(offscreen ? xs[i] - 10.0f : xs[i]); b.f32(0.25f * (float)i); b.f32(0.5f);
      b.f32(0); b.f32(0); b.f32(-1); b.f32(0); b.f32(0);
    }
    const uint32_t ins = (uint32_t)b.size();
    b.op(DL_OP_SETSTATUS, 24, 1); b.u32(DL_SETSTATUS_ALL); b.u32(DL_STATUS_DEFAULT); b.u32(2048); b.u32(2048); b.u32(0); b.u32(0);
    b.op(DL_OP_PROCESSVERTICES, 16, 1); b.u32(DL_PROCESSVERTICES_TRANSFORMLIGHT | DL_PROCESSVERTICES_UPDATEEXTENTS); b.u16(0); b.u16(0); b.u32(3); b.u32(0);
    const uint32_t branch_at = (uint32_t)b.size() - ins;
    b.op(DL_OP_BRANCHFORWARD, 16, 1); b.u32(DL_STATUS_CLIPINTERSECTIONALL); b.u32(0); b.u32(1); b.u32(4 + 16 + 12);
    b.op(DL_OP_TRIANGLE, 8, 1); b.u16(0); b.u16(1); b.u16(2); b.u16(0x700);
    b.op(DL_OP_EXIT, 0, 0);
    (void)branch_at;
    DlExecuteData d = execute_data(0, 3, ins, (uint32_t)b.size() - ins);
    DlStatus status = d.dsStatus;
    DlTransform t;
    dl_transform_init(&t);
    DlViewport1 vp = {44, 0, 0, 640, 480, 320.0f, 240.0f, 1.0f, 1.0f, 0.0f, 1.0f};
    dl_viewport7_from_v1(&vp, &t.viewport, &t.clip);
    dl_transform_prepare(&t);
    DlLighting light;
    dl_lighting_init(&light, 1);
    DlExecutor x;
    CHECK(dl_execute_begin(&x, dl_const_bytes(b.bytes.data(), b.size()), &d, &status) == DL_OK);
    std::vector<uint8_t> tl(3 * 32);
    std::vector<DlClipVertex> clip(3);
    DlCommand c;
    int triangles = 0;
    while (dl_execute_next(&x, &c) == DL_NEXT_COMMAND) {
      if (c.opcode == DL_OP_PROCESSVERTICES) {
        DlProcessVertices pv;
        CHECK(dl_command_process_vertices(&x, &c, 0, &pv) == DL_OK);
        CHECK(dl_execute_process(&pv, dl_const_bytes(b.bytes.data(), b.size()), &d, dl_bytes(tl.data(), tl.size()), clip.data(), clip.size(), &t, &light,
                                 &status) == DL_OK);
      }
      if (c.opcode == DL_OP_TRIANGLE) ++triangles;
    }
    CHECK(triangles == (offscreen ? 0 : 1));
    if (offscreen) {
      CHECK((status.dwStatus & DL_STATUS_CLIPINTERSECTIONALL) == (DL_CLIP_LEFT << 12) && (status.dwStatus & DL_CLIP_LEFT));
      CHECK(status.drExtent.x1 == 2048 && status.drExtent.x2 == 0);      // (nothing on the screen: the extents are as they were)
    } else {
      CHECK((status.dwStatus & DL_STATUS_CLIPINTERSECTIONALL) == 0 && (status.dwStatus & DL_STATUS_CLIPUNIONALL) == 0);
      CHECK(status.dwStatus & DL_STATUS_ZNOTVISIBLE);
      // x = -0.5 .. 0.5 at scale 320: 160 .. 480; y = 0 .. 0.5 at 240: 120 .. 240
      CHECK(status.drExtent.x1 == 160 && status.drExtent.x2 == 480 && status.drExtent.y1 == 120 && status.drExtent.y2 == 240);
      DlTlVertex v;
      memcpy(&v, tl.data() + 32, sizeof v);
      CHECK(near(v.sx, 480.0) && near(v.sy, 240.0 - 0.25 * 240.0) && near(v.sz, 0.5) && near(v.rhw, 1.0));
      CHECK(v.color == 0xFFFFFFFFu && v.specular == 0xFF000000u);      // (no material: not lit)
    }
  }
}

static void test_execute_refusals() {
  Buffer b;
  b.op(DL_OP_TRIANGLE, 8, 1); b.u16(0); b.u16(1); b.u16(2); b.u16(0);
  DlStatus status;
  memset(&status, 0, sizeof status);
  DlExecutor x;
  // The execute data: its size, instructions and vertices within the buffer.
  DlExecuteData d = execute_data(0, 0, 0, (uint32_t)b.size());
  CHECK(dl_execute_data_check(&d, (uint32_t)b.size()) == DL_OK);
  d.dwSize = 44;
  CHECK(dl_execute_data_check(&d, (uint32_t)b.size()) == DL_DDERR_INVALIDPARAMS);
  CHECK(dl_execute_data_check(nullptr, 100) == DL_DDERR_INVALIDPARAMS);
  d = execute_data(0, 0, 4, (uint32_t)b.size());
  CHECK(dl_execute_data_check(&d, (uint32_t)b.size()) == DL_DDERR_INVALIDPARAMS);
  d = execute_data(0, 0, 0xFFFFFFF0u, 0x20);      // (wraps)
  CHECK(dl_execute_data_check(&d, (uint32_t)b.size()) == DL_DDERR_INVALIDPARAMS);
  d = execute_data(0, 1, 0, (uint32_t)b.size());      // (a vertex of 32 bytes in 12)
  CHECK(dl_execute_data_check(&d, (uint32_t)b.size()) == DL_DDERR_INVALIDPARAMS);
  d = execute_data(0xFFFFFFFFu, 0, 0, 0);
  CHECK(dl_execute_data_check(&d, 16) == DL_DDERR_INVALIDPARAMS);
  d = execute_data(16, 0x08000000u, 0, 0);      // (count * 32 wraps)
  CHECK(dl_execute_data_check(&d, 64) == DL_DDERR_INVALIDPARAMS);
  d = execute_data(0, 0, 0, (uint32_t)b.size());
  CHECK(dl_execute_begin(&x, dl_const_bytes(b.bytes.data(), b.size()), &d, nullptr) == DL_DDERR_INVALIDPARAMS);
  // Indices beyond the vertices: the record is in range DL_RANGE, the stream goes on.
  {
    std::vector<uint8_t> bytes(4 * 32, 0);
    Buffer i;
    i.op(DL_OP_TRIANGLE, 8, 2); i.u16(0); i.u16(1); i.u16(4); i.u16(0); i.u16(0); i.u16(1); i.u16(3); i.u16(0);
    i.op(DL_OP_LINE, 4, 1); i.u16(9); i.u16(0);
    i.op(DL_OP_POINT, 4, 2); i.u16(2); i.u16(3); i.u16(4); i.u16(0);
    i.op(DL_OP_PROCESSVERTICES, 16, 3);
    i.u32(0); i.u16(2); i.u16(0); i.u32(3); i.u32(0);
    i.u32(0); i.u16(0); i.u16(2); i.u32(3); i.u32(0);
    i.u32(0); i.u16(0); i.u16(0); i.u32(0xFFFFFFFFu); i.u32(0);
    i.op(DL_OP_SPAN, 4, 1); i.u16(5); i.u16(0);
    bytes.insert(bytes.end(), i.bytes.begin(), i.bytes.end());
    DlExecuteData dd = execute_data(0, 4, 128, (uint32_t)i.size());
    const Decoded r = run(bytes, dd, &status);
    CHECK(r.last == DL_NEXT_END && r.commands.size() == 5);
    const int expected[] = {DL_RANGE, DL_OK, DL_RANGE, DL_RANGE, DL_OK, DL_RANGE, DL_RANGE, DL_RANGE, DL_RANGE};
    CHECK(r.record_results.size() == 9);
    for (size_t k = 0; k < r.record_results.size() && k < 9; ++k) CHECK(r.record_results[k] == expected[k]);
  }
  // Records smaller than their structure.
  const uint32_t small[][2] = {{DL_OP_POINT, 3}, {DL_OP_TRIANGLE, 6}, {DL_OP_MATRIXMULTIPLY, 8}, {DL_OP_STATERENDER, 4}, {DL_OP_PROCESSVERTICES, 12},
                               {DL_OP_BRANCHFORWARD, 12}, {DL_OP_SETSTATUS, 20}, {DL_OP_TEXTURELOAD, 4}, {DL_OP_SPAN, 2}, {DL_OP_MATRIXLOAD, 4}};
  for (const auto& sm : small) {
    Buffer s;
    s.op(sm[0], sm[1], 1);
    for (uint32_t k = 0; k < sm[1]; ++k) s.u8(0);
    DlExecuteData dd = execute_data(0, 0, 0, (uint32_t)s.size());
    const Decoded r = run(s.bytes, dd, &status);
    CHECK(r.last == DL_NEXT_ERROR && r.error == DL_D3DERR_EXECUTE_FAILED);
  }
  // Larger records than the structure are stepped over by their size; unknown opcodes skipped; empty instructions skipped.
  {
    Buffer s;
    s.op(DL_OP_LINE, 12, 2); s.u16(0); s.u16(1); s.u32(0xAAAAAAAAu); s.u32(0xBBBBBBBBu); s.u16(1); s.u16(0); s.u32(0); s.u32(0);
    s.op(0, 4, 1); s.u32(0);
    s.op(200, 8, 2); s.u32(1); s.u32(2); s.u32(3); s.u32(4);
    s.op(DL_OP_TRIANGLE, 8, 0);
    s.op(DL_OP_POINT, 0, 0);
    s.op(DL_OP_LINE, 4, 1); s.u16(1); s.u16(1);
    std::vector<uint8_t> bytes(2 * 32, 0);
    bytes.insert(bytes.end(), s.bytes.begin(), s.bytes.end());
    DlExecuteData dd = execute_data(0, 2, 64, (uint32_t)s.size());
    DlExecutor e;
    CHECK(dl_execute_begin(&e, dl_const_bytes(bytes.data(), bytes.size()), &dd, &status) == DL_OK);
    DlCommand c;
    DlLine l;
    CHECK(dl_execute_next(&e, &c) == DL_NEXT_COMMAND && c.opcode == DL_OP_LINE && c.size == 12);
    CHECK(dl_command_line(&e, &c, 1, &l) == DL_OK && l.v1 == 1 && l.v2 == 0);
    CHECK(dl_execute_next(&e, &c) == DL_NEXT_COMMAND && c.opcode == DL_OP_LINE && c.count == 1);
    CHECK(dl_command_line(&e, &c, 0, &l) == DL_OK && l.v1 == 1 && l.v2 == 1);
    CHECK(dl_execute_next(&e, &c) == DL_NEXT_END);      // (no EXIT: the end of the area ends it)
  }
  // Every shortened instruction area of a valid stream: no read past it, and an error or an end.
  {
    Buffer s;
    s.op(DL_OP_STATERENDER, 8, 2); s.u32(7); s.u32(1); s.u32(8); s.u32(3);
    s.op(DL_OP_TRIANGLE, 8, 1); s.u16(0); s.u16(1); s.u16(2); s.u16(0);
    s.op(DL_OP_SETSTATUS, 24, 1); for (int k = 0; k < 6; ++k) s.u32(1);
    s.op(DL_OP_EXIT, 0, 0);
    for (size_t length = 0; length <= s.size(); ++length) {
      std::vector<uint8_t> bytes(s.bytes.begin(), s.bytes.begin() + (long)length);
      std::vector<uint8_t> vertices(3 * 32, 0);
      bytes.insert(bytes.begin(), vertices.begin(), vertices.end());
      DlExecuteData dd = execute_data(0, 3, 96, (uint32_t)length);
      const Decoded r = run(bytes, dd, &status);
      const bool whole_instructions = length == 0 || length == 20 || length == 32 || length == 60 || length == 64;
      CHECK(r.last == (whole_instructions ? DL_NEXT_END : DL_NEXT_ERROR));
    }
  }
}

// Random and bit-flipped buffers: the executor and every accessor and the vertex processing must stay inside them (ASan) and stop.
static void test_execute_hostile() {
  DlTransform t;
  dl_transform_init(&t);
  dl_transform_prepare(&t);
  DlLighting light;
  dl_lighting_init(&light, 1);
  light.enabled = 1;
  light.lights = 1;
  light.light[0].type = DL_LIGHT_POINT;
  light.light[0].range = 10.0f;
  light.light[0].attenuation[1] = 1.0f;
  Buffer base;
  for (int i = 0; i < 4; ++i)
    for (int k = 0; k < 8; ++k) base.f32((float)(i + k) * 0.1f);
  const uint32_t ins = (uint32_t)base.size();
  base.op(DL_OP_SETSTATUS, 24, 1); base.u32(3); base.u32(DL_STATUS_DEFAULT); base.u32(2048); base.u32(2048); base.u32(0); base.u32(0);
  base.op(DL_OP_PROCESSVERTICES, 16, 2); base.u32(0); base.u16(0); base.u16(0); base.u32(4); base.u32(0); base.u32(1); base.u16(1); base.u16(0); base.u32(2); base.u32(0);
  base.op(DL_OP_BRANCHFORWARD, 16, 1); base.u32(DL_STATUS_CLIPINTERSECTIONALL); base.u32(0); base.u32(1); base.u32(32);
  base.op(DL_OP_TRIANGLE, 8, 2); base.u16(0); base.u16(1); base.u16(2); base.u16(0); base.u16(1); base.u16(2); base.u16(3); base.u16(0x700);
  base.op(DL_OP_LINE, 4, 1); base.u16(3); base.u16(0);
  base.op(DL_OP_STATERENDER, 8, 1); base.u32(21); base.u32(2);
  base.op(DL_OP_MATRIXMULTIPLY, 12, 1); base.u32(1); base.u32(2); base.u32(3);
  base.op(DL_OP_PROCESSVERTICES, 16, 1); base.u32(2 | 8 | 16); base.u16(0); base.u16(0); base.u32(4); base.u32(0);
  base.op(DL_OP_EXIT, 0, 0);
  auto exercise = [&](std::vector<uint8_t>& bytes, const DlExecuteData& d) {
    DlStatus status = d.dsStatus;
    DlExecutor x;
    if (dl_execute_begin(&x, dl_const_bytes(bytes.data(), bytes.size()), &d, &status) != DL_OK) return;
    std::vector<uint8_t> dst(4 * 32, 0);
    std::vector<DlClipVertex> clip(4);
    DlCommand c;
    int steps = 0;
    while (dl_execute_next(&x, &c) == DL_NEXT_COMMAND && ++steps < 70000) {
      for (uint32_t i = 0; i < c.count && i < 4096; ++i) {
        DlProcessVertices pv;
        DlTriangle tr;
        DlState st;
        if (c.opcode == DL_OP_PROCESSVERTICES && dl_command_process_vertices(&x, &c, i, &pv) == DL_OK)
          dl_execute_process(&pv, dl_const_bytes(bytes.data(), bytes.size()), &d, dl_bytes(dst.data(), dst.size()), clip.data(), clip.size(), &t, &light, &status);
        if (c.opcode == DL_OP_TRIANGLE && dl_command_triangle(&x, &c, i, &tr) == DL_OK) {
          DlClipVertex poly[DL_CLIP_MAX] = {clip[tr.v1], clip[tr.v2], clip[tr.v3]};
          const unsigned n = dl_clip_polygon(poly, 3, DL_CLIP_ALL);
          CHECK(n == 0 || (n >= 3 && n <= DL_CLIP_MAX));
        }
        if (c.opcode == DL_OP_STATERENDER && dl_command_state(&x, &c, i, &st) == DL_OK) {
          DlStateOps ops;
          DlStateContext ctx = {0, 0, 2, 24};
          dl_render_state(2, st.type, st.arg, &ctx, &ops);
          CHECK(ops.count <= DL_MAX_STATE_OPS);
        }
      }
    }
    CHECK(steps < 70000);
  };
  // Every bit of the instructions turned, one at a time.
  for (size_t bit = 0; bit < (base.size() - ins) * 8; ++bit) {
    std::vector<uint8_t> bytes = base.bytes;
    bytes[ins + bit / 8] ^= (uint8_t)(1u << (bit % 8));
    exercise(bytes, execute_data(0, 4, ins, (uint32_t)(base.size() - ins)));
  }
  // Random bytes and random execute data.
  for (int round = 0; round < 3000; ++round) {
    std::vector<uint8_t> bytes(random_number() % 400);
    for (auto& v : bytes) v = (uint8_t)random_number();
    if (round % 3 == 0)
      for (size_t k = 0; k + 4 <= bytes.size(); k += 4) bytes[k] = (uint8_t)(1 + random_number() % 14);      // (mostly real opcodes)
    const uint32_t size = (uint32_t)bytes.size();
    const uint32_t voff = size ? random_number() % size : 0, ioff = size ? random_number() % size : 0;
    const uint32_t vmax = size > voff ? (size - voff) / 32 : 0;
    DlExecuteData d = execute_data(voff, vmax ? random_number() % (vmax + 1) : 0, ioff, size - ioff);
    if (round % 7 == 0) d.dwInstructionLength = random_number();
    exercise(bytes, d);
  }
}

// ---- the transform ------------------------------------------------------------------------------------------------------------------------------

struct Mat { double m[4][4]; };
static Mat to_mat(const DlMatrix& a) { Mat r; for (int i = 0; i < 4; ++i) for (int j = 0; j < 4; ++j) r.m[i][j] = a.m[i][j]; return r; }
static Mat mul(const Mat& a, const Mat& b) {
  Mat r;
  for (int i = 0; i < 4; ++i)
    for (int j = 0; j < 4; ++j) { r.m[i][j] = 0; for (int k = 0; k < 4; ++k) r.m[i][j] += a.m[i][k] * b.m[k][j]; }
  return r;
}
static void apply(const Mat& m, const double p[3], double out[4]) {
  for (int c = 0; c < 4; ++c) out[c] = p[0] * m.m[0][c] + p[1] * m.m[1][c] + p[2] * m.m[2][c] + m.m[3][c];
}
static DlMatrix perspective(float zn, float zf, float fov_scale) {
  DlMatrix m;
  memset(&m, 0, sizeof m);
  m.m[0][0] = fov_scale;
  m.m[1][1] = fov_scale;
  m.m[2][2] = zf / (zf - zn);
  m.m[2][3] = 1.0f;
  m.m[3][2] = -zn * zf / (zf - zn);
  return m;
}
static DlMatrix random_affine() {
  DlMatrix m;
  dl_matrix_identity(&m);
  for (int i = 0; i < 3; ++i)
    for (int j = 0; j < 3; ++j) m.m[i][j] = (i == j ? 1.0f : 0.0f) + random_float(-0.3f, 0.3f);
  for (int j = 0; j < 3; ++j) m.m[3][j] = random_float(-2.0f, 2.0f);
  return m;
}

static void test_matrices() {
  for (int round = 0; round < 200; ++round) {
    DlMatrix a = random_affine(), b = perspective(0.5f, 50.0f, random_float(0.5f, 2.0f)), c, inv, id;
    dl_matrix_multiply(&c, &a, &b);
    const Mat r = mul(to_mat(a), to_mat(b));
    bool same = true;
    for (int i = 0; i < 4; ++i)
      for (int j = 0; j < 4; ++j) same = same && near(c.m[i][j], r.m[i][j], 1e-5);
    CHECK(same);
    CHECK(dl_matrix_invert(&inv, &a) == 1);
    dl_matrix_multiply(&id, &a, &inv);
    bool identity = true;
    for (int i = 0; i < 4; ++i)
      for (int j = 0; j < 4; ++j) identity = identity && fabs(id.m[i][j] - (i == j ? 1.0 : 0.0)) < 1e-4;
    CHECK(identity);
    dl_matrix_multiply(&a, &a, &b);      // (in place)
    CHECK(near(a.m[3][2], r.m[3][2], 1e-5));
  }
  DlMatrix zero, out;
  memset(&zero, 0, sizeof zero);
  CHECK(dl_matrix_invert(&out, &zero) == 0 && out.m[0][0] == 1.0f && out.m[0][1] == 0.0f);
}

static void test_viewports_and_transform() {
  // D3DVIEWPORT: screen x = X + W/2 + dvScaleX * x/w, y = Y + H/2 - dvScaleY * y/w, z = z/w, clipped at the rectangle.
  for (int round = 0; round < 300; ++round) {
    DlTransform t;
    dl_transform_init(&t);
    t.world = random_affine();
    t.view = random_affine();
    t.view.m[3][2] += 6.0f;
    t.projection = perspective(1.0f, 100.0f, random_float(0.8f, 1.6f));
    DlViewport1 vp = {44, random_number() % 50, random_number() % 50, 200 + random_number() % 400, 100 + random_number() % 300, 0, 0, 1.0f, 1.0f, 0.0f, 1.0f};
    vp.dvScaleX = (float)vp.dwWidth / 2.0f * random_float(0.5f, 1.5f);
    vp.dvScaleY = (float)vp.dwHeight / 2.0f * random_float(0.5f, 1.5f);
    dl_viewport7_from_v1(&vp, &t.viewport, &t.clip);
    dl_transform_prepare(&t);
    DlLighting l;
    dl_lighting_init(&l, 2);
    const float p[3] = {random_float(-3, 3), random_float(-3, 3), random_float(-3, 3)};
    DlClipVertex cv;
    const float tex[1][4] = {{0.25f, 0.75f, 0, 1}};
    dl_process_vertex(&t, &l, 0, p, nullptr, nullptr, nullptr, tex, 1, &cv);
    DlTlVertex tl;
    dl_tl_from_clip(&t, &cv, &tl);
    const Mat wvp = mul(mul(to_mat(t.world), to_mat(t.view)), to_mat(t.projection));
    const double pd[3] = {p[0], p[1], p[2]};
    double h[4];
    apply(wvp, pd, h);
    const double sx = vp.dwX + vp.dwWidth / 2.0 + vp.dvScaleX * h[0] / h[3], sy = vp.dwY + vp.dwHeight / 2.0 - vp.dvScaleY * h[1] / h[3];
    CHECK(near(tl.sx, sx, 1e-3) && near(tl.sy, sy, 1e-3) && near(tl.sz, h[2] / h[3], 1e-3) && near(tl.rhw, 1.0 / h[3], 1e-3));
    CHECK(tl.tu == 0.25f && tl.tv == 0.75f);
    // the clip codes: the screen point against the viewport's rectangle, z against 0 .. 1
    uint32_t code = 0;
    if (h[3] > 0) {
      if (sx < vp.dwX) code |= DL_CLIP_LEFT;
      if (sx > vp.dwX + vp.dwWidth) code |= DL_CLIP_RIGHT;
      if (sy < vp.dwY) code |= DL_CLIP_TOP;
      if (sy > vp.dwY + vp.dwHeight) code |= DL_CLIP_BOTTOM;
      if (h[2] < 0) code |= DL_CLIP_FRONT;
      if (h[2] > h[3]) code |= DL_CLIP_BACK;
      const double margin = 1e-3;
      const bool borderline = fabs(sx - vp.dwX) < margin || fabs(sx - vp.dwX - vp.dwWidth) < margin || fabs(sy - vp.dwY) < margin ||
                              fabs(sy - vp.dwY - vp.dwHeight) < margin || fabs(h[2]) < margin || fabs(h[2] - h[3]) < margin;
      if (!borderline) CHECK(cv.clip == code);
    }
  }
  // D3DVIEWPORT2: the clip volume dvClipX .. dvClipX + dvClipWidth (x), dvClipY - dvClipHeight .. dvClipY (y), dvMinZ .. dvMaxZ to the rectangle.
  for (int round = 0; round < 300; ++round) {
    DlTransform t;
    dl_transform_init(&t);
    t.projection = perspective(1.0f, 20.0f, 1.0f);
    DlViewport2 vp = {44, 10, 20, 300, 200, random_float(-1.5f, -0.5f), random_float(0.5f, 1.5f), random_float(1.0f, 3.0f), random_float(1.0f, 3.0f),
                      random_float(0.0f, 0.3f), random_float(0.6f, 1.0f)};
    dl_viewport7_from_v2(&vp, &t.viewport, &t.clip);
    dl_transform_prepare(&t);
    const float p[3] = {random_float(-4, 4), random_float(-4, 4), random_float(1.5f, 18.0f)};
    DlLighting l;
    dl_lighting_init(&l, 3);
    DlClipVertex cv;
    dl_process_vertex(&t, &l, 0, p, nullptr, nullptr, nullptr, nullptr, 0, &cv);
    DlTlVertex tl;
    dl_tl_from_clip(&t, &cv, &tl);
    const double pd[3] = {p[0], p[1], p[2]};
    double h[4];
    apply(to_mat(t.projection), pd, h);
    const double x = h[0] / h[3], y = h[1] / h[3], z = h[2] / h[3];
    const double sx = vp.dwX + (x - vp.dvClipX) / vp.dvClipWidth * vp.dwWidth, sy = vp.dwY + (vp.dvClipY - y) / vp.dvClipHeight * vp.dwHeight;
    const double sz = (z - vp.dvMinZ) / (vp.dvMaxZ - vp.dvMinZ);
    CHECK(near(tl.sx, sx, 1e-3) && near(tl.sy, sy, 1e-3) && near(tl.sz, sz, 1e-3));
    uint32_t code = 0;
    if (x < vp.dvClipX) code |= DL_CLIP_LEFT;
    if (x > vp.dvClipX + vp.dvClipWidth) code |= DL_CLIP_RIGHT;
    if (y > vp.dvClipY) code |= DL_CLIP_TOP;
    if (y < vp.dvClipY - vp.dvClipHeight) code |= DL_CLIP_BOTTOM;
    if (z < vp.dvMinZ) code |= DL_CLIP_FRONT;
    if (z > vp.dvMaxZ) code |= DL_CLIP_BACK;
    const double m = 1e-4;
    const bool borderline = fabs(x - vp.dvClipX) < m || fabs(x - vp.dvClipX - vp.dvClipWidth) < m || fabs(y - vp.dvClipY) < m ||
                            fabs(y - vp.dvClipY + vp.dvClipHeight) < m || fabs(z - vp.dvMinZ) < m || fabs(z - vp.dvMaxZ) < m;
    if (!borderline) CHECK(cv.clip == code);
    // and back from the screen (D3DPROCESSVERTICES_COPY's vertices for the clipper)
    DlClipVertex back;
    dl_clip_from_tl(&t, &tl, &back);
    float s2[4];
    dl_to_screen(&t.viewport, back.position, &s2[0], &s2[1], &s2[2], &s2[3]);
    CHECK(near(s2[0], tl.sx, 1e-3) && near(s2[1], tl.sy, 1e-3) && near(s2[2], tl.sz, 1e-3) && near(s2[3], tl.rhw, 1e-3));
  }
  // GetViewport / GetViewport2 of the other form (Wine's numbers), and SetViewport's checks.
  DlViewport2 v2 = {44, 0, 0, 640, 480, -1.0f, 1.0f, 2.0f, 2.0f, 0.0f, 1.0f};
  DlViewport1 v1;
  dl_viewport1_from_v2(&v2, &v1);
  CHECK(v1.dwSize == 44 && v1.dvScaleX == 320.0f && v1.dvScaleY == 240.0f && v1.dvMaxX == 1.0f && v1.dvMaxY == 1.0f && v1.dvMaxZ == 1.0f);
  DlViewport2 back;
  dl_viewport2_from_v1(&v1, &back);
  CHECK(back.dvClipWidth == 2.0f && back.dvClipHeight == 2.0f && back.dvClipX == -1.0f && back.dvClipY == 1.0f);
  // The same viewport either way puts a point at the same place.
  {
    DlViewport7 a, b;
    DlMatrix ca, cb;
    dl_viewport7_from_v1(&v1, &a, &ca);
    dl_viewport7_from_v2(&v2, &b, &cb);
    for (int i = 0; i < 4; ++i)
      for (int j = 0; j < 4; ++j) CHECK(near(ca.m[i][j], cb.m[i][j], 1e-6));
  }
  CHECK(dl_viewport_check(44, 44, 2, 0, 0, 640, 480, 640, 480) == DL_OK);
  CHECK(dl_viewport_check(40, 44, 2, 0, 0, 640, 480, 640, 480) == DL_DDERR_INVALIDPARAMS);
  CHECK(dl_viewport_check(44, 44, 2, 1, 0, 640, 480, 640, 480) == DL_DDERR_INVALIDPARAMS);
  CHECK(dl_viewport_check(44, 44, 2, 0xFFFFFFFFu, 0, 2, 480, 640, 480) == DL_DDERR_INVALIDPARAMS);
  CHECK(dl_viewport_check(44, 44, 1, 1, 0, 640, 480, 640, 480) == DL_OK);      // (Direct3D 1 does not check the rectangle)
  // The projection Direct3D 9 is given: the program's, then the clip space.
  DlMatrix proj = perspective(1, 10, 1), clip, out;
  DlViewport7 vp7;
  DlViewport1 half = {44, 0, 0, 200, 100, 50.0f, 50.0f, 1, 1, 0, 1};
  dl_viewport7_from_v1(&half, &vp7, &clip);
  dl_legacy_projection(&proj, &clip, &out);
  CHECK(near(out.m[0][0], 0.5) && near(out.m[1][1], 1.0) && near(out.m[2][3], 1.0));
}

static void test_status_merge() {
  DlStatus s;
  memset(&s, 0, sizeof s);
  s.dwStatus = DL_STATUS_DEFAULT;
  s.drExtent.x1 = 2048; s.drExtent.y1 = 2048;
  DlProcessResult r;
  dl_result_init(&r);
  dl_status_merge(&s, &r, 1);      // (nothing processed: nothing changes)
  CHECK(s.dwStatus == DL_STATUS_DEFAULT && s.drExtent.x1 == 2048);
  r.count = 2;
  r.clip_union = DL_CLIP_LEFT | DL_CLIP_TOP;
  r.clip_intersection = DL_CLIP_LEFT;
  r.has_extent = 1;
  r.x1 = 10.2f; r.y1 = 20.7f; r.x2 = 30.1f; r.y2 = 40.0f;
  dl_status_merge(&s, &r, 1);
  CHECK(s.dwStatus == ((DL_CLIP_LEFT | DL_CLIP_TOP) | (DL_CLIP_LEFT << 12) | DL_STATUS_ZNOTVISIBLE));
  CHECK(s.drExtent.x1 == 10 && s.drExtent.y1 == 20 && s.drExtent.x2 == 31 && s.drExtent.y2 == 40);
  r.clip_intersection = 0;
  r.x1 = 5; r.x2 = 6; r.y1 = 30; r.y2 = 50;
  dl_status_merge(&s, &r, 0);      // (no extents update)
  CHECK((s.dwStatus & DL_STATUS_CLIPINTERSECTIONALL) == 0 && s.drExtent.x1 == 10);
  dl_status_merge(&s, &r, 1);
  CHECK(s.drExtent.x1 == 5 && s.drExtent.y2 == 50 && s.drExtent.x2 == 31);
  CHECK(dl_floor_int(-1.5f) == -2 && dl_ceil_int(-1.5f) == -1 && dl_floor_int(3e10f) == INT32_MAX && dl_ceil_int(-3e10f) == INT32_MIN && dl_floor_int(NAN) == INT32_MIN);
}

// ---- lighting ----------------------------------------------------------------------------------------------------------------------------------

// A light as the reference sees it: everything in world space, in doubles.
struct RefLight { int type; double color[3], pos[3], dir[3], range, falloff, att[3], theta, phi; bool specular; };
static void normalize3(double v[3]) { const double l = sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); if (l > 0) for (int i = 0; i < 3; ++i) v[i] /= l; }
static double dot3(const double a[3], const double b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

// Direct3D 1-3 lighting of one vertex (world = identity, camera at `eye`), written out from the description in docs/Direct3DLegacy.md.
static void reference_legacy(const std::vector<RefLight>& lights, const double p[3], const double n_in[3], const double eye[3], const DlMaterial7& m,
                             const double ambient[3], bool specular_on, double out_diffuse[4], double out_specular[3]) {
  double n[3] = {n_in[0], n_in[1], n_in[2]};
  normalize3(n);
  double v[3] = {eye[0] - p[0], eye[1] - p[1], eye[2] - p[2]};
  normalize3(v);
  double dsum[3] = {0, 0, 0}, ssum[3] = {0, 0, 0};
  for (const RefLight& L : lights) {
    double l[3], weight = 1;
    if (L.type == DL_LIGHT_DIRECTIONAL) {
      for (int i = 0; i < 3; ++i) l[i] = -L.dir[i];
      normalize3(l);
    } else if (L.type == DL_LIGHT_PARALLELPOINT) {
      for (int i = 0; i < 3; ++i) l[i] = L.pos[i];      // (from the object's origin, which is 0)
      normalize3(l);
    } else {
      for (int i = 0; i < 3; ++i) l[i] = L.pos[i] - p[i];
      const double d = sqrt(dot3(l, l));
      normalize3(l);
      if (d > L.range) continue;
      const double r = (L.range - d) / L.range;
      weight = L.att[0] + L.att[1] * r + L.att[2] * r * r;
      if (weight <= 0) continue;
      if (L.type == DL_LIGHT_SPOT) {
        double axis[3] = {L.dir[0], L.dir[1], L.dir[2]};
        normalize3(axis);
        const double rho = -dot3(l, axis), co = cos(L.phi / 2), ci = cos(L.theta / 2);
        if (rho <= co) continue;
        if (rho < ci) weight *= pow((rho - co) / (ci - co), L.falloff);
      }
    }
    const double nl = dot3(n, l);
    if (nl <= 0) continue;
    for (int i = 0; i < 3; ++i) dsum[i] += weight * nl * L.color[i];
    if (!specular_on || !L.specular) continue;
    double hv[3] = {l[0] + v[0], l[1] + v[1], l[2] + v[2]};
    normalize3(hv);
    const double nh = dot3(n, hv);
    if (nh <= 0) continue;
    for (int i = 0; i < 3; ++i) ssum[i] += weight * pow(nh, m.power) * L.color[i];
  }
  const double md[3] = {m.diffuse.r, m.diffuse.g, m.diffuse.b}, ma[3] = {m.ambient.r, m.ambient.g, m.ambient.b}, ms[3] = {m.specular.r, m.specular.g, m.specular.b},
               me[3] = {m.emissive.r, m.emissive.g, m.emissive.b};
  for (int i = 0; i < 3; ++i) {
    out_diffuse[i] = std::min(1.0, std::max(0.0, me[i] + ma[i] * ambient[i] + md[i] * dsum[i]));
    out_specular[i] = specular_on ? std::min(1.0, std::max(0.0, ms[i] * ssum[i])) : 0.0;
  }
  out_diffuse[3] = std::min(1.0, std::max(0.0, (double)m.diffuse.a));
}

static void test_legacy_lighting() {
  for (int round = 0; round < 400; ++round) {
    DlTransform t;
    dl_transform_init(&t);
    // (the camera at (0, 0, -5) looking along +z: the view moves the world by +5 in z)
    t.view.m[3][2] = 5.0f;
    t.projection = perspective(1, 50, 1);
    dl_transform_prepare(&t);
    const double eye[3] = {0, 0, -5};
    DlLighting l;
    dl_lighting_init(&l, 1 + round % 3);
    l.enabled = 1;
    l.specular = round % 2;
    l.material.diffuse = {random_float(0, 1), random_float(0, 1), random_float(0, 1), random_float(0, 1)};
    l.material.ambient = {random_float(0, 1), random_float(0, 1), random_float(0, 1), 1};
    l.material.specular = {random_float(0, 1), random_float(0, 1), random_float(0, 1), 1};
    l.material.emissive = {random_float(0, 0.2f), random_float(0, 0.2f), random_float(0, 0.2f), 1};
    l.material.power = random_float(1, 30);
    const double ambient[3] = {random_float(0, 0.3f), random_float(0, 0.3f), random_float(0, 0.3f)};
    for (int i = 0; i < 3; ++i) l.ambient[i] = (float)ambient[i];
    std::vector<RefLight> ref;
    const unsigned count = 1 + random_number() % 4;
    for (unsigned k = 0; k < count; ++k) {
      DlLight2 in;
      memset(&in, 0, sizeof in);
      in.dwSize = k % 2 ? DL_LIGHT2_SIZE : DL_LIGHT_SIZE;
      in.dltType = 1 + random_number() % 4;
      in.dcvColor = {random_float(0, 1), random_float(0, 1), random_float(0, 1), 1};
      in.dvPosition = {random_float(-4, 4), random_float(-4, 4), random_float(-6, -1)};
      in.dvDirection = {random_float(-1, 1), random_float(-1, 1), random_float(0.2f, 1)};
      in.dvRange = random_float(3, 15);
      in.dvFalloff = random_float(0.5f, 2);
      in.dvAttenuation0 = random_float(0, 1);
      in.dvAttenuation1 = random_float(0, 1);
      in.dvAttenuation2 = random_float(0, 0.5f);
      in.dvTheta = random_float(0.2f, 1.0f);
      in.dvPhi = in.dvTheta + random_float(0.1f, 1.5f);
      in.dwFlags = DL_LIGHT_ACTIVE | (random_number() % 3 == 0 ? DL_LIGHT_NO_SPECULAR : 0);
      int active = 0;
      CHECK(dl_light_from_legacy(&in, &l.light[l.lights], &active) == DL_OK && active);
      ++l.lights;
      RefLight r;
      r.type = (int)in.dltType;
      r.color[0] = in.dcvColor.r; r.color[1] = in.dcvColor.g; r.color[2] = in.dcvColor.b;
      r.pos[0] = in.dvPosition.x; r.pos[1] = in.dvPosition.y; r.pos[2] = in.dvPosition.z;
      r.dir[0] = in.dvDirection.x; r.dir[1] = in.dvDirection.y; r.dir[2] = in.dvDirection.z;
      r.range = in.dvRange; r.falloff = in.dvFalloff; r.att[0] = in.dvAttenuation0; r.att[1] = in.dvAttenuation1; r.att[2] = in.dvAttenuation2;
      r.theta = in.dvTheta; r.phi = in.dvPhi;
      r.specular = in.dwSize < DL_LIGHT2_SIZE || !(in.dwFlags & DL_LIGHT_NO_SPECULAR);
      ref.push_back(r);
    }
    const float p[3] = {random_float(-1, 1), random_float(-1, 1), random_float(-1, 1)}, n[3] = {random_float(-1, 1), random_float(-1, 1), random_float(-1, 0.2f)};
    DlClipVertex cv;
    dl_process_vertex(&t, &l, 1, p, n, nullptr, nullptr, nullptr, 0, &cv);
    double rd[4], rs[3];
    const double pd[3] = {p[0], p[1], p[2]}, nd[3] = {n[0], n[1], n[2]};
    reference_legacy(ref, pd, nd, eye, l.material, ambient, l.specular != 0, rd, rs);
    bool same = true;
    for (int i = 0; i < 4; ++i) same = same && fabs(cv.diffuse[i] - rd[i]) < 2e-3;
    for (int i = 0; i < 3; ++i) same = same && fabs(cv.specular[i] - rs[i]) < 2e-3;
    CHECK(same);
    if (!same) printf("  round %d: %f %f %f / %f %f %f\n", round, cv.diffuse[0], cv.diffuse[1], cv.diffuse[2], rd[0], rd[1], rd[2]);
  }
  // Exact cases: a light straight on, the legacy attenuation's ends, a light past its range, a NO_SPECULAR light, and a D3DLIGHT2 that is off.
  DlTransform t;
  dl_transform_init(&t);
  dl_transform_prepare(&t);
  DlLighting l;
  dl_lighting_init(&l, 2);
  l.enabled = 1;
  l.specular = 0;
  l.material.diffuse = {1, 1, 1, 0.5f};
  l.lights = 1;
  DlLight2 in;
  memset(&in, 0, sizeof in);
  in.dwSize = DL_LIGHT2_SIZE;
  in.dltType = DL_LIGHT_POINT;
  in.dcvColor = {1, 0.5f, 0.25f, 1};
  in.dvPosition = {0, 0, -4};
  in.dvRange = 8;
  in.dvAttenuation1 = 1;      // (fades linearly to nothing at the range)
  in.dwFlags = DL_LIGHT_ACTIVE;
  int active;
  CHECK(dl_light_from_legacy(&in, &l.light[0], &active) == DL_OK && active);
  const float origin[3] = {0, 0, 0}, normal[3] = {0, 0, -1};
  float d[4], s[3];
  dl_light_vertex(&t, &l, origin, normal, nullptr, nullptr, d, s);
  CHECK(near(d[0], 0.5) && near(d[1], 0.25) && near(d[2], 0.125) && near(d[3], 0.5));      // (4 of the range of 8 left: half)
  in.dvRange = 4;
  dl_light_from_legacy(&in, &l.light[0], &active);
  dl_light_vertex(&t, &l, origin, normal, nullptr, nullptr, d, s);
  CHECK(d[0] == 0.0f);      // (at the range: nothing left)
  in.dvRange = 3.9f;
  dl_light_from_legacy(&in, &l.light[0], &active);
  dl_light_vertex(&t, &l, origin, normal, nullptr, nullptr, d, s);
  CHECK(d[0] == 0.0f);      // (past it)
  in.dvRange = 100; in.dvAttenuation0 = 1; in.dvAttenuation1 = 0;
  dl_light_from_legacy(&in, &l.light[0], &active);
  dl_light_vertex(&t, &l, origin, normal, nullptr, nullptr, d, s);
  CHECK(near(d[0], 1.0) && near(d[1], 0.5));      // (constant)
  in.dwFlags = DL_LIGHT_NO_SPECULAR;      // (and not active)
  CHECK(dl_light_from_legacy(&in, &l.light[0], &active) == DL_OK && !active && l.light[0].specular[0] == 0.0f && l.light[0].diffuse[0] == 1.0f);
  in.dwSize = DL_LIGHT_SIZE;      // (a D3DLIGHT: always on, dwFlags not read)
  CHECK(dl_light_from_legacy(&in, &l.light[0], &active) == DL_OK && active && l.light[0].specular[0] == 1.0f);
  in.dwSize = 72;
  CHECK(dl_light_from_legacy(&in, &l.light[0], &active) == DL_DDERR_INVALIDPARAMS);
  in.dwSize = DL_LIGHT2_SIZE;
  in.dltType = DL_LIGHT_GLSPOT;
  CHECK(dl_light_from_legacy(&in, &l.light[0], &active) == DL_DDERR_INVALIDPARAMS);
  in.dltType = 0;
  CHECK(dl_light_from_legacy(&in, &l.light[0], &active) == DL_DDERR_INVALIDPARAMS);
  // A parallel point light lights a moved object from the object's origin: every vertex the same way, unattenuated.
  {
    DlTransform tm;
    dl_transform_init(&tm);
    tm.world.m[3][0] = 10.0f;
    dl_transform_prepare(&tm);
    DlLighting lp;
    dl_lighting_init(&lp, 3);
    lp.enabled = 1;
    lp.material.diffuse = {1, 1, 1, 1};
    lp.lights = 1;
    DlLight2 pp;
    memset(&pp, 0, sizeof pp);
    pp.dwSize = DL_LIGHT2_SIZE;
    pp.dltType = DL_LIGHT_PARALLELPOINT;
    pp.dcvColor = {1, 1, 1, 1};
    pp.dvPosition = {10, 5, 0};      // (straight above the object's origin)
    pp.dwFlags = DL_LIGHT_ACTIVE;
    dl_light_from_legacy(&pp, &lp.light[0], &active);
    const float up[3] = {0, 1, 0}, a[3] = {-3, 0, 0}, b[3] = {3, 2, 1};
    float da[4], db[4], sa[3];
    dl_light_vertex(&tm, &lp, a, up, nullptr, nullptr, da, sa);
    dl_light_vertex(&tm, &lp, b, up, nullptr, nullptr, db, sa);
    CHECK(near(da[0], 1.0) && near(db[0], 1.0));
  }
  // Direct3D 7's model: 1 / (a0 + a1 d + a2 d^2), the light's own ambient, the vertex's colours as the material's (COLORVERTEX).
  {
    DlTransform t7;
    dl_transform_init(&t7);
    dl_transform_prepare(&t7);
    DlLighting l7;
    dl_lighting_init(&l7, 7);
    CHECK(l7.enabled && !l7.legacy && l7.color_vertex && l7.diffuse_source == DL_MCS_COLOR1);
    l7.material.diffuse = {0.5f, 0.5f, 0.5f, 1};
    l7.material.ambient = {1, 1, 1, 1};
    l7.lights = 1;
    DlLight7 L;
    memset(&L, 0, sizeof L);
    L.dltType = DL_LIGHT_POINT;
    L.dcvDiffuse = {1, 1, 1, 1};
    L.dcvAmbient = {0.1f, 0.2f, 0.3f, 1};
    L.dvPosition = {0, 0, -2};
    L.dvRange = 10;
    L.dvAttenuation0 = 0;
    L.dvAttenuation1 = 1;
    CHECK(dl_light_from_light7(&L, &l7.light[0]) == DL_OK);
    float d7[4], s7[3];
    const float vertex_colour[4] = {0.8f, 0.4f, 0.2f, 0.6f};
    dl_light_vertex(&t7, &l7, origin, normal, vertex_colour, nullptr, d7, s7);
    // (the light's ambient is attenuated like the rest: weight 1 / (0 + 1 * 2))
    CHECK(near(d7[0], 0.05 + 0.8 * 0.5) && near(d7[1], 0.1 + 0.4 * 0.5) && near(d7[2], 0.15 + 0.2 * 0.5) && near(d7[3], 0.6));
    l7.color_vertex = 0;
    dl_light_vertex(&t7, &l7, origin, normal, vertex_colour, nullptr, d7, s7);
    CHECK(near(d7[0], 0.05 + 0.25) && near(d7[3], 1.0));
    L.dltType = DL_LIGHT_PARALLELPOINT;
    CHECK(dl_light_from_light7(&L, &l7.light[0]) == DL_DDERR_INVALIDPARAMS);
  }
}

static void test_fog() {
  CHECK(dl_fog_factor(DL_FOG_NONE, 0, 1, 1, 5) == 1.0f);
  CHECK(near(dl_fog_factor(DL_FOG_LINEAR, 10, 20, 0, 15), 0.5) && dl_fog_factor(DL_FOG_LINEAR, 10, 20, 0, 25) == 0.0f && dl_fog_factor(DL_FOG_LINEAR, 10, 20, 0, 5) == 1.0f);
  CHECK(dl_fog_factor(DL_FOG_LINEAR, 10, 10, 0, 5) == 1.0f && dl_fog_factor(DL_FOG_LINEAR, 10, 10, 0, 15) == 0.0f);
  for (int i = 0; i < 100; ++i) {
    const float density = random_float(0.01f, 0.5f), distance = random_float(0, 40);
    CHECK(near(dl_fog_factor(DL_FOG_EXP, 0, 1, density, distance), exp(-density * distance), 1e-4));
    CHECK(near(dl_fog_factor(DL_FOG_EXP2, 0, 1, density, distance), exp(-(density * distance) * (density * distance)), 1e-4));
  }
  // Into the specular alpha of a lit vertex; Direct3D 1-3 leave a D3DLVERTEX's own; Direct3D 7 fogs every vertex; range fog.
  DlTransform t;
  dl_transform_init(&t);
  t.view.m[3][2] = 15.0f;
  dl_transform_prepare(&t);
  DlLighting l;
  dl_lighting_init(&l, 2);
  l.enabled = 1;
  l.fog_mode = DL_FOG_LINEAR;
  l.fog_start = 10;
  l.fog_end = 20;
  const float p[3] = {0, 0, 0}, n[3] = {0, 0, -1};
  DlClipVertex cv;
  dl_process_vertex(&t, &l, 1, p, n, nullptr, nullptr, nullptr, 0, &cv);
  CHECK(near(cv.specular[3], 0.5));
  const uint32_t white = 0xFFFFFFFFu, spec = 0x80000000u;
  dl_process_vertex(&t, &l, 0, p, nullptr, &white, &spec, nullptr, 0, &cv);
  CHECK(near(cv.specular[3], 128.0 / 255.0));
  l.legacy = 0;
  dl_process_vertex(&t, &l, 0, p, nullptr, &white, &spec, nullptr, 0, &cv);
  CHECK(near(cv.specular[3], 0.5));
  const float side[3] = {0, 8.660254f, 0};      // (distance sqrt(75 + 225) = 17.32, z 15)
  dl_process_vertex(&t, &l, 0, side, nullptr, &white, &spec, nullptr, 0, &cv);
  CHECK(near(cv.specular[3], 0.5));
  l.range_fog = 1;
  dl_process_vertex(&t, &l, 0, side, nullptr, &white, &spec, nullptr, 0, &cv);
  CHECK(near(cv.specular[3], (20.0 - sqrt(300.0)) / 10.0, 1e-3));
  // the light states that feed it
  dl_lighting_apply_light_state(&l, DL_LIGHTSTATE_FOGMODE, DL_FOG_EXP2);
  dl_lighting_apply_light_state(&l, DL_LIGHTSTATE_FOGDENSITY, dl_bits_of(0.25f));
  dl_lighting_apply_light_state(&l, DL_LIGHTSTATE_AMBIENT, 0x00FF8000u);
  CHECK(l.fog_mode == DL_FOG_EXP2 && l.fog_density == 0.25f && l.ambient[0] == 1.0f && near(l.ambient[1], 128.0 / 255.0) && l.ambient[2] == 0.0f);
  dl_lighting_apply_light_state(&l, DL_LIGHTSTATE_FOGMODE, 77);
  CHECK(l.fog_mode == DL_FOG_NONE);
}

// ---- clipping ---------------------------------------------------------------------------------------------------------------------------------

static DlClipVertex clip_vertex(float x, float y, float z, float w, float red) {
  DlClipVertex v;
  memset(&v, 0, sizeof v);
  v.position[0] = x; v.position[1] = y; v.position[2] = z; v.position[3] = w;
  v.diffuse[0] = red;
  v.clip = dl_clip_code(v.position, v.user, 0);
  return v;
}

static void test_clipper() {
  // Across the front plane: two vertices in front (z < 0), one behind.
  DlClipVertex poly[DL_CLIP_MAX] = {clip_vertex(0, 0, -1, 1, 0), clip_vertex(0.5f, 0, 0.5f, 1, 1), clip_vertex(0, 0.5f, -1, 1, 0)};
  CHECK(dl_triangle_clip_class(poly[0].clip, poly[1].clip, poly[2].clip) == 2);
  unsigned n = dl_clip_polygon(poly, 3, DL_CLIP_FRONT);
  CHECK(n == 3);
  for (unsigned i = 0; i < n; ++i) CHECK(poly[i].position[2] >= -1e-6f);
  // the new vertices lie at z = 0, a third of the way: colour 2/3
  int on_plane = 0;
  for (unsigned i = 0; i < n; ++i)
    if (fabs(poly[i].position[2]) < 1e-6f) { ++on_plane; CHECK(near(poly[i].diffuse[0], 2.0 / 3.0)); }
  CHECK(on_plane == 2);
  // Wholly outside one plane; wholly inside.
  CHECK(dl_triangle_clip_class(DL_CLIP_LEFT, DL_CLIP_LEFT | DL_CLIP_TOP, DL_CLIP_LEFT) == 1 && dl_triangle_clip_class(0, 0, 0) == 0);
  DlClipVertex outside[DL_CLIP_MAX] = {clip_vertex(-3, 0, 0.5f, 1, 0), clip_vertex(-2, 1, 0.5f, 1, 0), clip_vertex(-2, -1, 0.5f, 1, 0)};
  CHECK(dl_clip_polygon(outside, 3, DL_CLIP_ALL) == 0);
  // Random triangles: what is left is inside every plane, and a triangle inside stays as it was.
  for (int round = 0; round < 3000; ++round) {
    DlClipVertex p[DL_CLIP_MAX];
    for (int k = 0; k < 3; ++k) p[k] = clip_vertex(random_float(-3, 3), random_float(-3, 3), random_float(-2, 3), random_float(-0.5f, 2.5f), random_float(0, 1));
    for (int k = 0; k < 3; ++k)
      for (int u = 0; u < 6; ++u) p[k].user[u] = random_float(-1, 1);
    const uint32_t planes = round % 2 ? DL_CLIP_ALL : 0x3Fu;
    const bool inside = (p[0].clip | p[1].clip | p[2].clip) == 0 && planes == 0x3Fu;
    DlClipVertex before[3] = {p[0], p[1], p[2]};
    const unsigned m = dl_clip_polygon(p, 3, planes);
    CHECK(m == 0 || (m >= 3 && m <= DL_CLIP_MAX));
    for (unsigned i = 0; i < m; ++i)
      for (unsigned plane = 0; plane < 12; ++plane)
        if (planes & (1u << plane)) CHECK(dl_plane_distance(&p[i], plane) >= -1e-4f * (1 + fabs(p[i].position[3])));
    if (inside) {
      CHECK(m == 3);
      for (int k = 0; k < 3 && m == 3; ++k) CHECK(memcmp(p[k].position, before[k].position, sizeof p[k].position) == 0);
    }
  }
  // Lines.
  DlClipVertex a = clip_vertex(-2, 0, 0.5f, 1, 0), b = clip_vertex(0, 0, 0.5f, 1, 1);
  CHECK(dl_clip_line(&a, &b, DL_CLIP_ALL) == 1 && near(a.position[0], -1.0) && near(a.diffuse[0], 0.5) && b.position[0] == 0.0f);
  a = clip_vertex(-2, 0, 0.5f, 1, 0);
  b = clip_vertex(-3, 0, 0.5f, 1, 1);
  CHECK(dl_clip_line(&a, &b, DL_CLIP_ALL) == 0);
  // The planes of a transform.
  DlTransform t;
  dl_transform_init(&t);
  t.user_planes = 0x5;
  CHECK(dl_clip_planes(&t) == (0x3Fu | (0x5u << 6)));
}

static void test_user_clip_planes() {
  DlTransform t;
  dl_transform_init(&t);
  t.user_planes = 1u << 2;
  t.user_plane[2][0] = 1; t.user_plane[2][3] = -0.5f;      // (inside where x >= 0.5)
  dl_transform_prepare(&t);
  DlLighting l;
  dl_lighting_init(&l, 7);
  l.enabled = 0;
  const float a[3] = {0.25f, 0, 0.5f}, b[3] = {0.75f, 0, 0.5f};
  DlClipVertex va, vb;
  dl_process_vertex(&t, &l, 0, a, nullptr, nullptr, nullptr, nullptr, 0, &va);
  dl_process_vertex(&t, &l, 0, b, nullptr, nullptr, nullptr, nullptr, 0, &vb);
  CHECK(va.clip == (DL_CLIP_GEN0 << 2) && vb.clip == 0);
  CHECK(dl_clip_line(&va, &vb, dl_clip_planes(&t)) == 1 && near(va.position[0], 0.5));
}

// ---- TransformVertices ------------------------------------------------------------------------------------------------------------------------

static void test_transform_vertices() {
  DlTransform t;
  dl_transform_init(&t);
  DlViewport1 vp = {44, 10, 20, 200, 100, 100.0f, 50.0f, 1, 1, 0, 1};
  dl_viewport7_from_v1(&vp, &t.viewport, &t.clip);
  dl_transform_prepare(&t);
  // D3DLVERTEX in, D3DTLVERTEX out: the colours and texture coordinates go along.
  DlLVertex in[3] = {{0, 0, 0.5f, 0, 0x11223344u, 0x55667788u, 0.5f, 0.25f}, {0.5f, -0.5f, 0.25f, 0, 1, 2, 3, 4}, {2, 0, 0.5f, 0, 5, 6, 7, 8}};
  DlTlVertex out[3];
  DlHVertex h[3];
  uint32_t offscreen = 99;
  CHECK(dl_transform_vertices(&t, 3, dl_const_bytes(in, sizeof in), 32, dl_bytes(out, sizeof out), 32, nullptr, 0, DL_TRANSFORM_UNCLIPPED, &offscreen) == DL_OK);
  CHECK(offscreen == 0);
  CHECK(out[0].sx == 110.0f && out[0].sy == 70.0f && out[0].sz == 0.5f && out[0].rhw == 1.0f && out[0].color == 0x11223344u && out[0].specular == 0x55667788u &&
        out[0].tu == 0.5f && out[0].tv == 0.25f);
  CHECK(out[1].sx == 160.0f && out[1].sy == 95.0f && out[1].sz == 0.25f);
  CHECK(out[2].sx == 310.0f);      // (unclipped: put on the screen all the same)
  CHECK(dl_transform_vertices(&t, 3, dl_const_bytes(in, sizeof in), 32, dl_bytes(out, sizeof out), 32, h, 3, DL_TRANSFORM_CLIPPED, &offscreen) == DL_OK);
  CHECK(h[0].flags == 0 && h[1].flags == 0 && h[2].flags == DL_CLIP_RIGHT && offscreen == 0);
  CHECK(near(h[2].hx, 2.0) && near(h[2].hz, 0.5));      // (before the viewport's clip space)
  CHECK(near(out[2].sx, 2.0 * 100.0 / 100.0) && out[2].rhw == 1.0f);      // (clip space x y z w where it is clipped: 2 * 2 * 100 / 200)
  CHECK(dl_transform_vertices(&t, 1, dl_const_bytes(&in[2], 32), 32, dl_bytes(out, sizeof out), 32, h, 1, DL_TRANSFORM_CLIPPED, &offscreen) == DL_OK &&
        offscreen == DL_CLIP_RIGHT);
  // refusals
  CHECK(dl_transform_vertices(&t, 3, dl_const_bytes(in, sizeof in), 32, dl_bytes(out, sizeof out), 32, h, 3, 0, &offscreen) == DL_DDERR_INVALIDPARAMS);
  CHECK(dl_transform_vertices(&t, 3, dl_const_bytes(in, sizeof in), 32, dl_bytes(out, sizeof out), 32, nullptr, 0, DL_TRANSFORM_CLIPPED, &offscreen) == DL_DDERR_INVALIDPARAMS);
  CHECK(dl_transform_vertices(&t, 4, dl_const_bytes(in, sizeof in), 32, dl_bytes(out, sizeof out), 32, nullptr, 0, DL_TRANSFORM_UNCLIPPED, &offscreen) == DL_DDERR_INVALIDPARAMS);
  CHECK(dl_transform_vertices(&t, 3, dl_const_bytes(in, sizeof in), 8, dl_bytes(out, sizeof out), 32, nullptr, 0, DL_TRANSFORM_UNCLIPPED, &offscreen) == DL_DDERR_INVALIDPARAMS);
  // An output of only x y z w (stride 16), and the last input vertex just 12 bytes long.
  float small_out[3][4];
  float packed[7] = {0, 0, 0.5f, 99, 0.5f, -0.5f, 0.25f};
  CHECK(dl_transform_vertices(&t, 2, dl_const_bytes(packed, sizeof packed), 16, dl_bytes(small_out, 32), 16, nullptr, 0, DL_TRANSFORM_UNCLIPPED, &offscreen) == DL_OK);
  CHECK(small_out[1][0] == 160.0f);
}

// ---- caps -------------------------------------------------------------------------------------------------------------------------------------

static DlCaps9 sample_caps() {
  DlCaps9 c;
  memset(&c, 0, sizeof c);
  c.DevCaps = 0x001BBEF0u | DL_DEVCAPS_HWTRANSFORMANDLIGHT | DL_DEVCAPS_HWRASTERIZATION | DL_DEVCAPS_DRAWPRIMITIVES2EX | 0x00100000u /* PUREDEVICE */;
  c.PrimitiveMiscCaps = 0x2 | 0x10 | 0x20 | 0x40 | 0x80 /* COLORWRITEENABLE */;
  c.RasterCaps = 0x1 | 0x10 | 0x80 | 0x100 | 0x2000 | 0x10000 | 0x20000 | 0x100000 | 0x200000 | DL_PRASTERCAPS9_DEPTHBIAS | 0x1000000 /* SCISSORTEST */;
  c.ZCmpCaps = c.AlphaCmpCaps = 0xFF;
  c.SrcBlendCaps = c.DestBlendCaps = 0x3FFF;
  c.ShadeCaps = 0x8 | 0x200 | 0x4000 | 0x80000;
  c.TextureCaps = 0x1 | 0x4 | 0x40 | 0x400 | 0x800 | 0x2000 | 0x4000;      // (no POW2: any size)
  c.TextureFilterCaps = 0x0300 | 0x400 | 0x30000 | 0x03000000 | 0x04000000;
  c.TextureAddressCaps = 0x1F | 0x20 /* MIRRORONCE */;
  c.LineCaps = 0x20 | 0x1;
  c.MaxTextureWidth = c.MaxTextureHeight = 8192;
  c.MaxTextureRepeat = 8192;
  c.MaxTextureAspectRatio = 8192;
  c.MaxAnisotropy = 16;
  c.MaxVertexW = 1e10f;
  c.GuardBandLeft = -32768; c.GuardBandTop = -32768; c.GuardBandRight = 32768; c.GuardBandBottom = 32768;
  c.StencilCaps = 0x1FF;
  c.FVFCaps = 8 | 0x80000 | 0x100000;
  c.TextureOpCaps = 0x03FFFFFF;
  c.MaxTextureBlendStages = 8;
  c.MaxSimultaneousTextures = 8;
  c.VertexProcessingCaps = 0x1 | 0x2 | 0x8 | 0x10 | 0x20 | 0x40 | 0x100;
  c.MaxActiveLights = 8;
  c.MaxUserClipPlanes = 6;
  c.MaxVertexBlendMatrices = 4;
  c.color_key = 1;
  return c;
}

static void test_caps() {
  const DlCaps9 c = sample_caps();
  DlDeviceDesc7 d;
  dl_device_desc7(&c, DL_DEVICE_TNLHAL, &d);
  CHECK(d.dwDevCaps == ((c.DevCaps & 0x000BFFFFu)) && (d.dwDevCaps & DL_DEVCAPS_HWTRANSFORMANDLIGHT) && !(d.dwDevCaps & 0x00100000u));
  CHECK(dl_guid_equal(&d.deviceGUID, &dl_guid_tnlhal));
  CHECK(d.dpcLineCaps.dwSize == 56 && d.dpcTriCaps.dwSize == 56 && memcmp(&d.dpcLineCaps, &d.dpcTriCaps, sizeof d.dpcLineCaps) == 0);
  CHECK(d.dpcTriCaps.dwMiscCaps == 0x72);
  CHECK((d.dpcTriCaps.dwRasterCaps & DL_PRASTERCAPS_ZBIAS) && (d.dpcTriCaps.dwRasterCaps & DL_PRASTERCAPS_ANTIALIASEDGES) && !(d.dpcTriCaps.dwRasterCaps & 0x1000000u) &&
        !(d.dpcTriCaps.dwRasterCaps & DL_PRASTERCAPS9_DEPTHBIAS));
  CHECK(d.dpcTriCaps.dwSrcBlendCaps == 0x1FFF);
  CHECK(d.dpcTriCaps.dwShadeCaps == (0x8u | 0x2 | 0x200 | 0x80 | 0x4000 | 0x1000 | 0x80000 | 0x40000));
  // texture caps: POW2 | NONPOW2CONDITIONAL added (a device with no limit), TRANSPARENCY for the colour key, D3D9-only bits gone
  CHECK(d.dpcTriCaps.dwTextureCaps == (0x1u | 0x4 | 0x40 | 0x400 | 0x800 | 0x2 | 0x100 | 0x8));
  // filters: the legacy ones that follow from MIN / MAG / MIP
  CHECK(d.dpcTriCaps.dwTextureFilterCaps == (0x0300u | 0x400 | 0x30000 | 0x03000000 | 0x04000000 | 0x3F));
  CHECK(d.dpcTriCaps.dwTextureBlendCaps == 0xFF && d.dpcTriCaps.dwTextureAddressCaps == 0x1F && d.dpcTriCaps.dwStippleWidth == 32);
  CHECK(d.dwDeviceRenderBitDepth == (DL_DDBD_16 | DL_DDBD_24 | DL_DDBD_32) && d.dwDeviceZBufferBitDepth == (DL_DDBD_16 | DL_DDBD_24));
  CHECK(d.dwMinTextureWidth == 1 && d.dwMaxTextureWidth == 8192 && d.dwMaxAnisotropy == 16 && d.dvGuardBandLeft == -32768.0f);
  CHECK(d.dwStencilCaps == 0xFF && d.dwFVFCaps == (8u | 0x80000) && d.dwTextureOpCaps == 0x00FFFFFFu);
  CHECK(d.wMaxTextureBlendStages == 8 && d.wMaxSimultaneousTextures == 8 && d.dwMaxActiveLights == 8 && d.wMaxUserClipPlanes == 6 && d.wMaxVertexBlendMatrices == 4);
  CHECK(d.dwVertexProcessingCaps == (0x1u | 0x2 | 0x8 | 0x10 | 0x20 | DL_VTXPCAPS_VERTEXFOG));
  CHECK(d.dwReserved1 == 0 && d.dwReserved4 == 0);
  DlDeviceDesc7 hal, rgb;
  dl_device_desc7(&c, DL_DEVICE_HAL, &hal);
  dl_device_desc7(&c, DL_DEVICE_RGB, &rgb);
  CHECK(!(hal.dwDevCaps & DL_DEVCAPS_HWTRANSFORMANDLIGHT) && (hal.dwDevCaps & DL_DEVCAPS_HWRASTERIZATION) && dl_guid_equal(&hal.deviceGUID, &dl_guid_hal));
  CHECK(!(rgb.dwDevCaps & (DL_DEVCAPS_HWTRANSFORMANDLIGHT | DL_DEVCAPS_HWRASTERIZATION | DL_DEVCAPS_DRAWPRIMITIVES2EX)) && dl_guid_equal(&rgb.deviceGUID, &dl_guid_rgb));
  // A device whose textures must be powers of two keeps POW2 without NONPOW2CONDITIONAL.
  DlCaps9 p2 = c;
  p2.TextureCaps |= DL_PTEXTURECAPS_POW2;
  p2.color_key = 0;
  p2.MaxUserClipPlanes = 100;
  p2.MaxTextureBlendStages = 0x12345;
  dl_device_desc7(&p2, DL_DEVICE_HAL, &hal);
  CHECK((hal.dpcTriCaps.dwTextureCaps & DL_PTEXTURECAPS_POW2) && !(hal.dpcTriCaps.dwTextureCaps & (DL_PTEXTURECAPS_NONPOW2CONDITIONAL | DL_PTEXTURECAPS_TRANSPARENCY)));
  CHECK(hal.wMaxUserClipPlanes == 32 && hal.wMaxTextureBlendStages == 0xFFFF);

  // D3DDEVICEDESC for each version: sizes, flags, and the HAL / software differences.
  for (unsigned version = 1; version <= 3; ++version) {
    const uint32_t size = dl_device_desc_size(version);
    CHECK(size == (version == 1 ? 172u : version == 2 ? 204u : 252u));
    for (int kind : {DL_DEVICE_HAL, DL_DEVICE_RGB, DL_DEVICE_RAMP, DL_DEVICE_MMX}) {
      DlDeviceDesc h, s;
      dl_device_descs(&c, kind, version, &h, &s);
      CHECK(h.dwSize == size && s.dwSize == size);
      const uint8_t *hb = (const uint8_t *)&h, *sb = (const uint8_t *)&s;
      bool zero_after = true;
      for (size_t i = size; i < sizeof h; ++i) zero_after = zero_after && hb[i] == 0 && sb[i] == 0;
      CHECK(zero_after);
      CHECK(h.dtcTransformCaps.dwSize == 8 && h.dtcTransformCaps.dwCaps == DL_TRANSFORMCAPS_CLIP && h.bClipping == 1);
      CHECK(h.dlcLightingCaps.dwSize == 16 && h.dlcLightingCaps.dwCaps == 0xF && h.dlcLightingCaps.dwLightingModel == DL_LIGHTINGMODEL_RGB && h.dlcLightingCaps.dwNumLights == 8);
      CHECK(h.dwMaxVertexCount == 2048 && h.dwMaxBufferSize == 0 && h.dpcTriCaps.dwSize == 56 && s.dpcLineCaps.dwSize == 56);
      if (version >= 2) CHECK(h.dwMaxTextureWidth == 8192 && h.dwMinStippleWidth == 1 && h.dwMaxStippleHeight == 32);
      else CHECK(h.dwMaxTextureWidth == 0);
      if (version >= 3) CHECK(h.dwMaxAnisotropy == 16 && h.wMaxTextureBlendStages == 8 && h.dwFVFCaps == (8u | 0x80000));
      if (kind == DL_DEVICE_HAL) {
        CHECK(h.dcmColorModel == DL_COLOR_RGB && h.dwFlags == DL_DD_ALL && (h.dwDevCaps & DL_DEVCAPS_HWTRANSFORMANDLIGHT));
        CHECK((h.dpcTriCaps.dwTextureCaps & DL_PTEXTURECAPS_POW2) && !(s.dpcTriCaps.dwTextureCaps & DL_PTEXTURECAPS_POW2));
        CHECK(s.dcmColorModel == 0 && s.dwFlags == DL_DD_ALL && !(s.dwDevCaps & DL_DEVCAPS_HWTRANSFORMANDLIGHT) && (s.dwDevCaps & DL_DEVCAPS_HWRASTERIZATION));
      } else {
        CHECK(h.dcmColorModel == 0 && h.dwFlags == 0 && !(h.dwDevCaps & DL_DEVCAPS_HWRASTERIZATION) && !(h.dpcTriCaps.dwTextureCaps & DL_PTEXTURECAPS_POW2));
        CHECK(s.dcmColorModel == (kind == DL_DEVICE_RAMP ? DL_COLOR_MONO : DL_COLOR_RGB) && (s.dpcTriCaps.dwTextureCaps & DL_PTEXTURECAPS_POW2) &&
              !(s.dwDevCaps & (DL_DEVCAPS_HWRASTERIZATION | DL_DEVCAPS_HWTRANSFORMANDLIGHT)) && s.dwFlags == DL_DD_ALL);
      }
    }
  }
  // The devices of each version.
  const int v1[] = {DL_DEVICE_RAMP, DL_DEVICE_RGB, DL_DEVICE_HAL, DL_DEVICE_NONE}, v3[] = {DL_DEVICE_RGB, DL_DEVICE_HAL, DL_DEVICE_NONE},
            v7[] = {DL_DEVICE_RGB, DL_DEVICE_HAL, DL_DEVICE_TNLHAL, DL_DEVICE_NONE};
  for (unsigned i = 0; i < 4; ++i) CHECK(dl_enum_device(1, i) == v1[i] && dl_enum_device(2, i) == v1[i]);
  for (unsigned i = 0; i < 3; ++i) CHECK(dl_enum_device(3, i) == v3[i]);
  for (unsigned i = 0; i < 4; ++i) CHECK(dl_enum_device(7, i) == v7[i]);
  CHECK(dl_enum_device(4, 0) == DL_DEVICE_NONE && dl_enum_device(7, 1000) == DL_DEVICE_NONE);
  CHECK(strcmp(dl_device_name(DL_DEVICE_HAL), "Direct3D HAL") == 0 && strcmp(dl_device_name(DL_DEVICE_RGB), "RGB Emulation") == 0 &&
        strcmp(dl_device_name(DL_DEVICE_RAMP), "Ramp Emulation") == 0 && strcmp(dl_device_name(DL_DEVICE_TNLHAL), "Direct3D T&L HAL") == 0);
  for (int k = DL_DEVICE_RAMP; k <= DL_DEVICE_NULL; ++k) {
    CHECK(dl_device_kind(dl_device_guid(k)) == k);
    CHECK(dl_device_name(k)[0] && dl_device_description(k)[0]);
  }
  DlGuid other = dl_guid_hal;
  other.data4[7] ^= 1;
  CHECK(dl_device_kind(&other) == DL_DEVICE_NONE && dl_device_guid(DL_DEVICE_NONE) == nullptr);
  CHECK(dl_device_creatable(7, DL_DEVICE_TNLHAL) && !dl_device_creatable(3, DL_DEVICE_TNLHAL) && dl_device_creatable(2, DL_DEVICE_RAMP) &&
        !dl_device_creatable(3, DL_DEVICE_RAMP) && dl_device_creatable(3, DL_DEVICE_MMX) && !dl_device_creatable(7, DL_DEVICE_MMX) &&
        !dl_device_creatable(7, DL_DEVICE_NULL));
  // The GUIDs' bytes as d3d.h has them (the mingw build of d3d_legacy_layout.c compares all of them; one here).
  const uint8_t hal_bytes[16] = {0xE0, 0x3D, 0xE6, 0x84, 0xAA, 0x46, 0xCF, 0x11, 0x81, 0x6F, 0x00, 0x00, 0xC0, 0x20, 0x15, 0x6E};
  CHECK(memcmp(&dl_guid_hal, hal_bytes, 16) == 0);
}

static void test_find_device() {
  const DlCaps9 c = sample_caps();
  DlFindDeviceSearch s;
  memset(&s, 0, sizeof s);
  s.dwSize = DL_FINDDEVICESEARCH_SIZE;
  std::vector<uint8_t> result(DL_FINDDEVICERESULT_SIZE, 0xCC);
  auto set_size = [&](uint32_t size) { result.assign(size, 0xCC); memcpy(result.data(), &size, 4); };
  auto guid_of = [&]() { DlGuid g; memcpy(&g, result.data() + 4, 16); return g; };
  // No conditions: the first of the list, the HAL.
  set_size(DL_FINDDEVICERESULT_SIZE);
  CHECK(dl_find_device(&c, 3, &s, dl_bytes(result.data(), result.size())) == DL_OK);
  DlGuid g = guid_of();
  CHECK(dl_guid_equal(&g, &dl_guid_hal));
  uint32_t hw_size, sw_size;
  memcpy(&hw_size, result.data() + 20, 4);
  memcpy(&sw_size, result.data() + 20 + 252, 4);
  CHECK(hw_size == 252 && sw_size == 252);
  // Software: the first emulation device (Direct3D 2: MMX; 3: RGB).
  s.dwFlags = DL_FDS_HARDWARE;
  s.bHardware = 0;
  CHECK(dl_find_device(&c, 2, &s, dl_bytes(result.data(), result.size())) == DL_OK);
  g = guid_of();
  CHECK(dl_guid_equal(&g, &dl_guid_mmx));
  CHECK(dl_find_device(&c, 3, &s, dl_bytes(result.data(), result.size())) == DL_OK);
  g = guid_of();
  CHECK(dl_guid_equal(&g, &dl_guid_rgb));
  // A colour model: mono finds Ramp (versions 1 and 2) and nothing in 3.
  s.dwFlags = DL_FDS_COLORMODEL;
  s.dcmColorModel = DL_COLOR_MONO;
  CHECK(dl_find_device(&c, 1, &s, dl_bytes(result.data(), result.size())) == DL_OK);
  g = guid_of();
  CHECK(dl_guid_equal(&g, &dl_guid_ramp));
  CHECK(dl_find_device(&c, 3, &s, dl_bytes(result.data(), result.size())) == DL_DDERR_NOTFOUND);
  // By GUID; one the version does not have; caps the devices do not have.
  s.dwFlags = DL_FDS_GUID;
  s.guid = dl_guid_rgb;
  CHECK(dl_find_device(&c, 3, &s, dl_bytes(result.data(), result.size())) == DL_OK);
  s.guid = dl_guid_tnlhal;
  CHECK(dl_find_device(&c, 3, &s, dl_bytes(result.data(), result.size())) == DL_DDERR_NOTFOUND);
  s.dwFlags = DL_FDS_TRIANGLES | DL_FDS_RASTERCAPS;
  s.dpcPrimCaps.dwRasterCaps = 0x200;      // (STIPPLE: none has it)
  CHECK(dl_find_device(&c, 3, &s, dl_bytes(result.data(), result.size())) == DL_DDERR_NOTFOUND);
  s.dpcPrimCaps.dwRasterCaps = 0x10;
  CHECK(dl_find_device(&c, 3, &s, dl_bytes(result.data(), result.size())) == DL_OK);
  // POW2 in the texture caps: the HAL has it in its hardware caps, RGB in its software ones - both match (each by its own description).
  s.dwFlags = DL_FDS_TEXTURECAPS | DL_FDS_LINES;
  s.dpcPrimCaps.dwTextureCaps = DL_PTEXTURECAPS_POW2;
  CHECK(dl_find_device(&c, 3, &s, dl_bytes(result.data(), result.size())) == DL_OK);
  // A Direct3D 1 sized result: descriptions of 172 bytes at 20 and 192.
  s.dwFlags = 0;
  set_size(DL_FINDDEVICERESULT_V1_SIZE);
  CHECK(dl_find_device(&c, 1, &s, dl_bytes(result.data(), result.size())) == DL_OK);
  memcpy(&hw_size, result.data() + 20, 4);
  memcpy(&sw_size, result.data() + 20 + 172, 4);
  CHECK(hw_size == 172 && sw_size == 172);
  // A full result from IDirect3D (version 1): its descriptions are 172 bytes, the rest of each slot zero.
  set_size(DL_FINDDEVICERESULT_SIZE);
  CHECK(dl_find_device(&c, 1, &s, dl_bytes(result.data(), result.size())) == DL_OK);
  memcpy(&hw_size, result.data() + 20, 4);
  CHECK(hw_size == 172 && result[20 + 200] == 0 && result[20 + 252 + 251] == 0);
  // Sizes.
  set_size(DL_FINDDEVICERESULT_V2_SIZE + 4);
  CHECK(dl_find_device(&c, 2, &s, dl_bytes(result.data(), result.size())) == DL_DDERR_INVALIDPARAMS);
  set_size(DL_FINDDEVICERESULT_V2_SIZE);
  CHECK(dl_find_device(&c, 2, &s, dl_bytes(result.data(), result.size() - 1)) == DL_DDERR_INVALIDPARAMS);
  CHECK(dl_find_device(&c, 2, &s, dl_bytes(result.data(), result.size())) == DL_OK);
  s.dwSize = 88;
  CHECK(dl_find_device(&c, 2, &s, dl_bytes(result.data(), result.size())) == DL_DDERR_INVALIDPARAMS);
  CHECK(dl_find_device(&c, 2, nullptr, dl_bytes(result.data(), result.size())) == DL_DDERR_INVALIDPARAMS);
}

// ---- states -----------------------------------------------------------------------------------------------------------------------------------

static bool has_op(const DlStateOps& o, uint32_t target, uint32_t index, uint32_t state, uint32_t value) {
  for (unsigned i = 0; i < o.count; ++i)
    if (o.op[i].target == target && o.op[i].index == index && o.op[i].state == state && o.op[i].value == value) return true;
  return false;
}

static void test_render_states() {
  DlStateOps o;
  DlStateContext ctx = {0, DL_TBLEND_MODULATE, 2, 24};
  // The same in Direct3D 9.
  for (uint32_t s : {7u, 8u, 9u, 14u, 15u, 16u, 19u, 20u, 22u, 23u, 24u, 25u, 26u, 27u, 28u, 29u, 34u, 35u, 36u, 37u, 38u, 48u, 52u, 59u, 60u, 128u, 135u, 136u,
                     139u, 140u, 141u, 145u, 148u, 151u, 152u}) {
    for (unsigned version : {1u, 2u, 3u, 7u}) {
      CHECK(dl_render_state(version, s, 0x1234, &ctx, &o) == DL_OK && o.count == 1 && has_op(o, DL_SET_RENDER, 0, s, 0x1234));
    }
  }
  CHECK(dl_render_state(7, 137, 1, &ctx, &o) == DL_OK && has_op(o, DL_SET_RENDER, 0, 137, 1));
  for (uint32_t s : {137u, 142u, 143u}) CHECK(dl_render_state(3, s, 0, &ctx, &o) == DL_OK && o.count == 0);      // (LIGHTING .. before 7: nothing)
  uint32_t v;
  CHECK(dl_render_state_get_override(2, 137, &v) && v == 0xFFFFFFFFu && !dl_render_state_get_override(7, 137, &v) && !dl_render_state_get_override(3, 27, &v));
  // Sampler 0's.
  CHECK(dl_render_state(2, DL_RS_TEXTUREADDRESS, 3, &ctx, &o) == DL_OK && o.count == 2 && has_op(o, DL_SET_SAMPLER, 0, DL_SAMP9_ADDRESSU, 3) &&
        has_op(o, DL_SET_SAMPLER, 0, DL_SAMP9_ADDRESSV, 3));
  CHECK(dl_render_state(3, DL_RS_TEXTUREADDRESSU, 2, &ctx, &o) == DL_OK && o.count == 1 && has_op(o, DL_SET_SAMPLER, 0, DL_SAMP9_ADDRESSU, 2));
  CHECK(dl_render_state(7, DL_RS_TEXTUREADDRESSV, 4, &ctx, &o) == DL_OK && has_op(o, DL_SET_SAMPLER, 0, DL_SAMP9_ADDRESSV, 4));
  CHECK(dl_render_state(3, DL_RS_BORDERCOLOR, 0xFF00FF00u, &ctx, &o) == DL_OK && has_op(o, DL_SET_SAMPLER, 0, DL_SAMP9_BORDERCOLOR, 0xFF00FF00u));
  CHECK(dl_render_state(3, DL_RS_MIPMAPLODBIAS, dl_bits_of(-0.5f), &ctx, &o) == DL_OK && has_op(o, DL_SET_SAMPLER, 0, DL_SAMP9_MIPMAPLODBIAS, dl_bits_of(-0.5f)));
  CHECK(dl_render_state(3, DL_RS_ANISOTROPY, 4, &ctx, &o) == DL_OK && has_op(o, DL_SET_SAMPLER, 0, DL_SAMP9_MAXANISOTROPY, 4));
  // TEXTUREMAG / TEXTUREMIN: Wine's tables.
  const uint32_t mag[7] = {DL_TEXF9_POINT, DL_TEXF9_POINT, DL_TEXF9_LINEAR, DL_TEXF9_POINT, DL_TEXF9_LINEAR, DL_TEXF9_POINT, DL_TEXF9_LINEAR};
  const uint32_t min[7][2] = {{1, 0}, {1, 0}, {2, 0}, {1, 1}, {2, 1}, {1, 2}, {2, 2}};      // (filter, mip) for 0 .. 6
  for (uint32_t f = 0; f <= 6; ++f) {
    CHECK(dl_render_state(2, DL_RS_TEXTUREMAG, f, &ctx, &o) == DL_OK && o.count == 1 && has_op(o, DL_SET_SAMPLER, 0, DL_SAMP9_MAGFILTER, mag[f]));
    CHECK(dl_render_state(2, DL_RS_TEXTUREMIN, f, &ctx, &o) == DL_OK && o.count == 2 && has_op(o, DL_SET_SAMPLER, 0, DL_SAMP9_MINFILTER, min[f][0]) &&
          has_op(o, DL_SET_SAMPLER, 0, DL_SAMP9_MIPFILTER, min[f][1]));
    if (f >= 1) {
      CHECK(dl_legacy_min_filter(min[f][0], min[f][1]) == f);
      CHECK(dl_legacy_mag_filter(mag[f]) == (mag[f] == DL_TEXF9_LINEAR ? DL_FILTER_LINEAR : DL_FILTER_NEAREST));
    }
  }
  CHECK(dl_render_state(2, DL_RS_TEXTUREMAG, 99, &ctx, &o) == DL_OK && has_op(o, DL_SET_SAMPLER, 0, DL_SAMP9_MAGFILTER, DL_TEXF9_POINT));
  // WRAPU / WRAPV are bits of WRAP0.
  ctx.wrap0 = 2;
  CHECK(dl_render_state(2, DL_RS_WRAPU, 1, &ctx, &o) == DL_OK && has_op(o, DL_SET_RENDER, 0, DL_RS9_WRAP0, 3));
  CHECK(dl_render_state(2, DL_RS_WRAPV, 0, &ctx, &o) == DL_OK && has_op(o, DL_SET_RENDER, 0, DL_RS9_WRAP0, 0));
  ctx.wrap0 = 0;
  // EDGEANTIALIAS, ANTIALIAS, ZBIAS.
  CHECK(dl_render_state(3, DL_RS_EDGEANTIALIAS, 1, &ctx, &o) == DL_OK && has_op(o, DL_SET_RENDER, 0, DL_RS9_ANTIALIASEDLINEENABLE, 1));
  CHECK(dl_render_state(3, DL_RS_ANTIALIAS, 2, &ctx, &o) == DL_OK && has_op(o, DL_SET_RENDER, 0, DL_RS9_MULTISAMPLEANTIALIAS, 1));
  CHECK(dl_render_state(3, DL_RS_ZBIAS, 2, &ctx, &o) == DL_OK && has_op(o, DL_SET_RENDER, 0, DL_RS9_DEPTHBIAS, dl_bits_of(-2.0f / 16777216.0f)));
  CHECK(dl_depth_bias(0, 24) == 0 && dl_depth_bias(1, 16) == dl_bits_of(-1.0f / 65536.0f) && dl_depth_bias(1, 0) == dl_bits_of(-1.0f / 16777216.0f));
  // The handle and the texture map blend.
  CHECK(dl_render_state(2, DL_RS_TEXTUREHANDLE, 5, &ctx, &o) == DL_OK && o.count == 1 && has_op(o, DL_SET_TEXTURE, 0, 0, 5));
  CHECK(dl_render_state(7, DL_RS_TEXTUREHANDLE, 5, &ctx, &o) == DL_DDERR_INVALIDPARAMS && o.count == 0);
  CHECK(dl_render_state(7, DL_RS_TEXTUREMAPBLEND, 2, &ctx, &o) == DL_DDERR_INVALIDPARAMS);
  CHECK(dl_render_state(3, DL_RS_TEXTUREMAPBLEND, 9, &ctx, &o) == DL_DDERR_INVALIDPARAMS && dl_render_state(3, DL_RS_TEXTUREMAPBLEND, 0, &ctx, &o) == DL_DDERR_INVALIDPARAMS);
  // Kept by the device, nothing for Direct3D 9.
  for (uint32_t s : {4u, 10u, 11u, 12u, 13u, 30u, 31u, 32u, 33u, 39u, 41u, 50u, 51u, 64u, 80u, 95u, 138u, 144u, 42u, 61u, 100u, 149u, 255u}) {
    CHECK(dl_render_state(2, s, 1, &ctx, &o) == DL_OK && o.count == 0);
    CHECK(dl_render_state(7, s, 1, &ctx, &o) == DL_OK && o.count == 0);
  }
  // Refused.
  for (uint32_t s : {0u, 256u, 0x7FFFFFFFu}) CHECK(dl_render_state(3, s, 0, &ctx, &o) == DL_DDERR_INVALIDPARAMS);
  // Every state number with every version: no more ops than there is room for.
  for (uint32_t s = 0; s < 300; ++s)
    for (unsigned version : {1u, 2u, 3u, 7u}) {
      dl_render_state(version, s, random_number(), &ctx, &o);
      CHECK(o.count <= 12);
    }
}

static void test_texture_map_blend() {
  DlStateOps o;
  struct Want { uint32_t cop, c1, c2, aop, a1, a2; };
  const Want with_alpha[9] = {{},
    {DL_TOP_SELECTARG1, DL_TA_TEXTURE, DL_TA_CURRENT, DL_TOP_SELECTARG1, DL_TA_TEXTURE, DL_TA_CURRENT},      // DECAL
    {DL_TOP_MODULATE, DL_TA_TEXTURE, DL_TA_CURRENT, DL_TOP_SELECTARG1, DL_TA_TEXTURE, DL_TA_CURRENT},        // MODULATE (texture alpha)
    {DL_TOP_BLENDTEXTUREALPHA, DL_TA_TEXTURE, DL_TA_CURRENT, DL_TOP_SELECTARG2, DL_TA_TEXTURE, DL_TA_CURRENT},   // DECALALPHA
    {DL_TOP_MODULATE, DL_TA_TEXTURE, DL_TA_CURRENT, DL_TOP_MODULATE, DL_TA_TEXTURE, DL_TA_CURRENT},          // MODULATEALPHA
    {DL_TOP_BLENDTEXTUREALPHA, DL_TA_TEXTURE, DL_TA_CURRENT, DL_TOP_SELECTARG2, DL_TA_TEXTURE, DL_TA_CURRENT},   // DECALMASK
    {DL_TOP_BLENDTEXTUREALPHA, DL_TA_TEXTURE, DL_TA_CONSTANT, DL_TOP_SELECTARG2, DL_TA_TEXTURE, DL_TA_CURRENT},  // MODULATEMASK
    {DL_TOP_SELECTARG1, DL_TA_TEXTURE, DL_TA_CURRENT, DL_TOP_SELECTARG1, DL_TA_TEXTURE, DL_TA_CURRENT},      // COPY
    {DL_TOP_ADD, DL_TA_TEXTURE, DL_TA_CURRENT, DL_TOP_SELECTARG2, DL_TA_TEXTURE, DL_TA_CURRENT}};            // ADD
  for (uint32_t mode = 1; mode <= 8; ++mode) {
    CHECK(dl_texture_map_blend(mode, 0, 2, &o) == DL_OK);
    const Want& w = with_alpha[mode];
    CHECK(has_op(o, DL_SET_STAGE, 0, DL_TSS9_COLOROP, w.cop) && has_op(o, DL_SET_STAGE, 0, DL_TSS9_COLORARG1, w.c1) && has_op(o, DL_SET_STAGE, 0, DL_TSS9_COLORARG2, w.c2) &&
          has_op(o, DL_SET_STAGE, 0, DL_TSS9_ALPHAOP, w.aop) && has_op(o, DL_SET_STAGE, 0, DL_TSS9_ALPHAARG1, w.a1) && has_op(o, DL_SET_STAGE, 0, DL_TSS9_ALPHAARG2, w.a2));
    CHECK(o.count == (mode == DL_TBLEND_MODULATEMASK ? 12u : 6u));
    // no texture: the diffuse colour as it is
    CHECK(dl_texture_map_blend(mode, 0, 0, &o) == DL_OK && has_op(o, DL_SET_STAGE, 0, DL_TSS9_COLOROP, DL_TOP_SELECTARG2) &&
          has_op(o, DL_SET_STAGE, 0, DL_TSS9_ALPHAOP, DL_TOP_SELECTARG2) && has_op(o, DL_SET_STAGE, 0, DL_TSS9_COLORARG2, DL_TA_CURRENT) && o.count == 6);
  }
  // MODULATE of a texture without alpha: the diffuse alpha.
  CHECK(dl_texture_map_blend(DL_TBLEND_MODULATE, 0, 1, &o) == DL_OK && has_op(o, DL_SET_STAGE, 0, DL_TSS9_ALPHAOP, DL_TOP_SELECTARG2));
  // MODULATEMASK's second stage, and its end.
  CHECK(dl_texture_map_blend(DL_TBLEND_MODULATEMASK, 0, 2, &o) == DL_OK && has_op(o, DL_SET_STAGE, 0, DL_TSS9_CONSTANT, 0xFFFFFFFFu) &&
        has_op(o, DL_SET_STAGE, 1, DL_TSS9_COLOROP, DL_TOP_MODULATE) && has_op(o, DL_SET_STAGE, 1, DL_TSS9_COLORARG2, DL_TA_DIFFUSE));
  CHECK(dl_texture_map_blend(DL_TBLEND_DECAL, DL_TBLEND_MODULATEMASK, 2, &o) == DL_OK && o.count == 8 && has_op(o, DL_SET_STAGE, 1, DL_TSS9_COLOROP, DL_TOP_DISABLE) &&
        has_op(o, DL_SET_STAGE, 1, DL_TSS9_ALPHAOP, DL_TOP_DISABLE));
  // A reference evaluation of each mode on one texel and colour, through what the stage states say (one stage, BLENDTEXTUREALPHA = lerp by At).
  const double ct[4] = {0.8, 0.4, 0.2, 1.0}, cf[4] = {0.5, 0.5, 1.0, 0.25};
  auto argument = [&](uint32_t a, int i) { return a == DL_TA_TEXTURE ? ct[i] : a == DL_TA_CONSTANT ? 1.0 : cf[i]; };
  auto evaluate = [&](uint32_t op, uint32_t a1, uint32_t a2, int i) {
    const double x = argument(a1, i), y = argument(a2, i);
    switch (op) {
    case DL_TOP_SELECTARG1: return x;
    case DL_TOP_SELECTARG2: return y;
    case DL_TOP_MODULATE: return x * y;
    case DL_TOP_ADD: return std::min(1.0, x + y);
    case DL_TOP_BLENDTEXTUREALPHA: return x * ct[3] + y * (1 - ct[3]);
    default: return -1.0;
    }
  };
  const double expect_rgb[9][3] = {{}, {0.8, 0.4, 0.2}, {0.4, 0.2, 0.2}, {0.8, 0.4, 0.2}, {0.4, 0.2, 0.2}, {0.8, 0.4, 0.2}, {0.4, 0.2, 0.2}, {0.8, 0.4, 0.2}, {1.0, 0.9, 1.0}};
  const double expect_alpha[9] = {0, 1.0, 1.0, 0.25, 0.25, 0.25, 0.25, 1.0, 0.25};
  for (uint32_t mode = 1; mode <= 8; ++mode) {
    const Want& w = with_alpha[mode];
    double rgb[3];
    for (int i = 0; i < 3; ++i) rgb[i] = evaluate(w.cop, w.c1, w.c2, i);
    if (mode == DL_TBLEND_MODULATEMASK)
      for (int i = 0; i < 3; ++i) rgb[i] *= cf[i];      // (stage 1)
    const double alpha = evaluate(w.aop, w.a1, w.a2, 3);
    for (int i = 0; i < 3; ++i) CHECK(near(rgb[i], expect_rgb[mode][i]));
    CHECK(near(alpha, expect_alpha[mode]));
  }
  CHECK(dl_texture_map_blend(0, 0, 2, &o) == DL_DDERR_INVALIDPARAMS && dl_texture_map_blend(9, 0, 2, &o) == DL_DDERR_INVALIDPARAMS);
}

static void test_state_defaults() {
  uint32_t v;
  CHECK(dl_render_state_default(1, DL_RS_COLORKEYENABLE, 0, &v) && v == 1);
  CHECK(dl_render_state_default(2, DL_RS_COLORKEYENABLE, 0, &v) && v == 0);
  CHECK(dl_render_state_default(2, DL_RS_SPECULARENABLE, 0, &v) && v == 1);
  CHECK(dl_render_state_default(3, DL_RS_SPECULARENABLE, 0, &v) && v == 0);
  CHECK(dl_render_state_default(7, DL_RS_SPECULARENABLE, 0, &v) && v == 0);
  CHECK(dl_render_state_default(3, DL_RS_TEXTUREMAPBLEND, 0, &v) && v == DL_TBLEND_MODULATE);
  CHECK(!dl_render_state_default(7, DL_RS_TEXTUREMAPBLEND, 0, &v) && v == 0);
  CHECK(!dl_render_state_default(7, DL_RS_TEXTUREHANDLE, 0, &v) && dl_render_state_default(2, DL_RS_TEXTUREHANDLE, 0, &v) && v == 0);
  CHECK(dl_render_state_default(7, DL_RS_ZENABLE, 1, &v) && v == 1 && dl_render_state_default(7, DL_RS_ZENABLE, 0, &v) && v == 0);
  CHECK(dl_render_state_default(3, 8, 0, &v) && v == 3 && dl_render_state_default(3, 9, 0, &v) && v == 2 && dl_render_state_default(3, 22, 0, &v) && v == 3);
  CHECK(dl_render_state_default(3, 23, 0, &v) && v == 4 && dl_render_state_default(3, 19, 0, &v) && v == 2 && dl_render_state_default(3, 20, 0, &v) && v == 1);
  CHECK(dl_render_state_default(7, 37, 0, &v) && v == dl_bits_of(1.0f) && dl_render_state_default(7, 36, 0, &v) && v == 0);
  CHECK(dl_render_state_default(7, 143, 0, &v) && v == 0 && dl_render_state_default(3, 143, 0, &v) && v == 0xFFFFFFFFu);
  CHECK(dl_render_state_default(7, 137, 0, &v) && v == 1 && dl_render_state_default(7, 142, 0, &v) && v == 1 && dl_render_state_default(7, 141, 0, &v) && v == 1);
  CHECK(dl_render_state_default(7, 145, 0, &v) && v == 1 && dl_render_state_default(7, 146, 0, &v) && v == 2 && dl_render_state_default(7, 147, 0, &v) && v == 0);
  CHECK(dl_render_state_default(7, 58, 0, &v) && v == 0xFFFFFFFFu && dl_render_state_default(7, 56, 0, &v) && v == 8 && dl_render_state_default(7, 60, 0, &v) && v == 0xFFFFFFFFu);
  CHECK(dl_render_state_default(2, DL_RS_TEXTUREMIN, 0, &v) && v == DL_FILTER_NEAREST && dl_render_state_default(2, DL_RS_TEXTUREADDRESS, 0, &v) && v == 1);
  CHECK(dl_render_state_default(2, DL_RS_PLANEMASK, 0, &v) && v == 0xFFFFFFFFu && dl_render_state_default(2, DL_RS_ROP2, 0, &v) && v == 13);
  CHECK(!dl_render_state_default(2, 0, 0, &v) && !dl_render_state_default(2, 42, 0, &v) && !dl_render_state_default(2, 256, 0, &v) && !dl_render_state_default(7, 149, 0, &v));
  // light states
  CHECK(dl_light_state_default(DL_LIGHTSTATE_COLORMODEL, &v) && v == DL_COLOR_RGB);
  CHECK(dl_light_state_default(DL_LIGHTSTATE_FOGEND, &v) && v == dl_bits_of(1.0f) && dl_light_state_default(DL_LIGHTSTATE_COLORVERTEX, &v) && v == 1);
  CHECK(!dl_light_state_default(0, &v) && !dl_light_state_default(9, &v));
  // the CPU lighting's starting point
  DlLighting l;
  dl_lighting_init(&l, 2);
  CHECK(l.legacy && !l.enabled && l.specular && l.normalize && l.local_viewer && l.fog_end == 1.0f);
  dl_lighting_init(&l, 3);
  CHECK(!l.specular);
  // texture stage defaults
  CHECK(dl_texture_stage_default(0, 1) == DL_TOP_MODULATE && dl_texture_stage_default(1, 1) == DL_TOP_DISABLE && dl_texture_stage_default(0, 4) == DL_TOP_SELECTARG1);
  CHECK(dl_texture_stage_default(3, 11) == 3 && dl_texture_stage_default(2, DL_TSS_MIPFILTER) == DL_TFP_NONE && dl_texture_stage_default(0, DL_TSS_MAXANISOTROPY) == 1);
  CHECK(dl_texture_stage_default(0, 2) == DL_TA_TEXTURE && dl_texture_stage_default(0, 6) == DL_TA_CURRENT && dl_texture_stage_default(0, 24) == 0);
}

static void test_other_states() {
  DlStateOps o;
  // Light states.
  CHECK(dl_light_state(DL_LIGHTSTATE_MATERIAL, 3, &o) == DL_OK && o.count == 1 && has_op(o, DL_SET_MATERIAL, 0, 0, 3));
  CHECK(dl_light_state(DL_LIGHTSTATE_AMBIENT, 0x00102030u, &o) == DL_OK && has_op(o, DL_SET_RENDER, 0, DL_RS9_AMBIENT, 0x00102030u));
  CHECK(dl_light_state(DL_LIGHTSTATE_FOGMODE, DL_FOG_LINEAR, &o) == DL_OK && has_op(o, DL_SET_RENDER, 0, DL_RS9_FOGVERTEXMODE, DL_FOG_LINEAR));
  CHECK(dl_light_state(DL_LIGHTSTATE_FOGSTART, 1, &o) == DL_OK && has_op(o, DL_SET_RENDER, 0, DL_RS9_FOGSTART, 1));
  CHECK(dl_light_state(DL_LIGHTSTATE_FOGEND, 2, &o) == DL_OK && has_op(o, DL_SET_RENDER, 0, DL_RS9_FOGEND, 2));
  CHECK(dl_light_state(DL_LIGHTSTATE_FOGDENSITY, 3, &o) == DL_OK && has_op(o, DL_SET_RENDER, 0, DL_RS9_FOGDENSITY, 3));
  CHECK(dl_light_state(DL_LIGHTSTATE_COLORVERTEX, 0, &o) == DL_OK && has_op(o, DL_SET_RENDER, 0, DL_RS9_COLORVERTEX, 0));
  CHECK(dl_light_state(DL_LIGHTSTATE_COLORMODEL, DL_COLOR_RGB, &o) == DL_OK && o.count == 0);
  CHECK(dl_light_state(DL_LIGHTSTATE_COLORMODEL, DL_COLOR_MONO, &o) == DL_OK && o.count == 0);
  CHECK(dl_light_state(DL_LIGHTSTATE_COLORMODEL, 3, &o) == DL_DDERR_INVALIDPARAMS);
  CHECK(dl_light_state(0, 0, &o) == DL_DDERR_INVALIDPARAMS && dl_light_state(9, 0, &o) == DL_DDERR_INVALIDPARAMS);
  // Texture stage states: the sampler states with their filters.
  CHECK(dl_texture_stage_state(2, DL_TSS_ADDRESS, 3, &o) == DL_OK && o.count == 2 && has_op(o, DL_SET_SAMPLER, 2, DL_SAMP9_ADDRESSU, 3) &&
        has_op(o, DL_SET_SAMPLER, 2, DL_SAMP9_ADDRESSV, 3));
  const uint32_t to_sampler[][2] = {{DL_TSS_ADDRESSU, DL_SAMP9_ADDRESSU}, {DL_TSS_ADDRESSV, DL_SAMP9_ADDRESSV}, {DL_TSS_BORDERCOLOR, DL_SAMP9_BORDERCOLOR},
                                    {DL_TSS_MINFILTER, DL_SAMP9_MINFILTER}, {DL_TSS_MIPMAPLODBIAS, DL_SAMP9_MIPMAPLODBIAS}, {DL_TSS_MAXMIPLEVEL, DL_SAMP9_MAXMIPLEVEL},
                                    {DL_TSS_MAXANISOTROPY, DL_SAMP9_MAXANISOTROPY}};
  for (const auto& m : to_sampler) CHECK(dl_texture_stage_state(7, m[0], 2, &o) == DL_OK && o.count == 1 && has_op(o, DL_SET_SAMPLER, 7, m[1], 2));
  const uint32_t mag[][2] = {{DL_TFG_POINT, DL_TEXF9_POINT}, {DL_TFG_LINEAR, DL_TEXF9_LINEAR}, {DL_TFG_FLATCUBIC, DL_TEXF9_PYRAMIDALQUAD},
                             {DL_TFG_GAUSSIANCUBIC, DL_TEXF9_GAUSSIANQUAD}, {DL_TFG_ANISOTROPIC, DL_TEXF9_ANISOTROPIC}, {0, DL_TEXF9_POINT}, {9, DL_TEXF9_POINT}};
  for (const auto& m : mag) {
    CHECK(dl_texture_stage_state(0, DL_TSS_MAGFILTER, m[0], &o) == DL_OK && has_op(o, DL_SET_SAMPLER, 0, DL_SAMP9_MAGFILTER, m[1]));
    if (m[0] >= 1 && m[0] <= 5) CHECK(dl_tfg_from_d3d9(m[1]) == m[0]);
  }
  const uint32_t mip[][2] = {{DL_TFP_NONE, DL_TEXF9_NONE}, {DL_TFP_POINT, DL_TEXF9_POINT}, {DL_TFP_LINEAR, DL_TEXF9_LINEAR}, {0, DL_TEXF9_LINEAR}, {7, DL_TEXF9_NONE}};
  for (const auto& m : mip) {
    CHECK(dl_texture_stage_state(1, DL_TSS_MIPFILTER, m[0], &o) == DL_OK && has_op(o, DL_SET_SAMPLER, 1, DL_SAMP9_MIPFILTER, m[1]));
    if (m[0] >= 1 && m[0] <= 3) CHECK(dl_tfp_from_d3d9(m[1]) == m[0]);
  }
  for (uint32_t s : {1u, 2u, 3u, 4u, 5u, 6u, 7u, 10u, 11u, 22u, 23u, 24u})
    CHECK(dl_texture_stage_state(3, s, 9, &o) == DL_OK && o.count == 1 && has_op(o, DL_SET_STAGE, 3, s, 9));
  CHECK(dl_texture_stage_state(3, 0, 9, &o) == DL_OK && o.count == 0 && dl_texture_stage_state(3, 25, 9, &o) == DL_OK && o.count == 0);
  CHECK(dl_texture_stage_state(8, 1, 1, &o) == DL_DDERR_INVALIDPARAMS);
  // Transform states.
  CHECK(dl_transform_state(1, DL_TRANSFORMSTATE_WORLD) == 256 && dl_transform_state(2, DL_TRANSFORMSTATE_VIEW) == 2 && dl_transform_state(3, DL_TRANSFORMSTATE_PROJECTION) == 3);
  CHECK(dl_transform_state(3, DL_TRANSFORMSTATE_WORLD1) == 0 && dl_transform_state(7, DL_TRANSFORMSTATE_WORLD1) == 257 && dl_transform_state(7, 6) == 259);
  CHECK(dl_transform_state(7, 16) == 16 && dl_transform_state(7, 23) == 23 && dl_transform_state(7, 7) == 0 && dl_transform_state(7, 24) == 0 && dl_transform_state(7, 0) == 0);
}

// ---- conversions ------------------------------------------------------------------------------------------------------------------------------

static void test_lights_and_materials() {
  DlLightEntry e;
  memset(&e, 0, sizeof e);
  e.type = DL_LIGHT_POINT;
  e.diffuse[0] = 1; e.specular[1] = 0.5f;
  e.position[0] = 1; e.position[1] = 2; e.position[2] = 3;
  e.range = 10;
  e.attenuation[0] = 0.5f;
  DlLight7 out;
  CHECK(dl_light7_from_legacy(&e, &out) == 1 && out.dltType == DL_LIGHT_POINT && out.dvAttenuation0 == 2.0f && out.dcvDiffuse.r == 1.0f && out.dcvSpecular.g == 0.5f &&
        out.dcvAmbient.r == 0.0f && out.dvRange == 10.0f && out.dvPosition.z == 3.0f);
  e.attenuation[1] = 1;
  CHECK(dl_light7_from_legacy(&e, &out) == 0);      // (attenuation over the range: not d3d9's)
  e.type = DL_LIGHT_PARALLELPOINT;
  CHECK(dl_light7_from_legacy(&e, &out) == 0 && out.dltType == DL_LIGHT_DIRECTIONAL && out.dvDirection.x == -1.0f && out.dvDirection.z == -3.0f);
  e.type = DL_LIGHT_DIRECTIONAL;
  CHECK(dl_light7_from_legacy(&e, &out) == 1 && out.dvAttenuation0 == 1.0f);
  DlMaterial m;
  memset(&m, 0, sizeof m);
  m.dwSize = 80;
  m.diffuse = {1, 0.5f, 0.25f, 0.125f};
  m.emissive = {0.1f, 0.2f, 0.3f, 0.4f};
  m.power = 12;
  m.hTexture = 5;
  DlMaterial7 m7;
  CHECK(dl_material7_from_legacy(&m, &m7) == DL_OK && m7.diffuse.a == 0.125f && m7.emissive.b == 0.3f && m7.power == 12.0f);
  m.dwSize = 68;
  CHECK(dl_material7_from_legacy(&m, &m7) == DL_DDERR_INVALIDPARAMS && dl_material7_from_legacy(nullptr, &m7) == DL_DDERR_INVALIDPARAMS);
}

static void test_fvf() {
  // Strides and offsets, worked out by hand.
  struct Case { uint32_t fvf; uint32_t stride; };
  const Case cases[] = {
    {DL_FVF_VERTEX, 32}, {DL_FVF_LVERTEX, 32}, {DL_FVF_TLVERTEX, 32}, {DL_FVF_XYZ, 12}, {DL_FVF_XYZRHW, 16}, {DL_FVF_XYZB1, 16}, {DL_FVF_XYZB5, 32},
    {DL_FVF_XYZ | DL_FVF_NORMAL | DL_FVF_DIFFUSE | DL_FVF_SPECULAR | 0x800, 12 + 12 + 4 + 4 + 64},
    {DL_FVF_XYZ | 0x200 | (1u << 16) | (2u << 18), 12 + 12 + 16},       // (set 0 three numbers, set 1 four)
    {DL_FVF_XYZ | 0x300 | (3u << 16) | (0u << 18) | (2u << 20), 12 + 4 + 8 + 16},
    {0x00C | DL_FVF_NORMAL | 0x100, 12 + 16 + 12 + 8}, {0x00A | 0x100, 12 + 12 + 8},      // (XYZB4, XYZB3)
  };
  for (const Case& c : cases) CHECK(dl_fvf_stride(c.fvf) == c.stride);
  DlFvfLayout l;
  CHECK(dl_fvf_layout(DL_FVF_LVERTEX, &l) && l.reserved == 12 && l.diffuse == 16 && l.specular == 20 && l.tex[0] == 24 && l.normal == -1);
  CHECK(dl_fvf_layout(DL_FVF_XYZB5 | DL_FVF_NORMAL, &l) && l.blend == 12 && l.blend_weights == 5 && l.normal == 32 && l.position_size == 32);
  CHECK(dl_fvf_layout(DL_FVF_XYZ | 0x300 | (3u << 16) | (2u << 20), &l) && l.tex_size[0] == 1 && l.tex_size[1] == 2 && l.tex_size[2] == 4 && l.tex[2] == 24);
  // Refused codes.
  for (uint32_t bad : {0u, 0x001u | DL_FVF_XYZ, DL_FVF_XYZ | 0x1000u, DL_FVF_XYZ | 0x900u, DL_FVF_XYZ | 0x100u | (1u << 18), DL_FVF_NORMAL | 0x100u})
    CHECK(!dl_fvf_valid(bad, 7) && dl_fvf_stride(bad) == 0);
  CHECK(dl_fvf_valid(DL_FVF_XYZ | 0x100 | (1u << 16), 7) && !dl_fvf_valid(DL_FVF_XYZ | 0x100 | (1u << 16), 3));
  CHECK(dl_fvf_valid(DL_FVF_LVERTEX, 3) && dl_fvf_valid(DL_FVF_XYZ | 0x800, 7));
  // Every code with the low 16 bits: never a stride beyond the largest, and valid exactly when it has a layout.
  for (uint32_t fvf = 0; fvf < 0x10000; ++fvf) {
    const uint32_t s = dl_fvf_stride(fvf);
    CHECK(s <= 32 + 12 + 4 + 4 + 4 + 8 * 16);
    CHECK((s != 0) == (dl_fvf_valid(fvf, 7) != 0));
  }
  // D3DVERTEXTYPE.
  CHECK(dl_fvf_of_vertex_type(DL_VT_VERTEX) == 0x112 && dl_fvf_of_vertex_type(DL_VT_LVERTEX) == 0x1E2 && dl_fvf_of_vertex_type(DL_VT_TLVERTEX) == 0x1C4 &&
        dl_fvf_of_vertex_type(0) == 0 && dl_fvf_of_vertex_type(4) == 0);
  // Vertex elements: a D3DLVERTEX skips its reserved word; a transformed vertex is POSITIONT.
  DlVertexElement e[DL_MAX_ELEMENTS];
  unsigned n = dl_fvf_elements(DL_FVF_LVERTEX, e);
  CHECK(n == 5 && e[0].usage == DL_USAGE_POSITION && e[0].type == DL_DECLTYPE_FLOAT3 && e[1].offset == 16 && e[1].usage == DL_USAGE_COLOR && e[1].usage_index == 0 &&
        e[2].offset == 20 && e[2].usage_index == 1 && e[3].offset == 24 && e[3].usage == DL_USAGE_TEXCOORD && e[3].type == DL_DECLTYPE_FLOAT2 && e[4].stream == 0xFF &&
        e[4].type == DL_DECLTYPE_UNUSED);
  n = dl_fvf_elements(DL_FVF_TLVERTEX, e);
  CHECK(n == 5 && e[0].usage == DL_USAGE_POSITIONT && e[0].type == DL_DECLTYPE_FLOAT4);
  n = dl_fvf_elements(0x00C | DL_FVF_NORMAL | 0x300 | (3u << 16) | (2u << 20), e);
  CHECK(n == 7 && e[1].usage == DL_USAGE_BLENDWEIGHT && e[1].type == DL_DECLTYPE_FLOAT4 && e[2].offset == 28 && e[3].type == DL_DECLTYPE_FLOAT1 && e[4].type == DL_DECLTYPE_FLOAT2 &&
        e[5].type == DL_DECLTYPE_FLOAT4 && e[5].usage_index == 2);
  CHECK(dl_fvf_elements(0, e) == 0);
  for (uint32_t fvf = 0; fvf < 0x10000; fvf += 7) CHECK(dl_fvf_elements(fvf, e) <= DL_MAX_ELEMENTS);
  // Primitive counts.
  CHECK(dl_primitive_count(DL_PT_POINTLIST, 7) == 7 && dl_primitive_count(DL_PT_LINELIST, 7) == 3 && dl_primitive_count(DL_PT_LINESTRIP, 7) == 6 &&
        dl_primitive_count(DL_PT_TRIANGLELIST, 7) == 2 && dl_primitive_count(DL_PT_TRIANGLESTRIP, 7) == 5 && dl_primitive_count(DL_PT_TRIANGLEFAN, 7) == 5);
  CHECK(dl_primitive_count(DL_PT_LINESTRIP, 1) == 0 && dl_primitive_count(DL_PT_TRIANGLEFAN, 2) == 0 && dl_primitive_count(0, 9) == 0 && dl_primitive_count(7, 9) == 0);
  CHECK(dl_output_fvf(DL_FVF_VERTEX) == DL_FVF_TLVERTEX && dl_output_fvf(DL_FVF_XYZ | 0x200 | (1u << 18)) == (DL_FVF_XYZRHW | 0xC0 | 0x200 | (1u << 18)));
}

static void test_strided() {
  // Positions, normals and colours in separate arrays, two texture sets interleaved in one.
  const float positions[4][3] = {{1, 2, 3}, {4, 5, 6}, {7, 8, 9}, {10, 11, 12}};
  const float normals[4][3] = {{0, 0, 1}, {0, 1, 0}, {1, 0, 0}, {0, 0, -1}};
  const uint32_t colours[4] = {0x11, 0x22, 0x33, 0x44};
  const float tex[4][5] = {{0.1f, 0.2f, 9, 0.3f, 0.4f}, {1.1f, 1.2f, 9, 1.3f, 1.4f}, {2.1f, 2.2f, 9, 2.3f, 2.4f}, {3.1f, 3.2f, 9, 3.3f, 3.4f}};
  DlStridedData s;
  memset(&s, 0, sizeof s);
  s.position = {(void *)positions, 12};
  s.normal = {(void *)normals, 12};
  s.diffuse = {(void *)colours, 4};
  s.textureCoords[0] = {(void *)&tex[0][0], 20};
  s.textureCoords[1] = {(void *)&tex[0][3], 20};
  const uint32_t fvf = DL_FVF_XYZ | DL_FVF_NORMAL | DL_FVF_DIFFUSE | 0x200;
  std::vector<uint8_t> out(3 * dl_fvf_stride(fvf));
  CHECK(dl_gather_strided(fvf, &s, 1, 3, 4, dl_bytes(out.data(), out.size())) == DL_OK);
  float v[11];
  memcpy(v, out.data() + 2 * 44, 44);
  uint32_t colour;
  memcpy(&colour, &v[6], 4);
  CHECK(v[0] == 10.0f && v[5] == -1.0f && colour == 0x44 && v[7] == 3.1f && v[8] == 3.2f && v[9] == 3.3f && v[10] == 3.4f);
  // A stride of 0 repeats the one vertex.
  s.diffuse.dwStride = 0;
  CHECK(dl_gather_strided(fvf, &s, 0, 3, 4, dl_bytes(out.data(), out.size())) == DL_OK);
  memcpy(&colour, out.data() + 44 + 24, 4);
  CHECK(colour == 0x11);
  // Refusals: past what the pointers have, a missing pointer, an output too small, a bad code.
  CHECK(dl_gather_strided(fvf, &s, 2, 3, 4, dl_bytes(out.data(), out.size())) == DL_DDERR_INVALIDPARAMS);
  CHECK(dl_gather_strided(fvf, &s, 0, 3, 4, dl_bytes(out.data(), out.size() - 1)) == DL_DDERR_INVALIDPARAMS);
  CHECK(dl_gather_strided(0x9, &s, 0, 1, 4, dl_bytes(out.data(), out.size())) == DL_DDERR_INVALIDPARAMS);
  s.normal.lpvData = nullptr;
  CHECK(dl_gather_strided(fvf, &s, 0, 1, 4, dl_bytes(out.data(), out.size())) == DL_DDERR_INVALIDPARAMS);
  CHECK(dl_gather_strided(fvf, nullptr, 0, 1, 4, dl_bytes(out.data(), out.size())) == DL_DDERR_INVALIDPARAMS);
  // D3DLVERTEX's reserved word comes out zero.
  s.normal.lpvData = (void *)normals;
  s.specular = {(void *)colours, 4};
  s.diffuse.dwStride = 4;
  std::vector<uint8_t> lv(2 * 32, 0xEE);
  CHECK(dl_gather_strided(DL_FVF_LVERTEX, &s, 0, 2, 4, dl_bytes(lv.data(), lv.size())) == DL_OK);
  DlLVertex l1;
  memcpy(&l1, lv.data() + 32, 32);
  CHECK(l1.x == 4.0f && l1.reserved == 0 && l1.color == 0x22 && l1.specular == 0x22 && l1.tu == 1.1f && l1.tv == 1.2f);
}

static void test_process_fvf() {
  DlTransform t;
  dl_transform_init(&t);
  DlViewport7 vp = {0, 0, 100, 100, 0, 1};
  t.viewport = vp;
  dl_transform_prepare(&t);
  DlLighting l;
  dl_lighting_init(&l, 3);
  l.enabled = 1;
  l.material.diffuse = {1, 1, 1, 1};
  l.lights = 1;
  l.light[0].type = DL_LIGHT_DIRECTIONAL;
  l.light[0].diffuse[0] = 1;
  l.light[0].direction[2] = 1;      // (pointing away from the camera, onto normals facing it)
  // XYZ | NORMAL | TEX1 and XYZ | DIFFUSE | TEX2
  const float lit[2][8] = {{0.5f, 0.5f, 0.5f, 0, 0, -1, 0.25f, 0.75f}, {-0.5f, 0, 0.25f, 0, 0, 1, 0, 0}};
  std::vector<DlClipVertex> cv(2);
  DlProcessResult r;
  dl_result_init(&r);
  CHECK(dl_process_fvf(&t, &l, 1, DL_FVF_VERTEX, dl_const_bytes(lit, sizeof lit), 2, cv.data(), cv.size(), &r) == DL_OK);
  CHECK(near(cv[0].diffuse[0], 1.0) && cv[0].diffuse[1] == 0.0f && cv[1].diffuse[0] == 0.0f && r.count == 2 && r.clip_union == 0);
  std::vector<uint8_t> out(2 * 32);
  CHECK(dl_project_fvf(&t, DL_FVF_VERTEX, cv.data(), 2, dl_bytes(out.data(), out.size())) == DL_OK);
  DlTlVertex tl;
  memcpy(&tl, out.data(), 32);
  CHECK(tl.sx == 75.0f && tl.sy == 25.0f && tl.sz == 0.5f && tl.color == 0xFFFF0000u && tl.tu == 0.25f && tl.tv == 0.75f);
  // not lit: D3DDP_DONOTLIGHT, or no normal
  CHECK(dl_process_fvf(&t, &l, 0, DL_FVF_VERTEX, dl_const_bytes(lit, sizeof lit), 2, cv.data(), cv.size(), nullptr) == DL_OK && cv[0].diffuse[1] == 1.0f);
  struct DiffuseVertex { float x, y, z; uint32_t c; float t[4]; } dv[1] = {{0, 0, 0.5f, 0x80FF0000u, {1, 2, 3, 4}}};
  const uint32_t dfvf = DL_FVF_XYZ | DL_FVF_DIFFUSE | 0x200;
  CHECK(dl_process_fvf(&t, &l, 1, dfvf, dl_const_bytes(dv, sizeof dv), 1, cv.data(), cv.size(), nullptr) == DL_OK && near(cv[0].diffuse[3], 128.0 / 255.0) &&
        cv[0].diffuse[0] == 1.0f && cv[0].tex[1][1] == 4.0f);
  std::vector<uint8_t> o2(dl_fvf_stride(dl_output_fvf(dfvf)));
  CHECK(dl_project_fvf(&t, dfvf, cv.data(), 1, dl_bytes(o2.data(), o2.size())) == DL_OK);
  float second[2];
  memcpy(second, o2.data() + 24 + 8, 8);
  CHECK(second[0] == 3.0f && second[1] == 4.0f);
  // transformed vertices come through as they are
  const DlTlVertex pre[1] = {{30, 40, 0.25f, 0.5f, 0xFF00FF00u, 0, 1, 2}};
  CHECK(dl_process_fvf(&t, &l, 1, DL_FVF_TLVERTEX, dl_const_bytes(pre, sizeof pre), 1, cv.data(), cv.size(), nullptr) == DL_OK);
  CHECK(dl_project_fvf(&t, DL_FVF_TLVERTEX, cv.data(), 1, dl_bytes(out.data(), 32)) == DL_OK);
  memcpy(&tl, out.data(), 32);
  CHECK(near(tl.sx, 30) && near(tl.sy, 40) && near(tl.sz, 0.25) && near(tl.rhw, 0.5) && tl.color == 0xFF00FF00u && tl.tv == 2.0f);
  // refusals
  CHECK(dl_process_fvf(&t, &l, 1, DL_FVF_VERTEX, dl_const_bytes(lit, sizeof lit - 1), 2, cv.data(), cv.size(), nullptr) == DL_DDERR_INVALIDPARAMS);
  CHECK(dl_process_fvf(&t, &l, 1, DL_FVF_VERTEX, dl_const_bytes(lit, sizeof lit), 2, cv.data(), 1, nullptr) == DL_DDERR_INVALIDPARAMS);
  CHECK(dl_process_fvf(&t, &l, 1, 0x1001, dl_const_bytes(lit, sizeof lit), 1, cv.data(), 1, nullptr) == DL_D3DERR_INVALIDVERTEXFORMAT);
  CHECK(dl_project_fvf(&t, DL_FVF_VERTEX, cv.data(), 2, dl_bytes(out.data(), 63)) == DL_DDERR_INVALIDPARAMS);
}

static void test_execute_process_ops() {
  // TRANSFORM (D3DLVERTEX) keeps its colours; COPY copies; NOCOLOR keeps the destination's; ranges are checked.
  Buffer b;
  b.f32(0); b.f32(0); b.f32(0.5f); b.u32(0); b.u32(0x80112233u); b.u32(0x44556677u); b.f32(0.5f); b.f32(0.25f);
  b.f32(10); b.f32(20); b.f32(0.75f); b.f32(0.5f); b.u32(0xFFAABBCCu); b.u32(0x01020304u); b.f32(1); b.f32(2);
  const DlExecuteData d = execute_data(0, 2, 64, 0);
  DlTransform t;
  dl_transform_init(&t);
  DlViewport1 vp = {44, 0, 0, 100, 100, 50, 50, 1, 1, 0, 1};
  dl_viewport7_from_v1(&vp, &t.viewport, &t.clip);
  dl_transform_prepare(&t);
  DlLighting l;
  dl_lighting_init(&l, 1);
  DlStatus status;
  memset(&status, 0, sizeof status);
  status.dwStatus = DL_STATUS_DEFAULT;
  std::vector<uint8_t> dst(4 * 32, 0);
  std::vector<DlClipVertex> clip(4);
  DlProcessVertices pv = {DL_PROCESSVERTICES_TRANSFORM, 0, 2, 1, 0};
  CHECK(dl_execute_process(&pv, dl_const_bytes(b.bytes.data(), b.size()), &d, dl_bytes(dst.data(), dst.size()), clip.data(), clip.size(), &t, &l, &status) == DL_OK);
  DlTlVertex v;
  memcpy(&v, dst.data() + 64, 32);
  CHECK(v.sx == 50.0f && v.sy == 50.0f && v.sz == 0.5f && v.color == 0x80112233u && v.specular == 0x44556677u && v.tu == 0.5f);
  CHECK((status.dwStatus & DL_STATUS_CLIPINTERSECTIONALL) == 0);
  pv = {DL_PROCESSVERTICES_COPY, 1, 0, 1, 0};
  status.dwStatus = 0x12;
  CHECK(dl_execute_process(&pv, dl_const_bytes(b.bytes.data(), b.size()), &d, dl_bytes(dst.data(), dst.size()), clip.data(), clip.size(), &t, &l, &status) == DL_OK);
  memcpy(&v, dst.data(), 32);
  CHECK(v.sx == 10.0f && v.sy == 20.0f && v.sz == 0.75f && v.rhw == 0.5f && v.color == 0xFFAABBCCu && status.dwStatus == 0x12 && clip[0].clip == 0);
  CHECK(near(clip[0].position[3], 2.0));      // (w back from rhw)
  pv = {DL_PROCESSVERTICES_TRANSFORM | DL_PROCESSVERTICES_NOCOLOR, 0, 0, 1, 0};
  CHECK(dl_execute_process(&pv, dl_const_bytes(b.bytes.data(), b.size()), &d, dl_bytes(dst.data(), dst.size()), clip.data(), clip.size(), &t, &l, &status) == DL_OK);
  memcpy(&v, dst.data(), 32);
  CHECK(v.sx == 50.0f && v.color == 0xFFAABBCCu && v.specular == 0x01020304u);
  // ranges and ops
  pv = {DL_PROCESSVERTICES_TRANSFORM, 1, 0, 2, 0};
  CHECK(dl_execute_process(&pv, dl_const_bytes(b.bytes.data(), b.size()), &d, dl_bytes(dst.data(), dst.size()), clip.data(), clip.size(), &t, &l, &status) == DL_RANGE);
  pv = {DL_PROCESSVERTICES_TRANSFORM, 0, 3, 2, 0};
  CHECK(dl_execute_process(&pv, dl_const_bytes(b.bytes.data(), b.size()), &d, dl_bytes(dst.data(), dst.size()), clip.data(), clip.size(), &t, &l, &status) == DL_RANGE);
  pv = {DL_PROCESSVERTICES_TRANSFORM, 0, 2, 2, 0};
  CHECK(dl_execute_process(&pv, dl_const_bytes(b.bytes.data(), b.size()), &d, dl_bytes(dst.data(), dst.size()), clip.data(), 3, &t, &l, &status) == DL_RANGE);
  pv = {3, 0, 0, 1, 0};
  CHECK(dl_execute_process(&pv, dl_const_bytes(b.bytes.data(), b.size()), &d, dl_bytes(dst.data(), dst.size()), clip.data(), clip.size(), &t, &l, &status) == DL_BAD);
  pv = {DL_PROCESSVERTICES_TRANSFORM, 0, 0, 1, 0};
  CHECK(dl_execute_process(&pv, dl_const_bytes(b.bytes.data(), b.size()), &d, dl_bytes(dst.data(), dst.size()), nullptr, 0, &t, &l, &status) == DL_OK);
  // The execute data says more vertices than the buffer has: dl_execute_vertex refuses, nothing past the buffer is read.
  DlExecuteData lying = execute_data(32, 2, 0, 0);
  pv = {DL_PROCESSVERTICES_TRANSFORM, 0, 0, 2, 0};
  CHECK(dl_execute_process(&pv, dl_const_bytes(b.bytes.data(), b.size()), &lying, dl_bytes(dst.data(), dst.size()), clip.data(), clip.size(), &t, &l, &status) == DL_RANGE);
}

static void test_sphere_visibility() {
  DlTransform t;
  dl_transform_init(&t);
  t.projection = perspective(1, 10, 1);      // (a 90 degree frustum from z 1 to 10)
  dl_transform_prepare(&t);
  const float inside[3] = {0, 0, 5}, left[3] = {-5, 0, 5}, far_left[3] = {-20, 0, 5}, near_plane[3] = {0, 0, 1}, behind[3] = {0, 0, -5};
  CHECK(dl_sphere_visibility7(&t, inside, 1) == 0);
  CHECK(dl_sphere_visibility7(&t, left, 1) == DL_CLIP_LEFT);
  CHECK(dl_sphere_visibility7(&t, far_left, 1) == (DL_CLIP_LEFT | (DL_CLIP_LEFT << 12)));
  CHECK(dl_sphere_visibility7(&t, behind, 1) & (DL_CLIP_FRONT << 12));
  CHECK(dl_sphere_visibility7(&t, near_plane, 0.5f) == DL_CLIP_FRONT);
  CHECK(dl_sphere_visibility3(&t, inside, 1) == 0);
  CHECK(dl_sphere_visibility3(&t, left, 1) == (DL_VIS_INTERSECT_LEFT | DL_VIS_INTERSECT_FRUSTUM));
  CHECK(dl_sphere_visibility3(&t, far_left, 1) == (DL_VIS_OUTSIDE_LEFT | DL_VIS_OUTSIDE_FRUSTUM));
  const uint32_t both = dl_sphere_visibility3(&t, behind, 1);
  CHECK((both & DL_VIS_OUTSIDE_FRUSTUM) && (both & (DL_VIS_OUTSIDE_LEFT << 8)));      // (outside the near plane)
  // touching: Direct3D 3 counts it as crossing, 7 does not (the distance from the left plane of a point at x = -5, z = 5 is 0).
  const float touching[3] = {-6, 0, 5};
  const float d = (float)(1.0 / sqrt(2.0));
  CHECK(dl_sphere_visibility3(&t, touching, d) & DL_VIS_INTERSECT_LEFT);
  // user planes (world space, moved by the world matrix, radius scaled)
  t.user_planes = 1;
  t.user_plane[0][0] = 1;      // (x >= 0)
  t.world.m[0][0] = t.world.m[1][1] = t.world.m[2][2] = 2;
  dl_transform_prepare(&t);
  const float c1[3] = {0.4f, 0, 2.5f}, c2[3] = {1.5f, 0, 2.5f}, c3[3] = {-1.5f, 0, 2.5f};
  CHECK(dl_sphere_visibility7(&t, c1, 0.5f) == DL_CLIP_GEN0);      // (x 0.8, radius 1)
  CHECK(dl_sphere_visibility7(&t, c2, 0.5f) == 0);
  CHECK(dl_sphere_visibility7(&t, c3, 0.5f) & (DL_CLIP_GEN0 << 12));
}

// ---- against Wine's ddraw -------------------------------------------------------------------------------------------------------------------

// The scenes of tests/data/ddraw/wine_scenes.h as Wine's ddraw.dll drew them (tests/data/ddraw/wine_reference.txt, made by wine_reference.c):
// IDirect3DVertexBuffer::ProcessVertices with a D3DVIEWPORT2 and legacy lights, and IDirect3DViewport3::TransformVertices with a D3DVIEWPORT.
static int channel(uint32_t c, int shift) { return (int)((c >> shift) & 0xFF); }
static void test_against_wine() {
  FILE *f = fopen("tests/data/ddraw/wine_reference.txt", "r");
  CHECK(f != nullptr);
  if (!f) return;
  char line[512];
  int processed = 0, transformed = 0, colour_off = 0, specular_off = 0, parallel = 0;
  while (fgets(line, sizeof line, f)) {
    char kind[16];
    int s, i;
    unsigned long hr;
    if (sscanf(line, "%15s %d %d %lx", kind, &s, &i, &hr) != 4 || s < 0 || s >= SCENE_COUNT || i < 0 || i >= SCENE_VERTICES) continue;
    Scene scene;
    scene_make(s, &scene);
    DlTransform t;
    dl_transform_init(&t);
    memcpy(t.world.m, scene.world.m, sizeof t.world.m);
    memcpy(t.view.m, scene.view.m, sizeof t.view.m);
    memcpy(t.projection.m, scene.projection.m, sizeof t.projection.m);
    if (!strcmp(kind, "process")) {
      float sx, sy, sz, rhw;
      unsigned long colour, specular;
      if (sscanf(line, "%*s %*d %*d %*x %f %f %f %f %lx %lx", &sx, &sy, &sz, &rhw, &colour, &specular) != 6) continue;
      DlViewport2 vp = {44, scene.x, scene.y, scene.width, scene.height, scene.clip[0], scene.clip[1], scene.clip[2], scene.clip[3], scene.clip[4], scene.clip[5]};
      dl_viewport7_from_v2(&vp, &t.viewport, &t.clip);
      dl_transform_prepare(&t);
      DlLighting l;
      dl_lighting_init(&l, 3);
      l.enabled = 1;
      l.specular = (int)scene.specular;
      memcpy(&l.material.diffuse, scene.material[0], 16);
      memcpy(&l.material.ambient, scene.material[1], 16);
      memcpy(&l.material.specular, scene.material[2], 16);
      memcpy(&l.material.emissive, scene.material[3], 16);
      l.material.power = scene.power;
      dl_lighting_apply_light_state(&l, DL_LIGHTSTATE_AMBIENT, scene.ambient);
      for (int k = 0; k < scene.lights; ++k) {
        DlLight2 in;
        memset(&in, 0, sizeof in);
        in.dwSize = DL_LIGHT2_SIZE;
        in.dltType = scene.light[k].type;
        memcpy(&in.dcvColor, scene.light[k].color, 16);
        memcpy(&in.dvPosition, scene.light[k].position, 12);
        memcpy(&in.dvDirection, scene.light[k].direction, 12);
        in.dvRange = scene.light[k].range;
        in.dvFalloff = scene.light[k].falloff;
        in.dvAttenuation0 = scene.light[k].attenuation[0];
        in.dvAttenuation1 = scene.light[k].attenuation[1];
        in.dvAttenuation2 = scene.light[k].attenuation[2];
        in.dvTheta = scene.light[k].theta;
        in.dvPhi = scene.light[k].phi;
        in.dwFlags = scene.light[k].flags;
        int active;
        CHECK(dl_light_from_legacy(&in, &l.light[l.lights], &active) == DL_OK && active);
        if (in.dltType == DL_LIGHT_PARALLELPOINT) {
          // Wine (wined3d) aims a parallel point light from the camera: normalize(the light's place in view space). Direct3D's documentation and
          // dl_light_vertex aim it from the origin of the object it lights; to compare the rest, the light is made Wine's here - a directional light
          // along camera -> light (the scenes' views do not rotate).
          DlLightEntry& e = l.light[l.lights];
          e.type = DL_LIGHT_DIRECTIONAL;
          for (int c = 0; c < 3; ++c) e.direction[c] = -(e.position[c] - t.eye[c]);
          ++parallel;
        }
        ++l.lights;
      }
      const float p[3] = {scene.vertex[i][0], scene.vertex[i][1], scene.vertex[i][2]}, n[3] = {scene.vertex[i][3], scene.vertex[i][4], scene.vertex[i][5]};
      const float tex[1][4] = {{scene.vertex[i][6], scene.vertex[i][7], 0, 1}};
      DlClipVertex cv;
      dl_process_vertex(&t, &l, 1, p, n, nullptr, nullptr, tex, 1, &cv);
      DlTlVertex tl;
      dl_tl_from_clip(&t, &cv, &tl);
      CHECK(hr == 0);
      CHECK(near(tl.sx, sx, 1e-4) && near(tl.sy, sy, 1e-4) && near(tl.sz, sz, 1e-4) && near(tl.rhw, rhw, 1e-4));
      int worst = 0, worst_specular = 0;
      for (int shift = 0; shift <= 24; shift += 8) {
        worst = std::max(worst, abs(channel(tl.color, shift) - channel((uint32_t)colour, shift)));
        if (shift < 24) worst_specular = std::max(worst_specular, abs(channel(tl.specular, shift) - channel((uint32_t)specular, shift)));
      }
      if (worst > 2) {
        ++colour_off;
        printf("  wine scene %d vertex %d: colour %08x, wine %08lx\n", s, i, tl.color, colour);
      }
      if (worst_specular > 2) {
        ++specular_off;
        printf("  wine scene %d vertex %d: specular %08x, wine %08lx\n", s, i, tl.specular, specular);
      }
      ++processed;
    } else if (!strcmp(kind, "transform")) {
      float sx, sy, sz, rhw, hx, hy, hz;
      unsigned long flags;
      if (sscanf(line, "%*s %*d %*d %*x %f %f %f %f %lx %f %f %f", &sx, &sy, &sz, &rhw, &flags, &hx, &hy, &hz) != 8) continue;
      DlViewport1 vp = {44, scene.x, scene.y, scene.width, scene.height, scene.scale[0], scene.scale[1], 1, 1, 0, 1};
      dl_viewport7_from_v1(&vp, &t.viewport, &t.clip);
      dl_transform_prepare(&t);
      DlLVertex in;
      memset(&in, 0, sizeof in);
      in.x = scene.vertex[i][0]; in.y = scene.vertex[i][1]; in.z = scene.vertex[i][2];
      DlTlVertex out;
      DlHVertex h;
      uint32_t offscreen;
      CHECK(dl_transform_vertices(&t, 1, dl_const_bytes(&in, 32), 32, dl_bytes(&out, 32), 32, &h, 1, DL_TRANSFORM_CLIPPED, &offscreen) == DL_OK && hr == 0);
      CHECK(h.flags == flags);
      CHECK(near(h.hx, hx, 1e-4) && near(h.hy, hy, 1e-4) && near(h.hz, hz, 1e-4));
      CHECK(near(out.sx, sx, 1e-4) && near(out.sy, sy, 1e-4) && near(out.sz, sz, 1e-4) && near(out.rhw, rhw, 1e-4));
      ++transformed;
    }
  }
  fclose(f);
  CHECK(processed == SCENE_COUNT * SCENE_VERTICES && transformed == SCENE_COUNT * SCENE_VERTICES);
  printf("  against Wine: %d processed (%d parallel point lights), %d transformed vertices; %d colours and %d speculars differ\n", processed, parallel, transformed, colour_off,
         specular_off);
  CHECK(colour_off == 0 && specular_off == 0 && parallel > 0);
}

int main() {
  test_execute_opcodes();
  test_execute_status_and_branches();
  test_execute_trivial_reject();
  test_execute_refusals();
  test_execute_hostile();
  test_execute_process_ops();
  test_matrices();
  test_viewports_and_transform();
  test_status_merge();
  test_legacy_lighting();
  test_fog();
  test_clipper();
  test_user_clip_planes();
  test_transform_vertices();
  test_caps();
  test_find_device();
  test_render_states();
  test_texture_map_blend();
  test_state_defaults();
  test_other_states();
  test_lights_and_materials();
  test_fvf();
  test_strided();
  test_process_fvf();
  test_sphere_visibility();
  test_against_wine();
  printf("D3dLegacyTest: %d checks, %d failed\n", g_checks, g_failed);
  return g_failed ? 1 : 0;
}
