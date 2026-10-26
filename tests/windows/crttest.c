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

/* An ordinary C program (the usual mingw start-up code and headers) for TheoryCraftOS's msvcrt.dll; see tests/windows/build.sh, which builds it against the classic
 * msvcrt and against the Universal CRT. Prints "crttest: N checks, M failed"; the exit code is the number of failures. */
#include <errno.h>
#include <math.h>
#include <setjmp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <wchar.h>
#include <ctype.h>


static int g_checks, g_failed;
static void check(int ok, const char *what) {
  ++g_checks;
  if (ok) return;
  ++g_failed;
  printf("FAIL: %s\n", what);
}

static int compare_ints(const void *a, const void *b) { return *(const int *)a - *(const int *)b; }
static jmp_buf g_jump;
static int g_atexit_ran;
static void at_exit_handler(void) { g_atexit_ran = 1; puts("atexit handler ran"); }
static volatile int g_signalled;
static jmp_buf g_fault_jump;
static void on_fault(int s) { g_signalled = s; longjmp(g_fault_jump, 5); }
static void on_signal(int s) { g_signalled = s; }

static void test_format(void) {
  char b[256];
  snprintf(b, sizeof(b), "%d|%5d|%-5d|%05d|%+d|%x|%X|%#x|%o|%u", 42, 42, 42, 42, 42, 255, 255, 255, 8, 4000000000u);
  check(strcmp(b, "42|   42|42   |00042|+42|ff|FF|0xff|10|4000000000") == 0, "integer formats");
  snprintf(b, sizeof(b), "%s|%10s|%-10s|%.3s|%c", "abc", "abc", "abc", "abcdef", 'z');
  check(strcmp(b, "abc|       abc|abc       |abc|z") == 0, "string formats");
  snprintf(b, sizeof(b), "%lld|%llu|%llx|%zu|%ld", -1234567890123LL, 12345678901234ULL, 0xABCDEF012345ULL, (size_t)77, -5L);
  check(strcmp(b, "-1234567890123|12345678901234|abcdef012345|77|-5") == 0, "64-bit formats");
  snprintf(b, sizeof(b), "%f|%.2f|%8.3f|%e|%g|%g", 3.14159, 2.5, -1.5, 12345.678, 0.0001, 1e20);
  check(strcmp(b, "3.141590|2.50|  -1.500|1.234568e+04|0.0001|1e+20") == 0, "floating formats");
  snprintf(b, sizeof(b), "%.0f|%.1f|%g|%g", 0.5, 0.05, 100000.0, 1234567.0);
  check(strcmp(b, "0|0.1|100000|1.23457e+06") == 0, "floating rounding");
  snprintf(b, sizeof(b), "%*d|%-*d|%.*f", 6, 7, 6, 7, 2, 3.14159);
  check(strcmp(b, "     7|7     |3.14") == 0, "star widths");
  snprintf(b, sizeof(b), "%p|%%", (void *)0x1234);
  check(strstr(b, "1234") && strchr(b, '%'), "pointer and percent");
  check(snprintf(NULL, 0, "%d-%s", 12345, "xyz") == 9, "snprintf counts without a buffer");
  check(snprintf(b, 4, "%s", "abcdefgh") == 8 && strcmp(b, "abc") == 0, "snprintf truncates");
  wchar_t w[64];
  swprintf(w, 64, L"%ls=%d", L"wide", 5);
  check(wcscmp(w, L"wide=5") == 0, "wide printf");
  snprintf(b, sizeof(b), "%f %f %f", (double)INFINITY, -(double)INFINITY, 0.0);
  check(strcmp(b, "inf -inf 0.000000") == 0, "inf formats");
}

static void test_scan(void) {
  int i = 0, j = 0;
  unsigned x = 0;
  char s[32] = "", t[32] = "";
  double d = 0;
  float f = 0;
  check(sscanf("12 34", "%d %d", &i, &j) == 2 && i == 12 && j == 34, "scan integers");
  check(sscanf("ff 0x10 010", "%x %i %i", &x, &i, &j) == 3 && x == 255 && i == 16 && j == 8, "scan bases");
  check(sscanf("hello world", "%31s %31s", s, t) == 2 && !strcmp(s, "hello") && !strcmp(t, "world"), "scan words");
  check(sscanf("3.25 -1e2", "%lf %f", &d, &f) == 2 && d == 3.25 && f == -100.0f, "scan floats");
  check(sscanf("key=value", "%31[^=]=%31s", s, t) == 2 && !strcmp(s, "key") && !strcmp(t, "value"), "scan sets");
  check(sscanf("abc", "%d", &i) == 0, "scan mismatch");
  check(sscanf("", "%d", &i) == -1, "scan end of input");
  int n = 0;
  check(sscanf("77 rest", "%d%n", &i, &n) == 1 && i == 77 && n == 2, "scan count");
}

static void test_strings_and_memory(void) {
  char *p = malloc(100);
  check(p != NULL, "malloc");
  memset(p, 'a', 99);
  p[99] = 0;
  check(strlen(p) == 99, "strlen");
  p = realloc(p, 1000);
  check(p && strlen(p) == 99, "realloc keeps");
  free(p);
  int *z = calloc(10, sizeof(int));
  check(z && z[0] == 0 && z[9] == 0, "calloc zeroes");
  free(z);
  char b[64];
  strcpy(b, "Hello");
  strcat(b, ", ");
  strncat(b, "World!!!", 5);
  check(strcmp(b, "Hello, World") == 0, "strcpy strcat strncat");
  check(strchr(b, 'W') == b + 7 && strrchr(b, 'o') == b + 8 && strstr(b, "lo,") == b + 3, "strchr strrchr strstr");
  check(_stricmp("ABC", "abc") == 0 && strncmp("abcd", "abxx", 2) == 0, "string compares");
  char *d = _strdup("dup");
  check(d && !strcmp(d, "dup"), "_strdup");
  free(d);
  char tok[] = "a,b;c";
  check(!strcmp(strtok(tok, ",;"), "a") && !strcmp(strtok(NULL, ",;"), "b") && !strcmp(strtok(NULL, ",;"), "c") && !strtok(NULL, ",;"), "strtok");
  check(toupper('a') == 'A' && tolower('Q') == 'q' && isdigit('5') && !isdigit('a') && isalpha('x') && isspace(' ') && isupper('A') && ispunct('!'), "ctype");
  char *end;
  check(strtol("  -123abc", &end, 10) == -123 && *end == 'a', "strtol");
  check(strtoul("ff", NULL, 16) == 255 && strtoll("-9223372036854775807", NULL, 10) == -9223372036854775807LL, "strtoul strtoll");
  check(atoi("42") == 42 && atof("2.5") == 2.5 && atol("-7") == -7, "atoi atof atol");
  errno = 0;
  strtol("99999999999999999999", NULL, 10);
  check(errno == ERANGE, "strtol overflow sets errno");
  int a[] = {5, 3, 9, 1, 7, 2, 8};
  qsort(a, 7, sizeof(int), compare_ints);
  check(a[0] == 1 && a[3] == 5 && a[6] == 9, "qsort");
  int key = 7;
  int *found = bsearch(&key, a, 7, sizeof(int), compare_ints);
  check(found && *found == 7, "bsearch");
  check(abs(-5) == 5 && labs(-5L) == 5, "abs");
  srand(1);
  int r1 = rand();
  srand(1);
  check(rand() == r1 && r1 >= 0 && r1 < 32768, "rand repeats");
  wchar_t w[32];
  mbstowcs(w, "h\xc3\xa9llo", 32);
  check(wcslen(w) == 5 && w[1] == 0xE9, "mbstowcs decodes UTF-8");
  char back[32];
  wcstombs(back, w, 32);
  check(!strcmp(back, "h\xc3\xa9llo"), "wcstombs encodes UTF-8");
}

static void test_math(void) {
  check(fabs(sqrt(2.0) - 1.41421356237) < 1e-9, "sqrt");
  check(floor(2.7) == 2 && ceil(2.1) == 3 && fabs(-1.5) == 1.5 && fmod(7.5, 2) == 1.5, "floor ceil fabs fmod");
  check(fabs(sin(0.5) - 0.479425538604) < 1e-9 && fabs(cos(0.5) - 0.877582561890) < 1e-9, "sin cos");
  check(fabs(exp(1.0) - 2.718281828459) < 1e-9 && fabs(log(10.0) - 2.302585092994) < 1e-9, "exp log");
  check(fabs(pow(2.0, 10.0) - 1024.0) < 1e-9 && fabs(pow(2.0, 0.5) - 1.41421356237) < 1e-9, "pow");
  check(fabs(atan2(1.0, 1.0) - 0.785398163397) < 1e-9, "atan2");
  check(isnan(NAN) && !isnan(1.0) && isinf(INFINITY), "nan inf");
  check(round(2.5) == 3 && trunc(-2.7) == -2 && fabs(cbrt(27.0) - 3.0) < 1e-9 && fabs(hypot(3, 4) - 5) < 1e-9, "round trunc cbrt hypot");
}

static void test_files(void) {
  FILE *f = fopen("crttest.tmp", "w");
  check(f != NULL, "fopen for writing");
  if (f) {
    fprintf(f, "line one\nline %d\n", 2);
    fputs("third\n", f);
    check(fclose(f) == 0, "fclose");
  }
  f = fopen("crttest.tmp", "r");
  check(f != NULL, "fopen for reading");
  if (f) {
    char line[64];
    check(fgets(line, sizeof(line), f) && !strcmp(line, "line one\n"), "fgets 1");
    int n = 0;
    check(fscanf(f, "line %d\n", &n) == 1 && n == 2, "fscanf");
    check(fgets(line, sizeof(line), f) && !strcmp(line, "third\n"), "fgets 3");
    check(fgets(line, sizeof(line), f) == NULL && feof(f), "end of file");
    rewind(f);
    check(fgetc(f) == 'l' && ungetc('X', f) == 'X' && fgetc(f) == 'X', "fgetc ungetc");
    fseek(f, 0, SEEK_END);
    check(ftell(f) == 9 + 7 + 6 + 3, "ftell at the end counts the CR of each of the three lines");
    fclose(f);
  }
  f = fopen("crttest.bin", "wb");
  unsigned char data[300];
  for (int i = 0; i < 300; ++i) data[i] = (unsigned char)i;
  check(f && fwrite(data, 1, 300, f) == 300, "fwrite binary");
  if (f) fclose(f);
  f = fopen("crttest.bin", "rb");
  unsigned char back[300];
  check(f && fread(back, 1, 300, f) == 300 && !memcmp(data, back, 300), "fread binary");
  if (f) {
    fseek(f, 100, SEEK_SET);
    check(fgetc(f) == 100 && ftell(f) == 101, "fseek and ftell");
    fclose(f);
  }
  check(remove("crttest.tmp") == 0 && remove("crttest.bin") == 0, "remove");
  check(fopen("crttest.nonexistent", "r") == NULL && errno == ENOENT, "fopen of a missing file sets errno");
  f = fopen("crttest.app", "w");
  if (f) { fputs("a", f); fclose(f); }
  f = fopen("crttest.app", "a");
  if (f) { fputs("b", f); fclose(f); }
  f = fopen("crttest.app", "rb");
  char two[4] = {0};
  check(f && fread(two, 1, 3, f) == 2 && !strcmp(two, "ab"), "append mode");
  if (f) fclose(f);
  remove("crttest.app");
}

static void test_time_and_control(void) {
  time_t t = 86400 * 365 + 3600 * 5 + 61;
  struct tm *tm = gmtime(&t);
  check(tm && tm->tm_year == 71 && tm->tm_mon == 0 && tm->tm_mday == 1 && tm->tm_hour == 5 && tm->tm_min == 1 && tm->tm_sec == 1, "gmtime");
  char b[64];
  strftime(b, sizeof(b), "%Y-%m-%d %H:%M:%S %a %b", tm);
  check(!strcmp(b, "1971-01-01 05:01:01 Fri Jan"), "strftime");
  struct tm again = *tm;
  check(mktime(&again) == t, "mktime");
  time_t now = time(NULL);
  check(now >= 0 && time(NULL) >= now, "time does not run backwards");
  check(clock() >= 0, "clock");
  int j = setjmp(g_jump);
  if (j == 0) { longjmp(g_jump, 7); check(0, "longjmp returns"); }
  check(j == 7, "setjmp and longjmp");
  signal(SIGINT, on_signal);
  raise(SIGINT);
  check(g_signalled == SIGINT, "signal and raise");
  /* the C runtime turns processor faults into signals (through the unhandled-exception filter) */
  signal(SIGSEGV, on_fault);
  g_signalled = 0;
  if (setjmp(g_fault_jump) == 0) { *(volatile int *)0 = 1; check(0, "a write to address 0 returns"); }
  check(g_signalled == SIGSEGV, "a fault becomes SIGSEGV and the handler can longjmp out");
  signal(SIGFPE, on_fault);
  g_signalled = 0;
  if (setjmp(g_fault_jump) == 0) { volatile int zero = 0; volatile int x = 5 / zero; (void)x; check(0, "a division by zero returns"); }
  check(g_signalled == SIGFPE, "a division by zero becomes SIGFPE");
  atexit(at_exit_handler);
  check(getenv("TCOS_NO_SUCH_VARIABLE") == NULL, "getenv of a missing variable");
  _putenv("TCOS_CRT=hello");
  check(getenv("TCOS_CRT") && !strcmp(getenv("TCOS_CRT"), "hello"), "putenv and getenv");
}

int main(int argc, char **argv) {
  printf("crttest starting (%d arguments, %s)\n", argc, argc > 0 ? argv[0] : "?");
  fprintf(stderr, "this line goes to stderr\n");
  test_format();
  test_scan();
  test_strings_and_memory();
  test_math();
  test_files();
  test_time_and_control();
  printf("crttest: %d checks, %d failed\n", g_checks, g_failed);
  return g_failed;
}
