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

// The brcmfmac chip bring-up after Linux chip.c, pcie.c and sdio.c; see BroadcomWifi.hpp.
#include "Hardware/WirelessInterface/Broadcom/BroadcomWifi.hpp"
#include "Hardware/WirelessInterface/WifiChipFactory.hpp"
#include "Kernel/Delay.hpp"

namespace hardware {

using namespace brcmf_bp;

namespace {
constexpr uint32_t kPollMs = 50;
constexpr uint32_t kConsoleReadMax = 8192;
constexpr uint32_t kChip43602 = 0xaa52;
constexpr uint32_t kSdioEnableTimeoutUs = 100 * 1000;

BrcmfBusType bus_type_of(WifiBus bus) {
  switch (bus) {
    case WifiBus::Pci: return BrcmfBusType::Pcie;
    case WifiBus::Sdio: return BrcmfBusType::Sdio;
    case WifiBus::Usb: return BrcmfBusType::Usb;
  }
  return BrcmfBusType::Pcie;
}
}  // namespace

BrcmfChip::BrcmfChip(const WifiAdapterModel& model, const WifiChipBus& bus, allocators::IAllocator& allocator)
    : model_(model), bus_(bus), allocator_(allocator), bus_type_(bus_type_of(model.bus)), clm_(allocator), txcap_(allocator) {}

BrcmfChip::~BrcmfChip() { stop(); }

bool BrcmfChip::add_board_type(StringView board_type) {
  if (board_type_count_ >= kMaxBoardTypes || board_type.empty() || !board_types_[board_type_count_].assign(board_type)) return false;
  ++board_type_count_;
  return true;
}

void BrcmfChip::set_platform_mac(const MacAddress& mac) {
  has_platform_mac_ = true;
  for (int i = 0; i < 6; ++i) platform_mac_[i] = mac.bytes[i];
}

const char* BrcmfChip::firmware_name() const { return firmware_name_.empty() ? model_.firmware : firmware_name_.c_str(); }

WifiChipResult BrcmfChip::fail(WifiChipResult result, const char* message) {
  message_ = message;
  state_ = WifiChipState::Failed;
  return result;
}

bool BrcmfChip::wants_seed() const {
  if (seed_override_ != 0) return seed_override_ == 1;
  // Apple's firmware for these chips expects 256 host random bytes below the NVRAM (Linux "Download random seed"); which chips get one is from the Asahi work, unverified here
  return chip_id_.chip == 0x4364 || chip_id_.chip == 0x4377 || chip_id_.chip == 0x4378 || chip_id_.chip == 0x4387;
}

bool BrcmfChip::fill_random(ByteView out) {
  if (random_ && random_->fill(out)) return true;
  // no source: xorshift from the chip's identity (the firmware gets something, not entropy)
  uint64_t state = 0x9E3779B97F4A7C15ull ^ (uint64_t{chip_id_.chip} << 16) ^ chip_id_.revision;
  for (uint8_t& byte : out) {
    state ^= state << 13;
    state ^= state >> 7;
    state ^= state << 17;
    byte = static_cast<uint8_t>(state >> 24);
  }
  return true;
}

// The bus step before the ARM runs: PCIe brcmf_pcie_buscore_activate, SDIO brcmf_sdio_buscore_activate.
bool BrcmfChip::activate(uint32_t reset_vector) {
  if (bus_type_ == BrcmfBusType::Pcie) return backplane_->write32(0, reset_vector);
  const BrcmfCore* sdio_core = cores_.find(kCoreSdioDev);
  if (!sdio_core || !backplane_->write32(sdio_core->base + brcmf_sdio::kCoreIntStatus, 0xffffffff)) return false;
  if (reset_vector == 0) return true;
  const uint8_t bytes[4] = {static_cast<uint8_t>(reset_vector), static_cast<uint8_t>(reset_vector >> 8), static_cast<uint8_t>(reset_vector >> 16),
                            static_cast<uint8_t>(reset_vector >> 24)};
  return backplane_->write_block(0, ConstByteView(bytes));
}

WifiChipResult BrcmfChip::start(IWifiFirmwareSource& firmware) {
  stop();
  identity_ = WifiChipIdentity{};
  chip_id_ = BrcmfChipId{};
  cores_.count = 0;
  ram_ = BrcmfRamInfo{};
  firmware_name_.clear();
  nvram_name_.clear();
  clm_name_.clear();
  txcap_name_.clear();
  clm_.clear();
  txcap_.clear();
  nvram_bytes_ = 0;
  shared_address_ = 0;
  pcie_shared_ = BrcmfPcieShared{};
  sdio_shared_ = BrcmfSdioShared{};

  const WifiChipResult identified = identify();
  if (identified != WifiChipResult::Ok) return identified;
  state_ = WifiChipState::Identified;
  return download(firmware);
}

WifiChipResult BrcmfChip::identify() {
  backplane_ = nullptr;
  if (bus_type_ == BrcmfBusType::Pcie) {
    if (!bus_.registers || !bus_.pci) return fail(WifiChipResult::NoRegisters, "no BAR0 or configuration space for the backplane window");
    if (!pcie_) pcie_ = UniquePointer<BrcmfPcieBackplane>::make(allocator_, *bus_.registers, *bus_.pci, bus_.pci_device.address);
    if (!pcie_) return fail(WifiChipResult::NoRegisters, "out of memory");
    pcie_->forget_window();
    backplane_ = pcie_.get();
  } else if (bus_type_ == BrcmfBusType::Sdio) {
    if (!bus_.sdio) return fail(WifiChipResult::NoRegisters, "no SDIO function");
    if (!sdio_) sdio_ = UniquePointer<BrcmfSdioBackplane>::make(allocator_, *bus_.sdio);
    if (!sdio_) return fail(WifiChipResult::NoRegisters, "out of memory");
    sdio_->forget_window();
    backplane_ = sdio_.get();
    if (!brcmf_sdio_enable_function(*bus_.sdio, brcmf_sdio::kFunction1Bit, kSdioEnableTimeoutUs)) return fail(WifiChipResult::NoResponse, "SDIO function 1 not ready");
    if (!brcmf_sdio_request_alp(*bus_.sdio)) return fail(WifiChipResult::NoResponse, "no ALP clock (CHIPCLKCSR)");
  } else {
    return fail(WifiChipResult::Unsupported, "USB brcmfmac adapters are not supported");
  }

  uint32_t value = 0;
  if (!backplane_->read32(kEnumBase + kCcChipId, &value)) return fail(WifiChipResult::NoResponse, "ChipID read failed");
  if (value == 0 || value == 0xffffffff) return fail(WifiChipResult::NoResponse, "ChipID reads as all zeros or ones");
  chip_id_ = brcmf_decode_chip_id(value);
  identity_.chip_id = chip_id_.chip;
  identity_.revision = chip_id_.revision;
  brcmf_chip_name(chip_id_.chip, chip_id_.revision, chip_name_);
  if (chip_id_.socitype != kSociAi) return fail(WifiChipResult::UnknownRevision, "not an AXI backplane chip (the Sonics BCM4329 is not supported)");

  uint32_t erom_address = 0;
  if (!backplane_->read32(kEnumBase + kCcEromPtr, &erom_address)) return fail(WifiChipResult::NoResponse, "eromptr read failed");
  if ((erom_address & 3) != 0 || erom_address > 0xffffffffu - kEromBytes) return fail(WifiChipResult::NoResponse, "eromptr is not a usable address");
  uint32_t count = 0;
  for (uint32_t& word : erom_) {
    if (!backplane_->read32(erom_address + 4 * count, &word)) return fail(WifiChipResult::NoResponse, "enumeration ROM read failed");
    ++count;
  }
  const BrcmfEromResult scanned = brcmf_erom_scan(ArrayView<const uint32_t>(erom_), cores_);
  if (scanned != BrcmfEromResult::Ok) return fail(WifiChipResult::NoResponse, brcmf_erom_result_name(scanned));
  const uint16_t bus_core = bus_type_ == BrcmfBusType::Pcie ? kCorePcie2 : kCoreSdioDev;
  if (!brcmf_cores_usable(cores_, bus_core)) return fail(WifiChipResult::UnknownRevision, "the chip lacks chipcommon, an ARM core, its RAM or the bus core");

  // brcmf_chip_recognition's end: passive, the bus reset, passive again, then the RAM
  if (!brcmf_set_passive(*backplane_, chip_id_, cores_)) return fail(WifiChipResult::NoResponse, "the ARM could not be halted");
  if (bus_type_ == BrcmfBusType::Pcie) {
    if (!brcmf_pcie_reset_device(*pcie_, cores_)) return fail(WifiChipResult::NoResponse, "the watchdog reset failed");
    if (!brcmf_set_passive(*backplane_, chip_id_, cores_)) return fail(WifiChipResult::NoResponse, "the ARM could not be halted after the reset");
  }
  switch (brcmf_read_ram_info(*backplane_, chip_id_, cores_, &ram_)) {
    case BrcmfRamResult::Ok: break;
    case BrcmfRamResult::BusError: return fail(WifiChipResult::NoResponse, "RAM size registers unreadable");
    case BrcmfRamResult::NoMemoryCore: return fail(WifiChipResult::UnknownRevision, "no RAM core");
    case BrcmfRamResult::UnknownBase: return fail(WifiChipResult::UnknownRevision, "the chip's RAM base address is not known");
    case BrcmfRamResult::ZeroSize: return fail(WifiChipResult::NoResponse, "the chip reports no RAM");
  }
  return WifiChipResult::Ok;
}

bool BrcmfChip::load(IWifiFirmwareSource& firmware, const char* extension, ByteBuffer& out, BrcmfFileName& used) {
  used.clear();
  const BrcmfFirmwareMapping* mapping = brcmf_find_firmware(bus_type_, chip_id_.chip, chip_id_.revision);
  BrcmfFileName plain;
  if (!mapping || !brcmf_file_name(StringView(mapping->base), StringView(extension), plain)) return false;
  for (uint32_t i = 0; i < board_type_count_; ++i) {
    BrcmfFileName board;
    if (brcmf_board_file_name(plain, board_types_[i], board) && firmware.read(board.c_str(), out)) {
      used = board;
      return true;
    }
  }
  if (!firmware.read(plain.c_str(), out)) return false;
  used = plain;
  return true;
}

WifiChipResult BrcmfChip::download(IWifiFirmwareSource& firmware) {
  const BrcmfFirmwareMapping* mapping = brcmf_find_firmware(bus_type_, chip_id_.chip, chip_id_.revision);
  if (!mapping) return fail(WifiChipResult::UnknownRevision, "no firmware is known for this chip and revision");
  BrcmfFileName plain;
  if (!brcmf_file_name(StringView(mapping->base), StringView(kBrcmfExtensionCode), plain)) return fail(WifiChipResult::UnknownRevision, "firmware name too long");
  firmware_name_ = plain;

  ByteBuffer image(allocator_);
  if (!load(firmware, kBrcmfExtensionCode, image, firmware_name_)) {
    firmware_name_ = plain;
    return fail(WifiChipResult::FirmwareMissing, "firmware image not found");
  }
  if (image.size() < 4 || image.size() > ram_.size) return fail(WifiChipResult::FirmwareInvalid, "firmware image is empty or larger than the chip's RAM");
  (void)brcmf_image_version(image.view(), ArrayView<char>(identity_.firmware_version));

  // NVRAM: optional on PCIe (BRCMF_FW_REQF_OPTIONAL), required on SDIO
  ByteBuffer text(allocator_);
  ByteBuffer nvram(allocator_);
  if (load(firmware, kBrcmfExtensionNvram, text, nvram_name_)) {
    BrcmfNvramOptions options;
    if (bus_type_ == BrcmfBusType::Pcie) {
      options.domain_nr = 1;    // pci_domain_nr() + 1; the machines here have one PCI segment
      options.bus_nr = bus_.pci_device.address.bus;
    }
    options.has_platform_mac = has_platform_mac_;
    for (int i = 0; i < 6; ++i) options.platform_mac[i] = platform_mac_[i];
    const BrcmfNvramResult stripped = brcmf_nvram_strip(text.view(), options, nvram);
    if (stripped == BrcmfNvramResult::NoMemory) return fail(WifiChipResult::FirmwareInvalid, "out of memory for the NVRAM");
    if (stripped == BrcmfNvramResult::Empty) nvram.clear();
  }
  if (nvram.size() == 0 && bus_type_ == BrcmfBusType::Sdio) return fail(WifiChipResult::FirmwareMissing, "NVRAM file not found (or empty)");
  nvram_bytes_ = nvram.size();
  // the blobs are for after the start (the "clmload" / "txcapload" iovars)
  if (!load(firmware, kBrcmfExtensionClm, clm_, clm_name_)) clm_.clear();
  if (bus_type_ == BrcmfBusType::Pcie && !load(firmware, kBrcmfExtensionTxcap, txcap_, txcap_name_)) txcap_.clear();

  const bool seed = bus_type_ == BrcmfBusType::Pcie && wants_seed();
  BrcmfPcieLayout layout;
  if (!brcmf_pcie_layout(ram_, static_cast<uint32_t>(image.size()), static_cast<uint32_t>(nvram.size()), seed, &layout))
    return fail(WifiChipResult::FirmwareInvalid, "firmware and NVRAM do not fit into the chip's RAM");

  if (bus_type_ == BrcmfBusType::Sdio && !brcmf_sdio_request_ht(*bus_.sdio)) return fail(WifiChipResult::NoResponse, "no HT clock");
  // brcmf_pcie_enter_download_state: 43602 needs RAM banks 5 and 7 unprotected
  if (bus_type_ == BrcmfBusType::Pcie && chip_id_.chip == kChip43602) {
    const BrcmfCore* cr4 = cores_.find(kCoreArmCr4);
    if (!cr4 || !backplane_->write32(cr4->base + kCr4BankIdx, 5) || !backplane_->write32(cr4->base + kCr4BankPda, 0) ||
        !backplane_->write32(cr4->base + kCr4BankIdx, 7) || !backplane_->write32(cr4->base + kCr4BankPda, 0))
      return fail(WifiChipResult::NoResponse, "43602 RAM banks could not be opened");
  }
  const ConstByteView code = image.view();
  const uint32_t reset_vector = uint32_t{code[0]} | uint32_t{code[1]} << 8 | uint32_t{code[2]} << 16 | uint32_t{code[3]} << 24;
  if (!backplane_->write_block(layout.firmware, code)) return fail(WifiChipResult::NoResponse, "firmware download failed");
  if (bus_type_ == BrcmfBusType::Pcie && !backplane_->write32(layout.shared_word, 0)) return fail(WifiChipResult::NoResponse, "RAM write failed");
  if (nvram.size()) {
    if (!backplane_->write_block(layout.nvram, nvram.view())) return fail(WifiChipResult::NoResponse, "NVRAM download failed");
    if (seed) {
      uint8_t footer[8];
      brcmf_pcie_seed_footer(footer);
      uint8_t random[brcmf_pcie::kRandomSeedLength];
      if (!fill_random(ByteView(random)) || !backplane_->write_block(layout.seed_footer, ConstByteView(footer)) ||
          !backplane_->write_block(layout.seed, ConstByteView(random)))
        return fail(WifiChipResult::NoResponse, "random seed download failed");
    }
  }
  uint32_t baseline = 0;
  if (!backplane_->read32(layout.shared_word, &baseline)) return fail(WifiChipResult::NoResponse, "RAM read failed");

  // brcmf_pcie_exit_download_state: 43602's SOCRAM core is reset before the ARM runs
  if (bus_type_ == BrcmfBusType::Pcie && chip_id_.chip == kChip43602) {
    const BrcmfCore* socram = cores_.find(kCoreInternalMem);
    if (socram && !brcmf_core_reset(*backplane_, *socram, 0, 0, 0)) return fail(WifiChipResult::NoResponse, "SOCRAM reset failed");
  }
  if (!brcmf_set_active(*backplane_, cores_, *this, reset_vector)) return fail(WifiChipResult::FirmwareRejected, "the ARM could not be started");
  return wait_for_firmware(baseline);
}

WifiChipResult BrcmfChip::wait_for_firmware(uint32_t baseline) {
  const uint32_t shared_word = ram_.base + ram_.size - 4;
  uint32_t value = baseline;
  for (uint32_t waited = 0; waited < brcmf_pcie::kFirmwareUpTimeoutMs; waited += kPollMs) {
    delay_us(kPollMs * 1000);
    if (!backplane_->read32(shared_word, &value)) return fail(WifiChipResult::NoResponse, "RAM read failed");
    if (value != baseline && (bus_type_ == BrcmfBusType::Pcie || brcmf_sdio_shared_pointer_valid(value))) break;
  }
  if (value == baseline) return fail(WifiChipResult::FirmwareRejected, "the firmware did not start (no shared structure)");
  if (value < ram_.base || value - ram_.base >= ram_.size) return fail(WifiChipResult::FirmwareRejected, "the firmware's shared structure address is outside RAM");
  shared_address_ = value;

  uint32_t console_addr = 0;
  if (bus_type_ == BrcmfBusType::Pcie) {
    uint8_t bytes[brcmf_pcie::kSharedBytes] = {};
    if (value - ram_.base > ram_.size - sizeof bytes) return fail(WifiChipResult::FirmwareRejected, "the shared structure runs past the end of RAM");
    if (!backplane_->read_block(value, ByteView(bytes))) return fail(WifiChipResult::NoResponse, "shared structure read failed");
    switch (brcmf_pcie_parse_shared(ConstByteView(bytes), value, ram_, &pcie_shared_)) {
      case BrcmfSharedResult::Ok: break;
      case BrcmfSharedResult::UnsupportedVersion: return fail(WifiChipResult::FirmwareRejected, "unsupported PCIe shared protocol version");
      case BrcmfSharedResult::Truncated:
      case BrcmfSharedResult::BadAddress: return fail(WifiChipResult::FirmwareRejected, "malformed shared structure");
    }
    console_addr = pcie_shared_.console_addr;
  } else {
    uint8_t bytes[brcmf_sdio::kSharedBytes] = {};
    if (value - ram_.base > ram_.size - sizeof bytes) return fail(WifiChipResult::FirmwareRejected, "the shared structure runs past the end of RAM");
    if (!backplane_->read_block(value, ByteView(bytes))) return fail(WifiChipResult::NoResponse, "shared structure read failed");
    if (!brcmf_sdio_parse_shared(ConstByteView(bytes), value, &sdio_shared_)) return fail(WifiChipResult::FirmwareRejected, "unsupported sdpcm_shared version");
    console_addr = sdio_shared_.console_addr;
  }
  read_console(console_addr);
  state_ = WifiChipState::FirmwareLoaded;
  message_ = "firmware running; FullMAC commands (msgbuf / BCDC) are not written yet";
  return WifiChipResult::Unsupported;
}

void BrcmfChip::read_console(uint32_t console_addr) {
  if (console_addr < ram_.base || console_addr - ram_.base > ram_.size - brcmf_pcie::kConsoleBytes) return;
  uint8_t bytes[brcmf_pcie::kConsoleBytes] = {};
  BrcmfConsole console;
  if (!backplane_->read_block(console_addr, ByteView(bytes)) || !brcmf_parse_console(ConstByteView(bytes), ram_, &console)) return;
  ByteBuffer log(allocator_);
  const uint32_t length = console.size < kConsoleReadMax ? console.size : kConsoleReadMax;
  if (!log.put_zeros(length) || !backplane_->read_block(console.buffer, log.writable_view())) return;
  char version[sizeof identity_.firmware_version] = {};
  if (brcmf_console_version(log.view(), ArrayView<char>(version))) (void)ArrayView<char>(identity_.firmware_version).copy_from(ArrayView<const char>(version));
}

void BrcmfChip::stop() {
  // the ARM halted again and the cores held (DMA stops with the D11 core); then the chip is left as Linux leaves it on remove
  if (backplane_ && (state_ == WifiChipState::Identified || state_ == WifiChipState::FirmwareLoaded || state_ == WifiChipState::Ready))
    (void)brcmf_set_passive(*backplane_, chip_id_, cores_);
  state_ = WifiChipState::Off;
  message_ = "stopped";
}

UniquePointer<IWifiChip> make_broadcom_wifi_chip(const WifiAdapterModel& model, const WifiChipBus& bus, allocators::IAllocator& allocator) {
  if (model.vendor != WifiVendor::Broadcom || model.family != WifiDriverFamily::Brcmfmac) return UniquePointer<IWifiChip>();
  if (model.bus != WifiBus::Pci && model.bus != WifiBus::Sdio) return UniquePointer<IWifiChip>();
  return UniquePointer<IWifiChip>::make_as<BrcmfChip>(allocator, model, bus, allocator);
}

}  // namespace hardware
