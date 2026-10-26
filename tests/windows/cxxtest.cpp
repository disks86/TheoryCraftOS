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

// C++ exceptions on TheoryCraftOS's ntdll.dll, kernel32.dll and msvcrt.dll (mingw's g++, statically linked libstdc++ and libgcc; see tests/windows/build.sh). Prints
// "cxxtest: N checks, M failed"; the exit code is the number of failures.
#include <stdio.h>
#include <stdexcept>
#include <string>
#include <vector>
#include <map>
#include <typeinfo>
#include <new>

static int g_checks, g_failed;
static void check(bool ok, const char* what) {
  ++g_checks;
  if (ok) return;
  ++g_failed;
  printf("FAIL: %s\n", what);
}

static int g_live;      // objects alive: constructors minus destructors
struct Tracked {
  Tracked() { ++g_live; }
  Tracked(const Tracked&) { ++g_live; }
  ~Tracked() { --g_live; }
};

struct MyError {
  int code;
  explicit MyError(int c) : code(c) {}
};
struct DerivedError : MyError {
  DerivedError(int c) : MyError(c) {}
};

static void thrower(int what) {
  Tracked t;
  switch (what) {
    case 0: throw 42;
    case 1: throw std::runtime_error("runtime");
    case 2: throw MyError(7);
    case 3: throw DerivedError(9);
    case 4: throw std::string("text");
    case 5: throw 3.5;
    default: break;
  }
}

static void test_basic() {
  int caught = 0;
  try { thrower(0); } catch (int v) { caught = v; }
  check(caught == 42, "throw int, catch int");

  std::string message;
  try { thrower(1); } catch (const std::exception& e) { message = e.what(); }
  check(message == "runtime", "std::runtime_error through std::exception");

  int code = 0;
  try { thrower(2); } catch (const MyError& e) { code = e.code; }
  check(code == 7, "a class type");

  code = 0;
  try { thrower(3); } catch (const MyError& e) { code = e.code; }
  check(code == 9, "a derived class is caught by its base");

  std::string text;
  try { thrower(4); } catch (const std::string& s) { text = s; }
  check(text == "text", "std::string");

  bool other = false;
  try { thrower(5); } catch (int) { check(false, "double is not int"); } catch (...) { other = true; }
  check(other, "catch (...)");
  check(g_live == 0, "the destructors ran while unwinding");
}

static void test_nested() {
  int stage = 0;
  try {
    try {
      thrower(2);
    } catch (const MyError& e) {
      stage = 1;
      throw;      // rethrow
    }
  } catch (const MyError& e) {
    stage = 2 + e.code;
  }
  check(stage == 9, "rethrow reaches the outer handler");

  int inner = 0;
  try {
    try { throw 1; } catch (...) { inner = 1; throw std::runtime_error("second"); }
  } catch (const std::runtime_error& e) {
    inner += 10;
  }
  check(inner == 11, "a new exception thrown from a handler");

  std::exception_ptr saved;
  try { thrower(1); } catch (...) { saved = std::current_exception(); }
  bool again = false;
  try { std::rethrow_exception(saved); } catch (const std::runtime_error&) { again = true; }
  check(again, "std::exception_ptr");
}

static int depth_thrower(int depth) {
  Tracked t;
  if (depth == 0) throw MyError(depth);
  return depth_thrower(depth - 1) + 1;
}
static void test_deep() {
  int caught = -1;
  try { depth_thrower(300); } catch (const MyError& e) { caught = e.code; }
  check(caught == 0 && g_live == 0, "through 300 frames, with 300 destructors");
}

struct Guard {
  int* counter;
  explicit Guard(int* c) : counter(c) {}
  ~Guard() { ++*counter; }
};
static void with_guards(int* counter) {
  Guard a(counter);
  {
    Guard b(counter);
    thrower(0);
  }
}
static void test_destructors() {
  int count = 0;
  try { with_guards(&count); } catch (int) {}
  check(count == 2, "both guards were destroyed");
}

static void test_library() {
  std::vector<int> v;
  bool out_of_range = false;
  try { (void)v.at(5); } catch (const std::out_of_range&) { out_of_range = true; }
  check(out_of_range, "std::vector::at throws std::out_of_range");

  std::map<std::string, int> m;
  m["a"] = 1;
  bool missing = false;
  try { (void)m.at("b"); } catch (const std::out_of_range&) { missing = true; }
  check(missing && m.at("a") == 1, "std::map::at");

  bool bad_alloc = false;
  try { volatile size_t n = ~size_t(0) / 2; void* p = ::operator new(n); __asm__ volatile("" ::"r"(p)); } catch (const std::bad_alloc&) { bad_alloc = true; }
  check(bad_alloc, "an allocation too big");

  bool bad_cast = false;
  struct A { virtual ~A() {} };
  struct B : A {};
  struct C : A {};
  B b;
  A& a = b;
  try { (void)dynamic_cast<C&>(a); } catch (const std::bad_cast&) { bad_cast = true; }
  check(bad_cast && dynamic_cast<B*>(&a) == &b && std::string(typeid(a).name()).size() > 0, "dynamic_cast and typeid");

  std::string s = "exception";
  bool invalid = false;
  try { s.substr(100); } catch (const std::out_of_range&) { invalid = true; }
  check(invalid, "std::string::substr");
}

static int g_noexcept_calls;
static void no_throw() noexcept { ++g_noexcept_calls; }
static void test_misc() {
  no_throw();
  static Tracked once;      // a function-local static: thread-safe initialisation
  check(g_noexcept_calls == 1 && g_live == 1, "noexcept function and a local static");
  printf("%s %d %.2f\n", std::string("formatted").c_str(), 5, 2.5);
}

int main() {
  setvbuf(stdout, nullptr, _IONBF, 0);
  printf("cxxtest starting\n");
  test_basic();
  test_nested();
  test_deep();
  test_destructors();
  test_library();
  test_misc();
  printf("cxxtest: %d checks, %d failed\n", g_checks, g_failed);
  return g_failed;
}
