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

#include "Hardware/StorageDevice/Sd/SdStorageDevice.hpp"

namespace hardware {

bool SdStorageDevice::initialize() {
  generation_ = host_.generation();
  if (!card_.initialize()) return false;
  const SdCardInfo& card = card_.info();
  info_ = StorageInfo();
  info_.kind = StorageKind::Disk;
  info_.block_size = SdCard::kBlockSize;
  info_.block_count = card.block_count;
  info_.read_only = card.read_only || host_.write_protected();
  info_.removable = true;
  const char prefix[] = "SD ";
  uint32_t n = 0;
  for (const char* p = prefix; *p; ++p) info_.model[n++] = *p;
  for (uint32_t i = 0; card.name[i] && n < sizeof info_.model - 1; ++i) info_.model[n++] = card.name[i];
  while (n > 0 && info_.model[n - 1] == ' ') info_.model[--n] = 0;
  for (uint32_t i = 0; i < 8; ++i) {                                   // the serial number as eight hex digits
    const uint32_t digit = (card.serial >> (28 - 4 * i)) & 0xF;
    info_.serial[i] = static_cast<char>(digit < 10 ? '0' + digit : 'A' + digit - 10);
  }
  if (!prepare_bounce()) return false;
  return true;
}

void SdStorageDevice::shutdown() {
  kernel::ScopedMutex guard(mutex_);                                    // (a request still running is over by now)
  if (gone_) return;
  gone_ = true;
  if (host_.generation() == generation_ && host_.card_present()) host_.power_off();
  release_bounce();
}

bool SdStorageDevice::transfer(bool write, uint64_t lba, uint32_t blocks, ByteView dma) {
  if (write && (info_.read_only || host_.write_protected())) return false;
  const uint32_t step = card_.max_blocks() ? card_.max_blocks() : 1;      // (the host moves so many blocks at a time)
  for (uint32_t done = 0; done < blocks;) {
    if (!present()) return false;
    const uint32_t n = blocks - done < step ? blocks - done : step;
    const ByteView part = dma.subview(uint64_t{done} * SdCard::kBlockSize, uint64_t{n} * SdCard::kBlockSize);
    if (!(write ? card_.write(lba + done, n, part) : card_.read(lba + done, n, part))) return false;
    done += n;
  }
  return true;
}

// ---- the slot ---------------------------------------------------------------------------------------------------------------------------------------------------------------------

void SdSlotSource::discover(List<UniquePointer<IStorageDevice>>& found) {
  if (!host_.card_present()) {
    have_generation_ = false;                                          // (the next card is a new one whatever the host counts)
    attempts_ = 0;
    return;
  }
  const uint32_t generation = host_.generation();
  if (have_generation_ && generation == seen_generation_) {
    if (attempts_ == 0 || attempts_ >= kMaxAttempts) return;           // the card of this generation is taken care of (or given up on)
  } else {
    have_generation_ = true;
    seen_generation_ = generation;
    attempts_ = 0;
  }
  ++attempts_;
  UniquePointer<SdStorageDevice> device = UniquePointer<SdStorageDevice>::make(allocator_, host_, allocator_);
  if (!device) return;
  if (!device->initialize()) {                                         // not seated yet, not an SD memory card, or a card that does not answer: tried again a couple of times
    device->shutdown();
    return;
  }
  attempts_ = 0;
  found.push_back(UniquePointer<IStorageDevice>(static_cast<UniquePointer<SdStorageDevice>&&>(device)));
}

}  // namespace hardware
