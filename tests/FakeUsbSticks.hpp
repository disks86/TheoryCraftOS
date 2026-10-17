// Shared by the storage hot-plug tests: a thread-safe counting allocator, FAT12 volume images, and a fake xHCI-like host controller with
// USB sticks (Bulk-Only mass-storage targets) that can be plugged in and pulled out.
#pragma once
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <mutex>
#include <string>
#include <vector>
#include "Allocators/IAllocator.hpp"
#include "Hardware/Bus/Pci/IPciBus.hpp"
#include "Hardware/Bus/Usb/IUsbBus.hpp"

using namespace hardware;

struct TestAllocator final : allocators::IAllocator {
  long live = 0;   // (atomic: threads share the allocator in the thread test)
  void* allocate(uint64_t size) override { if (!size) return nullptr; __atomic_add_fetch(&live, 1, __ATOMIC_RELAXED); return aligned_alloc(64, (size + 63) / 64 * 64); }
  void* allocate_aligned(uint64_t size, uint64_t alignment) override {
    if (!size) return nullptr;
    __atomic_add_fetch(&live, 1, __ATOMIC_RELAXED);
    if (alignment < 64) alignment = 64;
    return aligned_alloc(alignment, (size + alignment - 1) / alignment * alignment);
  }
  void deallocate(void* p) override { if (p) { __atomic_sub_fetch(&live, 1, __ATOMIC_RELAXED); free(p); } }
};

typedef std::vector<uint8_t> Bytes;

// ------------------------------------------------------------------------------------------------ disk images

static void put16(uint8_t* p, uint32_t v) { p[0] = static_cast<uint8_t>(v); p[1] = static_cast<uint8_t>(v >> 8); }
static void put32(uint8_t* p, uint32_t v) { put16(p, v); put16(p + 2, v >> 16); }

// A FAT12 floppy-sized volume with a few files in the root directory.
struct Fat12 {
  Bytes img = Bytes(2880 * 512);
  uint32_t next = 2, entries = 0;

  explicit Fat12(const char* label11 = "SMALLVOL   ") {
    uint8_t* b = img.data();
    b[0] = 0xEB; b[1] = 0x3C; b[2] = 0x90;
    memcpy(b + 3, "TESTFMT ", 8);
    put16(b + 11, 512); b[13] = 1; put16(b + 14, 1); b[16] = 2; put16(b + 17, 224);
    put16(b + 19, 2880); b[21] = 0xF8; put16(b + 22, 9);
    memcpy(b + 43, label11, 11);
    b[510] = 0x55; b[511] = 0xAA;
    set_fat(0, 0xFF8);
    set_fat(1, 0xFFF);
  }
  void set_fat(uint32_t c, uint32_t v) {
    for (int copy = 0; copy < 2; ++copy) {
      uint8_t* f = img.data() + 512 + copy * 9 * 512;
      const uint32_t o = c + c / 2;
      if (c & 1) { f[o] = (f[o] & 0x0F) | ((v & 0xF) << 4); f[o + 1] = static_cast<uint8_t>(v >> 4); }
      else { f[o] = static_cast<uint8_t>(v); f[o + 1] = (f[o + 1] & 0xF0) | ((v >> 8) & 0xF); }
    }
  }
  void file(const char* name83, const Bytes& data) {
    uint32_t first = 0, prev = 0;
    for (size_t off = 0; off < data.size(); off += 512) {
      const uint32_t c = next++;
      if (prev) set_fat(prev, c); else first = c;
      set_fat(c, 0xFFF);
      const size_t n = data.size() - off < 512 ? data.size() - off : 512;
      memcpy(img.data() + (33 + c - 2) * 512, data.data() + off, n);
      prev = c;
    }
    uint8_t* e = img.data() + (1 + 18) * 512 + (entries++) * 32;
    memcpy(e, name83, 11);
    e[11] = 0x20;
    put16(e + 26, first);
    put32(e + 28, static_cast<uint32_t>(data.size()));
  }
};

static Bytes pattern(size_t n, uint8_t seed) {
  Bytes b(n);
  for (size_t i = 0; i < n; ++i) b[i] = static_cast<uint8_t>(seed + i * 7 + (i >> 8));
  return b;
}

static Bytes stick_with_files(const char* label = "SMALLVOL   ") {
  Fat12 fat(label);
  fat.file("HELLO   TXT", Bytes{'h', 'e', 'l', 'l', 'o', ' ', 'f', 'a', 't'});
  fat.file("BIG     BIN", pattern(3000, 5));
  return fat.img;
}

// The same volume behind an MBR partition table, starting at block 8.
static Bytes partitioned_stick() {
  Fat12 fat;
  fat.file("HELLO   TXT", Bytes{'p', 'a', 'r', 't'});
  Bytes disk(8 * 512 + fat.img.size());
  memcpy(disk.data() + 8 * 512, fat.img.data(), fat.img.size());
  uint8_t* e = disk.data() + 446;
  e[0] = 0x80; e[4] = 0x01;
  put32(e + 8, 8);
  put32(e + 12, 2880);
  disk[510] = 0x55; disk[511] = 0xAA;
  return disk;
}

// ------------------------------------------------------------------------------------------------ the fake host with sticks

// An absolute pointer: three buttons, X and Y as 16 bits each in 0..32767.
static const uint8_t kTabletDescriptor[] = {0x05, 0x01, 0x09, 0x02, 0xA1, 0x01, 0x09, 0x01, 0xA1, 0x00, 0x05, 0x09, 0x19, 0x01, 0x29, 0x03, 0x15, 0x00, 0x25, 0x01,
                                            0x95, 0x03, 0x75, 0x01, 0x81, 0x02, 0x95, 0x01, 0x75, 0x05, 0x81, 0x03, 0x05, 0x01, 0x09, 0x30, 0x09, 0x31, 0x15, 0x00,
                                            0x26, 0xFF, 0x7F, 0x75, 0x10, 0x95, 0x02, 0x81, 0x02, 0xC0, 0xC0};

// A Bulk-Only mass-storage target (or a keyboard, mouse or tablet) behind a root port.
struct FakeStickHost final : IUsbHostController {
  struct Stick {
    Bytes image;
    uint32_t pending_data = 0;       // bytes of data the host still reads (IN) after the last command
    Bytes response;
    uint32_t write_lba = 0, write_blocks = 0;
    bool expect_write = false;
    uint8_t csw_status = 0;
    uint32_t tag = 0;
    int commands = 0;
    int unplug_after = -1;           // yank the stick after this many commands
    bool broken_inquiry = false;     // answers nothing sensible
    bool no_storage = false;         // not a mass-storage device at all
    // Human interface devices instead of storage: 0 none, 1 boot keyboard, 2 boot mouse, 3 absolute tablet (report descriptor served), 4 vendor device
    int hid = 0;
    std::vector<Bytes> reports;      // what the device has sent and the host has not read yet
    bool fail_start = false;         // the interrupt endpoint cannot be set up
    // A printer (hid == 5): a bidirectional USB printer interface. Everything sent to it lands in `printed`.
    std::string device_id = "MFG:Brother;CMD:PJL,PCL;MDL:HL-2070N series;CLS:PRINTER;";
    uint8_t port_status = 0x18;      // selected, no error
    Bytes printed;
    int fail_out = 0;                // bulk OUT transfers still to fail
    // Storage: what the host asked for, and how the stick behaves when it is ejected.
    std::vector<uint8_t> opcodes;    // every SCSI command received
    bool sync_unsupported = false;   // SYNCHRONIZE CACHE: ILLEGAL REQUEST
    bool sync_fails = false;         // SYNCHRONIZE CACHE: MEDIUM ERROR
    uint8_t sense_key = 0;           // reported by the next REQUEST SENSE
    bool stopped = false, tray_opened = false, removal_allowed = false;
    bool optical = false;            // answers INQUIRY as a CD/DVD drive
  };
  static constexpr uint32_t kPorts = 4;
  Stick* sticks[kPorts] = {};
  uint32_t generation[kPorts] = {};
  std::vector<UsbHotplugEvent> events;
  bool hotplug = true;
  uint32_t set_configurations = 0;
  mutable std::recursive_mutex m;   // the host is shared between threads in the thread test

  const char* name() const override { return "fake host"; }
  bool initialize() override { return true; }
  void shutdown() override {}
  UsbControllerKind kind() const override { return UsbControllerKind::Xhci; }
  uint32_t port_count() const override { return kPorts; }
  bool port_info(uint32_t port, UsbPortInfo* out) const override { std::lock_guard<std::recursive_mutex> g(m); out->connected = port < kPorts && sticks[port]; out->speed = UsbSpeed::High; return true; }
  bool device_descriptor(uint32_t port, UsbDeviceDescriptor* out) const override { std::lock_guard<std::recursive_mutex> g(m);
    if (port >= kPorts || !sticks[port]) return false;
    memset(out, 0, sizeof *out);
    out->length = 18; out->descriptor_type = 1; out->vendor_id = 0x0781; out->product_id = 0x5567;
    return true;
  }
  bool configuration_descriptor(uint32_t port, ByteView buffer, uint16_t* length) const override { std::lock_guard<std::recursive_mutex> g(m);
    if (port >= kPorts || !sticks[port]) return false;
    Bytes c;
    auto put = [&](std::initializer_list<uint8_t> b) { c.insert(c.end(), b.begin(), b.end()); };
    put({9, 2, 0, 0, 1, 1, 0, 0x80, 50});
    const int hid = sticks[port]->hid;
    if (hid == 1) {   // boot keyboard
      put({9, 4, 0, 0, 1, 3, 1, 1, 0});
      put({9, 0x21, 0x11, 0x01, 0, 1, 0x22, 63, 0});
      put({7, 5, 0x81, 3, 8, 0, 10});
    } else if (hid == 2) {   // boot mouse
      put({9, 4, 0, 0, 1, 3, 1, 2, 0});
      put({9, 0x21, 0x11, 0x01, 0, 1, 0x22, 52, 0});
      put({7, 5, 0x81, 3, 4, 0, 10});
    } else if (hid == 3) {   // tablet: not a boot device, described by its report descriptor
      put({9, 4, 0, 0, 1, 3, 0, 0, 0});
      put({9, 0x21, 0x11, 0x01, 0, 1, 0x22, static_cast<uint8_t>(sizeof(kTabletDescriptor)), 0});
      put({7, 5, 0x81, 3, 8, 0, 4});
    } else if (hid == 5) {   // printer, bidirectional
      put({9, 4, 0, 0, 2, 7, 1, 2, 0});
      put({7, 5, 0x01, 2, 0x00, 0x02, 0});
      put({7, 5, 0x82, 2, 0x00, 0x02, 0});
    } else if (hid == 4) {   // vendor specific
      put({9, 4, 0, 0, 1, 0xFF, 0, 0, 0});
      put({7, 5, 0x81, 3, 8, 0, 10});
    } else if (sticks[port]->no_storage) {
      put({9, 4, 0, 0, 1, 3, 1, 1, 0});   // a keyboard
      put({7, 5, 0x81, 3, 8, 0, 10});
    } else {
      put({9, 4, 0, 0, 2, 8, 6, 0x50, 0});
      put({7, 5, 0x81, 2, 0x00, 0x02, 0});
      put({7, 5, 0x02, 2, 0x00, 0x02, 0});
    }
    c[2] = static_cast<uint8_t>(c.size());
    const size_t n = c.size() < buffer.size() ? c.size() : buffer.size();
    memcpy(buffer.data(), c.data(), n);
    *length = static_cast<uint16_t>(c.size());
    return true;
  }
  bool set_configuration(uint32_t port, uint8_t) override { std::lock_guard<std::recursive_mutex> g(m); ++set_configurations; return port < kPorts && sticks[port]; }
  bool control_out(uint32_t port, uint8_t, uint8_t, uint16_t, uint16_t) override { std::lock_guard<std::recursive_mutex> g(m); return port < kPorts && sticks[port]; }
  bool control_in(uint32_t port, uint8_t, uint8_t request, uint16_t value, uint16_t, ByteView buffer) override {   // (no GET_MAX_LUN: a stall)
    std::lock_guard<std::recursive_mutex> g(m);
    if (port >= kPorts || !sticks[port]) return false;
    Stick& s = *sticks[port];
    if (s.hid == 5) {   // printer class requests
      memset(buffer.data(), 0, buffer.size());
      if (request == 0) {   // GET_DEVICE_ID: big-endian length (counting itself), then the text
        const size_t n = s.device_id.size() + 2 < buffer.size() ? s.device_id.size() + 2 : buffer.size();
        buffer[0] = static_cast<uint8_t>((s.device_id.size() + 2) >> 8);
        buffer[1] = static_cast<uint8_t>(s.device_id.size() + 2);
        if (n > 2) memcpy(buffer.data() + 2, s.device_id.data(), n - 2);
        return true;
      }
      if (request == 1 && buffer.size() >= 1) { buffer[0] = s.port_status; return true; }   // GET_PORT_STATUS
      return false;
    }
    if (s.hid != 3 || request != 6 || value != 0x2200) return false;
    memcpy(buffer.data(), kTabletDescriptor, buffer.size() < sizeof kTabletDescriptor ? buffer.size() : sizeof kTabletDescriptor);
    return true;
  }
  bool start_interrupt_in(uint32_t port, uint8_t, uint16_t, uint8_t) override {
    std::lock_guard<std::recursive_mutex> g(m);
    return port < kPorts && sticks[port] && sticks[port]->hid && !sticks[port]->fail_start;
  }
  bool poll_interrupt_in(uint32_t port, uint8_t, ByteView buffer, uint16_t* length) override {
    std::lock_guard<std::recursive_mutex> g(m);
    if (port >= kPorts || !sticks[port] || sticks[port]->reports.empty()) return false;
    const Bytes r = sticks[port]->reports.front();
    sticks[port]->reports.erase(sticks[port]->reports.begin());
    const size_t n = r.size() < buffer.size() ? r.size() : buffer.size();
    memcpy(buffer.data(), r.data(), n);
    *length = static_cast<uint16_t>(n);
    return true;
  }
  // The device sends a report (it is read by whoever polls the port).
  void send_report(uint32_t port, const Bytes& report) {
    std::lock_guard<std::recursive_mutex> g(m);
    if (port < kPorts && sticks[port]) sticks[port]->reports.push_back(report);
  }
  bool start_bulk_endpoint(uint32_t port, uint8_t, uint16_t) override { std::lock_guard<std::recursive_mutex> g(m); return port < kPorts && sticks[port]; }
  bool reset_bulk_endpoint(uint32_t port, uint8_t) override { std::lock_guard<std::recursive_mutex> g(m); return port < kPorts && sticks[port]; }

  bool supports_hotplug() const override { return hotplug; }
  bool poll_hotplug(UsbHotplugEvent* event) override { std::lock_guard<std::recursive_mutex> g(m);
    if (events.empty()) return false;
    *event = events.front();
    events.erase(events.begin());
    return true;
  }
  uint32_t port_generation(uint32_t port) const override { std::lock_guard<std::recursive_mutex> g(m); return port < kPorts ? generation[port] : 0; }

  void plug(uint32_t port, Stick* stick) { std::lock_guard<std::recursive_mutex> g(m);
    sticks[port] = stick;
    if (hotplug) ++generation[port];
    events.push_back({0, port, true});
  }
  void unplug(uint32_t port) { std::lock_guard<std::recursive_mutex> g(m);
    sticks[port] = nullptr;
    if (hotplug) ++generation[port];
    events.push_back({0, port, false});
  }

  static uint32_t be32(const uint8_t* p) { return (uint32_t{p[0]} << 24) | (uint32_t{p[1]} << 16) | (uint32_t{p[2]} << 8) | p[3]; }

  void command(Stick& s, const uint8_t* cbw) {
    s.tag = cbw[4] | (cbw[5] << 8) | (cbw[6] << 16) | (uint32_t{cbw[7]} << 24);
    const uint8_t* cdb = cbw + 15;
    s.response.clear();
    s.csw_status = 0;
    s.expect_write = false;
    s.opcodes.push_back(cdb[0]);
    switch (cdb[0]) {
      case 0x12:   // INQUIRY
        if (s.broken_inquiry) { s.csw_status = 1; break; }
        s.response.assign(36, 0);
        s.response[0] = s.optical ? 5 : 0;
        s.response[1] = 0x80;   // removable
        s.response[4] = 31;
        memcpy(&s.response[8], "TESTSTK Hot-plug stick  ", 24);
        break;
      case 0x00: break;   // TEST UNIT READY
      case 0x25: {        // READ CAPACITY(10)
        s.response.assign(8, 0);
        const uint32_t last = static_cast<uint32_t>(s.image.size() / 512 - 1);
        for (int i = 0; i < 4; ++i) s.response[i] = static_cast<uint8_t>(last >> (24 - 8 * i));
        s.response[6] = 2;   // 512
        break;
      }
      case 0x28: {        // READ(10)
        const uint32_t lba = be32(cdb + 2), count = (cdb[7] << 8) | cdb[8];
        if (static_cast<uint64_t>(lba + count) * 512 > s.image.size()) { s.csw_status = 1; break; }
        s.response.assign(s.image.begin() + lba * 512, s.image.begin() + (lba + count) * 512);
        break;
      }
      case 0x2A:          // WRITE(10)
        s.write_lba = be32(cdb + 2);
        s.write_blocks = (cdb[7] << 8) | cdb[8];
        s.expect_write = true;
        break;
      case 0x35:          // SYNCHRONIZE CACHE
        if (s.sync_unsupported) { s.csw_status = 1; s.sense_key = 5; }
        else if (s.sync_fails) { s.csw_status = 1; s.sense_key = 3; }
        break;
      case 0x1E: s.removal_allowed = (cdb[4] & 1) == 0; break;   // PREVENT ALLOW MEDIUM REMOVAL
      case 0x1B: s.stopped = (cdb[4] & 1) == 0; s.tray_opened = (cdb[4] & 2) != 0; break;   // START STOP UNIT
      case 0x03:          // REQUEST SENSE
        s.response.assign(18, 0);
        s.response[0] = 0x70;
        s.response[2] = s.sense_key;
        s.sense_key = 0;
        break;
      default: s.csw_status = 1; break;
    }
    s.pending_data = static_cast<uint32_t>(s.response.size());
  }

  bool bulk_transfer(uint32_t port, uint8_t endpoint, ByteView data, uint32_t* transferred) override { std::lock_guard<std::recursive_mutex> g(m);
    if (transferred) *transferred = 0;
    if (port >= kPorts || !sticks[port]) return false;
    Stick& s = *sticks[port];
    if (s.hid == 5) {   // a printer: bytes out are printed, nothing comes back
      if (endpoint & 0x80) return true;
      if (s.unplug_after >= 0 && s.commands >= s.unplug_after) { unplug(port); return false; }
      ++s.commands;
      if (s.fail_out > 0) { --s.fail_out; return false; }
      s.printed.insert(s.printed.end(), data.data(), data.data() + data.size());
      if (transferred) *transferred = static_cast<uint32_t>(data.size());
      return true;
    }
    if (s.unplug_after >= 0 && s.commands >= s.unplug_after) {   // pulled out in the middle of the command
      unplug(port);
      return false;
    }
    if (!(endpoint & 0x80)) {
      if (s.expect_write) {   // the data stage of a write
        const size_t n = data.size() < s.write_blocks * 512u ? data.size() : s.write_blocks * 512u;
        if ((s.write_lba * 512u) + n <= s.image.size()) memcpy(s.image.data() + s.write_lba * 512u, data.data(), n);
        s.expect_write = false;
        if (transferred) *transferred = static_cast<uint32_t>(data.size());
        return true;
      }
      if (data.size() != 31 || memcmp(data.data(), "USBC", 4) != 0) return false;   // a command block wrapper
      ++s.commands;
      command(s, data.data());
      if (transferred) *transferred = 31;
      return true;
    }
    if (s.pending_data > 0) {   // the data stage of a read
      const size_t n = s.pending_data < data.size() ? s.pending_data : data.size();
      memcpy(data.data(), s.response.data(), n);
      s.pending_data = 0;
      if (transferred) *transferred = static_cast<uint32_t>(n);
      return true;
    }
    if (data.size() != 13) return false;
    memcpy(data.data(), "USBS", 4);
    memcpy(data.data() + 4, &s.tag, 4);
    memset(data.data() + 8, 0, 4);
    data.data()[12] = s.csw_status;
    if (transferred) *transferred = 13;
    return true;
  }
};

struct FakePci final : IPciBus {
  const char* name() const override { return "fake pci"; }
  bool initialize() override { return true; }
  void shutdown() override {}
  uint32_t device_count() const override { return 0; }
  bool pci_device(uint32_t, PciDeviceInfo*) const override { return false; }
  uint32_t read32(PciAddress, uint16_t) const override { return 0; }
  void write32(PciAddress, uint16_t, uint32_t) const override {}
  uint64_t memory_bar(PciAddress, uint8_t) const override { return 0; }
  uint16_t find_capability(PciAddress, uint8_t) const override { return 0; }
};

struct FakeUsb final : IUsbBus {
  std::vector<FakeStickHost*> hosts;
  const char* name() const override { return "fake usb"; }
  bool initialize() override { return true; }
  void shutdown() override {}
  uint32_t device_count() const override { return 0; }
  uint32_t controller_count() const override { return static_cast<uint32_t>(hosts.size()); }
  UsbControllerKind controller_kind(uint32_t) const override { return UsbControllerKind::Xhci; }
  uint32_t port_count(uint32_t c) const override { return hosts[c]->port_count(); }
  bool port_info(uint32_t c, uint32_t p, UsbPortInfo* out) const override { return hosts[c]->port_info(p, out); }
  bool device_descriptor(uint32_t c, uint32_t p, UsbDeviceDescriptor* out) const override { return hosts[c]->device_descriptor(p, out); }
  IUsbHostController* host_controller(uint32_t c) override { return c < hosts.size() ? hosts[c] : nullptr; }
  bool service_hotplug() override {
    bool changed = false;
    for (FakeStickHost* h : hosts) {
      UsbHotplugEvent event;
      while (h->supports_hotplug() && h->poll_hotplug(&event)) changed = true;
    }
    return changed;
  }
};

static FakeStickHost::Stick make_stick(Bytes image) {
  FakeStickHost::Stick s;
  s.image = static_cast<Bytes&&>(image);
  return s;
}

