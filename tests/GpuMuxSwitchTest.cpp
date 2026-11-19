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
// threads switching at once (only one in flight, the others Busy, the mux never seen half-moved).
#include <stdio.h>
#include <string.h>
#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "Hardware/GraphicsProcessingUnit/Multi/GpuManager.hpp"
#include "Hardware/GraphicsProcessingUnit/Multi/GpuMuxSwitch.hpp"

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
  CHECK(!sw.can_switch_to(kI, &why) && strstr(why, "cannot light the panel again"));
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

int main() {
  test_success();
  test_failures();
  test_refusals();
  test_gop_side();
  test_real_manager();
  test_threads();
  printf("GpuMuxSwitchTest: %d checks, %d failed\n", g_checks, g_failed);
  return g_failed ? 1 : 0;
}
