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
#include "Hardware/NetworkInterface/InterruptModeration.hpp"
#include "TsoReference.hpp"

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

// The Toeplitz hash of RSS over `input`, with a 40-byte key (Microsoft's RSS specification).
static uint32_t toeplitz(const uint8_t* key, const uint8_t* input, size_t length) {
  uint32_t result = 0, window = (uint32_t(key[0]) << 24) | (key[1] << 16) | (key[2] << 8) | key[3];
  for (size_t i = 0; i < length; ++i)
    for (int bit = 7; bit >= 0; --bit) {
      if (input[i] & (1u << bit)) result ^= window;
      const size_t next = i * 8 + (7 - bit) + 32;  // the key bit that comes into the window
      window = (window << 1) | ((next / 8 < 40) ? ((key[next / 8] >> (7 - next % 8)) & 1) : 0);
    }
  return result;
}

// True if an IPv4 TCP/UDP frame's transport checksum is right (UDP may say 0: none).
static bool is_v6(const Bytes& f) { return f.size() >= 54 && f[12] == 0x86 && f[13] == 0xDD; }
static bool transport_checksum_ok(const Bytes& f) {
  if (is_v6(f)) {
    const uint32_t l4 = (f[18] << 8) | f[19], protocol = f[20];
    if ((protocol != 6 && protocol != 17) || 54 + l4 > f.size()) return false;
    uint32_t sum = protocol + l4;
    for (int i = 22; i < 54; i += 2) sum += (f[i] << 8) | f[i + 1];
    for (uint32_t i = 0; i < l4; i += 2) sum += (f[54 + i] << 8) | (i + 1 < l4 ? f[54 + i + 1] : 0);
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return sum == 0xFFFF;
  }
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

// The same over IPv6 (2001:db8::1 -> 2001:db8::2): TCP or UDP straight after the 40-byte header.
static Bytes transport_frame6(uint8_t protocol, uint32_t payload, TransmitChecksum* request) {
  const uint32_t l4_header = protocol == 6 ? 20 : 8, l4 = l4_header + payload;
  Bytes f(54 + l4, 0);
  for (int i = 0; i < 6; ++i) { f[i] = 0x02; f[6 + i] = 0x04; }
  f[12] = 0x86;
  f[13] = 0xDD;
  f[14] = 0x60;
  f[18] = static_cast<uint8_t>(l4 >> 8);
  f[19] = static_cast<uint8_t>(l4);
  f[20] = protocol;
  f[21] = 64;
  f[22] = 0x20; f[23] = 0x01; f[24] = 0x0D; f[25] = 0xB8; f[37] = 1;
  f[38] = 0x20; f[39] = 0x01; f[40] = 0x0D; f[41] = 0xB8; f[53] = 2;
  uint8_t* t = &f[54];
  t[0] = 0xC0; t[1] = 0x02; t[2] = 0x00; t[3] = 0x50;
  if (protocol == 17) { t[4] = static_cast<uint8_t>(l4 >> 8); t[5] = static_cast<uint8_t>(l4); }
  else { t[12] = 0x50; t[13] = 0x18; }
  for (uint32_t i = 0; i < payload; ++i) t[l4_header + i] = static_cast<uint8_t>(i * 29 + 3);
  const uint32_t field = 54 + (protocol == 6 ? 16 : 6);
  uint32_t seed = protocol + l4;
  for (int i = 22; i < 54; i += 2) seed += (f[i] << 8) | f[i + 1];
  while (seed >> 16) seed = (seed & 0xFFFF) + (seed >> 16);
  f[field] = static_cast<uint8_t>(seed >> 8);
  f[field + 1] = static_cast<uint8_t>(seed);
  *request = TransmitChecksum{54, static_cast<uint16_t>(field)};
  return f;
}

// TSO headers as the driver must leave them: IPv4 total length and checksum zero, or IPv6 payload length zero.
static bool tso_headers_prepared(const Bytes& chain) {
  if (is_v6(chain)) return chain[18] == 0 && chain[19] == 0;
  return chain.size() >= 54 && chain[16] == 0 && chain[17] == 0 && chain[24] == 0 && chain[25] == 0;
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
  // MSI-X: the handler of every entry, and how many entries the "table" has (0: no MSI-X capability).
  QueueInterruptHandler queue_handler = nullptr;
  void* queue_context = nullptr;
  uint32_t msix_entries = 0, msix_attached = 0;
  std::vector<int> vector_raised = std::vector<int>(8, 0);
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
  uint32_t attach_interrupts(uint32_t count, QueueInterruptHandler h, void* context) override {
    if (attach_refused || count > msix_entries || handler || queue_handler) return 0;
    ++attaches;
    queue_handler = h;
    queue_context = context;
    msix_attached = count;
    return count;
  }
  void detach_interrupt() override {
    if (handler || queue_handler) ++detaches;
    handler = nullptr;
    queue_handler = nullptr;
    msix_attached = 0;
  }
  void raise_vector(uint32_t v) {
    if (!queue_handler || msi_lost || v >= msix_attached) return;
    ++raised;
    ++vector_raised[v];
    queue_handler(queue_context, v);
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
    if (off == mask_reg() && width == status_width()) {  // opening the mask over pending status interrupts at once
      const uint32_t was = raw(off, width);
      set_raw(off, width, v);
      if ((raw(status_reg(), status_width()) & v & ~was)) raise();
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
  Bytes chain;
  uint32_t chain_opts1 = 0, chain_opts2 = 0;
  int segmentations = 0;
  void run_transmit() {
    ++polls;
    for (int n = 0; n < 4096; ++n) {
      const uint64_t d = tx_ring() + 16ull * tx_index;
      const uint32_t opts1 = get32(d);
      if (!(opts1 & (1u << 31))) return;
      const uint32_t len = opts1 & 0x3FFF, opts2 = get32(d + 4);
      if (opts1 & (1u << 29)) {  // FirstFrag: a new chain
        if (!chain.empty()) { printf("model: a chain without LastFrag\n"); ++g_failed; }
        chain.clear();
        chain_opts1 = opts1;
        chain_opts2 = opts2;
      } else if ((opts1 & 0x07FF0000u & ~(3u << 28)) != (chain_opts1 & 0x07FF0000u & ~(3u << 28)) || opts2 != chain_opts2) {
        printf("model: the descriptors of a chain disagree\n");
        ++g_failed;
      }
      chain.insert(chain.end(), at(get64(d + 8)), at(get64(d + 8)) + len);
      if (opts1 & (1u << 28)) finish_chain();  // LastFrag
      put32(d, opts1 & ~(1u << 31));
      tx_index = (opts1 & (1u << 30)) ? 0 : tx_index + 1;
    }
  }
  void finish_chain() {
    const uint32_t opts1 = chain_opts1, opts2 = chain_opts2;
    if (is_v6(chain) && (opts1 & (1u << 26))) { printf("model: IPv4 giant send for an IPv6 segment\n"); ++g_failed; }
    if (opts1 & (1u << 25)) {  // IPv6 giant send: headers prepared as tcp_v6_gso_csum_prep leaves them
      ++segmentations;
      if (!is_v6(chain) || !tso_headers_prepared(chain) || ((opts1 >> 18) & 0x7F) != 54) { printf("model: IPv6 giant send set up wrong\n"); ++g_failed; }
      for (auto& f : reference_segment(chain.data(), chain.size(), static_cast<uint16_t>((opts2 >> 18) & 0x7FF), true)) sent.push_back(f);
    } else if (opts1 & (1u << 26)) {  // giant send: the TCP header at opts1 24:18, the MSS at opts2 28:18
      ++segmentations;
      if (rtl8169_identify(version << 20)->family == RtlFamily::Rtl8169 || !rtl8169_identify(version << 20)->tso) { printf("model: giant send on a chip without it\n"); ++g_failed; }
      if (((opts1 >> 18) & 0x7F) != 14u + (chain[14] & 0x0F) * 4u) { printf("model: the TCP header offset is wrong\n"); ++g_failed; }
      for (auto& f : reference_segment(chain.data(), chain.size(), static_cast<uint16_t>((opts2 >> 18) & 0x7FF), false)) sent.push_back(f);
    } else {
      // checksums: the RTL8169's TD0 bits (the chip finds the transport header itself), or TD1 with the header's offset
      const bool tcp0 = opts1 & (1u << 16), udp0 = opts1 & (1u << 17), tcp1 = opts2 & (1u << 30), udp1 = opts2 & (1u << 31);
      if (tcp0 || udp0 || tcp1 || udp1) {
        const bool v2 = rtl8169_identify(version << 20)->family != RtlFamily::Rtl8169;
        if ((tcp0 || udp0) == v2) { printf("model: checksum bits of the wrong descriptor format\n"); ++g_failed; }
        if ((tcp1 || udp1) && bool(opts2 & (1u << 28)) != is_v6(chain)) { printf("model: the IPv6 checksum bit does not match the frame\n"); ++g_failed; }
        if ((tcp1 || udp1) && bool(opts2 & (1u << 29)) == is_v6(chain)) { printf("model: the IPv4 checksum bit does not match the frame\n"); ++g_failed; }
        if ((tcp0 || udp0) && is_v6(chain)) { printf("model: an RTL8169 has no IPv6 checksum\n"); ++g_failed; }
        const uint32_t start = (tcp1 || udp1) ? (opts2 >> 18) & 0x3FF : 14 + (chain[14] & 0x0F) * 4u;
        hardware_checksum(chain.data(), static_cast<uint32_t>(chain.size()), start, start + ((tcp0 || tcp1) ? 16 : 6));
        ++checksummed;
      }
      sent.push_back(chain);
    }
    chain.clear();
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
  // What leaves the controller: short frames padded (TCTL.PSP) - after any checksum. A checksummed frame must come unpadded: a controller
  // that works the TCP/UDP length out from the frame (QEMU's) would count the padding.
  void emit(Bytes frame, bool checksummed) {
    const size_t ip_length = is_v6(frame) ? 54u + ((frame[18] << 8) | frame[19]) : frame.size() >= 34 ? 14u + ((frame[16] << 8) | frame[17]) : frame.size();
    if (checksummed && (frame[12] == 0x08 || is_v6(frame)) && frame.size() != ip_length) {
      printf("model: a checksummed frame padded before the controller\n");
      ++g_failed;
    }
    if (frame.size() < 60 && (r32(kTctl) & (1u << 3))) frame.resize(60, 0);
    sent.push_back(frame);
  }
  // ICR collects causes, IMS (set through IMS, cleared through IMC) says which interrupt; reading ICR clears it.
  uint32_t ims = 0;
  // MSI-X mode (GPIE.MSIX_MODE): causes go to EICR bits (a queue's through IVAR, the others through IVAR_MISC), EIMS enables them, EIAC
  // clears and EIAM masks a bit when its interrupt goes out.
  uint32_t eims = 0, eicr = 0;
  bool msix_mode() const { return r32(0x1514) & (1u << 4); }
  bool ivar_82576 = true;  // the IVAR layout: a byte per queue (82576) or two queues per register (82580 and later)
  int rx_vector(uint32_t q) const {
    const uint32_t index = ivar_82576 ? (q & 7) : (q >> 1), shift = ivar_82576 ? ((q & 8) << 1) : ((q & 1) << 4);
    const uint32_t entry = (r32(0x1700 + 4 * index) >> shift) & 0xFF;
    return (entry & 0x80) ? static_cast<int>(entry & 0x1F) : -1;
  }
  void fire_vector(uint32_t v) {
    eicr |= 1u << v;
    if (!(eims & (1u << v))) return;
    if (r32(0x152C) & (1u << v)) eicr &= ~(1u << v);
    if ((r32(0x1530) & (1u << v)) && (r32(0x1514) & (1u << 30))) eims &= ~(1u << v);
    raise_vector(v);
  }
  virtual void signal(uint32_t causes) {
    w32(0xC0, r32(0xC0) | causes);
    if (msix_mode()) {
      const uint32_t misc = (r32(0x1740) >> 8) & 0xFF;
      if ((causes & ims & ~(1u << 7)) && (misc & 0x80)) fire_vector(misc & 0x1F);
      return;
    }
    if (r32(0xC0) & ims) raise();
  }
  virtual void signal_queue(uint32_t q) {
    if (msix_mode()) {
      const int v = rx_vector(q);
      if (v >= 0) fire_vector(static_cast<uint32_t>(v));
      return;
    }
    signal(1u << 7);  // RXT0
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
    if (off == 0xD0) {  // IMS: unmasking a pending cause interrupts at once
      const uint32_t was = ims;
      ims |= v;
      w32(0xD0, ims);
      if (!msix_mode() && (r32(0xC0) & ims & ~was)) raise();
      return;
    }
    if (off == 0xD8) { ims &= ~v; w32(0xD0, ims); return; }
    if (off == 0xC8) { signal(v); return; }
    if (off == 0x1524) {  // EIMS: the same for the extended causes
      const uint32_t was = eims;
      eims |= v;
      for (uint32_t b = 0; b < 32; ++b)
        if ((eicr & eims & ~was) & (1u << b)) { eicr &= ~(1u << b); fire_vector(b); }
      return;
    }
    if (off == 0x1528) { eims &= ~v; return; }
    if (off == 0x1520) {  // EICS
      for (uint32_t b = 0; b < 32; ++b) if (v & (1u << b)) fire_vector(b);
      return;
    }
    if (off == kCtrl) {
      const uint32_t old = r32(kCtrl);
      if ((old & (1u << 16)) && !(v & (1u << 16)) && !(old & (1u << 17))) {  // LANPHYPC override released while the value was low: PHY power cycle
        ++lanphypc_toggles;
        phy_asleep = false;
        w32(kCtrlExt, r32(kCtrlExt) | (1u << 2));
      }
      if (v & reset_bit) {
        ++resets;
        ims = eims = eicr = 0;
        w32(0xD0, 0);
        w32(0x1514, 0);
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
  uint32_t rx_head[2] = {}, tx_head = 0;
  uint32_t last_hash = 0;
  int dropped = 0;
  int checksummed = 0, segmentations = 0;
  bool have_context = false, context_tse = false, context_ip = false, chain_popts_ixsm = false;
  uint32_t context_start = 0, context_field = 0, context_mss = 0, context_header = 0, context_payload = 0;
  Bytes chain;  // the pieces of the frame being sent
  // TSE as the 82574 does it (headers prepared by the driver: IP length and checksum zero, the seed without the length)
  void segment_chain(uint32_t mss, uint32_t header, uint32_t payload) {
    ++segmentations;
    if (!tso_headers_prepared(chain)) { printf("model: TSO headers not prepared\n"); ++g_failed; }
    if (is_v6(chain) && (context_ip || chain_popts_ixsm)) { printf("model: IPv4 header checksum asked for an IPv6 segment\n"); ++g_failed; }
    if (header + payload != chain.size()) { printf("model: TSO lengths disagree (%u + %u != %zu)\n", header, payload, chain.size()); ++g_failed; }
    for (auto& f : reference_segment(chain.data(), chain.size(), static_cast<uint16_t>(mss), true)) emit(f, false);
  }
  E1000eChip(uint16_t device_id, bool is_chipset) : IntelModel(device_id) {
    chipset = is_chipset;
    msix_entries = device_id == 0x10D3 || device_id == 0x10F6 ? 5 : 0;
    phy_address = is_chipset && device_id != 0x10BD ? 2 : 1;
  }
  uint64_t ring(uint32_t base) const { return r32(base) | (uint64_t{r32(base + 4)} << 32); }
  uint32_t count(uint32_t len_reg) const { return r32(len_reg) / 16; }
  void on_reset() override { rx_head[0] = rx_head[1] = tx_head = 0; w32(0x2810, 0); w32(0x2910, 0); w32(0x3810, 0); w32(0x00E4, 0); w32(0x5818, 0); }
  // MSI-X as the 82574 does it: IVAR names a vector for receive queue 0 (bits 3:0), 1 (7:4) and the other causes (19:16), bit 3 of each says
  // valid; a queue's cause in ICR with its IMS bit raises its vector (EIAC clears the cause), anything else with IMS.OTHER the other vector.
  bool msix82574() const { return msix_attached > 0 && r32(0x00E4) != 0; }
  int ivar_vector(uint32_t shift) const {
    const uint32_t f = (r32(0x00E4) >> shift) & 0xF;
    return (f & 8) ? static_cast<int>(f & 7) : -1;
  }
  void route(uint32_t causes) {
    for (uint32_t q = 0; q < 2; ++q) {
      const uint32_t bit = 1u << (20 + q);
      if ((causes & bit) && (r32(0xC0) & bit) && (ims & bit)) {
        if (r32(0x00DC) & bit) w32(0xC0, r32(0xC0) & ~bit);
        const int v = ivar_vector(4 * q);
        if (v >= 0) raise_vector(static_cast<uint32_t>(v));
      }
    }
    const uint32_t other = causes & r32(0xC0) & ims & ~((1u << 20) | (1u << 21) | (1u << 22) | (1u << 23) | (1u << 24));
    if (other && (ims & (1u << 24))) {
      const int v = ivar_vector(16);
      if (v >= 0) raise_vector(static_cast<uint32_t>(v));
    }
  }
  void signal(uint32_t causes) override {
    if (!msix82574()) { IntelModel::signal(causes); return; }
    w32(0xC0, r32(0xC0) | causes);
    route(causes);
  }
  void signal_queue(uint32_t q) override {
    if (msix82574()) signal(1u << (20 + q));
    else IntelModel::signal(1u << 7);  // RXT0
  }
  void on_write(uint32_t off, int width, uint32_t v) override {
    if (msix82574() && off == 0xD0) {  // IMS: a pending cause that becomes enabled interrupts at once
      const uint32_t was = ims;
      ims |= v;
      w32(0xD0, ims);
      route(~was & v);
      return;
    }
    if (msix82574() && off == 0xC8) { signal(v); return; }
    IntelModel::on_write(off, width, v);
  }
  int rss_queue(const Bytes& frame) {
    const uint32_t mrqc = r32(0x5818);
    if ((mrqc & 3) != 1 || frame.size() < 34 || frame[12] != 0x08 || frame[13] != 0) return 0;
    uint8_t key[40], input[12];
    for (int i = 0; i < 40; ++i) key[i] = static_cast<uint8_t>(r32(0x5C80 + 4 * (i / 4)) >> (8 * (i % 4)));
    memcpy(input, &frame[26], 8);
    size_t length = 8;
    if (frame[23] == 6 && (mrqc & (1u << 16)) && frame.size() >= 38) { memcpy(input + 8, &frame[34], 4); length = 12; }
    else if (!(mrqc & (1u << 17))) return 0;
    last_hash = toeplitz(key, input, length);
    const uint32_t entry = last_hash & 127;
    return static_cast<int>((r32(0x5C00 + 4 * (entry / 4)) >> (8 * (entry % 4) + 7)) & 1);
  }
  void after_write(uint32_t off, uint32_t v) override {
    if (off == 0x3818 && (r32(kTctl) & 2)) {  // TDT: send up to the tail
      const uint32_t n = count(0x3808);
      while (tx_head != v % n) {
        const uint64_t d = ring(0x3800) + 16ull * tx_head;
        const uint16_t len = get16(d + 8);
        const uint8_t cmd = at(d + 11)[0], type = at(d + 10)[0] >> 4;
        if (!(cmd & 0x20) && (cmd & 4)) { printf("model: legacy checksum insertion (QEMU ignores it)\n"); ++g_failed; }
        if ((cmd & 0x20) && type == 0) {  // a TCP/IP context descriptor: TUCSS, TUCSO (and for TSE PAYLEN, HDRLEN, MSS); not written back
          context_start = at(d + 4)[0];
          context_field = at(d + 5)[0];
          context_tse = cmd & 0x04;
          context_ip = cmd & 0x02;
          context_mss = get16(d + 14);
          context_header = at(d + 13)[0];
          context_payload = get16(d + 8) | (static_cast<uint32_t>(at(d + 10)[0] & 0x0F) << 16);
          have_context = true;
          tx_head = (tx_head + 1) % n;
          continue;
        }
        chain.insert(chain.end(), at(get64(d)), at(get64(d)) + len);
        const bool extended = (cmd & 0x20) && type == 1;
        if (extended && (at(d + 13)[0] & 1)) chain_popts_ixsm = true;
        if (cmd & 1) {  // EOP: the frame (or segment) is complete
          if (extended && (cmd & 0x04)) {  // TSE: cut it up
            if (!have_context || !context_tse) { printf("model: TSE without a segmentation context\n"); ++g_failed; }
            else segment_chain(context_mss, context_header, context_payload);
          } else {
            const bool txsm = extended && (at(d + 13)[0] & 2);
            if (txsm) {
              if (!have_context) { printf("model: TXSM without a context\n"); ++g_failed; }
              else hardware_checksum(chain.data(), static_cast<uint32_t>(chain.size()), context_start, context_field);
              ++checksummed;
            }
            emit(chain, txsm);
          }
          chain.clear();
          chain_popts_ixsm = false;
        }
        at(d + 12)[0] |= 1;  // DD
        tx_head = (tx_head + 1) % n;
      }
      w32(0x3810, tx_head);
    }
  }
  bool deliver(const Bytes& frame, uint8_t errors = 0, uint8_t status = 3, int queue = -1) {
    if (!(r32(kRctl) & 2)) { ++dropped; return false; }
    const uint32_t q = queue >= 0 ? static_cast<uint32_t>(queue) : static_cast<uint32_t>(rss_queue(frame));
    const uint32_t base = 0x2800 + 0x100 * q;
    const uint32_t n = count(base + 8);
    if (n == 0 || rx_head[q] == r32(base + 0x18) % n) { ++dropped; return false; }  // no descriptor
    const uint64_t d = ring(base) + 16ull * rx_head[q];
    memcpy(at(get64(d)), frame.data(), frame.size());
    if (r32(0x5008) & (1u << 15)) {  // extended write-back: RSS info, status and errors, length
      put64(d, 0xABCD000000000000ull | last_hash);
      put32(d + 8, status | (uint32_t{errors} << 24));
      put16(d + 12, static_cast<uint16_t>(frame.size()));
      put16(d + 14, 0);
    } else {
      put16(d + 8, static_cast<uint16_t>(frame.size()));
      at(d + 12)[0] = status;
      at(d + 13)[0] = errors;
    }
    rx_head[q] = (rx_head[q] + 1) % n;
    w32(base + 0x10, rx_head[q]);
    signal_queue(q);
    return true;
  }
};

struct IgbChip final : IntelModel {
  uint32_t rx_head[4] = {}, tx_head[4] = {};
  int dropped = 0;
  bool bad_tx_descriptor = false;
  struct Context {
    bool valid = false, tcp = false, ipv4 = false;
    uint32_t start = 0, mss = 0, l4 = 0;
  } context, contexts[4];
  struct Chain {
    Bytes bytes;
    uint32_t olinfo = 0;
    bool tse = false;
  } chains[4];
  int checksummed = 0, segmentations = 0;
  uint32_t last_hash = 0;
  IgbChip(uint16_t device_id, bool igc) : IntelModel(device_id) {
    if (igc) reset_bit = 1u << 29;
    ivar_82576 = device_id == 0x10C9 || device_id == 0x10E6 || device_id == 0x10E7 || device_id == 0x10E8 || device_id == 0x1526 ||
                 device_id == 0x150A || device_id == 0x1518 || device_id == 0x150D;
    msix_entries = device_id == 0x10A7 ? 0 : 10;
  }
  uint32_t rq(uint32_t q, uint32_t reg) const { return 0xC000 + 0x40 * q + reg; }
  uint64_t ring(uint32_t base) const { return r32(base) | (uint64_t{r32(base + 4)} << 32); }
  void on_reset() override {
    for (uint32_t q = 0; q < 4; ++q) {
      rx_head[q] = tx_head[q] = 0;
      w32(rq(q, 0x28), 0); w32(rq(q, 0x10), 0); w32(tq(q, 0x28), 0); w32(tq(q, 0x10), 0);
    }
  }
  uint32_t tq(uint32_t q, uint32_t reg) const { return 0xE000 + 0x40 * q + reg; }
  std::vector<uint32_t> sent_queue;  // the transmit queue each entry of `sent` left on
  uint32_t current_queue = 0;
  void after_write(uint32_t off, uint32_t v) override {
    if (off < 0xE000 || off >= 0xE100 || (off - 0xE000) % 0x40 != 0x18) return;  // a TDT
    const uint32_t q = (off - 0xE000) / 0x40;
    if (!(r32(kTctl) & 2) || !(r32(tq(q, 0x28)) & (1u << 25))) return;
    current_queue = q;
    const uint32_t n = r32(tq(q, 0x08)) / 16;
    if (n == 0) return;
    // (contexts are per queue)
    std::swap(context, contexts[q]);
    while (tx_head[q] != v % n) {
      const uint64_t d = ring(tq(q, 0)) + 16ull * tx_head[q];
      const uint32_t ctl = get32(d + 8), olinfo = get32(d + 12);
      if (((ctl >> 20) & 0xF) == 2 && (ctl & (1u << 29))) {  // a context descriptor: remembered, not written back
        const uint32_t lens = get32(d);
        context.start = ((lens >> 9) & 0x7F) + (lens & 0x1FF);
        context.tcp = ctl & (1u << 11);
        context.mss = olinfo >> 16;
        context.l4 = (olinfo >> 8) & 0xFF;
        context.ipv4 = ctl & (1u << 10);
        context.valid = true;
        tx_head[q] = (tx_head[q] + 1) % n;
        continue;
      }
      // a data descriptor (DEXT, DTYP 3) with IFCS; PAYLEN on the first of a chain
      if (((ctl >> 20) & 0xF) != 3 || !(ctl & (1u << 29)) || !(ctl & (1u << 25))) bad_tx_descriptor = true;
      Chain& chain = chains[q];
      if (chain.bytes.empty()) { chain.olinfo = olinfo; chain.tse = ctl & (1u << 31); }
      else if (olinfo != 0 || bool(ctl & (1u << 31)) != chain.tse) bad_tx_descriptor = true;
      chain.bytes.insert(chain.bytes.end(), at(get64(d)), at(get64(d)) + (ctl & 0xFFFF));
      put64(d, 0);  // write-back: the address is gone
      put32(d + 8, 0);
      if (ctl & (1u << 24)) {  // EOP
        if (!(ctl & (1u << 27))) bad_tx_descriptor = true;  // (RS on the last)
        put32(d + 12, 1);  // DD
        finish_chain(chain);
      } else {
        put32(d + 12, 0);
      }
      tx_head[q] = (tx_head[q] + 1) % n;
    }
    std::swap(context, contexts[q]);
    w32(tq(q, 0x10), tx_head[q]);
  }
  void finish_chain(Chain& chain) {
    const size_t before = sent.size();
    if (chain.tse) {  // TSE: PAYLEN is the payload, the context has MSS and the TCP header length
      ++segmentations;
      const uint32_t header = context.start + context.l4;
      if (!context.valid || !context.tcp || (chain.olinfo >> 14) != chain.bytes.size() - header || !(chain.olinfo & (3u << 8))) bad_tx_descriptor = true;
      if (!tso_headers_prepared(chain.bytes)) {
        printf("model: TSO headers not prepared\n");
        ++g_failed;
      }
      if (is_v6(chain.bytes) && (context.ipv4 || (chain.olinfo & (1u << 8)))) {
        printf("model: IPv4 header checksum asked for an IPv6 segment\n");
        ++g_failed;
      }
      for (auto& f : reference_segment(chain.bytes.data(), chain.bytes.size(), static_cast<uint16_t>(context.mss), true)) emit(f, false);
    } else {
      if ((chain.olinfo >> 14) != chain.bytes.size()) bad_tx_descriptor = true;
      if (chain.olinfo & (1u << 9)) {  // TXSM: the transport checksum, as the context describes
        if (!context.valid) bad_tx_descriptor = true;
        else hardware_checksum(chain.bytes.data(), static_cast<uint32_t>(chain.bytes.size()), context.start, context.start + (context.tcp ? 16 : 6));
        ++checksummed;
      }
      emit(chain.bytes, chain.olinfo & (1u << 9));
    }
    while (sent_queue.size() < before) sent_queue.push_back(0);
    while (sent_queue.size() < sent.size()) sent_queue.push_back(current_queue);
    chain.bytes.clear();
  }
  // The queue RSS picks for a frame: the hash of the IPv4 addresses (and the ports of TCP/UDP) with the key in RSSRK, through RETA.
  int rss_queue(const Bytes& frame) {
    const uint32_t mrqc = r32(0x5818);
    if (!(mrqc & 2) || frame.size() < 34 || frame[12] != 0x08 || frame[13] != 0) return 0;
    uint8_t key[40], input[12];
    for (int i = 0; i < 40; ++i) key[i] = static_cast<uint8_t>(r32(0x5C80 + 4 * (i / 4)) >> (8 * (i % 4)));
    memcpy(input, &frame[26], 8);
    size_t length = 8;
    const uint8_t protocol = frame[23];
    if (((protocol == 6 && (mrqc & (1u << 16))) || (protocol == 17 && (mrqc & (1u << 22)))) && frame.size() >= 38) {
      memcpy(input + 8, &frame[34], 4);
      length = 12;
    } else if (!(mrqc & (1u << 17))) {
      return 0;
    }
    last_hash = toeplitz(key, input, length);
    const uint32_t entry = last_hash & 127;
    return static_cast<int>((r32(0x5C00 + 4 * (entry / 4)) >> (8 * (entry % 4))) & 0x0F);
  }
  bool deliver(const Bytes& frame, uint32_t status = 3, int queue = -1) {
    const uint32_t q = queue >= 0 ? static_cast<uint32_t>(queue) : static_cast<uint32_t>(rss_queue(frame));
    if (q >= 4 || !(r32(kRctl) & 2) || !(r32(rq(q, 0x28)) & (1u << 25))) { ++dropped; return false; }
    if ((r32(rq(q, 0x0C)) & 0x7F) != 2 || ((r32(rq(q, 0x0C)) >> 25) & 7) != 1) { ++dropped; return false; }  // 2 KiB, advanced one buffer
    const uint32_t n = r32(rq(q, 0x08)) / 16;
    if (n == 0 || rx_head[q] == r32(rq(q, 0x18)) % n) { ++dropped; return false; }
    const uint64_t d = ring(rq(q, 0)) + 16ull * rx_head[q];
    const uint64_t buffer = get64(d);
    if (buffer == 0 || get64(d + 8) != 0) { printf("model: receive descriptor not in read format\n"); ++g_failed; ++dropped; return false; }
    memcpy(at(buffer), frame.data(), frame.size());
    put64(d, 0x1234567800000000ull);  // write-back: RSS/packet info
    put64(d + 8, (uint64_t{frame.size()} << 32) | status);
    rx_head[q] = (rx_head[q] + 1) % n;
    w32(rq(q, 0x10), rx_head[q]);
    signal_queue(q);
    return true;
  }
  // (the queue-0 view the older tests use)
  uint32_t& rx_head0() { return rx_head[0]; }
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
    if (m->rx_head[0] != m->r32(0xC018) % n) {
      const uint64_t d = m->ring(0xC000) + 16ull * m->rx_head[0];
      put64(d, rng());
      put64(d + 8, rng() | 1);  // done, with anything else
      m->rx_head[0] = (m->rx_head[0] + 1) % n;
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
  // IPv6: the same, TCP and UDP
  for (uint8_t protocol : {uint8_t{6}, uint8_t{17}}) {
    for (uint32_t payload : {0u, 7u, 600u, 1440u}) {
      TransmitChecksum request;
      const Bytes f = transport_frame6(protocol, payload, &request);
      const size_t before = m->sent.size();
      const int hardware_before = m->checksummed;
      CHECK(nic.transmit_frame(ConstByteView(f.data(), f.size()), request));
      CHECK(m->sent.size() == before + 1);
      if (m->sent.size() != before + 1) continue;
      Bytes out = m->sent.back();
      out.resize(f.size());
      CHECK(transport_checksum_ok(out));
      (void)hardware_before;
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
  m->msix_entries = 0;  // (the MSI path; the 82574's MSI-X has its own test)
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
  lost->msix_entries = 0;
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
  m->msix_entries = 0;  // (the MSI path; MSI-X has its own test)
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

// ---- interrupt moderation, MSI-X with RSS, segmentation offload -------------------------------------------------------------------------------

static void test_moderation_classes() {
  using L = InterruptLatency;
  CHECK(InterruptModerator::classify(L::Low, 0, 0) == L::Low);               // nothing seen: no change
  CHECK(InterruptModerator::classify(L::Low, 2, 200) == L::Lowest);          // a ping, a keystroke
  CHECK(InterruptModerator::classify(L::Low, 20, 30000) == L::Bulk);         // full frames
  CHECK(InterruptModerator::classify(L::Low, 50, 15000) == L::Lowest);       // many small ones
  CHECK(InterruptModerator::classify(L::Low, 3, 9000) == L::Bulk);           // big ones, few
  CHECK(InterruptModerator::classify(L::Lowest, 1, 30000) == L::Bulk);       // segmentation offload
  CHECK(InterruptModerator::classify(L::Lowest, 3, 3000) == L::Low);
  CHECK(InterruptModerator::classify(L::Bulk, 40, 60000) == L::Low);
  CHECK(InterruptModerator::classify(L::Bulk, 10, 60000) == L::Bulk);
  CHECK(InterruptModerator::classify(L::Bulk, 1, 100) == L::Low);
  CHECK(InterruptModerator::rate(L::Lowest) == 70000 && InterruptModerator::rate(L::Low) == 20000 && InterruptModerator::rate(L::Bulk) == 4000);
  CHECK(InterruptModerator::intel_interval(L::Lowest) == 56 && InterruptModerator::intel_interval(L::Low) == 196 &&
        InterruptModerator::intel_interval(L::Bulk) == 980);
  InterruptModerator m;
  CHECK(m.latency() == L::Low && !m.update());     // starts at 20000 a second
  m.account(1, 64);
  CHECK(!m.update() && m.latency() == L::Low);       // (a ping would be "lowest": conservative keeps it low)
  m.account(30, 45000);
  CHECK(m.update() && m.latency() == L::Bulk && m.interrupts_per_second() == 4000);
  m.account(0xFFFFFFFFu, 0xFFFFFFF0u);
  m.account(1, 100);                                // (sums saturate)
  m.update();
}

// An interrupt shuts the device's interrupt until rearm_interrupts(): more frames meanwhile raise none; rearming with frames waiting
// interrupts at once.
template <class Nic, class Model>
static void check_masking(Nic& nic, Model* m, bool msix) {
  g_wakes = 0;
  const int before = m->raised;
  CHECK(m->deliver(frame_of(100, 1)));
  CHECK(m->raised == before + 1 && g_wakes == 1);
  CHECK(m->deliver(frame_of(100, 2)) && m->deliver(frame_of(100, 3)));
  CHECK(m->raised == before + 1);                  // masked
  uint8_t buffer[2048];
  ReceivedFrame got;
  int taken = 0;
  while (nic.receive_frame(ByteView(buffer, sizeof buffer), &got)) ++taken;
  CHECK(taken == 3);
  nic.rearm_interrupts();
  CHECK(m->raised == before + 2);                  // the causes of frames 2 and 3 were still set: one more interrupt (as with NAPI)
  CHECK(!nic.receive_frame(ByteView(buffer, sizeof buffer), &got));
  nic.rearm_interrupts();
  CHECK(m->raised == before + 2);                  // nothing pending now: quiet, and open
  CHECK(m->deliver(frame_of(100, 4)));
  CHECK(m->raised == before + 3);                  // open again
  CHECK(nic.receive_frame(ByteView(buffer, sizeof buffer), &got));
  CHECK(m->deliver(frame_of(100, 5)));             // (masked, pending)
  nic.rearm_interrupts();
  CHECK(m->raised == before + 4);                  // pending when rearmed: at once
  while (nic.receive_frame(ByteView(buffer, sizeof buffer), &got)) {}
  nic.rearm_interrupts();
  (void)msix;
}

static void test_masking_and_throttle() {
  TestAllocator alloc;
  {
    UniquePointer<RtlChipModel> owned = UniquePointer<RtlChipModel>::make(alloc, 0x8168, 0x541, false);
    RtlChipModel* m = owned.get();
    Rtl8169NetworkInterface nic(util::move(owned), alloc);
    CHECK(nic.initialize() && nic.enable_interrupts(&count_wake, nullptr));
    CHECK(m->raw(m->mask_reg(), 2) != 0);          // open after the self-test
    check_masking(nic, m, false);
  }
  {
    UniquePointer<E1000eChip> owned = UniquePointer<E1000eChip>::make(alloc, 0x10D3, false);
    owned->msix_entries = 0;  // MSI
    E1000eChip* m = owned.get();
    E1000eNetworkInterface nic(util::move(owned), alloc);
    CHECK(nic.initialize() && nic.enable_interrupts(&count_wake, nullptr));
    CHECK(m->r32(0xC4) == 196);                    // ITR: 20000 a second to begin with
    check_masking(nic, m, false);
    // a stream of full frames: bulk (4000 a second); then a ping: low latency, and lowest after another
    for (int i = 0; i < 40; ++i) m->deliver(frame_of(1514, 6));
    uint8_t buffer[2048];
    ReceivedFrame got;
    while (nic.receive_frame(ByteView(buffer, sizeof buffer), &got)) {}
    nic.rearm_interrupts();
    CHECK(m->r32(0xC4) == 980 && nic.statistics().interrupt_rate == 4000);
    m->deliver(frame_of(64, 7));
    while (nic.receive_frame(ByteView(buffer, sizeof buffer), &got)) {}
    nic.rearm_interrupts();
    CHECK(m->r32(0xC4) == 196);
    m->deliver(frame_of(64, 8));
    while (nic.receive_frame(ByteView(buffer, sizeof buffer), &got)) {}
    nic.rearm_interrupts();
    CHECK(m->r32(0xC4) == 196 && nic.statistics().interrupt_rate == 20000);  // (conservative: never the lowest-latency class)
  }
  {
    UniquePointer<IgbChip> owned = UniquePointer<IgbChip>::make(alloc, 0x1533, false);
    owned->msix_entries = 0;  // MSI
    IgbChip* m = owned.get();
    IgbNetworkInterface nic(util::move(owned), alloc);
    CHECK(nic.initialize() && nic.enable_interrupts(&count_wake, nullptr) && nic.msix_vectors() == 0);
    CHECK(m->r32(0x1680) == (196u | (1u << 31)));  // EITR(0)
    check_masking(nic, m, false);
    for (int i = 0; i < 40; ++i) m->deliver(frame_of(1514, 6));
    uint8_t buffer[2048];
    ReceivedFrame got;
    while (nic.receive_frame(ByteView(buffer, sizeof buffer), &got)) {}
    nic.rearm_interrupts();
    CHECK(m->r32(0x1680) == (980u | (1u << 31)));
  }
  CHECK(alloc.dma.empty());
}

// Frames of `flows` TCP connections (different source ports), `per_flow` each.
static std::vector<Bytes> flow_frames(uint32_t flows, uint32_t per_flow) {
  std::vector<Bytes> out;
  for (uint32_t i = 0; i < per_flow; ++i)
    for (uint32_t f = 0; f < flows; ++f) {
      TransmitChecksum request;
      Bytes frame = transport_frame(6, 100, &request);
      frame[34] = static_cast<uint8_t>(0x80 + f);  // the source port
      frame[35] = static_cast<uint8_t>(f * 7);
      frame[60] = static_cast<uint8_t>(i);
      out.push_back(frame);
    }
  return out;
}

static void test_igb_msix(uint16_t device_id, bool igc, uint32_t queues) {
  TestAllocator alloc;
  UniquePointer<IgbChip> owned = UniquePointer<IgbChip>::make(alloc, device_id, igc);
  IgbChip* m = owned.get();
  {
    IgbNetworkInterface nic(util::move(owned), alloc);
    CHECK(nic.initialize() && nic.queue_count() == queues);
    for (uint32_t q = 0; q < queues; ++q) CHECK((m->r32(m->rq(q, 0x28)) & (1u << 25)) && m->r32(m->rq(q, 0x18)) == 255);  // every queue runs
    CHECK(m->r32(0x5818) == 0);  // RSS off until MSI-X: all of it to queue 0
    CHECK(m->deliver(frame_of(80, 1)) && m->rx_head[0] == 1);
    g_wakes = 0;
    CHECK(nic.enable_interrupts(&count_wake, nullptr));
    if (queues == 1) {  // (one queue: MSI)
      CHECK(nic.msix_vectors() == 0 && m->handler && nic.offloads().receive_queues == 1);
      return;
    }
    CHECK(nic.msix_vectors() == queues + 1 && m->msix_attached == queues + 1 && nic.offloads().receive_queues == queues);
    CHECK((m->r32(0x1514) & ((1u << 4) | (1u << 30) | (1u << 31) | 1u)) == ((1u << 4) | (1u << 30) | (1u << 31) | 1u));  // GPIE
    for (uint32_t q = 0; q < queues; ++q) CHECK(m->rx_vector(q) == static_cast<int>(q));
    CHECK(((m->r32(0x1740) >> 8) & 0xFF) == (0x80 | queues));      // the link: the last vector
    const uint32_t all = (1u << (queues + 1)) - 1;
    CHECK(m->r32(0x152C) == all && m->r32(0x1530) == all && m->eims == all);
    CHECK(m->vector_raised[queues] == 1 && m->vector_raised[0] == 1 && g_wakes == 2);  // the self-test, link vector and queue 0
    CHECK((m->r32(0x5818) & 0x2) && (m->r32(0x5000) & (1u << 13)));                    // RSS on, the hash in the descriptor
    for (uint32_t e = 0; e < 128; ++e) CHECK(((m->r32(0x5C00 + 4 * (e / 4)) >> (8 * (e % 4))) & 0xFF) == e % queues);
    // the key: Microsoft's verification suite gives 0x51ccc178 for 66.9.149.187:2794 -> 161.142.100.80:1766 over TCP
    TransmitChecksum request;
    Bytes probe = transport_frame(6, 10, &request);
    const uint8_t addresses[8] = {66, 9, 149, 187, 161, 142, 100, 80};
    memcpy(&probe[26], addresses, 8);
    probe[34] = 2794 >> 8; probe[35] = 2794 & 0xFF; probe[36] = 1766 >> 8; probe[37] = 1766 & 0xFF;
    m->rss_queue(probe);
    CHECK(m->last_hash == 0x51ccc178u);
    for (uint32_t v = 0; v <= queues; ++v) CHECK(m->r32(0x1680 + 4 * v) == (v < queues ? (196u | (1u << 31)) : 0u));  // EITR per queue vector

    // RSS spreads 16 flows over the queues; every queue's vector interrupts once (then masks itself) and the driver takes all of them
    uint8_t buffer[2048];
    ReceivedFrame got;
    while (nic.receive_frame(ByteView(buffer, sizeof buffer), &got)) {}
    nic.rearm_interrupts();
    std::vector<int> before(m->vector_raised);
    const std::vector<Bytes> frames = flow_frames(16, 4);
    for (const Bytes& f : frames) CHECK(m->deliver(f));
    uint32_t used = 0;
    for (uint32_t q = 0; q < queues; ++q) {
      const int raised = m->vector_raised[q] - before[q];
      if (raised) ++used;
      CHECK(raised <= 1);  // auto-masked after the first
    }
    CHECK(used >= 2);
    uint32_t taken = 0;
    while (nic.receive_frame(ByteView(buffer, sizeof buffer), &got)) ++taken;
    CHECK(taken == frames.size());
    uint32_t busy = 0;
    for (uint32_t q = 0; q < queues; ++q) if (nic.queue_frames(q) > 0) ++busy;
    CHECK(busy >= 2);
    // a flow stays on its queue: the frames of one flow come out in order
    // (checked through the queue the model chose for each frame of flow 0)
    const int q0 = m->rss_queue(frames[0]);
    for (size_t i = 0; i < frames.size(); i += 16) CHECK(m->rss_queue(frames[i]) == q0);
    nic.rearm_interrupts();  // (the causes of the frames that came while masked interrupt once more)
    while (nic.receive_frame(ByteView(buffer, sizeof buffer), &got)) {}
    nic.rearm_interrupts();
    CHECK(m->eims == all);
    // the link vector
    const int link_before = m->vector_raised[queues];
    m->signal(1u << 2);
    CHECK(m->vector_raised[queues] == link_before + 1);
    nic.rearm_interrupts();
    nic.disable_interrupts();
    CHECK(nic.msix_vectors() == 0 && m->r32(0x5818) == 0 && m->r32(0x1514) == 0 && !m->queue_handler && m->eims == 0);
    // frames still arrive (polled, queue 0)
    CHECK(m->deliver(frame_of(70, 9)) && nic.receive_frame(ByteView(buffer, sizeof buffer), &got) && got.length == 70);
  }
  CHECK(alloc.dma.empty());
  // MSI-X refused (no table, or a route that loses the messages): MSI on one queue instead
  UniquePointer<IgbChip> lost = UniquePointer<IgbChip>::make(alloc, device_id, igc);
  lost->msi_lost = true;
  {
    IgbNetworkInterface nic(util::move(lost), alloc);
    CHECK(nic.initialize() && !nic.enable_interrupts(&count_wake, nullptr) && !nic.interrupts_enabled() && nic.msix_vectors() == 0);
  }
  UniquePointer<IgbChip> small = UniquePointer<IgbChip>::make(alloc, device_id, igc);
  small->msix_entries = 2;  // fewer entries than queues + 1
  IgbChip* sm = small.get();
  {
    IgbNetworkInterface nic(util::move(small), alloc);
    CHECK(nic.initialize() && nic.enable_interrupts(&count_wake, nullptr) && nic.msix_vectors() == 0 && sm->handler != nullptr);
  }
  CHECK(alloc.dma.empty());
}

// Segmentation offload: segments of several sizes and MSS values come out as frames the way a controller makes them (TsoReference); each
// frame is right-sized and right-checksummed, the payload is whole and in order, and PSH/FIN are only on the last.
template <class Nic, class Model>
static void check_segmentation(Nic& nic, Model* m) {
  CHECK(nic.offloads().max_segmentation_bytes == 32768);
  for (uint32_t payload : {1u, 536u, 1460u, 1461u, 8000u, 16384u, 32000u}) {
    for (uint16_t mss : {uint16_t{536}, uint16_t{1460}}) {
      TransmitChecksum request;
      Bytes f = transport_frame(6, payload, &request);
      if (f.size() > 32768) continue;
      f[47] = 0x19;  // FIN, PSH, ACK
      const size_t before = m->sent.size();
      const int segmentations = m->segmentations;
      CHECK(nic.transmit_segmented(ConstByteView(f.data(), f.size()), request, mss));
      CHECK(m->segmentations == segmentations + 1);
      const uint32_t expect = (payload + mss - 1) / mss;
      CHECK(m->sent.size() == before + expect);
      if (m->sent.size() != before + expect) continue;
      Bytes joined;
      uint32_t sequence = 0;
      for (uint32_t k = 0; k < expect; ++k) {
        Bytes out = m->sent[before + k];
        const size_t ip_length = 14u + ((out[16] << 8) | out[17]);  // (a short last frame is padded on the wire)
        CHECK(ip_length <= out.size() && (out.size() == ip_length || out.size() == 60));
        out.resize(ip_length < out.size() ? ip_length : out.size());
        CHECK(out.size() <= kEthernetMaxFrame && transport_checksum_ok(out));
        uint32_t ip_sum = 0;
        for (int i = 14; i < 34; i += 2) ip_sum += (out[i] << 8) | out[i + 1];
        while (ip_sum >> 16) ip_sum = (ip_sum & 0xFFFF) + (ip_sum >> 16);
        CHECK(ip_sum == 0xFFFF);
        const uint32_t seq = (uint32_t(out[38]) << 24) | (out[39] << 16) | (out[40] << 8) | out[41];
        if (k > 0) CHECK(seq == sequence);
        sequence = seq + static_cast<uint32_t>(out.size() - 54);
        CHECK((out[47] & 0x09) == (k + 1 == expect ? 0x09 : 0));
        joined.insert(joined.end(), out.begin() + 54, out.end());
      }
      CHECK(joined == Bytes(f.begin() + 54, f.end()));
    }
  }
  // IPv6 segments
  for (uint32_t payload : {1u, 1440u, 1441u, 9000u, 30000u}) {
    TransmitChecksum r6;
    Bytes f6 = transport_frame6(6, payload, &r6);
    f6[67] = 0x19;
    const size_t before = m->sent.size();
    CHECK(nic.transmit_segmented(ConstByteView(f6.data(), f6.size()), r6, 1440));
    const uint32_t expect = (payload + 1439) / 1440;
    CHECK(m->sent.size() == before + expect);
    Bytes joined;
    for (size_t k = before; k < m->sent.size(); ++k) {
      Bytes out = m->sent[k];
      out.resize(54u + ((out[18] << 8) | out[19]) < out.size() ? 54u + ((out[18] << 8) | out[19]) : out.size());
      CHECK(is_v6(out) && out.size() <= kEthernetMaxFrame && transport_checksum_ok(out));
      CHECK((out[67] & 0x09) == (k + 1 == m->sent.size() ? 0x09 : 0));
      joined.insert(joined.end(), out.begin() + 74, out.end());
    }
    CHECK(joined == Bytes(f6.begin() + 74, f6.end()));
  }
  // what is refused: no MSS, too large, not TCP, nothing to cut
  TransmitChecksum request;
  Bytes f = transport_frame(6, 1000, &request);
  CHECK(!nic.transmit_segmented(ConstByteView(f.data(), f.size()), request, 0));
  TransmitChecksum big_request;
  Bytes big = transport_frame(6, 33000, &big_request);
  CHECK(!nic.transmit_segmented(ConstByteView(big.data(), big.size()), big_request, 1460));
  TransmitChecksum udp_request;
  Bytes udp = transport_frame(17, 3000, &udp_request);
  CHECK(!nic.transmit_segmented(ConstByteView(udp.data(), udp.size()), udp_request, 1460));
  TransmitChecksum empty_request;
  Bytes empty = transport_frame(6, 0, &empty_request);
  CHECK(!nic.transmit_segmented(ConstByteView(empty.data(), empty.size()), empty_request, 1460));
  // the ring fills and empties with segments (17 descriptors each) and frames mixed
  for (int i = 0; i < 200; ++i) {
    TransmitChecksum r;
    Bytes s = transport_frame(6, 30000, &r);
    CHECK(nic.transmit_segmented(ConstByteView(s.data(), s.size()), r, 1460));
    CHECK(nic.transmit(ConstByteView(f.data(), 100)));
  }
  CHECK(nic.statistics().segmented_sends >= 200);
}

static void test_segmentation() {
  TestAllocator alloc;
  {
    UniquePointer<E1000eChip> owned = UniquePointer<E1000eChip>::make(alloc, 0x10D3, false);
    E1000eChip* m = owned.get();
    E1000eNetworkInterface nic(util::move(owned), alloc);
    CHECK(nic.initialize());
    check_segmentation(nic, m);
  }
  for (uint16_t id : {uint16_t{0x10C9}, uint16_t{0x1533}, uint16_t{0x15F3}}) {
    UniquePointer<IgbChip> owned = UniquePointer<IgbChip>::make(alloc, id, id == 0x15F3);
    IgbChip* m = owned.get();
    IgbNetworkInterface nic(util::move(owned), alloc);
    CHECK(nic.initialize());
    check_segmentation(nic, m);
    CHECK(!m->bad_tx_descriptor);
  }
  // a device that sends nothing: segments are refused when the ring is full, never overwritten
  UniquePointer<IgbChip> owned = UniquePointer<IgbChip>::make(alloc, 0x1533, false);
  IgbChip* m = owned.get();
  {
    IgbNetworkInterface nic(util::move(owned), alloc);
    CHECK(nic.initialize());
    m->w32(kTctl, m->r32(kTctl) & ~2u);
    TransmitChecksum r;
    Bytes s = transport_frame(6, 30000, &r);
    uint32_t accepted = 0;
    while (nic.transmit_segmented(ConstByteView(s.data(), s.size()), r, 1460)) ++accepted;
    CHECK(accepted == (IgbNetworkInterface::kTransmitDescriptors - 1) / 16);  // (15 data descriptors and a context each)
    CHECK(m->sent.empty());
  }
  CHECK(alloc.dma.empty());
}

// Transmit queues: frames of different flows spread over the queues, a flow's frames stay on one queue in order, and a queue whose device
// side has stopped fills up without holding up the others.
static void test_igb_transmit_queues() {
  TestAllocator alloc;
  UniquePointer<IgbChip> owned = UniquePointer<IgbChip>::make(alloc, 0x1533, false);
  IgbChip* m = owned.get();
  {
    IgbNetworkInterface nic(util::move(owned), alloc);
    CHECK(nic.initialize() && nic.queue_count() == 4);
    for (uint32_t q = 0; q < 4; ++q) CHECK((m->r32(m->tq(q, 0x28)) & (1u << 25)) && m->r32(m->tq(q, 0x08)) == 256 * 16);
    const std::vector<Bytes> frames = flow_frames(16, 8);
    for (const Bytes& f : frames) CHECK(nic.transmit(ConstByteView(f.data(), f.size())));
    CHECK(m->sent.size() == frames.size() && m->sent_queue.size() == frames.size());
    uint32_t used = 0;
    for (uint32_t q = 0; q < 4; ++q) if (nic.queue_sent(q) > 0) ++used;
    CHECK(used >= 2);
    // each flow: one queue, frames in the order given (frame[60] counts them)
    for (uint32_t flow = 0; flow < 16; ++flow) {
      int queue = -1, last = -1;
      for (size_t i = 0; i < m->sent.size(); ++i) {
        if (m->sent[i][34] != 0x80 + flow) continue;
        if (queue < 0) queue = static_cast<int>(m->sent_queue[i]);
        CHECK(static_cast<int>(m->sent_queue[i]) == queue && m->sent[i][60] > last);
        last = m->sent[i][60];
      }
      CHECK(queue == static_cast<int>(IgbNetworkInterface::transmit_queue_for(ConstByteView(frames[flow].data(), frames[flow].size()), 4)));
    }
    // non-IP frames go to queue 0
    Bytes arp = frame_of(60, 1);
    arp[12] = 0x08;
    arp[13] = 0x06;
    CHECK(IgbNetworkInterface::transmit_queue_for(ConstByteView(arp.data(), arp.size()), 4) == 0);
    CHECK(IgbNetworkInterface::transmit_queue_for(ConstByteView(frames[3].data(), frames[3].size()), 1) == 0);
    // flow 0's queue stops sending: its ring fills (255 frames), the other flows still go out
    const uint32_t stuck = IgbNetworkInterface::transmit_queue_for(ConstByteView(frames[0].data(), frames[0].size()), 4);
    m->w32(m->tq(stuck, 0x28), m->r32(m->tq(stuck, 0x28)) & ~(1u << 25));
    uint32_t accepted = 0;
    while (accepted < 1000 && nic.transmit(ConstByteView(frames[0].data(), frames[0].size()))) ++accepted;
    CHECK(accepted == IgbNetworkInterface::kTransmitDescriptors - 1);
    size_t other = 16;
    while (other < frames.size() && IgbNetworkInterface::transmit_queue_for(ConstByteView(frames[other].data(), frames[other].size()), 4) == stuck) ++other;
    CHECK(other < frames.size());
    if (other < frames.size()) {
      const size_t before = m->sent.size();
      CHECK(nic.transmit(ConstByteView(frames[other].data(), frames[other].size())) && m->sent.size() == before + 1);
    }
    // segments go by their flow too
    TransmitChecksum r;
    Bytes big = transport_frame(6, 6000, &r);
    CHECK(nic.transmit_segmented(ConstByteView(big.data(), big.size()), r, 1460) ||
          IgbNetworkInterface::transmit_queue_for(ConstByteView(big.data(), big.size()), 4) == stuck);
  }
  CHECK(alloc.dma.empty());
}

// The 82574's MSI-X: a vector per receive queue and one for the other causes, RSS over the two queues, a throttle per queue vector; the
// chipset parts stay on one queue.
static void test_e1000e_msix() {
  TestAllocator alloc;
  UniquePointer<E1000eChip> owned = UniquePointer<E1000eChip>::make(alloc, 0x10D3, false);
  E1000eChip* m = owned.get();
  {
    E1000eNetworkInterface nic(util::move(owned), alloc);
    CHECK(nic.initialize() && nic.queue_count() == 2);
    CHECK((m->r32(0x5008) & (1u << 15)) && m->r32(0x2908) == 256 * 16 && m->r32(0x2918) == 255);  // extended descriptors, queue 1 runs
    CHECK(m->r32(0x5818) == 0);
    uint8_t buffer[2048];
    ReceivedFrame got;
    CHECK(m->deliver(frame_of(90, 1), 0, 0x03 | 0x40 | 0x20) && nic.receive_frame(ByteView(buffer, sizeof buffer), &got) && got.length == 90 &&
          got.transport_checksum_good);  // (the extended write-back read right)
    CHECK(m->deliver(frame_of(90, 1), 0x01) && !nic.receive_frame(ByteView(buffer, sizeof buffer), &got));  // a CRC error: dropped
    g_wakes = 0;
    CHECK(nic.enable_interrupts(&count_wake, nullptr) && nic.msix_vectors() == 3 && m->msix_attached == 3 && nic.offloads().receive_queues == 2);
    CHECK(m->ivar_vector(0) == 0 && m->ivar_vector(4) == 1 && m->ivar_vector(16) == 2);
    CHECK((m->r32(0x18) & ((1u << 24) | (1u << 31))) == ((1u << 24) | (1u << 31)) && !(m->r32(0x18) & (1u << 27)));
    CHECK(m->r32(0x00DC) == ((1u << 20) | (1u << 21)));
    CHECK(m->vector_raised[0] == 1 && m->vector_raised[2] == 1 && g_wakes == 2);  // the self-test: queue 0 and the link
    CHECK((m->r32(0x5818) & 3) == 1 && (m->r32(0x5000) & (1u << 13)));
    for (uint32_t e = 0; e < 128; ++e) CHECK(((m->r32(0x5C00 + 4 * (e / 4)) >> (8 * (e % 4))) & 0xFF) == ((e & 1) ? 0x80u : 0u));
    CHECK(m->r32(0x00E8) == 196 && m->r32(0x00EC) == 196);
    nic.rearm_interrupts();
    // 16 flows over the two queues, one interrupt per queue until rearmed, everything taken
    std::vector<int> before(m->vector_raised);
    const std::vector<Bytes> frames = flow_frames(16, 4);
    for (const Bytes& f : frames) CHECK(m->deliver(f));
    CHECK(m->vector_raised[0] - before[0] == 1 && m->vector_raised[1] - before[1] == 1);
    uint32_t taken = 0;
    while (nic.receive_frame(ByteView(buffer, sizeof buffer), &got)) ++taken;
    CHECK(taken == frames.size() && nic.queue_frames(0) > 0 && nic.queue_frames(1) > 0);
    nic.rearm_interrupts();
    while (nic.receive_frame(ByteView(buffer, sizeof buffer), &got)) {}
    nic.rearm_interrupts();
    const int link = m->vector_raised[2];
    m->signal(1u << 2);
    CHECK(m->vector_raised[2] == link + 1);
    // bulk traffic: both queue throttles go to 4000 a second
    for (int i = 0; i < 40; ++i) m->deliver(frame_of(1514, 6), 0, 3, i & 1);
    while (nic.receive_frame(ByteView(buffer, sizeof buffer), &got)) {}
    nic.rearm_interrupts();
    CHECK(m->r32(0x00E8) == 980 && m->r32(0x00EC) == 980);
    nic.disable_interrupts();
    CHECK(nic.msix_vectors() == 0 && m->r32(0x5818) == 0 && m->r32(0x00E4) == 0 && !m->queue_handler);
    CHECK(m->deliver(frame_of(70, 9)) && nic.receive_frame(ByteView(buffer, sizeof buffer), &got) && got.length == 70);
  }
  // no MSI-X table: MSI, one queue's worth
  UniquePointer<E1000eChip> plain = UniquePointer<E1000eChip>::make(alloc, 0x10D3, false);
  plain->msix_entries = 0;
  E1000eChip* p = plain.get();
  {
    E1000eNetworkInterface nic(util::move(plain), alloc);
    CHECK(nic.initialize() && nic.enable_interrupts(&count_wake, nullptr) && nic.msix_vectors() == 0 && p->handler && nic.offloads().receive_queues == 1);
  }
  // an I219: one queue, legacy descriptors
  UniquePointer<E1000eChip> lom = UniquePointer<E1000eChip>::make(alloc, 0x15BC, true);
  E1000eChip* l = lom.get();
  {
    E1000eNetworkInterface nic(util::move(lom), alloc);
    CHECK(nic.initialize() && nic.queue_count() == 1 && !(l->r32(0x5008) & (1u << 15)) && l->r32(0x2908) == 0);
  }
  CHECK(alloc.dma.empty());
}

// Realtek giant send: segments cut by the chip on the chips that have it, refused on the others.
static void test_rtl_segmentation() {
  TestAllocator alloc;
  for (const auto& c : {std::pair<uint16_t, uint32_t>{0x8168, 0x541}, {0x8125, 0x641}, {0x8168, 0x2C8}, {0x8136, 0x448}, {0x8169, 0x008}}) {
    const bool is_8125 = c.first == 0x8125;
    UniquePointer<RtlChipModel> owned = UniquePointer<RtlChipModel>::make(alloc, c.first, c.second, is_8125);
    RtlChipModel* m = owned.get();
    {
      Rtl8169NetworkInterface nic(util::move(owned), alloc);
      CHECK(nic.initialize());
      const bool tso = nic.chip()->tso;
      CHECK(tso == (c.second == 0x541 || c.second == 0x641));  // (the RTL8168E-VL, RTL8106E and RTL8169 have none)
      if (!tso) {
        CHECK(nic.offloads().max_segmentation_bytes == 0);
        TransmitChecksum r;
        Bytes f = transport_frame(6, 4000, &r);
        CHECK(!nic.transmit_segmented(ConstByteView(f.data(), f.size()), r, 1460));
        continue;
      }
      CHECK(nic.offloads().max_segmentation_bytes == 32000);
      for (uint32_t payload : {1u, 1460u, 1461u, 9000u, 31900u}) {
        TransmitChecksum r;
        Bytes f = transport_frame(6, payload, &r);
        f[47] = 0x19;
        const size_t before = m->sent.size();
        CHECK(nic.transmit_segmented(ConstByteView(f.data(), f.size()), r, 1460));
        const uint32_t expect = (payload + 1459) / 1460;
        CHECK(m->sent.size() == before + expect);
        Bytes joined;
        for (size_t k = before; k < m->sent.size(); ++k) {
          CHECK(m->sent[k].size() <= kEthernetMaxFrame && transport_checksum_ok(m->sent[k]));
          CHECK((m->sent[k][47] & 0x09) == (k + 1 == m->sent.size() ? 0x09 : 0));
          joined.insert(joined.end(), m->sent[k].begin() + 54, m->sent[k].end());
        }
        CHECK(joined == Bytes(f.begin() + 54, f.end()));
      }
      for (uint32_t payload : {1u, 1441u, 20000u}) {  // IPv6 giant send
        TransmitChecksum r6;
        Bytes f6 = transport_frame6(6, payload, &r6);
        const size_t before = m->sent.size();
        CHECK(nic.transmit_segmented(ConstByteView(f6.data(), f6.size()), r6, 1440));
        CHECK(m->sent.size() == before + (payload + 1439) / 1440);
        for (size_t k = before; k < m->sent.size(); ++k) CHECK(transport_checksum_ok(m->sent[k]));
      }
      // what is refused: too large, an MSS that does not fit the descriptor, not TCP
      TransmitChecksum r;
      Bytes big = transport_frame(6, 32000, &r);
      CHECK(!nic.transmit_segmented(ConstByteView(big.data(), big.size()), r, 1460));
      Bytes f = transport_frame(6, 3000, &r);
      CHECK(!nic.transmit_segmented(ConstByteView(f.data(), f.size()), r, 2048));
      TransmitChecksum u;
      Bytes udp = transport_frame(17, 3000, &u);
      CHECK(!nic.transmit_segmented(ConstByteView(udp.data(), udp.size()), u, 1460));
      // the ring with chains and single frames mixed, many times around
      for (int i = 0; i < 300; ++i) {
        Bytes s = transport_frame(6, 20000, &r);
        CHECK(nic.transmit_segmented(ConstByteView(s.data(), s.size()), r, 1460));
        CHECK(nic.transmit(ConstByteView(f.data(), 200)));
      }
      CHECK(m->segmentations >= 300 && nic.statistics().segmented_sends >= 300);
    }
  }
  CHECK(alloc.dma.empty());
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
  test_moderation_classes();
  test_masking_and_throttle();
  test_igb_msix(0x10C9, false, 4);  // 82576 (the IVAR byte per queue)
  test_igb_msix(0x1533, false, 4);  // I210 (two queues per IVAR)
  test_igb_msix(0x15F3, true, 4);   // I225-V
  test_igb_msix(0x1539, false, 1);  // I211: one queue, MSI
  test_segmentation();
  test_igb_transmit_queues();
  test_e1000e_msix();
  test_rtl_segmentation();
  if (g_failed) {
    printf("%d check(s) failed\n", g_failed);
    return 1;
  }
  printf("PciNicTest: all passed\n");
  return 0;
}
