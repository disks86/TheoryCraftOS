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

// The Windows programs' way into the PipeWire graph: NtTcosAudio (layout in src/Process/WindowsAudio.hpp), which winmm.dll (waveOut, waveIn),
// dsound.dll and mmdevapi.dll (WASAPI) call. Every process that plays or records is a client of the graph ("tcos-winmm"); every stream it opens a
// playback node, mixed with what the native and Linux programs play, or a capture node that hears the microphone along with every other program that
// records. A process's streams stop when it ends (process_exited, from win32k's exit hooks).
#pragma once
#include <stdint.h>
#include "Kernel/Sync.hpp"
#include "ObjectContainer/List.hpp"
#include "PipeWire/Graph.hpp"
#include "Process/Process.hpp"
#include "Process/Syscall.hpp"
#include "Utility/UniquePointer.hpp"

namespace pipewire {

class WindowsAudioService {
 public:
  static constexpr uint32_t kMaxStreamsPerProcess = 64;
  static constexpr uint64_t kMaxWrite = 1u << 20;
  // The stream number that stands for the source (the microphone's master gain) in the volume calls; 0 is the sink.
  static constexpr uint32_t kSourceStream = 0xFFFFFFFFu;
  // A volume call's stream number with this bit is a virtual sink's master volume (the rest: its node).
  static constexpr uint32_t kVirtualSinkStream = 0x80000000u;

  WindowsAudioService(Graph& graph, allocators::IAllocator& allocator) : graph_(graph), allocator_(allocator), owners_(allocator) {}
  ~WindowsAudioService();
  WindowsAudioService(const WindowsAudioService&) = delete;
  WindowsAudioService& operator=(const WindowsAudioService&) = delete;

  // One NtTcosAudio call of `p`.
  uint32_t call(process::Process& p, const process::SyscallRequest& r);
  // The streams of process `pid` stop, its client leaves the graph.
  void process_exited(uint64_t pid);
  // How many streams Windows programs have open (for tests).
  uint32_t stream_count();

  // The operations on a process's streams without a Process (what call() does once it has read the arguments); the tests drive these.
  // `target`: a virtual sink to play into (0: the speakers).
  uint32_t open(uint64_t pid, const PcmFormat& format, StringView name, uint32_t* stream, uint32_t target = 0);
  uint32_t write(uint64_t pid, uint32_t stream, ConstByteView pcm, uint32_t* taken);
  // A record stream: what the microphone hears from now, in `format` (with `monitor`: what the mixer plays, WASAPI's loopback capture).
  // `target`: a virtual sink whose monitor to record (what plays into it), or a virtual source to record.
  uint32_t open_capture(uint64_t pid, const PcmFormat& format, StringView name, uint32_t* stream, bool monitor = false, uint32_t target = 0);
  uint32_t read(uint64_t pid, uint32_t stream, ByteView out, uint32_t* got);
  uint32_t close(uint64_t pid, uint32_t stream, bool drain);
  // Stream 0 is the master volume (the sink), kSourceStream the microphone's master gain (the source). Left and right are 0 .. 0xFFFF; a stream's louder side is its volume, the balance its pan.
  uint32_t set_volume(uint64_t pid, uint32_t stream, uint32_t left, uint32_t right);
  uint32_t get_volume(uint64_t pid, uint32_t stream, uint32_t* left, uint32_t* right, bool* mute);
  uint32_t set_mute(uint64_t pid, uint32_t stream, bool mute);

 private:
  struct Owner {
    explicit Owner(allocators::IAllocator& a) : nodes(a) {}
    uint64_t pid = 0;
    uint32_t client = 0;
    List<uint32_t> nodes;
  };
  Owner* owner_locked(uint64_t pid, bool create);
  bool owns_locked(uint64_t pid, uint32_t node);
  // The graph node `stream` stands for (0: the sink), if the process may change its volume.
  uint32_t volume_node(uint64_t pid, uint32_t stream, uint32_t* node);
  uint32_t open_node(uint64_t pid, const PcmFormat& format, StringView name, bool capture, bool monitor, uint32_t* stream, uint32_t target = 0);

  Graph& graph_;
  allocators::IAllocator& allocator_;
  kernel::Mutex mutex_;
  List<UniquePointer<Owner>> owners_;
};

// Makes `service` the one NtTcosAudio reaches.
void install_windows_audio(WindowsAudioService& service);
// Process `pid` ended (for win32k's process exit hooks): the installed service lets go of its streams.
void windows_audio_process_exited(uint64_t pid);

}  // namespace pipewire
