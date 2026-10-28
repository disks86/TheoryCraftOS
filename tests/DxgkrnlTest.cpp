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
#include "Kernel/Dxgkrnl/CommandStream.hpp"
#include "Kernel/Dxgkrnl/DxgKernel.hpp"
#include "Utility/ByteStream.hpp"

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
Handle make_colored_pipeline(Env& e, bool textured = false) {
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
  pd.cull_mode = graphics::CullMode::None;
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
  w.draw(3);
  w.draw_indexed(3);
  w.end_pass();
  w.copy_region(3, 0, 0, 3, 1, 1, 2, 2);
  w.generate_mipmaps(3);
  CHECK(w.ok());
  std::vector<uint8_t> good(w.view().data(), w.view().data() + w.view().size());
  std::vector<Handle> list = {rt, vb, ib, tex};
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
  { Asm a(false, 1, 1); a.end(); d3d9_shader(e, a, &s); CHECK(s == status::kNotSupported); }                                        // ps_1_1
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
          graphics::Vec4 attrs[4] = {}, vary[4] = {}, color;
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

class TgsiMachine {
 public:
  float temp[64][4] = {}, in[16][4] = {}, out[16][4] = {}, cst[4100][4] = {}, imm[64][4] = {};
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
        for (int k = 0; k < 4; ++k) {
          char* e = nullptr;
          imm[i][k] = strtof(p, &e);
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
      if (op == "IF") stack.push_back(i);
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
          if (o == "IF") ++depth;
          if (o == "ENDIF") {
            if (!depth) break;
            --depth;
          }
        }
        pc = e + 1;
        continue;
      }
      if (op == "ENDIF") continue;
      if (op == "IF") {
        float c[4];
        if (!fetch(args.at(0), c)) return false;
        if (c[0] == 0) {   // to after the ELSE or the ENDIF
          size_t e = pc;
          int depth = 0;
          for (; e < body.size(); ++e) {
            const std::string o = body[e].substr(0, body[e].find(' '));
            if (o == "IF") ++depth;
            else if ((o == "ELSE" || o == "ENDIF") && !depth) break;
            else if (o == "ENDIF") --depth;
          }
          pc = e + 1;
        }
        continue;
      }
      if (!execute(op, args)) return false;
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
    if (r.file == "IN") return index >= 0 && index < 16 ? in[index] : nullptr;
    if (r.file == "OUT") return index >= 0 && index < 16 ? out[index] : nullptr;
    if (r.file == "IMM") return index >= 0 && index < 64 ? imm[index] : nullptr;
    if (r.file == "CONST") return index >= 0 && index < 4100 ? cst[index] : zero;
    return nullptr;
  }
  bool fetch(const std::string& text, float v[4]) {
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
    if (op == "ARL" || op == "ARR") {
      if (!need(2) || !fetch(args[1], a)) return false;
      addr = static_cast<int>(floorf(op == "ARR" ? a[0] + 0.5f : a[0]));
      return true;
    }
    if (op == "KILL_IF") {
      if (!need(1) || !fetch(args[0], a)) return false;
      for (float x : a) killed = killed || x < 0;
      return true;
    }
    if (!need(2) || !fetch(args[1], a)) return false;
    if (args.size() > 2 && op != "TEX" && op != "TXP" && !fetch(args[2], b)) return false;
    if (args.size() > 3 && op != "TEX" && op != "TXP" && !fetch(args[3], c)) return false;
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
      else if (op != "TEX" && op != "TXP") return false;
    }
    (void)scalar;
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
        graphics::Vec4 color = {};
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
  graphics::DeviceInfo info() const override { return inner_.info(); }
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
    put(shader, ((vertex ? 1u : 0u) << 16) | (version_major << 4)), put(shader, static_cast<uint32_t>(code.size() + 2));
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
        {24, 2, [](float a, float b, float) { return a == b ? 1.0f : 0.0f; }},
        {57, 2, [](float a, float b, float) { return a != b ? 1.0f : 0.0f; }},
        {49, 2, [](float a, float b, float) { return a < b ? 1.0f : 0.0f; }},
        {29, 2, [](float a, float b, float) { return a >= b ? 1.0f : 0.0f; }},
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
          if (rt == 2 && alu.opcode != 75) {   // (sqrt of a negative number: NaN, or 0 once saturated; the tokens give the root of its magnitude)
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
          graphics::Vec4 color = {};
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
      a.ins(30, {dx_dst(kOpTemp, 0, 0xF), dx_src(kOpTemp, 0), dx_src(kOpTemp, 1)});   // iadd
      a.ins(62, {});
      compile_dxbc(allocator, a.build(), &st);
      CHECK(st == status::kNotSupported);
      DxbcAsm loop = dx_pixel_shader();
      loop.ins(48, {});   // loop
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
          graphics::Vec4 varyings[4] = {}, color;
          uint8_t constants[128] = {};
          p->run_fragment(varyings, constants, &textures, &color);
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
    for (const std::vector<uint8_t>* blob : {&vs_blob, &color_blob, &tex_blob, &math_blob}) {
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
          graphics::Vec4 v[4] = {}, col;
          uint8_t constants[128] = {};
          p->run_fragment(v, constants, &textures, &col);
          graphics::ShaderIo io;
          p->run_vertex(v, constants, &io);
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
    for (uint32_t op : kOps)
      for (uint32_t n = 0; n < 24; ++n) {
        const uint32_t sources = op == 1 || op == 19 || op == 34 || op == 35 ? 1 : (op == 4 || op == 18 || op == 88) ? 3 : 2;
        const Asm original = one_instruction(true, op, sources, n);
        UniquePointer<D3d9Program> program;
        if (D3d9Program::compile(allocator, original.bytes(), &program) != status::kSuccess) continue;
        List<uint32_t> tokens(allocator);
        if (program->emit_svga_tokens(original.bytes(), &tokens) != status::kSuccess) {
          ++refused;
          continue;
        }
        Asm rewritten(true, 2, 0);
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
    CHECK(checked > 100 && wrong == 0);

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
    CHECK(spy.had_function && spy.svga_tokens > 8 && spy.eu_program == graphics::EuProgram::None);
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
  test_device_forms();
  test_sm3_and_sm51();
  if (g_failed) {
    printf("%d check(s) failed\n", g_failed);
    return 1;
  }
  printf("DxgkrnlTest: all passed\n");
  return 0;
}
