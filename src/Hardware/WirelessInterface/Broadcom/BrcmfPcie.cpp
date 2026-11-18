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

// The PCIe transport after Linux brcmfmac pcie.c; see BrcmfPcie.hpp.
#include "Hardware/WirelessInterface/Broadcom/BrcmfPcie.hpp"
#include "Kernel/Delay.hpp"
#include "Utility/ByteStream.hpp"

namespace hardware {

using namespace brcmf_pcie;

namespace {
constexpr uint16_t kLinkStatusCtrl = 0xbc;          // BRCMF_PCIE_REG_LINK_STATUS_CTRL
constexpr uint32_t kLinkStatusCtrlAspmEnable = 3;
}  // namespace

uint32_t BrcmfPcieBackplane::select(uint32_t address) {
  const uint32_t window = address & ~(kBar0RegSize - 1);
  if (!window_valid_ || window != window_) {
    pci_.write32(address_, kBar0Window, window);
    window_ = window;
    window_valid_ = true;
  }
  return address & (kBar0RegSize - 1);
}

bool BrcmfPcieBackplane::read32(uint32_t address, uint32_t* value) {
  if (address & 3) return false;
  *value = bar0_.read32(select(address));
  return true;
}

bool BrcmfPcieBackplane::write32(uint32_t address, uint32_t value) {
  if (address & 3) return false;
  bar0_.write32(select(address), value);
  return true;
}

bool BrcmfPcieBackplane::write_block(uint32_t address, ConstByteView data) {
  if (data.size() > 0x100000000ull - address) return false;
  uint64_t done = 0;
  while (done < data.size()) {
    const uint32_t at = address + static_cast<uint32_t>(done);
    const uint32_t offset = select(at);
    const uint64_t remaining = data.size() - done;
    if ((at & 3) == 0 && remaining >= 4) {
      const uint32_t word = uint32_t{data[done]} | uint32_t{data[done + 1]} << 8 | uint32_t{data[done + 2]} << 16 | uint32_t{data[done + 3]} << 24;
      bar0_.write32(offset, word);
      done += 4;
    } else {
      bar0_.write8(offset, data[done]);
      done += 1;
    }
  }
  return true;
}

bool BrcmfPcieBackplane::read_block(uint32_t address, ByteView data) {
  if (data.size() > 0x100000000ull - address) return false;
  uint64_t done = 0;
  while (done < data.size()) {
    const uint32_t at = address + static_cast<uint32_t>(done);
    const uint32_t offset = select(at);
    const uint64_t remaining = data.size() - done;
    if ((at & 3) == 0 && remaining >= 4) {
      const uint32_t word = bar0_.read32(offset);
      for (int i = 0; i < 4; ++i) data[done + i] = static_cast<uint8_t>(word >> (8 * i));
      done += 4;
    } else {
      data[done] = bar0_.read8(offset);
      done += 1;
    }
  }
  return true;
}

bool brcmf_pcie_reset_device(BrcmfPcieBackplane& bp, const BrcmfCoreList& cores) {
  const BrcmfCore* chipcommon = cores.find(brcmf_bp::kCoreChipCommon);
  const BrcmfCore* pcie2 = cores.find(brcmf_bp::kCorePcie2);
  if (!chipcommon || !pcie2) return false;
  const uint32_t link_control = bp.config_read32(kLinkStatusCtrl);
  bp.config_write32(kLinkStatusCtrl, link_control & ~kLinkStatusCtrlAspmEnable);
  if (!bp.write32(chipcommon->base + brcmf_bp::kCcWatchdog, 4)) return false;
  delay_us(100 * 1000);
  bp.forget_window();
  bp.config_write32(kLinkStatusCtrl, link_control);
  if (pcie2->revision <= 13) {
    for (uint16_t offset : kResetConfigOffsets) {
      uint32_t value = 0;
      if (!bp.write32(pcie2->base + kPcie2ConfigAddr, offset) || !bp.read32(pcie2->base + kPcie2ConfigData, &value) ||
          !bp.write32(pcie2->base + kPcie2ConfigData, value))
        return false;
    }
  }
  uint32_t mailbox = 0;
  if (!bp.read32(pcie2->base + kPcie2MailboxInt, &mailbox)) return false;
  if (mailbox != 0xffffffff && !bp.write32(pcie2->base + kPcie2MailboxInt, mailbox)) return false;
  return true;
}

bool brcmf_pcie_layout(const BrcmfRamInfo& ram, uint32_t firmware_bytes, uint32_t nvram_bytes, bool random_seed, BrcmfPcieLayout* out) {
  *out = BrcmfPcieLayout();
  if (ram.size < 8 || uint64_t{ram.base} + ram.size > 0x100000000ull || (nvram_bytes & 3) != 0) return false;
  out->firmware = ram.base;
  out->shared_word = ram.base + ram.size - 4;
  // what goes at the top of RAM: the NVRAM (its token in the last word), and the seed below it
  uint64_t top = 4;    // without NVRAM the last word is still the shared pointer's
  if (nvram_bytes) {
    top = nvram_bytes;
    if (random_seed) top += kRandomSeedFooterBytes + kRandomSeedLength;
  }
  if (top > ram.size || firmware_bytes > ram.size - top) return false;
  if (nvram_bytes) {
    out->nvram = ram.base + ram.size - nvram_bytes;
    if (random_seed) {
      out->seed_footer = out->nvram - kRandomSeedFooterBytes;
      out->seed = out->seed_footer - kRandomSeedLength;
    }
  }
  return true;
}

void brcmf_pcie_seed_footer(uint8_t out[8]) {
  ByteWriter writer(ByteView(out, 8));
  writer.le32(kRandomSeedLength);
  writer.le32(kRandomSeedMagic);
}

namespace {
bool inside_ram(const BrcmfRamInfo& ram, uint32_t address, uint32_t bytes) {
  return address >= ram.base && address - ram.base <= ram.size && bytes <= ram.size - (address - ram.base);
}
}  // namespace

BrcmfSharedResult brcmf_pcie_parse_shared(ConstByteView bytes, uint32_t address, const BrcmfRamInfo& ram, BrcmfPcieShared* out) {
  *out = BrcmfPcieShared();
  if (!inside_ram(ram, address, 4)) return BrcmfSharedResult::BadAddress;
  if (bytes.size() < kSharedBytes) return BrcmfSharedResult::Truncated;
  ByteReader in(bytes);
  out->address = address;
  out->flags = in.le32();
  out->version = static_cast<uint8_t>(out->flags & kSharedVersionMask);
  if (out->version < kMinSharedVersion || out->version > kMaxSharedVersion) return BrcmfSharedResult::UnsupportedVersion;
  if (out->flags & kSharedDmaIndex) out->dma_index_size = (out->flags & kSharedDma2BIdx) ? 2 : 4;
  if (!in.seek(kSharedConsoleAddrOffset)) return BrcmfSharedResult::Truncated;
  out->console_addr = in.le32();
  if (!in.seek(kSharedMaxRxBufPostOffset)) return BrcmfSharedResult::Truncated;
  out->max_rxbufpost = in.le16();
  if (out->max_rxbufpost == 0) out->max_rxbufpost = kDefaultMaxRxBufPost;
  out->rx_dataoffset = in.le32();
  out->htod_mb_data_addr = in.le32();
  out->dtoh_mb_data_addr = in.le32();
  out->ring_info_addr = in.le32();
  if (!in.ok()) return BrcmfSharedResult::Truncated;
  return BrcmfSharedResult::Ok;
}

bool brcmf_parse_console(ConstByteView bytes, const BrcmfRamInfo& ram, BrcmfConsole* out) {
  *out = BrcmfConsole();
  ByteReader in(bytes);
  if (!in.seek(kConsoleBufAddrOffset)) return false;
  out->buffer = in.le32();
  out->size = in.le32();
  out->write_index = in.le32();
  if (!in.ok() || out->size == 0 || !inside_ram(ram, out->buffer, out->size) || out->write_index >= out->size) return false;
  return true;
}

}  // namespace hardware
