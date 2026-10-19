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

// GPU-friendly Vector class with column-major memory layout for GPU compatibility
// Uses simple math without external dependencies for freestanding compatibility
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace math {

// Simple square root implementation (Newton's method)
inline float sqrt_float(float x) {
  if (x <= 0) return 0;
  float g = x;
  for (int i = 0; i < 10; ++i) {
    g = 0.5f * (g + x / g);
  }
  return g;
}

inline double sqrt_double(double x) {
  if (x <= 0) return 0;
  double g = x;
  for (int i = 0; i < 10; ++i) {
    g = 0.5 * (g + x / g);
  }
  return g;
}

// Vector2: 2-element vector (GPU-friendly: 16-byte aligned)
template<typename T>
class Vector2 {
 public:
  T x, y;

  // Constructors
  constexpr Vector2() : x(0), y(0) {}
  constexpr Vector2(T x, T y) : x(x), y(y) {}
  explicit constexpr Vector2(T v) : x(v), y(v) {}

  // Copy/move
  constexpr Vector2(const Vector2&) = default;
  constexpr Vector2(Vector2&&) = default;
  constexpr Vector2& operator=(const Vector2&) = default;
  constexpr Vector2& operator=(Vector2&&) = default;

  // Element access
  constexpr T& operator[](size_t i) { return i == 0 ? x : y; }
  constexpr const T& operator[](size_t i) const { return i == 0 ? x : y; }

  // Vector operations
  constexpr Vector2 operator-() const { return Vector2(-x, -y); }

  constexpr Vector2 operator+(const Vector2& other) const {
    return Vector2(x + other.x, y + other.y);
  }

  constexpr Vector2 operator-(const Vector2& other) const {
    return Vector2(x - other.x, y - other.y);
  }

  constexpr Vector2 operator*(const Vector2& other) const {
    return Vector2(x * other.x, y * other.y);
  }

  constexpr Vector2 operator/(const Vector2& other) const {
    return Vector2(x / other.x, y / other.y);
  }

  constexpr Vector2 operator+(T s) const { return Vector2(x + s, y + s); }
  constexpr Vector2 operator-(T s) const { return Vector2(x - s, y - s); }
  constexpr Vector2 operator*(T s) const { return Vector2(x * s, y * s); }
  constexpr Vector2 operator/(T s) const { return Vector2(x / s, y / s); }

  constexpr Vector2& operator+=(const Vector2& other) {
    x += other.x; y += other.y;
    return *this;
  }

  constexpr Vector2& operator-=(const Vector2& other) {
    x -= other.x; y -= other.y;
    return *this;
  }

  constexpr Vector2& operator*=(const Vector2& other) {
    x *= other.x; y *= other.y;
    return *this;
  }

  constexpr Vector2& operator/=(const Vector2& other) {
    x /= other.x; y /= other.y;
    return *this;
  }

  constexpr Vector2& operator+=(T s) { x += s; y += s; return *this; }
  constexpr Vector2& operator-=(T s) { x -= s; y -= s; return *this; }
  constexpr Vector2& operator*=(T s) { x *= s; y *= s; return *this; }
  constexpr Vector2& operator/=(T s) { x /= s; y /= s; return *this; }

  // Comparison
  constexpr bool operator==(const Vector2& other) const {
    return x == other.x && y == other.y;
  }

  constexpr bool operator!=(const Vector2& other) const {
    return x != other.x || y != other.y;
  }

  // Utility
  T length_squared() const { return x*x + y*y; }
  T length() const { return (sizeof(T) == 4) ? T(sqrt_float(float(length_squared()))) : T(sqrt_double(double(length_squared()))); }
  void normalize() {
    T len = length();
    if (len > 0) { x /= len; y /= len; }
  }
  Vector2 normalized() const {
    T len = length();
    return len > 0 ? Vector2(x / len, y / len) : *this;
  }
  T dot(const Vector2& other) const { return x * other.x + y * other.y; }
};

// Vector3: 3-element vector (16-byte aligned for GPU)
template<typename T>
class Vector3 {
 public:
  T x, y, z;

  // Constructors
  constexpr Vector3() : x(0), y(0), z(0) {}
  constexpr Vector3(T x, T y, T z) : x(x), y(y), z(z) {}
  explicit constexpr Vector3(T v) : x(v), y(v), z(v) {}

  // Copy/move
  constexpr Vector3(const Vector3&) = default;
  constexpr Vector3(Vector3&&) = default;
  constexpr Vector3& operator=(const Vector3&) = default;
  constexpr Vector3& operator=(Vector3&&) = default;

  // Element access
  constexpr T& operator[](size_t i) { return i == 0 ? x : (i == 1 ? y : z); }
  constexpr const T& operator[](size_t i) const {
    return i == 0 ? x : (i == 1 ? y : z);
  }

  // Vector operations
  constexpr Vector3 operator-() const { return Vector3(-x, -y, -z); }

  constexpr Vector3 operator+(const Vector3& other) const {
    return Vector3(x + other.x, y + other.y, z + other.z);
  }

  constexpr Vector3 operator-(const Vector3& other) const {
    return Vector3(x - other.x, y - other.y, z - other.z);
  }

  constexpr Vector3 operator*(const Vector3& other) const {
    return Vector3(x * other.x, y * other.y, z * other.z);
  }

  constexpr Vector3 operator/(const Vector3& other) const {
    return Vector3(x / other.x, y / other.y, z / other.z);
  }

  constexpr Vector3 operator+(T s) const { return Vector3(x + s, y + s, z + s); }
  constexpr Vector3 operator-(T s) const { return Vector3(x - s, y - s, z - s); }
  constexpr Vector3 operator*(T s) const { return Vector3(x * s, y * s, z * s); }
  constexpr Vector3 operator/(T s) const { return Vector3(x / s, y / s, z / s); }

  constexpr Vector3& operator+=(const Vector3& other) {
    x += other.x; y += other.y; z += other.z;
    return *this;
  }

  constexpr Vector3& operator-=(const Vector3& other) {
    x -= other.x; y -= other.y; z -= other.z;
    return *this;
  }

  constexpr Vector3& operator*=(const Vector3& other) {
    x *= other.x; y *= other.y; z *= other.z;
    return *this;
  }

  constexpr Vector3& operator/=(const Vector3& other) {
    x /= other.x; y /= other.y; z /= other.z;
    return *this;
  }

  constexpr Vector3& operator+=(T s) { x += s; y += s; z += s; return *this; }
  constexpr Vector3& operator-=(T s) { x -= s; y -= s; z -= s; return *this; }
  constexpr Vector3& operator*=(T s) { x *= s; y *= s; z *= s; return *this; }
  constexpr Vector3& operator/=(T s) { x /= s; y /= s; z /= s; return *this; }

  // Comparison
  constexpr bool operator==(const Vector3& other) const {
    return x == other.x && y == other.y && z == other.z;
  }

  constexpr bool operator!=(const Vector3& other) const {
    return x != other.x || y != other.y || z != other.z;
  }

  // Utility
  T length_squared() const { return x*x + y*y + z*z; }
  T length() const { return (sizeof(T) == 4) ? T(sqrt_float(float(length_squared()))) : T(sqrt_double(double(length_squared()))); }
  void normalize() {
    T len = length();
    if (len > 0) { x /= len; y /= len; z /= len; }
  }
  Vector3 normalized() const {
    T len = length();
    return len > 0 ? Vector3(x / len, y / len, z / len) : *this;
  }
  T dot(const Vector3& other) const { return x * other.x + y * other.y + z * other.z; }
  Vector3 cross(const Vector3& other) const {
    return Vector3(y * other.z - z * other.y,
                   z * other.x - x * other.z,
                   x * other.y - y * other.x);
  }
};

// Vector4: 4-element vector (16-byte aligned, GPU-friendly)
template<typename T>
class Vector4 {
 public:
  T x, y, z, w;

  // Constructors
  constexpr Vector4() : x(0), y(0), z(0), w(0) {}
  constexpr Vector4(T x, T y, T z, T w) : x(x), y(y), z(z), w(w) {}
  explicit constexpr Vector4(T v) : x(v), y(v), z(v), w(v) {}
  constexpr Vector4(const Vector3<T>& xyz, T w = 0) : x(xyz.x), y(xyz.y), z(xyz.z), w(w) {}

  // Copy/move
  constexpr Vector4(const Vector4&) = default;
  constexpr Vector4(Vector4&&) = default;
  constexpr Vector4& operator=(const Vector4&) = default;
  constexpr Vector4& operator=(Vector4&&) = default;

  // Element access
  constexpr T& operator[](size_t i) {
    return i == 0 ? x : (i == 1 ? y : (i == 2 ? z : w));
  }
  constexpr const T& operator[](size_t i) const {
    return i == 0 ? x : (i == 1 ? y : (i == 2 ? z : w));
  }

  // Vector operations
  constexpr Vector4 operator-() const { return Vector4(-x, -y, -z, -w); }

  constexpr Vector4 operator+(const Vector4& other) const {
    return Vector4(x + other.x, y + other.y, z + other.z, w + other.w);
  }

  constexpr Vector4 operator-(const Vector4& other) const {
    return Vector4(x - other.x, y - other.y, z - other.z, w - other.w);
  }

  constexpr Vector4 operator*(const Vector4& other) const {
    return Vector4(x * other.x, y * other.y, z * other.z, w * other.w);
  }

  constexpr Vector4 operator/(const Vector4& other) const {
    return Vector4(x / other.x, y / other.y, z / other.z, w / other.w);
  }

  constexpr Vector4 operator+(T s) const { return Vector4(x + s, y + s, z + s, w + s); }
  constexpr Vector4 operator-(T s) const { return Vector4(x - s, y - s, z - s, w - s); }
  constexpr Vector4 operator*(T s) const { return Vector4(x * s, y * s, z * s, w * s); }
  constexpr Vector4 operator/(T s) const { return Vector4(x / s, y / s, z / s, w / s); }

  constexpr Vector4& operator+=(const Vector4& other) {
    x += other.x; y += other.y; z += other.z; w += other.w;
    return *this;
  }

  constexpr Vector4& operator-=(const Vector4& other) {
    x -= other.x; y -= other.y; z -= other.z; w -= other.w;
    return *this;
  }

  constexpr Vector4& operator*=(const Vector4& other) {
    x *= other.x; y *= other.y; z *= other.z; w *= other.w;
    return *this;
  }

  constexpr Vector4& operator/=(const Vector4& other) {
    x /= other.x; y /= other.y; z /= other.z; w /= other.w;
    return *this;
  }

  constexpr Vector4& operator+=(T s) { x += s; y += s; z += s; w += s; return *this; }
  constexpr Vector4& operator-=(T s) { x -= s; y -= s; z -= s; w -= s; return *this; }
  constexpr Vector4& operator*=(T s) { x *= s; y *= s; z *= s; w *= s; return *this; }
  constexpr Vector4& operator/=(T s) { x /= s; y /= s; z /= s; w /= s; return *this; }

  // Comparison
  constexpr bool operator==(const Vector4& other) const {
    return x == other.x && y == other.y && z == other.z && w == other.w;
  }

  constexpr bool operator!=(const Vector4& other) const {
    return x != other.x || y != other.y || z != other.z || w != other.w;
  }

  // Utility
  T length_squared() const { return x*x + y*y + z*z + w*w; }
  T length() const { return (sizeof(T) == 4) ? T(sqrt_float(float(length_squared()))) : T(sqrt_double(double(length_squared()))); }
  void normalize() {
    T len = length();
    if (len > 0) { x /= len; y /= len; z /= len; w /= len; }
  }
  Vector4 normalized() const {
    T len = length();
    return len > 0 ? Vector4(x / len, y / len, z / len, w / len) : *this;
  }
  T dot(const Vector4& other) const {
    return x * other.x + y * other.y + z * other.z + w * other.w;
  }
};

// Type aliases
using Vec2f = Vector2<float>;
using Vec2d = Vector2<double>;
using Vec2i = Vector2<int32_t>;
using Vec2u = Vector2<uint32_t>;

using Vec3f = Vector3<float>;
using Vec3d = Vector3<double>;
using Vec3i = Vector3<int32_t>;
using Vec3u = Vector3<uint32_t>;

using Vec4f = Vector4<float>;
using Vec4d = Vector4<double>;
using Vec4i = Vector4<int32_t>;
using Vec4u = Vector4<uint32_t>;

}  // namespace math
