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

#pragma once
#include "Audio/AudioMixer.hpp"
#include "Hardware/AudioDevice/IAudioDevice.hpp"

// Plays a short tone (a 440 Hz triangle wave, a third of a second, at the device's
// current volume) and checks that the device takes all of it, plays it and reports
// it back in its statistics. Logs to the console; returns true if every check passes.
bool run_audio_demo(hardware::IAudioDevice& device);

// Opens two streams on the mixer, plays a different tone on each at the same time
// (a fifth apart) and checks that both were taken whole, mixed and played, and that
// the mixer reports them. Returns true if every check passes.
bool run_mixer_demo(audio::AudioMixer& mixer);
