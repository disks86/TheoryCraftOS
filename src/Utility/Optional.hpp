#pragma once
#include <new>
#include "Utility/Move.hpp"

// Either holds a T or holds nothing. The T lives inside the Optional - no
// allocation - and is constructed and destroyed as values come and go.
// Reading an empty Optional with value() or operator* is a bug (there are no
// exceptions); use has_value(), get() or value_or() when it might be empty.
template <class T>
class Optional {
 public:
  constexpr Optional() = default;
  Optional(const T& value) { construct(value); }
  Optional(T&& value) { construct(util::move(value)); }
  Optional(const Optional& other) {
    if (other.has_) construct(*other.ptr());
  }
  Optional(Optional&& other) {
    if (other.has_) construct(util::move(*other.ptr()));
  }
  ~Optional() { reset(); }

  Optional& operator=(const T& value) {
    if (has_) *ptr() = value;
    else construct(value);
    return *this;
  }
  Optional& operator=(T&& value) {
    if (has_) *ptr() = util::move(value);
    else construct(util::move(value));
    return *this;
  }
  Optional& operator=(const Optional& other) {
    if (this == &other) return *this;
    if (!other.has_) reset();
    else *this = *other.ptr();
    return *this;
  }
  Optional& operator=(Optional&& other) {
    if (this == &other) return *this;
    if (!other.has_) reset();
    else *this = util::move(*other.ptr());
    return *this;
  }

  // Destroys any current value and constructs a new one from `args` in place.
  template <class... Args>
  T& emplace(Args&&... args) {
    reset();
    new (storage_) T(util::forward<Args>(args)...);
    has_ = true;
    return *ptr();
  }

  void reset() {
    if (has_) ptr()->~T();
    has_ = false;
  }

  bool has_value() const { return has_; }
  explicit operator bool() const { return has_; }

  // The value, or nullptr when empty.
  T* get() { return has_ ? ptr() : nullptr; }
  const T* get() const { return has_ ? ptr() : nullptr; }
  // The value; the Optional must not be empty.
  T& value() { return *ptr(); }
  const T& value() const { return *ptr(); }
  T& operator*() { return *ptr(); }
  const T& operator*() const { return *ptr(); }
  T* operator->() { return ptr(); }
  const T* operator->() const { return ptr(); }

  // The value, or a copy of `fallback` when empty.
  template <class U>
  T value_or(U&& fallback) const {
    return has_ ? *ptr() : static_cast<T>(util::forward<U>(fallback));
  }

 private:
  template <class U>
  void construct(U&& value) {
    new (storage_) T(util::forward<U>(value));
    has_ = true;
  }
  T* ptr() { return reinterpret_cast<T*>(storage_); }
  const T* ptr() const { return reinterpret_cast<const T*>(storage_); }

  alignas(T) unsigned char storage_[sizeof(T)];
  bool has_ = false;
};
