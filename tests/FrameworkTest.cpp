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

// Host-side tests for Framework laptop support that is not the embedded controller: SMBIOS (entry points, the structure table, cut-short and
// bit-flipped tables), the Framework model and module catalogue, USB CDC-ACM serial ports (descriptors, line coding, a port against a fake
// host, hot-plug through UsbSerialPorts and SystemSerialPorts), and the Laptop 16's LED matrix module commands.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <deque>
#include <string>
#include <vector>
#include "Hardware/Firmware/Smbios.hpp"
#include "Hardware/Platform/Framework/FrameworkPlatform.hpp"
#include "Hardware/Platform/Framework/LedMatrix.hpp"
#include "Hardware/SerialPort/Usb/UsbCdcAcm.hpp"
#include "Hardware/SerialPort/Usb/UsbSerialPorts.hpp"

using namespace hardware;

struct TestAllocator final : allocators::IAllocator {
  int live = 0;
  void* allocate(uint64_t size) override { if (!size) return nullptr; ++live; return aligned_alloc(64, (size + 63) / 64 * 64); }
  void* allocate_aligned(uint64_t size, uint64_t alignment) override {
    if (!size) return nullptr;
    ++live;
    if (alignment < 64) alignment = 64;
    return aligned_alloc(alignment, (size + alignment - 1) / alignment * alignment);
  }
  void deallocate(void* p) override { if (p) { --live; free(p); } }
};

static int g_failed = 0, g_checks = 0;
#define CHECK(...) do { ++g_checks; if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)

typedef std::vector<uint8_t> Bytes;
static ConstByteView view(const Bytes& b) { return ConstByteView(b.data(), b.size()); }

// --- SMBIOS ---------------------------------------------------------------------------------------------

// One structure: its formatted part (type and length filled in) and its strings.
static void add_structure(Bytes& t, uint8_t type, Bytes formatted, std::initializer_list<const char*> strings) {
  formatted.insert(formatted.begin(), {type, 0, 0x00, 0x01});
  formatted[1] = static_cast<uint8_t>(formatted.size());
  t.insert(t.end(), formatted.begin(), formatted.end());
  for (const char* s : strings) { t.insert(t.end(), s, s + strlen(s)); t.push_back(0); }
  if (strings.size() == 0) t.push_back(0);
  t.push_back(0);
}

static Bytes framework_table(const char* product) {
  Bytes t;
  add_structure(t, 0, {1, 2, 0x00, 0xE8, 0x03, 0x00, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}, {"INSYDE Corp.", "03.05", "03/29/2024"});
  Bytes sys = {1, 2, 3, 4};                                 // manufacturer, product, version, serial
  for (int i = 0; i < 16; ++i) sys.push_back(static_cast<uint8_t>(i));   // UUID
  sys.insert(sys.end(), {6, 5, 6});                         // wake-up type, SKU, family
  add_structure(t, 1, sys, {"Framework", product, "A6", "FRANDGCP0123", "Laptop"});
  add_structure(t, 2, {1, 2, 3}, {"Framework", "FRANMDCP06", "A6"});
  add_structure(t, 3, {1, 10}, {"Framework"});
  // Processor: socket, type, family, manufacturer, id[8], version (string 2), voltage, ext clock(2), max speed(2), current(2), status.
  Bytes cpu(0x2A - 4, 0);
  cpu[0x10 - 4] = 2;
  cpu[0x14 - 4] = 0x40;
  cpu[0x15 - 4] = 0x13;                                     // 4928 MHz
  cpu[0x18 - 4] = 0x41;                                     // populated, enabled
  add_structure(t, 4, cpu, {"FP8", "AMD Ryzen 7 7840U w/ Radeon 780M Graphics"});
  // Two memory devices: 16 GiB DDR5 at 5600, and an empty slot.
  for (int slot = 0; slot < 2; ++slot) {
    Bytes m(0x28 - 4, 0);
    m[0x0C - 4] = slot == 0 ? 0x00 : 0x00;
    m[0x0D - 4] = slot == 0 ? 0x40 : 0x00;                  // 0x4000 MiB = 16 GiB
    m[0x10 - 4] = 1;                                        // locator string
    m[0x12 - 4] = 0x22;                                     // DDR5
    m[0x15 - 4] = 0xE0; m[0x16 - 4] = 0x15;                 // rated 5600
    m[0x17 - 4] = 2;                                        // manufacturer
    m[0x1A - 4] = 3;                                        // part
    m[0x20 - 4] = 0xE0; m[0x21 - 4] = 0x15;                 // configured 5600
    add_structure(t, 17, m, {slot == 0 ? "DIMM 0" : "DIMM 1", "Crucial", "CT16G56C46S5.M8G1"});
  }
  add_structure(t, 127, {}, {});
  return t;
}

static Bytes entry3(uint64_t address, uint32_t size) {
  Bytes e = {'_', 'S', 'M', '3', '_', 0, 0x18, 3, 6, 0, 1, 0};
  for (int i = 0; i < 4; ++i) e.push_back(static_cast<uint8_t>(size >> (8 * i)));
  for (int i = 0; i < 8; ++i) e.push_back(static_cast<uint8_t>(address >> (8 * i)));
  uint8_t sum = 0;
  for (uint8_t b : e) sum = static_cast<uint8_t>(sum + b);
  e[5] = static_cast<uint8_t>(-sum);
  return e;
}

static void test_smbios() {
  const Bytes table = framework_table("Laptop 13 (AMD Ryzen 7040Series)");
  SmbiosInfo s;
  CHECK(parse_smbios_table(view(table), &s));
  CHECK(strcmp(s.manufacturer, "Framework") == 0 && strcmp(s.product, "Laptop 13 (AMD Ryzen 7040Series)") == 0 && strcmp(s.sku, "Laptop") == 0);
  CHECK(strcmp(s.bios_vendor, "INSYDE Corp.") == 0 && strcmp(s.bios_version, "03.05") == 0 && strcmp(s.bios_date, "03/29/2024") == 0);
  CHECK(strcmp(s.board_product, "FRANMDCP06") == 0 && s.chassis_type == 10 && s.is_portable() && strcmp(smbios_chassis_type_name(10), "Notebook") == 0);
  CHECK(s.uuid[0] == 0 && s.uuid[15] == 15);
  CHECK(strcmp(s.processor, "AMD Ryzen 7 7840U w/ Radeon 780M Graphics") == 0 && s.processor_max_mhz == 4928 && s.processor_count == 1);
  CHECK(s.memory_count == 1 && s.memory_total_mib == 16384 && s.memory[0].speed_mts == 5600 && strcmp(smbios_memory_type_name(s.memory[0].type), "DDR5") == 0);
  CHECK(strcmp(s.memory[0].part, "CT16G56C46S5.M8G1") == 0 && strcmp(s.memory[0].locator, "DIMM 0") == 0);
  CHECK(s.structures == 8 && !s.truncated);   // (BIOS, system, board, chassis, processor, two memory devices, end)

  // Entry points: the 64-bit one, and a wrong checksum.
  uint64_t address = 0;
  uint32_t length = 0;
  uint8_t major = 0, minor = 0;
  Bytes e = entry3(0x7A000000, 4096);
  CHECK(parse_smbios_entry(view(e), &address, &length, &major, &minor) && address == 0x7A000000 && length == 4096 && major == 3 && minor == 6);
  e[9] ^= 1;
  CHECK(!parse_smbios_entry(view(e), &address, &length, &major, &minor));
  // The 32-bit one.
  Bytes e2 = {'_', 'S', 'M', '_', 0, 0x1F, 2, 8, 0, 0, 0, 0, 0, 0, 0, 0, '_', 'D', 'M', 'I', '_', 0, 0x00, 0x10, 0x00, 0x00, 0x0E, 0x00, 9, 0, 0x28};
  auto fix = [](Bytes& b, size_t from, size_t count, size_t at) { uint8_t sum = 0; for (size_t i = from; i < from + count; ++i) if (i != at) sum = static_cast<uint8_t>(sum + b[i]); b[at] = static_cast<uint8_t>(-sum); };
  fix(e2, 0x10, 0x0F, 0x15);
  fix(e2, 0, 0x1F, 4);
  CHECK(parse_smbios_entry(view(e2), &address, &length, &major, &minor) && address == 0x0E0000 && length == 0x1000 && major == 2 && minor == 8);
  CHECK(!parse_smbios_entry(ConstByteView(), &address, &length, &major, &minor));

  // A string number past the set is empty; a cut-short table stops where it breaks and never reads past it.
  Bytes odd;
  add_structure(odd, 1, {1, 9, 3, 4}, {"Maker"});
  add_structure(odd, 127, {}, {});
  CHECK(parse_smbios_table(view(odd), &s) && strcmp(s.manufacturer, "Maker") == 0 && s.product[0] == 0);
  for (size_t n = 0; n < table.size(); ++n) {
    Bytes cut(table.begin(), table.begin() + n);
    (void)parse_smbios_table(view(cut), &s);
  }
  for (size_t bit = 0; bit < table.size() * 8; ++bit) {
    Bytes flipped = table;
    flipped[bit / 8] ^= static_cast<uint8_t>(1u << (bit % 8));
    (void)parse_smbios_table(view(flipped), &s);
    CHECK(s.memory_count <= SmbiosInfo::kMaxMemory && strlen(s.product) < SmbiosInfo::kText);
  }
  // A structure that claims to be longer than the table.
  Bytes liar = {1, 200, 0, 0, 1, 0, 0};
  CHECK(!parse_smbios_table(view(liar), &s));
}

static void test_framework_models() {
  struct Case { const char* product; framework::Model model; };
  const Case cases[] = {
      {"Laptop (11th Gen Intel Core)", framework::Model::Laptop13Intel11},
      {"Laptop (12th Gen Intel Core)", framework::Model::Laptop13Intel12},
      {"Laptop (13th Gen Intel Core)", framework::Model::Laptop13Intel13},
      {"Laptop 13 (Intel Core Ultra Series 1)", framework::Model::Laptop13Ultra1},
      {"Laptop 13 (AMD Ryzen 7040Series)", framework::Model::Laptop13Amd7040},
      {"Laptop 13 (AMD Ryzen AI 300 Series)", framework::Model::Laptop13AmdAi300},
      {"Laptop 16 (AMD Ryzen 7040 Series)", framework::Model::Laptop16Amd7040},
      {"Laptop 16 (AMD Ryzen AI 300 Series)", framework::Model::Laptop16AmdAi300},
      {"Laptop 12 (13th Gen Intel Core)", framework::Model::Laptop12Intel13},
      {"Desktop (AMD Ryzen AI Max 300 Series)", framework::Model::Desktop},
      {"Laptop 17 (Something New)", framework::Model::Unknown},
  };
  for (const Case& c : cases) {
    SmbiosInfo s;
    CHECK(parse_smbios_table(view(framework_table(c.product)), &s));
    const framework::ModelInfo* m = framework::identify(s);
    CHECK(m && m->model == c.model);
  }
  SmbiosInfo s;
  CHECK(parse_smbios_table(view(framework_table("Laptop 16 (AMD Ryzen 7040 Series)")), &s));
  const framework::ModelInfo* m16 = framework::identify(s);
  CHECK(m16 && (m16->features & framework::kInputModules) && (m16->features & framework::kExpansionBay) && m16->expansion_card_bays == 6 && m16->ec == framework::EcChip::Npcx);
  strcpy(s.manufacturer, "Dell Inc.");
  CHECK(framework::identify(s) == nullptr);
  CHECK(framework::find_module(0x32AC, 0x0020) && framework::find_module(0x32AC, 0x0020)->kind == framework::ModuleKind::InputModule);
  CHECK(framework::find_module(0x0BDA, 0x8156) && !framework::find_module(0x1234, 0x0001));
  for (uint32_t i = 0; i < framework::module_count(); ++i)
    for (uint32_t j = i + 1; j < framework::module_count(); ++j)
      CHECK(!(framework::module_at(i).vendor_id == framework::module_at(j).vendor_id && framework::module_at(i).product_id == framework::module_at(j).product_id));
}

// --- CDC-ACM --------------------------------------------------------------------------------------------

struct Config {
  Bytes b;
  void add(std::initializer_list<uint8_t> d) {
    b.push_back(static_cast<uint8_t>(d.size() + 1));
    b.insert(b.end(), d.begin(), d.end());
  }
  Bytes done() {
    Bytes out = {9, 2, static_cast<uint8_t>(b.size() + 9), static_cast<uint8_t>((b.size() + 9) >> 8), 2, 1, 0, 0x80, 50};
    out.insert(out.end(), b.begin(), b.end());
    return out;
  }
};

// An RP2040-style ACM function: an interface association, the communication interface with its functional descriptors and notification
// endpoint, then the data interface with its bulk pair.
static Bytes acm_config(bool with_union = true) {
  Config c;
  c.add({11, 0, 2, 2, 2, 0, 0});                            // interface association (11)
  c.add({4, 0, 0, 1, 2, 2, 0, 0});                          // communication interface 0: ACM
  c.add({0x24, 0x00, 0x20, 0x01});                          // header
  c.add({0x24, 0x02, 0x02});                                // ACM: line coding and control lines
  if (with_union) c.add({0x24, 0x06, 0x00, 0x01});          // union: control 0, data 1
  c.add({0x24, 0x01, 0x00, 0x01});                          // call management
  c.add({5, 0x81, 3, 8, 0, 16});                            // notification endpoint
  c.add({4, 1, 0, 2, 0x0A, 0, 0, 0});                       // data interface 1
  c.add({5, 0x02, 2, 64, 0, 0});                            // bulk OUT
  c.add({5, 0x82, 2, 64, 0, 0});                            // bulk IN
  return c.done();
}

struct FakeHost final : IUsbHostController {
  Bytes config;
  uint16_t vendor = 0x2E8A, product = 0x000A;
  bool present = true, streams = true;
  uint32_t generation = 1;
  std::deque<Bytes> in_data, notifications;
  std::vector<Bytes> out_data, line_codings;
  std::vector<uint16_t> control_lines, breaks;
  const char* name() const override { return "fake host"; }
  bool initialize() override { return true; }
  void shutdown() override {}
  UsbControllerKind kind() const override { return UsbControllerKind::Xhci; }
  uint32_t port_count() const override { return 1; }
  bool port_info(uint32_t, UsbPortInfo* out) const override { out->connected = present; out->speed = UsbSpeed::Full; return true; }
  bool device_descriptor(uint32_t, UsbDeviceDescriptor* out) const override {
    if (!present) return false;
    memset(out, 0, sizeof *out);
    out->length = 18;
    out->descriptor_type = 1;
    out->vendor_id = vendor;
    out->product_id = product;
    out->configuration_count = 1;
    return true;
  }
  bool configuration_descriptor(uint32_t, ByteView buffer, uint16_t* length) const override {
    if (!present || config.size() > buffer.size()) return false;
    for (size_t i = 0; i < config.size(); ++i) buffer[i] = config[i];
    *length = static_cast<uint16_t>(config.size());
    return true;
  }
  bool set_configuration(uint32_t, uint8_t) override { return present; }
  bool control_out(uint32_t, uint8_t type, uint8_t request, uint16_t value, uint16_t index) override {
    if (!present || type != 0x21 || index != 0) return false;
    if (request == 0x22) control_lines.push_back(value);
    if (request == 0x23) breaks.push_back(value);
    return true;
  }
  bool control_in(uint32_t, uint8_t, uint8_t, uint16_t, uint16_t, ByteView) override { return false; }
  bool control_out_data(uint32_t, uint8_t type, uint8_t request, uint16_t, uint16_t index, ConstByteView data) override {
    if (!present || type != 0x21 || request != 0x20 || index != 0) return false;
    line_codings.push_back(Bytes(data.data(), data.data() + data.size()));
    return true;
  }
  bool start_interrupt_in(uint32_t, uint8_t endpoint, uint16_t, uint8_t) override { return present && endpoint == 0x81; }
  bool start_bulk_in_stream(uint32_t, uint8_t endpoint, uint16_t) override { return present && streams && endpoint == 0x82; }
  bool poll_interrupt_in(uint32_t, uint8_t endpoint, ByteView buffer, uint16_t* length) override {
    std::deque<Bytes>& q = endpoint == 0x82 ? in_data : notifications;
    if (!present || q.empty()) return false;
    const Bytes b = q.front();
    q.pop_front();
    for (size_t i = 0; i < b.size() && i < buffer.size(); ++i) buffer[i] = b[i];
    *length = static_cast<uint16_t>(b.size());
    return true;
  }
  bool start_bulk_endpoint(uint32_t, uint8_t endpoint, uint16_t) override { return present && endpoint == 0x02; }
  bool bulk_transfer(uint32_t, uint8_t endpoint, ByteView data, uint32_t* transferred) override {
    if (!present || endpoint != 0x02) return false;
    out_data.push_back(Bytes(data.data(), data.data() + data.size()));
    if (transferred) *transferred = static_cast<uint32_t>(data.size());
    return true;
  }
  bool reset_bulk_endpoint(uint32_t, uint8_t) override { return true; }
  uint32_t port_generation(uint32_t) const override { return generation; }
  void plug(uint16_t v, uint16_t p) { vendor = v; product = p; present = true; ++generation; }
  void unplug() { present = false; ++generation; }
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

struct NoPortIo final : ISerialPortIo {
  uint8_t read8(uint16_t) override { return 0xFF; }
  void write8(uint16_t, uint8_t) override {}
};

static void test_acm_protocol() {
  UsbAcmFunction f;
  const Bytes config = acm_config();
  CHECK(find_usb_acm_function(view(config), &f));
  CHECK(f.control_interface == 0 && f.data_interface == 1 && f.notify_endpoint == 0x81 && f.in_endpoint == 0x82 && f.out_endpoint == 0x02 && f.capabilities == 0x02);
  CHECK(find_usb_acm_function(view(acm_config(false)), &f));      // (no union descriptor: the next data interface)
  for (size_t n = 0; n < config.size(); ++n) {
    Bytes cut(config.begin(), config.begin() + n);
    UsbAcmFunction g;
    if (find_usb_acm_function(view(cut), &g)) CHECK(g.in_endpoint && g.out_endpoint);
  }
  Config hid;
  hid.add({4, 0, 0, 1, 3, 0, 0, 0});
  hid.add({5, 0x81, 3, 8, 0, 10});
  CHECK(!find_usb_acm_function(view(hid.done()), &f));

  SerialConfig sc;
  sc.baud = 9600;
  sc.data_bits = 7;
  sc.parity = SerialParity::Even;
  sc.stop_bits = SerialStopBits::Two;
  uint8_t coding[7];
  CHECK(encode_line_coding(sc, ByteView(coding)) && coding[0] == 0x80 && coding[1] == 0x25 && coding[4] == 2 && coding[5] == 2 && coding[6] == 7);
  sc.data_bits = 9;
  CHECK(!encode_line_coding(sc, ByteView(coding)));

  uint8_t lines = 0;
  SerialStatistics st;
  const Bytes state = {0xA1, 0x20, 0, 0, 0, 0, 2, 0, 0x01 | 0x02 | 0x40, 0};
  CHECK(decode_serial_state(view(state), &lines, &st) && lines == (SerialLines::kCts | SerialLines::kCarrier | SerialLines::kDsr) && st.overruns == 1);
  CHECK(!decode_serial_state(view(state).first(9), &lines, &st));
}

static void test_acm_port() {
  TestAllocator a;
  {
    FakeHost host;
    host.config = acm_config();
    UniquePointer<UsbCdcAcm> acm = UsbCdcAcm::open_device(host, 0, a);
    CHECK(acm && acm->state() == SerialState::Closed);
    CHECK(acm->write(view(Bytes{1, 2, 3})) == 0);                    // (closed: nothing taken)
    SerialConfig sc;
    CHECK(acm->open(sc) && acm->state() == SerialState::Open);
    CHECK(host.line_codings.size() == 1 && host.line_codings[0][0] == 0x00 && host.line_codings[0][1] == 0xC2 && host.line_codings[0][2] == 0x01);   // 115200
    CHECK(!host.control_lines.empty() && host.control_lines.back() == 3);
    CHECK(acm->write(view(Bytes{'h', 'i'})) == 2 && acm->transmit_pending());
    host.in_data.push_back({'o', 'k'});
    host.notifications.push_back({0xA1, 0x20, 0, 0, 0, 0, 2, 0, 0x02, 0});
    acm->poll();
    CHECK(host.out_data.size() == 1 && host.out_data[0] == Bytes({'h', 'i'}) && !acm->transmit_pending());
    uint8_t got[8];
    CHECK(acm->bytes_readable() == 2 && acm->read(ByteView(got)) == 2 && got[0] == 'o');
    CHECK((acm->lines() & SerialLines::kDsr) && (acm->lines() & SerialLines::kDtr));
    acm->set_lines(SerialLines::kDtr);
    CHECK(host.control_lines.back() == 1);
    acm->set_break(true);
    CHECK(host.breaks.back() == 0xFFFF);
    // More than the queue holds: the rest is an overrun.
    for (int i = 0; i < 300; ++i) host.in_data.push_back(Bytes(64, 'x'));
    for (int i = 0; i < 10; ++i) acm->poll();
    CHECK(acm->bytes_readable() == UsbCdcAcm::kQueue && acm->statistics().overruns > 0);
    host.unplug();
    CHECK(!acm->present() && acm->state() == SerialState::Error && acm->write(view(Bytes{1})) == 0);
    acm->poll();
    acm->close();
  }
  {
    // A controller that cannot keep a bulk IN endpoint armed: not taken.
    FakeHost host;
    host.config = acm_config();
    host.streams = false;
    CHECK(!UsbCdcAcm::open_device(host, 0, a));
  }
  CHECK(a.live == 0);
}

static void test_usb_serial_hotplug() {
  TestAllocator a;
  {
    FakeHost host;
    host.config = acm_config();
    host.plug(framework::kVendorFramework, framework::kLedMatrixProduct);
    FakeBus bus;
    bus.host = &host;
    NoPortIo io;
    SystemSerialPorts ports(io, a);
    UsbSerialPorts usb_serial(&bus, ports, a);
    std::vector<std::string> notes;
    usb_serial.set_notifier([](void* c, StringView t) { static_cast<std::vector<std::string>*>(c)->push_back(std::string(t.data(), t.size())); }, &notes);
    CHECK(usb_serial.initialize() == 1 && ports.count() == 1);
    CHECK(strcmp(ports.at(0)->name(), "Laptop 16 LED Matrix Input Module") == 0 && notes.size() == 1);

    // The LED matrix over that port.
    ledmatrix::LedMatrix matrix(ports.share(0));
    CHECK(matrix.open());
    CHECK(matrix.set_brightness(0x40) && matrix.show_percentage(150) && matrix.set_sleep(false));
    Bytes pixels(ledmatrix::kPixels, 0);
    pixels[0] = 1;
    pixels[ledmatrix::kPixels - 1] = 255;
    CHECK(matrix.draw(view(pixels)) && matrix.draw_greyscale(view(pixels)));
    CHECK(!matrix.draw(view(pixels).first(10)));
    ports.poll();
    Bytes sent;
    for (const Bytes& b : host.out_data) sent.insert(sent.end(), b.begin(), b.end());
    CHECK(sent.size() >= 6 && sent[0] == 0x32 && sent[1] == 0xAC && sent[2] == 0x00 && sent[3] == 0x40);   // brightness
    CHECK(sent[4] == 0x32 && sent[5] == 0xAC && sent[6] == 0x01 && sent[7] == 0x00 && sent[8] == 100);   // percentage, capped
    CHECK(sent[9] == 0x32 && sent[11] == 0x03 && sent[12] == 0);                                          // sleep off
    CHECK(sent[13] == 0x32 && sent[15] == 0x06 && sent[16] == 0x01 && sent[16 + 38] == 0x02);             // black and white: first pixel (bit 0) and last (pixel 305: byte 38, bit 1)
    CHECK(matrix.request_version());
    ports.poll();
    host.in_data.push_back({0, 5, 1});
    ports.poll();
    uint8_t major = 9, minor = 9, patch = 9;
    CHECK(matrix.poll_version(&major, &minor, &patch) && major == 0 && minor == 5 && patch == 1);

    // Pulled out: the port leaves the list (the matrix keeps a port that answers nothing); plugged back in: found again.
    host.unplug();
    CHECK(usb_serial.scan() == 1 && ports.count() == 0);
    CHECK(!matrix.set_brightness(1));
    host.plug(0x2E8A, 0x000A);
    CHECK(usb_serial.scan() == 1 && ports.count() == 1 && strcmp(ports.at(0)->name(), "USB serial port") == 0);
    ports.shutdown();
  }
  CHECK(a.live == 0);
}

static void test_led_matrix_encoding() {
  uint8_t out[80];
  CHECK(ledmatrix::encode(ledmatrix::Command::Version, ConstByteView(), ByteView(out)) == 3 && out[2] == 0x20);
  Bytes big(65, 0);
  CHECK(ledmatrix::encode(ledmatrix::Command::DrawBlackWhite, view(big), ByteView(out)) == 0);
  CHECK(ledmatrix::encode(ledmatrix::Command::Brightness, view(Bytes{1}), ByteView(out).first(3)) == 0);
  Bytes pixels(ledmatrix::kPixels, 0);
  pixels[8] = 1;                                     // row 0, column 8: bit 8
  pixels[9] = 1;                                     // row 1, column 0: bit 9
  uint8_t packed[ledmatrix::kBlackWhiteBytes];
  CHECK(ledmatrix::pack_black_white(view(pixels), ByteView(packed)) && packed[0] == 0 && packed[1] == 0x03 && packed[38] == 0);
  CHECK(!ledmatrix::pack_black_white(view(pixels), ByteView(packed).first(38)));
  uint8_t a, b, c;
  CHECK(!ledmatrix::decode_version(view(Bytes{1, 2}), &a, &b, &c));
}

int main() {
  test_smbios();
  test_framework_models();
  test_acm_protocol();
  test_acm_port();
  test_usb_serial_hotplug();
  test_led_matrix_encoding();
  printf("FrameworkTest: %d checks, %d failed\n", g_checks, g_failed);
  return g_failed ? 1 : 0;
}
