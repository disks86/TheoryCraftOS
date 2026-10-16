#pragma once
#include "Allocators/IAllocator.hpp"
#include "hardware/Bus/Pci/IPciBus.hpp"
#include "hardware/Bus/Usb/Xhci/IXhciController.hpp"
#include "hardware/Bus/Usb/Xhci/XhciRing.hpp"
#include "uefi.hpp"

namespace hardware {

// Polling xHCI driver. initialize() takes the controller from the firmware,
// resets and starts it, then for every connected root port: resets the port,
// enables a device slot, addresses the device and reads its device descriptor.
// Devices stay addressed (slot and EP0 ring kept) for later class drivers.
// shutdown() resets the controller and hands it back to the firmware. Interrupts
// are not used; every wait polls the event ring.
class XhciController final : public IXhciController {
 public:
  static constexpr uint32_t kMaxPorts = 32;

  XhciController(const IPciBus& pci, const PciDeviceInfo& device, allocators::IAllocator& allocator,
                 efi::SystemTable* st)
      : pci_(pci), device_(device), allocator_(allocator), st_(st) {}

  const char* name() const override { return "xHCI controller"; }
  bool initialize() override;
  void shutdown() override;

  uint16_t interface_version() const override { return version_; }
  uint32_t max_device_slots() const override { return max_slots_; }
  bool uses_64_byte_contexts() const override { return context_size_ == 64; }

  uint32_t port_count() const override { return ports_; }
  bool port_info(uint32_t port, UsbPortInfo* out) const override;
  bool device_descriptor(uint32_t port, UsbDeviceDescriptor* out) const override;

 private:
  struct Device {
    bool present = false;
    uint8_t slot = 0;
    uint32_t speed_id = 0;
    uint16_t max_packet0 = 0;
    uint8_t* output_context = nullptr;
    uint8_t* input_context = nullptr;
    xhci::ProducerRing control_ring;
    UsbDeviceDescriptor descriptor{};
  };

  // Bring-up steps.
  bool take_ownership();
  bool reset();
  bool setup_memory();
  bool start();

  // Command ring / event ring.
  bool run_command(uint64_t parameter, uint32_t control, xhci::Trb* completion);
  bool wait_event(uint32_t type, uint64_t pointer, xhci::Trb* out, uint32_t timeout_ms);
  void ring_doorbell(uint32_t slot, uint32_t target);

  // Device enumeration.
  bool reset_port(uint32_t port);
  bool enumerate_port(uint32_t port);
  bool address_device(uint32_t port, Device& d);
  bool update_max_packet(Device& d, uint16_t max_packet);
  bool control_in(Device& d, uint8_t request_type, uint8_t request, uint16_t value, uint16_t index,
                  uint16_t length);
  void release_device(Device& d);

  bool dma_reachable(const void* p) const;
  uint64_t port_register(uint32_t port) const;
  bool wait_for(uint64_t address, uint32_t mask, uint32_t value, uint32_t timeout_ms);
  void delay_us(uint64_t us) const;
  uint8_t* context_at(uint8_t* base, uint32_t index) const { return base + index * context_size_; }

  const IPciBus& pci_;
  PciDeviceInfo device_;
  allocators::IAllocator& allocator_;
  efi::SystemTable* st_;

  uint64_t cap_ = 0, op_ = 0, runtime_ = 0, doorbells_ = 0;
  uint16_t version_ = 0;
  uint32_t max_slots_ = 0, ports_ = 0, context_size_ = 32;

  uint64_t* dcbaa_ = nullptr;
  uint64_t* scratchpad_table_ = nullptr;
  uint32_t scratchpad_count_ = 0;
  uint8_t* scratchpad_pages_[32] = {};
  xhci::ProducerRing command_ring_;
  xhci::EventRing event_ring_;
  uint8_t* buffer_ = nullptr;  // 4 KiB bounce buffer for control transfers
  bool running_ = false;
  bool ac64_ = false;
  bool owns_controller_ = false;  // firmware drivers are detached

  Device devices_[kMaxPorts];
};

}  // namespace hardware
