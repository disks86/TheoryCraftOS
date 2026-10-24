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

// What a <video> or <audio> plays when its address is the one made from a MediaSource (Media Source Extensions). The player of the machine plays files, so
// the clip keeps one open per SourceBuffer on a snapshot of what that buffer holds around the playhead (MseBuffer::snapshot()) and swaps in a fresh
// snapshot when the playhead leaves the old one, when more data was appended to the end of it while the playhead is near that end, or when the data the
// playhead is going to meet was removed or replaced. Between swaps it is the players that play. Playing carries on while there is data; when the
// playhead meets the end of the data the clip stays "playing" but waits (readyState drops, "waiting" is fired by the page) until an append goes on.
#pragma once
#include "Browser/Objects.hpp"

namespace browser {

class MseClip final : public IVideoClip {
 public:
  MseClip(allocators::IAllocator& allocator, media::MseHub& hub, uint32_t source, IObjectEnvironment* env);
  ~MseClip();
  MseClip(const MseClip&) = delete;
  MseClip& operator=(const MseClip&) = delete;

  void set_environment(IObjectEnvironment* env) { env_ = env; }
  uint32_t source() const { return source_; }

  bool play() override;
  void pause() override;
  void seek(int64_t microseconds) override;
  void set_looping(bool looping) override { (void)looping; }
  void set_volume(double volume, bool muted) override;
  bool set_rate(double rate) override;
  bool playing() override { return playing_; }
  int64_t position_us() override;
  int64_t duration_us() override;
  uint32_t width() override;
  uint32_t height() override;
  void update() override;
  bool copy_picture(ArrayView<uint32_t> dest, uint32_t pitch, uint32_t* serial) override;

  bool dynamic() override { return true; }
  uint8_t ready_state() override;
  uint32_t buffered(ArrayView<double> pairs) override;
  uint32_t seekable(ArrayView<double> pairs) override;
  uint8_t error() override { return error_; }
  bool waiting_for_key() override { return need_key_; }
  // The keys to decrypt with (null: none; encrypted content then waits).
  void set_keys(const media::IMseKeys* keys) { keys_ = keys; }

 private:
  struct Child {
    UniquePointer<IVideoClip> clip;
    uint32_t buffer = 0;
    int64_t base = 0, start = 0, end = 0;   // element time = file time + base; the range (element time) the file covers
    uint32_t generation = 0;
    uint64_t epoch = 0, appended = 0, key_epoch = 0;
    bool video = false;
    bool started = false;   // play() was called on it (so it stopping means it ran to the end of its file)
  };
  static constexpr uint32_t kMaxChildren = media::MseHub::kMaxBuffersPerSource;

  Child* child_of(uint32_t buffer);
  void drop(Child* c);
  void drop_all();
  bool open_child(media::MseBuffer& buffer, const media::MseSnapshotPlan& plan, Child* into);
  Child* master();
  int64_t limit_of(const Child& c);
  void fail(uint8_t code);
  void sync_children();

  allocators::IAllocator& allocator_;
  media::MseHub& hub_;
  uint32_t source_;
  IObjectEnvironment* env_;
  Child children_[kMaxChildren];
  bool used_[kMaxChildren] = {};
  int64_t position_ = 0;
  bool playing_ = false, stalled_ = false, muted_ = false, need_key_ = false;
  const media::IMseKeys* keys_ = nullptr;
  uint64_t probe_signature_ = 0;
  bool probe_result_ = false, probed_ = false;
  bool blocked_on_key(media::MseSource* source);
  double volume_ = 1, rate_ = 1;
  uint8_t error_ = 0;
  uint32_t picture_serial_ = 0;
};

}  // namespace browser
