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
#pragma once

#include <stdint.h>
#include "Utility/ArrayView.hpp"

namespace process::security {

// Access tokens and the access check of Windows: the kernel's decision on whether a token may have some rights to an object that carries a security descriptor. A token is kept as
// the blob of src/Windows/tcos_token.h; everything here reads the blob and the descriptor through bounds-checked views, so a damaged one is refused and never read past its end.

constexpr uint32_t kDelete = 0x10000, kReadControl = 0x20000, kWriteDac = 0x40000, kWriteOwner = 0x80000, kSynchronize = 0x100000, kAccessSystemSecurity = 0x1000000,
                   kMaximumAllowed = 0x2000000, kGenericAll = 0x10000000, kGenericExecute = 0x20000000, kGenericWrite = 0x40000000, kGenericRead = 0x80000000u;
constexpr uint32_t kStatusSuccess = 0, kStatusAccessDenied = 0xC0000022u, kStatusPrivilegeNotHeld = 0xC0000061u, kStatusInvalidSecurityDescriptor = 0xC0000079u,
                   kStatusBadImpersonationLevel = 0xC00000A5u, kStatusInvalidParameter = 0xC000000Du, kStatusNoSecurityOnObject = 0xC00000D7u;

// SeSecurityPrivilege and the others the access check looks at (the low part of their LUIDs).
constexpr uint32_t kSeSecurityPrivilege = 8, kSeTakeOwnershipPrivilege = 9, kSeBackupPrivilege = 17, kSeRestorePrivilege = 18;

struct GenericMapping {
  uint32_t read = 0, write = 0, execute = 0, all = 0;
  uint32_t map(uint32_t access) const;      // the generic rights of `access` as the specific rights of the object type
};

// The part of a token that the check uses.
class TokenView {
 public:
  // Parses the blob: false if it is damaged (magic, size, offsets, arrays).
  bool parse(ConstByteView blob);
  uint32_t type() const { return type_; }
  uint32_t level() const { return level_; }
  bool is_impersonation() const { return type_ == 2; }
  ConstByteView user() const { return user_; }
  ConstByteView owner() const { return owner_; }
  ConstByteView primary_group() const { return primary_group_; }
  uint32_t group_count() const { return group_count_; }
  ConstByteView group(uint32_t index, uint32_t* attributes) const;
  uint32_t restricted_count() const { return restricted_count_; }
  ConstByteView restricted(uint32_t index, uint32_t* attributes) const;
  // Is the privilege (the low part of its LUID) held, and enabled?
  bool has_privilege(uint32_t low, bool* enabled) const;
  // The SID of the integrity level (S-1-16-n), as the number n; 0x2000 (medium) if the token has none.
  uint32_t integrity_level() const;
  // Does the token's identity include the SID: the user, or a group that is enabled (or, `deny`, one that only denies)?
  bool has_sid(ConstByteView sid, bool deny, bool restricted_list) const;
  ConstByteView default_dacl() const { return default_dacl_; }

 private:
  ConstByteView blob_, user_, owner_, primary_group_, default_dacl_;
  uint32_t type_ = 0, level_ = 0, user_attributes_ = 0, group_count_ = 0, groups_off_ = 0, privilege_count_ = 0, privileges_off_ = 0, restricted_count_ = 0, restricted_off_ = 0;
};

struct CheckResult {
  uint32_t status = kStatusAccessDenied;
  uint32_t granted = 0;
  uint64_t privileges_used = 0;      // a bit for each privilege (by the low part of its LUID) that decided
};

// The rights `desired` (generic rights are mapped here) that the token gets to an object with the self-relative security descriptor. Without MAXIMUM_ALLOWED the answer is all or
// nothing (granted 0 on STATUS_ACCESS_DENIED); with it, granted is everything the token may have.
constexpr uint32_t kBackupIntent = 1;      // a file opened for backup or restore: the privileges of those grant their rights
CheckResult access_check(ConstByteView descriptor, const TokenView& token, uint32_t desired, const GenericMapping& mapping, uint32_t flags = 0);

// Is the self-relative descriptor well formed (revision, offsets inside the buffer, SIDs and ACLs whole)?
bool valid_descriptor(ConstByteView descriptor);

// The SID and ACL at an offset of a self-relative descriptor: empty if there is none or it is damaged.
ConstByteView descriptor_owner(ConstByteView descriptor);
ConstByteView descriptor_group(ConstByteView descriptor);
ConstByteView descriptor_dacl(ConstByteView descriptor, bool* present);
ConstByteView descriptor_sacl(ConstByteView descriptor, bool* present);
uint16_t descriptor_control(ConstByteView descriptor);

}  // namespace process::security
