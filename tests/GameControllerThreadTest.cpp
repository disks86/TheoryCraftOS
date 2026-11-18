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

// The game controller service with real threads, run under ThreadSanitizer: the service's own thread polls the pads and follows hot-plug, one
// thread plugs Xbox and PlayStation pads in and out of four ports and makes them send reports, and two threads read and drive the slots the way
// XInput programs do (snapshots, rumble, NtTcosGamepad's words). No race may be reported, no call may see a pad that is half gone, and no
// memory may be left.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <deque>
#include <mutex>
#include <vector>
#include "Hardware/GameController/GameControllerService.hpp"
#include "Hardware/GameController/SonyProtocol.hpp"
#include "Hardware/GameController/WindowsGamepadService.hpp"
#include "Kernel/Thread.hpp"

using namespace hardware;
using namespace hardware::gamepad;

struct TestAllocator final : allocators::IAllocator {
  int live = 0;
  void* allocate(uint64_t size) override { if (!size) return nullptr; __atomic_add_fetch(&live, 1, __ATOMIC_RELAXED); return aligned_alloc(64, (size + 63) / 64 * 64); }
  void* allocate_aligned(uint64_t size, uint64_t alignment) override {
    if (!size) return nullptr;
    __atomic_add_fetch(&live, 1, __ATOMIC_RELAXED);
    if (alignment < 64) alignment = 64;
    return aligned_alloc(alignment, (size + alignment - 1) / alignment * alignment);
  }
  void deallocate(void* p) override { if (p) { __atomic_sub_fetch(&live, 1, __ATOMIC_RELAXED); free(p); } }
};

static int g_failed = 0;
static void fail(int line, const char* what) { printf("FAILED line %d: %s\n", line, what); __atomic_add_fetch(&g_failed, 1, __ATOMIC_RELAXED); }
#define CHECK(...) do { if (!(__VA_ARGS__)) fail(__LINE__, #__VA_ARGS__); } while (0)

static void nap(long us) {
  timespec ts{0, us * 1000};
  nanosleep(&ts, nullptr);
}

typedef std::vector<uint8_t> Bytes;

static Bytes config_for(bool xbox) {
  if (xbox)
    return {9, 2, 32, 0, 1, 1, 0, 0x80, 50,
            9, 4, 0, 0, 2, 0xFF, 0x5D, 0x01, 0,
            7, 5, 0x81, 3, 32, 0, 4,
            7, 5, 0x02, 3, 32, 0, 8};
  return {9, 2, 41, 0, 1, 1, 0, 0x80, 50,
          9, 4, 0, 0, 2, 3, 0, 0, 0,
          9, 0x21, 0x11, 0x01, 0, 1, 0x22, 200, 0,
          7, 5, 0x84, 3, 64, 0, 4,
          7, 5, 0x03, 3, 64, 0, 4};
}

// Four ports, each with a pad or none; every call takes the host's lock, as a real controller serializes its transfers.
struct ThreadedHost final : IUsbHostController {
  static constexpr uint32_t kPorts = 4;
  struct Port {
    bool present = false, xbox = false;
    uint32_t generation = 1;
    std::deque<Bytes> in;
    uint32_t sent = 0;
  };
  mutable std::mutex mutex;
  Port ports[kPorts];

  const char* name() const override { return "threaded host"; }
  bool initialize() override { return true; }
  void shutdown() override {}
  UsbControllerKind kind() const override { return UsbControllerKind::Xhci; }
  uint32_t port_count() const override { return kPorts; }
  bool port_info(uint32_t p, UsbPortInfo* out) const override {
    std::lock_guard<std::mutex> g(mutex);
    out->connected = ports[p].present;
    out->speed = UsbSpeed::Full;
    return true;
  }
  bool device_descriptor(uint32_t p, UsbDeviceDescriptor* out) const override {
    std::lock_guard<std::mutex> g(mutex);
    if (!ports[p].present) return false;
    memset(out, 0, sizeof *out);
    out->length = 18;
    out->descriptor_type = 1;
    out->vendor_id = ports[p].xbox ? 0x045E : sony::kVendor;
    out->product_id = ports[p].xbox ? 0x028E : sony::kDualShock4;
    out->configuration_count = 1;
    return true;
  }
  bool configuration_descriptor(uint32_t p, ByteView buffer, uint16_t* length) const override {
    std::lock_guard<std::mutex> g(mutex);
    if (!ports[p].present) return false;
    const Bytes c = config_for(ports[p].xbox);
    for (size_t i = 0; i < c.size(); ++i) buffer[i] = c[i];
    *length = static_cast<uint16_t>(c.size());
    return true;
  }
  bool alive(uint32_t p) const { std::lock_guard<std::mutex> g(mutex); return ports[p].present; }
  bool set_configuration(uint32_t p, uint8_t) override { return alive(p); }
  bool control_out(uint32_t p, uint8_t, uint8_t, uint16_t, uint16_t) override { return alive(p); }
  bool control_in(uint32_t, uint8_t, uint8_t, uint16_t, uint16_t, ByteView) override { return false; }
  bool start_interrupt_in(uint32_t p, uint8_t, uint16_t, uint8_t) override { return alive(p); }
  bool start_interrupt_out(uint32_t p, uint8_t, uint16_t, uint8_t) override { return alive(p); }
  bool interrupt_out(uint32_t p, uint8_t, ConstByteView) override {
    std::lock_guard<std::mutex> g(mutex);
    if (!ports[p].present) return false;
    ++ports[p].sent;
    return true;
  }
  bool poll_interrupt_in(uint32_t p, uint8_t, ByteView buffer, uint16_t* length) override {
    std::lock_guard<std::mutex> g(mutex);
    if (!ports[p].present || ports[p].in.empty()) return false;
    const Bytes b = ports[p].in.front();
    ports[p].in.pop_front();
    for (size_t i = 0; i < b.size() && i < buffer.size(); ++i) buffer[i] = b[i];
    *length = static_cast<uint16_t>(b.size());
    return true;
  }
  bool start_bulk_endpoint(uint32_t, uint8_t, uint16_t) override { return false; }
  bool bulk_transfer(uint32_t, uint8_t, ByteView, uint32_t*) override { return false; }
  bool reset_bulk_endpoint(uint32_t, uint8_t) override { return false; }
  uint32_t port_generation(uint32_t p) const override { std::lock_guard<std::mutex> g(mutex); return ports[p].generation; }

  void plug(uint32_t p, bool xbox) {
    std::lock_guard<std::mutex> g(mutex);
    ports[p].present = true;
    ports[p].xbox = xbox;
    ports[p].in.clear();
    ++ports[p].generation;
  }
  void unplug(uint32_t p) {
    std::lock_guard<std::mutex> g(mutex);
    ports[p].present = false;
    ++ports[p].generation;
  }
  void send(uint32_t p, uint16_t buttons) {
    std::lock_guard<std::mutex> g(mutex);
    if (!ports[p].present) return;
    if (ports[p].xbox) {
      Bytes r = {0x00, 0x14, static_cast<uint8_t>(buttons), static_cast<uint8_t>(buttons >> 8)};
      r.resize(20, 0);
      ports[p].in.push_back(r);
    } else {
      Bytes r(64, 0);
      r[0] = 0x01;
      r[1] = r[2] = r[3] = r[4] = 128;
      r[5] = 0x08 | ((buttons & 1) ? 0x20 : 0);
      ports[p].in.push_back(r);
    }
  }
};

struct FakeBus final : IUsbBus {
  IUsbHostController* host = nullptr;
  const char* name() const override { return "fake usb"; }
  bool initialize() override { return true; }
  void shutdown() override {}
  uint32_t device_count() const override { return 1; }
  uint32_t controller_count() const override { return 1; }
  UsbControllerKind controller_kind(uint32_t) const override { return UsbControllerKind::Xhci; }
  uint32_t port_count(uint32_t) const override { return host->port_count(); }
  bool port_info(uint32_t, uint32_t p, UsbPortInfo* out) const override { return host->port_info(p, out); }
  bool device_descriptor(uint32_t, uint32_t p, UsbDeviceDescriptor* out) const override { return host->device_descriptor(p, out); }
  IUsbHostController* host_controller(uint32_t c) override { return c == 0 ? host : nullptr; }
};

struct World {
  ThreadedHost host;
  FakeBus bus;
  GameControllerService* service = nullptr;
  bool stop = false;
  long reads = 0, connected_reads = 0;
};

static bool stopping(World* w) { return __atomic_load_n(&w->stop, __ATOMIC_ACQUIRE); }

static void plugger(void* arg) {
  World* w = static_cast<World*>(arg);
  uint32_t seed = 777;
  auto rnd = [&] { seed = seed * 1664525 + 1013904223; return seed >> 8; };
  bool in[ThreadedHost::kPorts] = {};
  // (The service looks for new pads every quarter second: a plug or unplug every 50 ms or so, for two seconds, gives it several to find.)
  for (int i = 0; i < 400; ++i) {
    const uint32_t port = rnd() % ThreadedHost::kPorts;
    if (rnd() % 10 == 0) {
      if (in[port]) w->host.unplug(port);
      else w->host.plug(port, rnd() % 2 == 0);
      in[port] = !in[port];
    }
    for (uint32_t p = 0; p < ThreadedHost::kPorts; ++p) w->host.send(p, static_cast<uint16_t>(rnd()));
    nap(5000);
  }
  __atomic_store_n(&w->stop, true, __ATOMIC_RELEASE);
}

static void reader(void* arg) {
  World* w = static_cast<World*>(arg);
  uint32_t last_packet[GameControllerService::kMaxPlayers] = {};
  while (!stopping(w)) {
    for (uint32_t player = 0; player < 4; ++player) {
      State s;
      uint32_t packet = 0;
      __atomic_add_fetch(&w->reads, 1, __ATOMIC_RELAXED);
      if (w->service->snapshot(player, &s, &packet)) {
        __atomic_add_fetch(&w->connected_reads, 1, __ATOMIC_RELAXED);
        last_packet[player] = packet;
        (void)w->service->set_vibration(player, static_cast<uint16_t>(packet * 997), 0);
      }
      uint64_t words[kGamepadServiceWords] = {1, player};
      const uint32_t status = gamepad_service_call(*w->service, ArrayView<uint64_t>(words));
      CHECK(status == 0 || status == 0xC000009Du);
      uint64_t info[kGamepadServiceWords] = {3, player};
      const uint32_t info_status = gamepad_service_call(*w->service, ArrayView<uint64_t>(info));
      CHECK(info_status == 0 || info_status == 0xC000009Du);
      if (info_status == 0) CHECK(info[4] == static_cast<uint64_t>(Protocol::Xbox360) || info[4] == static_cast<uint64_t>(Protocol::DualShock4));
    }
    nap(100);
  }
  (void)last_packet;
}

int main() {
  TestAllocator alloc;
  {
    World* w = new World;
    w->bus.host = &w->host;
    w->host.plug(0, true);
    GameControllerService service(&w->bus, alloc);
    w->service = &service;
    CHECK(service.start());                     // (spawns the "game-controllers" thread: the host's kernel::running() is true)
    CHECK(service.count() == 1);
    kernel::Thread* threads[3];
    threads[0] = kernel::spawn(plugger, w, "plugger");
    threads[1] = kernel::spawn(reader, w, "reader1");
    threads[2] = kernel::spawn(reader, w, "reader2");
    for (kernel::Thread* t : threads) kernel::join(t);
    // Everything pulled out: within a few scans the slots are empty.
    for (uint32_t p = 0; p < ThreadedHost::kPorts; ++p) w->host.unplug(p);
    bool empty = false;
    for (int i = 0; i < 100 && !empty; ++i) {
      nap(20000);
      empty = service.count() == 0;
    }
    CHECK(empty);
    CHECK(service.added() >= 2 && service.added() == service.removed());
    CHECK(w->connected_reads > 0 && w->reads > w->connected_reads);
    printf("GameControllerThreadTest: %u pads came and went, %ld reads (%ld of a connected pad)\n", service.added(), w->reads, w->connected_reads);
    service.stop();
    delete w;
  }
  CHECK(alloc.live == 0);
  if (g_failed) { printf("%d check(s) failed\n", g_failed); return 1; }
  printf("GameControllerThreadTest: all passed\n");
  return 0;
}
