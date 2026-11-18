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

#include "Hardware/GraphicsProcessingUnit/NVIDIA/Nvkm/NvFifo.hpp"

namespace hardware {
namespace nv {

namespace {

// ---- Registers (BAR0) and layouts, from memory of nouveau's fifo/gk104.c, gk110.c, gv100.c, tu102.c, top/gk104.c. ----
//   0x000200            PMC_ENABLE: bit 8 PFIFO; Kepler CE0 bit 6, CE1 bit 7 (else the PTOP reset bit)        confident (bit 8), fairly sure (CE)
//   0x000204            PBDMA enable mask (write all ones, read back which exist)                             confident
//   0x002100 / 0x002140 PFIFO interrupt status (write 1s to ack) / enable (left 0: completion is polled)       confident
//   0x002254            Kepler..Pascal: 0x10000000 | BAR1 offset of the USERD block >> 12                      confident
//   0x002270 / 0x002274 runlist submit: (target << 28) | address >> 12, then runlist << 20 | entries          confident
//   0x002284 + rl * 8   bit 20 set while the runlist update is pending                                         confident
//   0x002b00 + rl*0x10  Turing: runlist address low / +4 high / +8 entries; +0xc bit 15 pending               fairly sure
//   0x002634            preempt: channel id (Kepler..Pascal) or 0x01000000 | TSG id (Volta+); bit 20 pending   confident
//   0x022700 + i * 4    PTOP, 64 entries (decoded in read_topology)                                            fairly sure
//   0x800000 + id * 8   channel instance: 0x80000000 | instance >> 12 (target bits 29:28, VRAM 0)             confident
//   0x800004 + id * 8   channel control: write bit 10 to enable, bit 11 to disable                            confident
//   0x810090            Volta+: usermode doorbell, written with the channel's token (its id before Ampere)    confident (Turing), fairly sure (Volta)
//   USERD + 0x88 / 0x8c GP_GET / GP_PUT                                                                       confident
//   GPFIFO entry        word 0: address bits 31:2; word 1: address bits 39:32 | length in words << 10         confident
//   runlist entry gk104 8 bytes: channel id, 0                                                                confident
//   runlist gv100       TSG: (128 << 24) | (3 << 16) | 1, channel count, TSG id, 0;
//                       channel: USERD low | runqueue << 1, USERD high, id | instance low, instance high     fairly sure
//   RAMFC               the words in write_ramfc                                                              fairly sure (gk104), less sure (gv100)
constexpr uint32_t kPmcEnableReg = 0x000200, kPbdmaEnable = 0x000204;
constexpr uint32_t kFifoIntr = 0x002100, kFifoIntrEn = 0x002140, kUserdBar = 0x002254;
constexpr uint32_t kRunlistBase = 0x002270, kRunlistSubmit = 0x002274, kRunlistPending = 0x002284;
constexpr uint32_t kTuRunlist = 0x002b00;
constexpr uint32_t kPreempt = 0x002634;
constexpr uint32_t kTop = 0x022700;
constexpr uint32_t kChanInst = 0x800000, kChanCtrl = 0x800004;
constexpr uint32_t kDoorbell = 0x810090;

}  // namespace

const char* fifo_status_name(FifoStatus s) {
  switch (s) {
    case FifoStatus::Ok: return "ok";
    case FifoStatus::Unsupported: return "unsupported";
    case FifoStatus::NeedsGsp: return "needs GSP firmware";
    case FifoStatus::NoMemory: return "no memory";
    case FifoStatus::NoEngine: return "no engine";
    case FifoStatus::NotReady: return "not ready";
    case FifoStatus::Full: return "full";
    case FifoStatus::Busy: return "busy";
    case FifoStatus::Timeout: return "timeout";
    case FifoStatus::BadArgument: return "bad argument";
  }
  return "?";
}

bool read_topology(INvRegisters& regs, Topology* out) {
  *out = Topology{};
  TopEngine info;
  bool any = false;
  for (uint32_t i = 0; i < 64; ++i) {
    const uint32_t data = regs.read32(kTop + i * 4);
    if (data == 0xFFFFFFFFu) return false;
    switch (data & 3) {
      case 0: continue;  // not valid
      case 1:  // DATA
        info.instance = static_cast<uint8_t>((data & 0x3C000000) >> 26);
        info.addr = data & 0x00FFF000;
        if (data & 4) info.fault = static_cast<int8_t>((data & 0x000003F8) >> 3);
        break;
      case 2:  // ENUM
        if (data & 0x20) info.instance = static_cast<uint8_t>((data & 0x3C000000) >> 26);
        if (data & 0x10) info.runlist = static_cast<int8_t>((data & 0x01E00000) >> 21);
        if (data & 0x08) info.intr = static_cast<int8_t>((data & 0x000F8000) >> 15);
        if (data & 0x04) info.reset = static_cast<int8_t>((data & 0x00003E00) >> 9);
        break;
      case 3:  // ENGINE_TYPE
        info.type = (data & 0x7FFFFFFC) >> 2;
        any = true;
        break;
    }
    if (data & 0x80000000u) continue;  // chained: the next entry describes the same engine
    if (any) {
      if (out->count < Topology::kMaxEngines) ArrayView<TopEngine>(out->engines)[out->count++] = info;
      else out->truncated = true;
    }
    info = TopEngine{};
    any = false;
  }
  return true;
}

Optional<TopEngine> find_copy_engine(const Topology& t, uint32_t index) {
  for (const TopEngine& e : ArrayView<const TopEngine>(t.engines).first(t.count)) {
    const bool ce = (e.type >= kTopTypeCe0 && e.type <= kTopTypeCe2) || e.type == kTopTypeLce;
    if (!ce || e.runlist < 0) continue;
    if (index-- == 0) {
      TopEngine copy = e;
      if (e.type != kTopTypeLce) copy.instance = static_cast<uint8_t>(e.type - kTopTypeCe0);
      return copy;
    }
  }
  return {};
}

void PushBuilder::method(uint32_t subc, uint32_t mthd, ArrayView<const uint32_t> data) {
  if (data.size() == 0 || data.size() > 0x1FFF || subc > 7 || mthd > 0x7FFC || mthd % 4 || data.size() + 1 > kMaxWords - count_) {
    overflow_ = true;
    return;
  }
  const ArrayView<uint32_t> out(words_);
  out[count_++] = method_incr(subc, mthd, static_cast<uint32_t>(data.size()));
  for (const uint32_t v : data) out[count_++] = v;
}

// ---- NvFifo ----

NvFifo::NvFifo(INvRegisters& regs, const Chip& chip, INvMemory& memory, NvBar* bar, INvAperture* bar1, INvLock& lock, NvTimeouts timeouts)
    : regs_(regs), chip_(chip), memory_(memory), bar_(bar), bar1_(bar1), lock_(lock), timeouts_(timeouts) {}

NvFifo::~NvFifo() {
  for (NvChannel& ch : channels_)
    if (ch.active_) (void)destroy_channel(ch);
  for (RunlistState& r : runlists_)
    for (GpuMemory& m : r.buffer) memory_.release(m);
  if (userd_bar1_va_ && bar_) {
    (void)bar_->bar1().unmap(userd_bar1_va_, userd_.bytes);
    (void)bar_->bar1().free_va(userd_bar1_va_);
  }
  memory_.release(userd_);
}

bool NvFifo::poll_clear(uint32_t offset, uint32_t mask, uint32_t timeout_us) {
  for (uint32_t i = 0; i <= timeout_us; ++i) {
    if (!(regs_.read32(offset) & mask)) return true;
    regs_.delay_us(1);
  }
  return false;
}

FifoStatus NvFifo::init() {
  if (chip_.needs_gsp || chip_.family >= Family::Ampere) return FifoStatus::NeedsGsp;
  if (chip_.family < Family::Kepler || chip_.family > Family::Turing) return FifoStatus::Unsupported;
  NvLockGuard guard(lock_);
  if (ready_) return FifoStatus::Ok;
  if (!read_topology(regs_, &topology_) || !find_copy_engine(topology_, 0)) return FifoStatus::NoEngine;

  uint32_t enable = 0x00000100;  // PFIFO
  for (uint32_t i = 0;; ++i) {
    const Optional<TopEngine> ce = find_copy_engine(topology_, i);
    if (!ce) break;
    if (ce->reset >= 0) enable |= 1u << ce->reset;
    else if (chip_.family == Family::Kepler && ce->instance < 2) enable |= 1u << (6 + ce->instance);
  }
  regs_.mask(kPmcEnableReg, enable, enable);
  regs_.write32(kPbdmaEnable, 0xFFFFFFFFu);
  const uint32_t pbdmas = regs_.read32(kPbdmaEnable);
  if (pbdmas == 0 || pbdmas == 0xFFFFFFFFu) return FifoStatus::NoEngine;
  regs_.write32(kPbdmaEnable, pbdmas);
  regs_.write32(kFifoIntr, 0xFFFFFFFFu);
  regs_.write32(kFifoIntrEn, 0);

  userd_ = memory_.allocate(MemoryTarget::Vram, kMaxChannels * kUserdStride, 0x1000);
  if (!userd_.valid()) return FifoStatus::NoMemory;
  if (!uses_doorbell()) {
    if (!bar_ || !bar1_ || !bar_->ready()) return FifoStatus::NotReady;
    const Optional<uint64_t> va = bar_->bar1().allocate_va(userd_.bytes, 0x1000);
    if (!va) return FifoStatus::NoMemory;
    userd_bar1_va_ = *va;
    if (!bar_->bar1().map(*va, userd_, 0, userd_.bytes) || !bar_->bar1().flush()) return FifoStatus::Timeout;
    regs_.write32(kUserdBar, 0x10000000u | static_cast<uint32_t>(*va >> 12));
  }
  ready_ = true;
  return FifoStatus::Ok;
}

FifoStatus NvFifo::write_ramfc(NvChannel& ch) {
  const uint64_t userd = ch.userd_address();
  const uint32_t limit2 = 9;  // log2(kGpfifoEntries)
  struct Word { uint32_t offset, value; };
  const uint32_t ulo = static_cast<uint32_t>(userd), uhi = static_cast<uint32_t>(userd >> 32);
  const uint32_t glo = static_cast<uint32_t>(ch.gpfifo_va_), ghi = static_cast<uint32_t>(ch.gpfifo_va_ >> 32) | limit2 << 16;
  const Word kepler[] = {
      {0x08, ulo}, {0x0c, uhi}, {0x10, 0x0000face}, {0x30, 0xfffff902}, {0x48, glo}, {0x4c, ghi}, {0x84, 0x20400000},
      {0x94, 0x30000001}, {0x9c, 0x00000100}, {0xac, 0x0000001f}, {0xe8, ch.id_}, {0xb8, 0xf8000000}, {0xf8, 0x10003080},
      {0xfc, 0x10000010},
  };
  const Word volta[] = {
      {0x008, ulo}, {0x00c, uhi}, {0x010, 0x0000face}, {0x030, 0x7ffff902}, {0x048, glo}, {0x04c, ghi}, {0x084, 0x20400000},
      {0x094, 0x30000001}, {0x0e4, 0x00000000}, {0x0e8, ch.id_}, {0x0f4, 0x00001000}, {0x0f8, 0x10003080},
  };
  const ArrayView<const Word> words = uses_doorbell() ? ArrayView<const Word>(volta) : ArrayView<const Word>(kepler);
  for (const Word& w : words)
    if (!memory_.write32(ch.inst_, w.offset, w.value)) return FifoStatus::NoMemory;
  return FifoStatus::Ok;
}

FifoStatus NvFifo::update_runlist(uint32_t runlist) {
  RunlistState& state = ArrayView<RunlistState>(runlists_)[runlist];
  for (GpuMemory& m : state.buffer) {
    if (!m.valid()) m = memory_.allocate(MemoryTarget::Vram, 0x1000, 0x1000);
    if (!m.valid()) return FifoStatus::NoMemory;
  }
  const GpuMemory& buffer = ArrayView<GpuMemory>(state.buffer)[state.next];
  state.next ^= 1;
  uint32_t count = 0;
  uint64_t offset = 0;
  for (const NvChannel& ch : channels_) {
    if (!ch.active_ || ch.runlist_ != runlist) continue;
    bool ok = true;
    if (uses_tsg()) {
      const uint64_t userd = ch.userd_address(), inst = ch.inst_.gpu_address;
      const uint32_t entries[8] = {
          (128u << 24) | (3u << 16) | 1, 1, ch.id_, 0,                                       // the TSG (id = channel id), one channel
          static_cast<uint32_t>(userd), static_cast<uint32_t>(userd >> 32), ch.id_ | static_cast<uint32_t>(inst), static_cast<uint32_t>(inst >> 32),
      };
      for (uint32_t i = 0; i < 8; ++i) ok = ok && memory_.write32(buffer, offset + i * 4, entries[i]);
      offset += 32;
      count += 2;
    } else {
      ok = memory_.write32(buffer, offset, ch.id_) && memory_.write32(buffer, offset + 4, 0);
      offset += 8;
      count += 1;
    }
    if (!ok) return FifoStatus::NoMemory;
  }
  memory_.flush(buffer);
  if (chip_.family == Family::Turing) {
    regs_.write32(kTuRunlist + runlist * 0x10, static_cast<uint32_t>(buffer.gpu_address));
    regs_.write32(kTuRunlist + runlist * 0x10 + 4, static_cast<uint32_t>(buffer.gpu_address >> 32));
    regs_.write32(kTuRunlist + runlist * 0x10 + 8, count);
    return poll_clear(kTuRunlist + runlist * 0x10 + 0xc, 0x00008000, timeouts_.register_us) ? FifoStatus::Ok : FifoStatus::Timeout;
  }
  regs_.write32(kRunlistBase, (0u << 28) | static_cast<uint32_t>(buffer.gpu_address >> 12));
  regs_.write32(kRunlistSubmit, runlist << 20 | count);
  return poll_clear(kRunlistPending + runlist * 8, 0x00100000, timeouts_.register_us) ? FifoStatus::Ok : FifoStatus::Timeout;
}

FifoStatus NvFifo::preempt(NvChannel& ch) {
  regs_.write32(kPreempt, uses_tsg() ? (0x01000000u | ch.id_) : ch.id_);
  return poll_clear(kPreempt, 0x00100000, timeouts_.register_us) ? FifoStatus::Ok : FifoStatus::Timeout;
}

FifoStatus NvFifo::create_channel(NvVmm& vmm, uint32_t runlist, NvChannel** out) {
  *out = nullptr;
  if (runlist >= kMaxRunlists || !vmm.ready()) return FifoStatus::BadArgument;
  NvLockGuard guard(lock_);
  if (!ready_) return FifoStatus::NotReady;
  NvChannel* slot = nullptr;
  for (uint32_t i = 0; i < kMaxChannels; ++i) {
    NvChannel& c = ArrayView<NvChannel>(channels_)[i];
    if (!c.active_ && !c.inst_.valid()) { slot = &c; c.id_ = i; break; }
  }
  if (!slot) return FifoStatus::Full;
  NvChannel& ch = *slot;
  ch.fifo_ = this;
  ch.vmm_ = &vmm;
  ch.runlist_ = runlist;
  ch.gp_put_ = 0;
  ch.push_put_ = 0;
  auto fail = [&](FifoStatus status) {
    if (ch.gpfifo_va_) { (void)vmm.unmap(ch.gpfifo_va_, ch.gpfifo_.bytes); (void)vmm.free_va(ch.gpfifo_va_); }
    if (ch.push_va_) { (void)vmm.unmap(ch.push_va_, ch.push_.bytes); (void)vmm.free_va(ch.push_va_); }
    memory_.release(ch.inst_);
    memory_.release(ch.gpfifo_);
    memory_.release(ch.push_);
    ch.gpfifo_va_ = ch.push_va_ = 0;
    ch.vmm_ = nullptr;
    return status;
  };
  ch.inst_ = memory_.allocate(MemoryTarget::Vram, 0x1000, 0x1000);
  ch.gpfifo_ = memory_.allocate(MemoryTarget::Vram, kGpfifoEntries * 8, 0x1000);
  ch.push_ = memory_.allocate(MemoryTarget::Vram, kPushBytes, 0x1000);
  if (!ch.inst_.valid() || !ch.gpfifo_.valid() || !ch.push_.valid()) return fail(FifoStatus::NoMemory);
  const Optional<uint64_t> gva = vmm.allocate_va(ch.gpfifo_.bytes, 0x1000);
  if (gva) ch.gpfifo_va_ = *gva;
  const Optional<uint64_t> pva = vmm.allocate_va(ch.push_.bytes, 0x1000);
  if (pva) ch.push_va_ = *pva;
  if (!gva || !pva) return fail(FifoStatus::NoMemory);
  if (!vmm.map(ch.gpfifo_va_, ch.gpfifo_, 0, ch.gpfifo_.bytes) || !vmm.map(ch.push_va_, ch.push_, 0, ch.push_.bytes))
    return fail(FifoStatus::NoMemory);
  if (!vmm.flush()) return fail(FifoStatus::Timeout);
  for (uint32_t off = 0; off < kUserdStride; off += 4)
    if (!memory_.write32(userd_, ch.id_ * kUserdStride + off, 0)) return fail(FifoStatus::NoMemory);
  if (!vmm.join(ch.inst_)) return fail(FifoStatus::NoMemory);
  if (const FifoStatus s = write_ramfc(ch); s != FifoStatus::Ok) return fail(s);
  memory_.flush(ch.inst_);
  memory_.flush(userd_);

  regs_.write32(kChanInst + ch.id_ * 8, 0x80000000u | static_cast<uint32_t>(ch.inst_.gpu_address >> 12));
  ch.active_ = true;
  if (const FifoStatus s = update_runlist(runlist); s != FifoStatus::Ok) {
    ch.active_ = false;
    regs_.write32(kChanInst + ch.id_ * 8, 0);
    return fail(s);
  }
  regs_.mask(kChanCtrl + ch.id_ * 8, 0x00000400, 0x00000400);
  *out = &ch;
  return FifoStatus::Ok;
}

FifoStatus NvFifo::destroy_channel(NvChannel& ch) {
  NvLockGuard guard(lock_);
  if (!ch.active_ || ch.fifo_ != this) return FifoStatus::BadArgument;
  // Claim the channel as a submitter would, so no submit can start while it is torn down.
  if (__atomic_exchange_n(&ch.busy_, true, __ATOMIC_ACQ_REL)) return FifoStatus::Busy;
  struct Release {
    bool* flag;
    ~Release() { __atomic_store_n(flag, false, __ATOMIC_RELEASE); }
  } release{&ch.busy_};
  regs_.mask(kChanCtrl + ch.id_ * 8, 0x00000800, 0x00000800);
  const FifoStatus preempted = preempt(ch);
  ch.active_ = false;
  const FifoStatus removed = update_runlist(ch.runlist_);
  regs_.write32(kChanInst + ch.id_ * 8, 0);
  if (preempted != FifoStatus::Ok || removed != FifoStatus::Ok) {
    // The engine may still be reading the channel's memory: keep it (the slot stays taken) rather than hand it out again.
    return FifoStatus::Timeout;
  }
  NvVmm& vmm = *ch.vmm_;
  (void)vmm.unmap(ch.gpfifo_va_, ch.gpfifo_.bytes);
  (void)vmm.unmap(ch.push_va_, ch.push_.bytes);
  (void)vmm.free_va(ch.gpfifo_va_);
  (void)vmm.free_va(ch.push_va_);
  (void)vmm.flush();
  memory_.release(ch.inst_);
  memory_.release(ch.gpfifo_);
  memory_.release(ch.push_);
  ch.gpfifo_va_ = ch.push_va_ = 0;
  ch.vmm_ = nullptr;
  return FifoStatus::Ok;
}

bool NvFifo::write_gp_put(NvChannel& ch) {
  const uint64_t off = uint64_t(ch.id_) * kUserdStride + kUserdGpPut;
  if (!uses_doorbell()) {
    bar1_->write32(userd_bar1_va_ + off, ch.gp_put_);
    return true;
  }
  if (!memory_.write32(userd_, off, ch.gp_put_)) return false;
  memory_.flush(userd_);
  regs_.write32(kDoorbell, ch.id_);
  return true;
}

uint32_t NvFifo::read_gp_get(NvChannel& ch) {
  const uint64_t off = uint64_t(ch.id_) * kUserdStride + kUserdGpGet;
  if (!uses_doorbell()) return bar1_->read32(userd_bar1_va_ + off);
  uint32_t v = 0xFFFFFFFFu;
  if (!memory_.read32(userd_, off, &v)) return 0xFFFFFFFFu;
  return v;
}

// ---- NvChannel ----

uint64_t NvChannel::userd_address() const { return fifo_ ? fifo_->userd_.gpu_address + uint64_t(id_) * NvFifo::kUserdStride : 0; }

void NvChannel::pause_us(uint32_t us) {
  if (fifo_) fifo_->regs_.delay_us(us);
}

uint32_t NvChannel::gp_get() { return fifo_ ? fifo_->read_gp_get(*this) : 0xFFFFFFFFu; }

FifoStatus NvChannel::wait_idle() {
  if (!active_) return FifoStatus::NotReady;
  for (uint32_t i = 0; i <= fifo_->timeouts_.fence_us; ++i) {
    if (gp_get() == gp_put_) return FifoStatus::Ok;
    fifo_->regs_.delay_us(1);
  }
  return FifoStatus::Timeout;
}

FifoStatus NvChannel::submit(ArrayView<const uint32_t> words) {
  if (__atomic_exchange_n(&busy_, true, __ATOMIC_ACQ_REL)) return FifoStatus::Busy;
  const FifoStatus s = submit_locked(words);
  __atomic_store_n(&busy_, false, __ATOMIC_RELEASE);
  return s;
}

FifoStatus NvChannel::submit_locked(ArrayView<const uint32_t> words) {
  if (!active_) return FifoStatus::NotReady;
  const uint64_t bytes = words.size() * 4;
  if (bytes == 0 || bytes > NvFifo::kPushBytes || words.size() > 0x1FFFFF) return FifoStatus::BadArgument;
  INvMemory& mem = fifo_->memory_;
  const uint32_t entries = NvFifo::kGpfifoEntries;
  // A free GPFIFO slot (one is always left empty so GP_PUT == GP_GET means idle) and room in the push buffer; the push buffer only
  // wraps once the host has fetched everything, so what is in flight always lies before push_put_.
  bool wait = push_put_ + bytes > NvFifo::kPushBytes;
  if (!wait) {
    const uint32_t get = gp_get();
    if (get >= entries) return FifoStatus::Timeout;   // USERD unreadable: the GPU fell off the bus
    wait = (gp_put_ + 1) % entries == get;
  }
  if (wait) {
    if (const FifoStatus s = wait_idle(); s != FifoStatus::Ok) return s;
    if (push_put_ + bytes > NvFifo::kPushBytes) push_put_ = 0;
  }
  for (uint64_t i = 0; i < words.size(); ++i)
    if (!mem.write32(push_, push_put_ + i * 4, words[i])) return FifoStatus::NoMemory;
  mem.flush(push_);
  const uint64_t va = push_va_ + push_put_;
  const uint32_t lo = static_cast<uint32_t>(va) & ~3u;
  const uint32_t hi = (static_cast<uint32_t>(va >> 32) & 0xFF) | static_cast<uint32_t>(words.size()) << 10;
  if (!mem.write32(gpfifo_, gp_put_ * 8ull, lo) || !mem.write32(gpfifo_, gp_put_ * 8ull + 4, hi)) return FifoStatus::NoMemory;
  mem.flush(gpfifo_);
  push_put_ += static_cast<uint32_t>(bytes);
  gp_put_ = (gp_put_ + 1) % entries;
  return fifo_->write_gp_put(*this) ? FifoStatus::Ok : FifoStatus::NoMemory;
}

}  // namespace nv
}  // namespace hardware
