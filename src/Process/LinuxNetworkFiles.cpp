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
#include "Process/LinuxFiles.hpp"
#include "FileSystem/IFile.hpp"
#include "FileSystem/IFileSystem.hpp"
#include "Process/Process.hpp"
#include "Process/ProcessFiles.hpp"
#include "Process/Sockets.hpp"
#include "Utility/ByteBuffer.hpp"
#include "Utility/String.hpp"

// The files a Linux program's C library reads to find hosts by name: /etc/resolv.conf (which DNS servers to ask), /etc/hosts, /etc/nsswitch.conf,
// /etc/services and /etc/protocols. They are ordinary files in the program's tree (/Linux/etc), written the first time a program names one of them, so
// that everything that can be done with a file works on them. A file that is there already is left alone, with one exception: a resolv.conf that this
// code wrote (its first line says so) is written again when the DNS servers of the network configuration are no longer the ones in it. A resolv.conf
// someone put there by hand, or one a program wrote itself, is theirs.
namespace process::linuxabi {

namespace {

using filesystem::OpenMode;
using filesystem::Status;

constexpr const char* kMarker = "# Written by TheoryCraftOS from the network configuration. A file without this line is left alone.\n";
constexpr uint32_t kMostServers = 3;      // (what a C library reads from a resolv.conf: MAXNS)

bool put(ByteBuffer& out, StringView text) { return out.append(text.bytes()); }

bool put_number(ByteBuffer& out, uint32_t value) {
  char digits[10];
  uint32_t n = 0;
  do digits[n++] = char('0' + value % 10); while ((value /= 10) != 0);
  while (n > 0)
    if (!out.put8(uint8_t(digits[--n]))) return false;
  return true;
}

bool put_ipv4(ByteBuffer& out, const network::Ipv4Address& address) {
  const uint32_t v = address.value();
  for (uint32_t i = 0; i < 4; ++i) {
    if (i != 0 && !out.put8('.')) return false;
    if (!put_number(out, (v >> (24 - 8 * i)) & 0xFF)) return false;
  }
  return true;
}

// Eight groups of hexadecimal digits, without the "::" short form (which a reader does not need).
bool put_ipv6(ByteBuffer& out, const network::Ipv6Address& address) {
  for (uint32_t group = 0; group < 8; ++group) {
    if (group != 0 && !out.put8(':')) return false;
    const uint32_t value = uint32_t{address.bytes[group * 2]} << 8 | address.bytes[group * 2 + 1];
    bool started = false;
    for (int shift = 12; shift >= 0; shift -= 4) {
      const uint32_t digit = (value >> shift) & 0xF;
      if (digit == 0 && !started && shift != 0) continue;
      started = true;
      if (!out.put8(uint8_t("0123456789abcdef"[digit]))) return false;
    }
  }
  return true;
}

// resolv.conf as the network configuration has it now: the DNS server of each interface that has one, IPv4 ones first. With no network (or no server)
// the file names none, and a C library then asks the machine itself, where nobody answers.
bool resolver_configuration(ByteBuffer& out) {
  if (!put(out, StringView(kMarker))) return false;
  SocketManager* sockets = system_sockets();
  uint32_t servers = 0;
  if (sockets) {
    network::IpConfiguration c;
    for (uint32_t i = 0; servers < kMostServers && sockets->interface_configuration(i, &c); ++i) {
      if (c.dns.is_unspecified()) continue;
      if (!put(out, "nameserver ") || !put_ipv4(out, c.dns) || !out.put8('\n')) return false;
      ++servers;
    }
    network::Ipv6Configuration c6;
    for (uint32_t i = 0; servers < kMostServers && sockets->interface_configuration6(i, &c6); ++i) {
      if (c6.dns.is_unspecified()) continue;
      if (!put(out, "nameserver ") || !put_ipv6(out, c6.dns) || !out.put8('\n')) return false;
      ++servers;
    }
  }
  return true;
}

constexpr const char* kHosts =
    "127.0.0.1 localhost\n"
    "127.0.1.1 tcos\n"
    "::1 localhost ip6-localhost ip6-loopback\n";

constexpr const char* kSwitch =
    "passwd: files\n"
    "group: files\n"
    "shadow: files\n"
    "hosts: files dns\n"
    "networks: files\n"
    "protocols: files\n"
    "services: files\n"
    "ethers: files\n"
    "rpc: files\n";

constexpr const char* kProtocols =
    "ip 0 IP\n"
    "icmp 1 ICMP\n"
    "igmp 2 IGMP\n"
    "tcp 6 TCP\n"
    "udp 17 UDP\n"
    "ipv6 41 IPv6\n"
    "ipv6-icmp 58 IPv6-ICMP\n"
    "sctp 132 SCTP\n";

constexpr const char* kServices =
    "ftp-data 20/tcp\n"
    "ftp 21/tcp\n"
    "ssh 22/tcp\n"
    "telnet 23/tcp\n"
    "smtp 25/tcp mail\n"
    "domain 53/tcp\n"
    "domain 53/udp\n"
    "bootps 67/udp\n"
    "bootpc 68/udp\n"
    "tftp 69/udp\n"
    "http 80/tcp www\n"
    "kerberos 88/tcp\n"
    "kerberos 88/udp\n"
    "pop3 110/tcp\n"
    "sunrpc 111/tcp\n"
    "sunrpc 111/udp\n"
    "nntp 119/tcp\n"
    "ntp 123/udp\n"
    "imap 143/tcp imap2\n"
    "snmp 161/udp\n"
    "ldap 389/tcp\n"
    "https 443/tcp\n"
    "https 443/udp\n"
    "microsoft-ds 445/tcp\n"
    "submissions 465/tcp\n"
    "syslog 514/udp\n"
    "submission 587/tcp\n"
    "ipp 631/tcp\n"
    "ldaps 636/tcp\n"
    "rsync 873/tcp\n"
    "imaps 993/tcp\n"
    "pop3s 995/tcp\n"
    "socks 1080/tcp\n"
    "openvpn 1194/udp\n"
    "mqtt 1883/tcp\n"
    "nfs 2049/tcp\n"
    "mysql 3306/tcp\n"
    "rdp 3389/tcp\n"
    "svn 3690/tcp\n"
    "mdns 5353/udp\n"
    "postgresql 5432/tcp\n"
    "xmpp-client 5222/tcp\n"
    "x11 6000/tcp\n"
    "http-alt 8080/tcp\n"
    "git 9418/tcp\n";

// The file's whole contents (up to `limit` bytes). False if it is not there or cannot be read.
bool read_whole(filesystem::IFileSystem& fs, StringView host_path, uint64_t limit, ByteBuffer* out) {
  UniquePointer<filesystem::IFile> file;
  if (fs.open_file(host_path, OpenMode::Read, &file) != Status::Ok || !file) return false;
  const uint64_t size = file->size() < limit ? file->size() : limit;
  out->clear();
  if (!out->put_zeros(size)) return false;
  uint64_t got = 0;
  if (size != 0 && (file->read(0, out->writable_view(), &got) != Status::Ok || got != size)) return false;
  return true;
}

void write_whole(filesystem::IFileSystem& fs, StringView host_path, ConstByteView contents) {
  UniquePointer<filesystem::IFile> file;
  if (fs.open_file(host_path, OpenMode::CreateTruncate, &file) != Status::Ok || !file) return;      // (a file system that cannot be written: the program finds no file)
  uint64_t wrote = 0;
  file->write(0, contents, &wrote);
}

bool same(ConstByteView a, ConstByteView b) {
  if (a.size() != b.size()) return false;
  for (uint64_t i = 0; i < a.size(); ++i)
    if (a[i] != b[i]) return false;
  return true;
}

}  // namespace

void provide_network_file(Process& p, StringView virtual_path) {
  constexpr StringView kFolder("/etc/");
  if (!virtual_path.starts_with(kFolder)) return;
  const StringView name = virtual_path.without_prefix(kFolder.size());
  const bool resolver = name.equals("resolv.conf");
  const char* fixed = name.equals("hosts") ? kHosts : name.equals("nsswitch.conf") ? kSwitch : name.equals("protocols") ? kProtocols : name.equals("services") ? kServices : nullptr;
  if (!resolver && !fixed) return;
  ProcessFiles& files = p.files();
  filesystem::IFileSystem* fs = files.file_system();
  if (!files.attached() || !fs) return;
  String host(files.allocator()), folder(files.allocator());
  if (!files.host_path_of(virtual_path, &host) || !files.host_path_of(StringView("/etc"), &folder)) return;
  filesystem::DirectoryEntry entry;
  const bool there = fs->stat(host.view(), &entry) == Status::Ok;
  if (there && !resolver) return;
  if (there && entry.type != filesystem::NodeType::File) return;
  ByteBuffer wanted(files.allocator());
  if (resolver) {
    if (!resolver_configuration(wanted)) return;
    if (there) {
      // Ours to keep up to date only if this code wrote it; and written again only when it would say something else.
      ByteBuffer now(files.allocator());
      if (!read_whole(*fs, host.view(), 4096, &now)) return;
      const StringView marker(kMarker);
      if (now.size() < marker.size() || !same(now.view().first(marker.size()), marker.bytes())) return;
      if (same(now.view(), wanted.view())) return;
    }
  } else if (!put(wanted, StringView(fixed))) {
    return;
  }
  if (fs->stat(folder.view(), &entry) != Status::Ok) fs->create_directory(folder.view());
  write_whole(*fs, host.view(), wanted.view());
}

}  // namespace process::linuxabi
