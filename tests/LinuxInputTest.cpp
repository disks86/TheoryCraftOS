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
// /dev/input for Linux programs (src/Process/LinuxInput.cpp) through the real system calls of a Linux process, over the game controller service
// fed by a fake USB host (an Xbox 360 pad and a DualSense): the directory and stat, the evdev and joystick ioctls, the event streams, blocking and
// non-blocking reads, poll and epoll, rumble through EV_FF, grabs, unplugging, malformed arguments, and several threads at once.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <atomic>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "FileSystem/Ram/RamFileSystem.hpp"
#include "Hardware/GameController/GameControllerService.hpp"
#include "Hardware/GameController/SonyProtocol.hpp"
#include "Hardware/LinuxGamepads.hpp"
#include "Process/AddressSpace.hpp"
#include "Process/Objects.hpp"
#include "Process/Process.hpp"
#include "Process/Syscall.hpp"
#include "Process/UnixSockets.hpp"

using namespace process;
using namespace hardware;

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

// --- the fake USB host (as tests/GameControllerTest.cpp has it, with a lock for the threaded case) -------------------------------------------

struct Config {
  Bytes b;
  void add(std::initializer_list<uint8_t> d) {
    b.push_back(static_cast<uint8_t>(d.size() + 1));
    b.insert(b.end(), d.begin(), d.end());
  }
  Bytes done() {
    Bytes out = {9, 2, static_cast<uint8_t>(b.size() + 9), static_cast<uint8_t>((b.size() + 9) >> 8), 1, 1, 0, 0x80, 50};
    out.insert(out.end(), b.begin(), b.end());
    return out;
  }
};
static Bytes xbox_config(uint8_t subclass, uint8_t protocol) {
  Config c;
  c.add({4, 0, 0, 2, 0xFF, subclass, protocol, 0});
  c.add({5, 0x81, 3, 32, 0, 4});
  c.add({5, 0x02, 3, 32, 0, 8});
  return c.done();
}
static Bytes hid_config(uint16_t report_length, bool out_endpoint) {
  Config c;
  c.add({4, 0, 0, static_cast<uint8_t>(out_endpoint ? 2 : 1), 3, 0, 0, 0});
  c.add({0x21, 0x11, 0x01, 0, 1, 0x22, static_cast<uint8_t>(report_length), static_cast<uint8_t>(report_length >> 8)});
  c.add({5, 0x84, 3, 64, 0, 4});
  if (out_endpoint) c.add({5, 0x03, 3, 64, 0, 4});
  return c.done();
}

struct FakeHost final : IUsbHostController {
  Bytes config, report_descriptor;
  uint16_t vendor = 0, product = 0;
  bool present = true;
  uint32_t generation = 1;
  std::mutex lock;      // (the test's threads push reports while the service reads them)
  std::deque<Bytes> in_queue;
  std::vector<Bytes> sent;

  const char* name() const override { return "fake host"; }
  bool initialize() override { return true; }
  void shutdown() override {}
  UsbControllerKind kind() const override { return UsbControllerKind::Xhci; }
  uint32_t port_count() const override { return 1; }
  bool port_info(uint32_t, UsbPortInfo* out) const override { out->connected = present; out->speed = UsbSpeed::Full; return true; }
  bool device_descriptor(uint32_t, UsbDeviceDescriptor* out) const override {
    if (!present) return false;
    memset(out, 0, sizeof *out);
    out->length = 18;
    out->descriptor_type = 1;
    out->vendor_id = vendor;
    out->product_id = product;
    out->configuration_count = 1;
    return true;
  }
  bool configuration_descriptor(uint32_t, ByteView buffer, uint16_t* length) const override {
    if (!present || config.size() > buffer.size()) return false;
    for (size_t i = 0; i < config.size(); ++i) buffer[i] = config[i];
    *length = static_cast<uint16_t>(config.size());
    return true;
  }
  bool set_configuration(uint32_t, uint8_t) override { return present; }
  bool control_out(uint32_t, uint8_t, uint8_t, uint16_t, uint16_t) override { return present; }
  bool control_in(uint32_t, uint8_t type, uint8_t request, uint16_t value, uint16_t, ByteView buffer) override {
    if (!present || type != 0x81 || request != 6 || value != 0x2200) return false;
    for (size_t i = 0; i < buffer.size() && i < report_descriptor.size(); ++i) buffer[i] = report_descriptor[i];
    return true;
  }
  bool control_out_data(uint32_t, uint8_t, uint8_t, uint16_t, uint16_t, ConstByteView) override { return present; }
  bool start_interrupt_in(uint32_t, uint8_t, uint16_t, uint8_t) override { return present; }
  bool poll_interrupt_in(uint32_t, uint8_t, ByteView buffer, uint16_t* length) override {
    std::lock_guard<std::mutex> hold(lock);
    if (!present || in_queue.empty()) return false;
    const Bytes b = in_queue.front();
    in_queue.pop_front();
    for (size_t i = 0; i < b.size() && i < buffer.size(); ++i) buffer[i] = b[i];
    *length = static_cast<uint16_t>(b.size());
    return true;
  }
  bool start_interrupt_out(uint32_t, uint8_t, uint16_t, uint8_t) override { return present; }
  bool interrupt_out(uint32_t, uint8_t, ConstByteView data) override {
    std::lock_guard<std::mutex> hold(lock);
    if (!present) return false;
    sent.push_back(Bytes(data.data(), data.data() + data.size()));
    return true;
  }
  bool start_bulk_endpoint(uint32_t, uint8_t, uint16_t) override { return false; }
  bool bulk_transfer(uint32_t, uint8_t, ByteView, uint32_t*) override { return false; }
  bool reset_bulk_endpoint(uint32_t, uint8_t) override { return false; }
  uint32_t port_generation(uint32_t) const override { return generation; }

  void plug(uint16_t v, uint16_t p, const Bytes& c) { vendor = v; product = p; config = c; present = true; ++generation; sent.clear(); }
  void unplug() { present = false; ++generation; }
  void push(const Bytes& report) { std::lock_guard<std::mutex> hold(lock); in_queue.push_back(report); }
  Bytes last_sent() { std::lock_guard<std::mutex> hold(lock); return sent.empty() ? Bytes() : sent.back(); }
};

struct FakeBus final : IUsbBus {
  std::vector<IUsbHostController*> hosts;
  const char* name() const override { return "fake usb"; }
  bool initialize() override { return true; }
  void shutdown() override {}
  uint32_t device_count() const override { return static_cast<uint32_t>(hosts.size()); }
  uint32_t controller_count() const override { return static_cast<uint32_t>(hosts.size()); }
  UsbControllerKind controller_kind(uint32_t) const override { return UsbControllerKind::Xhci; }
  uint32_t port_count(uint32_t c) const override { return hosts[c]->port_count(); }
  bool port_info(uint32_t c, uint32_t p, UsbPortInfo* out) const override { return hosts[c]->port_info(p, out); }
  bool device_descriptor(uint32_t c, uint32_t p, UsbDeviceDescriptor* out) const override { return hosts[c]->device_descriptor(p, out); }
  IUsbHostController* host_controller(uint32_t c) override { return c < hosts.size() ? hosts[c] : nullptr; }
};

static Bytes xbox360_report(uint16_t buttons, uint8_t lt, uint8_t rt, int16_t lx, int16_t ly, int16_t rx, int16_t ry) {
  Bytes b = {0x00, 0x14, static_cast<uint8_t>(buttons), static_cast<uint8_t>(buttons >> 8), lt, rt};
  for (int16_t v : {lx, ly, rx, ry}) { b.push_back(static_cast<uint8_t>(v)); b.push_back(static_cast<uint8_t>(static_cast<uint16_t>(v) >> 8)); }
  b.resize(20, 0);
  return b;
}
// A DualSense USB report: sticks centred, `face` in byte 8's high nibble (square 0x10, cross 0x20, circle 0x40, triangle 0x80), hat centred.
static Bytes dualsense_report(uint8_t face, uint8_t lt) {
  Bytes r(64, 0);
  r[0] = 0x01;
  r[1] = 128; r[2] = 128; r[3] = 128; r[4] = 128;
  r[5] = lt;
  r[8] = uint8_t(0x08 | face);
  return r;
}

// --- a Linux process ---------------------------------------------------------------------------------------------------------------------------

// A host whose clock moves a little on every look (so that a wait with a timeout ends) and by the whole time on a sleep; `on_sleep` runs then.
struct Host final : IProcessHost {
  uint64_t now = 1000000000ull;
  std::function<void()> on_sleep;
  bool write_console(uint32_t, ConstByteView) override { return true; }
  bool read_console(ByteView, uint64_t* got) override { *got = 0; return true; }
  uint64_t realtime_ns() override { return 1700000000ull * 1000000000ull + now; }
  uint64_t monotonic_ns() override { return now += 1000; }
  void sleep_ns(uint64_t ns) override { now += ns; if (on_sleep) on_sleep(); }
  void yield() override {}
  void unhandled_syscall(SyscallAbi, uint64_t) override {}
};

struct NoWaitSync final : ISystemSync {
  void lock() override {}
  void unlock() override {}
  bool wait(uint64_t) override { return true; }
  void notify_all() override {}
};

struct Rig {
  AddressSpace space{A};
  Host host;
  Process* process = nullptr;
  uint64_t cursor = 0x10000;
  explicit Rig(filesystem::IFileSystem& fs) {
    CHECK(space.map(0x10000, 0x20000, kProtRead | kProtWrite));
    process = new Process(1, ProcessBinding{SyscallAbi::Linux, UserArch::X86_64, syscall_interface(SyscallAbi::Linux, UserArch::X86_64)}, space, host);
    process->attach_address_space(space, 0x100000);
    process->attach_file_system(A, fs);
  }
  ~Rig() { delete process; }
  void reset_memory() { cursor = 0x10000; }
  uint64_t alloc(uint64_t n) { const uint64_t at = cursor; cursor += (n + 15) & ~uint64_t{15}; CHECK(cursor <= 0x30000); return at; }
  uint64_t str(const std::string& s) {
    const uint64_t at = alloc(s.size() + 1);
    CHECK(space.write_privileged(at, ConstByteView(reinterpret_cast<const uint8_t*>(s.c_str()), s.size() + 1)));
    return at;
  }
  Bytes bytes(uint64_t at, uint64_t n) { Bytes b(n, 0); CHECK(space.read(at, ByteView(b.data(), n))); return b; }
  void put(uint64_t at, const Bytes& b) { CHECK(space.write_privileged(at, ConstByteView(b.data(), b.size()))); }
  uint32_t u32(uint64_t at) { const Bytes b = bytes(at, 4); return b[0] | b[1] << 8 | b[2] << 16 | uint32_t(b[3]) << 24; }
  int64_t s(uint64_t n, uint64_t a0 = 0, uint64_t a1 = 0, uint64_t a2 = 0, uint64_t a3 = 0, uint64_t a4 = 0, uint64_t a5 = 0) {
    SyscallRequest r;
    r.number = n;
    r.args[0] = a0; r.args[1] = a1; r.args[2] = a2; r.args[3] = a3; r.args[4] = a4; r.args[5] = a5;
    return int64_t(process->syscall(r));
  }
  int64_t open(const std::string& path, uint64_t flags) { return s(kOpen, str(path), flags); }
  enum : uint64_t { kRead = 0, kWrite = 1, kOpen = 2, kClose = 3, kStat = 4, kFstat = 5, kPoll = 7, kPread = 17, kIoctl = 16, kAccess = 21, kFcntl = 72,
                    kReadlink = 89, kEpollWait = 232, kEpollCtl = 233, kGetdents64 = 217, kEpollCreate1 = 291 };
};

constexpr uint64_t kONonBlock = 0x800, kORdWr = 2, kOWrOnly = 1, kODirectory = 0x10000;
constexpr int64_t kEnoent = -2, kEintr = -4, kEbadf = -9, kEagain = -11, kEfault = -14, kEbusy = -16, kEnodev = -19, kEnotdir = -20, kEisdir = -21,
                  kEinval = -22, kEnospc = -28, kEspipe = -29, kEnosys = -38;
constexpr uint64_t kBad = 0xDEAD0000;      // nothing is mapped there

constexpr uint32_t ioc(uint32_t dir, uint32_t type, uint32_t nr, uint32_t size) { return (dir << 30) | (size << 16) | (type << 8) | nr; }
constexpr uint32_t R = 2, W = 1;
constexpr uint32_t EVIOCGVERSION = ioc(R, 'E', 0x01, 4), EVIOCGID = ioc(R, 'E', 0x02, 8), EVIOCGEFFECTS = ioc(R, 'E', 0x84, 4),
                   EVIOCSFF = ioc(W, 'E', 0x80, 48), EVIOCRMFF = ioc(W, 'E', 0x81, 4), EVIOCGRAB = ioc(W, 'E', 0x90, 4), EVIOCREVOKE = ioc(W, 'E', 0x91, 4),
                   EVIOCSCLOCKID = ioc(W, 'E', 0xa0, 4);
constexpr uint32_t EVIOCGNAME(uint32_t n) { return ioc(R, 'E', 0x06, n); }
constexpr uint32_t EVIOCGPHYS(uint32_t n) { return ioc(R, 'E', 0x07, n); }
constexpr uint32_t EVIOCGUNIQ(uint32_t n) { return ioc(R, 'E', 0x08, n); }
constexpr uint32_t EVIOCGPROP(uint32_t n) { return ioc(R, 'E', 0x09, n); }
constexpr uint32_t EVIOCGKEY(uint32_t n) { return ioc(R, 'E', 0x18, n); }
constexpr uint32_t EVIOCGBIT(uint32_t ev, uint32_t n) { return ioc(R, 'E', 0x20 + ev, n); }
constexpr uint32_t EVIOCGABS(uint32_t abs, uint32_t n = 24) { return ioc(R, 'E', 0x40 + abs, n); }
constexpr uint32_t JSIOCGVERSION = ioc(R, 'j', 0x01, 4), JSIOCGAXES = ioc(R, 'j', 0x11, 1), JSIOCGBUTTONS = ioc(R, 'j', 0x12, 1);
constexpr uint32_t JSIOCGNAME(uint32_t n) { return ioc(R, 'j', 0x13, n); }
constexpr uint32_t JSIOCGAXMAP = ioc(R, 'j', 0x32, 64), JSIOCGBTNMAP = ioc(R, 'j', 0x34, 1024);

struct InputEvent { uint64_t sec, usec; uint16_t type, code; int32_t value; };
static std::vector<InputEvent> events_of(const Bytes& b) {
  std::vector<InputEvent> out;
  for (size_t at = 0; at + 24 <= b.size(); at += 24) {
    InputEvent e;
    memcpy(&e.sec, &b[at], 8);
    memcpy(&e.usec, &b[at + 8], 8);
    memcpy(&e.type, &b[at + 16], 2);
    memcpy(&e.code, &b[at + 18], 2);
    memcpy(&e.value, &b[at + 20], 4);
    out.push_back(e);
  }
  return out;
}
static bool has_event(const std::vector<InputEvent>& v, uint16_t type, uint16_t code, int32_t value) {
  for (const InputEvent& e : v) if (e.type == type && e.code == code && e.value == value) return true;
  return false;
}
static bool bit(const Bytes& b, uint32_t n) { return n / 8 < b.size() && (b[n / 8] >> (n % 8)) & 1; }

static Bytes input_event(uint16_t type, uint16_t code, int32_t value) {
  Bytes b(24, 0);
  memcpy(&b[16], &type, 2);
  memcpy(&b[18], &code, 2);
  memcpy(&b[20], &value, 4);
  return b;
}
static Bytes ff_rumble(int16_t id, uint16_t length_ms, uint16_t strong, uint16_t weak, uint16_t type = 0x50) {
  Bytes b(48, 0);
  memcpy(&b[0], &type, 2);
  memcpy(&b[2], &id, 2);
  memcpy(&b[10], &length_ms, 2);
  memcpy(&b[16], &strong, 2);
  memcpy(&b[18], &weak, 2);
  return b;
}

// Reads what an evdev descriptor has (non-blocking).
static std::vector<InputEvent> drain(Rig& r, int64_t fd) {
  const uint64_t buffer = r.alloc(24 * 64);
  const int64_t n = r.s(Rig::kRead, uint64_t(fd), buffer, 24 * 64);
  std::vector<InputEvent> v;
  if (n > 0) v = events_of(r.bytes(buffer, uint64_t(n)));
  r.reset_memory();
  return v;
}

struct World {
  filesystem::RamFileSystem fs{A};
  FakeHost xbox, sony;
  FakeBus bus;
  GameControllerService service{&bus, A};
  World() {
    CHECK(fs.create_directory("/Linux") == filesystem::Status::Ok);
    xbox.plug(0x045E, 0x028E, xbox_config(0x5D, 0x01));
    sony.plug(sony::kVendor, sony::kDualSense, hid_config(200, true));
    bus.hosts = {&xbox, &sony};
    CHECK(service.start());
    CHECK(service.count() == 2);
    install_linux_gamepads(&service);
  }
  ~World() {
    install_linux_gamepads(nullptr);
    service.stop();
  }
};

// --- the tests ---------------------------------------------------------------------------------------------------------------------------------

static void test_directory() {
  World w;
  Rig r(w.fs);
  const uint64_t st = r.alloc(144);
  CHECK(r.s(Rig::kStat, r.str("/dev/input"), st) == 0);
  CHECK((r.u32(st + 24) & 0170000) == 0040000);
  const uint32_t mtime = r.u32(st + 88);
  CHECK(r.s(Rig::kStat, r.str("/dev/input/event0"), st) == 0);
  CHECK((r.u32(st + 24) & 0170000) == 0020000 && r.u32(st + 40) == (13u << 8 | 64));
  CHECK(r.s(Rig::kStat, r.str("/dev/input/event1"), st) == 0 && r.u32(st + 40) == (13u << 8 | 65));
  CHECK(r.s(Rig::kStat, r.str("/dev/input/js1"), st) == 0 && r.u32(st + 40) == (13u << 8 | 1));
  CHECK(r.s(Rig::kStat, r.str("/dev/input/event2"), st) == kEnoent);
  CHECK(r.s(Rig::kStat, r.str("/dev/input/event01"), st) == kEnoent);
  CHECK(r.s(Rig::kStat, r.str("/dev/input/mice"), st) == kEnoent);
  CHECK(r.s(Rig::kAccess, r.str("/dev/input/event0"), 4) == 0);
  CHECK(r.s(Rig::kAccess, r.str("/dev/input"), 4) == 0);
  CHECK(r.s(Rig::kAccess, r.str("/dev/input/js7"), 4) == kEnoent);

  // The listing, as SDL's scan without udev reads it.
  const int64_t dir = r.open("/dev/input", kODirectory);
  CHECK(dir >= 0);
  CHECK(r.s(Rig::kFstat, uint64_t(dir), st) == 0 && (r.u32(st + 24) & 0170000) == 0040000);
  const uint64_t buffer = r.alloc(1024);
  const int64_t n = r.s(Rig::kGetdents64, uint64_t(dir), buffer, 1024);
  CHECK(n > 0);
  std::vector<std::string> names;
  bool all_chr = true;
  if (n > 0) {
    const Bytes b = r.bytes(buffer, uint64_t(n));
    for (size_t at = 0; at + 19 < b.size();) {
      const uint16_t length = uint16_t(b[at + 16] | b[at + 17] << 8);
      if (length == 0) break;
      const std::string name(reinterpret_cast<const char*>(&b[at + 19]));
      names.push_back(name);
      if (name != "." && name != ".." && b[at + 18] != 2) all_chr = false;
      at += length;
    }
  }
  CHECK(names == std::vector<std::string>({".", "..", "event0", "event1", "js0", "js1"}));
  CHECK(all_chr);
  CHECK(r.s(Rig::kGetdents64, uint64_t(dir), buffer, 1024) == 0);
  CHECK(r.s(Rig::kClose, uint64_t(dir)) == 0);
  CHECK(r.open("/dev/input", kODirectory | kORdWr) == kEisdir);
  CHECK(r.open("/dev/input/event0", kODirectory) == kEnotdir);
  CHECK(r.open("/dev/input/event3", 0) == kEnoent);

  // The DualSense leaves: its nodes go, and the folder's time moves on.
  w.sony.unplug();
  w.service.step(true);
  r.host.now += 5000000000ull;
  CHECK(r.s(Rig::kStat, r.str("/dev/input"), st) == 0 && r.u32(st + 88) > mtime);
  CHECK(r.s(Rig::kStat, r.str("/dev/input/event1"), st) == kEnoent);
}

static void test_event_ioctls() {
  World w;
  Rig r(w.fs);
  const int64_t fd = r.open("/dev/input/event0", kORdWr | kONonBlock);
  CHECK(fd >= 0);
  const uint64_t f = uint64_t(fd);
  const uint64_t out = r.alloc(1024);
  CHECK(r.s(Rig::kIoctl, f, EVIOCGVERSION, out) == 0 && r.u32(out) == 0x010001);
  CHECK(r.s(Rig::kIoctl, f, EVIOCGID, out) == 0);
  {
    const Bytes id = r.bytes(out, 8);
    CHECK(id[0] == 3 && id[1] == 0 && (id[2] | id[3] << 8) == 0x045E && (id[4] | id[5] << 8) == 0x028E);
  }
  const std::string name = "Xbox 360 Controller";
  CHECK(r.s(Rig::kIoctl, f, EVIOCGNAME(256), out) == int64_t(name.size() + 1));
  CHECK(std::string(reinterpret_cast<const char*>(r.bytes(out, name.size() + 1).data())) == name);
  r.put(out, Bytes(8, 0xEE));
  CHECK(r.s(Rig::kIoctl, f, EVIOCGNAME(4), out) == 4);
  {
    const Bytes b = r.bytes(out, 5);
    CHECK(b[0] == 'X' && b[3] == 'x' && b[4] == 0xEE);      // (cut at the length asked for, without a NUL, as Linux does)
  }
  CHECK(r.s(Rig::kIoctl, f, EVIOCGNAME(0), out) == 0);
  CHECK(r.s(Rig::kIoctl, f, EVIOCGPHYS(64), out) > 0);
  CHECK(r.s(Rig::kIoctl, f, EVIOCGUNIQ(64), out) == kEnoent);
  CHECK(r.s(Rig::kIoctl, f, EVIOCGPROP(8), out) == 8 && r.bytes(out, 8) == Bytes(8, 0));
  // Capabilities.
  CHECK(r.s(Rig::kIoctl, f, EVIOCGBIT(0, 8), out) == 8);
  {
    const Bytes ev = r.bytes(out, 8);
    CHECK(bit(ev, 0) && bit(ev, 1) && bit(ev, 3) && bit(ev, 0x15) && !bit(ev, 2));
  }
  CHECK(r.s(Rig::kIoctl, f, EVIOCGBIT(1, 96), out) == 96);
  {
    const Bytes keys = r.bytes(out, 96);
    for (uint32_t code : {0x130u, 0x131u, 0x133u, 0x134u, 0x136u, 0x137u, 0x13au, 0x13bu, 0x13cu, 0x13du, 0x13eu}) CHECK(bit(keys, code));
    CHECK(!bit(keys, 0x132) && !bit(keys, 0x138) && !bit(keys, 0x110));
  }
  CHECK(r.s(Rig::kIoctl, f, EVIOCGBIT(1, 200), out) == 96);      // (no more than the bitmap)
  CHECK(r.s(Rig::kIoctl, f, EVIOCGBIT(1, 40), out) == 40);       // (no more than asked for)
  CHECK(r.s(Rig::kIoctl, f, EVIOCGBIT(3, 8), out) == 8);
  {
    const Bytes abs = r.bytes(out, 8);
    for (uint32_t code : {0u, 1u, 2u, 3u, 4u, 5u, 0x10u, 0x11u}) CHECK(bit(abs, code));
    CHECK(!bit(abs, 6) && !bit(abs, 0x12));
  }
  CHECK(r.s(Rig::kIoctl, f, EVIOCGBIT(0x15, 16), out) == 16 && bit(r.bytes(out, 16), 0x50) && bit(r.bytes(out, 16), 0x60));
  CHECK(r.s(Rig::kIoctl, f, EVIOCGBIT(2, 8), out) == 8 && r.bytes(out, 8) == Bytes(8, 0));      // EV_REL: none
  CHECK(r.s(Rig::kIoctl, f, EVIOCGBIT(0x14, 8), out) == kEinval);                               // EV_REP: not a type evdev answers
  // Axes.
  auto absinfo = [&](uint32_t axis) {
    std::vector<int32_t> v(6, 0);
    CHECK(r.s(Rig::kIoctl, f, EVIOCGABS(axis), out) == 0);
    const Bytes b = r.bytes(out, 24);
    memcpy(v.data(), b.data(), 24);
    return v;
  };
  CHECK(absinfo(0) == std::vector<int32_t>({0, -32768, 32767, 16, 128, 0}));
  CHECK(absinfo(1) == std::vector<int32_t>({-1, -32768, 32767, 16, 128, 0}));      // (centred: ~0)
  CHECK(absinfo(2) == std::vector<int32_t>({0, 0, 255, 0, 0, 0}));
  CHECK(absinfo(0x10) == std::vector<int32_t>({0, -1, 1, 0, 0, 0}));
  CHECK(absinfo(0x20) == std::vector<int32_t>({0, 0, 0, 0, 0, 0}));                // (an axis the pad does not have: zeros, as Linux)
  r.put(out, Bytes(24, 0xEE));
  CHECK(r.s(Rig::kIoctl, f, EVIOCGABS(2, 20), out) == 0 && r.bytes(out, 24)[20] == 0xEE);   // (the older 20-byte absinfo)
  CHECK(r.s(Rig::kIoctl, f, EVIOCGEFFECTS, out) == 0 && r.u32(out) == 16);
  CHECK(r.s(Rig::kIoctl, f, EVIOCGKEY(96), out) == 96 && r.bytes(out, 96) == Bytes(96, 0));

  // Malformed arguments.
  CHECK(r.s(Rig::kIoctl, f, EVIOCGVERSION, kBad) == kEfault);
  CHECK(r.s(Rig::kIoctl, f, EVIOCGNAME(64), kBad) == kEfault);
  CHECK(r.s(Rig::kIoctl, f, EVIOCGBIT(1, 96), kBad) == kEfault);
  CHECK(r.s(Rig::kIoctl, f, EVIOCGABS(0), kBad) == kEfault);
  CHECK(r.s(Rig::kIoctl, f, ioc(R, 'E', 0x01, 2), out) == kEinval);      // EVIOCGVERSION of the wrong size
  CHECK(r.s(Rig::kIoctl, f, ioc(R, 'E', 0x02, 4), out) == kEinval);      // EVIOCGID of the wrong size
  CHECK(r.s(Rig::kIoctl, f, ioc(W, 'E', 0x80, 44), out) == kEinval);     // EVIOCSFF with the 32-bit size
  CHECK(r.s(Rig::kIoctl, f, ioc(R, 'X', 0x01, 4), out) == kEinval);
  CHECK(r.s(Rig::kIoctl, f, 0x5401, out) == kEinval);                    // TCGETS (isatty)
  CHECK(r.s(Rig::kIoctl, f, EVIOCSFF, kBad) == kEfault);
  CHECK(r.s(Rig::kIoctl, f, EVIOCSCLOCKID, kBad) == kEfault);
  r.put(out, Bytes{5, 0, 0, 0});
  CHECK(r.s(Rig::kIoctl, f, EVIOCSCLOCKID, out) == kEinval);
  r.put(out, Bytes{1, 0, 0, 0});
  CHECK(r.s(Rig::kIoctl, f, EVIOCSCLOCKID, out) == 0);
  CHECK(r.s(Rig::kIoctl, f, EVIOCRMFF, 99) == kEinval);
  CHECK(r.s(Rig::kIoctl, f, EVIOCRMFF, uint64_t(-1)) == kEinval);
  r.put(out, ff_rumble(-1, 0, 1, 1, 0x51));      // FF_PERIODIC: not offered
  CHECK(r.s(Rig::kIoctl, f, EVIOCSFF, out) == kEinval);
  r.put(out, ff_rumble(7, 0, 1, 1));             // an id that was never handed out
  CHECK(r.s(Rig::kIoctl, f, EVIOCSFF, out) == kEinval);
  CHECK(r.s(Rig::kIoctl, f, EVIOCREVOKE, 1) == kEinval);

  // The descriptor itself.
  const uint64_t st = r.alloc(144);
  CHECK(r.s(Rig::kFstat, f, st) == 0 && (r.u32(st + 24) & 0170000) == 0020000 && r.u32(st + 40) == (13u << 8 | 64));
  CHECK(r.s(Rig::kFcntl, f, 3) == int64_t(kORdWr | kONonBlock));
  CHECK(r.s(Rig::kFcntl, f, 4, 0) == 0 && r.s(Rig::kFcntl, f, 3) == int64_t(kORdWr));
  CHECK(r.s(Rig::kFcntl, f, 4, kONonBlock) == 0);
  const uint64_t link = r.alloc(64);
  const int64_t length = r.s(Rig::kReadlink, r.str("/proc/self/fd/" + std::to_string(fd)), link, 64);
  CHECK(length == 17 && std::string(reinterpret_cast<const char*>(r.bytes(link, 17).data()), 17) == "/dev/input/event0");
  CHECK(r.s(Rig::kPread, f, out, 24, 0) == kEspipe);
  CHECK(r.s(Rig::kClose, f) == 0);
  CHECK(r.s(Rig::kIoctl, f, EVIOCGVERSION, out) == kEbadf);
}

static void test_event_stream() {
  World w;
  Rig r(w.fs);
  const int64_t fd = r.open("/dev/input/event0", kONonBlock);
  CHECK(fd >= 0);
  const uint64_t f = uint64_t(fd);
  const uint64_t buffer = 0x20000;
  CHECK(r.s(Rig::kRead, f, buffer, 240) == kEagain);
  CHECK(r.s(Rig::kRead, f, buffer, 10) == kEinval);       // (smaller than one event)
  CHECK(r.s(Rig::kRead, f, buffer, 0) == kEagain);
  CHECK(r.s(Rig::kRead, f, kBad, 240) == kEagain);        // (nothing to copy yet)

  // A pressed, X (the west button: BTN_X 0x133 in xpad's layout), the left stick up, the right trigger, the d-pad up-left.
  w.xbox.push(xbox360_report(0x1000 | 0x4000 | 0x0001 | 0x0004, 0, 200, 0, 32767, 0, 0));
  w.service.step(false);
  std::vector<InputEvent> v = drain(r, fd);
  CHECK(v.size() == 7);
  CHECK(has_event(v, 1, 0x130, 1) && has_event(v, 1, 0x133, 1) && has_event(v, 3, 1, -32768) && has_event(v, 3, 5, 200));
  CHECK(has_event(v, 3, 0x10, -1) && has_event(v, 3, 0x11, -1));
  CHECK(!v.empty() && v.back().type == 0 && v.back().code == 0 && v.back().value == 0);      // SYN_REPORT ends the batch
  CHECK(!v.empty() && v[0].sec == 1700000001ull);      // CLOCK_REALTIME by default
  CHECK(drain(r, fd).empty());

  // Y (north: 0x134), released A, the stick down; read one event at a time.
  w.xbox.push(xbox360_report(0x8000 | 0x4000, 0, 200, 0, -32768, 0, 0));
  w.service.step(false);
  std::vector<InputEvent> one;
  for (int i = 0; i < 10; ++i) {
    const int64_t n = r.s(Rig::kRead, f, buffer, 30);      // (room for one whole event only)
    if (n < 0) break;
    CHECK(n == 24);
    const std::vector<InputEvent> e = events_of(r.bytes(buffer, 24));
    one.insert(one.end(), e.begin(), e.end());
  }
  CHECK(one.size() == 6);
  CHECK(has_event(one, 1, 0x130, 0) && has_event(one, 1, 0x134, 1) && has_event(one, 3, 1, 32767) && has_event(one, 3, 0x10, 0) && has_event(one, 3, 0x11, 0));
  // The state the program can ask for matches.
  CHECK(r.s(Rig::kIoctl, f, EVIOCGKEY(96), buffer) == 96);
  {
    const Bytes keys = r.bytes(buffer, 96);
    CHECK(bit(keys, 0x133) && bit(keys, 0x134) && !bit(keys, 0x130));
  }
  // EVIOCGKEY drops button events still queued (the program has the state), not the axes.
  w.xbox.push(xbox360_report(0x0100, 0, 200, 1000, -32768, 0, 0));
  w.service.step(false);
  CHECK(r.s(Rig::kIoctl, f, EVIOCGKEY(96), buffer) == 96 && bit(r.bytes(buffer, 96), 0x136));
  v = drain(r, fd);
  CHECK(v.size() == 2 && has_event(v, 3, 0, 1000) && v.back().type == 0);
  // A bad buffer: EFAULT.
  w.xbox.push(xbox360_report(0, 0, 0, 0, 0, 0, 0));
  w.service.step(false);
  CHECK(r.s(Rig::kRead, f, kBad, 240) == kEfault);
  // CLOCK_MONOTONIC.
  r.put(buffer, Bytes{1, 0, 0, 0});
  CHECK(r.s(Rig::kIoctl, f, EVIOCSCLOCKID, buffer) == 0);
  w.xbox.push(xbox360_report(0x0010, 0, 0, 0, 0, 0, 0));
  w.service.step(false);
  v = drain(r, fd);
  CHECK(v.size() == 2 && has_event(v, 1, 0x13b, 1) && v[0].sec < 100);
  r.s(Rig::kClose, f);

  // The DualSense (slot 1): hid-playstation's layout, by position, with the triggers as buttons too.
  const int64_t ds = r.open("/dev/input/event1", kONonBlock);
  CHECK(ds >= 0);
  const uint64_t out = r.alloc(128);
  CHECK(r.s(Rig::kIoctl, uint64_t(ds), EVIOCGNAME(128), out) > 1);
  CHECK(r.s(Rig::kIoctl, uint64_t(ds), EVIOCGBIT(1, 96), out) == 96 && bit(r.bytes(out, 96), 0x138) && bit(r.bytes(out, 96), 0x139));
  r.reset_memory();
  w.sony.push(dualsense_report(0x10 | 0x80, 40));      // square (west) and triangle (north), L2 pressed a little
  w.service.step(false);
  v = drain(r, ds);
  CHECK(has_event(v, 1, 0x134, 1) && has_event(v, 1, 0x133, 1) && has_event(v, 1, 0x138, 1) && has_event(v, 3, 2, 40));
  r.s(Rig::kClose, uint64_t(ds));
}

static void test_blocking_and_poll() {
  World w;
  static NoWaitSync sync;
  UnixSocketManager sockets(sync, A);
  set_system_unix_sockets(&sockets);
  {
    Rig r(w.fs);
    const int64_t fd = r.open("/dev/input/event0", 0);      // blocking
    CHECK(fd >= 0);
    const uint64_t f = uint64_t(fd);
    // poll: nothing yet (timeout 0, then 20 ms), then readable after a change.
    const uint64_t pfd = r.alloc(8);
    r.put(pfd, Bytes{uint8_t(fd), 0, 0, 0, 0x01, 0, 0, 0});
    CHECK(r.s(Rig::kPoll, pfd, 1, 0) == 0);
    CHECK(r.s(Rig::kPoll, pfd, 1, 20) == 0);
    r.put(pfd, Bytes{uint8_t(fd), 0, 0, 0, 0x05, 0, 0, 0});
    CHECK(r.s(Rig::kPoll, pfd, 1, 0) == 1 && r.bytes(pfd + 6, 2)[0] == 0x04);      // (always writable)
    w.xbox.push(xbox360_report(0x2000, 0, 0, 0, 0, 0, 0));
    w.service.step(false);
    r.put(pfd, Bytes{uint8_t(fd), 0, 0, 0, 0x01, 0, 0, 0});
    CHECK(r.s(Rig::kPoll, pfd, 1, 0) == 1 && r.bytes(pfd + 6, 2)[0] == 0x01);
    // epoll the same.
    const int64_t ep = r.s(Rig::kEpollCreate1, 0);
    CHECK(ep >= 0);
    const uint64_t ev = r.alloc(12);
    r.put(ev, Bytes{0x01, 0, 0, 0, 7, 0, 0, 0, 0, 0, 0, 0});
    CHECK(r.s(Rig::kEpollCtl, uint64_t(ep), 1, f, ev) == 0);
    const uint64_t got = r.alloc(12 * 4);
    CHECK(r.s(Rig::kEpollWait, uint64_t(ep), got, 4, 0) == 1 && r.bytes(got, 1)[0] == 0x01 && r.bytes(got + 4, 1)[0] == 7);
    std::vector<InputEvent> v = drain(r, fd);
    CHECK(v.size() == 2 && has_event(v, 1, 0x131, 1));
    CHECK(r.s(Rig::kEpollWait, uint64_t(ep), got, 4, 0) == 0);
    // A blocking read waits until the pad says something (here: the pad moves while the reader sleeps).
    int sleeps = 0;
    r.host.on_sleep = [&] {
      if (++sleeps == 3) {
        w.xbox.push(xbox360_report(0x0000, 0, 0, 0, 0, 0, 0));
        w.service.step(false);
      }
    };
    const uint64_t buffer = r.alloc(240);
    CHECK(r.s(Rig::kRead, f, buffer, 240) == 48 && sleeps == 3);
    // A blocking poll wakes up for it too.
    sleeps = 0;
    r.put(pfd, Bytes{uint8_t(fd), 0, 0, 0, 0x01, 0, 0, 0});
    std::thread mover([&] {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
      w.xbox.push(xbox360_report(0x0020, 0, 0, 0, 0, 0, 0));
      w.service.step(false);
    });
    CHECK(r.s(Rig::kPoll, pfd, 1, uint64_t(-1)) == 1);
    mover.join();
    r.host.on_sleep = nullptr;
    CHECK(r.s(Rig::kClose, uint64_t(ep)) == 0);
    CHECK(r.s(Rig::kClose, f) == 0);
  }
  set_system_unix_sockets(nullptr);
}

static void test_rumble() {
  World w;
  Rig r(w.fs);
  const int64_t fd = r.open("/dev/input/event0", kORdWr | kONonBlock);
  CHECK(fd >= 0);
  const uint64_t f = uint64_t(fd);
  const uint64_t effect = r.alloc(48), ev = r.alloc(24 * 2);
  r.put(effect, ff_rumble(-1, 0, 0x8000, 0x4000));
  CHECK(r.s(Rig::kIoctl, f, EVIOCSFF, effect) == 0);
  CHECK(r.bytes(effect + 2, 2) == Bytes({0, 0}));      // (id 0, written back)
  const size_t before = w.xbox.sent.size();
  r.put(ev, input_event(0x15, 0, 1));
  CHECK(r.s(Rig::kWrite, f, ev, 24) == 24);
  Bytes m = w.xbox.last_sent();
  CHECK(w.xbox.sent.size() == before + 1 && m.size() >= 5 && m[1] == 0x08 && m[3] == 0x80 && m[4] == 0x40);
  // Half the gain: half the strength.
  r.put(ev, input_event(0x15, 0x60, 0x8000));
  CHECK(r.s(Rig::kWrite, f, ev, 24) == 24);
  m = w.xbox.last_sent();
  CHECK(m.size() >= 5 && m[3] == 0x40 && m[4] == 0x20);
  r.put(ev, input_event(0x15, 0x60, 0xFFFF));
  CHECK(r.s(Rig::kWrite, f, ev, 24) == 24);
  // Changed while playing: at once.
  r.put(effect, ff_rumble(0, 0, 0xFF00, 0));
  CHECK(r.s(Rig::kIoctl, f, EVIOCSFF, effect) == 0);
  m = w.xbox.last_sent();
  CHECK(m.size() >= 5 && m[3] == 0xFF && m[4] == 0);
  // Stopped.
  r.put(ev, input_event(0x15, 0, 0));
  CHECK(r.s(Rig::kWrite, f, ev, 24) == 24);
  m = w.xbox.last_sent();
  CHECK(m.size() >= 5 && m[3] == 0 && m[4] == 0);
  // A second effect that lasts 10 ms: it stops by itself (seen on the next look at the descriptor).
  r.put(effect, ff_rumble(-1, 10, 0, 0x8000));
  CHECK(r.s(Rig::kIoctl, f, EVIOCSFF, effect) == 0 && r.bytes(effect + 2, 2) == Bytes({1, 0}));
  r.put(ev, input_event(0x15, 1, 1));
  CHECK(r.s(Rig::kWrite, f, ev, 24) == 24);
  m = w.xbox.last_sent();
  CHECK(m.size() >= 5 && m[4] == 0x80);
  r.host.now += 20000000;
  CHECK(r.s(Rig::kRead, f, 0x20000, 240) == kEagain);
  m = w.xbox.last_sent();
  CHECK(m.size() >= 5 && m[4] == 0);
  // Two events in one write; then removing a playing effect stops it.
  {
    Bytes two = input_event(0x15, 0, 1);
    const Bytes second = input_event(0x00, 0, 0);
    two.insert(two.end(), second.begin(), second.end());
    r.put(ev, two);
    CHECK(r.s(Rig::kWrite, f, ev, 48) == 48);
    CHECK(w.xbox.last_sent()[3] == 0xFF);
    CHECK(r.s(Rig::kIoctl, f, EVIOCRMFF, 0) == 0);
    CHECK(w.xbox.last_sent()[3] == 0);
    CHECK(r.s(Rig::kIoctl, f, EVIOCRMFF, 0) == kEinval);
  }
  // Every effect slot taken: ENOSPC.
  int made = 0;
  for (int i = 0; i < 20; ++i) {
    r.put(effect, ff_rumble(-1, 0, 1, 1));
    const int64_t s = r.s(Rig::kIoctl, f, EVIOCSFF, effect);
    if (s == 0) ++made;
    else CHECK(s == kEnospc);
  }
  CHECK(made == 15);
  // Malformed writes.
  CHECK(r.s(Rig::kWrite, f, ev, 10) == kEinval);
  CHECK(r.s(Rig::kWrite, f, kBad, 24) == kEfault);
  CHECK(r.s(Rig::kWrite, f, ev, 0) == 0);
  // Closing a descriptor whose effect plays stops the motors.
  r.put(ev, input_event(0x15, 1, 1));
  CHECK(r.s(Rig::kWrite, f, ev, 24) == 24);
  CHECK(w.xbox.last_sent()[4] == 0x80);
  CHECK(r.s(Rig::kClose, f) == 0);
  CHECK(w.xbox.last_sent()[4] == 0);
  // Read-only: no writing.
  const int64_t ro = r.open("/dev/input/event0", 0);
  CHECK(r.s(Rig::kWrite, uint64_t(ro), ev, 24) == kEbadf);
  r.s(Rig::kClose, uint64_t(ro));
}

static void test_joystick() {
  World w;
  Rig r(w.fs);
  const int64_t fd = r.open("/dev/input/js0", kONonBlock);
  CHECK(fd >= 0);
  const uint64_t f = uint64_t(fd);
  const uint64_t out = r.alloc(1024);
  CHECK(r.s(Rig::kIoctl, f, JSIOCGVERSION, out) == 0 && r.u32(out) == 0x020100);
  CHECK(r.s(Rig::kIoctl, f, JSIOCGAXES, out) == 0 && r.bytes(out, 1)[0] == 8);
  CHECK(r.s(Rig::kIoctl, f, JSIOCGBUTTONS, out) == 0 && r.bytes(out, 1)[0] == 11);
  CHECK(r.s(Rig::kIoctl, f, JSIOCGNAME(128), out) == 20);
  CHECK(r.s(Rig::kIoctl, f, JSIOCGAXMAP, out) == 64 && r.bytes(out, 8) == Bytes({0, 1, 2, 3, 4, 5, 0x10, 0x11}));
  CHECK(r.s(Rig::kIoctl, f, JSIOCGBTNMAP, out) == 1024);
  {
    const Bytes b = r.bytes(out, 4);
    CHECK((b[0] | b[1] << 8) == 0x130 && (b[2] | b[3] << 8) == 0x131);
  }
  CHECK(r.s(Rig::kIoctl, f, JSIOCGVERSION, kBad) == kEfault);
  CHECK(r.s(Rig::kIoctl, f, ioc(R, 'j', 0x11, 4), out) == kEinval);
  CHECK(r.s(Rig::kIoctl, f, EVIOCGVERSION, out) == kEinval);
  // The burst after open: every button, then every axis, marked JS_EVENT_INIT.
  const uint64_t buffer = 0x20000;
  const int64_t n = r.s(Rig::kRead, f, buffer, 8 * 64);
  CHECK(n == 8 * 19);
  if (n == 8 * 19) {
    const Bytes b = r.bytes(buffer, uint64_t(n));
    CHECK(b[6] == 0x81 && b[7] == 0 && b[8 * 10 + 6] == 0x81 && b[8 * 10 + 7] == 10 && b[8 * 11 + 6] == 0x82 && b[8 * 11 + 7] == 0);
    CHECK(int16_t(b[8 * 11 + 4] | b[8 * 11 + 5] << 8) == 0);
    CHECK(int16_t(b[8 * 12 + 4] | b[8 * 12 + 5] << 8) == 0);      // (Y centred: ~0 = -1, inside the flat part)
    CHECK(int16_t(b[8 * 13 + 4] | b[8 * 13 + 5] << 8) == -32767);  // (a trigger at rest is the bottom of the range)
  }
  CHECK(r.s(Rig::kRead, f, buffer, 64) == kEagain);
  CHECK(r.s(Rig::kRead, f, buffer, 4) == kEinval);
  // A and the left stick right, the right trigger full.
  w.xbox.push(xbox360_report(0x1000, 0, 255, 32767, 0, 0, 0));
  w.service.step(false);
  const int64_t m = r.s(Rig::kRead, f, buffer, 64);
  CHECK(m == 24);
  if (m == 24) {
    const Bytes b = r.bytes(buffer, 24);
    CHECK(b[6] == 0x01 && b[7] == 0 && b[4] == 1);
    CHECK(b[14] == 0x02 && b[15] == 0 && int16_t(b[12] | b[13] << 8) == 32767);
    CHECK(b[22] == 0x02 && b[23] == 5 && int16_t(b[20] | b[21] << 8) == 32767);
  }
  CHECK(r.s(Rig::kWrite, f, buffer, 8) == kEbadf);
  const int64_t rw = r.open("/dev/input/js0", kORdWr | kONonBlock);
  CHECK(r.s(Rig::kWrite, uint64_t(rw), buffer, 8) == kEinval);
  r.s(Rig::kClose, uint64_t(rw));
  // Blocking open: the burst is there at once.
  const int64_t b = r.open("/dev/input/js1", 0);
  CHECK(b >= 0 && r.s(Rig::kRead, uint64_t(b), buffer, 8) == 8);
  r.s(Rig::kClose, uint64_t(b));
  r.s(Rig::kClose, f);
}

static void test_grab_and_revoke() {
  World w;
  Rig r(w.fs);
  const int64_t a = r.open("/dev/input/event0", kONonBlock), b = r.open("/dev/input/event0", kONonBlock);
  CHECK(a >= 0 && b >= 0);
  CHECK(r.s(Rig::kIoctl, uint64_t(a), EVIOCGRAB, 1) == 0);
  CHECK(r.s(Rig::kIoctl, uint64_t(b), EVIOCGRAB, 1) == kEbusy);
  CHECK(r.s(Rig::kIoctl, uint64_t(b), EVIOCGRAB, 0) == kEinval);
  w.xbox.push(xbox360_report(0x1000, 0, 0, 0, 0, 0, 0));
  w.service.step(false);
  CHECK(drain(r, b).empty());
  CHECK(drain(r, a).size() == 2);
  CHECK(r.s(Rig::kIoctl, uint64_t(a), EVIOCGRAB, 0) == 0);
  w.xbox.push(xbox360_report(0x0000, 0, 0, 0, 0, 0, 0));
  w.service.step(false);
  CHECK(drain(r, b).size() == 2);      // (the press it did not see is not replayed: only the release)
  CHECK(drain(r, a).size() == 2);
  // A grab goes with the descriptor.
  CHECK(r.s(Rig::kIoctl, uint64_t(a), EVIOCGRAB, 1) == 0);
  r.s(Rig::kClose, uint64_t(a));
  CHECK(r.s(Rig::kIoctl, uint64_t(b), EVIOCGRAB, 1) == 0);
  // Revoked: done with the device.
  CHECK(r.s(Rig::kIoctl, uint64_t(b), EVIOCREVOKE, 0) == 0);
  CHECK(r.s(Rig::kRead, uint64_t(b), 0x20000, 240) == kEnodev);
  CHECK(r.s(Rig::kIoctl, uint64_t(b), EVIOCGVERSION, 0x20000) == kEnodev);
  const int64_t c = r.open("/dev/input/event0", kONonBlock);
  CHECK(r.s(Rig::kIoctl, uint64_t(c), EVIOCGRAB, 1) == 0);      // (the revoked one let go of its grab)
  r.s(Rig::kClose, uint64_t(b));
  r.s(Rig::kClose, uint64_t(c));
}

static void test_unplug() {
  World w;
  static NoWaitSync sync;
  UnixSocketManager sockets(sync, A);
  set_system_unix_sockets(&sockets);
  {
    Rig r(w.fs);
    const int64_t ev = r.open("/dev/input/event0", kONonBlock), js = r.open("/dev/input/js0", kONonBlock), blocking = r.open("/dev/input/event0", 0);
    CHECK(ev >= 0 && js >= 0 && blocking >= 0);
    w.xbox.unplug();
    w.service.step(true);
    CHECK(r.s(Rig::kRead, uint64_t(ev), 0x20000, 240) == kEnodev);
    CHECK(r.s(Rig::kRead, uint64_t(js), 0x20000, 240) == kEnodev);
    CHECK(r.s(Rig::kRead, uint64_t(blocking), 0x20000, 240) == kEnodev);      // (a blocking read does not wait for a pad that is gone)
    CHECK(r.s(Rig::kIoctl, uint64_t(ev), EVIOCGVERSION, 0x20000) == kEnodev);
    const uint64_t pfd = r.alloc(8);
    r.put(pfd, Bytes{uint8_t(ev), 0, 0, 0, 0x01, 0, 0, 0});
    CHECK(r.s(Rig::kPoll, pfd, 1, 0) == 1 && (r.bytes(pfd + 6, 1)[0] & 0x18) == 0x18);      // POLLHUP | POLLERR
    CHECK(r.open("/dev/input/event0", 0) == kEnoent);
    // Another pad in the same slot is a new device: the old descriptors stay dead, a new open sees it.
    w.xbox.plug(0x045E, 0x0B12, xbox_config(0x47, 0xD0));
    w.service.step(true);
    CHECK(r.s(Rig::kRead, uint64_t(ev), 0x20000, 240) == kEnodev);
    const int64_t fresh = r.open("/dev/input/event0", kONonBlock);
    CHECK(fresh >= 0);
    const uint64_t out = r.alloc(8);
    CHECK(r.s(Rig::kIoctl, uint64_t(fresh), EVIOCGID, out) == 0 && r.bytes(out, 8)[4] == 0x12);
    for (int64_t fd : {ev, js, blocking, fresh}) CHECK(r.s(Rig::kClose, uint64_t(fd)) == 0);
  }
  set_system_unix_sockets(nullptr);
}

static void test_no_provider() {
  filesystem::RamFileSystem fs{A};
  CHECK(fs.create_directory("/Linux") == filesystem::Status::Ok);
  Rig r(fs);
  const uint64_t st = r.alloc(144);
  CHECK(r.s(Rig::kStat, r.str("/dev/input/event0"), st) == kEnoent);
  const int64_t dir = r.open("/dev/input", kODirectory);
  CHECK(dir >= 0);
  const uint64_t buffer = r.alloc(512);
  CHECK(r.s(Rig::kGetdents64, uint64_t(dir), buffer, 512) == 48);      // ("." and ".." only)
  r.s(Rig::kClose, uint64_t(dir));
}

// Several threads at once: one plays the service's thread (reports in, a round of polling), three processes read the pad (blocking evdev, polling
// joystick, EVIOCGKEY/EVIOCGABS) and one rumbles it. Every reader must see well-formed batches, and nothing may race (ASan/UBSan here).
static void test_threads() {
  World w;
  std::atomic<bool> done{false};
  std::atomic<int> batches{0}, js_events{0}, rumbles{0};
  std::thread service([&] {
    for (int i = 0; i < 400; ++i) {
      w.xbox.push(xbox360_report(uint16_t((i & 1) ? 0x1000 : 0x2000), uint8_t(i), 0, int16_t(i * 50), 0, 0, 0));
      w.service.step(i % 50 == 0);
      std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
    done = true;
  });
  std::thread evdev([&] {
    Rig r(w.fs);
    const int64_t fd = r.open("/dev/input/event0", kONonBlock);
    CHECK(fd >= 0);
    while (!done) {
      for (const InputEvent& e : drain(r, fd)) {
        CHECK(e.type == 0 || e.type == 1 || e.type == 3);
        if (e.type == 0) ++batches;
      }
      std::this_thread::yield();
    }
    r.s(Rig::kClose, uint64_t(fd));
  });
  std::thread joystick([&] {
    Rig r(w.fs);
    const int64_t fd = r.open("/dev/input/js0", kONonBlock);
    CHECK(fd >= 0);
    const uint64_t pfd = r.alloc(8), buffer = r.alloc(8 * 64);
    while (!done) {
      r.put(pfd, Bytes{uint8_t(fd), 0, 0, 0, 0x01, 0, 0, 0});
      if (r.s(Rig::kPoll, pfd, 1, 0) == 1) {
        const int64_t n = r.s(Rig::kRead, uint64_t(fd), buffer, 8 * 64);
        CHECK(n > 0 && n % 8 == 0);
        if (n > 0) js_events += int(n / 8);
      }
      std::this_thread::yield();
    }
    r.s(Rig::kClose, uint64_t(fd));
  });
  std::thread state([&] {
    Rig r(w.fs);
    const int64_t fd = r.open("/dev/input/event0", kONonBlock);
    const uint64_t out = r.alloc(96);
    while (!done) {
      CHECK(r.s(Rig::kIoctl, uint64_t(fd), EVIOCGKEY(96), out) == 96);
      CHECK(r.s(Rig::kIoctl, uint64_t(fd), EVIOCGABS(0), out) == 0);
      std::this_thread::yield();
    }
    r.s(Rig::kClose, uint64_t(fd));
  });
  std::thread rumble([&] {
    Rig r(w.fs);
    const int64_t fd = r.open("/dev/input/event0", kORdWr | kONonBlock);
    const uint64_t effect = r.alloc(48), ev = r.alloc(24);
    r.put(effect, ff_rumble(-1, 0, 0x4000, 0x4000));
    CHECK(r.s(Rig::kIoctl, uint64_t(fd), EVIOCSFF, effect) == 0);
    for (int i = 0; !done; ++i) {
      r.put(ev, input_event(0x15, 0, i & 1));
      CHECK(r.s(Rig::kWrite, uint64_t(fd), ev, 24) == 24);
      ++rumbles;
      std::this_thread::yield();
    }
    r.s(Rig::kClose, uint64_t(fd));
  });
  service.join();
  evdev.join();
  joystick.join();
  state.join();
  rumble.join();
  CHECK(batches > 0 && js_events > 19 && rumbles > 0);
}

int main() {
  test_directory();
  test_event_ioctls();
  test_event_stream();
  test_blocking_and_poll();
  test_rumble();
  test_joystick();
  test_grab_and_revoke();
  test_unplug();
  test_no_provider();
  test_threads();
  CHECK(A.live == 0);
  printf("LinuxInputTest: %d checks, %d failed\n", int(g_checks), int(g_failed));
  return g_failed ? 1 : 0;
}
