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

// The Intel Integrated Sensor Hub from the host's side, against a model of the firmware written from the same sources as the driver: the IPC doorbells and message registers, the
// reset handshake, the ISHTP host bus messages (version, clients, properties, connect, flow control), fragmented messages in both directions, and the HID client with two sensor
// devices (an accelerometer, and a light sensor with a gyroscope in one device with a descriptor longer than one fragment). Also: no firmware, a firmware that resets itself,
// credits, and garbage on the wire. ASan and UBSan.
#include <initializer_list>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <map>
#include <vector>
#include "Hardware/Bus/Ish/IshDriver.hpp"

namespace hardware { void delay_us(uint64_t) {} }
using namespace hardware;

static int g_failed = 0;
#define CHECK(...) do { if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)

struct TestAllocator final : allocators::IAllocator {
  std::vector<void*> blocks;
  void* allocate(uint64_t size) override { return allocate_aligned(size, 16); }
  void* allocate_aligned(uint64_t size, uint64_t alignment) override { void* p = aligned_alloc(alignment, (size + alignment - 1) / alignment * alignment); blocks.push_back(p); return p; }
  void deallocate(void*) override {}
  ~TestAllocator() { for (void* p : blocks) free(p); }
};

typedef std::vector<uint8_t> Bytes;
static void D(Bytes& d, std::initializer_list<uint8_t> v) { d.insert(d.end(), v.begin(), v.end()); }

// One sensor collection: reporting state and power state selectors in feature report `feature_id`, the data in input report `input_id` (3 axes of 16 bits with unit exponent -2, or
// a single 16 bit value).
static void sensor_collection(Bytes& d, uint8_t top_usage, uint8_t feature_id, uint8_t input_id, uint16_t first_usage, uint32_t axes, bool exponent) {
  D(d, {0x05, 0x20, 0x09, top_usage, 0xA1, 0x01, 0x85, feature_id});
  D(d, {0x0A, 0x16, 0x03, 0xA1, 0x02, 0x0A, 0x40, 0x08, 0x0A, 0x41, 0x08, 0x15, 0x00, 0x25, 0x01, 0x75, 0x08, 0x95, 0x01, 0xB1, 0x00, 0xC0});
  D(d, {0x0A, 0x19, 0x03, 0xA1, 0x02, 0x0A, 0x50, 0x08, 0x0A, 0x51, 0x08, 0x15, 0x00, 0x25, 0x01, 0x75, 0x08, 0x95, 0x01, 0xB1, 0x00, 0xC0});
  D(d, {0x85, input_id});
  for (uint32_t a = 0; a < axes; ++a) D(d, {0x0A, static_cast<uint8_t>(first_usage + a), static_cast<uint8_t>((first_usage + a) >> 8)});
  D(d, {0x16, 0x00, 0x80, 0x26, 0xFF, 0x7F, 0x55, static_cast<uint8_t>(exponent ? 0x0E : 0x00), 0x75, 0x10, 0x95, static_cast<uint8_t>(axes), 0x81, 0x02, 0xC0});
}

static Bytes accel_descriptor() { Bytes d; sensor_collection(d, 0x73, 3, 2, 0x453, 3, true); return d; }
static Bytes light_gyro_descriptor() { Bytes d; sensor_collection(d, 0x41, 5, 4, 0x4D1, 1, false); sensor_collection(d, 0x76, 7, 6, 0x457, 3, true); return d; }

constexpr uint8_t kFwHid = 5, kFwOther = 9;                    // addresses of the firmware clients
const uint8_t kHidGuid[16] = {0x58, 0xCD, 0xAE, 0x33, 0x79, 0xB6, 0x54, 0x4E, 0x9B, 0xD9, 0xA0, 0x4D, 0x34, 0xF0, 0xC2, 0x26};

struct FakeIsh final : IIshRegisters {
  // knobs
  bool ack_reset = true, supports_version = true, has_hid_client = true, send_fc_after_connect = true;
  // registers
  std::map<uint32_t, uint32_t> reg;
  uint32_t host_message[32] = {};
  uint32_t fw_message[32] = {};
  uint32_t fw_doorbell = 0, host_doorbell = 0, fw_status = 0;
  std::vector<std::vector<uint8_t>> outbox;                     // messages for the host: doorbell (4 bytes) then the message
  // state of the firmware
  bool bus_started = false;
  uint32_t host_credits_given = 0;                              // flow control credits the host granted for the HID client
  uint8_t host_addr = 0;
  bool connected = false;
  Bytes assembling;
  std::vector<Bytes> pending_for_host;                          // messages waiting for a credit
  uint32_t violations = 0, resets_acked = 0, features_set = 0;
  Bytes last_feature;
  uint8_t last_feature_device = 0;
  Bytes accel = accel_descriptor(), second = light_gyro_descriptor();
  uint32_t handshakes = 0;

  uint32_t read32(uint32_t offset) override {
    if (offset == ish_reg::kFwStatus) return fw_status;
    if (offset == ish_reg::kIshToHostDoorbell) {
      if (!(fw_doorbell & ish_reg::kDoorbellBusy) && !outbox.empty()) load(outbox.front()), outbox.erase(outbox.begin());
      return fw_doorbell;
    }
    if (offset == ish_reg::kHostToIshDoorbell) return host_doorbell;
    if (offset >= ish_reg::kIshToHostMessage && offset < ish_reg::kIshToHostMessage + 128) return fw_message[(offset - ish_reg::kIshToHostMessage) / 4];
    return reg[offset];
  }
  void write32(uint32_t offset, uint32_t value) override {
    if (offset == ish_reg::kIshToHostDoorbell) { fw_doorbell = value; return; }
    if (offset >= ish_reg::kHostToIshMessage && offset < ish_reg::kHostToIshMessage + 128) { host_message[(offset - ish_reg::kHostToIshMessage) / 4] = value; return; }
    if (offset == ish_reg::kHostToIshDoorbell) {
      if (value & ish_reg::kDoorbellBusy) {
        Bytes m(value & 0x3FF);
        for (size_t i = 0; i < m.size(); ++i) m[i] = static_cast<uint8_t>(host_message[i / 4] >> (8 * (i % 4)));
        host_doorbell = value & ~ish_reg::kDoorbellBusy;           // (consumed at once)
        receive(value, m);
      }
      return;
    }
    reg[offset] = value;
  }

  void load(const Bytes& b) {
    const uint32_t doorbell = b[0] | (b[1] << 8) | (b[2] << 16) | (static_cast<uint32_t>(b[3]) << 24);
    memset(fw_message, 0, sizeof fw_message);
    for (size_t i = 4; i < b.size(); ++i) fw_message[(i - 4) / 4] |= static_cast<uint32_t>(b[i]) << (8 * ((i - 4) % 4));
    fw_doorbell = doorbell | ish_reg::kDoorbellBusy;
  }
  void to_host_mng(uint32_t command, const Bytes& payload) {
    const uint32_t d = (payload.size() & 0x3FF) | (ish_reg::kProtocolManagement << 10) | (command << 16);
    Bytes b = {static_cast<uint8_t>(d), static_cast<uint8_t>(d >> 8), static_cast<uint8_t>(d >> 16), static_cast<uint8_t>(d >> 24)};
    b.insert(b.end(), payload.begin(), payload.end());
    outbox.push_back(b);
  }
  void to_host_piece(uint8_t fw, uint8_t host, bool complete, const uint8_t* p, size_t n) {
    const uint32_t d = ((n + 4) & 0x3FF) | (ish_reg::kProtocolIshtp << 10);
    const uint32_t h = fw | (uint32_t{host} << 8) | (static_cast<uint32_t>(n) << 16) | (complete ? 1u << 31 : 0);
    Bytes b = {static_cast<uint8_t>(d), static_cast<uint8_t>(d >> 8), static_cast<uint8_t>(d >> 16), static_cast<uint8_t>(d >> 24), static_cast<uint8_t>(h), static_cast<uint8_t>(h >> 8),
               static_cast<uint8_t>(h >> 16), static_cast<uint8_t>(h >> 24)};
    b.insert(b.end(), p, p + n);
    outbox.push_back(b);
  }
  void hbm(const Bytes& body) { to_host_piece(0, 0, true, body.data(), body.size()); }
  // a message of the HID client to the host, fragmented, using one credit
  void send_hid(const Bytes& message) {
    if (!connected) return;
    if (host_credits_given == 0) { pending_for_host.push_back(message); ++violations_waiting; return; }
    --host_credits_given;
    size_t at = 0;
    do {
      const size_t n = message.size() - at < 124 ? message.size() - at : 124;
      to_host_piece(kFwHid, host_addr, at + n == message.size(), message.data() + at, n);
      at += n;
    } while (at < message.size());
  }
  uint32_t violations_waiting = 0;

  static Bytes hostif(uint8_t command, uint8_t device, const Bytes& payload) {
    Bytes m = {command, device, 0, 0, static_cast<uint8_t>(payload.size()), static_cast<uint8_t>(payload.size() >> 8)};
    m.insert(m.end(), payload.begin(), payload.end());
    return m;
  }
  // publishing an input report of a device
  void publish(uint8_t device, const Bytes& report) { send_hid(hostif(5, device, report)); }
  void publish_list(const std::vector<std::pair<uint8_t, Bytes>>& reports) {
    Bytes payload = {0, 0, static_cast<uint8_t>(reports.size()), 0};
    for (const auto& r : reports) {
      const uint16_t size = static_cast<uint16_t>(6 + r.second.size());
      payload.push_back(static_cast<uint8_t>(size));
      payload.push_back(static_cast<uint8_t>(size >> 8));
      const Bytes h = hostif(5, r.first, r.second);
      payload.insert(payload.end(), h.begin(), h.end());
    }
    payload[0] = static_cast<uint8_t>(payload.size());
    payload[1] = static_cast<uint8_t>(payload.size() >> 8);
    send_hid(hostif(6, 0, payload));
  }
  void reset_firmware() {
    bus_started = connected = false;
    host_credits_given = 0;
    pending_for_host.clear();
    assembling.clear();
    to_host_mng(ish_reg::kMngResetNotify, Bytes{7, 0, 0, 0});
  }

  void receive(uint32_t doorbell, const Bytes& m) {
    const uint32_t protocol = (doorbell >> 10) & 0xF, command = (doorbell >> 16) & 0xF;
    if (protocol == ish_reg::kProtocolManagement) {
      if (command == ish_reg::kMngResetNotify) {
        if (ack_reset) { to_host_mng(ish_reg::kMngResetNotifyAck, Bytes{m.size() > 0 ? m[0] : uint8_t{0}, 0, 0, 0}); fw_status = ish_reg::kFwIlup | ish_reg::kFwIshtpReady; }
      } else if (command == ish_reg::kMngResetNotifyAck) {
        ++resets_acked;
      }
      return;
    }
    if (protocol != ish_reg::kProtocolIshtp || m.size() < 4) return;
    const uint8_t fw = m[0], host = m[1];
    const uint32_t length = (m[2] | (m[3] << 8)) & 0x1FF;
    const bool complete = m[3] & 0x80;
    if (length + 4 > m.size()) { ++violations; return; }
    const Bytes payload(m.begin() + 4, m.begin() + 4 + length);
    if (fw == 0 && host == 0) { bus_message(payload); return; }
    if (fw != kFwHid || host != host_addr || !connected) { ++violations; return; }
    assembling.insert(assembling.end(), payload.begin(), payload.end());
    if (!complete) return;
    Bytes message = assembling;
    assembling.clear();
    hid_message(message);
    hbm(Bytes{0x08, kFwHid, host_addr, 0, 0, 0, 0, 0});          // a credit for the next message of the host
  }

  void bus_message(const Bytes& b) {
    if (b.empty()) return;
    switch (b[0]) {
      case 0x01: bus_started = true; hbm(Bytes{0x81, static_cast<uint8_t>(supports_version ? 1 : 0), 0, 1}); break;
      case 0x04: {
        Bytes r = {0x84, 0, 0, 0};
        Bytes map(32, 0);
        if (has_hid_client) map[kFwHid / 8] |= 1u << (kFwHid % 8);
        map[kFwOther / 8] |= 1u << (kFwOther % 8);
        r.insert(r.end(), map.begin(), map.end());
        hbm(r);
        break;
      }
      case 0x05: {
        if (b.size() < 2) break;
        Bytes r = {0x85, b[1], 0, 0};
        if (b[1] == kFwHid) r.insert(r.end(), kHidGuid, kHidGuid + 16);
        else r.insert(r.end(), 16, 0x77);
        r.insert(r.end(), {1, 1, 0, 0, 0x00, 0x10, 0, 0, 0, 0, 0, 0});     // version, connections, fixed, single buffer, max message 4096, DMA header length, reserved
        hbm(r);
        break;
      }
      case 0x06:
        if (b.size() >= 3 && b[1] == kFwHid) {
          host_addr = b[2];
          connected = true;
          hbm(Bytes{0x86, b[1], b[2], 0});
          if (send_fc_after_connect) hbm(Bytes{0x08, kFwHid, b[2], 0, 0, 0, 0, 0});     // the firmware's credit: the host may send one message
        } else if (b.size() >= 3) {
          hbm(Bytes{0x86, b[1], b[2], 1});
        }
        break;
      case 0x07: connected = false; if (b.size() >= 3) hbm(Bytes{0x87, b[1], b[2], 0}); break;
      case 0x08:
        if (b.size() >= 3 && b[1] == kFwHid && b[2] == host_addr) {
          ++host_credits_given;
          while (host_credits_given > 0 && !pending_for_host.empty()) { Bytes m = pending_for_host.front(); pending_for_host.erase(pending_for_host.begin()); send_hid(m); }
        }
        break;
      default: break;
    }
  }

  void hid_message(const Bytes& m) {
    if (m.size() < 6) { ++violations; return; }
    const uint8_t command = m[0] & 0x7F, device = m[1];
    const Bytes payload(m.begin() + 6, m.end());
    switch (command) {
      case 33: {
        Bytes p = {2};
        for (uint32_t id : {0x21u, 0x22u}) {
          p.insert(p.end(), {static_cast<uint8_t>(id), 0, 0, 0, 0x20, 0x34, 0x12, 0x86, 0x80});     // device id, class, product 0x1234, vendor 0x8086
        }
        send_hid(hostif(33 | 0x80, 0, p));
        break;
      }
      case 0: send_hid(hostif(0 | 0x80, device, Bytes{9, 0x21, 0x11, 0x01, 0, 1, 0x22, 0, 0})); break;
      case 1: send_hid(hostif(1 | 0x80, device, device == 0x21 ? accel : second)); break;
      case 3:
        ++features_set;
        last_feature = payload;
        last_feature_device = device;
        break;
      default: break;
    }
  }
};

static void run(IshDriver& d, int rounds = 5) { for (int i = 0; i < rounds; ++i) d.poll(); }

static Bytes accel_report(int16_t x, int16_t y, int16_t z) {
  return Bytes{2, static_cast<uint8_t>(x), static_cast<uint8_t>(x >> 8), static_cast<uint8_t>(y), static_cast<uint8_t>(y >> 8), static_cast<uint8_t>(z), static_cast<uint8_t>(z >> 8)};
}
static Bytes light_report(uint16_t lux) { return Bytes{4, static_cast<uint8_t>(lux), static_cast<uint8_t>(lux >> 8)}; }
static Bytes gyro_report(int16_t x, int16_t y, int16_t z) {
  return Bytes{6, static_cast<uint8_t>(x), static_cast<uint8_t>(x >> 8), static_cast<uint8_t>(y), static_cast<uint8_t>(y >> 8), static_cast<uint8_t>(z), static_cast<uint8_t>(z >> 8)};
}

static void test_bring_up() {
  TestAllocator a;
  FakeIsh fw;
  SensorHub hub;
  {
    IshDriver driver(a);
    CHECK(driver.start_registers(fw, &hub) == IshDriver::Outcome::Started && driver.sensor_count() == 3 && hub.count() == 3);
    CHECK((fw.reg[ish_reg::kHostComm] & 0x81) == 0x81 && fw.reg[ish_reg::kRemap2] == 1 && (fw.reg[ish_reg::kPimr] & 1) && !(fw.reg[ish_reg::kPimr] & (1u << 8)));
    IshHid* hid = driver.hid();
    CHECK(hid && hid->device_count() == 2 && hid->connected() && hid->device(0)->id() == 0x21 && hid->device(0)->vendor() == 0x8086 && hid->device(0)->product() == 0x1234);
    CHECK(hid->device(0)->report_descriptor().size() == fw.accel.size() && memcmp(hid->device(0)->report_descriptor().data(), fw.accel.data(), fw.accel.size()) == 0);
    CHECK(hid->device(1)->report_descriptor().size() == fw.second.size() && fw.second.size() > 124 && memcmp(hid->device(1)->report_descriptor().data(), fw.second.data(), fw.second.size()) == 0);   // (in several pieces)
    CHECK(hub.find(SensorKind::Accelerometer) && hub.find(SensorKind::Gyroscope) && hub.find(SensorKind::AmbientLight));
    CHECK(fw.features_set == 3 && fw.violations == 0);
    // the last feature report: the sensor's own device, the report id then the two selector indexes
    CHECK(fw.last_feature_device == 0x22 && fw.last_feature.size() == 3 && fw.last_feature[0] == 7 && fw.last_feature[1] == 1 && fw.last_feature[2] == 1);
    // reports: one, then a list of three for the two devices, bigger than one piece
    SensorReading r;
    CHECK(!hub.read(SensorKind::Accelerometer, &r));
    fw.publish(0x21, accel_report(100, -50, 981));
    CHECK(hub.read(SensorKind::Accelerometer, &r) && r.x > 0.99f && r.x < 1.01f && r.y < -0.49f && r.y > -0.51f && r.z > 9.80f && r.z < 9.82f && r.sequence == 1);
    fw.publish_list({{0x22, light_report(300)}, {0x22, gyro_report(100, 200, 300)}, {0x21, accel_report(0, 0, 100)}});
    CHECK(hub.read(SensorKind::AmbientLight, &r) && r.x == 300.0f);
    CHECK(hub.read(SensorKind::Gyroscope, &r) && r.y > 1.99f && r.y < 2.01f);
    CHECK(hub.read(SensorKind::Accelerometer, &r) && r.sequence == 2 && r.z > 0.99f && r.z < 1.01f);
    // credits: the firmware had to wait for none of them
    CHECK(fw.violations_waiting == 0);
    for (int i = 0; i < 20; ++i) fw.publish(0x21, accel_report(static_cast<int16_t>(i), 0, 0));
    run(driver, 30);
    CHECK(hub.read(SensorKind::Accelerometer, &r) && r.sequence >= 3 && fw.pending_for_host.empty());
    driver.stop();
    CHECK(hub.count() == 0);
  }
  CHECK(a.blocks.size() > 0);
}

static void test_failures() {
  {
    TestAllocator a;
    FakeIsh fw;
    fw.ack_reset = false;
    IshDriver d(a);
    CHECK(d.start_registers(fw, nullptr) == IshDriver::Outcome::FirmwareNotReady);
  }
  {
    TestAllocator a;
    FakeIsh fw;
    fw.supports_version = false;
    IshDriver d(a);
    CHECK(d.start_registers(fw, nullptr) == IshDriver::Outcome::BusFailed);
  }
  {
    TestAllocator a;
    FakeIsh fw;
    fw.has_hid_client = false;
    IshDriver d(a);
    CHECK(d.start_registers(fw, nullptr) == IshDriver::Outcome::NoHidClient);
  }
  {   // a connection the firmware gives no credit for: sending fails instead of hanging
    TestAllocator a;
    FakeIsh fw;
    fw.send_fc_after_connect = false;
    IshDriver d(a);
    CHECK(d.start_registers(fw, nullptr) != IshDriver::Outcome::Started);
  }
  CHECK(!IshDriver::supports({{0, 0, 0}, 0x8086, 0x1234, 0, 0, 0}) && IshDriver::supports({{0, 0, 0}, 0x8086, 0x9D35, 0, 0, 0}) && IshDriver::supports({{0, 0, 0}, 0x8086, 0xA135, 0, 0, 0}) &&
        !IshDriver::supports({{0, 0, 0}, 0x1022, 0x9D35, 0, 0, 0}));
}

static void test_firmware_reset() {
  TestAllocator a;
  FakeIsh fw;
  SensorHub hub;
  IshDriver driver(a);
  CHECK(driver.start_registers(fw, &hub) == IshDriver::Outcome::Started);
  SensorReading r;
  fw.publish(0x21, accel_report(100, 0, 0));
  CHECK(hub.read(SensorKind::Accelerometer, &r) && r.sequence == 1);
  const uint32_t features = fw.features_set;
  fw.reset_firmware();                                           // the firmware restarts: the host acknowledges, starts the bus again and switches the sensors on again
  run(driver, 10);
  CHECK(fw.resets_acked == 1 && fw.connected && driver.hid()->connected());
  CHECK(fw.features_set == features + 3);
  fw.publish(0x21, accel_report(200, 0, 0));
  CHECK(hub.read(SensorKind::Accelerometer, &r) && r.x > 1.99f && r.x < 2.01f);
}

static void test_garbage() {
  TestAllocator a;
  FakeIsh fw;
  SensorHub hub;
  IshDriver driver(a);
  CHECK(driver.start_registers(fw, &hub) == IshDriver::Outcome::Started);
  uint32_t seed = 12345;
  auto rnd = [&]() { seed = seed * 1664525u + 1013904223u; return seed >> 8; };
  for (int i = 0; i < 400; ++i) {
    // random messages from the "firmware": any protocol, any length (even beyond the 128 bytes the registers can hold), random bytes
    Bytes b(4 + rnd() % 140);
    for (uint8_t& x : b) x = static_cast<uint8_t>(rnd());
    const uint32_t length = static_cast<uint32_t>(b.size() - 4) & 0x3FF;
    const uint32_t d = length | ((rnd() % 4) << 10) | ((rnd() % 16) << 16);
    b[0] = static_cast<uint8_t>(d); b[1] = static_cast<uint8_t>(d >> 8); b[2] = static_cast<uint8_t>(d >> 16); b[3] = static_cast<uint8_t>(d >> 24);
    if (b.size() > 4 + 128) b.resize(4 + 128);
    fw.outbox.push_back(b);
    if (i % 7 == 0) driver.poll();
    if (rnd() % 5 == 0) {                                          // and valid-looking client pieces with bad lengths
      Bytes piece = {static_cast<uint8_t>(FakeIsh::hostif(5, 0x21, Bytes{}).size())};
      fw.to_host_piece(kFwHid, fw.host_addr, rnd() & 1, b.data(), b.size() > 20 ? 20 : b.size());
    }
  }
  run(driver, 500);
  SensorReading r;
  hub.read(SensorKind::Accelerometer, &r);                         // (nothing crashes; whether it still works is not promised after garbage)
  // truncated and flipped valid reports
  FakeIsh fw2;
  SensorHub hub2;
  IshDriver d2(a);
  CHECK(d2.start_registers(fw2, &hub2) == IshDriver::Outcome::Started);
  const Bytes good = FakeIsh::hostif(5, 0x21, accel_report(1, 2, 3));
  for (size_t n = 0; n <= good.size(); ++n) { Bytes cut(good.begin(), good.begin() + n); fw2.send_hid(cut); run(d2, 3); }
  for (size_t bit = 0; bit < good.size() * 8; ++bit) { Bytes f = good; f[bit / 8] ^= static_cast<uint8_t>(1u << (bit % 8)); fw2.send_hid(f); run(d2, 3); }
}

int main() {
  test_bring_up();
  test_failures();
  test_firmware_reset();
  test_garbage();
  printf(g_failed ? "IshTest: %d FAILED\n" : "IshTest: all passed\n", g_failed);
  return g_failed ? 1 : 0;
}
