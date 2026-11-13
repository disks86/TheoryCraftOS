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

// NtTcosAudio(PULONG64 arguments): this OS's own service (510), behind winmm.dll's wave output and input, dsound.dll (playback and capture) and WASAPI
// (mmdevapi.dll, render and capture). The work is the kernel's PipeWire graph (src/PipeWire/WindowsAudioService.hpp), installed at start-up with
// set_audio_handler so that the process layer does not depend on it. Each stream a Windows program opens is a node of the graph: a playback stream is
// mixed with the native and Linux programs' sound, a capture stream hears the microphone with every other program that records. The arguments are 8
// ULONG64 in the program's memory:
//
//   [0] operation  [1] stream  [2..5] parameters  [4..7] results (written back)
//
//   1 OPEN        [1] 0, or a virtual sink's node to play into (VIRTUAL_SINKS), [2] sample format (1 PCM, 3 IEEE float) | bits per sample << 16,
//                 [3] samples per second, [4] channels, [5] the program's name (UTF-8, may be 0), [6] its length (at most 255); [7] = the stream.
//   2 CLOSE       [1], [2] 1: let what is queued play out first. (Either kind of stream.)
//   3 WRITE       [1], [2] data, [3] bytes (at most 1 MiB); [7] = the bytes taken (whole frames; 0 when the stream is full). Never blocks.
//   4 STATUS      [1]; a playback stream: [4] = bytes the mixer has not taken yet, [5] = bytes not played yet (those and the device's queue), [6] = bytes
//                 written, [7] = bytes played. A capture stream: [4] = bytes waiting to be read, [5] = bytes lost (not read in time), [6] = bytes
//                 recorded, [7] = bytes read. (All in the stream's format.)
//   5 FLUSH       [1]: drops what is queued (playback) or what was recorded and not read (capture).
//   6 PAUSE       [1], [2] 1 paused, 0 playing (or recording: a paused capture stream drops what the microphone hears).
//   7 SET_VOLUME  [1] (0: the master volume, 0xFFFFFFFF: the microphone's master gain), [2] left, [3] right (0 .. 0xFFFF each). A playback stream's
//                 louder side is its volume and the balance becomes its pan; the master volumes and capture streams have no balance (the louder side).
//   8 GET_VOLUME  [1] (0: master, 0xFFFFFFFF: microphone); [6] = 1 when muted, [7] = left | right << 16.
//   9 DEVICES     [6] = the number of input devices (0 or 1: a microphone is there), [7] = the number of output devices (0 or 1: the mixer's).
//  10 SET_MUTE    [1] (0: master, 0xFFFFFFFF: microphone), [2] 1 muted, 0 not. The volume is kept.
//  11 OPEN_CAPTURE  as OPEN, [1] 0: the microphone, 1: the monitor (what the mixer plays, for WASAPI loopback), else a virtual sink's node: its
//                 monitor (what plays into it); [7] = a capture stream that records from now.
//  12 READ        [1], [2] buffer, [3] bytes (at most 1 MiB); [7] = the bytes read (whole frames; 0 when nothing is waiting). Never blocks.
//  13 VIRTUAL_SINKS  [2] which (0, 1, ...), [3] a buffer, [4] its size (at most 255); [5] = that virtual sink's node (0 past the last), [6] = how many
//                 there are, [7] = the length of its description written to [3] (UTF-8). The virtual sinks are the PipeWire graph's null sinks (pw-cli
//                 create-node adapter ...): a program plays into one with OPEN [1] = its node, records it with OPEN_CAPTURE [1] = its node, and
//                 its master volume is the volume calls' stream 0x80000000 | its node.
// STATUS_INVALID_HANDLE for a stream the process does not have, STATUS_INVALID_PARAMETER for a format the graph cannot take, STATUS_DEVICE_NOT_CONNECTED
// when there is no audio device (or, for OPEN_CAPTURE, no capture hub), STATUS_NOT_SUPPORTED without the graph.
#pragma once
#include <stdint.h>
#include "Process/Process.hpp"
#include "Process/Syscall.hpp"

namespace process::windowsabi {

using AudioHandler = uint32_t (*)(Process& p, const SyscallRequest& r);
void set_audio_handler(AudioHandler handler);
uint32_t nt_tcos_audio(Process& p, const SyscallRequest& r);

}  // namespace process::windowsabi
