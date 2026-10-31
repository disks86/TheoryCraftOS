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

// Host-side tests for the security descriptors of files: the import of a descriptor from program memory (self-relative and absolute, truncated and with flipped bits) and the
// access decisions.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "Process/FileSecurity.hpp"

void bounds_violation(const char*, uint64_t, uint64_t) { abort(); }

static int g_failed = 0;
#define CHECK(cond) do { if (!(cond)) { printf("FAILED line %d: %s\n", __LINE__, #cond); ++g_failed; } } while (0)

using namespace process::filesecurity;

// Program memory: a buffer that starts at `base`.
struct Memory { const uint8_t* data; uint64_t size, base; };
static bool read_memory(void* context, uint64_t address, ByteView out) {
  const Memory& m = *static_cast<Memory*>(context);
  if (address < m.base || address - m.base > m.size || out.size() > m.size - (address - m.base)) return false;
  memcpy(out.data(), m.data + (address - m.base), out.size());
  return true;
}

static const uint8_t kWorld[12] = {1, 1, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0};                                 // S-1-1-0
static const uint8_t kOther[16] = {1, 2, 0, 0, 0, 0, 0, 5, 32, 0, 0, 0, 0x2F, 0x02, 0, 0};             // S-1-5-32-559 (a group the user is not in)

// An ACL of `count` ACEs given as {type, flags, mask, sid, sid_size}.
struct Ace { uint8_t type, flags; uint32_t mask; const uint8_t* sid; uint32_t sid_size; };
static uint32_t build_acl(uint8_t* out, const Ace* aces, uint32_t count) {
  uint32_t size = 8;
  for (uint32_t i = 0; i < count; ++i) {
    const uint32_t ace_size = 8 + aces[i].sid_size;
    out[size] = aces[i].type; out[size + 1] = aces[i].flags; out[size + 2] = uint8_t(ace_size); out[size + 3] = uint8_t(ace_size >> 8);
    for (int k = 0; k < 4; ++k) out[size + 4 + k] = uint8_t(aces[i].mask >> (8 * k));
    memcpy(out + size + 8, aces[i].sid, aces[i].sid_size);
    size += ace_size;
  }
  memset(out, 0, 8);
  out[0] = 2; out[2] = uint8_t(size); out[3] = uint8_t(size >> 8); out[4] = uint8_t(count);
  return size;
}
// A self-relative descriptor with a DACL (control: the bits besides self-relative).
static uint32_t build_descriptor(uint8_t* out, uint16_t control, const uint8_t* acl, uint32_t acl_size) {
  memset(out, 0, 20);
  out[0] = 1;
  const uint16_t c = uint16_t(0x8000 | control);
  out[2] = uint8_t(c); out[3] = uint8_t(c >> 8);
  if (acl_size) { out[16] = 20; memcpy(out + 20, acl, acl_size); }
  return 20 + acl_size;
}

static bool import(const uint8_t* bytes, uint64_t size, uint64_t address, uint64_t base, Stored* stored) {
  Memory m{bytes, size, base};
  return import_descriptor(read_memory, &m, address, stored);
}

int main() {
  // "D:PAI(A;;0x1200a9;;;WD)": everybody may read, nobody write.
  uint8_t acl[128], sd[256];
  const Ace read_only[] = {{0, 0, 0x1200a9, kWorld, sizeof(kWorld)}};
  uint32_t acl_size = build_acl(acl, read_only, 1);
  uint32_t sd_size = build_descriptor(sd, 0x0004 | 0x1000 | 0x0400, acl, acl_size);
  Stored stored;
  CHECK(import(sd, sd_size, 0x1000, 0x1000, &stored));
  CHECK(stored.size == sd_size);
  CHECK(allows(stored, 0x1));            // FILE_LIST_DIRECTORY
  CHECK(allows(stored, 0x120089));       // FILE_GENERIC_READ
  CHECK(allows(stored, 0x80000000u));    // GENERIC_READ
  CHECK(!allows(stored, kFileAddFile));
  CHECK(!allows(stored, kFileAddSubdirectory));
  CHECK(!allows(stored, 0x40000000u));   // GENERIC_WRITE
  CHECK(!allows(stored, kDelete));
  CHECK(!allows(stored, kFileDeleteChild));
  CHECK(allows(stored, 0));
  CHECK(allows(stored, kMaximumAllowed));
  CHECK(allows(stored, 0x1 | kReadControl));
  CHECK(!allows(stored, 0x1 | 0x100000 | kWriteDac));      // (WRITE_DAC is not in the ACE, and the owner is not named)

  // A deny before an allow wins; an allow for a group the user is not in grants nothing; an inherit-only ACE does not apply to the node itself.
  const Ace denied_first[] = {{1, 0, 0x2, kWorld, sizeof(kWorld)}, {0, 0, 0x1F01FF, kWorld, sizeof(kWorld)}};
  acl_size = build_acl(acl, denied_first, 2);
  sd_size = build_descriptor(sd, 0x0004, acl, acl_size);
  CHECK(import(sd, sd_size, 0x2000, 0x2000, &stored));
  CHECK(!allows(stored, kFileAddFile));
  CHECK(allows(stored, kFileAddSubdirectory));
  const Ace other_group[] = {{0, 0, 0x1F01FF, kOther, sizeof(kOther)}};
  acl_size = build_acl(acl, other_group, 1);
  sd_size = build_descriptor(sd, 0x0004, acl, acl_size);
  CHECK(import(sd, sd_size, 0x2000, 0x2000, &stored));
  CHECK(!allows(stored, 0x1));
  const Ace inherit_only[] = {{0, 0x08 | 0x03, 0x1F01FF, kWorld, sizeof(kWorld)}};
  acl_size = build_acl(acl, inherit_only, 1);
  sd_size = build_descriptor(sd, 0x0004, acl, acl_size);
  CHECK(import(sd, sd_size, 0x2000, 0x2000, &stored));
  CHECK(!allows(stored, 0x1));

  // No DACL and a NULL DACL grant everything; an empty DACL grants nothing.
  sd_size = build_descriptor(sd, 0, nullptr, 0);
  CHECK(import(sd, sd_size, 0x2000, 0x2000, &stored) && allows(stored, 0x1F01FF));
  sd_size = build_descriptor(sd, 0x0004, nullptr, 0);
  CHECK(import(sd, sd_size, 0x2000, 0x2000, &stored) && allows(stored, 0x1F01FF));
  acl_size = build_acl(acl, nullptr, 0);
  sd_size = build_descriptor(sd, 0x0004, acl, acl_size);
  CHECK(import(sd, sd_size, 0x2000, 0x2000, &stored) && !allows(stored, 0x1));

  // The same DACL as an absolute descriptor (pointers): header, then the ACL elsewhere in the same memory.
  {
    const Ace again[] = {{0, 0, 0x1200a9, kWorld, sizeof(kWorld)}};
    uint8_t memory[256] = {};
    const uint64_t base = 0x7000;
    acl_size = build_acl(memory + 64, again, 1);
    memory[0] = 1;
    memory[2] = 0x04;
    const uint64_t acl_at = base + 64;
    for (int k = 0; k < 8; ++k) memory[32 + k] = uint8_t(acl_at >> (8 * k));
    CHECK(import(memory, sizeof(memory), base, base, &stored));
    CHECK(allows(stored, 0x1) && !allows(stored, kFileAddFile));
  }

  // Refused: a wrong revision, a descriptor at nothing, an ACL that is too large, an ACE that does not fit.
  uint8_t bad[256];
  acl_size = build_acl(acl, read_only, 1);
  sd_size = build_descriptor(bad, 0x0004, acl, acl_size);
  bad[0] = 2;
  CHECK(!import(bad, sd_size, 0x1000, 0x1000, &stored));
  sd_size = build_descriptor(bad, 0x0004, acl, acl_size);
  CHECK(!import(bad, sd_size, 0, 0x1000, &stored));
  bad[20 + 2] = 0xFF; bad[20 + 3] = 0xFF;      // AclSize 65535
  CHECK(!import(bad, sd_size, 0x1000, 0x1000, &stored));
  sd_size = build_descriptor(bad, 0x0004, acl, acl_size);
  bad[20 + 8 + 2] = 0xF0;      // the ACE says it is larger than the ACL
  CHECK(!import(bad, sd_size, 0x1000, 0x1000, &stored));

  // Truncation: every shorter memory either imports (it still has everything that is read) or is refused; and every single flipped bit does one of the two, whatever it does to
  // the decision afterwards.
  acl_size = build_acl(acl, denied_first, 2);
  sd_size = build_descriptor(sd, 0x0004, acl, acl_size);
  for (uint32_t cut = 0; cut < sd_size; ++cut) {
    const bool ok = import(sd, cut, 0x1000, 0x1000, &stored);
    CHECK(!ok);
  }
  for (uint32_t bit = 0; bit < sd_size * 8; ++bit) {
    uint8_t copy[256];
    memcpy(copy, sd, sd_size);
    copy[bit / 8] ^= uint8_t(1u << (bit % 8));
    if (import(copy, sd_size, 0x1000, 0x1000, &stored)) { (void)allows(stored, 0x2); (void)allows(stored, 0x80000000u); }
  }
  // A stored descriptor that is cut short or flipped is decided on without reading outside it.
  Stored mangled;
  CHECK(import(sd, sd_size, 0x1000, 0x1000, &mangled));
  for (uint32_t cut = 0; cut < mangled.size; ++cut) { Stored part = mangled; part.size = cut; (void)allows(part, 0x2); }
  for (uint32_t bit = 0; bit < mangled.size * 8; ++bit) { Stored flipped = mangled; flipped.bytes[bit / 8] ^= uint8_t(1u << (bit % 8)); (void)allows(flipped, 0x2); }

  if (g_failed == 0) printf("FileSecurityTest: all passed\n");
  return g_failed ? 1 : 0;
}
