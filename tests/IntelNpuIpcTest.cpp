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


// Host test for the Intel NPU message channel, job queues and interrupts: a pretend firmware reads the requests the host leaves in the shared memory, answers
// through the receive FIFO the way the real one does, consumes job queues on a doorbell and reports the jobs done.
#include <stdio.h>
#include <deque>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>
#include "Hardware/NeuralProcessingUnit/Intel/IntelNpuIrq.hpp"
#include "Hardware/NeuralProcessingUnit/Intel/IntelNpuJobs.hpp"

using namespace hardware;

static int g_failed = 0;
#define CHECK(...) do { if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)

struct TestAllocator final : allocators::IAllocator {
  void* allocate(uint64_t size) override { return malloc(size); }
  void* allocate_aligned(uint64_t size, uint64_t alignment) override { const uint64_t al = alignment < 16 ? 16 : alignment; return aligned_alloc(al, (size + al - 1) / al * al); }
  void deallocate(void* ptr) override { free(ptr); }
};

struct SimBar final : INpuRegisters {
  std::map<uint64_t, uint32_t> regs;
  std::map<uint64_t, uint64_t> writes64;
  std::set<uint64_t> touched;
  std::vector<std::pair<uint64_t, uint32_t>> log;   // every 32-bit write in order
  std::function<void(SimBar&, uint64_t, uint32_t)> on_write;
  std::function<bool(SimBar&, uint64_t, uint32_t*)> on_read;
  bool chaos = false;
  uint64_t rng = 0x9E3779B97F4A7C15ull;
  uint32_t read32(uint64_t offset) override {
    touched.insert(offset);
    uint32_t v = 0;
    if (on_read && on_read(*this, offset, &v)) return v;
    if (chaos) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return static_cast<uint32_t>(rng); }
    auto it = regs.find(offset);
    return it == regs.end() ? 0 : it->second;
  }
  void write32(uint64_t offset, uint32_t value) override {
    touched.insert(offset);
    log.push_back({offset, value});
    regs[offset] = value;
    if (on_write) on_write(*this, offset, value);
  }
  void write64(uint64_t offset, uint64_t value) override {
    touched.insert(offset);
    writes64[offset] = value;
    regs[offset] = static_cast<uint32_t>(value);
    regs[offset + 4] = static_cast<uint32_t>(value >> 32);
  }
};

static uint32_t ld32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (uint32_t{p[3]} << 24); }
static uint64_t ld64(const uint8_t* p) { return ld32(p) | (uint64_t{ld32(p + 4)} << 32); }
static void st32(uint8_t* p, uint32_t v) { for (int i = 0; i < 4; ++i) p[i] = static_cast<uint8_t>(v >> (8 * i)); }
static void st16(uint8_t* p, uint16_t v) { p[0] = static_cast<uint8_t>(v); p[1] = static_cast<uint8_t>(v >> 8); }

constexpr uint32_t kSlot = 192;

// The NPU side of the channel and the job queues.
struct SimFirmware {
  SimBar bar;
  IntelNpuIp gen;
  std::vector<uint8_t> tx = std::vector<uint8_t>(16384), rx = std::vector<uint8_t>(16384), jobq = std::vector<uint8_t>(4096);
  uint32_t tx_vpu = 0x90000000, rx_vpu = 0x90004000, jobq_vpu = 0x90010000;
  std::deque<uint32_t> fifo;
  uint32_t rx_next = 0;
  // behaviour
  bool silent = false, stall_jobs = false;
  uint32_t force_result = 0, force_type = 0, job_status = 0;
  uint32_t overruns = 0;
  // what it saw
  std::vector<IntelNpuJsmMessage> requests;
  std::vector<uint32_t> request_channels;
  std::vector<uint32_t> tx_addresses;
  std::vector<uint32_t> doorbells;
  uint32_t fail_type = 0;         // answer this request type with an error result
  uint32_t jobs_run = 0;
  struct Db { uint32_t id, ssid; uint64_t base, size; };
  std::vector<Db> dbs;
  bool tx_state_ok = true;

  explicit SimFirmware(IntelNpuIp g) : gen(g) {
    bar.on_read = [this](SimBar&, uint64_t off, uint32_t* v) {
      if (off == IntelNpuIpc::kRxFifoStatus) { *v = static_cast<uint32_t>(fifo.size() > 255 ? 255 : fifo.size()) << 16; return true; }
      if (off == IntelNpuIpc::kRxFifoAddress) { if (fifo.empty()) { *v = 0; } else { *v = fifo.front(); fifo.pop_front(); } return true; }
      return false;
    };
    bar.on_write = [this](SimBar&, uint64_t off, uint32_t v) {
      if (off == (gen == IntelNpuIp::Ip37xx ? IntelNpuIpc::kTx37xx : IntelNpuIpc::kTxLater)) on_tx(v);
      const uint64_t db_base = gen == IntelNpuIp::Ip37xx ? IntelNpuDoorbells::kBase37xx : IntelNpuDoorbells::kBaseLater;
      if (off >= db_base && off < db_base + 256 * IntelNpuDoorbells::kStride && ((off - db_base) % IntelNpuDoorbells::kStride) == 0 && v == 1)
        on_doorbell(static_cast<uint32_t>((off - db_base) / IntelNpuDoorbells::kStride));
    };
  }

  uint8_t* host(uint32_t vpu, uint32_t size) {
    if (vpu >= tx_vpu && vpu - tx_vpu + size <= tx.size()) return tx.data() + (vpu - tx_vpu);
    if (vpu >= rx_vpu && vpu - rx_vpu + size <= rx.size()) return rx.data() + (vpu - rx_vpu);
    if (vpu >= jobq_vpu && vpu - jobq_vpu + size <= jobq.size()) return jobq.data() + (vpu - jobq_vpu);
    return nullptr;
  }
  // Puts a message in the receive area and its address in the FIFO. Returns false when the host has not freed the next slot yet.
  bool deliver(uint32_t channel, const IntelNpuJsmMessage* msg, uint32_t boot_data = 0) {
    const uint32_t slot = rx_next;
    uint8_t* base = rx.data() + slot * kSlot;
    if (base[12] != 0 || base[64 + 12] != 0) { ++overruns; return false; }   // not released by the host
    rx_next = (rx_next + 1) % (rx.size() / kSlot);
    std::fill(base, base + kSlot, 0);
    st32(base + 0, msg ? rx_vpu + slot * kSlot + 64 : boot_data);
    st32(base + 4, msg ? 128 : 0);
    st16(base + 8, static_cast<uint16_t>(channel));
    base[10] = 1; base[11] = 0; base[12] = 1;
    if (msg) { std::copy(msg->bytes, msg->bytes + 128, base + 64); st32(base + 64 + 12, 1); }
    fifo.push_back(rx_vpu + slot * kSlot);
    return true;
  }
  void boot_message(uint32_t data = IntelNpuIpc::kBootMessageData) { deliver(IntelNpuIpc::kBootChannel, nullptr, data); }

  void on_tx(uint32_t slot_vpu) {
    tx_addresses.push_back(slot_vpu);
    const uint8_t* p = host(slot_vpu, kSlot);
    if (!p) { tx_state_ok = false; return; }
    IntelNpuJsmMessage req;
    std::copy(p + 64, p + 64 + 128, req.bytes);
    requests.push_back(req);
    request_channels.push_back(p[8] | (p[9] << 8));
    if (ld32(p) != slot_vpu + 64 || ld32(p + 4) != 128 || p[10] != 0 || p[11] != 1 || p[12] != 1 || ld32(p + 64 + 12) != 1) tx_state_ok = false;
    if (silent) return;
    IntelNpuJsmMessage rsp;
    rsp.set_type(force_type ? force_type : IntelNpuJsm::response_type(req.type()));
    rsp.set_request_id(req.request_id());
    rsp.set_result(req.type() == fail_type ? 2 : force_result);
    if (req.type() == IntelNpuJsm::kQueryEngineHeartbeat) rsp.put64(8, 0xABCDEF0123ull);
    if (req.type() == IntelNpuJsm::kRegisterDb) dbs.push_back({req.get32(0), req.get32(20), req.get64(8), req.get32(16)});
    if (req.type() == IntelNpuJsm::kHwsRegisterDb) dbs.push_back({req.get32(0), req.get32(4), req.get64(16), req.get64(24)});
    deliver(p[8] | (p[9] << 8), &rsp);
  }

  void on_doorbell(uint32_t id) {
    doorbells.push_back(id);
    if (stall_jobs) return;
    const Db* db = nullptr;
    for (const Db& d : dbs) if (d.id == id) db = &d;
    if (!db) return;
    uint8_t* q = host(static_cast<uint32_t>(db->base), static_cast<uint32_t>(db->size));
    if (!q) return;
    const uint32_t entries = static_cast<uint32_t>((db->size - 64) / 64);
    uint32_t head = ld32(q + 4);
    const uint32_t tail = ld32(q + 8);
    while (head != tail) {
      const uint8_t* e = q + 64 + head * 64;
      IntelNpuJsmMessage done;
      done.set_type(IntelNpuJsm::kJobDone);
      done.put32(0, 0); done.put32(4, id); done.put32(8, ld32(e + 8)); done.put32(12, job_status); done.put32(16, db->ssid); done.put64(24, 7);
      deliver(IntelNpuJsm::kChannelJobReturn, &done);
      ++jobs_run;
      head = (head + 1) % entries;
    }
    st32(q + 4, head);
  }
};

// Every receive slot has its header and message marked free again.
static bool released(const SimFirmware& fw) {
  for (size_t i = 0; i + kSlot <= fw.rx.size(); i += kSlot)
    if (fw.rx[i + 12] != 0 || fw.rx[i + 64 + 12] != 0) return false;
  return true;
}

struct Setup {
  SimFirmware fw;
  IntelNpuIpc ipc;
  explicit Setup(IntelNpuIp gen = IntelNpuIp::Ip40xx, bool init = true) : fw(gen), ipc(fw.bar, gen, true) {
    if (init) {
      CHECK(ipc.init(ByteView(fw.tx.data(), fw.tx.size()), fw.tx_vpu, ByteView(fw.rx.data(), fw.rx.size()), fw.rx_vpu));
      ipc.enable();
    }
  }
};

static void test_jsm() {
  using J = IntelNpuJsm;
  IntelNpuJsmMessage m = J::register_db(7, 5, 0x1122334455667788ull, 4096);
  CHECK(m.type() == 0x1102 && m.get32(0) == 7 && m.get64(8) == 0x1122334455667788ull && m.get32(16) == 4096 && m.get32(20) == 5);
  CHECK(ld32(m.bytes + 32) == 7 && ld64(m.bytes + 40) == 0x1122334455667788ull && ld32(m.bytes + 8) == 0x1102);   // payload at 32, type at 8
  m = J::hws_register_db(9, 4, 33, 0xAABB0000ull, 4096);
  CHECK(m.type() == 0x1117 && m.get32(0) == 9 && m.get32(4) == 4 && m.get64(8) == 33 && m.get64(16) == 0xAABB0000ull && m.get64(24) == 4096);
  m = J::hws_create_cmdq(4, 4, 33, 1234, 0, 0x90010000, 4096);
  CHECK(m.type() == 0x1114 && m.get64(0) == 1234 && m.get32(8) == 4 && m.get32(12) == 0 && m.get64(16) == 4 && m.get64(24) == 33 && m.get64(32) == 0x90010000 && m.get32(40) == 4096);
  m = J::hws_destroy_cmdq(4, 33);
  CHECK(m.type() == 0x1115 && m.get32(0) == 4 && m.get64(8) == 33);
  m = J::hws_set_context_sched_properties(4, 33, 2);
  CHECK(m.type() == 0x1116 && m.get32(0) == 4 && m.get64(8) == 33 && m.get32(16) == 2 && m.get64(32) == 20000 && m.get64(40) == 10000 && m.get64(48) == 0);
  { IntelNpuJsm::PriorityBands bands;
    bands.grace_period[1] = 111; bands.process_quantum[2] = 222; bands.process_grace_period[3] = 333; bands.normal_band_percentage = 25;
    m = J::priority_band_setup(bands);
    CHECK(m.type() == 0x1113 && m.get32(4) == 111 && m.get32(16 + 8) == 222 && m.get32(32 + 12) == 333 && m.get32(48) == 25 && J::response_type(0x1113) == 0x2214); }
  m = J::freq_config(8, 16, 48);
  CHECK(m.type() == 0x111F && m.get32(0) == 8 && m.get32(4) == 16 && m.get32(8) == 48);
  CHECK(J::engine_preempt(0, 3).get32(4) == 3 && J::ssid_release(6).get32(0) == 6 && J::unregister_db(8).get32(0) == 8 && J::pwr_d0i3_enter().get32(0) == 1);
  // Out-of-payload offsets read zero and write nothing.
  IntelNpuJsmMessage z;
  z.put32(93, 5); z.put64(90, 5); z.put32(96, 5);
  bool clean = true;
  for (uint8_t b : z.bytes) clean = clean && b == 0;
  CHECK(clean && z.get32(93) == 0 && z.get64(90) == 0);
  CHECK(J::response_type(0x1102) == 0x2202 && J::response_type(0x1117) == 0x2202 && J::response_type(0x1202) == 0x2302 && J::response_type(0x1234) == 0);
  CHECK(J::channel_for(0x1202) == 10 && J::channel_for(0x1102) == 0);
  CHECK(std::string(J::type_name(0x1104)) == "QUERY_ENGINE_HB" && std::string(J::type_name(0xBEEF)) == "UNKNOWN");
  IntelNpuJsmMessage done;
  done.put32(4, 3); done.put32(8, 0x101); done.put32(12, 0xE); done.put32(16, 2); done.put64(24, 99);
  const J::JobDone d = J::parse_job_done(done);
  CHECK(d.db_id == 3 && d.job_id == 0x101 && d.job_status == 0xE && d.ssid == 2 && d.cmdq_id == 99);
}

static void test_ipc_basics() {
  for (IntelNpuIp gen : {IntelNpuIp::Ip37xx, IntelNpuIp::Ip40xx, IntelNpuIp::Ip60xx}) {
    Setup s(gen);
    IntelNpuJsmMessage rsp;
    CHECK(s.ipc.on());
    // A request goes out in a slot with the header the firmware expects, and the slot's address in the right FIFO register.
    CHECK(s.ipc.transact(IntelNpuJsm::register_db(3, 2, 0x90010000, 4096), &rsp, 10) == IntelNpuIpcStatus::Ok);
    CHECK(s.fw.tx_state_ok && s.fw.tx_addresses.size() == 1 && s.fw.tx_addresses[0] == s.fw.tx_vpu);
    CHECK(rsp.type() == 0x2202 && rsp.request_id() == 1 && s.fw.requests[0].request_id() == 1 && s.fw.request_channels[0] == 0);
    CHECK(s.fw.requests[0].get32(0) == 3 && s.fw.requests[0].get64(8) == 0x90010000 && s.fw.requests[0].get32(20) == 2);
    CHECK(s.ipc.pending_messages() == 0);
    CHECK(released(s.fw));   // every received message was released
    // Ids count up and a consumer that is gone has released its slot: the next request reuses it.
    CHECK(s.ipc.transact(IntelNpuJsm::query_heartbeat(0), &rsp, 10) == IntelNpuIpcStatus::Ok && rsp.request_id() == 2 && IntelNpuJsm::parse_heartbeat(rsp) == 0xABCDEF0123ull);
    CHECK(s.fw.tx_addresses[1] == s.fw.tx_vpu);
    // The power message goes out on the general command channel.
    CHECK(s.ipc.transact(IntelNpuJsm::pwr_d0i3_enter(), &rsp, 10) == IntelNpuIpcStatus::Ok && s.fw.request_channels[2] == 10 && rsp.type() == 0x2302);
  }
  Setup s;
  IntelNpuJsmMessage rsp;
  // Unknown request types and missing output are refused before anything is sent.
  IntelNpuJsmMessage junk; junk.set_type(0x1234);
  CHECK(s.ipc.transact(junk, &rsp, 10) == IntelNpuIpcStatus::Invalid && s.ipc.transact(IntelNpuJsm::query_heartbeat(0), nullptr, 10) == IntelNpuIpcStatus::Invalid && s.fw.requests.empty());
  // Off: nothing is sent.
  s.ipc.disable();
  CHECK(s.ipc.transact(IntelNpuJsm::query_heartbeat(0), &rsp, 10) == IntelNpuIpcStatus::NotOn && s.fw.requests.empty());
  s.ipc.enable();
  // No answer.
  s.fw.silent = true;
  CHECK(s.ipc.transact(IntelNpuJsm::query_heartbeat(0), &rsp, 50) == IntelNpuIpcStatus::Timeout);
  s.fw.silent = false;
  // Wrong reply type; error result (the message is still given to the caller).
  s.fw.force_type = 0x2204;
  CHECK(s.ipc.transact(IntelNpuJsm::unregister_db(3), &rsp, 10) == IntelNpuIpcStatus::WrongResponse);
  s.fw.force_type = 0;
  s.fw.fail_type = IntelNpuJsm::kUnregisterDb;
  CHECK(s.ipc.transact(IntelNpuJsm::unregister_db(3), &rsp, 10) == IntelNpuIpcStatus::BadResult && rsp.result() == 2 && rsp.type() == 0x2203);
  s.fw.fail_type = 0;
  // Replies that are nobody's (another request id, another channel) are dropped and released, and do not disturb the real one.
  IntelNpuJsmMessage stray; stray.set_type(0x2204); stray.set_request_id(999);
  s.fw.deliver(0, &stray);
  s.fw.deliver(11, &stray);
  CHECK(s.ipc.transact(IntelNpuJsm::query_heartbeat(0), &rsp, 10) == IntelNpuIpcStatus::Ok && rsp.request_id() != 999);
  CHECK(s.ipc.pending_messages() == 0);
  CHECK(released(s.fw));
  // The slots of the transmit area run out when consumers keep their requests.
  std::vector<IntelNpuIpcConsumer> many(IntelNpuIpc::kMaxConsumers);
  uint32_t added = 0, sent = 0;
  for (auto& c : many) added += s.ipc.add_consumer(c, 0);
  CHECK(added == IntelNpuIpc::kMaxConsumers);
  IntelNpuIpcConsumer extra;
  CHECK(!s.ipc.add_consumer(extra, 0));
  CHECK(s.ipc.transact(IntelNpuJsm::query_heartbeat(0), &rsp, 10) == IntelNpuIpcStatus::NoConsumerSlot);
  for (auto& c : many) { IntelNpuJsmMessage r = IntelNpuJsm::query_heartbeat(0); sent += s.ipc.send(c, r) == IntelNpuIpcStatus::Ok; }
  CHECK(sent == 32);
  for (auto& c : many) s.ipc.remove_consumer(c);
  s.ipc.irq_handler();
  CHECK(s.ipc.pending_messages() == 0);   // all 32 replies found their consumers gone and were released
  // A second send by one consumer replaces its first (one slot per consumer).
  IntelNpuIpcConsumer one;
  CHECK(s.ipc.add_consumer(one, 0) && !s.ipc.add_consumer(one, 0));
  IntelNpuJsmMessage r1 = IntelNpuJsm::query_heartbeat(0), r2 = IntelNpuJsm::query_heartbeat(0);
  const size_t before = s.fw.tx_addresses.size();
  CHECK(s.ipc.send(one, r1) == IntelNpuIpcStatus::Ok && s.ipc.send(one, r2) == IntelNpuIpcStatus::Ok && s.fw.tx_addresses[before] == s.fw.tx_addresses[before + 1]);
  s.ipc.irq_handler();
  IntelNpuIpcHeader hdr;
  CHECK(s.ipc.receive(one, &hdr, &rsp) == IntelNpuIpcStatus::Ok && rsp.request_id() == r2.request_id() && hdr.channel == 0 && hdr.src_node == 1 && hdr.dst_node == 0);
  CHECK(s.ipc.receive(one, &hdr, &rsp) == IntelNpuIpcStatus::Empty);
  s.ipc.remove_consumer(one);
  CHECK(s.ipc.receive(one, &hdr, &rsp) == IntelNpuIpcStatus::Invalid);
}

static void test_ipc_init_and_state() {
  SimFirmware fw(IntelNpuIp::Ip40xx);
  IntelNpuIpc ipc(fw.bar, IntelNpuIp::Ip40xx, true);
  ByteView tx(fw.tx.data(), fw.tx.size()), rx(fw.rx.data(), fw.rx.size());
  CHECK(!ipc.init(tx.first(100), fw.tx_vpu, rx, fw.rx_vpu) && !ipc.init(tx, fw.tx_vpu, rx.first(100), fw.rx_vpu));   // too small
  CHECK(!ipc.init(tx, fw.tx_vpu + 8, rx, fw.rx_vpu) && !ipc.init(tx, fw.tx_vpu, rx, fw.rx_vpu + 32));               // not 64-byte aligned
  CHECK(!ipc.init(tx, 0xFFFFFFC0u, rx, fw.rx_vpu) && !ipc.init(tx, fw.tx_vpu, rx, 0xFFFFFFC0u));                     // would pass 4 GiB
  CHECK(!ipc.reset());
  CHECK(ipc.init(tx, fw.tx_vpu, rx, fw.rx_vpu) && !ipc.on());
  // reset() clears both areas and only works while off.
  fw.tx[5] = 9; fw.rx[7] = 9;
  CHECK(ipc.reset() && fw.tx[5] == 0 && fw.rx[7] == 0);
  ipc.enable();
  CHECK(!ipc.reset());
  // disable() aborts waiting consumers and frees what they had queued; callback consumers keep working after enable().
  IntelNpuIpcConsumer waiting, callback;
  struct Counter { int n = 0; } counter;
  CHECK(ipc.add_consumer(waiting, 0));
  CHECK(ipc.add_consumer(callback, 11, [](void* c, const IntelNpuIpcHeader&, const IntelNpuJsmMessage*) { ++static_cast<Counter*>(c)->n; }, &counter));
  IntelNpuJsmMessage req = IntelNpuJsm::query_heartbeat(0);
  CHECK(ipc.send(waiting, req) == IntelNpuIpcStatus::Ok);
  IntelNpuJsmMessage job; job.set_type(IntelNpuJsm::kJobDone);
  fw.deliver(11, &job);
  ipc.irq_handler();
  CHECK(ipc.pending_messages() == 2 && ipc.receive(callback, nullptr, nullptr) == IntelNpuIpcStatus::WrongMode);
  ipc.disable();
  CHECK(ipc.pending_messages() == 0 && ipc.receive(waiting, nullptr, nullptr) == IntelNpuIpcStatus::Aborted && !ipc.on());
  ipc.run_callbacks();
  CHECK(counter.n == 0);   // the queued callback was discarded
  CHECK(released(fw));
  CHECK(ipc.reset());
  // A callback message queued, then its consumer removed: it never runs and is released.
  ipc.enable();
  fw.deliver(11, &job);
  ipc.irq_handler();
  CHECK(ipc.pending_messages() == 1);
  ipc.remove_consumer(callback);
  CHECK(ipc.pending_messages() == 0);
  ipc.run_callbacks();
  CHECK(counter.n == 0);
  ipc.remove_consumer(waiting);
}

static void test_ipc_rx_robustness() {
  Setup s;
  IntelNpuIpcConsumer c, cb;
  struct Counter { int n = 0; } counter;
  CHECK(s.ipc.add_consumer(c, 0));
  IntelNpuJsmMessage req = IntelNpuJsm::query_heartbeat(0);
  s.fw.silent = true;
  CHECK(s.ipc.send(c, req) == IntelNpuIpcStatus::Ok);
  // More replies than the consumer queue holds: the rest are dropped and released.
  IntelNpuJsmMessage rsp; rsp.set_type(0x2204); rsp.set_request_id(req.request_id());
  for (int i = 0; i < 20; ++i) s.fw.deliver(0, &rsp);
  s.ipc.irq_handler();
  CHECK(s.ipc.pending_messages() == IntelNpuIpcConsumer::kQueueDepth && s.ipc.dropped_messages() == 4);
  int got = 0;
  while (s.ipc.receive(c, nullptr, nullptr) == IntelNpuIpcStatus::Ok) ++got;
  CHECK(got == 16 && s.ipc.pending_messages() == 0);
  CHECK(released(s.fw));
  s.ipc.remove_consumer(c);
  // Addresses the firmware should never send: outside the receive area, a header whose message points outside, a message cut off by the area's end.
  s.fw.fifo.push_back(0x12345678);
  s.fw.fifo.push_back(s.fw.rx_vpu + 16384 - 32);
  s.fw.fifo.push_back(0xFFFFFFFF);   // the device vanished: stop
  s.fw.fifo.push_back(s.fw.rx_vpu + 7);
  s.ipc.irq_handler();
  CHECK(s.ipc.pending_messages() == 0 && s.fw.fifo.size() == 1);   // stopped at the read error; the entry after it waits for the next interrupt
  s.fw.fifo.clear();
  {
    // A header at a valid place pointing to a message outside the area is freed and skipped.
    uint8_t* h = s.fw.rx.data();
    st32(h, 0x10); st32(h + 4, 128); st16(h + 8, 0); h[12] = 1;
    s.fw.fifo.push_back(s.fw.rx_vpu);
    s.ipc.irq_handler();
    CHECK(h[12] == 0 && s.ipc.pending_messages() == 0);
  }
  // A message that claims to be shorter than the full size: the missing part is zero, not stale.
  CHECK(s.ipc.add_consumer(c, 0));
  IntelNpuJsmMessage short_req = IntelNpuJsm::query_heartbeat(0);
  s.ipc.send(c, short_req);
  IntelNpuJsmMessage full; full.set_type(0x2204); full.set_request_id(short_req.request_id());
  for (int i = 32; i < 128; ++i) full.bytes[i] = 0xEE;
  s.fw.deliver(0, &full);
  st32(s.fw.rx.data() + s.fw.rx_next * 0 + (s.fw.rx_next + 84) % 85 * kSlot + 4, 40);   // data_size = 40
  s.ipc.irq_handler();
  IntelNpuIpcHeader hdr;
  CHECK(s.ipc.receive(c, &hdr, &rsp) == IntelNpuIpcStatus::Ok && hdr.data_size == 40 && rsp.bytes[39] == 0xEE && rsp.bytes[40] == 0 && rsp.bytes[100] == 0);
  s.ipc.remove_consumer(c);
  // Callback consumers: the interrupt handler only queues, run_callbacks() calls.
  CHECK(s.ipc.add_consumer(cb, 11, [](void* p, const IntelNpuIpcHeader& h, const IntelNpuJsmMessage* m) { ++static_cast<Counter*>(p)->n; if (!m || h.channel != 11) --static_cast<Counter*>(p)->n; }, &counter));
  IntelNpuJsmMessage job; job.set_type(IntelNpuJsm::kJobDone);
  for (int i = 0; i < 5; ++i) s.fw.deliver(11, &job);
  s.ipc.irq_handler();
  CHECK(counter.n == 0 && s.ipc.pending_messages() == 5);
  s.ipc.run_callbacks();
  CHECK(counter.n == 5 && s.ipc.pending_messages() == 0);
  s.ipc.remove_consumer(cb);
  // The FIFO drain is bounded even if the status register never says empty.
  s.fw.bar.on_read = [](SimBar&, uint64_t off, uint32_t* v) { *v = off == IntelNpuIpc::kRxFifoStatus ? 0x00FF0000u : 0x12345678u; return true; };
  s.ipc.irq_handler();
  CHECK(s.ipc.pending_messages() == 0);
}

static void test_boot_message() {
  {
    Setup s(IntelNpuIp::Ip40xx, true);
    s.ipc.disable();   // the boot message is awaited before enable()
    s.fw.boot_message();
    CHECK(s.ipc.wait_for_boot_message(10) == IntelNpuIpcStatus::Ok && s.ipc.pending_messages() == 0);
  }
  {
    Setup s;
    s.fw.boot_message(0x12345678);
    CHECK(s.ipc.wait_for_boot_message(10) == IntelNpuIpcStatus::Invalid);
    CHECK(s.ipc.wait_for_boot_message(20) == IntelNpuIpcStatus::Timeout);
    CHECK(released(s.fw));
  }
}

// ---- job queues ----

static void test_job_queue() {
  std::vector<uint8_t> mem(4096, 0xCC);
  IntelNpuCommandQueue q;
  CHECK(!q.ready() && q.push(IntelNpuJob()) == IntelNpuCommandQueue::PushResult::NotReady && q.pending() == 0);
  CHECK(!q.init(ByteView(mem.data(), 100), 0x90010000, 0, true) && !q.init(ByteView(mem.data(), mem.size()), 0x90010020, 0, true));
  CHECK(q.init(ByteView(mem.data(), mem.size()), 0x90010000, 4, true) && q.ready() && q.entry_count() == 63 && q.vpu_address() == 0x90010000);
  CHECK(ld32(mem.data() + 0) == 0 && ld32(mem.data() + 4) == 0 && ld32(mem.data() + 8) == 0 && ld32(mem.data() + 12) == 4 && mem[20] == 0);
  IntelNpuJob j;
  j.batch_buffer = 0x1122334455ull; j.job_id = 0x101; j.primary_preempt_addr = 0xAA00; j.primary_preempt_size = 0x1000; j.secondary_preempt_addr = 0xBB00; j.secondary_preempt_size = 0x2000;
  CHECK(q.push(j) == IntelNpuCommandQueue::PushResult::Ok && q.pending() == 1);
  const uint8_t* e = mem.data() + 64;
  CHECK(ld64(e) == 0x1122334455ull && ld32(e + 8) == 0x101 && ld32(e + 12) == 0 && ld64(e + 16) == 0 && ld64(e + 24) == 0 && ld64(e + 32) == 0xAA00 && ld32(e + 40) == 0x1000 &&
        ld32(e + 44) == 0x2000 && ld64(e + 48) == 0xBB00 && ld64(e + 56) == 0 && ld32(mem.data() + 8) == 1);
  j.null_submission = true; j.job_id = 5;
  CHECK(q.push(j) == IntelNpuCommandQueue::PushResult::Ok && ld32(mem.data() + 64 + 64 + 12) == 1 && ld32(mem.data() + 8) == 2);
  // Fill to capacity: one slot always stays empty.
  uint32_t pushed = 2;
  while (q.push(j) == IntelNpuCommandQueue::PushResult::Ok) ++pushed;
  CHECK(pushed == 62 && q.pending() == 62);
  // The NPU consumes some; space comes back and the tail wraps.
  st32(mem.data() + 4, 10);
  CHECK(q.pending() == 52 && q.push(j) == IntelNpuCommandQueue::PushResult::Ok);
  for (int i = 0; i < 9; ++i) CHECK(q.push(j) == IntelNpuCommandQueue::PushResult::Ok);   // 52 pending + 1 + 9 = 62: still not full
  CHECK(q.pending() == 62 && q.push(j) == IntelNpuCommandQueue::PushResult::Full);
  // A head outside the queue means the NPU misbehaves.
  st32(mem.data() + 4, 63);
  CHECK(q.pending() == UINT32_MAX && q.push(j) == IntelNpuCommandQueue::PushResult::Corrupt);
  st32(mem.data() + 4, 0xFFFFFFFF);
  CHECK(q.push(j) == IntelNpuCommandQueue::PushResult::Corrupt);
  q.reset();
  CHECK(q.pending() == 0 && ld32(mem.data() + 4) == 0 && ld32(mem.data() + 8) == 0 && q.push(j) == IntelNpuCommandQueue::PushResult::Ok);
}

static void test_doorbells() {
  IntelNpuDoorbells d;
  uint32_t id = 0, seen_first = 0;
  CHECK(d.allocate(&id) && id == 1 && d.allocate(&id) && id == 2 && d.in_use() == 2);
  d.release(1);
  CHECK(d.allocate(&id) && id == 3);   // cyclic: does not reuse 1 at once
  d.release(0); d.release(256); d.release(77);
  CHECK(d.in_use() == 2);
  uint32_t n = 0;
  while (d.allocate(&id)) { ++n; if (!seen_first) seen_first = id; }
  CHECK(n == 253 && d.in_use() == 255 && !d.allocate(&id));
  d.release(200);
  CHECK(d.allocate(&id) && id == 200);
  SimBar bar;
  CHECK(IntelNpuDoorbells::ring(bar, IntelNpuIp::Ip37xx, 5) && bar.regs[0x06300000 + 5 * 0x1000] == 1);
  CHECK(IntelNpuDoorbells::ring(bar, IntelNpuIp::Ip40xx, 255) && bar.regs[0x01300000 + 255 * 0x1000] == 1);
  CHECK(!IntelNpuDoorbells::ring(bar, IntelNpuIp::Ip40xx, 0) && !IntelNpuDoorbells::ring(bar, IntelNpuIp::Ip40xx, 256) && !IntelNpuDoorbells::ring(bar, IntelNpuIp::Unknown, 1));
  CHECK(IntelNpuHw::operational_window(IntelNpuIp::Ip37xx) >= IntelNpuDoorbells::window_end(IntelNpuIp::Ip37xx) &&
        IntelNpuHw::operational_window(IntelNpuIp::Ip60xx) >= IntelNpuDoorbells::window_end(IntelNpuIp::Ip60xx) && IntelNpuHw::operational_window(IntelNpuIp::Unknown) == 0);
}

static std::vector<uint32_t> types_of(const SimFirmware& fw) {
  std::vector<uint32_t> t;
  for (const auto& r : fw.requests) t.push_back(r.type());
  return t;
}

static void test_queue_registration() {
  for (bool hws : {true, false}) {
    Setup s;
    IntelNpuDoorbells dbs;
    IntelNpuQueueManager mgr(s.ipc, dbs, 10);
    IntelNpuCommandQueue q;
    CHECK(q.init(ByteView(s.fw.jobq.data(), s.fw.jobq.size()), s.fw.jobq_vpu, 0, true));
    IntelNpuRegisteredQueue reg;
    reg.ssid = 4; reg.cmdq_id = 33; reg.hardware_scheduler = hws; reg.priority_band = IntelNpuJsm::kBandFocus;
    CHECK(mgr.register_queue(reg, q, 99) == IntelNpuQueueStatus::Ok && reg.doorbell == 1 && dbs.in_use() == 1);
    const std::vector<uint32_t> expected = hws ? std::vector<uint32_t>{0x1114, 0x1116, 0x1117} : std::vector<uint32_t>{0x1102};
    CHECK(types_of(s.fw) == expected);
    if (hws) {
      CHECK(s.fw.requests[0].get64(0) == 99 && s.fw.requests[0].get64(24) == 33 && s.fw.requests[0].get64(32) == s.fw.jobq_vpu && s.fw.requests[0].get32(40) == 4096);
      CHECK(s.fw.requests[1].get32(16) == 2 && s.fw.requests[2].get32(0) == 1 && s.fw.requests[2].get32(4) == 4 && s.fw.requests[2].get64(24) == 4096);
    } else {
      CHECK(s.fw.requests[0].get32(0) == 1 && s.fw.requests[0].get64(8) == s.fw.jobq_vpu && s.fw.requests[0].get32(16) == 4096 && s.fw.requests[0].get32(20) == 4);
    }
    CHECK(mgr.register_queue(reg, q, 99) == IntelNpuQueueStatus::Ok && s.fw.requests.size() == expected.size());   // already registered: nothing sent
    CHECK(mgr.unregister_queue(reg) == IntelNpuQueueStatus::Ok && reg.doorbell == 0 && dbs.in_use() == 0);
    const std::vector<uint32_t> unreg = hws ? std::vector<uint32_t>{0x1103, 0x1115} : std::vector<uint32_t>{0x1103};
    const std::vector<uint32_t> all = types_of(s.fw);
    CHECK(std::vector<uint32_t>(all.end() - unreg.size(), all.end()) == unreg);
    CHECK(mgr.unregister_queue(reg) == IntelNpuQueueStatus::Ok);
    // Not a user context, a queue that was never set up.
    IntelNpuRegisteredQueue bad; bad.ssid = 1;
    CHECK(mgr.register_queue(bad, q, 1) == IntelNpuQueueStatus::Invalid);
    IntelNpuCommandQueue empty;
    reg.ssid = 4;
    CHECK(mgr.register_queue(reg, empty, 1) == IntelNpuQueueStatus::Invalid);
    // The firmware refuses: everything done so far is undone and no doorbell stays taken.
    s.fw.requests.clear();
    s.fw.fail_type = hws ? IntelNpuJsm::kHwsRegisterDb : IntelNpuJsm::kRegisterDb;
    CHECK(mgr.register_queue(reg, q, 1) == IntelNpuQueueStatus::Ipc && reg.doorbell == 0 && dbs.in_use() == 0 && reg.last_ipc_status == IntelNpuIpcStatus::BadResult);
    if (hws) {
      const std::vector<uint32_t> undone = {0x1114, 0x1116, 0x1117, 0x1115};
      CHECK(types_of(s.fw) == undone);
    }
    s.fw.fail_type = IntelNpuJsm::kSetContextSchedProperties;
    s.fw.requests.clear();
    const IntelNpuQueueStatus st = mgr.register_queue(reg, q, 1);
    CHECK(hws ? (st == IntelNpuQueueStatus::Ipc && types_of(s.fw) == std::vector<uint32_t>({0x1114, 0x1116, 0x1115})) : st == IntelNpuQueueStatus::Ok);
    s.fw.fail_type = 0;
    // Out of doorbells.
    IntelNpuDoorbells full;
    uint32_t id;
    while (full.allocate(&id)) {}
    IntelNpuQueueManager mgr2(s.ipc, full, 10);
    IntelNpuRegisteredQueue r2; r2.ssid = 4; r2.hardware_scheduler = hws;
    s.fw.requests.clear();
    CHECK(mgr2.register_queue(r2, q, 1) == IntelNpuQueueStatus::NoDoorbell && r2.doorbell == 0);
    // No firmware: the call times out.
    s.fw.silent = true;
    IntelNpuDoorbells d3;
    IntelNpuQueueManager mgr3(s.ipc, d3, 20);
    IntelNpuRegisteredQueue r3; r3.ssid = 4; r3.hardware_scheduler = hws;
    CHECK(mgr3.register_queue(r3, q, 1) == IntelNpuQueueStatus::Ipc && r3.last_ipc_status == IntelNpuIpcStatus::Timeout && d3.in_use() == 0);
  }
}

static void test_jobs_end_to_end() {
  Setup s;
  TestAllocator alloc;
  IntelNpuDoorbells dbs;
  IntelNpuQueueManager mgr(s.ipc, dbs, 10);
  IntelNpuCommandQueue q;
  CHECK(q.init(ByteView(s.fw.jobq.data(), s.fw.jobq.size()), s.fw.jobq_vpu, 0, true));
  IntelNpuRegisteredQueue reg; reg.ssid = 2; reg.cmdq_id = 1; reg.hardware_scheduler = true;
  IntelNpuJobTracker tracker(alloc);
  IntelNpuJobDoneSink sink; sink.tracker = &tracker;
  IntelNpuIpcConsumer job_consumer;
  CHECK(s.ipc.add_consumer(job_consumer, IntelNpuJsm::kChannelJobReturn, &IntelNpuJobDoneSink::on_message, &sink));
  IntelNpuJob job; job.batch_buffer = 0x91000000;
  uint32_t id = 0;
  CHECK(intel_npu_submit_job(s.fw.bar, IntelNpuIp::Ip40xx, q, reg, tracker, job, &id) == IntelNpuSubmit::NotRegistered);   // no doorbell yet
  CHECK(mgr.register_queue(reg, q, 1) == IntelNpuQueueStatus::Ok);
  // Submit: id from the context, entry in the queue, doorbell rung.
  CHECK(intel_npu_submit_job(s.fw.bar, IntelNpuIp::Ip40xx, q, reg, tracker, job, &id) == IntelNpuSubmit::Ok && id == 0x100 && tracker.in_flight() == 1);
  CHECK(ld32(s.fw.jobq.data() + 64 + 8) == 0x100 && ld64(s.fw.jobq.data() + 64) == 0x91000000 && s.fw.doorbells.size() == 1 && s.fw.doorbells[0] == reg.doorbell);
  CHECK(IntelNpuJobTracker::context_of(id) == 2);
  IntelNpuJobResult res;
  CHECK(!tracker.take_result(id, &res));   // not done yet
  s.ipc.irq_handler();
  s.ipc.run_callbacks();
  CHECK(sink.completed == 1 && sink.last.outcome == IntelNpuJobOutcome::Completed && sink.last.job_id == id && sink.last.ssid == 2);
  CHECK(tracker.take_result(id, &res) && res.outcome == IntelNpuJobOutcome::Completed && res.status == 0 && tracker.in_flight() == 0 && !tracker.take_result(id, &res));
  // Several jobs, ids in order; a faulting one is reported as needing a reset.
  s.fw.job_status = 0xE;
  uint32_t a = 0, b = 0;
  CHECK(intel_npu_submit_job(s.fw.bar, IntelNpuIp::Ip40xx, q, reg, tracker, job, &a) == IntelNpuSubmit::Ok && intel_npu_submit_job(s.fw.bar, IntelNpuIp::Ip40xx, q, reg, tracker, job, &b) == IntelNpuSubmit::Ok);
  CHECK(a == 0x101 && b == 0x102);
  s.ipc.irq_handler();
  s.ipc.run_callbacks();
  CHECK(sink.faults == 2 && tracker.take_result(a, &res) && res.outcome == IntelNpuJobOutcome::ContextFault && res.status == 0xE);
  // A repeated or invented completion changes nothing.
  IntelNpuJsm::JobDone dup; dup.job_id = b; dup.job_status = 0;
  CHECK(tracker.complete(dup).outcome == IntelNpuJobOutcome::Unknown);   // b was already completed above
  IntelNpuJsm::JobDone ghost; ghost.job_id = 0x5555; 
  CHECK(tracker.complete(ghost).outcome == IntelNpuJobOutcome::Unknown);
  CHECK(tracker.take_result(b, &res) && res.status == 0xE);
  IntelNpuJsmMessage junk; junk.set_type(0x1234);
  s.fw.deliver(11, &junk);
  IntelNpuJsmMessage bad_result = IntelNpuJsm::register_db(0, 0, 0, 0); bad_result.set_type(IntelNpuJsm::kJobDone); bad_result.set_result(2);
  s.fw.deliver(11, &bad_result);
  s.ipc.irq_handler();
  s.ipc.run_callbacks();
  CHECK(sink.ignored == 2);
  // Queue full: the id goes back.
  s.fw.stall_jobs = true;
  s.fw.job_status = 0;
  uint32_t n = 0;
  IntelNpuSubmit r;
  while ((r = intel_npu_submit_job(s.fw.bar, IntelNpuIp::Ip40xx, q, reg, tracker, job, &id)) == IntelNpuSubmit::Ok) ++n;
  CHECK(r == IntelNpuSubmit::QueueFull && n > 0 && tracker.in_flight() == n);
  s.fw.stall_jobs = false;
  // Context with 256 jobs in flight has no id left.
  IntelNpuJobTracker t2(alloc);
  uint32_t last = 0, count = 0;
  while (t2.begin(5, 1, &last)) ++count;
  CHECK(count == 256 && !t2.begin(0, 1, &last) && !t2.begin(1, 1, &last) && IntelNpuJobTracker::context_of(last) == 5);
  t2.abandon(last);
  CHECK(t2.begin(5, 1, &last) && t2.in_flight() == 256);
  // The job-done queue is not full; remove the consumer.
  s.ipc.remove_consumer(job_consumer);
  CHECK(mgr.unregister_queue(reg) == IntelNpuQueueStatus::Ok);
}

// ---- interrupts ----

static void test_irq() {
  const uint64_t both = (uint64_t{7} << 32) | 0xC00001F0ull;
  {
    SimBar ip, btrs;
    IntelNpuIrq irq(ip, btrs, intel_npu_platform(0x7D1D));
    irq.enable();
    CHECK(ip.regs[0x170] == 0x7F && ip.writes64[0x10240] == both && btrs.regs[0x30] == ~0x6u && btrs.regs[0x34] == 0);
    irq.clear();
    CHECK(ip.writes64[0x10220] == both);
    irq.disable();
    CHECK(btrs.regs[0x34] == 1 && btrs.regs[0x30] == 0xFFFFFFFFu && ip.writes64[0x10240] == 0 && ip.regs[0x170] == 0);
    irq.enable(true);
    CHECK(btrs.regs[0x30] == ~0x7u);
  }
  {
    SimBar ip, btrs;
    IntelNpuIrq irq(ip, btrs, intel_npu_platform(0x643E));
    irq.enable();
    CHECK(btrs.regs[0x04] == ~0x7Eu && btrs.regs[0x08] == 0);
    irq.disable();
    CHECK(btrs.regs[0x08] == 1 && btrs.regs[0x04] == 0xFFFFFFFFu);
  }
  // Nothing pending: not ours.
  {
    SimBar ip, btrs;
    IntelNpuIrq irq(ip, btrs, intel_npu_platform(0x643E));
    const IntelNpuIrqResult r = irq.handle();
    CHECK(!r.handled && !r.recovery_needed);
    CHECK(btrs.log.size() == 2 && btrs.log[0] == std::make_pair<uint64_t, uint32_t>(0x08, 1) && btrs.log[1] == std::make_pair<uint64_t, uint32_t>(0x08, 0));   // masked while looking, then unmasked
    IntelNpuIrq invalid(ip, btrs, IntelNpuPlatform());
    CHECK(!invalid.handle().handled);
  }
  // IP block: IPC drains the channel; MMU bits are reported; the status is cleared with what was read.
  {
    Setup s;
    TestAllocator alloc;
    SimBar btrs;
    IntelNpuIrq irq(s.fw.bar, btrs, intel_npu_platform(0x643E));
    IntelNpuMmu mmu(s.fw.bar, alloc, true);
    IntelNpuIpcConsumer c;
    CHECK(s.ipc.add_consumer(c, 0));
    IntelNpuJsmMessage req = IntelNpuJsm::query_heartbeat(0);
    s.fw.silent = true;
    s.ipc.send(c, req);
    IntelNpuJsmMessage rsp; rsp.set_type(0x2204); rsp.set_request_id(req.request_id());
    s.fw.deliver(0, &rsp);
    s.fw.bar.regs[0x10210] = 0x10 | 0x20 | 0x40 | 0x100 | 0x20000000;   // IPC, MMU event, MMU sync, firewall, and a bit that is not ours
    IntelNpuIrqResult r = irq.handle(&s.ipc, &mmu);
    CHECK(r.handled && r.ipc && r.mmu_events && r.mmu_sync && r.firewall && !r.mmu_errors && !r.recovery_needed && irq.firewall_count() == 1);
    CHECK(s.fw.bar.regs[0x10220] == (0x10 | 0x20 | 0x40 | 0x100));
    CHECK(s.ipc.receive(c, nullptr, nullptr) == IntelNpuIpcStatus::Ok);
    s.ipc.remove_consumer(c);
    // Watchdogs ask for recovery; an MMU global error is acknowledged when an MMU is given.
    s.fw.bar.regs[0x10210] = 0x80000000u | 0x40000000u | 0x80;
    s.fw.bar.regs[IntelNpuMmu::kRegGerror] = 0x4;
    r = irq.handle(&s.ipc, &mmu);
    CHECK(r.watchdog_mss && r.watchdog_nce && r.mmu_errors && r.recovery_needed);
    CHECK(s.fw.bar.regs[IntelNpuMmu::kRegGerrorn] == 0x4);
    // Without channel and MMU the flags are still set.
    s.fw.bar.regs[0x10210] = 0x10 | 0x80;
    r = irq.handle();
    CHECK(r.ipc && r.mmu_errors);
  }
  // Buttress errors: each source is cleared before the status.
  {
    SimBar ip, btrs;
    IntelNpuIrq irq(ip, btrs, intel_npu_platform(0x643E));
    irq.enable();
    btrs.regs[0x00] = 0x2 | 0x4 | 0x8 | 0x10 | 0x20 | 0x40 | 0x1 /* frequency change is masked off */ | 0x80;
    btrs.log.clear();
    IntelNpuIrqResult r = irq.handle();
    CHECK(r.handled && r.buttress_error && r.recovery_needed && r.survivability && !r.frequency_change);
    std::vector<uint64_t> order;
    for (auto& w : btrs.log) order.push_back(w.first);
    const std::vector<uint64_t> expected = {0x08, 0x18, 0x20, 0x44, 0x50, 0x60, 0x00, 0x08};
    CHECK(order == expected);
    CHECK(btrs.regs[0x00] == (0x2 | 0x4 | 0x8 | 0x10 | 0x20 | 0x40));   // exactly the enabled bits written back
  }
  {
    SimBar ip, btrs;
    IntelNpuIrq irq(ip, btrs, intel_npu_platform(0x7D1D));
    irq.enable(true);
    btrs.regs[0x04] = 0x1;   // frequency change alone: reported, no recovery
    IntelNpuIrqResult r = irq.handle();
    CHECK(r.handled && r.frequency_change && !r.buttress_error && !r.recovery_needed);
    btrs.regs[0x04] = 0x6;
    btrs.log.clear();
    r = irq.handle();
    CHECK(r.buttress_error && r.recovery_needed);
    std::vector<uint64_t> order;
    for (auto& w : btrs.log) order.push_back(w.first);
    const std::vector<uint64_t> expected = {0x34, 0xA8, 0xB4, 0x04, 0x34};
    CHECK(order == expected);
  }
}

// Whatever the registers read, the code ends and stays inside the registers it knows.
static void test_chaos() {
  for (uint32_t seed = 1; seed <= 6; ++seed) {
    Setup s(seed & 1 ? IntelNpuIp::Ip37xx : IntelNpuIp::Ip50xx);
    SimBar btrs;
    s.fw.bar.on_read = nullptr;
    s.fw.bar.chaos = true;
    s.fw.bar.rng = 0xABCDEFull * seed;
    btrs.chaos = true;
    btrs.rng = 0x1234567ull * seed;
    IntelNpuIrq irq(s.fw.bar, btrs, intel_npu_platform(seed & 1 ? 0x7D1D : 0xB03E));
    TestAllocator alloc;
    IntelNpuMmu mmu(s.fw.bar, alloc, true);
    IntelNpuJsmMessage rsp;
    for (int i = 0; i < 20; ++i) {
      (void)irq.handle(&s.ipc, &mmu);
      (void)s.ipc.transact(IntelNpuJsm::query_heartbeat(0), &rsp, 5);
      s.ipc.run_callbacks();
    }
    IntelNpuIpcConsumer c;
    CHECK(s.ipc.add_consumer(c, 0, nullptr, nullptr));
    (void)s.ipc.wait_for_boot_message(5);
    s.ipc.remove_consumer(c);
    for (uint64_t off : s.fw.bar.touched) {
      const bool known = off == IntelNpuIpc::kRxFifoStatus || off == IntelNpuIpc::kRxFifoAddress || off == IntelNpuIpc::kTx37xx || off == IntelNpuIpc::kTxLater || off == 0x170 ||
                         off == 0x10210 || off == 0x10220 || off == 0x10240 || off == IntelNpuMmu::kRegGerror || off == IntelNpuMmu::kRegGerrorn;
      CHECK(known);
    }
    for (uint64_t off : btrs.touched) CHECK(off < IntelNpuHw::kButtressWindow);
  }
}

int main() {
  test_jsm();
  test_ipc_basics();
  test_ipc_init_and_state();
  test_ipc_rx_robustness();
  test_boot_message();
  test_job_queue();
  test_doorbells();
  test_queue_registration();
  test_jobs_end_to_end();
  test_irq();
  test_chaos();
  if (g_failed) { printf("IntelNpuIpcTest: %d failed\n", g_failed); return 1; }
  printf("IntelNpuIpcTest: ok\n");
  return 0;
}
