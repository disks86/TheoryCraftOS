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

#include "AsyncDemo.hpp"
#include "Async/Combinators.hpp"
#include "console.hpp"
#include "Stream/AsyncStreamHelpers.hpp"
#include "Stream/Memory/MemoryStream.hpp"

using namespace async;
using namespace stream;

namespace {

bool g_ok = true;

void check(bool condition, StringView what) {
  console::puts(condition ? "  ok: " : "  FAILED: ");
  console::puts(what);
  console::puts("\n");
  if (!condition) g_ok = false;
}

Task<uint32_t> add_after(uint32_t a, uint32_t b, uint32_t ms) {
  co_await delay(ms);
  co_return a + b;
}

Task<uint32_t> sum_chain() {
  uint32_t total = 0;
  for (uint32_t i = 1; i <= 4; ++i) total += co_await add_after(i, i, 1);
  co_return total;
}

Task<void> nap(uint32_t ms, uint32_t* done) {
  co_await delay(ms);
  ++*done;
}

Task<void> overlapped(uint32_t* done) { co_await when_all(nap(20, done), nap(20, done), nap(20, done)); }

Task<void> critical(AsyncMutex* mutex, char name, char* log, uint32_t* length) {
  co_await mutex->lock();
  log[(*length)++] = name;
  co_await delay(2);  // another task could slip in here without the mutex
  log[(*length)++] = name;
  mutex->unlock();
}

Task<void> wait_then_note(AsyncEvent* event, uint32_t* seen) {
  co_await event->wait();
  ++*seen;
}

Task<void> raise_after(AsyncEvent* event, uint32_t ms) {
  co_await delay(ms);
  event->set();
}

Task<void> stream_demo(allocators::IAllocator& allocator, bool* ok) {
  *ok = false;
  MemoryStream source(allocator), sink(allocator);
  StreamStatus status = co_await write_text_async(source, "hello from an awaited stream");
  if (status != StreamStatus::Ok) co_return;
  source.seek(0, SeekOrigin::Begin, nullptr);
  uint64_t copied = 0;
  status = co_await copy_to_async(source, sink, &copied);
  if (status != StreamStatus::Ok || copied != 28 || sink.contents().size() != 28) co_return;
  uint8_t byte[1];
  CancellationSource cancel;
  cancel.cancel();
  const IoResult read = co_await source.read_async(ByteView(byte), cancel.token());
  *ok = read.status == StreamStatus::Cancelled;
}

}  // namespace

bool run_async_demo(hardware::IClock& clock, allocators::IAllocator& allocator) {
  console::puts("Async/await:\n");
  g_ok = true;
  set_frame_allocator(allocator);
  Executor executor(clock);

  check(executor.block_on(add_after(20, 22, 5)) == 42, "a task returns a value after a delay");
  check(executor.block_on(sum_chain()) == 2 + 4 + 6 + 8, "awaiting tasks in a loop");

  uint32_t done = 0;
  const uint64_t start = clock.now_us();
  executor.block_on(overlapped(&done));
  const uint64_t elapsed_ms = (clock.now_us() - start) / 1000;
  check(done == 3 && elapsed_ms >= 20 && elapsed_ms < 50, "three 20 ms delays overlap under when_all");

  AsyncMutex mutex;
  char log[8] = {};
  uint32_t length = 0;
  executor.spawn(critical(&mutex, 'a', log, &length));
  executor.spawn(critical(&mutex, 'b', log, &length));
  executor.spawn(critical(&mutex, 'c', log, &length));
  executor.run();
  check(length == 6 && log[0] == log[1] && log[2] == log[3] && log[4] == log[5], "a mutex keeps critical sections apart across awaits");

  AsyncEvent event;
  uint32_t seen = 0;
  executor.spawn(wait_then_note(&event, &seen));
  executor.spawn(wait_then_note(&event, &seen));
  executor.spawn(raise_after(&event, 5));
  executor.run();
  check(seen == 2, "an event wakes every waiter");

  bool streams_ok = false;
  executor.block_on(stream_demo(allocator, &streams_ok));
  check(streams_ok, "streams with co_await: write, copy_to_async, cancelled read");

  check(executor.active_tasks() == 0, "no task left behind");
  return g_ok;
}
