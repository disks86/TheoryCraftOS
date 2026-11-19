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

// Monitors plugged into a sleeping GPU (Multi/GpuHotplug): a hybrid laptop (Intel iGPU display, NVIDIA dGPU behind root port PEG0 with its HD
// audio) whose DSDT notifies the dGPU from a GPE method, delivered through AcpiEvents; the nodes found by watch_acpi (the GPU, its root port,
// its output Devices) and which Notify values count; the probe asked with the GPU awake under a lease that is given back (D3hot, and D3cold
// through a platform power handler with the probe resumed after the power cut); listeners told once per change; a GPU with a display driver or
// a probe that cannot see outputs passed on unchecked; the polling policy (off by default, period, low-battery skip, clock going back); and
// several threads notifying, servicing and adding / removing listeners at once (ASan and TSan).
#include <stdint.h>
#include <stdio.h>
#include <atomic>
#include <mutex>
#include <thread>
#include <vector>

static std::atomic<int> g_failed{0};
#define CHECK(...) do { if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)

#include "AcpiTestSupport.hpp"
#include "Hardware/GraphicsProcessingUnit/Multi/GpuHotplug.hpp"

namespace {

constexpr uint16_t kPm = 0x50;

// PCI functions with a PM capability; while the power is cut a function answers all ones and forgets its header.
struct Function {
  PciDeviceInfo info{};
  uint32_t config[64] = {};
  bool powered = true;
};

class GpuPci final : public IPciBus {
 public:
  std::vector<Function> functions;
  mutable std::mutex mutex;
  void add(uint8_t bus, uint8_t device, uint8_t function, uint16_t vendor, uint16_t device_id, uint8_t cls, uint8_t sub, uint32_t command) {
    Function f;
    f.info = {{bus, device, function}, vendor, device_id, cls, sub, 0};
    f.config[0] = vendor | (uint32_t{device_id} << 16);
    f.config[1] = command;
    f.config[2] = (uint32_t{cls} << 24) | (uint32_t{sub} << 16);
    f.config[0x34 / 4] = kPm;
    f.config[0x10 / 4] = 0xE0000000u;
    functions.push_back(f);
  }
  Function* find(PciAddress a) const {
    for (const Function& f : functions)
      if (f.info.address.bus == a.bus && f.info.address.device == a.device && f.info.address.function == a.function) return const_cast<Function*>(&f);
    return nullptr;
  }
  void cut(uint8_t bus, uint8_t device, bool on) {
    std::lock_guard<std::mutex> g(mutex);
    for (Function& f : functions) {
      if (f.info.address.bus != bus || f.info.address.device != device) continue;
      f.powered = on;
      if (!on) continue;
      const uint32_t id = f.config[0], cls = f.config[2];
      for (uint32_t& d : f.config) d = 0;
      f.config[0] = id;
      f.config[2] = cls;
      f.config[0x34 / 4] = kPm;
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
    if (!f || !f->powered || offset >= 256 || offset < 0x04 || offset == 0x08 || offset == 0x34) return;
    f->config[offset / 4] = value;
  }
  uint64_t memory_bar(PciAddress, uint8_t) const override { return 0; }
  uint16_t find_capability(PciAddress a, uint8_t id) const override {
    std::lock_guard<std::mutex> g(mutex);
    Function* f = find(a);
    return f && f->powered && id == 0x01 ? kPm : 0;
  }
  uint32_t pm_state(PciAddress a) const { return read32(a, kPm + 4) & 3; }
};

class FakePower final : public IGpuPlatformPower {
 public:
  GpuPci& pci;
  std::atomic<uint32_t> offs{0}, ons{0};
  explicit FakePower(GpuPci& p) : pci(p) {}
  GpuPowerMechanism mechanism() const override { return GpuPowerMechanism::AcpiPowerResources; }
  bool power_off() override { ++offs; pci.cut(1, 0, false); return true; }
  bool power_on() override { ++ons; pci.cut(1, 0, true); return true; }
  uint32_t power_on_delay_ms() const override { return 0; }
};

// The dGPU's probe: its outputs are what the test says; each refresh checks that the GPU is awake and leased.
struct ProbeState {
  std::atomic<uint32_t> connected{0};      // what the hardware has now
  std::atomic<uint32_t> refreshes{0}, resumes{0}, asleep_refreshes{0}, unleased_refreshes{0};
  std::atomic<bool> reads{true};
  std::atomic<bool> busy{false};           // a refresh in progress (two at once would be a bug: the pool serializes them)
  std::atomic<uint32_t> overlaps{0};
  GpuPci* pci = nullptr;
  GpuPool* pool = nullptr;
};
ProbeState* g_state = nullptr;

class FakeProbe final : public IGpuProbe {
 public:
  explicit FakeProbe(PciAddress a) : address_(a) {}
  const char* name() const override { return "Fake dGPU"; }
  const char* status() const override { return "identified"; }
  bool start() override { seen_ = g_state->connected.load(); return true; }
  bool resume() override { ++g_state->resumes; return true; }
  uint64_t vram_bytes() const override { return 4ull << 30; }
  int32_t temperature_millicelsius() override { return 50000; }
  uint32_t connected_outputs() const override { return seen_; }
  bool reads_outputs() const override { return g_state->reads.load(); }
  bool refresh_outputs() override {
    if (g_state->busy.exchange(true)) ++g_state->overlaps;
    ++g_state->refreshes;
    if (g_state->pci->pm_state(address_) != 0) ++g_state->asleep_refreshes;   // (all ones while the power is cut)
    if (g_state->pool->manager().leases(1) == 0) ++g_state->unleased_refreshes;
    const uint32_t now = g_state->connected.load();
    const bool changed = now != seen_;
    seen_ = now;
    g_state->busy = false;
    return changed;
  }

 private:
  PciAddress address_;
  uint32_t seen_ = 0;
};

UniquePointer<IGpuProbe> fake_factory(const GpuContext& context, allocators::IAllocator& allocator) {
  if (context.device.vendor_id != 0x10DE) return UniquePointer<IGpuProbe>();
  return UniquePointer<IGpuProbe>::make_as<FakeProbe>(allocator, context.device.address);
}

// Counts outputs_changed() per GPU.
struct Counter final : IGpuOutputListener {
  std::atomic<uint32_t> calls[GpuTopology::kMaxAdapters] = {};
  std::atomic<uint32_t> total{0};
  std::atomic<bool> gone{false};   // set after remove_listener() returned: no call may come any more
  std::atomic<uint32_t> late{0};
  void outputs_changed(uint32_t gpu) override {
    if (gone.load()) ++late;
    if (gpu < GpuTopology::kMaxAdapters) ++calls[gpu];
    ++total;
  }
};

// The DSDT: \_SB.PCI0 (host bridge) with GFX0 (00:02.0), the dGPU's root port PEG0 (00:01.0) holding PEGP (_ADR 0) with two output Devices
// (HDMI, DP0) and a _PRW; and \_GPE._L11, the hot-plug line's event, which notifies PEGP with 0x81.
Bytes hybrid_dsdt() {
  const Bytes pegp = cat({name_def("_ADR", num(0)), name_def("_PRW", package(2, cat({num(0x11), num(3)}))),
                          device("HDMI", name_def("_ADR", num(0x0100))), device("DP0_", name_def("_ADR", num(0x0101)))});
  const Bytes peg0 = cat({name_def("_ADR", num(0x00010000)), device("PEGP", pegp)});
  const Bytes pci0 = cat({name_def("_HID", num(0x080AD041)), name_def("_CID", num(0x030AD041)), device("GFX0", name_def("_ADR", num(0x00020000))),
                          device("PEG0", peg0)});
  const Bytes gpe = scope("\\_GPE", method("_L11", 0, notify("\\_SB.PCI0.PEG0.PEGP", 0x81)));
  return make_table("DSDT", cat({scope("\\_SB", device("PCI0", pci0)), gpe}));
}

// The machine: the PCI functions, the pool with its probe, the ACPI events with the hot-plug listener.
struct Laptop {
  GpuPci pci;
  TestAllocator allocator;
  ProbeState state;
  GpuPool pool{pci, allocator};
  FakePower power{pci};
  GpuHotplug hotplug{pool};
  Laptop() {
    pci.add(0, 2, 0, 0x8086, 0x9A49, 0x03, 0x00, 0x7);   // the boot display (decoding on)
    pci.add(1, 0, 0, 0x10DE, 0x28E0, 0x03, 0x00, 0x0);   // the dGPU
    pci.add(1, 0, 1, 0x10DE, 0x22BE, 0x04, 0x03, 0x0);   // its HD audio
    state.pci = &pci;
    state.pool = &pool;
    g_state = &state;
    pool.initialize();
    CHECK(pool.bind_probes(nullptr, pci, nullptr, fake_factory) == 1);
    CHECK(pool.manager().asleep(1));
  }
};

void test_acpi_notify() {
  Laptop l;
  FakeHost host;
  FakePorts ports;
  const Bytes dsdt = hybrid_dsdt();
  const Bytes fadt = make_fadt();
  const ConstByteView tables[1] = {ConstByteView(dsdt.data(), dsdt.size())};
  AcpiEvents events(host, ports, l.allocator);
  CHECK(events.initialize(tables, 1, ConstByteView(fadt.data(), fadt.size())) && events.enable_acpi_mode());
  acpi::AmlInterpreter& aml = events.interpreter();

  const GpuAcpiNodes nodes = l.hotplug.watch_acpi(1, aml, PciAddress{1, 0, 0}, PciAddress{0, 1, 0});
  acpi::Node* pegp = aml.find("\\_SB.PCI0.PEG0.PEGP");
  acpi::Node* peg0 = aml.find("\\_SB.PCI0.PEG0");
  acpi::Node* hdmi = aml.find("\\_SB.PCI0.PEG0.PEGP.HDMI");
  acpi::Node* gfx0 = aml.find("\\_SB.PCI0.GFX0");
  CHECK(pegp && peg0 && hdmi && gfx0);
  CHECK(nodes.device == pegp && nodes.bridge == peg0);
  CHECK(l.hotplug.acpi_nodes(1).device == pegp && l.hotplug.acpi_nodes(1).bridge == peg0);
  CHECK(l.hotplug.watched(1) && !l.hotplug.watched(0));
  CHECK(l.hotplug.watch_node(1, gfx0, GpuAcpiRole::OtherAdapter));
  CHECK(!l.hotplug.watch_node(1, nullptr, GpuAcpiRole::Device) && !l.hotplug.watch_node(7, gfx0, GpuAcpiRole::Device));
  CHECK(events.add_listener(&l.hotplug));
  // The GPU's wake event (_PRW) is the hot-plug line: armed as main.cpp does it.
  CHECK(events.enable_wake(nodes.device));

  Counter counter;
  CHECK(l.hotplug.add_listener(&counter));
  CHECK(l.hotplug.add_listener(&counter));   // (once)
  CHECK(l.hotplug.service(0) == 0 && l.state.refreshes == 0);   // nothing yet: the GPU is not woken
  CHECK(l.pool.manager().wake_count(1) == 1);                   // (the probe's start)

  // A monitor is plugged in while the GPU sleeps: the firmware's GPE method notifies PEGP.
  l.state.connected = 1;
  ports.raise(0x11);
  events.poll();
  CHECK(l.hotplug.notifications() == 1);
  CHECK(l.state.refreshes == 0);                                // (on_notify only marks it)
  CHECK(l.hotplug.service(10) == 1);
  CHECK(l.state.refreshes == 1 && l.state.asleep_refreshes == 0 && l.state.unleased_refreshes == 0);
  CHECK(counter.calls[1] == 1 && counter.total == 1);
  CHECK(l.pool.manager().leases(1) == 0 && l.pool.manager().asleep(1));   // the lease went back: asleep again
  CHECK(l.pool.manager().wake_count(1) == 2);
  CHECK(l.pool.probe_connected_outputs(1) == 1);
  CHECK(l.hotplug.service(20) == 0 && counter.total == 1);      // (nothing new)

  // Notified, but nothing changed: the probe looks, nobody is told.
  l.hotplug.on_notify(pegp, 0x80);
  CHECK(l.hotplug.service(30) == 0 && l.state.refreshes == 2 && counter.total == 1);
  // Several notifications before a service: one look.
  l.state.connected = 0;
  l.hotplug.on_notify(peg0, 0x00);
  l.hotplug.on_notify(hdmi, 0x81);
  l.hotplug.on_notify(gfx0, 0x81);
  CHECK(l.hotplug.notifications() == 5);
  CHECK(l.hotplug.service(40) == 1 && l.state.refreshes == 3 && counter.calls[1] == 2);
  // Values that do not mean an output change: brightness on an output, a cycle hotkey on the integrated GPU, a device check on the GPU itself,
  // nodes that are nobody's.
  l.hotplug.on_notify(hdmi, 0x86);
  l.hotplug.on_notify(gfx0, 0x80);
  l.hotplug.on_notify(pegp, 0x01);
  l.hotplug.on_notify(peg0, 0x81);
  l.hotplug.on_notify(aml.find("\\_SB.PCI0"), 0x81);
  l.hotplug.on_notify(nullptr, 0x81);
  CHECK(l.hotplug.notifications() == 5);
  CHECK(l.hotplug.service(50) == 0 && l.state.refreshes == 3);
  CHECK(GpuHotplug::is_output_notification(GpuAcpiRole::Bridge, 0x02) && GpuHotplug::is_output_notification(GpuAcpiRole::Device, 0x02));

  // D3cold: the GPU lost its power; the look wakes it through the platform, and the probe sets itself up again first.
  CHECK(l.pool.manager().attach_platform_power(1, &l.power));
  CHECK(l.pool.manager().service(60) == 1 && l.pool.manager().power_level(1) == GpuPowerLevel::D3Cold);
  const uint32_t resumes = l.state.resumes;
  l.state.connected = 2;
  CHECK(l.hotplug.request_check(1) && !l.hotplug.request_check(0));
  CHECK(l.hotplug.service(70) == 1 && counter.calls[1] == 3);
  CHECK(l.state.resumes == resumes + 1 && l.state.asleep_refreshes == 0);
  CHECK(l.power.ons == 1 && l.pool.manager().leases(1) == 0 && l.pool.manager().asleep(1));

  // A removed listener hears nothing.
  l.hotplug.remove_listener(&counter);
  l.state.connected = 0;
  CHECK(l.hotplug.request_check(1));
  CHECK(l.hotplug.service(80) == 1 && counter.total == 3);
  CHECK(l.hotplug.changes() == 4);
  CHECK(l.state.overlaps == 0);
}

// What cannot be looked at is passed on when the firmware says so, and never polled.
void test_cannot_tell() {
  Laptop l;
  Counter counter;
  CHECK(l.hotplug.add_listener(&counter));
  CHECK(l.hotplug.watch(1) && l.hotplug.watch(0) && !l.hotplug.watch(5));
  // The display GPU (index 0) has no probe (and here no driver either): a notification goes to the listeners unchecked, without a wake.
  CHECK(l.pool.refresh_outputs(0) == GpuOutputCheck::CannotTell && l.pool.refresh_outputs(9) == GpuOutputCheck::NoGpu);
  CHECK(l.hotplug.request_check(0));
  CHECK(l.hotplug.service(0) == 1 && counter.calls[0] == 1 && l.state.refreshes == 0);
  // A probe that cannot see outputs: the same, and the GPU stays asleep.
  l.state.reads = false;
  CHECK(!l.pool.outputs_readable(1));
  const uint32_t wakes = l.pool.manager().wake_count(1);
  CHECK(l.hotplug.request_check(1));
  CHECK(l.hotplug.service(10) == 1 && counter.calls[1] == 1 && l.state.refreshes == 0);
  CHECK(l.pool.manager().wake_count(1) == wakes);
  GpuOutputPollPolicy on;
  on.enabled = true;
  l.hotplug.set_poll_policy(on);
  for (uint64_t t = 100; t < 100000; t += 1000) (void)l.hotplug.service(t);
  CHECK(l.hotplug.checks() == 2 && counter.total == 2 && l.pool.manager().wake_count(1) == wakes);   // never polled
}

void test_polling() {
  Laptop l;
  Counter counter;
  CHECK(l.hotplug.add_listener(&counter));
  CHECK(l.hotplug.watch(1));
  // Off by default: the GPU is never woken.
  const GpuOutputPollPolicy defaults = l.hotplug.poll_policy();
  CHECK(!defaults.enabled && defaults.period_ms == 10000 && defaults.min_battery_percent == kDiscreteMinBatteryPercent);
  l.state.connected = 1;
  for (uint64_t t = 0; t <= 60000; t += 500) (void)l.hotplug.service(t);
  CHECK(l.state.refreshes == 0 && counter.total == 0 && l.pool.manager().wake_count(1) == 1);

  // On, every 10 s from the first service.
  GpuOutputPollPolicy on;
  on.enabled = true;
  l.hotplug.set_poll_policy(on);
  CHECK(l.hotplug.service(100000) == 0 && l.state.refreshes == 0);   // (the period starts)
  CHECK(l.hotplug.service(109999) == 0 && l.state.refreshes == 0);
  CHECK(l.hotplug.service(110000) == 1 && l.state.refreshes == 1 && counter.calls[1] == 1);
  CHECK(l.pool.manager().leases(1) == 0 && l.pool.manager().asleep(1));
  CHECK(l.hotplug.service(115000) == 0 && l.state.refreshes == 1);
  CHECK(l.hotplug.service(120000) == 0 && l.state.refreshes == 2 && counter.calls[1] == 1);   // looked, nothing new
  // A notification counts as a look: the next poll is a period after it.
  l.hotplug.on_notify(nullptr, 0x81);
  CHECK(l.hotplug.request_check(1));
  CHECK(l.hotplug.service(125000) == 0 && l.state.refreshes == 3);
  CHECK(l.hotplug.service(130000) == 0 && l.state.refreshes == 3);
  CHECK(l.hotplug.service(135000) == 0 && l.state.refreshes == 4);

  // On battery below the threshold: a sleeping GPU is not woken ...
  l.pool.manager().set_power_status(PowerSource::Battery, 20, false);
  l.state.connected = 2;
  CHECK(l.hotplug.service(145000) == 0 && l.state.refreshes == 4 && l.hotplug.polls_skipped() == 1);
  // ... but one that is awake anyway is looked at.
  CHECK(l.pool.manager().acquire(1));
  CHECK(l.hotplug.service(155000) == 1 && l.state.refreshes == 5 && counter.calls[1] == 2);
  l.pool.manager().release(1);
  // Battery saver counts as low; enough charge does not; mains does not.
  l.pool.manager().set_power_status(PowerSource::Battery, 90, true);
  CHECK(l.hotplug.service(165000) == 0 && l.hotplug.polls_skipped() == 2);
  l.pool.manager().set_power_status(PowerSource::Battery, 30, false);
  CHECK(l.hotplug.service(175000) == 0 && l.state.refreshes == 6);
  l.pool.manager().set_power_status(PowerSource::Ac, 5, false);
  CHECK(l.hotplug.service(185000) == 0 && l.state.refreshes == 7);
  // A notification wakes it whatever the battery says (the firmware saw a monitor).
  l.pool.manager().set_power_status(PowerSource::Battery, 5, true);
  l.state.connected = 3;
  CHECK(l.hotplug.request_check(1));
  CHECK(l.hotplug.service(186000) == 1 && l.state.refreshes == 8);
  l.pool.manager().set_power_status(PowerSource::Ac, 100, false);

  // A shorter period (raised to the minimum), a clock that goes back, and off again.
  on.period_ms = 10;
  l.hotplug.set_poll_policy(on);
  CHECK(l.hotplug.poll_policy().period_ms == GpuOutputPollPolicy::kMinPeriodMs);
  CHECK(l.hotplug.service(200000) == 0 && l.state.refreshes == 8);
  CHECK(l.hotplug.service(201000) == 0 && l.state.refreshes == 9);
  CHECK(l.hotplug.service(5000) == 0 && l.state.refreshes == 9);   // (back: the period starts again)
  CHECK(l.hotplug.service(5999) == 0 && l.state.refreshes == 9);
  CHECK(l.hotplug.service(6000) == 0 && l.state.refreshes == 10);
  on.enabled = false;
  l.hotplug.set_poll_policy(on);
  for (uint64_t t = 7000; t < 50000; t += 1000) (void)l.hotplug.service(t);
  CHECK(l.state.refreshes == 10);
  CHECK(l.pool.manager().leases(1) == 0 && l.state.asleep_refreshes == 0 && l.state.unleased_refreshes == 0);
}

void test_listener_list() {
  Laptop l;
  Counter counters[GpuHotplug::kMaxListeners + 1];
  for (uint32_t i = 0; i < GpuHotplug::kMaxListeners; ++i) CHECK(l.hotplug.add_listener(&counters[i]));
  CHECK(!l.hotplug.add_listener(&counters[GpuHotplug::kMaxListeners]) && !l.hotplug.add_listener(nullptr));
  l.hotplug.remove_listener(&counters[3]);
  CHECK(l.hotplug.add_listener(&counters[GpuHotplug::kMaxListeners]));
  CHECK(l.hotplug.watch(1));
  l.state.connected = 1;
  CHECK(l.hotplug.request_check(1));
  CHECK(l.hotplug.service(0) == 1);
  for (uint32_t i = 0; i <= GpuHotplug::kMaxListeners; ++i) CHECK(counters[i].calls[1] == (i == 3 ? 0u : 1u));
}

// One thread delivers notifications, two service (as the main loop and a second caller would), one polls with a short period, and one adds and
// removes listeners: every change is reported, the GPU is awake and leased whenever the probe looks, looks never overlap, and a removed listener
// is never called after remove_listener() returned.
void test_threads() {
  Laptop l;
  FakeHost host;
  FakePorts ports;
  const Bytes dsdt = hybrid_dsdt();
  const Bytes fadt = make_fadt();
  const ConstByteView tables[1] = {ConstByteView(dsdt.data(), dsdt.size())};
  AcpiEvents events(host, ports, l.allocator);
  CHECK(events.initialize(tables, 1, ConstByteView(fadt.data(), fadt.size())));
  const GpuAcpiNodes nodes = l.hotplug.watch_acpi(1, events.interpreter(), PciAddress{1, 0, 0}, PciAddress{0, 1, 0});
  CHECK(nodes.device && nodes.bridge);
  CHECK(l.pool.manager().attach_platform_power(1, &l.power));
  l.pool.manager().set_autosuspend_ms(1);
  GpuOutputPollPolicy on;
  on.enabled = true;
  on.period_ms = GpuOutputPollPolicy::kMinPeriodMs;
  l.hotplug.set_poll_policy(on);
  Counter steady;
  CHECK(l.hotplug.add_listener(&steady));

  std::atomic<bool> stop{false};
  std::atomic<uint64_t> clock{0};
  std::atomic<uint32_t> late{0};
  std::vector<std::thread> threads;
  threads.emplace_back([&] {   // the firmware
    for (uint32_t i = 0; !stop.load(); ++i) {
      l.state.connected = (i / 7) % 3;
      l.hotplug.on_notify(i & 1 ? nodes.device : nodes.bridge, i & 1 ? 0x81 : 0x00);
      std::this_thread::yield();
    }
  });
  for (int s = 0; s < 2; ++s)
    threads.emplace_back([&] {   // service() callers, with the clock moving on
      while (!stop.load()) (void)l.hotplug.service(clock.fetch_add(97));
    });
  threads.emplace_back([&] {   // the GPU manager's autosuspend and D3cold
    while (!stop.load()) (void)l.pool.manager().service(clock.load());
  });
  threads.emplace_back([&] {   // listeners coming and going
    while (!stop.load()) {
      Counter temporary;
      if (l.hotplug.add_listener(&temporary)) {
        std::this_thread::yield();
        l.hotplug.remove_listener(&temporary);
        temporary.gone = true;
        std::this_thread::yield();
        late += temporary.late.load();
      }
    }
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(600));
  stop = true;
  for (std::thread& t : threads) t.join();

  printf("  threads: %u notifications, %u looks, %u changes, %u wakes\n", l.hotplug.notifications(), l.hotplug.checks(), l.hotplug.changes(),
         l.pool.manager().wake_count(1));
  CHECK(l.hotplug.notifications() > 0 && l.hotplug.checks() > 0 && l.hotplug.changes() > 0);
  CHECK(steady.total == l.hotplug.changes());   // once per change
  CHECK(steady.calls[0] == 0);
  CHECK(l.state.overlaps == 0 && l.state.asleep_refreshes == 0 && l.state.unleased_refreshes == 0);
  CHECK(late == 0);
  CHECK(l.hotplug.wake_failures() == 0);
  for (uint64_t t = clock.load() + 1000; t < clock.load() + 1010; ++t) (void)l.pool.manager().service(t);
  CHECK(l.pool.manager().leases(1) == 0 && l.pool.manager().asleep(1));
}

}  // namespace

int main() {
  test_acpi_notify();
  test_cannot_tell();
  test_polling();
  test_listener_list();
  test_threads();
  if (g_failed.load()) {
    printf("GpuHotplugTest: %d FAILED\n", g_failed.load());
    return 1;
  }
  printf("GpuHotplugTest: all passed\n");
  return 0;
}
