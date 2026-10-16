#pragma once
#include <stdint.h>

// Ordering policies for the sorted containers. A "less" functor answers
// `less(a, b)`: does a sort before b? Elements are equivalent when neither sorts
// before the other.
template <class T>
struct DefaultLess {
  bool operator()(const T& a, const T& b) const { return a < b; }
};

// C strings order by content (not by address).
template <>
struct DefaultLess<const char*> {
  bool operator()(const char* a, const char* b) const {
    while (*a && *a == *b) {
      ++a;
      ++b;
    }
    return static_cast<unsigned char>(*a) < static_cast<unsigned char>(*b);
  }
};
