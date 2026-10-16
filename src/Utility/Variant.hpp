#pragma once
#include <new>
#include "Utility/Move.hpp"

// A value that is exactly one of the listed types (or empty). The value lives
// inside the Variant, sized for the largest alternative - no allocation. The
// alternatives must all be different types.
//
//   Variant<int, float, Name> v(3);       // holds an int
//   if (int* i = v.get<int>()) ...        // null unless it holds an int
//   v.emplace<Name>("core0");             // destroys the int, builds a Name
//   v.visit([](auto& x) { use(x); });     // calls the lambda with the held value
//
// A Variant is empty only after reset() or when default constructed; visit()
// must not be called on an empty one (there are no exceptions).
namespace variant_detail {

inline constexpr size_t kEmpty = static_cast<size_t>(-1);

// Position of T among Ts, or kEmpty if it is not one of them.
template <class T, class... Ts>
constexpr size_t index_of() {
  constexpr bool same[] = {util::kSame<T, Ts>...};
  for (size_t i = 0; i < sizeof...(Ts); ++i)
    if (same[i]) return i;
  return kEmpty;
}

template <class... Ts>
constexpr size_t max_size() {
  constexpr size_t sizes[] = {sizeof(Ts)...};
  size_t m = 0;
  for (size_t v : sizes) m = v > m ? v : m;
  return m;
}

template <class... Ts>
constexpr size_t max_align() {
  constexpr size_t aligns[] = {alignof(Ts)...};
  size_t m = 1;
  for (size_t v : aligns) m = v > m ? v : m;
  return m;
}

}  // namespace variant_detail

template <class... Ts>
class Variant {
  static_assert(sizeof...(Ts) > 0, "a Variant needs at least one alternative");

  template <size_t I, class First, class... Rest>
  struct TypeAtT {
    using type = typename TypeAtT<I - 1, Rest...>::type;
  };
  template <class First, class... Rest>
  struct TypeAtT<0, First, Rest...> {
    using type = First;
  };
  template <size_t I>
  using TypeAt = typename TypeAtT<I, Ts...>::type;

 public:
  static constexpr size_t kEmpty = variant_detail::kEmpty;
  static constexpr size_t kAlternatives = sizeof...(Ts);

  constexpr Variant() = default;
  ~Variant() { reset(); }

  // Holds a copy/move of `value`, whose type must be one of the alternatives.
  template <class U>
    requires(variant_detail::index_of<util::Decay<U>, Ts...>() != variant_detail::kEmpty)
  Variant(U&& value) {
    emplace<util::Decay<U>>(util::forward<U>(value));
  }

  Variant(const Variant& other) { copy_from<0>(other); }
  Variant(Variant&& other) { move_from<0>(other); }
  Variant& operator=(const Variant& other) {
    if (this != &other) {
      reset();
      copy_from<0>(other);
    }
    return *this;
  }
  Variant& operator=(Variant&& other) {
    if (this != &other) {
      reset();
      move_from<0>(other);
    }
    return *this;
  }
  template <class U>
    requires(variant_detail::index_of<util::Decay<U>, Ts...>() != variant_detail::kEmpty)
  Variant& operator=(U&& value) {
    emplace<util::Decay<U>>(util::forward<U>(value));
    return *this;
  }

  // Destroys the current value and constructs a T from `args` in its place.
  template <class T, class... Args>
  T& emplace(Args&&... args) {
    static_assert(variant_detail::index_of<T, Ts...>() != variant_detail::kEmpty,
                  "T is not an alternative of this Variant");
    reset();
    T* object = new (storage_) T(util::forward<Args>(args)...);
    index_ = variant_detail::index_of<T, Ts...>();
    return *object;
  }

  void reset() {
    if (index_ != kEmpty) destroy_active<0>();
    index_ = kEmpty;
  }

  // Which alternative is held (0-based, in declaration order), or kEmpty.
  size_t index() const { return index_; }
  bool empty() const { return index_ == kEmpty; }
  template <class T>
  bool holds() const { return index_ != kEmpty && index_ == variant_detail::index_of<T, Ts...>(); }

  // The held value if it is a T, else nullptr.
  template <class T>
  T* get() { return holds<T>() ? reinterpret_cast<T*>(storage_) : nullptr; }
  template <class T>
  const T* get() const { return holds<T>() ? reinterpret_cast<const T*>(storage_) : nullptr; }

  // Calls visitor(value) with the held alternative as an lvalue. The visitor
  // must accept every alternative and return the same type for all of them.
  template <class Visitor>
  decltype(auto) visit(Visitor&& visitor) { return visit_at<0>(visitor); }
  template <class Visitor>
  decltype(auto) visit(Visitor&& visitor) const { return visit_at<0>(visitor); }

 private:
  template <size_t I>
  TypeAt<I>* as() { return reinterpret_cast<TypeAt<I>*>(storage_); }
  template <size_t I>
  const TypeAt<I>* as() const { return reinterpret_cast<const TypeAt<I>*>(storage_); }

  // Each of these walks the alternatives until it reaches the held one.
  template <size_t I>
  void destroy_active() {
    if constexpr (I < sizeof...(Ts)) {
      if (index_ == I) as<I>()->~TypeAt<I>();
      else destroy_active<I + 1>();
    }
  }
  template <size_t I>
  void copy_from(const Variant& other) {
    if constexpr (I < sizeof...(Ts)) {
      if (other.index_ == I) emplace<TypeAt<I>>(*other.template as<I>());
      else copy_from<I + 1>(other);
    }
  }
  template <size_t I>
  void move_from(Variant& other) {
    if constexpr (I < sizeof...(Ts)) {
      if (other.index_ == I) emplace<TypeAt<I>>(util::move(*other.template as<I>()));
      else move_from<I + 1>(other);
    }
  }
  template <size_t I, class Visitor>
  decltype(auto) visit_at(Visitor& visitor) {
    if constexpr (I + 1 == sizeof...(Ts)) {
      return visitor(*as<I>());
    } else {
      if (index_ == I) return visitor(*as<I>());
      return visit_at<I + 1>(visitor);
    }
  }
  template <size_t I, class Visitor>
  decltype(auto) visit_at(Visitor& visitor) const {
    if constexpr (I + 1 == sizeof...(Ts)) {
      return visitor(*as<I>());
    } else {
      if (index_ == I) return visitor(*as<I>());
      return visit_at<I + 1>(visitor);
    }
  }

  alignas(variant_detail::max_align<Ts...>()) unsigned char storage_[variant_detail::max_size<Ts...>()];
  size_t index_ = kEmpty;
};
