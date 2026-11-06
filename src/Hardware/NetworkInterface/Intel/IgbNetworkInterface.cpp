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
// Queue 0 (igb's E1000_RDBAL(0)... and igc's IGC_RDBAL(0)...: the same offsets).
constexpr uint32_t kRdbal = 0xC000, kRdbah = 0xC004, kRdlen = 0xC008, kSrrctl = 0xC00C, kRdh = 0xC010, kRdt = 0xC018, kRxdctl = 0xC028;
constexpr uint32_t kTdbal = 0xE000, kTdbah = 0xE004, kTdlen = 0xE008, kTdh = 0xE010, kTdt = 0xE018, kTxdctl = 0xE028;
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
constexpr uint32_t kTxTypeData = 3u << 20, kTxExtended = 1u << 29, kTxReportStatus = 1u << 27, kTxInsertCrc = 1u << 25, kTxEndOfPacket = 1u << 24;
constexpr uint32_t kTxPayloadShift = 14, kTxDone = 1u << 0, kTxInsertTransportChecksum = 1u << 9;  // (POPTS.TXSM)
// Transmit context descriptor: MACLEN and IPLEN, the context type, the transport protocol.
constexpr uint32_t kTxTypeContext = 2u << 20, kTxContextTcp = 1u << 11, kTxMacLengthShift = 9;
// IVAR0: queue 0's receive and transmit causes to vector 0 (igb_assign_vector in MSI mode); the 82575 has no IVAR.
constexpr uint32_t kIvarQueue0ToVector0 = 0x8080;

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

  rx_ring_ = allocate_dma_view<ReceiveDescriptor>(allocator_, kReceiveDescriptors, 4096);
  tx_ring_ = allocate_dma_view<TransmitDescriptor>(allocator_, kTransmitDescriptors, 4096);
  buffers_ = allocate_dma_view<uint8_t>(allocator_, uint64_t{kReceiveDescriptors + kTransmitDescriptors} * kBufferSize, 4096);
  if (rx_ring_.empty() || tx_ring_.empty() || buffers_.empty()) return false;
  for (uint32_t i = 0; i < kReceiveDescriptors; ++i) post_receive(i);
  for (uint32_t i = 0; i < kTransmitDescriptors; ++i) tx_ring_[i] = TransmitDescriptor{nic_dma_address(tx_buffer(i)), 0, 0};
  tx_clean_ = 0;
  __atomic_thread_fence(__ATOMIC_RELEASE);

  // Receive queue: disabled while it is set up, enabled, and only then given its buffers (the tail).
  regs().write32(kRxdctl, 0);
  wait_for(regs(), kRxdctl, kQueueEnable, 0, 100, 100);
  const uint64_t rx = nic_dma_address(rx_ring_), tx = nic_dma_address(tx_ring_);
  regs().write32(kRdbal, static_cast<uint32_t>(rx));
  regs().write32(kRdbah, static_cast<uint32_t>(rx >> 32));
  regs().write32(kRdlen, kReceiveDescriptors * sizeof(ReceiveDescriptor));
  regs().write32(kRdh, 0);
  regs().write32(kRdt, 0);
  regs().write32(kSrrctl, kSrrctl2k | kSrrctlAdvancedOneBuffer | kSrrctlDropEnable);
  regs().write32(kRxdctl, kQueueThresholds | kQueueEnable);
  if (!wait_for(regs(), kRxdctl, kQueueEnable, kQueueEnable, 100, 100)) return false;
  regs().write32(kRdt, kReceiveDescriptors - 1);
  rx_next_ = 0;

  regs().write32(kTxdctl, 0);
  wait_for(regs(), kTxdctl, kQueueEnable, 0, 100, 100);
  regs().write32(kTdbal, static_cast<uint32_t>(tx));
  regs().write32(kTdbah, static_cast<uint32_t>(tx >> 32));
  regs().write32(kTdlen, kTransmitDescriptors * sizeof(TransmitDescriptor));
  regs().write32(kTdh, 0);
  regs().write32(kTdt, 0);
  regs().write32(kTxdctl, kQueueThresholds | kQueueEnable);
  if (!wait_for(regs(), kTxdctl, kQueueEnable, kQueueEnable, 100, 100)) return false;
  tx_next_ = 0;

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
      regs().write32(kRxdctl, 0);
      regs().write32(kTxdctl, 0);
      reset();
      regs().clear_bits32(kCtrlExt, kCtrlExtDriverLoaded);
    }
    device_->release();
    claimed_ = false;
  }
  started_ = false;
  release_view(allocator_, rx_ring_);
  release_view(allocator_, tx_ring_);
  release_view(allocator_, buffers_);
}

// The write-back overwrote the addresses: a descriptor goes back to the read format before the device gets it again.
void IgbNetworkInterface::post_receive(uint32_t i) {
  volatile ReceiveDescriptor& d = rx_ring_[i];
  d.packet_address = nic_dma_address(rx_buffer(i));
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

bool IgbNetworkInterface::enable_interrupts(NetworkWake wake, void* context) {
  kernel::ScopedMutex guard(mutex_);
  if (!started_) return false;
  if (model_->kind != IgbKind::I82575) regs().write32(kIvar0, kIvarQueue0ToVector0);
  return interrupts_.enable(*device_, wake, context);
}

// Frames the device has sent give their descriptors back: the data descriptor that ends each one has DD written back (a context descriptor
// in front of it does not).
void IgbNetworkInterface::reclaim_transmitted() {
  while (tx_clean_ != tx_next_) {
    const uint32_t end = tx_frame_end_[tx_clean_];
    volatile TransmitDescriptor& d = tx_ring_[end];
    if (!(d.offload_status & kTxDone)) return;
    tx_clean_ = (end + 1) % kTransmitDescriptors;
  }
}

bool IgbNetworkInterface::transmit(ConstByteView frame) { return transmit_frame(frame, TransmitChecksum{}); }

bool IgbNetworkInterface::transmit_frame(ConstByteView frame, TransmitChecksum checksum) {
  kernel::ScopedMutex guard(mutex_);
  if (!started_ || frame.empty() || frame.size() > kEthernetMaxFrame || (checksum.requested() && !checksum.fits(frame.size()))) {
    ++stats_.send_failures;
    return false;
  }
  // The checksum needs a context descriptor (MACLEN 14, IPLEN up to 511) in front of the data; anything else is finished in software.
  const bool hardware = checksum.requested() && (checksum.tcp() || checksum.udp()) && checksum.start >= kEthernetHeaderSize + 20 &&
                        checksum.start - kEthernetHeaderSize <= 511;
  const uint32_t needed = hardware ? 2 : 1;
  reclaim_transmitted();
  const uint32_t in_flight = (tx_next_ + kTransmitDescriptors - tx_clean_) % kTransmitDescriptors;
  if (kTransmitDescriptors - 1 - in_flight < needed) {  // the device has not sent enough yet
    ++stats_.send_failures;
    return false;
  }
  const uint32_t first = tx_next_;
  if (hardware) {
    volatile TransmitDescriptor& context = tx_ring_[first];
    context.address = (uint64_t{kEthernetHeaderSize} << kTxMacLengthShift) | (checksum.start - kEthernetHeaderSize);  // (VLAN 0, no seed)
    context.command_type_length = kTxTypeContext | kTxExtended | (checksum.tcp() ? kTxContextTcp : 0);
    context.offload_status = 0;  // (no MSS, context index 0)
    tx_next_ = (tx_next_ + 1) % kTransmitDescriptors;
  }
  const uint32_t slot = tx_next_;
  const uint32_t padded = frame.size() < kEthernetMinFrame ? kEthernetMinFrame : static_cast<uint32_t>(frame.size());
  const ByteView buffer = tx_buffer(slot).first(padded);
  buffer.fill(0);
  buffer.copy_from(frame);
  if (checksum.requested() && !hardware && !finish_transmit_checksum(buffer.first(frame.size()), checksum)) {
    tx_next_ = first;
    ++stats_.send_failures;
    return false;
  }
  volatile TransmitDescriptor& d = tx_ring_[slot];
  d.address = nic_dma_address(tx_buffer(slot));
  d.command_type_length = kTxTypeData | kTxExtended | kTxReportStatus | kTxInsertCrc | kTxEndOfPacket | padded;
  d.offload_status = (padded << kTxPayloadShift) | (hardware ? kTxInsertTransportChecksum : 0);  // (and DD clear)
  tx_frame_end_[first] = static_cast<uint8_t>(slot);
  __atomic_thread_fence(__ATOMIC_RELEASE);
  tx_next_ = (slot + 1) % kTransmitDescriptors;
  regs().write32(kTdt, tx_next_);
  ++stats_.frames_sent;
  return true;
}

bool IgbNetworkInterface::receive(ByteView buffer, uint32_t* length) {
  ReceivedFrame frame;
  if (!length || !receive_frame(buffer, &frame)) return false;
  *length = frame.length;
  return true;
}

bool IgbNetworkInterface::receive_frame(ByteView buffer, ReceivedFrame* received) {
  kernel::ScopedMutex guard(mutex_);
  if (!started_ || !received) return false;
  for (uint32_t tries = 0; tries < kReceiveDescriptors; ++tries) {
    volatile ReceiveDescriptor& d = rx_ring_[rx_next_];
    const uint64_t upper = d.header_address;
    const uint32_t status = static_cast<uint32_t>(upper);
    if (!(status & kRxDone)) return false;
    __atomic_thread_fence(__ATOMIC_ACQUIRE);

    const uint32_t frame = static_cast<uint16_t>(upper >> 32);
    const bool usable = !(status & kRxFrameErrors) && (status & kRxEndOfPacket) && frame > 0 && frame <= kBufferSize;
    if (usable) {
      buffer.copy_from(ConstByteView(rx_buffer(rx_next_)).first(frame).at_most(buffer.size()));
      const bool checked = !(status & kRxIgnoreChecksum);
      *received = ReceivedFrame{frame, checked && (status & kRxIpChecksum) && !(status & kRxIpError),
                                checked && (status & (kRxTcpChecksum | kRxUdpChecksum)) && !(status & kRxTcpUdpError)};
      ++stats_.frames_received;
    } else {
      ++stats_.frames_dropped;
    }
    post_receive(rx_next_);
    __atomic_thread_fence(__ATOMIC_RELEASE);
    regs().write32(kRdt, rx_next_);
    rx_next_ = (rx_next_ + 1) % kReceiveDescriptors;
    if (usable) return true;
  }
  return false;
}

}  // namespace hardware
