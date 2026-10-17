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

#include "Hardware/Bus/Usb/Ohci/OhciController.hpp"
#include "Hardware/Bus/Pci/FirmwareDriverDetach.hpp"

namespace hardware {

namespace {
constexpr uint32_t kPciCommand = 0x04;
// Registers.
constexpr uint32_t kRevision = 0x00, kControl = 0x04, kCommandStatus = 0x08, kInterruptStatus = 0x0C,
                   kInterruptDisable = 0x14, kHcca = 0x18, kControlHead = 0x20, kControlCurrent = 0x24,
                   kBulkHead = 0x28, kBulkCurrent = 0x2C, kFmInterval = 0x34, kPeriodicStart = 0x40,
                   kLsThreshold = 0x44, kRhDescriptorA = 0x48, kRhStatus = 0x50;
constexpr uint32_t kCtrlPeriodicEnable = 1u << 2, kCtrlControlEnable = 1u << 4, kCtrlBulkEnable = 1u << 5,
                   kCtrlOperational = 2u << 6, kCtrlInterruptRouting = 1u << 8;
constexpr uint32_t kCmdReset = 1u << 0, kCmdControlFilled = 1u << 1, kCmdBulkFilled = 1u << 2, kCmdOwnership = 1u << 3;
constexpr uint32_t kRhGlobalPower = 1u << 16;
// Root port status: reads and (write-to-set/clear) commands share the register.
constexpr uint32_t kPortConnected = 1u << 0, kPortEnabled = 1u << 1, kPortLowSpeed = 1u << 9;
constexpr uint32_t kPortSetEnable = 1u << 1, kPortSetReset = 1u << 4, kPortSetPower = 1u << 8;
constexpr uint32_t kPortConnectChange = 1u << 16, kPortEnableChange = 1u << 17, kPortResetChange = 1u << 20;
// Endpoint descriptor flags.
constexpr uint32_t kEdLowSpeed = 1u << 13, kEdSkip = 1u << 14;
// Transfer descriptor flags.
constexpr uint32_t kTdRounding = 1u << 18, kTdNoInterrupt = 7u << 21, kTdNotAccessed = 0xFu << 28;
constexpr uint32_t kDirSetup = 0, kDirOut = 1, kDirIn = 2;
constexpr uint32_t kToggleCarry = 0, kToggleData0 = 2, kToggleData1 = 3;
constexpr uint32_t kHeadHalted = 1, kHeadCarry = 2;
constexpr uint32_t kControlTimeoutMs = 2000, kBulkTimeoutMs = 5000;
constexpr uint32_t kMaxTdBytes = 4096;  // at most one page boundary inside a descriptor's buffer

uint32_t low32(const void* p) { return static_cast<uint32_t>(reinterpret_cast<uint64_t>(p)); }
bool below_4g(const void* p, uint64_t length) { return reinterpret_cast<uint64_t>(p) + length <= 0x100000000ull; }
}  // namespace

template <class T>
ArrayView<T> OhciController::low_allocate(uint64_t count, uint64_t alignment) {
  ArrayView<T> memory = allocate_dma_view<T>(allocator_, count, alignment);
  if (!memory.empty() && !below_4g(memory.data(), memory.size_bytes())) release_view(allocator_, memory);  // out of the controller's reach
  if (!memory.empty()) __builtin_memset(memory.data(), 0, memory.size_bytes());
  return memory;
}

// ---------------------------------------------------------------- bring-up

bool OhciController::initialize() {
  regs_ = pci_.memory_bar(device_.address, 0);
  if (!regs_) return false;
  detach_firmware_drivers(st_, device_.address);
  owns_controller_ = true;
  const uint32_t command = pci_.read32(device_.address, kPciCommand) & 0xFFFF;
  pci_.write32(device_.address, kPciCommand, command | (1u << 1) | (1u << 2));  // memory space + bus master
  if ((reg(kRevision) & 0xFF) != 0x10) return false;

  // An SMM/firmware driver may still own the controller (interrupt routing): ask for it.
  if (reg(kControl) & kCtrlInterruptRouting) {
    set_reg(kCommandStatus, kCmdOwnership);
    for (int i = 0; i < 1000 && (reg(kControl) & kCtrlInterruptRouting); ++i) delay_us(1000);
  }

  // Reset, keeping the frame interval the firmware chose (the reset itself clears it).
  uint32_t interval = reg(kFmInterval);
  const uint32_t frame_interval = (interval & 0x3FFF) ? (interval & 0x3FFF) : 0x2EDF;
  set_reg(kCommandStatus, kCmdReset);
  for (int i = 0; i < 100 && (reg(kCommandStatus) & kCmdReset); ++i) delay_us(100);
  if (reg(kCommandStatus) & kCmdReset) return shutdown(), false;

  hcca_ = low_allocate<Hcca>(1, 256).data();
  control_head_ = low_allocate<Ed>(1, 16).data();
  bulk_head_ = low_allocate<Ed>(1, 16).data();
  pool_ = low_allocate<Td>(kPoolTds, 16);
  control_buffer_ = low_allocate<uint8_t>(8192, 4096);
  bounce_ = low_allocate<uint8_t>(65536, 4096);
  if (!hcca_ || !control_head_ || !bulk_head_ || pool_.empty() || control_buffer_.empty() || bounce_.empty())
    return shutdown(), false;
  control_head_->flags = bulk_head_->flags = kEdSkip;  // anchors only, never run

  // The controller is suspended after the reset and must be operational within 2 ms.
  set_reg(kHcca, low32(hcca_));
  set_reg(kControlHead, low32(control_head_));
  set_reg(kBulkHead, low32(bulk_head_));
  set_reg(kControlCurrent, 0);
  set_reg(kBulkCurrent, 0);
  const uint32_t toggle = ~reg(kFmInterval) & (1u << 31);  // FrameIntervalToggle flips on every change
  set_reg(kFmInterval, toggle | (((frame_interval - 210) * 6 / 7) << 16) | frame_interval);
  set_reg(kPeriodicStart, frame_interval * 9 / 10);
  set_reg(kLsThreshold, 0x628);
  set_reg(kInterruptDisable, 0xC000007F);  // polled: no interrupt sources
  set_reg(kInterruptStatus, 0x7F);
  set_reg(kControl, kCtrlPeriodicEnable | kCtrlControlEnable | kCtrlBulkEnable | kCtrlOperational);
  running_ = true;

  ports_ = reg(kRhDescriptorA) & 0xFF;
  if (ports_ > kMaxPorts) ports_ = kMaxPorts;
  set_reg(kRhStatus, kRhGlobalPower);
  for (uint32_t port = 0; port < ports_; ++port) set_port(port, kPortSetPower);
  delay_us(uint64_t{(reg(kRhDescriptorA) >> 24) & 0xFF} * 2000 + 100000);  // power-on to power-good, then debounce
  for (uint32_t port = 0; port < ports_; ++port) {
    if (!(port_status(port) & kPortConnected)) continue;
    bool low = false;
    if (reset_port(port, &low)) enumerate_device(port, low);
  }
  return true;
}

void OhciController::shutdown() {
  if (regs_ && running_) set_reg(kControl, 0);  // UsbReset: all lists stop
  running_ = false;
  for (Device& d : devices_) {
    free_pipe(d.control);
    for (Pipe& p : d.pipes) free_pipe(p);
    d = Device{};
  }
  allocator_.deallocate(hcca_);
  allocator_.deallocate(control_head_);
  allocator_.deallocate(bulk_head_);
  release_view(allocator_, pool_);
  release_view(allocator_, control_buffer_);
  release_view(allocator_, bounce_);
  hcca_ = nullptr;
  control_head_ = bulk_head_ = periodic_first_ = nullptr;

  if (owns_controller_) {
    owns_controller_ = false;
    set_reg(kCommandStatus, kCmdReset);  // it still points at the memory just freed
    for (int i = 0; i < 100 && (reg(kCommandStatus) & kCmdReset); ++i) delay_us(100);
    reattach_firmware_drivers(st_, device_.address);
  }
}

// ---------------------------------------------------------------- ports

bool OhciController::port_info(uint32_t port, UsbPortInfo* out) const {
  if (port >= ports_ || !out) return false;
  const uint32_t status = port_status(port);
  out->connected = status & kPortConnected;
  out->speed = (status & kPortLowSpeed) ? UsbSpeed::Low : UsbSpeed::Full;
  return true;
}

bool OhciController::reset_port(uint32_t port, bool* low_speed) {
  set_port(port, kPortSetReset);
  for (int i = 0; i < 300 && !(port_status(port) & kPortResetChange); ++i) delay_us(1000);
  if (!(port_status(port) & kPortResetChange)) return false;
  set_port(port, kPortResetChange | kPortConnectChange | kPortEnableChange);  // write-one-to-clear
  if (!(port_status(port) & kPortEnabled)) set_port(port, kPortSetEnable);
  delay_us(20000);  // reset recovery
  const uint32_t status = port_status(port);
  if (!(status & kPortConnected) || !(status & kPortEnabled)) return false;
  *low_speed = status & kPortLowSpeed;
  return true;
}

// ---------------------------------------------------------------- endpoints

bool OhciController::create_pipe(Device& d, Pipe& p, List list, uint8_t endpoint_address, uint16_t max_packet) {
  p = Pipe{};
  p.list = list;
  p.address = endpoint_address;
  p.max_packet = max_packet;
  p.ed = low_allocate<Ed>(1, 16).data();
  p.spare[0] = low_allocate<Td>(1, 16).data();
  p.spare[1] = low_allocate<Td>(1, 16).data();
  if (!p.ed || !p.spare[0] || !p.spare[1]) {
    free_pipe(p);
    return false;
  }
  const uint32_t direction = list == List::Control ? 0 : ((endpoint_address & 0x80) ? kDirIn : kDirOut) + 0;
  p.ed->flags = d.address | (uint32_t{static_cast<uint8_t>(endpoint_address & 0x0F)} << 7) | (direction << 11) |
                (d.low_speed ? kEdLowSpeed : 0) | (uint32_t{max_packet} << 16);
  p.ed->tail = p.ed->head = low32(p.spare[0]);  // empty queue: head meets tail at the dummy
  p.ed->next = 0;
  return true;
}

// Publishes an endpoint to the controller. The new endpoint points onward before
// anything points at it, so the controller sees a valid list at every step.
void OhciController::link_pipe(Pipe& p) {
  switch (p.list) {
    case List::Control:
      p.ed->next = control_head_->next;
      __atomic_thread_fence(__ATOMIC_SEQ_CST);
      control_head_->next = low32(p.ed);
      break;
    case List::Bulk:
      p.ed->next = bulk_head_->next;
      __atomic_thread_fence(__ATOMIC_SEQ_CST);
      bulk_head_->next = low32(p.ed);
      break;
    case List::Interrupt:
      p.ed->next = periodic_first_ ? low32(periodic_first_) : 0;
      periodic_first_ = p.ed;
      __atomic_thread_fence(__ATOMIC_SEQ_CST);
      for (uint32_t i = 0; i < 32; ++i) hcca_->interrupt_table[i] = low32(p.ed);  // every frame
      break;
  }
}

void OhciController::unlink_pipe(Pipe& p) {
  p.ed->flags |= kEdSkip;
  Ed* prev = p.list == List::Control ? control_head_ : p.list == List::Bulk ? bulk_head_ : nullptr;
  if (prev) {
    for (int guard = 0; prev && guard < 256; ++guard) {
      if (prev->next == low32(p.ed)) {
        prev->next = p.ed->next;
        break;
      }
      prev = prev->next ? reinterpret_cast<Ed*>(static_cast<uint64_t>(prev->next)) : nullptr;
    }
  }  // interrupt endpoints stay chained (skipped) until shutdown
}

void OhciController::free_pipe(Pipe& p) {
  if (p.ed && running_) {
    unlink_pipe(p);
    delay_us(2000);  // let the controller finish the frame it may be walking
  }
  if (p.ed && p.list == List::Interrupt && running_) {
    // Skipped but still linked: keep the memory (a few bytes) rather than free what the list points to.
    p = Pipe{};
    return;
  }
  allocator_.deallocate(p.ed);
  allocator_.deallocate(p.spare[0]);
  allocator_.deallocate(p.spare[1]);
  release_view(allocator_, p.buffer);
  p = Pipe{};
}

void OhciController::fill_td(Td* td, uint32_t direction, uint32_t toggle, ConstByteView buffer, bool round) {
  const uint64_t bytes = buffer.size();
  td->flags = (round ? kTdRounding : 0) | (direction << 19) | kTdNoInterrupt | (toggle << 24) | kTdNotAccessed;
  td->current_buffer = bytes ? low32(buffer.data()) : 0;
  td->buffer_end = bytes ? low32(buffer.data()) + static_cast<uint32_t>(bytes) - 1 : 0;
  td->next = 0;
}

// Chains descriptors 0..count-1 (the first is the current dummy) and publishes
// them by moving the queue's tail to the other spare.
void OhciController::commit(Pipe& p, uint32_t count) {
  for (uint32_t i = 0; i < count; ++i) {
    Td* next = i + 1 < count ? descriptor(p, i + 1) : p.spare[p.dummy ^ 1];
    descriptor(p, i)->next = low32(next);
  }
  Td* tail = p.spare[p.dummy ^ 1];
  __builtin_memset(tail, 0, sizeof(Td));
  __atomic_thread_fence(__ATOMIC_SEQ_CST);
  p.ed->tail = low32(tail);
  if (p.list == List::Control) set_reg(kCommandStatus, kCmdControlFilled);
  else if (p.list == List::Bulk) set_reg(kCommandStatus, kCmdBulkFilled);
}

bool OhciController::wait(Pipe& p, uint32_t timeout_ms) {
  for (uint32_t waited = 0; waited <= timeout_ms * 10; ++waited) {
    const uint32_t head = reinterpret_cast<volatile Ed*>(p.ed)->head;
    if (head & kHeadHalted) break;
    if ((head & ~0xFu) == p.ed->tail) {
      p.dummy ^= 1;  // the old tail is the new dummy
      return true;
    }
    delay_us(100);
  }
  recover(p);
  return false;
}

// After a stall, error or timeout: stop the endpoint, empty its queue and clear
// the halt and toggle state.
void OhciController::recover(Pipe& p) {
  p.ed->flags |= kEdSkip;
  delay_us(2000);
  p.ed->head = p.ed->tail;
  p.dummy ^= 1;
  __atomic_thread_fence(__ATOMIC_SEQ_CST);
  p.ed->flags &= ~kEdSkip;
}

uint32_t OhciController::remaining(const Td* td) const {
  const uint32_t current = reinterpret_cast<const volatile Td*>(td)->current_buffer;
  return current ? td->buffer_end - current + 1 : 0;
}

// ---------------------------------------------------------------- devices

LegacyHostController::DeviceState* OhciController::state(uint32_t port) const {
  return port < ports_ ? const_cast<Device*>(&devices_[port]) : nullptr;
}

bool OhciController::open_control_pipe(uint32_t port) {
  Device& d = devices_[port];
  if (!create_pipe(d, d.control, List::Control, 0, 8)) return false;
  link_pipe(d.control);
  d.control.active = true;
  return true;
}

void OhciController::update_control_pipe(uint32_t port, uint8_t address, uint16_t max_packet) {
  Pipe& p = devices_[port].control;
  p.max_packet = max_packet;
  p.ed->flags = (p.ed->flags & ~(0x7Fu | (0x7FFu << 16))) | address | (uint32_t{max_packet} << 16);
}

void OhciController::close_device(uint32_t port) {
  Device& d = devices_[port];
  free_pipe(d.control);
  for (Pipe& p : d.pipes) free_pipe(p);
}

bool OhciController::control_transfer(uint32_t port, uint8_t request_type, uint8_t request, uint16_t value,
                                      uint16_t index, ByteView data, bool data_in) {
  Pipe& pipe = devices_[port].control;
  if (!pipe.ed || data.size() > 4096) return false;
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

  uint32_t n = 0;
  fill_td(descriptor(pipe, n++), kDirSetup, kToggleData0, setup, false);
  if (length) fill_td(descriptor(pipe, n++), data_in ? kDirIn : kDirOut, kToggleData1, buffer.first(length), data_in);
  fill_td(descriptor(pipe, n++), data_in ? kDirOut : kDirIn, kToggleData1, ConstByteView(), false);  // status stage
  commit(pipe, n);
  if (!wait(pipe, kControlTimeoutMs)) return false;
  if (data_in && length) data.copy_from(buffer.first(length));
  return true;
}

OhciController::Pipe* OhciController::find_pipe(Device& d, uint8_t address) {
  for (Pipe& p : d.pipes)
    if (p.active && p.address == address) return &p;
  return nullptr;
}

bool OhciController::start_bulk_endpoint(uint32_t port, uint8_t endpoint_address, uint16_t max_packet) {
  kernel::ScopedMutex lock_guard(lock_);
  if (port >= ports_ || !devices_[port].present || !max_packet || max_packet > 64) return false;
  Device& d = devices_[port];
  if (find_pipe(d, endpoint_address)) return true;
  for (Pipe& p : d.pipes) {
    if (p.active || p.ed) continue;
    if (!create_pipe(d, p, List::Bulk, endpoint_address, max_packet)) return false;
    link_pipe(p);
    p.active = true;
    return true;
  }
  return false;
}

bool OhciController::bulk_transfer(uint32_t port, uint8_t endpoint_address, ByteView data, uint32_t* transferred) {
  kernel::ScopedMutex lock_guard(lock_);
  if (transferred) *transferred = 0;
  if (port >= ports_ || !devices_[port].present || data.size() > 0x10000) return false;
  Pipe* pipe = find_pipe(devices_[port], endpoint_address);
  if (!pipe || pipe->list != List::Bulk) return false;

  const uint64_t length = data.size();
  const bool in = endpoint_address & 0x80;
  ByteView buffer = data;
  const bool bounced = length && !below_4g(data.data(), length);
  if (bounced) {  // the controller cannot address this buffer
    buffer = bounce_.first(length);
    if (!in) buffer.copy_from(data);
  }

  uint32_t n = 0;
  uint64_t offset = 0;
  do {
    const uint64_t chunk = length - offset < kMaxTdBytes ? length - offset : kMaxTdBytes;
    fill_td(descriptor(*pipe, n++), in ? kDirIn : kDirOut, kToggleCarry, buffer.subview(offset, chunk), in);
    offset += chunk;
  } while (offset < length);
  Td* tds[kPoolTds + 1];
  for (uint32_t i = 0; i < n; ++i) tds[i] = descriptor(*pipe, i);
  commit(*pipe, n);
  if (!wait(*pipe, kBulkTimeoutMs)) return false;

  uint32_t left = 0;
  for (uint32_t i = 0; i < n; ++i) left += remaining(tds[i]);
  const uint64_t moved = left <= length ? length - left : 0;
  if (bounced && in) data.copy_from(ConstByteView(buffer).first(moved));
  if (transferred) *transferred = static_cast<uint32_t>(moved);
  return true;
}

bool OhciController::reset_bulk_endpoint(uint32_t port, uint8_t endpoint_address) {
  kernel::ScopedMutex lock_guard(lock_);
  if (port >= ports_ || !devices_[port].present) return false;
  Pipe* pipe = find_pipe(devices_[port], endpoint_address);
  if (!pipe || pipe->list != List::Bulk) return false;
  pipe->ed->head = pipe->ed->tail & ~(kHeadHalted | kHeadCarry);  // empty, not halted, toggle 0
  return true;
}

bool OhciController::start_interrupt_in(uint32_t port, uint8_t endpoint_address, uint16_t max_packet,
                                        uint8_t /*interval: every frame is polled*/) {
  kernel::ScopedMutex lock_guard(lock_);
  if (port >= ports_ || !devices_[port].present || !(endpoint_address & 0x80) || !max_packet || max_packet > 64)
    return false;
  Device& d = devices_[port];
  if (find_pipe(d, endpoint_address)) return true;
  for (Pipe& p : d.pipes) {
    if (p.active || p.ed) continue;
    if (!create_pipe(d, p, List::Interrupt, endpoint_address, max_packet)) return false;
    p.buffer = low_allocate<uint8_t>(64, 16);
    if (p.buffer.empty()) {
      free_pipe(p);
      return false;
    }
    p.armed = descriptor(p, 0);
    fill_td(p.armed, kDirIn, kToggleCarry, p.buffer.first(max_packet), true);
    commit(p, 1);  // armed before the endpoint becomes visible
    link_pipe(p);
    p.active = true;
    return true;
  }
  return false;
}

bool OhciController::poll_interrupt_in(uint32_t port, uint8_t endpoint_address, ByteView buffer, uint16_t* length) {
  kernel::ScopedMutex lock_guard(lock_);
  if (port >= ports_ || !devices_[port].present) return false;
  Pipe* pipe = find_pipe(devices_[port], endpoint_address);
  if (!pipe || pipe->list != List::Interrupt) return false;
  const uint32_t head = reinterpret_cast<volatile Ed*>(pipe->ed)->head;
  const bool halted = head & kHeadHalted;
  if (!halted && (head & ~0xFu) != pipe->ed->tail) return false;  // nothing arrived yet

  const uint32_t left = remaining(pipe->armed);
  const uint32_t received = left <= pipe->max_packet ? pipe->max_packet - left : 0;  // the controller's word, kept in range
  if (!halted) {
    buffer.copy_from(ConstByteView(pipe->buffer).first(received).at_most(buffer.size()));  // as much as fits
    if (length) *length = static_cast<uint16_t>(received);
    pipe->dummy ^= 1;
  } else {
    recover(*pipe);
  }
  pipe->armed = descriptor(*pipe, 0);
  fill_td(pipe->armed, kDirIn, kToggleCarry, pipe->buffer.first(pipe->max_packet), true);
  commit(*pipe, 1);
  return !halted;
}

}  // namespace hardware
