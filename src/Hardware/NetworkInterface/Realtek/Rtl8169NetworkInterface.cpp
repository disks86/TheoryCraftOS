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

#include "Hardware/NetworkInterface/Realtek/Rtl8169NetworkInterface.hpp"
#include "Hardware/NetworkInterface/TcpSegmentation.hpp"
#include "Kernel/Delay.hpp"

namespace hardware {

namespace {
// Registers (r8169_main.c, enum rtl_registers / rtl8125_registers).
constexpr uint32_t kIdr0 = 0x00, kMar0 = 0x08, kTxDescLow = 0x20, kTxDescHigh = 0x24, kChipCmd = 0x37, kTxPoll = 0x38, kIntrMask = 0x3C, kIntrStatus = 0x3E;
constexpr uint32_t kIntrMask8125 = 0x38, kIntrStatus8125 = 0x3C, kTxPoll8125 = 0x90;
constexpr uint32_t kTxConfig = 0x40, kRxConfig = 0x44, kCfg9346 = 0x50, kPhyStatus = 0x6C, kRxMaxSize = 0xDA, kCPlusCmd = 0xE0, kIntrMitigate = 0xE2;
constexpr uint32_t kRxDescLow = 0xE4, kRxDescHigh = 0xE8, kMaxTxPacketSize = 0xEC, kMisc = 0xF0;

constexpr uint8_t kCmdReset = 0x10, kCmdRxEnable = 0x08, kCmdTxEnable = 0x04;
constexpr uint8_t kTxPollNormal = 0x40, kTxPollSoftwareInterrupt = 0x01;
// IntrStatus / IntrMask
constexpr uint32_t kIntRxOk = 0x0001, kIntRxError = 0x0002, kIntRxOverflow = 0x0010, kIntLinkChange = 0x0020, kIntRxFifoOverflow = 0x0040,
                   kIntSoftware = 0x0100;
constexpr uint32_t kInterruptMask = kIntRxOk | kIntRxError | kIntRxOverflow | kIntLinkChange | kIntRxFifoOverflow | kIntSoftware;
constexpr uint8_t kCfg9346Unlock = 0xC0, kCfg9346Lock = 0x00;
constexpr uint32_t kAcceptBroadcast = 0x08, kAcceptMulticast = 0x04, kAcceptMyPhys = 0x02, kAcceptAllPhys = 0x01, kAcceptMask = 0x3F;
constexpr uint32_t kTxDmaBurst = 7u << 8, kTxInterFrameGap = 3u << 24, kTxAutoFifo = 1u << 7;
constexpr uint32_t kMiscRxdvGated = 1u << 19;
constexpr uint16_t kCPlusRxChecksum = 1u << 5, kCPlusRxVlan = 1u << 6;
constexpr uint8_t kNoEarlyTx = 0x3F;  // (also the largest transmit packet, in 128-byte units, on the RTL8168)

// PHYstatus
constexpr uint16_t kPhyFullDuplex = 0x01, kPhyLink = 0x02, kPhy10 = 0x04, kPhy100 = 0x08, kPhy1000 = 0x10, kPhy2500 = 0x400, kPhy5000 = 0x1000;

// Descriptor opts1
constexpr uint32_t kDescOwn = 1u << 31, kRingEnd = 1u << 30, kFirstFrag = 1u << 29, kLastFrag = 1u << 28, kRxError = 1u << 21, kLengthMask = 0x3FFF;
// Receive checksums (opts1): the protocol the chip found (PID1:PID0 = UDP 10, TCP 01, other IP 11) and what failed.
constexpr uint32_t kRxProtoUdp = 1u << 18, kRxProtoTcp = 1u << 17, kRxProtoMask = kRxProtoUdp | kRxProtoTcp, kRxIpFail = 1u << 16,
                   kRxUdpFail = 1u << 15, kRxTcpFail = 1u << 14;
// Transmit checksums: the RTL8169 family in opts1 (TD0), every later chip in opts2 (TD1) with the transport header's offset.
constexpr uint32_t kTd0Tcp = 1u << 16, kTd0Udp = 1u << 17, kTd0Ip = 1u << 18;
constexpr uint32_t kTd1Ipv4 = 1u << 29, kTd1Tcp = 1u << 30, kTd1Udp = 1u << 31, kTd1TransportShift = 18, kTd1TransportMax = 0x3FF;
// Segmentation ("giant send", r8169's rtl8169_tso_csum_v2): opts1 says IPv4 and where the TCP header starts (7 bits), opts2 the MSS (11 bits).
constexpr uint32_t kTd1GiantSendIpv4 = 1u << 26, kTd1GiantTcpShift = 18, kTd1GiantTcpMax = 0x7F, kTd1MssShift = 18, kTd1MssMax = 0x7FF;
// IPv6 (r8169: TD1_IPv6_CS, TD1_GTSENV6; the chips without the v2 descriptor have no IPv6 offload).
constexpr uint32_t kTd1Ipv6 = 1u << 28, kTd1GiantSendIpv6 = 1u << 25;
constexpr uint32_t kCrcLength = 4;

constexpr uint32_t kRxFifoThreshold = 7u << 13, kRxDmaBurst = 7u << 8, kRx128Int = 1u << 15, kRxMulti = 1u << 14, kRxEarlyOff = 1u << 11, kRxFetch8125 = 8u << 27;
constexpr uint32_t kRxOld = kRxFifoThreshold | kRxDmaBurst, kRxDefault = kRx128Int | kRxDmaBurst, kRx8168c = kRx128Int | kRxMulti | kRxDmaBurst,
                   kRx8168g = kRx8168c | kRxEarlyOff, kRx8125 = kRxFetch8125 | kRxDmaBurst;

// r8169's rtl_chip_infos, most specific first. (The mac versions it gives them are in the comments.)
constexpr RtlChip kChips[] = {
    {0x7CF, 0x64A, RtlFamily::Rtl8126, kRx8125, true, true, true, "Realtek RTL8126A"},    // 70
    {0x7CF, 0x649, RtlFamily::Rtl8126, kRx8125, true, true, true, "Realtek RTL8126A"},    // 69
    {0x7CF, 0x688, RtlFamily::Rtl8125, kRx8125, true, true, true, "Realtek RTL8125D"},    // 64
    {0x7CF, 0x641, RtlFamily::Rtl8125, kRx8125, true, true, true, "Realtek RTL8125B"},    // 63
    {0x7CF, 0x609, RtlFamily::Rtl8125, kRx8125, true, true, true, "Realtek RTL8125A"},    // 61
    {0x7C8, 0x608, RtlFamily::Rtl8125, kRx8125, true, true, true, "Realtek RTL8125A"},    // 60
    {0x7CF, 0x54B, RtlFamily::Rtl8168, kRx8168g, true, true, true, "Realtek RTL8117"},    // 52
    {0x7CF, 0x54A, RtlFamily::Rtl8168, kRx8168g, true, true, true, "Realtek RTL8117"},    // 52
    {0x7C8, 0x500, RtlFamily::Rtl8168, kRx8168g, true, true, true, "Realtek RTL8168EP"},  // 49-51
    {0x7C8, 0x540, RtlFamily::Rtl8168, kRx8168g, true, true, true, "Realtek RTL8168H"},   // 45, 46
    {0x7CF, 0x5C8, RtlFamily::Rtl8168, kRx8168g, true, true, true, "Realtek RTL8411B"},   // 44
    {0x7CF, 0x509, RtlFamily::Rtl8168, kRx8168g, true, true, true, "Realtek RTL8168GU"},  // 42
    {0x7CF, 0x4C1, RtlFamily::Rtl8168, kRx8168g, true, true, true, "Realtek RTL8168G"},   // 41
    {0x7CF, 0x4C0, RtlFamily::Rtl8168, kRx8168g, true, true, true, "Realtek RTL8168G"},   // 40
    {0x7C8, 0x488, RtlFamily::Rtl8168, kRx8168c, true, false, true, "Realtek RTL8411"},   // 38
    {0x7CF, 0x481, RtlFamily::Rtl8168, kRx8168c, true, false, true, "Realtek RTL8168F"},  // 36
    {0x7CF, 0x480, RtlFamily::Rtl8168, kRx8168c, true, false, true, "Realtek RTL8168F"},  // 35
    {0x7C8, 0x2C8, RtlFamily::Rtl8168, kRx8168c, true, false, false, "Realtek RTL8168E-VL"},  // 34
    {0x7CF, 0x2C1, RtlFamily::Rtl8168, kRxDefault, false, false, true, "Realtek RTL8168E"},  // 32
    {0x7C8, 0x2C0, RtlFamily::Rtl8168, kRxDefault, false, false, true, "Realtek RTL8168E"},  // 33
    {0x7CF, 0x281, RtlFamily::Rtl8168, kRxDefault, false, false, true, "Realtek RTL8168D"},  // 25
    {0x7C8, 0x280, RtlFamily::Rtl8168, kRxDefault, false, false, true, "Realtek RTL8168D"},  // 26
    {0x7CF, 0x28A, RtlFamily::Rtl8168, kRxDefault, false, false, true, "Realtek RTL8168DP"},  // 28
    {0x7CF, 0x28B, RtlFamily::Rtl8168, kRxDefault, false, false, true, "Realtek RTL8168DP"},  // 31
    {0x7CF, 0x3C9, RtlFamily::Rtl8168, kRx8168c, false, false, false, "Realtek RTL8168C"},    // 23
    {0x7CF, 0x3C8, RtlFamily::Rtl8168, kRx8168c, false, false, false, "Realtek RTL8168C"},    // 18
    {0x7C8, 0x3C8, RtlFamily::Rtl8168, kRx8168c, false, false, false, "Realtek RTL8168CP"},   // 24
    {0x7CF, 0x3C0, RtlFamily::Rtl8168, kRx8168c, false, false, false, "Realtek RTL8168C"},    // 19
    {0x7CF, 0x3C2, RtlFamily::Rtl8168, kRx8168c, false, false, false, "Realtek RTL8168C"},    // 20
    {0x7CF, 0x3C3, RtlFamily::Rtl8168, kRx8168c, false, false, false, "Realtek RTL8168C"},    // 21
    {0x7C8, 0x3C0, RtlFamily::Rtl8168, kRx8168c, false, false, false, "Realtek RTL8168CP"},   // 22
    {0x7C8, 0x380, RtlFamily::Rtl8168, kRxOld, false, false, false, "Realtek RTL8168B"},      // 17
    {0x7C8, 0x300, RtlFamily::Rtl8168, kRxOld, false, false, false, "Realtek RTL8168B"},      // 11
    {0x7C8, 0x448, RtlFamily::Rtl8101, kRxDefault, false, false, false, "Realtek RTL8106E"},  // 39
    {0x7C8, 0x440, RtlFamily::Rtl8101, kRxDefault, true, false, false, "Realtek RTL8402"},    // 37
    {0x7CF, 0x409, RtlFamily::Rtl8101, kRxDefault, false, false, false, "Realtek RTL8105E"},  // 29
    {0x7C8, 0x408, RtlFamily::Rtl8101, kRxDefault, false, false, false, "Realtek RTL8105E"},  // 30
    {0x7CF, 0x349, RtlFamily::Rtl8101, kRxDefault, false, false, false, "Realtek RTL8102E"},  // 08
    {0x7CF, 0x249, RtlFamily::Rtl8101, kRxDefault, false, false, false, "Realtek RTL8102E"},  // 08
    {0x7CF, 0x348, RtlFamily::Rtl8101, kRxDefault, false, false, false, "Realtek RTL8102E"},  // 07
    {0x7CF, 0x248, RtlFamily::Rtl8101, kRxDefault, false, false, false, "Realtek RTL8102E"},  // 07
    {0x7CF, 0x240, RtlFamily::Rtl8101, kRxOld, false, false, false, "Realtek RTL8101E"},      // 14
    {0x7C8, 0x348, RtlFamily::Rtl8101, kRxDefault, false, false, false, "Realtek RTL8102E"},  // 09
    {0x7C8, 0x248, RtlFamily::Rtl8101, kRxDefault, false, false, false, "Realtek RTL8102E"},  // 09
    {0x7C8, 0x340, RtlFamily::Rtl8101, kRxOld, false, false, false, "Realtek RTL8101E"},      // 10
    {0xFC8, 0x980, RtlFamily::Rtl8169, kRxOld, false, false, false, "Realtek RTL8169sc/8110sc"},  // 06
    {0xFC8, 0x180, RtlFamily::Rtl8169, kRxOld, false, false, false, "Realtek RTL8169sb/8110sb"},  // 05
    {0xFC8, 0x100, RtlFamily::Rtl8169, kRxOld, false, false, false, "Realtek RTL8169s"},          // 04
    {0xFC8, 0x040, RtlFamily::Rtl8169, kRxOld, false, false, false, "Realtek RTL8169s"},          // 03
    {0xFC8, 0x008, RtlFamily::Rtl8169, kRxOld, false, false, false, "Realtek RTL8169"},           // 02
};

struct PciId {
  uint16_t vendor, device;
};
// r8169's PCI table: Realtek's own ids, and the boards sold under other names.
constexpr PciId kPciIds[] = {
    {0x10EC, 0x2502}, {0x10EC, 0x2600}, {0x10EC, 0x8129}, {0x10EC, 0x8136}, {0x10EC, 0x8161}, {0x10EC, 0x8162}, {0x10EC, 0x8167}, {0x10EC, 0x8168},
    {0x10EC, 0x8169}, {0x10EC, 0x8125}, {0x10EC, 0x8126}, {0x10EC, 0x3000}, {0x10FF, 0x8168}, {0x1186, 0x4300}, {0x1186, 0x4302}, {0x1259, 0xC107},
    {0x16EC, 0x0116}, {0x1737, 0x1032}, {0x0001, 0x8168},
};
}  // namespace

const RtlChip* rtl8169_identify(uint32_t tx_config) {
  const uint16_t version = (tx_config >> 20) & 0xFCF;
  for (const RtlChip& chip : kChips)
    if ((version & chip.mask) == chip.value) return &chip;
  return nullptr;
}

bool Rtl8169NetworkInterface::supports(uint16_t vendor_id, uint16_t device_id) {
  for (const PciId& id : kPciIds)
    if (id.vendor == vendor_id && id.device == device_id) return true;
  return false;
}

void Rtl8169NetworkInterface::write_interrupt_mask(uint32_t mask) {
  if (new_layout()) regs().write32(kIntrMask8125, mask);
  else regs().write16(kIntrMask, static_cast<uint16_t>(mask));
}

void Rtl8169NetworkInterface::mask_interrupts() {
  if (new_layout()) {
    regs().write32(kIntrMask8125, 0);
    regs().write32(kIntrStatus8125, 0xFFFFFFFFu);  // (write 1 to clear)
  } else {
    regs().write16(kIntrMask, 0);
    regs().write16(kIntrStatus, 0xFFFF);
  }
}

bool Rtl8169NetworkInterface::reset() {
  // Stop both directions first; chips with the RXDV gate close it so no half-received frame is written while the reset runs (rtl_hw_reset).
  regs().write8(kChipCmd, 0);
  if (chip_->rxdv_gate) regs().set_bits32(kMisc, kMiscRxdvGated);
  delay_us(2000);
  regs().write8(kChipCmd, kCmdReset);
  for (int i = 0; i < 1000; ++i) {  // 100 ms
    if (!(regs().read8(kChipCmd) & kCmdReset)) return true;
    delay_us(100);
  }
  return false;
}

bool Rtl8169NetworkInterface::initialize() {
  if (started_ || !device_) return false;
  if (!device_->claim()) return false;
  claimed_ = true;
  const uint32_t tx_config = regs().read32(kTxConfig);
  if (tx_config == 0xFFFFFFFFu) return false;  // nothing answers
  chip_ = rtl8169_identify(tx_config);
  if (!chip_) return false;

  for (int i = 0; i < 6; ++i) mac_.bytes[i] = regs().read8(kIdr0 + i);
  mask_interrupts();
  if (!reset()) return false;
  mask_interrupts();

  rx_ring_ = allocate_dma_view<Descriptor>(allocator_, kReceiveDescriptors, 256);
  tx_ring_ = allocate_dma_view<Descriptor>(allocator_, kTransmitDescriptors, 256);
  buffers_ = allocate_dma_view<uint8_t>(allocator_, uint64_t{kReceiveDescriptors + kTransmitDescriptors} * kBufferSize, 4096);
  if (rx_ring_.empty() || tx_ring_.empty() || buffers_.empty()) return false;
  for (uint32_t i = 0; i < kReceiveDescriptors; ++i) {
    rx_ring_[i].opts2 = 0;
    rx_ring_[i].address = nic_dma_address(rx_buffer(i));
    rx_ring_[i].opts1 = rx_opts1(i);
  }
  for (uint32_t i = 0; i < kTransmitDescriptors; ++i) {
    tx_ring_[i].opts2 = 0;
    tx_ring_[i].address = nic_dma_address(tx_buffer(i));
    tx_ring_[i].opts1 = i == kTransmitDescriptors - 1 ? kRingEnd : 0;  // ours: no DescOwn
  }
  __atomic_thread_fence(__ATOMIC_RELEASE);

  // rtl_hw_start: the configuration registers are written with the lock open.
  regs().write8(kCfg9346, kCfg9346Unlock);
  if (!new_layout()) {
    regs().write8(kMaxTxPacketSize, kNoEarlyTx);
    regs().write16(kIntrMitigate, 0);
  }
  regs().write16(kCPlusCmd, static_cast<uint16_t>((regs().read16(kCPlusCmd) & ~kCPlusRxVlan) | kCPlusRxChecksum));
  regs().write16(kRxMaxSize, kBufferSize);
  const uint64_t tx = nic_dma_address(tx_ring_), rx = nic_dma_address(rx_ring_);
  regs().write32(kTxDescHigh, static_cast<uint32_t>(tx >> 32));
  regs().write32(kTxDescLow, static_cast<uint32_t>(tx));
  regs().write32(kRxDescHigh, static_cast<uint32_t>(rx >> 32));
  regs().write32(kRxDescLow, static_cast<uint32_t>(rx));
  regs().write8(kCfg9346, kCfg9346Lock);
  if (chip_->rxdv_gate) regs().clear_bits32(kMisc, kMiscRxdvGated);

  regs().write8(kChipCmd, kCmdTxEnable | kCmdRxEnable);
  regs().write32(kRxConfig, chip_->rx_config);
  regs().write32(kTxConfig, kTxDmaBurst | kTxInterFrameGap | (chip_->auto_fifo ? kTxAutoFifo : 0));
  rx_next_ = 0;
  tx_next_ = tx_clean_ = 0;
  set_receive_mode();
  mask_interrupts();
  started_ = true;
  return true;
}

void Rtl8169NetworkInterface::shutdown() {
  disable_interrupts();
  if (claimed_) {
    if (chip_) {
      started_ = false;
      reset();  // the chip stops using our memory
      mask_interrupts();
    }
    device_->release();
    claimed_ = false;
  }
  started_ = false;
  release_view(allocator_, rx_ring_);
  release_view(allocator_, tx_ring_);
  release_view(allocator_, buffers_);
}

uint32_t Rtl8169NetworkInterface::rx_opts1(uint32_t i) const {
  return kDescOwn | (i == kReceiveDescriptors - 1 ? kRingEnd : 0) | kBufferSize;
}

void Rtl8169NetworkInterface::set_receive_mode() {
  // Without a list of groups to join, "some multicast" means all of it: every bit of the hash filter set.
  const uint32_t filter = all_multicast_ ? 0xFFFFFFFFu : 0;
  regs().write32(kMar0, filter);
  regs().write32(kMar0 + 4, filter);
  const uint32_t accept = kAcceptBroadcast | kAcceptMyPhys | (all_multicast_ ? kAcceptMulticast : 0);
  regs().write32(kRxConfig, (regs().read32(kRxConfig) & ~kAcceptMask) | accept);
}

bool Rtl8169NetworkInterface::accept_all_multicast(bool enable) {
  kernel::ScopedMutex guard(mutex_);
  if (!started_) return false;
  all_multicast_ = enable;
  set_receive_mode();
  return enable;
}

uint16_t Rtl8169NetworkInterface::phy_status() const {
  if (!started_) return 0;
  return new_layout() ? regs().read16(kPhyStatus) : regs().read8(kPhyStatus);
}

bool Rtl8169NetworkInterface::link_up() const { return phy_status() & kPhyLink; }

uint32_t Rtl8169NetworkInterface::link_speed_mbps() const {
  const uint16_t status = phy_status();
  if (!(status & kPhyLink)) return 0;
  if (chip_->family == RtlFamily::Rtl8126 && (status & kPhy5000)) return 5000;
  if (new_layout() && (status & kPhy2500)) return 2500;
  if (status & kPhy1000) return 1000;
  if (status & kPhy100) return 100;
  if (status & kPhy10) return 10;
  return 0;
}

LinkDuplex Rtl8169NetworkInterface::link_duplex() const {
  const uint16_t status = phy_status();
  if (!(status & kPhyLink)) return LinkDuplex::Unknown;
  return (status & kPhyFullDuplex) ? LinkDuplex::Full : LinkDuplex::Half;
}

bool Rtl8169NetworkInterface::transmit(ConstByteView frame) { return transmit_frame(frame, TransmitChecksum{}); }

bool Rtl8169NetworkInterface::transmit_frame(ConstByteView frame, TransmitChecksum checksum) {
  kernel::ScopedMutex guard(mutex_);
  return send(frame, checksum, 0);
}

bool Rtl8169NetworkInterface::transmit_segmented(ConstByteView frame, TransmitChecksum checksum, uint16_t mss) {
  kernel::ScopedMutex guard(mutex_);
  if (mss == 0 || !chip_ || !chip_->tso) {
    ++stats_.send_failures;
    return false;
  }
  return send(frame, checksum, mss);
}

uint32_t Rtl8169NetworkInterface::ring_end(uint32_t i) const { return i == kTransmitDescriptors - 1 ? kRingEnd : 0; }

// A frame (or segment) is done when the chip has handed back the last descriptor of its chain.
void Rtl8169NetworkInterface::reclaim_transmitted() {
  while (tx_clean_ != tx_next_) {
    const uint32_t end = tx_frame_end_[tx_clean_];
    volatile Descriptor& d = tx_ring_[end];
    if (d.opts1 & kDescOwn) return;
    tx_clean_ = (end + 1) % kTransmitDescriptors;
  }
}

// One frame in one descriptor, or (mss != 0) a TCP segment in a chain of them the chip cuts into frames. Every descriptor of a chain carries
// the same offload bits; the first one is handed to the chip last, so it never sees half a chain.
bool Rtl8169NetworkInterface::send(ConstByteView frame, TransmitChecksum checksum, uint16_t mss) {
  const bool segment = mss != 0;
  TsoLayout layout;
  bool fits = started_ && !frame.empty();
  if (segment)
    fits = fits && frame.size() <= kMaxSegmentation && mss <= kTd1MssMax && tso_layout(frame, checksum, &layout) && layout.tcp_start <= kTd1GiantTcpMax;
  else
    fits = fits && frame.size() <= kEthernetMaxFrame && (!checksum.requested() || checksum.fits(frame.size()));
  if (!fits) {
    ++stats_.send_failures;
    return false;
  }
  // Short frames are padded here: several of these chips send them unpadded or with garbage (r8169's rtl_test_hw_pad_bug); their checksum is
  // then finished in software too, as r8169 does.
  const uint32_t length = segment || frame.size() >= kEthernetMinFrame ? static_cast<uint32_t>(frame.size()) : kEthernetMinFrame;
  const uint32_t descriptors = (length + kBufferSize - 1) / kBufferSize;
  reclaim_transmitted();
  const uint32_t in_flight = (tx_next_ + kTransmitDescriptors - tx_clean_) % kTransmitDescriptors;
  if (kTransmitDescriptors - 1 - in_flight < descriptors) {  // the chip has not sent enough yet
    ++stats_.send_failures;
    return false;
  }

  uint32_t opts1 = 0, opts2 = 0;
  const bool ipv6 = frame_ip_version(frame) == 6;
  if (segment) {
    opts1 = (layout.ipv6 ? kTd1GiantSendIpv6 : kTd1GiantSendIpv4) | (uint32_t{layout.tcp_start} << kTd1GiantTcpShift);
    opts2 = uint32_t{mss} << kTd1MssShift;
  } else if (checksum.requested()) {
    bool hardware = (checksum.tcp() || checksum.udp()) && frame.size() >= kEthernetMinFrame && checksum.start <= kTd1TransportMax;
    if (hardware && chip_->family == RtlFamily::Rtl8169) {
      if (ipv6) hardware = false;
      else opts1 = kTd0Ip | (checksum.tcp() ? kTd0Tcp : kTd0Udp);
    } else if (hardware) {
      opts2 = (ipv6 ? kTd1Ipv6 : kTd1Ipv4) | (checksum.tcp() ? kTd1Tcp : kTd1Udp) | (uint32_t{checksum.start} << kTd1TransportShift);
    }
  }  // (otherwise it is finished in the buffer below)
  const bool software_checksum = !segment && checksum.requested() && opts1 == 0 && opts2 == 0;

  const uint32_t first = tx_next_;
  uint32_t slot = first;
  for (uint32_t offset = 0, n = 0; n < descriptors; ++n) {
    const uint32_t piece = length - offset < kBufferSize ? length - offset : kBufferSize;
    const ByteView buffer = tx_buffer(slot).first(piece);
    buffer.fill(0);  // (the padding of a short frame)
    if (offset < frame.size()) buffer.copy_from(frame.subview(offset, frame.size() - offset < piece ? frame.size() - offset : piece));
    if (n == 0 && software_checksum && !finish_transmit_checksum(buffer.first(frame.size()), checksum)) {
      ++stats_.send_failures;
      return false;
    }
    if (n == 0 && segment && layout.ipv6 && !prepare_tso_headers(buffer, layout)) return false;  // (IPv6 giant send: tcp_v6_gso_csum_prep)
    volatile Descriptor& d = tx_ring_[slot];
    d.opts2 = opts2;
    d.address = nic_dma_address(tx_buffer(slot));
    const uint32_t flags = opts1 | piece | ring_end(slot) | (n + 1 == descriptors ? kLastFrag : 0);
    if (n == 0) {
      d.opts1 = flags | kFirstFrag;  // (owned by the chip below, once the rest of the chain is)
    } else {
      __atomic_thread_fence(__ATOMIC_RELEASE);
      d.opts1 = flags | kDescOwn;
    }
    if (n + 1 == descriptors) tx_frame_end_[first] = static_cast<uint8_t>(slot);
    offset += piece;
    slot = (slot + 1) % kTransmitDescriptors;
  }
  __atomic_thread_fence(__ATOMIC_RELEASE);  // the frame, the addresses and the rest of the chain before the first descriptor's ownership bit
  tx_ring_[first].opts1 = tx_ring_[first].opts1 | kDescOwn;
  __atomic_thread_fence(__ATOMIC_SEQ_CST);
  tx_next_ = slot;
  if (new_layout())
    regs().write16(kTxPoll8125, 1);
  else
    regs().write8(kTxPoll, kTxPollNormal);
  ++stats_.frames_sent;
  if (segment) ++stats_.segmented_sends;
  return true;
}

bool Rtl8169NetworkInterface::receive(ByteView buffer, uint32_t* length) {
  ReceivedFrame frame;
  if (!length || !receive_frame(buffer, &frame)) return false;
  *length = frame.length;
  return true;
}

bool Rtl8169NetworkInterface::receive_frame(ByteView buffer, ReceivedFrame* received) {
  kernel::ScopedMutex guard(mutex_);
  if (!started_ || !received) return false;
  for (uint32_t tries = 0; tries < kReceiveDescriptors; ++tries) {
    volatile Descriptor& d = rx_ring_[rx_next_];
    const uint32_t status = d.opts1;
    if (status & kDescOwn) return false;
    __atomic_thread_fence(__ATOMIC_ACQUIRE);

    // A frame bigger than one buffer arrives in several descriptors: those, and frames with errors, are dropped.
    const uint32_t size = status & kLengthMask;
    const bool usable = !(status & kRxError) && (status & kFirstFrag) && (status & kLastFrag) && size > kCrcLength && size <= kBufferSize;
    if (usable) {
      const uint32_t frame = size - kCrcLength;
      buffer.copy_from(ConstByteView(rx_buffer(rx_next_)).first(frame).at_most(buffer.size()));  // as much as fits
      const uint32_t checked = status & (kRxProtoMask | kRxIpFail | kRxUdpFail | kRxTcpFail);
      *received = ReceivedFrame{frame, (status & kRxProtoMask) != 0 && !(status & kRxIpFail), checked == kRxProtoTcp || checked == kRxProtoUdp};
      ++stats_.frames_received;
    } else {
      ++stats_.frames_dropped;
    }
    d.opts2 = 0;
    d.address = nic_dma_address(rx_buffer(rx_next_));
    __atomic_thread_fence(__ATOMIC_RELEASE);
    d.opts1 = rx_opts1(rx_next_);  // back to the chip
    rx_next_ = (rx_next_ + 1) % kReceiveDescriptors;
    if (usable) return true;
  }
  return false;
}

// ---- interrupts -----------------------------------------------------------------------------------------------------------------------------------

// Takes no lock: the status bits are acknowledged (write 1 to clear) and whoever polls is woken; the rings are looked at by receive().
void Rtl8169NetworkInterface::on_interrupt(void* self) {
  Rtl8169NetworkInterface* nic = static_cast<Rtl8169NetworkInterface*>(self);
  INicRegisters& regs = nic->regs();
  const uint32_t status = nic->new_layout() ? regs.read32(kIntrStatus8125) : regs.read16(kIntrStatus);
  if (status == 0 || status == 0xFFFFu || status == 0xFFFFFFFFu) return;  // not ours, or the chip is gone
  if (nic->new_layout()) regs.write32(kIntrStatus8125, status);
  else regs.write16(kIntrStatus, static_cast<uint16_t>(status));
  nic->write_interrupt_mask(0);  // quiet until rearm_interrupts()
  __atomic_fetch_add(&nic->interrupt_count_, 1, __ATOMIC_ACQ_REL);
  const NetworkWake wake = __atomic_load_n(&nic->wake_, __ATOMIC_ACQUIRE);
  if (wake) wake(__atomic_load_n(&nic->wake_context_, __ATOMIC_ACQUIRE));
}

bool Rtl8169NetworkInterface::enable_interrupts(NetworkWake wake, void* context) {
  kernel::ScopedMutex guard(mutex_);
  if (!started_ || interrupts_ || !wake) return interrupts_;
  __atomic_store_n(&wake_context_, context, __ATOMIC_RELEASE);
  __atomic_store_n(&wake_, wake, __ATOMIC_RELEASE);
  if (!device_->attach_interrupt(&Rtl8169NetworkInterface::on_interrupt, this)) {
    __atomic_store_n(&wake_, static_cast<NetworkWake>(nullptr), __ATOMIC_RELEASE);
    return false;
  }
  mask_interrupts();
  write_interrupt_mask(kInterruptMask);
  interrupts_ = true;
  if (new_layout()) return true;  // (no software interrupt to test with: the mask stays open)
  // Forced software interrupt: if it does not come within 20 ms the message is lost somewhere, and the chip stays polled.
  const uint64_t before = __atomic_load_n(&interrupt_count_, __ATOMIC_ACQUIRE);
  regs().write8(kTxPoll, kTxPollSoftwareInterrupt);
  for (int i = 0; i < 200 && __atomic_load_n(&interrupt_count_, __ATOMIC_ACQUIRE) == before; ++i) delay_us(100);
  if (__atomic_load_n(&interrupt_count_, __ATOMIC_ACQUIRE) != before) {
    write_interrupt_mask(kInterruptMask);  // (the test interrupt masked it)
    return true;
  }
  write_interrupt_mask(0);
  device_->detach_interrupt();
  __atomic_store_n(&wake_, static_cast<NetworkWake>(nullptr), __ATOMIC_RELEASE);
  interrupts_ = false;
  return false;
}

// Status bits that came while the mask was shut are still set: opening it interrupts at once.
void Rtl8169NetworkInterface::rearm_interrupts() {
  if (interrupts_ && started_) write_interrupt_mask(kInterruptMask);
}

void Rtl8169NetworkInterface::disable_interrupts() {
  if (!interrupts_) return;
  if (claimed_) write_interrupt_mask(0);
  device_->detach_interrupt();
  __atomic_store_n(&wake_, static_cast<NetworkWake>(nullptr), __ATOMIC_RELEASE);
  interrupts_ = false;
}

}  // namespace hardware
