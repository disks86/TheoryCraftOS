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

// Remembered stream volumes, as PulseAudio's module-stream-restore keeps them (and WirePlumber does for PipeWire): a stream whose volume or mute is
// changed (by pavucontrol, pactl, the program itself, any protocol) has it remembered under its key, and the next stream of the same key starts with it.
// The key is PulseAudio's: "sink-input-by-media-role:music" when the stream says its role, else "-by-application-id:", "-by-application-name:" or
// "-by-media-name:" ("source-output-..." for a record stream). The entries are kept in a file of the PipeWire application's folder
// (filesystem::layout::kStreamRestore) through IStreamRestoreStorage, written a second after the last change at most.
//
// PulseServer serves it as module-stream-restore: the stream-restore extension (TEST, READ, WRITE, DELETE, SUBSCRIBE, EVENT) that pavucontrol and
// libpulse's pa_ext_stream_restore_* use. The device an entry names is kept and listed but streams are not moved to it (the graph has one sink and one
// source; a virtual one is a stream's own choice). Not thread safe: driven by its owner's thread (PulseServer::tick).
#pragma once
#include <stdint.h>
#include "FileSystem/IFileSystem.hpp"
#include "ObjectContainer/List.hpp"
#include "PipeWire/Graph.hpp"
#include "Utility/ByteBuffer.hpp"
#include "Utility/String.hpp"

namespace pipewire {

// Where the entries are kept between boots.
class IStreamRestoreStorage {
 public:
  // Replaces `out` with what is stored; false if there is nothing.
  virtual bool load(ByteBuffer& out) = 0;
  virtual bool save(ConstByteView bytes) = 0;

 protected:
  constexpr IStreamRestoreStorage() = default;
  ~IStreamRestoreStorage() = default;
};

// The file in the AppStore.
class FileStreamRestore final : public IStreamRestoreStorage {
 public:
  explicit FileStreamRestore(filesystem::IFileSystem& file_system) : file_system_(file_system) {}
  bool load(ByteBuffer& out) override;
  bool save(ConstByteView bytes) override;

 private:
  filesystem::IFileSystem& file_system_;
};

struct RestoreEntry {
  explicit RestoreEntry(allocators::IAllocator& a) : name(a), device(a) {}
  static constexpr uint32_t kMaxChannels = 32;      // (PulseAudio's PA_CHANNELS_MAX)
  String name;                       // "sink-input-by-application-name:Firefox"
  uint8_t channels = 0;              // 0: no volume remembered
  uint8_t positions[kMaxChannels] = {};      // (PulseAudio channel positions)
  uint32_t volumes[kMaxChannels] = {};       // (PulseAudio volumes: 0x10000 is 100%)
  bool mute = false;
  String device;                     // (may be empty)
};

class StreamRestore {
 public:
  static constexpr uint32_t kMaxEntries = 1024;
  static constexpr uint32_t kMaxNameBytes = 1024;
  static constexpr uint64_t kSaveDelayNs = 1000000000ull;      // (written this long after the last change)
  static constexpr uint32_t kFormatVersion = 1;

  StreamRestore(Graph& graph, allocators::IAllocator& allocator, IStreamRestoreStorage* storage);
  StreamRestore(const StreamRestore&) = delete;
  StreamRestore& operator=(const StreamRestore&) = delete;

  // What the storage holds (a broken or truncated file is taken as empty).
  void load();
  // The streams that came since the last call get their remembered volume; the ones whose volume or mute changed have it remembered. `now_ns`:
  // the graph's clock (for the delayed save). Returns true when an entry changed (the extension's subscribers get an EVENT).
  bool tick(uint64_t now_ns);
  // Writes what changed now (the module is going, the system shuts down).
  void flush();

  // The extension's operations. WRITE's modes (PulseAudio's pa_update_mode_t): 0 set (the entries become exactly these), 1 merge (added or
  // replaced), 2 replace (only the ones there are replaced). With `apply`, streams of those keys there now get the volume.
  enum class Mode : uint32_t { Set = 0, Merge = 1, Replace = 2 };
  [[nodiscard]] bool write(Mode mode, ArrayView<const RestoreEntry* const> entries, bool apply);
  void remove(StringView name);
  const List<UniquePointer<RestoreEntry>>& entries() const { return entries_; }
  const RestoreEntry* find(StringView name) const;
  uint64_t changes() const { return changes_; }

  // The serialised entries, and back (false and nothing changed when the bytes are not a whole, valid file of this format).
  [[nodiscard]] bool serialise(ByteBuffer& out) const;
  [[nodiscard]] bool deserialise(ConstByteView bytes);
  // The key a stream of these properties has (empty when it has nothing to be known by). `record`: a record stream.
  static bool key_of(const Properties& props, bool record, String* out);

 private:
  struct Seen {
    uint32_t node = 0;
    uint64_t serial = 0;      // (object.serial: an id may be used again)
    float left = 1.0f, right = 1.0f;
    bool mute = false;
  };
  RestoreEntry* find_locked(StringView name);
  bool store(StringView key, uint8_t channels, float left, float right, bool mute);
  void apply(uint32_t node, const RestoreEntry& e);
  void changed(uint64_t now_ns);

  Graph& graph_;
  allocators::IAllocator& allocator_;
  IStreamRestoreStorage* storage_;
  List<UniquePointer<RestoreEntry>> entries_;
  List<Seen> seen_;
  uint64_t changes_ = 0, dirty_since_ = 0;
  bool dirty_ = false;
  uint64_t now_ = 0;
};

}  // namespace pipewire
