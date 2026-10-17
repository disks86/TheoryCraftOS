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
#include "Hardware/Bus/Usb/Xhci/XhciController.hpp"
#include "Hardware/Bus/Pci/FirmwareDriverDetach.hpp"

namespace hardware {

using namespace xhci;

namespace {
constexpr uint32_t kPciCommand = 0x04;
constexpr uint32_t kPciMemorySpace = 1u << 1;
constexpr uint32_t kPciBusMaster = 1u << 2;

constexpr uint32_t kRingTimeoutMs = 1000;
constexpr uint32_t kMaxExtCaps = 64;

// Control-transfer defaults per port speed (PSIV): full speed starts at 64 and
// is corrected after reading the first 8 descriptor bytes.
uint16_t default_max_packet(uint32_t speed_id) {
  switch (speed_id) {
    case 2:  return 8;    // low speed
    case 1:  return 64;   // full speed
    case 3:  return 64;   // high speed
    default: return 512;  // SuperSpeed and up
  }
}

UsbSpeed to_usb_speed(uint32_t id) {
  switch (id) {
    case 1:  return UsbSpeed::Full;
    case 2:  return UsbSpeed::Low;
    case 3:  return UsbSpeed::High;
    case 4:  return UsbSpeed::Super;
    case 5:  return UsbSpeed::SuperPlus;
    default: return UsbSpeed::Unknown;
  }
}
}  // namespace

// ---------------------------------------------------------------- helpers

void XhciController::delay_us(uint64_t us) const { hardware::delay_us(us); }

bool XhciController::wait_for(uint64_t address, uint32_t mask, uint32_t value, uint32_t timeout_ms) {
  for (uint32_t waited = 0; waited <= timeout_ms * 10; ++waited) {
    if ((read32(address) & mask) == value) return true;
    delay_us(100);
  }
  return false;
}

uint64_t XhciController::port_register(uint32_t port) const {
  return op_ + kOpPortBase + uint64_t{port} * kPortStride;
}

void XhciController::ring_doorbell(uint32_t slot, uint32_t target) {
  write32(doorbells_ + uint64_t{slot} * 4, target);
}

// ---------------------------------------------------------------- bring-up

bool XhciController::initialize() {
  const uint64_t bar = pci_.memory_bar(device_.address, 0);
  if (!bar) return false;

  // The firmware's own xHCI driver must let go before we reset the controller.
  detach_firmware_drivers(st_, device_.address);
  owns_controller_ = true;

  // Memory decoding and bus mastering (DMA) must be on. Only the low command
  // bits are written: the upper half of this dword is write-1-to-clear status.
  const uint32_t command = pci_.read32(device_.address, kPciCommand) & 0xFFFF;
  pci_.write32(device_.address, kPciCommand, command | kPciMemorySpace | kPciBusMaster);

  cap_ = bar;
  const uint32_t cap_length = *reinterpret_cast<volatile uint8_t*>(cap_ + kCapLength);
  version_ = *reinterpret_cast<volatile uint16_t*>(cap_ + kCapVersion);
  const uint32_t params1 = read32(cap_ + kCapHcsParams1);
  const uint32_t params2 = read32(cap_ + kCapHcsParams2);
  const uint32_t hcc = read32(cap_ + kCapHccParams1);
  op_ = cap_ + cap_length;
  runtime_ = cap_ + (read32(cap_ + kCapRuntimeOffset) & ~0x1Fu);
  doorbells_ = cap_ + (read32(cap_ + kCapDoorbellOffset) & ~0x3u);
  max_slots_ = params1 & 0xFF;
  ports_ = (params1 >> 24) & 0xFF;
  if (ports_ > kMaxPorts) ports_ = kMaxPorts;
  context_size_ = (hcc & kHccContextSize64) ? 64 : 32;
  scratchpad_count_ = (((params2 >> 21) & 0x1F) << 5) | ((params2 >> 27) & 0x1F);

  ac64_ = hcc & kHccAc64;

  if (!take_ownership() || !reset() || !setup_memory() || !start()) {
    shutdown();
    return false;
  }

  for (uint32_t port = 0; port < ports_; ++port)
    if (read32(port_register(port)) & kPortConnected) enumerate_port(port);
  return true;
}

// Hands the controller from the BIOS/SMM to the OS (USB Legacy Support).
bool XhciController::take_ownership() {
  uint32_t xecp = (read32(cap_ + kCapHccParams1) >> 16) & 0xFFFF;  // in dwords
  for (uint32_t guard = 0; xecp && guard < kMaxExtCaps; ++guard) {
    const uint64_t cap = cap_ + uint64_t{xecp} * 4;
    const uint32_t header = read32(cap);
    if ((header & 0xFF) == kExtCapLegacySupport) {
      if (header & kLegacyBiosOwned) {
        write32(cap, header | kLegacyOsOwned);
        if (!wait_for(cap, kLegacyBiosOwned, 0, 1000)) return false;
      }
      return true;
    }
    const uint32_t next = (header >> 8) & 0xFF;
    if (!next) break;
    xecp += next;
  }
  return true;  // no legacy-support capability: nothing to hand over
}

bool XhciController::reset() {
  // Stop first (a controller left running cannot be reset), then reset.
  write32(op_ + kOpUsbCmd, read32(op_ + kOpUsbCmd) & ~kCmdRun);
  if (!wait_for(op_ + kOpUsbSts, kStsHalted, kStsHalted, 1000)) return false;
  write32(op_ + kOpUsbCmd, read32(op_ + kOpUsbCmd) | kCmdReset);
  if (!wait_for(op_ + kOpUsbCmd, kCmdReset, 0, 1000)) return false;
  return wait_for(op_ + kOpUsbSts, kStsNotReady, 0, 1000);
}

// Controllers without 64-bit addressing can only reach the first 4 GiB.
bool XhciController::dma_reachable(const void* p) const {
  return ac64_ || reinterpret_cast<uint64_t>(p) + 0x10000 <= 0x100000000ull;
}

bool XhciController::setup_memory() {
  write32(op_ + kOpConfig, max_slots_);  // enable every slot the controller has

  dcbaa_ = static_cast<uint64_t*>(allocator_.allocate_dma((max_slots_ + 1) * 8, 4096));
  buffer_ = allocate_dma_view<uint8_t>(allocator_, 4096, 4096);
  if (!dcbaa_ || buffer_.empty() || !dma_reachable(dcbaa_)) return false;
  __builtin_memset(dcbaa_, 0, (max_slots_ + 1) * 8);

  if (scratchpad_count_) {  // the controller keeps private state in these pages
    if (scratchpad_count_ > 32) return false;
    scratchpad_table_ = static_cast<uint64_t*>(allocator_.allocate_dma(scratchpad_count_ * 8, 64));
    if (!scratchpad_table_) return false;
    for (uint32_t i = 0; i < scratchpad_count_; ++i) {
      scratchpad_pages_[i] = static_cast<uint8_t*>(allocator_.allocate_dma(4096, 4096));
      if (!scratchpad_pages_[i]) return false;
      __builtin_memset(scratchpad_pages_[i], 0, 4096);
      scratchpad_table_[i] = reinterpret_cast<uint64_t>(scratchpad_pages_[i]);
    }
    dcbaa_[0] = reinterpret_cast<uint64_t>(scratchpad_table_);
  }
  write64(op_ + kOpDcbaap, reinterpret_cast<uint64_t>(dcbaa_));

  if (!command_ring_.init(allocator_)) return false;
  write64(op_ + kOpCrcr, command_ring_.address() | command_ring_.cycle());  // RCS = 1

  if (!event_ring_.init(allocator_)) return false;
  const uint64_t interrupter = runtime_ + kRtInterrupter0;
  write32(interrupter + kIrErstSize, 1);
  write64(interrupter + kIrDequeue, event_ring_.dequeue_address());
  write64(interrupter + kIrErstBase, event_ring_.segment_table_address());  // enables the ring
  return true;
}

bool XhciController::start() {
  write32(op_ + kOpUsbCmd, read32(op_ + kOpUsbCmd) | kCmdRun);
  if (!wait_for(op_ + kOpUsbSts, kStsHalted, 0, 1000)) return false;
  running_ = true;

  // Prove the command and event rings work before trusting them.
  Trb completion;
  return run_command(0, kTrbNoop << 10, &completion);
}

void XhciController::shutdown() {
  for (Device& d : devices_) release_device(d);
  if (cap_ && running_) {
    write32(op_ + kOpUsbCmd, read32(op_ + kOpUsbCmd) & ~kCmdRun);
    wait_for(op_ + kOpUsbSts, kStsHalted, kStsHalted, 1000);
  }
  running_ = false;
  command_ring_.release(allocator_);
  event_ring_.release(allocator_);
  for (uint8_t*& page : scratchpad_pages_) {
    allocator_.deallocate(page);
    page = nullptr;
  }
  allocator_.deallocate(scratchpad_table_);
  allocator_.deallocate(dcbaa_);
  release_view(allocator_, buffer_);
  scratchpad_table_ = nullptr;
  dcbaa_ = nullptr;

  // Give the controller back: reset it (it still points at the memory we just
  // freed) and let the firmware's USB stack bind to it again.
  if (owns_controller_) {
    owns_controller_ = false;
    if (reset()) reattach_firmware_drivers(st_, device_.address);
  }
}

// ---------------------------------------------------------------- events

bool XhciController::wait_event(uint32_t type, uint64_t pointer, Trb* out, uint32_t timeout_ms) {
  const uint64_t interrupter = runtime_ + kRtInterrupter0;
  for (uint32_t waited = 0; waited <= timeout_ms * 10; ++waited) {
    Trb event;
    while (event_ring_.pop(&event)) {
      // Tell the controller how far we have read (and clear the busy flag).
      write64(interrupter + kIrDequeue, event_ring_.dequeue_address() | kIrDequeueBusy);
      if (trb_type(event.control) == type && event.parameter == pointer) {
        *out = event;
        return true;
      }
      handle_event(event);  // interrupt-endpoint data that arrived while we waited
    }
    delay_us(100);
  }
  return false;
}

bool XhciController::run_command(uint64_t parameter, uint32_t control, Trb* completion) {
  const uint64_t trb = command_ring_.push(parameter, 0, control);
  ring_doorbell(0, 0);  // doorbell 0 = command ring
  if (!wait_event(kTrbCommandCompletion, trb, completion, kRingTimeoutMs)) return false;
  return completion_code(completion->status) == kCompletionSuccess;
}

// ---------------------------------------------------------------- ports

bool XhciController::port_info(uint32_t port, UsbPortInfo* out) const {
  if (port >= ports_ || !out) return false;
  const uint32_t status = read32(port_register(port));
  out->connected = status & kPortConnected;
  out->speed = to_usb_speed(port_speed_id(status));
  return true;
}

bool XhciController::device_descriptor(uint32_t port, UsbDeviceDescriptor* out) const {
  if (port >= ports_ || !out || !devices_[port].present) return false;
  *out = devices_[port].descriptor;
  return true;
}

bool XhciController::reset_port(uint32_t port) {
  const uint64_t reg = port_register(port);
  const uint32_t status = read32(reg);
  // A SuperSpeed port trains its link on its own and is already enabled.
  if (port_speed_id(status) >= 4 && (status & kPortEnabled)) return true;

  write32(reg, (status & kPortPreserveMask) | kPortReset);
  if (!wait_for(reg, kPortResetChange, kPortResetChange, 500)) return false;
  write32(reg, (read32(reg) & kPortPreserveMask) | kPortResetChange);  // acknowledge
  delay_us(10000);  // reset recovery time
  return read32(reg) & kPortEnabled;
}

// ---------------------------------------------------------------- devices

void XhciController::release_device(Device& d) {
  if (d.slot) {
    Trb completion;
    run_command(0, (kTrbDisableSlot << 10) | (uint32_t{d.slot} << 24), &completion);
    dcbaa_[d.slot] = 0;
  }
  for (InterruptEndpoint& e : d.endpoints) {
    e.ring.release(allocator_);
    release_view(allocator_, e.buffer);
    e = InterruptEndpoint{};
  }
  for (BulkEndpoint& e : d.bulk) {
    e.ring.release(allocator_);
    e = BulkEndpoint{};
  }
  release_view(allocator_, d.output_context);
  release_view(allocator_, d.input_context);
  d.control_ring.release(allocator_);
  d.present = false;
  d.slot = 0;
  d.configuration = 0;
  d.context_entries = 1;
  d.config_length = 0;
}

bool XhciController::enumerate_port(uint32_t port) {
  Device& d = devices_[port];
  if (!reset_port(port)) return false;
  d.speed_id = port_speed_id(read32(port_register(port)));
  if (!address_device(port, d)) {
    release_device(d);
    return false;
  }

  // The first 8 bytes of the device descriptor tell the real EP0 packet size.
  if (!transfer(d, 0x80, 6, 0x0100, 0, 8, true)) {
    release_device(d);
    return false;
  }
  const uint8_t reported = buffer_[7];
  const uint16_t max_packet = d.speed_id >= 4 ? (1u << reported) : reported;
  if (max_packet && max_packet != d.max_packet0 && !update_max_packet(d, max_packet)) {
    release_device(d);
    return false;
  }

  if (!transfer(d, 0x80, 6, 0x0100, 0, sizeof(UsbDeviceDescriptor), true)) {
    release_device(d);
    return false;
  }
  bytes_of(d.descriptor).copy_from(buffer_.first(sizeof(UsbDeviceDescriptor)));
  d.present = true;

  // Cache the first configuration (interfaces/endpoints) for class drivers. A
  // device without a readable one is still enumerated, just not usable by them.
  if (transfer(d, 0x80, 6, 0x0200, 0, 9, true)) {
    uint16_t total = buffer_[2] | (buffer_[3] << 8);
    if (total >= 9 && total <= sizeof(d.config) && transfer(d, 0x80, 6, 0x0200, 0, total, true)) {
      ByteView(d.config).copy_from(buffer_.first(total));
      d.config_length = total;
    }
  }
  return true;
}

bool XhciController::address_device(uint32_t port, Device& d) {
  Trb completion;
  if (!run_command(0, kTrbEnableSlot << 10, &completion)) return false;
  d.slot = completion.control >> 24;
  if (!d.slot || d.slot > max_slots_) {
    d.slot = 0;
    return false;
  }

  const uint32_t context_bytes = 33 * context_size_;  // input control + slot + 31 endpoints
  d.output_context = allocate_dma_view<uint8_t>(allocator_, context_bytes, 4096);
  d.input_context = allocate_dma_view<uint8_t>(allocator_, context_bytes, 4096);
  if (d.output_context.empty() || d.input_context.empty() || !d.control_ring.init(allocator_) ||
      !dma_reachable(d.output_context.data()) || !dma_reachable(d.input_context.data()))
    return false;
  d.output_context.fill(0);
  d.input_context.fill(0);
  dcbaa_[d.slot] = reinterpret_cast<uint64_t>(d.output_context.data());

  d.max_packet0 = default_max_packet(d.speed_id);
  const ArrayView<uint32_t> control = context_at(d.input_context, 0);
  control[1] = (1u << 0) | (1u << 1);  // add flags: slot context and EP0
  const ArrayView<uint32_t> slot = context_at(d.input_context, 1);
  slot[0] = (d.speed_id << 20) | (1u << 27);  // one context entry (EP0)
  slot[1] = (port + 1) << 16;                 // root hub port numbers start at 1
  const ArrayView<uint32_t> ep0 = context_at(d.input_context, 2);
  ep0[1] = (3u << 1) | (4u << 3) | (uint32_t{d.max_packet0} << 16);  // CErr 3, control endpoint
  const uint64_t ring = d.control_ring.address() | d.control_ring.cycle();  // DCS
  ep0[2] = static_cast<uint32_t>(ring);
  ep0[3] = static_cast<uint32_t>(ring >> 32);
  ep0[4] = 8;  // average TRB length

  return run_command(reinterpret_cast<uint64_t>(d.input_context.data()),
                     (kTrbAddressDevice << 10) | (uint32_t{d.slot} << 24), &completion);
}

bool XhciController::update_max_packet(Device& d, uint16_t max_packet) {
  const ArrayView<uint32_t> control = context_at(d.input_context, 0);
  control[0] = 0;
  control[1] = 1u << 1;  // evaluate EP0 only
  const ArrayView<uint32_t> ep0 = context_at(d.input_context, 2);
  ep0[1] = (ep0[1] & 0xFFFF) | (uint32_t{max_packet} << 16);
  Trb completion;
  if (!run_command(reinterpret_cast<uint64_t>(d.input_context.data()),
                   (kTrbEvaluateContext << 10) | (uint32_t{d.slot} << 24), &completion))
    return false;
  d.max_packet0 = max_packet;
  return true;
}

// Control transfer on endpoint 0. With data_in the data lands in buffer_;
// otherwise there is no data stage. (`length` is the wLength field.)
bool XhciController::transfer(Device& d, uint8_t request_type, uint8_t request, uint16_t value,
                              uint16_t index, uint16_t length, bool data_in) {
  if (data_in) buffer_.first(length).fill(0);
  const uint64_t setup = uint64_t{request_type} | (uint64_t{request} << 8) | (uint64_t{value} << 16) |
                         (uint64_t{index} << 32) | (uint64_t{length} << 48);
  constexpr uint32_t kTransferTypeIn = 3u << 16;  // setup TRB TRT field; 0 = no data stage

  d.control_ring.push(setup, 8,
                      (kTrbSetupStage << 10) | kTrbImmediateData | (data_in ? kTransferTypeIn : 0));
  if (data_in)
    d.control_ring.push(reinterpret_cast<uint64_t>(buffer_.data()), length,
                        (kTrbDataStage << 10) | kTrbDirectionIn);
  // The status stage runs opposite to the data: host-to-device after an IN data
  // stage, device-to-host when there was no data stage.
  const uint64_t status_trb = d.control_ring.push(
      0, 0, (kTrbStatusStage << 10) | kTrbIoc | (data_in ? 0 : kTrbDirectionIn));
  ring_doorbell(d.slot, 1);  // endpoint context index 1 = EP0

  Trb event;
  if (!wait_event(kTrbTransferEvent, status_trb, &event, kRingTimeoutMs)) return false;
  const uint32_t code = completion_code(event.status);
  return code == kCompletionSuccess || code == kCompletionShortPacket;
}

// ---------------------------------------------------------- class driver API

bool XhciController::configuration_descriptor(uint32_t port, ByteView buffer, uint16_t* length) const {
  if (port >= ports_ || !devices_[port].present || !devices_[port].config_length || buffer.empty()) return false;
  const Device& d = devices_[port];
  buffer.copy_from(ConstByteView(d.config).first(d.config_length).at_most(buffer.size()));  // as much as fits
  if (length) *length = d.config_length;
  return true;
}

bool XhciController::set_configuration(uint32_t port, uint8_t configuration) {
  kernel::ScopedMutex lock_guard(lock_);
  if (port >= ports_ || !devices_[port].present) return false;
  Device& d = devices_[port];
  if (d.configuration == configuration) return true;
  if (!transfer(d, 0x00, 9, configuration, 0, 0, false)) return false;
  d.configuration = configuration;
  return true;
}

bool XhciController::control_out(uint32_t port, uint8_t request_type, uint8_t request,
                                 uint16_t value, uint16_t index) {
  kernel::ScopedMutex lock_guard(lock_);
  if (port >= ports_ || !devices_[port].present) return false;
  return transfer(devices_[port], request_type, request, value, index, 0, false);
}

bool XhciController::control_in(uint32_t port, uint8_t request_type, uint8_t request, uint16_t value,
                                uint16_t index, ByteView buffer) {
  kernel::ScopedMutex lock_guard(lock_);
  if (port >= ports_ || !devices_[port].present || buffer.empty() || buffer.size() > 4096) return false;
  if (!transfer(devices_[port], request_type, request, value, index, static_cast<uint16_t>(buffer.size()), true))
    return false;
  buffer.copy_from(buffer_.first(buffer.size()));
  return true;
}

void XhciController::arm_endpoint(Device& d, InterruptEndpoint& e) {
  e.ready = false;
  e.pending_trb = e.ring.push(reinterpret_cast<uint64_t>(e.buffer.data()), e.max_packet,
                              (kTrbNormal << 10) | kTrbIoc | kTrbInterruptOnShortPacket);
  ring_doorbell(d.slot, e.context_id);
}

bool XhciController::start_interrupt_in(uint32_t port, uint8_t endpoint_address, uint16_t max_packet,
                                        uint8_t interval) {
  kernel::ScopedMutex lock_guard(lock_);
  if (port >= ports_ || !devices_[port].present || !(endpoint_address & 0x80)) return false;
  Device& d = devices_[port];
  InterruptEndpoint* e = nullptr;
  for (InterruptEndpoint& candidate : d.endpoints) {
    if (candidate.active && candidate.address == endpoint_address) return true;  // already running
    if (!candidate.active && !e) e = &candidate;
  }
  if (!e) return false;

  const uint8_t dci = (endpoint_address & 0x0F) * 2 + 1;  // IN endpoints are odd
  e->address = endpoint_address;
  e->context_id = dci;
  e->max_packet = max_packet;
  e->buffer = allocate_dma_view<uint8_t>(allocator_, max_packet < 64 ? 64 : max_packet, 64);
  if (e->buffer.empty() || !e->ring.init(allocator_) || !dma_reachable(e->buffer.data())) {
    e->ring.release(allocator_);
    release_view(allocator_, e->buffer);
    return false;
  }

  // Configure Endpoint: the input slot context starts as a copy of the live one.
  const uint32_t context_bytes = 33 * context_size_;
  d.input_context.fill(0);
  if (dci > d.context_entries) d.context_entries = dci;
  const ArrayView<uint32_t> control = context_at(d.input_context, 0);
  control[1] = (1u << 0) | (1u << dci);  // add: slot context and this endpoint
  const ArrayView<uint32_t> slot = context_at(d.input_context, 1);
  slot.copy_from(context_at(d.output_context, 0).first(8));
  slot[0] = (slot[0] & ~(0x1Fu << 27)) | (uint32_t{d.context_entries} << 27);

  // xHCI interval is 2^n * 125 us. Full/low speed bInterval counts 1 ms frames;
  // high/super speed bInterval is already an exponent plus one.
  uint32_t n;
  if (d.speed_id <= 2) {
    n = 3;
    while (n < 10 && (1u << (n - 3)) < interval) ++n;
  } else {
    n = interval ? interval - 1 : 0;
    if (n > 15) n = 15;
  }
  const ArrayView<uint32_t> ep = context_at(d.input_context, dci + 1);
  ep[0] = n << 16;
  ep[1] = (3u << 1) | (7u << 3) | (uint32_t{max_packet} << 16);  // CErr 3, interrupt IN
  const uint64_t ring = e->ring.address() | e->ring.cycle();
  ep[2] = static_cast<uint32_t>(ring);
  ep[3] = static_cast<uint32_t>(ring >> 32);
  ep[4] = (uint32_t{max_packet} << 16) | max_packet;  // max ESIT payload, average TRB length

  Trb completion;
  if (!run_command(reinterpret_cast<uint64_t>(d.input_context.data()),
                   (kTrbConfigureEndpoint << 10) | (uint32_t{d.slot} << 24), &completion)) {
    e->ring.release(allocator_);
    release_view(allocator_, e->buffer);
    return false;
  }
  e->active = true;
  arm_endpoint(d, *e);
  return true;
}

void XhciController::handle_event(const Trb& event) {
  if (trb_type(event.control) != kTrbTransferEvent) return;  // port changes etc.: ports are polled
  const uint32_t slot = event.control >> 24;
  const uint32_t endpoint_id = (event.control >> 16) & 0x1F;
  for (Device& d : devices_) {
    if (!d.slot || d.slot != slot) continue;
    for (InterruptEndpoint& e : d.endpoints) {
      if (!e.active || e.context_id != endpoint_id || e.pending_trb != event.parameter) continue;
      const uint32_t code = completion_code(event.status);
      if (code == kCompletionSuccess || code == kCompletionShortPacket) {
        const uint32_t residual = event.status & 0xFFFFFF;
        e.received = residual < e.max_packet ? e.max_packet - residual : 0;
        e.ready = true;
      }
      return;
    }
  }
}

void XhciController::pump_events() {
  const uint64_t interrupter = runtime_ + kRtInterrupter0;
  Trb event;
  bool any = false;
  while (event_ring_.pop(&event)) {
    handle_event(event);
    any = true;
  }
  if (any) write64(interrupter + kIrDequeue, event_ring_.dequeue_address() | kIrDequeueBusy);
}

bool XhciController::poll_interrupt_in(uint32_t port, uint8_t endpoint_address, ByteView buffer, uint16_t* length) {
  kernel::ScopedMutex lock_guard(lock_);
  if (port >= ports_ || !devices_[port].present) return false;
  Device& d = devices_[port];
  pump_events();
  for (InterruptEndpoint& e : d.endpoints) {
    if (!e.active || e.address != endpoint_address) continue;
    if (!e.ready) return false;
    buffer.copy_from(ConstByteView(e.buffer).first(e.received).at_most(buffer.size()));  // as much as fits
    if (length) *length = e.received;
    arm_endpoint(d, e);
    return true;
  }
  return false;
}

// ------------------------------------------------------------ bulk endpoints

bool XhciController::start_bulk_endpoint(uint32_t port, uint8_t endpoint_address, uint16_t max_packet) {
  kernel::ScopedMutex lock_guard(lock_);
  if (port >= ports_ || !devices_[port].present || !max_packet) return false;
  Device& d = devices_[port];
  BulkEndpoint* e = nullptr;
  for (BulkEndpoint& candidate : d.bulk) {
    if (candidate.active && candidate.address == endpoint_address) return true;  // already running
    if (!candidate.active && !e) e = &candidate;
  }
  if (!e || !e->ring.init(allocator_)) return false;

  const bool in = endpoint_address & 0x80;
  const uint8_t dci = (endpoint_address & 0x0F) * 2 + (in ? 1 : 0);
  e->address = endpoint_address;
  e->context_id = dci;

  const uint32_t context_bytes = 33 * context_size_;
  d.input_context.fill(0);
  if (dci > d.context_entries) d.context_entries = dci;
  const ArrayView<uint32_t> control = context_at(d.input_context, 0);
  control[1] = (1u << 0) | (1u << dci);  // add: slot context and this endpoint
  const ArrayView<uint32_t> slot = context_at(d.input_context, 1);
  slot.copy_from(context_at(d.output_context, 0).first(8));
  slot[0] = (slot[0] & ~(0x1Fu << 27)) | (uint32_t{d.context_entries} << 27);

  constexpr uint32_t kEndpointTypeBulkOut = 2, kEndpointTypeBulkIn = 6;
  const ArrayView<uint32_t> ep = context_at(d.input_context, dci + 1);
  ep[0] = 0;
  ep[1] = (3u << 1) | ((in ? kEndpointTypeBulkIn : kEndpointTypeBulkOut) << 3) | (uint32_t{max_packet} << 16);
  const uint64_t ring = e->ring.address() | e->ring.cycle();
  ep[2] = static_cast<uint32_t>(ring);
  ep[3] = static_cast<uint32_t>(ring >> 32);
  ep[4] = 4096;  // average TRB length

  Trb completion;
  if (!run_command(reinterpret_cast<uint64_t>(d.input_context.data()),
                   (kTrbConfigureEndpoint << 10) | (uint32_t{d.slot} << 24), &completion)) {
    e->ring.release(allocator_);
    return false;
  }
  e->active = true;
  return true;
}

bool XhciController::bulk_transfer(uint32_t port, uint8_t endpoint_address, ByteView data, uint32_t* transferred) {
  kernel::ScopedMutex lock_guard(lock_);
  if (transferred) *transferred = 0;
  const uint32_t length = static_cast<uint32_t>(data.size());
  if (port >= ports_ || !devices_[port].present || data.size() > 0x10000) return false;
  const uint64_t address = reinterpret_cast<uint64_t>(data.data());
  if (length && ((address & ~0xFFFFull) != ((address + length - 1) & ~0xFFFFull) || !dma_reachable(data.data())))
    return false;
  Device& d = devices_[port];
  for (BulkEndpoint& e : d.bulk) {
    if (!e.active || e.address != endpoint_address) continue;
    const uint64_t trb = e.ring.push(address, length & 0x1FFFF,
                                     (kTrbNormal << 10) | kTrbIoc | kTrbInterruptOnShortPacket);
    ring_doorbell(d.slot, e.context_id);
    Trb event;
    if (!wait_event(kTrbTransferEvent, trb, &event, 5000)) return false;
    const uint32_t code = completion_code(event.status);
    const uint32_t residual = event.status & 0xFFFFFF;
    if (transferred) *transferred = residual <= length ? length - residual : 0;
    return code == kCompletionSuccess || code == kCompletionShortPacket;
  }
  return false;
}

bool XhciController::reset_bulk_endpoint(uint32_t port, uint8_t endpoint_address) {
  kernel::ScopedMutex lock_guard(lock_);
  if (port >= ports_ || !devices_[port].present) return false;
  Device& d = devices_[port];
  for (BulkEndpoint& e : d.bulk) {
    if (!e.active || e.address != endpoint_address) continue;
    Trb completion;
    const uint32_t target = (uint32_t{e.context_id} << 16) | (uint32_t{d.slot} << 24);
    if (!run_command(0, (kTrbResetEndpoint << 10) | target, &completion)) return false;
    // The ring is abandoned where it stands: dequeue continues at the next free TRB.
    const uint64_t next_trb = e.ring.next_address();  // may wrap, so ask for the cycle state afterwards
    const uint64_t next = next_trb | e.ring.cycle();
    return run_command(next, (kTrbSetTrDequeuePointer << 10) | target, &completion);
  }
  return false;
}

}  // namespace hardware
