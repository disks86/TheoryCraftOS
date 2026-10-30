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

// Host-side tests for the kernel-mode Direct3D driver (src/Kernel/Dxgkrnl): handles and processes, allocation checks, locking, the command stream (rendering with the software
// device, truncated and bit-flipped streams), fences and the scheduler, video memory budget and eviction, presenting, shared allocations, the private escape ABI, and what
// happens when a command fails. Everything runs under ASan/UBSan, and the allocator must be empty again once the kernel is gone.
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>
#include "Graphics/Graphics3D/BuiltinShaders.hpp"
#include "Graphics/Graphics3D/Software/SoftwareDevice.hpp"
#include "Kernel/Dxgkrnl/Conformance.hpp"
#include "Kernel/Dxgkrnl/CommandStream.hpp"
#include "Kernel/Dxgkrnl/ConformanceBlobs.hpp"
#include "Kernel/Dxgkrnl/DxgKernel.hpp"
#include "Utility/ByteStream.hpp"
#include "DxgkrnlDeviceSuite.hpp"

using namespace dxgk;

static int g_failed = 0;
#define CHECK(...) do { if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)

namespace {

struct HostAllocator final : allocators::IAllocator {
  int live = 0;
  void* allocate(uint64_t size) override { ++live; return malloc(size); }
  void* allocate_aligned(uint64_t size, uint64_t alignment) override {
    void* p = nullptr;
    if (posix_memalign(&p, alignment < sizeof(void*) ? sizeof(void*) : alignment, size)) return nullptr;
    ++live;
    return p;
  }
  void deallocate(void* p) override { if (p) { --live; free(p); } }
};

// A display driver that keeps its screen in memory.
class FakeScreen final : public hardware::IGraphicsProcessingUnit {
 public:
  FakeScreen(uint32_t w, uint32_t h) : w_(w), h_(h), pixels_(uint64_t{w} * h, 0) {}
  const char* name() const override { return "fake screen"; }
  bool initialize() override { return true; }
  void shutdown() override {}
  uint32_t mode_count() const override { return 2; }
  bool mode_info(uint32_t i, hardware::DisplayMode* out) const override { if (i > 1) return false; *out = {w_ * (i + 1), h_ * (i + 1)}; return true; }
  uint32_t current_mode() const override { return mode_; }
  bool set_mode(uint32_t i) override { if (i > 1) return false; mode_ = i; return true; }
  uint32_t width() const override { return w_; }
  uint32_t height() const override { return h_; }
  void put_pixel(uint32_t x, uint32_t y, uint32_t c) override { if (x < w_ && y < h_) pixels_[uint64_t{y} * w_ + x] = c; }
  void fill_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t c) override { for (uint32_t j = 0; j < h; ++j) for (uint32_t i = 0; i < w; ++i) put_pixel(x + i, y + j, c); }
  void copy_rect(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) override {}
  void clear(uint32_t c) override { for (uint32_t& p : pixels_) p = c; }
  uint32_t glyph_width() const override { return 8; }
  uint32_t glyph_height() const override { return 16; }
  void draw_char(uint32_t, uint32_t, char, uint32_t, uint32_t) override {}
  void draw_text(uint32_t, uint32_t, const char*, uint32_t, uint32_t) override {}
  void present() override { ++presents; }
  uint32_t at(uint32_t x, uint32_t y) const { return pixels_[uint64_t{y} * w_ + x] & 0xFFFFFF; }
  uint32_t presents = 0;

 private:
  uint32_t w_, h_, mode_ = 0;
  std::vector<uint32_t> pixels_;
};

// Time that only passes when somebody waits; `on_wait` lets the test play the other thread.
class FakeHook final : public IWaitHook {
 public:
  void wait(uint64_t timeout_ns) override {
    ++waits;
    now += timeout_ns < 1000000 ? timeout_ns : 1000000;
    if (on_wait) on_wait();
  }
  void wake() override { ++wakes; }
  uint64_t now_ns() override { return now; }
  uint64_t now = 0;
  int waits = 0, wakes = 0;
  void (*on_wait)() = nullptr;
};

constexpr ProcessId kPid = 100, kPid2 = 200;
constexpr uint32_t kTexture = kAllocTexture, kTarget = kAllocRenderTarget;

// One machine: a software device behind one adapter (a second one on request), a kernel, a process with a device and a context.
struct Env {
  explicit Env(uint64_t budget = 64ull << 20, bool second_adapter = false) : screen(8, 6), device(allocator, &screen), device2(allocator, &screen), kernel(allocator) {
    AdapterConfig c;
    c.local_memory_budget = budget;
    c.display = &screen;
    kernel.add_adapter(device, c);
    if (second_adapter) kernel.add_adapter(device2, c);
    open(kPid, &adapter, &dev, &ctx);
  }
  void open(ProcessId pid, Handle* a, Handle* d, Handle* c, uint32_t adapter_index = 0) {
    CHECK(kernel.open_process(pid) == status::kSuccess);
    CHECK(kernel.open_adapter_from_index(pid, adapter_index, a) == status::kSuccess);
    CHECK(kernel.create_device(pid, *a, d) == status::kSuccess);
    if (c) CHECK(kernel.create_context(pid, *d, ContextPriority::Normal, c) == status::kSuccess);
  }
  Handle alloc(uint32_t flags, Format f, uint32_t w, uint32_t h, uint32_t mips = 1, ProcessId pid = kPid, Handle device_handle = 0) {
    AllocationDesc d;
    d.flags = flags, d.format = f, d.width = w, d.height = h, d.mip_levels = mips;
    Handle out = 0;
    CHECK(kernel.create_allocation(pid, device_handle ? device_handle : dev, d, &out) == status::kSuccess);
    return out;
  }
  Handle buffer(uint32_t flags, uint64_t size) {
    AllocationDesc d;
    d.flags = flags, d.size = size;
    Handle out = 0;
    CHECK(kernel.create_allocation(kPid, dev, d, &out) == status::kSuccess);
    return out;
  }
  // The word at (x, y) of a level-0 image.
  uint32_t pixel(Handle a, uint32_t x, uint32_t y, uint32_t level = 0, ProcessId pid = kPid) {
    LockResult r;
    if (kernel.lock(pid, a, kLockReadOnly, level, &r) != status::kSuccess) return 0xDEADDEAD;
    uint32_t v = 0;
    ByteReader in(r.data);
    in.skip(uint64_t{y} * r.pitch + x * 4);
    v = in.le32();
    kernel.unlock(pid, a);
    return v;
  }
  bool write(Handle a, ConstByteView bytes, uint32_t flags = kLockDiscard, uint32_t level = 0, ProcessId pid = kPid) {
    LockResult r;
    if (kernel.lock(pid, a, flags, level, &r) != status::kSuccess) return false;
    const bool ok = r.data.copy_from(bytes);
    kernel.unlock(pid, a);
    return ok;
  }
  bool fill(Handle a, uint32_t word, uint32_t level = 0, ProcessId pid = kPid) {
    LockResult r;
    if (kernel.lock(pid, a, kLockDiscard, level, &r) != status::kSuccess) return false;
    ArrayView<uint32_t> w;
    if (!r.data.try_cast<uint32_t>(&w)) return false;
    w.fill(word);
    kernel.unlock(pid, a);
    return true;
  }
  NtStatus render(CommandWriter& w, std::initializer_list<Handle> list, Handle context = 0) {
    std::vector<Handle> v(list);
    return kernel.render(kPid, context ? context : ctx, w.view(), ArrayView<const Handle>(v.data(), v.size()));
  }
  NtStatus clear(Handle target, graphics::Vec4 color, Handle context = 0) {
    CommandWriter w(allocator);
    w.begin_pass(0, kNoAllocation, kPassClearColor, color, 1.0f, 0);
    w.end_pass();
    return render(w, {target}, context);
  }

  HostAllocator allocator;
  FakeScreen screen;
  graphics::SoftwareDevice device, device2;
  Kernel kernel;
  Handle adapter = 0, dev = 0, ctx = 0;
};

// Creates the colored pipeline of BuiltinShaders (no culling) on the environment's device.
Handle make_colored_pipeline(Env& e, bool textured = false, graphics::CullMode cull = graphics::CullMode::None, graphics::FrontFace front = graphics::FrontFace::CounterClockwise) {
  Handle vs = 0, fs = 0, pipe = 0;
  CHECK(e.kernel.create_shader(kPid, e.dev, {textured ? ShaderKind::TexturedVertex : ShaderKind::ColoredVertex, graphics::ShaderStage::Vertex, {}}, &vs) == status::kSuccess);
  CHECK(e.kernel.create_shader(kPid, e.dev, {textured ? ShaderKind::TexturedFragment : ShaderKind::ColoredFragment, graphics::ShaderStage::Fragment, {}}, &fs) == status::kSuccess);
  graphics::GraphicsPipelineDesc d;
  if (textured) graphics::describe_textured_pipeline(&d);
  else graphics::describe_colored_pipeline(&d);
  PipelineDesc pd;
  pd.vertex_shader = vs, pd.fragment_shader = fs;
  pd.vertex_stride = d.vertex_stride, pd.attribute_count = d.attribute_count, pd.varying_count = d.varying_count, pd.texture_count = d.texture_count;
  for (uint32_t i = 0; i < d.attribute_count; ++i) pd.attributes[i] = d.attributes[i];
  pd.cull_mode = cull;
  pd.front_face = front;
  CHECK(e.kernel.create_pipeline(kPid, e.dev, pd, &pipe) == status::kSuccess);
  return pipe;
}

ConstByteView floats(const float* f, uint32_t count) { return ConstByteView(reinterpret_cast<const uint8_t*>(f), uint64_t{count} * 4); }

// The big triangle that covers the whole viewport, with one colour.
bool write_triangle(Env& e, Handle vb, float r, float g, float b) {
  const float v[21] = {-1, -1, 0.5f, r, g, b, 1, 3, -1, 0.5f, r, g, b, 1, -1, 3, 0.5f, r, g, b, 1};
  return e.write(vb, floats(v, 21));
}

void test_handle_table() {
  HostAllocator a;
  HandleTable<int> t(a, 3);
  Handle h1 = t.insert(SharedPointer<int>::make(a, 1)), h2 = t.insert(SharedPointer<int>::make(a, 2)), h3 = t.insert(SharedPointer<int>::make(a, 3));
  CHECK(h1 && h2 && h3 && h1 != h2 && h2 != h3);
  CHECK(t.count() == 3 && *t.get(h2) == 2 && *t.raw(h3) == 3);
  CHECK(t.remove(h2) && !t.remove(h2) && !t.get(h2) && t.count() == 2);
  Handle h4 = t.insert(SharedPointer<int>::make(a, 4));
  CHECK(h4 != h2 && (h4 & 0xFFFF) == (h2 & 0xFFFF));   // the slot is reused, the handle is not
  CHECK(!t.get(h2) && *t.get(h4) == 4);
  CHECK(!t.get(0) && !t.get(0x30010000) && !t.get(0x3001FFFF) && !t.get(h1 + 100) && !t.get(h1 ^ 0x10000000));   // (the last: right slot and generation, wrong kind)
  CHECK(!t.insert(SharedPointer<int>()));
  int sum = 0;
  t.for_each([&](Handle, const SharedPointer<int>& p) { sum += *p; });
  CHECK(sum == 1 + 3 + 4);
  // Reusing one slot over and over never makes a handle that was valid before.
  Handle seen = h4;
  for (int i = 0; i < 70000; ++i) {
    t.remove(seen);
    Handle n = t.insert(SharedPointer<int>::make(a, i));
    CHECK(n != 0 && n != seen);
    seen = n;
  }
}

void test_adapters_and_processes() {
  Env e(64ull << 20, true);
  CHECK(e.kernel.adapter_count() == 2);
  AdapterEntry entries[4];
  uint32_t total = 0;
  CHECK(e.kernel.enum_adapters(kPid, ArrayView<AdapterEntry>(entries, 4), &total) == status::kSuccess && total == 2);
  CHECK(entries[0].luid.low != entries[1].luid.low && entries[0].index == 0 && entries[1].index == 1);
  AdapterEntry one[1];
  CHECK(e.kernel.enum_adapters(kPid, ArrayView<AdapterEntry>(one, 1), &total) == status::kBufferTooSmall && total == 2);
  Handle by_luid = 0;
  CHECK(e.kernel.open_adapter_from_luid(kPid, entries[1].luid, &by_luid) == status::kSuccess);
  CHECK(e.kernel.open_adapter_from_luid(kPid, Luid{0x7777, 0}, &by_luid) == status::kNotFound);
  CHECK(e.kernel.open_adapter_from_index(kPid, 9, &by_luid) == status::kNotFound);

  AdapterCaps caps;
  CHECK(e.kernel.query_adapter_caps(kPid, e.adapter, &caps) == status::kSuccess);
  CHECK(caps.max_texture_dimension == kMaxTextureDimension && caps.max_texture_units == graphics::kMaxTextureUnits && !caps.accelerated && caps.name[0] == 's');
  VideoMemoryInfo vm;
  CHECK(e.kernel.query_video_memory_info(kPid, e.adapter, MemorySegmentGroup::Local, &vm) == status::kSuccess && vm.budget == 64ull << 20 && vm.current_usage == 0);

  // Display modes come from the screen.
  uint32_t count = 0, index = 9;
  DisplayModeInfo mode;
  CHECK(e.kernel.display_mode_count(kPid, e.adapter, &count) == status::kSuccess && count == 2);
  CHECK(e.kernel.display_mode(kPid, e.adapter, 1, &mode) == status::kSuccess && mode.width == 16 && mode.height == 12);
  CHECK(e.kernel.display_mode(kPid, e.adapter, 2, &mode) == status::kInvalidParameter);
  CHECK(e.kernel.set_display_mode(kPid, e.adapter, 1) == status::kSuccess && e.kernel.current_display_mode(kPid, e.adapter, &index) == status::kSuccess && index == 1);
  CHECK(e.kernel.set_display_mode(kPid, e.adapter, 5) == status::kInvalidParameter);
  uint32_t dummy = 0;
  CHECK(e.kernel.display_mode_count(kPid, entries[1].handle, &dummy) == status::kSuccess);

  // Handles belong to a process.
  Handle other = 0;
  CHECK(e.kernel.open_process(kPid2) == status::kSuccess);
  CHECK(e.kernel.create_device(kPid2, e.adapter, &other) == status::kInvalidHandle);
  CHECK(e.kernel.query_adapter_caps(kPid2, e.adapter, &caps) == status::kInvalidHandle);
  CHECK(e.kernel.query_adapter_caps(555, e.adapter, &caps) == status::kInvalidHandle);   // (a process that was never opened)
  CHECK(e.kernel.create_allocation(kPid2, e.dev, AllocationDesc(), &other) == status::kInvalidHandle);
  CHECK(e.kernel.destroy_device(kPid2, e.dev) == status::kInvalidHandle);
  CHECK(e.kernel.close_adapter(kPid, e.adapter) == status::kSuccess && e.kernel.close_adapter(kPid, e.adapter) == status::kInvalidHandle);
  DeviceState st;
  CHECK(e.kernel.get_device_state(kPid, e.dev, &st) == status::kSuccess && st.execution == DeviceExecState::Running && st.reset_count == 0);   // (the device outlives its adapter handle)
  e.kernel.close_process(kPid2);
}

void test_allocation_checks() {
  Env e;
  struct Case {
    AllocationDesc desc;
    NtStatus expect;
  };
  auto image = [](uint32_t flags, Format f, uint32_t w, uint32_t h, uint32_t mips = 1) {
    AllocationDesc d;
    d.flags = flags, d.format = f, d.width = w, d.height = h, d.mip_levels = mips;
    return d;
  };
  auto buf = [](uint32_t flags, uint64_t size) {
    AllocationDesc d;
    d.flags = flags, d.size = size;
    return d;
  };
  const Case bad[] = {
      {AllocationDesc(), status::kInvalidParameter},
      {image(1u << 20, Format::A8R8G8B8, 4, 4), status::kInvalidParameter},                                     // unknown flag
      {buf(kAllocVertexBuffer, 0), status::kInvalidParameter},
      {buf(kAllocVertexBuffer, kMaxBufferBytes + 1), status::kInvalidParameter},
      {buf(kAllocVertexBuffer | kAllocTexture, 64), status::kInvalidParameter},
      {buf(kAllocIndexBuffer | kAllocRenderTarget, 64), status::kInvalidParameter},
      {image(kTexture, Format::A8R8G8B8, 0, 4), status::kInvalidParameter},
      {image(kTexture, Format::A8R8G8B8, 4, 0), status::kInvalidParameter},
      {image(kTexture, Format::A8R8G8B8, kMaxTextureDimension + 1, 4), status::kInvalidParameter},
      {image(kTexture, Format::Unknown, 4, 4), status::kInvalidParameter},
      {image(kTexture, Format::D32Float, 4, 4), status::kInvalidParameter},                                     // a depth format needs the depth flag
      {image(kAllocDepthStencil, Format::A8R8G8B8, 4, 4), status::kInvalidParameter},                           // and a colour format may not have it
      {image(kAllocDepthStencil | kTexture, Format::D32Float, 4, 4), status::kInvalidParameter},
      {image(kAllocStaging | kTarget, Format::A8R8G8B8, 4, 4), status::kInvalidParameter},
      {image(kTexture | kTarget, Format::A8R8G8B8, 4, 4, 2), status::kInvalidParameter},                         // a mipmapped texture is only sampled
      {image(kTexture, Format::A8R8G8B8, 4, 4, 4), status::kInvalidParameter},                                   // (4 x 4 has 3 levels)
      {image(kTexture, Format::A8R8G8B8, 4, 4, 0), status::kInvalidParameter},
      {image(kAllocPresent | kAllocStaging, Format::A8R8G8B8, 4, 4), status::kInvalidParameter},
  };
  uint32_t n = 0;
  for (const Case& c : bad) {
    Handle h = 0;
    NtStatus s = e.kernel.create_allocation(kPid, e.dev, c.desc, &h);
    if (s != c.expect || h != 0) printf("  case %u gave %08x\n", n, s);
    CHECK(s == c.expect && h == 0);
    ++n;
  }
  // Good ones, and what they report.
  Handle t = e.alloc(kTexture, Format::X8R8G8B8, 8, 4, 4);
  AllocationInfo info;
  CHECK(e.kernel.query_allocation(kPid, t, &info) == status::kSuccess && info.pitch == 32 && info.bytes == (32 + 8 + 2 + 1) * 4 && !info.resident && !info.shared);
  Handle d = e.alloc(kAllocDepthStencil, Format::D24S8, 8, 8);
  Handle sh = e.alloc(kTexture | kAllocShared, Format::A8R8G8B8, 4, 4);
  CHECK(e.kernel.query_allocation(kPid, sh, &info) == status::kSuccess && info.shared);
  Handle vb = e.buffer(kAllocVertexBuffer | kAllocIndexBuffer, 256);
  CHECK(t && d && vb);

  // Locking.
  LockResult r;
  CHECK(e.kernel.lock(kPid, t, 0, 4, &r) == status::kInvalidParameter);            // no such level
  CHECK(e.kernel.lock(kPid, d, 0, 0, &r) == status::kNotSupported);                // depth has no CPU view
  CHECK(e.kernel.lock(kPid, vb, 0, 1, &r) == status::kInvalidParameter);
  CHECK(e.kernel.lock(kPid, t, 0x80, 0, &r) == status::kInvalidParameter);
  CHECK(e.kernel.lock(kPid, t, 0, 1, &r) == status::kSuccess && r.pitch == 16 && r.data.size() == 32);   // level 1 of 8 x 4 is 4 x 2
  CHECK(e.kernel.lock(kPid, t, 0, 0, &r) == status::kInvalidDeviceState);          // locked already
  CHECK(e.kernel.unlock(kPid, t) == status::kSuccess && e.kernel.unlock(kPid, t) == status::kInvalidDeviceState);
  CHECK(e.kernel.lock(kPid, vb, 0, 0, &r) == status::kSuccess && r.data.size() == 256 && r.pitch == 0);
  CHECK(e.kernel.unlock(kPid, vb) == status::kSuccess);
  // A locked allocation can be destroyed (the lock goes with it).
  CHECK(e.kernel.lock(kPid, vb, 0, 0, &r) == status::kSuccess && e.kernel.destroy_allocation(kPid, vb) == status::kSuccess);
  CHECK(e.kernel.destroy_allocation(kPid, vb) == status::kInvalidHandle && e.kernel.lock(kPid, vb, 0, 0, &r) == status::kInvalidHandle);
}

void test_system_limit() {
  HostAllocator a;
  FakeScreen screen(4, 4);
  graphics::SoftwareDevice device(a, &screen);
  {
    Kernel k(a);
    AdapterConfig c;
    c.system_memory_limit = 4096;
    k.add_adapter(device, c);
    Handle ad, dv;
    k.open_process(kPid);
    k.open_adapter_from_index(kPid, 0, &ad);
    k.create_device(kPid, ad, &dv);
    AllocationDesc d;
    d.flags = kTexture, d.format = Format::A8R8G8B8, d.width = 32, d.height = 32;   // 4096 bytes
    Handle h1 = 0, h2 = 0;
    CHECK(k.create_allocation(kPid, dv, d, &h1) == status::kSuccess);
    CHECK(k.create_allocation(kPid, dv, d, &h2) == status::kGraphicsNoVideoMemory && !h2);
    CHECK(k.destroy_allocation(kPid, h1) == status::kSuccess);
    CHECK(k.create_allocation(kPid, dv, d, &h2) == status::kSuccess);   // (the destroyed one gave its memory back)
  }
  CHECK(a.live == 0);
}

void test_clear_and_formats() {
  Env e;
  Handle bgra = e.alloc(kTarget, Format::A8R8G8B8, 4, 4), rgba = e.alloc(kTarget, Format::A8B8G8R8, 4, 4), x = e.alloc(kTarget, Format::X8R8G8B8, 4, 4);
  CHECK(e.clear(bgra, {1, 0, 0, 1}) == status::kSuccess);
  CHECK(e.pixel(bgra, 2, 2) == 0xFFFF0000);
  CHECK(e.clear(rgba, {1, 0, 0, 1}) == status::kSuccess);
  CHECK(e.pixel(rgba, 0, 0) == 0xFF0000FF);                // memory order R, G, B, A
  CHECK(e.clear(x, {0, 0, 1, 1}) == status::kSuccess);
  CHECK((e.pixel(x, 3, 3) & 0xFFFFFF) == 0x0000FF);
  AllocationInfo info;
  CHECK(e.kernel.query_allocation(kPid, bgra, &info) == status::kSuccess && info.resident);
  // CPU writes survive: the backing goes to the device when the target is next used (no clear: the pass loads it).
  CHECK(e.fill(bgra, 0xFF336699));
  CommandWriter w(e.allocator);
  w.begin_pass(0, kNoAllocation, 0, {0, 0, 0, 0}, 1, 0);
  w.end_pass();
  CHECK(e.render(w, {bgra}) == status::kSuccess && e.pixel(bgra, 1, 1) == 0xFF336699);
  // Locking a target that queued or old work wrote reads it back; a discard lock does not.
  CHECK(e.clear(bgra, {0, 1, 0, 1}) == status::kSuccess);
  LockResult r;
  CHECK(e.kernel.lock(kPid, bgra, kLockDiscard, 0, &r) == status::kSuccess);
  CHECK(e.kernel.unlock(kPid, bgra) == status::kSuccess);
}

void test_triangle() {
  Env e;
  Handle rt = e.alloc(kTarget, Format::A8R8G8B8, 16, 16), vb = e.buffer(kAllocVertexBuffer, 3 * 28);
  Handle pipe = make_colored_pipeline(e);
  CHECK(write_triangle(e, vb, 0, 1, 0));
  const float identity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
  CommandWriter w(e.allocator);
  w.begin_pass(0, kNoAllocation, kPassClearColor, {0, 0, 0, 1}, 1, 0);
  w.bind_pipeline(pipe);
  w.bind_vertex_buffer(1);
  w.push_constants(floats(identity, 16));
  w.set_viewport({0, 0, 16, 16, 0, 1});
  w.set_scissor({0, 0, 16, 16});
  w.draw(3);
  w.end_pass();
  CHECK(w.ok() && e.render(w, {rt, vb}) == status::kSuccess);
  CHECK(e.pixel(rt, 8, 8) == 0xFF00FF00 && e.pixel(rt, 0, 0) == 0xFF00FF00 && e.pixel(rt, 15, 15) == 0xFF00FF00);

  // A scissor limits the draw; the pass clears first.
  CommandWriter w2(e.allocator);
  w2.begin_pass(0, kNoAllocation, kPassClearColor, {0, 0, 0, 1}, 1, 0);
  w2.bind_pipeline(pipe);
  w2.bind_vertex_buffer(1);
  w2.push_constants(floats(identity, 16));
  w2.set_scissor({0, 0, 8, 16});
  w2.draw(3);
  w2.end_pass();
  CHECK(e.render(w2, {rt, vb}) == status::kSuccess);
  CHECK(e.pixel(rt, 2, 8) == 0xFF00FF00 && e.pixel(rt, 12, 8) == 0xFF000000);

  // Indexed, with a vertex offset: the same triangle from indices 0..2 and a buffer holding it after one unused vertex.
  Handle vb2 = e.buffer(kAllocVertexBuffer, 4 * 28), ib = e.buffer(kAllocIndexBuffer, 12);
  const float v[28] = {9, 9, 9, 1, 1, 1, 1, -1, -1, 0.5f, 1, 0, 0, 1, 3, -1, 0.5f, 1, 0, 0, 1, -1, 3, 0.5f, 1, 0, 0, 1};
  CHECK(e.write(vb2, floats(v, 28)));
  const uint8_t idx[6] = {0, 0, 1, 0, 2, 0};
  CHECK(e.write(ib, ConstByteView(idx, 6)));
  CommandWriter w3(e.allocator);
  w3.begin_pass(0, kNoAllocation, kPassClearColor, {0, 0, 0, 1}, 1, 0);
  w3.bind_pipeline(pipe);
  w3.bind_vertex_buffer(1);
  w3.bind_index_buffer(2, graphics::IndexType::Uint16);
  w3.push_constants(floats(identity, 16));
  w3.draw_indexed(3, 0, 1);
  w3.end_pass();
  CHECK(e.render(w3, {rt, vb2, ib}) == status::kSuccess && e.pixel(rt, 8, 8) == 0xFFFF0000);

  // Depth: a second, nearer pass of another colour wins, a farther one does not.
  Handle ds = e.alloc(kAllocDepthStencil, Format::D32Float, 16, 16);
  Handle depth_pipe = 0;
  {
    Handle vs, fs;
    e.kernel.create_shader(kPid, e.dev, {ShaderKind::ColoredVertex, graphics::ShaderStage::Vertex, {}}, &vs);
    e.kernel.create_shader(kPid, e.dev, {ShaderKind::ColoredFragment, graphics::ShaderStage::Fragment, {}}, &fs);
    graphics::GraphicsPipelineDesc d;
    graphics::describe_colored_pipeline(&d);
    PipelineDesc pd;
    pd.vertex_shader = vs, pd.fragment_shader = fs, pd.vertex_stride = d.vertex_stride, pd.attribute_count = 2, pd.varying_count = 1;
    pd.attributes[0] = d.attributes[0], pd.attributes[1] = d.attributes[1];
    pd.cull_mode = graphics::CullMode::None;
    pd.depth_test = pd.depth_write = true;
    CHECK(e.kernel.create_pipeline(kPid, e.dev, pd, &depth_pipe) == status::kSuccess);
  }
  Handle far_vb = e.buffer(kAllocVertexBuffer, 3 * 28);
  const float far_v[21] = {-1, -1, 0.9f, 0, 0, 1, 1, 3, -1, 0.9f, 0, 0, 1, 1, -1, 3, 0.9f, 0, 0, 1, 1};
  CHECK(e.write(far_vb, floats(far_v, 21)));
  CHECK(write_triangle(e, vb, 0, 1, 0));   // (depth 0.5: nearer)
  CommandWriter w4(e.allocator);
  w4.begin_pass(0, 1, kPassClearColor | kPassClearDepth, {0, 0, 0, 1}, 1, 0);
  w4.bind_pipeline(depth_pipe);
  w4.push_constants(floats(identity, 16));
  w4.bind_vertex_buffer(2);
  w4.draw(3);        // green at 0.5
  w4.bind_vertex_buffer(3);
  w4.draw(3);        // blue at 0.9: behind
  w4.end_pass();
  CHECK(e.render(w4, {rt, ds, vb, far_vb}) == status::kSuccess && e.pixel(rt, 8, 8) == 0xFF00FF00);
}

void test_texture_copy_mips() {
  Env e;
  Handle rt = e.alloc(kTarget, Format::A8R8G8B8, 8, 8), tex = e.alloc(kTexture, Format::A8R8G8B8, 4, 4), vb = e.buffer(kAllocVertexBuffer, 4 * 20);
  CHECK(e.fill(tex, 0xFFC08040));
  Handle pipe = make_colored_pipeline(e, true);
  const float quad[20] = {-1, -1, 0.5f, 0, 0, 3, -1, 0.5f, 2, 0, -1, 3, 0.5f, 0, 2};   // (one big triangle: 15 floats)
  CHECK(e.write(vb, floats(quad, 15)));
  const float identity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
  CommandWriter w(e.allocator);
  w.begin_pass(0, kNoAllocation, kPassClearColor, {0, 0, 0, 1}, 1, 0);
  w.bind_pipeline(pipe);
  w.bind_vertex_buffer(2);
  w.push_constants(floats(identity, 16));
  w.bind_texture(0, 1, graphics::SamplerDesc());
  w.draw(3);
  w.end_pass();
  CHECK(e.render(w, {rt, tex, vb}) == status::kSuccess && e.pixel(rt, 4, 4) == 0xFFC08040);
  // Drawing with the texture unbound is refused; so is drawing into a bound texture.
  CommandWriter w_unbound(e.allocator);
  w_unbound.begin_pass(0, kNoAllocation, 0, {0, 0, 0, 0}, 1, 0);
  w_unbound.bind_pipeline(pipe);
  w_unbound.bind_vertex_buffer(2);
  w_unbound.draw(3);
  w_unbound.end_pass();
  CHECK(e.render(w_unbound, {rt, tex, vb}) == status::kInvalidParameter);
  Handle tex_rt = e.alloc(kTexture | kTarget, Format::A8R8G8B8, 8, 8);
  CommandWriter w_self(e.allocator);
  w_self.bind_texture(0, 0, graphics::SamplerDesc());
  w_self.begin_pass(0, kNoAllocation, 0, {0, 0, 0, 0}, 1, 0);
  w_self.end_pass();
  CHECK(e.render(w_self, {tex_rt}) == status::kInvalidParameter);
  // A render target is used as a texture afterwards.
  CommandWriter w2(e.allocator);
  w2.begin_pass(0, kNoAllocation, kPassClearColor, {0, 0, 1, 1}, 1, 0);
  w2.end_pass();
  CHECK(e.render(w2, {tex_rt}) == status::kSuccess);
  CommandWriter w3(e.allocator);
  w3.begin_pass(0, kNoAllocation, kPassClearColor, {0, 0, 0, 1}, 1, 0);
  w3.bind_pipeline(pipe);
  w3.bind_vertex_buffer(2);
  w3.push_constants(floats(identity, 16));
  w3.bind_texture(0, 1, graphics::SamplerDesc());
  w3.draw(3);
  w3.end_pass();
  CHECK(e.render(w3, {rt, tex_rt, vb}) == status::kSuccess && e.pixel(rt, 4, 4) == 0xFF0000FF);

  // CopyRegion: a 2 x 2 block from (1, 1) of a ramp to (0, 0), then an overlapping copy within one image.
  Handle src = e.alloc(kTexture, Format::A8R8G8B8, 4, 4), dst = e.alloc(kTexture, Format::A8R8G8B8, 4, 4), other_format = e.alloc(kTexture, Format::X8R8G8B8, 4, 4);
  uint32_t ramp[16];
  for (uint32_t i = 0; i < 16; ++i) ramp[i] = 0xFF000000 | i;
  CHECK(e.write(src, ConstByteView(reinterpret_cast<const uint8_t*>(ramp), sizeof(ramp))));
  CommandWriter c(e.allocator);
  c.copy_region(0, 0, 0, 1, 1, 1, 2, 2);
  CHECK(e.render(c, {dst, src}) == status::kSuccess);
  CHECK(e.pixel(dst, 0, 0) == 0xFF000005 && e.pixel(dst, 1, 1) == 0xFF00000A && e.pixel(dst, 2, 2) == 0);
  CommandWriter c2(e.allocator);
  c2.copy_region(0, 0, 1, 0, 0, 0, 4, 3);   // rows 0..2 to rows 1..3 of the same image
  CHECK(e.render(c2, {src}) == status::kSuccess);
  CHECK(e.pixel(src, 0, 1) == 0xFF000000 && e.pixel(src, 3, 3) == 0xFF00000B && e.pixel(src, 0, 0) == 0xFF000000);
  CommandWriter bad(e.allocator);
  bad.copy_region(0, 3, 3, 1, 0, 0, 2, 2);
  CHECK(e.render(bad, {dst, src}) == status::kInvalidParameter);   // past the edge
  CommandWriter bad2(e.allocator);
  bad2.copy_region(0, 0, 0, 1, 0, 0, 2, 2);
  CHECK(e.render(bad2, {dst, other_format}) == status::kInvalidParameter);   // formats differ

  // Mipmaps: level 0 all one colour, so are the others once generated; and each level is lockable on its own.
  Handle mip = e.alloc(kTexture, Format::A8R8G8B8, 4, 4, 3);
  CHECK(e.fill(mip, 0xFF204060, 0));
  CommandWriter g(e.allocator);
  g.generate_mipmaps(0);
  CHECK(e.render(g, {mip}) == status::kSuccess);
  CHECK(e.pixel(mip, 1, 1, 1) == 0xFF204060 && e.pixel(mip, 0, 0, 2) == 0xFF204060);
  LockResult r;
  CHECK(e.kernel.lock(kPid, mip, 0, 1, &r) == status::kSuccess && r.pitch == 8 && r.data.size() == 16);
  CHECK(e.kernel.unlock(kPid, mip) == status::kSuccess);
  CommandWriter g2(e.allocator);
  g2.generate_mipmaps(0);
  CHECK(e.render(g2, {tex}) == status::kInvalidParameter);   // one level only
}

// Every prefix of a good stream, and every single-bit change, must be answered with a status (and never touch memory it should not: the sanitizers watch).
void test_stream_fuzz() {
  Env e;
  Handle rt = e.alloc(kTarget, Format::A8R8G8B8, 8, 8), vb = e.buffer(kAllocVertexBuffer, 3 * 28), ib = e.buffer(kAllocIndexBuffer, 64), tex = e.alloc(kTexture, Format::A8R8G8B8, 4, 4);
  AllocationDesc query_desc;
  query_desc.flags = kAllocQueryResult, query_desc.size = 16;
  Handle queries = 0;
  CHECK(e.kernel.create_allocation(kPid, e.dev, query_desc, &queries) == status::kSuccess);
  AllocationDesc ms_desc;
  ms_desc.flags = kAllocRenderTarget, ms_desc.format = Format::A8R8G8B8, ms_desc.width = ms_desc.height = 8, ms_desc.samples = 4;
  Handle ms_target = 0;
  CHECK(e.kernel.create_allocation(kPid, e.dev, ms_desc, &ms_target) == status::kSuccess);
  Handle pipe = make_colored_pipeline(e);
  CHECK(write_triangle(e, vb, 1, 1, 0));
  const float identity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
  CommandWriter w(e.allocator);
  w.begin_pass(0, kNoAllocation, kPassClearColor, {0, 0, 0, 1}, 1, 0);
  w.bind_pipeline(pipe);
  w.bind_vertex_buffer(1);
  w.bind_index_buffer(2, graphics::IndexType::Uint16);
  w.push_constants(floats(identity, 16));
  w.set_viewport({0, 0, 8, 8, 0, 1});
  w.set_scissor({0, 0, 8, 8});
  w.begin_query(4, 8);
  w.draw(3);
  w.end_query();
  w.draw_indexed(3);
  w.end_pass();
  w.resolve_region(0, 5);
  w.copy_region(3, 0, 0, 3, 1, 1, 2, 2);
  w.generate_mipmaps(3);
  CHECK(w.ok());
  std::vector<uint8_t> good(w.view().data(), w.view().data() + w.view().size());
  std::vector<Handle> list = {rt, vb, ib, tex, queries, ms_target};
  auto try_stream = [&](const std::vector<uint8_t>& bytes) {
    Handle ctx = 0;
    e.kernel.create_context(kPid, e.dev, ContextPriority::Normal, &ctx);
    NtStatus s = e.kernel.render(kPid, ctx, ConstByteView(bytes.data(), bytes.size()), ArrayView<const Handle>(list.data(), list.size()));
    e.kernel.destroy_context(kPid, ctx);
    return s;
  };
  CHECK(try_stream(good) == status::kInvalidParameter);   // (the copy and mipmaps name a texture of one level: generate_mipmaps is refused)
  good.resize(good.size() - 8);                             // drop GenerateMipmaps
  CHECK(try_stream(good) == status::kSuccess);

  uint32_t accepted = 0, refused = 0;
  for (size_t len = 0; len < good.size(); ++len) {
    std::vector<uint8_t> t(good.begin(), good.begin() + len);
    NtStatus s = try_stream(t);
    if (len == 0 || len % 4) CHECK(s == status::kInvalidParameter);
    s == status::kSuccess ? ++accepted : ++refused;
  }
  for (size_t byte = 0; byte < good.size(); ++byte)
    for (int bit = 0; bit < 8; ++bit) {
      std::vector<uint8_t> t = good;
      t[byte] ^= static_cast<uint8_t>(1 << bit);
      NtStatus s = try_stream(t);
      CHECK(s == status::kSuccess || s == status::kInvalidParameter || s == status::kInvalidHandle || s == status::kDeviceRemoved || s == status::kGraphicsNoVideoMemory);
      s == status::kSuccess ? ++accepted : ++refused;
    }
  CHECK(refused > 0 && accepted > 0);
  printf("  stream fuzz: %u accepted, %u refused\n", accepted, refused);
  // Random garbage.
  uint32_t seed = 12345;
  for (int i = 0; i < 300; ++i) {
    std::vector<uint8_t> t(4 * (1 + i % 40));
    for (uint8_t& b : t) { seed = seed * 1664525u + 1013904223u; b = static_cast<uint8_t>(seed >> 24); }
    try_stream(t);
  }
}

void test_stream_errors() {
  Env e;
  Handle rt = e.alloc(kTarget, Format::A8R8G8B8, 8, 8), vb = e.buffer(kAllocVertexBuffer, 3 * 28), ib = e.buffer(kAllocIndexBuffer, 12), tex = e.alloc(kTexture, Format::A8R8G8B8, 4, 4);
  Handle pipe = make_colored_pipeline(e);
  const float identity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
  auto expect = [&](const char* what, CommandWriter& w, std::initializer_list<Handle> list, NtStatus s) {
    NtStatus got = e.render(w, list);
    if (got != s) printf("  %s: %08x\n", what, got);
    CHECK(got == s);
  };
  { CommandWriter w(e.allocator); w.draw(3); expect("draw outside a pass", w, {rt}, status::kInvalidParameter); }
  { CommandWriter w(e.allocator); w.begin_pass(0, kNoAllocation, 0, {}, 1, 0); expect("pass left open", w, {rt}, status::kInvalidParameter); }
  { CommandWriter w(e.allocator); w.end_pass(); expect("end without begin", w, {rt}, status::kInvalidParameter); }
  { CommandWriter w(e.allocator); w.begin_pass(5, kNoAllocation, 0, {}, 1, 0); w.end_pass(); expect("index out of the list", w, {rt}, status::kInvalidParameter); }
  { CommandWriter w(e.allocator); w.begin_pass(1, kNoAllocation, 0, {}, 1, 0); w.end_pass(); expect("a buffer as target", w, {rt, vb}, status::kInvalidParameter); }
  { CommandWriter w(e.allocator); w.begin_pass(1, kNoAllocation, 0, {}, 1, 0); w.end_pass(); expect("a texture as target", w, {rt, tex}, status::kInvalidParameter); }
  { CommandWriter w(e.allocator); w.begin_pass(0, kNoAllocation, kPassClearDepth, {}, 2.0f, 0); w.end_pass(); expect("clear depth out of range", w, {rt}, status::kInvalidParameter); }
  { CommandWriter w(e.allocator); w.begin_pass(0, kNoAllocation, 0, {NAN, 0, 0, 0}, 1, 0); w.end_pass(); expect("NaN clear colour", w, {rt}, status::kInvalidParameter); }
  { CommandWriter w(e.allocator); w.begin_pass(0, kNoAllocation, 0, {}, 1, 0); w.draw(3); w.end_pass(); expect("draw without a pipeline", w, {rt}, status::kInvalidParameter); }
  { CommandWriter w(e.allocator); w.begin_pass(0, kNoAllocation, 0, {}, 1, 0); w.bind_pipeline(pipe); w.draw(3); w.end_pass(); expect("draw without vertices", w, {rt}, status::kInvalidParameter); }
  { CommandWriter w(e.allocator); w.begin_pass(0, kNoAllocation, 0, {}, 1, 0); w.bind_pipeline(0x12345); w.end_pass(); expect("no such pipeline", w, {rt}, status::kInvalidHandle); }
  { CommandWriter w(e.allocator); w.begin_pass(0, kNoAllocation, 0, {}, 1, 0); w.bind_pipeline(pipe); w.bind_vertex_buffer(1); w.draw(4); w.end_pass(); expect("draw past the vertex buffer", w, {rt, vb}, status::kInvalidParameter); }
  { CommandWriter w(e.allocator); w.begin_pass(0, kNoAllocation, 0, {}, 1, 0); w.bind_pipeline(pipe); w.bind_vertex_buffer(1, 28); w.draw(3); w.end_pass(); expect("draw past it from an offset", w, {rt, vb}, status::kInvalidParameter); }
  { CommandWriter w(e.allocator); w.begin_pass(0, kNoAllocation, 0, {}, 1, 0); w.bind_vertex_buffer(1, 1000); w.end_pass(); expect("offset past the buffer", w, {rt, vb}, status::kInvalidParameter); }
  { CommandWriter w(e.allocator); w.begin_pass(0, kNoAllocation, 0, {}, 1, 0); w.bind_index_buffer(1, graphics::IndexType::Uint32, 2); w.end_pass(); expect("misaligned index buffer", w, {rt, ib}, status::kInvalidParameter); }
  { CommandWriter w(e.allocator); w.begin_pass(0, kNoAllocation, 0, {}, 1, 0); w.bind_pipeline(pipe); w.bind_vertex_buffer(1); w.bind_index_buffer(2, graphics::IndexType::Uint16);
    w.draw_indexed(7); w.end_pass(); expect("too many indices", w, {rt, vb, ib}, status::kInvalidParameter); }
  { CommandWriter w(e.allocator); w.begin_pass(0, kNoAllocation, 0, {}, 1, 0); w.push_constants(ConstByteView(reinterpret_cast<const uint8_t*>(identity), 12)); w.end_pass();
    expect("push constants not a multiple of 16", w, {rt}, status::kInvalidParameter); }
  { CommandWriter w(e.allocator); w.begin_pass(0, kNoAllocation, 0, {}, 1, 0); w.set_viewport({0, 0, -1, 1, 0, 1}); w.end_pass(); expect("negative viewport", w, {rt}, status::kInvalidParameter); }
  { CommandWriter w(e.allocator); w.begin_pass(0, kNoAllocation, 0, {}, 1, 0); w.set_scissor({0, 0, 1u << 20, 1}); w.end_pass(); expect("huge scissor", w, {rt}, status::kInvalidParameter); }
  { CommandWriter w(e.allocator); w.begin_pass(0, kNoAllocation, 0, {}, 1, 0); w.begin_pass(0, kNoAllocation, 0, {}, 1, 0); expect("nested pass", w, {rt}, status::kInvalidParameter); }
  { CommandWriter w(e.allocator); w.begin_pass(0, kNoAllocation, 0, {}, 1, 0); w.copy_region(0, 0, 0, 0, 0, 0, 1, 1); w.end_pass(); expect("copy inside a pass", w, {rt}, status::kInvalidParameter); }
  // A handle that is not an allocation, an allocation of another process, an empty stream, an oversized list.
  { CommandWriter w(e.allocator); w.begin_pass(0, kNoAllocation, 0, {}, 1, 0); w.end_pass(); expect("bad allocation handle", w, {0x777}, status::kInvalidHandle); }
  { CommandWriter w(e.allocator); expect("empty stream", w, {rt}, status::kInvalidParameter); }
  CHECK(e.kernel.render(kPid, 0x99, ConstByteView(), ArrayView<const Handle>()) == status::kInvalidHandle);
  // The device is still fine after all of that.
  CHECK(e.clear(rt, {1, 1, 1, 1}) == status::kSuccess && e.pixel(rt, 0, 0) == 0xFFFFFFFF);
  DeviceState st;
  CHECK(e.kernel.get_device_state(kPid, e.dev, &st) == status::kSuccess && st.execution == DeviceExecState::Running);
}

// A stream that is fine to look at but names vertices that are not there: caught when it runs (the index buffer can change after the check), and the context is lost.
void test_fault() {
  Env e;
  Handle rt = e.alloc(kTarget, Format::A8R8G8B8, 8, 8), vb = e.buffer(kAllocVertexBuffer, 3 * 28), ib = e.buffer(kAllocIndexBuffer, 6);
  Handle pipe = make_colored_pipeline(e);
  CHECK(write_triangle(e, vb, 1, 0, 0));
  const uint8_t good_idx[6] = {0, 0, 1, 0, 2, 0}, bad_idx[6] = {0, 0, 1, 0, 7, 0};
  CHECK(e.write(ib, ConstByteView(good_idx, 6)));
  const float identity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
  CommandWriter w(e.allocator);
  w.begin_pass(0, kNoAllocation, 0, {}, 1, 0);
  w.bind_pipeline(pipe);
  w.bind_vertex_buffer(1);
  w.bind_index_buffer(2, graphics::IndexType::Uint16);
  w.push_constants(floats(identity, 16));
  w.draw_indexed(3);
  w.end_pass();
  CHECK(e.render(w, {rt, vb, ib}) == status::kSuccess);

  // Queued work keeps the CPU out of the buffers it uses (until the fence it waits for is signalled).
  Handle fence = 0;
  CHECK(e.kernel.create_sync(kPid, e.dev, SyncType::Fence, 0, 0, &fence) == status::kSuccess);
  CHECK(e.kernel.wait_sync_gpu(kPid, e.ctx, fence, 1) == status::kSuccess);
  CHECK(e.render(w, {rt, vb, ib}) == status::kSuccess);
  LockResult r;
  CHECK(e.kernel.lock(kPid, ib, kLockDoNotWait, 0, &r) == status::kGraphicsAllocationBusy);
  uint64_t value = 0;
  const uint64_t one = 1;
  CHECK(e.kernel.signal_sync_cpu(kPid, ArrayView<const Handle>(&fence, 1), ArrayView<const uint64_t>(&one, 1)) == status::kSuccess);
  bool idle = false;
  CHECK(e.kernel.context_idle(kPid, e.ctx, &idle) == status::kSuccess && idle);
  CHECK(e.kernel.lock(kPid, ib, kLockDoNotWait, 0, &r) == status::kSuccess && e.kernel.unlock(kPid, ib) == status::kSuccess);

  // Now a stream whose indices are bad from the start fails when it runs.
  Handle ib2 = e.buffer(kAllocIndexBuffer, 6);
  CHECK(e.write(ib2, ConstByteView(bad_idx, 6)));
  Handle gate = 0, after = 0;
  CHECK(e.kernel.create_sync(kPid, e.dev, SyncType::Fence, 0, 0, &gate) == status::kSuccess && e.kernel.create_sync(kPid, e.dev, SyncType::Fence, 0, 0, &after) == status::kSuccess);
  Handle ctx2 = 0;
  CHECK(e.kernel.create_context(kPid, e.dev, ContextPriority::Normal, &ctx2) == status::kSuccess);
  CHECK(e.kernel.wait_sync_gpu(kPid, ctx2, gate, 1) == status::kSuccess);
  CHECK(e.render(w, {rt, vb, ib2}, ctx2) == status::kSuccess);
  CHECK(e.kernel.signal_sync_gpu(kPid, ctx2, after, 1) == status::kSuccess);
  CHECK(e.kernel.signal_sync_cpu(kPid, ArrayView<const Handle>(&gate, 1), ArrayView<const uint64_t>(&one, 1)) == status::kSuccess);
  DeviceState st;
  CHECK(e.kernel.get_device_state(kPid, e.dev, &st) == status::kSuccess && st.execution == DeviceExecState::PageFault && st.reset_count == 1);
  CHECK(e.kernel.query_sync(kPid, after, &value) == status::kSuccess && value == 1);   // (the signal behind the failed work still happened)
  CHECK(e.render(w, {rt, vb, ib}, ctx2) == status::kDeviceRemoved);                       // the context is gone
  CHECK(e.render(w, {rt, vb, ib}) == status::kSuccess);                                   // another context of the device is not
}

void test_fences_and_scheduler() {
  Env e;
  Handle rt = e.alloc(kTarget, Format::A8R8G8B8, 4, 4);
  Handle f = 0, f2 = 0;
  CHECK(e.kernel.create_sync(kPid, e.dev, SyncType::Fence, 0, 0, &f) == status::kSuccess && e.kernel.create_sync(kPid, e.dev, SyncType::Fence, 0, 0, &f2) == status::kSuccess);
  auto signal_cpu = [&](Handle h, uint64_t v) { return e.kernel.signal_sync_cpu(kPid, ArrayView<const Handle>(&h, 1), ArrayView<const uint64_t>(&v, 1)); };
  auto query = [&](Handle h) { uint64_t v = 0; e.kernel.query_sync(kPid, h, &v); return v; };

  // A wait holds the context's later work back.
  CHECK(e.kernel.wait_sync_gpu(kPid, e.ctx, f, 1) == status::kSuccess);
  CHECK(e.clear(rt, {1, 0, 0, 1}) == status::kSuccess);
  bool idle = true;
  CHECK(e.kernel.context_idle(kPid, e.ctx, &idle) == status::kSuccess && !idle);
  LockResult r;
  CHECK(e.kernel.lock(kPid, rt, 0, 0, &r) == status::kGraphicsAllocationBusy);
  CHECK(e.pixel(rt, 0, 0) == 0xDEADDEAD);
  CHECK(signal_cpu(f, 1) == status::kSuccess);
  CHECK(e.pixel(rt, 0, 0) == 0xFFFF0000);
  CHECK(e.kernel.context_idle(kPid, e.ctx, &idle) == status::kSuccess && idle);
  CHECK(signal_cpu(f, 0) == status::kInvalidParameter && query(f) == 1);   // a fence only goes up

  // Work of two contexts ordered by a fence: B waits for what A signals.
  Handle ctx_a = e.ctx, ctx_b = 0;
  CHECK(e.kernel.create_context(kPid, e.dev, ContextPriority::Normal, &ctx_b) == status::kSuccess);
  CHECK(e.kernel.wait_sync_gpu(kPid, ctx_b, f2, 1) == status::kSuccess);
  CHECK(e.clear(rt, {0, 0, 1, 1}, ctx_b) == status::kSuccess);   // blue, after A's red
  CHECK(e.clear(rt, {1, 0, 0, 1}, ctx_a) == status::kSuccess);
  CHECK(e.kernel.signal_sync_gpu(kPid, ctx_a, f2, 1) == status::kSuccess);
  CHECK(query(f2) == 1 && e.pixel(rt, 1, 1) == 0xFF0000FF);

  // Priority: a high priority context goes first when both can run, so the low one's colour is what is left.
  Handle gate = 0, high = 0, low = 0;
  CHECK(e.kernel.create_sync(kPid, e.dev, SyncType::Fence, 0, 0, &gate) == status::kSuccess);
  CHECK(e.kernel.create_context(kPid, e.dev, ContextPriority::High, &high) == status::kSuccess && e.kernel.create_context(kPid, e.dev, ContextPriority::Low, &low) == status::kSuccess);
  CHECK(e.kernel.wait_sync_gpu(kPid, low, gate, 1) == status::kSuccess && e.clear(rt, {1, 0, 0, 1}, low) == status::kSuccess);
  CHECK(e.kernel.wait_sync_gpu(kPid, high, gate, 1) == status::kSuccess && e.clear(rt, {0, 1, 0, 1}, high) == status::kSuccess);
  CHECK(signal_cpu(gate, 1) == status::kSuccess);
  CHECK(e.pixel(rt, 0, 0) == 0xFFFF0000);
  // Semaphores count.
  Handle sem = 0;
  CHECK(e.kernel.create_sync(kPid, e.dev, SyncType::Semaphore, 0, 0, &sem) == status::kInvalidParameter);
  CHECK(e.kernel.create_sync(kPid, e.dev, SyncType::Semaphore, 3, 2, &sem) == status::kInvalidParameter);
  CHECK(e.kernel.create_sync(kPid, e.dev, SyncType::Semaphore, 0, 2, &sem) == status::kSuccess);
  CHECK(signal_cpu(sem, 0) == status::kSuccess && signal_cpu(sem, 0) == status::kSuccess && signal_cpu(sem, 0) == status::kInvalidParameter && query(sem) == 2);
  const uint64_t zero = 0;
  CHECK(e.kernel.wait_sync_cpu(kPid, ArrayView<const Handle>(&sem, 1), ArrayView<const uint64_t>(&zero, 1), true, 0) == status::kSuccess && query(sem) == 1);
  CHECK(e.kernel.wait_sync_cpu(kPid, ArrayView<const Handle>(&sem, 1), ArrayView<const uint64_t>(&zero, 1), true, 0) == status::kSuccess && query(sem) == 0);
  CHECK(e.kernel.wait_sync_cpu(kPid, ArrayView<const Handle>(&sem, 1), ArrayView<const uint64_t>(&zero, 1), true, 0) == status::kTimeout);
  // A GPU wait on a semaphore takes the signal when it runs.
  CHECK(e.kernel.wait_sync_gpu(kPid, e.ctx, sem, 0) == status::kSuccess && e.clear(rt, {0, 1, 0, 1}) == status::kSuccess && e.pixel(rt, 0, 0) == 0xDEADDEAD);   // (held back: busy)
  CHECK(signal_cpu(sem, 0) == status::kSuccess && query(sem) == 0 && e.pixel(rt, 0, 0) == 0xFF00FF00);

  // Waiting on the CPU: any/all, with a hook that plays the thread that signals.
  static Env* env = &e;
  static Handle ff = 0;
  Handle g1 = 0, g2 = 0;
  CHECK(e.kernel.create_sync(kPid, e.dev, SyncType::Fence, 0, 0, &g1) == status::kSuccess && e.kernel.create_sync(kPid, e.dev, SyncType::Fence, 0, 0, &g2) == status::kSuccess);
  ff = g2;
  FakeHook hook;
  e.kernel.set_wait_hook(&hook);
  const Handle pair[2] = {g1, g2};
  const uint64_t targets[2] = {4, 7};
  CHECK(e.kernel.wait_sync_cpu(kPid, ArrayView<const Handle>(pair, 2), ArrayView<const uint64_t>(targets, 2), false, 5000000) == status::kTimeout);   // nobody signals: time passes
  CHECK(hook.waits >= 1 && hook.now >= 5000000);
  hook.on_wait = [] { const uint64_t v = 7; env->kernel.signal_sync_cpu(kPid, ArrayView<const Handle>(&ff, 1), ArrayView<const uint64_t>(&v, 1)); };
  CHECK(e.kernel.wait_sync_cpu(kPid, ArrayView<const Handle>(pair, 2), ArrayView<const uint64_t>(targets, 2), false, ~uint64_t{0}) == status::kSuccess);   // any: g2 got there
  CHECK(e.kernel.wait_sync_cpu(kPid, ArrayView<const Handle>(pair, 2), ArrayView<const uint64_t>(targets, 2), true, 1000000) == status::kTimeout);        // all: g1 never does
  hook.on_wait = nullptr;
  CHECK(hook.wakes > 0);
  e.kernel.set_wait_hook(nullptr);
  // Bad arguments.
  CHECK(e.kernel.wait_sync_cpu(kPid, ArrayView<const Handle>(), ArrayView<const uint64_t>(), true, 0) == status::kInvalidParameter);
  CHECK(e.kernel.wait_sync_cpu(kPid, ArrayView<const Handle>(pair, 2), ArrayView<const uint64_t>(targets, 1), true, 0) == status::kInvalidParameter);
  const Handle junk = 0xABCD;
  CHECK(e.kernel.wait_sync_cpu(kPid, ArrayView<const Handle>(&junk, 1), ArrayView<const uint64_t>(&zero, 1), true, 0) == status::kInvalidHandle);
  CHECK(e.kernel.destroy_sync(kPid, g1) == status::kSuccess && e.kernel.destroy_sync(kPid, g1) == status::kInvalidHandle);

  // Destroying a context that still owes a signal does not leave anyone waiting for it.
  Handle owed = 0, doomed = 0;
  CHECK(e.kernel.create_sync(kPid, e.dev, SyncType::Fence, 0, 0, &owed) == status::kSuccess && e.kernel.create_sync(kPid, e.dev, SyncType::Fence, 0, 0, &gate) == status::kSuccess);
  CHECK(e.kernel.create_context(kPid, e.dev, ContextPriority::Normal, &doomed) == status::kSuccess);
  CHECK(e.kernel.wait_sync_gpu(kPid, doomed, gate, 99) == status::kSuccess && e.kernel.signal_sync_gpu(kPid, doomed, owed, 3) == status::kSuccess);
  CHECK(query(owed) == 0 && e.kernel.destroy_context(kPid, doomed) == status::kSuccess && query(owed) == 3);
}

void test_residency() {
  // Two 16 x 16 targets (1024 bytes each) fit; a third makes the least recently used leave.
  Env e(2 * 1024 + 100);
  Handle a = e.alloc(kTarget, Format::A8R8G8B8, 16, 16), b = e.alloc(kTarget, Format::A8R8G8B8, 16, 16), c = e.alloc(kTarget, Format::A8R8G8B8, 16, 16);
  CHECK(e.clear(a, {1, 0, 0, 1}) == status::kSuccess && e.clear(b, {0, 1, 0, 1}) == status::kSuccess);
  AllocationInfo ia, ib, ic;
  e.kernel.query_allocation(kPid, a, &ia);
  e.kernel.query_allocation(kPid, b, &ib);
  CHECK(ia.resident && ib.resident);
  CHECK(e.clear(c, {0, 0, 1, 1}) == status::kSuccess);
  e.kernel.query_allocation(kPid, a, &ia);
  e.kernel.query_allocation(kPid, b, &ib);
  e.kernel.query_allocation(kPid, c, &ic);
  CHECK(!ia.resident && ib.resident && ic.resident);
  CHECK(e.kernel.adapter(0)->vidmm().resident_bytes() == 2048 && e.kernel.adapter(0)->vidmm().resident_count() == 2);
  VideoMemoryInfo vm;
  CHECK(e.kernel.query_video_memory_info(kPid, e.adapter, MemorySegmentGroup::Local, &vm) == status::kSuccess && vm.current_usage == 2048 && vm.budget == 2148);
  // What was evicted is not lost.
  CHECK(e.pixel(a, 5, 5) == 0xFFFF0000 && e.pixel(b, 5, 5) == 0xFF00FF00 && e.pixel(c, 5, 5) == 0xFF0000FF);
  // Using the evicted one brings it back (and pushes another out); a pass that loads keeps the contents the CPU gave it.
  CHECK(e.fill(a, 0xFF445566));
  CommandWriter w(e.allocator);
  w.begin_pass(0, kNoAllocation, 0, {}, 1, 0);
  w.end_pass();
  CHECK(e.render(w, {a}) == status::kSuccess && e.pixel(a, 9, 9) == 0xFF445566);
  // All three at once cannot be resident.
  const Handle all[3] = {a, b, c};
  CHECK(e.kernel.make_resident(kPid, e.dev, ArrayView<const Handle>(all, 3)) == status::kGraphicsNoVideoMemory);
  CHECK(e.kernel.make_resident(kPid, e.dev, ArrayView<const Handle>(all, 2)) == status::kSuccess);
  CHECK(e.kernel.evict(kPid, e.dev, ArrayView<const Handle>(all, 3)) == status::kSuccess && e.kernel.adapter(0)->vidmm().resident_count() == 0);
  CHECK(e.pixel(b, 0, 0) == 0xFF00FF00);
  CHECK(e.kernel.make_resident(kPid, e.dev, ArrayView<const Handle>(all, 1)) == status::kSuccess && e.kernel.trim(kPid, e.dev) == status::kSuccess && e.kernel.adapter(0)->vidmm().resident_count() == 0);
  // A buffer and a staging image: the staging one is never resident.
  Handle st = e.alloc(kAllocStaging, Format::A8R8G8B8, 4, 4);
  CHECK(e.kernel.make_resident(kPid, e.dev, ArrayView<const Handle>(&st, 1)) == status::kInvalidParameter);
  // Something bigger than the whole budget: the pass fails and the device says it ran out of memory.
  Handle huge = e.alloc(kTarget, Format::A8R8G8B8, 64, 64);
  CHECK(e.kernel.make_resident(kPid, e.dev, ArrayView<const Handle>(&huge, 1)) == status::kGraphicsNoVideoMemory);
  Handle ctx2 = 0;
  CHECK(e.kernel.create_context(kPid, e.dev, ContextPriority::Normal, &ctx2) == status::kSuccess);
  CHECK(e.clear(huge, {1, 1, 1, 1}, ctx2) == status::kSuccess);   // (the stream is fine; running it is what fails)
  DeviceState st2;
  CHECK(e.kernel.get_device_state(kPid, e.dev, &st2) == status::kSuccess && st2.execution == DeviceExecState::ErrorOutOfMemory && st2.reset_count == 1);
  // A pinned working set: while one stream runs, its allocations are not evicted to make room for each other.
  Handle ctx3 = 0;
  CHECK(e.kernel.create_context(kPid, e.dev, ContextPriority::Normal, &ctx3) == status::kSuccess);
  CommandWriter two(e.allocator);
  two.begin_pass(0, kNoAllocation, kPassClearColor, {1, 1, 0, 1}, 1, 0);
  two.end_pass();
  two.begin_pass(1, kNoAllocation, kPassClearColor, {0, 1, 1, 1}, 1, 0);
  two.end_pass();
  CHECK(e.render(two, {a, b}, ctx3) == status::kSuccess && e.pixel(a, 0, 0) == 0xFFFFFF00 && e.pixel(b, 0, 0) == 0xFF00FFFF);
  CommandWriter three(e.allocator);
  for (uint32_t i = 0; i < 3; ++i) { three.begin_pass(i, kNoAllocation, 0, {}, 1, 0); three.end_pass(); }
  Handle ctx4 = 0;
  CHECK(e.kernel.create_context(kPid, e.dev, ContextPriority::Normal, &ctx4) == status::kSuccess);
  CHECK(e.render(three, {a, b, c}, ctx4) == status::kSuccess);
  CHECK(e.kernel.get_device_state(kPid, e.dev, &st2) == status::kSuccess && st2.execution == DeviceExecState::ErrorOutOfMemory && st2.reset_count == 2);
}

void test_present() {
  Env e;
  Handle back = e.alloc(kTarget | kAllocPresent, Format::A8R8G8B8, 8, 6), plain = e.alloc(kTarget, Format::A8R8G8B8, 8, 6);
  CHECK(e.clear(back, {0, 1, 0, 1}) == status::kSuccess);
  PresentArgs flip;
  flip.source = back;
  CHECK(e.kernel.present(kPid, e.ctx, flip) == status::kSuccess);
  CHECK(e.screen.presents == 1 && e.screen.at(0, 0) == 0x00FF00 && e.screen.at(7, 5) == 0x00FF00);
  // A flip of an image that cannot be shown is refused.
  PresentArgs bad;
  bad.source = plain;
  CHECK(e.kernel.present(kPid, e.ctx, bad) == status::kInvalidParameter);
  bad.source = 0x4444;
  CHECK(e.kernel.present(kPid, e.ctx, bad) == status::kInvalidHandle);
  // A blit present copies a region into the back buffer and shows that.
  CHECK(e.clear(plain, {0, 0, 1, 1}) == status::kSuccess);
  PresentArgs blit;
  blit.source = plain;
  blit.destination = back;
  blit.source_rect = {2, 2, 4, 3};
  blit.dest_x = 1;
  blit.dest_y = 1;
  CHECK(e.kernel.present(kPid, e.ctx, blit) == status::kSuccess);
  CHECK(e.screen.presents == 2 && e.screen.at(1, 1) == 0x0000FF && e.screen.at(4, 3) == 0x0000FF && e.screen.at(0, 0) == 0x00FF00 && e.screen.at(5, 1) == 0x00FF00);
  blit.dest_x = 6;   // 4 wide from x = 6 does not fit in 8
  CHECK(e.kernel.present(kPid, e.ctx, blit) == status::kInvalidParameter);
  blit.dest_x = -1;
  CHECK(e.kernel.present(kPid, e.ctx, blit) == status::kInvalidParameter);
  blit.dest_x = 0;
  blit.source_rect = {6, 0, 4, 1};
  CHECK(e.kernel.present(kPid, e.ctx, blit) == status::kInvalidParameter);
  blit.destination = plain;   // (no present flag on the destination)
  blit.source_rect = {0, 0, 1, 1};
  blit.source = back;
  CHECK(e.kernel.present(kPid, e.ctx, blit) == status::kInvalidParameter);
  // Presents wait their turn in the queue.
  Handle f = 0;
  CHECK(e.kernel.create_sync(kPid, e.dev, SyncType::Fence, 0, 0, &f) == status::kSuccess && e.kernel.wait_sync_gpu(kPid, e.ctx, f, 1) == status::kSuccess);
  CHECK(e.clear(back, {1, 0, 0, 1}) == status::kSuccess && e.kernel.present(kPid, e.ctx, flip) == status::kSuccess && e.screen.presents == 2);
  const uint64_t one = 1;
  CHECK(e.kernel.signal_sync_cpu(kPid, ArrayView<const Handle>(&f, 1), ArrayView<const uint64_t>(&one, 1)) == status::kSuccess);
  CHECK(e.screen.presents == 3 && e.screen.at(0, 0) == 0xFF0000);
}

void test_sharing_and_teardown() {
  HostAllocator a;
  FakeScreen screen(4, 4);
  graphics::SoftwareDevice dev1(a, &screen), dev2(a, &screen);
  {
    Kernel k(a);
    k.add_adapter(dev1, AdapterConfig());
    k.add_adapter(dev2, AdapterConfig());
    Handle ad1 = 0, d1 = 0, ad2 = 0, d2 = 0, other_adapter = 0, other_dev = 0;
    k.open_process(kPid);
    k.open_process(kPid2);
    CHECK(k.open_adapter_from_index(kPid, 0, &ad1) == status::kSuccess && k.create_device(kPid, ad1, &d1) == status::kSuccess);
    CHECK(k.open_adapter_from_index(kPid2, 0, &ad2) == status::kSuccess && k.create_device(kPid2, ad2, &d2) == status::kSuccess);
    CHECK(k.open_adapter_from_index(kPid2, 1, &other_adapter) == status::kSuccess && k.create_device(kPid2, other_adapter, &other_dev) == status::kSuccess);
    AllocationDesc d;
    d.flags = kAllocTexture | kAllocShared, d.format = Format::A8R8G8B8, d.width = 4, d.height = 4;
    Handle shared = 0, unshared = 0;
    CHECK(k.create_allocation(kPid, d1, d, &shared) == status::kSuccess);
    d.flags = kAllocTexture;
    CHECK(k.create_allocation(kPid, d1, d, &unshared) == status::kSuccess);
    uint32_t token = 0, token2 = 0;
    CHECK(k.share_allocation(kPid, unshared, &token) == status::kAccessDenied);
    CHECK(k.share_allocation(kPid, shared, &token) == status::kSuccess && token);
    CHECK(k.share_allocation(kPid, shared, &token2) == status::kSuccess && token2 != token);
    LockResult r;
    CHECK(k.lock(kPid, shared, 0, 0, &r) == status::kSuccess);
    ArrayView<uint32_t> words;
    CHECK(r.data.try_cast<uint32_t>(&words));
    words[5] = 0xCAFEF00D;
    CHECK(k.unlock(kPid, shared) == status::kSuccess);

    // The other process opens it and sees the same memory; its handle is its own.
    Handle opened = 0;
    AllocationDesc got;
    CHECK(k.open_shared_allocation(kPid2, d2, token, &opened, &got) == status::kSuccess && got.width == 4 && (got.flags & kAllocShared));
    CHECK(k.open_shared_allocation(kPid2, d2, 0x9999, &opened, &got) == status::kNotFound);
    CHECK(k.open_shared_allocation(kPid2, other_dev, token, &opened, &got) == status::kInvalidParameter);   // another adapter's memory
    CHECK(opened != 0);
    CHECK(k.lock(kPid2, opened, kLockReadOnly, 0, &r) == status::kSuccess && r.data.try_cast<uint32_t>(&words) && words[5] == 0xCAFEF00D);
    CHECK(k.unlock(kPid2, opened) == status::kSuccess);
    CHECK(k.close_share(kPid2, token) == status::kAccessDenied);
    CHECK(k.close_share(kPid, token2) == status::kSuccess && k.close_share(kPid, token2) == status::kNotFound);

    // The first process ends: its token goes, the allocation lives on in the second.
    k.close_process(kPid);
    CHECK(k.open_shared_allocation(kPid2, d2, token, &opened, &got) == status::kNotFound);
    CHECK(k.lock(kPid2, opened, kLockReadOnly, 0, &r) == status::kSuccess && r.data.try_cast<uint32_t>(&words) && words[5] == 0xCAFEF00D);
    // (a locked allocation in a process that ends is released with it)
    k.close_process(kPid2);
    k.open_process(kPid2);
    CHECK(k.lock(kPid2, opened, 0, 0, &r) == status::kInvalidHandle);
    // Closing a process twice, or one that never existed, is harmless.
    k.close_process(kPid2);
    k.close_process(kPid2);
    k.close_process(4242);
  }
  CHECK(a.live == 0);
}

// The kernel must give back everything when it goes away with processes still open and work still queued.
void test_teardown_with_work_queued() {
  HostAllocator a;
  FakeScreen screen(4, 4);
  graphics::SoftwareDevice device(a, &screen);
  {
    Kernel k(a);
    k.add_adapter(device, AdapterConfig());
    Handle ad, dv, ctx, rt, fence;
    k.open_process(kPid);
    k.open_adapter_from_index(kPid, 0, &ad);
    k.create_device(kPid, ad, &dv);
    k.create_context(kPid, dv, ContextPriority::Normal, &ctx);
    AllocationDesc d;
    d.flags = kAllocRenderTarget, d.format = Format::A8R8G8B8, d.width = 8, d.height = 8;
    k.create_allocation(kPid, dv, d, &rt);
    k.create_sync(kPid, dv, SyncType::Fence, 0, 0, &fence);
    k.wait_sync_gpu(kPid, ctx, fence, 1);
    CommandWriter w(a);
    w.begin_pass(0, kNoAllocation, kPassClearColor, {1, 0, 0, 1}, 1, 0);
    w.end_pass();
    CHECK(k.render(kPid, ctx, w.view(), ArrayView<const Handle>(&rt, 1)) == status::kSuccess);
    CHECK(k.render(kPid, ctx, w.view(), ArrayView<const Handle>(&rt, 1)) == status::kSuccess);
    // (the process is never closed: the destructor does it)
  }
  CHECK(a.live == 0);
}

void test_escape() {
  Env e;
  ByteBuffer req(e.allocator);
  uint8_t reply_bytes[16];
  uint32_t reply_size = 0;
  auto call = [&](ByteBuffer& b) { return e.kernel.escape(kPid, e.dev, b.view(), ByteView(reply_bytes, sizeof(reply_bytes)), &reply_size); };
  auto handle_of_reply = [&]() { ByteReader in(ConstByteView(reply_bytes, reply_size)); return in.le32(); };

  req.put32(static_cast<uint32_t>(EscapeOp::CreateShader)), req.put32(static_cast<uint32_t>(ShaderKind::ColoredVertex)), req.put32(0), req.put32(0);
  CHECK(call(req) == status::kSuccess && reply_size == 4);
  const Handle vs = handle_of_reply();
  req.clear();
  req.put32(static_cast<uint32_t>(EscapeOp::CreateShader)), req.put32(static_cast<uint32_t>(ShaderKind::ColoredFragment)), req.put32(1), req.put32(0);
  CHECK(call(req) == status::kSuccess);
  const Handle fs = handle_of_reply();
  // The pipeline of the coloured shaders, spelled out as the user-mode driver would.
  auto pipeline_request = [&](ByteBuffer& b, Handle v, Handle f) {
    b.clear();
    b.put32(static_cast<uint32_t>(EscapeOp::CreatePipeline));
    for (uint32_t w : {v, f, 28u, 2u, 1u, 0u, 0u /*cull none*/, 0u, 0u}) b.put32(w);
    const uint32_t attributes[4][3] = {{0, 2, 0}, {1, 3, 12}, {0, 0, 0}, {0, 0, 0}};
    for (const auto& a : attributes) for (uint32_t w : a) b.put32(w);
    for (uint32_t w : {0u, 7u, 0u, 0u, 0u, 255u, 255u, 0u}) b.put32(w);
  };
  pipeline_request(req, vs, fs);
  CHECK(call(req) == status::kSuccess);
  const Handle pipe = handle_of_reply();
  Handle rt = e.alloc(kTarget, Format::A8R8G8B8, 8, 8), vb = e.buffer(kAllocVertexBuffer, 3 * 28);
  CHECK(write_triangle(e, vb, 1, 0, 1));
  const float identity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
  CommandWriter w(e.allocator);
  w.begin_pass(0, kNoAllocation, kPassClearColor, {0, 0, 0, 1}, 1, 0);
  w.bind_pipeline(pipe);
  w.bind_vertex_buffer(1);
  w.push_constants(floats(identity, 16));
  w.draw(3);
  w.end_pass();
  CHECK(e.render(w, {rt, vb}) == status::kSuccess && e.pixel(rt, 4, 4) == 0xFFFF00FF);

  // Truncated and bit-flipped requests never crash and never succeed with garbage that is out of range.
  pipeline_request(req, vs, fs);
  std::vector<uint8_t> good(req.view().data(), req.view().data() + req.view().size());
  for (size_t len = 0; len < good.size(); ++len) {
    CHECK(e.kernel.escape(kPid, e.dev, ConstByteView(good.data(), len), ByteView(reply_bytes, sizeof(reply_bytes)), &reply_size) != status::kSuccess);
  }
  for (size_t byte = 0; byte < good.size(); ++byte)
    for (int bit = 0; bit < 8; ++bit) {
      std::vector<uint8_t> t = good;
      t[byte] ^= static_cast<uint8_t>(1 << bit);
      NtStatus s = e.kernel.escape(kPid, e.dev, ConstByteView(t.data(), t.size()), ByteView(reply_bytes, sizeof(reply_bytes)), &reply_size);
      if (s == status::kSuccess) e.kernel.destroy_pipeline(kPid, ByteReader(ConstByteView(reply_bytes, 4)).le32());
    }
  // A too-small reply buffer, a missing shader, the wrong stage, an unknown op, a handle of another kind.
  CHECK(e.kernel.escape(kPid, e.dev, req.view(), ByteView(reply_bytes, 2), &reply_size) == status::kBufferTooSmall);
  pipeline_request(req, 0x1234, fs);
  CHECK(call(req) == status::kInvalidHandle);
  pipeline_request(req, fs, vs);
  CHECK(call(req) == status::kInvalidParameter);
  req.clear();
  req.put32(77);
  CHECK(call(req) == status::kNotSupported);
  req.clear();
  req.put32(static_cast<uint32_t>(EscapeOp::DestroyPipeline)), req.put32(vs);
  CHECK(call(req) == status::kInvalidHandle);
  req.clear();
  req.put32(static_cast<uint32_t>(EscapeOp::DestroyPipeline)), req.put32(pipe);
  CHECK(call(req) == status::kSuccess && call(req) == status::kInvalidHandle);
  // Shaders from code the back end does not take: the software device has neither TGSI nor Direct3D 9 shaders.
  req.clear();
  req.put32(static_cast<uint32_t>(EscapeOp::CreateShader)), req.put32(static_cast<uint32_t>(ShaderKind::Tgsi)), req.put32(0), req.put32(8);
  req.append(text_bytes("FRAG\nEND"));
  CHECK(call(req) == status::kNotSupported);
  req.clear();
  req.put32(static_cast<uint32_t>(EscapeOp::CreateShader)), req.put32(static_cast<uint32_t>(ShaderKind::D3d9Tokens)), req.put32(0), req.put32(6);
  req.put_zeros(8);
  CHECK(call(req) == status::kInvalidParameter);   // (6 bytes: not whole tokens)
  // A pipeline outlives the destruction of its shader handles.
  CHECK(e.kernel.destroy_shader(kPid, vs) == status::kSuccess && e.kernel.destroy_shader(kPid, vs) == status::kInvalidHandle);
}

void test_device_teardown() {
  Env e;
  Handle rt = e.alloc(kTarget, Format::A8R8G8B8, 8, 8), f = 0, second_ctx = 0;
  CHECK(e.kernel.create_sync(kPid, e.dev, SyncType::Fence, 0, 0, &f) == status::kSuccess && e.kernel.create_context(kPid, e.dev, ContextPriority::High, &second_ctx) == status::kSuccess);
  Handle pipe = make_colored_pipeline(e);
  LockResult r;
  CHECK(e.kernel.lock(kPid, rt, 0, 0, &r) == status::kSuccess);
  CHECK(e.kernel.destroy_device(kPid, e.dev) == status::kSuccess);
  uint64_t v;
  AllocationInfo info;
  CHECK(e.kernel.query_sync(kPid, f, &v) == status::kInvalidHandle && e.kernel.query_allocation(kPid, rt, &info) == status::kInvalidHandle);
  CHECK(e.kernel.destroy_context(kPid, second_ctx) == status::kInvalidHandle && e.kernel.destroy_pipeline(kPid, pipe) == status::kInvalidHandle);
  CHECK(e.kernel.adapter(0)->vidmm().system_bytes() == 0 && e.kernel.adapter(0)->vidmm().resident_count() == 0);
}


// --- Direct3D 9 bytecode ---------------------------------------------------------------------------------------------------------------------------------------------

constexpr uint32_t kRegTemp = 0, kRegInput = 1, kRegConst = 2, kRegTex = 3, kRegRastOut = 4, kRegAttrOut = 5, kRegTexOut = 6, kRegColorOut = 8, kRegSampler = 10, kRegBool = 14;
constexpr uint32_t kOpCnd = 80, kOpCmp = 88;
constexpr uint32_t kOpMov = 1, kOpAdd = 2, kOpMul = 5, kOpM4x4 = 20, kOpLoop = 27, kOpDcl = 31, kOpIf = 40, kOpElse = 42, kOpEndIf = 43, kOpDefB = 47, kOpTexLd = 66, kOpDef = 81;
constexpr uint32_t kXyzw = 0xE4;
constexpr uint32_t kOpTemp = 0, kOpInput = 1, kOpOut = 2, kOpConstantBuffer = 8;

// A tiny assembler for the token streams.
struct Asm {
  explicit Asm(bool vertex, uint32_t major, uint32_t minor) { t.push_back((vertex ? 0xFFFE0000u : 0xFFFF0000u) | (major << 8) | minor); }
  static uint32_t type_bits(uint32_t type) { return ((type & 7) << 28) | ((type & 0x18) << 8); }
  static uint32_t dst(uint32_t type, uint32_t num, uint32_t mask = 0xF, uint32_t mod = 0) { return 0x80000000u | type_bits(type) | num | (mask << 16) | (mod << 20); }
  static uint32_t src(uint32_t type, uint32_t num, uint32_t swz = kXyzw, uint32_t mod = 0) { return 0x80000000u | type_bits(type) | num | (swz << 16) | (mod << 24); }
  Asm& op(uint32_t opcode, std::initializer_list<uint32_t> params, uint32_t control = 0) {
    t.push_back(opcode | (control << 16) | (static_cast<uint32_t>(params.size()) << 24));
    for (uint32_t p : params) t.push_back(p);
    return *this;
  }
  Asm& op_list(uint32_t opcode, const std::vector<uint32_t>& params) {
    t.push_back(opcode | (static_cast<uint32_t>(params.size()) << 24));
    for (uint32_t p : params) t.push_back(p);
    return *this;
  }
  Asm& dcl(uint32_t usage, uint32_t index, uint32_t type, uint32_t num) { return op(kOpDcl, {0x80000000u | usage | (index << 16), dst(type, num)}); }
  Asm& dcl_2d(uint32_t num) { return op(kOpDcl, {0x80000000u | (2u << 27), dst(kRegSampler, num)}); }
  Asm& def(uint32_t num, float a, float b, float c, float d) {
    t.push_back(kOpDef | (5u << 24));
    t.push_back(dst(kRegConst, num));
    for (float f : {a, b, c, d}) t.push_back(__builtin_bit_cast(uint32_t, f));
    return *this;
  }
  Asm& defb(uint32_t num, bool v) { return op(kOpDefB, {dst(kRegBool, num), v ? 1u : 0u}); }
  Asm& end() { t.push_back(0xFFFF); return *this; }
  ConstByteView bytes() const { return ConstByteView(reinterpret_cast<const uint8_t*>(t.data()), t.size() * 4); }
  std::vector<uint32_t> t;
};

Handle d3d9_shader(Env& e, const Asm& a, NtStatus* status_out = nullptr) {
  Handle h = 0;
  ShaderDesc d;
  d.kind = ShaderKind::D3d9Bytecode;
  d.code = a.bytes();
  const NtStatus s = e.kernel.create_shader(kPid, e.dev, d, &h);
  if (status_out) *status_out = s;
  else CHECK(s == status::kSuccess);
  return h;
}

Handle d3d9_pipeline(Env& e, Handle vs, Handle fs, uint32_t stride, uint32_t attributes, uint32_t textures) {
  PipelineDesc pd;
  pd.vertex_shader = vs, pd.fragment_shader = fs, pd.vertex_stride = stride, pd.attribute_count = attributes, pd.varying_count = 1, pd.texture_count = textures;
  pd.attributes[0] = {0, graphics::VertexFormat::Float3, 0};
  pd.attributes[1] = textures ? graphics::VertexAttribute{1, graphics::VertexFormat::Float2, 12} : graphics::VertexAttribute{1, graphics::VertexFormat::Float4, 12};
  pd.cull_mode = graphics::CullMode::None;
  Handle h = 0;
  CHECK(e.kernel.create_pipeline(kPid, e.dev, pd, &h) == status::kSuccess);
  return h;
}

Asm color_vs() {
  Asm a(true, 2, 0);
  a.dcl(0, 0, kRegInput, 0).dcl(10, 0, kRegInput, 1);
  a.op(kOpMov, {Asm::dst(kRegRastOut, 0), Asm::src(kRegInput, 0)});
  a.op(kOpMov, {Asm::dst(kRegAttrOut, 0), Asm::src(kRegInput, 1)});
  return a.end();
}
Asm color_ps() {
  Asm a(false, 2, 0);
  a.dcl(10, 0, kRegInput, 0);
  a.op(kOpMov, {Asm::dst(kRegColorOut, 0), Asm::src(kRegInput, 0)});
  return a.end();
}

void test_d3d9_bytecode() {
  Env e;
  Handle rt = e.alloc(kTarget, Format::A8R8G8B8, 16, 16), vb = e.buffer(kAllocVertexBuffer, 3 * 28);
  const float identity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
  auto draw = [&](Handle pipe, bool constants_matrix, const float* matrix) {
    CommandWriter w(e.allocator);
    w.begin_pass(0, kNoAllocation, kPassClearColor, {0, 0, 0, 1}, 1, 0);
    w.bind_pipeline(pipe);
    w.bind_vertex_buffer(1);
    if (constants_matrix) w.push_constants(floats(matrix, 16));
    w.draw(3);
    w.end_pass();
    return e.render(w, {rt, vb});
  };

  // Pass-through shaders: the triangle in the lower half of Direct3D's clip space (+Y up) ends up in the lower half of the picture.
  {
    const float v[21] = {-1, -1, 0.5f, 1, 0, 0, 1, 1, -1, 0.5f, 1, 0, 0, 1, 0, 0, 0.5f, 1, 0, 0, 1};
    CHECK(e.write(vb, floats(v, 21)));
    Handle pipe = d3d9_pipeline(e, d3d9_shader(e, color_vs()), d3d9_shader(e, color_ps()), 28, 2, 0);
    CHECK(draw(pipe, false, nullptr) == status::kSuccess);
    CHECK(e.pixel(rt, 8, 13) == 0xFFFF0000 && e.pixel(rt, 8, 2) == 0xFF000000 && e.pixel(rt, 1, 14) == 0xFFFF0000);
  }

  // A matrix from the push constants (m4x4), a constant the shader defines, and write masks / modifiers: output = def'd colour with its green doubled and red negated-abs'd.
  {
    Asm vs(true, 2, 0);
    vs.dcl(0, 0, kRegInput, 0);
    vs.def(4, 0.25f, 0.5f, 0.0f, 1.0f);
    vs.op(kOpM4x4, {Asm::dst(kRegRastOut, 0), Asm::src(kRegInput, 0), Asm::src(kRegConst, 0)});
    vs.op(kOpMov, {Asm::dst(kRegAttrOut, 0), Asm::src(kRegConst, 4)});
    vs.op(kOpAdd, {Asm::dst(kRegAttrOut, 0, 0x2 /* y */), Asm::src(kRegConst, 4, kXyzw), Asm::src(kRegConst, 4, kXyzw)});
    vs.op(kOpMov, {Asm::dst(kRegAttrOut, 0, 0x1 /* x */), Asm::src(kRegConst, 4, kXyzw, 11 /* abs */)});
    vs.end();
    const float v[21] = {-1, -1, 0.5f, 0, 0, 0, 0, 3, -1, 0.5f, 0, 0, 0, 0, -1, 3, 0.5f, 0, 0, 0, 0};
    CHECK(e.write(vb, floats(v, 21)));
    const float half[16] = {0.5f, 0, 0, 0, 0, 0.5f, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    Handle pipe = d3d9_pipeline(e, d3d9_shader(e, vs), d3d9_shader(e, color_ps()), 28, 1, 0);
    CHECK(draw(pipe, true, half) == status::kSuccess);
    const uint32_t centre = e.pixel(rt, 8, 8);
    CHECK(centre == 0xFF40FF00);   // r = 0.25, g = 0.5 + 0.5, b = 0, a = 1
    CHECK(e.pixel(rt, 15, 0) == 0xFF000000 && e.pixel(rt, 1, 14) == 0xFF000000);   // (scaled by a half: the corners are outside)
    // The same shader with the full-size matrix covers the corners too.
    CHECK(draw(pipe, true, identity) == status::kSuccess && e.pixel(rt, 15, 0) == 0xFF40FF00);
  }

  // Texture lookup: texld, and texcoords through oT0 / t0.
  {
    Asm vs(true, 2, 0);
    vs.dcl(0, 0, kRegInput, 0).dcl(5, 0, kRegInput, 1);
    vs.op(kOpMov, {Asm::dst(kRegRastOut, 0), Asm::src(kRegInput, 0)});
    vs.op(kOpMov, {Asm::dst(kRegTexOut, 0), Asm::src(kRegInput, 1)});
    vs.end();
    Asm ps(false, 2, 0);
    ps.dcl(5, 0, kRegTex, 0).dcl_2d(0);
    ps.op(kOpTexLd, {Asm::dst(kRegTemp, 0), Asm::src(kRegTex, 0), Asm::src(kRegSampler, 0)});
    ps.op(kOpMul, {Asm::dst(kRegColorOut, 0), Asm::src(kRegTemp, 0), Asm::src(kRegTemp, 0, 0xE4, 7 /* x2 */)});
    ps.end();
    Handle tex = e.alloc(kTexture, Format::A8R8G8B8, 4, 4);
    CHECK(e.fill(tex, 0xFF204060));
    Handle tvb = e.buffer(kAllocVertexBuffer, 3 * 20);
    const float quad[15] = {-1, -1, 0.5f, 0, 0, 3, -1, 0.5f, 2, 0, -1, 3, 0.5f, 0, 2};
    CHECK(e.write(tvb, floats(quad, 15)));
    Handle pipe = d3d9_pipeline(e, d3d9_shader(e, vs), d3d9_shader(e, ps), 20, 2, 1);
    CommandWriter w(e.allocator);
    w.begin_pass(0, kNoAllocation, kPassClearColor, {0, 0, 0, 1}, 1, 0);
    w.bind_pipeline(pipe);
    w.bind_vertex_buffer(2);
    w.bind_texture(0, 1, graphics::SamplerDesc());
    w.draw(3);
    w.end_pass();
    CHECK(e.render(w, {rt, tex, tvb}) == status::kSuccess);
    // texel * (2 * texel) per channel: 0x20/255 = 0.1255 -> 0.0315 -> 8; 0x40 -> 0.251 -> 0.126 -> 32 (0x20); 0x60 -> 0.376 -> 0.283 -> 72 (0x48)
    const uint32_t got = e.pixel(rt, 8, 8);
    CHECK(got == 0xFF082048 || got == 0xFF082047 || got == 0xFF092048);
  }

  // Static branches.
  for (int pass = 0; pass < 2; ++pass) {
    Asm ps(false, 2, 0);
    ps.def(5, 1, 0, 0, 1).def(6, 0, 0, 1, 1).defb(0, pass == 0);
    ps.op(kOpIf, {Asm::src(kRegBool, 0)});
    ps.op(kOpMov, {Asm::dst(kRegColorOut, 0), Asm::src(kRegConst, 5)});
    ps.op(kOpElse, {});
    ps.op(kOpMov, {Asm::dst(kRegColorOut, 0), Asm::src(kRegConst, 6)});
    ps.op(kOpEndIf, {});
    ps.end();
    const float v[21] = {-1, -1, 0.5f, 0, 0, 0, 0, 3, -1, 0.5f, 0, 0, 0, 0, -1, 3, 0.5f, 0, 0, 0, 0};
    CHECK(e.write(vb, floats(v, 21)));
    Handle pipe = d3d9_pipeline(e, d3d9_shader(e, color_vs()), d3d9_shader(e, ps), 28, 2, 0);
    CHECK(draw(pipe, false, nullptr) == status::kSuccess);
    CHECK(e.pixel(rt, 8, 8) == (pass == 0 ? 0xFFFF0000u : 0xFF0000FFu));
  }

  // What is refused, and how.
  NtStatus s = 0;
  { Asm a(false, 1, 1); a.op(71, {Asm::dst(kRegTex, 0), Asm::src(kRegTex, 1)}); a.end(); d3d9_shader(e, a, &s); CHECK(s == status::kNotSupported); }   // ps_1_1: texm3x2pad
  { Asm a(true, 2, 0); a.op(kOpLoop, {Asm::src(kRegConst, 0), Asm::src(kRegConst, 0)}); a.end(); d3d9_shader(e, a, &s); CHECK(s == status::kNotSupported); }
  { Asm a = color_vs(); a.t.back() = 0; d3d9_shader(e, a, &s); CHECK(s == status::kInvalidParameter); }                              // no end token
  { Asm a(true, 2, 0); a.op(kOpMov, {Asm::dst(kRegAttrOut, 0), Asm::src(kRegConst, 0)}); a.end(); d3d9_shader(e, a, &s); CHECK(s == status::kInvalidParameter); }   // never writes the position
  { Asm a(true, 2, 0); a.op(kOpMov, {Asm::dst(kRegRastOut, 0), Asm::src(kRegConst, 20)}); a.end(); d3d9_shader(e, a, &s); CHECK(s == status::kNotSupported); }   // c20: no such constant
  { Asm a(true, 2, 0); a.op(kOpMov, {Asm::dst(kRegRastOut, 0), Asm::src(kRegTemp, 40)}); a.end(); d3d9_shader(e, a, &s); CHECK(s == status::kInvalidParameter); }
  { Asm a(false, 2, 0); a.defb(0, true).op(kOpIf, {Asm::src(kRegBool, 0)}); a.end(); d3d9_shader(e, a, &s); CHECK(s == status::kInvalidParameter); }          // if without endif
  { Asm a(false, 2, 0); a.op(kOpMov, {Asm::dst(kRegColorOut, 1), Asm::src(kRegConst, 0)}); a.end(); d3d9_shader(e, a, &s); CHECK(s == status::kNotSupported); }  // oC1
  { Asm a(false, 4, 0); a.end(); d3d9_shader(e, a, &s); CHECK(s == status::kNotSupported); }
  { const uint8_t odd[7] = {1, 2, 3, 4, 5, 6, 7}; ShaderDesc d; d.kind = ShaderKind::D3d9Bytecode; d.code = ConstByteView(odd, 7); Handle h; CHECK(e.kernel.create_shader(kPid, e.dev, d, &h) == status::kInvalidParameter); }

  // Every prefix and every bit flip of real shaders: a status, never a crash; and what compiles can be run.
  Asm samples[3] = {color_vs(), color_ps(), Asm(false, 2, 0)};
  samples[2].dcl(5, 0, kRegTex, 0).dcl_2d(0).def(1, 1, 2, 3, 4);
  samples[2].op(kOpTexLd, {Asm::dst(kRegTemp, 0), Asm::src(kRegTex, 0), Asm::src(kRegSampler, 0)}).op(kOpMov, {Asm::dst(kRegColorOut, 0), Asm::src(kRegTemp, 0)}).end();
  uint32_t compiled = 0, refused = 0;
  for (const Asm& sample : samples) {
    const std::vector<uint8_t> bytes(sample.bytes().data(), sample.bytes().data() + sample.bytes().size());
    for (size_t len = 0; len < bytes.size(); len += 1) {
      UniquePointer<D3d9Program> p;
      D3d9Program::compile(e.allocator, ConstByteView(bytes.data(), len), &p) == status::kSuccess ? ++compiled : ++refused;
    }
    for (size_t byte = 0; byte < bytes.size(); ++byte)
      for (int bit = 0; bit < 8; ++bit) {
        std::vector<uint8_t> t = bytes;
        t[byte] ^= static_cast<uint8_t>(1 << bit);
        UniquePointer<D3d9Program> p;
        if (D3d9Program::compile(e.allocator, ConstByteView(t.data(), t.size()), &p) == status::kSuccess) {
          ++compiled;
          graphics::Vec4 attrs[4] = {}, vary[4] = {};
          graphics::Vec4 color_room[graphics::kMaxColorTargets] = {};
          graphics::Vec4& color = color_room[0];
          alignas(16) uint8_t constants[128] = {};
          graphics::ShaderIo io;
          if (p->stage() == graphics::ShaderStage::Vertex) p->run_vertex(attrs, constants, &io);
          else p->run_fragment(vary, constants, nullptr, &color);
        } else {
          ++refused;
        }
      }
  }
  CHECK(compiled > 0 && refused > 0);
  printf("  d3d9 shader fuzz: %u compiled, %u refused\n", compiled, refused);

  // The binding slots are a limited resource that is given back.
  std::vector<Handle> shaders;
  NtStatus last = 0;
  for (int i = 0; i < 40; ++i) {
    Handle h = d3d9_shader(e, color_ps(), &last);
    if (last != status::kSuccess) break;
    shaders.push_back(h);
  }
  CHECK(last == status::kInsufficientResources && shaders.size() >= 20 && shaders.size() < 40);
  CHECK(e.kernel.destroy_shader(kPid, shaders.back()) == status::kSuccess);
  d3d9_shader(e, color_ps(), &last);
  CHECK(last == status::kSuccess);
}


// --- Direct3D 9 bytecode to TGSI ---------------------------------------------------------------------------------------------------------------------------------------
//
// The emitted text is run by a small TGSI machine (the instructions the emitter uses, with TGSI's meaning) and compared with the interpreter on the same inputs.

struct TexSet final : graphics::ITextureSet {
  graphics::Vec4 sample(uint32_t unit, float u, float v) const override {
    return {0.25f + 0.5f * sinf(u * 3.0f + static_cast<float>(unit)), 0.5f + 0.4f * cosf(v * 2.0f), 0.1f + u * v * 0.3f, 0.9f};
  }
};

// (the instructions that read their registers as integers: a source's - and || are integer ones for them)
bool tgsi_integer_op(const std::string& op) {
  static const char* const kOps[] = {"UCMP", "USEQ", "USNE", "ISLT", "ISGE", "USLT", "USGE", "UADD", "INEG", "UMUL", "UMAD", "IMIN", "IMAX", "UMIN", "UMAX", "UDIV", "UMOD", "AND", "OR", "XOR", "NOT",
                                     "I2F", "U2F", "SHL", "ISHR", "USHR"};
  for (const char* o : kOps)
    if (op == o) return true;
  return false;
}
bool tgsi_if(const std::string& op) { return op == "IF" || op == "UIF"; }

class TgsiMachine {
 public:
  float temp[64][4] = {}, in[32][4] = {}, out[16][4] = {}, cst[4100][4] = {}, imm[64][4] = {}, sv[4][4] = {};   // (sv: VERTEXID, INSTANCEID)
  int addr = 0;
  bool killed = false;
  const graphics::ITextureSet* textures = nullptr;

  // Runs `text`; false when it does not parse or uses something the machine has not got.
  bool run(const std::string& text) {
    std::vector<std::string> body;
    size_t at = 0;
    while (at < text.size()) {
      size_t end = text.find('\n', at);
      if (end == std::string::npos) end = text.size();
      std::string l = text.substr(at, end - at);
      at = end + 1;
      size_t b = l.find_first_not_of(' ');
      if (b == std::string::npos) continue;
      l = l.substr(b);
      if (l.rfind("IMM[", 0) == 0) {
        const unsigned i = static_cast<unsigned>(atoi(l.c_str() + 4));
        const char* p = strchr(l.c_str(), '{');
        if (!p || i >= 64) return false;
        ++p;
        const bool bits = l.find("UINT32") != std::string::npos;   // (an integer literal, in decimal: the register holds its bits)
        for (int k = 0; k < 4; ++k) {
          char* e = nullptr;
          if (bits) imm[i][k] = __builtin_bit_cast(float, static_cast<uint32_t>(strtoul(p, &e, 10)));
          else imm[i][k] = strtof(p, &e);
          if (e == p) return false;
          p = e;
          while (*p == ',' || *p == ' ') ++p;
        }
      } else if (isdigit(static_cast<unsigned char>(l[0]))) {
        body.push_back(l.substr(l.find(':') + 2));
      }
    }
    // Structure.
    std::vector<size_t> else_of(body.size(), 0), end_of(body.size(), 0), stack;
    for (size_t i = 0; i < body.size(); ++i) {
      const std::string op = body[i].substr(0, body[i].find(' '));
      if (tgsi_if(op)) stack.push_back(i);
      else if (op == "ELSE") {
        if (stack.empty()) return false;
        else_of[stack.back()] = i;
        stack.back() = i;   // (the ELSE now stands for the open construct)
        end_of[i] = 0;
      } else if (op == "ENDIF") {
        if (stack.empty()) return false;
        end_of[stack.back()] = i;
        stack.pop_back();
      }
    }
    if (!stack.empty()) return false;
    std::vector<size_t> loops;
    uint32_t steps = 0;
    for (size_t pc = 0; pc < body.size();) {
      const std::string& l = body[pc];
      const size_t sp = l.find(' ');
      const std::string op = l.substr(0, sp);
      std::vector<std::string> args;
      if (sp != std::string::npos) {
        std::string rest = l.substr(sp + 1);
        size_t s = 0;
        while (s <= rest.size()) {
          size_t c = rest.find(',', s);
          if (c == std::string::npos) c = rest.size();
          std::string a = rest.substr(s, c - s);
          const size_t f = a.find_first_not_of(' ');
          args.push_back(f == std::string::npos ? "" : a.substr(f));
          s = c + 1;
        }
      }
      ++pc;
      if (op == "END") break;
      if (op == "ELSE") {   // the true branch ended: to the ENDIF
        size_t e = pc;
        int depth = 0;
        for (; e < body.size(); ++e) {
          const std::string o = body[e].substr(0, body[e].find(' '));
          if (tgsi_if(o)) ++depth;
          if (o == "ENDIF") {
            if (!depth) break;
            --depth;
          }
        }
        pc = e + 1;
        continue;
      }
      if (op == "ENDIF") continue;
      if (++steps > 100000) return false;
      if (op == "BGNLOOP") {
        loops.push_back(pc);
        continue;
      }
      if (op == "ENDLOOP") {
        if (loops.empty()) return false;
        pc = loops.back();
        continue;
      }
      if (op == "BRK" || op == "CONT") {
        size_t e = pc;
        int depth = 0;
        for (; e < body.size(); ++e) {
          const std::string o = body[e].substr(0, body[e].find(' '));
          if (o == "BGNLOOP") ++depth;
          if (o == "ENDLOOP") {
            if (!depth) break;
            --depth;
          }
        }
        if (e >= body.size() || loops.empty()) return false;
        if (op == "BRK") {
          loops.pop_back();
          pc = e + 1;
        } else {
          pc = e;   // (the ENDLOOP takes it back to the top)
        }
        continue;
      }
      if (tgsi_if(op)) {
        float c[4];
        if (!fetch(args.at(0), c, op == "UIF")) return false;
        if (op == "UIF" ? __builtin_bit_cast(uint32_t, c[0]) == 0 : c[0] == 0) {   // to after the ELSE or the ENDIF
          size_t e = pc;
          int depth = 0;
          for (; e < body.size(); ++e) {
            const std::string o = body[e].substr(0, body[e].find(' '));
            if (tgsi_if(o)) ++depth;
            else if ((o == "ELSE" || o == "ENDIF") && !depth) break;
            else if (o == "ENDIF") --depth;
          }
          pc = e + 1;
        }
        continue;
      }
      if (!execute(op, args)) { if (getenv("TGSI_TRACE")) printf("  TGSI machine cannot run: %s\n", l.c_str()); return false; }
    }
    return true;
  }

 private:
  struct Ref {
    bool neg = false, abs = false;
    std::string file;
    int index = 0;
    bool relative = false;
    std::string swz;
  };
  static bool parse(const std::string& in_text, Ref* r) {
    std::string s = in_text;
    if (s.empty()) return false;
    if (s[0] == '-') r->neg = true, s = s.substr(1);
    if (s[0] == '|') {
      r->abs = true;
      s = s.substr(1);
      if (s.empty() || s.back() != '|') return false;
      s.pop_back();
    }
    const size_t lb = s.find('[');
    if (lb == std::string::npos) return false;
    r->file = s.substr(0, lb);
    int depth = 0;
    size_t rb = lb;
    for (size_t i = lb; i < s.size(); ++i) {
      if (s[i] == '[') ++depth;
      if (s[i] == ']' && --depth == 0) {
        rb = i;
        break;
      }
    }
    if (rb == lb) return false;
    const std::string inner = s.substr(lb + 1, rb - lb - 1);
    if (inner.rfind("ADDR[0].x+", 0) == 0) r->relative = true, r->index = atoi(inner.c_str() + 10);
    else r->index = atoi(inner.c_str());
    if (rb + 1 < s.size()) {
      if (s[rb + 1] != '.') return false;
      r->swz = s.substr(rb + 2);
    }
    return true;
  }
  float* reg(const Ref& r) {
    int index = r.index + (r.relative ? addr : 0);
    static float zero[4];
    zero[0] = zero[1] = zero[2] = zero[3] = 0;
    if (r.file == "TEMP") return index >= 0 && index < 64 ? temp[index] : nullptr;
    if (r.file == "IN") return index >= 0 && index < 32 ? in[index] : nullptr;
    if (r.file == "SV") return index >= 0 && index < 4 ? sv[index] : nullptr;
    if (r.file == "OUT") return index >= 0 && index < 16 ? out[index] : nullptr;
    if (r.file == "IMM") return index >= 0 && index < 64 ? imm[index] : nullptr;
    if (r.file == "CONST") return index >= 0 && index < 4100 ? cst[index] : zero;
    return nullptr;
  }
  bool fetch(const std::string& text, float v[4], bool integer = false) {
    Ref r;
    if (!parse(text, &r)) return false;
    const float* p = reg(r);
    if (!p) return false;
    for (int i = 0; i < 4; ++i) {
      int ch = i;
      if (!r.swz.empty()) {
        if (r.swz.size() != 4) return false;
        ch = static_cast<int>(strchr("xyzw", r.swz[i]) - "xyzw");
      }
      v[i] = p[ch];
      if (integer) {
        int32_t x = __builtin_bit_cast(int32_t, v[i]);
        if (r.abs && x < 0) x = static_cast<int32_t>(0u - static_cast<uint32_t>(x));
        if (r.neg) x = static_cast<int32_t>(0u - static_cast<uint32_t>(x));
        v[i] = __builtin_bit_cast(float, x);
        continue;
      }
      if (r.abs) v[i] = fabsf(v[i]);
      if (r.neg) v[i] = -v[i];
    }
    return true;
  }
  bool store(const std::string& text, const float v[4]) {
    Ref r;
    if (!parse(text, &r)) return false;
    float* p = reg(r);
    if (!p) return false;
    const std::string mask = r.swz.empty() ? "xyzw" : r.swz;
    for (char c : mask) {
      const char* k = strchr("xyzw", c);
      if (!k) return false;
      p[k - "xyzw"] = v[k - "xyzw"];
    }
    return true;
  }
  bool execute(const std::string& op, const std::vector<std::string>& args) {
    float a[4] = {}, b[4] = {}, c[4] = {}, r[4] = {};
    auto need = [&](size_t n) { return args.size() >= n; };
    if (op == "ARL" || op == "ARR" || op == "UARL") {
      if (!need(2) || !fetch(args[1], a)) return false;
      addr = op == "UARL" ? static_cast<int>(__builtin_bit_cast(uint32_t, a[0])) : static_cast<int>(floorf(op == "ARR" ? a[0] + 0.5f : a[0]));
      return true;
    }
    if (op == "KILL") {
      killed = true;
      return true;
    }
    if (op == "KILL_IF") {
      if (!need(1) || !fetch(args[0], a)) return false;
      for (float x : a) killed = killed || x < 0;
      return true;
    }
    const bool integer = tgsi_integer_op(op), is_tex = op == "TEX" || op == "TXP" || op == "TXF" || op == "TXL" || op == "TXQ";
    const bool data_integer = integer && op != "UCMP";   // (UCMP's condition is an integer, what it selects is data)
    if (!need(2) || !fetch(args[1], a, integer)) return false;
    if (args.size() > 2 && !is_tex && !fetch(args[2], b, data_integer)) return false;
    if (args.size() > 3 && !is_tex && !fetch(args[3], c, data_integer)) return false;
    const bool scalar = op == "RCP" || op == "RSQ" || op == "EX2" || op == "LG2" || op == "COS" || op == "SIN";
    for (int i = 0; i < 4; ++i) {
      if (op == "MOV") r[i] = a[i];
      else if (op == "ADD") r[i] = a[i] + b[i];
      else if (op == "SUB") r[i] = a[i] - b[i];
      else if (op == "MUL") r[i] = a[i] * b[i];
      else if (op == "MAD") r[i] = a[i] * b[i] + c[i];
      else if (op == "MIN") r[i] = a[i] < b[i] ? a[i] : b[i];
      else if (op == "MAX") r[i] = a[i] > b[i] ? a[i] : b[i];
      else if (op == "SLT") r[i] = a[i] < b[i] ? 1.0f : 0.0f;
      else if (op == "SGE") r[i] = a[i] >= b[i] ? 1.0f : 0.0f;
      else if (op == "SEQ") r[i] = a[i] == b[i] ? 1.0f : 0.0f;
      else if (op == "FSEQ") r[i] = __builtin_bit_cast(float, a[i] == b[i] ? 0xFFFFFFFFu : 0u);
      else if (op == "FSNE") r[i] = __builtin_bit_cast(float, a[i] != b[i] ? 0xFFFFFFFFu : 0u);
      else if (op == "FSLT") r[i] = __builtin_bit_cast(float, a[i] < b[i] ? 0xFFFFFFFFu : 0u);
      else if (op == "FSGE") r[i] = __builtin_bit_cast(float, a[i] >= b[i] ? 0xFFFFFFFFu : 0u);
      else if (op == "SNE") r[i] = a[i] != b[i] ? 1.0f : 0.0f;
      else if (op == "CMP") r[i] = a[i] < 0 ? b[i] : c[i];
      else if (op == "FRC") r[i] = a[i] - floorf(a[i]);
      else if (op == "ABS") r[i] = fabsf(a[i]);
      else if (op == "SSG") r[i] = a[i] > 0 ? 1.0f : a[i] < 0 ? -1.0f : 0.0f;
      else if (op == "DP2") r[i] = a[0] * b[0] + a[1] * b[1];
      else if (op == "DP3") r[i] = a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
      else if (op == "DP4") r[i] = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
      else if (op == "RCP") r[i] = 1.0f / a[0];
      else if (op == "RSQ") r[i] = 1.0f / sqrtf(fabsf(a[0]));
      else if (op == "EX2") r[i] = exp2f(a[0]);
      else if (op == "LG2") r[i] = log2f(a[0]);
      else if (op == "COS") r[i] = cosf(a[0]);
      else if (op == "SIN") r[i] = sinf(a[0]);
      else if (op == "POW") r[i] = powf(a[0], b[0]);
      else if (op == "SQRT") r[i] = sqrtf(a[0]);
      else if (op == "CEIL") r[i] = ceilf(a[i]);
      else if (op == "FLR") r[i] = floorf(a[i]);
      else if (op == "TRUNC") r[i] = truncf(a[i]);
      else if (op == "ROUND") r[i] = nearbyintf(a[i]);
      else if (integer) {
        const uint32_t ua = __builtin_bit_cast(uint32_t, a[i]), ub = __builtin_bit_cast(uint32_t, b[i]), uc = __builtin_bit_cast(uint32_t, c[i]);
        const int32_t ia = static_cast<int32_t>(ua), ib = static_cast<int32_t>(ub);
        uint32_t u = 0;
        bool as_float = false;
        float f = 0;
        if (op == "UCMP") u = ua != 0 ? ub : uc;
        else if (op == "USEQ") u = ua == ub ? ~0u : 0u;
        else if (op == "USNE") u = ua != ub ? ~0u : 0u;
        else if (op == "ISLT") u = ia < ib ? ~0u : 0u;
        else if (op == "ISGE") u = ia >= ib ? ~0u : 0u;
        else if (op == "USLT") u = ua < ub ? ~0u : 0u;
        else if (op == "USGE") u = ua >= ub ? ~0u : 0u;
        else if (op == "UADD") u = ua + ub;
        else if (op == "INEG") u = 0u - ua;
        else if (op == "UMUL") u = ua * ub;
        else if (op == "UMAD") u = ua * ub + uc;
        else if (op == "IMIN") u = static_cast<uint32_t>(ia < ib ? ia : ib);
        else if (op == "IMAX") u = static_cast<uint32_t>(ia > ib ? ia : ib);
        else if (op == "UMIN") u = ua < ub ? ua : ub;
        else if (op == "UMAX") u = ua > ub ? ua : ub;
        else if (op == "UDIV") u = ub ? ua / ub : ~0u;
        else if (op == "UMOD") u = ub ? ua % ub : ~0u;
        else if (op == "AND") u = ua & ub;
        else if (op == "OR") u = ua | ub;
        else if (op == "XOR") u = ua ^ ub;
        else if (op == "NOT") u = ~ua;
        else if (op == "SHL") u = ua << (ub & 31);
        else if (op == "ISHR") u = static_cast<uint32_t>(ia >> (ub & 31));
        else if (op == "USHR") u = ua >> (ub & 31);
        else if (op == "I2F") as_float = true, f = static_cast<float>(ia);
        else if (op == "U2F") as_float = true, f = static_cast<float>(ua);
        else return false;
        r[i] = as_float ? f : __builtin_bit_cast(float, u);
      } else if (op == "F2I") {
        r[i] = __builtin_bit_cast(float, static_cast<uint32_t>(static_cast<int32_t>(a[i])));
      } else if (op == "F2U") {
        r[i] = __builtin_bit_cast(float, a[i] <= 0 ? 0u : static_cast<uint32_t>(a[i]));
      } else if (!is_tex) return false;
    }
    (void)scalar;
    if (op == "TXF" || op == "TXL" || op == "TXQ") {
      if (!need(4) || !textures) return false;
      const unsigned unit = static_cast<unsigned>(atoi(args[2].c_str() + 5));
      graphics::Vec4 t = {0, 0, 0, 0};
      if (op == "TXF") t = textures->fetch(unit, __builtin_bit_cast(int32_t, a[0]), __builtin_bit_cast(int32_t, a[1]), __builtin_bit_cast(uint32_t, a[3]));
      else if (op == "TXL") t = textures->sample_lod(unit, a[0], a[1], a[3]);
      else {
        uint32_t w = 0, h = 0, levels = 0;
        if (!textures->size(unit, __builtin_bit_cast(uint32_t, a[0]), &w, &h, &levels)) return false;
        t = {__builtin_bit_cast(float, w), __builtin_bit_cast(float, h), 0.0f, __builtin_bit_cast(float, levels)};
      }
      r[0] = t.x, r[1] = t.y, r[2] = t.z, r[3] = t.w;
    }
    if (op == "TEX" || op == "TXP") {
      if (!need(4) || !textures) return false;
      const unsigned unit = static_cast<unsigned>(atoi(args[2].c_str() + 5));
      float u = a[0], v = a[1];
      if (op == "TXP") {
        const float inv = a[3] == 0 ? 0.0f : 1.0f / a[3];
        u *= inv, v *= inv;
      }
      const graphics::Vec4 t = textures->sample(unit, u, v);
      r[0] = t.x, r[1] = t.y, r[2] = t.z, r[3] = t.w;
    }
    return store(args[0], r);
  }
};

bool near_value(float a, float b) {
  if (a != a || b != b) return (a != a) == (b != b);
  if (isinf(a) || isinf(b)) return a == b;
  return fabsf(a - b) <= 2e-3f + 2e-3f * fabsf(b);
}

constexpr float kInputs[4][4][4] = {
    {{0.37f, -0.61f, 0.93f, 0.27f}, {0.81f, 0.43f, -0.29f, 1.57f}, {0.23f, 0.71f, 0.13f, 2.1f}, {-0.77f, 0.19f, 0.58f, -0.33f}},
    {{-0.42f, 0.88f, 0.31f, -1.4f}, {1.7f, -0.35f, 0.64f, 0.22f}, {0.66f, 0.17f, -0.41f, 1.3f}, {0.91f, 0.07f, -0.83f, 0.45f}},
    {{1.9f, 0.14f, -0.72f, 0.61f}, {1.7f, 2.3f, 0.09f, -0.54f}, {-0.28f, 0.95f, 0.47f, 0.73f}, {0.12f, -1.6f, 0.38f, 0.86f}},
    {{0.05f, 0.33f, 0.77f, -0.91f}, {1.7f, -1.1f, 1.8f, 0.36f}, {0.52f, -0.63f, 0.24f, 1.2f}, {-0.15f, 0.69f, -0.47f, 0.29f}},
};
constexpr float kConstants[4][8][4] = {
    {{0.43f, -0.67f, 0.91f, 1.3f}, {-0.59f, 0.24f, 1.7f, -1.2f}, {0.31f, 0.83f, -0.45f, 0.62f}, {1.1f, -0.38f, 0.74f, 0.17f}, {0.2f, 0.4f, 0.6f, 0.8f}, {-0.3f, 0.5f, -0.7f, 0.9f}, {0.11f, 0.22f, 0.33f, 0.44f}, {0.55f, 0.66f, 0.77f, 0.88f}},
    {{-1.3f, 0.47f, 0.29f, 0.92f}, {0.68f, -0.83f, 0.35f, 1.6f}, {0.54f, 0.12f, -0.96f, 0.27f}, {-0.22f, 0.79f, 0.41f, -0.64f}, {0.9f, 0.1f, 0.3f, 0.7f}, {-0.1f, -0.5f, 0.2f, 0.4f}, {0.15f, 0.25f, 0.35f, 0.45f}, {0.85f, 0.75f, 0.65f, 0.55f}},
    {{0.77f, 0.31f, -0.52f, 0.18f}, {-0.94f, 0.63f, 0.26f, 0.81f}, {1.2f, -0.44f, 0.58f, -0.73f}, {0.39f, 0.97f, -0.21f, 0.66f}, {0.3f, 0.6f, 0.9f, 0.2f}, {0.7f, -0.2f, 0.4f, -0.6f}, {0.12f, 0.34f, 0.56f, 0.78f}, {0.98f, 0.76f, 0.54f, 0.32f}},
    {{0.19f, -0.86f, 0.42f, 0.57f}, {0.72f, 0.28f, -0.67f, 1.4f}, {-0.36f, 0.91f, 0.15f, 0.48f}, {0.84f, -0.13f, 0.69f, -0.95f}, {0.5f, 0.3f, 0.1f, 0.9f}, {-0.8f, 0.6f, -0.4f, 0.2f}, {0.13f, 0.31f, 0.53f, 0.71f}, {0.29f, 0.47f, 0.65f, 0.83f}},
};

struct TgsiCheck {
  HostAllocator& allocator;
  unsigned compared = 0, refused = 0, unsupported = 0, mismatched = 0;
  bool report = true;

  // Compiles `a`, emits it and compares the two on every input set. Returns the emit status (kSuccess when compared).
  NtStatus check(const Asm& a, const char* what) {
    UniquePointer<D3d9Program> program;
    if (D3d9Program::compile(allocator, a.bytes(), &program) != status::kSuccess) {
      ++refused;
      return status::kInvalidParameter;
    }
    String text(allocator);
    const NtStatus s = program->emit_tgsi(&text);
    if (s != status::kSuccess) {
      ++unsupported;
      return s;
    }
    const std::string source(text.data(), text.size());
    if (const char* dump = getenv("TGSI_DUMP")) {   // (set to a file name to collect every emitted text, for feeding a real TGSI parser)
      if (FILE* f = fopen(dump, "a")) {
        fprintf(f, "### %s %s\n%s", what, program->stage() == graphics::ShaderStage::Vertex ? "vertex" : "fragment", source.c_str());
        fclose(f);
      }
    }
    TexSet textures;
    for (unsigned set = 0; set < 4; ++set) {
      uint8_t constants[128];
      for (unsigned i = 0; i < 8; ++i)
        for (unsigned k = 0; k < 4; ++k) memcpy(constants + i * 16 + k * 4, &kConstants[set][i][k], 4);
      TgsiMachine m;
      m.textures = &textures;
      for (unsigned i = 0; i < 8; ++i)
        for (unsigned k = 0; k < 4; ++k) m.cst[i][k] = kConstants[set][i][k];
      bool same = true;
      if (program->stage() == graphics::ShaderStage::Vertex) {
        graphics::Vec4 attributes[4];
        for (unsigned i = 0; i < 4; ++i) {
          attributes[i] = {kInputs[set][i][0], kInputs[set][i][1], kInputs[set][i][2], kInputs[set][i][3]};
          for (unsigned k = 0; k < 4; ++k) m.in[i][k] = kInputs[set][i][k];
        }
        graphics::ShaderIo io = {};
        program->run_vertex(attributes, constants, &io);
        if (!m.run(source)) same = false;
        else {
          const float want[5][4] = {{io.position.x, io.position.y, io.position.z, io.position.w}, {io.varying[0].x, io.varying[0].y, io.varying[0].z, io.varying[0].w},
                                    {io.varying[1].x, io.varying[1].y, io.varying[1].z, io.varying[1].w}, {io.varying[2].x, io.varying[2].y, io.varying[2].z, io.varying[2].w},
                                    {io.varying[3].x, io.varying[3].y, io.varying[3].z, io.varying[3].w}};
          for (unsigned o = 0; o < 5; ++o)
            for (unsigned k = 0; k < 4; ++k) same = same && near_value(m.out[o][k], want[o][k]);
        }
      } else {
        graphics::Vec4 varyings[4];
        for (unsigned i = 0; i < 4; ++i) {
          varyings[i] = {kInputs[set][i][0], kInputs[set][i][1], kInputs[set][i][2], kInputs[set][i][3]};
          for (unsigned k = 0; k < 4; ++k) m.in[i][k] = kInputs[set][i][k];
        }
        graphics::Vec4 color_room[graphics::kMaxColorTargets] = {};
        graphics::Vec4& color = color_room[0];
        const bool alive = program->run_fragment(varyings, constants, &textures, &color);
        if (!m.run(source)) same = false;
        else {
          same = alive == !m.killed;
          if (alive) {
            const float want[4] = {color.x, color.y, color.z, color.w};
            for (unsigned k = 0; k < 4; ++k) same = same && near_value(m.out[0][k], want[k]);
          }
        }
      }
      if (!same) {
        ++mismatched;
        if (report) {
          report = false;
          printf("  tgsi mismatch in %s (input set %u):\n%s", what, set, source.c_str());
        }
        return status::kSuccess;
      }
    }
    ++compared;
    return status::kSuccess;
  }
};

// One instruction's worth of shader, with the variant picking sources, swizzles, modifiers, write mask and result modifiers.
Asm one_instruction(bool vertex, uint32_t opcode, uint32_t sources, uint32_t n) {
  static const uint32_t kSwizzles[] = {0xE4, 0x1B, 0x00, 0x4E, 0xAA, 0x39};
  static const uint32_t kModifiers[] = {0, 1, 2, 4, 6, 7, 11, 12, 3, 5, 8};
  static const uint32_t kMasks[] = {0xF, 0x3, 0x5, 0x8, 0xE};
  Asm a(vertex, 2, 0);
  struct Reg {
    uint32_t type, number;
  };
  const Reg pool[] = {{kRegInput, 0}, {kRegTemp, 1}, {kRegConst, 0}, {kRegConst, 1}, {kRegInput, 1}};
  const Reg second = vertex ? Reg{kRegInput, 2} : Reg{kRegTex, 0};
  a.op(kOpMov, {Asm::dst(kRegTemp, 0), Asm::src(kRegInput, 1)});
  a.op(kOpMov, {Asm::dst(kRegTemp, 1), Asm::src(second.type, second.number)});
  std::vector<uint32_t> params;
  const uint32_t dst_shift[] = {0, 0, 1, 15};
  uint32_t dst = Asm::dst(kRegTemp, 0, kMasks[n % 5], (n % 4) == 1 || (n % 4) == 3 ? 1 : 0) | (dst_shift[n % 4] << 24);
  params.push_back(dst);
  const bool matrix = opcode >= 20 && opcode <= 24;
  for (uint32_t i = 0; i < sources; ++i) {
    Reg r = pool[(n + i * 2) % 5];
    if (matrix && i == 1) r = {kRegConst, n % 4};
    if (i > 0 && r.type == kRegConst && !matrix) r = {kRegConst, (n + i) % 4};
    params.push_back(Asm::src(r.type, r.number, kSwizzles[(n + i) % 6], kModifiers[(n * 3 + i) % 11]));
  }
  a.op_list(opcode, params);
  if (vertex) {
    a.op(kOpMov, {Asm::dst(kRegRastOut, 0), Asm::src(kRegTemp, 0)});
    a.op(kOpMov, {Asm::dst(kRegAttrOut, 0), Asm::src(kRegTemp, 0)});
    a.op(kOpMov, {Asm::dst(kRegTexOut, 0), Asm::src(kRegTemp, 1)});
    a.op(kOpMov, {Asm::dst(kRegAttrOut, 1), Asm::src(kRegInput, 3)});
  } else {
    a.op(kOpMov, {Asm::dst(kRegColorOut, 0), Asm::src(kRegTemp, 0)});
  }
  return a.end();
}

void test_d3d9_tgsi() {
  HostAllocator allocator;
  {
    TgsiCheck t{allocator};
    // Every instruction, in both stages, with sources, swizzles, modifiers, masks and result modifiers varied.
    struct OpInfo {
      uint32_t op, sources;
    };
    static const OpInfo kOps[] = {{1, 1}, {2, 2}, {3, 2}, {4, 3}, {5, 2}, {6, 1}, {7, 1}, {8, 2}, {9, 2}, {10, 2}, {11, 2}, {12, 2}, {13, 2}, {14, 1}, {15, 1}, {16, 1}, {17, 2}, {18, 3},
                                  {19, 1}, {20, 2}, {21, 2}, {22, 2}, {23, 2}, {24, 2}, {32, 2}, {33, 2}, {34, 1}, {35, 1}, {36, 1}, {37, 1}, {88, 3}, {90, 3}};
    for (bool vertex : {false, true})
      for (const OpInfo& o : kOps)
        for (uint32_t n = 0; n < 48; ++n) t.check(one_instruction(vertex, o.op, o.sources, n), "one instruction");
    printf("  tgsi: %u compared, %u refused, %u not expressible, %u mismatched\n", t.compared, t.refused, t.unsupported, t.mismatched);
    CHECK(t.mismatched == 0 && t.compared > 2000 && t.unsupported == 0);

    // if / else / endif on a boolean, ifc in every mode (and nested), texld / texldp / texkill, relative addressing and a0.
    TgsiCheck f{allocator};
    for (bool value : {false, true}) {
      Asm a(false, 2, 0);
      a.defb(0, value);
      a.op(kOpMov, {Asm::dst(kRegTemp, 0), Asm::src(kRegInput, 1)});
      a.op(kOpIf, {Asm::src(kRegBool, 0)});
      a.op(kOpMov, {Asm::dst(kRegTemp, 0), Asm::src(kRegInput, 0)});
      a.op(kOpElse, {});
      a.op(kOpAdd, {Asm::dst(kRegTemp, 0), Asm::src(kRegInput, 0), Asm::src(kRegConst, 0)});
      a.op(kOpEndIf, {});
      a.op(kOpMov, {Asm::dst(kRegColorOut, 0), Asm::src(kRegTemp, 0)});
      a.end();
      CHECK(f.check(a, "if") == status::kSuccess);
    }
    for (uint32_t mode = 1; mode <= 6; ++mode)
      for (uint32_t pick = 0; pick < 4; ++pick) {
        Asm a(false, 2, 0);
        a.op(kOpMov, {Asm::dst(kRegTemp, 0), Asm::src(kRegInput, 1)});
        a.op(41, {Asm::src(kRegInput, pick % 2), Asm::src(kRegConst, pick / 2, 0x55)}, mode);
        a.op(kOpMov, {Asm::dst(kRegTemp, 0), Asm::src(kRegInput, 0)});
        a.op(41, {Asm::src(kRegTex, 0), Asm::src(kRegInput, 1, 0xAA)}, 7 - mode);
        a.op(kOpMul, {Asm::dst(kRegTemp, 0), Asm::src(kRegTemp, 0), Asm::src(kRegConst, 1)});
        a.op(kOpEndIf, {});
        a.op(kOpElse, {});
        a.op(kOpAdd, {Asm::dst(kRegTemp, 0), Asm::src(kRegTemp, 0), Asm::src(kRegConst, 0)});
        a.op(kOpEndIf, {});
        a.op(kOpMov, {Asm::dst(kRegColorOut, 0), Asm::src(kRegTemp, 0)});
        a.end();
        CHECK(f.check(a, "ifc") == status::kSuccess);
      }
    for (uint32_t control = 0; control < 2; ++control)
      for (uint32_t unit = 0; unit < 2; ++unit) {
        Asm a(false, 2, 0);
        a.dcl_2d(unit);
        a.op(kOpTexLd, {Asm::dst(kRegTemp, 0), Asm::src(kRegTex, 0), Asm::src(kRegSampler, unit)}, control);
        a.op(kOpMul, {Asm::dst(kRegTemp, 0), Asm::src(kRegTemp, 0), Asm::src(kRegInput, 0)});
        a.op(kOpMov, {Asm::dst(kRegColorOut, 0), Asm::src(kRegTemp, 0)});
        a.end();
        CHECK(f.check(a, "texld") == status::kSuccess);
      }
    for (uint32_t pick = 0; pick < 4; ++pick) {
      Asm a(false, 2, 0);
      a.op(kOpMov, {Asm::dst(kRegTemp, 1), Asm::src(kRegInput, pick % 2, 0xE4, pick >= 2 ? 4 : 0)});
      a.op(65, {Asm::dst(kRegTemp, 1)});
      a.op(kOpMov, {Asm::dst(kRegColorOut, 0), Asm::src(kRegInput, 0)});
      a.end();
      CHECK(f.check(a, "texkill") == status::kSuccess);
    }
    // A vertex shader: matrix transform with a def, mova and relative constants, vs_1_1's mov to a0.
    {
      Asm a(true, 2, 0);
      a.def(10, 0.5f, -0.25f, 2.0f, 1.0f);
      a.op(kOpM4x4, {Asm::dst(kRegTemp, 0), Asm::src(kRegInput, 0), Asm::src(kRegConst, 0)});
      a.op(kOpAdd, {Asm::dst(kRegRastOut, 0), Asm::src(kRegTemp, 0), Asm::src(kRegConst, 10)});
      a.op(46, {Asm::dst(kRegTex, 0, 1), Asm::src(kRegInput, 1, 0x00)});
      a.op(kOpMov, {Asm::dst(kRegAttrOut, 0), Asm::src(kRegConst, 4) | (1u << 13), Asm::src(kRegTex, 0, 0x00)});
      a.op(kOpMov, {Asm::dst(kRegTexOut, 0), Asm::src(kRegInput, 2)});
      a.end();
      CHECK(f.check(a, "def with relative") == status::kNotSupported);   // (a def cannot be indexed)
      Asm b(true, 2, 0);
      b.op(kOpM4x4, {Asm::dst(kRegTemp, 0), Asm::src(kRegInput, 0), Asm::src(kRegConst, 0)});
      b.op(kOpMov, {Asm::dst(kRegRastOut, 0), Asm::src(kRegTemp, 0)});
      b.op(46, {Asm::dst(kRegTex, 0, 1), Asm::src(kRegInput, 1, 0x00)});
      b.op(kOpMov, {Asm::dst(kRegAttrOut, 0), Asm::src(kRegConst, 1) | (1u << 13), Asm::src(kRegTex, 0, 0x00)});
      b.op(kOpMov, {Asm::dst(kRegTexOut, 0), Asm::src(kRegInput, 2)});
      b.end();
      CHECK(f.check(b, "mova relative") == status::kSuccess);
      Asm c(true, 1, 1);   // vs_1_1: mov a0.x truncates
      c.op(kOpMov, {Asm::dst(kRegRastOut, 0), Asm::src(kRegInput, 0)});
      c.op(kOpMov, {Asm::dst(kRegTex, 0, 1), Asm::src(kRegInput, 1, 0x00)});
      c.op(kOpMov, {Asm::dst(kRegAttrOut, 0), Asm::src(kRegConst, 1) | (1u << 13)});
      c.end();
      CHECK(f.check(c, "vs_1_1 a0") == status::kSuccess);
      // A def constant read as an operand and a large one that the text cannot say.
      Asm d(false, 2, 0);
      d.def(9, 0.125f, -3.5f, 100.0f, 0.0078125f);
      d.op(4, {Asm::dst(kRegColorOut, 0), Asm::src(kRegInput, 0), Asm::src(kRegConst, 9), Asm::src(kRegConst, 0)});
      d.end();
      CHECK(f.check(d, "def") == status::kSuccess);
      Asm big(false, 2, 0);
      big.def(9, 1e20f, 0, 0, 0);
      big.op(kOpMov, {Asm::dst(kRegColorOut, 0), Asm::src(kRegConst, 9)});
      big.end();
      CHECK(f.check(big, "big def") == status::kNotSupported);
    }
    printf("  tgsi control flow and friends: %u compared, %u mismatched\n", f.compared, f.mismatched);
    CHECK(f.mismatched == 0 && f.compared >= 30);

    // Truncated and bit-flipped streams: whatever compiles must emit (or be refused) without a sanitizer report.
    unsigned emitted = 0, refused = 0;
    uint32_t seed = 12345;
    auto rnd = [&]() { seed = seed * 1664525u + 1013904223u; return seed >> 8; };
    for (uint32_t n = 0; n < 60; ++n) {
      const OpInfo& o = kOps[n % (sizeof(kOps) / sizeof(kOps[0]))];
      const Asm base = one_instruction((n & 1) != 0, o.op, o.sources, n);
      const size_t words = base.t.size();
      for (size_t cut = 1; cut < words; ++cut) {
        UniquePointer<D3d9Program> p;
        String text(allocator);
        if (D3d9Program::compile(allocator, ConstByteView(reinterpret_cast<const uint8_t*>(base.t.data()), cut * 4), &p) == status::kSuccess) {
          p->emit_tgsi(&text) == status::kSuccess ? ++emitted : ++refused;
        }
      }
      for (int flip = 0; flip < 40; ++flip) {
        Asm copy = base;
        copy.t[rnd() % words] ^= 1u << (rnd() % 32);
        UniquePointer<D3d9Program> p;
        String text(allocator);
        if (D3d9Program::compile(allocator, copy.bytes(), &p) == status::kSuccess) {
          p->emit_tgsi(&text) == status::kSuccess ? ++emitted : ++refused;
        }
      }
    }
    printf("  tgsi fuzz: %u emitted, %u refused\n", emitted, refused);
    CHECK(emitted > 0);
  }
  CHECK(allocator.live == 0);
}

// A device that passes everything to another and remembers the last shader module it was asked for.
class SpyDevice final : public graphics::IGraphicsDevice {
 public:
  explicit SpyDevice(graphics::IGraphicsDevice& inner) : inner_(inner) {}
  std::string tgsi;
  bool had_function = false;
  uint64_t svga_tokens = 0;
  graphics::EuProgram eu_program = graphics::EuProgram::None;
  uint32_t eu_varying = 0;
  graphics::QpuProgram qpu_program = graphics::QpuProgram::None;
  bool qpu_rows = false;
  uint32_t limit = 0;   // 0: the inner device's push constant limit
  uint32_t feature_mask = 0xFFFFFFFFu;   // the features the device lets through
  graphics::DeviceInfo info() const override {
    graphics::DeviceInfo i = inner_.info();
    i.features &= feature_mask;
    if (limit) i.max_push_constant_bytes = limit;
    return i;
  }
  graphics::IBuffer* create_buffer(uint64_t size, uint32_t usage) override { return inner_.create_buffer(size, usage); }
  void destroy_buffer(graphics::IBuffer* buffer) override { inner_.destroy_buffer(buffer); }
  graphics::IImage* create_image(const graphics::ImageDesc& desc) override { return inner_.create_image(desc); }
  void destroy_image(graphics::IImage* image) override { inner_.destroy_image(image); }
  graphics::IShaderModule* create_shader_module(const graphics::ShaderModuleDesc& desc) override {
    tgsi = desc.tgsi ? desc.tgsi : "";
    had_function = desc.vertex_function || desc.fragment_function || desc.textured_fragment_function;
    svga_tokens = desc.d3d9_tokens ? desc.d3d9_token_count : 0;
    eu_program = desc.eu_program;
    eu_varying = desc.eu_varying;
    qpu_program = desc.qpu_program;
    qpu_rows = desc.qpu_matrix_rows;
    return inner_.create_shader_module(desc);
  }
  void destroy_shader_module(graphics::IShaderModule* module) override { inner_.destroy_shader_module(module); }
  graphics::IPipeline* create_graphics_pipeline(const graphics::GraphicsPipelineDesc& desc) override { return inner_.create_graphics_pipeline(desc); }
  void destroy_pipeline(graphics::IPipeline* pipeline) override { inner_.destroy_pipeline(pipeline); }
  graphics::ICommandBuffer* create_command_buffer() override { return inner_.create_command_buffer(); }
  void destroy_command_buffer(graphics::ICommandBuffer* commands) override { inner_.destroy_command_buffer(commands); }
  bool submit(graphics::ICommandBuffer* commands) override { return inner_.submit(commands); }
  bool present(graphics::IImage* image) override { return inner_.present(image); }

 private:
  graphics::IGraphicsDevice& inner_;
};

// A Direct3D 9 shader made through the kernel reaches the device as native functions and as TGSI text, both.
void test_d3d9_tgsi_reaches_device() {
  HostAllocator allocator;
  {
    FakeScreen screen(8, 6);
    graphics::SoftwareDevice software(allocator, &screen);
    SpyDevice spy(software);
    Kernel kernel(allocator);
    AdapterConfig c;
    c.display = &screen;
    kernel.add_adapter(spy, c);
    Handle adapter = 0, device = 0;
    CHECK(kernel.open_process(kPid) == status::kSuccess);
    CHECK(kernel.open_adapter_from_index(kPid, 0, &adapter) == status::kSuccess);
    CHECK(kernel.create_device(kPid, adapter, &device) == status::kSuccess);
    const Asm vs = color_vs(), ps = color_ps();
    ShaderDesc d;
    d.kind = ShaderKind::D3d9Bytecode;
    d.code = vs.bytes();
    Handle h = 0;
    CHECK(kernel.create_shader(kPid, device, d, &h) == status::kSuccess);
    CHECK(spy.had_function && spy.tgsi.rfind("VERT\n", 0) == 0 && spy.tgsi.find("MUL OUT[0]") != std::string::npos && spy.tgsi.find("END") != std::string::npos);
    d.code = ps.bytes();
    CHECK(kernel.create_shader(kPid, device, d, &h) == status::kSuccess);
    CHECK(spy.had_function && spy.tgsi.rfind("FRAG\n", 0) == 0 && spy.tgsi.find("MOV OUT[0]") != std::string::npos);
    // A shader the text cannot say still runs on the interpreter: a relative address with a def.
    Asm odd(true, 2, 0);
    odd.def(10, 1, 2, 3, 4);
    odd.op(kOpMov, {Asm::dst(kRegRastOut, 0), Asm::src(kRegConst, 10)});
    odd.op(46, {Asm::dst(kRegTex, 0, 1), Asm::src(kRegInput, 1, 0x00)});
    odd.op(kOpMov, {Asm::dst(kRegAttrOut, 0), Asm::src(kRegConst, 1) | (1u << 13), Asm::src(kRegTex, 0, 0x00)});
    odd.end();
    d.code = odd.bytes();
    CHECK(kernel.create_shader(kPid, device, d, &h) == status::kSuccess);
    CHECK(spy.had_function && spy.tgsi.empty());
    kernel.close_process(kPid);
  }
  CHECK(allocator.live == 0);
}

// --- DXBC ----------------------------------------------------------------------------------------------------------------------------------------------------------------

using Tokens = std::vector<uint32_t>;

Tokens dx_dst(uint32_t type, uint32_t index, uint32_t mask) { return {2u | (mask << 4) | (type << 12) | (1u << 20), index}; }
Tokens dx_src(uint32_t type, uint32_t index, uint32_t swizzle = 0xE4, uint32_t modifier = 0) {
  Tokens t = {2u | (1u << 2) | (swizzle << 4) | (type << 12) | (1u << 20) | (modifier ? 1u << 31 : 0u)};
  if (modifier) t.push_back(1u | (modifier << 6));
  t.push_back(index);
  return t;
}
Tokens dx_scalar(uint32_t type, uint32_t index, uint32_t component) { return {2u | (2u << 2) | (component << 4) | (type << 12) | (1u << 20), index}; }
Tokens dx_cb(uint32_t index, uint32_t element, uint32_t swizzle = 0xE4, uint32_t modifier = 0) {
  Tokens t = {2u | (1u << 2) | (swizzle << 4) | (8u << 12) | (2u << 20) | (modifier ? 1u << 31 : 0u)};
  if (modifier) t.push_back(1u | (modifier << 6));
  t.push_back(index), t.push_back(element);
  return t;
}
Tokens dx_imm(float a, float b, float c, float d) { return {0x4002u, __builtin_bit_cast(uint32_t, a), __builtin_bit_cast(uint32_t, b), __builtin_bit_cast(uint32_t, c), __builtin_bit_cast(uint32_t, d)}; }
Tokens dx_resource(uint32_t index, uint32_t swizzle = 0xE4) { return {2u | (1u << 2) | (swizzle << 4) | (7u << 12) | (1u << 20), index}; }
Tokens dx_sampler(uint32_t index) { return {(6u << 12) | (1u << 20), index}; }
Tokens dx_null() { return {13u << 12}; }

struct DxbcAsm {
  explicit DxbcAsm(bool vertex_stage) : vertex(vertex_stage) {}
  bool vertex;
  Tokens code;
  struct Element {
    const char* name;
    uint32_t index, reg;
  };
  std::vector<Element> inputs, outputs;
  uint32_t version_major = 4;
  uint32_t constant_buffer = 8;   // size of the cb0 the program declares, in vec4s (0: none)

  DxbcAsm& ins(uint32_t opcode, std::initializer_list<Tokens> operands, uint32_t bits = 0) {
    size_t words = 1;
    for (const Tokens& o : operands) words += o.size();
    code.push_back(opcode | bits | (static_cast<uint32_t>(words) << 24));
    for (const Tokens& o : operands) code.insert(code.end(), o.begin(), o.end());
    return *this;
  }
  DxbcAsm& raw(std::initializer_list<uint32_t> words) {
    code.insert(code.end(), words.begin(), words.end());
    return *this;
  }
  static void put(std::vector<uint8_t>& out, uint32_t v) {
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<uint8_t>(v >> (8 * i)));
  }
  static std::vector<uint8_t> signature(const std::vector<Element>& elements) {
    std::vector<uint8_t> out;
    put(out, static_cast<uint32_t>(elements.size())), put(out, 8);
    uint32_t name_at = 8 + 24 * static_cast<uint32_t>(elements.size());
    std::vector<uint8_t> names;
    for (const Element& e : elements) {
      put(out, name_at + static_cast<uint32_t>(names.size())), put(out, e.index), put(out, 0), put(out, 3), put(out, e.reg), put(out, 0x0F0F);
      for (const char* c = e.name; *c; ++c) names.push_back(static_cast<uint8_t>(*c));
      names.push_back(0);
    }
    out.insert(out.end(), names.begin(), names.end());
    while (out.size() % 4) out.push_back(0);
    return out;
  }
  std::vector<uint8_t> build() const {
    std::vector<uint8_t> shader;
    // (cb0 declared with `constant_buffer` vec4s, as the compiler does)
    const uint32_t declaration[4] = {0x04000059, 0x208E46, 0, constant_buffer};
    const size_t extra = constant_buffer ? 4 : 0;
    put(shader, ((vertex ? 1u : 0u) << 16) | (version_major << 4)), put(shader, static_cast<uint32_t>(code.size() + extra + 2));
    if (constant_buffer)
      for (uint32_t w : declaration) put(shader, w);
    for (uint32_t w : code) put(shader, w);
    struct Chunk {
      uint32_t fourcc;
      std::vector<uint8_t> data;
    };
    std::vector<Chunk> chunks = {{0x4E475349, signature(inputs)}, {0x4E47534F, signature(outputs)}, {0x52444853, shader}};
    std::vector<uint8_t> out = {'D', 'X', 'B', 'C'};
    out.insert(out.end(), 16, 0);
    uint32_t offset = 32 + 4 * static_cast<uint32_t>(chunks.size()), total = offset;
    for (const Chunk& c : chunks) total += 8 + static_cast<uint32_t>(c.data.size());
    put(out, 1), put(out, total), put(out, static_cast<uint32_t>(chunks.size()));
    for (const Chunk& c : chunks) {
      put(out, offset);
      offset += 8 + static_cast<uint32_t>(c.data.size());
    }
    for (const Chunk& c : chunks) {
      put(out, c.fourcc), put(out, static_cast<uint32_t>(c.data.size()));
      out.insert(out.end(), c.data.begin(), c.data.end());
    }
    return out;
  }
};

ConstByteView view_of(const std::vector<uint8_t>& v) { return ConstByteView(v.data(), v.size()); }

// The program a DXBC container makes, or null with the status.
UniquePointer<D3d9Program> compile_dxbc(HostAllocator& allocator, const std::vector<uint8_t>& bytes, NtStatus* status_out = nullptr) {
  UniquePointer<D3d9Program> p;
  const NtStatus s = D3d9Program::compile_dxbc(allocator, view_of(bytes), &p);
  if (status_out) *status_out = s;
  return p;
}

DxbcAsm dx_pixel_shader() {
  DxbcAsm a(false);
  a.inputs = {{"COLOR", 0, 0}, {"COLOR", 1, 1}, {"TEXCOORD", 0, 2}};
  a.outputs = {{"SV_Target", 0, 0}};
  a.ins(104, {{2}});   // dcl_temps 2
  return a;
}

float apply_dx_modifier(float x, uint32_t modifier) {
  if (modifier & 2) x = fabsf(x);
  if (modifier == 1 || modifier == 3) x = -x;
  return x;
}


// A program made from DXBC, written out as Direct3D 9 tokens, compiled again and run beside the original on every input set. 0: the same; 1: not expressible; 2: they differ.
// (A NaN of the original is not compared: sqrt of a negative number has none in the tokens.)
int roundtrip_agrees(HostAllocator& allocator, const D3d9Program& original, const graphics::ITextureSet* textures) {
  List<uint32_t> tokens(allocator);
  if (original.emit_d3d9_tokens(&tokens) != status::kSuccess) return 1;
  Asm a(false, 2, 0);
  a.t.clear();
  for (uint64_t i = 0; i < tokens.count(); ++i) a.t.push_back(tokens[i]);
  UniquePointer<D3d9Program> again;
  if (D3d9Program::compile(allocator, a.bytes(), &again) != status::kSuccess) {
    printf("  emitted tokens do not compile:");
    for (uint32_t t : a.t) printf(" %08x", t);
    printf("\n");
    return 2;
  }
  if (again->stage() != original.stage()) return 2;
  for (unsigned set = 0; set < 4; ++set) {
    uint8_t constants[128];
    for (unsigned i = 0; i < 8; ++i)
      for (unsigned k = 0; k < 4; ++k) memcpy(constants + i * 16 + k * 4, &kConstants[set][i][k], 4);
    graphics::Vec4 values[4];
    for (unsigned i = 0; i < 4; ++i) values[i] = {kInputs[set][i][0], kInputs[set][i][1], kInputs[set][i][2], kInputs[set][i][3]};
    auto same = [](const graphics::Vec4& x, const graphics::Vec4& y) {
      const float xs[4] = {x.x, x.y, x.z, x.w}, ys[4] = {y.x, y.y, y.z, y.w};
      for (int k = 0; k < 4; ++k)
        if (xs[k] == xs[k] && !near_value(xs[k], ys[k])) return false;
      return true;
    };
    if (original.stage() == graphics::ShaderStage::Vertex) {
      graphics::ShaderIo x = {}, y = {};
      original.run_vertex(values, constants, &x);
      again->run_vertex(values, constants, &y);
      bool ok = same(x.position, y.position);
      for (unsigned v = 0; v < 4; ++v) ok = ok && same(x.varying[v], y.varying[v]);
      if (!ok) return 2;
    } else {
      graphics::Vec4 x = {}, y = {};
      const bool alive_x = original.run_fragment(values, constants, textures, &x), alive_y = again->run_fragment(values, constants, textures, &y);
      if (alive_x != alive_y || (alive_x && !same(x, y))) return 2;
    }
  }
  return 0;
}

void test_dxbc() {
  HostAllocator allocator;
  {
    // A vertex shader with a matrix in cb0 gives what the Direct3D 9 shader that does the same gives.
    DxbcAsm vs(true);
    vs.inputs = {{"POSITION", 0, 0}, {"COLOR", 0, 1}};
    vs.outputs = {{"SV_Position", 0, 0}, {"COLOR", 0, 1}, {"TEXCOORD", 0, 2}};
    vs.ins(104, {{1}});
    for (uint32_t row = 0; row < 4; ++row) vs.ins(17, {dx_dst(kOpTemp, 0, 1u << row), dx_src(kOpInput, 0), dx_cb(0, row)});   // dp4 r0.<row>, v0, cb0[row]
    vs.ins(54, {dx_dst(kOpOut, 0, 0xF), dx_src(kOpTemp, 0)});
    vs.ins(54, {dx_dst(kOpOut, 1, 0xF), dx_src(kOpInput, 1)});
    vs.ins(54, {dx_dst(kOpOut, 2, 0xF), dx_src(kOpInput, 1, 0x1B)});   // (o2 = v1.wzyx)
    vs.ins(62, {});
    NtStatus st = 0;
    UniquePointer<D3d9Program> pd = compile_dxbc(allocator, vs.build(), &st);
    CHECK(st == status::kSuccess && pd && pd->stage() == graphics::ShaderStage::Vertex);

    Asm ref(true, 2, 0);
    ref.op(kOpM4x4, {Asm::dst(kRegTemp, 0), Asm::src(kRegInput, 0), Asm::src(kRegConst, 0)});
    ref.op(kOpMov, {Asm::dst(kRegRastOut, 0), Asm::src(kRegTemp, 0)});
    ref.op(kOpMov, {Asm::dst(kRegAttrOut, 0), Asm::src(kRegInput, 1)});
    ref.op(kOpMov, {Asm::dst(kRegTexOut, 0), Asm::src(kRegInput, 1, 0x1B)});
    ref.end();
    UniquePointer<D3d9Program> p9;
    CHECK(D3d9Program::compile(allocator, ref.bytes(), &p9) == status::kSuccess);
    bool same = pd && p9;
    if (same)
      for (unsigned set = 0; set < 4; ++set) {
        uint8_t constants[128];
        for (unsigned i = 0; i < 8; ++i)
          for (unsigned k = 0; k < 4; ++k) memcpy(constants + i * 16 + k * 4, &kConstants[set][i][k], 4);
        graphics::Vec4 attributes[4];
        for (unsigned i = 0; i < 4; ++i) attributes[i] = {kInputs[set][i][0], kInputs[set][i][1], kInputs[set][i][2], kInputs[set][i][3]};
        graphics::ShaderIo a = {}, b = {};
        pd->run_vertex(attributes, constants, &a);
        p9->run_vertex(attributes, constants, &b);
        same = same && a.position.x == b.position.x && a.position.y == b.position.y && a.position.z == b.position.z && a.position.w == b.position.w;
        for (unsigned v = 0; v < 4; ++v) same = same && a.varying[v].x == b.varying[v].x && a.varying[v].y == b.varying[v].y && a.varying[v].z == b.varying[v].z && a.varying[v].w == b.varying[v].w;
      }
    CHECK(same);
    TgsiCheck t{allocator};
    // (and the text of it: the machine runs it against the interpreter)
    String text(allocator);
    CHECK(pd && pd->emit_tgsi(&text) == status::kSuccess && text.size() > 100);
    CHECK(pd && roundtrip_agrees(allocator, *pd, nullptr) == 0);
  }

  // Every arithmetic instruction against its definition, with swizzles, modifiers, write masks and saturate.
  {
    struct Alu {
      uint32_t opcode, sources;
      float (*f)(float, float, float);
    };
    static const Alu kAlu[] = {
        {54, 1, [](float a, float, float) { return a; }},
        {0, 2, [](float a, float b, float) { return a + b; }},
        {56, 2, [](float a, float b, float) { return a * b; }},
        {50, 3, [](float a, float b, float c) { return a * b + c; }},
        {14, 2, [](float a, float b, float) { return a / b; }},
        {51, 2, [](float a, float b, float) { return a < b ? a : b; }},
        {52, 2, [](float a, float b, float) { return a > b ? a : b; }},
        {24, 2, [](float a, float b, float) { return __builtin_bit_cast(float, a == b ? 0xFFFFFFFFu : 0u); }},   // (comparisons: all ones for true)
        {57, 2, [](float a, float b, float) { return __builtin_bit_cast(float, a != b ? 0xFFFFFFFFu : 0u); }},
        {49, 2, [](float a, float b, float) { return __builtin_bit_cast(float, a < b ? 0xFFFFFFFFu : 0u); }},
        {29, 2, [](float a, float b, float) { return __builtin_bit_cast(float, a >= b ? 0xFFFFFFFFu : 0u); }},
        {55, 3, [](float a, float b, float c) { return a != 0 ? b : c; }},
        {26, 1, [](float a, float, float) { return a - floorf(a); }},
        {75, 1, [](float a, float, float) { return sqrtf(a); }},
        {68, 1, [](float a, float, float) { return 1.0f / sqrtf(fabsf(a)); }},
        {25, 1, [](float a, float, float) { return exp2f(a); }},
        {47, 1, [](float a, float, float) { return log2f(fabsf(a)); }},
        {64, 1, [](float a, float, float) { return nearbyintf(a); }},
        {65, 1, [](float a, float, float) { return floorf(a); }},
        {66, 1, [](float a, float, float) { return ceilf(a); }},
        {67, 1, [](float a, float, float) { return truncf(a); }},
    };
    static const uint32_t kSwizzles[] = {0xE4, 0x1B, 0x00, 0x4E, 0xAA, 0x39};
    static const uint32_t kMasks[] = {0xF, 0x3, 0x5, 0x8, 0xE};
    unsigned compared = 0, wrong = 0, text_wrong = 0;
    for (const Alu& alu : kAlu)
      for (uint32_t n = 0; n < 40; ++n) {
        DxbcAsm a = dx_pixel_shader();
        // r0 = v1; r0.<mask> = op(sources); o0 = r0
        a.ins(54, {dx_dst(kOpTemp, 0, 0xF), dx_src(kOpInput, 1)});
        struct Source {
          uint32_t type, index;
          bool constant;
        };
        const Source pool[] = {{kOpInput, 0, false}, {kOpInput, 2, false}, {kOpConstantBuffer, 0, true}, {kOpConstantBuffer, 1, true}, {kOpInput, 1, false}};
        std::vector<Tokens> operands = {dx_dst(kOpTemp, 0, kMasks[n % 5])};
        uint32_t chosen[3][3];
        for (uint32_t i = 0; i < alu.sources; ++i) {
          const Source& s = pool[(n + i * 2) % 5];
          const uint32_t swizzle = kSwizzles[(n + i) % 6], modifier = (n + i) % 3 == 0 ? 1 : (n + i) % 3 == 1 ? 3 : 0;
          chosen[i][0] = s.type == kOpConstantBuffer ? 100 + s.index : s.index, chosen[i][1] = swizzle, chosen[i][2] = modifier;
          operands.push_back(s.constant ? dx_cb(0, s.index, swizzle, modifier) : dx_src(s.type, s.index, swizzle, modifier));
        }
        Tokens flat;
        for (const Tokens& o : operands) flat.insert(flat.end(), o.begin(), o.end());
        const bool saturate = n % 4 == 1;
        a.code.push_back(alu.opcode | (saturate ? 0x2000u : 0u) | (static_cast<uint32_t>(flat.size() + 1) << 24));
        a.code.insert(a.code.end(), flat.begin(), flat.end());
        a.ins(54, {dx_dst(kOpOut, 0, 0xF), dx_src(kOpTemp, 0)});
        a.ins(62, {});
        NtStatus st = 0;
        UniquePointer<D3d9Program> p = compile_dxbc(allocator, a.build(), &st);
        if (st != status::kSuccess || !p) {
          ++wrong;
          continue;
        }
        String text(allocator);
        const bool have_text = p->emit_tgsi(&text) == status::kSuccess;
        const std::string source(text.data(), text.size());
        {
          TexSet no_textures;
          const int rt = roundtrip_agrees(allocator, *p, &no_textures);
          const bool comparison = alu.opcode == 24 || alu.opcode == 57 || alu.opcode == 49 || alu.opcode == 29;   // (all ones in the original, 1.0 in the tokens: only movc, if and discard read them alike)
          if (rt == 2 && alu.opcode != 75 && !comparison) {   // (sqrt of a negative number: NaN, or 0 once saturated; the tokens give the root of its magnitude)
            if (wrong == 0) printf("  dxbc op %u variant %u: the Direct3D 9 tokens differ\n", alu.opcode, n);
            ++wrong;
          }
          if (rt == 1) ++text_wrong;
        }
        for (unsigned set = 0; set < 2; ++set) {
          uint8_t constants[128];
          for (unsigned i = 0; i < 8; ++i)
            for (unsigned k = 0; k < 4; ++k) memcpy(constants + i * 16 + k * 4, &kConstants[set][i][k], 4);
          graphics::Vec4 varyings[4];
          for (unsigned i = 0; i < 4; ++i) varyings[i] = {kInputs[set][i][0], kInputs[set][i][1], kInputs[set][i][2], kInputs[set][i][3]};
          graphics::Vec4 color_room[graphics::kMaxColorTargets] = {};
          graphics::Vec4& color = color_room[0];
          p->run_fragment(varyings, constants, nullptr, &color);
          const float got[4] = {color.x, color.y, color.z, color.w};
          // The oracle.
          float expected[4] = {varyings[1].x, varyings[1].y, varyings[1].z, varyings[1].w};
          auto fetch = [&](uint32_t i, float v[4]) {
            const uint32_t id = chosen[i][0];
            float base[4];
            if (id >= 100) memcpy(base, kConstants[set][id - 100], 16);
            else memcpy(base, kInputs[set][id], 16);   // (input register N is varying N)
            for (int k = 0; k < 4; ++k) v[k] = apply_dx_modifier(base[(chosen[i][1] >> (2 * k)) & 3], chosen[i][2]);
          };
          float s0[4] = {}, s1[4] = {}, s2[4] = {};
          fetch(0, s0);
          if (alu.sources > 1) fetch(1, s1);
          if (alu.sources > 2) fetch(2, s2);
          for (int k = 0; k < 4; ++k) {
            if (!(kMasks[n % 5] & (1u << k))) continue;
            float r = alu.f(s0[k], s1[k], s2[k]);
            if (saturate) r = r > 0 ? (r < 1 ? r : 1.0f) : 0.0f;
            expected[k] = r;
          }
          bool ok = true;
          for (int k = 0; k < 4; ++k) ok = ok && near_value(got[k], expected[k]);
          if (!ok) {
            if (wrong == 0) printf("  dxbc op %u variant %u set %u: got %g %g %g %g, expected %g %g %g %g\n", alu.opcode, n, set, got[0], got[1], got[2], got[3], expected[0], expected[1], expected[2], expected[3]);
            ++wrong;
          }
          if (have_text) {
            TgsiMachine m;
            for (unsigned i = 0; i < 8; ++i)
              for (unsigned k = 0; k < 4; ++k) m.cst[i][k] = kConstants[set][i][k];
            for (unsigned i = 0; i < 4; ++i)
              for (unsigned k = 0; k < 4; ++k) m.in[i][k] = kInputs[set][i][k];
            bool tok = m.run(source);
            for (int k = 0; k < 4; ++k) tok = tok && near_value(m.out[0][k], got[k]);
            if (!tok) {
              if (text_wrong == 0) printf("  dxbc tgsi mismatch op %u variant %u set %u:\n%s", alu.opcode, n, set, source.c_str());
              ++text_wrong;
            }
          } else {
            ++text_wrong;
          }
        }
        ++compared;
      }
    printf("  dxbc arithmetic: %u programs, %u wrong, %u tgsi mismatches\n", compared, wrong, text_wrong);
    CHECK(wrong == 0 && text_wrong == 0 && compared == sizeof(kAlu) / sizeof(kAlu[0]) * 40);
  }

  // dp2/3/4, sincos, sample (swizzled resource), discard, if / else, literals.
  {
    TexSet textures;
    auto run = [&](const DxbcAsm& a, unsigned set, graphics::Vec4* color, bool* alive, bool compare_text) {
      NtStatus st = 0;
      UniquePointer<D3d9Program> p = compile_dxbc(allocator, a.build(), &st);
      CHECK(st == status::kSuccess && p);
      if (!p) return;
      uint8_t constants[128];
      for (unsigned i = 0; i < 8; ++i)
        for (unsigned k = 0; k < 4; ++k) memcpy(constants + i * 16 + k * 4, &kConstants[set][i][k], 4);
      graphics::Vec4 varyings[4];
      for (unsigned i = 0; i < 4; ++i) varyings[i] = {kInputs[set][i][0], kInputs[set][i][1], kInputs[set][i][2], kInputs[set][i][3]};
      *alive = p->run_fragment(varyings, constants, &textures, color);
      if (set == 0) {
        const int rt = roundtrip_agrees(allocator, *p, &textures);
        if (rt != 0) printf("  dxbc round trip through Direct3D 9 tokens: %s\n", rt == 1 ? "not expressible" : "differs");
        CHECK(rt == 0);
      }
      if (!compare_text) return;
      String text(allocator);
      CHECK(p->emit_tgsi(&text) == status::kSuccess);
      TgsiMachine m;
      m.textures = &textures;
      for (unsigned i = 0; i < 8; ++i)
        for (unsigned k = 0; k < 4; ++k) m.cst[i][k] = kConstants[set][i][k];
      for (unsigned i = 0; i < 4; ++i)
        for (unsigned k = 0; k < 4; ++k) m.in[i][k] = kInputs[set][i][k];
      const std::string source(text.data(), text.size());
      bool ok = m.run(source) && *alive == !m.killed;
      if (*alive) {
        const float want[4] = {color->x, color->y, color->z, color->w};
        for (int k = 0; k < 4; ++k) ok = ok && near_value(m.out[0][k], want[k]);
      }
      if (!ok) printf("  dxbc tgsi mismatch:\n%s", source.c_str());
      CHECK(ok);
    };
    for (unsigned set = 0; set < 4; ++set) {
      graphics::Vec4 c = {};
      bool alive = false;
      {   // dp2 / dp3 / dp4 with a literal, sincos with one null destination
        DxbcAsm a = dx_pixel_shader();
        a.ins(15, {dx_dst(kOpTemp, 0, 1), dx_src(kOpInput, 0), dx_imm(0.5f, 2.0f, 0, 0)});
        a.ins(16, {dx_dst(kOpTemp, 0, 2), dx_src(kOpInput, 0), dx_src(kOpInput, 1)});
        a.ins(17, {dx_dst(kOpTemp, 0, 4), dx_src(kOpInput, 0), dx_cb(0, 1)});
        a.ins(77, {dx_dst(kOpTemp, 1, 1), dx_null(), dx_scalar(kOpInput, 1, 2)});
        a.ins(77, {dx_null(), dx_dst(kOpTemp, 1, 2), dx_scalar(kOpInput, 1, 0)});
        a.ins(0, {dx_dst(kOpTemp, 0, 8), dx_src(kOpTemp, 1, 0x00), dx_src(kOpTemp, 1, 0x55)});
        a.ins(54, {dx_dst(kOpOut, 0, 0xF), dx_src(kOpTemp, 0)});
        a.ins(62, {});
        run(a, set, &c, &alive, true);
        const float* v0 = kInputs[set][0];
        const float* v1 = kInputs[set][1];
        const float* c1 = kConstants[set][1];
        const float want[4] = {v0[0] * 0.5f + v0[1] * 2.0f, v0[0] * v1[0] + v0[1] * v1[1] + v0[2] * v1[2], v0[0] * c1[0] + v0[1] * c1[1] + v0[2] * c1[2] + v0[3] * c1[3],
                               sinf(v1[2]) + cosf(v1[0])};
        // (r1.x = sin(v1.z) is written to the sine destination, r1.y = cos(v1.x) to the cosine one; r0.w = r1.x + r1.y)
        CHECK(alive && near_value(c.x, want[0]) && near_value(c.y, want[1]) && near_value(c.z, want[2]) && near_value(c.w, want[3]));
      }
      {   // sample with a swizzled resource, then discard
        DxbcAsm a = dx_pixel_shader();
        a.ins(88, {dx_resource(0)}, 3u << 11);   // dcl_resource_texture2d t0
        a.ins(69, {dx_dst(kOpTemp, 0, 0xF), dx_src(kOpInput, 2), dx_resource(0, 0x1B), dx_sampler(0)});   // sample r0, v2, t0.wzyx, s0
        a.ins(49, {dx_dst(kOpTemp, 1, 1), dx_scalar(kOpTemp, 0, 0), dx_imm(0.5f, 0.5f, 0.5f, 0.5f)});   // lt r1.x, r0.x, 0.5   (r0.x = texel.w = 0.9: false)
        a.ins(13, {dx_scalar(kOpTemp, 1, 0)}, 1u << 18);                                                 // discard_nz r1.x
        a.ins(54, {dx_dst(kOpOut, 0, 0xF), dx_src(kOpTemp, 0)});
        a.ins(62, {});
        run(a, set, &c, &alive, true);
        const graphics::Vec4 t = textures.sample(0, kInputs[set][2][0], kInputs[set][2][1]);
        CHECK(alive && near_value(c.x, t.w) && near_value(c.y, t.z) && near_value(c.z, t.y) && near_value(c.w, t.x));
      }
      {   // discard_z kills when the test value is zero
        DxbcAsm a = dx_pixel_shader();
        a.ins(54, {dx_dst(kOpTemp, 0, 0xF), dx_imm(0, 0, 0, 0)});
        a.ins(13, {dx_scalar(kOpTemp, 0, 0)}, 0);   // discard_z
        a.ins(54, {dx_dst(kOpOut, 0, 0xF), dx_src(kOpInput, 0)});
        a.ins(62, {});
        run(a, set, &c, &alive, true);
        CHECK(!alive);
      }
      {   // if_nz / else / endif, nested, on a comparison
        DxbcAsm a = dx_pixel_shader();
        a.ins(49, {dx_dst(kOpTemp, 0, 1), dx_scalar(kOpInput, 0, 0), dx_cb(0, 0, 0x55)});   // r0.x = v0.x < cb0[0].y
        a.ins(31, {dx_scalar(kOpTemp, 0, 0)}, 1u << 18);                                       // if_nz r0.x
        a.ins(54, {dx_dst(kOpTemp, 1, 0xF), dx_src(kOpInput, 0)});
        a.ins(49, {dx_dst(kOpTemp, 0, 1), dx_scalar(kOpInput, 1, 1), dx_imm(0.2f, 0.2f, 0.2f, 0.2f)});
        a.ins(31, {dx_scalar(kOpTemp, 0, 0)}, 1u << 18);
        a.ins(56, {dx_dst(kOpTemp, 1, 0xF), dx_src(kOpTemp, 1), dx_cb(0, 1)});
        a.ins(18, {});
        a.ins(0, {dx_dst(kOpTemp, 1, 0xF), dx_src(kOpTemp, 1), dx_imm(1, 1, 1, 1)});
        a.ins(21, {});
        a.ins(18, {});
        a.ins(54, {dx_dst(kOpTemp, 1, 0xF), dx_src(kOpInput, 1)});
        a.ins(21, {});
        a.ins(54, {dx_dst(kOpOut, 0, 0xF), dx_src(kOpTemp, 1)});
        a.ins(62, {});
        run(a, set, &c, &alive, true);
        const float* v0 = kInputs[set][0];
        const float* v1 = kInputs[set][1];
        const float* c0 = kConstants[set][0];
        const float* c1 = kConstants[set][1];
        float want[4];
        for (int k = 0; k < 4; ++k) want[k] = v0[0] < c0[1] ? (v1[1] < 0.2f ? v0[k] * c1[k] : v0[k] + 1.0f) : v1[k];
        CHECK(alive && near_value(c.x, want[0]) && near_value(c.y, want[1]) && near_value(c.z, want[2]) && near_value(c.w, want[3]));
      }
    }

    // Refusals and malformed containers.
    NtStatus st = 0;
    {
      DxbcAsm a = dx_pixel_shader();
      a.ins(45, {dx_dst(kOpTemp, 0, 0xF), dx_src(kOpTemp, 0), dx_src(kOpTemp, 1)});   // ld
      a.ins(62, {});
      compile_dxbc(allocator, a.build(), &st);
      CHECK(st == status::kNotSupported);
      DxbcAsm loop = dx_pixel_shader();
      loop.ins(76, {dx_src(kOpTemp, 0)});   // switch
      compile_dxbc(allocator, loop.build(), &st);
      CHECK(st == status::kNotSupported);
      compile_dxbc(allocator, std::vector<uint8_t>(8, 0), &st);
      CHECK(st == status::kInvalidParameter);
      DxbcAsm vs_no_position(true);
      vs_no_position.inputs = {{"POSITION", 0, 0}};
      vs_no_position.outputs = {{"COLOR", 0, 1}};
      vs_no_position.ins(54, {dx_dst(kOpOut, 1, 0xF), dx_src(kOpInput, 0)});
      vs_no_position.ins(62, {});
      compile_dxbc(allocator, vs_no_position.build(), &st);
      CHECK(st == status::kInvalidParameter);
      DxbcAsm uses_position(false);
      uses_position.inputs = {{"SV_Position", 0, 0}};
      uses_position.outputs = {{"SV_Target", 0, 0}};
      uses_position.ins(54, {dx_dst(kOpOut, 0, 0xF), dx_src(kOpInput, 0)});
      uses_position.ins(62, {});
      compile_dxbc(allocator, uses_position.build(), &st);
      CHECK(st == status::kNotSupported);
    }

    // Truncated and bit-flipped containers: refused or compiled, never a sanitizer report; what compiles also emits or is refused.
    {
      DxbcAsm a = dx_pixel_shader();
      a.ins(88, {dx_resource(0)}, 3u << 11);
      a.ins(69, {dx_dst(kOpTemp, 0, 0xF), dx_src(kOpInput, 2), dx_resource(0, 0x1B), dx_sampler(0)});
      a.ins(49, {dx_dst(kOpTemp, 1, 1), dx_scalar(kOpTemp, 0, 0), dx_imm(0.5f, 0.5f, 0.5f, 0.5f)});
      a.ins(31, {dx_scalar(kOpTemp, 1, 0)}, 1u << 18);
      a.ins(55, {dx_dst(kOpTemp, 0, 0xF), dx_src(kOpTemp, 0), dx_cb(0, 2), dx_src(kOpInput, 1)}, 0x2000);
      a.ins(21, {});
      a.ins(54, {dx_dst(kOpOut, 0, 0xF), dx_src(kOpTemp, 0)});
      a.ins(62, {});
      const std::vector<uint8_t> good = a.build();
      unsigned compiled = 0, refused = 0;
      for (size_t cut = 0; cut < good.size(); ++cut) {
        std::vector<uint8_t> t(good.begin(), good.begin() + static_cast<long>(cut));
        UniquePointer<D3d9Program> p = compile_dxbc(allocator, t, &st);
        st == status::kSuccess ? ++compiled : ++refused;
      }
      uint32_t seed = 777;
      for (int i = 0; i < 4000; ++i) {
        std::vector<uint8_t> t = good;
        for (int k = 0; k < 1 + i % 3; ++k) {
          seed = seed * 1664525u + 1013904223u;
          t[(seed >> 8) % t.size()] ^= static_cast<uint8_t>(1u << ((seed >> 3) % 8));
        }
        UniquePointer<D3d9Program> p = compile_dxbc(allocator, t, &st);
        if (st == status::kSuccess) {
          ++compiled;
          String text(allocator);
          p->emit_tgsi(&text);
          graphics::Vec4 varyings[4] = {}, color[graphics::kMaxColorTargets];
          uint8_t constants[128] = {};
          p->run_fragment(varyings, constants, &textures, color);
        } else {
          ++refused;
        }
      }
      printf("  dxbc fuzz: %u compiled, %u refused\n", compiled, refused);
      CHECK(compiled > 0 && refused > 0);
    }
  }
  CHECK(allocator.live == 0);
}

// DXBC made by a real HLSL compiler (Wine's d3dcompiler_47, which is vkd3d-shader; sources next to the blobs in tests/data/dxbc) rather than by the assembler above.
std::vector<uint8_t> read_blob(const char* name) {
  std::vector<uint8_t> out;
  char path[256];
  snprintf(path, sizeof path, "tests/data/dxbc/%s", name);
  if (FILE* f = fopen(path, "rb")) {
    uint8_t buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) out.insert(out.end(), buf, buf + n);
    fclose(f);
  }
  return out;
}

void test_dxbc_real() {
  HostAllocator allocator;
  {
    TexSet textures;
    NtStatus st = 0;
    const std::vector<uint8_t> vs_blob = read_blob("vs.dxbc"), color_blob = read_blob("ps_color.dxbc"), tex_blob = read_blob("ps_tex.dxbc"), math_blob = read_blob("ps_math.dxbc");
    CHECK(!vs_blob.empty() && !color_blob.empty() && !tex_blob.empty() && !math_blob.empty());
    UniquePointer<D3d9Program> vs = compile_dxbc(allocator, vs_blob, &st);
    CHECK(st == status::kSuccess && vs);
    UniquePointer<D3d9Program> color = compile_dxbc(allocator, color_blob, &st);
    CHECK(st == status::kSuccess && color);
    UniquePointer<D3d9Program> tex = compile_dxbc(allocator, tex_blob, &st);
    CHECK(st == status::kSuccess && tex && tex->uses_textures());
    UniquePointer<D3d9Program> math = compile_dxbc(allocator, math_blob, &st);
    CHECK(st == status::kSuccess && math);
    if (!vs || !color || !tex || !math) return;

    auto agrees = [&](const D3d9Program& p, unsigned set, graphics::Vec4* varyings, const uint8_t* constants, graphics::Vec4* color_out, bool* alive, const char* what) {
      String text(allocator);
      CHECK(p.emit_tgsi(&text) == status::kSuccess);
      TgsiMachine m;
      m.textures = &textures;
      for (unsigned i = 0; i < 8; ++i)
        for (unsigned k = 0; k < 4; ++k) memcpy(&m.cst[i][k], constants + i * 16 + k * 4, 4);
      for (unsigned i = 0; i < 4; ++i) {
        const float v[4] = {varyings[i].x, varyings[i].y, varyings[i].z, varyings[i].w};
        for (unsigned k = 0; k < 4; ++k) m.in[i][k] = v[k];
      }
      const std::string source(text.data(), text.size());
      bool ok = m.run(source) && *alive == !m.killed;
      if (*alive) {
        const float want[4] = {color_out->x, color_out->y, color_out->z, color_out->w};
        for (int k = 0; k < 4; ++k) ok = ok && near_value(m.out[0][k], want[k]);
      }
      if (!ok) printf("  real dxbc %s tgsi mismatch (set %u):\n%s", what, set, source.c_str());
      CHECK(ok);
    };

    for (const UniquePointer<D3d9Program>* program : {&vs, &color, &tex, &math}) {
      const int rt = roundtrip_agrees(allocator, **program, &textures);
      if (rt != 0) printf("  real dxbc round trip through Direct3D 9 tokens: %s\n", rt == 1 ? "not expressible" : "differs");
      CHECK(rt == 0);
    }
    for (unsigned set = 0; set < 4; ++set) {
      uint8_t constants[128];
      for (unsigned i = 0; i < 8; ++i)
        for (unsigned k = 0; k < 4; ++k) memcpy(constants + i * 16 + k * 4, &kConstants[set][i][k], 4);
      graphics::Vec4 attributes[4], varyings[4];
      for (unsigned i = 0; i < 4; ++i) attributes[i] = varyings[i] = {kInputs[set][i][0], kInputs[set][i][1], kInputs[set][i][2], kInputs[set][i][3]};
      const float(*in)[4] = kInputs[set];
      const float(*cb)[4] = kConstants[set];

      // vs: mul(float4(p, 1), m) with m column major: dot((p, 1), cb[j]); the colour as it is; texcoord0 = colour.xy * 2
      graphics::ShaderIo io = {};
      vs->run_vertex(attributes, constants, &io);
      const float p4[4] = {in[0][0], in[0][1], in[0][2], 1.0f};
      float clip[4];
      for (int j = 0; j < 4; ++j) clip[j] = p4[0] * cb[j][0] + p4[1] * cb[j][1] + p4[2] * cb[j][2] + p4[3] * cb[j][3];
      CHECK(near_value(io.position.x, clip[0]) && near_value(io.position.y, -clip[1]) && near_value(io.position.z, clip[2]) && near_value(io.position.w, clip[3]));
      CHECK(near_value(io.varying[0].x, in[1][0]) && near_value(io.varying[0].y, in[1][1]) && near_value(io.varying[0].z, in[1][2]) && near_value(io.varying[0].w, in[1][3]));
      CHECK(near_value(io.varying[2].x, in[1][0] * 2) && near_value(io.varying[2].y, in[1][1] * 2));
      {
        String text(allocator);
        CHECK(vs->emit_tgsi(&text) == status::kSuccess);
        TgsiMachine m;
        for (unsigned i = 0; i < 8; ++i)
          for (unsigned k = 0; k < 4; ++k) m.cst[i][k] = cb[i][k];
        for (unsigned i = 0; i < 4; ++i)
          for (unsigned k = 0; k < 4; ++k) m.in[i][k] = in[i][k];
        const std::string source(text.data(), text.size());
        bool ok = m.run(source);
        const float want[3][4] = {{io.position.x, io.position.y, io.position.z, io.position.w}, {io.varying[0].x, io.varying[0].y, io.varying[0].z, io.varying[0].w},
                                  {io.varying[2].x, io.varying[2].y, io.varying[2].z, io.varying[2].w}};
        const unsigned slot[3] = {0, 1, 3};
        for (int o = 0; o < 3; ++o)
          for (int k = 0; k < 4; ++k) ok = ok && near_value(m.out[slot[o]][k], want[o][k]);
        if (!ok) printf("  real vs tgsi mismatch:\n%s", source.c_str());
        CHECK(ok);
      }

      // ps_color: the colour
      graphics::Vec4 c = {};
      bool alive = color->run_fragment(varyings, constants, nullptr, &c);
      CHECK(alive && near_value(c.x, in[0][0]) && near_value(c.y, in[0][1]) && near_value(c.z, in[0][2]) && near_value(c.w, in[0][3]));
      agrees(*color, set, varyings, constants, &c, &alive, "ps_color");

      // ps_tex: tex.Sample(s, t) * c  (t is TEXCOORD0: varying 2)
      alive = tex->run_fragment(varyings, constants, &textures, &c);
      const graphics::Vec4 texel = textures.sample(0, in[2][0], in[2][1]);
      CHECK(alive && near_value(c.x, texel.x * in[0][0]) && near_value(c.y, texel.y * in[0][1]) && near_value(c.z, texel.z * in[0][2]) && near_value(c.w, texel.w * in[0][3]));
      agrees(*tex, set, varyings, constants, &c, &alive, "ps_tex");

      // ps_math
      alive = math->run_fragment(varyings, constants, &textures, &c);
      const float* cc = in[0];
      const float* d = in[1];
      const float tx = in[2][0], ty = in[2][1];
      const bool expect_alive = tx - 0.1f >= 0;
      CHECK(alive == expect_alive);
      if (alive && expect_alive) {
        float r[4] = {cc[0], cc[1], cc[2], cc[3]};
        if (tx > 0.5f)
          for (float& x : r) x *= 2;
        const float len = sqrtf(r[0] * r[0] + r[1] * r[1] + r[2] * r[2]);
        for (int k = 0; k < 3; ++k) r[k] /= len;
        const float s = sqrtf(fabsf(ty));
        r[3] = s < 0 ? 0 : s > 1 ? 1 : s;
        const float dp = cc[0] * d[0] + cc[1] * d[1] + cc[2] * d[2] + cc[3] * d[3];
        float want[4];
        for (int k = 0; k < 4; ++k) {
          const float lerp = cc[k] + tx * ((1 - d[k]) - cc[k]);
          const float fl = floorf(d[k] * 3), fr = cc[k] * 7 - floorf(cc[k] * 7);
          want[k] = r[k] * dp + lerp * cb[0][k] + powf(fabsf(cb[1][k]), 2.2f) + fl + fr;
        }
        const bool ok = near_value(c.x, want[0]) && near_value(c.y, want[1]) && near_value(c.z, want[2]) && near_value(c.w, want[3]);
        if (!ok) printf("  real ps_math set %u: got %g %g %g %g, expected %g %g %g %g\n", set, c.x, c.y, c.z, c.w, want[0], want[1], want[2], want[3]);
        CHECK(ok);
      }
      agrees(*math, set, varyings, constants, &c, &alive, "ps_math");
    }

    // Each blob truncated and bit-flipped.
    unsigned compiled = 0, refused = 0;
    uint32_t seed = 4242;
    // The volume, array and cube shaders say what they sample in the program and in the TGSI text.
    const std::vector<uint8_t> volume_blob = read_blob("ps_tex3d.dxbc"), array_blob = read_blob("ps_texarray.dxbc"), cube_blob = read_blob("ps_texcube.dxbc");
    {
      struct Kind { const std::vector<uint8_t>* blob; graphics::TextureKind kind; const char* name; };
      for (const Kind& k : {Kind{&volume_blob, graphics::TextureKind::Texture3D, "3D, FLOAT"}, Kind{&array_blob, graphics::TextureKind::Texture2DArray, "2D_ARRAY, FLOAT"},
                            Kind{&cube_blob, graphics::TextureKind::TextureCube, "CUBE, FLOAT"}}) {
        UniquePointer<D3d9Program> p = compile_dxbc(allocator, *k.blob, &st);
        CHECK(st == status::kSuccess && p && p->texture_kind(0) == k.kind && p->texture_kind(1) == graphics::TextureKind::Texture2D);
        CHECK((p->required_features() & graphics::kFeatureTextureKinds) != 0);
        String text(allocator);
        CHECK(p->emit_tgsi(&text) == status::kSuccess && strstr(std::string(text.view().data(), text.size()).c_str(), k.name));
      }
    }
    const std::vector<uint8_t> compute_blob = read_blob("cs_fill.dxbc"), geometry_blob = read_blob("gs_copy.dxbc"), int_write_blob = read_blob("ps_int_write.dxbc"), int_load_blob = read_blob("ps_int_load.dxbc"),
                                     hull_blob = read_blob("hs_tri.dxbc"), domain_blob = read_blob("ds_dent.dxbc");
    for (const std::vector<uint8_t>* blob : {&vs_blob, &color_blob, &tex_blob, &math_blob, &volume_blob, &array_blob, &cube_blob, &compute_blob, &geometry_blob, &int_write_blob, &int_load_blob, &hull_blob, &domain_blob}) {
      for (size_t cut = 0; cut < blob->size(); cut += 3) {
        std::vector<uint8_t> t(blob->begin(), blob->begin() + static_cast<long>(cut));
        UniquePointer<D3d9Program> p = compile_dxbc(allocator, t, &st);
        st == status::kSuccess ? ++compiled : ++refused;
      }
      for (int i = 0; i < 3000; ++i) {
        std::vector<uint8_t> t = *blob;
        seed = seed * 1664525u + 1013904223u;
        t[(seed >> 8) % t.size()] ^= static_cast<uint8_t>(1u << ((seed >> 3) % 8));
        UniquePointer<D3d9Program> p = compile_dxbc(allocator, t, &st);
        if (st == status::kSuccess) {
          ++compiled;
          String text(allocator);
          p->emit_tgsi(&text);
          graphics::Vec4 v[4] = {}, col[graphics::kMaxColorTargets];
          uint8_t constants[128] = {};
          p->run_fragment(v, constants, &textures, col);
          graphics::ShaderIo io;
          p->run_vertex(v, constants, &io);
          // (a compute shader, on buffers that are a little too small for what it does)
          uint8_t storage[graphics::kMaxStorageBuffers][64] = {};
          graphics::ComputeContext context;
          for (uint32_t k = 0; k < graphics::kMaxStorageBuffers; ++k) context.buffers[k] = ByteView(storage[k], sizeof storage[k]);
          context.dispatch_id[0] = context.dispatch_id[1] = 5;
          p->run_compute(context, constants);
          // (a geometry shader, given a triangle)
          graphics::GeometryInputData in;
          for (uint32_t k = 0; k < 3; ++k) in.position[k] = {static_cast<float>(k), 1.0f, 0.5f, 1.0f};
          graphics::GeometryOutputData out;
          p->run_geometry(in, constants, &out);
          CHECK(out.count <= graphics::kMaxGeometryVertices);
          // (a hull and a domain shader, given a patch)
          graphics::HullInputData hin;
          hin.control_points = 3;
          for (uint32_t k = 0; k < 3; ++k) hin.position[k] = {static_cast<float>(k), 1.0f, 0.5f, 1.0f};
          graphics::HullOutputData hout;
          p->run_hull(hin, constants, &hout);
          graphics::ShaderIo domain_io;
          p->run_domain(hout, 0.25f, 0.25f, 0.5f, constants, &domain_io);
        } else {
          ++refused;
        }
      }
    }
    printf("  real dxbc fuzz: %u compiled, %u refused\n", compiled, refused);
  }
  CHECK(allocator.live == 0);
}

// --- Bytecode for the devices without a shader compiler -------------------------------------------------------------------------------------------------------------------

void test_device_forms() {
  HostAllocator allocator;
  {
    // SVGA3D: a vertex shader's position goes through a temporary multiplied by c31; run through the interpreter with c31 defined, the result is the original with Y flipped.
    unsigned checked = 0, refused = 0, wrong = 0;
    static const uint32_t kOps[] = {1, 2, 5, 4, 8, 9, 10, 11, 18, 19, 20, 32, 34, 35, 88};
    for (uint32_t version : {0xFFFE0200u, 0xFFFE0101u})
    for (uint32_t op : kOps)
      for (uint32_t n = 0; n < 24; ++n) {
        const uint32_t sources = op == 1 || op == 19 || op == 34 || op == 35 ? 1 : (op == 4 || op == 18 || op == 88) ? 3 : 2;
        Asm original = one_instruction(true, op, sources, n);
        original.t[0] = version;   // (vs_2_0, or vs_1_1: the same instructions, whose length the opcode gives)
        UniquePointer<D3d9Program> program;
        if (D3d9Program::compile(allocator, original.bytes(), &program) != status::kSuccess) continue;
        List<uint32_t> tokens(allocator);
        if (program->emit_svga_tokens(original.bytes(), &tokens) != status::kSuccess) {
          ++refused;
          continue;
        }
        Asm rewritten(true, 2, 0);   // (the version word is the rewritten stream's own)
        rewritten.t.clear();
        for (uint64_t i = 0; i < tokens.count(); ++i) rewritten.t.push_back(tokens[i]);
        // (insert `def c31, 1, -1, 1, 1` after the version token)
        const uint32_t def[] = {kOpDef | (5u << 24), Asm::dst(kRegConst, 31), __builtin_bit_cast(uint32_t, 1.0f), __builtin_bit_cast(uint32_t, -1.0f), __builtin_bit_cast(uint32_t, 1.0f),
                                __builtin_bit_cast(uint32_t, 1.0f)};
        rewritten.t.insert(rewritten.t.begin() + 1, def, def + 6);
        UniquePointer<D3d9Program> second;
        if (D3d9Program::compile(allocator, rewritten.bytes(), &second) != status::kSuccess) {
          ++wrong;
          continue;
        }
        for (unsigned set = 0; set < 4; ++set) {
          uint8_t constants[128];
          for (unsigned i = 0; i < 8; ++i)
            for (unsigned k = 0; k < 4; ++k) memcpy(constants + i * 16 + k * 4, &kConstants[set][i][k], 4);
          graphics::Vec4 attributes[4];
          for (unsigned i = 0; i < 4; ++i) attributes[i] = {kInputs[set][i][0], kInputs[set][i][1], kInputs[set][i][2], kInputs[set][i][3]};
          graphics::ShaderIo a = {}, b = {};
          program->run_vertex(attributes, constants, &a);
          second->run_vertex(attributes, constants, &b);
          bool ok = near_value(a.position.x, b.position.x) && near_value(a.position.y, -b.position.y) && near_value(a.position.z, b.position.z) && near_value(a.position.w, b.position.w);
          for (unsigned v = 0; v < 4; ++v)
            ok = ok && near_value(a.varying[v].x, b.varying[v].x) && near_value(a.varying[v].y, b.varying[v].y) && near_value(a.varying[v].z, b.varying[v].z) &&
                 near_value(a.varying[v].w, b.varying[v].w);
          if (!ok) ++wrong;
        }
        ++checked;
      }
    printf("  svga tokens: %u vertex shaders rewritten and run, %u refused, %u wrong\n", checked, refused, wrong);
    CHECK(checked > 200 && wrong == 0);

    // The declarations name the inputs the way the device's vertex declaration does: location 1 a colour, 2 and up texture coordinates.
    Asm vs(true, 2, 0);
    vs.dcl(0, 0, kRegInput, 0).dcl(5, 3, kRegInput, 1).dcl(5, 0, kRegInput, 2);   // (position, texcoord3 at v1, texcoord0 at v2)
    vs.op(kOpMov, {Asm::dst(kRegRastOut, 0), Asm::src(kRegInput, 0)});
    vs.op(kOpMov, {Asm::dst(kRegAttrOut, 0), Asm::src(kRegInput, 1)});
    vs.op(kOpMov, {Asm::dst(kRegTexOut, 0), Asm::src(kRegInput, 2)});
    vs.end();
    UniquePointer<D3d9Program> program;
    CHECK(D3d9Program::compile(allocator, vs.bytes(), &program) == status::kSuccess);
    List<uint32_t> tokens(allocator);
    CHECK(program && program->emit_svga_tokens(vs.bytes(), &tokens) == status::kSuccess);
    bool colour_at_1 = false, texcoord_0_at_2 = false, mul_at_end = false;
    for (uint64_t i = 0; i + 2 < tokens.count(); ++i) {
      if (tokens[i] == (kOpDcl | (2u << 24)) && tokens[i + 2] == Asm::dst(kRegInput, 1)) colour_at_1 = (tokens[i + 1] & 0x1F) == 10 && ((tokens[i + 1] >> 16) & 0xF) == 0;
      if (tokens[i] == (kOpDcl | (2u << 24)) && tokens[i + 2] == Asm::dst(kRegInput, 2)) texcoord_0_at_2 = (tokens[i + 1] & 0x1F) == 5 && ((tokens[i + 1] >> 16) & 0xF) == 0;
    }
    mul_at_end = tokens.count() > 6 && tokens[tokens.count() - 1] == 0xFFFF && tokens[tokens.count() - 2] == 0xA0E4001F;
    CHECK(colour_at_1 && texcoord_0_at_2 && mul_at_end);

    // Pixel shaders go as they are; one that reads the push constants, or samples, is not for that device.
    const Asm ps = color_ps();
    CHECK(D3d9Program::compile(allocator, ps.bytes(), &program) == status::kSuccess);
    List<uint32_t> ps_tokens(allocator);
    CHECK(program->emit_svga_tokens(ps.bytes(), &ps_tokens) == status::kSuccess && ps_tokens.count() * 4 == ps.t.size() * 4);
    Asm reads(false, 2, 0);
    reads.op(kOpMul, {Asm::dst(kRegColorOut, 0), Asm::src(kRegInput, 0), Asm::src(kRegConst, 0)});
    reads.end();
    CHECK(D3d9Program::compile(allocator, reads.bytes(), &program) == status::kSuccess);
    List<uint32_t> none(allocator), reads_tokens(allocator);
    CHECK(program->emit_svga_tokens(reads.bytes(), &reads_tokens) == status::kSuccess && reads_tokens.count() == reads.t.size());   // (the device sets the push constants for pixel shaders too)
    Asm uses_c31(true, 2, 0);
    uses_c31.def(31, 1, 1, 1, 1);
    uses_c31.op(kOpMov, {Asm::dst(kRegRastOut, 0), Asm::src(kRegInput, 0)});
    uses_c31.end();
    CHECK(D3d9Program::compile(allocator, uses_c31.bytes(), &program) == status::kSuccess);
    CHECK(program->emit_svga_tokens(uses_c31.bytes(), &none) == status::kNotSupported);
    // A DXBC program is not Direct3D 9 tokens (it is written as such first).
    const std::vector<uint8_t> blob = read_blob("ps_color.dxbc");
    UniquePointer<D3d9Program> dx = compile_dxbc(allocator, blob);
    CHECK(dx && dx->emit_svga_tokens(ConstByteView(blob.data(), blob.size()), &none) == status::kNotSupported);

    // The Raspberry Pi's vertex kernel: the matrix transform, as columns (the built-in shader) or rows (dp4), or neither.
    {
      uint8_t colour = 99;
      Asm rows(true, 2, 0);   // m4x4 oPos, v0, c0 ; mov oD0, v1
      rows.op(kOpM4x4, {Asm::dst(kRegRastOut, 0), Asm::src(kRegInput, 0), Asm::src(kRegConst, 0)});
      rows.op(kOpMov, {Asm::dst(kRegAttrOut, 0), Asm::src(kRegInput, 1)});
      rows.end();
      CHECK(D3d9Program::compile(allocator, rows.bytes(), &program) == status::kSuccess && program->classify_vertex(&colour) == VertexKernel::MatrixRows && colour == 0);
      Asm columns(true, 2, 0);   // mul r0, c0, v0.x ; mad r0, c1, v0.y, r0 ; mad r0, c2, v0.z, r0 ; add oPos, r0, c3 ; mov oT1, v1
      columns.op(kOpMul, {Asm::dst(kRegTemp, 0), Asm::src(kRegConst, 0), Asm::src(kRegInput, 0, 0x00)});
      columns.op(4, {Asm::dst(kRegTemp, 0), Asm::src(kRegConst, 1), Asm::src(kRegInput, 0, 0x55), Asm::src(kRegTemp, 0)});
      columns.op(4, {Asm::dst(kRegTemp, 0), Asm::src(kRegConst, 2), Asm::src(kRegInput, 0, 0xAA), Asm::src(kRegTemp, 0)});
      columns.op(kOpAdd, {Asm::dst(kRegRastOut, 0), Asm::src(kRegTemp, 0), Asm::src(kRegConst, 3)});
      columns.op(kOpMov, {Asm::dst(kRegTexOut, 1), Asm::src(kRegInput, 1)});
      columns.end();
      CHECK(D3d9Program::compile(allocator, columns.bytes(), &program) == status::kSuccess && program->classify_vertex(&colour) == VertexKernel::MatrixColumns && colour == 3);
      // The matrix the V3D device loads (kernel_matrix_words) makes the fixed kernel give what the shader gives, for both forms.
      for (const Asm* shader : {&rows, &columns}) {
        UniquePointer<D3d9Program> p;
        CHECK(D3d9Program::compile(allocator, shader->bytes(), &p) == status::kSuccess);
        const VertexKernel k = p->classify_vertex(&colour);
        bool same = true;
        uint32_t seed = 99;
        for (int probe = 0; probe < 8; ++probe) {
          uint8_t constants[128] = {};
          uint32_t words[16];
          for (uint32_t i = 0; i < 16; ++i) {
            seed = seed * 1664525u + 1013904223u;
            const float f = static_cast<float>(static_cast<int32_t>(seed >> 8) % 2001 - 1000) * 0.002f;
            memcpy(constants + 4 * i, &f, 4);
            words[i] = __builtin_bit_cast(uint32_t, f);
          }
          graphics::Vec4 attributes[4] = {{0.3f, -0.4f, 0.7f, 1.0f}, {0.1f, 0.2f, 0.3f, 0.4f}, {}, {}};
          graphics::ShaderIo shader_out = {}, kernel_out = {};
          p->run_vertex(attributes, constants, &shader_out);
          graphics::kernel_matrix_words(words, k == VertexKernel::MatrixRows, true);
          uint8_t kernel_constants[128] = {};
          for (uint32_t i = 0; i < 16; ++i) memcpy(kernel_constants + 4 * i, &words[i], 4);
          graphics::colored_vertex_shader().vertex_function(attributes, kernel_constants, &kernel_out);
          same = same && near_value(shader_out.position.x, kernel_out.position.x) && near_value(shader_out.position.y, kernel_out.position.y) &&
                 near_value(shader_out.position.z, kernel_out.position.z) && near_value(shader_out.position.w, kernel_out.position.w);
        }
        CHECK(same);
      }
      Asm other(true, 2, 0);   // the same plus a constant: not the kernel
      other.op(kOpM4x4, {Asm::dst(kRegTemp, 0), Asm::src(kRegInput, 0), Asm::src(kRegConst, 0)});
      other.op(kOpAdd, {Asm::dst(kRegRastOut, 0), Asm::src(kRegTemp, 0), Asm::src(kRegConst, 4)});
      other.op(kOpMov, {Asm::dst(kRegAttrOut, 0), Asm::src(kRegInput, 1)});
      other.end();
      CHECK(D3d9Program::compile(allocator, other.bytes(), &program) == status::kSuccess && program->classify_vertex(&colour) == VertexKernel::None);
      Asm tinted(true, 2, 0);   // the colour halved: not the kernel
      tinted.op(kOpM4x4, {Asm::dst(kRegRastOut, 0), Asm::src(kRegInput, 0), Asm::src(kRegConst, 0)});
      tinted.op(kOpMul, {Asm::dst(kRegAttrOut, 0), Asm::src(kRegInput, 1), Asm::src(kRegConst, 5)});
      tinted.end();
      CHECK(D3d9Program::compile(allocator, tinted.bytes(), &program) == status::kSuccess && program->classify_vertex(&colour) == VertexKernel::None);
    }
    // Intel Gen9: which pixel shaders are one of its two kernels.
    uint8_t varying = 99;
    CHECK(D3d9Program::compile(allocator, ps.bytes(), &program) == status::kSuccess && program->classify_fragment(&varying) == FragmentKernel::Colored && varying == 0);
    Asm sample(false, 2, 0);
    sample.dcl_2d(0);
    sample.op(kOpTexLd, {Asm::dst(kRegTemp, 0), Asm::src(kRegTex, 0), Asm::src(kRegSampler, 0)});
    sample.op(kOpMov, {Asm::dst(kRegColorOut, 0), Asm::src(kRegTemp, 0)});
    sample.end();
    CHECK(D3d9Program::compile(allocator, sample.bytes(), &program) == status::kSuccess && program->classify_fragment(&varying) == FragmentKernel::Textured && varying == 2);
    Asm scaled = sample;
    scaled.t.back() = 0xFFFF;
    Asm tinted(false, 2, 0);   // texture times colour: not a kernel
    tinted.dcl_2d(0);
    tinted.op(kOpTexLd, {Asm::dst(kRegTemp, 0), Asm::src(kRegTex, 0), Asm::src(kRegSampler, 0)});
    tinted.op(kOpMul, {Asm::dst(kRegColorOut, 0), Asm::src(kRegTemp, 0), Asm::src(kRegInput, 0)});
    tinted.end();
    CHECK(D3d9Program::compile(allocator, tinted.bytes(), &program) == status::kSuccess && program->classify_fragment(&varying) == FragmentKernel::None);
    Asm swizzled(false, 2, 0);   // a colour with its channels swapped: not a kernel either
    swizzled.op(kOpMov, {Asm::dst(kRegColorOut, 0), Asm::src(kRegInput, 0, 0x1B)});
    swizzled.end();
    CHECK(D3d9Program::compile(allocator, swizzled.bytes(), &program) == status::kSuccess && program->classify_fragment(&varying) == FragmentKernel::None);
    Asm saturated(false, 2, 0);
    saturated.op(kOpMov, {Asm::dst(kRegColorOut, 0, 0xF, 1), Asm::src(kRegInput, 0)});
    saturated.end();
    CHECK(D3d9Program::compile(allocator, saturated.bytes(), &program) == status::kSuccess && program->classify_fragment(&varying) == FragmentKernel::None);
    UniquePointer<D3d9Program> vsp = compile_dxbc(allocator, read_blob("vs.dxbc"));
    CHECK(vsp && vsp->classify_fragment(&varying) == FragmentKernel::None);
    // The same two from a real compiler's DXBC.
    UniquePointer<D3d9Program> pc = compile_dxbc(allocator, read_blob("ps_color.dxbc")), psample = compile_dxbc(allocator, read_blob("ps_sample.dxbc")), ptex = compile_dxbc(allocator, read_blob("ps_tex.dxbc"));
    CHECK(pc && pc->classify_fragment(&varying) == FragmentKernel::Colored && varying == 0);
    CHECK(psample && psample->classify_fragment(&varying) == FragmentKernel::Textured && varying == 2);
    CHECK(ptex && ptex->classify_fragment(&varying) == FragmentKernel::None);
  }

  // What the kernel hands a device: the native functions, the TGSI text, the SVGA tokens, the Gen9 kernel.
  {
    FakeScreen screen(8, 6);
    graphics::SoftwareDevice software(allocator, &screen);
    SpyDevice spy(software);
    Kernel kernel(allocator);
    AdapterConfig c;
    c.display = &screen;
    kernel.add_adapter(spy, c);
    Handle adapter = 0, device = 0, h = 0;
    CHECK(kernel.open_process(kPid) == status::kSuccess);
    CHECK(kernel.open_adapter_from_index(kPid, 0, &adapter) == status::kSuccess);
    CHECK(kernel.create_device(kPid, adapter, &device) == status::kSuccess);
    ShaderDesc d;
    d.kind = ShaderKind::D3d9Bytecode;
    const Asm vs = color_vs(), ps = color_ps();
    d.code = vs.bytes();
    CHECK(kernel.create_shader(kPid, device, d, &h) == status::kSuccess);
    CHECK(spy.had_function && spy.svga_tokens > 8 && spy.eu_program == graphics::EuProgram::None && spy.qpu_program == graphics::QpuProgram::None);   // (color_vs flips nothing the kernel does)
    d.code = ps.bytes();
    CHECK(kernel.create_shader(kPid, device, d, &h) == status::kSuccess);
    CHECK(spy.had_function && spy.svga_tokens == ps.t.size() && spy.eu_program == graphics::EuProgram::ColoredFragment && spy.eu_varying == 0);
    // A DXBC shader: functions, text, and the SVGA tokens of the Direct3D 9 stream it can be written as .
    const std::vector<uint8_t> blob = read_blob("ps_sample.dxbc");
    d.kind = ShaderKind::Dxbc;
    d.code = view_of(blob);
    CHECK(kernel.create_shader(kPid, device, d, &h) == status::kSuccess);
    CHECK(spy.had_function && spy.svga_tokens > 8 && spy.eu_program == graphics::EuProgram::TexturedFragment && spy.eu_varying == 2 && spy.tgsi.rfind("FRAG\n", 0) == 0);
    // The vertex shader of a real compiler: the SVGA tokens end with the position multiplied by c31.
    {
      const std::vector<uint8_t> vblob = read_blob("vs_5_1.dxbc");
      d.code = view_of(vblob);
      CHECK(kernel.create_shader(kPid, device, d, &h) == status::kSuccess && spy.svga_tokens > 16);
    }
    // ... and a shader made through the escape ABI.
    const std::vector<uint8_t> vs_blob = read_blob("vs.dxbc");
    ByteBuffer request(allocator);
    uint8_t reply[16] = {};
    request.put32(static_cast<uint32_t>(EscapeOp::CreateShader)), request.put32(static_cast<uint32_t>(ShaderKind::Dxbc)), request.put32(0), request.put32(static_cast<uint32_t>(vs_blob.size()));
    for (uint8_t b : vs_blob) request.put8(b);
    while (request.size() % 4) request.put8(0);
    uint32_t reply_bytes = 0;
    CHECK(kernel.escape(kPid, device, request.view(), ByteView(reply, sizeof reply), &reply_bytes) == status::kSuccess && reply_bytes == 4);
    kernel.close_process(kPid);
  }
  CHECK(allocator.live == 0);
}

// --- Shader Model 3 on SVGA3D, Shader Model 5.1 -------------------------------------------------------------------------------------------------------------------------
// The blobs are from Wine's HLSL compiler (tests/data/dxbc): the same HLSL as the Shader Model 4.0 blobs, so every model must give what 4.0 gives.

bool same_vec(const graphics::Vec4& a, const graphics::Vec4& b) { return near_value(a.x, b.x) && near_value(a.y, b.y) && near_value(a.z, b.z) && near_value(a.w, b.w); }

void test_sm3_and_sm51() {
  HostAllocator allocator;
  {
    TexSet textures;
    NtStatus st = 0;
    struct Pair {
      const char* base;
      const char* other;
    };
    static const Pair kPairs[] = {{"vs.dxbc", "vs_5_0.dxbc"}, {"vs.dxbc", "vs_5_1.dxbc"}, {"ps_color.dxbc", "ps_color_5_1.dxbc"}, {"ps_tex.dxbc", "ps_tex_5_1.dxbc"},
                                  {"ps_math.dxbc", "ps_math_5_0.dxbc"}, {"ps_math.dxbc", "ps_math_5_1.dxbc"}, {"ps_sample.dxbc", "ps_sample_5_1.dxbc"}};
    for (const Pair& pair : kPairs) {
      UniquePointer<D3d9Program> a = compile_dxbc(allocator, read_blob(pair.base), &st);
      CHECK(st == status::kSuccess && a);
      UniquePointer<D3d9Program> b = compile_dxbc(allocator, read_blob(pair.other), &st);
      if (st != status::kSuccess || !b) printf("  %s: status %x\n", pair.other, static_cast<unsigned>(st));
      CHECK(st == status::kSuccess && b);
      if (!a || !b) continue;
      CHECK(a->stage() == b->stage() && a->uses_textures() == b->uses_textures());
      bool agree = true;
      for (unsigned set = 0; set < 4; ++set) {
        uint8_t constants[128];
        for (unsigned i = 0; i < 8; ++i)
          for (unsigned k = 0; k < 4; ++k) memcpy(constants + i * 16 + k * 4, &kConstants[set][i][k], 4);
        graphics::Vec4 values[4];
        for (unsigned i = 0; i < 4; ++i) values[i] = {kInputs[set][i][0], kInputs[set][i][1], kInputs[set][i][2], kInputs[set][i][3]};
        if (a->stage() == graphics::ShaderStage::Vertex) {
          graphics::ShaderIo x = {}, y = {};
          a->run_vertex(values, constants, &x);
          b->run_vertex(values, constants, &y);
          agree = agree && same_vec(x.position, y.position);
          for (unsigned v = 0; v < 4; ++v) agree = agree && same_vec(x.varying[v], y.varying[v]);
        } else {
          graphics::Vec4 x = {}, y = {};
          const bool alive_a = a->run_fragment(values, constants, &textures, &x), alive_b = b->run_fragment(values, constants, &textures, &y);
          agree = agree && alive_a == alive_b && (!alive_a || same_vec(x, y));
        }
      }
      if (!agree) printf("  %s differs from %s\n", pair.other, pair.base);
      CHECK(agree);
      String text(allocator);
      CHECK(b->emit_tgsi(&text) == status::kSuccess);
    }
    // Ranges the push constants cannot be: a space, another register, an array.
    {
      std::vector<uint8_t> blob = read_blob("ps_math_5_1.dxbc");
      // (the declaration `dcl_constant_buffer cb0[0:0], size 2, space 0`: change the space to 1, which this device has nothing for)
      bool patched = false;
      for (size_t i = 0; i + 28 <= blob.size() && !patched; i += 4) {
        uint32_t w[7];
        memcpy(w, &blob[i], sizeof w);
        if (w[0] == 0x7000059 && w[1] == 0x308e46 && w[6] == 0) {
          const uint32_t space = 1;
          memcpy(&blob[i + 24], &space, 4);
          patched = true;
        }
      }
      CHECK(patched);
      compile_dxbc(allocator, blob, &st);
      CHECK(st == status::kNotSupported);
    }
    // Truncations and bit flips of the 5.1 blobs.
    unsigned compiled = 0, refused = 0;
    uint32_t seed = 99;
    for (const char* name : {"vs_5_1.dxbc", "ps_tex_5_1.dxbc", "ps_math_5_1.dxbc"}) {
      const std::vector<uint8_t> blob = read_blob(name);
      for (size_t cut = 0; cut < blob.size(); cut += 5) {
        compile_dxbc(allocator, std::vector<uint8_t>(blob.begin(), blob.begin() + static_cast<long>(cut)), &st);
        st == status::kSuccess ? ++compiled : ++refused;
      }
      for (int i = 0; i < 3000; ++i) {
        std::vector<uint8_t> t = blob;
        seed = seed * 1664525u + 1013904223u;
        t[(seed >> 8) % t.size()] ^= static_cast<uint8_t>(1u << ((seed >> 3) % 8));
        UniquePointer<D3d9Program> p = compile_dxbc(allocator, t, &st);
        if (st == status::kSuccess) {
          ++compiled;
          String text(allocator);
          p->emit_tgsi(&text);
        } else {
          ++refused;
        }
      }
    }
    printf("  sm5.1 fuzz: %u compiled, %u refused\n", compiled, refused);
    CHECK(compiled > 0 && refused > 0);

    // Shader Model 3 (real bytecode): the same shader gives what the 4.0 one gives, and goes to SVGA3D with the position redirected.
    const std::vector<uint8_t> vs3 = read_blob("vs_3_0.sm3"), ps3 = read_blob("ps_color_3_0.sm3");
    UniquePointer<D3d9Program> v3, p3, v4 = compile_dxbc(allocator, read_blob("vs.dxbc"));
    CHECK(D3d9Program::compile(allocator, view_of(vs3), &v3) == status::kSuccess && D3d9Program::compile(allocator, view_of(ps3), &p3) == status::kSuccess && v3 && p3 && v4);
    if (v3 && p3 && v4) {
      bool agree = true;
      for (unsigned set = 0; set < 4; ++set) {
        uint8_t constants[128];
        for (unsigned i = 0; i < 8; ++i)
          for (unsigned k = 0; k < 4; ++k) memcpy(constants + i * 16 + k * 4, &kConstants[set][i][k], 4);
        graphics::Vec4 values[4];
        for (unsigned i = 0; i < 4; ++i) values[i] = {kInputs[set][i][0], kInputs[set][i][1], kInputs[set][i][2], kInputs[set][i][3]};
        graphics::ShaderIo x = {}, y = {};
        v3->run_vertex(values, constants, &x);
        v4->run_vertex(values, constants, &y);
        agree = agree && same_vec(x.position, y.position) && same_vec(x.varying[0], y.varying[0]);
        agree = agree && near_value(x.varying[2].x, y.varying[2].x) && near_value(x.varying[2].y, y.varying[2].y);
        graphics::Vec4 c = {};
        p3->run_fragment(values, constants, nullptr, &c);
        agree = agree && same_vec(c, values[0]);
      }
      CHECK(agree);
      List<uint32_t> tokens(allocator);
      CHECK(v3->emit_svga_tokens(view_of(vs3), &tokens) == status::kSuccess);
      // (the declared position output is o0 here: the stream ends with mul o0, rT, c31)
      CHECK(tokens.count() > 8 && tokens[tokens.count() - 1] == 0xFFFF && tokens[tokens.count() - 2] == 0xA0E4001F && tokens[tokens.count() - 5] == (kOpMul | (3u << 24)) &&
            tokens[tokens.count() - 4] == 0xE00F0000u);
      // Run the rewritten stream with c31 defined: the original with Y flipped.
      Asm rewritten(true, 3, 0);
      rewritten.t.clear();
      for (uint64_t i = 0; i < tokens.count(); ++i) rewritten.t.push_back(tokens[i]);
      const uint32_t def[] = {kOpDef | (5u << 24), Asm::dst(kRegConst, 31), __builtin_bit_cast(uint32_t, 1.0f), __builtin_bit_cast(uint32_t, -1.0f), __builtin_bit_cast(uint32_t, 1.0f),
                              __builtin_bit_cast(uint32_t, 1.0f)};
      rewritten.t.insert(rewritten.t.begin() + 1, def, def + 6);
      UniquePointer<D3d9Program> flipped;
      CHECK(D3d9Program::compile(allocator, rewritten.bytes(), &flipped) == status::kSuccess && flipped);
      bool flips = flipped != nullptr;
      for (unsigned set = 0; set < 4 && flipped; ++set) {
        uint8_t constants[128];
        for (unsigned i = 0; i < 8; ++i)
          for (unsigned k = 0; k < 4; ++k) memcpy(constants + i * 16 + k * 4, &kConstants[set][i][k], 4);
        graphics::Vec4 values[4];
        for (unsigned i = 0; i < 4; ++i) values[i] = {kInputs[set][i][0], kInputs[set][i][1], kInputs[set][i][2], kInputs[set][i][3]};
        graphics::ShaderIo x = {}, y = {};
        v3->run_vertex(values, constants, &x);
        flipped->run_vertex(values, constants, &y);
        flips = flips && near_value(x.position.x, y.position.x) && near_value(x.position.y, -y.position.y) && near_value(x.position.z, y.position.z) && near_value(x.position.w, y.position.w);
        for (unsigned v = 0; v < 4; ++v) flips = flips && same_vec(x.varying[v], y.varying[v]);
      }
      CHECK(flips);
      List<uint32_t> ps_tokens(allocator);
      CHECK(p3->emit_svga_tokens(view_of(ps3), &ps_tokens) == status::kSuccess && ps_tokens.count() * 4 == ps3.size());
    }

    // Shader Model 1.1 (real bytecode, no instruction lengths): the same shader again, and on its way to SVGA3D.
    {
      const std::vector<uint8_t> vs1 = read_blob("vs_1_1.sm1");
      UniquePointer<D3d9Program> v1;
      CHECK(D3d9Program::compile(allocator, view_of(vs1), &v1) == status::kSuccess && v1 && v4);
      List<uint32_t> tokens1(allocator);
      CHECK(v1 && v1->emit_svga_tokens(view_of(vs1), &tokens1) == status::kSuccess);
      if (v1 && v4 && tokens1.count()) {
        Asm rewritten(true, 1, 1);
        rewritten.t.clear();
        for (uint64_t i = 0; i < tokens1.count(); ++i) rewritten.t.push_back(tokens1[i]);
        const uint32_t def[] = {kOpDef | (5u << 24), Asm::dst(kRegConst, 31), __builtin_bit_cast(uint32_t, 1.0f), __builtin_bit_cast(uint32_t, -1.0f), __builtin_bit_cast(uint32_t, 1.0f),
                                __builtin_bit_cast(uint32_t, 1.0f)};
        rewritten.t.insert(rewritten.t.begin() + 1, def, def + 6);
        UniquePointer<D3d9Program> flipped;
        CHECK(D3d9Program::compile(allocator, rewritten.bytes(), &flipped) == status::kSuccess && flipped);
        bool ok = flipped != nullptr;
        for (unsigned set = 0; set < 4 && flipped; ++set) {
          uint8_t constants[128];
          for (unsigned i = 0; i < 8; ++i)
            for (unsigned k = 0; k < 4; ++k) memcpy(constants + i * 16 + k * 4, &kConstants[set][i][k], 4);
          graphics::Vec4 values[4];
          for (unsigned i = 0; i < 4; ++i) values[i] = {kInputs[set][i][0], kInputs[set][i][1], kInputs[set][i][2], kInputs[set][i][3]};
          graphics::ShaderIo x = {}, y = {}, reference = {};
          v1->run_vertex(values, constants, &x);
          flipped->run_vertex(values, constants, &y);
          v4->run_vertex(values, constants, &reference);
          ok = ok && same_vec(x.position, reference.position) && same_vec(x.varying[0], reference.varying[0]) && near_value(x.varying[2].x, reference.varying[2].x);
          ok = ok && near_value(x.position.x, y.position.x) && near_value(x.position.y, -y.position.y) && near_value(x.position.z, y.position.z) && near_value(x.position.w, y.position.w);
        }
        CHECK(ok);
      }
    }

    // A vs_3_0 whose position is not o0, with a texture coordinate where the device wants a colour.
    Asm vs(true, 3, 0);
    vs.dcl(0, 0, kRegInput, 0).dcl(5, 0, kRegInput, 1);
    vs.dcl(10, 0, kRegTexOut, 0).dcl(0, 0, kRegTexOut, 3);   // (colour at o0, position at o3)
    vs.op(kOpMov, {Asm::dst(kRegTexOut, 3), Asm::src(kRegInput, 0)});
    vs.op(kOpMov, {Asm::dst(kRegTexOut, 0), Asm::src(kRegInput, 1)});
    vs.end();
    UniquePointer<D3d9Program> program;
    List<uint32_t> tokens(allocator);
    CHECK(D3d9Program::compile(allocator, vs.bytes(), &program) == status::kSuccess && program && program->emit_svga_tokens(vs.bytes(), &tokens) == status::kSuccess);
    bool texcoord_became_colour = false;
    for (uint64_t i = 0; i + 2 < tokens.count(); ++i)
      if (tokens[i] == (kOpDcl | (2u << 24)) && tokens[i + 2] == Asm::dst(kRegInput, 1)) texcoord_became_colour = (tokens[i + 1] & 0x1F) == 10;
    CHECK(texcoord_became_colour && tokens.count() > 4 && tokens[tokens.count() - 4] == (0xE00F0000u | 3) && tokens[tokens.count() - 2] == 0xA0E4001F);
  }
  CHECK(allocator.live == 0);
}

// --- Shader Model 1 pixel shaders ----------------------------------------------------------------------------------------------------------------------------------------

void test_ps1() {
  HostAllocator allocator;
  {
    TexSet textures;
    // Run `a` on every input set; `expect` gives the colour (or null: killed) from the inputs and constants. The same program is checked as TGSI text and as SVGA tokens.
    auto check = [&](const Asm& a, const char* what, auto expect) {
      UniquePointer<D3d9Program> p;
      const NtStatus st = D3d9Program::compile(allocator, a.bytes(), &p);
      if (st != status::kSuccess || !p) {
        printf("  ps_1_x %s: status %x\n", what, static_cast<unsigned>(st));
        CHECK(false);
        return;
      }
      String text(allocator);
      CHECK(p->emit_tgsi(&text) == status::kSuccess);
      List<uint32_t> tokens(allocator);
      CHECK(p->emit_svga_tokens(a.bytes(), &tokens) == status::kSuccess && tokens.count() * 4 == a.t.size() * 4);
      const std::string source(text.data(), text.size());
      for (unsigned set = 0; set < 4; ++set) {
        uint8_t constants[128];
        for (unsigned i = 0; i < 8; ++i)
          for (unsigned k = 0; k < 4; ++k) memcpy(constants + i * 16 + k * 4, &kConstants[set][i][k], 4);
        graphics::Vec4 varyings[4];
        for (unsigned i = 0; i < 4; ++i) varyings[i] = {kInputs[set][i][0], kInputs[set][i][1], kInputs[set][i][2], kInputs[set][i][3]};
        graphics::Vec4 got = {};
        const bool alive = p->run_fragment(varyings, constants, &textures, &got);
        float want[4];
        const bool expect_alive = expect(set, textures, want);
        bool ok = alive == expect_alive;
        if (alive && expect_alive) ok = ok && near_value(got.x, want[0]) && near_value(got.y, want[1]) && near_value(got.z, want[2]) && near_value(got.w, want[3]);
        if (!ok) printf("  ps_1_x %s set %u: got %g %g %g %g (%s), expected %g %g %g %g (%s)\n", what, set, got.x, got.y, got.z, got.w, alive ? "alive" : "killed", want[0], want[1], want[2], want[3],
                        expect_alive ? "alive" : "killed");
        CHECK(ok);
        TgsiMachine m;
        m.textures = &textures;
        for (unsigned i = 0; i < 8; ++i)
          for (unsigned k = 0; k < 4; ++k) m.cst[i][k] = kConstants[set][i][k];
        for (unsigned i = 0; i < 4; ++i)
          for (unsigned k = 0; k < 4; ++k) m.in[i][k] = kInputs[set][i][k];
        bool tok = m.run(source) && alive == !m.killed;
        if (alive)
          for (int k = 0; k < 4; ++k) tok = tok && near_value(m.out[0][k], k == 0 ? got.x : k == 1 ? got.y : k == 2 ? got.z : got.w);
        if (!tok) printf("  ps_1_x %s tgsi mismatch:\n%s", what, source.c_str());
        CHECK(tok);
      }
    };
    auto in = [](unsigned set, unsigned i, unsigned k) { return kInputs[set][i][k]; };
    auto cb = [](unsigned set, unsigned i, unsigned k) { return kConstants[set][i][k]; };

    {   // tex t0 ; mul r0, t0, v0
      Asm a(false, 1, 1);
      a.op(66, {Asm::dst(kRegTex, 0)});
      a.op(kOpMul, {Asm::dst(kRegTemp, 0), Asm::src(kRegTex, 0), Asm::src(kRegInput, 0)});
      a.end();
      check(a, "tex", [&](unsigned set, const TexSet& t, float* w) {
        const graphics::Vec4 s = t.sample(0, in(set, 2, 0), in(set, 2, 1));
        w[0] = s.x * in(set, 0, 0), w[1] = s.y * in(set, 0, 1), w[2] = s.z * in(set, 0, 2), w[3] = s.w * in(set, 0, 3);
        return true;
      });
    }
    {   // texcoord t1 ; mov r0, t1   (w is 1)
      Asm a(false, 1, 0);
      a.op(64, {Asm::dst(kRegTex, 1)});
      a.op(kOpMov, {Asm::dst(kRegTemp, 0), Asm::src(kRegTex, 1)});
      a.end();
      check(a, "texcoord", [&](unsigned set, const TexSet&, float* w) {
        w[0] = in(set, 3, 0), w[1] = in(set, 3, 1), w[2] = in(set, 3, 2), w[3] = 1.0f;
        return true;
      });
    }
    {   // texkill t0 ; mov r0, v0
      Asm a(false, 1, 1);
      a.op(65, {Asm::dst(kRegTex, 0)});
      a.op(kOpMov, {Asm::dst(kRegTemp, 0), Asm::src(kRegInput, 0)});
      a.end();
      check(a, "texkill", [&](unsigned set, const TexSet&, float* w) {
        for (int k = 0; k < 4; ++k) w[k] = in(set, 0, k);
        return !(in(set, 2, 0) < 0 || in(set, 2, 1) < 0 || in(set, 2, 2) < 0);
      });
    }
    {   // cnd r0, v0.a, v1, c0   (per component: v0.a > 0.5 ? v1 : c0)
      Asm a(false, 1, 2);
      a.op(kOpCnd, {Asm::dst(kRegTemp, 0), Asm::src(kRegInput, 0, 0xFF), Asm::src(kRegInput, 1), Asm::src(kRegConst, 0)});
      a.end();
      check(a, "cnd", [&](unsigned set, const TexSet&, float* w) {
        for (int k = 0; k < 4; ++k) w[k] = in(set, 0, 3) > 0.5f ? in(set, 1, k) : cb(set, 0, k);
        return true;
      });
    }
    {   // cmp r0, v0, v1, c0   (per component: v0 >= 0 ? v1 : c0)
      Asm a(false, 1, 2);
      a.op(kOpCmp, {Asm::dst(kRegTemp, 0), Asm::src(kRegInput, 0), Asm::src(kRegInput, 1), Asm::src(kRegConst, 0)});
      a.end();
      check(a, "cmp", [&](unsigned set, const TexSet&, float* w) {
        for (int k = 0; k < 4; ++k) w[k] = in(set, 0, k) >= 0 ? in(set, 1, k) : cb(set, 0, k);
        return true;
      });
    }
    {   // texld r0, t0 ; texcrd r1, t1 ; mul r0, r0, r1     (ps_1_4)
      Asm a(false, 1, 4);
      a.op(kOpTexLd, {Asm::dst(kRegTemp, 0), Asm::src(kRegTex, 0)});
      a.op(64, {Asm::dst(kRegTemp, 1), Asm::src(kRegTex, 1)});
      a.op(kOpMul, {Asm::dst(kRegTemp, 0), Asm::src(kRegTemp, 0), Asm::src(kRegTemp, 1)});
      a.end();
      check(a, "texld", [&](unsigned set, const TexSet& t, float* w) {
        const graphics::Vec4 s = t.sample(0, in(set, 2, 0), in(set, 2, 1));
        w[0] = s.x * in(set, 3, 0), w[1] = s.y * in(set, 3, 1), w[2] = s.z * in(set, 3, 2), w[3] = s.w;
        return true;
      });
    }
    {   // texld r1, r0_dw.xyw     (projective, from a temporary; stage 1)
      Asm a(false, 1, 4);
      a.op(kOpMov, {Asm::dst(kRegTemp, 0), Asm::src(kRegInput, 1)});
      a.op(kOpTexLd, {Asm::dst(kRegTemp, 1), Asm::src(kRegTemp, 0, 0xC4, 10)});   // (swizzle x, y, w, w; the _dw modifier)
      a.op(kOpMov, {Asm::dst(kRegTemp, 0), Asm::src(kRegTemp, 1)});
      a.end();
      check(a, "texld projective", [&](unsigned set, const TexSet& t, float* w) {
        const float inv = in(set, 1, 3) == 0 ? 0.0f : 1.0f / in(set, 1, 3);
        const graphics::Vec4 s = t.sample(1, in(set, 1, 0) * inv, in(set, 1, 1) * inv);
        w[0] = s.x, w[1] = s.y, w[2] = s.z, w[3] = s.w;
        return true;
      });
    }
    {   // mul_x2_sat r0, v0, v1 ; and source modifiers: mad r0.rgb... = v0_bx2 * (1 - v1) + c0_x2
      Asm a(false, 1, 1);
      a.op(kOpMul, {Asm::dst(kRegTemp, 0, 0xF, 1) | (1u << 24), Asm::src(kRegInput, 0), Asm::src(kRegInput, 1)});
      a.end();
      check(a, "mul_x2_sat", [&](unsigned set, const TexSet&, float* w) {
        for (int k = 0; k < 4; ++k) {
          const float v = in(set, 0, k) * in(set, 1, k) * 2.0f;
          w[k] = v > 0 ? (v < 1 ? v : 1.0f) : 0.0f;
        }
        return true;
      });
      Asm b(false, 1, 1);
      b.op(4, {Asm::dst(kRegTemp, 0), Asm::src(kRegInput, 0, kXyzw, 4), Asm::src(kRegInput, 1, kXyzw, 6), Asm::src(kRegConst, 0, kXyzw, 7)});
      b.end();
      check(b, "source modifiers", [&](unsigned set, const TexSet&, float* w) {
        for (int k = 0; k < 4; ++k) w[k] = 2.0f * (in(set, 0, k) - 0.5f) * (1.0f - in(set, 1, k)) + 2.0f * cb(set, 0, k);
        return true;
      });
    }
    {   // a real compiler's ps_1_4: lerp(c, d, k.a) * 0.5 + c * d
      const std::vector<uint8_t> blob = read_blob("ps1_lerp_1_4.sm1");
      UniquePointer<D3d9Program> p;
      CHECK(!blob.empty() && D3d9Program::compile(allocator, view_of(blob), &p) == status::kSuccess && p);
      if (p) {
        bool ok = true;
        for (unsigned set = 0; set < 4; ++set) {
          uint8_t constants[128];
          for (unsigned i = 0; i < 8; ++i)
            for (unsigned k = 0; k < 4; ++k) memcpy(constants + i * 16 + k * 4, &kConstants[set][i][k], 4);
          graphics::Vec4 varyings[4];
          for (unsigned i = 0; i < 4; ++i) varyings[i] = {kInputs[set][i][0], kInputs[set][i][1], kInputs[set][i][2], kInputs[set][i][3]};
          graphics::Vec4 c = {};
          ok = ok && p->run_fragment(varyings, constants, nullptr, &c);
          const float got[4] = {c.x, c.y, c.z, c.w};
          for (int k = 0; k < 4; ++k) {
            const float lerp = in(set, 0, k) + cb(set, 0, 3) * (in(set, 1, k) - in(set, 0, k));
            ok = ok && near_value(got[k], lerp * 0.5f + in(set, 0, k) * in(set, 1, k));
          }
        }
        CHECK(ok);
        List<uint32_t> tokens(allocator);
        CHECK(p->emit_svga_tokens(view_of(blob), &tokens) == status::kSuccess && tokens.count() * 4 == blob.size());
      }
    }
    // Refusals, and truncated / bit-flipped streams.
    {
      NtStatus st = 0;
      Asm refused(false, 1, 1);
      refused.op(67, {Asm::dst(kRegTex, 1), Asm::src(kRegTex, 0)});   // texbem
      refused.end();
      UniquePointer<D3d9Program> p;
      st = D3d9Program::compile(allocator, refused.bytes(), &p);
      CHECK(st == status::kNotSupported);
      Asm stage2(false, 1, 1);
      stage2.op(66, {Asm::dst(kRegTex, 2)});
      stage2.end();
      CHECK(D3d9Program::compile(allocator, stage2.bytes(), &p) == status::kNotSupported);
      Asm base(false, 1, 4);
      base.op(kOpTexLd, {Asm::dst(kRegTemp, 0), Asm::src(kRegTex, 0)});
      base.op(kOpCnd, {Asm::dst(kRegTemp, 1), Asm::src(kRegTemp, 0, 0xFF), Asm::src(kRegInput, 1), Asm::src(kRegConst, 0)});
      base.op(kOpMul, {Asm::dst(kRegTemp, 0, 0xF, 1), Asm::src(kRegTemp, 0), Asm::src(kRegTemp, 1)});
      base.end();
      unsigned compiled = 0, refusals = 0;
      uint32_t seed = 31337;
      for (size_t cut = 1; cut < base.t.size(); ++cut) {
        Asm t = base;
        t.t.resize(cut);
        D3d9Program::compile(allocator, t.bytes(), &p) == status::kSuccess ? ++compiled : ++refusals;
      }
      for (int i = 0; i < 4000; ++i) {
        Asm t = base;
        seed = seed * 1664525u + 1013904223u;
        t.t[(seed >> 8) % t.t.size()] ^= 1u << ((seed >> 3) % 32);
        if (D3d9Program::compile(allocator, t.bytes(), &p) == status::kSuccess) {
          ++compiled;
          String text(allocator);
          p->emit_tgsi(&text);
          graphics::Vec4 v[4] = {}, c;
          uint8_t constants[128] = {};
          p->run_fragment(v, constants, &textures, &c);
        } else {
          ++refusals;
        }
      }
      printf("  ps_1_x fuzz: %u compiled, %u refused\n", compiled, refusals);
      CHECK(compiled > 0 && refusals > 0);
    }
  }
  CHECK(allocator.live == 0);
}

// The device suite (tests/DxgkrnlDeviceSuite.hpp) on the software device itself and on a device that passes everything through: the baseline of what other devices are held to.
// --- The conformance scenes ---------------------------------------------------------------------------------------------------------------------------------------------

struct ConformanceLog {
  unsigned same = 0, differs = 0, refused = 0, broken = 0;
  bool print_each = false;
};

void test_half_floats() {
  // Every half survives the trip to float and back (NaNs stay NaNs), and conversions round to nearest even.
  for (uint32_t h = 0; h < 0x10000; ++h) {
    const float f = graphics::half_to_float(static_cast<uint16_t>(h));
    const uint16_t back = graphics::float_to_half(f);
    if ((h & 0x7C00) == 0x7C00 && (h & 0x3FF)) CHECK((back & 0x7C00) == 0x7C00 && (back & 0x3FF));
    else CHECK(back == h);
  }
  CHECK(graphics::float_to_half(1.0f) == 0x3C00 && graphics::float_to_half(-2.0f) == 0xC000 && graphics::float_to_half(65504.0f) == 0x7BFF);
  CHECK(graphics::float_to_half(1e9f) == 0x7C00 && graphics::float_to_half(-1e9f) == 0xFC00 && graphics::float_to_half(1e-9f) == 0);
  CHECK(graphics::float_to_half(1.0f + 1.0f / 2048.0f) == 0x3C00);          // (a tie goes to the even fraction)
  CHECK(graphics::float_to_half(1.0f + 3.0f / 2048.0f) == 0x3C02);
  CHECK(graphics::half_to_float(0x0001) == 5.9604645e-8f);                    // (the smallest subnormal)
}

// The count an occlusion query left in a result buffer (~0 when it cannot be read).
uint64_t query_count(Env& e, Handle results, uint32_t index) {
  LockResult r;
  if (e.kernel.lock(kPid, results, kLockReadOnly, 0, &r) != status::kSuccess) return ~uint64_t{0};
  ByteReader in(r.data);
  in.skip(uint64_t{index} * 8);
  const uint64_t v = in.le64();
  e.kernel.unlock(kPid, results);
  return in.ok() ? v : ~uint64_t{0};
}

void test_occlusion_queries() {
  Env e;
  const float identity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
  Handle rt = e.alloc(kTarget, Format::A8R8G8B8, 16, 16), rt2 = e.alloc(kTarget, Format::A8R8G8B8, 16, 16), ds = e.alloc(kAllocDepthStencil, Format::D32Float, 16, 16);
  Handle vb = e.buffer(kAllocVertexBuffer, 3 * 28), far_vb = e.buffer(kAllocVertexBuffer, 3 * 28);
  AllocationDesc rd;
  rd.flags = kAllocQueryResult, rd.size = 64;
  Handle results = 0;
  CHECK(e.kernel.create_allocation(kPid, e.dev, rd, &results) == status::kSuccess);
  // A query buffer is system memory only: nothing else can be combined with it, and it needs room for a count.
  AllocationDesc bad = rd;
  bad.flags = kAllocQueryResult | kAllocVertexBuffer;
  Handle none = 0;
  CHECK(e.kernel.create_allocation(kPid, e.dev, bad, &none) == status::kInvalidParameter);
  bad = rd, bad.size = 4;
  CHECK(e.kernel.create_allocation(kPid, e.dev, bad, &none) == status::kInvalidParameter);
  bad = rd, bad.size = 0;
  CHECK(e.kernel.create_allocation(kPid, e.dev, bad, &none) == status::kInvalidParameter);

  Handle vs = 0, fs = 0, pipe = 0;
  CHECK(e.kernel.create_shader(kPid, e.dev, {ShaderKind::ColoredVertex, graphics::ShaderStage::Vertex, {}}, &vs) == status::kSuccess);
  CHECK(e.kernel.create_shader(kPid, e.dev, {ShaderKind::ColoredFragment, graphics::ShaderStage::Fragment, {}}, &fs) == status::kSuccess);
  graphics::GraphicsPipelineDesc gd;
  graphics::describe_colored_pipeline(&gd);
  PipelineDesc pd;
  pd.vertex_shader = vs, pd.fragment_shader = fs, pd.vertex_stride = gd.vertex_stride, pd.attribute_count = 2, pd.varying_count = 1;
  pd.attributes[0] = gd.attributes[0], pd.attributes[1] = gd.attributes[1];
  pd.cull_mode = graphics::CullMode::None;
  pd.depth_test = pd.depth_write = true;
  CHECK(e.kernel.create_pipeline(kPid, e.dev, pd, &pipe) == status::kSuccess);
  CHECK(write_triangle(e, vb, 0, 1, 0));   // (depth 0.5, covers the whole 16 x 16 target)
  const float far_v[21] = {-1, -1, 0.9f, 0, 0, 1, 1, 3, -1, 0.9f, 0, 0, 1, 1, -1, 3, 0.9f, 0, 0, 1, 1};
  CHECK(e.write(far_vb, floats(far_v, 21)));

  // List: rt 0, rt2 1, ds 2, vb 3, far_vb 4, results 5.
  // The triangle covers all 256 pixels; the same one again behind nothing (equal depth passes) under a scissor of half the width counts 128; the farther one fails the depth test;
  // an empty query counts nothing.
  CommandWriter w(e.allocator);
  w.begin_pass(0, 2, kPassClearColor | kPassClearDepth, {0, 0, 0, 1}, 1, 0);
  w.bind_pipeline(pipe);
  w.push_constants(floats(identity, 16));
  w.begin_query(5, 0);
  w.bind_vertex_buffer(3);
  w.draw(3);
  w.end_query();
  w.begin_query(5, 8);
  w.bind_vertex_buffer(4);
  w.draw(3);
  w.end_query();
  w.begin_query(5, 16);
  w.end_query();
  w.set_scissor({0, 0, 8, 16});
  w.begin_query(5, 24);
  w.bind_vertex_buffer(3);
  w.draw(3);
  w.end_query();
  w.end_pass();
  CHECK(w.ok() && e.render(w, {rt, rt2, ds, vb, far_vb, results}) == status::kSuccess);
  CHECK(query_count(e, results, 0) == 256 && query_count(e, results, 1) == 0 && query_count(e, results, 2) == 0 && query_count(e, results, 3) == 128 && query_count(e, results, 4) == 0);

  // A copy in the middle of the stream runs what was recorded before it: the second pass's query is a new one and lands where it says.
  CommandWriter w2(e.allocator);
  w2.begin_pass(0, 2, kPassClearColor | kPassClearDepth, {0, 0, 0, 1}, 1, 0);
  w2.bind_pipeline(pipe);
  w2.push_constants(floats(identity, 16));
  w2.bind_vertex_buffer(3);
  w2.begin_query(5, 32);
  w2.draw(3);
  w2.end_query();
  w2.end_pass();
  w2.copy_region(1, 0, 0, 0, 0, 0, 16, 16);
  w2.begin_pass(1, 2, kPassClearColor | kPassClearDepth, {0, 0, 0, 1}, 1, 0);
  w2.bind_pipeline(pipe);
  w2.push_constants(floats(identity, 16));
  w2.bind_vertex_buffer(3);
  w2.set_scissor({0, 0, 16, 4});
  w2.begin_query(5, 40);
  w2.draw(3);
  w2.end_query();
  w2.end_pass();
  CHECK(w2.ok() && e.render(w2, {rt, rt2, ds, vb, far_vb, results}) == status::kSuccess);
  CHECK(query_count(e, results, 4) == 256 && query_count(e, results, 5) == 64 && query_count(e, results, 6) == 0);

  // What the kernel refuses (nothing runs, and the results stay as they were).
  auto refused = [&](auto build) {
    CommandWriter c(e.allocator);
    build(c);
    return c.ok() && e.render(c, {rt, rt2, ds, vb, far_vb, results}) == status::kInvalidParameter;
  };
  CHECK(refused([&](CommandWriter& c) { c.begin_query(5, 0); }));                                                  // outside a pass
  CHECK(refused([&](CommandWriter& c) { c.begin_pass(0, kNoAllocation, 0, {}, 1, 0); c.end_query(); c.end_pass(); }));       // never begun
  CHECK(refused([&](CommandWriter& c) { c.begin_pass(0, kNoAllocation, 0, {}, 1, 0); c.begin_query(5, 0); c.end_pass(); }));   // still open at the end of the pass
  CHECK(refused([&](CommandWriter& c) { c.begin_pass(0, kNoAllocation, 0, {}, 1, 0); c.begin_query(5, 0); c.begin_query(5, 8); c.end_query(); c.end_pass(); }));   // two at once
  CHECK(refused([&](CommandWriter& c) { c.begin_pass(0, kNoAllocation, 0, {}, 1, 0); c.begin_query(5, 4); c.end_query(); c.end_pass(); }));       // not a multiple of 8
  CHECK(refused([&](CommandWriter& c) { c.begin_pass(0, kNoAllocation, 0, {}, 1, 0); c.begin_query(5, 64); c.end_query(); c.end_pass(); }));      // past the end
  CHECK(refused([&](CommandWriter& c) { c.begin_pass(0, kNoAllocation, 0, {}, 1, 0); c.begin_query(5, 0xFFFFFFF8u); c.end_query(); c.end_pass(); }));
  CHECK(refused([&](CommandWriter& c) { c.begin_pass(0, kNoAllocation, 0, {}, 1, 0); c.begin_query(3, 0); c.end_query(); c.end_pass(); }));       // a vertex buffer
  CHECK(refused([&](CommandWriter& c) { c.begin_pass(0, kNoAllocation, 0, {}, 1, 0); c.begin_query(0, 0); c.end_query(); c.end_pass(); }));       // an image
  CHECK(refused([&](CommandWriter& c) { c.begin_pass(0, kNoAllocation, 0, {}, 1, 0); c.begin_query(9, 0); c.end_query(); c.end_pass(); }));       // no such allocation
  // The limit: 64 queries are fine, a 65th is not.
  {
    CommandWriter c(e.allocator);
    c.begin_pass(0, kNoAllocation, 0, {}, 1, 0);
    for (uint32_t i = 0; i < graphics::kMaxOcclusionQueries; ++i) c.begin_query(5, 56), c.end_query();
    c.end_pass();
    CHECK(c.ok() && e.render(c, {rt, rt2, ds, vb, far_vb, results}) == status::kSuccess);
    CommandWriter c2(e.allocator);
    c2.begin_pass(0, kNoAllocation, 0, {}, 1, 0);
    for (uint32_t i = 0; i <= graphics::kMaxOcclusionQueries; ++i) c2.begin_query(5, 56), c2.end_query();
    c2.end_pass();
    CHECK(c2.ok() && e.render(c2, {rt, rt2, ds, vb, far_vb, results}) == status::kInvalidParameter);
  }
  // (a query buffer cannot be bound as a vertex buffer, whatever its size)
  CommandWriter bound(e.allocator);
  bound.begin_pass(0, kNoAllocation, 0, {}, 1, 0);
  bound.bind_vertex_buffer(5);
  bound.end_pass();
  CHECK(e.render(bound, {rt, rt2, ds, vb, far_vb, results}) == status::kInvalidParameter);

  // A device without the feature refuses the buffer and the stream.
  FakeScreen screen(8, 6);
  graphics::SoftwareDevice software(e.allocator, &screen);
  SpyDevice plain(software);
  plain.feature_mask = 0;
  Kernel kernel(e.allocator);
  AdapterConfig c;
  c.display = &screen;
  kernel.add_adapter(plain, c);
  Handle adapter = 0, dev = 0;
  CHECK(kernel.open_process(kPid) == status::kSuccess && kernel.open_adapter_from_index(kPid, 0, &adapter) == status::kSuccess && kernel.create_device(kPid, adapter, &dev) == status::kSuccess);
  Handle r2 = 0;
  CHECK(kernel.create_allocation(kPid, dev, rd, &r2) == status::kNotSupported);
}

void test_texture_kinds() {
  Env e;
  auto create = [&](graphics::TextureKind kind, uint32_t layers, uint32_t w, uint32_t h, uint32_t flags = kAllocTexture, Format f = Format::A8R8G8B8, uint32_t mips = 1) {
    AllocationDesc d;
    d.flags = flags, d.format = f, d.width = w, d.height = h, d.kind = kind, d.layers = layers, d.mip_levels = mips;
    Handle out = 0;
    const NtStatus s = e.kernel.create_allocation(kPid, e.dev, d, &out);
    return s == status::kSuccess ? status::kSuccess : s;
  };
  using graphics::TextureKind;
  CHECK(create(TextureKind::Texture3D, 4, 4, 4) == status::kSuccess);
  CHECK(create(TextureKind::Texture2DArray, 256, 2, 2) == status::kSuccess);
  CHECK(create(TextureKind::TextureCube, 6, 4, 4) == status::kSuccess);
  CHECK(create(TextureKind::Texture3D, 4, 4, 4, kAllocTexture | kAllocShared, Format::X8B8G8R8) == status::kSuccess);
  // What cannot be: no layers, too many, a cube that is not six squares, a render target, mipmaps, a float or depth format, staging, layers on a plain texture.
  CHECK(create(TextureKind::Texture3D, 0, 4, 4) == status::kInvalidParameter);
  CHECK(create(TextureKind::Texture2DArray, 257, 4, 4) == status::kInvalidParameter);
  CHECK(create(TextureKind::TextureCube, 5, 4, 4) == status::kInvalidParameter);
  CHECK(create(TextureKind::TextureCube, 6, 4, 8) == status::kInvalidParameter);
  CHECK(create(TextureKind::Texture3D, 4, 4, 4, kAllocTexture | kAllocRenderTarget) == status::kInvalidParameter);
  CHECK(create(TextureKind::Texture3D, 4, 4, 4, kAllocTexture, Format::A8R8G8B8, 2) == status::kInvalidParameter);
  CHECK(create(TextureKind::Texture3D, 4, 4, 4, kAllocTexture, Format::A16B16G16R16F) == status::kInvalidParameter);
  CHECK(create(TextureKind::Texture3D, 4, 4, 4, kAllocStaging) == status::kInvalidParameter);
  CHECK(create(TextureKind::Texture2D, 2, 4, 4) == status::kInvalidParameter);
  // The lock shows all the layers, one after the other.
  AllocationDesc d;
  d.flags = kAllocTexture, d.format = Format::A8R8G8B8, d.width = d.height = 4, d.kind = TextureKind::Texture2DArray, d.layers = 3;
  Handle array = 0;
  CHECK(e.kernel.create_allocation(kPid, e.dev, d, &array) == status::kSuccess);
  LockResult lock;
  CHECK(e.kernel.lock(kPid, array, kLockReadOnly, 0, &lock) == status::kSuccess && lock.data.size() == 3 * 4 * 4 * 4 && lock.pitch == 16);
  e.kernel.unlock(kPid, array);

  // A texture of one kind where the shader declares another is refused, the right one draws.
  namespace b = conformance_blobs;
  auto pipeline = [&](const uint8_t* ps, uint32_t ps_size) {
    Handle vs = 0, fs = 0, pipe = 0;
    ShaderDesc sv, sp;
    sv.kind = sp.kind = ShaderKind::Dxbc;
    sv.stage = graphics::ShaderStage::Vertex, sp.stage = graphics::ShaderStage::Fragment;
    sv.code = ConstByteView(b::k_vs_vol, sizeof b::k_vs_vol), sp.code = ConstByteView(ps, ps_size);
    CHECK(e.kernel.create_shader(kPid, e.dev, sv, &vs) == status::kSuccess && e.kernel.create_shader(kPid, e.dev, sp, &fs) == status::kSuccess);
    PipelineDesc pd;
    pd.vertex_shader = vs, pd.fragment_shader = fs, pd.vertex_stride = 28, pd.attribute_count = 2, pd.varying_count = 3, pd.texture_count = 1;
    pd.attributes[0] = {0, graphics::VertexFormat::Float3, 0};
    pd.attributes[1] = {1, graphics::VertexFormat::Float4, 12};
    pd.cull_mode = graphics::CullMode::None;
    CHECK(e.kernel.create_pipeline(kPid, e.dev, pd, &pipe) == status::kSuccess);
    return pipe;
  };
  Handle volume_pipe = pipeline(b::k_ps_tex3d, sizeof b::k_ps_tex3d), plain_pipe = pipeline(b::k_ps_tex, sizeof b::k_ps_tex);
  Handle plain = e.alloc(kTexture, Format::A8R8G8B8, 4, 4), volume = 0, target = e.alloc(kTarget, Format::A8R8G8B8, 8, 8), vb = e.buffer(kAllocVertexBuffer, 3 * 28);
  d.kind = TextureKind::Texture3D, d.layers = 4;
  CHECK(e.kernel.create_allocation(kPid, e.dev, d, &volume) == status::kSuccess);
  CHECK(write_triangle(e, vb, 0.5f, 0.5f, 0.5f));
  const float identity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
  auto draw_with = [&](Handle pipe, Handle texture) {
    CommandWriter w(e.allocator);
    w.begin_pass(0, kNoAllocation, kPassClearColor, {0, 0, 0, 1}, 1, 0);
    w.bind_pipeline(pipe);
    w.bind_vertex_buffer(1);
    w.push_constants(floats(identity, 16));
    w.bind_texture(0, 2, graphics::SamplerDesc());
    w.draw(3);
    w.end_pass();
    return e.render(w, {target, vb, texture});
  };
  CHECK(draw_with(volume_pipe, volume) == status::kSuccess);
  CHECK(draw_with(plain_pipe, plain) == status::kSuccess);
  CHECK(draw_with(volume_pipe, plain) == status::kInvalidParameter);
  CHECK(draw_with(plain_pipe, volume) == status::kInvalidParameter);

  // A device without the feature refuses the allocations and the shaders that need it.
  FakeScreen screen(8, 6);
  graphics::SoftwareDevice software(e.allocator, &screen);
  SpyDevice limited(software);
  limited.feature_mask = 0;
  Kernel kernel(e.allocator);
  AdapterConfig c;
  c.display = &screen;
  kernel.add_adapter(limited, c);
  Handle adapter = 0, dev = 0, refused = 0;
  CHECK(kernel.open_process(kPid) == status::kSuccess && kernel.open_adapter_from_index(kPid, 0, &adapter) == status::kSuccess && kernel.create_device(kPid, adapter, &dev) == status::kSuccess);
  CHECK(kernel.create_allocation(kPid, dev, d, &refused) == status::kNotSupported);
  ShaderDesc sp;
  sp.kind = ShaderKind::Dxbc, sp.stage = graphics::ShaderStage::Fragment, sp.code = ConstByteView(b::k_ps_tex3d, sizeof b::k_ps_tex3d);
  CHECK(kernel.create_shader(kPid, dev, sp, &refused) == status::kNotSupported);
}

void test_independent_blend() {
  Env e;
  Handle pipe = 0;
  auto make = [&](Env& env, bool independent, uint32_t factor, Handle* out) {
    Handle vs = 0, fs = 0;
    CHECK(env.kernel.create_shader(kPid, env.dev, {ShaderKind::ColoredVertex, graphics::ShaderStage::Vertex, {}}, &vs) == status::kSuccess);
    CHECK(env.kernel.create_shader(kPid, env.dev, {ShaderKind::ColoredFragment, graphics::ShaderStage::Fragment, {}}, &fs) == status::kSuccess);
    graphics::GraphicsPipelineDesc gd;
    graphics::describe_colored_pipeline(&gd);
    PipelineDesc pd;
    pd.vertex_shader = vs, pd.fragment_shader = fs, pd.vertex_stride = gd.vertex_stride, pd.attribute_count = 2, pd.varying_count = 1;
    pd.attributes[0] = gd.attributes[0], pd.attributes[1] = gd.attributes[1];
    pd.independent_blend = independent;
    pd.target_blend[1].custom = true;
    pd.target_blend[1].src_color = static_cast<graphics::BlendFactor>(factor);
    return env.kernel.create_pipeline(kPid, env.dev, pd, out);
  };
  CHECK(make(e, true, 1, &pipe) == status::kSuccess && make(e, false, 1, &pipe) == status::kSuccess);
  CHECK(make(e, true, 11, &pipe) == status::kInvalidParameter);   // (a factor that does not exist, in a state of a target)
  // A device without the feature refuses it (but takes the same pipeline without independent blending).
  FakeScreen screen(8, 6);
  graphics::SoftwareDevice software(e.allocator, &screen);
  SpyDevice limited(software);
  limited.feature_mask = 0;
  Kernel kernel(e.allocator);
  AdapterConfig c;
  c.display = &screen;
  kernel.add_adapter(limited, c);
  Handle adapter = 0, dev = 0, vs = 0, fs = 0;
  CHECK(kernel.open_process(kPid) == status::kSuccess && kernel.open_adapter_from_index(kPid, 0, &adapter) == status::kSuccess && kernel.create_device(kPid, adapter, &dev) == status::kSuccess);
  CHECK(kernel.create_shader(kPid, dev, {ShaderKind::ColoredVertex, graphics::ShaderStage::Vertex, {}}, &vs) == status::kSuccess);
  CHECK(kernel.create_shader(kPid, dev, {ShaderKind::ColoredFragment, graphics::ShaderStage::Fragment, {}}, &fs) == status::kSuccess);
  graphics::GraphicsPipelineDesc gd;
  graphics::describe_colored_pipeline(&gd);
  PipelineDesc pd;
  pd.vertex_shader = vs, pd.fragment_shader = fs, pd.vertex_stride = gd.vertex_stride, pd.attribute_count = 2, pd.varying_count = 1;
  pd.attributes[0] = gd.attributes[0], pd.attributes[1] = gd.attributes[1];
  Handle out = 0;
  CHECK(kernel.create_pipeline(kPid, dev, pd, &out) == status::kSuccess);
  pd.independent_blend = true;
  CHECK(kernel.create_pipeline(kPid, dev, pd, &out) == status::kNotSupported);
}

// A compute shader written token by token (the HLSL compiler at hand has no ByteAddressBuffer methods): ld_raw, store_raw and an immediate atomic on a raw buffer.
void test_compute() {
  HostAllocator allocator;
  std::vector<uint32_t> t;
  auto put = [&](std::initializer_list<uint32_t> words) { for (uint32_t w : words) t.push_back(w); };
  const uint32_t kUav = 0x0011E000, kUavMaskX = 0x0011E012, kUavXXXX = 0x0011E006, kTempX = 0x00100012, kTempSel = 0x0010000A, kThreadX = 0x0002000A, kImm = 0x00004001;
  t.push_back(0x00050050);   // cs_5_0
  t.push_back(0);            // (the length, below)
  put({157u | (3u << 24), kUav, 0});                       // dcl_uav_raw u0
  put({104u | (2u << 24), 3});                             // dcl_temps 3
  put({155u | (4u << 24), 16, 1, 1});                      // dcl_thread_group 16, 1, 1
  put({95u | (2u << 24), 0x00020012});                     // dcl_input vThreadID.x
  put({41u | (6u << 24), kTempX, 0, kThreadX, kImm, 2});   // ishl r0.x, vThreadID.x, l(2)
  put({166u | (6u << 24), kUavMaskX, 0, kTempSel, 0, kThreadX});   // store_raw u0.x, r0.x, vThreadID.x
  put({165u | (7u << 24), kTempX, 1, kTempSel, 0, kUavXXXX, 0});   // ld_raw r1.x, r0.x, u0.xxxx
  put({30u | (7u << 24), kTempX, 1, kTempSel, 1, kImm, 100});      // iadd r1.x, r1.x, l(100)
  put({166u | (7u << 24), kUavMaskX, 0, kTempSel, 0, kTempSel, 1});   // store_raw u0.x, r0.x, r1.x
  put({180u | (9u << 24), kTempX, 2, kUav, 0, kImm, 64, kImm, 1});   // imm_atomic_iadd r2.x, u0, l(64), l(1)
  put({62u | (1u << 24)});                                 // ret
  t[1] = static_cast<uint32_t>(t.size());
  std::vector<uint8_t> blob = {'D', 'X', 'B', 'C'};
  auto put32 = [&](uint32_t v) { for (int i = 0; i < 4; ++i) blob.push_back(static_cast<uint8_t>(v >> (8 * i))); };
  blob.insert(blob.end(), 16, 0);
  const uint32_t body = static_cast<uint32_t>(t.size() * 4);
  put32(1), put32(36 + 8 + body), put32(1), put32(36);
  blob.push_back('S'), blob.push_back('H'), blob.push_back('E'), blob.push_back('X');
  put32(body);
  for (uint32_t w : t) put32(w);

  UniquePointer<D3d9Program> program;
  CHECK(D3d9Program::compile_dxbc(allocator, ConstByteView(blob.data(), blob.size()), &program) == status::kSuccess && program);
  CHECK(program->stage() == graphics::ShaderStage::Compute && program->thread_group(0) == 16 && program->thread_group(1) == 1);
  CHECK(program->storage_kind(0) == D3d9Program::BufferKind::Raw && program->storage_kind(1) == D3d9Program::BufferKind::None);
  CHECK((program->required_features() & graphics::kFeatureCompute) != 0);
  uint8_t storage[128] = {};
  graphics::ComputeContext context;
  context.buffers[0] = ByteView(storage, sizeof storage);
  for (uint32_t tid = 0; tid < 16; ++tid) {
    context.thread_id[0] = context.dispatch_id[0] = tid;
    program->run_compute(context, nullptr);
  }
  ByteReader in{ConstByteView(storage, sizeof storage)};
  for (uint32_t tid = 0; tid < 16; ++tid) CHECK(in.le32() == tid + 100);   // (each thread: its number, read back, plus 100)
  CHECK(in.le32() == 16);                                                  // (the sixteen atomic adds)
  // A buffer smaller than the shader assumes: the writes outside it are dropped, the reads give zero, nothing is touched past it.
  uint8_t small[40] = {};
  context.buffers[0] = ByteView(small, sizeof small);
  for (uint32_t tid = 0; tid < 16; ++tid) {
    context.dispatch_id[0] = tid;
    program->run_compute(context, nullptr);
  }
  ByteReader small_in{ConstByteView(small, sizeof small)};
  for (uint32_t tid = 0; tid < 10; ++tid) CHECK(small_in.le32() == tid + 100);
  // No buffer bound: the instructions do nothing.
  context.buffers[0] = ByteView();
  program->run_compute(context, nullptr);
  // The same text for the GPU: the buffer, the thread numbers and the atomic.
  String text(allocator);
  CHECK(program->emit_tgsi(&text) == status::kSuccess);
  const std::string tgsi(text.view().data(), text.size());
  CHECK(tgsi.find("COMP") == 0 && tgsi.find("PROPERTY CS_FIXED_BLOCK_WIDTH 16") != std::string::npos && tgsi.find("DCL BUFFER[0]") != std::string::npos && tgsi.find("ATOMUADD") != std::string::npos &&
        tgsi.find("STORE BUFFER[0]") != std::string::npos && tgsi.find("LOAD ") != std::string::npos && tgsi.find("OUT[") == std::string::npos);
  // Truncated and bit-flipped.
  uint32_t seed = 99;
  for (size_t cut = 0; cut < blob.size(); cut += 2) {
    UniquePointer<D3d9Program> p;
    D3d9Program::compile_dxbc(allocator, ConstByteView(blob.data(), cut), &p);
  }
  for (int i = 0; i < 4000; ++i) {
    std::vector<uint8_t> flipped = blob;
    seed = seed * 1664525u + 1013904223u;
    flipped[(seed >> 8) % flipped.size()] ^= static_cast<uint8_t>(1u << ((seed >> 3) % 8));
    UniquePointer<D3d9Program> p;
    if (D3d9Program::compile_dxbc(allocator, ConstByteView(flipped.data(), flipped.size()), &p) == status::kSuccess) {
      uint8_t mem[64] = {};
      graphics::ComputeContext c;
      c.buffers[0] = c.buffers[1] = ByteView(mem, sizeof mem);
      p->run_compute(c, nullptr);
      String ignored(allocator);
      p->emit_tgsi(&ignored);
    }
  }
}

// The kernel's side of compute: storage allocations, the stream's BindUav and Dispatch, and what it refuses.
void test_compute_kernel() {
  Env e;
  namespace b = conformance_blobs;
  Handle cs = 0, cs_pipe = 0;
  ShaderDesc sd;
  sd.kind = ShaderKind::Dxbc, sd.stage = graphics::ShaderStage::Compute, sd.code = ConstByteView(b::k_cs_fill, sizeof b::k_cs_fill);
  CHECK(e.kernel.create_shader(kPid, e.dev, sd, &cs) == status::kSuccess);
  PipelineDesc pd;
  pd.compute_shader = cs;
  CHECK(e.kernel.create_pipeline(kPid, e.dev, pd, &cs_pipe) == status::kSuccess);
  pd.compute_shader = 0x7777;
  Handle bogus = 0;
  CHECK(e.kernel.create_pipeline(kPid, e.dev, pd, &bogus) == status::kInvalidHandle);
  auto storage = [&](uint64_t size, uint32_t flags = kAllocUnorderedAccess) {
    AllocationDesc d;
    d.flags = flags, d.size = size;
    Handle out = 0;
    return e.kernel.create_allocation(kPid, e.dev, d, &out) == status::kSuccess ? out : Handle{0};
  };
  Handle picture = storage(4096), structured = storage(16384), counters = storage(16), plain = e.buffer(kAllocVertexBuffer, 4096);
  CHECK(picture && structured && counters);
  // Flags that do not go with it, and sizes.
  AllocationDesc d;
  d.flags = kAllocUnorderedAccess | kAllocTexture, d.size = 64;
  Handle none = 0;
  CHECK(e.kernel.create_allocation(kPid, e.dev, d, &none) == status::kInvalidParameter);
  d.flags = kAllocUnorderedAccess, d.size = 0;
  CHECK(e.kernel.create_allocation(kPid, e.dev, d, &none) == status::kInvalidParameter);

  const uint32_t constants[4] = {0x00102030u, 0, 0, 0};
  auto run = [&](auto build) {
    CommandWriter w(e.allocator);
    build(w);
    return w.ok() ? e.render(w, {picture, structured, counters, plain}) : status::kNoMemory;
  };
  auto bind_all = [&](CommandWriter& w) {
    w.bind_pipeline(cs_pipe);
    w.push_constants(ConstByteView(reinterpret_cast<const uint8_t*>(constants), 16));
    w.bind_uav(0, 0, 0, 4096), w.bind_uav(1, 1, 0, 16384), w.bind_uav(2, 2, 0, 16);
  };
  // The first dispatch: the CPU reads what the shader wrote through lock().
  CHECK(run([&](CommandWriter& w) { bind_all(w); w.dispatch(4, 8, 1); }) == status::kSuccess);
  CHECK(e.pixel(picture, 0, 0) == 0xFF102030u);
  LockResult lock;
  CHECK(e.kernel.lock(kPid, counters, kLockReadOnly, 0, &lock) == status::kSuccess);
  ByteReader counter_in(lock.data);
  CHECK(counter_in.le32() == 1024 && counter_in.le32() == 1024);
  e.kernel.unlock(kPid, counters);
  // A second dispatch adds to the counters (the shader and the buffers are kept).
  CHECK(run([&](CommandWriter& w) { bind_all(w); w.dispatch(4, 8, 1); w.dispatch(1, 1, 1); }) == status::kSuccess);
  CHECK(e.kernel.lock(kPid, counters, kLockReadOnly, 0, &lock) == status::kSuccess);
  ByteReader again(lock.data);
  CHECK(again.le32() == 1024 + 1024 + 32 && again.le32() == 1024 + 1024 + 32);
  e.kernel.unlock(kPid, counters);
  // An empty dispatch is fine.
  CHECK(run([&](CommandWriter& w) { bind_all(w); w.dispatch(0, 4, 4); }) == status::kSuccess);

  // Refused: a dispatch without a pipeline, with a slot unbound, inside a pass, too large; a UAV that is not one, misaligned, out of range, in a pass; a compute pipeline in a pass.
  CHECK(run([&](CommandWriter& w) { w.dispatch(1); }) == status::kInvalidParameter);
  CHECK(run([&](CommandWriter& w) { w.bind_pipeline(cs_pipe); w.bind_uav(0, 0, 0, 4096); w.dispatch(1); }) == status::kInvalidParameter);
  CHECK(run([&](CommandWriter& w) { bind_all(w); w.dispatch(65536); }) == status::kInvalidParameter);
  CHECK(run([&](CommandWriter& w) { bind_all(w); w.dispatch(65535, 65535, 65535); }) == status::kInvalidParameter);
  CHECK(run([&](CommandWriter& w) { bind_all(w); w.begin_pass(kNoAllocation, kNoAllocation, 0, {}, 1, 0); w.dispatch(1); }) == status::kInvalidParameter);
  CHECK(run([&](CommandWriter& w) { w.bind_pipeline(cs_pipe); w.bind_uav(0, 3, 0, 4096); }) == status::kInvalidParameter);   // (a vertex buffer)
  CHECK(run([&](CommandWriter& w) { w.bind_uav(0, 0, 2, 64); }) == status::kInvalidParameter);                                 // (offset not a multiple of 4)
  CHECK(run([&](CommandWriter& w) { w.bind_uav(0, 0, 0, 62); }) == status::kInvalidParameter);
  CHECK(run([&](CommandWriter& w) { w.bind_uav(0, 0, 4092, 8); }) == status::kInvalidParameter);                               // (past the end)
  CHECK(run([&](CommandWriter& w) { w.bind_uav(0, 0, 0xFFFFFFFCu, 8); }) == status::kInvalidParameter);
  CHECK(run([&](CommandWriter& w) { w.bind_uav(0, 0, 0, 0); }) == status::kInvalidParameter);
  CHECK(run([&](CommandWriter& w) { w.bind_uav(graphics::kMaxStorageBuffers, 0, 0, 64); }) == status::kInvalidParameter);
  CHECK(run([&](CommandWriter& w) { w.begin_pass(0, kNoAllocation, 0, {}, 1, 0); w.bind_uav(0, 0, 0, 64); w.end_pass(); }) == status::kInvalidParameter);

  // A device without the feature: no storage allocations, no compute shaders.
  FakeScreen screen(8, 6);
  graphics::SoftwareDevice software(e.allocator, &screen);
  SpyDevice limited(software);
  limited.feature_mask = 0;
  Kernel kernel(e.allocator);
  AdapterConfig c;
  c.display = &screen;
  kernel.add_adapter(limited, c);
  Handle adapter = 0, dev = 0, refused = 0;
  CHECK(kernel.open_process(kPid) == status::kSuccess && kernel.open_adapter_from_index(kPid, 0, &adapter) == status::kSuccess && kernel.create_device(kPid, adapter, &dev) == status::kSuccess);
  d.flags = kAllocUnorderedAccess, d.size = 64;
  CHECK(kernel.create_allocation(kPid, dev, d, &refused) == status::kNotSupported);
  CHECK(kernel.create_shader(kPid, dev, sd, &refused) == status::kNotSupported);
}

void test_multisample() {
  Env e;
  auto create = [&](uint32_t flags, Format f, uint32_t samples, uint32_t w = 32, uint32_t h = 32, uint32_t mips = 1, Handle* out = nullptr) {
    AllocationDesc d;
    d.flags = flags, d.format = f, d.width = w, d.height = h, d.samples = samples, d.mip_levels = mips;
    Handle handle = 0;
    const NtStatus st = e.kernel.create_allocation(kPid, e.dev, d, &handle);
    if (out) *out = handle;
    return st;
  };
  CHECK(create(kAllocRenderTarget, Format::A8R8G8B8, 2) == status::kSuccess && create(kAllocRenderTarget, Format::A8R8G8B8, 4) == status::kSuccess);
  CHECK(create(kAllocRenderTarget, Format::A8R8G8B8, 8) == status::kSuccess && create(kAllocDepthStencil, Format::D32Float, 4) == status::kSuccess);
  CHECK(create(kAllocRenderTarget, Format::A8R8G8B8, 3) == status::kNotSupported && create(kAllocRenderTarget, Format::A8R8G8B8, 16) == status::kNotSupported);
  CHECK(create(kAllocRenderTarget, Format::A8R8G8B8, 64) == status::kNotSupported);
  // Render targets (that may be read by a shader, as a multisampled texture) and depth buffers only, one level, 8-bit colour.
  CHECK(create(kAllocRenderTarget | kAllocTexture, Format::A8R8G8B8, 4) == status::kSuccess && create(kAllocDepthStencil | kAllocTexture, Format::D32Float, 4) == status::kInvalidParameter);
  CHECK(create(kAllocTexture, Format::A8R8G8B8, 4) == status::kInvalidParameter);
  CHECK(create(kAllocRenderTarget | kAllocPresent, Format::A8R8G8B8, 4) == status::kInvalidParameter);
  CHECK(create(kAllocStaging, Format::A8R8G8B8, 4) == status::kInvalidParameter);
  CHECK(create(kAllocRenderTarget, Format::A8R8G8B8, 4, 32, 32, 2) == status::kInvalidParameter);
  CHECK(create(kAllocRenderTarget, Format::A16B16G16R16F, 4) == status::kInvalidParameter);
  // (the images below)
  Handle ms = 0, ms2 = 0, depth = 0, depth2 = 0, plain = 0, other_size = 0, texture = 0;
  CHECK(create(kAllocRenderTarget, Format::A8R8G8B8, 4, 32, 32, 1, &ms) == status::kSuccess && create(kAllocRenderTarget, Format::A8R8G8B8, 2, 32, 32, 1, &ms2) == status::kSuccess);
  CHECK(create(kAllocDepthStencil, Format::D32Float, 4, 32, 32, 1, &depth) == status::kSuccess && create(kAllocDepthStencil, Format::D32Float, 2, 32, 32, 1, &depth2) == status::kSuccess);
  CHECK(create(kAllocRenderTarget, Format::A8R8G8B8, 1, 32, 32, 1, &plain) == status::kSuccess && create(kAllocRenderTarget, Format::A8R8G8B8, 1, 16, 16, 1, &other_size) == status::kSuccess);
  CHECK(create(kAllocTexture | kAllocRenderTarget, Format::A8R8G8B8, 1, 32, 32, 1, &texture) == status::kSuccess);
  LockResult lock;
  CHECK(e.kernel.lock(kPid, ms, kLockReadOnly, 0, &lock) == status::kNotSupported);   // (no pixels of the CPU's)

  // A pipeline of the builtin shaders, with coverage per sample or without.
  auto pipeline = [&](bool multisample) {
    Handle vs = 0, fs = 0, pipe = 0;
    CHECK(e.kernel.create_shader(kPid, e.dev, {ShaderKind::ColoredVertex, graphics::ShaderStage::Vertex, {}}, &vs) == status::kSuccess);
    CHECK(e.kernel.create_shader(kPid, e.dev, {ShaderKind::ColoredFragment, graphics::ShaderStage::Fragment, {}}, &fs) == status::kSuccess);
    graphics::GraphicsPipelineDesc gd;
    graphics::describe_colored_pipeline(&gd);
    PipelineDesc pd;
    pd.vertex_shader = vs, pd.fragment_shader = fs, pd.vertex_stride = gd.vertex_stride, pd.attribute_count = 2, pd.varying_count = 1;
    pd.attributes[0] = gd.attributes[0], pd.attributes[1] = gd.attributes[1];
    pd.cull_mode = graphics::CullMode::None;
    pd.multisample = multisample;
    CHECK(e.kernel.create_pipeline(kPid, e.dev, pd, &pipe) == status::kSuccess);
    return pipe;
  };
  const Handle covering = pipeline(true), aliased = pipeline(false);
  // A red rectangle from x = 4.3 to 20.6 over the whole height, in NDC of a 32 x 32 target.
  const float x0 = 4.3f / 16 - 1, x1 = 20.6f / 16 - 1;
  const float v[42] = {x0, -1, 0.5f, 1, 0, 0, 1, x1, -1, 0.5f, 1, 0, 0, 1, x0, 1, 0.5f, 1, 0, 0, 1,
                       x0, 1, 0.5f, 1, 0, 0, 1, x1, -1, 0.5f, 1, 0, 0, 1, x1, 1, 0.5f, 1, 0, 0, 1};
  Handle vb = e.buffer(kAllocVertexBuffer, sizeof v);
  CHECK(e.write(vb, floats(v, 42)));
  const float identity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
  auto draw = [&](Handle pipe, Handle target, Handle depth_buffer, Handle resolve_to) {
    CommandWriter w(e.allocator);
    w.begin_pass(0, depth_buffer ? 1u : kNoAllocation, kPassClearColor | (depth_buffer ? kPassClearDepth : 0u), {0, 0, 0, 1}, 1, 0);
    w.bind_pipeline(pipe);
    w.bind_vertex_buffer(3);
    w.push_constants(floats(identity, 16));
    w.draw(6);
    w.end_pass();
    if (resolve_to) w.resolve_region(2, 0);
    return e.render(w, {target, depth_buffer ? depth_buffer : target, resolve_to ? resolve_to : target, vb});
  };
  auto red = [&](Handle image, uint32_t x, uint32_t y) { return (e.pixel(image, x, y) >> 16) & 0xFF; };
  // 4 samples: x offsets 1/8, 3/8, 5/8, 7/8 of a pixel, so pixel 4 (edge at 0.3) has three of its four samples inside, pixel 20 (edge at 0.6) two.
  CHECK(draw(covering, ms, 0, plain) == status::kSuccess);
  CHECK(red(plain, 10, 10) == 255 && red(plain, 3, 10) == 0 && red(plain, 22, 10) == 0 && red(plain, 4, 10) == 191 && red(plain, 20, 10) == 128 && e.pixel(plain, 10, 10) >> 24 == 0xFF);
  // 2 samples (x offsets 1/4, 3/4): pixel 4 has one of two.
  CHECK(draw(covering, ms2, 0, plain) == status::kSuccess);
  CHECK(red(plain, 4, 10) == 128 && red(plain, 20, 10) == 128 && red(plain, 10, 10) == 255);
  // Without multisample the pixel is all or nothing by its centre: pixel 4's centre (4.5) is inside, pixel 20's (20.5) is not (the edge at 20.6).
  CHECK(draw(aliased, ms, 0, plain) == status::kSuccess);
  CHECK(red(plain, 4, 10) == 255 && red(plain, 20, 10) == 255 && red(plain, 21, 10) == 0);
  // The same into a texture; and with a depth buffer of the same count.
  CHECK(draw(covering, ms, depth, texture) == status::kSuccess && red(texture, 4, 10) == 191 && red(texture, 10, 10) == 255);

  // Refused: counts that differ in a pass; resolving what is not multisampled, into a multisampled image, between sizes; copies; a resolve inside a pass.
  CHECK(draw(covering, ms, depth2, plain) == status::kInvalidParameter);
  auto stream = [&](auto build) {
    CommandWriter w(e.allocator);
    build(w);
    return e.render(w, {ms, plain, other_size, ms2, texture});
  };
  CHECK(stream([&](CommandWriter& w) { w.resolve_region(1, 0); }) == status::kSuccess);
  CHECK(stream([&](CommandWriter& w) { w.resolve_region(1, 1); }) == status::kInvalidParameter);   // (not multisampled)
  CHECK(stream([&](CommandWriter& w) { w.resolve_region(0, 3); }) == status::kInvalidParameter);   // (into a multisampled image)
  CHECK(stream([&](CommandWriter& w) { w.resolve_region(2, 0); }) == status::kInvalidParameter);   // (another size)
  CHECK(stream([&](CommandWriter& w) { w.resolve_region(0, 0); }) == status::kInvalidParameter);
  CHECK(stream([&](CommandWriter& w) { w.copy_region(1, 0, 0, 0, 0, 0, 8, 8); }) == status::kInvalidParameter);
  CHECK(stream([&](CommandWriter& w) { w.copy_region(0, 0, 0, 1, 0, 0, 8, 8); }) == status::kInvalidParameter);
  CHECK(stream([&](CommandWriter& w) { w.begin_pass(0, kNoAllocation, 0, {}, 1, 0); w.resolve_region(1, 0); w.end_pass(); }) == status::kInvalidParameter);

  // A device without the feature refuses the images and the pipeline flag.
  FakeScreen screen(8, 6);
  graphics::SoftwareDevice software(e.allocator, &screen);
  SpyDevice limited(software);
  limited.feature_mask = 0;
  Kernel kernel(e.allocator);
  AdapterConfig c;
  c.display = &screen;
  kernel.add_adapter(limited, c);
  Handle adapter = 0, dev = 0, refused = 0;
  CHECK(kernel.open_process(kPid) == status::kSuccess && kernel.open_adapter_from_index(kPid, 0, &adapter) == status::kSuccess && kernel.create_device(kPid, adapter, &dev) == status::kSuccess);
  AllocationDesc d;
  d.flags = kAllocRenderTarget, d.format = Format::A8R8G8B8, d.width = d.height = 8, d.samples = 4;
  CHECK(kernel.create_allocation(kPid, dev, d, &refused) == status::kNotSupported);
}

void test_geometry_kernel() {
  Env e;
  namespace b = conformance_blobs;
  Handle vs = 0, gs = 0, ps = 0;
  ShaderDesc sd;
  sd.kind = ShaderKind::Dxbc;
  sd.stage = graphics::ShaderStage::Vertex, sd.code = ConstByteView(b::k_vs, sizeof b::k_vs);
  CHECK(e.kernel.create_shader(kPid, e.dev, sd, &vs) == status::kSuccess);
  sd.stage = graphics::ShaderStage::Geometry, sd.code = ConstByteView(b::k_gs_copy, sizeof b::k_gs_copy);
  CHECK(e.kernel.create_shader(kPid, e.dev, sd, &gs) == status::kSuccess);
  sd.stage = graphics::ShaderStage::Fragment, sd.code = ConstByteView(b::k_ps_color, sizeof b::k_ps_color);
  CHECK(e.kernel.create_shader(kPid, e.dev, sd, &ps) == status::kSuccess);
  auto pipeline = [&](Handle geometry, Handle* out) {
    PipelineDesc pd;
    pd.vertex_shader = vs, pd.fragment_shader = ps, pd.geometry_shader = geometry, pd.vertex_stride = 28, pd.attribute_count = 2, pd.varying_count = 3;
    pd.attributes[0] = {0, graphics::VertexFormat::Float3, 0};
    pd.attributes[1] = {1, graphics::VertexFormat::Float4, 12};
    pd.cull_mode = graphics::CullMode::None;
    return e.kernel.create_pipeline(kPid, e.dev, pd, out);
  };
  Handle with_gs = 0, without = 0, wrong = 0;
  CHECK(pipeline(gs, &with_gs) == status::kSuccess && pipeline(0, &without) == status::kSuccess);
  CHECK(pipeline(vs, &wrong) == status::kInvalidParameter);   // (not a geometry shader)
  CHECK(pipeline(0x7777, &wrong) == status::kInvalidHandle);
  Handle target = e.alloc(kTarget, Format::A8R8G8B8, 32, 32), vb = e.buffer(kAllocVertexBuffer, 4 * 28);
  const float v[28] = {-0.9f, -0.9f, 0.5f, 1, 0, 0, 1, 0.9f, -0.9f, 0.5f, 0, 1, 0, 1, 0, 0.9f, 0.5f, 0, 0, 1, 1, 0.5f, 0.9f, 0.5f, 1, 1, 0, 1};
  CHECK(e.write(vb, floats(v, 28)));
  const float identity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
  auto draw = [&](Handle pipe, Topology topology, uint32_t count) {
    CommandWriter w(e.allocator);
    w.begin_pass(0, kNoAllocation, kPassClearColor, {0, 0, 0, 1}, 1, 0);
    w.bind_pipeline(pipe);
    w.bind_vertex_buffer(1);
    w.push_constants(floats(identity, 16));
    w.set_topology(topology);
    w.draw(count);
    w.end_pass();
    return e.render(w, {target, vb});
  };
  // It takes triangles: a list or a strip (which the kernel makes into a list); lines and points are refused (and a pipeline without it takes anything).
  CHECK(draw(with_gs, Topology::TriangleList, 3) == status::kSuccess);
  CHECK(e.pixel(target, 4, 28) != 0xFF000000u);
  CHECK(draw(with_gs, Topology::TriangleStrip, 4) == status::kSuccess);
  CHECK(draw(with_gs, Topology::LineList, 2) == status::kInvalidParameter);
  CHECK(draw(with_gs, Topology::PointList, 2) == status::kInvalidParameter);
  CHECK(draw(without, Topology::LineList, 2) == status::kSuccess);
  // A device without the feature refuses the shader.
  FakeScreen screen(8, 6);
  graphics::SoftwareDevice software(e.allocator, &screen);
  SpyDevice limited(software);
  limited.feature_mask = 0;
  Kernel kernel(e.allocator);
  AdapterConfig c;
  c.display = &screen;
  kernel.add_adapter(limited, c);
  Handle adapter = 0, dev = 0, refused = 0;
  CHECK(kernel.open_process(kPid) == status::kSuccess && kernel.open_adapter_from_index(kPid, 0, &adapter) == status::kSuccess && kernel.create_device(kPid, adapter, &dev) == status::kSuccess);
  sd.stage = graphics::ShaderStage::Geometry, sd.code = ConstByteView(b::k_gs_copy, sizeof b::k_gs_copy);
  CHECK(kernel.create_shader(kPid, dev, sd, &refused) == status::kNotSupported);
}

void test_formats() {
  Env e;
  auto create = [&](uint32_t flags, Format f, uint32_t w = 8, uint32_t h = 8, uint32_t mips = 1, uint32_t samples = 1, Handle* out = nullptr) {
    AllocationDesc d;
    d.flags = flags, d.format = f, d.width = w, d.height = h, d.mip_levels = mips, d.samples = samples;
    Handle handle = 0;
    const NtStatus st = e.kernel.create_allocation(kPid, e.dev, d, &handle);
    if (out) *out = handle;
    return st;
  };
  const Format all[] = {Format::A8R8G8B8Srgb, Format::A8B8G8R8Srgb, Format::A16B16G16R16, Format::A32B32G32R32Uint};
  for (Format f : all) CHECK(create(kAllocRenderTarget | kAllocTexture, f) == status::kSuccess && create(kAllocStaging, f) == status::kSuccess);
  // Not presentable; the wide ones have one level and cannot be multisampled; an sRGB image may.
  for (Format f : all) CHECK(create(kAllocRenderTarget | kAllocPresent, f) == status::kInvalidParameter);
  CHECK(create(kAllocTexture, Format::A16B16G16R16, 8, 8, 2) == status::kInvalidParameter && create(kAllocTexture, Format::A32B32G32R32Uint, 8, 8, 2) == status::kInvalidParameter);
  CHECK(create(kAllocTexture, Format::A8R8G8B8Srgb, 8, 8, 2) == status::kSuccess);
  CHECK(create(kAllocRenderTarget, Format::A16B16G16R16, 8, 8, 1, 4) == status::kInvalidParameter && create(kAllocRenderTarget, Format::A32B32G32R32Uint, 8, 8, 1, 4) == status::kInvalidParameter);
  // A device without the features refuses them (staging memory is only memory, and is fine).
  {
    FakeScreen screen(8, 6);
    graphics::SoftwareDevice software(e.allocator, &screen);
    SpyDevice limited(software);
    limited.feature_mask = 0;
    Kernel kernel(e.allocator);
    AdapterConfig c;
    c.display = &screen;
    kernel.add_adapter(limited, c);
    Handle adapter = 0, dev = 0, refused = 0;
    CHECK(kernel.open_process(kPid) == status::kSuccess && kernel.open_adapter_from_index(kPid, 0, &adapter) == status::kSuccess && kernel.create_device(kPid, adapter, &dev) == status::kSuccess);
    for (Format f : all) {
      AllocationDesc d;
      d.flags = kAllocRenderTarget, d.format = f, d.width = d.height = 8;
      CHECK(kernel.create_allocation(kPid, dev, d, &refused) == status::kNotSupported);
      d.flags = kAllocStaging;
      CHECK(kernel.create_allocation(kPid, dev, d, &refused) == status::kSuccess);
    }
  }
  // What a clear leaves, read back through the memory manager: 16-bit normalised (0.5, 0.25, 1, 0) is 8000 4000 FFFF 0000; the integers are the bits given; sRGB takes linear 0.5 as BC
  // (and 0.5 alpha, which is linear, as 80).
  Handle unorm = 0, uint = 0, srgb = 0;
  CHECK(create(kAllocRenderTarget, Format::A16B16G16R16, 4, 4, 1, 1, &unorm) == status::kSuccess && create(kAllocRenderTarget, Format::A32B32G32R32Uint, 4, 4, 1, 1, &uint) == status::kSuccess &&
        create(kAllocRenderTarget, Format::A8R8G8B8Srgb, 4, 4, 1, 1, &srgb) == status::kSuccess);
  auto clear = [&](Handle target, graphics::Vec4 colour) {
    CommandWriter w(e.allocator);
    w.begin_pass(0, kNoAllocation, kPassClearColor, colour, 1, 0);
    w.end_pass();
    return e.render(w, {target});
  };
  auto word = [&](Handle image, uint32_t index) {
    LockResult r;
    if (e.kernel.lock(kPid, image, kLockReadOnly, 0, &r) != status::kSuccess) return 0xDEADDEADu;
    ByteReader in(r.data);
    in.skip(uint64_t{index} * 4);
    const uint32_t v = in.le32();
    e.kernel.unlock(kPid, image);
    return v;
  };
  CHECK(clear(unorm, {0.5f, 0.25f, 1.0f, 0.0f}) == status::kSuccess && word(unorm, 0) == 0x40008000u && word(unorm, 1) == 0x0000FFFFu);
  CHECK(clear(uint, {__builtin_bit_cast(float, 7u), __builtin_bit_cast(float, 0xFFFFFFFFu), __builtin_bit_cast(float, 0x7FC00001u), __builtin_bit_cast(float, 1u)}) == status::kSuccess);
  CHECK(word(uint, 0) == 7 && word(uint, 1) == 0xFFFFFFFFu && word(uint, 2) == 0x7FC00001u && word(uint, 3) == 1 && word(uint, 4 * 15) == 7 && word(uint, 4 * 15 + 1) == 0xFFFFFFFFu);
  CHECK(clear(srgb, {0.5f, 0.5f, 0.5f, 0.5f}) == status::kSuccess && word(srgb, 0) == 0x80BCBCBCu);
  // The clear of an ordinary target still refuses what is not a colour.
  Handle plain = 0;
  CHECK(create(kAllocRenderTarget, Format::A8R8G8B8, 4, 4, 1, 1, &plain) == status::kSuccess && clear(plain, {__builtin_bit_cast(float, 0x7FC00001u), 0, 0, 0}) == status::kInvalidParameter);
  // Integer targets take no blending.
  {
    Handle vs = 0, fs = 0, pipe = 0;
    CHECK(e.kernel.create_shader(kPid, e.dev, {ShaderKind::ColoredVertex, graphics::ShaderStage::Vertex, {}}, &vs) == status::kSuccess);
    CHECK(e.kernel.create_shader(kPid, e.dev, {ShaderKind::ColoredFragment, graphics::ShaderStage::Fragment, {}}, &fs) == status::kSuccess);
    graphics::GraphicsPipelineDesc gd;
    graphics::describe_colored_pipeline(&gd);
    PipelineDesc pd;
    pd.vertex_shader = vs, pd.fragment_shader = fs, pd.vertex_stride = gd.vertex_stride, pd.attribute_count = 2, pd.varying_count = 1;
    pd.attributes[0] = gd.attributes[0], pd.attributes[1] = gd.attributes[1];
    pd.cull_mode = graphics::CullMode::None;
    pd.alpha_blend = true;
    CHECK(e.kernel.create_pipeline(kPid, e.dev, pd, &pipe) == status::kSuccess);
    Handle vb = e.buffer(kAllocVertexBuffer, 3 * 28);
    CHECK(write_triangle(e, vb, 1, 0, 0));
    const float identity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    auto draw = [&](Handle target) {
      CommandWriter w(e.allocator);
      w.begin_pass(0, kNoAllocation, kPassClearColor, {0, 0, 0, 1}, 1, 0);
      w.bind_pipeline(pipe);
      w.bind_vertex_buffer(1);
      w.push_constants(floats(identity, 16));
      w.draw(3);
      w.end_pass();
      return e.render(w, {target, vb});
    };
    CHECK(draw(uint) == status::kInvalidParameter && draw(unorm) == status::kSuccess && draw(srgb) == status::kSuccess);
    // (a red triangle over black with source-over: opaque, so the encoded red is FF)
    CHECK(word(srgb, 0) == 0xFFFF0000u);
  }
  // A multisampled sRGB image resolves as linear values: three of four samples of red average to 0.75, encoded E1 (an ordinary image would give BF).
  {
    Handle ms = 0, resolved = 0, vs = 0, fs = 0, pipe = 0;
    CHECK(create(kAllocRenderTarget, Format::A8R8G8B8Srgb, 16, 16, 1, 4, &ms) == status::kSuccess && create(kAllocRenderTarget, Format::A8R8G8B8Srgb, 16, 16, 1, 1, &resolved) == status::kSuccess);
    CHECK(e.kernel.create_shader(kPid, e.dev, {ShaderKind::ColoredVertex, graphics::ShaderStage::Vertex, {}}, &vs) == status::kSuccess);
    CHECK(e.kernel.create_shader(kPid, e.dev, {ShaderKind::ColoredFragment, graphics::ShaderStage::Fragment, {}}, &fs) == status::kSuccess);
    graphics::GraphicsPipelineDesc gd;
    graphics::describe_colored_pipeline(&gd);
    PipelineDesc pd;
    pd.vertex_shader = vs, pd.fragment_shader = fs, pd.vertex_stride = gd.vertex_stride, pd.attribute_count = 2, pd.varying_count = 1;
    pd.attributes[0] = gd.attributes[0], pd.attributes[1] = gd.attributes[1];
    pd.cull_mode = graphics::CullMode::None;
    pd.multisample = true;
    CHECK(e.kernel.create_pipeline(kPid, e.dev, pd, &pipe) == status::kSuccess);
    const float x0 = 4.3f / 8 - 1, x1 = 20.0f / 8 - 1;
    const float v[42] = {x0, -1, 0.5f, 1, 0, 0, 1, x1, -1, 0.5f, 1, 0, 0, 1, x0, 1, 0.5f, 1, 0, 0, 1, x0, 1, 0.5f, 1, 0, 0, 1, x1, -1, 0.5f, 1, 0, 0, 1, x1, 1, 0.5f, 1, 0, 0, 1};
    Handle vb = e.buffer(kAllocVertexBuffer, sizeof v);
    CHECK(e.write(vb, floats(v, 42)));
    const float identity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    CommandWriter w(e.allocator);
    w.begin_pass(0, kNoAllocation, kPassClearColor, {0, 0, 0, 1}, 1, 0);
    w.bind_pipeline(pipe);
    w.bind_vertex_buffer(2);
    w.push_constants(floats(identity, 16));
    w.draw(6);
    w.end_pass();
    w.resolve_region(1, 0);
    CHECK(e.render(w, {ms, resolved, vb}) == status::kSuccess);
    CHECK(e.pixel(resolved, 4, 8) == 0xFFE10000u && e.pixel(resolved, 8, 8) == 0xFFFF0000u && e.pixel(resolved, 3, 8) == 0xFF000000u);
  }
}

void test_tessellation_kernel() {
  Env e;
  namespace b = conformance_blobs;
  auto shader = [&](graphics::ShaderStage stage, const uint8_t* code, uint32_t size, Handle* out, Kernel* kernel = nullptr, Handle device = 0) {
    ShaderDesc sd;
    sd.kind = ShaderKind::Dxbc, sd.stage = stage, sd.code = ConstByteView(code, size);
    return (kernel ? kernel->create_shader(kPid, device, sd, out) : e.kernel.create_shader(kPid, e.dev, sd, out));
  };
  Handle vs = 0, hs = 0, ds = 0, ps = 0, gs = 0;
  CHECK(shader(graphics::ShaderStage::Vertex, b::k_vs, sizeof b::k_vs, &vs) == status::kSuccess && shader(graphics::ShaderStage::Hull, b::k_hs_tri, sizeof b::k_hs_tri, &hs) == status::kSuccess);
  CHECK(shader(graphics::ShaderStage::Domain, b::k_ds_dent, sizeof b::k_ds_dent, &ds) == status::kSuccess && shader(graphics::ShaderStage::Fragment, b::k_ps_color, sizeof b::k_ps_color, &ps) == status::kSuccess);
  CHECK(shader(graphics::ShaderStage::Geometry, b::k_gs_copy, sizeof b::k_gs_copy, &gs) == status::kSuccess);
  // (the stage a DXBC shader says it is, not the one asked for, is its stage: this is a hull shader too)
  Handle also_hull = 0;
  CHECK(shader(graphics::ShaderStage::Vertex, b::k_hs_tri, sizeof b::k_hs_tri, &also_hull) == status::kSuccess);
  auto pipeline = [&](Handle hull, Handle domain, Handle geometry, Handle* out) {
    PipelineDesc pd;
    pd.vertex_shader = vs, pd.fragment_shader = ps, pd.hull_shader = hull, pd.domain_shader = domain, pd.geometry_shader = geometry, pd.vertex_stride = 28, pd.attribute_count = 2, pd.varying_count = 3;
    pd.attributes[0] = {0, graphics::VertexFormat::Float3, 0};
    pd.attributes[1] = {1, graphics::VertexFormat::Float4, 12};
    pd.cull_mode = graphics::CullMode::None;
    return e.kernel.create_pipeline(kPid, e.dev, pd, out);
  };
  Handle tess = 0, plain = 0, bad = 0;
  CHECK(pipeline(hs, ds, 0, &tess) == status::kSuccess && pipeline(0, 0, 0, &plain) == status::kSuccess);
  CHECK(pipeline(hs, 0, 0, &bad) == status::kInvalidParameter && pipeline(0, ds, 0, &bad) == status::kInvalidParameter);   // (both or neither)
  CHECK(pipeline(ds, hs, 0, &bad) == status::kInvalidParameter);                                                         // (the wrong way round)
  CHECK(pipeline(vs, ds, 0, &bad) == status::kInvalidParameter && pipeline(also_hull, ds, 0, &bad) == status::kSuccess);
  CHECK(pipeline(hs, ds, gs, &bad) == status::kNotSupported);                                                           // (no geometry shader after the tessellator)
  Handle target = e.alloc(kTarget, Format::A8R8G8B8, 32, 32), vb = e.buffer(kAllocVertexBuffer, 6 * 28);
  const float v[42] = {-0.9f, -0.9f, 0.5f, 1, 0, 0, 1, 0.9f, -0.9f, 0.5f, 0, 1, 0, 1, 0, 0.9f, 0.5f, 0, 0, 1, 1, -0.9f, -0.9f, 0.5f, 1, 0, 0, 1, 0.9f, -0.9f, 0.5f, 0, 1, 0, 1, 0, 0.9f, 0.5f, 0, 0, 1, 1};
  CHECK(e.write(vb, floats(v, 42)));
  const float identity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
  auto draw = [&](Handle pipe, Topology topology, uint32_t count) {
    CommandWriter w(e.allocator);
    w.begin_pass(0, kNoAllocation, kPassClearColor, {0, 0, 0, 1}, 1, 0);
    w.bind_pipeline(pipe);
    w.bind_vertex_buffer(1);
    w.push_constants(floats(identity, 16));
    w.set_topology(topology);
    w.draw(count);
    w.end_pass();
    return e.render(w, {target, vb});
  };
  auto lit = [&] {
    uint32_t n = 0;
    for (uint32_t y = 0; y < 32; ++y)
      for (uint32_t x = 0; x < 32; ++x) n += (e.pixel(target, x, y) & 0xFFFFFF) != 0;
    return n;
  };
  CHECK(draw(tess, patch_list(3), 3) == status::kSuccess && lit() > 200);
  CHECK(draw(tess, patch_list(3), 6) == status::kSuccess && lit() > 200);   // (two patches)
  CHECK(draw(tess, patch_list(3), 2) == status::kSuccess && lit() == 0);    // (not enough vertices for a patch)
  // What does not go together: the control point count, a list with the tessellation pipeline, patches with a plain pipeline, a topology that does not exist.
  CHECK(draw(tess, patch_list(4), 4) == status::kInvalidParameter && draw(tess, patch_list(1), 3) == status::kInvalidParameter);
  CHECK(draw(tess, Topology::TriangleList, 3) == status::kInvalidParameter && draw(plain, patch_list(3), 3) == status::kInvalidParameter);
  CHECK(draw(tess, static_cast<Topology>(kPatchListBase + graphics::kMaxPatchControlPoints + 1), 3) == status::kInvalidParameter);
  // Factor 3 makes rings inside the triangle; factor 0 culls the patch.
  {
    const std::vector<uint8_t> big = read_blob("hs_tri3.dxbc"), zero = read_blob("hs_tri0.dxbc");
    Handle hs3 = 0, hs0 = 0, tess3 = 0, tess0 = 0;
    CHECK(shader(graphics::ShaderStage::Hull, big.data(), static_cast<uint32_t>(big.size()), &hs3) == status::kSuccess && shader(graphics::ShaderStage::Hull, zero.data(), static_cast<uint32_t>(zero.size()), &hs0) == status::kSuccess);
    CHECK(pipeline(hs3, ds, 0, &tess3) == status::kSuccess && pipeline(hs0, ds, 0, &tess0) == status::kSuccess);
    CHECK(draw(tess3, patch_list(3), 3) == status::kSuccess && lit() > 200);
    CHECK(draw(tess0, patch_list(3), 3) == status::kSuccess && lit() == 0);
  }
  // A device without the feature refuses the shaders and the topology.
  FakeScreen screen(8, 6);
  graphics::SoftwareDevice software(e.allocator, &screen);
  SpyDevice limited(software);
  limited.feature_mask = 0;
  Kernel kernel(e.allocator);
  AdapterConfig c;
  c.display = &screen;
  kernel.add_adapter(limited, c);
  Handle adapter = 0, dev = 0, refused = 0;
  CHECK(kernel.open_process(kPid) == status::kSuccess && kernel.open_adapter_from_index(kPid, 0, &adapter) == status::kSuccess && kernel.create_device(kPid, adapter, &dev) == status::kSuccess);
  CHECK(shader(graphics::ShaderStage::Hull, b::k_hs_tri, sizeof b::k_hs_tri, &refused, &kernel, dev) == status::kNotSupported);
  CHECK(shader(graphics::ShaderStage::Domain, b::k_ds_dent, sizeof b::k_ds_dent, &refused, &kernel, dev) == status::kNotSupported);
}

void test_compute_shared() {
  HostAllocator allocator;
  const std::vector<uint8_t> blob = read_blob("cs_shared.dxbc");
  UniquePointer<D3d9Program> program;
  CHECK(D3d9Program::compile_dxbc(allocator, ConstByteView(blob.data(), blob.size()), &program) == status::kSuccess && program);
  if (!program) return;
  CHECK(program->needs_group() && program->shared_bytes(0) == 256 && program->shared_total_bytes() == 256);
  CHECK((program->required_features() & graphics::kFeatureCompute) != 0);
  uint8_t storage[32 * 32 * 4] = {};
  graphics::ComputeContext context;
  context.buffers[0] = ByteView(storage, sizeof storage);
  context.group_id[0] = 1;
  program->run_compute_group(context, nullptr);
  ByteReader in{ConstByteView(storage, sizeof storage)};
  uint32_t words[1024];
  for (uint32_t& w : words) w = in.le32();
  // (group (1, 0): slot k holds k + 7, so thread 0 reads 8 from its neighbour and the sum of the 64 slots is 2016 + 448 = 2464, whose >> 3 is 308, low byte 0x34; the thread's blue is 4 * its number)
  for (uint32_t t = 0; t < 8; ++t) CHECK(words[8 + t] == (0xFF000000u | ((8 + t) << 16) | (0x34u << 8) | (t * 4)));
  CHECK(words[0] == 0 && words[7] == 0);   // (the other groups were not run)
  // The text for the GPU: the shared memory, the barrier.
  String text(allocator);
  CHECK(program->emit_tgsi(&text) == status::kSuccess);
  const std::string tgsi(text.view().data(), text.size());
  CHECK(tgsi.find("DCL MEMORY[0], SHARED") != std::string::npos && tgsi.find("BARRIER") != std::string::npos && tgsi.find("MEMORY[0]") != std::string::npos);
  // Truncated and bit-flipped: refused or compiled, never a crash.
  for (size_t n = 0; n < blob.size(); n += 11) {
    UniquePointer<D3d9Program> cut;
    D3d9Program::compile_dxbc(allocator, ConstByteView(blob.data(), n), &cut);
  }
  for (size_t bit = 0; bit < blob.size() * 8; bit += 37) {
    std::vector<uint8_t> flipped = blob;
    flipped[bit / 8] ^= static_cast<uint8_t>(1u << (bit % 8));
    UniquePointer<D3d9Program> bad;
    D3d9Program::compile_dxbc(allocator, ConstByteView(flipped.data(), flipped.size()), &bad);
    if (bad && bad->needs_group()) {
      uint8_t scratch[32 * 32 * 4] = {};
      graphics::ComputeContext c;
      c.buffers[0] = ByteView(scratch, sizeof scratch);
      bad->run_compute_group(c, nullptr);
    }
  }
}

// Stream output: a geometry shader with two streams of points (stream 0: the position and colour, stream 1: a vector made of them), and one with a triangle strip, written to buffers.
void test_stream_output() {
  Env e;
  namespace b = conformance_blobs;
  Handle vs = 0, gs = 0, gs_strip = 0, ps = 0;
  auto shader = [&](graphics::ShaderStage stage, const uint8_t* code, uint32_t size, Handle* out) {
    ShaderDesc sd;
    sd.kind = ShaderKind::Dxbc, sd.stage = stage, sd.code = ConstByteView(code, size);
    return e.kernel.create_shader(kPid, e.dev, sd, out);
  };
  CHECK(shader(graphics::ShaderStage::Vertex, b::k_vs, sizeof b::k_vs, &vs) == status::kSuccess && shader(graphics::ShaderStage::Fragment, b::k_ps_color, sizeof b::k_ps_color, &ps) == status::kSuccess);
  CHECK(shader(graphics::ShaderStage::Geometry, b::k_gs_streams, sizeof b::k_gs_streams, &gs) == status::kSuccess && shader(graphics::ShaderStage::Geometry, b::k_gs_copy, sizeof b::k_gs_copy, &gs_strip) == status::kSuccess);
  auto make = [&](Handle geometry, uint32_t entries, const graphics::StreamOutputEntry* list, uint32_t stride0, uint32_t stride1, int32_t rasterized, Handle* out) {
    PipelineDesc pd;
    pd.vertex_shader = vs, pd.fragment_shader = ps, pd.geometry_shader = geometry, pd.vertex_stride = 28, pd.attribute_count = 2, pd.varying_count = 3;
    pd.attributes[0] = {0, graphics::VertexFormat::Float3, 0};
    pd.attributes[1] = {1, graphics::VertexFormat::Float4, 12};
    pd.cull_mode = graphics::CullMode::None;
    pd.stream_output_count = entries;
    for (uint32_t i = 0; i < entries; ++i) pd.stream_output[i] = list[i];
    pd.stream_output_stride[0] = stride0, pd.stream_output_stride[1] = stride1;
    pd.rasterized_stream = rasterized;
    return e.kernel.create_pipeline(kPid, e.dev, pd, out);
  };
  // stream 0: position (output 0) and colour (output 1) in buffer 0; stream 1: the texture coordinate 1 (output 4) in buffer 1
  const graphics::StreamOutputEntry two[3] = {{0, 0, 0, 4, 0}, {0, 1, 0, 4, 0}, {1, 4, 0, 4, 1}};
  Handle pipe = 0, bad = 0;
  CHECK(make(gs, 3, two, 32, 16, 0, &pipe) == status::kSuccess);
  // What does not go together: no geometry shader, a stream the shader does not have, a buffer for two streams, a stride that is too short or not whole words, a rasterized stream that does not exist.
  CHECK(make(0, 3, two, 32, 16, 0, &bad) == status::kInvalidParameter);
  const graphics::StreamOutputEntry past[1] = {{2, 0, 0, 4, 0}}, shared[2] = {{0, 0, 0, 4, 0}, {1, 4, 0, 4, 0}}, empty[1] = {{0, 0, 0, 0, 0}}, overrun[1] = {{0, 0, 2, 4, 0}};
  CHECK(make(gs, 1, past, 16, 16, 0, &bad) == status::kInvalidParameter && make(gs, 2, shared, 32, 16, 0, &bad) == status::kInvalidParameter);
  CHECK(make(gs, 1, empty, 16, 16, 0, &bad) == status::kInvalidParameter && make(gs, 1, overrun, 32, 16, 0, &bad) == status::kInvalidParameter);
  CHECK(make(gs, 3, two, 24, 16, 0, &bad) == status::kInvalidParameter && make(gs, 3, two, 34, 16, 0, &bad) == status::kInvalidParameter && make(gs, 3, two, 32, 16, 4, &bad) == status::kInvalidParameter);
  Handle target = e.alloc(kTarget, Format::A8R8G8B8, 32, 32), vb = e.buffer(kAllocVertexBuffer, 3 * 28);
  AllocationDesc od;
  od.flags = kAllocStreamOutput, od.size = 3 * 32;
  Handle out0 = 0, out1 = 0;
  CHECK(e.kernel.create_allocation(kPid, e.dev, od, &out0) == status::kSuccess);
  od.size = 3 * 16;
  CHECK(e.kernel.create_allocation(kPid, e.dev, od, &out1) == status::kSuccess);
  const float v[21] = {-0.5f, -0.5f, 0.5f, 1, 0, 0, 1, 0.5f, -0.5f, 0.5f, 0, 1, 0, 1, 0, 0.5f, 0.5f, 0, 0, 1, 1};
  CHECK(e.write(vb, floats(v, 21)));
  const float identity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
  auto read_floats = [&](Handle h, uint32_t count, float* out) {
    LockResult lock;
    if (e.kernel.lock(kPid, h, kLockReadOnly, 0, &lock) != status::kSuccess) return false;
    ByteReader in(ConstByteView(lock.data.data(), lock.data.size()));
    for (uint32_t i = 0; i < count; ++i) out[i] = __builtin_bit_cast(float, in.le32());
    e.kernel.unlock(kPid, h);
    return in.ok();
  };
  auto draw = [&](Handle pipeline, uint32_t first_buffer, uint32_t second_buffer, bool bind_second) {
    CommandWriter w(e.allocator);
    w.begin_pass(0, kNoAllocation, kPassClearColor, {0, 0, 0, 1}, 1, 0);
    w.set_stream_output(0, first_buffer);
    if (bind_second) w.set_stream_output(1, second_buffer);
    w.bind_pipeline(pipeline);
    w.bind_vertex_buffer(1);
    w.push_constants(floats(identity, 16));
    w.draw(3);
    w.end_pass();
    return e.render(w, {target, vb, out0, out1});
  };
  // The buffers must all be bound for the draw.
  CHECK(draw(pipe, 2, 3, false) == status::kInvalidParameter);
  CHECK(draw(pipe, 2, 3, true) == status::kSuccess);
  float f0[24], f1[12];
  CHECK(read_floats(out0, 24, f0) && read_floats(out1, 12, f1));
  // Stream 0: the three vertices, position (+Y up, as the shader had it) then colour; stream 1: position * 2 + colour.
  for (uint32_t i = 0; i < 3; ++i) {
    const float* in = v + 7 * i;
    const float position[4] = {in[0], in[1], in[2], 1.0f}, colour[4] = {in[3], in[4], in[5], in[6]};
    for (uint32_t c = 0; c < 4; ++c) {
      CHECK(f0[8 * i + c] == position[c] && f0[8 * i + 4 + c] == colour[c]);
      CHECK(f1[4 * i + c] == position[c] * 2.0f + colour[c]);
    }
  }
  CHECK(e.pixel(target, 0, 0) == 0xFF000000u);   // (points: one pixel each, nothing near the corner)
  // A target that is too small for all the primitives: the ones that fit are written, whole.
  AllocationDesc small;
  small.flags = kAllocStreamOutput, small.size = 2 * 32 + 8;
  Handle short_buffer = 0;
  CHECK(e.kernel.create_allocation(kPid, e.dev, small, &short_buffer) == status::kSuccess);
  CommandWriter w(e.allocator);
  w.begin_pass(0, kNoAllocation, kPassClearColor, {0, 0, 0, 1}, 1, 0);
  w.set_stream_output(0, 2), w.set_stream_output(1, 3);
  w.bind_pipeline(pipe);
  w.bind_vertex_buffer(1);
  w.push_constants(floats(identity, 16));
  w.draw(3);
  w.end_pass();
  CHECK(e.render(w, {target, vb, short_buffer, out1}) == status::kSuccess);
  float sh[18];
  CHECK(read_floats(short_buffer, 18, sh));
  CHECK(sh[0] == v[0] && sh[8] == v[7] && sh[4] == 1 && sh[12] == 0 && sh[16] == 0 && sh[17] == 0);   // (two whole records; nothing of the third, not even the 8 bytes that were left)
  // No stream is drawn with -1.
  Handle quiet = 0;
  CHECK(make(gs, 3, two, 32, 16, -1, &quiet) == status::kSuccess);
  // A triangle strip: the shader of gs_copy emits two strips (the input triangle, and a copy), each a triangle; written as 6 vertices of position and colour in buffer 0.
  const graphics::StreamOutputEntry strip_entries[2] = {{0, 0, 0, 4, 0}, {0, 1, 0, 4, 0}};
  Handle strip_pipe = 0;
  CHECK(make(gs_strip, 2, strip_entries, 32, 0, 0, &strip_pipe) == status::kSuccess);
  AllocationDesc od2;
  od2.flags = kAllocStreamOutput, od2.size = 6 * 32;
  Handle strips = 0;
  CHECK(e.kernel.create_allocation(kPid, e.dev, od2, &strips) == status::kSuccess);
  CommandWriter w2(e.allocator);
  w2.begin_pass(0, kNoAllocation, kPassClearColor, {0, 0, 0, 1}, 1, 0);
  w2.set_stream_output(0, 2);
  w2.bind_pipeline(strip_pipe);
  w2.bind_vertex_buffer(1);
  w2.push_constants(floats(identity, 16));
  w2.draw(3);
  w2.end_pass();
  CHECK(e.render(w2, {target, vb, strips}) == status::kSuccess);
  float s[48];
  CHECK(read_floats(strips, 48, s));
  // (the first triangle: the vertices as they came; the second: moved to the corner, with red and blue swapped)
  CHECK(s[0] == -0.5f && s[1] == -0.5f && s[4] == 1 && s[5] == 0 && s[6] == 0 && s[8] == 0.5f && s[12] == 0 && s[13] == 1);
  CHECK(s[24] != 0.0f || s[25] != 0.0f);
  CHECK(s[28] == 0 && s[30] == 1);   // (the colour of the copy's first vertex: red 1 became blue 1)
  (void)quiet;
}


// Stream output statistics and timestamp queries, appending, stream output of vertex and domain shaders, and drawing a stream other than the first.
void test_stream_output_extras() {
  Env e;
  namespace b = conformance_blobs;
  auto shader = [&](graphics::ShaderStage stage, const uint8_t* code, uint32_t size, Handle* out) {
    ShaderDesc sd;
    sd.kind = ShaderKind::Dxbc, sd.stage = stage, sd.code = ConstByteView(code, size);
    return e.kernel.create_shader(kPid, e.dev, sd, out);
  };
  Handle vs = 0, gs = 0, gs_draw1 = 0, hs = 0, ds = 0, ps = 0;
  CHECK(shader(graphics::ShaderStage::Vertex, b::k_vs, sizeof b::k_vs, &vs) == status::kSuccess && shader(graphics::ShaderStage::Fragment, b::k_ps_color, sizeof b::k_ps_color, &ps) == status::kSuccess);
  CHECK(shader(graphics::ShaderStage::Geometry, b::k_gs_streams, sizeof b::k_gs_streams, &gs) == status::kSuccess && shader(graphics::ShaderStage::Geometry, b::k_gs_streams_draw1, sizeof b::k_gs_streams_draw1, &gs_draw1) == status::kSuccess);
  CHECK(shader(graphics::ShaderStage::Hull, b::k_hs_tri, sizeof b::k_hs_tri, &hs) == status::kSuccess && shader(graphics::ShaderStage::Domain, b::k_ds_dent, sizeof b::k_ds_dent, &ds) == status::kSuccess);
  auto make = [&](Handle hull, Handle domain, Handle geometry, uint32_t entries, const graphics::StreamOutputEntry* list, uint32_t stride0, uint32_t stride1, int32_t rasterized, Handle* out) {
    PipelineDesc pd;
    pd.vertex_shader = vs, pd.fragment_shader = ps, pd.hull_shader = hull, pd.domain_shader = domain, pd.geometry_shader = geometry, pd.vertex_stride = 28, pd.attribute_count = 2, pd.varying_count = 3;
    pd.attributes[0] = {0, graphics::VertexFormat::Float3, 0};
    pd.attributes[1] = {1, graphics::VertexFormat::Float4, 12};
    pd.cull_mode = graphics::CullMode::None;
    pd.stream_output_count = entries;
    for (uint32_t i = 0; i < entries; ++i) pd.stream_output[i] = list[i];
    pd.stream_output_stride[0] = stride0, pd.stream_output_stride[1] = stride1;
    pd.rasterized_stream = rasterized;
    return e.kernel.create_pipeline(kPid, e.dev, pd, out);
  };
  const graphics::StreamOutputEntry two[3] = {{0, 0, 0, 4, 0}, {0, 1, 0, 4, 0}, {1, 4, 0, 4, 1}};
  Handle pipe = 0;
  CHECK(make(0, 0, gs, 3, two, 32, 16, -1, &pipe) == status::kSuccess);
  Handle target = e.alloc(kTarget, Format::A8R8G8B8, 32, 32), vb = e.buffer(kAllocVertexBuffer, 3 * 28);
  const float v[21] = {-0.5f, -0.5f, 0.5f, 1, 0, 0, 1, 0.5f, -0.5f, 0.5f, 0, 1, 0, 1, 0, 0.5f, 0.5f, 0, 0, 1, 1};
  CHECK(e.write(vb, floats(v, 21)));
  const float identity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
  auto buffer = [&](uint32_t bytes) {
    AllocationDesc od;
    od.flags = kAllocStreamOutput, od.size = bytes;
    Handle out = 0;
    CHECK(e.kernel.create_allocation(kPid, e.dev, od, &out) == status::kSuccess);
    return out;
  };
  auto read_floats = [&](Handle h, uint32_t count, float* out) {
    LockResult lock;
    if (e.kernel.lock(kPid, h, kLockReadOnly, 0, &lock) != status::kSuccess) return false;
    ByteReader in(ConstByteView(lock.data.data(), lock.data.size()));
    for (uint32_t i = 0; i < count; ++i) out[i] = __builtin_bit_cast(float, in.le32());
    e.kernel.unlock(kPid, h);
    return in.ok();
  };

  // Statistics: the buffer of stream 0 holds two of the three records, so two are written and three needed; stream 1's holds all.
  Handle small0 = buffer(2 * 32 + 8), full1 = buffer(3 * 16), results = e.buffer(kAllocQueryResult, 64);
  auto statistic = [&](uint32_t kind, uint32_t stream) {
    CommandWriter w(e.allocator);
    w.begin_pass(0, kNoAllocation, kPassClearColor, {0, 0, 0, 1}, 1, 0);
    w.set_stream_output(0, 2), w.set_stream_output(1, 3);
    w.bind_pipeline(pipe);
    w.bind_vertex_buffer(1);
    w.push_constants(floats(identity, 16));
    w.begin_query(4, 0, kind | stream << 8);
    w.draw(3);
    w.end_query();
    w.end_pass();
    CHECK(e.render(w, {target, vb, small0, full1, results}) == status::kSuccess);
    return query_count(e, results, 0);
  };
  const uint32_t kWritten = static_cast<uint32_t>(graphics::QueryKind::SoPrimitivesWritten), kNeeded = static_cast<uint32_t>(graphics::QueryKind::SoPrimitivesNeeded);
  CHECK(statistic(kWritten, 0) == 2 && statistic(kNeeded, 0) == 3 && statistic(kWritten, 1) == 3 && statistic(kNeeded, 1) == 3);
  CHECK(statistic(kNeeded, 2) == 0 && statistic(kNeeded, 3) == 0);   // (streams the shader does not write)
  {
    // (a stream that does not exist, and a statistic of a stream other than the first for a kind that has one)
    CommandWriter w(e.allocator);
    w.begin_pass(0, kNoAllocation, kPassClearColor, {0, 0, 0, 1}, 1, 0);
    w.begin_query(2, 0, kWritten | 4u << 8);
    w.end_query();
    w.end_pass();
    CHECK(e.render(w, {target, vb, results}) == status::kInvalidParameter);
  }

  // Timestamps never go back.
  {
    CommandWriter w(e.allocator);
    w.begin_pass(0, kNoAllocation, kPassClearColor, {0, 0, 0, 1}, 1, 0);
    for (uint32_t q = 0; q < 3; ++q) {
      w.begin_query(2, q * 8, static_cast<uint32_t>(graphics::QueryKind::Timestamp));
      w.end_query();
    }
    w.end_pass();
    CHECK(e.render(w, {target, vb, results}) == status::kSuccess);
    const uint64_t t0 = query_count(e, results, 0), t1 = query_count(e, results, 1), t2 = query_count(e, results, 2);
    CHECK(t0 != 0 && t1 > t0 && t2 > t1);
  }
  {
    // The frequency (nanoseconds) and the disjoint flag (0).
    CommandWriter w(e.allocator);
    w.begin_pass(0, kNoAllocation, kPassClearColor, {0, 0, 0, 1}, 1, 0);
    w.begin_query(2, 0, static_cast<uint32_t>(graphics::QueryKind::TimestampDisjoint));
    w.end_query();
    w.begin_query(2, 8, static_cast<uint32_t>(graphics::QueryKind::TimestampFrequency));
    w.end_query();
    w.end_pass();
    CHECK(e.render(w, {target, vb, results}) == status::kSuccess);
    CHECK(query_count(e, results, 0) == 0 && query_count(e, results, 1) == 1000000000u);
  }
  {
    // An interruption of the clock shows once, in the first disjoint query after it.
    auto disjoint = [&] {
      CommandWriter w(e.allocator);
      w.begin_pass(0, kNoAllocation, kPassClearColor, {0, 0, 0, 1}, 1, 0);
      w.begin_query(2, 0, static_cast<uint32_t>(graphics::QueryKind::TimestampDisjoint));
      w.end_query();
      w.end_pass();
      CHECK(e.render(w, {target, vb, results}) == status::kSuccess);
      return query_count(e, results, 0);
    };
    CHECK(disjoint() == 0);
    e.kernel.adapter(0)->interrupt_clock();
    CHECK(disjoint() == 1 && disjoint() == 0);
  }

  // Appending: the second draw, in another pass, goes after the first's records.
  Handle big0 = buffer(6 * 32), big1 = buffer(6 * 16);
  Handle quiet = 0;
  CHECK(make(0, 0, gs, 3, two, 32, 16, -1, &quiet) == status::kSuccess);
  auto pass = [&](uint32_t offset0, uint32_t offset1) {
    CommandWriter w(e.allocator);
    w.begin_pass(0, kNoAllocation, kPassClearColor, {0, 0, 0, 1}, 1, 0);
    w.set_stream_output(0, 2, offset0), w.set_stream_output(1, 3, offset1);
    w.bind_pipeline(quiet);
    w.bind_vertex_buffer(1);
    w.push_constants(floats(identity, 16));
    w.draw(3);
    w.end_pass();
    return e.render(w, {target, vb, big0, big1});
  };
  CHECK(pass(0, 0) == status::kSuccess && pass(kStreamOutputAppendOffset, kStreamOutputAppendOffset) == status::kSuccess);
  float a0[48], a1[24];
  CHECK(read_floats(big0, 48, a0) && read_floats(big1, 24, a1));
  for (uint32_t i = 0; i < 24; ++i) CHECK(a0[i] == a0[24 + i]);   // (the second round of records is the first's again, after it)
  for (uint32_t i = 0; i < 12; ++i) CHECK(a1[i] == a1[12 + i]);
  CHECK(a0[0] == -0.5f && a0[8] == 0.5f && a0[16] == 0.0f && a1[3] != 0.0f);
  // A third round does not fit: nothing past the end is written and the draw is not an error. Starting over (offset 0) writes the first record again.
  CHECK(pass(kStreamOutputAppendOffset, kStreamOutputAppendOffset) == status::kSuccess && pass(kStreamOutputAppendOffset, kStreamOutputAppendOffset) == status::kSuccess);
  CHECK(pass(0, 0) == status::kSuccess);

  // A vertex shader's stream output: the vertices of the triangle, in order, position as it was (+Y up) and colour.
  const graphics::StreamOutputEntry vs_entries[2] = {{0, 0, 0, 4, 0}, {0, 1, 0, 4, 0}};
  Handle vs_pipe = 0, vs_bad = 0;
  CHECK(make(0, 0, 0, 2, vs_entries, 32, 0, -1, &vs_pipe) == status::kSuccess);
  const graphics::StreamOutputEntry vs_stream1[1] = {{1, 0, 0, 4, 0}};
  CHECK(make(0, 0, 0, 1, vs_stream1, 16, 0, -1, &vs_bad) == status::kInvalidParameter);
  Handle vs_out = buffer(3 * 32), vs_count = e.buffer(kAllocQueryResult, 16);
  {
    CommandWriter w(e.allocator);
    w.begin_pass(0, kNoAllocation, kPassClearColor, {0, 0, 0, 1}, 1, 0);
    w.set_stream_output(0, 2);
    w.bind_pipeline(vs_pipe);
    w.bind_vertex_buffer(1);
    w.push_constants(floats(identity, 16));
    w.begin_query(3, 0, kWritten);
    w.draw(3);
    w.end_query();
    w.end_pass();
    CHECK(e.render(w, {target, vb, vs_out, vs_count}) == status::kSuccess);
    float f[24];
    CHECK(read_floats(vs_out, 24, f));
    for (uint32_t i = 0; i < 3; ++i)
      for (uint32_t c = 0; c < 3; ++c) CHECK(f[8 * i + c] == v[7 * i + c] && f[8 * i + 4 + c] == v[7 * i + 3 + c]);
    CHECK(query_count(e, vs_count, 0) == 1);
  }

  // A domain shader's: the tessellated triangles of a three-point patch with factors 2 (positions, 16 bytes a vertex, as many as the tessellator makes).
  const graphics::StreamOutputEntry ds_entries[1] = {{0, 0, 0, 4, 0}};
  Handle ds_pipe = 0;
  CHECK(make(hs, ds, 0, 1, ds_entries, 16, 0, -1, &ds_pipe) == status::kSuccess);
  Handle ds_out = buffer(64 * 3 * 16);
  {
    CommandWriter w(e.allocator);
    w.begin_pass(0, kNoAllocation, kPassClearColor, {0, 0, 0, 1}, 1, 0);
    w.set_stream_output(0, 2);
    w.bind_pipeline(ds_pipe);
    w.bind_vertex_buffer(1);
    w.push_constants(floats(identity, 16));
    w.set_topology(patch_list(3));
    w.begin_query(3, 0, kWritten);
    w.draw(3);
    w.end_query();
    w.end_pass();
    CHECK(e.render(w, {target, vb, ds_out, vs_count}) == status::kSuccess);
    const uint64_t triangles = query_count(e, vs_count, 0);
    CHECK(triangles >= 4 && triangles <= 64);
    float f[64 * 12];
    CHECK(read_floats(ds_out, 64 * 12, f));
    for (uint64_t i = 0; i < triangles * 3; ++i) CHECK(f[4 * i + 3] == 1.0f && f[4 * i] >= -1.0f && f[4 * i] <= 1.0f && f[4 * i + 1] >= -1.0f && f[4 * i + 1] <= 1.0f);
  }

  // Stream 1 drawn: gs_streams_draw1 puts a point at each vertex (moved by half a pixel) in stream 1 and the vertex plus its colour in stream 0, which only the buffer gets.
  const graphics::StreamOutputEntry draw1_entries[2] = {{0, 4, 0, 4, 0}, {1, 0, 0, 4, 1}};
  Handle draw1 = 0, draw1_bad = 0;
  CHECK(make(0, 0, gs_draw1, 2, draw1_entries, 16, 16, 1, &draw1) == status::kSuccess);
  CHECK(make(0, 0, 0, 1, vs_entries, 32, 0, 1, &draw1_bad) == status::kInvalidParameter);   // (no stream 1 without a geometry shader)
  Handle d0 = buffer(3 * 16), d1 = buffer(3 * 16);
  {
    CommandWriter w(e.allocator);
    w.begin_pass(0, kNoAllocation, kPassClearColor, {0, 0, 0, 1}, 1, 0);
    w.set_stream_output(0, 2), w.set_stream_output(1, 3);
    w.bind_pipeline(draw1);
    w.bind_vertex_buffer(1);
    w.push_constants(floats(identity, 16));
    w.draw(3);
    w.end_pass();
    CHECK(e.render(w, {target, vb, d0, d1}) == status::kSuccess);
    float f[12], g[12];
    CHECK(read_floats(d0, 12, f) && read_floats(d1, 12, g));
    CHECK(f[0] == -0.5f + 1.0f && f[1] == -0.5f + 0.0f);   // (position + colour: red)
    CHECK(g[0] == -0.5f + 0.03125f && g[4] == 0.5f + 0.03125f);
    // (points at the pixel centres, the colour of each vertex)
    CHECK(e.pixel(target, 8, 23) == 0xFFFF0000u && e.pixel(target, 24, 23) == 0xFF00FF00u && e.pixel(target, 16, 7) == 0xFF0000FFu);
  }
}


// --- Direct3D 12 binding: root signatures, descriptor heaps, resource states -----------------------------------------------------------------------------------------------

struct SigRange {
  uint32_t type, count, base, space, offset;
};
struct SigParam {
  uint32_t type, visibility = 0;
  std::vector<SigRange> ranges;   // Table
  uint32_t reg = 0, space = 0, num = 0;   // Constants (num), Cbv / Srv / Uav (reg, space)
};

// A serialized root signature, the way D3D12SerializeRootSignature lays it out.
std::vector<uint8_t> serialize_signature(uint32_t version, const std::vector<SigParam>& params, const std::vector<std::vector<uint32_t>>& samplers = {}) {
  std::vector<uint32_t> w(6, 0);
  const uint32_t range_words = version == 2 ? 6 : 5;
  uint32_t cursor = 6 + 3 * static_cast<uint32_t>(params.size());   // (in words: after the header and the parameter array)
  std::vector<uint32_t> payload;
  w[0] = version, w[1] = static_cast<uint32_t>(params.size()), w[2] = 24, w[3] = static_cast<uint32_t>(samplers.size());
  std::vector<uint32_t> array;
  for (const SigParam& p : params) {
    array.push_back(p.type), array.push_back(p.visibility), array.push_back((cursor + static_cast<uint32_t>(payload.size())) * 4);
    if (p.type == 0) {
      const uint32_t ranges_at = cursor + static_cast<uint32_t>(payload.size()) + 2;
      payload.push_back(static_cast<uint32_t>(p.ranges.size())), payload.push_back(ranges_at * 4);
      for (const SigRange& r : p.ranges) {
        payload.push_back(r.type), payload.push_back(r.count), payload.push_back(r.base), payload.push_back(r.space);
        if (version == 2) payload.push_back(0);
        payload.push_back(r.offset);
      }
    } else if (p.type == 1) {
      payload.push_back(p.reg), payload.push_back(p.space), payload.push_back(p.num);
    } else {
      payload.push_back(p.reg), payload.push_back(p.space);
      if (version == 2) payload.push_back(0);
    }
  }
  (void)range_words;
  w[4] = (cursor + static_cast<uint32_t>(payload.size())) * 4;
  w.insert(w.end(), array.begin(), array.end());
  w.insert(w.end(), payload.begin(), payload.end());
  for (const std::vector<uint32_t>& sampler : samplers) w.insert(w.end(), sampler.begin(), sampler.end());
  std::vector<uint8_t> bytes;
  for (uint32_t word : w)
    for (int i = 0; i < 4; ++i) bytes.push_back(static_cast<uint8_t>(word >> (8 * i)));
  return bytes;
}

// A static sampler: filter, address u / v / w, bias, anisotropy, comparison, border, min lod, max lod, register, space, visibility.
std::vector<uint32_t> static_sampler(uint32_t filter, uint32_t address, uint32_t reg) {
  return {filter, address, address, address, 0, 1, 8, 0, 0, __builtin_bit_cast(uint32_t, 1000.0f), reg, 0, 0};
}

void test_root_signature_parse() {
  const std::vector<SigParam> params = {
      {0, 0, {{0, 2, 0, 0, 0}, {1, 0xFFFFFFFFu, 1, 0, 0xFFFFFFFFu}}},   // a table: two shader resource views from t0, then (appended) an unbounded range of unordered access views from u1
      {1, 5, {}, 0, 0, 16},                                             // 16 root constants at b0 for the pixel shader
      {2, 0, {}, 1, 0, 0},                                              // a root constant buffer view at b1
      {0, 0, {{3, 4, 0, 0, 0}}},                                        // a sampler table of four
  };
  for (uint32_t version : {1u, 2u}) {
    const std::vector<uint8_t> blob = serialize_signature(version, params, {static_sampler(0x15, 3, 0)});
    RootSignatureDesc desc;
    CHECK(parse_root_signature(ConstByteView(blob.data(), blob.size()), &desc) == status::kSuccess);
    CHECK(desc.parameter_count == 4 && desc.sampler_count == 1);
    CHECK(desc.parameters[0].type == RootParameterType::Table && desc.parameters[0].range_count == 2 && desc.parameters[0].ranges[1].offset == 2 && desc.parameters[0].ranges[1].count == kUnboundedDescriptors);
    CHECK(desc.parameters[0].table_size == kUnboundedDescriptors && !desc.parameters[0].sampler_table && desc.parameters[3].sampler_table && desc.parameters[3].table_size == 4);
    CHECK(desc.parameters[1].constant_count == 16 && desc.parameters[1].visibility == 5 && desc.parameters[2].shader_register == 1);
    CHECK(desc.dwords == 1 + 16 + 2 + 1);
    CHECK(desc.samplers[0].sampler.filter == graphics::Filter::Linear && desc.samplers[0].sampler.address_u == graphics::AddressMode::ClampToEdge);
    // Every truncation and a flipped bit anywhere: refused or parsed, never a crash.
    for (size_t n = 0; n <= blob.size(); ++n) {
      RootSignatureDesc cut;
      parse_root_signature(ConstByteView(blob.data(), n), &cut);
    }
    for (size_t bit = 0; bit < blob.size() * 8; ++bit) {
      std::vector<uint8_t> flipped = blob;
      flipped[bit / 8] ^= static_cast<uint8_t>(1u << (bit % 8));
      RootSignatureDesc other;
      parse_root_signature(ConstByteView(flipped.data(), flipped.size()), &other);
    }
  }
  // Against the rules: too many DWORDs, a table of views and samplers, an unbounded range in the middle, no ranges, a version that does not exist, a register space past its type.
  RootSignatureDesc d;
  auto bad = [&](const std::vector<SigParam>& p) {
    const std::vector<uint8_t> blob = serialize_signature(1, p);
    return parse_root_signature(ConstByteView(blob.data(), blob.size()), &d) == status::kInvalidParameter;
  };
  CHECK(bad({{1, 0, {}, 0, 0, 64}, {2, 0, {}, 1, 0, 0}}));                                       // (66 DWORDs)
  CHECK(bad({{0, 0, {{0, 1, 0, 0, 0}, {3, 1, 0, 0, 1}}}}));                                       // (a view and a sampler in one table)
  CHECK(bad({{0, 0, {{0, 0xFFFFFFFFu, 0, 0, 0}, {0, 1, 0, 0, 0xFFFFFFFFu}}}}));                  // (unbounded, then another)
  CHECK(bad({{0, 0, {}}}));                                                                       // (no ranges)
  CHECK(bad({{1, 0, {}, 0, 0, 0}}));                                                              // (no values)
  std::vector<uint8_t> blob = serialize_signature(1, {{1, 0, {}, 0, 0, 4}});
  blob[0] = 7;
  CHECK(parse_root_signature(ConstByteView(blob.data(), blob.size()), &d) == status::kInvalidParameter);
  // In a DXBC container (the RTS0 chunk).
  const std::vector<uint8_t> inner = serialize_signature(2, {{1, 0, {}, 0, 0, 4}});
  std::vector<uint8_t> container = {'D', 'X', 'B', 'C'};
  auto put32 = [&](uint32_t v) { for (int i = 0; i < 4; ++i) container.push_back(static_cast<uint8_t>(v >> (8 * i))); };
  container.insert(container.end(), 16, 0);
  put32(1), put32(static_cast<uint32_t>(36 + 8 + inner.size())), put32(1), put32(36);
  container.push_back('R'), container.push_back('T'), container.push_back('S'), container.push_back('0');
  put32(static_cast<uint32_t>(inner.size()));
  container.insert(container.end(), inner.begin(), inner.end());
  CHECK(parse_root_signature(ConstByteView(container.data(), container.size()), &d) == status::kSuccess && d.parameter_count == 1 && d.parameters[0].constant_count == 4);
  for (size_t n = 0; n < container.size(); n += 3) parse_root_signature(ConstByteView(container.data(), n), &d);
}

void test_d3d12_binding() {
  Env e;
  Handle rt = e.alloc(kTarget, Format::A8R8G8B8, 8, 8), tex = e.alloc(kTexture, Format::A8R8G8B8, 4, 4), vb = e.buffer(kAllocVertexBuffer, 4 * 20);
  CHECK(e.fill(tex, 0xFFC08040));
  Handle pipe = make_colored_pipeline(e, true);
  const float quad[20] = {-1, -1, 0.5f, 0, 0, 3, -1, 0.5f, 2, 0, -1, 3, 0.5f, 0, 2};
  CHECK(e.write(vb, floats(quad, 15)));
  const float identity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
  auto signature = [&](const std::vector<SigParam>& p, const std::vector<std::vector<uint32_t>>& samplers, Handle* out) {
    const std::vector<uint8_t> blob = serialize_signature(2, p, samplers);
    return e.kernel.create_root_signature(kPid, e.dev, ConstByteView(blob.data(), blob.size()), out);
  };
  // t0 through a table, b0 as 16 root constants, s0 static.
  Handle sig = 0, sig_samplers = 0, bad = 0;
  CHECK(signature({{0, 0, {{0, 1, 0, 0, 0}}}, {1, 0, {}, 0, 0, 16}}, {static_sampler(0, 1, 0)}, &sig) == status::kSuccess);
  CHECK(signature({{0, 0, {{0, 1, 0, 0, 0}}}, {1, 0, {}, 0, 0, 16}, {0, 0, {{3, 1, 0, 0, 0}}}}, {}, &sig_samplers) == status::kSuccess);
  const std::vector<uint8_t> garbage = {1, 2, 3};
  CHECK(e.kernel.create_root_signature(kPid, e.dev, ConstByteView(garbage.data(), garbage.size()), &bad) == status::kInvalidParameter);
  CHECK(e.kernel.create_root_signature(kPid, 0x7777, ConstByteView(garbage.data(), garbage.size()), &bad) == status::kInvalidHandle);
  Handle views = 0, samplers = 0, small = 0;
  CHECK(e.kernel.create_descriptor_heap(kPid, e.dev, DescriptorHeapType::CbvSrvUav, 8, &views) == status::kSuccess && e.kernel.create_descriptor_heap(kPid, e.dev, DescriptorHeapType::Sampler, 4, &samplers) == status::kSuccess);
  CHECK(e.kernel.create_descriptor_heap(kPid, e.dev, DescriptorHeapType::CbvSrvUav, 0, &bad) == status::kInvalidParameter && e.kernel.create_descriptor_heap(kPid, e.dev, DescriptorHeapType::CbvSrvUav, kMaxHeapDescriptors + 1, &bad) == status::kInvalidParameter);
  CHECK(e.kernel.create_descriptor_heap(kPid, e.dev, DescriptorHeapType::CbvSrvUav, 1, &small) == status::kSuccess);
  DescriptorDesc srv;
  srv.kind = DescriptorKind::Srv, srv.allocation = tex;
  CHECK(e.kernel.write_descriptor(kPid, views, 3, srv) == status::kSuccess && e.kernel.write_descriptor(kPid, views, 8, srv) == status::kInvalidParameter);
  CHECK(e.kernel.write_descriptor(kPid, samplers, 0, srv) == status::kInvalidParameter);   // (a view in the sampler heap)
  DescriptorDesc sampler;
  sampler.kind = DescriptorKind::Sampler;
  sampler.sampler.address_u = sampler.sampler.address_v = graphics::AddressMode::ClampToEdge;
  CHECK(e.kernel.write_descriptor(kPid, samplers, 1, sampler) == status::kSuccess && e.kernel.write_descriptor(kPid, views, 0, sampler) == status::kInvalidParameter);
  DescriptorDesc buffer_view;
  buffer_view.kind = DescriptorKind::Srv, buffer_view.allocation = vb;
  CHECK(e.kernel.write_descriptor(kPid, views, 0, buffer_view) == status::kInvalidParameter);   // (a shader resource view of a buffer)
  srv.allocation = 0x7777;
  CHECK(e.kernel.write_descriptor(kPid, views, 0, srv) == status::kInvalidHandle);
  CHECK(e.kernel.copy_descriptors(kPid, views, 0, views, 3, 1) == status::kSuccess && e.kernel.copy_descriptors(kPid, views, 0, samplers, 0, 1) == status::kInvalidParameter);
  CHECK(e.kernel.copy_descriptors(kPid, views, 6, views, 0, 3) == status::kInvalidParameter);
  CHECK(e.kernel.copy_descriptors(kPid, views, 1, views, 0, 2) == status::kSuccess);   // (an overlap, copied as memmove would: 1 is a copy of 0, which was a copy of 3; 2 has what 1 had, which was nothing)

  auto draw = [&](auto bind) {
    CommandWriter w(e.allocator);
    w.begin_pass(0, kNoAllocation, kPassClearColor, {0, 0, 0, 1}, 1, 0);
    w.bind_pipeline(pipe);
    w.bind_vertex_buffer(2);
    bind(w);
    w.draw(3);
    w.end_pass();
    return w.ok() ? e.render(w, {rt, tex, vb}) : status::kNoMemory;
  };
  // The reference: the same through the plain ops.
  CHECK(draw([&](CommandWriter& w) { w.push_constants(floats(identity, 16)); w.bind_texture(0, 1, graphics::SamplerDesc()); }) == status::kSuccess && e.pixel(rt, 4, 4) == 0xFFC08040);
  // The root signature way: the table at descriptor 1 (a copy of the texture's), the constants, the static sampler.
  CHECK(e.fill(rt, 0));
  CHECK(draw([&](CommandWriter& w) { w.set_root_signature(sig); w.set_descriptor_heaps({views}); w.set_root_constants(1, 0, floats(identity, 16)); w.set_root_table(0, 1); }) == status::kSuccess);
  CHECK(e.pixel(rt, 4, 4) == 0xFFC08040);
  // With a sampler table from the sampler heap too.
  CHECK(e.fill(rt, 0));
  CHECK(draw([&](CommandWriter& w) { w.set_root_signature(sig_samplers); w.set_descriptor_heaps({views, samplers}); w.set_root_constants(1, 0, floats(identity, 16)); w.set_root_table(0, 1); w.set_root_table(2, 1); }) == status::kSuccess);
  CHECK(e.pixel(rt, 4, 4) == 0xFFC08040);
  // Refused: a draw before the arguments are set, constants past the parameter, the wrong kind of parameter, a table past the heap, two heaps of a kind, a heap the process does not have, a signature it does not have.
  CHECK(draw([&](CommandWriter& w) { w.set_root_signature(sig); w.set_descriptor_heaps({views}); w.set_root_constants(1, 0, floats(identity, 16)); }) == status::kInvalidParameter);   // (no table)
  CHECK(draw([&](CommandWriter& w) { w.set_root_signature(sig); w.set_descriptor_heaps({views}); w.set_root_table(0, 1); }) == status::kInvalidParameter);                                     // (no constants)
  CHECK(draw([&](CommandWriter& w) { w.set_root_signature(sig); w.set_descriptor_heaps({views}); w.set_root_constants(1, 8, floats(identity, 16)); }) == status::kInvalidParameter);
  CHECK(draw([&](CommandWriter& w) { w.set_root_signature(sig); w.set_descriptor_heaps({views}); w.set_root_constants(0, 0, floats(identity, 16)); }) == status::kInvalidParameter);
  CHECK(draw([&](CommandWriter& w) { w.set_root_signature(sig); w.set_descriptor_heaps({views}); w.set_root_table(0, 8); }) == status::kInvalidParameter);
  CHECK(draw([&](CommandWriter& w) { w.set_root_signature(sig); w.set_descriptor_heaps({views, small}); }) == status::kInvalidParameter);
  CHECK(draw([&](CommandWriter& w) { w.set_root_signature(sig); w.set_descriptor_heaps({0x7777}); }) == status::kInvalidHandle);
  CHECK(draw([&](CommandWriter& w) { w.set_root_signature(0x7777); }) == status::kInvalidHandle);
  CHECK(draw([&](CommandWriter& w) { w.set_root_constants(1, 0, floats(identity, 16)); }) == status::kInvalidParameter);   // (no root signature)
  // A descriptor that was not written leaves its unit unbound: the draw is refused (the pipeline samples it).
  CHECK(draw([&](CommandWriter& w) { w.set_root_signature(sig); w.set_descriptor_heaps({views}); w.set_root_constants(1, 0, floats(identity, 16)); w.set_root_table(0, 5); }) == status::kInvalidParameter);
  // A root constant buffer view of a constant buffer allocation, in place of the root constants.
  Handle cb_sig = 0, cb = e.buffer(kAllocConstantBuffer, 64);
  CHECK(signature({{0, 0, {{0, 1, 0, 0, 0}}}, {2, 0, {}, 0, 0, 0}}, {static_sampler(0, 1, 0)}, &cb_sig) == status::kSuccess);
  CHECK(e.write(cb, floats(identity, 16)));
  CHECK(e.fill(rt, 0));
  auto draw_cb = [&](auto bind) {
    CommandWriter w(e.allocator);
    w.begin_pass(0, kNoAllocation, kPassClearColor, {0, 0, 0, 1}, 1, 0);
    w.bind_pipeline(pipe);
    w.bind_vertex_buffer(2);
    bind(w);
    w.draw(3);
    w.end_pass();
    return e.render(w, {rt, tex, vb, cb});
  };
  CHECK(draw_cb([&](CommandWriter& w) { w.set_root_signature(cb_sig); w.set_descriptor_heaps({views}); w.set_root_table(0, 0); w.set_root_descriptor(1, 3, 0); }) == status::kSuccess && e.pixel(rt, 4, 4) == 0xFFC08040);
  CHECK(draw_cb([&](CommandWriter& w) { w.set_root_signature(cb_sig); w.set_descriptor_heaps({views}); w.set_root_table(0, 0); w.set_root_descriptor(1, 3, 64); }) == status::kInvalidParameter);   // (past the end)
  CHECK(draw_cb([&](CommandWriter& w) { w.set_root_signature(cb_sig); w.set_descriptor_heaps({views}); w.set_root_table(0, 0); w.set_root_descriptor(1, 0, 0); }) == status::kInvalidParameter);   // (an image is not a constant buffer)
  // A constant buffer view in a table.
  Handle table_cb_sig = 0;
  CHECK(signature({{0, 0, {{0, 1, 0, 0, 0}, {2, 1, 0, 0, 1}}}}, {static_sampler(0, 1, 0)}, &table_cb_sig) == status::kSuccess);
  DescriptorDesc cbv;
  cbv.kind = DescriptorKind::Cbv, cbv.allocation = cb;
  srv.allocation = tex;
  CHECK(e.kernel.write_descriptor(kPid, views, 6, srv) == status::kSuccess && e.kernel.write_descriptor(kPid, views, 7, cbv) == status::kSuccess);
  CHECK(e.fill(rt, 0));
  CHECK(draw_cb([&](CommandWriter& w) { w.set_root_signature(table_cb_sig); w.set_descriptor_heaps({views}); w.set_root_table(0, 6); }) == status::kSuccess && e.pixel(rt, 4, 4) == 0xFFC08040);
  // The constant buffer: the size rules, and it never reaches the device.
  AllocationDesc od;
  od.flags = kAllocConstantBuffer, od.size = 20;
  Handle none = 0;
  CHECK(e.kernel.create_allocation(kPid, e.dev, od, &none) == status::kInvalidParameter);
  od.flags = kAllocConstantBuffer | kAllocVertexBuffer, od.size = 64;
  CHECK(e.kernel.create_allocation(kPid, e.dev, od, &none) == status::kInvalidParameter);
  AllocationInfo info;
  CHECK(e.kernel.query_allocation(kPid, cb, &info) == status::kSuccess && !info.resident);
  Handle device_list[1] = {cb};
  CHECK(e.kernel.make_resident(kPid, e.dev, ArrayView<const Handle>(device_list, 1)) == status::kInvalidParameter);
  // Destroying a device takes the signatures and heaps with it.
  CHECK(e.kernel.destroy_root_signature(kPid, sig) == status::kSuccess && e.kernel.destroy_root_signature(kPid, sig) == status::kInvalidHandle);
  CHECK(e.kernel.destroy_descriptor_heap(kPid, small) == status::kSuccess && e.kernel.destroy_descriptor_heap(kPid, small) == status::kInvalidHandle);
}

// Root unordered access views and root constants for a compute shader.
void test_d3d12_compute() {
  Env e;
  namespace b = conformance_blobs;
  Handle cs = 0, cs_pipe = 0;
  ShaderDesc sd;
  sd.kind = ShaderKind::Dxbc, sd.stage = graphics::ShaderStage::Compute, sd.code = ConstByteView(b::k_cs_fill, sizeof b::k_cs_fill);
  CHECK(e.kernel.create_shader(kPid, e.dev, sd, &cs) == status::kSuccess);
  PipelineDesc pd;
  pd.compute_shader = cs;
  CHECK(e.kernel.create_pipeline(kPid, e.dev, pd, &cs_pipe) == status::kSuccess);
  auto storage = [&](uint64_t size) { return e.buffer(kAllocUnorderedAccess, size); };
  Handle picture = storage(4096), structured = storage(16384), counters = storage(16);
  const std::vector<uint8_t> blob = serialize_signature(1, {{1, 0, {}, 0, 0, 4}, {4, 0, {}, 0, 0, 0}, {4, 0, {}, 1, 0, 0}, {0, 0, {{1, 1, 2, 0, 0}}}});
  Handle sig = 0, views = 0;
  CHECK(e.kernel.create_root_signature(kPid, e.dev, ConstByteView(blob.data(), blob.size()), &sig) == status::kSuccess);
  CHECK(e.kernel.create_descriptor_heap(kPid, e.dev, DescriptorHeapType::CbvSrvUav, 4, &views) == status::kSuccess);
  DescriptorDesc uav;
  uav.kind = DescriptorKind::Uav, uav.allocation = counters;
  CHECK(e.kernel.write_descriptor(kPid, views, 2, uav) == status::kSuccess);
  const uint32_t constants[4] = {0x00102030u, 0, 0, 0};
  auto run = [&](auto bind) {
    CommandWriter w(e.allocator);
    w.set_root_signature(sig);
    w.set_descriptor_heaps({views});
    w.set_root_constants(0, 0, ConstByteView(reinterpret_cast<const uint8_t*>(constants), 16));
    bind(w);
    w.bind_pipeline(cs_pipe);
    w.dispatch(4, 8, 1);
    return w.ok() ? e.render(w, {picture, structured, counters}) : status::kNoMemory;
  };
  // u0 and u1 as root descriptors, u2 from the table.
  CHECK(run([&](CommandWriter& w) { w.set_root_descriptor(1, 0, 0); w.set_root_descriptor(2, 1, 0); w.set_root_table(3, 2); }) == status::kSuccess);
  CHECK(e.pixel(picture, 0, 0) == 0xFF102030u);
  LockResult lock;
  CHECK(e.kernel.lock(kPid, counters, kLockReadOnly, 0, &lock) == status::kSuccess);
  ByteReader in(lock.data);
  CHECK(in.le32() == 1024);
  e.kernel.unlock(kPid, counters);
  // The root descriptor of a unordered access view must be a UAV buffer; the register space has to be 0 (the second one here is space 1: refused when used).
  CHECK(run([&](CommandWriter& w) { w.set_root_descriptor(1, 0, 0); w.set_root_descriptor(2, 1, 0); w.set_root_table(3, 2); w.set_root_descriptor(1, 2, 2); }) == status::kInvalidParameter);   // (offset not a multiple of 4... 2)
}


// Shader resource views of buffers: a root descriptor (t0) and a table entry (t1) reach a compute shader as read-only storage buffers.
void test_d3d12_buffer_views() {
  Env e;
  namespace b = conformance_blobs;
  Handle cs = 0, pipe = 0;
  ShaderDesc sd;
  sd.kind = ShaderKind::Dxbc, sd.stage = graphics::ShaderStage::Compute, sd.code = ConstByteView(b::k_dxil_cs_srv, sizeof b::k_dxil_cs_srv);
  CHECK(e.kernel.create_shader(kPid, e.dev, sd, &cs) == status::kSuccess);
  PipelineDesc pd;
  pd.compute_shader = cs;
  CHECK(e.kernel.create_pipeline(kPid, e.dev, pd, &pipe) == status::kSuccess);
  Handle dst = e.buffer(kAllocUnorderedAccess, 4096), src = e.buffer(kAllocUnorderedAccess, 16384), raw = e.buffer(kAllocUnorderedAccess, 4096);
  std::vector<uint32_t> a(4096), r(1024);
  for (uint32_t i = 0; i < a.size(); ++i) a[i] = 0x01010101u * (i % 251) + i * 7;
  for (uint32_t i = 0; i < r.size(); ++i) r[i] = 0x9E3779B9u * (i + 1);
  CHECK(e.write(src, ConstByteView(reinterpret_cast<const uint8_t*>(a.data()), 16384)) && e.write(raw, ConstByteView(reinterpret_cast<const uint8_t*>(r.data()), 4096)));
  const std::vector<uint8_t> blob = serialize_signature(1, {{4, 0, {}, 0, 0, 0}, {3, 0, {}, 0, 0, 0}, {0, 0, {{0, 1, 1, 0, 0}}}});
  Handle sig = 0, views = 0;
  CHECK(e.kernel.create_root_signature(kPid, e.dev, ConstByteView(blob.data(), blob.size()), &sig) == status::kSuccess);
  CHECK(e.kernel.create_descriptor_heap(kPid, e.dev, DescriptorHeapType::CbvSrvUav, 2, &views) == status::kSuccess);
  DescriptorDesc srv;
  srv.kind = DescriptorKind::Srv, srv.allocation = raw;
  CHECK(e.kernel.write_descriptor(kPid, views, 1, srv) == status::kSuccess);
  auto run = [&](Handle root_src) {
    CommandWriter w(e.allocator);
    w.set_root_signature(sig);
    w.set_descriptor_heaps({views});
    w.set_root_descriptor(0, 0, 0);
    w.set_root_descriptor(1, root_src, 0);
    w.set_root_table(2, 1);
    w.bind_pipeline(pipe);
    w.dispatch(4, 8, 1);
    return w.ok() ? e.render(w, {dst, src, raw}) : status::kNoMemory;
  };
  CHECK(run(1) == status::kSuccess);
  LockResult lock;
  CHECK(e.kernel.lock(kPid, dst, kLockReadOnly, 0, &lock) == status::kSuccess);
  ByteReader in(lock.data);
  bool same = true;
  for (uint32_t i = 0; i < 1024; ++i) same = same && in.le32() == a[4 * i] + a[4 * i + 3] * 3 + r[i] + (r[1023 - i] ^ a[4 * i + 1]);
  CHECK(same);
  e.kernel.unlock(kPid, dst);
  // A texture is not a buffer view; a buffer that is not a storage buffer is no root SRV.
  Handle vb = e.buffer(kAllocVertexBuffer, 4096);
  CHECK(run(vb) == status::kInvalidParameter);
}

// Resource states: an allocation made with kAllocTrackState is used only in states that allow it.
void test_d3d12_barriers() {
  Env e;
  AllocationDesc d;
  d.flags = kAllocRenderTarget | kAllocTrackState, d.format = Format::A8R8G8B8, d.width = 8, d.height = 8, d.initial_state = state::kCommon;
  Handle rt = 0, tex = 0, vb = 0, plain_rt = e.alloc(kTarget, Format::A8R8G8B8, 8, 8);
  CHECK(e.kernel.create_allocation(kPid, e.dev, d, &rt) == status::kSuccess);
  d.flags = kAllocTexture | kAllocTrackState, d.initial_state = state::kPixelShaderResource;
  CHECK(e.kernel.create_allocation(kPid, e.dev, d, &tex) == status::kSuccess);
  d.flags = kAllocVertexBuffer | kAllocTrackState, d.size = 4 * 20, d.initial_state = state::kGenericRead;
  CHECK(e.kernel.create_allocation(kPid, e.dev, d, &vb) == status::kSuccess);
  d.flags = kAllocRenderTarget | kAllocTrackState, d.initial_state = 0x8000;   // (not a state)
  Handle none = 0;
  CHECK(e.kernel.create_allocation(kPid, e.dev, d, &none) == status::kInvalidParameter);
  Handle pipe = make_colored_pipeline(e, true);
  const float quad[20] = {-1, -1, 0.5f, 0, 0, 3, -1, 0.5f, 2, 0, -1, 3, 0.5f, 0, 2};
  CHECK(e.write(vb, floats(quad, 15)));
  CHECK(e.fill(tex, 0xFFC08040));
  const float identity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
  auto frame = [&](auto barriers, Handle target = 0) {
    CommandWriter w(e.allocator);
    barriers(w);
    w.begin_pass(0, kNoAllocation, kPassClearColor, {0, 0, 0, 1}, 1, 0);
    w.bind_pipeline(pipe);
    w.bind_vertex_buffer(2);
    w.push_constants(floats(identity, 16));
    w.bind_texture(0, 1, graphics::SamplerDesc());
    w.draw(3);
    w.end_pass();
    return w.ok() ? e.render(w, {target ? target : rt, tex, vb}) : status::kNoMemory;
  };
  // Drawing into a target that is in the common state: refused. Moved to render target: fine.
  CHECK(frame([&](CommandWriter&) {}) == status::kInvalidParameter);
  // (a refused stream leaves the states as they were: the transition below still starts from the common state)
  CHECK(frame([&](CommandWriter& w) { w.transition(0, state::kCommon, state::kRenderTarget); w.transition(1, state::kCommon, state::kPixelShaderResource); }) == status::kInvalidParameter);   // (the texture is not in common)
  AllocationInfo info;
  CHECK(frame([&](CommandWriter& w) { w.transition(0, state::kCommon, state::kRenderTarget); }) == status::kSuccess && e.pixel(rt, 4, 4) == 0xFFC08040);
  // It stays a render target in the next stream: no barrier needed again, and a transition from the common state is now wrong.
  CHECK(frame([&](CommandWriter&) {}) == status::kSuccess);
  CHECK(frame([&](CommandWriter& w) { w.transition(0, state::kCommon, state::kRenderTarget); }) == status::kInvalidParameter);
  // Reading it as a texture needs another transition; a render target cannot be sampled while it is one. Back to the common state it can be drawn into no more.
  CHECK(frame([&](CommandWriter& w) { w.transition(0, state::kRenderTarget, state::kPixelShaderResource); }) == status::kInvalidParameter);
  CHECK(frame([&](CommandWriter& w) { w.transition(0, state::kRenderTarget, state::kCommon); }) == status::kInvalidParameter);
  // Rules of a barrier: before and after the same, a state that writes with another, an allocation that is not in the list, a state that does not exist, a split barrier.
  CHECK(frame([&](CommandWriter& w) { w.transition(2, state::kGenericRead, state::kGenericRead); }) == status::kInvalidParameter);
  CHECK(frame([&](CommandWriter& w) { w.transition(2, state::kGenericRead, state::kCopyDest | state::kCopySource); }) == status::kInvalidParameter);
  CHECK(frame([&](CommandWriter& w) { w.transition(9, state::kGenericRead, state::kCopyDest); }) == status::kInvalidParameter);
  CHECK(frame([&](CommandWriter& w) { w.transition(2, state::kGenericRead, 0x8000); }) == status::kInvalidParameter);
  CHECK(frame([&](CommandWriter& w) { w.resource_barrier({CommandWriter::Barrier{0, 2, state::kGenericRead, state::kCopyDest, 1}, CommandWriter::Barrier{0, 2, state::kGenericRead, state::kCopyDest, 2}}); w.transition(2, state::kCopyDest, state::kGenericRead); }) == status::kSuccess);
  // Unordered access and aliasing barriers are accepted (the first only for something in that state, or nothing in particular).
  CHECK(frame([&](CommandWriter& w) { w.resource_barrier({CommandWriter::Barrier{1, CommandWriter::Barrier::kNoAllocationIndex, 0, 0, 0}, CommandWriter::Barrier{2, 0, CommandWriter::Barrier::kNoAllocationIndex, 0, 0}}); }) == status::kSuccess);
  CHECK(frame([&](CommandWriter& w) { w.resource_barrier({CommandWriter::Barrier{1, 0, 0, 0, 0}}); }) == status::kInvalidParameter);   // (the target is not in the unordered access state)
  // An allocation that does not track states takes barriers as no-ops and is used in any state.
  CHECK(frame([&](CommandWriter&) {}, plain_rt) == status::kSuccess);
  // A texture bound while it is in no state that reads: refused. Moved to copy destination: not a shader resource.
  CHECK(frame([&](CommandWriter& w) { w.transition(1, state::kPixelShaderResource, state::kCopyDest); }) == status::kInvalidParameter);
  CHECK(e.kernel.query_allocation(kPid, tex, &info) == status::kSuccess);
}


// Resource heaps: allocations placed in one block of memory, overlapping, with the GPU's contents carried across by aliasing barriers.
void test_d3d12_heaps() {
  Env e;
  auto create = [&](uint32_t flags, uint64_t size, Handle heap, uint64_t offset, Handle* out) {
    AllocationDesc d;
    d.flags = flags, d.size = size, d.heap = heap, d.heap_offset = offset;
    return e.kernel.create_allocation(kPid, e.dev, d, out);
  };
  Handle heap = 0, none = 0;
  CHECK(create(kAllocHeap, 4096, 0, 0, &heap) == status::kSuccess);
  CHECK(create(kAllocHeap, 100, 0, 0, &none) == status::kInvalidParameter && create(kAllocHeap, 0, 0, 0, &none) == status::kInvalidParameter);
  CHECK(create(kAllocHeap | kAllocVertexBuffer, 4096, 0, 0, &none) == status::kInvalidParameter && create(kAllocHeap, 4096, heap, 0, &none) == status::kInvalidParameter);
  const uint64_t before = e.kernel.adapter(0)->vidmm().system_bytes();
  // Buffers, overlapping: 0 .. 256 and 128 .. 384 share 128 bytes; the third is apart.
  Handle first = 0, second = 0, apart = 0;
  CHECK(create(kAllocVertexBuffer, 256, heap, 0, &first) == status::kSuccess && create(kAllocVertexBuffer, 256, heap, 128, &second) == status::kSuccess && create(kAllocVertexBuffer, 256, heap, 1024, &apart) == status::kSuccess);
  CHECK(e.kernel.adapter(0)->vidmm().system_bytes() == before);   // (the memory is the heap's: not counted again)
  uint8_t pattern[256];
  for (uint32_t i = 0; i < 256; ++i) pattern[i] = static_cast<uint8_t>(i + 1);
  CHECK(e.write(first, ConstByteView(pattern, 256)));
  LockResult lock;
  CHECK(e.kernel.lock(kPid, second, kLockReadOnly, 0, &lock) == status::kSuccess);
  bool same = lock.data.size() == 256;
  for (uint32_t i = 0; i < 128 && same; ++i) same = lock.data[i] == pattern[128 + i];
  for (uint32_t i = 128; i < 256 && same; ++i) same = lock.data[i] == 0;   // (past the end of the first: the heap's own zeros)
  CHECK(same);
  e.kernel.unlock(kPid, second);
  CHECK(e.kernel.lock(kPid, apart, kLockReadOnly, 0, &lock) == status::kSuccess && lock.data[0] == 0);
  e.kernel.unlock(kPid, apart);
  // What does not go: past the end, misaligned, a handle that is not a heap, a heap of nothing, shared, a depth image, a multisampled one.
  CHECK(create(kAllocVertexBuffer, 256, heap, 3968 + 16, &none) == status::kInvalidParameter && create(kAllocVertexBuffer, 256, heap, 8, &none) == status::kInvalidParameter);
  CHECK(create(kAllocVertexBuffer, 256, first, 0, &none) == status::kInvalidParameter && create(kAllocVertexBuffer, 256, 0x7777, 0, &none) == status::kInvalidHandle);
  CHECK(create(kAllocVertexBuffer | kAllocShared, 256, heap, 0, &none) == status::kInvalidParameter);
  AllocationDesc depth;
  depth.flags = kAllocDepthStencil, depth.format = Format::D32Float, depth.width = depth.height = 8, depth.heap = heap;
  CHECK(e.kernel.create_allocation(kPid, e.dev, depth, &none) == status::kNotSupported);
  // The GPU: a render target A and a texture B at the same place. Draw red into A, pass the use to B with an aliasing barrier, and sample B.
  AllocationDesc id;
  id.flags = kAllocRenderTarget, id.format = Format::A8R8G8B8, id.width = id.height = 8, id.heap = heap, id.heap_offset = 2048;
  Handle a_target = 0, b_texture = 0;
  CHECK(e.kernel.create_allocation(kPid, e.dev, id, &a_target) == status::kSuccess);
  id.flags = kAllocTexture;
  CHECK(e.kernel.create_allocation(kPid, e.dev, id, &b_texture) == status::kSuccess);
  Handle target = e.alloc(kTarget, Format::A8R8G8B8, 8, 8), vb = e.buffer(kAllocVertexBuffer, 4 * 28), tvb = e.buffer(kAllocVertexBuffer, 4 * 20);
  Handle red_pipe = make_colored_pipeline(e, false), tex_pipe = make_colored_pipeline(e, true);
  CHECK(write_triangle(e, vb, 1, 0, 0));
  const float quad[20] = {-1, -1, 0.5f, 0, 0, 3, -1, 0.5f, 2, 0, -1, 3, 0.5f, 0, 2};
  CHECK(e.write(tvb, floats(quad, 15)));
  const float identity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
  auto run = [&](bool barrier) {
    CommandWriter w(e.allocator);
    w.begin_pass(0, kNoAllocation, kPassClearColor, {0, 0, 0, 1}, 1, 0);   // (list: A 0, B 1, target 2, vb 3, tvb 4)
    w.bind_pipeline(red_pipe);
    w.bind_vertex_buffer(3);
    w.push_constants(floats(identity, 16));
    w.draw(3);
    w.end_pass();
    if (barrier) w.resource_barrier({CommandWriter::Barrier{2, 0, 1, 0, 0}});
    w.begin_pass(2, kNoAllocation, kPassClearColor, {0, 0, 0, 1}, 1, 0);
    w.bind_pipeline(tex_pipe);
    w.bind_vertex_buffer(4);
    w.push_constants(floats(identity, 16));
    w.bind_texture(0, 1, graphics::SamplerDesc());
    w.draw(3);
    w.end_pass();
    return w.ok() ? e.render(w, {a_target, b_texture, target, vb, tvb}) : status::kNoMemory;
  };
  // Without the barrier B is what it was before A was drawn into (nothing); with it, it is A's picture.
  CHECK(run(false) == status::kSuccess && e.pixel(target, 4, 4) != 0xFFFF0000u);
  CHECK(run(true) == status::kSuccess && e.pixel(target, 4, 4) == 0xFFFF0000u);
  // The memory has it: the CPU reads it through B, through A, and through the heap.
  CHECK(e.pixel(b_texture, 2, 2) == 0xFFFF0000u && e.pixel(a_target, 3, 3) == 0xFFFF0000u);
  CHECK(e.kernel.lock(kPid, heap, kLockReadOnly, 0, &lock) == status::kSuccess && lock.data.size() == 4096);
  ByteReader heap_in(ConstByteView(lock.data.data(), lock.data.size()));
  heap_in.skip(2048);
  CHECK(heap_in.le32() == 0xFFFF0000u);
  e.kernel.unlock(kPid, heap);
  // The CPU writing through B reaches A (which is not resident-stale to the stream any more): drawn into again, A is what the stream draws, not what the CPU wrote.
  CHECK(e.fill(b_texture, 0xFF00FF00u));
  CHECK(e.pixel(a_target, 0, 0) == 0xFF00FF00u);
  // Aliasing barriers: not in a pass, only between placed allocations of one heap, and a null side is nothing.
  {
    CommandWriter w(e.allocator);
    w.begin_pass(2, kNoAllocation, 0, {0, 0, 0, 0}, 1, 0);
    w.resource_barrier({CommandWriter::Barrier{2, 0, 1, 0, 0}});
    w.end_pass();
    CHECK(e.render(w, {a_target, b_texture, target}) == status::kInvalidParameter);
  }
  {
    CommandWriter w(e.allocator);
    w.resource_barrier({CommandWriter::Barrier{2, 0, 2, 0, 0}});
    CHECK(e.render(w, {a_target, b_texture, target}) == status::kInvalidParameter);   // (the target is not placed)
    CommandWriter w2(e.allocator);
    w2.resource_barrier({CommandWriter::Barrier{2, 0, CommandWriter::Barrier::kNoAllocationIndex, 0, 0}, CommandWriter::Barrier{2, CommandWriter::Barrier::kNoAllocationIndex, 1, 0, 0}});
    CHECK(e.render(w2, {a_target, b_texture, target}) == status::kSuccess);
  }
  // The heap goes when its handle is destroyed but the allocations placed in it keep its memory.
  CHECK(e.kernel.destroy_allocation(kPid, heap) == status::kSuccess);
  CHECK(e.pixel(b_texture, 0, 0) == 0xFF00FF00u);
}


// --- DXIL (Shader Model 6): the same HLSL compiled by fxc to DXBC and by dxc to DXIL must give the same results -----------------------------------------------------------------

struct DxilTextures final : graphics::ITextureSet {
  graphics::Vec4 sample(uint32_t unit, float u, float v) const override { return {u, v, 0.25f + 0.1f * static_cast<float>(unit), 1.0f}; }
  graphics::Vec4 sample_lod(uint32_t unit, float u, float v, float lod) const override { return {u * 0.5f, v * 0.5f, lod * 0.125f, 0.5f + 0.1f * static_cast<float>(unit)}; }
  graphics::Vec4 fetch(uint32_t unit, int32_t x, int32_t y, uint32_t level) const override { return {x * 0.0625f, y * 0.125f, static_cast<float>(level) * 0.25f, 0.75f + static_cast<float>(unit) * 0.0625f}; }
  bool size(uint32_t, uint32_t level, uint32_t* width, uint32_t* height, uint32_t* levels) const override {
    *width = 8u >> level, *height = 4u >> level, *levels = 3;
    return level < 3;
  }
};

bool close_to(float a, float b) {
  if (a != a || b != b) return a != a && b != b;
  const float scale = 1.0f + (a < 0 ? -a : a);
  return (a - b < 0 ? b - a : a - b) <= 2e-4f * scale;
}

void test_dxil() {
  HostAllocator allocator;
  uint32_t seed = 12345;
  auto random = [&](float low, float high) {
    seed = seed * 1664525u + 1013904223u;
    return low + (high - low) * static_cast<float>(seed >> 8) / static_cast<float>(1u << 24);
  };
  auto load = [&](const char* name, UniquePointer<D3d9Program>* program) {
    const std::vector<uint8_t> blob = read_blob(name);
    const NtStatus st = D3d9Program::compile_dxbc(allocator, ConstByteView(blob.data(), blob.size()), program);
    if (st != status::kSuccess) printf("  %s: compile status %x\n", name, static_cast<unsigned>(st));
    return st == status::kSuccess && *program;
  };
  DxilTextures textures;
  // The pixel shaders: the same varyings, the same colour (and the same pixel killed).
  for (const char* name : {"dxil_ps_math", "dxil_ps_tex", "dxil_ps_flow", "dxil_ps_loop", "dxil_ps_loop2"}) {
    UniquePointer<D3d9Program> reference, translated;
    const std::string base(name);
    CHECK(load((base + ".dxbc").c_str(), &reference) && load((base + ".dxil").c_str(), &translated));
    if (!reference || !translated) continue;
    CHECK(translated->stage() == graphics::ShaderStage::Fragment && translated->color_output_count() == 1);
    unsigned alive_count = 0;
    for (uint32_t n = 0; n < 200; ++n) {
      graphics::Vec4 varyings[4] = {};
      varyings[0] = {random(-1, 2), random(-1, 2), random(-1, 2), random(-1, 2)};
      varyings[2] = {random(0, 1), random(0, 1), 0, 0};
      graphics::FragmentContext context;
      context.position = {random(0, 32), random(0, 32), random(0, 1), 1};
      graphics::Vec4 a_room[graphics::kMaxColorTargets] = {}, b_room[graphics::kMaxColorTargets] = {};
      uint8_t constants[128] = {};
      const bool a_alive = reference->run_fragment_ex(varyings, context, constants, &textures, a_room);
      const bool b_alive = translated->run_fragment_ex(varyings, context, constants, &textures, b_room);
      CHECK(a_alive == b_alive);
      alive_count += a_alive;
      if (a_alive && b_alive) {
        const bool same = close_to(a_room[0].x, b_room[0].x) && close_to(a_room[0].y, b_room[0].y) && close_to(a_room[0].z, b_room[0].z) && close_to(a_room[0].w, b_room[0].w);
        if (!same && n < 3) printf("  %s #%u: %g %g %g %g vs %g %g %g %g\n", name, n, a_room[0].x, a_room[0].y, a_room[0].z, a_room[0].w, b_room[0].x, b_room[0].y, b_room[0].z, b_room[0].w);
        CHECK(same);
      }
    }
    CHECK(alive_count > 20);   // (the test is about colours, not about every pixel being killed)
    printf("  dxil %s: 200 pixels compared, %u not killed\n", name, alive_count);
  }

  // The shader the old compiler cannot make (a switch, signed division, tan and the hyperbolic functions, an early return): compared with the same function in C++.
  {
    UniquePointer<D3d9Program> translated;
    CHECK(load("dxil_ps_more.dxil", &translated));
    if (translated) {
      unsigned early = 0, checked = 0;
      for (uint32_t n = 0; n < 300; ++n) {
        graphics::Vec4 varyings[4] = {};
        varyings[0] = {random(-1, 2), random(-1, 2), random(-1, 2), random(-1, 2)};
        varyings[2] = {n % 7 == 0 ? random(0, 0.1f) : random(0, 1), random(0, 1), 0, 0};
        const float cx = varyings[0].x, cy = varyings[0].y, cz = varyings[0].z, cw = varyings[0].w, tx = varyings[2].x, ty = varyings[2].y;
        graphics::Vec4 room[graphics::kMaxColorTargets] = {};
        uint8_t constants[128] = {};
        const bool alive = translated->run_fragment_ex(varyings, graphics::FragmentContext(), constants, &textures, room);
        float want[4];
        if (tx < 0.1f) {
          want[0] = cx, want[1] = cy, want[2] = cz, want[3] = 0.5f;
          ++early;
        } else {
          const int a = static_cast<int>(cx * 20.0f - 8.0f), b = static_cast<int>(ty * 6.0f) - 3, d = b == 0 ? 1 : b;
          want[0] = static_cast<float>(a / d) + static_cast<float>(a % d) * 0.25f;
          want[1] = __builtin_tanf(cy * 0.5f) + __builtin_coshf(cz * 0.3f) + __builtin_sinhf(cw * 0.3f) + __builtin_tanhf(cx * 0.2f);
          const uint32_t which = static_cast<uint32_t>(ty * 5.0f);
          want[2] = which == 0 ? 1.0f : which <= 2 ? cx * 2.0f : which == 3 ? __builtin_sqrtf(__builtin_fabsf(cy)) : -1.0f;
          want[3] = 1.0f;
        }
        const bool same = alive && close_to(room[0].x, want[0]) && close_to(room[0].y, want[1]) && close_to(room[0].z, want[2]) && close_to(room[0].w, want[3]);
        if (!same && checked < 3) printf("  more #%u: %g %g %g %g wants %g %g %g %g\n", n, room[0].x, room[0].y, room[0].z, room[0].w, want[0], want[1], want[2], want[3]);
        CHECK(same);
        ++checked;
      }
      CHECK(early > 10);
      printf("  dxil dxil_ps_more: 300 pixels compared with C++ (%u early returns)\n", early);
    }
  }

  // gather4 (GatherRed / GatherGreen of a 2D texture): the footprint's four texels in the order x (i0, j1), y (i1, j1), z (i1, j0), w (i0, j0); the fake texture's lookups give the texel
  // middle's coordinates halved, so the golden is computed from the same footprint.
  {
    UniquePointer<D3d9Program> translated;
    CHECK(load("dxil_ps_gather.dxil", &translated));
    if (translated) {
      if (getenv("TGSI_DUMP")) {
        String text(allocator);
        translated->emit_tgsi(&text);
        printf("%.*s\n", static_cast<int>(text.size()), text.data());
      }
      for (uint32_t n = 0; n < 200; ++n) {
        graphics::Vec4 varyings[4] = {};
        varyings[2] = {random(0, 1), random(0, 1), 0, 0};
        graphics::Vec4 room[graphics::kMaxColorTargets] = {};
        uint8_t constants[128] = {};
        const bool alive = translated->run_fragment_ex(varyings, graphics::FragmentContext(), constants, &textures, room);
        auto footprint = [&](float u, float v, float* red, float* green) {
          const float x = __builtin_floorf(u * 8.0f - 0.5f), y = __builtin_floorf(v * 4.0f - 0.5f);
          const float xs[4] = {x, x + 1, x + 1, x}, ys[4] = {y + 1, y + 1, y, y};
          for (uint32_t k = 0; k < 4; ++k) red[k] = (xs[k] + 0.5f) / 16.0f, green[k] = (ys[k] + 0.5f) / 8.0f;
        };
        float g_red[4], g[4], r[4], r_green[4];
        footprint(varyings[2].x, varyings[2].y, g_red, g);
        footprint(varyings[2].x * 0.9f, varyings[2].y * 0.9f, r, r_green);
        const float want[4] = {g[0] + g[3] * 2.0f, g[1] * 0.5f + g[2], r[0] + r[1] * 2.0f, r[2] * 0.25f + r[3]};
        const bool same = alive && close_to(room[0].x, want[0]) && close_to(room[0].y, want[1]) && close_to(room[0].z, want[2]) && close_to(room[0].w, want[3]);
        if (!same && n < 3) printf("  gather #%u: %g %g %g %g wants %g %g %g %g\n", n, room[0].x, room[0].y, room[0].z, room[0].w, want[0], want[1], want[2], want[3]);
        CHECK(same);
      }
    }
  }
  // Derivatives (ddx_fine / ddy_fine / ddx_coarse / ddy_coarse): the varyings are a plane over the quad, so each corner's are the first corner's plus its steps; the golden takes the
  // difference of the same function at the corners of the pair.
  {
    UniquePointer<D3d9Program> translated;
    CHECK(load("dxil_ps_deriv.dxil", &translated));
    if (translated) {
      struct Plane {
        graphics::Vec4 origin[4], step_x[4], step_y[4];
        graphics::Vec4 at(uint32_t i, uint32_t corner) const {
          const float fx = static_cast<float>(corner & 1), fy = static_cast<float>(corner >> 1);
          return {origin[i].x + step_x[i].x * fx + step_y[i].x * fy, origin[i].y + step_x[i].y * fx + step_y[i].y * fy, origin[i].z + step_x[i].z * fx + step_y[i].z * fy,
                  origin[i].w + step_x[i].w * fx + step_y[i].w * fy};
        }
      };
      for (uint32_t n = 0; n < 200; ++n) {
        Plane plane = {};
        for (uint32_t i = 0; i < 4; ++i) {
          plane.origin[i] = {random(-1, 1), random(-1, 1), random(-1, 1), random(-1, 1)};
          plane.step_x[i] = {random(-0.3f, 0.3f), random(-0.3f, 0.3f), random(-0.3f, 0.3f), random(-0.3f, 0.3f)};
          plane.step_y[i] = {random(-0.3f, 0.3f), random(-0.3f, 0.3f), random(-0.3f, 0.3f), random(-0.3f, 0.3f)};
        }
        const uint32_t own = n % 4;
        graphics::FragmentContext context;
        context.quad_corner = own;
        context.quad_user = &plane;
        context.quad_fn = [](const void* user, uint32_t corner, graphics::Vec4* varyings, graphics::Vec4* position) {
          const Plane& p = *static_cast<const Plane*>(user);
          for (uint32_t i = 0; i < 4; ++i) varyings[i] = p.at(i, corner);
          *position = {static_cast<float>(corner & 1), static_cast<float>(corner >> 1), 0, 1};
        };
        graphics::Vec4 varyings[4];
        for (uint32_t i = 0; i < 4; ++i) varyings[i] = plane.at(i, own);
        graphics::Vec4 room[graphics::kMaxColorTargets] = {};
        uint8_t constants[128] = {};
        const bool alive = translated->run_fragment_ex(varyings, context, constants, &textures, room);
        auto f = [&](uint32_t corner) {
          const graphics::Vec4 c = plane.at(0, corner), t = plane.at(2, corner);
          return c.x * c.y + t.x * t.x;
        };
        const uint32_t row = own & 2, col = own & 1;
        const float want[4] = {f(row + 1) - f(row), f(col + 2) - f(col),
                               plane.at(0, 1).z * plane.at(0, 1).z - plane.at(0, 0).z * plane.at(0, 0).z,
                               plane.at(0, 2).w + plane.at(2, 2).y * plane.at(2, 2).y - plane.at(0, 0).w - plane.at(2, 0).y * plane.at(2, 0).y};
        const bool same = alive && close_to(room[0].x, want[0]) && close_to(room[0].y, want[1]) && close_to(room[0].z, want[2]) && close_to(room[0].w, want[3]);
        if (!same && n < 4) printf("  deriv #%u (corner %u): %g %g %g %g wants %g %g %g %g\n", n, own, room[0].x, room[0].y, room[0].z, room[0].w, want[0], want[1], want[2], want[3]);
        CHECK(same);
      }
    }
  }
  // atan, asin and acos (polynomials in the translation): compared with libm within 1e-4.
  {
    UniquePointer<D3d9Program> translated;
    CHECK(load("dxil_ps_inv.dxil", &translated));
    if (translated) {
      auto near = [](float a, float b) { return __builtin_fabsf(a - b) < 1e-4f; };
      for (uint32_t n = 0; n < 300; ++n) {
        graphics::Vec4 varyings[4] = {};
        varyings[0] = {random(0, 1), random(0, 1), random(0, 1), 0};
        if (n == 0) varyings[0] = {0.5f, 0.5f, 0.5f, 0};
        if (n == 1) varyings[0] = {1, 1, 1, 0};
        if (n == 2) varyings[0] = {0, 0, 0, 0};
        graphics::Vec4 room[graphics::kMaxColorTargets] = {};
        uint8_t constants[128] = {};
        const bool alive = translated->run_fragment_ex(varyings, graphics::FragmentContext(), constants, &textures, room);
        const float x = varyings[0].x * 3.0f - 1.5f, y = varyings[0].y * 2.0f - 1.0f, z = varyings[0].z * 8.0f - 4.0f;
        const bool same = alive && near(room[0].x, __builtin_atanf(x)) && near(room[0].y, __builtin_asinf(y)) && near(room[0].z, __builtin_acosf(y)) && near(room[0].w, __builtin_atanf(z));
        if (!same) printf("  inv #%u: %g %g %g %g wants %g %g %g %g\n", n, room[0].x, room[0].y, room[0].z, room[0].w, __builtin_atanf(x), __builtin_asinf(y), __builtin_acosf(y), __builtin_atanf(z));
        CHECK(same);
      }
    }
  }
  // The vertex shader: a matrix and a tint from the constants.
  {
    UniquePointer<D3d9Program> reference, translated;
    CHECK(load("dxil_vs.dxbc", &reference) && load("dxil_vs.dxil", &translated));
    if (reference && translated) {
      CHECK(translated->stage() == graphics::ShaderStage::Vertex);
      for (uint32_t n = 0; n < 100; ++n) {
        graphics::Vec4 attributes[4] = {{random(-1, 1), random(-1, 1), random(-1, 1), 1}, {random(0, 1), random(0, 1), random(0, 1), random(0, 1)}, {}, {}};
        float constants_f[32];
        for (float& c : constants_f) c = random(-2, 2);
        uint8_t constants[128];
        for (uint32_t i = 0; i < 32; ++i) {
          const uint32_t bits = __builtin_bit_cast(uint32_t, constants_f[i]);
          for (uint32_t k = 0; k < 4; ++k) constants[4 * i + k] = static_cast<uint8_t>(bits >> (8 * k));
        }
        graphics::ShaderIo a = {}, b = {};
        reference->run_vertex_ex(attributes, constants, graphics::VertexContext(), &a);
        translated->run_vertex_ex(attributes, constants, graphics::VertexContext(), &b);
        const bool same = close_to(a.position.x, b.position.x) && close_to(a.position.y, b.position.y) && close_to(a.position.z, b.position.z) && close_to(a.position.w, b.position.w) &&
                          close_to(a.varying[0].x, b.varying[0].x) && close_to(a.varying[0].w, b.varying[0].w) && close_to(a.varying[2].x, b.varying[2].x) && close_to(a.varying[2].y, b.varying[2].y);
        if (!same && n < 3) printf("  vs #%u: %g %g %g %g vs %g %g %g %g\n", n, a.position.x, a.position.y, a.position.z, a.position.w, b.position.x, b.position.y, b.position.z, b.position.w);
        CHECK(same);
      }
    }
  }


  // A geometry shader: the vertices it emits and where it cut the strip.
  {
    UniquePointer<D3d9Program> reference, translated;
    CHECK(load("dxil_gs_copy.dxbc", &reference) && load("dxil_gs_copy.dxil", &translated));
    if (reference && translated) {
      CHECK(translated->stage() == graphics::ShaderStage::Geometry && translated->geometry_input() == graphics::GeometryPrimitive::Triangle && translated->geometry_max_vertices() == 6 &&
            translated->geometry_output() == graphics::GeometryTopology::TriangleStrip);
      for (uint32_t n = 0; n < 100; ++n) {
        graphics::GeometryInputData in;
        for (uint32_t v = 0; v < 3; ++v) {
          in.position[v] = {random(-1, 1), random(-1, 1), random(0, 1), random(0.5f, 2)};
          in.varying[v][0] = {random(0, 1), random(0, 1), random(0, 1), random(0, 1)};
          in.varying[v][2] = {random(0, 1), random(0, 1), 0, 0};
        }
        graphics::GeometryOutputData a, b;
        uint8_t constants[128] = {};
        reference->run_geometry(in, constants, &a);
        translated->run_geometry(in, constants, &b);
        bool same = a.count == b.count && a.count == 6;
        for (uint32_t i = 0; same && i < a.count; ++i)
          same = a.restart[i] == b.restart[i] && close_to(a.vertex[i].position.x, b.vertex[i].position.x) && close_to(a.vertex[i].position.y, b.vertex[i].position.y) &&
                 close_to(a.vertex[i].position.w, b.vertex[i].position.w) && close_to(a.vertex[i].varying[0].x, b.vertex[i].varying[0].x) && close_to(a.vertex[i].varying[0].z, b.vertex[i].varying[0].z) &&
                 close_to(a.vertex[i].varying[2].y, b.vertex[i].varying[2].y);
        CHECK(same);
      }
      printf("  dxil dxil_gs_copy: 100 triangles compared\n");
    }
  }

  // A domain shader: the vertex it makes of a patch at a point of the domain.
  {
    UniquePointer<D3d9Program> reference, translated;
    CHECK(load("dxil_ds_dent.dxbc", &reference) && load("dxil_ds_dent.dxil", &translated));
    if (reference && translated) {
      CHECK(translated->stage() == graphics::ShaderStage::Domain && translated->tess_domain() == graphics::TessDomain::Triangle && translated->tess_input_points() == 3);
      for (uint32_t n = 0; n < 100; ++n) {
        graphics::HullOutputData patch;
        for (uint32_t k = 0; k < 3; ++k) {
          patch.control_point[k].position = {random(-1, 1), random(-1, 1), random(0, 1), random(0.5f, 2)};
          patch.control_point[k].varying[0] = {random(0, 1), random(0, 1), random(0, 1), random(0, 1)};
          patch.control_point[k].varying[2] = {random(0, 1), random(0, 1), 0, 0};
        }
        const float u = random(0, 1), v = random(0, 1) * (1 - u), w = 1 - u - v;
        graphics::ShaderIo a = {}, b = {};
        uint8_t constants[128] = {};
        reference->run_domain(patch, u, v, w, constants, &a);
        translated->run_domain(patch, u, v, w, constants, &b);
        CHECK(close_to(a.position.x, b.position.x) && close_to(a.position.y, b.position.y) && close_to(a.position.z, b.position.z) && close_to(a.position.w, b.position.w) &&
              close_to(a.varying[0].x, b.varying[0].x) && close_to(a.varying[0].w, b.varying[0].w) && close_to(a.varying[2].x, b.varying[2].x) && close_to(a.varying[2].y, b.varying[2].y));
      }
      printf("  dxil dxil_ds_dent: 100 domain points compared\n");
    }
  }

  // A hull shader (a control point function and a patch constant function): the control points and the tessellation factors.
  {
    UniquePointer<D3d9Program> reference, translated;
    CHECK(load("dxil_hs_tri.dxbc", &reference) && load("dxil_hs_tri.dxil", &translated));
    if (reference && translated) {
      CHECK(translated->stage() == graphics::ShaderStage::Hull && translated->tess_domain() == graphics::TessDomain::Triangle && translated->tess_input_points() == 3 && translated->tess_output_points() == 3 &&
            translated->tess_partitioning() == reference->tess_partitioning() && translated->tess_output() == reference->tess_output());
      for (uint32_t n = 0; n < 50; ++n) {
        graphics::HullInputData in;
        in.control_points = 3;
        for (uint32_t k = 0; k < 3; ++k) {
          in.position[k] = {random(-1, 1), random(-1, 1), random(0, 1), random(0.5f, 2)};
          in.varying[k][0] = {random(0, 1), random(0, 1), random(0, 1), random(0, 1)};
          in.varying[k][2] = {random(0, 1), random(0, 1), 0, 0};
        }
        graphics::HullOutputData a, b;
        uint8_t constants[128] = {};
        reference->run_hull(in, constants, &a);
        translated->run_hull(in, constants, &b);
        bool same = true;
        for (uint32_t k = 0; k < 3; ++k)
          same = same && close_to(a.control_point[k].position.x, b.control_point[k].position.x) && close_to(a.control_point[k].position.w, b.control_point[k].position.w) &&
                 close_to(a.control_point[k].varying[0].y, b.control_point[k].varying[0].y) && close_to(a.control_point[k].varying[2].x, b.control_point[k].varying[2].x);
        for (uint32_t k = 0; k < 3; ++k) same = same && close_to(a.outer[k], b.outer[k]);
        same = same && close_to(a.inner[0], b.inner[0]) && a.outer[0] == 2.0f && a.inner[0] == 2.0f;
        if (!same && n < 2) printf("  hs #%u: outer %g %g %g inner %g cp0.x %g w %g v0.y %g | outer %g %g %g inner %g cp0.x %g w %g v0.y %g\n", n, a.outer[0], a.outer[1], a.outer[2], a.inner[0], a.control_point[0].position.x, a.control_point[0].position.w, a.control_point[0].varying[0].y, b.outer[0], b.outer[1], b.outer[2], b.inner[0], b.control_point[0].position.x, b.control_point[0].position.w, b.control_point[0].varying[0].y);
        CHECK(same);
      }
      printf("  dxil dxil_hs_tri: 50 patches compared\n");
    }
  }

  // The hull shaders of the other domains and partitionings, with the factors from a constant buffer.
  for (const char* name : {"dxil_hs_quad_cb", "dxil_hs_iso_cb", "dxil_hs_tri_odd_cb"}) {
    UniquePointer<D3d9Program> reference, translated;
    const std::string base(name);
    CHECK(load((base + ".dxbc").c_str(), &reference) && load((base + ".dxil").c_str(), &translated));
    if (!reference || !translated) continue;
    CHECK(translated->tess_domain() == reference->tess_domain() && translated->tess_partitioning() == reference->tess_partitioning() && translated->tess_output() == reference->tess_output() &&
          translated->tess_input_points() == reference->tess_input_points() && translated->tess_output_points() == reference->tess_output_points());
    const uint32_t points = reference->tess_input_points();
    for (uint32_t n = 0; n < 30; ++n) {
      graphics::HullInputData in;
      in.control_points = points;
      for (uint32_t k = 0; k < points; ++k) in.position[k] = {random(-1, 1), random(-1, 1), 0.5f, 1}, in.varying[k][0] = {random(0, 1), 0, 0, 1};
      uint8_t constants[128] = {};
      for (uint32_t i = 16; i < 24; ++i) {
        const float f = random(1, 9);
        const uint32_t bits = __builtin_bit_cast(uint32_t, f);
        for (uint32_t b = 0; b < 4; ++b) constants[4 * i + b] = static_cast<uint8_t>(bits >> (8 * b));
      }
      graphics::HullOutputData a, b;
      reference->run_hull(in, constants, &a);
      translated->run_hull(in, constants, &b);
      bool same = true;
      for (uint32_t k = 0; k < 4; ++k) same = same && close_to(a.outer[k], b.outer[k]);
      for (uint32_t k = 0; k < 2; ++k) same = same && close_to(a.inner[k], b.inner[k]);
      for (uint32_t k = 0; k < points; ++k) same = same && close_to(a.control_point[k].position.x, b.control_point[k].position.x);
      if (!same && n < 2) printf("  %s #%u: outer %g %g %g %g inner %g %g | %g %g %g %g %g %g\n", name, n, a.outer[0], a.outer[1], a.outer[2], a.outer[3], a.inner[0], a.inner[1], b.outer[0], b.outer[1], b.outer[2], b.outer[3], b.inner[0], b.inner[1]);
      CHECK(same);
    }
    printf("  dxil %s: 30 patches compared\n", name);
  }
  // The compute shaders: the same buffers in, the same buffers out.
  {
    UniquePointer<D3d9Program> reference, translated;
    CHECK(load("dxil_cs_fill.dxbc", &reference) && load("dxil_cs_fill.dxil", &translated));
    if (reference && translated) {
      CHECK(translated->stage() == graphics::ShaderStage::Compute && translated->thread_group(0) == 8 && translated->thread_group(1) == 4);
      auto run = [&](D3d9Program& p, std::vector<uint8_t>* picture, std::vector<uint8_t>* structured, std::vector<uint8_t>* counters) {
        picture->assign(4096, 0), structured->assign(16384, 0), counters->assign(16, 0);
        graphics::ComputeContext context;
        context.buffers[0] = ByteView(picture->data(), picture->size()), context.buffers[1] = ByteView(structured->data(), structured->size()), context.buffers[2] = ByteView(counters->data(), counters->size());
        const uint32_t k[4] = {0x00102030u, 0, 0, 0};
        uint8_t constants[128] = {};
        for (uint32_t i = 0; i < 4; ++i)
          for (uint32_t b = 0; b < 4; ++b) constants[4 * i + b] = static_cast<uint8_t>(k[i] >> (8 * b));
        for (uint32_t gy = 0; gy < 8; ++gy)
          for (uint32_t gx = 0; gx < 4; ++gx) {
            uint32_t index = 0;
            for (uint32_t ty = 0; ty < 4; ++ty)
              for (uint32_t tx = 0; tx < 8; ++tx, ++index) {
                context.group_id[0] = gx, context.group_id[1] = gy, context.thread_id[0] = tx, context.thread_id[1] = ty;
                context.dispatch_id[0] = gx * 8 + tx, context.dispatch_id[1] = gy * 4 + ty;
                context.group_index = index;
                p.run_compute(context, constants);
              }
          }
      };
      std::vector<uint8_t> a0, a1, a2, b0, b1, b2;
      run(*reference, &a0, &a1, &a2);
      run(*translated, &b0, &b1, &b2);
      CHECK(a0 == b0 && a1 == b1 && a2 == b2);
      CHECK(b0[0] != 0 && b2[0] == 0 && b2[1] == 4);   // (the counters reached 1024: 0x400)
      printf("  dxil dxil_cs_fill: %s\n", a0 == b0 && a1 == b1 && a2 == b2 ? "same buffers" : "buffers differ");
    }
  }

  // Shader resource views of buffers (t#: read-only storage buffers after the u# ones).
  {
    UniquePointer<D3d9Program> translated;
    CHECK(load("dxil_cs_srv.dxil", &translated));
    if (translated) {
      CHECK(translated->storage_kind(graphics::kMaxUavBuffers) == D3d9Program::BufferKind::Structured && translated->storage_kind(graphics::kMaxUavBuffers + 1) == D3d9Program::BufferKind::Raw &&
            translated->storage_kind(0) == D3d9Program::BufferKind::Structured);
      std::vector<uint32_t> src(1024 * 4), raw(1024), dst(1024, 0);
      for (uint32_t i = 0; i < src.size(); ++i) src[i] = 0x01010101u * (i % 251) + i * 7;
      for (uint32_t i = 0; i < raw.size(); ++i) raw[i] = 0x9E3779B9u * (i + 1);
      graphics::ComputeContext context;
      context.buffers[0] = ByteView(reinterpret_cast<uint8_t*>(dst.data()), dst.size() * 4);
      context.buffers[graphics::kMaxUavBuffers] = ByteView(reinterpret_cast<uint8_t*>(src.data()), src.size() * 4);
      context.buffers[graphics::kMaxUavBuffers + 1] = ByteView(reinterpret_cast<uint8_t*>(raw.data()), raw.size() * 4);
      uint8_t constants[128] = {};
      for (uint32_t y = 0; y < 32; ++y)
        for (uint32_t x = 0; x < 32; ++x) {
          context.thread_id[0] = x % 8, context.thread_id[1] = y % 4, context.dispatch_id[0] = x, context.dispatch_id[1] = y, context.group_index = (y % 4) * 8 + x % 8;
          translated->run_compute(context, constants);
        }
      bool same = true;
      for (uint32_t i = 0; i < 1024; ++i) same = same && dst[i] == src[4 * i] + src[4 * i + 3] * 3 + raw[i] + (raw[1023 - i] ^ src[4 * i + 1]);
      CHECK(same);
      printf("  dxil dxil_cs_srv: %s\n", same ? "buffer views read" : "buffer views differ");
    }
  }

  // A compute shader with group-shared memory and barriers (a reduction in a loop): the buffer after every group has run.
  {
    UniquePointer<D3d9Program> reference, translated;
    CHECK(load("dxil_cs_shared.dxbc", &reference) && load("dxil_cs_shared.dxil", &translated));
    if (reference && translated) {
      CHECK(translated->needs_group() && translated->shared_total_bytes() == 256);
      auto run = [&](D3d9Program& p, std::vector<uint8_t>* picture) {
        picture->assign(4096, 0);
        graphics::ComputeContext context;
        context.buffers[0] = ByteView(picture->data(), picture->size());
        for (uint32_t gy = 0; gy < 4; ++gy)
          for (uint32_t gx = 0; gx < 4; ++gx) {
            context.group_id[0] = gx, context.group_id[1] = gy;
            p.run_compute_group(context, nullptr);
          }
      };
      std::vector<uint8_t> a, b;
      run(*reference, &a);
      run(*translated, &b);
      CHECK(a == b);
      printf("  dxil dxil_cs_shared: %s\n", a == b ? "same buffer" : "buffers differ");
    }
  }
  // Truncated and with a bit flipped: refused or compiled, never a crash (and the sanitizers say nothing).
  for (const char* name : {"dxil_ps_math.dxil", "dxil_ps_flow.dxil", "dxil_vs.dxil", "dxil_ps_tex.dxil", "dxil_cs_fill.dxil", "dxil_ps_loop.dxil", "dxil_cs_shared.dxil", "dxil_ps_more.dxil", "dxil_gs_copy.dxil", "dxil_ds_dent.dxil", "dxil_hs_tri.dxil", "dxil_hs_quad_cb.dxil", "dxil_hs_iso_cb.dxil"}) {
    const std::vector<uint8_t> blob = read_blob(name);
    for (size_t n = 0; n < blob.size(); n += 11) {
      UniquePointer<D3d9Program> program;
      D3d9Program::compile_dxbc(allocator, ConstByteView(blob.data(), n), &program);
    }
    const size_t stride = getenv("DXIL_FUZZ_STRIDE") ? static_cast<size_t>(atoi(getenv("DXIL_FUZZ_STRIDE"))) : 37;   // (every bit when looking for something)
    for (size_t bit = 0; bit < blob.size() * 8; bit += stride) {
      std::vector<uint8_t> flipped = blob;
      flipped[bit / 8] ^= static_cast<uint8_t>(1u << (bit % 8));
      UniquePointer<D3d9Program> program;
      D3d9Program::compile_dxbc(allocator, ConstByteView(flipped.data(), flipped.size()), &program);
    }
  }
}


// Reserved (tiled) buffers: tiles mapped to places in heaps.
void test_d3d12_reserved() {
  Env e;
  auto create = [&](uint32_t flags, uint64_t size, Handle heap, uint64_t offset, Handle* out) {
    AllocationDesc d;
    d.flags = flags, d.size = size, d.heap = heap, d.heap_offset = offset;
    return e.kernel.create_allocation(kPid, e.dev, d, out);
  };
  Handle heap = 0, p = 0, q = 0, r = 0, none = 0;
  CHECK(create(kAllocHeap, 4 * kTileBytes, 0, 0, &heap) == status::kSuccess);
  CHECK(create(kAllocVertexBuffer, kTileBytes, heap, 0, &p) == status::kSuccess && create(kAllocVertexBuffer, kTileBytes, heap, 2 * kTileBytes, &q) == status::kSuccess);
  CHECK(create(kAllocVertexBuffer | kAllocReserved, 3 * kTileBytes, 0, 0, &r) == status::kSuccess);
  CHECK(create(kAllocVertexBuffer | kAllocReserved, 3 * kTileBytes + 4, 0, 0, &none) == status::kInvalidParameter && create(kAllocVertexBuffer | kAllocReserved, 0, 0, 0, &none) == status::kInvalidParameter);
  CHECK(create(kAllocQueryResult | kAllocReserved, kTileBytes, 0, 0, &none) == status::kInvalidParameter && create(kAllocHeap | kAllocReserved, kTileBytes, 0, 0, &none) == status::kInvalidParameter);
  CHECK(create(kAllocVertexBuffer | kAllocReserved, kTileBytes, heap, 0, &none) == status::kInvalidParameter);   // (reserved and placed)
  AllocationDesc image;
  image.flags = kAllocTexture | kAllocReserved, image.format = Format::A8R8G8B8, image.width = image.height = 8;
  CHECK(e.kernel.create_allocation(kPid, e.dev, image, &none) == status::kInvalidParameter);
  // Mapping: tile 0 to the start of the heap, tile 1 to its third tile, tile 2 nowhere.
  CHECK(e.kernel.update_tile_mappings(kPid, r, 0, heap, 0, 1) == status::kSuccess && e.kernel.update_tile_mappings(kPid, r, 1, heap, 2 * kTileBytes, 1) == status::kSuccess);
  CHECK(e.kernel.update_tile_mappings(kPid, r, 2, 0, 0, 1) == status::kSuccess);
  CHECK(e.kernel.update_tile_mappings(kPid, r, 3, heap, 0, 1) == status::kInvalidParameter && e.kernel.update_tile_mappings(kPid, r, 0, heap, 100, 1) == status::kInvalidParameter);
  CHECK(e.kernel.update_tile_mappings(kPid, r, 0, heap, 3 * kTileBytes, 2) == status::kInvalidParameter && e.kernel.update_tile_mappings(kPid, p, 0, heap, 0, 1) == status::kInvalidParameter);
  CHECK(e.kernel.update_tile_mappings(kPid, r, 0, 0x7777, 0, 1) == status::kInvalidHandle);
  std::vector<uint8_t> a(kTileBytes), b(kTileBytes);
  for (uint32_t i = 0; i < kTileBytes; ++i) a[i] = static_cast<uint8_t>(i % 251 + 1), b[i] = static_cast<uint8_t>(i % 241 + 2);
  CHECK(e.write(p, ConstByteView(a.data(), a.size())) && e.write(q, ConstByteView(b.data(), b.size())));
  LockResult lock;
  CHECK(e.kernel.lock(kPid, r, kLockReadOnly, 0, &lock) == status::kSuccess && lock.data.size() == 3 * kTileBytes);
  bool good = true;
  for (uint32_t i = 0; i < kTileBytes && good; ++i) good = lock.data[i] == a[i] && lock.data[kTileBytes + i] == b[i] && lock.data[2 * kTileBytes + i] == 0;   // (what is mapped, and nowhere: zeros)
  CHECK(good);
  e.kernel.unlock(kPid, r);
  // Writes through the reserved buffer reach the heap (and its placed allocations); those to an unmapped tile are dropped.
  CHECK(e.kernel.lock(kPid, r, 0, 0, &lock) == status::kSuccess);
  lock.data[kTileBytes + 100] = 0x5A;
  lock.data[2 * kTileBytes + 7] = 0x77;
  e.kernel.unlock(kPid, r);
  CHECK(e.kernel.lock(kPid, q, kLockReadOnly, 0, &lock) == status::kSuccess && lock.data[100] == 0x5A);
  e.kernel.unlock(kPid, q);
  CHECK(e.kernel.lock(kPid, r, kLockReadOnly, 0, &lock) == status::kSuccess && lock.data[2 * kTileBytes + 7] == 0);
  e.kernel.unlock(kPid, r);
  // The other way: what is written to the heap through a placed allocation shows through the reserved one.
  a[5] = 0xEE;
  CHECK(e.write(p, ConstByteView(a.data(), a.size())));
  CHECK(e.kernel.lock(kPid, r, kLockReadOnly, 0, &lock) == status::kSuccess && lock.data[5] == 0xEE);
  e.kernel.unlock(kPid, r);
  // The GPU reads it: a triangle written through the placed allocation is drawn from the reserved buffer.
  Handle target = e.alloc(kTarget, Format::A8R8G8B8, 8, 8);
  Handle pipe = make_colored_pipeline(e, false);
  CHECK(write_triangle(e, p, 0, 0, 1));
  const float identity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
  CommandWriter w(e.allocator);
  w.begin_pass(0, kNoAllocation, kPassClearColor, {0, 0, 0, 1}, 1, 0);
  w.bind_pipeline(pipe);
  w.bind_vertex_buffer(1);
  w.push_constants(floats(identity, 16));
  w.draw(3);
  w.end_pass();
  CHECK(e.render(w, {target, r}) == status::kSuccess && e.pixel(target, 4, 4) == 0xFF0000FFu);
  // Unmapped, the same draw gets zero vertices (nothing drawn).
  CHECK(e.kernel.update_tile_mappings(kPid, r, 0, 0, 0, 1) == status::kSuccess);
  CommandWriter w2(e.allocator);
  w2.begin_pass(0, kNoAllocation, kPassClearColor, {0, 0, 0, 1}, 1, 0);
  w2.bind_pipeline(pipe);
  w2.bind_vertex_buffer(1);
  w2.push_constants(floats(identity, 16));
  w2.draw(3);
  w2.end_pass();
  CHECK(e.render(w2, {target, r}) == status::kSuccess && e.pixel(target, 4, 4) == 0xFF000000u);
}

// Geometry shaders with adjacency input and instances.
void test_geometry_extras() {
  HostAllocator allocator;
  for (const char* name : {"gs_copy_inst2.dxbc", "gs_tri_adj.dxbc", "gs_line_adj.dxbc"}) {
    const std::vector<uint8_t> blob = read_blob(name);
    UniquePointer<D3d9Program> program;
    CHECK(D3d9Program::compile_dxbc(allocator, ConstByteView(blob.data(), blob.size()), &program) == status::kSuccess && program);
    if (!program) continue;
    String text(allocator);
    CHECK(program->emit_tgsi(&text) == status::kSuccess);
    const std::string tgsi(text.view().data(), text.size());
    if (__builtin_strstr(name, "inst2")) {
      CHECK(program->geometry_instances() == 2 && tgsi.find("GS_INVOCATIONS 2") != std::string::npos);
    } else {
      CHECK(program->geometry_instances() == 1 && tgsi.find(__builtin_strstr(name, "tri") ? "TRIANGLES_ADJACENCY" : "LINES_ADJACENCY") != std::string::npos);
      CHECK(program->geometry_input() == (__builtin_strstr(name, "tri") ? graphics::GeometryPrimitive::TriangleAdjacency : graphics::GeometryPrimitive::LineAdjacency));
    }
    // Truncated and bit-flipped: refused or compiled, never a crash.
    for (size_t n = 0; n < blob.size(); n += 7) {
      UniquePointer<D3d9Program> cut;
      D3d9Program::compile_dxbc(allocator, ConstByteView(blob.data(), n), &cut);
    }
    for (size_t bit = 0; bit < blob.size() * 8; bit += 61) {
      std::vector<uint8_t> flipped = blob;
      flipped[bit / 8] ^= static_cast<uint8_t>(1u << (bit % 8));
      UniquePointer<D3d9Program> bad;
      D3d9Program::compile_dxbc(allocator, ConstByteView(flipped.data(), flipped.size()), &bad);
    }
  }
}

// Alpha to coverage on the CPU renderer: a quad of whole pixels keeps round(alpha * 4) of the 4 samples, so the resolved red is that many quarters (a GPU dithers fractions from pixel to pixel,
// which the conformance scene therefore leaves out).
void test_alpha_to_coverage() {
  Env e;
  namespace b = conformance_blobs;
  Handle vs = 0, ps = 0, pipe = 0;
  ShaderDesc sd;
  sd.kind = ShaderKind::Dxbc, sd.stage = graphics::ShaderStage::Vertex, sd.code = ConstByteView(b::k_vs, sizeof b::k_vs);
  CHECK(e.kernel.create_shader(kPid, e.dev, sd, &vs) == status::kSuccess);
  sd.stage = graphics::ShaderStage::Fragment, sd.code = ConstByteView(b::k_ps_color, sizeof b::k_ps_color);
  CHECK(e.kernel.create_shader(kPid, e.dev, sd, &ps) == status::kSuccess);
  PipelineDesc pd;
  pd.vertex_shader = vs, pd.fragment_shader = ps, pd.vertex_stride = 28, pd.attribute_count = 2, pd.varying_count = 3;
  pd.attributes[0] = {0, graphics::VertexFormat::Float3, 0};
  pd.attributes[1] = {1, graphics::VertexFormat::Float4, 12};
  pd.cull_mode = graphics::CullMode::None;
  pd.multisample = true;
  pd.alpha_to_coverage = true;
  CHECK(e.kernel.create_pipeline(kPid, e.dev, pd, &pipe) == status::kSuccess);
  AllocationDesc md;
  md.flags = kTarget, md.format = Format::A8R8G8B8, md.width = md.height = 32, md.samples = 4;
  Handle multisampled = 0;
  CHECK(e.kernel.create_allocation(kPid, e.dev, md, &multisampled) == status::kSuccess);
  Handle target = e.alloc(kTarget, Format::A8R8G8B8, 32, 32), vb = e.buffer(kAllocVertexBuffer, 6 * 28);
  const float alphas[7] = {0.0f, 0.1f, 0.3f, 0.5f, 0.7f, 0.9f, 1.0f};
  const uint32_t expected[7] = {0, 0, 1, 2, 3, 4, 4};   // (0.1 * 4 = 0.4 rounds to 0; 0.3 * 4 = 1.2 to 1; 0.5 * 4 = 2; 0.7 * 4 = 2.8 to 3; 0.9 * 4 = 3.6 to 4)
  const float identity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
  for (uint32_t i = 0; i < 7; ++i) {
    const float a = alphas[i];
    const float v[42] = {-1, -1, 0.5f, 1, 0, 0, a, 1, -1, 0.5f, 1, 0, 0, a, -1, 1, 0.5f, 1, 0, 0, a, -1, 1, 0.5f, 1, 0, 0, a, 1, -1, 0.5f, 1, 0, 0, a, 1, 1, 0.5f, 1, 0, 0, a};
    CHECK(e.write(vb, floats(v, 42)));
    CommandWriter w(e.allocator);
    w.begin_pass(0, kNoAllocation, kPassClearColor, {0, 0, 0, 1}, 1.0f, 0);
    w.bind_pipeline(pipe);
    w.bind_vertex_buffer(2);
    w.push_constants(floats(identity, 16));
    w.draw(6);
    w.end_pass();
    w.resolve_region(1, 0);
    CHECK(e.render(w, {multisampled, target, vb}) == status::kSuccess);
    const uint32_t red = (e.pixel(target, 5, 5) >> 16) & 0xFF;
    CHECK(red == expected[i] * 255 / 4 || red == (expected[i] * 255 + 2) / 4);
  }
}

void test_conformance() {
  test_half_floats();
  HostAllocator allocator;
  FakeScreen screen(8, 6);
  graphics::SoftwareDevice device(allocator, &screen), reference(allocator, &screen);
  Kernel kernel(allocator);
  AdapterConfig c;
  c.display = &screen;
  CHECK(kernel.add_adapter(device, c) == 0 && kernel.add_adapter(reference, c) == 1);
  ConformanceLog log;
  const uint32_t bad = run_conformance(allocator, kernel, 0, 1, [](void* context, const SceneResult& r) {
    ConformanceLog& l = *static_cast<ConformanceLog*>(context);
    switch (r.verdict) {
      case SceneVerdict::Same: ++l.same; break;
      case SceneVerdict::Differs: ++l.differs; printf("  conformance: %s differs in %u pixels\n", r.name, r.bad_pixels); break;
      case SceneVerdict::Refused: ++l.refused; printf("  conformance: %s refused at %s\n", r.name, r.step); break;
      case SceneVerdict::Broken: ++l.broken; printf("  conformance: %s broken at %s: %x\n", r.name, r.step, static_cast<unsigned>(r.status)); break;
    }
    CHECK(r.device_picture && r.reference_picture && !r.reference_failed);
    if (__builtin_strstr(r.name, "float render target")) {
      // Where the two triangles overlap the target holds 2.5 in red (x 0.4 = 1.0); a clamping target would hold 1.0 there (x 0.4 = 0.4). One triangle alone gives 1.5 (x 0.4 = 0.6).
      uint32_t bright = 0, single = 0;
      for (uint32_t i = 0; i < 32 * 32; ++i) {
        const uint32_t red = (r.device_picture[i] >> 16) & 0xFF;
        if (red >= 240) ++bright;
        if (red >= 140 && red <= 165) ++single;
      }
      CHECK(bright >= 20 && single >= 20);
    }
    {
      auto px = [&](uint32_t x, uint32_t y) { return r.device_picture[y * 32 + x] & 0xFFFFFF; };
      if (__builtin_strstr(r.name, "volume texture")) {
        // One slice (the third, blue) over the half where the depth is fixed; where it runs, the four slices in turn (yellow, blue, green, red going down the picture), each shaded
        // from the left (dark) to the right (light) by the gradient inside the slice.
        CHECK(px(0, 0) == 0x585800 && px(0, 4) == 0x000058 && px(0, 8) == 0x005800 && px(0, 12) == 0x580000 && px(30, 12) == 0xdc0000);
        CHECK(px(0, 16) == 0x000040 && px(30, 16) == 0x0000a1);
      }
      if (__builtin_strstr(r.name, "texture array")) CHECK(px(2, 0) == 0x402000 && px(12, 0) == 0x004020 && px(22, 0) == 0x200040);   // layers 0, 1, 2
      if (__builtin_strstr(r.name, "cube texture"))
        CHECK(px(2, 16) == 0xfa0000 && px(12, 16) == 0x006464 && px(22, 16) == 0x004000 && px(2, 0) == 0x640064 && px(12, 0) == 0x000064 && px(22, 0) == 0xfafa00);   // +X -X +Y, -Y +Z -Z
    }
    if (__builtin_strstr(r.name, "independent blend")) {
      // In the overlap target 0 (even columns) holds both triangles added to the grey (red 0.1 + 0.5 + 0.1 ~ 0xb4), target 1 (odd ones) the second triangle alone, red and blue
      // swapped (0.3, 0.4, 0.1 ~ 4d 66 1a); where only the first triangle is, target 0 shows 9a 34 34 and target 1 its swap.
      auto near = [](uint32_t a, uint32_t b) {
        for (uint32_t shift = 0; shift < 24; shift += 8) {
          const int32_t d = static_cast<int32_t>((a >> shift) & 0xFF) - static_cast<int32_t>((b >> shift) & 0xFF);
          if (d > 3 || d < -3) return false;
        }
        return true;
      };
      CHECK(near(r.device_picture[16 * 32 + 12] & 0xFFFFFF, 0xb49a81) && near(r.device_picture[16 * 32 + 13] & 0xFFFFFF, 0x4d661a));
      CHECK(near(r.device_picture[28 * 32 + 8] & 0xFFFFFF, 0x9a3434) && near(r.device_picture[28 * 32 + 9] & 0xFFFFFF, 0x1a1a80));
    }
    if (__builtin_strstr(r.name, "geometry shader")) {
      // The triangle itself (red at the lower left, blue at the top) and its copy at half the size to the lower right with red and blue swapped.
      auto px = [&](uint32_t x, uint32_t y) { return r.device_picture[y * 32 + x] & 0xFFFFFF; };
      auto near = [](uint32_t a, uint32_t b) {
        for (uint32_t shift = 0; shift < 24; shift += 8) {
          const int32_t d = static_cast<int32_t>((a >> shift) & 0xFF) - static_cast<int32_t>((b >> shift) & 0xFF);
          if (d > 3 || d < -3) return false;
        }
        return true;
      };
      CHECK(near(px(4, 28), 0xdd1111) && near(px(14, 8), 0x2c11c2) && near(px(24, 28), 0x30705f) && px(24, 8) == 0 && px(0, 0) == 0);
    }
    {
      auto px = [&](uint32_t x, uint32_t y) { return r.device_picture[y * 32 + x]; };
      auto near = [](uint32_t a, uint32_t b, int32_t limit = 2) {
        for (uint32_t shift = 0; shift < 32; shift += 8) {
          const int32_t d = static_cast<int32_t>((a >> shift) & 0xFF) - static_cast<int32_t>((b >> shift) & 0xFF);
          if (d > limit || d < -limit) return false;
        }
        return true;
      };
      if (__builtin_strstr(r.name, "sRGB")) {
        // The stored image (even columns): the base colour (0.2, 0.5, 0.8) encoded, 7c bc e7; where the half transparent orange (1, 0.5, 0) lies over it the blend was done on linear
        // values (0.6, 0.5, 0.4) and then encoded: cb bc aa. The sampled image (odd columns) decodes it again: the blue channel, which the shader's colour leaves alone, is 0.8 * 255.
        CHECK(px(2, 12) == 0xFF7CBCE7 && near(px(8, 12), 0xFFCBBCAA) && px(0, 12) == 0xFF000000 && near(px(3, 12) & 0xFF, 0xCC, 1));
      }
      if (__builtin_strstr(r.name, "16-bit")) {
        // 20 additions of 0.0001 .. 0.0003, in 16 bits: 20 * 7/65535 .. 20 * 20/65535, a hundred times brighter: 54 101 156. In 8 bits each addition is rounded away: black.
        CHECK(near(px(7, 4), 0xFF36659C) && (px(6, 4) & 0xFFFFFF) == 0);
      }
      if (__builtin_strstr(r.name, "integer render")) CHECK(px(16, 4) == 0xFF3380CC && px(0, 0) == 0xFF14283C);   // (51, 128, 204 = 255 * the colour, and the cleared (20, 40, 60))
    }
    if (__builtin_strstr(r.name, "multisampled image as a shader")) {
      // The red rectangle's left edge is x = 4.3, which cuts pixel 4: the rows of its column read samples 0 .. 3 in turn, and of the four two are on the red side and two are not.
      auto px = [&](uint32_t x, uint32_t y) { return r.device_picture[y * 32 + x]; };
      uint32_t black = 0, red = 0;
      for (uint32_t y = 8; y < 12; ++y) (px(4, y) == 0xFF000000u ? black : red) += px(4, y) == 0xFF000000u || px(4, y) == 0xFFFF0000u;
      CHECK(black == 2 && red == 2 && px(10, 10) == 0xFFFF0000u && px(14, 14) == 0xFF00FF00u);
    }
    if (__builtin_strstr(r.name, "group-shared memory")) {
      // Thread 0 of group (0, 0): the neighbour's value is 1, the sum of the 64 slots (0 .. 63) 2016 (>> 3 is 252), the thread's own number 0. Thread 1 sees 2 and has 4 as its blue.
      auto px = [&](uint32_t x, uint32_t y) { return r.device_picture[y * 32 + x]; };
      CHECK(px(0, 0) == 0xFF01FC00u && px(1, 0) == 0xFF02FC04u && px(8, 0) == (0xFF000000u | (8u << 16) | (0x34u << 8)));
    }
    if (__builtin_strstr(r.name, "stream output of two streams")) {
      // Squares at the three positions of stream 0 (the lower left and lower right and the top middle, +Y up as the shader had it), coloured by 0.5 * q + 0.5 of stream 1.
      auto px = [&](uint32_t x, uint32_t y) { return r.device_picture[y * 32 + x] & 0xFFFFFF; };
      auto near = [](uint32_t a, uint32_t b) {
        for (uint32_t shift = 0; shift < 24; shift += 8) {
          const int32_t d = static_cast<int32_t>((a >> shift) & 0xFF) - static_cast<int32_t>((b >> shift) & 0xFF);
          if (d > 3 || d < -3) return false;
        }
        return true;
      };
      CHECK(near(px(8, 24), 0x7F00FF) && near(px(24, 24), 0xFF7FFF) && near(px(16, 8), 0x7FFFFF) && px(16, 24) == 0 && px(0, 0) == 0);
    }
    if (__builtin_strstr(r.name, "adjacency primitives")) {
      // The triangle (left) and the quad (right) both drawn.
      uint32_t left = 0, right = 0;
      for (uint32_t y = 0; y < 32; ++y)
        for (uint32_t x = 0; x < 32; ++x) (x < 16 ? left : right) += (r.device_picture[y * 32 + x] & 0xFFFFFF) != 0;
      CHECK(left > 60 && right > 150);
    }
    if (__builtin_strstr(r.name, "instanced geometry")) {
      uint32_t lit = 0;
      for (uint32_t i = 0; i < 32 * 32; ++i) lit += (r.device_picture[i] & 0xFFFFFF) != 0;
      CHECK(lit > 100);
    }
    if (__builtin_strstr(r.name, "depth resolve")) {
      // Counts of the three levels: black (near, 0.3), green (0.7) and blue (the cleared 1). Of the pixels where the rectangles' edges cut, the first sample's depth is one of them, the least the
      // nearer, the greatest the further: so there are more far (blue) pixels under "greatest" than "first" than "least", the reverse for black.
      uint32_t black = 0, green = 0, blue = 0;
      for (uint32_t i = 0; i < 32 * 32; ++i) {
        const uint32_t c = r.device_picture[i] & 0xFFFFFF;
        black += c == 0, green += c == 0x00FF00, blue += c == 0x0000FF;
      }
      CHECK(black + green + blue == 1024);
      static uint32_t s_black[3], s_blue[3];
      const uint32_t which = __builtin_strstr(r.name, "first") ? 0 : __builtin_strstr(r.name, "least") ? 1 : 2;
      s_black[which] = black, s_blue[which] = blue;
      if (which == 2) CHECK(s_black[1] > s_black[0] && s_black[0] > s_black[2] && s_blue[2] > s_blue[0] && s_blue[0] > s_blue[1]);   // (the scenes run in this order: first, least, greatest)
    }
    if (__builtin_strstr(r.name, "alpha to coverage")) {
      // The red channel of each band (the colour is red 1): none, all, none, all, all.
      auto red = [&](uint32_t band) { return (r.device_picture[16 * 32 + band * 6 + 3] >> 16) & 0xFF; };
      CHECK(red(0) == 0 && red(1) == 255 && red(2) == 0 && red(3) == 255 && red(4) == 255);
    }
    if (__builtin_strstr(r.name, "occlusion predicate and pipeline")) {
      // Stripes of 4 pixels from x = 1: predicate 1 (full red), predicate 0 (0.25 of the red is the base: 0), 12 of 16 vertices (0.75), 6 of 8 primitives (0.75), both runs counted (1).
      auto red = [&](uint32_t stripe) { return (r.device_picture[16 * 32 + 1 + 5 * stripe + 1] >> 16) & 0xFF; };
      CHECK(red(0) == 255 && red(1) == 0 && red(2) >= 190 && red(2) <= 192 && red(3) >= 190 && red(3) <= 192 && red(4) == 255 && red(5) == 255);
    }
    if (__builtin_strstr(r.name, "of every width") || __builtin_strstr(r.name, "10-bit normalised")) {
      // The colour of the triangle of each quarter, as the format keeps it (integers: colour * 255 (- 100); the 10-bit alpha of 0.7 is 2/3, of 0.3 1/3).
      auto tri = [&](uint32_t q) { return r.device_picture[((q >> 1) * 16 + 8) * 32 + (q & 1) * 16 + 8]; };
      if (__builtin_strstr(r.name, "of every width")) CHECK(tri(0) == 0xFF3380CC && tri(1) == 0xFF4D99E5 && tri(2) == 0xFFE56619 && tri(3) == 0xFF19B366);
      else CHECK(tri(0) == 0xFF4C99E5 && tri(1) == 0xFF00E573 && tri(2) == 0xAA3380CC && tri(3) == 0x550DF280);
    }
    if (__builtin_strstr(r.name, "GetDimensions of")) {
      // The alpha is the number of levels over 2 (one level: 0x80) wherever something is drawn; the size is in the colour: the 4 x 4 texels of a 3D image of 4 slices, an array of 3 layers
      // (blue 3 / 8 over 2 = 0x30 in the first band) and a cube map.
      uint32_t drawn = 0, wrong = 0;
      for (uint32_t i = 0; i < 32 * 32; ++i)
        if (r.device_picture[i] >> 24 == 0x80) ++drawn;
        else if ((r.device_picture[i] & 0xFFFFFF) != 0) ++wrong;
      CHECK(drawn > 500 && wrong == 0);
      if (__builtin_strstr(r.name, "image array")) CHECK((r.device_picture[8] & 0xFF) == 0x30);
      if (__builtin_strstr(r.name, "3D image")) CHECK((r.device_picture[20 * 32 + 8] & 0xFFFF00) == 0x102000);
    }
    if (__builtin_strstr(r.name, "tessellation, winding")) {
      // Back faces culled: the counter-clockwise patches (the right cells) are drawn, the clockwise ones (the left cells) are not.
      uint32_t left = 0, right = 0;
      for (uint32_t y = 0; y < 32; ++y)
        for (uint32_t x = 0; x < 32; ++x) (x < 16 ? left : right) += (r.device_picture[y * 32 + x] & 0xFFFFFF) != 0;
      CHECK(left == 0 && right > 100);
    }
    if (__builtin_strstr(r.name, "tessellation")) {
      // Every tessellation scene draws something in all four cells (the winding one in two), and the rings show: more than a plain triangle's worth of distinct colours.
      uint32_t lit = 0;
      for (uint32_t i = 0; i < 32 * 32; ++i) lit += (r.device_picture[i] & 0xFFFFFF) != 0;
      CHECK(lit > 30);
    }
    if (__builtin_strstr(r.name, "multisampling")) {
      // Inside a rectangle all four samples agree; on the left edge of the far one (x = 4.3) three of the four samples of the pixel are inside: 3/4 of red. Where the near green edge
      // (x = 12.4) cuts the pixel above the red, two samples are green and two red.
      auto px = [&](uint32_t x, uint32_t y) { return r.device_picture[y * 32 + x] & 0xFFFFFF; };
      auto near = [](uint32_t a, uint32_t b) {
        for (uint32_t shift = 0; shift < 24; shift += 8) {
          const int32_t d = static_cast<int32_t>((a >> shift) & 0xFF) - static_cast<int32_t>((b >> shift) & 0xFF);
          if (d > 2 || d < -2) return false;
        }
        return true;
      };
      // (columns 8 and 20 are even, 9 and 21 odd: both images give the same picture)
      CHECK(px(8, 10) == 0xFF0000 && px(9, 10) == 0xFF0000 && px(20, 20) == 0x00FF00 && px(21, 20) == 0x00FF00 && px(1, 1) == 0 && near(px(4, 10), 0xBF0000) && near(px(12, 15), 0x808000));
      CHECK(near(px(5, 10), 0xFF0000) && near(px(13, 15), 0x00FF00));
    }
    if (__builtin_strstr(r.name, "compute shader")) {
      // Every pixel from the formula of cs_fill.hlsl (the thread of x, y is number (y % 4) * 8 + x % 8 of group x / 8, y / 4).
      uint32_t wrong = 0;
      for (uint32_t y = 0; y < 32; ++y)
        for (uint32_t x = 0; x < 32; ++x) {
          const uint32_t gi = (y % 4) * 8 + x % 8;
          const uint32_t v = 0xFF000000u | ((x * 8) << 16) | ((y * 8) << 8) | (((x / 8) * 40 + gi) & 0xFF);
          const uint32_t want = v ^ 0x00102030u ^ ((x ^ y) & 0xF);
          if (r.device_picture[y * 32 + x] != want) ++wrong;
        }
      CHECK(wrong == 0);
    }
    if (__builtin_strstr(r.name, "occlusion queries")) {
      // The stripes: all 256 pixels of the open quad (red 255), half of them for the one half behind the near quad (about 128), none for the one wholly behind it.
      const uint32_t row = 8 * 32;
      const uint32_t open = (r.device_picture[row + 4] >> 16) & 0xFF, half = (r.device_picture[row + 16] >> 16) & 0xFF, none = (r.device_picture[row + 28] >> 16) & 0xFF;
      CHECK(open >= 250 && half >= 124 && half <= 132 && none <= 4);
    }
    if (__builtin_strstr(r.name, "multiple render targets")) {
      // Even columns come from target 0 (the vertex colour), odd ones from target 1 (red and blue swapped): across a pair of neighbours the red of one is the blue of the other.
      uint32_t swapped = 0, strong = 0;
      for (uint32_t y = 0; y < 32; ++y)
        for (uint32_t x = 0; x < 32; x += 2) {
          const uint32_t a = r.device_picture[y * 32 + x], b = r.device_picture[y * 32 + x + 1];
          auto ch = [](uint32_t v, uint32_t shift) { return static_cast<int32_t>((v >> shift) & 0xFF); };
          auto near = [](int32_t u, int32_t v) { return (u > v ? u - v : v - u) <= 40; };
          if (!(a & 0xFFFFFF) || !(b & 0xFFFFFF)) continue;
          if (near(ch(a, 16), ch(b, 0)) && near(ch(a, 0), ch(b, 16)) && near(ch(a, 8), ch(b, 8))) ++swapped;
          if (ch(a, 16) - ch(a, 0) > 100 || ch(a, 0) - ch(a, 16) > 100) ++strong;
        }
      CHECK(swapped >= 100 && strong >= 5);
    }
  }, &log);
  // The software renderer takes every scene, and draws the same picture as itself.
  CHECK(bad == 0 && log.differs == 0 && log.broken == 0 && log.refused == 0 && log.same >= 20);
  printf("  conformance: %u scenes, all the same on two software adapters\n", log.same);

  // A device that is told it has no blend state, instancing or lines refuses those scenes (honestly: kNotSupported), and the others still pass.
  FakeScreen screen2(8, 6);
  graphics::SoftwareDevice inner(allocator, &screen2);
  SpyDevice plain(inner);
  plain.feature_mask = 0;
  Kernel limited(allocator);
  AdapterConfig c2;
  c2.display = &screen2;
  CHECK(limited.add_adapter(plain, c2) == 0 && limited.add_adapter(reference, c2) == 1);
  ConformanceLog log2;
  const uint32_t bad2 = run_conformance(allocator, limited, 0, 1, [](void* context, const SceneResult& r) {
    ConformanceLog& l = *static_cast<ConformanceLog*>(context);
    r.verdict == SceneVerdict::Same ? ++l.same : r.verdict == SceneVerdict::Refused ? ++l.refused : r.verdict == SceneVerdict::Differs ? ++l.differs : ++l.broken;
  }, &log2);
  CHECK(bad2 == 0 && log2.refused >= 4 && log2.same >= 14);
  printf("  conformance: a device without the Direct3D 10 features refuses %u scenes and draws %u\n", log2.refused, log2.same);
}

void test_device_suite() {
  HostAllocator allocator;
  {
    FakeScreen screen(8, 6);
    graphics::SoftwareDevice software(allocator, &screen);
    SpyDevice spy(software);
    CHECK(dxgk_suite::compare(allocator, software, &screen, "software", 0, 0) == 0);
    CHECK(dxgk_suite::compare(allocator, spy, &screen, "spy", 0, 0) == 0);
    // What the adapter says of the software device: everything.
    Kernel kernel(allocator);
    CHECK(kernel.add_adapter(software, AdapterConfig()) == 0);
    const AdapterCaps& caps = kernel.adapter(0)->caps();
    CHECK(caps.stencil && caps.mipmaps && caps.textures && caps.programmable_vertex && caps.programmable_pixel);
  }
  CHECK(allocator.live == 0);
}

// --- Direct3D 10 constant buffers ----------------------------------------------------------------------------------------------------------------------------------------

Tokens dx_scalar_imm(uint32_t bits) { return {0x4001u, bits}; }
// cb<slot>[<element> + r<temp>.<component>]
Tokens dx_cb_relative(uint32_t slot, uint32_t element, uint32_t temp, uint32_t component) {
  return {2u | (1u << 2) | (0xE4u << 4) | (8u << 12) | (2u << 20) | (3u << 25), slot, element, 2u | (2u << 2) | (component << 4) | (1u << 20), temp};
}
// icb[r<temp>.<component>]
Tokens dx_icb_relative(uint32_t temp, uint32_t component) { return {2u | (1u << 2) | (0xE4u << 4) | (9u << 12) | (1u << 20) | (2u << 22), 2u | (2u << 2) | (component << 4) | (1u << 20), temp}; }
Tokens dx_icb(uint32_t index) { return {2u | (1u << 2) | (0xE4u << 4) | (9u << 12) | (1u << 20), index}; }

void test_dxbc_constant_buffers() {
  HostAllocator allocator;
  {
    // cb0[2] and cb1[4] are laid end to end: cb1[3] is register 5 of the constant block.
    auto program_for = [&](uint32_t index_bits) {
      DxbcAsm a = dx_pixel_shader();
      a.constant_buffer = 2;
      a.raw({0x04000059, 0x208E46, 1, 4});   // dcl_constant_buffer cb1[4]
      a.ins(54, {dx_dst(kOpTemp, 0, 1), dx_scalar_imm(index_bits)});
      a.ins(54, {dx_dst(kOpOut, 0, 0xF), dx_cb_relative(1, 1, 0, 0)});   // mov o0, cb1[r0.x + 1]
      a.ins(62, {});
      NtStatus st = 0;
      UniquePointer<D3d9Program> p = compile_dxbc(allocator, a.build(), &st);
      CHECK(st == status::kSuccess && p && p->constant_bytes() == 96);
      return p;
    };
    for (int32_t index : {2, 0, -1, 3, 100, -50}) {   // (1 + index is the element: 3 and 0 are in range, 4 and above or below 0 are not)
      UniquePointer<D3d9Program> p = program_for(static_cast<uint32_t>(index));
      if (!p) continue;
      for (unsigned set = 0; set < 4; ++set) {
        uint8_t constants[128];
        for (unsigned i = 0; i < 8; ++i)
          for (unsigned k = 0; k < 4; ++k) memcpy(constants + i * 16 + k * 4, &kConstants[set][i][k], 4);
        graphics::Vec4 varyings[4] = {}, c = {};
        p->run_fragment(varyings, constants, nullptr, &c);
        const int element = 1 + index;
        const bool inside = element >= 0 && element < 4;
        const float* want = kConstants[set][2 + (inside ? element : 0)];
        const bool ok = inside ? (near_value(c.x, want[0]) && near_value(c.y, want[1]) && near_value(c.z, want[2]) && near_value(c.w, want[3])) : (c.x == 0 && c.y == 0 && c.z == 0 && c.w == 0);
        if (!ok) printf("  cb1[r0.x + 1] with r0.x = %d: got %g %g %g %g\n", index, c.x, c.y, c.z, c.w);
        CHECK(ok);
      }
    }
    // The program as TGSI (the integer register becomes the address register); a device that is given 128 bytes cannot run a program that reads 256.
    {
      UniquePointer<D3d9Program> p = program_for(2);
      String text(allocator);
      CHECK(p && p->emit_tgsi(&text) == status::kSuccess);   // (UARL: the integer register is the address)
    }
    // The immediate constant buffer.
    {
      DxbcAsm a = dx_pixel_shader();
      a.constant_buffer = 0;
      const uint32_t icb_data[] = {53u | (3u << 11), 2 + 16, __builtin_bit_cast(uint32_t, 0.0f), __builtin_bit_cast(uint32_t, 0.0f), __builtin_bit_cast(uint32_t, 0.0f), __builtin_bit_cast(uint32_t, 1.0f),
                                   __builtin_bit_cast(uint32_t, 1.0f), __builtin_bit_cast(uint32_t, 2.0f), __builtin_bit_cast(uint32_t, 3.0f), __builtin_bit_cast(uint32_t, 1.0f),
                                   __builtin_bit_cast(uint32_t, 2.0f), __builtin_bit_cast(uint32_t, 4.0f), __builtin_bit_cast(uint32_t, 6.0f), __builtin_bit_cast(uint32_t, 1.0f),
                                   __builtin_bit_cast(uint32_t, 3.0f), __builtin_bit_cast(uint32_t, 6.0f), __builtin_bit_cast(uint32_t, 9.0f), __builtin_bit_cast(uint32_t, 1.0f)};
      a.code.insert(a.code.end(), icb_data, icb_data + 18);
      a.ins(54, {dx_dst(kOpTemp, 0, 1), dx_scalar_imm(3)});
      a.ins(0, {dx_dst(kOpOut, 0, 0xF), dx_icb_relative(0, 0), dx_icb(1)});   // add o0, icb[r0.x], icb[1]
      a.ins(62, {});
      NtStatus st = 0;
      UniquePointer<D3d9Program> p = compile_dxbc(allocator, a.build(), &st);
      CHECK(st == status::kSuccess && p);
      if (p) {
        uint8_t constants[128] = {};
        graphics::Vec4 varyings[4] = {}, c = {};
        p->run_fragment(varyings, constants, nullptr, &c);
        CHECK(c.x == 3.0f + 1.0f && c.y == 6.0f + 2.0f && c.z == 9.0f + 3.0f && c.w == 2.0f);
      }
    }
    // Declared sizes are checked: too big, a constant read past the end, a buffer that is not declared.
    {
      NtStatus st = 0;
      DxbcAsm big = dx_pixel_shader();
      big.constant_buffer = 200;
      big.raw({0x04000059, 0x208E46, 1, 100});   // 300 vec4s in all: more than 256
      big.ins(54, {dx_dst(kOpOut, 0, 0xF), dx_cb(0, 1)});
      big.ins(62, {});
      compile_dxbc(allocator, big.build(), &st);
      CHECK(st == status::kNotSupported);
      DxbcAsm past = dx_pixel_shader();
      past.constant_buffer = 2;
      past.ins(54, {dx_dst(kOpOut, 0, 0xF), dx_cb(0, 2)});   // cb0[2] of a cb0[2]
      past.ins(62, {});
      compile_dxbc(allocator, past.build(), &st);
      CHECK(st == status::kNotSupported);
      DxbcAsm undeclared = dx_pixel_shader();
      undeclared.ins(54, {dx_dst(kOpOut, 0, 0xF), dx_cb(3, 0)});
      undeclared.ins(62, {});
      compile_dxbc(allocator, undeclared.build(), &st);
      CHECK(st == status::kNotSupported);
    }
    // A device that takes 128 bytes of constants refuses a shader that reads 256; one that takes 4096 does not.
    {
      FakeScreen screen(8, 6);
      graphics::SoftwareDevice software(allocator, &screen);
      SpyDevice small(software);
      small.limit = 128;
      DxbcAsm a = dx_pixel_shader();
      a.constant_buffer = 16;
      a.ins(54, {dx_dst(kOpOut, 0, 0xF), dx_cb(0, 15)});
      a.ins(62, {});
      const std::vector<uint8_t> blob = a.build();
      for (SpyDevice* device : {&small}) {
        Kernel kernel(allocator);
        AdapterConfig c;
        c.display = &screen;
        kernel.add_adapter(*device, c);
        Handle adapter = 0, dev = 0, h = 0;
        CHECK(kernel.open_process(kPid) == status::kSuccess && kernel.open_adapter_from_index(kPid, 0, &adapter) == status::kSuccess && kernel.create_device(kPid, adapter, &dev) == status::kSuccess);
        ShaderDesc d;
        d.kind = ShaderKind::Dxbc;
        d.stage = graphics::ShaderStage::Fragment;
        d.code = view_of(blob);
        CHECK(kernel.create_shader(kPid, dev, d, &h) == status::kNotSupported);
        kernel.close_process(kPid);
      }
      Kernel kernel(allocator);
      AdapterConfig c;
      c.display = &screen;
      kernel.add_adapter(software, c);
      Handle adapter = 0, dev = 0, h = 0;
      CHECK(kernel.open_process(kPid) == status::kSuccess && kernel.open_adapter_from_index(kPid, 0, &adapter) == status::kSuccess && kernel.create_device(kPid, adapter, &dev) == status::kSuccess);
      ShaderDesc d;
      d.kind = ShaderKind::Dxbc;
      d.stage = graphics::ShaderStage::Fragment;
      d.code = view_of(blob);
      CHECK(kernel.create_shader(kPid, dev, d, &h) == status::kSuccess);
      AdapterCaps caps;
      CHECK(kernel.query_adapter_caps(kPid, adapter, &caps) == status::kSuccess && caps.max_push_constant_bytes == 4096);
      kernel.close_process(kPid);
    }
  }
  CHECK(allocator.live == 0);
}

// --- Direct3D 10 integer and bitwise instructions --------------------------------------------------------------------------------------------------------------------------

Tokens dx_imm_bits(uint32_t a, uint32_t b, uint32_t c, uint32_t d) { return {0x4002u, a, b, c, d}; }

void test_dxbc_integers() {
  HostAllocator allocator;
  {
    struct Op {
      uint32_t opcode, sources;
      uint32_t (*f)(uint32_t, uint32_t, uint32_t);
    };
    auto s32 = [](uint32_t v) { return static_cast<int32_t>(v); };
    (void)s32;
    static const Op kOps[] = {
        {30, 2, [](uint32_t x, uint32_t y, uint32_t) { return x + y; }},
        {40, 1, [](uint32_t x, uint32_t, uint32_t) { return 0u - x; }},
        {35, 3, [](uint32_t x, uint32_t y, uint32_t z) { return x * y + z; }},
        {36, 2, [](uint32_t x, uint32_t y, uint32_t) { return static_cast<int32_t>(x) > static_cast<int32_t>(y) ? x : y; }},
        {37, 2, [](uint32_t x, uint32_t y, uint32_t) { return static_cast<int32_t>(x) < static_cast<int32_t>(y) ? x : y; }},
        {83, 2, [](uint32_t x, uint32_t y, uint32_t) { return x > y ? x : y; }},
        {84, 2, [](uint32_t x, uint32_t y, uint32_t) { return x < y ? x : y; }},
        {82, 3, [](uint32_t x, uint32_t y, uint32_t z) { return x * y + z; }},
        {41, 2, [](uint32_t x, uint32_t y, uint32_t) { return x << (y & 31); }},
        {42, 2, [](uint32_t x, uint32_t y, uint32_t) { return static_cast<uint32_t>(static_cast<int32_t>(x) >> (y & 31)); }},
        {85, 2, [](uint32_t x, uint32_t y, uint32_t) { return x >> (y & 31); }},
        {1, 2, [](uint32_t x, uint32_t y, uint32_t) { return x & y; }},
        {60, 2, [](uint32_t x, uint32_t y, uint32_t) { return x | y; }},
        {87, 2, [](uint32_t x, uint32_t y, uint32_t) { return x ^ y; }},
        {59, 1, [](uint32_t x, uint32_t, uint32_t) { return ~x; }},
        {32, 2, [](uint32_t x, uint32_t y, uint32_t) { return x == y ? 0xFFFFFFFFu : 0u; }},
        {39, 2, [](uint32_t x, uint32_t y, uint32_t) { return x != y ? 0xFFFFFFFFu : 0u; }},
        {34, 2, [](uint32_t x, uint32_t y, uint32_t) { return static_cast<int32_t>(x) < static_cast<int32_t>(y) ? 0xFFFFFFFFu : 0u; }},
        {33, 2, [](uint32_t x, uint32_t y, uint32_t) { return static_cast<int32_t>(x) >= static_cast<int32_t>(y) ? 0xFFFFFFFFu : 0u; }},
        {79, 2, [](uint32_t x, uint32_t y, uint32_t) { return x < y ? 0xFFFFFFFFu : 0u; }},
        {80, 2, [](uint32_t x, uint32_t y, uint32_t) { return x >= y ? 0xFFFFFFFFu : 0u; }},
    };
    // Four lanes each of three sources: small, negative, large, shift counts past 31.
    const uint32_t x[4] = {7, 0xFFFFFFF9u, 0x80000000u, 0x12345678u}, y[4] = {3, 5, 0x7FFFFFFFu, 33}, z[4] = {100, 0xFFFFFF00u, 1, 0xDEADBEEFu};
    unsigned compared = 0, wrong = 0;
    for (const Op& op : kOps)
      for (unsigned variant = 0; variant < 3; ++variant) {
        DxbcAsm a = dx_pixel_shader();
        a.ins(54, {dx_dst(kOpTemp, 1, 0xF), dx_imm_bits(x[0], x[1], x[2], x[3])});
        a.ins(54, {dx_dst(kOpTemp, 2, 0xF), dx_imm_bits(y[0], y[1], y[2], y[3])});
        a.ins(54, {dx_dst(kOpTemp, 3, 0xF), dx_imm_bits(z[0], z[1], z[2], z[3])});
        // variant 0: as they are; 1: the first source negated (integer negation); 2: the first source's absolute value
        const uint32_t modifier = variant == 1 ? 1 : variant == 2 ? 2 : 0;
        std::vector<Tokens> operands = {dx_dst(kOpTemp, 0, 0xF), dx_src(kOpTemp, 1, 0xE4, modifier)};
        if (op.sources > 1) operands.push_back(dx_src(kOpTemp, 2));
        if (op.sources > 2) operands.push_back(dx_src(kOpTemp, 3));
        Tokens flat;
        for (const Tokens& t : operands) flat.insert(flat.end(), t.begin(), t.end());
        a.code.push_back(op.opcode | (static_cast<uint32_t>(flat.size() + 1) << 24));
        a.code.insert(a.code.end(), flat.begin(), flat.end());
        a.ins(54, {dx_dst(kOpOut, 0, 0xF), dx_src(kOpTemp, 0)});
        a.ins(62, {});
        NtStatus st = 0;
        UniquePointer<D3d9Program> p = compile_dxbc(allocator, a.build(), &st);
        if (st != status::kSuccess || !p) {
          printf("  integer op %u: status %x\n", op.opcode, static_cast<unsigned>(st));
          ++wrong;
          continue;
        }
        uint8_t constants[128] = {};
        graphics::Vec4 varyings[4] = {}, c = {};
        p->run_fragment(varyings, constants, nullptr, &c);
        const float got_f[4] = {c.x, c.y, c.z, c.w};
        bool ok = true;
        for (int k = 0; k < 4; ++k) {
          uint32_t first = x[k];
          if (modifier == 1) first = 0u - first;
          if (modifier == 2) first = static_cast<int32_t>(first) < 0 ? 0u - first : first;
          ok = ok && __builtin_bit_cast(uint32_t, got_f[k]) == op.f(first, y[k], z[k]);
        }
        if (!ok) {
          printf("  integer op %u variant %u wrong: %08x %08x %08x %08x\n", op.opcode, variant, __builtin_bit_cast(uint32_t, got_f[0]), __builtin_bit_cast(uint32_t, got_f[1]),
                 __builtin_bit_cast(uint32_t, got_f[2]), __builtin_bit_cast(uint32_t, got_f[3]));
          ++wrong;
        }
        // (TGSI text has the instructions; the Direct3D 9 tokens do not - run_ex / the machine compare in the loop and system value tests below)
        String text(allocator);
        CHECK(p->emit_tgsi(&text) == status::kSuccess);
        ++compared;
      }
    printf("  dxbc integers: %u programs, %u wrong\n", compared, wrong);
    CHECK(wrong == 0 && compared == sizeof(kOps) / sizeof(kOps[0]) * 3);

    // Conversions: ftoi / ftou saturate, itof / utof.
    {
      NtStatus st = 0;
      // (dcl_temps comes first in a real program: put the declaration before the code)
      DxbcAsm b = dx_pixel_shader();
      b.code.clear();
      b.ins(104, {{8}});
      b.ins(54, {dx_dst(kOpTemp, 1, 0xF), dx_imm(-2.7f, 3.9f, 3.0e10f, -3.0e10f)});
      b.ins(27, {dx_dst(kOpTemp, 2, 0xF), dx_src(kOpTemp, 1)});
      b.ins(28, {dx_dst(kOpTemp, 3, 0xF), dx_src(kOpTemp, 1)});
      b.ins(43, {dx_dst(kOpTemp, 4, 0xF), dx_src(kOpTemp, 2)});
      b.ins(86, {dx_dst(kOpTemp, 5, 0xF), dx_src(kOpTemp, 3)});
      b.ins(54, {dx_dst(kOpOut, 0, 0xF), dx_src(kOpTemp, 4)});
      b.ins(54, {dx_dst(kOpOut, 0, 0x0), dx_src(kOpTemp, 5)});
      b.ins(62, {});
      UniquePointer<D3d9Program> p = compile_dxbc(allocator, b.build(), &st);
      CHECK(st == status::kSuccess && p);
      if (p) {
        uint8_t constants[128] = {};
        graphics::Vec4 varyings[4] = {}, c = {};
        p->run_fragment(varyings, constants, nullptr, &c);
        // ftoi: -2 3 INT_MAX INT_MIN, back as floats
        CHECK(c.x == -2.0f && c.y == 3.0f && c.z == 2147483648.0f && c.w == -2147483648.0f);
      }
    }
    // The idiom of a comparison turned into 1.0: `lt r1, v0, cb0[0]` ; `and r0, r1, l(1.0f)`.
    {
      DxbcAsm a = dx_pixel_shader();
      a.ins(49, {dx_dst(kOpTemp, 1, 0xF), dx_src(kOpInput, 0), dx_cb(0, 0)});
      a.ins(1, {dx_dst(kOpTemp, 0, 0xF), dx_src(kOpTemp, 1), dx_imm_bits(0x3F800000, 0x3F800000, 0x3F800000, 0x3F800000)});
      a.ins(54, {dx_dst(kOpOut, 0, 0xF), dx_src(kOpTemp, 0)});
      a.ins(62, {});
      NtStatus st = 0;
      UniquePointer<D3d9Program> p = compile_dxbc(allocator, a.build(), &st);
      CHECK(st == status::kSuccess && p);
      for (unsigned set = 0; set < 4 && p; ++set) {
        uint8_t constants[128];
        for (unsigned i = 0; i < 8; ++i)
          for (unsigned k = 0; k < 4; ++k) memcpy(constants + i * 16 + k * 4, &kConstants[set][i][k], 4);
        graphics::Vec4 varyings[4], c = {};
        for (unsigned i = 0; i < 4; ++i) varyings[i] = {kInputs[set][i][0], kInputs[set][i][1], kInputs[set][i][2], kInputs[set][i][3]};
        p->run_fragment(varyings, constants, nullptr, &c);
        const float got[4] = {c.x, c.y, c.z, c.w};
        bool ok = true;
        for (int k = 0; k < 4; ++k) ok = ok && got[k] == (kInputs[set][0][k] < kConstants[set][0][k] ? 1.0f : 0.0f);
        CHECK(ok);
      }
    }
    // Two destinations: udiv (quotient, remainder) with a null one, umul's low word, a division by zero.
    {
      DxbcAsm a = dx_pixel_shader();
      a.ins(54, {dx_dst(kOpTemp, 1, 0xF), dx_imm_bits(100, 7, 0xFFFFFFFFu, 9)});
      a.ins(54, {dx_dst(kOpTemp, 2, 0xF), dx_imm_bits(7, 7, 2, 0)});
      a.ins(78, {dx_dst(kOpTemp, 3, 0xF), dx_dst(kOpTemp, 4, 0xF), dx_src(kOpTemp, 1), dx_src(kOpTemp, 2)});   // udiv r3 (quotient), r4 (remainder)
      a.ins(81, {dx_null(), dx_dst(kOpTemp, 5, 0xF), dx_src(kOpTemp, 1), dx_src(kOpTemp, 2)});                  // umul null, r5, ...
      a.ins(54, {dx_dst(kOpOut, 0, 0xF), dx_src(kOpTemp, 3)});
      a.ins(62, {});
      NtStatus st = 0;
      DxbcAsm b = a;
      b.code.insert(b.code.begin(), {104u | (2u << 24), 8u});
      UniquePointer<D3d9Program> p = compile_dxbc(allocator, b.build(), &st);
      CHECK(st == status::kSuccess && p);
      if (p) {
        uint8_t constants[128] = {};
        graphics::Vec4 varyings[4] = {}, c = {};
        p->run_fragment(varyings, constants, nullptr, &c);
        const uint32_t got[4] = {__builtin_bit_cast(uint32_t, c.x), __builtin_bit_cast(uint32_t, c.y), __builtin_bit_cast(uint32_t, c.z), __builtin_bit_cast(uint32_t, c.w)};
        CHECK(got[0] == 100 / 7 && got[1] == 1 && got[2] == 0xFFFFFFFFu / 2 && got[3] == 0xFFFFFFFFu);   // (x / 0 is all ones)
      }
      DxbcAsm high = dx_pixel_shader();
      high.ins(81, {dx_dst(kOpTemp, 5, 0xF), dx_dst(kOpTemp, 6, 0xF), dx_src(kOpTemp, 1), dx_src(kOpTemp, 2)});   // the high word of a product is not taken
      high.ins(62, {});
      compile_dxbc(allocator, high.build(), &st);
      CHECK(st == status::kNotSupported);
    }
  }
  CHECK(allocator.live == 0);
}

// --- Direct3D 10 loops -----------------------------------------------------------------------------------------------------------------------------------------------------

void test_dxbc_loops() {
  HostAllocator allocator;
  {
    auto run = [&](const DxbcAsm& a, graphics::Vec4* out, bool* tgsi_ok, bool check_text) {
      NtStatus st = 0;
      UniquePointer<D3d9Program> p = compile_dxbc(allocator, a.build(), &st);
      CHECK(st == status::kSuccess && p);
      if (!p) return;
      uint8_t constants[128] = {};
      graphics::Vec4 varyings[4] = {};
      p->run_fragment(varyings, constants, nullptr, out);
      if (!check_text) return;
      String text(allocator);
      const bool emitted = p->emit_tgsi(&text) == status::kSuccess;
      *tgsi_ok = emitted;
      if (!emitted) return;
      TgsiMachine m;
      const std::string source(text.data(), text.size());
      bool ok = m.run(source);
      const float want[4] = {out->x, out->y, out->z, out->w};
      for (int k = 0; k < 4; ++k) ok = ok && near_value(m.out[0][k], want[k]);
      if (!ok) printf("  loop tgsi mismatch:\n%s", source.c_str());
      CHECK(ok);
    };
    // Integers: count to 5, add 1.5 each time round, skipping the pass where the counter is 3.
    {
      DxbcAsm a = dx_pixel_shader();
      a.ins(104, {{2}});
      a.ins(54, {dx_dst(kOpTemp, 0, 0xF), dx_imm_bits(0, 0, 0, 0)});
      a.ins(48, {});                                                                                      // loop
      a.ins(33, {dx_dst(kOpTemp, 1, 1), dx_src(kOpTemp, 0, 0x00), dx_scalar_imm(5)});                     //   ige r1.x, r0.x, 5
      a.ins(3, {dx_scalar(kOpTemp, 1, 0)}, 1u << 18);                                                      //   breakc_nz r1.x
      a.ins(30, {dx_dst(kOpTemp, 0, 1), dx_src(kOpTemp, 0, 0x00), dx_scalar_imm(1)});                     //   iadd r0.x, r0.x, 1
      a.ins(32, {dx_dst(kOpTemp, 1, 2), dx_src(kOpTemp, 0, 0x00), dx_scalar_imm(3)});                     //   ieq r1.y, r0.x, 3
      a.ins(8, {dx_scalar(kOpTemp, 1, 1)}, 1u << 18);                                                      //   continuec_nz r1.y
      a.ins(0, {dx_dst(kOpTemp, 0, 2), dx_src(kOpTemp, 0, 0x55), dx_imm(1.5f, 1.5f, 1.5f, 1.5f)});        //   add r0.y, r0.y, 1.5
      a.ins(22, {});                                                                                      // endloop
      a.ins(54, {dx_dst(kOpOut, 0, 0xF), dx_src(kOpTemp, 0)});
      a.ins(62, {});
      graphics::Vec4 c = {};
      bool tgsi_ok = true;
      run(a, &c, &tgsi_ok, true);
      CHECK(__builtin_bit_cast(uint32_t, c.x) == 5 && c.y == 4 * 1.5f);   // (the counter is 5; the pass of 3 added nothing)
      CHECK(tgsi_ok);   // (the TGSI text of the integer loop agrees with the interpreter)
    }
    // Floats only: a loop TGSI text can say. Count to 4 in steps of 1.0, 0.5 more each time round; a nested loop and an if with a break inside it.
    {
      DxbcAsm a = dx_pixel_shader();
      a.ins(104, {{2}});
      a.ins(54, {dx_dst(kOpTemp, 0, 0xF), dx_imm(0, 0, 0, 0)});
      a.ins(48, {});                                                                                      // loop
      a.ins(29, {dx_dst(kOpTemp, 1, 1), dx_src(kOpTemp, 0, 0x00), dx_scalar_imm(__builtin_bit_cast(uint32_t, 4.0f))});   //   ge r1.x, r0.x, 4.0
      a.ins(3, {dx_scalar(kOpTemp, 1, 0)}, 1u << 18);                                                      //   breakc_nz r1.x
      a.ins(0, {dx_dst(kOpTemp, 0, 1), dx_src(kOpTemp, 0, 0x00), dx_imm(1.0f, 1.0f, 1.0f, 1.0f)});        //   add r0.x, r0.x, 1
      a.ins(0, {dx_dst(kOpTemp, 0, 2), dx_src(kOpTemp, 0, 0x55), dx_imm(0.5f, 0.5f, 0.5f, 0.5f)});        //   add r0.y, r0.y, 0.5
      a.ins(48, {});                                                                                      //   loop
      a.ins(0, {dx_dst(kOpTemp, 0, 4), dx_src(kOpTemp, 0, 0xAA), dx_imm(0.25f, 0.25f, 0.25f, 0.25f)});   //     add r0.z, r0.z, 0.25
      a.ins(2, {});                                                                                       //     break
      a.ins(22, {});                                                                                      //   endloop
      a.ins(22, {});                                                                                      // endloop
      a.ins(54, {dx_dst(kOpOut, 0, 0xF), dx_src(kOpTemp, 0)});
      a.ins(62, {});
      graphics::Vec4 c = {};
      bool tgsi_ok = false;
      run(a, &c, &tgsi_ok, true);
      CHECK(c.x == 4.0f && c.y == 2.0f && c.z == 1.0f);   // (the inner loop runs once a pass)
      CHECK(tgsi_ok);
    }
    // A loop with no way out stops after the step limit instead of hanging.
    {
      DxbcAsm a = dx_pixel_shader();
      a.ins(48, {});
      a.ins(22, {});
      a.ins(54, {dx_dst(kOpOut, 0, 0xF), dx_imm(1, 1, 1, 1)});
      a.ins(62, {});
      graphics::Vec4 c = {};
      bool tgsi_ok = false;
      run(a, &c, &tgsi_ok, false);
      CHECK(c.x == 0 && c.w == 0);
    }
    // Malformed nesting.
    {
      NtStatus st = 0;
      DxbcAsm stray = dx_pixel_shader();
      stray.ins(2, {});   // break outside a loop
      stray.ins(62, {});
      compile_dxbc(allocator, stray.build(), &st);
      CHECK(st == status::kInvalidParameter);
      DxbcAsm straddle = dx_pixel_shader();
      straddle.ins(48, {});
      straddle.ins(31, {dx_scalar(kOpTemp, 0, 0)}, 1u << 18);
      straddle.ins(22, {});   // endloop inside an if
      straddle.ins(21, {});
      straddle.ins(62, {});
      compile_dxbc(allocator, straddle.build(), &st);
      CHECK(st == status::kInvalidParameter);
    }
  }
  CHECK(allocator.live == 0);
}

// --- Primitive topologies --------------------------------------------------------------------------------------------------------------------------------------------------

void test_topologies() {
  Env e;
  Handle rt = e.alloc(kTarget, Format::A8R8G8B8, 16, 16), vb = e.buffer(kAllocVertexBuffer, 12 * 28), ib = e.buffer(kAllocIndexBuffer, 64);
  const float identity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
  auto vertices = [&](std::initializer_list<std::pair<float, float>> points) {
    std::vector<float> v;
    for (auto& p : points) {
      const float shade = 0.2f + (p.first > 0 ? 0.3f : 0.0f) + (p.second > 0 ? 0.2f : 0.0f);   // (a vertex has its colour wherever it is repeated)
      v.insert(v.end(), {p.first, p.second, 0.5f, shade, 1.0f - shade, 0.5f, 1.0f});
    }
    CHECK(e.write(vb, floats(v.data(), static_cast<uint32_t>(v.size()))));
  };
  auto picture = [&](Handle pipeline, auto commands) {
    CommandWriter w(e.allocator);
    w.begin_pass(0, kNoAllocation, kPassClearColor, {0, 0, 0, 1}, 1, 0);
    w.bind_pipeline(pipeline);
    w.bind_vertex_buffer(1);
    w.push_constants(floats(identity, 16));
    commands(w);
    w.end_pass();
    const NtStatus rendered = e.render(w, {rt, vb, ib});
    if (rendered != status::kSuccess) printf("  topology render: %x\n", static_cast<unsigned>(rendered));
    CHECK(w.ok() && rendered == status::kSuccess);
    std::vector<uint32_t> out;
    for (uint32_t y = 0; y < 16; ++y)
      for (uint32_t x = 0; x < 16; ++x) out.push_back(e.pixel(rt, x, y));
    return out;
  };
  auto lit = [](const std::vector<uint32_t>& p) {
    unsigned n = 0;
    for (uint32_t v : p) n += v != 0xFF000000u;
    return n;
  };
  auto write_indices16 = [&](std::initializer_list<uint16_t> list) {
    std::vector<uint16_t> idx(list);
    idx.resize(32, 0);
    CHECK(e.write(ib, ConstByteView(reinterpret_cast<const uint8_t*>(idx.data()), idx.size() * 2)));
  };

  // A strip of four vertices is the two triangles of a list, with every second one turned; with back faces culled both survive, or neither does.
  vertices({{-0.8f, -0.8f}, {0.8f, -0.8f}, {-0.8f, 0.8f}, {0.8f, 0.8f}});
  for (graphics::FrontFace front : {graphics::FrontFace::CounterClockwise, graphics::FrontFace::Clockwise}) {
    Handle pipe = make_colored_pipeline(e, false, graphics::CullMode::Back, front);
    const std::vector<uint32_t> strip = picture(pipe, [](CommandWriter& w) { w.set_topology(Topology::TriangleStrip); w.draw(4); });
    vertices({{-0.8f, -0.8f}, {0.8f, -0.8f}, {-0.8f, 0.8f}, {-0.8f, 0.8f}, {0.8f, -0.8f}, {0.8f, 0.8f}});   // (v0 v1 v2, then v2 v1 v3 written out)
    const std::vector<uint32_t> list = picture(pipe, [](CommandWriter& w) { w.draw(6); });
    vertices({{-0.8f, -0.8f}, {0.8f, -0.8f}, {-0.8f, 0.8f}, {0.8f, 0.8f}});
    CHECK(strip == list);
    // (the first triangle is on screen for one front face and culled for the other; its twin goes with it)
    const unsigned n = lit(strip);
    CHECK(n == 0 || n > 100);
    if (front == graphics::FrontFace::CounterClockwise) CHECK(n != 0 || lit(picture(make_colored_pipeline(e, false, graphics::CullMode::Back, graphics::FrontFace::Clockwise), [](CommandWriter& w) { w.set_topology(Topology::TriangleStrip); w.draw(4); })) > 100);
  }

  // A fan: the first vertex is in every triangle.
  {
    Handle pipe = make_colored_pipeline(e);
    const std::pair<float, float> fan[5] = {{0.0f, 0.0f}, {0.8f, 0.0f}, {0.5f, 0.6f}, {-0.3f, 0.8f}, {-0.8f, 0.1f}};
    vertices({fan[0], fan[1], fan[2], fan[3], fan[4]});
    const std::vector<uint32_t> as_fan = picture(pipe, [](CommandWriter& w) { w.set_topology(Topology::TriangleFan); w.draw(5); });
    vertices({fan[0], fan[1], fan[2], fan[0], fan[2], fan[3], fan[0], fan[3], fan[4]});
    const std::vector<uint32_t> as_list = picture(pipe, [](CommandWriter& w) { w.draw(9); });
    CHECK(as_fan == as_list && lit(as_fan) > 30);
  }

  // An indexed strip with a restart: a quad, then a triangle; the vertex offset shifts the indices.
  {
    Handle pipe = make_colored_pipeline(e);
    const std::pair<float, float> p[7] = {{-0.9f, -0.9f}, {-0.1f, -0.9f}, {-0.9f, -0.1f}, {-0.1f, -0.1f}, {0.1f, 0.1f}, {0.9f, 0.1f}, {0.5f, 0.9f}};
    vertices({p[0], p[1], p[2], p[3], p[4], p[5], p[6]});
    write_indices16({0, 1, 2, 3, 0xFFFF, 4, 5, 6});
    const std::vector<uint32_t> strip = picture(pipe, [](CommandWriter& w) { w.bind_index_buffer(2, graphics::IndexType::Uint16); w.set_topology(Topology::TriangleStrip); w.draw_indexed(8); });
    vertices({p[0], p[1], p[2], p[2], p[1], p[3], p[4], p[5], p[6]});
    const std::vector<uint32_t> list = picture(pipe, [](CommandWriter& w) { w.draw(9); });
    CHECK(strip == list && lit(strip) > 40);
    // After a strip, a list draw with the stream's own indices is drawn from them again.
    write_indices16({0, 1, 2, 4, 5, 6});
    vertices({p[0], p[1], p[2], p[3], p[4], p[5], p[6]});
    const std::vector<uint32_t> mixed = picture(pipe, [](CommandWriter& w) {
      w.bind_index_buffer(2, graphics::IndexType::Uint16);
      w.set_topology(Topology::TriangleStrip);
      w.draw_indexed(3);   // (0 1 2)
      w.set_topology(Topology::TriangleList);
      w.draw_indexed(3, 3);   // (4 5 6)
    });
    vertices({p[0], p[1], p[2], p[4], p[5], p[6]});
    const std::vector<uint32_t> expected = picture(pipe, [](CommandWriter& w) { w.draw(6); });
    CHECK(mixed == expected && lit(mixed) > 20);
    // Fewer than three vertices draw nothing; a bad topology is refused.
    vertices({p[0], p[1], p[2]});
    CHECK(lit(picture(pipe, [](CommandWriter& w) { w.set_topology(Topology::TriangleStrip); w.draw(2); })) == 0);
    CommandWriter bad(e.allocator);
    bad.begin_pass(0, kNoAllocation, kPassClearColor, {0, 0, 0, 1}, 1, 0);
    bad.set_topology(static_cast<Topology>(99));   // (7 is a patch list of two control points now)
    bad.end_pass();
    CHECK(e.render(bad, {rt, vb, ib}) == status::kInvalidParameter);
  }
}

// --- Direct3D 10 pipeline state, instancing, lines and points -------------------------------------------------------------------------------------------------------------

bool close8(uint32_t a, uint32_t b, uint32_t tolerance = 2) { return (a > b ? a - b : b - a) <= tolerance; }

void test_d3d10_state() {
  Env e;
  AdapterCaps caps;
  CHECK(e.kernel.query_adapter_caps(kPid, e.adapter, &caps) == status::kSuccess);
  const uint32_t all = graphics::kFeatureBlendState | graphics::kFeatureTexelAccess | graphics::kFeatureSystemInputs | graphics::kFeatureInstancing | graphics::kFeatureLines;
  CHECK((caps.features & all) == all);

  Handle rt = e.alloc(kTarget, Format::A8R8G8B8, 16, 16), vb = e.buffer(kAllocVertexBuffer, 256);
  const float identity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
  auto picture = [&](Handle pipe, auto commands) {
    CommandWriter w(e.allocator);
    w.begin_pass(0, kNoAllocation, kPassClearColor, {0.2f, 0.4f, 0.6f, 1.0f}, 1, 0);
    w.bind_pipeline(pipe);
    w.bind_vertex_buffer(1);
    w.push_constants(floats(identity, 16));
    commands(w);
    w.end_pass();
    const NtStatus s = e.render(w, {rt, vb});
    CHECK(w.ok());
    std::vector<uint32_t> out;
    if (s != status::kSuccess) {
      printf("  d3d10 render: %x\n", static_cast<unsigned>(s));
      return out;
    }
    for (uint32_t y = 0; y < 16; ++y)
      for (uint32_t x = 0; x < 16; ++x) out.push_back(e.pixel(rt, x, y));
    return out;
  };
  auto pipeline_for = [&](PipelineDesc pd, NtStatus* status_out = nullptr) {
    Handle vs = 0, fs = 0, pipe = 0;
    CHECK(e.kernel.create_shader(kPid, e.dev, {ShaderKind::ColoredVertex, graphics::ShaderStage::Vertex, {}}, &vs) == status::kSuccess);
    CHECK(e.kernel.create_shader(kPid, e.dev, {ShaderKind::ColoredFragment, graphics::ShaderStage::Fragment, {}}, &fs) == status::kSuccess);
    graphics::GraphicsPipelineDesc d;
    graphics::describe_colored_pipeline(&d);
    pd.vertex_shader = vs, pd.fragment_shader = fs;
    pd.vertex_stride = d.vertex_stride, pd.attribute_count = d.attribute_count, pd.varying_count = d.varying_count;
    for (uint32_t i = 0; i < d.attribute_count; ++i) pd.attributes[i] = d.attributes[i];
    pd.cull_mode = graphics::CullMode::None;
    const NtStatus s = e.kernel.create_pipeline(kPid, e.dev, pd, &pipe);
    if (status_out) *status_out = s;
    else CHECK(s == status::kSuccess);
    return pipe;
  };
  // A big triangle of one colour at half alpha.
  const float quad[21] = {-1, -1, 0.5f, 1, 0.5f, 0, 0.5f, 3, -1, 0.5f, 1, 0.5f, 0, 0.5f, -1, 3, 0.5f, 1, 0.5f, 0, 0.5f};
  CHECK(e.write(vb, floats(quad, 21)));

  // Blend state: additive, through the pipeline; and the same through the escape ABI's optional words.
  PipelineDesc add;
  add.blend.custom = add.blend.enable = true;
  add.blend.src_color = add.blend.dst_color = add.blend.src_alpha = add.blend.dst_alpha = graphics::BlendFactor::One;
  Handle pipe = pipeline_for(add);
  std::vector<uint32_t> img = picture(pipe, [](CommandWriter& w) { w.draw(3); });
  CHECK(!img.empty() && close8(img[40] >> 16 & 0xFF, 255) && close8(img[40] >> 8 & 0xFF, 0x66 + 0x80 > 255 ? 255 : 0x66 + 0x80) && close8(img[40] & 0xFF, 0x99));
  PipelineDesc mask_green;
  mask_green.blend.custom = true;
  mask_green.blend.write_mask = 0x2;
  img = picture(pipeline_for(mask_green), [](CommandWriter& w) { w.draw(3); });
  CHECK(!img.empty() && close8(img[40] >> 16 & 0xFF, 0x33) && close8(img[40] >> 8 & 0xFF, 0x80) && close8(img[40] & 0xFF, 0x99));   // (only green stored)
  {
    Handle vs = 0, fs = 0, via_escape = 0;
    CHECK(e.kernel.create_shader(kPid, e.dev, {ShaderKind::ColoredVertex, graphics::ShaderStage::Vertex, {}}, &vs) == status::kSuccess);
    CHECK(e.kernel.create_shader(kPid, e.dev, {ShaderKind::ColoredFragment, graphics::ShaderStage::Fragment, {}}, &fs) == status::kSuccess);
    ByteBuffer req(e.allocator);
    req.put32(static_cast<uint32_t>(EscapeOp::CreatePipeline)), req.put32(vs), req.put32(fs), req.put32(28), req.put32(2), req.put32(1), req.put32(0);
    req.put32(2), req.put32(0), req.put32(0);   // cull none, front face, flags
    const uint32_t attributes[12] = {0, 2, 0, 1, 3, 12, 0, 0, 0, 0, 0, 0};
    for (uint32_t w : attributes) req.put32(w);
    for (uint32_t w : {0u, 7u, 0u, 0u, 0u, 255u, 255u, 0u}) req.put32(w);   // stencil off
    for (uint32_t w : {7u, 1u, 1u, 1u, 1u, 1u, 1u, 0u, 0u, 15u, 0u}) req.put32(w);   // depth compare Always, additive, no per-instance attributes
    uint8_t reply[16];
    uint32_t reply_bytes = 0;
    const NtStatus st = e.kernel.escape(kPid, e.dev, req.view(), ByteView(reply, sizeof reply), &reply_bytes);
    CHECK(st == status::kSuccess && reply_bytes == 4);
    via_escape = ByteReader(ConstByteView(reply, 4)).le32();
    std::vector<uint32_t> a = picture(via_escape, [](CommandWriter& w) { w.draw(3); });
    CHECK(!a.empty() && a == picture(pipe, [](CommandWriter& w) { w.draw(3); }));
    // A malformed trailer is refused.
    req.put32(0);   // (no colour outputs)
    CHECK(e.kernel.escape(kPid, e.dev, req.view(), ByteView(reply, sizeof reply), &reply_bytes) == status::kInvalidParameter);
  }

  // Lines and points through the stream.
  {
    Handle plain = pipeline_for(PipelineDesc());
    const float line[14] = {-1, 0, 0.5f, 1, 1, 1, 1, 1, 0, 0.5f, 1, 1, 1, 1};
    CHECK(e.write(vb, floats(line, 14)));
    auto lit = [](const std::vector<uint32_t>& p) {
      unsigned n = 0;
      for (uint32_t v : p) n += v != 0xFF336699u;
      return n;
    };
    img = picture(plain, [](CommandWriter& w) { w.set_topology(Topology::LineList); w.draw(2); });
    CHECK(lit(img) >= 16 && lit(img) <= 18);
    const float strip[21] = {-1, -0.5f, 0.5f, 1, 1, 1, 1, 0, 0.5f, 0.5f, 1, 1, 1, 1, 0.9f, -0.5f, 0.5f, 1, 1, 1, 1};
    CHECK(e.write(vb, floats(strip, 21)));
    const std::vector<uint32_t> as_strip = picture(plain, [](CommandWriter& w) { w.set_topology(Topology::LineStrip); w.draw(3); });
    const float list[28] = {-1, -0.5f, 0.5f, 1, 1, 1, 1, 0, 0.5f, 0.5f, 1, 1, 1, 1, 0, 0.5f, 0.5f, 1, 1, 1, 1, 0.9f, -0.5f, 0.5f, 1, 1, 1, 1};
    CHECK(e.write(vb, floats(list, 28)));
    const std::vector<uint32_t> as_list = picture(plain, [](CommandWriter& w) { w.set_topology(Topology::LineList); w.draw(4); });
    if (as_strip != as_list) printf("  line strip: %zu pixels vs %zu; lit %u vs %u\n", as_strip.size(), as_list.size(), lit(as_strip), lit(as_list));
    CHECK(as_strip == as_list && lit(as_strip) > 12);
    const float points[14] = {-0.5f, -0.5f, 0.5f, 1, 1, 1, 1, 0.5f, 0.5f, 0.5f, 1, 1, 1, 1};
    CHECK(e.write(vb, floats(points, 14)));
    img = picture(plain, [](CommandWriter& w) { w.set_topology(Topology::PointList); w.draw(2); });
    CHECK(lit(img) == 2);
    // Back to triangles: the topology of the stream decides, not what the device last had.
    CHECK(e.write(vb, floats(quad, 21)));
    img = picture(plain, [](CommandWriter& w) {
      w.set_topology(Topology::PointList);
      w.draw(1);
      w.set_topology(Topology::TriangleList);
      w.draw(3);
    });
    CHECK(lit(img) == 256);
  }

  // Instancing: the per-instance attribute (location 1, two floats) shifts a small triangle; a vs_2_0 does the adding.
  {
    Asm vs(true, 2, 0);
    vs.dcl(0, 0, kRegInput, 0).dcl(5, 0, kRegInput, 1);
    vs.def(4, 0, 0, 0, 1);
    vs.op(kOpAdd, {Asm::dst(kRegRastOut, 0, 0x7), Asm::src(kRegInput, 0), Asm::src(kRegInput, 1)});
    vs.op(kOpMov, {Asm::dst(kRegRastOut, 0, 0x8), Asm::src(kRegConst, 4)});
    vs.def(5, 1, 1, 1, 1);
    vs.op(kOpMov, {Asm::dst(kRegAttrOut, 0), Asm::src(kRegConst, 5)});
    vs.end();
    Handle vsh = d3d9_shader(e, vs), psh = d3d9_shader(e, color_ps());
    PipelineDesc pd;
    pd.vertex_shader = vsh, pd.fragment_shader = psh, pd.vertex_stride = 20, pd.attribute_count = 2, pd.varying_count = 1;
    pd.attributes[0] = {0, graphics::VertexFormat::Float3, 0};
    pd.attributes[1] = {1, graphics::VertexFormat::Float2, 12, true};
    pd.cull_mode = graphics::CullMode::None;
    Handle instanced = 0;
    CHECK(e.kernel.create_pipeline(kPid, e.dev, pd, &instanced) == status::kSuccess);
    // elements 0, 1, 2: the triangle's three vertices AND the offsets of instances 0, 1, 2
    const float data[15] = {-1.0f, -1.0f, 0.5f, 0.0f, 0.0f, -0.7f, -1.0f, 0.5f, 1.0f, 0.0f, -1.0f, -0.7f, 0.5f, 0.0f, 1.0f};
    CHECK(e.write(vb, floats(data, 15)));
    img = picture(instanced, [](CommandWriter& w) { w.draw_instanced(3, 3); });
    unsigned lit = 0;
    for (uint32_t v : img) lit += v == 0xFFFFFFFFu;
    CHECK(!img.empty() && lit >= 3);   // (three small triangles; each covers a pixel or two)
    // instance 1 is at +1.0 in x: a lit pixel in the right half, instance 2 at +1.0 in y: one in the lower half
    bool right = false, lower = false;
    for (uint32_t y = 0; y < 16; ++y)
      for (uint32_t x = 0; x < 16; ++x)
        if (img[y * 16 + x] == 0xFFFFFFFFu) {
          right = right || x >= 8;
          lower = lower || y >= 8;
        }
    CHECK(right && lower);
    // A range that reaches past the vertex buffer's elements is refused when the stream is checked.
    CommandWriter w(e.allocator);
    w.begin_pass(0, kNoAllocation, kPassClearColor, {0, 0, 0, 1}, 1, 0);
    w.bind_pipeline(instanced);
    w.bind_vertex_buffer(1);
    w.draw_instanced(3, 100);
    w.end_pass();
    CHECK(e.render(w, {rt, vb}) == status::kInvalidParameter);
  }

  // A device without the features refuses the state and the draws rather than drawing something else.
  {
    FakeScreen screen(8, 6);
    graphics::SoftwareDevice software(e.allocator, &screen);
    SpyDevice plain_device(software);
    plain_device.feature_mask = 0;
    Kernel kernel(e.allocator);
    AdapterConfig c;
    c.display = &screen;
    kernel.add_adapter(plain_device, c);
    Handle adapter = 0, dev = 0, vsh = 0, fsh = 0, ctx = 0;
    CHECK(kernel.open_process(kPid) == status::kSuccess && kernel.open_adapter_from_index(kPid, 0, &adapter) == status::kSuccess && kernel.create_device(kPid, adapter, &dev) == status::kSuccess);
    CHECK(kernel.create_context(kPid, dev, ContextPriority::Normal, &ctx) == status::kSuccess);
    CHECK(kernel.create_shader(kPid, dev, {ShaderKind::ColoredVertex, graphics::ShaderStage::Vertex, {}}, &vsh) == status::kSuccess);
    CHECK(kernel.create_shader(kPid, dev, {ShaderKind::ColoredFragment, graphics::ShaderStage::Fragment, {}}, &fsh) == status::kSuccess);
    graphics::GraphicsPipelineDesc d;
    graphics::describe_colored_pipeline(&d);
    auto make = [&](PipelineDesc pd, Handle* out) {
      pd.vertex_shader = vsh, pd.fragment_shader = fsh;
      pd.vertex_stride = d.vertex_stride, pd.attribute_count = d.attribute_count, pd.varying_count = d.varying_count;
      for (uint32_t i = 0; i < d.attribute_count; ++i) pd.attributes[i] = d.attributes[i];
      return kernel.create_pipeline(kPid, dev, pd, out);
    };
    Handle h = 0;
    PipelineDesc blend, depth, instance;
    blend.blend.custom = true;
    depth.depth_compare = graphics::CompareOp::Greater;
    CHECK(make(blend, &h) == status::kNotSupported && make(depth, &h) == status::kNotSupported);
    CHECK(make(PipelineDesc(), &h) == status::kSuccess);
    AllocationDesc rd;
    rd.flags = kAllocRenderTarget, rd.format = Format::A8R8G8B8, rd.width = 8, rd.height = 8;
    Handle target = 0, buffer = 0;
    CHECK(kernel.create_allocation(kPid, dev, rd, &target) == status::kSuccess);
    AllocationDesc vd;
    vd.flags = kAllocVertexBuffer, vd.size = 256;
    CHECK(kernel.create_allocation(kPid, dev, vd, &buffer) == status::kSuccess);
    auto stream = [&](auto commands) {
      CommandWriter w(e.allocator);
      w.begin_pass(0, kNoAllocation, kPassClearColor, {0, 0, 0, 1}, 1, 0);
      w.bind_pipeline(h);
      w.bind_vertex_buffer(1);
      commands(w);
      w.end_pass();
      const Handle list[2] = {target, buffer};
      return kernel.render(kPid, ctx, w.view(), ArrayView<const Handle>(list, 2));
    };
    CHECK(stream([](CommandWriter& w) { w.draw(3); }) == status::kSuccess);
    CHECK(stream([](CommandWriter& w) { w.draw_instanced(3, 2); }) == status::kInvalidParameter);
    CHECK(stream([](CommandWriter& w) { w.set_topology(Topology::LineList); w.draw(2); }) == status::kInvalidParameter);
    CHECK(stream([](CommandWriter& w) { w.set_topology(Topology::TriangleStrip); w.draw(3); }) == status::kSuccess);   // (strips are expanded: any device)
    kernel.close_process(kPid);
  }
}

// --- Direct3D 10 system values and texel access (shaders from a real compiler) ------------------------------------------------------------------------------------------------

void test_dxbc_system_values() {
  Env e;
  const std::vector<uint8_t> ps_blob = read_blob("ps_texel.dxbc"), vs_blob = read_blob("vs_ids.dxbc");
  CHECK(!ps_blob.empty() && !vs_blob.empty());
  Handle rt = e.alloc(kTarget, Format::A8R8G8B8, 16, 16), vb = e.buffer(kAllocVertexBuffer, 256);
  const float identity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
  auto dxbc_shader = [&](const std::vector<uint8_t>& blob, graphics::ShaderStage stage, NtStatus* st = nullptr) {
    Handle h = 0;
    ShaderDesc d;
    d.kind = ShaderKind::Dxbc;
    d.stage = stage;
    d.code = view_of(blob);
    const NtStatus s = e.kernel.create_shader(kPid, e.dev, d, &h);
    if (st) *st = s;
    else CHECK(s == status::kSuccess);
    return h;
  };

  // --- ps_texel: Load, SampleLevel, GetDimensions, SV_Position, SV_IsFrontFace and SV_PrimitiveID of one texture and one triangle.
  {
    Asm vs(true, 2, 0);   // mov oPos, v0 ; mov oT0, v1   (the uv)
    vs.dcl(0, 0, kRegInput, 0).dcl(5, 0, kRegInput, 1);
    vs.op(kOpMov, {Asm::dst(kRegRastOut, 0), Asm::src(kRegInput, 0)});
    vs.op(kOpMov, {Asm::dst(kRegTexOut, 0), Asm::src(kRegInput, 1)});
    vs.end();
    Handle texture = e.alloc(kTexture, Format::A8R8G8B8, 8, 8, 4);
    for (uint32_t level = 0; level < 4; ++level) {
      const uint32_t n = 8u >> level;
      std::vector<uint32_t> pixels(n * n);
      for (uint32_t y = 0; y < n; ++y)
        for (uint32_t x = 0; x < n; ++x) pixels[y * n + x] = level == 0 ? 0xFF000000u | (x * 20) << 16 | (y * 20) << 8 : 0xFF000000u | (100 + x * 10) << 16 | (50 + y * 10) << 8 | 40;
      CHECK(e.write(texture, ConstByteView(reinterpret_cast<const uint8_t*>(pixels.data()), pixels.size() * 4), kLockDiscard, level));
    }
    Handle vsh = d3d9_shader(e, vs), psh = dxbc_shader(ps_blob, graphics::ShaderStage::Fragment);
    PipelineDesc pd;
    pd.vertex_shader = vsh, pd.fragment_shader = psh, pd.vertex_stride = 20, pd.attribute_count = 2, pd.varying_count = 3, pd.texture_count = 1;
    pd.attributes[0] = {0, graphics::VertexFormat::Float3, 0};
    pd.attributes[1] = {1, graphics::VertexFormat::Float2, 12};
    pd.cull_mode = graphics::CullMode::None;
    Handle pipe = 0;
    CHECK(e.kernel.create_pipeline(kPid, e.dev, pd, &pipe) == status::kSuccess);
    auto draw = [&](const float* v) {
      CHECK(e.write(vb, floats(v, 15)));
      CommandWriter w(e.allocator);
      w.begin_pass(0, kNoAllocation, kPassClearColor, {0, 0, 0, 1}, 1, 0);
      w.bind_pipeline(pipe);
      w.bind_vertex_buffer(1);
      graphics::SamplerDesc sampler;
      sampler.mip_filter = graphics::MipFilter::Nearest;
      w.bind_texture(0, 2, sampler);
      w.draw(3);
      w.end_pass();
      CHECK(e.render(w, {rt, vb, texture}) == status::kSuccess);
    };
    const float ccw[15] = {-1, -1, 0.5f, 0.5f, 0.5f, 3, -1, 0.5f, 0.5f, 0.5f, -1, 3, 0.5f, 0.5f, 0.5f};
    const float cw[15] = {-1, -1, 0.5f, 0.5f, 0.5f, -1, 3, 0.5f, 0.5f, 0.5f, 3, -1, 0.5f, 0.5f, 0.5f};
    draw(ccw);
    const uint32_t a = e.pixel(rt, 5, 3);
    draw(cw);
    const uint32_t b = e.pixel(rt, 5, 3);
    // a = Load(2, 1, 0): (40, 20, 0); b = SampleLevel(0.5, 0.5, 1.0): level 1 texel (2, 2) = (120, 70, 40); the rest: (5.5/16, 3.5/16, facing, 0) + (1, 1, 0.5, 0)
    auto expected = [&](uint32_t channel_index, bool front) {
      const float load[3] = {40.0f / 255, 20.0f / 255, 0.0f}, level[3] = {120.0f / 255, 70.0f / 255, 40.0f / 255};
      const float extra[3] = {5.5f / 16 + 1.0f, 3.5f / 16 + 1.0f, (front ? 1.0f : 0.0f) + 0.5f};
      float v = load[channel_index] * 0.5f + level[channel_index] * 0.25f + extra[channel_index];
      return static_cast<uint32_t>((v < 0 ? 0 : v > 1 ? 1 : v) * 255.0f + 0.5f);
    };
    // (red and green saturate at 1 here; blue tells the facing)
    const bool a_front = close8(a & 0xFF, expected(2, true), 3);
    const bool b_front = close8(b & 0xFF, expected(2, true), 3);
    CHECK(a_front != b_front);   // the two windings face opposite ways
    CHECK(close8(a & 0xFF, expected(2, a_front), 3) && close8(b & 0xFF, expected(2, b_front), 3));
    CHECK((a >> 16 & 0xFF) == 255 && (a >> 8 & 0xFF) == 255);
  }

  // --- vs_ids: SV_VertexID and SV_InstanceID, with per-instance attributes
  {
    Handle vsh = dxbc_shader(vs_blob, graphics::ShaderStage::Vertex), psh = d3d9_shader(e, color_ps());
    PipelineDesc pd;
    pd.vertex_shader = vsh, pd.fragment_shader = psh, pd.vertex_stride = 20, pd.attribute_count = 2, pd.varying_count = 1;
    pd.attributes[0] = {0, graphics::VertexFormat::Float3, 0};
    pd.attributes[1] = {1, graphics::VertexFormat::Float2, 12, true};
    pd.cull_mode = graphics::CullMode::None;
    Handle pipe = 0;
    CHECK(e.kernel.create_pipeline(kPid, e.dev, pd, &pipe) == status::kSuccess);
    const float data[15] = {-1.0f, -1.0f, 0.5f, 0.0f, 0.0f, -0.4f, -1.0f, 0.5f, 1.0f, 0.0f, -1.0f, -0.4f, 0.5f, 1.8f, 0.0f};
    CHECK(e.write(vb, floats(data, 15)));
    CommandWriter w(e.allocator);
    w.begin_pass(0, kNoAllocation, kPassClearColor, {0, 0, 0, 1}, 1, 0);
    w.bind_pipeline(pipe);
    w.bind_vertex_buffer(1);
    w.draw_instanced(3, 3);
    w.end_pass();
    CHECK(e.render(w, {rt, vb}) == status::kSuccess);
    uint32_t red_by_band[3] = {0, 0, 0};
    for (uint32_t y = 0; y < 16; ++y)
      for (uint32_t x = 0; x < 16; ++x) {
        const uint32_t p = e.pixel(rt, x, y);
        if (p == 0xFF000000u) continue;
        const uint32_t band = x < 4 ? 0 : x < 12 ? 1 : 2;
        if ((p >> 16 & 0xFF) > red_by_band[band]) red_by_band[band] = p >> 16 & 0xFF;
      }
    // instance 0: red 0; instance 1 (offset +1.0 clip = +8 pixels): red 0.1 = 26; instance 2 (+1.8 = +14.4 pixels): red 0.2 = 51
    CHECK(red_by_band[0] <= 2 && close8(red_by_band[1], 26, 2) && close8(red_by_band[2], 51, 2));
  }

  // The TGSI text of the same shaders (what virgl gets): the vertex shader with SV_VertexID / SV_InstanceID and integer arithmetic, and the pixel shader with Load, SampleLevel,
  // GetDimensions, SV_Position and SV_IsFrontFace, run by the TGSI machine against the interpreter; SV_PrimitiveID has no TGSI form that virglrenderer's GLSL takes.
  {
    struct TexelSet final : graphics::ITextureSet {
      graphics::Vec4 sample(uint32_t unit, float u, float v) const override { return {u, v, 0.25f + 0.1f * static_cast<float>(unit), 1.0f}; }
      graphics::Vec4 sample_lod(uint32_t unit, float u, float v, float lod) const override { return {u * 0.5f, v * 0.5f, lod * 0.125f, 0.5f + 0.1f * static_cast<float>(unit)}; }
      graphics::Vec4 fetch(uint32_t unit, int32_t x, int32_t y, uint32_t level) const override { return {x * 0.0625f, y * 0.125f, static_cast<float>(level) * 0.25f, 0.75f + static_cast<float>(unit) * 0.0625f}; }
      bool size(uint32_t unit, uint32_t level, uint32_t* width, uint32_t* height, uint32_t* levels) const override {
        (void)unit;
        *width = 8u >> level, *height = 4u >> level, *levels = 3;
        return level < 3;
      }
    };
    HostAllocator allocator;
    NtStatus st = 0;
    UniquePointer<D3d9Program> vsp = compile_dxbc(allocator, vs_blob, &st);
    CHECK(vsp && st == status::kSuccess);
    String text(allocator);
    CHECK(vsp->emit_tgsi(&text) == status::kSuccess);
    const std::string vs_text(text.data(), text.size());
    CHECK(vs_text.find("DCL SV[0], VERTEXID") != std::string::npos && vs_text.find("DCL SV[1], INSTANCEID") != std::string::npos);
    unsigned vertex_cases = 0;
    for (uint32_t vertex_id = 0; vertex_id < 4; ++vertex_id)
      for (uint32_t instance_id = 0; instance_id < 5; ++instance_id) {
        graphics::Vec4 attributes[4] = {{0.1f * static_cast<float>(vertex_id), -0.3f, 0.5f, 1}, {0.25f, -0.5f, 0, 0}, {}, {}};
        graphics::VertexContext context;
        context.vertex_id = vertex_id, context.instance_id = instance_id;
        graphics::ShaderIo io = {};
        uint8_t constants[128] = {};
        vsp->run_vertex_ex(attributes, constants, context, &io);
        TgsiMachine m;
        for (unsigned i = 0; i < 2; ++i) m.in[i][0] = attributes[i].x, m.in[i][1] = attributes[i].y, m.in[i][2] = attributes[i].z, m.in[i][3] = attributes[i].w;
        m.sv[0][0] = __builtin_bit_cast(float, vertex_id), m.sv[1][0] = __builtin_bit_cast(float, instance_id);
        CHECK(m.run(vs_text));
        CHECK(near_value(m.out[0][0], io.position.x) && near_value(m.out[0][1], io.position.y) && near_value(m.out[0][2], io.position.z) && near_value(m.out[0][3], io.position.w));
        CHECK(near_value(m.out[1][0], io.varying[0].x) && near_value(m.out[1][1], io.varying[0].y) && near_value(m.out[1][2], io.varying[0].z) && near_value(m.out[1][3], io.varying[0].w));
        ++vertex_cases;
      }
    CHECK(vertex_cases == 20);

    const std::vector<uint8_t> system_blob = read_blob("ps_system.dxbc");
    UniquePointer<D3d9Program> psp = compile_dxbc(allocator, system_blob, &st);
    CHECK(psp && st == status::kSuccess);
    String ps_string(allocator);
    CHECK(psp->emit_tgsi(&ps_string) == status::kSuccess);
    const std::string ps_text(ps_string.data(), ps_string.size());
    CHECK(ps_text.find("TXF") != std::string::npos && ps_text.find("TXL") != std::string::npos && ps_text.find("TXQ") != std::string::npos);
    CHECK(ps_text.find("DCL IN[16], POSITION") != std::string::npos && ps_text.find("DCL IN[17], FACE") != std::string::npos);
    TexelSet texels;
    unsigned pixel_cases = 0;
    for (bool front : {true, false})
      for (float x : {0.5f, 5.5f, 17.5f})
        for (float uv : {0.0f, 0.3f, 0.9f}) {
          graphics::Vec4 varyings[4] = {};
          varyings[2] = {uv, 1.0f - uv, 0, 0};
          graphics::FragmentContext context;
          context.position = {x, 9.5f, 0.25f, 0.8f};
          context.front_facing = front;
          graphics::Vec4 color_room[graphics::kMaxColorTargets] = {};
          graphics::Vec4& color = color_room[0];
          uint8_t constants[128] = {};
          const bool alive = psp->run_fragment_ex(varyings, context, constants, &texels, &color);
          TgsiMachine m;
          m.textures = &texels;
          m.in[2][0] = varyings[2].x, m.in[2][1] = varyings[2].y;
          m.in[16][0] = context.position.x, m.in[16][1] = context.position.y, m.in[16][2] = context.position.z, m.in[16][3] = context.position.w;
          m.in[17][0] = front ? 1.0f : -1.0f;
          CHECK(m.run(ps_text) && alive && !m.killed);
          CHECK(near_value(m.out[0][0], color.x) && near_value(m.out[0][1], color.y) && near_value(m.out[0][2], color.z) && near_value(m.out[0][3], color.w));
          ++pixel_cases;
        }
    CHECK(pixel_cases == 18);

    // A constant buffer of 64 registers, indexed by an integer register: the TGSI text reads the same registers through the address register.
    {
      const std::vector<uint8_t> cbuf_blob = read_blob("ps_cbuf.dxbc");
      UniquePointer<D3d9Program> cp = compile_dxbc(allocator, cbuf_blob, &st);
      CHECK(cp && st == status::kSuccess);
      String cstr(allocator);
      CHECK(cp->emit_tgsi(&cstr) == status::kSuccess);
      const std::string ctext(cstr.data(), cstr.size());
      CHECK(ctext.find("UARL") != std::string::npos && ctext.find("DCL CONST[0..63]") != std::string::npos);
      uint8_t constants[64 * 16];
      TgsiMachine base;
      for (unsigned j = 0; j < 64; ++j) {
        const float row[4] = {j / 64.0f, 1.0f - j / 64.0f, static_cast<float>(j & 1), 0.25f};
        memcpy(constants + j * 16, row, 16);
        for (unsigned k = 0; k < 4; ++k) base.cst[j][k] = row[k];
      }
      unsigned cases = 0;
      for (float x : {0.0f, 0.2f, 0.55f, 0.99f})
        for (float y : {0.0f, 0.4f, 0.97f}) {
          graphics::Vec4 varyings[4] = {};
          varyings[0] = {0.5f, 0.25f, 0.75f, 1.0f};
          varyings[2] = {x, y, 0, 0};
          graphics::Vec4 color_room[graphics::kMaxColorTargets] = {};
          graphics::Vec4& color = color_room[0];
          CHECK(cp->run_fragment_ex(varyings, graphics::FragmentContext(), constants, nullptr, &color));
          TgsiMachine m = base;
          m.in[0][0] = 0.5f, m.in[0][1] = 0.25f, m.in[0][2] = 0.75f, m.in[0][3] = 1.0f;
          m.in[2][0] = x, m.in[2][1] = y;
          CHECK(m.run(ctext));
          CHECK(near_value(m.out[0][0], color.x) && near_value(m.out[0][1], color.y) && near_value(m.out[0][2], color.z) && near_value(m.out[0][3], color.w));
          ++cases;
        }
      CHECK(cases == 12);
    }

    UniquePointer<D3d9Program> with_primitive = compile_dxbc(allocator, ps_blob, &st);
    CHECK(with_primitive && st == status::kSuccess);
    String refused(allocator);
    CHECK(with_primitive->emit_tgsi(&refused) == status::kSuccess && __builtin_strstr(refused.c_str(), "PRIMID"));   // (the primitive number is a PRIMID input)
  }

  // A device without system values or texel access does not get these shaders (they would read zeros).
  {
    FakeScreen screen(8, 6);
    graphics::SoftwareDevice software(e.allocator, &screen);
    SpyDevice plain_device(software);
    plain_device.feature_mask = 0;
    Kernel kernel(e.allocator);
    AdapterConfig c;
    c.display = &screen;
    kernel.add_adapter(plain_device, c);
    Handle adapter = 0, dev = 0, h = 0;
    CHECK(kernel.open_process(kPid) == status::kSuccess && kernel.open_adapter_from_index(kPid, 0, &adapter) == status::kSuccess && kernel.create_device(kPid, adapter, &dev) == status::kSuccess);
    ShaderDesc d;
    d.kind = ShaderKind::Dxbc;
    d.stage = graphics::ShaderStage::Fragment;
    d.code = view_of(ps_blob);
    CHECK(kernel.create_shader(kPid, dev, d, &h) == status::kNotSupported);
    d.stage = graphics::ShaderStage::Vertex;
    d.code = view_of(vs_blob);
    CHECK(kernel.create_shader(kPid, dev, d, &h) == status::kNotSupported);
    kernel.close_process(kPid);
  }

  // Truncations and bit flips of the two blobs.
  {
    HostAllocator allocator;
    unsigned compiled = 0, refused = 0;
    uint32_t seed = 2024;
    NtStatus st = 0;
    for (const std::vector<uint8_t>* blob : {&ps_blob, &vs_blob}) {
      for (size_t cut = 0; cut < blob->size(); cut += 3) {
        compile_dxbc(allocator, std::vector<uint8_t>(blob->begin(), blob->begin() + static_cast<long>(cut)), &st);
        st == status::kSuccess ? ++compiled : ++refused;
      }
      for (int i = 0; i < 3000; ++i) {
        std::vector<uint8_t> t = *blob;
        seed = seed * 1664525u + 1013904223u;
        t[(seed >> 8) % t.size()] ^= static_cast<uint8_t>(1u << ((seed >> 3) % 8));
        UniquePointer<D3d9Program> p = compile_dxbc(allocator, t, &st);
        if (st == status::kSuccess) {
          ++compiled;
          graphics::Vec4 v[4] = {};
          graphics::Vec4 color_room[graphics::kMaxColorTargets] = {};
          graphics::Vec4& color = color_room[0];
          uint8_t constants[128] = {};
          graphics::ShaderIo io;
          p->run_vertex_ex(v, constants, graphics::VertexContext(), &io);
          TexSet textures;
          p->run_fragment_ex(v, graphics::FragmentContext(), constants, &textures, &color);
        } else {
          ++refused;
        }
      }
    }
    printf("  dxbc system value fuzz: %u compiled, %u refused\n", compiled, refused);
  }
}

}  // namespace

int main() {
  test_handle_table();
  test_adapters_and_processes();
  test_allocation_checks();
  test_system_limit();
  test_clear_and_formats();
  test_triangle();
  test_texture_copy_mips();
  test_stream_errors();
  test_stream_fuzz();
  test_fault();
  test_fences_and_scheduler();
  test_residency();
  test_present();
  test_sharing_and_teardown();
  test_teardown_with_work_queued();
  test_escape();
  test_device_teardown();
  test_d3d9_bytecode();
  test_d3d9_tgsi();
  test_d3d9_tgsi_reaches_device();
  test_dxbc();
  test_dxbc_real();
  test_dxbc_constant_buffers();
  test_dxbc_integers();
  test_dxbc_loops();
  test_topologies();
  test_d3d10_state();
  test_dxbc_system_values();
  test_device_forms();
  test_sm3_and_sm51();
  test_ps1();
  test_device_suite();
  test_stream_output();
  test_stream_output_extras();
  test_root_signature_parse();
  test_d3d12_binding();
  test_d3d12_compute();
  test_d3d12_buffer_views();
  test_d3d12_barriers();
  test_d3d12_heaps();
  test_d3d12_reserved();
  test_dxil();
  test_compute_shared();
  test_geometry_extras();
  test_alpha_to_coverage();
  test_conformance();
  test_occlusion_queries();
  test_texture_kinds();
  test_independent_blend();
  test_compute();
  test_compute_kernel();
  test_multisample();
  test_geometry_kernel();
  test_formats();
  test_tessellation_kernel();
  if (g_failed) {
    printf("%d check(s) failed\n", g_failed);
    return 1;
  }
  printf("DxgkrnlTest: all passed\n");
  return 0;
}
