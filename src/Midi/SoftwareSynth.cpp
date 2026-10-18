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

#include "Midi/SoftwareSynth.hpp"
#include "Math/FloatMath.hpp"

namespace midi {

namespace {
constexpr float kRate = static_cast<float>(SoftwareSynth::kSampleRate);
constexpr uint32_t kBlock = 64;
constexpr float kFloor = 0.0004f;
constexpr uint32_t kLevelHarmonics[SoftwareSynth::kLevels] = {32, 16, 8, 4, 1};

// The strength of harmonic h (1-based) of a timbre.
float harmonic(uint32_t timbre, uint32_t h) {
  const float f = static_cast<float>(h);
  const bool odd = (h & 1) != 0;
  switch (timbre) {
    case 0: return h == 1 ? 1.0f : 0.0f;                                         // sine
    case 1: return (1.0f / f) * (h % 7 == 0 ? 0.25f : 1.0f) * (h <= 14 ? 1.0f : 0.4f);   // piano
    case 2: {                                                                     // organ: a few drawbars
      static const float kBars[11] = {0, 1.0f, 0.7f, 0.5f, 0.4f, 0, 0.25f, 0, 0.2f, 0, 0.1f};
      return h <= 10 ? kBars[h] : 0.0f;
    }
    case 3: return 1.0f / (f * (1.0f + 0.04f * f));                              // strings / saw
    case 4: return h <= 22 ? 1.0f / static_cast<float>(fmath::pow(f, 0.7)) : 0.0f;   // brass
    case 5: return odd ? 1.0f / f : 0.08f / f;                                   // reed
    case 6: return h == 1 ? 1.0f : h == 2 ? 0.3f : h == 3 ? 0.1f : h == 4 ? 0.04f : 0.0f;   // flute
    case 7: return 1.0f / static_cast<float>(fmath::pow(f, 1.4));                // pluck
    case 8: return h == 1 ? 1.0f : h == 2 ? 0.7f : h == 3 ? 0.35f : h == 4 ? 0.15f : h == 5 ? 0.07f : 0.0f;   // bass
    case 9: return odd ? 1.0f / f : 0.0f;                                        // square lead
    default: {                                                                   // bell
      switch (h) {
        case 1: return 1.0f;
        case 3: return 0.55f;
        case 5: return 0.4f;
        case 6: return 0.25f;
        case 9: return 0.15f;
        case 11: return 0.1f;
        case 13: return 0.07f;
        default: return 0.0f;
      }
    }
  }
}

float coefficient_for_ms(float ms) {   // an exponential approach that has covered 98% of the way after `ms`
  if (ms <= 0) return 0;
  const float samples = ms * kRate / 1000.0f;
  return static_cast<float>(fmath::exp(-4.0 / static_cast<double>(samples)));
}

float soft_clip(float x) {
  if (x > 3.0f) return 1.0f;
  if (x < -3.0f) return -1.0f;
  const float x2 = x * x;
  return x * (27.0f + x2) / (27.0f + 9.0f * x2);
}

struct DrumSound {
  float tone_hz, tone_end_hz, tone_gain, noise_gain, highpass, decay_ms;
};

DrumSound drum_sound(uint8_t note) {
  switch (note) {
    case 35: case 36: return {130, 45, 0.95f, 0.05f, 0, 260};
    case 38: case 40: return {200, 170, 0.35f, 0.75f, 0.5f, 190};
    case 37: return {800, 700, 0.5f, 0.3f, 0.5f, 40};
    case 39: return {0, 0, 0, 0.9f, 0.6f, 120};
    case 41: return {90, 60, 0.9f, 0.1f, 0, 320};
    case 43: return {110, 70, 0.9f, 0.1f, 0, 320};
    case 45: return {130, 85, 0.9f, 0.1f, 0, 300};
    case 47: return {155, 100, 0.9f, 0.1f, 0, 280};
    case 48: return {180, 115, 0.9f, 0.1f, 0, 260};
    case 50: return {210, 135, 0.9f, 0.1f, 0, 240};
    case 42: case 44: return {0, 0, 0, 0.6f, 0.9f, 45};
    case 46: return {0, 0, 0, 0.6f, 0.9f, 320};
    case 49: case 57: return {0, 0, 0, 0.7f, 0.85f, 1100};
    case 52: return {0, 0, 0, 0.7f, 0.8f, 700};
    case 55: return {0, 0, 0, 0.7f, 0.8f, 600};
    case 51: case 59: case 53: return {1500, 1500, 0.15f, 0.4f, 0.9f, 600};
    case 54: return {0, 0, 0, 0.5f, 0.95f, 130};
    case 56: return {560, 540, 0.6f, 0.05f, 0, 250};
    case 60: return {400, 380, 0.7f, 0.1f, 0, 90};
    case 61: return {300, 280, 0.7f, 0.1f, 0, 90};
    case 62: return {330, 300, 0.7f, 0.1f, 0, 150};
    case 63: return {250, 230, 0.7f, 0.1f, 0, 150};
    case 64: return {200, 180, 0.7f, 0.1f, 0, 150};
    case 65: return {420, 400, 0.6f, 0.2f, 0, 160};
    case 66: return {320, 300, 0.6f, 0.2f, 0, 160};
    case 67: return {900, 880, 0.5f, 0, 0, 180};
    case 68: return {700, 680, 0.5f, 0, 0, 180};
    case 69: case 70: return {0, 0, 0, 0.5f, 0.95f, 60};
    case 71: return {2200, 2200, 0.4f, 0, 0, 150};
    case 72: return {2200, 2200, 0.4f, 0, 0, 500};
    case 73: return {0, 0, 0, 0.5f, 0.8f, 120};
    case 74: return {0, 0, 0, 0.5f, 0.8f, 300};
    case 75: return {2400, 2400, 0.5f, 0, 0, 50};
    case 76: return {1000, 950, 0.6f, 0.05f, 0, 60};
    case 77: return {800, 760, 0.6f, 0.05f, 0, 60};
    case 78: case 79: return {400, 500, 0.5f, 0.1f, 0, 200};
    case 80: return {3500, 3500, 0.4f, 0, 0, 80};
    case 81: return {3500, 3500, 0.4f, 0, 0, 600};
    default: return {0, 0, 0, 0.5f, 0.8f, 60};
  }
}

}  // namespace

SoftwareSynth::Instrument SoftwareSynth::instrument(uint8_t program) {
  switch (program >> 3) {
    case 0: return {1, 2, 2200, 120, 0};
    case 1: return {10, 2, 1400, 100, 0};
    case 2: return {2, 8, 0, 40, 100};
    case 3: return {7, 2, 1100, 80, 0};
    case 4: return program >= 38 ? Instrument{3, 4, 300, 60, 60} : Instrument{8, 4, 600, 60, 40};
    case 5: return program == 45 ? Instrument{7, 2, 500, 60, 0} : Instrument{3, 90, 0, 220, 100};
    case 6: return {3, 130, 0, 260, 100};
    case 7: return {4, 45, 120, 90, 80};
    case 8: return {5, 35, 80, 70, 85};
    case 9: return {6, 45, 60, 90, 90};
    case 10: return program == 80 ? Instrument{9, 6, 0, 70, 100} : Instrument{3, 6, 0, 70, 100};
    case 11: return {3, 350, 0, 450, 100};
    case 12: return {10, 120, 800, 400, 40};
    case 13: return {7, 3, 900, 80, 0};
    case 14: return {10, 2, 350, 60, 0};
    default: return {0, 200, 600, 500, 20};
  }
}

SoftwareSynth::SoftwareSynth(allocators::IAllocator& allocator) : allocator_(allocator) {
  for (uint32_t n = 0; n < 128; ++n) note_hz_[n] = static_cast<float>(440.0 * fmath::pow(2.0, (static_cast<double>(n) - 69.0) / 12.0));
}

SoftwareSynth::~SoftwareSynth() {
  if (tables_) allocator_.deallocate(tables_);
}

bool SoftwareSynth::initialize() {
  if (tables_) return true;
  const uint64_t count = uint64_t{kTimbres} * kLevels * (kTableSize + 1);
  tables_ = static_cast<float*>(allocator_.allocate(count * sizeof(float)));
  if (!tables_) return false;
  build_tables();
  reset();
  return true;
}

void SoftwareSynth::build_tables() {
  // One sine wave, then every table is a sum of its harmonics.
  static_assert(kTableSize == 2048, "the index arithmetic below assumes 2048");
  float* sine = static_cast<float*>(allocator_.allocate(sizeof(float) * kTableSize));
  if (!sine) {
    allocator_.deallocate(tables_);
    tables_ = nullptr;
    return;
  }
  for (uint32_t i = 0; i < kTableSize; ++i) sine[i] = static_cast<float>(fmath::sin(6.283185307179586 * static_cast<double>(i) / kTableSize));
  for (uint32_t timbre = 0; timbre < kTimbres; ++timbre) {
    for (uint32_t level = 0; level < kLevels; ++level) {
      float* t = tables_ + (uint64_t{timbre} * kLevels + level) * (kTableSize + 1);
      float amps[33];
      for (uint32_t h = 1; h <= 32; ++h) amps[h] = h <= kLevelHarmonics[level] ? harmonic(timbre, h) : 0.0f;
      float peak = 0.0001f;
      for (uint32_t i = 0; i < kTableSize; ++i) {
        float sum = 0;
        for (uint32_t h = 1; h <= 32; ++h)
          if (amps[h] != 0.0f) sum += amps[h] * sine[(i * h) & (kTableSize - 1)];
        t[i] = sum;
        const float a = sum < 0 ? -sum : sum;
        if (a > peak) peak = a;
      }
      const float scale = 0.9f / peak;
      for (uint32_t i = 0; i < kTableSize; ++i) t[i] *= scale;
      t[kTableSize] = t[0];
    }
  }
  allocator_.deallocate(sine);
}

const float* SoftwareSynth::table_for(uint8_t timbre, float hz) const {
  const uint32_t level = hz < 650 ? 0 : hz < 1300 ? 1 : hz < 2600 ? 2 : hz < 5200 ? 3 : 4;
  return tables_ + (uint64_t{timbre} * kLevels + level) * (kTableSize + 1);
}

uint32_t SoftwareSynth::active_voices() const {
  kernel::ScopedMutex lock(mutex_);
  uint32_t n = 0;
  for (const Voice& v : voices_) n += v.stage != Stage::Free;
  return n;
}

void SoftwareSynth::reset() {
  kernel::ScopedMutex lock(mutex_);
  for (Voice& v : voices_) v = Voice();
  for (Channel& c : channels_) c = Channel();
  channels_[9].program = 0;
}

// --- Messages -------------------------------------------------------------------------------------

void SoftwareSynth::send(uint8_t status, uint8_t data1, uint8_t data2) {
  if (!tables_ || status < 0x80 || status >= 0xF0) return;
  kernel::ScopedMutex lock(mutex_);
  const uint8_t channel = status & 0x0F;
  data1 &= 0x7F;
  data2 &= 0x7F;
  switch (status & 0xF0) {
    case 0x80: note_off(channel, data1); break;
    case 0x90:
      if (data2 == 0) note_off(channel, data1);
      else note_on(channel, data1, data2);
      break;
    case 0xB0: control(channel, data1, data2); break;
    case 0xC0: channels_[channel].program = data1; break;
    case 0xE0: channels_[channel].bend = static_cast<uint16_t>(data1 | (data2 << 7)); break;
    default: break;   // key and channel pressure: not used
  }
}

void SoftwareSynth::control(uint8_t channel, uint8_t controller, uint8_t value) {
  Channel& c = channels_[channel];
  switch (controller) {
    case kControllerModulation: c.modulation = value; break;
    case kControllerVolume: c.volume = value; break;
    case kControllerPan: c.pan = value; break;
    case kControllerExpression: c.expression = value; break;
    case kControllerSustain: {
      const bool down = value >= 64;
      c.sustain = down;
      if (!down)
        for (Voice& v : voices_)
          if (v.stage != Stage::Free && v.channel == channel && v.held_by_pedal) {
            v.held_by_pedal = false;
            release_voice(v);
          }
      break;
    }
    case 100: c.rpn_lsb = value; break;
    case 101: c.rpn_msb = value; break;
    case 6:   // data entry: the pitch bend range (in semitones) when RPN 0 is selected
      if (c.rpn_msb == 0 && c.rpn_lsb == 0) c.bend_range = static_cast<float>(value);
      break;
    case 38:  // data entry, cents of the bend range
      if (c.rpn_msb == 0 && c.rpn_lsb == 0) c.bend_range += static_cast<float>(value) / 100.0f;
      break;
    case kControllerAllSoundOff:
      for (Voice& v : voices_)
        if (v.channel == channel) v = Voice();
      break;
    case kControllerAllNotesOff:
      for (Voice& v : voices_)
        if (v.stage != Stage::Free && v.channel == channel && !v.drum) {
          if (c.sustain) v.held_by_pedal = true;
          else release_voice(v);
        }
      break;
    case kControllerResetAll: {
      const uint8_t program = c.program;
      c = Channel();
      c.program = program;
      break;
    }
    default: break;
  }
}

SoftwareSynth::Voice* SoftwareSynth::allocate_voice(uint8_t channel, uint8_t note) {
  Voice* free_voice = nullptr;
  Voice* quietest_release = nullptr;
  Voice* oldest = nullptr;
  for (Voice& v : voices_) {
    if (v.stage == Stage::Free) {
      if (!free_voice) free_voice = &v;
      continue;
    }
    if (v.channel == channel && v.note == note && !v.drum && v.stage != Stage::Release) release_voice(v);   // the same key again: the old note goes
    if (v.stage == Stage::Release && (!quietest_release || v.level < quietest_release->level)) quietest_release = &v;
    if (!oldest || v.order < oldest->order) oldest = &v;
  }
  return free_voice ? free_voice : quietest_release ? quietest_release : oldest;
}

void SoftwareSynth::release_voice(Voice& v) {
  if (v.drum || v.stage == Stage::Free || v.stage == Stage::Release) return;
  v.stage = Stage::Release;
  v.held_by_pedal = false;
}

void SoftwareSynth::note_off(uint8_t channel, uint8_t note) {
  for (Voice& v : voices_) {
    if (v.stage == Stage::Free || v.stage == Stage::Release || v.channel != channel || v.note != note || v.drum) continue;
    if (channels_[channel].sustain) v.held_by_pedal = true;
    else release_voice(v);
  }
}

void SoftwareSynth::start_drum(Voice& v, uint8_t note) {
  const DrumSound d = drum_sound(note);
  v.drum = true;
  v.tone_hz = d.tone_hz;
  v.tone_end_hz = d.tone_end_hz;
  v.tone_gain = d.tone_gain;
  v.noise_gain = d.noise_gain;
  v.highpass = d.highpass;
  v.tone_phase = 0;
  v.noise = 0x9E3779B9u ^ (static_cast<uint32_t>(note) * 2654435761u) ^ counter_ * 40503u;
  v.noise_last = 0;
  v.level = 1.0f;
  v.stage = Stage::Decay;
  v.sustain_level = 0;
  v.decay_coefficient = coefficient_for_ms(d.decay_ms);
}

void SoftwareSynth::note_on(uint8_t channel, uint8_t note, uint8_t velocity) {
  Voice* v = allocate_voice(channel, note);
  if (!v) return;
  *v = Voice();
  v->channel = channel;
  v->note = note;
  v->velocity = velocity;
  v->order = ++counter_;
  const float vel = static_cast<float>(velocity) / 127.0f;
  v->gain = vel * (0.5f + 0.5f * vel);
  if (channel == 9) {
    start_drum(*v, note);
    return;
  }
  const Instrument in = instrument(channels_[channel].program);
  v->base_hz = note_hz_[note];
  v->table = table_for(in.timbre, v->base_hz * 1.12f);
  v->sustain_level = static_cast<float>(in.sustain) / 100.0f;
  v->attack_step = 1.0f / (static_cast<float>(in.attack_ms < 1 ? 1 : in.attack_ms) * kRate / 1000.0f);
  v->decay_coefficient = in.decay_ms ? coefficient_for_ms(in.decay_ms) : 0.0f;
  v->release_coefficient = coefficient_for_ms(in.release_ms);
  v->level = 0;
  v->stage = Stage::Attack;
}

// --- Rendering ------------------------------------------------------------------------------------

float SoftwareSynth::pitch_factor(const Channel& c) const {
  float semitones = (static_cast<float>(c.bend) - 8192.0f) / 8192.0f * c.bend_range;
  if (c.modulation) {   // vibrato: a slow wave up to a third of a semitone deep
    const float phase = lfo_phase_ - static_cast<float>(static_cast<int>(lfo_phase_));
    const float tri = phase < 0.5f ? 4.0f * phase - 1.0f : 3.0f - 4.0f * phase;
    semitones += tri * 0.33f * static_cast<float>(c.modulation) / 127.0f;
  }
  if (semitones == 0.0f) return 1.0f;
  return static_cast<float>(fmath::pow(2.0, static_cast<double>(semitones) / 12.0));
}

void SoftwareSynth::render_block(float* left, float* right, uint32_t frames) {
  float pitch[16], gain_l[16], gain_r[16];
  bool used[16] = {};
  for (const Voice& v : voices_)
    if (v.stage != Stage::Free) used[v.channel] = true;
  for (uint32_t c = 0; c < 16; ++c) {
    if (!used[c]) continue;
    const Channel& ch = channels_[c];
    pitch[c] = pitch_factor(ch);
    float g = (static_cast<float>(ch.volume) / 127.0f) * (static_cast<float>(ch.expression) / 127.0f);
    g *= g;
    float p = (static_cast<float>(ch.pan) - 64.0f) / 64.0f;
    p = p < -1.0f ? -1.0f : p > 1.0f ? 1.0f : p;
    gain_l[c] = g * static_cast<float>(fmath::sqrt(0.5 * (1.0 - static_cast<double>(p))));
    gain_r[c] = g * static_cast<float>(fmath::sqrt(0.5 * (1.0 + static_cast<double>(p))));
  }
  for (Voice& v : voices_) {
    if (v.stage == Stage::Free) continue;
    const float gl = gain_l[v.channel] * v.gain, gr = gain_r[v.channel] * v.gain;
    uint32_t inc = 0;
    if (!v.drum) inc = static_cast<uint32_t>(v.base_hz * pitch[v.channel] * (4294967296.0f / kRate));
    const float decay_floor = v.sustain_level;
    for (uint32_t i = 0; i < frames; ++i) {
      switch (v.stage) {
        case Stage::Attack:
          v.level += v.attack_step;
          if (v.level >= 1.0f) {
            v.level = 1.0f;
            v.stage = v.decay_coefficient > 0 && v.sustain_level < 1.0f ? Stage::Decay : Stage::Sustain;
          }
          break;
        case Stage::Decay:
          v.level = decay_floor + (v.level - decay_floor) * v.decay_coefficient;
          if (v.level - decay_floor < kFloor) {
            if (decay_floor <= kFloor) {
              v = Voice();   // died away
            } else {
              v.level = decay_floor;
              v.stage = Stage::Sustain;
            }
          }
          break;
        case Stage::Release:
          v.level *= v.release_coefficient;
          if (v.level < kFloor) v = Voice();
          break;
        default: break;
      }
      if (v.stage == Stage::Free) break;
      float s;
      if (v.drum) {
        float tone = 0;
        if (v.tone_gain > 0) {
          const float hz = v.tone_end_hz + (v.tone_hz - v.tone_end_hz) * v.level;
          v.tone_phase += hz / kRate;
          v.tone_phase -= static_cast<float>(static_cast<int>(v.tone_phase));
          const float x = v.tone_phase * static_cast<float>(kTableSize);
          const uint32_t idx = static_cast<uint32_t>(x);
          const float* t = tables_;   // timbre 0, level 0: the sine
          tone = (t[idx] + (t[idx + 1] - t[idx]) * (x - static_cast<float>(idx))) * v.tone_gain;
        }
        float noise = 0;
        if (v.noise_gain > 0) {
          v.noise ^= v.noise << 13;
          v.noise ^= v.noise >> 17;
          v.noise ^= v.noise << 5;
          const float white = static_cast<float>(static_cast<int32_t>(v.noise)) * (1.0f / 2147483648.0f);
          noise = (white - v.highpass * v.noise_last) * 0.6f * v.noise_gain;
          v.noise_last = white;
        }
        s = tone + noise;
      } else {
        const uint32_t idx = v.phase >> 21;
        const float frac = static_cast<float>(v.phase & 0x1FFFFFu) * (1.0f / 2097152.0f);
        s = v.table[idx] + (v.table[idx + 1] - v.table[idx]) * frac;
        v.phase += inc;
      }
      s *= v.level;
      left[i] += s * gl;
      right[i] += s * gr;
    }
  }
  lfo_phase_ += 5.5f * static_cast<float>(frames) / kRate;
  if (lfo_phase_ > 1000.0f) lfo_phase_ -= 1000.0f;
}

void SoftwareSynth::render(int16_t* out, uint32_t frames) {
  if (!tables_) {
    for (uint32_t i = 0; i < frames * 2; ++i) out[i] = 0;
    return;
  }
  kernel::ScopedMutex lock(mutex_);
  float left[kBlock], right[kBlock];
  for (uint32_t done = 0; done < frames;) {
    const uint32_t n = frames - done < kBlock ? frames - done : kBlock;
    for (uint32_t i = 0; i < n; ++i) left[i] = right[i] = 0;
    render_block(left, right, n);
    for (uint32_t i = 0; i < n; ++i) {
      out[(done + i) * 2] = static_cast<int16_t>(soft_clip(left[i] * 0.45f) * 30000.0f);
      out[(done + i) * 2 + 1] = static_cast<int16_t>(soft_clip(right[i] * 0.45f) * 30000.0f);
    }
    done += n;
  }
}

}  // namespace midi
