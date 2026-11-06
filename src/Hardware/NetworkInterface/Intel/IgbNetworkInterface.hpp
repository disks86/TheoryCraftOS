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

#pragma once
#include "Allocators/IAllocator.hpp"
#include "Hardware/NetworkInterface/Intel/IntelEthernetRegisters.hpp"
#include "Hardware/NetworkInterface/INetworkInterface.hpp"
#include "Hardware/NetworkInterface/InterruptModeration.hpp"
#include "Hardware/NetworkInterface/NicDevice.hpp"
#include "Hardware/NetworkInterface/TcpSegmentation.hpp"
#include "Kernel/Sync.hpp"
#include "Utility/AllocatedView.hpp"
#include "Utility/UniquePointer.hpp"

namespace hardware {

// The Intel controllers with per-queue registers and "advanced" descriptors: the gigabit 82575, 82576, 82580, I350, I354, I210 and I211 (Linux's igb; the
// I210/I211 is the second port of many desktop boards and the usual add-in card) and the 2.5 Gb/s I225 and I226 (Linux's igc; the on-board port of most
// Intel desktop boards since 2020). Both use per-queue registers at 0xC000 (receive) and 0xE000 (transmit), 0x40 apart, and the same 16-byte advanced
// descriptors, which the device overwrites with its write-back format. The I225/I226 reset with DEV_RST instead of RST and report 2.5 Gb/s in a STATUS
// bit of its own. QEMU's `igb` device is an 82576.
//
// - Receive: up to four queues. With MSI-X each gets its own vector (on its own core) and RSS (Toeplitz hash of the IPv4 addresses and TCP/UDP
//   ports, 128-entry redirection table) spreads flows over them; without it everything goes to queue 0. receive() takes from the queues in turn.
// - Transmit: as many queues as receive queues, 256 descriptors (2 KiB buffers) each; a frame goes to the queue its flow hashes to (IPv4
//   addresses and TCP/UDP ports), so a connection that fills its ring does not hold up the others and each one's frames stay in order. The
//   transport checksum is finished through a context descriptor in front of the frame; TCP segmentation offload takes segments of up to
//   32 KiB (context with MSS, TSE, a chain of data descriptors).
// - Interrupts: MSI-X (a vector per receive queue and one for the link) or MSI. Either way an interrupt masks itself until rearm_interrupts()
//   (MSI-X: EIAM auto-mask, MSI: IMC), and the throttle (EITR) follows the traffic (InterruptModerator). The 82575 (no IVAR) uses MSI only.
enum class IgbKind : uint8_t { I82575, I82576, I82580, I350, I354, I210, I211, I225, I226 };

struct IgbModel {
  uint16_t device_id;
  IgbKind kind;
  const char* name;
};
const IgbModel* igb_find_model(uint16_t device_id);
// How many receive queues this driver runs on the model (the hardware has more on some; four are enough to spread a desktop's flows).
uint32_t igb_receive_queues(IgbKind kind);

class IgbNetworkInterface final : public INetworkInterface {
 public:
  static constexpr uint32_t kReceiveDescriptors = 256;
  static constexpr uint32_t kTransmitDescriptors = 256;
  static constexpr uint32_t kBufferSize = 2048;
  static constexpr uint32_t kMaxQueues = 4;
  static constexpr uint32_t kMaxSegmentation = 32768;

  IgbNetworkInterface(UniquePointer<INicDevice> device, allocators::IAllocator& allocator) : device_(util::move(device)), allocator_(allocator) {}
  ~IgbNetworkInterface() { shutdown(); }

  static bool supports(uint16_t vendor_id, uint16_t device_id) { return vendor_id == 0x8086 && igb_find_model(device_id); }

  const char* name() const override { return model_ ? model_->name : "Intel igb"; }
  bool initialize() override;
  void shutdown() override;

  MacAddress mac_address() const override { return mac_; }
  bool link_up() const override;
  uint32_t link_speed_mbps() const override;
  LinkDuplex link_duplex() const override;
  bool transmit(ConstByteView frame) override;
  bool receive(ByteView buffer, uint32_t* length) override;
  bool accept_all_multicast(bool enable) override;
  NetworkOffloads offloads() const override {
    return NetworkOffloads{true, true, kMaxSegmentation, static_cast<uint16_t>(msix_vectors_ ? queues_ : 1)};
  }
  bool receive_frame(ByteView buffer, ReceivedFrame* frame) override;
  bool transmit_frame(ConstByteView frame, TransmitChecksum checksum) override;
  bool transmit_segmented(ConstByteView frame, TransmitChecksum checksum, uint16_t mss) override;
  bool enable_interrupts(NetworkWake wake, void* context) override;
  void disable_interrupts() override;
  bool interrupts_enabled() const override { return msix_vectors_ != 0 || interrupts_.enabled(); }
  void rearm_interrupts() override;
  NetworkStatistics statistics() const override;

  const IgbModel* model() const { return model_; }
  // Whether initialize() could restart the PHY's autonegotiation.
  bool autonegotiation_restarted() const { return autonegotiation_restarted_; }
  uint32_t queue_count() const { return queues_; }
  // Frames taken from each receive queue so far (RSS at work).
  uint64_t queue_frames(uint32_t queue) const { return queue < kMaxQueues ? rx_[queue].frames : 0; }
  // Frames (and segments) sent on each transmit queue so far.
  uint64_t queue_sent(uint32_t queue) const { return queue < kMaxQueues ? tx_[queue].sent : 0; }
  // The transmit queue a frame goes to (of `queues`): its flow's hash; 0 for anything that is not IPv4.
  static uint32_t transmit_queue_for(ConstByteView frame, uint32_t queues);
  // MSI-X vectors in use (receive queues + the link), 0 with MSI or polled.
  uint32_t msix_vectors() const { return msix_vectors_; }

 private:
  // Read format: the buffer's address (and a header buffer, unused). Write-back: packet info, then status/errors (offset 8) and the length (offset 12).
  struct ReceiveDescriptor {
    uint64_t packet_address;
    uint64_t header_address;
  };
  // Read format: address, command/type/length, payload length and options. Write-back: the status (DD) lands in bit 0 of the last dword.
  // A context descriptor has the same size: MACLEN/IPLEN, the context type and protocol, MSS/L4LEN/index.
  struct TransmitDescriptor {
    uint64_t address;
    uint32_t command_type_length;
    uint32_t offload_status;
  };
  static_assert(sizeof(ReceiveDescriptor) == 16 && sizeof(TransmitDescriptor) == 16);
  struct TransmitQueue {
    ArrayView<TransmitDescriptor> ring;
    ByteView buffers;
    uint32_t next = 0;   // the next descriptor to fill
    uint32_t clean = 0;  // the first descriptor the device may still be using
    uint8_t frame_end[kTransmitDescriptors] = {};  // for the descriptor a frame starts at: the one it ends at
    uint64_t sent = 0;
  };
  static_assert(kTransmitDescriptors <= 256);
  struct ReceiveQueue {
    ArrayView<ReceiveDescriptor> ring;
    ByteView buffers;
    uint32_t next = 0;
    uint64_t frames = 0;
  };

  INicRegisters& regs() const { return device_->registers(); }
  bool igc() const { return model_->kind >= IgbKind::I225; }
  bool reset();
  void restart_autonegotiation();
  bool start_receive_queue(uint32_t q);
  void post_receive(ReceiveQueue& queue, uint32_t i);
  bool receive_from(uint32_t q, ByteView buffer, ReceivedFrame* received);
  bool start_transmit_queue(uint32_t q);
  void reclaim_transmitted(TransmitQueue& queue);
  bool send(ConstByteView frame, TransmitChecksum checksum, uint16_t mss);
  bool enable_msix(NetworkWake wake, void* context);
  void configure_rss();
  void write_ivar(uint32_t queue, uint32_t vector);
  void write_throttle();
  static void on_queue_interrupt(void* self, uint32_t vector);
  ByteView rx_buffer(const ReceiveQueue& queue, uint32_t i) const { return queue.buffers.subview(uint64_t{i} * kBufferSize, kBufferSize); }
  ByteView tx_buffer(const TransmitQueue& queue, uint32_t i) const { return queue.buffers.subview(uint64_t{i} * kBufferSize, kBufferSize); }

  UniquePointer<INicDevice> device_;
  allocators::IAllocator& allocator_;
  const IgbModel* model_ = nullptr;
  bool claimed_ = false;
  bool started_ = false;
  bool autonegotiation_restarted_ = false;
  uint32_t queues_ = 1;
  ReceiveQueue rx_[kMaxQueues];
  uint32_t rx_turn_ = 0;  // the queue receive() looks at first
  TransmitQueue tx_[kMaxQueues];
  uint32_t receive_control_ = 0;
  MacAddress mac_{};
  kernel::Mutex mutex_;  // transmit and receive may come from different threads
  NetworkStatistics stats_;
  // Interrupts: MSI through IntelInterrupts, or MSI-X here.
  intel_eth::IntelInterrupts interrupts_;
  uint32_t msix_vectors_ = 0;
  NetworkWake wake_ = nullptr;       // read by the interrupt handlers (atomic)
  void* wake_context_ = nullptr;
  uint64_t msix_interrupts_ = 0;     // atomic
  InterruptModerator moderator_;
};

}  // namespace hardware
