// Host tests of the drivers for consumer PCI Express network controllers: Realtek RTL8111/8168/8125 (r8169 family), Intel e1000e (82574, I217/I218/I219)
// and Intel igb/igc (82576, I210, I225/I226). Each chip is a register-level model that runs the descriptor rings the way the hardware does (fetching
// descriptors and buffers from "DMA" memory, writing back status), so bring-up, sending, receiving, the receive filter and the link report are checked
// without hardware. A hostile model also writes garbage descriptors to make sure nothing the device reports can push the driver past its buffers.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <map>
#include <random>
#include <vector>
#include "Hardware/NetworkInterface/Intel/E1000eNetworkInterface.hpp"
#include "Hardware/NetworkInterface/Intel/IgbNetworkInterface.hpp"
#include "Hardware/NetworkInterface/Realtek/Rtl8169NetworkInterface.hpp"

namespace hardware {
void delay_us(uint64_t) {}
}  // namespace hardware

using namespace hardware;

static int g_failed = 0;
#define CHECK(...) do { if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)

struct TestAllocator final : allocators::IAllocator {
  std::map<void*, size_t> dma;
  void* allocate(uint64_t size) override { return malloc(size); }
  void* allocate_aligned(uint64_t size, uint64_t alignment) override {
    const uint64_t al = alignment < 16 ? 16 : alignment;
    return aligned_alloc(al, (size + al - 1) / al * al);
  }
  // memory the "chip" reads: in the low 2 GiB like the kernel's DMA pool, so that the pointer is the bus address
  void* allocate_dma(uint64_t size, uint64_t) override {
    const size_t rounded = (size + 4095) / 4096 * 4096;
    void* p = mmap(nullptr, rounded, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0);
    if (p == MAP_FAILED) return nullptr;
    dma[p] = rounded;
    return p;
  }
  void deallocate(void* p) override {
    if (!p) return;
    auto it = dma.find(p);
    if (it != dma.end()) { munmap(p, it->second); dma.erase(it); } else free(p);
  }
};

typedef std::vector<uint8_t> Bytes;
static uint8_t* at(uint64_t bus) { return reinterpret_cast<uint8_t*>(bus); }
static uint32_t get32(uint64_t bus) { uint32_t v; memcpy(&v, at(bus), 4); return v; }
static uint64_t get64(uint64_t bus) { uint64_t v; memcpy(&v, at(bus), 8); return v; }
static uint16_t get16(uint64_t bus) { uint16_t v; memcpy(&v, at(bus), 2); return v; }
static void put32(uint64_t bus, uint32_t v) { memcpy(at(bus), &v, 4); }
static void put64(uint64_t bus, uint64_t v) { memcpy(at(bus), &v, 8); }
static void put16(uint64_t bus, uint16_t v) { memcpy(at(bus), &v, 2); }
static Bytes frame_of(size_t size, uint8_t seed) {
  Bytes f(size);
  for (size_t i = 0; i < size; ++i) f[i] = static_cast<uint8_t>(seed + i * 7);
  for (int i = 0; i < 6; ++i) f[i] = 0xFF;  // broadcast
  return f;
}

// A device: the register file (little-endian bytes) with a hook on every write, and the claim/release bookkeeping.
struct ModelDevice : INicDevice, INicRegisters {
  std::vector<uint8_t> file;
  uint16_t vendor, device;
  int claims = 0, releases = 0;
  bool claim_fails = false;
  std::vector<Bytes> sent;
  ModelDevice(size_t size, uint16_t v, uint16_t d) : file(size, 0), vendor(v), device(d) {}

  bool claim() override { ++claims; return !claim_fails; }
  void release() override { ++releases; }
  INicRegisters& registers() override { return *this; }
  uint16_t vendor_id() const override { return vendor; }
  uint16_t device_id() const override { return device; }
  uint32_t config_read32(uint16_t) override { return 0xFFFFFFFFu; }
  void config_write32(uint16_t, uint32_t) override {}

  uint32_t raw(uint32_t off, int width) const {
    if (off + width > file.size()) { printf("model: access past the registers at %#x\n", off); abort(); }
    uint32_t v = 0;
    memcpy(&v, &file[off], width);
    return v;
  }
  void set_raw(uint32_t off, int width, uint32_t v) {
    if (off + width > file.size()) { printf("model: access past the registers at %#x\n", off); abort(); }
    memcpy(&file[off], &v, width);
  }
  uint32_t r32(uint32_t off) const { return raw(off, 4); }
  void w32(uint32_t off, uint32_t v) { set_raw(off, 4, v); }

  virtual uint32_t on_read(uint32_t off, int width) { return raw(off, width); }
  virtual void on_write(uint32_t off, int width, uint32_t v) { set_raw(off, width, v); }
  uint8_t read8(uint32_t o) override { return static_cast<uint8_t>(on_read(o, 1)); }
  uint16_t read16(uint32_t o) override { return static_cast<uint16_t>(on_read(o, 2)); }
  uint32_t read32(uint32_t o) override { return on_read(o, 4); }
  void write8(uint32_t o, uint8_t v) override { on_write(o, 1, v); }
  void write16(uint32_t o, uint16_t v) override { on_write(o, 2, v); }
  void write32(uint32_t o, uint32_t v) override { on_write(o, 4, v); }
};

// ---- Realtek -------------------------------------------------------------------------------------------------------------------------------------------

struct RtlChipModel final : ModelDevice {
  uint32_t version;
  bool new_layout;
  uint32_t tx_index = 0, rx_index = 0;
  int resets = 0, polls = 0, dropped = 0;
  uint8_t mac[6] = {0x00, 0xE0, 0x4C, 0x68, 0x01, 0x02};
  RtlChipModel(uint16_t device_id, uint32_t xid, bool is_8125) : ModelDevice(256, 0x10EC, device_id), version(xid), new_layout(is_8125) { power_on(); }
  void power_on() {
    std::fill(file.begin(), file.end(), 0);
    memcpy(&file[0], mac, 6);
    w32(0x40, version << 20);
    w32(0xF0, 1u << 19);  // the firmware left the RXDV gate closed
  }
  uint64_t tx_ring() const { return r32(0x20) | (uint64_t{r32(0x24)} << 32); }
  uint64_t rx_ring() const { return r32(0xE4) | (uint64_t{r32(0xE8)} << 32); }
  void on_write(uint32_t off, int width, uint32_t v) override {
    if (off == 0x37 && width == 1 && (v & 0x10)) {  // reset: the configuration goes back to defaults, the identity stays
      ++resets;
      const uint32_t misc = r32(0xF0);
      const uint32_t tx = r32(0x40);
      std::vector<uint8_t> keep(file.begin(), file.begin() + 6);
      std::fill(file.begin() + 0x20, file.end(), 0);
      w32(0x40, (tx & 0xFFF00000u));
      w32(0xF0, misc);
      tx_index = rx_index = 0;
      return;
    }
    set_raw(off, width, v);
    if ((!new_layout && off == 0x38 && width == 1 && (v & 0x40)) || (new_layout && off == 0x90 && width == 2 && (v & 1))) run_transmit();
  }
  void run_transmit() {
    ++polls;
    for (int n = 0; n < 1024; ++n) {
      const uint64_t d = tx_ring() + 16ull * tx_index;
      const uint32_t opts1 = get32(d);
      if (!(opts1 & (1u << 31))) return;
      if ((opts1 & (3u << 28)) == (3u << 28)) {
        const uint32_t len = opts1 & 0x3FFF;
        sent.emplace_back(at(get64(d + 8)), at(get64(d + 8)) + len);
      }
      put32(d, opts1 & ~(1u << 31));
      tx_index = (opts1 & (1u << 30)) ? 0 : tx_index + 1;
    }
  }
  // A frame arrives (with its 4-byte CRC, which these chips keep). `flags` adds error/fragment bits instead of FS|LS.
  bool deliver(const Bytes& frame, uint32_t flags = 3u << 28) {
    const uint64_t d = rx_ring() + 16ull * rx_index;
    const uint32_t opts1 = get32(d);
    if (!(opts1 & (1u << 31))) { ++dropped; return false; }
    const uint32_t room = opts1 & 0x3FFF;
    const uint32_t len = static_cast<uint32_t>(frame.size()) + 4;
    if (len > room) { ++dropped; return false; }
    memcpy(at(get64(d + 8)), frame.data(), frame.size());
    memset(at(get64(d + 8)) + frame.size(), 0xCC, 4);
    put32(d, (opts1 & (1u << 30)) | flags | len);
    rx_index = (opts1 & (1u << 30)) ? 0 : rx_index + 1;
    return true;
  }
};

static void test_rtl_identify() {
  CHECK(rtl8169_identify(0x541u << 20) && rtl8169_identify(0x541u << 20)->family == RtlFamily::Rtl8168 && strcmp(rtl8169_identify(0x541u << 20)->name, "Realtek RTL8168H") == 0);
  CHECK(rtl8169_identify(0x540u << 20) && rtl8169_identify(0x540u << 20)->rxdv_gate);
  CHECK(rtl8169_identify(0x641u << 20) && rtl8169_identify(0x641u << 20)->family == RtlFamily::Rtl8125);
  CHECK(rtl8169_identify(0x609u << 20) && strcmp(rtl8169_identify(0x609u << 20)->name, "Realtek RTL8125A") == 0);
  CHECK(rtl8169_identify(0x64Au << 20) && rtl8169_identify(0x64Au << 20)->family == RtlFamily::Rtl8126);
  CHECK(rtl8169_identify(0x2C8u << 20) && rtl8169_identify(0x2C8u << 20)->auto_fifo && !rtl8169_identify(0x2C8u << 20)->rxdv_gate);
  CHECK(rtl8169_identify(0x2CBu << 20) && strcmp(rtl8169_identify(0x2CBu << 20)->name, "Realtek RTL8168E-VL") == 0);  // mask 0x7C8: low bits free
  CHECK(rtl8169_identify(0x448u << 20) && rtl8169_identify(0x448u << 20)->family == RtlFamily::Rtl8101);
  CHECK(rtl8169_identify(0x008u << 20) && rtl8169_identify(0x008u << 20)->family == RtlFamily::Rtl8169);
  CHECK(rtl8169_identify(0x980u << 20) && rtl8169_identify(0x980u << 20)->family == RtlFamily::Rtl8169);
  CHECK(rtl8169_identify((0x541u << 20) | 0xFFFFF) == rtl8169_identify(0x541u << 20));  // the low 20 bits are configuration, not version
  CHECK(!rtl8169_identify(0x7FFu << 20) && !rtl8169_identify(0));
  CHECK(Rtl8169NetworkInterface::supports(0x10EC, 0x8168) && Rtl8169NetworkInterface::supports(0x10EC, 0x8125) && Rtl8169NetworkInterface::supports(0x10EC, 0x3000));
  CHECK(Rtl8169NetworkInterface::supports(0x1186, 0x4300) && !Rtl8169NetworkInterface::supports(0x10EC, 0x8139) && !Rtl8169NetworkInterface::supports(0x8086, 0x8168));
}

static void test_rtl_chip(uint16_t device_id, uint32_t xid, bool is_8125) {
  TestAllocator alloc;
  UniquePointer<RtlChipModel> owned = UniquePointer<RtlChipModel>::make(alloc, device_id, xid, is_8125);
  RtlChipModel* m = owned.get();
  {
    Rtl8169NetworkInterface nic(util::move(owned), alloc);
    CHECK(nic.initialize());
    CHECK(m->claims == 1 && m->resets == 1);
    const RtlChip* chip = nic.chip();
    CHECK(chip && chip == rtl8169_identify(xid << 20));
    CHECK(nic.mac_address() == MacAddress{{0x00, 0xE0, 0x4C, 0x68, 0x01, 0x02}});
    CHECK(m->raw(0x50, 1) == 0x00);                                   // configuration registers locked again
    CHECK(m->tx_ring() != 0 && m->rx_ring() != 0 && (m->rx_ring() & 0xFF) == 0 && (m->tx_ring() & 0xFF) == 0);  // 256-byte aligned rings
    CHECK((m->raw(0x37, 1) & 0x0C) == 0x0C);                          // transmit and receive on
    const uint32_t rx_config = m->r32(0x44);
    CHECK((rx_config & 0x3F) == (0x08 | 0x02));                       // broadcast + my address, no promiscuous
    CHECK((rx_config & ~0x3Fu) == chip->rx_config);
    CHECK(((m->r32(0x40) >> 7) & 1) == (chip->auto_fifo ? 1u : 0u));
    CHECK(m->raw(0xDA, 2) == Rtl8169NetworkInterface::kBufferSize);
    if (chip->rxdv_gate) CHECK((m->r32(0xF0) & (1u << 19)) == 0);  // the gate is open, or nothing is ever received
    if (is_8125) CHECK(m->r32(0x38) == 0);                            // interrupts masked (32-bit register on the RTL8125)
    else CHECK(m->raw(0x3C, 2) == 0);

    // link: PHYstatus
    m->set_raw(0x6C, 2, 0x02 | 0x01 | 0x10);
    CHECK(nic.link_up() && nic.link_speed_mbps() == 1000 && nic.link_duplex() == LinkDuplex::Full);
    m->set_raw(0x6C, 2, 0x02 | 0x08);
    CHECK(nic.link_speed_mbps() == 100 && nic.link_duplex() == LinkDuplex::Half);
    m->set_raw(0x6C, 2, 0x02 | 0x01 | 0x10 | 0x400);
    CHECK(nic.link_speed_mbps() == (is_8125 ? 2500u : 1000u));
    m->set_raw(0x6C, 2, 0);
    CHECK(!nic.link_up() && nic.link_speed_mbps() == 0 && nic.link_duplex() == LinkDuplex::Unknown);

    // transmit: short frames padded to 60, others as they are
    const Bytes small = frame_of(42, 1), big = frame_of(1514, 2);
    CHECK(nic.transmit(ConstByteView(small.data(), small.size())));
    CHECK(nic.transmit(ConstByteView(big.data(), big.size())));
    CHECK(m->sent.size() == 2 && m->polls == 2);
    CHECK(m->sent.size() == 2 && m->sent[0].size() == 60 && memcmp(m->sent[0].data(), small.data(), 42) == 0 && m->sent[0][59] == 0);
    CHECK(m->sent.size() == 2 && m->sent[1] == big);
    const Bytes huge = frame_of(1515, 3);
    CHECK(!nic.transmit(ConstByteView(huge.data(), huge.size())) && !nic.transmit(ConstByteView()));
    // three times around the ring: the ring-end bit wraps both sides
    m->polls = 0;
    for (int round = 0; round < 3; ++round)
      for (uint32_t i = 0; i < Rtl8169NetworkInterface::kTransmitDescriptors; ++i) CHECK(nic.transmit(ConstByteView(small.data(), small.size())));
    CHECK(m->sent.size() == 2 + 3 * Rtl8169NetworkInterface::kTransmitDescriptors);

    // receive: CRC dropped, frames in order, across the end of the ring
    uint8_t buffer[2048];
    uint32_t length = 0;
    CHECK(!nic.receive(ByteView(buffer, sizeof buffer), &length));
    for (int i = 0; i < 600; ++i) {
      const Bytes f = frame_of(60 + (i % 1400), static_cast<uint8_t>(i));
      CHECK(m->deliver(f));
      CHECK(nic.receive(ByteView(buffer, sizeof buffer), &length) && length == f.size() && memcmp(buffer, f.data(), f.size()) == 0);
    }
    // a frame with an error, then one spread over two descriptors: both dropped, the next good one comes through
    CHECK(m->deliver(frame_of(100, 9), (3u << 28) | (1u << 21)));
    CHECK(m->deliver(frame_of(100, 9), 1u << 29));
    const Bytes good = frame_of(200, 10);
    CHECK(m->deliver(good));
    CHECK(nic.receive(ByteView(buffer, sizeof buffer), &length) && length == 200 && memcmp(buffer, good.data(), 200) == 0);
    CHECK(nic.statistics().frames_dropped == 2);
    // a buffer that is too small gets the beginning, the length says how long the frame was
    CHECK(m->deliver(big));
    uint8_t tiny[16];
    CHECK(nic.receive(ByteView(tiny, sizeof tiny), &length) && length == 1514 && memcmp(tiny, big.data(), 16) == 0);
    // all 256 descriptors can be filled before the driver looks; the 257th frame is lost by the chip, not the driver
    for (uint32_t i = 0; i < Rtl8169NetworkInterface::kReceiveDescriptors; ++i) CHECK(m->deliver(frame_of(64, 1)));
    CHECK(!m->deliver(frame_of(64, 1)) && m->dropped == 1);
    uint32_t got = 0;
    while (nic.receive(ByteView(buffer, sizeof buffer), &length)) ++got;
    CHECK(got == Rtl8169NetworkInterface::kReceiveDescriptors);

    // multicast: all of it through the hash filter
    CHECK(nic.accept_all_multicast(true));
    CHECK((m->r32(0x44) & 0x04) && m->r32(0x08) == 0xFFFFFFFFu && m->r32(0x0C) == 0xFFFFFFFFu);
    CHECK(!nic.accept_all_multicast(false));
    CHECK(!(m->r32(0x44) & 0x04) && m->r32(0x08) == 0 && m->r32(0x0C) == 0);

    nic.shutdown();
    CHECK(m->releases == 1 && m->resets >= 2);
    CHECK(!nic.transmit(ConstByteView(small.data(), small.size())) && !nic.receive(ByteView(buffer, sizeof buffer), &length) && !nic.link_up());
    nic.shutdown();  // twice is fine
    CHECK(m->releases == 1);
  }
  CHECK(alloc.dma.empty());
}

static void test_rtl_failures() {
  TestAllocator alloc;
  {
    UniquePointer<RtlChipModel> owned = UniquePointer<RtlChipModel>::make(alloc, 0x8168, 0x7FF, false);  // a version r8169 does not know
    RtlChipModel* m = owned.get();
    Rtl8169NetworkInterface nic(util::move(owned), alloc);
    CHECK(!nic.initialize());
    nic.shutdown();
    CHECK(m->claims == 1 && m->releases == 1 && m->resets == 0);
  }
  {
    UniquePointer<RtlChipModel> owned = UniquePointer<RtlChipModel>::make(alloc, 0x8168, 0x541, false);
    owned->claim_fails = true;
    RtlChipModel* m = owned.get();
    Rtl8169NetworkInterface nic(util::move(owned), alloc);
    CHECK(!nic.initialize());
    nic.shutdown();
    CHECK(m->releases == 0);  // never claimed
  }
  {
    UniquePointer<RtlChipModel> owned = UniquePointer<RtlChipModel>::make(alloc, 0x8168, 0x541, false);
    std::fill(owned->file.begin(), owned->file.end(), 0xFF);  // fell off the bus
    Rtl8169NetworkInterface nic(util::move(owned), alloc);
    CHECK(!nic.initialize());
  }
  CHECK(alloc.dma.empty());
}

// A device that writes whatever it likes into the receive descriptors it was given: the driver must never read past a buffer or report more than it holds.
static void test_rtl_hostile() {
  TestAllocator alloc;
  UniquePointer<RtlChipModel> owned = UniquePointer<RtlChipModel>::make(alloc, 0x8125, 0x641, true);
  RtlChipModel* m = owned.get();
  Rtl8169NetworkInterface nic(util::move(owned), alloc);
  CHECK(nic.initialize());
  std::mt19937 rng(7);
  uint8_t buffer[1600];
  for (int i = 0; i < 4000; ++i) {
    const uint64_t d = m->rx_ring() + 16ull * m->rx_index;
    if (get32(d) & (1u << 31)) {
      const uint32_t opts1 = (rng() & ~(1u << 31)) | (get32(d) & (1u << 30));
      put32(d, opts1);
      m->rx_index = (opts1 & (1u << 30)) ? 0 : (m->rx_index + 1) % Rtl8169NetworkInterface::kReceiveDescriptors;
    }
    uint32_t length = 0;
    if (nic.receive(ByteView(buffer, sizeof buffer), &length)) CHECK(length <= Rtl8169NetworkInterface::kBufferSize);
  }
}

// ---- Intel ----------------------------------------------------------------------------------------------------------------------------------------------

constexpr uint32_t kCtrl = 0x0, kStatus = 0x8, kEecd = 0x10, kCtrlExt = 0x18, kMdic = 0x20, kRctl = 0x100, kTctl = 0x400, kExtcnf = 0xF00, kIosfpc = 0xF28;
constexpr uint32_t kRal = 0x5400, kRah = 0x5404, kFwsm = 0x5B54, kTarc0 = 0x3840, kEemngctl = 0x1010;

// What the e1000e and igb models share: the reset with the NVM reload, the PHY behind MDIC, the station address.
struct IntelModel : ModelDevice {
  uint8_t nvm_mac[6] = {0x00, 0x1B, 0x21, 0xAA, 0xBB, 0xCC};
  bool nvm_reload = true;      // the reset reloads RAL/RAH from the NVM
  bool chipset = false;        // reports LAN_INIT_DONE
  uint32_t reset_bit = 1u << 26;
  uint32_t phy_address = 1;
  bool phy_asleep = false;
  uint16_t phy[32] = {};
  int resets = 0, lanphypc_toggles = 0, phy_writes = 0;
  IntelModel(uint16_t device_id) : ModelDevice(0x10000, 0x8086, device_id) {
    phy[0] = 0x1140;  // autonegotiation on, full duplex
    phy[2] = 0x0141;
    load_mac();
    w32(kStatus, 0x2 | 0x1 | (2u << 6) | (1u << 19));  // link up, full duplex, 1000, bus master enabled
    w32(kFwsm, 1u << 6);
  }
  void load_mac() {
    w32(kRal, nvm_mac[0] | (nvm_mac[1] << 8) | (nvm_mac[2] << 16) | (uint32_t{nvm_mac[3]} << 24));
    w32(kRah, nvm_mac[4] | (nvm_mac[5] << 8) | (1u << 31));
  }
  virtual void on_reset() {}
  void on_write(uint32_t off, int width, uint32_t v) override {
    if (width != 4) { printf("model: %d-byte access to Intel register %#x\n", width, off); abort(); }
    if (off == kCtrl) {
      const uint32_t old = r32(kCtrl);
      if ((old & (1u << 16)) && !(v & (1u << 16)) && !(old & (1u << 17))) {  // LANPHYPC override released while the value was low: PHY power cycle
        ++lanphypc_toggles;
        phy_asleep = false;
        w32(kCtrlExt, r32(kCtrlExt) | (1u << 2));
      }
      if (v & reset_bit) {
        ++resets;
        w32(kCtrl, v & ~reset_bit & ~(1u << 2));
        w32(kRal, 0);
        w32(kRah, 0);
        if (nvm_reload) load_mac();
        w32(kEecd, r32(kEecd) | (1u << 9));
        w32(kEemngctl, 1u << 18);
        if (chipset) w32(kStatus, r32(kStatus) | (1u << 9));
        w32(kStatus, r32(kStatus) | (1u << 19));
        on_reset();
        return;
      }
      w32(kCtrl, v);
      if (v & (1u << 2)) w32(kStatus, r32(kStatus) & ~(1u << 19));  // GIO master disabled: nothing outstanding
      return;
    }
    if (off == kStatus) {  // only LAN_INIT_DONE can be cleared
      w32(kStatus, (r32(kStatus) & ~(1u << 9)) | (v & r32(kStatus) & (1u << 9)));
      return;
    }
    if (off == kMdic) {
      const uint32_t reg = (v >> 16) & 31, address = (v >> 21) & 31, op = (v >> 26) & 3;
      if (!(r32(kExtcnf) & (1u << 5)) && chipset) { printf("model: MDIC without the software flag\n"); ++g_failed; }
      if (phy_asleep || address != phy_address) { w32(kMdic, v | (1u << 28) | (1u << 30)); return; }
      if (op == 2) w32(kMdic, (v & 0xFFFF0000u) | phy[reg] | (1u << 28));
      else { phy[reg] = static_cast<uint16_t>(v); ++phy_writes; w32(kMdic, v | (1u << 28)); }
      return;
    }
    set_raw(off, 4, v);
    after_write(off, v);
  }
  virtual void after_write(uint32_t, uint32_t) {}
};

struct E1000eChip final : IntelModel {
  uint32_t rx_head = 0, tx_head = 0;
  int dropped = 0;
  E1000eChip(uint16_t device_id, bool is_chipset) : IntelModel(device_id) {
    chipset = is_chipset;
    phy_address = is_chipset && device_id != 0x10BD ? 2 : 1;
  }
  uint64_t ring(uint32_t base) const { return r32(base) | (uint64_t{r32(base + 4)} << 32); }
  uint32_t count(uint32_t len_reg) const { return r32(len_reg) / 16; }
  void on_reset() override { rx_head = tx_head = 0; w32(0x2810, 0); w32(0x3810, 0); }
  void after_write(uint32_t off, uint32_t v) override {
    if (off == 0x3818 && (r32(kTctl) & 2)) {  // TDT: send up to the tail
      const uint32_t n = count(0x3808);
      while (tx_head != v % n) {
        const uint64_t d = ring(0x3800) + 16ull * tx_head;
        const uint16_t len = get16(d + 8);
        const uint8_t cmd = at(d + 11)[0];
        if (cmd & 1) sent.emplace_back(at(get64(d)), at(get64(d)) + len);
        at(d + 12)[0] |= 1;  // DD
        tx_head = (tx_head + 1) % n;
      }
      w32(0x3810, tx_head);
    }
  }
  bool deliver(const Bytes& frame, uint8_t errors = 0, uint8_t status = 3) {
    if (!(r32(kRctl) & 2)) { ++dropped; return false; }
    const uint32_t n = count(0x2808);
    if (rx_head == r32(0x2818) % n) { ++dropped; return false; }  // no descriptor
    const uint64_t d = ring(0x2800) + 16ull * rx_head;
    memcpy(at(get64(d)), frame.data(), frame.size());
    put16(d + 8, static_cast<uint16_t>(frame.size()));
    at(d + 12)[0] = status;
    at(d + 13)[0] = errors;
    rx_head = (rx_head + 1) % n;
    w32(0x2810, rx_head);
    return true;
  }
};

struct IgbChip final : IntelModel {
  uint32_t rx_head = 0, tx_head = 0;
  int dropped = 0;
  bool bad_tx_descriptor = false;
  IgbChip(uint16_t device_id, bool igc) : IntelModel(device_id) {
    if (igc) reset_bit = 1u << 29;
  }
  uint64_t ring(uint32_t base) const { return r32(base) | (uint64_t{r32(base + 4)} << 32); }
  void on_reset() override { rx_head = tx_head = 0; w32(0xC028, 0); w32(0xE028, 0); w32(0xC010, 0); w32(0xE010, 0); }
  void after_write(uint32_t off, uint32_t v) override {
    if (off == 0xE018 && (r32(kTctl) & 2) && (r32(0xE028) & (1u << 25))) {
      const uint32_t n = r32(0xE008) / 16;
      while (tx_head != v % n) {
        const uint64_t d = ring(0xE000) + 16ull * tx_head;
        const uint32_t ctl = get32(d + 8), olinfo = get32(d + 12);
        // a data descriptor (DEXT, DTYP 3) with IFCS and EOP, PAYLEN matching the buffer length
        if (((ctl >> 20) & 0xF) != 3 || !(ctl & (1u << 29)) || !(ctl & (1u << 24)) || (olinfo >> 14) != (ctl & 0xFFFF)) bad_tx_descriptor = true;
        sent.emplace_back(at(get64(d)), at(get64(d)) + (ctl & 0xFFFF));
        put64(d, 0);       // write-back: the address is gone
        put32(d + 8, 0);
        put32(d + 12, 1);  // DD
        tx_head = (tx_head + 1) % n;
      }
      w32(0xE010, tx_head);
    }
  }
  bool deliver(const Bytes& frame, uint32_t status = 3) {
    if (!(r32(kRctl) & 2) || !(r32(0xC028) & (1u << 25))) { ++dropped; return false; }
    if ((r32(0xC00C) & 0x7F) != 2 || ((r32(0xC00C) >> 25) & 7) != 1) { ++dropped; return false; }  // 2 KiB, advanced one buffer
    const uint32_t n = r32(0xC008) / 16;
    if (rx_head == r32(0xC018) % n) { ++dropped; return false; }
    const uint64_t d = ring(0xC000) + 16ull * rx_head;
    const uint64_t buffer = get64(d);
    if (buffer == 0 || get64(d + 8) != 0) { printf("model: receive descriptor not in read format\n"); ++g_failed; ++dropped; return false; }
    memcpy(at(buffer), frame.data(), frame.size());
    put64(d, 0x1234567800000000ull);  // write-back: RSS/packet info
    put64(d + 8, (uint64_t{frame.size()} << 32) | status);
    rx_head = (rx_head + 1) % n;
    w32(0xC010, rx_head);
    return true;
  }
};

template <class Nic, class Model>
static void exercise_intel(Nic& nic, Model* m) {
  const Bytes small = frame_of(42, 1), big = frame_of(1514, 2);
  CHECK(nic.transmit(ConstByteView(small.data(), small.size())));
  CHECK(nic.transmit(ConstByteView(big.data(), big.size())));
  CHECK(m->sent.size() == 2 && m->sent[0].size() == 60 && memcmp(m->sent[0].data(), small.data(), 42) == 0);
  CHECK(m->sent.size() == 2 && m->sent[1] == big);
  for (int i = 0; i < 300; ++i) CHECK(nic.transmit(ConstByteView(big.data(), 100 + i)));
  CHECK(m->sent.size() == 302 && m->sent.back().size() == 399);

  uint8_t buffer[2048];
  uint32_t length = 0;
  CHECK(!nic.receive(ByteView(buffer, sizeof buffer), &length));
  for (int i = 0; i < 700; ++i) {
    const Bytes f = frame_of(60 + (i * 13) % 1450, static_cast<uint8_t>(i));
    CHECK(m->deliver(f));
    CHECK(nic.receive(ByteView(buffer, sizeof buffer), &length) && length == f.size() && memcmp(buffer, f.data(), f.size()) == 0);
  }
  // the chip has 255 descriptors to fill (one stays between tail and head)
  uint32_t delivered = 0;
  while (m->deliver(frame_of(64, 3))) ++delivered;
  CHECK(delivered == 255);
  uint32_t got = 0;
  while (nic.receive(ByteView(buffer, sizeof buffer), &length)) ++got;
  CHECK(got == 255);
  CHECK(m->deliver(frame_of(64, 4)));  // and there is room again
  CHECK(nic.receive(ByteView(buffer, sizeof buffer), &length) && length == 64);

  CHECK(nic.accept_all_multicast(true) && (m->r32(kRctl) & (1u << 4)));
  CHECK(!nic.accept_all_multicast(false) && !(m->r32(kRctl) & (1u << 4)));
  CHECK((m->r32(kRctl) & ((1u << 1) | (1u << 15) | (1u << 26))) == ((1u << 1) | (1u << 15) | (1u << 26)));  // on, broadcast, CRC stripped

  m->w32(kStatus, 0x2 | 0x1 | (2u << 6));
  CHECK(nic.link_up() && nic.link_speed_mbps() == 1000 && nic.link_duplex() == LinkDuplex::Full);
  m->w32(kStatus, 0x2 | (1u << 6));
  CHECK(nic.link_speed_mbps() == 100 && nic.link_duplex() == LinkDuplex::Half);
  m->w32(kStatus, 0);
  CHECK(!nic.link_up() && nic.link_speed_mbps() == 0);
  CHECK(m->r32(kCtrlExt) & (1u << 28));  // driver loaded
}

static void test_e1000e_models() {
  CHECK(e1000e_find_model(0x10D3) && e1000e_find_model(0x10D3)->kind == E1000eKind::Mac8257x && strcmp(e1000e_find_model(0x10D3)->name, "Intel 82574L") == 0);
  CHECK(e1000e_find_model(0x15BC) && e1000e_find_model(0x15BC)->kind == E1000eKind::PchSpt && strcmp(e1000e_find_model(0x15BC)->name, "Intel I219-V") == 0);
  CHECK(e1000e_find_model(0x153B) && e1000e_find_model(0x153B)->kind == E1000eKind::PchLpt);
  CHECK(e1000e_find_model(0x1502) && e1000e_find_model(0x1502)->kind == E1000eKind::Pch2);
  CHECK(e1000e_find_model(0x10DE) && e1000e_find_model(0x10DE)->kind == E1000eKind::Ich);
  CHECK(!e1000e_find_model(0x100E) && !e1000e_find_model(0x1533) && !e1000e_find_model(0x15F3));  // e1000, igb and igc parts are not e1000e's
  CHECK(E1000eNetworkInterface::supports(0x8086, 0x550B) && !E1000eNetworkInterface::supports(0x10EC, 0x10D3));
  CHECK(igb_find_model(0x1533) && igb_find_model(0x1533)->kind == IgbKind::I210 && igb_find_model(0x1539)->kind == IgbKind::I211);
  CHECK(igb_find_model(0x15F3) && igb_find_model(0x15F3)->kind == IgbKind::I225 && strcmp(igb_find_model(0x125C)->name, "Intel I226-V") == 0);
  CHECK(igb_find_model(0x10C9) && !igb_find_model(0x10D3) && !igb_find_model(0x100E));
  // no device id is claimed by two drivers
  for (uint32_t id = 0; id < 0x10000; ++id) {
    const int owners = (e1000e_find_model(id) ? 1 : 0) + (igb_find_model(id) ? 1 : 0) + (id == 0x100E || id == 0x100C || id == 0x100F ? 1 : 0);
    CHECK(owners <= 1);
  }
}

static void test_e1000e_82574() {
  TestAllocator alloc;
  UniquePointer<E1000eChip> owned = UniquePointer<E1000eChip>::make(alloc, 0x10D3, false);
  E1000eChip* m = owned.get();
  {
    E1000eNetworkInterface nic(util::move(owned), alloc);
    CHECK(nic.initialize());
    CHECK(strcmp(nic.name(), "Intel 82574L") == 0 && m->resets == 1 && m->lanphypc_toggles == 0 && !nic.phy_was_woken());
    CHECK(nic.mac_address() == MacAddress{{0x00, 0x1B, 0x21, 0xAA, 0xBB, 0xCC}});
    CHECK((m->r32(kCtrl) & (1u << 6)) && !(m->r32(kCtrl) & (1u << 2)));  // set link up, bus mastering back on
    CHECK(m->r32(kTarc0) == 0 && m->r32(kIosfpc) == 0);  // the I219 workaround is the I219's
    CHECK(m->phy_writes == 0);                             // autonegotiation was on already
    exercise_intel(nic, m);
    nic.shutdown();
    CHECK(m->releases == 1 && m->resets == 2 && !(m->r32(kCtrlExt) & (1u << 28)));
  }
  CHECK(alloc.dma.empty());
}

static void test_e1000e_i219() {
  TestAllocator alloc;
  UniquePointer<E1000eChip> owned = UniquePointer<E1000eChip>::make(alloc, 0x15BC, true);
  E1000eChip* m = owned.get();
  m->phy_asleep = true;   // the firmware left the PHY powered down (ULP)
  m->nvm_reload = false;  // and the reset does not bring the address back
  m->phy[0] = 0x0800;     // power down, no autonegotiation
  m->w32(kCtrlExt, 1u << 11);  // forced to SMBus
  {
    E1000eNetworkInterface nic(util::move(owned), alloc);
    CHECK(nic.initialize());
    CHECK(nic.phy_was_woken() && m->lanphypc_toggles == 1 && !m->phy_asleep);
    CHECK(!(m->r32(kCtrlExt) & (1u << 11)));
    CHECK((m->phy[0] & (1u << 12)) && !(m->phy[0] & (1u << 11)));  // autonegotiation on, powered up
    CHECK(nic.mac_address() == MacAddress{{0x00, 0x1B, 0x21, 0xAA, 0xBB, 0xCC}});  // taken from before the reset...
    CHECK(m->r32(kRah) == (0xBBu | (0xCCu << 8) | (1u << 31)));                 // ...and programmed back
    CHECK((m->r32(kTarc0) >> 28) == 2 && (m->r32(kIosfpc) & 0x10000));
    CHECK(!(m->r32(kStatus) & (1u << 9)));  // LAN_INIT_DONE seen and cleared
    CHECK(!(m->r32(kExtcnf) & (1u << 5)));  // MDIO ownership given back
    exercise_intel(nic, m);
  }
  CHECK(alloc.dma.empty());
  // the ME blocks PHY resets: no toggle
  UniquePointer<E1000eChip> blocked = UniquePointer<E1000eChip>::make(alloc, 0x15BC, true);
  E1000eChip* b = blocked.get();
  b->phy_asleep = true;
  b->w32(kFwsm, 0);
  {
    E1000eNetworkInterface nic(util::move(blocked), alloc);
    CHECK(nic.initialize() && !nic.phy_was_woken() && b->lanphypc_toggles == 0);
  }
  // no address anywhere: refused
  UniquePointer<E1000eChip> nameless = UniquePointer<E1000eChip>::make(alloc, 0x15BC, true);
  nameless->w32(kRah, 0);
  nameless->nvm_reload = false;
  E1000eChip* n = nameless.get();
  {
    E1000eNetworkInterface nic(util::move(nameless), alloc);
    CHECK(!nic.initialize());
    nic.shutdown();
    CHECK(n->releases == 1);
  }
  CHECK(alloc.dma.empty());
}

static void test_igb(uint16_t device_id, bool igc) {
  TestAllocator alloc;
  UniquePointer<IgbChip> owned = UniquePointer<IgbChip>::make(alloc, device_id, igc);
  IgbChip* m = owned.get();
  {
    IgbNetworkInterface nic(util::move(owned), alloc);
    CHECK(nic.initialize());
    CHECK(m->resets == 1 && nic.model() && nic.model()->device_id == device_id);
    CHECK(nic.mac_address() == MacAddress{{0x00, 0x1B, 0x21, 0xAA, 0xBB, 0xCC}});
    CHECK((m->r32(0xC028) & (1u << 25)) && (m->r32(0xE028) & (1u << 25)));
    CHECK(((m->r32(0xE028) >> 16) & 0x3F) == 1);  // write-back threshold 1: the polled driver sees every descriptor at once
    CHECK(m->r32(0xC018) == IgbNetworkInterface::kReceiveDescriptors - 1);
    CHECK(nic.autonegotiation_restarted() && (m->phy[0] & ((1u << 12) | (1u << 9))) == ((1u << 12) | (1u << 9)));
    exercise_intel(nic, m);
    CHECK(!m->bad_tx_descriptor);
    if (igc) {
      m->w32(kStatus, 0x2 | 0x1 | (2u << 6) | (1u << 22));
      CHECK(nic.link_speed_mbps() == 2500);
    } else {
      m->w32(kStatus, 0x2 | 0x1 | (2u << 6) | (1u << 22));
      CHECK(nic.link_speed_mbps() == 1000);  // bit 22 is something else before the I225
    }
    nic.shutdown();
    CHECK(m->releases == 1 && m->resets == 2);
  }
  CHECK(alloc.dma.empty());
}

// The igb model with a receive descriptor whose write-back status and length are random.
static void test_igb_hostile() {
  TestAllocator alloc;
  UniquePointer<IgbChip> owned = UniquePointer<IgbChip>::make(alloc, 0x15F3, true);
  IgbChip* m = owned.get();
  IgbNetworkInterface nic(util::move(owned), alloc);
  CHECK(nic.initialize());
  std::mt19937_64 rng(11);
  uint8_t buffer[100];
  for (int i = 0; i < 3000; ++i) {
    const uint32_t n = m->r32(0xC008) / 16;
    if (m->rx_head != m->r32(0xC018) % n) {
      const uint64_t d = m->ring(0xC000) + 16ull * m->rx_head;
      put64(d, rng());
      put64(d + 8, rng() | 1);  // done, with anything else
      m->rx_head = (m->rx_head + 1) % n;
    }
    uint32_t length = 0;
    if (nic.receive(ByteView(buffer, sizeof buffer), &length)) CHECK(length <= IgbNetworkInterface::kBufferSize);
  }
}

int main() {
  test_rtl_identify();
  test_rtl_chip(0x8168, 0x541, false);  // RTL8168H
  test_rtl_chip(0x8168, 0x2C8, false);  // RTL8168E-VL
  test_rtl_chip(0x8168, 0x3C8, false);  // RTL8168C
  test_rtl_chip(0x8136, 0x448, false);  // RTL8106E
  test_rtl_chip(0x8169, 0x008, false);  // RTL8169
  test_rtl_chip(0x8125, 0x641, true);   // RTL8125B
  test_rtl_chip(0x8126, 0x649, true);   // RTL8126A
  test_rtl_failures();
  test_rtl_hostile();
  test_e1000e_models();
  test_e1000e_82574();
  test_e1000e_i219();
  test_igb(0x10C9, false);  // 82576 (QEMU's igb)
  test_igb(0x1533, false);  // I210
  test_igb(0x1539, false);  // I211
  test_igb(0x15F3, true);   // I225-V
  test_igb(0x125C, true);   // I226-V
  test_igb_hostile();
  if (g_failed) {
    printf("%d check(s) failed\n", g_failed);
    return 1;
  }
  printf("PciNicTest: all passed\n");
  return 0;
}
