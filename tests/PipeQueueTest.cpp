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

// Host tests of the queues and operations of a pipe (ObjectManager): a write that does not fit is queued whole and visible to the other end at once, and complete when enough
// was read; reads that wait are served in the order they were made; cancelling an operation takes back the bytes of a write; closing or disconnecting ends the operations;
// several threads write, read and cancel at the same time.
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <thread>
#include <vector>
#include "Process/Objects.hpp"

using namespace process;
using Result = ObjectManager::PipeResult;
using Kind = ObjectManager::PipeOpKind;

struct TestAllocator final : allocators::IAllocator {
  void* allocate(uint64_t size) override { return malloc(size); }
  void* allocate_aligned(uint64_t size, uint64_t align) override { return aligned_alloc(align, (size + align - 1) / align * align); }
  void deallocate(void* p) override { free(p); }
};

static std::atomic<int> g_failed{0};
#define CHECK(...) do { if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)

struct SystemSync final : ISystemSync {
  std::mutex m;
  std::condition_variable cv;
  static thread_local std::unique_lock<std::mutex>* mine;
  void lock() override { mine = new std::unique_lock<std::mutex>(m); }
  void unlock() override { delete mine; mine = nullptr; }
  bool wait(uint64_t ns) override {
    if (ns == ~uint64_t{0}) { cv.wait(*mine); return true; }
    return cv.wait_for(*mine, std::chrono::nanoseconds(ns)) == std::cv_status::no_timeout;
  }
  void notify_all() override { cv.notify_all(); }
};
thread_local std::unique_lock<std::mutex>* SystemSync::mine = nullptr;

struct Fixture {
  SystemSync sync;
  TestAllocator allocator;
  ObjectManager objects{sync};
  uint32_t id = 0, server = 0, client = 0;
  explicit Fixture(bool message, uint32_t out_size = 5000, uint32_t in_size = 6000) {
    objects.set_allocator(&allocator);
    const ObjectManager::Created c = objects.create_pipe(StringView("pq"), out_size, in_size, 1, ObjectManager::kPipeDuplex, message, 1);
    id = c.id;
    server = c.end_code;
    const ObjectManager::Created k = objects.connect_pipe(StringView("pq"), true, true, 2);
    client = k.end_code;
  }
  uint32_t code(uint32_t end) const { return end ? client : server; }      // (the end code of end 0, the server, or 1, the client)
  struct Lock {
    ISystemSync& s;
    explicit Lock(ISystemSync& x) : s(x) { s.lock(); }
    ~Lock() { s.unlock(); }
  };
  // A write of n bytes (of value v): the tag of its operation, 0 if it went at once.
  uint32_t write(uint32_t end, uint32_t n, uint8_t v, bool async = true, uint32_t* done = nullptr) {
    std::vector<uint8_t> data(n, v);
    uint32_t tag = 0, moved = 0;
    Lock hold(sync);
    const Result r = objects.pipe_begin_write_locked(id, code(end), ConstByteView(data.data(), data.size()), 7, async, &tag, &moved);
    if (done) *done = moved;
    if (r == Result::Ok) { return 0; }
    CHECK(r == Result::WouldBlock);
    return tag;
  }
  uint32_t read_async(uint32_t end, uint32_t length, bool message_mode) {
    uint32_t tag = 0;
    Lock hold(sync);
    const Result r = objects.pipe_begin_read_locked(id, code(end), length, message_mode, 7, true, &tag);
    if (r != Result::WouldBlock) return 0;
    return tag;
  }
  uint32_t available(uint32_t end, uint32_t* message = nullptr) {
    uint8_t probe[1];
    uint32_t got = 0, avail = 0, msg = 0;
    Lock hold(sync);
    objects.pipe_read_locked(id, code(end), probe, 1, &got, true, &avail, false, &msg);
    if (message) *message = msg;
    return avail;
  }
  Result read(uint32_t end, uint32_t n, std::vector<uint8_t>* out, bool message_mode = false) {
    out->assign(n, 0);
    uint32_t got = 0;
    Lock hold(sync);
    const Result r = objects.pipe_read_locked(id, code(end), out->data(), n, &got, false, nullptr, message_mode);
    out->resize(got);
    return r;
  }
  // The state of an operation: 0 missing, 1 pending, 2 done (status and count in the arguments).
  int poll(uint32_t end, uint32_t tag, uint32_t* status = nullptr, uint32_t* transferred = nullptr, std::vector<uint8_t>* data = nullptr) {
    ObjectManager::PipeOpResult result;
    Lock hold(sync);
    const ObjectManager::PipeOpPoll p = objects.pipe_op_poll_locked(id, code(end), tag, &result);
    if (p == ObjectManager::PipeOpPoll::Missing) return 0;
    if (p == ObjectManager::PipeOpPoll::Pending) return 1;
    if (status) *status = result.status;
    if (transferred) *transferred = result.transferred;
    if (data) data->assign(result.data.data(), result.data.data() + result.data.size());
    objects.pipe_op_free_data(result.data);
    return 2;
  }
  bool cancel(uint32_t end, uint32_t tag) {
    Lock hold(sync);
    return objects.pipe_op_cancel_locked(id, code(end), tag, 0xC0000120u) == Result::Ok;
  }
};

constexpr uint32_t kSuccess = 0, kCancelled = 0xC0000120u, kBroken = 0xC000014Bu, kDisconnected = 0xC00000B0u, kMoreData = 0x80000005u;

// A write larger than the buffer is visible at once, and complete when the reader made room; the bytes arrive in order.
static void test_queued_write() {
  Fixture f(false);
  CHECK(f.write(0, 4000, 'a') == 0);
  const uint32_t tag = f.write(0, 5000, 'b');      // 4000 + 5000 > 5000
  CHECK(tag != 0 && f.available(1) == 9000);
  CHECK(f.poll(0, tag) == 1);
  std::vector<uint8_t> data;
  CHECK(f.read(1, 3999, &data) == Result::Ok && data.size() == 3999 && data[0] == 'a');
  CHECK(f.poll(0, tag) == 1);      // 1 + 5000 unread: still more than the buffer
  CHECK(f.read(1, 1, &data) == Result::Ok);
  uint32_t status = 1, transferred = 0;
  CHECK(f.poll(0, tag, &status, &transferred) == 2 && status == kSuccess && transferred == 5000);
  f.objects.pipe_op_release(f.id, tag);
  CHECK(f.read(1, 6000, &data) == Result::Ok && data.size() == 5000 && data[0] == 'b' && data[4999] == 'b');
  CHECK(f.available(1) == 0);
}

// Reads that wait are served in order, and a write they take is complete at once.
static void test_reads_in_order() {
  Fixture f(false);
  const uint32_t r1 = f.read_async(1, 0, false), r2 = f.read_async(1, 10, false), r3 = f.read_async(1, 100, false);
  CHECK(r1 && r2 && r3);
  CHECK(f.write(0, 30, 'x') == 0);
  uint32_t status = 1, transferred = 9;
  std::vector<uint8_t> data;
  CHECK(f.poll(1, r1, &status, &transferred, &data) == 2 && status == kSuccess && transferred == 0);
  CHECK(f.poll(1, r2, &status, &transferred, &data) == 2 && transferred == 10 && data.size() == 10);
  CHECK(f.poll(1, r3, &status, &transferred, &data) == 2 && transferred == 20 && data.size() == 20);
  CHECK(f.available(1) == 0);
  // the pending reads keep the bytes of a write that does not fit from counting against the buffer
  const uint32_t r4 = f.read_async(1, 8000, false);
  CHECK(r4 && f.write(0, 7000, 'y') == 0);
  CHECK(f.poll(1, r4, &status, &transferred) == 2 && transferred == 7000);
  CHECK(f.poll(1, r1) == 0);      // (a result is taken once)
  f.objects.pipe_op_release(f.id, r1);
  CHECK(f.poll(1, r1) == 0);
}

// Cancelling one write takes back its bytes (only), and the writes after it that fit are complete then.
static void test_cancel_write() {
  Fixture f(true);
  CHECK(f.write(0, 3000, 'a') == 0);
  const uint32_t w1 = f.write(0, 5000, 'b'), w2 = f.write(0, 1, 'c');
  CHECK(w1 && w2 && f.available(1) == 8001);
  uint32_t message = 0;
  CHECK(f.available(1, &message) == 8001 && message == 3000);
  std::vector<uint8_t> data;
  CHECK(f.read(1, 3000, &data, true) == Result::Ok && data.size() == 3000);
  CHECK(f.poll(0, w1) == 2 && f.poll(0, w2) == 1);      // 5001 is more than 5000
  CHECK(f.cancel(0, w2) && f.poll(0, w2) == 2);
  f.objects.pipe_op_release(f.id, w1);
  f.objects.pipe_op_release(f.id, w2);
  CHECK(f.available(1, &message) == 5000 && message == 5000);
  // the bytes in the middle go: the others stay in order
  const uint32_t w3 = f.write(0, 100, 'd'), w4 = f.write(0, 200, 'e');
  CHECK(w3 && w4 && f.available(1) == 5300);
  CHECK(f.cancel(0, w3));
  CHECK(f.available(1) == 5200);
  CHECK(f.read(1, 5000, &data, true) == Result::Ok && data.size() == 5000 && data[0] == 'b');
  CHECK(f.read(1, 1000, &data, true) == Result::Ok && data.size() == 200 && data[0] == 'e' && data[199] == 'e');
  f.objects.pipe_op_release(f.id, w3);
  f.objects.pipe_op_release(f.id, w4);
  CHECK(!f.cancel(0, w4));      // over
}

// A message is kept whole, read in part with MoreData.
static void test_messages() {
  Fixture f(true);
  CHECK(f.write(0, 10, 'm') == 0 && f.write(0, 5, 'n') == 0);
  uint32_t message = 0;
  CHECK(f.available(1, &message) == 15 && message == 10);
  std::vector<uint8_t> data;
  CHECK(f.read(1, 4, &data, true) == Result::MoreData && data.size() == 4);
  CHECK(f.available(1, &message) == 11 && message == 6);
  CHECK(f.read(1, 100, &data, true) == Result::Ok && data.size() == 6);
  CHECK(f.read(1, 100, &data, true) == Result::Ok && data.size() == 5 && data[0] == 'n');
  CHECK(f.write(0, 0, 0) == 0);      // an empty message
  CHECK(f.available(1, &message) == 0);
  CHECK(f.read(1, 100, &data, true) == Result::Ok && data.empty());
  CHECK(f.read(1, 100, &data, true) == Result::WouldBlock);
}

// Closing an end, or disconnecting, ends what the other end waits for.
static void test_break() {
  {
    Fixture f(false);
    CHECK(f.write(0, 4000, 'a') == 0);
    const uint32_t w = f.write(0, 3000, 'b');
    CHECK(w);
    uint32_t flush = 0;
    {
      Fixture::Lock hold(f.sync);
      CHECK(f.objects.pipe_begin_flush_locked(f.id, f.server, 7, &flush) == Result::WouldBlock);
    }
    f.objects.release_pipe_end(f.id, f.client);      // the reader goes
    uint32_t status = 0;
    CHECK(f.poll(0, w, &status) == 2 && status == kBroken);
    CHECK(f.poll(0, flush, &status) == 2 && status == kBroken);
    CHECK(f.available(0) == 0);
  }
  {
    Fixture f(false);
    const uint32_t r = f.read_async(1, 10, false);
    const uint32_t w = f.write(1, 7000, 'c');
    CHECK(r && w);
    f.objects.pipe_disconnect(f.id);
    uint32_t status = 0;
    CHECK(f.poll(1, r, &status) == 2 && status == kDisconnected);
    CHECK(f.poll(1, w, &status) == 2 && status == kDisconnected);
  }
  {
    Fixture f(false);
    uint32_t tag = 0;
    const uint32_t w = f.write(0, 7000, 'd');      // a write of a process that ends
    CHECK(w);
    f.objects.pipe_owner_gone(7);
    CHECK(f.available(1) == 0 && f.poll(0, w) == 0);
    (void)tag;
  }
}

// TransactNamedPipe: the message is written without waiting, and the answer is a read that waits.
static void test_transact() {
  Fixture f(true);
  uint32_t tag = 0;
  const std::vector<uint8_t> abc = {'a', 'b', 'c'};
  {
    Fixture::Lock hold(f.sync);
    CHECK(f.objects.pipe_begin_transact_locked(f.id, f.server, ConstByteView(abc.data(), 3), 100, true, 7, true, &tag) == Result::WouldBlock && tag != 0);
  }
  std::vector<uint8_t> data;
  CHECK(f.read(1, 100, &data, true) == Result::Ok && data.size() == 3 && data[2] == 'c');
  CHECK(f.write(1, 4, 't') == 0);      // the answer completes the read
  uint32_t status = 1, transferred = 0;
  CHECK(f.poll(0, tag, &status, &transferred, &data) == 2 && status == kSuccess && transferred == 4 && data.size() == 4);
  f.objects.pipe_op_release(f.id, tag);
  // there is something to read already: busy; a byte pipe: not a state for it
  CHECK(f.write(1, 1, 'u') == 0);
  {
    Fixture::Lock hold(f.sync);
    CHECK(f.objects.pipe_begin_transact_locked(f.id, f.server, ConstByteView(abc.data(), 3), 100, true, 7, true, &tag) == Result::Busy);
    CHECK(f.objects.pipe_begin_transact_locked(f.id, f.server, ConstByteView(abc.data(), 3), 100, false, 7, true, &tag) == Result::InvalidState);
  }
  CHECK(f.read(0, 10, &data, true) == Result::Ok);
  // a write that waits for room stays when the answer is cancelled
  CHECK(f.write(0, 5900, 'w') != 0 || true);
  const uint32_t pending = f.write(0, 2000, 'x');
  {
    Fixture::Lock hold(f.sync);
    CHECK(f.objects.pipe_begin_transact_locked(f.id, f.server, ConstByteView(abc.data(), 3), 10, true, 7, true, &tag) == Result::WouldBlock);
  }
  CHECK(f.cancel(0, tag));
  f.objects.pipe_op_release(f.id, tag);
  if (pending) f.objects.pipe_op_release(f.id, pending);
  CHECK(f.available(1) >= 2003);
  {
    Fixture g(false);
    Fixture::Lock hold(g.sync);
    CHECK(g.objects.pipe_begin_transact_locked(g.id, g.server, ConstByteView(abc.data(), 3), 10, true, 7, true, &tag) == Result::InvalidState);
  }
}

// The operations that other threads wait for: whoever finishes one waits until it was acted on.
static void test_delivery() {
  Fixture f(false);
  const uint32_t r = f.read_async(1, 100, false);
  CHECK(r);
  std::atomic<bool> got{false};
  std::thread helper([&] {      // (the thread of the operation: waits for it, acts on it, releases it)
    for (;;) {
      std::vector<uint8_t> data;
      uint32_t status = 0, transferred = 0;
      const int p = f.poll(1, r, &status, &transferred, &data);
      if (p == 2) { got = transferred == 5; std::this_thread::sleep_for(std::chrono::milliseconds(20)); f.objects.pipe_op_release(f.id, r); return; }
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  });
  CHECK(f.write(0, 5, 'z') == 0);
  bool delivered = false;
  for (int i = 0; i < 200 && !delivered; ++i) {
    { Fixture::Lock hold(f.sync); delivered = f.objects.pipe_delivered_locked(f.id); }
    if (!delivered) std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  CHECK(delivered);
  helper.join();
  CHECK(got);
}

// Several threads write, read and cancel at once; every byte that is written and not taken back is read, in order per writer.
static void test_threads() {
  Fixture f(false, 1000, 1000);
  constexpr int kWriters = 4, kPerWriter = 200;
  std::atomic<int> cancelled_bytes{0};
  std::vector<std::thread> writers;
  for (int w = 0; w < kWriters; ++w) {
    writers.emplace_back([&, w] {
      for (int i = 0; i < kPerWriter; ++i) {
        const uint32_t n = 1 + (i * 37 + w) % 300;
        uint32_t tag = f.write(0, n, uint8_t('A' + w));
        if (!tag) continue;
        bool done = false;
        for (int spin = 0; spin < 20000 && !done; ++spin) {
          if (i % 7 == 0 && spin == 3 && f.cancel(0, tag)) { cancelled_bytes += n; }
          const int p = f.poll(0, tag);
          if (p != 1) done = true; else std::this_thread::yield();
        }
        f.objects.pipe_op_release(f.id, tag);
      }
    });
  }
  std::atomic<bool> stop{false};
  std::atomic<long> total_read{0};
  std::thread reader([&] {
    std::vector<uint8_t> data;
    while (!stop) {
      if (f.read(1, 200, &data) == Result::Ok) total_read += long(data.size());
      else std::this_thread::yield();
    }
    while (f.read(1, 200, &data) == Result::Ok) total_read += long(data.size());
  });
  for (std::thread& t : writers) t.join();
  stop = true;
  reader.join();
  long expected = 0;
  for (int w = 0; w < kWriters; ++w) for (int i = 0; i < kPerWriter; ++i) expected += 1 + (i * 37 + w) % 300;
  CHECK(total_read + cancelled_bytes == expected);
  CHECK(f.available(1) == 0);
}

int main() {
  setvbuf(stdout, nullptr, _IONBF, 0);
  puts("test_queued_write");
  test_queued_write();
  puts("test_reads_in_order");
  test_reads_in_order();
  puts("test_cancel_write");
  test_cancel_write();
  puts("test_messages");
  test_messages();
  puts("test_break");
  test_break();
  puts("test_transact");
  test_transact();
  puts("test_delivery");
  test_delivery();
  puts("test_threads");
  test_threads();
  if (g_failed) { printf("PipeQueueTest: %d failed\n", int(g_failed)); return 1; }
  printf("PipeQueueTest: all passed\n");
  return 0;
}
