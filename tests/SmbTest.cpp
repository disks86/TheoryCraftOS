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

// Host-side tests for the SMB client: NTLMv2 against the worked example of MS-NLMP, the NTLM messages, and the pieces of the SMB2
// protocol (message framing, signing, name conversion, share list decoding). Against a real server: tests/smb_live.sh.
#include <stdio.h>
#include <string.h>
#include <string>
#include <vector>
#include "Network/Smb/Ntlm.hpp"
#include "Network/Smb/SmbFileSystem.hpp"

using namespace network;

struct TestAllocator final : allocators::IAllocator {
  int live = 0;
  void* allocate(uint64_t size) override { if (!size) return nullptr; ++live; return malloc(size); }
  void* allocate_aligned(uint64_t size, uint64_t) override { return allocate(size); }
  void deallocate(void* p) override { if (p) { --live; free(p); } }
};

static int g_failed = 0;
#define CHECK(...) do { if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)

static std::vector<uint8_t> unhex(const char* s) {
  std::vector<uint8_t> v;
  for (size_t i = 0; s[i] && s[i + 1]; i += 2) {
    auto nib = [](char c) { return c <= '9' ? c - '0' : c - 'a' + 10; };
    v.push_back(static_cast<uint8_t>(nib(s[i]) * 16 + nib(s[i + 1])));
  }
  return v;
}
static std::string hex(const uint8_t* p, size_t n) {
  std::string s;
  for (size_t i = 0; i < n; ++i) { char b[3]; snprintf(b, sizeof b, "%02x", p[i]); s += b; }
  return s;
}
static ConstByteView view(const std::vector<uint8_t>& v) { return ConstByteView(v.data(), v.size()); }

// MS-NLMP 4.2.4: User "User", Domain "Domain", Password "Password".
static void test_ntlm_vectors() {
  TestAllocator a;
  {
    NtlmCredentials cred;
    cred.user = "User";
    cred.domain = "Domain";
    cred.password = "Password";
    uint8_t ntowf[16];
    CHECK(NtlmClient::ntowf_v2(a, cred, ntowf));
    CHECK(hex(ntowf, 16) == "0c868a403bfd7a93a3001ef22ef02e3f");
    const std::vector<uint8_t> server_challenge = unhex("0123456789abcdef"), client_challenge = unhex("aaaaaaaaaaaaaaaa");
    const std::vector<uint8_t> info = unhex("02000c0044006f006d00610069006e0001000c005300650072007600650072000000000000000000");   // (Domain, Server, end, and the 4 zero bytes of the blob)
    // The target info in the example is the three pairs; the response appends the final zero bytes itself.
    const std::vector<uint8_t> pairs(info.begin(), info.begin() + 36);
    ByteBuffer response(a);
    uint8_t base[16];
    CHECK(NtlmClient::ntlm_v2_response(a, ntowf, server_challenge.data(), client_challenge.data(), 0, view(pairs), &response, base));
    CHECK(response.size() > 16 && hex(response.view().data(), 16) == "68cd0ab851e51c96aabc927bebef6a1c");
    CHECK(hex(base, 16) == "8de40ccadbc14a82f15cb0ad0de95ca3");
  }
  CHECK(a.live == 0);
}

// A random source with a fixed pattern.
struct FixedRandom final : crypto::IRandom {
  uint8_t next = 0x40;
  bool fill(ByteView out) override { for (uint8_t& b : out) b = next++; return true; }
};

static std::vector<uint8_t> le16(uint32_t v) { return {uint8_t(v), uint8_t(v >> 8)}; }

// A CHALLENGE_MESSAGE the way a server builds one: flags, challenge, target name and target info (domain, server, time).
static std::vector<uint8_t> make_challenge(bool with_time) {
  std::vector<uint8_t> info;
  auto pair = [&](uint32_t id, const std::vector<uint8_t>& value) {
    auto a = le16(id), b = le16(value.size());
    info.insert(info.end(), a.begin(), a.end());
    info.insert(info.end(), b.begin(), b.end());
    info.insert(info.end(), value.begin(), value.end());
  };
  pair(2, unhex("44006f006d00610069006e00"));            // domain "Domain"
  pair(1, unhex("5300650072007600650072"  "00"));       // server name
  if (with_time) pair(7, unhex("0011223344556677"));     // time
  pair(0, {});
  std::vector<uint8_t> m = {'N', 'T', 'L', 'M', 'S', 'S', 'P', 0, 2, 0, 0, 0};
  auto push32 = [&](uint32_t v) { for (int i = 0; i < 4; ++i) m.push_back(uint8_t(v >> (8 * i))); };
  push32(0); push32(56);                                  // target name: none, at 56
  push32(0xE2898215u);                                    // flags (unicode, ntlm, target info, extended security, 128, 56, ...)
  for (int i = 0; i < 8; ++i) m.push_back(uint8_t(0x10 + i));   // the server's challenge
  for (int i = 0; i < 8; ++i) m.push_back(0);
  push32(uint32_t(info.size()) << 16); push32(56);        // target info (length twice in the low 32 bits: set below)
  m[40] = uint8_t(info.size()); m[41] = uint8_t(info.size() >> 8); m[42] = uint8_t(info.size()); m[43] = uint8_t(info.size() >> 8);
  for (int i = 0; i < 8; ++i) m.push_back(0);             // version
  m.insert(m.end(), info.begin(), info.end());
  return m;
}

static uint32_t at32(const ByteBuffer& b, size_t at) { const uint8_t* p = b.view().data() + at; return p[0] | (p[1] << 8) | (p[2] << 16) | (uint32_t(p[3]) << 24); }

static void test_ntlm_messages() {
  TestAllocator a;
  {
    NtlmClient ntlm(a);
    ByteBuffer negotiate(a);
    CHECK(ntlm.negotiate(&negotiate) && negotiate.size() == 32 && memcmp(negotiate.view().data(), "NTLMSSP", 8) == 0 && at32(negotiate, 8) == 1);
    const uint32_t flags = at32(negotiate, 12);
    CHECK((flags & 0x1) && (flags & 0x200) && (flags & 0x80000) && (flags & 0x800000));   // unicode, ntlm, extended session security, target info
    FixedRandom random;
    NtlmCredentials cred;
    cred.user = "alice"; cred.domain = "WORKGROUP"; cred.password = "secret";
    // With a time in the target info: LM response of zeros, MIC present and right, key set.
    {
      const std::vector<uint8_t> challenge = make_challenge(true);
      ByteBuffer auth(a);
      CHECK(ntlm.authenticate(view(challenge), cred, "TCOS", 12345, random, &auth));
      CHECK(auth.size() > 88 && memcmp(auth.view().data(), "NTLMSSP", 8) == 0 && at32(auth, 8) == 3 && ntlm.has_session_key());
      const uint32_t lm_length = auth.view()[12] | (auth.view()[13] << 8), nt_length = auth.view()[20] | (auth.view()[21] << 8), user_length = auth.view()[36] | (auth.view()[37] << 8);
      CHECK(lm_length == 24 && nt_length > 16 + 28 && user_length == 10);
      // The MIC covers all three messages with its own field zeroed.
      ByteBuffer all(a), zeroed(a);
      zeroed.append(auth.view());
      for (int i = 0; i < 16; ++i) const_cast<uint8_t*>(zeroed.view().data())[72 + i] = 0;
      all.append(negotiate.view());
      all.append(view(challenge));
      all.append(zeroed.view());
      uint8_t mic[16];
      crypto::hmac(crypto::HashKind::Md5, ConstByteView(ntlm.session_key(), 16), all.view(), mic);
      CHECK(memcmp(mic, auth.view().data() + 72, 16) == 0);
      // The target info in the response carries the "MIC present" flag.
      const uint8_t flag_pair[8] = {6, 0, 4, 0, 2, 0, 0, 0};
      bool found = false;
      for (size_t i = 0; i + 8 <= auth.size(); ++i) found |= memcmp(auth.view().data() + i, flag_pair, 8) == 0;
      CHECK(found);
    }
    // Without a time: the LMv2 response is used, and there is no MIC.
    {
      const std::vector<uint8_t> challenge = make_challenge(false);
      ByteBuffer auth(a);
      CHECK(ntlm.authenticate(view(challenge), cred, "TCOS", 12345, random, &auth) && ntlm.has_session_key());
      CHECK((auth.view()[12] | (auth.view()[13] << 8)) == 24 && auth.view()[72] == 0 && auth.view()[73] == 0);
      bool lm_is_zero = true;
      const uint32_t lm_offset = at32(auth, 16);
      for (int i = 0; i < 24; ++i) lm_is_zero &= auth.view()[lm_offset + i] == 0;
      CHECK(!lm_is_zero);
    }
    // Anonymous: a one byte LM response, nothing else, no key.
    {
      NtlmCredentials none;
      const std::vector<uint8_t> challenge = make_challenge(true);
      ByteBuffer auth(a);
      CHECK(none.anonymous() && ntlm.authenticate(view(challenge), none, "TCOS", 0, random, &auth) && !ntlm.has_session_key());
      CHECK((auth.view()[12] | (auth.view()[13] << 8)) == 1 && (auth.view()[20] | (auth.view()[21] << 8)) == 0 && (at32(auth, 60) & 0x800));
    }
    // Damaged challenges are refused.
    {
      ByteBuffer auth(a);
      std::vector<uint8_t> bad = make_challenge(true);
      bad[8] = 5;
      CHECK(!ntlm.authenticate(view(bad), cred, "TCOS", 0, random, &auth));
      bad = make_challenge(true);
      bad.resize(30);
      CHECK(!ntlm.authenticate(view(bad), cred, "TCOS", 0, random, &auth));
      bad = make_challenge(true);
      bad[44] = 0xFF;   // target info offset past the end
      CHECK(!ntlm.authenticate(view(bad), cred, "TCOS", 0, random, &auth));
      bad = make_challenge(true);
      bad[22] &= ~0x1;  // the server does not do unicode
      bad[20] &= ~0x1;
      CHECK(!ntlm.authenticate(view(bad), cred, "TCOS", 0, random, &auth));
    }
  }
  CHECK(a.live == 0);
}

// NT status codes are told apart the way a filesystem user needs.
static void test_status_mapping() {
  using filesystem::Status;
  CHECK(SmbConnection::map_status(0) == Status::Ok);
  CHECK(SmbConnection::map_status(0xC0000034) == Status::NotFound && SmbConnection::map_status(0xC000003A) == Status::NotFound && SmbConnection::map_status(0xC00000CC) == Status::NotFound);
  CHECK(SmbConnection::map_status(0xC0000035) == Status::AlreadyExists && SmbConnection::map_status(0xC0000101) == Status::NotEmpty);
  CHECK(SmbConnection::map_status(0xC0000103) == Status::NotADirectory && SmbConnection::map_status(0xC00000BA) == Status::IsADirectory);
  CHECK(SmbConnection::map_status(0xC0000022) == Status::AccessDenied && SmbConnection::map_status(0xC000006D) == Status::LoginFailed);
  CHECK(SmbConnection::map_status(0xC0000043) == Status::Busy && SmbConnection::map_status(0xC000007F) == Status::NoSpace && SmbConnection::map_status(0xC00000A2) == Status::ReadOnly);
  CHECK(SmbConnection::map_status(0xC0000203) == Status::Network && SmbConnection::map_status(0x80000006) == Status::EndOfDirectory);
  CHECK(SmbConnection::map_status(0xDEADBEEF) == Status::IoError);
}

// A network that cannot reach anything: the parts of the filesystem that need no server.
struct NoNetwork final : ISmbNetwork {
  int attempts = 0;
  filesystem::Status connect(StringView, uint16_t, uint32_t, SharedPointer<ITcpSocket>*, INetworkStack**) override {
    ++attempts;
    return filesystem::Status::Network;
  }
};

static void test_filesystem_paths() {
  using namespace filesystem;
  TestAllocator a;
  {
    NoNetwork net;
    SmbEnvironment env;
    FixedRandom random;
    env.random = &random;
    SmbFileSystem fs(net, a, env);
    DirectoryEntry e;
    UniquePointer<IDirectory> dir;
    UniquePointer<IFile> file;
    CHECK(fs.stat("/", &e) == Status::Ok && e.type == NodeType::Directory && e.name.empty());
    CHECK(fs.stat("/srv", &e) == Status::Ok && e.name.equals("srv"));
    CHECK(fs.open_directory("/", &dir) == Status::Ok && dir->next(&e) == Status::EndOfDirectory);   // nobody visited yet
    CHECK(fs.open_directory("/srv", &dir) == Status::Network && net.attempts == 1);
    CHECK(fs.open_directory("/srv/share/x", &dir) == Status::Network);
    CHECK(fs.open_file("/srv/share/x.txt", OpenMode::Read, &file) == Status::Network);
    CHECK(fs.stat("/srv/share/x", &e) == Status::Network);
    CHECK(fs.open_file("/srv", OpenMode::Read, &file) == Status::IsADirectory && fs.open_file("/", OpenMode::Read, &file) == Status::IsADirectory);
    CHECK(fs.open_file("/srv/share", OpenMode::Read, &file) == Status::IsADirectory);
    CHECK(fs.open_directory("/srv/../x", &dir) == Status::InvalidPath && fs.stat("/srv/share/a\\b", &e) == Status::InvalidPath && fs.stat("/srv/sh:are", &e) == Status::InvalidPath);
    CHECK(fs.create_directory("/") == Status::AlreadyExists && fs.create_directory("/srv") == Status::Unsupported && fs.create_directory("/srv/share") == Status::AlreadyExists);
    CHECK(fs.remove("/") == Status::Unsupported && fs.remove("/srv") == Status::Unsupported && fs.remove("/srv/share") == Status::Unsupported);
    CHECK(fs.rename("/srv/a/x", "/srv/b/x") == Status::Unsupported && fs.rename("/srv", "/other") == Status::Unsupported);
    CHECK(fs.rename("/s1/share/a", "/s2/share/a") == Status::Unsupported);   // between servers is a copy
    // A visited server stays in the root listing; credentials with a domain are accepted in both spellings.
    fs.set_credentials("srv", "DOMAIN\\alice", "pw");
    fs.set_credentials("other:4455", "bob@example.org", "pw");
    CHECK(fs.open_directory("/", &dir) == Status::Ok);
    int count = 0;
    while (dir->next(&e) == Status::Ok) ++count;
    CHECK(count == 2);
    fs.forget("other:4455");
    CHECK(fs.open_directory("/", &dir) == Status::Ok);
    count = 0;
    while (dir->next(&e) == Status::Ok) ++count;
    CHECK(count == 1);
    dir.reset();
    CHECK(fs.info().type_name.equals("smb") && !fs.info().read_only);
  }
  CHECK(a.live == 0);
}

int main() {
  test_ntlm_vectors();
  test_ntlm_messages();
  test_status_mapping();
  test_filesystem_paths();
  if (g_failed) { printf("%d check(s) failed\n", g_failed); return 1; }
  printf("smb tests passed\n");
  return 0;
}
