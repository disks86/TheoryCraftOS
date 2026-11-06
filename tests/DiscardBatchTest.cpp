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

// The discard batch of the file systems (FileSystem/DiscardBatch): merging neighbours, a reused range cut out (and a range split around
// it), transactions committed and rolled back, the smallest range dropped when the batch is full, byte ranges rounded to whole device
// blocks, and a random run against a model that knows, block by block, what is free and committed - nothing in use is ever discarded.
#include <stdio.h>
#include <stdlib.h>
#include <set>
#include <vector>
#include "FileSystem/DiscardBatch.hpp"

void bounds_violation(const char* what, uint64_t index, uint64_t size) {
  fprintf(stderr, "BOUNDS VIOLATION: %s (index %llu, size %llu)\n", what, (unsigned long long)index, (unsigned long long)size);
  abort();
}

static int g_failed = 0;
#define CHECK(...) do { if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)

namespace {

struct Recorder final : hardware::IStorageDevice {
  struct Call { uint64_t lba, count; };
  std::vector<Call> calls;
  bool can = true;
  uint64_t blocks = 1u << 20;
  const char* name() const override { return "recorder"; }
  bool initialize() override { return true; }
  void shutdown() override {}
  hardware::StorageInfo info() const override {
    hardware::StorageInfo i;
    i.block_count = blocks;
    i.discard = can;
    return i;
  }
  hardware::StorageStatistics statistics() const override { return {}; }
  bool read(uint64_t, ByteView) override { return false; }
  bool write(uint64_t, ConstByteView) override { return false; }
  bool flush() override { return true; }
  bool discard(uint64_t lba, uint64_t count) override {
    calls.push_back({lba, count});
    return lba < blocks && count <= blocks - lba;
  }
  bool sent(uint64_t lba, uint64_t count) const {
    for (const Call& c : calls) if (c.lba == lba && c.count == count) return true;
    return false;
  }
};

void test_basics() {
  Recorder d;
  filesystem::DiscardBatch b;
  b.attach(&d, true);
  CHECK(b.enabled());
  b.freed(100, 10);
  b.freed(110, 5);    // touches: merged
  b.freed(90, 10);    // touches the other end
  b.freed(200, 1);
  CHECK(b.pending() == 2);
  CHECK(b.issue() == 2);
  CHECK(d.sent(90, 25) && d.sent(200, 1) && d.calls.size() == 2);
  CHECK(b.pending() == 0 && b.issue() == 0);

  // A reuse cuts a hole: the range splits.
  d.calls.clear();
  b.freed(1000, 100);
  b.reused(1040, 10);
  b.issue();
  CHECK(d.calls.size() == 2 && d.sent(1000, 40) && d.sent(1050, 50));
  // Reuse of a whole range, of its ends, of something not in the batch.
  d.calls.clear();
  b.freed(2000, 10);
  b.reused(2000, 10);
  b.freed(3000, 10);
  b.reused(2995, 7);
  b.reused(3008, 100);
  b.reused(5000, 3);
  b.issue();
  CHECK(d.calls.size() == 1 && d.sent(3002, 6));

  // Transactions: an uncommitted free is not sent; rollback forgets it, commit makes it go (and merges it with committed neighbours).
  d.calls.clear();
  b.freed(4000, 10, false);
  CHECK(b.issue() == 0 && b.pending() == 1);
  b.rollback();
  CHECK(b.pending() == 0);
  b.freed(4000, 10);
  b.freed(4010, 10, false);
  b.commit();
  CHECK(b.pending() == 1);
  b.issue();
  CHECK(d.calls.size() == 1 && d.sent(4000, 20));

  // Full: the smallest range goes, a smaller newcomer is dropped.
  d.calls.clear();
  for (uint32_t i = 0; i < filesystem::DiscardBatch::kMaxRanges; ++i) b.freed(10000 + i * 100, 2 + i);
  CHECK(b.pending() == filesystem::DiscardBatch::kMaxRanges);
  b.freed(90000, 1);   // smaller than everything: dropped
  b.freed(95000, 50);  // replaces the 2-block range
  CHECK(b.pending() == filesystem::DiscardBatch::kMaxRanges && b.statistics().dropped == 1 + 2);
  b.issue();
  CHECK(!d.sent(10000, 2) && d.sent(95000, 50) && !d.sent(90000, 1));

  // Bytes: a free covers only whole device blocks inside it, a reuse every block it touches.
  d.calls.clear();
  b.freed_bytes(512 * 3 + 100, 4096, 512);    // bytes 1636..5732: blocks 4..10 wholly inside (4 .. 11 exclusive)
  b.issue();
  CHECK(d.calls.size() == 1 && d.sent(4, 7));
  d.calls.clear();
  b.freed_bytes(0, 4096 * 4, 4096);
  b.reused_bytes(4096 + 1, 1, 4096);        // one byte of block 1: the whole block stays
  b.freed_bytes(4096 * 10 + 1, 4000, 4096); // inside one block, not all of it: nothing
  b.issue();
  CHECK(d.calls.size() == 2 && d.sent(0, 1) && d.sent(2, 2));
  CHECK(b.statistics().failed == 0);
}

void test_off() {
  Recorder no;
  no.can = false;
  filesystem::DiscardBatch b;
  b.attach(&no, true);
  CHECK(!b.enabled());
  b.freed(1, 1);
  CHECK(b.pending() == 0 && b.issue() == 0 && no.calls.empty());
  Recorder yes;
  b.attach(&yes, false);   // a read-only volume
  b.freed(1, 1);
  CHECK(!b.enabled() && b.issue() == 0 && yes.calls.empty());
  b.attach(nullptr, true);
  CHECK(!b.enabled());
  b.freed(UINT64_MAX - 1, 5);   // (no wrap even when on)
  b.attach(&yes, true);
  b.freed(UINT64_MAX - 1, 5);
  b.freed_bytes(UINT64_MAX - 10, 100, 512);
  CHECK(b.pending() == 0);
}

// Random frees, reuses, commits and rollbacks against a model; whatever is sent must be free and committed in the model at that moment.
void test_random() {
  Recorder d;
  d.blocks = 4096;
  filesystem::DiscardBatch b;
  b.attach(&d, true);
  std::vector<int> state(4096, 0);   // 0 in use, 1 freed (uncommitted), 2 freed and committed
  uint32_t seed = 12345;
  auto rnd = [&seed] { seed = seed * 1664525u + 1013904223u; return seed >> 8; };
  for (int step = 0; step < 20000 && !g_failed; ++step) {
    const uint32_t op = rnd() % 10;
    const uint64_t lba = rnd() % 4000, count = 1 + rnd() % 64;
    if (op < 4) {
      bool all_used = true;
      for (uint64_t k = lba; k < lba + count; ++k) all_used = all_used && state[k] == 0;
      if (!all_used) continue;   // (a file system frees only what it has)
      const bool committed = rnd() % 2;
      b.freed(lba, count, committed);
      for (uint64_t k = lba; k < lba + count; ++k) state[k] = committed ? 2 : 1;
    } else if (op < 7) {
      b.reused(lba, count);
      for (uint64_t k = lba; k < lba + count; ++k) state[k] = 0;
    } else if (op == 7) {
      b.commit();
      for (int& s : state) if (s == 1) s = 2;
    } else if (op == 8) {
      b.rollback();
      for (int& s : state) if (s == 1) s = 0;
    } else {
      d.calls.clear();
      b.issue();
      for (const Recorder::Call& c : d.calls)
        for (uint64_t k = c.lba; k < c.lba + c.count; ++k)
          if (state[k] != 2) {
            printf("FAILED: block %llu sent while %s (step %d)\n", (unsigned long long)k, state[k] == 0 ? "in use" : "uncommitted", step);
            ++g_failed;
            return;
          }
      for (const Recorder::Call& c : d.calls)
        for (uint64_t k = c.lba; k < c.lba + c.count; ++k) state[k] = 0;   // (sent: as good as unknown again; treated as in use)
    }
  }
  CHECK(b.pending() <= filesystem::DiscardBatch::kMaxRanges);
}

}  // namespace

int main() {
  test_basics();
  test_off();
  test_random();
  if (g_failed) {
    printf("DiscardBatchTest: %d FAILED\n", g_failed);
    return 1;
  }
  printf("DiscardBatchTest: all passed\n");
  return 0;
}
