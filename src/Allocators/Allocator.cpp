#include "Allocators/Allocator.hpp"

namespace allocators {

// Occupies exactly one block at the start of every run.
struct alignas(Allocator::kBlockSize) Allocator::Header {
  static constexpr uint64_t kFree = 0x46524545'46524545;
  static constexpr uint64_t kUsed = 0x55534544'55534544;

  uint64_t magic;
  uint64_t blocks;  // run length including this header block
  Header* next;     // next free run (free runs only)
};
namespace {
constexpr uint64_t kBlock = Allocator::kBlockSize;
}

bool Allocator::initialize() {
  static_assert(sizeof(Header) == kBlockSize, "header must fill exactly one block");
  auto start = reinterpret_cast<uint64_t>(ram_.base());
  uint64_t end = start + ram_.usable_bytes();
  start = (start + kBlock - 1) & ~(kBlock - 1);
  end &= ~(kBlock - 1);
  if (end < start + 2 * kBlock) return false;

  auto* h = reinterpret_cast<Header*>(start);
  h->magic = Header::kFree;
  h->blocks = (end - start) / kBlock;
  h->next = nullptr;
  free_list_ = h;
  return true;
}

void* Allocator::allocate(uint64_t size) {
  if (size == 0 || size > UINT64_MAX - 2 * kBlock) return nullptr;
  const uint64_t need = 1 + (size + kBlock - 1) / kBlock;  // header + payload

  Header** link = &free_list_;
  for (Header* h = free_list_; h; link = &h->next, h = h->next) {
    if (h->blocks < need) continue;
    if (h->blocks - need >= 2) {  // split: keep the tail as a free run
      auto* tail = reinterpret_cast<Header*>(reinterpret_cast<uint8_t*>(h) + need * kBlock);
      tail->magic = Header::kFree;
      tail->blocks = h->blocks - need;
      tail->next = h->next;
      *link = tail;
      h->blocks = need;
    } else {
      *link = h->next;
    }
    h->magic = Header::kUsed;
    h->next = nullptr;
    return reinterpret_cast<uint8_t*>(h) + kBlock;
  }
  return nullptr;
}

void Allocator::deallocate(void* ptr) {
  if (!ptr) return;
  auto* h = reinterpret_cast<Header*>(static_cast<uint8_t*>(ptr) - kBlock);
  if (h->magic != Header::kUsed) return;  // not ours, or a double free

  h->magic = Header::kFree;
  Header* prev = nullptr;
  Header* cur = free_list_;
  while (cur && cur < h) { prev = cur; cur = cur->next; }

  h->next = cur;
  if (prev) prev->next = h; else free_list_ = h;

  auto end_of = [](Header* x) {
    return reinterpret_cast<uint8_t*>(x) + x->blocks * kBlock;
  };
  if (cur && end_of(h) == reinterpret_cast<uint8_t*>(cur)) {  // merge forward
    h->blocks += cur->blocks;
    h->next = cur->next;
    cur->magic = 0;
  }
  if (prev && end_of(prev) == reinterpret_cast<uint8_t*>(h)) {  // merge back
    prev->blocks += h->blocks;
    prev->next = h->next;
    h->magic = 0;
  }
}

uint64_t Allocator::free_bytes() const {
  uint64_t total = 0;
  for (Header* h = free_list_; h; h = h->next) total += (h->blocks - 1) * kBlock;
  return total;
}

}  // namespace allocators
