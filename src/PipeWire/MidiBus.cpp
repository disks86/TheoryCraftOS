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

#include "PipeWire/MidiBus.hpp"

namespace pipewire {

uint8_t midi_message_size(uint8_t status) {
  if (status < 0x80) return 0;
  if (status < 0xF0) {
    const uint8_t kind = status & 0xF0;
    return kind == 0xC0 || kind == 0xD0 ? 2 : 3;
  }
  switch (status) {
    case 0xF1: case 0xF3: return 2;
    case 0xF2: return 3;
    case 0xF6: case 0xF8: case 0xFA: case 0xFB: case 0xFC: case 0xFE: case 0xFF: return 1;
    default: return 0;      // (F0/F7 system exclusive, F4/F5/F9/FD undefined)
  }
}

uint32_t ump_packet_words(uint32_t first_word) {
  switch (first_word >> 28) {
    case 0x0: case 0x1: case 0x2: case 0x6: case 0x7: return 1;
    case 0x3: case 0x4: case 0x8: case 0x9: case 0xA: return 2;
    case 0xB: case 0xC: return 3;
    default: return 4;      // 0x5, 0xD, 0xE, 0xF
  }
}

bool ump_to_midi(ArrayView<const uint32_t> packet, MidiMessage* out) {
  if (packet.size() == 0) return false;
  const uint32_t w0 = packet[0];
  const uint32_t type = w0 >> 28;
  MidiMessage m;
  if (type == 0x1 || type == 0x2) {
    m.bytes[0] = uint8_t(w0 >> 16);
    m.size = midi_message_size(m.bytes[0]);
    if (m.size == 0 || (type == 0x2 && m.bytes[0] >= 0xF0) || (type == 0x1 && m.bytes[0] < 0xF0)) return false;
    m.bytes[1] = uint8_t(w0 >> 8) & 0x7F;
    m.bytes[2] = uint8_t(w0) & 0x7F;
    *out = m;
    return true;
  }
  if (type != 0x4 || packet.size() < 2) return false;
  const uint32_t w1 = packet[1];
  const uint8_t kind = uint8_t(w0 >> 16) & 0xF0, channel = uint8_t(w0 >> 16) & 0x0F;
  const uint8_t index = uint8_t(w0 >> 8) & 0x7F;
  m.bytes[0] = kind | channel;
  switch (kind) {
    case 0x80: case 0x90: {      // note off / on: the 16-bit velocity's top seven bits (a note on is never velocity 0: that would be a note off)
      uint8_t velocity = uint8_t(w1 >> 25);
      if (kind == 0x90 && velocity == 0) velocity = 1;
      m.bytes[1] = index;
      m.bytes[2] = velocity;
      m.size = 3;
      break;
    }
    case 0xA0: case 0xB0:
      m.bytes[1] = index;
      m.bytes[2] = uint8_t(w1 >> 25);
      m.size = 3;
      break;
    case 0xC0:
      m.bytes[1] = uint8_t(w1 >> 24) & 0x7F;
      m.size = 2;
      break;
    case 0xD0:
      m.bytes[1] = uint8_t(w1 >> 25);
      m.size = 2;
      break;
    case 0xE0: {
      const uint32_t bend = w1 >> 18;      // (14 bits)
      m.bytes[1] = uint8_t(bend & 0x7F);
      m.bytes[2] = uint8_t(bend >> 7) & 0x7F;
      m.size = 3;
      break;
    }
    default: return false;      // (registered and assignable controllers, per-note messages: nothing in MIDI 1.0 says them in one message)
  }
  *out = m;
  return true;
}

bool make_sysex(allocators::IAllocator& allocator, ConstByteView bytes, MidiMessage* out) {
  if (bytes.size() < 2 || bytes[0] != 0xF0 || bytes[bytes.size() - 1] != 0xF7) return false;
  uint64_t length = 0;
  for (uint64_t i = 0; i < bytes.size(); ++i) {
    const uint8_t b = bytes[i];
    if (b >= 0xF8) continue;
    if (b >= 0x80 && i != 0 && i != bytes.size() - 1) return false;
    ++length;
  }
  if (length > kMaxSysex) return false;
  SharedPointer<ByteBuffer> buffer = SharedPointer<ByteBuffer>::make(allocator, allocator);
  if (!buffer) return false;
  for (uint64_t i = 0; i < bytes.size();) {      // (the runs between real-time bytes)
    uint64_t end = i;
    while (end < bytes.size() && bytes[end] < 0xF8) ++end;
    if (end > i && !buffer->append(bytes.subview(i, end - i))) return false;
    i = end + 1;
  }
  MidiMessage m;
  m.sysex = util::move(buffer);
  *out = util::move(m);
  return true;
}

uint32_t ump_words_for(const MidiMessage& m) {
  if (!m.is_sysex()) return 1;
  const uint64_t body = m.sysex->size() >= 2 ? m.sysex->size() - 2 : 0;
  const uint64_t packets = body == 0 ? 1 : (body + 5) / 6;
  return static_cast<uint32_t>(packets * 2);
}

bool UmpDecoder::decode(ArrayView<const uint32_t> packet, MidiMessage* out) {
  if (packet.size() == 0) return false;
  const uint32_t w0 = packet[0];
  if (w0 >> 28 != 0x3) return ump_to_midi(packet, out);
  // a 64-bit Data Message: a piece of system exclusive (status 0 whole, 1 start, 2 continue, 3 end; up to six bytes)
  if (packet.size() < 2) return false;
  const uint32_t w1 = packet[1];
  const uint32_t status = (w0 >> 20) & 0xF, count = (w0 >> 16) & 0xF;
  if (status > 3 || count > 6) {
    in_sysex_ = false;
    return false;
  }
  if (status == 0 || status == 1) {
    building_.clear();
    const uint8_t start = 0xF0;
    in_sysex_ = building_.append(ConstByteView(&start, 1));
  } else if (!in_sysex_) {
    return false;      // (a continuation without its start)
  }
  const uint8_t b[6] = {uint8_t(w0 >> 8), uint8_t(w0), uint8_t(w1 >> 24), uint8_t(w1 >> 16), uint8_t(w1 >> 8), uint8_t(w1)};
  for (uint32_t k = 0; k < count && in_sysex_; ++k) {
    const uint8_t d = b[k] & 0x7F;
    in_sysex_ = building_.size() < kMaxSysex - 1 && building_.append(ConstByteView(&d, 1));
  }
  if (!in_sysex_ || status == 1 || status == 2) return false;
  in_sysex_ = false;
  const uint8_t end = 0xF7;
  return building_.append(ConstByteView(&end, 1)) && make_sysex(allocator_, building_.view(), out);
}

uint32_t midi_to_ump(const MidiMessage& m) {
  if (m.is_sysex()) return 0;
  const uint32_t type = m.bytes[0] >= 0xF0 ? 0x1u : 0x2u;
  return type << 28 | uint32_t(m.bytes[0]) << 16 | uint32_t(m.size > 1 ? m.bytes[1] : 0) << 8 | uint32_t(m.size > 2 ? m.bytes[2] : 0);
}

void MidiQueue::push(const MidiMessage& m) {
  const uint64_t length = m.wire().size();
  if (length > kMaxBytes) return;
  kernel::ScopedMutex guard(mutex_);
  while (messages_.count() > 0 && (messages_.count() >= kMaxMessages || bytes_ > kMaxBytes - length)) {
    bytes_ -= messages_.front()->wire().size();
    messages_.dequeue();
    ++lost_;
  }
  if (messages_.enqueue(m)) bytes_ += length;
  else ++lost_;
}

bool MidiQueue::pop(MidiMessage* out) {
  kernel::ScopedMutex guard(mutex_);
  MidiMessage* front = messages_.front();
  if (!front) return false;
  bytes_ -= front->wire().size();
  *out = util::move(*front);
  messages_.dequeue();
  return true;
}

uint64_t MidiQueue::waiting() {
  kernel::ScopedMutex guard(mutex_);
  return messages_.count();
}

uint64_t MidiQueue::lost() {
  kernel::ScopedMutex guard(mutex_);
  return lost_;
}

void MidiBus::set_output(midi::IMidiSink* output) {
  kernel::ScopedMutex guard(mutex_);
  output_ = output;
}

SharedPointer<MidiQueue> MidiBus::listen(uint32_t owner) {
  SharedPointer<MidiQueue> q = SharedPointer<MidiQueue>::make(allocator_, allocator_, owner);
  if (!q) return q;
  kernel::ScopedMutex guard(mutex_);
  if (listeners_.count() >= kMaxListeners || !listeners_.push_back(q)) return SharedPointer<MidiQueue>();
  return q;
}

void MidiBus::unlisten(const MidiQueue* queue) {
  kernel::ScopedMutex guard(mutex_);
  for (uint64_t i = 0; i < listeners_.count(); ++i)
    if (listeners_[i].get() == queue) {
      listeners_.remove_at(i);
      return;
    }
}

void MidiBus::play(midi::IMidiSink& output, const MidiMessage& m) {
  if (m.is_sysex()) output.send_sysex(m.sysex->view());
  else output.send(m.bytes[0], m.size > 1 ? m.bytes[1] : 0, m.size > 2 ? m.bytes[2] : 0);
}

void MidiBus::deliver(const MidiMessage& m, bool to_queues, bool to_output) {
  // the output and the queues are called with the bus's lock let go (copies of what is there now)
  midi::IMidiSink* output = nullptr;
  SharedPointer<MidiQueue> copies[kMaxListeners];
  uint32_t n = 0;
  {
    kernel::ScopedMutex guard(mutex_);
    if (to_output) output = output_;
    if (to_queues)
      for (const SharedPointer<MidiQueue>& q : listeners_)
        if (n < kMaxListeners && (m.source == 0 || q->owner() != m.source)) copies[n++] = q;
  }
  if (output) play(*output, m);
  for (uint32_t i = 0; i < n; ++i) copies[i]->push(m);
}

void MidiBus::publish(const MidiMessage& m, uint64_t now_ns) {
  if (m.is_sysex() ? m.sysex->size() < 2 || m.sysex->size() > kMaxSysex : m.size == 0 || m.size > 3) return;
  bool later = false;
  {
    kernel::ScopedMutex guard(mutex_);
    ++published_;
    // (behind the ones already waiting when it is no later than they are, so the output hears a program's messages in order)
    if ((m.time_ns > now_ns || scheduled_.count() > 0) && scheduled_.count() < kMaxScheduled) {
      uint64_t at = scheduled_.count();
      while (at > 0 && scheduled_[at - 1].time_ns > m.time_ns) --at;
      later = scheduled_.insert(at, m);
    }
  }
  deliver(m, true, !later);
  if (later) pump(now_ns);
}

uint64_t MidiBus::pump(uint64_t now_ns) {
  for (;;) {
    MidiMessage m;
    midi::IMidiSink* output = nullptr;
    {
      kernel::ScopedMutex guard(mutex_);
      if (scheduled_.count() == 0) return 0;
      if (scheduled_[0].time_ns > now_ns) return scheduled_[0].time_ns;
      m = util::move(scheduled_[0]);
      scheduled_.remove_at(0);
      output = output_;
    }
    if (output) play(*output, m);
  }
}

uint64_t MidiBus::next_due() {
  kernel::ScopedMutex guard(mutex_);
  return scheduled_.count() ? scheduled_[0].time_ns : 0;
}

void MidiBus::send(uint8_t status, uint8_t data1, uint8_t data2) {
  MidiMessage m;
  m.bytes[0] = status;
  m.bytes[1] = data1;
  m.bytes[2] = data2;
  m.size = midi_message_size(status);
  if (m.size == 0) return;
  if (clock_) m.time_ns = clock_();
  deliver(m, true, false);
}

void MidiBus::send_sysex(ConstByteView message) {
  MidiMessage m;
  if (!make_sysex(allocator_, message, &m)) return;
  if (clock_) m.time_ns = clock_();
  deliver(m, true, false);
}

}  // namespace pipewire
