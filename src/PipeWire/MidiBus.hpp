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

// MIDI in the kernel's PipeWire (Graph.hpp): one bus that every MIDI port of every program is on, as PipeWire's Midi-Bridge is.
//   - What programs play (a pw_stream or pw_filter MIDI output port: pw-midiplay, a sequencer) goes to the bus's output (the machine's synthesizer: a
//     USB synthesizer or the MPU-401 when one is there, else the software synthesizer through the sound mixer) and to every other program's MIDI input.
//   - What keyboards play (the USB MIDI service hands its keys to input()) and what other programs play goes to every program's MIDI input port
//     (pw-midirecord, a DAW), each through a MidiQueue of its own.
// Messages are MIDI 1.0 channel and system messages of up to three bytes and whole system exclusive messages (F0 .. F7, at most kMaxSysex bytes). Each
// has the time it is to sound: a program's messages keep their places in its cycle (the Sequence's offsets), the output is handed them at those times
// (pump()), and an input port is given them at the same distances from each other. Thread safe: the bus's lock is never held while the output or a queue
// is called, so the output may call input() back.
#pragma once
#include <stdint.h>
#include "Kernel/Sync.hpp"
#include "Midi/MidiTypes.hpp"
#include "ObjectContainer/List.hpp"
#include "ObjectContainer/Queue.hpp"
#include "Utility/ArrayView.hpp"
#include "Utility/ByteBuffer.hpp"
#include "Utility/SharedPointer.hpp"

namespace pipewire {

constexpr uint32_t kMaxSysex = 65536;      // bytes of the longest system exclusive message carried (F0 and F7 counted)

struct MidiMessage {
  uint8_t bytes[3] = {};
  uint8_t size = 0;            // 1 .. 3 (0 for system exclusive)
  uint32_t source = 0;         // the graph node that played it (0: a keyboard)
  uint64_t time_ns = 0;        // when it is to sound (the bus's caller's clock)
  SharedPointer<ByteBuffer> sysex;      // a system exclusive message: its bytes from F0 to F7 (shared by the copies: never changed once made)
  bool is_sysex() const { return sysex.get() != nullptr; }
  // The message's bytes as they go on a MIDI cable.
  ConstByteView wire() const { return is_sysex() ? sysex->view() : ConstByteView(bytes, size); }
};

// A system exclusive message (F0 .. F7) of these bytes (real-time bytes among them are left out). False when it is not one (no F0 first, no F7 last,
// another status byte inside), is longer than kMaxSysex, or there is no memory.
bool make_sysex(allocators::IAllocator& allocator, ConstByteView bytes, MidiMessage* out);

// The bytes of a MIDI 1.0 stream (running status allowed) as messages: calls f(const MidiMessage&) for each. System exclusive messages are given whole
// when `sysex` is an allocator for them, else skipped (as are broken and unterminated ones). Returns how many were found.
template <class F>
uint32_t parse_midi_bytes(ConstByteView bytes, F&& f, allocators::IAllocator* sysex = nullptr);

// How many bytes a message with this status byte has (1 .. 3); 0 for a data byte, system exclusive and undefined status bytes.
uint8_t midi_message_size(uint8_t status);

// A UMP packet (Universal MIDI Packet: 1 to 4 32-bit words) as a MIDI 1.0 message, as PipeWire's spa_ump_to_midi does: MIDI 1.0 channel voice (type 2),
// system (type 1) and MIDI 2.0 channel voice (type 4, scaled down to 7 and 14 bits). False for the other kinds. *words: the packet's length in words.
uint32_t ump_packet_words(uint32_t first_word);
bool ump_to_midi(ArrayView<const uint32_t> packet, MidiMessage* out);
// A MIDI 1.0 message as a UMP word (MIDI 1.0 channel voice or system, group 0); 0 for system exclusive (see write_ump).
uint32_t midi_to_ump(const MidiMessage& m);
// A message as UMP words, calling put(uint32_t) for each: one word, or for system exclusive 64-bit Data Messages (type 3) of up to six bytes each.
// Returns how many words.
template <class F>
uint32_t write_ump(const MidiMessage& m, F&& put);
// How many UMP words write_ump gives for this message.
uint32_t ump_words_for(const MidiMessage& m);

// UMP packets to messages, system exclusive (type 3 Data Messages, which come in pieces) put back together: one per input stream.
class UmpDecoder {
 public:
  explicit UmpDecoder(allocators::IAllocator& allocator) : allocator_(allocator), building_(allocator) {}
  // True when this packet finished a message (*out); a piece of system exclusive, or a packet that says nothing in MIDI 1.0, gives false.
  bool decode(ArrayView<const uint32_t> packet, MidiMessage* out);
  bool building() const { return in_sysex_; }

 private:
  allocators::IAllocator& allocator_;
  ByteBuffer building_;
  bool in_sysex_ = false;
};

// What one MIDI input port of a program hears: the bus's messages, oldest first, at most kMaxMessages and kMaxBytes waiting (older ones are dropped and
// counted).
class MidiQueue {
 public:
  static constexpr uint32_t kMaxMessages = 4096;
  static constexpr uint64_t kMaxBytes = 1u << 20;
  MidiQueue(allocators::IAllocator& allocator, uint32_t owner) : messages_(allocator), owner_(owner) {}
  MidiQueue(const MidiQueue&) = delete;
  MidiQueue& operator=(const MidiQueue&) = delete;
  uint32_t owner() const { return owner_; }
  void push(const MidiMessage& m);
  bool pop(MidiMessage* out);
  enum class Choice { Take, Wait, Drop };
  // The oldest message if decide(const MidiMessage&) says Take; Wait leaves it there (false); Drop throws it away (counted as lost) and looks at the
  // next. decide is called with the queue's lock held: it must not call the queue.
  template <class F>
  bool pop_if(F&& decide, MidiMessage* out);
  uint64_t waiting();
  uint64_t lost();

 private:
  kernel::Mutex mutex_;
  Queue<MidiMessage> messages_;
  uint64_t bytes_ = 0;
  uint64_t lost_ = 0;
  uint32_t owner_;
};

class MidiBus final : public midi::IMidiSink {
 public:
  static constexpr uint32_t kMaxListeners = 64;
  static constexpr uint32_t kMaxScheduled = 4096;      // messages waiting for their time to go to the output (more go at once)
  explicit MidiBus(allocators::IAllocator& allocator) : allocator_(allocator), listeners_(allocator), scheduled_(allocator) {}
  MidiBus(const MidiBus&) = delete;
  MidiBus& operator=(const MidiBus&) = delete;

  // Where programs' notes sound (null: nowhere). Must outlive the bus or be set back to null first.
  void set_output(midi::IMidiSink* output);
  // A program's MIDI input port (`owner`: its graph node): hears keyboards and the other programs. Null when there are too many or no memory.
  SharedPointer<MidiQueue> listen(uint32_t owner);
  void unlisten(const MidiQueue* queue);
  // A program's MIDI output: to every queue but its own node's at once (each message keeps its time_ns), and to the output at m.time_ns: now when that
  // is not after `now_ns`, else by pump().
  void publish(const MidiMessage& m, uint64_t now_ns);
  void publish(const MidiMessage& m) { publish(m, m.time_ns); }
  // Hands the output the messages whose time has come. Returns when the next is due (0: none waiting).
  uint64_t pump(uint64_t now_ns);
  uint64_t next_due();
  // --- IMidiSink: a keyboard's keys (to every queue, not to the output: the keyboard's own routing sounds them)
  void send(uint8_t status, uint8_t data1, uint8_t data2) override;
  void send_sysex(ConstByteView message) override;
  void reset() override {}
  uint64_t published() const { return __atomic_load_n(&published_, __ATOMIC_RELAXED); }
  // What keyboards' messages are stamped with (the caller's clock, in ns; null: 0, which input ports read as the start of their cycle).
  void set_clock(uint64_t (*clock)()) { clock_ = clock; }

 private:
  void deliver(const MidiMessage& m, bool to_queues, bool to_output);
  static void play(midi::IMidiSink& output, const MidiMessage& m);
  allocators::IAllocator& allocator_;
  kernel::Mutex mutex_;
  midi::IMidiSink* output_ = nullptr;
  List<SharedPointer<MidiQueue>> listeners_;
  List<MidiMessage> scheduled_;
  uint64_t (*clock_)() = nullptr;      // (by time; equal times in the order published)
  uint64_t published_ = 0;
};

// ---- templates

template <class F>
bool MidiQueue::pop_if(F&& decide, MidiMessage* out) {
  kernel::ScopedMutex guard(mutex_);
  while (MidiMessage* front = messages_.front()) {
    const Choice c = decide(static_cast<const MidiMessage&>(*front));
    if (c == Choice::Wait) return false;
    bytes_ -= front->wire().size();
    if (c == Choice::Take) *out = util::move(*front);
    else ++lost_;
    messages_.dequeue();
    if (c == Choice::Take) return true;
  }
  return false;
}

template <class F>
uint32_t parse_midi_bytes(ConstByteView bytes, F&& f, allocators::IAllocator* sysex) {
  uint32_t found = 0;
  uint8_t running = 0;
  uint64_t i = 0;
  while (i < bytes.size()) {
    uint8_t status = bytes[i];
    if (status == 0xF0) {      // system exclusive: up to its F7 (real-time bytes inside are messages of their own; another status byte breaks it)
      const uint64_t start = i++;
      bool ended = false;
      while (i < bytes.size()) {
        const uint8_t d = bytes[i];
        if (d >= 0xF8) {
          MidiMessage rt;
          rt.bytes[0] = d;
          rt.size = 1;
          if (midi_message_size(d)) { f(rt); ++found; }
          ++i;
          continue;
        }
        if (d >= 0x80) {
          if (d == 0xF7) { ++i; ended = true; }
          break;
        }
        ++i;
      }
      running = 0;
      MidiMessage m;
      if (ended && sysex && make_sysex(*sysex, bytes.subview(start, i - start), &m)) { f(m); ++found; }
      continue;
    }
    if (status >= 0xF8) {      // real time: one byte, anywhere, running status kept
      MidiMessage m;
      m.bytes[0] = status;
      m.size = 1;
      if (midi_message_size(status)) { f(m); ++found; }
      ++i;
      continue;
    }
    uint64_t data_at = i + 1;
    if (status < 0x80) {      // running status: this is the first data byte
      if (!running) { ++i; continue; }
      status = running;
      data_at = i;
    }
    const uint8_t size = midi_message_size(status);
    if (size == 0) { ++i; running = 0; continue; }
    const uint64_t data_count = size - 1u;
    MidiMessage m;
    m.bytes[0] = status;
    m.size = size;
    // the data bytes (a real-time byte among them is a message of its own, in its place)
    uint64_t j = data_at, k = 0;
    bool good = true, cut = false;
    while (k < data_count) {
      if (j >= bytes.size()) { cut = true; break; }
      const uint8_t d = bytes[j];
      if (d >= 0xF8) {
        MidiMessage rt;
        rt.bytes[0] = d;
        rt.size = 1;
        if (midi_message_size(d)) { f(rt); ++found; }
        ++j;
        continue;
      }
      if (d >= 0x80) { good = false; break; }      // (a status byte where data was due: start again there)
      m.bytes[1 + k++] = d;
      ++j;
    }
    if (cut) break;
    if (!good) { i = j; running = 0; continue; }
    running = status < 0xF0 ? status : 0;
    f(m);
    ++found;
    i = j;
  }
  return found;
}

template <class F>
uint32_t write_ump(const MidiMessage& m, F&& put) {
  if (!m.is_sysex()) {
    put(midi_to_ump(m));
    return 1;
  }
  // the bytes between F0 and F7, six to a packet: status 0 complete in one, else 1 start, 2 continue, 3 end
  const ConstByteView all = m.sysex->view();
  const ConstByteView body = all.size() >= 2 ? all.subview(1, all.size() - 2) : ConstByteView();
  uint32_t words = 0;
  uint64_t at = 0;
  do {
    const uint64_t left = body.size() - at;
    const uint32_t count = left > 6 ? 6 : static_cast<uint32_t>(left);
    const bool first = at == 0, last = left <= 6;
    const uint32_t status = first && last ? 0 : first ? 1 : last ? 3 : 2;
    uint8_t b[6] = {};
    for (uint32_t k = 0; k < count; ++k) b[k] = body[at + k] & 0x7F;
    put(0x3u << 28 | status << 20 | count << 16 | uint32_t(b[0]) << 8 | b[1]);
    put(uint32_t(b[2]) << 24 | uint32_t(b[3]) << 16 | uint32_t(b[4]) << 8 | b[5]);
    words += 2;
    at += count;
  } while (at < body.size());
  return words;
}

}  // namespace pipewire
