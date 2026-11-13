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

#include "PipeWire/StreamRestore.hpp"
#include "FileSystem/SystemLayout.hpp"
#include "PipeWire/PulseServer.hpp"
#include "Utility/ByteStream.hpp"
#include "Utility/UniquePointer.hpp"

namespace pipewire {

namespace {
constexpr uint64_t kMaxFile = 1u << 20;
constexpr uint8_t kMagic[4] = {'T', 'C', 'S', 'R'};
constexpr uint8_t kPositionMono = 0, kPositionLeft = 1, kPositionRight = 2;      // (PulseAudio's channel positions)

// PipeWire's media roles as PulseAudio names them (module-stream-restore's map); others lower-cased as they are.
const char* const kRoles[][2] = {{"Movie", "video"}, {"Music", "music"}, {"Game", "game"}, {"Notification", "event"}, {"Communication", "phone"},
                                 {"Production", "production"}, {"Accessibility", "a11y"}, {"Test", "test"}};
}  // namespace

// ---- the file ---------------------------------------------------------------------------------------------------------------------------------------

bool FileStreamRestore::load(ByteBuffer& out) {
  UniquePointer<filesystem::IFile> file;
  if (file_system_.open_file(StringView(filesystem::layout::kStreamRestore), filesystem::OpenMode::Read, &file) != filesystem::Status::Ok || !file) return false;
  const uint64_t size = file->size();
  if (size == 0 || size > kMaxFile) return false;
  out.clear();
  if (!out.put_zeros(size)) return false;
  uint64_t got = 0;
  if (file->read(0, out.writable_view(), &got) != filesystem::Status::Ok || got != size) {
    out.clear();
    return false;
  }
  return true;
}

bool FileStreamRestore::save(ConstByteView bytes) {
  const char* const folders[] = {filesystem::layout::kAppStore, filesystem::layout::kPipeWireFolder};
  for (const char* folder : folders) {
    const filesystem::Status s = file_system_.create_directory(StringView(folder));
    if (s != filesystem::Status::Ok && s != filesystem::Status::AlreadyExists) return false;
  }
  UniquePointer<filesystem::IFile> file;
  if (file_system_.open_file(StringView(filesystem::layout::kStreamRestore), filesystem::OpenMode::CreateTruncate, &file) != filesystem::Status::Ok || !file)
    return false;
  uint64_t put = 0;
  if (file->write(0, bytes, &put) != filesystem::Status::Ok || put != bytes.size() || file->flush() != filesystem::Status::Ok) return false;
  return file_system_.flush() == filesystem::Status::Ok;
}

// ---- the entries ------------------------------------------------------------------------------------------------------------------------------------

StreamRestore::StreamRestore(Graph& graph, allocators::IAllocator& allocator, IStreamRestoreStorage* storage)
    : graph_(graph), allocator_(allocator), storage_(storage), entries_(allocator), seen_(allocator) {}

bool StreamRestore::key_of(const Properties& props, bool record, String* out) {
  out->clear();
  out->append(record ? "source-output-by-" : "sink-input-by-");
  const StringView role = props.get("media.role");
  if (!role.empty()) {
    out->append("media-role:");
    for (const auto& pair : kRoles)
      if (role == StringView(pair[0])) return out->append(pair[1]);
    for (char ch : role) {
      const char lower = ch >= 'A' && ch <= 'Z' ? static_cast<char>(ch - 'A' + 'a') : ch;
      if (!out->append(StringView(&lower, 1))) return false;
    }
    return true;
  }
  const char* const kinds[][2] = {{"application.id", "application-id:"}, {"application.name", "application-name:"}, {"media.name", "media-name:"}};
  for (const auto& kind : kinds) {
    const StringView value = props.get(kind[0]);
    if (value.empty()) continue;
    return out->append(kind[1]) && out->append(value) && out->size() <= kMaxNameBytes;
  }
  out->clear();
  return false;
}

RestoreEntry* StreamRestore::find_locked(StringView name) {
  for (UniquePointer<RestoreEntry>& e : entries_.view())
    if (e->name.view() == name) return e.get();
  return nullptr;
}

const RestoreEntry* StreamRestore::find(StringView name) const {
  for (const UniquePointer<RestoreEntry>& e : entries_.view())
    if (e->name.view() == name) return e.get();
  return nullptr;
}

void StreamRestore::changed(uint64_t now_ns) {
  ++changes_;
  if (!dirty_) dirty_since_ = now_ns;
  dirty_ = true;
  dirty_since_ = now_ns;      // (a second after the last change)
}

bool StreamRestore::store(StringView key, uint8_t channels, float left, float right, bool mute) {
  RestoreEntry* e = find_locked(key);
  if (!e) {
    if (entries_.count() >= kMaxEntries) return false;
    UniquePointer<RestoreEntry> made = UniquePointer<RestoreEntry>::make(allocator_, allocator_);
    if (!made || !made->name.assign(key) || !entries_.push_back(util::move(made))) return false;
    e = entries_[entries_.count() - 1].get();
  }
  if (channels == 1) {
    e->channels = 1;
    e->positions[0] = kPositionMono;
    e->volumes[0] = linear_to_pulse_volume(left > right ? left : right);
  } else {
    e->channels = 2;
    e->positions[0] = kPositionLeft;
    e->positions[1] = kPositionRight;
    e->volumes[0] = linear_to_pulse_volume(left);
    e->volumes[1] = linear_to_pulse_volume(right);
  }
  e->mute = mute;
  return true;
}

void StreamRestore::apply(uint32_t node, const RestoreEntry& e) {
  float volume = 1.0f;
  bool mute = false;
  graph_.get_volume(node, &volume, &mute);
  float left = volume, right = volume;
  if (e.channels == 1) {
    left = right = pulse_volume_to_linear(e.volumes[0]);
  } else if (e.channels >= 2) {
    // (the entry's left and right positions; a wider map's front pair, else its first two)
    int l = -1, r = -1;
    for (uint32_t i = 0; i < e.channels && i < RestoreEntry::kMaxChannels; ++i) {
      if (e.positions[i] == kPositionLeft && l < 0) l = static_cast<int>(i);
      if (e.positions[i] == kPositionRight && r < 0) r = static_cast<int>(i);
    }
    left = pulse_volume_to_linear(e.volumes[l >= 0 ? l : 0]);
    right = pulse_volume_to_linear(e.volumes[r >= 0 ? r : 1]);
  }
  graph_.set_balance(node, left, right, e.mute);
}

bool StreamRestore::tick(uint64_t now_ns) {
  now_ = now_ns;
  // the streams there are now (copied out: the graph is not called while it is locked)
  struct Now {
    uint32_t node;
    uint64_t serial;
    bool record;
    uint8_t channels;
    float left, right;
    bool mute;
    String key;
  };
  List<UniquePointer<Now>> now(allocator_);
  graph_.for_each([&](const GraphObject& o) {
    if (o.type != ObjectType::Node || (o.role != NodeRole::Playback && o.role != NodeRole::Capture)) return;
    if (o.props.get("state.restore") == StringView("false")) return;      // (a module's own streams)
    UniquePointer<Now> n = UniquePointer<Now>::make(allocator_, Now{o.id, o.object_serial, o.role == NodeRole::Capture, uint8_t(o.format.channels == 1 ? 1 : 2),
                                                                   o.volume * o.balance_left, o.volume * o.balance_right, o.mute, String(allocator_)});
    if (!n || !key_of(o.props, o.role == NodeRole::Capture, &n->key)) return;
    now.push_back(util::move(n));
  });
  bool any = false;
  // the ones that went are forgotten
  for (uint64_t k = seen_.count(); k > 0; --k) {
    bool still = false;
    for (const UniquePointer<Now>& n : now.view()) still |= n->node == seen_[k - 1].node && n->serial == seen_[k - 1].serial;
    if (!still) seen_.remove_at(k - 1);
  }
  for (const UniquePointer<Now>& n : now.view()) {
    Seen* seen = nullptr;
    for (Seen& s : seen_.view())
      if (s.node == n->node && s.serial == n->serial) seen = &s;
    if (!seen) {
      // a new stream: its remembered volume, if it has one
      Seen s;
      s.node = n->node;
      s.serial = n->serial;
      s.left = n->left;
      s.right = n->right;
      s.mute = n->mute;
      if (const RestoreEntry* e = find(n->key.view())) {
        apply(n->node, *e);
        float volume = 1.0f;
        bool mute = false;
        graph_.get_volume(n->node, &volume, &mute);
        float left = volume, right = volume;
        graph_.get_balance(n->node, &left, &right, &mute);
        s.left = left;
        s.right = right;
        s.mute = mute;
      }
      seen_.push_back(s);
      continue;
    }
    // a stream whose volume or mute changed: remembered (a difference under a PulseAudio volume step is the rounding of the last apply)
    auto differs = [](float a, float b) { return (a > b ? a - b : b - a) > 0.0005f; };
    if (differs(n->left, seen->left) || differs(n->right, seen->right) || n->mute != seen->mute) {
      seen->left = n->left;
      seen->right = n->right;
      seen->mute = n->mute;
      if (store(n->key.view(), n->channels, n->left, n->right, n->mute)) {
        changed(now_ns);
        any = true;
      }
    }
  }
  if (dirty_ && now_ns - dirty_since_ >= kSaveDelayNs) flush();
  return any;
}

void StreamRestore::flush() {
  if (!dirty_) return;
  dirty_ = false;
  if (!storage_) return;
  ByteBuffer bytes(allocator_);
  if (serialise(bytes)) storage_->save(bytes.view());
}

void StreamRestore::load() {
  if (!storage_) return;
  ByteBuffer bytes(allocator_);
  if (storage_->load(bytes) && !deserialise(bytes.view())) entries_.clear();
}

bool StreamRestore::write(Mode mode, ArrayView<const RestoreEntry* const> entries, bool apply_now) {
  for (const RestoreEntry* e : entries)
    if (!e || e->name.empty() || e->name.size() > kMaxNameBytes || e->channels > RestoreEntry::kMaxChannels) return false;
  if (mode == Mode::Set) entries_.clear();
  for (const RestoreEntry* e : entries) {
    RestoreEntry* to = find_locked(e->name.view());
    if (!to) {
      if (mode == Mode::Replace) continue;      // (replace: only the ones there are)
      if (entries_.count() >= kMaxEntries) return false;
      UniquePointer<RestoreEntry> made = UniquePointer<RestoreEntry>::make(allocator_, allocator_);
      if (!made || !made->name.assign(e->name.view()) || !entries_.push_back(util::move(made))) return false;
      to = entries_[entries_.count() - 1].get();
    }
    to->channels = e->channels;
    for (uint32_t i = 0; i < RestoreEntry::kMaxChannels; ++i) {
      to->positions[i] = e->positions[i];
      to->volumes[i] = e->volumes[i];
    }
    to->mute = e->mute;
    if (!to->device.assign(e->device.view())) return false;
  }
  changed(now_);
  if (apply_now) {
    // the streams there are now of those keys get it (and are not then taken as changed by the program)
    struct Target {
      uint32_t node;
      uint64_t serial;
      String key;
    };
    List<UniquePointer<Target>> targets(allocator_);
    graph_.for_each([&](const GraphObject& o) {
      if (o.type != ObjectType::Node || (o.role != NodeRole::Playback && o.role != NodeRole::Capture)) return;
      UniquePointer<Target> t = UniquePointer<Target>::make(allocator_, Target{o.id, o.object_serial, String(allocator_)});
      if (t && key_of(o.props, o.role == NodeRole::Capture, &t->key)) targets.push_back(util::move(t));
    });
    for (const UniquePointer<Target>& t : targets.view()) {
      const RestoreEntry* e = find(t->key.view());
      if (!e) continue;
      apply(t->node, *e);
      for (Seen& s : seen_.view()) {
        if (s.node != t->node || s.serial != t->serial) continue;
        float left = 1.0f, right = 1.0f;
        bool mute = false;
        graph_.get_balance(t->node, &left, &right, &mute);
        s.left = left;
        s.right = right;
        s.mute = mute;
      }
    }
  }
  return true;
}

void StreamRestore::remove(StringView name) {
  for (uint64_t k = 0; k < entries_.count(); ++k)
    if (entries_[k]->name.view() == name) {
      entries_.remove_at(k);
      changed(now_);
      return;
    }
}

// ---- the file's format: "TCSR", version, count, then each entry: name (u16 length + bytes), channels (u8), each channel's position (u8) and volume
// (u32), mute (u8), device (u16 length + bytes); little-endian ----------------------------------------------------------------------------------------

bool StreamRestore::serialise(ByteBuffer& out) const {
  out.clear();
  bool ok = out.append(ConstByteView(kMagic, 4)) && out.put32(kFormatVersion) && out.put32(static_cast<uint32_t>(entries_.count()));
  for (const UniquePointer<RestoreEntry>& e : entries_.view()) {
    ok = ok && out.put16(static_cast<uint32_t>(e->name.size())) && out.append(e->name.view().bytes()) && out.put8(e->channels);
    for (uint32_t i = 0; i < e->channels && ok; ++i) ok = out.put8(e->positions[i]) && out.put32(e->volumes[i]);
    ok = ok && out.put8(e->mute ? 1 : 0) && out.put16(static_cast<uint32_t>(e->device.size())) && out.append(e->device.view().bytes());
  }
  return ok;
}

bool StreamRestore::deserialise(ConstByteView bytes) {
  ByteReader r(bytes);
  const ConstByteView magic = r.take(4);
  if (!r.ok() || magic.size() != 4 || magic[0] != kMagic[0] || magic[1] != kMagic[1] || magic[2] != kMagic[2] || magic[3] != kMagic[3]) return false;
  if (r.le32() != kFormatVersion) return false;
  const uint32_t count = r.le32();
  if (!r.ok() || count > kMaxEntries) return false;
  List<UniquePointer<RestoreEntry>> read(allocator_);
  for (uint32_t k = 0; k < count; ++k) {
    UniquePointer<RestoreEntry> e = UniquePointer<RestoreEntry>::make(allocator_, allocator_);
    if (!e) return false;
    const uint16_t name_length = r.le16();
    if (!r.ok() || name_length == 0 || name_length > kMaxNameBytes) return false;
    const ConstByteView name = r.take(name_length);
    if (!r.ok() || name.size() != name_length || !e->name.assign(StringView(name))) return false;
    e->channels = r.u8();
    if (!r.ok() || e->channels > RestoreEntry::kMaxChannels) return false;
    for (uint32_t i = 0; i < e->channels; ++i) {
      e->positions[i] = r.u8();
      e->volumes[i] = r.le32();
    }
    const uint8_t mute = r.u8();
    const uint16_t device_length = r.le16();
    if (!r.ok() || mute > 1 || device_length > kMaxNameBytes) return false;
    e->mute = mute == 1;
    const ConstByteView device = r.take(device_length);
    if (!r.ok() || device.size() != device_length || !e->device.assign(StringView(device))) return false;
    for (const UniquePointer<RestoreEntry>& other : read.view())
      if (other->name.view() == e->name.view()) return false;      // (one entry a name)
    if (!read.push_back(util::move(e))) return false;
  }
  if (!r.at_end()) return false;
  entries_.clear();
  for (UniquePointer<RestoreEntry>& e : read.view()) entries_.push_back(util::move(e));
  return true;
}

}  // namespace pipewire
