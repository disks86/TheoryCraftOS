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

// Host-side tests for IStream and its implementations: MemoryStream, FileStream
// (over a RamFileSystem) and TcpStream (over two real network stacks on a fake wire).
#include <stdio.h>
#include <string>
#include "NetworkHarness.hpp"
#include "FileSystem/Ram/RamFileSystem.hpp"
#include "Stream/File/FileStream.hpp"
#include "Stream/Memory/MemoryStream.hpp"
#include "Stream/StreamHelpers.hpp"
#include "Stream/Tcp/TcpStream.hpp"

using namespace stream;

static int g_failed = 0;
#define CHECK(cond) do { if (!(cond)) { printf("FAILED line %d: %s\n", __LINE__, #cond); ++g_failed; } } while (0)
#define CHECK_STATUS(expr, want) do { StreamStatus got_ = (expr); if (got_ != (want)) { printf("FAILED line %d: %s -> %s, want %s\n", __LINE__, #expr, stream_status_name(got_), stream_status_name(want)); ++g_failed; } } while (0)

static ConstByteView bytes_of(const char* s) { return ConstByteView(reinterpret_cast<const uint8_t*>(s), strlen(s)); }

// Everything in the stream from its current position, as a std::string.
static std::string drain(IStream& s) {
  std::string out; uint8_t buf[7]; uint64_t got;
  while (s.read(ByteView(buf), &got) == StreamStatus::Ok && got) out.append(reinterpret_cast<char*>(buf), got);
  return out;
}

static void test_memory() {
  const long live_before = g_live;
  {
    TestAllocator a;
    MemoryStream m(a);
    CHECK(m.can_read() && m.can_write() && m.can_seek() && !m.can_timeout() && !m.is_closed());
    uint64_t n = 99, len = 99, pos = 99;
    CHECK_STATUS(m.read(ByteView(), &n), StreamStatus::Ok); CHECK(n == 0);
    uint8_t buf[16];
    CHECK_STATUS(m.read(ByteView(buf), &n), StreamStatus::Ok); CHECK(n == 0);  // empty stream: end at once
    CHECK_STATUS(write_text(m, "hello world"), StreamStatus::Ok);
    CHECK_STATUS(m.length(&len), StreamStatus::Ok); CHECK_STATUS(m.position(&pos), StreamStatus::Ok);
    CHECK(len == 11 && pos == 11);
    CHECK_STATUS(m.seek(0, SeekOrigin::Begin, &pos), StreamStatus::Ok); CHECK(pos == 0);
    CHECK_STATUS(m.read(ByteView(buf, 5), &n), StreamStatus::Ok); CHECK(n == 5 && !memcmp(buf, "hello", 5));
    CHECK_STATUS(m.seek(1, SeekOrigin::Current, nullptr), StreamStatus::Ok);
    CHECK(drain(m) == "world");
    CHECK_STATUS(m.seek(-5, SeekOrigin::End, &pos), StreamStatus::Ok); CHECK(pos == 6);
    CHECK_STATUS(write_text(m, "WORLD!"), StreamStatus::Ok);  // overwrite and extend
    CHECK(m.contents().size() == 12 && !memcmp(m.contents().data(), "hello WORLD!", 12));

    // Errors in seeking.
    CHECK_STATUS(m.seek(-1, SeekOrigin::Begin, &pos), StreamStatus::InvalidArgument);
    CHECK_STATUS(m.seek(-13, SeekOrigin::End, &pos), StreamStatus::InvalidArgument);
    CHECK_STATUS(m.seek(INT64_MAX, SeekOrigin::Begin, nullptr), StreamStatus::Ok);
    CHECK_STATUS(m.seek(INT64_MAX, SeekOrigin::Current, nullptr), StreamStatus::Ok);
    CHECK_STATUS(m.seek(INT64_MAX, SeekOrigin::Current, &pos), StreamStatus::InvalidArgument);  // would wrap around
    CHECK_STATUS(m.seek(12, SeekOrigin::Begin, nullptr), StreamStatus::Ok);
    CHECK_STATUS(m.position(&pos), StreamStatus::Ok); CHECK(pos == 12);

    // Seek past the end, write: the gap is zeros.
    CHECK_STATUS(m.seek(20, SeekOrigin::Begin, nullptr), StreamStatus::Ok);
    CHECK_STATUS(m.read(ByteView(buf), &n), StreamStatus::Ok); CHECK(n == 0);
    CHECK_STATUS(write_byte(m, 'X'), StreamStatus::Ok);
    CHECK(m.contents().size() == 21 && m.contents()[12] == 0 && m.contents()[19] == 0 && m.contents()[20] == 'X');

    // set_length: shrink, grow (zeros, not stale bytes).
    CHECK_STATUS(m.set_length(5), StreamStatus::Ok);
    CHECK_STATUS(m.set_length(8), StreamStatus::Ok);
    CHECK(!memcmp(m.contents().data(), "hello\0\0\0", 8));
    CHECK_STATUS(m.set_length(0), StreamStatus::Ok);
    m.seek(0, SeekOrigin::Begin, nullptr);

    // Many small writes and a large one (geometric growth).
    for (int i = 0; i < 1000; ++i) CHECK_STATUS(write_byte(m, static_cast<uint8_t>(i)), StreamStatus::Ok);
    CHECK(m.contents().size() == 1000 && m.contents()[999] == (999 & 255) && m.capacity() >= 1000);
    uint8_t big[100000]; memset(big, 7, sizeof big);
    CHECK_STATUS(m.write(ConstByteView(big)), StreamStatus::Ok);
    CHECK(m.contents().size() == 101000);
    m.reset();
    CHECK(m.contents().empty());

    // read_exactly / read_byte / copy_to.
    MemoryStream src(bytes_of("0123456789"));
    uint8_t four[4];
    CHECK_STATUS(read_exactly(src, ByteView(four)), StreamStatus::Ok); CHECK(!memcmp(four, "0123", 4));
    uint8_t one;
    CHECK_STATUS(read_byte(src, &one), StreamStatus::Ok); CHECK(one == '4');
    uint8_t eight[8];
    CHECK_STATUS(read_exactly(src, ByteView(eight)), StreamStatus::EndOfStream);  // only 5 left
    MemoryStream src2(bytes_of("copy me please"));
    uint64_t copied = 0;
    CHECK_STATUS(copy_to(src2, m, &copied), StreamStatus::Ok); CHECK(copied == 14 && m.contents().size() == 14);

    // Closing.
    CHECK_STATUS(m.close(), StreamStatus::Ok);
    CHECK(m.is_closed() && !m.can_read() && !m.can_write() && !m.can_seek());
    CHECK_STATUS(m.read(ByteView(buf), &n), StreamStatus::Closed); CHECK(n == 0);
    CHECK_STATUS(write_byte(m, 1), StreamStatus::Closed);
    CHECK_STATUS(m.seek(0, SeekOrigin::Begin, nullptr), StreamStatus::Closed);
    CHECK_STATUS(m.length(&len), StreamStatus::Closed);
    CHECK_STATUS(m.close(), StreamStatus::Ok);  // twice is fine
  }
  CHECK(g_live == live_before);

  // A fixed buffer: never grows, writes are all-or-nothing.
  {
    uint8_t storage[8];
    MemoryStream m{ByteView(storage), 0, true};
    CHECK_STATUS(write_text(m, "12345"), StreamStatus::Ok);
    CHECK_STATUS(write_text(m, "6789"), StreamStatus::NoSpace);   // would need 9
    CHECK(m.contents().size() == 5);
    CHECK_STATUS(write_text(m, "678"), StreamStatus::Ok);
    CHECK_STATUS(write_byte(m, '9'), StreamStatus::NoSpace);
    CHECK(!memcmp(storage, "12345678", 8));
    CHECK_STATUS(m.set_length(9), StreamStatus::NoSpace);
    CHECK_STATUS(m.reserve(9), StreamStatus::NoSpace);
    CHECK_STATUS(m.set_length(2), StreamStatus::Ok);
    m.seek(0, SeekOrigin::Begin, nullptr);
    CHECK(drain(m) == "12");
    MemoryStream with_data{ByteView(storage), 8, true};  // existing contents
    CHECK(drain(with_data) == "12345678");
  }
  // Read-only over existing data.
  {
    MemoryStream m(bytes_of("const"));
    CHECK(m.can_read() && !m.can_write());
    CHECK_STATUS(write_byte(m, 1), StreamStatus::ReadOnly);
    CHECK_STATUS(m.set_length(1), StreamStatus::ReadOnly);
    CHECK(drain(m) == "const");
  }
  // Out of memory leaves the stream as it was.
  {
    TestAllocator a;
    MemoryStream m(a, 4);
    CHECK_STATUS(write_text(m, "abcd"), StreamStatus::Ok);
    // (the harness allocator cannot be told to fail, so use a failing one)
    struct Failing final : allocators::IAllocator {
      void* allocate(uint64_t) override { return nullptr; }
      void* allocate_aligned(uint64_t, uint64_t) override { return nullptr; }
      void deallocate(void*) override {}
    } failing;
    MemoryStream f(failing, 16);
    CHECK(f.capacity() == 0);
    CHECK_STATUS(write_byte(f, 1), StreamStatus::OutOfMemory);
    CHECK(f.contents().empty());
    CHECK_STATUS(m.write(ConstByteView(bytes_of("e"))), StreamStatus::Ok);  // growing with a working allocator is fine
  }
  CHECK(g_live == live_before);
  // Base-class defaults: a stream that is nothing (via a closed MemoryStream's NotSupported paths) - checked through TcpStream below.
}

static void test_file() {
  TestAllocator a;
  {
    filesystem::RamFileSystem fs(a, 1000);
    UniquePointer<IStream> f;
    CHECK(FileStream::open(a, fs, "/nope", filesystem::OpenMode::Read, &f) == filesystem::Status::NotFound && !f);
    CHECK(FileStream::open(a, fs, "/log.txt", filesystem::OpenMode::CreateOrOpen, &f) == filesystem::Status::Ok && f);
    CHECK(f->can_read() && f->can_write() && f->can_seek() && !f->can_timeout());
    CHECK_STATUS(write_text(*f, "line one\n"), StreamStatus::Ok);
    CHECK_STATUS(write_text(*f, "line two\n"), StreamStatus::Ok);
    uint64_t len = 0, pos = 0;
    CHECK_STATUS(f->length(&len), StreamStatus::Ok); CHECK_STATUS(f->position(&pos), StreamStatus::Ok);
    CHECK(len == 18 && pos == 18);
    CHECK_STATUS(f->seek(5, SeekOrigin::Begin, nullptr), StreamStatus::Ok);
    uint8_t buf[8]; uint64_t n;
    CHECK_STATUS(read_exactly(*f, ByteView(buf, 3)), StreamStatus::Ok); CHECK(!memcmp(buf, "one", 3));
    CHECK_STATUS(f->seek(0, SeekOrigin::End, nullptr), StreamStatus::Ok);       // append
    CHECK_STATUS(write_text(*f, "three\n"), StreamStatus::Ok);
    f->seek(0, SeekOrigin::Begin, nullptr);
    CHECK(drain(*f) == "line one\nline two\nthree\n");
    CHECK_STATUS(f->seek(-1, SeekOrigin::Begin, nullptr), StreamStatus::InvalidArgument);
    CHECK_STATUS(f->flush(), StreamStatus::Ok);
    CHECK_STATUS(f->set_length(4), StreamStatus::Ok);
    f->seek(0, SeekOrigin::Begin, nullptr);
    CHECK(drain(*f) == "line");
    // Sparse write past the end: zero gap.
    CHECK_STATUS(f->seek(8, SeekOrigin::Begin, nullptr), StreamStatus::Ok);
    CHECK_STATUS(write_byte(*f, '!'), StreamStatus::Ok);
    f->seek(0, SeekOrigin::Begin, nullptr);
    std::string all = drain(*f);
    CHECK(all.size() == 9 && all.substr(0, 4) == "line" && all[4] == 0 && all[7] == 0 && all[8] == '!');
    // Size limit becomes NoSpace.
    f->seek(0, SeekOrigin::End, nullptr);
    uint8_t big[2000] = {};
    CHECK_STATUS(f->write(ConstByteView(big)), StreamStatus::NoSpace);
    CHECK_STATUS(f->close(), StreamStatus::Ok);
    CHECK(f->is_closed());
    CHECK_STATUS(f->read(ByteView(buf), &n), StreamStatus::Closed);
    CHECK_STATUS(write_byte(*f, 1), StreamStatus::Closed);
    CHECK_STATUS(f->close(), StreamStatus::Ok);
    // The file is closed, so the filesystem can now remove it.
    CHECK(fs.remove("/log.txt") == filesystem::Status::Ok);

    // Read-only streams.
    UniquePointer<IStream> w;
    CHECK(FileStream::open(a, fs, "/r", filesystem::OpenMode::CreateOrOpen, &w) == filesystem::Status::Ok);
    write_text(*w, "abc"); w.reset();
    CHECK(FileStream::open(a, fs, "/r", filesystem::OpenMode::Read, &f) == filesystem::Status::Ok);
    CHECK(f->can_read() && !f->can_write());
    CHECK_STATUS(write_byte(*f, 1), StreamStatus::ReadOnly);
    CHECK_STATUS(f->set_length(0), StreamStatus::ReadOnly);
    CHECK(drain(*f) == "abc");
    // Truncating open, and destroying a stream closes the file.
    f.reset();
    CHECK(FileStream::open(a, fs, "/r", filesystem::OpenMode::CreateTruncate, &f) == filesystem::Status::Ok);
    uint64_t length = 1; f->length(&length); CHECK(length == 0);
    f.reset();
    CHECK(fs.remove("/r") == filesystem::Status::Ok);
  }
  CHECK(g_live == 0);
}

// ---- TCP: a real client stack talking to a scripted server on the other stack.
struct Server {
  Lan& lan;
  std::function<void(Server&)> app;       // what the server does each time the client waits
  SharedPointer<ITcpSocket> listener, conn;
  std::string received;
  bool running = false;
  Server(Lan& l, uint16_t port) : lan(l) {
    listener = lan.b.tcp_open(); assert(listener->listen(port));
    auto previous = lan.nic_a.on_idle;
    lan.nic_a.on_idle = [this, previous] { if (previous) previous(); if (running) return; running = true; step(); running = false; };
  }
  void step() {
    if (!conn) conn = listener->accept();
    if (conn) { uint8_t b[1024]; for (uint32_t n; (n = conn->receive(ByteView(b))) > 0;) received.append(reinterpret_cast<char*>(b), n); }
    if (app) app(*this);
  }
  void send_all(const char* s) { ConstByteView v = bytes_of(s); while (!v.empty()) { uint32_t k = conn->send(v); v = v.subview(k); if (!k) lan.step(); } }
};

static void test_tcp() {
  const Ipv4Address server_ip = Ipv4Address::make(10, 0, 0, 2);
  {  // echo, bulk data, close
    Lan lan; lan.configure_static();
    Server server(lan, 7);
    server.app = [](Server& s) {
      if (!s.conn) return;
      static size_t echoed = 0;
      for (; echoed < s.received.size();) {  // echo what has arrived, but only for the first 5 bytes (the bulk part is a sink)
        if (echoed >= 5) break;
        uint32_t k = s.conn->send(ConstByteView(reinterpret_cast<const uint8_t*>(s.received.data()) + echoed, 5 - echoed));
        if (!k) break;
        echoed += k;
      }
    };
    UniquePointer<IStream> c;
    CHECK_STATUS(TcpStream::connect(lan.a, lan.alloc, server_ip, 7, 5000, &c), StreamStatus::Ok);
    CHECK(c && c->can_read() && c->can_write() && !c->can_seek() && c->can_timeout());
    uint64_t len;
    CHECK_STATUS(c->length(&len), StreamStatus::NotSupported);
    CHECK_STATUS(c->seek(0, SeekOrigin::Begin, nullptr), StreamStatus::NotSupported);
    CHECK_STATUS(c->set_length(0), StreamStatus::NotSupported);
    CHECK_STATUS(write_text(*c, "hello"), StreamStatus::Ok);
    CHECK_STATUS(c->flush(), StreamStatus::Ok);
    uint8_t back[5];
    CHECK_STATUS(read_exactly(*c, ByteView(back)), StreamStatus::Ok); CHECK(!memcmp(back, "hello", 5));
    std::vector<uint8_t> bulk(100000);
    for (size_t i = 0; i < bulk.size(); ++i) bulk[i] = static_cast<uint8_t>(i * 13 + (i >> 8));
    CHECK_STATUS(c->write(ConstByteView(bulk.data(), bulk.size())), StreamStatus::Ok);  // far more than the 16 KiB send buffer
    CHECK_STATUS(c->flush(), StreamStatus::Ok);
    CHECK(server.received.size() == 5 + bulk.size() && !memcmp(server.received.data() + 5, bulk.data(), bulk.size()));
    CHECK_STATUS(c->close(), StreamStatus::Ok);
    CHECK(c->is_closed());
    uint64_t n;
    CHECK_STATUS(c->read(ByteView(back), &n), StreamStatus::Closed);
    CHECK_STATUS(write_byte(*c, 1), StreamStatus::Closed);
    CHECK_STATUS(c->close(), StreamStatus::Ok);
    lan.run_until([&] { return server.conn && server.conn->peer_closed(); }, 2000);
    CHECK(server.conn && server.conn->peer_closed());
  }
  {  // the server sends and closes: read sees data then the end; a read timeout when nothing comes
    Lan lan; lan.configure_static();
    Server server(lan, 7);
    bool sent = false;
    server.app = [&](Server& s) {
      if (s.conn && !sent) { sent = true; s.send_all("bye"); s.conn->close(); }
    };
    UniquePointer<IStream> c;
    CHECK_STATUS(TcpStream::connect(lan.a, lan.alloc, server_ip, 7, 5000, &c), StreamStatus::Ok);
    CHECK(drain(*c) == "bye");
    uint64_t n = 5; uint8_t b[4];
    CHECK_STATUS(c->read(ByteView(b), &n), StreamStatus::Ok); CHECK(n == 0);  // still the end
    CHECK_STATUS(write_text(*c, "late"), StreamStatus::Ok);                   // half-closed: we may still send
  }
  {  // timeouts
    Lan lan; lan.configure_static();
    Server server(lan, 7);  // accepts, never says anything
    UniquePointer<IStream> c;
    CHECK_STATUS(TcpStream::connect(lan.a, lan.alloc, server_ip, 7, 5000, &c), StreamStatus::Ok);
    CHECK_STATUS(c->set_read_timeout(50), StreamStatus::Ok);
    uint8_t b[4]; uint64_t n = 9;
    const uint64_t t0 = lan.a.now_ms();
    CHECK_STATUS(c->read(ByteView(b), &n), StreamStatus::TimedOut); CHECK(n == 0);
    const uint64_t waited = lan.a.now_ms() - t0;
    CHECK(waited >= 50 && waited < 1000);
    CHECK_STATUS(c->set_read_timeout(kInfiniteTimeout), StreamStatus::Ok);
    // Connecting to an address nobody answers for.
    UniquePointer<IStream> none;
    CHECK_STATUS(TcpStream::connect(lan.a, lan.alloc, Ipv4Address::make(10, 0, 0, 77), 7, 300, &none), StreamStatus::TimedOut);
    CHECK(!none);
  }
  {  // refused, and reset
    Lan lan; lan.configure_static();
    UniquePointer<IStream> c;
    CHECK_STATUS(TcpStream::connect(lan.a, lan.alloc, server_ip, 99, 5000, &c), StreamStatus::ConnectionLost);  // nobody listens
    Server server(lan, 7);
    bool reset = false;
    server.app = [&](Server& s) { if (s.conn && !reset && !s.received.empty()) { reset = true; s.conn->abort(); } };
    CHECK_STATUS(TcpStream::connect(lan.a, lan.alloc, server_ip, 7, 5000, &c), StreamStatus::Ok);
    uint8_t b[4]; uint64_t n;
    CHECK_STATUS(write_text(*c, "x"), StreamStatus::Ok);   // the server resets the connection when it sees this
    CHECK_STATUS(c->read(ByteView(b), &n), StreamStatus::ConnectionLost);
    CHECK_STATUS(write_text(*c, "x"), StreamStatus::ConnectionLost);
  }
  {  // a stream over an accepted socket (the server side), name resolution failure
    Lan lan; lan.configure_static();
    auto listener = lan.b.tcp_open(); assert(listener->listen(80));
    auto client = lan.a.tcp_open(); assert(client->connect(server_ip, 80));
    SharedPointer<ITcpSocket> accepted;
    lan.run_until([&] { if (!accepted) accepted = listener->accept(); return accepted && client->is_connected(); });
    TcpStream server_side(lan.b, accepted);
    CHECK(server_side.remote_port() == client->local_port());
    CHECK_STATUS(client->send(bytes_of("ping")) == 4 ? StreamStatus::Ok : StreamStatus::IoError, StreamStatus::Ok);
    uint8_t b[8]; uint64_t n = 0;
    CHECK_STATUS(server_side.read(ByteView(b), &n), StreamStatus::Ok); CHECK(n == 4 && !memcmp(b, "ping", 4));
    server_side.abort();
    CHECK(server_side.is_closed());
    UniquePointer<IStream> c;
    CHECK_STATUS(TcpStream::connect(lan.a, lan.alloc, "no-such-host.invalid", 80, 200, &c), StreamStatus::HostNotFound);
    CHECK_STATUS(TcpStream::connect(lan.a, lan.alloc, "10.0.0.2", 80, 2000, &c), StreamStatus::Ok);  // a dotted quad needs no lookup
  }
}

int main() {
  setvbuf(stdout, nullptr, _IONBF, 0);
  test_memory();
  test_file();
  test_tcp();
  if (g_failed) { printf("%d check(s) failed\n", g_failed); return 1; }
  printf("stream tests passed\n");
  return 0;
}
