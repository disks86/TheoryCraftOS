// The few <utility>/<type_traits> pieces the utility types need. The image has
// no C++ standard library, so they are spelled out here.
#pragma once
#include <stddef.h>

namespace util {

template <class T> struct RemoveReferenceT { using type = T; };
template <class T> struct RemoveReferenceT<T&> { using type = T; };
template <class T> struct RemoveReferenceT<T&&> { using type = T; };
template <class T> using RemoveReference = typename RemoveReferenceT<T>::type;

template <class T> struct RemoveConstT { using type = T; };
template <class T> struct RemoveConstT<const T> { using type = T; };

// T without references and top-level const/volatile: what a value of "type T" is.
template <class T> struct DecayT { using type = typename RemoveConstT<RemoveReference<T>>::type; };
template <class T> using Decay = typename DecayT<T>::type;

template <class A, class B> inline constexpr bool kSame = false;
template <class A> inline constexpr bool kSame<A, A> = true;

template <class T>
constexpr RemoveReference<T>&& move(T&& value) {
  return static_cast<RemoveReference<T>&&>(value);
}

template <class T>
constexpr T&& forward(RemoveReference<T>& value) {
  return static_cast<T&&>(value);
}

template <class T>
constexpr T&& forward(RemoveReference<T>&& value) {
  return static_cast<T&&>(value);
}

}  // namespace util
