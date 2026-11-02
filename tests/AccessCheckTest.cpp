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

// Host-side tests for the access check of the kernel: tokens (blob of src/Windows/tcos_token.h) and self-relative security descriptors, the decisions, and truncated and bit-flipped
// blobs and descriptors (they must be refused or read without going past their end).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>
#include "Process/AccessCheck.hpp"
#include "Windows/tcos_token.h"

void bounds_violation(const char*, uint64_t, uint64_t) { abort(); }

static int g_failed = 0;
#define CHECK(cond) do { if (!(cond)) { printf("FAILED line %d: %s\n", __LINE__, #cond); ++g_failed; } } while (0)

using namespace process::security;

using Bytes = std::vector<uint8_t>;
static void put16(Bytes& b, uint32_t v) { b.push_back(uint8_t(v)); b.push_back(uint8_t(v >> 8)); }
static void put32(Bytes& b, uint32_t v) { put16(b, v & 0xFFFF); put16(b, v >> 16); }
static void set32(Bytes& b, size_t at, uint32_t v) { for (int i = 0; i < 4; ++i) b[at + i] = uint8_t(v >> (8 * i)); }

static Bytes sid(uint8_t authority, std::initializer_list<uint32_t> subs) {
  Bytes b = {1, uint8_t(subs.size()), 0, 0, 0, 0, 0, authority};
  for (uint32_t s : subs) put32(b, s);
  return b;
}
static const Bytes kUser = sid(5, {21, 1, 2, 3, 1000}), kAdmins = sid(5, {32, 544}), kEveryone = sid(1, {0}), kUsers = sid(5, {32, 545}), kOther = sid(5, {21, 1, 2, 3, 1001});
static const Bytes kHigh = sid(16, {12288}), kMedium = sid(16, {8192}), kLow = sid(16, {4096});

struct Group { Bytes sid; uint32_t attrs; };
struct Priv { uint32_t low, attrs; };

// A token blob: the user, the groups, the privileges, restricting SIDs, and the type and level.
static Bytes make_token(const Bytes& user, const std::vector<Group>& groups, const std::vector<Priv>& privileges, const std::vector<Group>& restricted, uint32_t type = 1, uint32_t level = 0) {
  TcosTokenBlob h = {};
  h.magic = TCOS_TOKEN_MAGIC;
  h.type = type;
  h.level = level;
  Bytes tail;
  uint32_t at = sizeof(h);
  auto place = [&](const Bytes& s) { uint32_t off = at; tail.insert(tail.end(), s.begin(), s.end()); at += uint32_t(s.size()); return off; };
  h.user_off = place(user);
  std::vector<uint32_t> g_off, r_off;
  for (const Group& g : groups) g_off.push_back(place(g.sid));
  for (const Group& g : restricted) r_off.push_back(place(g.sid));
  h.group_count = uint32_t(groups.size());
  h.groups_off = at;
  for (size_t i = 0; i < groups.size(); ++i) { put32(tail, g_off[i]); put32(tail, groups[i].attrs); at += 8; }
  h.restricted_count = uint32_t(restricted.size());
  h.restricted_off = at;
  for (size_t i = 0; i < restricted.size(); ++i) { put32(tail, r_off[i]); put32(tail, restricted[i].attrs); at += 8; }
  h.privilege_count = uint32_t(privileges.size());
  h.privileges_off = at;
  for (const Priv& p : privileges) { put32(tail, p.low); put32(tail, 0); put32(tail, p.attrs); at += 12; }
  h.size = at;
  Bytes out(reinterpret_cast<uint8_t*>(&h), reinterpret_cast<uint8_t*>(&h) + sizeof(h));
  out.insert(out.end(), tail.begin(), tail.end());
  return out;
}

struct Ace { uint8_t type, flags; uint32_t mask; Bytes sid; };
// A self-relative descriptor: owner, group and a DACL of the ACEs (`dacl` false: none present), and a SACL with the ACEs given (a mandatory label).
static Bytes make_sd(const Bytes& owner, const Bytes& group, bool dacl_present, const std::vector<Ace>& dacl, const std::vector<Ace>& sacl = {}) {
  auto acl_of = [](const std::vector<Ace>& aces) {
    Bytes a = {2, 0};
    put16(a, 0);
    put16(a, uint32_t(aces.size()));
    put16(a, 0);
    for (const Ace& e : aces) { a.push_back(e.type); a.push_back(e.flags); put16(a, uint32_t(8 + e.sid.size())); put32(a, e.mask); a.insert(a.end(), e.sid.begin(), e.sid.end()); }
    a[2] = uint8_t(a.size()); a[3] = uint8_t(a.size() >> 8);
    return a;
  };
  Bytes b = {1, 0};
  put16(b, 0x8000 | (dacl_present ? 4 : 0) | (sacl.empty() ? 0 : 0x10));
  for (int i = 0; i < 4; ++i) put32(b, 0);
  auto place = [&](size_t field, const Bytes& part) { set32(b, field, uint32_t(b.size())); b.insert(b.end(), part.begin(), part.end()); };
  place(4, owner);
  place(8, group);
  if (!sacl.empty()) place(12, acl_of(sacl));
  if (dacl_present && !dacl.empty()) place(16, acl_of(dacl));
  else if (dacl_present) place(16, acl_of({}));
  return b;
}

static ConstByteView view(const Bytes& b) { return ConstByteView(b.data(), b.size()); }
static const GenericMapping kFile = {0x120089, 0x120116, 0x1200A0, 0x1F01FF};

static CheckResult check(const Bytes& sd, const Bytes& token_blob, uint32_t desired, uint32_t flags = 0) {
  TokenView token;
  CHECK(token.parse(view(token_blob)));
  return access_check(view(sd), token, desired, kFile, flags);
}

static const std::vector<Group> kUserGroups = {{kEveryone, 7}, {kUsers, 7}};
static const Bytes kToken = make_token(kUser, kUserGroups, {{kSeSecurityPrivilege, 0}, {kSeBackupPrivilege, 2}, {kSeTakeOwnershipPrivilege, 0}}, {});

int main() {
  // an allowed ACE for a group grants its rights; the rest is refused
  {
    const Bytes sd = make_sd(kAdmins, kAdmins, true, {{0, 0, 0x120089, kUsers}});
    CHECK(check(sd, kToken, 0x120089).status == kStatusSuccess);
    CHECK(check(sd, kToken, kGenericRead).status == kStatusSuccess);
    CHECK(check(sd, kToken, 0x2).status == kStatusAccessDenied);
    CHECK(check(sd, kToken, 0).status == kStatusAccessDenied);
    const CheckResult maximum = check(sd, kToken, kMaximumAllowed);
    CHECK(maximum.status == kStatusSuccess && maximum.granted == 0x120089);
  }
  // a denying ACE before an allowing one wins; after it, it does not
  {
    const Bytes denied_first = make_sd(kAdmins, kAdmins, true, {{1, 0, 0x2, kUsers}, {0, 0, 0x120089 | 0x2, kEveryone}});
    CHECK(check(denied_first, kToken, 0x2).status == kStatusAccessDenied);
    CHECK(check(denied_first, kToken, 0x120089).status == kStatusSuccess);
    const Bytes allowed_first = make_sd(kAdmins, kAdmins, true, {{0, 0, 0x2, kEveryone}, {1, 0, 0x2, kUsers}});
    CHECK(check(allowed_first, kToken, 0x2).status == kStatusSuccess);
    const CheckResult maximum = check(denied_first, kToken, kMaximumAllowed);
    CHECK(maximum.granted == 0x120089);
  }
  // no DACL, a NULL DACL: everything; an empty one: nothing but what the owner always has
  {
    const Bytes none = make_sd(kAdmins, kAdmins, false, {});
    CHECK(check(none, kToken, 0x1F01FF).status == kStatusSuccess);
    const Bytes empty = make_sd(kUser, kAdmins, true, {});
    CHECK(check(empty, kToken, 0x1).status == kStatusAccessDenied);
    CHECK(check(empty, kToken, kReadControl | kWriteDac).status == kStatusSuccess);       // (the owner)
    const Bytes stranger = make_sd(kOther, kAdmins, true, {});
    CHECK(check(stranger, kToken, kReadControl).status == kStatusAccessDenied);
  }
  // an ACE for a group that is only for denying, or not enabled, does not allow; one that is for denying denies
  {
    const std::vector<Group> groups = {{kEveryone, 7}, {kUsers, 0x10}, {kAdmins, 0}};
    const Bytes token = make_token(kUser, groups, {}, {});
    const Bytes allow = make_sd(kOther, kOther, true, {{0, 0, 0x1, kUsers}, {0, 0, 0x2, kAdmins}});
    CHECK(check(allow, token, 0x1).status == kStatusAccessDenied);
    CHECK(check(allow, token, 0x2).status == kStatusAccessDenied);
    const Bytes deny = make_sd(kOther, kOther, true, {{1, 0, 0x1, kUsers}, {0, 0, 0x1, kEveryone}});
    CHECK(check(deny, token, 0x1).status == kStatusAccessDenied);
  }
  // inherit-only ACEs are not looked at
  {
    const Bytes sd = make_sd(kOther, kOther, true, {{0, 0x08, 0x1, kEveryone}});
    CHECK(check(sd, kToken, 0x1).status == kStatusAccessDenied);
  }
  // privileges: the one for the SACL, for taking ownership, for backup
  {
    const Bytes sd = make_sd(kOther, kOther, true, {{0, 0, 0x1, kEveryone}});
    CHECK(check(sd, kToken, kAccessSystemSecurity).status == kStatusPrivilegeNotHeld);       // (held, not enabled)
    const Bytes enabled = make_token(kUser, kUserGroups, {{kSeSecurityPrivilege, 2}, {kSeTakeOwnershipPrivilege, 2}, {kSeBackupPrivilege, 2}}, {});
    CHECK(check(sd, enabled, kAccessSystemSecurity).status == kStatusSuccess);
    CHECK(check(sd, enabled, kWriteOwner).status == kStatusSuccess);
    CHECK(check(sd, enabled, kReadControl).status == kStatusAccessDenied);
    CHECK(check(sd, enabled, kReadControl, kBackupIntent).status == kStatusSuccess);
    CHECK(check(sd, kToken, kWriteOwner).status == kStatusAccessDenied);
  }
  // an impersonation token below identification level is refused; a restricted token needs both lists to allow
  {
    const Bytes sd = make_sd(kOther, kOther, true, {{0, 0, 0x1, kUsers}});
    const Bytes anonymous = make_token(kUser, kUserGroups, {}, {}, 2, 0);
    CHECK(check(sd, anonymous, 0x1).status == kStatusBadImpersonationLevel);
    const Bytes identification = make_token(kUser, kUserGroups, {}, {}, 2, 1);
    CHECK(check(sd, identification, 0x1).status == kStatusSuccess);
    const Bytes restricted = make_token(kUser, kUserGroups, {}, {{kEveryone, 4}});
    CHECK(check(sd, restricted, 0x1).status == kStatusAccessDenied);
    const Bytes restricted_ok = make_token(kUser, kUserGroups, {}, {{kUsers, 4}});
    CHECK(check(sd, restricted_ok, 0x1).status == kStatusSuccess);
  }
  // a mandatory label keeps the writing of a token of lower integrity out
  {
    const std::vector<Group> low_groups = {{kEveryone, 7}, {kLow, 0x60}};
    const Bytes low = make_token(kUser, low_groups, {}, {});
    const Bytes sd = make_sd(kOther, kOther, true, {{0, 0, 0x1F01FF, kEveryone}}, {{0x11, 0, 1, kMedium}});
    CHECK(check(sd, low, 0x120089).status == kStatusSuccess);
    CHECK(check(sd, low, 0x120116).status == kStatusAccessDenied);
    CHECK(check(sd, kToken, 0x120116).status == kStatusSuccess);
  }
  // damaged tokens and descriptors: every truncation, every bit flip is refused or read without a fault (the sanitizers watch)
  {
    const Bytes sd = make_sd(kUser, kAdmins, true, {{0, 0, 0x120089, kUsers}, {1, 0, 0x2, kEveryone}}, {{0x11, 0, 1, kMedium}});
    for (size_t n = 0; n < kToken.size(); ++n) { TokenView t; CHECK(!t.parse(ConstByteView(kToken.data(), n))); }
    for (size_t n = 0; n < sd.size(); ++n) CHECK(!valid_descriptor(ConstByteView(sd.data(), n)) || n == sd.size());
    for (size_t bit = 0; bit < kToken.size() * 8; ++bit) {
      Bytes f = kToken;
      f[bit / 8] ^= uint8_t(1 << (bit % 8));
      TokenView t;
      if (t.parse(view(f))) { bool enabled; t.has_privilege(kSeSecurityPrivilege, &enabled); t.integrity_level(); t.has_sid(view(kUser), false, false); }
    }
    for (size_t bit = 0; bit < sd.size() * 8; ++bit) {
      Bytes f = sd;
      f[bit / 8] ^= uint8_t(1 << (bit % 8));
      TokenView t;
      CHECK(t.parse(view(kToken)));
      access_check(view(f), t, 0x120089, kFile);
      access_check(view(f), t, kMaximumAllowed, kFile);
    }
  }
  printf(g_failed ? "AccessCheckTest: %d FAILED\n" : "AccessCheckTest: ok\n", g_failed);
  return g_failed ? 1 : 0;
}
