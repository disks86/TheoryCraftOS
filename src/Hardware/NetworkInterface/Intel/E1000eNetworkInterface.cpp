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

#include "Hardware/NetworkInterface/Intel/E1000eNetworkInterface.hpp"
#include "Hardware/NetworkInterface/Intel/IntelEthernetRegisters.hpp"
#include "Kernel/Delay.hpp"

namespace hardware {

using namespace intel_eth;

namespace {
// Receive queue n: 0x2800 + 0x100 * n.
constexpr uint32_t kRdbal = 0x2800, kRdbah = 0x2804, kRdlen = 0x2808, kRdh = 0x2810, kRdt = 0x2818, kRxQueueStride = 0x100;
uint32_t rx_reg(uint32_t q, uint32_t reg) { return reg + kRxQueueStride * q; }
// The 82574's extras: extended receive descriptors (RFCTL.EXSTEN), RSS (MRQC: two queues, IPv4 and TCP hashing; RETA: the queue in bit 7 of
// each entry), MSI-X (IVAR: a vector for receive queue 0, 1 and the other causes; EIAC clears a queue's cause when its message goes out;
// CTRL_EXT.EIAME and PBA_CLR), and a throttle per vector (EITR).
constexpr uint32_t kRfctl = 0x5008, kRfctlExtended = 1u << 15;
constexpr uint32_t kMrqc = 0x5818, kMrqcRss2Queues = 0x1, kMrqcIpv4Tcp = 1u << 16, kMrqcIpv4 = 1u << 17, kReta = 0x5C00, kRssKey = 0x5C80;
constexpr uint32_t kRxcsumPcsd = 1u << 13;
constexpr uint32_t kIvar82574 = 0x00E4, kIvarValid = 0x8, kEiac82574 = 0x00DC, kEitr82574 = 0x00E8;
constexpr uint32_t kCtrlExtEiame = 1u << 24, kCtrlExtIame = 1u << 27, kCtrlExtPbaClear = 1u << 31;
constexpr uint32_t kIntRxQueue0 = 1u << 20, kIntRxQueue1 = 1u << 21, kIntOther = 1u << 24;
constexpr uint8_t kRssKeyBytes[40] = {0x6d, 0x5a, 0x56, 0xda, 0x25, 0x5b, 0x0e, 0xc2, 0x41, 0x67, 0x25, 0x3d, 0x43, 0xa3, 0x8f, 0xb0, 0xd0, 0xca, 0x2b, 0xcb,
                                      0xae, 0x7b, 0x30, 0xb4, 0x77, 0xcb, 0x2d, 0xa3, 0x80, 0x30, 0xf2, 0x0c, 0x6a, 0x42, 0xb7, 0x3b, 0xbe, 0xac, 0x01, 0xfa};
constexpr uint32_t kTdbal = 0x3800, kTdbah = 0x3804, kTdlen = 0x3808, kTdh = 0x3810, kTdt = 0x3818, kTarc0 = 0x3840;

constexpr uint32_t kExtcnfSwFlag = 1u << 5;   // MDIO software ownership (82573 and later, ICH and PCH)
constexpr uint32_t kFwsmResetAllowed = 1u << 6;  // RSPCIPHY: the ME lets the host reset (and power-cycle) the PHY
constexpr uint32_t kFextnvm3PhyCfgMask = 0x0C000000, kFextnvm3PhyCfg50ms = 0x08000000;
constexpr uint32_t kIosfpcRdmtsHex = 0x00010000, kTarc0MultiQ3 = 3u << 28, kTarc0MultiQ2 = 1u << 29;

constexpr uint8_t kStatusDescriptorDone = 1u << 0, kStatusEndOfPacket = 1u << 1, kStatusIgnoreChecksum = 1u << 2, kStatusUdpChecksum = 1u << 4,
                  kStatusTcpChecksum = 1u << 5, kStatusIpChecksum = 1u << 6;
constexpr uint8_t kErrorTcpUdpChecksum = 1u << 5, kErrorIpChecksum = 1u << 6;  // (checksum errors: the frame is passed on, software looks again)
constexpr uint8_t kCommandEndOfPacket = 1u << 0, kCommandInsertCrc = 1u << 1, kCommandReportStatus = 1u << 3, kCommandExtended = 1u << 5;
// Checksums go through a TCP/IP context descriptor (TUCSS/TUCSO; TUCMD: extended, TCP) and an extended data descriptor (DTYP 1, POPTS.TXSM),
// as e1000e does: the legacy descriptor's CSO/CSS insertion is not implemented everywhere (QEMU's 82574 ignores it).
constexpr uint8_t kContextTcp = 1u << 0, kDataDescriptorType = 1u << 4, kPoptsTransportChecksum = 1u << 1;
// Segmentation (e1000_tso): the context's TUCMD says IPv4, TCP and TSE and carries PAYLEN, HDRLEN and MSS; every data descriptor has TSE too
// and asks for both checksums (POPTS IXSM, TXSM).
constexpr uint8_t kContextIp = 1u << 1, kCommandSegmentation = 1u << 2, kPoptsIpChecksum = 1u << 0;
constexpr uint32_t kItr = 0x00C4;

using K = E1000eKind;
// e1000e's PCI table (hw.h). One line per device id; the names are what Intel sells them as.
constexpr E1000eModel kModels[] = {
    {0x105E, K::Mac8257x, "Intel 82571EB"}, {0x105F, K::Mac8257x, "Intel 82571EB"}, {0x1060, K::Mac8257x, "Intel 82571EB"},
    {0x10A4, K::Mac8257x, "Intel 82571EB"}, {0x10A5, K::Mac8257x, "Intel 82571EB"}, {0x10BC, K::Mac8257x, "Intel 82571EB"},
    {0x10D5, K::Mac8257x, "Intel 82571PT"}, {0x10D9, K::Mac8257x, "Intel 82571EB"}, {0x10DA, K::Mac8257x, "Intel 82571EB"},
    {0x107D, K::Mac8257x, "Intel 82572EI"}, {0x107E, K::Mac8257x, "Intel 82572EI"}, {0x107F, K::Mac8257x, "Intel 82572EI"},
    {0x10B9, K::Mac8257x, "Intel 82572EI"}, {0x108B, K::Mac8257x, "Intel 82573E"},  {0x108C, K::Mac8257x, "Intel 82573E"},
    {0x109A, K::Mac8257x, "Intel 82573L"},  {0x10D3, K::Mac8257x, "Intel 82574L"},  {0x10F6, K::Mac8257x, "Intel 82574LA"},
    {0x150C, K::Mac8257x, "Intel 82583V"},  {0x1096, K::Mac8257x, "Intel 80003ES2LAN"}, {0x1098, K::Mac8257x, "Intel 80003ES2LAN"},
    {0x10BA, K::Mac8257x, "Intel 80003ES2LAN"}, {0x10BB, K::Mac8257x, "Intel 80003ES2LAN"},
    // ICH8, ICH9, ICH10 (82566, 82567, 82562 PHYs)
    {0x1049, K::Ich, "Intel 82566MM"}, {0x104A, K::Ich, "Intel 82566DM"}, {0x104B, K::Ich, "Intel 82566DC"}, {0x104C, K::Ich, "Intel 82562V"},
    {0x104D, K::Ich, "Intel 82566MC"}, {0x10C4, K::Ich, "Intel 82562GT"}, {0x10C5, K::Ich, "Intel 82562G"},  {0x1501, K::Ich, "Intel 82567V-3"},
    {0x10BD, K::Ich, "Intel 82566DM-2"}, {0x10BF, K::Ich, "Intel 82567LF"}, {0x10C0, K::Ich, "Intel 82562V-2"}, {0x10C2, K::Ich, "Intel 82562G-2"},
    {0x10C3, K::Ich, "Intel 82562GT-2"}, {0x10CB, K::Ich, "Intel 82567V"},  {0x10E5, K::Ich, "Intel 82567LM-4"}, {0x10F5, K::Ich, "Intel 82567LM"},
    {0x294C, K::Ich, "Intel 82566DC-2"}, {0x10CC, K::Ich, "Intel 82567LM-2"}, {0x10CD, K::Ich, "Intel 82567LF-2"}, {0x10CE, K::Ich, "Intel 82567V-2"},
    {0x10DE, K::Ich, "Intel 82567LM-3"}, {0x10DF, K::Ich, "Intel 82567LF-3"}, {0x1525, K::Ich, "Intel 82567V-4"},
    // 5 Series / 3400 (82577, 82578), 6 and 7 Series (82579)
    {0x10EA, K::Pch, "Intel 82577LM"}, {0x10EB, K::Pch, "Intel 82577LC"}, {0x10EF, K::Pch, "Intel 82578DM"}, {0x10F0, K::Pch, "Intel 82578DC"},
    {0x1502, K::Pch2, "Intel 82579LM"}, {0x1503, K::Pch2, "Intel 82579V"},
    // 8 and 9 Series (I217, I218)
    {0x153A, K::PchLpt, "Intel I217-LM"}, {0x153B, K::PchLpt, "Intel I217-V"}, {0x155A, K::PchLpt, "Intel I218-LM"}, {0x1559, K::PchLpt, "Intel I218-V"},
    {0x15A0, K::PchLpt, "Intel I218-LM"}, {0x15A1, K::PchLpt, "Intel I218-V"}, {0x15A2, K::PchLpt, "Intel I218-LM"}, {0x15A3, K::PchLpt, "Intel I218-V"},
    // 100 Series and later (I219)
    {0x156F, K::PchSpt, "Intel I219-LM"}, {0x1570, K::PchSpt, "Intel I219-V"}, {0x15B7, K::PchSpt, "Intel I219-LM"}, {0x15B8, K::PchSpt, "Intel I219-V"},
    {0x15B9, K::PchSpt, "Intel I219-LM"}, {0x15D7, K::PchSpt, "Intel I219-LM"}, {0x15D8, K::PchSpt, "Intel I219-V"}, {0x15E3, K::PchSpt, "Intel I219-LM"},
    {0x15D6, K::PchSpt, "Intel I219-V"},  {0x15BD, K::PchSpt, "Intel I219-LM"}, {0x15BE, K::PchSpt, "Intel I219-V"}, {0x15BB, K::PchSpt, "Intel I219-LM"},
    {0x15BC, K::PchSpt, "Intel I219-V"},  {0x15DF, K::PchSpt, "Intel I219-LM"}, {0x15E0, K::PchSpt, "Intel I219-V"}, {0x15E1, K::PchSpt, "Intel I219-LM"},
    {0x15E2, K::PchSpt, "Intel I219-V"},  {0x0D4E, K::PchSpt, "Intel I219-LM"}, {0x0D4F, K::PchSpt, "Intel I219-V"}, {0x0D4C, K::PchSpt, "Intel I219-LM"},
    {0x0D4D, K::PchSpt, "Intel I219-V"},  {0x0D53, K::PchSpt, "Intel I219-LM"}, {0x0D55, K::PchSpt, "Intel I219-V"}, {0x15FB, K::PchSpt, "Intel I219-LM"},
    {0x15FC, K::PchSpt, "Intel I219-V"},  {0x15F9, K::PchSpt, "Intel I219-LM"}, {0x15FA, K::PchSpt, "Intel I219-V"}, {0x15F4, K::PchSpt, "Intel I219-LM"},
    {0x15F5, K::PchSpt, "Intel I219-V"},  {0x1A1E, K::PchSpt, "Intel I219-LM"}, {0x1A1F, K::PchSpt, "Intel I219-V"}, {0x1A1C, K::PchSpt, "Intel I219-LM"},
    {0x1A1D, K::PchSpt, "Intel I219-V"},  {0x0DC5, K::PchSpt, "Intel I219-LM"}, {0x0DC6, K::PchSpt, "Intel I219-V"}, {0x0DC7, K::PchSpt, "Intel I219-LM"},
    {0x0DC8, K::PchSpt, "Intel I219-V"},  {0x550A, K::PchSpt, "Intel I219-LM"}, {0x550B, K::PchSpt, "Intel I219-V"}, {0x550C, K::PchSpt, "Intel I219-LM"},
    {0x550D, K::PchSpt, "Intel I219-V"},  {0x550E, K::PchSpt, "Intel I219-LM"}, {0x550F, K::PchSpt, "Intel I219-V"}, {0x5510, K::PchSpt, "Intel I219-LM"},
    {0x5511, K::PchSpt, "Intel I219-V"},  {0x57A0, K::PchSpt, "Intel I219-LM"}, {0x57A1, K::PchSpt, "Intel I219-V"}, {0x57B3, K::PchSpt, "Intel I219-LM"},
    {0x57B4, K::PchSpt, "Intel I219-V"},  {0x57B5, K::PchSpt, "Intel I219-LM"}, {0x57B6, K::PchSpt, "Intel I219-V"}, {0x57B7, K::PchSpt, "Intel I219-LM"},
    {0x57B8, K::PchSpt, "Intel I219-V"},  {0x57B9, K::PchSpt, "Intel I219-LM"}, {0x57BA, K::PchSpt, "Intel I219-V"},
};
// The 82573, 82574 and 82583 take the PHY with the MDIO software ownership flag, like the chipset parts.
constexpr uint16_t kMdioFlagDevices[] = {0x108B, 0x108C, 0x109A, 0x10D3, 0x10F6, 0x150C};
bool uses_mdio_flag(uint16_t device_id) {
  for (uint16_t id : kMdioFlagDevices)
    if (id == device_id) return true;
  return false;
}
}  // namespace

const E1000eModel* e1000e_find_model(uint16_t device_id) {
  for (const E1000eModel& model : kModels)
    if (model.device_id == device_id) return &model;
  return nullptr;
}

// ---- the PHY, through the MDIO interface of the MAC -----------------------------------------------------------------------------------------------------

bool E1000eNetworkInterface::acquire_phy() {
  // The 82571/82572 and 80003ES2LAN arbitrate through SWSM instead; their PHY is not touched here.
  if (!chipset_mac() && !uses_mdio_flag(model_->device_id)) return false;
  regs().set_bits32(kExtcnfCtrl, kExtcnfSwFlag);
  if (wait_for(regs(), kExtcnfCtrl, kExtcnfSwFlag, kExtcnfSwFlag, 100, 1000)) return true;
  regs().clear_bits32(kExtcnfCtrl, kExtcnfSwFlag);  // the firmware holds it
  return false;
}

void E1000eNetworkInterface::release_phy() { regs().clear_bits32(kExtcnfCtrl, kExtcnfSwFlag); }

bool E1000eNetworkInterface::phy_read(uint32_t reg, uint16_t* value) { return mdic_read(regs(), pch() ? 2 : 1, reg, value); }

bool E1000eNetworkInterface::phy_write(uint32_t reg, uint16_t value) { return mdic_write(regs(), pch() ? 2 : 1, reg, value); }

bool E1000eNetworkInterface::phy_accessible() {
  if (!acquire_phy()) return false;
  uint16_t id = 0;
  const bool ok = phy_read(kPhyId1, &id) && id != 0 && id != 0xFFFF;
  release_phy();
  return ok;
}

// A PHY the firmware left in a low-power state does not answer on MDIO. Pulling the LANPHYPC line power-cycles it (e1000_toggle_lanphypc_pch_lpt); only
// allowed when the management engine does not block PHY resets.
void E1000eNetworkInterface::wake_phy() {
  if (!pch() || phy_accessible() || !(regs().read32(kFwsm) & kFwsmResetAllowed)) return;
  regs().write32(kFextnvm3, (regs().read32(kFextnvm3) & ~kFextnvm3PhyCfgMask) | kFextnvm3PhyCfg50ms);
  uint32_t ctrl = regs().read32(kCtrl);
  ctrl = (ctrl | kCtrlLanPhyPcOverride) & ~kCtrlLanPhyPcValue;
  regs().write32(kCtrl, ctrl);
  regs().read32(kStatus);
  delay_us(20);
  regs().write32(kCtrl, ctrl & ~kCtrlLanPhyPcOverride);
  regs().read32(kStatus);
  if (model_->kind < E1000eKind::PchLpt) {
    delay_us(50000);
  } else {
    wait_for(regs(), kCtrlExt, kCtrlExtLanPhyPcDone, kCtrlExtLanPhyPcDone, 20, 5000);
    delay_us(30000);
  }
  regs().clear_bits32(kCtrlExt, kCtrlExtForceSmbus);  // the MAC talks to the PHY over PCIe-style K1 again, not SMBus
  phy_woken_ = true;
}

// The PHY negotiates on its own once autonegotiation is on; a PHY the firmware powered down or left forced is put back to that.
void E1000eNetworkInterface::enable_autonegotiation() {
  if (!acquire_phy()) return;
  uint16_t control = 0;
  if (phy_read(kPhyControl, &control) && (!(control & kPhyAutoNegEnable) || (control & kPhyPowerDown)))
    phy_write(kPhyControl, static_cast<uint16_t>((control & ~kPhyPowerDown) | kPhyAutoNegEnable | kPhyRestartAutoNeg));
  release_phy();
}

// ---- bring-up -------------------------------------------------------------------------------------------------------------------------------------------

bool E1000eNetworkInterface::reset() {
  regs().write32(kImc, 0xFFFFFFFFu);
  stop_dma(regs());
  regs().set_bits32(kCtrl, kCtrlReset);  // the MAC only: the PHY keeps its link
  delay_us(20000);
  if (!wait_for(regs(), kCtrl, kCtrlReset, 0, 100, 1000)) return false;
  if (chipset_mac()) {
    // The chipset parts load their configuration from the flash after a reset and say when they are done.
    if (wait_for(regs(), kStatus, kStatusLanInitDone, kStatusLanInitDone, 1500, 100)) regs().clear_bits32(kStatus, kStatusLanInitDone);
  } else {
    wait_for(regs(), kEecd, kEecdAutoReadDone, kEecdAutoReadDone, 100, 100);
  }
  regs().write32(kImc, 0xFFFFFFFFu);
  regs().read32(kIcr);
  return true;
}

bool E1000eNetworkInterface::start_receive_queue(uint32_t q) {
  ReceiveQueue& queue = rx_[q];
  queue.ring = allocate_dma_view<ReceiveDescriptor>(allocator_, kReceiveDescriptors, 4096);
  queue.buffers = allocate_dma_view<uint8_t>(allocator_, uint64_t{kReceiveDescriptors} * kBufferSize, 4096);
  if (queue.ring.empty() || queue.buffers.empty()) return false;
  for (uint32_t i = 0; i < kReceiveDescriptors; ++i) queue.ring[i] = ReceiveDescriptor{nic_dma_address(rx_buffer(queue, i)), 0};
  __atomic_thread_fence(__ATOMIC_RELEASE);
  const uint64_t rx = nic_dma_address(queue.ring);
  regs().write32(rx_reg(q, kRdbal), static_cast<uint32_t>(rx));
  regs().write32(rx_reg(q, kRdbah), static_cast<uint32_t>(rx >> 32));
  regs().write32(rx_reg(q, kRdlen), kReceiveDescriptors * sizeof(ReceiveDescriptor));
  regs().write32(rx_reg(q, kRdh), 0);
  regs().write32(rx_reg(q, kRdt), kReceiveDescriptors - 1);  // all but one descriptor belong to the card
  queue.next = 0;
  return true;
}

// The 82574's MSI-X vectors: 0 and 1 are the receive queues (their cause is cleared by EIAC; the handler masks the queue until rearm), 2 the
// other causes (the link), whose ICR the handler reads.
void E1000eNetworkInterface::on_queue_interrupt(void* self, uint32_t vector) {
  E1000eNetworkInterface* nic = static_cast<E1000eNetworkInterface*>(self);
  if (vector < 2) nic->regs().write32(kImc, vector == 0 ? kIntRxQueue0 : kIntRxQueue1);
  else nic->regs().read32(kIcr);
  __atomic_fetch_add(&nic->msix_interrupts_, 1, __ATOMIC_ACQ_REL);
  const NetworkWake wake = __atomic_load_n(&nic->wake_, __ATOMIC_ACQUIRE);
  if (wake) wake(__atomic_load_n(&nic->wake_context_, __ATOMIC_ACQUIRE));
}

bool E1000eNetworkInterface::enable_msix(NetworkWake wake, void* context) {
  if (queues_ < 2) return false;
  __atomic_store_n(&wake_context_, context, __ATOMIC_RELEASE);
  __atomic_store_n(&wake_, wake, __ATOMIC_RELEASE);
  __atomic_store_n(&msix_vectors_, 3u, __ATOMIC_RELEASE);
  if (device_->attach_interrupts(3, &E1000eNetworkInterface::on_queue_interrupt, this) != 3) {
    __atomic_store_n(&msix_vectors_, 0u, __ATOMIC_RELEASE);
    __atomic_store_n(&wake_, static_cast<NetworkWake>(nullptr), __ATOMIC_RELEASE);
    return false;
  }
  regs().write32(kImc, 0xFFFFFFFFu);
  regs().read32(kIcr);
  regs().write32(kIvar82574, (kIvarValid | 0) | ((kIvarValid | 1) << 4) | ((kIvarValid | 2) << 16));
  regs().write32(kCtrlExt, (regs().read32(kCtrlExt) & ~kCtrlExtIame) | kCtrlExtEiame | kCtrlExtPbaClear);
  regs().write32(kEiac82574, kIntRxQueue0 | kIntRxQueue1);
  for (uint32_t i = 0; i < 10; ++i)
    regs().write32(kRssKey + 4 * i, kRssKeyBytes[4 * i] | (uint32_t{kRssKeyBytes[4 * i + 1]} << 8) | (uint32_t{kRssKeyBytes[4 * i + 2]} << 16) |
                                        (uint32_t{kRssKeyBytes[4 * i + 3]} << 24));
  for (uint32_t i = 0; i < 128; i += 4) {  // the queues in turn: queue 1 is bit 7 of an entry
    uint32_t value = 0;
    for (uint32_t j = 0; j < 4; ++j) value |= (((i + j) & 1) ? 0x80u : 0u) << (8 * j);
    regs().write32(kReta + i, value);
  }
  regs().set_bits32(kRxcsum, kRxcsumPcsd);
  regs().write32(kMrqc, kMrqcRss2Queues | kMrqcIpv4 | kMrqcIpv4Tcp);
  write_throttle();
  const uint64_t before = __atomic_load_n(&msix_interrupts_, __ATOMIC_ACQUIRE);
  regs().write32(kIms, kIntRxQueue0 | kIntRxQueue1 | kIntOther | kIntLinkChange);
  // The route is tested on queue 0's vector and the other causes' one; both must come within 20 ms.
  regs().write32(kIcs, kIntRxQueue0 | kIntLinkChange);
  for (int i = 0; i < 200 && __atomic_load_n(&msix_interrupts_, __ATOMIC_ACQUIRE) < before + 2; ++i) delay_us(100);
  if (__atomic_load_n(&msix_interrupts_, __ATOMIC_ACQUIRE) >= before + 2) {
    regs().write32(kIms, kIntRxQueue0 | kIntRxQueue1 | kIntOther | kIntLinkChange);
    return true;
  }
  disable_interrupts();
  return false;
}

void E1000eNetworkInterface::disable_interrupts() {
  if (!msix_vectors_) {
    interrupts_.disable(*device_, claimed_);
    return;
  }
  if (claimed_) {
    regs().write32(kImc, 0xFFFFFFFFu);
    regs().write32(kMrqc, 0);  // everything to queue 0 again (both are still taken from)
    regs().write32(kIvar82574, 0);
    regs().clear_bits32(kCtrlExt, kCtrlExtEiame | kCtrlExtPbaClear);
    regs().write32(kEiac82574, 0);
  }
  device_->detach_interrupt();
  __atomic_store_n(&msix_vectors_, 0u, __ATOMIC_RELEASE);
  __atomic_store_n(&wake_, static_cast<NetworkWake>(nullptr), __ATOMIC_RELEASE);
}

bool E1000eNetworkInterface::initialize() {
  if (started_ || !device_) return false;
  model_ = e1000e_find_model(device_->device_id());
  if (!model_ || device_->vendor_id() != 0x8086) return false;
  if (!device_->claim()) return false;
  claimed_ = true;
  if (regs().read32(kStatus) == 0xFFFFFFFFu) return false;  // nothing answers

  MacAddress before{};
  const bool had_mac = read_mac(regs(), &before);
  wake_phy();
  if (!reset()) return false;

  // The station address: reloaded from the NVM by the reset; if it was not, the one the firmware had programmed.
  if (!read_mac(regs(), &mac_)) {
    if (!had_mac) return false;
    mac_ = before;
    write_mac(regs(), mac_);
  }
  const uint32_t multicast_registers = chipset_mac() ? 32 : 128;
  for (uint32_t i = 0; i < multicast_registers; ++i) regs().write32(kMta + 4 * i, 0);

  uint32_t ctrl = regs().read32(kCtrl);
  ctrl |= kCtrlSetLinkUp | kCtrlAutoSpeed;
  ctrl &= ~(kCtrlLinkReset | kCtrlForceSpeed | kCtrlForceDuplex | kCtrlPhyReset | kCtrlGioMasterDisable);
  regs().write32(kCtrl, ctrl);
  regs().set_bits32(kCtrlExt, kCtrlExtDriverLoaded);  // tells the management engine a driver runs the port
  if (pch()) regs().clear_bits32(kCtrlExt, kCtrlExtForceSmbus);
  enable_autonegotiation();

  // The 82574 runs two receive queues with extended descriptors (RSS stays off until MSI-X: all of it to queue 0); the others one, legacy.
  const bool is_82574 = model_->device_id == 0x10D3 || model_->device_id == 0x10F6;
  queues_ = is_82574 ? 2 : 1;
  extended_rx_ = is_82574;
  if (extended_rx_) regs().set_bits32(kRfctl, kRfctlExtended);
  regs().write32(kMrqc, 0);
  for (uint32_t q = 0; q < queues_; ++q)
    if (!start_receive_queue(q)) return false;
  rx_turn_ = 0;
  tx_ring_ = allocate_dma_view<TransmitDescriptor>(allocator_, kTransmitDescriptors, 4096);
  tx_buffers_ = allocate_dma_view<uint8_t>(allocator_, uint64_t{kTransmitDescriptors} * kBufferSize, 4096);
  if (tx_ring_.empty() || tx_buffers_.empty()) return false;
  for (uint32_t i = 0; i < kTransmitDescriptors; ++i)
    tx_ring_[i] = TransmitDescriptor{nic_dma_address(tx_buffer(i)), 0, 0, 0, 0, 0, 0};
  tx_clean_ = 0;
  __atomic_thread_fence(__ATOMIC_RELEASE);

  const uint64_t tx = nic_dma_address(tx_ring_);
  regs().write32(kTdbal, static_cast<uint32_t>(tx));
  regs().write32(kTdbah, static_cast<uint32_t>(tx >> 32));
  regs().write32(kTdlen, kTransmitDescriptors * sizeof(TransmitDescriptor));
  regs().write32(kTdh, 0);
  regs().write32(kTdt, 0);
  tx_next_ = 0;
  if (model_->kind == E1000eKind::PchSpt) {
    // I219 erratum: the transmit DMA can hang with three outstanding requests; e1000e drops it to two.
    regs().set_bits32(kIosfpc, kIosfpcRdmtsHex);
    regs().write32(kTarc0, (regs().read32(kTarc0) & ~kTarc0MultiQ3) | kTarc0MultiQ2);
  }
  regs().write32(kTipg, kTipgDefault);

  regs().set_bits32(kRxcsum, kRxcsumIp | kRxcsumTcpUdp);
  receive_control_ = kRctlEnable | kRctlBroadcast | kRctlStripCrc;  // 2 KiB buffers: BSIZE = 0
  regs().write32(kRctl, receive_control_);
  regs().write32(kTctl, kTctlEnable | kTctlPadShortPackets | kTctlRetransmitLateCollision | kTctlCollisionThreshold | kTctlCollisionDistance);
  started_ = true;
  return true;
}

void E1000eNetworkInterface::shutdown() {
  disable_interrupts();
  if (claimed_) {
    started_ = false;
    if (model_ && regs().read32(kStatus) != 0xFFFFFFFFu) {
      reset();  // the card stops using our memory
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
  release_view(allocator_, tx_ring_);
  release_view(allocator_, tx_buffers_);
}

// ---- running --------------------------------------------------------------------------------------------------------------------------------------------

bool E1000eNetworkInterface::link_up() const { return started_ && (regs().read32(kStatus) & kStatusLinkUp); }

uint32_t E1000eNetworkInterface::link_speed_mbps() const { return started_ ? status_speed_mbps(regs().read32(kStatus), false) : 0; }

LinkDuplex E1000eNetworkInterface::link_duplex() const { return started_ ? status_duplex(regs().read32(kStatus)) : LinkDuplex::Unknown; }

bool E1000eNetworkInterface::accept_all_multicast(bool enable) {
  kernel::ScopedMutex guard(mutex_);
  if (!started_) return false;
  receive_control_ = enable ? (receive_control_ | kRctlMulticastPromiscuous) : (receive_control_ & ~kRctlMulticastPromiscuous);
  regs().write32(kRctl, receive_control_);
  return enable;
}

bool E1000eNetworkInterface::enable_interrupts(NetworkWake wake, void* context) {
  kernel::ScopedMutex guard(mutex_);
  if (!started_ || !wake) return false;
  if (interrupts_enabled()) return true;
  if (enable_msix(wake, context)) return true;
  if (!interrupts_.enable(*device_, wake, context)) return false;
  write_throttle();
  return true;
}

// ITR: the shortest time between two interrupts, in 256 ns units.
void E1000eNetworkInterface::write_throttle() {
  const uint32_t interval = InterruptModerator::intel_interval(moderator_.latency());
  if (msix_vectors_) {
    for (uint32_t v = 0; v < 2; ++v) regs().write32(kEitr82574 + 4 * v, interval);
  } else {
    regs().write32(kItr, interval);
  }
}

void E1000eNetworkInterface::rearm_interrupts() {
  kernel::ScopedMutex guard(mutex_);
  if (!started_ || !interrupts_enabled()) return;
  if (moderator_.update()) write_throttle();
  if (msix_vectors_) regs().write32(kIms, kIntRxQueue0 | kIntRxQueue1 | kIntOther | kIntLinkChange);
  else interrupts_.rearm();
}

bool E1000eNetworkInterface::transmit(ConstByteView frame) { return transmit_frame(frame, TransmitChecksum{}); }

// Frames the card has sent give their descriptors back: DD is written back to the data descriptor that ends each (not to a context descriptor).
void E1000eNetworkInterface::reclaim_transmitted() {
  while (tx_clean_ != tx_next_) {
    const uint32_t end = tx_frame_end_[tx_clean_];
    volatile TransmitDescriptor& d = tx_ring_[end];
    if (!(d.status & kStatusDescriptorDone)) return;
    tx_clean_ = (end + 1) % kTransmitDescriptors;
  }
}

bool E1000eNetworkInterface::transmit_frame(ConstByteView frame, TransmitChecksum checksum) {
  kernel::ScopedMutex guard(mutex_);
  return send(frame, checksum, 0);
}

bool E1000eNetworkInterface::transmit_segmented(ConstByteView frame, TransmitChecksum checksum, uint16_t mss) {
  kernel::ScopedMutex guard(mutex_);
  if (mss == 0) {
    ++stats_.send_failures;
    return false;
  }
  return send(frame, checksum, mss);
}

// One frame, or (mss != 0) one TCP segment the card cuts into frames: a context descriptor when a checksum or segmentation is asked for, then
// the data in as many 2 KiB buffers as it takes (extended data descriptors; a plain frame takes one legacy descriptor), EOP and RS on the last.
bool E1000eNetworkInterface::send(ConstByteView frame, TransmitChecksum checksum, uint16_t mss) {
  const bool segment = mss != 0;
  TsoLayout layout;
  bool fits = started_ && !frame.empty();
  if (segment) fits = fits && frame.size() <= kMaxSegmentation && tso_layout(frame, checksum, &layout) && layout.header_length < 256;
  else fits = fits && frame.size() <= kEthernetMaxFrame && (!checksum.requested() || checksum.fits(frame.size()));
  if (!fits) {
    ++stats_.send_failures;
    return false;
  }
  const bool hardware = segment || (checksum.requested() && (checksum.tcp() || checksum.udp()) && checksum.field < 256);
  // Short frames are padded by the controller (TCTL.PSP), after the checksum: padding them here would put the padding in the TCP/UDP length
  // of a controller that works the checksum out from the frame's length (QEMU's does).
  const uint32_t length = static_cast<uint32_t>(frame.size());
  const uint32_t data_descriptors = (length + kBufferSize - 1) / kBufferSize;
  const uint32_t needed = data_descriptors + (hardware ? 1 : 0);
  reclaim_transmitted();
  const uint32_t in_flight = (tx_next_ + kTransmitDescriptors - tx_clean_) % kTransmitDescriptors;
  if (kTransmitDescriptors - 1 - in_flight < needed) {  // the card has not sent enough yet
    ++stats_.send_failures;
    return false;
  }

  const uint32_t first = tx_next_;
  uint32_t slot = first;
  if (hardware) {
    // The context: the transport checksum covers TUCSS to the end of the frame (TUCSE 0) and goes to TUCSO; with segmentation also the IP
    // header checksum (IPCSS to IPCSE, at IPCSO), the payload length, the header length and the MSS.
    volatile TransmitDescriptor& context = tx_ring_[slot];
    uint64_t setup = (uint64_t{checksum.start} << 32) | (uint64_t{checksum.field} << 40);
    if (segment && !layout.ipv6) setup |= kEthernetHeaderSize | (uint64_t{kEthernetHeaderSize + 10} << 8) | (uint64_t{layout.tcp_start - 1u} << 16);
    context.address = setup;
    context.length = segment ? static_cast<uint16_t>(layout.payload) : 0;
    context.checksum_offset = segment ? static_cast<uint8_t>((layout.payload >> 16) & 0x0F) : 0;  // (PAYLEN 19:16, DTYP 0: context)
    context.command = static_cast<uint8_t>(kCommandExtended | (checksum.tcp() ? kContextTcp : 0) | (segment ? kCommandSegmentation : 0) |
                                           (segment && !layout.ipv6 ? kContextIp : 0));  // (IP: an IPv4 header, whose checksum is inserted)
    context.status = 0;
    context.checksum_start = segment ? static_cast<uint8_t>(layout.header_length) : 0;  // (HDRLEN)
    context.special = segment ? mss : 0;                                                  // (MSS)
    slot = (slot + 1) % kTransmitDescriptors;
  }
  for (uint32_t offset = 0, n = 0; n < data_descriptors; ++n) {
    const uint32_t piece = length - offset < kBufferSize ? length - offset : kBufferSize;
    const ByteView buffer = tx_buffer(slot).first(piece);
    buffer.copy_from(frame.subview(offset, piece));
    if (n == 0) {
      if (segment && !prepare_tso_headers(buffer, layout)) return false;  // (cannot fail: the layout was checked)
      if (!hardware && checksum.requested() && !finish_transmit_checksum(buffer.first(frame.size()), checksum)) {
        ++stats_.send_failures;
        return false;
      }
    }
    const bool last = n + 1 == data_descriptors;
    volatile TransmitDescriptor& d = tx_ring_[slot];
    d.address = nic_dma_address(tx_buffer(slot));
    d.length = static_cast<uint16_t>(piece);
    d.status = 0;
    d.special = 0;
    const uint8_t end = last ? kCommandEndOfPacket | kCommandReportStatus : 0;
    if (hardware) {  // an extended data descriptor: DTYP 1, POPTS where the legacy one has CSS
      d.checksum_offset = kDataDescriptorType;
      d.checksum_start = static_cast<uint8_t>(kPoptsTransportChecksum | (segment && !layout.ipv6 ? kPoptsIpChecksum : 0));
      d.command = static_cast<uint8_t>(end | kCommandInsertCrc | kCommandExtended | (segment ? kCommandSegmentation : 0));
    } else {
      d.checksum_offset = 0;
      d.checksum_start = 0;
      d.command = static_cast<uint8_t>(end | kCommandInsertCrc);
    }
    if (last) tx_frame_end_[first] = static_cast<uint8_t>(slot);
    offset += piece;
    slot = (slot + 1) % kTransmitDescriptors;
  }
  __atomic_thread_fence(__ATOMIC_RELEASE);  // descriptor contents before the tail moves
  tx_next_ = slot;
  regs().write32(kTdt, tx_next_);
  ++stats_.frames_sent;
  if (segment) ++stats_.segmented_sends;
  moderator_.account(1, length);
  return true;
}

bool E1000eNetworkInterface::receive(ByteView buffer, uint32_t* length) {
  ReceivedFrame frame;
  if (!length || !receive_frame(buffer, &frame)) return false;
  *length = frame.length;
  return true;
}

// The queues in turn, so a busy one does not starve the others (one thread reading every queue); a stack with a thread per queue uses
// receive_frame_from() instead.
bool E1000eNetworkInterface::receive_frame(ByteView buffer, ReceivedFrame* received) {
  if (!started_ || !received) return false;
  const uint32_t turn = __atomic_load_n(&rx_turn_, __ATOMIC_RELAXED);
  for (uint32_t k = 0; k < queues_; ++k) {
    const uint32_t q = (turn + k) % queues_;
    if (receive_frame_from(q, buffer, received)) {
      __atomic_store_n(&rx_turn_, (q + 1) % queues_, __ATOMIC_RELAXED);
      return true;
    }
  }
  return false;
}

// One queue, under its own lock: several queues are read at once (and transmitted on meanwhile, under mutex_), each by one thread.
bool E1000eNetworkInterface::receive_frame_from(uint32_t q, ByteView buffer, ReceivedFrame* received) {
  if (!started_ || !received || q >= queues_) return false;
  kernel::ScopedMutex guard(rx_[q].lock);
  return receive_from(q, buffer, received);
}

bool E1000eNetworkInterface::receive_from(uint32_t q, ByteView buffer, ReceivedFrame* received) {
  ReceiveQueue& queue = rx_[q];
  for (uint32_t tries = 0; tries < kReceiveDescriptors; ++tries) {
    volatile ReceiveDescriptor& d = queue.ring[queue.next];
    const uint64_t upper = d.upper;
    // legacy: length 15:0, status 39:32, errors 47:40; extended: status 19:0 and errors 31:24, length 47:32 (the error bits are the same)
    const uint8_t status = static_cast<uint8_t>(extended_rx_ ? upper : upper >> 32);
    if (!(status & kStatusDescriptorDone)) return false;
    __atomic_thread_fence(__ATOMIC_ACQUIRE);

    // With 2 KiB buffers and no long-packet mode a frame always fits one descriptor; one that does not end there is dropped.
    const uint32_t frame = static_cast<uint16_t>(extended_rx_ ? upper >> 32 : upper);
    const uint8_t errors = static_cast<uint8_t>(extended_rx_ ? upper >> 24 : upper >> 40);
    const bool usable = (errors & ~(kErrorTcpUdpChecksum | kErrorIpChecksum)) == 0 && (status & kStatusEndOfPacket) && frame > 0 && frame <= kBufferSize;
    if (usable) {
      buffer.copy_from(ConstByteView(rx_buffer(queue, queue.next)).first(frame).at_most(buffer.size()));  // as much as fits
      const bool checked = !(status & kStatusIgnoreChecksum);
      *received = ReceivedFrame{frame, checked && (status & kStatusIpChecksum) && !(errors & kErrorIpChecksum),
                                checked && (status & (kStatusTcpChecksum | kStatusUdpChecksum)) && !(errors & kErrorTcpUdpChecksum)};
      __atomic_add_fetch(&stats_.frames_received, 1, __ATOMIC_RELAXED);   // (shared by the queues: counted atomically)
      ++queue.frames;
      {
        kernel::ScopedMutex shared(mutex_);   // (the moderator is the interface's, not the queue's)
        moderator_.account(1, frame);
      }
    } else {
      ++stats_.frames_dropped;
    }
    // give the descriptor back to the card (an extended write-back overwrote the address)
    d.address = nic_dma_address(rx_buffer(queue, queue.next));
    d.upper = 0;
    __atomic_thread_fence(__ATOMIC_RELEASE);
    regs().write32(rx_reg(q, kRdt), queue.next);
    queue.next = (queue.next + 1) % kReceiveDescriptors;
    if (usable) return true;
  }
  return false;
}

}  // namespace hardware
