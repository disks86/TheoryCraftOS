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

// Runtime power management of a hybrid laptop's discrete GPU (GpuManager): its other functions sleeping with it, autosuspend, D3cold through a
// platform power handler with the configuration header saved and written back, functions a driver uses, the power source in select(), and the
// pool's probes for GPUs without a driver. Several threads take and drop leases while service() runs.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <atomic>
#include <mutex>
#include <thread>
#include <vector>
#include "Hardware/GraphicsProcessingUnit/Multi/GpuManager.hpp"
#include "Hardware/GraphicsProcessingUnit/Multi/GpuPool.hpp"

using namespace hardware;

static int g_failed = 0;
#define CHECK(...) do { if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)

namespace {

constexpr uint16_t kPm = 0x50;

// One PCI function with a 256-byte configuration space; while the power is cut it answers all ones and forgets everything.
struct Function {
  PciDeviceInfo info{};
  uint32_t config[64] = {};
  bool has_pm = true;
  bool powered = true;
  uint32_t writes = 0;
};

class FakePci final : public IPciBus {
 public:
  std::vector<Function> functions;
  mutable std::mutex mutex;
  Function& add(uint8_t bus, uint8_t device, uint8_t function, uint16_t vendor, uint16_t device_id, uint8_t cls, uint8_t sub, uint32_t command) {
    Function f;
    f.info = {{bus, device, function}, vendor, device_id, cls, sub, 0};
    f.config[0] = vendor | (uint32_t{device_id} << 16);
    f.config[1] = command;
    f.config[2] = (uint32_t{cls} << 24) | (uint32_t{sub} << 16);
    f.config[0x34 / 4] = kPm;
    f.config[0x10 / 4] = 0xE0000000u | (function << 20);   // BAR0
    f.config[0x14 / 4] = 0xC000000Cu;                       // BAR1 (64-bit prefetchable)
    f.config[0x3C / 4] = 0x10B;                             // interrupt line/pin
    functions.push_back(f);
    return functions.back();
  }
  Function* find(PciAddress a) const {
    for (const Function& f : functions)
      if (f.info.address.bus == a.bus && f.info.address.device == a.device && f.info.address.function == a.function) return const_cast<Function*>(&f);
    return nullptr;
  }
  // The power to the whole device (every function of bus:device) goes or comes back.
  void cut(uint8_t bus, uint8_t device, bool on) {
    std::lock_guard<std::mutex> g(mutex);
    for (Function& f : functions) {
      if (f.info.address.bus != bus || f.info.address.device != device) continue;
      f.powered = on;
      if (!on) continue;
      // Back from a reset: the IDs and read-only fields return, what the OS had programmed is gone, the PM state is D0.
      const uint32_t id = f.info.vendor_id | (uint32_t{f.info.device_id} << 16);
      const uint32_t cls = (uint32_t{f.info.class_code} << 24) | (uint32_t{f.info.subclass} << 16);
      memset(f.config, 0, sizeof f.config);
      f.config[0] = id;
      f.config[2] = cls;
      f.config[0x34 / 4] = kPm;
      f.config[0x3C / 4] = 0x100;
    }
  }
  const char* name() const override { return "fake"; }
  bool initialize() override { return true; }
  void shutdown() override {}
  uint32_t device_count() const override { return static_cast<uint32_t>(functions.size()); }
  bool pci_device(uint32_t i, PciDeviceInfo* out) const override { if (i >= functions.size()) return false; *out = functions[i].info; return true; }
  uint32_t read32(PciAddress a, uint16_t offset) const override {
    std::lock_guard<std::mutex> g(mutex);
    Function* f = find(a);
    if (!f || !f->powered || offset >= 256) return 0xFFFFFFFFu;
    return f->config[offset / 4];
  }
  void write32(PciAddress a, uint16_t offset, uint32_t value) const override {
    std::lock_guard<std::mutex> g(mutex);
    Function* f = find(a);
    if (!f || !f->powered || offset >= 256) return;
    ++f->writes;
    if (offset < 0x04 || offset == 0x08 || offset == 0x2C || offset == 0x34) return;   // read-only
    f->config[offset / 4] = value;
  }
  uint64_t memory_bar(PciAddress, uint8_t) const override { return 0; }
  uint16_t find_capability(PciAddress a, uint8_t id) const override {
    std::lock_guard<std::mutex> g(mutex);
    Function* f = find(a);
    return f && f->powered && f->has_pm && id == 0x01 ? kPm : 0;
  }
  uint32_t pm_state(PciAddress a) const { return read32(a, kPm + 4) & 3; }
};

std::atomic<uint32_t> g_delay_ms{0};
void count_delay(uint32_t ms) { g_delay_ms += ms; }

class FakePower final : public IGpuPlatformPower {
 public:
  FakePci& pci;
  uint8_t bus, device;
  bool fail_off = false, fail_on = false, dead_on = false;
  std::atomic<uint32_t> offs{0}, ons{0};
  bool saw_d3hot_before_off = true;
  FakePower(FakePci& p, uint8_t b, uint8_t d) : pci(p), bus(b), device(d) {}
  GpuPowerMechanism mechanism() const override { return GpuPowerMechanism::AcpiPowerResources; }
  bool power_off() override {
    for (const Function& f : pci.functions)
      if (f.info.address.bus == bus && f.info.address.device == device && f.info.class_code == 0x03 && (f.config[(kPm + 4) / 4] & 3) != 3) saw_d3hot_before_off = false;
    if (fail_off) return false;
    ++offs;
    pci.cut(bus, device, false);
    return true;
  }
  bool power_on() override {
    if (fail_on) return false;
    ++ons;
    if (!dead_on) pci.cut(bus, device, true);
    return true;
  }
  uint32_t power_on_delay_ms() const override { return 50; }
};

// iGPU at 00:02.0 (the display), dGPU at 01:00.0 with HD audio 01:00.1 and USB 01:00.2.
struct Laptop {
  FakePci pci;
  PciAddress igpu{0, 2, 0}, dgpu{1, 0, 0}, hda{1, 0, 1}, usb{1, 0, 2};
  Laptop() {
    pci.add(0, 2, 0, 0x8086, 0x9A49, 0x03, 0x00, 0x7);      // Tiger Lake Xe, decoding on: the boot display
    pci.add(1, 0, 0, 0x10DE, 0x28E0, 0x03, 0x00, 0x0);      // RTX 4060 Laptop
    pci.add(1, 0, 1, 0x10DE, 0x22BE, 0x04, 0x03, 0x0);      // its HD audio
    pci.add(1, 0, 2, 0x10DE, 0x1AEC, 0x0C, 0x03, 0x0);      // its USB-C controller
  }
};

void test_companions_and_autosuspend() {
  Laptop l;
  GpuManager m(l.pci, count_delay);
  m.initialize();
  CHECK(m.count() == 2 && m.is_hybrid() && m.display_index() == 0);
  CHECK(m.companion_count(1) == 2 && m.companion_count(0) == 0);
  // Asleep from the start, its functions with it; the display is untouched.
  CHECK(m.power_level(1) == GpuPowerLevel::D3Hot && l.pci.pm_state(l.dgpu) == 3 && l.pci.pm_state(l.hda) == 3 && l.pci.pm_state(l.usb) == 3);
  CHECK(l.pci.pm_state(l.igpu) == 0);
  // A lease wakes all three.
  CHECK(m.acquire(1) && l.pci.pm_state(l.dgpu) == 0 && l.pci.pm_state(l.hda) == 0 && l.pci.pm_state(l.usb) == 0 && m.wake_count(1) == 1);
  // With autosuspend the last release leaves it on until service() has seen it idle for the time.
  m.set_autosuspend_ms(5000);
  m.release(1);
  CHECK(m.power_level(1) == GpuPowerLevel::On);
  CHECK(m.service(1000) == 0 && m.power_level(1) == GpuPowerLevel::On);   // starts the clock
  CHECK(m.service(5999) == 0 && m.power_level(1) == GpuPowerLevel::On);
  // A lease taken in between stops the clock.
  CHECK(m.acquire(1)); m.release(1);
  CHECK(m.service(7000) == 0 && m.power_level(1) == GpuPowerLevel::On);
  CHECK(m.service(12000) == 1 && m.power_level(1) == GpuPowerLevel::D3Hot && l.pci.pm_state(l.hda) == 3);
  CHECK(m.wake_count(1) == 1);   // (the lease in between found it awake)
  // No cold sleep without a handler, or without permission.
  CHECK(m.service(20000) == 0 && m.power_level(1) == GpuPowerLevel::D3Hot);
  FakePower power(l.pci, 1, 0);
  CHECK(m.attach_platform_power(1, &power) && !m.attach_platform_power(5, &power));
  CHECK(m.service(21000) == 0 && power.offs == 0);
  m.allow_cold(1, true);
  CHECK(m.service(22000) == 1 && m.power_level(1) == GpuPowerLevel::D3Cold && power.offs == 1 && power.saw_d3hot_before_off);
  CHECK(l.pci.read32(l.dgpu, 0) == 0xFFFFFFFFu);
  CHECK(!m.attach_platform_power(1, nullptr));   // the handler that cut the power must stay
}

void test_cold_restore() {
  Laptop l;
  GpuManager m(l.pci, count_delay);
  m.initialize();
  FakePower power(l.pci, 1, 0);
  CHECK(m.attach_platform_power(1, &power));
  m.allow_cold(1, true);
  // What the OS had programmed (the firmware's BAR assignment, a command register with memory decoding on) must survive the power cut.
  CHECK(m.acquire(1));
  l.pci.write32(l.dgpu, 0x04, 0x0006);
  l.pci.write32(l.dgpu, 0x10, 0xD2000000u);
  l.pci.write32(l.hda, 0x10, 0xD3080000u);
  l.pci.write32(l.hda, 0x04, 0x0002);
  m.release(1);
  CHECK(m.power_level(1) == GpuPowerLevel::D3Hot);
  CHECK(m.service(0) == 1 && m.power_level(1) == GpuPowerLevel::D3Cold);
  g_delay_ms = 0;
  CHECK(m.acquire(1) && power.ons == 1 && m.power_level(1) == GpuPowerLevel::On && m.wake_count(1) == 2);
  CHECK(g_delay_ms >= 60);   // the handler's 50 ms, then the 10 ms D3hot -> D0 settle
  CHECK(l.pci.read32(l.dgpu, 0x10) == 0xD2000000u && (l.pci.read32(l.dgpu, 0x04) & 0xFFFF) == 0x0006);
  CHECK(l.pci.read32(l.dgpu, 0x14) == 0xC000000Cu && (l.pci.read32(l.dgpu, 0x3C) & 0xFF) == 0x0B);
  CHECK(l.pci.read32(l.hda, 0x10) == 0xD3080000u && (l.pci.read32(l.hda, 0x04) & 0xFFFF) == 0x0002);
  CHECK(l.pci.pm_state(l.dgpu) == 0 && l.pci.pm_state(l.hda) == 0);
  // While it holds a lease nothing takes it down.
  CHECK(m.service(100000) == 0 && m.power_level(1) == GpuPowerLevel::On);
  m.release(1);
  CHECK(m.service(100001) == 1 && m.power_level(1) == GpuPowerLevel::D3Cold);

  // A handler that refuses to switch it on: the lease fails and it stays cut off.
  power.fail_on = true;
  CHECK(!m.acquire(1) && m.leases(1) == 0 && m.power_level(1) == GpuPowerLevel::D3Cold);
  // ... or one that reports success but the device never answers.
  power.fail_on = false;
  power.dead_on = true;
  CHECK(!m.acquire(1) && m.power_level(1) == GpuPowerLevel::D3Cold);
  power.dead_on = false;
  CHECK(m.acquire(1) && m.power_level(1) == GpuPowerLevel::On);
  m.release(1);
  // One that refuses to switch it off leaves it in D3hot and tries again on the next service().
  power.fail_off = true;
  CHECK(m.service(200000) == 0 && m.power_level(1) == GpuPowerLevel::D3Hot);
  power.fail_off = false;
  CHECK(m.service(200001) == 1 && m.power_level(1) == GpuPowerLevel::D3Cold);
}

void test_busy_function() {
  Laptop l;
  GpuManager m(l.pci, count_delay);
  m.initialize();
  FakePower power(l.pci, 1, 0);
  CHECK(m.attach_platform_power(1, &power));
  m.allow_cold(1, true);
  CHECK(!m.mark_function_busy(l.igpu, true) && !m.mark_function_busy(PciAddress{9, 9, 0}, true));
  // An audio driver takes the HD-audio function: it is woken and stays awake while the GPU sleeps, and the GPU never goes cold.
  CHECK(m.mark_function_busy(l.hda, true) && l.pci.pm_state(l.hda) == 0);
  CHECK(m.acquire(1)); m.release(1);
  CHECK(m.power_level(1) == GpuPowerLevel::D3Hot && l.pci.pm_state(l.hda) == 0 && l.pci.pm_state(l.usb) == 3);
  CHECK(m.service(0) == 0 && power.offs == 0);
  // Given back: the power may go.
  CHECK(m.mark_function_busy(l.hda, false));
  CHECK(m.service(1) == 1 && m.power_level(1) == GpuPowerLevel::D3Cold);
  // Taken again while the power is cut: the GPU's power comes back for it (the function needs the device), and it sleeps again later.
  CHECK(m.mark_function_busy(l.hda, true) && power.ons == 1 && l.pci.pm_state(l.hda) == 0 && m.power_level(1) == GpuPowerLevel::On);
  CHECK(m.service(2) == 0);
  CHECK(m.service(3) == 1 && m.power_level(1) == GpuPowerLevel::D3Hot && l.pci.pm_state(l.hda) == 0);
}

void test_power_source() {
  Laptop l;
  GpuManager m(l.pci);
  m.initialize();
  GpuSelectionInput game;
  game.workload = GpuWorkload::Game;
  CHECK(m.select(game) == 1);                      // unknown power: the fast one
  m.set_power_status(PowerSource::Battery, 20, false);
  CHECK(m.select(game) == 0);                      // low battery: the integrated one
  m.set_power_status(PowerSource::Battery, 80, false);
  CHECK(m.select(game) == 1);
  m.set_power_status(PowerSource::Battery, 80, true);
  CHECK(m.select(game) == 0);                      // battery saver
  m.set_power_status(PowerSource::Ac, 10, false);
  CHECK(m.select(game) == 1);
  game.power = PowerSource::Battery;               // the caller's own power source wins
  game.battery_percent = 5;
  CHECK(m.select(game) == 0);
}

// --- The pool's probes.
struct ProbeLog {
  int starts = 0, resumes = 0;
  bool awake_at_start = false;
  FakePci* pci = nullptr;
};
ProbeLog g_probe;

class FakeProbe final : public IGpuProbe {
 public:
  explicit FakeProbe(PciAddress a) : address_(a) {}
  const char* name() const override { return "Fake dGPU"; }
  const char* status() const override { return "identified"; }
  bool start() override {
    ++g_probe.starts;
    g_probe.awake_at_start = g_probe.pci->pm_state(address_) == 0;
    return true;
  }
  bool resume() override { ++g_probe.resumes; return true; }
  uint64_t vram_bytes() const override { return 8ull << 30; }
  int32_t temperature_millicelsius() override { return 45000; }

 private:
  PciAddress address_;
};

UniquePointer<IGpuProbe> fake_factory(const GpuContext& context, allocators::IAllocator& allocator) {
  if (context.device.vendor_id != 0x10DE) return UniquePointer<IGpuProbe>();
  return UniquePointer<IGpuProbe>::make_as<FakeProbe>(allocator, context.device.address);
}

struct HeapAllocator final : allocators::IAllocator {
  void* allocate(uint64_t size) override { return allocate_aligned(size, 16); }
  void* allocate_aligned(uint64_t size, uint64_t alignment) override { return aligned_alloc(alignment, (size + alignment - 1) / alignment * alignment); }
  void deallocate(void* p) override { free(p); }
};

void test_pool_probes() {
  Laptop l;
  HeapAllocator heap;
  g_probe = ProbeLog{};
  g_probe.pci = &l.pci;
  GpuPool pool(l.pci, heap, count_delay);
  pool.initialize();
  CHECK(pool.bind_probes(nullptr, l.pci, nullptr, fake_factory) == 1);
  CHECK(g_probe.starts == 1 && g_probe.awake_at_start);
  CHECK(pool.node(1)->probe && !pool.node(0)->probe && pool.node(1)->probe->vram_bytes() == (8ull << 30));
  CHECK(pool.manager().power_level(1) == GpuPowerLevel::D3Hot);   // put back to sleep after the probe
  CHECK(pool.probe_temperature(1) == -1 && pool.probe_temperature(0) == -1);   // asleep: not woken for a sensor
  FakePower power(l.pci, 1, 0);
  CHECK(pool.manager().attach_platform_power(1, &power));
  CHECK(pool.manager().service(0) == 1 && pool.manager().power_level(1) == GpuPowerLevel::D3Cold);   // a probe lets it go cold
  // Someone renders on it: the probe catches up once (the power was cut), and reads the sensor while it is awake.
  CHECK(pool.manager().acquire(1));
  CHECK(pool.probe_temperature(1) == 45000 && g_probe.resumes == 1);
  CHECK(pool.probe_temperature(1) == 45000 && g_probe.resumes == 1);
  pool.manager().release(1);
  CHECK(pool.manager().asleep(1));
  CHECK(pool.bind_probes(nullptr, l.pci, nullptr, fake_factory) == 1 && g_probe.starts == 1);   // (again: nothing new)
}

// Several cores take and drop leases on the discrete GPU while another runs service(): the count never goes wrong, the GPU is awake whenever a
// lease is held, and at the end it sleeps.
void test_threads() {
  Laptop l;
  GpuManager m(l.pci);
  m.initialize();
  FakePower power(l.pci, 1, 0);
  CHECK(m.attach_platform_power(1, &power));
  m.allow_cold(1, true);
  m.set_autosuspend_ms(2);
  std::atomic<bool> stop{false};
  std::atomic<uint32_t> bad{0}, leases{0};
  std::vector<std::thread> workers;
  for (int t = 0; t < 4; ++t)
    workers.emplace_back([&] {
      for (int i = 0; i < 2000; ++i) {
        if (!m.acquire(1)) { ++bad; continue; }
        ++leases;
        if (l.pci.pm_state(l.dgpu) != 0) ++bad;   // a lease means it is on
        m.release(1);
      }
    });
  std::thread servicer([&] {
    uint64_t now = 0;
    while (!stop) (void)m.service(now += 1);
  });
  for (std::thread& w : workers) w.join();
  stop = true;
  servicer.join();
  CHECK(bad == 0 && leases == 8000 && m.leases(1) == 0);
  for (uint64_t t = 1000000; t < 1000010; ++t) (void)m.service(t);
  CHECK(m.power_level(1) == GpuPowerLevel::D3Cold);
  CHECK(power.offs == power.ons + 1);   // every cut but the last was undone by a wake
}

}  // namespace

int main() {
  test_companions_and_autosuspend();
  test_cold_restore();
  test_busy_function();
  test_power_source();
  test_pool_probes();
  test_threads();
  if (g_failed) { printf("GpuRuntimePowerTest: %d check(s) FAILED\n", g_failed); return 1; }
  printf("GpuRuntimePowerTest: ok\n");
  return 0;
}
