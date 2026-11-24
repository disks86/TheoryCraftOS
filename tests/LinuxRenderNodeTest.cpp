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
// /dev/dri/renderD128 for Linux programs (src/Process/LinuxRenderNode.cpp over src/Graphics/VirtGpu/VirtGpuSession.cpp) through the real system calls
// of a Linux process, over a pretend virtio-gpu that records what it is asked and signals fences when the test says so: the node, /dev/dri and the /sys
// entries libdrm reads (with the reads libdrm's xf86drm.c makes, realpath() done the way glibc does it), the DRM and virtio-gpu ioctls, buffer mappings,
// fences as sync_files, PRIME dma-bufs, the node's events, descriptors shared by dup and by another process, malformed arguments and several threads.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <atomic>
#include <chrono>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>
#include "FileSystem/Ram/RamFileSystem.hpp"
#include "Graphics/VirtGpu/IVirtGpuHost.hpp"
#include "Graphics/VirtGpu/VirtioVirtGpuHost.hpp"
#include "Process/AddressSpace.hpp"
#include "Process/LinuxRenderNode.hpp"
#include "Process/Objects.hpp"
#include "Process/Process.hpp"
#include "Process/ProcessFiles.hpp"
#include "Process/Syscall.hpp"
#include "Process/UnixSockets.hpp"
#include "Process/VirtGpuDevice.h"

using namespace process;

struct TestAllocator final : allocators::IAllocator {
  std::atomic<int> live{0};
  void* allocate(uint64_t size) override { if (!size) return nullptr; ++live; return aligned_alloc(64, (size + 63) / 64 * 64); }
  void* allocate_aligned(uint64_t size, uint64_t alignment) override {
    if (!size) return nullptr;
    ++live;
    if (alignment < 64) alignment = 64;
    return aligned_alloc(alignment, (size + alignment - 1) / alignment * alignment);
  }
  void deallocate(void* p) override { if (p) { --live; free(p); } }
};
static std::atomic<int> g_failed{0}, g_checks{0};
#define CHECK(...) do { ++g_checks; if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)
static TestAllocator A;

typedef std::vector<uint8_t> Bytes;

// --- the pretend device --------------------------------------------------------------------------------------------------------------------------------

struct FakeGpu final : graphics::virtgpu::IVirtGpuHost {
  std::mutex lock;
  std::atomic<uint64_t> next_fence{0};
  std::set<uint64_t> signaled;
  bool signal_on_wait = true;      // a wait without NOWAIT finishes the work (as the host would)
  uint32_t next_context = 0, next_resource = 0;
  struct Context { uint32_t capset; std::string name; bool destroyed = false; };
  std::map<uint32_t, Context> contexts;
  struct Blob { uint32_t context, mem, flags; uint64_t blob_id, size; uint8_t* guest = nullptr; uint8_t* host = nullptr; bool unref = false; };
  std::map<uint32_t, Blob> blobs;
  struct Submission { uint32_t context, ring; Bytes commands; uint64_t fence; };
  std::vector<Submission> submissions;
  uint32_t unmaps = 0;

  bool has_3d() const override { return true; }
  bool has_blob() const override { return true; }
  bool has_host_visible() const override { return true; }
  bool has_context_init() const override { return true; }
  bool has_resource_uuid() const override { return false; }
  uint64_t capset_mask() const override { return (1u << 1) | (1u << 2) | (1u << 4); }      // virgl, virgl2, venus
  bool capset_info(uint32_t id, uint32_t* max_version, uint32_t* max_size) const override {
    if (!((capset_mask() >> id) & 1)) return false;
    *max_version = id == 4 ? 0 : 2;
    *max_size = 160;
    return true;
  }
  uint32_t read_capset(uint32_t id, uint32_t, ByteView out) override {
    for (uint64_t i = 0; i < out.size(); ++i) out[i] = uint8_t(id * 16 + i);
    return uint32_t(out.size());
  }
  uint32_t create_context(uint32_t capset, ConstByteView name) override {
    std::lock_guard<std::mutex> hold(lock);
    contexts[++next_context] = Context{capset, std::string(reinterpret_cast<const char*>(name.data()), name.size())};
    return next_context;
  }
  void destroy_context(uint32_t context) override {
    std::lock_guard<std::mutex> hold(lock);
    contexts[context].destroyed = true;
  }
  uint32_t create_blob(uint32_t context, uint32_t mem, uint32_t flags, uint64_t blob_id, uint64_t size, ArrayView<const graphics::virtgpu::GuestRun> pages) override {
    std::lock_guard<std::mutex> hold(lock);
    Blob b{context, mem, flags, blob_id, size};
    for (const auto& run : pages) b.guest = reinterpret_cast<uint8_t*>(run.address);
    if ((mem == 1 || mem == 3) && (!b.guest || pages.size() != 1 || pages[0].length != size)) return 0;
    blobs[++next_resource] = b;
    return next_resource;
  }
  bool map_blob(uint32_t, uint32_t resource, uint64_t size, uint64_t* physical, uint32_t* info) override {
    std::lock_guard<std::mutex> hold(lock);
    Blob& b = blobs[resource];
    if (b.host || b.mem != 2) return false;
    b.host = static_cast<uint8_t*>(aligned_alloc(4096, size));
    memset(b.host, 0, size);
    *physical = reinterpret_cast<uint64_t>(b.host);
    *info = 1;
    return true;
  }
  bool unmap_blob(uint32_t, uint32_t resource, uint64_t physical) override {
    std::lock_guard<std::mutex> hold(lock);
    Blob& b = blobs[resource];
    if (reinterpret_cast<uint64_t>(b.host) != physical) return false;
    free(b.host);
    b.host = nullptr;
    ++unmaps;
    return true;
  }
  uint32_t create_resource_3d(uint32_t context, const graphics::virtgpu::Resource3d&, ArrayView<const graphics::virtgpu::GuestRun> pages) override {
    std::lock_guard<std::mutex> hold(lock);
    Blob b{context, 0, 0, 0, pages.size() ? pages[0].length : 0};
    for (const auto& run : pages) b.guest = reinterpret_cast<uint8_t*>(run.address);
    blobs[++next_resource] = b;
    return next_resource;
  }
  bool unref_resource(uint32_t resource) override {
    std::lock_guard<std::mutex> hold(lock);
    blobs[resource].unref = true;
    return true;
  }
  bool attach_resource(uint32_t, uint32_t) override { return true; }
  uint64_t transfer(uint32_t, uint32_t, bool, const graphics::virtgpu::Box&, uint32_t, uint64_t, uint32_t, uint32_t) override { return ++next_fence; }
  uint64_t submit(uint32_t context, ConstByteView commands, uint32_t ring) override {
    std::lock_guard<std::mutex> hold(lock);
    const uint64_t f = ++next_fence;
    submissions.push_back(Submission{context, ring, Bytes(commands.data(), commands.data() + commands.size()), f});
    return f;
  }
  bool fence_signaled(uint64_t fence) override {
    std::lock_guard<std::mutex> hold(lock);
    return signaled.count(fence) != 0;
  }
  bool wait_fence(uint64_t fence, uint64_t) override {
    std::lock_guard<std::mutex> hold(lock);
    if (signaled.count(fence)) return true;
    if (!signal_on_wait) return false;
    signaled.insert(fence);
    return true;
  }
  uint64_t max_submit_bytes() const override { return 65536; }

  void signal(uint64_t fence) {
    std::lock_guard<std::mutex> hold(lock);
    signaled.insert(fence);
  }
  void signal_all() {
    std::lock_guard<std::mutex> hold(lock);
    for (uint64_t f = 1; f <= next_fence; ++f) signaled.insert(f);
  }
  ~FakeGpu() {
    for (auto& entry : blobs) free(entry.second.host);
  }
};

// The virtio-gpu-pci function QEMU makes (0000:00:02.0, vendor 1af4, device 1050, subsystem 1af4:1100, revision 1, class 038000).
static RenderNodePci make_pci() {
  RenderNodePci p;
  p.domain = 0, p.bus = 0, p.device = 2, p.function = 0;
  const uint8_t header[] = {0xF4, 0x1A, 0x50, 0x10, 0x07, 0x05, 0x10, 0x00, 0x01, 0x00, 0x80, 0x03};
  memcpy(p.config, header, sizeof(header));
  p.config[0x2C] = 0xF4, p.config[0x2D] = 0x1A, p.config[0x2E] = 0x00, p.config[0x2F] = 0x11;
  for (int i = 0x40; i < 256; ++i) p.config[i] = uint8_t(i);
  return p;
}
static RenderNodePci g_pci = make_pci();

// --- a Linux process -------------------------------------------------------------------------------------------------------------------------------------

struct Host final : IProcessHost {
  std::atomic<uint64_t> now{1000000000ull};
  std::function<void()> on_sleep;
  bool write_console(uint32_t, ConstByteView) override { return true; }
  bool read_console(ByteView, uint64_t* got) override { *got = 0; return true; }
  uint64_t realtime_ns() override { return 1700000000ull * 1000000000ull + now; }
  uint64_t monotonic_ns() override { return now += 1000; }
  void sleep_ns(uint64_t ns) override { now += ns; if (on_sleep) on_sleep(); }
  void yield() override {}
  void unhandled_syscall(SyscallAbi, uint64_t) override {}
};

constexpr uint64_t kONonBlock = 0x800, kORdWr = 2, kOWrOnly = 1, kODirectory = 0x10000, kOCloexec = 0x80000, kOCreat = 0x40, kOExcl = 0x80;
constexpr int64_t kEnoent = -2, kEintr = -4, kEbadf = -9, kEagain = -11, kEacces = -13, kEfault = -14, kEbusy = -16, kEexist = -17, kEnodev = -19,
                  kEnotdir = -20, kEisdir = -21, kEinval = -22, kEnotty = -25, kEspipe = -29, kEopnotsupp = -95;
constexpr uint64_t kBad = 0xDEAD0000;      // nothing is mapped there
constexpr uint32_t kSIfMt = 0170000, kSIfDir = 040000, kSIfReg = 0100000, kSIfChr = 020000, kSIfLnk = 0120000;
constexpr uint64_t kProtRW = 3, kMapShared = 1, kMapPrivate = 2;
constexpr uint32_t kSyncMerge = 0xC0303E03u, kSyncFileInfo = 0xC0383E04u, kSyncSetDeadline = 0x40103E05u;
constexpr uint32_t kDmaBufSync = 0x40086200u, kDmaBufSetName = 0x40086201u, kDmaBufExportSync = 0xC0086202u, kDmaBufImportSync = 0x40086203u;

struct Rig {
  AddressSpace space{A};
  Host host;
  Process* process = nullptr;
  uint64_t cursor = 0x10000;
  explicit Rig(filesystem::IFileSystem& fs) {
    CHECK(space.map(0x10000, 0x40000, kProtRead | kProtWrite));
    process = new Process(1, ProcessBinding{SyscallAbi::Linux, UserArch::X86_64, syscall_interface(SyscallAbi::Linux, UserArch::X86_64)}, space, host);
    process->attach_address_space(space, 0x100000);
    process->attach_file_system(A, fs);
  }
  ~Rig() { delete process; }
  void reset_memory() { cursor = 0x10000; }
  uint64_t alloc(uint64_t n) { const uint64_t at = cursor; cursor += (n + 15) & ~uint64_t{15}; CHECK(cursor <= 0x50000); return at; }
  uint64_t str(const std::string& s) {
    const uint64_t at = alloc(s.size() + 1);
    CHECK(space.write_privileged(at, ConstByteView(reinterpret_cast<const uint8_t*>(s.c_str()), s.size() + 1)));
    return at;
  }
  Bytes bytes(uint64_t at, uint64_t n) { Bytes b(n, 0); CHECK(space.read(at, ByteView(b.data(), n))); return b; }
  void put(uint64_t at, const void* data, uint64_t n) { CHECK(space.write_privileged(at, ConstByteView(static_cast<const uint8_t*>(data), n))); }
  template <class T> uint64_t place(const T& value) { const uint64_t at = alloc(sizeof(T)); put(at, &value, sizeof(T)); return at; }
  template <class T> T get(uint64_t at) { T value; const Bytes b = bytes(at, sizeof(T)); memcpy(&value, b.data(), sizeof(T)); return value; }
  int64_t s(uint64_t n, uint64_t a0 = 0, uint64_t a1 = 0, uint64_t a2 = 0, uint64_t a3 = 0, uint64_t a4 = 0, uint64_t a5 = 0) {
    SyscallRequest r;
    r.number = n;
    r.args[0] = a0; r.args[1] = a1; r.args[2] = a2; r.args[3] = a3; r.args[4] = a4; r.args[5] = a5;
    return int64_t(process->syscall(r));
  }
  int64_t open(const std::string& path, uint64_t flags) { return s(kOpen, str(path), flags); }
  int64_t ioctl(int64_t fd, uint32_t request, uint64_t argument) { return s(kIoctl, uint64_t(fd), request, argument); }
  template <class T> int64_t ioctl(int64_t fd, uint32_t request, T* value) {
    const uint64_t at = place(*value);
    const int64_t r = ioctl(fd, request, at);
    *value = get<T>(at);
    return r;
  }
  // stat(path) / fstat(fd): st_mode, st_rdev, st_size, st_ino (x86-64's struct stat).
  struct Stat { uint32_t mode = 0; uint64_t rdev = 0, size = 0, ino = 0; int64_t result = 0; };
  Stat stat_at(uint64_t buffer, int64_t result) {
    Stat st;
    st.result = result;
    if (result == 0) {
      const Bytes b = bytes(buffer, 144);
      memcpy(&st.ino, &b[8], 8);
      memcpy(&st.mode, &b[24], 4);
      memcpy(&st.rdev, &b[40], 8);
      memcpy(&st.size, &b[48], 8);
    }
    return st;
  }
  Stat stat(const std::string& path, bool link = false) { const uint64_t b = alloc(144); return stat_at(b, s(link ? kLstat : kStat, str(path), b)); }
  Stat fstat(int64_t fd) { const uint64_t b = alloc(144); return stat_at(b, s(kFstat, uint64_t(fd), b)); }
  std::string readlink(const std::string& path, int64_t* result = nullptr) {
    const uint64_t b = alloc(256);
    const int64_t n = s(kReadlink, str(path), b, 256);
    if (result) *result = n;
    if (n <= 0) return std::string();
    const Bytes t = bytes(b, uint64_t(n));
    return std::string(t.begin(), t.end());
  }
  std::string read_file(const std::string& path, int64_t* result = nullptr) {
    const int64_t fd = open(path, 0);
    if (result) *result = fd;
    if (fd < 0) return std::string();
    const uint64_t b = alloc(512);
    const int64_t n = s(kRead, uint64_t(fd), b, 512);
    s(kClose, uint64_t(fd));
    if (result) *result = n;
    if (n <= 0) return std::string();
    const Bytes t = bytes(b, uint64_t(n));
    return std::string(t.begin(), t.end());
  }
  // The names (and d_type) a folder lists.
  std::map<std::string, int> list(const std::string& path, int64_t* result = nullptr) {
    std::map<std::string, int> names;
    const int64_t fd = open(path, kODirectory);
    if (result) *result = fd;
    if (fd < 0) return names;
    const uint64_t b = alloc(4096);
    for (;;) {
      const int64_t n = s(kGetdents64, uint64_t(fd), b, 4096);
      if (n <= 0) break;
      const Bytes d = bytes(b, uint64_t(n));
      for (size_t at = 0; at < d.size();) {
        uint16_t length = 0;
        memcpy(&length, &d[at + 16], 2);
        names[std::string(reinterpret_cast<const char*>(&d[at + 19]))] = d[at + 18];
        at += length;
      }
    }
    s(kClose, uint64_t(fd));
    return names;
  }
  // poll() of one descriptor, timeout 0: revents (or the error).
  int64_t poll(int64_t fd, uint16_t events) {
    const uint64_t b = alloc(8);
    uint8_t pfd[8] = {uint8_t(fd), uint8_t(fd >> 8), 0, 0, uint8_t(events), uint8_t(events >> 8), 0, 0};
    put(b, pfd, 8);
    const int64_t r = s(kPoll, b, 1, 0);
    if (r < 0) return r;
    const Bytes out = bytes(b + 6, 2);
    return r == 0 ? 0 : int64_t(out[0] | out[1] << 8);
  }
  enum : uint64_t { kRead = 0, kWrite = 1, kOpen = 2, kClose = 3, kStat = 4, kFstat = 5, kLstat = 6, kPoll = 7, kLseek = 8, kMmap = 9, kMunmap = 11,
                    kIoctl = 16, kAccess = 21, kDup = 32, kFcntl = 72, kReadlink = 89, kGetdents64 = 217, kEpollWait = 232, kEpollCtl = 233,
                    kOpenat = 257, kEpollCreate1 = 291 };
};

struct World {
  filesystem::RamFileSystem fs{A};
  FakeGpu gpu;
  World(bool with_pci = true) {
    CHECK(fs.create_directory("/Linux") == filesystem::Status::Ok);
    graphics::virtgpu::set_system_host(&gpu);
    set_render_node_pci(with_pci ? &g_pci : nullptr);
  }
  ~World() {
    graphics::virtgpu::set_system_host(nullptr);
    set_render_node_pci(nullptr);
  }
};

// An open node with a Venus context (two rings, both polled).
static int64_t open_with_context(Rig& r, uint64_t poll_mask = 3, uint64_t rings = 2) {
  const int64_t fd = r.open("/dev/dri/renderD128", kORdWr | kOCloexec);
  CHECK(fd >= 0);
  TcosVirtgpuContextSetParam params[3] = {{TCOS_VIRTGPU_CONTEXT_PARAM_CAPSET_ID, 4}, {TCOS_VIRTGPU_CONTEXT_PARAM_NUM_RINGS, rings},
                                          {TCOS_VIRTGPU_CONTEXT_PARAM_POLL_RINGS_MASK, poll_mask}};
  const uint64_t at = r.alloc(sizeof(params));
  r.put(at, params, sizeof(params));
  TcosVirtgpuContextInit init{3, 0, at};
  CHECK(r.ioctl(fd, TCOS_DRM_IOCTL_VIRTGPU_CONTEXT_INIT, &init) == 0);
  return fd;
}

static uint32_t create_blob(Rig& r, int64_t fd, uint32_t mem, uint32_t flags, uint64_t size, uint64_t blob_id = 0, int64_t* result = nullptr) {
  TcosVirtgpuResourceCreateBlob b{};
  b.blob_mem = mem, b.blob_flags = flags, b.size = size, b.blob_id = blob_id;
  const int64_t rr = r.ioctl(fd, TCOS_DRM_IOCTL_VIRTGPU_RESOURCE_CREATE_BLOB, &b);
  if (result) *result = rr;
  return rr == 0 ? b.bo_handle : 0;
}
static uint64_t map_offset(Rig& r, int64_t fd, uint32_t handle, int64_t* result = nullptr) {
  TcosVirtgpuMap m{0, handle, 0};
  const int64_t rr = r.ioctl(fd, TCOS_DRM_IOCTL_VIRTGPU_MAP, &m);
  if (result) *result = rr;
  return m.offset;
}
static int64_t execbuffer(Rig& r, int64_t fd, uint32_t flags, uint32_t ring, std::vector<uint32_t> handles, int32_t fence_in, int32_t* fence_out,
                          uint32_t command_bytes = 8) {
  Bytes commands(command_bytes, 0);
  for (size_t i = 0; i < commands.size(); ++i) commands[i] = uint8_t(i + 1);
  const uint64_t c = r.alloc(commands.size() + 1);
  if (!commands.empty()) r.put(c, commands.data(), commands.size());
  const uint64_t h = r.alloc(handles.size() * 4 + 4);
  if (!handles.empty()) r.put(h, handles.data(), handles.size() * 4);
  TcosVirtgpuExecbuffer e{};
  e.flags = flags, e.size = command_bytes, e.command = c, e.bo_handles = h, e.num_bo_handles = uint32_t(handles.size()), e.fence_fd = fence_in, e.ring_idx = ring;
  const int64_t rr = r.ioctl(fd, TCOS_DRM_IOCTL_VIRTGPU_EXECBUFFER, &e);
  if (fence_out) *fence_out = e.fence_fd;
  return rr;
}

// --- the tests ---------------------------------------------------------------------------------------------------------------------------------------

static void test_no_device() {
  filesystem::RamFileSystem fs{A};
  CHECK(fs.create_directory("/Linux") == filesystem::Status::Ok);
  graphics::virtgpu::set_system_host(nullptr);
  set_render_node_pci(&g_pci);
  {
    Rig r(fs);
    CHECK(r.open("/dev/dri/renderD128", kORdWr) == kEnoent);
    CHECK(r.stat("/dev/dri").result == kEnoent);
    CHECK(r.stat("/sys/class/drm").result == kEnoent);
    CHECK(r.readlink("/sys/dev/char/226:128") == "");
  }
  // The device without its PCI function known: the node is there, /sys is not.
  FakeGpu gpu;
  graphics::virtgpu::set_system_host(&gpu);
  set_render_node_pci(nullptr);
  {
    Rig r(fs);
    const int64_t fd = r.open("/dev/dri/renderD128", kORdWr);
    CHECK(fd >= 0);
    CHECK(r.stat("/sys/dev/char/226:128").result == kEnoent);
    r.s(Rig::kClose, uint64_t(fd));
  }
  graphics::virtgpu::set_system_host(nullptr);
}

static void test_node_and_directory() {
  World w;
  Rig r(w.fs);
  const int64_t fd = r.open("/dev/dri/renderD128", kORdWr | kOCloexec);
  CHECK(fd >= 0);
  CHECK(r.s(Rig::kFcntl, uint64_t(fd), 1) == 1);      // FD_CLOEXEC
  CHECK(r.s(Rig::kFcntl, uint64_t(fd), 3) == int64_t(kORdWr));      // F_GETFL
  const Rig::Stat st = r.fstat(fd);
  CHECK(st.result == 0 && (st.mode & kSIfMt) == kSIfChr && st.rdev == 0xE280);      // makedev(226, 128)
  CHECK(linuxabi::linux_device_number(226, 128) == 0xE280 && linuxabi::linux_device_number(4096, 256) == 0x0000100000100000ull && linuxabi::linux_device_number(4095, 1048575) == 0xFFFFFFFFull);
  const Rig::Stat by_path = r.stat("/dev/dri/renderD128");
  CHECK(by_path.result == 0 && by_path.mode == st.mode && by_path.rdev == st.rdev && by_path.ino == st.ino);
  CHECK(r.stat("/dev/dri").result == 0 && (r.stat("/dev/dri").mode & kSIfMt) == kSIfDir);
  CHECK(r.stat("/dev/dri/card0").result == kEnoent);
  CHECK(r.stat("/dev/dri/renderD128/x").result == kEnotdir);
  std::map<std::string, int> names = r.list("/dev/dri");
  CHECK(names.size() == 3 && names.count(".") && names.count("..") && names["renderD128"] == 2);      // DT_CHR
  CHECK(r.open("/dev/dri/renderD128", kODirectory) == kEnotdir);
  CHECK(r.open("/dev/dri", kORdWr) == kEisdir);
  CHECK(r.open("/dev/dri/renderD128", kOCreat | kOExcl | kORdWr) == kEexist);
  CHECK(r.s(Rig::kAccess, r.str("/dev/dri/renderD128"), 6) == 0);
  // /proc/self/fd names it; read and write are not what a render node does without events.
  CHECK(r.readlink("/proc/self/fd/" + std::to_string(fd)) == "/dev/dri/renderD128");
  const uint64_t b = r.alloc(16);
  CHECK(r.s(Rig::kWrite, uint64_t(fd), b, 8) == kEinval);
  CHECK(r.s(Rig::kLseek, uint64_t(fd), 0, 2) == 0);
  // DRM_IOCTL_VERSION: "virtio_gpu" 0.1.0
  TcosDrmVersion v{};
  v.name_len = 32, v.name = r.alloc(32);
  const uint64_t name_at = v.name;
  CHECK(r.ioctl(fd, TCOS_DRM_IOCTL_VERSION, &v) == 0 && v.version_major == 0 && v.version_minor == 1 && v.name_len == 10);
  const Bytes name = r.bytes(name_at, 10);
  CHECK(std::string(name.begin(), name.end()) == "virtio_gpu");
  CHECK(r.s(Rig::kClose, uint64_t(fd)) == 0);
}

// What libdrm reads: drmGetDevices2 -> process_device -> drmParseSubsystemType, drmParsePciBusInfo, drmParsePciDeviceInfo; drmGetMinorNameForFD;
// drmGetDeviceNameFromFd2.
static void test_sysfs() {
  World w;
  Rig r(w.fs);
  // drmNodeIsDRM: stat of /sys/dev/char/226:128/device/drm
  CHECK(r.stat("/sys/dev/char/226:128/device/drm").result == 0 && (r.stat("/sys/dev/char/226:128/device/drm").mode & kSIfMt) == kSIfDir);
  // get_subsystem_type: readlink(".../device/subsystem") -> ".../pci"
  CHECK(r.readlink("/sys/dev/char/226:128/device/subsystem") == "../../../bus/pci");
  CHECK(r.readlink("/sys/dev/char/226:128") == "../../devices/pci0000:00/0000:00:02.0/drm/renderD128");
  CHECK(r.readlink("/sys/class/drm/renderD128") == "../../devices/pci0000:00/0000:00:02.0/drm/renderD128");
  CHECK(r.readlink("/sys/bus/pci/devices/0000:00:02.0") == "../../../devices/pci0000:00/0000:00:02.0");
  CHECK(r.readlink("/sys/devices/pci0000:00/0000:00:02.0/drm/renderD128/device") == "../../../0000:00:02.0");
  CHECK(r.readlink("/sys/devices/pci0000:00/0000:00:02.0/drm/renderD128/subsystem") == "../../../../../class/drm");
  // realpath() as glibc does it: readlink of each component (EINVAL: not a link), following the links it finds.
  int64_t res = 0;
  r.readlink("/sys", &res); CHECK(res == kEinval);
  r.readlink("/sys/dev", &res); CHECK(res == kEinval);
  r.readlink("/sys/dev/char", &res); CHECK(res == kEinval);
  r.readlink("/sys/dev/char/226:129", &res); CHECK(res == kEnoent);
  r.readlink("/sys/devices/pci0000:00/0000:00:02.0/drm/renderD128", &res); CHECK(res == kEinval);
  r.readlink("/sys/devices/pci0000:00/0000:00:02.0/vendor/x", &res); CHECK(res == kEnotdir);
  // lstat of a link, stat of what it leads to
  CHECK((r.stat("/sys/dev/char/226:128", true).mode & kSIfMt) == kSIfLnk);
  CHECK((r.stat("/sys/dev/char/226:128").mode & kSIfMt) == kSIfDir);
  CHECK(r.stat("/sys/dev/char/226:128").ino == r.stat("/sys/devices/pci0000:00/0000:00:02.0/drm/renderD128").ino);
  // drmParsePciBusInfo: PCI_SLOT_NAME of the PCI function's uevent
  const std::string uevent = r.read_file("/sys/dev/char/226:128/device/uevent");
  CHECK(uevent == "DRIVER=virtio-pci\nPCI_CLASS=38000\nPCI_ID=1AF4:1050\nPCI_SUBSYS_ID=1AF4:1100\nPCI_SLOT_NAME=0000:00:02.0\n"
                  "MODALIAS=pci:v00001AF4d00001050sv00001AF4sd00001100bc03sc80i00\n");
  // drmParsePciDeviceInfo: the separate files (fscanf "%x"), or config
  CHECK(r.read_file("/sys/dev/char/226:128/device/vendor") == "0x1af4\n");
  CHECK(r.read_file("/sys/dev/char/226:128/device/device") == "0x1050\n");
  CHECK(r.read_file("/sys/dev/char/226:128/device/subsystem_vendor") == "0x1af4\n");
  CHECK(r.read_file("/sys/dev/char/226:128/device/subsystem_device") == "0x1100\n");
  CHECK(r.read_file("/sys/dev/char/226:128/device/revision") == "0x01\n");
  CHECK(r.read_file("/sys/dev/char/226:128/device/class") == "0x038000\n");
  const std::string config = r.read_file("/sys/dev/char/226:128/device/config");
  CHECK(config.size() == 256 && memcmp(config.data(), g_pci.config, 256) == 0);      // (the system's user sees it all)
  CHECK(r.stat("/sys/dev/char/226:128/device/config").size == 256 && (r.stat("/sys/dev/char/226:128/device/vendor").mode & 0777) == 0444);
  r.process->linux_state().effective_user = 1000;
  const std::string config_user = r.read_file("/sys/dev/char/226:128/device/config");
  CHECK(config_user.size() == 64 && memcmp(config_user.data(), g_pci.config, 64) == 0);      // (anyone else the header only)
  CHECK(r.s(Rig::kAccess, r.str("/sys/dev/char/226:128/device/vendor"), 2) == kEacces);
  r.process->linux_state().effective_user = 0;
  // the minor: dev, uevent (drmGetDeviceNameFromFd2 reads DEVNAME from /sys/dev/char/226:128/uevent)
  CHECK(r.read_file("/sys/dev/char/226:128/dev") == "226:128\n");
  CHECK(r.read_file("/sys/dev/char/226:128/uevent") == "MAJOR=226\nMINOR=128\nDEVNAME=dri/renderD128\nDEVTYPE=drm_minor\n");
  CHECK(r.read_file("/sys/class/drm/version") == "drm 1.1.0 20060810\n");
  // drmGetMinorNameForFD: the names under device/drm
  std::map<std::string, int> names = r.list("/sys/dev/char/226:128/device/drm");
  CHECK(names.size() == 3 && names["renderD128"] == 4);      // DT_DIR
  names = r.list("/sys/class/drm");
  CHECK(names.count("renderD128") && names["renderD128"] == 10 && names["version"] == 8);      // DT_LNK, DT_REG
  names = r.list("/sys/devices/pci0000:00/0000:00:02.0");
  CHECK(names.count("vendor") && names.count("config") && names.count("drm") && names["subsystem"] == 10 && names.size() == 12);
  names = r.list("/sys/dev/char");
  CHECK(names.size() == 3 && names["226:128"] == 10);
  // A folder opened through a link: relative names from it, fstat of it.
  const int64_t dir = r.open("/sys/dev/char/226:128", kODirectory);
  CHECK(dir >= 0);
  CHECK((r.fstat(dir).mode & kSIfMt) == kSIfDir);
  const int64_t dev = r.s(Rig::kOpenat, uint64_t(dir), r.str("device/vendor"), 0);
  CHECK(dev >= 0);
  const uint64_t b = r.alloc(16);
  CHECK(r.s(Rig::kRead, uint64_t(dev), b, 16) == 7);
  CHECK(r.s(Rig::kRead, uint64_t(dev), b, 16) == 0);
  CHECK(r.s(Rig::kLseek, uint64_t(dev), 2, 0) == 2 && r.s(Rig::kRead, uint64_t(dev), b, 16) == 5);
  CHECK(r.fstat(dev).result == 0 && (r.fstat(dev).mode & kSIfMt) == kSIfReg && r.fstat(dev).size == 4096);
  CHECK(r.ioctl(dev, 0x5401, b) == kEnotty);
  CHECK(r.s(Rig::kMmap, 0, 4096, 1, kMapShared, uint64_t(dev), 0) == kEnodev);
  CHECK(r.readlink("/proc/self/fd/" + std::to_string(dev)) == "/sys/devices/pci0000:00/0000:00:02.0/vendor");
  r.s(Rig::kClose, uint64_t(dev));
  r.s(Rig::kClose, uint64_t(dir));
  // Attributes take nothing; missing names are missing.
  CHECK(r.open("/sys/dev/char/226:128/device/vendor", kOWrOnly) == kEacces);
  CHECK(r.open("/sys/dev/char/226:128/device/uevent", kORdWr) == kEacces);
  CHECK(r.open("/sys/class/drm", kORdWr | kODirectory) == kEisdir);
  CHECK(r.open("/sys/class/drm/card0", 0) == kEnoent);
  CHECK(r.open("/sys/class/drm/renderD128", 0x20000) == -40);      // O_NOFOLLOW on a link: ELOOP
  CHECK(r.stat("/sys/nothing").result == kEnoent);
  CHECK(r.open("/sys/dev/char/226:128/dev", kODirectory) == kEnotdir);
}

static void test_queries(World& w) {
  Rig r(w.fs);
  const int64_t fd = r.open("/dev/dri/renderD128", kORdWr);
  // GETPARAM: ints written where `value` points
  struct { uint64_t param; int32_t expected; } params[] = {{TCOS_VIRTGPU_PARAM_3D_FEATURES, 1}, {TCOS_VIRTGPU_PARAM_CAPSET_QUERY_FIX, 1},
      {TCOS_VIRTGPU_PARAM_RESOURCE_BLOB, 1}, {TCOS_VIRTGPU_PARAM_HOST_VISIBLE, 1}, {TCOS_VIRTGPU_PARAM_CROSS_DEVICE, 0},
      {TCOS_VIRTGPU_PARAM_CONTEXT_INIT, 1}, {TCOS_VIRTGPU_PARAM_SUPPORTED_CAPSET_IDS, 0x16}, {TCOS_VIRTGPU_PARAM_EXPLICIT_DEBUG_NAME, 1}};
  for (const auto& p : params) {
    const uint64_t value = r.alloc(8);
    r.put(value, Bytes(8, 0xEE).data(), 8);
    TcosVirtgpuGetparam g{p.param, value};
    CHECK(r.ioctl(fd, TCOS_DRM_IOCTL_VIRTGPU_GETPARAM, &g) == 0);
    CHECK(r.get<int32_t>(value) == p.expected && r.bytes(value + 4, 1)[0] == 0xEE);      // (an int: four bytes and no more)
  }
  TcosVirtgpuGetparam bad{99, r.alloc(8)};
  CHECK(r.ioctl(fd, TCOS_DRM_IOCTL_VIRTGPU_GETPARAM, &bad) == kEinval);
  TcosVirtgpuGetparam fault{TCOS_VIRTGPU_PARAM_3D_FEATURES, kBad};
  CHECK(r.ioctl(fd, TCOS_DRM_IOCTL_VIRTGPU_GETPARAM, &fault) == kEfault);
  CHECK(r.ioctl(fd, TCOS_DRM_IOCTL_VIRTGPU_GETPARAM, kBad) == kEfault);
  // GET_CAPS
  const uint64_t caps = r.alloc(256);
  TcosVirtgpuGetCaps c{4, 0, caps, 64, 0};
  CHECK(r.ioctl(fd, TCOS_DRM_IOCTL_VIRTGPU_GET_CAPS, &c) == 0);
  const Bytes got = r.bytes(caps, 64);
  bool same = true;
  for (int i = 0; i < 64; ++i) same = same && got[i] == uint8_t(4 * 16 + i);
  CHECK(same);
  TcosVirtgpuGetCaps wrong{3, 0, caps, 64, 0};
  CHECK(r.ioctl(fd, TCOS_DRM_IOCTL_VIRTGPU_GET_CAPS, &wrong) == kEinval);
  TcosVirtgpuGetCaps newer{4, 1, caps, 64, 0};
  CHECK(r.ioctl(fd, TCOS_DRM_IOCTL_VIRTGPU_GET_CAPS, &newer) == kEinval);
  TcosVirtgpuGetCaps empty{4, 0, caps, 0, 0};
  CHECK(r.ioctl(fd, TCOS_DRM_IOCTL_VIRTGPU_GET_CAPS, &empty) == kEinval);
  TcosVirtgpuGetCaps nowhere{4, 0, kBad, 64, 0};
  CHECK(r.ioctl(fd, TCOS_DRM_IOCTL_VIRTGPU_GET_CAPS, &nowhere) == kEfault);
  // GET_CAP (DRM core)
  TcosDrmGetCap prime{TCOS_DRM_CAP_PRIME, 0};
  CHECK(r.ioctl(fd, TCOS_DRM_IOCTL_GET_CAP, &prime) == 0 && prime.value == 3);
  TcosDrmGetCap syncobj{TCOS_DRM_CAP_SYNCOBJ, 7};
  CHECK(r.ioctl(fd, TCOS_DRM_IOCTL_GET_CAP, &syncobj) == 0 && syncobj.value == 0);
  // Unknown calls, calls a render node may not make, the type and size bits of the number (drm_ioctl looks at the number only, copies what fits).
  CHECK(r.ioctl(fd, TCOS_IOC(3, 'd', 0x7F, 8), r.alloc(8)) == kEinval);
  CHECK(r.ioctl(fd, TCOS_IOC(3, 'd', 0x02, 8), r.alloc(8)) == kEacces);      // DRM_IOCTL_GET_MAGIC
  CHECK(r.ioctl(fd, TCOS_IOC(3, 'd', 0xC0, 16), r.alloc(16)) == kEopnotsupp);      // DRM_IOCTL_SYNCOBJ_CREATE
  {
    // GETPARAM with a size of 8 (the param only): `value` is then 0, a write there faults.
    const uint64_t at = r.alloc(16);
    const uint64_t param = TCOS_VIRTGPU_PARAM_3D_FEATURES;
    r.put(at, &param, 8);
    CHECK(r.ioctl(fd, TCOS_IOC(3, 'd', 0x43, 8), at) == kEfault);
  }
  r.s(Rig::kClose, uint64_t(fd));
}

static void test_context_init(World& w) {
  Rig r(w.fs);
  const size_t before = w.gpu.contexts.size();
  const int64_t fd = r.open("/dev/dri/renderD128", kORdWr);
  const uint64_t name = r.str("vkcube");
  TcosVirtgpuContextSetParam params[4] = {{TCOS_VIRTGPU_CONTEXT_PARAM_CAPSET_ID, 4}, {TCOS_VIRTGPU_CONTEXT_PARAM_NUM_RINGS, 2},
                                          {TCOS_VIRTGPU_CONTEXT_PARAM_POLL_RINGS_MASK, 1}, {TCOS_VIRTGPU_CONTEXT_PARAM_DEBUG_NAME, name}};
  const uint64_t at = r.alloc(sizeof(params));
  r.put(at, params, sizeof(params));
  TcosVirtgpuContextInit init{4, 0, at};
  CHECK(r.ioctl(fd, TCOS_DRM_IOCTL_VIRTGPU_CONTEXT_INIT, &init) == 0);
  CHECK(w.gpu.contexts.size() == before + 1);
  const FakeGpu::Context made = w.gpu.contexts.rbegin()->second;
  CHECK(made.capset == 4 && made.name == "vkcube");
  CHECK(r.ioctl(fd, TCOS_DRM_IOCTL_VIRTGPU_CONTEXT_INIT, &init) == kEexist);
  r.s(Rig::kClose, uint64_t(fd));
  CHECK(w.gpu.contexts.rbegin()->second.destroyed);      // (the last descriptor took the session and its context along)
  // The cases Linux refuses, each on a new open.
  auto refused = [&](std::vector<TcosVirtgpuContextSetParam> list, uint32_t count, uint64_t address = 0) {
    const int64_t f = r.open("/dev/dri/renderD128", kORdWr);
    const uint64_t a = r.alloc(list.size() * sizeof(TcosVirtgpuContextSetParam) + 16);
    if (!list.empty()) r.put(a, list.data(), list.size() * sizeof(TcosVirtgpuContextSetParam));
    TcosVirtgpuContextInit i{count, 0, address ? address : a};
    const int64_t result = r.ioctl(f, TCOS_DRM_IOCTL_VIRTGPU_CONTEXT_INIT, &i);
    r.s(Rig::kClose, uint64_t(f));
    return result;
  };
  CHECK(refused({{TCOS_VIRTGPU_CONTEXT_PARAM_CAPSET_ID, 3}}, 1) == kEinval);      // a type the host does not have
  CHECK(refused({{TCOS_VIRTGPU_CONTEXT_PARAM_CAPSET_ID, 64}}, 1) == kEinval);
  CHECK(refused({{TCOS_VIRTGPU_CONTEXT_PARAM_CAPSET_ID, 4}, {TCOS_VIRTGPU_CONTEXT_PARAM_CAPSET_ID, 4}}, 2) == kEinval);      // twice
  CHECK(refused({{TCOS_VIRTGPU_CONTEXT_PARAM_NUM_RINGS, 65}}, 1) == kEinval);
  CHECK(refused({{TCOS_VIRTGPU_CONTEXT_PARAM_NUM_RINGS, 2}, {TCOS_VIRTGPU_CONTEXT_PARAM_POLL_RINGS_MASK, 4}}, 2) == kEinval);      // ring 2 of 2
  CHECK(refused({{77, 0}}, 1) == kEinval);
  CHECK(refused({}, 1, kBad) == kEfault);
  CHECK(refused({{TCOS_VIRTGPU_CONTEXT_PARAM_DEBUG_NAME, kBad}}, 1) == kEfault);
  // A context made implicitly (a blob before CONTEXT_INIT) is a virgl one, and CONTEXT_INIT afterwards is EEXIST.
  const int64_t f = r.open("/dev/dri/renderD128", kORdWr);
  CHECK(create_blob(r, f, TCOS_VIRTGPU_BLOB_MEM_GUEST, TCOS_VIRTGPU_BLOB_FLAG_USE_MAPPABLE, 4096) != 0);
  CHECK(w.gpu.contexts.rbegin()->second.capset == 0);
  CHECK(r.ioctl(f, TCOS_DRM_IOCTL_VIRTGPU_CONTEXT_INIT, &init) == kEexist);
  r.s(Rig::kClose, uint64_t(f));
}

static void test_blobs_and_mappings(World& w) {
  Rig r(w.fs);
  const int64_t fd = open_with_context(r);
  // Guest memory: the blob is page-rounded; the mapping and the device's view are the same memory.
  int64_t res = 0;
  const uint32_t guest = create_blob(r, fd, TCOS_VIRTGPU_BLOB_MEM_GUEST, TCOS_VIRTGPU_BLOB_FLAG_USE_MAPPABLE, 5000, 0, &res);
  CHECK(res == 0 && guest != 0);
  TcosVirtgpuResourceInfo info{guest, 0, 0, 0};
  CHECK(r.ioctl(fd, TCOS_DRM_IOCTL_VIRTGPU_RESOURCE_INFO, &info) == 0 && info.size == 8192 && info.blob_mem == TCOS_VIRTGPU_BLOB_MEM_GUEST);
  const FakeGpu::Blob gb = w.gpu.blobs[info.res_handle];
  CHECK(gb.guest != nullptr && gb.size == 8192 && gb.mem == 1);
  const uint64_t off = map_offset(r, fd, guest, &res);
  CHECK(res == 0 && off >= 0x100000000ull && off % 4096 == 0);
  CHECK(map_offset(r, fd, guest) == off);      // (the same offset again)
  const int64_t at = r.s(Rig::kMmap, 0, 8192, kProtRW, kMapShared, uint64_t(fd), off);
  CHECK(at > 0 && at % 4096 == 0);
  const char hello[] = "written by the program";
  CHECK(r.space.write(uint64_t(at) + 4096, ConstByteView(reinterpret_cast<const uint8_t*>(hello), sizeof(hello))));
  CHECK(memcmp(gb.guest + 4096, hello, sizeof(hello)) == 0);
  gb.guest[0] = 0x5A;      // (and what the device writes, the program reads)
  CHECK(r.bytes(uint64_t(at), 1)[0] == 0x5A);
  // mmap's refusals: copy-on-write, an offset nobody was given, a size past the buffer, an offset inside it.
  CHECK(r.s(Rig::kMmap, 0, 8192, kProtRW, kMapPrivate, uint64_t(fd), off) == kEinval);
  CHECK(r.s(Rig::kMmap, 0, 4096, kProtRW, kMapShared, uint64_t(fd), off + 0x10000000) == kEinval);
  CHECK(r.s(Rig::kMmap, 0, 12288, kProtRW, kMapShared, uint64_t(fd), off) == kEinval);
  CHECK(r.s(Rig::kMmap, 0, 4096, kProtRW, kMapShared, uint64_t(fd), off + 4096) == kEinval);
  CHECK(r.s(Rig::kMmap, 0, 4096, kProtRW, kMapShared, uint64_t(fd), off + 1) == kEinval);
  // Host memory, mappable: mapped into the device window when made; the program writes the host's memory.
  const uint32_t host = create_blob(r, fd, TCOS_VIRTGPU_BLOB_MEM_HOST3D, TCOS_VIRTGPU_BLOB_FLAG_USE_MAPPABLE, 4096, 7, &res);
  CHECK(res == 0 && host != 0);
  TcosVirtgpuResourceInfo hinfo{host, 0, 0, 0};
  CHECK(r.ioctl(fd, TCOS_DRM_IOCTL_VIRTGPU_RESOURCE_INFO, &hinfo) == 0 && hinfo.blob_mem == 2);
  CHECK(w.gpu.blobs[hinfo.res_handle].blob_id == 7 && w.gpu.blobs[hinfo.res_handle].host != nullptr);
  const uint64_t hoff = map_offset(r, fd, host);
  CHECK(hoff != off);
  const int64_t hat = r.s(Rig::kMmap, 0, 4096, kProtRW, kMapShared, uint64_t(fd), hoff);
  CHECK(hat > 0);
  CHECK(r.space.write(uint64_t(hat) + 100, ConstByteView(reinterpret_cast<const uint8_t*>(hello), sizeof(hello))));
  CHECK(memcmp(w.gpu.blobs[hinfo.res_handle].host + 100, hello, sizeof(hello)) == 0);
  // Host memory that is not mappable: it has an offset, but mmap refuses.
  const uint32_t opaque = create_blob(r, fd, TCOS_VIRTGPU_BLOB_MEM_HOST3D, TCOS_VIRTGPU_BLOB_FLAG_USE_SHAREABLE, 4096, 8);
  CHECK(opaque != 0);
  const uint64_t ooff = map_offset(r, fd, opaque, &res);
  CHECK(res == 0);
  CHECK(r.s(Rig::kMmap, 0, 4096, kProtRW, kMapShared, uint64_t(fd), ooff) == kEinval);
  // A read-only open of the node cannot map for writing.
  {
    const int64_t ro = r.open("/dev/dri/renderD128", 0);
    CHECK(r.s(Rig::kMmap, 0, 4096, kProtRW, kMapShared, uint64_t(ro), 0x100000000ull) == kEacces);
    r.s(Rig::kClose, uint64_t(ro));
  }
  // CREATE_BLOB's refusals
  create_blob(r, fd, TCOS_VIRTGPU_BLOB_MEM_GUEST, TCOS_VIRTGPU_BLOB_FLAG_USE_MAPPABLE, 0, 0, &res); CHECK(res == kEinval);
  create_blob(r, fd, 9, TCOS_VIRTGPU_BLOB_FLAG_USE_MAPPABLE, 4096, 0, &res); CHECK(res == kEinval);
  create_blob(r, fd, TCOS_VIRTGPU_BLOB_MEM_GUEST, 0, 4096, 0, &res); CHECK(res == kEinval);
  create_blob(r, fd, TCOS_VIRTGPU_BLOB_MEM_GUEST, 0x80, 4096, 0, &res); CHECK(res == kEinval);
  create_blob(r, fd, TCOS_VIRTGPU_BLOB_MEM_GUEST, TCOS_VIRTGPU_BLOB_FLAG_USE_MAPPABLE, 4096, 5, &res); CHECK(res == kEinval);      // (a guest blob has no id)
  create_blob(r, fd, TCOS_VIRTGPU_BLOB_MEM_HOST3D, TCOS_VIRTGPU_BLOB_FLAG_USE_CROSS_DEVICE, 4096, 1, &res); CHECK(res == kEinval);
  CHECK(r.ioctl(fd, TCOS_DRM_IOCTL_VIRTGPU_RESOURCE_CREATE_BLOB, kBad) == kEfault);
  // GEM_CLOSE: the handle goes (twice: EINVAL); the mapping keeps the memory.
  TcosDrmGemClose close{guest, 0};
  CHECK(r.ioctl(fd, TCOS_DRM_IOCTL_GEM_CLOSE, &close) == 0);
  CHECK(r.ioctl(fd, TCOS_DRM_IOCTL_GEM_CLOSE, &close) == kEinval);
  TcosVirtgpuResourceInfo gone{guest, 0, 0, 0};
  CHECK(r.ioctl(fd, TCOS_DRM_IOCTL_VIRTGPU_RESOURCE_INFO, &gone) == kEnoent);
  CHECK(!w.gpu.blobs[info.res_handle].unref);      // (still mapped)
  CHECK(r.bytes(uint64_t(at) + 4096, 4)[0] == 'w');
  CHECK(r.s(Rig::kMunmap, uint64_t(at), 8192) == 0);
  CHECK(w.gpu.blobs[info.res_handle].unref);      // (the last use gave the resource back)
  CHECK(r.s(Rig::kMunmap, uint64_t(hat), 4096) == 0);
  r.s(Rig::kClose, uint64_t(fd));
  CHECK(w.gpu.blobs[hinfo.res_handle].unref && w.gpu.blobs[hinfo.res_handle].host == nullptr);      // (unmapped from the window, then unreferenced)
}

static void test_fences(World& w) {
  Rig r(w.fs);
  const int64_t fd = open_with_context(r);
  const uint32_t bo = create_blob(r, fd, TCOS_VIRTGPU_BLOB_MEM_GUEST, TCOS_VIRTGPU_BLOB_FLAG_USE_MAPPABLE, 4096);
  int32_t out = -1;
  CHECK(execbuffer(r, fd, TCOS_VIRTGPU_EXECBUF_FENCE_FD_OUT | TCOS_VIRTGPU_EXECBUF_RING_IDX, 0, {bo}, -1, &out) == 0);
  CHECK(out >= 0);
  const FakeGpu::Submission sub = w.gpu.submissions.back();
  CHECK(sub.ring == 0 && sub.commands.size() == 8 && sub.commands[0] == 1 && sub.commands[7] == 8);
  CHECK(r.s(Rig::kFcntl, uint64_t(out), 1) == 1);      // (an out fence is close-on-exec)
  // Not signaled: poll says nothing, WAIT with NOWAIT is busy.
  CHECK(r.poll(out, 1) == 0);
  TcosVirtgpuWait nowait{bo, TCOS_VIRTGPU_WAIT_NOWAIT};
  CHECK(r.ioctl(fd, TCOS_DRM_IOCTL_VIRTGPU_WAIT, &nowait) == kEbusy);
  TcosVirtgpuWait missing{9999, TCOS_VIRTGPU_WAIT_NOWAIT};
  CHECK(r.ioctl(fd, TCOS_DRM_IOCTL_VIRTGPU_WAIT, &missing) == kEnoent);
  // SYNC_IOC_FILE_INFO before: status 0, one fence named as virtio-gpu names them.
  uint8_t info[56] = {};
  const uint64_t info_at = r.alloc(56);
  r.put(info_at, info, 56);
  CHECK(r.ioctl(out, kSyncFileInfo, info_at) == 0);
  Bytes got = r.bytes(info_at, 56);
  int32_t status = 0;
  uint32_t count = 0;
  memcpy(&status, &got[32], 4);
  memcpy(&count, &got[40], 4);
  CHECK(status == 0 && count == 1);
  CHECK(std::string(reinterpret_cast<const char*>(got.data())).rfind("virtio_gpu-controlq", 0) == 0);
  // dup shares it; a merge with an unrelated fence has both.
  const int64_t copy = r.s(Rig::kDup, uint64_t(out));
  CHECK(copy >= 0 && r.poll(copy, 1) == 0);
  int32_t other = -1;
  CHECK(execbuffer(r, fd, TCOS_VIRTGPU_EXECBUF_FENCE_FD_OUT | TCOS_VIRTGPU_EXECBUF_RING_IDX, 1, {}, -1, &other) == 0);
  const uint64_t other_fence = w.gpu.submissions.back().fence;
  uint8_t merge[48] = {};
  memcpy(merge, "joined", 6);
  memcpy(merge + 32, &other, 4);
  const uint64_t merge_at = r.alloc(48);
  r.put(merge_at, merge, 48);
  CHECK(r.ioctl(out, kSyncMerge, merge_at) == 0);
  int32_t merged = -1;
  memcpy(&merged, r.bytes(merge_at + 36, 4).data(), 4);
  CHECK(merged >= 0 && r.s(Rig::kFcntl, uint64_t(merged), 1) == 1);
  // FILE_INFO with room for the fences: each one's names and status.
  const uint64_t array = r.alloc(80 * 4);
  uint8_t ask[56] = {};
  uint32_t two = 2;
  memcpy(ask + 40, &two, 4);
  memcpy(ask + 48, &array, 8);
  r.put(info_at, ask, 56);
  CHECK(r.ioctl(merged, kSyncFileInfo, info_at) == 0);
  got = r.bytes(info_at, 56);
  memcpy(&count, &got[40], 4);
  CHECK(count == 2 && std::string(reinterpret_cast<const char*>(got.data())) == "joined");
  Bytes entries = r.bytes(array, 160);
  CHECK(std::string(reinterpret_cast<const char*>(&entries[0])) == "controlq" && std::string(reinterpret_cast<const char*>(&entries[32])) == "virtio_gpu");
  uint32_t one = 1;
  memcpy(ask + 40, &one, 4);
  r.put(info_at, ask, 56);
  CHECK(r.ioctl(merged, kSyncFileInfo, info_at) == kEinval);      // (room for fewer than it has)
  uint8_t flagged[56] = {};
  flagged[36] = 1;
  r.put(info_at, flagged, 56);
  CHECK(r.ioctl(merged, kSyncFileInfo, info_at) == kEinval);
  CHECK(r.ioctl(merged, kSyncFileInfo, kBad) == kEfault);
  // MERGE's refusals: flags, a second descriptor that is not a sync_file.
  merge[40] = 1;
  r.put(merge_at, merge, 48);
  CHECK(r.ioctl(out, kSyncMerge, merge_at) == kEinval);
  merge[40] = 0;
  const int32_t not_fence = int32_t(fd);
  memcpy(merge + 32, &not_fence, 4);
  r.put(merge_at, merge, 48);
  CHECK(r.ioctl(out, kSyncMerge, merge_at) == kEnoent);
  CHECK(r.ioctl(out, 0x5401, merge_at) == kEnotty);
  uint8_t deadline[16] = {};
  r.put(merge_at, deadline, 16);
  CHECK(r.ioctl(out, kSyncSetDeadline, merge_at) == 0);
  deadline[8] = 1;
  r.put(merge_at, deadline, 16);
  CHECK(r.ioctl(out, kSyncSetDeadline, merge_at) == kEinval);
  // Signaled: poll is readable, WAIT returns, FILE_INFO says 1 with a time.
  w.gpu.signal(sub.fence);
  CHECK(r.poll(out, 1) == 1 && r.poll(copy, 1) == 1);
  CHECK(r.poll(merged, 1) == 0);      // (one of its two still runs)
  CHECK(r.ioctl(fd, TCOS_DRM_IOCTL_VIRTGPU_WAIT, &nowait) == 0);
  w.gpu.signal(other_fence);
  CHECK(r.poll(merged, 1) == 1);
  memcpy(ask + 40, &two, 4);
  r.put(info_at, ask, 56);
  CHECK(r.ioctl(merged, kSyncFileInfo, info_at) == 0);
  got = r.bytes(info_at, 56);
  memcpy(&status, &got[32], 4);
  entries = r.bytes(array, 160);
  uint64_t stamp = 0;
  memcpy(&stamp, &entries[72], 8);
  CHECK(status == 1 && entries[64] == 1 && stamp != 0);
  // Merging two signaled fences leaves the stub (one signaled fence).
  memcpy(merge + 32, &other, 4);
  r.put(merge_at, merge, 48);
  CHECK(r.ioctl(out, kSyncMerge, merge_at) == 0);
  int32_t stub = -1;
  memcpy(&stub, r.bytes(merge_at + 36, 4).data(), 4);
  r.put(info_at, ask, 56);
  CHECK(r.ioctl(stub, kSyncFileInfo, info_at) == 0);
  got = r.bytes(info_at, 56);
  memcpy(&count, &got[40], 4);
  memcpy(&status, &got[32], 4);
  CHECK(count == 1 && status == 1 && std::string(reinterpret_cast<const char*>(r.bytes(array, 32).data())) == "stub");
  // Merging two fences of one timeline keeps the later.
  int32_t a = -1, b = -1;
  CHECK(execbuffer(r, fd, TCOS_VIRTGPU_EXECBUF_FENCE_FD_OUT | TCOS_VIRTGPU_EXECBUF_RING_IDX, 0, {}, -1, &a) == 0);
  CHECK(execbuffer(r, fd, TCOS_VIRTGPU_EXECBUF_FENCE_FD_OUT | TCOS_VIRTGPU_EXECBUF_RING_IDX, 0, {}, -1, &b) == 0);
  memcpy(merge + 32, &b, 4);
  r.put(merge_at, merge, 48);
  CHECK(r.ioctl(a, kSyncMerge, merge_at) == 0);
  int32_t later = -1;
  memcpy(&later, r.bytes(merge_at + 36, 4).data(), 4);
  r.put(info_at, ask, 56);
  CHECK(r.ioctl(later, kSyncFileInfo, info_at) == 0);
  memcpy(&count, r.bytes(info_at + 40, 4).data(), 4);
  CHECK(count == 1);
  // FENCE_FD_IN: a sync_file is taken; anything else is EINVAL. A fence of another timeline is waited for before the submission (the fake finishes it).
  CHECK(execbuffer(r, fd, TCOS_VIRTGPU_EXECBUF_FENCE_FD_IN | TCOS_VIRTGPU_EXECBUF_RING_IDX, 1, {}, later, nullptr) == 0);
  CHECK(r.poll(later, 1) == 1);      // (waited for: the fake signaled it)
  CHECK(execbuffer(r, fd, TCOS_VIRTGPU_EXECBUF_FENCE_FD_IN, 0, {}, int32_t(fd), nullptr) == kEinval);
  CHECK(execbuffer(r, fd, TCOS_VIRTGPU_EXECBUF_FENCE_FD_IN, 0, {}, 4000, nullptr) == kEinval);
  // EXECBUFFER's refusals.
  CHECK(execbuffer(r, fd, 0x80, 0, {}, -1, nullptr) == kEinval);
  CHECK(execbuffer(r, fd, TCOS_VIRTGPU_EXECBUF_RING_IDX, 2, {}, -1, nullptr) == kEinval);      // (two rings: 0 and 1)
  CHECK(execbuffer(r, fd, 0, 0, {12345}, -1, nullptr) == kEnoent);
  CHECK(execbuffer(r, fd, 0, 0, {}, -1, nullptr, 6) == kEinval);      // (not whole dwords)
  CHECK(r.ioctl(fd, TCOS_DRM_IOCTL_VIRTGPU_EXECBUFFER, kBad) == kEfault);
  {
    TcosVirtgpuExecbuffer e{};
    e.size = 8, e.command = kBad;
    CHECK(r.ioctl(fd, TCOS_DRM_IOCTL_VIRTGPU_EXECBUFFER, &e) == kEfault);
    e.command = r.alloc(8), e.num_bo_handles = 1, e.bo_handles = kBad;
    CHECK(r.ioctl(fd, TCOS_DRM_IOCTL_VIRTGPU_EXECBUFFER, &e) == kEfault);
    e.num_bo_handles = 0, e.num_in_syncobjs = 1;
    CHECK(r.ioctl(fd, TCOS_DRM_IOCTL_VIRTGPU_EXECBUFFER, &e) == kEinval);
  }
  // A sync_file reads, writes and seeks as Linux's does.
  const uint64_t buf = r.alloc(8);
  CHECK(r.s(Rig::kRead, uint64_t(out), buf, 8) == kEinval);
  CHECK(r.s(Rig::kWrite, uint64_t(out), buf, 8) == kEbadf);
  CHECK(r.s(Rig::kLseek, uint64_t(out), 0, 0) == kEspipe);
  CHECK(r.s(Rig::kMmap, 0, 4096, 1, kMapShared, uint64_t(out), 0) == kEnodev);
  CHECK(r.readlink("/proc/self/fd/" + std::to_string(out)) == "anon_inode:sync_file");
  CHECK(r.fstat(out).result == 0 && r.fstat(out).mode == 0600);
  for (int64_t f : {int64_t(out), copy, int64_t(other), int64_t(merged), int64_t(stub), int64_t(a), int64_t(b), int64_t(later)}) CHECK(r.s(Rig::kClose, uint64_t(f)) == 0);
  w.gpu.signal_all();
  r.s(Rig::kClose, uint64_t(fd));
}

static void test_prime(World& w) {
  Rig r(w.fs);
  const int64_t fd = open_with_context(r);
  const uint32_t bo = create_blob(r, fd, TCOS_VIRTGPU_BLOB_MEM_GUEST, TCOS_VIRTGPU_BLOB_FLAG_USE_MAPPABLE | TCOS_VIRTGPU_BLOB_FLAG_USE_SHAREABLE, 8192);
  TcosDrmPrimeHandle to_fd{bo, TCOS_DRM_CLOEXEC | TCOS_DRM_RDWR, -1};
  CHECK(r.ioctl(fd, TCOS_DRM_IOCTL_PRIME_HANDLE_TO_FD, &to_fd) == 0 && to_fd.fd >= 0);
  const int64_t dmabuf = to_fd.fd;
  CHECK(r.s(Rig::kFcntl, uint64_t(dmabuf), 1) == 1);
  CHECK(r.s(Rig::kLseek, uint64_t(dmabuf), 0, 2) == 8192);      // SEEK_END: the size
  CHECK(r.s(Rig::kLseek, uint64_t(dmabuf), 0, 0) == 0);
  CHECK(r.s(Rig::kLseek, uint64_t(dmabuf), 0, 1) == kEinval);      // (SEEK_CUR: no)
  CHECK(r.s(Rig::kLseek, uint64_t(dmabuf), 4, 2) == kEinval);
  CHECK(r.fstat(dmabuf).size == 8192 && r.fstat(dmabuf).mode == 0600);
  CHECK(r.readlink("/proc/self/fd/" + std::to_string(dmabuf)) == "/dmabuf:");
  // The same handle back in this session; another session gets a handle of its own for the same resource.
  TcosDrmPrimeHandle back{0, 0, int32_t(dmabuf)};
  CHECK(r.ioctl(fd, TCOS_DRM_IOCTL_PRIME_FD_TO_HANDLE, &back) == 0 && back.handle == bo);
  const int64_t second = r.open("/dev/dri/renderD128", kORdWr);
  TcosDrmPrimeHandle there{0, 0, int32_t(dmabuf)};
  CHECK(r.ioctl(second, TCOS_DRM_IOCTL_PRIME_FD_TO_HANDLE, &there) == 0 && there.handle != 0);
  TcosVirtgpuResourceInfo i1{bo, 0, 0, 0}, i2{there.handle, 0, 0, 0};
  CHECK(r.ioctl(fd, TCOS_DRM_IOCTL_VIRTGPU_RESOURCE_INFO, &i1) == 0 && r.ioctl(second, TCOS_DRM_IOCTL_VIRTGPU_RESOURCE_INFO, &i2) == 0 && i1.res_handle == i2.res_handle);
  // A second export is a descriptor of the same buffer (same inode).
  TcosDrmPrimeHandle again{bo, 0, -1};
  CHECK(r.ioctl(fd, TCOS_DRM_IOCTL_PRIME_HANDLE_TO_FD, &again) == 0 && r.fstat(again.fd).ino == r.fstat(dmabuf).ino);
  CHECK(r.s(Rig::kMmap, 0, 8192, kProtRW, kMapShared, uint64_t(again.fd), 0) == kEacces);      // (exported without DRM_RDWR: read-only)
  CHECK(r.s(Rig::kMmap, 0, 8192, 1, kMapShared, uint64_t(again.fd), 0) > 0);
  // mmap of the dma-buf and of the node show the same memory.
  const int64_t via_buf = r.s(Rig::kMmap, 0, 4096, kProtRW, kMapShared, uint64_t(dmabuf), 4096);
  const uint64_t off = map_offset(r, fd, bo);
  const int64_t via_node = r.s(Rig::kMmap, 0, 8192, kProtRW, kMapShared, uint64_t(fd), off);
  CHECK(via_buf > 0 && via_node > 0);
  const uint8_t mark[4] = {1, 2, 3, 4};
  CHECK(r.space.write(uint64_t(via_buf), ConstByteView(mark, 4)));
  CHECK(r.bytes(uint64_t(via_node) + 4096, 4) == Bytes(mark, mark + 4));
  CHECK(r.s(Rig::kMmap, 0, 8192, kProtRW, kMapShared, uint64_t(dmabuf), 4096) == kEinval);      // (past the end)
  CHECK(r.s(Rig::kMmap, 0, 4096, kProtRW, kMapPrivate, uint64_t(dmabuf), 0) == kEinval);
  // DMA_BUF_IOCTL_SYNC and SET_NAME; nothing else.
  const uint64_t flags_at = r.alloc(8);
  uint64_t sync = 1 | 2;
  r.put(flags_at, &sync, 8);
  CHECK(r.ioctl(dmabuf, kDmaBufSync, flags_at) == 0);
  sync = 4 | 2;
  r.put(flags_at, &sync, 8);
  CHECK(r.ioctl(dmabuf, kDmaBufSync, flags_at) == 0);
  sync = 4;
  r.put(flags_at, &sync, 8);
  CHECK(r.ioctl(dmabuf, kDmaBufSync, flags_at) == kEinval);
  sync = 9;
  r.put(flags_at, &sync, 8);
  CHECK(r.ioctl(dmabuf, kDmaBufSync, flags_at) == kEinval);
  CHECK(r.ioctl(dmabuf, kDmaBufSync, kBad) == kEfault);
  CHECK(r.ioctl(dmabuf, kDmaBufSetName, r.str("swapchain")) == 0);
  CHECK(r.ioctl(dmabuf, kDmaBufSetName, r.str(std::string(32, 'x'))) == kEinval);
  CHECK(r.ioctl(dmabuf, 0x5401, flags_at) == kEnotty);
  // EXPORT_SYNC_FILE: the stub while idle; the buffer's fence while the GPU uses it. IMPORT_SYNC_FILE takes a sync_file only.
  uint8_t xs[8] = {2, 0, 0, 0, 0xFF, 0xFF, 0xFF, 0xFF};
  const uint64_t xs_at = r.alloc(8);
  r.put(xs_at, xs, 8);
  CHECK(r.ioctl(dmabuf, kDmaBufExportSync, xs_at) == 0);
  int32_t idle_fence = -1;
  memcpy(&idle_fence, r.bytes(xs_at + 4, 4).data(), 4);
  CHECK(idle_fence >= 0 && r.poll(idle_fence, 1) == 1);
  CHECK(r.poll(dmabuf, 5) == 5);      // (POLLIN | POLLOUT: nobody uses it)
  int32_t busy = -1;
  CHECK(execbuffer(r, fd, TCOS_VIRTGPU_EXECBUF_FENCE_FD_OUT, 0, {bo}, -1, &busy) == 0);
  CHECK(r.poll(dmabuf, 5) == 0);
  r.put(xs_at, xs, 8);
  CHECK(r.ioctl(dmabuf, kDmaBufExportSync, xs_at) == 0);
  int32_t busy_fence = -1;
  memcpy(&busy_fence, r.bytes(xs_at + 4, 4).data(), 4);
  CHECK(r.poll(busy_fence, 1) == 0);
  uint8_t bad_flags[8] = {0, 0, 0, 0, 0, 0, 0, 0};
  r.put(xs_at, bad_flags, 8);
  CHECK(r.ioctl(dmabuf, kDmaBufExportSync, xs_at) == kEinval);
  uint8_t imp[8] = {2, 0, 0, 0};
  memcpy(imp + 4, &idle_fence, 4);
  r.put(xs_at, imp, 8);
  CHECK(r.ioctl(dmabuf, kDmaBufImportSync, xs_at) == 0);
  memcpy(imp + 4, &dmabuf, 4);
  r.put(xs_at, imp, 8);
  CHECK(r.ioctl(dmabuf, kDmaBufImportSync, xs_at) == kEinval);
  w.gpu.signal_all();
  CHECK(r.poll(dmabuf, 5) == 5 && r.poll(busy_fence, 1) == 1);
  // Importing what is not a dma-buf of this device: EINVAL (a sync_file, a /sys file, a closed descriptor).
  TcosDrmPrimeHandle wrong{0, 0, busy};
  CHECK(r.ioctl(fd, TCOS_DRM_IOCTL_PRIME_FD_TO_HANDLE, &wrong) == kEinval);
  const int64_t sysfile = r.open("/sys/class/drm/version", 0);
  wrong.fd = int32_t(sysfile);
  CHECK(r.ioctl(fd, TCOS_DRM_IOCTL_PRIME_FD_TO_HANDLE, &wrong) == kEinval);
  wrong.fd = 4000;
  CHECK(r.ioctl(fd, TCOS_DRM_IOCTL_PRIME_FD_TO_HANDLE, &wrong) == kEinval);
  TcosDrmPrimeHandle no_handle{777, 0, -1};
  CHECK(r.ioctl(fd, TCOS_DRM_IOCTL_PRIME_HANDLE_TO_FD, &no_handle) == kEnoent);
  TcosDrmPrimeHandle bad_flag{bo, 0x10, -1};
  CHECK(r.ioctl(fd, TCOS_DRM_IOCTL_PRIME_HANDLE_TO_FD, &bad_flag) == kEinval);
  // A dma-buf keeps its buffer after the handle and the session are gone.
  TcosDrmGemClose close{bo, 0};
  CHECK(r.ioctl(fd, TCOS_DRM_IOCTL_GEM_CLOSE, &close) == 0);
  r.s(Rig::kClose, uint64_t(fd));
  CHECK(!w.gpu.blobs[i1.res_handle].unref);
  CHECK(r.s(Rig::kRead, uint64_t(dmabuf), flags_at, 8) == kEinval);
  for (int64_t f : {dmabuf, int64_t(again.fd), second, int64_t(idle_fence), int64_t(busy), int64_t(busy_fence), sysfile}) CHECK(r.s(Rig::kClose, uint64_t(f)) == 0);
  CHECK(r.s(Rig::kMunmap, uint64_t(via_buf), 4096) == 0 && r.s(Rig::kMunmap, uint64_t(via_node), 8192) == 0);
}

static void test_events(World& w) {
  Rig r(w.fs);
  const int64_t fd = open_with_context(r, 1);      // ring 0 polled, ring 1 not
  CHECK(r.poll(fd, 1) == 0);
  CHECK(execbuffer(r, fd, TCOS_VIRTGPU_EXECBUF_RING_IDX, 0, {}, -1, nullptr) == 0);
  const uint64_t first = w.gpu.submissions.back().fence;
  CHECK(execbuffer(r, fd, TCOS_VIRTGPU_EXECBUF_RING_IDX, 1, {}, -1, nullptr) == 0);
  const uint64_t unpolled = w.gpu.submissions.back().fence;
  // Nothing yet: poll 0, a non-blocking read EAGAIN.
  CHECK(r.s(Rig::kFcntl, uint64_t(fd), 4, kONonBlock) == 0 && (r.s(Rig::kFcntl, uint64_t(fd), 3) & int64_t(kONonBlock)));
  const uint64_t buf = r.alloc(64);
  CHECK(r.s(Rig::kRead, uint64_t(fd), buf, 64) == kEagain);
  w.gpu.signal(unpolled);
  CHECK(r.poll(fd, 1) == 0);      // (ring 1 is not one it asked about)
  w.gpu.signal(first);
  CHECK(r.poll(fd, 1 | 4) == (0x1));      // POLLIN only (a render node is never "writable")
  CHECK(r.s(Rig::kRead, uint64_t(fd), buf, 4) == 0);      // (an event does not fit: nothing)
  CHECK(r.s(Rig::kRead, uint64_t(fd), kBad, 64) == kEfault);
  CHECK(r.poll(fd, 1) == 1);      // (and it is still there)
  CHECK(r.s(Rig::kRead, uint64_t(fd), buf, 64) == 8);
  const Bytes e = r.bytes(buf, 8);
  uint32_t type = 0, length = 0;
  memcpy(&type, &e[0], 4);
  memcpy(&length, &e[4], 4);
  CHECK(type == TCOS_VIRTGPU_EVENT_FENCE_SIGNALED && length == 8);
  CHECK(r.s(Rig::kRead, uint64_t(fd), buf, 64) == kEagain);
  // Several at once.
  for (int i = 0; i < 3; ++i) CHECK(execbuffer(r, fd, TCOS_VIRTGPU_EXECBUF_RING_IDX, 0, {}, -1, nullptr) == 0);
  w.gpu.signal_all();
  CHECK(r.s(Rig::kRead, uint64_t(fd), buf, 64) == 24);
  // A blocking read waits until the fence signals (the fake signals it while the reader sleeps).
  CHECK(r.s(Rig::kFcntl, uint64_t(fd), 4, 0) == 0);
  CHECK(execbuffer(r, fd, TCOS_VIRTGPU_EXECBUF_RING_IDX, 0, {}, -1, nullptr) == 0);
  const uint64_t later = w.gpu.submissions.back().fence;
  int sleeps = 0;
  r.host.on_sleep = [&] { if (++sleeps == 3) w.gpu.signal(later); };
  CHECK(r.s(Rig::kRead, uint64_t(fd), buf, 64) == 8 && sleeps >= 3);
  r.host.on_sleep = nullptr;
  // A write-only open cannot read.
  const int64_t wo = r.open("/dev/dri/renderD128", kOWrOnly);
  CHECK(r.s(Rig::kRead, uint64_t(wo), buf, 64) == kEbadf);
  r.s(Rig::kClose, uint64_t(wo));
  r.s(Rig::kClose, uint64_t(fd));
}

// epoll over the node and a fence (a system with unix sockets: the waits go through the socket manager).
static void test_epoll(World& w) {
  struct NoWaitSync final : ISystemSync {
    void lock() override {}
    void unlock() override {}
    bool wait(uint64_t) override { return true; }
    void notify_all() override {}
  };
  static NoWaitSync sync;
  UnixSocketManager sockets(sync, A);
  set_system_unix_sockets(&sockets);
  {
    Rig r(w.fs);
    const int64_t fd = open_with_context(r, 1);
    int32_t fence = -1;
    CHECK(execbuffer(r, fd, TCOS_VIRTGPU_EXECBUF_RING_IDX | TCOS_VIRTGPU_EXECBUF_FENCE_FD_OUT, 0, {}, -1, &fence) == 0);
    const int64_t ep = r.s(Rig::kEpollCreate1, 0);
    CHECK(ep >= 0);
    const uint64_t ev = r.alloc(12);
    uint8_t want[12] = {0x01, 0, 0, 0, 7, 0, 0, 0, 0, 0, 0, 0};
    r.put(ev, want, 12);
    CHECK(r.s(Rig::kEpollCtl, uint64_t(ep), 1, uint64_t(fd), ev) == 0);
    want[4] = 9;
    r.put(ev, want, 12);
    CHECK(r.s(Rig::kEpollCtl, uint64_t(ep), 1, uint64_t(fence), ev) == 0);
    const uint64_t got = r.alloc(12 * 4);
    CHECK(r.s(Rig::kEpollWait, uint64_t(ep), got, 4, 0) == 0);
    w.gpu.signal_all();
    CHECK(r.s(Rig::kEpollWait, uint64_t(ep), got, 4, 0) == 2);
    // A poll with a timeout looks again until the fence signals (the fake signals it after a few sleeps).
    int32_t later = -1;
    CHECK(execbuffer(r, fd, TCOS_VIRTGPU_EXECBUF_RING_IDX | TCOS_VIRTGPU_EXECBUF_FENCE_FD_OUT, 1, {}, -1, &later) == 0);
    const uint64_t f = w.gpu.submissions.back().fence;
    // (no timeout: the wait looks again every millisecond of the system's clock until the device, another thread here, signals the fence)
    std::thread device([&] {
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
      w.gpu.signal(f);
    });
    const uint64_t pfd = r.alloc(8);
    uint8_t p[8] = {uint8_t(later), 0, 0, 0, 1, 0, 0, 0};
    r.put(pfd, p, 8);
    CHECK(r.s(Rig::kPoll, pfd, 1, uint64_t(-1)) == 1);
    device.join();
    for (int64_t d : {int64_t(fence), int64_t(later), ep, fd}) r.s(Rig::kClose, uint64_t(d));
  }
  set_system_unix_sockets(nullptr);
}

// One open file in two processes (what fork and SCM_RIGHTS give): the same session; it goes with the last descriptor.
static void test_sharing(World& w) {
  Rig a(w.fs), b(w.fs);
  const int64_t fd = open_with_context(a);
  const uint32_t context = w.gpu.next_context;
  const uint32_t bo = create_blob(a, fd, TCOS_VIRTGPU_BLOB_MEM_GUEST, TCOS_VIRTGPU_BLOB_FLAG_USE_MAPPABLE, 4096);
  const int64_t in_b = b.process->files().install(DescriptorKind::RenderNode, a.process->files().shared_object(uint64_t(fd)), 0);
  CHECK(in_b >= 0);
  TcosVirtgpuResourceInfo info{bo, 0, 0, 0};
  CHECK(b.ioctl(in_b, TCOS_DRM_IOCTL_VIRTGPU_RESOURCE_INFO, &info) == 0);      // (a handle of the one session)
  const int64_t dup = a.s(Rig::kDup, uint64_t(fd));
  CHECK(a.s(Rig::kClose, uint64_t(fd)) == 0 && a.s(Rig::kClose, uint64_t(dup)) == 0);
  CHECK(!w.gpu.contexts[context].destroyed);
  CHECK(b.ioctl(in_b, TCOS_DRM_IOCTL_VIRTGPU_RESOURCE_INFO, &info) == 0);
  CHECK(b.s(Rig::kClose, uint64_t(in_b)) == 0);
  CHECK(w.gpu.contexts[context].destroyed && w.gpu.blobs[info.res_handle].unref);
}

// Several threads on one session (each its own process holding the same open file), the device signaling meanwhile.
static void test_threads(World& w) {
  Rig owner(w.fs);
  const int64_t fd = open_with_context(owner, 3);
  std::atomic<bool> done{false};
  std::atomic<int> submitted{0}, failures{0};
  std::thread device([&] {
    while (!done) {
      w.gpu.signal_all();
      std::this_thread::yield();
    }
  });
  std::vector<std::thread> workers;
  for (int t = 0; t < 4; ++t)
    workers.emplace_back([&, t] {
      Rig r(w.fs);
      const int64_t mine = r.process->files().install(DescriptorKind::RenderNode, owner.process->files().shared_object(uint64_t(fd)), 0);
      if (mine < 0) { ++failures; return; }
      for (int i = 0; i < 60; ++i) {
        r.reset_memory();
        const uint32_t bo = create_blob(r, mine, TCOS_VIRTGPU_BLOB_MEM_GUEST, TCOS_VIRTGPU_BLOB_FLAG_USE_MAPPABLE, 4096);
        if (!bo) { ++failures; continue; }
        int32_t fence = -1;
        if (execbuffer(r, mine, TCOS_VIRTGPU_EXECBUF_RING_IDX | TCOS_VIRTGPU_EXECBUF_FENCE_FD_OUT, uint32_t(t & 1), {bo}, -1, &fence) != 0) ++failures;
        else ++submitted;
        if (t == 0) {      // (one reader of the node's events)
          const uint64_t buf = r.alloc(512);
          r.s(Rig::kFcntl, uint64_t(mine), 4, kONonBlock);
          const int64_t n = r.s(Rig::kRead, uint64_t(mine), buf, 512);
          if (n != kEagain && (n <= 0 || n % 8 != 0)) ++failures;
        }
        r.poll(fence, 1);
        TcosVirtgpuWait wait{bo, 0};
        if (r.ioctl(mine, TCOS_DRM_IOCTL_VIRTGPU_WAIT, &wait) != 0) ++failures;
        TcosDrmGemClose close{bo, 0};
        if (r.ioctl(mine, TCOS_DRM_IOCTL_GEM_CLOSE, &close) != 0) ++failures;
        r.s(Rig::kClose, uint64_t(fence));
      }
      r.s(Rig::kClose, uint64_t(mine));
    });
  for (std::thread& t : workers) t.join();
  done = true;
  device.join();
  CHECK(failures == 0 && submitted == 240);
  owner.s(Rig::kClose, uint64_t(fd));
}

int main() {
  const int live_at_start = A.live;
  test_no_device();
  test_node_and_directory();
  test_sysfs();
  {
    World w;
    test_queries(w);
    test_context_init(w);
    test_blobs_and_mappings(w);
    test_fences(w);
    test_prime(w);
    test_events(w);
    test_epoll(w);
    test_sharing(w);
    test_threads(w);
  }
  CHECK(A.live == live_at_start);
  printf("LinuxRenderNodeTest: %d checks, %d failed\n", int(g_checks), int(g_failed));
  return g_failed ? 1 : 0;
}
