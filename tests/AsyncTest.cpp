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

// Host-side tests for Task, Executor, the awaitables, when_all, the async primitives
// and the async streams (memory, file, and TCP over two real stacks on a fake wire).
#include <stdio.h>
#include <map>
#include <string>
#include "NetworkHarness.hpp"
#include "Async/Combinators.hpp"
#include "FileSystem/Ram/RamFileSystem.hpp"
#include "Stream/AsyncStreamHelpers.hpp"
#include "Stream/File/FileStream.hpp"
#include "Stream/Memory/MemoryStream.hpp"
#include "Stream/Tcp/TcpStream.hpp"

using namespace async;
using namespace stream;

static int g_failed = 0;
#define CHECK(cond) do { if (!(cond)) { printf("FAILED line %d: %s\n", __LINE__, #cond); ++g_failed; } } while (0)
#define CHECK_STATUS(expr, want) do { StreamStatus got_ = (expr); if (got_ != (want)) { printf("FAILED line %d: %s -> %s, want %s\n", __LINE__, #expr, stream_status_name(got_), stream_status_name(want)); ++g_failed; } } while (0)

static ConstByteView bytes_of(const char* s) { return ConstByteView(reinterpret_cast<const uint8_t*>(s), strlen(s)); }

// An allocator for coroutine frames that can be told to fail.
struct FrameAllocator final : allocators::IAllocator {
  long live = 0; bool fail = false;
  void* allocate(uint64_t size) override { if (fail) return nullptr; ++live; return malloc(size); }
  void* allocate_aligned(uint64_t size, uint64_t) override { return allocate(size); }
  void deallocate(void* p) override { if (p) { --live; free(p); } }
};
static FrameAllocator g_frames;

// A clock that moves forward a little each time it is read, so loops that wait make progress.
struct StepClock final : hardware::IClock {
  mutable uint64_t us = 0;
  const char* name() const override { return "step clock"; } bool initialize() override { return true; } void shutdown() override {}
  uint64_t now_us() const override { us += 50; return us; }
};

static Task<int> add_later(int a, int b) { co_await yield_now(); co_return a + b; }
static Task<int> nested(int n) { int sum = 0; for (int i = 0; i < n; ++i) sum += co_await add_later(i, 1); co_return sum; }
static Task<void> set_flag(bool* flag) { co_await yield_now(); *flag = true; }
static Task<std::string> make_string() { co_return std::string("hello"); }

static void test_tasks() {
  StepClock clock; Executor ex(clock);
  CHECK(ex.block_on(add_later(2, 3)) == 5);
  CHECK(ex.block_on(nested(4)) == 1 + 2 + 3 + 4);
  bool flag = false;
  ex.block_on(set_flag(&flag));
  CHECK(flag);
  CHECK(ex.block_on(make_string()) == "hello");           // a non-trivial result type
  CHECK(ex.block_on(Task<int>::from_value(7)) == 7);        // a finished task needs no coroutine
  ex.block_on(Task<void>::completed());
  // A task never started frees its frame; a moved-from Task is empty.
  { Task<int> t = add_later(1, 1); Task<int> u = util::move(t); CHECK(!t.valid() && u.valid()); }
  CHECK(g_frames.live == 0);
  // Out of memory for the coroutine frame: invalid task, FailedResult.
  g_frames.fail = true;
  Task<int> bad = add_later(1, 1);
  CHECK(!bad.valid());
  CHECK(ex.block_on(util::move(bad)) == 0);
  CHECK(!ex.spawn(set_flag(&flag)));
  g_frames.fail = false;
  CHECK(g_frames.live == 0);
  // Awaiting an already-finished sub-task and an invalid one inside a task.
  struct Local { static Task<int> run() { int a = co_await Task<int>::from_value(4); co_return a * 2; } };
  CHECK(ex.block_on(Local::run()) == 8);
}

static std::string g_log;
static Task<void> ticker(char name, int rounds) { for (int i = 0; i < rounds; ++i) { g_log += name; co_await yield_now(); } }
static Task<void> sleeper(char name, uint32_t ms) { co_await delay(ms); g_log += name; }

static void test_executor() {
  StepClock clock; Executor ex(clock);
  g_log.clear();
  CHECK(ex.spawn(ticker('A', 3)) && ex.spawn(ticker('B', 3)));
  CHECK(ex.active_tasks() == 2);
  ex.run();
  CHECK(g_log == "ABABAB" && ex.active_tasks() == 0);

  g_log.clear();
  const uint64_t t0 = clock.us;
  ex.spawn(sleeper('a', 30)); ex.spawn(sleeper('b', 10)); ex.spawn(sleeper('c', 20));
  ex.run();
  CHECK(g_log == "bca");
  CHECK(clock.us - t0 >= 30000 && clock.us - t0 < 60000);

  // wait_until: becomes true; times out.
  struct W {
    static Task<void> run(int* counter, bool* ok, bool* timed_out) {
      *ok = co_await wait_until([counter] { return ++*counter >= 5; });
      *timed_out = !co_await wait_until([] { return false; }, 10);
    }
  };
  int counter = 0; bool ok = false, timed_out = false;
  ex.block_on(W::run(&counter, &ok, &timed_out));
  CHECK(ok && timed_out && counter == 5);

  // The condition is tried before suspending: a true one never yields to other tasks.
  g_log.clear();
  struct Fast {
    static Task<void> run() { g_log += '1'; co_await wait_until([] { return true; }); g_log += '2'; }
  };
  ex.spawn(Fast::run()); ex.spawn(ticker('x', 1));
  ex.run();
  CHECK(g_log == "12x");

  // block_on also runs spawned tasks.
  g_log.clear();
  ex.spawn(sleeper('s', 5));
  CHECK(ex.block_on(nested(3)) == 6);
  ex.run();
  CHECK(g_log == "s");

  // Pollers run every pass.
  static int polls = 0; polls = 0;
  CHECK(ex.add_poller([](void*) { ++polls; }, nullptr));
  ex.block_on(nested(3));
  CHECK(polls > 3);
  CHECK(ex.remove_poller([](void*) { ++polls; }, nullptr) == false);  // a different function object: not found

  // current_executor.
  struct E { static Task<bool> run(Executor* want) { Executor* got = co_await current_executor(); co_return got == want; } };
  CHECK(ex.block_on(E::run(&ex)));
}

static void test_destroy_pending() {
  StepClock clock;
  {
    Executor ex(clock);
    struct N { static Task<void> run() { co_await nested(2); } };
    ex.spawn(sleeper('z', 100000)); ex.spawn(N::run()); ex.spawn(ticker('q', 5));
    ex.run_once();
    CHECK(ex.active_tasks() == 3);
    CHECK(g_frames.live > 0);
  }  // the unfinished tasks and everything they were awaiting are freed
  CHECK(g_frames.live == 0);
}

static Task<void> worker(uint32_t ms, int* done) { co_await delay(ms); ++*done; }
static Task<void> critical(AsyncMutex* m, char name, std::string* out) {
  co_await m->lock();
  *out += name; co_await delay(5); *out += name;   // another task must not run its section in between
  m->unlock();
}
static Task<void> waiter(AsyncEvent* e, std::string* out) { co_await e->wait(); *out += 'w'; }
static Task<void> setter(AsyncEvent* e, std::string* out) { co_await delay(10); *out += 's'; e->set(); }

static void test_combinators() {
  StepClock clock; Executor ex(clock);
  int done = 0;
  const uint64_t t0 = clock.us;
  struct All { static Task<void> run(int* done) { co_await when_all(worker(40, done), worker(40, done), worker(40, done)); } };
  ex.block_on(All::run(&done));
  CHECK(done == 3);
  const uint64_t elapsed = clock.us - t0;
  CHECK(elapsed >= 40000 && elapsed < 80000);          // concurrent: about 40 ms, not 120
  {
    Task<void> arr[2] = {worker(5, &done), worker(6, &done)};
    struct Arr { static Task<void> run(Task<void>* t) { co_await when_all(t, 2); } };
    ex.block_on(Arr::run(arr));
    CHECK(done == 5);
  }
  std::string out;
  AsyncMutex m;
  ex.spawn(critical(&m, 'a', &out)); ex.spawn(critical(&m, 'b', &out)); ex.spawn(critical(&m, 'c', &out));
  ex.run();
  CHECK(out == "aabbcc" && !m.is_locked());
  CHECK(m.try_lock() && !m.try_lock()); m.unlock();

  out.clear();
  AsyncEvent e;
  ex.spawn(waiter(&e, &out)); ex.spawn(waiter(&e, &out)); ex.spawn(setter(&e, &out));
  ex.run();
  CHECK(out == "sww" && e.is_set());
  struct Ev { static Task<bool> run(AsyncEvent* ev) { co_return co_await ev->wait(5); } };
  e.reset();
  CHECK(!ex.block_on(Ev::run(&e)));                    // timed out
  e.set();
  CHECK(ex.block_on(Ev::run(&e)));
  CHECK(g_frames.live == 0);
}

// ---- streams
// (Written with each co_await in a statement of its own: GCC mis-evaluates a co_await inside a
// larger && / || condition.)
#define STEP(cond) do { if (!(cond)) { printf("step failed line %d\n", __LINE__); *ok = false; } } while (0)

static Task<void> memory_round_trip(bool* ok) {
  MemoryStream m(g_frames);
  *ok = true;
  StreamStatus s = co_await write_text_async(m, "async hello");
  STEP(s == StreamStatus::Ok);
  m.seek(0, SeekOrigin::Begin, nullptr);
  uint8_t buf[5];
  s = co_await read_exactly_async(m, ByteView(buf));
  STEP(s == StreamStatus::Ok && !memcmp(buf, "async", 5));
  MemoryStream copy(g_frames);
  uint64_t copied = 0;
  s = co_await copy_to_async(m, copy, &copied);
  STEP(s == StreamStatus::Ok && copied == 6 && copy.contents().size() == 6);
  uint8_t more[8];
  s = co_await read_exactly_async(m, ByteView(more));
  STEP(s == StreamStatus::EndOfStream);
  IoResult r = co_await m.read_async(ByteView(more));
  STEP(r.status == StreamStatus::Ok && r.count == 0);
  CancellationSource cancel; cancel.cancel();
  r = co_await m.read_async(ByteView(more), cancel.token());
  STEP(r.status == StreamStatus::Cancelled);
  s = co_await m.write_async(bytes_of("x"), cancel.token());
  STEP(s == StreamStatus::Cancelled);
  s = co_await m.flush_async();
  STEP(s == StreamStatus::Ok);
  m.close();
  r = co_await m.read_async(ByteView(more));
  STEP(r.status == StreamStatus::Closed);
}

static Task<void> file_round_trip(filesystem::IFileSystem* fs, bool* ok) {
  UniquePointer<IAsyncStream> f;
  *ok = FileStream::open(g_frames, *fs, "/a.txt", filesystem::OpenMode::CreateOrOpen, &f) == filesystem::Status::Ok;
  if (!*ok) co_return;
  StreamStatus s = co_await write_text_async(*f, "file via await");
  STEP(s == StreamStatus::Ok);
  s = co_await f->flush_async();
  STEP(s == StreamStatus::Ok);
  f->seek(0, SeekOrigin::Begin, nullptr);
  uint8_t buf[14];
  s = co_await read_exactly_async(*f, ByteView(buf));
  STEP(s == StreamStatus::Ok && !memcmp(buf, "file via await", 14));
}

static void test_async_memory_and_file() {
  StepClock clock; Executor ex(clock);
  bool ok = false;
  ex.block_on(memory_round_trip(&ok));
  CHECK(ok);
  filesystem::RamFileSystem fs(g_frames);
  ok = false;
  ex.block_on(file_round_trip(&fs, &ok));
  CHECK(ok);
}

static Task<void> echo_client(Lan* lan, uint16_t port, const char* message, std::string* got, StreamStatus* status) {
  UniquePointer<IAsyncStream> c;
  *status = co_await TcpStream::connect_async(lan->a, g_frames, Ipv4Address::make(10, 0, 0, 2), port, 5000, &c);
  if (*status != StreamStatus::Ok) co_return;
  *status = co_await write_text_async(*c, message);
  if (*status == StreamStatus::Ok) *status = co_await c->flush_async();
  uint8_t buf[64];
  const size_t want = strlen(message);
  while (*status == StreamStatus::Ok && got->size() < want) {
    IoResult r = co_await c->read_async(ByteView(buf));
    *status = r.status;
    if (r.status == StreamStatus::Ok && r.count == 0) break;
    got->append(reinterpret_cast<char*>(buf), r.count);
  }
}

static void echo_app(Server& s) {
  if (!s.conn) return;
  static std::map<Server*, size_t> echoed;
  size_t& n = echoed[&s];
  while (n < s.received.size()) {
    uint32_t k = s.conn->send(ConstByteView(reinterpret_cast<const uint8_t*>(s.received.data()) + n, s.received.size() - n));
    if (!k) break;
    n += k;
  }
}

static Task<void> read_with_cancel(IAsyncStream* s, CancellationToken token, IoResult* result) {
  uint8_t b[8];
  *result = co_await s->read_async(ByteView(b), token);
}
static Task<void> cancel_later(CancellationSource* source, uint32_t ms) { co_await delay(ms); source->cancel(); }

static Task<void> accept_one(TcpListener* l, std::string* got, StreamStatus* status) {
  UniquePointer<IAsyncStream> c;
  *status = co_await l->accept_async(&c, 5000);
  if (*status != StreamStatus::Ok) co_return;
  uint8_t buf[16];
  IoResult r = co_await c->read_async(ByteView(buf));
  got->append(reinterpret_cast<char*>(buf), r.count);
  *status = co_await write_text_async(*c, "yo");
  if (*status == StreamStatus::Ok) *status = co_await c->flush_async();
}

static Task<void> client_of_listener(Lan* lan, std::string* got, StreamStatus* status) {
  UniquePointer<IAsyncStream> c;
  *status = co_await TcpStream::connect_async(lan->a, g_frames, Ipv4Address::make(10, 0, 0, 2), 80, 5000, &c);
  if (*status != StreamStatus::Ok) co_return;
  co_await write_text_async(*c, "hi");
  uint8_t buf[8];
  IoResult r = co_await c->read_async(ByteView(buf));
  got->append(reinterpret_cast<char*>(buf), r.count);
}

static void test_async_tcp() {
  {  // two clients at once on one executor, each talking to its own echo server
    Lan lan; lan.configure_static();
    Server s1(lan, 7), s2(lan, 8);
    s1.app = echo_app; s2.app = echo_app;
    Executor ex(lan.clock);
    std::string g1, g2; StreamStatus st1 = StreamStatus::Cancelled, st2 = StreamStatus::Cancelled;
    ex.spawn(echo_client(&lan, 7, "first connection", &g1, &st1));
    ex.spawn(echo_client(&lan, 8, "second connection, a bit longer", &g2, &st2));
    ex.run();
    CHECK(st1 == StreamStatus::Ok && g1 == "first connection");
    CHECK(st2 == StreamStatus::Ok && g2 == "second connection, a bit longer");
  }
  {  // bulk data through write_async/flush_async (more than the send buffer)
    Lan lan; lan.configure_static();
    Server s(lan, 7);
    Executor ex(lan.clock);
    struct Bulk {
      static Task<void> run(Lan* lan, StreamStatus* st) {
        UniquePointer<IAsyncStream> c;
        *st = co_await TcpStream::connect_async(lan->a, g_frames, Ipv4Address::make(10, 0, 0, 2), 7, 5000, &c);
        if (*st != StreamStatus::Ok) co_return;
        std::string data(100000, 'x'); for (size_t i = 0; i < data.size(); ++i) data[i] = char('a' + i % 23);
        *st = co_await c->write_async(ConstByteView(reinterpret_cast<const uint8_t*>(data.data()), data.size()));
        if (*st == StreamStatus::Ok) *st = co_await c->flush_async();
      }
    };
    StreamStatus st = StreamStatus::Cancelled;
    ex.block_on(Bulk::run(&lan, &st));
    CHECK(st == StreamStatus::Ok && s.received.size() == 100000 && s.received[22] == 'w' && s.received[23] == 'a');
  }
  {  // read timeout, cancellation, refused connection, connect timeout
    Lan lan; lan.configure_static();
    Server s(lan, 7);  // says nothing
    Executor ex(lan.clock);
    UniquePointer<IAsyncStream> c;
    CHECK_STATUS(ex.block_on(TcpStream::connect_async(lan.a, g_frames, Ipv4Address::make(10, 0, 0, 2), 7, 5000, &c)), StreamStatus::Ok);
    c->set_read_timeout(40);
    IoResult r;
    ex.block_on(read_with_cancel(c.get(), {}, &r));
    CHECK(r.status == StreamStatus::TimedOut);
    c->set_read_timeout(kInfiniteTimeout);
    CancellationSource cancel;
    ex.spawn(cancel_later(&cancel, 20));
    r = {};
    ex.block_on(read_with_cancel(c.get(), cancel.token(), &r));
    CHECK(r.status == StreamStatus::Cancelled);
    ex.run();
    UniquePointer<IAsyncStream> none;
    CHECK_STATUS(ex.block_on(TcpStream::connect_async(lan.a, g_frames, Ipv4Address::make(10, 0, 0, 2), 99, 5000, &none)), StreamStatus::ConnectionLost);
    CHECK_STATUS(ex.block_on(TcpStream::connect_async(lan.a, g_frames, Ipv4Address::make(10, 0, 0, 77), 7, 200, &none)), StreamStatus::TimedOut);
    CHECK_STATUS(ex.block_on(TcpStream::connect_async(lan.a, g_frames, "10.0.0.2", 7, 5000, &none)), StreamStatus::Ok);  // a dotted quad resolves at once
    CHECK(none);
    CHECK_STATUS(ex.block_on(TcpStream::connect_async(lan.a, g_frames, "no-such-host.invalid", 7, 200, &none)), StreamStatus::HostNotFound);
  }
  {  // a listener: accept_async on one side, connect_async on the other
    Lan lan; lan.configure_static();
    Executor ex(lan.clock);
    TcpListener listener(lan.b, g_frames);
    CHECK(listener.listen(80) && listener.port() == 80);
    std::string server_got, client_got; StreamStatus sst = StreamStatus::Cancelled, cst = StreamStatus::Cancelled;
    ex.spawn(accept_one(&listener, &server_got, &sst));
    ex.spawn(client_of_listener(&lan, &client_got, &cst));
    ex.run();
    CHECK(sst == StreamStatus::Ok && cst == StreamStatus::Ok && server_got == "hi" && client_got == "yo");
    // accept times out when nobody connects; stopping makes accept fail.
    UniquePointer<IAsyncStream> c;
    CHECK_STATUS(ex.block_on(listener.accept_async(&c, 30)), StreamStatus::TimedOut);
    CHECK_STATUS(listener.accept(30, &c), StreamStatus::TimedOut);
    listener.stop();
    CHECK_STATUS(ex.block_on(listener.accept_async(&c, 30)), StreamStatus::Closed);
  }
}

int main() {
  setvbuf(stdout, nullptr, _IONBF, 0);
  set_frame_allocator(g_frames);
  test_tasks();
  test_executor();
  test_destroy_pending();
  test_combinators();
  test_async_memory_and_file();
  test_async_tcp();
  CHECK(g_frames.live == 0);
  if (g_failed) { printf("%d check(s) failed\n", g_failed); return 1; }
  printf("async tests passed\n");
  return 0;
}
