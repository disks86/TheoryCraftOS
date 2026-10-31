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
#include "Process/FileSecurity.hpp"
#include "Utility/ByteStream.hpp"

namespace process::filesecurity {

namespace {

constexpr uint16_t kSelfRelative = 0x8000, kDaclPresent = 0x4, kDaclProtected = 0x1000, kDaclAutoInherited = 0x400, kDaclAutoInheritRequired = 0x100;
constexpr uint32_t kMaxSidBytes = 8 + 4 * 15;
constexpr uint8_t kAccessAllowed = 0, kAccessDenied = 1, kInheritOnly = 0x08;

// The SID S-1-5-21-0-0-0-1000 (the user this system's programs run as), and the groups of that user.
constexpr uint8_t kUser[] = {1, 5, 0, 0, 0, 0, 0, 5, 21, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xE8, 0x03, 0, 0};
constexpr uint8_t kEveryone[] = {1, 1, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0};                                  // S-1-1-0
constexpr uint8_t kLocal[] = {1, 1, 0, 0, 0, 0, 0, 2, 0, 0, 0, 0};                                     // S-1-2-0
constexpr uint8_t kInteractive[] = {1, 1, 0, 0, 0, 0, 0, 5, 4, 0, 0, 0};                               // S-1-5-4
constexpr uint8_t kAuthenticated[] = {1, 1, 0, 0, 0, 0, 0, 5, 11, 0, 0, 0};                            // S-1-5-11
constexpr uint8_t kAdministrators[] = {1, 2, 0, 0, 0, 0, 0, 5, 32, 0, 0, 0, 0x20, 0x02, 0, 0};        // S-1-5-32-544
constexpr uint8_t kUsers[] = {1, 2, 0, 0, 0, 0, 0, 5, 32, 0, 0, 0, 0x21, 0x02, 0, 0};                  // S-1-5-32-545

template <uint64_t N>
bool same_sid(ConstByteView sid, const uint8_t (&known)[N]) {
  if (sid.size() < N) return false;
  for (uint64_t i = 0; i < N; ++i) if (sid[i] != known[i]) return false;
  return true;
}
bool in_token(ConstByteView sid) {
  return same_sid(sid, kUser) || same_sid(sid, kEveryone) || same_sid(sid, kLocal) || same_sid(sid, kInteractive) || same_sid(sid, kAuthenticated) || same_sid(sid, kAdministrators) ||
         same_sid(sid, kUsers);
}

// The bytes of a SID that starts `view`; empty if it is not one (revision 1, up to 15 subauthorities, all there).
ConstByteView sid_at(ConstByteView view) {
  if (view.size() < 8 || view[0] != 1 || view[1] > 15) return ConstByteView();
  const uint64_t length = 8 + 4 * uint64_t{view[1]};
  return length <= view.size() ? view.first(length) : ConstByteView();
}

uint32_t le32_at(ConstByteView view, uint64_t at) { return uint32_t(view[at]) | uint32_t(view[at + 1]) << 8 | uint32_t(view[at + 2]) << 16 | uint32_t(view[at + 3]) << 24; }
uint16_t le16_at(ConstByteView view, uint64_t at) { return uint16_t(view[at] | view[at + 1] << 8); }

// Is the ACL well formed: the ACEs inside its size, each access-allowed or -denied one with a mask and a SID that fits?
bool valid_acl(ConstByteView acl) {
  if (acl.size() < 8 || acl[0] < 2 || acl[0] > 4) return false;
  if (le16_at(acl, 2) != acl.size()) return false;
  uint64_t at = 8;
  for (uint32_t i = 0, count = le16_at(acl, 4); i < count; ++i) {
    if (acl.size() - at < 8) return false;
    const uint64_t size = le16_at(acl, at + 2);
    if (size < 8 || size > acl.size() - at) return false;
    const uint8_t type = acl[at];
    if (type == kAccessAllowed || type == kAccessDenied || type == 2 /* audit */ || type == 3 /* alarm */) {
      if (sid_at(acl.subview(at + 8, size - 8)).empty()) return false;
    }
    at += size;
  }
  return true;
}

}  // namespace

uint32_t map_generic(uint32_t access) {
  uint32_t mapped = access & 0x00FFFFFFu;
  if (access & 0x80000000u) mapped |= 0x120089;      // GENERIC_READ: FILE_GENERIC_READ
  if (access & 0x40000000u) mapped |= 0x120116;      // GENERIC_WRITE: FILE_GENERIC_WRITE
  if (access & 0x20000000u) mapped |= 0x1200A0;      // GENERIC_EXECUTE: FILE_GENERIC_EXECUTE
  if (access & 0x10000000u) mapped |= 0x1F01FF;      // GENERIC_ALL: FILE_ALL_ACCESS
  return mapped | (access & kMaximumAllowed);
}

bool import_descriptor(ReadMemory read, void* context, uint64_t address, Stored* out) {
  out->size = 0;
  uint8_t head[40] = {};
  if (address == 0 || !read(context, address, ByteView(head, 8))) return false;
  if (head[0] != 1) return false;
  const uint16_t control = uint16_t(head[2] | head[3] << 8);
  uint64_t owner_at = 0, group_at = 0, dacl_at = 0;
  if (control & kSelfRelative) {
    if (!read(context, address, ByteView(head, 20))) return false;
    const uint64_t base = address;
    const ConstByteView h(head, 20);
    owner_at = le32_at(h, 4) ? base + le32_at(h, 4) : 0;
    group_at = le32_at(h, 8) ? base + le32_at(h, 8) : 0;
    dacl_at = le32_at(h, 16) ? base + le32_at(h, 16) : 0;
  } else {
    if (!read(context, address, ByteView(head, 40))) return false;
    auto pointer = [&](uint64_t at) { uint64_t v = 0; for (uint32_t i = 0; i < 8; ++i) v |= uint64_t{head[at + i]} << (8 * i); return v; };
    owner_at = pointer(8);
    group_at = pointer(16);
    dacl_at = pointer(32);
  }
  uint8_t owner[kMaxSidBytes], group[kMaxSidBytes], acl[kMaxAclBytes];
  uint64_t owner_size = 0, group_size = 0, acl_size = 0;
  auto import_sid = [&](uint64_t at, uint8_t* into, uint64_t* size) {
    if (at == 0) return true;
    uint8_t first[8];
    if (!read(context, at, ByteView(first, 8))) return false;
    if (first[0] != 1 || first[1] > 15) return false;
    const uint64_t length = 8 + 4 * uint64_t{first[1]};
    if (!read(context, at, ByteView(into, length))) return false;
    *size = length;
    return true;
  };
  if (!import_sid(owner_at, owner, &owner_size) || !import_sid(group_at, group, &group_size)) return false;
  if ((control & kDaclPresent) && dacl_at != 0) {
    uint8_t first[8];
    if (!read(context, dacl_at, ByteView(first, 8))) return false;
    acl_size = uint64_t{first[2]} | uint64_t{first[3]} << 8;
    if (acl_size < 8 || acl_size > kMaxAclBytes) return false;
    if (!read(context, dacl_at, ByteView(acl, acl_size)) || !valid_acl(ConstByteView(acl, acl_size))) return false;
  }
  // Built self-relative: header, owner, group, DACL.
  ByteWriter w{ByteView(out->bytes, sizeof(out->bytes))};
  const uint16_t kept = uint16_t(kSelfRelative | (control & (kDaclPresent | kDaclProtected | kDaclAutoInherited | kDaclAutoInheritRequired)));
  uint32_t offset = 20;
  const uint32_t owner_offset = owner_size ? offset : 0;
  offset += uint32_t(owner_size);
  const uint32_t group_offset = group_size ? offset : 0;
  offset += uint32_t(group_size);
  const uint32_t dacl_offset = acl_size ? offset : 0;
  w.u8(1);
  w.u8(0);
  w.le16(kept);
  w.le32(owner_offset);
  w.le32(group_offset);
  w.le32(0);      // no SACL
  w.le32(dacl_offset);
  w.bytes(ConstByteView(owner, owner_size));
  w.bytes(ConstByteView(group, group_size));
  w.bytes(ConstByteView(acl, acl_size));
  if (!w.ok()) return false;
  out->size = uint32_t(w.position());
  return true;
}

bool allows(const Stored& descriptor, uint32_t desired) {
  desired = map_generic(desired);
  if (desired & kMaximumAllowed) return true;
  desired &= ~uint32_t{0x01000000};      // ACCESS_SYSTEM_SECURITY is not asked for here
  if (descriptor.size < 20) return false;
  const ConstByteView d(descriptor.bytes, descriptor.size);
  if (d[0] != 1) return false;
  const uint16_t control = le16_at(d, 2);
  const uint32_t owner_offset = le32_at(d, 4), dacl_offset = le32_at(d, 16);
  if (owner_offset != 0 && owner_offset < d.size()) {
    const ConstByteView owner = sid_at(d.subview(owner_offset));
    if (!owner.empty() && in_token(owner)) desired &= ~(kReadControl | kWriteDac);      // (the owner can always read and change the descriptor)
  }
  if (desired == 0) return true;
  if (!(control & kDaclPresent)) return true;      // no DACL: everybody has everything
  if (dacl_offset == 0 || dacl_offset >= d.size()) return true;      // a NULL DACL
  const ConstByteView acl = d.subview(dacl_offset);
  if (acl.size() < 8 || le16_at(acl, 2) > acl.size()) return false;
  uint32_t remaining = desired;
  uint64_t at = 8;
  for (uint32_t i = 0, count = le16_at(acl, 4); i < count; ++i) {
    if (acl.size() - at < 8) return false;
    const uint64_t size = le16_at(acl, at + 2);
    if (size < 8 || size > acl.size() - at) return false;
    const uint8_t type = acl[at], flags = acl[at + 1];
    const ConstByteView sid = sid_at(acl.subview(at + 8, size - 8));
    if ((type == kAccessAllowed || type == kAccessDenied) && !(flags & kInheritOnly) && !sid.empty() && in_token(sid)) {
      const uint32_t mask = le32_at(acl, at + 4);
      if (type == kAccessDenied) { if (mask & remaining) return false; }
      else remaining &= ~mask;
      if (remaining == 0) return true;
    }
    at += size;
  }
  return remaining == 0;
}

}  // namespace process::filesecurity
