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

// The display mux switch (Multi/GpuMuxSwitch): the vga_switcheroo order (lease, DDC, light, mux, release), every failing step undone in
// reverse, the refusals (no mux answer needed, no driver, no panel output, a firmware framebuffer that cannot light the panel again, an
// earlier rollback that failed), the GOP side, the real GpuManager waking and sleeping the discrete GPU through the lease, and several
// threads switching at once (only one in flight, the others Busy, the mux never seen half-moved). The Intel integrated GPU's eDP panel output
// (IntelPanelOutput) against a model of its registers: what the firmware left found, let go and lit again in i915's order with the panel's waits,
// each failing step leaving nothing on, and the switch back to it that the firmware framebuffer refuses. GpuMuxDesktop: the panel's monitor
// moving with the panel between the GPUs' monitors (fake drivers), and the panel switched back when the monitors cannot follow.
#include <stdio.h>
#include <stdlib.h>
#include <map>
#include <string.h>
#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "Hardware/GraphicsProcessingUnit/Multi/GpuManager.hpp"
#include "Hardware/GraphicsProcessingUnit/Multi/GpuMuxSwitch.hpp"
#include "Hardware/GraphicsProcessingUnit/Intel/IntelGpu.hpp"
#include "FakeMonitors.hpp"

using namespace hardware;

static int g_failed = 0, g_checks = 0;
#define CHECK(...) do { ++g_checks; if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)

namespace {

constexpr GpuMuxTarget kI = GpuMuxTarget::Integrated, kD = GpuMuxTarget::Discrete;

// Everything the fakes do, in order: "lease+1", "ddc:D", "light:D", "mux:D", "release:I", "lease-0", ...
struct Log {
  std::mutex m;
  std::vector<std::string> events;
  void add(const std::string& e) { std::lock_guard<std::mutex> g(m); events.push_back(e); }
  std::string joined() {
    std::lock_guard<std::mutex> g(m);
    std::string s;
    for (const std::string& e : events) s += (s.empty() ? "" : " ") + e;
    return s;
  }
  void clear() { std::lock_guard<std::mutex> g(m); events.clear(); }
};
const char* side(GpuMuxTarget t) { return t == kD ? "D" : "I"; }

struct FakeMux final : IGpuMux {
  Log& log;
  bool can_tell = false, can_ddc = true, fail_switch = false, fail_switch_back = false, fail_ddc_back = false;
  GpuMuxTarget display = kI, ddc = kI;
  std::atomic<uint32_t> switches{0};
  explicit FakeMux(Log& l) : log(l) {}
  bool active(GpuMuxTarget* out) override {
    if (!can_tell) return false;
    *out = display;
    return true;
  }
  bool switch_to(GpuMuxTarget t) override {
    log.add(std::string("mux:") + side(t));
    if (fail_switch && t != display) {
      fail_switch = false;                 // (fails once: the switch back works unless fail_switch_back)
      if (fail_switch_back) fail_switch = true;
      return false;
    }
    if (fail_switch_back && t == kI && display == kI) return false;
    display = t;
    ddc = t;                               // (moving the display moves DDC with it, as gmux and ATPX do)
    ++switches;
    return true;
  }
  bool switch_ddc(GpuMuxTarget t) override {
    if (!can_ddc) return false;
    if (fail_ddc_back && t != ddc && ddc != display) {
      log.add(std::string("ddc-fail:") + side(t));
      return false;
    }
    log.add(std::string("ddc:") + side(t));
    ddc = t;
    return true;
  }
};

struct FakeOutput final : IPanelOutput {
  Log& log;
  GpuMuxTarget where;
  FakeMux* mux = nullptr;                  // to check that the DDC lines point here when the panel's EDID is read
  bool has = true, can = true, lit = false, relight = true;
  PanelLightStatus fail = PanelLightStatus::Ok;
  bool saw_ddc = false;
  uint32_t lights = 0, releases = 0;
  const char* status = "ok";
  FakeOutput(Log& l, GpuMuxTarget w, bool lit_now = false) : log(l), where(w), lit(lit_now) {}
  bool has_panel_output() const override { return has; }
  bool panel_lit() const override { return lit; }
  bool can_light_panel() const override { return can && (relight || lit); }
  PanelLightStatus light_panel() override {
    ++lights;
    log.add(std::string("light:") + side(where));
    if (mux) saw_ddc = mux->ddc == where;
    if (fail != PanelLightStatus::Ok) {
      status = "the fake failed";
      return fail;
    }
    lit = true;
    return PanelLightStatus::Ok;
  }
  void release_panel() override {
    ++releases;
    log.add(std::string("release:") + side(where));
    lit = false;
  }
  const char* panel_status() const override { return status; }
};

struct FakeLeases final : IGpuLeases {
  Log& log;
  bool fail = false;
  std::atomic<int> held[2] = {{0}, {0}};
  explicit FakeLeases(Log& l) : log(l) {}
  bool acquire(uint32_t index) override {
    if (index > 1) return false;
    if (fail) {
      log.add("lease-fail+" + std::to_string(index));
      return false;
    }
    log.add("lease+" + std::to_string(index));
    ++held[index];
    return true;
  }
  void release(uint32_t index) override {
    log.add("lease-" + std::to_string(index));
    if (index <= 1) --held[index];
  }
};

// A laptop: the integrated GPU (index 0) shows the panel at boot, the discrete one (index 1) can take it.
struct Rig {
  Log log;
  FakeMux mux{log};
  FakeOutput igpu{log, kI, true};
  FakeOutput dgpu{log, kD};
  FakeLeases leases{log};
  GpuMuxSwitch sw{mux, &leases, 0, 1};
  Rig() {
    igpu.mux = &mux;
    dgpu.mux = &mux;
    sw.attach_output(kI, &igpu);
    sw.attach_output(kD, &dgpu);
  }
};

}  // namespace

static void test_success() {
  Rig r;
  GpuMuxTarget at = kD;
  CHECK(r.sw.active(&at) && at == kI);
  const char* why = nullptr;
  CHECK(r.sw.can_switch_to(kD, &why) && why && !*why);
  const GpuMuxSwitchReport rep = r.sw.switch_to(kD);
  CHECK(rep.status == GpuMuxSwitchStatus::Switched && rep.ok() && rep.from == kI && rep.to == kD);
  CHECK(rep.woke && rep.ddc_switched && rep.mux_switched && !rep.rolled_back && rep.rollback_clean);
  // vga_switcheroo's order: wake, DDC, light, mux, release the old one
  CHECK(r.log.joined() == "lease+1 ddc:D light:D mux:D release:I");
  CHECK(r.dgpu.saw_ddc && r.dgpu.lit && !r.igpu.lit && r.mux.display == kD);
  CHECK(r.sw.active(&at) && at == kD && r.sw.switches() == 1 && r.sw.holds_lease(kD) && !r.sw.holds_lease(kI) && r.leases.held[1] == 1);
  // Again to the same side: nothing happens.
  r.log.clear();
  CHECK(r.sw.switch_to(kD).status == GpuMuxSwitchStatus::AlreadyActive && r.log.joined().empty());
  CHECK(!r.sw.can_switch_to(kD, &why) && strstr(why, "already"));
  // And back: the integrated side leased (index 0), the discrete one released and its lease given back (it may sleep).
  CHECK(r.sw.switch_to(kI).status == GpuMuxSwitchStatus::Switched);
  CHECK(r.log.joined() == "lease+0 ddc:I light:I mux:I release:D lease-1");
  CHECK(r.leases.held[1] == 0 && r.leases.held[0] == 1 && r.sw.switches() == 2 && r.mux.display == kI);
  // A mux that cannot move DDC alone (retina gmux, NVIDIA's _DSM): the panel's EDID comes once the display moved; nothing else differs.
  Rig n;
  n.mux.can_ddc = false;
  const GpuMuxSwitchReport b = n.sw.switch_to(kD);
  CHECK(b.ok() && !b.ddc_switched && n.log.joined() == "lease+1 light:D mux:D release:I");
  // No leases (a machine whose GPUs never sleep): the same without them.
  Log log;
  FakeMux mux(log);
  FakeOutput i(log, kI, true), d(log, kD);
  GpuMuxSwitch none(mux, nullptr, 0, 1);
  none.attach_output(kI, &i);
  none.attach_output(kD, &d);
  CHECK(none.switch_to(kD).ok() && !none.holds_lease(kD) && log.joined() == "ddc:D light:D mux:D release:I");
}

// Each failing step, undone in reverse order.
static void test_failures() {
  {
    Rig r;                                                         // the GPU does not wake: nothing else is tried
    r.leases.fail = true;
    const GpuMuxSwitchReport rep = r.sw.switch_to(kD);
    CHECK(rep.status == GpuMuxSwitchStatus::WakeFailed && !rep.ok() && r.log.joined() == "lease-fail+1");
    CHECK(r.igpu.lit && !r.dgpu.lit && r.mux.display == kI && !r.sw.holds_lease(kD));
  }
  {
    Rig r;                                                         // the target cannot light the panel: released, DDC back, lease back
    r.dgpu.fail = PanelLightStatus::LinkTraining;
    const GpuMuxSwitchReport rep = r.sw.switch_to(kD);
    CHECK(rep.status == GpuMuxSwitchStatus::LightFailed && rep.light == PanelLightStatus::LinkTraining && rep.rolled_back && rep.rollback_clean);
    CHECK(r.log.joined() == "lease+1 ddc:D light:D release:D ddc:I lease-1");
    CHECK(strcmp(rep.reason, "the fake failed") == 0 && r.igpu.lit && r.igpu.releases == 0 && r.mux.ddc == kI && r.leases.held[1] == 0);
    GpuMuxTarget at = kD;
    CHECK(r.sw.active(&at) && at == kI && r.sw.switches() == 0);
    r.dgpu.fail = PanelLightStatus::Ok;                            // the next attempt works
    CHECK(r.sw.switch_to(kD).ok() && r.leases.held[1] == 1);
  }
  {
    Rig r;                                                         // the mux will not move: mux back, release, DDC back, lease back
    r.mux.fail_switch = true;
    const GpuMuxSwitchReport rep = r.sw.switch_to(kD);
    CHECK(rep.status == GpuMuxSwitchStatus::MuxFailed && rep.rolled_back && rep.rollback_clean && !rep.mux_switched);
    CHECK(r.log.joined() == "lease+1 ddc:D light:D mux:D mux:I release:D ddc:I lease-1");
    CHECK(r.igpu.lit && !r.dgpu.lit && r.mux.display == kI && r.leases.held[1] == 0);
  }
  {
    Rig r;                                                         // ... and will not move back either: the state is unknown
    r.mux.fail_switch = true;
    r.mux.fail_switch_back = true;
    const GpuMuxSwitchReport rep = r.sw.switch_to(kD);
    CHECK(rep.status == GpuMuxSwitchStatus::MuxFailed && rep.rolled_back && !rep.rollback_clean);
    GpuMuxTarget at;
    CHECK(!r.sw.active(&at));
    const char* why = nullptr;
    CHECK(!r.sw.can_switch_to(kD, &why) && strstr(why, "not known"));
    r.mux.fail_switch = r.mux.fail_switch_back = false;
    CHECK(r.sw.switch_to(kD).status == GpuMuxSwitchStatus::StateUnknown);
    r.sw.set_known_active(kI);                                     // someone checked
    CHECK(r.sw.switch_to(kD).ok());
  }
  {
    Rig r;                                                         // a mux that can say where the panel is settles an unknown state itself
    r.mux.fail_switch = true;
    r.mux.fail_switch_back = true;
    CHECK(!r.sw.switch_to(kD).rollback_clean);
    r.mux.fail_switch = r.mux.fail_switch_back = false;
    r.mux.can_tell = true;
    CHECK(r.sw.switch_to(kD).ok());
    GpuMuxTarget at = kI;
    CHECK(r.sw.active(&at) && at == kD);
  }
  {
    Rig r;                                                         // the DDC lines will not move back after a failed light
    r.dgpu.fail = PanelLightStatus::NoEdid;
    r.mux.fail_ddc_back = true;
    const GpuMuxSwitchReport rep = r.sw.switch_to(kD);
    CHECK(rep.status == GpuMuxSwitchStatus::LightFailed && rep.rolled_back && !rep.rollback_clean && r.leases.held[1] == 0);
    GpuMuxTarget at;
    CHECK(!r.sw.active(&at));
  }
}

// Refused before anything is touched.
static void test_refusals() {
  {
    Rig r;
    r.sw.attach_output(kD, nullptr);                               // no driver on the discrete GPU
    const GpuMuxSwitchReport rep = r.sw.switch_to(kD);
    CHECK(rep.status == GpuMuxSwitchStatus::NoOutput && r.log.joined().empty() && r.leases.held[1] == 0);
    const char* why = nullptr;
    CHECK(!r.sw.can_switch_to(kD, &why) && strstr(why, "discrete"));
  }
  {
    Rig r;
    r.sw.attach_output(kI, nullptr);                               // nobody to let the panel go
    CHECK(r.sw.switch_to(kD).status == GpuMuxSwitchStatus::NoOutput && r.log.joined().empty());
  }
  {
    Rig r;
    r.dgpu.has = false;                                            // a discrete GPU whose tables list no panel output
    CHECK(r.sw.switch_to(kD).status == GpuMuxSwitchStatus::NoPanelOutput && r.log.joined().empty());
  }
  {
    Rig r;
    r.dgpu.can = false;                                            // its driver cannot light it (busy with an external monitor, say)
    r.dgpu.status = "an external monitor is driven";
    const GpuMuxSwitchReport rep = r.sw.switch_to(kD);
    CHECK(rep.status == GpuMuxSwitchStatus::CannotLight && strcmp(rep.reason, "an external monitor is driven") == 0 && r.log.joined().empty());
  }
  {
    Rig r;                                                         // the mux's own answer wins: it says the panel is on the discrete GPU already
    r.mux.can_tell = true;
    r.mux.display = kD;
    r.dgpu.lit = true;
    CHECK(r.sw.switch_to(kD).status == GpuMuxSwitchStatus::AlreadyActive && r.log.joined().empty());
  }
  {
    Rig r;                                                         // on the right side but unlit: lit, no mux move, nothing released
    r.mux.can_tell = true;
    r.mux.display = kD;
    const GpuMuxSwitchReport rep = r.sw.switch_to(kD);
    CHECK(rep.ok() && !rep.mux_switched && !rep.ddc_switched && r.log.joined() == "lease+1 light:D");
  }
  CHECK(strcmp(gpu_mux_switch_status_name(GpuMuxSwitchStatus::CannotLight), "that GPU's driver cannot light the panel") == 0);
  CHECK(strcmp(panel_light_status_name(PanelLightStatus::LinkTraining), "the panel's link did not train") == 0);
}

// The integrated GPU as the firmware's framebuffer: away is fine, back is refused and says why; nothing is touched by the refusal.
static void test_gop_side() {
  Log log;
  FakeMux mux(log);
  GopPanelOutput gop(true);
  FakeOutput d(log, kD);
  FakeLeases leases(log);
  GpuMuxSwitch sw(mux, &leases, 0, 1);
  sw.attach_output(kI, &gop);
  sw.attach_output(kD, &d);
  CHECK(gop.panel_lit() && gop.can_light_panel() && gop.light_panel() == PanelLightStatus::Ok);
  CHECK(sw.switch_to(kD).ok() && !gop.panel_lit() && !gop.can_light_panel());
  log.clear();
  const char* why = nullptr;
  CHECK(!sw.can_switch_to(kI, &why) && strstr(why, "until the computer restarts"));
  const GpuMuxSwitchReport back = sw.switch_to(kI);
  CHECK(back.status == GpuMuxSwitchStatus::CannotLight && strstr(back.reason, "firmware") && log.joined().empty());
  CHECK(d.lit && mux.display == kD && leases.held[1] == 1);
  CHECK(gop.light_panel() == PanelLightStatus::NotSupported);
  // The firmware did not show its picture on the panel (the mux pointed at the discrete GPU at boot): nothing to take back from.
  GopPanelOutput dark(false);
  CHECK(!dark.panel_lit() && !dark.can_light_panel());
}

// --- The real GpuManager: the lease wakes the discrete GPU and gives it back -------------------------------------------------------------
namespace {
constexpr uint16_t kPm = 0x50;
class TwoGpus final : public IPciBus {
 public:
  struct F {
    PciDeviceInfo info;
    uint32_t config[64] = {};
  };
  F f[2];
  mutable std::mutex m;
  TwoGpus() {
    f[0].info = {{0, 2, 0}, 0x8086, 0x9A49, 0x03, 0x00, 0};   // Tiger Lake Xe: the boot display (decoding on)
    f[1].info = {{1, 0, 0}, 0x10DE, 0x1F95, 0x03, 0x00, 0};   // a Turing laptop GPU
    for (F& x : f) {
      x.config[0] = x.info.vendor_id | uint32_t{x.info.device_id} << 16;
      x.config[2] = uint32_t{x.info.class_code} << 24;
      x.config[0x34 / 4] = kPm;
      x.config[kPm / 4] = 0x01;
    }
    f[0].config[1] = 0x7;
  }
  const char* name() const override { return "two gpus"; }
  bool initialize() override { return true; }
  void shutdown() override {}
  uint32_t device_count() const override { return 2; }
  bool pci_device(uint32_t i, PciDeviceInfo* out) const override {
    if (i >= 2) return false;
    *out = f[i].info;
    return true;
  }
  const F* find(PciAddress a) const {
    for (const F& x : f)
      if (x.info.address.bus == a.bus && x.info.address.device == a.device && x.info.address.function == a.function) return &x;
    return nullptr;
  }
  uint32_t read32(PciAddress a, uint16_t off) const override {
    std::lock_guard<std::mutex> g(m);
    const F* x = find(a);
    return x && off < 256 ? x->config[off / 4] : 0xFFFFFFFFu;
  }
  void write32(PciAddress a, uint16_t off, uint32_t v) const override {
    std::lock_guard<std::mutex> g(m);
    F* x = const_cast<F*>(find(a));   // (the fake's own storage; IPciBus's writes are const)
    if (x && off >= 0x04 && off < 256 && off != 0x08 && off != 0x34) x->config[off / 4] = v;
  }
  uint64_t memory_bar(PciAddress, uint8_t) const override { return 0; }
  uint16_t find_capability(PciAddress a, uint8_t id) const override { return find(a) && id == 0x01 ? kPm : 0; }
  uint32_t pm_state(uint32_t i) const { return read32(f[i].info.address, kPm + 4) & 3; }
};
}  // namespace

static void test_real_manager() {
  TwoGpus pci;
  GpuManager manager(pci);
  manager.initialize();
  CHECK(manager.count() == 2 && manager.display_index() == 0 && manager.asleep(1) && pci.pm_state(1) == 3);
  GpuManagerLeases leases(manager);
  Log log;
  FakeMux mux(log);
  FakeOutput i(log, kI, true), d(log, kD);
  {
    GpuMuxSwitch sw(mux, &leases, 0, 1);
    sw.attach_output(kI, &i);
    sw.attach_output(kD, &d);
    CHECK(sw.switch_to(kD).ok());
    CHECK(!manager.asleep(1) && pci.pm_state(1) == 0 && manager.leases(1) == 1 && manager.wake_count(1) == 1);
    CHECK(sw.switch_to(kI).ok());
    CHECK(manager.asleep(1) && pci.pm_state(1) == 3 && manager.leases(1) == 0 && manager.leases(0) == 1);
    CHECK(sw.switch_to(kD).ok() && manager.wake_count(1) == 2);
    GpuMuxSwitch::publish(&sw);
    CHECK(GpuMuxSwitch::published() == &sw);
  }
  // The switcher's destructor gave its leases back (the discrete GPU sleeps again) and withdrew itself.
  CHECK(manager.asleep(1) && manager.leases(1) == 0 && manager.leases(0) == 0 && GpuMuxSwitch::published() == nullptr);
}

// Several threads switching back and forth: one switch at a time (the others told Busy, never blocked), the outputs and the mux always
// agree once a switch is over, and every lease is given back in the end.
namespace {
struct SlowOutput final : IPanelOutput {
  GpuMuxTarget where;
  std::atomic<bool> lit{false};
  std::atomic<int>* inside;
  std::atomic<int>* overlaps;
  SlowOutput(GpuMuxTarget w, bool l, std::atomic<int>* in, std::atomic<int>* ov) : where(w), lit(l), inside(in), overlaps(ov) {}
  bool has_panel_output() const override { return true; }
  bool panel_lit() const override { return lit; }
  bool can_light_panel() const override { return true; }
  PanelLightStatus light_panel() override {
    if (inside->fetch_add(1) != 0) ++*overlaps;
    std::this_thread::sleep_for(std::chrono::microseconds(200));
    lit = true;
    inside->fetch_sub(1);
    return PanelLightStatus::Ok;
  }
  void release_panel() override { lit = false; }
  const char* panel_status() const override { return "slow"; }
};
struct CountingLeases final : IGpuLeases {
  std::atomic<int> held[2] = {{0}, {0}};
  bool acquire(uint32_t i) override { ++held[i]; return true; }
  void release(uint32_t i) override { --held[i]; }
};
struct AtomicMux final : IGpuMux {
  std::atomic<int> display{0};
  bool active(GpuMuxTarget*) override { return false; }
  bool switch_to(GpuMuxTarget t) override { display = t == kD ? 1 : 0; return true; }
};
}  // namespace

static void test_threads() {
  std::atomic<int> inside{0}, overlaps{0};
  SlowOutput i(kI, true, &inside, &overlaps), d(kD, false, &inside, &overlaps);
  CountingLeases leases;
  AtomicMux mux;
  std::atomic<int> switched{0}, busy{0}, other{0}, bad{0};
  {
    GpuMuxSwitch sw(mux, &leases, 0, 1);
    sw.attach_output(kI, &i);
    sw.attach_output(kD, &d);
    std::vector<std::thread> threads;
    for (int t = 0; t < 6; ++t)
      threads.emplace_back([&, t] {
        for (int k = 0; k < 200; ++k) {
          const GpuMuxTarget target = (t + k) % 2 ? kD : kI;
          const GpuMuxSwitchReport r = sw.switch_to(target);
          if (r.status == GpuMuxSwitchStatus::Switched) ++switched;
          else if (r.status == GpuMuxSwitchStatus::Busy) ++busy;
          else if (r.status != GpuMuxSwitchStatus::AlreadyActive) ++other;
          const char* why = nullptr;
          (void)sw.can_switch_to(target, &why);
          GpuMuxTarget at;
          if (!sw.active(&at)) ++bad;
        }
      });
    for (std::thread& t : threads) t.join();
    GpuMuxTarget at = kI;
    CHECK(sw.active(&at));
    // Afterwards exactly one side is lit, the mux points at it, and the switcher holds a lease on it alone.
    CHECK((at == kD) == d.lit.load() && (at == kI) == i.lit.load() && mux.display == (at == kD ? 1 : 0));
    CHECK(leases.held[at == kD ? 1 : 0] == 1 && leases.held[at == kD ? 0 : 1] == 0);
    CHECK(sw.switches() == static_cast<uint32_t>(switched.load()) && switched > 0 && !sw.in_flight());
  }
  CHECK(overlaps == 0 && other == 0 && bad == 0);
  CHECK(leases.held[0] == 0 && leases.held[1] == 0);
  printf("  threads: %d switched, %d busy\n", switched.load(), busy.load());
}

// --- The Intel integrated GPU's eDP panel (IntelPanelOutput) ------------------------------------------------------------------------------

namespace {

using P = IntelPanelOutput;

// The registers of an Intel display engine with the firmware's eDP panel lit on pipe A, and the parts of them that act: the panel power
// sequencer (on and off take a while, and after off the power cycle time runs), the DDI buffer's idle bit, the transcoder's state bit.
struct IntelRegs final : intel_gt::IRegisterBank {
  std::map<uint64_t, uint32_t> r;
  std::vector<std::string> writes;          // "name=value" in order, for the registers the panel sequence writes
  uint64_t now_us = 0, cycle_until_us = 0;
  bool power_stuck = false, port_stuck = false, pipe_stuck = false;
  bool xe = false;
  uint64_t func = 0, conf = 0, tp = 0;
  static IntelRegs* current;

  explicit IntelRegs(bool xe_lp = false) : xe(xe_lp) {
    func = xe ? P::kTransDdiFuncCtlA : P::kTransDdiFuncCtlEdp;
    conf = xe ? P::kTransConfA : P::kTransConfEdp;
    tp = xe ? P::kTglDpTpCtlA : P::kDpTpCtlA;
    r[intel_display::kPipeConfA] = intel_display::kPipeEnable;
    r[intel_display::kPlaneCtlA] = intel_display::kPlaneEnable | intel_display::kPlaneFormatXrgb8888;
    r[intel_display::kTransHtotalA] = (2199u << 16) | 1919u;
    r[intel_display::kTransVtotalA] = (1124u << 16) | 1079u;
    r[intel_display::kPipeSrcA] = (1919u << 16) | 1079u;
    r[intel_display::kPlaneSurfA] = 0x00400000;
    r[func] = xe ? P::kFuncEnable | P::kFuncModeDpSst | (1u << 27) | 0x40 : P::kFuncEnable | P::kFuncModeDpSst | (0u << 28) | (0u << 12) | 0x40;
    r[conf] = P::kConfEnable | P::kConfState;
    r[P::kDdiBufCtlA] = P::kDdiBufEnable | 0x6;                 // (4 lanes as the firmware set them)
    r[tp] = P::kDpTpEnable | P::kDpTpNormal | (1u << 18);
    r[P::kPpStatus] = P::kPpOn;
    r[P::kPpControl] = P::kPowerTargetOn | P::kPowerReset | P::kBacklightEnable;
    r[P::kPpOnDelays] = (2100u << 16) | 500u;                   // T1+T3 210 ms, T8 50 ms
    r[P::kPpOffDelays] = (5000u << 16) | 300u;                  // T10 500 ms, T9 30 ms
    r[P::kBlcPwmPchCtl1] = P::kBlcPwmEnable | 0x1234;
  }
  const char* name(uint64_t o) const {
    if (o == P::kPpControl) return "pp";
    if (o == P::kBlcPwmPchCtl1) return "blc";
    if (o == P::kDdiBufCtlA) return "ddi";
    if (o == tp) return "tp";
    if (o == func) return "func";
    if (o == conf) return "conf";
    if (o == intel_display::kPlaneSurfA) return "surf";
    return nullptr;
  }
  uint32_t read32(uint64_t o) override {
    if (o == P::kPpStatus) {
      uint32_t v = r[o];
      if (now_us < cycle_until_us) v |= P::kPpCycleDelayActive;
      return v;
    }
    auto it = r.find(o);
    return it == r.end() ? 0 : it->second;
  }
  void write32(uint64_t o, uint32_t v) override {
    if (const char* n = name(o)) {
      char text[48];
      snprintf(text, sizeof text, "%s=%x", n, v);
      writes.push_back(text);
    }
    r[o] = v;
    if (o == P::kPpControl) {
      const bool on = (v & P::kPowerTargetOn) != 0, was = (r[P::kPpStatus] & P::kPpOn) != 0;
      if (on && !power_stuck) r[P::kPpStatus] = P::kPpOn;
      if (!on && was) {
        r[P::kPpStatus] = 0;
        cycle_until_us = now_us + 500000;                           // (T11+T12, the sequencer's)
      }
    } else if (o == P::kDdiBufCtlA) {
      r[o] = (v & P::kDdiBufEnable) && !port_stuck ? (v & ~P::kDdiBufIdle) : (v | P::kDdiBufIdle);
    } else if (o == conf) {
      r[o] = (v & P::kConfEnable) && !pipe_stuck ? (v | P::kConfState) : (v & ~P::kConfState);
    }
  }
  std::string joined() const {
    std::string s;
    for (const std::string& w : writes) s += (s.empty() ? "" : " ") + w;
    return s;
  }
  // The position of the first write that starts with `prefix` (-1: none).
  int first(const std::string& prefix) const {
    for (size_t i = 0; i < writes.size(); ++i)
      if (writes[i].compare(0, prefix.size(), prefix) == 0) return static_cast<int>(i);
    return -1;
  }
};
IntelRegs* IntelRegs::current = nullptr;
void fake_delay(uint32_t us) { if (IntelRegs::current) IntelRegs::current->now_us += us; }

std::string hex(uint32_t v) { char t[16]; snprintf(t, sizeof t, "%x", v); return t; }

}  // namespace

static void test_intel_panel() {
  {   // Gen9.5: found, let go, lit again
    IntelRegs regs;
    IntelRegs::current = &regs;
    intel_display::Display display(regs, IntelGpuArchitecture::Gen9_5);
    CHECK(display.probe() && display.pipe() == 0);
    IntelPanelOutput panel(regs, display, IntelGpuArchitecture::Gen9_5, &fake_delay);
    CHECK(panel.probe() && panel.has_panel_output() && panel.panel_lit() && panel.can_light_panel() && panel.transcoder() == P::kEdpTranscoder && panel.port() == 0);
    CHECK(panel.light_panel() == PanelLightStatus::Ok && regs.writes.empty());      // (lit already: nothing written)
    const uint32_t fw_ddi = regs.r[P::kDdiBufCtlA], fw_func = regs.r[regs.func], fw_conf = regs.r[regs.conf];
    // Let go, in i915's order: backlight (PP_CONTROL, then the PWM), the transcoder, its DDI function, the port, the panel power.
    panel.release_panel();
    CHECK(!panel.panel_lit() && panel.can_light_panel() && strcmp(panel.panel_status(), "released") == 0);
    const int pp_blc = regs.first("pp="), blc = regs.first("blc="), conf = regs.first("conf="), func = regs.first("func="), tp = regs.first("tp="), ddi = regs.first("ddi=");
    CHECK(pp_blc == 0 && blc == 1 && conf > blc && func > conf && tp > func && ddi > tp && regs.writes.back() == "pp=0");
    CHECK(!(regs.r[P::kPpStatus] & P::kPpOn) && !(regs.r[regs.conf] & P::kConfEnable) && !(regs.r[P::kDdiBufCtlA] & P::kDdiBufEnable) &&
          !(regs.r[regs.func] & P::kFuncEnable) && !(regs.r[P::kBlcPwmPchCtl1] & P::kBlcPwmEnable));
    CHECK(panel.waited_us() >= 30000);                      // T9 from PP_OFF_DELAYS
    panel.release_panel();                                  // (not lit: nothing)
    // Lit again: the power cycle time waited out, power, the port with pattern 1, training, the transcoder, the plane, T8, the backlight.
    regs.writes.clear();
    const uint64_t before = panel.waited_us();
    regs.r[intel_display::kPlaneSurfA] = 0;                 // (to see the plane put back)
    CHECK(panel.light_panel() == PanelLightStatus::Ok && panel.panel_lit() && strcmp(panel.panel_status(), "lit") == 0);
    CHECK(regs.now_us >= regs.cycle_until_us && panel.waited_us() - before >= 50000 + 1000 + 600);
    CHECK(regs.writes[0] == "pp=3" && regs.writes[1] == "tp=" + hex(P::kDpTpEnable | P::kDpTpPattern1 | (1u << 18)) && regs.writes[2] == "ddi=" + hex(fw_ddi));
    CHECK(regs.writes[3] == "tp=" + hex(P::kDpTpEnable | P::kDpTpPattern2 | (1u << 18)) && regs.writes[4] == "tp=" + hex(P::kDpTpEnable | P::kDpTpIdle | (1u << 18)));
    CHECK(regs.writes[5] == "func=" + hex(fw_func) && regs.writes[6] == "conf=" + hex(fw_conf | P::kConfEnable) &&
          regs.writes[7] == "tp=" + hex(P::kDpTpEnable | P::kDpTpNormal | (1u << 18)));
    const int surf = regs.first("surf="), blc_on = regs.first("blc=");
    CHECK(surf > 7 && blc_on > surf && regs.writes.back() == "pp=7" && regs.r[intel_display::kPlaneSurfA] == 0x00400000);
    CHECK(regs.r[P::kBlcPwmPchCtl1] == (P::kBlcPwmEnable | 0x1234) && (regs.r[regs.conf] & P::kConfState) && (regs.r[P::kPpStatus] & P::kPpOn));
  }
  {   // Each failing step leaves nothing on.
    for (int step = 0; step < 3; ++step) {
      IntelRegs regs;
      IntelRegs::current = &regs;
      intel_display::Display display(regs, IntelGpuArchitecture::Gen11);
      CHECK(display.probe());
      IntelPanelOutput panel(regs, display, IntelGpuArchitecture::Gen11, &fake_delay);
      CHECK(panel.probe());
      panel.release_panel();
      regs.power_stuck = step == 0;
      regs.port_stuck = step == 1;
      regs.pipe_stuck = step == 2;
      const PanelLightStatus st = panel.light_panel();
      CHECK(st == (step == 0 ? PanelLightStatus::PowerFailed : step == 1 ? PanelLightStatus::LinkTraining : PanelLightStatus::ModeSet));
      CHECK(!panel.panel_lit() && !(regs.r[P::kPpControl] & P::kPowerTargetOn) && !(regs.r[P::kDdiBufCtlA] & P::kDdiBufEnable) &&
            !(regs.r[regs.conf] & P::kConfEnable) && !(regs.r[P::kBlcPwmPchCtl1] & P::kBlcPwmEnable) && !(regs.r[P::kPpControl] & P::kBacklightEnable));
      regs.power_stuck = regs.port_stuck = regs.pipe_stuck = false;
      CHECK(panel.light_panel() == PanelLightStatus::Ok && panel.panel_lit());      // (and it works once the hardware does)
    }
  }
  {   // Not lit at boot (the OS started with the panel on the discrete GPU), or a pipe that feeds no eDP: nothing to offer.
    IntelRegs off;
    IntelRegs::current = &off;
    off.r[P::kPpStatus] = 0;
    intel_display::Display d1(off, IntelGpuArchitecture::Gen9_5);
    CHECK(d1.probe());
    IntelPanelOutput p1(off, d1, IntelGpuArchitecture::Gen9_5, &fake_delay);
    CHECK(!p1.probe() && !p1.has_panel_output() && !p1.can_light_panel() && strstr(p1.panel_status(), "not lit") && p1.light_panel() == PanelLightStatus::NoPanel);
    IntelRegs other;                                        // (bit flip: the eDP transcoder's input is pipe B, the firmware's pipe is A)
    IntelRegs::current = &other;
    other.r[other.func] |= 5u << 12;
    intel_display::Display d2(other, IntelGpuArchitecture::Gen9_5);
    CHECK(d2.probe());
    IntelPanelOutput p2(other, d2, IntelGpuArchitecture::Gen9_5, &fake_delay);
    CHECK(!p2.probe() && !p2.has_panel_output() && other.writes.empty());
    IntelRegs nothing;                                      // (no pipe on at all)
    IntelRegs::current = &nothing;
    nothing.r[intel_display::kPipeConfA] = 0;
    intel_display::Display d3(nothing, IntelGpuArchitecture::Gen9_5);
    IntelPanelOutput p3(nothing, d3, IntelGpuArchitecture::Gen9_5, &fake_delay);
    CHECK(!d3.probe() && !p3.probe());
    IntelRegs fine;                                         // (no way to wait: found, but not lit again)
    IntelRegs::current = &fine;
    intel_display::Display d4(fine, IntelGpuArchitecture::Gen9_5);
    CHECK(d4.probe());
    IntelPanelOutput p4(fine, d4, IntelGpuArchitecture::Gen9_5, nullptr);
    CHECK(p4.probe() && !p4.can_light_panel());
    p4.release_panel();
    CHECK(p4.light_panel() == PanelLightStatus::NotSupported && !p4.panel_lit());
    p4.set_delay(&fake_delay);
    CHECK(p4.can_light_panel() && p4.light_panel() == PanelLightStatus::Ok);
  }
  {   // Xe-LP: the pipe's own transcoder on DDI A, DP_TP_CTL of the transcoder.
    IntelRegs regs(true);
    IntelRegs::current = &regs;
    intel_display::Display display(regs, IntelGpuArchitecture::XeLp);
    CHECK(display.probe());
    IntelPanelOutput panel(regs, display, IntelGpuArchitecture::XeLp, &fake_delay);
    CHECK(panel.probe() && panel.transcoder() == 0 && panel.port() == 0);
    panel.release_panel();
    CHECK(!(regs.r[P::kTglDpTpCtlA] & P::kDpTpEnable) && !(regs.r[P::kTransConfA] & P::kConfEnable));
    CHECK(panel.light_panel() == PanelLightStatus::Ok && (regs.r[P::kTglDpTpCtlA] & P::kDpTpPatternMask) == P::kDpTpNormal && (regs.r[P::kTransConfA] & P::kConfState));
    IntelRegs hdmi(true);                                   // (the pipe drives HDMI on DDI B: not the panel)
    IntelRegs::current = &hdmi;
    hdmi.r[hdmi.func] = P::kFuncEnable | (2u << 27);
    intel_display::Display d(hdmi, IntelGpuArchitecture::XeLp);
    CHECK(d.probe());
    IntelPanelOutput p(hdmi, d, IntelGpuArchitecture::XeLp, &fake_delay);
    CHECK(!p.probe());
  }
  {   // In the mux switch: the panel goes to the discrete GPU and, unlike with the firmware framebuffer, comes back.
    IntelRegs regs;
    IntelRegs::current = &regs;
    intel_display::Display display(regs, IntelGpuArchitecture::Gen9_5);
    CHECK(display.probe());
    IntelPanelOutput panel(regs, display, IntelGpuArchitecture::Gen9_5, &fake_delay);
    CHECK(panel.probe());
    Log log;
    FakeMux mux(log);
    FakeOutput d(log, kD);
    GpuMuxSwitch sw(mux, nullptr, 0, 1);
    sw.attach_output(kI, &panel);
    sw.attach_output(kD, &d);
    CHECK(sw.switch_to(kD).ok() && !panel.panel_lit() && d.lit && mux.display == kD);
    const char* why = nullptr;
    CHECK(sw.can_switch_to(kI, &why));
    const GpuMuxSwitchReport back = sw.switch_to(kI);
    CHECK(back.status == GpuMuxSwitchStatus::Switched && panel.panel_lit() && !d.lit && mux.display == kI);
    // The panel that would not light: the switch is undone, the panel stays with the discrete GPU.
    CHECK(sw.switch_to(kD).ok());
    regs.power_stuck = true;
    const GpuMuxSwitchReport failed = sw.switch_to(kI);
    CHECK(failed.status == GpuMuxSwitchStatus::LightFailed && failed.light == PanelLightStatus::PowerFailed && failed.rolled_back && failed.rollback_clean);
    CHECK(d.lit && !panel.panel_lit() && mux.display == kD);
  }
  IntelRegs::current = nullptr;
}

// --- The desktop's monitor of the panel (GpuMuxDesktop over SystemMonitors) ----------------------------------------------------------------

namespace {

struct HostAllocator final : allocators::IAllocator {
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

// A GPU's panel output that drives a fake GPU: lighting gives its screen the panel's size and marks the connector connected.
struct ScreenOutput final : IPanelOutput {
  fakemon::FakeGpu& gpu;
  fakemon::FakeSource& source;
  uint32_t connector;
  bool lit = false, relight = true, show_connector = true;
  ScreenOutput(fakemon::FakeGpu& g, fakemon::FakeSource& s, uint32_t c, bool lit_now) : gpu(g), source(s), connector(c), lit(lit_now) {}
  bool has_panel_output() const override { return true; }
  bool panel_lit() const override { return lit; }
  bool can_light_panel() const override { return relight || lit; }
  PanelLightStatus light_panel() override {
    lit = true;
    gpu.w = 1920;
    gpu.h = 1080;
    if (show_connector) source.connectors[connector].info.connected = true;
    return PanelLightStatus::Ok;
  }
  void release_panel() override {
    lit = false;
    source.connectors[connector].info.connected = false;
  }
  const char* panel_status() const override { return lit ? "lit" : "released"; }
};

}  // namespace

static void test_desktop_follows() {
  HostAllocator a;
  {
    // The integrated GPU (index 0) shows the panel as its one display (the firmware's framebuffer: an Unknown connector); the discrete GPU
    // (index 1) waits with nothing lit, its eDP connector empty.
    fakemon::FakeSource isrc, dsrc;
    isrc.multi = false;
    isrc.add("Display-0", fakemon::small_edid(), true, ConnectorType::Unknown);
    dsrc.multi = false;
    dsrc.add("eDP-1", fakemon::small_edid(), false, ConnectorType::Internal);
    dsrc.add("HDMI-1", fakemon::Bytes(), false, ConnectorType::Hdmi);
    fakemon::FakeGpu igpu, dgpu;
    igpu.source = &isrc;
    igpu.w = 1920;
    igpu.h = 1080;
    dgpu.source = &dsrc;
    dgpu.w = dgpu.h = 0;
    SystemMonitors monitors(&igpu, a);
    CHECK(monitors.add_gpu(&dgpu, 1));
    monitors.initialize();
    CHECK(monitors.layout().count() == 1 && monitors.layout().at(0).id == 0 && monitors.apply());
    CHECK(monitors.set_scale(0, 125) && monitors.apply());
    const MonitorRect rect = monitors.layout().at(0).rect;
    Log log;
    FakeMux mux(log);
    ScreenOutput i(igpu, isrc, 0, true), d(dgpu, dsrc, 0, false);
    GpuMuxSwitch sw(mux, nullptr, 0, 1);
    sw.attach_output(kI, &i);
    sw.attach_output(kD, &d);
    GpuMuxDesktop desk(sw, monitors, &igpu, &dgpu);
    const char* why = nullptr;
    GpuMuxTarget at = kD;
    CHECK(desk.active(&at) && at == kI && desk.can_switch_to(kD, &why) && strcmp(desk.gpu_name(kD), "fake gpu") == 0);
    // To the discrete GPU: the panel's monitor is the discrete GPU's eDP now, in the same place at the same scale, and primary.
    IBuiltInScreenSwitch::Result r = desk.switch_to(kD);
    CHECK(r.ok && mux.display == kD && d.lit && !i.lit);
    CHECK(monitors.layout().count() == 1 && monitors.layout().at(0).id == SystemMonitors::kIdsPerGpu && monitors.panel_id() == SystemMonitors::kIdsPerGpu);
    CHECK(monitors.layout().at(0).scale_percent == 125 && monitors.layout().at(0).primary && monitors.layout().at(0).rect.width == rect.width &&
          monitors.layout().at(0).rect.height == rect.height);
    CHECK(!monitors.find(0)->connected() && monitors.find(SystemMonitors::kIdsPerGpu)->connected() && monitors.find(SystemMonitors::kIdsPerGpu)->gpu() == 1);
    MonitorTarget targets[4];
    CHECK(monitors.targets(targets, 4) == 1 && targets[0].gpu == &dgpu && targets[0].gpu_index == 1);
    CHECK(monitors.apply() && monitors.needs_compositing());
    // A poll that sees the firmware's display still "connected" does not take the panel back.
    isrc.changed = true;
    (void)monitors.poll();
    CHECK(monitors.layout().count() == 1 && monitors.layout().at(0).id == SystemMonitors::kIdsPerGpu);
    // And back: the integrated GPU's monitor again, same place and scale; nothing is copied to the discrete GPU any more.
    r = desk.switch_to(kI);
    CHECK(r.ok && mux.display == kI && i.lit && !d.lit);
    CHECK(monitors.layout().count() == 1 && monitors.layout().at(0).id == 0 && monitors.layout().at(0).scale_percent == 125 && monitors.layout().at(0).primary);
    CHECK(monitors.targets(targets, 4) == 0 && monitors.find(0)->connected() && !monitors.find(SystemMonitors::kIdsPerGpu)->connected());
    CHECK(monitors.apply());
    // The monitors cannot follow (the discrete GPU lists no panel once lit): the panel is switched back, the layout as it was.
    d.show_connector = false;
    log.clear();
    r = desk.switch_to(kD);
    CHECK(!r.ok && r.rolled_back && strstr(r.reason, "did not appear") && mux.display == kI && i.lit && !d.lit);
    CHECK(monitors.layout().count() == 1 && monitors.layout().at(0).id == 0);
    // With the firmware framebuffer on the integrated side the panel cannot come back: the page is told why.
    d.show_connector = true;
    i.relight = false;
    CHECK(desk.switch_to(kD).ok && !desk.can_switch_to(kI, &why) && why && *why);
    CHECK(!desk.switch_to(kI).ok && monitors.layout().at(0).id == SystemMonitors::kIdsPerGpu);
  }
  {
    // A side the desktop has no driver for, and a discrete GPU the monitors were not told about (added when the panel moves there).
    fakemon::FakeSource isrc, dsrc;
    isrc.multi = false;
    isrc.add("Display-0", fakemon::small_edid(), true, ConnectorType::Unknown);
    dsrc.multi = false;
    dsrc.add("eDP-1", fakemon::small_edid(), false, ConnectorType::Internal);
    fakemon::FakeGpu igpu, dgpu;
    igpu.source = &isrc;
    igpu.w = 1920;
    igpu.h = 1080;
    dgpu.source = &dsrc;
    dgpu.w = dgpu.h = 0;
    SystemMonitors monitors(&igpu, a);
    monitors.initialize();
    Log log;
    FakeMux mux(log);
    ScreenOutput i(igpu, isrc, 0, true), d(dgpu, dsrc, 0, false);
    GpuMuxSwitch none(mux, nullptr, 0, -1);
    none.attach_output(kI, &i);
    none.attach_output(kD, &d);
    GpuMuxDesktop no_driver(none, monitors, &igpu, nullptr);
    const char* why = nullptr;
    CHECK(!no_driver.can_switch_to(kD, &why) && strstr(why, "no display driver") && !no_driver.switch_to(kD).ok && mux.display == kI);
    GpuMuxSwitch sw(mux, nullptr, 0, 3);
    sw.attach_output(kI, &i);
    sw.attach_output(kD, &d);
    GpuMuxDesktop late(sw, monitors, &igpu, &dgpu);
    CHECK(monitors.gpu_count() == 1 && late.switch_to(kD).ok && monitors.gpu_count() == 2 && monitors.gpu_of(monitors.layout().at(0).id) == 3);
  }
  CHECK(a.live == 0);
}

int main() {
  test_success();
  test_failures();
  test_refusals();
  test_gop_side();
  test_real_manager();
  test_threads();
  test_intel_panel();
  test_desktop_follows();
  printf("GpuMuxSwitchTest: %d checks, %d failed\n", g_checks, g_failed);
  return g_failed ? 1 : 0;
}
