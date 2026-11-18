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

// The AMD Sensor Fusion Hub driver against a model of the MP2 that decodes commands with the driver's own layout table (AmdSfhRegisters.hpp), so
// this checks the driver's logic - detection, start-up order, discovery, enabling, scaling, timeouts, untrusted values, suspend/stop, concurrent
// reads - not the register numbers, which are unverified. Also the PCI path (command register, BAR 2) on plain memory. Real threads
// (tests/HostThreads.cpp), ASan and UBSan.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <atomic>
#include <map>
#include <mutex>
#include <set>
#include <thread>
#include <vector>
#include "Hardware/Sensor/AmdSfh/AmdSfhDriver.hpp"

namespace hardware { void delay_us(uint64_t) {} }
using namespace hardware;
namespace L = amd_sfh_layout;

static int g_failed = 0;
#define CHECK(...) do { if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)

struct TestAllocator final : allocators::IAllocator {
  std::mutex lock;
  std::set<void*> live, freed_dma;
  std::set<void*> dma;
  void* allocate(uint64_t size) override { return allocate_aligned(size, 16); }
  void* allocate_aligned(uint64_t size, uint64_t alignment) override {
    void* p = aligned_alloc(alignment, (size + alignment - 1) / alignment * alignment);
    std::lock_guard<std::mutex> g(lock);
    live.insert(p);
    return p;
  }
  void* allocate_dma(uint64_t size, uint64_t alignment) override {
    void* p = allocate_aligned(size, alignment);
    std::lock_guard<std::mutex> g(lock);
    dma.insert(p);
    return p;
  }
  void deallocate(void* p) override {
    if (!p) return;
    std::lock_guard<std::mutex> g(lock);
    if (dma.count(p)) { dma.erase(p); freed_dma.insert(p); }
    live.erase(p);
    free(p);
  }
  ~TestAllocator() { for (void* p : live) free(p); }      // (buffers the driver left to the firmware on purpose)
};

static float bits_float(uint32_t b) { float f; memcpy(&f, &b, 4); return f; }
static uint32_t float_bits(float f) { uint32_t b; memcpy(&b, &f, 4); return b; }

// The MP2: mailbox registers, the firmware's answers, its writes into the DMA buffers (1.0) or the shared memory (1.1).
struct FakeMp2 final : IAmdSfhRegisters {
  AmdSfhGeneration generation;
  uint32_t interface_version = 2, mask = 0;            // 1.0: P2C_MSG3 = version | mask << 4
  uint32_t shared_base = 0x123;                        // 1.1: C2P_MSG22
  bool answer = true;                                  // answers commands (v2, 1.1)
  bool answer_disable = true;
  uint32_t answer_after_reads = 0;                     // the answer appears only after this many reads of P2C_MSG0
  int32_t sensor_id_offset = 0;                        // garbage: answers name another sensor
  bool dead = false;                                   // every read is all ones
  std::map<uint32_t, uint32_t> reg;
  std::map<uint8_t, uint32_t*> buffers;                // sensor id -> the DMA buffer the host gave
  std::set<uint8_t> enabled;
  struct Command { uint32_t cmd; uint8_t sensor; uint32_t raw; uint64_t address; };
  std::vector<Command> log;
  std::vector<uint32_t> writes;                         // offsets, in order
  std::atomic<uint32_t> response_reads{0};
  uint32_t pending_response = 0, reads_until_answer = 0;
  bool have_pending = false;
  std::vector<uint32_t> shared = std::vector<uint32_t>(L::kSharedMemoryBytes / 4);

  explicit FakeMp2(AmdSfhGeneration g) : generation(g) {}

  uint32_t read32(uint32_t offset) override {
    if (dead) return 0xFFFFFFFFu;
    if (offset == L::kActiveStatusRegister && generation != AmdSfhGeneration::Sfh11) return interface_version | (mask << 4);
    if (offset == L::kSharedMemoryRegister && generation == AmdSfhGeneration::Sfh11) return shared_base;
    if (offset == L::kResponseRegister) {
      ++response_reads;
      if (have_pending && reads_until_answer > 0) --reads_until_answer;
      else if (have_pending) { reg[offset] = pending_response; have_pending = false; }
    }
    return reg[offset];
  }
  void write64(uint32_t offset, uint64_t value) override {
    writes.push_back(offset);
    reg[offset] = static_cast<uint32_t>(value);
    reg[offset + 4] = static_cast<uint32_t>(value >> 32);
  }
  void write32(uint32_t offset, uint32_t value) override {
    writes.push_back(offset);
    reg[offset] = value;
    if (offset == L::kCommandRegister) command(value);
  }
  uint64_t reg64(uint32_t offset) { return reg[offset] | (uint64_t{reg[offset + 4]} << 32); }

  void command(uint32_t v) {
    Command c{};
    c.raw = v;
    uint32_t answer_value = 0;
    if (generation == AmdSfhGeneration::Sfh10V1) {
      c.cmd = v & 0xFF; c.sensor = static_cast<uint8_t>(v >> 8); c.address = reg64(L::kV1AddressRegister);
    } else if (generation == AmdSfhGeneration::Sfh10V2) {
      c.cmd = (v >> 10) & 0xF; c.sensor = static_cast<uint8_t>(v >> 24); c.address = reg64(L::kV2AddressRegister);
      const uint32_t code = c.cmd == L::kCmdEnable ? L::kV2SensorEnabled : L::kV2SensorDisabled;
      answer_value = (code << 8) | ((static_cast<uint32_t>(c.sensor + sensor_id_offset) & 0x3F) << 20);
    } else {
      c.sensor = static_cast<uint8_t>(v); c.cmd = (v >> 8) & 0xF;
      answer_value = ((static_cast<uint32_t>(c.sensor + sensor_id_offset) & 0xFF) << 8) | (c.cmd << 16);
    }
    log.push_back(c);
    if (c.cmd == L::kCmdEnable) {
      enabled.insert(c.sensor);
      if (generation != AmdSfhGeneration::Sfh11) buffers[c.sensor] = reinterpret_cast<uint32_t*>(c.address);   // (the test's bus address is the pointer)
    } else if (c.cmd == L::kCmdDisable) {
      enabled.erase(c.sensor);
    } else if (c.cmd == L::kCmdStopAll) {
      enabled.clear();
    }
    const bool answers = generation != AmdSfhGeneration::Sfh10V1 && answer && (c.cmd != L::kCmdDisable || answer_disable);
    if (answers) { pending_response = answer_value; have_pending = true; reads_until_answer = answer_after_reads; }
  }

  // The firmware's writes.
  void set10(uint8_t sensor, int32_t x, int32_t y = 0, int32_t z = 0) {
    uint32_t* b = buffers[sensor];
    if (!b) return;
    __atomic_store_n(&b[0], static_cast<uint32_t>(x), __ATOMIC_RELEASE);
    __atomic_store_n(&b[1], static_cast<uint32_t>(y), __ATOMIC_RELEASE);
    __atomic_store_n(&b[2], static_cast<uint32_t>(z), __ATOMIC_RELEASE);
  }
  void metadata(uint32_t version, uint32_t list) { shared[L::kV11FirmwareVersion / 4] = version; shared[L::kV11SensorList / 4] = list; }
  void set11_raw(uint8_t sensor, uint32_t x, uint32_t y = 0, uint32_t z = 0) {
    const uint32_t block = L::v11_block(sensor) / 4;
    shared[block + L::kV11Values / 4] = x;
    shared[block + L::kV11Values / 4 + 1] = y;
    shared[block + L::kV11Values / 4 + 2] = z;
    ++shared[block + L::kV11InterruptCount / 4];
  }
  void set11(uint8_t sensor, float x, float y = 0, float z = 0) { set11_raw(sensor, float_bits(x), float_bits(y), float_bits(z)); }
  uint32_t count_commands(uint32_t cmd) const { uint32_t n = 0; for (const Command& c : log) n += c.cmd == cmd; return n; }
};

// Maps the 1.1 shared memory onto the model's array (and, for the PCI test, BAR 2 onto a plain buffer).
struct FakeMapper final : IRegisterMapper {
  std::map<uint64_t, std::pair<void*, uint64_t>> windows;
  std::vector<std::pair<uint64_t, uint64_t>> asked;
  bool map_registers(uint64_t physical, uint64_t length, MmioRegion* region) override {
    asked.push_back({physical, length});
    auto it = windows.find(physical);
    if (it == windows.end() || length > it->second.second) return false;
    *region = MmioRegion(reinterpret_cast<uint64_t>(it->second.first), length);
    return true;
  }
  void add(uint64_t physical, void* memory, uint64_t length) { windows[physical] = {memory, length}; }
  void add_shared(FakeMp2& mp2) { add(uint64_t{mp2.shared_base} << L::kSharedMemoryShift, mp2.shared.data(), mp2.shared.size() * 4); }
};

struct FakePci final : IPciBus {
  PciDeviceInfo info{{0, 8, 2}, 0x1022, 0x15E4, 0x11, 0x80, 0};
  uint32_t command = 0x0000;
  uint64_t bar2 = 0xFE000000ull;
  const char* name() const override { return "fake pci"; }
  bool initialize() override { return true; }
  void shutdown() override {}
  uint32_t device_count() const override { return 1; }
  bool pci_device(uint32_t index, PciDeviceInfo* out) const override { if (index) return false; *out = info; return true; }
  uint32_t read32(PciAddress, uint16_t offset) const override { return offset == 4 ? command : 0; }
  void write32(PciAddress, uint16_t offset, uint32_t value) const override { if (offset == 4) const_cast<FakePci*>(this)->command = value; }
  uint64_t memory_bar(PciAddress, uint8_t index) const override { return index == 2 ? bar2 : 0; }
  uint16_t find_capability(PciAddress, uint8_t) const override { return 0; }
};

constexpr uint8_t kAccel = 0, kGyro = 1, kMag = 2, kPresence = 5, kAls = 19;
constexpr uint32_t kMaskAll10 = (1u << 0) | (1u << 1) | (1u << 2) | (1u << 16) | (1u << 19);

static bool near(float a, float b) { return fabsf(a - b) <= 1e-4f * (fabsf(b) > 1 ? fabsf(b) : 1); }

// Reads until the sensor has a sample (the zero grace) and returns it.
static bool sample(ISensor* s, SensorReading* r) {
  for (uint32_t i = 0; i < AmdSfhDriver::kZeroGraceReads + 1; ++i)
    if (s->read(r)) return true;
  return false;
}

static void test_detection() {
  PciDeviceInfo d{{0, 0, 0}, 0x1022, 0x15E4, 0, 0, 0};
  CHECK(AmdSfhDriver::supports(d));
  d.device_id = 0x164A;
  CHECK(AmdSfhDriver::supports(d));
  d.device_id = 0x15E5;
  CHECK(!AmdSfhDriver::supports(d));
  d.vendor_id = 0x8086; d.device_id = 0x15E4;
  CHECK(!AmdSfhDriver::supports(d));

  TestAllocator a;
  FakeMapper m;
  for (uint32_t version : {1u, 2u, 0u}) {
    FakeMp2 mp2(AmdSfhGeneration::Sfh10V1);
    mp2.interface_version = version;
    mp2.mask = 1u << 0;
    if (version == 2) mp2.generation = AmdSfhGeneration::Sfh10V2;
    AmdSfhDriver drv(a);
    CHECK(drv.start_registers(mp2, 0x15E4, m, nullptr) == AmdSfhDriver::Outcome::Started);
    CHECK(drv.generation() == (version == 2 ? AmdSfhGeneration::Sfh10V2 : AmdSfhGeneration::Sfh10V1));
  }
  FakeMp2 mp2(AmdSfhGeneration::Sfh11);
  mp2.metadata(0x1234, 1u << 3);
  m.add_shared(mp2);
  AmdSfhDriver drv(a);
  CHECK(drv.start_registers(mp2, 0x164A, m, nullptr) == AmdSfhDriver::Outcome::Started);
  CHECK(drv.generation() == AmdSfhGeneration::Sfh11 && drv.firmware_version() == 0x1234);
  CHECK(drv.start_registers(mp2, 0x1234, m, nullptr) == AmdSfhDriver::Outcome::UnknownDevice);

  // The PCI path: decoding and bus mastering on, the legacy interrupt off, BAR 2 mapped; registers on plain memory (an interface v1 MP2 with an
  // accelerometer, which needs no answers).
  FakePci pci;
  FakeMapper bar;
  std::vector<uint32_t> window(L::kRegisterBytes / 4);
  window[L::kActiveStatusRegister / 4] = 1 | (1u << 4);
  bar.add(pci.bar2, window.data(), L::kRegisterBytes);
  SensorHub hub;
  AmdSfhDriver pdrv(a);
  CHECK(pdrv.start(pci, pci.info, bar, &hub) == AmdSfhDriver::Outcome::Started);
  CHECK((pci.command & 0x406) == 0x406);
  CHECK(bar.asked.size() == 1 && bar.asked[0].first == pci.bar2 && bar.asked[0].second == L::kRegisterBytes);
  CHECK(window[L::kCommandRegister / 4] == L::v1_command(L::kCmdEnable, kAccel, L::kPeriodMs));
  CHECK(window[L::kV1AddressRegister / 4] != 0 && hub.count() == 1);
  pdrv.stop();
  CHECK(window[L::kCommandRegister / 4] == L::v1_command(L::kCmdStopAll, 0, 0) && hub.count() == 0);
  pci.info.device_id = 0x15E5;
  CHECK(pdrv.start(pci, pci.info, bar, &hub) == AmdSfhDriver::Outcome::UnknownDevice);
  pci.info.device_id = 0x15E4;
  pci.bar2 = 0;
  CHECK(pdrv.start(pci, pci.info, bar, &hub) == AmdSfhDriver::Outcome::BarUnmapped);
}

static void test_startup_v1() {
  TestAllocator a;
  FakeMapper m;
  FakeMp2 mp2(AmdSfhGeneration::Sfh10V1);
  mp2.interface_version = 1;
  mp2.mask = (1u << 0) | (1u << 19);
  SensorHub hub;
  AmdSfhDriver drv(a);
  CHECK(drv.start_registers(mp2, 0x15E4, m, &hub) == AmdSfhDriver::Outcome::Started);
  CHECK(drv.sensor_count() == 2 && hub.count() == 2);
  // address (64-bit) then parameter then command, per sensor
  CHECK(mp2.writes.size() == 6);
  CHECK(mp2.writes[0] == L::kV1AddressRegister && mp2.writes[1] == L::kV1ParameterRegister && mp2.writes[2] == L::kCommandRegister);
  CHECK(mp2.reg[L::kV1ParameterRegister] == L::v1_parameter(1, 16));
  CHECK(mp2.log.size() == 2 && mp2.log[0].sensor == kAccel && mp2.log[1].sensor == kAls);
  CHECK(mp2.log[0].cmd == L::kCmdEnable && (mp2.log[0].raw >> 16) == L::kPeriodMs);
  CHECK(mp2.log[0].address && mp2.log[0].address != mp2.log[1].address && (mp2.log[0].address & 63) == 0);
  CHECK(mp2.response_reads == 0);                       // (v1 never answers, and is not asked)
}

static void test_discovery_and_values_v2() {
  TestAllocator a;
  FakeMapper m;
  FakeMp2 mp2(AmdSfhGeneration::Sfh10V2);
  mp2.mask = kMaskAll10 | (1u << 3) | (1u << 7) | (1u << 27);    // + bits of sensors nobody knows
  mp2.answer_after_reads = 3;                                       // (answers take a while)
  SensorHub hub;
  AmdSfhDriver drv(a);
  CHECK(drv.start_registers(mp2, 0x15E4, m, &hub) == AmdSfhDriver::Outcome::Started);
  CHECK(drv.sensor_count() == 5 && hub.count() == 5);
  CHECK(hub.find(SensorKind::Accelerometer) && hub.find(SensorKind::Gyroscope) && hub.find(SensorKind::Magnetometer) && hub.find(SensorKind::AmbientLight) &&
        hub.find(SensorKind::HumanPresence));
  CHECK(mp2.log.size() == 5 && mp2.enabled.size() == 5);
  for (const FakeMp2::Command& c : mp2.log) {
    CHECK(c.cmd == L::kCmdEnable && c.address != 0);
    CHECK(((c.raw >> 16) & 0x7F) == L::kV2BufferLength);
    CHECK(((c.raw >> 23) & 1) == (c.sensor == kAls ? 1u : 0u));    // ALS: "C2P memory"
  }
  // address before command
  CHECK(mp2.writes.size() == 10 && mp2.writes[0] == L::kV2AddressRegister && mp2.writes[1] == L::kCommandRegister);

  // Nothing written yet: no sample for a few reads.
  SensorReading r;
  ISensor* accel = hub.find(SensorKind::Accelerometer);
  CHECK(!accel->read(&r));
  mp2.set10(kAccel, 0, -50000, 100000);
  mp2.set10(kGyro, 500000, -250000, 1000);
  mp2.set10(kMag, 25000, 0, -40000);
  mp2.set10(kAls, 350000);
  mp2.set10(kPresence, 1);
  CHECK(accel->read(&r) && r.kind == SensorKind::Accelerometer && near(r.x, 0) && near(r.y, -0.5f) && near(r.z, 1.0f));
  const uint32_t seq = r.sequence;
  CHECK(accel->read(&r) && r.sequence == seq);          // the same values: not a new sample
  mp2.set10(kAccel, 0, 0, 98000);
  CHECK(accel->read(&r) && r.sequence == seq + 1 && near(r.z, 0.98f));
  CHECK(hub.read(SensorKind::Gyroscope, &r) && near(r.x, 5.0f) && near(r.y, -2.5f) && near(r.z, 0.01f));
  CHECK(hub.read(SensorKind::Magnetometer, &r) && near(r.x, 0.25f) && near(r.z, -0.4f));
  CHECK(hub.read(SensorKind::AmbientLight, &r) && near(r.x, 350.0f) && r.y == 0 && r.z == 0);
  CHECK(hub.read(SensorKind::HumanPresence, &r) && r.x == 1.0f);
  mp2.set10(kPresence, 0);
  CHECK(hub.read(SensorKind::HumanPresence, &r) && r.x == 0.0f);
  CHECK(drv.rejected_samples() == 0);

  // A dark room: zeros are believed after the grace reads.
  FakeMp2 dark(AmdSfhGeneration::Sfh10V2);
  dark.mask = 1u << 19;
  AmdSfhDriver d2(a);
  CHECK(d2.start_registers(dark, 0x15E4, m, nullptr) == AmdSfhDriver::Outcome::Started);
  uint32_t reads = 0;
  while (!d2.sensor(0)->read(&r) && reads < 20) ++reads;
  CHECK(reads == AmdSfhDriver::kZeroGraceReads - 1 && r.x == 0.0f);
}

static void test_startup_and_values_v11() {
  TestAllocator a;
  FakeMapper m;
  FakeMp2 mp2(AmdSfhGeneration::Sfh11);
  mp2.metadata(0x00410002, 0x1F | (1u << 10) | (1u << 31));        // all five + reserved bits
  m.add_shared(mp2);
  SensorHub hub;
  AmdSfhDriver drv(a);
  CHECK(drv.start_registers(mp2, 0x164A, m, &hub) == AmdSfhDriver::Outcome::Started);
  CHECK(m.asked.size() == 1 && m.asked[0].first == (uint64_t{0x123} << 21) && m.asked[0].second == L::kSharedMemoryBytes);
  CHECK(drv.sensor_count() == 5 && hub.count() == 5 && mp2.log.size() == 5);
  const uint8_t order[] = {kAccel, kGyro, kMag, kAls, kPresence};
  for (uint32_t i = 0; i < 5; ++i) CHECK(mp2.log[i].cmd == L::kCmdEnable && mp2.log[i].sensor == order[i] && ((mp2.log[i].raw >> 20) & 0x7FF) == 1);

  SensorReading r;
  CHECK(!hub.read(SensorKind::AmbientLight, &r));       // no interrupt count yet, no values
  mp2.set11(kAls, 412.5f);
  CHECK(hub.read(SensorKind::AmbientLight, &r) && near(r.x, 412.5f));
  const uint32_t seq = r.sequence;
  mp2.set11(kAls, 412.5f);                              // the same value, a new interrupt: a new sample
  CHECK(hub.read(SensorKind::AmbientLight, &r) && r.sequence == seq + 1);
  mp2.set11(kAccel, 0.0f, 5000.0f, -10000.0f);
  CHECK(hub.read(SensorKind::Accelerometer, &r) && near(r.y, 0.5f) && near(r.z, -1.0f));
  mp2.set11(kGyro, 100000.0f, 0.0f, 0.0f);
  CHECK(hub.read(SensorKind::Gyroscope, &r) && near(r.x, 1.0f));
  mp2.set11(kMag, 30.0f, -12.0f, 0.0f);
  CHECK(hub.read(SensorKind::Magnetometer, &r) && near(r.x, 0.3f) && near(r.y, -0.12f));
  mp2.set11(kPresence, 1.0f);
  CHECK(hub.read(SensorKind::HumanPresence, &r) && r.x == 1.0f);
  CHECK(drv.rejected_samples() == 0);
}

static void test_stuck_mp2() {
  TestAllocator a;
  FakeMapper m;
  {
    // v2 that never answers: one command, one full timeout, then start-up gives up; the unanswered buffer is not freed.
    FakeMp2 mp2(AmdSfhGeneration::Sfh10V2);
    mp2.mask = kMaskAll10;
    mp2.answer = false;
    SensorHub hub;
    AmdSfhDriver drv(a);
    CHECK(drv.start_registers(mp2, 0x15E4, m, &hub) == AmdSfhDriver::Outcome::NoResponse);
    CHECK(mp2.log.size() == 1 && mp2.response_reads == L::kResponseAttempts && hub.count() == 0 && drv.sensor_count() == 0);
    CHECK(a.freed_dma.empty() && a.dma.size() == 1);
  }
  {
    // 1.1 that never answers
    FakeMp2 mp2(AmdSfhGeneration::Sfh11);
    mp2.metadata(1, 0x1F);
    mp2.answer = false;
    m.add_shared(mp2);
    AmdSfhDriver drv(a);
    CHECK(drv.start_registers(mp2, 0x164A, m, nullptr) == AmdSfhDriver::Outcome::NoResponse);
    CHECK(mp2.log.size() == 1 && mp2.response_reads == L::kResponseAttempts);
  }
  {
    // 1.1 whose firmware never fills in its metadata; one with no shared memory address; one whose memory cannot be mapped
    FakeMp2 mp2(AmdSfhGeneration::Sfh11);
    FakeMapper mm;
    mm.add_shared(mp2);
    AmdSfhDriver drv(a);
    CHECK(drv.start_registers(mp2, 0x164A, mm, nullptr) == AmdSfhDriver::Outcome::FirmwareNotReady && mp2.log.empty());
    mp2.shared_base = 0;
    CHECK(drv.start_registers(mp2, 0x164A, mm, nullptr) == AmdSfhDriver::Outcome::SharedMemoryUnmapped);
    mp2.shared_base = 0x456;
    mp2.metadata(1, 1);
    CHECK(drv.start_registers(mp2, 0x164A, mm, nullptr) == AmdSfhDriver::Outcome::SharedMemoryUnmapped);
  }
  {
    // a device that is gone (reads all ones), and one with no sensors
    FakeMp2 mp2(AmdSfhGeneration::Sfh10V2);
    mp2.dead = true;
    AmdSfhDriver drv(a);
    CHECK(drv.start_registers(mp2, 0x15E4, m, nullptr) == AmdSfhDriver::Outcome::NoResponse && mp2.log.empty());
    FakeMp2 dead11(AmdSfhGeneration::Sfh11);
    dead11.dead = true;
    CHECK(drv.start_registers(dead11, 0x164A, m, nullptr) == AmdSfhDriver::Outcome::NoResponse);
    FakeMp2 empty(AmdSfhGeneration::Sfh10V2);
    empty.mask = 1u << 5;                                // only a bit nobody knows
    CHECK(drv.start_registers(empty, 0x15E4, m, nullptr) == AmdSfhDriver::Outcome::NoSensors && empty.log.empty());
  }
}

static void test_garbage() {
  TestAllocator a;
  FakeMapper m;
  {
    // answers that name another (out of range) sensor do not count
    FakeMp2 mp2(AmdSfhGeneration::Sfh10V2);
    mp2.mask = 1u << 0;
    mp2.sensor_id_offset = 40;
    AmdSfhDriver drv(a);
    CHECK(drv.start_registers(mp2, 0x15E4, m, nullptr) == AmdSfhDriver::Outcome::NoResponse);
    FakeMp2 m11(AmdSfhGeneration::Sfh11);
    m11.metadata(1, 1u << 3);
    m11.sensor_id_offset = 200;
    m.add_shared(m11);
    CHECK(drv.start_registers(m11, 0x164A, m, nullptr) == AmdSfhDriver::Outcome::NoResponse);
  }
  {
    // impossible values are dropped (once each), the last good sample stays
    FakeMp2 mp2(AmdSfhGeneration::Sfh10V2);
    mp2.mask = (1u << 0) | (1u << 19);
    SensorHub hub;
    AmdSfhDriver drv(a);
    CHECK(drv.start_registers(mp2, 0x15E4, m, &hub) == AmdSfhDriver::Outcome::Started);
    SensorReading r;
    mp2.set10(kAls, 120000);
    CHECK(sample(hub.find(SensorKind::AmbientLight), &r) && near(r.x, 120.0f));
    const uint32_t seq = r.sequence;
    mp2.set10(kAls, -5000);                               // negative lux
    CHECK(hub.read(SensorKind::AmbientLight, &r) && near(r.x, 120.0f) && r.sequence == seq);
    CHECK(hub.read(SensorKind::AmbientLight, &r) && drv.rejected_samples() == 1);
    mp2.set10(kAls, 2000000000);                          // 2 million lux
    CHECK(hub.read(SensorKind::AmbientLight, &r) && near(r.x, 120.0f) && drv.rejected_samples() == 2);
    mp2.set10(kAccel, 2000000000, 0, 0);                  // 20000 g
    CHECK(!hub.read(SensorKind::Accelerometer, &r) && drv.rejected_samples() == 3);
    mp2.set10(kAccel, 0, 0, 100000);
    CHECK(hub.read(SensorKind::Accelerometer, &r) && near(r.z, 1.0f));
    mp2.set10(kAls, 80000);
    CHECK(hub.read(SensorKind::AmbientLight, &r) && near(r.x, 80.0f) && r.sequence == seq + 1);
  }
  {
    // 1.1: NaN, infinity, out of range floats
    FakeMp2 mp2(AmdSfhGeneration::Sfh11);
    mp2.metadata(1, (1u << 0) | (1u << 3));
    m.add_shared(mp2);
    SensorHub hub;
    AmdSfhDriver drv(a);
    CHECK(drv.start_registers(mp2, 0x164A, m, &hub) == AmdSfhDriver::Outcome::Started);
    SensorReading r;
    mp2.set11_raw(kAls, 0x7FC00000u);                     // NaN
    CHECK(!hub.read(SensorKind::AmbientLight, &r) && drv.rejected_samples() == 1);
    mp2.set11_raw(kAls, 0x7F800000u);                     // +infinity
    CHECK(!hub.read(SensorKind::AmbientLight, &r) && drv.rejected_samples() == 2);
    mp2.set11(kAls, 3.0e9f);
    CHECK(!hub.read(SensorKind::AmbientLight, &r) && drv.rejected_samples() == 3);
    mp2.set11(kAls, 55.0f);
    CHECK(hub.read(SensorKind::AmbientLight, &r) && near(r.x, 55.0f));
    mp2.set11_raw(kAccel, float_bits(0.0f), 0xFF800000u, 0);   // -infinity on one axis
    CHECK(!hub.read(SensorKind::Accelerometer, &r) && drv.rejected_samples() == 4);
    mp2.set11(kAccel, 1e12f, 0.0f, 0.0f);
    CHECK(!hub.read(SensorKind::Accelerometer, &r) && drv.rejected_samples() == 5);
    (void)bits_float;
  }
}

static void test_suspend_stop() {
  TestAllocator a;
  FakeMapper m;
  {
    FakeMp2 mp2(AmdSfhGeneration::Sfh10V2);
    mp2.mask = (1u << 0) | (1u << 19);
    SensorHub hub;
    AmdSfhDriver drv(a);
    CHECK(drv.start_registers(mp2, 0x15E4, m, &hub) == AmdSfhDriver::Outcome::Started);
    SensorReading r;
    mp2.set10(kAls, 300000);
    CHECK(hub.read(SensorKind::AmbientLight, &r) && near(r.x, 300.0f));
    CHECK(drv.suspend() && drv.suspended());
    CHECK(mp2.count_commands(L::kCmdDisable) == 2 && mp2.enabled.empty());
    CHECK(!hub.read(SensorKind::AmbientLight, &r));       // nothing while asleep
    drv.poll();
    CHECK(drv.resume() && !drv.suspended());
    CHECK(mp2.count_commands(L::kCmdEnable) == 4 && mp2.enabled.size() == 2);
    mp2.set10(kAls, 500000);
    CHECK(hub.read(SensorKind::AmbientLight, &r) && near(r.x, 500.0f));
    // stop: each sensor disabled (and confirmed), out of the hub, buffers freed
    const size_t dma_buffers = a.dma.size();
    drv.stop();
    CHECK(hub.count() == 0 && drv.sensor_count() == 0 && mp2.enabled.empty());
    CHECK(a.freed_dma.size() == dma_buffers && a.dma.empty());
    CHECK(drv.outcome() == AmdSfhDriver::Outcome::NotStarted);
    drv.poll();                                           // (harmless when stopped)
  }
  {
    // the firmware does not confirm the disables: suspend says so, and stop leaves the buffers to it
    FakeMp2 mp2(AmdSfhGeneration::Sfh10V2);
    mp2.mask = 1u << 0;
    SensorHub hub;
    AmdSfhDriver drv(a);
    CHECK(drv.start_registers(mp2, 0x15E4, m, &hub) == AmdSfhDriver::Outcome::Started);
    mp2.answer_disable = false;
    CHECK(!drv.suspend());
    CHECK(drv.resume());
    const size_t freed = a.freed_dma.size();
    drv.stop();
    CHECK(a.freed_dma.size() == freed && a.dma.size() == 1 && hub.count() == 0);
  }
  {
    // 1.1: disable and enable again
    FakeMp2 mp2(AmdSfhGeneration::Sfh11);
    mp2.metadata(1, 1u << 3);
    m.add_shared(mp2);
    SensorHub hub;
    AmdSfhDriver drv(a);
    CHECK(drv.start_registers(mp2, 0x164A, m, &hub) == AmdSfhDriver::Outcome::Started);
    CHECK(drv.suspend() && mp2.count_commands(L::kCmdDisable) == 1);
    CHECK(drv.resume() && mp2.count_commands(L::kCmdEnable) == 2);
    mp2.set11(kAls, 10.0f);
    SensorReading r;
    CHECK(hub.read(SensorKind::AmbientLight, &r) && near(r.x, 10.0f));
    drv.stop();
    CHECK(mp2.count_commands(L::kCmdDisable) == 2 && hub.count() == 0);
  }
  {
    // v1: stop all
    FakeMp2 mp2(AmdSfhGeneration::Sfh10V1);
    mp2.interface_version = 1;
    mp2.mask = 1u << 1;
    AmdSfhDriver drv(a);
    const size_t before = a.dma.size();
    CHECK(drv.start_registers(mp2, 0x15E4, m, nullptr) == AmdSfhDriver::Outcome::Started);
    CHECK(a.dma.size() == before + 1);
    drv.stop();
    CHECK(mp2.count_commands(L::kCmdStopAll) == 1 && a.dma.size() == before);
  }
}

// A poller takes the values (the firmware writes, then poll()) while readers read. Readers that only take what was polled must get whole samples
// (y = 2x, z = 3x: never half of one publish and half of the next) with a sequence that never goes back. Then readers that take values from the
// firmware's memory themselves as well: the sequence still never goes back and every value is one that was written. (A firmware that is
// interrupted in the middle of writing a buffer can be seen half-written: the hardware gives nothing to tell, so that phase checks no pairing.)
static void run_concurrent(bool refresh_on_read) {
  TestAllocator a;
  FakeMapper m;
  FakeMp2 mp2(AmdSfhGeneration::Sfh10V2);
  mp2.mask = (1u << 0) | (1u << 19);
  SensorHub hub;
  AmdSfhDriver drv(a);
  CHECK(drv.start_registers(mp2, 0x15E4, m, &hub) == AmdSfhDriver::Outcome::Started);
  drv.set_refresh_on_read(refresh_on_read);
  constexpr int32_t kUpdates = 20000;
  std::atomic<bool> done{false};
  std::atomic<int> torn{0}, backwards{0}, wrong{0}, samples{0};
  std::vector<std::thread> readers;
  for (int t = 0; t < 4; ++t) {
    readers.emplace_back([&] {
      uint32_t last = 0;
      float last_x = 0;
      ISensor* accel = hub.find(SensorKind::Accelerometer);
      ISensor* light = hub.find(SensorKind::AmbientLight);
      while (!done.load()) {
        SensorReading r;
        if (accel->read(&r)) {
          ++samples;
          if (!refresh_on_read && (!near(r.y, 2 * r.x) || !near(r.z, 3 * r.x))) ++torn;
          if (r.sequence < last) ++backwards;
          if (r.sequence > last && !refresh_on_read && r.x < last_x) ++backwards;
          if (r.x < 0 || r.x > kUpdates * 1.0001e-5f || r.z > 3 * kUpdates * 1.0001e-5f) ++wrong;
          last = r.sequence;
          last_x = r.x;
        }
        if (light->read(&r) && (r.x < 0 || r.x > kUpdates * 0.0100001f)) ++wrong;
      }
    });
  }
  std::thread poller([&] {
    for (int32_t k = 1; k <= kUpdates; ++k) {
      mp2.set10(kAccel, k, 2 * k, 3 * k);
      mp2.set10(kAls, k * 10);
      drv.poll();
    }
    done.store(true);
  });
  poller.join();
  for (std::thread& t : readers) t.join();
  drv.poll();
  SensorReading r;
  CHECK(hub.read(SensorKind::Accelerometer, &r) && near(r.x, kUpdates * 1e-5f) && near(r.z, 3 * kUpdates * 1e-5f));
  CHECK(torn == 0 && backwards == 0 && wrong == 0 && samples > 0);
  printf("  concurrent (%s): %d samples read, %u changed-while-read dropped\n", refresh_on_read ? "readers refresh" : "poller only", samples.load(), drv.rejected_samples());
  drv.stop();
  CHECK(hub.count() == 0);
}

static void test_concurrent_reads() {
  run_concurrent(false);
  run_concurrent(true);
}

int main() {
  test_detection();
  test_startup_v1();
  test_discovery_and_values_v2();
  test_startup_and_values_v11();
  test_stuck_mp2();
  test_garbage();
  test_suspend_stop();
  test_concurrent_reads();
  if (g_failed) {
    printf("AmdSfhTest: %d failure(s)\n", g_failed);
    return 1;
  }
  printf("AmdSfhTest: all passed\n");
  return 0;
}
