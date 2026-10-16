#include "hardware/Bus/Usb/UsbBus.hpp"

namespace hardware {

namespace {
// PCI programming interface values for class 0x0C, subclass 0x03.
UsbControllerKind kind_from_prog_if(uint8_t prog_if) {
  switch (prog_if) {
    case 0x00: return UsbControllerKind::Uhci;
    case 0x10: return UsbControllerKind::Ohci;
    case 0x20: return UsbControllerKind::Ehci;
    case 0x30: return UsbControllerKind::Xhci;
    default:   return UsbControllerKind::Unknown;
  }
}

// xHCI register layout: capability registers at the BAR, operational registers
// CAPLENGTH bytes later, port status/control registers 0x400 into those.
constexpr uint32_t kXhciPortRegisterOffset = 0x400;
constexpr uint32_t kXhciPortStride = 4;  // dwords per port
constexpr uint32_t kPortConnected = 1u << 0;
}  // namespace

bool UsbBus::initialize() {
  controller_count_ = 0;
  connected_ = 0;
  PciDeviceInfo d;
  for (uint32_t i = 0; pci_.pci_device(i, &d); ++i) {
    if (d.class_code != kPciClassSerialBus || d.subclass != kPciSubclassUsb) continue;
    const UsbControllerKind kind = kind_from_prog_if(d.prog_if);
    if (kind == UsbControllerKind::Unknown || controller_count_ == kMaxControllers) continue;

    Controller& c = controllers_[controller_count_++];
    c = {kind, 0, nullptr};
    if (kind != UsbControllerKind::Xhci) continue;

    const uint64_t bar = pci_.memory_bar(d.address, 0);
    if (!bar) continue;
    const auto* caps = reinterpret_cast<volatile uint8_t*>(bar);
    const uint32_t cap_length = caps[0];
    const uint32_t hcsparams1 = *reinterpret_cast<volatile uint32_t*>(bar + 4);
    c.ports = hcsparams1 >> 24;  // MaxPorts
    c.port_registers = reinterpret_cast<volatile uint32_t*>(bar + cap_length + kXhciPortRegisterOffset);

    for (uint32_t p = 0; p < c.ports; ++p)
      if (c.port_registers[p * kXhciPortStride] & kPortConnected) ++connected_;
  }
  return true;
}

UsbControllerKind UsbBus::controller_kind(uint32_t controller) const {
  return controller < controller_count_ ? controllers_[controller].kind : UsbControllerKind::Unknown;
}

uint32_t UsbBus::port_count(uint32_t controller) const {
  return controller < controller_count_ ? controllers_[controller].ports : 0;
}

bool UsbBus::port_info(uint32_t controller, uint32_t port, UsbPortInfo* out) const {
  if (controller >= controller_count_ || !out) return false;
  const Controller& c = controllers_[controller];
  if (port >= c.ports || !c.port_registers) return false;

  const uint32_t status = c.port_registers[port * kXhciPortStride];
  out->connected = status & kPortConnected;
  switch ((status >> 10) & 0xF) {  // Port Speed ID (default protocol speed table)
    case 1:  out->speed = UsbSpeed::Full; break;
    case 2:  out->speed = UsbSpeed::Low; break;
    case 3:  out->speed = UsbSpeed::High; break;
    case 4:  out->speed = UsbSpeed::Super; break;
    case 5:  out->speed = UsbSpeed::SuperPlus; break;
    default: out->speed = UsbSpeed::Unknown; break;
  }
  return true;
}

}  // namespace hardware
