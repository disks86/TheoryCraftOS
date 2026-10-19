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

// GPU-friendly Matrix class with column-major memory layout
// Uses simple implementations for freestanding compatibility
#pragma once
#include "Vector.hpp"
#include <cstring>
#include <algorithm>

namespace math {

// Simple sin/cos using Taylor series for 32-bit floats
inline float sin_float(float x) {
  const float PI = 3.14159265358979323846f;
  const float TWO_PI = 6.28318530717958647692f;
  while (x > PI) x -= TWO_PI;
  while (x < -PI) x += TWO_PI;
  if (x > PI / 2) x = PI - x;
  if (x < -PI / 2) x = -PI - x;
  float x2 = x * x;
  float result = x;
  float term = x;
  for (int n = 1; n < 10; ++n) {
    term *= -x2 / ((2 * n) * (2 * n + 1));
    result += term;
  }
  return result;
}

inline float cos_float(float x) {
  return sin_float(x + 1.57079632679489661923f);
}

// Simple sin/cos using Taylor series for 64-bit doubles
inline double sin_double(double x) {
  const double PI = 3.14159265358979323846;
  const double TWO_PI = 6.28318530717958647692;
  while (x > PI) x -= TWO_PI;
  while (x < -PI) x += TWO_PI;
  if (x > PI / 2) x = PI - x;
  if (x < -PI / 2) x = -PI - x;
  double x2 = x * x;
  double result = x;
  double term = x;
  for (int n = 1; n < 15; ++n) {
    term *= -x2 / ((2 * n) * (2 * n + 1));
    result += term;
  }
  return result;
}

inline double cos_double(double x) {
  return sin_double(x + 1.57079632679489661923);
}

// Base Matrix class with column-major storage (GPU-friendly)
template<typename T, size_t Rows, size_t Cols>
class Matrix {
 public:
  T data[Rows * Cols];

  constexpr Matrix() { std::memset(data, 0, sizeof(data)); }
  explicit constexpr Matrix(const T* values) { std::memcpy(data, values, sizeof(data)); }
  constexpr Matrix(const Matrix&) = default;
  constexpr Matrix(Matrix&&) = default;
  constexpr Matrix& operator=(const Matrix&) = default;
  constexpr Matrix& operator=(Matrix&&) = default;

  constexpr T& at(size_t row, size_t col) { return data[col * Rows + row]; }
  constexpr const T& at(size_t row, size_t col) const { return data[col * Rows + row]; }
  constexpr T& operator()(size_t row, size_t col) { return at(row, col); }
  constexpr const T& operator()(size_t row, size_t col) const { return at(row, col); }

  constexpr Matrix operator-() const {
    Matrix r;
    for (size_t i = 0; i < Rows * Cols; ++i) r.data[i] = -data[i];
    return r;
  }

  constexpr Matrix operator+(const Matrix& o) const {
    Matrix r;
    for (size_t i = 0; i < Rows * Cols; ++i) r.data[i] = data[i] + o.data[i];
    return r;
  }

  constexpr Matrix operator-(const Matrix& o) const {
    Matrix r;
    for (size_t i = 0; i < Rows * Cols; ++i) r.data[i] = data[i] - o.data[i];
    return r;
  }

  constexpr Matrix operator*(T s) const {
    Matrix r;
    for (size_t i = 0; i < Rows * Cols; ++i) r.data[i] = data[i] * s;
    return r;
  }

  constexpr Matrix operator/(T s) const {
    Matrix r;
    for (size_t i = 0; i < Rows * Cols; ++i) r.data[i] = data[i] / s;
    return r;
  }

  constexpr Matrix& operator+=(const Matrix& o) {
    for (size_t i = 0; i < Rows * Cols; ++i) data[i] += o.data[i];
    return *this;
  }

  constexpr Matrix& operator-=(const Matrix& o) {
    for (size_t i = 0; i < Rows * Cols; ++i) data[i] -= o.data[i];
    return *this;
  }

  constexpr Matrix& operator*=(T s) {
    for (size_t i = 0; i < Rows * Cols; ++i) data[i] *= s;
    return *this;
  }

  constexpr Matrix& operator/=(T s) {
    for (size_t i = 0; i < Rows * Cols; ++i) data[i] /= s;
    return *this;
  }

  template<size_t OC>
  constexpr Matrix<T, Rows, OC> operator*(const Matrix<T, Cols, OC>& o) const {
    Matrix<T, Rows, OC> r;
    for (size_t row = 0; row < Rows; ++row) {
      for (size_t col = 0; col < OC; ++col) {
        T sum = T(0);
        for (size_t k = 0; k < Cols; ++k) sum += at(row, k) * o.at(k, col);
        r.at(row, col) = sum;
      }
    }
    return r;
  }

  constexpr Vector3<T> operator*(const Vector3<T>& v) const {
    static_assert(Rows == 3 && Cols == 3);
    Vector3<T> r;
    r.x = at(0, 0)*v.x + at(0,1)*v.y + at(0,2)*v.z;
    r.y = at(1, 0)*v.x + at(1,1)*v.y + at(1,2)*v.z;
    r.z = at(2, 0)*v.x + at(2,1)*v.y + at(2,2)*v.z;
    return r;
  }

  constexpr Vector4<T> operator*(const Vector4<T>& v) const {
    static_assert(Rows == 4 && Cols == 4);
    Vector4<T> r;
    r.x = at(0, 0)*v.x + at(0,1)*v.y + at(0,2)*v.z + at(0,3)*v.w;
    r.y = at(1, 0)*v.x + at(1,1)*v.y + at(1,2)*v.z + at(1,3)*v.w;
    r.z = at(2, 0)*v.x + at(2,1)*v.y + at(2,2)*v.z + at(2,3)*v.w;
    r.w = at(3, 0)*v.x + at(3,1)*v.y + at(3,2)*v.z + at(3,3)*v.w;
    return r;
  }

  constexpr bool operator==(const Matrix& o) const {
    for (size_t i = 0; i < Rows * Cols; ++i) if (data[i] != o.data[i]) return false;
    return true;
  }

  constexpr bool operator!=(const Matrix& o) const {
    for (size_t i = 0; i < Rows * Cols; ++i) if (data[i] != o.data[i]) return true;
    return false;
  }

  Matrix<T, Cols, Rows> transpose() const {
    Matrix<T, Cols, Rows> r;
    for (size_t row = 0; row < Rows; ++row)
      for (size_t col = 0; col < Cols; ++col) r.at(col, row) = at(row, col);
    return r;
  }

  T trace() const {
    static_assert(Rows == Cols);
    T sum = T(0);
    for (size_t i = 0; i < Rows; ++i) sum += at(i, i);
    return sum;
  }

  static Matrix Identity() {
    Matrix r;
    for (size_t i = 0; i < std::min(Rows, Cols); ++i) r.at(i, i) = T(1);
    return r;
  }

  static Matrix Zero() {
    Matrix r;
    std::memset(r.data, 0, sizeof(r.data));
    return r;
  }

  static Matrix Diagonal(const T& d) {
    Matrix r = Zero();
    for (size_t i = 0; i < std::min(Rows, Cols); ++i) r.at(i, i) = d;
    return r;
  }
};

// 2x2 Matrix
template<typename T>
class Matrix2 : public Matrix<T, 2, 2> {
 public:
  using Base = Matrix<T, 2, 2>;
  using Base::Base;

  constexpr Matrix2() = default;
  constexpr Matrix2(T m00, T m10, T m01, T m11) {
    this->data[0] = m00; this->data[1] = m10;
    this->data[2] = m01; this->data[3] = m11;
  }

  T determinant() const { return this->at(0, 0)*this->at(1, 1) - this->at(0, 1)*this->at(1, 0); }
  Matrix2 inverse() const {
    T det = determinant();
    if (det == T(0)) return Zero();
    T inv = T(1) / det;
    return Matrix2(this->at(1, 1)*inv, -this->at(0, 1)*inv, -this->at(1, 0)*inv, this->at(0, 0)*inv);
  }
  static Matrix2 Identity() { return Matrix2(1, 0, 0, 1); }
  static Matrix2 Zero() { return Matrix2(0, 0, 0, 0); }
  static Matrix2 Scale(T sx, T sy) { return Matrix2(sx, 0, 0, sy); }
  static Matrix2 Rotation(T angle) {
    T c = (sizeof(T) == 4) ? T(cos_float(float(angle))) : T(cos_double(double(angle)));
    T s = (sizeof(T) == 4) ? T(sin_float(float(angle))) : T(sin_double(double(angle)));
    return Matrix2(c, s, -s, c);
  }
};

// 3x3 Matrix
template<typename T>
class Matrix3 : public Matrix<T, 3, 3> {
 public:
  using Base = Matrix<T, 3, 3>;
  using Base::Base;

  constexpr Matrix3() = default;
  constexpr Matrix3(T m00, T m10, T m20, T m01, T m11, T m21, T m02, T m12, T m22) {
    this->data[0] = m00; this->data[1] = m10; this->data[2] = m20;
    this->data[3] = m01; this->data[4] = m11; this->data[5] = m21;
    this->data[6] = m02; this->data[7] = m12; this->data[8] = m22;
  }

  T determinant() const {
    T a = this->at(0, 0), b = this->at(1, 0), c = this->at(2, 0);
    T d = this->at(0, 1), e = this->at(1, 1), f = this->at(2, 1);
    T g = this->at(0, 2), h = this->at(1, 2), i = this->at(2, 2);
    return a*(e*i - f*h) - b*(d*i - f*g) + c*(d*h - e*g);
  }
  Matrix3 inverse() const {
    T det = determinant();
    if (det == T(0)) return Zero();
    T inv = T(1) / det;
    T a = this->at(0, 0), b = this->at(1, 0), c = this->at(2, 0);
    T d = this->at(0, 1), e = this->at(1, 1), f = this->at(2, 1);
    T g = this->at(0, 2), h = this->at(1, 2), i = this->at(2, 2);
    return Matrix3(
      (e*i - f*h)*inv, (c*h - b*i)*inv, (b*f - c*e)*inv,
      (f*g - d*i)*inv, (a*i - c*g)*inv, (c*d - a*f)*inv,
      (d*h - e*g)*inv, (b*g - a*h)*inv, (a*e - b*d)*inv
    );
  }
  static Matrix3 Identity() { return Matrix3(1,0,0, 0,1,0, 0,0,1); }
  static Matrix3 Zero() { return Matrix3(0,0,0, 0,0,0, 0,0,0); }
  static Matrix3 Scale(T sx, T sy, T sz) { return Matrix3(sx,0,0, 0,sy,0, 0,0,sz); }
  static Matrix3 Scale(const Vector3<T>& s) { return Scale(s.x, s.y, s.z); }
  static Matrix3 RotationZ(T angle) {
    T c = (sizeof(T) == 4) ? T(cos_float(float(angle))) : T(cos_double(double(angle)));
    T s = (sizeof(T) == 4) ? T(sin_float(float(angle))) : T(sin_double(double(angle)));
    return Matrix3(c, s, 0, -s, c, 0, 0, 0, 1);
  }
};

// 4x4 Matrix
template<typename T>
class Matrix4 : public Matrix<T, 4, 4> {
 public:
  using Base = Matrix<T, 4, 4>;
  using Base::Base;

  constexpr Matrix4() = default;
  constexpr Matrix4(T m00, T m10, T m20, T m30, T m01, T m11, T m21, T m31, T m02, T m12, T m22, T m32, T m03, T m13, T m23, T m33) {
    this->data[0] = m00; this->data[1] = m10; this->data[2] = m20; this->data[3] = m30;
    this->data[4] = m01; this->data[5] = m11; this->data[6] = m21; this->data[7] = m31;
    this->data[8] = m02; this->data[9] = m12; this->data[10] = m22; this->data[11] = m32;
    this->data[12] = m03; this->data[13] = m13; this->data[14] = m23; this->data[15] = m33;
  }

  T determinant() const {
    T a = this->at(0, 0), b = this->at(1, 0), c = this->at(2, 0), d = this->at(3, 0);
    T e = this->at(0, 1), f = this->at(1, 1), g = this->at(2, 1), h = this->at(3, 1);
    T i = this->at(0, 2), j = this->at(1, 2), k = this->at(2, 2), l = this->at(3, 2);
    T m = this->at(0, 3), n = this->at(1, 3), o = this->at(2, 3), p = this->at(3, 3);
    auto det3 = [](T a, T b, T c, T d, T e, T f, T g, T h, T i) { return a*(e*i-f*h) - b*(d*i-f*g) + c*(d*h-e*g); };
    return a*det3(f,g,h,j,k,l,n,o,p) - b*det3(e,g,h,i,k,l,m,o,p) + c*det3(e,f,h,i,j,l,m,n,p) - d*det3(e,f,g,i,j,k,m,n,o);
  }

  Matrix4 inverse() const {
    T det = determinant();
    if (det == T(0)) return Zero();
    T inv = T(1) / det;

    auto minor = [this](size_t row, size_t col) -> Matrix3<T> {
      Matrix3<T> r;
      size_t mi = 0, mj = 0;
      for (size_t r2 = 0; r2 < 4; ++r2)
        for (size_t c2 = 0; c2 < 4; ++c2)
          if (r2 != row && c2 != col) {
            r.data[mj*3 + mi] = this->at(r2, c2);
            mi++; if (mi >= 3) { mi = 0; mj++; }
          }
      return r;
    };

    Matrix4 cof;
    for (size_t row = 0; row < 4; ++row)
      for (size_t col = 0; col < 4; ++col) {
        T sign = ((row + col) % 2 == 0) ? 1 : -1;
        cof.at(row, col) = minor(row, col).determinant() * sign * inv;
      }
    return cof.transpose();
  }

  static Matrix4 Identity() { return Matrix4(1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1); }
  static Matrix4 Zero() { return Matrix4(0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0); }
  static Matrix4 Scale(T sx, T sy, T sz, T sw = 1) { return Matrix4(sx,0,0,0, 0,sy,0,0, 0,0,sz,0, 0,0,0,sw); }
  static Matrix4 Scale(const Vector4<T>& s) { return Scale(s.x, s.y, s.z, s.w); }
  static Matrix4 Translation(T tx, T ty, T tz) { return Matrix4(1,0,0,tx, 0,1,0,ty, 0,0,1,tz, 0,0,0,1); }
  static Matrix4 Translation(const Vector3<T>& t) { return Translation(t.x, t.y, t.z); }
  static Matrix4 RotationX(T angle) {
    T c = (sizeof(T) == 4) ? T(cos_float(float(angle))) : T(cos_double(double(angle)));
    T s = (sizeof(T) == 4) ? T(sin_float(float(angle))) : T(sin_double(double(angle)));
    return Matrix4(1,0,0,0, 0,c,s,0, 0,-s,c,0, 0,0,0,1);
  }
  static Matrix4 RotationY(T angle) {
    T c = (sizeof(T) == 4) ? T(cos_float(float(angle))) : T(cos_double(double(angle)));
    T s = (sizeof(T) == 4) ? T(sin_float(float(angle))) : T(sin_double(double(angle)));
    return Matrix4(c,0,-s,0, 0,1,0,0, s,0,c,0, 0,0,0,1);
  }
  static Matrix4 RotationZ(T angle) {
    T c = (sizeof(T) == 4) ? T(cos_float(float(angle))) : T(cos_double(double(angle)));
    T s = (sizeof(T) == 4) ? T(sin_float(float(angle))) : T(sin_double(double(angle)));
    return Matrix4(c,s,0,0, -s,c,0,0, 0,0,1,0, 0,0,0,1);
  }
  static Matrix4 RotationAxis(T angle, const Vector3<T>& axis) {
    T c = (sizeof(T) == 4) ? T(cos_float(float(angle))) : T(cos_double(double(angle)));
    T s = (sizeof(T) == 4) ? T(sin_float(float(angle))) : T(sin_double(double(angle)));
    T t = T(1) - c;
    Vector3<T> a = axis.normalized();
    return Matrix4(
      a.x*a.x*t+c, a.x*a.y*t-a.z*s, a.x*a.z*t+a.y*s, 0,
      a.y*a.x*t+a.z*s, a.y*a.y*t+c, a.y*a.z*t-a.x*s, 0,
      a.z*a.x*t-a.y*s, a.z*a.y*t+a.x*s, a.z*a.z*t+c, 0,
      0, 0, 0, 1
    );
  }
};

// Type aliases
using Matrix2f = Matrix2<float>; using Matrix2d = Matrix2<double>;
using Matrix3f = Matrix3<float>; using Matrix3d = Matrix3<double>;
using Matrix4f = Matrix4<float>; using Matrix4d = Matrix4<double>;
using Mat2f = Matrix2f; using Mat2d = Matrix2d;
using Mat3f = Matrix3f; using Mat3d = Matrix3d;
using Mat4f = Matrix4f; using Mat4d = Matrix4d;

}  // namespace math
