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

#include "Hardware/NetworkInterface/VMware/Vmxnet3NetworkInterface.hpp"
#include "Hardware/Bus/Pci/FirmwareDriverDetach.hpp"
#include "Kernel/Delay.hpp"

namespace hardware {

namespace {
// Control registers (BAR 1).
constexpr uint32_t kRegVrrs = 0x00, kRegUvrs = 0x08, kRegDsal = 0x10, kRegDsah = 0x18, kRegCmd = 0x20, kRegMacl = 0x28, kRegMach = 0x30;
// Producer registers (BAR 0).
constexpr uint32_t kRegImr0 = 0x000, kRegTxProd = 0x600, kRegRxProd = 0x800, kRegRxProd2 = 0xA00;

constexpr uint32_t kCmdActivate = 0xCAFE0000, kCmdReset = 0xCAFE0002, kCmdUpdateRxMode = 0xCAFE0003, kCmdGetLink = 0xF00D0002,
                   kCmdGetPermMacLo = 0xF00D0003, kCmdGetPermMacHi = 0xF00D0004;
constexpr uint32_t kRxModeUnicast = 1, kRxModeBroadcast = 4, kRxModeAllMulticast = 8;
constexpr uint32_t kSharedMagic = 0xBABEFEE1;

// Layout of the driver-shared block and the queue descriptors (byte offsets; the structures are packed).
constexpr uint32_t kSharedBytes = 736;
constexpr uint32_t kDsMagic = 0;
constexpr uint32_t kMisc = 8;                       // DriverInfo(16), uptFeatures, ddPA, queueDescPA, ddLen, queueDescLen, mtu, ...
constexpr uint32_t kMiscDriverVersion = kMisc + 0, kMiscGos = kMisc + 4, kMiscRevSupported = kMisc + 8, kMiscUptSupported = kMisc + 12,
                   kMiscQueueDescPa = kMisc + 32, kMiscQueueDescLen = kMisc + 44, kMiscMtu = kMisc + 48, kMiscMaxRxSg = kMisc + 52,
                   kMiscTxQueues = kMisc + 54, kMiscRxQueues = kMisc + 55;
constexpr uint32_t kIntr = kMisc + 72;              // autoMask, numIntrs, eventIntrIdx, modLevels[25], intrCtrl
constexpr uint32_t kIntrNum = kIntr + 1, kIntrCtrl = kIntr + 28;
constexpr uint32_t kRxFilter = kIntr + 40;
constexpr uint32_t kRxMode = kRxFilter;
constexpr uint32_t kQueueDescBytes = 512, kTxDescOffset = 0, kRxDescOffset = 256;

uint32_t rd32(const uint8_t* p) { return *reinterpret_cast<const volatile uint32_t*>(p); }
void wr32(uint8_t* p, uint32_t v) { *reinterpret_cast<volatile uint32_t*>(p) = v; }
void wr64(uint8_t* p, uint64_t v) { wr32(p, static_cast<uint32_t>(v)); wr32(p + 4, static_cast<uint32_t>(v >> 32)); }
void wr8(uint8_t* p, uint8_t v) { *reinterpret_cast<volatile uint8_t*>(p) = v; }
void wr16(uint8_t* p, uint16_t v) { *reinterpret_cast<volatile uint16_t*>(p) = v; }

uint64_t address_of(const void* p) { return reinterpret_cast<uint64_t>(p); }
uint32_t reg_read(uint64_t base, uint32_t off) { return *reinterpret_cast<volatile uint32_t*>(base + off); }
void reg_write(uint64_t base, uint32_t off, uint32_t v) { *reinterpret_cast<volatile uint32_t*>(base + off) = v; }
constexpr uint32_t align_up(uint32_t n, uint32_t a) { return (n + a - 1) & ~(a - 1); }
}  // namespace

uint32_t Vmxnet3NetworkInterface::command(uint32_t code) const {
  reg_write(vd_, kRegCmd, code);
  __atomic_thread_fence(__ATOMIC_SEQ_CST);
  return reg_read(vd_, kRegCmd);
}

bool Vmxnet3NetworkInterface::initialize() {
  if (!supports(device_.vendor_id, device_.device_id)) return false;
  pt_ = pci_.memory_bar(device_.address, 0);
  vd_ = pci_.memory_bar(device_.address, 1);
  if (!pt_ || !vd_) return false;

  detach_firmware_drivers(st_, device_.address);
  owns_device_ = true;
  pci_.write32(device_.address, 0x04, (pci_.read32(device_.address, 0x04) & 0xFFFF) | 0x6);   // memory and bus master

  // Revision 1 of the device interface and of the UPT (pass-through) interface.
  if (!(reg_read(vd_, kRegVrrs) & 1) || !(reg_read(vd_, kRegUvrs) & 1)) {
    shutdown();
    return false;
  }
  reg_write(vd_, kRegVrrs, 1);
  reg_write(vd_, kRegUvrs, 1);
  command(kCmdReset);

  // One allocation: shared block, queue descriptors, the six rings, then the frame buffers.
  uint32_t offset = 0;
  const uint32_t shared_off = offset;            offset += align_up(kSharedBytes, 128);
  const uint32_t queues_off = offset;            offset += kQueueDescBytes;
  const uint32_t tx_off = offset;                offset += kTxRing * 16;
  const uint32_t txc_off = offset;               offset += kTxRing * 16;
  const uint32_t rx_off = offset;                offset += kRxRing * 16;
  const uint32_t rx2_off = offset;               offset += kRxRing2 * 16;
  const uint32_t rxc_off = offset;               offset += (kRxRing + kRxRing2) * 16;
  offset = align_up(offset, 4096);
  const uint32_t tx_buf_off = offset;            offset += kTxRing * kBufferBytes;
  const uint32_t rx_buf_off = offset;            offset += kRxRing * kBufferBytes;
  memory_ = allocate_dma_view<uint8_t>(allocator_, offset, 4096);
  if (memory_.empty()) {
    shutdown();
    return false;
  }
  memory_.fill(0);
  uint8_t* base = memory_.data();
  shared_ = base + shared_off;
  uint8_t* queues = base + queues_off;
  tx_ = {reinterpret_cast<volatile uint32_t*>(base + tx_off), kTxRing, 0, 1};
  tx_comp_ = {reinterpret_cast<volatile uint32_t*>(base + txc_off), kTxRing, 0, 1};
  rx_ = {reinterpret_cast<volatile uint32_t*>(base + rx_off), kRxRing, 0, 1};
  rx_comp_ = {reinterpret_cast<volatile uint32_t*>(base + rxc_off), kRxRing + kRxRing2, 0, 1};
  tx_buffers_ = base + tx_buf_off;
  rx_buffers_ = base + rx_buf_off;

  // MAC address: the permanent one, written back as the current one.
  const uint32_t lo = command(kCmdGetPermMacLo), hi = command(kCmdGetPermMacHi);
  for (uint32_t i = 0; i < 4; ++i) mac_.bytes[i] = static_cast<uint8_t>(lo >> (8 * i));
  mac_.bytes[4] = static_cast<uint8_t>(hi);
  mac_.bytes[5] = static_cast<uint8_t>(hi >> 8);
  reg_write(vd_, kRegMacl, lo);
  reg_write(vd_, kRegMach, hi & 0xFFFF);

  // The transmit queue descriptor (ctrl 16 bytes, then conf) and the receive queue descriptor.
  uint8_t* tq = queues + kTxDescOffset;
  wr32(tq + 4, 1);                                  // txThreshold
  wr64(tq + 16, address_of(base + tx_off));         // txRingBasePA
  wr64(tq + 32, address_of(base + txc_off));        // compRingBasePA (dataRingBasePA at +24 stays 0)
  wr32(tq + 56, kTxRing);                           // txRingSize
  wr32(tq + 64, kTxRing);                           // compRingSize
  // (dataRingSize at +60 and the interrupt index at +72 stay 0)
  uint8_t* rq = queues + kRxDescOffset;
  wr64(rq + 16, address_of(base + rx_off));         // rxRingBasePA[0]
  wr64(rq + 24, address_of(base + rx2_off));        // rxRingBasePA[1]
  wr64(rq + 32, address_of(base + rxc_off));        // compRingBasePA
  wr32(rq + 56, kRxRing);                           // rxRingSize[0]
  wr32(rq + 60, kRxRing2);                          // rxRingSize[1]
  wr32(rq + 64, kRxRing + kRxRing2);                // compRingSize

  wr32(shared_ + kDsMagic, kSharedMagic);
  wr32(shared_ + kMiscDriverVersion, 0x01040000);
  wr32(shared_ + kMiscGos, 2 | (1u << 2));          // 64-bit, Linux
  wr32(shared_ + kMiscRevSupported, 1);
  wr32(shared_ + kMiscUptSupported, 1);
  wr64(shared_ + kMiscQueueDescPa, address_of(queues));
  wr32(shared_ + kMiscQueueDescLen, kQueueDescBytes);
  wr32(shared_ + kMiscMtu, 1500);
  wr16(shared_ + kMiscMaxRxSg, 1);
  wr8(shared_ + kMiscTxQueues, 1);
  wr8(shared_ + kMiscRxQueues, 1);
  wr8(shared_ + kIntrNum, 1);
  wr32(shared_ + kIntrCtrl, 1);                     // interrupts disabled: polled
  wr32(shared_ + kRxMode, kRxModeUnicast | kRxModeBroadcast | kRxModeAllMulticast);

  // Receive buffers: all but one descriptor of the first ring (the ring is full when the producer meets the consumer).
  for (uint32_t i = 0; i < kRxRing - 1; ++i) post_receive(i);

  const uint64_t shared_pa = address_of(shared_);
  reg_write(vd_, kRegDsal, static_cast<uint32_t>(shared_pa));
  reg_write(vd_, kRegDsah, static_cast<uint32_t>(shared_pa >> 32));
  if (command(kCmdActivate) != 0) {
    shutdown();
    return false;
  }
  reg_write(pt_, kRegImr0, 1);   // interrupt 0 masked
  command(kCmdUpdateRxMode);
  reg_write(pt_, kRegRxProd, rx_.next);
  reg_write(pt_, kRegRxProd2, 0);
  started_ = true;
  return true;
}

// Hands receive buffer `slot` (it is also the descriptor's index) to the device.
void Vmxnet3NetworkInterface::post_receive(uint32_t slot) {
  volatile uint32_t* d = rx_.words + 4 * rx_.next;
  const uint64_t pa = address_of(rx_buffers_ + uint64_t{slot} * kBufferBytes);
  d[0] = static_cast<uint32_t>(pa);
  d[1] = static_cast<uint32_t>(pa >> 32);
  d[3] = 0;
  __atomic_thread_fence(__ATOMIC_RELEASE);
  d[2] = kBufferBytes | (rx_.gen << 31);   // length, head buffer type, and the generation that gives the descriptor to the device
  rx_.advance();
}

void Vmxnet3NetworkInterface::shutdown() {
  started_ = false;
  if (vd_ && owns_device_) command(kCmdReset);   // the device lets go of our memory
  release_view(allocator_, memory_);
  shared_ = nullptr;
  if (owns_device_) {
    owns_device_ = false;
    reattach_firmware_drivers(st_, device_.address);
  }
}

bool Vmxnet3NetworkInterface::link_up() const { return started_ && (command(kCmdGetLink) & 1); }
uint32_t Vmxnet3NetworkInterface::link_speed_mbps() const {
  if (!started_) return 0;
  const uint32_t link = command(kCmdGetLink);
  return (link & 1) ? link >> 16 : 0;
}

// Transmit completions: each frees the descriptor it names (one per frame here).
void Vmxnet3NetworkInterface::reap_transmit() {
  for (;;) {
    volatile uint32_t* c = tx_comp_.words + 4 * tx_comp_.next;
    if (((c[3] >> 31) & 1) != tx_comp_.gen) return;
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    if (tx_inflight_ > 0) --tx_inflight_;
    tx_comp_.advance();
  }
}

bool Vmxnet3NetworkInterface::transmit(ConstByteView frame) {
  kernel::ScopedMutex guard(mutex_);
  if (!started_ || frame.empty() || frame.size() > kEthernetMaxFrame) {
    ++stats_.send_failures;
    return false;
  }
  reap_transmit();
  if (tx_inflight_ >= kTxRing - 1) {
    ++stats_.send_failures;
    return false;
  }
  const uint32_t length = frame.size() < kEthernetMinFrame ? kEthernetMinFrame : static_cast<uint32_t>(frame.size());
  const ByteView buffer = ByteView(tx_buffers_ + uint64_t{tx_.next} * kBufferBytes, kBufferBytes).first(length);
  buffer.fill(0);
  buffer.copy_from(frame);
  volatile uint32_t* d = tx_.words + 4 * tx_.next;
  const uint64_t pa = address_of(buffer.data());
  d[0] = static_cast<uint32_t>(pa);
  d[1] = static_cast<uint32_t>(pa >> 32);
  d[3] = (1u << 12) | (1u << 13);   // end of packet, completion wanted
  __atomic_thread_fence(__ATOMIC_RELEASE);
  d[2] = length | (tx_.gen << 14);
  tx_.advance();
  ++tx_inflight_;
  __atomic_thread_fence(__ATOMIC_SEQ_CST);
  reg_write(pt_, kRegTxProd, tx_.next);
  ++stats_.frames_sent;
  return true;
}

bool Vmxnet3NetworkInterface::receive(ByteView buffer, uint32_t* length) {
  kernel::ScopedMutex guard(mutex_);
  if (!started_ || !length) return false;
  for (;;) {
    volatile uint32_t* c = rx_comp_.words + 4 * rx_comp_.next;
    if (((c[3] >> 31) & 1) != rx_comp_.gen) return false;
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    const uint32_t w0 = c[0], w2 = c[2];
    const uint32_t index = w0 & 0xFFF;
    const bool sop = (w0 >> 15) & 1, eop = (w0 >> 14) & 1, error = (w2 >> 14) & 1;
    const uint32_t size = w2 & 0x3FFF;
    const uint32_t ring = (w0 >> 16) & 0x3FF;   // 0: the first ring
    rx_comp_.advance();
    const bool usable = ring == 0 && sop && eop && !error && index < kRxRing && size >= 14 && size <= kBufferBytes;
    bool delivered = false;
    if (usable) {
      buffer.copy_from(ConstByteView(rx_buffers_ + uint64_t{index} * kBufferBytes, size).at_most(buffer.size()));
      *length = size;
      ++stats_.frames_received;
      delivered = true;
    } else {
      ++stats_.frames_dropped;
    }
    // The consumed descriptor frees a slot; give the next never-posted buffer to the device.
    post_receive(rx_.next);
    reg_write(pt_, kRegRxProd, rx_.next);
    if (delivered) return true;
  }
}

}  // namespace hardware
