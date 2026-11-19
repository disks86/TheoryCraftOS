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

// Runtime power of IntelHdaAudioDevice against the simulated controller of tests/FakeHda.hpp (a graphics card's HDMI / DisplayPort codec),
// put behind a PCI function with a power management capability: the idle suspend from service(), what it leaves the hardware in (stream
// stopped, codec in D3, link in reset, function in D3hot), the resume on the next write with sound (D0, the header back, the codec and the
// stream programmed again from scratch: the link reset wipes the simulated codec here, as it does a real one), the observer's calls and their
// order against the register accesses, and writers, volume changes and service() on several threads while no register is touched in D3.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "FakeHda.hpp"
#include "Hardware/AudioDevice/Intel/IntelHdaAudioDevice.hpp"

using namespace hardware;

static std::atomic<int> g_failed{0};
#define CHECK(...) do { if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)

struct TestAllocator final : allocators::IAllocator {
  void* allocate(uint64_t size) override { return malloc(size); }
  void* allocate_aligned(uint64_t size, uint64_t alignment) override {
    const uint64_t al = alignment < 16 ? 16 : alignment;
    return aligned_alloc(al, (size + al - 1) / al * al);
  }
  void deallocate(void* p) override { if (p) free(p); }
};

static const PciAddress kFunction{1, 0, 1};   // the GPU's audio function
static constexpr uint16_t kPmCap = 0x50;
static constexpr uint32_t kOutputStream = 0x80;   // (FakeHda has no input streams)

// The function's configuration space: a PM capability whose D3hot -> D0 transition loses the header (No_Soft_Reset clear), as most do.
struct FakeFunction final : IPciBus {
  mutable std::mutex lock;
  mutable uint32_t config[64] = {};
  mutable std::vector<std::string> log;   // "D3", "D0"
  FakeFunction() {
    config[0] = 0xAAE01002;          // vendor / device
    config[1] = 0x00100006;          // status: capability list; command: memory + bus master
    config[2] = 0x04030000;          // HD audio
    config[4] = 0xFE000004;          // BAR0 (64-bit)
    config[5] = 0x00000001;
    config[15] = 0x0000010B;         // interrupt line / pin
    config[0x34 / 4] = kPmCap;
    config[kPmCap / 4] = 0x00030001; // PM capability, version 3
  }
  uint32_t state() const { std::lock_guard<std::mutex> g(lock); return config[kPmCap / 4 + 1] & 3; }
  const char* name() const override { return "fake function"; }
  bool initialize() override { return true; }
  void shutdown() override {}
  uint32_t device_count() const override { return 1; }
  bool pci_device(uint32_t, PciDeviceInfo*) const override { return false; }
  uint32_t read32(PciAddress, uint16_t offset) const override { std::lock_guard<std::mutex> g(lock); return offset < 256 ? config[offset / 4] : 0; }
  void write32(PciAddress, uint16_t offset, uint32_t value) const override {
    std::lock_guard<std::mutex> g(lock);
    if (offset >= 256 || offset < 4) return;
    if (offset == kPmCap + 4) {
      const uint32_t old_state = config[offset / 4] & 3, new_state = value & 3;
      config[offset / 4] = (config[offset / 4] & ~3u) | new_state;
      log.push_back(new_state == 3 ? "D3" : "D0");
      if (old_state == 3 && new_state == 0) {   // the header is gone
        config[1] &= 0xFFFF0000;
        for (int i = 4; i <= 9; ++i) config[i] = 0;
        config[15] = 0x00000100;
      }
      return;
    }
    if (offset == 0x04) { config[1] = (config[1] & 0xFFFF0000) | (value & 0xFFFF); return; }
    config[offset / 4] = value;
  }
  uint64_t memory_bar(PciAddress, uint8_t) const override { return 0; }
  uint16_t find_capability(PciAddress, uint8_t id) const override { return id == 1 ? kPmCap : 0; }
};

// The controller's registers, through a lock (the simulation is not thread safe; the hardware is), with a playback position that moves while
// the stream runs, the codec wiped by a link reset, and a count of accesses made while the function is asleep (in D3hot, or "cold": the
// observer has been told it is idle and has not been told it is busy).
struct Bench final : IHdaRegisters {
  FakeHda hda;
  FakeFunction& function;
  std::mutex lock;
  std::atomic<bool> cold{false};
  std::atomic<uint32_t> asleep_accesses{0};
  std::atomic<uint32_t> accesses{0};
  uint32_t position = 0;
  bool moving = true;           // the position advances on each read while the stream runs
  explicit Bench(FakeFunction& f) : function(f) {}

  void touch() {
    ++accesses;
    if (cold.load() || function.state() != 0) ++asleep_accesses;
  }
  uint8_t read8(uint32_t reg) override { std::lock_guard<std::mutex> g(lock); touch(); return hda.read8(reg); }
  uint16_t read16(uint32_t reg) override { std::lock_guard<std::mutex> g(lock); touch(); return hda.read16(reg); }
  uint32_t read32(uint32_t reg) override {
    std::lock_guard<std::mutex> g(lock);
    touch();
    if (reg == kOutputStream + 0x04) {
      if (moving && (hda.mem[kOutputStream] & 2)) position = (position + 4096) % IntelHdaAudioDevice::kRingBytes;
      return position;
    }
    return hda.read32(reg);
  }
  void write8(uint32_t reg, uint8_t v) override {
    std::lock_guard<std::mutex> g(lock);
    touch();
    if (reg == kOutputStream && (v & 1)) position = 0;   // (stream reset)
    hda.write8(reg, v);
  }
  void write16(uint32_t reg, uint16_t v) override { std::lock_guard<std::mutex> g(lock); touch(); hda.write16(reg, v); }
  void write32(uint32_t reg, uint32_t v) override {
    std::lock_guard<std::mutex> g(lock);
    touch();
    if (reg == 0x08 && !(v & 1)) wipe_codec();   // the link reset resets the codec
    hda.write32(reg, v);
  }
  bool link_down() { std::lock_guard<std::mutex> g(lock); return hda.in_reset; }
  void wipe_codec() {
    for (FakeHda::Pin& p : hda.pins) p = FakeHda::Pin();
    for (FakeHda::Converter& c : hda.converters) c = FakeHda::Converter();
    hda.mem[kOutputStream] = 0;
  }
};

// Records the calls, and checks at each one what state the function is in.
struct Observer final : IPciFunctionPowerObserver {
  Bench* bench = nullptr;
  FakeFunction* function = nullptr;
  std::mutex lock;
  std::vector<std::string> calls;
  std::atomic<int> balance{0};          // busy - idle: never below -1 or above 0
  std::atomic<uint32_t> wrong{0};
  void function_idle(PciAddress a) override {
    if (a.bus != kFunction.bus || a.function != kFunction.function) ++wrong;
    if (function && function->state() != 3) ++wrong;            // in D3hot before the owner may cut the power
    if (bench && !bench->link_down()) ++wrong;                    // (the link is down)
    if (--balance != -1) ++wrong;
    if (bench) bench->cold = true;                              // the owner cuts the power: from here no access may come
    std::lock_guard<std::mutex> g(lock);
    calls.push_back("idle");
  }
  void function_busy(PciAddress a) override {
    if (a.bus != kFunction.bus || a.function != kFunction.function) ++wrong;
    if (function && function->state() != 3) ++wrong;            // nothing has been done to the function yet
    if (++balance != 0) ++wrong;
    if (bench) bench->cold = false;
    std::lock_guard<std::mutex> g(lock);
    calls.push_back("busy");
  }
  std::vector<std::string> snapshot() { std::lock_guard<std::mutex> g(lock); return calls; }
};

struct Rig {
  TestAllocator allocator;
  FakeFunction function;
  Bench bench{function};
  Observer observer;
  IntelHdaAudioDevice dev{bench, allocator, function, kFunction};
  Rig() {
    observer.bench = &bench;
    observer.function = &function;
  }
};

static std::vector<uint8_t> tone(uint32_t bytes) {
  std::vector<uint8_t> pcm(bytes);
  for (uint32_t i = 0; i < bytes; ++i) pcm[i] = static_cast<uint8_t>(i * 7 + 1);
  return pcm;
}

static uint32_t write_tone(IntelHdaAudioDevice& dev, uint32_t bytes) {
  const std::vector<uint8_t> pcm = tone(bytes);
  return dev.write(ConstByteView(pcm.data(), pcm.size()));
}

// What a working output looks like in the simulation: the HDMI pin and its converter set up, the InfoFrame sent, the stream running.
static void check_programmed(Rig& r) {
  FakeHda& h = r.bench.hda;
  CHECK(!h.in_reset);
  CHECK(h.pins[0].control == 0x40 && h.pins[0].power == 0);
  CHECK(h.converters[0].power == 0 && h.converters[0].digital == 0x01 && h.converters[0].channel_count == 1);
  CHECK(h.converters[0].stream == (IntelHdaAudioDevice::kStreamTag << 4) && h.converters[0].format == 0x0011);
  CHECK(h.pins[0].dip.size() == 14 && h.pins[0].dip[0] == 0x84 && h.pins[0].dip_xmit == 0xC0);
  CHECK((h.mem[kOutputStream] & 2) != 0);                                     // running
  CHECK(h.load(kOutputStream + 0x08, 4) == IntelHdaAudioDevice::kRingBytes);   // cyclic buffer length
  CHECK(h.load(kOutputStream + 0x0C, 2) == IntelHdaAudioDevice::kEntries - 1);
  CHECK(h.load(kOutputStream + 0x12, 2) == 0x0011);
  CHECK(h.load(kOutputStream + 0x18, 4) != 0 || h.load(kOutputStream + 0x1C, 4) != 0);   // the buffer list
  CHECK((h.mem[kOutputStream + 2] >> 4) == IntelHdaAudioDevice::kStreamTag);
  CHECK(h.load(0x4C, 1) == 0x02 && (h.load(0x5C, 1) & 0x02));   // CORB and RIRB running
}

static void test_idle_suspend() {
  Rig r;
  CHECK(r.dev.initialize());
  check_programmed(r);
  CHECK(r.dev.idle_suspend_ms() == 0);   // off until an observer comes
  r.dev.service(0);
  r.dev.service(100000);
  CHECK(r.dev.power_state() == IntelHdaAudioDevice::PowerState::Active);

  r.dev.set_power_observer(&r.observer, 3000);
  CHECK(r.dev.idle_suspend_ms() == 3000);
  CHECK(r.observer.snapshot().empty());   // (active: nothing to say)
  r.dev.service(100000);    // the timer starts
  r.dev.service(102999);
  CHECK(r.dev.power_state() == IntelHdaAudioDevice::PowerState::Active);
  // Sound resets the timer, and while it is queued the device does not suspend.
  r.bench.moving = false;
  CHECK(write_tone(r.dev, 8192) == 8192);
  r.dev.service(103000);
  r.dev.service(200000);
  CHECK(r.dev.power_state() == IntelHdaAudioDevice::PowerState::Active);   // still queued
  CHECK(!r.dev.suspend());
  CHECK(r.dev.queued_bytes() == 8192);
  r.bench.moving = true;
  CHECK(r.dev.queued_bytes() == 8192 - 4096);   // (one read moves it 4096)
  CHECK(r.dev.queued_bytes() == 0);
  r.dev.service(200001);
  CHECK(r.dev.power_state() == IntelHdaAudioDevice::PowerState::Suspended);   // idle for long enough, nothing queued
  CHECK(r.dev.suspend_count() == 1);

  // What it left behind: the stream stopped, the codec's function group in D3, the rings stopped, the link in reset, the function in D3hot.
  FakeHda& h = r.bench.hda;
  CHECK(!(h.mem[kOutputStream] & 2));
  CHECK(h.in_reset);
  CHECK(h.load(0x4C, 1) == 0 && h.load(0x5C, 1) == 0);
  CHECK(r.function.state() == 3);
  bool fg_d3 = false;
  for (const FakeHda::Verb& v : h.log)
    if (v.nid == FakeHda::kFunctionGroup && v.command == 0x70503) fg_d3 = true;
  CHECK(fg_d3);
  CHECK(r.observer.snapshot() == std::vector<std::string>{"idle"});

  // Suspended: the playback path answers from memory and leaves the hardware alone.
  const uint32_t before = r.bench.accesses.load();
  CHECK(r.dev.queued_bytes() == 0);
  CHECK(r.dev.free_bytes() == IntelHdaAudioDevice::kRingBytes);
  r.dev.set_volume(40);
  r.dev.set_muted(true);
  r.dev.set_muted(false);
  (void)r.dev.statistics();
  CHECK(r.dev.write(ConstByteView()) == 0);   // (no sound: no wake)
  CHECK(!r.dev.retarget());                   // (the resume finds the pin again)
  r.dev.service(300000);
  r.dev.service(900000);
  CHECK(r.bench.accesses.load() == before);
  CHECK(r.dev.power_state() == IntelHdaAudioDevice::PowerState::Suspended);
  CHECK(r.dev.suspend());   // already: true, nothing done
  CHECK(r.dev.suspend_count() == 1);
  CHECK(r.bench.asleep_accesses.load() == 0);
  CHECK(r.observer.wrong.load() == 0);
  r.dev.shutdown();
}

static void test_resume_on_open() {
  Rig r;
  CHECK(r.dev.initialize());
  r.dev.set_power_observer(&r.observer, 1000);
  CHECK(r.dev.suspend());
  CHECK(r.observer.snapshot() == std::vector<std::string>{"idle"});
  CHECK(r.bench.hda.pins[0].control == 0 && r.bench.hda.converters[0].stream == 0);   // (the link reset wiped the codec)
  CHECK(r.function.config[4] == 0xFE000004);   // the header is still there in D3hot ...

  // A stream opens: the first write with sound wakes the device, and is taken.
  r.bench.hda.log.clear();
  CHECK(write_tone(r.dev, 4096) == 4096);
  CHECK(r.dev.power_state() == IntelHdaAudioDevice::PowerState::Active);
  CHECK(r.dev.resume_count() == 1);
  CHECK(r.observer.snapshot() == (std::vector<std::string>{"idle", "busy"}));
  CHECK(r.function.state() == 0);
  // ... and written back after D0 lost it (BARs, interrupt line, command last).
  CHECK(r.function.config[4] == 0xFE000004 && r.function.config[5] == 0x00000001 && r.function.config[15] == 0x0000010B);
  CHECK((r.function.config[1] & 0xFFFF) == 0x0006);
  CHECK((r.function.log == std::vector<std::string>{"D3", "D0"}));
  check_programmed(r);   // the codec found and programmed again, the stream running
  bool rediscovered = false;
  for (const FakeHda::Verb& v : r.bench.hda.log)
    if (v.nid == FakeHda::kRoot && v.command == 0xF0004) rediscovered = true;
  CHECK(rediscovered);
  CHECK(r.dev.queued_bytes() > 0 || r.bench.position != 0);
  CHECK(r.bench.asleep_accesses.load() == 0);

  // Again: it goes idle after the period, and comes back.
  r.dev.service(0);
  CHECK(r.dev.queued_bytes() == 0);
  r.dev.service(500);
  CHECK(r.dev.power_state() == IntelHdaAudioDevice::PowerState::Active);
  r.dev.service(1000);   // (the activity was seen at 0: idle since)
  CHECK(r.dev.power_state() == IntelHdaAudioDevice::PowerState::Suspended);
  CHECK(r.dev.resume());
  CHECK(r.dev.resume());   // (already)
  CHECK(r.dev.resume_count() == 2);
  check_programmed(r);
  CHECK(r.observer.snapshot() == (std::vector<std::string>{"idle", "busy", "idle", "busy"}));
  CHECK(r.observer.wrong.load() == 0);
  CHECK(r.bench.asleep_accesses.load() == 0);

  // An observer set while the device sleeps learns that at once; null detaches it.
  CHECK(r.dev.suspend());
  Observer late;
  late.function = &r.function;
  late.balance = 0;
  r.dev.set_power_observer(&late);
  CHECK(late.snapshot() == std::vector<std::string>{"idle"});
  r.dev.set_power_observer(nullptr);
  r.bench.cold = false;
  CHECK(r.dev.resume());
  CHECK(late.snapshot() == std::vector<std::string>{"idle"});

  // A shutdown of a sleeping device wakes the function first (for the firmware).
  r.observer.balance = 0;
  r.dev.set_power_observer(&r.observer);
  CHECK(r.dev.suspend());
  r.dev.shutdown();
  CHECK(r.function.state() == 0);
  CHECK(r.observer.snapshot().back() == "busy");
  CHECK(r.bench.asleep_accesses.load() == 0);
}

static void test_refusals_and_failure() {
  {
    Rig r;
    CHECK(!r.dev.suspend());   // not initialized
    CHECK(r.dev.resume());     // (not suspended)
  }
  {
    // The codec does not come back: the device fails, plays nothing, and lets the function go again.
    Rig r;
    CHECK(r.dev.initialize());
    r.dev.set_power_observer(&r.observer);
    CHECK(r.dev.suspend());
    r.bench.hda.no_codec = true;
    CHECK(write_tone(r.dev, 4096) == 0);
    CHECK(r.dev.power_state() == IntelHdaAudioDevice::PowerState::Failed);
    CHECK(r.observer.snapshot() == (std::vector<std::string>{"idle", "busy", "idle"}));
    CHECK(!r.dev.resume());
    CHECK(write_tone(r.dev, 4096) == 0);
    CHECK(r.dev.statistics().write_failures >= 2);
    CHECK(r.observer.wrong.load() == 0);
    r.dev.shutdown();
  }
  {
    // No PCI function (a controller given by its registers alone): the link reset is the whole suspend.
    TestAllocator allocator;
    FakeHda hda;
    IntelHdaAudioDevice dev(hda, allocator);
    CHECK(dev.initialize());
    CHECK(dev.suspend());
    CHECK(hda.in_reset);
    CHECK(dev.resume());
    CHECK(!hda.in_reset && (hda.mem[kOutputStream] & 2));
    dev.shutdown();
  }
}

// Writers (the mixer), a volume changer and the idle policy on their own threads, and a thread forcing suspends and resumes: the registers
// are never touched while the function sleeps, the observer's calls alternate, and everything ends in a consistent state.
static void test_threads() {
  Rig r;
  CHECK(r.dev.initialize());
  r.dev.set_power_observer(&r.observer, 2);
  std::atomic<bool> stop{false};
  std::atomic<uint64_t> clock{0};
  std::atomic<uint64_t> written{0};
  std::vector<std::thread> threads;
  for (int w = 0; w < 2; ++w)
    threads.emplace_back([&, w] {
      const std::vector<uint8_t> pcm = tone(1024);
      for (int i = 0; !stop.load(); ++i) {
        if ((i + w) % 3 == 0) written += r.dev.write(ConstByteView(pcm.data(), pcm.size()));
        (void)r.dev.queued_bytes();
        (void)r.dev.free_bytes();
        std::this_thread::yield();
      }
    });
  threads.emplace_back([&] {
    for (uint8_t v = 0; !stop.load(); ++v) {
      r.dev.set_volume(v % 100);
      r.dev.set_muted(v & 1);
      (void)r.dev.statistics();
    }
  });
  threads.emplace_back([&] {
    while (!stop.load()) r.dev.service(clock += 1);
  });
  threads.emplace_back([&] {
    for (int i = 0; !stop.load(); ++i) {
      if (i & 1) (void)r.dev.suspend();
      else (void)r.dev.resume();
      (void)r.dev.retarget();
      if (i % 5 == 0) (void)r.dev.connections_changed();   // (a display came: GpuHotplug)
    }
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(700));
  stop = true;
  for (std::thread& t : threads) t.join();

  printf("  threads: %u suspends, %u resumes, %llu bytes written\n", r.dev.suspend_count(), r.dev.resume_count(),
         static_cast<unsigned long long>(written.load()));
  CHECK(r.dev.suspend_count() > 0 && r.dev.resume_count() > 0);
  CHECK(written.load() > 0);
  CHECK(r.bench.asleep_accesses.load() == 0);
  CHECK(r.observer.wrong.load() == 0);
  CHECK(r.dev.power_state() != IntelHdaAudioDevice::PowerState::Failed);
  // The calls alternate, idle first.
  const std::vector<std::string> calls = r.observer.snapshot();
  for (size_t i = 0; i < calls.size(); ++i) CHECK(calls[i] == (i % 2 == 0 ? "idle" : "busy"));
  CHECK(calls.size() == r.dev.suspend_count() + r.dev.resume_count());
  // And it still works.
  CHECK(r.dev.resume());
  r.bench.moving = false;
  CHECK(r.dev.write(ConstByteView(tone(4096).data(), 4096)) > 0 || r.dev.free_bytes() == 0);
  check_programmed(r);
  r.dev.shutdown();
}

// A display plugged into the GPU while its HD audio slept (GpuHotplug -> SystemAudioDevices::connections_changed): the controller reads its
// pins again and moves the output to the pin that now has a display with a valid ELD. Running, it retargets; suspended, it is resumed (which
// rediscovers the codec) and sleeps again after the idle period; failed, it does nothing.
static void test_connections_changed() {
  Rig r;
  std::atomic<uint32_t> display_pin{0xFF};   // the pin index (0..2) with a monitor behind it, 0xFF: none
  r.bench.hda.sense = [&](uint32_t i) { return i == display_pin.load() ? 0xC0000000u : 0u; };
  CHECK(r.dev.initialize());
  CHECK(r.dev.output_pin() == FakeHda::kPins[0] && !r.dev.output_has_display());   // (no display anywhere: the first digital pin)
  CHECK(r.dev.connection_changes() == 0);

  // Running: a monitor on the second pin.
  display_pin = 1;
  r.bench.hda.log.clear();
  CHECK(r.dev.connections_changed());
  CHECK(r.dev.output_pin() == FakeHda::kPins[1] && r.dev.output_has_display());
  bool sensed = false;
  for (const FakeHda::Verb& v : r.bench.hda.log)
    if (v.nid == FakeHda::kPins[1] && (v.command >> 8) == 0xF09) sensed = true;
  CHECK(sensed);
  CHECK(r.bench.hda.pins[1].control == 0x40 && r.bench.hda.pins[0].control == 0);   // the old pin silenced, the new one driven
  CHECK(r.dev.connection_changes() == 1);

  // Suspended: the monitor moves to the third pin while the controller (and the GPU) sleep; the change resumes it, it finds the new pin, and
  // the idle policy puts it back to sleep.
  r.dev.set_power_observer(&r.observer, 1000);
  CHECK(r.dev.suspend());
  display_pin = 2;
  CHECK(r.dev.connections_changed());
  CHECK(r.dev.power_state() == IntelHdaAudioDevice::PowerState::Active && r.dev.resume_count() == 1);
  CHECK(r.dev.output_pin() == FakeHda::kPins[2] && r.dev.output_has_display());
  CHECK(r.observer.snapshot() == (std::vector<std::string>{"idle", "busy"}));
  CHECK(r.dev.connection_changes() == 2);
  r.dev.service(0);
  r.dev.service(1000);
  CHECK(r.dev.power_state() == IntelHdaAudioDevice::PowerState::Suspended);
  CHECK(r.observer.snapshot() == (std::vector<std::string>{"idle", "busy", "idle"}));

  // The monitor is pulled out: back to the first digital pin, without a display.
  display_pin = 0xFF;
  CHECK(r.dev.connections_changed());
  CHECK(r.dev.output_pin() == FakeHda::kPins[0] && !r.dev.output_has_display());
  CHECK(r.bench.asleep_accesses.load() == 0 && r.observer.wrong.load() == 0);

  // Failed: nothing is touched.
  CHECK(r.dev.suspend());
  r.bench.hda.no_codec = true;
  CHECK(!r.dev.connections_changed());
  CHECK(r.dev.power_state() == IntelHdaAudioDevice::PowerState::Failed);
  const uint32_t before = r.bench.accesses.load();
  CHECK(!r.dev.connections_changed());
  CHECK(r.bench.accesses.load() == before);
  CHECK(r.dev.connection_changes() == 3);
  r.dev.shutdown();

  // Never started: false.
  Rig idle;
  CHECK(!idle.dev.connections_changed());
}

int main() {
  test_idle_suspend();
  test_resume_on_open();
  test_refusals_and_failure();
  test_connections_changed();
  test_threads();
  if (g_failed.load()) {
    printf("HdaRuntimePmTest: %d FAILED\n", g_failed.load());
    return 1;
  }
  printf("HdaRuntimePmTest: all passed\n");
  return 0;
}
