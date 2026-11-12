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
// Messages are MIDI 1.0 channel and system messages of up to three bytes (system exclusive is not carried). Thread safe: the bus's lock is never held
// while the output or a queue is called, so the output may call input() back.
#pragma once
#include <stdint.h>
#include "Kernel/Sync.hpp"
#include "Midi/MidiTypes.hpp"
#include "ObjectContainer/List.hpp"
#include "ObjectContainer/Queue.hpp"
#include "Utility/ArrayView.hpp"
#include "Utility/SharedPointer.hpp"

namespace pipewire {

struct MidiMessage {
  uint8_t bytes[3] = {};
  uint8_t size = 0;            // 1 .. 3
  uint32_t source = 0;         // the graph node that played it (0: a keyboard)
  uint64_t time_ns = 0;        // when it was played (the bus's caller's clock)
};

// The bytes of a MIDI 1.0 stream (running status allowed) as messages: calls f(const MidiMessage&) for each. System exclusive and broken messages are
// skipped. Returns how many were found.
template <class F>
uint32_t parse_midi_bytes(ConstByteView bytes, F&& f);

// How many bytes a message with this status byte has (1 .. 3); 0 for a data byte, system exclusive and undefined status bytes.
uint8_t midi_message_size(uint8_t status);

// A UMP packet (Universal MIDI Packet: 1 to 4 32-bit words) as a MIDI 1.0 message, as PipeWire's spa_ump_to_midi does: MIDI 1.0 channel voice (type 2),
// system (type 1) and MIDI 2.0 channel voice (type 4, scaled down to 7 and 14 bits). False for the other kinds. *words: the packet's length in words.
uint32_t ump_packet_words(uint32_t first_word);
bool ump_to_midi(ArrayView<const uint32_t> packet, MidiMessage* out);
// A MIDI 1.0 message as a UMP word (MIDI 1.0 channel voice or system, group 0).
uint32_t midi_to_ump(const MidiMessage& m);

// What one MIDI input port of a program hears: the bus's messages, oldest first, at most kMaxMessages waiting (older ones are dropped and counted).
class MidiQueue {
 public:
  static constexpr uint32_t kMaxMessages = 4096;
  MidiQueue(allocators::IAllocator& allocator, uint32_t owner) : messages_(allocator), owner_(owner) {}
  MidiQueue(const MidiQueue&) = delete;
  MidiQueue& operator=(const MidiQueue&) = delete;
  uint32_t owner() const { return owner_; }
  void push(const MidiMessage& m);
  bool pop(MidiMessage* out);
  uint64_t waiting();
  uint64_t lost();

 private:
  kernel::Mutex mutex_;
  Queue<MidiMessage> messages_;
  uint64_t lost_ = 0;
  uint32_t owner_;
};

class MidiBus final : public midi::IMidiSink {
 public:
  static constexpr uint32_t kMaxListeners = 64;
  explicit MidiBus(allocators::IAllocator& allocator) : allocator_(allocator), listeners_(allocator) {}
  MidiBus(const MidiBus&) = delete;
  MidiBus& operator=(const MidiBus&) = delete;

  // Where programs' notes sound (null: nowhere). Must outlive the bus or be set back to null first.
  void set_output(midi::IMidiSink* output);
  // A program's MIDI input port (`owner`: its graph node): hears keyboards and the other programs. Null when there are too many or no memory.
  SharedPointer<MidiQueue> listen(uint32_t owner);
  void unlisten(const MidiQueue* queue);
  // A program's MIDI output: to the output and to every queue but its own node's.
  void publish(const MidiMessage& m);
  // --- IMidiSink: a keyboard's keys (to every queue, not to the output: the keyboard's own routing sounds them)
  void send(uint8_t status, uint8_t data1, uint8_t data2) override;
  void reset() override {}
  uint64_t published() const { return __atomic_load_n(&published_, __ATOMIC_RELAXED); }

 private:
  void deliver(const MidiMessage& m, bool to_output);
  allocators::IAllocator& allocator_;
  kernel::Mutex mutex_;
  midi::IMidiSink* output_ = nullptr;
  List<SharedPointer<MidiQueue>> listeners_;
  uint64_t published_ = 0;
};

// ---- template

template <class F>
uint32_t parse_midi_bytes(ConstByteView bytes, F&& f) {
  uint32_t found = 0;
  uint8_t running = 0;
  uint64_t i = 0;
  while (i < bytes.size()) {
    uint8_t status = bytes[i];
    if (status == 0xF0) {      // system exclusive: up to its end (or the next status byte)
      ++i;
      while (i < bytes.size() && bytes[i] < 0x80) ++i;
      if (i < bytes.size() && bytes[i] == 0xF7) ++i;
      running = 0;
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

}  // namespace pipewire
