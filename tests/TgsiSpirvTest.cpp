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

// The Venus device's TGSI -> SPIR-V compiler (src/Graphics/Graphics3D/Venus/TgsiSpirv.hpp) on every TGSI text the kernel's producers write: the built-in
// shaders, the GL translator's (the GL conformance scenes, src/WebGl/AccelConformance.cpp, through the TGSI reference accelerator) and the Direct3D layer's
// (the Direct3D conformance scenes, src/Kernel/Dxgkrnl/Conformance.cpp, on the CPU renderer, whose shader modules carry the same text a GPU device gets).
// Each text must parse and compile, and the module must pass spirv-val for Vulkan 1.3 (SPIR-V 1.5) when the host has it. The reader's error paths are
// checked with truncated and bit-flipped texts. TGSI_SPIRV_KEEP=<dir> keeps the .tgsi / .spv files.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>
#include "Graphics/Graphics3D/BuiltinShaders.hpp"
#include "Graphics/Graphics3D/Software/SoftwareDevice.hpp"
#include "Graphics/Graphics3D/Venus/TgsiSpirv.hpp"
#include "Graphics/Graphics3D/Venus/TgsiText.hpp"
#include "Kernel/Dxgkrnl/Conformance.hpp"
#include "Kernel/Dxgkrnl/DxgKernel.hpp"
#include "WebGl/AccelConformance.hpp"

static int g_checks = 0, g_failed = 0;
#define CHECK(c)                                                       \
  do {                                                                 \
    ++g_checks;                                                        \
    if (!(c)) {                                                        \
      ++g_failed;                                                      \
      printf("FAILED %s:%d: %s\n", __FILE__, __LINE__, #c);            \
    }                                                                  \
  } while (0)

struct HostAllocator final : allocators::IAllocator {
  void* allocate(uint64_t size) override { return size ? malloc(size) : nullptr; }
  void* allocate_aligned(uint64_t size, uint64_t alignment) override { return size ? aligned_alloc(alignment < 16 ? 16 : alignment, (size + 63) & ~uint64_t{63}) : nullptr; }
  void deallocate(void* p) override { free(p); }
};

// ---- the corpus ---------------------------------------------------------------------------------------------------------------------------------------
struct Text {
  std::string where;
  std::string tgsi;
};
static std::vector<Text> g_texts;
static void keep(const std::string& where, const char* tgsi) {
  if (!tgsi) return;
  for (const Text& t : g_texts)
    if (t.tgsi == tgsi) return;
  g_texts.push_back({where, tgsi});
}

#include "TgsiAccel.hpp"

// The GL translator's texts: the reference accelerator, recording what it is given.
struct RecordingAccel : webgl::TgsiAccel {
  uint32_t create_program_stages(const char* vs, const char* fs, uint32_t attributes, uint32_t varyings, uint32_t textures, const webgl::AccelStages& st) override {
    keep("gl vs", vs);
    keep("gl fs", fs);
    keep("gl gs", st.gs);
    keep("gl tcs", st.tcs);
    keep("gl tes", st.tes);
    return webgl::TgsiAccel::create_program_stages(vs, fs, attributes, varyings, textures, st);
  }
};

// The Direct3D layer's texts: the CPU renderer, recording each module's text.
class RecordingDevice final : public graphics::IGraphicsDevice {
 public:
  explicit RecordingDevice(graphics::IGraphicsDevice& inner) : inner_(inner) {}
  graphics::DeviceInfo info() const override { return inner_.info(); }
  uint32_t format_caps(graphics::Format format) const override { return inner_.format_caps(format); }
  uint32_t max_image_dimension() const override { return inner_.max_image_dimension(); }
  graphics::IBuffer* create_buffer(uint64_t size, uint32_t usage) override { return inner_.create_buffer(size, usage); }
  void destroy_buffer(graphics::IBuffer* buffer) override { inner_.destroy_buffer(buffer); }
  graphics::IImage* create_image(const graphics::ImageDesc& desc) override { return inner_.create_image(desc); }
  void destroy_image(graphics::IImage* image) override { inner_.destroy_image(image); }
  graphics::IShaderModule* create_shader_module(const graphics::ShaderModuleDesc& desc) override {
    keep("d3d", desc.tgsi);
    return inner_.create_shader_module(desc);
  }
  void destroy_shader_module(graphics::IShaderModule* module) override { inner_.destroy_shader_module(module); }
  graphics::IPipeline* create_graphics_pipeline(const graphics::GraphicsPipelineDesc& desc) override { return inner_.create_graphics_pipeline(desc); }
  graphics::IPipeline* create_compute_pipeline(const graphics::IShaderModule* shader) override { return inner_.create_compute_pipeline(shader); }
  void destroy_pipeline(graphics::IPipeline* pipeline) override { inner_.destroy_pipeline(pipeline); }
  graphics::ICommandBuffer* create_command_buffer() override { return inner_.create_command_buffer(); }
  void destroy_command_buffer(graphics::ICommandBuffer* commands) override { inner_.destroy_command_buffer(commands); }
  bool submit(graphics::ICommandBuffer* commands) override { return inner_.submit(commands); }
  bool present(graphics::IImage* image) override { return inner_.present(image); }

 private:
  graphics::IGraphicsDevice& inner_;
};

class FakeScreen final : public hardware::IGraphicsProcessingUnit {
 public:
  const char* name() const override { return "fake screen"; }
  bool initialize() override { return true; }
  void shutdown() override {}
  uint32_t mode_count() const override { return 1; }
  bool mode_info(uint32_t i, hardware::DisplayMode* out) const override {
    if (i) return false;
    *out = {8, 6};
    return true;
  }
  uint32_t current_mode() const override { return 0; }
  bool set_mode(uint32_t i) override { return i == 0; }
  uint32_t width() const override { return 8; }
  uint32_t height() const override { return 6; }
  void put_pixel(uint32_t, uint32_t, uint32_t) override {}
  void fill_rect(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) override {}
  void copy_rect(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) override {}
  void clear(uint32_t) override {}
  uint32_t glyph_width() const override { return 8; }
  uint32_t glyph_height() const override { return 16; }
  void draw_char(uint32_t, uint32_t, char, uint32_t, uint32_t) override {}
  void draw_text(uint32_t, uint32_t, const char*, uint32_t, uint32_t) override {}
};

static void gather() {
  // the built-in shaders
  const graphics::ShaderModuleDesc builtins[] = {
      graphics::colored_vertex_shader(),        graphics::colored_fragment_shader(),          graphics::textured_vertex_shader(),
      graphics::textured_fragment_shader(),     graphics::textured_scaled_fragment_shader(),  graphics::tinted_vertex_shader(),
      graphics::tinted_fragment_shader(),       graphics::textured_masked_fragment_shader(),  graphics::textured_shadow_fragment_shader(),
      graphics::textured_blur_fragment_shader(), graphics::textured_color_fragment_shader(),
  };
  for (const graphics::ShaderModuleDesc& d : builtins) keep("builtin", d.tgsi);
  const size_t builtin_count = g_texts.size();
  // the GL translator
  {
    HostAllocator a;
    RecordingAccel acc;
    webgl::run_accel_conformance(a, acc, [](void*, const webgl::AccelSceneResult&) {}, nullptr);
  }
  const size_t gl_count = g_texts.size();
  // the Direct3D layer
  {
    HostAllocator allocator;
    FakeScreen screen;
    graphics::SoftwareDevice device(allocator, &screen), reference(allocator, &screen);
    RecordingDevice recording(device);
    dxgk::Kernel kernel(allocator);
    dxgk::AdapterConfig c;
    c.display = &screen;
    CHECK(kernel.add_adapter(recording, c) == 0 && kernel.add_adapter(reference, c) == 1);
    dxgk::run_conformance(allocator, kernel, 0, 1, [](void*, const dxgk::SceneResult&) {}, nullptr);
  }
  printf("corpus: %zu built-in, %zu GL, %zu Direct3D texts\n", builtin_count, gl_count - builtin_count, g_texts.size() - gl_count);
  CHECK(builtin_count >= 8 && gl_count - builtin_count >= 40 && g_texts.size() - gl_count >= 40);
}

// ---- compile and validate ----------------------------------------------------------------------------------------------------------------------------
static bool have_validator() { return system("spirv-val --version >/dev/null 2>&1") == 0; }

static void compile_all() {
  HostAllocator a;
  const char* keep_dir = getenv("TGSI_SPIRV_KEEP");
  const std::string dir = keep_dir ? keep_dir : (std::string(getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp") + "/tgsi-spirv");
  std::string mk = "mkdir -p '" + dir + "'";
  CHECK(system(mk.c_str()) == 0);
  const bool validate = have_validator();
  if (!validate) printf("spirv-val is not installed: modules are compiled, not validated\n");
  int compiled = 0, valid = 0;
  for (size_t i = 0; i < g_texts.size(); ++i) {
    const Text& t = g_texts[i];
    graphics::tgsi::Program p(a);
    graphics::tgsi::ParseError pe;
    // (the marks of a Direct3D text get what a device writes for bound storage: 32-bit float images, small buffer views)
    ByteBuffer substituted(a);
    StringView text(t.tgsi.c_str());
    if (graphics::tgsi::has_marks(text)) {
      graphics::tgsi::MarkValues mv;
      for (auto& f : mv.image_format) f = graphics::tgsi::ImageFormat::R32G32B32A32Float;
      for (auto& c : mv.image_count) c = 64;
      for (auto& c : mv.texel_count) c = 64;
      CHECK(graphics::tgsi::substitute_marks(text, mv, &substituted));
      text = StringView(substituted.view());
      CHECK(!graphics::tgsi::has_marks(text));
    }
    if (!graphics::tgsi::parse(text, &p, &pe)) {
      printf("FAILED parse %s #%zu line %u: %s\n", t.where.c_str(), i, pe.line, pe.message.c_str());
      ++g_failed;
      FILE* f = fopen((dir + "/fail" + std::to_string(i) + ".tgsi").c_str(), "w");
      if (f) fputs(t.tgsi.c_str(), f), fclose(f);
      continue;
    }
    graphics::tgsi::CompileOptions o;
    o.clip_distances = strstr(t.tgsi.c_str(), "CLIPDIST") ? 8 : 0;
    o.ddy_up = t.where.compare(0, 2, "gl") == 0;
    o.color_targets = 1;
    o.output_layer_any_stage = true;
    o.shared_bytes = 32768;
    graphics::tgsi::CompileResult r(a);
    ++g_checks;
    if (!graphics::tgsi::compile_spirv(p, o, &r)) {
      printf("FAILED compile %s #%zu line %u: %s\n", t.where.c_str(), i, r.line, r.error.c_str());
      ++g_failed;
      FILE* f = fopen((dir + "/fail" + std::to_string(i) + ".tgsi").c_str(), "w");
      if (f) fputs(t.tgsi.c_str(), f), fclose(f);
      continue;
    }
    ++compiled;
    const std::string base = dir + "/" + std::to_string(i);
    FILE* f = fopen((base + ".tgsi").c_str(), "w");
    if (f) fputs(t.tgsi.c_str(), f), fclose(f);
    f = fopen((base + ".spv").c_str(), "wb");
    if (f) {
      for (uint32_t w : r.words) fwrite(&w, 4, 1, f);
      fclose(f);
    }
    if (!validate) continue;
    const std::string cmd = "spirv-val --target-env vulkan1.3 '" + base + ".spv' > '" + base + ".val' 2>&1";
    ++g_checks;
    if (system(cmd.c_str()) == 0) {
      ++valid;
    } else {
      ++g_failed;
      printf("FAILED spirv-val %s #%zu (%s.spv):\n", t.where.c_str(), i, base.c_str());
      const std::string show = "head -c 600 '" + base + ".val'";
      if (system(show.c_str())) {}
      printf("\n");
    }
  }
  printf("compiled %d of %zu, %d valid\n", compiled, g_texts.size(), valid);
}

// ---- hand-written texts for what the corpus has little of ----------------------------------------------------------------------------------------------
static void compile_one(const char* name, const char* text, const graphics::tgsi::CompileOptions& o, bool expect = true) {
  HostAllocator a;
  graphics::tgsi::Program p(a);
  graphics::tgsi::ParseError pe;
  const bool parsed = graphics::tgsi::parse(StringView(text), &p, &pe);
  if (!parsed) {
    if (expect) printf("FAILED parse %s line %u: %s\n", name, pe.line, pe.message.c_str()), ++g_failed;
    return;
  }
  graphics::tgsi::CompileResult r(a);
  const bool ok = graphics::tgsi::compile_spirv(p, o, &r);
  ++g_checks;
  if (ok != expect) {
    ++g_failed;
    printf("FAILED %s: compiled %d, expected %d (%s line %u)\n", name, ok, expect, r.error.c_str(), r.line);
    return;
  }
  if (!ok || !have_validator()) return;
  const std::string base = std::string(getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp") + "/tgsi-spirv-" + name;
  FILE* f = fopen((base + ".spv").c_str(), "wb");
  for (uint32_t w : r.words) fwrite(&w, 4, 1, f);
  fclose(f);
  ++g_checks;
  const std::string cmd = "spirv-val --target-env vulkan1.3 '" + base + ".spv'";
  if (system(cmd.c_str()) != 0) ++g_failed, printf("FAILED spirv-val %s\n", name);
}

static void test_hand_written() {
  graphics::tgsi::CompileOptions o;
  o.shared_bytes = 64;
  compile_one("doubles",
              "COMP\nPROPERTY CS_FIXED_BLOCK_WIDTH 4\nPROPERTY CS_FIXED_BLOCK_HEIGHT 1\nPROPERTY CS_FIXED_BLOCK_DEPTH 1\nDCL SV[0], BLOCK_ID\nDCL SV[1], THREAD_ID\nDCL BUFFER[0]\n"
              "DCL TEMP[0..3]\nIMM[0] UINT32 {0, 0, 0, 0}\n  0: LOAD TEMP[0].xyzw, BUFFER[0], IMM[0].xxxx\n  1: DADD TEMP[1].xy, TEMP[0].xyxy, TEMP[0].zwzw\n"
              "  2: DMUL TEMP[1].zw, TEMP[0].xyxy, -TEMP[0].zwzw\n  3: DSLT TEMP[2].xy, TEMP[1].xyzw, TEMP[1].zwxy\n  4: D2F TEMP[2].z, TEMP[1].xyxy\n  5: F2D TEMP[3].xy, TEMP[2].zzzz\n"
              "  6: DRCP TEMP[3].zw, TEMP[3].xyxy\n  7: STORE BUFFER[0].xyzw, IMM[0].xxxx, TEMP[3].xyzw\n  8: END\n",
              o);
  compile_one("atomics",
              "COMP\nPROPERTY CS_FIXED_BLOCK_WIDTH 8\nPROPERTY CS_FIXED_BLOCK_HEIGHT 1\nPROPERTY CS_FIXED_BLOCK_DEPTH 1\nDCL SV[0], BLOCK_ID\nDCL SV[1], THREAD_ID\nDCL BUFFER[0]\n"
              "DCL IMAGE[0], 2D, PIPE_FORMAT_R32_UINT, WR\nDCL MEMORY[0], SHARED\nDCL TEMP[0..3]\nIMM[0] UINT32 {0, 1, 4, 9}\n"
              "  0: ATOMUADD TEMP[0].x, BUFFER[0], IMM[0].xxxx, IMM[0].yyyy\n  1: ATOMCAS TEMP[1].x, MEMORY[0], IMM[0].zzzz, IMM[0].xxxx, IMM[0].yyyy\n"
              "  2: ATOMUMAX TEMP[2].x, IMAGE[0], SV[1].xyzz, IMM[0].yyyy, 2D, PIPE_FORMAT_R32_UINT\n  3: MEMBAR IMM[0].wwww\n  4: BARRIER\n"
              "  5: RESQ TEMP[3].x, BUFFER[0]\n  6: STORE IMAGE[0].xyzw, SV[1].xyzz, TEMP[3].xxxx, 2D, PIPE_FORMAT_R32_UINT\n  7: END\n",
              o);
  compile_one("control",
              "FRAG\nDCL IN[0], GENERIC[0], PERSPECTIVE\nDCL OUT[0], COLOR\nDCL TEMP[0..2]\nIMM[0] FLT32 {0.000000000000, 1.000000000000, 2.000000000000, 0.500000000000}\n"
              "  0: MOV TEMP[0], IN[0]\n  1: BGNLOOP\n  2: SLT TEMP[1].x, TEMP[0].xxxx, IMM[0].zzzz\n  3: IF TEMP[1].xxxx\n  4: ADD TEMP[0].x, TEMP[0].xxxx, IMM[0].wwww\n"
              "  5: CONT\n  6: ELSE\n  7: BRK\n  8: ENDIF\n  9: ENDLOOP\n 10: KILL_IF -TEMP[0].xyzz\n 11: UIF TEMP[0].yyyy\n 12: KILL\n 13: RET\n 14: ENDIF\n"
              " 15: DDX TEMP[2], TEMP[0]\n 16: DDY_FINE TEMP[2].y, TEMP[0].xxxx\n 17: MOV OUT[0], TEMP[2]\n 18: END\n",
              o);
  compile_one("geometry streams",
              "GEOM\nPROPERTY GS_INPUT_PRIMITIVE TRIANGLES\nPROPERTY GS_OUTPUT_PRIMITIVE POINTS\nPROPERTY GS_MAX_OUTPUT_VERTICES 6\nPROPERTY GS_INVOCATIONS 2\n"
              "DCL IN[][0], POSITION\nDCL IN[][1], GENERIC[0]\nDCL SV[0], INVOCATIONID\nDCL OUT[0], POSITION\nDCL OUT[1], GENERIC[0]\nDCL OUT[2], LAYER\nDCL TEMP[0]\n"
              "IMM[0] UINT32 {0, 0, 0, 24064}\nIMM[1] UINT32 {1, 1, 1, 24065}\n  0: MOV OUT[0], IN[1][0]\n  1: MOV OUT[1], IN[2][1].xyzw\n  2: MOV OUT[2].x, SV[0].xxxx\n"
              "  3: EMIT IMM[0].xxxx\n  4: MOV OUT[0], IN[0][0]\n  5: EMIT IMM[1].xxxx\n  6: ENDPRIM IMM[1].xxxx\n  7: END\n",
              o);
  graphics::tgsi::StreamOutputEntry so[2] = {{0, 0, 4, 0, 0, 0}, {1, 1, 2, 1, 1, 0}};
  graphics::tgsi::CompileOptions oso;
  oso.stream_output = so, oso.stream_output_count = 2, oso.stream_output_stride[0] = 4, oso.stream_output_stride[1] = 2;
  compile_one("stream output",
              "VERT\nDCL IN[0]\nDCL IN[1]\nDCL SV[0], VERTEXID\nDCL SV[1], INSTANCEID\nDCL OUT[0], POSITION\nDCL OUT[1], GENERIC[0]\nDCL CONST[0..3]\nDCL TEMP[0]\n"
              "  0: MUL TEMP[0], CONST[0], IN[0].xxxx\n  1: MAD TEMP[0], CONST[1], IN[0].yyyy, TEMP[0]\n  2: MOV OUT[0], TEMP[0]\n  3: U2F TEMP[0].x, SV[0].xxxx\n"
              "  4: UADD TEMP[0].y, SV[1].xxxx, SV[0].xxxx\n  5: MOV OUT[1], TEMP[0]\n  6: END\n",
              oso);
  compile_one("tessellation control",
              "TESS_CTRL\nPROPERTY TCS_VERTICES_OUT 3\nDCL SV[0], INVOCATIONID\nDCL IN[][0], POSITION\nDCL IN[][1], GENERIC[0]\nDCL OUT[][0], POSITION\nDCL OUT[][1], GENERIC[0]\n"
              "DCL OUT[2], TESSOUTER\nDCL OUT[3], TESSINNER\nDCL OUT[4], PATCH[0]\nDCL TEMP[0]\nIMM[0] UINT32 {0, 1, 2, 0}\nIMM[1] FLT32 {2.000000000000, 1.000000000000, 0.0, 0.0}\n"
              "  0: USEQ TEMP[0].x, SV[0].xxxx, IMM[0].xxxx\n  1: UIF TEMP[0].xxxx\n  2: MOV OUT[0][0], IN[0][0]\n  3: MOV OUT[0][1], IN[0][1]\n  4: ENDIF\n"
              "  5: USEQ TEMP[0].x, SV[0].xxxx, IMM[0].yyyy\n  6: UIF TEMP[0].xxxx\n  7: MOV OUT[1][0], IN[1][0]\n  8: ENDIF\n  9: BARRIER\n 10: MOV TEMP[0], OUT[1][0]\n"
              " 11: MOV OUT[2].xyz, IMM[1].xxxx\n 12: MOV OUT[3].x, IMM[1].yyyy\n 13: MOV OUT[4], TEMP[0]\n 14: END\n",
              o);
  compile_one("tessellation evaluation",
              "TESS_EVAL\nPROPERTY TES_PRIM_MODE 4\nPROPERTY TES_SPACING 1\nPROPERTY TES_VERTEX_ORDER_CW 1\nPROPERTY TES_POINT_MODE 0\nDCL SV[0], TESSCOORD\nDCL IN[][0], POSITION\n"
              "DCL IN[4], PATCH[0]\nDCL OUT[0], POSITION\nDCL OUT[1], GENERIC[0]\nDCL TEMP[0]\n  0: MUL TEMP[0], IN[0][0], SV[0].xxxx\n  1: MAD TEMP[0], IN[1][0], SV[0].yyyy, TEMP[0]\n"
              "  2: MAD OUT[0], IN[2][0], SV[0].zzzz, TEMP[0]\n  3: MOV OUT[1], IN[4]\n  4: END\n",
              o);
  graphics::tgsi::CompileOptions ot;
  ot.sview_type[1] = graphics::tgsi::ValueType::Uint;
  compile_one("textures",
              "FRAG\nDCL IN[0], GENERIC[0], PERSPECTIVE\nDCL OUT[0], COLOR\nDCL OUT[1], COLOR[1]\nDCL SAMP[0]\nDCL SVIEW[0], CUBEARRAY, FLOAT\nDCL SAMP[1]\nDCL SVIEW[1], 2D_ARRAY, UINT\n"
              "DCL SAMP[2]\nDCL SVIEW[2], 2D_MSAA, FLOAT\nDCL SAMP[16]\nDCL SVIEW[16], BUFFER, FLOAT\nDCL SAMP[3]\nDCL SVIEW[3], 3D, FLOAT\nDCL TEMP[0..3]\n"
              "IMM[0] UINT32 {1, 4294967295, 0, 2}\n"
              "  0: TXL2 TEMP[0], IN[0], IN[0].wwww, SAMP[0], CUBEARRAY\n  1: TXF TEMP[1], IN[0], SAMP[1], 2D_ARRAY\n  2: TXF TEMP[2], IN[0], SAMP[2], 2D_MSAA\n"
              "  3: TXF TEMP[3], IN[0], SAMP[16], BUFFER\n  4: TXQ TEMP[3], IMM[0].zzzz, SAMP[0], CUBEARRAY\n  5: TG4 TEMP[2], IN[0], IMM[0].xxxx, SAMP[1], 2D_ARRAY, IMM[0].xyz\n"
              "  6: TXD TEMP[1], IN[0], IN[0].yyyy, IN[0].zzzz, SAMP[3], 3D\n  7: LODQ TEMP[1].xy, IN[0].xyyy, SAMP[3], 3D\n  8: TXB TEMP[0], IN[0], SAMP[3], 3D\n"
              "  9: TXP TEMP[0], IN[0], SAMP[3], 3D\n 10: MOV OUT[0], TEMP[0]\n 11: MOV OUT[1], TEMP[1]\n 12: END\n",
              ot);
  compile_one("unknown opcode", "VERT\n  0: FROB TEMP[0], IN[0]\n  1: END\n", o, false);
  compile_one("no end", "VERT\nDCL TEMP[0]\n  0: MOV TEMP[0], TEMP[0]\n", o, false);
}

// The reader must refuse or take any truncation and bit flip of a real text without reading out of bounds (ASan watches).
static void test_damaged_texts() {
  HostAllocator a;
  int refused = 0, taken = 0;
  for (size_t ti = 0; ti < g_texts.size() && ti < 40; ++ti) {
    const std::string& src = g_texts[ti].tgsi;
    for (size_t cut = 0; cut < src.size(); cut += 1 + src.size() / 64) {
      graphics::tgsi::Program p(a);
      graphics::tgsi::ParseError e;
      graphics::tgsi::parse(StringView(src.data(), cut), &p, &e) ? ++taken : ++refused;
    }
    for (size_t bit = 0; bit < src.size() * 8; bit += 1 + src.size() / 8) {
      std::string d = src;
      d[bit / 8] = static_cast<char>(d[bit / 8] ^ (1 << (bit % 8)));
      graphics::tgsi::Program p(a);
      graphics::tgsi::ParseError e;
      if (graphics::tgsi::parse(StringView(d.data(), d.size()), &p, &e)) {
        ++taken;
        graphics::tgsi::CompileOptions o;
        graphics::tgsi::CompileResult r(a);
        (void)graphics::tgsi::compile_spirv(p, o, &r);
      } else {
        ++refused;
      }
    }
  }
  printf("damaged texts: %d refused, %d taken\n", refused, taken);
  CHECK(refused > 0);
}

int main() {
  gather();
  compile_all();
  test_hand_written();
  test_damaged_texts();
  printf("%d checks, %d failed\n", g_checks, g_failed);
  return g_failed ? 1 : 0;
}
