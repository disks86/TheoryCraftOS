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

#include "Hardware/NetworkInterface/Intel/IgbNetworkInterface.hpp"
#include "Hardware/NetworkInterface/Intel/IntelEthernetRegisters.hpp"
#include "Kernel/Delay.hpp"

namespace hardware {

using namespace intel_eth;

namespace {
// Queue n's registers (igb's E1000_RDBAL(n)... and igc's IGC_RDBAL(n)...: the same offsets).
constexpr uint32_t kRxQueueBase = 0xC000, kTxQueueBase = 0xE000, kQueueStride = 0x40;
constexpr uint32_t kRdbal = 0x00, kRdbah = 0x04, kRdlen = 0x08, kSrrctl = 0x0C, kRdh = 0x10, kRdt = 0x18, kRxdctl = 0x28;
constexpr uint32_t kTdbal = 0x00, kTdbah = 0x04, kTdlen = 0x08, kTdh = 0x10, kTdt = 0x18, kTxdctl = 0x28;
uint32_t rx_reg(uint32_t q, uint32_t reg) { return kRxQueueBase + kQueueStride * q + reg; }
uint32_t tx_reg(uint32_t q, uint32_t reg) { return kTxQueueBase + kQueueStride * q + reg; }
constexpr uint32_t kEemngctl = 0x1010, kEemngctlConfigDone0 = 1u << 18;

constexpr uint32_t kQueueEnable = 1u << 25;
constexpr uint32_t kQueueThresholds = 8 | (1u << 8) | (1u << 16);  // prefetch 8, host 1, write-back 1: every descriptor is written back at once (we poll)
constexpr uint32_t kSrrctl2k = 2, kSrrctlAdvancedOneBuffer = 1u << 25, kSrrctlDropEnable = 1u << 31;

// Receive write-back: status in the low half of the second quadword, length in bits 47:32.
constexpr uint32_t kRxDone = 1u << 0, kRxEndOfPacket = 1u << 1, kRxIgnoreChecksum = 1u << 2, kRxUdpChecksum = 1u << 4, kRxTcpChecksum = 1u << 5,
                   kRxIpChecksum = 1u << 6;
constexpr uint32_t kRxFrameErrors = 0x01000000 | 0x02000000 | 0x04000000 | 0x10000000 | 0x80000000u;  // CE, SE, SEQ, CXE, RXE
constexpr uint32_t kRxTcpUdpError = 0x20000000, kRxIpError = 0x40000000;  // checksum errors: the frame is passed on, software looks again
// Transmit data descriptor (E1000_ADVTXD_*).
constexpr uint32_t kTxTypeData = 3u << 20, kTxExtended = 1u << 29, kTxReportStatus = 1u << 27, kTxInsertCrc = 1u << 25, kTxEndOfPacket = 1u << 24,
                   kTxSegmentation = 1u << 31;
constexpr uint32_t kTxPayloadShift = 14, kTxDone = 1u << 0, kTxInsertTransportChecksum = 1u << 9, kTxInsertIpChecksum = 1u << 8;  // (POPTS.TXSM, IXSM)
// Transmit context descriptor: MACLEN and IPLEN, the context type, IPv4 and the transport protocol, MSS and the TCP header length.
constexpr uint32_t kTxTypeContext = 2u << 20, kTxContextIpv4 = 1u << 10, kTxContextTcp = 1u << 11, kTxMacLengthShift = 9;
constexpr uint32_t kTxMssShift = 16, kTxL4LengthShift = 8;
// IVAR0: queue 0's receive and transmit causes to vector 0 (igb_assign_vector in MSI mode); the 82575 has no IVAR.
constexpr uint32_t kIvarQueue0ToVector0 = 0x8080, kIvarValid = 0x80;

// MSI-X (igb_configure_msix): GPIE, the extended interrupt registers, the vector of the "other" causes, the throttles.
constexpr uint32_t kGpie = 0x1514, kGpieNsicr = 1u << 0, kGpieMsix = 1u << 4, kGpieEiame = 1u << 30, kGpiePba = 1u << 31;
constexpr uint32_t kEics = 0x1520, kEims = 0x1524, kEiac = 0x152C, kEiam = 0x1530, kIvarMisc = 0x1740, kEitr0 = 0x1680;
constexpr uint32_t kEitrCountIgnore = 1u << 31;
// RSS (igb_setup_mrqc): MRQC, the redirection table, the hash key; RXCSUM.PCSD puts the hash in the descriptor.
constexpr uint32_t kMrqc = 0x5818, kReta = 0x5C00, kRssKey = 0x5C80, kRxcsumPcsd = 1u << 13;
constexpr uint32_t kMrqcRss = 0x2, kMrqcIpv4Tcp = 1u << 16, kMrqcIpv4 = 1u << 17, kMrqcIpv4Udp = 1u << 22;
// The Toeplitz key of Microsoft's RSS verification suite (any key works; this one has published test vectors).
constexpr uint8_t kRssKeyBytes[40] = {0x6d, 0x5a, 0x56, 0xda, 0x25, 0x5b, 0x0e, 0xc2, 0x41, 0x67, 0x25, 0x3d, 0x43, 0xa3, 0x8f, 0xb0, 0xd0, 0xca, 0x2b, 0xcb,
                                      0xae, 0x7b, 0x30, 0xb4, 0x77, 0xcb, 0x2d, 0xa3, 0x80, 0x30, 0xf2, 0x0c, 0x6a, 0x42, 0xb7, 0x3b, 0xbe, 0xac, 0x01, 0xfa};

using K = IgbKind;
constexpr IgbModel kModels[] = {
    {0x10A7, K::I82575, "Intel 82575EB"}, {0x10A9, K::I82575, "Intel 82575EB"}, {0x10D6, K::I82575, "Intel 82575GB"},
    {0x10C9, K::I82576, "Intel 82576"},   {0x10E6, K::I82576, "Intel 82576"},   {0x10E7, K::I82576, "Intel 82576"},   {0x10E8, K::I82576, "Intel 82576"},
    {0x1526, K::I82576, "Intel 82576"},   {0x150A, K::I82576, "Intel 82576NS"}, {0x1518, K::I82576, "Intel 82576NS"}, {0x150D, K::I82576, "Intel 82576"},
    {0x150E, K::I82580, "Intel 82580"},   {0x150F, K::I82580, "Intel 82580"},   {0x1510, K::I82580, "Intel 82580"},   {0x1511, K::I82580, "Intel 82580"},
    {0x1516, K::I82580, "Intel 82580"},   {0x1527, K::I82580, "Intel 82580"},
    {0x1521, K::I350, "Intel I350"},      {0x1522, K::I350, "Intel I350"},      {0x1523, K::I350, "Intel I350"},      {0x1524, K::I350, "Intel I350"},
    {0x1F40, K::I354, "Intel I354"},      {0x1F41, K::I354, "Intel I354"},      {0x1F45, K::I354, "Intel I354"},
    {0x1533, K::I210, "Intel I210"},      {0x1534, K::I210, "Intel I210"},      {0x1535, K::I210, "Intel I210-IT"},   {0x1536, K::I210, "Intel I210"},
    {0x1537, K::I210, "Intel I210"},      {0x1538, K::I210, "Intel I210"},      {0x157B, K::I210, "Intel I210"},      {0x157C, K::I210, "Intel I210"},
    {0x1539, K::I211, "Intel I211"},
    // igc
    {0x15F2, K::I225, "Intel I225-LM"},   {0x15F3, K::I225, "Intel I225-V"},    {0x15F8, K::I225, "Intel I225-I"},    {0x15F7, K::I225, "Intel I220-V"},
    {0x3100, K::I225, "Intel I225-K"},    {0x3101, K::I225, "Intel I225-K"},    {0x5502, K::I225, "Intel I225-LMvP"}, {0x0D9F, K::I225, "Intel I225-IT"},
    {0x15FD, K::I225, "Intel I225"},      {0x125B, K::I226, "Intel I226-LM"},   {0x125C, K::I226, "Intel I226-V"},    {0x125D, K::I226, "Intel I226-IT"},
    {0x125E, K::I226, "Intel I221-V"},    {0x125F, K::I226, "Intel I226"},      {0x3102, K::I226, "Intel I226-K"},    {0x5503, K::I226, "Intel I226-LMvP"},
};
}  // namespace

const IgbModel* igb_find_model(uint16_t device_id) {
  for (const IgbModel& model : kModels)
    if (model.device_id == device_id) return &model;
  return nullptr;
}

uint32_t igb_receive_queues(IgbKind kind) {
  switch (kind) {
    case IgbKind::I82575:  // (MSI-X there needs MSIXBM, not done: one queue)
    case IgbKind::I211:    // (igb leaves RSS off on the I211)
      return 1;
    default:
      return IgbNetworkInterface::kMaxQueues;
  }
}

bool IgbNetworkInterface::reset() {
  regs().write32(kImc, 0xFFFFFFFFu);
  regs().write32(kEimc, 0xFFFFFFFFu);
  stop_dma(regs());
  const uint32_t bit = igc() ? kCtrlDeviceReset : kCtrlReset;
  regs().set_bits32(kCtrl, bit);
  delay_us(igc() ? 25000 : 10000);
  if (!wait_for(regs(), kCtrl, bit, 0, 100, 1000)) return false;
  // The NVM (or the I210's iNVM) is read again after the reset: the station address, the PHY's configuration.
  wait_for(regs(), kEecd, kEecdAutoReadDone, kEecdAutoReadDone, 100, 100);
  if (model_->kind >= IgbKind::I210) wait_for(regs(), kEemngctl, kEemngctlConfigDone0, kEemngctlConfigDone0, 100, 100);
  regs().write32(kImc, 0xFFFFFFFFu);
  regs().write32(kEimc, 0xFFFFFFFFu);
  regs().read32(kIcr);
  return true;
}

// The internal PHY (address 1) negotiates again, powered up and not isolated, as igb and igc do when they set up a copper link. Some PHYs (and
// QEMU's 82576) only bring the link up after that; on the others it costs the few seconds a negotiation takes. A part whose PHY does not
// answer (SGMII or SerDes ports) keeps the link it has.
void IgbNetworkInterface::restart_autonegotiation() {
  uint16_t control = 0;
  if (!mdic_read(regs(), 1, kPhyControl, &control)) return;
  autonegotiation_restarted_ =
      mdic_write(regs(), 1, kPhyControl, static_cast<uint16_t>((control & ~(kPhyPowerDown | kPhyIsolate)) | kPhyAutoNegEnable | kPhyRestartAutoNeg));
}

// A receive queue: disabled while it is set up, enabled, and only then given its buffers (the tail).
bool IgbNetworkInterface::start_receive_queue(uint32_t q) {
  ReceiveQueue& queue = rx_[q];
  queue.ring = allocate_dma_view<ReceiveDescriptor>(allocator_, kReceiveDescriptors, 4096);
  queue.buffers = allocate_dma_view<uint8_t>(allocator_, uint64_t{kReceiveDescriptors} * kBufferSize, 4096);
  if (queue.ring.empty() || queue.buffers.empty()) return false;
  for (uint32_t i = 0; i < kReceiveDescriptors; ++i) post_receive(queue, i);
  __atomic_thread_fence(__ATOMIC_RELEASE);
  regs().write32(rx_reg(q, kRxdctl), 0);
  wait_for(regs(), rx_reg(q, kRxdctl), kQueueEnable, 0, 100, 100);
  const uint64_t rx = nic_dma_address(queue.ring);
  regs().write32(rx_reg(q, kRdbal), static_cast<uint32_t>(rx));
  regs().write32(rx_reg(q, kRdbah), static_cast<uint32_t>(rx >> 32));
  regs().write32(rx_reg(q, kRdlen), kReceiveDescriptors * sizeof(ReceiveDescriptor));
  regs().write32(rx_reg(q, kRdh), 0);
  regs().write32(rx_reg(q, kRdt), 0);
  regs().write32(rx_reg(q, kSrrctl), kSrrctl2k | kSrrctlAdvancedOneBuffer | kSrrctlDropEnable);
  regs().write32(rx_reg(q, kRxdctl), kQueueThresholds | kQueueEnable);
  if (!wait_for(regs(), rx_reg(q, kRxdctl), kQueueEnable, kQueueEnable, 100, 100)) return false;
  regs().write32(rx_reg(q, kRdt), kReceiveDescriptors - 1);
  queue.next = 0;
  return true;
}

bool IgbNetworkInterface::start_transmit_queue(uint32_t q) {
  TransmitQueue& queue = tx_[q];
  queue.ring = allocate_dma_view<TransmitDescriptor>(allocator_, kTransmitDescriptors, 4096);
  queue.buffers = allocate_dma_view<uint8_t>(allocator_, uint64_t{kTransmitDescriptors} * kBufferSize, 4096);
  if (queue.ring.empty() || queue.buffers.empty()) return false;
  for (uint32_t i = 0; i < kTransmitDescriptors; ++i) queue.ring[i] = TransmitDescriptor{nic_dma_address(tx_buffer(queue, i)), 0, 0};
  queue.next = queue.clean = 0;
  __atomic_thread_fence(__ATOMIC_RELEASE);
  regs().write32(tx_reg(q, kTxdctl), 0);
  wait_for(regs(), tx_reg(q, kTxdctl), kQueueEnable, 0, 100, 100);
  const uint64_t tx = nic_dma_address(queue.ring);
  regs().write32(tx_reg(q, kTdbal), static_cast<uint32_t>(tx));
  regs().write32(tx_reg(q, kTdbah), static_cast<uint32_t>(tx >> 32));
  regs().write32(tx_reg(q, kTdlen), kTransmitDescriptors * sizeof(TransmitDescriptor));
  regs().write32(tx_reg(q, kTdh), 0);
  regs().write32(tx_reg(q, kTdt), 0);
  regs().write32(tx_reg(q, kTxdctl), kQueueThresholds | kQueueEnable);
  return wait_for(regs(), tx_reg(q, kTxdctl), kQueueEnable, kQueueEnable, 100, 100);
}

uint32_t IgbNetworkInterface::transmit_queue_for(ConstByteView frame, uint32_t queues) {
  if (queues < 2 || frame.size() < kEthernetHeaderSize + 20 || frame[12] != 0x08 || frame[13] != 0x00) return 0;
  ByteReader in(frame.dropped(kEthernetHeaderSize + 12));
  uint32_t hash = in.be32() ^ in.be32();  // the addresses
  const uint32_t ip_header = (frame[kEthernetHeaderSize] & 0x0F) * 4u, protocol = frame[kEthernetHeaderSize + 9];
  if ((protocol == 6 || protocol == 17) && frame.size() >= kEthernetHeaderSize + ip_header + 4) {
    ByteReader ports(frame.dropped(kEthernetHeaderSize + ip_header));
    hash ^= ports.be32();
  }
  hash ^= hash >> 16;
  hash *= 0x45D9F3Bu;
  hash ^= hash >> 16;
  return hash % queues;
}

bool IgbNetworkInterface::initialize() {
  if (started_ || !device_) return false;
  model_ = igb_find_model(device_->device_id());
  if (!model_ || device_->vendor_id() != 0x8086) return false;
  if (!device_->claim()) return false;
  claimed_ = true;
  if (regs().read32(kStatus) == 0xFFFFFFFFu) return false;

  MacAddress before{};
  const bool had_mac = read_mac(regs(), &before);
  if (!reset()) return false;
  if (!read_mac(regs(), &mac_)) {
    if (!had_mac) return false;
    mac_ = before;
    write_mac(regs(), mac_);
  }
  for (uint32_t i = 0; i < 128; ++i) regs().write32(kMta + 4 * i, 0);

  uint32_t ctrl = regs().read32(kCtrl);
  ctrl |= kCtrlSetLinkUp;
  ctrl &= ~(kCtrlLinkReset | kCtrlForceSpeed | kCtrlForceDuplex | kCtrlPhyReset | kCtrlGioMasterDisable);
  regs().write32(kCtrl, ctrl);
  regs().set_bits32(kCtrlExt, kCtrlExtDriverLoaded);
  restart_autonegotiation();

  // Every queue runs from the start; until RSS is switched on (with MSI-X) everything arrives on queue 0.
  regs().write32(kMrqc, 0);
  queues_ = igb_receive_queues(model_->kind);
  for (uint32_t q = 0; q < queues_; ++q)
    if (!start_receive_queue(q)) return false;
  rx_turn_ = 0;

  for (uint32_t q = 0; q < queues_; ++q)
    if (!start_transmit_queue(q)) return false;

  regs().set_bits32(kRxcsum, kRxcsumIp | kRxcsumTcpUdp);
  receive_control_ = kRctlEnable | kRctlBroadcast | kRctlStripCrc;
  regs().write32(kRctl, receive_control_);
  regs().write32(kTctl, kTctlEnable | kTctlPadShortPackets | kTctlRetransmitLateCollision | kTctlCollisionThreshold | kTctlCollisionDistance);
  started_ = true;
  return true;
}

void IgbNetworkInterface::shutdown() {
  disable_interrupts();
  if (claimed_) {
    started_ = false;
    if (model_ && regs().read32(kStatus) != 0xFFFFFFFFu) {
      for (uint32_t q = 0; q < queues_; ++q) regs().write32(rx_reg(q, kRxdctl), 0);
      for (uint32_t q = 0; q < queues_; ++q) regs().write32(tx_reg(q, kTxdctl), 0);
      reset();
      regs().clear_bits32(kCtrlExt, kCtrlExtDriverLoaded);
    }
    device_->release();
    claimed_ = false;
  }
  started_ = false;
  for (ReceiveQueue& queue : rx_) {
    release_view(allocator_, queue.ring);
    release_view(allocator_, queue.buffers);
  }
  for (TransmitQueue& queue : tx_) {
    release_view(allocator_, queue.ring);
    release_view(allocator_, queue.buffers);
  }
}

// The write-back overwrote the addresses: a descriptor goes back to the read format before the device gets it again.
void IgbNetworkInterface::post_receive(ReceiveQueue& queue, uint32_t i) {
  volatile ReceiveDescriptor& d = queue.ring[i];
  d.packet_address = nic_dma_address(rx_buffer(queue, i));
  d.header_address = 0;
}

bool IgbNetworkInterface::link_up() const { return started_ && (regs().read32(kStatus) & kStatusLinkUp); }

uint32_t IgbNetworkInterface::link_speed_mbps() const { return started_ ? status_speed_mbps(regs().read32(kStatus), igc()) : 0; }

LinkDuplex IgbNetworkInterface::link_duplex() const { return started_ ? status_duplex(regs().read32(kStatus)) : LinkDuplex::Unknown; }

bool IgbNetworkInterface::accept_all_multicast(bool enable) {
  kernel::ScopedMutex guard(mutex_);
  if (!started_) return false;
  receive_control_ = enable ? (receive_control_ | kRctlMulticastPromiscuous) : (receive_control_ & ~kRctlMulticastPromiscuous);
  regs().write32(kRctl, receive_control_);
  return enable;
}

NetworkStatistics IgbNetworkInterface::statistics() const {
  NetworkStatistics s = stats_;
  s.interrupts = interrupts_.count() + __atomic_load_n(&msix_interrupts_, __ATOMIC_ACQUIRE);
  s.interrupt_rate = interrupts_enabled() ? moderator_.interrupts_per_second() : 0;
  return s;
}

// ---- interrupts -------------------------------------------------------------------------------------------------------------------------------------

// Vector v: v's receive queue's cause and its EIMS bit (EIAC cleared one, EIAM masked the other: the vector is quiet until rearmed).
// The last vector has the "other" causes, the link: ICR says which (and is cleared by the read).
void IgbNetworkInterface::on_queue_interrupt(void* self, uint32_t vector) {
  IgbNetworkInterface* nic = static_cast<IgbNetworkInterface*>(self);
  if (vector == __atomic_load_n(&nic->msix_vectors_, __ATOMIC_ACQUIRE) - 1) nic->regs().read32(kIcr);
  __atomic_fetch_add(&nic->msix_interrupts_, 1, __ATOMIC_ACQ_REL);
  const NetworkWake wake = __atomic_load_n(&nic->wake_, __ATOMIC_ACQUIRE);
  if (wake) wake(__atomic_load_n(&nic->wake_context_, __ATOMIC_ACQUIRE));
}

// Queue q's receive cause to MSI-X entry `vector`: the 82576 has a byte per queue at IVAR(q & 7), the later parts two queues per register.
void IgbNetworkInterface::write_ivar(uint32_t queue, uint32_t vector) {
  const uint32_t index = model_->kind == IgbKind::I82576 ? (queue & 7) : (queue >> 1);
  const uint32_t shift = model_->kind == IgbKind::I82576 ? ((queue & 8) << 1) : ((queue & 1) << 4);
  const uint32_t reg = kIvar0 + 4 * index;
  regs().write32(reg, (regs().read32(reg) & ~(0xFFu << shift)) | ((vector | kIvarValid) << shift));
}

void IgbNetworkInterface::configure_rss() {
  for (uint32_t i = 0; i < 10; ++i)
    regs().write32(kRssKey + 4 * i, kRssKeyBytes[4 * i] | (uint32_t{kRssKeyBytes[4 * i + 1]} << 8) | (uint32_t{kRssKeyBytes[4 * i + 2]} << 16) |
                                        (uint32_t{kRssKeyBytes[4 * i + 3]} << 24));
  for (uint32_t i = 0; i < 128; i += 4) {  // entry i in byte i % 4: the queues in turn
    uint32_t value = 0;
    for (uint32_t j = 0; j < 4; ++j) value |= ((i + j) % queues_) << (8 * j);
    regs().write32(kReta + i, value);
  }
  regs().set_bits32(kRxcsum, kRxcsumPcsd);
  regs().write32(kMrqc, kMrqcRss | kMrqcIpv4 | kMrqcIpv4Tcp | kMrqcIpv4Udp);
}

void IgbNetworkInterface::write_throttle() {
  const uint32_t interval = InterruptModerator::intel_interval(moderator_.latency());
  const uint32_t value = model_->kind == IgbKind::I82575 ? (interval | (interval << 16)) : (interval | kEitrCountIgnore);
  const uint32_t vectors = msix_vectors_ ? msix_vectors_ - 1 : 1;
  for (uint32_t v = 0; v < vectors; ++v) regs().write32(kEitr0 + 4 * v, value);
}

bool IgbNetworkInterface::enable_msix(NetworkWake wake, void* context) {
  if (queues_ < 2) return false;  // (one queue: MSI does the same with less)
  const uint32_t vectors = queues_ + 1;
  __atomic_store_n(&wake_context_, context, __ATOMIC_RELEASE);
  __atomic_store_n(&wake_, wake, __ATOMIC_RELEASE);
  __atomic_store_n(&msix_vectors_, vectors, __ATOMIC_RELEASE);
  if (device_->attach_interrupts(vectors, &IgbNetworkInterface::on_queue_interrupt, this) != vectors) {
    __atomic_store_n(&msix_vectors_, 0u, __ATOMIC_RELEASE);
    __atomic_store_n(&wake_, static_cast<NetworkWake>(nullptr), __ATOMIC_RELEASE);
    return false;
  }
  regs().write32(kImc, 0xFFFFFFFFu);
  regs().write32(kEimc, 0xFFFFFFFFu);
  regs().write32(kGpie, kGpieMsix | kGpiePba | kGpieEiame | kGpieNsicr);
  for (uint32_t q = 0; q < queues_; ++q) write_ivar(q, q);
  regs().write32(kIvarMisc, (queues_ | kIvarValid) << 8);
  const uint32_t all = (1u << vectors) - 1;
  regs().write32(kEiac, all);  // a vector's cause clears itself...
  regs().write32(kEiam, all);  // ...and masks the vector until rearm_interrupts()
  write_throttle();
  configure_rss();
  regs().write32(kIms, kIntLinkChange);
  regs().write32(kEims, all);
  // The route is tested on the link vector and on queue 0's; both must come through within 20 ms.
  const uint64_t before = __atomic_load_n(&msix_interrupts_, __ATOMIC_ACQUIRE);
  regs().write32(kEics, (1u << queues_) | 1u);
  for (int i = 0; i < 200 && __atomic_load_n(&msix_interrupts_, __ATOMIC_ACQUIRE) < before + 2; ++i) delay_us(100);
  if (__atomic_load_n(&msix_interrupts_, __ATOMIC_ACQUIRE) >= before + 2) {
    regs().write32(kEims, all);
    return true;
  }
  disable_interrupts();
  return false;
}

bool IgbNetworkInterface::enable_interrupts(NetworkWake wake, void* context) {
  kernel::ScopedMutex guard(mutex_);
  if (!started_ || !wake) return false;
  if (interrupts_enabled()) return true;
  if (model_->kind != IgbKind::I82575 && enable_msix(wake, context)) return true;
  if (model_->kind != IgbKind::I82575) regs().write32(kIvar0, kIvarQueue0ToVector0);
  if (!interrupts_.enable(*device_, wake, context)) return false;
  write_throttle();
  return true;
}

void IgbNetworkInterface::disable_interrupts() {
  if (msix_vectors_) {
    if (claimed_) {
      regs().write32(kEimc, 0xFFFFFFFFu);
      regs().write32(kImc, 0xFFFFFFFFu);
      regs().write32(kGpie, 0);
      regs().write32(kMrqc, 0);  // everything to queue 0 again (the other queues are still taken from)
    }
    device_->detach_interrupt();
    __atomic_store_n(&msix_vectors_, 0u, __ATOMIC_RELEASE);
    __atomic_store_n(&wake_, static_cast<NetworkWake>(nullptr), __ATOMIC_RELEASE);
    return;
  }
  interrupts_.disable(*device_, claimed_);
}

void IgbNetworkInterface::rearm_interrupts() {
  kernel::ScopedMutex guard(mutex_);
  if (!started_ || !interrupts_enabled()) return;
  if (moderator_.update()) write_throttle();
  if (msix_vectors_) regs().write32(kEims, (1u << msix_vectors_) - 1);
  else interrupts_.rearm();
}

// ---- transmitting -----------------------------------------------------------------------------------------------------------------------------------

// Frames the device has sent on a queue give their descriptors back: the data descriptor that ends each one has DD written back (a context descriptor
// in front of it, and the descriptors of a chain before the last, do not).
void IgbNetworkInterface::reclaim_transmitted(TransmitQueue& queue) {
  while (queue.clean != queue.next) {
    const uint32_t end = queue.frame_end[queue.clean];
    volatile TransmitDescriptor& d = queue.ring[end];
    if (!(d.offload_status & kTxDone)) return;
    queue.clean = (end + 1) % kTransmitDescriptors;
  }
}

bool IgbNetworkInterface::transmit(ConstByteView frame) { return transmit_frame(frame, TransmitChecksum{}); }

bool IgbNetworkInterface::transmit_frame(ConstByteView frame, TransmitChecksum checksum) {
  kernel::ScopedMutex guard(mutex_);
  return send(frame, checksum, 0);
}

bool IgbNetworkInterface::transmit_segmented(ConstByteView frame, TransmitChecksum checksum, uint16_t mss) {
  kernel::ScopedMutex guard(mutex_);
  if (mss == 0) {
    ++stats_.send_failures;
    return false;
  }
  return send(frame, checksum, mss);
}

// One frame, or (mss != 0) one TCP segment the device cuts into frames: a context descriptor when a checksum or segmentation is asked for,
// then the data in as many 2 KiB buffers as it takes, the last descriptor with EOP and RS.
bool IgbNetworkInterface::send(ConstByteView frame, TransmitChecksum checksum, uint16_t mss) {
  const bool segment = mss != 0;
  TsoLayout layout;
  bool fits = started_ && !frame.empty();
  if (segment) fits = fits && frame.size() <= kMaxSegmentation && tso_layout(frame, checksum, &layout) && layout.tcp_start - kEthernetHeaderSize <= 511;
  else fits = fits && frame.size() <= kEthernetMaxFrame && (!checksum.requested() || checksum.fits(frame.size()));
  if (!fits) {
    ++stats_.send_failures;
    return false;
  }
  // The checksum needs a context descriptor (MACLEN 14, IPLEN up to 511) in front of the data; anything else is finished in software.
  const bool hardware = segment || (checksum.requested() && (checksum.tcp() || checksum.udp()) && checksum.start >= kEthernetHeaderSize + 20 &&
                                    checksum.start - kEthernetHeaderSize <= 511);
  // Short frames are padded by the controller (TCTL.PSP), after the checksum: padding them here would put the padding in the TCP/UDP length
  // of a controller that works the checksum out from the frame's length (QEMU's does).
  const uint32_t length = static_cast<uint32_t>(frame.size());
  const uint32_t data_descriptors = (length + kBufferSize - 1) / kBufferSize;
  const uint32_t needed = data_descriptors + (hardware ? 1 : 0);
  const uint32_t q = transmit_queue_for(frame, queues_);
  TransmitQueue& queue = tx_[q];
  reclaim_transmitted(queue);
  const uint32_t in_flight = (queue.next + kTransmitDescriptors - queue.clean) % kTransmitDescriptors;
  if (kTransmitDescriptors - 1 - in_flight < needed) {  // the device has not sent enough yet
    ++stats_.send_failures;
    return false;
  }

  const uint32_t first = queue.next;
  uint32_t slot = first;
  if (hardware) {
    volatile TransmitDescriptor& context = queue.ring[slot];
    context.address = (uint64_t{kEthernetHeaderSize} << kTxMacLengthShift) | (checksum.start - kEthernetHeaderSize);  // (VLAN 0, no seed)
    context.command_type_length = kTxTypeContext | kTxExtended | (checksum.tcp() ? kTxContextTcp : 0) | (segment ? kTxContextIpv4 : 0);
    context.offload_status = segment ? ((uint32_t{mss} << kTxMssShift) | (static_cast<uint32_t>(layout.header_length - layout.tcp_start) << kTxL4LengthShift)) : 0;
    slot = (slot + 1) % kTransmitDescriptors;
  }
  const uint32_t first_data = slot;
  for (uint32_t offset = 0, n = 0; n < data_descriptors; ++n) {
    const uint32_t piece = length - offset < kBufferSize ? length - offset : kBufferSize;
    const ByteView buffer = tx_buffer(queue, slot).first(piece);
    buffer.copy_from(frame.subview(offset, piece));
    if (n == 0) {
      if (segment && !prepare_tso_headers(buffer, layout)) return false;  // (cannot fail: the layout was checked)
      if (!hardware && checksum.requested() && !finish_transmit_checksum(buffer.first(frame.size()), checksum)) {
        ++stats_.send_failures;
        return false;
      }
    }
    const bool last = n + 1 == data_descriptors;
    volatile TransmitDescriptor& d = queue.ring[slot];
    d.address = nic_dma_address(tx_buffer(queue, slot));
    d.command_type_length = kTxTypeData | kTxExtended | kTxInsertCrc | (segment ? kTxSegmentation : 0) | (last ? kTxEndOfPacket | kTxReportStatus : 0) | piece;
    d.offload_status = slot != first_data ? 0
                       : ((segment ? layout.payload : length) << kTxPayloadShift) | (hardware ? kTxInsertTransportChecksum : 0) |
                             (segment ? kTxInsertIpChecksum : 0);  // (and DD clear)
    if (last) queue.frame_end[first] = static_cast<uint8_t>(slot);
    offset += piece;
    slot = (slot + 1) % kTransmitDescriptors;
  }
  __atomic_thread_fence(__ATOMIC_RELEASE);
  queue.next = slot;
  regs().write32(tx_reg(q, kTdt), queue.next);
  ++queue.sent;
  ++stats_.frames_sent;
  if (segment) ++stats_.segmented_sends;
  moderator_.account(1, length);
  return true;
}

// ---- receiving --------------------------------------------------------------------------------------------------------------------------------------

bool IgbNetworkInterface::receive(ByteView buffer, uint32_t* length) {
  ReceivedFrame frame;
  if (!length || !receive_frame(buffer, &frame)) return false;
  *length = frame.length;
  return true;
}

bool IgbNetworkInterface::receive_frame(ByteView buffer, ReceivedFrame* received) {
  kernel::ScopedMutex guard(mutex_);
  if (!started_ || !received) return false;
  for (uint32_t k = 0; k < queues_; ++k) {  // the queues in turn, so a busy one does not starve the others
    const uint32_t q = (rx_turn_ + k) % queues_;
    if (receive_from(q, buffer, received)) {
      rx_turn_ = (q + 1) % queues_;
      return true;
    }
  }
  return false;
}

bool IgbNetworkInterface::receive_from(uint32_t q, ByteView buffer, ReceivedFrame* received) {
  ReceiveQueue& queue = rx_[q];
  for (uint32_t tries = 0; tries < kReceiveDescriptors; ++tries) {
    volatile ReceiveDescriptor& d = queue.ring[queue.next];
    const uint64_t upper = d.header_address;
    const uint32_t status = static_cast<uint32_t>(upper);
    if (!(status & kRxDone)) return false;
    __atomic_thread_fence(__ATOMIC_ACQUIRE);

    const uint32_t frame = static_cast<uint16_t>(upper >> 32);
    const bool usable = !(status & kRxFrameErrors) && (status & kRxEndOfPacket) && frame > 0 && frame <= kBufferSize;
    if (usable) {
      buffer.copy_from(ConstByteView(rx_buffer(queue, queue.next)).first(frame).at_most(buffer.size()));
      const bool checked = !(status & kRxIgnoreChecksum);
      *received = ReceivedFrame{frame, checked && (status & kRxIpChecksum) && !(status & kRxIpError),
                                checked && (status & (kRxTcpChecksum | kRxUdpChecksum)) && !(status & kRxTcpUdpError)};
      ++stats_.frames_received;
      ++queue.frames;
      moderator_.account(1, frame);
    } else {
      ++stats_.frames_dropped;
    }
    post_receive(queue, queue.next);
    __atomic_thread_fence(__ATOMIC_RELEASE);
    regs().write32(rx_reg(q, kRdt), queue.next);
    queue.next = (queue.next + 1) % kReceiveDescriptors;
    if (usable) return true;
  }
  return false;
}

}  // namespace hardware
