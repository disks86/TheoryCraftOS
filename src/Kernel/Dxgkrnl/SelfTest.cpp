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

#include "Kernel/Dxgkrnl/SelfTest.hpp"
#include "Kernel/Dxgkrnl/CommandStream.hpp"
#include "Utility/AllocatedView.hpp"
#include "Utility/ByteBuffer.hpp"
#include "Utility/ByteStream.hpp"

namespace dxgk {

namespace {

const uint32_t kVertexTokens[] = {0xFFFE0200, 0x0200001F, 0x80000000, 0x900F0000, 0x0200001F, 0x8000000A, 0x900F0001, 0x03000014, 0xC00F0000, 0x90E40000, 0xA0E40000,
                                  0x02000001, 0xD00F0000, 0x90E40001, 0x0000FFFF};
const uint32_t kPixelTokens[] = {0xFFFF0200, 0x0200001F, 0x8000000A, 0x900F0000, 0x02000001, 0x800F0800, 0x90E40000, 0x0000FFFF};
const float kVertices[21] = {-0.8f, -0.8f, 0.5f, 1, 0, 0, 1, 0.8f, -0.8f, 0.5f, 0, 1, 0, 1, 0, 0.8f, 0.5f, 0, 0, 1, 1};
const float kIdentity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};

// The words as the little-endian bytes the services take.
bool put_words(ByteBuffer& out, const uint32_t* words, uint32_t count) {
  for (uint32_t i = 0; i < count; ++i)
    if (!out.put32(words[i])) return false;
  return true;
}

}  // namespace

NtStatus self_test_render(allocators::IAllocator& allocator, Kernel& kernel, uint32_t adapter_index, ArrayView<uint32_t> pixels, const char** step) {
  const ProcessId pid = kSelfTestProcess;
  NtStatus s = status::kSuccess;
  const char* where = "";
  Handle adapter = 0, device = 0, context = 0, vs = 0, ps = 0, pipeline = 0, target = 0, vertex_buffer = 0;
  bool opened = false;
  auto done = [&](NtStatus result) {
    if (opened) kernel.close_process(pid);
    if (step) *step = where;
    return result;
  };
  if (pixels.size() < uint64_t{kSelfTestSize} * kSelfTestSize) return done(status::kInvalidParameter);
  where = "open_process";
  if ((s = kernel.open_process(pid)) != status::kSuccess) return done(s);
  opened = true;
  where = "open_adapter";
  if ((s = kernel.open_adapter_from_index(pid, adapter_index, &adapter)) != status::kSuccess) return done(s);
  where = "create_device";
  if ((s = kernel.create_device(pid, adapter, &device)) != status::kSuccess) return done(s);
  where = "create_context";
  if ((s = kernel.create_context(pid, device, ContextPriority::Normal, &context)) != status::kSuccess) return done(s);

  ByteBuffer vertex_code(allocator), pixel_code(allocator);
  if (!put_words(vertex_code, kVertexTokens, sizeof kVertexTokens / 4) || !put_words(pixel_code, kPixelTokens, sizeof kPixelTokens / 4)) return done(status::kNoMemory);
  ShaderDesc d;
  d.kind = ShaderKind::D3d9Bytecode;
  d.stage = graphics::ShaderStage::Vertex;
  d.code = vertex_code.view();
  where = "create_shader (vertex)";
  if ((s = kernel.create_shader(pid, device, d, &vs)) != status::kSuccess) return done(s);
  d.stage = graphics::ShaderStage::Fragment;
  d.code = pixel_code.view();
  where = "create_shader (pixel)";
  if ((s = kernel.create_shader(pid, device, d, &ps)) != status::kSuccess) return done(s);

  PipelineDesc pd;
  pd.vertex_shader = vs, pd.fragment_shader = ps, pd.vertex_stride = 28, pd.attribute_count = 2, pd.varying_count = 1, pd.texture_count = 0;
  pd.attributes[0] = {0, graphics::VertexFormat::Float3, 0};
  pd.attributes[1] = {1, graphics::VertexFormat::Float4, 12};
  pd.cull_mode = graphics::CullMode::None;
  where = "create_pipeline";
  if ((s = kernel.create_pipeline(pid, device, pd, &pipeline)) != status::kSuccess) return done(s);

  AllocationDesc rd;
  rd.flags = kAllocRenderTarget, rd.format = Format::A8R8G8B8, rd.width = kSelfTestSize, rd.height = kSelfTestSize;
  where = "create_allocation (target)";
  if ((s = kernel.create_allocation(pid, device, rd, &target)) != status::kSuccess) return done(s);
  AllocationDesc vd;
  vd.flags = kAllocVertexBuffer, vd.size = sizeof kVertices;
  where = "create_allocation (vertex buffer)";
  if ((s = kernel.create_allocation(pid, device, vd, &vertex_buffer)) != status::kSuccess) return done(s);
  LockResult lock;
  where = "lock (vertex buffer)";
  if ((s = kernel.lock(pid, vertex_buffer, kLockDiscard, 0, &lock)) != status::kSuccess) return done(s);
  ByteWriter w(lock.data);
  for (float f : kVertices) w.le32(__builtin_bit_cast(uint32_t, f));
  const bool wrote = w.ok();
  kernel.unlock(pid, vertex_buffer);
  if (!wrote) return done(status::kInvalidParameter);

  ByteBuffer constants(allocator);
  for (float f : kIdentity)
    if (!constants.put32(__builtin_bit_cast(uint32_t, f))) return done(status::kNoMemory);
  CommandWriter commands(allocator);
  commands.begin_pass(0, kNoAllocation, kPassClearColor, {0, 0, 0, 1}, 1, 0);
  commands.bind_pipeline(pipeline);
  commands.bind_vertex_buffer(1);
  commands.push_constants(constants.view());
  commands.draw(3);
  commands.end_pass();
  const Handle list[2] = {target, vertex_buffer};
  where = "render";
  if ((s = kernel.render(pid, context, commands.view(), ArrayView<const Handle>(list, 2))) != status::kSuccess) return done(s);
  DeviceState state;
  where = "device state";
  if ((s = kernel.get_device_state(pid, device, &state)) != status::kSuccess) return done(s);
  if (state.execution != DeviceExecState::Running) return done(status::kDeviceRemoved);

  where = "lock (target)";
  if ((s = kernel.lock(pid, target, kLockReadOnly, 0, &lock)) != status::kSuccess) return done(s);
  bool read = true;
  for (uint32_t y = 0; y < kSelfTestSize && read; ++y) {
    ByteView row;
    read = lock.data.try_subview(uint64_t{y} * lock.pitch, kSelfTestSize * 4, &row);
    ByteReader in(read ? ConstByteView(row.data(), row.size()) : ConstByteView());
    for (uint32_t x = 0; read && x < kSelfTestSize; ++x) pixels[uint64_t{y} * kSelfTestSize + x] = in.le32();
    read = read && in.ok();
  }
  kernel.unlock(pid, target);
  if (!read) return done(status::kInvalidParameter);
  where = "";
  return done(status::kSuccess);
}

bool self_test_picture_ok(ArrayView<const uint32_t> pixels) {
  if (pixels.size() < uint64_t{kSelfTestSize} * kSelfTestSize) return false;
  uint32_t lit = 0;
  for (uint32_t p : pixels.first(uint64_t{kSelfTestSize} * kSelfTestSize))
    if (p != 0xFF000000u) ++lit;
  const uint32_t middle = pixels[uint64_t{kSelfTestSize / 2} * kSelfTestSize + kSelfTestSize / 2];
  // (the triangle covers about a third of the picture; the corner above its apex is clear; the middle is a mix of the three colours)
  return pixels[0] == 0xFF000000u && middle != 0xFF000000u && lit > kSelfTestSize * kSelfTestSize / 8 && lit < kSelfTestSize * kSelfTestSize * 3 / 4;
}

bool self_test(allocators::IAllocator& allocator, Kernel& kernel, uint32_t adapter_index) {
  const uint64_t count = uint64_t{kSelfTestSize} * kSelfTestSize;
  ArrayView<uint32_t> pixels = allocate_view<uint32_t>(allocator, count);
  if (pixels.empty()) return false;
  const bool ok = self_test_render(allocator, kernel, adapter_index, pixels) == status::kSuccess && self_test_picture_ok(pixels);
  release_view(allocator, pixels);
  return ok;
}

}  // namespace dxgk
