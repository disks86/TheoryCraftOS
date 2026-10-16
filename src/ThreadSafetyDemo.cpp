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

#include "ThreadSafetyDemo.hpp"
#include "console.hpp"
#include "Kernel/Delay.hpp"
#include "Kernel/Sync.hpp"
#include "Kernel/Thread.hpp"

using namespace kernel;

namespace {

bool report(const char* what, bool ok) {
  console::Guard guard;
  console::puts("  ");
  console::puts(what);
  console::puts(ok ? ": ok\n" : ": FAILED\n");
  return ok;
}

uint32_t next_random(uint32_t* state) {
  *state = *state * 1664525u + 1013904223u;
  return *state >> 8;
}

// Runs `count` threads of `function` (each given its index through the job array)
// that all start at the same moment, and waits for them.
struct StartGate {
  Event go;  // manual reset
};

// ---------------------------------------------------------------- allocator

struct AllocJob {
  allocators::IAllocator* allocator;
  StartGate* gate;
  uint32_t seed;
  uint32_t rounds;
  bool ok;
};

void alloc_main(void* argument) {
  AllocJob* job = static_cast<AllocJob*>(argument);
  job->gate->go.wait();
  uint8_t* held[8] = {};
  uint32_t sizes[8] = {};
  uint32_t state = job->seed;
  job->ok = true;
  for (uint32_t i = 0; i < job->rounds && job->ok; ++i) {
    const uint32_t slot = next_random(&state) % 8;
    if (held[slot]) {  // verify the pattern survived while other threads allocated, then free
      for (uint32_t b = 0; b < sizes[slot]; ++b)
        if (held[slot][b] != static_cast<uint8_t>(job->seed + b + slot)) job->ok = false;
      job->allocator->deallocate(held[slot]);
      held[slot] = nullptr;
    } else {
      const uint32_t size = 16 + next_random(&state) % 4000;
      held[slot] = static_cast<uint8_t*>(job->allocator->allocate(size));
      if (!held[slot]) {
        job->ok = false;
        break;
      }
      sizes[slot] = size;
      for (uint32_t b = 0; b < size; ++b) held[slot][b] = static_cast<uint8_t>(job->seed + b + slot);
    }
    if ((i & 15) == 0) yield();
  }
  for (uint8_t* p : held) job->allocator->deallocate(p);
}

bool test_allocator(allocators::IAllocator& allocator) {
  constexpr uint32_t kThreads = 6;
  StartGate gate;
  AllocJob jobs[kThreads];
  Thread* threads[kThreads];
  for (uint32_t i = 0; i < kThreads; ++i) {
    jobs[i] = {&allocator, &gate, 1000 + i * 77, 600, false};
    threads[i] = spawn(alloc_main, &jobs[i], "alloc");
  }
  gate.go.set();
  bool ok = true;
  for (uint32_t i = 0; i < kThreads; ++i) {
    ok &= threads[i] && join(threads[i], 20000000);
    ok &= jobs[i].ok;
  }
  return report("allocator: 6 threads allocate, fill, verify and free", ok);
}

// ---------------------------------------------------------------- console

struct PrintJob {
  StartGate* gate;
  uint32_t id;
};

void print_main(void* argument) {
  PrintJob* job = static_cast<PrintJob*>(argument);
  job->gate->go.wait();
  for (uint32_t i = 0; i < 10; ++i) {
    console::Guard guard;  // several calls, one line
    console::puts("[T");
    console::put_dec(job->id);
    console::puts("] line ");
    console::put_dec(i);
    console::puts(" of ten\n");
    if (i % 3 == 0) yield();
  }
}

bool test_console() {
  console::puts("  console lines from 4 threads follow (each must be whole):\n");
  StartGate gate;
  PrintJob jobs[4];
  Thread* threads[4];
  for (uint32_t i = 0; i < 4; ++i) {
    jobs[i] = {&gate, i};
    threads[i] = spawn(print_main, &jobs[i], "print");
  }
  gate.go.set();
  bool ok = true;
  for (Thread* t : threads) ok &= t && join(t, 5000000);
  return report("console: 4 threads printed 10 lines each", ok);
}

// ---------------------------------------------------------------- storage

constexpr uint32_t kReferenceBlocks = 256;

struct StorageJob {
  hardware::IStorageDevice* device;
  StartGate* gate;
  const uint8_t* reference;  // the first reference_blocks blocks
  uint32_t reference_blocks;
  uint32_t block_size;
  uint32_t seed;
  uint32_t rounds;
  bool ok;
  uint8_t* scratch;  // read buffer
};

void storage_read_main(void* argument) {
  StorageJob* job = static_cast<StorageJob*>(argument);
  job->gate->go.wait();
  uint32_t state = job->seed;
  job->ok = true;
  for (uint32_t i = 0; i < job->rounds && job->ok; ++i) {
    const uint32_t count = 1 + next_random(&state) % 17;
    const uint32_t lba = next_random(&state) % (job->reference_blocks - count + 1);
    if (!job->device->read(lba, count, job->scratch) ||
        __builtin_memcmp(job->scratch, job->reference + uint64_t{lba} * job->block_size, uint64_t{count} * job->block_size) != 0)
      job->ok = false;
  }
}

// Writers each own a disjoint region at the end of the device.
struct WriteJob {
  hardware::IStorageDevice* device;
  StartGate* gate;
  uint64_t first_block;
  uint32_t blocks;
  uint32_t block_size;
  uint32_t id;
  bool ok;
  uint8_t* buffer;     // blocks * block_size, pattern
  uint8_t* original;   // saved contents
  uint8_t* readback;
};

void storage_write_main(void* argument) {
  WriteJob* job = static_cast<WriteJob*>(argument);
  job->gate->go.wait();
  const uint32_t bytes = job->blocks * job->block_size;
  job->ok = job->device->read(job->first_block, job->blocks, job->original);
  for (uint32_t round = 0; round < 5 && job->ok; ++round) {
    for (uint32_t b = 0; b < bytes; ++b) job->buffer[b] = static_cast<uint8_t>(job->id * 41 + round * 7 + b);
    job->ok = job->device->write(job->first_block, job->blocks, job->buffer) && job->device->flush() &&
              job->device->read(job->first_block, job->blocks, job->readback) &&
              __builtin_memcmp(job->buffer, job->readback, bytes) == 0;
  }
  job->ok = job->device->write(job->first_block, job->blocks, job->original) && job->ok;  // put it back
}

bool test_storage(hardware::SystemStorageDevices& storage, allocators::IAllocator& allocator) {
  if (storage.count() == 0) return true;
  constexpr uint32_t kReadersPerDevice = 3, kWriters = 4, kMaxDevices = 16;
  struct Reference {
    uint8_t* data = nullptr;
    uint32_t blocks = 0;
    bool valid = false;
  };
  const uint32_t devices = storage.count() < kMaxDevices ? storage.count() : kMaxDevices;
  Reference references[kMaxDevices];
  StorageJob jobs[kMaxDevices * kReadersPerDevice];
  Thread* threads[kMaxDevices * kReadersPerDevice];
  StartGate gate;

  uint32_t job_count = 0;
  for (uint32_t d = 0; d < devices; ++d) {
    hardware::IStorageDevice* dev = storage.at(d);
    const hardware::StorageInfo info = dev->info();
    uint32_t blocks = info.block_count < kReferenceBlocks ? static_cast<uint32_t>(info.block_count) : kReferenceBlocks;
    if (blocks < 32) continue;
    references[d].data = static_cast<uint8_t*>(allocator.allocate(uint64_t{blocks} * info.block_size));
    if (!references[d].data || !dev->read(0, blocks, references[d].data)) continue;
    references[d].blocks = blocks;
    references[d].valid = true;
    for (uint32_t r = 0; r < kReadersPerDevice; ++r) {
      StorageJob& job = jobs[job_count];
      job = {dev, &gate, references[d].data, blocks, info.block_size, 31 * (d + 1) + r, 120, false,
             static_cast<uint8_t*>(allocator.allocate(uint64_t{17} * info.block_size))};
      threads[job_count] = job.scratch ? spawn(storage_read_main, &job, "storage") : nullptr;
      ++job_count;
    }
  }

  // Writers: on the first device that carries the scratch marker.
  WriteJob writes[kWriters];
  Thread* write_threads[kWriters] = {};
  uint32_t writer_count = 0;
  hardware::IStorageDevice* scratch_device = nullptr;
  for (uint32_t d = 0; d < devices && !scratch_device; ++d) {
    hardware::IStorageDevice* dev = storage.at(d);
    const hardware::StorageInfo info = dev->info();
    if (info.read_only || !references[d].valid) continue;
    if (__builtin_memcmp(references[d].data, "TCOS-SCRATCH", 12) != 0) continue;
    scratch_device = dev;
    const uint32_t region = 8;  // blocks per writer
    for (uint32_t w = 0; w < kWriters; ++w) {
      const uint32_t bytes = region * info.block_size;
      uint8_t* memory = static_cast<uint8_t*>(allocator.allocate(uint64_t{bytes} * 3));
      if (!memory) break;
      writes[w] = {dev, &gate, info.block_count - uint64_t{kWriters} * region + uint64_t{w} * region, region, info.block_size, w, false,
                   memory, memory + bytes, memory + 2 * bytes};
      write_threads[w] = spawn(storage_write_main, &writes[w], "writer");
      ++writer_count;
    }
  }

  gate.go.set();
  bool readers_ok = job_count > 0, writers_ok = true;
  for (uint32_t j = 0; j < job_count; ++j) {
    readers_ok &= threads[j] && join(threads[j], 60000000);
    readers_ok &= jobs[j].ok;
  }
  for (uint32_t w = 0; w < writer_count; ++w) {
    writers_ok &= write_threads[w] && join(write_threads[w], 60000000);
    writers_ok &= writes[w].ok;
    allocator.deallocate(writes[w].buffer);
  }
  for (uint32_t j = 0; j < job_count; ++j) allocator.deallocate(jobs[j].scratch);
  for (uint32_t d = 0; d < devices; ++d) allocator.deallocate(references[d].data);

  bool ok = true;
  {
    console::Guard guard;
    console::puts("  storage: ");
    console::put_dec(job_count);
    console::puts(" reader threads on ");
    console::put_dec(devices);
    console::puts(" devices");
    if (writer_count) {
      console::puts(", ");
      console::put_dec(writer_count);
      console::puts(" writers on one scratch device");
    }
    console::puts("\n");
  }
  ok &= report("storage: concurrent random reads match the reference", readers_ok);
  if (writer_count) ok &= report("storage: concurrent writes to disjoint regions verify and restore", writers_ok);
  return ok;
}

// ---------------------------------------------------------------- network

struct NetJob {
  network::NetworkStack* stack;
  StartGate* gate;
  uint32_t id;
  uint32_t successes;
  uint32_t attempts;
  bool ok;
};

constexpr uint16_t kHostUdpEcho = 7778, kHostTcpEcho = 7777;

void udp_main(void* argument) {
  NetJob* job = static_cast<NetJob*>(argument);
  job->gate->go.wait();
  SharedPointer<network::IUdpSocket> socket = job->stack->udp_open(0);
  const network::Ipv4Address gateway = job->stack->configuration().gateway;
  job->ok = static_cast<bool>(socket);
  for (uint32_t i = 0; socket && i < 10; ++i) {
    uint8_t message[24];
    for (uint32_t b = 0; b < sizeof(message); ++b) message[b] = static_cast<uint8_t>(job->id * 16 + i + b);
    ++job->attempts;
    if (!socket->send_to(gateway, kHostUdpEcho, message, sizeof(message))) continue;
    uint8_t reply[64];
    uint32_t length = 0;
    const uint64_t deadline = job->stack->now_ms() + 1000;
    bool got = false;
    while (!got && job->stack->now_ms() < deadline) {
      got = socket->receive_from(reply, sizeof(reply), nullptr, nullptr, &length);
      if (!got) sleep_us(300);
    }
    if (got) {
      if (length == sizeof(message) && __builtin_memcmp(reply, message, length) == 0) ++job->successes;
      else job->ok = false;  // an answer that is not ours: crossed streams
    }
  }
}

void dns_main(void* argument) {
  NetJob* job = static_cast<NetJob*>(argument);
  job->gate->go.wait();
  network::Ipv4Address address;
  ++job->attempts;
  const bool found = job->stack->resolve("localhost", 4000, &address);
  const bool missing = !job->stack->resolve("no-such-host.invalid", 4000, &address);
  job->ok = found && missing;
  job->successes = found && missing;
}

void ping_main(void* argument) {
  NetJob* job = static_cast<NetJob*>(argument);
  job->gate->go.wait();
  uint32_t rtt;
  ++job->attempts;
  job->successes = job->stack->ping(job->stack->configuration().gateway, 2000, &rtt);
  job->ok = job->successes == 1;
}

void tcp_main(void* argument) {
  NetJob* job = static_cast<NetJob*>(argument);
  job->gate->go.wait();
  job->ok = true;
  SharedPointer<network::ITcpSocket> tcp = job->stack->tcp_open();
  if (!tcp || !tcp->connect(job->stack->configuration().gateway, kHostTcpEcho)) return;
  ++job->attempts;
  uint8_t message[40];
  for (uint32_t b = 0; b < sizeof(message); ++b) message[b] = static_cast<uint8_t>(job->id * 29 + b);
  const uint64_t deadline = job->stack->now_ms() + 4000;
  bool sent = false;
  uint8_t reply[64];
  uint32_t got = 0;
  while (job->stack->now_ms() < deadline && got < sizeof(message)) {
    if (tcp->state() == network::TcpState::Closed) break;
    if (tcp->is_connected() && !sent) sent = tcp->send(message, sizeof(message)) > 0;
    got += tcp->receive(reply + got, sizeof(reply) - got);
    if (got < sizeof(message)) sleep_us(300);
  }
  if (got == sizeof(message)) {
    if (__builtin_memcmp(reply, message, got) == 0) job->successes = 1;
    else job->ok = false;
  }
  tcp->close();
}

bool run_net_group(const char* what, network::NetworkStack* stack, void (*function)(void*), uint32_t threads,
                   bool needs_server, allocators::IAllocator& allocator) {
  StartGate gate;
  NetJob* jobs = static_cast<NetJob*>(allocator.allocate(sizeof(NetJob) * threads));
  Thread** handles = static_cast<Thread**>(allocator.allocate(sizeof(Thread*) * threads));
  if (!jobs || !handles) return false;
  for (uint32_t i = 0; i < threads; ++i) {
    jobs[i] = {stack, &gate, i, 0, 0, false};
    handles[i] = spawn(function, &jobs[i], "net", -1, -1, 96 * 1024);
  }
  gate.go.set();
  bool ok = true;
  uint32_t successes = 0, attempts = 0;
  for (uint32_t i = 0; i < threads; ++i) {
    ok &= handles[i] && join(handles[i], 60000000);
    ok &= jobs[i].ok;
    successes += jobs[i].successes;
    attempts += jobs[i].attempts;
  }
  allocator.deallocate(jobs);
  allocator.deallocate(handles);
  if (needs_server && successes == 0) {
    console::puts("  ");
    console::puts(what);
    console::puts(": no echo server on the host (optional)\n");
    return ok;
  }
  if (needs_server && successes != attempts) ok = false;
  return report(what, ok);
}

bool test_network(network::NetworkStackSet& stacks, allocators::IAllocator& allocator) {
  network::NetworkStack* stack = nullptr;
  for (uint32_t i = 0; i < stacks.count() && !stack; ++i)
    if (stacks.at(i)->configuration().configured()) stack = stacks.at(i);
  if (!stack) return true;
  bool ok = true;
  ok &= run_net_group("network: 4 threads, UDP echoes on separate sockets", stack, udp_main, 4, true, allocator);
  ok &= run_net_group("network: 3 threads resolving names at once", stack, dns_main, 3, false, allocator);
  ok &= run_net_group("network: 3 threads pinging at once", stack, ping_main, 3, false, allocator);
  ok &= run_net_group("network: 3 TCP echo connections at once", stack, tcp_main, 3, true, allocator);
  return ok;
}

}  // namespace

bool run_thread_safety_demo(hardware::SystemStorageDevices& storage, network::NetworkStackSet& stacks,
                            allocators::IAllocator& allocator) {
  bool ok = true;
  ok &= test_allocator(allocator);
  ok &= test_console();
  ok &= test_storage(storage, allocator);
  ok &= test_network(stacks, allocator);
  return ok;
}
