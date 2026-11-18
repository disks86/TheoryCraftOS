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

#include "Hardware/Bus/Thunderbolt/Usb4Policy.hpp"
#include "FileSystem/SystemLayout.hpp"
#include "Utility/UniquePointer.hpp"

namespace hardware {

// ------------------------------------------------------------------ the approval file

namespace {
constexpr uint32_t kMaxFile = FileUsb4ApprovalStore::kMaxApproved * 18;   // (16 hex digits and a newline each, with room)

int hex_digit(uint8_t c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

void copy_text(char* out, uint32_t capacity, const char* text) {
  uint32_t n = 0;
  for (; text && text[n] && n + 1 < capacity; ++n) out[n] = text[n];
  out[n] = 0;
}

// A router's key: its unique id, or (a router that reports none) its route with the top bit set, so that it is still told apart.
uint64_t key_of(const Usb4Router& r) { return r.uid ? r.uid : (r.route | (uint64_t{1} << 63)); }

bool has_adapter(const Usb4Router& r, uint32_t type) {
  for (uint32_t p = 0; p <= r.max_port && p < usb4::kMaxPorts; ++p)
    if (r.ports[p].type == type && !r.ports[p].disabled) return true;
  return false;
}
}  // namespace

uint32_t FileUsb4ApprovalStore::parse(ConstByteView text, uint64_t* ids, uint32_t capacity) {
  uint32_t count = 0;
  uint64_t value = 0;
  uint32_t digits = 0;
  bool bad = false;
  for (uint64_t i = 0; i <= text.size(); ++i) {
    const uint8_t c = i < text.size() ? text[i] : '\n';
    if (c == '\n' || c == '\r') {
      if (digits > 0 && !bad && count < capacity) {
        bool duplicate = false;
        for (uint32_t k = 0; k < count; ++k) duplicate = duplicate || ids[k] == value;
        if (!duplicate) ids[count++] = value;
      }
      value = 0;
      digits = 0;
      bad = false;
      continue;
    }
    const int d = hex_digit(c);
    if (d < 0 || digits == 16) { bad = true; continue; }
    value = (value << 4) | static_cast<uint64_t>(d);
    ++digits;
  }
  return count;
}

uint32_t FileUsb4ApprovalStore::format(const uint64_t* ids, uint32_t count, ByteView out) {
  static const char hex[] = "0123456789abcdef";
  uint32_t at = 0;
  for (uint32_t i = 0; i < count; ++i) {
    if (out.size() - at < 17) break;
    for (int shift = 60; shift >= 0; shift -= 4) out[at++] = static_cast<uint8_t>(hex[(ids[i] >> shift) & 0xF]);
    out[at++] = '\n';
  }
  return at;
}

uint32_t FileUsb4ApprovalStore::load(uint64_t* ids) {
  UniquePointer<filesystem::IFile> file;
  if (file_system_.open_file(StringView(filesystem::layout::kThunderboltApproved), filesystem::OpenMode::Read, &file) != filesystem::Status::Ok || !file) return 0;
  const uint64_t size = file->size() < kMaxFile ? file->size() : kMaxFile;
  uint8_t text[kMaxFile];
  uint64_t got = 0;
  if (file->read(0, ByteView(text).first(size), &got) != filesystem::Status::Ok) return 0;
  return parse(ConstByteView(text).first(got < size ? got : size), ids, kMaxApproved);
}

bool FileUsb4ApprovalStore::save(const uint64_t* ids, uint32_t count) {
  const char* folders[] = {"/AppStore", "/AppStore/Thunderbolt"};
  for (const char* folder : folders) {
    const filesystem::Status s = file_system_.create_directory(StringView(folder));
    if (s != filesystem::Status::Ok && s != filesystem::Status::AlreadyExists) return false;
  }
  uint8_t text[kMaxFile];
  const uint32_t length = format(ids, count, ByteView(text));
  UniquePointer<filesystem::IFile> file;
  if (file_system_.open_file(StringView(filesystem::layout::kThunderboltApproved), filesystem::OpenMode::CreateTruncate, &file) != filesystem::Status::Ok || !file) return false;
  uint64_t put = 0;
  if (length && (file->write(0, ConstByteView(text).first(length), &put) != filesystem::Status::Ok || put != length)) return false;
  return file->flush() == filesystem::Status::Ok && file_system_.flush() == filesystem::Status::Ok;
}

bool FileUsb4ApprovalStore::contains(uint64_t uid) {
  uint64_t ids[kMaxApproved];
  const uint32_t n = load(ids);
  for (uint32_t i = 0; i < n; ++i)
    if (ids[i] == uid) return true;
  return false;
}

bool FileUsb4ApprovalStore::add(uint64_t uid) {
  uint64_t ids[kMaxApproved];
  uint32_t n = load(ids);
  for (uint32_t i = 0; i < n; ++i)
    if (ids[i] == uid) return true;
  if (n == kMaxApproved) {                     // (full: the oldest goes)
    for (uint32_t i = 1; i < n; ++i) ids[i - 1] = ids[i];
    --n;
  }
  ids[n++] = uid;
  return save(ids, n);
}

void FileUsb4ApprovalStore::remove(uint64_t uid) {
  uint64_t ids[kMaxApproved];
  const uint32_t n = load(ids);
  uint32_t kept = 0;
  for (uint32_t i = 0; i < n; ++i)
    if (ids[i] != uid) ids[kept++] = ids[i];
  if (kept != n) (void)save(ids, kept);
}

// ------------------------------------------------------------------ the policy

Usb4Policy::Entry* Usb4Policy::entry_for(uint64_t uid) {
  for (Entry& e : entries_)
    if (e.used && e.info.uid == uid) return &e;
  return nullptr;
}

void Usb4Policy::take_up(uint32_t router_index, const Usb4Router& r, uint32_t* made) {
  Usb4DeviceInfo info;
  info.uid = key_of(r);
  copy_text(info.vendor, sizeof info.vendor, r.vendor_name[0] ? r.vendor_name : "Unknown vendor");
  copy_text(info.name, sizeof info.name, r.device_name[0] ? r.device_name : "USB4 device");
  info.depth = r.depth;
  if (r.parent >= 0)
    if (const Usb4Router* p = domain_.router(static_cast<uint32_t>(r.parent)); p && p->depth > 0) info.parent_uid = key_of(*p);
  info.pcie_capable = has_adapter(r, usb4::kTypePcieUp);
  const char* error = nullptr;
  if (has_adapter(r, usb4::kTypeUsb3Up) && !r.usb3_tunnel && domain_.approve_usb3(router_index, &error)) ++*made;
  info.usb3 = has_adapter(r, usb4::kTypeUsb3Up);
  if (info.pcie_capable) {
    info.remembered = store_ && store_->contains(info.uid);
    if (r.pcie_tunnel) {
      info.state = Usb4DeviceState::Allowed;
    } else if (info.remembered) {
      const bool ok = domain_.approve_pcie(router_index, &error);
      info.state = ok ? Usb4DeviceState::Allowed : Usb4DeviceState::Denied;
      if (ok) ++*made;
    } else {
      info.state = Usb4DeviceState::Waiting;
    }
  }
  kernel::ScopedMutex lock(mutex_);
  for (Entry& e : entries_) {
    if (e.used) continue;
    e.used = true;
    e.seen = true;
    e.info = info;
    __atomic_add_fetch(&changes_, 1, __ATOMIC_ACQ_REL);
    return;
  }
}

void Usb4Policy::carry_out(const Answer& a, uint32_t* made) {
  // The router of that id, if it is still there.
  int index = -1;
  for (uint32_t i = 0; i < domain_.router_count() && index < 0; ++i) {
    const Usb4Router* r = domain_.router(i);
    if (r && r->depth > 0 && key_of(*r) == a.uid) index = static_cast<int>(i);
  }
  if (a.kind == Answer::Forget) {
    if (store_) store_->remove(a.uid);
    kernel::ScopedMutex lock(mutex_);
    if (Entry* e = entry_for(a.uid)) e->info.remembered = false;
    __atomic_add_fetch(&changes_, 1, __ATOMIC_ACQ_REL);
    return;
  }
  Usb4DeviceState state = Usb4DeviceState::Denied;
  bool remembered = false;
  if ((a.kind == Answer::Approve || a.kind == Answer::ApproveAlways) && index >= 0) {
    const Usb4Router* r = domain_.router(static_cast<uint32_t>(index));
    const char* error = nullptr;
    const bool ok = r && (r->pcie_tunnel || domain_.approve_pcie(static_cast<uint32_t>(index), &error));
    if (ok) {
      state = Usb4DeviceState::Allowed;
      ++*made;
      if (a.kind == Answer::ApproveAlways && store_) remembered = store_->add(a.uid);
    }
  }
  kernel::ScopedMutex lock(mutex_);
  if (Entry* e = entry_for(a.uid)) {
    if (e->info.state == Usb4DeviceState::Waiting || a.kind != Answer::Deny) e->info.state = state;
    e->info.remembered = e->info.remembered || remembered;
  }
  __atomic_add_fetch(&changes_, 1, __ATOMIC_ACQ_REL);
}

uint32_t Usb4Policy::step() {
  uint32_t made = 0;
  // The answers that came since the last step.
  Answer answers[kMaxAnswers];
  uint32_t answered = 0;
  {
    kernel::ScopedMutex lock(mutex_);
    for (uint32_t i = 0; i < answer_count_; ++i) answers[answered++] = answers_[i];
    answer_count_ = 0;
    for (Entry& e : entries_) e.seen = false;
  }
  // The routers now: the new ones are taken up, the ones that left are forgotten.
  const uint32_t routers = domain_.router_count();
  uint32_t present = 0;
  for (uint32_t i = 0; i < routers; ++i) {
    const Usb4Router* r = domain_.router(i);
    if (!r || r->depth == 0) continue;               // (the host router)
    ++present;
    const uint64_t uid = key_of(*r);
    bool known = false;
    {
      kernel::ScopedMutex lock(mutex_);
      if (Entry* e = entry_for(uid)) {
        e->seen = true;
        known = true;
        e->info.state = r->pcie_tunnel ? Usb4DeviceState::Allowed : e->info.state;
      }
    }
    if (!known) take_up(i, *r, &made);
  }
  bool left = false;
  {
    kernel::ScopedMutex lock(mutex_);
    for (Entry& e : entries_) {
      if (!e.used || e.seen) continue;
      e = Entry();                                   // (unplugged: asked about again when it comes back)
      left = true;
    }
    if (left) __atomic_add_fetch(&changes_, 1, __ATOMIC_ACQ_REL);
  }
  for (uint32_t i = 0; i < answered; ++i) carry_out(answers[i], &made);
  // Displays: when the topology changed and now and then; the DP adapters' bandwidth requests every step.
  const bool changed = present != last_routers_ || left;
  last_routers_ = present;
  if (changed || ++steps_ % kDisplayScanSteps == 0) made += domain_.dp_connect_displays();
  (void)domain_.dp_poll_requests();
  return made;
}

uint32_t Usb4Policy::device_count() const {
  kernel::ScopedMutex lock(mutex_);
  uint32_t n = 0;
  for (const Entry& e : entries_) n += e.used ? 1 : 0;
  return n;
}

bool Usb4Policy::device(uint32_t index, Usb4DeviceInfo* out) const {
  kernel::ScopedMutex lock(mutex_);
  for (const Entry& e : entries_) {
    if (!e.used) continue;
    if (index-- == 0) {
      *out = e.info;
      return true;
    }
  }
  return false;
}

bool Usb4Policy::next_request(Usb4DeviceInfo* out) const {
  kernel::ScopedMutex lock(mutex_);
  for (const Entry& e : entries_) {
    if (!e.used || e.info.state != Usb4DeviceState::Waiting) continue;
    bool answered = false;                           // (an answer already queued: not asked again)
    for (uint32_t i = 0; i < answer_count_; ++i) answered = answered || answers_[i].uid == e.info.uid;
    if (answered) continue;
    if (e.info.parent_uid) {
      bool parent_ready = false;
      for (const Entry& p : entries_)
        if (p.used && p.info.uid == e.info.parent_uid) parent_ready = p.info.state == Usb4DeviceState::Allowed || !p.info.pcie_capable;
      if (!parent_ready) continue;
    }
    *out = e.info;
    return true;
  }
  return false;
}

void Usb4Policy::approve(uint64_t uid, bool always) {
  kernel::ScopedMutex lock(mutex_);
  if (answer_count_ < kMaxAnswers) answers_[answer_count_++] = Answer{uid, always ? Answer::ApproveAlways : Answer::Approve};
}

void Usb4Policy::deny(uint64_t uid) {
  kernel::ScopedMutex lock(mutex_);
  if (answer_count_ < kMaxAnswers) answers_[answer_count_++] = Answer{uid, Answer::Deny};
}

void Usb4Policy::forget(uint64_t uid) {
  kernel::ScopedMutex lock(mutex_);
  if (answer_count_ < kMaxAnswers) answers_[answer_count_++] = Answer{uid, Answer::Forget};
}

}  // namespace hardware
