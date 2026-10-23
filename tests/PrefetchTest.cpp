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

// Host-side tests for the page's background downloads: the scan of a page's HTML for what it will ask for, and the ResourcePrefetcher (parallel, bounded,
// cancellable, and it never waits behind work that has not started). Real threads (tests/HostThreads.cpp).
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <string>
#include <vector>
#include "Browser/ResourcePrefetcher.hpp"
#include "Browser/ResourceScan.hpp"

using namespace browser;

static int g_failed = 0;
#define CHECK(...) do { if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)

struct MallocAllocator final : allocators::IAllocator {
  void* allocate(uint64_t size) override { return malloc(size); }
  void* allocate_aligned(uint64_t size, uint64_t) override { return malloc(size); }
  void deallocate(void* p) override { free(p); }
};

static std::string scan(const std::string& html) {
  std::string out;
  scan_resources(StringView(html.data(), html.size()), [&](ResourceKind kind, StringView ref) {
    if (!out.empty()) out += " ";
    out += kind == ResourceKind::Script ? "S:" : kind == ResourceKind::Style ? "C:" : "I:";
    out += std::string(ref.data(), ref.size());
  });
  return out;
}

static void test_scan() {
  CHECK(scan("<html><head><script src=a.js></script><link rel=stylesheet href=\"b.css\"></head><body><img src='c.png'></body></html>") == "S:a.js C:b.css I:c.png");
  CHECK(scan("<SCRIPT SRC=\"x.js\" ASYNC></SCRIPT><LINK REL='Stylesheet' HREF=y.css>") == "S:x.js C:y.css");
  CHECK(scan("<!-- <script src=no.js></script> --><script src=yes.js></script>") == "S:yes.js");
  CHECK(scan("<script>var s = '<script src=no.js></script>';</script><script src=yes.js></script>") == "S:yes.js");
  CHECK(scan("<script type=module src=m.js></script><script nomodule src=n.js></script><script type=text/javascript src=j.js></script>") == "S:j.js");
  CHECK(scan("<link rel='icon' href=i.png><link rel='alternate stylesheet' href=s.css><a href=z.css>") == "C:s.css");
  CHECK(scan("<style>img{} <img src=no.png></style><textarea><img src=no2.png></textarea><img src=yes.png>") == "I:yes.png");
  CHECK(scan("<img alt='a>b' src=ok.png>") == "I:ok.png");
  CHECK(scan("") == "");
  CHECK(scan("<") == "" && scan("<script src") == "" && scan("<img src='unterminated") == "I:unterminated");
  std::string many;
  for (int i = 0; i < 500; ++i) many += "<img src=i" + std::to_string(i) + ".png>";
  uint32_t n = 0;
  scan_resources(StringView(many.data(), many.size()), [&](ResourceKind, StringView) { ++n; });
  CHECK(n == kMaxScannedResources);

  // truncation and bit flips must neither crash nor read outside the text (run under the address sanitizer)
  const std::string page = "<!doctype html><html><head><!-- c --><script src=\"a.js\" async></script><link rel=stylesheet href=b.css><style>p{}</style></head><body><img src=c.png alt=\"x\"/><script>x<y</script></body></html>";
  for (uint64_t cut = 0; cut <= page.size(); ++cut) {
    const std::string part = page.substr(0, cut);
    scan(part);
  }
  for (uint64_t i = 0; i < page.size(); ++i)
    for (int bit = 0; bit < 8; ++bit) {
      std::string flipped = page;
      flipped[i] = static_cast<char>(flipped[i] ^ (1 << bit));
      scan(flipped);
    }
}

struct Fake {
  uint32_t delay_ms = 0;
  uint32_t active = 0, max_active = 0, calls = 0;
  bool gate_closed = false;
  bool fail = false;
  uint64_t body_size = 16;
  uint32_t finished = 0;
};

static bool fake_fetch(void* context, StringView address, ByteBuffer* body) {
  Fake& f = *static_cast<Fake*>(context);
  const uint32_t now = __atomic_add_fetch(&f.active, 1, __ATOMIC_ACQ_REL);
  uint32_t seen = __atomic_load_n(&f.max_active, __ATOMIC_ACQUIRE);
  while (now > seen && !__atomic_compare_exchange_n(&f.max_active, &seen, now, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {}
  __atomic_add_fetch(&f.calls, 1, __ATOMIC_ACQ_REL);
  while (__atomic_load_n(&f.gate_closed, __ATOMIC_ACQUIRE)) kernel::sleep_us(500);
  if (f.delay_ms) kernel::sleep_us(uint64_t{f.delay_ms} * 1000);
  bool ok = !f.fail;
  if (ok) {
    for (uint64_t i = 0; i < f.body_size; ++i) {
      const uint8_t b = static_cast<uint8_t>(address[address.size() - 1]);
      ok = ok && body->append(ConstByteView(&b, 1));
    }
  }
  __atomic_sub_fetch(&f.active, 1, __ATOMIC_ACQ_REL);
  __atomic_add_fetch(&f.finished, 1, __ATOMIC_ACQ_REL);
  return ok;
}

static uint64_t now_ms() {
  timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<uint64_t>(ts.tv_sec) * 1000 + static_cast<uint64_t>(ts.tv_nsec) / 1000000;
}

static void test_parallel_and_bounded() {
  MallocAllocator a;
  Fake fake;
  fake.delay_ms = 40;
  ResourcePrefetcher pool(a, fake_fetch, &fake);
  const uint64_t start = now_ms();
  std::vector<std::string> urls;
  for (int i = 0; i < 16; ++i) urls.push_back("http://h/" + std::to_string(i) + "/" + std::string(1, static_cast<char>('a' + i)));
  for (const std::string& u : urls) pool.request(StringView(u.data(), u.size()));
  while (__atomic_load_n(&fake.finished, __ATOMIC_ACQUIRE) < 16) kernel::sleep_us(1000);
  const uint64_t elapsed = now_ms() - start;
  int hits = 0;
  for (const std::string& u : urls) {
    ByteBuffer got(a);
    if (pool.take(StringView(u.data(), u.size()), 5000, &got) == ResourcePrefetcher::Result::Hit && got.size() == 16 && got.view()[0] == static_cast<uint8_t>(u.back())) ++hits;
  }
  CHECK(hits == 16);
  CHECK(__atomic_load_n(&fake.max_active, __ATOMIC_ACQUIRE) <= ResourcePrefetcher::kMaxWorkers);
  CHECK(__atomic_load_n(&fake.max_active, __ATOMIC_ACQUIRE) >= 2);   // (really parallel)
  CHECK(elapsed < 16 * 40 * 3 / 4);                                  // (faster than one after the other)
}

static void test_queued_is_not_waited_for() {
  MallocAllocator a;
  Fake fake;
  fake.gate_closed = true;
  ResourcePrefetcher pool(a, fake_fetch, &fake);
  std::vector<std::string> urls;
  for (int i = 0; i < 6; ++i) urls.push_back("http://h/q" + std::to_string(i));
  for (const std::string& u : urls) pool.request(StringView(u.data(), u.size()));
  while (__atomic_load_n(&fake.active, __ATOMIC_ACQUIRE) < ResourcePrefetcher::kMaxWorkers) kernel::sleep_us(500);
  CHECK(pool.workers() == ResourcePrefetcher::kMaxWorkers);
  ByteBuffer got(a);
  const uint64_t start = now_ms();
  CHECK(pool.take(StringView(urls[5].data(), urls[5].size()), 10000, &got) == ResourcePrefetcher::Result::Miss);   // queued behind four stuck downloads: handed back at once
  CHECK(now_ms() - start < 500);
  // a download that takes too long is given up on, not waited for forever
  const uint64_t t2 = now_ms();
  CHECK(pool.take(StringView(urls[0].data(), urls[0].size()), 50, &got) == ResourcePrefetcher::Result::Failed);
  CHECK(now_ms() - t2 < 2000);
  __atomic_store_n(&fake.gate_closed, false, __ATOMIC_RELEASE);
}

static void test_failures_cancel_budget() {
  MallocAllocator a;
  {
    Fake fake;
    fake.fail = true;
    ResourcePrefetcher pool(a, fake_fetch, &fake);
    pool.request("http://h/bad");
    ByteBuffer got(a);
    ResourcePrefetcher::Result r = ResourcePrefetcher::Result::Miss;
    for (int i = 0; i < 2000 && (r = pool.take("http://h/bad", 5000, &got)) == ResourcePrefetcher::Result::Miss; ++i) {}
    CHECK(r == ResourcePrefetcher::Result::Failed || r == ResourcePrefetcher::Result::Miss);
    CHECK(pool.take("http://h/never", 10, &got) == ResourcePrefetcher::Result::Miss);
  }
  {
    Fake fake;
    fake.delay_ms = 30;
    ResourcePrefetcher pool(a, fake_fetch, &fake);
    pool.request("http://h/one");
    kernel::sleep_us(5000);
    pool.cancel_all();   // (it is running: it finishes, and the answer is thrown away)
    ByteBuffer got(a);
    CHECK(pool.take("http://h/one", 1000, &got) == ResourcePrefetcher::Result::Miss);
    pool.request("http://h/one");   // (asked for again after the new page opened: works)
    CHECK(pool.take("http://h/one", 5000, &got) != ResourcePrefetcher::Result::Failed);
  }
  {
    Fake fake;
    fake.body_size = 5u << 20;
    ResourcePrefetcher pool(a, fake_fetch, &fake);
    for (int i = 0; i < 4; ++i) {
      const std::string u = "http://h/big" + std::to_string(i);
      pool.request(StringView(u.data(), u.size()));
    }
    while (__atomic_load_n(&fake.finished, __ATOMIC_ACQUIRE) < 4 || pool.workers() != 0) kernel::sleep_us(1000);
    uint32_t hit = 0;
    for (int i = 0; i < 4; ++i) {
      const std::string u = "http://h/big" + std::to_string(i);
      ByteBuffer got(a);
      if (pool.take(StringView(u.data(), u.size()), 10, &got) == ResourcePrefetcher::Result::Hit) ++hit;
    }
    CHECK(hit == 2);   // 12 MiB budget: two of the 5 MiB answers fit, the rest are dropped (Miss)
  }
}

static void test_shutdown_waits() {
  MallocAllocator a;
  Fake fake;
  fake.delay_ms = 60;
  {
    ResourcePrefetcher pool(a, fake_fetch, &fake);
    for (int i = 0; i < 8; ++i) {
      const std::string u = "http://h/s" + std::to_string(i);
      pool.request(StringView(u.data(), u.size()));
    }
  }   // (the destructor returns only when no worker is left to touch `fake` or the pool)
  CHECK(__atomic_load_n(&fake.active, __ATOMIC_ACQUIRE) == 0);
}

int main() {
  setvbuf(stdout, nullptr, _IONBF, 0);
  test_scan();
  test_parallel_and_bounded();
  test_queued_is_not_waited_for();
  test_failures_cancel_budget();
  test_shutdown_waits();
  if (g_failed) {
    printf("%d prefetch checks failed\n", g_failed);
    return 1;
  }
  printf("prefetch tests passed\n");
  return 0;
}
