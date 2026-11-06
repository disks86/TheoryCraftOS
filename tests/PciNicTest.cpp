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

// What a checksum engine does: the ones'-complement sum from `start` to the end (the field holds the pseudo-header seed), complemented, into
// `field`. (Written independently of the driver's software fallback.)
static void hardware_checksum(uint8_t* frame, uint32_t length, uint32_t start, uint32_t field) {
  if (start >= length || field + 2 > length) { printf("model: checksum request outside the frame\n"); ++g_failed; return; }
  uint64_t sum = 0;
  for (uint32_t i = start; i < length; i += 2) sum += (uint32_t{frame[i]} << 8) | (i + 1 < length ? frame[i + 1] : 0);
  while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
  frame[field] = static_cast<uint8_t>(~sum >> 8);
  frame[field + 1] = static_cast<uint8_t>(~sum);
}

// True if an IPv4 TCP/UDP frame's transport checksum is right (UDP may say 0: none).
static bool transport_checksum_ok(const Bytes& f) {
  if (f.size() < 34 || f[12] != 0x08 || f[13] != 0x00) return false;
  const uint32_t ihl = (f[14] & 0x0F) * 4u, total = (f[16] << 8) | f[17], protocol = f[23];
  if ((protocol != 6 && protocol != 17) || 14 + total > f.size()) return false;
  const uint32_t l4 = total - ihl;
  uint32_t sum = protocol + l4;
  for (int i = 26; i < 34; i += 2) sum += (f[i] << 8) | f[i + 1];
  for (uint32_t i = 0; i < l4; i += 2) sum += (f[14 + ihl + i] << 8) | (i + 1 < l4 ? f[14 + ihl + i + 1] : 0);
  while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
  return sum == 0xFFFF;
}

// An IPv4 frame with a TCP (protocol 6) or UDP (17) payload of `payload` bytes after a header of `ip_header` bytes, the transport checksum
// field seeded with the pseudo-header sum the way the stack does it; `request` says where the checksum goes.
static Bytes transport_frame(uint8_t protocol, uint32_t payload, TransmitChecksum* request, uint32_t ip_header = 20) {
  const uint32_t l4_header = protocol == 6 ? 20 : 8, l4 = l4_header + payload;
  Bytes f(14 + ip_header + l4, 0);
  for (int i = 0; i < 6; ++i) { f[i] = 0x02; f[6 + i] = 0x04; }
  f[12] = 0x08;
  f[14] = static_cast<uint8_t>(0x40 | (ip_header / 4));
  f[16] = static_cast<uint8_t>((ip_header + l4) >> 8);
  f[17] = static_cast<uint8_t>(ip_header + l4);
  f[22] = 64;
  f[23] = protocol;
  const uint8_t addresses[8] = {10, 0, 2, 15, 93, 184, 216, 34};
  memcpy(&f[26], addresses, 8);
  uint8_t* t = &f[14 + ip_header];
  t[0] = 0xC0; t[1] = 0x01; t[2] = 0x00; t[3] = 0x50;
  if (protocol == 17) { t[4] = static_cast<uint8_t>(l4 >> 8); t[5] = static_cast<uint8_t>(l4); }
  else { t[12] = 0x50; t[13] = 0x18; }
  for (uint32_t i = 0; i < payload; ++i) t[l4_header + i] = static_cast<uint8_t>(i * 37 + 11);
  const uint32_t field = 14 + ip_header + (protocol == 6 ? 16 : 6);
  uint32_t seed = protocol + l4;
  for (int i = 26; i < 34; i += 2) seed += (f[i] << 8) | f[i + 1];
  while (seed >> 16) seed = (seed & 0xFFFF) + (seed >> 16);
  f[field] = static_cast<uint8_t>(seed >> 8);
  f[field + 1] = static_cast<uint8_t>(seed);
  *request = TransmitChecksum{static_cast<uint16_t>(14 + ip_header), static_cast<uint16_t>(field)};
  return f;
}

// A device: the register file (little-endian bytes) with a hook on every write, and the claim/release bookkeeping.
struct ModelDevice : INicDevice, INicRegisters {
  std::vector<uint8_t> file;
  uint16_t vendor, device;
  int claims = 0, releases = 0;
  bool claim_fails = false;
  std::vector<Bytes> sent;
  // The interrupt route: what attach_interrupt() was given. `msi_lost`: the route is set up but the messages never arrive.
  InterruptHandler handler = nullptr;
  void* handler_context = nullptr;
  bool attach_refused = false, msi_lost = false;
  int attaches = 0, detaches = 0, raised = 0;
  ModelDevice(size_t size, uint16_t v, uint16_t d) : file(size, 0), vendor(v), device(d) {}

  bool claim() override { ++claims; return !claim_fails; }
  void release() override { ++releases; }
  INicRegisters& registers() override { return *this; }
  uint16_t vendor_id() const override { return vendor; }
  uint16_t device_id() const override { return device; }
  uint32_t config_read32(uint16_t) override { return 0xFFFFFFFFu; }
  void config_write32(uint16_t, uint32_t) override {}
  bool attach_interrupt(InterruptHandler h, void* context) override {
    if (attach_refused) return false;
    ++attaches;
    handler = h;
    handler_context = context;
    return true;
  }
  void detach_interrupt() override {
    if (handler) ++detaches;
    handler = nullptr;
  }
  void raise() {
    if (!handler || msi_lost) return;
    ++raised;
    handler(handler_context);
  }

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
  uint32_t status_reg() const { return new_layout ? 0x3C : 0x3E; }
  uint32_t mask_reg() const { return new_layout ? 0x38 : 0x3C; }
  int status_width() const { return new_layout ? 4 : 2; }
  // An event: its status bit is set, and an interrupt goes out if the mask lets it.
  void signal(uint32_t bits) {
    set_raw(status_reg(), status_width(), raw(status_reg(), status_width()) | bits);
    if (raw(status_reg(), status_width()) & raw(mask_reg(), status_width())) raise();
  }
  int checksummed = 0;
  void on_write(uint32_t off, int width, uint32_t v) override {
    if (off == status_reg() && width == status_width()) {  // write 1 to clear
      set_raw(off, width, raw(off, width) & ~v);
      return;
    }
    if (!new_layout && off == 0x38 && width == 1 && (v & 0x01)) {  // forced software interrupt
      set_raw(off, width, v & ~1u);
      signal(0x100);
      if (!(v & 0x40)) return;
    }
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
        const uint32_t len = opts1 & 0x3FFF, opts2 = get32(d + 4);
        uint8_t* data = at(get64(d + 8));
        // checksums: the RTL8169's TD0 bits (the chip finds the transport header itself), or TD1 with the header's offset
        const bool tcp0 = opts1 & (1u << 16), udp0 = opts1 & (1u << 17), tcp1 = opts2 & (1u << 30), udp1 = opts2 & (1u << 31);
        if (tcp0 || udp0 || tcp1 || udp1) {
          const bool v2 = rtl8169_identify(version << 20)->family != RtlFamily::Rtl8169;
          if ((tcp0 || udp0) == v2) { printf("model: checksum bits of the wrong descriptor format\n"); ++g_failed; }
          const uint32_t start = (tcp1 || udp1) ? (opts2 >> 18) & 0x3FF : 14 + (data[14] & 0x0F) * 4u;
          hardware_checksum(data, len, start, start + ((tcp0 || tcp1) ? 16 : 6));
          ++checksummed;
        }
        sent.emplace_back(data, data + len);
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
    signal(0x0001);  // RxOK
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
  // ICR collects causes, IMS (set through IMS, cleared through IMC) says which interrupt; reading ICR clears it.
  uint32_t ims = 0;
  void signal(uint32_t causes) {
    w32(0xC0, r32(0xC0) | causes);
    if (r32(0xC0) & ims) raise();
  }
  uint32_t on_read(uint32_t off, int width) override {
    if (off == 0xC0) {
      const uint32_t v = r32(0xC0);
      w32(0xC0, 0);
      return v;
    }
    return raw(off, width);
  }
  void on_write(uint32_t off, int width, uint32_t v) override {
    if (width != 4) { printf("model: %d-byte access to Intel register %#x\n", width, off); abort(); }
    if (off == 0xD0) { ims |= v; w32(0xD0, ims); return; }
    if (off == 0xD8) { ims &= ~v; w32(0xD0, ims); return; }
    if (off == 0xC8) { signal(v); return; }
    if (off == kCtrl) {
      const uint32_t old = r32(kCtrl);
      if ((old & (1u << 16)) && !(v & (1u << 16)) && !(old & (1u << 17))) {  // LANPHYPC override released while the value was low: PHY power cycle
        ++lanphypc_toggles;
        phy_asleep = false;
        w32(kCtrlExt, r32(kCtrlExt) | (1u << 2));
      }
      if (v & reset_bit) {
        ++resets;
        ims = 0;
        w32(0xD0, 0);
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
  int checksummed = 0;
  bool have_context = false;
  uint32_t context_start = 0, context_field = 0;
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
        const uint8_t cmd = at(d + 11)[0], type = at(d + 10)[0] >> 4;
        if (cmd & 4) { printf("model: legacy checksum insertion (QEMU ignores it)\n"); ++g_failed; }
        if ((cmd & 0x20) && type == 0) {  // a TCP/IP context descriptor: TUCSS, TUCSO; not written back
          context_start = at(d + 4)[0];
          context_field = at(d + 5)[0];
          have_context = true;
          tx_head = (tx_head + 1) % n;
          continue;
        }
        if ((cmd & 0x20) && type == 1 && (at(d + 13)[0] & 2)) {  // an extended data descriptor with TXSM
          if (!have_context) { printf("model: TXSM without a context\n"); ++g_failed; }
          else hardware_checksum(at(get64(d)), len, context_start, context_field);
          ++checksummed;
        }
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
    signal(1u << 7);  // RXT0
    return true;
  }
};

struct IgbChip final : IntelModel {
  uint32_t rx_head = 0, tx_head = 0;
  int dropped = 0;
  bool bad_tx_descriptor = false;
  bool have_context = false, context_tcp = false;
  uint32_t context_start = 0;
  int checksummed = 0;
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
        if (((ctl >> 20) & 0xF) == 2 && (ctl & (1u << 29))) {  // a context descriptor: remembered, not written back
          const uint32_t lens = get32(d);
          context_start = ((lens >> 9) & 0x7F) + (lens & 0x1FF);
          context_tcp = ctl & (1u << 11);
          have_context = true;
          tx_head = (tx_head + 1) % n;
          continue;
        }
        // a data descriptor (DEXT, DTYP 3) with IFCS and EOP, PAYLEN matching the buffer length
        if (((ctl >> 20) & 0xF) != 3 || !(ctl & (1u << 29)) || !(ctl & (1u << 24)) || (olinfo >> 14) != (ctl & 0xFFFF)) bad_tx_descriptor = true;
        if (olinfo & (1u << 9)) {  // TXSM: the transport checksum, as the context describes
          if (!have_context) bad_tx_descriptor = true;
          else hardware_checksum(at(get64(d)), ctl & 0xFFFF, context_start, context_start + (context_tcp ? 16 : 6));
          ++checksummed;
        }
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
    signal(1u << 7);  // RXT0
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

// ---- interrupts and checksum offload ----------------------------------------------------------------------------------------------------------------------

static int g_wakes = 0;
static void count_wake(void*) { ++g_wakes; }

// Sends TCP and UDP frames of several sizes with a checksum request through `nic`; the model finishes them like the hardware, and every one
// must come out right. Short ones and requests the hardware cannot do are finished by the driver.
template <class Nic, class Model>
static void check_transmit_checksums(Nic& nic, Model* m, bool short_ones_in_software) {
  CHECK(nic.offloads().transmit_checksums && nic.offloads().receive_checksums);
  for (uint8_t protocol : {uint8_t{6}, uint8_t{17}}) {
    for (uint32_t payload : {0u, 1u, 9u, 100u, 1001u, 1460u}) {
      for (uint32_t ip_header : {20u, 24u}) {
        TransmitChecksum request;
        const Bytes f = transport_frame(protocol, payload, &request, ip_header);
        if (f.size() > kEthernetMaxFrame) continue;
        const size_t before = m->sent.size();
        const int hardware_before = m->checksummed;
        CHECK(nic.transmit_frame(ConstByteView(f.data(), f.size()), request));
        CHECK(m->sent.size() == before + 1);
        if (m->sent.size() != before + 1) continue;
        Bytes out = m->sent.back();
        out.resize(f.size());  // (the padding)
        CHECK(transport_checksum_ok(out));
        const bool by_hardware = m->checksummed == hardware_before + 1;
        if (short_ones_in_software && f.size() < kEthernetMinFrame) CHECK(!by_hardware);
        else CHECK(by_hardware);
        // everything but the checksum field went out as given
        Bytes expect = f;
        expect[request.field] = out[request.field];
        expect[request.field + 1] = out[request.field + 1];
        CHECK(out == expect);
      }
    }
  }
  // a request outside the frame is refused, not sent
  const Bytes small = frame_of(60, 5);
  const size_t before = m->sent.size();
  CHECK(!nic.transmit_frame(ConstByteView(small.data(), small.size()), TransmitChecksum{70, 76}) && m->sent.size() == before);
  CHECK(!nic.transmit_frame(ConstByteView(small.data(), small.size()), TransmitChecksum{40, 59}) && m->sent.size() == before);
}

static void test_rtl_interrupts_and_checksums(uint16_t device_id, uint32_t xid, bool is_8125) {
  TestAllocator alloc;
  UniquePointer<RtlChipModel> owned = UniquePointer<RtlChipModel>::make(alloc, device_id, xid, is_8125);
  RtlChipModel* m = owned.get();
  {
    Rtl8169NetworkInterface nic(util::move(owned), alloc);
    CHECK(!nic.enable_interrupts(&count_wake, nullptr));  // not started
    CHECK(nic.initialize());
    CHECK((m->raw(0xE0, 2) & (1u << 5)) != 0);            // receive checksums on (C+ command RxChkSum)
    g_wakes = 0;
    CHECK(nic.enable_interrupts(&count_wake, nullptr) && nic.interrupts_enabled() && m->attaches == 1);
    CHECK(m->raw(m->mask_reg(), m->status_width()) == (0x0001 | 0x0002 | 0x0010 | 0x0020 | 0x0040 | 0x0100));
    if (!is_8125) CHECK(m->raised == 1 && g_wakes == 1);      // the forced software interrupt went through
    CHECK(m->raw(m->status_reg(), m->status_width()) == 0);  // and was acknowledged
    const int wakes = g_wakes;
    CHECK(m->deliver(frame_of(100, 1)));
    CHECK(g_wakes == wakes + 1 && m->raw(m->status_reg(), m->status_width()) == 0);
    uint8_t buffer[2048];
    ReceivedFrame got;
    CHECK(nic.receive_frame(ByteView(buffer, sizeof buffer), &got) && got.length == 100 && !got.ipv4_header_checksum_good && !got.transport_checksum_good);
    // what the chip says about checksums
    struct { uint32_t bits; bool ip, l4; } cases[] = {
        {1u << 17, true, true},                // TCP, checked
        {1u << 18, true, true},                // UDP, checked
        {(1u << 17) | (1u << 18), true, false},  // other IP
        {(1u << 17) | (1u << 14), true, false},  // TCP, failed
        {(1u << 18) | (1u << 15), true, false},  // UDP, failed
        {(1u << 17) | (1u << 16), false, false},  // the IP header failed
    };
    for (const auto& c : cases) {
      CHECK(m->deliver(frame_of(80, 2), (3u << 28) | c.bits));
      CHECK(nic.receive_frame(ByteView(buffer, sizeof buffer), &got) && got.length == 80 && got.ipv4_header_checksum_good == c.ip &&
            got.transport_checksum_good == c.l4);
    }
    CHECK(nic.statistics().interrupts == static_cast<uint64_t>(m->raised));
    check_transmit_checksums(nic, m, true);
    nic.disable_interrupts();
    CHECK(!nic.interrupts_enabled() && m->detaches == 1 && m->raw(m->mask_reg(), m->status_width()) == 0);
    const int quiet = g_wakes;
    m->deliver(frame_of(64, 3));
    CHECK(g_wakes == quiet);
    // on again, then shut down with them on: the route is taken down
    CHECK(nic.enable_interrupts(&count_wake, nullptr));
    nic.shutdown();
    CHECK(m->detaches == 2);
  }
  CHECK(alloc.dma.empty());
  // a route that delivers nothing: kept polled (the RTL8125/8126 cannot tell)
  UniquePointer<RtlChipModel> lost = UniquePointer<RtlChipModel>::make(alloc, device_id, xid, is_8125);
  lost->msi_lost = true;
  RtlChipModel* l = lost.get();
  {
    Rtl8169NetworkInterface nic(util::move(lost), alloc);
    CHECK(nic.initialize());
    CHECK(nic.enable_interrupts(&count_wake, nullptr) == is_8125);
    if (!is_8125) CHECK(l->detaches == 1 && !nic.interrupts_enabled() && l->raw(l->mask_reg(), l->status_width()) == 0);
  }
  UniquePointer<RtlChipModel> refused = UniquePointer<RtlChipModel>::make(alloc, device_id, xid, is_8125);
  refused->attach_refused = true;
  {
    Rtl8169NetworkInterface nic(util::move(refused), alloc);
    CHECK(nic.initialize() && !nic.enable_interrupts(&count_wake, nullptr) && !nic.interrupts_enabled());
  }
  CHECK(alloc.dma.empty());
}

template <class Nic, class Model>
static void check_intel_interrupts(Nic& nic, Model* m) {
  g_wakes = 0;
  CHECK(nic.enable_interrupts(&count_wake, nullptr) && nic.interrupts_enabled() && m->attaches == 1);
  CHECK(m->ims == ((1u << 2) | (1u << 4) | (1u << 6) | (1u << 7)));
  CHECK(m->raised == 1 && g_wakes == 1 && m->r32(0xC0) == 0);  // the test interrupt (ICS), its cause read and cleared
  m->deliver(frame_of(90, 1));
  CHECK(g_wakes == 2 && m->r32(0xC0) == 0);
  uint8_t buffer[2048];
  ReceivedFrame got;
  CHECK(nic.receive_frame(ByteView(buffer, sizeof buffer), &got) && got.length == 90);
  m->signal(1u << 9);                // a cause that is not enabled: no interrupt
  CHECK(g_wakes == 2);
  CHECK(nic.statistics().interrupts == 2);
  nic.disable_interrupts();
  CHECK(!nic.interrupts_enabled() && m->detaches == 1 && m->ims == 0);
}

static void test_e1000e_interrupts_and_checksums(uint16_t device_id, bool chipset) {
  TestAllocator alloc;
  UniquePointer<E1000eChip> owned = UniquePointer<E1000eChip>::make(alloc, device_id, chipset);
  E1000eChip* m = owned.get();
  {
    E1000eNetworkInterface nic(util::move(owned), alloc);
    CHECK(nic.initialize());
    CHECK((m->r32(0x5000) & 0x300) == 0x300);  // RXCSUM: IP and TCP/UDP
    check_intel_interrupts(nic, m);
    uint8_t buffer[2048];
    ReceivedFrame got;
    struct { uint8_t status, errors; bool ip, l4; } cases[] = {
        {0x03 | 0x40 | 0x20, 0, true, true},     // IP and TCP checked
        {0x03 | 0x40 | 0x10, 0, true, true},     // IP and UDP checked
        {0x03 | 0x40, 0, true, false},           // IP only
        {0x03 | 0x40 | 0x20, 0x20, true, false},  // TCP wrong: still handed on, unchecked
        {0x03 | 0x40 | 0x20, 0x40, false, true},  // IP header wrong
        {0x03 | 0x40 | 0x20 | 0x04, 0, false, false},  // "ignore checksum indication"
    };
    for (const auto& c : cases) {
      CHECK(m->deliver(frame_of(70, 4), c.errors, c.status));
      CHECK(nic.receive_frame(ByteView(buffer, sizeof buffer), &got) && got.length == 70 && got.ipv4_header_checksum_good == c.ip &&
            got.transport_checksum_good == c.l4);
    }
    CHECK(m->deliver(frame_of(70, 4), 0x01));  // a CRC error: dropped
    CHECK(!nic.receive_frame(ByteView(buffer, sizeof buffer), &got));
    check_transmit_checksums(nic, m, false);
    CHECK(nic.enable_interrupts(&count_wake, nullptr));
    nic.shutdown();
    CHECK(m->detaches == 2);
  }
  UniquePointer<E1000eChip> lost = UniquePointer<E1000eChip>::make(alloc, device_id, chipset);
  lost->msi_lost = true;
  E1000eChip* l = lost.get();
  {
    E1000eNetworkInterface nic(util::move(lost), alloc);
    CHECK(nic.initialize() && !nic.enable_interrupts(&count_wake, nullptr) && !nic.interrupts_enabled() && l->detaches == 1 && l->ims == 0);
  }
  CHECK(alloc.dma.empty());
}

static void test_igb_interrupts_and_checksums(uint16_t device_id, bool igc) {
  TestAllocator alloc;
  UniquePointer<IgbChip> owned = UniquePointer<IgbChip>::make(alloc, device_id, igc);
  IgbChip* m = owned.get();
  {
    IgbNetworkInterface nic(util::move(owned), alloc);
    CHECK(nic.initialize());
    CHECK((m->r32(0x5000) & 0x300) == 0x300);
    check_intel_interrupts(nic, m);
    CHECK(m->r32(0x1700) == (nic.model()->kind == IgbKind::I82575 ? 0u : 0x8080u));
    uint8_t buffer[2048];
    ReceivedFrame got;
    struct { uint32_t status; bool ip, l4; } cases[] = {
        {0x03 | 0x40 | 0x20, true, true},
        {0x03 | 0x40 | 0x10, true, true},
        {0x03 | 0x40, true, false},
        {0x03 | 0x40 | 0x20 | 0x20000000, true, false},
        {0x03 | 0x40 | 0x20 | 0x40000000, false, true},
        {0x03 | 0x40 | 0x20 | 0x04, false, false},
    };
    for (const auto& c : cases) {
      CHECK(m->deliver(frame_of(70, 4), c.status));
      CHECK(nic.receive_frame(ByteView(buffer, sizeof buffer), &got) && got.length == 70 && got.ipv4_header_checksum_good == c.ip &&
            got.transport_checksum_good == c.l4);
    }
    check_transmit_checksums(nic, m, false);
    CHECK(!m->bad_tx_descriptor);
    // a context descriptor takes a slot: frames with checksums fill the ring twice as fast, and it still never overruns the device
    const size_t before = m->sent.size();
    TransmitChecksum request;
    const Bytes f = transport_frame(6, 500, &request);
    for (int i = 0; i < 500; ++i) CHECK(nic.transmit_frame(ConstByteView(f.data(), f.size()), request) || i % 2);
    for (int i = 0; i < 300; ++i) CHECK(nic.transmit(ConstByteView(f.data(), 100)));
    CHECK(m->sent.size() >= before + 550 && !m->bad_tx_descriptor);
    for (size_t i = before; i < m->sent.size(); ++i)
      if (m->sent[i].size() == f.size()) CHECK(transport_checksum_ok(m->sent[i]));
    nic.shutdown();
  }
  CHECK(alloc.dma.empty());
}

// An igb whose device sends nothing: the ring fills (two descriptors per checksummed frame) and is refused cleanly, never overwritten.
static void test_igb_ring_full() {
  TestAllocator alloc;
  UniquePointer<IgbChip> owned = UniquePointer<IgbChip>::make(alloc, 0x1533, false);
  IgbChip* m = owned.get();
  IgbNetworkInterface nic(util::move(owned), alloc);
  CHECK(nic.initialize());
  m->w32(kTctl, m->r32(kTctl) & ~2u);  // (the model sends only while TCTL.EN is set)
  TransmitChecksum request;
  const Bytes f = transport_frame(17, 300, &request);
  uint32_t accepted = 0;
  while (nic.transmit_frame(ConstByteView(f.data(), f.size()), request)) ++accepted;
  CHECK(accepted == (IgbNetworkInterface::kTransmitDescriptors - 1) / 2);
  CHECK(!nic.transmit(ConstByteView(f.data(), f.size())) || accepted * 2 + 1 <= IgbNetworkInterface::kTransmitDescriptors - 1);
  CHECK(m->sent.empty());
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
  test_rtl_interrupts_and_checksums(0x8168, 0x541, false);  // RTL8168H
  test_rtl_interrupts_and_checksums(0x8169, 0x008, false);  // RTL8169 (the other descriptor format)
  test_rtl_interrupts_and_checksums(0x8125, 0x641, true);   // RTL8125B
  test_e1000e_interrupts_and_checksums(0x10D3, false);      // 82574L
  test_e1000e_interrupts_and_checksums(0x15BC, true);       // I219-V
  test_igb_interrupts_and_checksums(0x10C9, false);         // 82576
  test_igb_interrupts_and_checksums(0x1533, false);         // I210
  test_igb_interrupts_and_checksums(0x10A7, false);         // 82575 (no IVAR)
  test_igb_interrupts_and_checksums(0x15F3, true);          // I225-V
  test_igb_ring_full();
  if (g_failed) {
    printf("%d check(s) failed\n", g_failed);
    return 1;
  }
  printf("PciNicTest: all passed\n");
  return 0;
}
