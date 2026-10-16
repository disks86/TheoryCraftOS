#pragma once
#include "Allocators/IAllocator.hpp"
#include "Network/NetworkTypes.hpp"

namespace network {

// A fixed-capacity circular byte buffer (the send and receive buffers of a TCP
// connection). The storage is one allocation made by reserve().
class ByteRing {
 public:
  explicit ByteRing(allocators::IAllocator& allocator) : allocator_(allocator) {}
  ~ByteRing() { allocator_.deallocate(data_); }
  ByteRing(const ByteRing&) = delete;
  ByteRing& operator=(const ByteRing&) = delete;

  bool reserve(uint32_t capacity) {
    if (data_) return capacity == capacity_;
    data_ = static_cast<uint8_t*>(allocator_.allocate(capacity));
    if (!data_) return false;
    capacity_ = capacity;
    return true;
  }

  uint32_t capacity() const { return capacity_; }
  uint32_t size() const { return size_; }
  uint32_t free_space() const { return capacity_ - size_; }

  // Appends as many of the bytes as fit; returns how many did.
  uint32_t write(const uint8_t* bytes, uint32_t length) {
    const uint32_t n = length < free_space() ? length : free_space();
    for (uint32_t i = 0; i < n; ++i) data_[(head_ + size_ + i) % capacity_] = bytes[i];
    size_ += n;
    return n;
  }
  // Copies up to `length` bytes starting `offset` bytes from the front, leaving them in place.
  uint32_t peek(uint32_t offset, uint8_t* out, uint32_t length) const {
    if (offset >= size_) return 0;
    const uint32_t n = length < size_ - offset ? length : size_ - offset;
    for (uint32_t i = 0; i < n; ++i) out[i] = data_[(head_ + offset + i) % capacity_];
    return n;
  }
  // Drops up to `length` bytes from the front.
  void discard(uint32_t length) {
    const uint32_t n = length < size_ ? length : size_;
    head_ = capacity_ ? (head_ + n) % capacity_ : 0;
    size_ -= n;
  }
  // Removes and copies out up to `length` bytes from the front.
  uint32_t read(uint8_t* out, uint32_t length) {
    const uint32_t n = peek(0, out, length);
    discard(n);
    return n;
  }

 private:
  allocators::IAllocator& allocator_;
  uint8_t* data_ = nullptr;
  uint32_t capacity_ = 0;
  uint32_t head_ = 0;
  uint32_t size_ = 0;
};

}  // namespace network
