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

// A General MIDI style synthesizer in software: 16 channels, up to kMaxVoices notes at once, and a sound for each of the 128 programs made
// from the family of instruments the program belongs to (pianos, organs, strings, brass, reeds, synth leads, ...) by additive wavetables
// with an attack / decay / sustain / release envelope; channel 10 plays drum sounds made from noise and falling tones. It understands note
// on / off, program change, pitch bend (with the bend range set by RPN 0), volume, expression, pan, modulation (vibrato), the sustain pedal,
// all sound off / all notes off and reset all controllers. Output is 48 kHz stereo, 16-bit.
//
// send() and render() may be called from different threads (a lock covers the state).
#pragma once
#include "Allocators/IAllocator.hpp"
#include "Kernel/Sync.hpp"
#include "Midi/MidiTypes.hpp"

namespace midi {

class SoftwareSynth final : public IMidiSink {
 public:
  static constexpr uint32_t kSampleRate = 48000, kMaxVoices = 48, kTableSize = 2048, kLevels = 5, kTimbres = 11;

  explicit SoftwareSynth(allocators::IAllocator& allocator);
  ~SoftwareSynth();
  SoftwareSynth(const SoftwareSynth&) = delete;
  SoftwareSynth& operator=(const SoftwareSynth&) = delete;

  // Builds the wavetables; false if memory ran out (then nothing sounds).
  bool initialize();

  void send(uint8_t status, uint8_t data1, uint8_t data2) override;
  void reset() override;
  // Mixes `frames` stereo frames (interleaved left, right) into `out`, replacing what it held.
  void render(int16_t* out, uint32_t frames);

  uint32_t active_voices() const;

 private:
  struct Channel {
    uint8_t program = 0, volume = 100, expression = 127, pan = 64, modulation = 0;
    uint16_t bend = 8192;
    float bend_range = 2.0f;   // semitones
    bool sustain = false;
    uint8_t rpn_msb = 127, rpn_lsb = 127;
  };
  enum class Stage : uint8_t { Free, Attack, Decay, Sustain, Release };
  struct Voice {
    Stage stage = Stage::Free;
    uint8_t channel = 0, note = 0, velocity = 0;
    bool held_by_pedal = false;   // the key went up while the pedal was down
    bool drum = false;
    uint32_t order = 0;           // when it started (to steal the oldest)
    // pitched voices
    const float* table = nullptr;
    uint32_t phase = 0;
    float base_hz = 0;
    // drums
    float tone_hz = 0, tone_end_hz = 0, tone_gain = 0, noise_gain = 0, highpass = 0, tone_phase = 0, noise_state = 0, noise_last = 0;
    uint32_t noise = 1;
    // envelope
    float level = 0, attack_step = 0, decay_coefficient = 0, sustain_level = 1, release_coefficient = 0;
    float gain = 0;               // velocity gain
  };
  struct Instrument {
    uint8_t timbre;
    uint16_t attack_ms, decay_ms, release_ms;
    uint8_t sustain;   // 0..100 (percent)
  };

  void note_on(uint8_t channel, uint8_t note, uint8_t velocity);
  void note_off(uint8_t channel, uint8_t note);
  void release_voice(Voice& v);
  void control(uint8_t channel, uint8_t controller, uint8_t value);
  void start_drum(Voice& v, uint8_t note);
  Voice* allocate_voice(uint8_t channel, uint8_t note);
  void render_block(float* left, float* right, uint32_t frames);
  void build_tables();
  const float* table_for(uint8_t timbre, float hz) const;
  static Instrument instrument(uint8_t program);
  float pitch_factor(const Channel& c) const;

  allocators::IAllocator& allocator_;
  mutable kernel::Mutex mutex_;
  float* tables_ = nullptr;       // kTimbres x kLevels x (kTableSize + 1)
  float note_hz_[128];
  Channel channels_[16];
  Voice voices_[kMaxVoices];
  uint32_t counter_ = 0;
  float lfo_phase_ = 0;
};

}  // namespace midi
