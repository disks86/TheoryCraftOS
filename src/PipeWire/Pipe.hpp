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


// PulseAudio's module-pipe-sink and module-pipe-source: a virtual sink whose audio (its monitor) is written as raw PCM into a named pipe, and a virtual
// source (Audio/Source/Virtual) fed with the raw PCM a program writes into one. The pipe is opened as PulseAudio opens it, for reading and writing at
// once (O_RDWR), so the module never waits for a partner: a program can `cat` the sink's pipe or write into the source's whenever it likes, and comes and
// goes without the module noticing.
//   - PipeSink records the sink's monitor in the module's format and writes it into the pipe in pieces of at most IPipeFiles::kAtomicBytes whole frames
//     (each goes in whole or not at all, so a reader never sees a frame cut in two). What does not fit (nobody reads, or not fast enough) is dropped,
//     as PulseAudio's sink does when its pipe is full; what the pipe already holds stays (a reader that starts late gets up to the pipe's size of older
//     audio first). As with every virtual sink here, its monitor delivers nothing while nothing plays into it: then nothing is written.
//   - PipeSource reads the pipe and plays it into the source, keeping about `latency` queued there: past that it stops reading, and the writer waits in
//     the pipe (the source's clock paces it). Mono is played as stereo (the mixer would pan it 3 dB down on each side).
// The pipes themselves are behind IPipeFiles: the kernel's Linux FIFOs (src/Process/Fifo.hpp, KernelPipeFiles.hpp), the host's in tests/pipewire, a fake
// in PipeWireTest. Not thread safe: pumped by PulseServer::tick.
#pragma once
#include <stdint.h>
#include "PipeWire/Graph.hpp"
#include "Utility/ByteBuffer.hpp"
#include "Utility/UniquePointer.hpp"

namespace pipewire {

// One open pipe. Neither call blocks. (Owned through UniquePointer::make_as, which destroys the object as what it was made: no virtual destructor, so
// the kernel needs no operator delete.)
class IPipeFile {
 public:
  // As much of `data` as there is room for, all of it or nothing for up to IPipeFiles::kAtomicBytes; 0 when it is full.
  virtual uint32_t write(ConstByteView data) = 0;
  // What is there, up to out.size() bytes; 0 when nothing is.
  virtual uint32_t read(ByteView out) = 0;

 protected:
  constexpr IPipeFile() = default;
  ~IPipeFile() = default;
};

class IPipeFiles {
 public:
  static constexpr uint32_t kAtomicBytes = 4096;      // (PIPE_BUF)
  // The named pipe at `path`, made when it is missing (as a Linux program names it: /tmp/music.output), opened for reading and writing. Null when it
  // cannot be (the folder is not there, the name is something else).
  virtual UniquePointer<IPipeFile> open(StringView path) = 0;

 protected:
  constexpr IPipeFiles() = default;
  ~IPipeFiles() = default;
};

// PulseAudio's names of the sample formats ("s16le", "float32le", "ulaw", ...; the native-endian ones as little-endian); false for others.
[[nodiscard]] bool pulse_format_named(StringView name, SampleFormat* out);

class PipeSink {
 public:
  PipeSink(Graph& graph, allocators::IAllocator& allocator) : graph_(graph), pending_(allocator) {}
  ~PipeSink() { close(); }
  PipeSink(const PipeSink&) = delete;
  PipeSink& operator=(const PipeSink&) = delete;

  // Records the monitor of `sink` (a virtual sink's node) in `format` for `file`. False when the stream cannot be made.
  [[nodiscard]] bool open(uint32_t client, uint32_t sink, const PcmFormat& format, UniquePointer<IPipeFile> file);
  void close();
  // Moves what was recorded into the pipe. Returns the bytes written.
  uint64_t pump();
  uint64_t written() const { return written_; }
  uint64_t dropped() const { return dropped_; }
  uint32_t record_node() const { return record_node_; }

 private:
  Graph& graph_;
  ByteBuffer pending_;
  UniquePointer<IPipeFile> file_;
  SharedPointer<RecordStream> record_;
  uint32_t record_node_ = 0, frame_ = 0;
  uint64_t written_ = 0, dropped_ = 0;
};

class PipeSource {
 public:
  static constexpr uint32_t kDefaultLatencyMs = 100;

  PipeSource(Graph& graph, allocators::IAllocator& allocator) : graph_(graph), scratch_(allocator) {}
  ~PipeSource() { close(); }
  PipeSource(const PipeSource&) = delete;
  PipeSource& operator=(const PipeSource&) = delete;

  // Plays what `file` brings, in `format`, into `source` (a virtual source's node). False when the stream cannot be made.
  [[nodiscard]] bool open(uint32_t client, uint32_t source, const PcmFormat& format, UniquePointer<IPipeFile> file, uint32_t latency_ms = kDefaultLatencyMs);
  void close();
  // Moves what the pipe has into the source, up to the latency. Returns the bytes read from the pipe.
  uint64_t pump();
  uint64_t read_bytes() const { return read_; }
  uint32_t play_node() const { return play_node_; }

 private:
  Graph& graph_;
  ByteBuffer scratch_;
  UniquePointer<IPipeFile> file_;
  SharedPointer<PlaybackStream> play_;
  PcmFormat format_;
  uint32_t play_node_ = 0;
  uint32_t carry_ = 0;      // (bytes of a frame cut short at the end of the last read, kept at the start of scratch_)
  uint64_t latency_bytes_ = 0, read_ = 0;
};

}  // namespace pipewire
