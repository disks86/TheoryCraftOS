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

// MIDI in this OS: where the notes of a song go (an IMidiSink), and the events of a song file in time order. A sink is either the software
// synthesizer (SoftwareSynth, which renders sound the audio system plays) or a MIDI output of the hardware (a sound card's or an interface's
// port, see Hardware/MidiDevice) that makes the sound itself.
#pragma once
#include <stdint.h>

namespace midi {

// A channel message: `status` holds the kind in the high nibble (0x80 note off, 0x90 note on, 0xA0 key pressure, 0xB0 controller,
// 0xC0 program change, 0xD0 channel pressure, 0xE0 pitch bend) and the channel (0-15) in the low one. Program change and channel pressure
// have one data byte (data2 is 0). Pitch bend is 14 bits: data1 the low seven, data2 the high seven.
struct Event {
  uint64_t time_us = 0;   // from the start of the song
  uint8_t status = 0, data1 = 0, data2 = 0;
};

constexpr uint8_t kControllerVolume = 7, kControllerPan = 10, kControllerExpression = 11, kControllerSustain = 64, kControllerAllSoundOff = 120,
                  kControllerResetAll = 121, kControllerAllNotesOff = 123, kControllerModulation = 1;

// What the notes are sent to. Calls may come from any thread but one at a time per sink.
class IMidiSink {
 public:
  virtual void send(uint8_t status, uint8_t data1, uint8_t data2) = 0;
  // Silences everything and puts every channel back to its start values.
  virtual void reset() = 0;

 protected:
  constexpr IMidiSink() = default;
  ~IMidiSink() = default;
};

// Sounds that are playing stop (all sound off, all notes off on every channel), without touching the programs and volumes.
inline void silence(IMidiSink& sink) {
  for (uint8_t channel = 0; channel < 16; ++channel) {
    sink.send(static_cast<uint8_t>(0xB0 | channel), kControllerAllSoundOff, 0);
    sink.send(static_cast<uint8_t>(0xB0 | channel), kControllerAllNotesOff, 0);
  }
}

}  // namespace midi
