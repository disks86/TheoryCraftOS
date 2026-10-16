#pragma once
#include "Allocators/IAllocator.hpp"
#include "hardware/Bus/Pci/IPciBus.hpp"
#include "hardware/NetworkInterface/INetworkInterface.hpp"
#include "ObjectContainer/List.hpp"
#include "Utility/UniquePointer.hpp"
#include "uefi.hpp"

namespace hardware {

// Finds every network interface the machine has and brings them up. Each PCI
// network controller gets a native driver where this OS has one (virtio-net,
// Intel e1000, Realtek RTL8139); any other NIC the firmware can drive is used
// through the firmware's driver instead.
class SystemNetworkInterfaces {
 public:
  SystemNetworkInterfaces(IPciBus& pci, allocators::IAllocator& allocator, efi::SystemTable* st)
      : pci_(pci), allocator_(allocator), st_(st), interfaces_(allocator) {}

  void initialize();
  // Stops every interface and frees them.
  void shutdown();

  uint32_t count() const { return static_cast<uint32_t>(interfaces_.count()); }
  INetworkInterface* at(uint32_t index) const {
    return index < interfaces_.count() ? interfaces_[index].get() : nullptr;
  }

 private:
  void find_native();
  void find_firmware();

  IPciBus& pci_;
  allocators::IAllocator& allocator_;
  efi::SystemTable* st_;
  List<UniquePointer<INetworkInterface>> interfaces_;
};

}  // namespace hardware
