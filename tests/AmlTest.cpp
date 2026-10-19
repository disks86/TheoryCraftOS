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

// Host-side tests for the AML interpreter: tables are assembled here byte by byte (there is no ASL compiler at hand), loaded, and their methods evaluated
// against a pretend embedded controller. Also feeds it damaged tables, which must never stop it.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <initializer_list>
#include <vector>
#include "Hardware/Acpi/AmlInterpreter.hpp"
#include "Hardware/Acpi/EmbeddedController.hpp"
#include "Hardware/Battery/AcpiBattery.hpp"

using namespace acpi;

static int g_failed = 0;
#define CHECK(...) do { if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)

struct TestAllocator final : allocators::IAllocator {
  std::vector<void*> blocks;
  void* allocate(uint64_t size) override { return allocate_aligned(size, 16); }
  void* allocate_aligned(uint64_t size, uint64_t alignment) override { void* p = aligned_alloc(alignment, (size + alignment - 1) / alignment * alignment); blocks.push_back(p); return p; }
  void deallocate(void*) override {}
  ~TestAllocator() { for (void* p : blocks) free(p); }
};

struct FakeHost final : IAmlHost {
  uint8_t ec[256] = {};
  uint32_t ec_reads = 0, ec_writes = 0;
  uint64_t io_last_write = 0;
  bool region_read(const RegionAccess& a, uint64_t* value) override {
    if (a.space == RegionSpace::EmbeddedControl) {
      if (a.address > 255 || a.width_bits != 8) return false;
      *value = ec[a.address];
      ++ec_reads;
      return true;
    }
    if (a.space == RegionSpace::SystemMemory) {
      *value = 0x1000 + a.address;
      return true;
    }
    return false;
  }
  bool region_write(const RegionAccess& a, uint64_t value) override {
    if (a.space == RegionSpace::EmbeddedControl) {
      if (a.address > 255 || a.width_bits != 8) return false;
      ec[a.address] = static_cast<uint8_t>(value);
      ++ec_writes;
      return true;
    }
    if (a.space == RegionSpace::SystemIo) {
      io_last_write = value;
      return true;
    }
    return false;
  }
};

// --- A tiny assembler -----------------------------------------------------------------------------------------------------------------------

typedef std::vector<uint8_t> Bytes;

static Bytes cat(std::initializer_list<Bytes> parts) {
  Bytes r;
  for (const Bytes& p : parts) r.insert(r.end(), p.begin(), p.end());
  return r;
}
static Bytes b(std::initializer_list<uint8_t> v) { return Bytes(v); }
static Bytes seg(const char* s) {
  Bytes r(4, '_');
  for (int i = 0; i < 4 && s[i]; ++i) r[i] = static_cast<uint8_t>(s[i]);
  return r;
}
// A name path: "\\_SB.EC0.BSTS", "BSTS", "^FOO".
static Bytes path(const char* s) {
  Bytes r;
  if (*s == '\\') { r.push_back(0x5C); ++s; }
  while (*s == '^') { r.push_back(0x5E); ++s; }
  std::vector<Bytes> segs;
  while (*s) {
    char t[5] = {};
    int n = 0;
    while (*s && *s != '.') t[n++] = *s++;
    segs.push_back(seg(t));
    if (*s == '.') ++s;
  }
  if (segs.size() == 2) r.push_back(0x2E);
  else if (segs.size() > 2) { r.push_back(0x2F); r.push_back(static_cast<uint8_t>(segs.size())); }
  for (const Bytes& sg : segs) r.insert(r.end(), sg.begin(), sg.end());
  return r;
}
// A PkgLength-wrapped body.
static Bytes pkg(const Bytes& body) {
  Bytes r;
  const uint32_t n = static_cast<uint32_t>(body.size());
  if (n + 1 < 64) r.push_back(static_cast<uint8_t>(n + 1));
  else if (n + 2 < 4096) { const uint32_t t = n + 2; r.push_back(static_cast<uint8_t>(0x40 | (t & 0xF))); r.push_back(static_cast<uint8_t>(t >> 4)); }
  else { const uint32_t t = n + 3; r.push_back(static_cast<uint8_t>(0x80 | (t & 0xF))); r.push_back(static_cast<uint8_t>(t >> 4)); r.push_back(static_cast<uint8_t>(t >> 12)); }
  r.insert(r.end(), body.begin(), body.end());
  return r;
}
static Bytes num(uint64_t v) {
  if (v == 0) return b({0x00});
  if (v == 1) return b({0x01});
  if (v <= 0xFF) return b({0x0A, static_cast<uint8_t>(v)});
  if (v <= 0xFFFF) return b({0x0B, static_cast<uint8_t>(v), static_cast<uint8_t>(v >> 8)});
  if (v <= 0xFFFFFFFFull) return b({0x0C, static_cast<uint8_t>(v), static_cast<uint8_t>(v >> 8), static_cast<uint8_t>(v >> 16), static_cast<uint8_t>(v >> 24)});
  Bytes r = b({0x0E});
  for (int i = 0; i < 8; ++i) r.push_back(static_cast<uint8_t>(v >> (8 * i)));
  return r;
}
static Bytes str(const char* s) { Bytes r = b({0x0D}); while (*s) r.push_back(static_cast<uint8_t>(*s++)); r.push_back(0); return r; }
static Bytes name_def(const char* n, const Bytes& value) { return cat({b({0x08}), path(n), value}); }
static Bytes method(const char* n, int argc, const Bytes& body) { return cat({b({0x14}), pkg(cat({path(n), b({static_cast<uint8_t>(argc)}), body}))}); }
static Bytes device(const char* n, const Bytes& body) { return cat({b({0x5B, 0x82}), pkg(cat({path(n), body}))}); }
static Bytes scope(const char* n, const Bytes& body) { return cat({b({0x10}), pkg(cat({path(n), body}))}); }
static Bytes region(const char* n, uint8_t space, uint64_t offset, uint64_t length) { return cat({b({0x5B, 0x80}), path(n), b({space}), num(offset), num(length)}); }
static Bytes field_len(uint32_t n) { return n < 64 ? b({static_cast<uint8_t>(n)}) : b({static_cast<uint8_t>(0x40 | (n & 0xF)), static_cast<uint8_t>(n >> 4)}); }
static Bytes fld(const char* n, uint32_t bits) { return cat({seg(n), field_len(bits)}); }
static Bytes skip(uint32_t bits) { return cat({b({0x00}), field_len(bits)}); }
static Bytes field(const char* region_name, uint8_t flags, const Bytes& list) { return cat({b({0x5B, 0x81}), pkg(cat({path(region_name), b({flags}), list}))}); }
static Bytes store(const Bytes& src, const Bytes& dst) { return cat({b({0x70}), src, dst}); }
static Bytes ret(const Bytes& v) { return cat({b({0xA4}), v}); }
static Bytes op2(uint8_t op, const Bytes& a, const Bytes& c) { return cat({b({op}), a, c}); }
static Bytes add(const Bytes& a, const Bytes& c) { return cat({b({0x72}), a, c, b({0x00})}); }
static Bytes sub(const Bytes& a, const Bytes& c) { return cat({b({0x74}), a, c, b({0x00})}); }
static Bytes lequal(const Bytes& a, const Bytes& c) { return op2(0x93, a, c); }
static Bytes lless(const Bytes& a, const Bytes& c) { return op2(0x95, a, c); }
static Bytes if_(const Bytes& cond, const Bytes& body) { return cat({b({0xA0}), pkg(cat({cond, body}))}); }
static Bytes else_(const Bytes& body) { return cat({b({0xA1}), pkg(body)}); }
static Bytes while_(const Bytes& cond, const Bytes& body) { return cat({b({0xA2}), pkg(cat({cond, body}))}); }
static Bytes package(uint8_t count, const Bytes& elements) { return cat({b({0x12}), pkg(cat({b({count}), elements}))}); }
static Bytes buffer(const Bytes& size, const Bytes& init) { return cat({b({0x11}), pkg(cat({size, init}))}); }
static Bytes index(const Bytes& src, const Bytes& i) { return cat({b({0x88}), src, i, b({0x00})}); }
static Bytes index_to(const Bytes& src, const Bytes& i, const Bytes& target) { return cat({b({0x88}), src, i, target}); }
static const Bytes kLocal0 = b({0x60}), kLocal1 = b({0x61}), kArg0 = b({0x68}), kArg1 = b({0x69});
static const Bytes kOne = b({0x01}), kZero = b({0x00});

static Bytes make_table(const char* signature, const Bytes& aml, uint8_t revision = 2) {
  Bytes t(36, 0);
  memcpy(&t[0], signature, 4);
  const uint32_t length = static_cast<uint32_t>(36 + aml.size());
  for (int i = 0; i < 4; ++i) t[4 + i] = static_cast<uint8_t>(length >> (8 * i));
  t[8] = revision;
  t.insert(t.end(), aml.begin(), aml.end());
  return t;
}

// --- The pretend machine -------------------------------------------------------------------------------------------------------------------

static Bytes machine_aml() {
  const Bytes ec = device("EC0", cat({
      name_def("_HID", num(0x090CD041)),
      name_def("_CRS", buffer(num(18), b({0x47, 0x01, 0x62, 0x00, 0x62, 0x00, 0x01, 0x01, 0x47, 0x01, 0x66, 0x00, 0x66, 0x00, 0x01, 0x01, 0x79, 0x00}))),
      region("ECOR", 3, 0, 0x100),
      field("ECOR", 0x01, cat({skip(0x10 * 8), fld("BSTS", 8), fld("BRAT", 16), fld("BREM", 16), fld("BVLT", 16), skip(8 * 9), fld("BFUL", 16), fld("BDES", 16),
                                skip(8 * 12), fld("FLG0", 1), fld("FLG1", 3), fld("FLG2", 4), fld("WIDE", 128)}))}));
  const Bytes battery = device("BAT0", cat({
      name_def("_HID", num(0x0A0CD041)),
      method("_STA", 0, ret(num(0x1F))),
      name_def("PBST", package(4, cat({kZero, kZero, kZero, kZero}))),
      method("_BST", 0, cat({store(path("\\_SB.EC0.BSTS"), index_to(path("PBST"), num(0), kZero)),
                             store(path("\\_SB.EC0.BRAT"), index_to(path("PBST"), num(1), kZero)),
                             store(path("\\_SB.EC0.BREM"), index_to(path("PBST"), num(2), kZero)),
                             store(path("\\_SB.EC0.BVLT"), index_to(path("PBST"), num(3), kZero)),
                             ret(path("PBST"))})),
      name_def("PBIF", package(3, cat({kOne, kZero, kZero}))),
      method("_BIF", 0, cat({store(path("\\_SB.EC0.BDES"), index_to(path("PBIF"), num(1), kZero)),
                             store(path("\\_SB.EC0.BFUL"), index_to(path("PBIF"), num(2), kZero)),
                             ret(path("PBIF"))}))}));
  // Methods that exercise the language.
  const Bytes tests = device("TST0", cat({
      method("ADD1", 1, ret(add(kArg0, kOne))),
      method("FIB", 1, cat({if_(lless(kArg0, num(2)), ret(kArg0)),
                            ret(add(path("FIB") .size() ? cat({path("FIB"), sub(kArg0, kOne)}) : Bytes(), cat({path("FIB"), sub(kArg0, num(2))})))})),
      method("SUMN", 1, cat({store(kZero, kLocal0), store(kArg0, kLocal1),
                             while_(kLocal1, cat({cat({b({0x72}), kLocal0, kLocal1, kLocal0}), b({0x76, 0x61})})),
                             ret(kLocal0)})),
      method("BRK", 0, cat({store(kZero, kLocal0), while_(kOne, cat({b({0x75, 0x60}), if_(lequal(kLocal0, num(5)), b({0xA5}))})), ret(kLocal0)})),
      method("BUFF", 0, cat({name_def("BUF0", buffer(num(8), b({1, 2, 3, 4, 5, 6, 7, 8}))), b({0x8A}), path("BUF0"), num(2), path("DW00"), ret(path("DW00"))})),
      method("HANG", 0, while_(kOne, kZero)),
      method("OSIY", 0, ret(cat({path("\\_OSI"), str("Windows 2015")}))),
      method("OSIN", 0, ret(cat({path("\\_OSI"), str("Linux")}))),
      method("STRL", 0, ret(cat({b({0x87}), str("hello")}))),
      method("IDX", 0, ret(cat({b({0x83}), index(package(3, cat({num(10), num(20), num(30)})), num(1))}))),
      method("ELSE", 1, cat({if_(lequal(kArg0, num(1)), ret(num(100))), else_(ret(num(200)))})),
      method("TMP", 0, cat({store(num(5), path("\\_SB.EC0.FLG1")), ret(path("\\_SB.EC0.FLG1"))})),
      method("BADN", 0, ret(path("NOPE"))),
      method("MULS", 0, ret(cat({b({0x77}), num(6), num(7), kZero}))),
      method("DIVS", 0, cat({cat({b({0x78}), num(17), num(5), kLocal0, kLocal1}), ret(add(kLocal0, cat({b({0x79}), kLocal1, num(8), kZero})))})),
      name_def("VAL0", num(0x12345678)),
      method("GETV", 0, ret(path("VAL0"))),
      method("SETV", 1, cat({store(kArg0, path("VAL0")), ret(path("VAL0"))})),
      name_def("MEMR", num(0)),
      region("MEMX", 0, 0x4000, 0x10),
      field("MEMX", 0x03, cat({fld("MEM0", 32)})),
      method("MEMV", 0, ret(path("MEM0"))),
  }));
  return cat({scope("\\_SB", cat({ec, battery, tests}))});
}

static void test_basics() {
  TestAllocator a;
  FakeHost host;
  AmlInterpreter vm(a, host);
  Bytes table = make_table("DSDT", machine_aml());
  CHECK(vm.load_table(ConstByteView(table.data(), table.size())));
  CHECK(!vm.failed());

  // Finding things, and device IDs.
  Node* bat = vm.find("\\_SB.BAT0");
  Node* ec = vm.find("\\_SB.EC0");
  CHECK(bat && ec && vm.is_device(bat) && vm.is_device(ec) && !vm.find("\\_SB.NOPE"));
  CHECK(vm.device_has_id(bat, "PNP0C0A") && !vm.device_has_id(bat, "PNP0C09") && vm.device_has_id(ec, "PNP0C09"));
  char id[8];
  eisa_id_to_string(0x0A0CD041, id);
  CHECK(strcmp(id, "PNP0C0A") == 0);
  eisa_id_to_string(0x0105D041, id);
  CHECK(strcmp(id, "PNP0501") == 0);

  uint64_t v = 0;
  CHECK(vm.evaluate_integer("\\_SB.BAT0._STA", &v) && v == 0x1F);

  // The battery methods read the controller's fields.
  host.ec[0x10] = 0x01;                                // discharging
  host.ec[0x11] = 0x34; host.ec[0x12] = 0x12;          // rate 0x1234
  host.ec[0x13] = 0x00; host.ec[0x14] = 0x10;          // remaining 0x1000
  host.ec[0x15] = 0x88; host.ec[0x16] = 0x2F;          // voltage 0x2F88
  host.ec[0x20] = 0x00; host.ec[0x21] = 0x20;          // full charge 0x2000
  host.ec[0x22] = 0x00; host.ec[0x23] = 0x28;          // design 0x2800
  Value r;
  CHECK(vm.evaluate("\\_SB.BAT0._BST", &r));
  CHECK(r.kind == ValueKind::Package && r.length == 4);
  CHECK(r.elements[0].integer == 1 && r.elements[1].integer == 0x1234 && r.elements[2].integer == 0x1000 && r.elements[3].integer == 0x2F88);
  host.ec[0x10] = 0x02; host.ec[0x14] = 0x08;           // charging, less remaining
  CHECK(vm.evaluate("\\_SB.BAT0._BST", &r) && r.elements[0].integer == 2 && r.elements[2].integer == 0x0800);   // (the package is updated, not remade)
  CHECK(vm.evaluate("\\_SB.BAT0._BIF", &r) && r.length == 3 && r.elements[0].integer == 1 && r.elements[1].integer == 0x2800 && r.elements[2].integer == 0x2000);

  // Language.
  Value arg;
  arg.kind = ValueKind::Integer;
  arg.integer = 41;
  CHECK(vm.evaluate(vm.find("\\_SB.TST0.ADD1"), &arg, 1, &r) && r.integer == 42);
  arg.integer = 7;
  CHECK(vm.evaluate(vm.find("\\_SB.TST0.FIB"), &arg, 1, &r) && r.integer == 13);
  arg.integer = 30;                                   // recursion deeper than allowed is refused, not run into the ground
  CHECK(!vm.evaluate(vm.find("\\_SB.TST0.FIB"), &arg, 1, &r) && vm.failed());
  arg.integer = 10;
  CHECK(vm.evaluate(vm.find("\\_SB.TST0.SUMN"), &arg, 1, &r) && r.integer == 55);
  CHECK(vm.evaluate("\\_SB.TST0.BRK", &r) && r.integer == 5);
  CHECK(vm.evaluate("\\_SB.TST0.BUFF", &r) && r.integer == 0x06050403);
  CHECK(vm.evaluate("\\_SB.TST0.OSIY", &r) && r.integer == ~0ull);
  CHECK(vm.evaluate("\\_SB.TST0.OSIN", &r) && r.integer == 0);
  CHECK(vm.evaluate("\\_SB.TST0.STRL", &r) && r.integer == 5);
  CHECK(vm.evaluate("\\_SB.TST0.IDX", &r) && r.integer == 20);
  arg.integer = 1;
  CHECK(vm.evaluate(vm.find("\\_SB.TST0.ELSE"), &arg, 1, &r) && r.integer == 100);
  arg.integer = 2;
  CHECK(vm.evaluate(vm.find("\\_SB.TST0.ELSE"), &arg, 1, &r) && r.integer == 200);
  CHECK(vm.evaluate("\\_SB.TST0.MULS", &r) && r.integer == 42);
  CHECK(vm.evaluate("\\_SB.TST0.DIVS", &r) && r.integer == 2 + (3 << 8));
  CHECK(vm.evaluate("\\_SB.TST0.GETV", &r) && r.integer == 0x12345678);
  arg.integer = 7;
  CHECK(vm.evaluate(vm.find("\\_SB.TST0.SETV"), &arg, 1, &r) && r.integer == 7);
  CHECK(vm.evaluate("\\_SB.TST0.GETV", &r) && r.integer == 7);

  // Bit fields: 1, 3 and 4 bits in one byte; writing keeps the rest.
  host.ec[0x30] = 0xA5;
  CHECK(vm.evaluate("\\_SB.EC0.FLG0", &r) && r.integer == 1);
  CHECK(vm.evaluate("\\_SB.EC0.FLG1", &r) && r.integer == 2);
  CHECK(vm.evaluate("\\_SB.EC0.FLG2", &r) && r.integer == 10);
  CHECK(vm.evaluate("\\_SB.TST0.TMP", &r) && r.integer == 5 && host.ec[0x30] == 0xAB);
  // A 128-bit field comes back as a buffer.
  for (int i = 0; i < 16; ++i) host.ec[0x31 + i] = static_cast<uint8_t>(0x40 + i);
  CHECK(vm.evaluate("\\_SB.EC0.WIDE", &r) && r.kind == ValueKind::Buffer && r.length == 16 && r.bytes[0] == 0x40 && r.bytes[15] == 0x4F);
  // A memory region reads 32 bits at once, from the region's offset.
  CHECK(vm.evaluate("\\_SB.TST0.MEMV", &r) && r.integer == 0x1000 + 0x4000);

  // Things that must fail and not hurt.
  CHECK(!vm.evaluate("\\_SB.TST0.HANG", &r) && vm.failed());
  CHECK(!vm.evaluate("\\_SB.TST0.BADN", &r) && vm.failed());
  CHECK(vm.evaluate("\\_SB.BAT0._STA", &r) && !vm.failed() && r.integer == 0x1F);   // still fine afterwards

  // Walking the namespace finds the batteries.
  uint32_t batteries = 0;
  for (Node* n = vm.first_child(vm.find("\\_SB")); n; n = vm.next_sibling(n))
    if (vm.is_device(n) && vm.device_has_id(n, "PNP0C0A")) ++batteries;
  CHECK(batteries == 1);

  // Memory use is steady: repeated evaluation does not pile up.
  for (int i = 0; i < 2000; ++i) vm.evaluate("\\_SB.BAT0._BST", &r);
  CHECK(vm.evaluate("\\_SB.BAT0._BST", &r));
}

static void test_32_bit_tables() {
  TestAllocator a;
  FakeHost host;
  AmlInterpreter vm(a, host);
  Bytes table = make_table("DSDT", cat({scope("\\_SB", cat({method("BIG", 0, ret(add(num(0xFFFFFFFFull), num(2)))), method("ONES", 0, ret(b({0xFF})))}))}), 1);
  CHECK(vm.load_table(ConstByteView(table.data(), table.size())));
  Value r;
  CHECK(vm.evaluate("\\_SB.BIG", &r) && r.integer == 1);           // revision 1: integers are 32 bits
  CHECK(vm.evaluate("\\_SB.ONES", &r) && r.integer == 0xFFFFFFFFull);
}

static void test_second_table() {
  TestAllocator a;
  FakeHost host;
  AmlInterpreter vm(a, host);
  Bytes dsdt = make_table("DSDT", cat({scope("\\_SB", cat({name_def("AAAA", num(1)), device("DEV0", name_def("_HID", str("ACPI0003")))}))}));
  Bytes ssdt = make_table("SSDT", cat({scope("\\_SB.DEV0", method("_PSR", 0, ret(num(1)))), scope("\\_SB", name_def("BBBB", num(2)))}));
  CHECK(vm.load_table(ConstByteView(dsdt.data(), dsdt.size())));
  CHECK(vm.load_table(ConstByteView(ssdt.data(), ssdt.size())));
  uint64_t v = 0;
  CHECK(vm.evaluate_integer("\\_SB.DEV0._PSR", &v) && v == 1);
  CHECK(vm.evaluate_integer("\\_SB.AAAA", &v) && v == 1 && vm.evaluate_integer("\\_SB.BBBB", &v) && v == 2);
  CHECK(vm.device_has_id(vm.find("\\_SB.DEV0"), "ACPI0003"));
}

// Damaged tables: cut short, bytes changed. Loading and evaluating everything must neither crash nor hang (the sanitizers watch).
static void test_damaged() {
  const Bytes good = make_table("DSDT", machine_aml());
  uint32_t seed = 12345;
  auto rnd = [&]() { seed = seed * 1664525u + 1013904223u; return seed >> 8; };
  for (int round = 0; round < 1500; ++round) {
    Bytes t = good;
    const int mode = round % 3;
    if (mode == 0) {
      t.resize(36 + rnd() % (t.size() - 36));            // cut short (the length field is fixed up)
      for (int i = 0; i < 4; ++i) t[4 + i] = static_cast<uint8_t>(t.size() >> (8 * i));
    } else {
      const int changes = 1 + rnd() % 6;
      for (int i = 0; i < changes; ++i) t[36 + rnd() % (t.size() - 36)] = static_cast<uint8_t>(rnd());
    }
    TestAllocator a;
    FakeHost host;
    AmlInterpreter vm(a, host);
    vm.load_table(ConstByteView(t.data(), t.size()));
    // Evaluate everything in the namespace.
    struct Walk {
      static void go(AmlInterpreter& vm, Node* n, int depth) {
        for (Node* c = vm.first_child(n); c; c = vm.next_sibling(c)) {
          if (depth < 8) go(vm, c, depth + 1);
          Value r;
          if (!vm.is_region(c)) vm.evaluate(c, nullptr, 0, &r);
        }
      }
    };
    Walk::go(vm, vm.root(), 0);
  }
}

// --- The embedded controller's protocol ------------------------------------------------------------------------------------------------

// A pretend controller on the two ports: takes a command, then bytes, the way the specification describes; the input buffer reads "full" for a few polls
// after each write.
struct FakeEcPorts final : hardware::IEcPorts {
  uint8_t memory[256] = {};
  uint16_t data_port = 0x62, command_port = 0x66;
  enum { Idle, ReadAddress, WriteAddress, WriteData } state = Idle;
  uint8_t address = 0, output = 0;
  bool output_full = false;
  int busy_polls = 0;
  bool stuck = false;            // never accepts anything
  uint32_t polls = 0, delays = 0;
  bool stale_event = false;      // an old answer waits in the output buffer
  uint8_t in8(uint16_t port) override {
    if (port == command_port) {
      ++polls;
      uint8_t status = 0;
      if (output_full) status |= 1;
      if (stuck) status |= 2;
      else if (busy_polls > 0) { status |= 2; --busy_polls; }
      return status;
    }
    if (port == data_port) { output_full = false; return output; }
    return 0xFF;
  }
  void out8(uint16_t port, uint8_t value) override {
    busy_polls = 2;
    if (port == command_port) {
      state = value == 0x80 ? ReadAddress : value == 0x81 ? WriteAddress : Idle;
    } else if (port == data_port) {
      if (state == ReadAddress) { output = memory[value]; output_full = true; state = Idle; }
      else if (state == WriteAddress) { address = value; state = WriteData; }
      else if (state == WriteData) { memory[address] = value; state = Idle; }
    }
  }
  void delay_us(uint32_t) override { ++delays; }
};

static void test_embedded_controller() {
  FakeEcPorts ports;
  hardware::EmbeddedController ec(ports);
  uint8_t v = 0;
  CHECK(!ec.read(0, &v));                      // not configured
  ec.configure(0x62, 0x66);
  ports.memory[0x12] = 0x5A;
  CHECK(ec.read(0x12, &v) && v == 0x5A && ports.delays > 0);
  CHECK(ec.write(0x20, 0xC3) && ports.memory[0x20] == 0xC3);
  CHECK(ec.read(0x20, &v) && v == 0xC3);
  ports.output = 0x77; ports.output_full = true;   // a leftover answer is dropped, not mistaken for ours
  ports.memory[0x30] = 0x11;
  CHECK(ec.read(0x30, &v) && v == 0x11);
  ports.stuck = true;
  CHECK(!ec.read(0x12, &v) && !ec.broken());
  CHECK(!ec.read(0x12, &v) && !ec.read(0x12, &v) && ec.broken());
  ports.stuck = false;
  CHECK(!ec.read(0x12, &v));                   // given up on
  ec.configure(0x62, 0x66);                    // (configuring again starts over)
  CHECK(ec.read(0x12, &v) && v == 0x5A);
}

// --- The battery ---------------------------------------------------------------------------------------------------------------------------

struct FakeAcpiHost final : hardware::IAcpiHost {
  uint8_t ec[256] = {};
  uint16_t data_port = 0, command_port = 0;
  uint32_t accesses = 0;
  bool fail_ec = false;
  bool region_read(const acpi::RegionAccess& a, uint64_t* value) override {
    ++accesses;
    if (a.space != RegionSpace::EmbeddedControl || fail_ec || a.address > 255) return false;
    *value = ec[a.address];
    return true;
  }
  bool region_write(const acpi::RegionAccess& a, uint64_t value) override {
    ++accesses;
    if (a.space != RegionSpace::EmbeddedControl || fail_ec || a.address > 255) return false;
    ec[a.address] = static_cast<uint8_t>(value);
    return true;
  }
  void configure_ec(uint16_t data, uint16_t command) override { data_port = data; command_port = command; }
};

static void test_battery() {
  TestAllocator a;
  FakeAcpiHost host;
  hardware::AcpiBattery battery(host, a);
  Bytes table = make_table("DSDT", machine_aml());
  ConstByteView tables[1] = {ConstByteView(table.data(), table.size())};
  CHECK(battery.initialize(tables, 1) && battery.battery_count() == 1);
  CHECK(host.data_port == 0x62 && host.command_port == 0x66);   // from the controller's _CRS

  host.ec[0x13] = 0x00; host.ec[0x14] = 0x10;    // remaining 0x1000
  host.ec[0x20] = 0x00; host.ec[0x21] = 0x20;    // last full 0x2000
  host.ec[0x22] = 0x00; host.ec[0x23] = 0x28;    // design 0x2800
  host.ec[0x10] = 1;                             // discharging
  hardware::BatteryStatus s = battery.read();
  CHECK(s.present && s.percent == 50 && s.state == hardware::BatteryState::Discharging);
  host.ec[0x10] = 2;
  s = battery.read();
  CHECK(s.percent == 50 && s.state == hardware::BatteryState::Charging);
  host.ec[0x10] = 0;                             // neither: holding below full
  s = battery.read();
  CHECK(s.percent == 50 && s.state == hardware::BatteryState::NotCharging);
  host.ec[0x14] = 0x20;                          // remaining = last full
  s = battery.read();
  CHECK(s.percent == 100 && s.state == hardware::BatteryState::Full);
  host.ec[0x13] = 0xFF; host.ec[0x14] = 0xFF;    // more than the last full charge says: capped
  CHECK(battery.read().percent == 100);

  // The controller stops answering: unknown, not a wrong number; and it recovers.
  host.fail_ec = true;
  s = battery.read();
  CHECK(s.percent == -1 || s.percent == 100);
  for (int i = 0; i < 6; ++i) s = battery.read();
  CHECK(s.percent == -1 && s.state == hardware::BatteryState::Unknown);
  host.fail_ec = false;
  host.ec[0x13] = 0x00; host.ec[0x14] = 0x08;
  s = battery.read();
  CHECK(s.present && s.percent == 25);
}

static void test_no_battery() {
  TestAllocator a;
  FakeAcpiHost host;
  hardware::AcpiBattery battery(host, a);
  // A desktop: a controller but no battery device. Nothing is touched.
  Bytes table = make_table("DSDT", scope("\\_SB", device("EC0", cat({name_def("_HID", num(0x090CD041)), region("ECOR", 3, 0, 0x100)}))));
  ConstByteView tables[1] = {ConstByteView(table.data(), table.size())};
  CHECK(!battery.initialize(tables, 1) && battery.battery_count() == 0);
  CHECK(host.accesses == 0 && host.command_port == 0);
  hardware::BatteryStatus s = battery.read();
  CHECK(!s.present && s.percent == -1);
  // A battery bay without a battery in it.
  FakeAcpiHost host2;
  hardware::AcpiBattery empty(host2, a);
  Bytes table2 = make_table("DSDT", scope("\\_SB", device("BAT0", cat({name_def("_HID", num(0x0A0CD041)), method("_STA", 0, ret(num(0x0F)))}))));
  ConstByteView tables2[1] = {ConstByteView(table2.data(), table2.size())};
  CHECK(empty.initialize(tables2, 1));
  s = empty.read();
  CHECK(!s.present && s.percent == -1);
}

int main() {
  test_embedded_controller();
  test_battery();
  test_no_battery();
  test_basics();
  test_32_bit_tables();
  test_second_table();
  test_damaged();
  if (g_failed) {
    printf("%d check(s) failed\n", g_failed);
    return 1;
  }
  printf("aml tests passed\n");
  return 0;
}
