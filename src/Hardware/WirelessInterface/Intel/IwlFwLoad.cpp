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


#include "Hardware/WirelessInterface/Intel/IwlFwLoad.hpp"
#include "Kernel/Delay.hpp"
#include "Utility/AllocatedView.hpp"

namespace hardware {

using namespace iwl_csr;

namespace {
constexpr uint32_t kChunkTimeout = 5000000;                      // 5 s per chunk, as iwlwifi waits
constexpr uint32_t kMaxPartSections = 64;
}  // namespace

bool IwlFwLoad::load_chunk(uint32_t device_address, uint64_t bus_address, uint32_t bytes, const char** error) {
  if (!device_.grab_nic_access()) {
    *error = "the MAC would not wake for the firmware transfer";
    return false;
  }
  // acknowledge what is left of earlier transfers
  device_.write32(kFhIntStatus, kFhIntTxMask);
  device_.write32(kInt, kIntBitFhTx);
  const uint32_t ch = iwl_fh::kServiceChannel;
  device_.write32(iwl_fh::tx_config(ch), iwl_fh::kTxChannelPause);
  device_.write32(iwl_fh::service_sram_address(ch), device_address);
  device_.write32(iwl_fh::tfdib_ctrl0(ch), static_cast<uint32_t>(bus_address));
  device_.write32(iwl_fh::tfdib_ctrl1(ch), (static_cast<uint32_t>(bus_address >> 32) << iwl_fh::kTfdibAddressShift) | bytes);
  device_.write32(iwl_fh::tx_buffer_status(ch), (1u << iwl_fh::kBufferStatusTbNumPos) | (1u << iwl_fh::kBufferStatusTbIdxPos) | iwl_fh::kBufferStatusValid);
  device_.write32(iwl_fh::tx_config(ch), iwl_fh::kTxChannelEnable | iwl_fh::kTxCreditDisable | iwl_fh::kTxInterruptHostEndTfd);
  device_.release_nic_access();
  // The transfer is done when the flow handler reports a transmit channel interrupt.
  uint32_t waited = 0;
  while (!(device_.read32(kFhIntStatus) & kFhIntTxMask)) {
    if (waited >= kChunkTimeout) {
      *error = "a firmware chunk did not finish loading";
      return false;
    }
    delay_us(10);
    waited += 10;
  }
  device_.write32(kFhIntStatus, kFhIntTxMask);
  device_.write32(kInt, kIntBitFhTx);
  ++chunks_loaded_;
  return true;
}

bool IwlFwLoad::load_section(const IwlSection& section, const char** error) {
  const uint64_t length = section.data.size();
  if (length == 0 || length % 4 != 0 || length > 0xFFFFFFFFu) {
    *error = "a firmware section is not a whole number of dwords";
    return false;
  }
  const uint64_t chunk = length < iwl_fh::kMaxTransferLength ? length : iwl_fh::kMaxTransferLength;
  ArrayView<uint8_t> bounce = allocate_dma_view<uint8_t>(allocator_, chunk, 4096);
  if (bounce.size() == 0) {
    *error = "no memory for the firmware transfer buffer";
    return false;
  }
  // the chip addresses the buffer with 36 bits at most, and a transfer must not cross a 4 GiB line
  const uint64_t bus = reinterpret_cast<uint64_t>(bounce.data());
  bool ok = bus >> 36 == 0 && bus >> 32 == (bus + chunk - 1) >> 32;
  if (!ok) *error = "the firmware transfer buffer is out of the chip's reach";
  for (uint64_t offset = 0; ok && offset < length; offset += chunk) {
    const uint32_t bytes = static_cast<uint32_t>(length - offset < chunk ? length - offset : chunk);
    const uint32_t destination = section.device_offset + static_cast<uint32_t>(offset);
    const bool extended = destination >= kIwlFwMemExtendedStart && destination <= kIwlFwMemExtendedEnd;
    bounce.subview(0, bytes).copy_from(section.data.subview(offset, bytes));
    if (extended && device_.grab_nic_access()) {
      device_.set_prph_bits(iwl_prph::kLmpmChick, iwl_prph::kLmpmChickExtendedAddressSpace);
      device_.release_nic_access();
    }
    ok = load_chunk(destination, bus, bytes, error);
    if (extended && device_.grab_nic_access()) {
      device_.clear_prph_bits(iwl_prph::kLmpmChick, iwl_prph::kLmpmChickExtendedAddressSpace);
      device_.release_nic_access();
    }
  }
  release_view(allocator_, bounce);
  return ok;
}

bool IwlFwLoad::load_cpu(const IwlSection* sections, uint32_t count, uint32_t cpu, const char** error) {
  const uint32_t shift = cpu == 1 ? 0 : 16;
  uint32_t section_number = 1;
  for (uint32_t i = 0; i < count; ++i) {
    if (!load_section(sections[i], error)) return false;
    ++sections_loaded_;
    // tell the ROM which sections are in
    if (!device_.grab_nic_access()) {
      *error = "the MAC would not wake to announce a firmware section";
      return false;
    }
    device_.write32(iwl_fh::kUcodeLoadStatus, device_.read32(iwl_fh::kUcodeLoadStatus) | (section_number << shift));
    device_.release_nic_access();
    section_number = (section_number << 1) | 1;
  }
  // every section of this processor is in
  if (!device_.grab_nic_access()) {
    *error = "the MAC would not wake to finish the firmware load";
    return false;
  }
  device_.write32(iwl_fh::kUcodeLoadStatus, cpu == 1 ? 0xFFFFu : 0xFFFFFFFFu);
  device_.release_nic_access();
  return true;
}

bool IwlFwLoad::load(const IwlFirmware& firmware, IwlImage image, const char** error) {
  const char* local = nullptr;
  if (!error) error = &local;
  sections_loaded_ = chunks_loaded_ = 0;
  if (!firmware.has_image(image)) {
    *error = "the firmware has no such image";
    return false;
  }
  IwlSection lmac[kMaxPartSections], umac[kMaxPartSections];
  const uint32_t lmac_count = firmware.part(image, 0, lmac, kMaxPartSections);
  const uint32_t umac_count = firmware.part(image, 1, umac, kMaxPartSections);
  if (lmac_count == 0) {
    *error = "the firmware image has no sections for the first processor";
    return false;
  }
  if (!device_.grab_nic_access()) {
    *error = "the MAC would not wake to start the firmware load";
    return false;
  }
  // (the original value is only kept for debugging by iwlwifi) and the processors' reset is released before the sections go in
  device_.write_prph(iwl_prph::kWfpmGp2, 0x01010101);
  device_.write_prph(iwl_prph::kReleaseCpuReset, iwl_prph::kReleaseCpuResetBit);
  device_.release_nic_access();
  if (!load_cpu(lmac, lmac_count, 1, error)) return false;
  return load_cpu(umac, umac_count, 2, error);
}

bool IwlFwLoad::wait_alive(uint32_t timeout_us, const char** error) {
  const char* local = nullptr;
  if (!error) error = &local;
  uint32_t waited = 0;
  for (;;) {
    const uint32_t cause = device_.read32(kInt);
    if (cause & (kIntBitSwError | kIntBitHwError)) {
      device_.write32(kInt, kIntBitSwError | kIntBitHwError);
      *error = "the firmware reported an error while starting";
      return false;
    }
    if (cause & kIntBitAlive) {
      device_.write32(kInt, kIntBitAlive);
      return true;
    }
    if (waited >= timeout_us) {
      *error = "the firmware did not come alive";
      return false;
    }
    delay_us(10);
    waited += 10;
  }
}

}  // namespace hardware
