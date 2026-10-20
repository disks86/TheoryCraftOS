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

// Host-side tests for the spreadsheet model: reading and showing numbers, formulas (operators, references, ranges, functions, errors, cycles, long chains
// of cells), CSV in and out, and moving the references of a copied formula.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "Spreadsheet/Sheet.hpp"

using namespace spreadsheet;

static int g_failed = 0;
#define CHECK(...) do { if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)

struct TestAllocator final : allocators::IAllocator {
  int live = 0;
  void* allocate(uint64_t size) override { ++live; return malloc(size); }
  void* allocate_aligned(uint64_t size, uint64_t) override { return allocate(size); }
  void deallocate(void* p) override { if (p) { --live; free(p); } }
};

static TestAllocator g_pool;   // for the strings the helpers build

// How a cell shows, compared with `expected`.
static bool shows(Sheet& s, uint32_t c, uint32_t r, const char* expected) {
  String out(g_pool);
  s.display(c, r, &out);
  const bool same = out.view().size() == strlen(expected) && memcmp(out.c_str(), expected, strlen(expected)) == 0;
  if (!same) printf("   cell %u,%u shows \"%s\", expected \"%s\"\n", c, r, out.c_str(), expected);
  return same;
}

// Types `typed` into A1 and checks how it shows.
static bool formula_shows(const char* typed, const char* expected) {
  TestAllocator a;
  bool ok;
  {
    Sheet s(a);
    s.set_text(0, 0, typed);
    ok = shows(s, 0, 0, expected);
    if (!ok) printf("   (typed %s)\n", typed);
  }
  return ok && a.live == 0;
}

static void test_numbers() {
  CHECK(formula_shows("12", "12"));
  CHECK(formula_shows("-2.5", "-2.5"));
  CHECK(formula_shows(" 7 ", "7"));
  CHECK(formula_shows("1e3", "1000"));
  CHECK(formula_shows("1.50", "1.5"));
  CHECK(formula_shows("abc", "abc"));
  CHECK(formula_shows("12abc", "12abc"));
  CHECK(formula_shows("", ""));
  CHECK(formula_shows("=0.1+0.2", "0.3"));
  CHECK(formula_shows("=1/3", "0.3333333333"));
  CHECK(formula_shows("=2/3", "0.6666666667"));
  CHECK(formula_shows("=1234567.891", "1234567.891"));
  CHECK(formula_shows("=1e20", "1E+20"));
  CHECK(formula_shows("=0.00001", "0.00001"));
  CHECK(formula_shows("=1e-7", "1E-7"));
  CHECK(formula_shows("=123456789012", "123456789012"));
  CHECK(formula_shows("=-0.5", "-0.5"));
  CHECK(formula_shows("=100", "100"));
  CHECK(formula_shows("=0", "0"));
  CHECK(formula_shows("=9.99999999999", "10"));
  double d = 0;
  CHECK(Sheet::parse_number("3.25", &d) && d == 3.25);
  CHECK(!Sheet::parse_number("3.2.5", &d) && !Sheet::parse_number("", &d) && !Sheet::parse_number("-", &d) && !Sheet::parse_number("1e", &d));
}

static void test_operators() {
  CHECK(formula_shows("=1+2*3", "7"));
  CHECK(formula_shows("=(1+2)*3", "9"));
  CHECK(formula_shows("=2^10", "1024"));
  CHECK(formula_shows("=2^-1", "0.5"));
  CHECK(formula_shows("=-2^2", "4"));
  CHECK(formula_shows("=2*3^2", "18"));
  CHECK(formula_shows("=10-4-3", "3"));
  CHECK(formula_shows("=100/10/5", "2"));
  CHECK(formula_shows("= 1 + 2", "3"));
  CHECK(formula_shows("=1/0", "#DIV/0!"));
  CHECK(formula_shows("=\"a\"&\"b\"", "ab"));
  CHECK(formula_shows("=\"x\"&1+2", "x3"));
  CHECK(formula_shows("=\"a\"\"b\"", "a\"b"));
  CHECK(formula_shows("=\"abc\"+1", "#VALUE!"));
  CHECK(formula_shows("=\"4\"+1", "5"));
  CHECK(formula_shows("=1=1", "1"));
  CHECK(formula_shows("=1<>1", "0"));
  CHECK(formula_shows("=2>1", "1"));
  CHECK(formula_shows("=2<=1", "0"));
  CHECK(formula_shows("=\"a\"=\"A\"", "1"));
  CHECK(formula_shows("=\"a\"<\"b\"", "1"));
  CHECK(formula_shows("=1<\"a\"", "1"));
  CHECK(formula_shows("=0.1+0.2=0.3", "1"));
  CHECK(formula_shows("=1+", "#ERR!"));
  CHECK(formula_shows("=(1", "#ERR!"));
  CHECK(formula_shows("=1 2", "#ERR!"));
  CHECK(formula_shows("=\"abc", "#ERR!"));
  CHECK(formula_shows("=FOO(1)", "#NAME?"));
  CHECK(formula_shows("=xyz", "#NAME?"));
  CHECK(formula_shows("=TRUE+TRUE", "2"));
  CHECK(formula_shows("=", "="));   // (a lone equals sign is text)
}

static void test_functions() {
  CHECK(formula_shows("=ABS(-3)", "3"));
  CHECK(formula_shows("=INT(-1.5)", "-2"));
  CHECK(formula_shows("=INT(2.9)", "2"));
  CHECK(formula_shows("=SQRT(16)", "4"));
  CHECK(formula_shows("=SQRT(-1)", "#NUM!"));
  CHECK(formula_shows("=ROUND(2.567,2)", "2.57"));
  CHECK(formula_shows("=ROUND(-2.5,0)", "-3"));
  CHECK(formula_shows("=ROUND(1234,-2)", "1200"));
  CHECK(formula_shows("=MOD(10,3)", "1"));
  CHECK(formula_shows("=MOD(-1,3)", "2"));
  CHECK(formula_shows("=MOD(5,0)", "#DIV/0!"));
  CHECK(formula_shows("=POWER(2,10)", "1024"));
  CHECK(formula_shows("=POWER(2,0.5)", "1.414213562"));
  CHECK(formula_shows("=POWER(-8,3)", "-512"));
  CHECK(formula_shows("=AND(1,1,1)", "1"));
  CHECK(formula_shows("=AND(1,0)", "0"));
  CHECK(formula_shows("=OR(0,0,1)", "1"));
  CHECK(formula_shows("=OR(0,0)", "0"));
  CHECK(formula_shows("=NOT(0)", "1"));
  CHECK(formula_shows("=LEN(\"hello\")", "5"));
  CHECK(formula_shows("=LEN(12345)", "5"));
  CHECK(formula_shows("=IF(1>0,\"yes\",\"no\")", "yes"));
  CHECK(formula_shows("=IF(1<0,\"yes\",\"no\")", "no"));
  CHECK(formula_shows("=IF(1<0,5)", "0"));
  CHECK(formula_shows("=IF(1,5,1/0)", "5"));      // the branch not taken is not worked out
  CHECK(formula_shows("=IF(0,1/0,6)", "6"));
  CHECK(formula_shows("=IF(1/0,1,2)", "#DIV/0!"));
  CHECK(formula_shows("=SUM(1,2,3)", "6"));
  CHECK(formula_shows("=SUM()", "0"));
  CHECK(formula_shows("=MAX(3,9,4)", "9"));
  CHECK(formula_shows("=MIN(3,9,4)", "3"));
  CHECK(formula_shows("=AVERAGE(2,4)", "3"));
  CHECK(formula_shows("=AVERAGE()", "#DIV/0!"));
  CHECK(formula_shows("=COUNT(1,2,\"x\")", "#VALUE!"));
  CHECK(formula_shows("=sum(1,2)", "3"));        // (names are not case sensitive)
}

static void test_cells() {
  TestAllocator a;
  {
    Sheet s(a);
    s.set_text(0, 0, "5");
    s.set_text(1, 0, "=A1*2");
    CHECK(shows(s, 1, 0, "10"));
    s.set_text(0, 0, "7");                       // a change reaches the cells that read it
    CHECK(shows(s, 1, 0, "14"));
    s.set_text(2, 0, "=B1+A1");                  // and those that read those
    CHECK(shows(s, 2, 0, "21"));
    s.set_text(0, 0, "x");
    CHECK(shows(s, 1, 0, "#VALUE!") && shows(s, 2, 0, "#VALUE!"));
    s.set_text(0, 0, "");
    CHECK(shows(s, 1, 0, "0"));
    double d = 0;
    CHECK(s.number(1, 0, &d) && d == 0);
    CHECK(!s.number(5, 5, &d));

    // Ranges.
    for (uint32_t r = 0; r < 5; ++r) {
      char t[8];
      snprintf(t, sizeof t, "%u", r + 1);
      s.set_text(4, r, t);                       // E1:E5 = 1..5
    }
    s.set_text(4, 5, "text");
    s.set_text(5, 0, "=SUM(E1:E6)");
    s.set_text(5, 1, "=AVERAGE(E1:E5)");
    s.set_text(5, 2, "=MAX(E1:E6)");
    s.set_text(5, 3, "=MIN(E2:E6)");
    s.set_text(5, 4, "=COUNT(E1:E6)");
    s.set_text(5, 5, "=SUM(E5:E1)");           // (corners in any order)
    CHECK(shows(s, 5, 0, "15") && shows(s, 5, 1, "3") && shows(s, 5, 2, "5") && shows(s, 5, 3, "2") && shows(s, 5, 4, "5") && shows(s, 5, 5, "15"));
    s.set_text(4, 2, "=1/0");                    // an error in the range is the result
    CHECK(shows(s, 5, 0, "#DIV/0!"));
    s.set_text(6, 0, "=E1:E2");                  // a range alone is not a value
    CHECK(shows(s, 6, 0, "#VALUE!"));
    s.set_text(6, 1, "=SUM(F1:F1)");             // sums of sums
    CHECK(shows(s, 6, 1, "#DIV/0!"));

    // Cells that lead back to themselves.
    s.clear();
    s.set_text(0, 0, "=B1");
    s.set_text(1, 0, "=A1+1");
    CHECK(shows(s, 0, 0, "#CYCLE!") && shows(s, 1, 0, "#CYCLE!"));
    s.set_text(2, 0, "=C1");
    CHECK(shows(s, 2, 0, "#CYCLE!"));
    s.set_text(3, 0, "=A1");                     // reads a cycle
    CHECK(shows(s, 3, 0, "#CYCLE!"));
    s.set_text(1, 0, "5");                       // the cycle is broken
    CHECK(shows(s, 0, 0, "5") && shows(s, 1, 0, "5") && shows(s, 3, 0, "5"));

    // A chain as long as the sheet: no recursion, so no stack trouble.
    s.clear();
    for (uint32_t r = 0; r + 1 < kRows; ++r) {
      char t[16];
      snprintf(t, sizeof t, "=A%u+1", r + 2);
      s.set_text(0, r, t);
    }
    s.set_text(0, kRows - 1, "0");
    CHECK(shows(s, 0, 0, "999"));
    CHECK(shows(s, 0, 500, "499"));

    // Outside the sheet.
    CHECK(!s.set_text(kColumns, 0, "x") && !s.set_text(0, kRows, "x"));
    s.clear();
    s.set_text(0, 0, "=BA1");
    CHECK(shows(s, 0, 0, "#NAME?"));
    s.set_text(0, 0, "=$B$2+B$2+$B2");
    CHECK(shows(s, 0, 0, "0"));
    CHECK(s.modified());
  }
  CHECK(a.live == 0);
}

static void test_csv() {
  TestAllocator a;
  {
    Sheet s(a);
    s.set_text(0, 0, "name");
    s.set_text(1, 0, "a,b");
    s.set_text(2, 0, "say \"hi\"");
    s.set_text(0, 1, "1");
    s.set_text(1, 1, "=A2*2");
    s.set_text(0, 2, " padded ");
    s.set_text(2, 2, "line\nbreak");
    String csv(a);
    CHECK(s.save_csv(&csv));
    CHECK(strcmp(csv.c_str(), "name,\"a,b\",\"say \"\"hi\"\"\"\n1,=A2*2,\n\" padded \",,\"line\nbreak\"\n") == 0);
    Sheet t(a);
    uint32_t dropped = 99;
    CHECK(t.load_csv(csv.view(), &dropped) && dropped == 0 && !t.modified());
    CHECK(t.text(1, 0).size() == 3 && memcmp(t.text(1, 0).data(), "a,b", 3) == 0);
    CHECK(strcmp(String(a).c_str(), "") == 0);
    CHECK(shows(t, 1, 1, "2") && shows(t, 2, 0, "say \"hi\"") && shows(t, 0, 2, " padded ") && shows(t, 2, 2, "line\nbreak"));
    String again(a);
    CHECK(t.save_csv(&again) && strcmp(again.c_str(), csv.c_str()) == 0);

    // Other writers: CRLF, a missing final line break, empty lines, rows of different lengths, text that goes beyond the sheet.
    Sheet u(a);
    CHECK(u.load_csv("a,b,c\r\n1,2\r\n\r\n,,z", &dropped) && dropped == 0);
    CHECK(shows(u, 0, 0, "a") && shows(u, 2, 0, "c") && shows(u, 1, 1, "2") && shows(u, 2, 3, "z") && shows(u, 0, 3, "") && shows(u, 0, 2, ""));
    String wide(a);
    for (uint32_t i = 0; i < kColumns + 3; ++i) wide.append("x,");
    wide.append("end");
    CHECK(u.load_csv(wide.view(), &dropped) && dropped == 4);   // 3 extra "x" and "end"
    CHECK(u.load_csv("\"unterminated,field", &dropped));
    CHECK(shows(u, 0, 0, "unterminated,field"));
    CHECK(u.load_csv("", &dropped) && u.text(0, 0).empty());
  }
  CHECK(a.live == 0);
}

static void test_helpers() {
  TestAllocator a;
  String name(a);
  Sheet::column_name(0, &name);
  CHECK(strcmp(name.c_str(), "A") == 0);
  Sheet::column_name(25, &name);
  CHECK(strcmp(name.c_str(), "Z") == 0);
  Sheet::column_name(26, &name);
  CHECK(strcmp(name.c_str(), "AA") == 0);
  Sheet::column_name(51, &name);
  CHECK(strcmp(name.c_str(), "AZ") == 0);
  uint32_t c, r;
  CHECK(Sheet::parse_reference("A1", &c, &r) && c == 0 && r == 0);
  CHECK(Sheet::parse_reference("$AZ$1000", &c, &r) && c == 51 && r == 999);
  CHECK(Sheet::parse_reference("b7", &c, &r) && c == 1 && r == 6);
  CHECK(!Sheet::parse_reference("BA1", &c, &r) && !Sheet::parse_reference("A1001", &c, &r) && !Sheet::parse_reference("A0", &c, &r) && !Sheet::parse_reference("A", &c, &r) &&
        !Sheet::parse_reference("1A", &c, &r) && !Sheet::parse_reference("A1B", &c, &r) && !Sheet::parse_reference("", &c, &r));

  String out(a);
  CHECK(Sheet::shift_references("=A1+$B$2+C$3+$D4", 1, 1, &out) && strcmp(out.c_str(), "=B2+$B$2+D$3+$D5") == 0);
  CHECK(Sheet::shift_references("=SUM(A1:B2)*2", 0, 2, &out) && strcmp(out.c_str(), "=SUM(A3:B4)*2") == 0);
  CHECK(Sheet::shift_references("=A1+1", -1, 0, &out) && strcmp(out.c_str(), "=#REF!+1") == 0);
  CHECK(Sheet::shift_references("=\"A1\"&A1", 1, 0, &out) && strcmp(out.c_str(), "=\"A1\"&B1") == 0);
  CHECK(Sheet::shift_references("=LOG10(A1)", 0, 1, &out) && strcmp(out.c_str(), "=LOG10(A2)") == 0);
  CHECK(Sheet::shift_references("=AZ1", 1, 0, &out) && strcmp(out.c_str(), "=#REF!") == 0);
  CHECK(Sheet::shift_references("=Y1", 1, 0, &out) && strcmp(out.c_str(), "=Z1") == 0);
  CHECK(Sheet::shift_references("=Z1", 1, 0, &out) && strcmp(out.c_str(), "=AA1") == 0);
  Sheet s(a);
  s.set_text(0, 0, "1");
  s.set_text(3, 3, "=A1");
  uint32_t columns, rows;
  s.used_extent(&columns, &rows);
  CHECK(columns == 4 && rows == 4);
  s.clear_range(0, 0, 0, 0);
  s.used_extent(&columns, &rows);
  CHECK(columns == 4 && rows == 4 && s.text(0, 0).empty());
  s.clear();
  s.used_extent(&columns, &rows);
  CHECK(columns == 0 && rows == 0);
}

int main() {
  test_numbers();
  test_operators();
  test_functions();
  test_cells();
  test_csv();
  test_helpers();
  if (g_failed) {
    printf("%d check(s) failed\n", g_failed);
    return 1;
  }
  printf("spreadsheet tests passed\n");
  return 0;
}
