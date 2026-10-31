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
#include "Process/AccessCheck.hpp"
#include "Windows/tcos_token.h"

namespace process::security {

namespace {

constexpr uint16_t kSelfRelative = 0x8000, kDaclPresent = 0x4, kSaclPresent = 0x10;
constexpr uint8_t kAccessAllowed = 0, kAccessDenied = 1, kMandatoryLabel = 0x11, kInheritOnly = 0x08;
constexpr uint32_t kGroupEnabled = 0x4, kGroupUseForDenyOnly = 0x10, kGroupIntegrity = 0x20;
constexpr uint32_t kLabelNoWriteUp = 1, kLabelNoReadUp = 2, kLabelNoExecuteUp = 4;
constexpr uint32_t kPrivilegeEnabled = 0x2, kPrivilegeEnabledByDefault = 0x1;
constexpr uint32_t kStandardWriteBits = kDelete | kWriteDac | kWriteOwner;

bool get16(ConstByteView v, uint64_t at, uint16_t* out) {
  uint8_t a, b;
  if (!v.try_get(at, &a) || !v.try_get(at + 1, &b)) return false;
  *out = uint16_t(a | b << 8);
  return true;
}
bool get32(ConstByteView v, uint64_t at, uint32_t* out) {
  uint16_t lo, hi;
  if (!get16(v, at, &lo) || !get16(v, at + 2, &hi)) return false;
  *out = uint32_t(lo) | uint32_t(hi) << 16;
  return true;
}

// The SID that starts the view (revision 1, up to 15 subauthorities, all there): its bytes; empty if there is none.
ConstByteView sid_at(ConstByteView view) {
  uint8_t revision, count;
  if (!view.try_get(0, &revision) || !view.try_get(1, &count) || revision != 1 || count > 15) return ConstByteView();
  ConstByteView sid;
  return view.try_subview(0, 8 + 4 * uint64_t{count}, &sid) ? sid : ConstByteView();
}
bool same_sid(ConstByteView a, ConstByteView b) {
  if (a.empty() || a.size() != b.size()) return false;
  for (uint64_t i = 0; i < a.size(); ++i) if (a[i] != b[i]) return false;
  return true;
}
// The last subauthority of a SID (0 if it has none).
uint32_t last_sub(ConstByteView sid) {
  uint32_t v = 0;
  return sid.size() >= 12 && get32(sid, sid.size() - 4, &v) ? v : 0;
}

// The ACL at the start of the view, cut to its size: empty if it is damaged (the size, the ACEs inside it).
ConstByteView acl_at(ConstByteView view) {
  uint8_t revision;
  uint16_t size, count;
  if (!view.try_get(0, &revision) || revision < 2 || revision > 4 || !get16(view, 2, &size) || !get16(view, 4, &count) || size < 8) return ConstByteView();
  ConstByteView acl;
  if (!view.try_subview(0, size, &acl)) return ConstByteView();
  uint64_t at = 8;
  for (uint32_t i = 0; i < count; ++i) {
    uint16_t ace_size;
    if (acl.size() - at < 4 || !get16(acl, at + 2, &ace_size) || ace_size < 4 || ace_size > acl.size() - at) return ConstByteView();
    at += ace_size;
  }
  return acl;
}

// One ACE of an ACL.
struct Ace {
  uint8_t type = 0, flags = 0;
  uint32_t mask = 0;
  ConstByteView sid;
  bool has_object_type = false;
};
// The ACE at `at`; false if it has no mask and SID (an ACE of a kind that is not known to hold a SID is passed over, not read).
bool ace_at(ConstByteView acl, uint64_t at, uint64_t* size, Ace* out) {
  uint16_t ace_size;
  if (!get16(acl, at + 2, &ace_size)) return false;
  *size = ace_size;
  out->type = acl[at];
  out->flags = acl[at + 1];
  out->sid = ConstByteView();
  out->has_object_type = false;
  uint64_t sid_at_offset = at + 8;
  const uint8_t t = out->type;
  const bool plain = t <= 3 || t == 9 || t == 10 || t == 0x11 || t == 0x12 || t == 0x13 || t == 0x14 || t == 0x15 || t == 0x16;
  const bool object = t == 5 || t == 6 || t == 7 || t == 8 || t == 11 || t == 12 || t == 13 || t == 14;
  if (!plain && !object) return false;
  if (!get32(acl, at + 4, &out->mask)) return false;
  if (object) {
    uint32_t flags;
    if (!get32(acl, at + 8, &flags)) return false;
    sid_at_offset = at + 12;
    if (flags & 1) { sid_at_offset += 16; out->has_object_type = true; }
    if (flags & 2) sid_at_offset += 16;
  }
  if (sid_at_offset >= at + ace_size) return false;
  ConstByteView rest;
  if (!acl.try_subview(sid_at_offset, at + ace_size - sid_at_offset, &rest)) return false;
  out->sid = sid_at(rest);
  return !out->sid.empty();
}

}  // namespace

uint32_t GenericMapping::map(uint32_t access) const {
  uint32_t mapped = access;
  if (access & kGenericRead) mapped |= read;
  if (access & kGenericWrite) mapped |= write;
  if (access & kGenericExecute) mapped |= execute;
  if (access & kGenericAll) mapped |= all;
  return mapped & ~(kGenericRead | kGenericWrite | kGenericExecute | kGenericAll);
}

bool TokenView::parse(ConstByteView blob) {
  *this = TokenView();
  uint32_t magic, size;
  if (!get32(blob, 0, &magic) || magic != TCOS_TOKEN_MAGIC || !get32(blob, 4, &size) || size < sizeof(TcosTokenBlob) || size > blob.size()) return false;
  blob_ = blob.first(size);
  uint32_t user_off, owner_off, pgroup_off, dacl_off;
  if (!get32(blob_, 8, &type_) || !get32(blob_, 12, &level_)) return false;
  // (the layout is that of TcosTokenBlob, read field by field)
  constexpr uint64_t kUserOff = 8 + 4 * 2 + 8 * 4 + 4 * 4;      // magic, size, type, level, four 64 bit numbers, session..policy
  if (!get32(blob_, kUserOff, &user_off) || !get32(blob_, kUserOff + 4, &user_attributes_) || !get32(blob_, kUserOff + 8, &owner_off) || !get32(blob_, kUserOff + 12, &pgroup_off) ||
      !get32(blob_, kUserOff + 16, &dacl_off) || !get32(blob_, kUserOff + 20, &group_count_) || !get32(blob_, kUserOff + 24, &groups_off_) ||
      !get32(blob_, kUserOff + 28, &privilege_count_) || !get32(blob_, kUserOff + 32, &privileges_off_) || !get32(blob_, kUserOff + 36, &restricted_count_) ||
      !get32(blob_, kUserOff + 40, &restricted_off_)) return false;
  if (group_count_ > TCOS_TOKEN_MAX_GROUPS || privilege_count_ > TCOS_TOKEN_MAX_PRIVILEGES || restricted_count_ > TCOS_TOKEN_MAX_RESTRICTED) return false;
  if (user_off == 0 || user_off >= blob_.size()) return false;
  user_ = sid_at(blob_.subview(user_off));
  if (user_.empty()) return false;
  if (owner_off != 0) { if (owner_off >= blob_.size()) return false; owner_ = sid_at(blob_.subview(owner_off)); if (owner_.empty()) return false; }
  if (pgroup_off != 0) { if (pgroup_off >= blob_.size()) return false; primary_group_ = sid_at(blob_.subview(pgroup_off)); if (primary_group_.empty()) return false; }
  if (dacl_off != 0) { if (dacl_off >= blob_.size()) return false; default_dacl_ = acl_at(blob_.subview(dacl_off)); if (default_dacl_.empty()) return false; }
  for (uint32_t i = 0; i < group_count_; ++i) { uint32_t a; if (group(i, &a).empty()) return false; }
  for (uint32_t i = 0; i < restricted_count_; ++i) { uint32_t a; if (restricted(i, &a).empty()) return false; }
  uint32_t low;
  uint64_t end;
  if (privilege_count_ != 0 && (privileges_off_ > blob_.size() || !get32(blob_, privileges_off_ + 12 * uint64_t{privilege_count_} - 12, &low) || (end = privileges_off_ + 12 * uint64_t{privilege_count_}) > blob_.size())) return false;
  return true;
}

ConstByteView TokenView::group(uint32_t index, uint32_t* attributes) const {
  uint32_t sid_off;
  if (index >= group_count_ || !get32(blob_, groups_off_ + 8 * uint64_t{index}, &sid_off) || !get32(blob_, groups_off_ + 8 * uint64_t{index} + 4, attributes) || sid_off >= blob_.size()) return ConstByteView();
  return sid_at(blob_.subview(sid_off));
}
ConstByteView TokenView::restricted(uint32_t index, uint32_t* attributes) const {
  uint32_t sid_off;
  if (index >= restricted_count_ || !get32(blob_, restricted_off_ + 8 * uint64_t{index}, &sid_off) || !get32(blob_, restricted_off_ + 8 * uint64_t{index} + 4, attributes) || sid_off >= blob_.size()) return ConstByteView();
  return sid_at(blob_.subview(sid_off));
}
bool TokenView::has_privilege(uint32_t low, bool* enabled) const {
  for (uint32_t i = 0; i < privilege_count_; ++i) {
    uint32_t l, h, a;
    const uint64_t at = privileges_off_ + 12 * uint64_t{i};
    if (!get32(blob_, at, &l) || !get32(blob_, at + 4, &h) || !get32(blob_, at + 8, &a)) return false;
    if (l == low && h == 0) { *enabled = (a & kPrivilegeEnabled) != 0; return true; }
  }
  return false;
}
uint32_t TokenView::integrity_level() const {
  for (uint32_t i = 0; i < group_count_; ++i) {
    uint32_t attributes;
    const ConstByteView sid = group(i, &attributes);
    if ((attributes & kGroupIntegrity) && sid.size() >= 12 && sid[1] == 1 && sid[7] == 16) return last_sub(sid);
  }
  return 0x2000;
}
bool TokenView::has_sid(ConstByteView sid, bool deny, bool restricted_list) const {
  if (restricted_list) {
    for (uint32_t i = 0; i < restricted_count_; ++i) {
      uint32_t attributes;
      if (same_sid(restricted(i, &attributes), sid)) return true;
    }
    return false;
  }
  if (same_sid(user_, sid)) return deny || !(user_attributes_ & kGroupUseForDenyOnly);
  for (uint32_t i = 0; i < group_count_; ++i) {
    uint32_t attributes;
    if (!same_sid(group(i, &attributes), sid)) continue;
    if (attributes & kGroupUseForDenyOnly) { if (deny) return true; continue; }
    if (attributes & kGroupEnabled) return true;
  }
  return false;
}

// ---- security descriptors -----------------------------------------------------------------------------------------------------------------------------

uint16_t descriptor_control(ConstByteView d) {
  uint16_t c = 0;
  return get16(d, 2, &c) ? c : 0;
}
static ConstByteView part_at(ConstByteView d, uint64_t field) {
  uint32_t off;
  if (!get32(d, field, &off) || off == 0 || off >= d.size()) return ConstByteView();
  return d.subview(off);
}
ConstByteView descriptor_owner(ConstByteView d) { return sid_at(part_at(d, 4)); }
ConstByteView descriptor_group(ConstByteView d) { return sid_at(part_at(d, 8)); }
ConstByteView descriptor_sacl(ConstByteView d, bool* present) {
  *present = (descriptor_control(d) & kSaclPresent) != 0;
  return *present ? acl_at(part_at(d, 12)) : ConstByteView();
}
ConstByteView descriptor_dacl(ConstByteView d, bool* present) {
  *present = (descriptor_control(d) & kDaclPresent) != 0;
  return *present ? acl_at(part_at(d, 16)) : ConstByteView();
}

bool valid_descriptor(ConstByteView d) {
  uint8_t revision;
  if (d.size() < 20 || !d.try_get(0, &revision) || revision != 1 || !(descriptor_control(d) & kSelfRelative)) return false;
  uint32_t owner, group, sacl, dacl;
  if (!get32(d, 4, &owner) || !get32(d, 8, &group) || !get32(d, 12, &sacl) || !get32(d, 16, &dacl)) return false;
  if (owner && descriptor_owner(d).empty()) return false;
  if (group && descriptor_group(d).empty()) return false;
  bool present;
  if (sacl && (descriptor_control(d) & kSaclPresent) && descriptor_sacl(d, &present).empty()) return false;
  if (dacl && (descriptor_control(d) & kDaclPresent) && descriptor_dacl(d, &present).empty()) return false;
  return true;
}

// ---- the check ------------------------------------------------------------------------------------------------------------------------------------

namespace {

// Goes through a DACL for one identity (the token, or its restricting SIDs). Without `maximum`: `remaining` is what is still asked for; clears what the allowing ACEs give, and says
// false if a denying ACE takes some of it. With `maximum`: gathers in *granted what is allowed and not denied.
bool walk_dacl(ConstByteView acl, const TokenView& token, bool restricted, bool maximum, uint32_t* remaining, uint32_t* granted) {
  uint16_t count = 0;
  get16(acl, 4, &count);
  uint64_t at = 8;
  uint32_t denied = 0;
  for (uint32_t i = 0; i < count; ++i) {
    uint64_t size = 0;
    Ace ace;
    uint16_t ace_size = 0;
    get16(acl, at + 2, &ace_size);
    size = ace_size;
    if (!ace_at(acl, at, &size, &ace)) { at += ace_size; continue; }
    at += size;
    if (ace.flags & kInheritOnly) continue;
    if (ace.type == kAccessAllowed || (ace.type == 5 && !ace.has_object_type)) {
      if (!token.has_sid(ace.sid, false, restricted)) continue;
      if (maximum) *granted |= ace.mask & ~denied;
      else { *remaining &= ~ace.mask; if (*remaining == 0) return true; }
    } else if (ace.type == kAccessDenied || (ace.type == 6 && !ace.has_object_type)) {
      if (!token.has_sid(ace.sid, true, restricted)) continue;
      if (maximum) denied |= ace.mask & ~*granted;
      else if (ace.mask & *remaining) return false;
    }
  }
  return true;
}

// The rights that the mandatory label of the descriptor keeps from a token of lower integrity.
uint32_t label_removes(ConstByteView descriptor, const TokenView& token, const GenericMapping& mapping) {
  const uint32_t level = token.integrity_level();
  uint32_t label_level = 0x2000, policy = kLabelNoWriteUp;
  bool found = false, present;
  const ConstByteView sacl = descriptor_sacl(descriptor, &present);
  if (!sacl.empty()) {
    uint16_t count = 0;
    get16(sacl, 4, &count);
    uint64_t at = 8;
    for (uint32_t i = 0; i < count; ++i) {
      uint64_t size = 0;
      Ace ace;
      uint16_t ace_size = 0;
      get16(sacl, at + 2, &ace_size);
      if (ace_at(sacl, at, &size, &ace) && ace.type == kMandatoryLabel) { label_level = last_sub(ace.sid); policy = ace.mask; found = true; break; }
      at += ace_size;
    }
  }
  if (level >= label_level) return 0;
  if (!found && level >= 0x2000) return 0;
  uint32_t removed = 0;
  if (policy & kLabelNoWriteUp) removed |= (mapping.write & ~kSynchronize & ~kReadControl) | kStandardWriteBits;
  if (policy & kLabelNoReadUp) removed |= mapping.read & ~kReadControl & ~kSynchronize;
  if (policy & kLabelNoExecuteUp) removed |= mapping.execute & ~kReadControl & ~kSynchronize;
  return removed;
}

}  // namespace

CheckResult access_check(ConstByteView descriptor, const TokenView& token, uint32_t desired, const GenericMapping& mapping, uint32_t flags) {
  CheckResult result;
  if (!valid_descriptor(descriptor) || descriptor_owner(descriptor).empty() || descriptor_group(descriptor).empty()) { result.status = kStatusInvalidSecurityDescriptor; return result; }
  if (token.is_impersonation() && token.level() < 1) { result.status = kStatusBadImpersonationLevel; return result; }
  const bool maximum = (desired & kMaximumAllowed) != 0;
  const uint32_t asked = mapping.map(desired & ~kMaximumAllowed);
  if (!maximum && asked == 0) { result.status = kStatusAccessDenied; return result; }      // (nothing asked for is nothing granted)
  uint32_t remaining = asked, granted = 0;
  bool enabled = false;
  if (remaining & kAccessSystemSecurity) {
    if (!token.has_privilege(kSeSecurityPrivilege, &enabled) || !enabled) { result.status = kStatusPrivilegeNotHeld; return result; }
    granted |= kAccessSystemSecurity;
    remaining &= ~kAccessSystemSecurity;
    result.privileges_used |= uint64_t{1} << kSeSecurityPrivilege;
  } else if (maximum && token.has_privilege(kSeSecurityPrivilege, &enabled) && enabled) {
    granted |= kAccessSystemSecurity;
    result.privileges_used |= uint64_t{1} << kSeSecurityPrivilege;
  }
  if (token.has_privilege(kSeTakeOwnershipPrivilege, &enabled) && enabled && ((remaining & kWriteOwner) || maximum)) {
    granted |= kWriteOwner;
    remaining &= ~kWriteOwner;
    result.privileges_used |= uint64_t{1} << kSeTakeOwnershipPrivilege;
  }
  if (flags & kBackupIntent) {      // the privileges of backup and restore (of files): the rights they stand for
    constexpr uint32_t kBackupRights = kReadControl | kSynchronize | 0x1200A9u | kAccessSystemSecurity, kRestoreRights = kWriteDac | kWriteOwner | kDelete | 0x120116u | 0x6u | kAccessSystemSecurity;
    if (token.has_privilege(kSeBackupPrivilege, &enabled) && enabled && (remaining & kBackupRights)) { granted |= remaining & kBackupRights; remaining &= ~kBackupRights; result.privileges_used |= uint64_t{1} << kSeBackupPrivilege; }
    if (token.has_privilege(kSeRestorePrivilege, &enabled) && enabled && (remaining & kRestoreRights)) { granted |= remaining & kRestoreRights; remaining &= ~kRestoreRights; result.privileges_used |= uint64_t{1} << kSeRestorePrivilege; }
  }
  bool dacl_present;
  const ConstByteView dacl = descriptor_dacl(descriptor, &dacl_present);
  // The owner may always read and change the descriptor, unless the DACL speaks for the owner itself (an ACE for OWNER RIGHTS, S-1-3-4).
  bool owner_rights_ace = false;
  if (!dacl.empty()) {
    uint16_t count = 0;
    get16(dacl, 4, &count);
    uint64_t at = 8;
    for (uint32_t i = 0; i < count; ++i) {
      uint64_t size = 0;
      Ace ace;
      uint16_t ace_size = 0;
      get16(dacl, at + 2, &ace_size);
      if (ace_at(dacl, at, &size, &ace) && ace.sid.size() == 12 && ace.sid[7] == 3 && last_sub(ace.sid) == 4) owner_rights_ace = true;
      at += ace_size;
    }
  }
  if (!owner_rights_ace && token.has_sid(descriptor_owner(descriptor), false, false)) {
    granted |= kReadControl | kWriteDac;
    remaining &= ~(kReadControl | kWriteDac);
  }
  const uint32_t privilege_granted = granted;
  if (!dacl_present || dacl.empty()) {
    // no DACL, or a NULL one: everything is allowed (an empty one, size 8, is not empty() here: acl_at gives its 8 bytes)
    granted |= remaining | (maximum ? mapping.all : 0);
    remaining = 0;
  } else {
    if (maximum) {
      uint32_t gathered = 0, dummy = 0;
      walk_dacl(dacl, token, false, true, &dummy, &gathered);
      if (token.restricted_count() != 0) {
        uint32_t by_restricted = 0;
        walk_dacl(dacl, token, true, true, &dummy, &by_restricted);
        gathered &= by_restricted;
      }
      granted |= gathered;
      remaining &= ~granted;
    } else {
      const uint32_t before = remaining;
      if (!walk_dacl(dacl, token, false, false, &remaining, &granted)) { result.status = kStatusAccessDenied; return result; }
      if (remaining == 0 && token.restricted_count() != 0) {      // a restricted token must be allowed by its restricting SIDs as well
        uint32_t second = before, unused = 0;
        if (!walk_dacl(dacl, token, true, false, &second, &unused) || second != 0) { result.status = kStatusAccessDenied; return result; }
      }
    }
  }
  // The mandatory label: a token of lower integrity does not get some of the rights.
  const uint32_t removed = label_removes(descriptor, token, mapping);
  if (removed) {
    if (!maximum && (asked & removed)) { result.status = kStatusAccessDenied; return result; }
    granted &= ~removed;
  }
  if (maximum) {
    if (asked != 0 && (asked & ~granted)) { result.status = kStatusAccessDenied; return result; }
    result.granted = granted;
    result.status = granted ? kStatusSuccess : kStatusAccessDenied;
    return result;
  }
  if (remaining != 0) { result.status = kStatusAccessDenied; return result; }
  result.granted = asked;
  result.status = kStatusSuccess;
  return result;
}

}  // namespace process::security
