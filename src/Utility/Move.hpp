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
