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

#include "Hardware/Platform/RaspberryPi/VcMailbox.hpp"

namespace hardware {

bool MmioVcMailboxPort::send(uint32_t channel, uint32_t value) {
  if ((value & 0xF) != 0 || channel > 0xF) return false;
  if (!registers_.wait32(kWriteStatus, kStatusFull, 0, attempts_)) return false;
  registers_.write32(kWrite, value | channel);
  return true;
}

bool MmioVcMailboxPort::receive(uint32_t channel, uint32_t* value) {
  for (;;) {
    if (!registers_.wait32(kReadStatus, kStatusEmpty, 0, attempts_)) return false;
    const uint32_t word = registers_.read32(kRead);
    if ((word & 0xF) == channel) {
      *value = word & ~0xFu;
      return true;
    }
  }
}

bool VcPropertyMessage::words_needed(ArrayView<const VcTag> tags, uint64_t* words) {
  uint64_t total = 2;  // total size, request/response code
  for (const VcTag& tag : tags) {
    const uint64_t add = 3 + tag.values.size();
    if (add > UINT32_MAX / 4 - total) return false;
    total += add;
  }
  total += 1;  // end tag
  *words = total;
  return true;
}

bool VcPropertyMessage::build(ArrayView<const VcTag> tags) {
  uint64_t needed = 0;
  if (!words_needed(tags, &needed) || needed > storage_.size()) return false;
  for (const VcTag& tag : tags)
    if (tag.id == 0) return false;  // 0 ends the list
  uint64_t at = 0;
  storage_[at++] = static_cast<uint32_t>(needed * 4);
  storage_[at++] = kRequest;
  for (const VcTag& tag : tags) {
    storage_[at++] = tag.id;
    storage_[at++] = static_cast<uint32_t>(tag.values.size() * 4);
    storage_[at++] = 0;  // request: the firmware sets bit 31 and the length of its answer
    for (uint32_t value : tag.values) storage_[at++] = value;
  }
  storage_[at++] = 0;
  return true;
}

bool VcPropertyMessage::parse(ArrayView<VcTag> tags) {
  uint32_t total = 0;
  if (!storage_.try_get(0, &total) || total % 4 != 0 || total / 4 > storage_.size()) return false;
  uint32_t code = 0;
  if (!storage_.try_get(1, &code) || code != kResponseOk) return false;
  uint64_t at = 2;
  const uint64_t end = total / 4;
  for (VcTag& tag : tags) {
    uint32_t id = 0, buffer_bytes = 0, tag_code = 0;
    if (end < 3 || at > end - 3) return false;
    id = storage_[at];
    buffer_bytes = storage_[at + 1];
    tag_code = storage_[at + 2];
    at += 3;
    if (id != tag.id || buffer_bytes != tag.values.size() * 4 || !(tag_code & kTagResponse)) return false;
    if (tag.values.size() > end - at) return false;
    const uint32_t used = tag_code & ~kTagResponse;
    if (used > buffer_bytes) return false;
    tag.response_words = (used + 3) / 4;
    for (uint32_t& value : tag.values) value = storage_[at++];
  }
  return true;
}

bool VcMailbox::transact(ArrayView<VcTag> tags) {
  VcPropertyMessage message(memory_.words());
  if (!message.build(ArrayView<const VcTag>(tags.data(), tags.size()))) return false;
  const uint32_t bus = memory_.bus_address();
  memory_.clean();
  if (!port_.send(kVcMailboxPropertyChannel, bus)) return false;
  uint32_t answer = 0;
  if (!port_.receive(kVcMailboxPropertyChannel, &answer) || answer != bus) return false;
  memory_.invalidate();
  return message.parse(tags);
}

bool VcMailbox::call(uint32_t tag, ArrayView<const uint32_t> request, ArrayView<uint32_t> response, uint32_t* response_words) {
  constexpr uint64_t kMaxValueWords = 64;  // an EDID block (32 words plus two) is the largest tag this OS sends
  const uint64_t count = request.size() > response.size() ? request.size() : response.size();
  if (count > kMaxValueWords) return false;
  uint32_t values[kMaxValueWords] = {};
  for (uint64_t i = 0; i < request.size(); ++i) values[i] = request[i];
  VcTag t;
  t.id = tag;
  t.values = ArrayView<uint32_t>(values, count);
  if (!transact(ArrayView<VcTag>(&t, 1))) return false;
  for (uint64_t i = 0; i < response.size(); ++i) response[i] = values[i];
  if (response_words) *response_words = t.response_words;
  return true;
}

}  // namespace hardware
