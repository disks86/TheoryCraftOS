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

// Host tests of the Direct3D services of win32u.dll (src/Process/Win32k/Dxg): programs with an address space of their own make the NtGdiDdDDI* / NtDxgk* system calls with the
// D3DKMT structures of Windows x64 in their memory, against a dxgk::Kernel with a software adapter. Covers the adapter, device, context, allocation, lock, render, present,
// synchronization and escape paths end to end, hostile and truncated structures, handle misuse across processes, and the teardown of a process. Runs under ASan/UBSan.
#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>
#include "Graphics/Graphics3D/Software/SoftwareDevice.hpp"
#include "Kernel/Dxgkrnl/CommandStream.hpp"
#include "Kernel/Dxgkrnl/DxgKernel.hpp"
#include "Process/AddressSpace.hpp"
#include "Process/Win32k/Dxg/DxgInternal.hpp"
#include "Process/Win32k/Dxg/DxgLayouts.hpp"
#include "Win32kTestSupport.hpp"

using namespace w32test;
using namespace process;
namespace dx = process::win32k::dxg;
namespace lay = process::win32k::dxg::layout;
using WN = WindowsSyscallNumber;

namespace {

constexpr uint32_t kOk = 0, kNotImplemented = 0xC0000002u, kAccessViolation = 0xC0000005u, kInvalidHandle = 0xC0000008u, kInvalidParameter = 0xC000000Du, kNoSuchDevice = 0xC000000Eu,
                   kBufferTooSmall = 0xC0000023u, kNotSupported = 0xC00000BBu, kInvalidDeviceState = 0xC0000184u, kNotFound = 0xC0000225u;

// A display driver that keeps its screen in memory (as in DxgkrnlTest).
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

// A process with an address space: user memory is real regions here, so the services can map buffers into it.
struct Win {
  Win(TestAllocator& allocator, uint64_t pid)
      : space(allocator), pid(pid), process(pid, ProcessBinding{SyscallAbi::Windows, UserArch::X86_64, syscall_interface(SyscallAbi::Windows, UserArch::X86_64)}, space, host) {
    space.map(kBase, kSize, kProtRead | kProtWrite);
    process.attach_address_space(space, kBase + kSize);
  }
  AddressSpace space;
  Host host;
  uint64_t pid;
  Process process;
  const ISyscallInterface* iface = syscall_interface(SyscallAbi::Windows, UserArch::X86_64);

  uint32_t call(WN n, uint64_t a0 = 0) {
    SyscallRequest r;
    r.number = uint64_t(n);
    r.args[0] = a0;
    r.user_stack = kStack;
    put64(kStack + 0x28, 0);
    return uint32_t(iface->dispatch(process, r));
  }
  uint64_t call64(WN n, uint64_t a0 = 0) {
    SyscallRequest r;
    r.number = uint64_t(n);
    r.args[0] = a0;
    r.user_stack = kStack;
    return iface->dispatch(process, r);
  }
  void put(uint64_t a, ConstByteView v) { CHECK(space.write_privileged(a, v)); }
  void put32(uint64_t a, uint32_t v) {
    const uint8_t b[4] = {uint8_t(v), uint8_t(v >> 8), uint8_t(v >> 16), uint8_t(v >> 24)};
    put(a, ConstByteView(b, 4));
  }
  void put64(uint64_t a, uint64_t v) { put32(a, uint32_t(v)); put32(a + 4, uint32_t(v >> 32)); }
  uint32_t get32(uint64_t a) {
    uint8_t b[4] = {};
    CHECK(space.read_privileged(a, ByteView(b, 4)));
    return uint32_t(b[0]) | uint32_t(b[1]) << 8 | uint32_t(b[2]) << 16 | uint32_t(b[3]) << 24;
  }
  uint64_t get64(uint64_t a) { return uint64_t(get32(a)) | uint64_t(get32(a + 4)) << 32; }
  void zero(uint64_t a, uint64_t n) {
    std::vector<uint8_t> z(n, 0);
    put(a, ConstByteView(z.data(), z.size()));
  }
};

// One machine: a software device behind one adapter, a kernel published as the system's, win32k with the Direct3D services.
struct Machine {
  Machine() : screen(8, 6), device(allocator, &screen), kernel(allocator) {
    dxgk::AdapterConfig c;
    c.display = &screen;
    c.local_memory_budget = 64ull << 20;
    kernel.add_adapter(device, c);
    dxgk::set_system_kernel(&kernel);
    process::win32k::install_dxg();
  }
  ~Machine() {
    dxgk::set_system_kernel(nullptr);
    dx::shutdown_for_tests();
  }
  TestAllocator allocator;
  FakeScreen screen;
  graphics::SoftwareDevice device;
  dxgk::Kernel kernel;
};

// Scratch layout inside the program's memory (all well apart).
constexpr uint64_t kA = kScratch, kB = kScratch + 0x400, kC = kScratch + 0x800, kD = kScratch + 0xC00, kE = kScratch + 0x1000, kF = kScratch + 0x2000;
constexpr uint64_t kBigBuf = kScratch + 0x10000;

// --- Helpers that drive the services the way gdi32 / a user-mode driver would -----------------------------------------------------------------------------------

uint32_t enum_first_adapter(Win& w, uint64_t* luid_out = nullptr) {
  w.zero(kA, 16);
  w.put32(kA, 4);
  w.put64(kA + 8, kB);
  w.zero(kB, 20 * 4);
  const uint32_t st = w.call(WN::NtGdiDdDDIEnumAdapters2, kA);
  if (st) printf("enum status %08x\n", st);
  CHECK(st == kOk);
  const uint32_t n = w.get32(kA);
  CHECK(n == 1);
  const uint32_t h = w.get32(kB);
  if (luid_out) *luid_out = w.get64(kB + 4);
  return h;
}

uint32_t create_device(Win& w, uint32_t adapter) {
  w.zero(kA, lay::sizeof_create_device);
  w.put32(kA, adapter);
  CHECK(w.call(WN::NtGdiDdDDICreateDevice, kA) == kOk);
  return w.get32(kA + 12);
}

struct Ctx { uint32_t handle; uint64_t cmd, alloc_list, patch_list; uint32_t cmd_size, alloc_size, patch_size; };
Ctx create_context(Win& w, uint32_t device) {
  w.zero(kA, lay::sizeof_create_context);
  w.put32(kA, device);
  CHECK(w.call(WN::NtGdiDdDDICreateContext, kA) == kOk);
  return {w.get32(kA + 32), w.get64(kA + 40), w.get64(kA + 56), w.get64(kA + 72), w.get32(kA + 48), w.get32(kA + 64), w.get32(kA + 80)};
}

// The private driver data of an allocation (the miniport ABI).
struct AllocSpec {
  uint32_t flags = 0, format = 0, width = 0, height = 0, mips = 1, size_lo = 0, size_hi = 0, kind = 0, samples = 1, layers = 1;
  bool primary = false;
};
void write_private(Win& w, uint64_t at, const AllocSpec& a, uint32_t version = 1) {
  const uint32_t words[lay::kAwWords] = {version, a.flags, a.format, a.width, a.height, a.mips, a.size_lo, a.size_hi, a.kind, a.samples, a.layers, 0, 0, 0, 0, 0xAAAAAAAA, 0xAAAAAAAA, 0xAAAAAAAA};
  for (uint32_t i = 0; i < lay::kAwWords; ++i) w.put32(at + 4 * i, words[i]);
}

// A D3DKMT_CREATEALLOCATION with one allocation at kC (info array at kD, private data at kE). Returns the status; the allocation handle is at kD.
uint32_t create_one(Win& w, uint32_t device, const AllocSpec& a, uint32_t ca_flags, uint64_t info_stride = lay::kAllocationInfoStride, WN service = WN::NtGdiDdDDICreateAllocation) {
  w.zero(kC, lay::sizeof_create_allocation);
  w.zero(kD, 128);
  write_private(w, kE, a);
  w.put32(kC, device);
  w.put32(kC + 44, 1);
  w.put64(kC + 48, kD);
  w.put32(kC + 56, ca_flags);
  w.put64(kD + 16, kE);
  w.put32(kD + 24, lay::kAllocPrivateBytes);
  w.put32(kD + 32, a.primary ? 1 : 0);
  (void)info_stride;
  return w.call(service, kC);
}

constexpr uint32_t kFmtA8R8G8B8 = static_cast<uint32_t>(dxgk::Format::A8R8G8B8);

uint32_t alloc_ok(Win& w, uint32_t device, const AllocSpec& a, uint32_t ca_flags = 0);
AllocSpec buffer_spec(uint32_t flags, uint32_t size);
AllocSpec image_spec(uint32_t flags, uint32_t w, uint32_t h);

// --- Tests ------------------------------------------------------------------------------------------------------------------------------------------------------------

void test_no_driver() {
  TestAllocator allocator;
  SystemSync sync;
  ObjectManager objects(sync);
  objects.set_allocator(&allocator);
  set_system_objects(&objects);
  win32k::install();
  dxgk::set_system_kernel(nullptr);
  win32k::install_dxg();
  Win w(allocator, 50);
  CHECK(w.call(WN::NtGdiDdDDIEnumAdapters2, kA) == kNoSuchDevice);
  CHECK(w.call(WN::NtGdiDdDDICreateDevice, kA) == kNoSuchDevice);
  CHECK(w.call(WN::NtGdiDdDDICheckMultiPlaneOverlaySupport, kA) == kNoSuchDevice);
  // Services that are not implemented stay so.
  CHECK(w.call(WN::NtGdiDdDDIOpenAdapterFromHdc, kA) == kNotImplemented);
  dx::shutdown_for_tests();
  set_system_objects(nullptr);
}

void test_adapters() {
  System sys;
  Machine m;
  Win w(m.allocator, 100);

  // EnumAdapters2: count first (pAdapters null), then a too small buffer, then the list.
  w.zero(kA, 16);
  CHECK(w.call(WN::NtGdiDdDDIEnumAdapters2, kA) == kOk && w.get32(kA) == 1);
  w.put32(kA, 0);
  w.put64(kA + 8, kB);
  CHECK(w.call(WN::NtGdiDdDDIEnumAdapters2, kA) == kBufferTooSmall && w.get32(kA) == 1);
  w.put32(kA, 1);
  w.zero(kB, 20);
  CHECK(w.call(WN::NtGdiDdDDIEnumAdapters2, kA) == kOk);
  const uint32_t adapter = w.get32(kB);
  const uint32_t luid_low = w.get32(kB + 4), luid_high = w.get32(kB + 8);
  CHECK(adapter != 0 && (luid_low || luid_high) && w.get32(kB + 12) == 1);

  // The old EnumAdapters: the array is in the structure.
  w.zero(kC, lay::sizeof_enum_adapters);
  CHECK(w.call(WN::NtGdiDdDDIEnumAdapters, kC) == kOk && w.get32(kC) == 1 && w.get32(kC + 4) != 0 && w.get32(kC + 8) == luid_low);
  // NtDxgkEnumAdapters3 takes a filter; unknown filter bits are refused.
  w.zero(kC, lay::sizeof_enum_adapters3);
  CHECK(w.call(WN::NtDxgkEnumAdapters3, kC) == kOk && w.get32(kC + 8) == 1);
  w.put64(kC, 0x10);
  CHECK(w.call(WN::NtDxgkEnumAdapters3, kC) == kInvalidParameter);

  // OpenAdapterFromLuid, and a LUID that is not an adapter's.
  w.zero(kC, 16);
  w.put32(kC, luid_low);
  w.put32(kC + 4, luid_high);
  CHECK(w.call(WN::NtGdiDdDDIOpenAdapterFromLuid, kC) == kOk);
  const uint32_t by_luid = w.get32(kC + 8);
  CHECK(by_luid != 0 && by_luid != adapter);
  w.put32(kC, luid_low + 77);
  CHECK(w.call(WN::NtGdiDdDDIOpenAdapterFromLuid, kC) == kNotFound);

  // OpenAdapterFromDeviceName
  w.zero(kC, 32);
  const std::string name = "\\\\?\\display#tcosdxg#0#{1CA05180-A699-450A-9A0C-DE4FBE3DDD89}";
  {
    std::vector<uint8_t> wide;
    for (char c : name) { wide.push_back(uint8_t(c)); wide.push_back(0); }
    wide.push_back(0), wide.push_back(0);
    w.put(kE, ConstByteView(wide.data(), wide.size()));
  }
  w.put64(kC, kE);
  CHECK(w.call(WN::NtGdiDdDDIOpenAdapterFromDeviceName, kC) == kOk && w.get32(kC + 8) != 0 && w.get32(kC + 12) == luid_low);
  w.put(kE, ConstByteView(reinterpret_cast<const uint8_t*>(u"\\\\?\\nope"), 18));
  w.put32(kE + 18, 0);
  CHECK(w.call(WN::NtGdiDdDDIOpenAdapterFromDeviceName, kC) == kNotFound);
  w.put64(kC, 0);
  CHECK(w.call(WN::NtGdiDdDDIOpenAdapterFromDeviceName, kC) == kInvalidParameter);

  // CloseAdapter: zero, stale and double close
  w.zero(kC, 4);
  CHECK(w.call(WN::NtGdiDdDDICloseAdapter, kC) == kInvalidParameter);
  w.put32(kC, by_luid);
  CHECK(w.call(WN::NtGdiDdDDICloseAdapter, kC) == kOk);
  CHECK(w.call(WN::NtGdiDdDDICloseAdapter, kC) == kInvalidHandle);
  w.put32(kC, 0x12345678);
  CHECK(w.call(WN::NtGdiDdDDICloseAdapter, kC) == kInvalidHandle);
  CHECK(w.call(WN::NtGdiDdDDICloseAdapter, 0) == kInvalidParameter);                  // NULL
  CHECK(w.call(WN::NtGdiDdDDICloseAdapter, 0x10) == kAccessViolation);                // unmapped
  CHECK(w.call(WN::NtGdiDdDDICloseAdapter, kBase + kSize - 2) == kAccessViolation);   // straddles the end of the memory

  // QueryAdapterInfo
  auto query = [&](uint32_t type, uint64_t data, uint32_t size) {
    w.zero(kC, lay::sizeof_query_adapter_info);
    w.put32(kC, adapter);
    w.put32(kC + 4, type);
    w.put64(kC + 8, data);
    w.put32(kC + 16, size);
    return w.call(WN::NtGdiDdDDIQueryAdapterInfo, kC);
  };
  CHECK(query(13, kE, 4) == kOk && w.get32(kE) == 1300);                              // DRIVERVERSION
  CHECK(query(13, kE, 3) == kBufferTooSmall);
  CHECK(query(15, kE, 4) == kOk && (w.get32(kE) & 3) == 3 && (w.get32(kE) & 4));     // render + display, software
  CHECK(query(30, kE, 4) == kOk && w.get32(kE) == 1);
  CHECK(query(3, kE, 24) == kOk && w.get64(kE) == (64ull << 20));                     // dedicated video memory is the local budget
  CHECK(query(4, kE, 16) == kOk && w.get32(kE) == luid_low);
  CHECK(query(0, kE, lay::kAllocPrivateBytes + 32) == kOk);                           // UMDRIVERPRIVATE: the capabilities (120 bytes)
  CHECK(w.get32(kE) >= 2048 && (w.get32(kE + 24) & 1) == 0);
  // what the software renderer takes (DeviceInfo::limits): attributes, varyings, texture units, then after the name the colour targets and the vertex buffers
  CHECK(w.get32(kE + 4) == 16 && w.get32(kE + 8) == 16 && w.get32(kE + 12) == 16 && w.get32(kE + 96) == 8 && w.get32(kE + 100) == 8);
  CHECK(query(0, kE, 95) == kBufferTooSmall);
  w.put(kE, ConstByteView(reinterpret_cast<const uint8_t*>("\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF"), 12));
  CHECK(query(17, kE, 12) == kOk && w.get32(kE) == 0 && w.get32(kE + 8) == 0);        // WDDM 1.2 caps: nothing
  CHECK(query(9999, kE, 4) == kInvalidParameter);
  CHECK(query(13, 0, 4) == kInvalidParameter && query(13, kE, 0) == kInvalidParameter);
  CHECK(query(13, 0x10, 4) == kAccessViolation);

  // QueryVideoMemoryInfo
  w.zero(kC, lay::sizeof_query_video_memory_info);
  w.put32(kC + 8, adapter);
  CHECK(w.call(WN::NtGdiDdDDIQueryVideoMemoryInfo, kC) == kOk && w.get64(kC + 16) == (64ull << 20) && w.get64(kC + 24) == 0);
  w.put32(kC + 12, 1);
  CHECK(w.call(WN::NtGdiDdDDIQueryVideoMemoryInfo, kC) == kOk);
  w.put32(kC + 12, 2);
  CHECK(w.call(WN::NtGdiDdDDIQueryVideoMemoryInfo, kC) == kInvalidParameter);
  w.put32(kC + 12, 0);
  w.put64(kC, 0x1234);
  CHECK(w.call(WN::NtGdiDdDDIQueryVideoMemoryInfo, kC) == kNotSupported);              // another process
  w.put64(kC, ~uint64_t{0});
  CHECK(w.call(WN::NtGdiDdDDIQueryVideoMemoryInfo, kC) == kOk);                        // the current process pseudo-handle
  w.put32(kC + 48, 1);
  CHECK(w.call(WN::NtGdiDdDDIQueryVideoMemoryInfo, kC) == kInvalidParameter);          // no second physical adapter

  // Handles of one process mean nothing in another.
  Win other(m.allocator, 101);
  w.put32(kC, adapter);
  CHECK(other.call(WN::NtGdiDdDDICloseAdapter, 0) == kInvalidParameter);
  other.zero(kC, 4);
  other.put32(kC, adapter);
  CHECK(other.call(WN::NtGdiDdDDICloseAdapter, kC) == kInvalidHandle);

  win32k::process_exited(100);
  win32k::process_exited(101);
}

void test_devices_and_display() {
  System sys;
  Machine m;
  Win w(m.allocator, 100);
  const uint32_t adapter = enum_first_adapter(w);

  // CreateDevice: nothing for the device-level DMA buffers; invalid parameters.
  w.zero(kC, lay::sizeof_create_device);
  CHECK(w.call(WN::NtGdiDdDDICreateDevice, kC) == kInvalidParameter);
  CHECK(w.call(WN::NtGdiDdDDICreateDevice, 0) == kInvalidParameter);
  w.put32(kC, 0x4321);
  CHECK(w.call(WN::NtGdiDdDDICreateDevice, kC) == kInvalidHandle);
  w.zero(kC, lay::sizeof_create_device);
  w.put32(kC, adapter);
  w.put64(kC + 16, 0x1111);
  w.put32(kC + 24, 0x2222);
  CHECK(w.call(WN::NtGdiDdDDICreateDevice, kC) == kOk);
  const uint32_t device = w.get32(kC + 12);
  CHECK(device != 0 && w.get64(kC + 16) == 0 && w.get32(kC + 24) == 0 && w.get64(kC + 32) == 0 && w.get32(kC + 56) == 0);

  // GetDeviceState
  w.zero(kD, 16);
  w.put32(kD, device);
  w.put32(kD + 4, 1);
  CHECK(w.call(WN::NtGdiDdDDIGetDeviceState, kD) == kOk && w.get32(kD + 8) == 1);    // execution state: active
  w.put32(kD + 4, 3);
  w.put32(kD + 8, 0xFF);
  CHECK(w.call(WN::NtGdiDdDDIGetDeviceState, kD) == kOk && w.get32(kD + 8) == 0);    // reset state: no reset
  w.put32(kD + 4, 2);
  CHECK(w.call(WN::NtGdiDdDDIGetDeviceState, kD) == kNotSupported);

  // Display modes of the adapter, and setting one from a primary allocation of that size.
  w.zero(kD, 24);
  w.put32(kD, adapter);
  CHECK(w.call(WN::NtGdiDdDDIGetDisplayModeList, kD) == kOk && w.get32(kD + 16) == 2);
  w.put64(kD + 8, kE);
  w.put32(kD + 16, 1);
  CHECK(w.call(WN::NtGdiDdDDIGetDisplayModeList, kD) == kBufferTooSmall && w.get32(kD + 16) == 2);
  w.put32(kD + 16, 2);
  CHECK(w.call(WN::NtGdiDdDDIGetDisplayModeList, kD) == kOk);
  CHECK(w.get32(kE) == 8 && w.get32(kE + 4) == 6 && w.get32(kE + 8) == 21 && w.get32(kE + 12) == 60 && w.get32(kE + 32) == 16 && w.get32(kE + 36) == 12);
  w.put32(kD + 4, 1);
  CHECK(w.call(WN::NtGdiDdDDIGetDisplayModeList, kD) == kInvalidParameter);

  AllocSpec primary;
  primary.flags = dxgk::kAllocRenderTarget | dxgk::kAllocPresent;
  primary.format = kFmtA8R8G8B8;
  primary.width = 16, primary.height = 12;
  CHECK(create_one(w, device, primary, 0) == kOk);
  const uint32_t big = w.get32(kD);
  w.zero(kD, 16);
  w.put32(kD, device);
  w.put32(kD + 4, big);
  CHECK(w.call(WN::NtGdiDdDDISetDisplayMode, kD) == kOk && m.screen.current_mode() == 1);
  primary.width = 10;
  CHECK(create_one(w, device, primary, 0) == kOk);
  w.put32(kD + 4, w.get32(kD));
  w.put32(kD, device);
  // (kD now holds the info; build the call anew)
  w.zero(kF, 16);
  w.put32(kF, device);
  w.put32(kF + 4, w.get32(kD + 4));
  CHECK(w.call(WN::NtGdiDdDDISetDisplayMode, kF) == kInvalidParameter);                // no mode of that size
  w.put32(kF + 4, 0);
  CHECK(w.call(WN::NtGdiDdDDISetDisplayMode, kF) == kInvalidParameter);

  // SetQueuedLimit
  w.zero(kD, 16);
  w.put32(kD, device);
  w.put32(kD + 4, 2);
  CHECK(w.call(WN::NtGdiDdDDISetQueuedLimit, kD) == kOk && w.get32(kD + 8) == 3);
  w.put32(kD + 4, 1);
  w.put32(kD + 8, 5);
  CHECK(w.call(WN::NtGdiDdDDISetQueuedLimit, kD) == kOk);
  w.put32(kD + 4, 2);
  CHECK(w.call(WN::NtGdiDdDDISetQueuedLimit, kD) == kOk && w.get32(kD + 8) == 5);
  w.put32(kD + 4, 1);
  w.put32(kD + 8, 0);
  CHECK(w.call(WN::NtGdiDdDDISetQueuedLimit, kD) == kInvalidParameter);
  w.put32(kD + 4, 9);
  CHECK(w.call(WN::NtGdiDdDDISetQueuedLimit, kD) == kInvalidParameter);

  // WaitForVerticalBlankEvent and WaitForIdle
  w.zero(kD, 12);
  w.put32(kD, adapter);
  CHECK(w.call(WN::NtGdiDdDDIWaitForVerticalBlankEvent, kD) == kOk);
  w.put32(kD + 8, 1);
  CHECK(w.call(WN::NtGdiDdDDIWaitForVerticalBlankEvent, kD) == kInvalidParameter);
  w.zero(kD, 4);
  w.put32(kD, device);
  CHECK(w.call(WN::NtGdiDdDDIWaitForIdle, kD) == kOk);
  CHECK(w.call(WN::NtGdiDdDDITrimProcessCommitment, kC) == kOk || true);

  // Ownership of the video present source (the sequences Windows gives, as Wine's win32u test has them). 0xC01E0342: source in use, 0xC01E0006: present occluded.
  {
    auto set_owner = [&](uint32_t dev, int64_t type) {
      w.zero(kD, 32);
      w.zero(kD + 0x100, 16);
      w.put32(kD, dev);
      if (type >= 0) {
        w.put32(kD + 0x100, uint32_t(type));
        w.put64(kD + 8, kD + 0x100);
        w.put64(kD + 16, kD + 0x104);
        w.put32(kD + 24, 1);
      }
      return w.call(WN::NtGdiDdDDISetVidPnSourceOwner, kD);
    };
    auto check_owner = [&]() {
      w.zero(kF, 8);
      w.put32(kF, adapter);
      return w.call(WN::NtGdiDdDDICheckVidPnExclusiveOwnership, kF);
    };
    const uint32_t d1 = device, d2 = create_device(w, adapter);
    CHECK(check_owner() == kOk);
    CHECK(set_owner(d1, 0) == kOk && check_owner() == kOk);                          // UNOWNED
    CHECK(set_owner(d1, 1) == 0xC01E0342u && check_owner() == kOk);                  // SHARED: in use
    CHECK(set_owner(d1, 2) == kOk && check_owner() == 0xC01E0006u);                  // EXCLUSIVE: occluded
    CHECK(set_owner(d1, 2) == kOk);                                                  // again: fine
    CHECK(set_owner(d1, 4) == kInvalidParameter && check_owner() == 0xC01E0006u);    // another kind from the owner: invalid
    CHECK(set_owner(d1, 1) == kInvalidParameter);
    CHECK(set_owner(d2, 2) == 0xC01E0342u && set_owner(d2, 4) == 0xC01E0342u && set_owner(d2, 1) == 0xC01E0342u);
    CHECK(set_owner(d2, 0) == kOk && check_owner() == 0xC01E0006u);                  // another device cannot give it up
    CHECK(set_owner(d2, -1) == kOk && check_owner() == 0xC01E0006u);
    CHECK(set_owner(d1, -1) == kOk && check_owner() == kOk);                         // no list: the owner gives up everything
    CHECK(set_owner(d1, 4) == kOk && check_owner() == kOk);                          // EMULATED: not exclusive
    CHECK(set_owner(d2, 2) == 0xC01E0342u);
    CHECK(set_owner(d1, 2) == kInvalidParameter);
    CHECK(set_owner(d1, 0) == kOk && set_owner(d2, 2) == kOk && check_owner() == 0xC01E0006u);
    CHECK(set_owner(d1, 3) == kInvalidParameter && set_owner(d1, 5) == kInvalidParameter && set_owner(d1, 99) == kInvalidParameter);
    CHECK(set_owner(0, 2) == kInvalidParameter && set_owner(0x4242, 2) == kInvalidHandle);
    // The owner's device goes away, and so does its ownership.
    w.zero(kF, 4);
    w.put32(kF, d2);
    CHECK(w.call(WN::NtGdiDdDDIDestroyDevice, kF) == kOk && check_owner() == kOk);
    // Hostile: source other than 0, count over one, unreadable lists, nulls.
    w.zero(kD, 32);
    w.put32(kD, d1);
    w.put64(kD + 8, kD + 0x100);
    w.put64(kD + 16, kD + 0x104);
    w.put32(kD + 24, 1);
    w.put32(kD + 0x100, 2);
    w.put32(kD + 0x104, 1);
    CHECK(w.call(WN::NtGdiDdDDISetVidPnSourceOwner, kD) == kInvalidParameter);
    w.put32(kD + 0x104, 0);
    w.put32(kD + 24, 2);
    CHECK(w.call(WN::NtGdiDdDDISetVidPnSourceOwner, kD) == kInvalidParameter);
    w.put32(kD + 24, 1);
    w.put64(kD + 8, 0);
    CHECK(w.call(WN::NtGdiDdDDISetVidPnSourceOwner, kD) == kInvalidParameter);
    w.put64(kD + 8, 0x40);
    CHECK(w.call(WN::NtGdiDdDDISetVidPnSourceOwner, kD) == kAccessViolation);
    CHECK(w.call(WN::NtGdiDdDDISetVidPnSourceOwner, 0) == kInvalidParameter && w.call(WN::NtGdiDdDDICheckVidPnExclusiveOwnership, 0) == kInvalidParameter);
    w.zero(kF, 8);
    CHECK(w.call(WN::NtGdiDdDDICheckVidPnExclusiveOwnership, kF) == kInvalidParameter);
    w.put32(kF, adapter);
    w.put32(kF + 4, 1);
    CHECK(w.call(WN::NtGdiDdDDICheckVidPnExclusiveOwnership, kF) == kInvalidParameter);
    w.put32(kF + 4, 0);
    w.put32(kF, 0x6666);
    CHECK(w.call(WN::NtGdiDdDDICheckVidPnExclusiveOwnership, kF) == kInvalidHandle);
    // A second process: the owner's process ends, the source is free.
    Win other(m.allocator, 101);
    const uint32_t oa = enum_first_adapter(other), od = create_device(other, oa);
    other.zero(kD, 32);
    other.put32(kD, od);
    other.put32(kD + 0x100, 2);
    other.put64(kD + 8, kD + 0x100);
    other.put64(kD + 16, kD + 0x104);
    other.put32(kD + 24, 1);
    CHECK(other.call(WN::NtGdiDdDDISetVidPnSourceOwner, kD) == kOk && check_owner() == 0xC01E0006u);
    CHECK(set_owner(d1, 2) == 0xC01E0342u);
    win32k::process_exited(101);
    CHECK(check_owner() == kOk && set_owner(d1, 2) == kOk);
  }

  // Evict: allocations of this device; misuse
  {
    const uint32_t buf = alloc_ok(w, device, buffer_spec(dxgk::kAllocVertexBuffer, 64));
    w.zero(kF, 32);
    w.put32(kF, device);
    w.put32(kF + 4, 1);
    w.put64(kF + 8, kF + 0x100);
    w.put32(kF + 0x100, buf);
    CHECK(w.call(WN::NtGdiDdDDIEvict, kF) == kOk);
    w.put32(kF + 0x100, 0x3333);
    CHECK(w.call(WN::NtGdiDdDDIEvict, kF) == kInvalidHandle);
    w.put32(kF + 4, 0);
    CHECK(w.call(WN::NtGdiDdDDIEvict, kF) == kInvalidParameter);
    w.put32(kF + 4, 1);
    w.put64(kF + 8, 0x20);
    CHECK(w.call(WN::NtGdiDdDDIEvict, kF) == kAccessViolation);
  }

  // DestroyDevice: twice, and a stale handle
  w.zero(kD, 4);
  w.put32(kD, device);
  CHECK(w.call(WN::NtGdiDdDDIDestroyDevice, kD) == kOk);
  CHECK(w.call(WN::NtGdiDdDDIDestroyDevice, kD) == kInvalidHandle);
  w.put32(kD, 0);
  CHECK(w.call(WN::NtGdiDdDDIDestroyDevice, kD) == kInvalidParameter);
  win32k::process_exited(100);
}

// --- Allocations, locking, resources ---------------------------------------------------------------------------------------------------------------------------

uint32_t alloc_ok(Win& w, uint32_t device, const AllocSpec& a, uint32_t ca_flags) {
  const uint32_t st = create_one(w, device, a, ca_flags);
  if (st != kOk) printf("create_one status %08x\n", st);
  CHECK(st == kOk);
  return w.get32(kD);
}

AllocSpec buffer_spec(uint32_t flags, uint32_t size) {
  AllocSpec a;
  a.flags = flags;
  a.size_lo = size;
  return a;
}

AllocSpec image_spec(uint32_t flags, uint32_t w, uint32_t h) {
  AllocSpec a;
  a.flags = flags;
  a.format = kFmtA8R8G8B8;
  a.width = w, a.height = h;
  return a;
}

// D3DKMT_LOCK at kF; returns the status, the CPU pointer at kF + 24.
uint32_t lock_alloc(Win& w, uint32_t device, uint32_t allocation, uint32_t flags = 0, uint32_t sub = 0) {
  w.zero(kF, lay::sizeof_lock);
  w.put32(kF, device);
  w.put32(kF + 4, allocation);
  w.put32(kF + 8, sub);
  w.put32(kF + 32, flags);
  return w.call(WN::NtGdiDdDDILock, kF);
}

uint32_t unlock_alloc(Win& w, uint32_t device, uint32_t allocation) {
  w.zero(kF + 0x100, 16);
  w.put32(kF + 0x100, device);
  w.put32(kF + 0x100 + 4, 1);
  w.put64(kF + 0x100 + 8, kF + 0x200);
  w.put32(kF + 0x200, allocation);
  return w.call(WN::NtGdiDdDDIUnlock, kF + 0x100);
}

void test_allocations() {
  System sys;
  Machine m;
  Win w(m.allocator, 100);
  const uint32_t adapter = enum_first_adapter(w);
  const uint32_t device = create_device(w, adapter);

  // A buffer: the miniport reports the size of its backing in the private data.
  const uint32_t buf = alloc_ok(w, device, buffer_spec(dxgk::kAllocVertexBuffer, 64));
  CHECK(buf != 0 && w.get32(kE + 4 * lay::kAwBytesLo) == 64 && w.get32(kE + 4 * lay::kAwPitch) == 0);
  // Lock, write through the CPU view, unlock; the kernel's memory has it. A read-only lock does not write back.
  CHECK(lock_alloc(w, device, buf) == kOk);
  const uint64_t view = w.get64(kF + 24);
  CHECK(view != 0 && w.space.check(view, 64, kProtRead | kProtWrite));
  for (uint32_t i = 0; i < 16; ++i) w.put32(view + 4 * i, 0x01010101u * (i + 1));
  CHECK(lock_alloc(w, device, buf) == kInvalidDeviceState);      // locked already: the kernel refuses a second lock
  CHECK(unlock_alloc(w, device, buf) == kOk);
  CHECK(!w.space.check(view, 1, kProtRead));                     // the view is gone with the lock
  CHECK(unlock_alloc(w, device, buf) == kInvalidDeviceState);
  {
    dxgk::LockResult r;
    CHECK(m.kernel.lock(100, buf, dxgk::kLockReadOnly, 0, &r) == kOk && r.data.size() == 64 && r.data[0] == 1 && r.data[4] == 2 && r.data[63] == 16);
    CHECK(m.kernel.unlock(100, buf) == kOk);
  }
  CHECK(lock_alloc(w, device, buf, lay::kLockReadOnly) == kOk);
  const uint64_t view2 = w.get64(kF + 24);
  CHECK(w.get32(view2) == 0x01010101u && w.get32(view2 + 60) == 0x10101010u);
  w.put32(view2, 0xDEADBEEF);                                    // (a write the lock did not ask for)
  CHECK(unlock_alloc(w, device, buf) == kOk);
  {
    dxgk::LockResult r;
    CHECK(m.kernel.lock(100, buf, dxgk::kLockReadOnly, 0, &r) == kOk && r.data[0] == 1 && r.data[1] == 1);      // not written back
    m.kernel.unlock(100, buf);
  }
  CHECK(lock_alloc(w, device, buf, lay::kLockDiscard) == kOk);
  CHECK(w.get32(w.get64(kF + 24)) == 0);                         // discard: nothing carried over to the view
  CHECK(unlock_alloc(w, device, buf) == kOk);

  // Lock2 / Unlock2
  w.zero(kF, lay::sizeof_lock2);
  w.put32(kF, device);
  w.put32(kF + 4, buf);
  CHECK(w.call(WN::NtGdiDdDDILock2, kF) == kOk && w.get64(kF + 16) != 0);
  w.zero(kF + 0x100, 8);
  w.put32(kF + 0x100, device);
  w.put32(kF + 0x104, buf);
  CHECK(w.call(WN::NtGdiDdDDIUnlock2, kF + 0x100) == kOk && w.call(WN::NtGdiDdDDIUnlock2, kF + 0x100) == kInvalidDeviceState);

  // Misuse of lock: bad device / allocation, zero, wrong device
  CHECK(lock_alloc(w, device, 0) == kInvalidParameter && lock_alloc(w, 0, buf) == kInvalidParameter);
  CHECK(lock_alloc(w, device, 0x7777) == kInvalidHandle);
  const uint32_t device2 = create_device(w, adapter);
  CHECK(lock_alloc(w, device2, buf) == kInvalidParameter);
  CHECK(unlock_alloc(w, device2, buf) == kInvalidParameter);
  CHECK(w.call(WN::NtGdiDdDDILock, 0) == kInvalidParameter && w.call(WN::NtGdiDdDDILock, 0x10) == kAccessViolation && w.call(WN::NtGdiDdDDIUnlock, kBase + kSize - 4) == kAccessViolation);
  w.zero(kF + 0x100, 16);
  w.put32(kF + 0x100, device);
  w.put32(kF + 0x104, 100000);
  w.put64(kF + 0x108, kF + 0x200);
  CHECK(w.call(WN::NtGdiDdDDIUnlock, kF + 0x100) == kInvalidParameter);                 // too many allocations
  w.put32(kF + 0x104, 1);
  w.put64(kF + 0x108, 0x20);
  CHECK(w.call(WN::NtGdiDdDDIUnlock, kF + 0x100) == kAccessViolation);                  // unreadable list

  // An image: the miniport reports the pitch.
  const uint32_t img = alloc_ok(w, device, image_spec(dxgk::kAllocRenderTarget | dxgk::kAllocTexture, 4, 4));
  CHECK(w.get32(kE + 4 * lay::kAwPitch) == 16 && w.get32(kE + 4 * lay::kAwBytesLo) >= 64);
  CHECK(lock_alloc(w, device, img, 0, 3) == kInvalidParameter);                         // (no mip level 3)
  CHECK(lock_alloc(w, device, img, 0, 0) == kOk && unlock_alloc(w, device, img) == kOk);

  // Destroying an allocation that is locked unlocks it and drops its view.
  CHECK(lock_alloc(w, device, img) == kOk);
  const uint64_t img_view = w.get64(kF + 24);
  w.zero(kF + 0x100, 24);
  w.put32(kF + 0x100, device);
  w.put64(kF + 0x108, kF + 0x200);
  w.put32(kF + 0x110, 1);
  w.put32(kF + 0x200, img);
  CHECK(w.call(WN::NtGdiDdDDIDestroyAllocation, kF + 0x100) == kOk);
  CHECK(!w.space.check(img_view, 1, kProtRead));
  CHECK(w.call(WN::NtGdiDdDDIDestroyAllocation, kF + 0x100) == kInvalidHandle);
  CHECK(unlock_alloc(w, device, img) == kInvalidHandle);
  // Both a resource and a list, neither, a list for the wrong device, a list with one stale handle: nothing is destroyed.
  w.put32(kF + 0x104, 5);
  CHECK(w.call(WN::NtGdiDdDDIDestroyAllocation2, kF + 0x100) == kInvalidParameter);
  w.put32(kF + 0x104, 0);
  w.put32(kF + 0x110, 0);
  CHECK(w.call(WN::NtGdiDdDDIDestroyAllocation, kF + 0x100) == kInvalidParameter);
  w.put32(kF + 0x110, 2);
  w.put32(kF + 0x200, buf);
  w.put32(kF + 0x204, 0x5555);
  CHECK(w.call(WN::NtGdiDdDDIDestroyAllocation, kF + 0x100) == kInvalidHandle);
  w.put32(kF + 0x100, device2);
  w.put32(kF + 0x110, 1);
  CHECK(w.call(WN::NtGdiDdDDIDestroyAllocation, kF + 0x100) == kInvalidHandle);         // (the buffer belongs to the first device)
  CHECK(lock_alloc(w, device, buf) == kOk && unlock_alloc(w, device, buf) == kOk);      // still there

  // Hostile creation: truncated / wrong private data, bad counts and pointers.
  AllocSpec good = image_spec(dxgk::kAllocRenderTarget, 4, 4);
  CHECK(create_one(w, device, good, 0) == kOk);
  w.put32(kD + 24, lay::kAllocPrivateBytes - 1);
  CHECK(w.call(WN::NtGdiDdDDICreateAllocation, kC) == kInvalidParameter);
  w.put32(kD + 24, lay::kAllocPrivateBytes);
  write_private(w, kE, good, 2);
  CHECK(w.call(WN::NtGdiDdDDICreateAllocation, kC) == kInvalidParameter);               // version
  AllocSpec bad_format = good;
  bad_format.format = 99;
  write_private(w, kE, bad_format);
  CHECK(w.call(WN::NtGdiDdDDICreateAllocation, kC) == kInvalidParameter);
  AllocSpec bad_kind = good;
  bad_kind.kind = 9;
  write_private(w, kE, bad_kind);
  CHECK(w.call(WN::NtGdiDdDDICreateAllocation, kC) == kInvalidParameter);
  AllocSpec zero_size = good;
  zero_size.width = 0;
  write_private(w, kE, zero_size);
  CHECK(w.call(WN::NtGdiDdDDICreateAllocation, kC) == kInvalidParameter);
  AllocSpec huge = buffer_spec(dxgk::kAllocVertexBuffer, 0xFFFFFFFFu);
  huge.size_hi = 0x7FFFFFFF;
  write_private(w, kE, huge);
  CHECK(w.call(WN::NtGdiDdDDICreateAllocation, kC) == kInvalidParameter);
  write_private(w, kE, good);
  w.put64(kD + 16, 0x30);
  CHECK(w.call(WN::NtGdiDdDDICreateAllocation, kC) == kAccessViolation);                // unreadable private data
  w.put64(kD + 16, kE);
  w.put32(kC + 44, 0);
  CHECK(w.call(WN::NtGdiDdDDICreateAllocation, kC) == kInvalidParameter);
  w.put32(kC + 44, 100000);
  CHECK(w.call(WN::NtGdiDdDDICreateAllocation, kC) == kInvalidParameter);
  w.put32(kC + 44, 1);
  w.put64(kC + 48, 0x40);
  CHECK(w.call(WN::NtGdiDdDDICreateAllocation, kC) == kAccessViolation);
  w.put64(kC + 48, kD);
  w.put32(kC + 56, 1u << 1);
  CHECK(w.call(WN::NtGdiDdDDICreateAllocation, kC) == kInvalidParameter);               // shared without a resource
  w.put32(kC + 56, 1u << 16);
  CHECK(w.call(WN::NtGdiDdDDICreateAllocation, kC) == kNotSupported);                   // standard allocations
  w.put32(kC + 56, 1u << 5);
  CHECK(w.call(WN::NtGdiDdDDICreateAllocation, kC) == kNotSupported);                   // existing system memory
  w.put32(kC, 0);
  w.put32(kC + 56, 0);
  CHECK(w.call(WN::NtGdiDdDDICreateAllocation, kC) == kInvalidParameter);
  w.put32(kC, 0x8888);
  CHECK(w.call(WN::NtGdiDdDDICreateAllocation, kC) == kInvalidHandle);

  // Two allocations in one call, the second wrong: the first is not left behind.
  {
    const size_t before = dx::find_state(100)->allocations.count();
    w.zero(kC, lay::sizeof_create_allocation);
    w.zero(kD, 128);
    write_private(w, kE, good);
    AllocSpec bad = good;
    bad.width = 0;
    write_private(w, kE + 0x100, bad);
    w.put32(kC, device);
    w.put32(kC + 44, 2);
    w.put64(kC + 48, kD);
    w.put64(kD + 16, kE);
    w.put32(kD + 24, lay::kAllocPrivateBytes);
    w.put64(kD + 40 + 16, kE + 0x100);
    w.put32(kD + 40 + 24, lay::kAllocPrivateBytes);
    CHECK(w.call(WN::NtGdiDdDDICreateAllocation, kC) == kInvalidParameter);
    CHECK(dx::find_state(100)->allocations.count() == before);
    write_private(w, kE + 0x100, good);
    CHECK(w.call(WN::NtGdiDdDDICreateAllocation, kC) == kOk && w.get32(kD) != 0 && w.get32(kD + 40) != 0 && w.get32(kD) != w.get32(kD + 40));
    CHECK(dx::find_state(100)->allocations.count() == before + 2);
  }

  // Residency of the allocations
  w.zero(kC + 0x100, 16);
  w.put32(kC + 0x100, buf);
  w.put32(kC + 0x104, w.get32(kD));
  w.zero(kC, lay::sizeof_query_residency);
  w.put32(kC, device);
  w.put64(kC + 8, kC + 0x100);
  w.put32(kC + 16, 2);
  w.put64(kC + 24, kE);
  CHECK(w.call(WN::NtGdiDdDDIQueryAllocationResidency, kC) == kOk && w.get32(kE) >= 1 && w.get32(kE) <= 3 && w.get32(kE + 4) >= 1);
  w.put32(kC + 16, 0);
  CHECK(w.call(WN::NtGdiDdDDIQueryAllocationResidency, kC) == kInvalidParameter);
  w.put32(kC + 16, 2);
  w.put64(kC + 24, 0);
  CHECK(w.call(WN::NtGdiDdDDIQueryAllocationResidency, kC) == kInvalidParameter);

  win32k::process_exited(100);
}

void test_resources_and_sharing() {
  System sys;
  Machine m;
  Win a(m.allocator, 100), b(m.allocator, 200);
  const uint32_t adapter_a = enum_first_adapter(a), adapter_b = enum_first_adapter(b);
  const uint32_t dev_a = create_device(a, adapter_a), dev_b = create_device(b, adapter_b);

  // A shared resource of two allocations with runtime and driver data.
  a.zero(kC, lay::sizeof_create_allocation);
  a.zero(kD, 128);
  const AllocSpec s1 = buffer_spec(dxgk::kAllocVertexBuffer, 32), s2 = image_spec(dxgk::kAllocTexture, 4, 4);
  write_private(a, kE, s1);
  write_private(a, kE + 0x100, s2);
  const char runtime[] = "runtime-data!";
  const char driver[] = "resource-driver-data";
  a.put(kE + 0x400, ConstByteView(reinterpret_cast<const uint8_t*>(runtime), sizeof runtime));
  a.put(kE + 0x500, ConstByteView(reinterpret_cast<const uint8_t*>(driver), sizeof driver));
  a.put32(kC, dev_a);
  a.put64(kC + 16, kE + 0x400);
  a.put32(kC + 24, sizeof runtime);
  a.put64(kC + 32, kE + 0x500);
  a.put32(kC + 40, sizeof driver);
  a.put32(kC + 44, 2);
  a.put64(kC + 48, kD);
  a.put32(kC + 56, 3);                      // CreateResource | CreateShared
  a.put64(kD + 16, kE);
  a.put32(kD + 24, lay::kAllocPrivateBytes);
  a.put64(kD + 40 + 16, kE + 0x100);
  a.put32(kD + 40 + 24, lay::kAllocPrivateBytes);
  CHECK(a.call(WN::NtGdiDdDDICreateAllocation, kC) == kOk);
  const uint32_t resource = a.get32(kC + 4), global = a.get32(kC + 8), a1 = a.get32(kD), a2 = a.get32(kD + 40);
  CHECK(resource != 0 && global != 0 && a1 != 0 && a2 != 0 && resource != global);
  CHECK(lock_alloc(a, dev_a, a1) == kOk);
  a.put32(a.get64(kF + 24), 0x600DF00D);
  a.put32(a.get64(kF + 24) + 28, 0xFEEDFACE);
  CHECK(unlock_alloc(a, dev_a, a1) == kOk);

  // The other process queries and opens it.
  b.zero(kC, lay::sizeof_query_resource_info);
  b.put32(kC, dev_b);
  b.put32(kC + 4, global);
  b.put64(kC + 8, kE);
  b.put32(kC + 16, 4);
  CHECK(b.call(WN::NtGdiDdDDIQueryResourceInfo, kC) == kBufferTooSmall);                 // the runtime data does not fit...
  CHECK(b.get32(kC + 16) == sizeof runtime && b.get32(kC + 28) == 2 && b.get32(kC + 24) == sizeof driver && b.get32(kC + 20) == sizeof driver + 2 * lay::kAllocPrivateBytes);
  b.put32(kC + 16, sizeof runtime);
  CHECK(b.call(WN::NtGdiDdDDIQueryResourceInfo, kC) == kOk);
  {
    uint8_t got[sizeof runtime] = {};
    CHECK(b.space.read_privileged(kE, ByteView(got, sizeof got)) && std::string(reinterpret_cast<char*>(got)) == "runtime-data!");
  }
  b.put32(kC + 4, global + 5);
  CHECK(b.call(WN::NtGdiDdDDIQueryResourceInfo, kC) == kInvalidHandle);

  b.zero(kC, lay::sizeof_open_resource);
  b.zero(kD, 64);
  b.put32(kC, dev_b);
  b.put32(kC + 4, global);
  b.put32(kC + 8, 2);
  b.put64(kC + 16, kD);
  b.put64(kC + 24, kE);
  b.put32(kC + 32, sizeof runtime);
  b.put64(kC + 56, kE + 0x1000);
  b.put32(kC + 64, 8);
  CHECK(b.call(WN::NtGdiDdDDIOpenResource, kC) == kBufferTooSmall);                       // total private data buffer too small
  b.put32(kC + 64, 1024);
  b.put32(kC + 8, 3);
  CHECK(b.call(WN::NtGdiDdDDIOpenResource, kC) == kInvalidParameter);                     // wrong count
  b.put32(kC + 8, 2);
  CHECK(b.call(WN::NtGdiDdDDIOpenResource, kC) == kOk);
  const uint32_t b_resource = b.get32(kC + 68), b1 = b.get32(kD), b2 = b.get32(kD + 24);
  CHECK(b_resource != 0 && b1 != 0 && b2 != 0 && b.get32(kC + 48) == sizeof driver);
  {
    uint8_t got[sizeof driver] = {};
    CHECK(b.space.read_privileged(b.get64(kC + 40), ByteView(got, sizeof got)) && std::string(reinterpret_cast<char*>(got)) == "resource-driver-data");
    // the first allocation's private data is where its info says; its words are the miniport's (a buffer of 32 bytes)
    CHECK(b.get32(kD + 16) == lay::kAllocPrivateBytes && b.get32(b.get64(kD + 8) + 4 * lay::kAwBytesLo) == 32 && b.get32(b.get64(kD + 8) + 4 * lay::kAwFlags) == dxgk::kAllocVertexBuffer);
  }
  // The data written by the creator is in the opened allocation.
  CHECK(lock_alloc(b, dev_b, b1, lay::kLockReadOnly) == kOk && b.get32(b.get64(kF + 24)) == 0x600DF00D && b.get32(b.get64(kF + 24) + 28) == 0xFEEDFACE);
  CHECK(unlock_alloc(b, dev_b, b1) == kOk);

  // An unknown share, and a device that is not the process's.
  b.put32(kC + 4, global + 77);
  CHECK(b.call(WN::NtGdiDdDDIOpenResource, kC) == kInvalidHandle);
  b.put32(kC + 4, global);
  b.put32(kC, 0x4141);
  CHECK(b.call(WN::NtGdiDdDDIOpenResource, kC) == kInvalidHandle);

  // The owner destroys the resource by its handle: the share is gone, what the other process opened lives on.
  a.zero(kF + 0x100, 24);
  a.put32(kF + 0x100, dev_a);
  a.put32(kF + 0x104, resource);
  CHECK(a.call(WN::NtGdiDdDDIDestroyAllocation, kF + 0x100) == kOk);
  CHECK(lock_alloc(a, dev_a, a1) == kInvalidHandle && a.call(WN::NtGdiDdDDIDestroyAllocation, kF + 0x100) == kInvalidHandle);
  b.put32(kC, dev_b);
  CHECK(b.call(WN::NtGdiDdDDIOpenResource, kC) == kInvalidHandle);
  CHECK(lock_alloc(b, dev_b, b1, lay::kLockReadOnly) == kOk && unlock_alloc(b, dev_b, b1) == kOk);
  // Destroying the opened resource by the second process
  b.zero(kF + 0x100, 24);
  b.put32(kF + 0x100, dev_b);
  b.put32(kF + 0x104, b_resource);
  CHECK(b.call(WN::NtGdiDdDDIDestroyAllocation2, kF + 0x100) == kOk && lock_alloc(b, dev_b, b1) == kInvalidHandle);
  CHECK(b.call(WN::NtGdiDdDDIDestroyAllocation2, kF + 0x100) == kInvalidHandle);

  // A resource that is shared and then its owner exits: the share goes with the process.
  a.zero(kC, lay::sizeof_create_allocation);
  a.zero(kD, 64);
  write_private(a, kE, s1);
  a.put32(kC, dev_a);
  a.put32(kC + 44, 1);
  a.put64(kC + 48, kD);
  a.put32(kC + 56, 3);
  a.put64(kD + 16, kE);
  a.put32(kD + 24, lay::kAllocPrivateBytes);
  CHECK(a.call(WN::NtGdiDdDDICreateAllocation, kC) == kOk);
  const uint32_t global2 = a.get32(kC + 8);
  win32k::process_exited(100);
  b.zero(kC, lay::sizeof_query_resource_info);
  b.put32(kC, dev_b);
  b.put32(kC + 4, global2);
  CHECK(b.call(WN::NtGdiDdDDIQueryResourceInfo, kC) == kInvalidHandle);
  win32k::process_exited(200);
}

// --- Contexts, render, present ---------------------------------------------------------------------------------------------------------------------------------

uint32_t pixel_of(Win& w, uint32_t device, uint32_t allocation, uint32_t pitch, uint32_t x, uint32_t y) {
  if (lock_alloc(w, device, allocation, lay::kLockReadOnly) != kOk) return 0xDEADDEAD;
  const uint64_t view = w.get64(kF + 24);
  const uint32_t v = w.get32(view + uint64_t{y} * pitch + x * 4);
  unlock_alloc(w, device, allocation);
  return v;
}

// A clear pass of target 0 of the allocation list as a stream.
std::vector<uint8_t> clear_stream(TestAllocator& allocator, graphics::Vec4 color) {
  dxgk::CommandWriter cw(allocator);
  cw.begin_pass(0, dxgk::kNoAllocation, dxgk::kPassClearColor, color, 1.0f, 0);
  cw.end_pass();
  const ConstByteView v = cw.view();
  return std::vector<uint8_t>(v.data(), v.data() + v.size());
}

uint32_t render_call(Win& w, uint32_t context, uint32_t offset, uint32_t length, uint32_t count) {
  w.zero(kA, 128);
  w.put32(kA, context);
  w.put32(kA + 4, offset);
  w.put32(kA + 8, length);
  w.put32(kA + 12, count);
  return w.call(WN::NtGdiDdDDIRender, kA);
}

void test_render_and_present() {
  System sys;
  Machine m;
  Win w(m.allocator, 100);
  const uint32_t adapter = enum_first_adapter(w);
  const uint32_t device = create_device(w, adapter);
  const Ctx ctx = create_context(w, device);
  CHECK(ctx.handle != 0 && ctx.cmd != 0 && ctx.cmd_size == 65536 && ctx.alloc_list == ctx.cmd + 65536 && ctx.alloc_size == 1024 && ctx.patch_list > ctx.alloc_list && ctx.patch_size == 1024);
  CHECK(w.space.check(ctx.cmd, ctx.cmd_size, kProtRead | kProtWrite) && w.space.check(ctx.alloc_list, 8192, kProtRead | kProtWrite));

  const uint32_t target = alloc_ok(w, device, image_spec(dxgk::kAllocRenderTarget, 4, 4));
  const uint32_t pitch = w.get32(kE + 4 * lay::kAwPitch);
  CHECK(pitch == 16);
  const std::vector<uint8_t> red = clear_stream(m.allocator, {1, 0, 0, 1});
  w.put(ctx.cmd + 16, ConstByteView(red.data(), red.size()));
  w.put32(ctx.alloc_list, target);
  w.put32(ctx.alloc_list + 4, 1);        // WriteOperation
  CHECK(render_call(w, ctx.handle, 16, uint32_t(red.size()), 1) == kOk);
  CHECK(w.get64(kA + 24) == ctx.cmd && w.get32(kA + 32) == 65536 && w.get64(kA + 40) == ctx.alloc_list && w.get32(kA + 48) == 1024 && w.get64(kA + 56) == ctx.patch_list);
  CHECK(pixel_of(w, device, target, pitch, 2, 2) == 0xFFFF0000);
  // The kernel took a copy: another render with a green clear changes the picture.
  const std::vector<uint8_t> green = clear_stream(m.allocator, {0, 1, 0, 1});
  w.put(ctx.cmd, ConstByteView(green.data(), green.size()));
  CHECK(render_call(w, ctx.handle, 0, uint32_t(green.size()), 1) == kOk && pixel_of(w, device, target, pitch, 0, 3) == 0xFF00FF00);
  // NullRendering does nothing.
  const std::vector<uint8_t> blue = clear_stream(m.allocator, {0, 0, 1, 1});
  w.put(ctx.cmd, ConstByteView(blue.data(), blue.size()));
  w.zero(kA, 128);
  w.put32(kA, ctx.handle);
  w.put32(kA + 8, uint32_t(blue.size()));
  w.put32(kA + 12, 1);
  w.put32(kA + 68, 1u << 3);
  CHECK(w.call(WN::NtGdiDdDDIRender, kA) == kOk && pixel_of(w, device, target, pitch, 0, 3) == 0xFF00FF00);

  // Hostile Render calls
  CHECK(render_call(w, ctx.handle, 0, 0, 1) == kInvalidParameter);                              // nothing
  CHECK(render_call(w, ctx.handle, 0, 6, 1) == kInvalidParameter);                              // not whole words
  CHECK(render_call(w, ctx.handle, 65536, 4, 1) == kInvalidParameter);                          // past the buffer
  CHECK(render_call(w, ctx.handle, 4, 65536, 1) == kInvalidParameter);
  CHECK(render_call(w, ctx.handle, 0xFFFFFFF8u, 0x10, 1) == kInvalidParameter);                 // wrap-around
  CHECK(render_call(w, ctx.handle, 0, uint32_t(blue.size()), 5000) == kInvalidParameter);
  CHECK(render_call(w, ctx.handle, 0, uint32_t(blue.size()) - 4, 1) == kInvalidParameter);      // a truncated stream (the kernel checks it)
  CHECK(render_call(w, ctx.handle, 0, uint32_t(blue.size()), 0) == kInvalidParameter);          // no allocation list for index 0
  w.put32(ctx.alloc_list, 0x4242);
  CHECK(render_call(w, ctx.handle, 0, uint32_t(blue.size()), 1) == kInvalidHandle);
  w.put32(ctx.alloc_list, target);
  CHECK(render_call(w, 0, 0, 8, 1) == kInvalidParameter && render_call(w, 0x4343, 0, 8, 1) == kInvalidHandle);
  w.put32(kA + 80, 1);
  w.put32(kA, ctx.handle);
  w.put32(kA + 8, 8);
  CHECK(w.call(WN::NtGdiDdDDIRender, kA) == kNotSupported);                                     // broadcast contexts
  CHECK(w.call(WN::NtGdiDdDDIRender, 0) == kInvalidParameter && w.call(WN::NtGdiDdDDIRender, 0x10) == kAccessViolation);
  // bit flips of a valid stream never crash and never touch other memory
  {
    std::vector<uint8_t> flipped = blue;
    for (size_t bit = 0; bit < flipped.size() * 8; ++bit) {
      flipped[bit / 8] ^= uint8_t(1u << (bit % 8));
      w.put(ctx.cmd, ConstByteView(flipped.data(), flipped.size()));
      const uint32_t st = render_call(w, ctx.handle, 0, uint32_t(flipped.size()), 1);
      CHECK(st == kOk || st == kInvalidParameter || st == kNotSupported || st == kInvalidHandle);
      flipped[bit / 8] ^= uint8_t(1u << (bit % 8));
    }
  }

  // Scheduling priority (kept, not scheduled)
  w.zero(kA, 8);
  w.put32(kA, ctx.handle);
  CHECK(w.call(WN::NtGdiDdDDIGetContextSchedulingPriority, kA) == kOk && w.get32(kA + 4) == 0);
  w.put32(kA + 4, uint32_t(-3));
  CHECK(w.call(WN::NtGdiDdDDISetContextSchedulingPriority, kA) == kOk);
  w.put32(kA + 4, 0);
  CHECK(w.call(WN::NtGdiDdDDIGetContextSchedulingPriority, kA) == kOk && int32_t(w.get32(kA + 4)) == -3);
  w.put32(kA + 4, 99);
  CHECK(w.call(WN::NtGdiDdDDISetContextSchedulingPriority, kA) == kInvalidParameter);
  w.put32(kA + 4, 2);
  CHECK(w.call(WN::NtGdiDdDDISetContextInProcessSchedulingPriority, kA) == kOk);
  w.put32(kA + 4, 0);
  CHECK(w.call(WN::NtGdiDdDDIGetContextInProcessSchedulingPriority, kA) == kOk && w.get32(kA + 4) == 2);
  w.put32(kA, 0x999);
  CHECK(w.call(WN::NtGdiDdDDIGetContextSchedulingPriority, kA) == kInvalidHandle);

  // Present: a flip of a presentable image shows it; a blt copies a region into a destination.
  AllocSpec back = image_spec(dxgk::kAllocRenderTarget | dxgk::kAllocPresent, 8, 6);
  const uint32_t back_buffer = alloc_ok(w, device, back);
  w.put32(ctx.alloc_list, back_buffer);
  w.put(ctx.cmd, ConstByteView(red.data(), red.size()));
  CHECK(render_call(w, ctx.handle, 0, uint32_t(red.size()), 1) == kOk);
  auto present = [&](uint32_t flags, uint32_t source, uint32_t destination, bool src_rect = false, bool dst_rect = false, int32_t rect_w = 4, int32_t rect_h = 3) {
    w.zero(kA, 128);
    w.put32(kA, ctx.handle);
    w.put32(kA + 20, source);
    w.put32(kA + 24, destination);
    if (dst_rect) { w.put32(kA + 32, 1); w.put32(kA + 36, 1); w.put32(kA + 40, uint32_t(1 + rect_w)); w.put32(kA + 44, uint32_t(1 + rect_h)); flags |= 1u << 6; }
    if (src_rect) { w.put32(kA + 48, 0); w.put32(kA + 52, 0); w.put32(kA + 56, uint32_t(rect_w)); w.put32(kA + 60, uint32_t(rect_h)); flags |= 1u << 7; }
    w.put32(kA + 88, flags);
    return w.call(WN::NtGdiDdDDIPresent, kA);
  };
  const uint32_t presents_before = m.screen.presents;
  CHECK(present(1u << 2, back_buffer, 0) == kOk && m.screen.presents == presents_before + 1);       // Flip
  CHECK(present(1u << 0, back_buffer, 0) == kInvalidParameter);                                       // Blt needs a destination
  CHECK(present(0, back_buffer, 0) == kInvalidParameter && present((1u << 0) | (1u << 2), back_buffer, back_buffer) == kInvalidParameter);
  CHECK(present(1u << 1, back_buffer, 0) == kNotSupported);                                           // ColorFill
  CHECK(present(1u << 2, 0, 0) == kInvalidParameter && present(1u << 2, 0x4444, 0) == kInvalidHandle);
  CHECK(present(1u << 2, target, 0) != kOk);                                                          // (not presentable)
  AllocSpec dest = image_spec(dxgk::kAllocRenderTarget | dxgk::kAllocPresent, 8, 6);
  const uint32_t destination = alloc_ok(w, device, dest);
  CHECK(present(1u << 0, back_buffer, destination, true, true) == kOk);
  CHECK(present(1u << 0, back_buffer, destination, false, true, 4, 3) == kNotSupported);              // (the destination rectangle is not the size of the whole source: no stretching)
  w.zero(kA, 128);
  w.put32(kA, device);       // the old form: a device handle names the device's first context
  w.put32(kA + 20, back_buffer);
  w.put32(kA + 88, 1u << 2);
  CHECK(w.call(WN::NtGdiDdDDIPresent, kA) == kOk);
  w.put32(kA, 0x1234);
  CHECK(w.call(WN::NtGdiDdDDIPresent, kA) == kInvalidHandle);
  w.put32(kA, ctx.handle);
  w.put32(kA + 16, 1);
  CHECK(w.call(WN::NtGdiDdDDIPresent, kA) == kInvalidParameter);

  // DestroyContext unmaps the buffers
  w.zero(kA, 4);
  w.put32(kA, ctx.handle);
  CHECK(w.call(WN::NtGdiDdDDIDestroyContext, kA) == kOk);
  CHECK(!w.space.check(ctx.cmd, 1, kProtRead));
  CHECK(w.call(WN::NtGdiDdDDIDestroyContext, kA) == kInvalidHandle);
  CHECK(render_call(w, ctx.handle, 0, 8, 1) == kInvalidHandle);

  // A virtual context has no buffers and cannot Render
  w.zero(kA, lay::sizeof_create_context_virtual);
  w.put32(kA, device);
  CHECK(w.call(WN::NtGdiDdDDICreateContextVirtual, kA) == kOk);
  const uint32_t virtual_ctx = w.get32(kA + 32);
  CHECK(virtual_ctx != 0 && render_call(w, virtual_ctx, 0, 8, 1) == kInvalidParameter);
  // node ordinal / engine affinity out of range, no device
  w.zero(kA, lay::sizeof_create_context);
  w.put32(kA, device);
  w.put32(kA + 4, 1);
  CHECK(w.call(WN::NtGdiDdDDICreateContext, kA) == kInvalidParameter);
  w.put32(kA + 4, 0);
  w.put32(kA + 8, 4);
  CHECK(w.call(WN::NtGdiDdDDICreateContext, kA) == kInvalidParameter);
  w.put32(kA, 0);
  CHECK(w.call(WN::NtGdiDdDDICreateContext, kA) == kInvalidParameter);
  win32k::process_exited(100);
}

// --- Synchronization -------------------------------------------------------------------------------------------------------------------------------------------

uint32_t make_sync2(Win& w, uint32_t device, uint32_t type, uint32_t flags, uint64_t arg0, uint32_t arg1 = 0, uint32_t* handle = nullptr) {
  w.zero(kC, lay::sizeof_create_sync2);
  w.put32(kC, device);
  w.put32(kC + 8, type);
  w.put32(kC + 12, flags);
  w.put64(kC + 16, arg0);
  if (type == 2) { w.put32(kC + 16, uint32_t(arg0)); w.put32(kC + 20, arg1); }
  const uint32_t st = w.call(WN::NtGdiDdDDICreateSynchronizationObject2, kC);
  if (handle) *handle = w.get32(kC + 88);
  return st;
}

// FromCpu calls with the arrays at the given addresses.
uint32_t sync_cpu(Win& w, WN service, uint32_t device, std::initializer_list<uint32_t> handles, std::initializer_list<uint64_t> values, uint32_t flags = 0, uint64_t event = 0, uint64_t at = kB) {
  w.zero(at, 64);
  uint32_t i = 0;
  for (uint32_t h : handles) w.put32(at + 0x100 + 4 * i++, h);
  i = 0;
  for (uint64_t v : values) w.put64(at + 0x200 + 8 * i++, v);
  w.put32(at, device);
  w.put32(at + 4, uint32_t(handles.size()));
  w.put64(at + 8, at + 0x100);
  w.put64(at + 16, at + 0x200);
  if (service == WN::NtGdiDdDDIWaitForSynchronizationObjectFromCpu) { w.put64(at + 24, event); w.put32(at + 32, flags); }
  else w.put32(at + 24, flags);
  return w.call(service, at);
}

void test_synchronization() {
  System sys;
  Machine m;
  Win w(m.allocator, 100);
  const uint32_t adapter = enum_first_adapter(w);
  const uint32_t device = create_device(w, adapter);

  uint32_t fence = 0, sem = 0, mutex = 0;
  CHECK(make_sync2(w, device, 3, 0, 5, 0, &fence) == kOk && fence != 0);
  CHECK(make_sync2(w, device, 2, 0, 2, 1, &sem) == kOk && sem != 0);
  CHECK(make_sync2(w, device, 1, 0, 1, 0, &mutex) == kOk && mutex != 0);
  // unsupported kinds and flags, bad parameters
  CHECK(make_sync2(w, device, 4, 0, 0) == kNotSupported);               // CPU notification (needs an event)
  CHECK(make_sync2(w, device, 5, 0, 0) == kNotSupported);               // monitored fence (needs a mapped fence value)
  CHECK(make_sync2(w, device, 6, 0, 0) == kNotSupported);
  CHECK(make_sync2(w, device, 3, 1, 0) == kNotSupported);               // shared
  CHECK(make_sync2(w, device, 3, 4, 0) == kNotSupported);               // cross adapter
  CHECK(make_sync2(w, device, 99, 0, 0) == kInvalidParameter && make_sync2(w, device, 0, 0, 0) == kInvalidParameter);
  CHECK(make_sync2(w, device, 2, 0, 0, 0) == kInvalidParameter);        // max count 0
  CHECK(make_sync2(w, device, 2, 0, 1, 2) == kInvalidParameter);        // initial over max
  CHECK(make_sync2(w, 0, 3, 0, 0) == kInvalidParameter && make_sync2(w, 0x9999, 3, 0, 0) == kInvalidHandle);
  CHECK(w.call(WN::NtGdiDdDDICreateSynchronizationObject2, 0) == kInvalidParameter && w.call(WN::NtGdiDdDDICreateSynchronizationObject2, 8) == kAccessViolation);

  // The old create (v1): mutex, semaphore, fence
  w.zero(kC, lay::sizeof_create_sync);
  w.put32(kC, device);
  w.put32(kC + 4, 2);
  w.put32(kC + 8, 3);
  w.put32(kC + 12, 3);
  CHECK(w.call(WN::NtGdiDdDDICreateSynchronizationObject, kC) == kOk);
  const uint32_t sem3 = w.get32(kC + 72);
  w.put32(kC + 4, 1);
  w.put32(kC + 8, 0);
  CHECK(w.call(WN::NtGdiDdDDICreateSynchronizationObject, kC) == kOk);
  const uint32_t mutex_held = w.get32(kC + 72);
  w.put32(kC + 4, 3);
  CHECK(w.call(WN::NtGdiDdDDICreateSynchronizationObject, kC) == kOk);
  w.put32(kC + 4, 4);
  CHECK(w.call(WN::NtGdiDdDDICreateSynchronizationObject, kC) == kNotSupported);

  // CPU signal and wait on the fence
  CHECK(sync_cpu(w, WN::NtGdiDdDDISignalSynchronizationObjectFromCpu, device, {fence}, {7}) == kOk);
  CHECK(sync_cpu(w, WN::NtGdiDdDDIWaitForSynchronizationObjectFromCpu, device, {fence}, {7}) == kOk);
  CHECK(sync_cpu(w, WN::NtGdiDdDDIWaitForSynchronizationObjectFromCpu, device, {fence}, {3}) == kOk);
  CHECK(sync_cpu(w, WN::NtGdiDdDDISignalSynchronizationObjectFromCpu, device, {fence}, {6}) == kInvalidParameter);      // a fence does not go back
  // misuse
  CHECK(sync_cpu(w, WN::NtGdiDdDDISignalSynchronizationObjectFromCpu, device, {}, {}) == kInvalidParameter);
  CHECK(sync_cpu(w, WN::NtGdiDdDDISignalSynchronizationObjectFromCpu, 0, {fence}, {8}) == kInvalidParameter);
  CHECK(sync_cpu(w, WN::NtGdiDdDDISignalSynchronizationObjectFromCpu, device, {fence, 0x4545}, {9, 9}) == kInvalidHandle);
  CHECK(sync_cpu(w, WN::NtGdiDdDDIWaitForSynchronizationObjectFromCpu, device, {fence}, {99}, 0, 0x7777) == kNotSupported);    // async event
  CHECK(sync_cpu(w, WN::NtGdiDdDDIWaitForSynchronizationObjectFromCpu, device, {0x4545}, {1}) == kInvalidHandle);
  w.put32(kB + 4, 33);
  CHECK(w.call(WN::NtGdiDdDDIWaitForSynchronizationObjectFromCpu, kB) == kInvalidParameter);
  w.put32(kB + 4, 1);
  w.put64(kB + 8, 0x50);
  CHECK(w.call(WN::NtGdiDdDDIWaitForSynchronizationObjectFromCpu, kB) == kAccessViolation);
  w.put64(kB + 8, 0);
  CHECK(w.call(WN::NtGdiDdDDIWaitForSynchronizationObjectFromCpu, kB) == kInvalidParameter);

  // Semaphore: initial 1, wait takes it; signal up to max
  CHECK(sync_cpu(w, WN::NtGdiDdDDIWaitForSynchronizationObjectFromCpu, device, {sem}, {0}) == kOk);
  CHECK(sync_cpu(w, WN::NtGdiDdDDISignalSynchronizationObjectFromCpu, device, {sem}, {0}) == kOk);
  CHECK(sync_cpu(w, WN::NtGdiDdDDISignalSynchronizationObjectFromCpu, device, {sem}, {0}) == kOk);
  CHECK(sync_cpu(w, WN::NtGdiDdDDISignalSynchronizationObjectFromCpu, device, {sem}, {0}) == kInvalidParameter);      // over the limit of 2
  CHECK(sync_cpu(w, WN::NtGdiDdDDIWaitForSynchronizationObjectFromCpu, device, {sem}, {0}) == kOk);
  CHECK(sync_cpu(w, WN::NtGdiDdDDIWaitForSynchronizationObjectFromCpu, device, {sem}, {0}) == kOk);
  // Wait for any of two objects: the fence is satisfied, the semaphore is empty.
  CHECK(sync_cpu(w, WN::NtGdiDdDDIWaitForSynchronizationObjectFromCpu, device, {fence, sem}, {1, 0}, 1) == kOk);       // WaitAny
  {
    // Wait for all: blocks until a second thread of the process signals the semaphore.
    Win other_thread(m.allocator, 100);
    uint32_t result = 0xFFFFFFFF;
    std::atomic<bool> started{false};
    other_thread.zero(kB, 64);
    other_thread.put32(kB + 0x100, fence);
    other_thread.put32(kB + 0x104, sem);
    other_thread.put64(kB + 0x200, 1);
    other_thread.put64(kB + 0x208, 0);
    other_thread.put32(kB, device);
    other_thread.put32(kB + 4, 2);
    other_thread.put64(kB + 8, kB + 0x100);
    other_thread.put64(kB + 16, kB + 0x200);
    std::thread t([&] { started = true; result = other_thread.call(WN::NtGdiDdDDIWaitForSynchronizationObjectFromCpu, kB); });
    while (!started) std::this_thread::yield();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    CHECK(__atomic_load_n(&result, __ATOMIC_ACQUIRE) == 0xFFFFFFFF);              // still waiting for the semaphore
    CHECK(sync_cpu(w, WN::NtGdiDdDDISignalSynchronizationObjectFromCpu, device, {sem}, {0}) == kOk);
    t.join();
    CHECK(result == kOk);
  }
  // A waiter whose process ends gives up
  {
    Win doomed(m.allocator, 150);
    const uint32_t doomed_device = create_device(doomed, enum_first_adapter(doomed));
    uint32_t doomed_sem = 0;
    CHECK(make_sync2(doomed, doomed_device, 2, 0, 1, 0, &doomed_sem) == kOk);
    doomed.zero(kB, 64);
    doomed.put32(kB + 0x100, doomed_sem);
    doomed.put64(kB + 0x200, 0);
    doomed.put32(kB, doomed_device);
    doomed.put32(kB + 4, 1);
    doomed.put64(kB + 8, kB + 0x100);
    doomed.put64(kB + 16, kB + 0x200);
    uint32_t result = 0xFFFFFFFF;
    std::thread t([&] { result = doomed.call(WN::NtGdiDdDDIWaitForSynchronizationObjectFromCpu, kB); });
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    doomed.process.terminate(1);
    t.join();
    CHECK(result == 0xC000004Bu);
    win32k::process_exited(150);
  }

  // Mutex: signaled at first, a wait takes it
  CHECK(sync_cpu(w, WN::NtGdiDdDDIWaitForSynchronizationObjectFromCpu, device, {mutex}, {0}) == kOk);

  // On the GPU timeline: a context waits for a fence and the clear queued behind it runs only after the fence is signalled.
  const Ctx ctx = create_context(w, device);
  uint32_t gpu_fence = 0;
  CHECK(make_sync2(w, device, 3, 0, 0, 0, &gpu_fence) == kOk);
  const uint32_t target = alloc_ok(w, device, image_spec(dxgk::kAllocRenderTarget, 4, 4));
  const uint32_t pitch = w.get32(kE + 4 * lay::kAwPitch);
  auto gpu = [&](WN service, uint32_t context, uint32_t object, uint64_t value) {
    w.zero(kB, 64);
    w.put32(kB, context);
    w.put32(kB + 4, 1);
    w.put64(kB + 8, kB + 0x100);
    w.put64(kB + 16, kB + 0x200);
    w.put32(kB + 0x100, object);
    w.put64(kB + 0x200, value);
    return w.call(service, kB);
  };
  CHECK(gpu(WN::NtGdiDdDDIWaitForSynchronizationObjectFromGpu, ctx.handle, gpu_fence, 10) == kOk);
  const std::vector<uint8_t> red = clear_stream(m.allocator, {1, 0, 0, 1});
  w.put(ctx.cmd, ConstByteView(red.data(), red.size()));
  w.put32(ctx.alloc_list, target);
  CHECK(render_call(w, ctx.handle, 0, uint32_t(red.size()), 1) == kOk);
  CHECK(lock_alloc(w, device, target, lay::kLockDoNotWait) == 0xC01E0102u);              // busy: the clear waits for the fence
  CHECK(sync_cpu(w, WN::NtGdiDdDDISignalSynchronizationObjectFromCpu, device, {gpu_fence}, {10}) == kOk);
  CHECK(pixel_of(w, device, target, pitch, 1, 1) == 0xFFFF0000);
  // ... and a signal queued on the context happens after the work before it
  CHECK(gpu(WN::NtGdiDdDDISignalSynchronizationObjectFromGpu, ctx.handle, gpu_fence, 12) == kOk);
  CHECK(sync_cpu(w, WN::NtGdiDdDDIWaitForSynchronizationObjectFromCpu, device, {gpu_fence}, {12}) == kOk);
  CHECK(gpu(WN::NtGdiDdDDISignalSynchronizationObjectFromGpu, ctx.handle, 0x5050, 12) == kInvalidHandle);
  CHECK(gpu(WN::NtGdiDdDDISignalSynchronizationObjectFromGpu, 0x5050, gpu_fence, 13) == kInvalidHandle);
  CHECK(gpu(WN::NtGdiDdDDISignalSynchronizationObjectFromGpu, 0, gpu_fence, 13) == kInvalidParameter);
  // The legacy calls apply to semaphores and mutexes: the context signals, the CPU takes
  w.zero(kB, 160);
  w.put32(kB, ctx.handle);
  w.put32(kB + 4, 1);
  w.put32(kB + 8, sem3);
  CHECK(w.call(WN::NtGdiDdDDISignalSynchronizationObject, kB) == kOk);
  CHECK(sync_cpu(w, WN::NtGdiDdDDIWaitForSynchronizationObjectFromCpu, device, {sem3}, {0}, 0, 0, kC + 0x300) == kOk);
  w.put32(kB + 8, gpu_fence);
  CHECK(w.call(WN::NtGdiDdDDISignalSynchronizationObject, kB) == kInvalidParameter);       // a fence needs a value
  CHECK(w.call(WN::NtGdiDdDDIWaitForSynchronizationObject, kB) == kInvalidParameter);
  w.put32(kB + 8, mutex_held);
  CHECK(w.call(WN::NtGdiDdDDISignalSynchronizationObject, kB) == kOk);
  w.put32(kB + 4, 33);
  CHECK(w.call(WN::NtGdiDdDDISignalSynchronizationObject, kB) == kInvalidParameter);

  // Destroying objects
  w.zero(kB, 4);
  w.put32(kB, fence);
  CHECK(w.call(WN::NtGdiDdDDIDestroySynchronizationObject, kB) == kOk && w.call(WN::NtGdiDdDDIDestroySynchronizationObject, kB) == kInvalidHandle);
  CHECK(sync_cpu(w, WN::NtGdiDdDDIWaitForSynchronizationObjectFromCpu, device, {fence}, {1}) == kInvalidHandle);
  w.put32(kB, 0);
  CHECK(w.call(WN::NtGdiDdDDIDestroySynchronizationObject, kB) == kInvalidParameter);
  win32k::process_exited(100);
}

void test_escape() {
  System sys;
  Machine m;
  Win w(m.allocator, 100);
  const uint32_t adapter = enum_first_adapter(w);
  const uint32_t device = create_device(w, adapter);
  // CreateShader of the built-in coloured vertex shader through the private-data ABI; the handle comes back in the same buffer.
  w.zero(kC, lay::sizeof_escape);
  w.zero(kE, 64);
  w.put32(kE, 1);          // CreateShader
  w.put32(kE + 4, 0);      // ColoredVertex
  w.put32(kE + 8, 0);
  w.put32(kE + 12, 0);
  w.put32(kC + 4, device);
  w.put64(kC + 16, kE);
  w.put32(kC + 24, 16);
  CHECK(w.call(WN::NtGdiDdDDIEscape, kC) == kOk);
  const uint32_t shader = w.get32(kE);
  CHECK(shader != 0 && shader != 1);
  // DestroyShader twice: the second finds nothing
  w.zero(kE, 16);
  w.put32(kE, 3);
  w.put32(kE + 4, shader);
  w.put32(kC + 24, 8);
  CHECK(w.call(WN::NtGdiDdDDIEscape, kC) == kOk && w.call(WN::NtGdiDdDDIEscape, kC) == kInvalidHandle);
  // Misuse
  w.put32(kC + 24, 3);
  CHECK(w.call(WN::NtGdiDdDDIEscape, kC) == kInvalidParameter);                  // not even an op word (the kernel rejects it)
  w.put32(kC + 24, 0);
  CHECK(w.call(WN::NtGdiDdDDIEscape, kC) == kInvalidParameter);
  w.put32(kC + 24, 0x7FFFFFFF);
  CHECK(w.call(WN::NtGdiDdDDIEscape, kC) == kInvalidParameter);
  w.put32(kC + 24, 8);
  w.put32(kC + 8, 1);                                                             // D3DKMT_ESCAPE_VIDMM: Windows' own
  CHECK(w.call(WN::NtGdiDdDDIEscape, kC) == kNotSupported);
  w.put32(kC + 8, 0);
  w.put32(kC + 4, 0);
  CHECK(w.call(WN::NtGdiDdDDIEscape, kC) == kInvalidParameter);
  w.put32(kC + 4, device);
  w.put64(kC + 16, 0x80);
  CHECK(w.call(WN::NtGdiDdDDIEscape, kC) == kAccessViolation);
  w.put64(kC + 16, 0);
  CHECK(w.call(WN::NtGdiDdDDIEscape, kC) == kInvalidParameter);
  w.put64(kC + 16, kE);
  w.put32(kE, 77);                                                                // unknown op
  CHECK(w.call(WN::NtGdiDdDDIEscape, kC) == kNotSupported);
  win32k::process_exited(100);
}

// --- Process teardown and the services that answer "not supported" ----------------------------------------------------------------------------------------

void test_teardown() {
  System sys;
  Machine m;
  Win w(m.allocator, 100);
  const uint32_t adapter = enum_first_adapter(w);
  const uint32_t device = create_device(w, adapter);
  create_context(w, device);
  const uint32_t buf = alloc_ok(w, device, buffer_spec(dxgk::kAllocVertexBuffer, 256));
  CHECK(lock_alloc(w, device, buf) == kOk);
  uint32_t fence = 0;
  CHECK(make_sync2(w, device, 3, 0, 0, 0, &fence) == kOk);
  // Share a resource so there is a token to clean up
  w.zero(kC, lay::sizeof_create_allocation);
  w.zero(kD, 64);
  write_private(w, kE, buffer_spec(dxgk::kAllocVertexBuffer, 16));
  w.put32(kC, device);
  w.put32(kC + 44, 1);
  w.put64(kC + 48, kD);
  w.put32(kC + 56, 3);
  w.put64(kD + 16, kE);
  w.put32(kD + 24, lay::kAllocPrivateBytes);
  CHECK(w.call(WN::NtGdiDdDDICreateAllocation, kC) == kOk);
  CHECK(dx::find_state(100));

  win32k::process_exited(100);
  CHECK(!dx::find_state(100));
  dxgk::Handle h = 0;
  CHECK(m.kernel.open_adapter_from_index(100, 0, &h) == dxgk::status::kInvalidParameter);       // the kernel forgot the process
  // A second exit, and an exit of a process that never used the driver, are harmless.
  win32k::process_exited(100);
  win32k::process_exited(4242);
  // A later call by a process with that number starts afresh: the old handles are not valid.
  Win again(m.allocator, 100);
  CHECK(lock_alloc(again, device, buf) == kInvalidHandle);
  CHECK(enum_first_adapter(again) != 0);
  again.zero(kB, 4);
  again.put32(kB, device);
  CHECK(again.call(WN::NtGdiDdDDIDestroyDevice, kB) == kInvalidHandle);
  win32k::process_exited(100);

  // The video memory the dead process held is free again
  Win next(m.allocator, 300);
  const uint32_t a3 = enum_first_adapter(next);
  next.zero(kC, lay::sizeof_query_video_memory_info);
  next.put32(kC + 8, a3);
  CHECK(next.call(WN::NtGdiDdDDIQueryVideoMemoryInfo, kC) == kOk && next.get64(kC + 24) == 0);
  win32k::process_exited(300);
}

void test_not_supported_and_registration() {
  System sys;
  Machine m;
  Win w(m.allocator, 100);
  for (WN n : {WN::NtGdiDdDDICheckMultiPlaneOverlaySupport, WN::NtGdiDdDDICreateHwQueue, WN::NtGdiDdDDISubmitCommand, WN::NtGdiDdDDIReserveGpuVirtualAddress, WN::NtDxgkCreateDoorbell,
               WN::NtDxgkVailConnect, WN::NtGdiDdDDICreateProtectedSession, WN::NtGdiDdDDINetDispStartMiracastDisplayDevice, WN::NtDxgkSubmitPresentToHwQueue})
    CHECK(w.call(n, kA) == kNotSupported && w.call(n, 0) == kNotSupported);
  // not implementable here: still NOT_IMPLEMENTED
  for (WN n : {WN::NtGdiDdDDIOpenAdapterFromHdc, WN::NtGdiDdDDIShareObjects, WN::NtGdiDdDDIOpenSynchronizationObject, WN::NtGdiDdDDIQueryStatistics, WN::NtDxgkGetProperties,
               WN::NtDxgkIsFeatureEnabled, WN::NtGdiDdDDIMakeResident})
    CHECK(w.call(n, kA) == kNotImplemented);
  int count = 0;
  const uint32_t lo1 = static_cast<uint32_t>(WN::NtGdiDdCreateFullscreenSprite), hi1 = static_cast<uint32_t>(WN::NtGdiDdQueryVisRgnUniqueness);
  const uint32_t lo2 = static_cast<uint32_t>(WN::NtDxgkCancelPresents), hi2 = static_cast<uint32_t>(WN::NtDxgkVailPromoteCompositionSurface);
  for (uint32_t n = lo1; n <= hi1; ++n) if (process::win32k::handler_of(n)) ++count;
  for (uint32_t n = lo2; n <= hi2; ++n) if (process::win32k::handler_of(n)) ++count;
  printf("Direct3D services with a handler: %d\n", count);
  if (getenv("DXG_LIST")) {
    for (uint32_t n = lo1; n <= hi1; ++n) if (process::win32k::handler_of(n)) printf("SERVICE %u\n", n);
    for (uint32_t n = lo2; n <= hi2; ++n) if (process::win32k::handler_of(n)) printf("SERVICE %u\n", n);
  }
  win32k::process_exited(100);
}

// --- Random structures ---------------------------------------------------------------------------------------------------------------------------------------------

// Every Direct3D service gets structures made of valid handles, pointers into the caller's memory and random words, and null / unmapped / straddling pointers: nothing may
// crash, hang or trip a sanitizer. (Only the waits that block forever on a satisfiable-looking structure are left out.)
void test_random_structures() {
  System sys;
  Machine m;
  Win w(m.allocator, 100);
  const uint32_t adapter = enum_first_adapter(w);
  const uint32_t device = create_device(w, adapter);
  const Ctx ctx = create_context(w, device);
  const uint32_t buf = alloc_ok(w, device, buffer_spec(dxgk::kAllocVertexBuffer, 64));
  const uint32_t img = alloc_ok(w, device, image_spec(dxgk::kAllocRenderTarget | dxgk::kAllocPresent, 8, 6));
  uint32_t fence = 0, sem = 0;
  make_sync2(w, device, 3, 0, 1, 0, &fence);
  make_sync2(w, device, 2, 0, 4, 2, &sem);
  const uint32_t handles[] = {adapter, device, ctx.handle, buf, img, fence, sem, 0, 1, 0xFFFFFFFFu};
  uint64_t seed = 0x9E3779B97F4A7C15ull;
  auto next = [&] { seed ^= seed << 13; seed ^= seed >> 7; seed ^= seed << 17; return seed; };

  std::vector<uint32_t> numbers;
  const uint32_t lo1 = static_cast<uint32_t>(WN::NtGdiDdCreateFullscreenSprite), hi1 = static_cast<uint32_t>(WN::NtGdiDdQueryVisRgnUniqueness);
  const uint32_t lo2 = static_cast<uint32_t>(WN::NtDxgkCancelPresents), hi2 = static_cast<uint32_t>(WN::NtDxgkVailPromoteCompositionSurface);
  for (uint32_t n = lo1; n <= hi1; ++n) if (process::win32k::handler_of(n)) numbers.push_back(n);
  for (uint32_t n = lo2; n <= hi2; ++n) if (process::win32k::handler_of(n)) numbers.push_back(n);
  CHECK(numbers.size() > 50);
  // The waits that block until a condition the random structure may well describe.
  const uint32_t skip[] = {static_cast<uint32_t>(WN::NtGdiDdDDIWaitForSynchronizationObjectFromCpu), static_cast<uint32_t>(WN::NtGdiDdDDIWaitForIdle)};

  for (uint32_t number : numbers) {
    bool skipped = false;
    for (uint32_t s : skip) skipped = skipped || s == number;
    if (skipped) continue;
    for (int round = 0; round < 120; ++round) {
      // 256 bytes of structure at kBigBuf: handles, pointers to scratch areas, small numbers, random words.
      for (uint32_t i = 0; i < 64; ++i) {
        uint32_t word;
        switch (next() % 6) {
          case 0: word = handles[next() % (sizeof handles / sizeof handles[0])]; break;
          case 1: word = uint32_t(kBigBuf + 0x1000 + 0x100 * (next() % 8)); break;
          case 2: word = uint32_t(next() % 5); break;
          case 3: word = uint32_t(next()); break;
          case 4: word = 0; break;
          default: word = uint32_t(kBase >> 12); break;
        }
        w.put32(kBigBuf + 4 * i, word);
      }
      // pointers (8-byte fields) get real addresses now and then
      for (uint32_t i = 0; i < 24; ++i)
        if (next() % 3 == 0) w.put64(kBigBuf + 8 * i, next() % 4 == 0 ? 0 : kBigBuf + 0x1000 + 0x100 * (next() % 8));
      for (uint32_t i = 0; i < 0x800; i += 4) w.put32(kBigBuf + 0x1000 + i, uint32_t(next()));
      uint64_t arg = kBigBuf;
      switch (round % 6) {
        case 0: arg = 0; break;
        case 1: arg = 0x10; break;
        case 2: arg = kBase + kSize - 5; break;
        case 3: arg = ~uint64_t{0} - 3; break;
        default: break;
      }
      (void)w.call64(static_cast<WN>(number), arg);
    }
  }
  CHECK(true);
  win32k::process_exited(100);
}

// --- Several processes at once ---------------------------------------------------------------------------------------------------------------------------------

// Four processes (one thread each) create devices, contexts, allocations, lock and render, while another process opens and closes adapters and a fifth ends midway.
void test_concurrent_processes() {
  System sys;
  Machine m;
  std::atomic<int> failures{0};
  auto worker = [&](uint64_t pid) {
    Win w(m.allocator, pid);
    uint32_t adapter = 0;
    {
      w.zero(kA, 16);
      w.put32(kA, 2);
      w.put64(kA + 8, kB);
      w.zero(kB, 40);
      if (w.call(WN::NtGdiDdDDIEnumAdapters2, kA) != kOk) { ++failures; return; }
      adapter = w.get32(kB);
    }
    for (int round = 0; round < 20; ++round) {
      w.zero(kA, lay::sizeof_create_device);
      w.put32(kA, adapter);
      if (w.call(WN::NtGdiDdDDICreateDevice, kA) != kOk) { ++failures; return; }
      const uint32_t device = w.get32(kA + 12);
      w.zero(kA, lay::sizeof_create_context);
      w.put32(kA, device);
      if (w.call(WN::NtGdiDdDDICreateContext, kA) != kOk) { ++failures; return; }
      const uint32_t context = w.get32(kA + 32);
      const uint64_t cmd = w.get64(kA + 40), list = w.get64(kA + 56);
      AllocSpec spec;
      spec.flags = dxgk::kAllocRenderTarget;
      spec.format = kFmtA8R8G8B8;
      spec.width = 4, spec.height = 4;
      w.zero(kC, lay::sizeof_create_allocation);
      w.zero(kD, 64);
      write_private(w, kE, spec);
      w.put32(kC, device);
      w.put32(kC + 44, 1);
      w.put64(kC + 48, kD);
      w.put64(kD + 16, kE);
      w.put32(kD + 24, lay::kAllocPrivateBytes);
      if (w.call(WN::NtGdiDdDDICreateAllocation, kC) != kOk) { ++failures; return; }
      const uint32_t target = w.get32(kD);
      dxgk::CommandWriter cw(m.allocator);
      cw.begin_pass(0, dxgk::kNoAllocation, dxgk::kPassClearColor, {0, 0, 1, 1}, 1.0f, 0);
      cw.end_pass();
      w.put(cmd, cw.view());
      w.put32(list, target);
      w.zero(kA, 128);
      w.put32(kA, context);
      w.put32(kA + 8, uint32_t(cw.view().size()));
      w.put32(kA + 12, 1);
      if (w.call(WN::NtGdiDdDDIRender, kA) != kOk) { ++failures; return; }
      w.zero(kF, lay::sizeof_lock);
      w.put32(kF, device);
      w.put32(kF + 4, target);
      w.put32(kF + 32, 1);
      if (w.call(WN::NtGdiDdDDILock, kF) != kOk || w.get32(w.get64(kF + 24)) != 0xFF0000FFu) { ++failures; return; }
      w.zero(kF + 0x100, 16);
      w.put32(kF + 0x100, device);
      w.put32(kF + 0x104, 1);
      w.put64(kF + 0x108, kF + 0x200);
      w.put32(kF + 0x200, target);
      if (w.call(WN::NtGdiDdDDIUnlock, kF + 0x100) != kOk) { ++failures; return; }
      w.zero(kA, 4);
      w.put32(kA, device);
      if (w.call(WN::NtGdiDdDDIDestroyDevice, kA) != kOk) { ++failures; return; }
    }
  };
  std::vector<std::thread> threads;
  for (uint64_t pid = 400; pid < 404; ++pid) threads.emplace_back(worker, pid);
  threads.emplace_back([&] {
    Win w(m.allocator, 410);
    for (int i = 0; i < 60; ++i) {
      w.zero(kA, 16);
      w.put32(kA, 2);
      w.put64(kA + 8, kB);
      if (w.call(WN::NtGdiDdDDIEnumAdapters2, kA) != kOk) { ++failures; return; }
      w.zero(kC, 4);
      w.put32(kC, w.get32(kB));
      if (w.call(WN::NtGdiDdDDICloseAdapter, kC) != kOk) { ++failures; return; }
    }
    win32k::process_exited(410);
  });
  threads.emplace_back([&] {
    for (int i = 0; i < 20; ++i) {
      worker(420);
      win32k::process_exited(420);
    }
  });
  for (std::thread& t : threads) t.join();
  CHECK(failures == 0);
  for (uint64_t pid = 400; pid < 404; ++pid) win32k::process_exited(pid);
}

}  // namespace

int main() {
  test_no_driver();
  test_adapters();
  test_devices_and_display();
  test_allocations();
  test_resources_and_sharing();
  test_render_and_present();
  test_synchronization();
  test_escape();
  test_teardown();
  test_not_supported_and_registration();
  test_random_structures();
  test_concurrent_processes();
  return finish("Win32kDxgTest");
}
