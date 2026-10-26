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

#include "Hardware/GraphicsProcessingUnit/Broadcom/V3dSelfTest.hpp"
#include "Hardware/GraphicsProcessingUnit/Broadcom/V3dDraw.hpp"
#include "Hardware/GraphicsProcessingUnit/Broadcom/V3dRenderer.hpp"

namespace hardware {

namespace {
constexpr uint32_t kZero = 0, kOne = 0x3F800000, kHalf = 0x3F000000, kMinusHalf = 0xBF000000;
constexpr uint32_t kPitchPixels = 128;  // a multiple of 16: rows start on 64-byte boundaries
constexpr uint32_t kOverflowBytes = 256 * 1024;

// A block that is given back when the test ends.
class Block {
 public:
  explicit Block(IV3dGpuMemory& memory) : memory_(memory) {}
  ~Block() { if (block.valid()) memory_.release(block); }
  Block(const Block&) = delete;
  Block& operator=(const Block&) = delete;
  bool get(uint32_t bytes) { return memory_.allocate(bytes, 4096, &block); }
  V3dGpuBlock block;

 private:
  IV3dGpuMemory& memory_;
};

void put32(ArrayView<uint8_t> bytes, uint32_t at, uint32_t value) {
  for (uint32_t b = 0; b < 4; ++b) bytes[at + b] = static_cast<uint8_t>(value >> (8 * b));
}

class TriangleContent final : public IV3dBinningContent {
 public:
  TriangleContent(const V3dFrameDesc& frame, const V3dGpuBlock& code, const V3dGpuBlock& defaults, const V3dGpuBlock& vertices, const v3d_shader::ShaderCode& shaders)
      : frame_(frame), code_(code), defaults_(defaults), vertices_(vertices), shaders_(shaders) {}

  bool emit(V3dControlList& bcl, V3dControlList& indirect) override {
    V3dViewport vp;
    vp.width = frame_.width;
    vp.height = frame_.height;
    V3dDrawState state;
    state.cull = V3dDrawState::Cull::None;  // the winding convention is not part of this test
    if (!V3dDrawEmitter::emit_state(bcl, frame_, vp, state)) return false;

    uint32_t identity[16] = {};
    identity[0] = identity[5] = identity[10] = identity[15] = kOne;
    uint32_t cs[v3d_shader::kCoordinateUniformWords], vs[v3d_shader::kVertexUniformWords], fs[v3d_shader::kFragmentUniformWords];
    const uint32_t x_scale = v3d_shader::float_bits(vp.width * 128), y_scale = v3d_shader::float_bits(vp.height * 128);
    if (!v3d_shader::coordinate_uniforms(ArrayView<uint32_t>(cs), ArrayView<const uint32_t>(identity), x_scale, y_scale) ||
        !v3d_shader::vertex_uniforms(ArrayView<uint32_t>(vs), ArrayView<const uint32_t>(identity), x_scale, y_scale, vp.z_scale_bits, vp.z_offset_bits))
      return false;
    fs[0] = v3d_shader::kTlbColorConfig;

    V3dDrawCall call;
    call.code_address = code_.gpu_address;
    call.code = shaders_;
    call.defaults_address = defaults_.gpu_address;
    call.attribute_count = 2;
    call.attributes[0] = {vertices_.gpu_address, 28, 3, 3, 3};
    call.attributes[1] = {vertices_.gpu_address + 12, 28, 4, 0, 4};
    call.vertex_count = 3;
    call.coordinate_uniforms = ArrayView<const uint32_t>(cs);
    call.vertex_uniforms = ArrayView<const uint32_t>(vs);
    call.fragment_uniforms = ArrayView<const uint32_t>(fs);
    return V3dDrawEmitter::emit_draw(bcl, indirect, call);
  }

 private:
  const V3dFrameDesc& frame_;
  const V3dGpuBlock& code_;
  const V3dGpuBlock& defaults_;
  const V3dGpuBlock& vertices_;
  const v3d_shader::ShaderCode& shaders_;
};
}  // namespace

void V3dSelfTest::count_pixels(ArrayView<const uint32_t> pixels, uint32_t pitch, uint32_t width, uint32_t height, V3dSelfTestResult* result) {
  result->clear_pixels = result->drawn_pixels = 0;
  for (uint32_t y = 0; y < height; ++y)
    for (uint32_t x = 0; x < width; ++x) {
      uint32_t pixel = 0;
      if (!pixels.try_get(uint64_t{y} * pitch + x, &pixel)) return;
      if (pixel == kClearPixel) ++result->clear_pixels;
      else ++result->drawn_pixels;
    }
}

V3dSelfTestResult V3dSelfTest::run(V3dGpu& gpu, IV3dGpuMemory& memory, uint32_t attempts, IGraphicsProcessingUnit* display) {
  V3dSelfTestResult result;
  if (!gpu.initialized()) return result;

  uint64_t words[256];
  v3d_shader::ShaderCode shaders;
  if (!v3d_shader::build_colored_pipeline(ArrayView<uint64_t>(words, 256), &shaders)) return result;
  const uint32_t instructions = shaders.fragment_z.first + shaders.fragment_z.instructions;

  Block code(memory), defaults(memory), vertices(memory), image(memory), job(memory), overflow(memory);
  if (!code.get(instructions * 8) || !defaults.get(256) || !vertices.get(3 * 28) || !image.get(kPitchPixels * V3dSelfTestResult::kHeight * 4)) return result;
  for (uint32_t i = 0; i < instructions; ++i)
    for (uint32_t b = 0; b < 8; ++b) code.block.cpu[i * 8 + b] = static_cast<uint8_t>(words[i] >> (8 * b));
  memory.clean(code.block);
  uint32_t defaults_words[64];
  V3dDrawEmitter::fill_attribute_defaults(ArrayView<uint32_t>(defaults_words));
  for (uint32_t i = 0; i < 64; ++i) put32(defaults.block.cpu, i * 4, defaults_words[i]);
  memory.clean(defaults.block);

  // Three vertices: position (x, y, z) then colour (r, g, b, a) as float bit patterns.
  const uint32_t triangle[3][7] = {{kMinusHalf, kMinusHalf, kHalf, kOne, kZero, kZero, kOne},
                                   {kHalf, kMinusHalf, kHalf, kZero, kOne, kZero, kOne},
                                   {kZero, kHalf, kHalf, kZero, kZero, kOne, kOne}};
  for (uint32_t v = 0; v < 3; ++v)
    for (uint32_t k = 0; k < 7; ++k) put32(vertices.block.cpu, (v * 7 + k) * 4, triangle[v][k]);
  memory.clean(vertices.block);

  V3dFrameDesc frame;
  frame.width = V3dSelfTestResult::kWidth;
  frame.height = V3dSelfTestResult::kHeight;
  frame.color = {image.block.gpu_address, kPitchPixels * 4};
  frame.swap_rb = true;  // the image is B, G, R, A in memory: 0xAARRGGBB
  frame.clear_color = true;
  frame.clear_color_rgba8 = 0xFF804020;  // R 0x20, G 0x40, B 0x80, A 0xFF with R lowest

  V3dRenderLayout layout;
  layout.binner_bytes = 4096;
  layout.indirect_bytes = 4096;
  uint64_t needed = 0;
  if (!V3dRenderer::memory_needed(frame, layout, &needed) || needed > UINT32_MAX - 4096 || !job.get(static_cast<uint32_t>(needed))) return result;
  V3dMemoryBlock overflow_blocks[1];
  uint32_t overflow_count = 0;
  if (overflow.get(kOverflowBytes)) overflow_blocks[overflow_count++] = {overflow.block.gpu_address, kOverflowBytes};
  result.memory_ok = true;

  TriangleContent content(frame, code.block, defaults.block, vertices.block, shaders);
  V3dBlockJobMemory job_memory(memory, job.block);
  result.job = V3dRenderer::render(gpu, frame, job_memory, attempts, ArrayView<const V3dMemoryBlock>(overflow_blocks, overflow_count), layout, &content);
  memory.invalidate(image.block);

  ArrayView<uint32_t> pixels;
  if (image.block.cpu.try_cast<uint32_t>(&pixels)) {
    count_pixels(ArrayView<const uint32_t>(pixels.data(), pixels.size()), kPitchPixels, V3dSelfTestResult::kWidth, V3dSelfTestResult::kHeight, &result);
    if (display && result.job == V3dJobResult::Done) {
      display->blit(0, 0, V3dSelfTestResult::kWidth, V3dSelfTestResult::kHeight, pixels.data(), kPitchPixels);
      display->present();
    }
  }
  return result;
}

}  // namespace hardware
