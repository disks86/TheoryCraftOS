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

// NtTcosAudio(PULONG64 arguments): this OS's own service (510), behind winmm.dll's wave output. The work is the kernel's PipeWire graph
// (src/PipeWire/WindowsAudioService.hpp), installed at start-up with set_audio_handler so that the process layer does not depend on it. Each stream a
// Windows program opens is a node of the graph, mixed with the native and Linux programs' sound. The arguments are 8 ULONG64 in the program's memory:
//
//   [0] operation  [1] stream  [2..5] parameters  [6], [7] results (written back)
//
//   1 OPEN        [2] sample format (1 PCM, 3 IEEE float) | bits per sample << 16, [3] samples per second, [4] channels, [5] the program's name (UTF-8, may be
//                 0), [6] its length (at most 255); [7] = the stream.
//   2 CLOSE       [1], [2] 1: let what is queued play out first.
//   3 WRITE       [1], [2] data, [3] bytes (at most 1 MiB); [7] = the bytes taken (whole frames; 0 when the stream is full). Never blocks.
//   4 STATUS      [1]; [5] = bytes queued, [6] = bytes written, [7] = bytes played (in the stream's format).
//   5 FLUSH       [1]: drops what is queued.
//   6 PAUSE       [1], [2] 1 paused, 0 playing.
//   7 SET_VOLUME  [1] (0: the master volume), [2] left, [3] right (0 .. 0xFFFF each).
//   8 GET_VOLUME  [1] (0: master); [7] = left | right << 16.
//   9 DEVICES     [7] = the number of output devices (0 or 1: the mixer's).
// STATUS_INVALID_HANDLE for a stream the process does not have, STATUS_INVALID_PARAMETER for a format the graph cannot take, STATUS_DEVICE_NOT_CONNECTED
// when there is no audio device, STATUS_NOT_SUPPORTED without the graph.
#pragma once
#include <stdint.h>
#include "Process/Process.hpp"
#include "Process/Syscall.hpp"

namespace process::windowsabi {

using AudioHandler = uint32_t (*)(Process& p, const SyscallRequest& r);
void set_audio_handler(AudioHandler handler);
uint32_t nt_tcos_audio(Process& p, const SyscallRequest& r);

}  // namespace process::windowsabi
