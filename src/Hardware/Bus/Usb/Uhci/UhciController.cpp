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

#include "Hardware/Bus/Usb/Uhci/UhciController.hpp"
#include "Hardware/Bus/Pci/FirmwareDriverDetach.hpp"
#include "Hardware/IoPort.hpp"

namespace hardware {

namespace {
constexpr uint32_t kPciCommand = 0x04, kPciLegacySupport = 0xC0, kPciBar4 = 0x20;
// Registers.
constexpr uint16_t kCmd = 0x00, kSts = 0x02, kIntr = 0x04, kFrNum = 0x06, kFrBase = 0x08, kSofMod = 0x0C;
constexpr uint16_t kCmdRun = 1, kCmdHostReset = 2, kCmdGlobalReset = 4, kCmdConfigure = 1 << 6, kCmdMaxPacket64 = 1 << 7;
constexpr uint16_t kStsHalted = 1 << 5;
// Port status/control.
constexpr uint16_t kPortConnected = 1 << 0, kPortConnectChange = 1 << 1, kPortEnabled = 1 << 2,
                   kPortEnableChange = 1 << 3, kPortAlwaysOne = 1 << 7, kPortLowSpeed = 1 << 8, kPortReset = 1 << 9;
// Link pointers.
constexpr uint32_t kTerminate = 1, kQueueHead = 2, kDepthFirst = 4;
// Transfer descriptor status/token.
constexpr uint32_t kTdActive = 1u << 23, kTdLowSpeed = 1u << 26, kTdErrorCount3 = 3u << 27;
constexpr uint32_t kTdErrors = 0x00760000;  // stalled, buffer error, babble, timeout/CRC, bit stuffing (NAK is not one)
constexpr uint32_t kPidSetup = 0x2D, kPidIn = 0x69, kPidOut = 0xE1;
constexpr uint32_t kControlTimeoutMs = 2000, kBulkTimeoutMs = 5000;

uint32_t low32(const void* p) { return static_cast<uint32_t>(reinterpret_cast<uint64_t>(p)); }
bool below_4g(const void* p, uint64_t length) { return reinterpret_cast<uint64_t>(p) + length <= 0x100000000ull; }
}  // namespace

uint16_t UhciController::read16(uint16_t reg) const { return IoPort(io_ + reg).read16(); }
void UhciController::write16(uint16_t reg, uint16_t value) const { IoPort(io_ + reg).write16(value); }

template <class T>
ArrayView<T> UhciController::low_allocate(uint64_t count, uint64_t alignment) {
  ArrayView<T> memory = allocate_dma_view<T>(allocator_, count, alignment);
  if (!memory.empty() && !below_4g(memory.data(), memory.size_bytes())) release_view(allocator_, memory);  // out of the controller's reach
  if (!memory.empty()) __builtin_memset(memory.data(), 0, memory.size_bytes());
  return memory;
}

// ---------------------------------------------------------------- bring-up

bool UhciController::initialize() {
  const uint32_t bar = pci_.read32(device_.address, kPciBar4);
  if (!(bar & 1)) return false;  // not an I/O BAR
  io_ = static_cast<uint16_t>(bar & 0xFFFC);

  detach_firmware_drivers(st_, device_.address);
  owns_controller_ = true;
  const uint32_t command = pci_.read32(device_.address, kPciCommand) & 0xFFFF;
  pci_.write32(device_.address, kPciCommand, command | 1u | (1u << 2));  // I/O space + bus master
  pci_.write32(device_.address, kPciLegacySupport, 0x8F00 | 0x2000);     // clear SMI status, PIRQ on

  // Global reset, then host-controller reset.
  write16(kCmd, kCmdGlobalReset);
  delay_us(50000);
  write16(kCmd, 0);
  delay_us(10000);
  write16(kCmd, kCmdHostReset);
  for (int i = 0; i < 100 && (read16(kCmd) & kCmdHostReset); ++i) delay_us(1000);
  if (read16(kCmd) & kCmdHostReset) return shutdown(), false;

  frame_list_ = low_allocate<uint32_t>(1024, 4096);
  head_ = make_queue_head();
  pool_ = low_allocate<Td>(kPoolTds, 16);
  control_buffer_ = low_allocate<uint8_t>(8192, 4096);
  bounce_ = low_allocate<uint8_t>(65536, 4096);
  if (frame_list_.empty() || !head_ || pool_.empty() || control_buffer_.empty() || bounce_.empty()) return shutdown(), false;
  head_->link = kTerminate;
  head_->element = kTerminate;
  for (uint32_t i = 0; i < 1024; ++i) frame_list_[i] = low32(head_) | kQueueHead;

  write16(kIntr, 0);  // polled
  write16(kSts, 0x3F);
  write16(kFrNum, 0);
  IoPort(io_ + kFrBase).write32(low32(frame_list_.data()));
  IoPort(io_ + kSofMod).write8(0x40);
  write16(kCmd, kCmdRun | kCmdConfigure | kCmdMaxPacket64);
  running_ = true;

  // The root ports: registers exist as long as bit 7 reads as 1.
  ports_ = 0;
  while (ports_ < kMaxPorts) {
    const uint16_t v = port_status(ports_);
    if (v == 0xFFFF || !(v & kPortAlwaysOne)) break;
    ++ports_;
  }
  delay_us(100000);  // connect debounce
  for (uint32_t port = 0; port < ports_; ++port) {
    if (!(port_status(port) & kPortConnected)) continue;
    bool low = false;
    if (reset_port(port, &low)) enumerate_device(port, low);
  }
  return true;
}

void UhciController::shutdown() {
  if (io_ && running_) {
    write16(kCmd, 0);
    for (int i = 0; i < 100 && !(read16(kSts) & kStsHalted); ++i) delay_us(100);
  }
  running_ = false;
  for (Device& d : devices_) {
    free_pipe(d.control);
    for (Pipe& p : d.pipes) free_pipe(p);
    d = Device{};
  }
  release_view(allocator_, frame_list_);
  allocator_.deallocate(head_);
  release_view(allocator_, pool_);
  release_view(allocator_, control_buffer_);
  release_view(allocator_, bounce_);
  head_ = nullptr;

  if (owns_controller_) {
    owns_controller_ = false;
    write16(kCmd, kCmdHostReset);  // it still points at the memory just freed
    for (int i = 0; i < 100 && (read16(kCmd) & kCmdHostReset); ++i) delay_us(1000);
    reattach_firmware_drivers(st_, device_.address);
  }
}

// ---------------------------------------------------------------- ports

bool UhciController::port_info(uint32_t port, UsbPortInfo* out) const {
  if (port >= ports_ || !out) return false;
  const uint16_t status = port_status(port);
  out->connected = status & kPortConnected;
  out->speed = (status & kPortLowSpeed) ? UsbSpeed::Low : UsbSpeed::Full;
  return true;
}

bool UhciController::reset_port(uint32_t port, bool* low_speed) {
  set_port(port, kPortReset);
  delay_us(50000);
  set_port(port, 0);
  delay_us(10000);
  // Enable the port and acknowledge the change bits (write-one-to-clear).
  set_port(port, kPortEnabled | kPortConnectChange | kPortEnableChange);
  delay_us(20000);  // reset recovery
  const uint16_t status = port_status(port);
  if (!(status & kPortConnected) || !(status & kPortEnabled)) return false;
  *low_speed = status & kPortLowSpeed;
  return true;
}

// ---------------------------------------------------------------- queue heads and descriptors

UhciController::Qh* UhciController::make_queue_head() {
  ArrayView<Qh> memory = low_allocate<Qh>(1, 16);
  Qh* qh = memory.data();
  if (qh) qh->link = qh->element = kTerminate;
  return qh;
}

// Inserts right after the list head; the new head points onward before anything points at it.
void UhciController::link_queue_head(Qh* qh) {
  qh->link = head_->link;
  __atomic_thread_fence(__ATOMIC_SEQ_CST);
  head_->link = low32(qh) | kQueueHead;
}

void UhciController::unlink_queue_head(Qh* qh) {
  Qh* prev = head_;
  for (int guard = 0; guard < 256; ++guard) {
    if (prev->link & kTerminate) return;
    Qh* next = reinterpret_cast<Qh*>(static_cast<uint64_t>(prev->link & ~0xFu));
    if (next == qh) {
      prev->link = qh->link;
      return;
    }
    prev = next;
  }
}

void UhciController::free_pipe(Pipe& p) {
  if (p.qh) {
    if (running_) {
      unlink_queue_head(p.qh);
      delay_us(2000);  // let the controller finish the frame it may be walking
    }
    allocator_.deallocate(p.qh);
  }
  allocator_.deallocate(p.td);
  release_view(allocator_, p.buffer);
  p = Pipe{};
}

void UhciController::fill_td(Td* td, uint32_t pid, uint8_t device_address, uint8_t endpoint, uint32_t toggle,
                             bool low_speed, ConstByteView buffer) {
  const uint64_t bytes = buffer.size();  // at most 1280 (a USB 1.1 packet is far smaller)
  td->link = kTerminate;
  td->status = kTdActive | kTdErrorCount3 | (low_speed ? kTdLowSpeed : 0);
  td->token = pid | (uint32_t{device_address} << 8) | (uint32_t{endpoint} << 15) | (toggle << 19) |
              (static_cast<uint32_t>(bytes ? bytes - 1 : 0x7FF) << 21);
  td->buffer = bytes ? low32(buffer.data()) : 0;
}

// Chains `count` descriptors depth-first onto a queue head and waits for all of them.
bool UhciController::run(Pipe& pipe, ArrayView<Td> descriptors, uint32_t timeout_ms) {
  const uint64_t count = descriptors.size();
  for (uint64_t i = 0; i + 1 < count; ++i) descriptors[i].link = low32(&descriptors[i + 1]) | kDepthFirst;
  __atomic_thread_fence(__ATOMIC_SEQ_CST);
  pipe.qh->element = low32(descriptors.data());

  bool done = false, failed = false;
  for (uint32_t waited = 0; waited <= timeout_ms * 10 && !done && !failed; ++waited) {
    done = true;
    for (Td& td : descriptors) {
      const uint32_t status = reinterpret_cast<volatile Td*>(&td)->status;
      if (status & kTdErrors) {
        failed = true;
        break;
      }
      if (status & kTdActive) {
        done = false;
        break;
      }
    }
    if (!done && !failed) delay_us(100);
  }
  if (!done || failed) {
    pipe.qh->element = kTerminate;
    delay_us(2000);  // out of the controller's hands before the descriptors are reused
    return false;
  }
  return true;
}

// ---------------------------------------------------------------- devices

LegacyHostController::DeviceState* UhciController::state(uint32_t port) const {
  return port < ports_ ? const_cast<Device*>(&devices_[port]) : nullptr;
}

bool UhciController::open_control_pipe(uint32_t port) {
  Pipe& p = devices_[port].control;
  p = Pipe{};
  p.qh = make_queue_head();
  if (!p.qh) return false;
  p.active = true;
  p.max_packet = 8;
  link_queue_head(p.qh);
  return true;
}

void UhciController::update_control_pipe(uint32_t port, uint8_t /*address*/, uint16_t max_packet) {
  devices_[port].control.max_packet = max_packet;
}

void UhciController::close_device(uint32_t port) {
  Device& d = devices_[port];
  free_pipe(d.control);
  for (Pipe& p : d.pipes) free_pipe(p);
}

bool UhciController::control_transfer(uint32_t port, uint8_t request_type, uint8_t request, uint16_t value,
                                      uint16_t index, ByteView data, bool data_in) {
  Device& d = devices_[port];
  Pipe& pipe = d.control;
  if (!pipe.qh || data.size() > 4096) return false;
  const uint16_t length = static_cast<uint16_t>(data.size());
  const ByteView setup = control_buffer_.first(8);
  const ByteView buffer = control_buffer_.subview(64, 4096);  // device-reachable staging area for the data stage
  ByteWriter packet(setup);
  packet.u8(request_type);
  packet.u8(request);
  packet.le16(value);
  packet.le16(index);
  packet.le16(length);
  if (data_in) buffer.first(length).fill(0);

  // Setup, the data packets, then the status stage: at most 2 + 4096/8 descriptors.
  uint32_t n = 0, toggle = 1;
  fill_td(&pool_[n++], kPidSetup, d.address, 0, 0, d.low_speed, setup);
  for (uint32_t offset = 0; offset < length; toggle ^= 1) {
    const uint32_t chunk = length - offset < pipe.max_packet ? length - offset : pipe.max_packet;
    fill_td(&pool_[n++], data_in ? kPidIn : kPidOut, d.address, 0, toggle, d.low_speed, buffer.subview(offset, chunk));
    offset += chunk;
  }
  fill_td(&pool_[n++], data_in ? kPidOut : kPidIn, d.address, 0, 1, d.low_speed, ConstByteView());  // status stage
  if (!run(pipe, pool_.first(n), kControlTimeoutMs)) return false;
  if (data_in && length) data.copy_from(buffer.first(length));
  return true;
}

UhciController::Pipe* UhciController::find_pipe(Device& d, uint8_t address) {
  for (Pipe& p : d.pipes)
    if (p.active && p.address == address) return &p;
  return nullptr;
}

bool UhciController::start_bulk_endpoint(uint32_t port, uint8_t endpoint_address, uint16_t max_packet) {
  kernel::ScopedMutex lock_guard(lock_);
  if (port >= ports_ || !devices_[port].present || !max_packet || max_packet > 64) return false;
  Device& d = devices_[port];
  if (find_pipe(d, endpoint_address)) return true;
  for (Pipe& p : d.pipes) {
    if (p.active) continue;
    p = Pipe{};
    p.qh = make_queue_head();
    if (!p.qh) return false;
    p.address = endpoint_address;
    p.max_packet = max_packet;
    p.active = true;
    link_queue_head(p.qh);
    return true;
  }
  return false;
}

bool UhciController::bulk_transfer(uint32_t port, uint8_t endpoint_address, ByteView data, uint32_t* transferred) {
  if (transferred) *transferred = 0;
  if (port >= ports_ || !devices_[port].present || data.size() > 0x10000) return false;
  Device& d = devices_[port];
  Pipe* pipe = find_pipe(d, endpoint_address);
  if (!pipe || pipe->interrupt) return false;

  const uint64_t length = data.size();
  const bool in = endpoint_address & 0x80;
  ByteView buffer = data;
  const bool bounced = length && !below_4g(data.data(), length);
  if (bounced) {  // the controller cannot address this buffer
    buffer = bounce_.first(length);
    if (!in) buffer.copy_from(data);
  }

  uint64_t offset = 0, moved = 0;
  bool first = true;
  while (first || offset < length) {
    first = false;
    uint32_t n = 0, toggle = pipe->toggle;
    const uint64_t batch_start = offset;
    do {
      const uint64_t chunk = length - offset < pipe->max_packet ? length - offset : pipe->max_packet;
      fill_td(&pool_[n++], in ? kPidIn : kPidOut, d.address, endpoint_address & 0x0F, toggle, d.low_speed,
              buffer.subview(offset, chunk));
      toggle ^= 1;
      offset += chunk;
    } while (offset < length && n < kBatchTds);
    if (!run(*pipe, pool_.first(n), kBulkTimeoutMs)) return false;
    pipe->toggle = static_cast<uint8_t>(toggle);
    if (in) {
      for (uint32_t i = 0; i < n; ++i) moved += (pool_[i].status + 1) & 0x7FF;  // actual length field is n-1
    } else {
      moved += offset - batch_start;
    }
  }
  if (bounced && in) data.copy_from(ConstByteView(buffer).first(moved < length ? moved : length));
  if (transferred) *transferred = static_cast<uint32_t>(moved);
  return true;
}

bool UhciController::reset_bulk_endpoint(uint32_t port, uint8_t endpoint_address) {
  kernel::ScopedMutex lock_guard(lock_);
  if (port >= ports_ || !devices_[port].present) return false;
  Pipe* pipe = find_pipe(devices_[port], endpoint_address);
  if (!pipe || pipe->interrupt) return false;
  pipe->qh->element = kTerminate;
  pipe->toggle = 0;  // CLEAR_FEATURE(ENDPOINT_HALT) resets the device's toggle too
  return true;
}

bool UhciController::start_interrupt_in(uint32_t port, uint8_t endpoint_address, uint16_t max_packet,
                                        uint8_t /*interval: every frame is polled*/) {
  kernel::ScopedMutex lock_guard(lock_);
  if (port >= ports_ || !devices_[port].present || !(endpoint_address & 0x80) || !max_packet || max_packet > 64)
    return false;
  Device& d = devices_[port];
  if (find_pipe(d, endpoint_address)) return true;
  for (Pipe& p : d.pipes) {
    if (p.active) continue;
    p = Pipe{};
    p.qh = make_queue_head();
    ArrayView<Td> td_memory = low_allocate<Td>(1, 16);
    p.td = td_memory.data();
    p.buffer = low_allocate<uint8_t>(64, 16);
    if (!p.qh || !p.td || p.buffer.empty()) {
      free_pipe(p);
      return false;
    }
    p.address = endpoint_address;
    p.max_packet = max_packet;
    p.interrupt = true;
    fill_td(p.td, kPidIn, d.address, endpoint_address & 0x0F, 0, d.low_speed, p.buffer.first(max_packet));
    p.qh->element = low32(p.td);  // armed before the queue head becomes visible
    link_queue_head(p.qh);
    p.active = true;
    return true;
  }
  return false;
}

bool UhciController::poll_interrupt_in(uint32_t port, uint8_t endpoint_address, ByteView buffer, uint16_t* length) {
  kernel::ScopedMutex lock_guard(lock_);
  if (port >= ports_ || !devices_[port].present) return false;
  Device& d = devices_[port];
  Pipe* pipe = find_pipe(d, endpoint_address);
  if (!pipe || !pipe->interrupt) return false;
  const uint32_t status = reinterpret_cast<volatile Td*>(pipe->td)->status;
  if (status & kTdActive) return false;  // nothing arrived yet
  const bool failed = status & kTdErrors;
  const uint32_t received = (status + 1) & 0x7FF;
  if (!failed) {
    buffer.copy_from(ConstByteView(pipe->buffer).first(received < pipe->max_packet ? received : pipe->max_packet).at_most(buffer.size()));
    if (length) *length = static_cast<uint16_t>(received);
    pipe->toggle ^= 1;
  }
  fill_td(pipe->td, kPidIn, d.address, endpoint_address & 0x0F, pipe->toggle, d.low_speed,
          pipe->buffer.first(pipe->max_packet));
  __atomic_thread_fence(__ATOMIC_SEQ_CST);
  pipe->qh->element = low32(pipe->td);
  return !failed;
}

}  // namespace hardware
