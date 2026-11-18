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

// The ACPI side of hybrid graphics against AML shaped like laptops' DSDTs: finding a GPU's Device by _ADR under its root port (and the bus
// problem: decoy devices with the same _ADR under other bridges, a second host bridge with _BBN), D3cold through _PR3 on the GPU and on the
// root port (_PS3 / _OFF last first / _STA skips / _PR0 / _ON first first / _PS0), NVIDIA Optimus (the caps call: UUID bytes, revision,
// function, argument; skipped where _PR3 exists), JT, the old hybrid mux _DSM, AMD ATPX (verify, parameters, power control, mux switch, the
// hybrid rule) and firmware that faults or answers garbage. Every method records what it was called with in root Names the test reads back.
#include <random>
#include "AcpiTestSupport.hpp"
#include "Hardware/GraphicsProcessingUnit/Multi/Acpi/GpuAcpiPower.hpp"

// --- AML builders this test needs on top of AcpiTestSupport.hpp.

static Bytes buf(const Bytes& bytes) { return cat({b({0x11}), pkg(cat({num(bytes.size()), bytes}))}); }
static Bytes buf_zero(uint32_t n) { return cat({b({0x11}), pkg(num(n))}); }
static Bytes ret(const Bytes& v) { return cat({b({0xA4}), v}); }
static Bytes if_(const Bytes& cond, const Bytes& body) { return cat({b({0xA0}), pkg(cat({cond, body}))}); }
static Bytes lequal(const Bytes& a, const Bytes& c) { return cat({b({0x93}), a, c}); }
static Bytes add(const Bytes& a, const Bytes& c) { return cat({b({0x72}), a, c, b({0x00})}); }
static Bytes mul(const Bytes& a, const Bytes& c) { return cat({b({0x77}), a, c, b({0x00})}); }
static Bytes arg(int i) { return b({static_cast<uint8_t>(0x68 + i)}); }
static Bytes object_type(const Bytes& x) { return cat({b({0x8E}), x}); }
static Bytes power_resource(const char* n, const Bytes& body) { return cat({b({0x5B, 0x84}), pkg(cat({path(n), b({0x00, 0x00, 0x00}), body}))}); }
// Appends `id` to the log in the root Name `log` (`shift` per entry: 16 for nibbles, 256 for bytes).
static Bytes log_to(const char* log, const Bytes& id, uint32_t shift = 16) { return store(add(mul(path(log), num(shift)), id), path(log)); }
// DerefOf(Index(Buffer () {0}, 50)): an index outside the buffer, which stops the method.
static Bytes fault() { return ret(cat({b({0x83}), b({0x88}), buf(b({0})), num(50), b({0x00})})); }
static Bytes uuid_bytes(const AcpiDsmUuid& u) { return Bytes(u.bytes, u.bytes + 16); }

static Bytes le32(uint32_t v) { return b({static_cast<uint8_t>(v), static_cast<uint8_t>(v >> 8), static_cast<uint8_t>(v >> 16), static_cast<uint8_t>(v >> 24)}); }

// The root Names every method records into.
static Bytes recorder_names() {
  return cat({name_def("\\PLOG", num(0)), name_def("\\P0ST", num(1)), name_def("\\P1ST", num(1)), name_def("\\FLOG", num(0)), name_def("\\UUID", buf_zero(16)),
              name_def("\\REVN", num(0)), name_def("\\FUNC", num(0)), name_def("\\AR3B", buf_zero(4)), name_def("\\AR3I", num(0)), name_def("\\A3TY", num(0)),
              name_def("\\AFUN", num(0)), name_def("\\ATY1", num(0)), name_def("\\AARG", buf_zero(4)), name_def("\\ALOG", num(0))});
}

// Two power resources: PG00 (_ON logs 6, _OFF 5) and PG01 (_ON 8, _OFF 7), their state in \P0ST and \P1ST.
static Bytes power_resources() {
  auto res = [](const char* n, const char* state, int on, int off) {
    return power_resource(n, cat({method("_STA", 0, ret(path(state))), method("_ON_", 0, cat({log_to("\\PLOG", num(on)), store(num(1), path(state))})),
                                  method("_OFF", 0, cat({log_to("\\PLOG", num(off)), store(num(0), path(state))}))}));
  };
  return cat({res("PG00", "\\P0ST", 6, 5), res("PG01", "\\P1ST", 8, 7)});
}

static Bytes ps(const char* n, int id, bool faults = false) { return method(n, 0, faults ? fault() : log_to("\\PLOG", num(id))); }

struct DsmAnswer {
  uint32_t function;
  Bytes value;
};
struct DsmUuidAnswers {
  AcpiDsmUuid uuid;
  std::vector<DsmAnswer> answers;
};

// A _DSM that records Arg0, Arg1, Arg2 and Arg3 and, for each UUID it knows, logs Arg2 + 1 into \FLOG (bytes) and answers.
static Bytes dsm(const std::vector<DsmUuidAnswers>& uuids) {
  Bytes body = cat({store(arg(0), path("\\UUID")), store(arg(1), path("\\REVN")), store(arg(2), path("\\FUNC")), store(object_type(arg(3)), path("\\A3TY")),
                    if_(lequal(path("\\A3TY"), num(3)), store(arg(3), path("\\AR3B"))), if_(lequal(path("\\A3TY"), num(1)), store(arg(3), path("\\AR3I")))});
  for (const DsmUuidAnswers& u : uuids) {
    Bytes inner = log_to("\\FLOG", add(arg(2), num(1)), 256);
    for (const DsmAnswer& a : u.answers) inner = cat({inner, if_(lequal(arg(2), num(a.function)), a.value)});
    body = cat({body, if_(lequal(arg(0), buf(uuid_bytes(u.uuid))), inner)});
  }
  body = cat({body, ret(buf(b({0x00})))});
  return method("_DSM", 4, body);
}

// The laptop: a host bridge PCI0 (PNP0A08, _CID PNP0A03) with the integrated GPU GFX0 (00:02.0), a root port RP01 (00:1C.0) whose PXSX
// (_ADR 0) is the Wi-Fi card, and the GPU's root port PEG0 (00:01.0) with PEGP (_ADR 0). A second host bridge PCI1 has _BBN 0x40.
struct Machine {
  bool pr3_on_bridge = true;
  bool pr3_on_gpu = false;
  bool gpu_pr0 = true;
  bool gpu_ps = true;
  bool gpu_ps3_faults = false;
  bool bridge_ps = true;
  Bytes gpu_pr3 = package(1, path("\\_SB.PG00"));
  Bytes bridge_pr3 = package(1, path("\\_SB.PG00"));
  Bytes gpu_extra, igpu_extra, bridge_extra;
};

static Bytes machine_table(const Machine& m) {
  Bytes pegp = name_def("_ADR", num(0));
  if (m.gpu_ps) pegp = cat({pegp, ps("_PS3", 1, m.gpu_ps3_faults), ps("_PS0", 2)});
  if (m.pr3_on_gpu) {
    pegp = cat({pegp, name_def("_PR3", m.gpu_pr3)});
    if (m.gpu_pr0) pegp = cat({pegp, name_def("_PR0", package(1, path("\\_SB.PG00")))});
  }
  pegp = cat({pegp, m.gpu_extra});
  Bytes peg0 = name_def("_ADR", num(0x00010000));
  if (m.bridge_ps) peg0 = cat({peg0, ps("_PS3", 3), ps("_PS0", 4)});
  if (m.pr3_on_bridge) peg0 = cat({peg0, name_def("_PR0", m.bridge_pr3), name_def("_PR3", m.bridge_pr3)});
  peg0 = cat({peg0, m.bridge_extra, device("PEGP", pegp)});
  const Bytes pci0 = cat({name_def("_HID", num(0x080AD041)), name_def("_CID", num(0x030AD041)), device("GFX0", cat({name_def("_ADR", num(0x00020000)), m.igpu_extra})),
                          device("RP01", cat({name_def("_ADR", num(0x001C0000)), device("PXSX", name_def("_ADR", num(0)))})), device("PEG0", peg0)});
  const Bytes pci1 = cat({name_def("_HID", num(0x080AD041)), name_def("_BBN", num(0x40)), device("DEV3", name_def("_ADR", num(0x00030000)))});
  return make_table("DSDT", cat({recorder_names(), scope("\\_SB", cat({power_resources(), device("PCI0", pci0), device("PCI1", pci1)}))}));
}

struct Vm {
  TestAllocator allocator;
  FakeHost host;
  Bytes table;
  AmlInterpreter vm{allocator, host};
  explicit Vm(const Machine& m) : table(machine_table(m)) { CHECK(vm.load_table(ConstByteView(table.data(), table.size()))); }
  uint64_t integer(const char* name) {
    uint64_t v = ~0ull;
    CHECK(vm.evaluate_integer(name, &v));
    return v;
  }
  Bytes buffer(const char* name) {
    Value v;
    if (!vm.evaluate(name, &v) || v.kind != ValueKind::Buffer) return Bytes();
    return Bytes(v.bytes, v.bytes + v.length);
  }
};

static const PciAddress kGpu{1, 0, 0}, kRootPort{0, 1, 0}, kIgpu{0, 2, 0};

static bool is(Vm& v, acpi::Node* n, const char* path) { return n != nullptr && n == v.vm.find(path); }

// --- Finding devices

static void test_lookup() {
  Machine m;
  m.gpu_extra = method("_ROM", 2, ret(buf_zero(4)));
  Vm v(m);
  CHECK(is(v, GpuAcpiNamespace::find_pci_device(v.vm, kGpu, kRootPort), "\\_SB.PCI0.PEG0.PEGP"));
  CHECK(is(v, GpuAcpiNamespace::find_pci_device(v.vm, kIgpu), "\\_SB.PCI0.GFX0"));
  CHECK(is(v, GpuAcpiNamespace::find_pci_device(v.vm, kRootPort), "\\_SB.PCI0.PEG0"));
  // The Wi-Fi card behind RP01 has the same _ADR: the hint tells them apart.
  CHECK(is(v, GpuAcpiNamespace::find_pci_device(v.vm, PciAddress{2, 0, 0}, PciAddress{0, 0x1C, 0}), "\\_SB.PCI0.RP01.PXSX"));
  // Without a hint the Device that looks like a GPU wins.
  CHECK(is(v, GpuAcpiNamespace::find_pci_device(v.vm, kGpu), "\\_SB.PCI0.PEG0.PEGP"));
  // A bridge that is not there, a bridge on another host bridge's bus.
  CHECK(GpuAcpiNamespace::find_pci_device(v.vm, kGpu, PciAddress{0, 5, 0}) == nullptr);
  CHECK(GpuAcpiNamespace::find_pci_device(v.vm, kGpu, PciAddress{0x40, 1, 0}) == nullptr);
  // _BBN: 40:03.0 is under PCI1, 00:03.0 is nowhere.
  CHECK(is(v, GpuAcpiNamespace::find_pci_device(v.vm, PciAddress{0x40, 3, 0}), "\\_SB.PCI1.DEV3"));
  CHECK(GpuAcpiNamespace::find_pci_device(v.vm, PciAddress{0, 3, 0}) == nullptr);
  CHECK(GpuAcpiNamespace::find_pci_device(v.vm, PciAddress{0, 2, 1}) == nullptr);
  const GpuAcpiNodes nodes = GpuAcpiNamespace::find_gpu(v.vm, kGpu, kRootPort);
  CHECK(is(v, nodes.device, "\\_SB.PCI0.PEG0.PEGP"));
  CHECK(is(v, nodes.bridge, "\\_SB.PCI0.PEG0"));
  const GpuAcpiNodes igpu = GpuAcpiNamespace::find_gpu(v.vm, kIgpu);
  CHECK(is(v, igpu.device, "\\_SB.PCI0.GFX0"));
  CHECK(igpu.bridge == nullptr);   // (on the host bridge's bus)
  // A GPU without a Device of its own still finds its bridge through the hint.
  const GpuAcpiNodes lonely = GpuAcpiNamespace::find_gpu(v.vm, PciAddress{1, 0, 1}, kRootPort);
  CHECK(lonely.device == nullptr);
  CHECK(is(v, lonely.bridge, "\\_SB.PCI0.PEG0"));
}

// --- Power resources

static void test_pr3_on_root_port() {
  Machine m;
  Vm v(m);
  UniquePointer<IGpuPlatformPower> p = GpuAcpiPower::find(v.vm, kGpu, kRootPort, 0x8086, v.allocator);
  CHECK(p);
  if (!p) return;
  CHECK(p->mechanism() == GpuPowerMechanism::AcpiPowerResources);
  CHECK(p->power_off());
  CHECK(v.integer("\\PLOG") == 0x135);   // GPU _PS3, bridge _PS3, PG00 _OFF
  CHECK(v.integer("\\P0ST") == 0);
  CHECK(p->power_off());                  // already off: _STA skips the _OFF
  CHECK(v.integer("\\PLOG") == 0x13513);
  CHECK(p->power_on());
  CHECK(v.integer("\\PLOG") == 0x13513642);   // PG00 _ON, bridge _PS0, GPU _PS0
  CHECK(v.integer("\\P0ST") == 1);
}

static void test_pr3_on_gpu() {
  Machine m;
  m.pr3_on_gpu = true;
  m.gpu_pr3 = package(2, cat({path("\\_SB.PG00"), path("\\_SB.PG01")}));
  Vm v(m);
  GpuPowerResourceSet set;
  const GpuAcpiNodes nodes = GpuAcpiNamespace::find_gpu(v.vm, kGpu, kRootPort);
  set.read(v.vm, nodes.device, nodes.bridge);
  CHECK(set.has_resources());
  CHECK(!set.on_bridge());   // the GPU's own come first
  CHECK(set.d3_resources().size() == 2);
  CHECK(set.d0_resources().size() == 1);
  CHECK(set.power_off(v.vm));
  CHECK(v.integer("\\PLOG") == 0x175);   // GPU _PS3, PG01 _OFF, PG00 _OFF (last first); the bridge's _PS3 is not the GPU's
  CHECK(set.power_on(v.vm));
  CHECK(v.integer("\\PLOG") == 0x175682);   // _PR0's PG00 _ON, then PG01 _ON, then GPU _PS0

  // _STA: a resource already off is not turned off again; one already on is not turned on again.
  Vm w(m);
  set.read(w.vm, GpuAcpiNamespace::find_gpu(w.vm, kGpu, kRootPort).device, nullptr);
  CHECK(w.vm.evaluate("\\_SB.PG01._OFF", nullptr));   // PG01 off by itself (logs 7)
  CHECK(set.power_off(w.vm));
  CHECK(w.integer("\\PLOG") == 0x715);
  CHECK(w.vm.evaluate("\\_SB.PG00._ON_", nullptr));   // PG00 back on by itself (logs 6)
  CHECK(set.power_on(w.vm));
  CHECK(w.integer("\\PLOG") == 0x715682);
}

static void test_pr3_garbage() {
  // _PR3 that is no package, a package of things that are not power resources, a faulting _PS3, no _PS methods at all.
  {
    Machine m;
    m.bridge_pr3 = num(5);
    Vm v(m);
    CHECK(!GpuAcpiPower::find(v.vm, kGpu, kRootPort, 0x8086, v.allocator));
  }
  {
    Machine m;
    m.bridge_pr3 = package(3, cat({num(7), path("\\_SB.PCI0"), path("\\_SB.NONE")}));
    Vm v(m);
    CHECK(!GpuAcpiPower::find(v.vm, kGpu, kRootPort, 0x8086, v.allocator));
  }
  {
    Machine m;
    m.bridge_pr3 = package(3, cat({num(7), path("\\_SB.PG01"), path("\\_SB.PG01")}));   // (a duplicate is listed once)
    Vm v(m);
    GpuPowerResourceSet set;
    const GpuAcpiNodes nodes = GpuAcpiNamespace::find_gpu(v.vm, kGpu, kRootPort);
    set.read(v.vm, nodes.device, nodes.bridge);
    CHECK(set.d3_resources().size() == 1);
    CHECK(set.on_bridge());
  }
  {
    Machine m;
    m.gpu_ps3_faults = true;
    Vm v(m);
    UniquePointer<IGpuPlatformPower> p = GpuAcpiPower::find(v.vm, kGpu, kRootPort, 0x8086, v.allocator);
    CHECK(p);
    if (p) {
      CHECK(!p->power_off());
      CHECK(v.integer("\\PLOG") == 0);   // nothing was cut
      CHECK(v.integer("\\P0ST") == 1);
    }
  }
  {
    Machine m;
    m.pr3_on_bridge = false;
    Vm v(m);
    CHECK(!GpuAcpiPower::find(v.vm, kGpu, kRootPort, 0x8086, v.allocator));
    CHECK(!GpuAcpiPower::find(v.vm, kGpu, kRootPort, GpuAcpiPower::kVendorAmd, v.allocator));
    CHECK(!GpuAcpiPower::find_mux(v.vm, kGpu, kRootPort, GpuAcpiPower::kVendorNvidia, v.allocator));
  }
}

// --- NVIDIA

static DsmUuidAnswers optimus_answers() {
  // Function 0: bits 0, 0x1A and 0x1B. Function 0x1A: caps 0x01 (enabled) as a 4-byte buffer.
  return DsmUuidAnswers{kNvidiaOptimusDsmUuid, {{0, ret(buf(b({0x01, 0x00, 0x00, 0x0C})))}, {0x1A, ret(buf(le32(0x00000001)))}, {0x1B, ret(buf(le32(0)))}}};
}

static void test_optimus() {
  Machine m;
  m.pr3_on_bridge = false;   // the old Optimus way: the _DSM arms _PS3 to cut the power
  m.gpu_extra = dsm({optimus_answers()});
  Vm v(m);
  UniquePointer<IGpuPlatformPower> p = GpuAcpiPower::find(v.vm, kGpu, kRootPort, GpuAcpiPower::kVendorNvidia, v.allocator);
  CHECK(p);
  if (!p) return;
  CHECK(p->mechanism() == GpuPowerMechanism::NvidiaOptimus);
  NvidiaOptimusPower* o = static_cast<NvidiaOptimusPower*>(p.get());
  CHECK(o->capabilities() == 1);
  CHECK(!o->skips_dsm());
  CHECK(v.integer("\\FLOG") == 0x011B);   // function 0, then the caps read (0x1A + 1)
  CHECK(p->power_off());
  CHECK(v.integer("\\FLOG") == 0x011B1C1B);   // then flags (0x1B), then caps with the power-down argument
  CHECK(v.integer("\\FUNC") == 0x1A);
  CHECK(v.integer("\\REVN") == 0x100);
  CHECK(v.integer("\\A3TY") == 3);   // a buffer
  CHECK(v.buffer("\\AR3B") == b({0x01, 0x00, 0x00, 0x03}));   // 0x03000001, little endian
  CHECK(v.buffer("\\UUID") == b({0xF8, 0xD8, 0x86, 0xA4, 0xDA, 0x0B, 0x1B, 0x47, 0xA7, 0x2B, 0x60, 0x42, 0xA6, 0xB5, 0xBE, 0xE0}));
  CHECK(v.integer("\\PLOG") == 0x1);   // then the GPU's _PS3
  CHECK(p->power_on());
  CHECK(v.integer("\\PLOG") == 0x12);
  CHECK(!GpuAcpiPower::find_mux(v.vm, kGpu, kRootPort, GpuAcpiPower::kVendorNvidia, v.allocator));

  // With _PR3 on the root port the _DSM is skipped (nouveau's optimus_skip_dsm) and the power resources do it.
  Machine n;
  n.gpu_extra = dsm({optimus_answers()});
  Vm w(n);
  UniquePointer<IGpuPlatformPower> q = GpuAcpiPower::find(w.vm, kGpu, kRootPort, GpuAcpiPower::kVendorNvidia, w.allocator);
  CHECK(q && q->mechanism() == GpuPowerMechanism::NvidiaOptimus);
  if (q) {
    CHECK(static_cast<NvidiaOptimusPower*>(q.get())->skips_dsm());
    const uint64_t before = w.integer("\\FLOG");
    CHECK(q->power_off());
    CHECK(w.integer("\\FLOG") == before);
    CHECK(w.integer("\\PLOG") == 0x135);
  }

  // A caps call that faults: power_off fails before _PS3 runs.
  Machine f;
  f.pr3_on_bridge = false;
  f.gpu_extra = dsm({DsmUuidAnswers{kNvidiaOptimusDsmUuid, {{0, ret(buf(b({0x01, 0x00, 0x00, 0x04})))}, {0x1A, if_(lequal(path("\\AR3B"), buf(le32(0x03000001))), fault())}}}});
  Vm x(f);
  UniquePointer<IGpuPlatformPower> r = GpuAcpiPower::find(x.vm, kGpu, kRootPort, GpuAcpiPower::kVendorNvidia, x.allocator);
  CHECK(r);
  if (r) {
    CHECK(!r->power_off());
    CHECK(x.integer("\\PLOG") == 0);
  }
  // A _DSM whose function 0 lacks bit 0, or that faults: no Optimus.
  Machine g;
  g.pr3_on_bridge = false;
  g.gpu_extra = dsm({DsmUuidAnswers{kNvidiaOptimusDsmUuid, {{0, ret(buf(b({0x00, 0x00, 0x00, 0x0C})))}}}});
  Vm y(g);
  CHECK(!GpuAcpiPower::find(y.vm, kGpu, kRootPort, GpuAcpiPower::kVendorNvidia, y.allocator));
  Machine h;
  h.pr3_on_bridge = false;
  h.gpu_extra = method("_DSM", 4, fault());
  Vm z(h);
  CHECK(!GpuAcpiPower::find(z.vm, kGpu, kRootPort, GpuAcpiPower::kVendorNvidia, z.allocator));
}

static void test_jt() {
  Machine m;   // _PR3 on the root port, Optimus and JT both present (a newer laptop)
  m.gpu_extra = dsm({optimus_answers(), DsmUuidAnswers{kNvidiaJtDsmUuid, {{0, ret(buf(b({0x03})))}, {1, ret(num(0x00200001))}}}});
  Vm v(m);
  UniquePointer<IGpuPlatformPower> p = GpuAcpiPower::find(v.vm, kGpu, kRootPort, GpuAcpiPower::kVendorNvidia, v.allocator);
  CHECK(p && p->mechanism() == GpuPowerMechanism::NvidiaJt);
  if (!p) return;
  NvidiaJtPower* jt = static_cast<NvidiaJtPower*>(p.get());
  CHECK(jt->capabilities() == 0x00200001);
  CHECK(jt->jt_revision() == 2);
  CHECK(v.integer("\\REVN") == 0x103);   // the last call was JT_FUNC_CAPS
  CHECK(v.integer("\\FUNC") == 1);
  CHECK(v.buffer("\\UUID") == uuid_bytes(kNvidiaJtDsmUuid));
  CHECK(p->power_off());
  CHECK(v.integer("\\PLOG") == 0x135);
  CHECK(p->power_on());
  CHECK(v.integer("\\PLOG") == 0x135642);

  // JT without power resources has nothing to cut the power with.
  Machine n = m;
  n.pr3_on_bridge = false;
  n.gpu_ps = false;
  n.gpu_extra = dsm({DsmUuidAnswers{kNvidiaJtDsmUuid, {{0, ret(buf(b({0x03})))}}}});
  Vm w(n);
  CHECK(!GpuAcpiPower::find(w.vm, kGpu, kRootPort, GpuAcpiPower::kVendorNvidia, w.allocator));
}

static void test_nvidia_mux() {
  // The old hybrid _DSM, on the integrated GPU's Device: function 0 -> 0x0F (power, LED/mux, active), function 2 refuses 0x11 with 0x80000002.
  Machine m;
  m.pr3_on_bridge = false;
  m.gpu_ps = false;
  m.igpu_extra = dsm({DsmUuidAnswers{kNvidiaMuxDsmUuid, {{0, ret(num(0x0F))}, {2, if_(lequal(arg(3), num(0x11)), ret(num(0x80000002)))}, {3, ret(num(0))}}}});
  Vm v(m);
  CHECK(!GpuAcpiPower::find(v.vm, kGpu, kRootPort, GpuAcpiPower::kVendorNvidia, v.allocator));   // (not without knowing where the iGPU is)
  UniquePointer<IGpuPlatformPower> p = GpuAcpiPower::find(v.vm, kGpu, kRootPort, GpuAcpiPower::kVendorNvidia, v.allocator, kIgpu);
  CHECK(p && p->mechanism() == GpuPowerMechanism::NvidiaHybridDsm);
  if (p) {
    CHECK(p->power_off());
    CHECK(v.integer("\\FUNC") == 3);
    CHECK(v.integer("\\REVN") == 0x102);
    CHECK(v.integer("\\A3TY") == 1);   // an integer
    CHECK(v.integer("\\AR3I") == 2);   // stamina: off
    CHECK(v.buffer("\\UUID") == uuid_bytes(kNvidiaMuxDsmUuid));
    CHECK(p->power_on());
    CHECK(v.integer("\\AR3I") == 1);   // speed: on
  }
  UniquePointer<IGpuMux> mux = GpuAcpiPower::find_mux(v.vm, kGpu, kRootPort, GpuAcpiPower::kVendorNvidia, v.allocator, kIgpu);
  CHECK(mux);
  if (!mux) return;
  GpuMuxTarget t;
  CHECK(!mux->active(&t));
  CHECK(mux->switch_to(GpuMuxTarget::Discrete));
  CHECK(v.integer("\\FUNC") == 2);
  CHECK(v.integer("\\AR3I") == 0x12);
  CHECK(mux->active(&t) && t == GpuMuxTarget::Discrete);
  CHECK(!mux->switch_to(GpuMuxTarget::Integrated));   // the firmware said 0x80000002
  CHECK(mux->active(&t) && t == GpuMuxTarget::Discrete);
  CHECK(!mux->switch_ddc(GpuMuxTarget::Integrated));
}

// --- AMD ATPX

static uint32_t g_slept = 0;
static void sleep_counter(uint32_t ms) { g_slept += ms; }

// ATPX on the integrated GPU: records Arg0 (\AFUN, and +1 into \ALOG), the type of Arg1 (\ATY1) and a buffer Arg1 (\AARG).
static Bytes atpx(const Bytes& verify, const Bytes& params) {
  return method("ATPX", 2, cat({store(arg(0), path("\\AFUN")), log_to("\\ALOG", add(arg(0), num(1))), store(object_type(arg(1)), path("\\ATY1")),
                                if_(lequal(path("\\ATY1"), num(3)), store(arg(1), path("\\AARG"))), if_(lequal(arg(0), num(0)), verify),
                                if_(lequal(arg(0), num(1)), params), ret(num(0))}));
}
static Bytes verify_answer(uint32_t functions) { return ret(buf(cat({b({8, 0, 1, 0}), le32(functions)}))); }
static Bytes params_answer(uint32_t valid, uint32_t flags) { return ret(buf(cat({b({10, 0}), le32(valid), le32(flags)}))); }

static void test_atpx() {
  Machine m;
  m.pr3_on_bridge = false;
  m.gpu_ps = false;
  m.igpu_extra = atpx(verify_answer(0x3F), params_answer(0, 0));
  Vm v(m);
  g_slept = 0;
  UniquePointer<IGpuPlatformPower> p = GpuAcpiPower::find(v.vm, kGpu, kRootPort, GpuAcpiPower::kVendorAmd, v.allocator, {}, sleep_counter);
  CHECK(p && p->mechanism() == GpuPowerMechanism::AmdAtpx);   // (found by searching the namespace: the iGPU was not named)
  if (p) {
    AmdAtpx* a = static_cast<AmdAtpx*>(p.get());
    CHECK(a->info().verify.version == 1);
    CHECK(a->info().power_control && a->info().display_mux && a->info().i2c_mux && a->info().switch_start && a->info().switch_end);
    CHECK(!a->info().hybrid);
    CHECK(v.integer("\\ALOG") == 0x12);   // verify, then the parameters
    CHECK(v.integer("\\ATY1") == 1);       // no parameters: the integer 0
    CHECK(p->power_off());
    CHECK(v.integer("\\AFUN") == 2);
    CHECK(v.integer("\\ATY1") == 3);
    CHECK(v.buffer("\\AARG") == b({3, 0, 0, 0}));   // size 3, state 0 (the fourth byte is the Name's padding)
    CHECK(g_slept == 200);
    CHECK(p->power_on());
    CHECK(v.buffer("\\AARG") == b({3, 0, 1, 0}));
    CHECK(g_slept == 200);
  }
  UniquePointer<IGpuMux> mux = GpuAcpiPower::find_mux(v.vm, kGpu, kRootPort, GpuAcpiPower::kVendorAmd, v.allocator, kIgpu);
  CHECK(mux);
  if (mux) {
    const uint64_t before = v.integer("\\ALOG");
    CHECK(mux->switch_to(GpuMuxTarget::Discrete));
    CHECK(v.integer("\\ALOG") == ((before * 16 + 6) * 16 + 4) * 16 * 16 + 5 * 16 + 7);   // start, display mux, I2C mux, end
    CHECK(v.buffer("\\AARG") == b({4, 0, 1, 0}));
    GpuMuxTarget t;
    CHECK(mux->active(&t) && t == GpuMuxTarget::Discrete);
    CHECK(mux->switch_ddc(GpuMuxTarget::Integrated));
    CHECK(v.integer("\\AFUN") == 4);
    CHECK(v.buffer("\\AARG") == b({4, 0, 0, 0}));
  }

  // Only power control, flagged through the parameters (dynamic PX) instead of the function bits; no mux.
  {
    Machine n = m;
    n.igpu_extra = atpx(verify_answer(AmdAtpx::kPxParametersSupported), params_answer(AmdAtpx::kDynamicPxSupported, AmdAtpx::kDynamicPxSupported));
    Vm w(n);
    UniquePointer<IGpuPlatformPower> q = GpuAcpiPower::find(w.vm, kGpu, kRootPort, GpuAcpiPower::kVendorAmd, w.allocator, kIgpu);
    CHECK(q && q->mechanism() == GpuPowerMechanism::AmdAtpx);
    CHECK(!GpuAcpiPower::find_mux(w.vm, kGpu, kRootPort, GpuAcpiPower::kVendorAmd, w.allocator, kIgpu));
  }
  // Hybrid graphics: the power resources instead of ATPX power control; and without them, nothing.
  {
    Machine n = m;
    n.pr3_on_bridge = true;
    n.igpu_extra = atpx(verify_answer(0x3F), params_answer(AmdAtpx::kMsHybridGraphicsSupported, AmdAtpx::kMsHybridGraphicsSupported));
    Vm w(n);
    UniquePointer<IGpuPlatformPower> q = GpuAcpiPower::find(w.vm, kGpu, kRootPort, GpuAcpiPower::kVendorAmd, w.allocator, kIgpu);
    CHECK(q && q->mechanism() == GpuPowerMechanism::AcpiPowerResources);
    if (q) {
      CHECK(q->power_off());
      CHECK(w.integer("\\PLOG") == 0x35);
      CHECK(w.integer("\\AFUN") == 1);   // ATPX power control was not used
    }
    n.pr3_on_bridge = false;
    Vm x(n);
    CHECK(!GpuAcpiPower::find(x.vm, kGpu, kRootPort, GpuAcpiPower::kVendorAmd, x.allocator, kIgpu));
  }
  // Not hybrid, but the root port has _PR3: the power resources are preferred too.
  {
    Machine n = m;
    n.pr3_on_bridge = true;
    Vm w(n);
    UniquePointer<IGpuPlatformPower> q = GpuAcpiPower::find(w.vm, kGpu, kRootPort, GpuAcpiPower::kVendorAmd, w.allocator, kIgpu);
    CHECK(q && q->mechanism() == GpuPowerMechanism::AcpiPowerResources);
  }
  // Garbage: a short answer, a size word that is too small, an integer, a faulting parameters call, a parameters answer that is too short.
  const Bytes bad_verifies[] = {ret(buf(b({8}))), ret(buf(cat({b({4, 0, 1, 0}), le32(0x3F)}))), ret(num(0x3F)), fault(), ret(buf(b({0xFF, 0xFF, 1, 0, 0x3F})))};
  for (const Bytes& bad : bad_verifies) {
    Machine n = m;
    n.igpu_extra = atpx(bad, params_answer(0, 0));
    Vm w(n);
    CHECK(!GpuAcpiPower::find(w.vm, kGpu, kRootPort, GpuAcpiPower::kVendorAmd, w.allocator, kIgpu));
    CHECK(!GpuAcpiPower::find_mux(w.vm, kGpu, kRootPort, GpuAcpiPower::kVendorAmd, w.allocator, kIgpu));
  }
  const Bytes bad_params[] = {fault(), ret(buf(b({10, 0, 1, 2, 3}))), ret(num(1)), ret(buf(cat({b({6, 0}), le32(0), le32(0)})))};
  for (const Bytes& bad : bad_params) {
    Machine n = m;
    n.igpu_extra = atpx(verify_answer(0x3F), bad);
    Vm w(n);
    CHECK(!GpuAcpiPower::find(w.vm, kGpu, kRootPort, GpuAcpiPower::kVendorAmd, w.allocator, kIgpu));
  }
  // Power control that faults.
  {
    Machine n = m;
    n.igpu_extra = method("ATPX", 2, cat({if_(lequal(arg(0), num(0)), verify_answer(AmdAtpx::kPowerControlSupported)), fault()}));
    Vm w(n);
    UniquePointer<IGpuPlatformPower> q = GpuAcpiPower::find(w.vm, kGpu, kRootPort, GpuAcpiPower::kVendorAmd, w.allocator, kIgpu);
    CHECK(q);
    if (q) {
      CHECK(!q->power_off());
      CHECK(!q->power_on());
    }
  }
}

static void test_atpx_parsers() {
  const Bytes verify = cat({b({8, 0, 1, 0}), le32(0x0000003F)});
  CHECK(AmdAtpx::parse_verify_interface(ConstByteView(verify.data(), verify.size())));
  for (size_t n = 0; n < verify.size(); ++n) CHECK(!AmdAtpx::parse_verify_interface(ConstByteView(verify.data(), n)));   // every truncation
  const Bytes params = cat({b({10, 0}), le32(0x4000), le32(0x4020)});
  const Optional<AtpxPxParameters> pp = AmdAtpx::parse_px_parameters(ConstByteView(params.data(), params.size()));
  CHECK(pp && pp->valid_flags == 0x4000 && pp->flags == 0x4020);
  for (size_t n = 0; n < params.size(); ++n) CHECK(!AmdAtpx::parse_px_parameters(ConstByteView(params.data(), n)));
  // A longer size word than the structure is fine (newer firmware adds fields); the buffer still has to hold the structure.
  const Bytes longer = cat({b({0x20, 0}), le32(1), le32(1), b({9, 9})});
  CHECK(AmdAtpx::parse_px_parameters(ConstByteView(longer.data(), longer.size())));
  // Bit flips: any answer parses or is refused, without reading outside it.
  std::mt19937 rng(7);
  for (int i = 0; i < 2000; ++i) {
    Bytes x = rng() & 1 ? verify : params;
    x[rng() % x.size()] ^= static_cast<uint8_t>(1u << (rng() % 8));
    if (rng() % 4 == 0) x.resize(rng() % (x.size() + 1));
    const Optional<AtpxVerifyInterface> a = AmdAtpx::parse_verify_interface(ConstByteView(x.data(), x.size()));
    const Optional<AtpxPxParameters> c = AmdAtpx::parse_px_parameters(ConstByteView(x.data(), x.size()));
    if (a) (void)AmdAtpx::derive(*a, c);
  }
  // amdgpu_atpx_validate's rules.
  AtpxVerifyInterface v;
  v.functions = AmdAtpx::kPxParametersSupported;
  AtpxPxParameters p;
  p.valid_flags = AmdAtpx::kDfpSignalMuxed | AmdAtpx::kSeparateMuxForI2c;
  p.flags = AmdAtpx::kDfpSignalMuxed;   // (the I2C bit is valid but clear)
  AmdAtpxInfo info = AmdAtpx::derive(v, p);
  CHECK(info.display_mux && !info.i2c_mux && !info.power_control && !info.hybrid);
  p.flags |= AmdAtpx::kSeparateMuxForI2c | AmdAtpx::kDynamicDgpuPowerOffSupported | AmdAtpx::kDgpuRequiresPowerForDisplays;
  p.valid_flags |= AmdAtpx::kDynamicDgpuPowerOffSupported | AmdAtpx::kDgpuRequiresPowerForDisplays;
  info = AmdAtpx::derive(v, p);
  CHECK(info.display_mux && info.i2c_mux && info.power_control && info.dgpu_needs_power_for_displays);
  v.functions |= AmdAtpx::kPowerControlSupported;
  p.flags = p.valid_flags = AmdAtpx::kMsHybridGraphicsSupported;
  info = AmdAtpx::derive(v, p);
  CHECK(info.hybrid && !info.power_control);
  p.valid_flags = 0;   // a flag that is not valid does not count
  info = AmdAtpx::derive(v, p);
  CHECK(!info.hybrid && info.power_control);
}

static void test_dsm_helpers() {
  Machine m;
  m.gpu_extra = dsm({DsmUuidAnswers{kNvidiaOptimusDsmUuid, {{0, ret(buf(b({0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80, 0x01})))},
                                                            {5, ret(buf(b({})))}, {6, ret(buf(b({0x34, 0x12})))}, {7, ret(package(1, num(1)))}}}});
  Vm v(m);
  acpi::Node* d = v.vm.find("\\_SB.PCI0.PEG0.PEGP._DSM");
  CHECK(AcpiDsm::supported(v.vm, d, kNvidiaOptimusDsmUuid, 0x100) == 1);   // a long buffer: its first eight bytes
  CHECK(AcpiDsm::supported(v.vm, d, kNvidiaJtDsmUuid, 0x103) == 0);        // Buffer () {0}: nothing
  CHECK(!AcpiDsm::call_u32(v.vm, d, kNvidiaOptimusDsmUuid, 0x100, 5, DsmArgument::Integer, 0));   // an empty buffer
  const Optional<uint32_t> two = AcpiDsm::call_u32(v.vm, d, kNvidiaOptimusDsmUuid, 0x100, 6, DsmArgument::Integer, 0);
  CHECK(two && *two == 0x1234);
  CHECK(!AcpiDsm::call_u32(v.vm, d, kNvidiaOptimusDsmUuid, 0x100, 7, DsmArgument::Integer, 0));   // a package
  CHECK(!AcpiDsm::call_u32(v.vm, nullptr, kNvidiaOptimusDsmUuid, 0x100, 7, DsmArgument::Integer, 0));
  CHECK(!AcpiDsm::call_u32(v.vm, v.vm.find("\\PLOG"), kNvidiaOptimusDsmUuid, 0x100, 7, DsmArgument::Integer, 0));   // not a method
}

int main() {
  (void)&make_fadt;   // (AcpiTestSupport.hpp's builders this test does not use)
  (void)&notify;
  test_lookup();
  test_pr3_on_root_port();
  test_pr3_on_gpu();
  test_pr3_garbage();
  test_optimus();
  test_jt();
  test_nvidia_mux();
  test_atpx();
  test_atpx_parsers();
  test_dsm_helpers();
  if (g_failed) {
    printf("GpuAcpiPowerTest: %d failed\n", g_failed);
    return 1;
  }
  printf("GpuAcpiPowerTest: all passed\n");
  return 0;
}
