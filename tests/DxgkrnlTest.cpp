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
#include "Graphics/Graphics3D/VertexFetch.hpp"
#include "Graphics/Graphics3D/Virgl/VirglVertexPatch.hpp"
#include "Kernel/Dxgkrnl/Conformance.hpp"
#include "Kernel/Dxgkrnl/CommandStream.hpp"
#include "Kernel/Dxgkrnl/ConformanceBlobs.hpp"
#include "Kernel/Dxgkrnl/DxgKernel.hpp"
#include "Utility/ByteStream.hpp"
#include "DxgkrnlDeviceSuite.hpp"
#include "TgsiMachine.hpp"
#include "../src/Windows/d3d9/shaders.inc"

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

bool close8(uint32_t a, uint32_t b, uint32_t tolerance = 2) { return (a > b ? a - b : b - a) <= tolerance; }

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
      {image(kAllocDepthStencil | kTexture | kTarget, Format::D32Float, 4, 4), status::kInvalidParameter},       // (a depth buffer read as a texture is not a colour target)
      {image(kAllocStaging | kTarget, Format::A8R8G8B8, 4, 4), status::kInvalidParameter},
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
  // Drawing with the texture unbound is refused.
  CommandWriter w_unbound(e.allocator);
  w_unbound.begin_pass(0, kNoAllocation, 0, {0, 0, 0, 0}, 1, 0);
  w_unbound.bind_pipeline(pipe);
  w_unbound.bind_vertex_buffer(2);
  w_unbound.draw(3);
  w_unbound.end_pass();
  CHECK(e.render(w_unbound, {rt, tex, vb}) == status::kInvalidParameter);
  Handle tex_rt = e.alloc(kTexture | kTarget, Format::A8R8G8B8, 8, 8);
  // A pass into an image a texture unit still holds from an earlier draw lets the unit go: the pass is taken, and a draw in it that samples through the unit
  // is refused (it would read the image it draws into).
  CommandWriter w_self(e.allocator);
  w_self.bind_texture(0, 0, graphics::SamplerDesc());
  w_self.begin_pass(0, kNoAllocation, 0, {0, 0, 0, 0}, 1, 0);
  w_self.end_pass();
  CHECK(e.render(w_self, {tex_rt}) == status::kSuccess);
  CommandWriter w_self_draw(e.allocator);
  w_self_draw.bind_texture(0, 1, graphics::SamplerDesc());
  w_self_draw.begin_pass(1, kNoAllocation, 0, {0, 0, 0, 0}, 1, 0);
  w_self_draw.bind_pipeline(pipe);
  w_self_draw.bind_vertex_buffer(2);
  w_self_draw.push_constants(floats(identity, 16));
  w_self_draw.draw(3);
  w_self_draw.end_pass();
  CHECK(e.render(w_self_draw, {rt, tex_rt, vb}) == status::kInvalidParameter);
  CommandWriter w_self_bind(e.allocator);
  w_self_bind.begin_pass(1, kNoAllocation, 0, {0, 0, 0, 0}, 1, 0);
  w_self_bind.bind_texture(0, 1, graphics::SamplerDesc());
  w_self_bind.end_pass();
  CHECK(e.render(w_self_bind, {rt, tex_rt, vb}) == status::kInvalidParameter);
  // Render to texture in one stream: sample it, draw into it, sample it again - the second draw sees what the pass between drew.
  CommandWriter w_ping(e.allocator);
  w_ping.begin_pass(1, kNoAllocation, kPassClearColor, {1, 0, 0, 1}, 1, 0);      // tex_rt red
  w_ping.end_pass();
  w_ping.begin_pass(0, kNoAllocation, kPassClearColor, {0, 0, 0, 1}, 1, 0);      // rt from tex_rt
  w_ping.bind_pipeline(pipe);
  w_ping.bind_vertex_buffer(2);
  w_ping.push_constants(floats(identity, 16));
  w_ping.bind_texture(0, 1, graphics::SamplerDesc());
  w_ping.draw(3);
  w_ping.end_pass();
  w_ping.begin_pass(1, kNoAllocation, kPassClearColor, {0, 1, 0, 1}, 1, 0);      // tex_rt green, while unit 0 still holds it
  w_ping.end_pass();
  w_ping.begin_pass(0, kNoAllocation, 0, {0, 0, 0, 1}, 1, 0);
  w_ping.bind_pipeline(pipe);
  w_ping.bind_vertex_buffer(2);
  w_ping.push_constants(floats(identity, 16));
  w_ping.bind_texture(0, 1, graphics::SamplerDesc());
  w_ping.draw(3);
  w_ping.end_pass();
  CHECK(e.render(w_ping, {rt, tex_rt, vb}) == status::kSuccess && e.pixel(rt, 4, 4) == 0xFF00FF00);
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
  Handle src = e.alloc(kTexture, Format::A8R8G8B8, 4, 4), dst = e.alloc(kTexture, Format::A8R8G8B8, 4, 4), other_format = e.alloc(kTexture, Format::A16B16G16R16F, 4, 4);
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
  CHECK(e.render(bad2, {dst, other_format}) == status::kInvalidParameter);   // formats of other sizes
  CommandWriter same_size(e.allocator);
  same_size.copy_region(0, 0, 0, 1, 1, 1, 2, 2);
  const Handle opaque = e.alloc(kTexture, Format::X8R8G8B8, 4, 4);
  CHECK(e.render(same_size, {opaque, src}) == status::kSuccess && (e.pixel(opaque, 0, 0) & 0xFFFFFF) == (e.pixel(src, 1, 1) & 0xFFFFFF));   // (pixels of one size: the bytes)

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

// Vertex buffer slots with no buffer (kFeature3NullVertexBuffers) and of stride 0 (kFeature3ZeroStride) on the CPU renderer: the colour of slot 1 unbound reads (0, 0, 0, 0) - black
// over the red clear, a Float4 colour's alpha 0 too -, also after BindVertexBuffer of kNoVertexBuffer took a buffer away; a stride of 0 gives every vertex the first colour.
void test_unbound_vertex_buffers() {
  Env e;
  Handle vs = 0, fs = 0, two_slots = 0, zero_stride = 0;
  CHECK(e.kernel.create_shader(kPid, e.dev, {ShaderKind::ColoredVertex, graphics::ShaderStage::Vertex, {}}, &vs) == status::kSuccess);
  CHECK(e.kernel.create_shader(kPid, e.dev, {ShaderKind::ColoredFragment, graphics::ShaderStage::Fragment, {}}, &fs) == status::kSuccess);
  graphics::GraphicsPipelineDesc d;
  graphics::describe_colored_pipeline(&d);
  PipelineDesc pd;
  pd.vertex_shader = vs, pd.fragment_shader = fs;
  pd.vertex_stride = 12, pd.attribute_count = d.attribute_count, pd.varying_count = d.varying_count;
  for (uint32_t i = 0; i < d.attribute_count; ++i) pd.attributes[i] = d.attributes[i];
  pd.attributes[1].slot = 1, pd.attributes[1].offset = 0;   // (the colour from a buffer of its own)
  pd.cull_mode = graphics::CullMode::None;
  pd.slot_strides[1] = 16;
  CHECK(e.kernel.create_pipeline(kPid, e.dev, pd, &two_slots) == status::kSuccess);
  pd.slot_strides[1] = 0;
  CHECK(e.kernel.create_pipeline(kPid, e.dev, pd, &zero_stride) == status::kSuccess);
  pd.attributes[1].offset = 2048;   // (at stride 0 an attribute lies within the first 2048 bytes)
  Handle refused = 0;
  CHECK(e.kernel.create_pipeline(kPid, e.dev, pd, &refused) == status::kInvalidParameter);
  Handle rt = e.alloc(kTarget, Format::A8R8G8B8, 8, 8), positions = e.buffer(kAllocVertexBuffer, 36), colours = e.buffer(kAllocVertexBuffer, 48);
  const float p[9] = {-1, -1, 0.5f, 3, -1, 0.5f, -1, 3, 0.5f};
  const float c[12] = {0, 1, 0, 1, 0, 0, 1, 1, 0, 0, 1, 1};   // (green, then blue twice)
  CHECK(e.write(positions, floats(p, 9)) && e.write(colours, floats(c, 12)));
  const uint32_t black = 0x00000000u, green = 0xFF00FF00u;   // (the colour (0, 0, 0, 0): a Float4 attribute has its w)
  const float identity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
  for (int k = 0; k < 3; ++k) {
    CommandWriter w(e.allocator);
    w.begin_pass(0, kNoAllocation, kPassClearColor, {1, 0, 0, 1}, 1, 0);
    w.bind_pipeline(k == 2 ? zero_stride : two_slots);
    w.push_constants(floats(identity, 16));
    w.set_viewport({0, 0, 8, 8, 0, 1});
    w.set_scissor({0, 0, 8, 8});
    w.bind_vertex_buffer(1);
    if (k != 0) w.bind_vertex_buffer_slot(1, 2);
    if (k == 1) w.unbind_vertex_buffer_slot(1);   // (bound, then taken away)
    w.draw(3);
    w.end_pass();
    CHECK(e.render(w, {rt, positions, colours}) == status::kSuccess);
    const uint32_t got = e.pixel(rt, 2, 2);
    if (got != (k == 2 ? green : black)) printf("  unbound vertex buffers %d: %08x\n", k, got);
    CHECK(got == (k == 2 ? green : black));
  }
  // (an allocation index of kNoVertexBuffer takes an offset of 0 only)
  CommandWriter bad(e.allocator);
  bad.begin_pass(0, kNoAllocation, 0, {}, 1, 0);
  bad.bind_vertex_buffer_slot(1, kNoVertexBuffer, 4);
  bad.end_pass();
  CHECK(e.render(bad, {rt}) == status::kInvalidParameter);
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
  { CommandWriter w(e.allocator); w.begin_pass(0, kNoAllocation, 0, {}, 1, 0); w.bind_pipeline(pipe); w.draw(3); w.end_pass(); expect("draw without vertices", w, {rt}, status::kSuccess); }   // (kFeature3NullVertexBuffers: the slot reads zeros)
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

// The present tap: each present that showed an image hands it over when the tap wants it (as the display shows it), none when it does not, none for
// a refused present; it follows adapters added later; taken away, nothing more.
struct RecordingTap final : IPresentTap {
  std::vector<uint32_t> buffer;
  std::vector<uint32_t> last;
  uint32_t width = 0, height = 0, asked = 0, read = 0;
  bool want = true;
  ArrayView<uint32_t> present_wanted(uint32_t w, uint32_t h) override {
    ++asked;
    if (!want) return ArrayView<uint32_t>();
    buffer.assign(size_t(w) * h, 0);
    return ArrayView<uint32_t>(buffer.data(), buffer.size());
  }
  void present_read(uint32_t w, uint32_t h) override {
    ++read;
    width = w;
    height = h;
    last = buffer;
  }
};

void test_present_tap() {
  Env e;
  RecordingTap tap;
  e.kernel.set_present_tap(&tap);
  Handle back = e.alloc(kTarget | kAllocPresent, Format::A8R8G8B8, 8, 6), plain = e.alloc(kTarget, Format::A8R8G8B8, 8, 6);
  CHECK(e.clear(back, {0, 0, 1, 1}) == status::kSuccess);
  PresentArgs flip;
  flip.source = back;
  CHECK(e.kernel.present(kPid, e.ctx, flip) == status::kSuccess && tap.asked == 1 && tap.read == 1 && tap.width == 8 && tap.height == 6);
  CHECK(tap.last.size() == 48 && (tap.last[0] & 0xFFFFFF) == 0x0000FF && (tap.last[47] & 0xFFFFFF) == 0x0000FF);
  // a blit present: the shown image (the destination) with the region copied in
  CHECK(e.clear(plain, {1, 0, 0, 1}) == status::kSuccess);
  PresentArgs blit;
  blit.source = plain;
  blit.destination = back;
  blit.source_rect = {0, 0, 2, 2};
  CHECK(e.kernel.present(kPid, e.ctx, blit) == status::kSuccess && tap.read == 2 && tap.last.size() == 48);
  CHECK((tap.last[0] & 0xFFFFFF) == 0xFF0000 && (tap.last[9] & 0xFFFFFF) == 0xFF0000 && (tap.last[2] & 0xFFFFFF) == 0x0000FF);
  // not wanted: asked, not read; refused presents: not even asked
  tap.want = false;
  CHECK(e.kernel.present(kPid, e.ctx, flip) == status::kSuccess && tap.asked == 3 && tap.read == 2);
  PresentArgs bad;
  bad.source = plain;
  CHECK(e.kernel.present(kPid, e.ctx, bad) == status::kInvalidParameter && tap.asked == 3);
  // taken away: nothing
  tap.want = true;
  e.kernel.set_present_tap(nullptr);
  CHECK(e.kernel.present(kPid, e.ctx, flip) == status::kSuccess && tap.asked == 3);
  // a kernel whose tap is set before its adapter: the adapter has it
  HostAllocator a;
  FakeScreen screen(4, 4);
  graphics::SoftwareDevice device(a, &screen);
  Kernel k(a);
  RecordingTap early;
  k.set_present_tap(&early);
  AdapterConfig c;
  c.display = &screen;
  CHECK(k.add_adapter(device, c) == 0);
  Handle adapter = 0, dev = 0, ctx = 0, img = 0;
  CHECK(k.open_process(kPid) == status::kSuccess && k.open_adapter_from_index(kPid, 0, &adapter) == status::kSuccess && k.create_device(kPid, adapter, &dev) == status::kSuccess &&
        k.create_context(kPid, dev, ContextPriority::Normal, &ctx) == status::kSuccess);
  AllocationDesc d;
  d.flags = kTarget | kAllocPresent;
  d.format = Format::A8R8G8B8;
  d.width = 4, d.height = 4;
  CHECK(k.create_allocation(kPid, dev, d, &img) == status::kSuccess);
  PresentArgs f2;
  f2.source = img;
  CHECK(k.present(kPid, ctx, f2) == status::kSuccess && early.read == 1 && early.width == 4);
  k.set_present_tap(nullptr);
  k.close_process(kPid);
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
  auto pipeline_request = [&](ByteBuffer& b, Handle v, Handle f, uint32_t cull = 0 /*none*/) {
    b.clear();
    b.put32(static_cast<uint32_t>(EscapeOp::CreatePipeline));
    for (uint32_t w : {v, f, 28u, 2u, 1u, 0u, cull, 0u, 0u}) b.put32(w);
    const uint32_t attributes[4][3] = {{0, 2, 0}, {1, 3, 12}, {0, 0, 0}, {0, 0, 0}};
    for (const auto& a : attributes) for (uint32_t w : a) b.put32(w);
    for (uint32_t w : {0u, 7u, 0u, 0u, 0u, 255u, 255u, 0u}) b.put32(w);
  };
  // (the cull word's kCullConservative bit: conservative rasterization; other high bits are refused)
  pipeline_request(req, vs, fs, kCullConservative | 1);
  CHECK(call(req) == status::kSuccess);
  CHECK(e.kernel.destroy_pipeline(kPid, handle_of_reply()) == status::kSuccess);
  pipeline_request(req, vs, fs, 0x200);
  CHECK(call(req) == status::kInvalidParameter);
  pipeline_request(req, vs, fs, kCullConservative | 3);
  CHECK(call(req) == status::kInvalidParameter);
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
          graphics::Vec4 attrs[16] = {}, vary[16] = {};
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
  for (int i = 0; i < 160; ++i) {
    Handle h = d3d9_shader(e, color_ps(), &last);
    if (last != status::kSuccess) break;
    shaders.push_back(h);
  }
  CHECK(last == status::kInsufficientResources && shaders.size() >= 100 && shaders.size() < 160);
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

using tgsi_test::TgsiMachine;
using tgsi_test::tgsi_if;
using tgsi_test::tgsi_integer_op;

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
        graphics::Vec4 attributes[16];
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
        graphics::Vec4 varyings[16];
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
      // A def constant read as an operand, and ones a decimal with twelve fraction digits cannot say exactly (a large one, a tiny one that would read as 0, a
      // negative zero): those go in UINT32 immediates of their bits.
      Asm d(false, 2, 0);
      d.def(9, 0.125f, -3.5f, 100.0f, 0.0078125f);
      d.op(4, {Asm::dst(kRegColorOut, 0), Asm::src(kRegInput, 0), Asm::src(kRegConst, 9), Asm::src(kRegConst, 0)});
      d.end();
      CHECK(f.check(d, "def") == status::kSuccess);
      Asm big(false, 2, 0);
      big.def(9, 1e20f, 1e-30f, -0.0f, 0.1f);
      big.op(kOpMov, {Asm::dst(kRegColorOut, 0), Asm::src(kRegConst, 9)});
      big.end();
      CHECK(f.check(big, "big def") == status::kSuccess);
      {
        UniquePointer<D3d9Program> program;
        CHECK(D3d9Program::compile(allocator, big.bytes(), &program) == status::kSuccess);
        String text(allocator);
        CHECK(program && program->emit_tgsi(&text) == status::kSuccess);
        const std::string t(text.data(), text.size());
        char want[96];
        snprintf(want, sizeof want, "UINT32 {%u, %u, %u, %u}", __builtin_bit_cast(uint32_t, 1e20f), __builtin_bit_cast(uint32_t, 1e-30f), __builtin_bit_cast(uint32_t, -0.0f), __builtin_bit_cast(uint32_t, 0.1f));
        CHECK(t.find(want) != std::string::npos);
      }
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
  uint32_t feature2_mask = 0xFFFFFFFFu;  // ... and of the second word
  uint32_t feature3_mask = 0xFFFFFFFFu;  // ... and of the third
  bool no_blend_constant = false;        // the device says it has no constant blend factors
  graphics::DeviceInfo info() const override {
    graphics::DeviceInfo i = inner_.info();
    i.features &= feature_mask;
    i.features2 &= feature2_mask;
    i.features3 &= feature3_mask;
    if (no_blend_constant) i.blend_constant = false;
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
    graphics::Vec4 values[16];
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
        graphics::Vec4 attributes[16];
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
          graphics::Vec4 varyings[16];
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
      graphics::Vec4 varyings[16];
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
      loop.ins(76, {dx_src(kOpTemp, 0)});   // switch (with no case and no endswitch: malformed - switches are taken, tests/DxbcFlowTest.cpp)
      compile_dxbc(allocator, loop.build(), &st);
      CHECK(st == status::kInvalidParameter);
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
          graphics::Vec4 varyings[graphics::kMaxVaryings] = {}, color[graphics::kMaxColorTargets];   // (the varyings a program may read: any of them)
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
      graphics::Vec4 attributes[16], varyings[16];
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
          graphics::Vec4 v[16] = {}, col[graphics::kMaxColorTargets];
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
          graphics::GeometryInputData in = {};
          for (uint32_t k = 0; k < 3; ++k) in.position[k] = {static_cast<float>(k), 1.0f, 0.5f, 1.0f};
          graphics::GeometryOutputData out = {};
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

// Varyings the compiler packs two to a register (vs_packed / ps_packed: TEXCOORD1 in o1.xy, TEXCOORD2 in o1.zw, TEXCOORD3 in o2.xy), as ANGLE's shaders for Chrome's
// video quads have them. Both stages must send such a register through the same varying: before, the vertex shader took its first element's slot and the pixel shader
// its last one's, so the Y and U planes were sampled at coordinates nobody wrote and Chrome's video on virgl showed only the V plane (green and red). Checked on the CPU
// (run_vertex into run_fragment) and as TGSI (the vertex shader's OUT[n + 1] into the pixel shader's IN[n], as virgl links GENERIC[n]).
void test_packed_varyings() {
  HostAllocator allocator;
  {
    TexSet textures;
    NtStatus st = 0;
    const std::vector<uint8_t> vs_blob = read_blob("vs_packed.dxbc"), ps_blob = read_blob("ps_packed.dxbc");
    CHECK(!vs_blob.empty() && !ps_blob.empty());
    UniquePointer<D3d9Program> vs = compile_dxbc(allocator, vs_blob, &st);
    CHECK(st == status::kSuccess && vs);
    UniquePointer<D3d9Program> ps = compile_dxbc(allocator, ps_blob, &st);
    CHECK(st == status::kSuccess && ps && ps->uses_textures());
    if (!vs || !ps) return;
    String vs_text(allocator), ps_text(allocator);
    CHECK(vs->emit_tgsi(&vs_text, graphics::kMaxVaryings) == status::kSuccess && ps->emit_tgsi(&ps_text, graphics::kMaxVaryings) == status::kSuccess);
    const std::string vs_source(vs_text.data(), vs_text.size()), ps_source(ps_text.data(), ps_text.size());
    for (unsigned set = 0; set < 4; ++set) {
      uint8_t constants[128] = {};
      const float identity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
      memcpy(constants, identity, sizeof identity);
      graphics::Vec4 attributes[16] = {};
      const float(*in)[4] = kInputs[set];
      attributes[0] = {in[0][0], in[0][1], in[0][2], 1};
      attributes[1] = {in[1][0], in[1][1], in[1][2], in[1][3]};
      const float* c = in[1];
      const graphics::Vec4 y = textures.sample(0, c[0] * 2, c[1] * 2), u = textures.sample(1, c[2] * 2, c[3] * 2), v = textures.sample(2, c[1] * 2, c[0] * 2);

      graphics::ShaderIo io = {};
      vs->run_vertex(attributes, constants, &io);
      graphics::Vec4 colour = {};
      const bool alive = ps->run_fragment(io.varying, constants, &textures, &colour);
      const bool cpu = alive && near_value(colour.x, y.x) && near_value(colour.y, u.x) && near_value(colour.z, v.x) && near_value(colour.w, 1);
      if (!cpu) printf("  packed varyings on the CPU (set %u): got (%f, %f, %f), want (%f, %f, %f)\n", set, colour.x, colour.y, colour.z, y.x, u.x, v.x);
      CHECK(cpu);

      TgsiMachine vm;
      memcpy(vm.cst[0], identity, sizeof identity);
      for (unsigned i = 0; i < 2; ++i) {
        const float a[4] = {attributes[i].x, attributes[i].y, attributes[i].z, attributes[i].w};
        memcpy(vm.in[i], a, sizeof a);
      }
      bool ok = vm.run(vs_source);
      TgsiMachine pm;
      pm.textures = &textures;
      for (int i = 0; i + 1 < TgsiMachine::kRegs; ++i) memcpy(pm.in[i], vm.out[i + 1], sizeof pm.in[i]);
      ok = ok && pm.run(ps_source) && !pm.killed && near_value(pm.out[0][0], y.x) && near_value(pm.out[0][1], u.x) && near_value(pm.out[0][2], v.x);
      if (!ok) printf("  packed varyings as TGSI (set %u): got (%f, %f, %f), want (%f, %f, %f)\n%s%s", set, pm.out[0][0], pm.out[0][1], pm.out[0][2], y.x, u.x, v.x, vs_source.c_str(), ps_source.c_str());
      CHECK(ok);
    }
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
          graphics::Vec4 attributes[16];
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
          graphics::Vec4 attributes[16] = {{0.3f, -0.4f, 0.7f, 1.0f}, {0.1f, 0.2f, 0.3f, 0.4f}, {}, {}};
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
        graphics::Vec4 values[16];
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
        graphics::Vec4 values[16];
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
        graphics::Vec4 values[16];
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
          graphics::Vec4 values[16];
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
        graphics::Vec4 varyings[16];
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
          graphics::Vec4 varyings[16];
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
          graphics::Vec4 v[16] = {}, c;
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
  CHECK(refused([&](CommandWriter& c) { c.begin_query(5, 0); }));                                                  // never ended
  CHECK(refused([&](CommandWriter& c) { c.begin_pass(0, kNoAllocation, 0, {}, 1, 0); c.begin_query(5, 0); c.end_pass(); }));   // still open at the end of the stream
  CHECK(refused([&](CommandWriter& c) { c.begin_pass(0, kNoAllocation, 0, {}, 1, 0); c.end_query(); c.end_pass(); }));       // never begun
  CHECK(refused([&](CommandWriter& c) { c.begin_query(5, 0); c.end_query(); c.end_query(); }));                           // ended twice
  CHECK(refused([&](CommandWriter& c) { c.begin_query(5, 0); c.end_query(5, 8); c.end_query(); }));                      // ending a place no query writes
  CHECK(refused([&](CommandWriter& c) { c.begin_query(5, 0); c.end_query(4, 0); c.end_query(); }));                      // ... of another allocation
  CHECK(refused([&](CommandWriter& c) { c.begin_query(5, 0); c.end_query(9, 0); c.end_query(); }));                      // ... of none
  CHECK(refused([&](CommandWriter& c) { c.begin_query(5, 8); c.begin_query(5, 8); c.end_query(); c.end_query(); }));     // two open queries writing one place
  CHECK(refused([&](CommandWriter& c) { c.begin_query(5, 0, 11); c.begin_query(5, 40, 0); c.end_query(); c.end_query(); }));   // ... or overlapping (88 bytes from 0)
  CHECK(refused([&](CommandWriter& c) { c.begin_query(5, 0, 1u << 8); c.end_query(); }));                               // a stream for a kind without one
  CHECK(refused([&](CommandWriter& c) { c.begin_query(5, 0, 1u << 12); c.end_query(); }));                              // bits the kind word does not have
  CHECK(refused([&](CommandWriter& c) { c.begin_query(5, 0, 14); c.end_query(); }));                                    // no such kind
  CHECK(refused([&](CommandWriter& c) { c.begin_query(5, 0, 6 | kQueryAccumulate); c.end_query(); }));                  // a time is not added up
  CHECK(refused([&](CommandWriter& c) { c.begin_query(5, 0, 9 | kQueryAccumulate); c.end_query(); }));
  CHECK(refused([&](CommandWriter& c) { c.begin_query(5, 0, 11); c.end_query(); }));                                    // 88 bytes do not fit in 64
  CHECK(refused([&](CommandWriter& c) { c.begin_query(5, 0, kQueryKindPipelineStatistics1); c.end_query(); }));
  CHECK(refused([&](CommandWriter& c) { c.begin_query(5, 56, kQueryKindSoStatistics); c.end_query(); }));               // 16 bytes at 56 do not either
  CHECK(refused([&](CommandWriter& c) { c.begin_pass(0, kNoAllocation, 0, {}, 1, 0); c.begin_query(5, 4); c.end_query(); c.end_pass(); }));       // not a multiple of 8
  CHECK(refused([&](CommandWriter& c) { c.begin_pass(0, kNoAllocation, 0, {}, 1, 0); c.begin_query(5, 64); c.end_query(); c.end_pass(); }));      // past the end
  CHECK(refused([&](CommandWriter& c) { c.begin_pass(0, kNoAllocation, 0, {}, 1, 0); c.begin_query(5, 0xFFFFFFF8u); c.end_query(); c.end_pass(); }));
  CHECK(refused([&](CommandWriter& c) { c.begin_pass(0, kNoAllocation, 0, {}, 1, 0); c.begin_query(3, 0); c.end_query(); c.end_pass(); }));       // a vertex buffer
  CHECK(refused([&](CommandWriter& c) { c.begin_pass(0, kNoAllocation, 0, {}, 1, 0); c.begin_query(0, 0); c.end_query(); c.end_pass(); }));       // an image
  CHECK(refused([&](CommandWriter& c) { c.begin_pass(0, kNoAllocation, 0, {}, 1, 0); c.begin_query(9, 0); c.end_query(); c.end_pass(); }));       // no such allocation
  // The limit: kMaxOcclusionQueries queries are fine, one more is not.
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
  // Several queries open at once, nested, across passes and a flush, ended by name or the last one open, and added to an earlier stream's.
  {
    AllocationDesc sd;
    sd.flags = kAllocQueryResult, sd.size = 1024;
    Handle stats = 0;
    CHECK(e.kernel.create_allocation(kPid, e.dev, sd, &stats) == status::kSuccess);
    const uint32_t kStatistics = static_cast<uint32_t>(graphics::QueryKind::PipelineStatistics), kPixels = static_cast<uint32_t>(graphics::QueryKind::PsInvocations);
    const uint32_t kPredicate = static_cast<uint32_t>(graphics::QueryKind::OcclusionPredicate);
    CommandWriter c(e.allocator);
    c.begin_query(5, 0);                    // A: the samples of everything
    c.begin_query(5, 8, kStatistics);       // B: 8 .. 96
    c.begin_query(5, 96, kPixels);          // D
    c.begin_query(5, 120, kPredicate);      // F: anything passed
    c.begin_pass(0, 2, kPassClearColor | kPassClearDepth, {0, 0, 0, 1}, 1, 0);
    c.bind_pipeline(pipe);
    c.push_constants(floats(identity, 16));
    c.bind_vertex_buffer(3);
    c.draw(3);                              // 256
    c.begin_query(5, 104);                  // C: the far triangle (fails the depth test)
    c.begin_query(5, 112, kPredicate);      // E: the same, as a predicate
    c.bind_vertex_buffer(4);
    c.draw(3);
    c.end_query(5, 104);
    c.end_query();                          // (E: the last open)
    c.end_pass();
    c.copy_region(1, 0, 0, 0, 0, 0, 16, 16);   // (runs what came before: the open queries go on in the next command buffer)
    c.begin_pass(1, 2, kPassClearColor | kPassClearDepth, {0, 0, 0, 1}, 1, 0);
    c.bind_pipeline(pipe);
    c.push_constants(floats(identity, 16));
    c.bind_vertex_buffer(3);
    c.set_scissor({0, 0, 8, 16});
    c.draw(3);                              // 128
    c.end_pass();
    c.end_query(5, 8);
    c.end_query(5, 96);
    c.end_query(5, 120);
    c.end_query();                          // (A)
    CHECK(c.ok() && e.render(c, {rt, rt2, ds, vb, far_vb, stats}) == status::kSuccess);
    CHECK(query_count(e, stats, 0) == 384 && query_count(e, stats, 13) == 0 && query_count(e, stats, 14) == 0 && query_count(e, stats, 15) == 1);
    // (B: three draws of a triangle, each given to the clipper and not outside the view; no other stage)
    const uint64_t pixels = query_count(e, stats, 12);
    CHECK(pixels >= 384);
    const uint64_t expect[11] = {9, 3, 9, 0, 0, 3, 3, pixels, 0, 0, 0};
    for (uint32_t k = 0; k < 11; ++k) CHECK(query_count(e, stats, 1 + k) == expect[k]);

    // Added to what the places hold: the samples of another full triangle, and a predicate that now passes.
    CommandWriter more(e.allocator);
    more.begin_query(5, 0, kQueryAccumulate);
    more.begin_query(5, 112, kPredicate | kQueryAccumulate);
    more.begin_query(5, 8, kStatistics | kQueryAccumulate);
    more.begin_pass(0, 2, kPassClearColor | kPassClearDepth, {0, 0, 0, 1}, 1, 0);
    more.bind_pipeline(pipe);
    more.push_constants(floats(identity, 16));
    more.bind_vertex_buffer(3);
    more.draw(3);
    more.end_pass();
    more.end_query();
    more.end_query();
    more.end_query();
    CHECK(more.ok() && e.render(more, {rt, rt2, ds, vb, far_vb, stats}) == status::kSuccess);
    CHECK(query_count(e, stats, 0) == 640 && query_count(e, stats, 14) == 1 && query_count(e, stats, 1) == 12 && query_count(e, stats, 2) == 4);

    // A strip is drawn as a list, but counted as Direct3D counts it: a quad of four vertices is four vertices, two primitives, four vertex shader runs. The kernel's own kinds:
    // PipelineStatistics1 (the eleven values and three zeros).
    Handle quad = e.buffer(kAllocVertexBuffer, 4 * 28);
    const float q[28] = {-1, -1, 0.5f, 1, 0, 0, 1, 1, -1, 0.5f, 1, 0, 0, 1, -1, 1, 0.5f, 1, 0, 0, 1, 1, 1, 0.5f, 1, 0, 0, 1};
    CHECK(e.write(quad, floats(q, 28)));
    CommandWriter strip(e.allocator);
    strip.begin_query(5, 200, kStatistics);
    strip.begin_query(5, 304, kQueryKindPipelineStatistics1);
    strip.begin_query(5, 416, static_cast<uint32_t>(graphics::QueryKind::IaVertices));
    strip.begin_query(5, 424, static_cast<uint32_t>(graphics::QueryKind::VsInvocations));
    strip.begin_query(5, 432);
    strip.begin_pass(0, 2, kPassClearColor | kPassClearDepth, {0, 0, 0, 1}, 1, 0);
    strip.bind_pipeline(pipe);
    strip.push_constants(floats(identity, 16));
    strip.bind_vertex_buffer(3);
    strip.set_topology(Topology::TriangleStrip);
    strip.draw(4);
    strip.end_pass();
    for (uint32_t k = 0; k < 5; ++k) strip.end_query();
    CHECK(strip.ok() && e.render(strip, {rt, rt2, ds, quad, far_vb, stats}) == status::kSuccess);
    CHECK(query_count(e, stats, 54) == 256);   // (the quad covers the target)
    CHECK(query_count(e, stats, 52) == 4 && query_count(e, stats, 53) == 4);
    const uint64_t strip_expect[11] = {4, 2, 4, 0, 0, 2, 2, query_count(e, stats, 25 + 7), 0, 0, 0};
    for (uint32_t k = 0; k < 11; ++k) CHECK(query_count(e, stats, 25 + k) == strip_expect[k] && query_count(e, stats, 304 / 8 + k) == strip_expect[k]);
    for (uint32_t k = 11; k < 14; ++k) CHECK(query_count(e, stats, 304 / 8 + k) == 0);
    CHECK(strip_expect[7] >= 256);
    e.kernel.destroy_allocation(kPid, quad);
    e.kernel.destroy_allocation(kPid, stats);
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
  // (with the device's kFeature2LayeredTargets and kFeature2StorageImages: layered render targets and storage images, of any colour format; a wide volume too)
  CHECK(create(TextureKind::Texture3D, 4, 4, 4, kAllocTexture | kAllocRenderTarget) == status::kSuccess);
  CHECK(create(TextureKind::Texture2DArray, 3, 4, 4, kAllocTexture | kAllocRenderTarget | kAllocStorageImage, Format::A32B32G32R32F) == status::kSuccess);
  CHECK(create(TextureKind::Texture3D, 4, 4, 4, kAllocTexture, Format::A16B16G16R16F) == status::kSuccess);
  CHECK(create(TextureKind::Texture3D, 4, 4, 4, kAllocTexture | kAllocStorageImage, Format::R32Uint) == status::kSuccess);
  // What cannot be: no layers, too many, a cube that is not six squares, a storage cube, a storage image without kAllocTexture or of sRGB, mipmaps, staging, layers on a plain texture.
  CHECK(create(TextureKind::Texture3D, 0, 4, 4) == status::kInvalidParameter);
  CHECK(create(TextureKind::Texture2DArray, 257, 4, 4) == status::kInvalidParameter);
  CHECK(create(TextureKind::TextureCube, 5, 4, 4) == status::kInvalidParameter);
  CHECK(create(TextureKind::TextureCube, 6, 4, 8) == status::kInvalidParameter);
  CHECK(create(TextureKind::TextureCube, 6, 4, 4, kAllocTexture | kAllocStorageImage) == status::kSuccess);   // (its faces an array to shaders: kFeature2MipmappedLayers)
  CHECK(create(TextureKind::Texture2DArray, 2, 4, 4, kAllocStorageImage) == status::kInvalidParameter);
  CHECK(create(TextureKind::Texture2DArray, 2, 4, 4, kAllocTexture | kAllocStorageImage, Format::A8R8G8B8Srgb) == status::kInvalidParameter);
  CHECK(create(TextureKind::Texture3D, 4, 4, 4, kAllocTexture, Format::A8R8G8B8, 2) == status::kSuccess);   // (levels and staging: kFeature2MipmappedLayers, test_mipmapped_layers)
  CHECK(create(TextureKind::Texture3D, 4, 4, 4, kAllocStaging) == status::kSuccess);
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
  // (a texture of another kind than the shader declares: the draw is left out - Direct3D leaves what it reads undefined - and the context goes on)
  CHECK(draw_with(volume_pipe, plain) == status::kSuccess);
  CHECK(draw_with(plain_pipe, volume) == status::kSuccess);

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
  CHECK(make(e, true, graphics::kBlendFactorCount, &pipe) == status::kInvalidParameter);   // (a factor that does not exist, in a state of a target)
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

// The constant blend factors: taken by a device that has them (the CPU renderer), refused with kNotSupported by one that does not, in the pipeline's own state and in a
// target's; a pipeline that names them only in a state that does not blend is not refused.
void test_blend_constant() {
  Env e;
  auto make = [](Kernel& kernel, Handle dev, graphics::BlendFactor factor, bool in_target, bool enable, Handle* out) {
    Handle vs = 0, fs = 0;
    CHECK(kernel.create_shader(kPid, dev, {ShaderKind::ColoredVertex, graphics::ShaderStage::Vertex, {}}, &vs) == status::kSuccess);
    CHECK(kernel.create_shader(kPid, dev, {ShaderKind::ColoredFragment, graphics::ShaderStage::Fragment, {}}, &fs) == status::kSuccess);
    graphics::GraphicsPipelineDesc gd;
    graphics::describe_colored_pipeline(&gd);
    PipelineDesc pd;
    pd.vertex_shader = vs, pd.fragment_shader = fs, pd.vertex_stride = gd.vertex_stride, pd.attribute_count = 2, pd.varying_count = 1;
    pd.attributes[0] = gd.attributes[0], pd.attributes[1] = gd.attributes[1];
    graphics::BlendState& b = in_target ? pd.target_blend[0] : pd.blend;
    pd.independent_blend = in_target;
    b.custom = true, b.enable = enable, b.src_color = factor, b.dst_alpha = graphics::BlendFactor::InvConstantAlpha;
    b.constant[0] = 0.25f, b.constant[3] = 0.5f;
    return kernel.create_pipeline(kPid, dev, pd, out);
  };
  Handle pipe = 0;
  CHECK(make(e.kernel, e.dev, graphics::BlendFactor::ConstantColor, false, true, &pipe) == status::kSuccess);
  CHECK(make(e.kernel, e.dev, graphics::BlendFactor::InvConstantColor, true, true, &pipe) == status::kSuccess);
  FakeScreen screen(8, 6);
  graphics::SoftwareDevice software(e.allocator, &screen);
  SpyDevice limited(software);
  limited.no_blend_constant = true;
  Kernel kernel(e.allocator);
  AdapterConfig c;
  c.display = &screen;
  kernel.add_adapter(limited, c);
  Handle adapter = 0, dev = 0;
  CHECK(kernel.open_process(kPid) == status::kSuccess && kernel.open_adapter_from_index(kPid, 0, &adapter) == status::kSuccess && kernel.create_device(kPid, adapter, &dev) == status::kSuccess);
  AdapterCaps caps;
  CHECK(kernel.query_adapter_caps(kPid, adapter, &caps) == status::kSuccess && !caps.blend_constant);
  CHECK(make(kernel, dev, graphics::BlendFactor::ConstantColor, false, true, &pipe) == status::kNotSupported);
  CHECK(make(kernel, dev, graphics::BlendFactor::ConstantAlpha, true, true, &pipe) == status::kNotSupported);
  CHECK(make(kernel, dev, graphics::BlendFactor::ConstantColor, false, false, &pipe) == status::kSuccess);   // (blending off: the factors are not used)
  AdapterCaps full;
  Handle first = 0;
  CHECK(e.kernel.open_adapter_from_index(kPid, 0, &first) == status::kSuccess && e.kernel.query_adapter_caps(kPid, first, &full) == status::kSuccess && full.blend_constant);
}

// Conservative rasterization (kFeature2ConservativeRaster, the CreatePipeline cull word's kCullConservative): a sliver that holds no pixel centre draws nothing without it and every pixel
// its area touches with it; a device without the feature refuses the pipeline.
void test_conservative_raster() {
  Env e;
  auto make = [](Kernel& kernel, Handle dev, bool conservative, Handle* out) {
    Handle vs = 0, fs = 0;
    CHECK(kernel.create_shader(kPid, dev, {ShaderKind::ColoredVertex, graphics::ShaderStage::Vertex, {}}, &vs) == status::kSuccess);
    CHECK(kernel.create_shader(kPid, dev, {ShaderKind::ColoredFragment, graphics::ShaderStage::Fragment, {}}, &fs) == status::kSuccess);
    graphics::GraphicsPipelineDesc gd;
    graphics::describe_colored_pipeline(&gd);
    PipelineDesc pd;
    pd.vertex_shader = vs, pd.fragment_shader = fs, pd.vertex_stride = gd.vertex_stride, pd.attribute_count = 2, pd.varying_count = 1;
    pd.attributes[0] = gd.attributes[0], pd.attributes[1] = gd.attributes[1];
    pd.cull_mode = graphics::CullMode::None;
    pd.conservative = conservative;
    return kernel.create_pipeline(kPid, dev, pd, out);
  };
  Handle plain = 0, conservative = 0;
  CHECK(make(e.kernel, e.dev, false, &plain) == status::kSuccess && make(e.kernel, e.dev, true, &conservative) == status::kSuccess);
  // (pixels (2.1, 2.1), (5.9, 2.1), (2.1, 2.4) of a 16 x 16 target: x = px / 8 - 1, y = 1 - py / 8)
  auto ndc_x = [](float px) { return px / 8.0f - 1.0f; };
  auto ndc_y = [](float py) { return 1.0f - py / 8.0f; };
  const float v[21] = {ndc_x(2.1f), ndc_y(2.1f), 0.5f, 0, 1, 0, 1, ndc_x(5.9f), ndc_y(2.1f), 0.5f, 0, 1, 0, 1, ndc_x(2.1f), ndc_y(2.4f), 0.5f, 0, 1, 0, 1};
  Handle rt = e.alloc(kTarget, Format::A8R8G8B8, 16, 16), vb = e.buffer(kAllocVertexBuffer, 3 * 28);
  CHECK(e.write(vb, floats(v, 21)));
  const float identity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
  auto draw = [&](Handle pipe) {
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
  };
  draw(plain);
  uint32_t lit = 0;
  for (uint32_t y = 0; y < 16; ++y)
    for (uint32_t x = 0; x < 16; ++x) lit += e.pixel(rt, x, y) != 0xFF000000 ? 1 : 0;
  CHECK(lit == 0);
  draw(conservative);
  const uint32_t row = e.pixel(rt, 2, 2) == 0xFF00FF00 ? 2 : 13;   // (the row of pixel y 2: counted from the top or, as the target's rows are kept, from the bottom)
  for (uint32_t x = 2; x <= 5; ++x) CHECK(e.pixel(rt, x, row) == 0xFF00FF00);
  lit = 0;
  for (uint32_t y = 0; y < 16; ++y)
    for (uint32_t x = 0; x < 16; ++x) lit += e.pixel(rt, x, y) != 0xFF000000 ? 1 : 0;
  CHECK(lit == 4);
  FakeScreen screen(8, 6);
  graphics::SoftwareDevice software(e.allocator, &screen);
  SpyDevice limited(software);
  limited.feature2_mask = ~graphics::kFeature2ConservativeRaster;
  Kernel kernel(e.allocator);
  AdapterConfig c;
  c.display = &screen;
  kernel.add_adapter(limited, c);
  Handle adapter = 0, dev = 0, pipe = 0;
  CHECK(kernel.open_process(kPid) == status::kSuccess && kernel.open_adapter_from_index(kPid, 0, &adapter) == status::kSuccess && kernel.create_device(kPid, adapter, &dev) == status::kSuccess);
  CHECK(make(kernel, dev, true, &pipe) == status::kNotSupported);
  CHECK(make(kernel, dev, false, &pipe) == status::kSuccess);
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


// Storage images (BindUavImage, kAllocStorageImage) on the CPU renderer: a compute shader writing a 2D array, the atomics on R32 images of one, two and three dimensions (one an
// R32_UINT view of an R32_FLOAT allocation), and what is refused.
void test_storage_images_kernel() {
  Env e;
  auto blob_shader = [&](const char* name, graphics::ShaderStage stage, Handle* out) {
    const std::vector<uint8_t> code = read_blob(name);
    ShaderDesc sd;
    sd.kind = ShaderKind::Dxbc, sd.stage = stage, sd.code = ConstByteView(code.data(), code.size());
    return !code.empty() && e.kernel.create_shader(kPid, e.dev, sd, out) == status::kSuccess;
  };
  Handle fill_cs = 0, atomic_cs = 0, fill = 0, atomic = 0;
  CHECK(blob_shader("cs_uav_array.dxbc", graphics::ShaderStage::Compute, &fill_cs) && blob_shader("cs_uav_atomic.dxbc", graphics::ShaderStage::Compute, &atomic_cs));
  PipelineDesc pd;
  pd.compute_shader = fill_cs;
  CHECK(e.kernel.create_pipeline(kPid, e.dev, pd, &fill) == status::kSuccess);
  pd.compute_shader = atomic_cs;
  CHECK(e.kernel.create_pipeline(kPid, e.dev, pd, &atomic) == status::kSuccess);
  auto image = [&](graphics::TextureKind kind, uint32_t layers, uint32_t w, uint32_t h, Format f, uint32_t flags = kAllocTexture | kAllocStorageImage) {
    AllocationDesc d;
    d.flags = flags, d.format = f, d.width = w, d.height = h, d.kind = kind, d.layers = layers;
    Handle out = 0;
    return e.kernel.create_allocation(kPid, e.dev, d, &out) == status::kSuccess ? out : Handle{0};
  };
  using graphics::TextureKind;
  const Handle picture = image(TextureKind::Texture2DArray, 3, 8, 8, Format::A8B8G8R8), plain = image(TextureKind::Texture2D, 1, 8, 8, Format::A8B8G8R8, kAllocTexture);
  const Handle counts = image(TextureKind::Texture2D, 1, 4, 1, Format::R32Uint), least = image(TextureKind::Texture2D, 1, 4, 1, Format::R32Sint);
  const Handle flags = image(TextureKind::Texture2D, 1, 4, 1, Format::R32Float), most = image(TextureKind::Texture3D, 2, 2, 2, Format::R32Sint);
  CHECK(picture && plain && counts && least && flags && most);
  auto run = [&](auto build) {
    CommandWriter w(e.allocator);
    build(w);
    return w.ok() ? e.render(w, {picture, plain, counts, least, flags, most}) : status::kNoMemory;
  };
  CHECK(run([&](CommandWriter& w) { w.bind_pipeline(fill); w.bind_uav_image(0, 0, 0, 0, 3); w.dispatch(1, 1, 3); }) == status::kSuccess);
  LockResult lock;
  CHECK(e.kernel.lock(kPid, picture, kLockReadOnly, 0, &lock) == status::kSuccess);
  int wrong = 0;
  auto unorm8 = [](float v) { return static_cast<uint8_t>(v * 255.0f + 0.5f); };
  for (uint32_t z = 0; z < 3; ++z)
    for (uint32_t i = 0; i < 64; ++i) {
      const uint8_t* t = lock.data.data() + (z * 64 + i) * 4;
      if (t[0] != unorm8((i % 8) / 8.0f) || t[1] != unorm8((i / 8) / 8.0f) || t[2] != unorm8(z / 3.0f) || t[3] != 255) ++wrong;
    }
  e.kernel.unlock(kPid, picture);
  CHECK(wrong == 0);
  // The atomics: starting values written by the CPU (counts and bits 0, the least the largest int, the most the smallest).
  auto fill_words = [&](Handle a, uint32_t value, uint32_t count) {
    std::vector<uint8_t> bytes(count * 4);
    for (uint32_t i = 0; i < count; ++i) for (uint32_t k = 0; k < 4; ++k) bytes[i * 4 + k] = static_cast<uint8_t>(value >> (8 * k));
    return e.write(a, ConstByteView(bytes.data(), bytes.size()));
  };
  CHECK(fill_words(counts, 0, 4) && fill_words(least, 0x7FFFFFFFu, 4) && fill_words(flags, 0, 4) && fill_words(most, 0x80000000u, 8));
  const Format r32u = Format::R32Uint;
  CHECK(run([&](CommandWriter& w) {
          w.bind_pipeline(atomic);
          w.bind_uav_image(0, 2), w.bind_uav_image(1, 3), w.bind_uav_image(2, 4, 0, 0, 1, r32u), w.bind_uav_image(3, 5, 0, 0, 2);
          w.dispatch(1);
        }) == status::kSuccess);
  auto words = [&](Handle a, uint32_t count) {
    std::vector<uint32_t> out(count, 0);
    LockResult l;
    if (e.kernel.lock(kPid, a, kLockReadOnly, 0, &l) != status::kSuccess) return out;
    ByteReader in(l.data);
    for (uint32_t& v : out) v = in.le32();
    e.kernel.unlock(kPid, a);
    return out;
  };
  const std::vector<uint32_t> c = words(counts, 4), l = words(least, 4), f = words(flags, 4), m = words(most, 8);
  CHECK(c[0] == 16 && c[1] == 16 && c[2] == 16 && c[3] == 16);
  CHECK(static_cast<int32_t>(l[0]) == -60 && static_cast<int32_t>(l[3]) == -63);
  CHECK(f[0] == 0xFFFFFFFFu && f[1] == 1 && f[2] == 163 && f[3] == 162);   // (the CPU runs the threads in order)
  for (uint32_t i = 0; i < 8; ++i) CHECK(m[i] == 56 + i);
  // Refused: nothing bound where the shader declares an image; an integer image where it declares floats (and the other way); a texture that is not a storage image; layers or a
  // level the image has not; a view of another pixel size; a 2D slot given several layers.
  CHECK(run([&](CommandWriter& w) { w.bind_pipeline(fill); w.dispatch(1); }) == status::kInvalidParameter);
  CHECK(run([&](CommandWriter& w) { w.bind_pipeline(fill); w.bind_uav_image(0, 2); w.dispatch(1); }) == status::kInvalidParameter);
  CHECK(run([&](CommandWriter& w) { w.bind_pipeline(atomic); w.bind_uav_image(0, 0), w.bind_uav_image(1, 3), w.bind_uav_image(2, 4, 0, 0, 1, r32u), w.bind_uav_image(3, 5, 0, 0, 2); w.dispatch(1); }) == status::kInvalidParameter);
  CHECK(run([&](CommandWriter& w) { w.bind_uav_image(0, 1); }) == status::kInvalidParameter);
  CHECK(run([&](CommandWriter& w) { w.bind_uav_image(0, 0, 0, 2, 2); }) == status::kInvalidParameter);
  CHECK(run([&](CommandWriter& w) { w.bind_uav_image(0, 0, 1, 0, 1); }) == status::kInvalidParameter);
  CHECK(run([&](CommandWriter& w) { w.bind_uav_image(0, 0, 0, 0, 1, Format::A32B32G32R32Uint); }) == status::kInvalidParameter);
  CHECK(run([&](CommandWriter& w) { w.bind_uav_image(graphics::kMaxStorageImages, 0); }) == status::kInvalidParameter);
  CHECK(run([&](CommandWriter& w) { w.bind_pipeline(atomic); w.bind_uav_image(0, 0, 0, 0, 3), w.bind_uav_image(1, 3), w.bind_uav_image(2, 4, 0, 0, 1, r32u), w.bind_uav_image(3, 5, 0, 0, 2); w.dispatch(1); }) == status::kInvalidParameter);
  // A device without storage images: no such allocations, no shaders that declare them.
  FakeScreen screen(8, 6);
  graphics::SoftwareDevice software(e.allocator, &screen);
  SpyDevice limited(software);
  limited.feature2_mask = 0;
  Kernel kernel(e.allocator);
  AdapterConfig cfg;
  cfg.display = &screen;
  kernel.add_adapter(limited, cfg);
  Handle adapter = 0, dev = 0, refused = 0;
  CHECK(kernel.open_process(kPid) == status::kSuccess && kernel.open_adapter_from_index(kPid, 0, &adapter) == status::kSuccess && kernel.create_device(kPid, adapter, &dev) == status::kSuccess);
  AllocationDesc d;
  d.flags = kAllocTexture | kAllocStorageImage, d.format = Format::A8B8G8R8, d.width = d.height = 4;
  CHECK(kernel.create_allocation(kPid, dev, d, &refused) == status::kNotSupported);
  const std::vector<uint8_t> code = read_blob("cs_uav_array.dxbc");
  ShaderDesc sd;
  sd.kind = ShaderKind::Dxbc, sd.stage = graphics::ShaderStage::Compute, sd.code = ConstByteView(code.data(), code.size());
  CHECK(kernel.create_shader(kPid, dev, sd, &refused) == status::kNotSupported);
  d.flags = kAllocTexture | kAllocRenderTarget, d.kind = graphics::TextureKind::Texture2DArray, d.layers = 2;
  CHECK(kernel.create_allocation(kPid, dev, d, &refused) == status::kNotSupported);   // (no layered targets either)
}

// A pass of the layers of an array (kPassLayers): a geometry shader sends triangle n to layer n (SV_RenderTargetArrayIndex); a pass of layers 1 and 2 of it, and what is refused.
void test_layered_pass_kernel() {
  Env e;
  Handle vs = 0, gs = 0, ps = 0, pipe = 0;
  auto blob_shader = [&](const char* name, graphics::ShaderStage stage, Handle* out) {
    const std::vector<uint8_t> code = read_blob(name);
    ShaderDesc sd;
    sd.kind = ShaderKind::Dxbc, sd.stage = stage, sd.code = ConstByteView(code.data(), code.size());
    return !code.empty() && e.kernel.create_shader(kPid, e.dev, sd, out) == status::kSuccess;
  };
  CHECK(blob_shader("vs.dxbc", graphics::ShaderStage::Vertex, &vs) && blob_shader("gs_layer.dxbc", graphics::ShaderStage::Geometry, &gs) &&
        blob_shader("ps_color.dxbc", graphics::ShaderStage::Fragment, &ps));
  PipelineDesc pd;
  pd.vertex_shader = vs, pd.fragment_shader = ps, pd.geometry_shader = gs, pd.vertex_stride = 28, pd.attribute_count = 2, pd.varying_count = 3;
  pd.attributes[0] = {0, graphics::VertexFormat::Float3, 0};
  pd.attributes[1] = {1, graphics::VertexFormat::Float4, 12};
  pd.cull_mode = graphics::CullMode::None;
  CHECK(e.kernel.create_pipeline(kPid, e.dev, pd, &pipe) == status::kSuccess);
  AllocationDesc d;
  d.flags = kAllocTexture | kAllocRenderTarget, d.format = Format::A8R8G8B8, d.width = d.height = 16, d.kind = graphics::TextureKind::Texture2DArray, d.layers = 3;
  Handle slices = 0, depths = 0;
  CHECK(e.kernel.create_allocation(kPid, e.dev, d, &slices) == status::kSuccess);
  d.flags = kAllocDepthStencil, d.format = Format::D32Float;
  CHECK(e.kernel.create_allocation(kPid, e.dev, d, &depths) == status::kSuccess);
  // three triangles over the whole target, red, green, blue
  float v[9 * 7];
  for (uint32_t t = 0; t < 3; ++t) {
    const float tri[3][2] = {{-1, -1}, {3, -1}, {-1, 3}};
    for (uint32_t k = 0; k < 3; ++k) {
      float* p = &v[(t * 3 + k) * 7];
      p[0] = tri[k][0], p[1] = tri[k][1], p[2] = 0.5f, p[3] = t == 0, p[4] = t == 1, p[5] = t == 2, p[6] = 1;
    }
  }
  const Handle vb = e.buffer(kAllocVertexBuffer, sizeof v);
  CHECK(e.write(vb, floats(v, 9 * 7)));
  const float identity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
  auto draw = [&](std::initializer_list<uint32_t> layers, uint32_t depth, uint32_t count) {
    CommandWriter w(e.allocator);
    w.begin_pass_layers(0, {}, depth, kPassClearColor | kPassClearDepth, {0, 0, 0, 1}, 1, 0, layers);
    w.bind_pipeline(pipe);
    w.bind_vertex_buffer(2);
    w.push_constants(floats(identity, 16));
    w.draw(count);
    w.end_pass();
    return e.render(w, {slices, depths, vb});
  };
  auto layer_pixel = [&](uint32_t layer) {
    LockResult l;
    uint32_t out = 0;
    if (e.kernel.lock(kPid, slices, kLockReadOnly, 0, &l) == status::kSuccess) {
      ByteReader in(l.data.subview(uint64_t{layer} * 16 * 16 * 4 + (8 * 16 + 8) * 4, 4));
      out = in.le32();
      e.kernel.unlock(kPid, slices);
    }
    return out;
  };
  CHECK(draw({0, 3, 0, 3}, 1, 9) == status::kSuccess);
  CHECK(layer_pixel(0) == 0xFFFF0000u && layer_pixel(1) == 0xFF00FF00u && layer_pixel(2) == 0xFF0000FFu);
  // layers 1 and 2: the first triangle goes to layer 1, the second to 2, the third (past the pass's two) to its first, 1 again - and is drawn over the first there
  CHECK(draw({1, 2, 1, 2}, 1, 9) == status::kSuccess);
  CHECK(layer_pixel(0) == 0xFFFF0000u && layer_pixel(1) == 0xFF0000FFu && layer_pixel(2) == 0xFF00FF00u);
  // Refused: layers the target has not, none, a count of zero, a block that does not add up.
  CHECK(draw({2, 2, 0, 1}, kNoAllocation, 3) == status::kInvalidParameter);
  CHECK(draw({0, 0, 0, 1}, kNoAllocation, 3) == status::kInvalidParameter);
  CHECK(draw({3, 1, 0, 1}, kNoAllocation, 3) == status::kInvalidParameter);
  CHECK(draw({0, 1, 0}, kNoAllocation, 3) == status::kInvalidParameter);
}


// Depth buffers read as textures (kFeature2DepthTextures) and texture and sampler pairs: a depth and stencil pass into a D24S8 buffer that is a texture too, then its depth compared
// (SampleCmpLevelZero with two comparison samplers and sampled with a third: three pairs of t0), its stencil plane loaded, filtered comparisons, an array of depth, and one texture with two
// samplers - on the CPU renderer, through the kernel's command stream.
bool close_to(float a, float b);
void test_depth_textures_kernel() {
  // The pairs a shader's lookups make: t0 with s0, s1, s2 through units 0, 1, 2 (the first sampler keeps t0's own unit); ps_pair's t0 with s0 and s1, and t1 with s1.
  {
    HostAllocator allocator;
    UniquePointer<D3d9Program> program;
    const std::vector<uint8_t> shadow = read_blob("ps_shadow.dxbc"), pair = read_blob("ps_pair.dxbc");
    CHECK(D3d9Program::compile_dxbc(allocator, ConstByteView(shadow.data(), shadow.size()), &program) == status::kSuccess);
    if (program) {
      CHECK(program->texture_unit_mask() == 7 && program->compare_units() == 3);
      CHECK(program->texture_register(0) == 0 && program->sampler_register(0) == 0 && program->texture_register(1) == 0 && program->sampler_register(1) == 1 &&
            program->texture_register(2) == 0 && program->sampler_register(2) == 2);
      String text(allocator);
      CHECK(program->emit_tgsi(&text) == status::kSuccess);   // (the comparisons from the constants after the program's own: ShaderModuleDesc::sampler_constants)
      CHECK(text.view().find("DCL CONST[960..975]") != StringView::npos && text.view().find("CONST[961].zzzz") != StringView::npos);   // (graphics::kSamplerCompareRegister: after the 960 rows of push constants)
    }
    UniquePointer<D3d9Program> pairs;
    CHECK(D3d9Program::compile_dxbc(allocator, ConstByteView(pair.data(), pair.size()), &pairs) == status::kSuccess);
    if (pairs) {
      CHECK(pairs->texture_unit_mask() == 7 && pairs->compare_units() == 0);
      CHECK(pairs->texture_register(0) == 0 && pairs->sampler_register(0) == 0 && pairs->texture_register(2) == 0 && pairs->sampler_register(2) == 1 &&
            pairs->texture_register(1) == 1 && pairs->sampler_register(1) == 1);
    }
    // sample_c_lz with an offset in texels (the compilers in reach do not make one: assembled here) - r0 = t0.SampleCmpLevelZero(s0, v2.xy, v0.x, int2(2, -1)): the point moved by the
    // offset over the base level's size reaches the texture set, with the reference and level zero.
    struct CompareSpy final : graphics::ITextureSet {
      mutable float u = -1, v = -1, reference = -1;
      mutable bool level_zero = false;
      graphics::Vec4 sample(uint32_t, float, float) const override { return {0, 0, 0, 1}; }
      bool size(uint32_t, uint32_t level, uint32_t* width, uint32_t* height, uint32_t* levels) const override {
        *width = 8, *height = 4, *levels = 1;
        return level == 0;
      }
      float sample_compare(uint32_t, float u_, float v_, float, float r, bool zero) const override {
        u = u_, v = v_, reference = r, level_zero = zero;
        return 0.375f;
      }
    } spy;
    DxbcAsm a = dx_pixel_shader();
    a.version_major = 4;
    a.constant_buffer = 0;
    a.ins(88, {dx_resource(0)}, 3u << 11);   // dcl_resource_texture2d t0
    Tokens sample = {71u | (1u << 31) | (12u << 24), 0x1u | (2u << 9) | (0xFu << 13)};   // (sample_c_lz, the extended token: sample controls, offsets 2, -1)
    for (const Tokens& o : {dx_dst(kOpTemp, 0, 0xF), dx_src(kOpInput, 2), dx_resource(0, 0x00), dx_sampler(0), dx_scalar(kOpInput, 0, 0)}) sample.insert(sample.end(), o.begin(), o.end());
    a.code.insert(a.code.end(), sample.begin(), sample.end());
    a.ins(54, {dx_dst(kOpOut, 0, 0xF), dx_src(kOpTemp, 0)});
    a.ins(62, {});
    NtStatus st = 0;
    UniquePointer<D3d9Program> offset = compile_dxbc(allocator, a.build(), &st);
    CHECK(st == status::kSuccess && offset);
    if (offset) {
      graphics::Vec4 varyings[16] = {};
      varyings[0] = {0.6f, 0, 0, 0};
      varyings[2] = {0.5f, 0.5f, 0, 0};
      graphics::Vec4 room[graphics::kMaxColorTargets] = {};
      uint8_t constants[128] = {};
      CHECK(offset->run_fragment_ex(varyings, graphics::FragmentContext(), constants, &spy, room));
      CHECK(close_to(spy.u, 0.75f) && close_to(spy.v, 0.25f) && close_to(spy.reference, 0.6f) && spy.level_zero && close_to(room[0].x, 0.375f) && close_to(room[0].w, 0.375f));
      String text(allocator);
      CHECK(offset->emit_tgsi(&text) == status::kSuccess && text.view().find("TG4") != StringView::npos);
    }
  }
  Env e;
  Handle vs = 0, colour_ps = 0, shadow_ps = 0, stencil_ps = 0, pcf_ps = 0, pair_ps = 0, array_ps = 0;
  auto blob_shader = [&](const char* name, graphics::ShaderStage stage, Handle* out) {
    const std::vector<uint8_t> code = read_blob(name);
    ShaderDesc sd;
    sd.kind = ShaderKind::Dxbc, sd.stage = stage, sd.code = ConstByteView(code.data(), code.size());
    return !code.empty() && e.kernel.create_shader(kPid, e.dev, sd, out) == status::kSuccess;
  };
  CHECK(blob_shader("vs.dxbc", graphics::ShaderStage::Vertex, &vs) && blob_shader("ps_color.dxbc", graphics::ShaderStage::Fragment, &colour_ps) &&
        blob_shader("ps_shadow.dxbc", graphics::ShaderStage::Fragment, &shadow_ps) && blob_shader("ps_stencil.dxbc", graphics::ShaderStage::Fragment, &stencil_ps) &&
        blob_shader("ps_shadow_pcf.dxbc", graphics::ShaderStage::Fragment, &pcf_ps) && blob_shader("ps_pair.dxbc", graphics::ShaderStage::Fragment, &pair_ps) &&
        blob_shader("ps_shadow_array.dxbc", graphics::ShaderStage::Fragment, &array_ps));
  auto pipeline = [&](Handle ps, uint32_t textures, bool depth, Handle* out) {
    PipelineDesc pd;
    pd.vertex_shader = vs, pd.fragment_shader = ps, pd.vertex_stride = 28, pd.attribute_count = 2, pd.varying_count = 3, pd.texture_count = textures;
    pd.attributes[0] = {0, graphics::VertexFormat::Float3, 0};
    pd.attributes[1] = {1, graphics::VertexFormat::Float4, 12};
    pd.cull_mode = graphics::CullMode::None;
    if (depth) {
      pd.depth_test = pd.depth_write = true;
      pd.depth_compare = graphics::CompareOp::Less;
      pd.stencil.enable = true;
      pd.stencil.pass_op = graphics::StencilOp::Replace;
      pd.stencil.reference = 5;
    }
    return e.kernel.create_pipeline(kPid, e.dev, pd, out) == status::kSuccess;
  };
  Handle depth_pipe = 0, shadow_pipe = 0, stencil_pipe = 0, pcf_pipe = 0, pair_pipe = 0, array_pipe = 0;
  CHECK(pipeline(colour_ps, 0, true, &depth_pipe) && pipeline(shadow_ps, 3, false, &shadow_pipe) && pipeline(stencil_ps, 2, false, &stencil_pipe) &&
        pipeline(pcf_ps, 1, false, &pcf_pipe) && pipeline(pair_ps, 3, false, &pair_pipe) && pipeline(array_ps, 1, false, &array_pipe));
  // (the buffer: a device without the feature has none)
  AllocationDesc dd;
  dd.flags = kAllocDepthStencil | kAllocTexture, dd.format = Format::D24S8, dd.width = dd.height = 64;
  Handle depth = 0, target = e.alloc(kAllocRenderTarget, Format::A8R8G8B8, 64, 64);
  CHECK(e.kernel.create_allocation(kPid, e.dev, dd, &depth) == status::kSuccess);
  {
    FakeScreen screen(8, 6);
    graphics::SoftwareDevice inner(e.allocator, &screen);
    SpyDevice limited(inner);
    limited.feature2_mask = ~static_cast<uint32_t>(graphics::kFeature2DepthTextures);
    Kernel k(e.allocator);
    AdapterConfig c;
    c.display = &screen;
    CHECK(k.add_adapter(limited, c) == 0);
    Handle a = 0, d = 0, h = 0;
    CHECK(k.open_process(kPid) == status::kSuccess && k.open_adapter_from_index(kPid, 0, &a) == status::kSuccess && k.create_device(kPid, a, &d) == status::kSuccess);
    CHECK(k.create_allocation(kPid, d, dd, &h) == status::kNotSupported && h == 0);
    AllocationDesc multi = dd;
    multi.samples = 4;
    CHECK(k.create_allocation(kPid, d, multi, &h) == status::kNotSupported && h == 0);   // (nor of several samples)
    CHECK(e.kernel.create_allocation(kPid, e.dev, multi, &h) == status::kSuccess && h != 0);   // (several samples: read through Texture2DMS units, kFeature3MultisampleDepthLoads)
    CHECK(e.kernel.destroy_allocation(kPid, h) == status::kSuccess);
  }
  // The depth pass: the top half of the buffer at depth 0.25 and stencil 5, the rest cleared to 1 and 0.
  auto vert = [](float* p, float x, float y, float z, float u, float v, float reference) { p[0] = x, p[1] = y, p[2] = z, p[3] = u / 2, p[4] = v / 2, p[5] = reference, p[6] = 1; };
  float top[6 * 7], quad[6 * 7];
  const float corners[6][2] = {{-1, 1}, {1, 1}, {-1, 0}, {-1, 0}, {1, 1}, {1, 0}}, full[6][2] = {{-1, 1}, {1, 1}, {-1, -1}, {-1, -1}, {1, 1}, {1, -1}};
  for (uint32_t i = 0; i < 6; ++i) {
    vert(&top[i * 7], corners[i][0], corners[i][1], 0.25f, 0, 0, 0);
    vert(&quad[i * 7], full[i][0], full[i][1], 0.5f, (full[i][0] + 1) / 2, (1 - full[i][1]) / 2, 0.5f);   // (uv 0, 0 at the top left; the reference 0.5)
  }
  const Handle top_vb = e.buffer(kAllocVertexBuffer, sizeof top), quad_vb = e.buffer(kAllocVertexBuffer, sizeof quad);
  CHECK(e.write(top_vb, floats(top, 6 * 7)) && e.write(quad_vb, floats(quad, 6 * 7)));
  const float identity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
  {
    CommandWriter w(e.allocator);
    w.begin_pass(0, 1, kPassClearColor | kPassClearDepth | kPassClearStencil, {0, 0, 0, 1}, 1.0f, 0);
    w.bind_pipeline(depth_pipe);
    w.bind_vertex_buffer(2);
    w.push_constants(floats(identity, 16));
    w.draw(6);
    w.end_pass();
    CHECK(e.render(w, {target, depth, top_vb}) == status::kSuccess);
  }
  graphics::SamplerDesc less_equal, greater, plain, linear;
  less_equal.compare = graphics::CompareOp::LessEqual;
  greater.compare = graphics::CompareOp::Greater;
  less_equal.address_u = less_equal.address_v = greater.address_u = greater.address_v = plain.address_u = plain.address_v = graphics::AddressMode::ClampToEdge;
  linear = less_equal;
  linear.filter = graphics::Filter::Linear;
  graphics::SamplerDesc stencil_view = plain;
  stencil_view.stencil = true;
  // Reads it through `pipe` with the units bound as given (allocation 1 the depth buffer, 3 the second texture) into the target, and gives the target's pixel at (x, y) for each point.
  auto read = [&](Handle pipe, std::initializer_list<std::pair<uint32_t, graphics::SamplerDesc>> units, Handle second, std::initializer_list<std::pair<uint32_t, uint32_t>> points,
                  uint32_t* out, uint32_t on_second = ~0u) {
    CommandWriter w(e.allocator);
    w.begin_pass(0, kNoAllocation, kPassClearColor, {0, 0, 0, 0}, 1.0f, 0);
    w.bind_pipeline(pipe);
    w.bind_vertex_buffer(2);
    w.push_constants(floats(identity, 16));
    for (const auto& u : units) w.bind_texture(u.first, u.first == on_second ? 3 : 1, u.second);
    w.draw(6);
    w.end_pass();
    const NtStatus st = second ? e.render(w, {target, depth, quad_vb, second}) : e.render(w, {target, depth, quad_vb});
    if (st != status::kSuccess) printf("  depth textures: render %x\n", st);
    uint32_t n = 0;
    for (const auto& p : points) out[n++] = e.pixel(target, p.first, p.second);
    return st;
  };
  auto red = [](uint32_t p) { return (p >> 16) & 0xFF; };
  auto green = [](uint32_t p) { return (p >> 8) & 0xFF; };
  auto blue = [](uint32_t p) { return p & 0xFF; };
  uint32_t px[4] = {};
  // The comparisons: 0.5 <= 0.25 fails above, 0.5 <= 1 passes below; greater the other way round; the plain sampler gives the depth.
  CHECK(read(shadow_pipe, {{0, less_equal}, {1, greater}, {2, plain}}, 0, {{10, 10}, {50, 50}}, px) == status::kSuccess);
  printf("  depth textures: compared %08x (top) %08x (bottom)\n", px[0], px[1]);
  CHECK(red(px[0]) == 0 && green(px[0]) == 255 && blue(px[0]) >= 62 && blue(px[0]) <= 66);
  CHECK(red(px[1]) == 255 && green(px[1]) == 0 && blue(px[1]) == 255);
  // ... the same samplers the other way round on the units: the pairs are the units'
  CHECK(read(shadow_pipe, {{0, greater}, {1, less_equal}, {2, plain}}, 0, {{10, 10}}, px) == status::kSuccess);
  CHECK(red(px[0]) == 255 && green(px[0]) == 0);
  // The stencil plane (5 above, 0 below) beside the depth.
  CHECK(read(stencil_pipe, {{0, plain}, {1, stencil_view}}, 0, {{10, 10}, {50, 50}}, px) == status::kSuccess);
  printf("  depth textures: stencil %08x (top) %08x (bottom)\n", px[0], px[1]);
  CHECK(green(px[0]) == 5 && green(px[1]) == 0 && red(px[0]) >= 62 && red(px[0]) <= 66 && red(px[1]) == 255);
  // A device whose textures read one plane at a time (no kFeature2DepthAndStencilPlanes: virgl) is not given a draw that reads both planes of one buffer: the draw is refused; one plane is fine.
  {
    FakeScreen screen(8, 6);
    graphics::SoftwareDevice inner(e.allocator, &screen);
    SpyDevice one_plane(inner);
    one_plane.feature2_mask = ~static_cast<uint32_t>(graphics::kFeature2DepthAndStencilPlanes);
    Kernel k(e.allocator);
    AdapterConfig c;
    c.display = &screen;
    CHECK(k.add_adapter(one_plane, c) == 0);
    Handle a = 0, d = 0, context = 0, kvs = 0, kps = 0, kpipe = 0, kdepth = 0, ktarget = 0, kvb = 0;
    CHECK(k.open_process(kPid) == status::kSuccess && k.open_adapter_from_index(kPid, 0, &a) == status::kSuccess && k.create_device(kPid, a, &d) == status::kSuccess &&
          k.create_context(kPid, d, ContextPriority::Normal, &context) == status::kSuccess);
    const std::vector<uint8_t> vs_code = read_blob("vs.dxbc"), ps_code = read_blob("ps_stencil.dxbc");
    ShaderDesc sd;
    sd.kind = ShaderKind::Dxbc, sd.stage = graphics::ShaderStage::Vertex, sd.code = ConstByteView(vs_code.data(), vs_code.size());
    CHECK(k.create_shader(kPid, d, sd, &kvs) == status::kSuccess);
    sd.stage = graphics::ShaderStage::Fragment, sd.code = ConstByteView(ps_code.data(), ps_code.size());
    CHECK(k.create_shader(kPid, d, sd, &kps) == status::kSuccess);
    PipelineDesc pd;
    pd.vertex_shader = kvs, pd.fragment_shader = kps, pd.vertex_stride = 28, pd.attribute_count = 2, pd.varying_count = 3, pd.texture_count = 2;
    pd.attributes[0] = {0, graphics::VertexFormat::Float3, 0};
    pd.attributes[1] = {1, graphics::VertexFormat::Float4, 12};
    pd.cull_mode = graphics::CullMode::None;
    CHECK(k.create_pipeline(kPid, d, pd, &kpipe) == status::kSuccess);
    AllocationDesc td;
    td.flags = kAllocRenderTarget, td.format = Format::A8R8G8B8, td.width = td.height = 64;
    AllocationDesc vd;
    vd.flags = kAllocVertexBuffer, vd.size = sizeof quad;
    CHECK(k.create_allocation(kPid, d, dd, &kdepth) == status::kSuccess && k.create_allocation(kPid, d, td, &ktarget) == status::kSuccess &&
          k.create_allocation(kPid, d, vd, &kvb) == status::kSuccess);
    auto draw = [&](const graphics::SamplerDesc& first) {
      CommandWriter w(e.allocator);
      w.begin_pass(0, kNoAllocation, kPassClearColor, {0, 0, 0, 0}, 1.0f, 0);
      w.bind_pipeline(kpipe);
      w.bind_vertex_buffer(2);
      w.push_constants(floats(identity, 16));
      w.bind_texture(0, 1, first);
      w.bind_texture(1, 1, stencil_view);
      w.draw(6);
      w.end_pass();
      const Handle list[3] = {ktarget, kdepth, kvb};
      return k.render(kPid, context, w.view(), ArrayView<const Handle>(list, 3));
    };
    CHECK(draw(plain) == status::kInvalidParameter);          // (the depth through unit 0, the stencil through unit 1)
    CHECK(draw(stencil_view) == status::kSuccess);            // (the stencil through both)
    // (a pipeline that has unit 0 but whose shader reads only unit 1: the depth bound at unit 0 is no matter - the device is given the stencil there too -, and the draw is made)
    const std::vector<uint8_t> only_code = read_blob("ps_stencil_only.dxbc");
    sd.code = ConstByteView(only_code.data(), only_code.size());
    Handle only_ps = 0;
    CHECK(k.create_shader(kPid, d, sd, &only_ps) == status::kSuccess);
    pd.fragment_shader = only_ps;
    CHECK(k.create_pipeline(kPid, d, pd, &kpipe) == status::kSuccess);
    LockResult lr;
    if (k.lock(kPid, kvb, kLockDiscard, 0, &lr) == status::kSuccess) {
      ConstByteView src = floats(quad, 6 * 7);
      lr.data.first(src.size()).copy_from(src);
      k.unlock(kPid, kvb);
    }
    CHECK(draw(plain) == status::kSuccess);
    LockResult read;
    uint32_t top = 0;
    if (k.lock(kPid, ktarget, kLockReadOnly, 0, &read) == status::kSuccess) {
      ByteReader in(read.data);
      in.skip(uint64_t{10} * read.pitch + 10 * 4);
      top = in.le32();
      k.unlock(kPid, ktarget);
    }
    CHECK((top >> 24) == 0xFF && ((top >> 8) & 0xFF) == 0);   // (drawn; the buffer's stencil is the 0 it starts with)
  }
  // (a colour image has no stencil plane)
  {
    const Handle colour = e.alloc(kAllocTexture, Format::A8R8G8B8, 4, 4);
    CommandWriter w(e.allocator);
    w.begin_pass(0, kNoAllocation, kPassClearColor, {0, 0, 0, 0}, 1.0f, 0);
    w.bind_texture(1, 1, stencil_view);
    w.end_pass();
    CHECK(e.render(w, {target, colour}) == status::kInvalidParameter);
  }
  // Filtered, a quarter of a texel down: the pixels of row 31 take a quarter of row 32 (which passes) and three quarters of row 31 (which fails); far from the edge they do not
  // blend. The offset lookup is the same two texels over; GatherCmp's four are two of each.
  {
    float shifted[6 * 7];
    for (uint32_t i = 0; i < 6; ++i) vert(&shifted[i * 7], full[i][0], full[i][1], 0.5f, (full[i][0] + 1) / 2, (1 - full[i][1]) / 2 + 0.25f / 64, 0.5f);
    CHECK(e.write(quad_vb, floats(shifted, 6 * 7)));
    CHECK(read(pcf_pipe, {{0, linear}}, 0, {{20, 31}, {20, 10}, {20, 50}}, px) == status::kSuccess);
    printf("  depth textures: filtered %08x (the edge) %08x %08x\n", px[0], px[1], px[2]);
    CHECK(red(px[0]) >= 60 && red(px[0]) <= 68 && green(px[0]) >= 60 && green(px[0]) <= 68 && blue(px[0]) >= 125 && blue(px[0]) <= 130);
    CHECK((px[1] & 0xFFFFFF) == 0 && (px[2] & 0xFFFFFF) == 0xFFFFFF);
    CHECK(e.write(quad_vb, floats(quad, 6 * 7)));
  }
  // One texture with two samplers - wrap and clamp - and a second texture with the second: the coordinate goes past 1 on the right half.
  {
    const Handle ramp = e.alloc(kAllocTexture, Format::A8R8G8B8, 4, 1), second = e.alloc(kAllocTexture, Format::A8R8G8B8, 1, 1);
    const uint32_t texels[4] = {0xFF200000u, 0xFF600000u, 0xFFA00000u, 0xFFE00000u}, one = 0xFF00C000u;
    CHECK(e.write(ramp, ConstByteView(reinterpret_cast<const uint8_t*>(texels), sizeof texels)) && e.write(second, ConstByteView(reinterpret_cast<const uint8_t*>(&one), 4)));
    float wide[6 * 7];
    for (uint32_t i = 0; i < 6; ++i) vert(&wide[i * 7], full[i][0], full[i][1], 0.5f, full[i][0] + 1, (1 - full[i][1]) / 2, 0);   // (u 0 .. 2)
    CHECK(e.write(quad_vb, floats(wide, 6 * 7)));
    graphics::SamplerDesc wrap;
    wrap.address_u = wrap.address_v = graphics::AddressMode::Repeat;
    graphics::SamplerDesc clamp = plain;
    CommandWriter w(e.allocator);
    w.begin_pass(0, kNoAllocation, kPassClearColor, {0, 0, 0, 0}, 1.0f, 0);
    w.bind_pipeline(pair_pipe);
    w.bind_vertex_buffer(1);
    w.push_constants(floats(identity, 16));
    w.bind_texture(0, 2, wrap);
    w.bind_texture(2, 2, clamp);   // (t0 with s1)
    w.bind_texture(1, 3, clamp);
    w.draw(6);
    w.end_pass();
    CHECK(e.render(w, {target, quad_vb, ramp, second}) == status::kSuccess);
    const uint32_t left = e.pixel(target, 4, 10), right = e.pixel(target, 36, 10);   // (u 0.14: texel 0 both ways; u 1.14: texel 0 wrapped, texel 3 clamped)
    printf("  texture and sampler pairs: %08x %08x\n", left, right);
    CHECK(red(left) == 0x20 && green(left) == 0x20 && blue(left) == 0xC0 && red(right) == 0x20 && green(right) == 0xE0 && blue(right) == 0xC0);
    CHECK(e.write(quad_vb, floats(quad, 6 * 7)));
  }
  // An array of depth: two slices, the first cleared to 0.75 and the second to 0.25, compared with 0.5 and the second loaded.
  {
    AllocationDesc ad;
    ad.flags = kAllocDepthStencil | kAllocTexture, ad.format = Format::D32Float, ad.width = ad.height = 32, ad.kind = graphics::TextureKind::Texture2DArray, ad.layers = 2;
    Handle layers = 0;
    CHECK(e.kernel.create_allocation(kPid, e.dev, ad, &layers) == status::kSuccess);
    const Handle small = e.alloc(kAllocRenderTarget, Format::A8R8G8B8, 32, 32);
    for (uint32_t slice = 0; slice < 2; ++slice) {
      CommandWriter w(e.allocator);
      w.begin_pass_layers(0, {}, 1, kPassClearColor | kPassClearDepth, {0, 0, 0, 1}, slice == 0 ? 0.75f : 0.25f, 0, {0, 1, slice, 1});
      w.end_pass();
      CHECK(e.render(w, {small, layers}) == status::kSuccess);
    }
    CommandWriter w(e.allocator);
    w.begin_pass(0, kNoAllocation, kPassClearColor, {0, 0, 0, 0}, 1.0f, 0);
    w.bind_pipeline(array_pipe);
    w.bind_vertex_buffer(2);
    w.push_constants(floats(identity, 16));
    w.bind_texture(0, 1, less_equal);
    w.draw(6);
    w.end_pass();
    CHECK(e.render(w, {target, layers, quad_vb}) == status::kSuccess);
    const uint32_t p = e.pixel(target, 20, 20);
    printf("  depth textures: array %08x\n", p);
    CHECK(red(p) == 255 && green(p) == 0 && blue(p) >= 62 && blue(p) <= 66);
  }
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
  // Render targets and depth buffers only (both may be read by a shader, as a multisampled texture: kFeature3MultisampleLoads / MultisampleDepthLoads), one level, 8-bit colour.
  CHECK(create(kAllocRenderTarget | kAllocTexture, Format::A8R8G8B8, 4) == status::kSuccess && create(kAllocDepthStencil | kAllocTexture, Format::D32Float, 4) == status::kSuccess);
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
  // Not presentable; the wide ones cannot be multisampled (levels they may have on a device with kFeature2MipmappedLayers); an sRGB image may.
  for (Format f : all) CHECK(create(kAllocRenderTarget | kAllocPresent, f) == status::kInvalidParameter);
  CHECK(create(kAllocTexture, Format::A16B16G16R16, 8, 8, 2) == status::kSuccess && create(kAllocTexture, Format::A32B32G32R32Uint, 8, 8, 2) == status::kSuccess);   // (levels: kFeature2MipmappedLayers)
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
    // A binding's end (Direct3D 12's SizeInBytes): stream 0 in a large buffer stops after two records (64 bytes, and 8 of room that a record does not fit in) - the third is
    // not written and the bytes after the end are as they were; the counts are those of a full buffer. An append binding keeps the end.
    Handle wide0 = buffer(6 * 32), wide1 = buffer(6 * 16), counts = e.buffer(kAllocQueryResult, 64);
    float marker[48];
    for (float& m : marker) m = 7.0f;
    CHECK(e.write(wide0, floats(marker, 48)));
    auto bounded = [&](uint32_t offset, uint32_t end, uint32_t kind) {
      CommandWriter w(e.allocator);
      w.begin_pass(0, kNoAllocation, kPassClearColor, {0, 0, 0, 1}, 1, 0);
      w.set_stream_output(0, 2, offset, end), w.set_stream_output(1, 3, 0);
      w.bind_pipeline(pipe);
      w.bind_vertex_buffer(1);
      w.push_constants(floats(identity, 16));
      w.begin_query(4, 0, kind);
      w.draw(3);
      w.end_query();
      w.end_pass();
      const NtStatus s = e.render(w, {target, vb, wide0, wide1, counts});
      return s == status::kSuccess ? query_count(e, counts, 0) : ~uint64_t{0};
    };
    CHECK(bounded(0, 2 * 32 + 8, kWritten) == 2);
    CHECK(bounded(0, 2 * 32 + 8, kNeeded) == 3);
    float f[48];
    CHECK(read_floats(wide0, 48, f));
    CHECK(f[0] == -0.5f && f[8] == 0.5f && f[16] == 7.0f && f[23] == 7.0f && f[47] == 7.0f);   // (records 0 and 1; the third not written)
    // (the end after the third record: all three; from an offset: the room is end - offset)
    CHECK(bounded(0, 3 * 32, kWritten) == 3 && bounded(32, 3 * 32, kWritten) == 2 && bounded(32, 32, kWritten) == 0 && bounded(32, 32, kNeeded) == 3);
    CHECK(bounded(0, kStreamOutputNoEnd, kWritten) == 3);
    // (an end that is not a multiple of 4, past the buffer, or before the offset: refused)
    CHECK(bounded(0, 66, kWritten) == ~uint64_t{0} && bounded(0, 6 * 32 + 4, kWritten) == ~uint64_t{0} && bounded(64, 32, kWritten) == ~uint64_t{0});
    // (appending in a second pass goes on up to the same end: one more record fits after the first two of three)
    {
      CommandWriter w(e.allocator);
      for (uint32_t round = 0; round < 2; ++round) {
        w.begin_pass(0, kNoAllocation, kPassClearColor, {0, 0, 0, 1}, 1, 0);
        w.set_stream_output(0, 2, round ? kStreamOutputAppendOffset : 0, 4 * 32), w.set_stream_output(1, 3, round ? kStreamOutputAppendOffset : 0);
        w.bind_pipeline(pipe);
        w.bind_vertex_buffer(1);
        w.push_constants(floats(identity, 16));
        if (round) w.begin_query(4, 0, kWritten);
        w.draw(3);
        if (round) w.end_query();
        w.end_pass();
      }
      CHECK(e.render(w, {target, vb, wide0, wide1, counts}) == status::kSuccess);
      CHECK(query_count(e, counts, 0) == 1);
    }
    e.kernel.destroy_allocation(kPid, wide0);
    e.kernel.destroy_allocation(kPid, wide1);
    e.kernel.destroy_allocation(kPid, counts);
  }
  {
    // All at once: the kernel's SoStatistics of both streams (written, needed), the separate counts, and the pipeline statistics of the geometry shader's draw, over two passes.
    Handle many = e.buffer(kAllocQueryResult, 512);
    CommandWriter w(e.allocator);
    w.begin_query(4, 0, kQueryKindSoStatistics);
    w.begin_query(4, 16, kQueryKindSoStatistics | 1u << 8);
    w.begin_query(4, 32, kWritten);
    w.begin_query(4, 40, kNeeded);
    w.begin_query(4, 48, static_cast<uint32_t>(graphics::QueryKind::PipelineStatistics));
    for (uint32_t round = 0; round < 2; ++round) {
      w.begin_pass(0, kNoAllocation, kPassClearColor, {0, 0, 0, 1}, 1, 0);
      w.set_stream_output(0, 2, 0), w.set_stream_output(1, 3, 0);
      w.bind_pipeline(pipe);
      w.bind_vertex_buffer(1);
      w.push_constants(floats(identity, 16));
      w.draw(3);
      w.end_pass();
    }
    for (uint32_t k = 0; k < 5; ++k) w.end_query();
    CHECK(e.render(w, {target, vb, small0, full1, many}) == status::kSuccess);
    CHECK(query_count(e, many, 0) == 4 && query_count(e, many, 1) == 6 && query_count(e, many, 2) == 6 && query_count(e, many, 3) == 6);
    CHECK(query_count(e, many, 4) == 4 && query_count(e, many, 5) == 6);
    // (two draws of one triangle: one geometry shader run each, which makes three points on each stream; nothing is rasterized)
    const uint64_t expect[11] = {6, 2, 6, 2, 12, 0, 0, 0, 0, 0, 0};
    for (uint32_t k = 0; k < 11; ++k) CHECK(query_count(e, many, 6 + k) == expect[k]);
    e.kernel.destroy_allocation(kPid, many);
  }
  {
    // (a stream that does not exist, and a statistic of a stream other than the first for a kind that has one)
    CommandWriter w(e.allocator);
    w.begin_pass(0, kNoAllocation, kPassClearColor, {0, 0, 0, 1}, 1, 0);
    w.begin_query(2, 0, kWritten | 4u << 8);
    w.end_query();
    w.end_pass();
    CHECK(e.render(w, {target, vb, results}) == status::kInvalidParameter);
  }

  {
    // A pipeline whose fragment shader stands in for none (kPipelineFlagStandInFragment, as d3d12.dll gives a pipeline without a pixel shader): its runs are no PSInvocations.
    auto ps_runs = [&](bool stand_in) {
      PipelineDesc pd;
      pd.vertex_shader = vs, pd.fragment_shader = ps, pd.vertex_stride = 28, pd.attribute_count = 2, pd.varying_count = 3;
      pd.attributes[0] = {0, graphics::VertexFormat::Float3, 0};
      pd.attributes[1] = {1, graphics::VertexFormat::Float4, 12};
      pd.cull_mode = graphics::CullMode::None;
      pd.stand_in_fragment = stand_in;
      Handle drawn = 0;
      if (e.kernel.create_pipeline(kPid, e.dev, pd, &drawn) != status::kSuccess) return ~uint64_t{0};
      Handle stats = e.buffer(kAllocQueryResult, 88);
      CommandWriter w(e.allocator);
      w.begin_pass(0, kNoAllocation, kPassClearColor, {0, 0, 0, 1}, 1, 0);
      w.bind_pipeline(drawn);
      w.bind_vertex_buffer(1);
      w.push_constants(floats(identity, 16));
      w.begin_query(2, 0, static_cast<uint32_t>(graphics::QueryKind::PipelineStatistics));
      w.draw(3);
      w.end_query();
      w.end_pass();
      const NtStatus s = e.render(w, {target, vb, stats});
      const uint64_t runs = s == status::kSuccess && query_count(e, stats, 2) == 3 ? query_count(e, stats, 7) : ~uint64_t{0};   // (VSInvocations 3, then PSInvocations)
      e.kernel.destroy_allocation(kPid, stats);
      return runs;
    };
    const uint64_t counted = ps_runs(false), stood_in = ps_runs(true);
    CHECK(counted != ~uint64_t{0} && counted > 0 && stood_in == 0);
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
    // (the pipeline statistics of the patch: one hull shader run, a domain shader run for each point the tessellator made - at least the triangles' corners are shared -, three vertices)
    Handle tess_stats = e.buffer(kAllocQueryResult, 88);
    CommandWriter t(e.allocator);
    t.begin_pass(0, kNoAllocation, kPassClearColor, {0, 0, 0, 1}, 1, 0);
    t.set_stream_output(0, 2);
    t.bind_pipeline(ds_pipe);
    t.bind_vertex_buffer(1);
    t.push_constants(floats(identity, 16));
    t.set_topology(patch_list(3));
    t.begin_query(3, 0, static_cast<uint32_t>(graphics::QueryKind::PipelineStatistics));
    t.draw(3);
    t.end_query();
    t.end_pass();
    CHECK(e.render(t, {target, vb, ds_out, tess_stats}) == status::kSuccess);
    CHECK(query_count(e, tess_stats, 0) == 3 && query_count(e, tess_stats, 1) == 1 && query_count(e, tess_stats, 2) == 3 && query_count(e, tess_stats, 8) == 1);
    CHECK(query_count(e, tess_stats, 9) >= 3 && query_count(e, tess_stats, 9) <= triangles * 3 && query_count(e, tess_stats, 3) == 0 && query_count(e, tess_stats, 5) == 0);
    e.kernel.destroy_allocation(kPid, tess_stats);
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
        if (version >= 2) payload.push_back(0);
        payload.push_back(r.offset);
      }
    } else if (p.type == 1) {
      payload.push_back(p.reg), payload.push_back(p.space), payload.push_back(p.num);
    } else {
      payload.push_back(p.reg), payload.push_back(p.space);
      if (version >= 2) payload.push_back(0);
    }
  }
  (void)range_words;
  w[4] = (cursor + static_cast<uint32_t>(payload.size())) * 4;
  w.insert(w.end(), array.begin(), array.end());
  w.insert(w.end(), payload.begin(), payload.end());
  for (const std::vector<uint32_t>& sampler : samplers) {
    w.insert(w.end(), sampler.begin(), sampler.begin() + 13);
    if (version == 3) w.push_back(sampler.size() > 13 ? sampler[13] : 0);   // (1.2: D3D12_SAMPLER_FLAGS after each static sampler)
  }
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
  for (uint32_t version : {1u, 2u, 3u}) {
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
  // Version 1.2's static samplers: the integer border colours with their flag (the same colours as the float ones), not without it nor the flag without them; no
  // sampler of coordinates in texels (NonNormalizedCoordinateSamplersSupported is FALSE); the colours of 1.2 are not 1.0's or 1.1's.
  {
    auto sampler12 = [](uint32_t border, uint32_t flags) {
      std::vector<uint32_t> s = static_sampler(0x15, 4, 0);
      s[7] = border;
      s.push_back(flags);
      return s;
    };
    auto parse12 = [&](uint32_t version, const std::vector<uint32_t>& sampler, RootSignatureDesc* out) {
      const std::vector<uint8_t> blob = serialize_signature(version, {{1, 0, {}, 0, 0, 4}}, {sampler});
      return parse_root_signature(ConstByteView(blob.data(), blob.size()), out);
    };
    RootSignatureDesc s12;
    CHECK(parse12(3, sampler12(4, 1), &s12) == status::kSuccess && s12.sampler_count == 1 && s12.samplers[0].sampler.border == graphics::BorderColor::OpaqueWhite);
    CHECK(parse12(3, sampler12(3, 1), &s12) == status::kSuccess && s12.samplers[0].sampler.border == graphics::BorderColor::OpaqueBlack);
    CHECK(parse12(3, sampler12(2, 0), &s12) == status::kSuccess && s12.samplers[0].sampler.border == graphics::BorderColor::OpaqueWhite);
    CHECK(parse12(3, sampler12(4, 0), &s12) == status::kInvalidParameter);   // (an integer colour without its flag)
    CHECK(parse12(3, sampler12(1, 1), &s12) == status::kInvalidParameter);   // (the flag with a float colour)
    CHECK(parse12(3, sampler12(0, 2), &s12) == status::kInvalidParameter);   // (coordinates in texels)
    CHECK(parse12(3, sampler12(0, 8), &s12) == status::kInvalidParameter);   // (a flag that does not exist)
    CHECK(parse12(2, sampler12(3, 0), &s12) == status::kInvalidParameter);   // (1.2's colour in a 1.1 root signature)
    CHECK(parse12(3, sampler12(5, 1), &s12) == status::kInvalidParameter);   // (no sixth colour)
  }
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

// The buffer copies (CopyBuffer, CopyBufferImage) of the stream and the escapes that make Direct3D 12 objects, as d3d12.dll uses them.
void test_d3d12_copies_and_escapes() {
  Env e;
  auto bytes_of = [](const std::vector<uint8_t>& v) { return ConstByteView(v.data(), v.size()); };
  std::vector<uint8_t> pattern(64);
  for (uint32_t i = 0; i < 64; ++i) pattern[i] = static_cast<uint8_t>(i * 3 + 1);
  Handle a = e.buffer(kAllocVertexBuffer, 64), b = e.buffer(kAllocVertexBuffer, 64);
  CHECK(e.write(a, bytes_of(pattern)));
  {
    CommandWriter w(e.allocator);
    w.copy_buffer(1, 8, 0, 16, 32);
    CHECK(e.render(w, {a, b}) == status::kSuccess);
  }
  auto read = [&](Handle h, uint32_t offset) {
    LockResult r;
    uint8_t v = 0xEE;
    if (e.kernel.lock(kPid, h, kLockReadOnly, 0, &r) == status::kSuccess) {
      v = r.data[offset];
      e.kernel.unlock(kPid, h);
    }
    return v;
  };
  CHECK(read(b, 8) == pattern[16] && read(b, 39) == pattern[47] && read(b, 7) == 0 && read(b, 40) == 0);
  // refused: out of range, overlapping inside one buffer, an image, a pass in progress
  Handle img = e.alloc(kTexture, Format::A8R8G8B8, 4, 4);
  for (int bad = 0; bad < 4; ++bad) {
    CommandWriter w(e.allocator);
    if (bad == 0) w.copy_buffer(1, 40, 0, 0, 32);
    if (bad == 1) w.copy_buffer(0, 0, 0, 16, 32);
    if (bad == 2) w.copy_buffer(2, 0, 0, 0, 16);
    if (bad == 3) { w.begin_pass(2, kNoAllocation, 0, {0, 0, 0, 1}, 1, 0); w.copy_buffer(1, 0, 0, 0, 4); w.end_pass(); }
    CHECK(e.render(w, {a, b, img}) == status::kInvalidParameter);
  }
  // a buffer to a texture and back: rows 20 bytes apart, a 3 x 2 rectangle at (1, 1) of a 4 x 4 image
  CHECK(e.fill(img, 0));
  Handle staging = e.buffer(kAllocVertexBuffer, 128), back = e.buffer(kAllocVertexBuffer, 128);
  std::vector<uint8_t> rows(128, 0);
  for (uint32_t r = 0; r < 2; ++r)
    for (uint32_t x = 0; x < 12; ++x) rows[8 + r * 20 + x] = static_cast<uint8_t>(0x10 + r * 0x20 + x);
  CHECK(e.write(staging, bytes_of(rows)));
  {
    CommandWriter w(e.allocator);
    w.copy_buffer_to_image(0, 0, 0, 1, 1, 3, 2, 1, 8, 20);
    CHECK(e.render(w, {img, staging}) == status::kSuccess);
  }
  CHECK(e.pixel(img, 0, 0) == 0 && e.pixel(img, 1, 1) == 0x13121110u && e.pixel(img, 3, 1) == 0x1B1A1918u && e.pixel(img, 1, 2) == 0x33323130u && e.pixel(img, 1, 3) == 0);
  {
    CommandWriter w(e.allocator);
    w.copy_image_to_buffer(0, 0, 0, 1, 1, 3, 2, 1, 32, 16);
    CHECK(e.render(w, {img, back}) == status::kSuccess);
  }
  CHECK(read(back, 32) == 0x10 && read(back, 43) == 0x1B && read(back, 48) == 0x30 && read(back, 59) == 0x3B && read(back, 44) == 0);
  // refused: a rectangle outside the image, a pitch that is too short, a missing level, a buffer too small
  for (int bad = 0; bad < 4; ++bad) {
    CommandWriter w(e.allocator);
    if (bad == 0) w.copy_buffer_to_image(0, 0, 0, 2, 0, 3, 1, 1, 0, 16);
    if (bad == 1) w.copy_buffer_to_image(0, 0, 0, 0, 0, 3, 1, 1, 0, 8);
    if (bad == 2) w.copy_buffer_to_image(0, 1, 0, 0, 0, 1, 1, 1, 0, 4);
    if (bad == 3) w.copy_buffer_to_image(0, 0, 0, 0, 0, 4, 4, 1, 0, 16);
    CHECK(e.render(w, {img, e.buffer(kAllocVertexBuffer, 32)}) == status::kInvalidParameter);
  }
  // the small levels of a block-compressed image are one block each: a copy writes the whole 4 x 4 block of the 2 x 2 and 1 x 1 levels (Direct3D 9's LockRect of
  // such a level, Direct3D 12's CopyTextureRegion of it); a rectangle past the block is still refused
  {
    Handle bc = e.alloc(kTexture, Format::Bc1, 8, 8, 4);      // (levels 8, 4, 2, 1)
    Handle blocks = e.buffer(kAllocVertexBuffer, 64), out = e.buffer(kAllocVertexBuffer, 64);
    std::vector<uint8_t> block(64, 0);
    for (uint32_t i = 0; i < 8; ++i) block[i] = static_cast<uint8_t>(0xC0 + i);
    CHECK(e.write(blocks, bytes_of(block)));
    for (uint32_t level = 2; level < 4; ++level) {
      {
        CommandWriter w(e.allocator);
        w.copy_buffer_to_image(0, level, 0, 0, 0, 4, 4, 1, 0, 8);
        CHECK(e.render(w, {bc, blocks}) == status::kSuccess);
      }
      {
        CommandWriter w(e.allocator);
        w.copy_image_to_buffer(0, level, 0, 0, 0, 4, 4, 1, 8 * level, 8);
        CHECK(e.render(w, {bc, out}) == status::kSuccess);
      }
      CHECK(read(out, 8 * level) == 0xC0 && read(out, 8 * level + 7) == 0xC7);
      CommandWriter w(e.allocator);
      w.copy_buffer_to_image(0, level, 0, 4, 0, 4, 4, 1, 0, 8);      // (a second block: the level has one)
      CHECK(e.render(w, {bc, blocks}) == status::kInvalidParameter);
    }
  }
  // narrow layouts: R8 and R8G8 rows into an A8B8G8R8 image become (r, g or 0, 0, 255), and come out as the first channels; R16G16 of an A16B16G16R16 image the same in 16 bits
  {
    Handle wide8 = e.alloc(kTexture, Format::A8B8G8R8, 4, 2);
    CHECK(e.fill(wide8, 0x11223344u));
    Handle narrow = e.buffer(kAllocVertexBuffer, 64), out = e.buffer(kAllocVertexBuffer, 64);
    std::vector<uint8_t> r8(64, 0);
    for (uint32_t i = 0; i < 16; ++i) r8[i] = static_cast<uint8_t>(0xA0 + i);
    CHECK(e.write(narrow, bytes_of(r8)));
    {
      CommandWriter w(e.allocator);
      w.copy_buffer_image_narrow(0, 0, 0, 0, 0, 0, 4, 2, 1, 0, 8, NarrowLayout::R8);      // (rows 8 bytes apart: 4 used)
      CHECK(e.render(w, {wide8, narrow}) == status::kSuccess);
    }
    CHECK(e.pixel(wide8, 0, 0) == 0xFF0000A0u && e.pixel(wide8, 3, 0) == 0xFF0000A3u && e.pixel(wide8, 0, 1) == 0xFF0000A8u);
    {
      CommandWriter w(e.allocator);
      w.copy_buffer_image_narrow(0, 0, 0, 0, 1, 1, 2, 1, 1, 0, 16, NarrowLayout::R8G8);      // (a 2 x 1 rectangle at (1, 1): pairs A0A1, A2A3)
      CHECK(e.render(w, {wide8, narrow}) == status::kSuccess);
    }
    CHECK(e.pixel(wide8, 1, 1) == 0xFF00A1A0u && e.pixel(wide8, 2, 1) == 0xFF00A3A2u && e.pixel(wide8, 3, 1) == 0xFF0000ABu);
    {
      CommandWriter w(e.allocator);
      w.copy_buffer_image_narrow(1, 0, 0, 0, 0, 1, 4, 1, 1, 4, 4, NarrowLayout::R8);
      CHECK(e.render(w, {wide8, out}) == status::kSuccess);
    }
    CHECK(read(out, 4) == 0xA8 && read(out, 5) == 0xA0 && read(out, 6) == 0xA2 && read(out, 7) == 0xAB && read(out, 3) == 0);
    Handle wide16 = e.alloc(kTexture, Format::A16B16G16R16, 2, 1);
    std::vector<uint8_t> rg16 = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08};
    Handle n16 = e.buffer(kAllocVertexBuffer, 16);
    CHECK(e.write(n16, bytes_of(rg16)));
    {
      CommandWriter w(e.allocator);
      w.copy_buffer_image_narrow(0, 0, 0, 0, 0, 0, 2, 1, 1, 0, 8, NarrowLayout::R16G16);
      CHECK(e.render(w, {wide16, n16}) == status::kSuccess);
    }
    LockResult lr;
    if (e.kernel.lock(kPid, wide16, kLockReadOnly, 0, &lr) == status::kSuccess) {
      const uint8_t want[16] = {0x01, 0x02, 0x03, 0x04, 0, 0, 0xFF, 0xFF, 0x05, 0x06, 0x07, 0x08, 0, 0, 0xFF, 0xFF};
      bool same = lr.data.size() >= 16;
      for (uint32_t i = 0; same && i < 16; ++i) same = lr.data[i] == want[i];
      CHECK(same);
      e.kernel.unlock(kPid, wide16);
    } else {
      CHECK(false);
    }
    // refused: a narrow layout of an image it does not fit (8-bit layout on a 16-bit image, a BGRA image), a layout number that is not one, a pitch shorter than the narrow row
    for (int bad = 0; bad < 4; ++bad) {
      CommandWriter w(e.allocator);
      Handle bgra = e.alloc(kTexture, Format::A8R8G8B8, 4, 2);
      if (bad == 0) w.copy_buffer_image_narrow(0, 0, 0, 0, 0, 0, 2, 1, 1, 0, 8, NarrowLayout::R8);
      if (bad == 1) w.copy_buffer_image_narrow(0, 2, 0, 0, 0, 0, 2, 1, 1, 0, 8, NarrowLayout::R8);
      if (bad == 2) w.copy_buffer_image_narrow(0, 3, 0, 0, 0, 0, 2, 1, 1, 0, 8, static_cast<NarrowLayout>(0x7F));
      if (bad == 3) w.copy_buffer_image_narrow(0, 3, 0, 0, 0, 0, 4, 1, 1, 0, 6, NarrowLayout::R8G8);
      CHECK(e.render(w, {wide16, n16, bgra, wide8}) == status::kInvalidParameter);
    }
  }

  // the escapes: a root signature (RTS0 free serialized form, version 1.0, no parameters), a descriptor heap, descriptors, a fence's value
  auto escape = [&](std::vector<uint32_t> request, uint32_t reply_words, std::vector<uint32_t>* reply = nullptr) {
    std::vector<uint32_t> buffer(std::max<size_t>(request.size(), reply_words) + 1, 0);
    std::copy(request.begin(), request.end(), buffer.begin());
    uint32_t reply_bytes = 0;
    const NtStatus st = e.kernel.escape(kPid, e.dev, ConstByteView(reinterpret_cast<const uint8_t*>(request.data()), request.size() * 4),
                                        ByteView(reinterpret_cast<uint8_t*>(buffer.data()), buffer.size() * 4), &reply_bytes);
    if (reply) reply->assign(buffer.begin(), buffer.begin() + reply_bytes / 4);
    return st;
  };
  std::vector<uint32_t> reply;
  const uint32_t empty_signature[6] = {1, 0, 24, 0, 24, 0};
  std::vector<uint32_t> create_signature = {6, sizeof empty_signature};
  create_signature.insert(create_signature.end(), empty_signature, empty_signature + 6);
  CHECK(escape(create_signature, 1, &reply) == status::kSuccess && reply.size() == 1 && reply[0] != 0);
  const uint32_t signature = reply.empty() ? 0 : reply[0];
  CHECK(escape({7, signature}, 0) == status::kSuccess && escape({7, signature}, 0) == status::kInvalidHandle);
  CHECK(escape({6, 3, 0x01020304}, 1) == status::kInvalidParameter);
  CHECK(escape({8, 0, 4}, 1, &reply) == status::kSuccess && reply.size() == 1);
  const uint32_t heap = reply.empty() ? 0 : reply[0];
  CHECK(escape({8, 2, 4}, 1) == status::kInvalidParameter);
  // two descriptors: a constant buffer view of a vertex buffer range (index 1), a cleared one (index 2)
  std::vector<uint32_t> write = {10, heap, 2};
  const uint32_t d1[14] = {1, 3, a, 16, 0, 32, 0, 0, 0, 0, 0, 0, 0, 0}, d2[14] = {2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
  write.insert(write.end(), d1, d1 + 14);
  write.insert(write.end(), d2, d2 + 14);
  CHECK(escape(write, 0) == status::kSuccess);
  CHECK(escape({11, heap, 0, heap, 1, 2}, 0) == status::kSuccess);
  CHECK(escape({11, heap, 3, heap, 1, 2}, 0) == status::kInvalidParameter);
  CHECK(escape({10, heap, 1, 9, 3, a, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}, 0) == status::kInvalidParameter);      // (a descriptor past the heap)
  CHECK(escape({9, heap}, 0) == status::kSuccess && escape({9, heap}, 0) == status::kInvalidHandle);
  Handle fence = 0;
  CHECK(e.kernel.create_sync(kPid, e.dev, SyncType::Fence, 5, 0, &fence) == status::kSuccess);
  CHECK(escape({13, fence}, 2, &reply) == status::kSuccess && reply.size() == 2 && reply[0] == 5 && reply[1] == 0);
  const uint64_t value = 0x100000007ull;
  Handle handles[1] = {fence};
  CHECK(e.kernel.signal_sync_cpu(kPid, ArrayView<const Handle>(handles, 1), ArrayView<const uint64_t>(&value, 1)) == status::kSuccess);
  CHECK(escape({13, fence}, 2, &reply) == status::kSuccess && reply.size() == 2 && reply[0] == 7 && reply[1] == 1);
  CHECK(escape({13, 0x7777}, 2) == status::kInvalidHandle);
  CHECK(escape({99}, 0) == status::kNotSupported);
}

// A reserved texture with a mip chain has the tiles of its standard levels and one more for the mip tail; stream output offsets are asked with an escape.
void test_reserved_mip_tail() {
  Env e;
  AllocationDesc td;
  td.flags = kAllocTexture | kAllocReserved, td.format = Format::A8R8G8B8, td.width = 256, td.height = 256, td.mip_levels = 9;
  Handle texture = 0;
  CHECK(e.kernel.create_allocation(kPid, e.dev, td, &texture) == status::kSuccess);
  AllocationDesc hd;
  hd.flags = kAllocHeap, hd.size = 7 * 65536;
  Handle heap = 0;
  CHECK(e.kernel.create_allocation(kPid, e.dev, hd, &heap) == status::kSuccess);
  CHECK(e.kernel.update_tile_mappings(kPid, texture, 0, heap, 0, 6) == status::kSuccess);      // 4 + 1 tiles of the standard levels and the tail
  CHECK(e.kernel.update_tile_mappings(kPid, texture, 0, heap, 0, 7) == status::kInvalidParameter);
  td.mip_levels = 10;
  Handle bad = 0;
  CHECK(e.kernel.create_allocation(kPid, e.dev, td, &bad) == status::kInvalidParameter);      // (more levels than the size has)
  Handle buffer = e.buffer(kAllocVertexBuffer, 64);
  uint32_t request[2] = {14, buffer}, reply[4] = {};
  uint32_t reply_bytes = 0;
  CHECK(e.kernel.escape(kPid, e.dev, ConstByteView(reinterpret_cast<const uint8_t*>(request), 8), ByteView(reinterpret_cast<uint8_t*>(reply), 16), &reply_bytes) == status::kInvalidParameter);   // (not a stream output buffer)
}

// Reserved textures of other texel sizes: Direct3D's tile shapes (R8 256 x 256, R8G8 / R16 256 x 128, R16G16 128 x 128, 64-bit 128 x 64, 128-bit 64 x 64), the narrow
// texels of R8 / R8G8 / R16 / R16G16 in the heap widened in the image and narrowed back, and packed mip levels over several tiles.
void test_reserved_texel_sizes() {
  Env e;
  AllocationDesc hd;
  hd.flags = kAllocHeap, hd.size = 16 * kTileBytes;
  Handle heap = 0;
  CHECK(e.kernel.create_allocation(kPid, e.dev, hd, &heap) == status::kSuccess);
  // (a buffer placed over the whole heap: what the program's other resources would see there)
  AllocationDesc bd;
  bd.flags = kAllocVertexBuffer, bd.size = 16 * kTileBytes, bd.heap = heap;
  Handle view = 0;
  CHECK(e.kernel.create_allocation(kPid, e.dev, bd, &view) == status::kSuccess);
  auto reserved = [&](Format f, NarrowLayout narrow, uint32_t w, uint32_t h, uint32_t mips, Handle* out) {
    AllocationDesc d;
    d.flags = kAllocTexture | kAllocReserved, d.format = f, d.width = w, d.height = h, d.mip_levels = mips, d.tile_narrow = static_cast<uint32_t>(narrow);
    return e.kernel.create_allocation(kPid, e.dev, d, out);
  };
  auto tiles_of = [&](Handle r) {      // (how many tiles it has: the last one that maps)
    uint32_t n = 0;
    while (n < 64 && e.kernel.update_tile_mappings(kPid, r, n, 0, 0, 1) == status::kSuccess) ++n;
    return n;
  };
  Handle r8 = 0, rg8 = 0, r16 = 0, rg16 = 0, wide = 0, wide32 = 0, plain = 0, none = 0;
  CHECK(reserved(Format::A8B8G8R8, NarrowLayout::R8, 512, 256, 1, &r8) == status::kSuccess && tiles_of(r8) == 2);          // 256 x 256
  CHECK(reserved(Format::A8B8G8R8, NarrowLayout::R8G8, 256, 256, 1, &rg8) == status::kSuccess && tiles_of(rg8) == 2);      // 256 x 128
  CHECK(reserved(Format::A16B16G16R16, NarrowLayout::R16, 256, 128, 1, &r16) == status::kSuccess && tiles_of(r16) == 1);   // 256 x 128
  CHECK(reserved(Format::A16B16G16R16, NarrowLayout::R16G16, 256, 128, 1, &rg16) == status::kSuccess && tiles_of(rg16) == 2);   // 128 x 128
  CHECK(reserved(Format::A16B16G16R16, NarrowLayout::None, 128, 128, 1, &wide) == status::kSuccess && tiles_of(wide) == 2);       // 128 x 64
  CHECK(reserved(Format::A32B32G32R32F, NarrowLayout::None, 128, 64, 1, &wide32) == status::kSuccess && tiles_of(wide32) == 2);   // 64 x 64
  CHECK(reserved(Format::A8R8G8B8, NarrowLayout::R8, 256, 256, 1, &none) == status::kInvalidParameter);           // (R8 of an image that is not A8B8G8R8)
  CHECK(reserved(Format::A8B8G8R8, NarrowLayout::R16, 256, 256, 1, &none) == status::kInvalidParameter);          // (R16 of an 8-bit image)
  CHECK(reserved(Format::A8B8G8R8, static_cast<NarrowLayout>(9), 256, 256, 1, &none) == status::kInvalidParameter);
  // R8: the heap's bytes are the texels; the image has (r, 0, 0, 255). Tile 0 is the left 256 x 256, mapped to heap tile 3; tile 1 nowhere.
  std::vector<uint8_t> heap_bytes(16 * kTileBytes);
  for (uint32_t y = 0; y < 256; ++y)
    for (uint32_t x = 0; x < 256; ++x) heap_bytes[3 * kTileBytes + y * 256 + x] = static_cast<uint8_t>(x ^ y);
  CHECK(e.write(view, ConstByteView(heap_bytes.data(), heap_bytes.size())));
  CHECK(e.kernel.update_tile_mappings(kPid, r8, 0, heap, 3 * kTileBytes, 1) == status::kSuccess);
  LockResult lock;
  CHECK(e.kernel.lock(kPid, r8, kLockReadOnly, 0, &lock) == status::kSuccess && lock.data.size() == 512u * 256 * 4);
  bool good = true;
  for (uint32_t y = 0; y < 256 && good; y += 17)
    for (uint32_t x = 0; x < 512 && good; x += 13) {
      ByteReader px(lock.data.subview((uint64_t{y} * 512 + x) * 4, 4));
      good = px.le32() == (x < 256 ? 0xFF000000u | ((x ^ y) & 0xFF) : 0xFF000000u);      // (nowhere: zero texels, widened)
    }
  CHECK(good);
  e.kernel.unlock(kPid, r8);
  // Written through the texture: the red channel goes to the heap's byte, the others are not in it.
  CHECK(e.kernel.lock(kPid, r8, 0, 0, &lock) == status::kSuccess);
  ByteWriter(lock.data.subview((uint64_t{5} * 512 + 7) * 4, 4)).le32(0x80402099u);
  ByteWriter(lock.data.subview((uint64_t{5} * 512 + 300) * 4, 4)).le32(0xFF0000AAu);      // (an unmapped tile: dropped)
  e.kernel.unlock(kPid, r8);
  CHECK(e.kernel.lock(kPid, view, kLockReadOnly, 0, &lock) == status::kSuccess);
  CHECK(lock.data[3 * kTileBytes + 5 * 256 + 7] == 0x99 && lock.data[3 * kTileBytes + 5 * 256 + 8] == ((8 ^ 5) & 0xFF));
  e.kernel.unlock(kPid, view);
  CHECK(e.pixel(r8, 300, 5) == 0xFF000000u);
  // R16G16 on 128 x 128 tiles: tile 1 (the right half) mapped to heap tile 5, its texel (x, y) at 5 * 64 KiB + (y * 128 + x - 128) * 4.
  CHECK(e.kernel.update_tile_mappings(kPid, rg16, 1, heap, 5 * kTileBytes, 1) == status::kSuccess);
  CHECK(e.kernel.lock(kPid, view, 0, 0, &lock) == status::kSuccess);
  ByteWriter(lock.data.subview(5 * kTileBytes + (uint64_t{9} * 128 + 3) * 4, 4)).le32(0xBEEF1234u);
  e.kernel.unlock(kPid, view);
  CHECK(e.kernel.lock(kPid, rg16, kLockReadOnly, 0, &lock) == status::kSuccess);
  {
    ByteReader px(lock.data.subview((uint64_t{9} * 256 + 131) * 8, 8));
    const uint16_t r = px.le16(), g = px.le16(), b = px.le16(), a = px.le16();
    CHECK(r == 0x1234 && g == 0xBEEF && b == 0 && a == 0xFFFF);
  }
  e.kernel.unlock(kPid, rg16);
  // 64-bit pixels on 128 x 64 tiles: tile 1 is the lower half.
  CHECK(e.kernel.update_tile_mappings(kPid, wide, 1, heap, 6 * kTileBytes, 1) == status::kSuccess);
  CHECK(e.kernel.lock(kPid, wide, 0, 0, &lock) == status::kSuccess);
  ByteWriter(lock.data.subview((uint64_t{70} * 128 + 2) * 8, 8)).le64(0x0102030405060708ull);
  e.kernel.unlock(kPid, wide);
  CHECK(e.kernel.lock(kPid, view, kLockReadOnly, 0, &lock) == status::kSuccess);
  CHECK(ByteReader(lock.data.subview(6 * kTileBytes + (uint64_t{6} * 128 + 2) * 8, 8)).le64() == 0x0102030405060708ull);
  e.kernel.unlock(kPid, view);
  // Packed levels over several tiles: an R8 texture 4096 x 64 has no standard level (64 < 256); its 7 levels' texels (262144 + 65536 + ... bytes) fill 6 tiles.
  Handle tall = 0;
  CHECK(reserved(Format::A8B8G8R8, NarrowLayout::R8, 4096, 64, 7, &tall) == status::kSuccess && tiles_of(tall) == 6);
  // (map the six to heap tiles 8 .. 13 and mark the first texel of level 1, which is at 262144 in the packed run: tile 4, offset 0)
  CHECK(e.kernel.update_tile_mappings(kPid, tall, 0, heap, 8 * kTileBytes, 6) == status::kSuccess);
  CHECK(e.kernel.lock(kPid, view, 0, 0, &lock) == status::kSuccess);
  lock.data[12 * kTileBytes] = 0x5C;
  lock.data[12 * kTileBytes + 2048 + 1] = 0x3D;      // (level 1 is 2048 wide: texel (1, 1))
  e.kernel.unlock(kPid, view);
  CHECK((e.pixel(tall, 0, 0, 1) & 0xFFu) == 0x5C && (e.pixel(tall, 1, 1, 1) & 0xFFu) == 0x3D);
  // A plain 4-byte texture keeps the old layout: 128 x 128 tiles and the packed levels after them.
  CHECK(reserved(Format::A8R8G8B8, NarrowLayout::None, 256, 128, 8, &plain) == status::kSuccess && tiles_of(plain) == 3);
}

// The software renderer behind a device that reports the limits of the fixed pipelines (DeviceInfo::limits at their defaults): what the device takes is checked against what it says.
class NarrowDevice final : public graphics::IGraphicsDevice {
 public:
  explicit NarrowDevice(graphics::IGraphicsDevice& inner) : inner_(inner) {}
  graphics::DeviceInfo info() const override {
    graphics::DeviceInfo i = inner_.info();
    i.limits = graphics::DeviceLimits();
    return i;
  }
  graphics::IBuffer* create_buffer(uint64_t size, uint32_t usage) override { return inner_.create_buffer(size, usage); }
  void destroy_buffer(graphics::IBuffer* b) override { inner_.destroy_buffer(b); }
  graphics::IImage* create_image(const graphics::ImageDesc& d) override { return inner_.create_image(d); }
  void destroy_image(graphics::IImage* i) override { inner_.destroy_image(i); }
  graphics::IShaderModule* create_shader_module(const graphics::ShaderModuleDesc& d) override { return inner_.create_shader_module(d); }
  void destroy_shader_module(graphics::IShaderModule* m) override { inner_.destroy_shader_module(m); }
  graphics::IPipeline* create_graphics_pipeline(const graphics::GraphicsPipelineDesc& d) override { return inner_.create_graphics_pipeline(d); }
  graphics::IPipeline* create_compute_pipeline(const graphics::IShaderModule* s) override { return inner_.create_compute_pipeline(s); }
  void destroy_pipeline(graphics::IPipeline* p) override { inner_.destroy_pipeline(p); }
  graphics::ICommandBuffer* create_command_buffer() override { return inner_.create_command_buffer(); }
  void destroy_command_buffer(graphics::ICommandBuffer* c) override { inner_.destroy_command_buffer(c); }
  bool submit(graphics::ICommandBuffer* c) override { return inner_.submit(c); }
  bool present(graphics::IImage* i) override { return inner_.present(i); }

 private:
  graphics::IGraphicsDevice& inner_;
};

void test_device_limits() {
  HostAllocator allocator;
  FakeScreen screen(8, 6);
  graphics::SoftwareDevice software(allocator, &screen);
  NarrowDevice narrow(software);
  Kernel kernel(allocator);
  AdapterConfig config;
  config.display = &screen;
  Handle adapter = 0, dev = 0, ctx = 0;
  CHECK(kernel.add_adapter(narrow, config) == 0);
  CHECK(kernel.open_process(kPid) == status::kSuccess && kernel.open_adapter_from_index(kPid, 0, &adapter) == status::kSuccess);
  CHECK(kernel.create_device(kPid, adapter, &dev) == status::kSuccess && kernel.create_context(kPid, dev, ContextPriority::Normal, &ctx) == status::kSuccess);
  AdapterCaps caps;
  CHECK(kernel.query_adapter_caps(kPid, adapter, &caps) == status::kSuccess);
  CHECK(caps.max_vertex_attributes == 4 && caps.max_varyings == 4 && caps.max_texture_units == 2 && caps.max_color_targets == 4 && caps.max_vertex_buffers == 1);
  // the software renderer itself takes the whole capacity
  Kernel full(allocator);
  full.add_adapter(software, config);
  AdapterCaps all;
  CHECK(full.open_process(kPid) == status::kSuccess);
  Handle fa = 0;
  CHECK(full.open_adapter_from_index(kPid, 0, &fa) == status::kSuccess && full.query_adapter_caps(kPid, fa, &all) == status::kSuccess);
  CHECK(all.max_vertex_attributes == graphics::kMaxVertexAttributes && all.max_varyings == graphics::kMaxVaryings && all.max_texture_units == graphics::kMaxTextureUnits &&
        all.max_color_targets == graphics::kMaxColorTargets && all.max_vertex_buffers == graphics::kMaxVertexBuffers);

  Handle vs = 0, fs = 0;
  CHECK(kernel.create_shader(kPid, dev, {ShaderKind::ColoredVertex, graphics::ShaderStage::Vertex, {}}, &vs) == status::kSuccess);
  CHECK(kernel.create_shader(kPid, dev, {ShaderKind::ColoredFragment, graphics::ShaderStage::Fragment, {}}, &fs) == status::kSuccess);
  auto pipeline = [&](auto change) {
    graphics::GraphicsPipelineDesc d;
    graphics::describe_colored_pipeline(&d);
    PipelineDesc pd;
    pd.vertex_shader = vs, pd.fragment_shader = fs;
    pd.vertex_stride = d.vertex_stride, pd.attribute_count = d.attribute_count, pd.varying_count = d.varying_count, pd.texture_count = d.texture_count;
    for (uint32_t i = 0; i < d.attribute_count; ++i) pd.attributes[i] = d.attributes[i];
    pd.cull_mode = graphics::CullMode::None;
    change(pd);
    Handle h = 0;
    return kernel.create_pipeline(kPid, dev, pd, &h);
  };
  CHECK(pipeline([](PipelineDesc&) {}) == status::kSuccess);
  CHECK(pipeline([](PipelineDesc& pd) { pd.attribute_count = 5; pd.vertex_stride = 80; for (uint32_t i = 0; i < 5; ++i) pd.attributes[i] = {i, graphics::VertexFormat::Float4, i * 16}; }) == status::kInvalidParameter);
  CHECK(pipeline([](PipelineDesc& pd) { pd.attributes[1].slot = 1; pd.slot_strides[1] = 16; }) == status::kInvalidParameter);      // (a second vertex buffer)
  CHECK(pipeline([](PipelineDesc& pd) { pd.texture_count = 3; }) == status::kInvalidParameter);
  CHECK(pipeline([](PipelineDesc& pd) { pd.varying_count = 5; }) == status::kInvalidParameter);
  CHECK(pipeline([](PipelineDesc& pd) { pd.color_target_count = 5; }) == status::kInvalidParameter);
  // a pixel shader that samples four textures, and a vertex shader with five inputs, are not for this device either; the full renderer takes them
  Handle ps4 = 0, vs6 = 0;
  const std::vector<uint8_t> tex4 = read_blob("ps_tex4.dxbc"), attrs6 = read_blob("vs_attrs6.dxbc");
  CHECK(kernel.create_shader(kPid, dev, {ShaderKind::Dxbc, graphics::ShaderStage::Fragment, ConstByteView(tex4.data(), tex4.size())}, &ps4) == status::kNotSupported);
  CHECK(kernel.create_shader(kPid, dev, {ShaderKind::Dxbc, graphics::ShaderStage::Vertex, ConstByteView(attrs6.data(), attrs6.size())}, &vs6) == status::kNotSupported);
  Handle full_dev = 0;
  CHECK(full.create_device(kPid, fa, &full_dev) == status::kSuccess);
  CHECK(full.create_shader(kPid, full_dev, {ShaderKind::Dxbc, graphics::ShaderStage::Fragment, ConstByteView(tex4.data(), tex4.size())}, &ps4) == status::kSuccess);
  CHECK(full.create_shader(kPid, full_dev, {ShaderKind::Dxbc, graphics::ShaderStage::Vertex, ConstByteView(attrs6.data(), attrs6.size())}, &vs6) == status::kSuccess);
  // the stream: a texture unit, a pass with more targets, a vertex buffer slot beyond the device's
  Handle t0 = 0, rt = 0, rt2 = 0, rt3 = 0, rt4 = 0, rt5 = 0, vb = 0;
  AllocationDesc td;
  td.flags = kTexture, td.format = Format::A8R8G8B8, td.width = 4, td.height = 4;
  CHECK(kernel.create_allocation(kPid, dev, td, &t0) == status::kSuccess);
  td.flags = kTarget;
  for (Handle* t : {&rt, &rt2, &rt3, &rt4, &rt5}) CHECK(kernel.create_allocation(kPid, dev, td, t) == status::kSuccess);
  AllocationDesc bd;
  bd.flags = kAllocVertexBuffer, bd.size = 64;
  CHECK(kernel.create_allocation(kPid, dev, bd, &vb) == status::kSuccess);
  auto run = [&](auto record) {
    CommandWriter w(allocator);
    record(w);
    const Handle list[] = {t0, rt, rt2, vb, rt3, rt4, rt5};
    return kernel.render(kPid, ctx, w.view(), ArrayView<const Handle>(list, 7));
  };
  CHECK(run([](CommandWriter& w) { w.bind_texture(1, 0, graphics::SamplerDesc()); }) == status::kSuccess);
  CHECK(run([](CommandWriter& w) { w.bind_texture(2, 0, graphics::SamplerDesc()); }) == status::kInvalidParameter);
  CHECK(run([](CommandWriter& w) { w.begin_pass_targets(1, {2, 4, 5}, kNoAllocation, kPassClearColor, {0, 0, 0, 1}, 1, 0); w.end_pass(); }) == status::kSuccess);      // (four targets)
  CHECK(run([](CommandWriter& w) { w.begin_pass_targets(1, {2, 4, 5, 6}, kNoAllocation, kPassClearColor, {0, 0, 0, 1}, 1, 0); w.end_pass(); }) == status::kInvalidParameter);      // (a fifth)
  CHECK(run([](CommandWriter& w) { w.bind_vertex_buffer(3, 0); }) == status::kSuccess);
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


// Reserved (tiled) textures: 128 x 128 tiles of 4-byte pixels mapped to places in a heap.
void test_d3d12_reserved_texture() {
  Env e;
  auto create = [&](uint32_t flags, uint32_t width, uint32_t height, Handle heap, uint64_t offset, Handle* out) {
    AllocationDesc d;
    d.flags = flags, d.format = Format::A8R8G8B8, d.width = width, d.height = height, d.heap = heap, d.heap_offset = offset;
    return e.kernel.create_allocation(kPid, e.dev, d, out);
  };
  Handle heap = 0, p = 0, q = 0, r = 0, none = 0;
  CHECK(create(kAllocHeap, 0, 0, 0, 0, &heap) == status::kInvalidParameter);   // (a heap has a size, not a shape)
  AllocationDesc hd;
  hd.flags = kAllocHeap, hd.size = 4 * kTileBytes;
  CHECK(e.kernel.create_allocation(kPid, e.dev, hd, &heap) == status::kSuccess);
  CHECK(create(kAllocTexture, 128, 128, heap, 0, &p) == status::kSuccess && create(kAllocTexture, 128, 128, heap, 2 * kTileBytes, &q) == status::kSuccess);
  CHECK(create(kAllocTexture | kAllocReserved, 256, 256, 0, 0, &r) == status::kSuccess);
  CHECK(create(kAllocTexture | kAllocReserved, 256, 0, 0, 0, &none) == status::kInvalidParameter);
  Handle edge = 0;
  CHECK(create(kAllocTexture | kAllocReserved, 200, 256, 0, 0, &edge) == status::kSuccess);      // (any size: its edge tiles partly used)
  CHECK(e.kernel.update_tile_mappings(kPid, edge, 0, heap, 0, 4) == status::kSuccess && e.kernel.update_tile_mappings(kPid, edge, 4, heap, 0, 1) == status::kInvalidParameter);
  CHECK(create(kAllocTexture | kAllocPresent | kAllocReserved, 256, 256, 0, 0, &none) == status::kInvalidParameter && create(kAllocTexture | kAllocReserved, 256, 256, heap, 0, &none) == status::kInvalidParameter);
  CHECK(e.kernel.update_tile_mappings(kPid, r, 0, heap, 0, 1) == status::kSuccess && e.kernel.update_tile_mappings(kPid, r, 3, heap, 2 * kTileBytes, 1) == status::kSuccess);
  CHECK(e.kernel.update_tile_mappings(kPid, r, 4, heap, 0, 1) == status::kInvalidParameter && e.kernel.update_tile_mappings(kPid, r, 0, heap, 3 * kTileBytes, 2) == status::kInvalidParameter);
  CHECK(e.fill(p, 0xFF112233u) && e.fill(q, 0xFF445566u));
  // Reading it: tile 0 holds what the placed texture at the start of the heap does, tile 3 the other, the tiles between nothing.
  CHECK(e.pixel(r, 10, 10) == 0xFF112233u && e.pixel(r, 127, 127) == 0xFF112233u && e.pixel(r, 128, 128) == 0xFF445566u && e.pixel(r, 255, 255) == 0xFF445566u);
  CHECK(e.pixel(r, 200, 10) == 0 && e.pixel(r, 10, 200) == 0);
  // Writing a pixel of tile 3 through the reserved texture: the placed texture shows it at the same place of its own; a write to an unmapped tile is dropped.
  LockResult lock;
  CHECK(e.kernel.lock(kPid, r, 0, 0, &lock) == status::kSuccess);
  const uint32_t mark = 0xFF778899u;
  ByteWriter(lock.data.subview((200u * 256 + 200) * 4, 4)).le32(mark);
  ByteWriter(lock.data.subview((10u * 256 + 200) * 4, 4)).le32(mark);
  e.kernel.unlock(kPid, r);
  CHECK(e.pixel(q, 72, 72) == mark && e.pixel(r, 200, 10) == 0);
  // And the GPU reads it: a triangle that shows the texture over the target gives one quadrant of each kind (two black, one of each colour).
  Handle target = e.alloc(kTarget, Format::A8R8G8B8, 8, 8), vb = e.buffer(kAllocVertexBuffer, 3 * 20);
  Handle pipe = make_colored_pipeline(e, true);
  const float quad[15] = {-1, -1, 0.5f, 0, 0, 3, -1, 0.5f, 2, 0, -1, 3, 0.5f, 0, 2};
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
  CHECK(e.render(w, {target, r, vb}) == status::kSuccess);
  unsigned first = 0, second = 0, black = 0;
  for (uint32_t y : {2u, 6u})
    for (uint32_t x : {2u, 6u}) {
      const uint32_t c = e.pixel(target, x, y);
      first += c == 0xFF112233u, second += c == 0xFF445566u || c == mark, black += (c & 0xFFFFFF) == 0;
    }
  CHECK(first == 1 && second == 1 && black == 2);
  // Two tiles on one place of the heap (Direct3D's REUSE_SINGLE_TILE): a write through the first is what the second shows - the unchanged alias does not put the old texels back.
  CHECK(e.kernel.update_tile_mappings(kPid, r, 1, heap, 2 * kTileBytes, 1) == status::kSuccess);
  const uint32_t alias_mark = 0xFF0A0B0Cu;
  CHECK(e.kernel.lock(kPid, r, 0, 0, &lock) == status::kSuccess);
  ByteWriter(lock.data.subview((82u * 256 + 210) * 4, 4)).le32(alias_mark);      // (tile 1, its texel 82, 82)
  e.kernel.unlock(kPid, r);
  CHECK(e.pixel(q, 82, 82) == alias_mark && e.pixel(r, 210, 210) == alias_mark && e.pixel(r, 210, 82) == alias_mark);
}

// Reserved textures beyond tier 1 (VidMm ReservedLayout): the tile numbering of arrays, volumes and block-compressed formats; render targets whose unmapped tiles drop what is drawn;
// CopyTiles; residency feedback (sample_cl_s, ld_s, gather4_s and check_access_fully_mapped: ps_tiled) and a lookup's own clamp (ps_tiled_clamp) on the CPU renderer.
// Shader Model 5's bit operations (cs_bits: countbits, firstbit_hi / _lo / _shi, ubfe, ibfe, bfi, bfrev) become TGSI's POPC, LSB, UBFE, IBFE, BFI, BREV (d3d12test
// checks what they compute against the HLSL on every device); a geometry shader whose inputs a register indexes (gs_loop: v[r0.x][register]) compiles, its inputs gathered by movc.
void test_bits_and_indexed_inputs() {
  HostAllocator allocator;
  NtStatus st = 0;
  const std::vector<uint8_t> bits_blob = read_blob("cs_bits.dxbc"), loop_blob = read_blob("gs_loop.dxbc");
  CHECK(!bits_blob.empty() && !loop_blob.empty());
  UniquePointer<D3d9Program> bits = compile_dxbc(allocator, bits_blob, &st);
  CHECK(bits && st == status::kSuccess);
  if (bits) {
    String text(allocator);
    CHECK(bits->emit_tgsi(&text) == status::kSuccess);
    for (const char* op : {"POPC", "LSB", "UBFE", "IBFE", "BFI", "BREV"})
      if (text.view().find(op) == StringView::npos) {
        printf("  bit operations: no %s in the TGSI\n", op);
        CHECK(false);
      }
  }
  UniquePointer<D3d9Program> loop = compile_dxbc(allocator, loop_blob, &st);
  CHECK(loop && st == status::kSuccess);
  if (loop) {
    String text(allocator);
    CHECK(loop->emit_tgsi(&text) == status::kSuccess);
  }
}

void test_reserved_tiers() {
  // --- the numbering
  {
    AllocationDesc d;
    d.flags = kAllocTexture, d.format = Format::A8B8G8R8, d.width = d.height = 256, d.mip_levels = 9, d.kind = graphics::TextureKind::Texture2DArray, d.layers = 3;
    ReservedLayout l;
    CHECK(reserved_layout(d, &l));
    // (256 x 256: 2 x 2 tiles and 1; the seven levels from 64 x 64 down packed, 21844 bytes: one tile per slice)
    CHECK(l.standard == 2 && l.columns[0] == 2 && l.rows[0] == 2 && l.first[1] == 4 && l.packed_first == 5 && l.packed_tiles == 1 && l.per_slice == 6 && l.total == 18 && l.slices == 3);
    d.kind = graphics::TextureKind::Texture3D, d.layers = 32, d.width = d.height = 64, d.mip_levels = 7;
    CHECK(reserved_layout(d, &l));
    // (a volume of 4-byte texels: 32 x 32 x 16 tiles; 64 x 64 x 32 is 2 x 2 x 2 of them, 32 x 32 x 16 one, the rest packed: 2341 texels)
    CHECK(l.volume && l.shape.width == 32 && l.shape.height == 32 && l.shape.depth == 16 && l.standard == 2 && l.columns[0] == 2 && l.deep[0] == 2 && l.first[1] == 8 &&
          l.packed_bytes == 2341 * 4 && l.packed_tiles == 1 && l.total == 10 && l.slices == 1);
    d.kind = graphics::TextureKind::Texture2D, d.layers = 1, d.format = Format::Bc1, d.width = 1024, d.height = 512, d.mip_levels = 3;
    CHECK(reserved_layout(d, &l));
    // (BC1 blocks of 8 bytes: 128 x 64 blocks, 512 x 256 texels a tile; 1024 x 512 is 2 x 2, 512 x 256 one, 256 x 128 packed: 64 x 32 blocks)
    CHECK(l.block == 4 && l.shape.width == 128 && l.shape.height == 64 && l.standard == 2 && l.first[1] == 4 && l.packed_bytes == 64 * 32 * 8 && l.total == 6);
    d.format = Format::A8B8G8R8, d.width = 300, d.height = 200, d.mip_levels = 1, d.tile_narrow = static_cast<uint32_t>(NarrowLayout::R8);
    CHECK(reserved_layout(d, &l));
    CHECK(l.element_bytes == 1 && l.image_bytes == 4 && l.shape.width == 256 && l.standard == 0 && l.packed_bytes == 300 * 200 && l.total == 1);   // (smaller than a tile: all packed)
  }
  Env e;
  AllocationDesc hd;
  hd.flags = kAllocHeap, hd.size = 8 * kTileBytes;
  Handle heap = 0;
  CHECK(e.kernel.create_allocation(kPid, e.dev, hd, &heap) == status::kSuccess);
  auto reserved = [&](uint32_t flags, Format f, uint32_t w, uint32_t h, uint32_t mips, graphics::TextureKind kind, uint32_t layers, uint32_t samples = 1) {
    AllocationDesc d;
    d.flags = flags | kAllocReserved, d.format = f, d.width = w, d.height = h, d.mip_levels = mips, d.kind = kind, d.layers = layers, d.samples = samples;
    Handle h_out = 0;
    return e.kernel.create_allocation(kPid, e.dev, d, &h_out) == status::kSuccess ? h_out : 0;
  };
  // (what may be reserved: not a multisampled target or a depth buffer)
  CHECK(!reserved(kAllocTexture | kAllocRenderTarget, Format::A8R8G8B8, 256, 256, 1, graphics::TextureKind::Texture2D, 1, 4));
  CHECK(reserved(kAllocTexture | kAllocRenderTarget, Format::A8R8G8B8, 256, 256, 1, graphics::TextureKind::Texture2DArray, 2) != 0);
  CHECK(reserved(kAllocTexture | kAllocStorageImage, Format::A8R8G8B8, 256, 256, 1, graphics::TextureKind::Texture3D, 16) != 0);
  CHECK(!reserved(kAllocTexture | kAllocRenderTarget, Format::Bc1, 256, 256, 1, graphics::TextureKind::Texture2D, 1));   // (block-compressed: sampled only)

  // --- a volume: tile 1 (texels 32 .. 63 across, 0 .. 31 down, slices 0 .. 15) from a buffer by CopyTiles, read back through the volume's backing and by CopyTiles again
  {
    const Handle volume = reserved(kAllocTexture, Format::A8R8G8B8, 64, 64, 1, graphics::TextureKind::Texture3D, 32);
    CHECK(volume != 0);
    CHECK(e.kernel.update_tile_mappings(kPid, volume, 1, heap, 2 * kTileBytes, 1) == status::kSuccess);
    std::vector<uint32_t> tile(kTileBytes / 4);
    for (uint32_t i = 0; i < tile.size(); ++i) tile[i] = 0xFF000000u | i;   // (texel x, y, z of the tile: (z * 32 + y) * 32 + x)
    const Handle up = e.buffer(kAllocVertexBuffer, 2 * kTileBytes), down = e.buffer(kAllocVertexBuffer, 2 * kTileBytes);
    CHECK(e.write(up, ConstByteView(reinterpret_cast<const uint8_t*>(tile.data()), kTileBytes)));
    CommandWriter w(e.allocator);
    w.copy_tiles(0, 0, 1, 1, 1, 0);
    w.copy_tiles(1, 0, 0, 2, 2, 0);   // (tiles 0 - unmapped: zeros - and 1 back out)
    CHECK(e.render(w, {volume, up, down}) == status::kSuccess);
    LockResult lock;
    CHECK(e.kernel.lock(kPid, volume, kLockReadOnly, 0, &lock) == status::kSuccess);
    auto texel = [&](uint32_t x, uint32_t y, uint32_t z) {
      ConstByteView v;
      return ConstByteView(lock.data).try_subview(((uint64_t{z} * 64 + y) * 64 + x) * 4, 4, &v) ? ByteReader(v).le32() : 0xDEADu;
    };
    CHECK(texel(40, 5, 3) == (0xFF000000u | ((3u * 32 + 5) * 32 + 8)) && texel(63, 31, 15) == (0xFF000000u | ((15u * 32 + 31) * 32 + 31)));
    CHECK(texel(5, 5, 3) == 0 && texel(40, 40, 3) == 0 && texel(40, 5, 16) == 0);   // (other tiles: unmapped, zero)
    e.kernel.unlock(kPid, volume);
    CHECK(e.kernel.lock(kPid, down, kLockReadOnly, 0, &lock) == status::kSuccess);
    bool zeros = true, same = true;
    for (uint32_t i = 0; i < kTileBytes / 4; ++i) {
      zeros = zeros && ByteReader(ConstByteView(lock.data).subview(uint64_t{i} * 4, 4)).le32() == 0;
      same = same && ByteReader(ConstByteView(lock.data).subview(kTileBytes + uint64_t{i} * 4, 4)).le32() == tile[i];
    }
    e.kernel.unlock(kPid, down);
    CHECK(zeros && same);
  }

  // --- a render target of four tiles, tile 0 mapped: a clear reaches the heap there and nowhere else; what lies in the unmapped tiles reads as zero afterwards
  {
    const Handle target = reserved(kAllocTexture | kAllocRenderTarget, Format::A8R8G8B8, 256, 256, 1, graphics::TextureKind::Texture2D, 1);
    CHECK(target != 0);
    CHECK(e.kernel.update_tile_mappings(kPid, target, 0, heap, 4 * kTileBytes, 1) == status::kSuccess);
    CommandWriter w(e.allocator);
    w.begin_pass(0, kNoAllocation, kPassClearColor, {0.0f, 1.0f, 0.0f, 1.0f}, 1, 0);
    w.end_pass();
    CHECK(e.render(w, {target}) == status::kSuccess);
    CHECK(e.pixel(target, 10, 10) == 0xFF00FF00u && e.pixel(target, 127, 127) == 0xFF00FF00u && e.pixel(target, 200, 10) == 0 && e.pixel(target, 10, 200) == 0);
    // (the heap holds it: a second reserved texture with its tile 3 there shows it)
    const Handle other = reserved(kAllocTexture, Format::A8R8G8B8, 256, 256, 1, graphics::TextureKind::Texture2D, 1);
    CHECK(e.kernel.update_tile_mappings(kPid, other, 3, heap, 4 * kTileBytes, 1) == status::kSuccess);
    CHECK(e.pixel(other, 130, 130) == 0xFF00FF00u && e.pixel(other, 10, 10) == 0);
    // (moved to another place of the heap: tile 0 shows what is there - nothing written yet -, the old place keeps the clear)
    CHECK(e.kernel.update_tile_mappings(kPid, target, 0, heap, 5 * kTileBytes, 1) == status::kSuccess);
    CHECK(e.pixel(target, 10, 10) == 0 && e.pixel(other, 130, 130) == 0xFF00FF00u);
  }

  // --- residency feedback: a 256 x 256 texture with tiles 0 and 3 mapped (red 0x80 in both), drawn over an 8 x 8 target at uv (x + 0.5) / 8, (y + 0.5) / 8
  const std::vector<uint8_t> ps_blob = read_blob("ps_tiled.dxbc"), clamp_blob = read_blob("ps_tiled_clamp.dxbc");
  CHECK(!ps_blob.empty() && !clamp_blob.empty());
  auto dxbc_ps = [&](const std::vector<uint8_t>& blob) {
    Handle h = 0;
    ShaderDesc d;
    d.kind = ShaderKind::Dxbc, d.stage = graphics::ShaderStage::Fragment, d.code = view_of(blob);
    CHECK(e.kernel.create_shader(kPid, e.dev, d, &h) == status::kSuccess);
    return h;
  };
  Asm vs(true, 2, 0);   // mov oPos, v0 ; mov oT0, v1   (the uv)
  vs.dcl(0, 0, kRegInput, 0).dcl(5, 0, kRegInput, 1);
  vs.op(kOpMov, {Asm::dst(kRegRastOut, 0), Asm::src(kRegInput, 0)});
  vs.op(kOpMov, {Asm::dst(kRegTexOut, 0), Asm::src(kRegInput, 1)});
  vs.end();
  const Handle vsh = d3d9_shader(e, vs);
  auto pipeline = [&](Handle ps) {
    PipelineDesc pd;
    pd.vertex_shader = vsh, pd.fragment_shader = ps, pd.vertex_stride = 20, pd.attribute_count = 2, pd.varying_count = 3, pd.texture_count = 1;
    pd.attributes[0] = {0, graphics::VertexFormat::Float3, 0};
    pd.attributes[1] = {1, graphics::VertexFormat::Float2, 12};
    pd.cull_mode = graphics::CullMode::None;
    Handle pipe = 0;
    CHECK(e.kernel.create_pipeline(kPid, e.dev, pd, &pipe) == status::kSuccess);
    return pipe;
  };
  const Handle rt = e.alloc(kTarget, Format::A8R8G8B8, 8, 8), vb = e.buffer(kAllocVertexBuffer, 64);
  const float tri[15] = {-1, -1, 0.5f, 0, 1, 3, -1, 0.5f, 2, 1, -1, 3, 0.5f, 0, -1};
  CHECK(e.write(vb, floats(tri, 15)));
  auto draw = [&](Handle pipe, Handle texture, const graphics::SamplerDesc& sampler) {
    CommandWriter w(e.allocator);
    w.begin_pass(0, kNoAllocation, kPassClearColor, {0, 0, 0, 0}, 1, 0);
    w.bind_pipeline(pipe);
    w.bind_vertex_buffer(1);
    w.bind_texture(0, 2, sampler);
    w.draw(3);
    w.end_pass();
    CHECK(e.render(w, {rt, vb, texture}) == status::kSuccess);
  };
  {
    const Handle texture = reserved(kAllocTexture, Format::A8R8G8B8, 256, 256, 1, graphics::TextureKind::Texture2D, 1);
    CHECK(texture != 0);
    CHECK(e.kernel.update_tile_mappings(kPid, texture, 0, heap, 6 * kTileBytes, 1) == status::kSuccess && e.kernel.update_tile_mappings(kPid, texture, 3, heap, 7 * kTileBytes, 1) == status::kSuccess);
    std::vector<uint32_t> red(kTileBytes / 4, 0xFF800000u);
    const Handle up = e.buffer(kAllocVertexBuffer, kTileBytes);
    CHECK(e.write(up, ConstByteView(reinterpret_cast<const uint8_t*>(red.data()), kTileBytes)));
    CommandWriter w(e.allocator);
    w.copy_tiles(0, 0, 0, 1, 1, 0);
    w.copy_tiles(0, 0, 3, 1, 1, 0);
    CHECK(e.render(w, {texture, up}) == status::kSuccess);
    draw(pipeline(dxbc_ps(ps_blob)), texture, graphics::SamplerDesc());
    unsigned mapped = 0, unmapped = 0;
    for (uint32_t y = 0; y < 8; ++y)
      for (uint32_t x = 0; x < 8; ++x) {
        const uint32_t c = e.pixel(rt, x, y);
        const bool in_mapped = (x < 4) == (y < 4);   // (tiles 0 and 3)
        if (in_mapped) mapped += close8(c >> 16 & 0xFF, 0x80, 2) && (c & 0xFFFF) == 0xFFFF && (c >> 24) == 0xFF;
        else unmapped += c == 0;   // (nothing read was mapped: zeros and three statuses of 0)
      }
    printf("residency feedback: %u of 32 mapped pixels, %u of 32 unmapped right\n", mapped, unmapped);
    CHECK(mapped == 32 && unmapped == 32);
  }
  // --- a lookup's own clamp: four levels of 32 x 32 down, each one colour; sampled at magnification, the clamp of 2 gives level 2
  {
    const Handle texture = e.alloc(kTexture, Format::A8R8G8B8, 32, 32, 4);
    const uint32_t colours[4] = {0xFF102030u, 0xFF405060u, 0xFF708090u, 0xFFA0B0C0u};
    for (uint32_t level = 0; level < 4; ++level) {
      const uint32_t n = 32u >> level;
      std::vector<uint32_t> pixels(n * n, colours[level]);
      CHECK(e.write(texture, ConstByteView(reinterpret_cast<const uint8_t*>(pixels.data()), pixels.size() * 4), kLockDiscard, level));
    }
    graphics::SamplerDesc sampler;
    sampler.mip_filter = graphics::MipFilter::Nearest;
    draw(pipeline(dxbc_ps(clamp_blob)), texture, sampler);
    CHECK(e.pixel(rt, 3, 3) == colours[2] && e.pixel(rt, 6, 1) == colours[2]);
  }
  // (a device without kFeature2TileResidency refuses these shaders: the TGSI text has no residency feedback)
  {
    HostAllocator allocator;
    NtStatus st = 0;
    UniquePointer<D3d9Program> program = compile_dxbc(allocator, ps_blob, &st);
    CHECK(program && st == status::kSuccess && program->uses_feedback() && (program->required_features2() & graphics::kFeature2TileResidency));
    String text(allocator);
    CHECK(program && program->emit_tgsi(&text) == status::kNotSupported);
    // (the loads of buffers and storage images with a status - ld_raw_s, ld_structured_s, ld_uav_typed_s of a typed buffer and of a texture: cs_tiled_loads - are taken too, and
    // need the same feature; d3d12test runs it on reserved resources)
    const std::vector<uint8_t> loads_blob = read_blob("cs_tiled_loads.dxbc");
    UniquePointer<D3d9Program> loads = compile_dxbc(allocator, loads_blob, &st);
    CHECK(loads && st == status::kSuccess && loads->uses_feedback() && (loads->required_features2() & graphics::kFeature2TileResidency));
    String loads_text(allocator);
    CHECK(loads && loads->emit_tgsi(&loads_text) == status::kNotSupported);
  }
}

// Typed views of buffers (BindTexelBuffer, Buffer<> and RWBuffer<>) on the CPU renderer: a compute shader reads R8G8B8A8_UNORM and R16G16_FLOAT views of t2 and t9, writes and reads
// back an R16G16B16A16_FLOAT view of u5 and adds to an R32_UINT view of u1 atomically; the same through descriptors of a root signature's table; and what is refused.
void test_texel_buffers_kernel() {
  Env e;
  auto blob_shader = [&](const char* name, graphics::ShaderStage stage, Handle* out) {
    const std::vector<uint8_t> code = read_blob(name);
    ShaderDesc sd;
    sd.kind = ShaderKind::Dxbc, sd.stage = stage, sd.code = ConstByteView(code.data(), code.size());
    return !code.empty() && e.kernel.create_shader(kPid, e.dev, sd, out) == status::kSuccess;
  };
  Handle cs = 0, pipe = 0;
  CHECK(blob_shader("cs_typed_buffer.dxbc", graphics::ShaderStage::Compute, &cs));
  PipelineDesc pd;
  pd.compute_shader = cs;
  CHECK(e.kernel.create_pipeline(kPid, e.dev, pd, &pipe) == status::kSuccess);
  const Handle colours = e.buffer(kAllocUnorderedAccess, 16), halves = e.buffer(kAllocUnorderedAccess, 20), counts = e.buffer(kAllocUnorderedAccess, 32),
               results = e.buffer(kAllocUnorderedAccess, 40);
  uint8_t colour_bytes[16];
  uint16_t half_words[10] = {};
  for (uint32_t i = 0; i < 4; ++i) {
    colour_bytes[i * 4] = static_cast<uint8_t>(i * 40), colour_bytes[i * 4 + 1] = 100, colour_bytes[i * 4 + 2] = 0, colour_bytes[i * 4 + 3] = 255;
    half_words[2 + i * 2] = graphics::float_to_half(0.5f * static_cast<float>(i)), half_words[2 + i * 2 + 1] = graphics::float_to_half(2.0f + static_cast<float>(i));   // (from element 1)
  }
  CHECK(e.write(colours, ConstByteView(colour_bytes, 16)) && e.write(halves, ConstByteView(reinterpret_cast<const uint8_t*>(half_words), 20)) && e.fill(counts, 0) && e.fill(results, 0));
  auto check = [&](const char* what) {
    LockResult lock;
    int wrong = 0;
    CHECK(e.kernel.lock(kPid, results, kLockReadOnly, 0, &lock) == status::kSuccess);
    float x[4] = {};
    for (uint32_t i = 0; i < 4; ++i) {
      ByteReader in(lock.data.subview(8 + i * 8, 8));   // (the view of u5 starts at element 1)
      const float c[4] = {graphics::half_to_float(in.le16()), graphics::half_to_float(in.le16()), graphics::half_to_float(in.le16()), graphics::half_to_float(in.le16())};
      const float want_x = graphics::half_to_float(graphics::float_to_half(static_cast<float>(i * 40) / 255.0f + 0.5f * static_cast<float>(i)));
      x[i] = want_x;
      wrong += c[0] != want_x || c[1] != graphics::half_to_float(graphics::float_to_half(200.0f / 255.0f)) || c[2] != 2.0f + static_cast<float>(i) || c[3] != 4.0f;
    }
    e.kernel.unlock(kPid, results);
    CHECK(e.kernel.lock(kPid, counts, kLockReadOnly, 0, &lock) == status::kSuccess);
    ByteReader in(lock.data);
    const uint32_t sum = in.le32(), most = in.le32();
    wrong += sum != 10 || most != 30;
    for (uint32_t i = 0; i < 4; ++i) wrong += in.le32() != static_cast<uint32_t>(x[i] * 100.0f + 0.5f) + 4000;
    e.kernel.unlock(kPid, counts);
    if (wrong) std::printf("  texel buffers (%s): %d wrong\n", what, wrong);
    CHECK(wrong == 0);
  };
  using graphics::TexelFormat;
  const uint32_t t2 = graphics::kTexelBufferSlotBase + 2, t9 = graphics::kTexelBufferSlotBase + 9;
  auto bind_all = [&](CommandWriter& w) {
    w.bind_pipeline(pipe);
    w.bind_texel_buffer(t2, 0, 0, 4, TexelFormat::R8G8B8A8Unorm);
    w.bind_texel_buffer(t9, 1, 1, 4, TexelFormat::R16G16Float);
    w.bind_texel_buffer(1, 2, 0, 8, TexelFormat::R32Uint);
    w.bind_texel_buffer(5, 3, 1, 4, TexelFormat::R16G16B16A16Float);
  };
  auto run = [&](auto build) {
    CommandWriter w(e.allocator);
    build(w);
    return w.ok() ? e.render(w, {colours, halves, counts, results}) : status::kNoMemory;
  };
  CHECK(run([&](CommandWriter& w) { bind_all(w); w.dispatch(1); }) == status::kSuccess);
  check("bound");

  // Refused: a slot left unbound, a view of another kind of values (floats where the shader reads integers), a format a u# view cannot store, elements past the end or a view that wraps,
  // a slot past the last, a format that is none, a buffer that shaders cannot use.
  CHECK(run([&](CommandWriter& w) { bind_all(w); w.bind_texel_buffer(t9, kNoAllocation, 0, 0, TexelFormat::Undefined); w.dispatch(1); }) == status::kInvalidParameter);
  CHECK(run([&](CommandWriter& w) { bind_all(w); w.bind_texel_buffer(1, 2, 0, 8, TexelFormat::R32Float); w.dispatch(1); }) == status::kInvalidParameter);
  CHECK(run([&](CommandWriter& w) { w.bind_texel_buffer(5, 3, 0, 3, TexelFormat::R32G32B32Float); }) == status::kInvalidParameter);
  CHECK(run([&](CommandWriter& w) { w.bind_texel_buffer(t2, 0, 1, 4, TexelFormat::R8G8B8A8Unorm); }) == status::kInvalidParameter);
  CHECK(run([&](CommandWriter& w) { w.bind_texel_buffer(t2, 0, 0xFFFFFFFFu, 2, TexelFormat::R8G8B8A8Unorm); }) == status::kInvalidParameter);
  CHECK(run([&](CommandWriter& w) { w.bind_texel_buffer(graphics::kMaxImageSlots, 0, 0, 1, TexelFormat::R8G8B8A8Unorm); }) == status::kInvalidParameter);
  CHECK(run([&](CommandWriter& w) { w.bind_texel_buffer(t2, 0, 0, 1, static_cast<TexelFormat>(1)); }) == status::kInvalidParameter);   // (R32G32B32A32_TYPELESS)
  const Handle vertices = e.buffer(kAllocVertexBuffer, 64);
  {
    CommandWriter w(e.allocator);
    w.bind_texel_buffer(t2, 0, 0, 4, TexelFormat::R8G8B8A8Unorm);
    CHECK(e.render(w, {vertices}) == status::kInvalidParameter);
  }

  // Through a root signature: a table of the four views (t2 and t9 typed shader resource views, u1 and u5 typed unordered access views of the same elements).
  CHECK(e.fill(counts, 0) && e.fill(results, 0));
  const std::vector<uint8_t> blob = serialize_signature(1, {{0, 0, {{0, 1, 2, 0, 0}, {0, 1, 9, 0, 1}, {1, 1, 1, 0, 2}, {1, 1, 5, 0, 3}}}});
  Handle sig = 0, views = 0;
  CHECK(e.kernel.create_root_signature(kPid, e.dev, ConstByteView(blob.data(), blob.size()), &sig) == status::kSuccess);
  CHECK(e.kernel.create_descriptor_heap(kPid, e.dev, DescriptorHeapType::CbvSrvUav, 4, &views) == status::kSuccess);
  auto view = [&](uint32_t index, DescriptorKind kind, Handle a, uint64_t offset, uint64_t size, TexelFormat f) {
    DescriptorDesc d;
    d.kind = kind, d.allocation = a, d.offset = offset, d.size = size, d.texel_format = static_cast<uint32_t>(f);
    return e.kernel.write_descriptor(kPid, views, index, d);
  };
  CHECK(view(0, DescriptorKind::Srv, colours, 0, 16, TexelFormat::R8G8B8A8Unorm) == status::kSuccess);
  CHECK(view(1, DescriptorKind::Srv, halves, 4, 16, TexelFormat::R16G16Float) == status::kSuccess);
  CHECK(view(2, DescriptorKind::Uav, counts, 0, 32, TexelFormat::R32Uint) == status::kSuccess);
  CHECK(view(3, DescriptorKind::Uav, results, 8, 32, TexelFormat::R16G16B16A16Float) == status::kSuccess);
  // (descriptors refused: an offset that is not a whole element, a format an unordered access view cannot store, a format that is none)
  CHECK(view(3, DescriptorKind::Uav, results, 4, 32, TexelFormat::R16G16B16A16Float) == status::kInvalidParameter);
  CHECK(view(3, DescriptorKind::Uav, results, 0, 36, TexelFormat::R32G32B32Float) == status::kInvalidParameter);
  CHECK(view(0, DescriptorKind::Srv, colours, 0, 16, static_cast<TexelFormat>(5)) == status::kInvalidParameter);
  CHECK(run([&](CommandWriter& w) {
          w.set_root_signature(sig);
          w.set_descriptor_heaps({views});
          w.set_root_table(0, 0);
          w.bind_pipeline(pipe);
          w.dispatch(1);
        }) == status::kSuccess);
  check("table");
  // (a raw view where the shader declares a typed one is refused)
  CHECK(view(0, DescriptorKind::Srv, colours, 0, 16, TexelFormat::Undefined) == status::kSuccess);
  CHECK(run([&](CommandWriter& w) {
          w.set_root_signature(sig);
          w.set_descriptor_heaps({views});
          w.set_root_table(0, 0);
          w.bind_pipeline(pipe);
          w.dispatch(1);
        }) == status::kInvalidParameter);

  // The decoding and encoding of every format, against values worked out by hand; and elements outside a view read as zero.
  {
    using graphics::decode_texel;
    using graphics::encode_texel;
    uint8_t b[16] = {};
    const graphics::Vec4 v = {0.25f, -0.5f, 1.5f, 1.0f};
    encode_texel(TexelFormat::R8G8B8A8Snorm, v, ByteView(b, 4));
    CHECK(b[0] == 32 && b[1] == 0xC0 && b[2] == 127 && b[3] == 127);   // (0.25 * 127 = 31.75 -> 32, -63.5 -> -64, 1.5 clamped)
    const graphics::Vec4 s = decode_texel(TexelFormat::R8G8B8A8Snorm, ConstByteView(b, 4));
    CHECK(s.x == 32.0f / 127.0f && s.y == -64.0f / 127.0f && s.z == 1.0f);
    const uint8_t lowest[1] = {0x80};
    CHECK(decode_texel(TexelFormat::R8Snorm, ConstByteView(lowest, 1)).x == -1.0f);   // (-128 reads as -1, as -127 does)
    encode_texel(TexelFormat::R11G11B10Float, {1.0f, 2.5f, -3.0f, 0}, ByteView(b, 4));
    const graphics::Vec4 f = decode_texel(TexelFormat::R11G11B10Float, ConstByteView(b, 4));
    CHECK(f.x == 1.0f && f.y == 2.5f && f.z == 0.0f && f.w == 1.0f);
    encode_texel(TexelFormat::R11G11B10Float, {1e10f, 65024.0f, 0.0f, 0}, ByteView(b, 4));   // (saturated to the largest finite value: 65024, and 64512 for the 10-bit one)
    const graphics::Vec4 big = decode_texel(TexelFormat::R11G11B10Float, ConstByteView(b, 4));
    CHECK(big.x == 65024.0f && big.y == 65024.0f);
    encode_texel(TexelFormat::R10G10B10A2Uint, {__builtin_bit_cast(float, 5000u), __builtin_bit_cast(float, 7u), __builtin_bit_cast(float, 1023u), __builtin_bit_cast(float, 9u)}, ByteView(b, 4));
    CHECK((b[0] | b[1] << 8 | b[2] << 16 | static_cast<uint32_t>(b[3]) << 24) == (1023u | 7u << 10 | 1023u << 20 | 3u << 30));   // (integers clamped to the channel)
    encode_texel(TexelFormat::R16Sint, {__builtin_bit_cast(float, static_cast<uint32_t>(-40000)), 0, 0, 0}, ByteView(b, 2));
    CHECK(b[0] == 0x00 && b[1] == 0x80);
    CHECK(__builtin_bit_cast(uint32_t, decode_texel(TexelFormat::R16Sint, ConstByteView(b, 2)).x) == static_cast<uint32_t>(-32768));
    CHECK(__builtin_bit_cast(uint32_t, decode_texel(TexelFormat::R16Sint, ConstByteView(b, 2)).w) == 1u);   // (the alpha of an integer format: the integer 1)
    encode_texel(TexelFormat::B5G6R5Unorm, {1.0f, 0.0f, 1.0f, 1.0f}, ByteView(b, 2));
    CHECK(b[0] == 0x1F && b[1] == 0xF8);
    encode_texel(TexelFormat::A8Unorm, {0, 0, 0, 0.5f}, ByteView(b, 1));
    CHECK(b[0] == 128 && decode_texel(TexelFormat::A8Unorm, ConstByteView(b, 1)).x == 0.0f);
    CHECK(decode_texel(TexelFormat::R32Float, ConstByteView(b, 2)).x == 0.0f);   // (a short element: nothing read)
  }
}

// Append, consume and counter views (the hidden counters: BindUav's slots from kCounterSlotBase, imm_atomic_alloc / imm_atomic_consume), bufinfo, and buffers in slots far above the
// first ones (u60, t100): directly, and through descriptors with counters.
void test_uav_counters_kernel() {
  Env e;
  const std::vector<uint8_t> code = read_blob("cs_append.dxbc");
  ShaderDesc sd;
  sd.kind = ShaderKind::Dxbc, sd.stage = graphics::ShaderStage::Compute, sd.code = ConstByteView(code.data(), code.size());
  Handle cs = 0, pipe = 0;
  CHECK(!code.empty() && e.kernel.create_shader(kPid, e.dev, sd, &cs) == status::kSuccess);
  PipelineDesc pd;
  pd.compute_shader = cs;
  CHECK(e.kernel.create_pipeline(kPid, e.dev, pd, &pipe) == status::kSuccess);
  const Handle appended = e.buffer(kAllocUnorderedAccess, 64), consumed = e.buffer(kAllocUnorderedAccess, 32), counted = e.buffer(kAllocUnorderedAccess, 32),
               wide = e.buffer(kAllocUnorderedAccess, 40), far = e.buffer(kAllocUnorderedAccess, 32), counters = e.buffer(kAllocUnorderedAccess, 16);
  auto words = [](std::initializer_list<uint32_t> list) {
    std::vector<uint8_t> out;
    for (uint32_t w : list)
      for (uint32_t k = 0; k < 4; ++k) out.push_back(static_cast<uint8_t>(w >> (8 * k)));
    return out;
  };
  auto read = [&](Handle a, uint32_t index) {
    LockResult lock;
    if (e.kernel.lock(kPid, a, kLockReadOnly, 0, &lock) != status::kSuccess) return 0xDEADDEADu;
    ByteReader in(lock.data);
    in.skip(uint64_t{index} * 4);
    const uint32_t v = in.le32();
    e.kernel.unlock(kPid, a);
    return v;
  };
  auto reset = [&]() {
    const std::vector<uint8_t> values = words({100, 101, 102, 103, 104, 105, 106, 107}), raw = words({1000, 1001, 1002, 1003, 1004, 1005, 1006, 1007}), start = words({0, 6, 0, 0});
    return e.fill(appended, 0) && e.fill(counted, 0) && e.fill(wide, 0) && e.write(consumed, ConstByteView(values.data(), values.size())) &&
           e.write(far, ConstByteView(raw.data(), raw.size())) && e.write(counters, ConstByteView(start.data(), start.size()));
  };
  auto check = [&](const char* what) {
    int wrong = 0;
    for (uint32_t k = 0; k < 6; ++k) wrong += read(appended, k * 2) != 105 - k || read(appended, k * 2 + 1) != k;   // (the threads in order: each took the last value left)
    for (uint32_t k = 0; k < 8; ++k) wrong += read(counted, k) != k * 3 + 1 || read(wide, k) != 1007 + k;
    wrong += read(wide, 8) != 32 || read(wide, 9) != 8;   // (bufinfo: the raw view's bytes, the structured view's structures)
    wrong += read(counters, 0) != 6 || read(counters, 1) != 0 || read(counters, 2) != 8;
    if (wrong) std::printf("  counters (%s): %d wrong\n", what, wrong);
    CHECK(wrong == 0);
  };
  const uint32_t c = graphics::kCounterSlotBase;
  auto bind_all = [&](CommandWriter& w) {
    w.bind_pipeline(pipe);
    w.bind_uav(0, 0, 0, 64), w.bind_uav(1, 1, 0, 32), w.bind_uav(2, 2, 0, 32), w.bind_uav(60, 3, 0, 40), w.bind_uav(graphics::kMaxUavBuffers + 100, 4, 0, 32);
    w.bind_uav(c + 0, 5, 0, 4), w.bind_uav(c + 1, 5, 4, 4), w.bind_uav(c + 2, 5, 8, 4);
  };
  auto run = [&](auto build) {
    CommandWriter w(e.allocator);
    build(w);
    return w.ok() ? e.render(w, {appended, consumed, counted, wide, far, counters}) : status::kNoMemory;
  };
  CHECK(reset());
  CHECK(run([&](CommandWriter& w) { bind_all(w); w.dispatch(1); }) == status::kSuccess);
  check("bound");
  // (a counter left unbound refuses the dispatch)
  {
    CommandWriter w(e.allocator);
    w.bind_pipeline(pipe);
    w.bind_uav(0, 0, 0, 64), w.bind_uav(1, 1, 0, 32), w.bind_uav(2, 2, 0, 32), w.bind_uav(60, 3, 0, 40), w.bind_uav(graphics::kMaxUavBuffers + 100, 4, 0, 32);
    w.bind_uav(c + 0, 5, 0, 4), w.bind_uav(c + 1, 5, 4, 4);
    w.dispatch(1);
    CHECK(e.render(w, {appended, consumed, counted, wide, far, counters}) == status::kInvalidParameter);
  }

  // Through a root signature: u0 .. u2 descriptors with counters (words of one buffer), u60 and t100 descriptors of their own ranges.
  CHECK(reset());
  const std::vector<uint8_t> blob = serialize_signature(1, {{0, 0, {{1, 3, 0, 0, 0}, {1, 1, 60, 0, 3}, {0, 1, 100, 0, 4}}}});
  Handle sig = 0, views = 0;
  CHECK(e.kernel.create_root_signature(kPid, e.dev, ConstByteView(blob.data(), blob.size()), &sig) == status::kSuccess);
  CHECK(e.kernel.create_descriptor_heap(kPid, e.dev, DescriptorHeapType::CbvSrvUav, 5, &views) == status::kSuccess);
  auto view = [&](uint32_t index, DescriptorKind kind, Handle a, uint64_t size, Handle counter, uint64_t counter_offset) {
    DescriptorDesc d;
    d.kind = kind, d.allocation = a, d.size = size, d.counter = counter, d.counter_offset = counter_offset;
    return e.kernel.write_descriptor(kPid, views, index, d);
  };
  CHECK(view(0, DescriptorKind::Uav, appended, 64, counters, 0) == status::kSuccess && view(1, DescriptorKind::Uav, consumed, 32, counters, 4) == status::kSuccess &&
        view(2, DescriptorKind::Uav, counted, 32, counters, 8) == status::kSuccess && view(3, DescriptorKind::Uav, wide, 40, 0, 0) == status::kSuccess &&
        view(4, DescriptorKind::Srv, far, 32, 0, 0) == status::kSuccess);
  // (counters refused: misaligned, past the end, of a shader resource view, of a buffer shaders cannot write)
  const Handle vertices = e.buffer(kAllocVertexBuffer, 64);
  CHECK(view(2, DescriptorKind::Uav, counted, 32, counters, 2) == status::kInvalidParameter);
  CHECK(view(2, DescriptorKind::Uav, counted, 32, counters, 16) == status::kInvalidParameter);
  CHECK(view(4, DescriptorKind::Srv, far, 32, counters, 0) == status::kInvalidParameter);
  CHECK(view(2, DescriptorKind::Uav, counted, 32, vertices, 0) == status::kInvalidParameter);
  CHECK(run([&](CommandWriter& w) {
          w.set_root_signature(sig);
          w.set_descriptor_heaps({views});
          w.set_root_table(0, 0);
          w.bind_pipeline(pipe);
          w.dispatch(1);
        }) == status::kSuccess);
  check("table");
  // (a view without a counter where the shader counts is refused)
  CHECK(view(2, DescriptorKind::Uav, counted, 32, 0, 0) == status::kSuccess);
  CHECK(run([&](CommandWriter& w) {
          w.set_root_signature(sig);
          w.set_descriptor_heaps({views});
          w.set_root_table(0, 0);
          w.bind_pipeline(pipe);
          w.dispatch(1);
        }) == status::kInvalidParameter);
}

// Unordered access views in a vertex shader (Direct3D 11.1's UAVs at every stage): it counts its vertices into a typed view and keeps their positions in a structured one.
void test_vertex_uav_kernel() {
  Env e;
  namespace b = conformance_blobs;
  const std::vector<uint8_t> code = read_blob("vs_uav.dxbc");
  Handle vs = 0, ps = 0, pipe = 0;
  ShaderDesc sd;
  sd.kind = ShaderKind::Dxbc, sd.stage = graphics::ShaderStage::Vertex, sd.code = ConstByteView(code.data(), code.size());
  CHECK(!code.empty() && e.kernel.create_shader(kPid, e.dev, sd, &vs) == status::kSuccess);
  const std::vector<uint8_t> pcode = read_blob("ps_color.dxbc");
  sd.stage = graphics::ShaderStage::Fragment, sd.code = ConstByteView(pcode.data(), pcode.size());
  CHECK(!pcode.empty() && e.kernel.create_shader(kPid, e.dev, sd, &ps) == status::kSuccess);
  (void)b::k_vs;
  PipelineDesc pd;
  pd.vertex_shader = vs, pd.fragment_shader = ps, pd.vertex_stride = 16, pd.attribute_count = 1, pd.varying_count = 1;
  pd.attributes[0] = {0, graphics::VertexFormat::Float4, 0};
  pd.cull_mode = graphics::CullMode::None;
  const NtStatus made = e.kernel.create_pipeline(kPid, e.dev, pd, &pipe);
  CHECK(made == status::kSuccess);
  if (made != status::kSuccess) return;
  const Handle target = e.alloc(kTarget, Format::A8R8G8B8, 8, 8), vb = e.buffer(kAllocVertexBuffer, 48), seen = e.buffer(kAllocUnorderedAccess, 16), positions = e.buffer(kAllocUnorderedAccess, 48);
  const float tri[12] = {-1, -1, 0.5f, 1, 3, -1, 0.5f, 1, -1, 3, 0.5f, 1};
  CHECK(e.write(vb, floats(tri, 12)) && e.fill(seen, 0) && e.fill(positions, 0));
  CommandWriter w(e.allocator);
  w.begin_pass(0, kNoAllocation, kPassClearColor, {0, 0, 0, 1}, 1, 0);
  w.bind_texel_buffer(1, 2, 0, 4, graphics::TexelFormat::R32Uint);
  w.bind_uav(2, 3, 0, 48);
  w.bind_pipeline(pipe);
  w.bind_vertex_buffer(1);
  w.draw(3);
  w.end_pass();
  CHECK(e.render(w, {target, vb, seen, positions}) == status::kSuccess);
  LockResult lock;
  CHECK(e.kernel.lock(kPid, seen, kLockReadOnly, 0, &lock) == status::kSuccess);
  CHECK(ByteReader(lock.data).le32() == 3);
  e.kernel.unlock(kPid, seen);
  CHECK(e.kernel.lock(kPid, positions, kLockReadOnly, 0, &lock) == status::kSuccess);
  ByteReader in(lock.data);
  int wrong = 0;
  for (float v : tri) wrong += __builtin_bit_cast(float, in.le32()) != v;
  e.kernel.unlock(kPid, positions);
  CHECK(wrong == 0);
  // The TGSI of the shader: its typed view is IMAGE[0] of a buffer with the marks the device fills in, the structured one BUFFER[0].
  UniquePointer<D3d9Program> program;
  CHECK(D3d9Program::compile_dxbc(e.allocator, ConstByteView(code.data(), code.size()), &program) == status::kSuccess);
  String text(e.allocator);
  graphics::TgsiBindings bindings;
  CHECK(program && program->emit_tgsi(&text, graphics::kMaxVaryings, &bindings) == status::kSuccess);
  CHECK(bindings.image_count == 1 && bindings.images[0] == 1 && bindings.storage_count == 1 && bindings.storage[0] == 2);
  CHECK(text.view().find("DCL IMAGE[0], BUFFER, IMGFMT0, WR") != StringView::npos && text.view().find("IMGOFS0") != StringView::npos && text.view().find("IMGLEN0") != StringView::npos);
}


// Views of buffers in a draw through a root signature: the root descriptor (t0) and a table entry (t1) reach the pixel shader, and the picture is the one a direct bind gives.
void test_d3d12_buffer_views_in_draw() {
  Env e;
  namespace b = conformance_blobs;
  Handle vs = 0, ps = 0, pipe = 0;
  ShaderDesc sd;
  sd.kind = ShaderKind::Dxbc, sd.stage = graphics::ShaderStage::Vertex, sd.code = ConstByteView(b::k_vs, sizeof b::k_vs);
  CHECK(e.kernel.create_shader(kPid, e.dev, sd, &vs) == status::kSuccess);
  sd.stage = graphics::ShaderStage::Fragment, sd.code = ConstByteView(b::k_dxil_ps_srv, sizeof b::k_dxil_ps_srv);
  CHECK(e.kernel.create_shader(kPid, e.dev, sd, &ps) == status::kSuccess);
  PipelineDesc pd;
  pd.vertex_shader = vs, pd.fragment_shader = ps, pd.vertex_stride = 28, pd.attribute_count = 2, pd.varying_count = 3;
  pd.attributes[0] = {0, graphics::VertexFormat::Float3, 0};
  pd.attributes[1] = {1, graphics::VertexFormat::Float4, 12};
  pd.cull_mode = graphics::CullMode::None;
  CHECK(e.kernel.create_pipeline(kPid, e.dev, pd, &pipe) == status::kSuccess);
  Handle target = e.alloc(kTarget, Format::A8R8G8B8, 8, 8), vb = e.buffer(kAllocVertexBuffer, 3 * 28);
  const float tri[21] = {-1, -1, 0.5f, 0.9f, 0.3f, 0.2f, 1, 3, -1, 0.5f, 0.1f, 0.8f, 0.4f, 1, -1, 3, 0.5f, 0.4f, 0.2f, 0.9f, 1};
  CHECK(e.write(vb, floats(tri, 21)));
  Handle structured = e.buffer(kAllocUnorderedAccess, 32 * 16), raw = e.buffer(kAllocUnorderedAccess, 64);
  std::vector<uint32_t> a(32 * 4), r(16);
  for (uint32_t i = 0; i < a.size(); ++i) a[i] = 0x9E3779B9u * (i + 1);
  for (uint32_t i = 0; i < r.size(); ++i) r[i] = 0x85EBCA6Bu * (i + 3);
  CHECK(e.write(structured, ConstByteView(reinterpret_cast<const uint8_t*>(a.data()), 512)) && e.write(raw, ConstByteView(reinterpret_cast<const uint8_t*>(r.data()), 64)));
  const float identity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
  // direct
  {
    CommandWriter w(e.allocator);
    w.bind_uav(graphics::kMaxUavBuffers, 2, 0, 512);
    w.bind_uav(graphics::kMaxUavBuffers + 1, 3, 0, 64);
    w.begin_pass(0, kNoAllocation, kPassClearColor, {0, 0, 0, 1}, 1, 0);
    w.bind_pipeline(pipe);
    w.bind_vertex_buffer(1);
    w.push_constants(floats(identity, 16));
    w.draw(3);
    w.end_pass();
    CHECK(e.render(w, {target, vb, structured, raw}) == status::kSuccess);
  }
  std::vector<uint32_t> direct;
  for (uint32_t y = 0; y < 8; ++y)
    for (uint32_t x = 0; x < 8; ++x) direct.push_back(e.pixel(target, x, y));
  CHECK(direct[4 * 8 + 4] != 0);
  // through a root signature: t0 a root descriptor, t1 a table entry
  const std::vector<uint8_t> blob = serialize_signature(1, {{3, 0, {}, 0, 0, 0}, {0, 0, {{0, 1, 1, 0, 0}}}});
  Handle sig = 0, views = 0;
  CHECK(e.kernel.create_root_signature(kPid, e.dev, ConstByteView(blob.data(), blob.size()), &sig) == status::kSuccess);
  CHECK(e.kernel.create_descriptor_heap(kPid, e.dev, DescriptorHeapType::CbvSrvUav, 1, &views) == status::kSuccess);
  DescriptorDesc srv;
  srv.kind = DescriptorKind::Srv, srv.allocation = raw;
  CHECK(e.kernel.write_descriptor(kPid, views, 0, srv) == status::kSuccess);
  CommandWriter w(e.allocator);
  w.set_root_signature(sig);
  w.set_descriptor_heaps({views});
  w.set_root_descriptor(0, 2, 0);
  w.set_root_table(1, 0);
  w.begin_pass(0, kNoAllocation, kPassClearColor, {0, 0, 0, 1}, 1, 0);
  w.bind_pipeline(pipe);
  w.bind_vertex_buffer(1);
  w.push_constants(floats(identity, 16));
  w.draw(3);
  w.end_pass();
  CHECK(e.render(w, {target, vb, structured, raw}) == status::kSuccess);
  bool same = true;
  for (uint32_t y = 0; y < 8; ++y)
    for (uint32_t x = 0; x < 8; ++x) same = same && e.pixel(target, x, y) == direct[y * 8 + x];
  CHECK(same);
  // A draw whose shader reads a view nobody bound is refused.
  CommandWriter bad(e.allocator);
  bad.begin_pass(0, kNoAllocation, kPassClearColor, {0, 0, 0, 1}, 1, 0);
  bad.bind_pipeline(pipe);
  bad.bind_vertex_buffer(1);
  bad.push_constants(floats(identity, 16));
  bad.draw(3);
  bad.end_pass();
  CHECK(e.render(bad, {target, vb}) == status::kInvalidParameter);
}


// Pixel shaders that write buffers (RWStructuredBuffer, RWByteAddressBuffer with atomics): every pixel writes its place, and the draw leaves the counters at the number of pixels.
void test_pixel_shader_stores() {
  Env e;
  namespace b = conformance_blobs;
  Handle vs = 0, ps = 0, pipe = 0;
  ShaderDesc sd;
  sd.kind = ShaderKind::Dxbc, sd.stage = graphics::ShaderStage::Vertex, sd.code = ConstByteView(b::k_vs, sizeof b::k_vs);
  CHECK(e.kernel.create_shader(kPid, e.dev, sd, &vs) == status::kSuccess);
  sd.stage = graphics::ShaderStage::Fragment, sd.code = ConstByteView(b::k_dxil_ps_uav, sizeof b::k_dxil_ps_uav);
  CHECK(e.kernel.create_shader(kPid, e.dev, sd, &ps) == status::kSuccess);
  PipelineDesc pd;
  pd.vertex_shader = vs, pd.fragment_shader = ps, pd.vertex_stride = 28, pd.attribute_count = 2, pd.varying_count = 3;
  pd.attributes[0] = {0, graphics::VertexFormat::Float3, 0};
  pd.attributes[1] = {1, graphics::VertexFormat::Float4, 12};
  pd.cull_mode = graphics::CullMode::None;
  CHECK(e.kernel.create_pipeline(kPid, e.dev, pd, &pipe) == status::kSuccess);
  Handle target = e.alloc(kTarget, Format::A8R8G8B8, 32, 32), vb = e.buffer(kAllocVertexBuffer, 3 * 28);
  const float tri[21] = {-1, -1, 0.5f, 0.9f, 0.3f, 0.2f, 1, 3, -1, 0.5f, 0.1f, 0.8f, 0.4f, 1, -1, 3, 0.5f, 0.4f, 0.2f, 0.9f, 1};
  CHECK(e.write(vb, floats(tri, 21)));
  Handle picture = e.buffer(kAllocUnorderedAccess, 1024 * 16), counters = e.buffer(kAllocUnorderedAccess, 16);
  const float identity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
  CommandWriter w(e.allocator);
  w.bind_uav(0, 2, 0, 1024 * 16);
  w.bind_uav(1, 3, 0, 16);
  w.begin_pass(0, kNoAllocation, kPassClearColor, {0, 0, 0, 1}, 1, 0);
  w.bind_pipeline(pipe);
  w.bind_vertex_buffer(1);
  w.push_constants(floats(identity, 16));
  w.draw(3);
  w.end_pass();
  CHECK(e.render(w, {target, vb, picture, counters}) == status::kSuccess);
  LockResult lock;
  CHECK(e.kernel.lock(kPid, picture, kLockReadOnly, 0, &lock) == status::kSuccess);
  ByteReader in(lock.data);
  bool placed = true;
  for (uint32_t i = 0; i < 1024; ++i) {
    const uint32_t x = in.le32(), y = in.le32();
    in.le32();
    const uint32_t last = in.le32();
    placed = placed && x == i % 32 && y == i / 32 && last == i * 7 + 1;
  }
  CHECK(placed);
  e.kernel.unlock(kPid, picture);
  CHECK(e.kernel.lock(kPid, counters, kLockReadOnly, 0, &lock) == status::kSuccess);
  ByteReader counted(lock.data);
  CHECK(counted.le32() == 1024 && counted.le32() == 1536);
  e.kernel.unlock(kPid, counters);
  CHECK(e.pixel(target, 5, 5) != 0);
  // With a depth test that every fragment fails the shader still runs (its writes are not the depth test's to stop), and nothing is drawn.
  Handle depth = e.alloc(kAllocDepthStencil, Format::D32Float, 32, 32), blocked = 0;
  PipelineDesc dpd = pd;
  dpd.depth_test = dpd.depth_write = true;
  CHECK(e.kernel.create_pipeline(kPid, e.dev, dpd, &blocked) == status::kSuccess);
  CommandWriter w2(e.allocator);
  w2.bind_uav(0, 3, 0, 1024 * 16);
  w2.bind_uav(1, 4, 0, 16);
  w2.begin_pass(0, 2, kPassClearColor | kPassClearDepth, {0, 0, 0, 1}, 0.0f, 0);
  w2.bind_pipeline(blocked);
  w2.bind_vertex_buffer(1);
  w2.push_constants(floats(identity, 16));
  w2.draw(3);
  w2.end_pass();
  CHECK(e.render(w2, {target, vb, depth, picture, counters}) == status::kSuccess);
  CHECK(e.pixel(target, 5, 5) == 0xFF000000u);
  CHECK(e.kernel.lock(kPid, counters, kLockReadOnly, 0, &lock) == status::kSuccess);
  ByteReader again(lock.data);
  CHECK(again.le32() == 2048 && again.le32() == 3072);
  e.kernel.unlock(kPid, counters);
}



// Vertex shaders that write buffers: each vertex logs itself at its number and bumps a counter.
void test_vertex_shader_stores() {
  Env e;
  namespace b = conformance_blobs;
  Handle vs = 0, ps = 0, pipe = 0;
  ShaderDesc sd;
  sd.kind = ShaderKind::Dxbc, sd.stage = graphics::ShaderStage::Vertex, sd.code = ConstByteView(b::k_dxil_vs_uav, sizeof b::k_dxil_vs_uav);
  CHECK(e.kernel.create_shader(kPid, e.dev, sd, &vs) == status::kSuccess);
  sd.stage = graphics::ShaderStage::Fragment, sd.code = ConstByteView(b::k_ps_color, sizeof b::k_ps_color);
  CHECK(e.kernel.create_shader(kPid, e.dev, sd, &ps) == status::kSuccess);
  PipelineDesc pd;
  pd.vertex_shader = vs, pd.fragment_shader = ps, pd.vertex_stride = 28, pd.attribute_count = 2, pd.varying_count = 3;
  pd.attributes[0] = {0, graphics::VertexFormat::Float3, 0};
  pd.attributes[1] = {1, graphics::VertexFormat::Float4, 12};
  pd.cull_mode = graphics::CullMode::None;
  CHECK(e.kernel.create_pipeline(kPid, e.dev, pd, &pipe) == status::kSuccess);
  Handle target = e.alloc(kTarget, Format::A8R8G8B8, 8, 8), vb = e.buffer(kAllocVertexBuffer, 3 * 28);
  const float tri[21] = {-0.5f, -0.25f, 0.5f, 0.9f, 0.3f, 0.2f, 1, 0.75f, -0.5f, 0.5f, 0.1f, 0.8f, 0.4f, 1, -0.125f, 0.5f, 0.5f, 0.4f, 0.2f, 0.9f, 1};
  CHECK(e.write(vb, floats(tri, 21)));
  Handle log = e.buffer(kAllocUnorderedAccess, 3 * 16), counter = e.buffer(kAllocUnorderedAccess, 16);
  const float identity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
  CommandWriter w(e.allocator);
  w.bind_uav(0, 2, 0, 3 * 16);
  w.bind_uav(1, 3, 0, 16);
  w.begin_pass(0, kNoAllocation, kPassClearColor, {0, 0, 0, 1}, 1, 0);
  w.bind_pipeline(pipe);
  w.bind_vertex_buffer(1);
  w.push_constants(floats(identity, 16));
  w.draw(3);
  w.end_pass();
  CHECK(e.render(w, {target, vb, log, counter}) == status::kSuccess);
  LockResult lock;
  CHECK(e.kernel.lock(kPid, log, kLockReadOnly, 0, &lock) == status::kSuccess);
  ByteReader in(lock.data);
  bool same = true;
  for (uint32_t v = 0; v < 3; ++v) {
    const float x = __builtin_bit_cast(float, in.le32()), y = __builtin_bit_cast(float, in.le32()), z = __builtin_bit_cast(float, in.le32()), id = __builtin_bit_cast(float, in.le32());
    same = same && x == tri[7 * v] && y == tri[7 * v + 1] && z == tri[7 * v + 2] && id == static_cast<float>(v);
  }
  CHECK(same);
  e.kernel.unlock(kPid, log);
  CHECK(e.kernel.lock(kPid, counter, kLockReadOnly, 0, &lock) == status::kSuccess);
  CHECK(ByteReader(lock.data).le32() == 3);
  e.kernel.unlock(kPid, counter);
  CHECK(e.pixel(target, 4, 4) != 0);
}


// Geometry shaders that write buffers: each triangle logs its first two corners at the place its colour names and bumps a counter.
void test_geometry_shader_stores() {
  Env e;
  namespace b = conformance_blobs;
  Handle vs = 0, gs = 0, ps = 0, pipe = 0;
  ShaderDesc sd;
  sd.kind = ShaderKind::Dxbc, sd.stage = graphics::ShaderStage::Vertex, sd.code = ConstByteView(b::k_vs, sizeof b::k_vs);
  CHECK(e.kernel.create_shader(kPid, e.dev, sd, &vs) == status::kSuccess);
  sd.stage = graphics::ShaderStage::Geometry, sd.code = ConstByteView(b::k_dxil_gs_uav, sizeof b::k_dxil_gs_uav);
  CHECK(e.kernel.create_shader(kPid, e.dev, sd, &gs) == status::kSuccess);
  sd.stage = graphics::ShaderStage::Fragment, sd.code = ConstByteView(b::k_ps_color, sizeof b::k_ps_color);
  CHECK(e.kernel.create_shader(kPid, e.dev, sd, &ps) == status::kSuccess);
  PipelineDesc pd;
  pd.vertex_shader = vs, pd.geometry_shader = gs, pd.fragment_shader = ps, pd.vertex_stride = 28, pd.attribute_count = 2, pd.varying_count = 3;
  pd.attributes[0] = {0, graphics::VertexFormat::Float3, 0};
  pd.attributes[1] = {1, graphics::VertexFormat::Float4, 12};
  pd.cull_mode = graphics::CullMode::None;
  CHECK(e.kernel.create_pipeline(kPid, e.dev, pd, &pipe) == status::kSuccess);
  Handle target = e.alloc(kTarget, Format::A8R8G8B8, 8, 8), vb = e.buffer(kAllocVertexBuffer, 6 * 28);
  // two triangles, the first with colour.x 3/255 and the second 7/255
  float v[42];
  const float corners[2][3][2] = {{{-0.5f, -0.5f}, {0.5f, -0.5f}, {0, 0.5f}}, {{-0.25f, -0.25f}, {0.25f, -0.25f}, {0, 0.25f}}};
  for (uint32_t t = 0; t < 2; ++t)
    for (uint32_t k = 0; k < 3; ++k) {
      float* o = v + 7 * (3 * t + k);
      o[0] = corners[t][k][0], o[1] = corners[t][k][1], o[2] = 0.5f, o[3] = (t == 0 ? 3.0f : 7.0f) / 255.0f, o[4] = 0.5f, o[5] = 0.5f, o[6] = 1;
    }
  CHECK(e.write(vb, floats(v, 42)));
  Handle log = e.buffer(kAllocUnorderedAccess, 16 * 16), counter = e.buffer(kAllocUnorderedAccess, 16);
  const float identity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
  CommandWriter w(e.allocator);
  w.bind_uav(0, 2, 0, 16 * 16);
  w.bind_uav(1, 3, 0, 16);
  w.begin_pass(0, kNoAllocation, kPassClearColor, {0, 0, 0, 1}, 1, 0);
  w.bind_pipeline(pipe);
  w.bind_vertex_buffer(1);
  w.push_constants(floats(identity, 16));
  w.draw(6);
  w.end_pass();
  CHECK(e.render(w, {target, vb, log, counter}) == status::kSuccess);
  LockResult lock;
  CHECK(e.kernel.lock(kPid, log, kLockReadOnly, 0, &lock) == status::kSuccess);
  float words[64];
  ByteReader in(lock.data);
  for (float& f : words) f = __builtin_bit_cast(float, in.le32());
  e.kernel.unlock(kPid, log);
  auto near = [](float a, float b) { return __builtin_fabsf(a - b) < 1e-4f; };
  // (the geometry shader sees positions with Y as the devices want it, down: the sign of y is turned back for the shader; the logged y is the vertex shader's)
  CHECK(near(words[4 * 3], corners[0][0][0]) && near(words[4 * 3 + 1], corners[0][0][1]) && near(words[4 * 3 + 2], corners[0][1][0]) && near(words[4 * 3 + 3], corners[0][1][1]));
  CHECK(near(words[4 * 7], corners[1][0][0]) && near(words[4 * 7 + 1], corners[1][0][1]) && near(words[4 * 7 + 2], corners[1][1][0]) && near(words[4 * 7 + 3], corners[1][1][1]));
  CHECK(e.kernel.lock(kPid, counter, kLockReadOnly, 0, &lock) == status::kSuccess);
  CHECK(ByteReader(lock.data).le32() == 2);
  e.kernel.unlock(kPid, counter);
  CHECK(e.pixel(target, 4, 4) != 0);
}


// Hull and domain shaders that write buffers: the control point function logs each output control point, the patch constant function counts the patch, and the domain shader counts the
// points of the tessellation (a factor of 3 makes 12 points of a triangle, each shaded once) and sums a coordinate.
void test_tessellation_shader_stores() {
  Env e;
  namespace b = conformance_blobs;
  Handle vs = 0, hs = 0, ds = 0, ps = 0, pipe = 0;
  ShaderDesc sd;
  sd.kind = ShaderKind::Dxbc, sd.stage = graphics::ShaderStage::Vertex, sd.code = ConstByteView(b::k_vs, sizeof b::k_vs);
  CHECK(e.kernel.create_shader(kPid, e.dev, sd, &vs) == status::kSuccess);
  sd.stage = graphics::ShaderStage::Hull, sd.code = ConstByteView(b::k_dxil_hs_uav, sizeof b::k_dxil_hs_uav);
  CHECK(e.kernel.create_shader(kPid, e.dev, sd, &hs) == status::kSuccess);
  sd.stage = graphics::ShaderStage::Domain, sd.code = ConstByteView(b::k_dxil_ds_uav, sizeof b::k_dxil_ds_uav);
  CHECK(e.kernel.create_shader(kPid, e.dev, sd, &ds) == status::kSuccess);
  sd.stage = graphics::ShaderStage::Fragment, sd.code = ConstByteView(b::k_ps_color, sizeof b::k_ps_color);
  CHECK(e.kernel.create_shader(kPid, e.dev, sd, &ps) == status::kSuccess);
  PipelineDesc pd;
  pd.vertex_shader = vs, pd.hull_shader = hs, pd.domain_shader = ds, pd.fragment_shader = ps, pd.vertex_stride = 28, pd.attribute_count = 2, pd.varying_count = 3;
  pd.attributes[0] = {0, graphics::VertexFormat::Float3, 0};
  pd.attributes[1] = {1, graphics::VertexFormat::Float4, 12};
  pd.cull_mode = graphics::CullMode::None;
  CHECK(e.kernel.create_pipeline(kPid, e.dev, pd, &pipe) == status::kSuccess);
  Handle target = e.alloc(kTarget, Format::A8R8G8B8, 8, 8), vb = e.buffer(kAllocVertexBuffer, 3 * 28);
  const float tri[21] = {-0.75f, -0.5f, 0.5f, 0.9f, 0.3f, 0.2f, 1, 0.75f, -0.5f, 0.5f, 0.1f, 0.8f, 0.4f, 1, 0.25f, 0.75f, 0.5f, 0.4f, 0.2f, 0.9f, 1};
  CHECK(e.write(vb, floats(tri, 21)));
  Handle log = e.buffer(kAllocUnorderedAccess, 3 * 16), patches = e.buffer(kAllocUnorderedAccess, 16), points = e.buffer(kAllocUnorderedAccess, 16), sums = e.buffer(kAllocUnorderedAccess, 16);
  const float identity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
  CommandWriter w(e.allocator);
  w.bind_uav(0, 2, 0, 3 * 16);
  w.bind_uav(1, 3, 0, 16);
  w.bind_uav(2, 4, 0, 16);
  w.bind_uav(3, 5, 0, 16);
  w.begin_pass(0, kNoAllocation, kPassClearColor, {0, 0, 0, 1}, 1, 0);
  w.bind_pipeline(pipe);
  w.bind_vertex_buffer(1);
  w.push_constants(floats(identity, 16));
  w.set_topology(patch_list(3));
  w.draw(3);
  w.end_pass();
  CHECK(e.render(w, {target, vb, log, patches, points, sums}) == status::kSuccess);
  LockResult lock;
  auto word = [&](Handle buffer, uint32_t index) {
    uint32_t v = 0;
    if (e.kernel.lock(kPid, buffer, kLockReadOnly, 0, &lock) == status::kSuccess) {
      ByteReader in(lock.data);
      for (uint32_t i = 0; i <= index; ++i) v = in.le32();
      e.kernel.unlock(kPid, buffer);
    }
    return v;
  };
  CHECK(word(patches, 0) == 1);   // (one patch)
  CHECK(word(points, 0) == 12);   // (the points of a triangle of factor 3: 9 on the edges and a ring of 3 inside, each shaded once)
  CHECK(word(sums, 0) == 4000);   // (the points are symmetric: the mean of the second coordinate is 1/3, and 12 points of round(1000 * v) add up to 4000)
  float logged[12];
  CHECK(e.kernel.lock(kPid, log, kLockReadOnly, 0, &lock) == status::kSuccess);
  ByteReader in(lock.data);
  for (float& f : logged) f = __builtin_bit_cast(float, in.le32());
  e.kernel.unlock(kPid, log);
  bool same = true;
  for (uint32_t i = 0; i < 3; ++i) same = same && __builtin_fabsf(logged[4 * i] - tri[7 * i]) < 1e-4f && __builtin_fabsf(logged[4 * i + 1] - tri[7 * i + 1]) < 1e-4f && logged[4 * i + 2] == static_cast<float>(i);
  CHECK(same);
  CHECK(e.pixel(target, 4, 4) != 0);
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
// Mirrors (VidMm::set_mirror): an array of two 4 x 2 planes and an integer image of one, kept up to date by refresh_mirror; one that writes back; the checks of set_mirror; the
// escape that makes one; and an array of a wide format the software device samples (layers of Vec4s).
void test_mirrors() {
  Env e;
  VidMm& mm = e.kernel.adapter(0)->vidmm();
  auto make = [&](uint32_t flags, Format f, uint32_t w, uint32_t h, graphics::TextureKind kind, uint32_t layers) {
    AllocationDesc d;
    d.flags = flags, d.format = f, d.width = w, d.height = h, d.kind = kind, d.layers = layers;
    SharedPointer<Allocation> a;
    CHECK(mm.create(d, &a) == status::kSuccess);
    return a;
  };
  using graphics::TextureKind;
  SharedPointer<Allocation> y0 = make(kAllocTexture | kAllocRenderTarget, Format::A8B8G8R8, 4, 2, TextureKind::Texture2D, 1), y1 = make(kAllocTexture, Format::A8B8G8R8, 4, 2, TextureKind::Texture2D, 1);
  SharedPointer<Allocation> array = make(kAllocTexture, Format::A8B8G8R8, 4, 2, TextureKind::Texture2DArray, 2), integer = make(kAllocTexture | kAllocRenderTarget, Format::A8B8G8R8Uint, 4, 2, TextureKind::Texture2D, 1);
  if (!y0 || !y1 || !array || !integer) return;
  auto fill = [&](Allocation& a, uint8_t base) {
    ByteView b = a.writable_backing();
    for (uint64_t i = 0; i < b.size(); ++i) b[i] = static_cast<uint8_t>((i % 4) == 3 ? 255 : (i % 4) == 0 ? base + i / 4 : 0);   // (r, 0, 0, 255): a widened R8 plane
    mm.note_cpu_write(a);
  };
  fill(*y0, 10);
  fill(*y1, 100);
  // The checks: sources of the mirror's size and channel width, as many as its layers; a 2D one has one; a mirror is not a source; write-back is for one render target.
  const SharedPointer<Allocation> both[2] = {y0, y1};
  CHECK(mm.set_mirror(*array, ArrayView<const SharedPointer<Allocation>>(both, 1), 0, 0) == status::kInvalidParameter);
  CHECK(mm.set_mirror(*array, ArrayView<const SharedPointer<Allocation>>(both, 2), kMirrorWriteBack, 1) == status::kInvalidParameter);
  CHECK(mm.set_mirror(*array, ArrayView<const SharedPointer<Allocation>>(both, 2), 4, 0) == status::kInvalidParameter);
  SharedPointer<Allocation> wrong_size = make(kAllocTexture, Format::A8B8G8R8, 2, 2, TextureKind::Texture2D, 1), wide = make(kAllocTexture, Format::A16B16G16R16, 4, 2, TextureKind::Texture2D, 1);
  const SharedPointer<Allocation> mixed[2] = {y0, wrong_size}, widths[2] = {y0, wide};
  CHECK(mm.set_mirror(*array, ArrayView<const SharedPointer<Allocation>>(mixed, 2), 0, 0) == status::kInvalidParameter);
  CHECK(mm.set_mirror(*array, ArrayView<const SharedPointer<Allocation>>(widths, 2), 0, 0) == status::kInvalidParameter);
  CHECK(mm.set_mirror(*array, ArrayView<const SharedPointer<Allocation>>(both, 2), 0, 0) == status::kSuccess && array->is_mirror() && !array->writes_back());
  const SharedPointer<Allocation> self[1] = {array};
  CHECK(mm.set_mirror(*integer, ArrayView<const SharedPointer<Allocation>>(self, 1), 0, 0) == status::kInvalidParameter);   // (a mirror is no source)
  // The array: each layer its source's bytes, copied again only when the source changed.
  CHECK(mm.refresh_mirror(*array, true));
  ConstByteView got = array->backing();
  CHECK(got.size() == 2 * 4 * 2 * 4 && got[0] == 10 && got[4] == 11 && got[32] == 100 && got[3] == 255);
  const uint64_t version = array->version();
  CHECK(mm.refresh_mirror(*array, true) && array->version() == version && !mm.mirror_needs_read_back(*array));
  fill(*y1, 50);
  CHECK(mm.refresh_mirror(*array, true) && array->version() != version && array->backing()[32] == 50 && array->backing()[0] == 10);
  // A source the GPU drew into: not copied while it cannot be read back (the mirror keeps what it had), then read back and copied.
  mm.note_gpu_write(*y1);
  bool stale = false;
  CHECK(mm.mirror_needs_read_back(*array) && mm.refresh_mirror(*array, false, &stale) && stale && array->backing()[32] == 50);
  CHECK(mm.refresh_mirror(*array, true, &stale) && !stale && !mm.mirror_needs_read_back(*array));
  // The integer image: the same bytes, the alpha the integer 1; written back, its red goes into the plane and the rest of the plane stays.
  const SharedPointer<Allocation> one[1] = {y0};
  CHECK(mm.set_mirror(*integer, ArrayView<const SharedPointer<Allocation>>(one, 1), kMirrorInteger | kMirrorWriteBack, 0) == status::kInvalidParameter);   // (no channels to give back)
  CHECK(mm.set_mirror(*integer, ArrayView<const SharedPointer<Allocation>>(one, 1), kMirrorInteger | kMirrorWriteBack, 1) == status::kSuccess && integer->writes_back());
  CHECK(mm.refresh_mirror(*integer, true));
  got = integer->backing();
  CHECK(got[0] == 10 && got[1] == 0 && got[3] == 1 && got[7] == 1);
  ByteView drawn = integer->writable_backing();
  for (uint64_t i = 0; i < drawn.size(); i += 4) drawn[i] = 77, drawn[i + 1] = 88;   // (what a pass drew: red 77, green 88 - green is not the view's)
  mm.note_cpu_write(*integer);
  CHECK(mm.write_back_mirror(*integer));
  got = y0->backing();
  CHECK(got[0] == 77 && got[1] == 0 && got[3] == 255 && got[4] == 77);
  CHECK(!mm.write_back_mirror(*array));   // (only one that writes back)
  CHECK(mm.set_mirror(*array, ArrayView<const SharedPointer<Allocation>>(), 0, 0) == status::kSuccess && !array->is_mirror());
  // The escape (EscapeOp::SetMirror) with the process's handles: a mirror array of two planes, and handles that are not the process's.
  Handle p0 = e.alloc(kAllocTexture, Format::A8B8G8R8, 4, 2), p1 = e.alloc(kAllocTexture, Format::A8B8G8R8, 4, 2), m = 0;
  AllocationDesc md;
  md.flags = kAllocTexture, md.format = Format::A8B8G8R8, md.width = 4, md.height = 2, md.kind = TextureKind::Texture2DArray, md.layers = 2;
  CHECK(e.kernel.create_allocation(kPid, e.dev, md, &m) == status::kSuccess);
  auto escape = [&](std::initializer_list<uint32_t> words) {
    uint8_t request[64] = {}, reply[16] = {};
    ByteWriter w(ByteView(request, sizeof request));
    for (uint32_t v : words) w.le32(v);
    uint32_t reply_bytes = 0;
    return e.kernel.escape(kPid, e.dev, ConstByteView(request, words.size() * 4), ByteView(reply, sizeof reply), &reply_bytes);
  };
  CHECK(escape({static_cast<uint32_t>(EscapeOp::SetMirror), m, 0, 0, 2, p0, p1}) == status::kSuccess);
  CHECK(escape({static_cast<uint32_t>(EscapeOp::SetMirror), m, 0, 0, 2, p0, 0x7777}) == status::kInvalidHandle);
  CHECK(escape({static_cast<uint32_t>(EscapeOp::SetMirror), 0x7777, 0, 0, 1, p0}) == status::kInvalidHandle);
  CHECK(escape({static_cast<uint32_t>(EscapeOp::SetMirror), m, 0, 0, 2, p0}) == status::kInvalidParameter);   // (fewer words than the count says)
  // An array of a wide format: made, filled layer by layer, sampled by layer.
  SharedPointer<Allocation> wide_array = make(kAllocTexture, Format::A16B16G16R16, 4, 2, TextureKind::Texture2DArray, 3);
  if (wide_array) {
    ByteView b = wide_array->writable_backing();
    for (uint64_t i = 0; i + 1 < b.size(); i += 2) b[i + 1] = static_cast<uint8_t>(i / 64 * 64 + 32);   // (layer l: every channel's high byte 64 l + 32)
    mm.note_cpu_write(*wide_array);
    CHECK(mm.make_resident(*wide_array) == status::kSuccess && wide_array->image() && wide_array->image()->layer_count() == 3);
  }
}

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

// gather4_po: the offset of the footprint in a register. The shader is assembled here (the compilers in reach do not make one): r1 = ftoi(v0.xy * 16 - 4); o0 = the red of the four texels
// of the footprint moved by r1. The same bytes are tests/data/dxbc/gather_po.dxbc, which the conformance scenes carry (GEN_GATHER_PO=<path> writes it).
std::vector<uint8_t> gather_po_shader() {
  DxbcAsm a = dx_pixel_shader();
  a.version_major = 4;
  a.constant_buffer = 0;
  a.ins(88, {dx_resource(0)}, 3u << 11);   // dcl_resource_texture2d t0
  a.ins(56, {dx_dst(kOpTemp, 1, 0x3), dx_src(kOpInput, 0), dx_imm(16, 16, 16, 16)});
  a.ins(0, {dx_dst(kOpTemp, 1, 0x3), dx_src(kOpTemp, 1), dx_imm(-4, -4, -4, -4)});
  a.ins(27, {dx_dst(kOpTemp, 1, 0x3), dx_src(kOpTemp, 1)});   // ftoi
  a.ins(127, {dx_dst(kOpTemp, 0, 0xF), dx_src(kOpInput, 2), dx_src(kOpTemp, 1), dx_resource(0, 0x00), dx_sampler(0)});   // gather4_po r0, v2, r1, t0.xxxx, s0
  a.ins(54, {dx_dst(kOpOut, 0, 0xF), dx_src(kOpTemp, 0)});
  a.ins(62, {});
  return a.build();
}

void test_gather_register_offsets() {
  HostAllocator allocator;
  const std::vector<uint8_t> bytes = gather_po_shader();
  if (const char* path = getenv("GEN_GATHER_PO")) {
    if (FILE* f = fopen(path, "wb")) {
      fwrite(bytes.data(), 1, bytes.size(), f);
      fclose(f);
    }
  }
  const std::vector<uint8_t> shipped = read_blob("gather_po.dxbc");
  CHECK(shipped == bytes);   // (the blob of the conformance scenes is this shader)
  NtStatus st = 0;
  UniquePointer<D3d9Program> p = compile_dxbc(allocator, bytes, &st);
  CHECK(st == status::kSuccess && p);
  if (!p) return;
  DxilTextures textures;
  uint32_t seed = 99;
  auto random = [&](float low, float high) {
    seed = seed * 1664525u + 1013904223u;
    return low + (high - low) * static_cast<float>(seed >> 8) / static_cast<float>(1u << 24);
  };
  for (uint32_t n = 0; n < 200; ++n) {
    graphics::Vec4 varyings[16] = {};
    varyings[0] = {random(0, 0.5f), random(0, 0.5f), 0, 0};
    varyings[2] = {random(0, 1), random(0, 1), 0, 0};
    uint8_t constants[128] = {};
    graphics::Vec4 room[graphics::kMaxColorTargets] = {};
    CHECK(p->run_fragment_ex(varyings, graphics::FragmentContext(), constants, &textures, room));
    const int ox = static_cast<int>(varyings[0].x * 16.0f - 4.0f), oy = static_cast<int>(varyings[0].y * 16.0f - 4.0f);
    const float x = __builtin_floorf(varyings[2].x * 8.0f - 0.5f) + static_cast<float>(ox), y = __builtin_floorf(varyings[2].y * 4.0f - 0.5f) + static_cast<float>(oy);
    (void)y;
    const float xs[4] = {x, x + 1, x + 1, x};
    CHECK(close_to(room[0].x, (xs[0] + 0.5f) / 16.0f) && close_to(room[0].y, (xs[1] + 0.5f) / 16.0f) && close_to(room[0].z, (xs[2] + 0.5f) / 16.0f) && close_to(room[0].w, (xs[3] + 0.5f) / 16.0f));
  }
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
  // Residency feedback and the lookups' clamps (tiled resources tier 2): dxc's DXIL of ps_tiled / ps_tiled_clamp (dxil_ps_tiled*.hlsl) does what fxc's DXBC does, with textures whose
  // lookups report texels at u above one half unmapped and show the clamp they were given.
  {
    struct FeedbackTextures final : graphics::ITextureSet {
      mutable bool on = false, unmapped = false;
      mutable float clamp = -1e30f;
      void note(float u) const {
        if (on && u > 0.5f) unmapped = true;
      }
      float shown() const { return clamp > -1e29f ? clamp : 0.25f; }
      graphics::Vec4 sample(uint32_t, float u, float v) const override { return note(u), graphics::Vec4{u, v, shown(), 1.0f}; }
      graphics::Vec4 sample_lod(uint32_t, float u, float v, float lod) const override { return note(u), graphics::Vec4{u * 0.5f, v * 0.5f, (lod > clamp ? lod : clamp) * 0.125f, 0.5f}; }
      graphics::Vec4 fetch(uint32_t, int32_t x, int32_t y, uint32_t level) const override {
        return note(static_cast<float>(x) / 8.0f), graphics::Vec4{x * 0.0625f, y * 0.125f, static_cast<float>(level) * 0.25f, 0.75f};
      }
      bool size(uint32_t, uint32_t level, uint32_t* width, uint32_t* height, uint32_t* levels) const override {
        *width = 8u >> level, *height = 8u >> level, *levels = 3;
        return level < 3;
      }
      void begin_feedback(float min_lod) const override { on = true, unmapped = false, clamp = min_lod; }
      bool end_feedback() const override {
        const bool mapped = !unmapped;
        on = unmapped = false, clamp = -1e30f;
        return mapped;
      }
    } feedback_textures;
    const char* const pairs[2][2] = {{"ps_tiled.dxbc", "dxil_ps_tiled.dxil"}, {"ps_tiled_clamp.dxbc", "dxil_ps_tiled_clamp.dxil"}};
    unsigned unmapped_seen = 0, mapped_seen = 0;
    for (const auto& pair : pairs) {
      UniquePointer<D3d9Program> reference, translated;
      CHECK(load(pair[0], &reference) && load(pair[1], &translated));
      if (!reference || !translated) continue;
      CHECK(reference->uses_feedback() && translated->uses_feedback());
      for (uint32_t n = 0; n < 100; ++n) {
        graphics::Vec4 varyings[16] = {};
        varyings[2] = {random(0, 1), random(0, 1), 0, 0};
        graphics::FragmentContext context;
        context.position = {random(0, 32), random(0, 32), random(0, 1), 1};
        graphics::Vec4 a_room[graphics::kMaxColorTargets] = {}, b_room[graphics::kMaxColorTargets] = {};
        uint8_t constants[128] = {};
        CHECK(reference->run_fragment_ex(varyings, context, constants, &feedback_textures, a_room) && translated->run_fragment_ex(varyings, context, constants, &feedback_textures, b_room));
        const bool same = close_to(a_room[0].x, b_room[0].x) && close_to(a_room[0].y, b_room[0].y) && close_to(a_room[0].z, b_room[0].z) && close_to(a_room[0].w, b_room[0].w);
        if (!same && n < 3) printf("  %s #%u: %g %g %g %g vs %g %g %g %g\n", pair[1], n, a_room[0].x, a_room[0].y, a_room[0].z, a_room[0].w, b_room[0].x, b_room[0].y, b_room[0].z, b_room[0].w);
        CHECK(same);
        if (&pair == &pairs[0]) (a_room[0].w == 1.0f ? mapped_seen : unmapped_seen) += 1;
      }
    }
    CHECK(mapped_seen > 10 && unmapped_seen > 10);   // (both outcomes of the status were seen)
  }
  // The pixel shaders: the same varyings, the same colour (and the same pixel killed).
  for (const char* name : {"dxil_ps_math", "dxil_ps_tex", "dxil_ps_flow", "dxil_ps_loop", "dxil_ps_loop2"}) {
    UniquePointer<D3d9Program> reference, translated;
    const std::string base(name);
    CHECK(load((base + ".dxbc").c_str(), &reference) && load((base + ".dxil").c_str(), &translated));
    if (!reference || !translated) continue;
    CHECK(translated->stage() == graphics::ShaderStage::Fragment && translated->color_output_count() == 1);
    unsigned alive_count = 0;
    for (uint32_t n = 0; n < 200; ++n) {
      graphics::Vec4 varyings[16] = {};
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
        graphics::Vec4 varyings[16] = {};
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
      for (uint32_t n = 0; n < 200; ++n) {
        graphics::Vec4 varyings[16] = {};
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
        graphics::Vec4 varyings[16];
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
  // sample_d and sample_b: the level from the gradients (or the quad's differences of the coordinate) and the bias, over the fake texture whose lookup shows the level in the blue channel.
  {
    UniquePointer<D3d9Program> translated;
    CHECK(load("dxil_ps_grad.dxil", &translated));
    if (translated) {
      struct Plane {
        graphics::Vec4 origin[4], step_x[4], step_y[4];
        graphics::Vec4 at(uint32_t i, uint32_t corner) const {
          const float fx = static_cast<float>(corner & 1), fy = static_cast<float>(corner >> 1);
          return {origin[i].x + step_x[i].x * fx + step_y[i].x * fy, origin[i].y + step_x[i].y * fx + step_y[i].y * fy, origin[i].z + step_x[i].z * fx + step_y[i].z * fy,
                  origin[i].w + step_x[i].w * fx + step_y[i].w * fy};
        }
      };
      auto level = [](float ax, float ay, float bx, float by) {
        const float l1 = __builtin_sqrtf(ax * ax * 64.0f + ay * ay * 16.0f), l2 = __builtin_sqrtf(bx * bx * 64.0f + by * by * 16.0f);
        return __builtin_log2f(l1 > l2 ? l1 : l2);
      };
      for (uint32_t n = 0; n < 200; ++n) {
        Plane plane = {};
        for (uint32_t i = 0; i < 4; ++i) {
          plane.origin[i] = {random(0.2f, 1), random(0.2f, 1), random(0.2f, 1), random(0.2f, 1)};
          plane.step_x[i] = {random(0.01f, 0.2f), random(0.01f, 0.2f), random(0.01f, 0.2f), random(0.01f, 0.2f)};
          plane.step_y[i] = {random(0.01f, 0.2f), random(0.01f, 0.2f), random(0.01f, 0.2f), random(0.01f, 0.2f)};
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
        graphics::Vec4 varyings[16];
        for (uint32_t i = 0; i < 4; ++i) varyings[i] = plane.at(i, own);
        graphics::Vec4 room[graphics::kMaxColorTargets] = {};
        uint8_t constants[128] = {};
        const bool alive = translated->run_fragment_ex(varyings, context, constants, &textures, room);
        const graphics::Vec4 c = varyings[0], uv = varyings[2];
        const float lod_a = level(c.x * 0.1f, c.y * 0.05f, 0.02f, c.z * 0.2f);
        const uint32_t row = own & 2, col = own & 1;
        const graphics::Vec4 r0 = plane.at(2, row), r1 = plane.at(2, row + 1), c0 = plane.at(2, col), c1 = plane.at(2, col + 2);
        const float lod_b = level(r1.x - r0.x, r1.y - r0.y, c1.x - c0.x, c1.y - c0.y) + (c.w * 2.0f - 1.0f);
        const float want_blue = lod_a * 0.125f * 0.5f + lod_b * 0.125f * 0.5f;
        const float want_red = uv.x * 0.5f;   // (both lookups give u / 2)
        const bool same = alive && close_to(room[0].z, want_blue) && close_to(room[0].x, want_red);
        if (!same && n < 3) printf("  grad #%u: %g %g wants %g %g\n", n, room[0].x, room[0].z, want_red, want_blue);
        CHECK(same);
      }
    }
  }
  // CalculateLevelOfDetail (clamped and not): the same level from the quad's differences of the coordinate.
  {
    UniquePointer<D3d9Program> translated;
    CHECK(load("dxil_ps_lod.dxil", &translated));
    if (translated) {
      struct Plane {
        graphics::Vec4 origin[4], step_x[4], step_y[4];
        graphics::Vec4 at(uint32_t i, uint32_t corner) const {
          const float fx = static_cast<float>(corner & 1), fy = static_cast<float>(corner >> 1);
          return {origin[i].x + step_x[i].x * fx + step_y[i].x * fy, origin[i].y + step_x[i].y * fx + step_y[i].y * fy, origin[i].z + step_x[i].z * fx + step_y[i].z * fy,
                  origin[i].w + step_x[i].w * fx + step_y[i].w * fy};
        }
      };
      auto level = [](float ax, float ay, float bx, float by) {
        const float l1 = __builtin_sqrtf(ax * ax * 64.0f + ay * ay * 16.0f), l2 = __builtin_sqrtf(bx * bx * 64.0f + by * by * 16.0f);
        return __builtin_log2f(l1 > l2 ? l1 : l2);
      };
      for (uint32_t n = 0; n < 200; ++n) {
        Plane plane = {};
        for (uint32_t i = 0; i < 4; ++i) {
          plane.origin[i] = {random(0.2f, 1), random(0.2f, 1), random(0.2f, 1), random(0.2f, 1)};
          plane.step_x[i] = {random(0.01f, 0.2f), random(0.01f, 0.2f), random(0.01f, 0.2f), random(0.01f, 0.2f)};
          plane.step_y[i] = {random(0.01f, 0.2f), random(0.01f, 0.2f), random(0.01f, 0.2f), random(0.01f, 0.2f)};
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
        graphics::Vec4 varyings[16];
        for (uint32_t i = 0; i < 4; ++i) varyings[i] = plane.at(i, own);
        graphics::Vec4 room[graphics::kMaxColorTargets] = {};
        uint8_t constants[128] = {};
        const bool alive = translated->run_fragment_ex(varyings, context, constants, &textures, room);
        const uint32_t row = own & 2, col = own & 1;
        const graphics::Vec4 r0 = plane.at(2, row), r1 = plane.at(2, row + 1), c0 = plane.at(2, col), c1 = plane.at(2, col + 2);
        const float lod = level(r1.x - r0.x, r1.y - r0.y, c1.x - c0.x, c1.y - c0.y), top = 2.0f;
        const float want_x = lod < 0 ? 0 : lod > top ? top : lod;
        const bool same = alive && close_to(room[0].x, want_x) && close_to(room[0].y, lod);
        if (!same && n < 3) printf("  lod #%u: %g %g wants %g %g\n", n, room[0].x, room[0].y, want_x, lod);
        CHECK(same);
      }
    }
  }
  // A texture array indexed by a variable: the lookup of the element the index picks (the fake texture's colour says which unit answered).
  {
    UniquePointer<D3d9Program> translated;
    CHECK(load("dxil_ps_array.dxil", &translated));
    if (translated) {
      bool seen[2] = {};
      for (uint32_t n = 0; n < 200; ++n) {
        graphics::Vec4 varyings[16] = {};
        varyings[0] = {random(0, 1), 0, 0, 0};
        varyings[2] = {random(0, 1), random(0, 1), 0, 0};
        graphics::Vec4 room[graphics::kMaxColorTargets] = {};
        uint8_t constants[128] = {};
        const bool alive = translated->run_fragment_ex(varyings, graphics::FragmentContext(), constants, &textures, room);
        const uint32_t i = static_cast<uint32_t>(varyings[0].x * 1.99f);
        seen[i] = true;
        const float fi = static_cast<float>(i);
        const bool same = alive && close_to(room[0].x, varyings[2].x + 0.0625f) && close_to(room[0].y, varyings[2].y + 0.25f) && close_to(room[0].z, 0.25f + 0.1f * fi + 0.0f) && close_to(room[0].w, 1.75f + 0.0625f * fi);
        if (!same && n < 3) printf("  array #%u (%u): %g %g %g %g\n", n, i, room[0].x, room[0].y, room[0].z, room[0].w);
        CHECK(same);
      }
      CHECK(seen[0] && seen[1]);
    }
  }
  // sample_c / sample_c_lz: the four texels of the footprint compared with the reference (reference <= texel) and filtered bilinearly; the fake texture's red is (x + 0.5) / 16.
  {
    UniquePointer<D3d9Program> translated;
    CHECK(load("dxil_ps_cmp.dxil", &translated));
    if (translated) {
      auto compared = [](float u, float v, float reference) {
        const float px = u * 8.0f - 0.5f, py = v * 4.0f - 0.5f, x = __builtin_floorf(px), y = __builtin_floorf(py), fx = px - x, fy = py - y;
        float pass[2][2];
        for (int row = 0; row < 2; ++row)
          for (int col = 0; col < 2; ++col) pass[row][col] = reference <= (x + static_cast<float>(col) + 0.5f) / 16.0f ? 1.0f : 0.0f;
        const float top = pass[0][0] * (1 - fx) + pass[0][1] * fx, bottom = pass[1][0] * (1 - fx) + pass[1][1] * fx;
        return top * (1 - fy) + bottom * fy;
      };
      unsigned partial = 0;
      for (uint32_t n = 0; n < 300; ++n) {
        graphics::Vec4 varyings[16] = {};
        varyings[0] = {random(0, 0.5f), random(0, 0.5f), 0, 0};
        varyings[2] = {random(0, 1), random(0, 1), 0, 0};
        graphics::Vec4 room[graphics::kMaxColorTargets] = {};
        uint8_t constants[128] = {};
        const bool alive = translated->run_fragment_ex(varyings, graphics::FragmentContext(), constants, &textures, room);
        const float a = compared(varyings[2].x, varyings[2].y, varyings[0].x), b = compared(varyings[2].x * 0.9f, varyings[2].y * 0.9f, varyings[0].y);
        if (a > 0.01f && a < 0.99f) ++partial;
        const bool same = alive && close_to(room[0].x, a) && close_to(room[0].y, b) && close_to(room[0].z, a * b);
        if (!same && n < 3) printf("  cmp #%u: %g %g %g wants %g %g %g\n", n, room[0].x, room[0].y, room[0].z, a, b, a * b);
        CHECK(same);
      }
      CHECK(partial > 20);
    }
  }
  // Doubles: arithmetic, conversions, a select, a loop-carried sum, fma and comparisons, against the same function in C++ (doubles all the way, the float at the end).
  {
    UniquePointer<D3d9Program> translated;
    CHECK(load("dxil_ps_double.dxil", &translated));
    if (translated) {
      for (uint32_t n = 0; n < 300; ++n) {
        graphics::Vec4 varyings[16] = {};
        varyings[0] = {random(0.1f, 2), random(0, 2), random(0, 1), 0};
        graphics::Vec4 room[graphics::kMaxColorTargets] = {};
        uint8_t constants[128] = {};
        const bool alive = translated->run_fragment_ex(varyings, graphics::FragmentContext(), constants, &textures, room);
        const double a = varyings[0].x, b = static_cast<double>(varyings[0].y) * 3.0 + a, d = b / (a + 1.5), e = varyings[0].z > 0.5f ? d : -a;
        double sum = 0;
        for (int i = 0; i < 4; ++i) sum += a * static_cast<double>(i);
        const double f = a * b + 1.0;
        const uint32_t u = static_cast<uint32_t>(b * 4.0);
        const float want[4] = {static_cast<float>(d), static_cast<float>(e), static_cast<float>(sum), static_cast<float>(f) + static_cast<float>(u) + (a < b ? 100.0f : 0.0f)};
        const bool same = alive && close_to(room[0].x, want[0]) && close_to(room[0].y, want[1]) && close_to(room[0].z, want[2]) && close_to(room[0].w, want[3]);
        if (!same && n < 3) printf("  double #%u: %g %g %g %g wants %g %g %g %g\n", n, room[0].x, room[0].y, room[0].z, room[0].w, want[0], want[1], want[2], want[3]);
        CHECK(same);
      }
    }
  }
  // 64-bit integers: add, subtract, multiply, divide, remainder (unsigned and signed), shifts, comparisons and conversions, against the same function in C++.
  {
    UniquePointer<D3d9Program> translated;
    CHECK(load("dxil_ps_int64.dxil", &translated));
    if (translated) {
      for (uint32_t n = 0; n < 300; ++n) {
        graphics::Vec4 varyings[16] = {};
        varyings[0] = {random(0, 1), random(0, 1), random(0, 1), random(0, 1)};
        varyings[2] = {random(0, 1), random(0, 1), 0, 0};
        graphics::Vec4 room[graphics::kMaxColorTargets] = {};
        uint8_t constants[128] = {};
        const bool alive = translated->run_fragment_ex(varyings, graphics::FragmentContext(), constants, &textures, room);
        const float cx = varyings[0].x, cy = varyings[0].y, cz = varyings[0].z, cw = varyings[0].w, tx = varyings[2].x, ty = varyings[2].y;
        const uint64_t a = (uint64_t{static_cast<uint32_t>(cx * 4000000000.0f)} << 20) + uint64_t{static_cast<uint32_t>(cy * 3000000000.0f)} + 12345;
        const uint64_t b = (uint64_t{static_cast<uint32_t>(cz * 100.0f)} << 3) + uint64_t{static_cast<uint32_t>(tx * 1000.0f)} + 7;
        const uint64_t sum = a + b, diff = a - b, prod = a * b, quo = a / b, rem = a % b;
        const uint64_t sh = (a << static_cast<uint32_t>(ty * 40.0f)) ^ (b >> 3) ^ (a >> static_cast<uint32_t>(cw * 50.0f));
        const int64_t sa = static_cast<int64_t>(a) - static_cast<int64_t>(b << 30), sq = sa / static_cast<int64_t>(b + 3), sr = sa % static_cast<int64_t>(b + 3);
        const uint64_t s5 = (a < b ? 1 : 0) + (a > b ? 2 : 0) + (sa < 0 ? 4 : 0) + (a >= b ? 8 : 0) + (static_cast<int64_t>(a) <= static_cast<int64_t>(b) ? 16 : 0) + (a == b ? 32 : 0) + (sa != 0 ? 64 : 0);
        const float want[4] = {static_cast<float>(static_cast<uint32_t>(sum >> 32)) + static_cast<float>(static_cast<uint32_t>(sum)) * 0.0000001f,
                               static_cast<float>(static_cast<uint32_t>(diff ^ (diff >> 32))) + static_cast<float>(static_cast<uint32_t>(prod >> 40)),
                               static_cast<float>(static_cast<uint32_t>(quo)) + static_cast<float>(static_cast<uint32_t>(rem)) * 3.0f + static_cast<float>(static_cast<uint32_t>(sh >> 17)),
                               static_cast<float>(static_cast<uint32_t>(sq >> 16)) + static_cast<float>(static_cast<uint32_t>(sr)) * 0.5f + static_cast<float>(static_cast<uint32_t>(s5)) + static_cast<float>(sa) * 1e-12f +
                                   static_cast<float>(static_cast<int64_t>(a >> 3))};
        const bool same = alive && close_to(room[0].x, want[0]) && close_to(room[0].y, want[1]) && close_to(room[0].z, want[2]) && close_to(room[0].w, want[3]);
        if (!same && n < 3) printf("  int64 #%u: %g %g %g %g wants %g %g %g %g\n", n, room[0].x, room[0].y, room[0].z, room[0].w, want[0], want[1], want[2], want[3]);
        CHECK(same);
      }
    }
  }
  // Returns from inside a loop.
  {
    UniquePointer<D3d9Program> translated;
    CHECK(load("dxil_ps_retloop.dxil", &translated));
    if (translated) {
      unsigned inside = 0;
      for (uint32_t n = 0; n < 300; ++n) {
        graphics::Vec4 varyings[16] = {};
        varyings[0] = {random(-1.5f, 1), 0, 0, 0};
        varyings[2] = {random(0, 1), 0, 0, 0};
        graphics::Vec4 room[graphics::kMaxColorTargets] = {};
        uint8_t constants[128] = {};
        const bool alive = translated->run_fragment_ex(varyings, graphics::FragmentContext(), constants, &textures, room);
        float acc = 0, want[4] = {0, -1, 0, 1};
        bool done = false;
        for (int i = 0; i < 8 && !done; ++i) {
          acc += varyings[0].x + 0.1f;
          if (acc > varyings[2].x * 4.0f) want[0] = acc, want[1] = static_cast<float>(i), want[2] = 1, want[3] = 1, done = true, ++inside;
          else if (acc < -2.0f) want[0] = 0, want[1] = 0, want[2] = acc, want[3] = 0.5f, done = true, ++inside;
        }
        if (!done) want[0] = acc;
        const bool same = alive && close_to(room[0].x, want[0]) && close_to(room[0].y, want[1]) && close_to(room[0].z, want[2]) && close_to(room[0].w, want[3]);
        if (!same && n < 3) printf("  retloop #%u: %g %g %g %g wants %g %g %g %g\n", n, room[0].x, room[0].y, room[0].z, room[0].w, want[0], want[1], want[2], want[3]);
        CHECK(same);
      }
      CHECK(inside > 100 && inside < 300);
    }
  }
  { UniquePointer<D3d9Program> shown; CHECK(load("dxil_ps_retloop_show.dxil", &shown)); }
  // Returns from inside a real loop (a trip count that is not a constant).
  {
    UniquePointer<D3d9Program> translated;
    CHECK(load("dxil_ps_retloop2.dxil", &translated));
    if (translated) {
      unsigned inside = 0;
      for (uint32_t n = 0; n < 300; ++n) {
        graphics::Vec4 varyings[16] = {};
        varyings[0] = {random(-1.5f, 1), 0, 0, 0};
        varyings[2] = {random(0, 1), random(0, 1), 0, 0};
        graphics::Vec4 room[graphics::kMaxColorTargets] = {};
        uint8_t constants[128] = {};
        const bool alive = translated->run_fragment_ex(varyings, graphics::FragmentContext(), constants, &textures, room);
        float acc = 0, want[4] = {0, -1, 0, 1};
        bool done = false;
        const int trips = static_cast<int>(varyings[2].y * 8.0f) + 1;
        for (int i = 0; i < trips && !done; ++i) {
          acc += varyings[0].x + 0.1f;
          if (acc > varyings[2].x * 4.0f) want[0] = acc, want[1] = static_cast<float>(i), want[2] = 1, want[3] = 1, done = true, ++inside;
          else if (acc < -2.0f) want[0] = 0, want[1] = 0, want[2] = acc, want[3] = 0.5f, done = true, ++inside;
        }
        if (!done) want[0] = acc;
        const bool same = alive && close_to(room[0].x, want[0]) && close_to(room[0].y, want[1]) && close_to(room[0].z, want[2]) && close_to(room[0].w, want[3]);
        if (!same && n < 3) printf("  retloop2 #%u: %g %g %g %g wants %g %g %g %g\n", n, room[0].x, room[0].y, room[0].z, room[0].w, want[0], want[1], want[2], want[3]);
        CHECK(same);
      }
      CHECK(inside > 100 && inside < 300);
    }
  }
  // 16-bit types (min16 / native half, int16, uint16): results rounded to halves and wrapped to 16 bits after each operation, against the same function in C++.
  {
    UniquePointer<D3d9Program> translated;
    CHECK(load("dxil_ps_half.dxil", &translated));
    if (translated) {
      auto H = [](float x) { return graphics::half_to_float(graphics::float_to_half(x)); };
      auto s16 = [](int32_t x) { return static_cast<int32_t>(static_cast<int16_t>(x)); };
      unsigned checked = 0;
      for (uint32_t n = 0; n < 300; ++n) {
        graphics::Vec4 varyings[16] = {};
        varyings[0] = {random(0, 2), random(0, 2), random(0, 1), random(0, 1)};
        varyings[2] = {random(0, 1), random(0, 1), 0, 0};
        const float hv[4] = {H(random(-2, 2)), H(random(-2, 2)), H(random(-2, 2)), H(random(-2, 2))};
        const int16_t kv[4] = {static_cast<int16_t>(random(-100, 100)), static_cast<int16_t>(random(-300, 300)), static_cast<int16_t>(random(-20, 20)), static_cast<int16_t>(random(-100, 100))};
        const int16_t divisor_shift = kv[2] == -3 ? 5 : kv[2];   // (a division by zero is for no one to compare)
        uint8_t constants[128] = {};
        ByteWriter cw(ByteView(constants, 16));
        for (int i = 0; i < 4; ++i) cw.le16(graphics::float_to_half(hv[i]));
        for (int i = 0; i < 4; ++i) cw.le16(static_cast<uint16_t>(i == 2 ? divisor_shift : kv[i]));
        graphics::Vec4 room[graphics::kMaxColorTargets] = {};
        const bool alive = translated->run_fragment_ex(varyings, graphics::FragmentContext(), constants, &textures, room);
        const float cx = varyings[0].x, cy = varyings[0].y, cz = varyings[0].z, cw_ = varyings[0].w, tx = varyings[2].x, ty = varyings[2].y;
        const float a = H(cx), b = H(H(cy * 3.0f) + hv[0]);
        const float s = H(H(a * b) + hv[1]), s_fused = H(a * b + hv[1]);
        const float d = H(H(a + H(0.001f)) / H(b + H(1.5f)));
        const float m = H((a > b ? a : b) - (a < hv[2] ? a : hv[2]));
        const int32_t i = s16(s16(static_cast<int32_t>(cz * 200.0f)) - kv[0]), j = s16(s16(i * s16(static_cast<int32_t>(tx * 300.0f))) + kv[1]);
        const uint32_t u = static_cast<uint16_t>(static_cast<int32_t>(ty * 60000.0f)), v = (u + static_cast<uint16_t>(static_cast<int32_t>(cw_ * 9000.0f))) & 0xFFFF;
        const int32_t sh = s16(s16(j >> 3) + s16(i / s16(divisor_shift + 3)));
        const float q = H(H(static_cast<float>(i)) * H(0.01f));
        const float want[4] = {s * 0.1f + d, m + q, static_cast<float>(j) + static_cast<float>(sh) * 0.5f, static_cast<float>(v) + (u > v ? 1.0f : 0.0f) + (i < kv[3] ? 2.0f : 0.0f)};
        const float want_fused[4] = {s_fused * 0.1f + d, want[1], want[2], want[3]};
        const bool same = alive && (close_to(room[0].x, want[0]) || close_to(room[0].x, want_fused[0])) && close_to(room[0].y, want[1]) && close_to(room[0].z, want[2]) && close_to(room[0].w, want[3]);
        if (!same && n < 4) printf("  half #%u: %g %g %g %g wants %g %g %g %g\n", n, room[0].x, room[0].y, room[0].z, room[0].w, want[0], want[1], want[2], want[3]);
        CHECK(same);
        ++checked;
      }
      CHECK(checked == 300);
    }
  }
  // Views of buffers in a pixel shader (a structured and a byte address buffer, read by index): the FragmentContext carries the bound buffers.
  {
    UniquePointer<D3d9Program> translated;
    CHECK(load("dxil_ps_srv.dxil", &translated));
    if (translated) {
      CHECK((translated->required_features() & graphics::kFeatureStorageInDraws) != 0);
      std::vector<uint32_t> structured(32 * 4), raw(16);
      for (uint32_t i = 0; i < structured.size(); ++i) structured[i] = 0x9E3779B9u * (i + 1);
      for (uint32_t i = 0; i < raw.size(); ++i) raw[i] = 0x85EBCA6Bu * (i + 3);
      ByteView views[graphics::kMaxStorageBuffers];
      views[graphics::kMaxUavBuffers] = ByteView(reinterpret_cast<uint8_t*>(structured.data()), structured.size() * 4);
      views[graphics::kMaxUavBuffers + 1] = ByteView(reinterpret_cast<uint8_t*>(raw.data()), raw.size() * 4);
      for (uint32_t n = 0; n < 200; ++n) {
        graphics::Vec4 varyings[16] = {};
        varyings[0] = {random(0, 1), random(0, 1), random(0, 1), 0};
        varyings[2] = {random(0, 1), 0, 0, 0};
        graphics::FragmentContext context;
        context.storage = views;
        graphics::Vec4 room[graphics::kMaxColorTargets] = {};
        uint8_t constants[128] = {};
        const bool alive = translated->run_fragment_ex(varyings, context, constants, &textures, room);
        const uint32_t i = static_cast<uint32_t>(varyings[0].x * 7.99f) * 4 + static_cast<uint32_t>(varyings[0].y * 3.99f);
        const uint32_t r = raw[static_cast<uint32_t>(varyings[2].x * 3.99f) + static_cast<uint32_t>(varyings[0].z * 3.99f)];
        const float want[4] = {static_cast<float>(structured[4 * i] & 255) / 255.0f, static_cast<float>(structured[4 * i + 1] & 255) / 255.0f, static_cast<float>((structured[4 * i + 2] ^ r) & 255) / 255.0f,
                               static_cast<float>((structured[4 * i + 3] + r) & 255) / 255.0f};
        const bool same = alive && close_to(room[0].x, want[0]) && close_to(room[0].y, want[1]) && close_to(room[0].z, want[2]) && close_to(room[0].w, want[3]);
        if (!same && n < 3) printf("  ps srv #%u: %g %g %g %g wants %g %g %g %g\n", n, room[0].x, room[0].y, room[0].z, room[0].w, want[0], want[1], want[2], want[3]);
        CHECK(same);
      }
    }
  }
  // gather4 with constant offsets, and gather4_c (the comparison of each texel of the footprint): against the same footprint worked out in C++.
  {
    UniquePointer<D3d9Program> offsets, compared;
    CHECK(load("dxil_ps_gather_off.dxil", &offsets) && load("dxil_ps_gather_cmp.dxil", &compared));
    // (the fake texture's lookups give the middle of the texel halved: red (x + 0.5) / 16, green (y + 0.5) / 8)
    auto footprint = [](float u, float v, int ox, int oy, float* red, float* green) {
      const float x = __builtin_floorf(u * 8.0f - 0.5f) + static_cast<float>(ox), y = __builtin_floorf(v * 4.0f - 0.5f) + static_cast<float>(oy);
      const float xs[4] = {x, x + 1, x + 1, x}, ys[4] = {y + 1, y + 1, y, y};
      for (int k = 0; k < 4; ++k) red[k] = (xs[k] + 0.5f) / 16.0f, green[k] = (ys[k] + 0.5f) / 8.0f;
    };
    for (uint32_t n = 0; n < 200 && offsets && compared; ++n) {
      graphics::Vec4 varyings[16] = {};
      varyings[0] = {random(0, 0.5f), random(0, 0.5f), 0, 0};
      varyings[2] = {random(0, 1), random(0, 1), 0, 0};
      uint8_t constants[128] = {};
      graphics::Vec4 room[graphics::kMaxColorTargets] = {};
      CHECK(offsets->run_fragment_ex(varyings, graphics::FragmentContext(), constants, &textures, room));
      float r1[4], g1[4], r2[4], g2[4];
      footprint(varyings[2].x, varyings[2].y, 1, -2, r1, g1);
      footprint(varyings[2].x, varyings[2].y, -3, 2, r2, g2);
      CHECK(close_to(room[0].x, r1[0]) && close_to(room[0].y, r1[3]) && close_to(room[0].z, g2[1]) && close_to(room[0].w, g2[2]));
      room[0] = {};
      CHECK(compared->run_fragment_ex(varyings, graphics::FragmentContext(), constants, &textures, room));
      float ra[4], ga[4], rb[4], gb[4];
      footprint(varyings[2].x, varyings[2].y, 0, 0, ra, ga);
      footprint(varyings[2].x, varyings[2].y, 1, 1, rb, gb);
      auto pass = [](float reference, float texel) { return reference <= texel ? 1.0f : 0.0f; };
      const float want[4] = {(pass(varyings[0].x, ra[0]) + pass(varyings[0].x, ra[1]) * 2) * 0.0625f, (pass(varyings[0].x, ra[2]) * 4 + pass(varyings[0].x, ra[3]) * 8) * 0.0625f,
                             (pass(varyings[0].y, rb[0]) + pass(varyings[0].y, rb[1]) * 2) * 0.0625f, (pass(varyings[0].y, rb[2]) * 4 + pass(varyings[0].y, rb[3]) * 8) * 0.0625f};
      const bool same = close_to(room[0].x, want[0]) && close_to(room[0].y, want[1]) && close_to(room[0].z, want[2]) && close_to(room[0].w, want[3]);
      if (!same && n < 3) printf("  gather cmp #%u: %g %g %g %g wants %g %g %g %g\n", n, room[0].x, room[0].y, room[0].z, room[0].w, want[0], want[1], want[2], want[3]);
      CHECK(same);
    }
  }
  // atan, asin and acos (polynomials in the translation): compared with libm within 1e-4.
  {
    UniquePointer<D3d9Program> translated;
    CHECK(load("dxil_ps_inv.dxil", &translated));
    if (translated) {
      auto near = [](float a, float b) { return __builtin_fabsf(a - b) < 1e-4f; };
      for (uint32_t n = 0; n < 300; ++n) {
        graphics::Vec4 varyings[16] = {};
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
        graphics::Vec4 attributes[16] = {{random(-1, 1), random(-1, 1), random(-1, 1), 1}, {random(0, 1), random(0, 1), random(0, 1), random(0, 1)}, {}, {}};
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


  // A view of a buffer in a vertex shader, indexed by SV_VertexID.
  {
    UniquePointer<D3d9Program> translated;
    CHECK(load("dxil_vs_srv.dxil", &translated));
    if (translated) {
      CHECK((translated->required_features() & graphics::kFeatureStorageInDraws) != 0);
      float table[16];
      for (float& f : table) f = random(-1, 1);
      ByteView views[graphics::kMaxStorageBuffers];
      views[graphics::kMaxUavBuffers] = ByteView(reinterpret_cast<uint8_t*>(table), sizeof table);
      for (uint32_t n = 0; n < 100; ++n) {
        graphics::Vec4 attributes[16] = {{random(-1, 1), random(-1, 1), random(-1, 1), 1}, {random(0, 1), random(0, 1), random(0, 1), random(0, 1)}, {}, {}};
        graphics::VertexContext context;
        context.vertex_id = n;
        context.storage = views;
        uint8_t constants[128] = {};
        graphics::ShaderIo io = {};
        translated->run_vertex_ex(attributes, constants, context, &io);
        const float* d = table + 4 * (n & 3);
        const bool same = close_to(io.position.x, attributes[0].x + d[0]) && close_to(io.position.y, -(attributes[0].y + d[1])) && close_to(io.position.z, attributes[0].z + d[2]) && close_to(io.varying[0].x, attributes[1].x * d[3]) &&
                          close_to(io.varying[0].w, attributes[1].w * d[3]) && close_to(io.varying[2].x, attributes[0].x + d[3]) && close_to(io.varying[2].y, attributes[0].y + 0.5f);
        if (!same && n < 3) printf("  vs srv #%u: %g %g %g wants %g %g %g\n", n, io.position.x, io.position.y, io.position.z, attributes[0].x + d[0], -(attributes[0].y + d[1]), attributes[0].z + d[2]);
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
        graphics::GeometryInputData in = {};
        for (uint32_t v = 0; v < 3; ++v) {
          in.position[v] = {random(-1, 1), random(-1, 1), random(0, 1), random(0.5f, 2)};
          in.varying[v][0] = {random(0, 1), random(0, 1), random(0, 1), random(0, 1)};
          in.varying[v][2] = {random(0, 1), random(0, 1), 0, 0};
        }
        graphics::GeometryOutputData a = {}, b = {};
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

  // A geometry shader with two output streams (the streams of each vertex and every output of it).
  {
    UniquePointer<D3d9Program> reference, translated;
    CHECK(load("dxil_gs_streams.dxbc", &reference) && load("dxil_gs_streams.dxil", &translated));
    if (reference && translated) {
      for (uint32_t n = 0; n < 100; ++n) {
        graphics::GeometryInputData in = {};
        for (uint32_t v = 0; v < 3; ++v) {
          in.position[v] = {random(-1, 1), random(-1, 1), random(0, 1), random(0.5f, 2)};
          in.varying[v][0] = {random(0, 1), random(0, 1), random(0, 1), random(0, 1)};
          in.varying[v][2] = {random(0, 1), random(0, 1), 0, 0};
        }
        graphics::GeometryOutputData a = {}, b = {};
        uint8_t constants[128] = {};
        reference->run_geometry(in, constants, &a);
        translated->run_geometry(in, constants, &b);
        bool same = a.count == b.count && a.count == 6;
        bool both_streams[2] = {};
        for (uint32_t i = 0; same && i < a.count; ++i) {
          same = a.stream[i] == b.stream[i] && a.restart[i] == b.restart[i] && close_to(a.vertex[i].position.x, b.vertex[i].position.x) && close_to(a.vertex[i].position.w, b.vertex[i].position.w);
          for (uint32_t k = 0; k < 4; ++k) same = same && close_to(a.vertex[i].varying[k].x, b.vertex[i].varying[k].x) && close_to(a.vertex[i].varying[k].w, b.vertex[i].varying[k].w);
          if (a.stream[i] < 2) both_streams[a.stream[i]] = true;
        }
        if (!same && n == 0) printf("  streams: counts %u %u\n", a.count, b.count);
        if (!same && n == 0) for (uint32_t i = 0; i < a.count; ++i) { printf("   %u: stream %u/%u restart %d/%d pos %g %g %g %g | %g %g %g %g\n", i, a.stream[i], b.stream[i], a.restart[i], b.restart[i], a.vertex[i].position.x, a.vertex[i].position.y, a.vertex[i].position.z, a.vertex[i].position.w, b.vertex[i].position.x, b.vertex[i].position.y, b.vertex[i].position.z, b.vertex[i].position.w); for (uint32_t k = 0; k < 4; ++k) printf("      var%u %g %g %g %g | %g %g %g %g\n", k, a.vertex[i].varying[k].x, a.vertex[i].varying[k].y, a.vertex[i].varying[k].z, a.vertex[i].varying[k].w, b.vertex[i].varying[k].x, b.vertex[i].varying[k].y, b.vertex[i].varying[k].z, b.vertex[i].varying[k].w); }
        CHECK(same && both_streams[0] && both_streams[1]);
      }
      printf("  dxil dxil_gs_streams: 100 triangles compared\n");
    }
  }

  // Views of buffers in a geometry shader and in the hull and domain shaders of a patch.
  {
    UniquePointer<D3d9Program> gs, hs, ds;
    CHECK(load("dxil_gs_srv.dxil", &gs) && load("dxil_hs_srv.dxil", &hs) && load("dxil_ds_srv.dxil", &ds));
    float table[16];
    for (float& f : table) f = random(0.5f, 3.0f);
    ByteView views[graphics::kMaxStorageBuffers];
    views[graphics::kMaxUavBuffers] = ByteView(reinterpret_cast<uint8_t*>(table), sizeof table);
    if (gs) {
      CHECK(gs->stage() == graphics::ShaderStage::Geometry && (gs->required_features() & graphics::kFeatureStorageInDraws));
      for (uint32_t n = 0; n < 50; ++n) {
        graphics::GeometryInputData in = {};
        in.storage = views;
        for (uint32_t v = 0; v < 3; ++v) {
          in.position[v] = {random(-1, 1), random(-1, 1), random(0, 1), random(0.5f, 2)};
          in.varying[v][0] = {random(0, 1), random(0, 1), random(0, 1), random(0, 1)};
        }
        graphics::GeometryOutputData out = {};
        uint8_t constants[128] = {};
        gs->run_geometry(in, constants, &out);
        bool same = out.count == 3;
        for (uint32_t v = 0; same && v < 3; ++v)
          same = close_to(out.vertex[v].position.x, in.position[v].x + table[4 * v]) && close_to(out.vertex[v].position.y, in.position[v].y - table[4 * v + 1]) && close_to(out.vertex[v].position.w, in.position[v].w + table[4 * v + 3]) &&
                 close_to(out.vertex[v].varying[0].x, in.varying[v][0].x * table[12]);
        CHECK(same);
      }
    }
    if (hs && ds) {
      CHECK(hs->stage() == graphics::ShaderStage::Hull && ds->stage() == graphics::ShaderStage::Domain);
      for (uint32_t n = 0; n < 50; ++n) {
        graphics::HullInputData in;
        in.storage = views;
        in.control_points = 3;
        for (uint32_t v = 0; v < 3; ++v) in.position[v] = {random(-1, 1), random(-1, 1), random(0, 1), 1};
        graphics::HullOutputData patch;
        uint8_t constants[128] = {};
        hs->run_hull(in, constants, &patch);
        CHECK(close_to(patch.outer[0], table[0]) && close_to(patch.outer[1], table[1]) && close_to(patch.outer[2], table[2]) && close_to(patch.inner[0], table[3]));
        patch.storage = views;
        const float u = random(0, 1), v = random(0, 1) * (1 - u), w = 1 - u - v;
        graphics::ShaderIo io = {};
        ds->run_domain(patch, u, v, w, constants, &io);
        const graphics::Vec4 p0 = patch.control_point[0].position, p1 = patch.control_point[1].position, p2 = patch.control_point[2].position;
        CHECK(close_to(io.position.x, p0.x * u + p1.x * v + p2.x * w + table[4] * u * v));
      }
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
  image.tile_narrow = static_cast<uint32_t>(NarrowLayout::R8);
  CHECK(e.kernel.create_allocation(kPid, e.dev, image, &none) == status::kInvalidParameter);      // (a narrow layout of an image that is not A8B8G8R8)
  image.tile_narrow = 0;
  image.flags = kAllocDepthStencil | kAllocTexture | kAllocReserved, image.format = Format::D32Float;
  CHECK(e.kernel.create_allocation(kPid, e.dev, image, &none) == status::kSuccess);      // (a reserved depth buffer: floats in its backing, kFeature2DepthCopies)
  image.format = Format::D24S8;
  CHECK(e.kernel.create_allocation(kPid, e.dev, image, &none) == status::kInvalidParameter);      // (... not one with a stencil plane)
  image.format = Format::D32Float, image.tile_narrow = static_cast<uint32_t>(NarrowLayout::R8);
  CHECK(e.kernel.create_allocation(kPid, e.dev, image, &none) == status::kInvalidParameter);      // (its heaps' texels are floats or 16-bit codes)
  image.tile_narrow = 0;
  image.format = Format::A8R8G8B8;
  AllocationDesc narrow_buffer;
  narrow_buffer.flags = kAllocVertexBuffer, narrow_buffer.size = kTileBytes, narrow_buffer.tile_narrow = 1;
  CHECK(e.kernel.create_allocation(kPid, e.dev, narrow_buffer, &none) == status::kInvalidParameter);      // (a narrow layout is of reserved textures)
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

// Derivatives through the whole draw: the rasterizer hands each pixel the corners of its quad, so ddx_fine of a function of the interpolated coordinate is its slope per pixel.
void test_derivative_draw() {
  Env e;
  namespace b = conformance_blobs;
  Handle vs = 0, ps = 0, pipe = 0;
  ShaderDesc sd;
  sd.kind = ShaderKind::Dxbc, sd.stage = graphics::ShaderStage::Vertex, sd.code = ConstByteView(b::k_vs, sizeof b::k_vs);
  CHECK(e.kernel.create_shader(kPid, e.dev, sd, &vs) == status::kSuccess);
  sd.stage = graphics::ShaderStage::Fragment, sd.code = ConstByteView(b::k_dxil_ps_deriv_show, sizeof b::k_dxil_ps_deriv_show);
  CHECK(e.kernel.create_shader(kPid, e.dev, sd, &ps) == status::kSuccess);
  PipelineDesc pd;
  pd.vertex_shader = vs, pd.fragment_shader = ps, pd.vertex_stride = 28, pd.attribute_count = 2, pd.varying_count = 3;
  pd.attributes[0] = {0, graphics::VertexFormat::Float3, 0};
  pd.attributes[1] = {1, graphics::VertexFormat::Float4, 12};
  pd.cull_mode = graphics::CullMode::None;
  CHECK(e.kernel.create_pipeline(kPid, e.dev, pd, &pipe) == status::kSuccess);
  Handle target = e.alloc(kTarget, Format::A8R8G8B8, 32, 32), vb = e.buffer(kAllocVertexBuffer, 6 * 28);
  const float corners[6][4] = {{-0.9f, -0.9f, 0, 0}, {0.9f, -0.9f, 1, 0}, {-0.9f, 0.9f, 0, 1}, {-0.9f, 0.9f, 0, 1}, {0.9f, -0.9f, 1, 0}, {0.9f, 0.9f, 1, 1}};
  float v[42];
  for (uint32_t i = 0; i < 6; ++i) {
    const float* c = corners[i];
    const float one[7] = {c[0], c[1], 0.5f, c[2] / 2, c[3] / 2, 1, 1};
    for (uint32_t k = 0; k < 7; ++k) v[7 * i + k] = one[k];
  }
  CHECK(e.write(vb, floats(v, 42)));
  const float identity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
  CommandWriter w(e.allocator);
  w.begin_pass(0, kNoAllocation, kPassClearColor, {0, 0, 0, 1}, 1.0f, 0);
  w.bind_pipeline(pipe);
  w.bind_vertex_buffer(1);
  w.push_constants(floats(identity, 16));
  w.draw(6);
  w.end_pass();
  { const NtStatus rs = e.render(w, {target, vb}); if (rs != status::kSuccess) printf("render status %x\n", static_cast<unsigned>(rs)); CHECK(rs == status::kSuccess); }
  // (at the middle u = v = 0.5: d(u v / 4 + u u) / du = 1.125 per unit, 1 / 28.8 unit per pixel, times 8 and 255: about 80; the blue one is a function of v only)
  const uint32_t pixel = e.pixel(target, 16, 16);
  const uint32_t red = (pixel >> 16) & 0xFF, blue = pixel & 0xFF;
  CHECK(red >= 70 && red <= 90 && blue == 0);
  // (the same pair of pixels of a quad gets the same value: fine derivatives are per row)
  CHECK(((e.pixel(target, 16, 16) >> 16) & 0xFF) == ((e.pixel(target, 17, 16) >> 16) & 0xFF));
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


// --- Fixed state and formats of gap pass 4 -----------------------------------------------------------------------------------------------------------------------------------

// The filter word (filter_word / decode_filter_word: the separate magnification filter, the reductions, coordinates in texels) and a D3D12_FILTER read as a sampler; SetTopology's
// strip cut word; the line modes on the CPU renderer (a horizontal line between rows 7 and 8 of a 16 x 16 target: one row aliased, two as a quadrilateral 1.4 wide, one 1.0 wide,
// and an alpha-antialiased diagonal whose pixels hold the part of them it covers in the alpha), the escape's 'LIN1' part and a device without the line modes; the R1 copies
// (NarrowLayout::R1: bits into R8Unorm and A8B8G8R8 images and back); mirrors of the one- and two-channel families and of several levels.
void test_fixed_state_gaps() {
  {
    graphics::SamplerDesc s;
    s.filter = graphics::Filter::Nearest, s.separate_mag = true, s.mag_filter = graphics::Filter::Linear, s.reduction = graphics::SamplerReduction::Maximum, s.unnormalized = true;
    graphics::SamplerDesc back;
    CHECK(filter_word(s) == (0u | 2u | 4u | (2u << 4) | 0x100u) && decode_filter_word(filter_word(s), &back) && back.separate_mag && back.mag_filter == graphics::Filter::Linear &&
          back.filter == graphics::Filter::Nearest && back.reduction == graphics::SamplerReduction::Maximum && back.unnormalized);
    CHECK(decode_filter_word(1, &back) && back.filter == graphics::Filter::Linear && !back.separate_mag && back.mag_filter == graphics::Filter::Linear);
    CHECK(!decode_filter_word(4, &back) && !decode_filter_word(3u << 4, &back) && !decode_filter_word(0x200, &back) && !decode_filter_word(0x8, &back));
    CHECK(graphics::sampler_features2(s) == (graphics::kFeature2SeparateFilters | graphics::kFeature2SamplerReductions | graphics::kFeature2UnnormalizedCoordinates));
    graphics::SamplerDesc d;
    CHECK(d3d12_filter(0x10, &d) && d.filter == graphics::Filter::Linear && d.mag_filter == graphics::Filter::Nearest && d.separate_mag && d.mip_filter == graphics::MipFilter::Nearest);
    CHECK(d3d12_filter(0x154, &d) && !d.separate_mag && d.filter == graphics::Filter::Linear && d.reduction == graphics::SamplerReduction::Minimum && d.mip_filter == graphics::MipFilter::Nearest);
    CHECK(d3d12_filter(0x1D5, &d) && d.reduction == graphics::SamplerReduction::Maximum && d.mip_filter == graphics::MipFilter::Linear);
    CHECK(!d3d12_filter(0x40, &d) && !d3d12_filter(0x2, &d) && !d3d12_filter(0x200, &d));
    CHECK(is_strip_cut(StripCut::IndexMax, 2, 0xFFFF) && !is_strip_cut(StripCut::IndexMax, 4, 0xFFFF) && is_strip_cut(StripCut::Cut16, 4, 0xFFFF) && !is_strip_cut(StripCut::None, 2, 0xFFFF) &&
          is_strip_cut(StripCut::Cut32, 4, 0xFFFFFFFFu) && !is_strip_cut(StripCut::Cut32, 2, 0xFFFF));
  }
  Env e;
  AdapterCaps caps;
  CHECK(e.kernel.query_adapter_caps(kPid, e.adapter, &caps) == status::kSuccess);
  const uint32_t mine = graphics::kFeature2SeparateFilters | graphics::kFeature2SamplerReductions | graphics::kFeature2UnnormalizedCoordinates | graphics::kFeature2LineModes;
  CHECK((caps.features2 & mine) == mine);
  Handle rt = e.alloc(kTarget, Format::A8R8G8B8, 16, 16), vb = e.buffer(kAllocVertexBuffer, 256);
  const float identity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
  auto pipeline_for = [&](PipelineDesc pd) {
    Handle vs = 0, fs = 0, pipe = 0;
    CHECK(e.kernel.create_shader(kPid, e.dev, {ShaderKind::ColoredVertex, graphics::ShaderStage::Vertex, {}}, &vs) == status::kSuccess);
    CHECK(e.kernel.create_shader(kPid, e.dev, {ShaderKind::ColoredFragment, graphics::ShaderStage::Fragment, {}}, &fs) == status::kSuccess);
    graphics::GraphicsPipelineDesc d;
    graphics::describe_colored_pipeline(&d);
    pd.vertex_shader = vs, pd.fragment_shader = fs;
    pd.vertex_stride = d.vertex_stride, pd.attribute_count = d.attribute_count, pd.varying_count = d.varying_count;
    for (uint32_t i = 0; i < d.attribute_count; ++i) pd.attributes[i] = d.attributes[i];
    pd.cull_mode = graphics::CullMode::None;
    CHECK(e.kernel.create_pipeline(kPid, e.dev, pd, &pipe) == status::kSuccess);
    return pipe;
  };
  auto picture = [&](Handle pipe, Topology topology) {
    CommandWriter w(e.allocator);
    w.begin_pass(0, kNoAllocation, kPassClearColor, {0, 0, 0, 1}, 1, 0);
    w.bind_pipeline(pipe);
    w.bind_vertex_buffer(1);
    w.push_constants(floats(identity, 16));
    w.set_topology(topology);
    w.draw(2);
    w.end_pass();
    CHECK(e.render(w, {rt, vb}) == status::kSuccess);
    std::vector<uint32_t> out;
    for (uint32_t y = 0; y < 16; ++y)
      for (uint32_t x = 0; x < 16; ++x) out.push_back(e.pixel(rt, x, y));
    return out;
  };
  {
    const float horizontal[14] = {-0.75f, 0, 0.5f, 1, 0, 0, 1, 0.75f, 0, 0.5f, 1, 0, 0, 1};
    CHECK(e.write(vb, floats(horizontal, 14)));
    uint32_t rows[3] = {0, 0, 0};
    const graphics::LineMode modes[3] = {graphics::LineMode::Aliased, graphics::LineMode::QuadrilateralWide, graphics::LineMode::QuadrilateralNarrow};
    for (int m = 0; m < 3; ++m) {
      PipelineDesc pd;
      pd.line_mode = modes[m];
      const std::vector<uint32_t> img = picture(pipeline_for(pd), Topology::LineList);
      for (uint32_t y = 0; y < 16 && img.size() == 256; ++y) rows[m] += (img[y * 16 + 8] & 0xFFFFFF) != 0;
    }
    printf("  line modes: aliased %u rows, quadrilateral 1.4 %u, 1.0 %u\n", rows[0], rows[1], rows[2]);
    CHECK(rows[0] == 1 && rows[1] == 2 && rows[2] == 1);
    // (an alpha-antialiased diagonal over black, blended with the source's alpha: partly covered pixels hold less than the full red, and every lit pixel is near the line)
    const float diagonal[14] = {-0.7f, -0.6f, 0.5f, 1, 0, 0, 1, 0.7f, 0.5f, 0.5f, 1, 0, 0, 1};
    CHECK(e.write(vb, floats(diagonal, 14)));
    PipelineDesc smooth;
    smooth.line_mode = graphics::LineMode::AlphaAntialiased;
    smooth.blend.custom = smooth.blend.enable = true;
    smooth.blend.src_color = graphics::BlendFactor::SrcAlpha, smooth.blend.dst_color = graphics::BlendFactor::InvSrcAlpha;
    smooth.blend.src_alpha = graphics::BlendFactor::One, smooth.blend.dst_alpha = graphics::BlendFactor::Zero;
    const std::vector<uint32_t> img = picture(pipeline_for(smooth), Topology::LineList);
    uint32_t partial = 0, lit = 0;
    for (uint32_t v : img) {
      const uint32_t r = (v >> 16) & 0xFF;
      lit += r != 0, partial += r != 0 && r < 250;
    }
    printf("  alpha-antialiased line: %u pixels, %u partly covered\n", lit, partial);
    CHECK(lit >= 14 && partial >= 8 && img.size() == 256 && (img[0] & 0xFFFFFF) == 0 && (img[15] & 0xFFFFFF) == 0);
  }
  // (the escape's 'LIN1' part: a word of a known mode; twice, or a mode past the last, is refused)
  {
    Handle vs = 0, fs = 0;
    CHECK(e.kernel.create_shader(kPid, e.dev, {ShaderKind::ColoredVertex, graphics::ShaderStage::Vertex, {}}, &vs) == status::kSuccess);
    CHECK(e.kernel.create_shader(kPid, e.dev, {ShaderKind::ColoredFragment, graphics::ShaderStage::Fragment, {}}, &fs) == status::kSuccess);
    auto request = [&](std::initializer_list<uint32_t> tail) {
      ByteBuffer req(e.allocator);
      req.put32(static_cast<uint32_t>(EscapeOp::CreatePipeline)), req.put32(vs), req.put32(fs), req.put32(28), req.put32(2), req.put32(1), req.put32(0);
      req.put32(0), req.put32(0), req.put32(0);   // cull none, front face, flags
      for (uint32_t w : {0u, 2u, 0u, 1u, 3u, 12u, 0u, 0u, 0u, 0u, 0u, 0u}) req.put32(w);
      for (uint32_t w : {0u, 7u, 0u, 0u, 0u, 255u, 255u, 0u}) req.put32(w);   // stencil off
      for (uint32_t w : {7u, 0u, 0u, 1u, 0u, 1u, 0u, 0u, 0u, 15u, 0u}) req.put32(w);   // depth compare Always, no blending, no per-instance attributes
      for (uint32_t w : {1u, 0u}) req.put32(w);   // one target, no independent blend
      for (int t = 0; t < 3; ++t)
        for (uint32_t w : {0u, 0u, 1u, 0u, 1u, 0u, 0u, 0u, 15u}) req.put32(w);
      for (uint32_t w : {0u, 0u, 0u, 0u}) req.put32(w);   // no multisample, no geometry, hull or domain shader
      for (uint32_t w : {0u, 0u, 0u, 0u, 0u}) req.put32(w);   // no stream output, no more attributes, instance bits, slots, strides
      req.put32(0);   // no more blends
      for (uint32_t w : {0u, 0u, 0u, 0u}) req.put32(w);   // the constant blend colour
      for (uint32_t w : tail) req.put32(w);
      uint8_t reply[16];
      uint32_t reply_bytes = 0;
      return e.kernel.escape(kPid, e.dev, req.view(), ByteView(reply, sizeof reply), &reply_bytes);
    };
    const NtStatus plain = request({});
    if (plain != status::kSuccess) printf("  'LIN1' base request: %x\n", static_cast<unsigned>(plain));
    CHECK(plain == status::kSuccess);
    CHECK(request({kPipelineLineTag, 1, 2}) == status::kSuccess);
    CHECK(request({kPipelineLineTag, 1, 4}) == status::kInvalidParameter);
    CHECK(request({kPipelineLineTag, 2, 1, 1}) == status::kInvalidParameter);
    CHECK(request({kPipelineLineTag, 1, 1, kPipelineLineTag, 1, 1}) == status::kInvalidParameter);
  }
  // (a device without the line modes, the separate filters or the reductions refuses pipelines and bindings that need them)
  {
    FakeScreen screen2(8, 6);
    graphics::SoftwareDevice inner(e.allocator, &screen2);
    SpyDevice plain(inner);
    plain.feature2_mask = ~mine;
    Kernel limited(e.allocator);
    AdapterConfig c2;
    c2.display = &screen2;
    CHECK(limited.add_adapter(plain, c2) == 0);
    Handle a = 0, d = 0, vs = 0, fs = 0, pipe = 0;
    CHECK(limited.open_process(kPid) == status::kSuccess && limited.open_adapter_from_index(kPid, 0, &a) == status::kSuccess && limited.create_device(kPid, a, &d) == status::kSuccess);
    CHECK(limited.create_shader(kPid, d, {ShaderKind::ColoredVertex, graphics::ShaderStage::Vertex, {}}, &vs) == status::kSuccess);
    CHECK(limited.create_shader(kPid, d, {ShaderKind::ColoredFragment, graphics::ShaderStage::Fragment, {}}, &fs) == status::kSuccess);
    PipelineDesc pd;
    graphics::GraphicsPipelineDesc gd;
    graphics::describe_colored_pipeline(&gd);
    pd.vertex_shader = vs, pd.fragment_shader = fs, pd.vertex_stride = gd.vertex_stride, pd.attribute_count = gd.attribute_count, pd.varying_count = gd.varying_count;
    for (uint32_t i = 0; i < gd.attribute_count; ++i) pd.attributes[i] = gd.attributes[i];
    pd.line_mode = graphics::LineMode::AlphaAntialiased;
    CHECK(limited.create_pipeline(kPid, d, pd, &pipe) == status::kNotSupported);
    pd.line_mode = graphics::LineMode::Aliased;
    CHECK(limited.create_pipeline(kPid, d, pd, &pipe) == status::kSuccess);
  }
  // R1 copies: two rows of 12 pixels (2 bytes each, the low nibble of the second byte past the width) into an R8Unorm image and an A8B8G8R8 one, and back
  {
    Handle r8 = e.alloc(kTexture, Format::R8Unorm, 12, 2), wide = e.alloc(kTexture, Format::A8B8G8R8, 12, 2);
    Handle bits = e.buffer(kAllocVertexBuffer, 64), back = e.buffer(kAllocVertexBuffer, 64);
    std::vector<uint8_t> rows(64, 0);
    rows[0] = 0xA5, rows[1] = 0xF3, rows[8] = 0x01, rows[9] = 0x80;
    CHECK(e.write(bits, ConstByteView(rows.data(), rows.size())));
    std::vector<uint8_t> zeros(64, 0x0F);
    CHECK(e.write(back, ConstByteView(zeros.data(), zeros.size())));
    for (int k = 0; k < 2; ++k) {
      CommandWriter w(e.allocator);
      w.copy_buffer_image_narrow(0, 0, 0, 0, 0, 0, 12, 2, 1, 0, 8, NarrowLayout::R1);
      CHECK(e.render(w, {k ? wide : r8, bits}) == status::kSuccess);
    }
    LockResult lr;
    if (e.kernel.lock(kPid, r8, kLockReadOnly, 0, &lr) == status::kSuccess) {
      printf("  R1 into R8Unorm: %02x %02x %02x %02x, pitch %u\n", lr.data[0], lr.data[1], lr.data[2], lr.data[3], static_cast<unsigned>(lr.pitch));
      // (0xA5: 1 0 1 0 0 1 0 1; 0xF3: 1 1 1 1; row 1: 0x01 - pixel 7 -, 0x80 - pixel 8)
      CHECK(lr.data[0] == 0xFF && lr.data[1] == 0 && lr.data[2] == 0xFF && lr.data[5] == 0xFF && lr.data[8] == 0xFF && lr.data[11] == 0xFF);
      CHECK(lr.data[lr.pitch + 7] == 0xFF && lr.data[lr.pitch + 8] == 0xFF && lr.data[lr.pitch + 6] == 0 && lr.data[lr.pitch + 9] == 0);
      e.kernel.unlock(kPid, r8);
    } else CHECK(0);
    CHECK(e.pixel(wide, 0, 0) == 0xFF0000FFu && e.pixel(wide, 1, 0) == 0xFF000000u && e.pixel(wide, 8, 1) == 0xFF0000FFu);
    {
      CommandWriter w(e.allocator);
      w.copy_buffer_image_narrow(1, 0, 0, 0, 0, 0, 12, 2, 1, 0, 8, NarrowLayout::R1);
      CHECK(e.render(w, {r8, back}) == status::kSuccess);
    }
    auto byte_at = [&](uint32_t i) {
      LockResult b;
      uint8_t v = 0;
      if (e.kernel.lock(kPid, back, kLockReadOnly, 0, &b) == status::kSuccess) {
        v = b.data[i];
        e.kernel.unlock(kPid, back);
      }
      return v;
    };
    // (the bits as they were; the four past the width in the second byte keep the buffer's 0xF)
    CHECK(byte_at(0) == 0xA5 && byte_at(1) == 0xFF && byte_at(8) == 0x01 && byte_at(9) == 0x8F);
    // (refused: an R1 copy of an image of another format, a pitch too short for the bits)
    Handle r16 = e.alloc(kTexture, Format::R16Unorm, 12, 2);
    CommandWriter w(e.allocator);
    w.copy_buffer_image_narrow(0, 0, 0, 0, 0, 0, 12, 2, 1, 0, 8, NarrowLayout::R1);
    CHECK(e.render(w, {r16, bits}) == status::kInvalidParameter);
    CommandWriter w2(e.allocator);
    w2.copy_buffer_image_narrow(0, 0, 0, 0, 0, 0, 12, 2, 1, 0, 1, NarrowLayout::R1);
    CHECK(e.render(w2, {r8, bits}) == status::kInvalidParameter);
  }
  // Mirrors of the narrow families: an R8Unorm image shown as R8Uint (the bytes as they are, no alpha), written back; R16Unorm as R16Float; another family refused; a mirror of
  // two levels of a texture of three, each of its levels the source's
  {
    VidMm& mm = e.kernel.adapter(0)->vidmm();
    auto make = [&](uint32_t flags, Format f, uint32_t w, uint32_t h, uint32_t mips) {
      AllocationDesc d;
      d.flags = flags, d.format = f, d.width = w, d.height = h, d.mip_levels = mips;
      SharedPointer<Allocation> a;
      CHECK(mm.create(d, &a) == status::kSuccess);
      return a;
    };
    SharedPointer<Allocation> r8 = make(kAllocTexture, Format::R8Unorm, 4, 2, 1), as_uint = make(kAllocTexture | kAllocRenderTarget, Format::R8Uint, 4, 2, 1);
    SharedPointer<Allocation> r16 = make(kAllocTexture, Format::R16Unorm, 4, 2, 1), as_half = make(kAllocTexture, Format::R16Float, 4, 2, 1);
    SharedPointer<Allocation> levels = make(kAllocTexture, Format::R8G8Unorm, 8, 4, 3), two = make(kAllocTexture, Format::R8G8Snorm, 4, 2, 2);
    if (!r8 || !as_uint || !r16 || !as_half || !levels || !two) return;
    ByteView b = r8->writable_backing();
    for (uint64_t i = 0; i < b.size(); ++i) b[i] = static_cast<uint8_t>(200 + i);
    mm.note_cpu_write(*r8);
    const SharedPointer<Allocation> one[1] = {r8}, other[1] = {r16};
    CHECK(mm.set_mirror(*as_uint, ArrayView<const SharedPointer<Allocation>>(other, 1), 0, 0) == status::kInvalidParameter);   // (R16 bytes for an R8 view)
    CHECK(mm.set_mirror(*as_uint, ArrayView<const SharedPointer<Allocation>>(one, 1), kMirrorWriteBack, 1) == status::kSuccess);
    CHECK(mm.refresh_mirror(*as_uint, true) && as_uint->backing().size() == 8 && as_uint->backing()[0] == 200 && as_uint->backing()[7] == 207);
    ByteView drawn = as_uint->writable_backing();
    drawn[3] = 9;
    mm.note_cpu_write(*as_uint);
    CHECK(mm.write_back_mirror(*as_uint) && r8->backing()[3] == 9 && r8->backing()[2] == 202);
    ByteView h = r16->writable_backing();
    for (uint64_t i = 0; i < h.size(); ++i) h[i] = static_cast<uint8_t>(i);
    mm.note_cpu_write(*r16);
    CHECK(mm.set_mirror(*as_half, ArrayView<const SharedPointer<Allocation>>(other, 1), 0, 0) == status::kSuccess && mm.refresh_mirror(*as_half, true) && as_half->backing()[15] == 15);
    ByteView l = levels->writable_backing();
    for (uint64_t i = 0; i < l.size(); ++i) l[i] = static_cast<uint8_t>(i);
    mm.note_cpu_write(*levels);
    const SharedPointer<Allocation> twice[2] = {levels, levels};
    const uint32_t subresources[2] = {1, 2};      // (levels 1 and 2 of the source, layer 0: the mirror's levels 0 and 1)
    CHECK(mm.set_mirror(*two, ArrayView<const SharedPointer<Allocation>>(twice, 1), 0, 0) == status::kInvalidParameter);   // (one source for two levels)
    CHECK(mm.set_mirror(*two, ArrayView<const SharedPointer<Allocation>>(twice, 2), 0, 0) == status::kInvalidParameter);   // (levels need the subresources)
    CHECK(mm.set_mirror(*two, ArrayView<const SharedPointer<Allocation>>(twice, 2), 0, 0, ArrayView<const uint32_t>(subresources, 2)) == status::kSuccess);
    CHECK(mm.refresh_mirror(*two, true));
    uint64_t at = 0, bytes = 0, src_at = 0, src_bytes = 0;
    CHECK(two->subresource_range(1, 0, &at, &bytes) && levels->subresource_range(2, 0, &src_at, &src_bytes) && bytes == src_bytes && bytes == 2 * 1 * 2);
    CHECK(two->backing()[0] == levels->backing()[64] && two->backing()[at] == levels->backing()[src_at] && two->backing()[at + 3] == levels->backing()[src_at + 3]);
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
    if (!(r.device_picture && r.reference_picture && !r.reference_failed)) printf("  conformance scene failed: %s (%s) %x dev=%d ref=%d rf=%d\n", r.name, r.step, static_cast<unsigned>(r.status), !!r.device_picture, !!r.reference_picture, !!r.reference_failed);
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
    if (r.device_picture && getenv("DXGK_DUMP_SCENE") && __builtin_strstr(r.name, getenv("DXGK_DUMP_SCENE"))) {
      // (DXGK_DUMP_SCENE=<part of a name>: the scene's picture, a pixel a word)
      printf("  %s:\n", r.name);
      for (uint32_t y = 0; y < 32; ++y) {
        for (uint32_t x = 0; x < 32; ++x) printf("%08x ", r.device_picture[y * 32 + x]);
        printf("\n");
      }
    }
    if (r.device_picture) {
      // The rasterizer, depth-stencil and blend state of Direct3D 10 / 11 (Conformance.cpp says what each scene draws).
      auto px = [&](uint32_t x, uint32_t y) { return r.device_picture[y * 32 + x] & 0xFFFFFF; };
      auto argb = [&](uint32_t x, uint32_t y) { return r.device_picture[y * 32 + x]; };
      auto count = [&](uint32_t colour) {
        uint32_t n = 0;
        for (uint32_t i = 0; i < 32 * 32; ++i) n += (r.device_picture[i] & 0xFFFFFF) == colour;
        return n;
      };
      // (the scenes' vertex shaders have +y up: a clip-space y of -1 is the bottom row, so the rows the scenes describe are counted from the bottom here)
      auto at = [&](uint32_t x, uint32_t y_from_bottom) { return px(x, 31 - y_from_bottom); };
      auto at_argb = [&](uint32_t x, uint32_t y_from_bottom) { return argb(x, 31 - y_from_bottom); };
      if (!__builtin_strcmp(r.name, "Direct3D 10 depth bias, float format")) {
        for (uint32_t x : {1u, 16u, 30u}) CHECK(at(x, 3) == 0x00FF00 && at(x, 11) == 0xFF0000 && at(x, 19) == 0x0000FF && at(x, 27) == 0xFF0000);
      }
      if (!__builtin_strcmp(r.name, "Direct3D 10 depth bias, normalised formats")) {
        for (uint32_t x : {1u, 16u, 30u}) CHECK(at(x, 3) == 0xFF0000 && at(x, 9) == 0x00FF00 && at(x, 16) == 0x00FF00 && at(x, 22) == 0xFF0000 && at(x, 29) == 0x0000FF);
      }
      if (!__builtin_strcmp(r.name, "Direct3D 10 two-sided stencil")) {
        // (the left triangle faces away: its stencil is inverted to 0x0F, which the back faces' test finds; the right one's is 1)
        CHECK(at(4, 15) == 0xFFFFFF && at(28, 15) == 0x00FF00 && at(16, 2) == 0 && at(16, 30) == 0);
      }
      if (!__builtin_strcmp(r.name, "Direct3D 10 wireframe")) {
        // (edges only: the triangles' insides stay black; the third triangle faces away and is culled)
        CHECK(count(0xFF0000) >= 30 && count(0x00FF00) >= 30 && count(0x0000FF) == 0 && at(6, 15) == 0 && at(26, 15) == 0);
      }
      if (!__builtin_strcmp(r.name, "Direct3D 10 depth clip and clamp")) {
        // (left: cut where the depth passes 1 or 0 - at half the height of each triangle; right: all of it)
        CHECK(at(8, 3) == 0xFF0000 && at(8, 13) == 0 && at(24, 3) == 0x00FF00 && at(24, 13) == 0x00FF00);
        CHECK(at(8, 19) == 0x0000FF && at(8, 29) == 0 && at(24, 19) == 0xFFFF00 && at(24, 29) == 0xFFFF00);
      }
      if (!__builtin_strcmp(r.name, "Direct3D 10 dual-source blending (DXBC)")) {
        // (0.6, 0.2, 0.1) + 0.5 * (0.1, 0.2, 0.6) = (0.65, 0.3, 0.4); alpha 1 + 0.5 * (1 - 1)
        const uint32_t c = argb(16, 16);
        CHECK(close8(c >> 16 & 0xFF, 166) && close8(c >> 8 & 0xFF, 77) && close8(c & 0xFF, 102) && (c >> 24) == 0xFF);
        CHECK(argb(1, 30) == 0xFF808080u);
      }
      if (!__builtin_strcmp(r.name, "Direct3D 10 border colours (DXBC)")) {
        // (the corners are the border's colour, (0.2, 0.4, 0.6) - point filtered on the left, linear on the right; the middle of each half is the checker)
        CHECK(px(1, 1) == 0x336699 && px(14, 30) == 0x336699 && px(17, 1) == 0x336699 && px(30, 30) == 0x336699);
        CHECK(px(8, 16) != 0x336699 && px(24, 16) != 0x336699 && px(8, 16) != 0);
      }
      if (!__builtin_strcmp(r.name, "Direct3D 10 separate minification and magnification filters (DXBC)")) {
        // (the checker, magnified: blended between its texels on the left - colours other than its own -, its texels' own colours on the right)
        uint32_t blends = 0, own = 0;
        for (uint32_t y = 2; y < 30; ++y)
          for (uint32_t x = 1; x < 15; ++x) blends += px(x, y) != 0xFFA020 && px(x, y) != 0x2060F0 && px(x, y) != 0xFFFFFF;
        for (uint32_t y = 0; y < 32; ++y)
          for (uint32_t x = 17; x < 32; ++x) own += px(x, y) == 0xFFA020 || px(x, y) == 0x2060F0 || px(x, y) == 0xFFFFFF;
        CHECK(blends > 100 && own == 32 * 15);
      }
      if (!__builtin_strcmp(r.name, "Direct3D 11.1 minimum and maximum filters (DXBC)")) {
        // (between texels of the two colours: their greatest channels (ff a0 f0) on the left, their least (20 60 20) on the right)
        uint32_t greatest = 0, least = 0;
        for (uint32_t i = 0; i < 32 * 32; ++i) greatest += (i % 32) < 16 && px(i % 32, i / 32) == 0xFFA0F0, least += (i % 32) >= 16 && px(i % 32, i / 32) == 0x206020;
        CHECK(greatest > 50 && least > 50);
      }
      if (!__builtin_strcmp(r.name, "Direct3D 12 non-normalized sampler coordinates (DXBC)")) {
        // (the right half - coordinates in texels - the same picture as the left)
        uint32_t same = 0;
        for (uint32_t y = 0; y < 32; ++y)
          for (uint32_t x = 0; x < 16; ++x) same += px(x, y) == px(x + 16, y);
        CHECK(same >= 32 * 16 - 32);
      }
      if (!__builtin_strcmp(r.name, "Direct3D 12 index buffer strip cut")) {
        // (left: two triangles at the top, nothing below them; right: the strip reaches vertex 65535 at the bottom)
        CHECK(px(4, 2) == 0xFF0000 && px(12, 2) == 0x00FF00 && px(8, 20) == 0 && px(24, 20) != 0);
      }
      if (!__builtin_strcmp(r.name, "Direct3D 10 several viewports and SV_ViewportArrayIndex (DXBC)")) {
        // (red in the left half above row 20 - its scissor rectangle -, green in the right half, blue in the middle of the left half; nothing in the left half below the scissor)
        CHECK(px(2, 2) == 0xFF0000 && px(29, 2) == 0x00FF00 && px(29, 29) == 0x00FF00 && px(8, 14) == 0x0000FF && px(2, 28) == 0 && px(14, 25) == 0);
      }
      if (!__builtin_strcmp(r.name, "Direct3D 11.1 logic operations")) {
        // clear (33 66 99 ff); XOR (ff 66 00 ff) -> (cc 00 99 00); OR_INVERTED of (33 ff cc ff) on the right; SET of green and alpha in the corner
        CHECK(at_argb(4, 4) == 0x00CC0099u);       // (XOR alone)
        CHECK(at_argb(29, 20) == 0xFFFF66BBu);     // (OR_INVERTED over the clear colour: ff 66 bb ff, as ARGB ff ff 66 bb)
        CHECK(at_argb(20, 4) == 0x00CC00BBu);      // (OR_INVERTED over the XOR: cc 00 bb 00)
        CHECK(at_argb(2, 29) == 0xFF33FF99u);      // (SET of green and alpha over the clear colour)
        CHECK(at_argb(16, 1) == 0xFF336699u);      // (nothing drawn)
      }
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
        graphics::Vec4 varyings[16] = {}, c = {};
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
        graphics::Vec4 varyings[16] = {}, c = {};
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
      CHECK(kernel.query_adapter_caps(kPid, adapter, &caps) == status::kSuccess && caps.max_push_constant_bytes == graphics::kMaxPushConstantBytes);
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
        graphics::Vec4 varyings[16] = {}, c = {};
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
        graphics::Vec4 varyings[16] = {}, c = {};
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
        graphics::Vec4 varyings[16], c = {};
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
        graphics::Vec4 varyings[16] = {}, c = {};
        p->run_fragment(varyings, constants, nullptr, &c);
        const uint32_t got[4] = {__builtin_bit_cast(uint32_t, c.x), __builtin_bit_cast(uint32_t, c.y), __builtin_bit_cast(uint32_t, c.z), __builtin_bit_cast(uint32_t, c.w)};
        CHECK(got[0] == 100 / 7 && got[1] == 1 && got[2] == 0xFFFFFFFFu / 2 && got[3] == 0xFFFFFFFFu);   // (x / 0 is all ones)
      }
      // (the high word of a product: umul r5 (high), r6 (low))
      DxbcAsm high = dx_pixel_shader();
      high.ins(54, {dx_dst(kOpTemp, 1, 0xF), dx_imm_bits(100, 7, 0xFFFFFFFFu, 9)});
      high.ins(54, {dx_dst(kOpTemp, 2, 0xF), dx_imm_bits(7, 7, 2, 0)});
      high.ins(81, {dx_dst(kOpTemp, 5, 0xF), dx_dst(kOpTemp, 6, 0xF), dx_src(kOpTemp, 1), dx_src(kOpTemp, 2)});
      high.ins(54, {dx_dst(kOpOut, 0, 0xF), dx_src(kOpTemp, 5)});
      high.ins(62, {});
      DxbcAsm wrapped = high;
      wrapped.code.insert(wrapped.code.begin(), {104u | (2u << 24), 8u});
      UniquePointer<D3d9Program> hp = compile_dxbc(allocator, wrapped.build(), &st);
      CHECK(st == status::kSuccess && hp);
      if (hp) {
        uint8_t constants[128] = {};
        graphics::Vec4 varyings[16] = {}, c = {};
        hp->run_fragment(varyings, constants, nullptr, &c);
        const uint32_t got[4] = {__builtin_bit_cast(uint32_t, c.x), __builtin_bit_cast(uint32_t, c.y), __builtin_bit_cast(uint32_t, c.z), __builtin_bit_cast(uint32_t, c.w)};
        CHECK(got[0] == 0 && got[1] == 0 && got[2] == 1 && got[3] == 0);
      }
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
      graphics::Vec4 varyings[16] = {};
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


// The rasterizer state of Direct3D 10+ (kFeature2RasterizerState), dual-source blending and logic operations: what is refused, by a device without them and as values, and the
// escape's tagged part (the same pipeline through the escape draws the same picture). The pictures themselves are the conformance scenes'.
void test_rasterizer_state() {
  Env e;
  auto shaders = [](Kernel& kernel, Handle dev, PipelineDesc* pd) {
    Handle vs = 0, fs = 0;
    CHECK(kernel.create_shader(kPid, dev, {ShaderKind::ColoredVertex, graphics::ShaderStage::Vertex, {}}, &vs) == status::kSuccess);
    CHECK(kernel.create_shader(kPid, dev, {ShaderKind::ColoredFragment, graphics::ShaderStage::Fragment, {}}, &fs) == status::kSuccess);
    graphics::GraphicsPipelineDesc gd;
    graphics::describe_colored_pipeline(&gd);
    pd->vertex_shader = vs, pd->fragment_shader = fs, pd->vertex_stride = gd.vertex_stride, pd->attribute_count = 2, pd->varying_count = 1;
    pd->attributes[0] = gd.attributes[0], pd->attributes[1] = gd.attributes[1];
    pd->cull_mode = graphics::CullMode::None;
  };
  auto make = [&](Kernel& kernel, Handle dev, PipelineDesc pd) {
    shaders(kernel, dev, &pd);
    Handle h = 0;
    return kernel.create_pipeline(kPid, dev, pd, &h);
  };
  PipelineDesc wire, bias, clamp, two_sided, logic, dual;
  wire.fill_mode = graphics::FillMode::Wireframe;
  bias.depth_bias.constant = 4, bias.depth_bias.slope = 1.5f, bias.depth_bias.unorm_bits = 24;
  clamp.depth_clip = false;
  two_sided.separate_back_stencil = true, two_sided.back_stencil.pass_op = graphics::StencilOp::Invert;
  logic.logic_op_enable = true, logic.logic_op = graphics::LogicOp::Xor;
  dual.blend.custom = dual.blend.enable = true, dual.blend.dst_color = graphics::BlendFactor::InvSrc1Color;
  for (const PipelineDesc& pd : {wire, bias, clamp, two_sided, logic, dual}) CHECK(make(e.kernel, e.dev, pd) == status::kSuccess);
  // (values that are none)
  PipelineDesc bad = bias;
  bad.depth_bias.unorm_bits = 8;
  CHECK(make(e.kernel, e.dev, bad) == status::kInvalidParameter);
  bad = logic, bad.logic_op = static_cast<graphics::LogicOp>(16);
  CHECK(make(e.kernel, e.dev, bad) == status::kInvalidParameter);
  bad = dual, bad.color_target_count = 2;   // (dual-source blending into more than one target)
  CHECK(make(e.kernel, e.dev, bad) == status::kInvalidParameter);
  bad = bias, bad.depth_bias.slope = __builtin_nanf("");
  CHECK(make(e.kernel, e.dev, bad) == status::kInvalidParameter);
  // A device without the state refuses each of them, and makes the pipelines that do not use it.
  {
    FakeScreen screen(8, 6);
    graphics::SoftwareDevice software(e.allocator, &screen);
    SpyDevice limited(software);
    limited.feature2_mask = ~(graphics::kFeature2RasterizerState | graphics::kFeature2DualSourceBlend | graphics::kFeature2LogicOps);
    Kernel kernel(e.allocator);
    AdapterConfig c;
    c.display = &screen;
    kernel.add_adapter(limited, c);
    Handle adapter = 0, dev = 0;
    CHECK(kernel.open_process(kPid) == status::kSuccess && kernel.open_adapter_from_index(kPid, 0, &adapter) == status::kSuccess && kernel.create_device(kPid, adapter, &dev) == status::kSuccess);
    for (const PipelineDesc& pd : {wire, bias, clamp, two_sided, logic, dual}) CHECK(make(kernel, dev, pd) == status::kNotSupported);
    PipelineDesc off = dual;
    off.blend.enable = false;   // (blending off: the factors are not used)
    CHECK(make(kernel, dev, off) == status::kSuccess && make(kernel, dev, PipelineDesc()) == status::kSuccess);
  }
  // The escape's tagged part: a wireframe, XOR pipeline drawn both ways gives the same picture, which differs from the plain one.
  {
    Handle rt = e.alloc(kTarget, Format::A8R8G8B8, 16, 16), vb = e.buffer(kAllocVertexBuffer, 256);
    const float tri[21] = {-0.8f, -0.7f, 0.5f, 1, 0.4f, 0, 1, 0.7f, -0.6f, 0.5f, 1, 0.4f, 0, 1, -0.1f, 0.8f, 0.5f, 1, 0.4f, 0, 1};
    CHECK(e.write(vb, floats(tri, 21)));
    const float identity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    auto picture = [&](Handle pipe) {
      CommandWriter w(e.allocator);
      w.begin_pass(0, kNoAllocation, kPassClearColor, {0.2f, 0.4f, 0.6f, 1.0f}, 1, 0);
      w.bind_pipeline(pipe);
      w.bind_vertex_buffer(1);
      w.push_constants(floats(identity, 16));
      w.draw(3);
      w.end_pass();
      CHECK(e.render(w, {rt, vb}) == status::kSuccess);
      std::vector<uint32_t> out;
      for (uint32_t y = 0; y < 16; ++y)
        for (uint32_t x = 0; x < 16; ++x) out.push_back(e.pixel(rt, x, y));
      return out;
    };
    PipelineDesc both = wire;
    both.logic_op_enable = true, both.logic_op = graphics::LogicOp::Xor;
    shaders(e.kernel, e.dev, &both);
    Handle direct = 0;
    CHECK(e.kernel.create_pipeline(kPid, e.dev, both, &direct) == status::kSuccess);
    ByteBuffer req(e.allocator);
    req.put32(static_cast<uint32_t>(EscapeOp::CreatePipeline)), req.put32(both.vertex_shader), req.put32(both.fragment_shader), req.put32(28), req.put32(2), req.put32(1), req.put32(0);
    req.put32(0), req.put32(0), req.put32(0);   // cull none, front face, flags
    for (uint32_t w : {0u, 2u, 0u, 1u, 3u, 12u, 0u, 0u, 0u, 0u, 0u, 0u}) req.put32(w);
    for (uint32_t w : {0u, 7u, 0u, 0u, 0u, 255u, 255u, 0u}) req.put32(w);                  // stencil off
    for (uint32_t w : {3u, 0u, 0u, 1u, 0u, 1u, 0u, 0u, 0u, 15u, 0u}) req.put32(w);         // depth compare, no blending, no per-instance attributes
    req.put32(1), req.put32(0);                                                                // one colour output, no independent blending
    for (uint32_t t = 0; t < 3; ++t)
      for (uint32_t w : {0u, 0u, 1u, 0u, 1u, 0u, 0u, 0u, 15u}) req.put32(w);
    req.put32(0), req.put32(0), req.put32(0), req.put32(0), req.put32(0);                     // multisample, geometry, hull, domain shaders, no stream output
    req.put32(0), req.put32(0), req.put32(0), req.put32(0), req.put32(0);                     // no more attributes, instances, slots, strides or blends
    for (uint32_t c = 0; c < 4; ++c) req.put32(0);                                            // the constant colour
    const uint64_t tagged = req.view().size();
    req.put32(kPipelineRasterTag), req.put32(kPipelineRasterWords);
    for (uint32_t w : {1u, 1u, 0u, 0u, 0u, 0u, 0u, 7u, 0u, 0u, 0u, 255u, 255u, 1u, static_cast<uint32_t>(graphics::LogicOp::Xor), 0u}) req.put32(w);
    uint8_t reply[16];
    uint32_t reply_bytes = 0;
    CHECK(e.kernel.escape(kPid, e.dev, req.view(), ByteView(reply, sizeof reply), &reply_bytes) == status::kSuccess && reply_bytes == 4);
    const Handle via_escape = ByteReader(ConstByteView(reply, 4)).le32();
    const std::vector<uint32_t> a = picture(via_escape), b = picture(direct);
    PipelineDesc plain;
    shaders(e.kernel, e.dev, &plain);
    Handle plain_pipe = 0;
    CHECK(e.kernel.create_pipeline(kPid, e.dev, plain, &plain_pipe) == status::kSuccess);
    CHECK(!a.empty() && a == b && a != picture(plain_pipe));
    // (the outline only: the middle of the triangle keeps the clear colour)
    CHECK(!a.empty() && (a[8 * 16 + 7] & 0xFFFFFF) == 0x336699);
    // A part of the wrong size, an unknown tag and a value that is none are refused.
    auto patched = [&](uint64_t offset, uint32_t value) {
      ByteBuffer copy(e.allocator);
      CHECK(copy.append(req.view()) && copy.patch32(offset, value));
      return e.kernel.escape(kPid, e.dev, copy.view(), ByteView(reply, sizeof reply), &reply_bytes);
    };
    CHECK(patched(tagged + 4, kPipelineRasterWords - 1) == status::kInvalidParameter);
    CHECK(patched(tagged, 0x12345678u) == status::kInvalidParameter);
    CHECK(patched(tagged + 8 + 5 * 4, 12) == status::kInvalidParameter);   // (the depth format's bits: 12)
    CHECK(patched(tagged + 8 + 14 * 4, 16) == status::kInvalidParameter);  // (logic operation 16)
    CHECK(patched(tagged + 8 + 15 * 4, 256) == status::kInvalidParameter); // (a back stencil reference of 256)
  }
}

// SV_ViewportArrayIndex and SV_RenderTargetArrayIndex written by a vertex shader (vs_viewport.dxbc: the colour's alpha says them), kFeature2LayerFromAnyStage; a device without it refuses the
// shader, and one without several viewports a stream that sets them.
void test_viewports_from_vertex_shader() {
  Env e;
  const std::vector<uint8_t> vs_code = read_blob("vs_viewport.dxbc"), ps_code = read_blob("ps_color.dxbc");
  CHECK(!vs_code.empty() && !ps_code.empty());
  Handle vs = 0, ps = 0, pipe = 0;
  CHECK(e.kernel.create_shader(kPid, e.dev, {ShaderKind::Dxbc, graphics::ShaderStage::Vertex, ConstByteView(vs_code.data(), vs_code.size())}, &vs) == status::kSuccess);
  CHECK(e.kernel.create_shader(kPid, e.dev, {ShaderKind::Dxbc, graphics::ShaderStage::Fragment, ConstByteView(ps_code.data(), ps_code.size())}, &ps) == status::kSuccess);
  PipelineDesc pd;
  pd.vertex_shader = vs, pd.fragment_shader = ps, pd.vertex_stride = 28, pd.attribute_count = 2, pd.varying_count = 3;
  pd.attributes[0] = {0, graphics::VertexFormat::Float3, 0};
  pd.attributes[1] = {1, graphics::VertexFormat::Float4, 12};
  pd.cull_mode = graphics::CullMode::None;
  CHECK(e.kernel.create_pipeline(kPid, e.dev, pd, &pipe) == status::kSuccess);
  // Two layers of 16 x 16; viewports: the left and the right half. A red triangle to viewport 0 layer 0 (alpha 0), a green one to viewport 1 layer 0 (alpha 0.5), a blue one to
  // viewport 1 layer 1 (alpha 0.625).
  AllocationDesc ad;
  ad.flags = kAllocTexture | kAllocRenderTarget, ad.format = Format::A8R8G8B8, ad.width = 16, ad.height = 16, ad.kind = graphics::TextureKind::Texture2DArray, ad.layers = 2;
  Handle rt = 0;
  CHECK(e.kernel.create_allocation(kPid, e.dev, ad, &rt) == status::kSuccess);
  Handle vb = e.buffer(kAllocVertexBuffer, 256);
  const float tri[63] = {-1, -1, 0.5f, 1, 0, 0, 0, 3, -1, 0.5f, 1, 0, 0, 0, -1, 3, 0.5f, 1, 0, 0, 0,
                         -1, -1, 0.5f, 0, 1, 0, 0.5f, 3, -1, 0.5f, 0, 1, 0, 0.5f, -1, 3, 0.5f, 0, 1, 0, 0.5f,
                         -1, -1, 0.5f, 0, 0, 1, 0.625f, 3, -1, 0.5f, 0, 0, 1, 0.625f, -1, 3, 0.5f, 0, 0, 1, 0.625f};
  CHECK(e.write(vb, floats(tri, 63)));
  const float identity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
  const graphics::Viewport viewports[2] = {{0, 0, 8, 16, 0, 1}, {8, 0, 8, 16, 0, 1}};
  CommandWriter w(e.allocator);
  w.begin_pass_layers(0, {}, kNoAllocation, kPassClearColor, {0, 0, 0, 1}, 1, 0, {0, 2, 0, 1});
  w.bind_pipeline(pipe);
  w.bind_vertex_buffer(1);
  w.push_constants(floats(identity, 16));
  w.set_viewports(ArrayView<const graphics::Viewport>(viewports, 2));
  w.draw(9);
  w.end_pass();
  CHECK(w.ok() && e.render(w, {rt, vb}) == status::kSuccess);
  // (the layers one after the other in the allocation's memory, 16 x 16 pixels each)
  auto at = [&](uint32_t layer, uint32_t x, uint32_t y) {
    LockResult l;
    uint32_t out = 0xDEADBEEFu;
    if (e.kernel.lock(kPid, rt, kLockReadOnly, 0, &l) == status::kSuccess) {
      ByteReader in(l.data.subview(uint64_t{layer} * 16 * 16 * 4 + (uint64_t{y} * 16 + x) * 4, 4));
      out = in.le32() & 0xFFFFFF;
      e.kernel.unlock(kPid, rt);
    }
    return out;
  };
  if (!(at(0, 3, 8) == 0xFF0000 && at(0, 12, 8) == 0x00FF00 && at(1, 3, 8) == 0 && at(1, 12, 8) == 0x0000FF))
    printf("  viewports from the vertex shader: %06x %06x / %06x %06x\n", at(0, 3, 8), at(0, 12, 8), at(1, 3, 8), at(1, 12, 8));
  CHECK(at(0, 3, 8) == 0xFF0000 && at(0, 12, 8) == 0x00FF00 && at(1, 3, 8) == 0 && at(1, 12, 8) == 0x0000FF);
  // A device without the layer from the vertex shader refuses it; one without several viewports, a stream that sets two.
  FakeScreen screen(8, 6);
  graphics::SoftwareDevice software(e.allocator, &screen);
  SpyDevice limited(software);
  limited.feature2_mask = ~(graphics::kFeature2LayerFromAnyStage | graphics::kFeature2Viewports);
  Kernel kernel(e.allocator);
  AdapterConfig c;
  c.display = &screen;
  kernel.add_adapter(limited, c);
  Handle adapter = 0, dev = 0, ctx = 0, shader = 0;
  CHECK(kernel.open_process(kPid) == status::kSuccess && kernel.open_adapter_from_index(kPid, 0, &adapter) == status::kSuccess && kernel.create_device(kPid, adapter, &dev) == status::kSuccess);
  CHECK(kernel.create_context(kPid, dev, ContextPriority::Normal, &ctx) == status::kSuccess);
  CHECK(kernel.create_shader(kPid, dev, {ShaderKind::Dxbc, graphics::ShaderStage::Vertex, ConstByteView(vs_code.data(), vs_code.size())}, &shader) == status::kNotSupported);
}

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

// --- Vertex formats and instance step rates -------------------------------------------------------------------------------------------------------------------------------------

// What every VertexFormat reads (graphics::read_vertex_attribute: the software and Vega devices), and what a buffer that ends early gives.
void test_vertex_fetch_formats() {
  using graphics::VertexFormat;
  auto bits = [](float f) { return __builtin_bit_cast(uint32_t, f); };
  auto read = [](std::vector<uint8_t> bytes, VertexFormat f, uint64_t offset = 0) { return graphics::read_vertex_attribute(ConstByteView(bytes.data(), bytes.size()), offset, f); };
  auto near = [](float a, float b) { return fabsf(a - b) < 1e-5f; };
  // R10G10B10A2: R = 1023, G = 512, B = 1, A = 2
  const uint32_t word = 1023u | 512u << 10 | 1u << 20 | 2u << 30;
  const std::vector<uint8_t> w = {static_cast<uint8_t>(word), static_cast<uint8_t>(word >> 8), static_cast<uint8_t>(word >> 16), static_cast<uint8_t>(word >> 24)};
  graphics::Vec4 v = read(w, VertexFormat::Rgb10A2Uint);
  CHECK(bits(v.x) == 1023 && bits(v.y) == 512 && bits(v.z) == 1 && bits(v.w) == 2);
  CHECK(graphics::vertex_format_is_integer(VertexFormat::Rgb10A2Uint) && !graphics::vertex_format_is_integer(VertexFormat::Rgb10A2Unorm) && !graphics::vertex_format_is_integer(VertexFormat::Rg11B10Float));
  v = read(w, VertexFormat::Rgb10A2Unorm);
  CHECK(near(v.x, 1.0f) && near(v.y, 512.0f / 1023.0f) && near(v.z, 1.0f / 1023.0f) && near(v.w, 2.0f / 3.0f));
  // R11G11B10: R = 1.0 (exponent 15, mantissa 0), G = 0.5 + 0.5 / 64 * 32 = 0.75, B = 2.0 (a 10-bit one: exponent 16)
  const uint32_t r11 = 15u << 6, g11 = 14u << 6 | 32u, b10 = 16u << 5;
  const uint32_t packed = r11 | g11 << 11 | b10 << 22;
  v = read({static_cast<uint8_t>(packed), static_cast<uint8_t>(packed >> 8), static_cast<uint8_t>(packed >> 16), static_cast<uint8_t>(packed >> 24)}, VertexFormat::Rg11B10Float);
  CHECK(v.x == 1.0f && v.y == 0.75f && v.z == 2.0f && v.w == 1.0f);
  // (the R11G11B10 value Wine's d3d11 test_vertex_formats expects: 1.89453125e-01, 1.3e+01, 3.8125)
  {
    const uint32_t wine = (12u << 6 | 33u) | (18u << 6 | 40u) << 11 | (16u << 5 | 29u) << 22;   // 0.125 * (1 + 33 / 64), 8 * (1 + 40 / 64), 2 * (1 + 29 / 32)
    v = read({static_cast<uint8_t>(wine), static_cast<uint8_t>(wine >> 8), static_cast<uint8_t>(wine >> 16), static_cast<uint8_t>(wine >> 24)}, VertexFormat::Rg11B10Float);
    CHECK(near(v.x, 0.1894531f) && near(v.y, 13.0f) && near(v.z, 3.8125f));
  }
  // B8G8R8X8: the fourth byte is not read
  v = read({0x40, 0x80, 0xFF, 0x12}, VertexFormat::Bgrx8Unorm);
  CHECK(v.x == 1.0f && near(v.y, 128.0f / 255.0f) && near(v.z, 64.0f / 255.0f) && v.w == 1.0f);
  v = read({0x40, 0x80, 0xFF, 0x33}, VertexFormat::Bgra8Unorm);
  CHECK(v.x == 1.0f && near(v.w, 0x33 / 255.0f));
  // signed normalised: the most negative value is -1; integers sign extended; missing components 0, 0, 0 and 1 (the integer 1)
  v = read({0x80, 0x81, 0x7F}, VertexFormat::Snorm8x3);
  CHECK(v.x == -1.0f && v.y == -1.0f && v.z == 1.0f && v.w == 1.0f);
  v = read({0x00, 0x80, 0x01, 0x80}, VertexFormat::Snorm16x2);
  CHECK(v.x == -1.0f && near(v.y, -32767.0f / 32767.0f) && v.z == 0.0f && v.w == 1.0f);
  v = read({0xFE, 0xFF}, VertexFormat::Sint16x1);
  CHECK(bits(v.x) == 0xFFFFFFFEu && bits(v.y) == 0 && bits(v.w) == 1);
  v = read({0xFF, 0x02}, VertexFormat::Uint8x2);
  CHECK(bits(v.x) == 255 && bits(v.y) == 2 && bits(v.z) == 0 && bits(v.w) == 1);
  v = read({0x00, 0x3C, 0x00, 0xC0}, VertexFormat::Half2);   // 1.0, -2.0
  CHECK(v.x == 1.0f && v.y == -2.0f && v.z == 0.0f && v.w == 1.0f);
  v = read({0xFF, 0xFF, 0x00, 0x00}, VertexFormat::Unorm16x2);
  CHECK(v.x == 1.0f && v.y == 0.0f);
  // a buffer that ends inside the element: what is outside reads as 0 (and nothing is read past the end, ASan)
  v = read({0x01, 0x02}, VertexFormat::Rgb10A2Uint);
  CHECK(bits(v.x) == 0 && bits(v.y) == 0 && bits(v.w) == 0);
  v = read({0x00, 0x00, 0x80, 0x3F, 0x00}, VertexFormat::Float2, 2);   // (the second float is cut off)
  CHECK(v.x == 0.0f && v.y == 0.0f);
  v = read({}, VertexFormat::Rg11B10Float, ~uint64_t{0} - 2);
  CHECK(v.x == 0.0f && v.w == 1.0f);
  // every format's size, and that the count covers the new ones
  CHECK(graphics::kVertexFormatCount == 53 && graphics::vertex_format_bytes(VertexFormat::Rgb10A2Uint) == 4 && graphics::vertex_format_bytes(VertexFormat::Rg11B10Float) == 4 &&
        graphics::vertex_format_bytes(VertexFormat::Bgrx8Unorm) == 4 && graphics::vertex_format_bytes(VertexFormat::Half3) == 6 && graphics::vertex_format_bytes(VertexFormat::Sint8x3) == 3);
  // instance_element: first_instance + instance / rate, rate 0 never steps
  graphics::VertexAttribute a{1, VertexFormat::Float2, 0, true};
  CHECK(graphics::instance_element(a, 5, 7) == 12);
  a.step_rate = 3;
  CHECK(graphics::instance_element(a, 5, 7) == 7 && graphics::instance_element(a, 0, 2) == 0);
  a.step_rate = 0;
  CHECK(graphics::instance_element(a, 5, 1000) == 5);
}

// The virgl device's vertex shader copy for the formats its host reads otherwise (VirglVertexPatch.hpp).
void test_virgl_vertex_patch() {
  using graphics::virgl_patch::InputFix;
  const char* text = "VERT\nDCL IN[0]\nDCL IN[1]\nDCL IN[2]\nDCL OUT[0], POSITION\nDCL TEMP[0..3]\nIMM[0] FLT32 {1.0000, 0.0000, 0.0000, 0.0000}\n"
                     "  0: MOV TEMP[0], IN[0]\n  1: ADD TEMP[1], IN[1].xyzw, -IN[2]\n  2: MOV OUT[0], TEMP[0]\n  3: END\n";
  InputFix fixes[3] = {InputFix::None, InputFix::UnpackRgb10A2Uint, InputFix::ForceW1};
  HostAllocator allocator;
  String out(allocator);
  CHECK(graphics::virgl_patch::patch_vertex_inputs(StringView(text), ArrayView<const InputFix>(fixes, 3), &out));
  const std::string s(out.c_str());
  CHECK(s.find("DCL TEMP[0..5]") != std::string::npos && s.find("DCL TEMP[0..3]") == std::string::npos);
  CHECK(s.find("IMM[1] UINT32 {0, 10, 20, 30}\nIMM[2] UINT32 {1023, 1023, 1023, 3}\nIMM[3] UINT32 {0, 0, 0, 1065353216}\n") != std::string::npos);
  CHECK(s.find("  USHR TEMP[4], IN[1].xxxx, IMM[1]\n  AND TEMP[4], TEMP[4], IMM[2]\n  MOV TEMP[5], IN[2]\n  MOV TEMP[5].w, IMM[3].wwww\n  0: MOV TEMP[0], IN[0]\n") != std::string::npos);
  CHECK(s.find("ADD TEMP[1], TEMP[4].xyzw, -TEMP[5]") != std::string::npos);
  // the new instructions come after every declaration
  CHECK(s.find("USHR") > s.find("IMM[3]") && s.find("DCL IN[2]") < s.find("USHR"));
  // nothing to fix: the same text
  InputFix none[3] = {};
  CHECK(graphics::virgl_patch::patch_vertex_inputs(StringView(text), ArrayView<const InputFix>(none, 3), &out) && std::string(out.c_str()) == text);
  // an input read through the address register cannot be followed; a text with no instructions is not a shader
  CHECK(!graphics::virgl_patch::patch_vertex_inputs(StringView("VERT\nDCL IN[0]\nDCL TEMP[0]\n  0: MOV TEMP[0], IN[ADDR[0].x]\n  1: END\n"), ArrayView<const InputFix>(fixes, 3), &out));
  CHECK(!graphics::virgl_patch::patch_vertex_inputs(StringView("VERT\nDCL IN[0]\n"), ArrayView<const InputFix>(fixes, 3), &out));
  CHECK(!graphics::virgl_patch::patch_vertex_inputs(StringView("VERT\nDCL TEMP[x]\n  0: END\n"), ArrayView<const InputFix>(fixes, 3), &out));
  // every prefix of the text (a cut-off shader) and every byte changed: refused or patched, never read past the end (ASan)
  const std::string full(text);
  for (size_t n = 0; n < full.size(); ++n) (void)graphics::virgl_patch::patch_vertex_inputs(StringView(full.data(), n), ArrayView<const InputFix>(fixes, 3), &out);
  for (size_t i = 0; i < full.size(); ++i)
    for (uint32_t bit = 0; bit < 8; ++bit) {
      std::string flipped = full;
      flipped[i] = static_cast<char>(flipped[i] ^ (1u << bit));
      if (flipped[i] == 0) continue;
      (void)graphics::virgl_patch::patch_vertex_inputs(StringView(flipped.c_str()), ArrayView<const InputFix>(fixes, 3), &out);
    }
}

// Draws with the new formats and with step rates other than 1 on the software device, through the kernel (and its escape).
void test_vertex_formats_and_step_rates() {
  Env e;
  Handle rt = e.alloc(kTarget, Format::A8R8G8B8, 16, 16), vb = e.buffer(kAllocVertexBuffer, 512);
  // vs_2_0: the position is v0 + v1 (a per-instance offset), the colour v2
  Asm vs(true, 2, 0);
  vs.dcl(0, 0, kRegInput, 0).dcl(5, 0, kRegInput, 1).dcl(10, 0, kRegInput, 2);
  vs.def(4, 0, 0, 0, 1);
  vs.op(kOpAdd, {Asm::dst(kRegRastOut, 0, 0x7), Asm::src(kRegInput, 0), Asm::src(kRegInput, 1)});
  vs.op(kOpMov, {Asm::dst(kRegRastOut, 0, 0x8), Asm::src(kRegConst, 4)});
  vs.op(kOpMov, {Asm::dst(kRegAttrOut, 0), Asm::src(kRegInput, 2)});
  vs.end();
  const Handle vsh = d3d9_shader(e, vs), psh = d3d9_shader(e, color_ps());
  // slot 0: a triangle over a quarter (per vertex); slot 1 at byte 64: offsets (per instance); slot 2 at byte 128: colours (per instance or per vertex)
  const float triangle[9] = {-1.0f, -1.0f, 0.5f, 0.0f, -1.0f, 0.5f, -1.0f, 0.0f, 0.5f};
  const float offsets[6] = {0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f};   // (0, 0), (1, 0) and (0, 1): three quarters
  auto put = [&](uint32_t at, ConstByteView bytes) {
    std::vector<uint8_t> all(512, 0);
    LockResult r;
    if (e.kernel.lock(kPid, vb, kLockReadOnly, 0, &r) == status::kSuccess) {
      for (uint64_t i = 0; i < r.data.size() && i < all.size(); ++i) all[i] = r.data[i];
      e.kernel.unlock(kPid, vb);
    }
    for (uint64_t i = 0; i < bytes.size(); ++i) all[at + i] = bytes[i];
    CHECK(e.write(vb, ConstByteView(all.data(), all.size())));
  };
  put(0, floats(triangle, 9));
  put(64, floats(offsets, 6));
  auto make = [&](graphics::VertexFormat colour, bool colour_per_instance, uint32_t offset_rate, uint32_t colour_rate, uint32_t colour_stride, NtStatus* status_out = nullptr) {
    PipelineDesc pd;
    pd.vertex_shader = vsh, pd.fragment_shader = psh, pd.vertex_stride = 12, pd.attribute_count = 3, pd.varying_count = 1;
    pd.attributes[0] = {0, graphics::VertexFormat::Float3, 0};
    pd.attributes[1] = {1, graphics::VertexFormat::Float2, 0, true, 1, offset_rate};
    pd.attributes[2] = {2, colour, 0, colour_per_instance, 2, colour_rate};
    pd.slot_strides[1] = 8, pd.slot_strides[2] = colour_stride;
    pd.cull_mode = graphics::CullMode::None;
    Handle h = 0;
    const NtStatus s = e.kernel.create_pipeline(kPid, e.dev, pd, &h);
    if (status_out) *status_out = s;
    else CHECK(s == status::kSuccess);
    return h;
  };
  auto picture = [&](Handle pipe, uint32_t instances, uint32_t first_instance, uint32_t colour_at = 128, NtStatus* status_out = nullptr) {
    CommandWriter w(e.allocator);
    w.begin_pass(0, kNoAllocation, kPassClearColor, {0, 0, 0, 1}, 1, 0);
    w.bind_pipeline(pipe);
    w.bind_vertex_buffer_slot(0, 1, 0);
    w.bind_vertex_buffer_slot(1, 1, 64);
    w.bind_vertex_buffer_slot(2, 1, colour_at);
    w.draw_instanced(3, instances, 0, first_instance);
    w.end_pass();
    const NtStatus s = e.render(w, {rt, vb});
    if (status_out) *status_out = s;
    std::vector<uint32_t> out;
    if (s != status::kSuccess) return out;
    for (uint32_t y = 0; y < 16; ++y)
      for (uint32_t x = 0; x < 16; ++x) out.push_back(e.pixel(rt, x, y));
    return out;
  };
  // (a pixel well inside each place's triangle: the place of offset (0, 0), (1, 0) and (0, 1); the picture's first row is clip space's y = +1)
  auto at = [](const std::vector<uint32_t>& img, uint32_t quarter) { return img.empty() ? 0xDEADDEADu : img[(quarter == 2 ? 6 : 14) * 16 + (quarter == 1 ? 9 : 1)]; };

  // Colours as B8G8R8X8 (the fourth byte is not alpha: the picture's alpha is 1), one an instance with step rate 1: instance n gets element n.
  const uint8_t bgrx[12] = {0x00, 0x00, 0xFF, 0x00, 0x00, 0xFF, 0x00, 0x10, 0xFF, 0x00, 0x00, 0x7F};   // red, green, blue
  put(128, ConstByteView(bgrx, 12));
  Handle pipe = make(graphics::VertexFormat::Bgrx8Unorm, true, 1, 1, 4);
  std::vector<uint32_t> img = picture(pipe, 3, 0);
  CHECK(at(img, 0) == 0xFFFF0000u && at(img, 1) == 0xFF00FF00u && at(img, 2) == 0xFF0000FFu);
  // Offsets stepping every second instance, colours every third: instances 0, 1 at offset 0, 2, 3 at offset 1; colours 0, 0, 0, 1 - the last one drawn over a place wins.
  pipe = make(graphics::VertexFormat::Bgrx8Unorm, true, 2, 3, 4);
  img = picture(pipe, 4, 0);
  CHECK(at(img, 0) == 0xFFFF0000u && at(img, 1) == 0xFF00FF00u && at(img, 2) == 0xFF000000u);
  // ... from the first instance 1: offsets 1 + n / 2 (offsets 1, then 2), colours 1 + n / 3 (green, green, green, blue)
  img = picture(pipe, 4, 1);
  CHECK(at(img, 0) == 0xFF000000u && at(img, 1) == 0xFF00FF00u && at(img, 2) == 0xFF0000FFu);
  // A colour of step rate 0: every instance reads the first instance's element.
  pipe = make(graphics::VertexFormat::Bgrx8Unorm, true, 1, 0, 4);
  img = picture(pipe, 3, 0);
  CHECK(at(img, 0) == 0xFFFF0000u && at(img, 1) == 0xFFFF0000u && at(img, 2) == 0xFFFF0000u);
  img = picture(pipe, 2, 1);   // (instances 1, 2: offsets 1 and 2, the colour of element 1)
  CHECK(at(img, 0) == 0xFF000000u && at(img, 1) == 0xFF00FF00u && at(img, 2) == 0xFF00FF00u);
  // The bounds: a slot whose elements end where the instances' last read is fits, one element fewer does not. Rate 2, 4 instances: 2 offset elements (slot 1 at 64 + 8 bytes
  // of 512: put its view 16 bytes before the end).
  {
    put(512 - 16, floats(offsets, 4));
    pipe = make(graphics::VertexFormat::Bgrx8Unorm, true, 2, 0, 4);
    auto run = [&](uint32_t offset_at, uint32_t instances) {
      CommandWriter c(e.allocator);
      c.begin_pass(0, kNoAllocation, kPassClearColor, {0, 0, 0, 1}, 1, 0);
      c.bind_pipeline(pipe);
      c.bind_vertex_buffer_slot(0, 1, 0);
      c.bind_vertex_buffer_slot(1, 1, offset_at);
      c.bind_vertex_buffer_slot(2, 1, 512 - 4);   // (a rate-0 colour: one element)
      c.draw_instanced(3, instances, 0, 0);
      c.end_pass();
      return e.render(c, {rt, vb});
    };
    CHECK(run(512 - 16, 4) == status::kSuccess);
    CHECK(run(512 - 16, 5) == status::kInvalidParameter);   // (instance 4 reads element 2)
    CHECK(run(512 - 8, 4) == status::kInvalidParameter);
    CHECK(run(512 - 8, 2) == status::kSuccess);
  }

  // The new formats as per-vertex colours (a red triangle: what each reads as red 1, green 0.5, blue 0).
  {
    // R11G11B10_FLOAT: R 1.0, G 0.5 (exponent 14), B 0; the alpha is 1
    const uint32_t f11 = (15u << 6) | ((14u << 6) << 11);
    const uint32_t f11s[3] = {f11, f11, f11};
    put(128, ConstByteView(reinterpret_cast<const uint8_t*>(f11s), 12));
    pipe = make(graphics::VertexFormat::Rg11B10Float, false, 1, 0, 4);
    img = picture(pipe, 1, 0);
    CHECK(close8(at(img, 0) >> 16 & 0xFF, 255) && close8(at(img, 0) >> 8 & 0xFF, 128) && close8(at(img, 0) & 0xFF, 0) && (at(img, 0) >> 24) == 255);
    // R10G10B10A2_UINT is integers in the shader's register: a vs_2_0 reads them as floats (tiny ones: black); the integer path is in test_vertex_fetch_formats and the
    // Direct3D 11/12 tests. Here: the pipeline is made and draws.
    pipe = make(graphics::VertexFormat::Rgb10A2Uint, false, 1, 0, 4);
    img = picture(pipe, 1, 0);
    CHECK(!img.empty() && (at(img, 0) & 0xFFFFFF) == 0);
  }

  // The escape: the step rates in the tagged part kPipelineStepRateTag after the constant blend colour (its word count, then a word for each attribute from the first; a
  // per-vertex one's is 0), alone or with the rasterizer state's part kPipelineRasterTag, in either order.
  {
    put(128, ConstByteView(bgrx, 12));
    ByteBuffer req(e.allocator);
    req.put32(static_cast<uint32_t>(EscapeOp::CreatePipeline)), req.put32(vsh), req.put32(psh), req.put32(12), req.put32(3), req.put32(1), req.put32(0);
    req.put32(0), req.put32(0), req.put32(0);   // cull none, front face, flags
    const uint32_t attributes[12] = {0, 2, 0, 1, 1, 0, 2, static_cast<uint32_t>(graphics::VertexFormat::Bgrx8Unorm), 0, 0, 0, 0};
    for (uint32_t w : attributes) req.put32(w);
    for (uint32_t w : {0u, 7u, 0u, 0u, 0u, 255u, 255u, 0u}) req.put32(w);   // stencil off
    for (uint32_t w : {3u, 0u, 0u, 1u, 0u, 1u, 0u, 0u, 0u, 15u, 6u}) req.put32(w);   // depth compare LessEqual, no blending, attributes 1 and 2 per instance
    req.put32(1), req.put32(0);   // one colour output, no independent blending
    for (uint32_t t = 0; t < 3; ++t)
      for (uint32_t w : {0u, 0u, 1u, 0u, 1u, 0u, 0u, 0u, 15u}) req.put32(w);
    req.put32(0), req.put32(0), req.put32(0), req.put32(0), req.put32(0);   // multisample, geometry, hull, domain, stream output entries
    req.put32(0), req.put32(0), req.put32(0 | 1u << 4 | 2u << 8), req.put32(2), req.put32(8), req.put32(4), req.put32(0);   // no extra attributes, slots 0 1 2, strides of 1 and 2, no more blends
    for (uint32_t i = 0; i < 4; ++i) req.put32(0);   // the constant blend colour
    const uint64_t before_rates = req.size();
    req.put32(kPipelineStepRateTag), req.put32(3), req.put32(0), req.put32(2), req.put32(3);   // offsets every second instance, colours every third
    uint8_t reply[16];
    uint32_t reply_bytes = 0;
    CHECK(e.kernel.escape(kPid, e.dev, req.view(), ByteView(reply, sizeof reply), &reply_bytes) == status::kSuccess && reply_bytes == 4);
    const Handle via_escape = ByteReader(ConstByteView(reply, 4)).le32();
    CHECK(picture(via_escape, 4, 0) == picture(make(graphics::VertexFormat::Bgrx8Unorm, true, 2, 3, 4), 4, 0));
    // fewer rate words than attributes: the rest step by 1
    ByteBuffer two(e.allocator);
    CHECK(two.append(req.view().first(before_rates)));
    two.put32(kPipelineStepRateTag), two.put32(2), two.put32(0), two.put32(2);
    CHECK(e.kernel.escape(kPid, e.dev, two.view(), ByteView(reply, sizeof reply), &reply_bytes) == status::kSuccess);
    CHECK(picture(ByteReader(ConstByteView(reply, 4)).le32(), 4, 0) == picture(make(graphics::VertexFormat::Bgrx8Unorm, true, 2, 1, 4), 4, 0));
    // refused: more rate words than attributes, a per-vertex attribute with a rate, a count with its words cut off, a word too many, a format past the last
    auto refused = [&](std::initializer_list<uint32_t> tail, uint64_t keep) {
      ByteBuffer b(e.allocator);
      CHECK(b.append(req.view().first(keep)));
      for (uint32_t w : tail) b.put32(w);
      return e.kernel.escape(kPid, e.dev, b.view(), ByteView(reply, sizeof reply), &reply_bytes) == status::kInvalidParameter;
    };
    CHECK(refused({kPipelineStepRateTag, 4, 0, 1, 1, 1}, before_rates));
    CHECK(refused({kPipelineStepRateTag, 1, 5}, before_rates));
    CHECK(refused({kPipelineStepRateTag, 3, 0, 2}, before_rates));
    CHECK(refused({kPipelineStepRateTag, 3, 0, 2, 3, 9}, before_rates));
    CHECK(refused({kPipelineStepRateTag, 3, 0, 2, 3, kPipelineStepRateTag, 3, 0, 2, 3}, before_rates));   // (a part twice)
    CHECK(refused({3, 0, 2, 3}, before_rates));   // (the untagged count)
    CHECK(refused({0xFFFFFFFFu}, before_rates));
    // with the rasterizer state's part (its plain values) before or after the step rates: the same pipeline
    const std::initializer_list<uint32_t> raster = {kPipelineRasterTag, kPipelineRasterWords, 0u, 1u, 0u, 0u, 0u, 0u, 0u, 7u, 0u, 0u, 0u, 255u, 255u, 0u, 4u, 0u};
    const std::initializer_list<uint32_t> rates = {kPipelineStepRateTag, 3u, 0u, 2u, 3u};
    for (int order = 0; order < 2; ++order) {
      ByteBuffer both(e.allocator);
      CHECK(both.append(req.view().first(before_rates)));
      for (uint32_t w : order ? raster : rates) both.put32(w);
      for (uint32_t w : order ? rates : raster) both.put32(w);
      CHECK(e.kernel.escape(kPid, e.dev, both.view(), ByteView(reply, sizeof reply), &reply_bytes) == status::kSuccess && reply_bytes == 4);
      CHECK(picture(ByteReader(ConstByteView(reply, 4)).le32(), 4, 0) == picture(via_escape, 4, 0));
    }
    for (uint64_t cut = before_rates + 1; cut < req.size(); ++cut) CHECK(refused({}, cut));   // (a word cut in the middle)
    ByteBuffer bad_format(e.allocator);
    CHECK(bad_format.append(req.view()));
    bad_format.writable_view()[4 * (10 + 7)] = static_cast<uint8_t>(graphics::kVertexFormatCount);   // (attribute 2's format word)
    CHECK(e.kernel.escape(kPid, e.dev, bad_format.view(), ByteView(reply, sizeof reply), &reply_bytes) == status::kInvalidParameter);
    // every bit of the rate part flipped: made or refused, never anything else (and never a read past the request: ASan)
    for (uint64_t i = before_rates; i < req.size(); ++i)
      for (uint32_t bit = 0; bit < 8; ++bit) {
        ByteBuffer f(e.allocator);
        CHECK(f.append(req.view()));
        f.writable_view()[i] = static_cast<uint8_t>(f.view()[i] ^ (1u << bit));
        const NtStatus s = e.kernel.escape(kPid, e.dev, f.view(), ByteView(reply, sizeof reply), &reply_bytes);
        CHECK(s == status::kSuccess || s == status::kInvalidParameter);
      }
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
        graphics::Vec4 attributes[16] = {{0.1f * static_cast<float>(vertex_id), -0.3f, 0.5f, 1}, {0.25f, -0.5f, 0, 0}, {}, {}};
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
          graphics::Vec4 varyings[16] = {};
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
      // The vertex shader of d3d9.dll's fixed-function pipeline (src/Windows/d3d9/shaders.inc, made from ff_vs.hlsl): the kernel takes it, with 11
      // registers of constants. (The pixel shaders of that pipeline are made of the texture stages as it runs: tests/FfStagesTest.cpp.)
      {
        UniquePointer<D3d9Program> fixed_vs;
        CHECK(D3d9Program::compile_dxbc(allocator, ConstByteView(g_ff_vs, sizeof(g_ff_vs)), &fixed_vs) == status::kSuccess && fixed_vs && fixed_vs->constant_bytes() == 11 * 16);
        CHECK(fixed_vs && fixed_vs->stage() == graphics::ShaderStage::Vertex);
      }
      // The same shader with its constant buffers placed after another stage's (a Direct3D 11 pipeline's pixel shader, after the vertex shader's 4
      // registers): the block is that much longer, the text reads the registers from there on, and a place past the block is refused.
      {
        CHECK(cp->constant_bytes() == 64 * 16);
        UniquePointer<D3d9Program> placed;
        CHECK(D3d9Program::compile_dxbc(allocator, view_of(cbuf_blob), &placed, 4) == status::kSuccess && placed && placed->constant_bytes() == 68 * 16);
        String placed_text(allocator);
        CHECK(placed && placed->emit_tgsi(&placed_text) == status::kSuccess);
        CHECK(std::string(placed_text.data(), placed_text.size()).find("DCL CONST[0..67]") != std::string::npos);
        UniquePointer<D3d9Program> refused;
        CHECK(D3d9Program::compile_dxbc(allocator, view_of(cbuf_blob), &refused, D3d9Program::kMaxConstantRegisters - 63) == status::kNotSupported);
        CHECK(D3d9Program::compile_dxbc(allocator, view_of(cbuf_blob), &refused, D3d9Program::kMaxConstantRegisters + 1) == status::kInvalidParameter);
        CHECK(D3d9Program::compile_dxbc(allocator, view_of(cbuf_blob), &refused, D3d9Program::kMaxConstantRegisters - 64) == status::kSuccess);
      }
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
          graphics::Vec4 varyings[16] = {};
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
    CHECK(with_primitive->emit_tgsi(&refused) == status::kSuccess && __builtin_strstr(refused.c_str(), "PRIM_ID"));   // (the primitive number is a PRIM_ID input (Gallium's name))
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
          graphics::Vec4 v[16] = {};
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

// Levels of every image (graphics::kFeature2MipmappedLayers, the CPU renderer): arrays, volumes, cubes and cube arrays of several levels, sampled at a level (SampleLevel, Load,
// derivatives), through views of levels and layers and of another kind (a slice as a 2D texture, an array as a cube or a cube array), drawn into at a level (kPassLevels, also
// while a view of another level of the target is sampled), copied between subresources (CopyRegion's subresource words, CopyBufferImage), their levels made (GenerateMipmaps of
// arrays, volumes and wide formats), storage images of a cube's faces and of a level of an array, a texture descriptor's view; and what a device without the feature refuses.
void test_mipmapped_layers() {
  Env e;
  using graphics::TextureKind;
  auto create = [&](Kernel& kernel, Handle dev, TextureKind kind, uint32_t layers, uint32_t w, uint32_t h, uint32_t mips, uint32_t flags = kAllocTexture, Format f = Format::A8R8G8B8,
                    Handle* out = nullptr) {
    AllocationDesc d;
    d.flags = flags, d.format = f, d.width = w, d.height = h, d.kind = kind, d.layers = layers, d.mip_levels = mips;
    Handle h_ = 0;
    const NtStatus s = kernel.create_allocation(kPid, dev, d, &h_);
    if (out) *out = h_;
    return s;
  };
  auto make = [&](TextureKind kind, uint32_t layers, uint32_t w, uint32_t h, uint32_t mips, uint32_t flags = kAllocTexture, Format f = Format::A8R8G8B8) {
    Handle out = 0;
    CHECK(create(e.kernel, e.dev, kind, layers, w, h, mips, flags, f, &out) == status::kSuccess);
    return out;
  };
  // (the colour of layer k of level l of a test texture)
  auto colour = [](uint32_t level, uint32_t layer) { return 0xFF000000u | (0x20u + level * 0x40u) << 16 | (0x10u + layer * 0x20u) << 8 | 0x33u; };
  // Every layer of every level its colour (`levels` levels, `layers_at(level)` layers each, `size(level)` pixels a layer).
  auto fill_levels = [&](Handle a, uint32_t levels, uint32_t w, uint32_t h, auto layers_at) {
    for (uint32_t l = 0; l < levels; ++l) {
      LockResult r;
      CHECK(e.kernel.lock(kPid, a, kLockDiscard, l, &r) == status::kSuccess);
      ArrayView<uint32_t> words;
      CHECK(r.data.try_cast<uint32_t>(&words));
      const uint64_t layer_pixels = uint64_t{graphics::mip_extent(w, l)} * graphics::mip_extent(h, l);
      CHECK(words.size() == layer_pixels * layers_at(l));
      for (uint32_t k = 0; k < layers_at(l); ++k) words.subview(k * layer_pixels, layer_pixels).fill(colour(l, k));
      e.kernel.unlock(kPid, a);
    }
  };
  // The word at (x, y) of layer `layer` of level `level`.
  auto texel = [&](Handle a, uint32_t level, uint32_t layer, uint32_t x, uint32_t y) {
    // (a level's layers follow each other, each pitch * the level's height bytes)
    AllocationInfo about;
    LockResult r;
    if (e.kernel.query_allocation(kPid, a, &about) != status::kSuccess || e.kernel.lock(kPid, a, kLockReadOnly, level, &r) != status::kSuccess) return 0xDEADDEADu;
    ByteReader at(r.data);
    at.skip((uint64_t{layer} * graphics::mip_extent(about.desc.height, level) + y) * r.pitch + uint64_t{x} * 4);
    const uint32_t v = at.le32();
    e.kernel.unlock(kPid, a);
    return at.ok() ? v : 0xDEADDEADu;
  };

  // --- What can be made, and its memory: a level of a layered image has all its layers, a volume's level half the slices.
  const Handle array = make(TextureKind::Texture2DArray, 3, 8, 8, 4);
  LockResult lock;
  CHECK(e.kernel.lock(kPid, array, kLockReadOnly, 1, &lock) == status::kSuccess && lock.data.size() == 4 * 4 * 4 * 3 && lock.pitch == 16);
  e.kernel.unlock(kPid, array);
  CHECK(e.kernel.lock(kPid, array, kLockReadOnly, 3, &lock) == status::kSuccess && lock.data.size() == 1 * 1 * 4 * 3);
  e.kernel.unlock(kPid, array);
  const Handle volume = make(TextureKind::Texture3D, 4, 8, 8, 4);
  const uint64_t volume_bytes[4] = {8 * 8 * 4 * 4, 4 * 4 * 4 * 2, 2 * 2 * 4 * 1, 1 * 1 * 4 * 1};
  for (uint32_t l = 0; l < 4; ++l) {
    CHECK(e.kernel.lock(kPid, volume, kLockReadOnly, l, &lock) == status::kSuccess && lock.data.size() == volume_bytes[l]);
    e.kernel.unlock(kPid, volume);
  }
  AllocationInfo info;
  CHECK(e.kernel.query_allocation(kPid, volume, &info) == status::kSuccess && info.bytes == volume_bytes[0] + volume_bytes[1] + volume_bytes[2] + volume_bytes[3]);
  Handle h = 0;
  CHECK(create(e.kernel, e.dev, TextureKind::TextureCubeArray, 12, 4, 4, 3) == status::kSuccess);
  CHECK(create(e.kernel, e.dev, TextureKind::TextureCubeArray, 13, 4, 4, 1) == status::kInvalidParameter);   // (six layers a cube)
  CHECK(create(e.kernel, e.dev, TextureKind::TextureCubeArray, 12, 4, 8, 1) == status::kInvalidParameter);   // (square)
  CHECK(create(e.kernel, e.dev, TextureKind::TextureCube, 6, 4, 4, 3, kAllocTexture | kAllocStorageImage) == status::kSuccess);
  CHECK(create(e.kernel, e.dev, TextureKind::Texture2DArray, 2, 8, 8, 3, kAllocTexture | kAllocRenderTarget) == status::kSuccess);
  CHECK(create(e.kernel, e.dev, TextureKind::Texture2DArray, 2, 8, 8, 2, kAllocDepthStencil, Format::D24S8) == status::kSuccess);
  CHECK(create(e.kernel, e.dev, TextureKind::Texture2D, 1, 8, 8, 4, kAllocDepthStencil, Format::D32Float) == status::kSuccess);
  CHECK(create(e.kernel, e.dev, TextureKind::Texture2DArray, 2, 8, 8, 2, kAllocTexture | kAllocRenderTarget, Format::A16B16G16R16F) == status::kSuccess);
  CHECK(create(e.kernel, e.dev, TextureKind::Texture3D, 2, 8, 8, 2, kAllocStaging) == status::kSuccess);
  CHECK(create(e.kernel, e.dev, TextureKind::Texture2DArray, 2, 8, 8, 5, kAllocTexture) == status::kInvalidParameter);   // (8 x 8 has four levels)
  CHECK(create(e.kernel, e.dev, TextureKind::Texture2DArray, 2, 8, 8, 2, kAllocTexture | kAllocPresent) == status::kInvalidParameter);
  // A device without the feature: arrays, volumes and cubes of one level, targets and depth buffers of one, wide formats of one, no cube arrays (a 2D texture's levels and staging
  // memory are fine).
  {
    FakeScreen screen(8, 6);
    graphics::SoftwareDevice software(e.allocator, &screen);
    SpyDevice limited(software);
    limited.feature2_mask = ~static_cast<uint32_t>(graphics::kFeature2MipmappedLayers);
    Kernel kernel(e.allocator);
    AdapterConfig c;
    c.display = &screen;
    kernel.add_adapter(limited, c);
    Handle adapter = 0, dev = 0, ctx = 0;
    CHECK(kernel.open_process(kPid) == status::kSuccess && kernel.open_adapter_from_index(kPid, 0, &adapter) == status::kSuccess && kernel.create_device(kPid, adapter, &dev) == status::kSuccess);
    CHECK(kernel.create_context(kPid, dev, ContextPriority::Normal, &ctx) == status::kSuccess);
    CHECK(create(kernel, dev, TextureKind::Texture2DArray, 3, 8, 8, 2) == status::kNotSupported);
    CHECK(create(kernel, dev, TextureKind::Texture3D, 4, 8, 8, 2) == status::kNotSupported);
    CHECK(create(kernel, dev, TextureKind::TextureCubeArray, 12, 4, 4, 1) == status::kNotSupported);
    CHECK(create(kernel, dev, TextureKind::TextureCube, 6, 4, 4, 1, kAllocTexture | kAllocStorageImage) == status::kNotSupported);
    CHECK(create(kernel, dev, TextureKind::Texture2D, 1, 8, 8, 2, kAllocTexture | kAllocRenderTarget) == status::kNotSupported);
    CHECK(create(kernel, dev, TextureKind::Texture2D, 1, 8, 8, 2, kAllocDepthStencil, Format::D32Float) == status::kNotSupported);
    CHECK(create(kernel, dev, TextureKind::Texture2D, 1, 8, 8, 2, kAllocTexture, Format::A16B16G16R16F) == status::kNotSupported);
    CHECK(create(kernel, dev, TextureKind::Texture2D, 1, 8, 8, 2) == status::kSuccess);
    CHECK(create(kernel, dev, TextureKind::Texture2DArray, 3, 8, 8, 2, kAllocStaging) == status::kSuccess);
    // (and the stream's views and levels)
    Handle tex = 0, target = 0;
    CHECK(create(kernel, dev, TextureKind::Texture2D, 1, 8, 8, 2, kAllocTexture, Format::A8R8G8B8, &tex) == status::kSuccess);
    CHECK(create(kernel, dev, TextureKind::Texture2D, 1, 8, 8, 1, kAllocRenderTarget, Format::A8R8G8B8, &target) == status::kSuccess);
    std::vector<Handle> list = {target, tex};
    CommandWriter w(e.allocator);
    w.bind_texture_view(0, 1, graphics::SamplerDesc(), 1, 0, 0, 0);
    CHECK(kernel.render(kPid, ctx, w.view(), ArrayView<const Handle>(list.data(), list.size())) == status::kInvalidParameter);
    CommandWriter whole(e.allocator);
    whole.bind_texture_view(0, 1, graphics::SamplerDesc(), 0, 0, 0, 0);   // (a view of all of it is the texture)
    CHECK(kernel.render(kPid, ctx, whole.view(), ArrayView<const Handle>(list.data(), list.size())) == status::kSuccess);
    CommandWriter levels(e.allocator);
    levels.begin_pass_levels(0, {}, kNoAllocation, kPassClearColor, {0, 0, 0, 1}, 1, 0, {}, {0, 0});
    levels.end_pass();
    CHECK(kernel.render(kPid, ctx, levels.view(), ArrayView<const Handle>(list.data(), list.size())) == status::kInvalidParameter);
  }

  // --- Sampling: vs_vol's texture coordinate is the vertex colour, the same over the whole target.
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
  const Handle lod_pipe = pipeline(b::k_ps_texarray_lod, sizeof b::k_ps_texarray_lod), array_pipe = pipeline(b::k_ps_texarray, sizeof b::k_ps_texarray);
  const Handle level_pipe = pipeline(b::k_ps_texelarray_level, sizeof b::k_ps_texelarray_level), slice_pipe = pipeline(b::k_ps_texslice, sizeof b::k_ps_texslice);
  const Handle cube_pipe = pipeline(b::k_ps_texcube, sizeof b::k_ps_texcube), cube_array_pipe = pipeline(b::k_ps_texcubearray, sizeof b::k_ps_texcubearray);
  const Handle cube_array_lod_pipe = pipeline(b::k_ps_texcubearray_lod, sizeof b::k_ps_texcubearray_lod), volume_lod_pipe = pipeline(b::k_ps_tex3d_lod, sizeof b::k_ps_tex3d_lod);
  const Handle target = e.alloc(kTarget, Format::A8R8G8B8, 8, 8), vb = e.buffer(kAllocVertexBuffer, 3 * 28);
  const float identity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
  struct View {
    uint32_t base_level = 0, level_count = 0, first_layer = 0, layer_count = 0;
    bool given = false;
  };
  // Draws the big triangle with texture coordinate (u, v, w) and alpha `a` through `pipe` with `texture` (seen through `view` when given); the status of the stream.
  auto draw = [&](Handle pipe, Handle texture, float u, float v, float w, float a, View view = View(), graphics::SamplerDesc sampler = graphics::SamplerDesc()) {
    const float vertices[21] = {-1, -1, 0.5f, u, v, w, a, 3, -1, 0.5f, u, v, w, a, -1, 3, 0.5f, u, v, w, a};
    CHECK(e.write(vb, floats(vertices, 21)));
    CommandWriter cw(e.allocator);
    cw.begin_pass(0, kNoAllocation, kPassClearColor, {0, 0, 0, 1}, 1, 0);
    cw.bind_pipeline(pipe);
    cw.bind_vertex_buffer(1);
    cw.push_constants(floats(identity, 16));
    if (view.given) cw.bind_texture_view(0, 2, sampler, view.base_level, view.level_count, view.first_layer, view.layer_count);
    else cw.bind_texture(0, 2, sampler);
    cw.draw(3);
    cw.end_pass();
    return e.render(cw, {target, vb, texture});
  };
  // (a sampler that reads the levels: SampleLevel's level is the nearest)
  graphics::SamplerDesc levels_sampler;
  levels_sampler.mip_filter = graphics::MipFilter::Nearest;
  // An array of three layers of 4 x 4, three levels: SampleLevel 1 reads level 1 of the layer the coordinate names, Load and GetDimensions of level 1.
  const Handle small = make(TextureKind::Texture2DArray, 3, 4, 4, 3);
  fill_levels(small, 3, 4, 4, [](uint32_t) { return 3u; });
  CHECK(draw(lod_pipe, small, 0.5f, 0.5f, 2.0f, 1, View(), levels_sampler) == status::kSuccess && e.pixel(target, 3, 3) == colour(1, 2));
  CHECK(draw(lod_pipe, small, 0.5f, 0.5f, 2.0f, 1) == status::kSuccess && e.pixel(target, 3, 3) == colour(0, 2));   // (a sampler without levels reads level 0)
  CHECK(draw(level_pipe, small, 0.5f, 0.5f, 1.0f, 1) == status::kSuccess && e.pixel(target, 3, 3) == colour(1, 1));
  CHECK(draw(level_pipe, small, 0.5f, 0.5f, 1.0f, 0) == status::kSuccess && e.pixel(target, 3, 3) == 0x30202030u);   // (2 x 2, 3 layers, 3 levels: each / 16)
  // Views: from level 1 and layer 1, level 0 of the view is level 1 and its layer 0 layer 1 (and its sizes are those); its levels are two.
  View from_one;
  from_one.given = true, from_one.base_level = 1, from_one.first_layer = 1;
  CHECK(draw(array_pipe, small, 0.5f, 0.5f, 0.0f, 1, from_one) == status::kSuccess && e.pixel(target, 3, 3) == colour(1, 1));
  CHECK(draw(lod_pipe, small, 0.5f, 0.5f, 1.0f, 1, from_one, levels_sampler) == status::kSuccess && e.pixel(target, 3, 3) == colour(2, 2));
  CHECK(draw(level_pipe, small, 0.5f, 0.5f, 0.0f, 0, from_one) == status::kSuccess && e.pixel(target, 3, 3) == 0x20101020u);   // (1 x 1, 2 layers, 2 levels)
  View one_level = from_one;
  one_level.level_count = 1, one_level.layer_count = 1;
  CHECK(draw(lod_pipe, small, 0.5f, 0.5f, 5.0f, 1, one_level, levels_sampler) == status::kSuccess && e.pixel(target, 3, 3) == colour(1, 1));   // (the lod and the layer clamped to the view)
  // A slice of the array as a 2D texture (a view of one layer); without the view the array is not one.
  View slice;
  slice.given = true, slice.first_layer = 2, slice.layer_count = 1;
  CHECK(draw(slice_pipe, small, 0.5f, 0.5f, 0, 1, slice) == status::kSuccess && e.pixel(target, 3, 3) == colour(0, 2));
  CHECK(draw(slice_pipe, small, 0.5f, 0.5f, 0, 1) == status::kSuccess);   // (left out: the array is not a 2D texture)
  View two;
  two.given = true, two.layer_count = 2;
  CHECK(draw(slice_pipe, small, 0.5f, 0.5f, 0, 1, two) == status::kSuccess);   // (two layers are not a 2D texture: left out)
  // Views the image does not have.
  View past;
  past.given = true, past.base_level = 3;
  CHECK(draw(array_pipe, small, 0.5f, 0.5f, 0, 1, past) == status::kInvalidParameter);
  past.base_level = 1, past.level_count = 3;
  CHECK(draw(array_pipe, small, 0.5f, 0.5f, 0, 1, past) == status::kInvalidParameter);
  past.level_count = 0, past.first_layer = 3;
  CHECK(draw(array_pipe, small, 0.5f, 0.5f, 0, 1, past) == status::kInvalidParameter);
  // Six layers of an array seen as a cube: (0.1, 0.1, 1) is on +Z, the fifth face.
  const Handle six = make(TextureKind::Texture2DArray, 8, 4, 4, 1);
  fill_levels(six, 1, 4, 4, [](uint32_t) { return 8u; });
  View cube_view;
  cube_view.given = true, cube_view.first_layer = 2, cube_view.layer_count = 6;
  CHECK(draw(cube_pipe, six, 0.1f, 0.1f, 1.0f, 1, cube_view) == status::kSuccess && e.pixel(target, 3, 3) == colour(0, 2 + 4));
  CHECK(draw(cube_pipe, six, 0.1f, 0.1f, 1.0f, 1) == status::kSuccess);   // (eight layers are not a cube: left out)
  // A cube array of two cubes, two levels: (0.55, 0.5, 1) * 2 - 1 points at +Z, alpha 0.1 names cube 1: layer 10; and the same through SampleLevel 1.
  const Handle cubes = make(TextureKind::TextureCubeArray, 12, 4, 4, 2);
  fill_levels(cubes, 2, 4, 4, [](uint32_t) { return 12u; });
  CHECK(draw(cube_array_pipe, cubes, 0.55f, 0.5f, 1.0f, 0.1f) == status::kSuccess && e.pixel(target, 3, 3) == colour(0, 10));
  CHECK(draw(cube_array_lod_pipe, cubes, 0.55f, 0.5f, 1.0f, 0.1f, View(), levels_sampler) == status::kSuccess && e.pixel(target, 3, 3) == colour(1, 10));
  CHECK(draw(cube_array_pipe, cubes, 0.55f, 0.5f, 1.0f, 0.9f) == status::kSuccess && e.pixel(target, 3, 3) == colour(0, 10));   // (the cube clamped)
  CHECK(draw(cube_array_pipe, cubes, 0.5f, 1.0f, 0.5f, 0.0f) == status::kSuccess && e.pixel(target, 3, 3) == colour(0, 2));    // (+Y of cube 0)
  // gather4 of a cube array (ps_gather_cubearray: red, red, blue, blue of the footprint, which is all of one layer's colour): +Z of cube 1, then -X of cube 0 (layer 1).
  const Handle gather_cubes_pipe = pipeline(b::k_ps_gather_cubearray, sizeof b::k_ps_gather_cubearray);
  auto gathered = [&](uint32_t c) { return (c & 0xFF) << 24 | ((c >> 16) & 0xFF) << 16 | ((c >> 16) & 0xFF) << 8 | (c & 0xFF); };
  CHECK(draw(gather_cubes_pipe, cubes, 0.55f, 0.5f, 1.0f, 0.1f) == status::kSuccess && e.pixel(target, 3, 3) == gathered(colour(0, 10)));
  printf("  levels: gather of a cube array %08x (want %08x)\n", static_cast<unsigned>(e.pixel(target, 3, 3)), static_cast<unsigned>(gathered(colour(0, 10))));
  CHECK(draw(gather_cubes_pipe, cubes, 0.0f, 0.45f, 0.55f, 0.0f) == status::kSuccess && e.pixel(target, 3, 3) == gathered(colour(0, 1)));
  // ... and twelve layers of an array as one, a cube array as an array, a cube of a cube array.
  View all;
  all.given = true;
  const Handle twelve = make(TextureKind::Texture2DArray, 12, 4, 4, 1);
  fill_levels(twelve, 1, 4, 4, [](uint32_t) { return 12u; });
  CHECK(draw(cube_array_pipe, twelve, 0.55f, 0.5f, 1.0f, 0.1f, all) == status::kSuccess && e.pixel(target, 3, 3) == colour(0, 10));
  CHECK(draw(array_pipe, cubes, 0.5f, 0.5f, 7.0f, 1, all) == status::kSuccess && e.pixel(target, 3, 3) == colour(0, 7));
  View second_cube;
  second_cube.given = true, second_cube.first_layer = 6, second_cube.layer_count = 6;
  CHECK(draw(cube_pipe, cubes, 0.1f, 0.1f, 1.0f, 1, second_cube) == status::kSuccess && e.pixel(target, 3, 3) == colour(0, 10));
  // A volume of 4 x 4 x 4, three levels: level 1 has two slices, (0.5, 0.5, 0.75) is in the second.
  const Handle box = make(TextureKind::Texture3D, 4, 4, 4, 3);
  fill_levels(box, 3, 4, 4, [](uint32_t l) { return l == 0 ? 4u : l == 1 ? 2u : 1u; });
  CHECK(draw(volume_lod_pipe, box, 0.5f, 0.5f, 0.75f, 1, View(), levels_sampler) == status::kSuccess && e.pixel(target, 3, 3) == colour(1, 1));
  CHECK(draw(volume_lod_pipe, box, 0.5f, 0.5f, 0.25f, 1, View(), levels_sampler) == status::kSuccess && e.pixel(target, 3, 3) == colour(1, 0));
  View volume_view;
  volume_view.given = true, volume_view.base_level = 1;
  CHECK(draw(volume_lod_pipe, box, 0.5f, 0.5f, 0.25f, 1, volume_view, levels_sampler) == status::kSuccess && e.pixel(target, 3, 3) == colour(2, 0));
  volume_view.first_layer = 1, volume_view.layer_count = 1;
  CHECK(draw(volume_lod_pipe, box, 0.5f, 0.5f, 0.25f, 1, volume_view) == status::kInvalidParameter);   // (a volume's view has all its slices)
  // Derivatives: a 16 x 16 array whose coordinates run over the 8 x 8 target twice as fast as its texels - two texels a pixel, level 1 by the nearest level.
  {
    const Handle big = make(TextureKind::Texture2DArray, 2, 16, 16, 5);
    fill_levels(big, 5, 16, 16, [](uint32_t) { return 2u; });
    const float vertices[21] = {-1, -1, 0.5f, 0, 0, 1, 1, 3, -1, 0.5f, 2, 0, 1, 1, -1, 3, 0.5f, 0, 2, 1, 1};
    CHECK(e.write(vb, floats(vertices, 21)));
    graphics::SamplerDesc mipmapped;
    mipmapped.mip_filter = graphics::MipFilter::Nearest;
    CommandWriter cw(e.allocator);
    cw.begin_pass(0, kNoAllocation, kPassClearColor, {0, 0, 0, 1}, 1, 0);
    cw.bind_pipeline(array_pipe);
    cw.bind_vertex_buffer(1);
    cw.push_constants(floats(identity, 16));
    cw.bind_texture(0, 2, mipmapped);
    cw.draw(3);
    cw.end_pass();
    CHECK(e.render(cw, {target, vb, big}) == status::kSuccess && e.pixel(target, 2, 2) == colour(1, 1) && e.pixel(target, 5, 6) == colour(1, 1));
  }

  // --- Drawing into a level: a clear of layer 1 of level 1, then a triangle into level 2 of layer 0; nothing else changes.
  const Handle rt = make(TextureKind::Texture2DArray, 2, 8, 8, 3, kAllocTexture | kAllocRenderTarget);
  fill_levels(rt, 3, 8, 8, [](uint32_t) { return 2u; });
  {
    CommandWriter cw(e.allocator);
    cw.begin_pass_levels(0, {}, kNoAllocation, kPassClearColor, {1, 0, 0, 1}, 1, 0, {1, 1, 0, 1}, {1, 0});
    cw.end_pass();
    CHECK(e.render(cw, {rt}) == status::kSuccess);
    CHECK(texel(rt, 1, 1, 3, 3) == 0xFFFF0000u && texel(rt, 1, 0, 3, 3) == colour(1, 0) && texel(rt, 0, 1, 3, 3) == colour(0, 1) && texel(rt, 2, 1, 0, 0) == colour(2, 1));
    const Handle colored = make_colored_pipeline(e);
    CHECK(write_triangle(e, vb, 0, 0, 1));
    CommandWriter dw(e.allocator);
    dw.begin_pass_levels(0, {}, kNoAllocation, 0, {0, 0, 0, 0}, 1, 0, {}, {2, 0});
    dw.bind_pipeline(colored);
    dw.bind_vertex_buffer(1);
    dw.push_constants(floats(identity, 16));
    dw.draw(3);
    dw.end_pass();
    CHECK(e.render(dw, {rt, vb}) == status::kSuccess);
    CHECK(texel(rt, 2, 0, 1, 1) == 0xFF0000FFu && texel(rt, 2, 1, 1, 1) == colour(2, 1) && texel(rt, 1, 0, 0, 0) == colour(1, 0));
    // (a level the target does not have; a depth buffer of another size at its level)
    CommandWriter bad(e.allocator);
    bad.begin_pass_levels(0, {}, kNoAllocation, kPassClearColor, {0, 0, 0, 0}, 1, 0, {}, {3, 0});
    bad.end_pass();
    CHECK(e.render(bad, {rt}) == status::kInvalidParameter);
    const Handle depth = make(TextureKind::Texture2D, 1, 8, 8, 2, kAllocDepthStencil, Format::D32Float);
    CommandWriter sized(e.allocator);
    sized.begin_pass_levels(0, {}, 1, kPassClearColor | kPassClearDepth, {0, 0, 0, 0}, 1, 0, {}, {1, 1});
    sized.end_pass();
    CHECK(e.render(sized, {rt, depth}) == status::kSuccess);
    // (a depth level larger than the pass is taken - its top left part, as Direct3D 9 and OpenGL have it -, a smaller one is not)
    CommandWriter larger(e.allocator);
    larger.begin_pass_levels(0, {}, 1, kPassClearColor | kPassClearDepth, {0, 0, 0, 0}, 1, 0, {}, {1, 0});
    larger.end_pass();
    CHECK(e.render(larger, {rt, depth}) == status::kSuccess);
    CommandWriter unsized(e.allocator);
    unsized.begin_pass_levels(0, {}, 1, kPassClearColor | kPassClearDepth, {0, 0, 0, 0}, 1, 0, {}, {0, 1});
    unsized.end_pass();
    CHECK(e.render(unsized, {rt, depth}) == status::kInvalidParameter);
    // (layers of a level of a volume: level 1 of four slices has two)
    const Handle vrt = make(TextureKind::Texture3D, 4, 8, 8, 2, kAllocTexture | kAllocRenderTarget);
    CommandWriter vw(e.allocator);
    vw.begin_pass_levels(0, {}, kNoAllocation, kPassClearColor, {0, 1, 0, 1}, 1, 0, {1, 1, 0, 1}, {1, 0});
    vw.end_pass();
    CHECK(e.render(vw, {vrt}) == status::kSuccess && texel(vrt, 1, 1, 0, 0) == 0xFF00FF00u && texel(vrt, 1, 0, 0, 0) == 0);
    CommandWriter vbad(e.allocator);
    vbad.begin_pass_levels(0, {}, kNoAllocation, kPassClearColor, {0, 1, 0, 1}, 1, 0, {2, 1, 0, 1}, {1, 0});
    vbad.end_pass();
    CHECK(e.render(vbad, {vrt}) == status::kInvalidParameter);
  }
  // Drawing into level 1 while sampling level 0 of the same texture: a view of level 0 alone may be bound in the pass; one that shows level 1 may not.
  {
    const Handle chain = make(TextureKind::Texture2D, 1, 8, 8, 2, kAllocTexture | kAllocRenderTarget);
    fill_levels(chain, 2, 8, 8, [](uint32_t) { return 1u; });
    const float vertices[21] = {-1, -1, 0.5f, 0.5f, 0.5f, 0, 1, 3, -1, 0.5f, 0.5f, 0.5f, 0, 1, -1, 3, 0.5f, 0.5f, 0.5f, 0, 1};
    CHECK(e.write(vb, floats(vertices, 21)));
    auto down = [&](uint32_t level_count) {
      CommandWriter cw(e.allocator);
      cw.begin_pass_levels(0, {}, kNoAllocation, 0, {0, 0, 0, 0}, 1, 0, {}, {1, 0});
      cw.bind_pipeline(slice_pipe);
      cw.bind_vertex_buffer(1);
      cw.push_constants(floats(identity, 16));
      cw.bind_texture_view(0, 0, graphics::SamplerDesc(), 0, level_count, 0, 0);
      cw.draw(3);
      cw.end_pass();
      return e.render(cw, {chain, vb});
    };
    CHECK(down(1) == status::kSuccess && texel(chain, 1, 0, 2, 2) == colour(0, 0));
    CHECK(down(2) == status::kInvalidParameter);
  }

  // --- Copies between subresources: level 1 of layer 2 of the small array into a 2D texture at (1, 1); a whole level of three layers into another array; slices of a volume's level.
  {
    const Handle flat = e.alloc(kTexture, Format::A8R8G8B8, 4, 4);
    CHECK(e.fill(flat, 0xFF000000u));
    CommandWriter cw(e.allocator);
    cw.copy_subresource_region(0, 0, 0, 1, 1, 1, 1, 2, 0, 0, 2, 2);
    CHECK(e.render(cw, {flat, small}) == status::kSuccess && e.pixel(flat, 1, 1) == colour(1, 2) && e.pixel(flat, 2, 2) == colour(1, 2) && e.pixel(flat, 0, 0) == 0xFF000000u &&
          e.pixel(flat, 3, 3) == 0xFF000000u);
    const Handle other = make(TextureKind::Texture2DArray, 3, 4, 4, 3);
    CommandWriter whole(e.allocator);
    for (uint32_t l = 0; l < 3; ++l) whole.copy_subresource_region(0, l, 0, 0, 0, 1, l, 0, 0, 0, graphics::mip_extent(4, l), graphics::mip_extent(4, l), 3);
    CHECK(e.render(whole, {other, small}) == status::kSuccess);
    bool same = true;
    for (uint32_t l = 0; l < 3; ++l)
      for (uint32_t k = 0; k < 3; ++k) same &= texel(other, l, k, 0, 0) == colour(l, k);
    CHECK(same);
    const Handle box2 = make(TextureKind::Texture3D, 4, 4, 4, 3);
    CommandWriter slices(e.allocator);
    slices.copy_subresource_region(0, 1, 0, 0, 0, 1, 1, 1, 0, 0, 2, 2, 1);   // (slice 1 of level 1 into slice 0)
    CHECK(e.render(slices, {box2, box}) == status::kSuccess && texel(box2, 1, 0, 1, 1) == colour(1, 1) && texel(box2, 1, 1, 1, 1) == 0);
    // (past a level's layers, past its size, a level it does not have)
    CommandWriter bad(e.allocator);
    bad.copy_subresource_region(0, 1, 2, 0, 0, 1, 1, 1, 0, 0, 2, 2, 1);
    CHECK(e.render(bad, {box2, box}) == status::kInvalidParameter);
    CommandWriter bad2(e.allocator);
    bad2.copy_subresource_region(0, 1, 0, 1, 0, 1, 1, 0, 0, 0, 2, 2, 1);
    CHECK(e.render(bad2, {box2, box}) == status::kInvalidParameter);
    CommandWriter bad3(e.allocator);
    bad3.copy_subresource_region(0, 3, 0, 0, 0, 1, 0, 0, 0, 0, 1, 1, 1);
    CHECK(e.render(bad3, {box2, box}) == status::kInvalidParameter);
    // Block-compressed: whole blocks of a level (8 x 8: level 1 is one block).
    const Handle bc_a = make(TextureKind::Texture2DArray, 2, 8, 8, 2, kAllocTexture, Format::Bc1), bc_b = make(TextureKind::Texture2DArray, 2, 8, 8, 2, kAllocTexture, Format::Bc1);
    uint8_t blocks[2 * 8];
    for (uint32_t i = 0; i < sizeof blocks; ++i) blocks[i] = static_cast<uint8_t>(0x40 + i);
    CHECK(e.write(bc_a, ConstByteView(blocks, sizeof blocks), kLockDiscard, 1));
    CommandWriter bc(e.allocator);
    bc.copy_subresource_region(0, 1, 0, 0, 0, 1, 1, 1, 0, 0, 4, 4, 1);
    CHECK(e.render(bc, {bc_b, bc_a}) == status::kSuccess);
    CHECK(e.kernel.lock(kPid, bc_b, kLockReadOnly, 1, &lock) == status::kSuccess && lock.data.size() == 16 && lock.data[0] == 0x48 && lock.data[7] == 0x4F && lock.data[8] == 0);
    e.kernel.unlock(kPid, bc_b);
    CommandWriter bc_bad(e.allocator);
    bc_bad.copy_subresource_region(0, 0, 0, 2, 0, 1, 0, 0, 0, 0, 4, 4, 1);   // (not on a block)
    CHECK(e.render(bc_bad, {bc_b, bc_a}) == status::kInvalidParameter);
    // CopyBufferImage into a layer of a level: row 1 of level 1 of layer 1 of the small array.
    const Handle buffer = e.buffer(kAllocVertexBuffer, 64);
    const uint32_t row[2] = {0xFF123456u, 0xFF654321u};
    CHECK(e.write(buffer, ConstByteView(reinterpret_cast<const uint8_t*>(row), sizeof row), kLockDiscard));
    CommandWriter up(e.allocator);
    up.copy_buffer_to_image(0, 1, 1, 0, 1, 2, 1, 1, 0, 8);
    CHECK(e.render(up, {small, buffer}) == status::kSuccess && texel(small, 1, 1, 0, 1) == 0xFF123456u && texel(small, 1, 1, 1, 1) == 0xFF654321u && texel(small, 1, 1, 0, 0) == colour(1, 1));
    CommandWriter up_bad(e.allocator);
    up_bad.copy_buffer_to_image(0, 1, 2, 0, 0, 1, 1, 1, 0, 8);   // (level 1 of a four-slice volume has two)
    CHECK(e.render(up_bad, {box, buffer}) == status::kInvalidParameter);
  }

  // --- GenerateMipmaps: each layer of an array its own chain; a volume's levels by 2 x 2 x 2; a wide format's by its values.
  {
    const Handle layers = make(TextureKind::Texture2DArray, 2, 4, 4, 3);
    LockResult r;
    CHECK(e.kernel.lock(kPid, layers, kLockDiscard, 0, &r) == status::kSuccess);
    ArrayView<uint32_t> words;
    CHECK(r.data.try_cast<uint32_t>(&words));
    for (uint32_t i = 0; i < 16; ++i) words[i] = (i % 2) ? 0xFF0000FFu : 0xFF000000u, words[16 + i] = 0xFF404040u;   // (layer 0: blue every other column)
    e.kernel.unlock(kPid, layers);
    CommandWriter g(e.allocator);
    g.generate_mipmaps(0);
    CHECK(e.render(g, {layers}) == status::kSuccess);
    CHECK(texel(layers, 1, 0, 1, 1) == 0xFF000080u && texel(layers, 2, 0, 0, 0) == 0xFF000080u && texel(layers, 2, 1, 0, 0) == 0xFF404040u);
    const Handle vol = make(TextureKind::Texture3D, 4, 4, 4, 3);
    CHECK(e.kernel.lock(kPid, vol, kLockDiscard, 0, &r) == status::kSuccess && r.data.try_cast<uint32_t>(&words));
    for (uint32_t i = 0; i < 64; ++i) words[i] = i < 32 ? 0xFF0000FFu : 0xFF000000u;   // (slices 0 and 1 blue, 2 and 3 black)
    e.kernel.unlock(kPid, vol);
    CommandWriter gv(e.allocator);
    gv.generate_mipmaps(0);
    CHECK(e.render(gv, {vol}) == status::kSuccess && texel(vol, 1, 0, 0, 0) == 0xFF0000FFu && texel(vol, 1, 1, 0, 0) == 0xFF000000u && texel(vol, 2, 0, 0, 0) == 0xFF000080u);
    const Handle wide = make(TextureKind::Texture2DArray, 2, 4, 4, 2, kAllocTexture, Format::A32B32G32R32F);
    CHECK(e.kernel.lock(kPid, wide, kLockDiscard, 0, &r) == status::kSuccess);
    ArrayView<float> values;
    CHECK(r.data.try_cast<float>(&values));
    for (uint32_t i = 0; i < 32; ++i) values[4 * i] = i < 16 ? static_cast<float>(i % 2) * 3.0f : 0.25f, values[4 * i + 1] = 0, values[4 * i + 2] = 0, values[4 * i + 3] = 1;
    e.kernel.unlock(kPid, wide);
    CommandWriter gw(e.allocator);
    gw.generate_mipmaps(0);
    CHECK(e.render(gw, {wide}) == status::kSuccess);
    CHECK(e.kernel.lock(kPid, wide, kLockReadOnly, 1, &r) == status::kSuccess && r.data.try_cast<float>(&values) && values.size() == 2 * 2 * 2 * 4 && values[0] == 1.5f &&
          values[16] == 0.25f && values[19] == 1.0f);
    e.kernel.unlock(kPid, wide);
    const Handle integers = make(TextureKind::Texture2DArray, 2, 4, 4, 2, kAllocTexture, Format::A32B32G32R32Uint);
    CommandWriter gi(e.allocator);
    gi.generate_mipmaps(0);
    CHECK(e.render(gi, {integers}) == status::kInvalidParameter);   // (integers are not filtered)
    // A level drawn into, then the chain made from it: the GPU's level 0 comes back first.
    const Handle drawn = make(TextureKind::Texture2D, 1, 4, 4, 3, kAllocTexture | kAllocRenderTarget);
    CommandWriter dg(e.allocator);
    dg.begin_pass(0, kNoAllocation, kPassClearColor, {0, 1, 0, 1}, 1, 0);
    dg.end_pass();
    dg.generate_mipmaps(0);
    CHECK(e.render(dg, {drawn}) == status::kSuccess && texel(drawn, 2, 0, 0, 0) == 0xFF00FF00u);
  }

  // --- Storage images: the six faces of a cube at its level 1, as an array of six (cs_uav_array writes (x / 8, y / 8, layer / layers, 1)); a level of an array.
  {
    const std::vector<uint8_t> code = read_blob("cs_uav_array.dxbc");
    ShaderDesc sd;
    sd.kind = ShaderKind::Dxbc, sd.stage = graphics::ShaderStage::Compute, sd.code = ConstByteView(code.data(), code.size());
    Handle cs = 0, fill = 0;
    CHECK(!code.empty() && e.kernel.create_shader(kPid, e.dev, sd, &cs) == status::kSuccess);
    PipelineDesc pd;
    pd.compute_shader = cs;
    CHECK(e.kernel.create_pipeline(kPid, e.dev, pd, &fill) == status::kSuccess);
    const Handle cube = make(TextureKind::TextureCube, 6, 16, 16, 2, kAllocTexture | kAllocStorageImage, Format::A8B8G8R8);
    CommandWriter cw(e.allocator);
    cw.bind_pipeline(fill);
    cw.bind_uav_image(0, 0, 1, 0, 6);
    cw.dispatch(1, 1, 6);
    CHECK(e.render(cw, {cube}) == status::kSuccess);
    // (A8B8G8R8: 0xAABBGGRR; texel (4, 0) of face 3 is (0.5, 0, 0.5, 1))
    CHECK(texel(cube, 1, 3, 4, 0) == 0xFF800080u && texel(cube, 1, 5, 0, 0) == 0xFFD50000u && texel(cube, 0, 3, 4, 0) == 0);
    const Handle levels = make(TextureKind::Texture2DArray, 2, 16, 16, 3, kAllocTexture | kAllocStorageImage, Format::A8B8G8R8);
    CommandWriter lw(e.allocator);
    lw.bind_pipeline(fill);
    lw.bind_uav_image(0, 0, 2, 1, 1);
    lw.dispatch(1, 1, 1);
    CHECK(e.render(lw, {levels}) == status::kSuccess && texel(levels, 2, 1, 2, 0) == 0xFF000040u && texel(levels, 2, 0, 2, 0) == 0);
    CommandWriter past(e.allocator);
    past.bind_pipeline(fill);
    past.bind_uav_image(0, 0, 3, 0, 1);
    past.dispatch(1, 1, 1);
    CHECK(e.render(past, {levels}) == status::kInvalidParameter);
  }

  // --- Mirrors of subresources: a 4 x 4 mirror of level 1 of layer 1 of an 8 x 8 array that writes the red back (what a narrow array's unordered access view of a level is made of);
  // the compute shader writes every channel of the mirror, the array's level 1 of layer 1 gets the red alone, the rest stays.
  {
    const std::vector<uint8_t> code = read_blob("cs_uav_array.dxbc");
    ShaderDesc sd;
    sd.kind = ShaderKind::Dxbc, sd.stage = graphics::ShaderStage::Compute, sd.code = ConstByteView(code.data(), code.size());
    Handle cs = 0, fill = 0;
    CHECK(!code.empty() && e.kernel.create_shader(kPid, e.dev, sd, &cs) == status::kSuccess);
    PipelineDesc pd;
    pd.compute_shader = cs;
    CHECK(e.kernel.create_pipeline(kPid, e.dev, pd, &fill) == status::kSuccess);
    const Handle source = make(TextureKind::Texture2DArray, 2, 8, 8, 2, kAllocTexture, Format::A8B8G8R8);
    const Handle mirror = make(TextureKind::Texture2D, 1, 4, 4, 1, kAllocTexture | kAllocStorageImage, Format::A8B8G8R8);
    fill_levels(source, 2, 8, 8, [](uint32_t) { return 2u; });
    const Handle sources[1] = {source};
    const uint32_t level1_layer1[1] = {1 | 1u << 16}, past[1] = {2 | 1u << 16}, wrong_size[1] = {0 | 1u << 16};
    CHECK(e.kernel.set_mirror(kPid, mirror, kMirrorWriteBack, 1, ArrayView<const Handle>(sources, 1), ArrayView<const uint32_t>(past, 1)) == status::kInvalidParameter);
    CHECK(e.kernel.set_mirror(kPid, mirror, kMirrorWriteBack, 1, ArrayView<const Handle>(sources, 1), ArrayView<const uint32_t>(wrong_size, 1)) == status::kInvalidParameter);
    CHECK(e.kernel.set_mirror(kPid, mirror, kMirrorWriteBack, 1, ArrayView<const Handle>(sources, 1)) == status::kInvalidParameter);   // (level 0 of an array: not without the words)
    CHECK(e.kernel.set_mirror(kPid, mirror, kMirrorWriteBack, 1, ArrayView<const Handle>(sources, 1), ArrayView<const uint32_t>(level1_layer1, 1)) == status::kSuccess);
    CommandWriter cw(e.allocator);
    cw.bind_pipeline(fill);
    cw.bind_uav_image(0, 0);
    cw.dispatch(1, 1, 1);
    CHECK(e.render(cw, {mirror, source}) == status::kSuccess);
    // (A8B8G8R8: 0xAABBGGRR; the shader's red at (2, 0) of the 4 x 4 is 2 / 8: 0x40, the colour's green and blue stay)
    const uint32_t before = colour(1, 1);
    CHECK(texel(source, 1, 1, 2, 0) == ((before & 0xFFFFFF00u) | 0x40u) && texel(source, 1, 0, 2, 0) == colour(1, 0) && texel(source, 0, 1, 2, 0) == colour(0, 1));
  }

  // --- A texture's descriptor carries its view (the offset words: first level, count of levels, first layer, count of layers).
  {
    Handle views = 0;
    CHECK(e.kernel.create_descriptor_heap(kPid, e.dev, DescriptorHeapType::CbvSrvUav, 4, &views) == status::kSuccess);
    DescriptorDesc srv;
    srv.kind = DescriptorKind::Srv, srv.allocation = small;
    srv.offset = 1 | (uint64_t{1} << 16) | (uint64_t{2} << 32) | (uint64_t{1} << 48);
    CHECK(e.kernel.write_descriptor(kPid, views, 0, srv) == status::kSuccess);
    srv.offset = 0;
    CHECK(e.kernel.write_descriptor(kPid, views, 1, srv) == status::kSuccess);   // (all of it)
    srv.offset = 3;
    CHECK(e.kernel.write_descriptor(kPid, views, 2, srv) == status::kInvalidParameter);   // (a level it does not have)
    srv.offset = uint64_t{2} << 32 | uint64_t{2} << 48;
    CHECK(e.kernel.write_descriptor(kPid, views, 2, srv) == status::kInvalidParameter);   // (layers 2 and 3 of three)
    srv.allocation = box, srv.offset = uint64_t{1} << 48;
    CHECK(e.kernel.write_descriptor(kPid, views, 2, srv) == status::kInvalidParameter);   // (one slice of a volume)
  }
  (void)h;
  (void)array;
}

// Depth buffers and multisampled images, which the device keeps, copied by it (graphics::kFeature2DepthCopies): each plane of a D24S8 buffer into a buffer in the depth layouts
// (CopyBufferImage's words 5 .. 8) and a rectangle of a D32 buffer's depth from one, whole subresources and one plane of them (CopyRegion and its planes word), a multisampled image
// copied whole and resolved; and what is refused - part of a subresource, a colour layout for a depth buffer, a depth layout for a colour image, a stencil plane where there is none.
void test_depth_copies() {
  Env e;
  const Handle target = e.alloc(kTarget, Format::A8R8G8B8, 8, 8);
  const Handle depth = e.alloc(kAllocDepthStencil, Format::D24S8, 8, 8), other = e.alloc(kAllocDepthStencil, Format::D24S8, 8, 8), d32 = e.alloc(kAllocDepthStencil, Format::D32Float, 8, 8);
  const Handle buf = e.buffer(kAllocVertexBuffer, 1024);
  // (the depth buffers cleared: `depth` to 0.25 and stencil 7, `other` to 1 and 0, `d32` to 0.5)
  auto clear_depth = [&](Handle d, float value, uint32_t stencil) {
    CommandWriter w(e.allocator);
    w.begin_pass(0, 1, kPassClearColor | kPassClearDepth | kPassClearStencil, {0, 0, 0, 1}, value, stencil);
    w.end_pass();
    CHECK(e.render(w, {target, d}) == status::kSuccess);
  };
  clear_depth(depth, 0.25f, 7), clear_depth(other, 1.0f, 0), clear_depth(d32, 0.5f, 0);
  auto word_at = [&](uint64_t offset) {
    LockResult r;
    uint32_t v = 0xDEADDEADu;
    if (e.kernel.lock(kPid, buf, kLockReadOnly, 0, &r) != status::kSuccess) return v;
    ByteReader in(r.data);
    in.skip(offset);
    v = in.le32();
    e.kernel.unlock(kPid, buf);
    return v;
  };
  // Each plane into the buffer: the depth as 24-bit codes (rows of 32 bytes), the stencil as bytes (rows of 8, from 512).
  {
    CommandWriter w(e.allocator);
    w.copy_buffer_image_narrow(1, 0, 0, 0, 0, 0, 8, 8, 1, 0, 32, NarrowLayout::DepthUnorm24);
    w.copy_buffer_image_narrow(1, 0, 0, 0, 0, 0, 8, 8, 1, 512, 8, NarrowLayout::Stencil8);
    CHECK(e.render(w, {depth, buf}) == status::kSuccess);
  }
  const uint32_t code = static_cast<uint32_t>(0.25 * 16777215.0 + 0.5);
  CHECK(word_at(0) == code && word_at(7 * 32 + 28) == code && word_at(512) == 0x07070707u && word_at(512 + 60) == 0x07070707u);
  // A rectangle of floats into the left half of the D32 buffer, then all of it out as 16-bit codes.
  {
    float quarter[16];
    for (float& f : quarter) f = 0.75f;
    CHECK(e.write(buf, floats(quarter, 16)));
    CommandWriter w(e.allocator);
    w.copy_buffer_image_narrow(0, 0, 0, 0, 0, 0, 4, 4, 1, 0, 16, NarrowLayout::DepthFloat32);
    w.copy_buffer_image_narrow(1, 0, 0, 0, 0, 0, 8, 8, 1, 256, 16, NarrowLayout::DepthUnorm16);
    CHECK(e.render(w, {d32, buf}) == status::kSuccess);
    const uint32_t first = word_at(256), right = word_at(256 + 12), below = word_at(256 + 7 * 16);
    printf("  depth copies: 16-bit codes %08x %08x %08x\n", static_cast<unsigned>(first), static_cast<unsigned>(right), static_cast<unsigned>(below));
    CHECK(first == 0xBFFFBFFFu && right == 0x80008000u && below == 0x80008000u);   // (0.75: 49151, 0.5: 32768)
  }
  // The stencil plane of `depth` into `other` (its depth stays 1), then both planes of `other` into `depth`.
  {
    CommandWriter w(e.allocator);
    w.copy_whole_subresources(1, 0, 0, 0, 0, 0, 8, 8, 1, 2);
    w.copy_buffer_image_narrow(1, 1, 0, 0, 0, 0, 8, 8, 2, 0, 32, NarrowLayout::DepthUnorm24);
    w.copy_buffer_image_narrow(1, 1, 0, 0, 0, 0, 8, 8, 2, 512, 8, NarrowLayout::Stencil8);
    CHECK(e.render(w, {depth, other, buf}) == status::kSuccess);
    CHECK(word_at(0) == 0xFFFFFFu && word_at(512) == 0x07070707u);
    CommandWriter all(e.allocator);
    all.copy_whole_subresources(0, 0, 0, 1, 0, 0, 8, 8);
    all.copy_buffer_image_narrow(1, 0, 0, 0, 0, 0, 8, 8, 2, 0, 32, NarrowLayout::DepthUnorm24);
    CHECK(e.render(all, {depth, other, buf}) == status::kSuccess);
    CHECK(word_at(0) == 0xFFFFFFu);
  }
  // What is refused.
  auto refused = [&](auto record, std::initializer_list<Handle> list) {
    CommandWriter w(e.allocator);
    record(w);
    return e.render(w, list) == status::kInvalidParameter;
  };
  CHECK(refused([](CommandWriter& w) { w.copy_subresource_region(1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 4, 8); }, {depth, other}));   // (part of a subresource)
  CHECK(refused([](CommandWriter& w) { w.copy_whole_subresources(1, 0, 0, 0, 0, 0, 8, 8, 1, 2); }, {d32, d32}));   // (no stencil; and one subresource into itself)
  CHECK(refused([](CommandWriter& w) { w.copy_whole_subresources(1, 0, 0, 0, 0, 0, 8, 8); }, {depth, d32}));   // (formats that differ)
  CHECK(refused([](CommandWriter& w) { w.copy_buffer_image_narrow(1, 0, 0, 0, 0, 0, 8, 8, 1, 0, 32, NarrowLayout::R8); }, {depth, buf}));
  CHECK(refused([](CommandWriter& w) { w.copy_image_to_buffer(0, 0, 0, 0, 0, 8, 8, 1, 0, 32); }, {depth, buf}));
  CHECK(refused([](CommandWriter& w) { w.copy_buffer_image_narrow(1, 0, 0, 0, 0, 0, 8, 8, 1, 0, 32, NarrowLayout::DepthFloat32); }, {target, buf}));
  CHECK(refused([](CommandWriter& w) { w.copy_buffer_image_narrow(1, 0, 0, 0, 0, 0, 8, 8, 1, 0, 8, NarrowLayout::Stencil8); }, {d32, buf}));
  CHECK(refused([](CommandWriter& w) { w.copy_whole_subresources(1, 0, 0, 0, 0, 0, 8, 8, 1, 1); }, {target, e.alloc(kTarget, Format::A8R8G8B8, 8, 8)}));   // (planes of colour images)
  // A multisampled image copied whole into another, then resolved.
  AllocationDesc md;
  md.flags = kAllocRenderTarget, md.format = Format::A8R8G8B8, md.width = md.height = 8, md.samples = 4;
  Handle ms_a = 0, ms_b = 0;
  CHECK(e.kernel.create_allocation(kPid, e.dev, md, &ms_a) == status::kSuccess && e.kernel.create_allocation(kPid, e.dev, md, &ms_b) == status::kSuccess);
  CHECK(e.clear(ms_a, {0, 1, 0, 1}) == status::kSuccess && e.clear(ms_b, {1, 0, 0, 1}) == status::kSuccess);
  {
    CommandWriter w(e.allocator);
    w.copy_whole_subresources(1, 0, 0, 0, 0, 0, 8, 8);
    w.resolve_region(2, 1);
    CHECK(e.render(w, {ms_a, ms_b, target}) == status::kSuccess);
    CHECK(e.pixel(target, 0, 0) == 0xFF00FF00u && e.pixel(target, 7, 7) == 0xFF00FF00u);
  }
}

// --- Direct3D 12's optional rasterizer features on the CPU renderer: depth bounds, programmable sample positions, view instancing, coarse shading, barycentrics ---------------

// A colored pipeline (BuiltinShaders) with the given depth state and multisampling.
Handle colored_pipeline(Env& e, bool depth_test, bool depth_write, bool multisample = false) {
  Handle vs = 0, fs = 0, pipe = 0;
  CHECK(e.kernel.create_shader(kPid, e.dev, {ShaderKind::ColoredVertex, graphics::ShaderStage::Vertex, {}}, &vs) == status::kSuccess);
  CHECK(e.kernel.create_shader(kPid, e.dev, {ShaderKind::ColoredFragment, graphics::ShaderStage::Fragment, {}}, &fs) == status::kSuccess);
  graphics::GraphicsPipelineDesc d;
  graphics::describe_colored_pipeline(&d);
  PipelineDesc pd;
  pd.vertex_shader = vs, pd.fragment_shader = fs, pd.vertex_stride = d.vertex_stride, pd.attribute_count = 2, pd.varying_count = 1;
  pd.attributes[0] = d.attributes[0], pd.attributes[1] = d.attributes[1];
  pd.cull_mode = graphics::CullMode::None;
  pd.depth_test = depth_test, pd.depth_write = depth_write;
  pd.depth_compare = depth_test ? graphics::CompareOp::LessEqual : graphics::CompareOp::Always;
  pd.multisample = multisample;
  CHECK(e.kernel.create_pipeline(kPid, e.dev, pd, &pipe) == status::kSuccess);
  return pipe;
}

// Two triangles of a rectangle from (x0, y0) to (x1, y1) in clip space at depth z, of one colour: 6 vertices of 7 floats.
void rect_vertices(float* v, float x0, float y0, float x1, float y1, float z, float r, float g, float b) {
  const float corners[6][2] = {{x0, y0}, {x1, y0}, {x0, y1}, {x1, y0}, {x1, y1}, {x0, y1}};
  for (uint32_t k = 0; k < 6; ++k) {
    float* p = v + 7 * k;
    p[0] = corners[k][0], p[1] = corners[k][1], p[2] = z, p[3] = r, p[4] = g, p[5] = b, p[6] = 1;
  }
}

const float kIdentity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};

// The depth bounds test (SetDepthBounds): a depth buffer of 0.25 on the left half and 0.75 on the right, then a green rectangle over the whole target with the depth test off - only where
// the depth buffer's value is inside the bounds is it drawn; off, it is drawn everywhere. Lines and points are tested too. A device without the feature refuses the op.
void test_depth_bounds_kernel() {
  Env e;
  const Handle writer = colored_pipeline(e, true, true), over = colored_pipeline(e, false, false);
  const Handle rt = e.alloc(kTarget, Format::A8R8G8B8, 16, 16), ds = e.alloc(kAllocDepthStencil, Format::D32Float, 16, 16), vb = e.buffer(kAllocVertexBuffer, 18 * 28);
  float v[18 * 7];
  rect_vertices(v, -1, -1, 0, 1, 0.25f, 1, 0, 0);
  rect_vertices(v + 42, 0, -1, 1, 1, 0.75f, 1, 0, 0);
  rect_vertices(v + 84, -1, -1, 1, 1, 0.5f, 0, 1, 0);
  CHECK(e.write(vb, floats(v, 18 * 7)));
  auto draw = [&](bool enable, float min, float max) {
    CommandWriter w(e.allocator);
    w.begin_pass(0, 1, kPassClearColor | kPassClearDepth, {0, 0, 0, 1}, 1, 0);
    w.bind_pipeline(writer);
    w.bind_vertex_buffer(2);
    w.push_constants(floats(kIdentity, 16));
    w.draw(12);
    w.set_depth_bounds(enable, min, max);
    w.bind_pipeline(over);
    w.draw(6, 12);
    w.end_pass();
    return e.render(w, {rt, ds, vb});
  };
  CHECK(draw(true, 0.5f, 1.0f) == status::kSuccess);
  CHECK(e.pixel(rt, 3, 8) == 0xFFFF0000u && e.pixel(rt, 12, 8) == 0xFF00FF00u);
  CHECK(draw(true, 0.0f, 0.5f) == status::kSuccess);
  CHECK(e.pixel(rt, 3, 8) == 0xFF00FF00u && e.pixel(rt, 12, 8) == 0xFFFF0000u);
  CHECK(draw(true, 0.25f, 0.25f) == status::kSuccess);   // (inclusive bounds: exactly 0.25 passes)
  CHECK(e.pixel(rt, 3, 8) == 0xFF00FF00u && e.pixel(rt, 12, 8) == 0xFFFF0000u);
  CHECK(draw(true, 0.8f, 0.7f) == status::kSuccess);     // (min above max: nothing passes)
  CHECK(e.pixel(rt, 3, 8) == 0xFFFF0000u && e.pixel(rt, 12, 8) == 0xFFFF0000u);
  CHECK(draw(false, 0.5f, 1.0f) == status::kSuccess);
  CHECK(e.pixel(rt, 3, 8) == 0xFF00FF00u && e.pixel(rt, 12, 8) == 0xFF00FF00u);
  // (the state holds across passes of the stream, and a stream starts with it off)
  {
    CommandWriter w(e.allocator);
    w.set_depth_bounds(true, 0.5f, 1.0f);
    w.begin_pass(0, 1, kPassClearColor | kPassClearDepth, {0, 0, 0, 1}, 1, 0);
    w.bind_pipeline(writer);
    w.bind_vertex_buffer(2);
    w.push_constants(floats(kIdentity, 16));
    w.draw(12);
    w.end_pass();
    w.begin_pass(0, 1, 0, {0, 0, 0, 1}, 1, 0);
    w.bind_pipeline(over);
    w.bind_vertex_buffer(2);
    w.push_constants(floats(kIdentity, 16));
    w.draw(6, 12);
    w.end_pass();
    CHECK(e.render(w, {rt, ds, vb}) == status::kSuccess);
    // (the writer's own draw tested the cleared buffer, 1.0, inside 0.5 .. 1: drawn)
    CHECK(e.pixel(rt, 3, 8) == 0xFFFF0000u && e.pixel(rt, 12, 8) == 0xFF00FF00u);
  }
  // Refused: a bound outside 0 .. 1, NaN, an enable word other than 0 or 1.
  auto bad = [&](uint32_t enable, float min, float max) {
    CommandWriter w(e.allocator);
    w.set_depth_bounds(enable != 0, min, max);
    w.begin_pass(0, 1, kPassClearColor, {0, 0, 0, 1}, 1, 0);
    w.end_pass();
    return e.render(w, {rt, ds});
  };
  CHECK(bad(1, -0.1f, 1) == status::kInvalidParameter && bad(1, 0, 1.5f) == status::kInvalidParameter && bad(1, __builtin_nanf(""), 1) == status::kInvalidParameter);
  FakeScreen screen(8, 6);
  graphics::SoftwareDevice software(e.allocator, &screen);
  SpyDevice limited(software);
  limited.feature2_mask = ~graphics::kFeature2DepthBounds;
  Kernel kernel(e.allocator);
  AdapterConfig c;
  c.display = &screen;
  kernel.add_adapter(limited, c);
  Handle adapter = 0, dev = 0, ctx = 0, target = 0;
  CHECK(kernel.open_process(kPid) == status::kSuccess && kernel.open_adapter_from_index(kPid, 0, &adapter) == status::kSuccess && kernel.create_device(kPid, adapter, &dev) == status::kSuccess);
  CHECK(kernel.create_context(kPid, dev, ContextPriority::Normal, &ctx) == status::kSuccess);
  AllocationDesc ad;
  ad.flags = kTarget, ad.format = Format::A8R8G8B8, ad.width = ad.height = 8;
  CHECK(kernel.create_allocation(kPid, dev, ad, &target) == status::kSuccess);
  CommandWriter w(e.allocator);
  w.set_depth_bounds(true, 0, 1);
  w.begin_pass(0, kNoAllocation, kPassClearColor, {0, 0, 0, 1}, 1, 0);
  w.end_pass();
  CHECK(kernel.render(kPid, ctx, w.view(), ArrayView<const Handle>(&target, 1)) == status::kInvalidParameter);
}

// Programmable sample positions (SetSamplePositions): a white rectangle whose right edge is at x 3.75 of an 8 x 8 target of 4 samples, resolved. The standard positions cover 3 of the 4
// samples of pixel 3; positions for each pixel of a quad - the even columns' at -8 / 16, the odd ones' at 7 / 16 - cover pixel 2 wholly and pixel 3 not at all; the standard ones again
// after pixels 0. A single-sample pass takes its one position too. Malformed positions are refused.
void test_sample_positions_kernel() {
  Env e;
  const Handle pipe = colored_pipeline(e, false, false, true), plain = colored_pipeline(e, false, false, false);
  AllocationDesc d;
  d.flags = kAllocRenderTarget, d.format = Format::A8R8G8B8, d.width = d.height = 8, d.samples = 4;
  Handle ms = 0;
  CHECK(e.kernel.create_allocation(kPid, e.dev, d, &ms) == status::kSuccess);
  const Handle resolved = e.alloc(kTarget, Format::A8R8G8B8, 8, 8), single = e.alloc(kTarget, Format::A8R8G8B8, 8, 8), vb = e.buffer(kAllocVertexBuffer, 6 * 28);
  float v[6 * 7];
  rect_vertices(v, -1, -1, 3.75f / 4.0f - 1.0f, 1, 0.5f, 1, 1, 1);
  CHECK(e.write(vb, floats(v, 6 * 7)));
  auto draw = [&](std::initializer_list<graphics::SamplePosition> positions, uint32_t samples, uint32_t pixels, bool reset) {
    CommandWriter w(e.allocator);
    std::vector<graphics::SamplePosition> list(positions);
    if (pixels) w.set_sample_positions(samples, pixels, ArrayView<const graphics::SamplePosition>(list.data(), list.size()));
    if (reset) w.set_sample_positions(0, 0, ArrayView<const graphics::SamplePosition>());
    w.begin_pass(0, kNoAllocation, kPassClearColor, {0, 0, 0, 1}, 1, 0);
    w.bind_pipeline(pipe);
    w.bind_vertex_buffer(2);
    w.push_constants(floats(kIdentity, 16));
    w.draw(6);
    w.end_pass();
    w.resolve_region(1, 0);
    return e.render(w, {ms, resolved, vb});
  };
  auto red = [&](uint32_t x) { return (e.pixel(resolved, x, 4) >> 16) & 0xFF; };
  CHECK(draw({}, 0, 0, false) == status::kSuccess);
  CHECK(red(2) == 0xFF && red(3) >= 0xBE && red(3) <= 0xC0 && red(4) == 0);   // (3 of 4 samples)
  const graphics::SamplePosition l = {-8, 0}, r = {7, 0};
  CHECK(draw({l, l, l, l, r, r, r, r, l, l, l, l, r, r, r, r}, 4, 4, false) == status::kSuccess);
  CHECK(red(2) == 0xFF && red(3) == 0 && red(1) == 0xFF);
  CHECK(draw({l, l, l, l}, 4, 1, false) == status::kSuccess);   // (one pattern for every pixel: pixel 3's samples at 3.0)
  CHECK(red(3) == 0xFF && red(4) == 0);
  CHECK(draw({l, l, l, l}, 4, 1, true) == status::kSuccess);    // (and the standard ones again)
  CHECK(red(3) >= 0xBE && red(3) <= 0xC0);
  {
    // A single sample: pixel 3's centre (3.5) is inside, its sample moved to 3.9375 is not.
    CommandWriter w(e.allocator);
    const graphics::SamplePosition one[1] = {r};
    w.set_sample_positions(1, 1, ArrayView<const graphics::SamplePosition>(one, 1));
    w.begin_pass(0, kNoAllocation, kPassClearColor, {0, 0, 0, 1}, 1, 0);
    w.bind_pipeline(plain);
    w.bind_vertex_buffer(1);
    w.push_constants(floats(kIdentity, 16));
    w.draw(6);
    w.end_pass();
    CHECK(e.render(w, {single, vb}) == status::kSuccess);
    CHECK(e.pixel(single, 2, 4) == 0xFFFFFFFFu && e.pixel(single, 3, 4) == 0xFF000000u);
  }
  // Refused: 3 samples, 2 pixels, 4 pixels of 8 samples, a position past 7, words missing, and 16 samples (a count the device has not).
  auto refused = [&](uint32_t samples, uint32_t pixels, std::initializer_list<graphics::SamplePosition> positions) {
    CommandWriter w(e.allocator);
    std::vector<graphics::SamplePosition> list(positions);
    w.set_sample_positions(samples, pixels, ArrayView<const graphics::SamplePosition>(list.data(), list.size()));
    return e.render(w, {ms});
  };
  CHECK(refused(3, 1, {l, l, l}) == status::kInvalidParameter && refused(4, 2, {l, l, l, l, l, l, l, l}) == status::kInvalidParameter);
  CHECK(refused(8, 4, {}) == status::kInvalidParameter && refused(1, 1, {{8, 0}}) == status::kInvalidParameter && refused(4, 1, {l, l}) == status::kInvalidParameter);
  CHECK(refused(16, 1, {l, l, l, l, l, l, l, l, l, l, l, l, l, l, l, l}) == status::kInvalidParameter);
  // Copies of samples (ResolveRegion's kResolveCopySamples, Direct3D 12's DECOMPRESS into another resource): columns 2 .. 5 of the drawing into columns 4 .. 7 of a second image of
  // 4 samples, cleared - resolved, its column 5 is the drawing's column 3, three samples of four, exactly.
  Handle other = 0;
  CHECK(e.kernel.create_allocation(kPid, e.dev, d, &other) == status::kSuccess);
  CHECK(draw({}, 0, 0, false) == status::kSuccess);
  {
    CommandWriter w(e.allocator);
    w.begin_pass(1, kNoAllocation, kPassClearColor, {0, 0, 1, 1}, 1, 0);
    w.end_pass();
    w.copy_samples(1, 0, {2, 0, 4, 8}, 4, 0);
    w.resolve_region(2, 1);
    CHECK(e.render(w, {ms, other, resolved}) == status::kSuccess);
    CHECK(e.pixel(resolved, 4, 4) == 0xFFFFFFFFu && e.pixel(resolved, 6, 4) == 0xFF000000u && e.pixel(resolved, 1, 4) == 0xFF0000FFu);
    CHECK(red(5) >= 0xBE && red(5) <= 0xC0);
  }
  auto copy = [&](uint32_t to, uint32_t from, graphics::Rect2D region, uint32_t x, uint32_t y) {
    CommandWriter w(e.allocator);
    w.copy_samples(to, from, region, x, y);
    return e.render(w, {ms, other, resolved});
  };
  CHECK(copy(1, 0, {0, 0, 8, 8}, 0, 0) == status::kSuccess);
  CHECK(copy(0, 0, {0, 0, 4, 4}, 4, 4) == status::kInvalidParameter);   // (one image)
  CHECK(copy(2, 0, {0, 0, 4, 4}, 0, 0) == status::kInvalidParameter);   // (one sample: not the same count)
  CHECK(copy(1, 0, {4, 0, 5, 4}, 0, 0) == status::kInvalidParameter && copy(1, 0, {0, 0, 4, 4}, 5, 0) == status::kInvalidParameter && copy(1, 0, {0, 0, 0, 4}, 0, 0) == status::kInvalidParameter);
}

// The view instance (SetViewInstance): the layer and viewport offsets of the view are added to what the shaders choose (here nothing: 0), so draws of views 0 .. 2 at layer offsets
// 0 .. 2 fill the three layers of an array; a viewport offset picks the second viewport. A view of 4 or more, or offsets past the limits, are refused.
void test_view_instance_kernel() {
  Env e;
  const Handle pipe = colored_pipeline(e, false, false);
  AllocationDesc d;
  d.flags = kAllocTexture | kAllocRenderTarget, d.format = Format::A8R8G8B8, d.width = d.height = 16, d.kind = graphics::TextureKind::Texture2DArray, d.layers = 3;
  Handle slices = 0;
  CHECK(e.kernel.create_allocation(kPid, e.dev, d, &slices) == status::kSuccess);
  const Handle vb = e.buffer(kAllocVertexBuffer, 18 * 28);
  float v[18 * 7];
  rect_vertices(v, -1, -1, 1, 1, 0.5f, 1, 0, 0);
  rect_vertices(v + 42, -1, -1, 1, 1, 0.5f, 0, 1, 0);
  rect_vertices(v + 84, -1, -1, 1, 1, 0.5f, 0, 0, 1);
  CHECK(e.write(vb, floats(v, 18 * 7)));
  auto layer_pixel = [&](uint32_t layer, uint32_t x, uint32_t y) {
    LockResult lk;
    uint32_t out = 0;
    if (e.kernel.lock(kPid, slices, kLockReadOnly, 0, &lk) == status::kSuccess) {
      ByteReader in(lk.data.subview(uint64_t{layer} * 16 * 16 * 4 + (uint64_t{y} * 16 + x) * 4, 4));
      out = in.le32();
      e.kernel.unlock(kPid, slices);
    }
    return out;
  };
  CommandWriter w(e.allocator);
  w.begin_pass_layers(0, {}, kNoAllocation, kPassClearColor, {0, 0, 0, 1}, 1, 0, {0, 3, 0, 1});
  w.bind_pipeline(pipe);
  w.bind_vertex_buffer(1);
  w.push_constants(floats(kIdentity, 16));
  for (uint32_t view = 0; view < 3; ++view) {
    w.set_view_instance(view, 0, view);
    w.draw(6, 6 * view);
  }
  w.end_pass();
  CHECK(e.render(w, {slices, vb}) == status::kSuccess);
  CHECK(layer_pixel(0, 8, 8) == 0xFFFF0000u && layer_pixel(1, 8, 8) == 0xFF00FF00u && layer_pixel(2, 8, 8) == 0xFF0000FFu);
  // Viewport offsets: viewport 0 the left half, 1 the right half; the second draw at offset 1 lands in the right half.
  const Handle rt = e.alloc(kTarget, Format::A8R8G8B8, 16, 16);
  CommandWriter w2(e.allocator);
  w2.begin_pass(0, kNoAllocation, kPassClearColor, {0, 0, 0, 1}, 1, 0);
  w2.bind_pipeline(pipe);
  w2.bind_vertex_buffer(1);
  w2.push_constants(floats(kIdentity, 16));
  {
    const graphics::Viewport vps[2] = {{0, 0, 8, 16, 0, 1}, {8, 0, 8, 16, 0, 1}};
    const graphics::Rect2D scs[2] = {{0, 0, 8, 16}, {8, 0, 8, 16}};
    w2.set_viewports(ArrayView<const graphics::Viewport>(vps, 2));
    w2.set_scissors(ArrayView<const graphics::Rect2D>(scs, 2));
  }
  w2.set_view_instance(1, 1, 0);
  w2.draw(6, 6);   // (green, right half)
  w2.set_view_instance(0, 0, 0);
  w2.draw(6, 0);   // (red, left half)
  w2.end_pass();
  CHECK(e.render(w2, {rt, vb}) == status::kSuccess);
  CHECK(e.pixel(rt, 3, 8) == 0xFFFF0000u && e.pixel(rt, 12, 8) == 0xFF00FF00u);
  auto refused = [&](uint32_t view, uint32_t vp, uint32_t layer) {
    CommandWriter b(e.allocator);
    b.set_view_instance(view, vp, layer);
    return e.render(b, {rt});
  };
  CHECK(refused(4, 0, 0) == status::kInvalidParameter && refused(0, 16, 0) == status::kInvalidParameter && refused(0, 0, 2048) == status::kInvalidParameter);
  CHECK(refused(3, 15, 2047) == status::kSuccess);
}

// Coarse pixel shading (SetShadingRate): a gradient across a 32 x 32 target (the colour of the colored pipeline's vertices, interpolated) drawn at 2x2 has the same colour in each 2 x 2
// block, at 1x1 not; 4x4 blocks of 4; the shading-rate image's tiles (16 x 16) choose their own rate with the image combiner OVERRIDE; MIN of the set rate and the image's. The rate
// sanitizing is checked on its own: what a pass of 4 samples makes of 4x4.
void test_shading_rate_kernel() {
  CHECK(graphics::sanitize_shading_rate(0xA, 1) == 0xA && graphics::sanitize_shading_rate(0xA, 2) == 0x6 && graphics::sanitize_shading_rate(0xA, 4) == 0x5);
  CHECK(graphics::sanitize_shading_rate(0x5, 8) == 0x4 && graphics::sanitize_shading_rate(0x5, 16) == 0x0 && graphics::sanitize_shading_rate(0x9, 2) == 0x5);
  CHECK(graphics::combine_shading_rate(graphics::ShadingRateCombiner::Sum, 0x5, 0x5) == 0xA && graphics::combine_shading_rate(graphics::ShadingRateCombiner::Sum, 0x9, 0x5) == 0xA);
  CHECK(graphics::combine_shading_rate(graphics::ShadingRateCombiner::Min, 0x1, 0x4) == 0x0 && graphics::combine_shading_rate(graphics::ShadingRateCombiner::Max, 0x1, 0x4) == 0x5);
  CHECK(graphics::combine_shading_rate(graphics::ShadingRateCombiner::Override, 0x1, 0x6) == 0x6 && graphics::combine_shading_rate(graphics::ShadingRateCombiner::Passthrough, 0x1, 0x6) == 0x1);
  Env e;
  const Handle pipe = colored_pipeline(e, false, false);
  const Handle rt = e.alloc(kTarget, Format::A8R8G8B8, 32, 32), vb = e.buffer(kAllocVertexBuffer, 6 * 28);
  // (red from 0 at the left edge to 1 at the right, green from 0 at the top to 1 at the bottom)
  const float v[6 * 7] = {-1, -1, 0.5f, 0, 0, 0, 1, 1, -1, 0.5f, 1, 0, 0, 1, -1, 1, 0.5f, 0, 1, 0, 1, 1, -1, 0.5f, 1, 0, 0, 1, 1, 1, 0.5f, 1, 1, 0, 1, -1, 1, 0.5f, 0, 1, 0, 1};
  CHECK(e.write(vb, floats(v, 6 * 7)));
  AllocationDesc id;
  id.flags = kAllocTexture, id.format = Format::R8Uint, id.width = id.height = 2;
  Handle image = 0;
  CHECK(e.kernel.create_allocation(kPid, e.dev, id, &image) == status::kSuccess);
  auto draw = [&](uint32_t rate, uint32_t primitive_combiner, uint32_t image_combiner, bool with_image) {
    CommandWriter w(e.allocator);
    w.set_shading_rate(rate, primitive_combiner, image_combiner, with_image ? 2 : kNoAllocation);
    w.begin_pass(0, kNoAllocation, kPassClearColor, {0, 0, 0, 1}, 1, 0);
    w.bind_pipeline(pipe);
    w.bind_vertex_buffer(1);
    w.push_constants(floats(kIdentity, 16));
    w.draw(6);
    w.end_pass();
    return e.render(w, {rt, vb, image});
  };
  // Whether every block of bw x bh pixels from (x0, y0) of w x h has one colour ...
  auto uniform = [&](uint32_t x0, uint32_t y0, uint32_t w, uint32_t h, uint32_t bw, uint32_t bh) {
    for (uint32_t y = y0; y < y0 + h; ++y)
      for (uint32_t x = x0; x < x0 + w; ++x)
        if (e.pixel(rt, x, y) != e.pixel(rt, x - (x - x0) % bw, y - (y - y0) % bh)) return false;
    return true;
  };
  // ... and the blocks twice as wide, or twice as high, have not (the gradient runs both ways): the coarse pixels are bw x bh.
  auto blocks = [&](uint32_t x0, uint32_t y0, uint32_t w, uint32_t h, uint32_t bw, uint32_t bh) {
    return uniform(x0, y0, w, h, bw, bh) && !uniform(x0, y0, w, h, 2 * bw, bh) && !uniform(x0, y0, w, h, bw, 2 * bh);
  };
  CHECK(draw(0x0, 0, 0, false) == status::kSuccess && blocks(0, 0, 32, 32, 1, 1));
  CHECK(draw(0x5, 0, 0, false) == status::kSuccess && blocks(0, 0, 32, 32, 2, 2));
  CHECK(draw(0xA, 0, 0, false) == status::kSuccess && blocks(0, 0, 32, 32, 4, 4));
  CHECK(draw(0x6, 0, 0, false) == status::kSuccess && blocks(0, 0, 32, 32, 2, 4));
  CHECK(draw(0x9, 0, 0, false) == status::kSuccess && blocks(0, 0, 32, 32, 4, 2));
  // (the coarse pixel's colour is the gradient at its centre: the 2x2 block at (0, 0) has the colour of (1, 1), which is between pixel (0, 0)'s and (1, 1)'s at 1x1)
  CHECK(draw(0x5, 0, 0, false) == status::kSuccess);
  const uint32_t coarse = e.pixel(rt, 4, 4);
  CHECK(draw(0x0, 0, 0, false) == status::kSuccess);
  const uint32_t fine_lo = e.pixel(rt, 4, 4), fine_hi = e.pixel(rt, 5, 5);
  CHECK(((coarse >> 16) & 0xFF) >= ((fine_lo >> 16) & 0xFF) && ((coarse >> 16) & 0xFF) <= ((fine_hi >> 16) & 0xFF) && coarse != fine_lo);
  // The image: tiles (0, 0) 1x1, (1, 0) 2x2, (0, 1) 4x4, (1, 1) 2x1 - stored rows as the image's rows (the device reads texel (x, y) of tile (x, y)).
  {
    LockResult lk;
    CHECK(e.kernel.lock(kPid, image, kLockDiscard, 0, &lk) == status::kSuccess);
    const uint8_t rates[4] = {0x0, 0x5, 0xA, 0x4};
    for (uint32_t y = 0; y < 2; ++y)
      for (uint32_t x = 0; x < 2; ++x) lk.data[uint64_t{y} * lk.pitch + x] = rates[y * 2 + x];
    e.kernel.unlock(kPid, image);
  }
  CHECK(draw(0x0, 0, 1, true) == status::kSuccess);   // (image combiner OVERRIDE)
  // (tile rows: the image's row y is the target's pixel rows 16y .. 16y + 15 as the device keeps them - check either way up)
  const bool top_first = uniform(16, 0, 16, 16, 2, 2);
  const uint32_t t0 = top_first ? 0 : 16, t1 = top_first ? 16 : 0;
  CHECK(blocks(0, t0, 16, 16, 1, 1) && blocks(16, t0, 16, 16, 2, 2) && blocks(0, t1, 16, 16, 4, 4) && blocks(16, t1, 16, 16, 2, 1));
  CHECK(draw(0x1, 0, 2, true) == status::kSuccess);   // (MIN of 1x2 and each tile's: 1x1, 1x2, 1x2, 1x1)
  CHECK(blocks(0, t0, 16, 16, 1, 1) && blocks(16, t0, 16, 16, 1, 2) && blocks(0, t1, 16, 16, 1, 2) && blocks(16, t1, 16, 16, 1, 1));
  CHECK(draw(0x0, 0, 0, true) == status::kSuccess && blocks(0, 0, 32, 32, 1, 1));   // (PASSTHROUGH: the image is not used)
  // Refused: a rate that is not one, a combiner past SUM, an image that is not R8_UINT.
  const Handle wrong = e.alloc(kTexture, Format::A8R8G8B8, 2, 2);
  auto refused = [&](uint32_t rate, uint32_t c0, uint32_t c1, Handle img) {
    CommandWriter w(e.allocator);
    w.set_shading_rate(rate, c0, c1, img ? 0 : kNoAllocation);
    return img ? e.render(w, {img}) : e.render(w, {rt});
  };
  CHECK(refused(0x2, 0, 0, 0) == status::kInvalidParameter && refused(0x0, 5, 0, 0) == status::kInvalidParameter && refused(0x0, 0, 5, 0) == status::kInvalidParameter);
  CHECK(refused(0x0, 0, 1, wrong) == status::kInvalidParameter && refused(0x0, 0, 1, image) == status::kSuccess);
}

// The Shader Model 6.1+ values the DXIL translator reads, run on the interpreter: SV_ViewID in a vertex and a pixel shader, SV_Barycentrics (perspective-correct and noperspective),
// GetAttributeAtVertex, SV_ShadingRate written by a vertex shader and read by a pixel shader, IsHelperLane; what each needs of the device; the TGSI emitter refuses them; truncated and
// flipped blobs never crash.
void test_dxil_sm61_values() {
  HostAllocator allocator;
  auto compile = [&](const char* name, UniquePointer<D3d9Program>* out) {
    const std::vector<uint8_t> blob = read_blob(name);
    return !blob.empty() && D3d9Program::compile_dxbc(allocator, ConstByteView(blob.data(), blob.size()), out) == status::kSuccess && *out;
  };
  UniquePointer<D3d9Program> vs_view, ps_view, ps_bary, ps_attr, ps_rate, vs_rate, ps_helper;
  CHECK(compile("dxil_vs_view.dxil", &vs_view) && compile("dxil_ps_view.dxil", &ps_view) && compile("dxil_ps_bary.dxil", &ps_bary) && compile("dxil_ps_attr.dxil", &ps_attr));
  CHECK(compile("dxil_ps_rate.dxil", &ps_rate) && compile("dxil_vs_rate.dxil", &vs_rate) && compile("dxil_ps_helper.dxil", &ps_helper));
  if (!vs_view || !ps_view || !ps_bary || !ps_attr || !ps_rate || !vs_rate || !ps_helper) return;
  CHECK(!(vs_view->required_features2() & graphics::kFeature2ViewInstancing) && !(ps_view->required_features2() & graphics::kFeature2ViewInstancing));   // (SV_ViewID is 0 without it)
  CHECK((ps_bary->required_features2() & graphics::kFeature2Barycentrics) && (ps_attr->required_features2() & graphics::kFeature2Barycentrics));
  CHECK((ps_rate->required_features2() & graphics::kFeature2ShadingRate) && (vs_rate->required_features2() & graphics::kFeature2ShadingRate));
  const uint8_t constants[256] = {};
  graphics::Vec4 attributes[graphics::kMaxVertexAttributes] = {{0.5f, 0.25f, 0.5f, 1}, {0.1f, 0.2f, 0.3f, 6}};
  // SV_ViewID of a vertex shader: red for view 0, green for 1.
  for (uint32_t view = 0; view < 2; ++view) {
    graphics::VertexContext vc;
    vc.view_id = view;
    graphics::ShaderIo io;
    vs_view->run_vertex_ex(attributes, constants, vc, &io);
    CHECK(io.varying[0].x == (view == 0 ? 1.0f : 0.0f) && io.varying[0].y == (view == 1 ? 1.0f : 0.0f));
  }
  // ... of a pixel shader: blue for view 1.
  graphics::Vec4 varyings[graphics::kMaxVaryings] = {{0.25f, 0.5f, 0, 1}};
  graphics::Vec4 color[graphics::kMaxColorTargets];
  graphics::FragmentContext fc;
  fc.view_id = 1;
  CHECK(ps_view->run_fragment_ex(varyings, fc, constants, nullptr, color) && color[0].x == 0.25f && color[0].y == 0.5f && color[0].z == 1.0f);
  fc.view_id = 0;
  CHECK(ps_view->run_fragment_ex(varyings, fc, constants, nullptr, color) && color[0].z == 0.0f);
  // SV_Barycentrics: the perspective-correct weights in red, green, blue, the noperspective first one in alpha.
  fc.barycentrics = {0.2f, 0.3f, 0.5f, 0};
  fc.barycentrics_linear = {0.7f, 0.1f, 0.2f, 0};
  CHECK(ps_bary->run_fragment_ex(varyings, fc, constants, nullptr, color) && color[0].x == 0.2f && color[0].y == 0.3f && color[0].z == 0.5f && color[0].w == 0.7f);
  // GetAttributeAtVertex: vertex 1's colour, and the alpha of the three mixed by the barycentrics.
  const graphics::Vec4 v0[graphics::kMaxVaryings] = {{1, 0, 0, 0.0f}}, v1[graphics::kMaxVaryings] = {{0, 1, 0, 1.0f}}, v2[graphics::kMaxVaryings] = {{0, 0, 1, 0.5f}};
  fc.vertex_varyings[0] = v0, fc.vertex_varyings[1] = v1, fc.vertex_varyings[2] = v2;
  CHECK(ps_attr->run_fragment_ex(varyings, fc, constants, nullptr, color) && color[0].x == 0 && color[0].y == 1 && color[0].z == 0);
  CHECK(color[0].w > 0.549f && color[0].w < 0.551f);   // (0.3 * 1 + 0.5 * 0.5)
  // SV_ShadingRate: a vertex shader's from the colour's alpha (6: 2x4), a pixel shader's in blue (rate / 16) with its position.
  {
    graphics::ShaderIo io;
    vs_rate->run_vertex_ex(attributes, constants, graphics::VertexContext(), &io);
    CHECK(io.shading_rate == 6);
    fc.position = {4, 12, 0.5f, 1};
    fc.shading_rate = 5;
    CHECK(ps_rate->run_fragment_ex(varyings, fc, constants, nullptr, color) && color[0].x == 0.5f && color[0].y == 0.5f && color[0].z == 5.0f / 16.0f);
  }
  // IsHelperLane: 0 for a pixel that is drawn, 1 for a helper run.
  fc.helper = false;
  CHECK(ps_helper->run_fragment_ex(varyings, fc, constants, nullptr, color) && color[0].x == 0.0f);
  fc.helper = true;
  CHECK(ps_helper->run_fragment_ex(varyings, fc, constants, nullptr, color) && color[0].x == 1.0f);
  // (no TGSI for the barycentric and shading-rate ones: virgl is not given them; SV_ViewID is the immediate 0 there - every draw is view 0 without view instancing)
  for (D3d9Program* p : {ps_bary.get(), ps_attr.get(), ps_rate.get(), vs_rate.get()}) {
    String text(allocator);
    CHECK(p->emit_tgsi(&text, graphics::kMaxVaryings) != status::kSuccess);
  }
  for (D3d9Program* p : {vs_view.get(), ps_view.get()}) {
    String text(allocator);
    CHECK(p->emit_tgsi(&text, graphics::kMaxVaryings) == status::kSuccess);
  }
  // Truncated and with a bit flipped: refused or compiled, never a crash.
  for (const char* name : {"dxil_vs_view.dxil", "dxil_ps_view.dxil", "dxil_ps_bary.dxil", "dxil_ps_attr.dxil", "dxil_ps_rate.dxil", "dxil_vs_rate.dxil", "dxil_ps_helper.dxil"}) {
    const std::vector<uint8_t> blob = read_blob(name);
    for (size_t n = 0; n < blob.size(); n += 13) {
      UniquePointer<D3d9Program> program;
      D3d9Program::compile_dxbc(allocator, ConstByteView(blob.data(), n), &program);
    }
    for (size_t bit = 0; bit < blob.size() * 8; bit += 41) {
      std::vector<uint8_t> flipped = blob;
      flipped[bit / 8] ^= static_cast<uint8_t>(1u << (bit % 8));
      UniquePointer<D3d9Program> program;
      D3d9Program::compile_dxbc(allocator, ConstByteView(flipped.data(), flipped.size()), &program);
    }
  }
}

// Barycentrics on the CPU renderer through the kernel: the DXIL pixel shader of SV_Barycentrics on a triangle whose vertices have w 1 (perspective-correct and linear agree), and
// GetAttributeAtVertex - the triangle all of vertex 1's colour. A clipped triangle (one vertex past the near plane) keeps the weights of the whole triangle.
void test_barycentrics_kernel() {
  Env e;
  namespace b = conformance_blobs;
  auto shader = [&](const char* name, graphics::ShaderStage stage, Handle* out) {
    const std::vector<uint8_t> code = read_blob(name);
    ShaderDesc sd;
    sd.kind = ShaderKind::Dxbc, sd.stage = stage, sd.code = ConstByteView(code.data(), code.size());
    return !code.empty() && e.kernel.create_shader(kPid, e.dev, sd, out) == status::kSuccess;
  };
  Handle vs = 0, bary = 0, attr = 0;
  ShaderDesc sd;
  sd.kind = ShaderKind::Dxbc, sd.stage = graphics::ShaderStage::Vertex, sd.code = ConstByteView(b::k_vs, sizeof b::k_vs);
  CHECK(e.kernel.create_shader(kPid, e.dev, sd, &vs) == status::kSuccess);
  CHECK(shader("dxil_ps_bary.dxil", graphics::ShaderStage::Fragment, &bary) && shader("dxil_ps_attr.dxil", graphics::ShaderStage::Fragment, &attr));
  auto pipeline = [&](Handle ps) {
    PipelineDesc pd;
    pd.vertex_shader = vs, pd.fragment_shader = ps, pd.vertex_stride = 28, pd.attribute_count = 2, pd.varying_count = 3;
    pd.attributes[0] = {0, graphics::VertexFormat::Float3, 0};
    pd.attributes[1] = {1, graphics::VertexFormat::Float4, 12};
    pd.cull_mode = graphics::CullMode::None;
    Handle out = 0;
    CHECK(e.kernel.create_pipeline(kPid, e.dev, pd, &out) == status::kSuccess);
    return out;
  };
  const Handle with_bary = pipeline(bary), with_attr = pipeline(attr);
  const Handle rt = e.alloc(kTarget, Format::A8R8G8B8, 16, 16), vb = e.buffer(kAllocVertexBuffer, 3 * 28);
  // (vertices at pixels (0, 16), (16, 16), (0, 0) of the target - y up in clip space -: red, green, blue)
  const float v[21] = {-1, -1, 0.5f, 1, 0, 0, 1, 1, -1, 0.5f, 0, 1, 0, 1, -1, 1, 0.5f, 0, 0, 1, 1};
  CHECK(e.write(vb, floats(v, 21)));
  auto draw = [&](Handle pipe) {
    CommandWriter w(e.allocator);
    w.begin_pass(0, kNoAllocation, kPassClearColor, {0, 0, 0, 0}, 1, 0);
    w.bind_pipeline(pipe);
    w.bind_vertex_buffer(1);
    w.push_constants(floats(kIdentity, 16));
    w.draw(3);
    w.end_pass();
    return e.render(w, {rt, vb});
  };
  CHECK(draw(with_bary) == status::kSuccess);
  // Pixel (2, 2) from the top - centre (2.5, 2.5): weights of vertex 1 (16, 16) is x / 16 = 0.156, of vertex 2 (0, 0) is 1 - y / 16 = 0.844 ... for a pixel counted from the top;
  // look for the lit pixel whose weights sum to one and check them against its own coordinates.
  bool found = false;
  for (uint32_t y = 0; y < 16 && !found; ++y)
    for (uint32_t x = 0; x < 16 && !found; ++x) {
      const uint32_t p = e.pixel(rt, x, y);
      const float r = static_cast<float>((p >> 16) & 0xFF) / 255.0f, g = static_cast<float>((p >> 8) & 0xFF) / 255.0f, bl = static_cast<float>(p & 0xFF) / 255.0f, a = static_cast<float>(p >> 24) / 255.0f;
      if (x != 4 || p == 0) continue;
      found = true;
      // (the sum is 1; green is (x + 0.5) / 16; red equals alpha - perspective-correct and linear agree with w 1)
      CHECK(r + g + bl > 0.98f && r + g + bl < 1.02f && g > 4.5f / 16.0f - 0.01f && g < 4.5f / 16.0f + 0.01f && r > a - 0.01f && r < a + 0.01f);
    }
  CHECK(found);
  CHECK(draw(with_attr) == status::kSuccess);
  uint32_t lit = 0, green = 0;
  for (uint32_t y = 0; y < 16; ++y)
    for (uint32_t x = 0; x < 16; ++x) {
      const uint32_t p = e.pixel(rt, x, y);
      if (p == 0) continue;
      ++lit;
      if ((p & 0x00FFFFFFu) == 0x0000FF00u) ++green;
    }
  CHECK(lit > 100 && green == lit);
  // A device without barycentrics refuses the pixel shader.
  FakeScreen screen(8, 6);
  graphics::SoftwareDevice software(e.allocator, &screen);
  SpyDevice limited(software);
  limited.feature2_mask = ~graphics::kFeature2Barycentrics;
  Kernel kernel(e.allocator);
  AdapterConfig c;
  c.display = &screen;
  kernel.add_adapter(limited, c);
  Handle adapter = 0, dev = 0, ps = 0;
  CHECK(kernel.open_process(kPid) == status::kSuccess && kernel.open_adapter_from_index(kPid, 0, &adapter) == status::kSuccess && kernel.create_device(kPid, adapter, &dev) == status::kSuccess);
  const std::vector<uint8_t> code = read_blob("dxil_ps_bary.dxil");
  sd.stage = graphics::ShaderStage::Fragment, sd.code = ConstByteView(code.data(), code.size());
  CHECK(kernel.create_shader(kPid, dev, sd, &ps) == status::kNotSupported);
}

// Multisampling in shaders (graphics::DeviceFeature3 bits 0-7) on the CPU renderer, DXBC and DXIL: samples of multisampled colour images, arrays and depth buffers read through
// Texture2DMS units (Load, GetDimensions, GetSamplePosition), the rasterizer's count and positions, centroid, noperspective and flat inputs, shaders run for each sample
// (SV_SampleIndex, inputs at the sample), EvaluateAttribute*, SV_Coverage in and out and SV_InnerCoverage; and the refusals of a device without them.
void test_msaa_shaders_kernel() {
  Env e;
  auto shader = [&](Kernel& k, Handle device, const char* name, graphics::ShaderStage stage, Handle* out) {
    const std::vector<uint8_t> code = read_blob(name);
    ShaderDesc sd;
    sd.kind = ShaderKind::Dxbc, sd.stage = stage, sd.code = ConstByteView(code.data(), code.size());
    if (code.empty()) return status::kNotFound;
    return k.create_shader(kPid, device, sd, out);
  };
  // (every fixture is a program the kernel takes)
  for (const char* name : {"vs_msaa.dxbc", "ps_ms_load.dxbc", "ps_ms_info.dxbc", "ps_ms_array.dxbc", "ps_ms_depth.dxbc", "ps_ms_stencil.dxbc", "ps_centre.dxbc", "ps_centroid.dxbc",
                           "ps_noperspective.dxbc", "ps_perspective.dxbc", "ps_flat.dxbc", "ps_sample_index.dxbc", "ps_sample_input.dxbc", "ps_eval.dxbc", "ps_coverage.dxbc", "ps_omask.dxbc",
                           "ps_rt_info.dxbc", "ps_inner.dxbc", "dxil_ps_ms_load.dxil", "dxil_ps_ms_info.dxil", "dxil_ps_ms_array.dxil", "dxil_ps_ms_depth.dxil", "dxil_ps_centroid.dxil",
                           "dxil_ps_noperspective.dxil", "dxil_ps_flat.dxil", "dxil_ps_sample_index.dxil", "dxil_ps_sample_input.dxil", "dxil_ps_eval.dxil", "dxil_ps_coverage.dxil",
                           "dxil_ps_omask.dxil", "dxil_ps_rt_info.dxil", "dxil_ps_inner.dxil"}) {
    const std::vector<uint8_t> code = read_blob(name);
    UniquePointer<D3d9Program> program;
    const NtStatus st = D3d9Program::compile_dxbc(e.allocator, ConstByteView(code.data(), code.size()), &program);
    if (st != status::kSuccess) printf("  msaa fixture %s: %x\n", name, static_cast<unsigned>(st));
    CHECK(st == status::kSuccess);
  }
  Handle vs = 0;
  CHECK(shader(e.kernel, e.dev, "vs_msaa.dxbc", graphics::ShaderStage::Vertex, &vs) == status::kSuccess);
  // A pipeline of vs_msaa and the pixel shader `name` (textures: units it reads), with coverage per sample.
  auto pipeline = [&](const char* name, uint32_t textures, bool conservative = false) {
    Handle ps = 0, out = 0;
    CHECK(shader(e.kernel, e.dev, name, graphics::ShaderStage::Fragment, &ps) == status::kSuccess);
    PipelineDesc pd;
    pd.vertex_shader = vs, pd.fragment_shader = ps, pd.vertex_stride = 28, pd.attribute_count = 2, pd.varying_count = 2, pd.texture_count = textures;
    pd.attributes[0] = {0, graphics::VertexFormat::Float3, 0};
    pd.attributes[1] = {1, graphics::VertexFormat::Float4, 12};
    pd.cull_mode = graphics::CullMode::None;
    pd.multisample = true;
    pd.conservative = conservative;
    CHECK(e.kernel.create_pipeline(kPid, e.dev, pd, &out) == status::kSuccess);
    return out;
  };
  struct V {
    float x, y, z, r, g, b, w;
  };
  // (a point of the target in pixels (y down) as a vertex: of a target `size` pixels wide and high)
  auto at = [](float px, float py, float size, float r, float g, float b, float w = 1, float z = 0.5f) { return V{px / size * 2 - 1, 1 - py / size * 2, z, r, g, b, w}; };
  const Handle vb = e.buffer(kAllocVertexBuffer, 64 * sizeof(V));
  auto draw = [&](Handle pipe, Handle target, const V* v, uint32_t count, Handle depth = 0, uint32_t first_layer = 0, uint32_t layers = 0) {
    if (!e.write(vb, ConstByteView(reinterpret_cast<const uint8_t*>(v), uint64_t{count} * sizeof(V)))) return status::kInvalidParameter;
    CommandWriter w(e.allocator);
    const uint32_t flags = kPassClearColor | (depth ? kPassClearDepth : 0u);
    if (layers) w.begin_pass_layers(0, {}, depth ? 2u : kNoAllocation, flags, {0, 0, 0, 1}, 1.0f, 0, {first_layer, layers, depth ? first_layer : 0u, depth ? layers : 1u});
    else w.begin_pass(0, depth ? 2u : kNoAllocation, flags, {0, 0, 0, 1}, 1.0f, 0);
    w.bind_pipeline(pipe);
    w.bind_vertex_buffer(1);
    w.draw(count);
    w.end_pass();
    return e.render(w, {target, vb, depth ? depth : target});
  };
  // A rectangle of quads from (x0, y0) to (x1, y1) pixels.
  auto rect = [&](V* v, float x0, float y0, float x1, float y1, float size, float r, float g, float b, float z = 0.5f) {
    v[0] = at(x0, y0, size, r, g, b, 1, z), v[1] = at(x1, y0, size, r, g, b, 1, z), v[2] = at(x0, y1, size, r, g, b, 1, z);
    v[3] = at(x0, y1, size, r, g, b, 1, z), v[4] = at(x1, y0, size, r, g, b, 1, z), v[5] = at(x1, y1, size, r, g, b, 1, z);
  };
  // Reads the samples of a 16 x 16 multisampled image of four samples through `reader` (a Texture2DMS reader: pixel (x, y) of the 64 x `rows` result is sample x mod 4 of (x / 4, y)).
  const Handle readout = e.alloc(kTarget, Format::A8R8G8B8, 64, 48);
  auto read_samples = [&](Handle reader, Handle image, uint32_t rows = 16) {
    V full[3] = {at(0, 0, 64, 1, 1, 1), at(128, 0, 64, 1, 1, 1), at(0, 128, 64, 1, 1, 1)};
    if (!e.write(vb, ConstByteView(reinterpret_cast<const uint8_t*>(full), sizeof full))) return status::kInvalidParameter;
    CommandWriter w(e.allocator);
    w.begin_pass(0, kNoAllocation, kPassClearColor, {0, 0, 0, 0}, 1.0f, 0);
    w.bind_pipeline(reader);
    w.bind_vertex_buffer(1);
    w.bind_texture(0, 2, graphics::SamplerDesc());
    w.draw(3);
    w.end_pass();
    (void)rows;
    return e.render(w, {readout, vb, image});
  };
  auto sample_of = [&](uint32_t x, uint32_t y, uint32_t k) { return e.pixel(readout, x * 4 + k, y); };
  const graphics::SamplePosition* four = graphics::standard_sample_positions(4);
  auto covered_by = [&](float x0, float x1, uint32_t px, uint32_t k) {   // (sample k of pixel column px inside x0 .. x1)
    const float sx = static_cast<float>(px) + 0.5f + static_cast<float>(four[k].x) / 16.0f;
    return sx >= x0 && sx < x1;
  };

  AllocationDesc md;
  md.flags = kAllocRenderTarget | kAllocTexture, md.format = Format::A8R8G8B8, md.width = md.height = 16, md.samples = 4;
  Handle ms = 0;
  CHECK(e.kernel.create_allocation(kPid, e.dev, md, &ms) == status::kSuccess);
  const Handle ms_load = pipeline("ps_ms_load.dxbc", 1), ms_info = pipeline("ps_ms_info.dxbc", 1);
  const Handle colour = pipeline("ps_flat.dxbc", 0);
  // Every sample: a red rectangle from x = 4.3 to 12.6 over the whole height.
  {
    V v[6];
    rect(v, 4.3f, 0, 12.6f, 16, 16, 1, 0, 0);
    CHECK(draw(colour, ms, v, 6) == status::kSuccess);
    CHECK(read_samples(ms_load, ms) == status::kSuccess);
    uint32_t right = 0, wrong = 0;
    for (uint32_t x = 0; x < 16; ++x)
      for (uint32_t k = 0; k < 4; ++k) {
        const uint32_t want = covered_by(4.3f, 12.6f, x, k) ? 0xFFFF0000u : 0xFF000000u;
        (sample_of(x, 7, k) == want ? right : wrong) += 1;
      }
    printf("  msaa: %u samples as drawn, %u not (pixel 4: %08x %08x %08x %08x)\n", right, wrong, sample_of(4, 7, 0), sample_of(4, 7, 1), sample_of(4, 7, 2), sample_of(4, 7, 3));
    CHECK(wrong == 0 && right == 64);
    // GetDimensions and GetSamplePosition
    CHECK(read_samples(ms_info, ms) == status::kSuccess);
    bool info = true;
    for (uint32_t k = 0; k < 4; ++k) {
      const uint32_t p = e.pixel(readout, k, 3);   // (readout x = k: GetSamplePosition(k))
      info = info && ((p >> 16) & 0xFF) == 16 && ((p >> 8) & 0xFF) == 16 && (p & 0xFF) == 4 * 16 + static_cast<uint32_t>(four[k].x + 8) && (p >> 24) == static_cast<uint32_t>(four[k].y + 8);
    }
    CHECK(info);
  }
  // The rasterizer's count and positions, drawn into the multisampled image (every sample the same: the pixel's)
  {
    const Handle rt_info = pipeline("ps_rt_info.dxbc", 0);
    V v[6];
    rect(v, 0, 0, 16, 16, 16, 1, 1, 1);
    CHECK(draw(rt_info, ms, v, 6) == status::kSuccess && read_samples(ms_load, ms) == status::kSuccess);
    bool ok = true;
    for (uint32_t x = 0; x < 8; ++x) {
      const uint32_t p = sample_of(x, 2, 1);
      ok = ok && ((p >> 16) & 0xFF) == 64 && ((p >> 8) & 0xFF) == static_cast<uint32_t>(four[x % 4].x + 8) && (p & 0xFF) == static_cast<uint32_t>(four[x % 4].y + 8);
    }
    CHECK(ok);
  }
  // A multisampled array: layer 0 red on the left half, layer 1 green on the right, read through Texture2DMSArray (and its GetDimensions); then layer 1 resolved.
  {
    AllocationDesc ad = md;
    ad.kind = graphics::TextureKind::Texture2DArray, ad.layers = 2;
    Handle array = 0;
    CHECK(e.kernel.create_allocation(kPid, e.dev, ad, &array) == status::kSuccess);
    V v[6];
    rect(v, 0, 0, 8, 16, 16, 1, 0, 0);
    CHECK(draw(colour, array, v, 6, 0, 0, 1) == status::kSuccess);
    rect(v, 8, 0, 16, 16, 16, 0, 1, 0);
    CHECK(draw(colour, array, v, 6, 0, 1, 1) == status::kSuccess);
    const Handle ms_array = pipeline("ps_ms_array.dxbc", 1);
    CHECK(read_samples(ms_array, array) == status::kSuccess);
    const uint32_t info = e.pixel(readout, 0, 40);
    printf("  msaa array: %08x %08x %08x %08x, dimensions %08x\n", e.pixel(readout, 4, 2), e.pixel(readout, 60, 2), e.pixel(readout, 4, 18), e.pixel(readout, 60, 18), info);
    CHECK(e.pixel(readout, 4, 2) == 0xFFFF0000u && e.pixel(readout, 60, 2) == 0xFF000000u && e.pixel(readout, 4, 18) == 0xFF000000u && e.pixel(readout, 60, 18) == 0xFF00FF00u);
    CHECK(info == 0x04101002u);   // (16 wide, 16 high, 2 layers, 4 samples)
    const Handle plain = e.alloc(kTarget, Format::A8R8G8B8, 16, 16);
    CommandWriter w(e.allocator);
    w.resolve_region_layer(0, 1, 0, 1);
    CHECK(e.render(w, {plain, array}) == status::kSuccess);
    CHECK(e.pixel(plain, 12, 4) == 0xFF00FF00u && e.pixel(plain, 3, 4) == 0xFF000000u);
    CommandWriter bad(e.allocator);
    bad.resolve_region_layer(0, 1, 0, 2);   // (no layer 2)
    CHECK(e.render(bad, {plain, array}) == status::kInvalidParameter);
  }
  // A multisampled depth buffer read by a shader: a rectangle at depth 0.25 from x = 4.3, the rest cleared to 1.
  {
    AllocationDesc dd;
    dd.flags = kAllocDepthStencil | kAllocTexture, dd.format = Format::D32Float, dd.width = dd.height = 16, dd.samples = 4;
    Handle depth = 0;
    CHECK(e.kernel.create_allocation(kPid, e.dev, dd, &depth) == status::kSuccess);
    Handle ps = 0, with_depth = 0;
    CHECK(shader(e.kernel, e.dev, "ps_flat.dxbc", graphics::ShaderStage::Fragment, &ps) == status::kSuccess);
    PipelineDesc pd;
    pd.vertex_shader = vs, pd.fragment_shader = ps, pd.vertex_stride = 28, pd.attribute_count = 2, pd.varying_count = 2;
    pd.attributes[0] = {0, graphics::VertexFormat::Float3, 0};
    pd.attributes[1] = {1, graphics::VertexFormat::Float4, 12};
    pd.cull_mode = graphics::CullMode::None;
    pd.multisample = true, pd.depth_test = pd.depth_write = true;
    CHECK(e.kernel.create_pipeline(kPid, e.dev, pd, &with_depth) == status::kSuccess);
    V v[6];
    rect(v, 4.3f, 0, 16, 16, 16, 1, 1, 1, 0.25f);
    CHECK(draw(with_depth, ms, v, 6, depth) == status::kSuccess);
    const Handle ms_depth = pipeline("ps_ms_depth.dxbc", 1);
    CHECK(read_samples(ms_depth, depth) == status::kSuccess);
    bool ok = true;
    for (uint32_t x = 3; x < 6; ++x)
      for (uint32_t k = 0; k < 4; ++k) ok = ok && ((sample_of(x, 5, k) >> 16) & 0xFF) == (covered_by(4.3f, 16, x, k) ? 64u : 255u);
    CHECK(ok);
  }
  // Centroid: a triangle whose red is the weight of its vertex at the top - zero along its lower edge, below zero beyond. At the centre, partly covered pixels below that edge read red
  // below zero; at the centroid none does.
  {
    V t[3] = {at(-4, 12.37f, 16, 0, 0, 0), at(20, 12.37f, 16, 0, 0, 0), at(8, -10, 16, 1, 0, 0)};
    {
      // (a rectangle's lower edge through the row's lower samples, at 12.37: sample 0 of row 12 - at 12.125 - is inside, the others are not; its upper edge at 3.4 keeps samples 2 and 3 of row 3)
      V r6[6];
      rect(r6, 0, 3.4f, 16, 12.37f, 16, 1, 1, 1);
      CHECK(draw(colour, ms, r6, 6) == status::kSuccess && read_samples(ms_load, ms) == status::kSuccess);
      CHECK(sample_of(5, 3, 0) == 0xFF000000u && sample_of(5, 3, 1) == 0xFF000000u && sample_of(5, 3, 2) == 0xFFFFFFFFu && sample_of(5, 3, 3) == 0xFFFFFFFFu);
      CHECK(sample_of(5, 12, 0) == 0xFFFFFFFFu && sample_of(5, 12, 1) == 0xFF000000u && sample_of(5, 12, 2) == 0xFF000000u && sample_of(5, 12, 3) == 0xFF000000u);
    }
    auto reds = [&](const char* name) {
      CHECK(draw(pipeline(name, 0), ms, t, 3) == status::kSuccess && read_samples(ms_load, ms) == status::kSuccess);
      uint32_t n = 0, lit = 0;
      for (uint32_t y = 0; y < 16; ++y)
        for (uint32_t x = 0; x < 64; ++x) {
          const uint32_t p = e.pixel(readout, x, y);
          n += ((p >> 16) & 0xFF) == 255;
          lit += (p & 0xFFFFFF) != 0;
        }
      printf("  msaa %s: %u of %u covered samples below zero\n", name, n, lit);
      return n;
    };
    CHECK(reds("ps_centre.dxbc") >= 16 && reds("ps_centroid.dxbc") == 0 && reds("dxil_ps_centroid.dxil") == 0);
  }
  // noperspective (the vertices' w 1 and 4): linear in the screen, as the pixel's own x; the perspective-correct value is not.
  {
    const Handle plain = e.alloc(kTarget, Format::A8R8G8B8, 64, 64);
    V t[3] = {{-1, -1, 0.5f, -1, 0, 0, 1}, {1, -1, 0.5f, 1, 0, 0, 4}, {-1, 1, 0.5f, -1, 0, 0, 4}};
    auto worst = [&](const char* name) {
      Handle ps = 0, pipe = 0;
      CHECK(shader(e.kernel, e.dev, name, graphics::ShaderStage::Fragment, &ps) == status::kSuccess);
      PipelineDesc pd;
      pd.vertex_shader = vs, pd.fragment_shader = ps, pd.vertex_stride = 28, pd.attribute_count = 2, pd.varying_count = 2;
      pd.attributes[0] = {0, graphics::VertexFormat::Float3, 0};
      pd.attributes[1] = {1, graphics::VertexFormat::Float4, 12};
      pd.cull_mode = graphics::CullMode::None;
      CHECK(e.kernel.create_pipeline(kPid, e.dev, pd, &pipe) == status::kSuccess);
      CHECK(draw(pipe, plain, t, 3) == status::kSuccess);
      uint32_t most = 0;
      for (uint32_t y = 0; y < 64; ++y)
        for (uint32_t x = 0; x + 2 < y; ++x) {
          const uint32_t r = (e.pixel(plain, x, y) >> 16) & 0xFF;
          if (r > most) most = r;
        }
      return most;
    };
    const uint32_t linear = worst("ps_noperspective.dxbc"), dxil = worst("dxil_ps_noperspective.dxil"), perspective = worst("ps_perspective.dxbc");
    printf("  noperspective: worst %u (DXIL %u), perspective-correct %u\n", linear, dxil, perspective);
    CHECK(linear <= 2 && dxil <= 2 && perspective >= 64);
    // nointerpolation: the first vertex's colour all over (the vertices red, green and blue)
    V f[3] = {{-1, -1, 0.5f, 1, 0, 0, 1}, {1, -1, 0.5f, 0, 1, 0, 1}, {-1, 1, 0.5f, 0, 0, 1, 1}};
    for (const char* name : {"ps_flat.dxbc", "dxil_ps_flat.dxil"}) {
      Handle ps = 0, pipe = 0;
      CHECK(shader(e.kernel, e.dev, name, graphics::ShaderStage::Fragment, &ps) == status::kSuccess);
      PipelineDesc pd;
      pd.vertex_shader = vs, pd.fragment_shader = ps, pd.vertex_stride = 28, pd.attribute_count = 2, pd.varying_count = 2;
      pd.attributes[0] = {0, graphics::VertexFormat::Float3, 0};
      pd.attributes[1] = {1, graphics::VertexFormat::Float4, 12};
      pd.cull_mode = graphics::CullMode::None;
      CHECK(e.kernel.create_pipeline(kPid, e.dev, pd, &pipe) == status::kSuccess);
      CHECK(draw(pipe, plain, f, 3) == status::kSuccess);
      CHECK(e.pixel(plain, 3, 60) == 0xFFFF0000u && e.pixel(plain, 20, 40) == 0xFFFF0000u && e.pixel(plain, 1, 10) == 0xFFFF0000u && e.pixel(plain, 50, 10) == 0xFF000000u);
    }
  }
  // Shaders run for each sample: SV_SampleIndex (and the position at the sample, SV_Coverage's own bit), and an input at the sample (red and green the screen's x and y over 16).
  {
    V v[6];
    rect(v, 0, 0, 16, 16, 16, 1, 1, 1);
    for (const char* name : {"ps_sample_index.dxbc", "dxil_ps_sample_index.dxil"}) {
      CHECK(draw(pipeline(name, 0), ms, v, 6) == status::kSuccess && read_samples(ms_load, ms) == status::kSuccess);
      bool ok = true;
      for (uint32_t k = 0; k < 4; ++k) {
        const uint32_t p = sample_of(5, 6, k);
        const uint32_t r = (p >> 16) & 0xFF, g = (p >> 8) & 0xFF, b = p & 0xFF;
        ok = ok && close8(r, (k + 1) * 64 - (k == 3 ? 1 : 0), 1) && g == static_cast<uint32_t>(four[k].x + 8) && b == (16u << k);
        if (!ok) printf("  sample index %s: sample %u %08x\n", name, k, p);
      }
      CHECK(ok);
    }
    V g[6];
    // (the colour: x / 16 and y / 16 of the screen at each vertex, so the fractions of r * 16 * 4 ... ps_sample_input takes x * 64: a 64-wide screen's; drawn into a 16 x 16 image the
    // colours are x / 64 and y / 64 at the corners (0, 0) .. (16, 16) as a 64-wide screen would have them, i.e. the pixel's own coordinates over 64)
    g[0] = at(0, 0, 16, 0, 0, 0), g[1] = at(16, 0, 16, 16.0f / 64, 0, 0), g[2] = at(0, 16, 16, 0, 16.0f / 64, 0);
    g[3] = at(0, 16, 16, 0, 16.0f / 64, 0), g[4] = at(16, 0, 16, 16.0f / 64, 0, 0), g[5] = at(16, 16, 16, 16.0f / 64, 16.0f / 64, 0);
    for (const char* name : {"ps_sample_input.dxbc", "dxil_ps_sample_input.dxil"}) {
      CHECK(draw(pipeline(name, 0), ms, g, 6) == status::kSuccess && read_samples(ms_load, ms) == status::kSuccess);
      bool ok = true;
      for (uint32_t k = 0; k < 4; ++k) {
        const uint32_t p = sample_of(6, 9, k);
        ok = ok && close8((p >> 16) & 0xFF, static_cast<uint32_t>(four[k].x + 8), 1) && close8((p >> 8) & 0xFF, static_cast<uint32_t>(four[k].y + 8), 1);
        if (!ok) printf("  sample input %s: sample %u %08x\n", name, k, p);
      }
      CHECK(ok);
    }
    // EvaluateAttributeAtSample (sample y mod 4), Snapped (x mod 16 - 8 sixteenths) and Centroid (the centre: every sample covered)
    for (const char* name : {"ps_eval.dxbc", "dxil_ps_eval.dxil"}) {
      CHECK(draw(pipeline(name, 0), ms, g, 6) == status::kSuccess && read_samples(ms_load, ms) == status::kSuccess);
      bool ok = true;
      for (uint32_t y = 0; y < 4; ++y)
        for (uint32_t x = 0; x < 16; ++x) {
          const uint32_t p = sample_of(x, y, 0);
          ok = ok && close8((p >> 16) & 0xFF, static_cast<uint32_t>(four[y].x + 8), 1) && close8((p >> 8) & 0xFF, static_cast<uint32_t>(four[y].y + 8), 1) &&
               close8(p & 0xFF, x & 15, 1) && close8(p >> 24, 8, 1);
          if (!ok) {
            printf("  evaluate %s: pixel (%u, %u) %08x\n", name, x, y, p);
            y = 4;
            break;
          }
        }
      CHECK(ok);
    }
  }
  // SV_Coverage in (the rectangle from x = 4.3 to 12.6: 4 samples inside, 3 at pixel 4, 1 at 12) and out (white to the samples of the mask x mod 16).
  {
    V v[6];
    rect(v, 4.3f, 0, 12.6f, 16, 16, 1, 1, 1);
    for (const char* name : {"ps_coverage.dxbc", "dxil_ps_coverage.dxil"}) {
      CHECK(draw(pipeline(name, 0), ms, v, 6) == status::kSuccess && read_samples(ms_load, ms) == status::kSuccess);
      bool ok = true;
      for (uint32_t x = 3; x < 14; ++x) {
        if (x >= 7 && x <= 9) continue;   // (the rectangle's diagonal crosses row 8 at 8.45: each of its triangles covers part of those pixels, and SV_Coverage is the triangle's)
        uint32_t mask = 0;
        for (uint32_t k = 0; k < 4; ++k) mask |= covered_by(4.3f, 12.6f, x, k) ? 1u << k : 0u;
        for (uint32_t k = 0; k < 4; ++k) {
          if (!(mask >> k & 1)) continue;
          const uint32_t p = sample_of(x, 8, k);
          const bool right = close8((p >> 16) & 0xFF, (static_cast<uint32_t>(__builtin_popcount(mask)) * 255 + 2) / 4, 1) && ((p >> 8) & 0xFF) == mask * 16;
          if (!right && ok) printf("  coverage %s: pixel %u sample %u %08x (mask %x)\n", name, x, k, p, mask);
          ok = ok && right;
        }
      }
      CHECK(ok);
    }
    rect(v, 0, 0, 16, 16, 16, 1, 1, 1);
    for (const char* name : {"ps_omask.dxbc", "dxil_ps_omask.dxil"}) {
      CHECK(draw(pipeline(name, 0), ms, v, 6) == status::kSuccess && read_samples(ms_load, ms) == status::kSuccess);
      bool ok = true;
      for (uint32_t x = 0; x < 16; ++x)
        for (uint32_t k = 0; k < 4; ++k) ok = ok && sample_of(x, 3, k) == ((x >> k & 1) ? 0xFFFFFFFFu : 0xFF000000u);
      CHECK(ok);
    }
  }
  // SV_InnerCoverage with conservative rasterization: whole pixels green, the edge's red.
  {
    const Handle plain = e.alloc(kTarget, Format::A8R8G8B8, 16, 16);
    V t[3] = {at(2.3f, 2.2f, 16, 1, 1, 1), at(14.6f, 2.2f, 16, 1, 1, 1), at(2.3f, 14.7f, 16, 1, 1, 1)};
    for (const char* name : {"ps_inner.dxbc", "dxil_ps_inner.dxil"}) {
      CHECK(draw(pipeline(name, 0, true), plain, t, 3) == status::kSuccess);
      CHECK(e.pixel(plain, 5, 5) == 0xFF00FF00u && e.pixel(plain, 2, 5) == 0xFFFF0000u && e.pixel(plain, 5, 2) == 0xFFFF0000u && e.pixel(plain, 0, 0) == 0xFF000000u);
    }
  }
  // DXIL's Texture2DMS reads: the same samples as the DXBC shader's.
  {
    V v[6];
    rect(v, 4.3f, 0, 12.6f, 16, 16, 1, 0, 0);
    CHECK(draw(colour, ms, v, 6) == status::kSuccess);
    CHECK(read_samples(ms_load, ms) == status::kSuccess);
    uint32_t dxbc[64];
    for (uint32_t x = 0; x < 64; ++x) dxbc[x] = e.pixel(readout, x, 7);
    CHECK(read_samples(pipeline("dxil_ps_ms_load.dxil", 1), ms) == status::kSuccess);
    bool same = true;
    for (uint32_t x = 0; x < 64; ++x) same = same && e.pixel(readout, x, 7) == dxbc[x];
    CHECK(same);
    CHECK(read_samples(pipeline("dxil_ps_ms_info.dxil", 1), ms) == status::kSuccess);
    const uint32_t p = e.pixel(readout, 1, 3);
    CHECK(((p >> 16) & 0xFF) == 16 && (p & 0xFF) == 4 * 16 + static_cast<uint32_t>(four[1].x + 8) && (p >> 24) == static_cast<uint32_t>(four[1].y + 8));
    const Handle rt_info = pipeline("dxil_ps_rt_info.dxil", 0);
    rect(v, 0, 0, 16, 16, 16, 1, 1, 1);
    CHECK(draw(rt_info, ms, v, 6) == status::kSuccess && read_samples(ms_load, ms) == status::kSuccess);
    const uint32_t q = sample_of(2, 2, 0);
    CHECK(((q >> 16) & 0xFF) == 64 && ((q >> 8) & 0xFF) == static_cast<uint32_t>(four[2].x + 8));
  }

  // A device without them refuses the shaders and the images.
  FakeScreen screen(8, 6);
  graphics::SoftwareDevice software(e.allocator, &screen);
  SpyDevice limited(software);
  limited.feature3_mask = 0;
  Kernel kernel(e.allocator);
  AdapterConfig c;
  c.display = &screen;
  kernel.add_adapter(limited, c);
  Handle adapter = 0, dev = 0, refused = 0;
  CHECK(kernel.open_process(kPid) == status::kSuccess && kernel.open_adapter_from_index(kPid, 0, &adapter) == status::kSuccess && kernel.create_device(kPid, adapter, &dev) == status::kSuccess);
  for (const char* name : {"ps_ms_load.dxbc", "ps_ms_array.dxbc", "ps_centroid.dxbc", "ps_noperspective.dxbc", "ps_sample_index.dxbc", "ps_sample_input.dxbc", "ps_eval.dxbc",
                           "ps_coverage.dxbc", "ps_omask.dxbc", "ps_rt_info.dxbc", "ps_inner.dxbc", "dxil_ps_ms_load.dxil", "dxil_ps_eval.dxil", "dxil_ps_coverage.dxil"}) {
    const NtStatus st = shader(kernel, dev, name, graphics::ShaderStage::Fragment, &refused);
    if (st != status::kNotSupported) printf("  msaa: %s on a device without the features: %x\n", name, static_cast<unsigned>(st));
    CHECK(st == status::kNotSupported);
  }
  CHECK(shader(kernel, dev, "ps_flat.dxbc", graphics::ShaderStage::Fragment, &refused) == status::kSuccess);   // (flat inputs need nothing)
  AllocationDesc dd;
  dd.flags = kAllocDepthStencil | kAllocTexture, dd.format = Format::D32Float, dd.width = dd.height = 16, dd.samples = 4;
  CHECK(kernel.create_allocation(kPid, dev, dd, &refused) == status::kNotSupported);
  AllocationDesc ad = md;
  ad.kind = graphics::TextureKind::Texture2DArray, ad.layers = 2;
  CHECK(kernel.create_allocation(kPid, dev, ad, &refused) == status::kNotSupported);
}

int main() {
  test_handle_table();
  test_adapters_and_processes();
  test_allocation_checks();
  test_system_limit();
  test_clear_and_formats();
  test_triangle();
  test_texture_copy_mips();
  test_stream_errors();
  test_unbound_vertex_buffers();
  test_stream_fuzz();
  test_fault();
  test_fences_and_scheduler();
  test_residency();
  test_present();
  test_present_tap();
  test_sharing_and_teardown();
  test_teardown_with_work_queued();
  test_escape();
  test_device_teardown();
  test_d3d9_bytecode();
  test_d3d9_tgsi();
  test_d3d9_tgsi_reaches_device();
  test_dxbc();
  test_dxbc_real();
  test_packed_varyings();
  test_dxbc_constant_buffers();
  test_dxbc_integers();
  test_dxbc_loops();
  test_topologies();
  test_d3d10_state();
  test_rasterizer_state();
  test_viewports_from_vertex_shader();
  test_vertex_fetch_formats();
  test_virgl_vertex_patch();
  test_vertex_formats_and_step_rates();
  test_dxbc_system_values();
  test_device_forms();
  test_sm3_and_sm51();
  test_ps1();
  test_device_suite();
  test_stream_output();
  test_stream_output_extras();
  test_root_signature_parse();
  test_d3d12_binding();
  test_d3d12_copies_and_escapes();
  test_reserved_mip_tail();
  test_reserved_texel_sizes();
  test_device_limits();
  test_d3d12_compute();
  test_d3d12_buffer_views();
  test_d3d12_buffer_views_in_draw();
  test_pixel_shader_stores();
  test_vertex_shader_stores();
  test_geometry_shader_stores();
  test_tessellation_shader_stores();
  test_gather_register_offsets();
  test_d3d12_barriers();
  test_d3d12_heaps();
  test_mirrors();
  test_d3d12_reserved();
  test_d3d12_reserved_texture();
  test_reserved_tiers();
  test_bits_and_indexed_inputs();
  test_dxil();
  test_compute_shared();
  test_geometry_extras();
  test_alpha_to_coverage();
  test_derivative_draw();
  test_conformance();
  test_fixed_state_gaps();
  test_occlusion_queries();
  test_texture_kinds();
  test_mipmapped_layers();
  test_depth_copies();
  test_independent_blend();
  test_blend_constant();
  test_conservative_raster();
  test_compute();
  test_compute_kernel();
  test_storage_images_kernel();
  test_texel_buffers_kernel();
  test_uav_counters_kernel();
  test_vertex_uav_kernel();
  test_layered_pass_kernel();
  test_depth_textures_kernel();
  test_multisample();
  test_geometry_kernel();
  test_formats();
  test_tessellation_kernel();
  test_depth_bounds_kernel();
  test_sample_positions_kernel();
  test_view_instance_kernel();
  test_shading_rate_kernel();
  test_dxil_sm61_values();
  test_barycentrics_kernel();
  test_msaa_shaders_kernel();
  if (g_failed) {
    printf("%d check(s) failed\n", g_failed);
    return 1;
  }
  printf("DxgkrnlTest: all passed\n");
  return 0;
}
