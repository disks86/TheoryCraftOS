#include "hardware/Bus/Pcie/EcamPcieBus.hpp"

namespace hardware {

namespace {
struct McfgEntry {
  uint64_t base_address;
  uint16_t segment;
  uint8_t start_bus, end_bus;
  uint32_t reserved;
} __attribute__((packed));
constexpr uint32_t kMcfgHeaderSize = sizeof(AcpiTableHeader) + 8;  // 8 reserved bytes
}  // namespace

bool EcamPcieBus::initialize() {
  const AcpiTableHeader* mcfg = acpi_.find("MCFG");
  if (!mcfg || mcfg->length < kMcfgHeaderSize + sizeof(McfgEntry)) return false;

  const auto* entries = reinterpret_cast<const McfgEntry*>(
      reinterpret_cast<const uint8_t*>(mcfg) + kMcfgHeaderSize);
  const uint32_t count = (mcfg->length - kMcfgHeaderSize) / sizeof(McfgEntry);
  for (uint32_t i = 0; i < count; ++i) {
    if (entries[i].segment != 0) continue;
    base_ = entries[i].base_address;
    first_bus_ = entries[i].start_bus;
    last_bus_ = entries[i].end_bus;
    scan(first_bus_, last_bus_);
    return true;
  }
  return false;
}

volatile uint32_t* EcamPcieBus::locate(PciAddress a, uint16_t offset) const {
  if (!base_ || a.bus < first_bus_ || a.bus > last_bus_ || offset >= 4096) return nullptr;
  uint64_t address = base_ + (uint64_t{static_cast<uint32_t>(a.bus - first_bus_)} << 20) +
                     (uint64_t{a.device} << 15) + (uint64_t{a.function} << 12) + (offset & 0xFFC);
  return reinterpret_cast<volatile uint32_t*>(address);
}

uint32_t EcamPcieBus::read32(PciAddress address, uint16_t offset) const {
  volatile uint32_t* p = locate(address, offset);
  return p ? *p : 0xFFFFFFFF;
}

void EcamPcieBus::write32(PciAddress address, uint16_t offset, uint32_t value) const {
  if (volatile uint32_t* p = locate(address, offset)) *p = value;
}

uint16_t EcamPcieBus::find_extended_capability(PciAddress address, uint16_t id) const {
  uint16_t offset = 0x100;
  for (int guard = 0; offset >= 0x100 && guard < 96; ++guard) {  // at most (4096-256)/4 entries
    uint32_t header = read32(address, offset);
    if (header == 0 || header == 0xFFFFFFFF) return 0;
    if ((header & 0xFFFF) == id) return offset;
    offset = (header >> 20) & 0xFFC;
  }
  return 0;
}

}  // namespace hardware
