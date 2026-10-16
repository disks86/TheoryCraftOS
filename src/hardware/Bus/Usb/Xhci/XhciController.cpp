#include "hardware/Bus/Usb/Xhci/XhciController.hpp"
#include "hardware/Bus/Pci/FirmwareDriverDetach.hpp"

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

void XhciController::delay_us(uint64_t us) const { st_->boot_services->stall(us); }

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

  dcbaa_ = static_cast<uint64_t*>(allocator_.allocate_aligned((max_slots_ + 1) * 8, 4096));
  buffer_ = static_cast<uint8_t*>(allocator_.allocate_aligned(4096, 4096));
  if (!dcbaa_ || !buffer_ || !dma_reachable(dcbaa_)) return false;
  __builtin_memset(dcbaa_, 0, (max_slots_ + 1) * 8);

  if (scratchpad_count_) {  // the controller keeps private state in these pages
    if (scratchpad_count_ > 32) return false;
    scratchpad_table_ = static_cast<uint64_t*>(allocator_.allocate_aligned(scratchpad_count_ * 8, 64));
    if (!scratchpad_table_) return false;
    for (uint32_t i = 0; i < scratchpad_count_; ++i) {
      scratchpad_pages_[i] = static_cast<uint8_t*>(allocator_.allocate_aligned(4096, 4096));
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
  allocator_.deallocate(buffer_);
  scratchpad_table_ = nullptr;
  dcbaa_ = nullptr;
  buffer_ = nullptr;

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
      // Anything else (e.g. port status changes) is dropped: ports are polled.
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
  allocator_.deallocate(d.output_context);
  allocator_.deallocate(d.input_context);
  d.control_ring.release(allocator_);
  d.output_context = d.input_context = nullptr;
  d.present = false;
  d.slot = 0;
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
  if (!control_in(d, 0x80, 6, 0x0100, 0, 8)) {
    release_device(d);
    return false;
  }
  const uint8_t reported = buffer_[7];
  const uint16_t max_packet = d.speed_id >= 4 ? (1u << reported) : reported;
  if (max_packet && max_packet != d.max_packet0 && !update_max_packet(d, max_packet)) {
    release_device(d);
    return false;
  }

  if (!control_in(d, 0x80, 6, 0x0100, 0, sizeof(UsbDeviceDescriptor))) {
    release_device(d);
    return false;
  }
  __builtin_memcpy(&d.descriptor, buffer_, sizeof(UsbDeviceDescriptor));
  d.present = true;
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
  d.output_context = static_cast<uint8_t*>(allocator_.allocate_aligned(context_bytes, 4096));
  d.input_context = static_cast<uint8_t*>(allocator_.allocate_aligned(context_bytes, 4096));
  if (!d.output_context || !d.input_context || !d.control_ring.init(allocator_) ||
      !dma_reachable(d.output_context) || !dma_reachable(d.input_context))
    return false;
  __builtin_memset(d.output_context, 0, context_bytes);
  __builtin_memset(d.input_context, 0, context_bytes);
  dcbaa_[d.slot] = reinterpret_cast<uint64_t>(d.output_context);

  d.max_packet0 = default_max_packet(d.speed_id);
  auto* control = reinterpret_cast<uint32_t*>(context_at(d.input_context, 0));
  control[1] = (1u << 0) | (1u << 1);  // add flags: slot context and EP0
  auto* slot = reinterpret_cast<uint32_t*>(context_at(d.input_context, 1));
  slot[0] = (d.speed_id << 20) | (1u << 27);  // one context entry (EP0)
  slot[1] = (port + 1) << 16;                 // root hub port numbers start at 1
  auto* ep0 = reinterpret_cast<uint32_t*>(context_at(d.input_context, 2));
  ep0[1] = (3u << 1) | (4u << 3) | (uint32_t{d.max_packet0} << 16);  // CErr 3, control endpoint
  const uint64_t ring = d.control_ring.address() | d.control_ring.cycle();  // DCS
  ep0[2] = static_cast<uint32_t>(ring);
  ep0[3] = static_cast<uint32_t>(ring >> 32);
  ep0[4] = 8;  // average TRB length

  return run_command(reinterpret_cast<uint64_t>(d.input_context),
                     (kTrbAddressDevice << 10) | (uint32_t{d.slot} << 24), &completion);
}

bool XhciController::update_max_packet(Device& d, uint16_t max_packet) {
  auto* control = reinterpret_cast<uint32_t*>(context_at(d.input_context, 0));
  control[0] = 0;
  control[1] = 1u << 1;  // evaluate EP0 only
  auto* ep0 = reinterpret_cast<uint32_t*>(context_at(d.input_context, 2));
  ep0[1] = (ep0[1] & 0xFFFF) | (uint32_t{max_packet} << 16);
  Trb completion;
  if (!run_command(reinterpret_cast<uint64_t>(d.input_context),
                   (kTrbEvaluateContext << 10) | (uint32_t{d.slot} << 24), &completion))
    return false;
  d.max_packet0 = max_packet;
  return true;
}

// Device-to-host control transfer on endpoint 0; the data lands in buffer_.
bool XhciController::control_in(Device& d, uint8_t request_type, uint8_t request, uint16_t value,
                                uint16_t index, uint16_t length) {
  __builtin_memset(buffer_, 0, length);
  const uint64_t setup = uint64_t{request_type} | (uint64_t{request} << 8) | (uint64_t{value} << 16) |
                         (uint64_t{index} << 32) | (uint64_t{length} << 48);
  constexpr uint32_t kTransferTypeIn = 3u << 16;  // setup TRB TRT field

  d.control_ring.push(setup, 8, (kTrbSetupStage << 10) | kTrbImmediateData | kTransferTypeIn);
  d.control_ring.push(reinterpret_cast<uint64_t>(buffer_), length, (kTrbDataStage << 10) | kTrbDirectionIn);
  const uint64_t status_trb =
      d.control_ring.push(0, 0, (kTrbStatusStage << 10) | kTrbIoc);  // status stage runs host-to-device
  ring_doorbell(d.slot, 1);  // endpoint context index 1 = EP0

  Trb event;
  if (!wait_event(kTrbTransferEvent, status_trb, &event, kRingTimeoutMs)) return false;
  const uint32_t code = completion_code(event.status);
  return code == kCompletionSuccess || code == kCompletionShortPacket;
}

}  // namespace hardware
