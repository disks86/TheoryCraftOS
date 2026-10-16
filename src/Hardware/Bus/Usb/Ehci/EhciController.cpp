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

#include "Kernel/Delay.hpp"
#include "Kernel/Sync.hpp"
#include "Hardware/Bus/Usb/Ehci/EhciController.hpp"
#include "Hardware/Bus/Pci/FirmwareDriverDetach.hpp"

namespace hardware {

using namespace ehci;

namespace {
constexpr uint32_t kPciCommand = 0x04;
constexpr uint32_t kPciMemorySpace = 1u << 1;
constexpr uint32_t kPciBusMaster = 1u << 2;
constexpr uint32_t kControlTimeoutMs = 2000;
constexpr uint32_t kBulkTimeoutMs = 5000;

uint32_t low(uint64_t v) { return static_cast<uint32_t>(v); }
uint32_t high(uint64_t v) { return static_cast<uint32_t>(v >> 32); }
uint64_t address_of(const void* p) { return reinterpret_cast<uint64_t>(p); }
}  // namespace

// ---------------------------------------------------------------- helpers

void EhciController::delay_us(uint64_t us) const { hardware::delay_us(us); }

bool EhciController::wait_for(uint64_t address, uint32_t mask, uint32_t value, uint32_t timeout_ms) {
  for (uint32_t waited = 0; waited <= timeout_ms * 10; ++waited) {
    if ((read32(address) & mask) == value) return true;
    delay_us(100);
  }
  return false;
}

// Data buffers need 64-bit addressing above 4 GiB. (Descriptors never go there:
// they are checked separately and the segment register stays 0.)
bool EhciController::reachable(const void* p, uint64_t length) const {
  return address64_ || address_of(p) + length <= 0x100000000ull;
}

// ---------------------------------------------------------------- bring-up

bool EhciController::initialize() {
  const uint64_t bar = pci_.memory_bar(device_.address, 0);
  if (!bar) return false;
  detach_firmware_drivers(st_, device_.address);
  owns_controller_ = true;
  const uint32_t command = pci_.read32(device_.address, kPciCommand) & 0xFFFF;
  pci_.write32(device_.address, kPciCommand, command | kPciMemorySpace | kPciBusMaster);

  cap_ = bar;
  op_ = cap_ + *reinterpret_cast<volatile uint8_t*>(cap_ + kCapLength);
  const uint32_t params = read32(cap_ + kCapHcsParams);
  ports_ = params & kHcsPortCount;
  if (ports_ > kMaxPorts) ports_ = kMaxPorts;
  address64_ = read32(cap_ + kCapHccParams) & kHccAddress64;

  if (!take_ownership() || !reset() || !setup_memory() || !start()) {
    shutdown();
    return false;
  }

  // Let devices announce themselves (connect debounce), then enumerate.
  if (params & kHcsPortPower) {
    for (uint32_t port = 0; port < ports_; ++port)
      write32(port_register(port), (read32(port_register(port)) & ~kPortWriteOneToClear) | kPortPower);
  }
  delay_us(200000);
  for (uint32_t port = 0; port < ports_; ++port)
    if (read32(port_register(port)) & kPortConnected) enumerate_port(port);
  return true;
}

// Takes the controller from the BIOS/SMM (USB Legacy Support, in PCI config space).
bool EhciController::take_ownership() {
  uint32_t offset = (read32(cap_ + kCapHccParams) >> 8) & 0xFF;
  for (int guard = 0; offset >= 0x40 && guard < 16; ++guard) {
    const uint32_t header = pci_.read32(device_.address, offset);
    if ((header & 0xFF) == 1) {
      if (header & kLegacyBiosOwned) {
        pci_.write32(device_.address, offset, header | kLegacyOsOwned);
        for (int i = 0; i < 1000 && (pci_.read32(device_.address, offset) & kLegacyBiosOwned); ++i) delay_us(1000);
        if (pci_.read32(device_.address, offset) & kLegacyBiosOwned) return false;
      }
      pci_.write32(device_.address, offset + 4, 0);  // no system-management interrupts
      return true;
    }
    offset = (header >> 8) & 0xFF;
  }
  return true;
}

bool EhciController::reset() {
  write32(op_ + kOpUsbCmd, read32(op_ + kOpUsbCmd) & ~kCmdRun);
  if (!wait_for(op_ + kOpUsbSts, kStsHalted, kStsHalted, 1000)) return false;
  write32(op_ + kOpUsbCmd, read32(op_ + kOpUsbCmd) | kCmdReset);
  return wait_for(op_ + kOpUsbCmd, kCmdReset, 0, 1000);
}

bool EhciController::setup_memory() {
  async_head_ = static_cast<Qh*>(allocator_.allocate_aligned(sizeof(Qh), 64));
  pool_ = static_cast<Qtd*>(allocator_.allocate_aligned(sizeof(Qtd) * kPoolQtds, 64));
  frame_list_ = static_cast<uint32_t*>(allocator_.allocate_aligned(kFrameListEntries * 4, 4096));
  buffer_ = static_cast<uint8_t*>(allocator_.allocate_aligned(8192, 4096));
  if (!async_head_ || !pool_ || !frame_list_ || !buffer_) return false;
  if ((address_of(async_head_) | address_of(pool_) | address_of(frame_list_)) >> 32) return false;
  if (!reachable(buffer_, 8192)) return false;

  __builtin_memset(async_head_, 0, sizeof(Qh));
  __builtin_memset(pool_, 0, sizeof(Qtd) * kPoolQtds);
  __builtin_memset(buffer_, 0, 8192);
  for (uint32_t i = 0; i < kFrameListEntries; ++i) frame_list_[i] = kLinkTerminate;
  // The list head: points to itself, marked as head of the reclamation list, never runs.
  async_head_->horizontal = low(address_of(async_head_)) | kLinkTypeQueueHead;
  async_head_->characteristics = kEndpointHeadOfList | kEndpointSpeedHigh | (64u << 16);
  async_head_->capabilities = 1u << 30;
  async_head_->overlay.next = kLinkTerminate;
  async_head_->overlay.alternate = kLinkTerminate;
  async_head_->overlay.token = kTokenHalted;
  return true;
}

bool EhciController::start() {
  write32(op_ + kOpUsbIntr, 0);  // polled
  write32(op_ + kOpCtrlDsSegment, 0);
  write32(op_ + kOpUsbSts, 0x3F);
  write32(op_ + kOpPeriodicListBase, low(address_of(frame_list_)));
  write32(op_ + kOpAsyncListAddr, low(address_of(async_head_)));
  write32(op_ + kOpUsbCmd, kCmdRun | kCmdAsyncEnable | kCmdPeriodicEnable | kCmdInterruptThreshold8);
  if (!wait_for(op_ + kOpUsbSts, kStsHalted, 0, 1000)) return false;
  running_ = true;
  write32(op_ + kOpConfigFlag, 1);  // route every port to this controller, not the companions
  return true;
}

void EhciController::shutdown() {
  if (cap_ && running_) {
    write32(op_ + kOpUsbCmd, read32(op_ + kOpUsbCmd) & ~(kCmdRun | kCmdAsyncEnable | kCmdPeriodicEnable));
    wait_for(op_ + kOpUsbSts, kStsHalted, kStsHalted, 1000);
  }
  running_ = false;
  for (Device& d : devices_) release_device(d);
  allocator_.deallocate(async_head_);
  allocator_.deallocate(pool_);
  allocator_.deallocate(frame_list_);
  allocator_.deallocate(buffer_);
  async_head_ = nullptr;
  pool_ = nullptr;
  frame_list_ = nullptr;
  buffer_ = nullptr;
  periodic_first_ = nullptr;

  // Give the controller back: reset it (it still points at the memory just freed)
  // and let the firmware's USB stack bind to it again.
  if (owns_controller_) {
    owns_controller_ = false;
    if (reset()) reattach_firmware_drivers(st_, device_.address);
  }
}

// ---------------------------------------------------------------- queue heads

Qh* EhciController::make_queue_head(uint8_t address, uint8_t endpoint, uint16_t max_packet, bool control) {
  Qh* qh = static_cast<Qh*>(allocator_.allocate_aligned(sizeof(Qh), 64));
  if (!qh) return nullptr;
  if (address_of(qh) >> 32) {
    allocator_.deallocate(qh);
    return nullptr;
  }
  __builtin_memset(qh, 0, sizeof(Qh));
  qh->characteristics = (address & 0x7F) | (uint32_t{endpoint} << 8) | kEndpointSpeedHigh |
                        (control ? kEndpointDataToggleFromTd : 0) | (uint32_t{max_packet} << 16);
  qh->capabilities = 1u << 30;  // one transaction per micro-frame
  qh->overlay.next = kLinkTerminate;
  qh->overlay.alternate = kLinkTerminate;
  return qh;
}

void EhciController::free_queue_head(Qh* qh) { allocator_.deallocate(qh); }

// Links a finished queue head in right after the list head; the controller may be
// walking the list, so the new head points onward before anything points at it.
void EhciController::link_async(Qh* qh) {
  qh->horizontal = async_head_->horizontal;
  __atomic_thread_fence(__ATOMIC_SEQ_CST);
  async_head_->horizontal = low(address_of(qh)) | kLinkTypeQueueHead;
}

// Interrupt queue heads form one chain that every frame-list entry starts at: a
// new head goes first, so each entry is valid before and after it is rewritten.
void EhciController::link_periodic(Qh* qh) {
  qh->horizontal = periodic_first_ ? low(address_of(periodic_first_)) | kLinkTypeQueueHead : kLinkTerminate;
  periodic_first_ = qh;
  __atomic_thread_fence(__ATOMIC_SEQ_CST);
  for (uint32_t i = 0; i < kFrameListEntries; ++i) frame_list_[i] = low(address_of(qh)) | kLinkTypeQueueHead;
}

// After a stall, error or timeout: park the queue head, wait for the controller
// to let go of it, then give it an empty, healthy queue (data toggle 0).
void EhciController::recover_queue_head(Qh* qh) {
  qh->overlay.token = kTokenHalted;
  __atomic_thread_fence(__ATOMIC_SEQ_CST);
  delay_us(2000);
  qh->overlay.next = kLinkTerminate;
  qh->overlay.alternate = kLinkTerminate;
  qh->overlay.token = 0;
}

void EhciController::fill_qtd(Qtd* qtd, uint32_t pid, uint64_t data, uint32_t bytes, uint32_t toggle,
                              bool interrupt_on_complete) {
  __builtin_memset(qtd, 0, sizeof(Qtd));
  qtd->next = kLinkTerminate;
  qtd->alternate = kLinkTerminate;
  qtd->token = kTokenActive | pid | kTokenErrorCount3 | (interrupt_on_complete ? kTokenInterruptOnComplete : 0) |
               (bytes << 16) | (toggle ? kTokenToggle : 0);
  if (!bytes) return;
  qtd->buffer[0] = low(data);
  qtd->buffer_high[0] = high(data);
  for (uint32_t i = 1; i < 5; ++i) {
    const uint64_t page = (data & ~0xFFFull) + uint64_t{i} * 4096;
    qtd->buffer[i] = low(page);
    qtd->buffer_high[i] = high(page);
  }
}

// Runs `count` consecutive pool descriptors on a queue head and waits for them.
// *remaining receives the bytes that were not transferred.
bool EhciController::run(Qh* qh, Qtd* first, uint32_t count, uint32_t timeout_ms, uint32_t* remaining) {
  for (uint32_t i = 0; i + 1 < count; ++i) first[i].next = low(address_of(&first[i + 1]));
  __atomic_thread_fence(__ATOMIC_SEQ_CST);
  qh->overlay.alternate = kLinkTerminate;
  qh->overlay.next = low(address_of(first));  // idle queue: the controller picks it up on its next visit

  bool done = false, failed = false;
  for (uint32_t waited = 0; waited <= timeout_ms * 10 && !done && !failed; ++waited) {
    done = true;
    for (uint32_t i = 0; i < count; ++i) {
      const uint32_t token = reinterpret_cast<volatile Qtd*>(&first[i])->token;
      if (token & (kTokenHalted | kTokenErrors)) {
        failed = true;
        break;
      }
      if (token & kTokenActive) done = false;
    }
    if (!done && !failed) delay_us(100);
  }
  if (!done || failed) {
    recover_queue_head(qh);
    return false;
  }
  uint32_t left = 0;
  for (uint32_t i = 0; i < count; ++i) left += (first[i].token >> 16) & 0x7FFF;
  if (remaining) *remaining = left;
  return true;
}

// ---------------------------------------------------------------- ports

bool EhciController::port_info(uint32_t port, UsbPortInfo* out) const {
  if (port >= ports_ || !out) return false;
  const uint32_t status = read32(port_register(port));
  out->connected = status & kPortConnected;
  out->speed = (status & kPortEnabled) ? UsbSpeed::High : UsbSpeed::Unknown;
  return true;
}

// Resets a port; a device that does not come up as high speed is handed to the
// companion controller. True when a high-speed device is enabled.
bool EhciController::reset_port(uint32_t port) {
  const uint64_t reg = port_register(port);
  uint32_t status = read32(reg);
  if (!(status & kPortConnected)) return false;
  if (((status & kPortLineStatusMask) >> kPortLineStatusShift) == 1) {  // K state: low-speed device
    write32(reg, (status & ~kPortWriteOneToClear) | kPortOwner);
    return false;
  }
  write32(reg, (status & ~(kPortWriteOneToClear | kPortEnabled)) | kPortReset);
  delay_us(50000);
  write32(reg, read32(reg) & ~(kPortWriteOneToClear | kPortReset));
  if (!wait_for(reg, kPortReset, 0, 200)) return false;
  delay_us(10000);  // reset recovery
  status = read32(reg);
  write32(reg, status | kPortWriteOneToClear);  // acknowledge the change bits
  if (!(status & kPortEnabled)) {               // full speed: the companion controller's business
    write32(reg, (status & ~kPortWriteOneToClear) | kPortOwner);
    return false;
  }
  return true;
}

// ---------------------------------------------------------------- devices

void EhciController::release_device(Device& d) {
  for (Endpoint& e : d.endpoints) {
    free_queue_head(e.qh);
    allocator_.deallocate(e.qtd);
    allocator_.deallocate(e.buffer);
    e = Endpoint{};
  }
  free_queue_head(d.control_qh);
  d.control_qh = nullptr;
  d.present = false;
  d.address = 0;
  d.configuration = 0;
  d.config_length = 0;
}

// Control transfer on endpoint 0: setup, optional data, status. With data_in the
// data lands at buffer_ + 4096; otherwise there is no data stage.
bool EhciController::control(Device& d, uint8_t request_type, uint8_t request, uint16_t value, uint16_t index,
                             uint16_t length, bool data_in) {
  if (!d.control_qh || length > 4096) return false;
  uint8_t* setup = buffer_;
  uint8_t* data = buffer_ + 4096;
  setup[0] = request_type;
  setup[1] = request;
  setup[2] = static_cast<uint8_t>(value);
  setup[3] = static_cast<uint8_t>(value >> 8);
  setup[4] = static_cast<uint8_t>(index);
  setup[5] = static_cast<uint8_t>(index >> 8);
  setup[6] = static_cast<uint8_t>(length);
  setup[7] = static_cast<uint8_t>(length >> 8);
  if (data_in) __builtin_memset(data, 0, length);

  uint32_t n = 0;
  fill_qtd(&pool_[n++], kTokenPidSetup, address_of(setup), 8, 0, false);
  if (data_in) fill_qtd(&pool_[n++], kTokenPidIn, address_of(data), length, 1, false);
  // The status stage runs opposite to the data: out after an IN data stage, in otherwise.
  fill_qtd(&pool_[n++], data_in ? kTokenPidOut : kTokenPidIn, 0, 0, 1, true);
  return run(d.control_qh, pool_, n, kControlTimeoutMs, nullptr);
}

bool EhciController::enumerate_port(uint32_t port) {
  Device& d = devices_[port];
  if (!reset_port(port)) return false;
  d.control_qh = make_queue_head(0, 0, 64, true);  // high-speed devices always have 64-byte EP0 packets
  if (!d.control_qh) return false;
  link_async(d.control_qh);

  const uint8_t address = static_cast<uint8_t>(port + 1);
  if (!control(d, 0x80, 6, 0x0100, 0, sizeof(UsbDeviceDescriptor), true)) return release_device(d), false;
  __builtin_memcpy(&d.descriptor, buffer_ + 4096, sizeof(UsbDeviceDescriptor));
  if (!control(d, 0x00, 5, address, 0, 0, false)) return release_device(d), false;
  delay_us(5000);  // the device needs a moment to adopt its address
  d.control_qh->characteristics = (d.control_qh->characteristics & ~0x7Fu) | address;
  d.address = address;
  d.present = true;

  // Cache the first configuration for class drivers; a device without a readable
  // one is still enumerated, just not usable by them.
  if (control(d, 0x80, 6, 0x0200, 0, 9, true)) {
    const uint16_t total = buffer_[4096 + 2] | (buffer_[4096 + 3] << 8);
    if (total >= 9 && total <= sizeof(d.config) && control(d, 0x80, 6, 0x0200, 0, total, true)) {
      __builtin_memcpy(d.config, buffer_ + 4096, total);
      d.config_length = total;
    }
  }
  return true;
}

// ---------------------------------------------------------- class driver API

bool EhciController::device_descriptor(uint32_t port, UsbDeviceDescriptor* out) const {
  if (port >= ports_ || !out || !devices_[port].present) return false;
  *out = devices_[port].descriptor;
  return true;
}

bool EhciController::configuration_descriptor(uint32_t port, uint8_t* buffer, uint16_t capacity,
                                              uint16_t* length) const {
  if (port >= ports_ || !devices_[port].present || !devices_[port].config_length || !buffer) return false;
  const Device& d = devices_[port];
  const uint16_t n = d.config_length < capacity ? d.config_length : capacity;
  __builtin_memcpy(buffer, d.config, n);
  if (length) *length = d.config_length;
  return true;
}

bool EhciController::set_configuration(uint32_t port, uint8_t configuration) {
  kernel::ScopedMutex lock_guard(lock_);
  if (port >= ports_ || !devices_[port].present) return false;
  Device& d = devices_[port];
  if (d.configuration == configuration) return true;
  if (!control(d, 0x00, 9, configuration, 0, 0, false)) return false;
  d.configuration = configuration;
  return true;
}

bool EhciController::control_out(uint32_t port, uint8_t request_type, uint8_t request, uint16_t value,
                                 uint16_t index) {
  kernel::ScopedMutex lock_guard(lock_);
  if (port >= ports_ || !devices_[port].present) return false;
  return control(devices_[port], request_type, request, value, index, 0, false);
}

bool EhciController::control_in(uint32_t port, uint8_t request_type, uint8_t request, uint16_t value,
                                uint16_t index, void* buffer, uint16_t length) {
  kernel::ScopedMutex lock_guard(lock_);
  if (port >= ports_ || !devices_[port].present || !buffer || length > 4096) return false;
  if (!control(devices_[port], request_type, request, value, index, length, true)) return false;
  __builtin_memcpy(buffer, buffer_ + 4096, length);
  return true;
}

EhciController::Endpoint* EhciController::find_endpoint(Device& d, uint8_t address) {
  for (Endpoint& e : d.endpoints)
    if (e.active && e.address == address) return &e;
  return nullptr;
}

bool EhciController::start_interrupt_in(uint32_t port, uint8_t endpoint_address, uint16_t max_packet,
                                        uint8_t /*interval: every frame is polled*/) {
  kernel::ScopedMutex lock_guard(lock_);
  if (port >= ports_ || !devices_[port].present || !(endpoint_address & 0x80) || !max_packet) return false;
  Device& d = devices_[port];
  if (find_endpoint(d, endpoint_address)) return true;
  Endpoint* e = nullptr;
  for (Endpoint& candidate : d.endpoints)
    if (!candidate.active && !candidate.qh) {
      e = &candidate;
      break;
    }
  if (!e) return false;

  e->qh = make_queue_head(d.address, endpoint_address & 0x0F, max_packet, false);
  e->qtd = static_cast<Qtd*>(allocator_.allocate_aligned(sizeof(Qtd), 64));
  e->buffer = static_cast<uint8_t*>(allocator_.allocate_aligned(max_packet < 64 ? 64 : max_packet, 64));
  if (!e->qh || !e->qtd || !e->buffer || (address_of(e->qtd) >> 32) || !reachable(e->buffer, max_packet)) {
    free_queue_head(e->qh);
    allocator_.deallocate(e->qtd);
    allocator_.deallocate(e->buffer);
    *e = Endpoint{};
    return false;
  }
  e->address = endpoint_address;
  e->max_packet = max_packet;
  e->interrupt = true;
  e->qh->capabilities |= 0x01;  // start-split mask: micro-frame 0 of every frame
  fill_qtd(e->qtd, kTokenPidIn, address_of(e->buffer), max_packet, 0, true);
  e->qh->overlay.next = low(address_of(e->qtd));  // armed before the head becomes visible
  link_periodic(e->qh);
  e->active = true;
  return true;
}

bool EhciController::poll_interrupt_in(uint32_t port, uint8_t endpoint_address, void* buffer, uint16_t capacity,
                                       uint16_t* length) {
  kernel::ScopedMutex lock_guard(lock_);
  if (port >= ports_ || !devices_[port].present || !buffer) return false;
  Endpoint* e = find_endpoint(devices_[port], endpoint_address);
  if (!e || !e->interrupt) return false;
  const uint32_t token = reinterpret_cast<volatile Qtd*>(e->qtd)->token;
  if (token & kTokenActive) return false;  // nothing arrived yet
  const bool failed = token & (kTokenHalted | kTokenErrors);
  const uint32_t received = e->max_packet - ((token >> 16) & 0x7FFF);
  if (!failed) {
    const uint16_t n = received < capacity ? received : capacity;
    __builtin_memcpy(buffer, e->buffer, n);
    if (length) *length = static_cast<uint16_t>(received);
  } else {
    recover_queue_head(e->qh);
  }
  fill_qtd(e->qtd, kTokenPidIn, address_of(e->buffer), e->max_packet, 0, true);
  e->qh->overlay.next = low(address_of(e->qtd));
  return !failed;
}

bool EhciController::start_bulk_endpoint(uint32_t port, uint8_t endpoint_address, uint16_t max_packet) {
  kernel::ScopedMutex lock_guard(lock_);
  if (port >= ports_ || !devices_[port].present || !max_packet) return false;
  Device& d = devices_[port];
  if (find_endpoint(d, endpoint_address)) return true;
  for (Endpoint& e : d.endpoints) {
    if (e.active || e.qh) continue;
    e.qh = make_queue_head(d.address, endpoint_address & 0x0F, max_packet, false);
    if (!e.qh) return false;
    e.address = endpoint_address;
    e.max_packet = max_packet;
    e.interrupt = false;
    link_async(e.qh);
    e.active = true;
    return true;
  }
  return false;
}

bool EhciController::bulk_transfer(uint32_t port, uint8_t endpoint_address, void* data, uint32_t length,
                                   uint32_t* transferred) {
  kernel::ScopedMutex lock_guard(lock_);
  if (transferred) *transferred = 0;
  if (port >= ports_ || !devices_[port].present || length > 0x10000) return false;
  Endpoint* e = find_endpoint(devices_[port], endpoint_address);
  if (!e || e->interrupt || (length && (!data || !reachable(data, length)))) return false;

  const uint32_t pid = (endpoint_address & 0x80) ? kTokenPidIn : kTokenPidOut;
  const uint64_t base = address_of(data);
  uint32_t n = 0, offset = 0;
  do {
    const uint32_t chunk = length - offset < kChunkBytes ? length - offset : kChunkBytes;
    offset += chunk;
    fill_qtd(&pool_[n++], pid, base + offset - chunk, chunk, 0, offset == length);  // toggle comes from the queue head
  } while (offset < length);

  uint32_t remaining = 0;
  if (!run(e->qh, pool_, n, kBulkTimeoutMs, &remaining)) return false;
  if (transferred) *transferred = length - remaining;
  return true;
}

bool EhciController::reset_bulk_endpoint(uint32_t port, uint8_t endpoint_address) {
  kernel::ScopedMutex lock_guard(lock_);
  if (port >= ports_ || !devices_[port].present) return false;
  Endpoint* e = find_endpoint(devices_[port], endpoint_address);
  if (!e || e->interrupt) return false;
  recover_queue_head(e->qh);
  return true;
}

}  // namespace hardware
