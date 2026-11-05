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

#include "Allocators/Allocator.hpp"

namespace allocators {

namespace {

constexpr uint64_t kFrame = PageFrameAllocator::kFrameSize;
constexpr uint32_t kDepotSlots = 16;
constexpr uint64_t kSpanMagic = 0x5350414E'48454144;   // "SPANHEAD"
constexpr uint64_t kFreeCookie = 0xF4EEB10C'4A11C0DE;  // (xored with the address) marks an object as sitting on a free list
constexpr uint32_t kReleasing = ~0u;                   // Span::trim_free of a span reclaim() is giving back

// Frame tags: what the allocation at a frame is.
constexpr uint64_t kTagKind = 3;
constexpr uint64_t kTagLarge = 1;   // (frames << 2) | 1, on the first frame of a frames allocation
constexpr uint64_t kTagSpan = 2;    // span header address | 2, on every frame of a span

constexpr uint32_t kClassSize[Allocator::kClassCount] = {
    32, 48, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384, 448, 512, 640, 768, 896,
    1024, 1280, 1536, 1792, 2048, 2560, 3072, 3584, 4096, 5120, 6144, 7168, 8192, 10240, 12288, 14336, 16384};

// Sixteen-byte steps up to 128, then four classes per power of two.
uint32_t class_of(uint64_t size) {
  if (size <= 32) return 0;
  if (size <= 128) return static_cast<uint32_t>((size + 15) / 16 - 2);
  const uint32_t k = 63 - __builtin_clzll(size - 1);             // 2^k < size <= 2^(k + 1)
  const uint64_t step = uint64_t{1} << (k - 2);
  return 7 + 4 * (k - 7) + static_cast<uint32_t>((size - (uint64_t{1} << k) + step - 1) / step) - 1;
}
// The smallest class that holds `size` and whose objects are multiples of `alignment` (<= 4096; 16384 always qualifies).
uint32_t class_for(uint64_t size, uint64_t alignment) {
  uint32_t c = class_of(size);
  while (kClassSize[c] & (alignment - 1)) ++c;
  return c;
}
uint32_t span_frames(uint32_t size) { return size <= 256 ? 4 : size <= 1024 ? 16 : size <= 4096 ? 32 : 64; }
// How many free objects a core's magazine collects before it goes to the depot (32 KiB worth, 8 to 256 objects).
constexpr uint32_t magazine_limit(uint32_t size) { return 32768 / size < 8 ? 8 : 32768 / size > 256 ? 256 : 32768 / size; }
// offset / size as (offset * reciprocal) >> 40, exact for offsets below 2^24 (spans are 256 KiB at most): the error is below
// offset / 2^40, which is less than the 1 / size a quotient needs to change.
constexpr uint64_t reciprocal_of(uint32_t size) { return (uint64_t{1} << 40) / size + 1; }

// The allocator's own bookkeeping lives in memory it hands out: span headers at the end of each span, free-list links inside free
// objects, the cache tables in the frame allocator's metadata. Each of those types is plain integers and pointers (implicit-lifetime),
// the addresses are aligned for them (span ends and objects are multiples of 16, metadata of 64), and the memory is owned by the
// allocator for as long as the object is used that way.
template <class T> T* place(uint64_t address) { return reinterpret_cast<T*>(address); }
uint64_t address_of(const void* p) { return reinterpret_cast<uint64_t>(p); }

}  // namespace

// The last 64 bytes of every span.
struct alignas(64) Allocator::Span {
  uint64_t magic;
  uint64_t base;          // the first object, which is the span's first frame
  uint32_t size;          // of each object
  uint32_t capacity;      // objects in the span
  uint32_t frames;
  uint8_t cls;
  uint8_t dma;
  uint32_t trim_free;     // reclaim() only: free objects seen
  Span* trim_next;        // reclaim() only: spans to give back
  uint64_t reciprocal;    // reciprocal_of(size): finds an object's index without a division
  uint32_t limit;         // magazine_limit(size)
  uint32_t end;           // capacity * size: offsets past it are not objects
};

// A free object. A list of them is a magazine; its first object also knows the length and the last object, so that lists join in O(1).
struct Allocator::FreeObject {
  FreeObject* next;
  uint64_t count;     // (head only)
  FreeObject* tail;   // (head only)
  uint64_t cookie;    // kFreeCookie ^ address while free
};

struct Allocator::Caches {
  struct alignas(64) Core {
    FreeObject* cell[2][kClassCount];   // [dma][class]: this core's magazine
  };
  Core cores[PageFrameAllocator::kCoreSlots];
  FreeObject* depot[2][kClassCount][kDepotSlots];
};

namespace {

template <class Object>
uint64_t cookie_of(const Object* o) { return kFreeCookie ^ address_of(o); }

template <class Object>
void push(Object*& head, Object* o) {
  o->next = head;
  o->count = head ? head->count + 1 : 1;
  o->tail = head ? head->tail : o;
  o->cookie = cookie_of(o);
  head = o;
}
template <class Object>
Object* pop(Object*& head) {
  Object* o = head;
  Object* n = o->next;
  if (n) {
    n->count = o->count - 1;
    n->tail = o->tail;
  }
  head = n;
  o->cookie = 0;
  return o;
}
template <class Object>
Object* join(Object* a, Object* b) {
  if (!a) return b;
  if (!b) return a;
  a->tail->next = b;
  a->count += b->count;
  a->tail = b->tail;
  return a;
}
template <class Object>
Object* take(Object** cell) { return __atomic_exchange_n(cell, static_cast<Object*>(nullptr), __ATOMIC_ACQUIRE); }
template <class Object>
bool put(Object** cell, Object* list) {
  Object* expected = nullptr;
  return __atomic_compare_exchange_n(cell, &expected, list, false, __ATOMIC_RELEASE, __ATOMIC_RELAXED);
}

}  // namespace

void Allocator::set_core_source(uint32_t (*core)()) {
  __atomic_store_n(&core_, core, __ATOMIC_RELEASE);
  frames_.set_core_source(core);
}

bool Allocator::add_area(uint64_t base, uint64_t bytes) {
  if (caches_) return frames_.add_region(base, bytes);
  void* extra = nullptr;
  if (!frames_.add_region(base, bytes, sizeof(Caches), &extra)) return false;
  caches_ = place<Caches>(address_of(extra));   // (zeroed: every magazine and depot slot empty)
  return true;
}

bool Allocator::initialize() {
  static_assert(sizeof(Span) == 64, "a span header is one cache line");
  static_assert(sizeof(FreeObject) == 32, "a free object fits the smallest class");
  frames_.reset();
  caches_ = nullptr;
  reclaiming_ = 0;
  return add_region(ram_.base(), ram_.usable_bytes()) && caches_;
}

bool Allocator::add_region(void* base, uint64_t bytes) {
  const uint64_t start = address_of(base);
  if (!base || bytes == 0 || bytes > UINT64_MAX - start) return false;
  const uint64_t end = start + bytes;
  if (start < kLowLimit && end > kLowLimit) {   // one area per pool
    const bool low = add_area(start, kLowLimit - start);
    const bool high = add_area(kLowLimit, end - kLowLimit);
    return low || high;
  }
  return add_area(start, bytes);
}

Allocator::FreeObject** Allocator::core_cell(bool dma, uint32_t cls) {
  uint32_t (*core)() = __atomic_load_n(&core_, __ATOMIC_ACQUIRE);
  const uint32_t slot = core ? core() % PageFrameAllocator::kCoreSlots : 0;
  return &caches_->cores[slot].cell[dma][cls];
}

Allocator::FreeObject* Allocator::depot_pop(bool dma, uint32_t cls) {
  for (FreeObject*& cell : caches_->depot[dma][cls]) {
    if (!__atomic_load_n(&cell, __ATOMIC_RELAXED)) continue;
    if (FreeObject* list = take(&cell)) return list;
  }
  return nullptr;
}

void Allocator::depot_push(bool dma, uint32_t cls, FreeObject* list) {
  FreeObject* (&cells)[kDepotSlots] = caches_->depot[dma][cls];
  for (;;) {
    for (FreeObject*& cell : cells)
      if (!__atomic_load_n(&cell, __ATOMIC_RELAXED) && put(&cell, list)) return;
    // Every slot is taken: join one of the lists to ours and look for room again.
    for (FreeObject*& cell : cells) {
      if (FreeObject* other = take(&cell)) {
        list = join(list, other);
        if (put(&cell, list)) return;
        break;
      }
    }
  }
}

Allocator::FreeObject* Allocator::new_span(uint32_t cls, bool dma) {
  const uint32_t size = kClassSize[cls], frames = span_frames(size);
  const uint64_t base = frames_.allocate(frames, 1, dma);
  if (!base) return nullptr;
  Span* span = place<Span>(base + frames * kFrame - sizeof(Span));
  span->magic = kSpanMagic;
  span->base = base;
  span->size = size;
  span->capacity = static_cast<uint32_t>((frames * kFrame - sizeof(Span)) / size);
  span->frames = frames;
  span->cls = static_cast<uint8_t>(cls);
  span->dma = dma;
  span->trim_free = 0;
  span->trim_next = nullptr;
  span->reciprocal = reciprocal_of(size);
  span->limit = magazine_limit(size);
  span->end = span->capacity * size;
  for (uint32_t f = 0; f < frames; ++f) __atomic_store_n(frames_.frame_tag(base + f * kFrame), address_of(span) | kTagSpan, __ATOMIC_RELEASE);
  FreeObject* list = nullptr;
  for (uint32_t i = span->capacity; i-- > 0;) push(list, place<FreeObject>(base + uint64_t{i} * size));
  return list;
}

void* Allocator::allocate_small(uint32_t cls, bool dma) {
  if (!caches_) return nullptr;
  FreeObject** cell = core_cell(dma, cls);
  FreeObject* list = take(cell);
  if (!list) list = depot_pop(dma, cls);
  if (!list) list = new_span(cls, dma);
  if (!list) return nullptr;
  FreeObject* o = pop(list);
  if (list && !put(cell, list)) depot_push(dma, cls, list);   // (someone on this core filled the cell meanwhile)
  return o;
}

void* Allocator::allocate_frames(uint64_t size, uint64_t alignment, bool dma) {
  if (size > UINT64_MAX - kFrame) return nullptr;
  const uint64_t frames = (size + kFrame - 1) / kFrame;
  const uint64_t base = frames_.allocate(frames, alignment > kFrame ? alignment / kFrame : 1, dma);
  if (!base) return nullptr;
  __atomic_store_n(frames_.frame_tag(base), (frames << 2) | kTagLarge, __ATOMIC_RELEASE);
  return place<void>(base);
}

void* Allocator::allocate_with_reclaim(uint64_t size, uint64_t alignment, bool dma) {
  for (int attempt = 0; attempt < 2; ++attempt) {
    void* p = size <= kMaxSmall && alignment <= kFrame ? allocate_small(class_for(size, alignment), dma) : allocate_frames(size, alignment, dma);
    if (p) return p;
    if (attempt == 0) reclaim();   // empty spans may hold the memory this needs
  }
  return nullptr;
}

void* Allocator::allocate(uint64_t size) {
  if (size == 0) return nullptr;
  return allocate_with_reclaim(size, 1, false);
}

void* Allocator::allocate_aligned(uint64_t size, uint64_t alignment) {
  if (size == 0 || alignment == 0 || (alignment & (alignment - 1))) return nullptr;
  return allocate_with_reclaim(size, alignment, false);
}

void* Allocator::allocate_dma(uint64_t size, uint64_t alignment) {
  if (size == 0 || alignment == 0 || (alignment & (alignment - 1))) return nullptr;
  return allocate_with_reclaim(size, alignment, true);
}

Allocator::Span* Allocator::span_of(const void* object) const {
  const uint64_t* tag = frames_.frame_tag(address_of(object) & ~(kFrame - 1));
  if (!tag) return nullptr;
  const uint64_t t = __atomic_load_n(tag, __ATOMIC_ACQUIRE);
  return (t & kTagKind) == kTagSpan ? place<Span>(t & ~kTagKind) : nullptr;
}

void Allocator::free_small(Span* span, void* ptr) {
  const uint64_t offset = address_of(ptr) - span->base;
  if (span->magic != kSpanMagic || offset >= span->end || ((offset * span->reciprocal) >> 40) * span->size != offset) return;   // not an object we handed out
  FreeObject* o = place<FreeObject>(address_of(ptr));
  if (o->cookie == cookie_of(o)) return;   // already free (a second free of the same pointer does nothing)
  const bool dma = span->dma;
  const uint32_t cls = span->cls;
  FreeObject** cell = core_cell(dma, cls);
  FreeObject* list = take(cell);
  push(list, o);
  if (list->count >= span->limit) depot_push(dma, cls, list);
  else if (!put(cell, list)) depot_push(dma, cls, list);
}

void Allocator::deallocate(void* ptr) {
  if (!ptr) return;
  const uint64_t address = address_of(ptr);
  uint64_t* tag = frames_.frame_tag(address & ~(kFrame - 1));
  if (!tag) return;   // not ours
  uint64_t t = __atomic_load_n(tag, __ATOMIC_ACQUIRE);
  if ((t & kTagKind) == kTagLarge) {
    // Clearing the tag first makes a second free of the same pointer find nothing.
    if ((address & (kFrame - 1)) == 0 && __atomic_compare_exchange_n(tag, &t, 0, false, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED))
      frames_.free(address, t >> 2);
  } else if ((t & kTagKind) == kTagSpan) {
    free_small(place<Span>(t & ~kTagKind), ptr);
  }
}

// Takes every magazine and depot list (an exchange each, so no core is stopped), counts the free objects of each span, gives back the
// spans that are entirely free and returns the other objects to the depot. Objects in a magazine that a caller holds at that moment
// are not seen, which only keeps their spans for the next time.
void Allocator::reclaim() {
  if (!caches_ || __atomic_exchange_n(&reclaiming_, 1u, __ATOMIC_ACQUIRE)) return;
  for (int dma = 0; dma < 2; ++dma) {
    for (uint32_t cls = 0; cls < kClassCount; ++cls) {
      FreeObject* all = nullptr;
      for (Caches::Core& core : caches_->cores) all = join(all, take(&core.cell[dma][cls]));
      for (FreeObject*& cell : caches_->depot[dma][cls]) all = join(all, take(&cell));
      if (!all) continue;
      for (FreeObject* o = all; o; o = o->next)
        if (Span* s = span_of(o)) ++s->trim_free;
      FreeObject* keep = nullptr;
      Span* release = nullptr;
      for (FreeObject* o = all; o;) {
        FreeObject* next = o->next;
        if (Span* s = span_of(o)) {
          if (s->trim_free == s->capacity) {
            s->trim_free = kReleasing;
            s->trim_next = release;
            release = s;
          } else if (s->trim_free != kReleasing) {
            push(keep, o);
          }
        }
        o = next;
      }
      for (FreeObject* o = keep; o; o = o->next) span_of(o)->trim_free = 0;
      if (keep) depot_push(dma, cls, keep);
      while (release) {
        Span* s = release;
        release = s->trim_next;
        const uint64_t base = s->base;
        const uint32_t frames = s->frames;
        s->magic = 0;
        for (uint32_t f = 0; f < frames; ++f) __atomic_store_n(frames_.frame_tag(base + f * kFrame), uint64_t{0}, __ATOMIC_RELEASE);
        frames_.free(base, frames);
      }
    }
  }
  __atomic_store_n(&reclaiming_, 0u, __ATOMIC_RELEASE);
}

}  // namespace allocators
